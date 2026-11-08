// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CECHANCE C1 (docs/specs/OS-CECHANCE.md), the sequencer's side, on the host and on the Cortex-M4F (`qemu_seq_model`):
//   * the values: 0..100 a probability with tracker-ce's rule (a draw in 0..99 below the value plays: 0 never, 50 half, 100 always), 101 Group with Last, 102 Not Last,
//     103..138 Play N / Skip M and 139..174 Skip N / Play M (N, M 1..6, counted in passes of the pattern); every value decodes to its N, M;
//   * the per-track state: last starts true, 101 / 102 follow it and do not update it; a cancelled row runs none of its FX (no note, no tempo, no CC); only the first
//     Chance of a row is evaluated (column 2, then column 1); Random value never touches a Chance column;
//   * the pass counting (Q4 a): each track's own loop — Forward, Backward, PingPong, Random every len rows — a polymeter pair (8 against 16), a pattern restart, a song
//     slot repeated (counts on) or changed (resets the pass, keeps last), a Perform track's own pattern switch (that track only);
//   * the resets: Stop and Play, a locate (pass 0, last kept);
//   * only the values 0..100 draw from the RNG: the conditions 101..174 leave the stream alone;
//   * a cancelled row still ends the PREVIOUS row's tracker FX (OS-COREFX): the type-0 TrkFx of the column goes out on the cancelled row (review MAJOR: the early return in pre_fx skipped it).
#include <doctest/doctest.h>

#include <o3os/seq/sequencer.h>

#include <cstdint>
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

constexpr std::uint32_t kBpm = 12000;

seq::Sequencer& make() {
  seq::Sequencer& s = g_slot.make(seq_project());
  g_col.n = 0;
  g_col.total_events = 0;
  g_col.hash = 0xCBF29CE484222325ull;
  return s;
}

// A fresh project of one pattern (id 1, `len` steps) at 120 BPM.
void pattern(std::uint32_t len) {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(kBpm) == model::Status::Ok);
  CHECK(p.create_pattern(1, len) == model::Status::Ok);
}

// The exact sample of step n (n x 48 ticks) at 120 BPM, from the rational (core/qtime.h).
std::uint64_t exact_sample(std::uint64_t ticks) {
  const std::uint64_t n = ticks * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t d = 4u * core::kRefCyclesPerSample.num * static_cast<std::uint64_t>(kBpm);
  return (2u * n + d) / (2u * d);
}
std::uint64_t at_us(std::uint32_t step, std::uint32_t micro = 0) { return exact_sample(48u * step + micro); }

const Rec* find(const Collector& c, EventKind k, std::uint8_t track, std::uint64_t t) {
  for (std::uint32_t i = 0; i < c.n; ++i) {
    if (c.rec[i].ev.kind == k && c.rec[i].ev.track == track && c.rec[i].t == t) {
      return &c.rec[i];
    }
  }
  return nullptr;
}

// A note started on `track` at the start of `step` (the run began at sample 0).
bool note_at(std::uint8_t track, std::uint32_t step) { return find(g_col, EventKind::NoteOn, track, at_us(step)) != nullptr; }

bool queue_tracks(seq::Sequencer& s, std::uint32_t mask, std::uint32_t pattern) {
  return s.commands().push(seq::Command{seq::Command::Kind::QueueTrackPattern, 0, static_cast<std::uint16_t>(pattern), mask});
}

}  // namespace

TEST_CASE("seq chance: every value 101..174 decodes right: 101 / 102 the with-last conditions, 103..138 Play N Skip M, 139..174 Skip N Play M") {
  CHECK(fx::kChanceMax == 174u);
  CHECK(fx::kChanceWithLast == 101u);
  CHECK(fx::kChanceNotLast == 102u);
  for (std::uint32_t v = 103u; v <= 138u; ++v) {
    std::uint32_t n = 0;
    std::uint32_t m = 0;
    fx::chance_cycle(static_cast<std::uint8_t>(v), n, m);
    const std::uint32_t i = v - 103u;
    CHECK_MESSAGE(n == 1u + i / 6u, v);
    CHECK_MESSAGE(m == 1u + i % 6u, v);
  }
  for (std::uint32_t v = 139u; v <= 174u; ++v) {
    std::uint32_t n = 0;
    std::uint32_t m = 0;
    fx::chance_cycle(static_cast<std::uint8_t>(v), n, m);
    const std::uint32_t i = v - 139u;
    CHECK_MESSAGE(n == 1u + i / 6u, v);
    CHECK_MESSAGE(m == 1u + i % 6u, v);
  }
  std::uint32_t n = 0;
  std::uint32_t m = 0;
  fx::chance_cycle(103u, n, m);
  CHECK(n == 1u);
  CHECK(m == 1u);  // Play 1 Skip 1
  fx::chance_cycle(138u, n, m);
  CHECK(n == 6u);
  CHECK(m == 6u);  // Play 6 Skip 6
  fx::chance_cycle(139u, n, m);
  CHECK(n == 1u);
  CHECK(m == 1u);  // Skip 1 Play 1
  fx::chance_cycle(174u, n, m);
  CHECK(n == 6u);
  CHECK(m == 6u);  // Skip 6 Play 6
}

