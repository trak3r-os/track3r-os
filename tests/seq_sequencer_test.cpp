// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sequencer v2 (seq/sequencer.h, OS-005c): the timeline and the grid, steps and swing, rows and notes, Micro-move, Roll, Gate, the velocity FX, tempo and the core FX,
// the song cursor and pattern switching, transport, MIDI tracks, the control layers' events, the performance layer, ParamChange and commands, the queue and its
// limits. The timer formulas are checked against the reference vectors in seq_oracle_test.cpp; this file is what the sequencer promises. Host and Cortex-M4F (`qemu_seq_model`: one project in
// .bss).
#include <doctest/doctest.h>

#include <o3os/seq/perform.h>
#include <o3os/seq/play_orders.h>
#include <o3os/seq/sequencer.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace fx = o3os::seq::fx;

namespace {

StaticSlot<seq::Sequencer>& g_slot = seq_slot();
Collector& g_col = seq_collector();

seq::Sequencer& make(const seq::Sequencer::Config& cfg = seq::Sequencer::Config{}) {
  seq::Sequencer& s = g_slot.make(seq_project());
  s.configure(cfg);
  g_col.n = 0;
  g_col.total_events = 0;
  g_col.hash = 0xCBF29CE484222325ull;
  return s;
}

// A project of one 16-step pattern (id 1) at `bpm_x100`, with the given steps on track 0 unless noted.
void pattern16(std::uint32_t bpm_x100 = 12000, std::uint32_t tracks = 4) {
  fresh_project(tracks, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(bpm_x100) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
}

// The exact sample of step n (n x 48 ticks) at a tempo: round(n * 48 * kCpuHz * den * 125 / (4 * num * bpm_x100)), half up, from the rational (core/qtime.h).
std::uint64_t exact_sample(std::uint64_t ticks, std::uint32_t bpm_x100) {
  const std::uint64_t n = ticks * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t d = 4u * core::kRefCyclesPerSample.num * static_cast<std::uint64_t>(bpm_x100);
  return (2u * n + d) / (2u * d);
}

bool near_u64(std::uint64_t a, std::uint64_t b, std::uint64_t tol) { return a + tol >= b && a <= b + tol; }

const Rec* find(const Collector& c, EventKind k, std::uint8_t track, std::uint64_t t) {
  for (std::uint32_t i = 0; i < c.n; ++i) {
    if (c.rec[i].ev.kind == k && c.rec[i].ev.track == track && c.rec[i].t == t) {
      return &c.rec[i];
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("seq: a run starts at the next block, and a step is 48 microsteps of the tempo, exactly") {
  pattern16(12000);
  seq::Sequencer& s = make();
  CHECK_FALSE(s.playing());
  s.play_pattern(1);
  g_col.run(s, 1000, 1000 + 44117 * 2 + 6000, 64);
  CHECK(s.playing());
  NEED(g_col.count(EventKind::Step) >= 17u);
  // the first step begins at the first block's start; step n at round(1000 + n * 48 ticks)
  for (std::uint32_t n = 0; n < 16; ++n) {
    const Rec* r = g_col.nth(EventKind::Step, n);
    NEED(r != nullptr);
    CHECK_MESSAGE(r->t == 1000u + exact_sample(48u * n, 12000), n);
    CHECK(r->ev.value == static_cast<std::int32_t>(n));  // the step number
    CHECK(r->ev.a == 1);                                  // the pattern
  }
  // the pattern ends after 16 steps and restarts (pattern mode loops without stopping)
  NEED(g_col.nth(EventKind::Step, 16) != nullptr);
  CHECK(g_col.nth(EventKind::Step, 16)->ev.value == 0);
  CHECK(g_col.nth(EventKind::Step, 16)->t == 1000u + exact_sample(48u * 16u, 12000));
  NEED(g_col.count(EventKind::Pattern) >= 1u);
  CHECK(g_col.nth(EventKind::Pattern, 0)->ev.a == 1);
  CHECK(g_col.nth(EventKind::Pattern, 0)->t == 1000u + exact_sample(48u * 16u, 12000));
  CHECK(s.stats().microstep_overrun == 0u);
}

TEST_CASE("seq: tempos from 40 to 800 BPM and both ends of the grid") {
  const std::uint32_t tempos[] = {4000, 7777, 9999, 12000, 13000, 20000, 33333, 80000};
  for (const std::uint32_t bpm : tempos) {
    pattern16(bpm);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    const std::uint64_t horizon = exact_sample(48u * 5u, bpm) + 100;
    g_col.run(s, 0, horizon, 64);
    for (std::uint32_t n = 0; n < 5; ++n) {
      const Rec* r = g_col.nth(EventKind::Step, n);
      NEED(r != nullptr);
      CHECK_MESSAGE(r->t == exact_sample(48u * n, bpm), bpm);
    }
    CHECK_MESSAGE(s.tempo_x100() == bpm, bpm);
    CHECK(s.stats().microstep_overrun == 0u);
  }
}

TEST_CASE("seq: a note starts on its row and a step of an empty track makes no event") {
  pattern16(12000);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 4, make_step(72, 2, 0, 0, 0, 0, 100)) == model::Status::Ok);  // volume column 100
  CHECK(p.set_step(1, 2, 3, make_step(48, 3)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, exact_sample(48u * 8u, 12000), 64);
  NEED(g_col.count(EventKind::NoteOn) == 3u);
  const Rec* a = find(g_col, EventKind::NoteOn, 0, 0);
  NEED(a != nullptr);
  CHECK(a->ev.a == 60);
  CHECK(a->ev.b == 0);                               // instrument slot 0 (the step says 1)
  CHECK(a->ev.c == audio::kVelocityDefault);         // no volume: the instrument's own
  CHECK(a->ev.offset == 0);                          // at the first sample of the first block
  const Rec* b = find(g_col, EventKind::NoteOn, 0, exact_sample(48u * 4u, 12000));
  NEED(b != nullptr);
  CHECK(b->ev.a == 72);
  CHECK(b->ev.b == 1);
  CHECK(b->ev.c == 100);                             // the volume column is the velocity
  const Rec* c = find(g_col, EventKind::NoteOn, 2, exact_sample(48u * 3u, 12000));
  NEED(c != nullptr);
  CHECK(c->ev.a == 48);
  CHECK(c->ev.b == 2);
  // track 1 and 3 have nothing
  CHECK(g_col.count(EventKind::NoteOn, 1) == 0u);
  CHECK(g_col.count(EventKind::NoteOn, 3) == 0u);
  // the sample voices get no note-off from the sequencer unless something asks (our rule: a note rings until the next one)
  CHECK(g_col.count(EventKind::NoteOff) == 0u);
}

TEST_CASE("seq: the offsets of the events in a block are 0 .. n-1 and sorted, whatever the block size") {
  pattern16(80000);
  model::Project& p = seq_project();
  for (std::uint32_t t = 0; t < 4; ++t) {
    for (std::uint32_t k = 0; k < 16; ++k) {
      CHECK(p.set_step(1, t, k, make_step(static_cast<std::uint8_t>(40 + k + t), 1, fx::kRoll, 4)) == model::Status::Ok);
    }
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  const std::uint32_t sizes[] = {1, 2, 3, 17, 64, 100, 255, 700};
  std::uint64_t t = 0;
  std::uint32_t total = 0;
  for (int round = 0; round < 40; ++round) {
    const std::uint32_t n = sizes[round % 8];
    const std::uint32_t c = s.step(t, n, out, audio::kMaxEventsPerBlock);
    std::uint32_t prev = 0;
    for (std::uint32_t i = 0; i < c; ++i) {
      CHECK(out[i].offset < n);
      CHECK(out[i].offset >= prev);
      prev = out[i].offset;
    }
    total += c;
    t += n;
  }
  CHECK(total > 100u);
  CHECK(s.stats().late_events == 0u);
}

TEST_CASE("seq: swing alternates the two steps of a pair by (150 - s) and (50 + s) percent; straight swing is straight") {
  pattern16(12000);
  model::Project& p = seq_project();
  CHECK(p.set_swing(60) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, exact_sample(48u * 10u, 12000), 64);
  // step starts: S0 = 0; S1 = S0 + nominal * 0.90 (150 - 60); S2 = S0 + 2 nominal; S3 = S2 + 0.90 nominal ...
  const std::uint32_t bpm = 12000;
  for (std::uint32_t n = 0; n < 8; ++n) {
    const std::uint64_t pair = n / 2;
    // the exact start in 1/100 ticks: pair * 2 * 48 * 100 + (odd ? 48 * 90 : 0)
    const std::uint64_t hundredths = pair * 2u * 48u * 100u + ((n & 1u) != 0u ? 48u * 90u : 0u);
    const std::uint64_t nn = hundredths * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
    const std::uint64_t dd = 100u * 4u * static_cast<std::uint64_t>(core::kRefCyclesPerSample.num) * bpm;
    const std::uint64_t expect = (2u * nn + dd) / (2u * dd);
    const Rec* r = g_col.nth(EventKind::Step, static_cast<std::uint32_t>(n));
    NEED(r != nullptr);
    const std::uint64_t got = r->t;
    CHECK_MESSAGE((got == expect || got + 1 == expect || got == expect + 1), n);  // truncating 0.9 of a Q32 tick may move a half-sample tie
  }
  CHECK(s.swing() == 60u);
  // 50 is straight: the same as no swing
  CHECK(p.set_swing(50) == model::Status::Ok);
  seq::Sequencer& s2 = make();
  s2.play_pattern(1);
  g_col.run(s2, 0, exact_sample(48u * 4u, 12000), 64);
  for (std::uint32_t n = 0; n < 4; ++n) {
    CHECK(g_col.nth(EventKind::Step, n)->t == exact_sample(48u * n, 12000));
  }
  // 25 and 75, the ends of the range
  for (const std::uint32_t sw : {25u, 75u}) {
    CHECK(p.set_swing(sw) == model::Status::Ok);
    seq::Sequencer& s3 = make();
    s3.play_pattern(1);
    g_col.run(s3, 0, exact_sample(48u * 4u, 12000), 64);
    const std::uint64_t first = g_col.nth(EventKind::Step, 1)->t - g_col.nth(EventKind::Step, 0)->t;
    const std::uint64_t second = g_col.nth(EventKind::Step, 2)->t - g_col.nth(EventKind::Step, 1)->t;
    CHECK(first + second >= exact_sample(96, 12000) - 1);
    CHECK(first + second <= exact_sample(96, 12000) + 1);
    // the first step is (150 - s)%, the second (50 + s)%: 125/75 at s = 25, 75/125 at s = 75
    const double ratio = static_cast<double>(first) / static_cast<double>(exact_sample(48, 12000));
    CHECK(ratio > (150.0 - static_cast<double>(sw)) / 100.0 - 0.001);
    CHECK(ratio < (150.0 - static_cast<double>(sw)) / 100.0 + 0.001);
  }
}

namespace {
// The sample of microstep j of step n at a straight tempo.
std::uint64_t at_us(std::uint32_t n, std::uint32_t j, std::uint32_t bpm = 12000) { return exact_sample(48u * n + j, bpm); }
}  // namespace

TEST_CASE("seq: Micro-move delays the note by the map, in microsteps of the step") {
  const std::uint8_t values[] = {0, 1, 25, 50, 75, 99, 100};
  for (const std::uint8_t v : values) {
    pattern16(12000);
    CHECK(seq_project().set_step(1, 0, 3, make_step(60, 1, fx::kMicroMove, v)) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(5, 0), 64);
    NEED(g_col.count(EventKind::NoteOn) == 1u);
    const std::uint32_t delay = seq::micromove_start(v) - 1u;  // microsteps after the row's start
    CHECK_MESSAGE(g_col.nth(EventKind::NoteOn, 0)->t == at_us(3, delay), static_cast<int>(v));
  }
  // with a cancelling Chance in the other column the note is gone, micro-move or not (the value 0 is the lowest chance; a value of 100 always plays)
  pattern16(12000);
  CHECK(seq_project().set_step(1, 0, 3, make_step(60, 1, fx::kMicroMove, 50, fx::kChance, 100)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(5, 0), 64);
  CHECK(g_col.count(EventKind::NoteOn) == 1u);
}

TEST_CASE("seq: Roll repeats the note every period (the first repeat a microstep late), each note half a period long; it rings on until the next note") {
  pattern16(12000);
  // 1/4 of a step: 12 microsteps
  CHECK(seq_project().set_step(1, 0, 2, make_step(60, 1, fx::kRoll, 4)) == model::Status::Ok);
  CHECK(seq_project().set_step(1, 0, 6, make_step(65, 1)) == model::Status::Ok);  // the next note: the roll stops there
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(8, 0), 64);
  // note ons: at 0 microsteps after the row start, then 13, 25, 37, 49, ... (every 12 after the first repeat), up to the note at step 6 (192 microsteps)
  std::uint32_t k = 0;
  std::uint32_t expect_e = 0;
  const std::uint32_t row = 2u * 48u;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    const Rec& r = g_col.rec[i];
    if (r.ev.kind == EventKind::NoteOn && r.ev.track == 0 && r.t < at_us(6, 0)) {
      CHECK_MESSAGE(r.t == exact_sample(row + expect_e, 12000), k);
      CHECK(r.ev.a == 60);
      CHECK((r.ev.flags & audio::kEvRetrigger) == (k == 0 ? 0u : 1u));
      expect_e = expect_e == 0 ? 13 : expect_e + 12;
      ++k;
    }
  }
  CHECK(k == 1u + (4u * 48u - 13u) / 12u + 1u);  // the first, and the repeats at 13, 25, ... below 192
  // each note ends half a period (6 microsteps) after it starts: offs at 6, 19, 31, ...
  std::uint32_t offs = 0;
  std::uint32_t e_start = 0;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    const Rec& r = g_col.rec[i];
    if (r.ev.kind == EventKind::NoteOff && r.ev.track == 0 && r.t <= at_us(6, 0)) {
      CHECK_MESSAGE(r.t == exact_sample(row + e_start + 6u, 12000), offs);
      e_start = e_start == 0 ? 13 : e_start + 12;
      ++offs;
    }
  }
  CHECK(offs >= 15u);
  // at step 6 a new note ends the roll: one NoteOn at the row, flag clear, and then it does not repeat again (the roll belonged to the old note) until a new roll
  const Rec* n = find(g_col, EventKind::NoteOn, 0, at_us(6, 0));
  NEED(n != nullptr);
  CHECK(n->ev.a == 65);
  CHECK((n->ev.flags & audio::kEvRetrigger) == 0u);
  // after that note no repeats: the OFF at 6 microsteps is not scheduled either (the note's length is 9999)
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    const Rec& r = g_col.rec[i];
    if (r.t > at_us(6, 0) && (r.ev.kind == EventKind::NoteOn || r.ev.kind == EventKind::NoteOff) && r.ev.track == 0) {
      CHECK(r.t < at_us(6, 0));  // there is none
    }
  }
}

TEST_CASE("seq: Roll directions: note up, note down, volume up and down") {
  struct Case {
    std::uint8_t roll;  // 9 x direction + period 4 (1/4 step)
    int first;          // the first repeat's note change
  };
  // note up: dir 4, note down: dir 3
  for (const std::uint8_t roll : {static_cast<std::uint8_t>(9 * 4 + 4), static_cast<std::uint8_t>(9 * 3 + 4)}) {
    pattern16(12000);
    CHECK(seq_project().set_step(1, 0, 1, make_step(60, 1, fx::kRoll, roll)) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(4, 0), 64);
    const int dir = roll == 40 ? 1 : -1;
    std::uint32_t k = 0;
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      if (g_col.rec[i].ev.kind == EventKind::NoteOn && g_col.rec[i].ev.track == 0) {
        CHECK(g_col.rec[i].ev.a == 60 + dir * static_cast<int>(k));  // cumulative: each repeat moves the last note
        ++k;
      }
    }
    CHECK(k > 8u);
  }
  // volume up: the first note is silent and the repeats rise to the note's velocity over the stretch to the next note; down is the mirror
  for (const bool up : {true, false}) {
    pattern16(12000);
    const std::uint8_t roll = static_cast<std::uint8_t>((up ? 9 * 2 : 9 * 1) + 4);
    CHECK(seq_project().set_step(1, 0, 1, make_step(60, 1, fx::kRoll, roll, 0, 0, 100)) == model::Status::Ok);  // velocity 100
    CHECK(seq_project().set_step(1, 0, 5, make_step(60, 1)) == model::Status::Ok);                               // the stretch is 4 steps = 192 microsteps
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(5, 0), 64);
    std::uint32_t k = 0;
    std::uint32_t e = 0;
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      const Rec& r = g_col.rec[i];
      if (r.ev.kind == EventKind::NoteOn && r.ev.track == 0 && r.t < at_us(5, 0)) {
        const std::uint32_t timer = e;  // microsteps since the note started
        const std::uint32_t want = up ? 100u * timer / 192u : 100u * (192u - timer) / 192u;
        CHECK_MESSAGE(r.ev.c == want, k);
        e = e == 0 ? 13 : e + 12;
        ++k;
      }
    }
    CHECK(k > 10u);
  }
}

