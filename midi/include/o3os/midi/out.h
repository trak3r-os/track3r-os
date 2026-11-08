// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI out, the router (OS-005c; ADR 0013 D5 "MIDI out (clock, notes of MIDI tracks, CC A-F, PC, hang bookkeeping PAR-063): due sample times ->
// timed queue -> PIT-compare emitter (DIN); USB MIDI is 1 ms-quantised by the bus"). It is the one place where the sequencer's MIDI events
// (audio::SeqEvent kinds MidiNoteOn .. MidiSpp) become bytes with a due time, in the render context:
//
//   event -> bytes (note, CC A..F through the device's CC table, program change, clock, start, continue, stop, SPP; OS-MTC A3: the MIDI Time Code quarter frame and full frame)
//         -> due time in CPU cycles of the audio timeline (the sample of the event, to a fraction of a sample for the clock), plus the OUT OFFSET of
//            the port (signed: a positive offset delays, a negative one advances; cancels cable and host latency), plus the event's own delay when it
//            has one (OS-INSTUI S8b: an instrument's OFFSET, SeqEvent::value in microseconds, which stays separate from the port's)
//         -> the staging queue (a sorted TimedQueue: events of different ports and offsets arrive out of order and leave in due order)
//         -> at the end of the block, everything due before the horizon is handed to the port: the DinEmitter's two rings and the UsbMidiOut ring,
//            which are single-producer queues, so their consumers (the PIT-compare interrupt, the SOF interrupt) never see an out-of-order item.
//
// MIDI TIME CODE (OS-MTC A3, docs/specs/OS-MTC.md 3.4). MidiMtcQf and MidiMtcFull go to `mtc_ports`, the ports the clock would go to (A-1: CLOCK OUT, without the CLOCK MODE gate that
// empties `clock_ports` while the mode is Off), each port with its OUT OFFSET, and
// are timed like the clock (the quarter frame to a fraction of a sample). A quarter frame is a two-byte system common message `F1 0nnndddd` (a normal message for both ports: DIN 0.64 ms on the
// wire, USB code index 0x2). A full frame is the SysEx `F0 7F 7F 01 01 hh mm ss ff F7`: the DIN emitter takes it through DinEmitter::push_sysex, the USB ring as four event words
// (usb_sysex_events: code indexes 0x4, 0x4, 0x4, 0x5), pushed in order; each port gets it whole or not at all (a port that has no room for all of it keeps it staged, as any item).
// A staged full frame keeps its packed time in the three bytes of its Msg and is marked by `rt` == kStagedFullFrame (no growth of the 256-entry staging queue).
//
// The horizon is two blocks (about 2.9 ms): far enough ahead that the emitter's compare is programmed in time, near enough that a stop or a mute
// finds little to take back.
//
// HANG BOOKKEEPING (PAR-063). Every note-on that is queued is counted by a HangBook (midi/hang.h), every note-off pays one. A stop, a mute of a
// track or a panic asks for the notes that are still owed: stop_all() drops the notes that were queued but not yet handed over, lets what the ports
// already hold play out (a note-on handed over must not be able to follow the off that is owed for it), and then pumps a bounded number of
// note-offs per block (kPanicPerBlock), due behind all of it, until nothing is owed. Notes that were never sent on never get an
// off, and an off that is not owed (a second off for a note, an off after a panic) is not sent. The book follows what was queued: a note-off that a full staging queue lost (on any
// port) does not pay its note, which stays owed until another off, a stop, a mute or a panic releases it (a chord sends four notes for one: OS-PFX). While `suppress` is on (an offline export: PAR-063,
// "MIDI Out suppressed during offline export") nothing is queued and the book is untouched.
//
// Threading: send(), flush(), stop_all() and release_channel() are all called from the render context (seq/midi_link.cpp, MidiLink::step: one thread, in
// that order each block); nothing here is called from an ISR (the ports' own SPSC rings carry the hand-over). A panic's note-off that the full staging queue
// refused is kept (retry slot) and staged first on the next flush, so an overflow never drops a note-off (OS-SMALL3).
//
// Everything is bounded: send() does one push; flush() moves at most kMaxMovePerBlock items and pumps at most kPanicPerBlock notes.

#include <o3os/audio/events.h>
#include <o3os/core/qtime.h>
#include <o3os/midi/din.h>
#include <o3os/midi/hang.h>
#include <o3os/midi/in.h>
#include <o3os/midi/message.h>
#include <o3os/midi/timed_queue.h>
#include <o3os/midi/usb_out.h>

#include <cstdint>
#include <type_traits>

namespace o3os::midi {

inline constexpr std::uint8_t kPortDin = 1u << 0;
inline constexpr std::uint8_t kPortUsb = 1u << 1;
inline constexpr std::uint8_t kCcUnassigned = 0xFF;  // a CC slot with no number sends nothing

class MidiOut {
 public:
  static constexpr std::uint8_t kStagedFullFrame = 2;   // Staged::rt of an MTC full frame
  static constexpr std::uint32_t kStagingSize = 256;
  static constexpr std::uint32_t kMaxMovePerBlock = 64;
  static constexpr std::uint32_t kPanicPerBlock = 16;
  // How far ahead of the block's end items are handed to the ports: two blocks of the audio clock, in CPU cycles.
  static constexpr std::uint64_t kHorizonCycles = 2u * (static_cast<std::uint64_t>(core::kRefCyclesPerSample.num) * 64u / core::kRefCyclesPerSample.den);