TEST_CASE("seq chance: Play 1 Skip 1 (103) alternates passes: of 4 passes of a 16-step pattern, passes 1 and 3 sound") {
  pattern(16);
  model::Project& p = seq_project();
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 103)) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(16 * 4), 64);
  for (std::uint32_t st = 0; st < 64u; ++st) {
    CHECK_MESSAGE(note_at(0, st) == ((st / 16u) % 2u == 0u), st);  // pass 0 and 2 of 0..3
  }
  CHECK(g_col.count(EventKind::NoteOn, 0) == 32u);
}

TEST_CASE("seq chance: Play 2 Skip 3 (111) over 15 passes plays on the passes 0, 1, 5, 6, 10, 11") {
  pattern(4);
  model::Project& p = seq_project();
  for (std::uint32_t k = 0; k < 4; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 111)) == model::Status::Ok);  // 103 + 1*6 + 2: N 2, M 3
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(4 * 15), 64);
  for (std::uint32_t st = 0; st < 60u; ++st) {
    CHECK_MESSAGE(note_at(0, st) == ((st / 4u) % 5u < 2u), st);
  }
  CHECK(g_col.count(EventKind::NoteOn, 0) == 4u * 6u);
}

TEST_CASE("seq chance: Skip 3 Play 2 (152) over 15 passes plays on the passes 3, 4, 8, 9, 13, 14") {
  pattern(4);
  model::Project& p = seq_project();
  for (std::uint32_t k = 0; k < 4; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 152)) == model::Status::Ok);  // 139 + 2*6 + 1: Skip 3, Play 2
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(4 * 15), 64);
  for (std::uint32_t st = 0; st < 60u; ++st) {
    CHECK_MESSAGE(note_at(0, st) == ((st / 4u) % 5u >= 3u), st);
  }
  CHECK(g_col.count(EventKind::NoteOn, 0) == 4u * 6u);
}

TEST_CASE("seq chance: 101 and 102 follow the last result, do not update it, and last starts true (101 plays on the first row)") {
  // last starts true: a row of 101 alone plays
  pattern(16);
  model::Project& p = seq_project();
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 101)) == model::Status::Ok);
  }
  {
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(16), 64);
    CHECK(g_col.count(EventKind::NoteOn, 0) == 16u);
  }
  // the sequence: 100 plays (last true), 101 plays (not stored), 102 does not, 101 plays, 0 never (last false), 102 plays, 101 and 101 do not
  pattern(16);
  const std::uint8_t vals[8] = {100, 101, 102, 101, 0, 102, 101, 101};
  const bool want[8] = {true, true, false, true, false, true, false, false};
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, vals[k % 8u])) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(16 * 2), 64);
  for (std::uint32_t st = 0; st < 32u; ++st) {
    CHECK_MESSAGE(note_at(0, st) == want[st % 8u], st);
  }
}