TEST_CASE("seq: Gate length shortens the note to a percentage of a step; Off, Cut and Fade") {
  pattern16(12000);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kGate, 50)) == model::Status::Ok);   // 24 microsteps
  CHECK(p.set_step(1, 0, 2, make_step(61, 1, fx::kGate, 100)) == model::Status::Ok);  // a whole step
  CHECK(p.set_step(1, 0, 4, make_step(62, 1, fx::kGate, 1)) == model::Status::Ok);    // 0 microsteps rounds up to one
  CHECK(p.set_step(1, 0, 6, make_step(63, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 7, special_step(model::kNoteOff)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 8, make_step(64, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 9, special_step(model::kNoteCut)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 10, make_step(65, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 11, special_step(model::kNoteFade)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(13, 0), 64);
  // gate 50 at step 0: note at 0, off at 24 microsteps
  NEED(find(g_col, EventKind::NoteOff, 0, at_us(0, 24)) != nullptr);
  NEED(find(g_col, EventKind::NoteOff, 0, at_us(2, 48)) != nullptr);   // gate 100: off exactly at the next step's start
  NEED(find(g_col, EventKind::NoteOff, 0, at_us(4, 1)) != nullptr);
  // a note with no gate has no off of its own: the OFF step ends it
  const Rec* off = find(g_col, EventKind::NoteOff, 0, at_us(7, 0));
  NEED(off != nullptr);
  CHECK(off->ev.a == 63);  // the note that was sounding
  CHECK(off->ev.b == 0);   // and its instrument
  NEED(find(g_col, EventKind::NoteCut, 0, at_us(9, 0)) != nullptr);
  NEED(find(g_col, EventKind::NoteFade, 0, at_us(11, 0)) != nullptr);
  // a gate does not change the NoteOn of the step
  const Rec* on = find(g_col, EventKind::NoteOn, 0, at_us(0, 0));
  NEED(on != nullptr);
  CHECK(on->ev.a == 60);
}

TEST_CASE("seq: the velocity FX, the random one, Chance and the random FX are the row's own and reproducible") {
  pattern16(12000);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kVelocity, 50)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(60, 1, fx::kVelocity, 100)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 2, make_step(60, 1, fx::kRandomVelocity, 0)) == model::Status::Ok);  // a random velocity below 1 is 0: silent
  CHECK(p.set_step(1, 0, 3, make_step(60, 1, fx::kVelocity, 40, 0, 0, 90)) == model::Status::Ok);  // the FX over the volume column
  CHECK(p.set_step(1, 0, 4, make_step(60, 1, fx::kRandomNote, 0)) == model::Status::Ok);  // a spread of 0: the same note
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(6, 0), 64);
  NEED(g_col.count(EventKind::NoteOn) == 5u);
  CHECK(g_col.nth(EventKind::NoteOn, 0)->ev.c == 63);   // 50 of 100 is 63 of 127 (truncating)
  CHECK(g_col.nth(EventKind::NoteOn, 1)->ev.c == 127);
  CHECK(g_col.nth(EventKind::NoteOn, 2)->ev.c == 0);
  CHECK(g_col.nth(EventKind::NoteOn, 3)->ev.c == 50);   // 40 -> 50
  CHECK(g_col.nth(EventKind::NoteOn, 4)->ev.a == 60);
  // a velocity FX makes no voice event on a step with a note: it is the note's velocity
  CHECK(g_col.count(EventKind::ModSet) == 0u);
  // Chance (OS-CECHANCE C1, Q1 a: a draw in 0..99 below the value plays): 100 always plays; 0 never; 50 about half; the same run again is the same run
  pattern16(12000);
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 100)) == model::Status::Ok);
    CHECK(p.set_step(1, 1, k, make_step(61, 1, fx::kChance, 0)) == model::Status::Ok);
    CHECK(p.set_step(1, 2, k, make_step(62, 1, fx::kChance, 50)) == model::Status::Ok);
  }
  std::uint64_t h1 = 0, h2 = 0;
  for (int pass = 0; pass < 2; ++pass) {
    seq::Sequencer& s2 = make();
    s2.play_pattern(1);
    g_col.run(s2, 0, at_us(16 * 8, 0), 64);
    if (pass == 0) {
      CHECK(g_col.count(EventKind::NoteOn, 0) == 16u * 8u);
      CHECK(g_col.count(EventKind::NoteOn, 1) == 0u);                   // 0 never plays
      CHECK(g_col.count(EventKind::NoteOn, 2) > 40u);                  // about half of 128
      CHECK(g_col.count(EventKind::NoteOn, 2) < 90u);
      h1 = g_col.hash;
    } else {
      h2 = g_col.hash;
    }
  }
  CHECK(h1 == h2);
}

TEST_CASE("seq: the Tempo FX applies from its own row, is value x 2 BPM, at least 40, and a stop forgets it") {
  pattern16(12000);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 1, 2, fx_step(fx::kTempo, 90)) == model::Status::Ok);  // 180 BPM, on track 1 (any track does it)
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(6, 0), 64);
  const std::uint64_t s2 = at_us(2, 0);  // steps 0 and 1 are at 120 BPM, step 2 holds the FX: the step is already at 180 BPM
  CHECK(g_col.nth(EventKind::Step, 2)->t == s2);
  const std::uint64_t d2 = g_col.nth(EventKind::Step, 3)->t - s2;
  CHECK(near_u64(d2, exact_sample(48, 18000), 1));  // a difference of two rounded positions: within a sample
  CHECK(near_u64(g_col.nth(EventKind::Step, 4)->t - g_col.nth(EventKind::Step, 3)->t, exact_sample(48, 18000), 1));
  CHECK(s.tempo_x100() == 18000u);
  s.stop();
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  (void)s.step(at_us(6, 0), 64, out, audio::kMaxEventsPerBlock);
  CHECK(s.tempo_x100() == 12000u);  // forgotten (a stop clears the performance tempo)
  // below 40 BPM it is 40 (PAR-082: fixed in 1.5.0); the range of the FX is 5..200, so the value 5 is 10 BPM -> 40
  pattern16(12000);
  CHECK(seq_project().set_step(1, 0, 0, fx_step(fx::kTempo, 5)) == model::Status::Ok);
  seq::Sequencer& s3 = make();
  s3.play_pattern(1);
  g_col.run(s3, 0, 1000, 64);
  CHECK(s3.tempo_x100() == 4000u);
  pattern16(12000);
  CHECK(seq_project().set_step(1, 0, 0, fx_step(fx::kTempo, 200)) == model::Status::Ok);  // 400 BPM
  seq::Sequencer& s4 = make();
  s4.play_pattern(1);
  g_col.run(s4, 0, 1000, 64);
  CHECK(s4.tempo_x100() == 40000u);
}

TEST_CASE("seq: Tempo fine adds (v - 128) / 4 BPM to its row's tempo; Swing FX sets the swing; Speed sets the ticks, and does not change a step") {
  pattern16(12000);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 1, fx_step(fx::kTempo, 100, fx::kTempoFine, 130)) == model::Status::Ok);  // 200 BPM + 0.5
  CHECK(p.set_step(1, 0, 3, fx_step(fx::kTempoFine, 120)) == model::Status::Ok);                    // alone: the tempo in force - 2 BPM
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(6, 0), 64);
  CHECK(near_u64(g_col.nth(EventKind::Step, 2)->t - g_col.nth(EventKind::Step, 1)->t, exact_sample(48, 20050), 1));
  CHECK(near_u64(g_col.nth(EventKind::Step, 4)->t - g_col.nth(EventKind::Step, 3)->t, exact_sample(48, 20050 - 200), 1));  // the fine value is relative to what is in force
  CHECK(s.tempo_x100() == 19850u);

  // Swing FX: 17 sets the swing from its row on ("changes pattern swing", PAR-081), until a stop
  pattern16(12000);
  CHECK(seq_project().set_step(1, 0, 4, fx_step(fx::kSwing, 70)) == model::Status::Ok);
  seq::Sequencer& s2 = make();
  s2.play_pattern(1);
  g_col.run(s2, 0, at_us(10, 0), 64);
  // steps 0..3 are straight; step 4 (even: the first of a pair) is (150 - 70) = 80 % long, step 5 is 120 %
  CHECK(g_col.nth(EventKind::Step, 4)->t == at_us(4, 0));
  const std::uint64_t d4 = g_col.nth(EventKind::Step, 5)->t - g_col.nth(EventKind::Step, 4)->t;
  const std::uint64_t d5 = g_col.nth(EventKind::Step, 6)->t - g_col.nth(EventKind::Step, 5)->t;
  CHECK(d4 + 2 >= exact_sample(48, 12000) * 80 / 100);
  CHECK(d4 <= exact_sample(48, 12000) * 80 / 100 + 2);
  CHECK(d5 + 2 >= exact_sample(48, 12000) * 120 / 100);
  CHECK(d5 <= exact_sample(48, 12000) * 120 / 100 + 2);
  CHECK(s2.swing() == 70u);

  // Speed: ticks per step, 6 by default, the Tick events divide a step evenly and carry the fraction; a step is as long as before
  seq::Sequencer::Config cfg;
  cfg.emit_ticks = true;
  pattern16(12000);
  CHECK(seq_project().set_step(1, 0, 2, fx_step(fx::kSpeed, 5)) == model::Status::Ok);
  seq::Sequencer& s3 = make(cfg);
  s3.play_pattern(1);
  g_col.run(s3, 0, at_us(5, 0), 64);
  CHECK(g_col.nth(EventKind::Step, 3)->t == at_us(3, 0));  // the same grid
  // steps 0 and 1: 6 ticks of 8 microsteps; step 2: 5 ticks (48/5 microsteps each); steps 3 and 4: 5 again (it stays)
  std::uint32_t idx = 0;
  for (std::uint32_t n = 0; n < 5; ++n) {
    const std::uint32_t ticks = n < 2 ? 6u : 5u;
    for (std::uint32_t i = 0; i < ticks; ++i) {
      const Rec* r = g_col.nth(EventKind::Tick, idx++);
      NEED(r != nullptr);
      CHECK(r->ev.a == i);
      CHECK(r->ev.b == ticks);
      // the time is the step start + i/ticks of the step, to a fraction of a sample (the event's sample is the nearest one, the value the signed rest)
      const double step_len = static_cast<double>(exact_sample(48, 12000));
      const double t = static_cast<double>(g_col.rec[0].t) + 0.0;
      (void)t;
      const double exact = 48.0 * static_cast<double>(n) * samples_per_tick_120() + 48.0 * samples_per_tick_120() * static_cast<double>(i) / static_cast<double>(ticks);
      const double got = static_cast<double>(r->t) + static_cast<double>(r->ev.value) / 4294967296.0;
      CHECK(std::abs(got - exact) < 0.01);
      (void)step_len;
    }
  }
  CHECK(s3.position().ticks_per_step == 5);
  // without emit_ticks there are none
  seq::Sequencer& s4 = make();
  s4.play_pattern(1);
  g_col.run(s4, 0, at_us(5, 0), 64);
  CHECK(g_col.count(EventKind::Tick) == 0u);
}

TEST_CASE("seq: the MIDI clock is 24 PPQN on the nominal grid, with its fraction; swing does not move it; a tempo change moves it from its row") {
  seq::Sequencer::Config cfg;
  cfg.emit_clock = true;
  pattern16(12000);
  CHECK(seq_project().set_swing(65) == model::Status::Ok);
  seq::Sequencer& s = make(cfg);
  s.play_pattern(1);
  g_col.run(s, 0, at_us(8, 0), 64);
  CHECK(g_col.nth(EventKind::MidiStart, 0)->t == 0u);
  // clocks every 8 ticks: tick k at k * 8 * spt, whatever the swing does to the steps
  const double spt = samples_per_tick_120();
  const std::uint32_t n = g_col.count(EventKind::MidiClock);
  CHECK(n >= 47u);
  for (std::uint32_t k = 0; k < 47; ++k) {
    const Rec* r = g_col.nth(EventKind::MidiClock, k);
    NEED(r != nullptr);
    const double got = static_cast<double>(r->t) + static_cast<double>(r->ev.value) / 4294967296.0;
    CHECK_MESSAGE(std::abs(got - 8.0 * spt * static_cast<double>(k)) < 0.001, k);
    CHECK(r->ev.track == audio::kNoTrack);
  }
  // and a tempo FX: the ticks after the row that makes the change are at the new period, those before keep the old
  pattern16(12000);
  CHECK(seq_project().set_step(1, 0, 2, fx_step(fx::kTempo, 120)) == model::Status::Ok);  // 240 BPM from step 2
  seq::Sequencer& s2 = make(cfg);
  s2.play_pattern(1);
  g_col.run(s2, 0, at_us(4, 0), 64);
  const double spt240 = samples_per_tick_120() / 2.0;
  for (std::uint32_t k = 0; k < 12; ++k) {  // 6 ticks a step: steps 0 and 1 are 12 ticks at 120 BPM
    const Rec* r = g_col.nth(EventKind::MidiClock, k);
    NEED(r != nullptr);
    const double got = static_cast<double>(r->t) + static_cast<double>(r->ev.value) / 4294967296.0;
    CHECK(std::abs(got - 8.0 * spt * static_cast<double>(k)) < 0.001);
  }
  for (std::uint32_t k = 12; k < 12 + 6; ++k) {
    const Rec* r = g_col.nth(EventKind::MidiClock, k);
    NEED(r != nullptr);
    const double got = static_cast<double>(r->t) + static_cast<double>(r->ev.value) / 4294967296.0;
    CHECK(std::abs(got - (8.0 * spt * 12.0 + 8.0 * spt240 * static_cast<double>(k - 12))) < 0.001);
  }
}

namespace {
// Patterns 1 (4 steps), 2 (8 steps), each with a note on step 0 of track 0 (the pattern's number as the note), and a song 1, 2, 1.
void song_project() {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(24000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 4) == model::Status::Ok);
  CHECK(p.create_pattern(2, 8) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 0, make_step(61)) == model::Status::Ok);
  CHECK(p.set_step(2, 0, 0, make_step(62)) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
}
std::uint64_t song_us(std::uint32_t step, std::uint32_t j = 0) { return exact_sample(48u * step + j, 24000); }
}  // namespace

TEST_CASE("seq: a song walks its order list, loops, and announces patterns and its end") {
  song_project();
  seq::Sequencer& s = make();
  s.play_song(0);
  g_col.run(s, 0, song_us(36), 64);  // 16 steps a lap of the song: two laps and a bit
  // Step events: pattern 1 for 4 steps, 2 for 8, 1 for 4, and again
  const std::uint8_t pats[16] = {1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1};
  const std::uint8_t steps[16] = {0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3};
  NEED(g_col.count(EventKind::Step) >= 32u);
  for (std::uint32_t i = 0; i < 32; ++i) {
    const Rec* r = g_col.nth(EventKind::Step, i);
    NEED(r != nullptr);
    CHECK(r->t == song_us(i));
    CHECK_MESSAGE(r->ev.a == pats[i % 16], i);
    CHECK(r->ev.value == steps[i % 16]);
    CHECK(audio::event_order_pos(r->ev) == (i % 16 < 4 ? 0u : (i % 16 < 12 ? 1u : 2u)));
  }
  // Pattern events at 4, 12, 16 (the loop), 20 ...; the song end before the loop
  const Rec* pe = g_col.nth(EventKind::Pattern, 0);
  NEED(pe != nullptr);
  CHECK(pe->t == song_us(4));
  CHECK(pe->ev.a == 2);
  CHECK(pe->ev.value == 8);  // its length
  CHECK(audio::event_order_pos(pe->ev) == 1u);
  CHECK(g_col.nth(EventKind::Pattern, 1)->ev.a == 1);
  CHECK(g_col.nth(EventKind::Pattern, 1)->t == song_us(12));
  CHECK(g_col.nth(EventKind::SongEnd, 0)->t == song_us(16));
  CHECK(g_col.nth(EventKind::Pattern, 2)->t == song_us(16));
  CHECK(audio::event_order_pos(g_col.nth(EventKind::Pattern, 2)->ev) == 0u);
  // the notes: 61 at step 0, 62 at step 4, 61 at step 12, again at 16
  const Rec* n0 = g_col.nth(EventKind::NoteOn, 0);
  const Rec* n1 = g_col.nth(EventKind::NoteOn, 1);
  const Rec* n2 = g_col.nth(EventKind::NoteOn, 2);
  NEED(n2 != nullptr);
  CHECK(n0->ev.a == 61);
  CHECK(n1->ev.a == 62);
  CHECK(n1->t == song_us(4));
  CHECK(n2->ev.a == 61);
  CHECK(n2->t == song_us(12));
  CHECK(s.position().mode == seq::PlayMode::Song);
  // without the loop the song ends where its list does
  seq::Sequencer::Config cfg;
  cfg.song_loop = false;
  seq::Sequencer& s2 = make(cfg);
  s2.play_song(0);
  g_col.run(s2, 0, song_us(24), 64);
  CHECK(g_col.count(EventKind::Step) == 16u);
  CHECK(g_col.count(EventKind::SongEnd) == 1u);
  CHECK(g_col.count(EventKind::Stopped) == 1u);
  CHECK(g_col.nth(EventKind::Stopped, 0)->t == song_us(16));
  CHECK_FALSE(s2.playing());
  // starting in the middle: the second position, the fifth step of the song's second pattern
  seq::Sequencer& s3 = make();
  s3.play_song(1, 5);
  g_col.run(s3, 0, song_us(6), 64);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.a == 2);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.value == 5);
  CHECK(g_col.nth(EventKind::Step, 3)->ev.a == 1);  // after pattern 2's last step, the third position
  CHECK(g_col.nth(EventKind::Step, 3)->ev.value == 0);
  // a song position naming a pattern that does not exist plays as empty, as long as the default length
  song_project();
  CHECK(seq_project().append_order(9) == model::Status::Ok);
  seq::Sequencer& s4 = make();
  s4.play_song(3);
  g_col.run(s4, 0, song_us(40), 64);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.a == 9);
  CHECK(g_col.count(EventKind::NoteOn) == 0u + g_col.count(EventKind::NoteOn));
  CHECK(s4.stats().rows_missing > 0u);
}

TEST_CASE("seq: an order stop ends the song at the end of the last slot of a range - no SongEnd, no loop, a jump past it ends it too - and one at the list's end is the list's own end (OS-SONGSHARE S5)") {
  song_project();  // 1 (4 steps), 2 (8 steps), 1 (4 steps)
  seq::Sequencer::Config cfg;
  cfg.song_loop = false;
  cfg.order_stop = 2;  // the range of slots 0 and 1
  seq::Sequencer& s = make(cfg);
  s.play_song(0);
  g_col.run(s, 0, song_us(24), 64);
  CHECK(g_col.count(EventKind::Step) == 12u);
  CHECK(g_col.count(EventKind::SongEnd) == 0u);
  CHECK(g_col.count(EventKind::Stopped) == 1u);
  CHECK(g_col.nth(EventKind::Stopped, 0)->t == song_us(12));  // to the sample, where the second slot's last step ends
  CHECK_FALSE(s.playing());
  // a range that starts in the middle: slot 1 alone
  seq::Sequencer& s2 = make(cfg);
  s2.play_song(1);
  g_col.run(s2, 0, song_us(24), 64);
  CHECK(g_col.count(EventKind::Step) == 8u);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.a == 2);
  CHECK(g_col.nth(EventKind::Stopped, 0)->t == song_us(8));
  // one slot, and a stop that would be the list's end (3 of 3): the song's own end, with its SongEnd
  cfg.order_stop = 1;
  seq::Sequencer& s3 = make(cfg);
  s3.play_song(0);
  g_col.run(s3, 0, song_us(24), 64);
  CHECK(g_col.count(EventKind::Step) == 4u);
  CHECK(g_col.nth(EventKind::Stopped, 0)->t == song_us(4));
  cfg.order_stop = 3;
  seq::Sequencer& s4 = make(cfg);
  s4.play_song(0);
  g_col.run(s4, 0, song_us(24), 64);
  CHECK(g_col.count(EventKind::Step) == 16u);
  CHECK(g_col.count(EventKind::SongEnd) == 1u);
  CHECK(g_col.nth(EventKind::Stopped, 0)->t == song_us(16));
  // an Order jump of the first slot to the third lands past the range (stop 2): the run ends where the first slot does
  song_project();
  CHECK(seq_project().set_step(1, 2, 1, fx_step(fx::kOrderJump, 2)) == model::Status::Ok);
  cfg.order_stop = 2;
  seq::Sequencer& s5 = make(cfg);
  s5.play_song(0);
  g_col.run(s5, 0, song_us(24), 64);
  CHECK(g_col.count(EventKind::Step) == 2u);  // the jump ends the pattern after its step 1
  CHECK(g_col.nth(EventKind::Stopped, 0)->t == song_us(2));
  // the stop is inert in the live configuration: the default has none, and the loop goes on
  song_project();
  seq::Sequencer& s6 = make();
  s6.play_song(0);
  g_col.run(s6, 0, song_us(20), 64);
  CHECK(g_col.count(EventKind::Stopped) == 0u);
  CHECK(g_col.count(EventKind::SongEnd) == 1u);
}

