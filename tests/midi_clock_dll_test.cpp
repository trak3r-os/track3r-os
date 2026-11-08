// SPDX-License-Identifier: GPL-3.0-or-later
//
// The clock-follow loop and the stamp map (midi/clock_dll.h, OS-005c) against simulated external clocks: steady, +-1 % tempo offset, jitter of 50 us (a DIN master) and
// of +-1 ms (a USB host), a tempo ramp and a tempo step, dropped clocks. For each case the lock time and the steady-state phase error are measured against the ideal
// clock the simulation was generated from (the radar card's targets: lock within a beat, phase rms <= 30 us from a DIN master, never a jump). OS-MIDISYNC M3: a step jump
// of the tempo (180 <-> 90 BPM) must settle under 1 ms within 3 s, without the steady-state jitter rejection getting worse. The M3 review adds: dropped clocks are
// counted only inside the score window at a re-fit, the widen band's floor is above the jitter's error (4 ms, stats().widens counts the episodes), and jittered
// steps, dropout runs with bursts, a slow ramp and tempo glides behave. OS-MIDISYNC M5: across the 120 -> 180 -> 90 jumps of the unit battery's test 07
// (platform/mk66/tools/hil/MIDISYNC.md) the emitted tick is the master's clock index at every step boundary, so steps = clocks / 6 with none missing or doubled.
// Set O3OS_SEQ_REPORT=1 to print the table. Host and Cortex-M4F (`qemu_midi`).
#include <doctest/doctest.h>

#include <o3os/core/qtime.h>
#include <o3os/midi/clock_dll.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "support/rng.h"

namespace midi = o3os::midi;
namespace core = o3os::core;
using o3os::test::Rng;

namespace {

constexpr double kFs = 180000000.0 * core::kRefCyclesPerSample.den / core::kRefCyclesPerSample.num;  // samples per second

#if defined(__arm__)
constexpr double kScaleSeconds = 0.25;
#else
constexpr double kScaleSeconds = 1.0;
#endif

struct Sim {
  const char* name;
  double bpm0;          // the master's tempo at the start
  double bpm1;          // at the end (a ramp) or after the step
  int profile;          // 0 constant, 1 linear ramp over the run, 2 step at the middle
  double jitter_us;     // uniform +-
  double drop_pct;      // clocks that never arrive
  double nominal_bpm;   // what the loop assumed before the first interval
  double seconds;
  std::uint32_t seed;
  std::uint32_t burst_at;  // a run of 6 missing clocks starting here (0: none)
  double steady_rms_limit_us;
  double lock_clocks_limit;   // the loop's own locked() flag, in clocks from the first (for a step: from the step)
  std::uint32_t track_mhz;    // the tracking bandwidth, millihertz (500: FOLLOW NORMAL)
};

struct Metrics {
  double lock_clocks = 0;   // the loop's locked() flag first true
  double lock_ms = 0;
  double settle_clocks = 0; // the phase error stays within 1.5 x its steady worst case (and a sample) from here on
  double rms_us = 0;
  double max_us = 0;
  double tempo_err_pct = 0;
  std::uint32_t tick_mismatch = 0;
  std::uint32_t clocks = 0;
  std::uint32_t reacquires = 0;
  std::uint32_t missing = 0;
  double max_step_us = 0;  // the largest change between two consecutive intervals the loop gave (slew, not jump)
};

Metrics simulate(const Sim& sim) {
  midi::ClockDll::Config dcfg;
  dcfg.track_mhz = sim.track_mhz;
  midi::ClockDll dll(dcfg);
  const double nominal_period = kFs * 60.0 / (sim.nominal_bpm * 24.0);
  dll.reset(0, static_cast<core::Q32>(nominal_period * 4294967296.0));
  Rng rng(sim.seed);
  // `total` is the length of a ramp, in clocks of the full run: the target runs a fraction of it, over the first part of the same ramp (the same lag, not a steeper one)
  const std::uint32_t total = static_cast<std::uint32_t>(sim.seconds * sim.bpm0 * 24.0 / 60.0);
  const std::uint32_t run_clocks = static_cast<std::uint32_t>(sim.seconds * kScaleSeconds * sim.bpm0 * 24.0 / 60.0);
  const std::uint32_t half = run_clocks / 2;

  static double err[20000];  // samples, one per accepted clock (indexed by clock number)
  static bool seen[20000];
  const std::uint32_t cap = run_clocks < 20000u ? run_clocks : 20000u;
  for (std::uint32_t i = 0; i < cap; ++i) {
    seen[i] = false;
  }
  double t_true = 1000.0;  // samples: the first clock
  double t_first = t_true;
  double bpm = sim.bpm0;
  Metrics m;
  double prev_used = 0, prev_interval = 0;
  bool have_prev = false;
  std::uint32_t prev_c = 0;
  std::uint32_t first_locked = 0;
  bool locked_seen = false;
  std::uint32_t after_step = 0;  // the clock from which `locked` is looked for again (a step re-acquires)
  std::uint32_t missing_run = 0;
  for (std::uint32_t c = 0; c < cap; ++c) {
    // the master's tempo at this clock
    if (sim.profile == 1) {
      bpm = sim.bpm0 + (sim.bpm1 - sim.bpm0) * static_cast<double>(c) / static_cast<double>(total);
    } else if (sim.profile == 2) {
      bpm = c < half ? sim.bpm0 : sim.bpm1;
    }
    const double period = kFs * 60.0 / (bpm * 24.0);
    if (c != 0u) {
      t_true += period;
    }
    bool drop = c != 0u && (rng.below(10000) < static_cast<std::uint32_t>(sim.drop_pct * 100.0));
    if (sim.burst_at != 0u && c >= sim.burst_at && c < sim.burst_at + 6u) {
      drop = true;
    }
    if (drop) {
      ++missing_run;
      continue;
    }
    missing_run = 0;
    const double jitter = sim.jitter_us != 0.0 ? ((static_cast<double>(rng.below(2000001)) / 1000000.0) - 1.0) * sim.jitter_us * kFs / 1.0e6 : 0.0;
    const double obs = t_true + jitter;
    const midi::ClockDll::Result r = dll.on_clock(static_cast<core::SQ32>(obs * 4294967296.0));
    if (r == midi::ClockDll::Result::Ignored) {
      continue;
    }
    ++m.clocks;
    if (dll.tick() != c) {
      ++m.tick_mismatch;
    }
    if (sim.profile == 2 && c == half) {
      locked_seen = false;  // the step: look for the lock again
      after_step = c;
    }
    if (!locked_seen && dll.locked()) {
      locked_seen = true;
      first_locked = c - after_step;
    }
    const double used = static_cast<double>(dll.time_of_tick(dll.tick())) / 4294967296.0;
    if (c < cap) {
      err[c] = used - t_true;
      seen[c] = true;
    }
    if (have_prev && c == prev_c + 1u) {
      const double interval = used - prev_used;
      if (prev_interval != 0.0 && c >= 3u) {
        const double step = std::fabs(interval - prev_interval) / kFs * 1.0e6;
        if (step > m.max_step_us && (r == midi::ClockDll::Result::Tracked)) {
          m.max_step_us = step;
        }
      }
      prev_interval = interval;
    } else {
      prev_interval = 0.0;
    }
    prev_used = used;
    prev_c = c;
    have_prev = true;
  }
  // steady state: the last quarter of the clocks that were seen
  double sum2 = 0, mx = 0;
  std::uint32_t n = 0;
  for (std::uint32_t c = cap - cap / 4u; c < cap; ++c) {  // the last quarter (a step at the middle has settled by then)
    if (!seen[c]) {
      continue;
    }
    sum2 += err[c] * err[c];
    mx = std::fabs(err[c]) > mx ? std::fabs(err[c]) : mx;
    ++n;
  }
  const double rms = n != 0u ? std::sqrt(sum2 / n) : 0.0;
  m.rms_us = rms / kFs * 1.0e6;
  m.max_us = mx / kFs * 1.0e6;
  // lock: the first clock from which every error stays within 1.5 x the steady-state worst case (and at least a sample, 22.7 us)
  const double thr = 1.5 * mx + 1.0;
  std::uint32_t lock = cap;
  for (std::uint32_t c = cap; c-- > 0;) {
    if (!seen[c]) {
      continue;
    }
    if (std::fabs(err[c]) > thr) {
      break;
    }
    lock = c;
  }
  m.settle_clocks = sim.profile == 2 ? (lock > half ? static_cast<double>(lock - half) : 0.0) : static_cast<double>(lock);
  m.lock_clocks = locked_seen ? static_cast<double>(first_locked) : 1.0e9;
  m.lock_ms = locked_seen ? m.lock_clocks * 60.0 / (sim.bpm0 * 24.0) * 1000.0 : -1.0;
  (void)t_first;
  const double tempo = static_cast<double>(dll.bpm_x100()) / 100.0;
  const double want = sim.profile == 0 ? sim.bpm0 : (sim.profile == 2 ? sim.bpm1 : sim.bpm0 + (sim.bpm1 - sim.bpm0) * static_cast<double>(cap) / static_cast<double>(total));
  m.tempo_err_pct = (tempo - want) / want * 100.0;
  m.reacquires = dll.stats().reacquires;
  m.missing = dll.stats().missing;
  return m;
}

}  // namespace

