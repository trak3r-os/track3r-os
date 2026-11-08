// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PERFMODES M2: the sequencer plays the custom orders C1..C20 and their rolls (seq/sequencer.cpp, docs/specs/OS-PERFMODES.md 3.2) against the data of
// seq/play_orders.h, over the offline Sequencer of tests/support/seq_fixture.h. A pattern whose row r holds note r on the track, so the note names the step
// played. Every order C1..C20 step by step for 256 steps against play_order_entry (two passes of the wrap); the same on tracks of 64, 32, 16 and 7 steps;
// PAT LEN keeps an order inside its length; a switch into a custom mode in mid-pattern joins the order at the track's place (F5); the rolls (Q3 a): where
// they land, the first free column, a step without a note, C3's 30 % over 4,096 steps; an E-PERF-07 queue lands at the order's wrap (F6); the PerfTrack
// command's mask is 0x1F and a value above 23 is Forward; two renders of a song with C3 on four tracks are byte-identical. Host and Cortex-M4F
// (`qemu_seq_model`).
#include <doctest/doctest.h>

#include <o3os/core/qtime.h>
#include <o3os/seq/play_orders.h>
#include <o3os/seq/sequencer.h>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

#include <cstdint>
#include <initializer_list>

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace seq = o3os::seq;
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

// The exact sample of tick n at a tempo: round(n * kCpuHz * den * 125 / (4 * num * bpm_x100)), half up (core/qtime.h).
std::uint64_t exact_sample(std::uint64_t ticks, std::uint32_t bpm_x100) {
  const std::uint64_t n = ticks * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t d = 4u * core::kRefCyclesPerSample.num * static_cast<std::uint64_t>(bpm_x100);
  return (2u * n + d) / (2u * d);
}

constexpr std::uint32_t kBpm = 24000;
std::uint64_t us(std::uint32_t step, std::uint32_t j = 0) { return exact_sample(48u * step + j, kBpm); }

// A project of one pattern (id 1) of `len` steps at 240 BPM, row r holding note r on track 0 (the note names the step), the other tracks empty.
void order_project(std::uint32_t len, std::uint32_t tracks = 1) {
  fresh_project(tracks, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(kBpm) == model::Status::Ok);
  CHECK(p.create_pattern(1, len) == model::Status::Ok);
  for (std::uint32_t r = 0; r < len; ++r) {
    CHECK(p.set_step(1, 0, r, make_step(static_cast<std::uint8_t>(r))) == model::Status::Ok);
  }
}

const Rec* find_kind(EventKind k, std::uint8_t track, std::uint64_t t) {
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == k && g_col.rec[i].ev.track == track && g_col.rec[i].t == t) {
      return &g_col.rec[i];
    }
  }
  return nullptr;
}

// The note a track starts on a step (0 when none).
int note_at(std::uint8_t track, std::uint32_t step) {
  const Rec* r = find_kind(EventKind::NoteOn, track, us(step));
  return r != nullptr ? r->ev.a : 0;
}

seq::TrackPlayMode custom(unsigned order) { return static_cast<seq::TrackPlayMode>(static_cast<unsigned>(seq::TrackPlayMode::Custom1) + order); }

}  // namespace

TEST_CASE("play orders: every order C1..C20 step by step for 256 steps against play_order_entry (two passes of the wrap)") {
  for (unsigned o = 0; o < seq::kPlayOrders; ++o) {
    order_project(128);
    seq::Sequencer& s = make();
    s.set_perf_track(0, 0, 0, custom(o));
    s.play_pattern(1);
    g_col.run(s, 0, us(256), 64);
    for (std::uint32_t i = 0; i < 256u; ++i) {
      const seq::PlayOrderEntry e = seq::play_order_entry(o, i % seq::kPlayOrderLen);
      const unsigned msg = o * 1000u + i;
      CHECK_MESSAGE(note_at(0, i) == static_cast<int>(e.step - 1u), msg);
    }
  }
}

