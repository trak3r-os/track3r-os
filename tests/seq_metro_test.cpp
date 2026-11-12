// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-METRO (docs/specs/OS-METRO.md section 3 and 5), the sequencer's side, on the host and on the Cortex-M4F (`qemu_seq_model`):
//   * the Metro event: nothing while the metronome is off (seq_param::Metro), a beat on every 4th step with the first of a bar accented, at the sample of the step it belongs to (swing and tempo
//     changes move the step and the beat together), counted from the pattern's start, wrapping with the pattern;
//   * the count-in (OS-COUNTIN): the transport counts N bars ON the start point (the play line waits there, nothing of the pattern plays), the Step events of those bars are flagged kEvCountIn and
//     carry the start point, the position reports the steps left and the step of the start point, the start point is the first step played, at the exact sample the count-in ends, a stop ends it,
//     a later start (or a song, or a resume) has none;
//   * the command ring carries the bars in the PlayPattern command's `b`.
#include <doctest/doctest.h>

#include <o3os/seq/sequencer.h>

#include <cstdint>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;

namespace {

StaticSlot<seq::Sequencer>& g_slot = seq_slot();
Collector& g_col = seq_collector();

seq::Sequencer& make() {
  seq::Sequencer& s = g_slot.make(seq_project());
  g_col.n = 0;
  g_col.total_events = 0;
  g_col.hash = 0xCBF29CE484222325ull;
  return s;
}

// A project of one pattern (id 1) of `length` steps at 120 BPM.
void pattern_of(std::uint32_t length, std::uint32_t bpm_x100 = 12000) {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(bpm_x100) == model::Status::Ok);
  CHECK(p.create_pattern(1, length) == model::Status::Ok);
}

std::uint64_t exact_sample(std::uint64_t ticks, std::uint32_t bpm_x100) {
  const std::uint64_t n = ticks * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t d = 4u * core::kRefCyclesPerSample.num * static_cast<std::uint64_t>(bpm_x100);
  return (2u * n + d) / (2u * d);
}

void metro(seq::Sequencer& s, bool on) { CHECK(s.apply(core::ParamChange{core::param_id(core::ParamKind::Project, 0, seq::seq_param::Metro), on ? 1.0f : 0.0f, 1})); }

// The Step event of the run at the time `t` (the beat's own), or null.
const Rec* step_at(std::uint64_t t) {
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::Step && g_col.rec[i].t == t) {
      return &g_col.rec[i];
    }
  }
  return nullptr;
}

// Runs [from, to) in blocks of 64 and records the published count-in after each: `counts[i]` is the value after block i.
struct Walk {
  static constexpr std::uint32_t kMax = 4096;
  std::uint8_t counts[kMax];
  std::uint32_t blocks = 0;
};
Walk g_walk;

void walk(seq::Sequencer& s, std::uint64_t from, std::uint64_t to) {
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  g_walk.blocks = 0;
  for (std::uint64_t t = from; t < to; t += 64u) {
    const std::uint32_t c = s.step(t, 64, out, audio::kMaxEventsPerBlock);
    for (std::uint32_t i = 0; i < c; ++i) {
      g_col.add(t, out[i]);
    }
    if (g_walk.blocks < Walk::kMax) {
      g_walk.counts[g_walk.blocks++] = s.play_position().read().count_in;
    }
  }
}

}  // namespace

TEST_CASE("metro seq: no Metro event until the metronome is switched on; then one on every 4th step, the first of a bar accented, at the step's own sample") {
  pattern_of(64);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, exact_sample(48u * 20u, 12000), 64);
  CHECK(g_col.count(EventKind::Step) >= 20u);
  CHECK(g_col.count(EventKind::Metro) == 0u);  // off by default
  // On: the next run (a fresh sequencer, so the beats start at step 0).
  seq::Sequencer& s2 = make();
  metro(s2, true);
  s2.play_pattern(1);
  g_col.run(s2, 0, exact_sample(48u * 66u, 12000), 64);
  const std::uint32_t beats = g_col.count(EventKind::Metro);
  CHECK(beats == 17u);  // steps 0, 4, ..., 60 and, after the wrap, 0 again
  for (std::uint32_t b = 0; b < beats; ++b) {
    const Rec* m = g_col.nth(EventKind::Metro, b);
    NEED(m != nullptr);
    const std::uint32_t step = (b * 4u) % 64u;
    CHECK_MESSAGE(m->ev.value == static_cast<std::int32_t>(step), b);
    CHECK_MESSAGE(m->ev.a == ((step % 16u) == 0u ? 1u : 0u), b);  // the first beat of a bar
    CHECK(m->ev.flags == 0);
    CHECK(m->ev.track == audio::kNoTrack);
    CHECK_MESSAGE(m->t == exact_sample(48u * (b * 4u), 12000), b);  // a straight grid: step n at n x 48 ticks
    const Rec* st = step_at(m->t);
    NEED(st != nullptr);  // the Step event of the same step, the same sample
    CHECK(st->ev.value == m->ev.value);
  }
  // Switched off again: no more beats from the next step on.
  metro(s2, false);
  const std::uint32_t before = g_col.count(EventKind::Metro);
  g_col.run(s2, exact_sample(48u * 66u, 12000), exact_sample(48u * 90u, 12000), 64);
  CHECK(g_col.count(EventKind::Metro) == before);
}

