// SPDX-License-Identifier: GPL-3.0-or-later
// The master scratch stage (OS-DJ C1): see master_scratch.h.
#include <o3os/audio/master_scratch.h>

#include <cstring>

namespace o3os::audio {

namespace {

// The ring is held at HALF scale (+-2.0 of the mix fits in 16 bits): the mix is float to the limiter and a hot sum would be clipped by the ring while the stage is armed. The read doubles it again.
inline std::int16_t to_q15_half(float x) noexcept {
  float v = x * (0.5f * 32767.0f);
  v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
  return static_cast<std::int16_t>(static_cast<int>(v + (v >= 0.0f ? 0.5f : -0.5f)));
}

// OS-SCRATCH-IDLE2: the ring write is the stage's biggest cost at a standstill (128 uncached SDRAM stores a block). to_q15_half's float compares are skipped where its clamp is the identity: |x| <= 2.0
// (bits <= 0x40000000; NaN and inf are above) gives |x * 16383.5| <= 32767, so the result is trunc(v + copysign(0.5, v)) with the sign taken from the input's bit (for v = -0.0 the "+0.5" of the compare and
// the "-0.5" of the sign both truncate to 0). Every other input takes to_q15_half itself: the same int16 for every float, NaN included.
[[gnu::noinline]] std::int32_t q15_half_slow(float x) noexcept { return to_q15_half(x); }  // (out of line: the loop below stays small)

inline std::uint32_t bits_of(float x) noexcept {
  std::uint32_t b;
  std::memcpy(&b, &x, sizeof b);
  return b;
}

// For |x| <= 2.0 (`b` = bits_of(x)).
inline std::int32_t q15_half_fast(float x, std::uint32_t b) noexcept {
  const std::uint32_t hb = 0x3f000000u | (b & 0x80000000u);  // +-0.5
  float h;
  std::memcpy(&h, &hb, sizeof h);
  return static_cast<std::int32_t>(x * (0.5f * 32767.0f) + h);
}

// `n` frames into the ring at `p` (interleaved L R; the run does not wrap). Wide: both halves in one word store (the ring is 4-byte aligned; little endian: the same two halfwords), half the SDRAM accesses.
template <bool Wide>
inline void write_run(std::int16_t* p, const float* left, const float* right, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i, p += 2) {
    const float xl = left[i];
    const float xr = right[i];
    const std::uint32_t bl = bits_of(xl);
    const std::uint32_t br = bits_of(xr);
    std::int32_t l;
    std::int32_t r;
    if (((bl | br) << 1) > 0x80000000u) {  // (the OR of two patterns <= 0x40000000 is above it only when one is exactly 2.0: that frame takes the slow path too, which is exact for every input)
      l = q15_half_slow(xl);
      r = q15_half_slow(xr);
    } else {
      l = q15_half_fast(xl, bl);
      r = q15_half_fast(xr, br);
    }
    if (Wide) {
      const std::uint32_t w = (static_cast<std::uint32_t>(l) & 0xFFFFu) | (static_cast<std::uint32_t>(r) << 16);
      std::memcpy(p, &w, sizeof w);
    } else {
      p[0] = static_cast<std::int16_t>(l);
      p[1] = static_cast<std::int16_t>(r);
    }
  }
}

}  // namespace

bool MasterScratch::stage(std::int16_t* ring, std::uint32_t frames) noexcept {
  if (ring == nullptr || busy() || frames < kMinFrames) {
    return false;
  }
  ring_ = ring;
  cap_ = frames < kMaxFrames ? frames : kMaxFrames;
  staged_.fetch_add(1u, std::memory_order_release);
  return true;
}

void MasterScratch::unstage() noexcept { done_.store(staged_.load(std::memory_order_acquire), std::memory_order_release); }

