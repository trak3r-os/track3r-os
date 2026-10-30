// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SONG-VIEW (docs/specs/OS-SONG-VIEW.md; new Community OS behaviour): the pure logic of the Song screen's STAGE page (the countdown arithmetic, what is NOW and NEXT).
#include <doctest/doctest.h>

#include <o3os/app/model.h>
#include <o3os/app/song_source.h>
#include <o3os/app/song_stage.h>
#include <o3os/audio/play_position.h>
#include <o3os/model/project.h>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::StageMode;
using app::StageNext;

namespace {

// A project of slots 1:A(16 steps) 2:B(32) 3:C(64) over patterns 1..3.
struct StageRig {
  app::Model model;
  StageRig() {
    model::Project& p = test::arena(1).project;
    p.reset();
    NEED(p.create_pattern(1, 16) == model::Status::Ok);
    NEED(p.create_pattern(2, 32) == model::Status::Ok);
    NEED(p.create_pattern(3, 64) == model::Status::Ok);
    NEED(p.set_pattern_name(1, "INTRO", 5) == model::Status::Ok);
    for (unsigned id = 1; id <= 3; ++id) {
      NEED(p.append_order(id) == model::Status::Ok);
    }
    model.project = &p;
  }
  static audio::PlayPosition::Value at(unsigned order, unsigned step, unsigned ustep = 0) {
    audio::PlayPosition::Value v;
    v.playing = true;
    v.order = static_cast<std::uint16_t>(order);
    v.step = static_cast<std::uint16_t>(step);
    v.ustep = static_cast<std::uint8_t>(ustep);
    return v;
  }
};

}  // namespace

TEST_CASE("stage: bars left count the bar the playhead is in, so the last bar says 1") {
  CHECK(app::stage_bars_left(64, 0) == 4u);
  CHECK(app::stage_bars_left(64, 15) == 4u);  // still in bar 1
  CHECK(app::stage_bars_left(64, 16) == 3u);  // bar 2 starts
  CHECK(app::stage_bars_left(64, 63) == 1u);
  CHECK(app::stage_bars_left(16, 0) == 1u);
  CHECK(app::stage_bars_left(24, 0) == 2u);   // a pattern of 1.5 bars: the half bar is a bar
  CHECK(app::stage_bars_left(24, 16) == 1u);
  CHECK(app::stage_bars_left(0, 0) == 0u);    // no steps: nothing to count
  CHECK(app::stage_bars_left(16, 40) == 1u);  // a pattern shortened under the playhead: the last bar
}

TEST_CASE("stage: the bar the playhead is in counts from the start, agreeing with the bar ticks, also for lengths that are not a multiple of 16") {
  CHECK(app::stage_bar_of(64, 0) == 1u);
  CHECK(app::stage_bar_of(64, 16) == 2u);
  CHECK(app::stage_bar_of(64, 63) == 4u);
  CHECK(app::stage_bar_of(24, 8) == 1u);   // was "BAR 2 OF 2" when counted back from the end
  CHECK(app::stage_bar_of(24, 16) == 2u);
  CHECK(app::stage_bar_of(24, 23) == 2u);
  CHECK(app::stage_bar_of(40, 8) == 1u);
  CHECK(app::stage_bar_of(40, 32) == 3u);  // the short last bar
  CHECK(app::stage_bar_of(40, 39) == 3u);
  CHECK(app::stage_bar_of(40, 90) == 3u);  // past the end: the last bar
  CHECK(app::stage_bar_of(0, 5) == 0u);
}

TEST_CASE("stage: the progress bar follows the playhead to the microstep and stays inside the bar") {
  CHECK(app::stage_progress(16, 0, 0, 480) == 0u);
  CHECK(app::stage_progress(16, 8, 0, 480) == 240u);
  CHECK(app::stage_progress(16, 8, 24, 480) == 255u);  // half a step more: 8.5 / 16 of 480
  CHECK(app::stage_progress(16, 15, 47, 480) < 480u);
  CHECK(app::stage_progress(16, 99, 0, 480) == 480u);  // past the end: full, never over
  CHECK(app::stage_progress(0, 3, 3, 480) == 0u);
  CHECK(app::stage_progress(16, 3, 3, 0) == 0u);
  CHECK(app::stage_progress(128, 127, 47, 760) <= 760u);
}