TEST_CASE("seq chance: a cancelled row runs none of its FX (Q2 a): no note, no tempo, no CC") {
  pattern(16);
  model::Project& p = seq_project();
  // step 0: a Tempo FX beside a failing Chance (0 never plays): the tempo does not apply, the note does not start
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kTempo, 90, fx::kChance, 0)) == model::Status::Ok);
  // step 1: a CC beside a failing Chance
  CHECK(p.set_step(1, 0, 1, make_step(61, 1, fx::kCcA, 99, fx::kChance, 0)) == model::Status::Ok);
  // step 2: the control — Chance 100 in column 2 plays, and the Tempo of column 1 applies (180 BPM from its own row)
  CHECK(p.set_step(1, 0, 2, make_step(62, 1, fx::kTempo, 90, fx::kChance, 100)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(4), 64);  // steps 0..2 (step 2 already at 180 BPM, so the horizon holds no later row with a note)
  CHECK(g_col.count(EventKind::NoteOn, 0) == 1u);
  CHECK(!note_at(0, 0));
  CHECK(!note_at(0, 1));
  CHECK(note_at(0, 2));
  CHECK(g_col.count(EventKind::MidiCc) == 0u);  // the CC of the cancelled row never went out
  CHECK(s.tempo_x100() == 18000u);               // only the playing row's Tempo applied
}

TEST_CASE("seq chance: two Chance columns on one row — only the first (column 2, then column 1) is evaluated (Q3 a)") {
  pattern(16);
  model::Project& p = seq_project();
  // column 2 holds Chance 100, column 1 a failing Chance 0: the row plays
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kChance, 0, fx::kChance, 100)) == model::Status::Ok);
  // column 2 holds the failing Chance 0, column 1 a Chance 100: the row is cancelled
  CHECK(p.set_step(1, 0, 1, make_step(61, 1, fx::kChance, 100, fx::kChance, 0)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(4), 64);
  CHECK(note_at(0, 0));
  CHECK(!note_at(0, 1));
  CHECK(g_col.count(EventKind::NoteOn, 0) == 1u);
}

TEST_CASE("seq chance: Random value beside a Chance leaves the Chance value alone (Q3 a)") {
  pattern(16);
  model::Project& p = seq_project();
  for (std::uint32_t k = 0; k < 16; ++k) {
    // a Random value of 255 over a Chance 100: had it written the Chance column, some rows would fail
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kRandomValue, 255, fx::kChance, 100)) == model::Status::Ok);
  }
  {
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(16 * 2), 64);
    CHECK(g_col.count(EventKind::NoteOn, 0) == 32u);
  }
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 100, fx::kRandomValue, 255)) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(16 * 2), 64);
  CHECK(g_col.count(EventKind::NoteOn, 0) == 32u);
}

TEST_CASE("seq chance: a pass follows each track's own loop (Q4 a): a polymeter pair, 8 against 16") {
  pattern(16);
  model::Project& p = seq_project();
  CHECK(p.set_track_length(1, 0, 8) == model::Status::Ok);  // track 0 loops every 8 steps, track 1 every 16
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 103)) == model::Status::Ok);
    CHECK(p.set_step(1, 1, k, make_step(61, 1, fx::kChance, 103)) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(32), 64);
  for (std::uint32_t st = 0; st < 32u; ++st) {
    CHECK_MESSAGE(note_at(0, st) == ((st / 8u) % 2u == 0u), st);
    CHECK_MESSAGE(note_at(1, st) == ((st / 16u) % 2u == 0u), st);
  }
}

TEST_CASE("seq chance: Backward, PingPong and Random modes each count their own passes") {
  // Backward, length 4: the rows 3, 2, 1, 0 and a pass end every 4 steps
  // PingPong, length 4: 0, 1, 2, 3, 2, 1 — a pass end every 6 steps
  // Random, length 5: a pass end every 5 rows
  struct ModeCase {
    seq::TrackPlayMode mode;
    std::uint32_t len;
    std::uint32_t period;  // steps per pass
    std::uint32_t steps;   // how many steps to check
  };
  const ModeCase cases[] = {
      {seq::TrackPlayMode::Backward, 4, 4, 12},
      {seq::TrackPlayMode::PingPong, 4, 6, 13},
      {seq::TrackPlayMode::Random, 5, 5, 15},
  };
  for (const ModeCase& c : cases) {
    pattern(16);
    model::Project& p = seq_project();
    for (std::uint32_t k = 0; k < 16; ++k) {
      CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 103)) == model::Status::Ok);
    }
    seq::Sequencer& s = make();
    s.set_perf_track(0, 0, c.len, c.mode);
    s.play_pattern(1);
    g_col.run(s, 0, at_us(c.steps), 64);
    const std::uint32_t tag = static_cast<std::uint32_t>(c.mode) * 100u;
    for (std::uint32_t st = 0; st < c.steps; ++st) {
      CHECK_MESSAGE(note_at(0, st) == ((st / c.period) % 2u == 0u), (tag + st));
    }
  }
}

