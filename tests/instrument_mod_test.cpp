// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S6: the MOD page (F4) of the Instrument screen. Per destination (the six envelope slots) a SOURCE row Off / Envelope / LFO, kept as the two stored bytes (the envelope's enable and loop bytes), and the slot's LFO (SamplerParams::lfo[e]: shape, speed, amount). The labels and the ranges follow the instrument file (noted at each check). No stored layout
// changes: the round trip of an OG instrument through the page is in og_instrument_test.cpp. The S5 review's two minors are checked here too: no wavetable window shown for an instrument that plays nothing,
// and INIT resetting the envelope delay / hold / loop and the LFOs.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/wave_math.h>
#include <o3os/audio/voices.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include "../app/src/param_access.h"  // write_param, read_param (private to app/src): the WT tests call the write and read path themselves

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::DispatchResult;
using core::ActionArg;
using core::ParamKind;
namespace ip = model::instrument_param;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];

constexpr unsigned kUsed = 4;  // instrument 5: a sample instrument with audio
constexpr std::uint32_t kFrames = 8000;

core::ParamId inst_id(unsigned slot, unsigned field) { return core::param_id(ParamKind::Instrument, slot, field); }

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory;
  std::vector<std::int16_t> peaks;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  explicit Rig(std::uint32_t frames = kFrames) : memory(100000), peaks(20000) {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), 100000, peaks.data(), peaks.size());
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::strcpy(in.name, "KICK");
    std::strcpy(in.sample_path, "samples/kick.wav");
    NEED(arena->project.set_instrument(kUsed, in) == model::Status::Ok);
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < frames; ++i) {
      p[i] = static_cast<std::int16_t>((i % 64u) * 400 - 12000);
    }
    NEED(bank.commit_load(kUsed, p, frames));
    bank.rebind_now(kUsed);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena->project, journal);
    app->attach_document(editor.get());
    app->attach_samples(&bank);
    const_cast<app::ViewState&>(app->state()).instrument = static_cast<int>(kUsed) + 1;
    pass();
    tap(Button::Params);
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
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
  void shift_tap(Button b) {
    press(Button::Shift);
    pass(1);
    tap(b);
    release(Button::Shift);
    pass(1);
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  const model::Instrument* inst() const { return arena->project.instrument(kUsed); }
  app::ViewState& view() { return const_cast<app::ViewState&>(app->state()); }
  // The MOD page on destination `e`, the cursor on row `row`.
  void go_mod(unsigned e, unsigned row = 0) {
    invoke("param.group", ip::GroupEnvelope);
    view().params.env = static_cast<int>(e);
    view().params.row = 0;
    for (unsigned i = 0; i < row; ++i) {
      tap(Button::Down);
    }
    pass();
  }
  float value(unsigned field) const { return arena->project.get_param(inst_id(kUsed, field)); }
  std::vector<std::uint16_t> rows() {
    std::uint16_t f[app::ParamView::kMaxRows] = {};
    const unsigned n = app::params_mod_rows(app->model(), app->state(), f);
    return std::vector<std::uint16_t>(f, f + n);
  }
  const char* why(unsigned row) { return app::params_mod_row_why(app->model(), app->state(), row); }
  void settle() {
    std::uint64_t now = 1000000;
    for (int i = 0; i < 3 && bank.rebind_pending(); ++i) {
      (void)bank.poll(now, true, true, true);
      now += 100;
    }
  }
};

// The state change on a copy of the two bytes: `up` is value > 0 without rolling over the list, else value < 0.
void og_env_state(model::Envelope& e, bool up) {
  if (up) {
    if (e.enable == 0) {
      e.enable = 1;
      e.loop = 0;
    } else if (e.loop == 0) {
      e.loop = 1;
    }
  } else if (e.loop == 1) {
    e.loop = 0;
    e.enable = 1;
  } else if (e.enable == 1) {
    e.enable = 0;
  }
}

}  // namespace

// ---- the rows: labels and ranges are those of docs/specs/OS-INSTUI.md ----------------------------------------------------------------------------------------------------------------

TEST_CASE("mod rows: the SOURCE words, the five shapes and the speeds follow the instrument file, and so do the ranges") {
  for (unsigned e = 0; e < ip::kEnvelopes; ++e) {
    const core::ParamDesc* src = model::param_desc(inst_id(0, ip::mod(e, ip::ModSource)));
    NEED(src != nullptr);
    CHECK(std::strcmp(src->name, "SOURCE") == 0);
    CHECK(src->list_count == 5);  // the source labels (the last two are OS-CEWTENV W7a's)
    CHECK(std::strcmp(src->names[0], "Off") == 0);
    CHECK(std::strcmp(src->names[1], "Envelope") == 0);
    CHECK(std::strcmp(src->names[2], "LFO") == 0);
    CHECK(std::strcmp(src->names[3], "WT Env") == 0);
    CHECK(std::strcmp(src->names[4], "WT LFO") == 0);
    CHECK((src->min == 0.0f && src->max == 4.0f));
    CHECK(src->def == (e == 0 ? 1.0f : 0.0f));  // our default: the volume envelope on, the others off
    CHECK((src->flags & core::kParamLearnable) != 0);
    CHECK(src->group == ip::GroupEnvelope);

    const core::ParamDesc* shape = model::param_desc(inst_id(0, ip::mod(e, ip::LfoShape)));
    NEED(shape != nullptr);
    CHECK(shape->list_count == 6);  // the six LFO shapes (the sixth, Sine, is OS-LFOSINE's)
    const char* const shapes[6] = {"Rev Saw", "Saw", "Triangle", "Square", "Random", "Sine"};
    for (unsigned i = 0; i < 6; ++i) {
      CHECK(std::strcmp(shape->names[i], shapes[i]) == 0);
    }
    CHECK((shape->min == 0.0f && shape->max == 5.0f));
    CHECK(shape->def == 2.0f);  // our default is the triangle, also what init_instrument stores

    const core::ParamDesc* speed = model::param_desc(inst_id(0, ip::mod(e, ip::LfoSpeed)));
    NEED(speed != nullptr);
    // the 29 LFO speed labels; the amp slot's table is the last 24 of them
    const unsigned count = e == 0 ? 24u : 29u;
    const unsigned base = e == 0 ? 5u : 0u;
    CHECK(speed->list_count == count);
    CHECK((speed->min == 0.0f && speed->max == static_cast<float>(count - 1u)));
    CHECK(speed->def == 0.0f);
    static const char* const labels[29] = {"128 steps", "96 steps", "64 steps", "48 steps", "32 steps", "24 steps", "16 steps", "12 steps", "8 steps", "6 steps",
                                           "4 steps", "3 steps", "2 steps", "3/2 step", "1 step", "3/4 step", "1/2 step", "3/8 step", "1/3 step", "1/4 step",
                                           "3/16 step", "1/6 step", "1/8 step", "1/12 step", "1/16 step", "1/24 step", "1/32 step", "1/48 step", "1/64 step"};
    for (unsigned i = 0; i < count; ++i) {
      CHECK_MESSAGE(std::strcmp(speed->names[i], labels[base + i]) == 0, "destination " << e << " speed " << i);
    }

    const core::ParamDesc* amount = model::param_desc(inst_id(0, ip::mod(e, ip::LfoAmount)));
    NEED(amount != nullptr);
    CHECK((amount->min == 0.0f && amount->max == 1.0f));  // the largest amount, 1.0
    CHECK(amount->def == 0.5f);                           // our default LFO amount
    CHECK(amount->detents == 100);                        // the amount steps by 0.01
    for (const core::ParamDesc* d : {shape, speed, amount}) {
      CHECK((d->flags & core::kParamLearnable) != 0);
      CHECK(d->group == ip::GroupEnvelope);
      CHECK(std::strlen(d->name) <= core::kParamNameChars);
    }
  }
  // the group is now called MOD
  CHECK(std::string(model::param_group_name(ParamKind::Instrument, ip::GroupEnvelope)) == "MOD");
}

