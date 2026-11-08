// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-LEARNFOLLOW S1 in the app: F5 FOLLOW in the Learn overlay (the action "learn.follow") toggles the target's binding between "this
// instrument" and "the selected instrument", rebinding a followed field keeps the follow, and a follow binding acts only when the
// selected instrument has the field with its meaning (the fit rule, 3.3). Host only, on the app rig of params_app_test.cpp; the LRNB
// round trip of a follow binding is param_model_test.cpp's, here driven through the action.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/screens/mixer_screen.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/screens/screen.h>
#include <o3os/audio/module_voices.h>
#include <o3os/core/input_event.h>
#include <o3os/core/learn.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/instrument.h>
#include <o3os/model/params.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

// The app rig of params_app_test.cpp: the fakes, a demo project in arena 1, its journal and editor attached, a ParamChange ring.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  core::ParamRing ring;

  Rig() {
    model::Project& p = project();
    sim::fill_demo_project(p);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_param_ring(&ring);
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
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  // The newest notification in the queue.
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

const core::LearnBinding* slots(Rig& rig) { return rig.project().settings().learn_slots(); }
bool store_valid(Rig& rig) { return core::learn_store_valid(slots(rig), core::kLearnSlots); }

// A module of our own, compiled into the test, so that a Module instrument's params page has rows without the card's machinery.
struct TestModState {
  float p[2];
};
int testmod_init(void*, const o3m_host*) { return 0; }
std::uint32_t testmod_render(void*, const float*, float* out, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = 0.0f;
  }
  return 0;
}
void testmod_set_param(void*, std::uint32_t, float) {}
void testmod_note_on(void*, std::uint32_t, std::uint32_t, std::uint32_t) {}
void testmod_note_off(void*, std::uint32_t) {}
void testmod_shutdown(void*) {}
const o3m_param kTestModParams[2] = {{"CUTOFF", 0.0f, 100.0f, 50.0f, 0u}, {"RESO", 0.0f, 100.0f, 20.0f, 0u}};
const o3m_module kTestMod = {O3M_ABI_MAJOR,       O3M_ABI_MINOR,      O3M_KIND_INSTRUMENT, sizeof(o3m_module), "testmod",
                             sizeof(TestModState), 2,               kTestModParams,      testmod_init,
                             testmod_render,       testmod_set_param,  testmod_note_on,     testmod_note_off,    testmod_shutdown, nullptr, 0};

// The same rig with a ModuleUi attached and instrument 3 (slot 2) a Module instrument bound to the test module.
struct ModuleRig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  core::ParamRing ring;
  module::ModuleSet set;
  module::ModuleLoader loader;
  audio::ModuleVoices voices;
  app::ModuleUi mod_ui;
  alignas(64) std::uint8_t cold_mem[64 * 1024];
  alignas(16) std::uint8_t scratch[audio::ModuleVoices::kScratchBytes];
  alignas(16) TestModState mod_state;

  ModuleRig() {
    model::Project& p = test::arena(1).project;
    sim::fill_demo_project(p);
    CHECK(set.cold().init(cold_mem, 0x70000000u, sizeof cold_mem));
    CHECK(set.add_native("testmod", kTestMod));
    CHECK(voices.attach_scratch(scratch, sizeof scratch));
    mod_ui.attach(set, loader, voices);
    app->attach_modules(&mod_ui);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_param_ring(&ring);
    CHECK(p.add_module("testmod", 7, kTestMod.abi_major, kTestMod.abi_minor) == model::Status::Ok);
    model::Instrument mod;
    model::init_instrument(mod, model::InstrumentKind::Module);
    std::memcpy(mod.name, "MOD", 3);
    mod.module_index = 0;
    CHECK(p.set_instrument(2, mod) == model::Status::Ok);
    CHECK(voices.bind(2, kTestMod, 0x8102, &mod_state, nullptr, 0) == audio::ModuleVoices::Bind::Ok);
    app->run_once();
  }

  model::Project& project() { return test::arena(1).project; }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    pass(1);
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass(1);
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  std::string last_note() const {
    const core::NotifyQueue& q = app->state().notify;
    return q.empty() ? std::string() : std::string(q.at(q.size() - 1).text.c_str());
  }
  void go_to(unsigned page, unsigned row = 0) {
    tap(Button::Params);
    invoke("param.group", static_cast<ActionArg>(page));
    for (unsigned i = 0; i < row; ++i) {
      tap(Button::Down);
    }
  }
};

}  // namespace

