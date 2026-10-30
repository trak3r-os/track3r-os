// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-002d in the app: the generated parameter page (Instrument PARAMS) driven by its keys, parameter edits as undoable actions that tell
// the audio side, and MIDI Learn (target, binding, pickup, jump, relative, scopes, undo). The tables, the maths, the Learn rules and the
// Project's side are param_test.cpp and param_model_test.cpp; the pictures are the gallery's `params` states and scenarios/params.txt.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/learn_list.h>
#include <o3os/app/midi_input.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/screens/screen.h>
#include <o3os/core/input_event.h>
#include <o3os/core/param.h>
#include <o3os/midi/cc_in.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>
#include <o3os/sim/scenario.h>

#include <cmath>
#include <cstring>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::DispatchResult;
using core::ActionArg;
using core::ParamId;
using core::ParamKind;
namespace ip = model::instrument_param;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

ParamId inst(unsigned slot, unsigned field) { return core::param_id(ParamKind::Instrument, slot, field); }

ActionArg cc(unsigned channel, unsigned controller, unsigned value) {
  return static_cast<ActionArg>((channel << 16) | (controller << 8) | value);
}

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  core::ParamRing ring;

  explicit Rig(bool document = true, bool bindings = false) {
    model::Project& p = project();
    if (document) {
      if (bindings) {
        sim::fill_demo_project_with_bindings(p);
      } else {
        sim::fill_demo_project(p);
      }
      journal.begin(g_ring, sizeof g_ring);
      editor = std::make_unique<model::Editor>(p, journal);
      app->attach_document(editor.get());
      app->attach_param_ring(&ring);
    } else {
      p.reset();
    }
    app->run_once();
  }

  model::Project& project() { return test::arena(1).project; }
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
  void pad(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 127));
    pass(1);
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass(1);
  }
  // OS-INSTUI O3: Shift + a pad selects the instrument without a sound (a pad alone plays it).
  void shift_pad(unsigned index) {
    press(Button::Shift);
    pass(1);
    pad(index);
    release(Button::Shift);
    pass(1);
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  // The notification at the head of the queue (also while a modal overlay holds it back), "" when there is none.
  std::string note() const { return app->state().notify.empty() ? std::string() : std::string(app->state().notify.head().text.c_str()); }
  void clear_notes() {
    pass(1);
    pass(10000);
    pass(10000);
  }
  // The newest notification in the queue (a modal overlay holds the older ones back).
  std::string last_note() const {
    const core::NotifyQueue& q = app->state().notify;
    return q.empty() ? std::string() : std::string(q.at(q.size() - 1).text.c_str());
  }
  // What the audio side has been told since the last call, in order.
  std::vector<core::ParamChange> heard() {
    std::vector<core::ParamChange> out;
    core::ParamChange c;
    while (ring.pop(c)) {
      out.push_back(c);
    }
    return out;
  }
  float value(ParamId id) { return project().get_param(id); }
  void go_to(unsigned page, unsigned row = 0) {
    tap(Button::Params);
    invoke("param.group", static_cast<ActionArg>(page));
    for (unsigned i = 0; i < row; ++i) {
      tap(Button::Down);
    }
  }
};

}  // namespace

// ---- the page and its cursor -------------------------------------------------------------------------------------------------

TEST_CASE("params screen: the Params button reaches it, its pages and rows come from the tables, the cursor stays on the page") {
  Rig rig;
  CHECK(app::find_screen(app::screen_registry(), app::ScreenId::Instrument)->screen == &app::kParamsScreen);
  CHECK(app::kParamsScreen.pages() == ip::kGroups);
  rig.tap(Button::Params);
  CHECK(rig.app->state().screen == app::ScreenId::Instrument);
  const app::ViewState& v = rig.app->state();
  const app::Model& m = rig.app->model();
  rig.tap(Button::F2);
  CHECK(app::kParamsScreen.count(m, v) == 6);  // VOICE (PLAY ENGINE moved to the source page, O5; VEL> closes it, OS-INSTUI S9)
  CHECK(app::kParamsScreen.focus_param(m, v) == inst(0, ip::Volume));
  for (int i = 0; i < 10; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(app::kParamsScreen.position(v) == 5);  // clamped to the page
  CHECK(app::kParamsScreen.focus_param(m, v) == inst(0, ip::VelSens));
  rig.tap(Button::Up);
  CHECK(app::kParamsScreen.focus_param(m, v) == inst(0, ip::Glide));
  rig.tap(Button::Up);
  CHECK(app::kParamsScreen.focus_param(m, v) == inst(0, ip::FineTune));
  // F2..F5 are the pages (F1 is the source page, OS-INSTUI), and the page starts at its first row.
  rig.tap(Button::F3);
  CHECK(v.page[app::screen_index(app::ScreenId::Instrument)] == ip::GroupFilter);
  CHECK(app::kParamsScreen.count(m, v) == 4);
  CHECK(app::kParamsScreen.focus_param(m, v) == inst(0, ip::FilterEnable));
  rig.tap(Button::F4);
  CHECK(app::kParamsScreen.count(m, v) == 6);  // MOD: SOURCE and the envelope's five rows (an LFO shows SOURCE and its three)
  CHECK(app::kParamsScreen.focus_param(m, v) == inst(0, ip::mod(0, ip::ModSource)));  // OS-INSTUI S6: the MOD page starts on SOURCE
  // F4 again on the MOD page steps the six envelopes, wrapping (OS-INSTUI-ARROWS: Left and Right edit the row now, the pages are the F-keys).
  const auto page = [&] { return v.page[app::screen_index(app::ScreenId::Instrument)]; };
  rig.tap(Button::F4);
  rig.tap(Button::F4);
  CHECK(v.params.env == 2);
  CHECK(app::kParamsScreen.focus_param(m, v) == inst(0, ip::mod(2, ip::ModSource)));
  for (int i = 0; i < 3; ++i) {
    rig.tap(Button::F4);
  }
  CHECK(v.params.env == 5);
  CHECK(page() == ip::GroupEnvelope);
  rig.tap(Button::Down);
  CHECK(app::kParamsScreen.focus_param(m, v) == inst(0, ip::envelope(5, ip::EnvAmount)));
  rig.tap(Button::F4);
  CHECK(v.params.env == 0);  // past the last envelope: round to the first, on the same page and row
  CHECK(page() == ip::GroupEnvelope);
  CHECK(app::kParamsScreen.position(v) == 1);
  rig.tap(Button::F5);
  CHECK(page() == ip::GroupSends);
  CHECK(app::kParamsScreen.count(m, v) == 4);  // SENDS
  rig.tap(Button::F5);
  CHECK(page() == ip::GroupSends);  // a group of one window: the same key again does nothing
  // Pressing Params again goes to F1, the first page (it no longer cycles: UI map rule 3): the source page of a sample instrument (OS-INSTUI S3).
  rig.tap(Button::Params);
  CHECK(page() == ip::GroupSource);
  rig.tap(Button::F3);
  rig.tap(Button::Params);
  CHECK(page() == ip::GroupSource);
  // The instrument is the one Shift + a pad selected.
  rig.tap(Button::F3);
  rig.shift_pad(3);
  CHECK(app::kParamsScreen.focus_param(m, v) == core::param_id(ParamKind::Instrument, 3, ip::FilterEnable));
  // OS-SHIFTROW: Shift + F1..F5 are the Shift row's (NAME .. SAVE wait for S5 / S7: they say so), and never the plain page key.
  rig.tap(Button::F4);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F5);
  CHECK(app::kParamsScreen.position(v) == 0);
  CHECK(v.page[app::screen_index(app::ScreenId::Instrument)] == ip::GroupEnvelope);  // (F4 chose MOD; Shift + F5 did not choose SENDS)
  rig.tap(Button::F5);  // with Shift still down this is Shift + F5 again
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.app->failed_frames() == 0u);
}