TEST_CASE("mod rows: a fresh instrument has our LFO defaults (Triangle, speed 0, 0.5), and the stored LFO fields are where the rows say") {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  for (unsigned e = 0; e < model::kLfos; ++e) {
    CHECK(in.sampler.lfo[e].shape == 2);
    CHECK(in.sampler.lfo[e].speed == 0);
    CHECK(in.sampler.lfo[e].amount == 0.5f);
  }
  model::ParamField f;
  NEED(model::find_param(inst_id(0, ip::mod(3, ip::LfoAmount)), f));
  CHECK(model::param_store(&in, f, 0.25f));
  CHECK(in.sampler.lfo[3].amount == 0.25f);
  CHECK(in.sampler.lfo[2].amount == 0.5f);  // the neighbours stay
  CHECK(in.sampler.lfo[4].amount == 0.5f);
  NEED(model::find_param(inst_id(0, ip::mod(5, ip::LfoSpeed)), f));
  CHECK(model::param_store(&in, f, 28.0f));
  CHECK(in.sampler.lfo[5].speed == 28);
  CHECK_FALSE(model::param_store(&in, f, 29.0f));  // past the 29 rates
  NEED(model::find_param(inst_id(0, ip::mod(0, ip::LfoSpeed)), f));
  CHECK(model::param_store(&in, f, 23.0f));
  CHECK_FALSE(model::param_store(&in, f, 24.0f));  // the amp slot has 24 (CONSTRAIN_SPEED_AMP_ENVELOPE 23)
  NEED(model::find_param(inst_id(0, ip::mod(2, ip::LfoShape)), f));
  CHECK(model::param_store(&in, f, 5.0f));  // Sine (OS-LFOSINE)
  CHECK_FALSE(model::param_store(&in, f, 6.0f));
  CHECK(in.sampler.lfo[2].shape == 5);
  CHECK(!model::validate_instrument(in, 0));
}

// ---- SOURCE = enable + loop (the instrument file's flags) ----------------------------------------------------------------------------------------------------------------

TEST_CASE("mod source: Off, Envelope and LFO are the enable and loop bytes, and the page reads them back") {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  for (unsigned e = 0; e < ip::kEnvelopes; ++e) {
    model::ParamField f;
    NEED(model::find_param(inst_id(0, ip::mod(e, ip::ModSource)), f));
    const model::Envelope before = in.sampler.envelope[e];
    CHECK(model::param_store(&in, f, 2.0f));  // LFO: enabled, loop set (Envelope -> LFO sets loop)
    CHECK((in.sampler.envelope[e].enable == 1 && in.sampler.envelope[e].loop == 1));
    CHECK(model::param_load(&in, f) == 2.0f);
    CHECK(model::param_store(&in, f, 1.0f));  // Envelope: enabled, no loop
    CHECK((in.sampler.envelope[e].enable == 1 && in.sampler.envelope[e].loop == 0));
    CHECK(model::param_load(&in, f) == 1.0f);
    CHECK(model::param_store(&in, f, 0.0f));  // Off: both clear
    CHECK((in.sampler.envelope[e].enable == 0 && in.sampler.envelope[e].loop == 0));
    CHECK(model::param_load(&in, f) == 0.0f);
    CHECK_FALSE(model::param_store(&in, f, 5.0f));  // past WT LFO
    CHECK_FALSE(model::param_store(&in, f, -1.0f));
    // nothing else of the envelope moved
    const model::Envelope now = in.sampler.envelope[e];
    CHECK((now.amount == before.amount && now.delay == before.delay && now.attack == before.attack && now.hold == before.hold && now.decay == before.decay));
    CHECK((now.sustain == before.sustain && now.release == before.release));
  }
  // loop set on a slot that is not enabled (an OG file can hold it: the fixture's envelope 1) reads as Off; the page never shows an LFO that does not run
  in.sampler.envelope[1].enable = 0;
  in.sampler.envelope[1].loop = 1;
  model::ParamField f;
  NEED(model::find_param(inst_id(0, ip::mod(1, ip::ModSource)), f));
  CHECK(model::param_load(&in, f) == 0.0f);
}

