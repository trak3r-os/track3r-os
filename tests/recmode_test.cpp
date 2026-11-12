// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-RECMODE2 (docs/specs/OS-RECMODE2.md; the maintainer's design, new Community OS behaviour): REC and Shift + REC on the Pattern screen, the three states WATCH (Browse), STEP (Edit free) and LIVE (Edit following),
// every cell of the transitions table of section 2 (stopped and playing), the count-in only on Shift + REC from stopped, a LIVE take as one undo unit, and the end of LIVE when the transport stops by itself.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <memory>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::PatternMode;
namespace ds = o3os::model::device_setting;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

struct Sink final : app::TransportSink {
  unsigned plays = 0;
  unsigned counted = 0;
  unsigned stops = 0;
  unsigned bars = 0;
  bool ok = true;  // false: the transport refuses to start (play / play_count_in return false)
  bool play(unsigned) noexcept override {
    ++plays;
    return ok;
  }
  bool play_count_in(unsigned, unsigned b) noexcept override {
    ++counted;
    bars = b;
    return ok;
  }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override {
    ++stops;
    return true;
  }
};

enum class St { Watch, Step, Live };

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  audio::PlayPosition pos;
  Sink sink;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  Rig() {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, 64);
    (void)p.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_play_position(&pos);
    app->attach_transport(&sink);
    app->run_once();
    CHECK(app->device_settings().set(ds::CountIn, 0.0f) == model::Status::Ok);
  }
  const app::ViewState& state() const { return app->state(); }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  // The sequencer, as it publishes: `step` of pattern 1, or stopped.
  void play(unsigned step, bool playing = true) {
    pos.publish(playing, 1, step, 12000, audio::PlayPosition::kNoOrder, 0);
    pos.publish_count_in(0);
    pass();
  }
  void stop() { play(0, false); }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    pass();
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void shift_tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
    pass();
    tap(b);
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
    pass();
  }
  void pad(unsigned n) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(n), 100));
    pass();
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(n), 0));
    pass();
  }
  St st() const {
    const app::ViewState& s = state();
    if (s.mode[app::screen_index(app::ScreenId::Pattern)] != static_cast<unsigned>(PatternMode::Edit)) {
      return St::Watch;
    }
    return s.edit.rec_free ? St::Step : St::Live;
  }
  int row() const { return app->view().row(); }
  const model::Step& step(int track, int row) { return test::arena(0).project.pattern(1)->steps[track][row]; }
};

// Into a starting state, stopped or playing (at step 5). LIVE exists only while playing.
void enter(Rig& rig, St from, bool playing) {
  if (playing) {
    rig.play(5);
  }
  if (from == St::Step) {
    rig.tap(Button::Rec);
  } else if (from == St::Live) {
    rig.shift_tap(Button::Rec);
  }
  NEED(rig.st() == from);
}

}  // namespace

TEST_CASE("recmode: WATCH, playing") {
  {  // REC: STEP, the cursor stops on the play line's row; the song goes on
    Rig rig;
    enter(rig, St::Watch, true);
    rig.tap(Button::Rec);
    CHECK(rig.st() == St::Step);
    CHECK(rig.row() == 5);
    CHECK(rig.state().playing);
    rig.play(9);
    CHECK(rig.row() == 5);
    CHECK(rig.sink.stops == 0u);
  }
  {  // Shift + REC: LIVE (no new transport, no count-in)
    Rig rig;
    enter(rig, St::Watch, true);
    rig.shift_tap(Button::Rec);
    CHECK(rig.st() == St::Live);
    CHECK(rig.sink.plays == 0u);
    CHECK(rig.sink.counted == 0u);
    rig.play(9);
    CHECK(rig.row() == 9);
  }
  {  // PLAY: stop, WATCH, stopped
    Rig rig;
    enter(rig, St::Watch, true);
    rig.tap(Button::Play);
    CHECK(rig.sink.stops == 1u);
    CHECK_FALSE(rig.state().playing);
    CHECK(rig.st() == St::Watch);
  }
}