TEST_CASE("seq chance: a song slot repeated counts on; a different pattern resets the pass and keeps last") {
  // song [1, 1, 2], every step of both patterns a Chance on track 0
  auto build = [](std::uint8_t p2_value) {
    fresh_project(4, 4);
    model::Project& p = seq_project();
    CHECK(p.set_tempo_x100(kBpm) == model::Status::Ok);
    CHECK(p.create_pattern(1, 16) == model::Status::Ok);
    CHECK(p.create_pattern(2, 16) == model::Status::Ok);
    CHECK(p.append_order(1) == model::Status::Ok);
    CHECK(p.append_order(1) == model::Status::Ok);
    CHECK(p.append_order(2) == model::Status::Ok);
    for (std::uint32_t k = 0; k < 16; ++k) {
      CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 103)) == model::Status::Ok);
      CHECK(p.set_step(2, 0, k, make_step(61, 1, fx::kChance, p2_value)) == model::Status::Ok);
    }
  };
  // pattern 2 with Play 1 Skip 1: slot 0 (pass 0) plays, slot 1 (the same pattern again, pass 1) does not, slot 2 (pattern 2, pass reset to 0) plays
  {
    build(103);
    seq::Sequencer& s = make();
    s.play_song(0);
    g_col.run(s, 0, at_us(48), 64);
    for (std::uint32_t st = 0; st < 48u; ++st) {
      const std::uint32_t slot = st / 16u;
      const bool want = slot == 0u || slot == 2u;
      CHECK_MESSAGE(note_at(0, st) == want, st);
    }
    CHECK(g_col.count(EventKind::NoteOn, 0) == 32u);
  }
  // pattern 2 with Group with Last: slot 1's last result was "no" (pass 1 of 103), the pattern change keeps it, so 101 does not play in slot 2
  {
    build(101);
    seq::Sequencer& s = make();
    s.play_song(0);
    g_col.run(s, 0, at_us(48), 64);
    CHECK(g_col.count(EventKind::NoteOn, 0) == 16u);  // slot 0 only
    for (std::uint32_t st = 16u; st < 48u; ++st) {
      CHECK_MESSAGE(!note_at(0, st), st);
    }
  }
}

TEST_CASE("seq chance: a Perform track's own pattern switch resets that track's pass only") {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(kBpm) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 4) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 103)) == model::Status::Ok);  // track 0: the pattern's 16 steps
    CHECK(p.set_step(1, 1, k, make_step(62, 1, fx::kChance, 103)) == model::Status::Ok);  // track 1's rows in pattern 1
  }
  for (std::uint32_t k = 0; k < 4; ++k) {
    CHECK(p.set_step(2, 1, k, make_step(61, 1, fx::kChance, 103)) == model::Status::Ok);  // track 1's rows in pattern 2 (4 steps)
  }
  seq::Sequencer& s = make();
  s.set_perf_track(1, 2, 0, seq::TrackPlayMode::Forward);  // track 1 reads pattern 2 from the start
  s.play_pattern(1);
  g_col.run(s, 0, at_us(2), 64);
  CHECK(queue_tracks(s, 0x2u, 1));  // track 1 switches to pattern 1 at its own loop end (the end of step 3)
  g_col.run(s, at_us(2), at_us(24), 64);
  for (std::uint32_t st = 0; st < 24u; ++st) {
    CHECK_MESSAGE(note_at(0, st) == (st < 16u), st);  // track 0: its pass runs 0..15 and 16..23, untouched by track 1's switch
    // track 1: pattern 2's pass 0 (steps 0..3), then the switch resets its pass to 0 on pattern 1 (steps 4..15; the pattern's restart at 16 cuts it, Q4 a)
    CHECK_MESSAGE(note_at(1, st) == (st < 16u), st);
  }
}

TEST_CASE("seq chance: Stop and Play reset the pass and last (F6)") {
  pattern(16);
  model::Project& p = seq_project();
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 103)) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(20), 64);  // pass 0 plays steps 0..15, pass 1 does not play 16..19
  CHECK(g_col.count(EventKind::NoteOn, 0) == 16u);
  s.stop();
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  (void)s.step(at_us(20), 64, out, audio::kMaxEventsPerBlock);
  s.play_pattern(1);  // a fresh start: pass 0 again
  g_col.n = 0;
  g_col.run(s, at_us(20), at_us(20) + at_us(16), 64);
  CHECK(g_col.count(EventKind::NoteOn, 0) == 16u);  // every step of the new run's first pass played
  NEED(g_col.nth(EventKind::NoteOn, 0, 0) != nullptr);
  CHECK(g_col.nth(EventKind::NoteOn, 0, 0)->t == at_us(20));
}