TEST_CASE("mod source: the SOURCE row steps up and down through its states") {
  // From each state, one click up / down on the row gives the neighbour SOURCE value, and the same two bytes.
  for (unsigned state = 0; state < 3; ++state) {
    for (const bool up : {true, false}) {
      if (state == 2 && up) {
        continue;  // (past LFO the list goes on: WT Env, WT Lfo, OS-CEWTENV W7a)
      }
      model::Envelope og{1.0f, 0, 3000, 0, 0, 1.0f, 1000, static_cast<std::uint8_t>(state == 2 ? 1 : 0), static_cast<std::uint8_t>(state != 0 ? 1 : 0)};
      model::Instrument in;
      model::init_instrument(in, model::InstrumentKind::Sample);
      in.sampler.envelope[2] = og;
      og_env_state(og, up);
      const core::ParamDesc* d = model::param_desc(inst_id(0, ip::mod(2, ip::ModSource)));
      NEED(d != nullptr);
      model::ParamField f;
      NEED(model::find_param(inst_id(0, ip::mod(2, ip::ModSource)), f));
      const float now = model::param_load(&in, f);
      CHECK(model::param_store(&in, f, core::param_step(*d, now, up ? 1 : -1)));
      CHECK_MESSAGE((in.sampler.envelope[2].enable == og.enable && in.sampler.envelope[2].loop == og.loop), "state " << state << (up ? " up" : " down"));
    }
  }
}

// ---- the page ------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("mod page: the rows follow the source; the encoder on SOURCE walks Off, Envelope, LFO, one undo unit each") {
  Rig rig;
  rig.go_mod(1);  // PANNING: off in a fresh instrument
  NEED(rig.rows().size() == 6u);
  CHECK(rig.rows()[0] == ip::mod(1, ip::ModSource));
  CHECK(rig.rows()[1] == ip::envelope(1, ip::EnvAmount));
  CHECK(rig.rows()[5] == ip::envelope(1, ip::EnvRelease));  // Off shows the envelope's rows (dimmed)
  CHECK(app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()) == inst_id(kUsed, ip::mod(1, ip::ModSource)));
  const std::size_t depth = rig.editor->journal().depth();
  rig.turn(1);  // Envelope
  CHECK(rig.value(ip::mod(1, ip::ModSource)) == 1.0f);
  CHECK((rig.inst()->sampler.envelope[1].enable == 1 && rig.inst()->sampler.envelope[1].loop == 0));
  CHECK(rig.editor->journal().depth() == depth + 1u);
  NEED(rig.rows().size() == 6u);
  rig.pass(1600);  // (OS-UNDOGRP: clicks within 1500 ms on one row are one undo unit)
  rig.turn(1);  // LFO
  CHECK(rig.value(ip::mod(1, ip::ModSource)) == 2.0f);
  CHECK((rig.inst()->sampler.envelope[1].enable == 1 && rig.inst()->sampler.envelope[1].loop == 1));
  CHECK(rig.editor->journal().depth() == depth + 2u);
  NEED(rig.rows().size() == 4u);
  CHECK(rig.rows()[1] == ip::mod(1, ip::LfoShape));
  CHECK(rig.rows()[2] == ip::mod(1, ip::LfoSpeed));
  CHECK(rig.rows()[3] == ip::mod(1, ip::LfoAmount));
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);  // (the WT sources past LFO are the cases of "wt mod page" below)
  CHECK(rig.value(ip::mod(1, ip::ModSource)) == 2.0f);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.value(ip::mod(1, ip::ModSource)) == 1.0f);
  CHECK(rig.rows().size() == 6u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.value(ip::mod(1, ip::ModSource)) == 0.0f);
  CHECK((rig.inst()->sampler.envelope[1].enable == 0 && rig.inst()->sampler.envelope[1].loop == 0));
  // the LFO rows edit the stored LFO of that destination (and no other)
  rig.invoke("edit.redo");
  rig.invoke("edit.redo");
  rig.tap(Button::Down);
  rig.turn(2);  // SHAPE: Triangle (the default) -> Random
  CHECK(rig.inst()->sampler.lfo[1].shape == 4);
  rig.tap(Button::Down);
  rig.turn(5);  // SPEED
  CHECK(rig.inst()->sampler.lfo[1].speed == 5);
  rig.tap(Button::Down);
  rig.turn(-20);  // AMOUNT 0.5 -> 0.3
  CHECK(rig.inst()->sampler.lfo[1].amount == doctest::Approx(0.3f));
  CHECK(rig.inst()->sampler.lfo[0].shape == 2);
  CHECK(rig.inst()->sampler.lfo[2].shape == 2);
  CHECK(rig.app->failed_frames() == 0u);
}

TEST_CASE("mod page: Delete puts a row back to its default; the volume destination starts as an Envelope, the others Off") {
  Rig rig;
  rig.go_mod(0);
  CHECK(rig.value(ip::mod(0, ip::ModSource)) == 1.0f);
  CHECK(rig.rows().size() == 6u);
  rig.turn(1);  // LFO on the amp
  CHECK(rig.value(ip::mod(0, ip::ModSource)) == 2.0f);
  rig.tap(Button::Delete);  // SOURCE back to its default: Envelope
  CHECK(rig.value(ip::mod(0, ip::ModSource)) == 1.0f);
  CHECK((rig.inst()->sampler.envelope[0].enable == 1 && rig.inst()->sampler.envelope[0].loop == 0));
  rig.go_mod(2);
  CHECK(rig.value(ip::mod(2, ip::ModSource)) == 0.0f);
}

TEST_CASE("mod page: every destination draws in every mode, with a project and without one") {
  Rig rig;
  for (unsigned e = 0; e < ip::kEnvelopes; ++e) {
    for (unsigned source = 0; source < 5; ++source) {  // (3 and 4: WT Env and WT Lfo, which draw the contour: OS-CEWTENV W7b)
      NEED(rig.editor->set_param(inst_id(kUsed, ip::mod(e, ip::ModSource)), static_cast<float>(source)) == model::Status::Ok);
      rig.go_mod(e);
      for (unsigned shape = 0; shape < 5 && source == 2; ++shape) {
        NEED(rig.editor->set_param(inst_id(kUsed, ip::mod(e, ip::LfoShape)), static_cast<float>(shape)) == model::Status::Ok);
        rig.pass();
      }
      rig.pass();
    }
  }
  CHECK(rig.app->failed_frames() == 0u);
}

// ---- dimming: never hidden, always a reason (O8) --------------------------------------------------------------------------------------------------------

