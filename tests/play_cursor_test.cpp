// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PLAYCUR (docs/specs/OS-RECFLOW.md; new Community OS behaviour): Shift + Play on the Pattern screen plays from the cursor's row. Stopped, the pattern being edited starts at that row; playing, the play
// line jumps to it (a locate on the transport) and the transport, the take and the mode are untouched. It never counts in (COUNT-IN stays Shift + REC from stop). With a block selected it is the Focus
// Loop's reserved key and does the same until that exists. Elsewhere than Pattern it is the plain Play.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <memory>
#include <string>

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
  unsigned from_calls = 0;
  unsigned locates = 0;
  unsigned pattern = 0;
  unsigned row = 0;
  unsigned located = 0;
  unsigned loops = 0;
  unsigned loop_first = 0;
  unsigned loop_last = 0;
  bool ok = true;
  bool loop_ok = true;  // loop_rows alone refuses (the command ring is full)
  bool play(unsigned) noexcept override {
    ++plays;
    return ok;
  }
  bool play_count_in(unsigned, unsigned) noexcept override {
    ++counted;
    return ok;
  }
  bool play_from(unsigned p, unsigned r) noexcept override {
    ++from_calls;
    pattern = p;
    row = r;
    return ok;
  }
  bool locate_row(unsigned r) noexcept override {
    ++locates;
    located = r;
    return ok;
  }
  bool loop_rows(unsigned first, unsigned last) noexcept override {
    ++loops;
    loop_first = first;
    loop_last = last;
    return ok && loop_ok;
  }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override {
    ++stops;
    return true;
  }
};

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
    (void)p.create_pattern(2, 64);
    (void)p.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_play_position(&pos);
    app->attach_transport(&sink);
    app->run_once();
    CHECK(app->device_settings().set(ds::CountIn, 2.0f) == model::Status::Ok);  // COUNT-IN 2 bars: Shift + Play must not use it
  }
  const app::ViewState& state() const { return app->state(); }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void play(unsigned step, unsigned pattern = 1, bool playing = true) {
    pos.publish(playing, pattern, step, 12000, audio::PlayPosition::kNoOrder, 0);
    pos.publish_count_in(0);
    pass();
  }
  // The engine's view of a running row loop (what Sequencer::set_row_loop publishes) at the line's `step`.
  void play_looping(unsigned step, unsigned first, unsigned last, unsigned pattern = 1) {
    pos.publish_row_loop(first, last);
    play(step, pattern);
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    pass();
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void shift_tap(Button b, unsigned n = 1) {  // (Shift held across the n taps: letting it go ends the block's extension, the next one starts afresh)
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
    pass();
    for (unsigned i = 0; i < n; ++i) {
      tap(b);
    }
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
    pass();
  }
  void down(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      tap(Button::Down);
    }
  }
  int row() const { return app->view().row(); }
  bool step_mode() const {
    return state().mode[app::screen_index(app::ScreenId::Pattern)] == static_cast<unsigned>(PatternMode::Edit) && state().edit.rec_free;
  }
};

}  // namespace

TEST_CASE("play cursor: stopped, Shift + Play starts the pattern at the cursor's row, in WATCH and in STEP, and never counts in") {
  {
    Rig rig;  // WATCH, stopped
    rig.down(20);
    CHECK(rig.row() == 20);
    rig.shift_tap(Button::Play);
    CHECK(rig.sink.from_calls == 1u);
    CHECK(rig.sink.pattern == 1u);
    CHECK(rig.sink.row == 20u);
    CHECK(rig.sink.plays == 0u);
    CHECK(rig.sink.counted == 0u);  // COUNT-IN is 2 bars
    CHECK(rig.state().playing);
    rig.play(20);  // the line is where the cursor was
    CHECK(rig.row() == 20);
  }
  {
    Rig rig;  // STEP, stopped: the cursor stays, so does the mode
    rig.tap(Button::Rec);
    CHECK(rig.step_mode());
    rig.down(7);
    rig.shift_tap(Button::Play);
    CHECK(rig.sink.from_calls == 1u);
    CHECK(rig.sink.row == 7u);
    CHECK(rig.state().playing);
    CHECK(rig.step_mode());
    rig.play(7);
    rig.play(9);
    CHECK(rig.row() == 7);
  }
  {
    Rig rig;  // the first row is a start from row 1, still no count-in
    rig.shift_tap(Button::Play);
    CHECK(rig.sink.row == 0u);
    CHECK(rig.sink.counted == 0u);
  }
}

