// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI controller input for MIDI Learn (OS-013a; docs/formats/o3-settings.md section 6, ADR 0009 rev 1 mechanism 5, ADR 0013 D5).
//
// The bytes that MidiIn::drain hands back as "not timing" (`pass`: status bytes and data bytes of every channel message, untouched, in order) go through one CcParser
// a port, which knows running status and picks out the control changes. Each one is one 32-bit word in a small SPSC ring, from the render (the one producer: MidiLink
// feeds it while it drains the midiRt rings) to the main loop (the one consumer: App::poll_midi_cc), where the `midi.cc` action binds or moves a parameter. WHY the
// main loop applies it: a parameter is written through the editor (undo, generations, the ParamChange ring whose one producer is the main loop, ADR 0009), none of
// which the render may touch; the render only parses, so the cost on its side is a few instructions a byte and a ring push per message.
//
// NEW COMMUNITY OS BEHAVIOUR (MIDI Learn is not in the official manual, official-manual-map): channel mode messages (controllers 120..127: all sound off, reset controllers, ...) are not controls and are
// dropped here, so that "the next incoming CC binds" can never bind an all-notes-off. Which port and channel are listened to (the device settings MIDI IN and IN CHANNEL)
// is decided by the consumer, from the settings themselves, so that nothing is copied to the render and a change takes effect at once.
//
// NOTES IN (OS-005g). The same parser also picks out the note messages: note on (a note on with velocity 0 is a note off), note off, and the channel mode messages all sound
// off (120) and all notes off (123), which are an all-notes-off here and still never a control. They go to a SECOND ring (NoteMsg, 32 words) with the same producer and consumer
// (App::poll_midi_notes), not to the control-change ring: a knob flood must never be able to push out the note off of a held note (a stuck note). The main loop applies the
// message by handing it to the live note queue (audio/live_notes.h), the render's way into the voices; the extra hop costs one pass of the main loop (about 16 ms) before the block
// that starts the note. A full note ring drops the message and counts it (note_dropped): the consumer then sends an all-notes-off for the keyboard, so a lost off cannot leave a note
// ringing.
//
// Real time and storage: bounded (one parser step a byte, one push a message), no allocation, no blocking. The object is plain storage with no constructor work beyond
// zeroing: the platform places it in SDRAM (about 170 bytes) so that SRAM_U, which is nearly all spoken for, grows by one pointer (MidiLink::attach_cc). A full ring drops
// the new message and counts it; absolute controller values catch up with the next message (a relative one loses its increment, which a flood can cause only if the main
// loop stalls).

#include <o3os/core/ring.h>
#include <o3os/midi/in.h>
#include <o3os/midi/message.h>
#include <o3os/midi/monitor.h>

#include <atomic>
#include <cstdint>

