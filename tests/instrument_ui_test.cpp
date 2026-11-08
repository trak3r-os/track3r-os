// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S1 and S2 in the app: the Instrument screen's key rows (F1 follows the instrument, F6 / F7 the neighbours, the Shift row), F6 / F7 and Shift + F6 / F7 (O1), Left / Right turning the
// pages, the Params button going to F1, the header's engine words (Auto resolved), the pads that play the instrument in view (S2, O3, O4) and the release of their notes whenever the instrument changes
// (O2). The pictures are the gallery's `instrument` states; the tables behind the page are params_app_test.cpp's.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/chrome.h>
#include <o3os/app/pad_maps.h>
#include <o3os/app/pad_note.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/audio/voices.h>
#include <o3os/core/input_event.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

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
namespace ip = model::instrument_param;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

// What the pads asked of the voices, in order; a release also records the instrument in view at that moment (O2: before it changes).
struct PadLog final : app::PreviewSink {
  struct Down {
    unsigned pad, track, instrument, note;
  };
  struct Up {
    unsigned pad;
    int instrument_in_view;
  };
  const app::App* app = nullptr;
  std::vector<Down> downs;
  std::vector<Up> ups;
  void preview(unsigned, unsigned) noexcept override {}
  void pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note) noexcept override { downs.push_back({pad, track, instrument, note}); }
  void pad_up(unsigned pad) noexcept override { ups.push_back({pad, app != nullptr ? app->state().instrument : 0}); }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  PadLog log;

  explicit Rig(bool document = true) {
    model::Project& p = project();
    if (document) {
      sim::fill_demo_project(p);
      journal.begin(g_ring, sizeof g_ring);
      editor = std::make_unique<model::Editor>(p, journal);
      app->attach_document(editor.get());
    } else {
      p.reset();
    }
    log.app = app.get();
    app->attach_preview(&log);
    app->run_once();
    tap(Button::Params);
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
  void shift_tap(Button b) {
    press(Button::Shift);
    pass(1);
    tap(b);
    release(Button::Shift);
    pass(1);
  }
  void pad_down(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 127));
    pass(1);
  }
  void pad_up(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass(1);
  }
  void pad(unsigned index) {
    pad_down(index);
    pad_up(index);
  }
  void shift_pad(unsigned index) {
    press(Button::Shift);
    pass(1);
    pad(index);
    release(Button::Shift);
    pass(1);
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  int instrument() const { return app->state().instrument; }
  std::string note() const { return app->state().notify.empty() ? std::string() : std::string(app->state().notify.head().text.c_str()); }
  void select(int n) { (void)invoke("instrument.select", static_cast<ActionArg>(n)); }
  // Slot `n` (1-based) as a MIDI instrument on `channel`, or free.
  void make_midi(unsigned n, unsigned channel) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Midi);
    in.midi_channel = static_cast<std::uint8_t>(channel);
    NEED_VOID(project().set_instrument(n - 1u, in) == model::Status::Ok);
  }
  void free_slot(unsigned n) { (void)project().clear_instrument(n - 1u); }
  static void NEED_VOID(bool ok) { CHECK_MESSAGE(ok, "rig setup"); }
};

bool dim(const app::ParamsKeyRow& row, unsigned key) { return ((row.dim >> key) & 1u) != 0u; }

// (filled in place: two of the labels point into the row itself)
void keys_of(const Rig& rig, bool shift, app::ParamsKeyRow& row) {
  app::ViewState view = rig.app->state();
  view.shift = shift;
  app::params_key_row(rig.app->model(), view, row);
}

}  // namespace

// ---- S1: F6 / F7 and the key rows ---------------------------------------------------------------------------------------------------------

TEST_CASE("instrument ui: F6 and F7 step one slot, 1..64, with no wrap and no word at the ends (O1)") {
  Rig rig;
  rig.select(1);
  rig.tap(Button::F6);
  CHECK(rig.instrument() == 1);
  CHECK(rig.note().empty());  // a non-event says nothing
  rig.tap(Button::F7);
  CHECK(rig.instrument() == 2);
  rig.tap(Button::F7);
  rig.tap(Button::F6);
  CHECK(rig.instrument() == 2);
  for (int i = 0; i < 70; ++i) {
    (void)rig.invoke("instrument.step", 1);
  }
  CHECK(rig.instrument() == 64);
  CHECK(rig.invoke("instrument.step", 1) == DispatchResult::Unchanged);
  rig.tap(Button::F7);
  CHECK(rig.instrument() == 64);
  rig.tap(Button::F6);
  CHECK(rig.instrument() == 63);  // (a free slot is a slot too)
  CHECK(rig.invoke("instrument.step", 0) == DispatchResult::Unchanged);
}