TEST_CASE("recmode: STEP, playing") {
  {  // REC: WATCH, following again
    Rig rig;
    enter(rig, St::Step, true);
    rig.tap(Button::Rec);
    CHECK(rig.st() == St::Watch);
    rig.play(9);
    CHECK(rig.row() == 9);
  }
  {  // Shift + REC: LIVE
    Rig rig;
    enter(rig, St::Step, true);
    rig.shift_tap(Button::Rec);
    CHECK(rig.st() == St::Live);
    CHECK(rig.sink.counted == 0u);
    rig.play(9);
    CHECK(rig.row() == 9);
  }
  {  // PLAY: stop; stay in STEP
    Rig rig;
    enter(rig, St::Step, true);
    rig.tap(Button::Play);
    CHECK(rig.sink.stops == 1u);
    CHECK_FALSE(rig.state().playing);
    CHECK(rig.st() == St::Step);
  }
}

TEST_CASE("recmode: LIVE") {
  {  // REC (OS-LIVEEXIT): WATCH; recording ends, the view keeps following the play line; playing goes on
    Rig rig;
    enter(rig, St::Live, true);
    rig.play(7);
    rig.pad(12);
    CHECK(rig.state().edit.take_open);
    const unsigned d0 = rig.journal.depth();
    rig.tap(Button::Rec);
    CHECK(rig.st() == St::Watch);
    CHECK_FALSE(rig.state().edit.take_open);
    CHECK(rig.journal.depth() == d0 + 1u);  // the take is one closed unit, not undone
    CHECK(rig.state().playing);
    CHECK(rig.sink.stops == 0u);
    rig.play(11);
    CHECK(rig.row() == 11);
    rig.pad(13);  // no longer recording
    CHECK(rig.step(0, 11).note == 0u);
    CHECK(rig.step(0, 7).note != 0u);
    CHECK(rig.app->invoke(core::action_id("edit.undo")) == app::DispatchResult::Changed);  // the whole take in one undo
    CHECK(rig.step(0, 7).note == 0u);
  }
  {  // OS-MASTERVOL: a turn of the master volume (Master held + the encoder) does not end the take; what is recorded before and after is one unit
    Rig rig;
    enter(rig, St::Live, true);
    rig.play(7);
    rig.pad(12);
    CHECK(rig.state().edit.take_open);
    const unsigned d0 = rig.journal.depth();
    const core::ParamId volume_id = core::param_id(core::ParamKind::Master, 0, model::master_param::Volume);
    const float vol = test::arena(0).project.get_param(volume_id);
    rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Master), true));
    rig.pass();
    rig.input.push(core::make_encoder_event(rig.clock.now(), 0, -5));
    rig.pass();
    CHECK(test::arena(0).project.get_param(volume_id) == vol - 5.0f);
    CHECK(rig.state().edit.take_open);  // the take goes on
    CHECK(rig.journal.depth() == d0);   // and the volume has no unit of its own
    rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Master), false));
    rig.pass();
    CHECK(rig.st() == St::Live);
    rig.play(11);
    rig.pad(13);
    CHECK(rig.step(0, 11).note != 0u);  // still recording
    rig.tap(Button::Rec);
    CHECK(rig.journal.depth() == d0 + 1u);  // both notes, one unit
    CHECK(rig.app->invoke(core::action_id("edit.undo")) == app::DispatchResult::Changed);
    CHECK(rig.step(0, 7).note == 0u);
    CHECK(rig.step(0, 11).note == 0u);
    CHECK(test::arena(0).project.get_param(volume_id) == vol - 5.0f);  // (the volume is not an undo step)
  }
  {  // Shift + REC (OS-UXF-PAT item 8): LIVE entered from WATCH goes back to WATCH; recording ends, the view keeps following; playing goes on
    Rig rig;
    enter(rig, St::Live, true);
    rig.play(7);
    rig.pad(12);
    CHECK(rig.state().edit.take_open);
    rig.shift_tap(Button::Rec);
    CHECK(rig.st() == St::Watch);
    CHECK_FALSE(rig.state().edit.take_open);
    CHECK(rig.state().playing);
    rig.play(11);
    CHECK(rig.row() == 11);
  }
  {  // Shift + REC (OS-LIVEEXIT, OS-UXF-PAT item 8): LIVE entered from STEP goes back to STEP, the cursor on the current row; recording ends; playing goes on
    Rig rig;
    enter(rig, St::Step, true);
    rig.shift_tap(Button::Rec);
    NEED(rig.st() == St::Live);
    rig.play(7);
    rig.pad(12);
    CHECK(rig.state().edit.take_open);
    rig.shift_tap(Button::Rec);
    CHECK(rig.st() == St::Step);
    CHECK_FALSE(rig.state().edit.take_open);
    CHECK(rig.row() == 7);
    CHECK(rig.state().playing);
    rig.play(11);
    CHECK(rig.row() == 7);
    rig.shift_tap(Button::Rec);  // on again from STEP; REC in LIVE is WATCH; and LIVE entered from WATCH leaves for WATCH
    CHECK(rig.st() == St::Live);
    rig.tap(Button::Rec);
    CHECK(rig.st() == St::Watch);
    rig.shift_tap(Button::Rec);
    CHECK(rig.st() == St::Live);
    rig.shift_tap(Button::Rec);
    CHECK(rig.st() == St::Watch);
  }
  {  // the cursor is on the row the play line is on even when the published position moved since the last pass
    Rig rig;
    enter(rig, St::Step, true);
    rig.shift_tap(Button::Rec);
    NEED(rig.st() == St::Live);
    rig.pos.publish(true, 1, 9, 12000, audio::PlayPosition::kNoOrder, 0);  // published, not yet read by a pass
    rig.shift_tap(Button::Rec);
    CHECK(rig.st() == St::Step);
    CHECK(rig.row() == 9);
  }
  {  // PLAY: stop; recording ends; WATCH, stopped
    Rig rig;
    enter(rig, St::Live, true);
    rig.play(7);
    rig.pad(12);
    rig.tap(Button::Play);
    CHECK(rig.sink.stops == 1u);
    CHECK_FALSE(rig.state().playing);
    CHECK(rig.st() == St::Watch);
    CHECK_FALSE(rig.state().edit.take_open);
  }
}