namespace o3os::midi {

inline constexpr std::uint8_t kCcFirstChannelMode = 120;  // controllers from here up are channel mode messages
inline constexpr std::uint8_t kCcAllSoundOff = 120;       // ... of which these two are an all-notes-off for the notes in (OS-005g)
inline constexpr std::uint8_t kCcAllNotesOff = 123;

// A control change as the consumer sees it: `channel` 1..16 (the wire's 0..15 plus one, the unit of LearnBinding::channel and of the `midi.cc` action), `cc` and `value` 0..127.
struct CcMsg {
  std::uint8_t port;  // Port
  std::uint8_t channel;
  std::uint8_t cc;
  std::uint8_t value;
};

constexpr std::uint32_t cc_pack(const CcMsg& m) noexcept {
  return static_cast<std::uint32_t>(m.port) | (static_cast<std::uint32_t>(m.channel) << 8) | (static_cast<std::uint32_t>(m.cc) << 16) |
         (static_cast<std::uint32_t>(m.value) << 24);
}
constexpr CcMsg cc_unpack(std::uint32_t w) noexcept {
  return CcMsg{static_cast<std::uint8_t>(w & 0xFFu), static_cast<std::uint8_t>((w >> 8) & 0xFFu), static_cast<std::uint8_t>((w >> 16) & 0xFFu),
               static_cast<std::uint8_t>((w >> 24) & 0xFFu)};
}

// A note message as the consumer sees it: `channel` 1..16, `note` 0..127, `velocity` 1..127 for a note on, 0..127 (the release velocity) for a note off, 0 for an all-notes-off
// (then `note` is the controller, 120 or 123).
enum class NoteKind : std::uint8_t { On = 1, Off = 2, AllOff = 3 };
struct NoteMsg {
  std::uint8_t port;
  std::uint8_t channel;
  NoteKind kind;
  std::uint8_t note;
  std::uint8_t velocity;
};
// Five fields in a 32-bit word: the port (0..1) and the kind (1..3) share a byte, the velocity is 7 bits and the note is 7 bits, so the word is port | kind << 4 | channel << 8
// | note << 16 | velocity << 24.
constexpr std::uint32_t note_pack(const NoteMsg& m) noexcept {
  return static_cast<std::uint32_t>(m.port & 0x0Fu) | (static_cast<std::uint32_t>(m.kind) << 4) | (static_cast<std::uint32_t>(m.channel) << 8) |
         (static_cast<std::uint32_t>(m.note & 0x7Fu) << 16) | (static_cast<std::uint32_t>(m.velocity & 0x7Fu) << 24);
}
constexpr NoteMsg note_unpack(std::uint32_t w) noexcept {
  return NoteMsg{static_cast<std::uint8_t>(w & 0x0Fu), static_cast<std::uint8_t>((w >> 8) & 0xFFu), static_cast<NoteKind>((w >> 4) & 0x0Fu),
                 static_cast<std::uint8_t>((w >> 16) & 0xFFu), static_cast<std::uint8_t>((w >> 24) & 0xFFu)};
}

// OS-MIDIREC-P1 S3 (A8): the stamped copy of a message, for a Lane recording. While a take is open the render pushes each note and control change a second time, here, with the
// position of the recording track at the block that drained it (the pattern and the lane tick, step * 48 + microstep). 8 bytes. `channel` is 1..16 like the other rings; `a` / `b` are
// the note and velocity (On, Off), the controller and value (Cc) or the controller 120 / 123 and 0 (AllOff).
enum class StampKind : std::uint8_t { On = 1, Off = 2, AllOff = 3, Cc = 4 };
struct StampMsg {
  std::uint16_t tick;
  StampKind kind;
  std::uint8_t port;
  std::uint8_t channel;
  std::uint8_t a;
  std::uint8_t b;
  std::uint8_t pattern;
};
static_assert(sizeof(StampMsg) == 8, "a stamped message is two words");

// One channel message that the parser picked out: a control change (`cc` is the controller, `value` its value) or a note message (see NoteMsg; `note` is in `cc`, `velocity` in `value`).
struct ChannelMsg {
  enum class Kind : std::uint8_t { Cc, Note, Bend };  // Bend (OS-PIANOBEND-REC): the pitch wheel, `cc` = the low 7 bits and `value` the high 7 bits of the 14-bit value (8192 = centre)
  Kind kind;
  NoteKind note_kind;  // when kind is Note
  std::uint8_t ch;     // 0..15 as on the wire
  std::uint8_t cc;
  std::uint8_t value;
};

// Running-status parser for the control changes and the notes of one port. Real-time bytes (0xF8..0xFF) are ignored without touching the state (they may sit inside a message); a system
// common or SysEx byte cancels running status (so SysEx data is never read as a message); a status byte starts a message; data bytes complete it, and with running status
// the next pair is another message of the same kind. (A Song Position Pointer is consumed by MidiIn and never reaches this parser: a sender that interleaves one with
// running status and does not repeat the status byte afterwards is not handled; the standard says system common cancels running status.)
class CcParser {
 public:
  // True when `b` completes a control change: `ch` (0..15 as on the wire), `cc` and `value` are written. Notes are not reported here (next() does).
  bool feed(std::uint8_t b, std::uint8_t& ch, std::uint8_t& cc, std::uint8_t& value) noexcept {
    ChannelMsg m{};
    if (next(b, m) && m.kind == ChannelMsg::Kind::Cc) {
      ch = m.ch;
      cc = m.cc;
      value = m.value;
      return true;
    }
    return false;
  }
  // True when `b` completes a control change or a note message (a note on with velocity 0 is a note off; the controllers 120 and 123 are an all-notes-off, the other channel mode
  // controllers are dropped). `m` is written when it is true.
  bool next(std::uint8_t b, ChannelMsg& m) noexcept {
    if (is_realtime(b)) {
      return false;
    }
    if (is_status(b)) {
      status_ = b < 0xF0u ? b : std::uint8_t{0};
      have_ = 0;
      return false;
    }
    if (status_ == 0u) {
      return false;  // a data byte of a message that is not ours (SysEx, system common) or with no status yet
    }
    if (data_bytes_of(status_) == 1u) {
      return false;  // program change, channel pressure: one data byte, nothing to pick out
    }
    if (have_ == 0u) {
      first_ = b;
      have_ = 1;
      return false;
    }
    have_ = 0;  // running status: the next data byte starts another message of this kind
    m.ch = static_cast<std::uint8_t>(status_ & 0x0Fu);
    m.cc = first_;
    m.value = b;
    switch (status_ & 0xF0u) {
      case 0x80u:
        m.kind = ChannelMsg::Kind::Note;
        m.note_kind = NoteKind::Off;
        return true;
      case 0x90u:
        m.kind = ChannelMsg::Kind::Note;
        m.note_kind = b == 0u ? NoteKind::Off : NoteKind::On;
        return true;
      case 0xB0u:
        if (first_ < kCcFirstChannelMode) {
          m.kind = ChannelMsg::Kind::Cc;
          return true;
        }
        if (first_ == kCcAllSoundOff || first_ == kCcAllNotesOff) {
          m.kind = ChannelMsg::Kind::Note;
          m.note_kind = NoteKind::AllOff;
          m.value = 0;
          return true;
        }
        return false;
      case 0xE0u:
        m.kind = ChannelMsg::Kind::Bend;
        return true;
      default:
        return false;  // poly pressure
    }
  }
  void reset() noexcept { *this = CcParser{}; }

