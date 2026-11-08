// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MTC slice A1: the song time map (seq/song_time.h, docs/specs/OS-MTC.md section 3.2). The map walks the order list as the sequencer plays
// it, so the tests are of two kinds: the arithmetic (constant tempo, the Tempo FX and its below-40 clamp, Tempo fine, Swing, polymetric rows,
// Break, Order jump forward and backward with the kWalkCap cut, Pattern mode, a missing pattern) checked against the sequencer's own formulas,
// and the ORACLE: offline Sequencer runs whose Step event samples must equal the rounded map times, exactly to the sample, plus the round trip
// song_position_at(song_time_at(x)) == x. Host and Cortex-M4F (`qemu_seq_model`).
#include <doctest/doctest.h>

#include <o3os/seq/song_time.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>
#include <set>

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

seq::Sequencer& make_seq(const seq::Sequencer::Config& cfg = seq::Sequencer::Config{}) {
  seq::Sequencer& s = g_slot.make(seq_project());
  s.configure(cfg);
  g_col.n = 0;
  g_col.total_events = 0;
  g_col.hash = 0xCBF29CE484222325ull;
  return s;
}

// The exact sample of `ticks` ticks at a tempo: round(ticks * kCpuHz * den * 125 / (4 * num * bpm_x100)), half up (core/qtime.h).
std::uint64_t exact_sample(std::uint64_t ticks, std::uint32_t bpm_x100) {
  const std::uint64_t n = ticks * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t d = 4u * core::kRefCyclesPerSample.num * static_cast<std::uint64_t>(bpm_x100);
  return (2u * n + d) / (2u * d);
}

// The step length the sequencer's finish_step computes (the shared step_length_q32), for building expected times in the tests.
core::Q32 dur_at(std::uint32_t bpm_x100, std::uint32_t swing, bool odd) {
  return seq::step_length_q32(core::samples_per_tick_q32(bpm_x100), swing, odd);
}

model::Project& proj(std::uint32_t bpm_x100 = 12000, std::uint32_t tracks = 4) {
  fresh_project(tracks, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(bpm_x100) == model::Status::Ok);
  return p;
}

// The time of the sample `t` of a run that started at `origin`, as a Q32 the inverse map is asked for: half a sample before the event's
// sample, so the step whose rounded sample is `t` is the first one at or after it.
core::Q32 ask_of(std::uint64_t origin, std::uint64_t t) {
  if (t <= origin) {
    return 0;
  }
  return ((t - origin) << core::kQ32Frac) - core::kQ32Half;
}

}  // namespace

TEST_CASE("song_time: every Row FX of kFxInfo is classified for the map (the guard)") {
  for (unsigned t = 0; t < seq::kFxInfoCount; ++t) {
    const std::uint8_t type = static_cast<std::uint8_t>(t);
    if (seq::fx_kind(type) == seq::FxKind::Row) {
      CHECK_MESSAGE(seq::map_fx_known(type), t);
    }
  }
  // the timing FX of F6 that move the timeline are timing; the rest are known and not
  CHECK(seq::map_fx_is_timing(fx::kBreakPattern));
  CHECK(seq::map_fx_is_timing(fx::kTempo));
  CHECK(seq::map_fx_is_timing(fx::kSwing));
  CHECK(seq::map_fx_is_timing(fx::kTempoFine));
  CHECK(seq::map_fx_is_timing(fx::kOrderJump));
  CHECK_FALSE(seq::map_fx_is_timing(fx::kSpeed));
  CHECK(seq::map_fx_known(fx::kSpeed));
}

