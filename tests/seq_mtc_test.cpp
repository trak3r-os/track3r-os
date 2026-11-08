// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MTC A3, the sequencer's half (docs/specs/OS-MTC.md section 3): the time code generator driven by a run. Offline Sequencer runs over a project: no events with emit_mtc off (and not one
// byte of the rest changes), the first full frame and the quarter frames on the exact sample from the song start, the count-in's time before the first step, silence until the main loop's
// answer for a start anywhere else (and a stale stamp dropped), the relocations (the song's loop, a locate), the stop's full frame, Pattern mode running on through its loop, a change of
// the settings. The main loop's side (app::MtcSync) is tests/mtc_sync_test.cpp. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/midi/mtc.h>
#include <o3os/seq/song_time.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace mtc = o3os::midi::mtc;

namespace {

StaticSlot<seq::Sequencer>& g_slot = seq_slot();
Collector& g_col = seq_collector();

constexpr bool kRate44100 = core::kRefCyclesPerSample.num == 1800000u && core::kRefCyclesPerSample.den == 441u;
constexpr std::uint64_t kLead = 64;       // start_lead: the first step is at this sample
constexpr std::uint64_t kLap = 88200;     // 16 steps at 120 BPM, 44,100 Hz: two seconds exactly
constexpr std::uint32_t kPeriod25 = 441;  // samples of a quarter frame at 25 fps

seq::Sequencer::Config mtc_cfg(std::uint32_t fps = 25, std::uint32_t hour = 1) {
  seq::Sequencer::Config c;
  c.emit_mtc = true;
  c.mtc_fps = static_cast<std::uint8_t>(fps);
  c.mtc_hour = static_cast<std::uint8_t>(hour);
  c.start_lead = kLead;
  return c;
}

seq::Sequencer& make_seq(const seq::Sequencer::Config& cfg) {
  seq::Sequencer& s = g_slot.make(seq_project());
  s.configure(cfg);
  g_col.n = 0;
  g_col.total_events = 0;
  g_col.hash = 0xCBF29CE484222325ull;
  return s;
}

// Two patterns of 16 steps, a song of [1, 2, 1], 120 BPM.
void two_patterns() {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 16) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
}

// What the main loop's app::MtcSync does with a request word: the song time of the position by the A1 map, as the command that answers it.
seq::Command answer_of(std::uint32_t request) {
  const std::uint32_t order = (request >> 16) & 0xFFu;
  const seq::SongPoint pt = order != 0xFFu ? seq::song_time_at(seq_project(), seq::PlayMode::Song, order, request & 0xFFu)
                                           : seq::song_time_at(seq_project(), seq::PlayMode::Pattern, (request >> 8) & 0xFFu, request & 0xFFu);
  return seq::Command{seq::Command::Kind::MtcAnchor, static_cast<std::uint8_t>(request >> 24), 0, static_cast<std::uint32_t>(core::q32_floor(pt.time))};
}

std::uint32_t count_mtc() { return g_col.count(EventKind::MidiMtcQf) + g_col.count(EventKind::MidiMtcFull); }

mtc::Time time_of(const Rec* r) { return mtc::unpack_time(static_cast<std::uint32_t>(r->ev.value)); }

}  // namespace

TEST_CASE("seq mtc: with emit_mtc off a run has no time code, whatever the rate says, and the default configuration is the same run") {
  two_patterns();
  seq::Sequencer::Config off = mtc_cfg();
  off.emit_mtc = false;  // fps set, switch off: the offline export's case
  seq::Sequencer& s = make_seq(off);
  s.play_song(0, 0);
  g_col.run(s, 0, 2 * kLap, 64);
  CHECK(count_mtc() == 0u);
  CHECK(g_col.total_events > 30u);
  const std::uint64_t hash_off = g_col.hash;
  // The default configuration is the same run.
  seq::Sequencer& s2 = make_seq(seq::Sequencer::Config{});
  seq::Sequencer::Config d;
  d.start_lead = kLead;
  s2.configure(d);
  s2.play_song(0, 0);
  g_col.run(s2, 0, 2 * kLap, 64);
  CHECK(count_mtc() == 0u);
  CHECK(g_col.hash == hash_off);
  CHECK_FALSE(s2.mtc().anchored());
  CHECK(s2.mtc_request() == 0u);
}

