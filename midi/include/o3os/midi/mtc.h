// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI Time Code out (OS-MTC A3; docs/specs/OS-MTC.md section 3). Pure: integer and Q32 arithmetic only, no allocation, no I/O, no double, bounded.
// New Community OS behaviour: the official firmware sends no MTC. The formats are the MIDI 1.0 Detailed Specification's, "MIDI Time Code".
//
// WHAT IS HERE.
//   * the rate table (Off, 24, 25, 30 fps; 29.97 drop-frame is not offered, Q1 a) and the device setting's value to a rate;
//   * the encoders: the quarter-frame data byte `0nnn dddd` of a time, the full-frame SysEx body `7F 7F 01 01 hh mm ss ff`, the packed 24-bit time
//     that a MidiMtcFull SeqEvent carries in `value`;
//   * the exact due time of a quarter frame in Q32 samples, and the number of the first quarter frame at or after a time;
//   * Generator: the quarter-frame and full-frame events of one run, from the run's wall clock and an anchor (spec 3.1). It is the sequencer's
//     to drive: the Sequencer owns one, anchors it at a run start, calls step() once per block and stop() at a stop. It knows nothing of tempo,
//     songs or the project: MTC has no tempo, time code advances with the sample timeline whatever the tempo does.
//
// THE TIME. A time is counted in quarter frames q from 00:00:00:00 (4 x fps a second). Quarter frame q is due where the time reaches q x R / (4 fps)
// samples (R the engine's sample rate: 44,100 Hz in the default build, where the three periods 459.375, 441 and 367.5 samples are exact in Q32.32). Its
// piece is q mod 8, so piece 0 always starts an even frame count and a whole cycle of eight spans two frames; the time a cycle spells is the frame at
// which its piece 0 was sent (a receiver adds two frames when piece 7 arrives). The day is 24 hours, a whole number of cycles at every rate offered:
// the count wraps there.
//
// REAL-TIME RULES. Generator::step() runs in the render context: no allocation, no I/O, a bounded number of events a call (at most kMaxEvents, in
// practice one quarter frame and one full frame), one 32-step Q32 division per quarter frame.

#include <o3os/audio/events.h>
#include <o3os/core/audio_clock.h>
#include <o3os/core/qtime.h>

#include <cstdint>

