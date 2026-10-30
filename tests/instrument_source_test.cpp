// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S3: the Params F1 source page and the play mode. F1 on a sample instrument opens it; its rows follow the play mode (O8's exception) and never pass eight; PLAY MODE goes through the validating
// action (a wavetable needs a window the sample can hold, a grain stays a grain, a loop stays a loop, slices without slices say where to make them); the other rows are clamped to the sample; every edit is one
// undo unit that rebinds the slot. The tables behind it are param_source_test.cpp's, the window arithmetic wave_math_test.cpp's.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/play_mode.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/wave_math.h>
#include <o3os/audio/voices.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include "../app/src/param_access.h"  // write_param (private to app/src): the stereo test calls the write path itself

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
namespace ip = model::instrument_param;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];

constexpr std::uint32_t kFrames = 96000;
constexpr unsigned kSlot = 4;   // instrument 5: a sample of kFrames
constexpr unsigned kShort = 6;  // instrument 7: a sample of 20 frames
constexpr unsigned kEmpty = 8;  // instrument 9: a sample instrument with no audio

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

  Rig() : memory(400000), peaks(40000) {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), 400000, peaks.data(), peaks.size());
    for (const unsigned slot : {kSlot, kShort, kEmpty}) {
      model::Instrument in;
      model::init_instrument(in, model::InstrumentKind::Sample);
      std::strcpy(in.name, "SRC");
      NEED(arena->project.set_instrument(slot, in) == model::Status::Ok);
    }
    model::Instrument midi;
    model::init_instrument(midi, model::InstrumentKind::Midi);
    midi.midi_channel = 2;
    NEED(arena->project.set_instrument(9, midi) == model::Status::Ok);
    load(kSlot, kFrames);
    load(kShort, 20);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena->project, journal);
    app->attach_document(editor.get());
    app->attach_samples(&bank);
    select(kSlot);
    tap(Button::Params);
    tap(Button::F1);  // the source page
  }
  void load(unsigned slot, std::uint32_t frames) {
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < frames; ++i) {
      p[i] = static_cast<std::int16_t>((i % 64u) * 400 - 12000);
    }
    NEED(bank.commit_load(slot, p, frames));
    bank.rebind_now(slot);
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    pass(1);
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass(1);
  }
  void turn(int detents) {
    while (detents != 0) {
      const int step = detents > 20000 ? 20000 : (detents < -20000 ? -20000 : detents);
      input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(step)));
      pass(1);
      detents -= step;
    }
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  void select(unsigned slot) { (void)invoke("instrument.select", static_cast<ActionArg>(slot + 1)); }
  const model::Instrument& inst(unsigned slot = kSlot) const { return *arena->project.instrument(slot); }
  std::string note() const { return app->state().notify.empty() ? std::string() : std::string(app->state().notify.head().text.c_str()); }
  void clear_notes() {
    pass(10000);
    pass(10000);
  }
  unsigned page() const { return app->state().page[app::screen_index(app::ScreenId::Instrument)]; }
  int rows() const { return app::kParamsScreen.count(app->model(), app->state()); }
  core::ParamId focus() const { return app::kParamsScreen.focus_param(app->model(), app->state()); }
  // The field of row `row` of the page in view.
  unsigned field_at(int row) {
    const_cast<app::ViewState&>(app->state()).params.row = row;  // (the cursor, set directly: the tests are about the rows, not about Down)
    return core::param_field(focus());
  }
  float value(unsigned field, unsigned slot = kSlot) const { return arena->project.get_param(core::param_id(core::ParamKind::Instrument, slot, field)); }
  void set_mode(unsigned mode) {
    clear_notes();
    (void)invoke("instrument.play_mode", static_cast<ActionArg>(mode));
  }
};

}  // namespace