TEST_CASE("clock map: a stamp is a time on the sample timeline, before or after the reference, across the counter's wrap") {
  midi::ClockMap map;
  map.set_ref(100000, 0xFFFFFF00u);  // the counter is about to wrap
  const core::CyclesPerSample r{4080u, 1u};
  // the reference itself
  CHECK(map.to_rel(0xFFFFFF00u, 100000, r) == 0);
  // 4080 cycles later (across the wrap): one sample
  CHECK(map.to_rel(0xFFFFFF00u + 4080u, 100000, r) == static_cast<core::SQ32>(core::kQ32One));
  // 4080 cycles earlier: minus one sample
  CHECK(map.to_rel(0xFFFFFF00u - 4080u, 100000, r) == -static_cast<core::SQ32>(core::kQ32One));
  // the origin is another sample: the reference is 100000 - 99990 = 10 samples after it
  CHECK(map.to_rel(0xFFFFFF00u, 99990, r) == 10 * static_cast<core::SQ32>(core::kQ32One));
  // half a sample
  CHECK(map.to_rel(0xFFFFFF00u + 2040u, 100000, r) == static_cast<core::SQ32>(core::kQ32Half));
  // a DIN byte before the reference: 57,600 cycles = 14.1176 samples
  const core::SQ32 back = map.to_rel(0xFFFFFF00u - 57600u, 100000, r);
  CHECK(back < -14 * static_cast<core::SQ32>(core::kQ32One));
  CHECK(back > -15 * static_cast<core::SQ32>(core::kQ32One));
  // the 44,100 Hz clock: 441 samples are 1,800,000 cycles
  const core::CyclesPerSample r2{1800000u, 441u};
  CHECK(map.to_rel(0xFFFFFF00u + 1800000u, 100000, r2) == 441 * static_cast<core::SQ32>(core::kQ32One));
}

TEST_CASE("clock dll: the first clocks, the fit, glitches, dropouts, a long silence and the tick count") {
  midi::ClockDll dll;
  const core::Q32 spt = core::samples_per_tick_q32(12000);
  dll.reset(500, 8u * spt);
  CHECK_FALSE(dll.has_phase());
  const core::SQ32 one = static_cast<core::SQ32>(core::kQ32One);
  const core::SQ32 base = 1000 * one;
  const core::SQ32 period = static_cast<core::SQ32>(8u * spt);  // 919 samples
  const auto at = [&](std::int64_t k) { return base + static_cast<core::SQ32>(k) * period; };
  CHECK(dll.on_clock(base) == midi::ClockDll::Result::Started);
  CHECK(dll.has_phase());
  CHECK_FALSE(dll.running());
  CHECK(dll.tick() == 0u);
  // a clock 1 sample later is no clock of any tempo we follow
  CHECK(dll.on_clock(base + one) == midi::ClockDll::Result::Ignored);
  // the first clocks are fitted: provisional until `fit_clocks` of them, then the loop runs, with the period of the straight line through them
  for (std::uint32_t k = 1; k < 12; ++k) {
    CHECK(dll.on_clock(at(k)) == midi::ClockDll::Result::Acquiring);
    CHECK(dll.tick() == k);
    CHECK(dll.has_phase());
  }
  CHECK(dll.running());
  CHECK(dll.period() == static_cast<core::Q32>(period));  // exact: the points are exactly on a line
  CHECK(dll.bpm_x100() == 12000u);
  CHECK(dll.time_of_tick(11) == at(11));
  // the next clocks on time are tracked with no error, and the loop's times are exact
  CHECK(dll.on_clock(at(12)) == midi::ClockDll::Result::Tracked);
  CHECK(dll.tick() == 12u);
  CHECK(dll.time_of_tick(12) == at(12));
  CHECK(dll.time_of_tick(13) == at(13));  // the prediction
  CHECK(dll.time_of_tick(15) == at(15));  // extrapolated
  CHECK(dll.time_of_tick(10) == at(10));  // and back
  CHECK_FALSE(dll.locked());              // eight good clocks are needed
  for (std::uint32_t k = 13; k < 40; ++k) {
    CHECK(dll.on_clock(at(k)) == midi::ClockDll::Result::Tracked);
  }
  CHECK(dll.locked());
  CHECK(dll.tick() == 39u);
  // a missing clock: the loop advances over it
  CHECK(dll.on_clock(at(41)) == midi::ClockDll::Result::Dropout);
  CHECK(dll.tick() == 41u);
  CHECK(dll.stats().missing == 1u);
  // a clock far too early is ignored; the next one on time is the 42nd
  CHECK(dll.on_clock(at(41) + period / 5) == midi::ClockDll::Result::Ignored);
  CHECK(dll.on_clock(at(42)) == midi::ClockDll::Result::Tracked);
  CHECK(dll.tick() == 42u);
  // a long silence: the loop starts again from the next clock, the count of ticks goes on
  CHECK(dll.on_clock(at(200)) == midi::ClockDll::Result::Reacquired);
  CHECK(dll.tick() == 200u);
  CHECK_FALSE(dll.running());
  CHECK(dll.on_clock(at(201)) == midi::ClockDll::Result::Acquiring);
  CHECK(dll.tick() == 201u);
  // staleness
  CHECK_FALSE(dll.stale(at(202), 8));
  CHECK(dll.stale(at(220), 8));
  // a clock that arrives early twice in a row is a tempo that went up: the second one is taken as the next clock, and the count stays right
  midi::ClockDll up;
  up.reset(0, 8u * spt);
  for (std::uint32_t k = 0; k < 30; ++k) {
    (void)up.on_clock(static_cast<core::SQ32>(k) * period);
  }
  CHECK(up.tick() == 29u);
  const core::SQ32 fast = period * 3 / 5;  // 40 % faster
  std::uint32_t expect_tick = 29;
  for (std::uint32_t k = 1; k <= 30; ++k) {
    (void)up.on_clock(29 * period + static_cast<core::SQ32>(k) * fast);
    ++expect_tick;
    CHECK(up.tick() == expect_tick);
  }
  CHECK(up.stats().reacquires >= 1u);
  CHECK(std::llabs(static_cast<std::int64_t>(up.bpm_x100()) - 20000) < 100);  // 120 BPM * 5/3 = 200 BPM
}