TEST_CASE("play cursor: playing, Shift + Play moves the play line to the cursor's row and leaves the transport, the mode and the take alone") {
  Rig rig;
  rig.tap(Button::Rec);  // STEP: a free cursor while the song plays
  rig.play(3);
  rig.down(30);
  CHECK(rig.row() == 30);
  rig.shift_tap(Button::Play);
  CHECK(rig.sink.locates == 1u);
  CHECK(rig.sink.located == 30u);
  CHECK(rig.sink.from_calls == 0u);
  CHECK(rig.sink.stops == 0u);
  CHECK(rig.sink.counted == 0u);
  CHECK(rig.state().playing);
  CHECK(rig.step_mode());
  CHECK(rig.row() == 30);
  // LIVE: the cursor rides the line, so the row is the line's: a jump to where it already is, no stop, the take is not closed.
  Rig live;
  live.play(5);
  live.shift_tap(Button::Rec);  // LIVE
  live.play(12);
  live.shift_tap(Button::Play);
  CHECK(live.sink.locates == 1u);
  CHECK(live.sink.located == 12u);
  CHECK(live.sink.stops == 0u);
  CHECK(live.state().playing);
  CHECK_FALSE(live.step_mode());
  CHECK(live.state().mode[app::screen_index(app::ScreenId::Pattern)] == static_cast<unsigned>(PatternMode::Edit));
}

TEST_CASE("focus loop: stopped with a block selected, Shift + Play starts at the block's first row and loops its last, the block stays, no count-in") {
  Rig rig;
  rig.tap(Button::Rec);
  rig.down(4);
  rig.shift_tap(Button::Down, 3);
  CHECK(rig.state().edit.selection.active);
  CHECK(rig.row() == 7);  // the block is rows 4..7, the cursor its moving end
  rig.shift_tap(Button::Play);
  CHECK(rig.sink.from_calls == 1u);
  CHECK(rig.sink.pattern == 1u);
  CHECK(rig.sink.row == 4u);
  CHECK(rig.sink.loops == 1u);
  CHECK(rig.sink.loop_first == 4u);
  CHECK(rig.sink.loop_last == 7u);
  CHECK(rig.sink.counted == 0u);
  CHECK(rig.sink.plays == 0u);
  CHECK(rig.state().edit.selection.active);
  CHECK(rig.state().playing);
}

TEST_CASE("focus loop: a loop command the transport refuses after the start still plays, and says so") {
  Rig rig;
  rig.sink.loop_ok = false;
  rig.tap(Button::Rec);
  rig.down(2);
  rig.shift_tap(Button::Down, 2);
  rig.shift_tap(Button::Play);
  CHECK(rig.sink.from_calls == 1u);
  CHECK(rig.sink.loops == 1u);
  CHECK(rig.state().playing);
  CHECK(std::string(rig.app->message()) == "LOOP NOT SET (BUSY)");
}

TEST_CASE("focus loop: the block's anchor below the cursor gives the same rows; a refusing transport starts nothing") {
  Rig rig;
  rig.tap(Button::Rec);
  rig.down(9);
  rig.shift_tap(Button::Up, 2);
  CHECK(rig.state().edit.selection.active);
  CHECK(rig.row() == 7);  // anchor 9, cursor 7
  rig.shift_tap(Button::Play);
  CHECK(rig.sink.row == 7u);
  CHECK(rig.sink.loop_first == 7u);
  CHECK(rig.sink.loop_last == 9u);
  Rig no;
  no.sink.ok = false;
  no.tap(Button::Rec);
  no.shift_tap(Button::Down);
  no.shift_tap(Button::Play);
  CHECK_FALSE(no.state().playing);
}