TEST_CASE("recmode: WATCH, stopped") {
  {  // REC: STEP at the cursor; no playback
    Rig rig;
    rig.tap(Button::Down);
    rig.tap(Button::Down);
    rig.tap(Button::Rec);
    CHECK(rig.st() == St::Step);
    CHECK(rig.row() == 2);
    CHECK(rig.sink.plays == 0u);
    CHECK(rig.sink.counted == 0u);
    CHECK_FALSE(rig.state().playing);
  }
  {  // Shift + REC: play + count-in + LIVE (COUNT-IN on)
    Rig rig;
    CHECK(rig.app->device_settings().set(ds::CountIn, 2.0f) == model::Status::Ok);
    rig.shift_tap(Button::Rec);
    CHECK(rig.sink.counted == 1u);
    CHECK(rig.sink.bars == 2u);
    CHECK(rig.sink.plays == 0u);
    CHECK(rig.state().playing);
    CHECK(rig.st() == St::Live);
  }
  {  // ... with COUNT-IN off, LIVE at once
    Rig rig;
    rig.shift_tap(Button::Rec);
    CHECK(rig.sink.counted == 0u);
    CHECK(rig.sink.plays == 1u);
    CHECK(rig.state().playing);
    CHECK(rig.st() == St::Live);
  }
  {  // REC or Shift + REC during the count-in (the orchestrator's default, to be confirmed by the maintainer): the count-in is cancelled, the transport stops, WATCH stopped, nothing written
    for (int shifted = 0; shifted < 2; ++shifted) {
      Rig rig;
      CHECK(rig.app->device_settings().set(ds::CountIn, 1.0f) == model::Status::Ok);
      rig.shift_tap(Button::Rec);
      CHECK(rig.st() == St::Live);
      rig.pos.publish(true, 1, 0, 12000, audio::PlayPosition::kNoOrder, 0);
      rig.pos.publish_count_in(3);  // the count-in is running
      rig.pass();
      const unsigned d0 = rig.journal.depth();
      if (shifted != 0) {
        rig.shift_tap(Button::Rec);
      } else {
        rig.tap(Button::Rec);
      }
      CHECK(rig.sink.stops == 1u);
      CHECK_FALSE(rig.state().playing);
      CHECK(rig.st() == St::Watch);
      CHECK_FALSE(rig.state().edit.take_open);
      CHECK(rig.journal.depth() == d0);
    }
  }
  {  // The same from STEP (OS-UXF-PAT item 8): Shift + REC began LIVE from STEP, so the cancel goes back to STEP stopped
    for (int shifted = 0; shifted < 2; ++shifted) {
      Rig rig;
      CHECK(rig.app->device_settings().set(ds::CountIn, 1.0f) == model::Status::Ok);
      rig.tap(Button::Rec);  // WATCH -> STEP
      CHECK(rig.st() == St::Step);
      rig.shift_tap(Button::Rec);
      CHECK(rig.st() == St::Live);
      rig.pos.publish(true, 1, 0, 12000, audio::PlayPosition::kNoOrder, 0);
      rig.pos.publish_count_in(3);
      rig.pass();
      if (shifted != 0) {
        rig.shift_tap(Button::Rec);
      } else {
        rig.tap(Button::Rec);
      }
      CHECK(rig.sink.stops == 1u);
      CHECK_FALSE(rig.state().playing);
      CHECK(rig.st() == St::Step);
      CHECK_FALSE(rig.state().edit.live_from_step);
      CHECK_FALSE(rig.state().edit.take_open);
    }
  }
  {  // PLAY: play, WATCH, playing; never a count-in
    Rig rig;
    CHECK(rig.app->device_settings().set(ds::CountIn, 1.0f) == model::Status::Ok);
    rig.tap(Button::Play);
    CHECK(rig.sink.plays == 1u);
    CHECK(rig.sink.counted == 0u);
    CHECK(rig.state().playing);
    CHECK(rig.st() == St::Watch);
  }
}