TEST_CASE("seq mtc: from the song start at 25 fps: the full frame at the first step, quarter frames on the exact sample, the pieces in order") {
  if (!kRate44100) {
    return;
  }
  two_patterns();
  seq::Sequencer& s = make_seq(mtc_cfg(25, 1));
  s.play_song(0, 0);
  g_col.run(s, 0, kLap + 4096, 64);
  const Rec* full = g_col.nth(EventKind::MidiMtcFull, 0);
  NEED(full != nullptr);
  CHECK(full->t == kLead);  // the sample of the first Step
  const Rec* step0 = g_col.nth(EventKind::Step, 0);
  NEED(step0 != nullptr);
  CHECK(step0->t == full->t);
  CHECK(time_of(full) == mtc::Time{1, 0, 0, 0});
  CHECK(mtc::unpack_code(static_cast<std::uint32_t>(full->ev.value)) == 1u);
  CHECK(g_col.count(EventKind::MidiMtcFull) == 1u);  // no relocation in the song's flow (slot 1 to slot 2)
  const std::uint32_t n_qf = g_col.count(EventKind::MidiMtcQf);
  NEED(n_qf > 150u);
  CHECK(s.mtc_request() == 0u);  // the fast path asked nobody
  for (std::uint32_t k = 0; k < n_qf; ++k) {
    const Rec* q = g_col.nth(EventKind::MidiMtcQf, k);
    NEED(q != nullptr);
    CHECK_MESSAGE(q->t == kLead + kPeriod25 * (k + 1u), "quarter frame " << k + 1u);
    CHECK(q->ev.value == 0);
    CHECK((q->ev.a >> 4) == ((k + 1u) & 7u));
    CHECK(q->ev.a == mtc::qf_byte(360000u + k + 1u, 25));
  }
  // The second pattern's step 1 is at 01:00:02:00 on the time code: the quarter frame at that sample is piece 0 of quarter frame 360,200.
  const Rec* at_lap = nullptr;
  for (std::uint32_t k = 0; k < n_qf; ++k) {
    const Rec* q = g_col.nth(EventKind::MidiMtcQf, k);
    if (q->t == kLead + kLap) {
      at_lap = q;
    }
  }
  NEED(at_lap != nullptr);
  CHECK(at_lap->ev.a == mtc::qf_byte(360200u, 25));
}

TEST_CASE("seq mtc: the time code is the wall clock, not the tempo: a Tempo FX moves the notes against it, never the quarter frames") {
  if (!kRate44100) {
    return;
  }
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 64) == model::Status::Ok);  // 64 steps: 4 at 120 BPM, then 60 at 180 BPM = 5.5 s, so the song's loop is past the run
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 4, fx_step(seq::fx::kTempo, 90)) == model::Status::Ok);  // 180 BPM from step 5 on
  seq::Sequencer& s = make_seq(mtc_cfg(30, 0));
  s.play_song(0, 0);
  g_col.run(s, 0, 3 * 44100, 64);
  // the notes moved against the time code: step 5 of the pattern (index 4, which holds the FX) lasts 3,675 samples, the first four lasted 5,512.5
  const Rec* st4 = g_col.nth(EventKind::Step, 4);
  const Rec* st5 = g_col.nth(EventKind::Step, 5);
  NEED(st4 != nullptr);
  NEED(st5 != nullptr);
  CHECK(st4->t == kLead + 22050u);
  CHECK(st5->t - st4->t == 3675u);
  CHECK(g_col.count(EventKind::MidiMtcFull) == 1u);
  const std::uint32_t n_qf = g_col.count(EventKind::MidiMtcQf);
  NEED(n_qf > 300u);
  for (std::uint32_t k = 0; k < n_qf; ++k) {
    const Rec* q = g_col.nth(EventKind::MidiMtcQf, k);
    NEED(q != nullptr);
    // 30 fps: 367.5 samples, the fraction in `value`: exact time = t + value / 2^32 = lead + 367.5 (k + 1)
    const std::int64_t exact = static_cast<std::int64_t>(q->t << 32) + q->ev.value;
    CHECK_MESSAGE(exact == static_cast<std::int64_t>((kLead << 32) + (core::Q32{367} << 32) * (k + 1u) + (core::Q32{0x80000000u}) * (k + 1u)), "quarter frame " << k + 1u);
  }
}