TEST_CASE("song_time: constant tempo, exact Q32 sample counts, 16 and 128 steps, the song's loop, a missing pattern") {
  model::Project& p = proj(12000);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  const core::Q32 spt = core::samples_per_tick_q32(12000);
  const core::Q32 step = 48u * spt;  // swing 50: both factors are 100
  for (std::uint32_t n = 0; n < 16; ++n) {
    const seq::SongPoint pt = seq::song_time_at(p, seq::PlayMode::Song, 0, n);
    CHECK_FALSE(pt.capped);
    CHECK(pt.time == core::Q32{n} * step);
    CHECK(core::q32_round(pt.time) == exact_sample(48u * n, 12000));
    CHECK(pt.tempo_x100 == 12000u);
    CHECK(pt.swing == 50u);
    CHECK(pt.order == 0u);
    CHECK(pt.pattern == 1u);
    CHECK(pt.step == n);
  }
  // the inverse at the same times, and past one lap (the song loops): step 0 again at the lap's period
  for (std::uint32_t n = 0; n < 16; ++n) {
    const seq::SongPoint pt = seq::song_time_at(p, seq::PlayMode::Song, 0, n);
    const seq::SongPoint q = seq::song_position_at(p, seq::PlayMode::Song, 0, pt.time);
    CHECK_FALSE(q.capped);
    CHECK(q.time == pt.time);
    CHECK(q.order == pt.order);
    CHECK(q.step == pt.step);
    CHECK(q.pattern == pt.pattern);
    CHECK(q.tempo_x100 == pt.tempo_x100);
    CHECK(q.swing == pt.swing);
  }
  const seq::SongPoint lap2 = seq::song_position_at(p, seq::PlayMode::Song, 0, core::Q32{16} * step);
  CHECK(lap2.step == 0u);
  CHECK(lap2.order == 0u);
  CHECK(lap2.time == core::Q32{16} * step);
  CHECK(seq::song_position_at(p, seq::PlayMode::Song, 0, 0).time == 0);

  // a 128-step pattern
  model::Project& p2 = proj(20000);
  CHECK(p2.create_pattern(1, 128) == model::Status::Ok);
  CHECK(p2.append_order(1) == model::Status::Ok);
  const core::Q32 step128 = 48u * core::samples_per_tick_q32(20000);
  const seq::SongPoint last = seq::song_time_at(p2, seq::PlayMode::Song, 0, 127);
  CHECK(last.time == core::Q32{127} * step128);
  CHECK(last.step == 127u);
  const seq::SongPoint wrap = seq::song_position_at(p2, seq::PlayMode::Song, 0, core::Q32{128} * step128);
  CHECK(wrap.step == 0u);
  CHECK(wrap.time == core::Q32{128} * step128);

  // a slot naming a pattern that does not exist plays as empty, at the default length
  model::Project& p3 = proj(12000);
  CHECK(p3.append_order(9) == model::Status::Ok);
  const std::uint32_t deflen = p3.default_pattern_length();
  const seq::SongPoint miss = seq::song_time_at(p3, seq::PlayMode::Song, 0, deflen - 1);
  CHECK_FALSE(miss.capped);
  CHECK(miss.pattern == 9u);
  CHECK(miss.step == deflen - 1u);
  CHECK(miss.time == core::Q32{deflen - 1u} * step);
  const seq::SongPoint miss_wrap = seq::song_position_at(p3, seq::PlayMode::Song, 0, core::Q32{deflen} * step);
  CHECK(miss_wrap.step == 0u);
  CHECK(miss_wrap.pattern == 9u);
  CHECK(miss_wrap.time == core::Q32{deflen} * step);
}

TEST_CASE("song_time: a Tempo FX mid-pattern (5..200, the below-40 clamp) and a Tempo fine that stays") {
  model::Project& p = proj(12000);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  // step 4: Tempo 100 -> 200 BPM; step 8: Tempo fine 138 (+2.50 BPM, on top of the FX tempo, and it stays); step 12: Tempo 5 -> clamped to 40 BPM
  CHECK(p.set_step(1, 0, 4, fx_step(fx::kTempo, 100)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 8, fx_step(fx::kTempoFine, 138)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 12, fx_step(fx::kTempo, 5)) == model::Status::Ok);
  const std::uint32_t bpm[16] = {12000, 12000, 12000, 12000, 20000, 20000, 20000, 20000, 20250, 20250, 20250, 20250, 4000, 4000, 4000, 4000};
  core::Q32 t = 0;
  for (std::uint32_t n = 0; n < 16; ++n) {
    const seq::SongPoint pt = seq::song_time_at(p, seq::PlayMode::Song, 0, n);
    CHECK_FALSE(pt.capped);
    CHECK(pt.time == t);
    CHECK(pt.tempo_x100 == bpm[n]);
    t += dur_at(bpm[n], 50, (n & 1u) != 0u);
  }
  // the song's loop does not reset the FX tempo (only a run's start does, F5), so the lap starts at the last Tempo FX's 40 BPM
  const seq::SongPoint lap2 = seq::song_position_at(p, seq::PlayMode::Song, 0, t);
  CHECK(lap2.step == 0u);
  CHECK(lap2.tempo_x100 == 4000u);
  CHECK(lap2.time == t);
  // the extremes of the FX range: 200 -> 400 BPM, 5 -> 40 BPM
  model::Project& p2 = proj(12000);
  CHECK(p2.create_pattern(1, 4) == model::Status::Ok);
  CHECK(p2.set_step(1, 0, 1, fx_step(fx::kTempo, 200)) == model::Status::Ok);
  CHECK(p2.append_order(1) == model::Status::Ok);
  CHECK(seq::song_time_at(p2, seq::PlayMode::Song, 0, 1).tempo_x100 == 40000u);
  CHECK(seq::song_time_at(p2, seq::PlayMode::Song, 0, 2).tempo_x100 == 40000u);
  // a Tempo fine alone (no Tempo FX) adjusts the project tempo and stays
  model::Project& p3 = proj(12000);
  CHECK(p3.create_pattern(1, 4) == model::Status::Ok);
  CHECK(p3.set_step(1, 0, 2, fx_step(fx::kTempoFine, 148)) == model::Status::Ok);  // +5.00 BPM
  CHECK(p3.append_order(1) == model::Status::Ok);
  CHECK(seq::song_time_at(p3, seq::PlayMode::Song, 0, 1).tempo_x100 == 12000u);
  CHECK(seq::song_time_at(p3, seq::PlayMode::Song, 0, 2).tempo_x100 == 12500u);
  CHECK(seq::song_time_at(p3, seq::PlayMode::Song, 0, 3).tempo_x100 == 12500u);
  // the columns of one row: pre_fx runs column 1 then column 0, so the first column's FX is the last one written and wins
  model::Project& p4 = proj(12000);
  CHECK(p4.create_pattern(1, 4) == model::Status::Ok);
  CHECK(p4.set_step(1, 0, 1, fx_step(fx::kTempo, 100, fx::kTempo, 60)) == model::Status::Ok);  // col 0: 200 BPM, col 1: 120 BPM
  CHECK(p4.append_order(1) == model::Status::Ok);
  CHECK(seq::song_time_at(p4, seq::PlayMode::Song, 0, 1).tempo_x100 == 20000u);
}