TEST_CASE("instrument ui: Shift + F6 and F7 go to the previous and next slot in use, skip the free ones and stop at the last") {
  Rig rig;
  for (unsigned n = 4; n <= 9; ++n) {
    rig.free_slot(n);  // the demo's ten: 1, 2, 3 and 10 stay
  }
  rig.select(3);
  rig.shift_tap(Button::F7);
  CHECK(rig.instrument() == 10);
  rig.shift_tap(Button::F7);
  CHECK(rig.instrument() == 10);  // none after it
  rig.shift_tap(Button::F6);
  CHECK(rig.instrument() == 3);
  rig.shift_tap(Button::F6);
  rig.shift_tap(Button::F6);
  CHECK(rig.instrument() == 1);
  rig.shift_tap(Button::F6);
  CHECK(rig.instrument() == 1);
  // from a free slot, the nearest in use either way
  rig.select(6);
  rig.shift_tap(Button::F6);
  CHECK(rig.instrument() == 3);
  rig.select(6);
  rig.shift_tap(Button::F7);
  CHECK(rig.instrument() == 10);
}

TEST_CASE("instrument ui: with no project open F6 / F7 still step and Shift + F6 / F7 find nothing in use") {
  Rig rig(false);
  rig.select(5);
  rig.tap(Button::F7);
  CHECK(rig.instrument() == 6);
  rig.shift_tap(Button::F7);
  CHECK(rig.instrument() == 6);
  rig.shift_tap(Button::F6);
  CHECK(rig.instrument() == 6);
  CHECK(app::instrument_in_use(rig.app->model(), 6, 1) == 0);
}

TEST_CASE("instrument ui: the key rows follow the instrument (D1): F1 by the kind, F6 / F7 the neighbours, dim where there is none or the slice is not built") {
  Rig rig;
  rig.make_midi(7, 3);
  rig.free_slot(31);
  rig.select(5);
  app::ParamsKeyRow row{};
  keys_of(rig, false, row);
  CHECK(std::string(row.label[0]) == "SAMPLE");
  CHECK(!dim(row, 0));  // the source page is built (S3)
  CHECK(std::string(row.label[1]) == "VOICE");
  CHECK(std::string(row.label[2]) == "FILTER");
  CHECK(std::string(row.label[3]) == "MOD");
  CHECK(std::string(row.label[4]) == "SENDS");
  CHECK(std::string(row.label[5]) == "<04");
  CHECK(std::string(row.label[6]) == "06>");
  CHECK(std::string(row.label[7]) == "UNDO");
  CHECK((row.dim & ~1u) == 0u);
  for (unsigned i = 0; i < 8; ++i) {
    CHECK_MESSAGE(std::strlen(row.label[i]) <= 7u, row.label[i]);
  }
  // the ends: dimmed, the label stays the word
  rig.select(1);
  keys_of(rig, false, row);
  CHECK(std::string(row.label[5]) == "<INS");
  CHECK(dim(row, 5));
  CHECK(!dim(row, 6));
  CHECK(std::string(row.label[6]) == "02>");
  for (int i = 0; i < 70; ++i) {
    (void)rig.invoke("instrument.step", 1);  // (instrument.select is the pads', 1..48)
  }
  keys_of(rig, false, row);
  CHECK(std::string(row.label[6]) == "INS>");
  CHECK(dim(row, 6));
  CHECK(std::string(row.label[5]) == "<63");
  // a MIDI instrument and a free slot
  rig.select(7);
  keys_of(rig, false, row);
  CHECK(std::string(row.label[0]) == "MIDI");
  CHECK(!dim(row, 0));  // OS-INSTUI S8a / S8b: the MIDI and CC pages; FILTER, MOD and SENDS are dim (instrument_midi_page_test.cpp)
  CHECK(std::string(row.label[1]) == "CC");
  CHECK(!dim(row, 1));
  CHECK((dim(row, 2) && dim(row, 3) && dim(row, 4)));
  rig.select(31);
  keys_of(rig, false, row);
  CHECK(std::string(row.label[0]) == "SAMPLE");  // the type chooser (S5): SAMPLE MIDI PASTE MODULE LOAD
  CHECK(!dim(row, 0));
  CHECK(std::string(row.label[3]) == "MODULE");
  // the Shift row (a MIDI instrument): NAME, COPY and DELETE are live, PASTE is dim while nothing is copied and SAVE is for a sample instrument (S7); F6 / F7 the numbers of the nearest in use, F8 REDO
  rig.select(7);
  keys_of(rig, true, row);
  const char* names[5] = {"NAME", "COPY", "PASTE", "DELETE", "SAVE"};
  for (unsigned i = 0; i < 5; ++i) {
    CHECK(std::string(row.label[i]) == names[i]);
    CHECK(dim(row, i) == (i == 2 || i == 4));
  }
  CHECK(std::string(row.label[5]) == "<06");
  CHECK(std::string(row.label[6]) == "08>");
  CHECK(std::string(row.label[7]) == "REDO");
  CHECK(!dim(row, 5));
  CHECK(!dim(row, 6));
  rig.select(10);
  for (unsigned n = 11; n <= 64; ++n) {
    rig.free_slot(n);
  }
  keys_of(rig, true, row);
  CHECK(std::string(row.label[6]) == "USED>");  // none after it: the word, dimmed
  CHECK(dim(row, 6));
}

