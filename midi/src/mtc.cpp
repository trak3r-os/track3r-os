// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI Time Code out (midi/mtc.h): the generator of a run's quarter frames and full frames.
#include <o3os/midi/mtc.h>

namespace o3os::midi::mtc {

namespace {
// A base is moved forward once the quarter-frame count since it reaches this, so that the Q32 offset (and its multiplication) stays small however long a run is.
constexpr std::uint32_t kRebaseAfter = 1u << 20;
constexpr std::uint64_t kLow32 = 0xFFFFFFFFull;
}  // namespace

std::uint64_t Generator::hour_samples() const noexcept {
  return std::uint64_t{hour_} * 3600u * core::kCpuHz * core::kRefCyclesPerSample.den / core::kRefCyclesPerSample.num;
}

void Generator::anchor(std::uint64_t at, std::int64_t song_samples) noexcept {
  if (fps_ == 0u) {
    reset();
    return;
  }
  std::int64_t abs = song_samples + static_cast<std::int64_t>(hour_samples());
  if (abs < 0) {
    at += static_cast<std::uint64_t>(-abs);  // no time below 00:00:00:00: the code starts where it reaches it
    abs = 0;
  }
  const std::uint64_t day = std::uint64_t{kSecondsPerDay} * core::kCpuHz * core::kRefCyclesPerSample.den / core::kRefCyclesPerSample.num;
  anchored_ = true;
  full_pending_ = true;
  full_at_ = at;
  anchor_at_ = at;
  anchor_abs_ = static_cast<std::uint64_t>(abs) % day;  // Q32 holds 27 hours: everything below works inside a day
  // The first quarter frame: the first boundary at least kFirstQfDelayMs after the full frame.
  const std::uint64_t first_from = anchor_abs_ + core::samples_in_ms(kFirstQfDelayMs);
  q_next_ = qf_at_or_after(first_from, fps_);
  base_q_ = q_next_;
  const core::Q32 due = qf_time_q32(q_next_, fps_);
  const core::Q32 from = anchor_abs_ << core::kQ32Frac;  // below 2^64: a day is below 2^32 samples
  const core::Q32 lead = due > from ? due - from : 0u;   // under a quarter frame and the delay: a few hundred samples
  base_whole_ = at + (lead >> core::kQ32Frac);
  base_frac_ = static_cast<std::uint32_t>(lead & kLow32);
  schedule();
}

void Generator::schedule() noexcept {
  std::uint32_t delta = q_next_ - base_q_;
  if (delta >= kRebaseAfter) {
    const core::Q32 move = qf_time_q32(delta, fps_);
    const std::uint64_t frac = std::uint64_t{base_frac_} + (move & kLow32);
    base_whole_ += (move >> core::kQ32Frac) + (frac >> core::kQ32Frac);
    base_frac_ = static_cast<std::uint32_t>(frac & kLow32);
    base_q_ = q_next_;
    delta = 0;
  }
  const core::Q32 off = qf_time_q32(delta, fps_);
  const std::uint64_t frac = std::uint64_t{base_frac_} + (off & kLow32);
  due_whole_ = base_whole_ + (off >> core::kQ32Frac) + (frac >> core::kQ32Frac);
  due_frac_ = static_cast<std::uint32_t>(frac & kLow32);
}

std::uint32_t Generator::packed_at(std::uint64_t at) const noexcept {
  const std::uint64_t elapsed = at > anchor_at_ ? at - anchor_at_ : 0u;
  const std::uint64_t frame = frame_at(anchor_abs_ + elapsed, fps_);
  return pack_full(time_of_frame(frame, fps_), rate_code(fps_));
}

std::uint32_t Generator::step(std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out) noexcept {
  if (!anchored_ || fps_ == 0u || out == nullptr) {
    return 0;
  }
  const std::uint64_t end = block_start + n;
  std::uint32_t count = 0;
  if (full_pending_ && full_at_ < end) {
    audio::SeqEvent e{};
    e.offset = static_cast<std::uint16_t>(full_at_ > block_start ? full_at_ - block_start : 0u);
    e.kind = audio::EventKind::MidiMtcFull;
    e.track = audio::kNoTrack;
    e.value = static_cast<std::int32_t>(packed_at(full_at_));
    out[count++] = e;
    full_pending_ = false;
  }
  while (count < kMaxEvents && !full_pending_) {
    const std::uint64_t nearest = due_whole_ + (due_frac_ >= 0x80000000u ? 1u : 0u);  // the nearest sample, halves up, as every event's `offset`
    if (nearest >= end) {
      break;
    }
    audio::SeqEvent e{};
    if (nearest >= block_start) {
      e.offset = static_cast<std::uint16_t>(nearest - block_start);
      e.value = static_cast<std::int32_t>(due_frac_);  // the time minus its nearest sample, signed, in 2^-32 sample (the low 32 bits of the Q32)
    }                                                    // else: late (a backlog): offset 0, no fraction, as the sequencer does
    e.kind = audio::EventKind::MidiMtcQf;
    e.track = audio::kNoTrack;
    e.a = qf_byte(q_next_, fps_);
    out[count++] = e;
    ++q_next_;
    schedule();
  }
  return count;
}

bool Generator::stop(std::uint64_t at, audio::SeqEvent& out) noexcept {
  const bool say = anchored_ && fps_ != 0u && !full_pending_;
  const std::uint32_t packed = say ? packed_at(at) : 0u;
  reset();
  if (!say) {
    return false;
  }
  out = audio::SeqEvent{};
  out.kind = audio::EventKind::MidiMtcFull;
  out.track = audio::kNoTrack;
  out.value = static_cast<std::int32_t>(packed);
  return true;
}

}  // namespace o3os::midi::mtc
