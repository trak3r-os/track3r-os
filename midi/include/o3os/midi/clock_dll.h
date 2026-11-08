// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The clock-follow loop and the stamp-to-timeline map (OS-005c; ADR 0013 D5 "MIDI in", radar card midi-clock-sync, "Slave"). Pure: it is fed
// the time of each MIDI timing clock (24 per quarter note) as the ISR stamped it, and keeps an estimate of the clock's period and phase that
// the sequencer reads, and that SLEWS: it never jumps.
//
// THE LOOP. A second-order delay-locked loop after Adriaensen, "Using a DLL to filter time" (kokkinizita.linuxaudio.org/papers/usingdll.pdf, the
// maths only; the paper states no licence and no code is taken). With e the error between the clock's observed time and the time the loop
// predicted for it, and F the clock rate in clocks per second:
//
//     omega = 2 pi B / F      b = sqrt(2) omega      c = omega^2          (critically damped, B the loop bandwidth in Hz)
//     t0 = t1;   t1 = t1 + b e + T;   T = T + c e                         (t1: the predicted time of the NEXT clock; T: the period)
//
// The output is the prediction made before the clock was seen, so the time the loop gives a clock is never revised by that clock: a microstep
// the sequencer placed between t0 and t1 is final, and the correction a late or early clock brings lands in the following ticks. A constant
// tempo offset is followed with zero steady error; a tempo ramp leaves a constant lag; the residual jitter of the output is about the input's
// times sqrt(2B/F) (the card's arithmetic, measured by the tests of this slice, not assumed).
//
// ACQUISITION. The first clock sets the phase and the next `fit_clocks` (12: half a beat) are fitted by least squares, a straight line through their times, whose
// slope is the first period and whose end is the first phase: much better than the interval between two jittery clocks (the card's own plan, "tempo from the first
// two clocks", is the plain estimate: its error is the jitter over one period, 0.2 % for a DIN master and 4 % for a USB host, which the loop then needs a long
// time to pull out). Then the loop runs at the acquire bandwidth (4 Hz) for `acquire_clocks` clocks (0 by default: the fit is good enough to start tracking) and
// at the tracking bandwidth (0.5 Hz, the FOLLOW NORMAL of the card). omega is capped (at 0.5) so that a fast bandwidth at a slow tempo stays stable. A clock that is missing shows as an observation about k periods after the
// last one: the loop advances k ticks and corrects by the error against the extrapolation (a dropout of up to `max_dropout` clocks). A clock
// that comes less than half a period after the prediction is a glitch and ignored. A tempo that jumps (an error beyond `reacquire_pct` of a
// period and beyond 4 ms, for three clocks in a row) re-acquires, keeping the tick count: the 4 ms floor keeps a jittered master at a fast tempo
// (where the percent of a period is under the jitter) from reading as a jump and re-anchoring the phase to one raw jittered stamp (OS-MIDISYNC M4).
//
// A master that has SLOWED is indistinguishable from one that drops clocks at the instant, but not over time: a sustained density of "missing" clocks (the leaky
// drop score, `drop_score_max`) re-acquires too, taking back the extra ticks the dropouts had added, so the tick stays the master's clock index (OS-MIDISYNC M3:
// a 180 -> 90 BPM step is exactly this — every clock looked like one missing, the loop saw no error at all and the sequencer ran on at the old tempo). Only the drops
// inside the score window are taken back (the count is cleared when the window falls empty), so a long run of genuine dropouts cannot push the tick behind the master's
// clock index at a later re-fit. The level is 800, so four consecutive "missing" clock observations (leaky score 848) re-acquire: at 1024 (six of them) the tick ran
// ahead past the next step boundary (a step is 6 clocks) before the take-back, and the pull-back re-crossed that boundary, so the sequencer emitted the step twice
// (OS-MIDISYNC M5: after the 180 -> 90 jump of battery test 07, 2 steps missing and 2 extra). A sustained error of one sign beyond the jitter band (beyond `widen_pct` of a period and 4 ms, for `widen_clocks` clocks) widens the loop to
// the acquire bandwidth for at most `widen_max` clocks and then returns to the narrow one: a step the reacquire threshold does not reach still settles fast, and the
// steady-state jitter rejection is unchanged (the band's 4 ms floor is above the ~3 ms a +-2 ms jittered clock's error reaches).
//
// ARITHMETIC. Times are SQ32 (Q32.32 samples) relative to an origin sample that the loop owns (origin()), so they never overflow; the loop
// moves the origin on every few hours. The gains are single-precision floats (about ten operations a clock); the state is integer.
//
// STAMPS. The ISR stamps a byte with the 32-bit DWT cycle count. ClockMap turns a stamp into the sample timeline from one reference pair (the
// first sample of a block and the cycle count stamped at that block's boundary), by one division: no clock-domain estimation, the audio clock and
// the cycle counter being one PLL (radar card, finding 2).