TEST_CASE("clock dll: lock time and steady-state phase error against simulated external clocks") {
  const bool report = std::getenv("O3OS_SEQ_REPORT") != nullptr;
  //                     name                                   bpm0   bpm1  prof  jit   drop  nominal  secs  seed burst  rms   lock  B mHz
  const Sim sims[] = {
      {"steady 120 BPM",                                       120.0, 120.0, 0,    0.0,  0.0,  120.0,   20.0, 1,   0,     1.0,  24.0, 500},
      {"steady 120, DIN jitter +-50 us",                       120.0, 120.0, 0,   50.0,  0.0,  120.0,   30.0, 2,   0,    12.0,  30.0, 500},
      {"+1 % off nominal, jitter +-50 us",                     121.2, 121.2, 0,   50.0,  0.0,  120.0,   30.0, 3,   0,    12.0,  30.0, 500},
      {"-1 % off nominal, jitter +-50 us",                     118.8, 118.8, 0,   50.0,  0.0,  120.0,   30.0, 4,   0,    12.0,  30.0, 500},
      {"jitter +-500 us (USB 1 ms frames)",                    120.0, 120.0, 0,  500.0,  0.0,  120.0,   60.0, 5,   0,   100.0,  30.0, 500},
      {"  the same, FOLLOW LOOSE (0.25 Hz)",                   120.0, 120.0, 0,  500.0,  0.0,  120.0,   60.0, 6,   0,    70.0,  30.0, 250},
      {"jitter +-1 ms",                                        120.0, 120.0, 0, 1000.0,  0.0,  120.0,   60.0, 7,   0,   200.0,  60.0, 500},
      {"drift +1 % over the run, jitter +-50 us",              120.0, 121.2, 1,   50.0,  0.0,  120.0,   30.0, 14,  0,    45.0,  30.0, 500},
      {"drift -1 % over the run, jitter +-50 us",              120.0, 118.8, 1,   50.0,  0.0,  120.0,   30.0, 15,  0,    45.0,  30.0, 500},
      {"tempo ramp 120 -> 140 in 60 s, jitter +-50 us",        120.0, 140.0, 1,   50.0,  0.0,  120.0,   60.0, 8,   0,   350.0,  30.0, 500},
      {"tempo step 120 -> 150, jitter +-50 us",                120.0, 150.0, 2,   50.0,  0.0,  120.0,   30.0, 9,   0,    12.0,  30.0, 500},
      {"dropouts 5 %, jitter +-50 us",                         120.0, 120.0, 0,   50.0,  5.0,  120.0,   30.0, 10,  0,    12.0,  30.0, 500},
      {"burst of 6 missing clocks, jitter +-50 us",            120.0, 120.0, 0,   50.0,  0.0,  120.0,   30.0, 11,  300,  12.0,  30.0, 500},
      {"fast 300 BPM, jitter +-50 us",                         300.0, 300.0, 0,   50.0,  0.0,  300.0,   20.0, 12,  0,    12.0,  30.0, 500},
      {"slow 60 BPM, jitter +-50 us",                           60.0,  60.0, 0,   50.0,  0.0,   60.0,   40.0, 13,  0,    14.0,  30.0, 500},
  };
  if (report) {
    std::printf("dll  %-46s %10s %9s %9s %10s %9s %8s %8s\n", "case", "locked at", "lock ms", "settle", "rms us", "max us", "tempo %", "slew us");
  }
  for (const Sim& s : sims) {
    const Metrics m = simulate(s);
    CHECK_MESSAGE(m.tick_mismatch == 0u, s.name);
    CHECK_MESSAGE(m.rms_us <= s.steady_rms_limit_us, s.name);
    CHECK_MESSAGE(m.lock_clocks <= s.lock_clocks_limit, s.name);
    CHECK_MESSAGE(m.lock_ms >= 0.0, s.name);
    CHECK_MESSAGE(m.max_step_us < 1500.0, s.name);  // slew, not a jump: the interval between two clocks changes by a fraction of the error that caused it
    if (s.profile != 1) {
      CHECK_MESSAGE(std::fabs(m.tempo_err_pct) < 0.2, s.name);
    }
    if (report) {
      std::printf("dll  %-46s %10.0f %9.1f %9.0f %10.2f %9.2f %8.3f %8.1f  (reacq %u, missing %u)\n", s.name, m.lock_clocks, m.lock_ms, m.settle_clocks, m.rms_us, m.max_us,
                  m.tempo_err_pct, m.max_step_us, static_cast<unsigned>(m.reacquires), static_cast<unsigned>(m.missing));
    }
  }
}

