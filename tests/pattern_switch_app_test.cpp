// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PATSWITCH (docs/specs/OS-PATSWITCH.md), the App's side: F1 HELD on Pattern switches the playing pattern at once (the sequencer's rule is tests/seq_patswitch_test.cpp).
//   * a tap of F1 opens nothing; the hold opens the PATTERN chooser and the release closes it; stopped, in a count-in or with a sink that refuses, a pick is a view pick as before;
//   * held and playing, a pad, the encoder, Up, Down and Shift + encoder switch the PLAYING pattern (TransportSink::switch_pattern), in WATCH, LIVE and STEP alike, and the pads light as PATS lights them;
//   * a pick is compared with where the sequencer is going (the pending switch, else the pattern that plays), not with the view: a pick back takes a switch back, a view parked on another pattern still switches;
//   * the view holds the pick until the switch lands (and no longer than a few steps); the PATS blink (the queue beaten by a live jump) ends, but not for a queue made after a switch that lands;
//   * a pattern the project lacks (PATS queued, F1 held, stopped) is gone to: the transport runs on, the view follows with the play bar, and nothing is created.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/pad_lights.h>
#include <o3os/app/pattern_edit.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/audio/play_position.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::PadRole;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

// A transport that takes every request (or none, `accept`), and keeps what the held chooser and the PATS pads asked for.
struct Sink final : app::TransportSink {
  std::vector<unsigned> picks;  // switch_pattern(id), in order
  unsigned queued = 0;          // queue_pattern(id)
  unsigned queue_calls = 0;
  unsigned refused = 0;
  bool accept = true;
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
  bool queue_pattern(unsigned id) noexcept override {
    if (!accept) {
      return false;
    }
    queued = id;
    ++queue_calls;
    return true;
  }
  bool switch_pattern(unsigned id) noexcept override {
    if (!accept) {
      ++refused;
      return false;
    }
    picks.push_back(id);
    return true;
  }
  unsigned last() const { return picks.empty() ? 0u : picks.back(); }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  Sink sink;
  audio::PlayPosition position;
  unsigned base_depth = 0;

  // Pattern 1 holds a note, 2 holds a note, 3 is empty (it exists), 9 does not exist.
  explicit Rig(unsigned length = 16) {
    model::Project& p = test::arena(0).project;
    p.reset();
    p.start_new();  // C major, the runtime's default: the document is not unsaved at the start (OS-DEFSCALE)
    (void)p.create_pattern(1, length);
    (void)p.create_pattern(2, length);
    (void)p.create_pattern(3, length);
    (void)p.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_transport(&sink);
    app->attach_play_position(&position);
    model::Step note = model::kEmptyStep;
    note.note = 61;
    CHECK(editor->set_steps(1, 0, 2, &note, 1) == model::Status::Ok);
    CHECK(editor->set_steps(2, 0, 4, &note, 1) == model::Status::Ok);
    base_depth = journal.depth();  // (the setup is not what the tests count)
    app->run_once();
  }
  ~Rig() { app.reset(); }  // (the App's destructor closes an open take on the editor, which is destroyed before the App by the member order)
  model::Project& project() { return test::arena(0).project; }
  const app::ViewState& state() const { return app->state(); }
  const app::PatternEdit& edit() const { return app->state().edit; }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b) {
    press(b);
    pass(1);
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
  // OS-UXF-PAT 7: the pads' modes are the PADS picker's (Shift + F2, then F1..F6 = NOTES PIANO ISO TRACKS PATS VU); `old` is the former Shift key of the mode (F2 NOTES .. F7 VU).
  void pad_mode(Button old) {
    shift_tap(Button::F2);
    tap(static_cast<Button>(static_cast<unsigned>(old) - 1u));
  }
  void hold_f1() {
    press(Button::F1);
    pass(core::kGestureTimings.hold_ms + 1);
  }
  void release_f1() {
    release(Button::F1);
    pass(1);
  }
  void tap_pad(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 100));
    pass(1);
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 0));
    pass(1);
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass(1);
  }
  void shift_turn(int detents) {
    press(Button::Shift);
    pass(1);
    turn(detents);
    release(Button::Shift);
    pass(1);
  }
  // The published position: playing `pattern` at `step` (order 0xFFFF: Pattern mode; a number: that song row).
  void play(unsigned pattern, unsigned step, std::uint32_t order = audio::PlayPosition::kNoOrder) {
    position.publish(true, pattern, step, 12000, order);
    pass(1);
  }
  void stop() {
    position.publish(false, 1, 0, 12000);
    pass(1);
  }
  bool lights(PadRole (&roles)[48]) const { return app->pad_lights(roles); }
  // STEP: Edit with the free cursor (REC); the view then stays where the user put it while the sequencer plays.
  void step_mode() {
    (void)app->invoke(core::action_id("view.mode"), static_cast<core::ActionArg>(app::PatternMode::Browse));
    (void)app->invoke(core::action_id("pattern.edit_toggle"));
  }
  unsigned depth() const { return journal.depth() - base_depth; }
};

}  // namespace