TEST_CASE("seq mtc: a count-in carries the time before the first step; MTC START 00 sends nothing before 00:00:00:00") {
  if (!kRate44100) {
    return;
  }
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  // one bar (16 steps = 2 s) of count-in, MTC START 01: the full frame at the run's start reads 00:59:58:00
  seq::Sequencer& s = make_seq(mtc_cfg(25, 1));
  s.play_pattern(1, 0, 1);
  g_col.run(s, 0, kLap + 4096, 64);
  const Rec* full = g_col.nth(EventKind::MidiMtcFull, 0);
  NEED(full != nullptr);
  CHECK(full->t == kLead);
  CHECK(time_of(full) == mtc::Time{0, 59, 58, 0});
  // the first real step (a Step without kEvCountIn) is at 01:00:00:00: the quarter frame that falls on its sample is piece 0 of 360,000
  std::uint64_t first_real = 0;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::Step && (g_col.rec[i].ev.flags & audio::kEvCountIn) == 0u) {
      first_real = g_col.rec[i].t;
      break;
    }
  }
  CHECK(first_real == kLead + kLap);
  bool on_step = false;
  for (std::uint32_t k = 0; k < g_col.count(EventKind::MidiMtcQf); ++k) {
    const Rec* q = g_col.nth(EventKind::MidiMtcQf, k);
    if (q->t == first_real) {
      on_step = q->ev.a == mtc::qf_byte(360000u, 25);
    }
  }
  CHECK(on_step);
  // MTC START 00: the same count-in; the time code waits for 00:00:00:00, which is the first real step
  seq::Sequencer& s0 = make_seq(mtc_cfg(25, 0));
  s0.play_pattern(1, 0, 1);
  g_col.run(s0, 0, kLap + 4096, 64);
  const Rec* f0 = g_col.nth(EventKind::MidiMtcFull, 0);
  NEED(f0 != nullptr);
  CHECK(f0->t == kLead + kLap);
  CHECK(time_of(f0) == mtc::Time{0, 0, 0, 0});
  CHECK(g_col.nth(EventKind::MidiMtcQf, 0)->t > f0->t);
}

TEST_CASE("seq mtc: a start anywhere but the song's first step asks the main loop and is silent until the answer; a stale stamp is dropped; the answer's time moves on with the samples") {
  if (!kRate44100) {
    return;
  }
  two_patterns();
  seq::Sequencer& s = make_seq(mtc_cfg(25, 1));
  s.play_song(1, 0);  // slot 2, step 1: song time 2 s
  g_col.run(s, 0, 320, 64);
  CHECK(count_mtc() == 0u);
  CHECK_FALSE(s.mtc().anchored());
  const std::uint32_t req = s.mtc_request();
  CHECK((req >> 24) != 0u);
  CHECK(((req >> 16) & 0xFFu) == 1u);  // the order position
  CHECK(((req >> 8) & 0xFFu) == 2u);   // its pattern
  CHECK((req & 0xFFu) == 0u);          // the step
  const seq::Command ans = answer_of(req);
  CHECK(ans.c == kLap);
  // a stale stamp: nothing
  seq::Command stale = ans;
  stale.a = static_cast<std::uint8_t>(ans.a + 1u);
  CHECK(s.commands().push(stale));
  g_col.run(s, 320, 640, 64);
  CHECK(count_mtc() == 0u);
  CHECK_FALSE(s.mtc().anchored());
  // the right one, taken at the block that starts at 640: the song time then is 2 s + (640 - 64) samples
  CHECK(s.commands().push(ans));
  g_col.run(s, 640, 640 + 4096, 64);
  const Rec* full = g_col.nth(EventKind::MidiMtcFull, 0);
  NEED(full != nullptr);
  CHECK(full->t == 640u);
  CHECK(time_of(full) == mtc::Time{1, 0, 2, 0});  // song time 88,200 + (640 - 64) samples = 2.013 s: frame 50 of the second hour's third second
  CHECK(s.mtc_request() == req);                    // nothing newer was asked
  // the grid is the song's own: every quarter frame is on 64 + 441 k, as from the start
  const std::uint32_t n_qf = g_col.count(EventKind::MidiMtcQf);
  NEED(n_qf > 5u);
  for (std::uint32_t k = 0; k < n_qf; ++k) {
    const Rec* q = g_col.nth(EventKind::MidiMtcQf, k);
    NEED(q != nullptr);
    CHECK_MESSAGE((q->t - kLead) % kPeriod25 == 0u, "quarter frame at " << q->t);
    CHECK(q->t > full->t);
  }
}