namespace o3os::midi::mtc {

// The device setting MTC OUT (id 27): 0 Off, 1 = 24 fps, 2 = 25 fps, 3 = 30 fps. `fps` below is 0 (Off) or the frame rate itself.
inline constexpr std::uint8_t kSettingOff = 0;
inline constexpr std::uint8_t kSettingMax = 3;
inline constexpr std::uint8_t kMaxHour = 23;   // MTC START: 00..23 hours (device setting id 28, default 1)

constexpr std::uint8_t fps_of_setting(std::uint32_t v) noexcept { return v == 1u ? 24u : (v == 2u ? 25u : (v == 3u ? 30u : 0u)); }
constexpr bool valid_fps(std::uint32_t fps) noexcept { return fps == 24u || fps == 25u || fps == 30u; }
// The rate code of the wire: 0 = 24, 1 = 25, 2 = 29.97 drop-frame (not offered), 3 = 30. An invalid fps gives 0.
constexpr std::uint8_t rate_code(std::uint32_t fps) noexcept { return fps == 25u ? 1u : (fps == 30u ? 3u : 0u); }

inline constexpr std::uint32_t kSecondsPerDay = 24u * 3600u;
// Quarter frames in a day at `fps` (0 for an invalid rate): 8,294,400 at 24, 8,640,000 at 25, 10,368,000 at 30.
constexpr std::uint32_t quarter_frames_per_day(std::uint32_t fps) noexcept { return valid_fps(fps) ? kSecondsPerDay * 4u * fps : 0u; }

struct Time {
  std::uint8_t h = 0;   // 0..23
  std::uint8_t m = 0;   // 0..59
  std::uint8_t s = 0;   // 0..59
  std::uint8_t f = 0;   // 0..fps-1
};
constexpr bool operator==(const Time& a, const Time& b) noexcept { return a.h == b.h && a.m == b.m && a.s == b.s && a.f == b.f; }

// The time of a frame count from 00:00:00:00, wrapping at 24 hours. An invalid fps gives 00:00:00:00.
constexpr Time time_of_frame(std::uint64_t frame, std::uint32_t fps) noexcept {
  if (!valid_fps(fps)) {
    return Time{};
  }
  const std::uint64_t per_day = std::uint64_t{kSecondsPerDay} * fps;
  const std::uint64_t n = frame % per_day;
  const std::uint64_t secs = n / fps;
  return Time{static_cast<std::uint8_t>(secs / 3600u), static_cast<std::uint8_t>((secs / 60u) % 60u), static_cast<std::uint8_t>(secs % 60u), static_cast<std::uint8_t>(n % fps)};
}

// The frame count of a time (the inverse), for a time that is valid at `fps`.
constexpr std::uint32_t frame_of_time(const Time& t, std::uint32_t fps) noexcept {
  return ((static_cast<std::uint32_t>(t.h) * 60u + t.m) * 60u + t.s) * fps + t.f;
}

// The data byte `0nnn dddd` of quarter frame `piece` (0..7) of `t`: 0 frames low nibble, 1 frames high (one bit), 2 seconds low, 3 seconds high (two bits), 4 minutes low,
// 5 minutes high, 6 hours low, 7 hours high (one bit) with the rate code in bits 1..2.
constexpr std::uint8_t qf_data(std::uint32_t piece, const Time& t, std::uint8_t code) noexcept {
  std::uint32_t nibble = 0;
  switch (piece & 7u) {
    case 0: nibble = t.f & 0x0Fu; break;
    case 1: nibble = (t.f >> 4) & 0x01u; break;
    case 2: nibble = t.s & 0x0Fu; break;
    case 3: nibble = (t.s >> 4) & 0x03u; break;
    case 4: nibble = t.m & 0x0Fu; break;
    case 5: nibble = (t.m >> 4) & 0x03u; break;
    case 6: nibble = t.h & 0x0Fu; break;
    default: nibble = ((t.h >> 4) & 0x01u) | ((code & 0x03u) << 1); break;
  }
  return static_cast<std::uint8_t>(((piece & 7u) << 4) | nibble);
}

// The data byte of quarter frame number `q` (counted from 00:00:00:00, any number: it wraps at the day): the piece is q mod 8 and the time is that of the frame
// at which piece 0 of its cycle was sent.
constexpr std::uint8_t qf_byte(std::uint64_t q, std::uint32_t fps) noexcept {
  const std::uint64_t per_day = quarter_frames_per_day(fps);
  if (per_day == 0u) {
    return 0;
  }
  const std::uint64_t n = q % per_day;
  const std::uint64_t cycle_start = n - (n & 7u);
  return qf_data(static_cast<std::uint32_t>(n & 7u), time_of_frame(cycle_start / 4u, fps), rate_code(fps));
}

// The full frame (a locate; SysEx `F0 7F 7F 01 01 hh mm ss ff F7`) as a 24-bit packed value, which a MidiMtcFull SeqEvent carries in `value`:
// bits 0..4 frames, 5..10 seconds, 11..16 minutes, 17..21 hours, 22..23 the rate code.
constexpr std::uint32_t pack_full(const Time& t, std::uint8_t code) noexcept {
  return (static_cast<std::uint32_t>(t.f) & 0x1Fu) | ((static_cast<std::uint32_t>(t.s) & 0x3Fu) << 5) | ((static_cast<std::uint32_t>(t.m) & 0x3Fu) << 11) |
         ((static_cast<std::uint32_t>(t.h) & 0x1Fu) << 17) | ((static_cast<std::uint32_t>(code) & 0x03u) << 22);
}
constexpr Time unpack_time(std::uint32_t v) noexcept {
  return Time{static_cast<std::uint8_t>((v >> 17) & 0x1Fu), static_cast<std::uint8_t>((v >> 11) & 0x3Fu), static_cast<std::uint8_t>((v >> 5) & 0x3Fu), static_cast<std::uint8_t>(v & 0x1Fu)};
}
constexpr std::uint8_t unpack_code(std::uint32_t v) noexcept { return static_cast<std::uint8_t>((v >> 22) & 0x03u); }

// The SysEx body of a full frame between F0 and F7: `7F 7F 01 01 hh mm ss ff`, hh = rate code << 5 | hours. Writes kFullFrameBodyBytes bytes (8).
inline constexpr std::uint32_t kFullFrameBodyBytes = 8;
inline constexpr std::uint32_t kFullFrameMessageBytes = kFullFrameBodyBytes + 2;  // with F0 and F7
constexpr void full_frame_body(std::uint32_t packed, std::uint8_t* out) noexcept {
  const Time t = unpack_time(packed);
  out[0] = 0x7F;
  out[1] = 0x7F;
  out[2] = 0x01;
  out[3] = 0x01;
  out[4] = static_cast<std::uint8_t>((unpack_code(packed) << 5) | (t.h & 0x1Fu));
  out[5] = t.m;
  out[6] = t.s;
  out[7] = t.f;
}

// The time of quarter frame `q` after the origin, as Q32 samples: q x R / (4 fps), floor, exact at 44,100 Hz. `q` below 2^26 and an `fps` that is valid (0 for an
// invalid one): the result is below 2^32 samples (27 hours).
constexpr core::Q32 qf_time_q32(std::uint32_t q, std::uint32_t fps, core::CyclesPerSample r = core::kRefCyclesPerSample) noexcept {
  if (!valid_fps(fps)) {
    return 0;
  }
  return core::q32_div(std::uint64_t{q} * core::kCpuHz * r.den, std::uint64_t{4u} * fps * r.num);
}

// The number of the first quarter frame due at or after `samples` whole samples after the origin (the inverse of qf_time_q32 on whole samples).
constexpr std::uint32_t qf_at_or_after(std::uint64_t samples, std::uint32_t fps, core::CyclesPerSample r = core::kRefCyclesPerSample) noexcept {
  if (!valid_fps(fps)) {
    return 0;
  }
  const std::uint64_t num = samples * 4u * fps * r.num;       // quarter frames x (kCpuHz x den)
  const std::uint64_t den = std::uint64_t{core::kCpuHz} * r.den;
  return static_cast<std::uint32_t>((num + den - 1u) / den);
}

// The frame (floor) that `samples` whole samples after the origin fall in.
constexpr std::uint64_t frame_at(std::uint64_t samples, std::uint32_t fps, core::CyclesPerSample r = core::kRefCyclesPerSample) noexcept {
  if (!valid_fps(fps)) {
    return 0;
  }
  return samples * fps * r.num / (std::uint64_t{core::kCpuHz} * r.den);
}

// Where a run's time code stands: the events of each block. See the file header.
class Generator {
 public:
  // The most events step() hands back in one call (one quarter frame and the pending full frame).
  static constexpr std::uint32_t kMaxEvents = 2;
  // The first quarter frame of a run is the first boundary at least this long after its full frame (DIN has sent the full frame's ten bytes, 3.2 ms, by then).
  static constexpr std::uint32_t kFirstQfDelayMs = 4;