TEST_CASE("metro seq: a pattern that is not a whole number of bars restarts the bar with the pattern; a swung or a changing tempo moves the beat with its step") {
  pattern_of(20);
  seq::Sequencer& s = make();
  metro(s, true);
  s.play_pattern(1);
  g_col.run(s, 0, exact_sample(48u * 45u, 12000), 64);
  // 20 steps: beats at 0 (accent), 4, 8, 12, 16 (not accented: the bar of 16 steps is the pattern's first), then the pattern wraps to 0 (accent).
  const std::uint32_t want_step[] = {0, 4, 8, 12, 16, 0, 4, 8, 12, 16, 0};
  const std::uint32_t want_accent[] = {1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1};
  NEED(g_col.count(EventKind::Metro) >= 11u);
  for (std::uint32_t b = 0; b < 11; ++b) {
    const Rec* m = g_col.nth(EventKind::Metro, b);
    NEED(m != nullptr);
    CHECK_MESSAGE(m->ev.value == static_cast<std::int32_t>(want_step[b]), b);
    CHECK_MESSAGE(m->ev.a == want_accent[b], b);
  }
  // Swing 66: the odd steps move, the beats (even steps, the start of a pair) do not.
  pattern_of(64);
  CHECK(seq_project().set_swing(66) == model::Status::Ok);
  seq::Sequencer& sw = make();
  metro(sw, true);
  sw.play_pattern(1);
  g_col.run(sw, 0, exact_sample(48u * 18u, 12000), 64);
  for (std::uint32_t b = 0; b < 5; ++b) {
    const Rec* m = g_col.nth(EventKind::Metro, b);
    NEED(m != nullptr);
    CHECK_MESSAGE(m->t == exact_sample(48u * (b * 4u), 12000), b);
    CHECK(step_at(m->t) != nullptr);
  }
  // A tempo change in the middle of the run: every beat is still on the Step event of its step, and the beats come faster.
  pattern_of(64);
  seq::Sequencer& tp = make();
  metro(tp, true);
  tp.play_pattern(1);
  g_col.run(tp, 0, exact_sample(48u * 14u, 12000), 64);
  CHECK(tp.apply(core::ParamChange{seq::tempo_param(), 240.0f, 1}));
  g_col.run(tp, exact_sample(48u * 14u, 12000), exact_sample(48u * 14u, 12000) + 44117u * 6u, 64);
  NEED(g_col.count(EventKind::Metro) >= 8u);
  std::uint64_t gap_before = 0;
  std::uint64_t gap_after = 0;
  for (std::uint32_t b = 0; b < g_col.count(EventKind::Metro); ++b) {
    const Rec* m = g_col.nth(EventKind::Metro, b);
    NEED(m != nullptr);
    CHECK(step_at(m->t) != nullptr);
    if (b == 1) {
      gap_before = m->t - g_col.nth(EventKind::Metro, 0)->t;
    }
    if (b == 7) {
      gap_after = m->t - g_col.nth(EventKind::Metro, 6)->t;
    }
  }
  CHECK(gap_before > 0u);
  CHECK(gap_after > 0u);
  CHECK(gap_after * 2u + 4u >= gap_before);  // at 240 BPM a beat is half as long
  CHECK(gap_after * 2u <= gap_before + 4u);
}

TEST_CASE("metro seq: a count-in of one bar waits 16 steps on the start point, and the start point is played when the last of them ends") {
  pattern_of(64);
  seq::Sequencer& s = make();
  s.play_pattern(1, 0, 1);
  const std::uint64_t end = exact_sample(48u * 40u, 12000);
  walk(s, 0, end);
  NEED(g_col.count(EventKind::Step) >= 33u);
  // The first 16 Step events wait on step 0 (OS-COUNTIN), flagged; the 17th is step 0 played, plain, and the line goes on from it.
  for (std::uint32_t n = 0; n < 16; ++n) {
    const Rec* r = g_col.nth(EventKind::Step, n);
    NEED(r != nullptr);
    CHECK_MESSAGE(r->ev.value == 0, n);
    CHECK_MESSAGE(r->ev.flags == audio::kEvCountIn, n);
    CHECK_MESSAGE(r->t == exact_sample(48u * n, 12000), n);
  }
  for (std::uint32_t n = 16; n < 32; ++n) {
    const Rec* r = g_col.nth(EventKind::Step, n);
    NEED(r != nullptr);
    CHECK_MESSAGE(r->ev.value == static_cast<std::int32_t>(n - 16u), n);
    CHECK_MESSAGE(r->ev.flags == 0, n);
    CHECK_MESSAGE(r->t == exact_sample(48u * n, 12000), n);  // the start point at exactly 16 steps
  }
  // The position: 16 steps left in the first step, 1 in the last, 0 from the start point, and never up.
  std::uint32_t first_zero = 0;
  for (std::uint32_t b = 0; b < g_walk.blocks; ++b) {
    if (g_walk.counts[b] == 0u) {
      first_zero = b;
      break;
    }
  }
  CHECK(first_zero > 0u);
  CHECK(g_walk.counts[0] == 16u);
  CHECK(g_walk.counts[first_zero - 1u] == 1u);
  for (std::uint32_t b = 1; b < g_walk.blocks; ++b) {
    CHECK((g_walk.counts[b] <= g_walk.counts[b - 1u] || g_walk.counts[b - 1u] == 0u));
    if (b >= first_zero) {
      CHECK(g_walk.counts[b] == 0u);
    }
  }
  // The count-in ends with the last microstep of its last step (the step cursor is already the start point's, as the published step is for any step): the block that holds it is the first to publish 0.
  const std::uint64_t last_us = exact_sample(48u * 16u - 1u, 12000);
  CHECK(static_cast<std::uint64_t>(first_zero) * 64u <= last_us);
  CHECK(static_cast<std::uint64_t>(first_zero + 1u) * 64u > last_us);
  CHECK(s.position().count_in == 0u);
}