// ---- F5 FOLLOW: the toggle ----------------------------------------------------------------------------------------------------

TEST_CASE("learn follow: F5 makes the target's binding follow the selected instrument, as one undo unit; undo gives the fixed binding back") {
  Rig rig;
  CHECK(rig.invoke("instrument.select", 3) == DispatchResult::Changed);
  rig.go_to(ip::GroupFilter, 2);  // CUTOFF of instrument 3
  CHECK(app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()) == inst(2, ip::Cutoff));
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.app->learn());
  CHECK(rig.invoke("midi.cc", cc(1, 74, 64)) == DispatchResult::Changed);  // bind CC 74 to instrument 3's CUTOFF
  CHECK(rig.project().settings().learn_count() == 1u);
  CHECK(rig.project().settings().learn(0).param == inst(2, ip::Cutoff));
  const unsigned depth = rig.editor->journal().depth();
  rig.tap(Button::F5);  // F5 in the Learn context
  CHECK(rig.project().settings().learn_count() == 1u);  // the store holds the follow binding and nothing else on CC 74
  const core::LearnBinding& b = rig.project().settings().learn(0);
  CHECK(b.param == inst(0, ip::Cutoff));  // index 0: only its kind and field count
  CHECK((b.flags & core::kLearnFollow) != 0);
  CHECK(b.cc == 74);
  CHECK(b.channel == 1);
  CHECK(b.mode == static_cast<std::uint8_t>(core::LearnMode::Pickup));  // the mode is kept
  CHECK(core::learn_find_cc(slots(rig), core::kLearnSlots, 1, 74) == 0);
  CHECK(core::learn_find_cc(slots(rig), core::kLearnSlots, 1, 74, 1) == -1);  // nothing else listens to CC 74
  CHECK(store_valid(rig));
  CHECK(rig.last_note() == "CC 74 FOLLOWS: CUTOFF");
  CHECK(rig.editor->journal().depth() == depth + 1u);  // F5 is one undo unit
  // Undo gives the fixed binding back; undo again takes the binding away.
  rig.invoke("learn.toggle");  // close Learn
  rig.pass(1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.project().settings().learn_count() == 1u);
  CHECK(rig.project().settings().learn(0).param == inst(2, ip::Cutoff));
  CHECK(rig.project().settings().learn(0).flags == 0);
  CHECK(store_valid(rig));
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.project().settings().learn_count() == 0u);
}

TEST_CASE("learn follow: the message moves the selected instrument's parameter, and leaves the others alone") {
  Rig rig;
  rig.invoke("instrument.select", 3);
  rig.go_to(ip::GroupFilter, 2);
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.invoke("midi.cc", cc(1, 74, 64)) == DispatchResult::Changed);  // bind CC 74 to instrument 3's CUTOFF
  rig.tap(Button::F5);                                                     // follow
  rig.tap(Button::F2);                                                     // pickup -> jump
  rig.invoke("learn.toggle");
  rig.pass(1);
  const ParamId c3 = inst(2, ip::Cutoff);
  CHECK(rig.invoke("midi.cc", cc(1, 74, 100)) == DispatchResult::Changed);
  CHECK(rig.value(c3) == doctest::Approx(100.0 / 127.0).epsilon(1.0e-4));  // instrument 3, the selected one
  CHECK(rig.invoke("instrument.select", 5) == DispatchResult::Changed);
  CHECK(rig.invoke("midi.cc", cc(1, 74, 127)) == DispatchResult::Changed);
  CHECK(rig.value(inst(4, ip::Cutoff)) == 1.0f);                             // instrument 5 moved
  CHECK(rig.value(c3) == doctest::Approx(100.0 / 127.0).epsilon(1.0e-4));  // instrument 3 left alone
  CHECK(rig.heard().size() == 2u);
}