TEST_CASE("mod page dims the destinations the instrument does not use, with the reason, and never hides a row") {
  Rig rig;
  // ONE SHOT, filter off: the wavetable position needs WAVETABLE, the granular position GRANULAR, the cutoff a filter that is on
  rig.go_mod(3);
  CHECK(rig.rows().size() == 6u);  // the rows are all there
  for (unsigned row = 0; row < rig.rows().size(); ++row) {
    CHECK(std::string(rig.why(row) != nullptr ? rig.why(row) : "") == "WAVETABLE OR GRANULAR");  // SOURCE too: the destination is not used
  }
  rig.go_mod(4);
  CHECK(std::string(rig.why(0) != nullptr ? rig.why(0) : "") == "GRANULAR ONLY");
  rig.go_mod(2);
  CHECK(std::string(rig.why(0) != nullptr ? rig.why(0) : "") == "FILTER OFF");
  CHECK(std::string(rig.why(5) != nullptr ? rig.why(5) : "") == "FILTER OFF");
  // the destinations that are always used: volume, panning, fine tune
  for (const unsigned e : {0u, 1u, 5u}) {
    rig.go_mod(e);
    CHECK(rig.why(0) == nullptr);
  }
  // the filter on: the cutoff is used
  NEED(rig.editor->set_param(inst_id(kUsed, ip::FilterEnable), 1.0f) == model::Status::Ok);
  rig.go_mod(2);
  CHECK(rig.why(0) == nullptr);
  // WAVETABLE: the wavetable position is used, the granular one still is not; GRANULAR the other way round
  CHECK(rig.invoke("instrument.play_mode", 6) == DispatchResult::Changed);
  rig.go_mod(3);
  CHECK(rig.why(0) == nullptr);
  rig.go_mod(4);
  CHECK(std::string(rig.why(0) != nullptr ? rig.why(0) : "") == "GRANULAR ONLY");
  CHECK(rig.invoke("instrument.play_mode", 7) == DispatchResult::Changed);
  rig.go_mod(4);
  CHECK(rig.why(0) == nullptr);
  rig.go_mod(3);
  CHECK(rig.why(0) == nullptr);  // OS-CEWTENV W7b: slot 3 is the grain LENGTH on a granular instrument
  // a SOURCE that is Off dims the rows under it (not SOURCE itself), and says so
  rig.go_mod(1);
  CHECK(rig.why(0) == nullptr);
  CHECK(std::string(rig.why(1) != nullptr ? rig.why(1) : "").find("SOURCE OFF") == 0u);
  rig.turn(1);  // Envelope: all rows apply
  CHECK(rig.why(1) == nullptr);
  // the destination's reason wins over the source's
  CHECK(rig.invoke("instrument.play_mode", 0) == DispatchResult::Changed);
  rig.go_mod(3);
  CHECK(std::string(rig.why(1) != nullptr ? rig.why(1) : "") == "WAVETABLE OR GRANULAR");
}

TEST_CASE("mod page: a dimmed row is still editable (dimmed, not locked)") {
  Rig rig;
  rig.go_mod(3);  // WAVETABLE OR GRANULAR, on a one shot
  CHECK(rig.why(0) != nullptr);
  rig.turn(2);
  CHECK(rig.value(ip::mod(3, ip::ModSource)) == 2.0f);
}

// ---- OS-CEWTENV W7b: slot 3 is the grain LENGTH on a granular instrument (enable bit 0x02), the wavetable position on a wavetable (0x01) ------------------------

TEST_CASE("mod page: slot 3 on a granular instrument writes the grain length bit (2), on a wavetable the position bit (1), and the bit follows the play mode") {
  Rig rig;
  const auto enable = [&] { return rig.inst()->sampler.envelope[3].enable; };
  const core::ParamId src = inst_id(kUsed, ip::mod(3, ip::ModSource));
  CHECK(rig.invoke("instrument.play_mode", 7) == DispatchResult::Changed);
  NEED(rig.editor->set_param(src, 1.0f) == model::Status::Ok);
  CHECK(enable() == 2);                              // GRAN LENGTH: bit 0x02, which is what the engine reads on a granular instrument
  CHECK(rig.value(ip::mod(3, ip::ModSource)) == 1.0f);  // and the row still reads Envelope
  NEED(rig.editor->set_param(src, 3.0f) == model::Status::Ok);  // WT Env
  CHECK(enable() == 2);
  CHECK(rig.inst()->sampler.envelope[3].loop == 2);
  CHECK(rig.invoke("instrument.play_mode", 6) == DispatchResult::Changed);  // to a wavetable: the bit moves to the position
  CHECK(enable() == 1);
  CHECK(rig.invoke("instrument.play_mode", 7) == DispatchResult::Changed);  // and back
  CHECK(enable() == 2);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // the mode change is one unit, the bit goes with it
  CHECK(enable() == 1);  // the undo left the wavetable mode in place
  CHECK(rig.inst()->sampler.play_mode == 6);
  NEED(rig.editor->set_param(src, 2.0f) == model::Status::Ok);
  CHECK(enable() == 1);  // a wavetable: the position bit
  NEED(rig.editor->set_param(src, 0.0f) == model::Status::Ok);
  CHECK(enable() == 0);
  CHECK(rig.invoke("instrument.play_mode", 7) == DispatchResult::Changed);
  CHECK(enable() == 0);  // Off stays Off
}

TEST_CASE("mod page: the slot 3 header says Gran Length on a granular instrument") {
  Rig rig;
  CHECK(rig.invoke("instrument.play_mode", 7) == DispatchResult::Changed);
  rig.go_mod(3);
  rig.pass();
  CHECK(rig.app->failed_frames() == 0u);
  CHECK(rig.why(0) == nullptr);
}