TEST_CASE("source page: F1 on a sample instrument opens it, and the Params button again comes back to it") {
  Rig rig;
  CHECK(rig.page() == ip::GroupSource);  // the Params button went to F1
  rig.tap(Button::F3);
  CHECK(rig.page() == ip::GroupFilter);
  rig.tap(Button::F1);
  CHECK(rig.page() == ip::GroupSource);
  rig.tap(Button::F2);
  rig.tap(Button::Params);
  CHECK(rig.page() == ip::GroupSource);
  app::ParamsKeyRow keys{};
  app::ViewState view = rig.app->state();
  app::params_key_row(rig.app->model(), view, keys);
  CHECK(std::string(keys.label[0]) == "SAMPLE");
  CHECK((keys.dim & 1u) == 0u);  // no longer waiting for its slice
  // the pages in Left / Right order: SOURCE, VOICE, FILTER, ENV, SENDS, round
  rig.pass(1000);
  (void)rig.invoke("instrument.turn", 1);
  CHECK(rig.page() == ip::GroupVoice);
  rig.pass(1000);
  (void)rig.invoke("instrument.turn", -1);
  CHECK(rig.page() == ip::GroupSource);
  rig.pass(1000);
  (void)rig.invoke("instrument.turn", -1);
  CHECK(rig.page() == ip::GroupSends);  // wrapped to the last key
}

TEST_CASE("source page: the rows follow the play mode, PLAY MODE first and PLAY ENGINE last, and never pass eight (O5, O8)") {
  Rig rig;
  struct Want {
    unsigned mode;
    std::vector<unsigned> fields;
  };
  const Want wants[] = {
      {0, {ip::PlayMode, ip::PlayEngine, ip::PlayStereo}},  // (OS-STEREO step 2: STEREO follows PLAY ENGINE)
      {1, {ip::PlayMode, ip::PlayEngine, ip::PlayStereo}},
      {2, {ip::PlayMode, ip::PlayEngine, ip::PlayStereo}},
      {3, {ip::PlayMode, ip::PlayEngine, ip::PlayStereo}},
      {4, {ip::PlayMode, ip::Slices, ip::Slice, ip::PlayEngine, ip::PlayStereo}},
      {5, {ip::PlayMode, ip::Slices, ip::Slice, ip::PlayEngine, ip::PlayStereo}},
      {6, {ip::PlayMode, ip::WtPos, ip::WtSize, ip::PlayEngine, ip::PlayStereo}},
      {7, {ip::PlayMode, ip::GrainPos, ip::GrainLength, ip::GrainShape, ip::GrainLoop, ip::PlayEngine, ip::PlayStereo}},
  };
  for (const Want& w : wants) {
    rig.set_mode(w.mode);
    CHECK_MESSAGE(rig.inst().sampler.play_mode == w.mode, w.mode);
    NEED(rig.rows() == static_cast<int>(w.fields.size()));
    CHECK(rig.rows() <= app::ParamView::kMaxRows);
    for (unsigned i = 0; i < w.fields.size(); ++i) {
      CHECK_MESSAGE(rig.field_at(static_cast<int>(i)) == w.fields[i], "mode " << w.mode << " row " << i);
    }
  }
}

TEST_CASE("source page: the F1 label follows the play mode") {
  Rig rig;
  const char* const labels[8] = {"SAMPLE", "SAMPLE", "SAMPLE", "SAMPLE", "SLICE", "SLICE", "WT", "GRAIN"};
  for (unsigned mode = 0; mode < 8; ++mode) {
    rig.set_mode(mode);
    app::ParamsKeyRow keys{};
    app::ViewState view = rig.app->state();
    app::params_key_row(rig.app->model(), view, keys);
    CHECK_MESSAGE(std::string(keys.label[0]) == labels[mode], mode);
  }
}