 private:
  std::uint8_t status_ = 0;  // the running channel status, 0 when there is none
  std::uint8_t have_ = 0;    // data bytes of the message seen so far (0 or 1)
  std::uint8_t first_ = 0;   // the first data byte, once seen
};

class CcInput {
 public:
  static constexpr std::uint32_t kRingSize = 32;  // power of two; a pass of the main loop takes 16 (App::kMidiCcPerPass), the render adds at most a few messages a block
  static constexpr std::uint32_t kNoteRingSize = 32;  // the notes' own ring (OS-005g); a pass takes 16 (App::kMidiNotesPerPass)
  static constexpr std::uint32_t kStampRingSize = 256;  // OS-MIDIREC-P1 S3: the stamped ring (2 KiB); a pass of the main loop takes up to 64 (App kLaneStampPerPass)
  static constexpr std::uint8_t kNoTrack = 0xFF;        // take_track(): no take is open

  // ---- the stamp (OS-MIDIREC-P1 S3) -----------------------------------------------------------------------------------------------
  // Single-writer protocol. `take_track_` is written by the main loop only (release) and read by the render once a block (acquire): the track whose take is open, kNoTrack when none. The stamp
  // (`stamp_on_`, pattern, tick) is written and read by the render only (MidiLink sets it once a block before it feeds the bytes, from the sequencer's own position), so it needs no atomic.
  // The stamped ring has the render as its one producer and the main loop as its one consumer. With no take open nothing here is touched: feed() is what it was.
  void open_take(std::uint8_t track) noexcept { take_track_.store(track < 16u ? track : kNoTrack, std::memory_order_release); }  // main loop
  void close_take() noexcept { take_track_.store(kNoTrack, std::memory_order_release); }                                      // main loop
  std::uint8_t take_track() const noexcept { return take_track_.load(std::memory_order_acquire); }                              // render (and the main loop)
  void set_stamp(std::uint8_t pattern, std::uint32_t tick) noexcept {                                                           // render
    stamp_on_ = true;
    stamp_pattern_ = pattern;
    stamp_tick_ = static_cast<std::uint16_t>(tick);
  }
  void clear_stamp() noexcept { stamp_on_ = false; }  // render

