// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The user's MIDI settings as the engine acts on them (OS-005c2): clock mode, the port that is followed, the ports clock and transport go out on, the
// per-port offsets, the notes' routing. A plain value the main loop (the Config screen's MIDI page, the settings) hands to the engine and the render reads;
// the engine is seq::MidiLink (seq/midi_link.h), which turns it into the sequencer's, the router's and the clock-follow loop's own configurations.
//
// WHAT IS PERSISTENT (OS-013a). The device settings file holds CLOCK IN, CLOCK SYNC, CLOCK MODE, MIDI OUT and OUT CHANNEL, which seq::sync_config_from_settings() turns into `mode`,
// `in_port`, `sync_correction`, `out_ports`, `note_ports` and `notes_channel` (and, since E-CFG-03, TRANSPORT IN / OUT into `transport_in` / `transport_out`; since OS-MTC A3, MTC OUT / MTC START into `mtc_fps` / `mtc_hour`) (MIDI IN and IN CHANNEL filter MIDI Learn's input in the main loop and are not part of this struct).
// The rest (the offsets, FOLLOW, the CC numbers, and CLOCK OUT / the notes' ports as the project may override them) has no row in the settings table. Since OS-004d they are the PROJECT's: the
// project file's MIDI chunk (docs/formats/o3-project.md; seq::project_midi_state / apply_project_midi), applied after the device settings when a project is loaded and stored in the project
// whenever they change; a project without the chunk leaves them as they are, and a reboot returns them to their defaults.
//
// Units: offsets are in 0.1 ms, the radar card's unit, positive = delay.

#include <o3os/core/audio_clock.h>
#include <o3os/midi/in.h>
#include <o3os/midi/mtc.h>
#include <o3os/midi/out.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace o3os::midi {

// Off: no clock, no transport, in or out. Master: this tracker's clock drives the others: 24 ppqn clock, Start, Continue, Stop and Song Position Pointer go out
// on `out_ports` while it plays (the transport itself is the user's). Slave: the clock and the transport come from `in_port`: its clock is followed through
// the clock-follow loop, its Start, Continue, Stop and SPP move the transport; `out_ports` relays them (the clock is the followed clock).
enum class SyncMode : std::uint8_t { Off = 0, Master = 1, Slave = 2 };

// The follow loop's bandwidth, the radar card's FOLLOW TIGHT | NORMAL | LOOSE: B = 1.0, 0.5, 0.25 Hz (midi/clock_dll.h).
enum class FollowTightness : std::uint8_t { Tight = 0, Normal = 1, Loose = 2 };

inline constexpr std::int16_t kMaxOffsetDms = 1000;     // +-100 ms
inline constexpr std::int8_t kMaxSyncCorrection = 6;    // PAR-119, the manual's section 3.7 range
inline constexpr std::uint8_t kPortMask = kPortDin | kPortUsb;

struct MidiConfig {
  SyncMode mode = SyncMode::Off;
  Port in_port = Port::Din;                 // Slave: the port that is followed (CLOCK IN)
  std::uint8_t out_ports = 0;               // kPortDin | kPortUsb: clock, Start, Continue, Stop, SPP go out here (CLOCK OUT)
  std::uint8_t note_ports = kPortDin;       // notes, CC, program change of MIDI tracks (off, usb, jack, both)
  std::uint8_t notes_channel = 0;           // 0..15: where a MIDI event without a channel goes
  FollowTightness follow = FollowTightness::Normal;
  std::int8_t sync_correction = 0;          // PAR-119: pulses added to the clock number a Start or Continue begins on, -6..+6
  std::uint8_t cc_number[6] = {kCcUnassigned, kCcUnassigned, kCcUnassigned, kCcUnassigned, kCcUnassigned, kCcUnassigned};  // CC A..F, kCcUnassigned: sends nothing
  std::int16_t in_offset_dms[kPortCount] = {0, 0};    // IN OFFSET per port, 0.1 ms: the followed clock's delay (+) or advance (-); an advance reaches back one clock period at most
  std::int16_t out_offset_dms[kPortCount] = {0, 0};   // OUT OFFSET per port, 0.1 ms: everything sent on that port is delayed (+) or advanced (-)
  // E-CFG-03: TRANSPORT IN / OUT of the Config MIDI page (device settings 23 and 24; sync_config_from_settings). Written by the main loop with the rest of the value, read by the render at a block start.
  bool transport_in = true;                 // false: Start, Continue, Stop and SPP from the followed port are ignored (the clock is still followed)
  bool transport_out = true;                // false: no Start, Continue, Stop or SPP goes out (the clock still does)
  // OS-MTC A3: MTC OUT and MTC START of the Config MIDI page (device settings 27 and 28; sync_config_from_settings). The time code goes to `out_ports`, as the clock does.
  std::uint8_t mtc_fps = 0;                 // 0 Off, else 24, 25 or 30: the quarter frames and full frames the sequencer makes (midi/mtc.h)
  std::uint8_t mtc_hour = 1;                // 0..23: the hour of the time code at the song's first step (Logic's default plays bar 1 at 01:00:00:00)
  // OS-MTC B3: MTC IN of the Config MIDI page (device setting 29; sync_config_from_settings). The unit chases the time code of that port (midi/mtc_in.h); not Off turns the clock follow off and mutes MTC OUT.
  std::uint8_t mtc_in = 0;                  // 0 Off, 1 DIN, 2 USB
};
static_assert(std::is_trivially_copyable<MidiConfig>::value, "the config is copied bytewise through SeqLockBox");