TEST_CASE("metro seq: two bars, a start step other than 0, a pattern that is not a bar long, a short pattern that loops, and bars past 2") {
  // Two bars from step 0 of 64: 32 steps waiting on 0, flagged, then 0 played and 1.
  pattern_of(64);
  seq::Sequencer& s = make();
  s.play_pattern(1, 0, 2);
  g_col.run(s, 0, exact_sample(48u * 34u, 12000), 64);
  for (std::uint32_t n = 0; n < 32; ++n) {
    CHECK_MESSAGE(g_col.nth(EventKind::Step, n)->ev.value == 0, n);
    CHECK_MESSAGE(g_col.nth(EventKind::Step, n)->ev.flags == audio::kEvCountIn, n);
  }
  CHECK(g_col.nth(EventKind::Step, 32)->ev.value == 0);
  CHECK(g_col.nth(EventKind::Step, 32)->ev.flags == 0);
  CHECK(g_col.nth(EventKind::Step, 33)->ev.value == 1);
  // From step 8, one bar: 16 steps waiting on 8, flagged, then 8.
  seq::Sequencer& s2 = make();
  s2.play_pattern(1, 8, 1);
  g_col.run(s2, 0, exact_sample(48u * 18u, 12000), 64);
  for (std::uint32_t n = 0; n < 16; ++n) {
    CHECK_MESSAGE(g_col.nth(EventKind::Step, n)->ev.value == 8, n);
    CHECK_MESSAGE(g_col.nth(EventKind::Step, n)->ev.flags == audio::kEvCountIn, n);
  }
  CHECK(g_col.nth(EventKind::Step, 16)->ev.value == 8);
  CHECK(g_col.nth(EventKind::Step, 16)->ev.flags == 0);
  // A pattern of 40 steps, one bar: 16 steps waiting on 0, flagged, then 0.
  pattern_of(40);
  seq::Sequencer& s3 = make();
  s3.play_pattern(1, 0, 1);
  g_col.run(s3, 0, exact_sample(48u * 18u, 12000), 64);
  for (std::uint32_t n = 0; n < 16; ++n) {
    CHECK_MESSAGE(g_col.nth(EventKind::Step, n)->ev.value == 0, n);
    CHECK_MESSAGE(g_col.nth(EventKind::Step, n)->ev.flags == audio::kEvCountIn, n);
  }
  CHECK(g_col.nth(EventKind::Step, 16)->ev.value == 0);
  CHECK(g_col.nth(EventKind::Step, 16)->ev.flags == 0);
  // A pattern of 8 steps, two bars: the count-in is 32 steps all the same (the pattern does not loop in it), then the start point.
  pattern_of(8);
  seq::Sequencer& s4 = make();
  s4.play_pattern(1, 0, 2);
  g_col.run(s4, 0, exact_sample(48u * 34u, 12000), 64);
  for (std::uint32_t n = 0; n < 32; ++n) {
    CHECK_MESSAGE(g_col.nth(EventKind::Step, n)->ev.value == 0, n);
    CHECK_MESSAGE(g_col.nth(EventKind::Step, n)->ev.flags == audio::kEvCountIn, n);
  }
  CHECK(g_col.nth(EventKind::Step, 32)->ev.value == 0);
  CHECK(g_col.nth(EventKind::Step, 32)->ev.flags == 0);
  // More than two bars is two.
  pattern_of(64);
  seq::Sequencer& s5 = make();
  s5.play_pattern(1, 0, 9);
  g_col.run(s5, 0, exact_sample(48u * 34u, 12000), 64);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.flags == audio::kEvCountIn);
  CHECK(g_col.nth(EventKind::Step, 31)->ev.flags == audio::kEvCountIn);
  CHECK(g_col.nth(EventKind::Step, 32)->ev.flags == 0);
}

TEST_CASE("metro seq: the click counts in with the bar of its own: its first beat is accented whatever the pattern's length; with the metronome off there is no click, only the flags") {
  pattern_of(40);
  seq::Sequencer& s = make();
  metro(s, true);
  s.play_pattern(1, 0, 1);
  g_col.run(s, 0, exact_sample(48u * 21u, 12000), 64);
  // The count-in is 16 steps waiting on step 0: beats at its steps 0 (the bar's first: accent), 4, 8, 12 (value: the step the line waits on); all flagged; then the start point played, a beat, accented, plain.
  NEED(g_col.count(EventKind::Metro) >= 6u);
  const std::uint32_t want_step[] = {0, 0, 0, 0, 0, 4};
  const std::uint32_t want_accent[] = {1, 0, 0, 0, 1, 0};
  for (std::uint32_t b = 0; b < 6; ++b) {
    const Rec* m = g_col.nth(EventKind::Metro, b);
    NEED(m != nullptr);
    CHECK_MESSAGE(m->ev.value == static_cast<std::int32_t>(want_step[b]), b);
    CHECK_MESSAGE(m->ev.a == want_accent[b], b);
    CHECK_MESSAGE(m->ev.flags == (b < 4 ? audio::kEvCountIn : 0), b);
    CHECK_MESSAGE(m->t == exact_sample(48u * (b * 4u), 12000), b);
  }
  // Two bars: the second bar's first beat is a bar's too.
  pattern_of(64);
  seq::Sequencer& s2 = make();
  metro(s2, true);
  s2.play_pattern(1, 0, 2);
  g_col.run(s2, 0, exact_sample(48u * 33u, 12000), 64);
  NEED(g_col.count(EventKind::Metro) >= 8u);
  for (std::uint32_t b = 0; b < 8; ++b) {
    CHECK_MESSAGE(g_col.nth(EventKind::Metro, b)->ev.a == ((b % 4u) == 0u ? 1u : 0u), b);
    CHECK_MESSAGE(g_col.nth(EventKind::Metro, b)->ev.value == 0, b);
  }
  // Off: the Step events still carry the flag (the visual count-in and a take that waits for the start point need it), and there is no Metro event.
  seq::Sequencer& s3 = make();
  s3.play_pattern(1, 0, 1);
  g_col.run(s3, 0, exact_sample(48u * 20u, 12000), 64);
  CHECK(g_col.count(EventKind::Metro) == 0u);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.flags == audio::kEvCountIn);
}