TEST_CASE("mod page: a loaded granular instrument whose slot 3 enable is 1 reads SOURCE Off (the engine reads bit 2 only), the bytes stay, and a wavetable reads 1 and 2 as on") {
  Rig rig;
  model::Instrument in = *rig.inst();
  in.sampler.play_mode = 7;
  in.sampler.envelope[3].enable = 1;  // an OG file or an older project: the position bit on a granular instrument
  in.sampler.envelope[3].loop = 0;
  NEED(rig.editor->set_instrument(kUsed, in) == model::Status::Ok);
  const core::ParamId src = inst_id(kUsed, ip::mod(3, ip::ModSource));
  CHECK(rig.arena->project.get_param(src) == 0.0f);  // plays nothing, so the page says Off
  CHECK(rig.inst()->sampler.envelope[3].enable == 1);  // a loaded byte is not rewritten
  CHECK(rig.value(ip::mod(3, ip::ModSource)) == 0.0f);
  in.sampler.envelope[3].enable = 2;
  NEED(rig.editor->set_instrument(kUsed, in) == model::Status::Ok);
  CHECK(rig.arena->project.get_param(src) == 1.0f);
  in.sampler.play_mode = 6;
  in.sampler.envelope[3].enable = 2;
  NEED(rig.editor->set_instrument(kUsed, in) == model::Status::Ok);
  CHECK(rig.arena->project.get_param(src) == 1.0f);  // any other mode: 1 or 2 is on
  in.sampler.envelope[3].enable = 1;
  NEED(rig.editor->set_instrument(kUsed, in) == model::Status::Ok);
  CHECK(rig.arena->project.get_param(src) == 1.0f);
  in.sampler.play_mode = 7;  // back to granular with enable 1 and set the source: the write sets the right bit
  NEED(rig.editor->set_instrument(kUsed, in) == model::Status::Ok);
  NEED(rig.editor->set_param(src, 1.0f) == model::Status::Ok);
  CHECK(rig.inst()->sampler.envelope[3].enable == 2);
}

// ---- the engine: the LFO is audible, and an edit reaches it at the handover, through undo and redo ------------------------------------------------------------

TEST_CASE("mod engine: SOURCE and the LFO rows reach the voices' descriptor when the slot is rebound, and undo and redo carry them") {
  Rig rig;
  NEED(rig.editor->set_param(inst_id(kUsed, ip::FilterEnable), 1.0f) == model::Status::Ok);
  rig.settle();
  rig.bank.request_rebind(kUsed);
  rig.settle();
  audio::SampleDesc d = rig.voices->sample(kUsed);
  CHECK_FALSE(d.filter.mod.lfo);
  CHECK(d.filter.mod.kind == audio::kFmodNone);
  // the cutoff's destination (slot 2) as an LFO: Triangle -> Square (3), rate 7 ("12 steps"), amount 0.8
  rig.go_mod(2);
  rig.turn(2);  // SOURCE: LFO
  CHECK(rig.bank.pending(kUsed));  // the page's edit marks the slot: it is the handover that applies it (stopped, no live note, voices silent)
  rig.tap(Button::Down);
  rig.turn(1);  // SHAPE Square
  rig.tap(Button::Down);
  rig.turn(7);  // SPEED
  rig.tap(Button::Down);
  rig.turn(30);  // AMOUNT 0.5 -> 0.8
  rig.settle();
  d = rig.voices->sample(kUsed);
  CHECK(d.filter.mod.lfo);
  CHECK(d.filter.mod.kind == audio::kFmodLfo);
  CHECK(d.filter.mod.shape == 3);
  CHECK(d.filter.mod.rate == 7);
  CHECK(d.filter.mod.amount == doctest::Approx(0.8f));
  // the amp (slot 0) as an LFO is the tremolo: the plain gain stays, the rate is the amp table's
  rig.go_mod(0);
  rig.turn(1);  // Envelope -> LFO
  rig.tap(Button::Down);
  rig.turn(-1);  // SHAPE Triangle -> Saw
  rig.tap(Button::Down);
  rig.turn(30);  // SPEED, held to the amp's 24 rates (0..23)
  rig.settle();
  d = rig.voices->sample(kUsed);
  CHECK(d.env.lfo);
  CHECK(d.amp_mod.lfo);
  CHECK(d.amp_mod.shape == 1);
  CHECK(d.amp_mod.rate == 23);
  CHECK_FALSE(d.env.enable);  // an LFO slot is not an ADSR
  // undo takes them back, redo brings them again: the unit is a parameter edit, the slot is rebound by the applied list
  const std::size_t depth = rig.editor->journal().depth();
  for (std::size_t i = 0; i < depth; ++i) {
    CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  }
  CHECK(rig.bank.pending(kUsed));
  rig.settle();
  d = rig.voices->sample(kUsed);
  CHECK_FALSE(d.filter.mod.lfo);
  CHECK_FALSE(d.amp_mod.lfo);
  CHECK(d.env.enable);
  for (std::size_t i = 0; i < depth; ++i) {
    CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  }
  CHECK(rig.bank.pending(kUsed));
  rig.settle();
  d = rig.voices->sample(kUsed);
  CHECK(d.filter.mod.lfo);
  CHECK(d.filter.mod.shape == 3);
  CHECK(d.filter.mod.rate == 7);
  CHECK(d.amp_mod.lfo);
  CHECK(d.amp_mod.rate == 23);
}

TEST_CASE("mod engine: an LFO-only edit (the speed of a slot that already is an LFO) marks the slot too, so the unit and the engine agree after undo") {
  Rig rig;
  NEED(rig.editor->set_param(inst_id(kUsed, ip::FilterEnable), 1.0f) == model::Status::Ok);
  NEED(rig.editor->set_param(inst_id(kUsed, ip::mod(2, ip::ModSource)), 2.0f) == model::Status::Ok);
  rig.bank.request_rebind(kUsed);
  rig.settle();
  CHECK(rig.voices->sample(kUsed).filter.mod.rate == 0);
  rig.go_mod(2, 2);  // SPEED
  rig.turn(9);
  CHECK(rig.bank.pending(kUsed));
  rig.settle();
  CHECK(rig.voices->sample(kUsed).filter.mod.rate == 9);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.bank.pending(kUsed));
  rig.settle();
  CHECK(rig.voices->sample(kUsed).filter.mod.rate == 0);
}

// ---- the S5 review: INIT, and a wavetable window the engine does not play -------------------------------------------------------------------------------------

// ---- OS-CEWTENV W7a: the WT ENV and WT LFO sources and their rows -------------------------------------------------------------------------------------------------------------------