// ---- OS-FU5: the PLAY ENGINE row ---------------------------------------------------------------------------------------------------

namespace {

unsigned first_sample_slot(model::Project& p) {
  for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
    if (p.instrument(i) != nullptr && p.instrument(i)->kind == model::InstrumentKind::Sample) {
      return i;
    }
  }
  return model::kMaxInstruments;
}

// The sample instrument `slot` at `rate` Hz, as one unit of the editor.
void set_rate(Rig& rig, unsigned slot, std::uint32_t rate) {
  model::Instrument in = *rig.project().instrument(slot);
  in.sample_rate = rate;
  NEED(rig.editor->set_instrument(slot, in) == model::Status::Ok);
}

}  // namespace

TEST_CASE("params screen: PLAY ENGINE is a row of the VOICE page the encoder edits, one undo unit that rebinds, and it is not the F1 module ENGINE") {
  Rig rig;
  const unsigned slot = first_sample_slot(rig.project());
  NEED(slot < model::kMaxInstruments);
  set_rate(rig, slot, 8287);
  rig.shift_pad(slot);
  rig.go_to(ip::GroupSource, 9);  // (the last row of the source page, whatever the mode: O5; STEREO since OS-STEREO step 2, PLAY ENGINE is the one above it)
  rig.tap(Button::Up);
  const ParamId id = inst(slot, ip::PlayEngine);
  CHECK(app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()) == id);
  const core::ParamDesc* d = model::param_desc(id);
  NEED(d != nullptr);
  CHECK(std::strcmp(d->name, "PLAY ENGINE") == 0);
  CHECK((d->flags & core::kParamLearnable) == 0);  // no MIDI CC drives it: the rules live in the action
  CHECK(rig.value(id) == 0.0f);                    // Auto
  rig.heard();
  rig.turn(1);                                      // Modern
  CHECK(rig.value(id) == 1.0f);
  CHECK(rig.project().instrument(slot)->engine == model::kInstEngineModern);
  rig.turn(1);                                      // Amiga (the sample is at 8,287 Hz: playable)
  CHECK(rig.value(id) == 2.0f);
  CHECK(rig.heard().empty());                       // not a parameter the audio table knows: the bank rebinds the slot instead
  rig.turn(1);
  CHECK(rig.value(id) == 3.0f);
  rig.turn(10);                                     // stops at the end
  CHECK(rig.value(id) == 4.0f);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // one turn, one unit
  CHECK(rig.value(id) == 3.0f);
  rig.invoke("param.reset");
  CHECK(rig.value(id) == 0.0f);
}

TEST_CASE("params screen: a sample too fast for Paula refuses AMIGA with CONVERT TO AMIGA FIRST; the encoder steps past it, the action does not") {
  Rig rig;
  const unsigned slot = first_sample_slot(rig.project());
  NEED(slot < model::kMaxInstruments);
  rig.shift_pad(slot);
  rig.go_to(ip::GroupSource, 9);  // (the last row of the source page, whatever the mode: O5; STEREO since OS-STEREO step 2, PLAY ENGINE is the one above it)
  rig.tap(Button::Up);
  const auto engine = [&]() { return rig.project().instrument(slot)->engine; };
  for (const std::uint32_t rate : {0u, 44100u, 31389u}) {  // 0 means the 44,100 Hz default; 31,389 Hz is period 112
    set_rate(rig, slot, rate);
    rig.invoke("param.reset");
    rig.clear_notes();
    rig.turn(1);                                          // Modern
    CHECK(engine() == model::kInstEngineModern);
    rig.turn(1);                                          // Amiga is refused: the step goes on to 12-bit and says why
    CHECK(engine() == model::kInstEngineBits12);
    CHECK(rig.last_note() == "CONVERT TO AMIGA FIRST");
    rig.clear_notes();
    rig.turn(-1);                                         // 12-bit down: past Amiga to Modern
    CHECK(engine() == model::kInstEngineModern);
    CHECK(rig.last_note() == "CONVERT TO AMIGA FIRST");
    rig.clear_notes();
    CHECK(rig.invoke("instrument.engine", model::kInstEngineAmiga) == DispatchResult::Unchanged);  // a plain choice: refused
    CHECK(engine() == model::kInstEngineModern);
    CHECK(rig.last_note() == "CONVERT TO AMIGA FIRST");
  }
  set_rate(rig, slot, 31388);  // period 113: the lowest Paula plays
  rig.invoke("param.reset");
  CHECK(rig.invoke("instrument.engine", model::kInstEngineAmiga) == DispatchResult::Changed);
  CHECK(engine() == model::kInstEngineAmiga);
}

TEST_CASE("params screen: the Navigator lists it as a screen that exists and opens it; F6 and F7 have no function and say nothing") {
  Rig rig;
  rig.invoke("navigator.open");
  rig.pass(1);
  CHECK(rig.app->state().overlay == app::OverlayId::Navigator);
  rig.tap(Button::Right);
  rig.tap(Button::Right);
  CHECK(rig.app->state().navigator == 2);
  rig.tap(Button::F1);  // open the chosen cell
  CHECK(rig.app->state().screen == app::ScreenId::Instrument);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  rig.clear_notes();
  rig.tap(Button::F6);
  CHECK(rig.note().empty());  // a key whose label is "--" has no function and says nothing (OS-CFGWIRE)
  rig.invoke("param.group", 1);
  CHECK(rig.app->state().page[app::screen_index(app::ScreenId::Instrument)] == ip::GroupFilter);
  CHECK(rig.invoke("param.group", 5) == DispatchResult::Unchanged);  // a page that is not there
  CHECK(rig.invoke("param.group", -1) == DispatchResult::Unchanged);
  CHECK(rig.invoke("param.group", 1) == DispatchResult::Unchanged);  // the page it is on
}

TEST_CASE("params screen: every page of every envelope draws, with and without a project, in every theme") {
  for (const bool document : {true, false}) {
    Rig rig(document);
    rig.tap(Button::Params);
    for (unsigned t = 0; t < ui::kThemeCount; ++t) {
      rig.app->set_theme(ui::theme_id_at(t));
      for (unsigned page = 0; page < ip::kGroups; ++page) {
        rig.invoke("param.group", static_cast<ActionArg>(page));
        for (int env = 0; env < (page == ip::GroupEnvelope ? 6 : 1); ++env) {
          rig.tap(Button::Down);
          if (page == ip::GroupEnvelope) {
            rig.tap(Button::F4);  // F4 again: the next envelope
          }
          rig.pass(1);
        }
      }
    }
    CHECK(rig.app->failed_frames() == 0u);
    CHECK(rig.app->frames() >= 9u * ui::kThemeCount);  // (ten windows a theme: the five pages and the five further envelopes of MOD by F4 again; a step that changes nothing commits no frame)
  }
}

// ---- editing -----------------------------------------------------------------------------------------------------------------

