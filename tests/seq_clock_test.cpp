// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-ABI2 A3 (docs/specs/OS-INSFX-ABI2.md section 3.4), the sequencer's side of get_clock: Sequencer::clock_at(sample), the playhead as the engine asks it at a block boundary. Host and
// Cortex-M4F (`qemu_seq_model`, one project in .bss).
//   * stopped: no flags, beat 0, the project's tempo (the Perform override and the step rate are not in it);
//   * the beat counts from step 0 of the pattern that plays: the boundary of step n is n / 4 beats, inside a step it is linear in the sample (within 1e-4 beat), under swing (a pair is two nominal steps
//     with unequal halves), a Tempo FX, half and double time; `tempo_x100` is what plays;
//   * STARTED once for each run (a start, a restart, a resume), PLAYING while it runs, WRAPPED when the beat went back (a pattern's end, a Break pattern, a jump back) and not for a jump forward;
//   * a count-in holds the start point's beat; a run from step k starts the beat at k / 4;
//   * under an external clock (seq_follow_test.cpp) the beat follows the master's microsteps.
#include <doctest/doctest.h>

#include <o3os/audio/event_source.h>
#include <o3os/seq/sequencer.h>

#include <cmath>
#include <cstdint>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace fx = o3os::seq::fx;

namespace {

constexpr std::uint32_t kPlaying = 1u;  // O3M_CLOCK_PLAYING, STARTED, WRAPPED (module/sdk/o3module.h)
constexpr std::uint32_t kStarted = 2u;
constexpr std::uint32_t kWrapped = 4u;

StaticSlot<seq::Sequencer>& g_slot = seq_slot();
Collector& g_col = seq_collector();

seq::Sequencer& make() {
  seq::Sequencer& s = g_slot.make(seq_project());
  g_col.n = 0;
  g_col.total_events = 0;
  g_col.hash = 0xCBF29CE484222325ull;
  return s;
}

// A project of one pattern (id 1) of `length` steps at `bpm_x100`.
void pattern_of(std::uint32_t length, std::uint32_t bpm_x100 = 12000) {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(bpm_x100) == model::Status::Ok);
  CHECK(p.create_pattern(1, length) == model::Status::Ok);
}

double beat_of(const audio::ClockPos& c) { return static_cast<double>(c.beat_q16) / 65536.0; }

// The sequencer's blocks from `t` to `to` (64 samples each, the last one shortened to end exactly at `to`); returns `to`.
std::uint64_t advance(seq::Sequencer& s, std::uint64_t t, std::uint64_t to) {
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  while (t < to) {
    const std::uint32_t n = static_cast<std::uint32_t>(to - t < 64u ? to - t : 64u);
    (void)s.step(t, n, out, audio::kMaxEventsPerBlock);
    t += n;
  }
  return t;
}

// The samples of a run's step boundaries: Step event k is the start of step k (a fresh sequencer, 64-sample blocks).
struct Grid {
  static constexpr std::uint32_t kMax = 40;
  std::uint64_t t[kMax + 1];
};

Grid grid_of(std::uint32_t steps, std::uint32_t rate) {
  Grid g{};
  seq::Sequencer& s = make();
  s.set_play_rate(rate);
  s.play_pattern(1);
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  for (std::uint64_t t = 0; g_col.count(EventKind::Step) < steps + 1u && t < 4000000u; t += 64u) {
    const std::uint32_t c = s.step(t, 64, out, audio::kMaxEventsPerBlock);
    for (std::uint32_t i = 0; i < c; ++i) {
      g_col.add(t, out[i]);
    }
  }
  const std::uint32_t have = g_col.count(EventKind::Step);
  CHECK(have >= steps + 1u);
  for (std::uint32_t k = 0; k <= steps && k < have; ++k) {
    g.t[k] = g_col.nth(EventKind::Step, k)->t;
  }
  return g;
}

