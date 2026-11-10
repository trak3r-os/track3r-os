// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-ENCLEARN (docs/specs/OS-ENCLEARN.md) on the app rig: Instr + FX2 makes the Learn row under the cursor the encoder's one target (no clear gesture: the same row again keeps it, another row replaces
// it); the encoder moves it while Instr + FX1 are held, one fine step a detent, a coarse one with Shift, through the write of a relative Learn binding; letting go of either key ends it; the turn
// leaves no tap, popup or column jump behind and wins over a held Vol; a LIVE take and a STEP gesture record the move as the step FX of "Record knob moves as FX", a parameter with no FX says so.
// Host only (the app, the host fakes and the play position stub).
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/instrument.h>
#include <o3os/model/param_auto.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/seq/fx.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::PatternMode;
using app::ScreenId;
using core::ActionArg;
using core::ParamId;
using core::ParamKind;
namespace ip = model::instrument_param;
namespace seq = o3os::seq;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

ParamId inst(unsigned slot, unsigned field) { return core::param_id(ParamKind::Instrument, slot, field); }

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  audio::PlayPosition pos;
  core::ParamRing ring;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);  // after the editor: destroyed first

  // Pattern 1 (16 steps) and two sample instruments (slot 0: Low-pass, cutoff 0.30); the app is on the Pattern screen in Browse (WATCH).
  Rig() {
    model::Project& p = project();
    p.reset();
    (void)p.create_pattern(1, 16);
    (void)p.append_order(1);
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::memcpy(in.name, "TEST", 4);
    (void)p.set_instrument(0, in);
    (void)p.set_instrument(1, in);
    (void)p.set_param(inst(0, ip::Cutoff), 0.30f);
    (void)p.set_param(inst(0, ip::FilterType), 0.0f);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_play_position(&pos);
    app->attach_param_ring(&ring);
    app->run_once();
  }
  model::Project& project() { return test::arena(2).project; }
  const app::ViewState& state() const { return app->state(); }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void press(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    pass();
  }
  void release(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void tap(Button b) {
    press(b);
    release(b);
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass();
  }
  app::DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  void screen(ScreenId id) { (void)invoke("nav.screen", static_cast<ActionArg>(id)); }
  void live() { (void)invoke("view.mode", static_cast<ActionArg>(PatternMode::Edit)); }
  void play(unsigned step) {
    pos.publish(true, 1, step, 12000, audio::PlayPosition::kNoOrder, 0);
    pos.publish_count_in(0);
    pass();
  }
  // The Instrument screen with the cursor on `want` of instrument 1 (the page by F-key).
  bool focus(Button page, ParamId want) {
    tap(Button::Params);
    tap(page);
    for (int i = 0; i < 12 && app::kParamsScreen.focus_param(app->model(), app->state()) != want; ++i) {
      tap(Button::Down);
    }
    return app::kParamsScreen.focus_param(app->model(), app->state()) == want;
  }
  // Instr + FX2 on the row under the cursor.
  void assign() {
    press(Button::Instr);
    press(Button::Fx);
    release(Button::Fx);
    release(Button::Instr);
  }
  void hold() {
    press(Button::Instr);
    press(Button::Vol);
  }
  void let_go() {
    release(Button::Vol);
    release(Button::Instr);
  }
  float value(ParamId id) { return project().get_param(id); }
  bool noted(const char* text) const {
    const core::NotifyQueue& q = app->state().notify;
    for (unsigned i = 0; i < q.size(); ++i) {
      if (std::string(q.at(i).text.c_str()).find(text) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
  void expire() { pass(app::ViewState::kMessageMs + 500); }
  const model::Step& step(int track, int row) { return project().pattern(1)->steps[track][row]; }
  bool no_fx_anywhere() {
    const model::Pattern* pat = project().pattern(1);
    for (unsigned t = 0; t < model::kMaxTracks; ++t) {
      for (unsigned r = 0; r < model::kMaxPatternLength; ++r) {
        if (pat->steps[t][r].fx[0].type != model::kFxNone || pat->steps[t][r].fx[1].type != model::kFxNone) {
          return false;
        }
      }
    }
    return true;
  }
};

// A Rig with the cutoff made the encoder's target and the app back on the Pattern screen.
void target_cutoff(Rig& rig) {
  NEED(rig.focus(Button::F3, inst(0, ip::Cutoff)));
  rig.assign();
  NEED(rig.state().enc_target == inst(0, ip::Cutoff));
  rig.screen(ScreenId::Pattern);
}

}  // namespace

TEST_CASE("OS-ENCLEARN Instr + FX2 on a Learn row makes it the target; another row replaces it; the same row again keeps it and says it again; a refused row says so and keeps the target") {
  Rig rig;
  CHECK(rig.state().enc_target == ParamId::None);
  NEED(rig.focus(Button::F3, inst(0, ip::Cutoff)));
  rig.assign();
  CHECK(rig.state().enc_target == inst(0, ip::Cutoff));
  CHECK(rig.noted("ENCODER: CUTOFF"));
  CHECK(rig.noted("INS 1"));
  // the same row again: kept, said again (no clear gesture)
  rig.expire();
  CHECK_FALSE(rig.noted("ENCODER"));
  rig.assign();
  CHECK(rig.state().enc_target == inst(0, ip::Cutoff));
  CHECK(rig.noted("ENCODER: CUTOFF"));
  // another row replaces the one target
  NEED(rig.focus(Button::F3, inst(0, ip::Resonance)));
  rig.assign();
  CHECK(rig.state().enc_target == inst(0, ip::Resonance));
  // a screen with no Learn row refuses, the target stays
  rig.screen(ScreenId::Pattern);
  rig.expire();
  rig.assign();
  CHECK(rig.noted("NO TARGET FOR ENCODER"));
  CHECK(rig.state().enc_target == inst(0, ip::Resonance));
}

TEST_CASE("OS-ENCLEARN with no target the turn says NO ENCODER TARGET and moves nothing") {
  Rig rig;
  rig.hold();
  rig.turn(1);
  CHECK(rig.noted("NO ENCODER TARGET (INSTR + FX2)"));
  CHECK(rig.value(inst(0, ip::Cutoff)) == 0.30f);
  CHECK(rig.state().screen == ScreenId::Pattern);
}

TEST_CASE("OS-ENCLEARN Instr + FX1 held, a detent is one fine step of the target, Shift a coarse one; releasing either key ends it") {
  Rig rig;
  target_cutoff(rig);
  const float start = rig.value(inst(0, ip::Cutoff));
  rig.hold();
  rig.turn(1);
  const float fine = rig.value(inst(0, ip::Cutoff)) - start;
  CHECK(fine > 0.0f);
  CHECK(rig.noted("CUTOFF"));
  rig.turn(-1);
  CHECK(rig.value(inst(0, ip::Cutoff)) == doctest::Approx(start));
  rig.press(Button::Shift);
  rig.turn(1);
  const float coarse = rig.value(inst(0, ip::Cutoff)) - start;
  CHECK(coarse > fine);
  rig.release(Button::Shift);
  rig.turn(-1);
  rig.let_go();
  const float rest = rig.value(inst(0, ip::Cutoff));
  // letting go of either key ends it: no turn moves the target any more
  rig.press(Button::Instr);
  rig.turn(3);
  CHECK(rig.value(inst(0, ip::Cutoff)) == rest);
  rig.press(Button::Vol);
  rig.turn(1);
  CHECK(rig.value(inst(0, ip::Cutoff)) != rest);
  rig.release(Button::Instr);  // the other order
  const float held = rig.value(inst(0, ip::Cutoff));
  rig.turn(3);
  CHECK(rig.value(inst(0, ip::Cutoff)) == held);
  rig.release(Button::Vol);
  // the target is fixed to its row: the screen the app is on does not matter
  CHECK(rig.state().enc_target == inst(0, ip::Cutoff));
}

TEST_CASE("OS-ENCLEARN a plain Instr + Vol press does nothing; a turn leaves no popup, tap or column jump, also after a long hold, and wins over a held Vol") {
  Rig rig;
  target_cutoff(rig);
  const auto col = rig.state().pattern.column();
  const app::OverlayId none = rig.state().overlay;
  rig.press(Button::Instr);
  rig.press(Button::Vol);
  rig.pass(600);  // past every hold time
  rig.release(Button::Vol);
  rig.release(Button::Instr);
  rig.pass(100);
  CHECK(rig.state().screen == ScreenId::Pattern);
  CHECK(rig.state().overlay == none);
  CHECK(rig.state().pattern.column() == col);
  CHECK(rig.value(inst(0, ip::Cutoff)) == 0.30f);
  // a turn: the target moves, nothing else does
  rig.press(Button::Instr);
  rig.press(Button::Vol);
  rig.turn(2);
  rig.pass(600);
  rig.release(Button::Vol);
  rig.release(Button::Instr);
  rig.pass(100);
  CHECK(rig.value(inst(0, ip::Cutoff)) > 0.30f);
  CHECK(rig.state().screen == ScreenId::Pattern);
  CHECK(rig.state().overlay == none);
  CHECK(rig.state().pattern.column() == col);
  // Vol pressed first, then Instr: still the target (the held Vol would edit the column value otherwise)
  const float before = rig.value(inst(0, ip::Cutoff));
  rig.press(Button::Vol);
  rig.press(Button::Instr);
  rig.turn(1);
  CHECK(rig.value(inst(0, ip::Cutoff)) > before);
  rig.release(Button::Instr);
  rig.release(Button::Vol);
  CHECK(rig.no_fx_anywhere());  // (WATCH records nothing)
  CHECK(rig.state().screen == ScreenId::Pattern);
  CHECK(rig.state().pattern.column() == col);
}

TEST_CASE("OS-ENCLEARN a held Fx does not take the turn from the target") {
  Rig rig;
  target_cutoff(rig);
  rig.press(Button::Fx);
  rig.press(Button::Instr);
  rig.press(Button::Vol);
  rig.turn(1);
  CHECK(rig.value(inst(0, ip::Cutoff)) > 0.30f);
  CHECK(rig.no_fx_anywhere());
  rig.release(Button::Fx);
  rig.release(Button::Vol);
  rig.release(Button::Instr);
}

TEST_CASE("OS-ENCLEARN LIVE records the move as the cutoff FX at the play line of the cursor's track, one value per step; WATCH records nothing") {
  {
    Rig rig;
    target_cutoff(rig);
    rig.live();
    (void)rig.invoke("cursor.track", 2);
    rig.play(3);
    CHECK(rig.state().edit.take_open);
    const int t = rig.app->view().track();
    rig.hold();
    rig.turn(5);
    CHECK(rig.step(t, 3).fx[0].type == seq::fx::kLowPass);
    const unsigned first = rig.step(t, 3).fx[0].value;
    CHECK(first > 30u);
    CHECK(rig.step(t, 3).fx[1].type == model::kFxNone);
    rig.turn(5);
    CHECK(rig.step(t, 3).fx[0].type == seq::fx::kLowPass);
    CHECK(rig.step(t, 3).fx[0].value > first);  // the same column, a new value
    CHECK(rig.step(t, 3).fx[1].type == model::kFxNone);
    CHECK(rig.noted("REC FX: CUTOFF"));
    rig.let_go();
  }
  {
    Rig rig;  // WATCH (Browse): the parameter moves, nothing records, nothing is said about FX
    target_cutoff(rig);
    rig.play(3);
    rig.hold();
    rig.turn(5);
    CHECK(rig.value(inst(0, ip::Cutoff)) > 0.30f);
    CHECK(rig.no_fx_anywhere());
    CHECK_FALSE(rig.noted("HAS NO FX"));
    rig.let_go();
  }
}

TEST_CASE("OS-ENCLEARN STEP writes the FX on the cursor's step of the cursor's track; one hold gesture is one undo unit, the next gesture another") {
  Rig rig;
  target_cutoff(rig);
  rig.tap(Button::Rec);  // WATCH -> STEP (Edit, free)
  NEED(rig.state().edit.rec_free);
  for (int i = 0; i < 3; ++i) {
    rig.tap(Button::Down);
  }
  const int t = rig.app->view().track();
  const int row = rig.state().pattern.row();
  CHECK(row > 0);
  const unsigned d0 = rig.journal.depth();
  rig.hold();
  rig.turn(3);
  rig.turn(3);
  CHECK(rig.step(t, row).fx[0].type == seq::fx::kLowPass);
  const unsigned v1 = rig.step(t, row).fx[0].value;
  CHECK(v1 > 30u);
  rig.let_go();
  CHECK(rig.journal.depth() == d0 + 1);  // two detents' worth of moves, one unit
  // a second gesture on the next row
  rig.tap(Button::Down);
  const int row2 = rig.state().pattern.row();
  CHECK(row2 == row + 1);
  rig.hold();
  rig.turn(2);
  rig.let_go();
  CHECK(rig.step(t, row2).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.journal.depth() == d0 + 2);
  (void)rig.invoke("edit.undo");  // takes only the second gesture back
  CHECK(rig.step(t, row2).fx[0].type == model::kFxNone);
  CHECK(rig.step(t, row).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(t, row).fx[0].value == v1);
  (void)rig.invoke("edit.undo");
  CHECK(rig.no_fx_anywhere());
}

TEST_CASE("OS-ENCLEARN a parameter that cannot be recorded (VEL>, no FX and no CC letter, OS-PARAMAUTO) moves the sound and records nothing; LIVE and STEP say HAS NO FX on the first move of a gesture, WATCH says nothing") {
  {
    Rig rig;  // STEP
    NEED(rig.focus(Button::F2, inst(0, ip::VelSens)));
    rig.assign();
    rig.screen(ScreenId::Pattern);
    rig.tap(Button::Rec);
    NEED(rig.state().edit.rec_free);
    const float v0 = rig.value(inst(0, ip::VelSens));
    rig.hold();
    rig.turn(-2);
    CHECK(rig.value(inst(0, ip::VelSens)) != v0);
    CHECK(rig.noted("REC FX: VEL> HAS NO FX"));
    CHECK(rig.state().edit.enc_said);
    CHECK(rig.no_fx_anywhere());
    rig.expire();  // the queue drops only its head per pass: two messages are queued
    rig.expire();
    rig.turn(-1);  // the second move of the gesture: not said again
    CHECK_FALSE(rig.noted("HAS NO FX"));
    rig.let_go();
    CHECK_FALSE(rig.state().edit.enc_said);
    rig.hold();  // the next gesture says it again
    rig.turn(-1);
    CHECK(rig.noted("HAS NO FX"));
    rig.let_go();
    CHECK(rig.no_fx_anywhere());
  }
  {
    Rig rig;  // LIVE
    NEED(rig.focus(Button::F2, inst(0, ip::VelSens)));
    rig.assign();
    rig.screen(ScreenId::Pattern);
    rig.live();
    rig.play(2);
    const float v0 = rig.value(inst(0, ip::VelSens));
    rig.hold();
    rig.turn(-2);
    CHECK(rig.value(inst(0, ip::VelSens)) != v0);
    CHECK(rig.noted("REC FX: VEL> HAS NO FX"));
    CHECK(rig.no_fx_anywhere());
    rig.let_go();
  }
  {
    Rig rig;  // WATCH
    NEED(rig.focus(Button::F2, inst(0, ip::VelSens)));
    rig.assign();
    rig.screen(ScreenId::Pattern);
    rig.hold();
    rig.turn(-2);
    CHECK_FALSE(rig.noted("HAS NO FX"));
    CHECK(rig.no_fx_anywhere());
    rig.let_go();
  }
}

TEST_CASE("OS-PARAMAUTO the encoder move of a parameter with no FX of its own records CC A and the strip names the letter; the letter belongs to the parameter") {
  Rig rig;
  NEED(rig.focus(Button::F2, inst(0, ip::Volume)));
  rig.assign();
  rig.screen(ScreenId::Pattern);
  rig.live();
  rig.play(2);
  CHECK(rig.project().instrument(0)->midi_cc[0] == 0);  // assigning takes no letter
  rig.hold();
  rig.turn(-2);
  CHECK(rig.noted("REC FX: VOLUME -> CC A"));
  CHECK(rig.project().instrument(0)->midi_cc[0] == model::auto_code(inst(0, ip::Volume)));
  const int t = rig.app->view().track();
  CHECK(rig.step(t, 2).fx[0].type == seq::fx::kCcA);
  CHECK(rig.step(t, 2).fx[0].value < 127u);
  CHECK_FALSE(rig.noted("HAS NO FX"));
  rig.let_go();
}

TEST_CASE("OS-ENCLEARN one key let go mid-gesture swallows the turn until both are up: the held key edits nothing") {
  Rig rig;
  target_cutoff(rig);
  rig.tap(Button::Rec);  // STEP
  NEED(rig.state().edit.rec_free);
  for (int i = 0; i < 3; ++i) {
    rig.tap(Button::Down);
  }
  const int t = rig.app->view().track();
  const int row = rig.state().pattern.row();
  rig.hold();
  rig.turn(3);
  const model::Step before = rig.step(t, row);
  const float cutoff = rig.value(inst(0, ip::Cutoff));
  rig.release(Button::Instr);  // Vol stays down; the gesture's undo unit closes here
  const unsigned d1 = rig.journal.depth();
  rig.turn(2);
  rig.pass(600);
  rig.turn(-2);
  CHECK(std::memcmp(&before, &rig.step(t, row), sizeof before) == 0);
  CHECK(rig.state().pattern.row() == row);
  CHECK(rig.value(inst(0, ip::Cutoff)) == cutoff);
  CHECK(rig.journal.depth() == d1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  rig.release(Button::Vol);
  rig.pass(100);
  CHECK(std::memcmp(&before, &rig.step(t, row), sizeof before) == 0);
  CHECK(rig.state().pattern.row() == row);
}

TEST_CASE("OS-ENCLEARN an input reset with the keys held ends the gesture: its undo unit closes and the next turn is not the target's") {
  Rig rig;
  target_cutoff(rig);
  rig.tap(Button::Rec);  // STEP
  NEED(rig.state().edit.rec_free);
  rig.tap(Button::Down);
  const int t = rig.app->view().track();
  const int row = rig.state().pattern.row();
  rig.hold();
  rig.turn(3);
  CHECK(rig.state().edit.enc_take);
  const unsigned v = rig.step(t, row).fx[0].value;
  const float cutoff = rig.value(inst(0, ip::Cutoff));
  rig.app->reset_input();
  CHECK_FALSE(rig.state().edit.enc_take);
  CHECK_FALSE(rig.state().edit.enc_said);
  CHECK_FALSE(rig.state().edit.take_open);
  rig.turn(2);  // the keys count as up: not the target's turn
  CHECK(rig.step(t, row).fx[0].value == v);
  CHECK(rig.value(inst(0, ip::Cutoff)) == cutoff);
}

TEST_CASE("OS-ENCLEARN Shift pressed mid-hold, then both keys let go with no turn: no tap, popup, jump or move") {
  Rig rig;
  target_cutoff(rig);
  const auto col = rig.state().pattern.column();
  const int row = rig.state().pattern.row();
  const app::OverlayId none = rig.state().overlay;
  rig.press(Button::Instr);
  rig.press(Button::Vol);
  rig.press(Button::Shift);
  rig.release(Button::Vol);
  rig.release(Button::Instr);
  rig.release(Button::Shift);
  rig.pass(600);
  CHECK(rig.state().screen == ScreenId::Pattern);
  CHECK(rig.state().overlay == none);
  CHECK(rig.state().pattern.column() == col);
  CHECK(rig.state().pattern.row() == row);
  CHECK(rig.value(inst(0, ip::Cutoff)) == 0.30f);
  CHECK(rig.no_fx_anywhere());
}