TEST_CASE("params edit: the encoder changes the row under the cursor, F8 undoes it, Shift + F8 redoes it, the audio side hears each") {
  Rig rig;
  rig.go_to(ip::GroupVoice, 0);  // VOLUME of instrument 1
  CHECK(rig.value(inst(0, ip::Volume)) == 100.0f);
  rig.turn(-5);
  CHECK(rig.value(inst(0, ip::Volume)) == 95.0f);
  std::vector<core::ParamChange> heard = rig.heard();
  NEED(heard.size() == 1u);
  CHECK(heard[0].id == inst(0, ip::Volume));
  CHECK(heard[0].value == 95.0f);
  CHECK(heard[0].ramp_samples == 0u);  // the engine's own smoothing
  CHECK(rig.editor->journal().depth() == 1u);
  rig.pass(1600);  // (OS-UNDOGRP: a second click within 1500 ms joins the first unit; after a pause it is a unit of its own)
  rig.turn(100);  // clamped at the top: 100
  CHECK(rig.value(inst(0, ip::Volume)) == 100.0f);
  rig.turn(1);  // already there: nothing changes, nothing is sent, no unit
  CHECK(rig.heard().size() == 1u);
  CHECK(rig.editor->journal().depth() == 2u);
  // Undo and redo go through F8 and Shift + F8 on this page too, and the audio side hears the values they restore.
  rig.tap(Button::F8);
  CHECK(rig.value(inst(0, ip::Volume)) == 95.0f);
  heard = rig.heard();
  NEED(heard.size() == 1u);
  CHECK((heard[0].id == inst(0, ip::Volume) && heard[0].value == 95.0f));
  rig.tap(Button::F8);
  CHECK(rig.value(inst(0, ip::Volume)) == 100.0f);
  heard = rig.heard();
  NEED(heard.size() == 1u);
  CHECK(heard[0].value == 100.0f);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F8);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.value(inst(0, ip::Volume)) == 95.0f);
  heard = rig.heard();
  NEED(heard.size() == 1u);
  CHECK(heard[0].value == 95.0f);
  CHECK(rig.app->session().param_resyncs == 0u);
  // Shift + encoder is the coarse step: the descriptor's coarse (10 detents for a level).
  rig.shift_turn(-1);
  CHECK(rig.value(inst(0, ip::Volume)) == 85.0f);
  // Delete puts the row back to its default, as one undo unit.
  rig.tap(Button::Delete);
  CHECK(rig.value(inst(0, ip::Volume)) == 50.0f);  // our default volume
  rig.tap(Button::F8);
  CHECK(rig.value(inst(0, ip::Volume)) == 85.0f);
  // The row below: PANNING shows from the centre; the encoder steps it by one.
  rig.tap(Button::Down);
  const float pan = rig.value(inst(0, ip::Panning));
  rig.turn(2);
  CHECK(rig.value(inst(0, ip::Panning)) == pan + 2.0f);
  CHECK(rig.value(inst(0, ip::Volume)) == 85.0f);
  CHECK(rig.project().check_invariants());
}

TEST_CASE("params edit: each kind of storage is edited as the descriptor says: whole numbers, floats, switches, names, the envelope's scale") {
  Rig rig;
  // FILTER page: the switch, the type, the cutoff and resonance as fractions of their ranges (1 % a detent).
  rig.go_to(ip::GroupFilter, 0);
  CHECK(rig.value(inst(0, ip::FilterEnable)) == 1.0f);
  rig.turn(-1);
  CHECK(rig.value(inst(0, ip::FilterEnable)) == 0.0f);
  rig.turn(5);
  CHECK(rig.value(inst(0, ip::FilterEnable)) == 1.0f);  // an enum stops at its last name
  rig.tap(Button::Down);
  rig.turn(1);
  CHECK(rig.value(inst(0, ip::FilterType)) == 1.0f);
  rig.tap(Button::Down);
  const float cutoff = rig.value(inst(0, ip::Cutoff));
  rig.turn(3);
  CHECK(rig.value(inst(0, ip::Cutoff)) == doctest::Approx(cutoff + 0.03f).epsilon(1.0e-5));
  rig.heard();
  rig.shift_turn(1);  // coarse: ten detents of 1 %
  CHECK(rig.value(inst(0, ip::Cutoff)) == doctest::Approx(cutoff + 0.13f).epsilon(1.0e-5));
  std::vector<core::ParamChange> heard = rig.heard();
  NEED(heard.size() == 1u);
  CHECK(heard[0].value == rig.value(inst(0, ip::Cutoff)));  // what is sent is what is stored
  rig.tap(Button::Down);
  rig.turn(10);
  CHECK(rig.value(inst(0, ip::Resonance)) == doctest::Approx(0.43f).epsilon(1.0e-5));  // 10 % of 4.3
  // ENV page, the attack of the volume envelope: 10 ms, then 10 ms steps up to a second.
  rig.invoke("param.group", ip::GroupEnvelope);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.value(inst(0, ip::envelope(0, ip::EnvAttack))) == 10.0f);
  rig.turn(1);
  CHECK(rig.value(inst(0, ip::envelope(0, ip::EnvAttack))) == 20.0f);
  rig.turn(98);  // 100 steps in all: a second
  CHECK(rig.value(inst(0, ip::envelope(0, ip::EnvAttack))) == 1000.0f);
  rig.turn(1);  // and from there 100 ms a step
  CHECK(rig.value(inst(0, ip::envelope(0, ip::EnvAttack))) == 1100.0f);
  rig.turn(10000);
  CHECK(rig.value(inst(0, ip::envelope(0, ip::EnvAttack))) == 10000.0f);
  // Another envelope has its own values.
  rig.tap(Button::F4);  // F4 again on the MOD page: the next envelope
  rig.turn(1);
  CHECK(rig.value(inst(0, ip::envelope(1, ip::EnvAttack))) == 3100.0f);  // from our 3 s default, a step of 100 ms
  CHECK(rig.value(inst(0, ip::envelope(0, ip::EnvAttack))) == 10000.0f);
  // The sends page: a bit depth moves in whole bits and stops at 4.
  rig.invoke("param.group", ip::GroupSends);
  for (int i = 0; i < 3; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.value(inst(0, ip::BitDepth)) == 16.0f);
  rig.shift_turn(-1);
  CHECK(rig.value(inst(0, ip::BitDepth)) == 12.0f);  // coarse is 4 bits
  rig.turn(-100);
  CHECK(rig.value(inst(0, ip::BitDepth)) == 4.0f);
  CHECK(rig.project().check_invariants());
}

TEST_CASE("params edit: a free slot, no project and a full ring are each said or counted, and nothing is written wrongly") {
  {
    Rig rig;
    rig.go_to(ip::GroupVoice, 0);
    rig.shift_pad(15);  // instrument 16: the demo has ten
    rig.clear_notes();
    rig.heard();
    rig.turn(3);
    CHECK(rig.note() == "");  // OS-INSTUI S5: the free slot is the type chooser, which has no rows to edit
    CHECK(rig.heard().empty());
    CHECK(rig.editor->journal().depth() == 0u);
    CHECK(rig.project().instrument(15) == nullptr);
  }
  {
    Rig rig(false);
    rig.go_to(ip::GroupVoice, 0);
    CHECK(rig.invoke("param.edit", 1) == DispatchResult::Changed);  // it says why (a notification), it is not Disabled: a device setting would work
    CHECK(rig.note() == "NO PROJECT OPEN");
    rig.pass(1);
    CHECK(rig.invoke("learn.clear") == DispatchResult::Changed);  // "nothing bound": said
    CHECK(rig.app->failed_frames() == 0u);
  }
  {
    // A ring that is full: the edit is made, the push is refused and counted.
    Rig rig;
    rig.go_to(ip::GroupVoice, 0);
    while (rig.ring.push(core::ParamChange{core::ParamId::None, 0.0f, 0})) {
    }
    CHECK(rig.app->session().param_drops == 0u);
    rig.turn(-1);
    CHECK(rig.value(inst(0, ip::Volume)) == 99.0f);
    CHECK(rig.app->session().param_drops == 1u);
    rig.turn(-1);
    CHECK(rig.app->session().param_drops == 2u);
  }
}