TEST_CASE("instrument ui: the Shift + F1..F5 row is live since S7: dim keys say why and change nothing") {
  Rig rig;
  const std::uint8_t page = rig.app->state().page[app::screen_index(app::ScreenId::Instrument)];
  // PASTE with nothing copied says so (and is dim); the rest are the S7 tests' (instrument_clip_test.cpp).
  rig.pass(10000);
  rig.shift_tap(Button::F3);
  CHECK(rig.note() == "NOTHING TO PASTE: SHIFT+F2 COPIES");
  CHECK(rig.app->state().page[app::screen_index(app::ScreenId::Instrument)] == page);
}

// ---- S1: pages -----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("instrument ui: F1 on a sample instrument opens the source page, on a MIDI instrument the MIDI page (S8a), on a free slot F4 MODULE opens the module picker (the type chooser, S5)") {
  Rig rig;
  rig.make_midi(7, 3);
  rig.select(2);
  rig.tap(Button::F3);
  const auto page = [&] { return static_cast<unsigned>(rig.app->state().page[app::screen_index(app::ScreenId::Instrument)]); };
  rig.pass(10000);
  rig.tap(Button::F1);
  CHECK(page() == ip::GroupSource);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  rig.select(7);  // a MIDI instrument: the source page is a sample instrument's; its F1 is the MIDI page (S8a), which is already there
  rig.pass(10000);
  rig.tap(Button::F1);
  CHECK(rig.note().empty());
  CHECK(app::kParamsScreen.count(rig.app->model(), rig.app->state()) == 6);  // CHANNEL, VELOCITY, PROGRAM, OUT, PB RANGE, OFFSET (S8b)
  rig.free_slot(31);
  rig.select(31);
  rig.pass(10000);
  rig.tap(Button::F4);
  CHECK(rig.note() == "NO MODULE SYSTEM IN THIS BUILD");  // (the rig attaches none; with one the picker opens: module_ui_test)
  CHECK(rig.invoke("instrument.page", 1) == DispatchResult::Unchanged);  // the pages are not there: F1..F5 are the type chooser's (context instrument.empty)
  CHECK(rig.invoke("instrument.page", 5) == DispatchResult::Unchanged);  // there is no F6 page
}

TEST_CASE("instrument ui: a held arrow (auto-repeat) stops at the last page; a fresh press wraps") {
  Rig rig;
  const auto page = [&] { return static_cast<unsigned>(rig.app->state().page[app::screen_index(app::ScreenId::Instrument)]); };
  rig.tap(Button::F5);
  CHECK(page() == ip::GroupSends);
  rig.pass(1000);
  CHECK(rig.invoke("instrument.turn", 1) == DispatchResult::Changed);  // a fresh press: round to the first key, the source page
  CHECK(page() == ip::GroupSource);
  rig.pass(1000);
  CHECK(rig.invoke("instrument.turn", -1) == DispatchResult::Changed);  // and back (fresh)
  CHECK(page() == ip::GroupSends);
  rig.pass(100);  // 100 ms later: the repeat of a held key
  CHECK(rig.invoke("instrument.turn", 1) == DispatchResult::Unchanged);
  CHECK(page() == ip::GroupSends);
  rig.pass(100);
  CHECK(rig.invoke("instrument.turn", -1) == DispatchResult::Changed);  // the other way is not blocked
}