TEST_CASE("play orders: the same on tracks of 64, 32, 16 and 7 steps (the step played is the entry modulo the length)") {
  for (const std::uint32_t len : {64u, 32u, 16u, 7u}) {
    for (unsigned o = 0; o < seq::kPlayOrders; ++o) {
      order_project(len);
      seq::Sequencer& s = make();
      s.set_perf_track(0, 0, 0, custom(o));
      s.play_pattern(1);
      g_col.run(s, 0, us(256), 64);
      for (std::uint32_t i = 0; i < 256u; ++i) {
        const seq::PlayOrderEntry e = seq::play_order_entry(o, i % len);
        const unsigned msg = len * 100000u + o * 1000u + i;
        CHECK_MESSAGE(note_at(0, i) == static_cast<int>((e.step - 1u) % len), msg);
      }
    }
  }
}

TEST_CASE("play orders: PAT LEN 8 keeps an order inside 8 steps") {
  order_project(128);
  seq::Sequencer& s = make();
  s.set_perf_pattern_length(8);
  s.set_perf_track(0, 0, 0, custom(0));  // C1
  s.play_pattern(1);
  g_col.run(s, 0, us(32), 64);
  for (std::uint32_t i = 0; i < 32u; ++i) {
    const seq::PlayOrderEntry e = seq::play_order_entry(0, i % 8u);
    CHECK_MESSAGE(note_at(0, i) == static_cast<int>((e.step - 1u) % 8u), i);
  }
}

TEST_CASE("play orders: a switch FWD -> C5 in mid-pattern joins the order at the track's place (F5); C5 -> BACK goes on as BACK") {
  order_project(128);
  seq::Sequencer& s = make();
  s.set_perf_track(0, 0, 0, seq::TrackPlayMode::Forward);
  s.play_pattern(1);
  g_col.run(s, 0, us(10), 64);  // steps 0..9: notes 0..9, ord 0..10
  s.set_perf_track(0, 0, 0, custom(4));  // C5, switched on step 11 (1-based)
  g_col.run(s, us(10), us(20), 64);
  CHECK(note_at(0, 10) == 10);  // the step the switch lands on is still the forward one
  // the next step plays entry 12 (1-based) of C5: its entries 1-16 are 1 2 3 4 5 6 7 8 8 7 6 5 4 3 2 1, so entry 12 is step 5, the note 4
  CHECK(note_at(0, 11) == 4);
  CHECK(note_at(0, 11) == static_cast<int>(seq::play_order_entry(4, 11).step - 1u));
  for (std::uint32_t i = 12; i < 20u; ++i) {
    CHECK_MESSAGE(note_at(0, i) == static_cast<int>(seq::play_order_entry(4, i % seq::kPlayOrderLen).step - 1u), i);
  }
  // C5 -> BACK: the step after the C5 step goes one back from it (the top when it was 0)
  s.set_perf_track(0, 0, 0, seq::TrackPlayMode::Backward);
  g_col.run(s, us(20), us(23), 64);
  const int pos20 = note_at(0, 20);
  CHECK(note_at(0, 21) == (pos20 == 0 ? 127 : pos20 - 1));
}