TEST_CASE("params edit: undoing a whole instrument that was replaced is counted, for the engine to re-read") {
  Rig rig;
  model::Instrument in = *rig.project().instrument(0);
  in.sampler.tune = 5;
  CHECK(rig.editor->set_instrument(0, in) == model::Status::Ok);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.app->session().param_resyncs == 1u);
  // OS-H3B: the engine is also sent the sound parameters of the instrument the unit replaced (the list the Editor keeps holds 16 and says it is incomplete), each as the project now holds it
  const std::vector<core::ParamChange> heard = rig.heard();
  CHECK(!heard.empty());
  for (const core::ParamChange& c : heard) {
    CHECK(c.value == rig.value(c.id));
  }
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(rig.app->session().param_resyncs == 2u);
}

TEST_CASE("params edit: a macro replays parameter edits as one unit") {
  Rig rig;
  rig.go_to(ip::GroupVoice, 0);
  CHECK(rig.invoke("macro.record") == DispatchResult::Changed);
  rig.turn(-3);
  rig.tap(Button::Down);
  rig.turn(4);
  CHECK(rig.invoke("macro.record") == DispatchResult::Changed);
  CHECK(rig.value(inst(0, ip::Volume)) == 97.0f);
  const float pan = rig.value(inst(0, ip::Panning));
  rig.tap(Button::Up);  // the cursor back where the recording began: a macro replays keys, not rows
  rig.heard();
  CHECK(rig.invoke("macro.play") == DispatchResult::Changed);
  CHECK(rig.value(inst(0, ip::Volume)) == 94.0f);
  CHECK(rig.value(inst(0, ip::Panning)) == pan + 4.0f);
  CHECK(rig.heard().size() == 2u);
  const unsigned depth = rig.editor->journal().depth();
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // the whole macro, in one step
  CHECK(rig.editor->journal().depth() == depth - 1u);
  CHECK(rig.value(inst(0, ip::Volume)) == 97.0f);
  CHECK(rig.value(inst(0, ip::Panning)) == pan);
}

// ---- MIDI Learn --------------------------------------------------------------------------------------------------------------

TEST_CASE("learn: the overlay's target is the row under the cursor, and a controller binds to it, undoably") {
  Rig rig;
  rig.go_to(ip::GroupFilter, 2);  // CUTOFF
  const app::Model& m = rig.app->model();
  CHECK(app::kParamsScreen.focus_param(m, rig.app->state()) == inst(0, ip::Cutoff));
  CHECK(rig.project().settings().learn_count() == 0u);
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.app->learn());
  // While it is up, a message binds; it does not move anything.
  const float cutoff = rig.value(inst(0, ip::Cutoff));
  CHECK(rig.invoke("midi.cc", cc(1, 74, 64)) == DispatchResult::Changed);
  CHECK(rig.value(inst(0, ip::Cutoff)) == cutoff);
  CHECK(rig.project().settings().learn_count() == 1u);
  const core::LearnBinding& b = rig.project().settings().learn(0);
  CHECK(b.param == inst(0, ip::Cutoff));
  CHECK(b.cc == 74);
  CHECK(b.channel == 1);
  CHECK(b.mode == static_cast<std::uint8_t>(core::LearnMode::Pickup));  // the maintainer's default
  CHECK(rig.last_note() == "CC 74 > CUTOFF");
  CHECK(rig.heard().empty());  // a binding is not a parameter change
  // F1 unbinds the target, F2 cycles how its controller moves it.
  rig.tap(Button::F2);
  CHECK(rig.project().settings().learn(0).mode == static_cast<std::uint8_t>(core::LearnMode::Jump));
  rig.tap(Button::F2);
  CHECK(rig.project().settings().learn(0).mode == static_cast<std::uint8_t>(core::LearnMode::Relative));
  rig.tap(Button::F2);
  CHECK(rig.project().settings().learn(0).mode == static_cast<std::uint8_t>(core::LearnMode::Pickup));
  rig.tap(Button::F1);
  CHECK(rig.project().settings().learn_count() == 0u);
  CHECK(rig.last_note() == "UNBOUND CUTOFF");
  rig.tap(Button::F1);  // nothing bound any more: said
  CHECK(rig.last_note() == "NOTHING BOUND TO THIS");
  // All of it is in the undo journal: the unbind, the three mode changes, the binding.
  CHECK(rig.editor->journal().depth() == 5u);
  rig.invoke("learn.toggle");  // close Learn: F8 is Back there, the action is the same everywhere
  rig.pass(1);
  CHECK_FALSE(rig.app->learn());
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.project().settings().learn_count() == 1u);
  for (int i = 0; i < 3; ++i) {
    CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  }
  CHECK(rig.project().settings().learn(0).mode == static_cast<std::uint8_t>(core::LearnMode::Pickup));
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.project().settings().learn_count() == 0u);
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(rig.project().settings().learn(0).cc == 74);
  CHECK(rig.value(inst(0, ip::Cutoff)) == cutoff);  // the bindings never touched the value
}

TEST_CASE("learn: a controller binds one parameter at a time, and the target must be a parameter") {
  Rig rig;
  rig.go_to(ip::GroupFilter, 2);
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.invoke("midi.cc", cc(0, 74, 0)) == DispatchResult::Changed);  // any channel
  rig.tap(Button::Down);                                                    // RESONANCE: the Learn overlay does not take the arrows...
  CHECK(app::kParamsScreen.position(rig.app->state()) == 2);                // ...they are swallowed with the overlay open
  rig.invoke("learn.toggle");
  rig.pass(1);
  rig.tap(Button::Down);
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()) == inst(0, ip::Resonance));
  CHECK(rig.invoke("midi.cc", cc(2, 74, 0)) == DispatchResult::Changed);  // the same controller to the next parameter
  CHECK(rig.project().settings().learn_count() == 1u);                    // one controller moves one parameter: cutoff lost it
  CHECK(rig.project().settings().learn(0).param == inst(0, ip::Resonance));
  CHECK(rig.project().settings().learn(0).channel == 2);
  CHECK(rig.project().settings().check_invariants());
  // Learn over a screen that has no parameters has no target.
  rig.invoke("learn.toggle");  // closed
  rig.pass(1);
  rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Pattern));
  rig.invoke("learn.toggle");  // open over the Pattern screen
  rig.pass(1);
  CHECK(rig.app->learn());
  CHECK(rig.invoke("midi.cc", cc(1, 9, 9)) == DispatchResult::Changed);
  CHECK(rig.last_note() == "NO TARGET TO LEARN");
  CHECK(rig.project().settings().learn_count() == 1u);
  // Without a project there is nowhere to keep a project binding.
  Rig bare(false);
  bare.go_to(ip::GroupFilter, 2);
  bare.invoke("learn.toggle");
  bare.pass(1);
  bare.clear_notes();
  CHECK(bare.invoke("midi.cc", cc(1, 74, 5)) == DispatchResult::Changed);
  CHECK(bare.last_note() == "NO PROJECT OPEN");
}