TEST_CASE("seq mtc: a stop sends one full frame with the stop time and then nothing; a new Play starts clean") {
  if (!kRate44100) {
    return;
  }
  two_patterns();
  seq::Sequencer& s = make_seq(mtc_cfg(25, 1));
  s.play_song(0, 0);
  g_col.run(s, 0, 44100, 64);
  const std::uint32_t before = g_col.count(EventKind::MidiMtcQf);
  NEED(before > 90u);
  CHECK(g_col.count(EventKind::MidiMtcFull) == 1u);
  s.stop();
  g_col.run(s, 44100, 44100 + 3 * 441 + 64, 64);
  CHECK(g_col.count(EventKind::MidiMtcFull) == 2u);
  const Rec* stop = g_col.nth(EventKind::MidiMtcFull, 1);
  NEED(stop != nullptr);
  CHECK(stop->t == 44100u);  // the block's first sample
  // song time at 44,100 (the lead is 64): 44,036 samples = 24.96 frames = frame 24; at 25 fps: 01:00:00:24
  CHECK(time_of(stop) == mtc::Time{1, 0, 0, 24});
  CHECK(g_col.count(EventKind::MidiMtcQf) == before);  // none after the stop
  CHECK_FALSE(s.mtc().anchored());
  // Play again: a new full frame at 01:00:00:00 and a fresh grid
  s.play_song(0, 0);
  g_col.run(s, 44100 + 3 * 441 + 64, 44100 + 3 * 441 + 64 + 2048, 64);
  CHECK(g_col.count(EventKind::MidiMtcFull) == 3u);
  CHECK(time_of(g_col.nth(EventKind::MidiMtcFull, 2)) == mtc::Time{1, 0, 0, 0});
}

TEST_CASE("seq mtc: the song's loop to slot 1 is a relocation: a new full frame and the grid from 01:00:00:00") {
  if (!kRate44100) {
    return;
  }
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  seq::Sequencer& s = make_seq(mtc_cfg(25, 1));
  s.play_song(0, 0);
  g_col.run(s, 0, 2 * kLap + 2048, 64);
  NEED(g_col.count(EventKind::MidiMtcFull) >= 2u);
  const Rec* loop = g_col.nth(EventKind::MidiMtcFull, 1);
  CHECK(loop->t == kLead + kLap);
  CHECK(time_of(loop) == mtc::Time{1, 0, 0, 0});
  // the first quarter frame after it is one period later: piece 1 of the new run
  const Rec* next = nullptr;
  for (std::uint32_t k = 0; k < g_col.count(EventKind::MidiMtcQf); ++k) {
    const Rec* q = g_col.nth(EventKind::MidiMtcQf, k);
    if (q->t > loop->t) {
      next = q;
      break;
    }
  }
  NEED(next != nullptr);
  CHECK(next->t == loop->t + kPeriod25);
  CHECK(next->ev.a == mtc::qf_byte(360001u, 25));
}