TEST_CASE("seq chance: a locate resets the pass and keeps last") {
  pattern(16);
  model::Project& p = seq_project();
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kChance, 103)) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(18), 64);  // pass 0 plays 0..15, pass 1 does not play 16..17
  CHECK(!note_at(0, 16));
  CHECK(!note_at(0, 17));
  s.locate_row(0);  // lands at the next step boundary: the pass counts from 0 again
  g_col.run(s, at_us(18), at_us(34), 64);
  CHECK(note_at(0, 18));  // the located row 0 with pass 0 plays (without the reset it would be pass 1, row 2: no)
  for (std::uint32_t st = 18u; st < 34u; ++st) {
    CHECK_MESSAGE(note_at(0, st), st);  // the whole located pass 0 plays
  }
}

TEST_CASE("seq chance: the conditions 101..174 draw nothing from the RNG (only 0..100 does)") {
  pattern(16);
  model::Project& p = seq_project();
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kRandomNote, 20)) == model::Status::Ok);
  }
  std::uint8_t notes[32];
  std::uint32_t nn = 0;
  {
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(16 * 2), 64);
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      if (g_col.rec[i].ev.kind == EventKind::NoteOn && g_col.rec[i].ev.track == 0u) {
        NEED(nn < 32u);
        notes[nn++] = g_col.rec[i].ev.a;
      }
    }
    CHECK(nn == 32u);
  }
  // the same rows with a Group with Last (101: always plays, never stores, never draws) beside the Random note: the same notes, the same stream
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(60, 1, fx::kRandomNote, 20, fx::kChance, 101)) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(16 * 2), 64);
  std::uint32_t nn2 = 0;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::NoteOn && g_col.rec[i].ev.track == 0u) {
      NEED(nn2 < 32u);
      CHECK_MESSAGE(g_col.rec[i].ev.a == notes[nn2], nn2);
      ++nn2;
    }
  }
  CHECK(nn2 == nn);
}

TEST_CASE("seq chance: a cancelled row still ends the previous row's tracker FX (Q2 a runs nothing of the row's own, but the OS-COREFX bookkeeping of the previous row's FX goes on)") {
  for (const std::uint8_t type : {fx::kTrkRetrig, fx::kTrkTremor}) {
    CAPTURE(int(type));
    pattern(16);
    model::Project& p = seq_project();
    CHECK(p.set_step(1, 0, 0, make_step(60, 1, type, 0x21)) == model::Status::Ok);              // row 0: the tracker FX in column 1
    CHECK(p.set_step(1, 0, 1, make_step(61, 1, 0, 0, fx::kChance, 0)) == model::Status::Ok);     // row 1: a note and a failing Chance (0 never plays) in column 2
    CHECK(p.set_step(1, 0, 2, make_step(62, 1)) == model::Status::Ok);                             // row 2: a plain note
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(3), 64);
    auto trk = [](std::uint64_t t, std::uint8_t a, std::uint8_t col) {
      for (std::uint32_t i = 0; i < g_col.n; ++i) {
        const Rec& r = g_col.rec[i];
        if (r.ev.kind == EventKind::TrkFx && r.ev.track == 0 && r.t == t && r.ev.a == a && r.ev.flags == col) {
          return true;
        }
      }
      return false;
    };
    CHECK(trk(at_us(0), type, 0));  // the effect starts on row 0
    CHECK(trk(at_us(1), 0, 0));     // its end goes out on the cancelled row 1 (before the fix the early return skipped it and the voice kept the FX through the cancelled row)
    CHECK(!trk(at_us(2), 0, 0));    // once: row 2 owes nothing (on the unfixed code the end lands here instead, so this fails too)
    CHECK(note_at(0, 0));
    CHECK(!note_at(0, 1));  // Q2 a stands: the cancelled row's note does not start
    CHECK(note_at(0, 2));
    CHECK(g_col.count(EventKind::TrkFx, 0) == 2u);
  }
}