  // ---- the render (the one producer) ------------------------------------------------------------------------------------------
  // One byte that MidiIn did not take for timing, with the port it came from.
  void feed(const RtItem& item) noexcept {
    const std::uint32_t port = item.port < kPortCount ? item.port : 0u;
    monitor_.feed(port, item.byte);  // OS-H1: the apps' MIDI-in ring (midi/monitor.h): off unless an app asked, then every channel message of both ports
    ChannelMsg m{};
    if (!parser_[port].next(item.byte, m)) {
      return;
    }
    if (m.kind == ChannelMsg::Kind::Bend) {
      const std::uint32_t f = bend_filter_.load(std::memory_order_relaxed);  // MIDI IN / IN CHANNEL: a bend of another port or channel must not overwrite one that is listened to
      if (((f >> 8) < 2u && (f >> 8) != port) || ((f & 0xFFu) != 0u && (f & 0xFFu) != m.ch + 1u)) {
        return;
      }
      // OS-PIANOBEND-REC: the wheel is a level, so only the latest value matters: one mailbox word (a flood of bends cannot fill the note ring and cost a note), not stamped into a take.
      bend_.store(kBendPending | (port << 24) | ((m.ch + 1u) << 16) | (static_cast<std::uint32_t>(m.value) << 7) | m.cc, std::memory_order_release);
      return;
    }
    if (stamp_on_) {
      push_stamped(static_cast<std::uint8_t>(port), m);
    }
    if (m.kind == ChannelMsg::Kind::Cc) {
      const CcMsg c{static_cast<std::uint8_t>(port), static_cast<std::uint8_t>(m.ch + 1u), m.cc, m.value};
      if (!ring_.push(cc_pack(c))) {
        dropped_.store(dropped_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);  // one writer
      }
    } else {
      const NoteMsg n{static_cast<std::uint8_t>(port), static_cast<std::uint8_t>(m.ch + 1u), m.note_kind, m.cc, m.value};
      if (!notes_.push(note_pack(n))) {
        note_dropped_.store(note_dropped_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);  // one writer
      }
    }
  }