TEST_CASE("clock dll: a tempo step settles under 1 ms within 3 s (OS-MIDISYNC M3), and the jitter rejection is unchanged") {
  // The phase error here is what the sequencer plays against: the loop's time for the master's clock index c minus that clock's true time — the unit battery's
  // per-step error, sampled at the clocks. 24 ppqn clocks at bpm0 until locked, then a step jump to bpm1 (both ways, 180 <-> 90 BPM: the battery's failing case).
  struct StepResult {
    bool locked_before = false;
    double settle_s = 1.0e9;  // the last clock after the jump whose error is over 1 ms, in seconds after the jump (1e9: none in the run)
    bool reaches_3s = false;
    double err_at_3s_ms = 0.0;
    double max_after_ms = 0.0;
    std::uint32_t mismatch_after_settle = 0;
    double bpm_err_pct = 0.0;
    std::uint32_t reacquires = 0;
  };
  const auto step_run = [](double bpm0, double bpm1) {
    midi::ClockDll dll;  // the default config: FOLLOW NORMAL
    const double period0 = kFs * 60.0 / (bpm0 * 24.0);
    dll.reset(0, static_cast<core::Q32>(period0 * 4294967296.0));
    const std::uint32_t pre = static_cast<std::uint32_t>(240.0 * kScaleSeconds);  // 240 clocks at 180 BPM: locked well before the end
    const std::uint32_t post = static_cast<std::uint32_t>(4.0 * kScaleSeconds * bpm1 * 24.0 / 60.0) + 1u;  // 4 s at the new tempo
    const std::uint32_t cap = pre + post < 600u ? pre + post : 600u;
    static double err_ms[600];  // by master clock index
    static bool seen[600];
    static bool tick_ok[600];
    for (std::uint32_t i = 0; i < cap; ++i) {
      seen[i] = false;
    }
    double t_true = 1000.0;  // samples: the first clock
    StepResult r;
    for (std::uint32_t c = 0; c < cap; ++c) {
      if (c == pre) {
        r.locked_before = dll.locked();
      }
      const double bpm = c < pre ? bpm0 : bpm1;
      const double period = kFs * 60.0 / (bpm * 24.0);
      if (c != 0u) {
        t_true += period;
      }
      const midi::ClockDll::Result res = dll.on_clock(static_cast<core::SQ32>(t_true * 4294967296.0));
      if (res == midi::ClockDll::Result::Ignored) {
        continue;
      }
      err_ms[c] = (static_cast<double>(dll.time_of_tick(c)) / 4294967296.0 - t_true) / kFs * 1000.0;
      seen[c] = true;
      tick_ok[c] = dll.tick() == c;
    }
    std::uint32_t settle_c = pre;
    for (std::uint32_t c = pre; c < cap; ++c) {
      if (seen[c] && std::fabs(err_ms[c]) > 1.0) {
        settle_c = c;
      }
    }
    r.settle_s = settle_c > pre ? (static_cast<double>(settle_c) - pre) * 60.0 / (bpm1 * 24.0) : 0.0;
    for (std::uint32_t c = pre; c < cap; ++c) {
      if (seen[c]) {
        const double s = (static_cast<double>(c) - pre) * 60.0 / (bpm1 * 24.0);
        if (s >= 3.0) {
          r.reaches_3s = true;
          r.err_at_3s_ms = err_ms[c];
          break;
        }
      }
    }
    for (std::uint32_t c = pre; c < cap; ++c) {
      if (seen[c]) {
        const double e = std::fabs(err_ms[c]);
        if (e > r.max_after_ms) {
          r.max_after_ms = e;
        }
      }
    }
    for (std::uint32_t c = settle_c + 1u; c < cap; ++c) {
      if (seen[c] && !tick_ok[c]) {
        ++r.mismatch_after_settle;
      }
    }
    r.bpm_err_pct = (static_cast<double>(dll.bpm_x100()) / 100.0 - bpm1) / bpm1 * 100.0;
    r.reacquires = dll.stats().reacquires;
    return r;
  };
  const struct {
    double bpm0, bpm1;
    const char* name;
  } steps[] = {{180.0, 90.0, "180 -> 90 BPM"}, {90.0, 180.0, "90 -> 180 BPM"}};
  for (const auto& st : steps) {
    const StepResult r = step_run(st.bpm0, st.bpm1);
    CHECK_MESSAGE(r.locked_before, st.name << ": not locked before the jump");
    CHECK_MESSAGE(r.settle_s <= 3.0,
                  st.name << ": settle " << r.settle_s << " s after the jump (limit 3 s); error at 3 s " << r.err_at_3s_ms << " ms, worst after the jump "
                           << r.max_after_ms << " ms, reacquires " << r.reacquires);
    const bool err3_ok = !r.reaches_3s || std::fabs(r.err_at_3s_ms) <= 1.0;
    CHECK_MESSAGE(err3_ok, st.name << ": error at 3 s after the jump " << r.err_at_3s_ms << " ms (limit 1 ms)");
    CHECK_MESSAGE(r.mismatch_after_settle == 0u,
                  st.name << ": " << r.mismatch_after_settle << " clocks after the settle with the tick count off the master's clock index");
    CHECK_MESSAGE(std::fabs(r.bpm_err_pct) < 0.2, st.name << ": tempo error " << r.bpm_err_pct << " % after the step");
  }

  // The steady-state jitter rejection must not have got worse: the spread of the phase error at 120 BPM with a +-2 ms master, over the last 240 of 600 clocks
  // (the same run on the host and on the target), is no larger than the value measured on dev before the fix (rms 363.686 us, worst 947.386 us, seed 7).
  {
    midi::ClockDll dll;
    const double period = kFs * 60.0 / (120.0 * 24.0);
    dll.reset(0, static_cast<core::Q32>(period * 4294967296.0));
    Rng rng(7);
    static double err_us[600];
    std::uint32_t n = 0;
    double t_true = 1000.0;
    for (std::uint32_t c = 0; c < 600; ++c) {
      if (c != 0u) {
        t_true += period;
      }
      const double jitter = ((static_cast<double>(rng.below(2000001)) / 1000000.0) - 1.0) * 2000.0 * kFs / 1.0e6;
      const midi::ClockDll::Result res = dll.on_clock(static_cast<core::SQ32>((t_true + jitter) * 4294967296.0));
      if (res == midi::ClockDll::Result::Ignored) {
        continue;
      }
      err_us[n++] = (static_cast<double>(dll.time_of_tick(c)) / 4294967296.0 - t_true) / kFs * 1.0e6;
    }
    const std::uint32_t w = n < 240u ? n : 240u;
    double sum2 = 0.0, mx = 0.0;
    for (std::uint32_t i = n - w; i < n; ++i) {
      sum2 += err_us[i] * err_us[i];
      mx = std::fabs(err_us[i]) > mx ? std::fabs(err_us[i]) : mx;
    }
    const double rms = w != 0u ? std::sqrt(sum2 / w) : 0.0;
    const bool spread_ok = rms <= 364.0 && mx <= 948.0;
    CHECK_MESSAGE(spread_ok, "jitter spread at 120 BPM +-2 ms: rms " << rms << " us, worst " << mx
                                                          << " us (dev before the fix, same run: rms 363.686 us, worst 947.386 us)");
  }
}