TEST_CASE("recmode: STEP, stopped") {
  {  // REC: WATCH
    Rig rig;
    enter(rig, St::Step, false);
    rig.tap(Button::Rec);
    CHECK(rig.st() == St::Watch);
    CHECK(rig.sink.plays == 0u);
  }
  {  // Shift + REC: play + count-in + LIVE
    Rig rig;
    CHECK(rig.app->device_settings().set(ds::CountIn, 1.0f) == model::Status::Ok);
    enter(rig, St::Step, false);
    rig.shift_tap(Button::Rec);
    CHECK(rig.sink.counted == 1u);
    CHECK(rig.sink.bars == 1u);
    CHECK(rig.state().playing);
    CHECK(rig.st() == St::Live);
  }
  {  // PLAY: play; stay in STEP, no count-in
    Rig rig;
    CHECK(rig.app->device_settings().set(ds::CountIn, 1.0f) == model::Status::Ok);
    enter(rig, St::Step, false);
    rig.tap(Button::Play);
    CHECK(rig.sink.plays == 1u);
    CHECK(rig.sink.counted == 0u);
    CHECK(rig.state().playing);
    CHECK(rig.st() == St::Step);
  }
}

TEST_CASE("recmode: Shift + REC with no transport that starts changes nothing, and does nothing off the Pattern screen") {
  Rig rig;
  rig.app->attach_transport(nullptr);
  rig.shift_tap(Button::Rec);
  CHECK(rig.st() == St::Watch);
  rig.tap(Button::Rec);
  CHECK(rig.st() == St::Step);
  rig.shift_tap(Button::Rec);
  CHECK(rig.st() == St::Step);
  rig.tap(Button::Song);
  rig.play(3);
  rig.shift_tap(Button::Rec);
  CHECK(rig.st() == St::Step);
  CHECK(rig.state().edit.rec_free);  // the Pattern state is untouched by Shift + REC on the Song screen
  CHECK(rig.state().mode[app::screen_index(app::ScreenId::Pattern)] == static_cast<std::uint8_t>(PatternMode::Edit));
}

TEST_CASE("recmode: Shift + REC refused by the transport returns false and changes nothing") {
  for (const float count_in : {0.0f, 2.0f}) {  // play() and play_count_in()
    for (const St from : {St::Watch, St::Step}) {
      Rig rig;
      CHECK(rig.app->device_settings().set(ds::CountIn, count_in) == model::Status::Ok);
      enter(rig, from, false);
      const app::ViewState before = rig.state();
      rig.sink.ok = false;
      CHECK(rig.app->invoke(core::action_id("pattern.rec_free")) == app::DispatchResult::Unchanged);  // Shift + REC's action: false
      CHECK((rig.sink.plays + rig.sink.counted) == 1u);  // it was asked
      CHECK(rig.st() == from);
      CHECK_FALSE(rig.state().playing);
      CHECK(rig.state().edit.rec_free == before.edit.rec_free);
      CHECK(rig.state().mode[app::screen_index(app::ScreenId::Pattern)] == before.mode[app::screen_index(app::ScreenId::Pattern)]);
    }
  }
}