TEST_CASE("metro seq: a stop ends the count-in, a later start has none, a song and a resume have none, and the command ring carries the bars in `b`") {
  pattern_of(64);
  seq::Sequencer& s = make();
  s.play_pattern(1, 0, 2);
  walk(s, 0, exact_sample(48u * 5u, 12000));
  CHECK(g_walk.counts[g_walk.blocks - 1u] > 0u);
  CHECK(s.position().count_in > 0u);
  s.stop();
  walk(s, exact_sample(48u * 5u, 12000), exact_sample(48u * 5u, 12000) + 640u);
  CHECK(g_walk.counts[g_walk.blocks - 1u] == 0u);
  CHECK(s.position().count_in == 0u);
  CHECK_FALSE(s.playing());
  // The bars asked for once are not asked for again: a plain play starts at step 0, unflagged.
  g_col.n = 0;
  const std::uint64_t t0 = exact_sample(48u * 6u, 12000);
  s.play_pattern(1);
  g_col.run(s, t0, t0 + exact_sample(48u * 3u, 12000), 64);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.value == 0);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.flags == 0);
  // A play that replaces the bars asked for before it ran has none.
  seq::Sequencer& s2 = make();
  s2.play_pattern(1, 0, 2);
  s2.play_pattern(1);
  g_col.run(s2, 0, exact_sample(48u * 3u, 12000), 64);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.value == 0);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.flags == 0);
  // A resume after a count-in picks the position up with none.
  seq::Sequencer& s3 = make();
  s3.play_pattern(1, 0, 1);
  g_col.run(s3, 0, exact_sample(48u * 20u, 12000), 64);
  s3.stop();
  g_col.run(s3, exact_sample(48u * 20u, 12000), exact_sample(48u * 20u, 12000) + 640u, 64);
  g_col.n = 0;
  s3.resume();
  const std::uint64_t t1 = exact_sample(48u * 20u, 12000) + 640u;
  g_col.run(s3, t1, t1 + exact_sample(48u * 3u, 12000), 64);
  CHECK(g_col.count(EventKind::Step) >= 2u);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.flags == 0);
  CHECK(s3.position().count_in == 0u);
  // The command: PlayPattern with the bars in b.
  seq::Sequencer& s4 = make();
  CHECK(s4.commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 1, 0}));
  g_col.run(s4, 0, exact_sample(48u * 18u, 12000), 64);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.value == 0);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.flags == audio::kEvCountIn);
  CHECK(g_col.nth(EventKind::Step, 16)->ev.value == 0);
  CHECK(g_col.nth(EventKind::Step, 16)->ev.flags == 0);
  seq::Sequencer& s5 = make();
  CHECK(s5.commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0}));  // b = 0: as before
  g_col.run(s5, 0, exact_sample(48u * 3u, 12000), 64);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.value == 0);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.flags == 0);
}

// Review finding (OS-METRO; from OS-COUNTIN the count-in waits on the start point, but a start point other than 0 would still send SPP + Continue, which only a follower that honours a Song Position
// Pointer takes for a start). A counted run is a fresh run: MIDI Start, no SPP, no Continue, whatever step it starts on. A plain start from a step other than 0 keeps SPP + Continue.
TEST_CASE("metro seq: a counted run with the MIDI clock out announces itself with Start, not with SPP and Continue, from any start point; a plain start from step 48 still sends SPP and Continue") {
  seq::Sequencer::Config cfg;
  cfg.emit_clock = true;
  const std::uint64_t end = exact_sample(48u * 20u, 12000);
  for (std::uint32_t bars = 1; bars <= 2; ++bars) {
    pattern_of(64);
    seq::Sequencer& s = make();
    s.configure(cfg);
    s.play_pattern(1, 0, bars);
    g_col.run(s, 0, end, 64);
    CHECK_MESSAGE(g_col.count(EventKind::MidiStart) == 1u, bars);
    CHECK_MESSAGE(g_col.count(EventKind::MidiSpp) == 0u, bars);
    CHECK_MESSAGE(g_col.count(EventKind::MidiContinue) == 0u, bars);
    CHECK_MESSAGE(g_col.count(EventKind::MidiClock) > 0u, bars);
    const Rec* start = g_col.nth(EventKind::MidiStart, 0);
    NEED(start != nullptr);
    CHECK_MESSAGE(start->t == 0u, bars);  // with the count-in's first step, before its first clock
    CHECK(g_col.nth(EventKind::Step, 0)->ev.value == 0);
  }
  // A start point other than 0 (the app counts in from row 1 only, the API allows it): Start all the same, no SPP.
  pattern_of(64);
  seq::Sequencer& s8 = make();
  s8.configure(cfg);
  s8.play_pattern(1, 8, 1);
  g_col.run(s8, 0, end, 64);
  CHECK(g_col.count(EventKind::MidiStart) == 1u);
  CHECK(g_col.count(EventKind::MidiSpp) == 0u);
  CHECK(g_col.count(EventKind::MidiContinue) == 0u);
  // The start point 0 of a short pattern: Start as before.
  pattern_of(16);
  seq::Sequencer& s16 = make();
  s16.configure(cfg);
  s16.play_pattern(1, 0, 1);
  g_col.run(s16, 0, end, 64);
  CHECK(g_col.count(EventKind::MidiStart) == 1u);
  CHECK(g_col.count(EventKind::MidiSpp) == 0u);
  // No count-in, a start from step 48: SPP 48, then Continue, no Start (a locate, as before).
  pattern_of(64);
  seq::Sequencer& sp = make();
  sp.configure(cfg);
  sp.play_pattern(1, 48);
  g_col.run(sp, 0, end, 64);
  CHECK(g_col.count(EventKind::MidiStart) == 0u);
  CHECK(g_col.count(EventKind::MidiContinue) == 1u);
  const Rec* spp = g_col.nth(EventKind::MidiSpp, 0);
  NEED(spp != nullptr);
  CHECK(spp->ev.value == 48);
}

