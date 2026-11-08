// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI in, the timing side (OS-005c; ADR 0013 D5 "MIDI in: bytes timestamped in the UART/USB ISR (H13 out) -> midiRt ring -> clock-follow PLL,
// Start/Stop/Continue/SPP as transport events; PAR-119 correction reserved until it is specified").
//
// The ISR of each port (DIN's UART, the USB MIDI endpoint) stamps every byte it receives with the cycle counter and pushes one RtItem into the
// `midiRt` ring (core::SpscRing, one producer per ring: one ring for each port, or both ISRs at one priority sharing one; the driver's choice,
// OS-005c2). The render drains it at the start of every block, which is all MidiIn::drain does:
//
//   * each byte goes through a small per-port parser that knows where a message ends (a real-time byte may sit inside any message, a Song
//     Position Pointer is three bytes), and splits the stream into clock, start, continue, stop, SPP and everything else;
//   * the selected port's clocks go to the ClockDll, corrected for the port's input latency: a DIN byte is stamped when it is complete, 320 us
//     (one byte time) after the start bit, which is when the sender's clock happened; a USB byte is stamped at the frame it arrived in, which is
//     on average 0.5 ms after it was sent (the card's finding: a documented constant per port, so that the ports compare equal);
//   * Start and Continue "take effect on the next Timing Clock" (the standard), so they wait for it and then become a TransportEvent carrying
//     the number of that clock; SPP and Stop become events at once;
//   * the other port's real-time bytes are dropped (counted), and every byte that is not timing goes back to the caller (`pass`), untouched and in
//     order, for the MIDI input of the tracks, MIDI Learn and the note path.
//
// The USER's input offset (IN OFFSET: signed, in 0.1 ms, per port) is not applied here: the loop follows the true clock, and the sequencer shifts
// its own timeline by the offset (seq::Sequencer::set_follow), where a delay is a shift and an advance is limited to one clock period.
//
// MIDI TIME CODE (OS-MTC B2; docs/specs/OS-MTC.md 4.1). With `Config::mtc_enabled` and a sink handed to drain(), the parser of the MTC port also knows two kinds: a quarter frame
// (`F1 0nnn dddd`: the F1 is consumed, its data byte becomes `MtcQf`) and a full frame (the SysEx `F0 7F <any device id> 01 01 hh mm ss ff F7` with valid fields: the F7 becomes `MtcFull`
// carrying the packed time of mtc::pack_full()). Both go to the sink as an MtcEvent whose stamp is corrected for the port's latency to the instant the sender began the message (a DIN
// quarter frame is stamped when its second byte is complete, a full frame when its last is). The bytes of a full frame still go to `pass`: a SysEx reaches the note path complete, as every
// SysEx does today (it skips it); only the quarter frames are taken. The other port, and every port with MTC off or no sink, parses exactly as before: F1 and its data byte pass. A
// real-time byte (a MIDI clock) inside either message does not disturb it. The chaser that reads the sink is midi/mtc_in.h.
//
// PAR-119 (Clock Sync Correction, -6..+6 pulses, applied once after a restart): the behaviour is not specified (the official manual gives two ranges); the hook is
// MidiIn::Config::start_pulses, an integer added to the clock number of a Start or Continue, default 0 and not used by anything else.

#include <o3os/core/ring.h>
#include <o3os/midi/clock_dll.h>
#include <o3os/midi/din.h>

#include <cstdint>
#include <type_traits>

namespace o3os::midi {

enum class Port : std::uint8_t { Din = 0, Usb = 1 };
inline constexpr std::uint32_t kPortCount = 2;

// One stamped byte, as the ISR pushes it.
struct RtItem {
  std::uint32_t cycles;  // the cycle counter when the byte was complete
  std::uint8_t byte;
  std::uint8_t port;     // Port
  std::uint16_t pad;
};
static_assert(sizeof(RtItem) == 8 && std::is_trivially_copyable<RtItem>::value, "RtItem is two words");

inline constexpr std::uint32_t kRtInRingSize = 256;
using RtInRing = core::SpscRing<RtItem, kRtInRingSize>;  // the `midiRt` ring of ADR 0013 D1

// What a byte is, once the parser has seen where it sits.
enum class ByteKind : std::uint8_t {
  Pass,       // not timing: give it back to the caller
  Consumed,   // part of an SPP, not complete yet
  Clock,
  Start,
  Continue,
  Stop,
  Spp,        // the third byte of an SPP: `value` is the position in MIDI beats
  // MTC (only when the parser was told so, set_mtc(true); additive):
  MtcQf,      // the data byte of a quarter frame: `value` is that byte `0nnn dddd` (the F1 before it was Consumed)
  MtcFull,    // the F7 of a valid full frame: `value` is the packed time (mtc::pack_full); the byte itself is also Pass for the caller
};

class InParser {
 public:
  struct Out {
    ByteKind kind;
    std::uint32_t value;  // Spp: the position (14 bits); MtcQf: the data byte; MtcFull: the packed time (24 bits)
  };
  Out feed(std::uint8_t b) noexcept;
  void reset() noexcept { *this = InParser{}; }
  // The MTC kinds are recognised only while this is on (the port MTC IN names). Off, F1 and the bytes of a full frame are what they always were: not ours.
  void set_mtc(bool on) noexcept {
    mtc_ = on;
    if (!on) {
      qf_ = false;
      sx_ok_ = false;
    }
  }