// A second run of the same project, asked for the beat in the middle of every step and at every step boundary: the mid-step beat is (k + the fraction of the step that has passed) / 4, the boundary
// is the next step's row / 4 (the pattern's end shows the old pattern's last boundary, len / 4: the wrap is seen one block later). Returns the tempo it ends on.
std::uint32_t check_grid(const Grid& g, std::uint32_t steps, std::uint32_t len, std::uint32_t rate) {
  seq::Sequencer& s = make();
  s.set_play_rate(rate);
  s.play_pattern(1);
  std::uint64_t t = 0;
  std::uint32_t tempo = 0;
  for (std::uint32_t k = 0; k < steps; ++k) {
    const std::uint64_t mid = (g.t[k] + g.t[k + 1]) / 2u;
    t = advance(s, t, mid);
    const audio::ClockPos m = s.clock_at(mid);
    const double want_mid = (static_cast<double>(k % len) + static_cast<double>(mid - g.t[k]) / static_cast<double>(g.t[k + 1] - g.t[k])) / 4.0;
    // 1e-4 beat, plus what the rounding of the two boundary samples the mid-point is measured from adds (a sample and a half of the step: 800 BPM has steps of 826 samples)
    const double tol = 1e-4 + 1.5 / (4.0 * static_cast<double>(g.t[k + 1] - g.t[k]));
    CHECK_MESSAGE(std::fabs(beat_of(m) - want_mid) < tol, "mid of step " << k << ": " << beat_of(m) << " vs " << want_mid);
    t = advance(s, t, g.t[k + 1]);
    const audio::ClockPos b = s.clock_at(t);
    const std::uint32_t next = (k + 1u) % len;
    const double want_edge = static_cast<double>(next == 0u ? len : next) / 4.0;
    CHECK_MESSAGE(std::fabs(beat_of(b) - want_edge) < tol, "boundary after step " << k << ": " << beat_of(b) << " vs " << want_edge);
    CHECK((b.flags & kPlaying) != 0u);
    tempo = b.tempo_x100;
  }
  return tempo;
}

}  // namespace

TEST_CASE("seq clock: stopped says no flags, beat 0 and the project's tempo; the Perform override and the step rate are not in it") {
  pattern_of(16, 12000);
  seq::Sequencer& s = make();
  audio::ClockPos c = s.clock_at(1000);
  CHECK((c.flags == 0u && c.beat_q16 == 0u && c.tempo_x100 == 12000u));
  s.set_perf_tempo(24000);
  s.set_play_rate(2);
  c = s.clock_at(2000);
  CHECK((c.flags == 0u && c.beat_q16 == 0u && c.tempo_x100 == 12000u));  // what plays when it plays is tempo_x100(); stopped it is the project's
  CHECK(s.tempo_x100() == 48000u);
  // play_pattern() takes effect at the next block: until then it is stopped too
  s.set_perf_tempo(0);
  s.set_play_rate(0);
  s.play_pattern(1);
  c = s.clock_at(0);
  CHECK((c.flags == 0u && c.beat_q16 == 0u));
  // a project tempo that is not 120
  pattern_of(16, 9050);
  seq::Sequencer& s2 = make();
  CHECK(s2.clock_at(0).tempo_x100 == 9050u);
}