bool MasterScratch::set_mode(float mode, std::uint32_t ramp_samples) noexcept {
  if (mode == 0.0f) {
    if (phase_ != Phase::Off) {
      exit_req_ = true;
    }
    return true;
  }
  if (mode == 3.0f) {  // OS-DJSTOPLEN: a new length for the TAPE STOP in force. The brake is rebased so the rate stays where it is and falls to 0 over the new length from there (a few flops, no ring access).
    if (phase_ != Phase::Tape || ramp_samples == 0u) {
      return false;
    }
    const float r = rate_ < 0.0f ? 0.0f : (rate_ > 1.0f ? 1.0f : rate_);
    len_ = ramp_samples;
    t_ = static_cast<std::uint32_t>((1.0f - r) * static_cast<float>(len_));
    return true;
  }
  if ((mode != 1.0f && mode != 2.0f) || phase_ != Phase::Off) {
    return false;
  }
  const std::uint32_t staged = staged_.load(std::memory_order_acquire);
  if (staged == done_.load(std::memory_order_acquire) || ring_ == nullptr) {
    return false;  // no ring staged: nothing to take
  }
  epoch_ = staged;
  phase_ = mode == 1.0f ? Phase::Platter : Phase::Tape;
  exit_req_ = false;
  widx_ = 0;
  written_ = 0;
  lag_i_ = 0;
  lag_f_ = 0.0f;
  rate_ = 1.0f;  // the record was playing when the hand came down
  t_ = 0;
  len_ = ramp_samples != 0u ? ramp_samples : 1u;
  back_age_ = 255;
  fade_ = 0;
  hermite_ = mode == 1.0f;  // OS-SCRATCH: only the Platter reads with the Hermite; the TAPE STOP's output is the C1 linear one
  aa_l_ = 0.0f;             // OS-BUG-SCRATCH: the anti-alias low-pass starts at rest (it follows the read while bypassed)
  aa_r_ = 0.0f;
  blk_ = 0;
  n_ev_ = 0;
  dir_ = 0;
  debt_ = 0.0f;
  hand_rate_ = 0.0f;
  last_count_ =count_ != nullptr ? count_() : std::uint16_t{0};
  if (bypass_ != nullptr) {
    *bypass_ = false;
  }
  return true;
}

void MasterScratch::finish() noexcept {
  phase_ = Phase::Off;
  exit_req_ = false;
  if (bypass_ != nullptr) {
    *bypass_ = true;
  }
  done_.store(epoch_, std::memory_order_release);  // the last access to the ring was before this store
}

void MasterScratch::abort() noexcept {
  if (phase_ != Phase::Off) {
    finish();
  }
}

void MasterScratch::begin_release() noexcept {
  phase_ = Phase::Release;
  fade_ = 0;
}

void MasterScratch::write_block(const float* left, const float* right) noexcept {
  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "write_run packs L in the low halfword");
  const bool wide = (reinterpret_cast<std::uintptr_t>(ring_) & 3u) == 0u;
  std::uint32_t idx = widx_;
  std::uint32_t done = 0;
  while (done < kBlock) {  // at most two runs: the ring's end splits the block once
    const std::uint32_t run = kBlock - done < cap_ - idx ? kBlock - done : cap_ - idx;
    if (wide) {
      write_run<true>(ring_ + 2u * idx, left + done, right + done, run);
    } else {
      write_run<false>(ring_ + 2u * idx, left + done, right + done, run);
    }
    done += run;
    idx += run;
    if (idx == cap_) {
      idx = 0;
    }
  }
}

// OS-BUG-SCRATCH: main context. Out-of-range values (and NaN) are clamped; the inertia is stored as the per-block one-pole coefficient (no libm). Each store is one relaxed atomic.
void MasterScratch::set_tuning(float inertia_ms, float ahead, float gate_slope, bool aa) noexcept {
  const auto clamp = [](float v, float lo, float hi) { return v >= lo ? (v <= hi ? v : hi) : lo; };  // NaN fails the first compare: lo
  inertia_.store(inertia_for_ms(clamp(inertia_ms, 5.0f, 500.0f)), std::memory_order_relaxed);
  ahead_.store(clamp(ahead, 0.0f, 4.0f), std::memory_order_relaxed);
  gate_.store(clamp(gate_slope, 1.0f, 64.0f), std::memory_order_relaxed);
  aa_.store(aa, std::memory_order_relaxed);
}