#include <o3os/core/audio_clock.h>
#include <o3os/core/qtime.h>

#include <cstdint>

namespace o3os::midi {

// Q32 samples from a float span, for clock_dll.cpp (and its test). Rounded to nearest (halves away from zero), not truncated: the two loop
// corrections are added to the phase and the period on every clock, and a truncation there biases them by up to one Q32 unit per clock, always
// the same way for errors of one sign (OS-MIDISYNC M4).
//
// The conversion itself goes through two int32 halves — a float -> int64 one would call __aeabi_f2lz and pull libgcc's soft-double helpers
// into the image (OS-FLASHDIET T3) — so |p| is clamped at 2^46 in float first: the int32 high half then always fits (a raw |p| >= 2^47 would
// overflow it, UB). Exact while |p| < 2^40. The calls stay far below the clamp: kWidenMin is 4 ms of samples (0.004 x 44100 = 176, |p| ~ 0.7 x 2^40),
// and the loop's two calls take b * ef and c * ef with ef the phase error e in samples; at them drops <= max_dropout (a longer gap returns
// Reacquired before the corrections) and in the plain path |e| < period_/2 <= kPeriodMax/2, so |p| <~ 2^42. The re-acquire band does not bound e
// on the first two large-error clocks, and the kk-override path can leave e at the whole gap, which is what the clamp is for (it trips only
// beyond a 2^14-sample = 0.37 s error, which the re-acquire band resets three clocks later).
namespace detail {
constexpr core::SQ32 float_to_q(float samples) noexcept {
  const float p = samples * static_cast<float>(core::kQ32One) + (samples < 0.0f ? -0.5f : 0.5f);
  const float lim = 70368744177664.0f;  // 2^46
  const float q = p < -lim ? -lim : (p > lim ? lim : p);
  const std::int32_t hi = static_cast<std::int32_t>(q * (1.0f / 65536.0f));
  return static_cast<std::int64_t>(hi) * 65536 + static_cast<std::int32_t>(q - static_cast<float>(hi) * 65536.0f);
}
}  // namespace detail

// ---- the stamp map -----------------------------------------------------------------------------------------------------
class ClockMap {
 public:
  // The reference: `sample` is the timeline's sample number at the instant the cycle counter read `cycles` (the DMA ISR's stamp of a block
  // boundary and that block's first sample). Called by the render at the start of every block.
  void set_ref(std::uint64_t sample, std::uint32_t cycles) noexcept {
    ref_sample_ = sample;
    ref_cycles_ = cycles;
  }
  std::uint64_t ref_sample() const noexcept { return ref_sample_; }

  // `cycles` as a time on the sample timeline, in Q32 samples relative to the whole sample `origin` (positive when the stamp is later than the
  // origin). The stamp may be before or after the reference by up to 2^31 cycles (11.9 s at 180 MHz): only differences are meaningful, as for the
  // counter itself.
  core::SQ32 to_rel(std::uint32_t cycles, std::uint64_t origin, core::CyclesPerSample r = core::kRefCyclesPerSample) const noexcept {
    const std::int32_t delta = static_cast<std::int32_t>(cycles - ref_cycles_);
    const std::uint64_t mag = delta < 0 ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(delta)) : static_cast<std::uint64_t>(delta);
    const core::SQ32 dq = static_cast<core::SQ32>(core::q32_from_cycles(mag, r));
    const core::SQ32 base = (static_cast<core::SQ32>(ref_sample_) - static_cast<core::SQ32>(origin)) * static_cast<core::SQ32>(core::kQ32One);
    return base + (delta < 0 ? -dq : dq);
  }