TEST_CASE("recmode: a LIVE take is one undo unit; the notes go to the play line in LIVE and to the cursor in STEP") {
  Rig rig;
  rig.play(3);
  rig.tap(Button::Rec);        // STEP
  rig.shift_tap(Button::Rec);  // LIVE, entered from STEP
  const unsigned d0 = rig.journal.depth();
  rig.pad(12);
  rig.play(8);
  rig.pad(13);
  rig.play(12);
  rig.pad(14);
  CHECK(rig.step(0, 8).note != 0u);
  CHECK(rig.step(0, 12).note != 0u);
  rig.shift_tap(Button::Rec);  // LIVE -> STEP (OS-LIVEEXIT) closes the take
  CHECK(rig.journal.depth() == d0 + 1u);
  // STEP while playing: a pad enters at the cursor (the row the take left it on), not at the play line
  const int at = rig.row();
  rig.play(20);
  rig.pad(15);
  CHECK(rig.step(0, at).note != 0u);
  CHECK(rig.step(0, 20).note == 0u);
  CHECK(rig.row() == at + 1);
  CHECK(rig.journal.depth() == d0 + 2u);
  CHECK(rig.app->invoke(core::action_id("edit.undo")) == app::DispatchResult::Changed);  // the step entry
  CHECK(rig.app->invoke(core::action_id("edit.undo")) == app::DispatchResult::Changed);  // the whole take in one
  CHECK(rig.journal.depth() == d0);
  CHECK(rig.step(0, 8).note == 0u);
  CHECK(rig.step(0, 12).note == 0u);
}

TEST_CASE("recmode: the transport stopping by itself (the end of a song) ends LIVE, and its take; STEP stays") {
  {
    Rig rig;
    rig.play(3);
    rig.shift_tap(Button::Rec);
    const unsigned d0 = rig.journal.depth();
    rig.pad(12);
    CHECK(rig.state().edit.take_open);
    rig.stop();  // the sequencer says it stopped: nobody pressed Play
    CHECK(rig.st() == St::Watch);
    CHECK_FALSE(rig.state().playing);
    CHECK_FALSE(rig.state().edit.take_open);
    CHECK(rig.journal.depth() == d0 + 1u);
    rig.play(4);  // and a new run is not recording
    rig.pad(13);
    CHECK(rig.step(0, 4).note == model::kNoteOff);  // (the first note's OFF, written when the pad was let go: pad 13 wrote nothing)
  }
  {
    Rig rig;
    rig.play(3);
    rig.tap(Button::Rec);
    rig.stop();
    CHECK(rig.st() == St::Step);
  }
}

// OS-ROWWRAP: the wrap of a fresh Up / Down at the ends of the pattern, playing and stopped. WATCH and LIVE follow the playhead while it plays, so the arrows do not wrap there (the cursor
// is the playhead's); STEP's cursor is free and wraps as stopped.
TEST_CASE("OS-ROWWRAP: STEP wraps while playing; the cursor that follows the playhead does not") {
  Rig step;
  step.play(5);
  step.tap(Button::Rec);  // STEP, the cursor stops on the play line's row
  NEED(step.st() == St::Step);
  (void)step.app->invoke(core::action_id("cursor.jump_fraction"), 0);
  CHECK(step.row() == 0);
  step.tap(Button::Up);
  CHECK(step.row() == 63);
  step.tap(Button::Down);
  CHECK(step.row() == 0);

  Rig watch;  // WATCH, playing at step 0: the cursor rides the playhead; an Up does not wrap it
  watch.play(0);
  CHECK(watch.row() == 0);
  watch.tap(Button::Up);
  CHECK(watch.row() == 0);
  watch.play(63);
  CHECK(watch.row() == 63);
  watch.tap(Button::Down);
  CHECK(watch.row() == 63);

  Rig live;
  live.play(0);
  live.shift_tap(Button::Rec);
  NEED(live.st() == St::Live);
  live.tap(Button::Up);
  CHECK(live.row() == 0);

  Rig stopped;  // stopped, WATCH: it wraps
  stopped.tap(Button::Up);
  CHECK(stopped.row() == 63);
}

// OS-BUG-STEPSND: a pad is heard in every state that lets it be played. STEP wrote the note and was silent (stopped or playing); the pad plays on the cursor's track, once, and its release
// is passed on. Which track the engine gives it when that track sounds is pick_track's (audio/live_notes.h); the sequencer's next note on the track cuts it.
namespace {
struct PadSink final : app::PreviewSink {
  unsigned downs = 0, ups = 0, track = 99, note = 0;
  void preview(unsigned, unsigned) noexcept override {}
  void pad_down(unsigned, unsigned t, unsigned, unsigned n) noexcept override {
    ++downs;
    track = t;
    note = n;
  }
  void pad_up(unsigned) noexcept override { ++ups; }
};
}  // namespace