TEST_CASE("play mode: WAVETABLE fits the window to the sample (2048 held to the length, a power of two), sets the sample type, and is one undo unit") {
  Rig rig;
  CHECK(rig.invoke("instrument.play_mode", 6) == DispatchResult::Changed);
  CHECK(rig.inst().sampler.play_mode == 6);
  CHECK(rig.inst().sampler.sample_type == 1);
  CHECK(rig.inst().sampler.wavetable_window_size == 2048);
  CHECK(rig.inst().sampler.wavetable_window == 0u);
  CHECK(rig.note() == "PLAY MODE: WAVETABLE");
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.inst().sampler.play_mode == 0);
  CHECK(rig.inst().sampler.sample_type == 0);
  CHECK(rig.inst().sampler.wavetable_window_size == 0);
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(rig.inst().sampler.play_mode == 6);
  // leaving it puts the type back
  CHECK(rig.invoke("instrument.play_mode", 0) == DispatchResult::Changed);
  CHECK(rig.inst().sampler.sample_type == 0);
  // a size the sample cannot hold is fitted: 3000 frames take 2048 at most, an existing 512 stays, a position past the windows goes to the last
  rig.select(kSlot);
  model::Instrument in = rig.inst();
  in.sampler.wavetable_window_size = 512;
  in.sampler.wavetable_window = 5000;
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  CHECK(rig.invoke("instrument.play_mode", 6) == DispatchResult::Changed);
  CHECK(rig.inst().sampler.wavetable_window_size == 512);
  CHECK(rig.inst().sampler.wavetable_window == kFrames / 512u - 1u);
}

TEST_CASE("play mode: WAVETABLE on a sample under 32 frames is refused and says why; a sample instrument with no audio is allowed") {
  Rig rig;
  rig.select(kShort);
  (void)rig.invoke("instrument.play_mode", 6);
  CHECK(rig.inst(kShort).sampler.play_mode == 0);
  CHECK(rig.note() == "WAVETABLE NEEDS A SAMPLE OF 32 FRAMES");
  rig.select(kEmpty);  // no audio known: the size stays as the engine would play it
  CHECK(rig.invoke("instrument.play_mode", 6) == DispatchResult::Changed);
  CHECK(rig.inst(kEmpty).sampler.play_mode == 6);
  CHECK(rig.inst(kEmpty).sampler.wavetable_window_size == 2048);
}

TEST_CASE("play mode: GRANULAR keeps the grain a grain (44 .. min(length, 44100)), SLICE without slices says where to make them, a broken loop gets the whole window") {
  Rig rig;
  model::Instrument in = rig.inst();
  in.sampler.grain_length = 0;
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  CHECK(rig.invoke("instrument.play_mode", 7) == DispatchResult::Changed);
  CHECK(rig.inst().sampler.grain_length == 441);
  rig.select(kShort);  // 20 frames: the grain cannot be longer than the sample
  CHECK(rig.invoke("instrument.play_mode", 7) == DispatchResult::Changed);
  CHECK(rig.inst(kShort).sampler.grain_length == 20);
  rig.select(kSlot);
  rig.clear_notes();
  CHECK(rig.invoke("instrument.play_mode", 5) == DispatchResult::Changed);
  CHECK(rig.note() == "BEAT SLICE: NO SLICES, AUTO OR EVEN ON WAVE");
  in = rig.inst();
  in.sampler.slice_count = 4;
  in.sampler.slices[1] = 16000;
  in.sampler.slices[2] = 32000;
  in.sampler.slices[3] = 48000;
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  rig.clear_notes();
  CHECK(rig.invoke("instrument.play_mode", 4) == DispatchResult::Changed);
  CHECK(rig.note() == "PLAY MODE: SLICE");
  // a loop whose points break start <= loop start < loop end <= end
  in = rig.inst();
  in.sampler.start = 10000;
  in.sampler.end = 50000;
  in.sampler.loop_start = 60000;
  in.sampler.loop_end = 20000;
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  CHECK(rig.invoke("instrument.play_mode", 1) == DispatchResult::Changed);
  CHECK(rig.inst().sampler.loop_start == 10000);
  CHECK(rig.inst().sampler.loop_end == 50000);
}

