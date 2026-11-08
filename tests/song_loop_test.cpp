// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SONG3 S7 (new Community OS behaviour): hold F2 on the Song screen loops the selected slots (the cursor's alone with no range). The key goes through TransportSink::loop_range; the sequencer's own
// behaviour (the wrap, the edits of the list, the Rec jump) is tests/seq_order_sync_test.cpp ("song loop: ..."). What is read back is audio::PlayPosition::Value::loop_first / loop_last.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <memory>
#include <string>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];

struct LoopLog final : app::TransportSink {
  unsigned plays = 0;
  unsigned play_row = 0;
  unsigned loops = 0;
  unsigned loop_first = 0;
  unsigned loop_last = 0;
  unsigned jumps = 0;
  unsigned jump_row = 0;
  bool accept_loop = true;
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned row) noexcept override {
    ++plays;
    play_row = row;
    return true;
  }
  bool stop() noexcept override { return true; }
  bool queue_jump(unsigned row) noexcept override {
    ++jumps;
    jump_row = row;
    return true;
  }
  bool loop_range(unsigned first, unsigned last) noexcept override {
    if (!accept_loop) {
      return false;
    }
    ++loops;
    loop_first = first;
    loop_last = last;
    return true;
  }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  LoopLog log;
  audio::PlayPosition position;

  explicit Rig(unsigned slots = 6) {
    model::Project& p = test::arena(0).project;
    p.reset();
    for (unsigned id = 1; id <= 3; ++id) {
      NEED(p.create_pattern(id, 16) == model::Status::Ok);
    }
    for (unsigned i = 0; i < slots; ++i) {
      NEED(p.append_order(1 + i % 3) == model::Status::Ok);
    }
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_transport(&log);
    app->attach_play_position(&position);
    app->run_once();
    tap(Button::Song);
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b) {
    press(b);
    pass(1);
    release(b);
    pass(1);
  }
  void hold(Button b) {
    press(b);
    pass(1);
    pass(500);  // past the hold time
    release(b);
    pass(1);
  }
  void shift_tap(Button b) {
    press(Button::Shift);
    pass(1);
    tap(b);
    release(Button::Shift);
    pass(1);
  }
  void select_slots(unsigned first, unsigned last) {  // the cursor to `first` (Down), then a range to `last` (Shift + Down)
    for (unsigned i = 0; i < first; ++i) {
      tap(Button::Down);
    }
    for (unsigned i = first; i < last; ++i) {
      shift_tap(Button::Down);
    }
  }
  void playing_at(unsigned order, unsigned lf = audio::PlayPosition::kNoOrder, unsigned ll = audio::PlayPosition::kNoOrder) {
    position.publish(true, 1 + order % 3, 0, 12000, order);
    position.publish_loop(lf, ll);
    pass(1);
  }
  std::string message() const { return app->message(); }
};

}  // namespace

TEST_CASE("song loop key: held with a range stopped, the song starts at its first slot and the range loops") {
  Rig rig;
  rig.select_slots(1, 3);
  rig.hold(Button::F2);
  CHECK(rig.log.plays == 1u);
  CHECK(rig.log.play_row == 1u);
  CHECK(rig.log.loops == 1u);
  CHECK(rig.log.loop_first == 1u);
  CHECK(rig.log.loop_last == 3u);
  CHECK(rig.log.jumps == 0u);
  CHECK(rig.message() == "LOOP 01-03");
}

TEST_CASE("song loop key: no range is the cursor's slot alone") {
  Rig rig;
  rig.select_slots(2, 2);
  rig.hold(Button::F2);
  CHECK(rig.log.play_row == 2u);
  CHECK((rig.log.loop_first == 2u && rig.log.loop_last == 2u));
  CHECK(rig.message() == "LOOP 02-02");
}

TEST_CASE("song loop key: a tap of F2 is still Play song, at the release") {
  Rig rig;
  rig.select_slots(1, 1);
  rig.tap(Button::F2);
  CHECK(rig.log.plays == 1u);
  CHECK(rig.log.play_row == 1u);
  CHECK(rig.log.loops == 0u);  // (the loop is only the held key's)
}

TEST_CASE("song loop key: playing inside the range, only the loop is set; outside it, the first slot is queued as a Rec jump") {
  Rig rig;
  rig.select_slots(1, 3);
  rig.playing_at(2);
  rig.hold(Button::F2);
  CHECK(rig.log.plays == 0u);
  CHECK(rig.log.loops == 1u);
  CHECK(rig.log.jumps == 0u);

  Rig out;
  out.select_slots(1, 3);
  out.playing_at(5);
  out.hold(Button::F2);
  CHECK(out.log.plays == 0u);
  CHECK(out.log.loops == 1u);
  CHECK(out.log.jumps == 1u);
  CHECK(out.log.jump_row == 1u);
}

TEST_CASE("song loop key: the same range held again takes the loop back, another range replaces it") {
  Rig rig;
  rig.select_slots(1, 3);
  rig.playing_at(2, 1, 3);  // the sequencer says the range 1..3 loops
  rig.hold(Button::F2);
  CHECK(rig.log.loops == 1u);
  CHECK(rig.log.loop_last == 0xFFFFu);  // the cancel
  CHECK(rig.message() == "LOOP OFF");

  Rig other;
  other.select_slots(1, 4);
  other.playing_at(2, 1, 3);
  other.hold(Button::F2);
  CHECK((other.log.loop_first == 1u && other.log.loop_last == 4u));
  CHECK(other.message() == "LOOP 01-04");
}

TEST_CASE("song loop key: an empty song, and a transport that refuses the loop, say so and start nothing more") {
  Rig empty(0);
  empty.hold(Button::F2);
  CHECK(empty.message() == "SONG IS EMPTY");
  CHECK(empty.log.loops == 0u);

  Rig rig;
  rig.log.accept_loop = false;
  rig.select_slots(1, 2);
  rig.playing_at(0);
  rig.hold(Button::F2);
  CHECK(rig.message() == "LOOP NOT SET");
  CHECK(rig.log.jumps == 0u);
}

TEST_CASE("song loop: the position carries the range the sequencer published") {
  audio::PlayPosition pp;
  CHECK_FALSE(pp.read().looping());
  pp.publish_loop(3, 6);
  CHECK(pp.read().looping());
  CHECK((pp.read().loop_first == 3u && pp.read().loop_last == 6u));
  pp.publish_loop(0, 0);  // slot 0 alone is a range
  CHECK((pp.read().looping() && pp.read().loop_first == 0u && pp.read().loop_last == 0u));
  pp.publish_loop(0xFF, 0);  // the sequencer's "no loop"
  CHECK_FALSE(pp.read().looping());
  CHECK(pp.read().loop_first == audio::PlayPosition::kNoOrder);
}