TEST_CASE("seq clock: a straight grid: the beat at every block end is the sample over the beat's length within 1e-4, PLAYING all along, STARTED once, WRAPPED once a lap") {
  pattern_of(16, 12000);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  const double spb = samples_per_second() * 60.0 / 120.0;  // samples in a beat
  const double lap = 4.0;                                   // 16 steps
  std::uint32_t started = 0;
  std::uint32_t wrapped = 0;
  std::uint32_t checked = 0;
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  const std::uint64_t end = static_cast<std::uint64_t>(spb * lap * 3.5);
  for (std::uint64_t t = 0; t < end; t += 64) {
    (void)s.step(t, 64, out, audio::kMaxEventsPerBlock);
    const audio::ClockPos c = s.clock_at(t + 64);
    CHECK((c.flags & kPlaying) != 0u);
    CHECK(c.tempo_x100 == 12000u);
    started += (c.flags & kStarted) != 0u ? 1u : 0u;
    wrapped += (c.flags & kWrapped) != 0u ? 1u : 0u;
    CHECK(((c.flags & kStarted) != 0u) == (t == 0u));  // the first block only
    if ((c.flags & (kStarted | kWrapped)) == (kStarted | kWrapped)) {
      CHECK_MESSAGE(false, "STARTED and WRAPPED together at " << t);
    }
    const double want = std::fmod(static_cast<double>(t + 64) / spb, lap);
    if (want > 3e-4 && want < lap - 3e-4) {  // not within a sample of the wrap
      CHECK_MESSAGE(std::fabs(beat_of(c) - want) < 1e-4, "block at " << t << ": " << beat_of(c) << " vs " << want);
      ++checked;
    }
  }
  CHECK(started == 1u);
  CHECK(wrapped == 3u);  // laps 1, 2 and 3 end inside the run
  CHECK(checked > 3000u);
}

TEST_CASE("seq clock: the beat is piecewise linear: swing, a Tempo FX, half and double time (the boundary of step n is n / 4, the middle of a step is where its length puts it)") {
  pattern_of(16, 12000);
  Grid g = grid_of(20, 0);
  CHECK(check_grid(g, 20, 16, 0) == 12000u);

  // swing: a pair is two nominal steps, the even step of 70 % lasts 0.8 of one and the odd 1.2
  pattern_of(16, 12000);
  CHECK(seq_project().set_swing(70) == model::Status::Ok);
  g = grid_of(20, 0);
  CHECK(check_grid(g, 20, 16, 0) == 12000u);
  {
    // not linear in time: at the middle of the pair's time the beat is already past the pair's middle (the first step was short)
    seq::Sequencer& s = make();
    s.play_pattern(1);
    const std::uint64_t mid = (g.t[0] + g.t[2]) / 2u;
    (void)advance(s, 0, mid);
    CHECK(std::fabs(beat_of(s.clock_at(mid)) - 0.25) > 0.03);
  }

  // a Tempo FX on step 2 (90 x 2 = 180 BPM, from its own row): the clock's tempo is what plays after it
  pattern_of(16, 12000);
  CHECK(seq_project().set_step(1, 1, 2, fx_step(fx::kTempo, 90)) == model::Status::Ok);
  g = grid_of(20, 0);
  CHECK(check_grid(g, 20, 16, 0) == 18000u);

  // half time: the steps are twice as long, the tempo that plays is half
  pattern_of(16, 12000);
  g = grid_of(12, 1);
  CHECK(check_grid(g, 12, 16, 1) == 6000u);
  // double time with swing
  pattern_of(16, 12000);
  CHECK(seq_project().set_swing(60) == model::Status::Ok);
  g = grid_of(24, 2);
  CHECK(check_grid(g, 24, 16, 2) == 24000u);
  // and the FX tempo at half time is scaled, not replaced
  pattern_of(16, 12000);
  CHECK(seq_project().set_step(1, 0, 2, fx_step(fx::kTempo, 120)) == model::Status::Ok);  // 240 BPM
  g = grid_of(12, 1);
  CHECK(check_grid(g, 12, 16, 1) == 12000u);
}

TEST_CASE("seq clock: the extremes of the tempo range keep the beat inside its step") {
  const std::uint32_t tempos[] = {4000, 33333, 80000};
  for (const std::uint32_t bpm : tempos) {
    pattern_of(16, bpm);
    const Grid g = grid_of(6, 0);
    CHECK_MESSAGE(check_grid(g, 6, 16, 0) == bpm, bpm);
  }
}