// OS-COUNTIN: what the pre-roll played before (the pattern's last N bars: notes of its own) is gone. The play line waits on the start point, nothing of the pattern sounds in the count-in,
// the start point's own note is the first one and sounds at the exact sample the count-in ends, and the line is shown on the start point meanwhile.
TEST_CASE("metro seq: the count-in plays no note of the pattern, the play line waits on row 1, and row 1 is the first step played") {
  for (std::uint32_t bars = 1; bars <= 2; ++bars) {
    pattern_of(64);
    model::Project& p = seq_project();
    CHECK(p.set_step(1, 0, 0, make_step(60, 1)) == model::Status::Ok);    // row 1: the first note
    CHECK(p.set_step(1, 0, 1, make_step(62, 1)) == model::Status::Ok);
    CHECK(p.set_step(1, 0, 63, make_step(70, 1)) == model::Status::Ok);   // the last bar's notes: the old pre-roll played them
    CHECK(p.set_step(1, 1, 50, make_step(72, 1)) == model::Status::Ok);
    CHECK(p.set_step(1, 1, 36, make_step(73, 1)) == model::Status::Ok);
    seq::Sequencer& s = make();
    metro(s, true);
    s.play_pattern(1, 0, bars);
    const std::uint32_t steps = 16u * bars;
    const std::uint64_t start_t = exact_sample(48u * steps, 12000);
    // Block by block: the published step is row 1 and the count-in counts down, until the start point; the step never leaves row 1 in the count-in.
    audio::SeqEvent out[audio::kMaxEventsPerBlock];
    std::uint32_t seen = 0;
    for (std::uint64_t t = 0; t < start_t + exact_sample(48u * 3u, 12000); t += 64u) {
      const std::uint32_t c = s.step(t, 64, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        g_col.add(t, out[i]);
      }
      const audio::PlayPosition::Value v = s.play_position().read();
      if (t + 64u + 1000u <= start_t) {  // well inside the count-in (the count-in ends a microstep, about 115 samples, early)
        CHECK_MESSAGE(v.count_in != 0u, bars);
        CHECK_MESSAGE(v.step == 0u, bars);
        CHECK_MESSAGE(v.playing, bars);
        ++seen;
      }
    }
    CHECK(seen > 10u);
    // No note, no note-off, no track event in the count-in; the clicks are there.
    const Rec* first_note = g_col.nth(EventKind::NoteOn, 0);
    NEED(first_note != nullptr);
    CHECK_MESSAGE(first_note->t == start_t, bars);  // the start point's own note, at the sample the count-in ends
    CHECK_MESSAGE(first_note->ev.a == 60u, bars);
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      const Rec& r = g_col.rec[i];
      if (r.t < start_t && r.ev.kind != EventKind::Step && r.ev.kind != EventKind::Metro && r.ev.kind != EventKind::FxReset) {
        FAIL_CHECK("an event of the pattern in the count-in: kind " << static_cast<int>(r.ev.kind) << " at " << r.t);
      }
    }
    CHECK_MESSAGE(g_col.count(EventKind::Metro) >= 4u * bars + 1u, bars);  // the count-in's beats and row 1's own
    // The step after the start point is row 2.
    const Rec* second = g_col.nth(EventKind::NoteOn, 1);
    NEED(second != nullptr);
    CHECK_MESSAGE(second->ev.a == 62u, bars);
    CHECK_MESSAGE(second->t == exact_sample(48u * (steps + 1u), 12000), bars);
  }
}

// The take alignment (OS-METROALIGN): the engine offers a Step event to a take that waits only when it is not flagged, so a take armed before the count-in, whatever it waits for (the next step, the
// bar, the pattern's step 0), begins at the sample of row 1 played: the first unflagged Step event is step 0 at exactly the end of the count-in.
TEST_CASE("metro seq: the first Step event a waiting take can use is row 1, at the sample the count-in ends; the steps before it are flagged and carry row 1") {
  pattern_of(64);
  seq::Sequencer& s = make();
  s.play_pattern(1, 0, 1);
  g_col.run(s, 0, exact_sample(48u * 20u, 12000), 64);
  const Rec* usable = nullptr;
  for (std::uint32_t i = 0; i < g_col.n && usable == nullptr; ++i) {
    const Rec& r = g_col.rec[i];
    if (r.ev.kind == EventKind::Step && (r.ev.flags & audio::kEvCountIn) == 0u) {
      usable = &r;
    }
  }
  NEED(usable != nullptr);
  CHECK(usable->ev.value == 0);
  CHECK(usable->t == exact_sample(48u * 16u, 12000));
  CHECK((usable->ev.value % 16) == 0);  // a Bar take and a Pattern take accept it
}

// OS-PLAYCUR (docs/specs/OS-RECFLOW.md), the sequencer's side: a start at a row, and the live jump of the play line to a row.
namespace {
// Track 0 plays the note `row` on row `row` (a = the row), so a NoteOn names the row it came from.
void notes_by_row(std::uint32_t length) {
  pattern_of(length);
  model::Project& p = seq_project();
  for (std::uint32_t r = 0; r < length; ++r) {
    CHECK(p.set_step(1, 0, r, make_step(static_cast<std::uint8_t>(r), 1)) == model::Status::Ok);
  }
}
}  // namespace

TEST_CASE("playcur seq: a start from a row plays that row first, at the exact sample, with no count-in") {
  notes_by_row(64);
  seq::Sequencer& s = make();
  s.play_pattern(1, 20, 0);
  g_col.run(s, 0, exact_sample(48u * 6u, 12000), 64);
  const Rec* first = g_col.nth(EventKind::Step, 0);
  NEED(first != nullptr);
  CHECK(first->ev.value == 20);
  CHECK(first->ev.flags == 0);
  CHECK(first->t == 0u);
  for (std::uint32_t n = 0; n < 5; ++n) {
    const Rec* on = g_col.nth(EventKind::NoteOn, n);
    NEED(on != nullptr);
    CHECK_MESSAGE(on->ev.a == 20u + n, n);
    CHECK_MESSAGE(on->t == exact_sample(48u * n, 12000), n);
  }
}

TEST_CASE("playcur seq: while playing, the line jumps to the row at the next step boundary, once: the row jumped from is not repeated, the row jumped to plays once") {
  notes_by_row(64);
  for (std::uint32_t via_ring = 0; via_ring < 2; ++via_ring) {
    seq::Sequencer& s = make();
    s.play_pattern(1);
    const std::uint64_t mid = exact_sample(48u * 3u + 20u, 12000);  // inside step 3
    g_col.run(s, 0, mid, 64);
    if (via_ring != 0u) {
      CHECK(s.commands().push(seq::Command{seq::Command::Kind::LocateRow, 0, 0, 40}));
    } else {
      s.locate_row(40);
    }
    g_col.run(s, mid, exact_sample(48u * 8u, 12000), 64);
    // Rows 0..3 as played, then 40, 41, 42, 43 (never 4, never 40 twice).
    const std::uint32_t want[] = {0, 1, 2, 3, 40, 41, 42, 43};
    for (std::uint32_t n = 0; n < 8; ++n) {
      const Rec* on = g_col.nth(EventKind::NoteOn, n);
      NEED(on != nullptr);
      CHECK_MESSAGE(on->ev.a == want[n], "ring " << via_ring << " n " << n);
      CHECK_MESSAGE(on->t == exact_sample(48u * n, 12000), "ring " << via_ring << " n " << n);  // the grid is untouched: the jump is on the step boundary
    }
    CHECK(g_col.count(EventKind::NoteOn) >= 8u);
  }
  // A row past the pattern's length wraps by it; the jump to the row the line is on is a no-op for the notes (the next step plays, not the same one again).
  notes_by_row(32);
  seq::Sequencer& w = make();
  w.play_pattern(1);
  g_col.run(w, 0, exact_sample(48u * 2u + 10u, 12000), 64);
  w.locate_row(32u + 7u);
  g_col.run(w, exact_sample(48u * 2u + 10u, 12000), exact_sample(48u * 5u, 12000), 64);
  CHECK(g_col.nth(EventKind::NoteOn, 3)->ev.a == 7u);
  CHECK(g_col.nth(EventKind::NoteOn, 4)->ev.a == 8u);
}