TEST_CASE("instrument ui: the strip's play mode and engine words are clipped to what the CPU readout leaves") {
  platform::host::FakeDisplay display;
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  ui::Renderer r(builder, theme);
  const int room = ui::kScreenW - 2 * theme.metrics.pad - app::StatusStrip::load_width(r) - 4 * theme.metrics.gap;
  const char* const modes[8] = {"ONE SHOT", "LOOP FWD", "LOOP BWD", "PING-PONG", "SLICE", "BEAT SLICE", "WAVETABLE", "GRANULAR"};
  const char* const engines[5] = {"AUTO>MODERN", "AUTO>AMIGA", "AUTO>12-BIT", "AMIGA", "8-BIT"};
  for (const char* mode : modes) {
    for (const char* engine : engines) {
      core::FixedString<40> text;
      text.assign(mode);
      text.append(" / ");
      text.append(engine);
      const int x = 330;  // (where the field starts after PARAMS, the transport badge and "INS 07 NAME": measured in the gallery picture)
      app::params_how_clip(r, text, x);
      CHECK_MESSAGE(x + theme.metrics.gap + r.text_width(text.c_str(), ui::Face::Data) <= room, text.c_str());
    }
  }
  core::FixedString<40> shorter;
  shorter.assign("BEAT SLICE / AUTO>12-BIT");
  app::params_how_clip(r, shorter, 330);
  CHECK(shorter.size() >= 20u);  // clipped only a little, if at all
}

// OS-INSTUI-ARROWS: Left / Right are one encoder detent on the row under the cursor, Shift + Left / Right the coarse step; the pages are the F-keys.
TEST_CASE("instrument ui: Left and Right edit the row like one encoder detent, Shift the coarse step, and the page and row stay") {
  Rig rig;
  const auto page = [&] { return static_cast<unsigned>(rig.app->state().page[app::screen_index(app::ScreenId::Instrument)]); };
  const auto focus = [&] { return app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()); };
  const auto value = [&] { return rig.project().get_param(focus()); };
  const auto turn = [&](int detents) {
    rig.input.push(core::make_encoder_event(rig.clock.now(), 0, static_cast<std::int16_t>(detents)));
    rig.pass(1);
  };
  rig.tap(Button::F3);  // FILTER
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  const core::ParamId id = focus();
  CHECK(id == core::param_id(core::ParamKind::Instrument, static_cast<unsigned>(rig.instrument() - 1), ip::Cutoff));
  const float v0 = value();
  rig.tap(Button::Right);
  const float right = value();
  CHECK(right > v0);
  rig.pass(2000);
  rig.tap(Button::Left);
  CHECK(value() == doctest::Approx(v0));
  CHECK(page() == ip::GroupFilter);  // no page turned, the cursor did not move
  CHECK(focus() == id);
  // The same value as one detent of the encoder, and the same undo: F8 puts it back.
  rig.pass(2000);
  rig.tap(Button::Right);
  CHECK(value() == right);
  rig.tap(Button::F8);
  CHECK(value() == doctest::Approx(v0));
  rig.pass(2000);
  turn(1);
  CHECK(value() == right);
  // Shift: the coarse step, as Shift + encoder.
  rig.pass(2000);
  rig.shift_tap(Button::Right);
  const float coarse = value();
  CHECK(coarse > right);
  rig.tap(Button::F8);
  CHECK(value() == doctest::Approx(right));
  rig.pass(2000);
  rig.press(Button::Shift);
  rig.pass(1);
  turn(1);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(value() == coarse);
  rig.shift_tap(Button::Left);
  CHECK(value() == doctest::Approx(right));
  CHECK(page() == ip::GroupFilter);
  // A quick run of arrows is one undo unit, as detents of the encoder are.
  rig.pass(2000);
  rig.tap(Button::F8);
  rig.pass(2000);
  const float base = value();
  for (int i = 0; i < 4; ++i) {
    rig.pass(60);
    rig.tap(Button::Right);
  }
  CHECK(value() > base);
  rig.tap(Button::F8);
  CHECK(value() == doctest::Approx(base));
}