TEST_CASE("seq clock: WRAPPED at a pattern's end every lap, at a Break pattern every time, at a jump back and not at a jump forward") {
  // a lap of 8 steps: one WRAPPED a lap, and as many as the Pattern events the sequencer emitted
  pattern_of(8, 12000);
  {
    seq::Sequencer& s = make();
    s.play_pattern(1);
    const double spb = samples_per_second() * 60.0 / 120.0;
    const std::uint64_t end = static_cast<std::uint64_t>(spb * 2.0 * 5.5);  // 5.5 laps
    audio::SeqEvent out[audio::kMaxEventsPerBlock];
    std::uint32_t wrapped = 0;
    std::uint32_t started = 0;
    for (std::uint64_t t = 0; t < end; t += 64) {
      const std::uint32_t c = s.step(t, 64, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        g_col.add(t, out[i]);
      }
      const audio::ClockPos p = s.clock_at(t + 64);
      wrapped += (p.flags & kWrapped) != 0u ? 1u : 0u;
      started += (p.flags & kStarted) != 0u ? 1u : 0u;
    }
    CHECK(g_col.count(EventKind::Pattern) == 5u);
    CHECK(wrapped == 5u);
    CHECK(started == 1u);
  }
  // a Break pattern after step 3 of 16: the pattern restarts every 4 steps
  pattern_of(16, 12000);
  CHECK(seq_project().set_step(1, 2, 3, fx_step(fx::kBreakPattern, 1)) == model::Status::Ok);
  {
    seq::Sequencer& s = make();
    s.play_pattern(1);
    const double spb = samples_per_second() * 60.0 / 120.0;
    const std::uint64_t end = static_cast<std::uint64_t>(spb * 6.5);  // 6.5 laps of 4 steps
    audio::SeqEvent out[audio::kMaxEventsPerBlock];
    std::uint32_t wrapped = 0;
    double top = 0.0;
    for (std::uint64_t t = 0; t < end; t += 64) {
      (void)s.step(t, 64, out, audio::kMaxEventsPerBlock);
      const audio::ClockPos p = s.clock_at(t + 64);
      wrapped += (p.flags & kWrapped) != 0u ? 1u : 0u;
      top = std::fmax(top, beat_of(p));
    }
    CHECK(wrapped == 6u);
    CHECK(top <= 1.0 + 1e-4);  // the beat never passed the break: step 4 is the pattern's start again
  }
  // a locate while playing: back is WRAPPED once, forward is not
  pattern_of(16, 12000);
  {
    seq::Sequencer& s = make();
    s.play_pattern(1);
    std::uint64_t t = advance(s, 0, 35000);  // about step 6
    const double before = beat_of(s.clock_at(t));
    CHECK(before > 1.4);
    s.locate_row(1);
    std::uint32_t wrapped = 0;
    double after = 0.0;
    for (std::uint32_t b = 0; b < 200; ++b) {  // 12,800 samples: the jump lands at the next step boundary, the line runs on
      t = advance(s, t, t + 64);
      const audio::ClockPos p = s.clock_at(t);
      wrapped += (p.flags & kWrapped) != 0u ? 1u : 0u;
      after = beat_of(p);
    }
    CHECK(wrapped == 1u);
    CHECK(after < before);  // it went back: row 1 plus what has run since, not row 6 plus it
    // forward: row 12 from wherever it is: the beat jumps up and nobody is told it wrapped
    s.locate_row(12);
    wrapped = 0;
    double top = 0.0;
    for (std::uint32_t b = 0; b < 100; ++b) {  // 6,400 samples, a bit over a step: the jump landed and nothing wrapped yet
      t = advance(s, t, t + 64);
      const audio::ClockPos p = s.clock_at(t);
      wrapped += (p.flags & kWrapped) != 0u ? 1u : 0u;
      top = std::fmax(top, beat_of(p));
    }
    CHECK(top >= 12.0 / 4.0);
    CHECK(wrapped == 0u);
  }
}