TEST_CASE("play orders: C4's rolls land on steps 8, 16, 24, 32 at 1/1 flat (the repeats counted); a step without a note repeats the last note (F9)") {
  order_project(32);
  model::Project& p = seq_project();
  for (const std::uint32_t r : {7u, 15u, 23u, 31u}) {  // the steps of C4's rolled entries 8, 16, 24, 32 (1-based): empty, both FX columns free
    CHECK(p.set_step(1, 0, r, fx_step(0, 0)) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  s.set_perf_track(0, 0, 0, custom(3));  // C4
  s.play_pattern(1);
  g_col.run(s, 0, us(64), 64);
  for (std::uint32_t i = 0; i < 64u; ++i) {
    const bool rolled = (i % 8u) == 7u;
    const Rec* r = find_kind(EventKind::NoteOn, 0, us(i));
    NEED(r != nullptr);
    if (rolled) {
      CHECK_MESSAGE((r->ev.flags & audio::kEvRetrigger) != 0u, i);         // the roll repeats at once
      CHECK_MESSAGE(r->ev.a == static_cast<int>((i - 1u) % 32u), i);        // the last note (F9)
    } else {
      CHECK_MESSAGE((r->ev.flags & audio::kEvRetrigger) == 0u, i);
      CHECK_MESSAGE(r->ev.a == static_cast<int>(i % 32u), i);  // the pattern restarts every 32 steps
    }
  }
}

TEST_CASE("play orders: C15 rolls volume-down on every step (the note halved); a step whose two columns hold FX gets no roll") {
  order_project(32);
  seq::Sequencer& s = make();
  s.set_perf_track(0, 0, 0, custom(14));  // C15: dec 1/1 100 % on every entry
  s.play_pattern(1);
  g_col.run(s, 0, us(32), 64);
  for (std::uint32_t i = 0; i < 32u; ++i) {
    CHECK_MESSAGE(find_kind(EventKind::NoteOff, 0, us(i, 24)) != nullptr, i);  // the 1/1 roll halves the note
    CHECK_MESSAGE(note_at(0, i) == static_cast<int>(i), i);
  }
  // C4, with the rolled step 8 (row 7) holding an FX in both columns: no roll there, but on the other rolled steps
  order_project(32);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 7, make_step(7, 1, fx::kPanning, 70, fx::kLowPass, 40)) == model::Status::Ok);
  seq::Sequencer& s2 = make();
  s2.set_perf_track(0, 0, 0, custom(3));  // C4
  s2.play_pattern(1);
  g_col.run(s2, 0, us(32), 64);
  CHECK(find_kind(EventKind::NoteOff, 0, us(7, 24)) == nullptr);  // both columns hold an FX: nothing inserted
  CHECK(note_at(0, 7) == 7);
  for (const std::uint32_t i : {15u, 23u, 31u}) {
    CHECK_MESSAGE(find_kind(EventKind::NoteOff, 0, us(i, 24)) != nullptr, i);  // the other rolled steps roll (100 %)
  }
}

TEST_CASE("play orders: C3's 30 % rolls over 4,096 steps land within 30 +- 3 % (the seeded RNG)") {
  order_project(128);
  seq::Sequencer& s = make();
  s.set_perf_track(0, 0, 0, custom(2));  // C3: flat 1/1 30 % on entries 1-29 and 128 of every 128
  s.play_pattern(1);
  // Count track 0's NoteOff events over 4,096 steps with a local runner (the collector's records cap at 6000): every drawn roll halves its step's note.
  std::uint32_t offs = 0;
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  const std::uint64_t end = us(4096);
  for (std::uint64_t t = 0; t < end;) {
    const std::uint32_t n_blk = static_cast<std::uint32_t>(end - t < 1024u ? end - t : 1024u);
    const std::uint32_t c = s.step(t, n_blk, out, audio::kMaxEventsPerBlock);
    for (std::uint32_t i = 0; i < c; ++i) {
      if (out[i].kind == EventKind::NoteOff && out[i].track == 0) {
        ++offs;
      }
    }
    t += n_blk;
  }
  // C3 rolls on 30 of every 128 entries: 4,096 steps hold 960 draws (the seeded RNG lands at 291, 30.3 %).
  const double frac = static_cast<double>(offs) / 960.0;
  CHECK_MESSAGE(frac >= 0.27, frac);
  CHECK_MESSAGE(frac <= 0.33, frac);
}