TEST_CASE("patswitch app: a tap of F1 opens the PATTERN chooser (OS-UXF-PAT item 6) and F1 or F8 closes it; the hold opens it and the release closes it, in every state") {
  Rig rig;
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::PatternChooser);
  CHECK(rig.edit().chooser_held);  // the live PATTERN chooser: pads, Copy and Paste work as under the hold
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  rig.tap(Button::F1);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK_FALSE(rig.edit().chooser_held);  // F8 clears the "live PATTERN chooser" flag with the overlay
  CHECK(rig.sink.picks.empty());
  rig.hold_f1();
  CHECK(rig.state().overlay == app::OverlayId::PatternChooser);
  CHECK(rig.edit().chooser_held);
  rig.release_f1();
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK_FALSE(rig.edit().chooser_held);
  // playing: the same
  rig.play(1, 4);
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::PatternChooser);
  rig.tap_pad(2);  // a tap-opened chooser switches the playing pattern like the hold's
  NEED(rig.sink.picks.size() == 1u);
  CHECK(rig.sink.last() == 3);
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  rig.sink.picks.clear();
  rig.hold_f1();
  CHECK(rig.state().overlay == app::OverlayId::PatternChooser);
  rig.release_f1();
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.sink.picks.empty());  // opening and closing never touch the playback
}

TEST_CASE("patswitch app: F1 held while playing: a pad, the encoder, Up, Down and Shift + encoder switch the playing pattern; the view keeps the pick until it lands") {
  Rig rig;
  rig.play(1, 4);
  rig.hold_f1();
  NEED(rig.state().overlay == app::OverlayId::PatternChooser);
  rig.tap_pad(2);  // pattern 3
  NEED(rig.sink.picks.size() == 1u);
  CHECK(rig.sink.last() == 3);
  CHECK(rig.edit().pattern == 3);
  CHECK(rig.edit().switch_to == 3);
  // The sequencer still plays pattern 1 until its step boundary: the view is not pulled back to it.
  rig.play(1, 5);
  CHECK(rig.edit().pattern == 3);
  CHECK(rig.edit().switch_to == 3);
  // It lands: the wait is over, the view and the playing pattern agree.
  rig.play(3, 6);
  CHECK(rig.edit().switch_to == 0);
  CHECK(rig.edit().pattern == 3);
  // The encoder counts from the last pick: +2 asks for 5, Up 6, Down 5, Shift + one detent 13.
  rig.turn(2);
  CHECK(rig.sink.last() == 5);
  rig.tap(Button::Up);
  CHECK(rig.sink.last() == 6);
  rig.tap(Button::Down);
  CHECK(rig.sink.last() == 5);
  rig.shift_turn(1);
  CHECK(rig.sink.last() == 13);
  CHECK(rig.edit().pattern == 13);
  CHECK(rig.edit().switch_to == 13);
  CHECK(rig.depth() == 0u);  // a pick is not an undo unit
  rig.release_f1();
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.edit().switch_to == 13);  // the release does not take it back: it lands at its boundary
  rig.play(13, 7);
  CHECK(rig.edit().switch_to == 0);
  CHECK(rig.edit().pattern == 13);
}