TEST_CASE("seq mtc: the slot-range loop's wrap relocates the time code at each wrap: a new full frame and the grid from 01:00:00:00") {
  if (!kRate44100) {
    return;
  }
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 16) == model::Status::Ok);
  CHECK(p.create_pattern(3, 16) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  CHECK(p.append_order(3) == model::Status::Ok);
  seq::Sequencer& s = make_seq(mtc_cfg(25, 1));
  s.set_song_loop(false);  // the range's loop is not the song's own: the wrap is the range's, not the list's end (no SongEnd)
  s.play_song(0, 0);
  g_col.run(s, 0, 64, 64);  // one block: the order list is adopted
  s.set_loop(0, 1);         // slots 0..1 loop: the song plays 1, 2, 1, 2, ... (a lap of the range is two patterns = 2 kLap)
  g_col.run(s, 64, kLead + 6 * kLap + 2048, 64);
  // a full frame at the run's start and at each of the three wraps, each from 01:00:00:00
  NEED(g_col.count(EventKind::MidiMtcFull) == 4u);
  for (std::uint32_t k = 0; k < 4u; ++k) {
    const Rec* full = g_col.nth(EventKind::MidiMtcFull, k);
    NEED(full != nullptr);
    CHECK(full->t == kLead + 2 * kLap * k);
    CHECK(time_of(full) == mtc::Time{1, 0, 0, 0});
    // the quarter frames re-grid from the wrap: the first after it is one period later, piece 1 of the new run
    const Rec* next = nullptr;
    for (std::uint32_t q = 0; q < g_col.count(EventKind::MidiMtcQf); ++q) {
      const Rec* f = g_col.nth(EventKind::MidiMtcQf, q);
      if (f->t > full->t) {
        next = f;
        break;
      }
    }
    NEED(next != nullptr);
    CHECK(next->t == full->t + kPeriod25);
    CHECK(next->ev.a == mtc::qf_byte(360001u, 25));
  }
}

TEST_CASE("seq mtc: Pattern mode runs on through the pattern's loop: no second full frame, the grid unbroken") {
  if (!kRate44100) {
    return;
  }
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  seq::Sequencer& s = make_seq(mtc_cfg(25, 1));
  s.play_pattern(1, 0, 0);
  g_col.run(s, 0, 2 * kLap + 2048, 64);
  CHECK(g_col.count(EventKind::MidiMtcFull) == 1u);
  CHECK(g_col.count(EventKind::Pattern) >= 2u);  // the pattern did loop
  const std::uint32_t n_qf = g_col.count(EventKind::MidiMtcQf);
  NEED(n_qf > 400u);
  for (std::uint32_t k = 0; k < n_qf; ++k) {
    CHECK(g_col.nth(EventKind::MidiMtcQf, k)->t == kLead + kPeriod25 * (k + 1u));
  }
}

TEST_CASE("seq mtc: a locate while playing relocates: the row's song time is asked, the generator is silent until the answer, the old stamp is dropped") {
  if (!kRate44100) {
    return;
  }
  two_patterns();
  seq::Sequencer& s = make_seq(mtc_cfg(25, 1));
  s.play_song(0, 0);
  g_col.run(s, 0, 2000, 64);
  CHECK(s.mtc().anchored());
  CHECK(s.mtc_request() == 0u);
  s.locate_row(4);  // this pattern, row 5: song time 4 steps
  g_col.run(s, 2000, 2000 + 2 * 5512 + 64, 64);  // a step is 5,512.5 samples: the line lands at the next boundary
  const std::uint32_t req = s.mtc_request();
  CHECK((req >> 24) != 0u);
  CHECK((req & 0xFFu) == 4u);
  CHECK(((req >> 16) & 0xFFu) == 0u);
  CHECK_FALSE(s.mtc().anchored());
  const std::uint32_t qf_before = g_col.count(EventKind::MidiMtcQf);
  g_col.run(s, 2000 + 2 * 5512 + 64, 2000 + 2 * 5512 + 64 + 1024, 64);
  CHECK(g_col.count(EventKind::MidiMtcQf) == qf_before);  // silent while it waits
  const seq::Command ans = answer_of(req);
  CHECK(ans.c == 22050u);  // 4 steps of 5,512.5 samples
  CHECK(s.commands().push(ans));
  const std::uint64_t from = 2000 + 2 * 5512 + 64 + 1024;
  g_col.run(s, from, from + 2048, 64);
  CHECK(s.mtc().anchored());
  const std::uint32_t fulls = g_col.count(EventKind::MidiMtcFull);
  const Rec* last_full = g_col.nth(EventKind::MidiMtcFull, fulls - 1u);
  NEED(last_full != nullptr);
  CHECK(last_full->t >= from);
}