TEST_CASE("OS-BUG-STEPSND: a pad sounds in STEP, stopped or playing, and in LIVE, once, on the cursor's track; its release is passed on") {
  for (const bool playing : {false, true}) {
    Rig rig;
    PadSink pads;
    rig.app->attach_preview(&pads);
    (void)rig.app->invoke(core::action_id("cursor.track"), 3);  // the cursor on track 3
    enter(rig, St::Step, playing);
    const int row = rig.row();
    rig.pad(14);
    CHECK_MESSAGE(pads.downs == 1u, "playing " << playing);
    CHECK(pads.ups == 1u);
    CHECK(pads.track == 3u);
    CHECK(rig.step(3, row).note != 0);  // written at the cursor's row (the cursor then moved on by JUMP)
  }
  Rig live;
  PadSink pads;
  live.app->attach_preview(&pads);
  (void)live.app->invoke(core::action_id("cursor.track"), 3);
  enter(live, St::Live, true);
  live.pad(14);
  CHECK(pads.downs == 1u);  // LIVE: the take's pad sounds itself
  CHECK(pads.ups == 1u);
  CHECK(pads.track == 3u);
}

// E-GLOB-06 (new Community OS behaviour): Rec, then Play inside the chord window, is Shift + Rec; neither key then taps. Play never waits (OS-BATCH-E2E), so Play first is a plain Play tap.
TEST_CASE("recmode: REC then PLAY inside the chord window is LIVE only, with no stray REC toggle or PLAY") {
  const auto press = [](Rig& rig, Button b, bool down) {
    rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(b), down));
    rig.pass(5);
  };
  {
    const Button first = Button::Rec;
    const Button second = Button::Play;
    {  // playing, WATCH: LIVE, the transport neither stopped nor restarted
      Rig rig;
      enter(rig, St::Watch, true);
      press(rig, first, true);
      press(rig, second, true);
      press(rig, Button::Rec, false);
      press(rig, Button::Play, false);
      rig.pass(100);
      CHECK(rig.st() == St::Live);
      CHECK(rig.state().playing);
      CHECK(rig.sink.stops == 0u);
      CHECK(rig.sink.plays == 0u);
    }
    {  // stopped (COUNT-IN off): the transport starts and it is LIVE
      Rig rig;
      press(rig, first, true);
      press(rig, second, true);
      press(rig, Button::Rec, false);
      press(rig, Button::Play, false);
      rig.pass(100);
      CHECK(rig.sink.plays == 1u);
      CHECK(rig.sink.stops == 0u);
      CHECK(rig.st() == St::Live);
    }
    {  // LIVE: the chord ends it (back to WATCH, where it was entered from), the transport keeps going
      Rig rig;
      enter(rig, St::Live, true);
      press(rig, first, true);
      press(rig, second, true);
      press(rig, Button::Rec, false);
      press(rig, Button::Play, false);
      rig.pass(100);
      CHECK(rig.st() == St::Watch);
      CHECK(rig.sink.stops == 0u);
    }
  }
  {  // PLAY never waits for a partner: its tap is delivered at the press (the transport start is a sound trigger, aligned with the pads), so PLAY first is a plain PLAY and the REC after it a plain REC
    Rig rig;
    CHECK(rig.sink.plays == 0u);
    rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Play), true));
    rig.pass(1);
    CHECK(rig.sink.plays == 1u);  // before the release and long before the 40 ms window is over
    press(rig, Button::Rec, true);
    press(rig, Button::Play, false);
    press(rig, Button::Rec, false);
    rig.pass(100);
    CHECK(rig.sink.plays == 1u);
    CHECK(rig.st() != St::Live);
  }
  {  // more than the window apart they are two taps again: REC (STEP), then PLAY (stop)
    Rig rig;
    enter(rig, St::Watch, true);
    press(rig, Button::Rec, true);
    rig.pass(100);
    press(rig, Button::Play, true);
    press(rig, Button::Rec, false);
    press(rig, Button::Play, false);
    rig.pass(100);
    CHECK(rig.st() == St::Step);  // the REC tap gave STEP; the PLAY tap stops the transport and leaves STEP as it is
    CHECK(rig.sink.stops == 1u);
  }
}