TEST_CASE("song_time: swing — the project swing, a Swing FX from its row on, the pair parity from the run start, both columns") {
  model::Project& p = proj(12000);
  CHECK(p.set_swing(60) == model::Status::Ok);
  CHECK(p.create_pattern(1, 8) == model::Status::Ok);
  // a Swing FX 70 at step 2 (both columns of one row: column 1 then column 0, the first wins)
  CHECK(p.set_step(1, 0, 2, fx_step(fx::kSwing, 70, fx::kSwing, 30)) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  const std::uint32_t sw[8] = {60, 60, 70, 70, 70, 70, 70, 70};
  core::Q32 t = 0;
  for (std::uint32_t n = 0; n < 8; ++n) {
    const seq::SongPoint pt = seq::song_time_at(p, seq::PlayMode::Song, 0, n);
    CHECK(pt.time == t);
    CHECK(pt.swing == sw[n]);
    CHECK(pt.tempo_x100 == 12000u);
    t += dur_at(12000, sw[n], (n & 1u) != 0u);
  }
  // the pair always adds to two nominal steps, whatever the swing (exact Q32; the two truncating divisions can only lose one Q32 unit)
  const core::Q32 nominal = 48u * core::samples_per_tick_q32(12000);
  for (std::uint32_t s : {25u, 50u, 75u}) {
    const core::Q32 pair = dur_at(12000, s, false) + dur_at(12000, s, true);
    const bool pair_ok = pair == 2u * nominal || pair + 1u == 2u * nominal;
    CHECK_MESSAGE(pair_ok, s);
  }
  // the parity runs on across the pattern's end (the run start is the only reset)
  model::Project& p2 = proj(12000);
  CHECK(p2.set_swing(70) == model::Status::Ok);
  CHECK(p2.create_pattern(1, 4) == model::Status::Ok);
  CHECK(p2.create_pattern(2, 4) == model::Status::Ok);
  CHECK(p2.append_order(1) == model::Status::Ok);
  CHECK(p2.append_order(2) == model::Status::Ok);
  // steps 0..3 of pattern 1 (even, odd, even, odd at swing 70), then step 0 of pattern 2 is step 4 of the run: even again
  const seq::SongPoint s3 = seq::song_time_at(p2, seq::PlayMode::Song, 0, 3);
  const seq::SongPoint s4 = seq::song_time_at(p2, seq::PlayMode::Song, 1, 0);
  CHECK(s4.time == s3.time + dur_at(12000, 70, true));   // step 3 was odd
  CHECK(s4.time == 2u * (dur_at(12000, 70, false) + dur_at(12000, 70, true)));
  CHECK(s4.order == 1u);
}

TEST_CASE("song_time: polymetric tracks — a short track's row k mod len_t carries the timing FX") {
  model::Project& p = proj(12000, 4);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.set_track_length(1, 1, 4) == model::Status::Ok);   // track 1 loops every 4 steps
  CHECK(p.set_track_length(1, 2, 8) == model::Status::Ok);   // track 2 every 8
  // track 1's rows 1 and 3: Tempo 100 (200 BPM) and Tempo 50 (100 BPM), so they land on song steps 1, 5, 9, 13 and 3, 7, 11, 15
  CHECK(p.set_step(1, 1, 1, fx_step(fx::kTempo, 100)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 3, fx_step(fx::kTempo, 50)) == model::Status::Ok);
  // track 2's row 2: Swing 65, on song steps 2 and 10 (from the first one on, the Swing FX stays)
  CHECK(p.set_step(1, 2, 2, fx_step(fx::kSwing, 65)) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  const std::uint32_t bpm[16] = {12000, 20000, 20000, 10000, 10000, 20000, 20000, 10000, 10000, 20000, 20000, 10000, 10000, 20000, 20000, 10000};
  const std::uint32_t sw[16] = {50, 50, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65};
  core::Q32 t = 0;
  for (std::uint32_t n = 0; n < 16; ++n) {
    const seq::SongPoint pt = seq::song_time_at(p, seq::PlayMode::Song, 0, n);
    CHECK_FALSE(pt.capped);
    CHECK(pt.time == t);
    CHECK(pt.tempo_x100 == bpm[n]);
    CHECK(pt.swing == sw[n]);
    CHECK(pt.step == n);
    t += dur_at(bpm[n], sw[n], (n & 1u) != 0u);
  }
  // the pattern's length is the longest active track (16), not the short ones
  const seq::SongPoint wrap = seq::song_position_at(p, seq::PlayMode::Song, 0, t);
  CHECK(wrap.step == 0u);
  CHECK(wrap.pattern == 1u);
}

TEST_CASE("song_time: a Break pattern ends the pattern at its step; the steps past it never play") {
  model::Project& p = proj(12000);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 8) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 5, fx_step(fx::kBreakPattern, 1)) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  const core::Q32 step = 48u * core::samples_per_tick_q32(12000);
  // steps 0..5 of pattern 1 play, then the song goes to order 1
  const seq::SongPoint next = seq::song_time_at(p, seq::PlayMode::Song, 1, 0);
  CHECK_FALSE(next.capped);
  CHECK(next.time == 6u * step);
  CHECK(next.pattern == 2u);
  for (std::uint32_t n = 0; n <= 5; ++n) {
    const seq::SongPoint pt = seq::song_time_at(p, seq::PlayMode::Song, 0, n);
    CHECK(pt.time == core::Q32{n} * step);
  }
  // step 6 of pattern 1 is never reached: the walk is cut at kWalkCap
  const seq::SongPoint gone = seq::song_time_at(p, seq::PlayMode::Song, 0, 6);
  CHECK(gone.capped);
  // in Pattern mode a Break restarts the pattern at step 0
  const seq::SongPoint pm = seq::song_time_at(p, seq::PlayMode::Pattern, 1, 0);
  CHECK_FALSE(pm.capped);
  CHECK(pm.time == 0);
  const seq::SongPoint pm_after = seq::song_position_at(p, seq::PlayMode::Pattern, 1, 6u * step);
  CHECK(pm_after.step == 0u);  // the pattern restarted at the Break: step 0 again
  CHECK(pm_after.time == 6u * step);
}