TEST_CASE("wt mod page: SOURCE cycles Off, ENV, LFO, WT ENV, WT LFO, and each source has its rows") {
  Rig rig;
  rig.go_mod(1);
  const std::size_t base = rig.editor->journal().depth();
  const char* const words[5] = {"Off", "Envelope", "LFO", "WT Env", "WT LFO"};
  app::ParamScratch scratch;
  const core::ParamDesc* d = app::describe_param(rig.app->model(), inst_id(kUsed, ip::mod(1, ip::ModSource)), scratch);
  NEED(d != nullptr);
  for (unsigned s = 0; s < 5; ++s) {
    CHECK(std::string(d->names[s]) == words[s]);
  }
  CHECK(d->max == 4.0f);
  for (unsigned s = 1; s < 5; ++s) {
    rig.pass(1600);  // a pause past the 1.5 s window starts a new undo step
    rig.turn(1);
    CHECK(rig.value(ip::mod(1, ip::ModSource)) == static_cast<float>(s));
  }
  CHECK(rig.editor->journal().depth() == base + 4);  // one undo unit per step
  const model::SamplerParams& sp = rig.inst()->sampler;
  CHECK((sp.envelope[1].enable == 1 && sp.envelope[1].loop == 3));  // WT LFO is loop 3
  rig.turn(1);                                                       // the end of the list stays
  CHECK(rig.value(ip::mod(1, ip::ModSource)) == 4.0f);
  // the rows of WT LFO: SOURCE, WAVE, SPEED, WARP X, WARP Y, AMOUNT
  std::vector<std::uint16_t> r = rig.rows();
  NEED(r.size() == 6);
  CHECK(r[0] == ip::mod(1, ip::ModSource));
  CHECK(r[1] == ip::wt(1, ip::WtWave));
  CHECK(r[2] == ip::mod(1, ip::LfoSpeed));
  CHECK(r[3] == ip::wt(1, ip::WtWarpX));
  CHECK(r[4] == ip::wt(1, ip::WtWarpY));
  CHECK(r[5] == ip::mod(1, ip::LfoAmount));
  rig.turn(-1);  // WT ENV: SOURCE, WAVE, DURATION, WARP X, WARP Y, SUSTAIN, RELEASE, AMOUNT
  CHECK((sp.envelope[1].enable == 1 && sp.envelope[1].loop == 2));
  r = rig.rows();
  NEED(r.size() == 8);
  CHECK(r[1] == ip::wt(1, ip::WtWave));
  CHECK(r[2] == ip::wt(1, ip::WtDuration));
  CHECK(r[3] == ip::wt(1, ip::WtWarpX));
  CHECK(r[4] == ip::wt(1, ip::WtWarpY));
  CHECK(r[5] == ip::wt(1, ip::WtSustain));
  CHECK(r[6] == ip::envelope(1, ip::EnvRelease));
  CHECK(r[7] == ip::mod(1, ip::LfoAmount));  // the depth the sound reads (put_mod: lfo[e].amount), not the envelope's amount
  rig.turn(-1);  // LFO: SOURCE, SHAPE, SPEED, AMOUNT
  CHECK(rig.rows().size() == 4);
  CHECK(rig.rows()[1] == ip::mod(1, ip::LfoShape));
  rig.turn(-1);  // ENV
  for (const std::uint16_t f : rig.rows()) {
    CHECK_FALSE(ip::is_wt_field(f));
  }
}

TEST_CASE("wt mod page: a WT ENV's AMOUNT row is the slot's lfo amount (what the sound reads); the amp slot has no AMOUNT row") {
  Rig rig;
  NEED(rig.editor->set_param(inst_id(kUsed, ip::mod(1, ip::ModSource)), 3.0f) == model::Status::Ok);  // WT ENV on slot 1
  NEED(rig.editor->set_param(inst_id(kUsed, ip::mod(0, ip::ModSource)), 3.0f) == model::Status::Ok);  // and on the amp slot
  rig.settle();
  const model::SamplerParams& sp = rig.inst()->sampler;
  rig.go_mod(1, 7);
  const auto env_amount = sp.envelope[1].amount;
  const float before = rig.value(ip::mod(1, ip::LfoAmount));
  rig.turn(5);
  CHECK(rig.value(ip::mod(1, ip::LfoAmount)) != before);
  CHECK(sp.envelope[1].amount == env_amount);  // the envelope's own amount is not touched
  rig.go_mod(0);
  const std::vector<std::uint16_t> r = rig.rows();
  CHECK(r.size() == 7u);
  for (const std::uint16_t f : r) {
    CHECK(f != ip::mod(0, ip::LfoAmount));
    CHECK(f != ip::envelope(0, ip::EnvAmount));
  }
}

TEST_CASE("wt mod page: the WT fields read and write the instrument, one undo unit each, and the slot is rebound") {
  Rig rig;
  NEED(rig.editor->set_param(inst_id(kUsed, ip::mod(2, ip::ModSource)), 3.0f) == model::Status::Ok);  // WT ENV on slot 2
  rig.settle();
  auto model = std::make_unique<app::Model>();
  app::ViewState view;
  app::ActionContext ctx{*model, view, app::App::kSoftKeyLabels, 1000, true, &rig.app->session()};
  ctx.edit = rig.editor.get();
  ctx.samples = &rig.bank;
  const model::SamplerParams& sp = rig.inst()->sampler;
  const std::size_t depth = rig.editor->journal().depth();
  const std::uint32_t gen = rig.arena->project.generation();
  const auto decay0 = sp.envelope[2].decay;
  // WAVE: the contour byte of lfo[2]
  const core::ParamId wave = inst_id(kUsed, ip::wt(2, ip::WtWave));
  CHECK(app::write_param(ctx, wave, 77.0f) == app::WriteResult::Changed);
  CHECK(sp.lfo[2].shape == 77);
  CHECK(rig.bank.pending(kUsed));  // the slot rebinds
  CHECK(rig.editor->journal().depth() == depth + 1);
  CHECK(rig.arena->project.generation() != gen);
  rig.settle();
  CHECK_FALSE(rig.bank.pending(kUsed));
  CHECK(app::write_param(ctx, wave, 77.0f) == app::WriteResult::Unchanged);
  CHECK(rig.editor->journal().depth() == depth + 1);
  // DURATION: the slot's decay in ms
  const core::ParamId dur = inst_id(kUsed, ip::wt(2, ip::WtDuration));
  CHECK(app::write_param(ctx, dur, 20000.0f) == app::WriteResult::Changed);
  CHECK(sp.envelope[2].decay == 20000);
  CHECK(app::write_param(ctx, dur, 20010.0f) == app::WriteResult::Rejected);
  CHECK(sp.envelope[2].decay == 20000);
  CHECK(app::write_param(ctx, dur, 1.0f) == app::WriteResult::Rejected);  // 0 and 1 play as 1000 ms: the page starts at 2
  CHECK(app::write_param(ctx, dur, 2.0f) == app::WriteResult::Changed);
  CHECK(sp.envelope[2].decay == 2);
  // WARP X and Y share the byte: X the high nibble, Y the low one; -7..+7 by the nibble law (9..15 are -7..-1)
  const core::ParamId wx = inst_id(kUsed, ip::wt(2, ip::WtWarpX));
  const core::ParamId wy = inst_id(kUsed, ip::wt(2, ip::WtWarpY));
  CHECK(app::write_param(ctx, wx, 3.0f) == app::WriteResult::Changed);
  CHECK(app::write_param(ctx, wy, -7.0f) == app::WriteResult::Changed);
  CHECK(sp.lfo[2].wt_bend == ((3u << 4) | 9u));
  CHECK(model::wt_param_value(*rig.inst(), ip::wt(2, ip::WtWarpY)) == -7.0f);
  CHECK(model::wt_param_value(*rig.inst(), ip::wt(2, ip::WtWarpX)) == 3.0f);
  CHECK(app::write_param(ctx, wx, -1.0f) == app::WriteResult::Changed);
  CHECK(sp.lfo[2].wt_bend == ((15u << 4) | 9u));
  CHECK(app::write_param(ctx, wx, 8.0f) == app::WriteResult::Rejected);
  CHECK(app::write_param(ctx, wy, std::numeric_limits<float>::quiet_NaN()) == app::WriteResult::Rejected);
  // SUSTAIN: OFF (0) or 1..99
  const core::ParamId sus = inst_id(kUsed, ip::wt(2, ip::WtSustain));
  CHECK(app::write_param(ctx, sus, 99.0f) == app::WriteResult::Changed);
  CHECK(sp.lfo[2].wt_sust == 99);
  CHECK(app::write_param(ctx, sus, 100.0f) == app::WriteResult::Rejected);
  CHECK(app::write_param(ctx, sus, 0.0f) == app::WriteResult::Changed);
  CHECK(sp.lfo[2].wt_sust == 0);
  // eight changes, eight undo units; undoing them all restores the instrument's bytes
  CHECK(rig.editor->journal().depth() == depth + 8);
  for (std::size_t i = 0; i < 8; ++i) {
    CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  }
  CHECK(rig.editor->journal().depth() == depth);
  CHECK(sp.lfo[2].wt_bend == 0);
  CHECK(sp.envelope[2].decay == decay0);
}