TEST_CASE("playcur seq: a jump asked while stopped or during the count-in does nothing; Shift + Play never counts in") {
  notes_by_row(64);
  seq::Sequencer& s = make();
  s.locate_row(10);  // stopped: nothing to move
  s.play_pattern(1);
  g_col.run(s, 0, exact_sample(48u * 4u, 12000), 64);
  CHECK(g_col.nth(EventKind::NoteOn, 0)->ev.a == 0u);
  CHECK(g_col.nth(EventKind::NoteOn, 3)->ev.a == 3u);
  // The count-in: the line waits on row 1 and the jump is dropped; row 1 plays first, then row 2.
  seq::Sequencer& c = make();
  c.play_pattern(1, 0, 1);
  g_col.run(c, 0, exact_sample(48u * 5u, 12000), 64);
  c.locate_row(30);
  g_col.run(c, exact_sample(48u * 5u, 12000), exact_sample(48u * 20u, 12000), 64);
  const Rec* on0 = g_col.nth(EventKind::NoteOn, 0);
  NEED(on0 != nullptr);
  CHECK(on0->ev.a == 0u);
  CHECK(on0->t == exact_sample(48u * 16u, 12000));
  CHECK(g_col.nth(EventKind::NoteOn, 1)->ev.a == 1u);
  // The API allows a count-in on another row (the app counts in from row 1 only, never from Shift + Play): it waits on that row, then plays it.
  seq::Sequencer& f = make();
  f.play_pattern(1, 20, 1);
  g_col.run(f, 0, exact_sample(48u * 18u, 12000), 64);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.value == 20);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.flags == audio::kEvCountIn);
  CHECK(g_col.nth(EventKind::NoteOn, 0)->ev.a == 20u);
  CHECK(g_col.nth(EventKind::NoteOn, 0)->t == exact_sample(48u * 16u, 12000));
}

TEST_CASE("playcur seq: a Stop before the jump lands drops it: Resume goes on where the run stopped; an SPP locate while stopped still works") {
  notes_by_row(64);
  const std::uint64_t mid = exact_sample(48u * 3u + 20u, 12000);
  std::int32_t resumed_at[2] = {-1, -1};
  for (std::uint32_t jump = 0; jump < 2; ++jump) {
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, mid, 64);
    if (jump != 0u) {
      s.locate_row(40);
    }
    s.stop();
    g_col.run(s, mid, mid + 64u * 4u, 64);  // before the next step boundary
    CHECK_FALSE(s.playing());
    const std::uint32_t seen = g_col.count(EventKind::Step);
    s.resume();
    g_col.run(s, mid + 64u * 4u, mid + 64u * 4u + exact_sample(48u * 3u, 12000), 64);
    const Rec* st = g_col.nth(EventKind::Step, seen);
    NEED(st != nullptr);
    resumed_at[jump] = st->ev.value;
  }
  CHECK(resumed_at[0] == resumed_at[1]);  // the locate made no difference once the Stop came first
  CHECK(resumed_at[1] != 40);
  // SPP locate() while stopped: the next Resume starts at the located step.
  seq::Sequencer& l = make();
  l.locate(20);
  l.resume();
  g_col.run(l, 0, exact_sample(48u * 2u, 12000), 64);
  const Rec* first = g_col.nth(EventKind::Step, 0);
  NEED(first != nullptr);
  CHECK(first->ev.value == 20);
}

TEST_CASE("playcur seq: a jump with a note still sounding keeps its note-off: the Off step after the landing, and a Stop, still end it") {
  pattern_of(64);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 3, make_step(60, 1)) == model::Status::Ok);  // a sample note that stays open past the jump
  model::Step off = model::kEmptyStep;
  off.note = model::kNoteOff;
  CHECK(p.set_step(1, 0, 42, off) == model::Status::Ok);
  for (std::uint32_t via_stop = 0; via_stop < 2; ++via_stop) {
    seq::Sequencer& s = make();
    s.play_pattern(1);
    const std::uint64_t mid = exact_sample(48u * 3u + 20u, 12000);
    g_col.run(s, 0, mid, 64);
    CHECK(g_col.count(EventKind::NoteOn) == 1u);
    CHECK(g_col.count(EventKind::NoteOff) == 0u);
    s.locate_row(40);
    if (via_stop != 0u) {
      g_col.run(s, mid, exact_sample(48u * 5u, 12000), 64);  // landed on row 40, the note is still open (rows 40, 41 are empty)
      CHECK(g_col.count(EventKind::NoteOff) == 0u);
      s.stop();
      g_col.run(s, exact_sample(48u * 5u, 12000), exact_sample(48u * 6u, 12000), 64);
    } else {
      g_col.run(s, mid, exact_sample(48u * 10u, 12000), 64);  // rows 40, 41, 42: the Off step
      const Rec* offrec = g_col.nth(EventKind::NoteOff, 0);
      NEED(offrec != nullptr);
      CHECK(offrec->t == exact_sample(48u * 6u, 12000));  // row 42 is the third step after the landing at 48 * 4
    }
    CHECK(g_col.count(EventKind::NoteOff) == 1u);
  }
}

// OS-UXF Focus Loop (docs/specs/OS-UXF.md, row 2), the sequencer's side: the rows of the pattern playing that loop, Sequencer::set_row_loop / Command::SetRowLoop.
namespace {
// The row (the NoteOn `a` of notes_by_row) of the n-th note played.
std::uint32_t note_row(std::uint32_t n) {
  const Rec* on = g_col.nth(EventKind::NoteOn, n);
  return on != nullptr ? on->ev.a : 0xFFFFu;
}
}  // namespace