TEST_CASE("song_time: an Order jump forward and backward; the self-loop is cut at kWalkCap") {
  model::Project& p = proj(12000);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 8) == model::Status::Ok);
  CHECK(p.create_pattern(3, 8) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 15, fx_step(fx::kOrderJump, 2)) == model::Status::Ok);  // at pattern 1's end: to order 2 (pattern 3)
  CHECK(p.set_step(3, 0, 7, fx_step(fx::kOrderJump, 0)) == model::Status::Ok);    // at pattern 3's step 7: back to order 0 -> a self-loop
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  CHECK(p.append_order(3) == model::Status::Ok);
  const core::Q32 step = 48u * core::samples_per_tick_q32(12000);
  // forward: order 2 starts after pattern 1's 16 steps
  const seq::SongPoint fwd = seq::song_time_at(p, seq::PlayMode::Song, 2, 0);
  CHECK_FALSE(fwd.capped);
  CHECK(fwd.time == 16u * step);
  CHECK(fwd.pattern == 3u);
  // backward: pattern 3 ends at its step 7 (the jump) and the song is at order 0 again: step 0 of order 0 in the second lap
  const seq::SongPoint back = seq::song_position_at(p, seq::PlayMode::Song, 0, 16u * step + 8u * step);
  CHECK(back.order == 0u);
  CHECK(back.step == 0u);
  CHECK(back.pattern == 1u);
  CHECK(back.time == 24u * step);
  // the self-loop: a time past everything kWalkCap steps reach is answered by the last step walked, capped
  const seq::SongPoint cap = seq::song_position_at(p, seq::PlayMode::Song, 0, core::Q32{1} << 62);
  CHECK(cap.capped);
  CHECK(cap.time == core::Q32{seq::kWalkCap - 1u} * step);  // constant tempo: every step is `step` long
  // the walk's step cycle is 16 + 8 steps, so the last walked step is 65535 mod 24 = 15, of order 0
  CHECK(cap.step == 15u);
  CHECK(cap.order == 0u);
  // an Order jump out of range (or in Pattern mode) ends the pattern like a Break: in Song mode the song advances
  model::Project& p2 = proj(12000);
  CHECK(p2.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p2.create_pattern(2, 16) == model::Status::Ok);
  CHECK(p2.create_pattern(3, 16) == model::Status::Ok);
  CHECK(p2.set_step(2, 0, 3, fx_step(fx::kOrderJump, 200)) == model::Status::Ok);  // 200 >= song length 3: ignored
  CHECK(p2.append_order(1) == model::Status::Ok);
  CHECK(p2.append_order(2) == model::Status::Ok);
  CHECK(p2.append_order(3) == model::Status::Ok);
  const seq::SongPoint after = seq::song_time_at(p2, seq::PlayMode::Song, 2, 0);
  CHECK(after.time == 16u * step + 4u * step);  // pattern 2 ran to its step 3, then the song advanced to order 2
}