TEST_CASE("learn: pickup waits for the knob, jump follows it at once, relative counts, and the screen's own edit makes the knob catch up again") {
  Rig rig(true, true);  // the demo with CC 74 on instrument 1's cutoff (pickup, any channel) and CC 71 on its resonance (jump, channel 1)
  CHECK(rig.project().settings().learn_count() == 2u);
  const ParamId cutoff = inst(0, ip::Cutoff);
  CHECK(rig.value(cutoff) == doctest::Approx(0.30f));  // 38.1 on the knob
  rig.heard();
  // Below, far, near: only the message within one of the parameter moves it.
  CHECK(rig.invoke("midi.cc", cc(1, 74, 100)) == DispatchResult::Unchanged);
  CHECK(rig.invoke("midi.cc", cc(1, 74, 60)) == DispatchResult::Unchanged);
  CHECK(rig.invoke("midi.cc", cc(1, 74, 40)) == DispatchResult::Unchanged);
  CHECK(rig.value(cutoff) == doctest::Approx(0.30f));
  CHECK(rig.heard().empty());
  CHECK(rig.invoke("midi.cc", cc(1, 74, 38)) == DispatchResult::Changed);
  CHECK(rig.value(cutoff) == doctest::Approx(38.0f / 127.0f));
  std::vector<core::ParamChange> heard = rig.heard();
  NEED(heard.size() == 1u);
  CHECK(heard[0].id == cutoff);
  CHECK(heard[0].value == rig.value(cutoff));
  // Now it follows, wherever the knob goes, and the undo journal has a unit for each (a sweep is as many units as messages).
  CHECK(rig.invoke("midi.cc", cc(1, 74, 127)) == DispatchResult::Changed);
  CHECK(rig.value(cutoff) == 1.0f);
  CHECK(rig.invoke("midi.cc", cc(1, 74, 0)) == DispatchResult::Changed);
  CHECK(rig.value(cutoff) == 0.0f);
  CHECK(rig.heard().size() == 2u);
  // A message on another channel than the binding's is not for it (CC 71 is on channel 1); one of channel 0 is for every binding.
  CHECK(rig.invoke("midi.cc", cc(2, 71, 90)) == DispatchResult::Unchanged);
  CHECK(rig.invoke("midi.cc", cc(1, 71, 90)) == DispatchResult::Changed);  // jump
  CHECK(rig.value(inst(0, ip::Resonance)) == doctest::Approx(90.0f / 127.0f * 4.3f).epsilon(1.0e-4));
  // The encoder moves the cutoff from the page: the knob lost it, and must reach it again.
  rig.go_to(ip::GroupFilter, 2);
  rig.turn(30);
  CHECK(rig.value(cutoff) == doctest::Approx(0.30f));
  rig.heard();
  CHECK(rig.invoke("midi.cc", cc(1, 74, 5)) == DispatchResult::Unchanged);
  CHECK(rig.invoke("midi.cc", cc(1, 74, 127)) == DispatchResult::Changed);  // 5 -> 127 went through 38
  CHECK(rig.value(cutoff) == 1.0f);
  // Relative: 1..63 up, 65..127 down.
  rig.invoke("learn.toggle");
  rig.pass(1);
  rig.tap(Button::F2);  // pickup -> jump
  rig.tap(Button::F2);  // jump -> relative
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.project().settings().learn(0).mode == static_cast<std::uint8_t>(core::LearnMode::Relative));
  rig.turn(-50);
  CHECK(rig.value(cutoff) == doctest::Approx(0.5f).epsilon(1.0e-4));
  CHECK(rig.invoke("midi.cc", cc(1, 74, 3)) == DispatchResult::Changed);
  CHECK(rig.value(cutoff) == doctest::Approx(0.53f).epsilon(1.0e-4));
  CHECK(rig.invoke("midi.cc", cc(1, 74, 127)) == DispatchResult::Changed);
  CHECK(rig.value(cutoff) == doctest::Approx(0.52f).epsilon(1.0e-4));
  CHECK(rig.invoke("midi.cc", cc(1, 74, 64)) == DispatchResult::Unchanged);
  CHECK(rig.invoke("midi.cc", cc(1, 74, 0)) == DispatchResult::Unchanged);
  // A message that is not a message: refused whole.
  CHECK(rig.invoke("midi.cc", cc(17, 74, 3)) == DispatchResult::Unchanged);
  CHECK(rig.invoke("midi.cc", cc(1, 128, 3)) == DispatchResult::Unchanged);
  CHECK(rig.invoke("midi.cc", static_cast<ActionArg>(0x01000000 | cc(1, 74, 3))) == DispatchResult::Unchanged);
  CHECK(rig.project().check_invariants());
}

TEST_CASE("learn: an edit made on the screen is not crossed by a knob last seen before it, and a loaded project makes every knob catch up again") {
  Rig rig(true, true);  // CC 74 (pickup, any channel) on instrument 1's cutoff
  const ParamId cutoff = inst(0, ip::Cutoff);
  const auto knob = [&](unsigned value) { return rig.invoke("midi.cc", cc(1, 74, value)); };
  // The knob at 11 has caught the cutoff (a step away at 12) and drives it.
  CHECK(rig.editor->set_param(cutoff, 12.0f / 127.0f) == model::Status::Ok);
  CHECK(knob(11) == DispatchResult::Changed);
  CHECK(rig.value(cutoff) == doctest::Approx(11.0f / 127.0f));
  // The encoder (here: the editor, as a screen edit is) takes it to 76; the knob, still at 11, is turned up to 110. It went from 11 past 76,
  // but the 11 is from before the edit, so it has not "passed" the parameter: nothing moves.
  CHECK(rig.editor->set_param(cutoff, 76.0f / 127.0f) == model::Status::Ok);
  rig.heard();
  CHECK(knob(110) == DispatchResult::Unchanged);
  CHECK(rig.value(cutoff) == doctest::Approx(76.0f / 127.0f));
  CHECK(rig.heard().empty());
  // Turned back down through 76 it does catch it, and from then on drives it.
  CHECK(knob(70) == DispatchResult::Changed);
  CHECK(rig.value(cutoff) == doctest::Approx(70.0f / 127.0f));
  CHECK(knob(120) == DispatchResult::Changed);
  // A project loaded or imported behind the editor's back: the bindings may be others, so every knob has to catch its parameter again.
  rig.app->session().reset_history();
  CHECK(knob(5) == DispatchResult::Unchanged);
  CHECK(rig.value(cutoff) == doctest::Approx(120.0f / 127.0f));
}

TEST_CASE("learn: a macro does not record controller messages, only the edits the keys make") {
  Rig rig(true, true);  // CC 74 (jump after one catch) on instrument 1's cutoff
  rig.go_to(ip::GroupVoice, 0);
  CHECK(rig.invoke("macro.record") == DispatchResult::Changed);
  CHECK(rig.invoke("midi.cc", cc(1, 74, 38)) == DispatchResult::Changed);  // caught (0.30 is 38.1 on the knob) and written
  CHECK(rig.app->session().macro.count == 0u);
  rig.turn(-3);  // a key edit is recorded
  CHECK(rig.app->session().macro.count == 1u);
  CHECK(rig.invoke("macro.record") == DispatchResult::Changed);
  CHECK(rig.app->session().macro.count == 1u);
}