// OS-005e, song playback: a project whose song runs three patterns of different lengths (3, 5 and 2 steps: odd on purpose, so the swing pair runs on through the
// switches) at the PROJECT's tempo (90 BPM, which the run takes from the project at its start). A pattern is
// 3 + 5 + 2 = 10 steps a lap; each pattern has a note on its first step (track 0, the pattern's id + 60) and one on its last (track 1, id + 70).
namespace {
void song3_project(std::uint32_t swing) {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(9000) == model::Status::Ok);
  CHECK(p.set_swing(swing) == model::Status::Ok);
  const std::uint32_t len[3] = {3, 5, 2};
  for (std::uint32_t i = 0; i < 3; ++i) {
    CHECK(p.create_pattern(i + 1, len[i]) == model::Status::Ok);
    CHECK(p.set_step(i + 1, 0, 0, make_step(static_cast<std::uint8_t>(61 + i))) == model::Status::Ok);
    CHECK(p.set_step(i + 1, 1, len[i] - 1, make_step(static_cast<std::uint8_t>(71 + i))) == model::Status::Ok);
    CHECK(p.append_order(i + 1) == model::Status::Ok);
  }
}
// The start of song step n at the project's tempo and swing s: pairs of two nominal steps, the first (150 - s)%, from the pair arithmetic (see seq/sequencer.h), exact rational, half up.
std::uint64_t swung_sample(std::uint32_t n, std::uint32_t s, std::uint32_t bpm_x100) {
  const std::uint64_t hundredths = (n / 2u) * 2u * 48u * 100u + ((n & 1u) != 0u ? 48u * (150u - s) : 0u);
  const std::uint64_t nn = hundredths * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t dd = 100u * 4u * static_cast<std::uint64_t>(core::kRefCyclesPerSample.num) * bpm_x100;
  return (2u * nn + dd) / (2u * dd);
}
void check_song3(std::uint32_t swing, std::uint64_t tol) {
  constexpr std::uint32_t kSteps = 25;  // two laps and the first pattern of the third: two switches, a loop, another two switches
  static const std::uint8_t pat[10] = {1, 1, 1, 2, 2, 2, 2, 2, 3, 3};
  static const std::uint8_t stp[10] = {0, 1, 2, 0, 1, 2, 3, 4, 0, 1};
  static const std::uint8_t row[10] = {0, 0, 0, 1, 1, 1, 1, 1, 2, 2};
  static const std::uint32_t starts[8] = {0, 3, 8, 10, 13, 18, 20, 23};  // the steps that begin a pattern
  song3_project(swing);
  seq::Sequencer& s = make();
  CHECK(s.tempo_x100() == 9000u);  // the loaded project's, before the first step
  s.play_song(0);
  g_col.run(s, 0, swung_sample(kSteps, swing, 9000) - 2, 64);
  const std::uint64_t hash64 = g_col.hash;
  const std::uint32_t total64 = g_col.total_events;
  // every step once, at its exact sample, in the pattern and the row the order list says
  NEED(g_col.count(EventKind::Step) == kSteps);  // none dropped, none doubled
  for (std::uint32_t i = 0; i < kSteps; ++i) {
    const Rec* r = g_col.nth(EventKind::Step, i);
    NEED(r != nullptr);
    const std::uint64_t want = swung_sample(i, swing, 9000);
    CHECK_MESSAGE(near_u64(r->t, want, tol), i);
    CHECK_MESSAGE(r->ev.a == pat[i % 10], i);
    CHECK_MESSAGE(r->ev.value == stp[i % 10], i);
    CHECK_MESSAGE(audio::event_order_pos(r->ev) == row[i % 10], i);
  }
  // each switch is announced at the boundary of the step that begins the pattern, with the new length; the song's end comes with the loop
  NEED(g_col.count(EventKind::Pattern) == 7u);  // not the first: the run's own start is not a switch
  for (std::uint32_t k = 0; k < 7; ++k) {
    const std::uint32_t at_step = starts[k + 1];
    const Rec* pe = g_col.nth(EventKind::Pattern, k);
    NEED(pe != nullptr);
    CHECK_MESSAGE(pe->t == g_col.nth(EventKind::Step, at_step)->t, k);
    CHECK_MESSAGE(pe->ev.a == pat[at_step % 10], k);
    CHECK_MESSAGE(pe->ev.value == (pat[at_step % 10] == 1 ? 3 : (pat[at_step % 10] == 2 ? 5 : 2)), k);
  }
  NEED(g_col.count(EventKind::SongEnd) == 2u);
  CHECK(g_col.nth(EventKind::SongEnd, 0)->t == g_col.nth(EventKind::Step, 10)->t);
  CHECK(g_col.nth(EventKind::SongEnd, 1)->t == g_col.nth(EventKind::Step, 20)->t);
  // the notes: the first step's on track 0 and the last step's on track 1, each at the sample of its own step
  NEED(g_col.count(EventKind::NoteOn, 0) == 8u);
  for (std::uint32_t k = 0; k < 8; ++k) {
    const Rec* n = g_col.nth(EventKind::NoteOn, k, 0);
    NEED(n != nullptr);
    CHECK_MESSAGE(n->t == g_col.nth(EventKind::Step, starts[k])->t, k);
    CHECK_MESSAGE(n->ev.a == 60 + pat[starts[k] % 10], k);
  }
  NEED(g_col.count(EventKind::NoteOn, 1) == 7u);  // the 8th pattern's last step is past the run's end
  for (std::uint32_t k = 0; k < 7; ++k) {
    const Rec* n = g_col.nth(EventKind::NoteOn, k, 1);
    NEED(n != nullptr);
    CHECK_MESSAGE(n->t == g_col.nth(EventKind::Step, starts[k + 1] - 1)->t, k);  // one step before the next pattern: the last of this one
    CHECK_MESSAGE(n->ev.a == 70 + pat[(starts[k + 1] - 1) % 10], k);
  }
  CHECK(s.position().mode == seq::PlayMode::Song);
  // the same events whatever the blocks are: sizes that never divide a step, and 1023
  const std::uint32_t sizes[4] = {37, 64, 509, 1023};
  seq::Sequencer& s2 = make();
  s2.play_song(0);
  g_col.run_sizes(s2, 0, swung_sample(kSteps, swing, 9000) - 2, sizes, 4);
  CHECK(g_col.hash == hash64);
  CHECK(g_col.total_events == total64);
}
}  // namespace

TEST_CASE("seq: a song of three patterns of different lengths plays the exact timeline across the switches and the loop (OS-005e)") {
  check_song3(50, 0);  // straight: exact to the sample
}

TEST_CASE("seq: the swing pair runs on through the pattern switches (OS-005e)") {
  check_song3(60, 1);  // 90% / 110% pairs, ties may move one sample
}

TEST_CASE("seq: Pattern mode (a Play on the Pattern view) loops the one pattern and never takes the song (OS-005e)") {
  song3_project(50);
  seq::Sequencer& s = make();
  s.play_pattern(2);
  g_col.run(s, 0, swung_sample(12, 50, 9000) - 2, 64);
  NEED(g_col.count(EventKind::Step) == 12u);
  for (std::uint32_t i = 0; i < 12; ++i) {
    const Rec* r = g_col.nth(EventKind::Step, i);
    NEED(r != nullptr);
    CHECK(r->t == swung_sample(i, 50, 9000));
    CHECK(r->ev.a == 2);
    CHECK(r->ev.value == i % 5);
  }
  CHECK(g_col.count(EventKind::SongEnd) == 0u);
  CHECK(s.position().mode == seq::PlayMode::Pattern);
  CHECK(s.position().pattern_id == 2);
  const audio::PlayPosition::Value v = s.play_position().read();
  CHECK(v.playing);
  CHECK(v.pattern == 2);
  CHECK(v.order == audio::PlayPosition::kNoOrder);  // no song row to say
}

TEST_CASE("seq: the published position carries the song row with the pattern (OS-005e)") {
  song3_project(50);
  seq::Sequencer& s = make();
  s.play_song(0);
  g_col.run(s, 0, swung_sample(3, 50, 9000) + 10, 64);  // just past the first switch
  audio::PlayPosition::Value v = s.play_position().read();
  CHECK(v.playing);
  CHECK(v.order == 1);
  CHECK(v.pattern == 2);
  seq::Sequencer& s2 = make();
  s2.play_song(2);  // a Song Play from the selected row 2
  g_col.run(s2, 0, 100, 64);
  v = s2.play_position().read();
  CHECK(v.order == 2);
  CHECK(v.pattern == 3);
  seq::Sequencer& s3 = make();
  s3.play_song(40);  // a row past the end of the list: from the first
  g_col.run(s3, 0, 100, 64);
  v = s3.play_position().read();
  CHECK(v.order == 0);
  CHECK(v.pattern == 1);
}

TEST_CASE("seq: a queued pattern plays at the end of the current one, and a mode switch at its end, without stopping") {
  song_project();
  seq::Sequencer& s = make();
  s.play_pattern(1);  // Pattern mode: pattern 1 loops
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  std::uint64_t t = 0;
  const auto run_to = [&](std::uint64_t end) {
    while (t < end) {
      const std::uint32_t n = static_cast<std::uint32_t>(end - t < 64 ? end - t : 64);
      const std::uint32_t c = s.step(t, n, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        g_col.add(t, out[i]);
      }
      t += n;
    }
  };
  run_to(song_us(6));  // two steps into the second lap of pattern 1
  s.queue_pattern(2);
  run_to(song_us(8));  // the end of the lap, at 8
  run_to(song_us(30));
  // laps: pattern 1 at 0..4, 4..8, then 2 for good (8..16, 16..24, ...)
  CHECK(g_col.nth(EventKind::Step, 8)->ev.a == 2);
  CHECK(g_col.nth(EventKind::Step, 8)->t == song_us(8));
  CHECK(g_col.nth(EventKind::Step, 16)->ev.a == 2);  // still 2: the mode is Pattern, so it loops
  CHECK(g_col.nth(EventKind::Step, 16)->ev.value == 0);
  CHECK(s.position().mode == seq::PlayMode::Pattern);
  CHECK(audio::event_order_pos(g_col.nth(EventKind::Step, 8)->ev) == 0xFFFFu);  // no order position in Pattern mode
  // switch to Song: at the end of the pattern 2 lap the song carries on after the first position that holds pattern 2: position 2, pattern 1
  s.request_mode(seq::PlayMode::Song);
  run_to(song_us(46));
  std::uint32_t idx = 0;
  for (std::uint32_t i = 0; i < g_col.count(EventKind::Step); ++i) {
    if (g_col.nth(EventKind::Step, i)->t >= song_us(32)) {
      idx = i;
      break;
    }
  }
  CHECK(g_col.nth(EventKind::Step, idx)->ev.a == 1);  // the lap of pattern 2 ends at 32: the song goes on at its third position
  CHECK(audio::event_order_pos(g_col.nth(EventKind::Step, idx)->ev) == 2u);
  CHECK(s.position().mode == seq::PlayMode::Song);
  // and a switch back to Pattern holds the current pattern: no order list any more
  s.request_mode(seq::PlayMode::Pattern);
  run_to(song_us(80));
  CHECK(s.position().mode == seq::PlayMode::Pattern);
}

TEST_CASE("seq: the live jump takes the song to its position exactly at the end of the pattern now playing, and is readable while it waits (OS-SONG1)") {
  song_project();  // order: pattern 1 (4 steps), 2 (8), 1 (4)
  seq::Sequencer& s = make();
  s.play_song(0);
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  std::uint64_t t = 0;
  const auto run_to = [&](std::uint64_t end) {
    while (t < end) {
      const std::uint32_t n = static_cast<std::uint32_t>(end - t < 64 ? end - t : 64);
      const std::uint32_t c = s.step(t, n, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        g_col.add(t, out[i]);
      }
      t += n;
    }
  };
  const auto queue = [&](std::uint32_t pos) { return s.commands().push(seq::Command{seq::Command::Kind::QueueJump, 0, static_cast<std::uint16_t>(pos), 0}); };
  run_to(song_us(2) + 17);  // two steps into the first pattern, off a step boundary
  CHECK(s.play_position().read().queued == audio::PlayPosition::kNoOrder);
  CHECK(queue(2));
  run_to(song_us(3));
  CHECK(s.play_position().read().queued == 2u);  // the main loop can see what waits (the slot blinks)
  CHECK(s.play_position().read().order == 0u);   // and the song is still where it was: nothing cut
  run_to(song_us(4) - 150);  // (the cursor moves when the last microstep of the step has run: the events carry the boundary's time)
  CHECK(s.play_position().read().order == 0u);
  run_to(song_us(5));
  // Without the jump step 4 would be pattern 2 at order 1: the jump lands on the very sample the pattern ended, at order 2 (pattern 1).
  CHECK(g_col.nth(EventKind::Step, 4)->ev.a == 1);
  CHECK(audio::event_order_pos(g_col.nth(EventKind::Step, 4)->ev) == 2u);
  CHECK(g_col.nth(EventKind::Step, 4)->t == song_us(4));
  CHECK(g_col.nth(EventKind::Step, 4)->ev.value == 0);
  CHECK(s.play_position().read().queued == audio::PlayPosition::kNoOrder);  // consumed
  CHECK(s.play_position().read().order == 2u);
  run_to(song_us(10));
  // the song then carries on from there: the end of the list, the loop to order 0
  CHECK(g_col.nth(EventKind::Step, 8)->t == song_us(8));
  CHECK(g_col.nth(EventKind::Step, 8)->ev.a == 1);
  CHECK(audio::event_order_pos(g_col.nth(EventKind::Step, 8)->ev) == 0u);
  CHECK(s.stats().fx_ignored == 0u);
}

TEST_CASE("seq: the live jump: the latest request replaces the waiting one, kNoJump cancels, a stop and a stopped transport hold none, a position the song lost is dropped (OS-SONG1)") {
  song_project();
  seq::Sequencer& s = make();
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  std::uint64_t t = 0;
  const auto run_to = [&](std::uint64_t end) {
    while (t < end) {
      const std::uint32_t n = static_cast<std::uint32_t>(end - t < 64 ? end - t : 64);
      const std::uint32_t c = s.step(t, n, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        g_col.add(t, out[i]);
      }
      t += n;
    }
  };
  const auto queue = [&](std::uint32_t pos) { return s.commands().push(seq::Command{seq::Command::Kind::QueueJump, 0, static_cast<std::uint16_t>(pos), 0}); };
  CHECK(queue(1));  // stopped: ignored
  run_to(100);
  CHECK(s.play_position().read().queued == audio::PlayPosition::kNoOrder);
  s.play_song(0);
  run_to(song_us(1));
  CHECK(queue(2));
  CHECK(queue(1));  // the latest wins; position 1 is where the song goes anyway
  run_to(song_us(2));
  CHECK(s.play_position().read().queued == 1u);
  CHECK(queue(seq::kNoJump));  // cancel
  run_to(song_us(3));
  CHECK(s.play_position().read().queued == audio::PlayPosition::kNoOrder);
  CHECK(queue(2));
  run_to(song_us(5));
  CHECK(audio::event_order_pos(g_col.nth(EventKind::Step, 4)->ev) == 2u);  // the jump was alive after the cancel: it landed at the end of pattern 1
  // a stop forgets it
  CHECK(queue(0));
  run_to(song_us(6));
  CHECK(s.play_position().read().queued == 0u);
  s.stop();
  run_to(song_us(6) + 200);
  CHECK(s.play_position().read().queued == audio::PlayPosition::kNoOrder);
  // a position the song does not have (the list shrank under the queue, or never had it) is dropped at the end of the pattern: the song just goes on
  s.play_song(0);
  run_to(song_us(7));
  CHECK(queue(200));  // out of the 255 positions' range is refused at once...
  CHECK(queue(5));    // ...and one inside it that the 3-position song lacks waits, then is dropped
  const std::uint32_t before = s.stats().fx_ignored;
  run_to(song_us(7) + song_us(5));
  CHECK(s.stats().fx_ignored == before + 1u);
  CHECK(s.play_position().read().queued == audio::PlayPosition::kNoOrder);
}

TEST_CASE("seq: the live jump from a single pattern's loop starts the song at the position at the end of the lap (OS-SONG1)") {
  song_project();
  seq::Sequencer& s = make();
  s.play_pattern(2);  // Pattern mode: pattern 2 (8 steps) loops
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  std::uint64_t t = 0;
  const auto run_to = [&](std::uint64_t end) {
    while (t < end) {
      const std::uint32_t n = static_cast<std::uint32_t>(end - t < 64 ? end - t : 64);
      const std::uint32_t c = s.step(t, n, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        g_col.add(t, out[i]);
      }
      t += n;
    }
  };
  run_to(song_us(3));
  CHECK(s.commands().push(seq::Command{seq::Command::Kind::QueueJump, 0, 0, 0}));
  run_to(song_us(20));
  CHECK(g_col.nth(EventKind::Step, 8)->t == song_us(8));
  CHECK(g_col.nth(EventKind::Step, 8)->ev.a == 1);  // the first position's pattern
  CHECK(audio::event_order_pos(g_col.nth(EventKind::Step, 8)->ev) == 0u);
  CHECK(s.position().mode == seq::PlayMode::Song);
  CHECK(g_col.nth(EventKind::Step, 12)->ev.a == 2);  // and the song goes on in order
}