TEST_CASE("song_time: Pattern mode — the time counts from the pattern's step 0 and runs on through the loops") {
  model::Project& p = proj(12000);
  CHECK(p.create_pattern(1, 8) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 4, fx_step(fx::kTempo, 100)) == model::Status::Ok);  // 200 BPM from step 4 on
  const core::Q32 d120 = dur_at(12000, 50, false);  // swing 50: both parities are the nominal step
  const core::Q32 d200e = dur_at(20000, 50, false);
  const core::Q32 d200o = dur_at(20000, 50, true);
  const core::Q32 durs[8] = {d120, d120, d120, d120, d200e, d200o, d200e, d200o};  // parities of the run: 0,1,0,1,...
  const core::Q32 lap = 4u * d120 + d200e + d200o + d200e + d200o;
  core::Q32 t = 0;
  for (std::uint32_t n = 0; n < 8; ++n) {
    const seq::SongPoint pt = seq::song_time_at(p, seq::PlayMode::Pattern, 1, n);
    CHECK_FALSE(pt.capped);
    CHECK(pt.order == audio::PlayPosition::kNoOrder);
    CHECK(pt.pattern == 1u);
    CHECK(pt.step == n);
    CHECK(pt.time == t);
    CHECK(pt.tempo_x100 == (n < 4u ? 12000u : 20000u));
    t += durs[n];
  }
  // the second loop: the pattern's own Tempo FX left 200 BPM, so the period is not the first loop's
  const seq::SongPoint lap2 = seq::song_position_at(p, seq::PlayMode::Pattern, 1, lap);
  CHECK(lap2.step == 0u);
  CHECK(lap2.time == lap);
  CHECK(lap2.tempo_x100 == 20000u);
  const seq::SongPoint mid = seq::song_position_at(p, seq::PlayMode::Pattern, 1, lap + d200e);
  CHECK(mid.step == 1u);
  CHECK(mid.time == lap + d200e);
}

