// SPDX-License-Identifier: GPL-3.0-or-later
//
// The clock-follow loop (midi/clock_dll.h). State is integer (Q32 samples), the gains are single-precision float.
#include <o3os/midi/clock_dll.h>

namespace o3os::midi {

namespace {

constexpr float kTwoPi = 6.28318531f;
constexpr float kSqrt2 = 1.41421356f;
constexpr float kQ = 4294967296.0f;
constexpr float kInvQ = 1.0f / 4294967296.0f;
constexpr float kOmegaMax = 0.5f;  // above this the discrete loop rings: the acquire bandwidth is lowered at slow tempos instead
constexpr float kSampleRate = static_cast<float>(core::kCpuHz) * static_cast<float>(core::kRefCyclesPerSample.den) /
                              static_cast<float>(core::kRefCyclesPerSample.num);

// The period of a clock (8 ticks of the grid) at this OS's tempo limits: 800 BPM and 40 BPM.
constexpr core::SQ32 kPeriodMin = static_cast<core::SQ32>(8u * core::samples_per_tick_q32(80000u));
constexpr core::SQ32 kPeriodMax = static_cast<core::SQ32>(8u * core::samples_per_tick_q32(4000u));
constexpr core::SQ32 kRebaseAt = core::SQ32{1} << 62;

core::SQ32 abs64(core::SQ32 v) noexcept { return v < 0 ? -v : v; }

// floor(a / b) for b > 0.
core::SQ32 floor_div(core::SQ32 a, core::SQ32 b) noexcept {
  core::SQ32 q = a / b;
  if ((a % b) != 0 && a < 0) {
    --q;
  }
  return q;
}

// A Q32 value as a float, through its two 32-bit halves (an int64 -> float conversion would call __aeabi_l2f): the high word plus the sign
// carry of the low one, the low word read signed, so that v = hi * 2^32 + lo holds exactly and a small negative v keeps its low word small.
constexpr float q32_to_float(core::SQ32 v) noexcept {
  const std::uint64_t u = static_cast<std::uint64_t>(v);
  const std::int32_t hi = static_cast<std::int32_t>(static_cast<std::uint32_t>(u >> 32)) + static_cast<std::int32_t>(static_cast<std::uint32_t>(u) >> 31);
  const std::int32_t lo = static_cast<std::int32_t>(static_cast<std::uint32_t>(u));
  return static_cast<float>(hi) * kQ + static_cast<float>(lo);
}

using detail::float_to_q;  // in the header, testable (midi_clock_dll_test.cpp checks it at its bound)

// The floor of the widen and re-acquire bands: 4 ms, above the ~3 ms a +-2 ms jittered clock's error reaches at any tempo, so jitter alone never
// widens or re-acquires the loop. The re-acquire band is reacquire_pct of a period AND this floor: at a fast tempo the percent alone falls below
// the jitter (at 300 BPM, 20 % of 8.33 ms is 1.67 ms), and a jittered master would then re-acquire every few clocks, each re-acquire re-anchoring
// the phase to one raw jittered stamp instead of slewing (OS-MIDISYNC M4).
constexpr core::SQ32 kWidenMin = float_to_q(0.004f * kSampleRate);

}  // namespace

void ClockDll::reset(std::uint64_t origin, core::Q32 nominal_period) noexcept {
  const core::SQ32 nominal = static_cast<core::SQ32>(nominal_period);
  origin_ = origin;
  nominal_ = nominal < kPeriodMin ? kPeriodMin : (nominal > kPeriodMax ? kPeriodMax : nominal);
  period_ = nominal_;
  state_ = State::Idle;
  t0_ = t1_ = last_obs_ = 0;
  tick_ = 0;
  since_acquire_ = 0;
  bad_run_ = 0;
  err_ = 0;
  err_n_ = 0;
  err_pos_ = 0;
  drop_score_ = 0;
  drop_extra_ = 0;
  run_extra_ = 0;
  run_n_ = 0;
  slow_hold_ = false;
  err_run_ = 0;
  wide_clocks_ = 0;
  for (core::SQ32& e : err_hist_) {
    e = 0;
  }
}

void ClockDll::clamp_period() noexcept {
  if (period_ < kPeriodMin) {
    period_ = kPeriodMin;
  } else if (period_ > kPeriodMax) {
    period_ = kPeriodMax;
  }
}

void ClockDll::rebase_if_needed() noexcept {
  if (abs64(t0_) < kRebaseAt) {
    return;
  }
  const core::SQ32 shift = (t0_ >> core::kQ32Frac) << core::kQ32Frac;  // whole samples, floor
  origin_ += static_cast<std::uint64_t>(shift >> core::kQ32Frac);
  t0_ -= shift;
  t1_ -= shift;
  last_obs_ -= shift;
}

float ClockDll::omega() const noexcept {
  const bool wide = since_acquire_ < cfg_.acquire_clocks || wide_clocks_ > 0u;  // a sustained error widens the loop, bounded
  const std::uint32_t mhz = wide ? cfg_.acquire_mhz : cfg_.track_mhz;
  const float period_s = q32_to_float(period_) * kInvQ / kSampleRate;  // seconds per clock
  float w = kTwoPi * (static_cast<float>(mhz) * 0.001f) * period_s;
  if (w > kOmegaMax) {
    w = kOmegaMax;
  }
  return w;
}

// A new first clock: the fit starts again from it, the period is whatever the loop had.
void ClockDll::start_fit(core::SQ32 t) noexcept {
  fit_t0_ = t;
  fit_u_[0] = 0;
  fit_n_ = 1;
  t0_ = t;
  last_obs_ = t;
  t1_ = t + period_;
  since_acquire_ = 0;
  drop_score_ = 0;
  drop_extra_ = 0;
  run_extra_ = 0;
  run_n_ = 0;
  err_run_ = 0;
  wide_clocks_ = 0;
  state_ = State::Started;
}

// One of the first clocks. Its time joins the fit; after `fit_clocks` of them the straight line through the points gives the period and the phase.
ClockDll::Result ClockDll::fit_clock(core::SQ32 t) noexcept {
  const core::SQ32 u = t - fit_t0_;
  const core::SQ32 gap = t - last_obs_;
  if (gap < kPeriodMin / 2) {
    ++stats_.ignored;
    return Result::Ignored;  // too soon to be the next clock of any tempo we follow
  }
  // The clocks of a fit are evenly spaced: a gap far from the one before (or, for the second clock, from any tempo we follow) starts the fit again from this clock.
  const core::SQ32 expect = fit_n_ >= 2u ? fit_u_[fit_n_ - 1u] / static_cast<core::SQ32>(fit_n_ - 1u) : 0;
  const bool odd = fit_n_ >= 2u ? (abs64(gap - expect) * 100 > expect * static_cast<core::SQ32>(cfg_.reacquire_pct)) : (gap > kPeriodMax + kPeriodMax / 2);
  if (odd) {
    ++tick_;  // the count goes on: this clock is the next one, as far as we can tell
    start_fit(t);
    return Result::Started;
  }
  const std::uint32_t want = cfg_.fit_clocks < 2u ? 2u : (cfg_.fit_clocks > 16u ? 16u : cfg_.fit_clocks);
  fit_u_[fit_n_++] = u;
  slow_hold_ = false;  // two clocks fitted: the period is the master's again (settled_tick())
  ++tick_;
  ++stats_.clocks;
  last_obs_ = t;
  if (fit_n_ < want) {
    // Provisional: the average interval so far.
    period_ = u / static_cast<core::SQ32>(fit_n_ - 1u);
    clamp_period();
    t0_ = t;
    t1_ = t + period_;
    return Result::Acquiring;
  }
  // Least squares through (i, u_i), i = 0 .. n - 1: slope = (n sum(i u) - sum(i) sum(u)) / (n sum(i^2) - sum(i)^2), exact in integers (u below 2^48, n <= 16).
  const core::SQ32 n = static_cast<core::SQ32>(fit_n_);
  core::SQ32 si = 0, su = 0, siu = 0, sii = 0;
  for (core::SQ32 i = 0; i < n; ++i) {
    si += i;
    su += fit_u_[i];
    siu += i * fit_u_[i];
    sii += i * i;
  }
  const core::SQ32 den = n * sii - si * si;
  const core::SQ32 slope = (n * siu - si * su) / den;
  const core::SQ32 icpt = (su - slope * si) / n;
  period_ = slope;
  clamp_period();
  t0_ = fit_t0_ + icpt + period_ * (n - 1);  // the line's value at the last clock: the filtered time of this one
  t1_ = t0_ + period_;
  since_acquire_ = 0;
  bad_run_ = 0;
  err_n_ = 0;
  err_pos_ = 0;
  state_ = State::Running;
  return Result::Acquiring;
}

ClockDll::Result ClockDll::on_clock(core::SQ32 t) noexcept {
  switch (state_) {
    case State::Idle:
      period_ = nominal_;
      tick_ = 0;
      bad_run_ = 0;
      start_fit(t);
      ++stats_.clocks;
      return Result::Started;

    case State::Started:
      return fit_clock(t);

    case State::Running:
      break;
  }

  // Running. Which clock is this? The one the loop predicted, or one of the next few if some were missing; and not at all if it is far too early.
  const core::SQ32 d = t - t1_;
  core::SQ32 kk = 1 + floor_div(d + period_ / 2, period_);
  if (kk >= 2) {
    // Clocks are missing only if the ones that came are evenly spaced at about the period we know. Otherwise the tempo has changed (a slower master looks like
    // a dropout of the clocks that have not come): this is the next clock, and the error shows it.
    const core::SQ32 implied = (t - last_obs_) / kk;
    if (abs64(implied - period_) * 100 > period_ * static_cast<core::SQ32>(cfg_.reacquire_pct)) {
      kk = 1;
    }
  }
  if (kk < 1) {
    // Early by more than half a period. One such clock is a glitch; two in a row are a tempo that went up, and this one is the next clock.
    ++stats_.ignored;
    if (++bad_run_ < 2u) {
      return Result::Ignored;
    }
    kk = 1;
  }
  const core::SQ32 drops = kk - 1;
  if (drops > static_cast<core::SQ32>(cfg_.max_dropout)) {
    // A long silence: this clock is a new first one (the master may have changed tempo), the count of ticks goes on.
    tick_ += static_cast<std::uint64_t>(kk);
    bad_run_ = 0;
    start_fit(t);
    ++stats_.reacquires;
    stats_.missing += static_cast<std::uint32_t>(drops);
    return Result::Reacquired;
  }
  // A master that has slowed looks like one that drops clocks, clock after clock: a sustained density of "missing" clocks (the leaky score) re-acquires
  // as well, and the extra ticks those dropouts had added are taken back, so the tick stays the master's clock index. OS-MIDISYNC M3: a 180 -> 90 BPM step
  // is exactly this — every clock looked like one missing, the loop saw no error at all and ran on at the old tempo. The level is 800, so four consecutive
  // missing-clock observations (leaky score 848) re-acquire: at the old 1024 (six of them) the tick ran ahead past the next step boundary (a step is 6 clocks)
  // before the take-back, and the pull-back re-crossed that boundary, so the sequencer emitted the step twice (OS-MIDISYNC M5: after the 180 -> 90 jump of
  // battery test 07, 2 steps missing and 2 extra).
  drop_score_ = drop_score_ * 7u / 8u + (drops >= 1 ? 256u : 0u);
  if (drop_score_ >= cfg_.drop_score_max) {
    tick_ += 1u;
    if (drop_extra_ <= tick_) {
      tick_ -= drop_extra_;
    }
    bad_run_ = 0;
    start_fit(t);
    slow_hold_ = true;  // the slowdown the dropouts were: start_fit keeps the old period until the second clock
    ++stats_.reacquires;
    return Result::Reacquired;
  }
  if (drops >= 1) {
    run_extra_ += static_cast<std::uint32_t>(drops);
    ++run_n_;
    drop_extra_ += static_cast<std::uint32_t>(drops);
    if (drop_extra_ < static_cast<std::uint32_t>(drops)) {
      drop_extra_ = 0xFFFFFFFFu;  // saturate
    }
  } else if (drop_score_ < 256u) {
    // The score window holds no drop any more: the drops counted so far were genuine dropouts and the tick count keeps them. Only the drops
    // inside the window are taken back at a re-acquire, so a long dropout run followed by a re-fit cannot subtract hundreds of ticks.
    drop_extra_ = 0;
  }
  if (drops == 0) {
    run_extra_ = 0;
    run_n_ = 0;  // a clean observation: the run of skipped clocks ends, the tick is the master's index
  }
  const core::SQ32 tp = t1_ + drops * period_;  // the prediction for this clock
  const core::SQ32 e = t - tp;

  // A tempo that has jumped shows as an error that stays large: three in a row restart the loop from this clock. The band has the 4 ms floor of
  // the widen band below, so a jittered master at a fast tempo (where reacquire_pct of a period is under the jitter) does not read as a jump.
  if (abs64(e) * 100 > period_ * static_cast<core::SQ32>(cfg_.reacquire_pct) && abs64(e) > kWidenMin) {
    ++bad_run_;
  } else {
    bad_run_ = 0;
  }
  if (bad_run_ >= 3u) {
    tick_ += static_cast<std::uint64_t>(kk);
    if (drop_extra_ != 0u && drop_extra_ <= tick_) {
      tick_ -= drop_extra_;  // the "missing" clocks were the master slowing down, not missing
    }
    bad_run_ = 0;
    start_fit(t);
    ++stats_.reacquires;
    return Result::Reacquired;
  }

  // A sustained error of one sign beyond the jitter band — a tempo step the reacquire threshold above does not reach — widens the loop to the acquire
  // bandwidth for a bounded run of clocks; when the error clears, the widening runs out and the narrow loop is back (the band's floor of 4 ms keeps a
  // master's +-2 ms of jitter out of this at any tempo, so the steady-state rejection does not change).
  if (abs64(e) * 100 > period_ * static_cast<core::SQ32>(cfg_.widen_pct) && abs64(e) > kWidenMin) {
    const std::int32_t s = e > 0 ? 1 : -1;
    err_run_ = (err_run_ != 0 && (err_run_ > 0) == (s > 0)) ? err_run_ + s : s;
    const std::int32_t lim = static_cast<std::int32_t>(cfg_.widen_clocks);
    if (err_run_ > lim) {
      err_run_ = lim;
    } else if (err_run_ < -lim) {
      err_run_ = -lim;
    }
    if (err_run_ >= lim || err_run_ <= -lim) {
      if (wide_clocks_ == 0u) {
        ++stats_.widens;  // a new widening episode: the loop goes to the acquire bandwidth
      }
      wide_clocks_ = cfg_.widen_max;
    }
  } else {
    err_run_ = 0;
  }

  const float w = omega();
  const float b = kSqrt2 * w;
  const float c = w * w;
  const float ef = q32_to_float(e) * kInvQ;  // samples
  t0_ = tp;
  t1_ = tp + float_to_q(b * ef) + period_;
  period_ += float_to_q(c * ef);
  clamp_period();
  tick_ += static_cast<std::uint64_t>(kk);
  last_obs_ = t;
  if (since_acquire_ < 0xFFFFFFFFu) {
    ++since_acquire_;
  }
  if (wide_clocks_ > 0u) {
    --wide_clocks_;
  }
  err_ = e;
  err_hist_[err_pos_] = e;
  err_pos_ = (err_pos_ + 1u) & 7u;
  if (err_n_ < 8u) {
    ++err_n_;
  }
  ++stats_.clocks;
  if (drops != 0) {
    ++stats_.dropouts;
    stats_.missing += static_cast<std::uint32_t>(drops);
  }
  rebase_if_needed();
  return drops != 0 ? Result::Dropout : Result::Tracked;
}

core::SQ32 ClockDll::time_of_tick(std::uint64_t tick) const noexcept {
  if (tick > tick_) {
    return t1_ + static_cast<core::SQ32>(tick - tick_ - 1u) * period_;
  }
  if (tick == tick_) {
    return t0_;
  }
  return t0_ - static_cast<core::SQ32>(tick_ - tick) * period_;
}

std::uint32_t ClockDll::bpm_x100() const noexcept {
  return core::bpm_x100_from_tick_q32(static_cast<core::Q32>(period_) / 8u);
}

float ClockDll::last_error_samples() const noexcept { return q32_to_float(err_) * kInvQ; }

bool ClockDll::locked() const noexcept {
  if (state_ != State::Running || since_acquire_ < cfg_.acquire_clocks || err_n_ < 8u) {
    return false;
  }
  for (const core::SQ32 e : err_hist_) {
    if (abs64(e) * 100 > period_ * static_cast<core::SQ32>(cfg_.lock_pct)) {
      return false;
    }
  }
  return true;
}

}  // namespace o3os::midi