  // ---- the main loop (the one consumer) -----------------------------------------------------------------------------------------
  // The apps' MIDI-in ring (OS-H1): the render fills it while an app that holds O3M_CAP_MIDI_IN runs, the app module host drains it.
  MidiMonitor& monitor() noexcept { return monitor_; }
  bool pop(CcMsg& out) noexcept {
    std::uint32_t w = 0;
    if (!ring_.pop(w)) {
      return false;
    }
    out = cc_unpack(w);
    return true;
  }
  bool pop_note(NoteMsg& out) noexcept {
    std::uint32_t w = 0;
    if (!notes_.pop(w)) {
      return false;
    }
    out = note_unpack(w);
    return true;
  }
  // OS-PIANOBEND-REC: the latest pitch bend since the last call (main loop): `value14` 0..16383 (8192 = centre), `channel` 1..16, `port`. False when none came. A bend that arrives between
  // two calls replaces the one before (single mailbox word, exchange: a store of the render after the exchange is taken by the next call).
  struct BendMsg {
    std::uint8_t port;
    std::uint8_t channel;
    std::uint16_t value14;
  };
  // The ports and channel the wheel is listened to (the device settings MIDI IN: 0, 1 or 2 = both, and IN CHANNEL: 0 = every channel, else 1..16); the main loop sets it each pass, the render filters by it before the store.
  void set_bend_filter(unsigned in_port, unsigned in_channel) noexcept {
    bend_filter_.store(((in_port < 2u ? in_port : 2u) << 8) | (in_channel & 0xFFu), std::memory_order_relaxed);
  }
  bool take_bend(BendMsg& out) noexcept {
    const std::uint32_t w = bend_.exchange(0u, std::memory_order_acquire);
    if ((w & kBendPending) == 0u) {
      return false;
    }
    out = BendMsg{static_cast<std::uint8_t>((w >> 24) & 0x0Fu), static_cast<std::uint8_t>((w >> 16) & 0xFFu), static_cast<std::uint16_t>(w & 0x3FFFu)};
    return true;
  }
  // OS-MIDIREC-P1 S3: the next stamped message of the open take (main loop).
  bool pop_stamped(StampMsg& out) noexcept { return stamped_.pop(out); }
  // Stamped messages lost to a full ring since start (the consumer compares it with the last value it saw; a lost note off is closed by the take's end).
  std::uint32_t stamp_dropped() const noexcept { return stamp_dropped_.load(std::memory_order_relaxed); }
  // Note messages lost to a full ring since start (the consumer compares it with the last value it saw and silences the keyboard's notes when it moved).
  std::uint32_t note_dropped() const noexcept { return note_dropped_.load(std::memory_order_relaxed); }
  // Messages lost to a full ring since start (a word the render writes and the main loop reads: a block old at worst).
  std::uint32_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

 private:
  void push_stamped(std::uint8_t port, const ChannelMsg& m) noexcept {
    StampMsg s{};
    s.tick = stamp_tick_;
    s.port = port;
    s.channel = static_cast<std::uint8_t>(m.ch + 1u);
    s.pattern = stamp_pattern_;
    s.a = m.cc;
    s.b = m.value;
    if (m.kind == ChannelMsg::Kind::Cc) {
      s.kind = StampKind::Cc;
    } else {
      s.kind = m.note_kind == NoteKind::On ? StampKind::On : (m.note_kind == NoteKind::Off ? StampKind::Off : StampKind::AllOff);
    }
    if (!stamped_.push(s)) {
      stamp_dropped_.store(stamp_dropped_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);  // one writer
    }
  }

  core::SpscRing<std::uint32_t, kRingSize> ring_;
  core::SpscRing<std::uint32_t, kNoteRingSize> notes_;
  core::SpscRing<StampMsg, kStampRingSize> stamped_;
  std::atomic<std::uint8_t> take_track_{kNoTrack};  // main loop writes (release), render reads (acquire)
  bool stamp_on_ = false;                           // render only
  std::uint8_t stamp_pattern_ = 0;                  // render only
  std::uint16_t stamp_tick_ = 0;                    // render only
  std::atomic<std::uint32_t> stamp_dropped_{0};
  CcParser parser_[kPortCount];  // render only
  MidiMonitor monitor_;
  std::atomic<std::uint32_t> dropped_{0};
  std::atomic<std::uint32_t> note_dropped_{0};
  static constexpr std::uint32_t kBendPending = 0x80000000u;
  std::atomic<std::uint32_t> bend_filter_{2u << 8};  // port bits 8.., channel bits 0..7 (set_bend_filter): the main loop writes, the render reads; the default listens to everything
  std::atomic<std::uint32_t> bend_{0};  // the latest pitch bend: pending bit 31, port bits 24..27, channel 1..16 bits 16..23, value 14 bits; the render stores, the main loop exchanges
};

}  // namespace o3os::midi