TEST_CASE("clock dll: the M3 review — drops counted only inside the score window, the widen floor above the jitter, glides without re-acquire [OS-MIDISYNC]") {
  // The review of the M3 fix (77dd346d). (1) drop_extra_ accumulated every dropped clock since the last re-fit, so a long dropout run followed by a re-fit
  // subtracted hundreds of ticks and the tick landed far behind the master's clock index: it now counts only the drops inside the score window (cleared when the
  // window falls empty). (2) The widen band's floor was 2.5 ms, below the ~3 ms a +-2 ms jittered clock's error reaches, so jitter alone could widen the loop: it is
  // 4 ms now, and stats().widens counts the widening episodes. Where the master is jittered, the error asserted is the MEAN of the phase error over the last 8
  // clocks: a single clock's filtered error can sit near 1 ms with a +-2 ms master, the mean over a few clocks is what the sequencer hears.
  const auto period_at = [](double bpm) { return kFs * 60.0 / (bpm * 24.0); };
  const auto jitter_of = [](Rng& rng, double jitter_us) {
    return ((static_cast<double>(rng.below(2000001)) / 1000000.0) - 1.0) * jitter_us * kFs / 1.0e6;
  };
  // The last clock after `pre` whose mean |error| over the last 8 seen clocks is over 1 ms, in seconds after the step.
  const auto settle_win_s = [](const double* err_ms, const bool* seen, std::uint32_t pre, std::uint32_t cap, double bpm1) {
    double s = 0.0;
    for (std::uint32_t c = pre; c < cap; ++c) {
      if (!seen[c]) {
        continue;
      }
      double sum = 0.0;
      std::uint32_t n = 0;
      for (std::uint32_t k = c; k >= pre && k + 8u > c; --k) {
        if (seen[k]) {
          sum += std::fabs(err_ms[k]);
          ++n;
        }
      }
      if (n != 0u && sum / n > 1.0) {
        s = (static_cast<double>(c) - pre) * 60.0 / (bpm1 * 24.0);
      }
    }
    return s;
  };

  // The 180 -> 90 step with a jittered master: still settles under 1 ms within 3 s (the mean over the last 8 clocks), and the tick realigns at the step's re-acquire.
  const auto step_jitter_run = [&](double jitter_us, std::uint32_t seed) {
    struct R {
      double settle_s = 0.0;
      double last8_ms = 0.0;
      std::uint32_t mism_after = 0;
      double bpm_err_pct = 0.0;
    };
    midi::ClockDll dll;
    dll.reset(0, static_cast<core::Q32>(period_at(180.0) * 4294967296.0));
    Rng rng(seed);
    const std::uint32_t pre = static_cast<std::uint32_t>(240.0 * kScaleSeconds);
    const std::uint32_t post = static_cast<std::uint32_t>(4.0 * kScaleSeconds * 90.0 * 24.0 / 60.0) + 1u;
    const std::uint32_t cap = pre + post;
    static double err_ms[600];
    static bool seen[600];
    static bool tick_ok[600];
    for (std::uint32_t i = 0; i < cap; ++i) {
      seen[i] = false;
    }
    double t_true = 1000.0;
    std::uint32_t last_reacq = pre;
    for (std::uint32_t c = 0; c < cap; ++c) {
      const double bpm = c < pre ? 180.0 : 90.0;
      if (c != 0u) {
        t_true += period_at(bpm);
      }
      const midi::ClockDll::Result r = dll.on_clock(static_cast<core::SQ32>((t_true + jitter_of(rng, jitter_us)) * 4294967296.0));
      if (r == midi::ClockDll::Result::Ignored) {
        continue;
      }
      if (r == midi::ClockDll::Result::Reacquired) {
        last_reacq = c;
      }
      err_ms[c] = (static_cast<double>(dll.time_of_tick(c)) / 4294967296.0 - t_true) / kFs * 1000.0;
      seen[c] = true;
      tick_ok[c] = dll.tick() == c;
    }
    R r;
    r.settle_s = settle_win_s(err_ms, seen, pre, cap, 90.0);
    double s8 = 0.0;
    std::uint32_t n8 = 0;
    for (std::uint32_t c = cap; c-- > 0u;) {
      if (n8 >= 8u) {
        break;
      }
      if (seen[c]) {
        s8 += std::fabs(err_ms[c]);
        ++n8;
      }
    }
    r.last8_ms = n8 != 0u ? s8 / n8 : 0.0;
    for (std::uint32_t c = last_reacq + 1u; c < cap; ++c) {
      if (seen[c] && !tick_ok[c]) {
        ++r.mism_after;
      }
    }
    r.bpm_err_pct = (static_cast<double>(dll.bpm_x100()) / 100.0 - 90.0) / 90.0 * 100.0;
    return r;
  };
  const struct {
    double jitter_us;
    std::uint32_t seed;
    const char* name;
  } jsteps[] = {{1000.0, 21, "180 -> 90 with +-1 ms jitter"}, {2000.0, 22, "180 -> 90 with +-2 ms jitter"}};
  for (const auto& js : jsteps) {
    const auto r = step_jitter_run(js.jitter_us, js.seed);
    CHECK_MESSAGE(r.settle_s <= 3.0, js.name << ": settles " << r.settle_s << " s after the step (limit 3 s; asserted as the mean over 8 clocks)");
    CHECK_MESSAGE(r.last8_ms <= 1.0, js.name << ": mean phase error over the last 8 clocks " << r.last8_ms << " ms (limit 1 ms)");
    CHECK_MESSAGE(r.mism_after == 0u, js.name << ": " << r.mism_after << " clocks after the re-acquire with the tick off the master's clock index");
    CHECK_MESSAGE(std::fabs(r.bpm_err_pct) < 0.2, js.name << ": tempo error " << r.bpm_err_pct << " %");
  }

  // 5 % dropped clocks for 2000 clocks, then the 180 -> 90 step: the re-fit takes back only the drops inside the score window, so the tick is the master's
  // clock index again after the step's re-acquire (before the fix it landed ~100 clocks behind).
  {
    midi::ClockDll dll;
    dll.reset(0, static_cast<core::Q32>(period_at(180.0) * 4294967296.0));
    Rng rng(31);
    const std::uint32_t pre = static_cast<std::uint32_t>(2000.0 * kScaleSeconds);
    const std::uint32_t post = static_cast<std::uint32_t>(4.0 * kScaleSeconds * 90.0 * 24.0 / 60.0) + 1u;
    const std::uint32_t cap = pre + post;
    static double err_ms[2200];
    static bool seen[2200];
    static bool tick_ok[2200];
    for (std::uint32_t i = 0; i < cap; ++i) {
      seen[i] = false;
    }
    double t_true = 1000.0;
    std::uint32_t reacq_before_step = 0;
    std::uint32_t reacq_at_step = 0;
    for (std::uint32_t c = 0; c < cap; ++c) {
      const double bpm = c < pre ? 180.0 : 90.0;
      if (c != 0u) {
        t_true += period_at(bpm);
      }
      if (c == pre) {
        reacq_before_step = dll.stats().reacquires;
      }
      const bool drop = c >= 16u && c < pre && rng.below(10000) < 500u;  // 5 %, before the step only, after the fit
      if (drop) {
        continue;
      }
      const midi::ClockDll::Result r = dll.on_clock(static_cast<core::SQ32>((t_true + jitter_of(rng, 50.0)) * 4294967296.0));
      if (r == midi::ClockDll::Result::Ignored) {
        continue;
      }
      if (r == midi::ClockDll::Result::Reacquired && c >= pre) {
        reacq_at_step = c;
      }
      err_ms[c] = (static_cast<double>(dll.time_of_tick(c)) / 4294967296.0 - t_true) / kFs * 1000.0;
      seen[c] = true;
      tick_ok[c] = dll.tick() == c;
    }
    CHECK_MESSAGE(reacq_before_step == 0u, "5 % drop run: " << reacq_before_step << " re-acquires before the step (limit 0)");
    CHECK_MESSAGE(reacq_at_step != 0u, "the step did not re-acquire");
    std::uint32_t mism_after = 0;
    for (std::uint32_t c = reacq_at_step + 1u; c < cap; ++c) {
      if (seen[c] && !tick_ok[c]) {
        ++mism_after;
      }
    }
    CHECK_MESSAGE(mism_after == 0u, "5 % drops then 180 -> 90: " << mism_after
                                                                << " clocks after the re-acquire with the tick off the master's clock index (was ~100 before the fix)");
    const double settle_s = settle_win_s(err_ms, seen, pre, cap, 90.0);
    CHECK_MESSAGE(settle_s <= 3.0, "5 % drops then 180 -> 90: settles " << settle_s << " s after the step (limit 3 s)");
    const double bpm_err = (static_cast<double>(dll.bpm_x100()) / 100.0 - 90.0) / 90.0 * 100.0;
    CHECK_MESSAGE(std::fabs(bpm_err) < 0.2, "5 % drops then 180 -> 90: tempo error " << bpm_err << " %");
  }

  // A 5 % drop run with bursts of 6 missing clocks: no re-acquire, no widening, and the tick stays the master's clock index throughout.
  {
    midi::ClockDll dll;
    dll.reset(0, static_cast<core::Q32>(period_at(120.0) * 4294967296.0));
    Rng rng(10);
    const std::uint32_t cap = static_cast<std::uint32_t>(2000.0 * kScaleSeconds);
    const std::uint32_t burst1 = static_cast<std::uint32_t>(500.0 * kScaleSeconds);
    const std::uint32_t burst2 = static_cast<std::uint32_t>(1500.0 * kScaleSeconds);
    double t_true = 1000.0;
    std::uint32_t mismatch = 0;
    for (std::uint32_t c = 0; c < cap; ++c) {
      if (c != 0u) {
        t_true += period_at(120.0);
      }
      bool drop = c >= 16u && rng.below(10000) < 500u;  // 5 %, after the fit
      if (c >= burst1 && c < burst1 + 6u) {
        drop = true;
      }
      if (c >= burst2 && c < burst2 + 6u) {
        drop = true;
      }
      if (drop) {
        continue;
      }
      const midi::ClockDll::Result r = dll.on_clock(static_cast<core::SQ32>((t_true + jitter_of(rng, 50.0)) * 4294967296.0));
      if (r == midi::ClockDll::Result::Ignored) {
        continue;
      }
      if (dll.tick() != c) {
        ++mismatch;
      }
    }
    CHECK_MESSAGE(dll.stats().reacquires == 0u, "5 % drops with bursts: " << dll.stats().reacquires << " re-acquires (limit 0)");
    CHECK_MESSAGE(mismatch == 0u, "5 % drops with bursts: " << mismatch << " clocks with the tick off the master's clock index (limit 0)");
    CHECK_MESSAGE(dll.stats().widens == 0u, "5 % drops with bursts: " << dll.stats().widens << " widening episodes (limit 0)");
  }

  // A slow ramp and two glides (120 -> 125, 120 -> 140 over the run): the loop follows with no re-acquire and no widening.
  const auto ramp_run = [&](double bpm0, double bpm1, double seconds, std::uint32_t seed) {
    struct R {
      std::uint32_t reacquires = 0;
      std::uint32_t widens = 0;
      std::uint32_t mismatch = 0;
      double tempo_err_pct = 0.0;
    };
    midi::ClockDll dll;
    dll.reset(0, static_cast<core::Q32>(period_at(bpm0) * 4294967296.0));
    Rng rng(seed);
    const std::uint32_t total = static_cast<std::uint32_t>(seconds * bpm0 * 24.0 / 60.0);
    double t_true = 1000.0;
    R r;
    for (std::uint32_t c = 0; c < total; ++c) {
      const double bpm = bpm0 + (bpm1 - bpm0) * static_cast<double>(c) / static_cast<double>(total);
      if (c != 0u) {
        t_true += period_at(bpm);
      }
      const midi::ClockDll::Result res = dll.on_clock(static_cast<core::SQ32>((t_true + jitter_of(rng, 50.0)) * 4294967296.0));
      if (res == midi::ClockDll::Result::Ignored) {
        continue;
      }
      if (dll.tick() != c) {
        ++r.mismatch;
      }
    }
    r.reacquires = dll.stats().reacquires;
    r.widens = dll.stats().widens;
    r.tempo_err_pct = (static_cast<double>(dll.bpm_x100()) / 100.0 - bpm1) / bpm1 * 100.0;
    return r;
  };
  {
    const auto r = ramp_run(120.0, 121.0, 60.0 * kScaleSeconds, 51);
    CHECK_MESSAGE(r.widens == 0u, "slow ramp 120 -> 121: " << r.widens << " widening episodes (limit 0)");
    CHECK_MESSAGE(r.reacquires == 0u, "slow ramp 120 -> 121: " << r.reacquires << " re-acquires (limit 0)");
    CHECK_MESSAGE(r.mismatch == 0u, "slow ramp 120 -> 121: " << r.mismatch << " clocks with the tick off the master's clock index");
  }
  const struct {
    double bpm1;
    std::uint32_t seed;
    const char* name;
  } glides[] = {{125.0, 41, "glide 120 -> 125"}, {140.0, 8, "glide 120 -> 140"}};
  for (const auto& g : glides) {
    const auto r = ramp_run(120.0, g.bpm1, 60.0 * kScaleSeconds, g.seed);
    CHECK_MESSAGE(r.reacquires == 0u, g.name << ": " << r.reacquires << " re-acquires (limit 0)");
    CHECK_MESSAGE(r.widens == 0u, g.name << ": " << r.widens << " widening episodes (limit 0)");
    CHECK_MESSAGE(r.mismatch == 0u, g.name << ": " << r.mismatch << " clocks with the tick off the master's clock index");
    // no tempo check here: at the end of a glide the loop lags the tempo a little, as the table above exempts profile 1 too
  }

  // 10^5 clocks at 120 BPM with a +-2 ms master: the widening never arms (the band's floor of 4 ms is above the ~3 ms the jitter's error reaches).
  {
    midi::ClockDll dll;
    dll.reset(0, static_cast<core::Q32>(period_at(120.0) * 4294967296.0));
    Rng rng(7);
    const std::uint32_t cap = static_cast<std::uint32_t>(100000.0 * kScaleSeconds);  // 10^5 clocks on the host
    double t_true = 1000.0;
    for (std::uint32_t c = 0; c < cap; ++c) {
      if (c != 0u) {
        t_true += period_at(120.0);
      }
      const midi::ClockDll::Result r = dll.on_clock(static_cast<core::SQ32>((t_true + jitter_of(rng, 2000.0)) * 4294967296.0));
      if (r == midi::ClockDll::Result::Ignored) {
        continue;
      }
    }
    CHECK_MESSAGE(dll.stats().widens == 0u, "120 BPM +-2 ms, " << cap << " clocks: " << dll.stats().widens
                                                                << " widening episodes (limit 0; was armable at the old 2.5 ms floor)");
    CHECK_MESSAGE(dll.stats().reacquires == 0u, "120 BPM +-2 ms, " << cap << " clocks: " << dll.stats().reacquires << " re-acquires (limit 0)");
  }
}