TEST_CASE("patswitch app: a pick is compared with where the sequencer is going: a pick back takes the switch back, a detent forth and back sends two and ends with nothing waiting") {
  Rig rig;
  rig.play(1, 4);
  rig.hold_f1();
  rig.tap_pad(2);  // 3
  rig.tap_pad(0);  // back to 1, the pattern that plays, inside the window
  NEED(rig.sink.picks.size() == 2u);
  CHECK(rig.sink.picks[0] == 3);
  CHECK(rig.sink.picks[1] == 1);  // sent: the sequencer takes the waiting switch back
  CHECK(rig.edit().switch_to == 0);
  CHECK(rig.edit().pattern == 1);
  rig.turn(1);  // 2
  rig.turn(-1);  // 1 again
  NEED(rig.sink.picks.size() == 4u);
  CHECK(rig.sink.picks[2] == 2);
  CHECK(rig.sink.picks[3] == 1);
  CHECK(rig.edit().switch_to == 0);
  // The same pattern twice does not ask twice: the pending target is where it is going.
  rig.tap_pad(2);
  rig.tap_pad(2);
  CHECK(rig.sink.picks.size() == 5u);
  CHECK(rig.sink.last() == 3);
  CHECK(rig.edit().switch_to == 3);
  // In Pattern mode the pick of the pattern that plays, with nothing waiting, sends nothing.
  rig.play(3, 5);
  NEED(rig.edit().switch_to == 0);
  rig.tap_pad(2);
  CHECK(rig.sink.picks.size() == 5u);
  CHECK(rig.edit().pattern == 3);
}

TEST_CASE("patswitch app: a view parked on another pattern (STEP) still switches to it: the pick is not lost to the early-out of the view pick") {
  Rig rig;
  rig.hold_f1();
  rig.tap_pad(4);  // stopped: the view goes to pattern 5
  rig.release_f1();
  NEED(rig.edit().pattern == 5);
  CHECK(rig.sink.picks.empty());
  rig.step_mode();
  rig.play(1, 3);
  CHECK(rig.edit().pattern == 5);  // STEP: the view is the user's, 1 plays
  rig.hold_f1();
  rig.tap_pad(4);  // pattern 5 again: the view already is on it, but 1 plays
  NEED(rig.sink.picks.size() == 1u);
  CHECK(rig.sink.last() == 5);
  CHECK(rig.edit().switch_to == 5);
  rig.release_f1();
  rig.play(5, 4);
  CHECK(rig.edit().switch_to == 0);
  CHECK(rig.edit().pattern == 5);
  // The same rule in LIVE and in WATCH: the pick switches.
  (void)rig.app->invoke(core::action_id("view.mode"), static_cast<core::ActionArg>(app::PatternMode::Browse));
  rig.hold_f1();
  rig.tap_pad(1);
  CHECK(rig.sink.last() == 2);
  rig.release_f1();
  (void)rig.app->invoke(core::action_id("view.mode"), static_cast<core::ActionArg>(app::PatternMode::Edit));  // Edit, following: LIVE
  rig.play(2, 1);
  rig.hold_f1();
  rig.tap_pad(2);
  CHECK(rig.sink.last() == 3);
}