TEST_CASE("play orders: an E-PERF-07 queue on a C5 track lands at the order's wrap (F6), not at the pattern's end") {
  fresh_project(1, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(kBpm) == model::Status::Ok);
  CHECK(p.create_pattern(1, 4) == model::Status::Ok);
  CHECK(p.create_pattern(2, 8) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 4; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(static_cast<std::uint8_t>(61 + k))) == model::Status::Ok);
  }
  for (std::uint32_t k = 0; k < 8; ++k) {
    CHECK(p.set_step(2, 0, k, make_step(static_cast<std::uint8_t>(20 + k))) == model::Status::Ok);
  }
  seq::Sequencer& s = make();
  s.set_perf_track(0, 0, 4, custom(4));  // C5 on a 4-step track (the track's PAT LEN): the order's wrap (span 4) is at step 4
  s.play_pattern(2);
  g_col.run(s, 0, us(1), 64);
  CHECK(s.commands().push(seq::Command{seq::Command::Kind::QueueTrackPattern, 0, 1, 0x1u}));  // pattern 1 for track 0
  g_col.run(s, us(1), us(14), 64);
  // steps 0..3: C5's entries 1..4 play pattern 2's rows 0..3 (20..23); at step 4 the order wraps and the queue lands: pattern 1 (61..64); at step 8 the pattern
  // ends and restarts, the track still on pattern 1
  const int want[14] = {20, 21, 22, 23, 61, 62, 63, 64, 61, 62, 63, 64, 61, 62};
  for (std::uint32_t i = 0; i < 14u; ++i) {
    CHECK_MESSAGE(note_at(0, i) == want[i], i);
  }
}

TEST_CASE("play orders: the PerfTrack command's mask is 0x1F: C1 comes through, a value above 23 is Forward") {
  order_project(16);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, us(4), 64);  // steps 0..3 forward, ord 4
  CHECK(s.commands().push(seq::Command{seq::Command::Kind::PerfTrack, 0, 0, 4u << 8}));  // C1 through the command
  g_col.run(s, us(4), us(8), 64);
  CHECK(note_at(0, 4) == 4);                                                     // the step the command lands on is still forward
  CHECK(note_at(0, 5) == static_cast<int>(seq::play_order_entry(0, 5).step - 1u));  // C1's entry 6 (1-based): step 2, the note 1
  CHECK(note_at(0, 5) == 1);
  // above 23: Forward
  order_project(16);
  seq::Sequencer& s2 = make();
  s2.play_pattern(1);
  g_col.run(s2, 0, us(2), 64);
  CHECK(s2.commands().push(seq::Command{seq::Command::Kind::PerfTrack, 0, 0, 24u << 8}));
  CHECK(s2.commands().push(seq::Command{seq::Command::Kind::PerfTrack, 0, 0, 31u << 8}));  // the mask's top
  g_col.run(s2, us(2), us(6), 64);
  for (std::uint32_t i = 2; i < 6u; ++i) {
    CHECK_MESSAGE(note_at(0, i) == static_cast<int>(i), i);  // forward
  }
}

TEST_CASE("play orders: two offline renders of a song with C3 on four tracks are byte-identical") {
  std::uint64_t hashes[2] = {0, 0};
  for (int pass = 0; pass < 2; ++pass) {
    fresh_project(4, 4);
    model::Project& p = seq_project();
    CHECK(p.set_tempo_x100(kBpm) == model::Status::Ok);
    CHECK(p.create_pattern(1, 4) == model::Status::Ok);
    CHECK(p.create_pattern(2, 8) == model::Status::Ok);
    for (std::uint32_t t = 0; t < 4; ++t) {
      for (std::uint32_t k = 0; k < 4; ++k) {
        CHECK(p.set_step(1, t, k, make_step(static_cast<std::uint8_t>(40 + k + t))) == model::Status::Ok);
      }
      for (std::uint32_t k = 0; k < 8; ++k) {
        CHECK(p.set_step(2, t, k, make_step(static_cast<std::uint8_t>(60 + k + t))) == model::Status::Ok);
      }
    }
    CHECK(p.append_order(1) == model::Status::Ok);
    CHECK(p.append_order(2) == model::Status::Ok);
    CHECK(p.append_order(1) == model::Status::Ok);
    seq::Sequencer& s = make();
    for (std::uint32_t t = 0; t < 4; ++t) {
      s.set_perf_track(t, 0, 0, custom(2));  // C3 on the four tracks
    }
    s.play_song(0);
    g_col.run(s, 0, us(64), 64);
    hashes[pass] = g_col.hash;
  }
  CHECK(hashes[0] == hashes[1]);
}