TEST_CASE("seq mtc: a change of the settings in a run stops the time code; the next run follows the new ones") {
  if (!kRate44100) {
    return;
  }
  two_patterns();
  seq::Sequencer& s = make_seq(mtc_cfg(25, 1));
  s.play_song(0, 0);
  g_col.run(s, 0, 4096, 64);
  CHECK(s.mtc().anchored());
  s.configure(mtc_cfg(30, 1));  // MTC OUT changed under a run
  CHECK_FALSE(s.mtc().anchored());
  const std::uint32_t before = count_mtc();
  g_col.run(s, 4096, 8192, 64);
  CHECK(count_mtc() == before);
  s.play_song(0, 0);
  g_col.run(s, 8192, 8192 + 4096, 64);
  const Rec* full = g_col.nth(EventKind::MidiMtcFull, 1);
  NEED(full != nullptr);
  CHECK(mtc::unpack_code(static_cast<std::uint32_t>(full->ev.value)) == 3u);  // 30 fps
  // MTC OUT Off: nothing more
  s.configure(mtc_cfg(0, 1));
  CHECK_FALSE(s.mtc().anchored());
}

// A song whose only pattern holds a Tempo FX at its step 9 (180 BPM from there): pass 1 lasts 8 x 5,512.5 + 8 x 3,675 = 73,500 samples. The Tempo FX is sticky: before the fix the song's loop kept
// it, so pass 2 began at 180 BPM while the time code went back to 01:00:00:00 (the map and the fast path call that the project tempo): the notes ran early against the time code, up to 296 ms
// at the end of the first half (OS-MTC loop bug, the unit finding of the S6.1 bench).
void loop_tempo_song() {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 8, fx_step(seq::fx::kTempo, 90)) == model::Status::Ok);
}

TEST_CASE("seq mtc: with MTC OUT on, every pass of a looping song plays its steps at the same time code: the Tempo FX of pass 1 does not leak into pass 2") {
  if (!kRate44100) {
    return;
  }
  constexpr std::uint64_t kPass = 73500;
  loop_tempo_song();
  seq::Sequencer& s = make_seq(mtc_cfg(25, 1));
  s.play_song(0, 0);
  g_col.run(s, 0, 2 * kPass + 4096, 64);
  NEED(g_col.count(EventKind::Step) >= 32u);
  NEED(g_col.count(EventKind::MidiMtcFull) >= 2u);
  const Rec* loop = g_col.nth(EventKind::MidiMtcFull, 1);  // the loop's relocation: the time code is back at the song start here
  CHECK(time_of(loop) == mtc::Time{1, 0, 0, 0});
  CHECK(loop->t == kLead + kPass);
  for (std::uint32_t k = 0; k < 16u; ++k) {
    const Rec* a = g_col.nth(EventKind::Step, k);
    const Rec* b = g_col.nth(EventKind::Step, 16u + k);
    NEED(a != nullptr);
    NEED(b != nullptr);
    CHECK_MESSAGE(b->t - loop->t == a->t - kLead, "step " << k << " of pass 2 is " << static_cast<std::int64_t>((b->t - loop->t)) - static_cast<std::int64_t>(a->t - kLead) << " samples off pass 1");
  }
}

TEST_CASE("seq mtc: with MTC OUT off the song's loop is unchanged: the Tempo FX stays in force (F5)") {
  if (!kRate44100) {
    return;
  }
  constexpr std::uint64_t kPass = 73500;
  loop_tempo_song();
  seq::Sequencer::Config c;
  c.start_lead = kLead;
  seq::Sequencer& s = make_seq(c);
  s.play_song(0, 0);
  g_col.run(s, 0, 2 * kPass + 4096, 64);
  const Rec* p2s0 = g_col.nth(EventKind::Step, 16);
  const Rec* p2s1 = g_col.nth(EventKind::Step, 17);
  NEED(p2s0 != nullptr);
  NEED(p2s1 != nullptr);
  CHECK(p2s1->t - p2s0->t == 3675u);  // pass 2 starts at 180 BPM, as before
}