TEST_CASE("patswitch app: stopped, in a count-in, or with a sink that refuses, a pick is a view pick or nothing") {
  Rig rig;
  rig.hold_f1();
  rig.tap_pad(4);  // stopped: selects the pattern for editing, as before
  CHECK(rig.edit().pattern == 5);
  CHECK(rig.sink.picks.empty());
  CHECK(rig.edit().switch_to == 0);
  rig.turn(1);
  CHECK(rig.edit().pattern == 6);
  rig.release_f1();
  // a count-in: the line waits on its start point, the pick is a view pick
  rig.position.publish(true, 1, 0, 12000);
  rig.position.publish_count_in(4);
  rig.pass(1);
  rig.hold_f1();
  rig.tap_pad(2);
  CHECK(rig.edit().pattern == 3);
  CHECK(rig.sink.picks.empty());
  rig.release_f1();
  rig.position.publish_count_in(0);
  // a sink that refuses (no sequencer behind it, ring full): the pick is not applied
  rig.play(1, 2);
  (void)rig.app->invoke(core::action_id("view.mode"), static_cast<core::ActionArg>(app::PatternMode::Browse));
  rig.pass(1);
  NEED(rig.edit().pattern == 1);
  rig.sink.accept = false;
  rig.hold_f1();
  rig.tap_pad(2);
  CHECK(rig.sink.refused == 1u);
  CHECK(rig.edit().pattern == 1);
  CHECK(rig.edit().switch_to == 0);
  rig.turn(1);
  CHECK(rig.edit().pattern == 1);
  rig.release_f1();
}

TEST_CASE("patswitch app: in Song mode every pick, the playing pattern's own pad too, sends (the song is left); a switch that never lands does not hold the view for ever") {
  Rig rig;
  rig.play(1, 4, 0);  // the song's row 0, pattern 1
  rig.hold_f1();
  rig.tap_pad(0);  // the pattern the song is on: loops it
  NEED(rig.sink.picks.size() == 1u);
  CHECK(rig.sink.last() == 1);
  rig.play(1, 5);  // the song is left: Pattern mode
  CHECK(rig.edit().switch_to == 0);
  rig.tap_pad(0);  // Pattern mode now: nothing
  CHECK(rig.sink.picks.size() == 1u);
  rig.release_f1();
  // The guard: a pick the sequencer never takes (the position goes on, the pattern does not change)
  rig.hold_f1();
  rig.tap_pad(2);
  NEED(rig.edit().switch_to == 3);
  for (unsigned s = 6; s < 6 + app::kSwitchGuardSteps; ++s) {
    CHECK(rig.edit().switch_to == 3);
    rig.play(1, s);
  }
  CHECK(rig.edit().switch_to == 0);
  rig.release_f1();
  CHECK(rig.edit().pattern == 1);  // WATCH goes back to the pattern that plays
  // A different pattern than the target starting ends the wait too.
  rig.hold_f1();
  rig.tap_pad(1);
  NEED(rig.edit().switch_to == 2);
  rig.release_f1();
  rig.play(3, 0);
  CHECK(rig.edit().switch_to == 0);
  CHECK(rig.edit().pattern == 3);
  // A stop ends it
  rig.hold_f1();
  rig.tap_pad(1);
  NEED(rig.edit().switch_to == 2);
  rig.stop();
  CHECK(rig.edit().switch_to == 0);
}