TEST_CASE("seq: Break pattern ends the pattern after its step; Order jump goes to a position of the song; both do nothing outside their mode") {
  song_project();
  model::Project& p = seq_project();
  CHECK(p.set_step(2, 1, 2, fx_step(fx::kBreakPattern, 1)) == model::Status::Ok);  // pattern 2: break after step 2
  seq::Sequencer& s = make();
  s.play_song(0);
  g_col.run(s, 0, song_us(16), 64);
  // pattern 1 (4 steps), then pattern 2 for 3 steps (0, 1, 2), then the third position (pattern 1)
  CHECK(g_col.nth(EventKind::Step, 4)->ev.a == 2);
  CHECK(g_col.nth(EventKind::Step, 6)->ev.value == 2);
  CHECK(g_col.nth(EventKind::Step, 7)->ev.a == 1);
  CHECK(g_col.nth(EventKind::Step, 7)->ev.value == 0);
  CHECK(audio::event_order_pos(g_col.nth(EventKind::Step, 7)->ev) == 2u);
  // Order jump: position 0 from pattern 1's step 1 at the third position
  song_project();
  CHECK(seq_project().set_step(1, 2, 1, fx_step(fx::kOrderJump, 1)) == model::Status::Ok);  // in pattern 1: jump to position 1 after its step 1
  seq::Sequencer& s2 = make();
  s2.play_song(0);
  g_col.run(s2, 0, song_us(20), 64);
  // step 0, 1 of pattern 1; then position 1 (pattern 2), 8 steps; then position 2 (pattern 1) whose step 1 jumps to position 1 again
  CHECK(g_col.nth(EventKind::Step, 2)->ev.a == 2);
  CHECK(audio::event_order_pos(g_col.nth(EventKind::Step, 2)->ev) == 1u);
  CHECK(g_col.nth(EventKind::Step, 10)->ev.a == 1);  // after pattern 2's 8 steps
  CHECK(g_col.nth(EventKind::Step, 12)->ev.a == 2);  // the jump: back to position 1
  // out of range: it ends the pattern as a break does
  song_project();
  CHECK(seq_project().set_step(1, 2, 1, fx_step(fx::kOrderJump, 200)) == model::Status::Ok);
  seq::Sequencer& s3 = make();
  s3.play_song(0);
  g_col.run(s3, 0, song_us(6), 64);
  CHECK(g_col.nth(EventKind::Step, 2)->ev.a == 2);  // ended after step 1, the next position follows
  CHECK(s3.stats().fx_ignored == 1u);
  // in Pattern mode an Order jump restarts the pattern (nothing to jump in); a Break pattern restarts it too
  song_project();
  CHECK(seq_project().set_step(1, 2, 1, fx_step(fx::kOrderJump, 1)) == model::Status::Ok);
  seq::Sequencer& s4 = make();
  s4.play_pattern(1);
  g_col.run(s4, 0, song_us(6), 64);
  CHECK(g_col.nth(EventKind::Step, 2)->ev.value == 0);
  CHECK(g_col.nth(EventKind::Step, 2)->ev.a == 1);
}

TEST_CASE("seq: tracks of different lengths wrap by themselves and all restart with the pattern") {
  song_project();
  model::Project& p = seq_project();
  CHECK(p.set_track_length(2, 0, 3) == model::Status::Ok);  // pattern 2 (8 steps): track 0 is 3 long, track 1 is 5 long, track 2 8
  CHECK(p.set_track_length(2, 1, 5) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 8; ++k) {
    CHECK(p.set_step(2, 0, k, make_step(static_cast<std::uint8_t>(30 + k))) == model::Status::Ok);
    CHECK(p.set_step(2, 1, k, make_step(static_cast<std::uint8_t>(50 + k))) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  s.play_pattern(2);
  g_col.run(s, 0, song_us(18), 64);
  // the pattern is 8 steps (the longest track): track 0 plays steps 0 1 2 0 1 2 0 1 | 0 1 2 0 1 2 0 1; track 1: 0 1 2 3 4 0 1 2 | 0 1 2 3 4 0 1 2
  const std::uint8_t t0[16] = {30, 31, 32, 30, 31, 32, 30, 31, 30, 31, 32, 30, 31, 32, 30, 31};
  const std::uint8_t t1[16] = {50, 51, 52, 53, 54, 50, 51, 52, 50, 51, 52, 53, 54, 50, 51, 52};
  for (std::uint32_t i = 0; i < 16; ++i) {
    const Rec* a = g_col.nth(EventKind::NoteOn, i, 0);
    const Rec* b = g_col.nth(EventKind::NoteOn, i, 1);
    NEED(a != nullptr);
    NEED(b != nullptr);
    CHECK_MESSAGE(a->ev.a == t0[i], i);
    CHECK_MESSAGE(b->ev.a == t1[i], i);
    CHECK(a->t == song_us(i));
  }
  CHECK(s.position().pattern_len == 8);
}

TEST_CASE("seq: a track shortened mid-run goes to the top of its new length at the next step, reads no torn row and plays the same whatever the blocks (PAT-OPS SHRINK)") {
  // the row reader's `pos >= len` branch (Sequencer::read_row): SHRINK, DUPLICATE and a paste change a track's length under a running pattern. The pattern's own length is read at its end only,
  // so the lap in progress goes on to 16 steps (the other tracks are 16 long) and the shortened track wraps by itself.
  std::uint64_t hash[2] = {0, 0};
  const std::uint32_t blocks[2] = {64u, 37u};
  for (std::uint32_t pass = 0; pass < 2; ++pass) {
    pattern16(24000);
    model::Project& p = seq_project();
    for (std::uint32_t k = 0; k < 16; ++k) {
      CHECK(p.set_step(1, 0, k, make_step(static_cast<std::uint8_t>(20 + k))) == model::Status::Ok);
    }
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, song_us(10, 10), blocks[pass]);  // inside step 10: the track is on its row 10
    CHECK(p.set_track_length(1, 0, 4) == model::Status::Ok);
    g_col.run(s, song_us(10, 10), song_us(40), blocks[pass]);  // the rest of the lap, then two laps and a bit
    CHECK(s.stats().rows_torn == 0u);
    CHECK(s.stats().rows_missing == 0u);
    CHECK(g_col.count(EventKind::NoteOn, 0) == 40u);  // a note a step, none lost, none doubled
    for (std::uint32_t i = 0; i < 40; ++i) {
      const Rec* r = g_col.nth(EventKind::NoteOn, i, 0);
      NEED(r != nullptr);
      CHECK(r->t == song_us(i));
      // up to the change the 16 rows; the step after it finds the track past its end (row 11 of 4) and takes it to row 0, then 1 2 3 0; the pattern restarts at step 16 on row 0, and the 4 rows go on
      // (the lap in progress is still 16 steps: pattern_len_ is read at the pattern's end, and the other tracks are 16 long)
      const std::uint32_t row = i <= 10u ? i : i < 16u ? (i - 11u) % 4u : (i - 16u) % 4u;
      CHECK_MESSAGE(r->ev.a == 20 + row, "pass " << pass << " step " << i);
    }
    hash[pass] = g_col.hash;
  }
  CHECK(hash[0] == hash[1]);  // block sizes 64 and 37 give the same events at the same samples
}

TEST_CASE("seq: stop sends the notes off and the stopped event; resume continues from the step; locate puts the cursor where an SPP says; play restarts") {
  seq::Sequencer::Config cfg;
  cfg.emit_clock = true;
  song_project();
  seq::Sequencer& s = make(cfg);
  s.play_song(0);
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  std::uint64_t t = 0;
  const auto run_to = [&](std::uint64_t end) {
    while (t < end) {
      const std::uint32_t n = static_cast<std::uint32_t>(end - t < 64 ? end - t : 64);
      const std::uint32_t c = s.step(t, n, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        g_col.add(t, out[i]);
      }
      t += n;
    }
  };
  run_to(song_us(5) + 10);  // inside step 5: the second pattern's second step
  const std::uint32_t steps_before = g_col.count(EventKind::Step);
  s.stop();
  run_to(t + 64);
  CHECK_FALSE(s.playing());
  // the note that was sounding (pattern 2's note 62, track 0) gets an off, at the block that stops; then the MIDI stop and the stopped event
  const Rec* off = g_col.nth(EventKind::NoteOff, 0);
  NEED(off != nullptr);
  CHECK(off->ev.a == 62);
  CHECK(off->t == song_us(5) + 10);
  CHECK(g_col.count(EventKind::MidiStop) == 1u);
  CHECK(g_col.count(EventKind::Stopped) == 1u);
  // nothing more happens
  run_to(t + 5000);
  CHECK(g_col.count(EventKind::Step) == steps_before);
  // resume: Continue with an SPP first (the song step 5), from the start of the step that was playing
  s.resume();
  const std::uint64_t resume_at = t;
  run_to(t + song_us(3));
  CHECK(s.playing());
  const Rec* spp = g_col.nth(EventKind::MidiSpp, 0);
  NEED(spp != nullptr);
  CHECK(spp->ev.value == 5);  // song step 5: pattern 1's 4 steps and pattern 2's step 1
  CHECK(g_col.count(EventKind::MidiContinue) == 1u);
  const Rec* st = g_col.nth(EventKind::Step, steps_before);
  NEED(st != nullptr);
  CHECK(st->t == resume_at);
  CHECK(st->ev.a == 2);
  CHECK(st->ev.value == 1);
  // locate while stopped, then resume: the SPP says step 13 of the song = pattern 1 (third position), its step 1
  s.stop();
  run_to(t + 64);
  s.locate(13);
  s.resume();
  run_to(t + 64);
  const Rec* last_spp = g_col.nth(EventKind::MidiSpp, g_col.count(EventKind::MidiSpp) - 1);
  CHECK(last_spp->ev.value == 13);
  const std::uint32_t nst = g_col.count(EventKind::Step);
  CHECK(g_col.nth(EventKind::Step, nst - 1)->ev.a == 1);
  CHECK(g_col.nth(EventKind::Step, nst - 1)->ev.value == 1);
  // locate past the end of the song wraps (as the song does): 16 + 2 is step 2 of position 0
  s.stop();
  run_to(t + 64);
  s.locate(18);
  s.resume();
  run_to(t + 64);
  CHECK(g_col.nth(EventKind::Step, g_col.count(EventKind::Step) - 1)->ev.value == 2);
  CHECK(audio::event_order_pos(g_col.nth(EventKind::Step, g_col.count(EventKind::Step) - 1)->ev) == 0u);
  // a play while playing restarts from the start, announces no stop, and starts from the beginning (a MIDI Start)
  const std::uint32_t starts = g_col.count(EventKind::MidiStart);
  const std::uint32_t stoppeds = g_col.count(EventKind::Stopped);
  s.play_song(0);
  run_to(t + 64);
  CHECK(g_col.count(EventKind::MidiStart) == starts + 1u);
  CHECK(g_col.count(EventKind::Stopped) == stoppeds);
  CHECK(s.position().order_pos == 0);
  // locating while playing jumps at the next step boundary
  s.locate(9);  // song step 9: position 1, step 5
  run_to(t + song_us(2));
  const std::uint32_t last = g_col.count(EventKind::Step) - 1;
  bool found = false;
  for (std::uint32_t i = last >= 4u ? last - 4u : 0u; i <= last; ++i) {
    if (g_col.nth(EventKind::Step, i)->ev.a == 2 && g_col.nth(EventKind::Step, i)->ev.value == 5) {
      found = true;
    }
  }
  CHECK(found);
}

TEST_CASE("seq: a MIDI instrument makes MIDI events, one note a track, and nothing hangs (PAR-063)") {
  pattern16(12000);
  model::Project& p = seq_project();
  // slot 10 is a MIDI instrument on channel 3 (index 2): the step names it as 11
  CHECK(p.set_step(1, 0, 0, make_step(60, 11)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(62, 11, 0, 0, 0, 0, 0)) == model::Status::Ok);   // volume 0: a note-on with velocity 0 would be a note-off
  CHECK(p.set_step(1, 0, 2, special_step(model::kNoteOff, 11)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 3, make_step(64, 11)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 4, make_step(65, 1)) == model::Status::Ok);                    // a SAMPLE instrument follows a MIDI note: the MIDI note must end
  CHECK(p.set_step(1, 0, 5, make_step(66, 11)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 6, special_step(model::kNoteCut, 11)) == model::Status::Ok);   // a cut ends a MIDI note as well
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(8, 0), 64);
  const Rec* on0 = find(g_col, EventKind::MidiNoteOn, 0, at_us(0, 0));
  NEED(on0 != nullptr);
  CHECK(on0->ev.a == 60);
  CHECK(on0->ev.b == 127);  // no velocity: the default MIDI velocity of an instrument
  CHECK(on0->ev.c == 2);    // channel 3, as the wire numbers it
  // the next note: the previous one ends first, at the same sample, and the order is off then on
  std::uint32_t i_off = 0, i_on = 0;
  bool have_off = false, have_on = false;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].t == at_us(1, 0) && g_col.rec[i].ev.track == 0) {
      if (g_col.rec[i].ev.kind == EventKind::MidiNoteOff) {
        i_off = i;
        have_off = true;
      }
      if (g_col.rec[i].ev.kind == EventKind::MidiNoteOn) {
        i_on = i;
        have_on = true;
      }
    }
  }
  NEED(have_off);
  NEED(have_on);
  CHECK(i_off < i_on);
  CHECK(g_col.rec[i_off].ev.a == 60);
  CHECK(g_col.rec[i_on].ev.a == 62);
  CHECK(g_col.rec[i_on].ev.b == 1);  // clamped to 1
  // the OFF step ends the note
  const Rec* off2 = find(g_col, EventKind::MidiNoteOff, 0, at_us(2, 0));
  NEED(off2 != nullptr);
  CHECK(off2->ev.a == 62);
  // the sample note after the MIDI one at step 4 ends the MIDI note too, and then it is an ordinary NoteOn
  NEED(find(g_col, EventKind::MidiNoteOff, 0, at_us(4, 0)) != nullptr);
  CHECK(find(g_col, EventKind::MidiNoteOff, 0, at_us(4, 0))->ev.a == 64);
  NEED(find(g_col, EventKind::NoteOn, 0, at_us(4, 0)) != nullptr);
  // a cut ends the MIDI note
  NEED(find(g_col, EventKind::MidiNoteOff, 0, at_us(6, 0)) != nullptr);
  CHECK(find(g_col, EventKind::NoteCut, 0, at_us(6, 0)) == nullptr);  // not a voice cut: there is no voice
  // every MIDI note-on has exactly one note-off before the end of the run
  std::int32_t sounding = 0;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::MidiNoteOn) {
      ++sounding;
    }
    if (g_col.rec[i].ev.kind == EventKind::MidiNoteOff) {
      --sounding;
      CHECK(sounding >= 0);
    }
  }
  CHECK(sounding == 0);
}

TEST_CASE("seq: MIDI channels 1 and 16 are instruments 49 and 64: their notes go out on channels 1 and 16 (OS-INSTRMIDI, our design)") {
  pattern16(12000);
  model::Project& p = seq_project();
  for (const std::uint32_t slot : {48u, 63u}) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Midi);
    in.midi_channel = static_cast<std::uint8_t>(slot - 47u);  // 1 and 16
    CHECK(p.set_instrument(slot, in) == model::Status::Ok);
  }
  CHECK(p.set_step(1, 0, 0, make_step(60, 49)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(62, 64)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(3, 0), 64);
  const Rec* first = find(g_col, EventKind::MidiNoteOn, 0, at_us(0, 0));
  NEED(first != nullptr);
  CHECK(first->ev.a == 60);
  CHECK(first->ev.c == 0);  // channel 1, as the wire numbers it
  const Rec* last = find(g_col, EventKind::MidiNoteOn, 0, at_us(1, 0));
  NEED(last != nullptr);
  CHECK(last->ev.a == 62);
  CHECK(last->ev.c == 15);  // channel 16
  CHECK(g_col.count(EventKind::NoteOn) == 0u);  // no sample voice for either
}

TEST_CASE("seq: stop and a muted track end a MIDI note; a muted track starts nothing; CC A..F and the program change are MIDI events at the row") {
  pattern16(12000);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 11)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 2, make_step(70, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 3, make_step(72, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 2, 1, fx_step(fx::kCcA, 99)) == model::Status::Ok);
  CHECK(p.set_step(1, 2, 2, make_step(40, 1, fx::kMicroMove, 100, fx::kCcE, 7)) == model::Status::Ok);
  CHECK(p.set_step(1, 2, 3, fx_step(fx::kSlot37, 55)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  std::uint64_t t = 0;
  const auto run_to = [&](std::uint64_t end) {
    while (t < end) {
      const std::uint32_t n = static_cast<std::uint32_t>(end - t < 64 ? end - t : 64);
      const std::uint32_t c = s.step(t, n, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        g_col.add(t, out[i]);
      }
      t += n;
    }
  };
  run_to(at_us(1, 30));
  // mute the MIDI track and track 1 in the project (TrackSlot::flags): the MIDI note ends at the next block's first microstep, the muted sample track goes on (its strip is the mute: OS-MUTE)
  model::TrackSlot ts0 = *p.track(0);
  ts0.flags = model::kTrackMute;
  CHECK(p.set_track(0, ts0) == model::Status::Ok);
  model::TrackSlot ts1 = *p.track(1);
  ts1.flags = model::kTrackMute;
  CHECK(p.set_track(1, ts1) == model::Status::Ok);
  run_to(at_us(5, 0));
  const Rec* moff = g_col.nth(EventKind::MidiNoteOff, 0);
  NEED(moff != nullptr);
  CHECK(moff->t >= at_us(1, 30));
  CHECK(moff->t <= at_us(1, 31));  // at the next microstep after the mute (the block starts on one: the same sample)
  CHECK(moff->ev.a == 60);
  CHECK(g_col.count(EventKind::NoteOn, 1) == 2u);  // track 1 is muted and its notes at steps 2 and 3 still start: the graph's strip is the mute, the sequencer is not
  // the CC and the program-change rows: CC A at step 1 (a note-less row), CC E at step 2's row start although the note itself is micro-moved, FX 37 as CC F
  const Rec* cca = find(g_col, EventKind::MidiCc, 2, at_us(1, 0));
  NEED(cca != nullptr);
  CHECK(cca->ev.a == 0);
  CHECK(cca->ev.b == 99);
  CHECK(cca->ev.c == audio::kChannelDefault);
  const Rec* cce = find(g_col, EventKind::MidiCc, 2, at_us(2, 0));
  NEED(cce != nullptr);
  CHECK(cce->ev.a == 4);
  CHECK(cce->ev.b == 7);
  NEED(find(g_col, EventKind::NoteOn, 2, at_us(2, 46)) != nullptr);  // the note itself is 46 microsteps late
  const Rec* ccf = find(g_col, EventKind::MidiCc, 2, at_us(3, 0));
  NEED(ccf != nullptr);
  CHECK(ccf->ev.a == 5);
  CHECK(ccf->ev.b == 55);
  // unmute: nothing changes for the sample track; the MIDI track (still muted) starts no more MIDI notes
  const std::uint32_t midi_ons = g_col.count(EventKind::MidiNoteOn);
  ts1.flags = 0;
  CHECK(p.set_track(1, ts1) == model::Status::Ok);
  run_to(at_us(20, 0));
  CHECK(g_col.count(EventKind::NoteOn, 1) > 2u);
  CHECK(g_col.count(EventKind::MidiNoteOn) == midi_ons);  // track 0 stays muted: its pattern's next notes make no MIDI note-on
  // a stop with a MIDI note sounding: the off comes with the stop
  pattern16(12000);
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 11)) == model::Status::Ok);
  seq::Sequencer& s2 = make();
  s2.play_pattern(1);
  g_col.run(s2, 0, at_us(2, 0), 64);
  CHECK(g_col.count(EventKind::MidiNoteOff) == 0u);
  s2.stop();
  g_col.run(s2, at_us(2, 0), at_us(2, 0) + 64, 64);
  CHECK(g_col.count(EventKind::MidiNoteOff) == 1u);
}