TEST_CASE("focus loop seq: a start at the first row plays the block and wraps at its last row on the exact sample, again and again; the position says so") {
  notes_by_row(32);
  for (std::uint32_t via_ring = 0; via_ring < 2; ++via_ring) {
    seq::Sequencer& s = make();
    s.play_pattern(1, 4, 0);
    if (via_ring != 0u) {
      CHECK(s.commands().push(seq::Command{seq::Command::Kind::SetRowLoop, 4, 7, 0}));
    } else {
      s.set_row_loop(4, 7);
    }
    g_col.run(s, 0, exact_sample(48u * 14u, 12000), 64);
    CHECK(s.looping_rows());
    const audio::PlayPosition::Value v = s.play_position().read();
    CHECK(v.row_looping());
    CHECK(v.row_first == 4);
    CHECK(v.row_last == 7);
    const std::uint32_t want[] = {4, 5, 6, 7, 4, 5, 6, 7, 4, 5, 6, 7, 4, 5};
    for (std::uint32_t n = 0; n < 14; ++n) {
      CHECK_MESSAGE(note_row(n) == want[n], "ring " << via_ring << " n " << n);
      const Rec* on = g_col.nth(EventKind::NoteOn, n);
      NEED(on != nullptr);
      CHECK_MESSAGE(on->t == exact_sample(48u * n, 12000), "ring " << via_ring << " n " << n);  // the wrap costs no time: the grid goes on
    }
    CHECK(g_col.count(EventKind::Pattern) >= 3u);  // one at the start, one a wrap
  }
}

TEST_CASE("focus loop seq: a block that ends on the pattern's last row wraps with the pattern's end, and one row loops on itself") {
  notes_by_row(8);
  seq::Sequencer& s = make();
  s.play_pattern(1, 5, 0);
  s.set_row_loop(5, 7);
  g_col.run(s, 0, exact_sample(48u * 7u, 12000), 64);
  const std::uint32_t want[] = {5, 6, 7, 5, 6, 7, 5};
  for (std::uint32_t n = 0; n < 7; ++n) {
    CHECK_MESSAGE(note_row(n) == want[n], n);
  }
  seq::Sequencer& one = make();
  one.play_pattern(1, 3, 0);
  one.set_row_loop(3, 3);
  g_col.run(one, 0, exact_sample(48u * 4u, 12000), 64);
  for (std::uint32_t n = 0; n < 4; ++n) {
    CHECK_MESSAGE(note_row(n) == 3u, n);
  }
}

TEST_CASE("focus loop seq: a range set while the line is before it runs up to the block, then loops; the rows before are not looped") {
  notes_by_row(32);
  seq::Sequencer& s = make();
  s.play_pattern(1, 0, 0);
  s.set_row_loop(4, 6);
  g_col.run(s, 0, exact_sample(48u * 11u, 12000), 64);
  const std::uint32_t want[] = {0, 1, 2, 3, 4, 5, 6, 4, 5, 6, 4};
  for (std::uint32_t n = 0; n < 11; ++n) {
    CHECK_MESSAGE(note_row(n) == want[n], n);
  }
}

TEST_CASE("focus loop seq: a block on a track shorter than the pattern wraps with the pattern's rows, the short track plays on at its own length") {
  notes_by_row(16);
  CHECK(seq_project().set_track_length(1, 0, 6) == model::Status::Ok);  // track 0 loops its rows 0..5 inside the 16-row pattern
  seq::Sequencer& s = make();
  s.play_pattern(1, 8, 0);
  s.set_row_loop(8, 11);
  g_col.run(s, 0, exact_sample(48u * 10u, 12000), 64);
  // Pattern rows 8..11 on a 6-step track: its steps 2, 3, 4, 5, then back to 2 (the landing seats every track at the block's first row, mod its length).
  const std::uint32_t want[] = {2, 3, 4, 5, 2, 3, 4, 5, 2, 3};
  for (std::uint32_t n = 0; n < 10; ++n) {
    CHECK_MESSAGE(note_row(n) == want[n], n);
  }
}

TEST_CASE("focus loop seq: a pattern change or a stop takes the range away; a queued pattern is what the wrap lets play") {
  notes_by_row(32);
  CHECK(seq_project().create_pattern(2, 32) == model::Status::Ok);
  for (std::uint32_t r = 0; r < 32; ++r) {
    CHECK(seq_project().set_step(2, 0, r, make_step(static_cast<std::uint8_t>(100 + r), 1)) == model::Status::Ok);
  }
  {  // switch_pattern while looping: pattern 2 from its row 0, no loop
    seq::Sequencer& s = make();
    s.play_pattern(1, 4, 0);
    s.set_row_loop(4, 7);
    g_col.run(s, 0, exact_sample(48u * 5u + 10u, 12000), 64);
    s.switch_pattern(2);
    g_col.run(s, exact_sample(48u * 5u + 10u, 12000), exact_sample(48u * 14u, 12000), 64);
    CHECK_FALSE(s.looping_rows());
    CHECK_FALSE(s.play_position().read().row_looping());
    std::uint32_t first2 = 0xFFFFu;
    for (std::uint32_t n = 0; n < g_col.count(EventKind::NoteOn) && first2 == 0xFFFFu; ++n) {
      if (note_row(n) >= 100u) {
        first2 = note_row(n);
      }
    }
    CHECK(first2 == 106u);  // OS-PATSWITCH: the switch goes on at the next row of the old pattern (the line was in row 5, so row 6), the loop's wrap does not act on it
    CHECK(note_row(g_col.count(EventKind::NoteOn) - 1u) >= 107u);  // and goes on past the old block's end
  }
  {  // a pattern queued for the pattern's end: the wrap lets it play, the loop is over
    seq::Sequencer& s = make();
    s.play_pattern(1, 4, 0);
    s.set_row_loop(4, 7);
    g_col.run(s, 0, exact_sample(48u + 10u, 12000), 64);
    s.queue_pattern(2);  // (after the start has landed: the start clears what was queued before it)
    g_col.run(s, exact_sample(48u + 10u, 12000), exact_sample(48u * 12u, 12000), 64);
    CHECK_FALSE(s.looping_rows());
    CHECK(note_row(4) == 8u);  // the line ran on past the block's last row
  }
  {  // a stop clears it, a new start does not bring it back
    seq::Sequencer& s = make();
    s.play_pattern(1, 4, 0);
    s.set_row_loop(4, 7);
    g_col.run(s, 0, exact_sample(48u * 2u, 12000), 64);
    s.stop();
    g_col.run(s, exact_sample(48u * 2u, 12000), exact_sample(48u * 3u, 12000), 64);
    CHECK_FALSE(s.looping_rows());
    CHECK_FALSE(s.play_position().read().row_looping());
    s.play_pattern(1, 4, 0);
    g_col.run(s, exact_sample(48u * 3u, 12000), exact_sample(48u * 12u, 12000), 64);
    CHECK_FALSE(s.looping_rows());
    CHECK(note_row(g_col.count(EventKind::NoteOn) - 1u) >= 8u);  // no wrap: it ran past row 7
  }
}