TEST_CASE("wt mod page: the WT fields are described but are no table row (not learnable), refuse a MIDI slot, and a stale contour reads as a Triangle once the slot is an LFO") {
  Rig rig;
  const core::ParamId id = inst_id(kUsed, ip::wt(0, ip::WtDuration));
  app::ParamScratch scratch;
  const core::ParamDesc* d = app::describe_param(rig.app->model(), id, scratch);
  NEED(d != nullptr);
  CHECK(std::string(d->name) == "DURATION");
  CHECK(model::param_desc(id) == nullptr);
  CHECK_FALSE(model::instrument_field_app_writable(ip::wt(0, ip::WtDuration)));
  CHECK(app::is_wt_param(id));
  CHECK_FALSE(app::is_wt_param(inst_id(kUsed, ip::mod(0, ip::LfoShape))));
  auto model = std::make_unique<app::Model>();
  app::ViewState view;
  app::ActionContext ctx{*model, view, app::App::kSoftKeyLabels, 1000, true, &rig.app->session()};
  ctx.edit = rig.editor.get();
  ctx.samples = &rig.bank;
  CHECK(app::write_param(ctx, inst_id(9, ip::wt(0, ip::WtWave)), 5.0f) == app::WriteResult::NoInstrument);  // the MIDI slot of the rig
  // WT LFO with a contour 77, then LFO: the stored byte stays, the page reads the Triangle
  NEED(rig.editor->set_param(inst_id(kUsed, ip::mod(3, ip::ModSource)), 4.0f) == model::Status::Ok);
  CHECK(app::write_param(ctx, inst_id(kUsed, ip::wt(3, ip::WtWave)), 77.0f) == app::WriteResult::Changed);
  NEED(rig.editor->set_param(inst_id(kUsed, ip::mod(3, ip::ModSource)), 2.0f) == model::Status::Ok);
  const core::ParamId shape = inst_id(kUsed, ip::mod(3, ip::LfoShape));
  float shown = -1.0f;
  CHECK(app::read_param(rig.app->model(), shape, shown));
  CHECK(shown == static_cast<float>(model::kLfoShapeTriangle));
}

TEST_CASE("init: the envelope delay, hold and loop and the LFOs go back to the defaults too; kind, sample, slices, points, name stay") {
  Rig rig;
  model::Instrument edited = *rig.inst();
  edited.sampler.start = 1000;
  edited.sampler.end = 60000;
  edited.sampler.slice_count = 2;
  edited.sampler.slices[0] = 20000;
  edited.sampler.slices[1] = 40000;
  for (unsigned e = 0; e < model::kEnvelopes; ++e) {
    edited.sampler.envelope[e].delay = static_cast<std::uint16_t>(100 + e);
    edited.sampler.envelope[e].hold = static_cast<std::uint16_t>(200 + e);
    edited.sampler.envelope[e].loop = 1;
    edited.sampler.envelope[e].enable = 1;
    edited.sampler.lfo[e] = model::Lfo{static_cast<std::uint8_t>(4 - e % 5), static_cast<std::uint8_t>(10 + e), 0, 0, 0.9f};
  }
  NEED(rig.editor->set_instrument(kUsed, edited) == model::Status::Ok);
  rig.shift_tap(Button::Delete);
  const model::Instrument& now = *rig.inst();
  model::Instrument fresh;
  model::init_instrument(fresh, model::InstrumentKind::Sample);
  for (unsigned e = 0; e < model::kEnvelopes; ++e) {
    CHECK_MESSAGE(std::memcmp(&now.sampler.envelope[e], &fresh.sampler.envelope[e], sizeof(model::Envelope)) == 0, "envelope " << e);
    CHECK(now.sampler.envelope[e].delay == 0);
    CHECK(now.sampler.envelope[e].hold == 0);
    CHECK_MESSAGE((now.sampler.lfo[e].shape == fresh.sampler.lfo[e].shape && now.sampler.lfo[e].speed == fresh.sampler.lfo[e].speed && now.sampler.lfo[e].amount == fresh.sampler.lfo[e].amount), "lfo " << e);
  }
  CHECK(now.sampler.envelope[0].enable == 1);  // the volume envelope's default
  CHECK(now.sampler.envelope[0].loop == 0);
  CHECK(now.kind == model::InstrumentKind::Sample);
  CHECK(std::strcmp(now.name, "KICK") == 0);
  CHECK(std::strcmp(now.sample_path, "samples/kick.wav") == 0);
  CHECK((now.sampler.start == 1000 && now.sampler.end == 60000));
  CHECK((now.sampler.slice_count == 2 && now.sampler.slices[0] == 20000 && now.sampler.slices[1] == 40000));
  CHECK(rig.bank.pcm(kUsed).frames == kFrames);
  // one undo unit takes the lot back
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(std::memcmp(&rig.inst()->sampler, &edited.sampler, sizeof edited.sampler) == 0);
}