TEST_CASE("play mode: the same mode changes nothing, a MIDI instrument is refused, and the list steps stop at the ends") {
  Rig rig;
  CHECK(rig.invoke("instrument.play_mode", 0) == DispatchResult::Unchanged);
  CHECK(rig.invoke("instrument.play_mode", -1) == DispatchResult::Unchanged);  // before ONE SHOT: nothing
  CHECK(rig.invoke("instrument.play_mode", -2) == DispatchResult::Changed);    // next
  CHECK(rig.inst().sampler.play_mode == 1);
  CHECK(rig.invoke("instrument.play_mode", 8) == DispatchResult::Unchanged);
  rig.set_mode(7);
  CHECK(rig.invoke("instrument.play_mode", -2) == DispatchResult::Unchanged);  // after GRANULAR: nothing
  rig.select(9);
  rig.clear_notes();
  (void)rig.invoke("instrument.play_mode", 3);
  CHECK(rig.note() == "NO SAMPLE INSTRUMENT HERE");
}

TEST_CASE("source rows: the encoder edits PLAY MODE through the validating action, and Delete puts it back to ONE SHOT") {
  Rig rig;
  (void)rig.field_at(0);
  rig.turn(1);
  CHECK(rig.inst().sampler.play_mode == 1);
  rig.turn(5);
  CHECK(rig.inst().sampler.play_mode == 6);  // WAVETABLE: through the action, so the window was fitted
  CHECK(rig.inst().sampler.wavetable_window_size == 2048);
  rig.turn(1);
  CHECK(rig.inst().sampler.play_mode == 7);
  rig.turn(1);
  CHECK(rig.inst().sampler.play_mode == 7);  // the end of the list
  (void)rig.invoke("param.reset");
  CHECK(rig.inst().sampler.play_mode == 0);
}

TEST_CASE("source rows: WT SIZE steps by counters (32 .. 2048), moves the position with it, and Delete restores the default window") {
  Rig rig;
  rig.set_mode(6);
  NEED(rig.field_at(2) == ip::WtSize);
  rig.turn(-1);
  CHECK(rig.inst().sampler.wavetable_window_size == 1024);
  rig.turn(1);
  CHECK(rig.inst().sampler.wavetable_window_size == 2048);
  rig.turn(1);
  CHECK(rig.inst().sampler.wavetable_window_size == 2048);  // the top
  NEED(rig.field_at(1) == ip::WtPos);
  rig.turn(10);
  const float pos = rig.value(ip::WtPos);
  CHECK(pos == 10.0f);
  NEED(rig.field_at(2) == ip::WtSize);
  rig.turn(-2);  // 2048 -> 512: two counters, four times as many windows, the same part of the sample
  CHECK(rig.inst().sampler.wavetable_window_size == 512);
  CHECK(rig.value(ip::WtPos) == 40.0f);
  for (int i = 0; i < 10; ++i) {
    rig.turn(-1);
  }
  CHECK(rig.inst().sampler.wavetable_window_size == 32);
  (void)rig.invoke("param.reset");
  CHECK(rig.inst().sampler.wavetable_window_size == 2048);
}

TEST_CASE("source rows: WT POS stops at the last window, GRAIN LENGTH at the sample, SLICE at the slices there are, SLICES is a readout") {
  Rig rig;
  rig.set_mode(6);
  NEED(rig.field_at(1) == ip::WtPos);
  rig.turn(1000);
  CHECK(rig.value(ip::WtPos) == static_cast<float>(kFrames / 2048u - 1u));  // 46 windows: 0 .. 45
  rig.set_mode(7);
  NEED(rig.field_at(2) == ip::GrainLength);
  rig.turn(1000000);
  CHECK(rig.value(ip::GrainLength) == 44100.0f);  // (the sample is longer than the top)
  (void)rig.invoke("param.reset");
  CHECK(rig.value(ip::GrainLength) == 0.0f);   // not set: the engine plays 441, which is where the encoder starts
  rig.turn(-35);                               // (a click is 11 samples)
  CHECK(rig.value(ip::GrainLength) > 44.0f);
  rig.turn(-3);
  CHECK(rig.value(ip::GrainLength) == 44.0f);  // the floor
  (void)rig.invoke("param.reset");
  CHECK(rig.value(ip::GrainLength) == 0.0f);   // not set: the engine plays 441
  rig.select(kShort);
  rig.set_mode(7);
  NEED(rig.field_at(2) == ip::GrainLength);
  rig.turn(1000);
  CHECK(rig.value(ip::GrainLength, kShort) == 20.0f);  // a sample shorter than the grain's floor: the grain is the sample
  rig.select(kSlot);
  model::Instrument in = rig.inst();
  in.sampler.slice_count = 3;
  in.sampler.play_mode = 5;
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  NEED(rig.field_at(2) == ip::Slice);
  rig.turn(10);
  CHECK(rig.value(ip::Slice) == 2.0f);
  NEED(rig.field_at(1) == ip::Slices);
  rig.clear_notes();
  rig.turn(1);
  CHECK(rig.value(ip::Slices) == 3.0f);
  CHECK(rig.note() == "SLICES ARE MADE ON WAVE: AUTO, EVEN, ADD, DEL");
}