// OS-SCRATCH: the hand's rate for the coming block, from the TIME between count events (in blocks), not the counts of one block. `hand` is this block's count difference (clamped). Render only.
float MasterScratch::platter_target(int hand) noexcept {
  ++blk_;
  if (hand != 0) {
    const std::int8_t s = hand > 0 ? 1 : -1;
    if (s != dir_ || blk_ - ev_blk_[0] > kStopBlocks) {
      n_ev_ = 0;  // a reversal, or a restart after a rest: the old events say nothing about this motion
    }
    if (s != dir_) {
      debt_ = 0.0f;  // a reversal: the debt of the old direction (up to kDebtMax after a clamped flick) is not paid back the other way, the head follows the new direction at once
    }
    for (unsigned i = kHist - 1u; i > 0u; --i) {
      ev_blk_[i] = ev_blk_[i - 1u];
      ev_n_[i] = ev_n_[i - 1u];
    }
    ev_blk_[0] = blk_;
    ev_n_[0] = static_cast<std::int16_t>(hand);
    n_ev_ = n_ev_ < kHist ? n_ev_ + 1u : kHist;
    dir_ = s;
    debt_ += static_cast<float>(hand) * kFramesPerCount;
  } else if (n_ev_ != 0u && blk_ - ev_blk_[0] > kStopBlocks) {
    n_ev_ = 0;
  }
  float v = 0.0f;  // counts a block
  if (n_ev_ == 1u) {
    v = static_cast<float>(ev_n_[0]) * (1.0f / kFirstSpan);
  } else if (n_ev_ > 1u) {
    int sum = 0;
    for (unsigned i = 0; i + 1u < n_ev_; ++i) {
      sum += ev_n_[i];
    }
    v = static_cast<float>(sum) / static_cast<float>(ev_blk_[0] - ev_blk_[n_ev_ - 1u]);
  }
  const std::uint32_t age = n_ev_ != 0u ? blk_ - ev_blk_[0] : 0u;
  if (age > 1u) {  // no count for `age` blocks: the hand is slower than one count in kHold times that (OS-BUG-SCRATCH: a steady hand's estimate holds across the gap between two counts)
    const float cap = kHold / static_cast<float>(age);
    v = v > cap ? cap : (v < -cap ? -cap : v);
  }
  const float dirf = static_cast<float>(dir_);
  const float owed = debt_ * dirf;  // > 0: the hand is ahead of what the head has been given; < 0: the head is ahead of the hand
  float tgt = v * kCountRate;
  hand_rate_ = tgt;
  float mag = tgt * dirf;  // the rate along the hand's direction, >= 0
  if (owed > kFramesPerCount) {  // pay the position back, never beyond max(the hand's own rate, kPullMax)
    const float lim = mag > kPullMax ? mag : kPullMax;
    mag += (owed - kFramesPerCount) * kPull;
    mag = mag > lim ? lim : mag;
  }
  const float reach = ahead_.load(std::memory_order_relaxed);  // main writes (set_tuning), read once a block
  const float ahead = n_ev_ > 1u || reach < kLoneAhead ? reach : kLoneAhead;  // a lone count keeps the nudge reach; a steady hand (two events or more) gets the whole kAhead
  const float room = (owed + ahead * kFramesPerCount) * (1.0f / static_cast<float>(kBlock));  // the rate that spends exactly what the head may still play
  mag = mag > room ? (room > 0.0f ? room : 0.0f) : mag;  // never further ahead of the hand than `ahead` (a lone count does not carry on)
  tgt = mag * dirf;
  debt_ -= tgt * static_cast<float>(kBlock);
  debt_ = debt_ > kDebtMax ? kDebtMax : (debt_ < -kDebtMax ? -kDebtMax : debt_);
  return tgt;
}

// The head's rate at the end of this block (the block ramps linearly from the one before).
float MasterScratch::next_rate() noexcept {
  float r1 = rate_;
  switch (phase_) {
    case Phase::Platter: {
      int hand = 0;
      if (count_ != nullptr) {
        const std::uint16_t now = count_();
        hand = static_cast<std::int16_t>(static_cast<std::uint16_t>(now - last_count_));
        last_count_ = now;
      }
      hand = hand > kMaxHand ? kMaxHand : (hand < -kMaxHand ? -kMaxHand : hand);
      const float tgt = platter_target(hand);
      r1 = rate_ + (tgt - rate_) * inertia_.load(std::memory_order_relaxed);
      if (tgt == 0.0f && r1 < 0.001f && r1 > -0.001f) {
        r1 = 0.0f;
      }
      if (r1 <= -kSpinMin && hand_rate_ <= -kSpinMin) {  // (the hand's own rate too: with the inertia the head's lingers after a hand that has stopped)
        back_age_ = 0;
        back_rate_ = r1;
      } else if (back_age_ < 255u) {
        ++back_age_;
      }
      break;
    }
    case Phase::Tape:
      t_ += kBlock;
      r1 = t_ >= len_ ? 0.0f : 1.0f - static_cast<float>(t_) / static_cast<float>(len_);
      break;
    case Phase::Spin:
      t_ += kBlock;
      r1 = t_ >= kSpinSamples ? 0.0f : spin_rate_ * (1.0f - static_cast<float>(t_) / static_cast<float>(kSpinSamples));
      break;
    default:
      break;  // Release: the rate is held
  }
  return r1;
}