TEST_CASE("focus loop seq: a Break pattern inside the block is a wrap, a range past a shortened pattern is dormant, a bad range is no loop") {
  notes_by_row(16);
  CHECK(seq_project().set_step(1, 0, 5, make_step(5, 1, seq::fx::kBreakPattern, 1)) == model::Status::Ok);
  {
    seq::Sequencer& s = make();
    s.play_pattern(1, 4, 0);
    s.set_row_loop(4, 8);  // row 5 breaks: the line wraps there, not at 8
    g_col.run(s, 0, exact_sample(48u * 6u, 12000), 64);
    const std::uint32_t want[] = {4, 5, 4, 5, 4, 5};
    for (std::uint32_t n = 0; n < 6; ++n) {
      CHECK_MESSAGE(note_row(n) == want[n], n);
    }
  }
  CHECK(seq_project().set_step(1, 0, 5, make_step(5, 1)) == model::Status::Ok);  // (the break goes: this one is about the pattern's own wrap)
  {
    seq::Sequencer& s = make();
    s.play_pattern(1, 0, 0);
    s.set_row_loop(20, 25);  // past the 16 rows: dormant, the pattern plays and wraps as it always did
    g_col.run(s, 0, exact_sample(48u * 18u, 12000), 64);
    CHECK(note_row(16) == 0u);
    CHECK(note_row(17) == 1u);
  }
  {
    seq::Sequencer& s = make();
    s.play_pattern(1, 0, 0);
    s.set_row_loop(6, 3);
    CHECK_FALSE(s.looping_rows());
    s.set_row_loop(2, model::kMaxPatternLength);
    CHECK_FALSE(s.looping_rows());
    s.set_row_loop(2, 5);
    CHECK(s.looping_rows());
    s.set_row_loop(0, seq::kNoJump);
    CHECK_FALSE(s.looping_rows());
  }
}

TEST_CASE("focus loop seq: a range asked while nothing runs is ignored, and no later resume brings it back") {
  notes_by_row(32);
  seq::Sequencer& s = make();
  s.set_row_loop(4, 7);  // stopped, no start waiting
  CHECK_FALSE(s.looping_rows());
  CHECK(s.commands().push(seq::Command{seq::Command::Kind::SetRowLoop, 4, 7, 0}));  // the same through the ring, drained by a block that plays nothing
  g_col.run(s, 0, exact_sample(48u, 12000), 64);
  CHECK_FALSE(s.looping_rows());
  s.play_pattern(1, 0, 0);
  g_col.run(s, exact_sample(48u, 12000), exact_sample(48u * 3u, 12000), 64);
  s.stop();
  g_col.run(s, exact_sample(48u * 3u, 12000), exact_sample(48u * 4u, 12000), 64);
  s.resume();
  g_col.run(s, exact_sample(48u * 4u, 12000), exact_sample(48u * 16u, 12000), 64);
  CHECK_FALSE(s.looping_rows());
  CHECK(note_row(g_col.count(EventKind::NoteOn) - 1u) >= 8u);  // ran on past row 7: no wrap
}

TEST_CASE("focus loop seq: a locate by position (an SPP, locate()) takes the range away; locate_row keeps it") {
  notes_by_row(32);
  for (std::uint32_t in_place = 0; in_place < 2; ++in_place) {
    seq::Sequencer& s = make();
    s.play_pattern(1, 4, 0);
    s.set_row_loop(4, 7);
    g_col.run(s, 0, exact_sample(48u + 10u, 12000), 64);
    if (in_place != 0u) {
      s.locate_row(5);
    } else {
      s.locate(5);
    }
    g_col.run(s, exact_sample(48u + 10u, 12000), exact_sample(48u * 14u, 12000), 64);
    CHECK(s.looping_rows() == (in_place != 0u));
    CHECK(s.play_position().read().row_looping() == (in_place != 0u));
    const std::uint32_t last = note_row(g_col.count(EventKind::NoteOn) - 1u);
    if (in_place != 0u) {
      CHECK(last <= 7u);  // still wrapping inside the block
    } else {
      CHECK(last >= 8u);  // ran on past row 7
    }
  }
}

TEST_CASE("focus loop seq: in Song mode the block wraps and the song does not advance to the next slot") {
  notes_by_row(16);
  CHECK(seq_project().create_pattern(2, 16) == model::Status::Ok);
  for (std::uint32_t r = 0; r < 16; ++r) {
    CHECK(seq_project().set_step(2, 0, r, make_step(static_cast<std::uint8_t>(100 + r), 1)) == model::Status::Ok);
  }
  CHECK(seq_project().append_order(1) == model::Status::Ok);
  CHECK(seq_project().append_order(2) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_song(0, 4);
  s.set_row_loop(4, 7);
  g_col.run(s, 0, exact_sample(48u * 20u, 12000), 64);  // past the slot's 16 rows
  CHECK(s.looping_rows());
  for (std::uint32_t n = 0; n < g_col.count(EventKind::NoteOn); ++n) {
    CHECK_MESSAGE(note_row(n) == 4u + (n % 4u), n);  // slot 2 (rows 100 and up) never plays
  }
  CHECK(g_col.count(EventKind::NoteOn) >= 19u);
  s.stop();
  g_col.run(s, exact_sample(48u * 20u, 12000), exact_sample(48u * 21u, 12000), 64);
  CHECK_FALSE(s.looping_rows());
}