TEST_CASE("learn: a follow binding acts on the selected instrument, and a binding of the device's scope is kept outside the project") {
  Rig rig;
  // A follow binding: the volume of whichever instrument is selected.
  CHECK(rig.editor->set_learn_slot(0, core::LearnBinding{inst(0, ip::Volume), 7, 0, static_cast<std::uint8_t>(core::LearnMode::Jump), core::kLearnFollow}) ==
        model::Status::Ok);
  CHECK(rig.invoke("midi.cc", cc(1, 7, 64)) == DispatchResult::Changed);  // instrument 1
  CHECK(rig.value(inst(0, ip::Volume)) == doctest::Approx(64.0f / 127.0f * 100.0f).epsilon(0.01));
  // (A pad no longer selects the instrument in every column: on the Pattern screen it is the Instr column's, in Edit, or previews in Browse,
  // OS-009. The action the pad used to run is still the way to select one.)
  CHECK(rig.invoke("instrument.select", 5) == DispatchResult::Changed);  // instrument 5
  CHECK(rig.invoke("midi.cc", cc(1, 7, 127)) == DispatchResult::Changed);
  CHECK(rig.value(inst(4, ip::Volume)) == 100.0f);
  CHECK(rig.value(inst(0, ip::Volume)) == doctest::Approx(50.0f).epsilon(0.02));  // the first one was left alone
  CHECK(rig.invoke("instrument.select", 31) == DispatchResult::Changed);  // a free slot: the message finds no parameter
  CHECK(rig.invoke("midi.cc", cc(1, 7, 3)) == DispatchResult::Unchanged);

  // The device's store (the mixer and the controller's own settings): a track's volume, bound in the device block, and a setting of the
  // device, which needs no project at all.
  const core::LearnBinding mixer{core::param_id(ParamKind::Track, 2, model::track_param::Volume), 20, 0, static_cast<std::uint8_t>(core::LearnMode::Jump), 0};
  CHECK(rig.app->device_settings().set_learn(0, mixer) == model::Status::Ok);
  CHECK(rig.app->device_settings().set_learn(1, core::LearnBinding{core::param_id(ParamKind::Device, 0, model::device_setting::MetronomeVolume), 21, 0,
                                                              static_cast<std::uint8_t>(core::LearnMode::Jump), 0}) == model::Status::Ok);
  rig.heard();
  const unsigned depth = rig.editor->journal().depth();
  CHECK(rig.invoke("midi.cc", cc(1, 20, 127)) == DispatchResult::Changed);
  CHECK(rig.project().track(2)->volume == 100);
  CHECK(rig.editor->journal().depth() == depth);  // a track's volume is the project's, but the Mixer has no history (OS-UNDOGRP D10): no unit
  CHECK(rig.invoke("midi.cc", cc(1, 21, 0)) == DispatchResult::Changed);
  CHECK(rig.app->device_settings().get(model::device_setting::MetronomeVolume) == 0.0f);
  CHECK(rig.editor->journal().depth() == depth);  // a device setting is not either
  std::vector<core::ParamChange> heard = rig.heard();
  NEED(heard.size() == 2u);
  CHECK(heard[0].id == core::param_id(ParamKind::Track, 2, model::track_param::Volume));
  CHECK(heard[1].id == core::param_id(ParamKind::Device, 0, model::device_setting::MetronomeVolume));
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // (the follow binding's unit: the instrument 5 volume)
  CHECK(rig.project().track(2)->volume == 100);  // the controller's track volume is final
  CHECK(rig.app->device_settings().get(model::device_setting::MetronomeVolume) == 0.0f);  // and undo leaves the device alone
  // Without a document the device's bindings still work, and the project's parameters are not reachable.
  Rig bare(false);
  CHECK(bare.app->device_settings().set_learn(0, core::LearnBinding{core::param_id(ParamKind::Device, 0, model::device_setting::MetronomeVolume), 21, 0,
                                                              static_cast<std::uint8_t>(core::LearnMode::Relative), 0}) == model::Status::Ok);
  CHECK(bare.invoke("midi.cc", cc(1, 21, 5)) == DispatchResult::Changed);
  CHECK(bare.app->device_settings().get(model::device_setting::MetronomeVolume) == 55.0f);  // 50 + 5 detents of 1
}

// ---- OS-013a: MIDI input end to end, and the list of bindings ---------------------------------------------------------------------

namespace {

// The bytes of control changes as the render's parser would have taken them from a port.
void send_cc(midi::CcInput& in, unsigned port, unsigned channel, unsigned controller, unsigned value) {
  const std::uint8_t bytes[3] = {static_cast<std::uint8_t>(0xB0u | (channel - 1u)), static_cast<std::uint8_t>(controller), static_cast<std::uint8_t>(value)};
  for (const std::uint8_t b : bytes) {
    in.feed(midi::RtItem{0, b, static_cast<std::uint8_t>(port), 0});
  }
}

}  // namespace

TEST_CASE("midi input: which messages MIDI IN and IN CHANNEL let through") {
  using app::midi_in_accepts;
  CHECK(midi_in_accepts(2, 0, 0, 1));  // DIN + USB, every channel
  CHECK(midi_in_accepts(2, 0, 1, 16));
  CHECK(midi_in_accepts(0, 0, 0, 5));  // DIN only
  CHECK_FALSE(midi_in_accepts(0, 0, 1, 5));
  CHECK(midi_in_accepts(1, 0, 1, 5));  // USB only
  CHECK_FALSE(midi_in_accepts(1, 0, 0, 5));
  CHECK(midi_in_accepts(2, 3, 0, 3));  // one channel
  CHECK_FALSE(midi_in_accepts(2, 3, 0, 4));
  CHECK_FALSE(midi_in_accepts(0, 3, 1, 3));  // both must hold
}