// OS-SCRATCH-IDLE: the lag after one block at rate 0 (+1 frame a frame), bit for bit what the per-sample loop of process() leaves. Frame 0 goes through advance_lag (it rounds `lag_f + 1` once, onto the
// 2^-23 grid); from then on `lag_f + 1` is exact and n = 1, so only the whole part moves, held by the lag limit (which also zeroes the fraction) exactly as the loop does at the top of each frame.
// OS-SCRATCH-IDLE2: the 63 frames after frame 0 in closed form. The loop is L <- min(L, m_i) + 1 for i = 1..63, with m_i = read_limit(before + i + 1): non-decreasing, +1 at most a frame, so m_i - i never rises.
// Hence (a) the loop clamps at some frame iff it clamps at the last one (i = 63: L1 + 62 >= m_63, L1 the lag after frame 0, which has gone up by i - 1 by frame i if nothing clamped before), and (b) the lag after
// the block is min(L1 + 63, m_63 + 1): every other term m_i + 64 - i is at least m_63 + 1. The fraction is zeroed exactly when a clamp happened.
void MasterScratch::still_lag(std::uint32_t cap, std::uint32_t before, std::int32_t ring_lim, std::int32_t& lag_i, float& lag_f) noexcept {
  std::int32_t li = lag_i;
  float lf = lag_f;
  const std::int32_t lag_max = read_limit_for(cap, before + 1u, ring_lim);
  if (li >= lag_max) {
    li = lag_max;
    lf = 0.0f;
  }
  advance_lag(li, lf, 1.0f);
  const std::int32_t last = read_limit_for(cap, before + kBlock, ring_lim);
  if (li + static_cast<std::int32_t>(kBlock - 2u) >= last) {
    li = last + 1;
    lf = 0.0f;
  } else {
    li += static_cast<std::int32_t>(kBlock - 1u);
  }
  lag_i = li;
  lag_f = lf;
}