TEST_CASE("clock dll: OS-MIDISYNC M4 — a steady 300 BPM master holds its phase over 10 minutes, and the soak's worst error stays under 4 ms") {
  // The unit battery (2026-11-05): at a steady 300 BPM the follower's phase offset grew +0.8..0.9 ms per minute while 60-240 BPM held a 0.2-0.3 ms
  // mean, and a 30-minute soak at 120 BPM with a +-2 ms master had a worst single error of 4.46 ms (limit 4.0 ms). Simulated here: (1) a perfect master
  // at 300 BPM for 10 simulated minutes — the trend of the phase error (first-minute against last-minute mean, and a straight-line fit over the
  // per-minute means) stays under 0.1 ms/min; (2) the battery's jittered master at the same tempo (+-2 ms, 10 minutes) — same trend limit, and no
  // re-acquire (a jittered master must never restart the loop: at 300 BPM the 20 %-of-a-period band is only 1.67 ms, below the jitter); (3) 120 BPM
  // with +-2 ms of jitter for 30 simulated minutes — the worst single phase error stays under 4.0 ms.
  const auto period_at = [](double bpm) { return kFs * 60.0 / (bpm * 24.0); };
  const auto run_steady = [&](double bpm, double minutes_d, double jitter_us, std::uint32_t seed, bool count_reacq) {
    struct R {
      double drift_ms_per_min = 0.0;
      double fit_ms_per_min = 0.0;
      double worst_ms = 0.0;
      std::uint32_t reacquires = 0;
      std::uint32_t widens = 0;
      std::uint32_t clocks = 0;
    };
    midi::ClockDll dll;
    const double period = period_at(bpm);
    dll.reset(0, static_cast<core::Q32>(period * 4294967296.0));
    Rng rng(seed);
    const std::uint32_t minutes = static_cast<std::uint32_t>(minutes_d * kScaleSeconds);
    const std::uint32_t per_min = static_cast<std::uint32_t>(bpm * 24.0);  // clocks a minute
    const std::uint32_t cap = minutes * per_min;
    static double min_sum[32];
    static std::uint32_t min_n[32];
    for (std::uint32_t i = 0; i < 32; ++i) {
      min_sum[i] = 0.0;
      min_n[i] = 0;
    }
    double t_true = 1000.0;
    R r;
    for (std::uint32_t c = 0; c < cap; ++c) {
      if (c != 0u) {
        t_true += period;
      }
      const double jitter = jitter_us != 0.0 ? ((static_cast<double>(rng.below(2000001)) / 1000000.0) - 1.0) * jitter_us * kFs / 1.0e6 : 0.0;
      const midi::ClockDll::Result res = dll.on_clock(static_cast<core::SQ32>((t_true + jitter) * 4294967296.0));
      if (res == midi::ClockDll::Result::Ignored) {
        continue;
      }
      const double err = static_cast<double>(dll.time_of_tick(dll.tick())) / 4294967296.0 - t_true;  // samples
      const double ms = std::fabs(err) / kFs * 1.0e3;
      if (ms > r.worst_ms) {
        r.worst_ms = ms;
      }
      const std::uint32_t m = c / per_min;
      if (m < 32u) {
        min_sum[m] += err;
        ++min_n[m];
      }
      ++r.clocks;
    }
    const double first = min_n[0] != 0u ? min_sum[0] / min_n[0] : 0.0;
    const double last = min_n[minutes - 1u] != 0u ? min_sum[minutes - 1u] / min_n[minutes - 1u] : 0.0;
    r.drift_ms_per_min = (last - first) / kFs * 1.0e3 / static_cast<double>(minutes - 1u);
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    std::uint32_t nm = 0;
    for (std::uint32_t m = 0; m < minutes && m < 32u; ++m) {
      if (min_n[m] == 0u) {
        continue;
      }
      const double x = static_cast<double>(m);
      const double y = min_sum[m] / min_n[m] / kFs * 1.0e3;
      sx += x;
      sy += y;
      sxx += x * x;
      sxy += x * y;
      ++nm;
    }
    r.fit_ms_per_min = nm > 1u ? (nm * sxy - sx * sy) / (nm * sxx - sx * sx) : 0.0;
    r.reacquires = dll.stats().reacquires;
    r.widens = dll.stats().widens;
    (void)count_reacq;
    return r;
  };

  // (1) the perfect 300 BPM master, 10 simulated minutes
  {
    const auto r = run_steady(300.0, 10.0, 0.0, 7, false);
    const bool trend_ok = r.drift_ms_per_min < 0.1 && r.drift_ms_per_min > -0.1 && r.fit_ms_per_min < 0.1 && r.fit_ms_per_min > -0.1;
    CHECK_MESSAGE(trend_ok, "300 BPM perfect master, " << r.clocks << " clocks: phase drift " << r.drift_ms_per_min << " ms/min (first-last), fit "
                                                        << r.fit_ms_per_min << " ms/min (limit 0.1; unit battery saw +0.8..0.9 ms/min)");
  }
  // (2) the battery's jittered master at 300 BPM (+-2 ms), 10 simulated minutes: same trend limit, and no re-acquire
  {
    const auto r = run_steady(300.0, 10.0, 2000.0, 7, true);
    const bool trend_ok = r.drift_ms_per_min < 0.1 && r.drift_ms_per_min > -0.1 && r.fit_ms_per_min < 0.1 && r.fit_ms_per_min > -0.1;
    CHECK_MESSAGE(trend_ok, "300 BPM +-2 ms master, " << r.clocks << " clocks: phase drift " << r.drift_ms_per_min << " ms/min (first-last), fit "
                                                       << r.fit_ms_per_min << " ms/min (limit 0.1)");
    CHECK_MESSAGE(r.reacquires == 0u, "300 BPM +-2 ms master, " << r.clocks << " clocks: " << r.reacquires
                                                                 << " re-acquires (limit 0: at 300 BPM the 20 %-of-a-period band is 1.67 ms, below the +-2 ms jitter)");
  }
  // (3) 120 BPM with +-2 ms of jitter, 30 simulated minutes: the worst single phase error
  {
    const auto r = run_steady(120.0, 30.0, 2000.0, 7, false);
    CHECK_MESSAGE(r.worst_ms < 4.0, "120 BPM +-2 ms soak, " << r.clocks << " clocks: worst phase error " << r.worst_ms
                                                             << " ms (limit 4.0; unit battery saw 4.46 ms), re-acquires " << r.reacquires);
  }
}