TEST_CASE("patswitch app: the pads light with the patterns while F1 is held, as PATS lights them: Front plays (stopped: the view), Back has content, Off empty; a tap-opened LENGTH chooser has none") {
  Rig rig;
  PadRole roles[48];
  NEED(rig.lights(roles));            // NOTES lights its notes by scale (OS-NOTESLED2): pad 36 (C, in the default chromatic scale) Front
  CHECK(roles[36] == PadRole::Front);
  rig.hold_f1();
  NEED(rig.lights(roles));
  CHECK(roles[0] == PadRole::Front);  // stopped: the pattern in view (1)
  CHECK(roles[1] == PadRole::Back);   // 2 holds a note
  CHECK(roles[2] == PadRole::Off);    // 3 exists and holds nothing
  CHECK(roles[8] == PadRole::Off);    // 9 does not exist
  rig.release_f1();
  NEED(rig.lights(roles));  // back to the NOTES board's lights (C major, OS-DEFSCALE: C# is outside the scale, Back)
  CHECK(roles[37] == PadRole::Back);
  rig.play(2, 3);
  rig.hold_f1();
  NEED(rig.lights(roles));
  CHECK(roles[1] == PadRole::Front);  // 2 plays
  CHECK(roles[0] == PadRole::Back);   // 1 holds a note
  rig.release_f1();
  rig.tap(Button::F2);
  NEED(rig.state().overlay == app::OverlayId::LengthChooser);
  CHECK_FALSE(rig.lights(roles));
}

TEST_CASE("patswitch app: the PATS blink ends when a live jump beat the queue, not when a switch lands and the queue was made after it") {
  Rig rig;
  rig.play(1, 4);
  rig.pad_mode(Button::F6);
  NEED(rig.state().padmode.fixed() == app::PadMode::Patterns);
  rig.tap_pad(5);  // queues pattern 6
  CHECK(rig.edit().queued_pattern == 6);
  // A live jump of the Song screen took the end of the pattern: another pattern starts, not the queued one: the blink ends at once, not at the next stop
  rig.play(2, 0, 1);
  CHECK(rig.edit().queued_pattern == 0);
  // The queue made after a switch is the newer intent: the landing (pattern 3 shows, the target) does not end its blink, the start of the queued pattern does.
  rig.pad_mode(Button::F2);  // back to NOTES
  rig.hold_f1();
  rig.tap_pad(2);  // switch to 3 (pending)
  NEED(rig.edit().switch_to == 3);
  rig.release_f1();
  rig.pad_mode(Button::F6);
  rig.tap_pad(7);  // queues pattern 8
  NEED(rig.edit().queued_pattern == 8);
  rig.play(3, 5);  // the landing of the switch
  CHECK(rig.edit().switch_to == 0);
  CHECK(rig.edit().queued_pattern == 8);
  rig.play(8, 0);  // the queue starts at the new pattern's end
  CHECK(rig.edit().queued_pattern == 0);
  // The held pick takes the PATS queue (the sequencer drops it): the blink ends with the pick
  rig.tap_pad(9);  // queues 10
  NEED(rig.edit().queued_pattern == 10);
  rig.pad_mode(Button::F2);
  rig.hold_f1();
  rig.tap_pad(1);
  CHECK(rig.edit().queued_pattern == 0);
  CHECK(rig.sink.last() == 2);
}

TEST_CASE("patswitch app: an empty or unused pattern is gone to, in PATS (queued) and with F1 held (at once): the transport runs on, the view follows with the play bar, nothing is created") {
  Rig rig;
  NEED(rig.project().pattern(9) == nullptr);
  const unsigned patterns_before = rig.project().pattern_count();
  // PATS: the pad of an unused pattern queues it (it is not refused)
  rig.play(1, 4);
  rig.pad_mode(Button::F6);
  rig.tap_pad(8);
  CHECK(rig.sink.queued == 9);
  CHECK(rig.edit().queued_pattern == 9);
  rig.play(9, 0);  // the end of pattern 1: the empty pattern plays, at the length a new one gets
  CHECK(rig.state().play.playing);
  CHECK(rig.edit().queued_pattern == 0);
  CHECK(rig.edit().pattern == 9);  // WATCH follows it
  rig.play(9, 5);
  CHECK(rig.app->view().row() == 5);  // the play bar runs through the empty grid
  rig.play(9, 31);
  CHECK(rig.app->view().row() == 31);
  // F1 held: the same at once
  rig.pad_mode(Button::F2);
  rig.hold_f1();
  rig.tap_pad(10);  // pattern 11, unused
  CHECK(rig.sink.last() == 11);
  CHECK(rig.edit().pattern == 11);
  rig.release_f1();
  rig.play(11, 6);
  CHECK(rig.state().play.playing);
  CHECK(rig.edit().pattern == 11);
  CHECK(rig.app->view().row() == 6);
  CHECK(rig.edit().switch_to == 0);
  // nothing was created, and nothing is an undo unit
  CHECK(rig.project().pattern(9) == nullptr);
  CHECK(rig.project().pattern(11) == nullptr);
  CHECK(rig.project().pattern_count() == patterns_before);
  CHECK(rig.depth() == 0u);
  // stopped: the pick selects it for editing, as today, and creates nothing
  rig.stop();
  rig.hold_f1();
  rig.tap_pad(12);  // pattern 13
  CHECK(rig.edit().pattern == 13);
  rig.release_f1();
  CHECK(rig.project().pattern(13) == nullptr);
  CHECK(rig.project().pattern_count() == patterns_before);
  CHECK(rig.depth() == 0u);
}