void MasterScratch::process(float* left, float* right, std::uint32_t n) noexcept {
  if (n != kBlock || phase_ == Phase::Off || ring_ == nullptr) {
    return;
  }
  if (exit_req_) {
    exit_req_ = false;
    if (phase_ == Phase::Platter && back_age_ <= kSpinWindow) {
      phase_ = Phase::Spin;
      spin_rate_ = back_rate_;
      t_ = 0;
    } else if (phase_ == Phase::Platter || phase_ == Phase::Tape) {
      begin_release();
    }
  }
  const std::uint32_t idx0 = widx_;
  const std::uint32_t before = written_;
  write_block(left, right);
  widx_ = (idx0 + kBlock) % cap_;
  written_ = before + kBlock < cap_ ? before + kBlock : cap_;

  const float r0 = rate_;
  const float r1 = next_rate();
  const float dr = (r1 - r0) / static_cast<float>(kBlock);
  float rr = r0 + 0.5f * dr;
  std::int32_t lag_i = lag_i_;
  float lag_f = lag_f_;
  const std::int32_t ring_lim = static_cast<std::int32_t>(cap_ - kBlock - 2u);  // the block is written whole before it is read: the oldest kBlock frames of a full ring are already this block's
  const bool releasing = phase_ == Phase::Release;
  // OS-BUG-SCRATCH: the tuning, read once a block. The gate's slope is the tuned one only for the SCR hand itself (Platter phase, Hermite); Spin, Release and the TAPE STOP keep the C1 one. The anti-alias low-pass runs only on a Hermite read at
  // |rate| > 1 (coefficient min(1, kAaCut / the block's larger end rate)); otherwise it is bypassed and its state follows the read, so the output is the unfiltered one bit for bit and the filter engages without a step.
  const float slope = (hermite_ && phase_ == Phase::Platter) ? gate_.load(std::memory_order_relaxed) : kGateSlope;
  const float rmax = (r0 < 0.0f ? -r0 : r0) > (r1 < 0.0f ? -r1 : r1) ? (r0 < 0.0f ? -r0 : r0) : (r1 < 0.0f ? -r1 : r1);
  const float aa_a = rmax > 1.0f ? (kAaCut < rmax ? kAaCut / rmax : 1.0f) : 1.0f;
  const bool aa_on = hermite_ && aa_a < 1.0f && aa_.load(std::memory_order_relaxed);
  float aa_l = aa_l_;
  float aa_r = aa_r_;
  // OS-SCRATCH-IDLE: a head at a standstill (the rate was 0 and stays 0, no release fade) has the gate g = 0 for all 64 frames: the output is exact silence and only the lag moves (1 frame a frame). The
  // per-sample loop below would compute exactly that, at about 90 cycles a frame; this does the same lag arithmetic without the read, the gate and the mix. (Only a non-finite input differs: it would stay NaN.)
  const bool still = !releasing && r0 == 0.0f && r1 == 0.0f;
  if (still) {
    for (std::uint32_t i = 0; i < kBlock; ++i) {
      left[i] = 0.0f;
      right[i] = 0.0f;
    }
    still_lag(cap_, before, ring_lim, lag_i, lag_f);
    aa_l = 0.0f;  // the read is 0 at a standstill: the filter state follows it
    aa_r = 0.0f;
  }
  for (std::uint32_t i = 0, n_play = still ? 0u : kBlock; i < n_play; ++i, rr += dr) {
    const std::int32_t lag_max = read_limit(before + i + 1u, ring_lim);
    if (lag_i >= lag_max) {
      lag_i = lag_max;
      lag_f = 0.0f;
    }
    float w = 1.0f;
    if (releasing) {
      const std::uint32_t f = fade_ + i;
      w = f >= kReleaseSamples ? 0.0f : 1.0f - static_cast<float>(f) / static_cast<float>(kReleaseSamples);
    }
    const float mag = rr < 0.0f ? -rr : rr;
    const float g = mag * slope < 1.0f ? mag * slope : 1.0f;
    float hl = 0.0f;
    float hr = 0.0f;
    if (g > 0.0f && w > 0.0f) {
      const std::uint32_t ib = static_cast<std::uint32_t>(lag_i);
      std::uint32_t j = idx0 + i + cap_ - ib;
      j = j >= cap_ ? j - cap_ : j;
      j = j >= cap_ ? j - cap_ : j;
      const std::uint32_t k = j == 0u ? cap_ - 1u : j - 1u;
      if (hermite_) {
        // OS-SCRATCH: 4-point Hermite (Catmull-Rom) between the frames at lag_i (t = 0) and lag_i + 1 (t = 1); one newer tap and one older. The newer tap is the head's own frame when that is the last
        // one written (lag 0 on the block's last frame); the older one is always written (the lag limit leaves it, and at the limit lag_f is 0, which reads the frame itself).
        const std::uint32_t m = (lag_i == 0 && i == kBlock - 1u) ? j : (j + 1u == cap_ ? 0u : j + 1u);
        const std::uint32_t o = k == 0u ? cap_ - 1u : k - 1u;
        const float s = g * (2.0f / 32767.0f);
        hl = hermite4(static_cast<float>(ring_[2u * m]), static_cast<float>(ring_[2u * j]), static_cast<float>(ring_[2u * k]), static_cast<float>(ring_[2u * o]), lag_f) * s;
        hr = hermite4(static_cast<float>(ring_[2u * m + 1u]), static_cast<float>(ring_[2u * j + 1u]), static_cast<float>(ring_[2u * k + 1u]), static_cast<float>(ring_[2u * o + 1u]), lag_f) * s;
      } else {
        const float a = (1.0f - lag_f) * g * (2.0f / 32767.0f);
        const float b = lag_f * g * (2.0f / 32767.0f);
        hl = static_cast<float>(ring_[2u * j]) * a + static_cast<float>(ring_[2u * k]) * b;
        hr = static_cast<float>(ring_[2u * j + 1u]) * a + static_cast<float>(ring_[2u * k + 1u]) * b;
      }
    }
    if (aa_on) {
      aa_l += aa_a * (hl - aa_l);
      aa_r += aa_a * (hr - aa_r);
      hl = aa_l;
      hr = aa_r;
    } else {
      aa_l = hl;
      aa_r = hr;
    }
    left[i] = left[i] * (1.0f - w) + hl * w;
    right[i] = right[i] * (1.0f - w) + hr * w;
    advance_lag(lag_i, lag_f, 1.0f - rr);
  }
  const std::int32_t lag_cap = read_limit(written_, ring_lim);  // (the last sample's update may have overshot: the next block clamps too, but the lag is never read stale)
  if (lag_i >= lag_cap) {
    lag_i = lag_cap;
    lag_f = 0.0f;
  }
  lag_i_ = lag_i;
  lag_f_ = lag_f;
  aa_l_ = aa_l;
  aa_r_ = aa_r;
  rate_ = r1;
  if (releasing) {
    fade_ += kBlock;
    if (fade_ >= kReleaseSamples) {
      finish();
    }
  } else if (phase_ == Phase::Spin && t_ >= kSpinSamples) {
    begin_release();
  }
}

}  // namespace o3os::audio