TEST_CASE("learn follow: F5 on another instrument's same field pins the binding back to that instrument") {
  Rig rig;
  rig.invoke("instrument.select", 3);
  rig.go_to(ip::GroupFilter, 2);
  rig.invoke("learn.toggle");
  rig.pass(1);
  rig.invoke("midi.cc", cc(1, 74, 64));
  rig.tap(Button::F5);  // follow
  rig.tap(Button::F2);  // jump
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.invoke("instrument.select", 5) == DispatchResult::Changed);
  rig.go_to(ip::GroupFilter, 2);  // CUTOFF of instrument 5
  CHECK(app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()) == inst(4, ip::Cutoff));
  rig.invoke("learn.toggle");
  rig.pass(1);
  rig.tap(Button::F5);  // pinned to instrument 5
  CHECK(rig.project().settings().learn_count() == 1u);
  const core::LearnBinding& b = rig.project().settings().learn(0);
  CHECK(b.param == inst(4, ip::Cutoff));
  CHECK(b.flags == 0);
  CHECK(b.cc == 74);
  CHECK(b.mode == static_cast<std::uint8_t>(core::LearnMode::Jump));
  CHECK(store_valid(rig));
  CHECK(rig.last_note() == "CC 74 PINNED: CUTOFF INS 05");
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.invoke("instrument.select", 3) == DispatchResult::Changed);
  CHECK(rig.invoke("midi.cc", cc(1, 74, 20)) == DispatchResult::Changed);  // the pinned binding acts on instrument 5 whatever is selected
  CHECK(rig.value(inst(4, ip::Cutoff)) == doctest::Approx(20.0 / 127.0).epsilon(1.0e-4));
  CHECK(rig.value(inst(2, ip::Cutoff)) != doctest::Approx(20.0 / 127.0).epsilon(1.0e-4));  // instrument 3 is not its target
}

// ---- rebinding keeps the follow ------------------------------------------------------------------------------------------------

TEST_CASE("learn follow: rebinding a followed field stays a follow binding and keeps the mode") {
  Rig rig;
  rig.invoke("instrument.select", 3);
  rig.go_to(ip::GroupFilter, 2);
  rig.invoke("learn.toggle");
  rig.pass(1);
  rig.invoke("midi.cc", cc(1, 74, 64));  // bind CC 74
  rig.tap(Button::F5);                   // follow
  rig.tap(Button::F2);                   // jump
  CHECK(rig.invoke("midi.cc", cc(1, 20, 10)) == DispatchResult::Changed);  // rebind the field to CC 20
  CHECK(rig.project().settings().learn_count() == 1u);
  const core::LearnBinding& b = rig.project().settings().learn(0);
  CHECK(b.param == inst(0, ip::Cutoff));
  CHECK((b.flags & core::kLearnFollow) != 0);
  CHECK(b.cc == 20);
  CHECK(b.mode == static_cast<std::uint8_t>(core::LearnMode::Jump));  // the mode the user chose is kept
  CHECK(store_valid(rig));
  CHECK(rig.last_note() == "CC 20 > CUTOFF");
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.invoke("midi.cc", cc(1, 20, 90)) == DispatchResult::Changed);
  CHECK(rig.value(inst(2, ip::Cutoff)) == doctest::Approx(90.0 / 127.0).epsilon(1.0e-4));  // the selected instrument
}

TEST_CASE("learn follow: with another follow binding of the field on CC 20, F5 leaves one follow binding of the field, on CC 74") {
  Rig rig;
  rig.invoke("instrument.select", 3);
  rig.go_to(ip::GroupFilter, 2);
  rig.invoke("learn.toggle");
  rig.pass(1);
  rig.invoke("midi.cc", cc(1, 74, 64));  // CC 74 fixed on instrument 3's CUTOFF (slot 0)
  // A follow binding of CUTOFF on CC 20 (slot 1), as an earlier F5 left it.
  CHECK(rig.editor->set_learn_slot(1, core::LearnBinding{inst(0, ip::Cutoff), 20, 0,
                                                        static_cast<std::uint8_t>(core::LearnMode::Jump), core::kLearnFollow}) == model::Status::Ok);
  CHECK(store_valid(rig));
  rig.tap(Button::F5);  // the target's binding learn_find_param finds first is the fixed one on CC 74 (the lower slot)
  CHECK(rig.project().settings().learn_count() == 1u);  // one follow binding of CUTOFF remains
  const int at = core::learn_find_cc(slots(rig), core::kLearnSlots, 1, 74);
  CHECK(at >= 0);
  const core::LearnBinding& b = rig.project().settings().learn(static_cast<unsigned>(at));
  CHECK(b.param == inst(0, ip::Cutoff));
  CHECK((b.flags & core::kLearnFollow) != 0);
  CHECK(b.cc == 74);                                                    // on the toggled controller
  CHECK(core::learn_find_cc(slots(rig), core::kLearnSlots, 1, 20) == -1);  // CC 20 lost its binding
  CHECK(store_valid(rig));
  CHECK(rig.last_note() == "CC 74 FOLLOWS: CUTOFF");
}