TEST_CASE("song_time: a target the song never plays answers at once, capped (not a match): order past the list, a Pattern-mode id without a pattern, a step past the pattern's length") {
  model::Project& p = proj(12000);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 8) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);  // song_length 2
  const core::Q32 step = 48u * core::samples_per_tick_q32(12000);
  // Song mode, an order past the list's end (the list loops, so the walk could never match it): capped, the request echoed, time 0
  const seq::SongPoint bad_order = seq::song_time_at(p, seq::PlayMode::Song, 5, 0);
  CHECK(bad_order.capped);
  CHECK(bad_order.time == 0);
  CHECK(bad_order.order == 5u);
  CHECK(bad_order.pattern == 0u);  // order(5) is past the list: id 0
  CHECK(bad_order.step == 0u);
  // Song mode, a step past the target pattern's length (16 for order 0, 8 for order 1)
  const seq::SongPoint bad_step = seq::song_time_at(p, seq::PlayMode::Song, 0, 16);
  CHECK(bad_step.capped);
  CHECK(bad_step.order == 0u);
  CHECK(bad_step.pattern == 1u);
  CHECK(bad_step.step == 16u);
  CHECK(seq::song_time_at(p, seq::PlayMode::Song, 1, 8).capped);
  // the steps just inside still answer normally
  const seq::SongPoint ok_last = seq::song_time_at(p, seq::PlayMode::Song, 0, 15);
  CHECK_FALSE(ok_last.capped);
  CHECK(ok_last.time == 15u * step);
  // Pattern mode, a pattern id that does not exist (9 is not in the pool): capped; an existing one answers
  const seq::SongPoint bad_pat = seq::song_time_at(p, seq::PlayMode::Pattern, 9, 0);
  CHECK(bad_pat.capped);
  CHECK(bad_pat.order == audio::PlayPosition::kNoOrder);
  CHECK(bad_pat.pattern == 9u);
  CHECK_FALSE(seq::song_time_at(p, seq::PlayMode::Pattern, 2, 0).capped);
  // Pattern mode, a step past the pattern's length
  CHECK(seq::song_time_at(p, seq::PlayMode::Pattern, 2, 8).capped);
  // Song mode with an empty order list still plays its position 0 as an empty pattern at the default length: reachable
  CHECK(p.clear_order() == model::Status::Ok);
  const std::uint32_t deflen = p.default_pattern_length();
  const seq::SongPoint empty_song = seq::song_time_at(p, seq::PlayMode::Song, 0, 3);
  CHECK_FALSE(empty_song.capped);
  CHECK(empty_song.time == 3u * step);
  CHECK(empty_song.pattern == 0u);
  CHECK(seq::song_time_at(p, seq::PlayMode::Song, 0, deflen).capped);  // past the default length
  CHECK(seq::song_time_at(p, seq::PlayMode::Song, 1, 0).capped);        // past the empty list's one position
}

TEST_CASE("song_time: the round trip song_position_at(song_time_at(x)) == x for every step of an FX-heavy song") {
  model::Project& p = proj(15000, 4);
  CHECK(p.set_swing(58) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 12) == model::Status::Ok);
  CHECK(p.create_pattern(3, 6) == model::Status::Ok);
  CHECK(p.set_track_length(1, 1, 4) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 4, fx_step(fx::kTempo, 90)) == model::Status::Ok);              // 180 BPM from step 4
  CHECK(p.set_step(1, 1, 2, fx_step(fx::kSwing, 66)) == model::Status::Ok);                // swing on steps 2, 6, 10, 14
  CHECK(p.set_step(1, 0, 9, fx_step(fx::kTempoFine, 140)) == model::Status::Ok);           // +3 BPM on step 9, stays
  CHECK(p.set_step(2, 0, 3, fx_step(fx::kBreakPattern, 1)) == model::Status::Ok);          // pattern 2 ends at step 3
  CHECK(p.set_step(3, 0, 5, fx_step(fx::kOrderJump, 1)) == model::Status::Ok);             // pattern 3 ends at step 5, to order 1
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  CHECK(p.append_order(3) == model::Status::Ok);
  std::uint32_t checked = 0;
  for (std::uint32_t order = 0; order < p.song_length(); ++order) {
    const std::uint32_t len = [&] {
      std::uint32_t l = p.pattern_length(p.order(order));
      if (l == 0u) {
        l = p.default_pattern_length();
      }
      return l;
    }();
    for (std::uint32_t step = 0; step < len; ++step) {
      const seq::SongPoint pt = seq::song_time_at(p, seq::PlayMode::Song, order, step);
      if (pt.capped) {
        continue;  // a step the song never plays (past a Break)
      }
      const seq::SongPoint q = seq::song_position_at(p, seq::PlayMode::Song, 0, pt.time);
      CHECK_FALSE(q.capped);
      CHECK(q.time == pt.time);
      CHECK(q.order == pt.order);
      CHECK(q.step == pt.step);
      CHECK(q.pattern == pt.pattern);
      CHECK(q.tempo_x100 == pt.tempo_x100);
      CHECK(q.swing == pt.swing);
      ++checked;
    }
  }
  CHECK(checked == 26u);  // 16 steps of pattern 1 + 4 of pattern 2 (the Break at its step 3) + 6 of pattern 3 (the jump at its step 5)
}