TEST_CASE("seq clock: STARTED for each run: a start, a restart over a run and a resume; a run from step k starts its beat at k / 4; a run that began and ended between two calls tells nobody") {
  pattern_of(16, 12000);
  seq::Sequencer& s = make();
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  s.play_pattern(1, 5);
  std::uint64_t t = 0;
  (void)s.step(t, 64, out, audio::kMaxEventsPerBlock);
  t += 64;
  audio::ClockPos c = s.clock_at(t);
  CHECK(c.flags == (kPlaying | kStarted));
  CHECK((beat_of(c) >= 5.0 / 4.0 && beat_of(c) < 5.0 / 4.0 + 0.01));
  t = advance(s, t, 50000);
  c = s.clock_at(t);
  CHECK(c.flags == kPlaying);
  CHECK(beat_of(c) > 1.25);
  // Play over the run: a fresh run (the beat goes back, but it is a start)
  s.play_pattern(1);
  t = advance(s, t, t + 64);
  c = s.clock_at(t);
  CHECK(c.flags == (kPlaying | kStarted));
  CHECK(beat_of(c) < 0.05);
  t = advance(s, t, t + 64);
  CHECK(s.clock_at(t).flags == kPlaying);
  // Stop, then resume where it stopped
  t = advance(s, t, 30000);
  s.stop();
  t = advance(s, t, t + 64);
  c = s.clock_at(t);
  CHECK((c.flags == 0u && c.beat_q16 == 0u && c.tempo_x100 == 12000u));
  s.resume();
  t = advance(s, t, t + 64);
  c = s.clock_at(t);
  CHECK(c.flags == (kPlaying | kStarted));
  CHECK(static_cast<std::uint32_t>(beat_of(c) * 4.0) == s.position().step);  // on the step it went on from
  t = advance(s, t, t + 64);
  CHECK(s.clock_at(t).flags == kPlaying);
  // a run that began and ended between two calls: the clock never saw it run
  s.stop();
  t = advance(s, t, t + 64);
  CHECK(s.clock_at(t).flags == 0u);
  s.play_pattern(1);
  t = advance(s, t, t + 64);  // (no clock_at: the engine asks once a block; a second block follows before it does)
  s.stop();
  t = advance(s, t, t + 64);
  CHECK(s.clock_at(t).flags == 0u);
  s.play_pattern(1);
  t = advance(s, t, t + 64);
  CHECK(s.clock_at(t).flags == (kPlaying | kStarted));  // and the next run is told
}

TEST_CASE("seq clock: a count-in holds the start point's beat, with no WRAPPED, and the run goes on from it") {
  pattern_of(16, 12000);
  seq::Sequencer& s = make();
  s.play_pattern(1, 8, 1);  // one bar of count-in, then from step 8 (beat 2.0)
  const double spb = samples_per_second() * 60.0 / 120.0;
  const std::uint64_t count_in_end = static_cast<std::uint64_t>(spb * 4.0);  // 16 steps
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  std::uint32_t counted = 0;
  std::uint32_t wrapped = 0;
  const std::uint64_t end = count_in_end + static_cast<std::uint64_t>(spb * 0.5);
  for (std::uint64_t t = 0; t < end; t += 64) {
    (void)s.step(t, 64, out, audio::kMaxEventsPerBlock);
    const audio::ClockPos c = s.clock_at(t + 64);
    wrapped += (c.flags & kWrapped) != 0u ? 1u : 0u;
    CHECK((c.flags & kPlaying) != 0u);
    if (s.position().count_in != 0u) {
      CHECK_MESSAGE(c.beat_q16 == 2u * 65536u, "count-in block at " << t << ": " << beat_of(c));
      ++counted;
    }
  }
  CHECK(counted > 1000u);
  CHECK(wrapped == 0u);
  const audio::ClockPos last = s.clock_at(end);
  CHECK(std::fabs(beat_of(last) - 2.5) < 1e-3);  // half a beat after the start point
}