// E-PAT-35 (new Community OS behaviour): PAT SWITCH = Pattern end turns a pick of the held PATTERN chooser into the PATS queue.
TEST_CASE("patswitch app: PAT SWITCH = Pattern end: a pick queues for the end of the pattern (no step switch); the same pick or the playing pattern takes it back") {
  Rig rig;
  CHECK(rig.app->device_settings().get(model::device_setting::PatSwitch) == 0.0f);  // the default is Step
  CHECK(rig.app->device_settings().set(model::device_setting::PatSwitch, 1.0f) == model::Status::Ok);
  rig.play(1, 4);
  rig.hold_f1();
  NEED(rig.state().overlay == app::OverlayId::PatternChooser);
  rig.tap_pad(2);  // pattern 3
  CHECK(rig.sink.picks.empty());  // no switch at the next step
  CHECK(rig.sink.queue_calls == 1u);
  CHECK(rig.sink.queued == 3u);
  CHECK(rig.edit().queued_pattern == 3);
  CHECK(rig.edit().switch_to == 0);
  CHECK(rig.edit().pattern == 3);
  rig.play(1, 5);  // the pattern that plays goes on; the held chooser keeps the view on the pick
  CHECK(rig.edit().pattern == 3);
  CHECK(rig.edit().queued_pattern == 3);
  rig.tap_pad(2);  // the queued one again: taken back
  CHECK(rig.sink.queued == 0u);
  CHECK(rig.edit().queued_pattern == 0);
  rig.tap_pad(1);  // pattern 2 queues ...
  CHECK(rig.sink.queued == 2u);
  const unsigned calls = rig.sink.queue_calls;
  rig.tap_pad(0);  // ... and the playing pattern (Pattern mode) takes it back
  CHECK(rig.sink.queued == 0u);
  CHECK(rig.sink.queue_calls == calls + 1u);
  rig.tap_pad(0);  // nothing waits: nothing to take back, no command
  CHECK(rig.sink.queue_calls == calls + 1u);
  rig.tap_pad(2);  // pattern 3 queues, F1 is let go
  CHECK(rig.edit().queued_pattern == 3);
  rig.release_f1();
  rig.play(1, 6);  // the view follows the playing pattern again, the queue waits
  CHECK(rig.edit().pattern == 1);
  CHECK(rig.edit().queued_pattern == 3);
  rig.play(3, 0);  // the end of the pattern: it starts
  CHECK(rig.edit().queued_pattern == 0);
  CHECK(rig.edit().pattern == 3);
  CHECK(rig.sink.picks.empty());
  CHECK(rig.depth() == 0u);
  // a sink that refuses: nothing is queued, nothing changes
  rig.sink.accept = false;
  rig.hold_f1();
  rig.tap_pad(1);
  CHECK(rig.edit().queued_pattern == 0);
  CHECK(rig.edit().pattern == 3);
  rig.release_f1();
}