TEST_CASE("midi input: a control change binds in the Learn overlay and then moves the parameter, and MIDI IN and IN CHANNEL decide which messages count") {
  namespace ds = model::device_setting;
  Rig rig;
  midi::CcInput in;
  rig.go_to(ip::GroupFilter, 2);  // CUTOFF
  rig.invoke("learn.toggle");
  rig.pass(1);
  // Nothing to take: nothing happens. A message on DIN binds (the default MIDI IN is both ports, every channel).
  CHECK(rig.app->poll_midi_cc(in) == 0u);
  send_cc(in, 0, 1, 74, 10);
  CHECK(rig.app->poll_midi_cc(in) == 1u);
  CHECK(rig.project().settings().learn_count() == 1u);
  CHECK(rig.project().settings().learn(0).cc == 74);
  rig.tap(Button::F2);  // pickup -> jump: the parameter takes the knob's position at once
  CHECK(rig.project().settings().learn(0).mode == static_cast<std::uint8_t>(core::LearnMode::Jump));
  rig.invoke("learn.toggle");
  rig.pass(1);
  rig.heard();
  // A message of the bound controller moves it, and the audio side hears it (the main loop is the ParamChange ring's producer).
  send_cc(in, 1, 1, 74, 127);
  CHECK(rig.app->poll_midi_cc(in) == 1u);
  CHECK(rig.value(inst(0, ip::Cutoff)) == doctest::Approx(1.0f));
  std::vector<core::ParamChange> heard = rig.heard();
  NEED(heard.size() == 1u);
  CHECK(heard[0].id == inst(0, ip::Cutoff));
  // MIDI IN = DIN: the same controller on USB is not listened to
  CHECK(rig.app->device_settings().set(ds::MidiInPort, 0.0f) == model::Status::Ok);
  send_cc(in, 1, 1, 74, 0);
  CHECK(rig.app->poll_midi_cc(in) == 0u);
  CHECK(rig.value(inst(0, ip::Cutoff)) == doctest::Approx(1.0f));
  send_cc(in, 0, 1, 74, 0);
  CHECK(rig.app->poll_midi_cc(in) == 1u);
  CHECK(rig.value(inst(0, ip::Cutoff)) == doctest::Approx(0.0f));
  // IN CHANNEL = 3: the binding's channel 1 is not heard, however it was bound
  CHECK(rig.app->device_settings().set(ds::MidiInPort, 2.0f) == model::Status::Ok);
  CHECK(rig.app->device_settings().set(ds::MidiInChannel, 3.0f) == model::Status::Ok);
  send_cc(in, 0, 1, 74, 127);
  CHECK(rig.app->poll_midi_cc(in) == 0u);
  CHECK(rig.value(inst(0, ip::Cutoff)) == doctest::Approx(0.0f));
  CHECK(rig.app->device_settings().set(ds::MidiInChannel, 1.0f) == model::Status::Ok);
  send_cc(in, 0, 1, 74, 127);
  CHECK(rig.app->poll_midi_cc(in) == 1u);
  CHECK(rig.value(inst(0, ip::Cutoff)) == doctest::Approx(1.0f));
  // A pass takes at most kMidiCcPerPass messages (a flood does not hold the main loop), and keeps the rest for the next
  CHECK(rig.app->device_settings().set(ds::MidiInChannel, 0.0f) == model::Status::Ok);
  for (unsigned i = 0; i < midi::CcInput::kRingSize; ++i) {
    send_cc(in, 0, 1, 74, (i & 1u) != 0u ? 127u : 0u);  // alternate, so each one is a change
  }
  CHECK(rig.app->poll_midi_cc(in) == app::kMidiCcPerPass);
  CHECK(rig.app->poll_midi_cc(in) == midi::CcInput::kRingSize - app::kMidiCcPerPass);
  CHECK(rig.app->poll_midi_cc(in) == 0u);
  CHECK(rig.project().check_invariants());
}

TEST_CASE("midi input: a controller bound in the device's store moves a setting with no project at all") {
  Rig bare(false);
  midi::CcInput in;
  bare.tap(Button::Config);  // CONFIG, then F6: the PATTERN page, METRO VOL (its sixth row) under the cursor
  bare.tap(Button::F6);
  for (int i = 0; i < 5; ++i) {
    bare.tap(Button::Down);
  }
  CHECK(bare.app->state().screen == app::ScreenId::Config);
  bare.invoke("learn.toggle");
  bare.pass(1);
  send_cc(in, 1, 5, 21, 0);
  CHECK(bare.app->poll_midi_cc(in) == 1u);
  CHECK(bare.app->device_settings().learn_count() == 1u);
  CHECK(bare.app->device_settings().learn(0).channel == 5);
  CHECK(bare.app->device_settings().learn(0).param == core::param_id(ParamKind::Device, 0, model::device_setting::MetronomeVolume));
  bare.tap(Button::F2);  // jump
  bare.invoke("learn.toggle");
  bare.pass(1);
  send_cc(in, 1, 5, 21, 127);
  CHECK(bare.app->poll_midi_cc(in) == 1u);
  CHECK(bare.app->device_settings().get(model::device_setting::MetronomeVolume) == 100.0f);
}

TEST_CASE("learn list: both stores in one list, a cursor, delete one (undoable for the project), clear all asks twice") {
  Rig rig;
  const auto bind = [&](unsigned slot, ParamId id, unsigned cc_number) {
    CHECK(rig.editor->set_learn_slot(slot, core::LearnBinding{id, static_cast<std::uint8_t>(cc_number), 0, static_cast<std::uint8_t>(core::LearnMode::Pickup), 0}) ==
          model::Status::Ok);
  };
  bind(0, inst(0, ip::Cutoff), 74);
  bind(3, inst(2, ip::Volume), 7);
  CHECK(rig.app->device_settings().set_learn(1, core::LearnBinding{core::param_id(ParamKind::Track, 1, model::track_param::Volume), 20, 2,
                                                                   static_cast<std::uint8_t>(core::LearnMode::Relative), 0}) == model::Status::Ok);
  const app::Model& m = rig.app->model();
  CHECK(app::learn_entry_count(m) == 3u);
  app::LearnEntry e{};
  NEED(app::learn_entry_at(m, 0, e));  // the project's first, in slot order
  CHECK((e.scope == core::LearnScope::Project && e.slot == 0u && e.binding->cc == 74));
  NEED(app::learn_entry_at(m, 1, e));
  CHECK((e.scope == core::LearnScope::Project && e.slot == 3u && e.binding->cc == 7));
  NEED(app::learn_entry_at(m, 2, e));  // then the device's
  CHECK((e.scope == core::LearnScope::Device && e.slot == 1u && e.binding->cc == 20));
  CHECK_FALSE(app::learn_entry_at(m, 3, e));

  rig.tap(Button::Params);  // a screen with a target under the cursor, under the overlay
  rig.invoke("learn.toggle");
  rig.pass(1);
  const app::ViewState& v = rig.app->state();
  CHECK(v.learn.cursor == 0);
  rig.tap(Button::Down);
  CHECK(v.learn.cursor == 1);
  rig.turn(5);  // the encoder too, and the end of the list stops it
  CHECK(v.learn.cursor == 2);
  rig.tap(Button::Up);
  CHECK(v.learn.cursor == 1);
  CHECK(rig.app->failed_frames() == 0u);

  // F3 deletes the one under the cursor: the project's second binding, as one undo unit
  const unsigned depth = rig.editor->journal().depth();
  rig.tap(Button::F3);
  CHECK(app::learn_entry_count(m) == 2u);
  CHECK(rig.project().settings().learn_count() == 1u);
  CHECK(rig.editor->journal().depth() == depth + 1u);
  CHECK(v.learn.cursor == 1);  // the device's binding is now under it
  rig.tap(Button::F3);         // the device's binding: not an undo unit
  CHECK(app::learn_entry_count(m) == 1u);
  CHECK(rig.app->device_settings().learn_count() == 0u);
  CHECK(rig.editor->journal().depth() == depth + 1u);
  CHECK(v.learn.cursor == 0);  // pulled back onto the list
  rig.invoke("learn.toggle");  // the overlay closes and opens again at the top of the list
  rig.pass(1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.project().settings().learn_count() == 2u);  // the deleted project binding is back
  CHECK(rig.project().settings().learn(3).cc == 7);

  // F4 arms Clear all, a move or any other key disarms it, a second F4 clears both stores
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK_FALSE(v.learn.confirm);
  rig.tap(Button::F4);
  CHECK(v.learn.confirm);
  CHECK(app::learn_entry_count(m) == 2u);
  rig.tap(Button::Down);
  CHECK_FALSE(v.learn.confirm);
  rig.tap(Button::F4);
  CHECK(v.learn.confirm);
  rig.invoke("learn.toggle");  // closing disarms it
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK_FALSE(rig.app->state().learn.confirm);
  CHECK(app::learn_entry_count(m) == 2u);
  CHECK(rig.app->device_settings().set_learn(0, core::LearnBinding{core::param_id(ParamKind::Master, 0, model::master_param::Volume), 9, 0,
                                                                   static_cast<std::uint8_t>(core::LearnMode::Jump), 0}) == model::Status::Ok);
  CHECK(app::learn_entry_count(m) == 3u);
  rig.tap(Button::F4);
  rig.tap(Button::F4);
  CHECK(app::learn_entry_count(m) == 0u);
  CHECK(rig.project().settings().learn_count() == 0u);
  CHECK(rig.app->device_settings().learn_count() == 0u);
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // the project's two bindings are one undo unit
  CHECK(rig.project().settings().learn_count() == 2u);
  // Nothing to delete or clear: nothing happens (and the first Clear all of a list that has something only arms)
  CHECK(rig.invoke("learn.clear_all") == DispatchResult::Changed);
  CHECK(app::learn_entry_count(m) == 2u);
  CHECK(rig.invoke("learn.clear_all") == DispatchResult::Changed);
  CHECK(app::learn_entry_count(m) == 0u);
  CHECK(rig.invoke("learn.delete") == DispatchResult::Unchanged);
  CHECK(rig.invoke("learn.clear_all") == DispatchResult::Unchanged);
  CHECK(rig.app->failed_frames() == 0u);
}