TEST_CASE("stage: stopped, the cursor's slot is NOW and the one after it NEXT; the last loops to slot 0") {
  StageRig rig;
  const app::SongSource src(rig.model);
  const audio::PlayPosition::Value stopped;
  app::StageState s = app::stage_state(src, stopped, 1, 16);
  CHECK(s.mode == StageMode::Stopped);
  CHECK((s.now_slot == 1 && s.now_pattern == 2u && s.steps == 32u));
  CHECK((s.next_slot == 2 && s.next_pattern == 3u && s.next_kind == StageNext::Next));
  CHECK(s.bars_left == 0u);  // nothing counts while stopped
  s = app::stage_state(src, stopped, 2, 16);
  CHECK((s.next_slot == 0 && s.next_kind == StageNext::Loop));
  s = app::stage_state(src, stopped, 99, 16);  // a cursor past the list reads as the last slot, as the screen draws it
  CHECK(s.now_slot == 2);
  s = app::stage_state(src, stopped, -5, 16);
  CHECK(s.now_slot == 0);
}

TEST_CASE("stage: playing, NOW is the song row, NEXT the following slot, the countdown the bars to it") {
  StageRig rig;
  const app::SongSource src(rig.model);
  app::StageState s = app::stage_state(src, StageRig::at(2, 20, 5), 0, 16);
  CHECK(s.mode == StageMode::Song);
  CHECK((s.now_slot == 2 && s.now_pattern == 3u && s.steps == 64u && s.step == 20u && s.ustep == 5u));
  CHECK(s.bars_left == 3u);  // 44 steps left: bars 2, 3 and 4 of 4
  CHECK((s.next_slot == 0 && s.next_kind == StageNext::Loop && s.next_pattern == 1u));
  s = app::stage_state(src, StageRig::at(0, 15), 2, 16);
  CHECK((s.now_slot == 0 && s.next_slot == 1 && s.next_kind == StageNext::Next));
  CHECK(s.bars_left == 1u);
}

TEST_CASE("stage: a queued live jump replaces NEXT; a jump to a slot the list no longer has does not") {
  StageRig rig;
  const app::SongSource src(rig.model);
  audio::PlayPosition::Value v = StageRig::at(0, 0);
  v.queued = 2;
  app::StageState s = app::stage_state(src, v, 0, 16);
  CHECK((s.next_slot == 2 && s.next_kind == StageNext::Jump && s.next_pattern == 3u));
  v.queued = 40;
  s = app::stage_state(src, v, 0, 16);
  CHECK((s.next_slot == 1 && s.next_kind == StageNext::Next));
}

TEST_CASE("stage: pattern mode and a song row the list lost both read as the playing pattern looping") {
  StageRig rig;
  const app::SongSource src(rig.model);
  audio::PlayPosition::Value v;
  v.playing = true;
  v.order = audio::PlayPosition::kNoOrder;
  v.pattern = 2;
  v.step = 20;
  app::StageState s = app::stage_state(src, v, 0, 32);
  CHECK(s.mode == StageMode::Pattern);
  CHECK((s.now_slot == -1 && s.now_pattern == 2u && s.next_kind == StageNext::Loop && s.next_pattern == 2u));
  CHECK(s.bars_left == 1u);  // 12 steps left of 32
  v.order = 9;  // the list has three slots
  s = app::stage_state(src, v, 0, 32);
  CHECK((s.mode == StageMode::Pattern && s.now_slot == -1));
}

TEST_CASE("stage: an empty list has no slots to name, stopped or playing") {
  StageRig rig;
  test::arena(1).project.reset();  // (the rig's project: the model holds it read-only)
  const app::SongSource src(rig.model);
  const app::StageState s = app::stage_state(src, audio::PlayPosition::Value{}, 0, 16);
  CHECK((s.now_slot == -1 && s.next_slot == -1 && s.next_kind == StageNext::None));
}