TEST_CASE("instrument ui: Left and Right edit a MIDI instrument's row too, and the chooser of an empty slot keeps turning its pages") {
  Rig rig;
  const auto page = [&] { return static_cast<unsigned>(rig.app->state().page[app::screen_index(app::ScreenId::Instrument)]); };
  rig.make_midi(5, 3);
  rig.select(5);
  rig.tap(Button::F1);
  CHECK(page() == ip::GroupMidi);
  CHECK(rig.project().instrument(4)->midi_channel == 3u);
  rig.tap(Button::Right);
  CHECK(rig.project().instrument(4)->midi_channel == 4u);
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  CHECK(rig.project().instrument(4)->midi_channel == 2u);
  CHECK(page() == ip::GroupMidi);  // (the MIDI instrument's pages are F1 and F2 too)
  rig.tap(Button::F1);             // the same key again: a group of one window
  CHECK(page() == ip::GroupMidi);
  // The empty slot's chooser: Left / Right are the old page turn, no parameter is written.
  rig.free_slot(31);
  rig.select(31);
  rig.pass(10000);
  const unsigned before = page();
  rig.tap(Button::Right);
  CHECK(page() != before);
  CHECK(rig.app->failed_frames() == 0u);
}

// ---- S1: the header ----------------------------------------------------------------------------------------------------------------------

TEST_CASE("instrument ui: the engine in the header is the one the voice plays with; Auto shows what it resolved to") {
  auto voices = std::make_unique<audio::VoicePool>();
  auto arena = std::make_unique<model::ModelArena>();
  auto mem = std::make_unique<app::SampleBankMem>();
  std::vector<std::int16_t> memory(1000);
  app::SampleBank bank;
  bank.init(*voices, *arena, *mem, memory.data(), static_cast<std::uint32_t>(memory.size()));
  app::Model model;
  model.samples = &bank;
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  core::FixedString<16> text;
  app::params_engine_text(model, 0, in, text);
  CHECK(std::string(text.c_str()) == "AUTO>MODERN");
  in.sample_rate = 8287;  // a Paula rate: Auto plays the Amiga engine (SampleBank::engine_for)
  app::params_engine_text(model, 0, in, text);
  CHECK(std::string(text.c_str()) == "AUTO>AMIGA");
  in.sampler.play_mode = 4;  // slices are not what Paula plays
  app::params_engine_text(model, 0, in, text);
  CHECK(std::string(text.c_str()) == "AUTO>MODERN");
  in.sampler.play_mode = 0;
  in.engine = model::kInstEngineModern;
  app::params_engine_text(model, 0, in, text);
  CHECK(std::string(text.c_str()) == "MODERN");
  in.engine = model::kInstEngineBits12;
  app::params_engine_text(model, 0, in, text);
  CHECK(std::string(text.c_str()) == "12-BIT");
  in.engine = model::kInstEngineBits8;
  app::params_engine_text(model, 0, in, text);
  CHECK(std::string(text.c_str()) == "8-BIT");
  in.engine = model::kInstEngineAmiga;
  app::params_engine_text(model, 0, in, text);
  CHECK(std::string(text.c_str()) == "AMIGA");
  model.samples = nullptr;
  in.engine = model::kInstEngineAuto;
  app::params_engine_text(model, 0, in, text);
  CHECK(std::string(text.c_str()) == "AUTO");  // no bank: nothing to resolve it with
}

// ---- S2: the pads -------------------------------------------------------------------------------------------------------------------------

TEST_CASE("instrument ui: a pad plays the instrument in view on the live-note path and holds until its release (D2)") {
  Rig rig;
  rig.select(3);
  rig.pad_down(5);
  NEED(rig.log.downs.size() == 1u);
  CHECK(rig.log.downs[0].pad == 5u);
  CHECK(rig.log.downs[0].instrument == 3u);
  CHECK(rig.log.downs[0].note == rig.app->state().edit.pads.note(5));  // the NOTES layout, as Pattern's pads
  CHECK(rig.app->state().inst_pads == (std::uint64_t{1} << 5));
  CHECK(rig.instrument() == 3);  // playing is not selecting
  CHECK(rig.log.ups.empty());
  rig.pad_up(5);
  NEED(rig.log.ups.size() == 1u);
  CHECK(rig.log.ups[0].pad == 5u);
  CHECK(rig.app->state().inst_pads == 0u);
}