// The ORACLE: an offline Sequencer run (driven as the seq tests drive it) records the sample at which each step starts; every one must equal
// origin + round(map time), exactly to the sample, and the inverse must answer that same step.
namespace {

void oracle_song(model::Project& p, seq::PlayMode mode, std::uint32_t order_arg, std::uint64_t horizon, std::uint32_t min_steps) {
  seq::Sequencer& s = make_seq();
  if (mode == seq::PlayMode::Song) {
    s.play_song(0);
  } else {
    s.play_pattern(order_arg);
  }
  g_col.run(s, 0, horizon, 64);
  NEED(g_col.count(EventKind::Step) >= min_steps);
  const Rec* first = g_col.nth(EventKind::Step, 0);
  NEED(first != nullptr);
  const std::uint64_t origin = first->t;
  std::set<std::pair<unsigned, unsigned>> seen;  // the (order, step) pairs already checked through song_time_at (their first lap)
  const std::uint32_t steps = g_col.count(EventKind::Step);
  for (std::uint32_t i = 0; i < steps; ++i) {
    const Rec* r = g_col.nth(EventKind::Step, i);
    NEED(r != nullptr);
    const unsigned ev_order = audio::event_order_pos(r->ev);
    const unsigned ev_step = static_cast<unsigned>(r->ev.value);
    const seq::SongPoint q = seq::song_position_at(p, mode, order_arg, ask_of(origin, r->t));
    CHECK_FALSE(q.capped);
    CHECK_MESSAGE(origin + core::q32_round(q.time) == r->t, i);
    CHECK(q.step == ev_step);
    CHECK(q.pattern == r->ev.a);
    if (mode == seq::PlayMode::Song) {
      CHECK(q.order == ev_order);
    } else {
      CHECK(q.order == audio::PlayPosition::kNoOrder);
    }
    if (seen.insert({ev_order, ev_step}).second) {
      // Song mode: the event's order position; Pattern mode: the pattern id (song_time_at takes it in `order` there)
      const std::uint32_t target = mode == seq::PlayMode::Song ? ev_order : order_arg;
      const seq::SongPoint pt = seq::song_time_at(p, mode, target, ev_step);
      CHECK_FALSE(pt.capped);
      CHECK_MESSAGE(origin + core::q32_round(pt.time) == r->t, i);
      CHECK(pt.time == q.time);
      CHECK(pt.tempo_x100 == q.tempo_x100);
      CHECK(pt.swing == q.swing);
    }
  }
  CHECK(s.stats().microstep_overrun == 0u);
}

}  // namespace

TEST_CASE("song_time oracle: the map equals the offline sequencer's step samples") {
  // A: constant tempo, three slots (4, 8, 4 steps), more than one lap
  {
    model::Project& p = proj(24000);
    CHECK(p.create_pattern(1, 4) == model::Status::Ok);
    CHECK(p.create_pattern(2, 8) == model::Status::Ok);
    CHECK(p.set_step(1, 0, 0, make_step(61)) == model::Status::Ok);
    CHECK(p.set_step(2, 0, 0, make_step(62)) == model::Status::Ok);
    CHECK(p.append_order(1) == model::Status::Ok);
    CHECK(p.append_order(2) == model::Status::Ok);
    CHECK(p.append_order(1) == model::Status::Ok);
    oracle_song(p, seq::PlayMode::Song, 0, exact_sample(48u * 36u, 24000), 36u);
  }
  // B: a Tempo FX mid-pattern on a short polymetric track, a Swing FX, a Tempo fine, two slots
  {
    model::Project& p = proj(18000, 4);
    CHECK(p.set_swing(55) == model::Status::Ok);
    CHECK(p.create_pattern(1, 16) == model::Status::Ok);
    CHECK(p.set_track_length(1, 1, 4) == model::Status::Ok);
    CHECK(p.set_step(1, 1, 1, fx_step(fx::kTempo, 80)) == model::Status::Ok);   // 160 BPM on steps 1, 5, 9, 13
    CHECK(p.set_step(1, 0, 6, fx_step(fx::kSwing, 70)) == model::Status::Ok);   // swing from step 6
    CHECK(p.set_step(1, 0, 10, fx_step(fx::kTempoFine, 136)) == model::Status::Ok);
    CHECK(p.create_pattern(2, 8) == model::Status::Ok);
    CHECK(p.set_step(2, 0, 3, fx_step(fx::kTempo, 100)) == model::Status::Ok);   // 200 BPM from pattern 2's step 3
    CHECK(p.append_order(1) == model::Status::Ok);
    CHECK(p.append_order(2) == model::Status::Ok);
    // the horizon covers 40 steps although four of them run at the slower 160 BPM of the track 1 FX
    oracle_song(p, seq::PlayMode::Song, 0, exact_sample(48u * 56u, 18000), 40u);
  }
  // C: a Break and an Order jump back (a self-loop), a few laps of it
  {
    model::Project& p = proj(24000);
    CHECK(p.create_pattern(1, 16) == model::Status::Ok);
    CHECK(p.create_pattern(2, 8) == model::Status::Ok);
    CHECK(p.set_step(1, 0, 5, fx_step(fx::kBreakPattern, 1)) == model::Status::Ok);
    CHECK(p.set_step(2, 0, 3, fx_step(fx::kOrderJump, 0)) == model::Status::Ok);
    CHECK(p.append_order(1) == model::Status::Ok);
    CHECK(p.append_order(2) == model::Status::Ok);
    // the song loops 1(6 steps) -> 2(4 steps) -> jump to 0: 10 steps a lap
    oracle_song(p, seq::PlayMode::Song, 0, exact_sample(48u * 50u, 24000), 50u);
  }
  // D: Pattern mode with a Tempo FX and a Swing FX, more than one loop
  {
    model::Project& p = proj(20000);
    CHECK(p.create_pattern(1, 16) == model::Status::Ok);
    CHECK(p.set_step(1, 0, 4, fx_step(fx::kTempo, 120)) == model::Status::Ok);  // 240 BPM from step 4
    CHECK(p.set_step(1, 0, 8, fx_step(fx::kSwing, 68)) == model::Status::Ok);
    oracle_song(p, seq::PlayMode::Pattern, 1, exact_sample(48u * 40u, 20000), 40u);
  }
}