TEST_CASE("wavetable: a stored window size the sample cannot hold plays nothing, and the page says so instead of drawing a window") {
  // 1500 frames, a stored size of 2048 (valid): the engine plays nothing (og_samples.cpp, OS-007h), whatever wt_fit_size would have fitted
  CHECK(app::wt_engine_size(2048, 1500) == 2048);
  CHECK(app::wt_play_size(2048, 1500) == 0);
  CHECK_FALSE(app::wt_window_frames(1500, 2048, 0).valid);
  CHECK(app::wt_play_size(1024, 1500) == 1024);
  CHECK(app::wt_window_frames(1500, 1024, 0).valid);
  CHECK(app::wt_play_size(0, 1500) == 1024);   // unset: fitted, as the engine does
  CHECK(app::wt_play_size(100, 1500) == 1024);  // not a power of two: fitted
  CHECK(app::wt_play_size(0, 31) == 0);
  CHECK(app::wt_play_size(32, 20) == 0);
  CHECK(app::wt_play_size(2048, 0) == 2048);  // no sample known: as the engine would play it
  Rig rig(1500);
  model::Instrument in = *rig.inst();
  in.sampler.play_mode = 6;
  in.sampler.sample_type = 1;
  in.sampler.wavetable_window_size = 2048;
  NEED(rig.editor->set_instrument(kUsed, in) == model::Status::Ok);
  rig.bank.request_rebind(kUsed);
  rig.settle();
  CHECK(rig.voices->sample(kUsed).wt_size == 0u);  // nothing plays
  CHECK(rig.voices->sample(kUsed).length == 0u);
  rig.invoke("param.group", ip::GroupSource);
  rig.tap(Button::Down);  // WT POS
  char text[core::kParamTextChars + 1];
  app::params_source_text(rig.app->model(), kUsed, ip::WtSize, text);
  CHECK(std::string(text) == "2048");  // the stored size, as the engine reads it (not a fitted 1024 over a silent instrument)
  rig.pass();
  CHECK(rig.app->failed_frames() == 0u);
  // WT SIZE is the way out: one step down is a size the sample holds
  rig.tap(Button::Down);  // WT SIZE
  rig.turn(-1);
  CHECK(rig.inst()->sampler.wavetable_window_size == 1024);
  rig.bank.request_rebind(kUsed);
  rig.settle();
  CHECK(rig.voices->sample(kUsed).wt_size == 1024u);
}

TEST_CASE("mod source: Off on a stored enable 0 / loop 1 (which reads as Off) still rewrites the loop byte, and is an edit: undo record, generation, dirty") {
  Rig rig;
  model::Instrument in = *rig.inst();
  in.sampler.envelope[1].enable = 0;
  in.sampler.envelope[1].loop = 1;  // what an OG file can hold; the page reads it as Off
  NEED(rig.editor->set_instrument(kUsed, in) == model::Status::Ok);
  const core::ParamId id = inst_id(kUsed, ip::mod(1, ip::ModSource));
  CHECK(rig.arena->project.get_param(id) == 0.0f);
  const std::uint32_t generation = rig.arena->project.param_generation(id);
  const std::size_t depth = rig.editor->journal().depth();
  const std::uint32_t whole = rig.arena->project.generation();  // (the project's own change counter is what the saved / modified state follows)
  CHECK(rig.editor->set_param(id, 0.0f) == model::Status::Ok);
  CHECK((rig.inst()->sampler.envelope[1].enable == 0 && rig.inst()->sampler.envelope[1].loop == 0));  // canonical now
  CHECK(rig.arena->project.param_generation(id) != generation);
  CHECK(rig.editor->journal().depth() == depth + 1u);
  CHECK(rig.arena->project.generation() != whole);
  // an Off that already is canonical is no edit
  const std::uint32_t after = rig.arena->project.param_generation(id);
  CHECK(rig.editor->set_param(id, 0.0f) == model::Status::Ok);
  CHECK(rig.arena->project.param_generation(id) == after);
  CHECK(rig.editor->journal().depth() == depth + 1u);
}

// OS-AUDIT E-INST-05: the FILTER page dims CUTOFF and RESONANCE (never hides a row) while the filter is off, and says why; ENABLE and TYPE stay lit.
TEST_CASE("filter page dims cutoff and resonance while the filter is off") {
  Rig rig;
  rig.invoke("param.group", ip::GroupFilter);
  rig.pass();
  std::uint16_t f[app::ParamView::kMaxRows] = {};
  const unsigned n = app::params_mod_rows(rig.app->model(), rig.app->state(), f);
  NEED(n >= 4u);
  CHECK(rig.value(ip::FilterEnable) == 0.0f);
  for (unsigned row = 0; row < n; ++row) {
    const char* why = app::params_filter_row_why(rig.app->model(), rig.app->state(), row);
    if (f[row] == ip::Cutoff || f[row] == ip::Resonance) {
      CHECK(std::string(why != nullptr ? why : "") == "FILTER OFF");
    } else {
      CHECK(why == nullptr);
    }
  }
  // the filter on: every row applies again
  NEED(rig.editor->set_param(inst_id(kUsed, ip::FilterEnable), 1.0f) == model::Status::Ok);
  rig.pass();
  for (unsigned row = 0; row < n; ++row) {
    CHECK(app::params_filter_row_why(rig.app->model(), rig.app->state(), row) == nullptr);
  }
  // the other pages never answer
  rig.invoke("param.group", ip::GroupVoice);
  rig.pass();
  CHECK(app::params_filter_row_why(rig.app->model(), rig.app->state(), 0) == nullptr);
}