  // `fps` 0 (Off) or 24, 25, 30; `hour` 0..23 (MTC START). Anything else is Off / 0. Takes effect at the next anchor(); a change while running is the caller's to
  // follow with reset() or anchor().
  void configure(std::uint32_t fps, std::uint32_t hour) noexcept {
    fps_ = valid_fps(fps) ? static_cast<std::uint8_t>(fps) : std::uint8_t{0};
    hour_ = hour > kMaxHour ? kMaxHour : static_cast<std::uint8_t>(hour);
  }
  bool enabled() const noexcept { return fps_ != 0u; }
  std::uint32_t fps() const noexcept { return fps_; }
  std::uint32_t hour() const noexcept { return hour_; }

  // Nothing is sent until the next anchor(): no wrong time is ever sent.
  void reset() noexcept {
    anchored_ = false;
    full_pending_ = false;
  }
  bool anchored() const noexcept { return anchored_; }

  // The song time `song_samples` (whole samples from the song's first step, MTC START not included; negative before it: a recording's count-in) is at the timeline
  // sample `at`: a full frame is due at `at` and the quarter frames follow from the first boundary at least kFirstQfDelayMs later. No time below 00:00:00:00 is sent: a
  // time before it (MTC START 00 and a count-in) moves the anchor to where the code reaches 00:00:00:00. Re-anchoring (a relocation) replaces everything. Off: does nothing.
  void anchor(std::uint64_t at, std::int64_t song_samples) noexcept;

  // The events of the block [block_start, block_start + n): the pending full frame and the quarter frame that falls in it (each with `offset` in the block, the
  // exact sample in `value` as MidiClock). Returns the count (0..kMaxEvents). Nothing before an anchor.
  std::uint32_t step(std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out) noexcept;

  // The run stopped at the timeline sample `at`: one full frame with the stop time (Logic's playhead goes there), then silence until the next anchor. False when
  // there is nothing to say (off, never anchored, or a full frame is still pending).
  bool stop(std::uint64_t at, audio::SeqEvent& out) noexcept;

  // The time code at the timeline sample `at` (a whole sample at or after the anchor), MTC START included, as the packed value of pack_full().
  std::uint32_t packed_at(std::uint64_t at) const noexcept;

  // The number of the quarter frame the generator will send next (counted from 00:00:00:00, MTC START included, within a day or a little over) and its due sample: for the tests and
  // the console.
  std::uint32_t next_q() const noexcept { return q_next_; }
  std::uint64_t next_due_sample() const noexcept { return due_whole_; }
  std::uint32_t next_due_frac() const noexcept { return due_frac_; }

 private:
  void schedule() noexcept;  // computes due_whole_ / due_frac_ of q_next_ from the base

  std::uint8_t fps_ = 0;
  std::uint8_t hour_ = 1;
  std::uint64_t hour_samples() const noexcept;  // MTC START in samples
  bool anchored_ = false;
  bool full_pending_ = false;
  std::uint64_t anchor_at_ = 0;       // the timeline sample of the anchor
  std::uint64_t anchor_abs_ = 0;      // the time there from 00:00:00:00 (MTC START included), whole samples, within a day
  std::uint32_t q_next_ = 0;          // the next quarter frame to send, counted from 00:00:00:00 (MTC START included; it may run past a day: the byte wraps)
  // The base of the due times: the quarter frame `base_q_` is due at base_whole_ + base_frac_ / 2^32 samples; q_next_ is due at that plus qf_time_q32(q_next_ - base_q_).
  std::uint32_t base_q_ = 0;
  std::uint64_t base_whole_ = 0;
  std::uint32_t base_frac_ = 0;
  std::uint64_t due_whole_ = 0;
  std::uint32_t due_frac_ = 0;
  std::uint64_t full_at_ = 0;
};

}  // namespace o3os::midi::mtc