// OS-MTC B3 (the chase starts a run AT a step): the state before the step's own FX, the swing pair's parity, and the walk that does not loop (a time past the song's end is `past_end`, Q9 a).
TEST_CASE("song_time: tempo_in, swing_in and the pair's parity are the state before the step's FX; loop = false ends the walk at the end of the order list") {
  model::Project& p = proj(12000);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 16) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 3, fx_step(fx::kTempo, 70)) == model::Status::Ok);   // 140 BPM from step 3
  CHECK(p.set_step(1, 0, 5, fx_step(fx::kSwing, 60)) == model::Status::Ok);   // swing 60 from step 5
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  const seq::SongPoint s2 = seq::song_time_at(p, seq::PlayMode::Song, 0, 2);
  CHECK(s2.tempo_x100 == 12000u);
  CHECK(s2.tempo_in_x100 == 12000u);
  const seq::SongPoint s3 = seq::song_time_at(p, seq::PlayMode::Song, 0, 3);
  CHECK(s3.tempo_x100 == 14000u);     // after its row: the step that holds the FX already plays at it
  CHECK(s3.tempo_in_x100 == 12000u);  // before it: what a run that starts here must be given, its own row sets 140 again
  const seq::SongPoint s4 = seq::song_time_at(p, seq::PlayMode::Song, 0, 4);
  CHECK(s4.tempo_in_x100 == 14000u);
  const seq::SongPoint s5 = seq::song_time_at(p, seq::PlayMode::Song, 0, 5);
  CHECK(s5.swing == 60u);
  CHECK(s5.swing_in == 50u);
  CHECK(seq::song_time_at(p, seq::PlayMode::Song, 0, 6).swing_in == 60u);
  // the pair's parity counts steps from the run start
  for (std::uint32_t n = 0; n < 16; ++n) {
    CHECK_MESSAGE(seq::song_time_at(p, seq::PlayMode::Song, 0, n).odd == ((n % 2u) == 1u), n);
  }
  CHECK(seq::song_time_at(p, seq::PlayMode::Song, 1, 0).odd == false);  // 16 steps before it: even
  CHECK(seq::song_time_at(p, seq::PlayMode::Song, 1, 3).odd == true);
  // loop = false: a time inside the song is the same answer, a time past its end is past_end
  const core::Q32 end = seq::song_time_at(p, seq::PlayMode::Song, 1, 15).time;
  const seq::SongPoint in = seq::song_position_at(p, seq::PlayMode::Song, 0, end, false);
  CHECK_FALSE(in.past_end);
  CHECK(in.order == 1u);
  CHECK(in.step == 15u);
  const seq::SongPoint within_last = seq::song_position_at(p, seq::PlayMode::Song, 0, end + 1u, false);  // inside the last step, after its start: no step starts after it
  CHECK(within_last.past_end);
  const seq::SongPoint way_past = seq::song_position_at(p, seq::PlayMode::Song, 0, end * 3u, false);
  CHECK(way_past.past_end);
  CHECK_FALSE(way_past.capped);
  // loop = true (the default) is A1's walk: the song's loop, the same step as the first slot's
  const seq::SongPoint looped = seq::song_position_at(p, seq::PlayMode::Song, 0, end * 3u);
  CHECK_FALSE(looped.past_end);
  CHECK_FALSE(looped.capped);
  // Pattern mode loops the pattern whatever `loop` says (Q10)
  const seq::SongPoint pat = seq::song_position_at(p, seq::PlayMode::Pattern, 2, end * 3u, false);
  CHECK_FALSE(pat.past_end);
  CHECK_FALSE(pat.capped);
}