  struct Config {
    std::uint8_t note_ports = kPortDin;     // where notes, CC and program changes go (off, usb, jack, both)
    std::uint8_t clock_ports = 0;           // where clock, start, continue, stop and SPP go (CLOCK OUT)
    std::uint8_t mtc_ports = 0;             // OS-MTC A3: where MIDI Time Code goes (MTC OUT on: the ports CLOCK OUT names, whatever CLOCK MODE says; 0 sends none)
    std::uint8_t cc_channel = 0;            // the device's notes-out channel, 0..15: where kChannelDefault goes
    std::uint8_t cc_number[6] = {kCcUnassigned, kCcUnassigned, kCcUnassigned, kCcUnassigned, kCcUnassigned, kCcUnassigned};  // CC A..F
    std::uint8_t usb_cable = 0;
    // OUT OFFSET per port, CPU cycles, signed: delays (+) or advances (-) everything sent on that port.
    std::int32_t offset_cycles[kPortCount] = {0, 0};
  };

  struct Stats {
    std::uint32_t queued = 0;
    std::uint32_t handed = 0;
    std::uint32_t staging_overflow = 0;   // dropped: the staging queue was full
    std::uint32_t port_full = 0;          // an item waited because a port's ring was full (counted once per block per port)
    std::uint32_t stray_offs = 0;         // note-offs that were not owed and so not sent
    std::uint32_t dropped_cc = 0;         // a CC slot with no number
    std::uint32_t suppressed = 0;
    std::uint32_t panic_offs = 0;
  };

  MidiOut() = default;

  void configure(const Config& c) noexcept { cfg_ = c; }
  const Config& config() const noexcept { return cfg_; }
  // The ports. A null port drops what is addressed to it (counted nowhere: it is not attached).
  void attach(DinEmitter* din, UsbMidiOut* usb) noexcept {
    din_ = din;
    usb_ = usb;
  }
  void set_suppress(bool on) noexcept { suppress_ = on; }
  bool suppress() const noexcept { return suppress_; }
  void reset() noexcept;

  // One sequencer event of a MIDI kind, in the block that starts at `block_start` (the timeline's sample). Other kinds are ignored (false).
  bool send(const audio::SeqEvent& ev, std::uint64_t block_start) noexcept;

  // At the end of a block: hands the ports what is due before `block_end_cycles + kHorizonCycles` and pumps the panic. `block_end_cycles` is the
  // block's end on the audio timeline in CPU cycles.
  void flush(std::uint64_t block_end_cycles) noexcept;

  // The transport stopped or the project closed: queued notes that were not handed over are dropped, and every note still owed gets an off, a few
  // per block from the next flush(), due one horizon after `due_cycles` (behind whatever the ports already hold). A restart within that horizon
  // that sounds the same note on the same channel would be cut by the off: a stop followed by a play inside 3 ms, which no player does.
  void stop_all(std::uint64_t due_cycles) noexcept;
  // The notes of one channel only, for a muted MIDI track whose channel is known (everything else keeps sounding).
  void release_channel(std::uint8_t ch, std::uint64_t due_cycles) noexcept;

  const HangBook& book() const noexcept { return book_; }
  const Stats& stats() const noexcept { return stats_; }
  std::uint32_t staged() const noexcept { return staging_.size(); }
  bool panic_pending() const noexcept { return panic_pending_; }

 private:
  struct Staged {
    Msg m;
    std::uint8_t port;   // 0 DIN, 1 USB
    std::uint8_t rt;     // 1 real-time byte; kStagedFullFrame: an MTC full frame (Msg::b holds the packed time, low byte first)
    std::uint8_t pay;    // a note-on: this copy is the one that pays the hang book if it is dropped before it is handed over (the first port that took the note)
  };
  // Queues the message for each port in `ports`; returns the ports that took it (a port whose queue was full lost it: staging_overflow).
  std::uint8_t stage(std::uint64_t due, std::uint8_t ports, const Msg& m, bool rt, bool note_on = false) noexcept { return stage_as(due, ports, m, rt ? 1u : 0u, note_on); }
  std::uint8_t stage_as(std::uint64_t due, std::uint8_t ports, const Msg& m, std::uint8_t rt_field, bool note_on) noexcept;
  // OS-MTC A3: a staged full frame (see the header); the packed time is midi::mtc::pack_full()'s, 24 bits.
  std::uint8_t stage_full_frame(std::uint64_t due, std::uint8_t ports, std::uint32_t packed) noexcept;
  std::uint64_t offset_due(std::uint64_t due, std::uint32_t port) const noexcept;

  Config cfg_{};
  DinEmitter* din_ = nullptr;
  UsbMidiOut* usb_ = nullptr;
  TimedQueue<Staged, kStagingSize> staging_;
  HangBook book_;
  bool suppress_ = false;
  bool panic_pending_ = false;
  std::uint16_t panic_channels_ = 0;  // the channels whose owed notes the pump is releasing
  std::uint64_t panic_due_ = 0;
  // The panic's off that found no room in the staging queue (OS-SMALL3): the ports that still lack it, retried first at the next flush. 0 = none.
  std::uint8_t retry_ports_ = 0;
  std::uint8_t retry_ch_ = 0;
  std::uint8_t retry_note_ = 0;
  // OS-INSTUI S8b: the ports the notes still owed went to (an instrument may route its notes to a port of its own), so that a panic's offs go to all of them. Cleared when nothing is owed.
  std::uint8_t used_ports_ = 0;
  std::uint64_t retry_due_ = 0;
  Stats stats_{};
};

}  // namespace o3os::midi