// ---- the refusals --------------------------------------------------------------------------------------------------------------

TEST_CASE("learn follow: a track parameter and an unbound target say so and change nothing") {
  Rig rig;
  rig.tap(Button::Master);  // the Mixer: its first row is a track's LEVEL
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Mixer);
  CHECK(core::param_kind(app::kMixerScreen.focus_param(rig.app->model(), rig.app->state())) == ParamKind::Track);
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.invoke("learn.follow") == DispatchResult::Changed);
  CHECK(rig.last_note() == "FOLLOW: INSTRUMENT PARAMETERS ONLY");
  CHECK(rig.project().settings().learn_count() == 0u);
  rig.invoke("learn.toggle");
  rig.pass(1);
  // An instrument target with nothing bound to it.
  CHECK(rig.invoke("instrument.select", 3) == DispatchResult::Changed);
  rig.go_to(ip::GroupFilter, 2);
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.invoke("learn.follow") == DispatchResult::Changed);
  CHECK(rig.last_note() == "NOTHING BOUND TO THIS");
  CHECK(rig.project().settings().learn_count() == 0u);
  CHECK(store_valid(rig));
}

TEST_CASE("learn follow: a module parameter refuses (a module parameter never follows)") {
  ModuleRig rig;
  CHECK(rig.invoke("instrument.select", 3) == DispatchResult::Changed);  // the Module instrument
  rig.go_to(0, 0);                                                        // its first parameter
  CHECK(app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()) == inst(2, ip::kModuleFirst));
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.invoke("learn.follow") == DispatchResult::Changed);
  CHECK(rig.last_note() == "FOLLOW: NOT FOR A MODULE PARAMETER");
  CHECK(rig.project().settings().learn_count() == 0u);
}

// ---- the fit rule (3.3) --------------------------------------------------------------------------------------------------------

TEST_CASE("learn follow: the fit rule — a follow binding does nothing on an instrument of another kind, and writes no journal unit") {
  Rig rig;
  CHECK(rig.editor->set_learn_slot(0, core::LearnBinding{inst(0, ip::Cutoff), 74, 0,
                                                        static_cast<std::uint8_t>(core::LearnMode::Jump), core::kLearnFollow}) == model::Status::Ok);
  CHECK(rig.editor->set_learn_slot(1, core::LearnBinding{inst(0, ip::MidiVelocity), 75, 0,
                                                        static_cast<std::uint8_t>(core::LearnMode::Jump), core::kLearnFollow}) == model::Status::Ok);
  model::Instrument midi;
  model::init_instrument(midi, model::InstrumentKind::Midi);
  midi.midi_channel = 3;  // a MIDI instrument needs a channel
  std::memcpy(midi.name, "KEYS", 4);
  CHECK(rig.editor->set_instrument(4, midi) == model::Status::Ok);  // instrument 5 is a MIDI one (through the editor: the journal keeps its units)
  CHECK(rig.invoke("instrument.select", 5) == DispatchResult::Changed);  // the MIDI instrument
  rig.heard();
  const unsigned depth = rig.editor->journal().depth();
  CHECK(rig.invoke("midi.cc", cc(1, 74, 100)) == DispatchResult::Unchanged);  // a sampler field on a MIDI instrument: silent
  CHECK(rig.heard().empty());
  CHECK(rig.editor->journal().depth() == depth);  // no write, no journal, no strip message
  // A follow MIDI VELOCITY with a Sample instrument selected does nothing.
  CHECK(rig.invoke("instrument.select", 3) == DispatchResult::Changed);  // a sample instrument
  CHECK(rig.invoke("midi.cc", cc(1, 75, 100)) == DispatchResult::Unchanged);
  CHECK(rig.heard().empty());
  CHECK(rig.editor->journal().depth() == depth);
  // ... and the same binding acts on a MIDI instrument (one write, one journal unit).
  CHECK(rig.invoke("instrument.select", 5) == DispatchResult::Changed);
  const float vel_before = rig.value(inst(4, ip::MidiVelocity));
  CHECK(rig.invoke("midi.cc", cc(1, 75, 100)) == DispatchResult::Changed);
  CHECK(rig.value(inst(4, ip::MidiVelocity)) != vel_before);
  CHECK(rig.editor->journal().depth() == depth + 1u);
}