TEST_CASE("source page: rows that do not apply are dimmed with the reason, not hidden (O8)") {
  Rig rig;
  rig.select(kEmpty);  // a sample instrument with no audio
  rig.set_mode(6);
  CHECK(rig.rows() == 5);  // the rows are all there (PLAY MODE, WT POS, WT SIZE, PLAY ENGINE, STEREO)
  rig.select(kSlot);
  rig.set_mode(5);
  CHECK(rig.rows() == 5);  // SLICE is a row whatever the slices (dimmed while there are none)
  CHECK(rig.app->frames() >= 1u);
  CHECK(rig.app->failed_frames() == 0u);
}

// OS-STEREO step 2 (INST 1.5): the STEREO row, a virtual field after PLAY ENGINE. Nothing provides a side signal before slice (b), so STEREO is refused on the strip while the bank has none, and MONO is always
// allowed (a flag a file carried is the way back).
TEST_CASE("stereo row: STEREO is refused while the bank holds no side signal, the strip says why, and the instrument stays mono") {
  Rig rig;
  rig.clear_notes();
  NEED(rig.rows() == 3);
  CHECK(rig.field_at(2) == ip::PlayStereo);
  CHECK(rig.value(ip::PlayStereo) == 0.0f);  // (the table has no such row: the value is the instrument's byte)
  CHECK_FALSE(rig.bank.has_side(kSlot));
  const std::uint32_t before = rig.arena->project.generation();
  rig.turn(1);  // MONO -> STEREO
  CHECK(rig.note() == "STEREO NEEDS A STEREO LOAD");
  CHECK(rig.inst().stereo == 0);
  CHECK(rig.arena->project.generation() == before);  // nothing was written, nothing to undo
  rig.clear_notes();
  rig.turn(-1);  // MONO stays MONO: no refusal, no write
  CHECK(rig.inst().stereo == 0);
  CHECK(rig.note().empty());
}

TEST_CASE("stereo row: the row reads the instrument, and MONO clears a flag a file carried, as one undo unit") {
  Rig rig;
  model::Instrument s = rig.inst();
  s.stereo = 1;  // as a hand-made INST 1.5 file would leave it
  NEED(rig.arena->project.set_instrument(kSlot, s) == model::Status::Ok);
  rig.clear_notes();
  CHECK(rig.field_at(2) == ip::PlayStereo);
  app::ParamScratch scratch;
  const core::ParamId id = rig.focus();
  const core::ParamDesc* d = app::describe_param(rig.app->model(), id, scratch);
  NEED(d != nullptr);
  CHECK(std::string(d->name) == "STEREO");
  CHECK(std::string(d->names[0]) == "MONO");
  CHECK(std::string(d->names[1]) == "STEREO");
  rig.turn(-1);  // STEREO -> MONO
  CHECK(rig.inst().stereo == 0);
  CHECK(rig.note().empty());
  (void)rig.invoke("edit.undo");
  CHECK(rig.inst().stereo == 1);  // one undo unit: back to what the file had
  (void)rig.invoke("edit.redo");
  CHECK(rig.inst().stereo == 0);
}