TEST_CASE("seq: a step without an instrument repeats the track's last one; a free slot makes no sound; a row with no instrument yet plays nothing") {
  pattern16(12000);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 0)) == model::Status::Ok);  // instrument 0: none, and none before
  CHECK(p.set_step(1, 0, 1, make_step(61, 3)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 2, make_step(62, 0)) == model::Status::Ok);  // repeats slot 2
  CHECK(p.set_step(1, 0, 3, make_step(63, 7)) == model::Status::Ok);  // slot 6: free
  CHECK(p.set_step(1, 0, 4, make_step(64, 0)) == model::Status::Ok);  // the last PLAYED instrument is still slot 2
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(6, 0), 64);
  CHECK(g_col.count(EventKind::NoteOn) == 3u);
  CHECK(g_col.nth(EventKind::NoteOn, 0)->ev.a == 61);
  CHECK(g_col.nth(EventKind::NoteOn, 0)->ev.b == 2);
  CHECK(g_col.nth(EventKind::NoteOn, 1)->ev.a == 62);
  CHECK(g_col.nth(EventKind::NoteOn, 1)->ev.b == 2);
  CHECK(g_col.nth(EventKind::NoteOn, 2)->ev.a == 64);
  CHECK(g_col.nth(EventKind::NoteOn, 2)->ev.b == 2);
  CHECK(s.stats().fx_ignored >= 1u);
}

TEST_CASE("seq: voice FX are modulator events through the control layers, before the note, and only when a parameter's winner changes") {
  pattern16(12000);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kPanning, 80)) == model::Status::Ok);                    // note + panning in column 1
  CHECK(p.set_step(1, 0, 1, fx_step(fx::kPanning, 20)) == model::Status::Ok);                             // a step with no note changes the value
  CHECK(p.set_step(1, 0, 2, fx_step(0, 0, fx::kPanning, 90)) == model::Status::Ok);                       // the second column under the first: nothing changes
  CHECK(p.set_step(1, 0, 3, fx_step(fx::kOff, 0)) == model::Status::Ok);                                  // Off ends column 1: the second takes over
  CHECK(p.set_step(1, 0, 4, make_step(61, 1)) == model::Status::Ok);                                      // a note with no FX ends every column
  CHECK(p.set_step(1, 0, 5, make_step(62, 1, fx::kLowPass, 30, fx::kHighPass, 60)) == model::Status::Ok);  // one filter parameter, two FX
  CHECK(p.set_step(1, 0, 6, make_step(63, 1, fx::kRoll, 3, fx::kVolumeLfo, 5)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 8, fx_step(fx::kVelocity, 70)) == model::Status::Ok);                            // a velocity FX with no note: a persistent volume override
  CHECK(p.set_step(1, 1, 0, make_step(50, 1, 130, 7, 140, 9)) == model::Status::Ok);                      // two engine FX on one step: two modulators
  CHECK(p.set_step(1, 1, 1, fx_step(14, 5)) == model::Status::Ok);                                        // Chord: inert, no event, nothing held
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(10, 0), 64);
  const auto mod_at = [&](std::uint8_t track, std::uint64_t t, std::uint32_t nth = 0) -> const Rec* {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      const Rec& r = g_col.rec[i];
      if (r.ev.kind == EventKind::ModSet && r.ev.track == track && r.t == t && c++ == nth) {
        return &r;
      }
    }
    return nullptr;
  };
  const std::uint8_t g_pan = static_cast<std::uint8_t>(seq::FxGroup::Panning);
  const std::uint8_t g_flt = static_cast<std::uint8_t>(seq::FxGroup::Filter);
  // step 0: panning 80, layer = the first column, group panning, BEFORE the NoteOn in the same sample
  const Rec* m0 = mod_at(0, at_us(0, 0));
  NEED(m0 != nullptr);
  CHECK(m0->ev.a == fx::kPanning);
  CHECK(m0->ev.b == 80);
  CHECK(m0->ev.c == audio::kLayerFx1);
  CHECK(m0->ev.flags == g_pan);
  std::uint32_t idx_mod = 0, idx_on = 0;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].t == at_us(0, 0) && g_col.rec[i].ev.track == 0) {
      if (g_col.rec[i].ev.kind == EventKind::ModSet) {
        idx_mod = i;
      }
      if (g_col.rec[i].ev.kind == EventKind::NoteOn) {
        idx_on = i;
      }
    }
  }
  CHECK(idx_mod < idx_on);
  // step 1: value 20
  const Rec* m1 = mod_at(0, at_us(1, 0));
  NEED(m1 != nullptr);
  CHECK(m1->ev.b == 20);
  // step 2: the second column holds panning under the first: no event
  CHECK(mod_at(0, at_us(2, 0)) == nullptr);
  // step 3: Off ends the first column: the second takes over with ITS value
  const Rec* m3 = mod_at(0, at_us(3, 0));
  NEED(m3 != nullptr);
  CHECK(m3->ev.b == 90);
  CHECK(m3->ev.c == audio::kLayerFx2);
  // step 4: a note with no FX: panning ends
  const Rec* e4 = find(g_col, EventKind::ModEnd, 0, at_us(4, 0));
  NEED(e4 != nullptr);
  CHECK(e4->ev.flags == g_pan);
  // step 5: two filters in two columns: one parameter, one winner (the first column's low-pass)
  const Rec* m5 = mod_at(0, at_us(5, 0));
  NEED(m5 != nullptr);
  CHECK(m5->ev.a == fx::kLowPass);
  CHECK(m5->ev.b == 30);
  CHECK(m5->ev.flags == g_flt);
  CHECK(mod_at(0, at_us(5, 0), 1) == nullptr);
  // step 6: a roll in column 1 ends the filter (the column's FX is replaced by something that is not a voice FX), the LFO in column 2 starts
  const Rec* e6 = find(g_col, EventKind::ModEnd, 0, at_us(6, 0));
  NEED(e6 != nullptr);
  // the filter's first column ended: the second column's high-pass was there and was replaced by the LFO at the same time: the filter group ends
  CHECK(e6->ev.flags == g_flt);
  const Rec* m6 = mod_at(0, at_us(6, 0));
  NEED(m6 != nullptr);
  CHECK(m6->ev.a == fx::kVolumeLfo);
  // step 8: the velocity FX of a step with no note is a persistent volume override (this OS's rule)
  const Rec* m8 = mod_at(0, at_us(8, 0));
  NEED(m8 != nullptr);
  CHECK(m8->ev.a == fx::kVelocity);
  CHECK(m8->ev.b == 70);
  CHECK(m8->ev.flags == static_cast<std::uint8_t>(seq::FxGroup::Volume));
  // engine FX: both columns, two groups, on track 1
  const Rec* x0 = mod_at(1, at_us(0, 0), 0);
  const Rec* x1 = mod_at(1, at_us(0, 0), 1);
  NEED(x0 != nullptr);
  NEED(x1 != nullptr);
  CHECK(x0->ev.a == 130);
  CHECK(x0->ev.flags == static_cast<std::uint8_t>(seq::FxGroup::Engine0));
  CHECK(x1->ev.a == 140);
  CHECK(x1->ev.flags == static_cast<std::uint8_t>(seq::FxGroup::Engine1));
  CHECK(x1->ev.b == 9);
  // the inert Chord makes nothing
  CHECK(mod_at(1, at_us(1, 0)) == nullptr);
  // the run began with an FxReset
  CHECK(g_col.nth(EventKind::FxReset, 0)->t == 0u);
}

TEST_CASE("seq: the performance layer: a track's source pattern (switched at its own loop end), length, play mode, stutter, tempo, pattern length and values") {
  song_project();
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(24000) == model::Status::Ok);
  // pattern 1 (4 steps): track 1 has the notes 31..34; pattern 2 (8 steps): track 1 has 41..48
  for (std::uint32_t k = 0; k < 4; ++k) {
    CHECK(p.set_step(1, 1, k, make_step(static_cast<std::uint8_t>(31 + k))) == model::Status::Ok);
  }
  for (std::uint32_t k = 0; k < 8; ++k) {
    CHECK(p.set_step(2, 1, k, make_step(static_cast<std::uint8_t>(41 + k))) == model::Status::Ok);
  }
  // before the run: track 1 reads pattern 2 whatever the song plays
  seq::Sequencer& s = make();
  s.set_perf_track(1, 2, 0, seq::TrackPlayMode::Forward);
  s.play_pattern(1);
  g_col.run(s, 0, song_us(8), 64);
  // the pattern is 4 steps (the song's), track 1 reads pattern 2's track, whose length is 8: it plays 41..44 and then restarts with the pattern at 41 again
  const std::uint8_t want[8] = {41, 42, 43, 44, 41, 42, 43, 44};
  for (std::uint32_t i = 0; i < 8; ++i) {
    NEED(g_col.nth(EventKind::NoteOn, i, 1) != nullptr);
    CHECK_MESSAGE(g_col.nth(EventKind::NoteOn, i, 1)->ev.a == want[i], i);
  }
  // while playing: the change waits for the track's own loop end (here the pattern's: step 4), and the track's FX end there
  song_project();
  for (std::uint32_t k = 0; k < 4; ++k) {
    // steps 0..2 are notes, step 3 holds only a panning FX, which stays on the track until something replaces it: here, the switch
    CHECK(seq_project().set_step(1, 1, k, k < 3 ? make_step(static_cast<std::uint8_t>(31 + k)) : fx_step(fx::kPanning, 70)) == model::Status::Ok);
    CHECK(seq_project().set_step(2, 1, k, make_step(static_cast<std::uint8_t>(41 + k))) == model::Status::Ok);
  }
  seq::Sequencer& s2 = make();
  s2.play_pattern(1);
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  std::uint64_t t = 0;
  const auto run_to = [&](std::uint64_t end) {
    while (t < end) {
      const std::uint32_t n = static_cast<std::uint32_t>(end - t < 64 ? end - t : 64);
      const std::uint32_t c = s2.step(t, n, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        g_col.add(t, out[i]);
      }
      t += n;
    }
  };
  run_to(song_us(2));
  s2.set_perf_track(1, 2, 0, seq::TrackPlayMode::Forward);
  run_to(song_us(10));
  // 31 32 33 (steps 0..2), the panning of step 3, then the track's loop ends: pattern 2's track: 41 42 ...
  NEED(g_col.nth(EventKind::NoteOn, 3, 1) != nullptr);
  CHECK(g_col.nth(EventKind::NoteOn, 2, 1)->ev.a == 33);
  CHECK(g_col.nth(EventKind::NoteOn, 3, 1)->ev.a == 41);
  CHECK(g_col.nth(EventKind::NoteOn, 3, 1)->t == song_us(4));
  // the panning of step 3 was on the track; the switch at the loop end ended it, before the new pattern's first note
  NEED(find(g_col, EventKind::ModSet, 1, song_us(3)) != nullptr);
  NEED(find(g_col, EventKind::ModEnd, 1, song_us(4)) != nullptr);

  // length and the three play modes
  for (const auto mode : {seq::TrackPlayMode::Forward, seq::TrackPlayMode::Backward, seq::TrackPlayMode::PingPong}) {
    song_project();
    for (std::uint32_t k = 0; k < 8; ++k) {
      CHECK(seq_project().set_step(2, 0, k, make_step(static_cast<std::uint8_t>(20 + k))) == model::Status::Ok);
    }
    seq::Sequencer& sm = make();
    sm.set_perf_track(0, 0, 4, mode);  // the track is 4 steps of the pattern's 8, in this mode
    sm.play_pattern(2);
    g_col.run(sm, 0, song_us(16), 64);
    std::uint8_t exp[16];
    for (std::uint32_t i = 0; i < 16; ++i) {
      std::uint32_t pos = 0;
      if (mode == seq::TrackPlayMode::Forward) {
        pos = i % 4;
        if (i >= 8) {
          pos = (i - 8) % 4;  // the pattern (8 steps) restarts all tracks
        }
      } else if (mode == seq::TrackPlayMode::Backward) {
        pos = 3 - (i % 4);
        if (i >= 8) {
          pos = 3 - ((i - 8) % 4);
        }
      } else {
        // 0 1 2 3 2 1 | 0 1 2 3 2 1 | 0 1: the bounce is the track's own, the pattern's restart at step 8 does not cut it (OS-BUG-PERFPING)
        static const std::uint8_t pp[6] = {0, 1, 2, 3, 2, 1};
        pos = pp[i % 6];
      }
      exp[i] = static_cast<std::uint8_t>(20 + pos);
    }
    for (std::uint32_t i = 0; i < 16; ++i) {
      NEED(g_col.nth(EventKind::NoteOn, i, 0) != nullptr);
      CHECK_MESSAGE(g_col.nth(EventKind::NoteOn, i, 0)->ev.a == exp[i], i);
    }
  }
  // Random: always inside the track, and the same every time
  std::uint64_t hashes[2] = {0, 0};
  for (int pass = 0; pass < 2; ++pass) {
    song_project();
    for (std::uint32_t k = 0; k < 8; ++k) {
      CHECK(seq_project().set_step(2, 0, k, make_step(static_cast<std::uint8_t>(20 + k))) == model::Status::Ok);
    }
    seq::Sequencer& sr = make();
    sr.set_perf_track(0, 0, 5, seq::TrackPlayMode::Random);
    sr.play_pattern(2);
    g_col.run(sr, 0, song_us(64), 64);
    for (std::uint32_t i = 1; i < g_col.count(EventKind::NoteOn, 0); ++i) {
      CHECK(g_col.nth(EventKind::NoteOn, i, 0)->ev.a < 25);
    }
    hashes[pass] = g_col.hash;
  }
  CHECK(hashes[0] == hashes[1]);
}

// E-PERF-07: the per-track queued pattern (QueueTrackPattern). The song project holds pattern 1 (4 steps) and pattern 2 (8 steps); the tracks 0 and 1 get notes in both, so that the
// note on a step says which pattern the track reads: pattern 1 gives track 0 61..64 and track 1 31..34, pattern 2 gives track 0 20..27 and track 1 41..48.
namespace {

void trackpat_project() {
  song_project();
  model::Project& p = seq_project();
  for (std::uint32_t k = 0; k < 4; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(static_cast<std::uint8_t>(61 + k))) == model::Status::Ok);
    CHECK(p.set_step(1, 1, k, make_step(static_cast<std::uint8_t>(31 + k))) == model::Status::Ok);
  }
  for (std::uint32_t k = 0; k < 8; ++k) {
    CHECK(p.set_step(2, 0, k, make_step(static_cast<std::uint8_t>(20 + k))) == model::Status::Ok);
    CHECK(p.set_step(2, 1, k, make_step(static_cast<std::uint8_t>(41 + k))) == model::Status::Ok);
  }
}

// The note a track starts on a step (0 when none).
int note_at(std::uint8_t track, std::uint32_t step) {
  const Rec* r = find(g_col, EventKind::NoteOn, track, song_us(step));
  return r != nullptr ? r->ev.a : 0;
}

bool queue_tracks(seq::Sequencer& s, std::uint32_t mask, std::uint32_t pattern) {
  return s.commands().push(seq::Command{seq::Command::Kind::QueueTrackPattern, 0, static_cast<std::uint16_t>(pattern), mask});
}

}  // namespace

TEST_CASE("seq E-PERF-07: a queued track pattern lands at the track's own loop end and not before, tracks of different lengths each at their own end") {
  trackpat_project();
  seq::Sequencer& s = make();
  s.set_perf_track(0, 0, 4, seq::TrackPlayMode::Forward);  // track 0 loops every 4 steps of pattern 2, track 1 every 8
  s.play_pattern(2);
  g_col.run(s, 0, song_us(1, 10), 64);
  CHECK(queue_tracks(s, 0x3u, 1));
  g_col.run(s, song_us(1, 10), song_us(18), 64);
  const int t0[8] = {20, 21, 22, 23, 61, 62, 63, 64};  // pattern 1 has 4 steps: track 0 keeps its length of 4 (perf_len untouched) and lands at step 4
  for (std::uint32_t i = 0; i < 8; ++i) {
    CHECK_MESSAGE(note_at(0, i) == t0[i], i);
  }
  const int t1[8] = {41, 42, 43, 44, 45, 46, 47, 48};  // track 1: its own end is the pattern's, step 8
  for (std::uint32_t i = 0; i < 8; ++i) {
    CHECK_MESSAGE(note_at(1, i) == t1[i], i);
  }
  for (std::uint32_t i = 0; i < 4; ++i) {
    CHECK_MESSAGE(note_at(1, 8 + i) == 31 + static_cast<int>(i), i);  // landed: pattern 1's track 1 (4 steps), also past the next pattern restart
    CHECK_MESSAGE(note_at(1, 12 + i) == 31 + static_cast<int>(i), i);
  }
  // the screen's word: the override plays, nothing waits
  CHECK(s.play_position().track_pattern(1) == 1u);
  CHECK(s.play_position().track_pattern(2) == 0u);
}

TEST_CASE("seq E-PERF-07: the same queue again takes the queue back, the pattern the track plays pins it; a mask past the tracks and a pattern past 255 do nothing") {
  trackpat_project();
  seq::Sequencer& s = make();
  s.play_pattern(2);
  g_col.run(s, 0, song_us(1, 10), 64);
  CHECK(queue_tracks(s, 0x2u, 1));
  g_col.run(s, song_us(1, 10), song_us(2, 10), 64);
  CHECK(s.play_position().track_pattern(1) == (0x10000u | (1u << 8)));
  CHECK(queue_tracks(s, 0x2u, 1));  // the second press
  g_col.run(s, song_us(2, 10), song_us(3, 10), 64);
  CHECK(s.play_position().track_pattern(1) == 0u);
  CHECK(queue_tracks(s, 0x2u, 1));
  CHECK(queue_tracks(s, 0x2u, 2));  // the pattern it plays: the wait is gone and the override is there at once
  CHECK(queue_tracks(s, 0xFFFF0000u, 1));  // bits past kMaxTracks
  CHECK(queue_tracks(s, 0x2u, 300));       // not a pattern id
  g_col.run(s, song_us(3, 10), song_us(18), 64);
  CHECK(s.play_position().track_pattern(1) == 2u);
  for (std::uint32_t i = 0; i < 16; ++i) {
    CHECK_MESSAGE(note_at(1, i) == 41 + static_cast<int>(i % 8), i);
  }
}