 private:
  std::uint8_t status_ = 0;  // running status (channel voice) or the system-common status being collected
  std::uint8_t need_ = 0;
  std::uint8_t have_ = 0;
  std::uint8_t d_[2] = {0, 0};
  bool sysex_ = false;
  bool mtc_ = false;
  bool qf_ = false;                      // an F1 was taken and its data byte is next
  bool sx_ok_ = false;                   // the SysEx so far could still be a full frame
  std::uint8_t sx_n_ = 0;                // data bytes of the SysEx so far (saturating at 9)
  std::uint8_t sx_d_[4] = {0, 0, 0, 0};  // hh mm ss ff
};

// One MTC message for the chaser (midi/mtc_in.h), in the order the bytes came.
struct MtcEvent {
  enum class Kind : std::uint8_t { Qf, Full };
  Kind kind;
  std::uint8_t port;
  std::uint8_t data;     // Qf: the data byte `0nnn dddd`
  std::uint8_t pad;
  std::uint32_t value;   // Full: the packed time (mtc::pack_full)
  std::uint32_t cycles;  // the cycle counter when the sender began the message (the stamp minus the port's latency)
};
static_assert(sizeof(MtcEvent) == 12 && std::is_trivially_copyable<MtcEvent>::value, "MtcEvent is three words");

struct TransportEvent {
  enum class Kind : std::uint8_t { Start, Continue, Stop, Spp };
  Kind kind;
  std::uint8_t port;
  std::uint16_t spp;     // Spp: the position in MIDI beats (steps), 0..16383
  std::uint64_t tick;    // Start, Continue: the number (ClockDll::tick) of the clock the transport begins on
};

class MidiIn {
 public:
  struct Config {
    Port clock_port = Port::Din;   // CLOCK IN: which port's clock and transport are followed
    bool clock_enabled = false;    // CLOCK IN is not INTERNAL
    std::int8_t start_pulses = 0;  // PAR-119, reserved: added to the clock number of a Start or Continue
    bool mtc_enabled = false;      // MTC IN is not Off (OS-MTC B2): the MTC port's quarter frames and full frames go to the sink of drain(); every other byte is unchanged
    Port mtc_port = Port::Usb;     // MTC IN: which port's time code is taken
    // The input latency of each port, CPU cycles, subtracted from a stamp: a DIN byte is stamped one byte time after the sender's clock, a USB one
    // 0.5 ms after on average.
    std::uint32_t latency_cycles[kPortCount] = {static_cast<std::uint32_t>(din::kByteCycles), core::kCpuHz / 2000u};
  };

  struct Stats {
    std::uint32_t bytes[kPortCount] = {0, 0};
    std::uint32_t clocks = 0;
    std::uint32_t ignored_port = 0;  // timing bytes of the port that is not followed
    std::uint32_t transport = 0;
    std::uint32_t passed = 0;
    std::uint32_t pass_overflow = 0;
    std::uint32_t mtc_qf = 0;    // quarter frames handed to the sink
    std::uint32_t mtc_full = 0;  // full frames handed to the sink
  };

  struct Drained {
    std::uint32_t events = 0;  // TransportEvents written
    std::uint32_t passed = 0;  // RtItems written to `pass`
    std::uint32_t items = 0;   // items taken from the ring
    std::uint32_t mtc = 0;     // MtcEvents written
  };

  MidiIn() = default;
  void configure(const Config& c) noexcept {
    cfg_ = c;
    armed_ = Armed::None;
  }
  const Config& config() const noexcept { return cfg_; }
  void reset() noexcept;

  // Takes up to `max_items` stamped bytes from the ring (bounded work per block). Transport events go to `ev` (at most `ev_cap`), the bytes
  // that are not timing to `pass` (at most `pass_cap`; when full the item stays in the ring for the next block). `map` converts stamps; `dll`
  // is fed the clocks of the selected port. The MTC kinds go to `mtc` (at most `mtc_cap`, in order) when the config has MTC on and a sink is given (an item with no room waits, like the
  // others); with no sink the call is what it was before MTC.
  Drained drain(RtInRing& ring, const ClockMap& map, ClockDll& dll, std::uint32_t max_items, TransportEvent* ev,
                               std::uint32_t ev_cap, RtItem* pass, std::uint32_t pass_cap, MtcEvent* mtc = nullptr, std::uint32_t mtc_cap = 0) noexcept;

  const Stats& stats() const noexcept { return stats_; }

 private:
  enum class Armed : std::uint8_t { None, Start, Continue };

  Config cfg_{};
  InParser parser_[kPortCount];
  Armed armed_ = Armed::None;
  std::uint8_t armed_port_ = 0;
  RtItem held_{};          // an item popped and not yet processed (no room for its output)
  bool have_held_ = false;
  Stats stats_{};
};

}  // namespace o3os::midi