TEST_CASE("clock dll: OS-MIDISYNC M5 — across the 120 -> 180 -> 90 jumps of battery test 07 the tick is the master's clock index at every step boundary, no step missing or doubled") {
  // The unit battery's test 07 (platform/mk66/tools/hil/MIDISYNC.md): 30 s each at 120, 180 and 90 BPM over USB, so +-1-2 ms of jitter on every clock; the jumps
  // land on step boundaries (a segment is a whole number of steps). A step is 6 clocks at 24 ppqn and the sequencer counts steps on the loop's tick, so at every
  // step boundary the emitted tick must equal the clocks sent so far and the number of step boundaries the tick has crossed must be clocks / 6: none missing (a
  // boundary skipped) and none doubled (a boundary crossed twice — which is what the take-back of phantom ticks at a drop-score re-acquire does when the re-acquire
  // comes only after the tick has run past the next boundary: after the 180 -> 90 step every clock looked like one missing, the tick ran ahead over the whole score
  // window and was pulled back at the re-acquire, re-crossing the boundary; on the unit that was 2 steps missing and 2 extra).
  const auto period_at = [](double bpm) { return kFs * 60.0 / (bpm * 24.0); };
  const auto jitter_of = [](Rng& rng, double jitter_us) {
    return ((static_cast<double>(rng.below(2000001)) / 1000000.0) - 1.0) * jitter_us * kFs / 1.0e6;
  };
  const auto run = [&](double jitter_us, std::uint32_t seed) {
    struct R {
      std::uint32_t bad_boundaries = 0;  // step boundaries where the tick or the crossed-boundary count is off
      std::uint32_t first_bad = 0xFFFFFFFFu;
      std::uint32_t clock_mismatch = 0;  // clocks at any position where the tick is not the master's clock index
      std::uint64_t steps = 0;           // step boundaries crossed by the emitted tick stream (a re-cross after a take-back counts again)
      std::uint32_t reacquires = 0;
      std::uint32_t clocks = 0;
      double bpm_err_pct = 0.0;
    };
    midi::ClockDll dll;
    dll.reset(0, static_cast<core::Q32>(period_at(120.0) * 4294967296.0));
    Rng rng(seed);
    const struct {
      double bpm, seconds;
    } segs[] = {{120.0, 30.0}, {180.0, 30.0}, {90.0, 30.0}};
    double t_true = 1000.0;
    std::uint64_t prev_tick = 0;
    std::uint32_t c = 0;
    R r;
    for (const auto& seg : segs) {
      const std::uint32_t n = static_cast<std::uint32_t>(seg.seconds * kScaleSeconds * seg.bpm * 24.0 / 60.0);
      for (std::uint32_t i = 0; i < n; ++i) {
        if (c != 0u) {
          t_true += period_at(seg.bpm);
        }
        const double obs = t_true + jitter_of(rng, jitter_us);
        const midi::ClockDll::Result res = dll.on_clock(static_cast<core::SQ32>(obs * 4294967296.0));
        if (res != midi::ClockDll::Result::Ignored) {
          const std::uint64_t tk = dll.tick();
          const std::int64_t d = static_cast<std::int64_t>(tk / 6u) - static_cast<std::int64_t>(prev_tick / 6u);
          r.steps += static_cast<std::uint64_t>(d < 0 ? -d : d);
          if (tk != c) {
            ++r.clock_mismatch;
          }
          if (c % 6u == 5u && (tk != c || r.steps != static_cast<std::uint64_t>(c / 6u))) {
            ++r.bad_boundaries;
            if (r.first_bad == 0xFFFFFFFFu) {
              r.first_bad = c;
            }
          }
          if (res == midi::ClockDll::Result::Reacquired) {
            ++r.reacquires;
          }
          prev_tick = tk;
        }
        ++c;
      }
    }
    r.clocks = c;
    r.bpm_err_pct = (static_cast<double>(dll.bpm_x100()) / 100.0 - 90.0) / 90.0 * 100.0;
    return r;
  };
  const struct {
    double jitter_us;
    std::uint32_t seed;
    const char* name;
  } runs[] = {{1000.0, 21, "120 -> 180 -> 90, +-1 ms jitter"}, {2000.0, 22, "120 -> 180 -> 90, +-2 ms jitter"}, {2000.0, 1, "120 -> 180 -> 90, +-2 ms jitter, seed 1"}};
  for (const auto& rn : runs) {
    const auto r = run(rn.jitter_us, rn.seed);
    CHECK_MESSAGE(r.bad_boundaries == 0u,
                  rn.name << ": " << r.bad_boundaries << " step boundaries with the emitted tick or the step count off (first at clock " << r.first_bad << " of "
                           << r.clocks << "; " << r.clock_mismatch << " clocks with the tick off the clock index, " << r.reacquires << " re-acquires)");
    CHECK_MESSAGE(std::fabs(r.bpm_err_pct) < 0.2, rn.name << ": tempo error " << r.bpm_err_pct << " % at the end (limit 0.2)");
  }
}