 private:
  std::uint64_t ref_sample_ = 0;
  std::uint32_t ref_cycles_ = 0;
};

// ---- the loop -----------------------------------------------------------------------------------------------------------
class ClockDll {
 public:
  struct Config {
    std::uint32_t acquire_mhz = 4000;   // loop bandwidth while acquiring, millihertz (4 Hz)
    std::uint32_t track_mhz = 500;      // loop bandwidth once locked, millihertz (0.5 Hz: FOLLOW NORMAL; TIGHT 1.0, LOOSE 0.25)
    std::uint32_t fit_clocks = 12;      // clocks fitted by least squares before the loop starts (2..16)
    std::uint32_t acquire_clocks = 0;   // clocks at the acquire bandwidth after the fit
    std::uint32_t reacquire_pct = 20;   // an error beyond this percent of a period and beyond 4 ms, three clocks in a row, re-acquires
    std::uint32_t max_dropout = 24;     // clocks that may be missing between two seen (a beat); more re-acquires
    std::uint32_t lock_pct = 5;         // locked() when every recent error is below this percent of a period
    std::uint32_t drop_score_max = 800;  // a leaky score of "missing clock" observations (256 each, 7/8 kept per clock) at this level: the master has slowed, re-fit. Four in a row (score 848) is the level: the re-acquire must fire before the extra ticks the dropouts added can carry the tick past the next step boundary (6 clocks), or the take-back re-crosses that boundary and the sequencer emits the step twice (OS-MIDISYNC M5)
    std::uint32_t widen_pct = 10;       // an error beyond this percent of a period and beyond 4 ms, of one sign for widen_clocks clocks, widens the loop
    std::uint32_t widen_clocks = 4;     // clocks of one-sign error beyond the band that start the widening
    std::uint32_t widen_max = 64;       // clocks the loop stays at the acquire bandwidth once widened
  };

  enum class Result : std::uint8_t {
    Started,     // the first clock after a reset
    Acquiring,   // one of the first clocks, being fitted: the period and phase are provisional
    Tracked,     // the normal case
    Dropout,     // clocks were missing: the loop advanced over them
    Ignored,     // too early to be the next clock (a glitch)
    Reacquired,  // a tempo jump or a long gap: phase and period restart from this clock, the tick count goes on
  };

  struct Stats {
    std::uint32_t clocks = 0;       // accepted
    std::uint32_t ignored = 0;
    std::uint32_t dropouts = 0;     // observations that skipped at least one clock
    std::uint32_t missing = 0;      // clocks inferred missing
    std::uint32_t reacquires = 0;
    std::uint32_t widens = 0;       // widening episodes: the loop went to the acquire bandwidth on a sustained one-sign error beyond the band
  };

  ClockDll() = default;
  explicit ClockDll(const Config& c) : cfg_(c) {}
  void configure(const Config& c) noexcept { cfg_ = c; }
  const Config& config() const noexcept { return cfg_; }

  // Forget the lock. `origin` is the whole sample that times are relative to from now on; `nominal` the period (samples per clock, Q32) to
  // assume until two clocks have been seen: the sequencer's current tempo * 8 ticks.
  void reset(std::uint64_t origin, core::Q32 nominal_period) noexcept;

  // A clock observed at `t` (Q32 samples relative to origin(), as ClockMap::to_rel gives it, already corrected for the port's latency).
  Result on_clock(core::SQ32 t) noexcept;