TEST_CASE("instrument ui: Shift + a pad selects the instrument without a sound (O3)") {
  Rig rig;
  rig.shift_pad(8);
  CHECK(rig.instrument() == 9);
  CHECK(rig.log.downs.empty());
  CHECK(rig.app->state().inst_pads == 0u);
}

TEST_CASE("instrument ui: a beat slice instrument plays slice n on pad n; PIANO plays its own keys and its jump pads are dead (O4)") {
  Rig rig;
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  in.sampler.play_mode = app::kPlayModeBeatSlice;
  in.sampler.slice_count = 8;
  NEED(rig.project().set_instrument(11, in) == model::Status::Ok);
  rig.select(12);
  rig.pad(7);
  NEED(rig.log.downs.size() == 1u);
  CHECK(rig.log.downs[0].note == audio::kSliceRootNote + 7u);
  CHECK(rig.log.downs[0].instrument == 12u);
  // a layout: PIANO, chosen on Pattern (Shift + F2), is the one the pads have here
  rig.log.downs.clear();
  rig.tap(Button::Pattern);
  (void)rig.invoke("pads.mode", 1);
  rig.tap(Button::Params);
  rig.select(1);
  const app::NoteEntry& entry = rig.app->state().edit.entry;
  rig.pad(13);  // column 2, row 2: a white key
  NEED(rig.log.downs.size() == 1u);
  CHECK(rig.log.downs[0].note == entry.locked(app::pad_fn_note(entry, app::piano_pad(13))));
  rig.pad(0);  // column 1: the pattern jump, not a note
  rig.pad(11);  // column 12: the octave key
  CHECK(rig.log.downs.size() == 1u);
  app::PadRole roles[48];
  CHECK(app::kParamsScreen.pad_roles(rig.app->model(), rig.app->state(), roles));
  CHECK(roles[0] == app::PadRole::Off);
  CHECK(roles[11] == app::PadRole::Off);
  // NOTES again: the board's scale lights (OS-NOTESLED), as on Pattern: the root's pad Front
  rig.tap(Button::Pattern);
  (void)rig.invoke("pads.mode", 0);
  rig.tap(Button::Params);
  CHECK(app::kParamsScreen.pad_roles(rig.app->model(), rig.app->state(), roles));
  unsigned fronts = 0;
  for (const app::PadRole role : roles) {
    fronts += role == app::PadRole::Front ? 1u : 0u;
  }
  CHECK(fronts > 0u);  // the roots of the board's octaves
}

TEST_CASE("instrument ui: changing the instrument releases the held notes first, on the instrument that started them (O2), MIDI included") {
  Rig rig;
  rig.make_midi(6, 3);
  // F7 while a pad holds a note of a sample instrument
  rig.select(5);
  rig.pad_down(2);
  rig.tap(Button::F7);
  NEED(rig.log.ups.size() == 1u);
  CHECK(rig.log.ups[0].pad == 2u);
  CHECK(rig.log.ups[0].instrument_in_view == 5);  // before the change
  CHECK(rig.instrument() == 6);
  CHECK(rig.app->state().inst_pads == 0u);
  rig.pad_up(2);  // the finger lets go later: the sink is told again (it knows the pad is free), nothing is played
  CHECK(rig.log.downs.size() == 1u);
  // the MIDI instrument: the pad's note is the sink's (a live MIDI note-on on its channel) and F6 / F7 / Shift + F7 / a pick ends it
  rig.log.ups.clear();
  rig.pad_down(9);
  NEED(rig.log.downs.size() == 2u);
  CHECK(rig.log.downs[1].instrument == 6u);
  rig.tap(Button::F7);
  NEED(rig.log.ups.size() == 1u);
  CHECK(rig.log.ups[0].pad == 9u);
  CHECK(rig.log.ups[0].instrument_in_view == 6);
  rig.pad_up(9);
  rig.log.ups.clear();
  rig.pad_down(1);
  rig.pad_down(4);
  rig.shift_tap(Button::F6);
  CHECK(rig.log.ups.size() == 2u);  // both pads
  CHECK(rig.app->state().inst_pads == 0u);
  rig.pad_up(1);
  rig.pad_up(4);
  rig.log.ups.clear();
  rig.pad_down(3);
  const int before = rig.instrument();
  rig.select(20);  // the action Shift + pad and the lists run
  NEED(rig.log.ups.size() == 1u);
  CHECK(rig.log.ups[0].instrument_in_view == before);
}