TEST_CASE("clock dll: detail::float_to_q rounds to nearest (halves away from zero) and converts through int32 halves, exact below 2^40, clamped at 2^46 [OS-FLASHDIET T3]") {
  // The reference of OS-MIDISYNC M4, one float -> int64 conversion of samples * 2^32 +- 0.5 (exact on the host while |p| < 2^63).
  const auto ref = [](float samples) {
    const float p = samples * 4294967296.0f + (samples < 0.0f ? -0.5f : 0.5f);
    return static_cast<std::int64_t>(p);
  };
  const auto at = [](float samples) { return midi::detail::float_to_q(samples); };
  constexpr std::int64_t kClamp = static_cast<std::int64_t>(70368744177664.0f);  // 2^46, the clamp
  // rounding, halves away from zero: samples = +-2^-33 give p = +-1.0 exactly
  CHECK(at(0.0f) == 0);
  CHECK(at(1.0f) == ref(1.0f));
  CHECK(at(-1.0f) == ref(-1.0f));
  CHECK(at(0.5f) == ref(0.5f));
  CHECK(at(-0.5f) == ref(-0.5f));
  CHECK(at(1.1920929e-7f) == ref(1.1920929e-7f));   // 2^-23: p = 0.5 + 0.5 = 1
  CHECK(at(-1.1920929e-7f) == ref(-1.1920929e-7f));  // -2^-23: p = -1
  // the exactness bound |samples * 2^32| < 2^40, i.e. |samples| < 256, and the loop's own input (kWidenMin: 4 ms)
  CHECK(at(255.99999f) == ref(255.99999f));
  CHECK(at(-255.99999f) == ref(-255.99999f));
  CHECK(at(256.0f) == ref(256.0f));
  CHECK(at(0.004f * 44100.0f) == ref(0.004f * 44100.0f));
  CHECK(at(-0.004f * 44100.0f) == ref(-0.004f * 44100.0f));
  // at and beyond the clamp (|samples| >= 2^14): the int32 high half would overflow at |p| >= 2^47, so the clamp holds it at 2^46
  CHECK(at(16384.0f) == kClamp);
  CHECK(at(-16384.0f) == -kClamp);
  CHECK(at(16385.0f) == kClamp);
  CHECK(at(-16385.0f) == -kClamp);
  CHECK(at(1e9f) == kClamp);
  CHECK(at(-1e9f) == -kClamp);
  CHECK(at(1e30f) == kClamp);
  CHECK(at(-1e30f) == -kClamp);
  // a sweep against the reference below the clamp, and the clamp above it
  Rng rng(0x7331u);
  for (unsigned i = 0; i < 20000u; ++i) {
    std::uint32_t bits = rng.next();
    float f;
    std::memcpy(&f, &bits, 4);
    if (!std::isfinite(f)) {
      continue;
    }
    const float p = f * 4294967296.0f + (f < 0.0f ? -0.5f : 0.5f);
    const std::int64_t got = at(f);
    if (std::fabs(p) < 70368744177664.0f) {  // below the clamp: the reference conversion is exact here
      CHECK(got == ref(f));
    } else {
      CHECK(got == (p > 0.0f ? kClamp : -kClamp));
    }
  }
}