TEST_CASE("param actions: an edit on a screen with no parameters does nothing, and the registry has them all") {
  Rig rig;
  CHECK(rig.invoke("param.edit", 1) == DispatchResult::Unchanged);  // Pattern: no focus parameter
  CHECK(rig.invoke("param.coarse", 1) == DispatchResult::Unchanged);
  CHECK(rig.invoke("param.reset") == DispatchResult::Unchanged);
  CHECK(rig.invoke("param.group", 1) == DispatchResult::Unchanged);  // Pattern has one page
  CHECK(rig.invoke("learn.clear") == DispatchResult::Unchanged);
  CHECK(rig.invoke("learn.mode") == DispatchResult::Unchanged);
  for (const char* name : {"param.edit", "param.coarse", "param.reset", "param.group", "learn.clear", "learn.mode", "midi.cc"}) {
    CHECK_MESSAGE(app::find_action(app::action_registry(), core::action_id(name)) != nullptr, name);
  }
  // The editing needs the base screen's focus: not while a modal overlay is up.
  rig.go_to(ip::GroupVoice, 0);
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.invoke("param.edit", 1) == DispatchResult::Disabled);
  CHECK(rig.invoke("param.reset") == DispatchResult::Disabled);
  CHECK(rig.invoke("param.group", 2) == DispatchResult::Disabled);
  CHECK(rig.value(inst(0, ip::Volume)) == 100.0f);
}

TEST_CASE("param ring: a long editing session ends with the audio side where the project is") {
  Rig rig;
  struct Audio {
    std::uint32_t id[16] = {};
    float value[16] = {};
    unsigned n = 0;
    void hear(const core::ParamChange& c) {
      for (unsigned i = 0; i < n; ++i) {
        if (id[i] == static_cast<std::uint32_t>(c.id)) {
          value[i] = c.value;
          return;
        }
      }
      id[n] = static_cast<std::uint32_t>(c.id);
      value[n++] = c.value;
    }
  } audio;
  rig.go_to(ip::GroupVoice, 0);
  std::uint32_t seed = 4242u;
  unsigned blocks = 0;
  for (unsigned i = 0; i < 600; ++i) {
    seed = seed * 1664525u + 1013904223u;
    switch ((seed >> 20) % 5) {
      case 0: rig.turn(static_cast<int>((seed >> 8) % 7) - 3); break;
      case 1: rig.tap(Button::Down); break;
      case 2: rig.tap(Button::Up); break;
      case 3: rig.shift_turn(static_cast<int>((seed >> 8) % 3) - 1); break;
      default: rig.tap(Button::F8); break;  // undo
    }
    // The engine, every few edits: a block takes at most 64.
    if (i % 5 == 0) {
      core::ParamChange c;
      unsigned taken = 0;
      while (taken < core::kMaxParamsPerBlock && rig.ring.pop(c)) {
        audio.hear(c);
        ++taken;
      }
      ++blocks;
    }
  }
  core::ParamChange c;
  while (rig.ring.pop(c)) {
    audio.hear(c);
  }
  CHECK(blocks == 120u);
  CHECK(audio.n >= 3u);
  CHECK(rig.app->session().param_drops == 0u);
  for (unsigned i = 0; i < audio.n; ++i) {
    CHECK_MESSAGE(audio.value[i] == rig.project().get_param(static_cast<ParamId>(audio.id[i])), "parameter " << audio.id[i]);
  }
  CHECK(rig.project().check_invariants());
}

// ---- the scenario words of this slice ------------------------------------------------------------------------------------------

TEST_CASE("scenario: document and midicc are commands, checked when the script is parsed") {
  std::vector<sim::ScenarioCommand> cmds;
  std::string error;
  CHECK(sim::parse_scenario("document\ndocument bindings\nmidicc 1 74 127\nmidicc 16 0 0\n", cmds, error));
  NEED(cmds.size() == 4u);
  CHECK(cmds[0].kind == sim::ScenarioCommand::Kind::Document);
  CHECK(cmds[0].value == 0u);
  CHECK(cmds[1].value == 1u);
  CHECK(cmds[2].kind == sim::ScenarioCommand::Kind::MidiCc);
  CHECK(cmds[2].arg == cc(1, 74, 127));
  CHECK(cmds[3].arg == cc(16, 0, 0));
  for (const char* bad : {"document now\n", "midicc\n", "midicc 1 74\n", "midicc 0 74 1\n", "midicc 17 74 1\n", "midicc 1 128 1\n", "midicc 1 74 128\n",
                          "midicc 1 -1 3\n", "midicc 1 74 +3\n", "midicc a b c\n", "midicc 1 74 1 1\n"}) {
    CHECK_MESSAGE(!sim::parse_scenario(bad, cmds, error), bad);
  }
}

TEST_CASE("simulator: a document opened by the scenario is edited by the keys, and the fake audio side hears it") {
  sim::Simulator sim;
  CHECK(sim.project() == nullptr);
  CHECK(sim.heard_params().empty());
  sim.open_document(true);
  NEED(sim.project() != nullptr);
  CHECK(sim.project()->settings().learn_count() == 2u);
  sim.tap(Button::Params);
  sim.encoder(-4);
  CHECK(sim.project()->get_param(inst(0, ip::Volume)) == 96.0f);
  NEED(sim.heard_params().size() == 1u);
  CHECK(sim.heard_params()[0].id == inst(0, ip::Volume));
  CHECK(sim.heard_params()[0].value == 96.0f);
  CHECK(sim.action(core::action_id("midi.cc"), cc(1, 71, 127)) == DispatchResult::Changed);
  CHECK(sim.heard_params().size() == 2u);
  sim.open_document(false);  // replaced: a new project, the old heard list gone
  CHECK(sim.project()->get_param(inst(0, ip::Volume)) == 100.0f);
  CHECK(sim.heard_params().empty());
  CHECK(sim.healthy());
}