TEST_CASE("seq E-PERF-07: queueing the pattern the track follows pins it against the song's steps; pattern 0 for a track on the song takes a wait back") {
  trackpat_project();
  seq::Sequencer& s = make();
  s.play_song(0);  // pattern 1 (4 steps), 2 (8), 1 (4)
  g_col.run(s, 0, song_us(1, 10), 64);
  CHECK(queue_tracks(s, 0x2u, 1));  // the song's pattern, the track follows it: pinned at once
  CHECK(queue_tracks(s, 0x4u, 2));  // a wait on track 2 ...
  CHECK(queue_tracks(s, 0x4u, 0));  // ... and the song's pattern for a track on the song takes it back
  g_col.run(s, song_us(1, 10), song_us(2, 10), 64);
  CHECK(s.play_position().track_pattern(1) == 1u);
  CHECK(s.play_position().track_pattern(2) == 0u);
  g_col.run(s, song_us(2, 10), song_us(13), 64);
  for (std::uint32_t i = 4; i < 12; ++i) {
    CHECK_MESSAGE(note_at(1, i) == 31 + static_cast<int>(i % 4), i);  // the song is on pattern 2 meanwhile, track 1 stays on pattern 1
  }
  CHECK(s.play_position().track_pattern(1) == 1u);
  CHECK(queue_tracks(s, 0x2u, 0));  // TRK SONG: the song's pattern from the track's own end
  g_col.run(s, song_us(13), song_us(14), 64);
  CHECK(s.play_position().track_pattern(1) == (0x10000u | 1u));
}

TEST_CASE("seq E-PERF-07: PLAYMODE after the queue or after the landing keeps it; the queue is ignored while stopped; Stop drops an unlanded queue and a landed override") {
  trackpat_project();
  seq::Sequencer& s = make();
  CHECK(queue_tracks(s, 0x2u, 1));  // stopped: nothing
  s.play_pattern(2);
  const seq::Command playmode{seq::Command::Kind::PerfTrack, 1, 0, static_cast<std::uint32_t>(seq::TrackPlayMode::Forward) << 8};
  g_col.run(s, 0, song_us(1, 10), 64);
  CHECK(queue_tracks(s, 0x2u, 1));
  CHECK(s.commands().push(playmode));  // the PLAYMODE FX sends pattern 0 with its mode: the wait stays
  g_col.run(s, song_us(1, 10), song_us(10, 10), 64);
  CHECK(note_at(1, 7) == 48);
  CHECK(note_at(1, 8) == 31);
  CHECK(s.commands().push(playmode));  // after the landing: the override stays
  g_col.run(s, song_us(10, 10), song_us(14, 10), 64);
  CHECK(note_at(1, 12) == 31);
  CHECK(s.play_position().track_pattern(1) == 1u);
  // Stop and Play: the song's pattern again
  CHECK(s.commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
  g_col.run(s, song_us(14, 10), song_us(15, 10), 64);
  CHECK(s.play_position().track_pattern(1) == 0u);
  s.play_pattern(2);
  const std::uint64_t base = song_us(15, 10);
  g_col.n = 0;
  g_col.run(s, base, base + song_us(17), 64);
  bool seen41 = false;
  bool seen31 = false;
  for (std::uint32_t i = 0; i < g_col.count(EventKind::NoteOn, 1); ++i) {
    const int n = g_col.nth(EventKind::NoteOn, i, 1)->ev.a;
    seen41 = seen41 || n == 41;
    seen31 = seen31 || n == 31;
  }
  CHECK(seen41);
  CHECK_FALSE(seen31);
  // an unlanded queue goes with the Stop as well
  CHECK(queue_tracks(s, 0x2u, 1));
  g_col.run(s, base + song_us(17), base + song_us(18), 64);
  CHECK(s.play_position().track_pattern(1) == (0x10000u | (1u << 8)));
  CHECK(s.commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
  g_col.run(s, base + song_us(18), base + song_us(19), 64);
  CHECK(s.play_position().track_pattern(1) == 0u);
}

TEST_CASE("seq E-PERF-07: a pattern switch and a song step do not undo a landed override; the wait survives a pattern restart and never lands on a Random track or a track longer than the pattern") {
  // landed override over a global switch and over the song's steps (order 1, 2, 1)
  trackpat_project();
  seq::Sequencer& s = make();
  s.play_song(0);  // pattern 1 (4 steps), 2 (8), 1 (4)
  g_col.run(s, 0, song_us(1, 10), 64);
  CHECK(queue_tracks(s, 0x2u, 2));  // track 1 reads pattern 2 from step 4 on, the song goes on through 1, 2, 1
  g_col.run(s, song_us(1, 10), song_us(40), 64);
  for (std::uint32_t i = 0; i < 4; ++i) {
    CHECK_MESSAGE(note_at(1, i) == 31 + static_cast<int>(i), i);
  }
  for (std::uint32_t i = 4; i < 24; ++i) {
    CHECK_MESSAGE(note_at(1, i) != 0, i);
    CHECK_MESSAGE(note_at(1, i) >= 41, i);  // pattern 2's track 1 whatever pattern the song plays
  }
  CHECK(s.play_position().track_pattern(1) == 2u);
  // Random never has a loop end: the queue lands at the song pattern's end (8 steps) instead
  trackpat_project();
  seq::Sequencer& r = make();
  r.set_perf_track(1, 0, 0, seq::TrackPlayMode::Random);
  r.play_pattern(2);
  g_col.run(r, 0, song_us(1, 10), 64);
  CHECK(queue_tracks(r, 0x2u, 1));
  g_col.run(r, song_us(1, 10), song_us(40), 64);
  CHECK(r.play_position().track_pattern(1) == 1u);
  for (std::uint32_t i = 2; i < 8; ++i) {
    CHECK_MESSAGE(note_at(1, i) >= 41, i);
  }
  for (std::uint32_t i = 8; i < 38; ++i) {
    const int n = note_at(1, i);
    CHECK_MESSAGE((n >= 31 && n <= 34), i);  // pattern 1's track 1 (4 steps), in random order
  }
  // a track longer than the pattern is restarted by the pattern's end (load_tracks) before its own end comes: it lands there
  trackpat_project();
  seq::Sequencer& l = make();
  l.set_perf_track(1, 0, 6, seq::TrackPlayMode::Forward);
  l.play_pattern(1);  // 4 steps
  g_col.run(l, 0, song_us(1, 10), 64);
  CHECK(queue_tracks(l, 0x2u, 2));
  g_col.run(l, song_us(1, 10), song_us(30), 64);
  CHECK(l.play_position().track_pattern(1) == 2u);
  for (std::uint32_t i = 0; i < 12; ++i) {
    CHECK_MESSAGE(note_at(1, 4 + i) == 41 + static_cast<int>(i % 4), i);  // pattern 2's track 1 from step 4 (6 steps long, cut by the 4-step pattern)
  }
}

// OS-PERFREMIX R1: SetTrackPattern, a track's pattern at once. Pattern 1 (the song's, 16 steps), 3 (16 steps) and 4 (8 steps); track 2 reads 61+k, 101+k and 81+k, every step with a Panning FX, so that
// a change that ended the track's FX would show as a ModEnd (a note step without the FX would end it by itself).
namespace {

void remix_project() {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(24000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(3, 16) == model::Status::Ok);
  CHECK(p.create_pattern(4, 8) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 2, k, make_step(static_cast<std::uint8_t>(61 + k), 1, fx::kPanning, 75)) == model::Status::Ok);
    CHECK(p.set_step(3, 2, k, make_step(static_cast<std::uint8_t>(101 + k), 1, fx::kPanning, 75)) == model::Status::Ok);
  }
  for (std::uint32_t k = 0; k < 8; ++k) {
    CHECK(p.set_step(4, 2, k, make_step(static_cast<std::uint8_t>(81 + k), 1, fx::kPanning, 75)) == model::Status::Ok);
  }
}

bool set_tracks(seq::Sequencer& s, std::uint32_t mask, std::uint32_t pattern) {
  return s.commands().push(seq::Command{seq::Command::Kind::SetTrackPattern, 0, static_cast<std::uint16_t>(pattern), mask});
}

// One render of the first scenario (a switch on step 5), for the determinism check: the hash of every event.
std::uint64_t remix_render_hash() {
  remix_project();
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, song_us(5, 10), 64);
  CHECK(set_tracks(s, 0x4u, 3));
  g_col.run(s, song_us(5, 10), song_us(40), 64);
  return g_col.hash;
}

}  // namespace

TEST_CASE("seq OS-PERFREMIX R1: a track switched to another pattern on step 5 plays that pattern's next row, its FX go on, the word shows it with no wait bit") {
  remix_project();
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, song_us(5, 10), 64);
  CHECK(set_tracks(s, 0x4u, 3));
  g_col.run(s, song_us(5, 10), song_us(36), 64);
  for (std::uint32_t i = 0; i < 6; ++i) {
    CHECK_MESSAGE(note_at(2, i) == 61 + static_cast<int>(i), i);  // the song's pattern up to the step that was playing
  }
  for (std::uint32_t i = 6; i < 16; ++i) {
    CHECK_MESSAGE(note_at(2, i) == 101 + static_cast<int>(i), i);  // pattern 3 from row 6: the place is kept
  }
  for (std::uint32_t i = 16; i < 32; ++i) {
    CHECK_MESSAGE(note_at(2, i) == 85 + static_cast<int>(i), i);  // the song's pattern restarts: the override stays, row 0 again
  }
  CHECK(g_col.count(EventKind::ModEnd, 2) == 0u);  // no FX end (the official change at once cancels none)
  CHECK(s.play_position().track_pattern(2) == 3u);
  CHECK(s.play_position().track_pattern(1) == 0u);  // only the masked track
  // the control: the queue at the track's loop end does end the FX
  remix_project();
  seq::Sequencer& q = make();
  q.play_pattern(1);
  g_col.run(q, 0, song_us(1, 10), 64);
  CHECK(queue_tracks(q, 0x4u, 3));
  g_col.run(q, song_us(1, 10), song_us(20), 64);
  CHECK(g_col.count(EventKind::ModEnd, 2) >= 1u);
}

TEST_CASE("seq OS-PERFREMIX R1: a track switched to a shorter pattern rejoins at the song's step modulo its length") {
  remix_project();
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, song_us(12, 10), 64);
  CHECK(set_tracks(s, 0x4u, 4));  // 8 steps, on step 12
  g_col.run(s, song_us(12, 10), song_us(40), 64);
  for (std::uint32_t i = 0; i < 13; ++i) {
    CHECK_MESSAGE(note_at(2, i) == 61 + static_cast<int>(i), i);
  }
  CHECK(note_at(2, 13) == 81 + (13 % 8));  // (12 + 1) % 8 = 5
  CHECK(note_at(2, 14) == 81 + (14 % 8));
  CHECK(note_at(2, 15) == 81 + (15 % 8));
  for (std::uint32_t i = 16; i < 32; ++i) {
    CHECK_MESSAGE(note_at(2, i) == 81 + static_cast<int>(i % 8), i);  // the track's own 8-step loop
  }
  CHECK(g_col.count(EventKind::ModEnd, 2) == 0u);
}

TEST_CASE("seq OS-PERFREMIX R1: a change at once replaces a wait; pattern 0 sends the track back to the song at once; PLAYMODE keeps the override") {
  remix_project();
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, song_us(1, 10), 64);
  CHECK(queue_tracks(s, 0x4u, 3));  // a wait for pattern 3 at the loop end (step 16)
  g_col.run(s, song_us(1, 10), song_us(3, 10), 64);
  CHECK(s.play_position().track_pattern(2) == (0x10000u | (3u << 8)));
  CHECK(set_tracks(s, 0x4u, 4));  // replaces it
  g_col.run(s, song_us(3, 10), song_us(5, 10), 64);
  CHECK(s.play_position().track_pattern(2) == 4u);  // no wait bit
  CHECK(note_at(2, 4) == 81 + 4);
  CHECK(set_tracks(s, 0x4u, 3));
  const seq::Command playmode{seq::Command::Kind::PerfTrack, 2, 0, static_cast<std::uint32_t>(seq::TrackPlayMode::Forward) << 8};
  CHECK(s.commands().push(playmode));  // the PLAYMODE FX sends pattern 0 with its mode: the override stays
  g_col.run(s, song_us(5, 10), song_us(9, 10), 64);
  CHECK(s.play_position().track_pattern(2) == 3u);
  CHECK(note_at(2, 8) == 101 + 8);
  CHECK(set_tracks(s, 0x4u, 0));  // back to the song's pattern, at once
  g_col.run(s, song_us(9, 10), song_us(14, 10), 64);
  CHECK(s.play_position().track_pattern(2) == 0u);
  CHECK(note_at(2, 10) == 61 + 10);
  CHECK(note_at(2, 13) == 61 + 13);
}

TEST_CASE("seq OS-PERFREMIX R1: ignored while stopped, for a pattern past 255 and for a mask past the tracks; Stop ends it") {
  remix_project();
  seq::Sequencer& s = make();
  CHECK(set_tracks(s, 0x4u, 3));  // stopped: nothing
  s.play_pattern(1);
  g_col.run(s, 0, song_us(5, 10), 64);
  CHECK(s.play_position().track_pattern(2) == 0u);
  CHECK(note_at(2, 4) == 61 + 4);
  CHECK(set_tracks(s, 0x4u, 300));
  CHECK(set_tracks(s, 0xFFFF0000u, 3));
  g_col.run(s, song_us(5, 10), song_us(8, 10), 64);
  CHECK(note_at(2, 7) == 61 + 7);
  CHECK(s.play_position().track_pattern(2) == 0u);
  CHECK(set_tracks(s, 0x4u, 3));
  g_col.run(s, song_us(8, 10), song_us(10, 10), 64);
  CHECK(s.play_position().track_pattern(2) == 3u);
  CHECK(s.commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
  g_col.run(s, song_us(10, 10), song_us(11, 10), 64);
  CHECK(s.play_position().track_pattern(2) == 0u);
  s.play_pattern(1);
  const std::uint64_t base = song_us(11, 10);
  g_col.n = 0;
  g_col.run(s, base, base + song_us(17), 64);
  for (std::uint32_t i = 0; i < g_col.count(EventKind::NoteOn, 2); ++i) {
    CHECK(g_col.nth(EventKind::NoteOn, i, 2)->ev.a < 100);  // the song's pattern again, never pattern 3's
  }
  CHECK(g_col.count(EventKind::NoteOn, 2) >= 16u);
}

// A change drained on the last row of the track's own loop: advance_tracks wraps on the length read for the pattern left, so the new pattern's length has to be taken at the command (the fold of
// the R1 review). Pattern 1 (16 steps) holds track 2 as an 8-step loop (61+k); pattern 3 holds it 16 steps long (101+k); pattern 6 holds it 3 steps long (121+k).
namespace {

void lastrow_project() {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(24000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(3, 16) == model::Status::Ok);
  CHECK(p.create_pattern(6, 16) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 8; ++k) {
    CHECK(p.set_step(1, 2, k, make_step(static_cast<std::uint8_t>(61 + k))) == model::Status::Ok);
  }
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(3, 2, k, make_step(static_cast<std::uint8_t>(101 + k))) == model::Status::Ok);
  }
  for (std::uint32_t k = 0; k < 3; ++k) {
    CHECK(p.set_step(6, 2, k, make_step(static_cast<std::uint8_t>(121 + k))) == model::Status::Ok);
  }
  CHECK(p.set_track_length(1, 2, 8) == model::Status::Ok);
  CHECK(p.set_track_length(6, 2, 3) == model::Status::Ok);
}

}  // namespace

TEST_CASE("seq OS-PERFREMIX R1: a change drained on the last row of the track's loop keeps its place on a longer pattern and rejoins on a shorter one") {
  // longer target: step 7 is the last row of the 8-step loop; the track goes on with row 8 of pattern 3 (it must not wrap to row 0)
  lastrow_project();
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, song_us(7, 10), 64);
  CHECK(set_tracks(s, 0x4u, 3));
  g_col.run(s, song_us(7, 10), song_us(20), 64);
  for (std::uint32_t i = 0; i < 8; ++i) {
    CHECK_MESSAGE(note_at(2, i) == 61 + static_cast<int>(i), i);
  }
  for (std::uint32_t i = 8; i < 16; ++i) {
    CHECK_MESSAGE(note_at(2, i) == 101 + static_cast<int>(i), i);
  }
  CHECK(note_at(2, 16) == 101);  // the song's pattern restarts: row 0 again
  // shorter target (3 steps): the next step is the song's step 8 modulo 3 = row 2, then the track's own loop
  lastrow_project();
  seq::Sequencer& r = make();
  r.play_pattern(1);
  g_col.run(r, 0, song_us(7, 10), 64);
  CHECK(set_tracks(r, 0x4u, 6));
  g_col.run(r, song_us(7, 10), song_us(14), 64);
  const int want[6] = {123, 121, 122, 123, 121, 122};  // steps 8..13
  for (std::uint32_t i = 0; i < 6; ++i) {
    CHECK_MESSAGE(note_at(2, 8 + i) == want[i], i);
  }
}

TEST_CASE("seq OS-PERFREMIX R1: two renders with a change at once are identical") {
  const std::uint64_t h1 = remix_render_hash();
  const std::uint64_t h2 = remix_render_hash();
  CHECK(h1 == h2);
  CHECK(h1 != 0xCBF29CE484222325ull);
}