namespace detail {
template <typename T>
constexpr T clamp_to(T v, T lo, T hi) noexcept {
  return v < lo ? lo : (v > hi ? hi : v);
}
}  // namespace detail

// The value with every field forced into its range (what the engine acts on): an out-of-range value is clamped, never refused.
constexpr MidiConfig sanitized(MidiConfig c) noexcept {
  c.mode = static_cast<std::uint8_t>(c.mode) > static_cast<std::uint8_t>(SyncMode::Slave) ? SyncMode::Off : c.mode;
  c.in_port = static_cast<std::uint8_t>(c.in_port) >= kPortCount ? Port::Din : c.in_port;
  c.out_ports = static_cast<std::uint8_t>(c.out_ports & kPortMask);
  c.note_ports = static_cast<std::uint8_t>(c.note_ports & kPortMask);
  c.notes_channel = static_cast<std::uint8_t>(c.notes_channel & 0x0Fu);
  c.follow = static_cast<std::uint8_t>(c.follow) > static_cast<std::uint8_t>(FollowTightness::Loose) ? FollowTightness::Normal : c.follow;
  c.sync_correction = detail::clamp_to<std::int8_t>(c.sync_correction, -kMaxSyncCorrection, kMaxSyncCorrection);
  for (std::uint8_t& n : c.cc_number) {
    n = n > 127u ? kCcUnassigned : n;
  }
  c.mtc_fps = mtc::valid_fps(c.mtc_fps) ? c.mtc_fps : std::uint8_t{0};
  c.mtc_hour = c.mtc_hour > mtc::kMaxHour ? mtc::kMaxHour : c.mtc_hour;
  c.mtc_in = c.mtc_in > 2u ? std::uint8_t{0} : c.mtc_in;
  for (std::uint32_t p = 0; p < kPortCount; ++p) {
    c.in_offset_dms[p] = detail::clamp_to<std::int16_t>(c.in_offset_dms[p], -kMaxOffsetDms, kMaxOffsetDms);
    c.out_offset_dms[p] = detail::clamp_to<std::int16_t>(c.out_offset_dms[p], -kMaxOffsetDms, kMaxOffsetDms);
  }
  return c;
}