TEST_CASE("focus loop: playing, Shift + Play jumps to the block's first row and sets the loop; while it loops, Shift + Play and Play stop it") {
  Rig rig;
  rig.tap(Button::Rec);
  rig.play(30);
  rig.down(4);
  rig.shift_tap(Button::Down, 2);
  CHECK(rig.state().edit.selection.active);
  rig.shift_tap(Button::Play);
  CHECK(rig.sink.locates == 1u);
  CHECK(rig.sink.located == 4u);
  CHECK(rig.sink.from_calls == 0u);
  CHECK(rig.sink.loops == 1u);
  CHECK(rig.sink.loop_first == 4u);
  CHECK(rig.sink.loop_last == 6u);
  CHECK(rig.sink.stops == 0u);
  rig.play_looping(5, 4, 6);  // the engine reports the loop
  CHECK(rig.state().play.row_looping());
  rig.shift_tap(Button::Play);  // again: stop, no new jump or loop
  CHECK(rig.sink.stops == 1u);
  CHECK(rig.sink.locates == 1u);
  CHECK(rig.sink.loops == 1u);
  Rig plain;
  plain.tap(Button::Rec);
  plain.shift_tap(Button::Down);
  plain.play_looping(1, 0, 1);
  plain.tap(Button::Play);
  CHECK(plain.sink.stops == 1u);
}

TEST_CASE("focus loop: with no block Shift + Play is the play-from-cursor of OS-PLAYCUR and sets no loop") {
  Rig rig;
  rig.down(6);
  rig.shift_tap(Button::Play);
  CHECK(rig.sink.from_calls == 1u);
  CHECK(rig.sink.row == 6u);
  CHECK(rig.sink.loops == 0u);
  Rig playing;
  playing.tap(Button::Rec);  // STEP: the cursor is free of the playhead
  playing.play(2);
  playing.down(8);
  playing.shift_tap(Button::Play);
  CHECK(playing.sink.locates == 1u);
  CHECK(playing.sink.located == 8u);
  CHECK(playing.sink.loops == 0u);
}

TEST_CASE("play cursor: a playing pattern that is not the one being edited is refused with a message; a transport that refuses changes nothing; the plain Play is untouched") {
  Rig rig;
  rig.tap(Button::Rec);
  rig.play(3, 2);  // the line is in pattern 2, the view's pattern is 1 (a free cursor)
  rig.down(5);
  rig.shift_tap(Button::Play);
  CHECK(rig.sink.locates == 0u);
  CHECK(rig.sink.stops == 0u);
  CHECK(rig.state().playing);
  CHECK(std::string(rig.app->message()) == "THE PLAYHEAD IS IN ANOTHER PATTERN");
  // A refusing transport: stopped, nothing starts and the badge stays.
  Rig no;
  no.sink.ok = false;
  no.down(4);
  no.shift_tap(Button::Play);
  CHECK(no.sink.from_calls == 1u);
  CHECK_FALSE(no.state().playing);
  // Plain Play: the whole pattern from its first step, no from-row call; and Stop.
  Rig plain;
  plain.down(9);
  plain.tap(Button::Play);
  CHECK(plain.sink.plays == 1u);
  CHECK(plain.sink.from_calls == 0u);
  plain.play(0);
  plain.tap(Button::Play);
  CHECK(plain.sink.stops == 1u);
}

TEST_CASE("play cursor: off the Pattern screen Shift + Play is the plain Play") {
  Rig rig;
  rig.tap(Button::Song);
  rig.shift_tap(Button::Play);
  CHECK(rig.sink.from_calls == 0u);
  CHECK(rig.sink.locates == 0u);
}