TEST_CASE("learn follow: pickup after a selection change — the knob must catch the new instrument's value first") {
  Rig rig;
  rig.invoke("instrument.select", 3);
  rig.go_to(ip::GroupFilter, 2);
  rig.invoke("learn.toggle");
  rig.pass(1);
  rig.invoke("midi.cc", cc(1, 74, 64));  // bind, pickup
  rig.tap(Button::F5);                   // follow
  rig.invoke("learn.toggle");
  rig.pass(1);
  // Instrument 3's cutoff is 0.42 (53.3 on the knob's 0..127 scale); instrument 5's is 0.54 (68.6).
  const ParamId c3 = inst(2, ip::Cutoff);
  CHECK(rig.value(c3) == doctest::Approx(0.42).epsilon(1.0e-4));
  CHECK(rig.invoke("midi.cc", cc(1, 74, 100)) == DispatchResult::Unchanged);  // far away: nothing
  CHECK(rig.invoke("midi.cc", cc(1, 74, 53)) == DispatchResult::Changed);     // caught, and it drives
  CHECK(rig.value(c3) == doctest::Approx(53.0 / 127.0).epsilon(1.0e-4));
  CHECK(rig.invoke("instrument.select", 5) == DispatchResult::Changed);
  CHECK(rig.invoke("midi.cc", cc(1, 74, 53)) == DispatchResult::Unchanged);  // the knob has not caught instrument 5's value
  CHECK(rig.value(inst(4, ip::Cutoff)) == doctest::Approx(0.54).epsilon(1.0e-4));
  CHECK(rig.invoke("midi.cc", cc(1, 74, 69)) == DispatchResult::Changed);  // 68.6 is within one step of 69
  CHECK(rig.value(inst(4, ip::Cutoff)) == doctest::Approx(69.0 / 127.0).epsilon(1.0e-4));
}

// ---- stored: the LRNB chunk already carries the flag ---------------------------------------------------------------------------

TEST_CASE("learn follow: a follow binding made by F5 survives the LRNB write and read") {
  Rig rig;
  rig.invoke("instrument.select", 3);
  rig.go_to(ip::GroupFilter, 2);
  rig.invoke("learn.toggle");
  rig.pass(1);
  rig.invoke("midi.cc", cc(1, 74, 64));
  rig.tap(Button::F5);
  std::vector<std::uint8_t> bytes;
  CHECK(test::save(rig.project(), bytes) == formats::native::Progress::Done);
  model::ModelArena& a = test::arena(2);
  formats::native::Report report{};
  CHECK(test::load(a, bytes, &report) == formats::native::Progress::Done);
  CHECK(report.error == formats::native::Error::None);
  CHECK(a.project.settings().learn_count() == 1u);
  const core::LearnBinding& b = a.project.settings().learn(0);
  CHECK(b.param == inst(0, ip::Cutoff));
  CHECK((b.flags & core::kLearnFollow) != 0);
  CHECK(b.cc == 74);
  CHECK(b.channel == 1);
  CHECK(b.mode == static_cast<std::uint8_t>(core::LearnMode::Pickup));
  CHECK(core::learn_store_valid(a.project.settings().learn_slots(), core::kLearnSlots));
}