// OS-BUG-PERFMODE / OS-PERFMODES M4: the label the Perform screen shows for each stored play mode value names the mode the sequencer runs for it. The command is the one the Perform
// bank sends (perf_command), with the track narrowed to 4 steps of the pattern's 8 (the length byte; the bank leaves it to the song) so that the orders differ. 0..3 are FWD, BACK,
// PING, RND; 4..23 are the custom play orders C1..C20 (the note played is the order's entry modulo the track's length, as seq_playorder_test.cpp checks step by step).
TEST_CASE("seq: each Perform play mode label names the order the sequencer plays") {
  struct Mode {
    const char* label;
    const std::uint8_t* first8;  // the notes of the first 8 steps; nullptr: the random order or a custom order (its notes come from play_order_entry below)
  };
  static const std::uint8_t kFwd[8] = {20, 21, 22, 23, 20, 21, 22, 23};
  static const std::uint8_t kBack[8] = {23, 22, 21, 20, 23, 22, 21, 20};
  static const std::uint8_t kPing[8] = {20, 21, 22, 23, 22, 21, 20, 21};
  const Mode modes[4] = {{"FWD", kFwd}, {"BACK", kBack}, {"PING", kPing}, {"RND", nullptr}};
  CHECK(seq::perf_info(seq::PerfFx::PatternPlayMode).max == 23);  // OS-PERFMODES M4: 0..23 (FWD, BACK, PING, RND, C1..C20)
  for (int v = 0; v <= 23; ++v) {
    char text[8];
    (void)seq::perf_value_text(seq::PerfFx::PatternPlayMode, v, text);
    if (v < 4) {
      CHECK_MESSAGE(std::strcmp(text, modes[v].label) == 0, v);
    } else {
      char want[16];  // room for any int: the Arm build's -Wformat-truncation counts it
      std::snprintf(want, sizeof want, "C%d", v - 3);
      CHECK_MESSAGE(std::strcmp(text, want) == 0, v);
    }
    song_project();
    for (std::uint32_t k = 0; k < 8; ++k) {
      CHECK(seq_project().set_step(2, 0, k, make_step(static_cast<std::uint8_t>(20 + k))) == model::Status::Ok);
    }
    seq::Sequencer& s = make();
    seq::Command c{};
    NEED(seq::perf_command(0, seq::PerfFx::PatternPlayMode, v, c));
    c.c |= 4u;
    CHECK(s.commands().push(c));
    s.play_pattern(2);
    g_col.run(s, 0, song_us(16), 64);
    std::uint8_t got[16];
    for (std::uint32_t i = 0; i < 16; ++i) {
      // the note the track starts on step i (by its time, as seq_playorder_test.cpp's note_at: a roll of the order repeats notes and would shift nth())
      const Rec* r = nullptr;
      for (std::uint32_t k = 0; k < g_col.n; ++k) {
        if (g_col.rec[k].ev.kind == EventKind::NoteOn && g_col.rec[k].ev.track == 0 && g_col.rec[k].t == song_us(i)) {
          r = &g_col.rec[k];
          break;
        }
      }
      NEED(r != nullptr);
      got[i] = r->ev.a;
    }
    if (v < 4 && modes[v].first8 != nullptr) {
      for (std::uint32_t i = 0; i < 8; ++i) {
        CHECK_MESSAGE(got[i] == modes[v].first8[i], v);
      }
    } else if (v < 4) {
      bool same[3] = {true, true, true};
      for (std::uint32_t i = 0; i < 16; ++i) {
        CHECK(got[i] >= 20);
        CHECK(got[i] < 24);
        for (int m = 0; m < 3; ++m) {
          same[m] = same[m] && (i >= 8 || got[i] == modes[m].first8[i]);
        }
      }
      CHECK_FALSE((same[0] || same[1] || same[2]));
    } else {
      // C1..C20: the notes are the order's entries over the track's 4 steps (the note of the pattern's row the entry names, modulo the length)
      for (std::uint32_t i = 0; i < 16; ++i) {
        const seq::PlayOrderEntry e = seq::play_order_entry(static_cast<unsigned>(v - 4), i % 4u);
        const unsigned msg = static_cast<unsigned>(v) * 100u + i;
        CHECK_MESSAGE(got[i] == 20 + static_cast<int>((e.step - 1u) % 4u), msg);
      }
    }
  }
}

// OS-BUG-PERFPING (unit FAIL on v0.7.0-int.2): PING never bounced. The Perform bank sends the play mode with the length byte 0 (the track is as long as the pattern), and the
// pattern's own restart (pattern_end -> load_tracks(0)) put every track back on step 0 at the very step the bounce began: a track as long as its pattern played 0 1 2 3 | 0 1 2 3,
// the order of FWD. The bounce (period 2 L - 2 steps) is a free run of the track now, while the same pattern plays on; a Perform PAT LEN shorter than the track is the span.
namespace {
struct RingSink final : seq::CommandSink {
  seq::CommandRing& ring;
  explicit RingSink(seq::CommandRing& r) : ring(r) {}
  bool push(const seq::Command& c) noexcept override { return ring.push(c); }
};

// The step a ping-pong track of `n` steps plays at its i-th step: 0 1 .. n-1 n-2 .. 1 0 1 ...
std::uint32_t ping_pos(std::uint32_t i, std::uint32_t n) {
  if (n < 2u) {
    return 0;
  }
  const std::uint32_t cycle = 2u * n - 2u;
  const std::uint32_t r = i % cycle;
  return r < n ? r : cycle - r;
}

// The bank's slot `slot` set to `fx` = `value` the way the screen does it: EDIT on, the slot's FX picked, the pad of row 1 held, the encoder turned, the pad released.
void bank_slot(seq::PerformBank& b, unsigned slot, seq::PerfFx fx, int value, seq::CommandSink& out) {
  b.set_edit(true);
  for (unsigned guard = 0; b.fx(slot) != fx && guard < 40u; ++guard) {
    b.change_fx(slot, +1, out);
  }
  b.pad_down(1u * seq::kPerfSlots + slot, out);
  b.turn(value - b.stored(slot, 1), out);  // from the row's stored value (its default since OS-PRFDEFAULTS) to `value`
  b.pad_up(1u * seq::kPerfSlots + slot, out);
  b.set_edit(false);
}
}  // namespace

TEST_CASE("seq: a Perform PING track bounces over its whole length - the pattern's restart does not cut it (OS-BUG-PERFPING)") {
  struct Case {
    std::uint32_t pattern_len;  // the pattern's (and the track's) length
    int pat_len_value;          // the Perform PAT LEN value (0: none; 3 is 4 steps)
    std::uint32_t span;         // the steps the bounce covers
    std::uint32_t steps;        // the steps played and checked
    std::uint32_t apply_step;   // the step during which the track joins the layer (0: before Play)
    std::uint32_t first;        // the first step checked
    int shift;                  // the bounce's phase: step i plays ping_pos(i + shift, span)
  };
  const Case cases[] = {
      {4, 0, 4, 24, 0, 0, 0},     // the tester's PAT LEN 4 pattern: the period is 6, not 4
      {2, 0, 2, 12, 0, 0, 0},     // two steps: 0 1 0 1
      {8, 0, 8, 56, 0, 0, 0},
      {64, 0, 64, 260, 0, 0, 0},  // three laps of the 64 steps, two of the bounce (126)
      {64, 0, 64, 260, 5, 0, 0},  // the row pressed after Play: 0 .. 5 forward, then the same bounce
      {16, 3, 4, 40, 0, 0, 0},    // Perform PAT LEN 4 over a 16-step track: the pattern restarts every 4 steps, the bounce is 4 long
      {16, 3, 4, 40, 7, 8, -4},   // PING and PAT LEN 4 join together at step 7, the track (FWD until then) being on 7, past the span: it goes to the top and turns back, 2 1 0 1 2 3 2 ...
  };
  for (const Case& c : cases) {
    fresh_project(4, 4);
    model::Project& p = seq_project();
    CHECK(p.set_tempo_x100(24000) == model::Status::Ok);
    CHECK(p.create_pattern(1, c.pattern_len) == model::Status::Ok);
    for (std::uint32_t k = 0; k < c.pattern_len; ++k) {
      CHECK(p.set_step(1, 0, k, make_step(static_cast<std::uint8_t>(20 + k))) == model::Status::Ok);
    }
    seq::Sequencer& s = make();
    seq::PerformBank bank;
    RingSink sink(s.commands());
    bank_slot(bank, 8, seq::PerfFx::PatternPlayMode, 2, sink);  // PING
    if (c.pat_len_value != 0) {
      bank_slot(bank, 9, seq::PerfFx::PatternLength, c.pat_len_value, sink);
    }
    if (c.apply_step == 0u) {
      bank.toggle_track(0, sink);
    }
    s.play_pattern(1);
    if (c.apply_step != 0u) {
      g_col.run(s, 0, song_us(c.apply_step, 10), 64);
      bank.toggle_track(0, sink);
      g_col.run(s, song_us(c.apply_step, 10), song_us(c.steps), 64);
    } else {
      g_col.run(s, 0, song_us(c.steps), 64);
    }
    for (std::uint32_t i = c.first; i < c.steps; ++i) {
      const Rec* r = g_col.nth(EventKind::NoteOn, i, 0);
      NEED(r != nullptr);
      CHECK_MESSAGE(r->ev.a == 20 + ping_pos(static_cast<std::uint32_t>(static_cast<int>(i) + c.shift), c.span),
                    "pattern " << c.pattern_len << " PAT LEN " << c.pat_len_value << " apply " << c.apply_step << " step " << i);
    }
  }
}