TEST_CASE("stereo row: a MIDI instrument has no such page, and the write path refuses the field on an instrument that is not a sample") {
  Rig rig;
  const core::ParamId id = core::param_id(core::ParamKind::Instrument, 9, ip::PlayStereo);  // the MIDI instrument of the rig
  app::ParamScratch scratch;
  CHECK(app::is_play_stereo_param(id));
  CHECK(app::describe_param(rig.app->model(), id, scratch) != nullptr);  // (the descriptor is the field's, whatever the slot)
  CHECK(model::param_desc(id) == nullptr);                               // (not a table row: the generation block does not grow)
  CHECK_FALSE(model::instrument_field_app_writable(ip::PlayStereo));
  // The write path itself (not only the descriptor): a MIDI slot is NoInstrument whatever the value; on the sample slot NaN, a negative and a value past the two choices are Rejected and nothing is written.
  auto model = std::make_unique<app::Model>();
  app::ViewState view;
  app::ActionContext ctx{*model, view, app::App::kSoftKeyLabels, 1000, true, &rig.app->session()};
  ctx.edit = rig.editor.get();
  ctx.samples = &rig.bank;
  const std::uint32_t before = rig.arena->project.generation();
  for (const float v : {0.0f, 1.0f}) {
    CHECK(app::write_param(ctx, id, v) == app::WriteResult::NoInstrument);
  }
  CHECK(rig.arena->project.instrument(9)->stereo == 0);
  const core::ParamId sample_id = core::param_id(core::ParamKind::Instrument, kSlot, ip::PlayStereo);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (const float v : {nan, -1.0f, 2.0f, 255.0f}) {
    CHECK(app::write_param(ctx, sample_id, v) == app::WriteResult::Rejected);
  }
  CHECK(app::write_param(ctx, sample_id, 0.0f) == app::WriteResult::Unchanged);  // already MONO
  CHECK(rig.inst().stereo == 0);
  CHECK(rig.arena->project.generation() == before);
}

// The instrument is 720 B with no padding (Instrument::reserved), so the whole-instrument memcmp of a no-op edit and of PASTE compares defined bytes.
TEST_CASE("stereo flag: a no-op instrument edit records no undo unit, and a paste of an identical instrument says it already holds it") {
  Rig rig;
  static_assert(sizeof(model::Instrument) == offsetof(model::Instrument, reserved) + sizeof(model::Instrument::reserved), "no tail padding");
  const std::size_t depth = rig.editor->journal().depth();
  const model::Instrument same = rig.inst();
  NEED(rig.editor->set_instrument(kSlot, same) == model::Status::Ok);
  CHECK(rig.editor->journal().depth() == depth);  // nothing changed: no unit
  model::Instrument fresh = rig.inst();
  std::memset(fresh.reserved, 0xAA, sizeof fresh.reserved);  // a caller's stray bytes are not stored
  NEED(rig.editor->set_instrument(kSlot, fresh) == model::Status::Ok);
  CHECK(rig.editor->journal().depth() == depth);
  CHECK(rig.inst().reserved[0] == 0);
  CHECK(rig.inst().reserved[2] == 0);
  // PASTE: copy slot 5 (SHIFT+F2 on the Instrument screen), paste it in a second slot that holds the same instrument.
  rig.clear_notes();
  rig.select(kSlot);
  (void)rig.invoke("instrument.copy");
  (void)rig.invoke("instrument.paste");
  rig.clear_notes();
  (void)rig.invoke("instrument.paste");
  CHECK(rig.note().find("ALREADY HOLDS THIS") != std::string::npos);
}

TEST_CASE("source page: a wavetable with an unset window size plays the window the page shows (fitted to a short sample), not silence") {
  std::vector<std::int16_t> pcm(1000, 1000);  // under 2048 frames: the unset size (0) is fitted to 512
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  in.sampler.play_mode = 6;
  in.sampler.wavetable_window_size = 0;
  const audio::SampleDesc d = app::make_sample_desc(in, pcm.data(), 1000, nullptr);
  CHECK(d.length == app::wt_fit_size(0, 1000));
  CHECK(d.wt_size == 512);
  CHECK(d.wt_windows == 1);
  pcm.resize(20);  // under 32 frames: no window, silent (the action refuses the mode there)
  CHECK(app::make_sample_desc(in, pcm.data(), 20, nullptr).length == 0u);
}