// What the Config page shows, published by the render once a block (relaxed single-word stores, one writer) and read by the main loop. A set of words that may be a
// block apart, which is all a status line needs.
struct SyncStatus {
  bool following = false;       // the sequencer's timeline is the master's clock
  bool clock_present = false;   // a clock of the followed port has been seen and is not stale
  bool locked = false;          // the loop's own verdict (midi/clock_dll.h locked())
  std::uint32_t bpm_x100 = 0;   // the followed clock's tempo, 0 without one
  std::uint32_t clocks = 0;     // clocks of the followed port the loop has been given since the mode was set
  std::uint32_t error_us = 0;   // |the loop's last error| in microseconds
  std::uint32_t transport_events = 0;  // Start, Continue, Stop, SPP applied since boot
  // OS-MTC B3 (MTC IN not Off; all 0 otherwise): the chaser's state for the Config row's info line (`MTC 01:00:12:05 25 LOCKED`) and the console.
  std::uint8_t mtc_state = 0;           // MtcState below
  std::uint8_t mtc_code = 1;            // the master's rate code: 0 24, 1 25, 2 29.97 drop-frame, 3 30
  std::uint32_t mtc_time = 0;           // the last time the master named, packed hh << 24 | mm << 16 | ss << 8 | ff (0 until a whole cycle was read)
  std::int32_t mtc_rate_ppm = 0;        // the master's rate against the unit's audio clock, parts per million from 1.0
  std::int32_t mtc_error_us = 0;        // the run's phase error against the master at the last step, microseconds (positive: the unit is ahead)
  std::uint32_t mtc_relocations = 0;    // relocations and stops of the chase since the mode was set
};
// What a chase says (SyncStatus::mtc_state). Off: MTC IN is Off. Waiting: no master yet (Idle). Reading: quarter frames come, the time is not known yet. Armed: the start is scheduled (the unit's run began or waits for its sample). Locked: the run agrees with the master.
// Freewheel: quarter frames are missing, the run goes on at the last rate. Rate: the master's rate is outside 0.94..1.06 (not followed). PastEnd: the master is past the song's end (silence, it waits for a locate back).
enum class MtcState : std::uint8_t { Off = 0, Waiting = 1, Reading = 2, Armed = 3, Locked = 4, Freewheel = 5, Rate = 6, PastEnd = 7 };

// 0.1 ms in the units the engine works in.
constexpr std::int32_t offset_us(std::int16_t dms) noexcept { return static_cast<std::int32_t>(dms) * 100; }
constexpr std::int32_t offset_cycles(std::int16_t dms) noexcept { return static_cast<std::int32_t>(dms) * static_cast<std::int32_t>(core::kCpuHz / 10000u); }
static_assert(core::kCpuHz % 10000u == 0u, "0.1 ms is a whole number of CPU cycles");

// The loop bandwidth of a tightness, millihertz.
constexpr std::uint32_t follow_mhz(FollowTightness f) noexcept {
  return f == FollowTightness::Tight ? 1000u : (f == FollowTightness::Loose ? 250u : 500u);
}

// A value passed from ONE writer context (the main loop) to ONE reader of higher priority on the same core (the render): a sequence lock that never makes
// the reader wait. write() makes the count odd, stores, makes it even; read_if_changed() returns false when the count is odd (the writer was preempted in
// the middle: the reader keeps what it has and tries again next block), when it has not moved since the last successful read, or when it moved during the
// copy. On one core the reader cannot run while the writer does, only in the middle of it, so the copy is never torn; the double check also covers a reader
// of another core. The latest write always wins; none is queued.
template <typename T>
class SeqLockBox {
  static_assert(std::is_trivially_copyable<T>::value, "the value is copied bytewise");

 public:
  // The writer's context only.
  void write(const T& v) noexcept {
    begin_write();
    put(v);
    end_write();
  }

  // The reader's context only. `seen` is the reader's own copy of the count after its last successful read (start at 0: a box that was never written has count 0).
  bool read_if_changed(T& out, std::uint32_t& seen) const noexcept {
    const std::uint32_t s1 = seq_.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0u || s1 == seen) {
      return false;
    }
    T copy;
    std::memcpy(&copy, &value_, sizeof(T));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq_.load(std::memory_order_relaxed) != s1) {
      return false;
    }
    out = copy;
    seen = s1;
    return true;
  }

  // The writer's own view of what it last wrote (a plain read from the writer's context).
  const T& last_written() const noexcept { return value_; }

 protected:
  // The three steps of write(), apart so that a test can put a reader between them (a reader that preempts the writer).
  void begin_write() noexcept {
    seq_.store(seq_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);  // odd: a write is in progress
    std::atomic_thread_fence(std::memory_order_release);
  }
  void put(const T& v) noexcept { std::memcpy(&value_, &v, sizeof(T)); }
  void end_write() noexcept { seq_.store(seq_.load(std::memory_order_relaxed) + 1u, std::memory_order_release); }  // even again: a new value

 private:
  T value_{};
  std::atomic<std::uint32_t> seq_{0};
};

}  // namespace o3os::midi