  // ---- what the sequencer reads ---------------------------------------------------------------------------------------
  std::uint64_t origin() const noexcept { return origin_; }
  bool running() const noexcept { return state_ == State::Running; }
  bool has_phase() const noexcept { return state_ != State::Idle; }
  // Index of the last accepted clock (counts from the first after a reset, dropouts included), and its filtered time.
  std::uint64_t tick() const noexcept { return tick_; }
  // The time the loop gives clock `tick`: the filtered time of the last accepted one, the prediction for the next, extrapolated by the period
  // beyond that (and back before it). Q32 relative to origin(). Only meaningful when has_phase().
  core::SQ32 time_of_tick(std::uint64_t tick) const noexcept;
  // OS-MIDISYNC 07. The highest clock index a step may be played at on the loop's word alone. No bound (the maximum) while the loop tracks. While a slowdown
  // may be under way it is the index of the last clock actually seen: a "missing clock" observation is a dropout or a master at half tempo, which the loop
  // cannot tell apart until the leaky score re-acquires, and every phantom tick it adds (and the fast period it keeps predicting with) puts the tick that far
  // ahead of the master's clock index, so a step reached only through them would sound early. tick() and time_of_tick() are not changed by this.
  //   - two or more consecutive observations that each skipped clocks: tick() minus the clocks they skipped (a clean observation ends the run);
  //   - one such observation: tick() + 1. It may be a single dropout, which the loop bridges in time and whose next clock the sequencer plays on the prediction
  //     (waiting for it would play it late by the clock's stamp and the block): only the clocks beyond the next one wait. A slowdown's first clocks are
  //     the part no follower can know (the next one is predicted a fast period out, and the loop cannot yet see it is late);
  //   - after the re-acquire that run ended in, until the second clock refits the period: tick() (the loop predicts with the old period meanwhile).
  std::uint64_t settled_tick() const noexcept {
    if (slow_hold_) {
      return tick_;
    }
    if (run_n_ == 1u) {
      return tick_ + 1u;
    }
    if (run_n_ >= 2u && run_extra_ <= tick_) {
      return tick_ - run_extra_;
    }
    return ~std::uint64_t{0};
  }
  // Samples per clock, Q32, and the tempo it means, BPM x 100.
  core::Q32 period() const noexcept { return static_cast<core::Q32>(period_); }
  std::uint32_t bpm_x100() const noexcept;
  // The last error (observed minus predicted), samples as a float, for the status line; and the largest of the last eight clocks.
  float last_error_samples() const noexcept;
  bool locked() const noexcept;
  // No clock for more than `clocks` periods before `now` (Q32 relative to origin()): the master has gone quiet.
  bool stale(core::SQ32 now, std::uint32_t clocks) const noexcept {
    return state_ != State::Idle && now - last_obs_ > static_cast<core::SQ32>(period_) * static_cast<core::SQ32>(clocks);
  }
  const Stats& stats() const noexcept { return stats_; }

 private:
  enum class State : std::uint8_t { Idle, Started, Running };
  void rebase_if_needed() noexcept;
  void clamp_period() noexcept;
  void start_fit(core::SQ32 t) noexcept;
  Result fit_clock(core::SQ32 t) noexcept;
  float omega() const noexcept;

  Config cfg_{};
  State state_ = State::Idle;
  std::uint64_t origin_ = 0;
  core::SQ32 t0_ = 0;       // filtered time of tick_
  core::SQ32 t1_ = 0;       // predicted time of tick_ + 1
  core::SQ32 period_ = 0;   // T
  core::SQ32 last_obs_ = 0; // the last accepted observation (for the second clock and for stale())
  core::SQ32 nominal_ = 0;
  std::uint64_t tick_ = 0;
  std::uint32_t since_acquire_ = 0;
  std::uint32_t bad_run_ = 0;
  core::SQ32 fit_t0_ = 0;          // the first clock of the fit
  core::SQ32 fit_u_[16] = {};      // the times of the fitted clocks relative to fit_t0_
  std::uint32_t fit_n_ = 0;
  core::SQ32 err_ = 0;
  core::SQ32 err_hist_[8] = {};
  std::uint32_t err_pos_ = 0;
  std::uint32_t err_n_ = 0;
  std::uint32_t drop_score_ = 0;   // leaky score of recent observations that looked like missing clocks
  std::uint32_t drop_extra_ = 0;   // ticks the dropouts inside the score window have added (taken back at a re-acquire when they were a slowdown; cleared when the window falls empty)
  std::uint32_t run_extra_ = 0;    // clocks skipped by the current run of consecutive "missing" observations (settled_tick())
  std::uint32_t run_n_ = 0;        // the observations in that run
  bool slow_hold_ = false;         // re-acquired from a suspected slowdown and the period is not refitted yet (settled_tick())
  std::int32_t err_run_ = 0;       // signed run of consecutive errors of one sign beyond the widen band
  std::uint32_t wide_clocks_ = 0;  // clocks left at the acquire bandwidth (the widening is bounded)
  Stats stats_{};
};

}  // namespace o3os::midi