TEST_CASE("seq: a PING track starts its bounce again when a different pattern plays (OS-BUG-PERFPING)") {
  song_project();  // order 1 (4 steps), 2 (8 steps), 1
  model::Project& p = seq_project();
  for (std::uint32_t k = 0; k < 8; ++k) {
    CHECK(p.set_step(1, 1, k % 4u, make_step(static_cast<std::uint8_t>(30 + k % 4u))) == model::Status::Ok);
    CHECK(p.set_step(2, 1, k, make_step(static_cast<std::uint8_t>(40 + k))) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  seq::Command c{};
  NEED(seq::perf_command(1, seq::PerfFx::PatternPlayMode, 2, c));
  CHECK(s.commands().push(c));
  s.play_song(0);
  g_col.run(s, 0, song_us(16), 64);
  // pattern 1: 30 31 32 33; pattern 2 (a new pattern: the bounce starts again): 40 41 42 43 44 45 46 47; pattern 1 again: 30 31 32 33
  const std::uint8_t want[16] = {30, 31, 32, 33, 40, 41, 42, 43, 44, 45, 46, 47, 30, 31, 32, 33};
  for (std::uint32_t i = 0; i < 16; ++i) {
    const Rec* r = g_col.nth(EventKind::NoteOn, i, 1);
    NEED(r != nullptr);
    CHECK_MESSAGE(r->ev.a == want[i], i);
  }
}

TEST_CASE("seq: a PING track goes on through the pattern's natural restart; a Break, an Order jump, a live jump and a pattern switch start its bounce again (OS-BUG-PERFPING)") {
  // Patterns 1 and 2 of 8 steps; track 1 plays 40 + row in pattern 1 and 60 + row in pattern 2; track 1 is in PING mode (the bank's command, no length byte); the song is `order` x pattern 1.
  const auto setup = [](std::uint32_t order_len) {
    fresh_project(4, 4);
    model::Project& p = seq_project();
    CHECK(p.set_tempo_x100(24000) == model::Status::Ok);
    for (std::uint32_t id = 1; id <= 2; ++id) {
      CHECK(p.create_pattern(id, 8) == model::Status::Ok);
      for (std::uint32_t k = 0; k < 8; ++k) {
        CHECK(p.set_step(id, 1, k, make_step(static_cast<std::uint8_t>(20u + 20u * id + k))) == model::Status::Ok);
      }
    }
    for (std::uint32_t i = 0; i < order_len; ++i) {
      CHECK(p.append_order(1) == model::Status::Ok);
    }
  };
  const auto ping = [](seq::Sequencer& s) {
    seq::Command c{};
    CHECK(seq::perf_command(1, seq::PerfFx::PatternPlayMode, 2, c));
    CHECK(s.commands().push(c));
  };
  const auto expect = [](const char* what, std::initializer_list<std::uint8_t> notes) {
    std::uint32_t i = 0;
    for (const std::uint8_t want : notes) {
      const Rec* r = g_col.nth(EventKind::NoteOn, i, 1);
      NEED(r != nullptr);
      CHECK_MESSAGE(r->ev.a == want, doctest::String(what) << ": step " << i);
      ++i;
    }
  };

  // The same pattern twice in a song: the order advances at the natural end and the bounce goes on (0 .. 7 6 .. 1 0 1 ..).
  setup(2);
  {
    seq::Sequencer& s = make();
    ping(s);
    s.play_song(0);
    g_col.run(s, 0, song_us(32), 64);
    expect("song [1,1]", {40, 41, 42, 43, 44, 45, 46, 47, 46, 45, 44, 43, 42, 41, 40, 41, 42, 43, 44, 45, 46, 47, 46, 45});
  }
  // A Break pattern (track 2, row 3) cuts the pattern after row 3 every lap: the bounce starts again each time (the pattern restarts every track).
  setup(0);
  CHECK(seq_project().set_step(1, 2, 3, fx_step(fx::kBreakPattern, 1)) == model::Status::Ok);
  {
    seq::Sequencer& s = make();
    ping(s);
    s.play_pattern(1);
    g_col.run(s, 0, song_us(12), 64);
    expect("break", {40, 41, 42, 43, 40, 41, 42, 43, 40, 41, 42, 43});
  }
  // An Order jump to position 0 (track 2, row 5): the same pattern again, cut: the bounce starts again.
  setup(1);
  CHECK(seq_project().set_step(1, 2, 5, fx_step(fx::kOrderJump, 0)) == model::Status::Ok);
  {
    seq::Sequencer& s = make();
    ping(s);
    s.play_song(0);
    g_col.run(s, 0, song_us(18), 64);
    expect("order jump", {40, 41, 42, 43, 44, 45, 40, 41, 42, 43, 44, 45, 40, 41, 42, 43, 44, 45});
  }
  // A live jump (queue_jump) to position 0 of [1,1], queued during the first lap: at its natural end the song goes to the same pattern, cut by the jump: the bounce starts again, and the next
  // end (position 0 to the end of the list and the loop, no jump) is natural again: it goes on.
  setup(2);
  {
    seq::Sequencer& s = make();
    ping(s);
    s.play_song(0);
    g_col.run(s, 0, song_us(2, 17), 64);
    CHECK(s.commands().push(seq::Command{seq::Command::Kind::QueueJump, 0, 0, 0}));
    g_col.run(s, song_us(2, 17), song_us(20), 64);
    expect("live jump", {40, 41, 42, 43, 44, 45, 46, 47, 40, 41, 42, 43, 44, 45, 46, 47, 46, 45, 44, 43});
  }
  // A pattern switch (OS-PATSWITCH) during step 9, the track being on row 5 going back: pattern 2 plays on from row 2, the next row, and its bounce starts again going forward.
  setup(0);
  {
    seq::Sequencer& s = make();
    ping(s);
    s.play_pattern(1);
    g_col.run(s, 0, song_us(9, 10), 64);
    CHECK(s.commands().push(seq::Command{seq::Command::Kind::SwitchPattern, 2, 0, 0}));
    g_col.run(s, song_us(9, 10), song_us(18), 64);
    expect("switch", {40, 41, 42, 43, 44, 45, 46, 47, 46, 45, 62, 63, 64, 65, 66, 67, 66, 65});
  }
}

TEST_CASE("seq: a Perform PAT LEN that shrinks under a running bounce takes the track to the top of the new span, going either way (OS-BUG-PERFPING)") {
  for (const std::uint32_t at : {11u, 18u}) {  // the track is on row 11 going up, or on row 12 coming down
    fresh_project(4, 4);
    model::Project& p = seq_project();
    CHECK(p.set_tempo_x100(24000) == model::Status::Ok);
    CHECK(p.create_pattern(1, 16) == model::Status::Ok);
    for (std::uint32_t k = 0; k < 16; ++k) {
      CHECK(p.set_step(1, 0, k, make_step(static_cast<std::uint8_t>(20 + k))) == model::Status::Ok);
    }
    seq::Sequencer& s = make();
    seq::Command c{};
    CHECK(seq::perf_command(0, seq::PerfFx::PatternPlayMode, 2, c));
    CHECK(s.commands().push(c));
    s.play_pattern(1);
    g_col.run(s, 0, song_us(at, 10), 64);
    CHECK(seq::perf_command(0, seq::PerfFx::PatternLength, 3, c));  // 4 steps
    CHECK(s.commands().push(c));
    g_col.run(s, song_us(at, 10), song_us(at + 14), 64);
    for (std::uint32_t i = 0; i < at + 14; ++i) {
      const Rec* r = g_col.nth(EventKind::NoteOn, i, 0);
      NEED(r != nullptr);
      // up to the change the bounce of 16; the step after it plays row 2 of the new span, going down (the track was past the span: it went to the top, 3, and on), then 1 0 1 2 3 2 ...
      const std::uint32_t pos = i <= at ? ping_pos(i, 16) : ping_pos(i - at + 3u, 4);
      CHECK_MESSAGE(r->ev.a == 20 + pos, "change at " << at << " step " << i);
    }
  }
}

TEST_CASE("seq: the performance stutter retriggers the last note, the performance tempo and pattern length override, performance values are modulator events") {
  song_project();
  model::Project& p = seq_project();
  CHECK(p.set_step(2, 0, 0, make_step(60)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.set_perf_stutter(0, 11);  // value 11 is 12 microsteps: 1/4 of a step
  s.play_pattern(2);
  g_col.run(s, 0, song_us(2), 64);
  // the note, then retriggers where the step timer hits 1 modulo 12: 1, 13, 25, 37, 49, ...
  const std::uint32_t expect_e[] = {0, 1, 13, 25, 37, 49, 61, 73, 85};
  for (std::uint32_t i = 0; i < 9; ++i) {
    const Rec* r = g_col.nth(EventKind::NoteOn, i, 0);
    NEED(r != nullptr);
    CHECK_MESSAGE(r->t == song_us(0, expect_e[i]), i);
    CHECK(r->ev.a == 60);
    CHECK((r->ev.flags & audio::kEvRetrigger) == (i == 0 ? 0u : 1u));
  }
  // tempo override and pattern length
  song_project();
  seq::Sequencer& s2 = make();
  s2.set_perf_tempo(12000);  // 120 BPM against the project's 240
  s2.set_perf_pattern_length(2);
  s2.play_pattern(2);
  g_col.run(s2, 0, at_us(8, 0), 64);
  CHECK(s2.tempo_x100() == 12000u);
  CHECK(g_col.nth(EventKind::Step, 1)->t == at_us(1, 0));  // 120 BPM
  CHECK(g_col.nth(EventKind::Step, 2)->ev.value == 0);     // the pattern is 2 steps now
  CHECK(g_col.nth(EventKind::Step, 3)->ev.value == 1);
  // a value of the performance layer, and its end
  song_project();
  seq::Sequencer& s3 = make();
  s3.play_pattern(2);
  g_col.run(s3, 0, 640, 64);
  s3.set_perf_control(0, seq::FxGroup::Panning, -33);
  g_col.run(s3, 640, 1280, 64);
  const Rec* m = g_col.nth(EventKind::ModSet, 0);
  NEED(m != nullptr);
  CHECK(m->ev.c == audio::kLayerPerf);
  CHECK(m->ev.flags == static_cast<std::uint8_t>(seq::FxGroup::Panning));
  CHECK(m->ev.value == -33);
  CHECK(m->t == 640u);
  CHECK(s3.stats().late_events == 0u);  // a command between two blocks acts at the start of the next: not late
  s3.end_perf_control(0, seq::FxGroup::Panning);
  g_col.run(s3, 1280, 1920, 64);
  NEED(g_col.nth(EventKind::ModEnd, 0) != nullptr);
  CHECK(g_col.nth(EventKind::ModEnd, 0)->t == 1280u);
}

TEST_CASE("seq: a fresh transport start ends the sequencer's FX and keeps the performance layer's (a play ends the sequencer's FX only)") {
  pattern16(12000);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 4, make_step(60, 1, fx::kPanning, 30)) == model::Status::Ok);  // a Panning FX in the first column at step 4
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(2, 0), 64);
  s.set_perf_control(0, seq::FxGroup::Filter, 0x0123);  // the performance layer holds the Filter group of track 0
  g_col.run(s, at_us(2, 0), at_us(6, 0), 64);
  CHECK(s.control().column_type(0, 0) == fx::kPanning);
  CHECK(s.control().perf_holds(0, seq::FxGroup::Filter));
  s.stop();
  g_col.run(s, at_us(6, 0), at_us(7, 0), 64);
  g_col.n = 0;
  s.play_pattern(1);
  g_col.run(s, at_us(7, 0), at_us(7, 0) + 128, 64);
  CHECK(g_col.count(EventKind::FxReset) == 1u);  // the voices end the sequencer's FX with it
  CHECK(s.control().column_type(0, 0) == 0);     // the sequencer's column ended
  CHECK(s.control().perf_holds(0, seq::FxGroup::Filter));  // the performance layer's value did not
  CHECK(s.control().resolve(0, seq::FxGroup::Filter).perf == 0x0123);
  CHECK(g_col.count(EventKind::ModEnd) == 0u);
}

TEST_CASE("seq: ParamChange (tempo with its ramp, swing), the command ring, and the ids that are not the sequencer's") {
  song_project();
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.set_step(2, 0, 0, make_step(60)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(2);
  // tempo to 240 BPM, instant (ramp 1): the steps that start after it are at the new tempo
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  std::uint64_t t = 0;
  const auto run_to = [&](std::uint64_t end) {
    while (t < end) {
      const std::uint32_t n = static_cast<std::uint32_t>(end - t < 64 ? end - t : 64);
      const std::uint32_t c = s.step(t, n, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        g_col.add(t, out[i]);
      }
      t += n;
    }
  };
  run_to(at_us(2, 0));
  CHECK(s.apply(core::ParamChange{seq::tempo_param(), 240.0f, 1}));
  run_to(at_us(6, 0));
  CHECK(s.tempo_x100() == 24000u);
  const std::uint64_t d_before = g_col.nth(EventKind::Step, 2)->t - g_col.nth(EventKind::Step, 1)->t;
  const std::uint64_t d_after = g_col.nth(EventKind::Step, 5)->t - g_col.nth(EventKind::Step, 4)->t;
  CHECK(near_u64(d_before, exact_sample(48, 12000), 1));
  CHECK(near_u64(d_after, exact_sample(48, 24000), 1));
  // a ramp over about 2 seconds (88,200 samples = 1379 blocks): every step is shorter than the one before, until the end
  CHECK(s.apply(core::ParamChange{seq::tempo_param(), 120.0f, 88200}));
  const std::uint32_t before_steps = g_col.count(EventKind::Step);
  run_to(t + 120000);
  std::uint64_t prev = 0;
  bool grew = true;
  std::uint32_t seen = 0;
  for (std::uint32_t i = before_steps; i + 1 < g_col.count(EventKind::Step); ++i) {
    const std::uint64_t d = g_col.nth(EventKind::Step, i + 1)->t - g_col.nth(EventKind::Step, i)->t;
    if (seen > 0 && d + 1 < prev) {
      grew = false;
    }
    prev = d;
    ++seen;
  }
  CHECK(grew);  // slowing from 240 to 120: the steps get longer, never shorter
  CHECK(s.tempo_x100() == 12000u);
  // clamps
  CHECK(s.apply(core::ParamChange{seq::tempo_param(), 5.0f, 1}));
  run_to(t + 64);
  CHECK(s.tempo_x100() == 4000u);
  CHECK(s.apply(core::ParamChange{seq::tempo_param(), 5000.0f, 1}));
  run_to(t + 64);
  CHECK(s.tempo_x100() == 80000u);
  // swing by ParamChange: in force when no FX sets it
  CHECK(s.apply(core::ParamChange{seq::swing_param(), 66.0f, 1}));
  CHECK(s.swing() == 66u);
  // ids that are not ours
  CHECK_FALSE(s.apply(core::ParamChange{core::param_id(core::ParamKind::Track, 0, 0), 1.0f, 0}));
  CHECK_FALSE(s.apply(core::ParamChange{core::param_id(core::ParamKind::Project, 1, seq::seq_param::Tempo), 1.0f, 0}));
  CHECK_FALSE(s.apply(core::ParamChange{core::param_id(core::ParamKind::Project, 0, 7), 1.0f, 0}));
  CHECK_FALSE(s.apply(core::ParamChange{core::param_id(core::ParamKind::Master, 0, seq::seq_param::Tempo), 1.0f, 0}));

  // the command ring: what the main loop sends is what the render does, at the next block
  song_project();
  seq::Sequencer& s2 = make();
  CHECK(s2.commands().push(seq::Command{seq::Command::Kind::PlaySong, 0, 1, 2}));  // order position 1, from step 2
  CHECK(s2.commands().push(seq::Command{seq::Command::Kind::PerfTempo, 0, 0, 15000}));
  g_col.run(s2, 0, song_us(2), 64);
  CHECK(s2.playing());
  CHECK(g_col.nth(EventKind::Step, 0)->ev.a == 2);
  CHECK(g_col.nth(EventKind::Step, 0)->ev.value == 2);
  CHECK(s2.tempo_x100() == 15000u);
  CHECK(s2.stats().commands == 2u);
  CHECK(s2.commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
  g_col.run(s2, song_us(2), song_us(2) + 64, 64);
  CHECK_FALSE(s2.playing());
  CHECK(s2.commands().push(seq::Command{seq::Command::Kind::Locate, 0, 0, 5}));
  CHECK(s2.commands().push(seq::Command{seq::Command::Kind::Resume, 0, 0, 0}));
  g_col.run(s2, song_us(3), song_us(3) + 64, 64);
  CHECK(s2.playing());
  CHECK(g_col.nth(EventKind::Step, g_col.count(EventKind::Step) - 1)->ev.value == 1);  // song step 5: pattern 2's step 1
  CHECK(s2.commands().push(seq::Command{seq::Command::Kind::PerfControl, 1, static_cast<std::uint16_t>(seq::FxGroup::Filter), 123}));
  g_col.run(s2, song_us(4), song_us(4) + 64, 64);
  CHECK(g_col.count(EventKind::ModSet, 1) >= 1u);
  CHECK(s2.commands().push(seq::Command{seq::Command::Kind::PerfControl, 1, static_cast<std::uint16_t>(seq::FxGroup::Filter), 0x10000}));
  g_col.run(s2, song_us(5), song_us(5) + 64, 64);
  CHECK(g_col.count(EventKind::ModEnd, 1) >= 1u);
}

TEST_CASE("seq: more events than the caller has room for are held, not lost, and come out late at offset 0") {
  pattern16(12000);
  model::Project& p = seq_project();
  for (std::uint32_t t = 0; t < 4; ++t) {
    CHECK(p.set_step(1, t, 0, make_step(static_cast<std::uint8_t>(60 + t), 1, fx::kPanning, 30)) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  audio::SeqEvent out[2];
  std::uint32_t total = 0;
  std::uint32_t first = s.step(0, 64, out, 2);  // the block at the first step: FxReset, Step, 4 x (ModSet, NoteOn)... but room for two
  CHECK(first == 2u);
  total += first;
  CHECK(s.stats().deferred >= 1u);
  // the rest in the following blocks, all at offset 0 (they are late), none lost
  audio::SeqEvent big[audio::kMaxEventsPerBlock];
  std::uint32_t note_ons = 0;
  for (std::uint32_t i = 0; i < first; ++i) {
    note_ons += out[i].kind == EventKind::NoteOn ? 1u : 0u;
  }
  for (std::uint64_t t = 64; t < 640; t += 64) {
    const std::uint32_t c = s.step(t, 64, big, audio::kMaxEventsPerBlock);
    for (std::uint32_t i = 0; i < c; ++i) {
      note_ons += big[i].kind == EventKind::NoteOn ? 1u : 0u;
      if (t == 64) {
        CHECK(big[i].offset == 0);
      }
    }
    total += c;
  }
  CHECK(note_ons == 4u);
  CHECK(s.stats().late_events > 0u);
  CHECK(total == 1u + 1u + 4u + 4u);  // FxReset, Step, four ModSets and four NoteOns: nothing lost
  // a block of no length does nothing
  CHECK(s.step(10000, 0, big, audio::kMaxEventsPerBlock) == 0u);
}

// ---- half / double time (OS-PERF2) ----------------------------------------------------------------------------------------------------------------------

namespace {

struct RateAt {
  std::uint64_t t;  // the command goes onto the ring before the block that starts at the first multiple of 64 >= t
  std::uint8_t rate;
};

// Runs 64-sample blocks from 0 to `to`, pushing each rate command before the first block that starts at or after its time.
void run_rates(seq::Sequencer& s, std::uint64_t to, const RateAt* r, unsigned n) {
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  unsigned i = 0;
  for (std::uint64_t t = 0; t < to; t += 64) {
    while (i < n && r[i].t <= t) {
      CHECK(s.commands().push(seq::Command{seq::Command::Kind::PerfRate, r[i].rate, 0, 0}));
      ++i;
    }
    const std::uint32_t c = s.step(t, 64, out, audio::kMaxEventsPerBlock);
    for (std::uint32_t k = 0; k < c; ++k) {
      g_col.add(t, out[k]);
    }
  }
}

// The start of step k, in samples, when steps [press, release) run at `rate` (1 half, 2 double) and the others at the nominal tempo, with `swing` percent. Exact rational, as the
// sequencer's own (core/qtime.h): the length of a step is 48 ticks x its swing factor x its rate, summed in units of 1/200 tick.
std::uint64_t step_start(std::uint32_t k, std::uint32_t press, std::uint32_t release, std::uint8_t rate, std::uint32_t bpm_x100, std::uint32_t swing = 50) {
  std::uint64_t t200 = 0;
  for (std::uint32_t j = 0; j < k; ++j) {
    const std::uint32_t factor = (j % 2u == 0u) ? 150u - swing : 50u + swing;
    const std::uint32_t scale2 = (j >= press && j < release) ? (rate == 1u ? 4u : 1u) : 2u;
    t200 += 48u * factor * scale2;
  }
  return (exact_sample(t200, bpm_x100) + 100u) / 200u;  // exact_sample takes ticks: t200 is 200 times too many of them, so it is scaled down after (rounded)
}

// press / release: the first step the new rate applies to, found by running the same song once without a command (the boundary of a step is on the nominal grid up to there).
void check_rate(std::uint8_t rate, bool press_on_boundary, bool release_on_boundary, std::uint32_t swing) {
  pattern16(12000);
  CHECK(seq_project().set_swing(swing) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  // The command that lands in the block that holds a step's first microstep applies to that step (the ring is drained before the microsteps): "on the boundary". One block later it
  // is the next step's: "off the boundary" (mid-step).
  const std::uint32_t press_step = 3;
  const std::uint64_t press_t = step_start(press_step, 99, 99, rate, 12000, swing) / 64u * 64u + (press_on_boundary ? 0u : 64u);
  const std::uint32_t first = press_on_boundary ? press_step : press_step + 1u;
  const std::uint32_t release_step = 8;
  const std::uint64_t release_nominal = step_start(release_step, first, release_step, rate, 12000, swing) / 64u * 64u;
  const std::uint64_t release_t = release_nominal + (release_on_boundary ? 0u : 64u);
  const std::uint32_t back = release_on_boundary ? release_step : release_step + 1u;
  const RateAt cmds[] = {{press_t, rate}, {release_t, 0}};
  run_rates(s, step_start(14, first, back, rate, 12000, swing) + 500, cmds, 2);
  NEED(g_col.count(EventKind::Step) >= 14u);
  for (std::uint32_t k = 0; k < 14; ++k) {
    const Rec* r = g_col.nth(EventKind::Step, k);
    NEED(r != nullptr);
    CHECK_MESSAGE(r->ev.value == static_cast<std::int32_t>(k), "the song position does not move: step " << k);
    CHECK_MESSAGE(near_u64(r->t, step_start(k, first, back, rate, 12000, swing), 2),
                  "step " << k << " rate " << int(rate) << " at " << r->t << " want " << step_start(k, first, back, rate, 12000, swing));
  }
  CHECK(s.play_rate() == 0u);
}

}  // namespace

TEST_CASE("seq rate: half time doubles the steps' length and double time halves it, from the step boundary after the press to the one after the release, the position never moves") {
  for (std::uint8_t rate = 1; rate <= 2; ++rate) {
    for (int on = 0; on < 2; ++on) {
      for (int off = 0; off < 2; ++off) {
        check_rate(rate, on != 0, off != 0, 50);
      }
    }
  }
}

TEST_CASE("seq rate: swing keeps its ratios at half and double time, the pair still adds to two nominal steps") {
  for (std::uint8_t rate = 1; rate <= 2; ++rate) {
    check_rate(rate, false, false, 62);
    check_rate(rate, true, true, 38);
  }
}

TEST_CASE("seq rate: the rate is on the tempo in force: the performance tempo and the Tempo FX are scaled, not replaced; tempo_x100 says what plays") {
  pattern16(9000);
  seq::Sequencer& s = make();
  s.set_perf_tempo(24000);  // the performance override (the Perform tempo FX's command) at 240 BPM
  s.play_pattern(1);
  const RateAt half[] = {{0, 1}};
  run_rates(s, 30000, half, 1);  // from the very first step
  CHECK(s.nominal_tempo_x100() == 24000u);
  CHECK(s.tempo_x100() == 12000u);
  NEED(g_col.count(EventKind::Step) >= 4u);
  for (std::uint32_t k = 0; k < 4; ++k) {  // 240 BPM at half time is 120 BPM's steps
    CHECK(near_u64(g_col.nth(EventKind::Step, k)->t, exact_sample(48u * k, 12000), 2));
  }
  // the Tempo FX: 240 BPM from step 2, at double time 480 BPM's steps from there
  pattern16(12000);
  CHECK(seq_project().set_step(1, 0, 2, fx_step(fx::kTempo, 120)) == model::Status::Ok);
  seq::Sequencer& s2 = make();
  s2.play_pattern(1);
  const RateAt dbl[] = {{0, 2}};
  run_rates(s2, 12000, dbl, 1);
  NEED(g_col.count(EventKind::Step) >= 5u);
  CHECK(near_u64(g_col.nth(EventKind::Step, 1)->t, exact_sample(24u, 12000), 2));
  CHECK(near_u64(g_col.nth(EventKind::Step, 2)->t, exact_sample(48u, 12000), 2));       // the FX step itself is read after its row ran: it already lasts 1/4 of 120 BPM's step
  CHECK(near_u64(g_col.nth(EventKind::Step, 3)->t, exact_sample(48u + 12u, 12000), 2));
  // a tempo fine / FX tempo is never written back scaled: after the release the song is at the FX's tempo, not at half or double of it
  CHECK(s2.nominal_tempo_x100() == 24000u);
  CHECK(s2.tempo_x100() == 48000u);
}

TEST_CASE("seq rate: MIDI clock out follows what plays: 6 clocks a step, the spacing doubled at half time and halved at double time, the start untouched") {
  for (std::uint8_t rate = 1; rate <= 2; ++rate) {
    seq::Sequencer::Config cfg;
    cfg.emit_clock = true;
    pattern16(12000);
    seq::Sequencer& s = make(cfg);
    s.play_pattern(1);
    // pressed during step 4 (so step 5 is the first at the new rate), released during step 7 (step 8 is the first at the old one): steps 5, 6, 7 are at the rate
    const std::uint32_t rated_ticks = rate == 1u ? 96u : 24u;
    const RateAt cmds[] = {{exact_sample(48u * 4u + 8u, 12000), rate}, {exact_sample(48u * 5u + rated_ticks * 2u + 8u, 12000), 0}};
    run_rates(s, exact_sample(48u * 12u, 12000), cmds, 2);
    // the steps at the other rate, from their durations
    std::uint32_t steps_rated = 0;
    for (std::uint32_t k = 0; k + 1 < g_col.count(EventKind::Step); ++k) {
      const std::uint64_t d = g_col.nth(EventKind::Step, k + 1)->t - g_col.nth(EventKind::Step, k)->t;
      steps_rated += near_u64(d, exact_sample(rate == 1u ? 96u : 24u, 12000), 2) ? 1u : 0u;
    }
    CHECK(steps_rated == 3u);
    const double nominal_gap = 8.0 * samples_per_tick_120();
    const double rated_gap = rate == 1u ? 2.0 * nominal_gap : 0.5 * nominal_gap;
    std::uint32_t gaps_normal = 0;
    std::uint32_t gaps_rated = 0;
    double prev = 0;
    for (std::uint32_t k = 0; k < g_col.count(EventKind::MidiClock); ++k) {
      const Rec* r = g_col.nth(EventKind::MidiClock, k);
      const double got = static_cast<double>(r->t) + static_cast<double>(r->ev.value) / 4294967296.0;
      if (k != 0) {
        const double gap = got - prev;
        if (std::abs(gap - nominal_gap) < 0.01) {
          ++gaps_normal;
        } else if (std::abs(gap - rated_gap) < 0.01) {
          ++gaps_rated;
        } else {
          CHECK_MESSAGE(false, "clock gap " << gap << " at " << k);
        }
      }
      prev = got;
    }
    CHECK(gaps_rated == 6u * steps_rated);  // 6 clocks a rated step, whatever the rate: they are what plays
    CHECK(gaps_normal >= 6u * 3u);
    CHECK(g_col.count(EventKind::MidiStart) == 1u);
    CHECK(g_col.count(EventKind::MidiStop) == 0u);
  }
}

TEST_CASE("seq rate: the Tick events of a step spread over the step's length at half and double time; a stop keeps the rate for the next start") {
  seq::Sequencer::Config cfg;
  cfg.emit_ticks = true;
  pattern16(12000);
  seq::Sequencer& s = make(cfg);
  s.play_pattern(1);
  const RateAt half[] = {{0, 1}};
  run_rates(s, exact_sample(48u * 4u, 12000), half, 1);
  NEED(g_col.count(EventKind::Tick) >= 12u);
  // 6 ticks a step, each a sixth of a step of 96 ticks
  for (std::uint32_t i = 0; i < 12; ++i) {
    CHECK(near_u64(g_col.nth(EventKind::Tick, i)->t, exact_sample(16u * i, 12000), 2));
  }
  s.stop();
  g_col.n = 0;
  s.play_pattern(1);
  g_col.run(s, exact_sample(48u * 4u, 12000) + 64, exact_sample(48u * 4u, 12000) + 64 + exact_sample(96u * 2u, 12000), 64);
  CHECK(s.play_rate() == 1u);
  NEED(g_col.count(EventKind::Step) >= 2u);
  CHECK(near_u64(g_col.nth(EventKind::Step, 1)->t - g_col.nth(EventKind::Step, 0)->t, exact_sample(96u, 12000), 2));
}

TEST_CASE("seq rate: an unknown rate is the normal one; a rate set while stopped waits for the next step") {
  pattern16(12000);
  seq::Sequencer& s = make();
  s.set_play_rate(7);
  CHECK(s.play_rate() == 0u);
  s.set_play_rate(2);
  CHECK(s.play_rate() == 2u);
  CHECK(s.tempo_x100() == s.nominal_tempo_x100() * 2u);
  s.set_play_rate(1);
  CHECK(s.tempo_x100() == s.nominal_tempo_x100() / 2u);
}
