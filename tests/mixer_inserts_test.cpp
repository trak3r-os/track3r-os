// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX S4 (docs/specs/OS-INSFX.md, sections 4 and 11): the Mixer's INSERTS page through the App with a real InsertHost behind the module system: the page is the fifth of the Mixer (F6), Insert opens
// the picker of modules/effects (the card's modules, the one the project names and the card lacks greyed), a pick binds the effect and writes the project's record, F7 bypasses (host and record), Delete
// removes both, and every refusal is a toast (INSERTS FULL, MODULE MISSING, DELETE THE INSERT FIRST). OS-INSFX S6: the MASTER row takes an insert too (two instances, L and R: two of the four). The cell reads (cost, forced) come from the host. Native effect modules; the card is a
// temporary directory. Host only. (The simulator has no insert host: the scenario qa_inserts.txt shows the records and the picker.)
#include <doctest/doctest.h>

#include "support/key_row.h"
#include "support/mem_card.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/static_slot.h"
#include "../app/src/param_access.h"  // read_param (private to app/src)

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/governor_watch.h>
#include <o3os/app/mixer_view.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/preset_list.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/insert_host.h>
#include <o3os/audio/module_governor.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/taps.h>
#include <o3os/core/input_event.h>
#include <o3os/core/learn.h>
#include <o3os/core/param.h>
#include <o3os/hal/audio.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>
#include <o3os/model/undo.h>
#include <o3os/module/loader.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/platform/host/dir_storage.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/renderer.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace o3os;
using app::Button;
namespace fs = std::filesystem;

alignas(8) std::uint8_t g_ring[64 * 1024];

// ---- native modules: two effects and an instrument ---------------------------------------------------------------------------------------
struct alignas(16) FxState {
  float gain;
  float values[32];  // OS-INSFX S5: every parameter the module was told, by id
  std::uint32_t pad[3];
};
int fx_init(void* s, const o3m_host*) {
  static_cast<FxState*>(s)->gain = 1.0f;
  return 0;
}
void fx_shutdown(void*) {}
void fx_set_param(void* s, std::uint32_t id, float v) {
  FxState* f = static_cast<FxState*>(s);
  if (id == 0u) {
    f->gain = v;
  }
  if (id < 32u) {
    f->values[id] = v;  // OS-INSFX S5: what the render applied to the insert's instance
  }
}
std::uint32_t fx_render(void* s, const float* in, float* out, std::uint32_t n) {
  const float g = static_cast<const FxState*>(s)->gain;
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = in[i] * g;
  }
  return 1;
}
void fx_note_on(void*, std::uint32_t, std::uint32_t, std::uint32_t) {}
void fx_note_off(void*, std::uint32_t) {}
const o3m_param kParams[1] = {{"Gain", 0.0f, 4.0f, 1.0f, 0}};
#define FX(name, kind) \
  o3m_module { O3M_ABI_MAJOR, O3M_ABI_MINOR, kind, sizeof(o3m_module), name, sizeof(FxState), 1, kParams, fx_init, fx_render, fx_set_param, fx_note_on, fx_note_off, fx_shutdown, nullptr, 0 }
const o3m_module kBiquad = FX("biquad", O3M_KIND_EFFECT);
const o3m_module kChorus = FX("chorus", O3M_KIND_EFFECT);
const o3m_module kSynth = FX("synth", O3M_KIND_INSTRUMENT);

// OS-INSFX S5: an effect with ten parameters (two pages of eight) and one view, registered native only (no card file, so the picker's list is what it was).
const o3m_param kPagedParams[10] = {{"P00", 0.0f, 1.0f, 0.5f, 0}, {"P01", 0.0f, 1.0f, 0.5f, 0}, {"P02", 0.0f, 1.0f, 0.5f, 0}, {"P03", 0.0f, 1.0f, 0.5f, 0},
                                     {"P04", 0.0f, 1.0f, 0.5f, 0}, {"P05", 0.0f, 1.0f, 0.5f, 0}, {"P06", 0.0f, 1.0f, 0.5f, 0}, {"P07", 0.0f, 1.0f, 0.5f, 0},
                                     {"P08", 0.0f, 1.0f, 0.5f, 0}, {"P09", 0.0f, 1.0f, 0.5f, 0}};
std::uint32_t g_fx_view_calls = 0;
const o3m_draw_api* g_fx_api = nullptr;
void fx_view_draw(void*, std::uint32_t, o3m_canvas* c) {
  ++g_fx_view_calls;
  g_fx_api->rect(c, 0, 0, 4, 4, O3M_COLOR_TEXT);
  g_fx_api->text(c, 6, 2, "FX", O3M_FACE_DATA, O3M_COLOR_ACCENT);
}
const o3m_ui kPagedUi = {sizeof(o3m_ui), nullptr, nullptr, fx_view_draw, nullptr, nullptr};
const o3m_module kPaged = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), "paged", sizeof(FxState), 10, kPagedParams,
                           fx_init, fx_render, fx_set_param, fx_note_on, fx_note_off, fx_shutdown, &kPagedUi, 1};

// OS-INSFX S7: an effect with two learnable parameters and one that MIDI Learn may not bind (O3M_PARAM_NO_LEARN), native only.
const o3m_param kLearnParams[3] = {{"Cutoff", 0.0f, 1.0f, 0.5f, 0}, {"Reso", 0.0f, 1.0f, 0.5f, 0}, {"Mode", 0.0f, 1.0f, 0.0f, O3M_PARAM_NO_LEARN}};
const o3m_module kLearnable = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), "lrn", sizeof(FxState), 3, kLearnParams,
                               fx_init, fx_render, fx_set_param, fx_note_on, fx_note_off, fx_shutdown, nullptr, 0};

// OS-INSFX-ABI2 A8: an effect with 32 parameters (P00 0..1, P01 an INT 0..10 with the default 5, the rest 0..1; the default is 0.5 but P01) and two factory presets, native only.
#define PFX_ROW(n) {"P" #n, 0.0f, 1.0f, 0.5f, 0}
const o3m_param kPfxParams[32] = {PFX_ROW(00), {"P01", 0.0f, 10.0f, 5.0f, O3M_PARAM_INT}, PFX_ROW(02), PFX_ROW(03), PFX_ROW(04), PFX_ROW(05), PFX_ROW(06), PFX_ROW(07), PFX_ROW(08), PFX_ROW(09),
                                  PFX_ROW(10), PFX_ROW(11), PFX_ROW(12), PFX_ROW(13), PFX_ROW(14), PFX_ROW(15), PFX_ROW(16), PFX_ROW(17), PFX_ROW(18), PFX_ROW(19),
                                  PFX_ROW(20), PFX_ROW(21), PFX_ROW(22), PFX_ROW(23), PFX_ROW(24), PFX_ROW(25), PFX_ROW(26), PFX_ROW(27), PFX_ROW(28), PFX_ROW(29),
                                  PFX_ROW(30), PFX_ROW(31)};
const float kPfxWide[32] = {0.9f,  8.0f,  0.1f,  0.125f, 0.15f, 0.175f, 0.2f,  0.225f, 0.25f, 0.275f, 0.3f,  0.325f, 0.35f, 0.375f, 0.4f,  0.425f,
                            0.45f, 0.475f, 0.5f, 0.525f, 0.55f, 0.575f, 0.6f,  0.625f, 0.65f, 0.675f, 0.7f,  0.725f, 0.75f, 0.775f, 0.8f,  0.825f};
const float kPfxTwo[2] = {0.2f, 7.4f};  // two values: the other thirty take their defaults; the INT is rounded to 7
const o3m_preset kPfxTable[2] = {{"Wide", kPfxWide, 32}, {"Two", kPfxTwo, 2}};
const o3m_module kPfx = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), "pfx", sizeof(FxState), 32, kPfxParams,
                         fx_init, fx_render, fx_set_param, fx_note_on, fx_note_off, fx_shutdown, nullptr, 0,
                         nullptr, nullptr, 0, nullptr, nullptr, nullptr, 0, kPfxTable, 2};

// OS-INSFX-ABI2 A5: a 1.12 WIDENS effect (render_st with O3M_FX_WIDENS): a stereo insert, marked ST on the INSERTS cell and the picker line.
std::uint32_t fx_widen_render(void*, const float* il, const float* ir, float* ol, float* orr, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    const float m = (il[i] + ir[i]) * 0.5f;
    ol[i] = m * 1.5f;
    orr[i] = m * 0.5f;
  }
  return 1;
}
const o3m_module kWiden = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), "widen", sizeof(FxState), 1, kParams,
                           fx_init, fx_render, fx_set_param, fx_note_on, fx_note_off, fx_shutdown, nullptr, 0,
                           nullptr, nullptr, 0, nullptr, nullptr, fx_widen_render, O3M_FX_WIDENS, nullptr, 0};

// The insert host's own clock: every call moves it by g_step, so one run of an effect measures exactly g_step cycles (0: the host's default clock, which never moves).
std::uint32_t g_ins_clock = 0;
std::uint32_t g_step = 0;
std::uint32_t ins_clock() noexcept { return g_ins_clock += g_step; }
namespace gps = audio::graph_param::strip;
namespace gpm = audio::graph_param::master;  // OS-INSFX S5 review: filling the graph's param ring with a master change

bool route_insert(void* self, const core::ParamChange& c) noexcept {
  return core::param_kind(c.id) == core::ParamKind::Insert && static_cast<audio::InsertHost*>(self)->apply_param(c);
}

struct Card {
  fs::path root;
  Card() {
    root = fs::temp_directory_path() / ("o3m_mixins_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    fs::remove_all(root);
    fs::create_directories(root / "modules" / "effects");
    fs::create_directories(root / "modules" / "instruments");
    for (const char* f : {"modules/effects/biquad.tmod", "modules/effects/chorus.tmod", "modules/effects/synth.tmod", "modules/effects/notes.txt"}) {
      std::ofstream out(root / f, std::ios::binary);
      out << "x";
    }
  }
  ~Card() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

struct Modules {
  module::ModuleSet set;
  module::ModuleLoader loader;
  audio::ModuleVoices voices;
  app::ModuleUi ui;
  alignas(64) std::uint8_t scratch[audio::ModuleVoices::kScratchBytes] = {};
  alignas(64) std::uint8_t sdram[16384] = {};
};

// The App over the demo project (twelve tracks) with the module system, the card and a real insert host.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  core::ParamRing ring;
  Card card;
  platform::host::DirStorage storage{card.root.string()};
  std::unique_ptr<Modules> mods = std::make_unique<Modules>();
  audio::Graph* g = nullptr;
  audio::InsertHost* h = nullptr;
  audio::InsertHost::Blobs blobs;
  alignas(4) std::int16_t dc_block[hal::audio::kBlock] = {};

  explicit Rig(bool with_host = true) {
    g_ins_clock = 0;
    g_step = 0;
    NEED(storage.mount() == hal::storage::Status::Ok);
    (void)mods->voices.attach_scratch(mods->scratch, sizeof mods->scratch);
    (void)mods->set.cold().init(mods->sdram, 0x70000000u, sizeof mods->sdram);
    mods->ui.attach(mods->set, mods->loader, mods->voices);
    mods->ui.set_storage(&storage);
    CHECK(mods->set.add_native("biquad", kBiquad));
    CHECK(mods->set.add_native("chorus", kChorus));
    CHECK(mods->set.add_native("synth", kSynth));
    CHECK(mods->set.add_native("paged", kPaged));  // OS-INSFX S5: no card file, so the picker's list is unchanged
    CHECK(mods->set.add_native("lrn", kLearnable));  // OS-INSFX S7: the same
    CHECK(mods->set.add_native("pfx", kPfx));  // OS-INSFX-ABI2 A8: the same
    CHECK(mods->set.add_native("widen", kWiden));  // OS-INSFX-ABI2 A5: a stereo effect, native only
    g_fx_api = mods->voices.host().draw;
    g_fx_view_calls = 0;
    if (with_host) {
      static test::StaticSlot<audio::Graph> gslot;
      static test::StaticSlot<audio::InsertHost> hslot;
      static audio::TapMem taps;
      g = &gslot.make(+[]() noexcept -> std::uint32_t { return 0u; }, taps);
      h = &hslot.make();
      std::memset(blobs, 0, sizeof blobs);
      h->attach(*g, mods->voices, &blobs);
      g->set_param_hook(&route_insert, h);
      mods->ui.attach_inserts(*h);
    }
    model::Project& p = project();
    sim::fill_demo_project(p);
    CHECK(p.add_module("reverb", 6, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);  // named by the project, not on the card
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_param_ring(&ring);
    app->attach_modules(&mods->ui);
    app->run_once();
  }

  model::Project& project() { return test::arena(1).project; }
  // A sounding track 0 (a constant feed: an insert on a silent strip is never run, so never measured), and the cycles one run of an effect takes from now on.
  void sound(unsigned track, std::uint32_t cycles_per_run) {
    for (std::int16_t& s : dc_block) {
      s = 8000;
    }
    CHECK(g->params().push(core::ParamChange{audio::strip_param(track, gps::Source), 3.0f, 1}));
    g->feed(track, dc_block);
    g_step = cycles_per_run;
    h->set_cycles(&ins_clock);
  }
  // A record of the project's INSX with no instance behind it: what a project load leaves when the module is missing. `name` is in MODR already.
  void record(unsigned track, unsigned slot, const char* name) {
    int ref = -1;
    for (unsigned i = 0; i < project().module_count(); ++i) {
      if (std::strcmp(project().module(i)->name, name) == 0) {
        ref = static_cast<int>(i);
      }
    }
    NEED(ref >= 0);
    model::InsertRecord r{};
    r.module = static_cast<std::uint8_t>(ref);
    r.where = static_cast<std::uint8_t>(track);
    r.slot = static_cast<std::uint8_t>(slot);
    CHECK(editor->set_insert(r) == model::Status::Ok);
  }
  // The right-hand text of the cell (track, slot) as draw_inserts_page composes it: the same inputs (the record's bypass flag, the host's instance, has_inserts) through the same function
  // (append_insert_status). The glyphs the screen then draws are bitmap tiles, not a string: the draw call itself is the scenario's picture (qa_inserts.txt), without a host.
  std::string cell_text(unsigned track, unsigned slot) {
    const model::InsertRecord* rec = project().find_insert(track, slot);
    if (rec == nullptr) {
      return "-";
    }
    app::ModuleUi::InsertCell live{};
    const bool bound = mods->ui.insert_cell(track, slot, live);
    core::FixedString<32> s;
    app::append_insert_status(s, (rec->flags & model::kInsertFlagBypass) != 0u, bound, live.forced, mods->ui.has_inserts(), live.cost, live.st);
    return s.c_str();
  }
  // The render's side: blocks at the boundary of which the host applies its commands and the main loop's service() finishes a bind or a reset (an un-bypass is a reset: shutdown, init, activate).
  void blocks(unsigned n) {
    if (h == nullptr) {
      return;
    }
    alignas(8) static std::int16_t in[hal::audio::kBlockSamples] = {};
    alignas(8) static std::int16_t out[hal::audio::kBlockSamples] = {};
    for (unsigned i = 0; i < n; ++i) {
      hal::audio::BlockReport report{};
      const hal::audio::BlockIo io{in, out, 0, &report};
      g->render(io);
      h->begin_block();
      h->service();
    }
  }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  // OS-INSFX S7: an action by name (midi.cc, learn.toggle), and a control change message as its argument.
  app::DispatchResult invoke(const char* name, core::ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  static core::ActionArg cc(unsigned channel, unsigned controller, unsigned value) { return static_cast<core::ActionArg>((channel << 16) | (controller << 8) | value); }
  void learn_toggle() {
    invoke("learn.toggle");
    pass(1);
  }
  void turn(int detents) {  // the encoder
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass(1);
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    pass(1);
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass(1);
  }
  void tap(Button b, unsigned times) {
    for (unsigned i = 0; i < times; ++i) {
      tap(b);
    }
  }
  std::string last_note() const {
    const core::NotifyQueue& q = app->state().notify;
    return q.empty() ? std::string() : std::string(q.at(q.size() - 1).text.c_str());
  }
  app::MixerPage page() const { return app::mixer::page_of(app->state()); }
  // The INSERTS page, the cursor on (track, slot): from the top-left cell.
  void inserts_page() {
    tap(Button::Master);
    tap(Button::F6);
    CHECK(page() == app::MixerPage::Inserts);
  }
  void cell(unsigned track, unsigned slot) {
    tap(Button::Up, 16);
    tap(Button::Left, 2);
    tap(Button::Down, track);
    tap(Button::Right, slot);
  }
  // Insert on the cell, then the picker's `index`-th line and F1.
  void assign(unsigned track, unsigned slot, unsigned index) {
    cell(track, slot);
    tap(Button::Insert);
    NEED(app->state().overlay == app::OverlayId::ModulePicker);
    tap(Button::Down, index);
    tap(Button::F1);
  }
  // The host's instance at `where` (track * 2 + slot), or nullptr.
  audio::InsertHost::Info instance(unsigned where) {
    audio::InsertHost::Info in{};
    for (unsigned i = 0; h != nullptr && i < audio::InsertHost::kInstances; ++i) {
      if (h->info(i, in) && in.where == where && !in.leaving) {
        return in;
      }
    }
    in.where = 0xFF;
    return in;
  }
  FxState* fx_state(unsigned where) { return static_cast<FxState*>(const_cast<void*>(instance(where).state)); }
};

// One frame of `fn(renderer)` on a raster (as tests/module_ui_test.cpp's picture()).
template <typename Fn>
std::vector<std::uint8_t> picture(const ui::Theme& theme, Fn fn) {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, theme);
  CHECK(r.begin(ui::Color::Pane));
  fn(r);
  CHECK(r.end());
  return std::vector<std::uint8_t>(raster.rgba(), raster.rgba() + sim::EveRaster::kFramebufferBytes);
}

}  // namespace

TEST_CASE("mixer inserts: F6 opens the INSERTS page, the fifth of the Mixer, and the picker lists the card's effects with the project's missing one greyed") {
  Rig rig;
  rig.record(5, 0, "reverb");  // an INSX record of T6 INS A names reverb: the project needs it, the card lacks it
  rig.inserts_page();
  CHECK(app::kMixerPages == 5u);
  // Insert: the picker of modules/effects (the synth is an instrument on the effects folder: listed, refused at the pick; notes.txt is no module)
  rig.tap(Button::Insert);
  CHECK(rig.app->state().overlay == app::OverlayId::ModulePicker);
  CHECK(rig.app->state().modules.insert_where == 0);
  const app::ModuleUi& ui = rig.mods->ui;
  NEED(ui.effect_count() == 4u);  // biquad, chorus, synth (sorted) and, after them, reverb (named by the project, not on the card)
  const char* names[4] = {"biquad", "chorus", "synth", "reverb"};
  for (unsigned i = 0; i < 4; ++i) {
    NEED(ui.effect_at(i) != nullptr);
    CHECK_MESSAGE(std::strcmp(ui.effect_at(i)->stem, names[i]) == 0, names[i]);
  }
  using State = app::CatalogEntry::State;
  unsigned missing = 0;
  for (unsigned i = 0; i < ui.effect_count(); ++i) {
    missing += ui.effect_at(i)->state == State::Missing ? 1u : 0u;
  }
  CHECK(missing == 1u);
  rig.tap(Button::F8);  // back: the engine picker's own key
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.page() == app::MixerPage::Inserts);
}

TEST_CASE("mixer inserts: a pick binds the effect and writes the record; F7 bypasses both; Delete removes both") {
  Rig rig;
  rig.inserts_page();
  const model::Project& p = rig.project();
  rig.assign(2, 1, 0);  // T3 INS B: biquad
  CHECK(rig.last_note() == "INSERT SET");
  CHECK(rig.h->bound(2 * 2 + 1));
  const model::InsertRecord* rec = p.find_insert(2, 1);
  NEED(rec != nullptr);
  CHECK(std::strcmp(p.module(rec->module)->name, "biquad") == 0);
  CHECK((rec->flags & model::kInsertFlagBypass) == 0u);
  app::ModuleUi::InsertCell c{};
  CHECK(rig.mods->ui.insert_cell(2, 1, c));
  CHECK_FALSE(c.bypass);
  CHECK_FALSE(c.forced);

  // F7 bypasses the insert of the cell: the host and the project's flag
  rig.cell(2, 1);
  rig.tap(Button::F7);
  CHECK(rig.last_note() == "INSERT BYPASSED");
  CHECK(rig.mods->ui.insert_cell(2, 1, c));
  CHECK(c.bypass);
  rec = p.find_insert(2, 1);
  NEED(rec != nullptr);
  CHECK((rec->flags & model::kInsertFlagBypass) != 0u);
  rig.tap(Button::F7);  // and back
  CHECK(rig.last_note() == "INSERT ON");
  rig.blocks(16);  // (the un-bypass is a reset of the effect: it ends within a few blocks)
  CHECK(rig.mods->ui.insert_cell(2, 1, c));
  CHECK_FALSE(c.bypass);
  CHECK((p.find_insert(2, 1)->flags & model::kInsertFlagBypass) == 0u);

  // Insert on a cell that has an insert is refused: delete it first
  rig.tap(Button::Insert);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.last_note() == "DELETE THE INSERT FIRST");

  // Delete: the instance leaves and the record goes
  rig.tap(Button::Delete);
  CHECK(rig.last_note() == "INSERT DELETED");
  CHECK(p.find_insert(2, 1) == nullptr);
  CHECK_FALSE(rig.mods->ui.insert_cell(2, 1, c));
  rig.tap(Button::F7);
  CHECK(rig.last_note() == "NO INSERT HERE");
}

TEST_CASE("mixer inserts: the refusals are toasts: the greyed module, INSERTS FULL, an instrument module, the master's row when the four are taken") {
  Rig rig;
  rig.record(5, 0, "reverb");  // (the first of the project's four records: no instance, its module is not on the card)
  rig.inserts_page();
  const model::Project& p = rig.project();
  // the list is the card's modules, sorted (biquad, chorus, synth), then the one an INSX record names and the card lacks (reverb)
  rig.assign(0, 0, 3);
  CHECK(rig.last_note() == "MODULE MISSING");
  CHECK(p.find_insert(0, 0) == nullptr);
  CHECK_FALSE(rig.h->bound(0));
  // synth (index 2): a module of another kind
  rig.assign(0, 0, 2);
  CHECK(rig.last_note() == "NOT AN EFFECT MODULE");
  CHECK(p.find_insert(0, 0) == nullptr);
  // four inserts are the project's cap (kMaxInserts): the fifth Insert says so and opens nothing
  rig.assign(0, 0, 0);
  rig.assign(0, 1, 1);
  rig.assign(1, 0, 0);
  CHECK(p.inserts().count == 4u);
  rig.cell(2, 0);
  rig.tap(Button::Insert);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.last_note() == "INSERTS FULL");
  // the master's row (S6) needs two instances: with the four taken it says INSERTS FULL and opens nothing
  rig.cell(12, 0);
  rig.tap(Button::Insert);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.last_note() == "INSERTS FULL");
  CHECK(p.inserts().count == 4u);
  // F8 stays the track's SOLO on this page (as on ROUTING): on a track it toggles the flag, on the master row it says TRACKS ONLY
  rig.pass(3000);  // (the strip's earlier notices run out first)
  rig.tap(Button::F8);
  CHECK(rig.last_note() == "TRACKS ONLY");
  rig.cell(3, 0);
  const core::ParamId solo = core::param_id(core::ParamKind::Track, 3, model::track_param::Solo);
  CHECK(p.get_param(solo) == 0.0f);
  rig.tap(Button::F8);
  CHECK(p.get_param(solo) != 0.0f);
}

TEST_CASE("mixer inserts: the cell reads the cost from the host and says UNMEASURED before a block measured it") {
  Rig rig;
  rig.inserts_page();
  rig.assign(0, 0, 0);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  app::ModuleUi::InsertCell c{};
  CHECK(rig.mods->ui.insert_cell(0, 0, c));
  CHECK(c.cost == 0u);  // no block has run yet
  core::FixedString<24> s;
  app::append_block_cost(s, c.cost);
  CHECK(std::string(s.c_str()) == "UNMEASURED");
  // the cost of one instance of an effect (the picker's figure) is the same estimate, by stem
  CHECK(rig.mods->ui.insert_cost_of("biquad") == 0u);
  CHECK_FALSE(rig.mods->ui.insert_cell(5, 1, c));  // an empty cell has none
  s.clear();
  app::append_block_cost(s, 0u);
  CHECK(std::string(s.c_str()) == "UNMEASURED");
}

TEST_CASE("mixer inserts: without the sound side an assign says so and the records still delete and bypass") {
  Rig rig(false);
  model::Project& p = rig.project();
  model::InsertRecord r{};
  r.where = 0;
  r.slot = 0;
  r.module = 0;  // (the table's first entry: whichever it is, the record only needs an index below module_count())
  CHECK(p.add_module("biquad", 6, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  r.module = static_cast<std::uint8_t>(p.module_count() - 1u);
  CHECK(rig.editor->set_insert(r) == model::Status::Ok);
  rig.inserts_page();
  CHECK(app::ModuleUi::insert_text(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "biquad")) == std::string("NO SOUND IN THIS BUILD"));
  rig.cell(0, 0);
  rig.tap(Button::F7);  // the record's flag
  CHECK((p.find_insert(0, 0)->flags & model::kInsertFlagBypass) != 0u);
  rig.tap(Button::Delete);
  CHECK(p.find_insert(0, 0) == nullptr);
}

TEST_CASE("mixer inserts: a measured cost is in the cell, ~N% or <1%, and in the picker's figure of the module") {
  Rig rig;
  rig.inserts_page();
  rig.sound(0, audio::gov::cycles_of_percent(20));
  rig.assign(0, 0, 0);
  CHECK(rig.last_note() == "INSERT SET");
  rig.blocks(16);
  app::ModuleUi::InsertCell c{};
  NEED(rig.mods->ui.insert_cell(0, 0, c));
  CHECK(c.cost == audio::gov::cycles_of_percent(20));
  CHECK_FALSE(c.forced);
  CHECK(rig.mods->ui.insert_cost_of("biquad") == c.cost);
  core::FixedString<24> s;
  app::append_block_cost(s, c.cost);
  CHECK(std::string(s.c_str()) == "~20%");
  CHECK(rig.cell_text(0, 0) == "~20%");  // what the page prints in the cell
  rig.mods->ui.insert_bypass(*rig.editor, 0, 0, true);
  CHECK(rig.cell_text(0, 0) == "BYPASS  ~20%");  // the cost of a bypassed insert stays, after the mark
}

TEST_CASE("mixer inserts: a cost under half a percent of the block reads <1% in the cell") {
  Rig rig;
  rig.inserts_page();
  rig.sound(0, audio::gov::kBlockCycles / 500u);
  rig.assign(0, 0, 0);
  rig.blocks(16);
  app::ModuleUi::InsertCell c{};
  NEED(rig.mods->ui.insert_cell(0, 0, c));
  CHECK(c.cost == audio::gov::kBlockCycles / 500u);
  CHECK(rig.cell_text(0, 0) == "<1%");
}

TEST_CASE("mixer inserts: an insert over its budget says OVER BUDGET, F7 is refused with NO CPU while it would be forced again, and a re-bind of it is refused too") {
  Rig rig;
  rig.inserts_page();
  rig.sound(0, audio::gov::cycles_of_percent(60));  // over the instance budget (50 % of the block)
  rig.assign(0, 0, 0);
  CHECK(rig.last_note() == "INSERT SET");
  rig.blocks(16);
  app::ModuleUi::InsertCell c{};
  NEED(rig.mods->ui.insert_cell(0, 0, c));
  CHECK(c.forced);
  CHECK(rig.cell_text(0, 0) == "OVER BUDGET  ~60%");
  // F7 would bring it back (the net bypassed it): its measured cost is over the budget, the host says so, and nothing changes
  rig.cell(0, 0);
  rig.pass(3000);
  rig.tap(Button::F7);
  CHECK(rig.last_note() == "NO CPU");
  CHECK(rig.mods->ui.insert_cell(0, 0, c));
  CHECK(c.forced);
  CHECK((rig.project().find_insert(0, 0)->flags & model::kInsertFlagBypass) == 0u);
  // Delete it and assign the same module again: the host remembers that it measured over the threshold
  rig.tap(Button::Delete);
  rig.blocks(16);
  rig.pass(3000);
  rig.assign(0, 0, 0);
  CHECK(rig.last_note() == "NO CPU");
  CHECK(rig.project().find_insert(0, 0) == nullptr);
  CHECK_FALSE(rig.h->bound(0));
}

TEST_CASE("mixer inserts: a record with no instance reads NOT BOUND, F7 says MODULE MISSING: DELETE OR RELOAD, Delete removes the record") {
  Rig rig;
  rig.record(5, 0, "reverb");
  rig.inserts_page();
  CHECK(rig.cell_text(5, 0) == "NOT BOUND");
  rig.cell(5, 0);
  app::ModuleUi::InsertCell c{};
  CHECK_FALSE(rig.mods->ui.insert_cell(5, 0, c));
  rig.tap(Button::F7);
  CHECK(rig.last_note() == "MODULE MISSING: DELETE OR RELOAD");
  rig.tap(Button::Insert);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.last_note() == "DELETE THE INSERT FIRST");
  rig.tap(Button::Delete);
  CHECK(rig.last_note() == "INSERT DELETED");
  CHECK(rig.project().find_insert(5, 0) == nullptr);
  CHECK(rig.cell_text(5, 0) == "-");
}

TEST_CASE("mixer inserts: without the sound side a record is not said to be NOT BOUND") {
  Rig rig(false);
  rig.record(5, 0, "reverb");
  rig.inserts_page();
  CHECK(rig.cell_text(5, 0).empty());
}

TEST_CASE("mixer inserts: a bind right after the Delete of the same cell says INSERT LEAVING: TRY AGAIN, and works once the render has cleared it") {
  Rig rig;
  rig.inserts_page();
  rig.assign(2, 1, 0);
  CHECK(rig.last_note() == "INSERT SET");
  rig.blocks(16);
  rig.cell(2, 1);
  rig.tap(Button::Delete);
  CHECK(rig.last_note() == "INSERT DELETED");
  CHECK(rig.project().find_insert(2, 1) == nullptr);
  rig.pass(3000);
  rig.assign(2, 1, 0);  // no block has passed: the old instance is still on its way out
  CHECK(rig.last_note() == "INSERT LEAVING: TRY AGAIN");
  CHECK(rig.project().find_insert(2, 1) == nullptr);
  rig.blocks(16);
  rig.pass(3000);
  rig.assign(2, 1, 0);
  CHECK(rig.last_note() == "INSERT SET");
  CHECK(rig.project().find_insert(2, 1) != nullptr);
}

TEST_CASE("mixer inserts: the console's verbs and the page are one path: ModuleUi::insert_assign / insert_remove / insert_bypass and the replies the console prints") {
  Rig rig;
  app::ModuleUi& ui = rig.mods->ui;
  model::Editor& ed = *rig.editor;
  const auto text = [](app::ModuleUi::InsertResult r) { return std::string(app::ModuleUi::insert_text(r)); };
  using R = app::ModuleUi::InsertResult;
  CHECK(ui.insert_assign(ed, 0, 0, "biquad") == R::Ok);  // `ins bind 1 1 biquad`
  rig.blocks(4);
  const model::InsertRecord* rec = rig.project().find_insert(0, 0);
  NEED(rec != nullptr);
  CHECK(std::strcmp(rig.project().module(rec->module)->name, "biquad") == 0);
  app::ModuleUi::InsertCell c{};
  CHECK(ui.insert_cell(0, 0, c));  // the page reads the insert the console made
  CHECK(text(ui.insert_assign(ed, 0, 0, "chorus")) == "DELETE THE INSERT FIRST");
  CHECK(text(ui.insert_assign(ed, 1, 0, "synth")) == "NOT AN EFFECT MODULE");
  CHECK(text(ui.insert_assign(ed, 1, 0, "nofile")) == "MODULE MISSING");  // not in the set: asked of the card, which has no such file
  CHECK(text(ui.insert_assign(ed, 17, 0, "biquad")) == "NO SUCH INSERT");  // (track 16 is the master, S6: it has an insert; 17 is nothing)
  CHECK(text(ui.insert_assign(ed, 1, 2, "biquad")) == "NO SUCH INSERT");
  CHECK(rig.project().find_insert(1, 0) == nullptr);  // the refusals changed nothing
  CHECK(ui.insert_bypass(ed, 0, 0, true) == R::Ok);  // `ins bypass 1 1 on`
  CHECK((rig.project().find_insert(0, 0)->flags & model::kInsertFlagBypass) != 0u);
  CHECK(text(ui.insert_bypass(ed, 3, 0, true)) == "NO INSERT HERE");
  CHECK(ui.insert_remove(ed, 0, 0) == R::Ok);  // `ins unbind 1 1`
  CHECK(rig.project().find_insert(0, 0) == nullptr);
  CHECK(text(ui.insert_remove(ed, 0, 0)) == "NO INSERT HERE");
  CHECK(text(ui.insert_remove(ed, 17, 0)) == "NO SUCH INSERT");
}

TEST_CASE("mixer inserts: an empty modules/effects folder, and no card at all, give a picker with no module from the card that says NO SUCH EFFECT, not a crash") {
  Rig rig;
  std::error_code ec;
  for (const char* f : {"biquad", "chorus", "synth"}) {
    fs::remove(rig.card.root / "modules" / "effects" / (std::string(f) + ".tmod"), ec);
  }
  rig.inserts_page();
  rig.tap(Button::Insert);
  CHECK(rig.app->state().overlay == app::OverlayId::ModulePicker);
  CHECK(rig.mods->ui.effect_count() == 0u);  // (reverb is in MODR only: no insert record names it)
  rig.tap(Button::F1);
  CHECK(rig.last_note() == "NO SUCH EFFECT");
  CHECK(rig.project().inserts().count == 0u);
  rig.tap(Button::F8);
  // no card layer at all
  rig.mods->ui.set_storage(nullptr);
  rig.tap(Button::Insert);
  CHECK(rig.app->state().overlay == app::OverlayId::ModulePicker);
  CHECK(rig.mods->ui.effect_count() == 0u);
  rig.pass(3000);
  rig.tap(Button::F1);
  CHECK(rig.last_note() == "NO SUCH EFFECT");
  CHECK(rig.project().inserts().count == 0u);
}

TEST_CASE("mixer inserts: the effects' list names as missing only what an INSX record uses, the instruments' list never an inserted effect") {
  Rig rig;  // MODR names reverb; no INSX record uses it
  app::ModuleUi& ui = rig.mods->ui;
  ui.scan_effects(&rig.project());
  CHECK(ui.effect_count() == 3u);  // the card's three, no greyed line
  ui.scan_instruments(&rig.project());
  const unsigned before = ui.instrument_count();
  bool listed = false;
  for (unsigned i = 0; i < before; ++i) {
    listed = listed || std::strcmp(ui.instrument_at(i)->stem, "reverb") == 0;
  }
  CHECK(listed);  // a module the project names and no insert uses is an engine the card lacks: greyed in the engine picker as before
  rig.record(7, 1, "reverb");
  ui.scan_effects(&rig.project());
  CHECK(ui.effect_count() == 4u);
  CHECK(ui.effect_at(3)->state == app::CatalogEntry::State::Missing);
  CHECK(std::strcmp(ui.effect_at(3)->stem, "reverb") == 0);
  ui.scan_instruments(&rig.project());
  CHECK(ui.instrument_count() == before - 1u);  // an inserted effect is not an engine
  for (unsigned i = 0; i < ui.instrument_count(); ++i) {
    CHECK(std::strcmp(ui.instrument_at(i)->stem, "reverb") != 0);
  }
}

// ---- OS-INSFX S5: the generated parameter page of an insert (docs/specs/OS-INSFX.md section 5, S5) -------------------------------------------------

TEST_CASE("insert params page: Enter (Params) on a bound INSERTS cell opens the insert's page; an unbound cell and an empty master cell do not") {
  Rig rig;
  rig.inserts_page();
  rig.assign(2, 1, 0);  // T3 INS B: biquad, the host's where 5
  CHECK(rig.last_note() == "INSERT SET");
  rig.blocks(4);
  rig.cell(2, 1);
  rig.tap(Button::Params);  // Enter
  const app::ViewState& v = rig.app->state();
  const app::Model& m = rig.app->model();
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK(v.params.insert == 5u);
  CHECK(app::params_insert_target(v));
  CHECK(app::kParamsScreen.count(m, v) == 1u);  // biquad's one parameter
  const core::ParamId id = core::param_id(core::ParamKind::Insert, 5, 0);
  CHECK(app::kParamsScreen.focus_param(m, v) == id);
  CHECK(app::is_insert_param(id));
  app::ParamScratch scratch;
  NEED(app::describe_param(m, id, scratch) != nullptr);
  CHECK(std::strcmp(scratch.desc.name, "GAIN") == 0);
  // the values come from the record's blob (the assign wrote the module's default)
  float value = 0.0f;
  CHECK(app::read_param(m, id, value));
  CHECK(value == 1.0f);
  // Exit (the Mixer's screen button) returns to the INSERTS page on the same cell, and the target is cleared
  rig.tap(Button::Master);
  CHECK(v.screen == app::ScreenId::Mixer);
  CHECK(app::mixer::page_of(v) == app::MixerPage::Inserts);
  const app::mixer::Cursor cur = app::mixer::cursor(m, v);
  CHECK(cur.row == 2u);
  CHECK(cur.col == 1u);
  CHECK(v.params.insert == app::ParamView::kNoInsert);
  CHECK_FALSE(app::params_insert_target(v));
  // an unbound cell does not open: the page stays on the Mixer
  rig.pass(3000);
  rig.cell(0, 0);
  rig.tap(Button::Params);
  CHECK(v.screen == app::ScreenId::Mixer);
  CHECK(app::mixer::page_of(v) == app::MixerPage::Inserts);
  CHECK(rig.last_note() == "NO INSERT HERE");
  // the master's row with no insert does not open either (S6: it has one cell, empty here)
  rig.cell(12, 0);
  rig.tap(Button::Params);
  CHECK(v.screen == app::ScreenId::Mixer);
  CHECK(rig.last_note() == "NO INSERT HERE");
  // Params on another Mixer page keeps its global meaning: the instrument in view's page
  rig.tap(Button::F1);  // the MIXER page
  rig.tap(Button::Params);
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK_FALSE(app::params_insert_target(v));
}

// OS-INSKEYS: F6 on the INSERTS page is PARAMS (as the Params button), F6 on the insert's page is BACK to the same cell, F7 and Shift + F6 / F7 do nothing there; the other Mixer pages keep F6.
TEST_CASE("insert params page: F6 PARAMS on the INSERTS page, F6 BACK to the same cell, F7 and Shift + F6 / F7 do nothing, F6 elsewhere on the Mixer is unchanged") {
  Rig rig;
  rig.inserts_page();
  rig.assign(2, 1, 0);  // T3 INS B: biquad, where 5
  rig.blocks(4);
  const app::ViewState& v = rig.app->state();
  const app::Model& m = rig.app->model();
  // the INSERTS row reads PARAMS on F6
  CHECK(app::kMixerInsertKeys[5] == std::string("PARAMS"));
  // an empty cell: F6 says NO INSERT HERE and stays
  rig.pass(3000);
  rig.cell(0, 0);
  rig.tap(Button::F6);
  CHECK(v.screen == app::ScreenId::Mixer);
  CHECK(app::mixer::page_of(v) == app::MixerPage::Inserts);
  CHECK(rig.last_note() == "NO INSERT HERE");
  // a bound cell: F6 opens its page
  rig.cell(2, 1);
  rig.tap(Button::F6);
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK(v.params.insert == 5u);
  CHECK(app::kParamsScreen.field_context(m, v) == app::Context::InsertParams);
  // the labels: F6 BACK, F7 blank; shifted, F6 and F7 blank
  app::ParamsKeyRow row{};
  app::params_key_row(m, v, row);
  CHECK(std::string(row.label[5]) == "BACK");
  CHECK(std::string(row.label[6]).empty());
  CHECK((row.dim & ((1u << 5) | (1u << 6))) == 0u);
  // F7 and Shift + F6 / F7 do nothing: the screen, the target and the instrument in view stay
  const int instrument = v.instrument;
  rig.tap(Button::F7);
  rig.press(Button::Shift);
  rig.pass(1);
  app::params_key_row(m, v, row);
  CHECK(std::string(row.label[5]).empty());
  CHECK(std::string(row.label[6]).empty());
  rig.tap(Button::F6);
  rig.tap(Button::F7);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK(v.params.insert == 5u);
  CHECK(v.instrument == instrument);
  // F6 is BACK: the INSERTS page, the same cell, the target cleared
  rig.tap(Button::F6);
  CHECK(v.screen == app::ScreenId::Mixer);
  CHECK(app::mixer::page_of(v) == app::MixerPage::Inserts);
  const app::mixer::Cursor cur = app::mixer::cursor(m, v);
  CHECK(cur.row == 2u);
  CHECK(cur.col == 1u);
  CHECK(v.params.insert == app::ParamView::kNoInsert);
  // the instrument's own page is unchanged: F6 steps to the previous instrument in view, F7 to the next
  rig.tap(Button::F1);  // the MIXER page
  rig.tap(Button::Params);
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK_FALSE(app::params_insert_target(v));
  CHECK(app::kParamsScreen.field_context(m, v) != app::Context::InsertParams);
  const int here = v.instrument;
  rig.tap(Button::F7);
  CHECK(v.instrument == here + 1);
  rig.tap(Button::F6);
  CHECK(v.instrument == here);
  // F6 on the other Mixer pages is the page key (INSERTS from MIXER, ROUTING, MST FX; STEMS keeps its APPLY: mixer_test.cpp)
  rig.tap(Button::Master);
  CHECK(v.screen == app::ScreenId::Mixer);
  rig.tap(Button::F1);
  rig.tap(Button::F6);
  CHECK(v.screen == app::ScreenId::Mixer);
  CHECK(app::mixer::page_of(v) == app::MixerPage::Inserts);
  rig.tap(Button::F2);
  CHECK(app::mixer::page_of(v) == app::MixerPage::Routing);
  rig.tap(Button::F6);
  CHECK(app::mixer::page_of(v) == app::MixerPage::Inserts);
  rig.tap(Button::F3);
  rig.tap(Button::F6);
  CHECK(app::mixer::page_of(v) == app::MixerPage::Inserts);
}

TEST_CASE("insert params page: the pages are the module's, an edit writes the INSX blob and sends ParamChange{Insert}, fine and coarse, Delete the default, no undo entry") {
  Rig rig;
  rig.inserts_page();
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "paged") == app::ModuleUi::InsertResult::Ok);  // T2 INS A, where 2, ten parameters
  rig.blocks(4);
  const model::Project& p = rig.project();
  const model::InsertRecord* rec = p.find_insert(1, 0);
  NEED(rec != nullptr);
  CHECK(rec->n == 10u);
  CHECK(rec->param[0] == 0.5f);
  rig.cell(1, 0);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  const app::Model& m = rig.app->model();
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK(v.params.insert == 2u);
  // page 1 of 2: eight rows, the first is P00 = ParamKind::Insert, where 2, field 0
  CHECK(app::kParamsScreen.count(m, v) == 8u);
  CHECK(app::kParamsScreen.focus_param(m, v) == core::param_id(core::ParamKind::Insert, 2, 0));
  // F2: the second page, two rows, the first is field 8
  rig.tap(Button::F2);
  CHECK(app::kParamsScreen.count(m, v) == 2u);
  CHECK(app::kParamsScreen.focus_param(m, v) == core::param_id(core::ParamKind::Insert, 2, 8));
  rig.tap(Button::F1);
  CHECK(app::kParamsScreen.count(m, v) == 8u);
  // no undo entry before the edit
  const unsigned depth_before = static_cast<unsigned>(rig.editor->journal(model::UndoArea::Sound).depth());
  // the encoder: a fine step (1/100 of the 0..1 range)
  rig.turn(10);
  rec = p.find_insert(1, 0);
  NEED(rec != nullptr);
  CHECK(std::fabs(rec->param[0] - 0.6f) < 1e-6f);  // the blob is written through the editor
  // the ParamChange{Insert} went to the graph's ring (the host's, with the binding's generation)
  core::ParamChange c{};
  CHECK(rig.g->params().pop(c));
  CHECK(c.id == core::param_id(core::ParamKind::Insert, 2, 0));
  CHECK(std::fabs(c.value - 0.6f) < 1e-6f);
  CHECK(c.ramp_samples != 0u);  // the binding's generation
  (void)rig.g->params().push(c);  // back: the render's drain applies it at the block start
  rig.blocks(2);
  NEED(rig.fx_state(2) != nullptr);
  CHECK(std::fabs(rig.fx_state(2)->values[0] - 0.6f) < 1e-6f);
  // no undo entry: the depth is unchanged and F8 says so (D9)
  CHECK(static_cast<unsigned>(rig.editor->journal(model::UndoArea::Sound).depth()) == depth_before);
  rig.pass(3000);
  rig.tap(Button::F8);
  CHECK(rig.last_note() == "NOTHING TO UNDO HERE");
  CHECK(static_cast<unsigned>(rig.editor->journal(model::UndoArea::Sound).depth()) == depth_before);
  // Shift + the encoder: the coarse step (ten fine ones)
  rig.press(Button::Shift);
  rig.turn(1);
  rig.release(Button::Shift);
  rec = p.find_insert(1, 0);
  CHECK(std::fabs(rec->param[0] - 0.7f) < 1e-5f);
  CHECK(rig.g->params().pop(c));
  CHECK(std::fabs(c.value - 0.7f) < 1e-5f);
  (void)rig.g->params().push(c);
  // Delete: the row's default, still no undo entry
  rig.tap(Button::Delete);
  rec = p.find_insert(1, 0);
  CHECK(std::fabs(rec->param[0] - 0.5f) < 1e-6f);
  CHECK(static_cast<unsigned>(rig.editor->journal(model::UndoArea::Sound).depth()) == depth_before);
  // a row past the record's blob extends it (the module's defaults fill the gap): page 2, row 0 is field 8
  rig.tap(Button::F2);
  rig.turn(20);  // field 8 was never set: the blob had n = 10 already, so this just edits it
  rec = p.find_insert(1, 0);
  CHECK(std::fabs(rec->param[8] - 0.7f) < 1e-5f);
  // Exit returns to the INSERTS page on the same cell
  rig.tap(Button::Master);
  CHECK(v.screen == app::ScreenId::Mixer);
  CHECK(app::mixer::page_of(v) == app::MixerPage::Inserts);
  const app::mixer::Cursor cur = app::mixer::cursor(m, v);
  CHECK(cur.row == 1u);
  CHECK(cur.col == 0u);
  CHECK(v.params.insert == app::ParamView::kNoInsert);
}

TEST_CASE("insert params page: a short blob is extended with the module's defaults when a row past it is edited") {
  Rig rig;
  // "paged" is native, not in MODR: add it through the project, then a record whose blob holds only the first parameter (what a project file can hold)
  CHECK(rig.project().add_module("paged", 5, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  const unsigned ref = rig.project().module_count() - 1u;
  model::InsertRecord r{};
  r.where = 3;
  r.slot = 1;
  r.module = static_cast<std::uint8_t>(ref);
  r.n = 1u;
  r.param[0] = 0.25f;
  CHECK(rig.editor->set_insert(r) == model::Status::Ok);
  // bind the host's instance at the cell (the load's path: the stored blob is applied to it), like InsertBinder does at a project load
  const int idx = rig.mods->set.find("paged");
  NEED(idx >= 0);
  const o3m_module& mod = *rig.mods->set.at(static_cast<unsigned>(idx))->entry;
  std::uint8_t* mem = nullptr;
  std::uint32_t addr = 0;
  NEED(rig.mods->set.cold().take(mod.state_bytes, 16, mem, addr));
  std::memset(mem, 0, mod.state_bytes);
  const model::InsertRecord* stored = rig.project().find_insert(3, 1);
  NEED(stored != nullptr);
  CHECK(rig.h->bind(3u * 2u + 1u, mod, module::fault_id("paged"), mem, reinterpret_cast<const std::uint8_t*>(stored->param), stored->n * 4u, true) ==
        audio::InsertHost::Bind::Ok);
  rig.blocks(4);
  // the page on the cell (the record exists, the module is loaded and bound)
  rig.inserts_page();
  rig.cell(3, 1);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  const app::Model& m = rig.app->model();
  CHECK(v.params.insert == 7u);
  // page 2, row 0 (field 8): the blob does not reach it, the page shows the module's default
  rig.tap(Button::F2);
  const core::ParamId id = core::param_id(core::ParamKind::Insert, 7, 8);
  CHECK(app::kParamsScreen.focus_param(m, v) == id);
  float shown = 0.0f;
  CHECK(app::read_param(m, id, shown));
  CHECK(shown == 0.5f);  // the default, not a gap
  // editing it extends the blob: the defaults fill 1..7, the value goes to 8
  rig.turn(10);
  const model::InsertRecord* rec = rig.project().find_insert(3, 1);
  NEED(rec != nullptr);
  CHECK(rec->n == 9u);
  CHECK(rec->param[0] == 0.25f);  // the stored value is kept
  CHECK(rec->param[1] == 0.5f);
  CHECK(rec->param[7] == 0.5f);
  CHECK(std::fabs(rec->param[8] - 0.6f) < 1e-6f);
  CHECK(model::validate_insert_state(rig.project().inserts(), rig.project().module_count()) == nullptr);  // what a save would accept
  // and the module heard the new value at the block start
  core::ParamChange c{};
  CHECK(rig.g->params().pop(c));
  CHECK(c.id == id);
  (void)rig.g->params().push(c);  // back: the render's drain applies it
  rig.blocks(2);
  NEED(rig.fx_state(7) != nullptr);
  CHECK(std::fabs(rig.fx_state(7)->values[8] - 0.6f) < 1e-6f);
}

TEST_CASE("insert params page: the module's views are the page's, for the bound instance only") {
  Rig rig;
  const model::Project& p = rig.project();
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 0, 0, "paged") == app::ModuleUi::InsertResult::Ok);  // where 0
  rig.blocks(4);
  const app::ParamTarget target = app::ParamTarget::of_insert(0);
  CHECK(rig.mods->ui.view_count(p, target) == 1u);       // bound: the view is there
  CHECK(rig.mods->ui.param_count(p, target) == 10u);
  CHECK(rig.mods->ui.bound_module(p, target) != nullptr);
  // drawn for the instance's state, with the blob's values
  const ui::Theme& theme = ui::theme(ui::ThemeId::Amiga);
  g_fx_view_calls = 0;
  const std::vector<std::uint8_t> drawn = picture(theme, [&](ui::Renderer& r) {
    rig.mods->ui.begin_view_frame();
    CHECK(rig.mods->ui.draw_slot_view(r, p, target, 0, ui::Rect{0, 0, 120, 60}));
  });
  CHECK(g_fx_view_calls == 1u);
  CHECK(drawn.size() == sim::EveRaster::kFramebufferBytes);
  // a record without an instance (the module loaded, nothing bound there) has no views
  CHECK(rig.project().add_module("biquad", 6, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);  // into MODR, for the record below
  rig.record(5, 0, "biquad");  // biquad is loaded, no host instance at where 10
  CHECK(rig.mods->ui.view_count(p, app::ParamTarget::of_insert(10)) == 0u);
  // and a cell with no record at all has no module
  CHECK(rig.mods->ui.bound_module(p, app::ParamTarget::of_insert(11)) == nullptr);
  CHECK(rig.mods->ui.param_count(p, app::ParamTarget::of_insert(11)) == 0u);
  // without the host (a bare ModuleUi, the simulator) an insert has no views either
  Rig bare(false);
  CHECK(bare.project().add_module("biquad", 6, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  bare.record(5, 0, "biquad");
  CHECK(bare.mods->ui.view_count(bare.project(), app::ParamTarget::of_insert(10)) == 0u);
  CHECK(bare.mods->ui.bound_module(bare.project(), app::ParamTarget::of_insert(10)) != nullptr);  // the record's module is loaded: the page opens
}

// OS-INSFX S5 review: a full graph param ring is not a drop. InsertHost::set_param stores the value in the host blob and flags it dirty even when the ring is full (it returns false), and flush_params retries it
// on a later service tick, so send_param_change counts a drop only when no instance is bound. Here the ring is full: the edit is not counted, and once the render drains the filler the next service tick delivers
// the stored value.
TEST_CASE("insert params page: a full graph ring is not a drop: the host keeps the value dirty and the next service tick delivers it") {
  Rig rig;
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "paged") == app::ModuleUi::InsertResult::Ok);  // T2 INS A, the host's where 2
  rig.blocks(4);
  NEED(rig.instance(2).where == 2u);  // bound
  unsigned filled = 0;
  while (rig.g->params().push(core::ParamChange{audio::master_param(gpm::Volume), 1.0f, 1})) {
    ++filled;
  }
  CHECK(filled == core::kParamRingSize);  // the graph's ring is full
  const std::uint32_t drops = rig.app->session().param_drops;
  rig.inserts_page();
  rig.cell(1, 0);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK(v.params.insert == 2u);
  rig.turn(10);  // P00: 0.5 -> 0.6, written to the blob and stored in the host's copy
  const model::InsertRecord* rec = rig.project().find_insert(1, 0);
  NEED(rec != nullptr);
  CHECK(std::fabs(rec->param[0] - 0.6f) < 1e-6f);
  CHECK(rig.app->session().param_drops == drops);  // not a drop: the instance is bound, the value is in its blob, dirty
  core::ParamChange c{};
  CHECK(!rig.g->params().push(core::ParamChange{audio::master_param(gpm::Volume), 0.0f, 1}));  // the ring is still full: the host's flush_params could not push
  rig.blocks(4);                   // the render drains the filler; the service tick's flush_params delivers the stored value
  bool found = false;
  for (unsigned i = 0; i < core::kParamRingSize + 8u && rig.g->params().pop(c); ++i) {
    if (core::param_kind(c.id) == core::ParamKind::Insert) {
      found = true;
      break;
    }
  }
  CHECK(found);
  CHECK(c.id == core::param_id(core::ParamKind::Insert, 2, 0));
  CHECK(std::fabs(c.value - 0.6f) < 1e-6f);
  CHECK(c.ramp_samples != 0u);  // the binding's generation
  (void)rig.g->params().push(c);  // back: the render's drain applies it at the block start
  rig.blocks(2);
  NEED(rig.fx_state(2) != nullptr);
  CHECK(std::fabs(rig.fx_state(2)->values[0] - 0.6f) < 1e-6f);
}

// OS-INSFX S5 review: the other side of the same rule — an edit on a cell with no bound instance IS a drop (the blob is still written, a rebind applies it).
TEST_CASE("insert params page: an edit on a cell with no bound instance is counted as a drop") {
  Rig rig;
  CHECK(rig.project().add_module("paged", 5, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);  // into MODR, for the record below
  rig.record(5, 0, "paged");  // an INSX record with no host instance (the host's where 10)
  rig.inserts_page();
  rig.cell(5, 0);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK(v.params.insert == 10u);
  const std::uint32_t drops = rig.app->session().param_drops;
  rig.turn(10);  // P00: the default 0.5 -> 0.6
  CHECK(rig.app->session().param_drops == drops + 1u);  // no instance bound: a real drop
  const model::InsertRecord* rec = rig.project().find_insert(5, 0);
  NEED(rec != nullptr);
  CHECK(rec->n == 1u);  // the short blob was extended with the module's default
  CHECK(std::fabs(rec->param[0] - 0.6f) < 1e-6f);  // the blob is written all the same
}

// OS-INSFX S5 review: the insert target is view state, so a replaced project clears it too (a load, F3 NEW, the simulator's reset: the platform's finish_load and SimProject::finish both attach the document again).
// The Instrument params then show the instrument in view's page, not the insert's.
TEST_CASE("insert params page: replacing the project (attach_document) clears the insert target: the page is the instrument's again") {
  Rig rig;
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 2, 1, "biquad") == app::ModuleUi::InsertResult::Ok);  // T3 INS B, the host's where 5
  rig.blocks(4);
  rig.inserts_page();
  rig.cell(2, 1);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  const app::Model& m = rig.app->model();
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK(v.params.insert == 5u);
  CHECK(core::param_kind(app::kParamsScreen.focus_param(m, v)) == core::ParamKind::Insert);
  model::Editor reloaded(rig.project(), rig.journal);  // the platform's finish_load: history_reset, attach_document
  rig.app->attach_document(&reloaded);
  CHECK(v.screen == app::ScreenId::Instrument);  // the load does not change the screen
  CHECK(v.params.insert == app::ParamView::kNoInsert);
  CHECK_FALSE(app::params_insert_target(v));
  CHECK(core::param_kind(app::kParamsScreen.focus_param(m, v)) == core::ParamKind::Instrument);  // the instrument in view's page, not the insert's
  // and opening the Instrument params again shows the instrument page
  rig.tap(Button::Master);  // back to the Mixer's INSERTS page
  rig.tap(Button::F1);      // the MIXER page
  rig.tap(Button::Params);  // its global meaning: the instrument in view's page
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK(v.params.insert == app::ParamView::kNoInsert);
  CHECK(core::param_kind(app::kParamsScreen.focus_param(m, v)) == core::ParamKind::Instrument);
}

// OS-INSFX S5 review: on an insert page the Shift+F row is dimmed; NAME / COPY / PASTE / DELETE / INIT / SAVE are guarded and say nothing, and USED (Shift + F6 / F7) did nothing but take the page over to an
// instrument — now guarded too. The plain F6 / F7 keep their designed meaning (an instrument chosen on the page takes it over).
TEST_CASE("insert params page: the Shift+F keys do nothing on an insert page") {
  Rig rig;
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "paged") == app::ModuleUi::InsertResult::Ok);  // the host's where 2
  rig.blocks(4);
  rig.inserts_page();
  rig.cell(1, 0);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  CHECK(v.params.insert == 2u);
  const std::string note = rig.last_note();
  const int instrument = v.instrument;
  CHECK(rig.app->invoke(core::action_id("instrument.name")) == app::DispatchResult::Unchanged);
  CHECK(rig.app->invoke(core::action_id("instrument.copy")) == app::DispatchResult::Unchanged);
  CHECK(rig.app->invoke(core::action_id("instrument.paste")) == app::DispatchResult::Unchanged);
  CHECK(rig.app->invoke(core::action_id("instrument.delete")) == app::DispatchResult::Unchanged);
  CHECK(rig.app->invoke(core::action_id("instrument.init")) == app::DispatchResult::Unchanged);
  // OS-MODPRESET P4: SAVE does act on a bound insert's page (it asks for a preset name; this rig has no save layer, so it says so and the keyboard does not open)
  CHECK(rig.app->invoke(core::action_id("instrument.save")) == app::DispatchResult::Changed);
  CHECK(rig.last_note() == "NO CARD WRITE LAYER");
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  const std::string note2 = rig.last_note();
  CHECK(rig.app->invoke(core::action_id("instrument.used"), -1) == app::DispatchResult::Unchanged);
  CHECK(rig.app->invoke(core::action_id("instrument.used"), 1) == app::DispatchResult::Unchanged);
  CHECK(v.params.insert == 2u);       // the page is still the insert's
  CHECK(v.instrument == instrument);  // no instrument was chosen behind the dimmed keys
  CHECK(rig.last_note() == note2);    // and nothing was said since
  // the plain F6 / F7 still take the page over (an instrument chosen on the page, a pad or F6 / F7)
  CHECK(rig.app->invoke(core::action_id("instrument.step"), 1) == app::DispatchResult::Changed);
  CHECK(v.params.insert == app::ParamView::kNoInsert);
}

// ---- OS-INSFX S7: MIDI Learn on an insert's parameters --------------------------------------------------------------------------------------------------

namespace {

const core::ParamId kInsLrnCutoff = core::param_id(core::ParamKind::Insert, 2, 0);  // T2 INS A (where 2), "Cutoff"
const core::ParamId kInsLrnReso = core::param_id(core::ParamKind::Insert, 2, 1);

// T2 INS A holds "lrn" (three parameters), its page is open on the first row.
void open_lrn_page(Rig& rig) {
  rig.inserts_page();
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "lrn") == app::ModuleUi::InsertResult::Ok);
  rig.blocks(4);
  rig.cell(1, 0);
  rig.tap(Button::Params);
  NEED(rig.app->state().screen == app::ScreenId::Instrument);
  NEED(rig.app->state().params.insert == 2u);
}

const core::LearnBinding* learn_slots(Rig& rig) { return rig.project().settings().learn_slots(); }

// What the render applied to the insert's instance after the ring's entries are drained at the block start.
float heard_by_module(Rig& rig, unsigned field) {
  rig.blocks(2);
  const FxState* st = rig.fx_state(2);
  CHECK(st != nullptr);
  return st != nullptr ? st->values[field] : -1.0f;
}

}  // namespace

TEST_CASE("insert learn: a CC binds to the row of an insert's page, moves it live (the blob and the instance), and Unbind clears it") {
  Rig rig;
  open_lrn_page(rig);
  CHECK(app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()) == kInsLrnCutoff);
  rig.learn_toggle();
  CHECK(rig.app->learn());
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 64)) == app::DispatchResult::Changed);  // the overlay is up: the message binds to the row under the cursor
  CHECK(rig.last_note().rfind("CC 74 > ", 0) == 0u);
  CHECK(rig.project().settings().learn_count() == 1u);
  CHECK(rig.project().settings().learn(0).param == kInsLrnCutoff);
  CHECK(rig.project().settings().learn(0).cc == 74u);
  CHECK(rig.project().settings().learn(0).mode == static_cast<std::uint8_t>(core::LearnMode::Pickup));
  CHECK(core::learn_store_valid(learn_slots(rig), core::kLearnSlots));
  CHECK(rig.app->model().device.learn_count() == 0u);  // the project's store: it is saved with the project (LRNB)
  rig.tap(Button::F2);                                 // pickup -> jump (a knob far from the value must move it at once)
  CHECK(rig.project().settings().learn(0).mode == static_cast<std::uint8_t>(core::LearnMode::Jump));
  rig.learn_toggle();
  CHECK_FALSE(rig.app->learn());
  const model::Project& p = rig.project();
  const unsigned depth = static_cast<unsigned>(rig.editor->journal(model::UndoArea::Sound).depth());
  // the controller moves the parameter: the record's blob, the ring, the module's instance
  core::ParamChange c{};
  while (rig.g->params().pop(c)) {
  }
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 100)) == app::DispatchResult::Changed);
  const model::InsertRecord* rec = p.find_insert(1, 0);
  NEED(rec != nullptr);
  CHECK(rec->param[0] == doctest::Approx(100.0 / 127.0).epsilon(1.0e-4));
  CHECK(rig.g->params().pop(c));
  CHECK(c.id == kInsLrnCutoff);
  CHECK(c.value == doctest::Approx(100.0 / 127.0).epsilon(1.0e-4));
  CHECK(c.ramp_samples != 0u);  // the binding's generation
  (void)rig.g->params().push(c);
  CHECK(heard_by_module(rig, 0) == doctest::Approx(100.0 / 127.0).epsilon(1.0e-4));
  CHECK(p.get_param(kInsLrnCutoff) == rec->param[0]);
  CHECK(static_cast<unsigned>(rig.editor->journal(model::UndoArea::Sound).depth()) == depth);  // an insert's value is not an undo unit (D9)
  // another row is not moved by that controller
  CHECK(rec->param[1] == 0.5f);
  // Unbind (F1 in the overlay) takes the binding away; the message then does nothing
  rig.learn_toggle();
  rig.tap(Button::F1);
  CHECK(rig.last_note().rfind("UNBOUND ", 0) == 0u);
  CHECK(rig.project().settings().learn_count() == 0u);
  rig.learn_toggle();
  const float kept = rec->param[0];
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 10)) == app::DispatchResult::Unchanged);
  CHECK(p.find_insert(1, 0)->param[0] == kept);
}

TEST_CASE("insert learn: Pickup catches the knob, two controllers on two parameters of one insert do not make each other catch again, a screen edit does") {
  Rig rig;
  open_lrn_page(rig);
  rig.learn_toggle();
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 64)) == app::DispatchResult::Changed);  // CC 74 on Cutoff (row 0)
  rig.learn_toggle();
  rig.tap(Button::Down);
  CHECK(app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()) == kInsLrnReso);
  rig.learn_toggle();
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 75, 64)) == app::DispatchResult::Changed);  // CC 75 on Reso (row 1)
  rig.learn_toggle();
  CHECK(rig.project().settings().learn_count() == 2u);
  const model::Project& p = rig.project();
  CHECK(p.get_param(kInsLrnCutoff) == 0.5f);
  // pickup: a knob far from the value does nothing
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 100)) == app::DispatchResult::Unchanged);
  CHECK(p.get_param(kInsLrnCutoff) == 0.5f);
  // the knob arrives (the value is at 63.5 of 127): it catches, and from then on drives
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 64)) == app::DispatchResult::Changed);
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 75, 64)) == app::DispatchResult::Changed);
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 70)) == app::DispatchResult::Changed);  // Reso moved in between: Cutoff does not catch again
  CHECK(p.get_param(kInsLrnCutoff) == doctest::Approx(70.0 / 127.0).epsilon(1.0e-4));
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 75, 70)) == app::DispatchResult::Changed);
  CHECK(p.get_param(kInsLrnReso) == doctest::Approx(70.0 / 127.0).epsilon(1.0e-4));
  // the page's own encoder edits Reso (the cursor is on it): the CC must catch it again
  rig.turn(-10);
  CHECK(p.get_param(kInsLrnReso) < 70.0f / 127.0f - 0.05f);
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 75, 100)) == app::DispatchResult::Unchanged);
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 80)) == app::DispatchResult::Changed);  // Cutoff was not touched by the page
}

TEST_CASE("insert learn: a row the module marks O3M_PARAM_NO_LEARN is refused, and a binding to an insert that is gone does nothing") {
  Rig rig;
  open_lrn_page(rig);
  rig.tap(Button::Down, 2);  // "Mode", NO_LEARN
  CHECK(app::kParamsScreen.focus_param(rig.app->model(), rig.app->state()) == core::param_id(core::ParamKind::Insert, 2, 2));
  rig.learn_toggle();
  (void)rig.invoke("midi.cc", Rig::cc(1, 74, 64));
  CHECK(rig.last_note() == "NO TARGET TO LEARN");
  CHECK(rig.project().settings().learn_count() == 0u);
  // a learnable row binds, then the insert is deleted: the binding stays in the store (the spec's tolerant rule) and a message finds no parameter
  rig.learn_toggle();
  rig.tap(Button::Up, 2);
  rig.learn_toggle();
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 64)) == app::DispatchResult::Changed);
  rig.learn_toggle();
  CHECK(rig.project().settings().learn_count() == 1u);
  rig.tap(Button::Master);  // leave the page
  CHECK(rig.mods->ui.insert_remove(*rig.editor, 1, 0) == app::ModuleUi::InsertResult::Ok);
  CHECK(rig.project().find_insert(1, 0) == nullptr);
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 90)) == app::DispatchResult::Unchanged);
  CHECK(rig.project().settings().learn_count() == 1u);
  CHECK(core::learn_store_valid(learn_slots(rig), core::kLearnSlots));
}

// ---- OS-INSFX S6: the master insert on the Mixer's INSERTS page ---------------------------------------------------------------------------------

TEST_CASE("mixer inserts: the MASTER row takes an insert: two instances, the record is where 16, F7 bypasses the pair, Delete removes it") {
  Rig rig;
  rig.inserts_page();
  const model::Project& p = rig.project();
  rig.assign(12, 0, 0);  // the MASTER row (the demo has twelve tracks): biquad
  CHECK(rig.last_note() == "INSERT SET");
  const model::InsertRecord* rec = p.find_insert(model::kInsertWhereMaster, 0);
  NEED(rec != nullptr);
  CHECK(std::strcmp(p.module(rec->module)->name, "biquad") == 0);
  CHECK(rig.h->bound(audio::InsertHost::kMasterWhere));
  CHECK(rig.instance(audio::InsertHost::kMasterWhere).where == audio::InsertHost::kMasterWhere);  // L
  CHECK(rig.instance(audio::InsertHost::kMasterWhere + 1u).where == audio::InsertHost::kMasterWhere + 1u);  // R: its own instance
  CHECK(rig.fx_state(audio::InsertHost::kMasterWhere) != rig.fx_state(audio::InsertHost::kMasterWhere + 1u));  // each has a state block of its own
  CHECK(model::insert_instances(p.inserts()) == 2u);
  rig.blocks(4);
  app::ModuleUi::InsertCell c{};
  CHECK(rig.mods->ui.insert_cell(model::kInsertWhereMaster, 0, c));
  CHECK_FALSE(c.bypass);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.cell_text(model::kInsertWhereMaster, 0).find("NOT BOUND") == std::string::npos);

  // F7 bypasses the pair (the host's, and the record's flag); back on, a reset of both
  rig.cell(12, 0);
  rig.tap(Button::F7);
  CHECK(rig.last_note() == "INSERT BYPASSED");
  CHECK(rig.mods->ui.insert_cell(model::kInsertWhereMaster, 0, c));
  CHECK(c.bypass);
  CHECK((p.find_insert(model::kInsertWhereMaster, 0)->flags & model::kInsertFlagBypass) != 0u);
  rig.blocks(4);
  CHECK(rig.cell_text(model::kInsertWhereMaster, 0).find("BYPASS") != std::string::npos);
  rig.tap(Button::F7);
  CHECK(rig.last_note() == "INSERT ON");
  rig.blocks(16);
  CHECK(rig.mods->ui.insert_cell(model::kInsertWhereMaster, 0, c));
  CHECK_FALSE(c.bypass);
  CHECK(rig.h->bound(audio::InsertHost::kMasterWhere));

  // a taken cell: delete it first
  rig.tap(Button::Insert);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.last_note() == "DELETE THE INSERT FIRST");

  // Delete: the pair leaves and the record goes
  rig.tap(Button::Delete);
  CHECK(rig.last_note() == "INSERT DELETED");
  CHECK(p.find_insert(model::kInsertWhereMaster, 0) == nullptr);
  rig.blocks(8);
  CHECK_FALSE(rig.h->bound(audio::InsertHost::kMasterWhere));
  CHECK_FALSE(rig.mods->ui.insert_cell(model::kInsertWhereMaster, 0, c));
  CHECK_FALSE(rig.g->master_stage_active(audio::MasterStage::Insert));
  rig.tap(Button::F7);
  CHECK(rig.last_note() == "NO INSERT HERE");
}

TEST_CASE("mixer inserts: the master's insert counts 2 of the cap of four: master + two tracks fill it, a third track and a master beside three tracks say INSERTS FULL") {
  {
    Rig rig;
    rig.inserts_page();
    const model::Project& p = rig.project();
    rig.assign(12, 0, 0);  // the master: two instances
    rig.assign(0, 0, 1);
    rig.assign(0, 1, 0);
    CHECK(p.inserts().count == 3u);  // three records, four instances
    CHECK(model::insert_instances(p.inserts()) == 4u);
    rig.cell(1, 0);
    rig.tap(Button::Insert);
    CHECK(rig.app->state().overlay == app::OverlayId::None);
    CHECK(rig.last_note() == "INSERTS FULL");
    CHECK(p.find_insert(1, 0) == nullptr);
    // the host agrees: a bind of a fourth track insert has no instance to take
    alignas(16) std::uint8_t state[sizeof(FxState)] = {};
    CHECK(rig.h->bind(2, kBiquad, 1, state, nullptr, 0, true) == audio::InsertHost::Bind::NoInstance);
  }
  {
    Rig rig;
    rig.inserts_page();
    const model::Project& p = rig.project();
    rig.assign(0, 0, 0);
    rig.assign(0, 1, 1);
    rig.assign(1, 0, 0);  // three track inserts: three of the four
    // OS-INSFX-ABI2 A7: the master's need is the chosen module's (one instance for a stereo effect), so with three taken the picker still opens; a MONO module is refused at the pick
    rig.cell(12, 0);
    rig.tap(Button::Insert);
    CHECK(rig.app->state().overlay == app::OverlayId::ModulePicker);
    rig.tap(Button::Down, 0);
    rig.tap(Button::F1);  // biquad: two instances beside the three tracks
    CHECK(rig.app->state().overlay == app::OverlayId::None);
    CHECK(rig.last_note() == "INSERTS FULL");
    CHECK(p.find_insert(model::kInsertWhereMaster, 0) == nullptr);
    CHECK_FALSE(rig.h->bound(audio::InsertHost::kMasterWhere));
    CHECK(rig.mods->ui.insert_assign(*rig.editor, model::kInsertWhereMaster, 0, "biquad") == app::ModuleUi::InsertResult::Full);  // the console path says the same
    // with one of the three removed the master fits
    rig.cell(1, 0);
    rig.tap(Button::Delete);
    rig.blocks(8);
    rig.pass(3000);  // (the strip's earlier notices run out first)
    rig.assign(12, 0, 0);
    CHECK(rig.last_note() == "INSERT SET");
    CHECK(rig.h->bound(audio::InsertHost::kMasterWhere));
  }
}

TEST_CASE("mixer inserts: a stereo effect on the master is ONE instance: the ST cell, three tracks plus the stereo master fill the cap, a fourth says INSERTS FULL [OS-INSFX-ABI2 A7]") {
  Rig rig;
  rig.inserts_page();
  const model::Project& p = rig.project();
  rig.assign(0, 0, 0);
  rig.assign(0, 1, 1);
  rig.assign(1, 0, 0);  // three track records: three of the four instances
  // the stereo effect on the master (the console path; "widen" is native, so its shape is known): ONE instance fits beside the three
  CHECK(rig.mods->ui.insert_assign(*rig.editor, model::kInsertWhereMaster, 0, "widen") == app::ModuleUi::InsertResult::Ok);
  CHECK(rig.last_note() == "INSERT SET");
  CHECK(rig.h->bound(audio::InsertHost::kMasterWhere));
  CHECK_FALSE(rig.h->bound(audio::InsertHost::kMasterWhere + 1u));  // not the pair's second record
  rig.blocks(4);
  CHECK(rig.instance(audio::InsertHost::kMasterWhere).where == audio::InsertHost::kMasterWhere);
  CHECK(rig.instance(audio::InsertHost::kMasterWhere + 1u).where == 0xFFu);  // no right record
  app::ModuleUi::InsertCell c{};
  CHECK(rig.mods->ui.insert_cell(model::kInsertWhereMaster, 0, c));
  CHECK(c.st);  // the ST mark (A5) is the module's, the master's cell included
  CHECK(rig.cell_text(model::kInsertWhereMaster, 0).rfind("ST", 0) == 0u);
  // the cap: the stereo master's record counts 1 when its module is stereo (the file alone counts 2)
  CHECK(model::insert_instances(p.inserts()) == 5u);  // the conservative file-side count: the master's record 2 + the three tracks (the host took 1 + 3: the binds above are what count)
  rig.cell(1, 1);
  rig.tap(Button::Insert);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.last_note() == "INSERTS FULL");  // four instances are taken
  CHECK(p.find_insert(1, 1) == nullptr);
  // a mono effect on the master beside the same three tracks still does not fit (S6: two instances)
  rig.cell(12, 0);
  rig.tap(Button::Delete);  // the stereo master out: three records
  rig.blocks(8);
  rig.pass(3000);
  CHECK(rig.mods->ui.insert_assign(*rig.editor, model::kInsertWhereMaster, 0, "biquad") == app::ModuleUi::InsertResult::Full);
  // with one of the three tracks removed the mono master fits (the pair: two records), and then nothing does
  rig.cell(1, 0);
  rig.tap(Button::Delete);
  rig.blocks(8);
  rig.pass(3000);
  CHECK(rig.mods->ui.insert_assign(*rig.editor, model::kInsertWhereMaster, 0, "biquad") == app::ModuleUi::InsertResult::Ok);
  rig.blocks(4);
  CHECK(rig.h->bound(audio::InsertHost::kMasterWhere));
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 2, 0, "biquad") == app::ModuleUi::InsertResult::Full);  // the pair took the last two records
}

TEST_CASE("mixer inserts: the master's console path: ModuleUi::insert_assign / insert_remove / insert_bypass with track 16, slot 0; slot 1 is refused") {
  Rig rig;
  app::ModuleUi& ui = rig.mods->ui;
  using R = app::ModuleUi::InsertResult;
  CHECK(ui.insert_assign(*rig.editor, model::kInsertWhereMaster, 1, "biquad") == R::BadWhere);  // the master has one cell
  CHECK(ui.insert_assign(*rig.editor, model::kInsertWhereMaster + 1u, 0, "biquad") == R::BadWhere);
  CHECK(ui.insert_remove(*rig.editor, model::kInsertWhereMaster, 1) == R::BadWhere);
  CHECK(ui.insert_bypass(*rig.editor, model::kInsertWhereMaster, 1, true) == R::BadWhere);
  CHECK(ui.insert_assign(*rig.editor, model::kInsertWhereMaster, 0, "biquad") == R::Ok);
  CHECK(ui.insert_assign(*rig.editor, model::kInsertWhereMaster, 0, "chorus") == R::Taken);
  rig.blocks(4);
  CHECK(ui.insert_bypass(*rig.editor, model::kInsertWhereMaster, 0, true) == R::Ok);
  CHECK((rig.project().find_insert(model::kInsertWhereMaster, 0)->flags & model::kInsertFlagBypass) != 0u);
  CHECK(ui.insert_bypass(*rig.editor, model::kInsertWhereMaster, 0, false) == R::Ok);
  rig.blocks(16);
  CHECK(ui.insert_remove(*rig.editor, model::kInsertWhereMaster, 0) == R::Ok);
  CHECK(ui.insert_remove(*rig.editor, model::kInsertWhereMaster, 0) == R::NoInsert);
  // a pair is released for the next bind of the same module: no new state blocks (the free list), whichever order the two records were freed in
  rig.blocks(8);
  CHECK(ui.insert_assign(*rig.editor, model::kInsertWhereMaster, 0, "biquad") == R::Ok);
  CHECK(ui.insert_assign(*rig.editor, 3, 0, "biquad") == R::Ok);
  CHECK(ui.insert_assign(*rig.editor, 4, 0, "biquad") == R::Ok);
  CHECK(ui.insert_assign(*rig.editor, 5, 0, "biquad") == R::Full);  // two for the master, two for the tracks
}

TEST_CASE("insert params page: Enter on the master's bound cell opens its page (target 32); an edit reaches BOTH instances and writes the record") {
  Rig rig;
  rig.inserts_page();
  rig.assign(12, 0, 0);  // the master: biquad
  rig.blocks(4);
  rig.cell(12, 0);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  const app::Model& m = rig.app->model();
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK(v.params.insert == audio::InsertHost::kMasterWhere);
  CHECK(app::params_insert_target(v));
  const core::ParamId id = core::param_id(core::ParamKind::Insert, audio::InsertHost::kMasterWhere, 0);
  CHECK(app::kParamsScreen.focus_param(m, v) == id);
  CHECK(app::is_insert_param(id));
  app::ParamScratch scratch;
  NEED(app::describe_param(m, id, scratch) != nullptr);
  CHECK(std::strcmp(scratch.desc.name, "GAIN") == 0);
  rig.turn(10);  // a fine step
  const model::InsertRecord* rec = rig.project().find_insert(model::kInsertWhereMaster, 0);
  NEED(rec != nullptr);
  CHECK(rec->param[0] != 1.0f);
  const float edited = rec->param[0];
  rig.blocks(4);  // the host's ParamChange (the key 32) is drained at a block start and reaches both instances
  NEED(rig.fx_state(audio::InsertHost::kMasterWhere) != nullptr);
  NEED(rig.fx_state(audio::InsertHost::kMasterWhere + 1u) != nullptr);
  CHECK(rig.fx_state(audio::InsertHost::kMasterWhere)->values[0] == edited);
  CHECK(rig.fx_state(audio::InsertHost::kMasterWhere + 1u)->values[0] == edited);
  // Exit returns to the INSERTS page on the master's cell
  rig.tap(Button::Master);
  CHECK(v.screen == app::ScreenId::Mixer);
  CHECK(app::mixer::page_of(v) == app::MixerPage::Inserts);
  CHECK(app::mixer::cursor(m, v).row == 12u);
  CHECK(v.params.insert == app::ParamView::kNoInsert);
}

// OS-INSFX S6 + S7: the master insert's Learn id has the index 32 (core::kInsertIdMaster). A binding loaded from LRNB (or made in the overlay) moves BOTH instances of the pair.
TEST_CASE("insert learn: a binding on the master insert's id (index 32), loaded as LRNB gives it or made in the overlay, moves the blob and BOTH instances, L and R") {
  Rig rig;
  const core::ParamId cutoff = core::param_id(core::ParamKind::Insert, audio::InsertHost::kMasterWhere, 0);
  CHECK(core::insert_param_valid(cutoff));
  CHECK(rig.mods->ui.insert_assign(*rig.editor, model::kInsertWhereMaster, 0, "lrn") == app::ModuleUi::InsertResult::Ok);
  rig.blocks(4);
  const model::Project& p = rig.project();
  // a binding as a loaded project holds it (the store of the file's LRNB), Jump so that the first message moves it
  const core::LearnBinding loaded{cutoff, 74, 0, static_cast<std::uint8_t>(core::LearnMode::Jump), 0};
  NEED(rig.editor->set_learn_slot(0, loaded) == model::Status::Ok);
  CHECK(rig.project().settings().learn_count() == 1u);
  core::ParamChange c{};
  while (rig.g->params().pop(c)) {
  }
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 100)) == app::DispatchResult::Changed);
  const model::InsertRecord* rec = p.find_insert(model::kInsertWhereMaster, 0);
  NEED(rec != nullptr);
  CHECK(rec->param[0] == doctest::Approx(100.0 / 127.0).epsilon(1.0e-4));
  rig.blocks(3);
  NEED(rig.fx_state(audio::InsertHost::kMasterWhere) != nullptr);
  NEED(rig.fx_state(audio::InsertHost::kMasterWhere + 1u) != nullptr);
  const float want = rec->param[0];
  CHECK(rig.fx_state(audio::InsertHost::kMasterWhere)->values[0] == want);       // L
  CHECK(rig.fx_state(audio::InsertHost::kMasterWhere + 1u)->values[0] == want);  // R
  CHECK(rig.h->stats().stale == 0u);
  // a second move changes both again
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 74, 20)) == app::DispatchResult::Changed);
  rig.blocks(3);
  const float want2 = p.find_insert(model::kInsertWhereMaster, 0)->param[0];
  CHECK(want2 != want);
  CHECK(rig.fx_state(audio::InsertHost::kMasterWhere)->values[0] == want2);
  CHECK(rig.fx_state(audio::InsertHost::kMasterWhere + 1u)->values[0] == want2);

  // bound in the overlay on the master's page: the same id, the same two instances
  rig.inserts_page();
  rig.cell(12, 0);
  rig.tap(Button::Params);
  NEED(rig.app->state().params.insert == audio::InsertHost::kMasterWhere);
  rig.learn_toggle();
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 75, 64)) == app::DispatchResult::Changed);
  CHECK(rig.project().settings().learn_count() == 1u);  // (a parameter has one binding: this controller replaces the loaded one)
  CHECK(rig.project().settings().learn(0).param == cutoff);
  CHECK(rig.project().settings().learn(0).cc == 75u);
  if (rig.project().settings().learn(0).mode != static_cast<std::uint8_t>(core::LearnMode::Jump)) {
    rig.tap(Button::F2);  // Jump
  }
  CHECK(rig.project().settings().learn(0).mode == static_cast<std::uint8_t>(core::LearnMode::Jump));
  rig.learn_toggle();
  CHECK(rig.invoke("midi.cc", Rig::cc(1, 75, 127)) == app::DispatchResult::Changed);
  rig.blocks(3);
  CHECK(rig.fx_state(audio::InsertHost::kMasterWhere)->values[0] == doctest::Approx(1.0f));
  CHECK(rig.fx_state(audio::InsertHost::kMasterWhere + 1u)->values[0] == doctest::Approx(1.0f));
}

// ---- OS-INSFX-ABI2 A8: factory presets on an insert (docs/specs/OS-INSFX-ABI2.md section 3.6) -------------------------------------------------------------------------------------

TEST_CASE("insert presets: the module's table is the insert's list; a module with none lists none") {
  Rig rig;
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "pfx") == app::ModuleUi::InsertResult::Ok);     // where 2
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 1, "biquad") == app::ModuleUi::InsertResult::Ok);  // where 3: no presets
  const model::Project& p = rig.project();
  const app::ParamTarget pfx = app::ParamTarget::of_insert(2);
  CHECK(rig.mods->ui.preset_count(p, pfx) == 2u);
  app::ParamSet sets[3];
  CHECK(rig.mods->ui.factory_presets(p, pfx, sets, 3) == 2u);
  CHECK(std::strcmp(sets[0].name, "Wide") == 0);
  CHECK(sets[0].given() == 32u);
  CHECK(sets[0].source == app::ParamSet::Source::Factory);
  CHECK(std::strcmp(sets[1].name, "Two") == 0);
  CHECK(sets[1].given() == 2u);
  CHECK(rig.mods->ui.preset_count(p, app::ParamTarget::of_insert(3)) == 0u);
  CHECK(rig.mods->ui.preset_count(p, app::ParamTarget::of_insert(4)) == 0u);  // no record there
  app::ParamSet none;
  CHECK(!rig.mods->ui.factory_preset(p, app::ParamTarget::of_insert(3), 0, none));
}

TEST_CASE("insert presets: applying one writes every parameter into the INSX blob, sends 32 ParamChanges with the binding's generation, and leaves no undo entry") {
  Rig rig;
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "pfx") == app::ModuleUi::InsertResult::Ok);  // T2 INS A, where 2
  rig.blocks(4);
  const model::Project& p = rig.project();
  const app::ParamTarget target = app::ParamTarget::of_insert(2);
  const model::InsertRecord* rec = p.find_insert(1, 0);
  NEED(rec != nullptr);
  CHECK(rec->n == 32u);
  CHECK(rec->param[0] == 0.5f);  // the module's defaults, from the bind
  app::ParamSet wide;
  NEED(rig.mods->ui.factory_preset(p, target, 0, wide));
  const std::size_t depth[3] = {rig.editor->journal(model::UndoArea::Pattern).depth(), rig.editor->journal(model::UndoArea::Song).depth(), rig.editor->journal(model::UndoArea::Sound).depth()};
  unsigned dropped = 99;
  CHECK(rig.mods->ui.apply_preset(*rig.editor, target, wide, &dropped) == app::ModuleUi::PresetResult::Ok);
  CHECK(dropped == 0u);
  rec = p.find_insert(1, 0);
  NEED(rec != nullptr);
  CHECK(rec->n == 32u);
  for (unsigned i = 0; i < 32; ++i) {
    CHECK(rec->param[i] == kPfxWide[i]);
  }
  CHECK(model::validate_insert_state(p.inserts(), p.module_count()) == nullptr);  // what a save would accept
  // thirty-two changes in the graph's ring, in parameter order, each with the binding's generation
  core::ParamChange got[32] = {};
  for (unsigned i = 0; i < 32; ++i) {
    NEED(rig.g->params().pop(got[i]));
    CHECK(got[i].id == core::param_id(core::ParamKind::Insert, 2, i));
    CHECK(got[i].value == kPfxWide[i]);
    CHECK(got[i].ramp_samples != 0u);
  }
  core::ParamChange extra{};
  CHECK(!rig.g->params().pop(extra));  // and no more than that
  for (const core::ParamChange& c : got) {
    CHECK(rig.g->params().push(c));  // back: the render's drain applies them at the block start
  }
  rig.blocks(2);
  NEED(rig.fx_state(2) != nullptr);
  for (unsigned i = 0; i < 32; ++i) {
    CHECK(rig.fx_state(2)->values[i] == kPfxWide[i]);
  }
  // not an undo step (D9): no ring gained a unit, and Undo has nothing to take back
  CHECK(rig.editor->journal(model::UndoArea::Pattern).depth() == depth[0]);
  CHECK(rig.editor->journal(model::UndoArea::Song).depth() == depth[1]);
  CHECK(rig.editor->journal(model::UndoArea::Sound).depth() == depth[2]);
}

TEST_CASE("insert presets: a preset of two values sets the other thirty to their defaults, a whole-number value is rounded, a value past the range is held to it") {
  Rig rig;
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "pfx") == app::ModuleUi::InsertResult::Ok);
  rig.blocks(4);
  const model::Project& p = rig.project();
  const app::ParamTarget target = app::ParamTarget::of_insert(2);
  app::ParamSet wide;
  app::ParamSet two;
  NEED(rig.mods->ui.factory_preset(p, target, 0, wide));
  NEED(rig.mods->ui.factory_preset(p, target, 1, two));
  CHECK(rig.mods->ui.apply_preset(*rig.editor, target, wide) == app::ModuleUi::PresetResult::Ok);
  CHECK(rig.mods->ui.apply_preset(*rig.editor, target, two) == app::ModuleUi::PresetResult::Ok);  // over the first: nothing of it is left but the defaults
  const model::InsertRecord* rec = p.find_insert(1, 0);
  NEED(rec != nullptr);
  CHECK(rec->param[0] == 0.2f);
  CHECK(rec->param[1] == 7.0f);
  for (unsigned i = 2; i < 32; ++i) {
    CHECK(rec->param[i] == 0.5f);
  }
  // a card's set: 99 on a 0..1 float, -3 on the INT 0..10, NaN
  app::ParamSet card;
  card.source = app::ParamSet::Source::Card;
  CHECK(card.set_value(0, 99.0f));
  CHECK(card.set_value(1, -3.0f));
  CHECK(card.set_value(2, std::numeric_limits<float>::quiet_NaN()));
  CHECK(rig.mods->ui.apply_preset(*rig.editor, target, card) == app::ModuleUi::PresetResult::Ok);
  rec = p.find_insert(1, 0);
  NEED(rec != nullptr);
  CHECK(rec->param[0] == 1.0f);
  CHECK(rec->param[1] == 0.0f);
  CHECK(rec->param[2] == 0.5f);
  CHECK(model::validate_insert_state(p.inserts(), p.module_count()) == nullptr);
}

TEST_CASE("insert presets: with the graph's ring full the 32 values are kept by the host and delivered by the next ticks, and none is counted as a drop") {
  Rig rig;
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "pfx") == app::ModuleUi::InsertResult::Ok);
  rig.blocks(4);
  const model::Project& p = rig.project();
  const app::ParamTarget target = app::ParamTarget::of_insert(2);
  app::ParamSet wide;
  NEED(rig.mods->ui.factory_preset(p, target, 0, wide));
  while (rig.g->params().push(core::ParamChange{audio::master_param(gpm::Volume), 1.0f, 1})) {
  }
  unsigned dropped = 99;
  CHECK(rig.mods->ui.apply_preset(*rig.editor, target, wide, &dropped) == app::ModuleUi::PresetResult::Ok);
  CHECK(dropped == 0u);  // the instance is bound: the values are in the host's blob, dirty
  const model::InsertRecord* rec = p.find_insert(1, 0);
  NEED(rec != nullptr);
  CHECK(rec->param[31] == kPfxWide[31]);  // the project has all of them
  std::uint32_t seen = 0;
  core::ParamChange c{};
  std::vector<core::ParamChange> mine;
  for (unsigned round = 0; round < 8 && seen != 0xFFFFFFFFu; ++round) {
    rig.blocks(1);  // the render drains the ring; the service tick pushes what was held back
    for (unsigned i = 0; i < core::kParamRingSize + 8u && rig.g->params().pop(c); ++i) {
      if (core::param_kind(c.id) == core::ParamKind::Insert) {
        CHECK(core::param_index(c.id) == 2u);
        CHECK(c.value == kPfxWide[core::param_field(c.id)]);
        seen |= std::uint32_t{1} << core::param_field(c.id);
        mine.push_back(c);
      }
    }
  }
  CHECK(seen == 0xFFFFFFFFu);  // every parameter reached the ring
  for (const core::ParamChange& m : mine) {
    (void)rig.g->params().push(m);
  }
  rig.blocks(2);
  NEED(rig.fx_state(2) != nullptr);
  for (unsigned i = 0; i < 32; ++i) {
    CHECK(rig.fx_state(2)->values[i] == kPfxWide[i]);
  }
}

TEST_CASE("insert presets: a record with no instance takes the blob and counts every value as a drop; a cell with no record is refused") {
  Rig rig;
  CHECK(rig.project().add_module("pfx", 3, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  rig.record(5, 0, "pfx");  // an INSX record with no host instance (where 10)
  const model::Project& p = rig.project();
  const app::ParamTarget target = app::ParamTarget::of_insert(10);
  app::ParamSet wide;
  NEED(rig.mods->ui.factory_preset(p, target, 0, wide));  // the module is in the set: the list is there without an instance
  unsigned dropped = 0;
  CHECK(rig.mods->ui.apply_preset(*rig.editor, target, wide, &dropped) == app::ModuleUi::PresetResult::Ok);
  CHECK(dropped == 32u);  // nothing bound: a rebind starts from the blob
  const model::InsertRecord* rec = p.find_insert(5, 0);
  NEED(rec != nullptr);
  CHECK(rec->n == 32u);
  CHECK(rec->param[17] == kPfxWide[17]);
  CHECK(rig.mods->ui.apply_preset(*rig.editor, app::ParamTarget::of_insert(12), wide, &dropped) == app::ModuleUi::PresetResult::NoModule);
  CHECK(dropped == 0u);
}


TEST_CASE("mixer inserts: a stereo effect is marked ST on the cell (and the picker line reads the same flag) [OS-INSFX-ABI2 A5]") {
  Rig rig;
  rig.inserts_page();
  // a mono effect first: no ST anywhere in the cell's text
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 0, 0, "biquad") == app::ModuleUi::InsertResult::Ok);
  rig.sound(0, 500);
  rig.blocks(4);
  app::ModuleUi::InsertCell c{};
  CHECK(rig.mods->ui.insert_cell(0, 0, c));
  CHECK_FALSE(c.st);
  CHECK(rig.cell_text(0, 0).find("ST") == std::string::npos);
  CHECK_FALSE(rig.mods->ui.effect_st("biquad"));
  // a WIDENS effect on T2 INS A: ST leads the cell's text, the cost follows
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "widen") == app::ModuleUi::InsertResult::Ok);
  rig.sound(1, 500);
  rig.blocks(4);
  CHECK(rig.mods->ui.insert_cell(1, 0, c));
  CHECK(c.st);
  const std::string txt = rig.cell_text(1, 0);
  CHECK(txt.rfind("ST", 0) == 0u);                       // the mark leads
  CHECK(txt.find("UNMEASURED") == std::string::npos);    // the insert was measured
  CHECK(rig.mods->ui.effect_st("widen"));
  CHECK_FALSE(rig.mods->ui.effect_st("nofile"));         // not in the set: not stereo
  // the master's cell reads the same flag (the pair's module is the stereo one)
  CHECK(rig.mods->ui.insert_assign(*rig.editor, model::kInsertWhereMaster, 0, "widen") == app::ModuleUi::InsertResult::Ok);
  rig.blocks(4);
  CHECK(rig.mods->ui.insert_cell(model::kInsertWhereMaster, 0, c));
  CHECK(c.st);
  CHECK(rig.cell_text(model::kInsertWhereMaster, 0).rfind("ST", 0) == 0u);
}

// ---- OS-INSFX-ABI2 A6, the page level (QA-MIX-102): a bound 1.12 effect's KEY row reads OFF, T1..T16 and refuses Learn; a 1.11 effect has no KEY ------------

// A 1.12 effect with a KEY parameter (INT 0..16). It also carries O3M_PARAM_MOD on purpose: the review fold clears that flag for a KEY (a modulator must not re-aim
// the key), pinned below. A 1.11 effect with the same flagged parameter: its KEY bit is not honoured (key_ok is false), so the row is a plain INT.
const o3m_param kKeyParams[2] = {{"Gain", 0.0f, 4.0f, 1.0f, 0}, {"Key", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY | O3M_PARAM_MOD}};
const o3m_module kKeyFx = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), "key12", sizeof(FxState), 2, kKeyParams,
                           fx_init, fx_render, fx_set_param, fx_note_on, fx_note_off, fx_shutdown, nullptr, 0};
const o3m_module kKeyOld = {O3M_ABI_MAJOR, 11, O3M_KIND_EFFECT, O3M_MODULE_SIZE(11), "key11", sizeof(FxState), 2, kKeyParams,
                            fx_init, fx_render, fx_set_param, fx_note_on, fx_note_off, fx_shutdown, nullptr, 0};

TEST_CASE("insert page: a bound 1.12 effect's KEY row reads OFF, T1..T16 (T3 here) and is not a Learn target; a 1.11 effect's KEY flag is not honoured") {
  Rig rig;
  CHECK(rig.mods->set.add_native("key12", kKeyFx));
  CHECK(rig.mods->set.add_native("key11", kKeyOld));
  rig.inserts_page();
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "key12") == app::ModuleUi::InsertResult::Ok);
  rig.blocks(4);
  rig.cell(1, 0);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  const app::Model& m = rig.app->model();
  NEED(v.screen == app::ScreenId::Instrument);
  NEED(v.params.insert == 2u);  // T2 INS A: the host's where 2
  // the KEY row is field 1: describe gives the 17 names and clears Learn
  const core::ParamId key_id = core::param_id(core::ParamKind::Insert, 2, 1);
  app::ParamScratch scratch;
  NEED(app::describe_param(m, key_id, scratch) != nullptr);
  CHECK(scratch.desc.list_count == 17u);
  CHECK(scratch.desc.unit == core::ParamUnit::Enum);
  CHECK(std::strcmp(scratch.desc.names[0], "OFF") == 0);
  CHECK(std::strcmp(scratch.desc.names[3], "T3") == 0);
  CHECK(std::strcmp(scratch.desc.names[16], "T16") == 0);
  CHECK((scratch.desc.flags & core::kParamLearnable) == 0u);
  CHECK((scratch.desc.flags & core::kParamModulatable) == 0u);  // the KEY row carries O3M_PARAM_MOD on purpose: a modulator must not re-aim the key (the review fold)
  // set the value to 3 with the encoder (an INT 0..16 row: one detent a step) and the row reads T3
  rig.tap(Button::Down, 1);  // focus the KEY row
  CHECK(app::kParamsScreen.focus_param(m, v) == key_id);
  rig.turn(3);
  float value = 0.0f;
  CHECK(app::read_param(m, key_id, value));
  CHECK(value == 3.0f);
  NEED(app::describe_param(m, key_id, scratch) != nullptr);
  CHECK(std::strcmp(scratch.desc.names[static_cast<unsigned>(value)], "T3") == 0);
  // Learn on the KEY row is refused (learn_bind, param_actions.cpp): no binding, the note says so
  rig.learn_toggle();
  (void)rig.invoke("midi.cc", Rig::cc(1, 74, 64));
  CHECK(rig.last_note() == "NO TARGET TO LEARN");
  CHECK(rig.project().settings().learn_count() == 0u);
  rig.learn_toggle();
  // the 1.11 effect: bound on T3 INS A (where 6), its KEY-flagged row is a plain INT 0..16: no names, and it stays a Learn target
  rig.tap(Button::Master);  // back to the Mixer's INSERTS page
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 3, 0, "key11") == app::ModuleUi::InsertResult::Ok);
  rig.blocks(4);
  rig.cell(3, 0);
  rig.tap(Button::Params);
  NEED(rig.app->state().params.insert == 6u);
  const core::ParamId old_id = core::param_id(core::ParamKind::Insert, 6, 1);
  NEED(app::describe_param(m, old_id, scratch) != nullptr);
  CHECK(scratch.desc.names == nullptr);
  CHECK(scratch.desc.list_count != 17u);
  CHECK((scratch.desc.flags & core::kParamLearnable) != 0u);  // not a KEY: learnable as any row
}

// ---- OS-INSFX-ABI2 A9: the F5 PRESET list on an insert's parameter page (docs/specs/OS-INSFX-ABI2.md section 3.6) -----------------------------------------------------------------

TEST_CASE("insert preset list: F5 on the insert's page opens the module's list, Down chooses, F1 loads it into the blob and the host, the strip says its name, and no undo entry is made") {
  Rig rig;
  rig.inserts_page();
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "pfx") == app::ModuleUi::InsertResult::Ok);  // T2 INS A, where 2: two presets, 32 parameters
  rig.blocks(4);
  rig.cell(1, 0);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  const app::Model& m = rig.app->model();
  NEED(v.params.insert == 2u);
  app::ParamsKeyRow row{};
  app::params_key_row(m, v, row);
  CHECK(std::strcmp(row.label[4], "PRESET") == 0);  // the page has pages F1..F4 and F5 is the list
  CHECK(app::preset_list_count(m, v) == 2u);
  const model::Project& p = rig.project();
  const std::size_t depth[3] = {rig.editor->journal(model::UndoArea::Pattern).depth(), rig.editor->journal(model::UndoArea::Song).depth(), rig.editor->journal(model::UndoArea::Sound).depth()};
  rig.tap(Button::F5);
  CHECK(v.overlay == app::OverlayId::Preset);
  CHECK(v.screen == app::ScreenId::Instrument);  // over the page
  CHECK(v.modules.preset == 0u);
  {
    const test::RowRead keys = test::read_owner_row(m, v);
    CHECK(keys.label[0] == "LOAD");
    CHECK(keys.label[7] == "BACK");
    CHECK(keys.label[4].empty());  // the list owns the row: no PRESET key under itself
  }
  rig.tap(Button::Down);
  CHECK(v.modules.preset == 1u);
  rig.tap(Button::Down);
  CHECK(v.modules.preset == 1u);  // the last one: the list does not wrap
  rig.tap(Button::Up);
  rig.tap(Button::Down);
  rig.tap(Button::F1);  // "Two": 0.2 and 7.4 (an INT: 7), the other thirty take their defaults
  CHECK(v.overlay == app::OverlayId::None);
  CHECK(rig.last_note() == "PRESET TWO LOADED");
  const model::InsertRecord* rec = p.find_insert(1, 0);
  NEED(rec != nullptr);
  CHECK(rec->n == 32u);
  CHECK(rec->param[0] == 0.2f);
  CHECK(rec->param[1] == 7.0f);
  for (unsigned i = 2; i < 32; ++i) {
    CHECK(rec->param[i] == 0.5f);
  }
  rig.blocks(2);
  NEED(rig.fx_state(2) != nullptr);
  CHECK(rig.fx_state(2)->values[0] == 0.2f);  // the instance heard it
  CHECK(rig.fx_state(2)->values[1] == 7.0f);
  CHECK(rig.editor->journal(model::UndoArea::Pattern).depth() == depth[0]);  // not an undo step (D9)
  CHECK(rig.editor->journal(model::UndoArea::Song).depth() == depth[1]);
  CHECK(rig.editor->journal(model::UndoArea::Sound).depth() == depth[2]);
  // F8 closes the list and loads nothing
  rig.tap(Button::F5);
  CHECK(v.overlay == app::OverlayId::Preset);
  CHECK(v.modules.preset == 0u);  // the cursor starts on the first preset each time
  rig.tap(Button::F8);
  CHECK(v.overlay == app::OverlayId::None);
  CHECK(p.find_insert(1, 0)->param[0] == 0.2f);
}

TEST_CASE("insert preset list: a module with no presets opens the list with nothing to load (F1 is blank and does nothing), F8 closes it") {
  Rig rig;
  rig.inserts_page();
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 1, "biquad") == app::ModuleUi::InsertResult::Ok);  // T2 INS B, where 3: no presets
  rig.blocks(4);
  rig.cell(1, 1);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  const app::Model& m = rig.app->model();
  NEED(v.params.insert == 3u);
  CHECK(app::preset_list_count(m, v) == 0u);
  const float before = rig.project().find_insert(1, 1)->param[0];
  rig.tap(Button::F5);
  CHECK(v.overlay == app::OverlayId::Preset);
  {
    const test::RowRead keys = test::read_owner_row(m, v);
    CHECK(keys.label[0].empty());
    CHECK(keys.label[7] == "BACK");
  }
  rig.tap(Button::F1);
  CHECK(v.overlay == app::OverlayId::Preset);  // nothing to load: the list stays
  CHECK(rig.project().find_insert(1, 1)->param[0] == before);
  rig.tap(Button::Down);
  CHECK(v.modules.preset == 0u);
  rig.tap(Button::F8);
  CHECK(v.overlay == app::OverlayId::None);
}

TEST_CASE("insert preset list: the encoder moves the cursor and stops at the ends, and the list captures every key but its own") {
  Rig rig;
  rig.inserts_page();
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "pfx") == app::ModuleUi::InsertResult::Ok);
  rig.blocks(4);
  rig.cell(1, 0);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  rig.tap(Button::F5);
  NEED(v.overlay == app::OverlayId::Preset);
  rig.turn(5);  // the encoder: past the last one it stops there
  CHECK(v.modules.preset == 1u);
  rig.turn(-5);
  CHECK(v.modules.preset == 0u);
  CHECK(app::kPresetOverlay.count(rig.app->model(), v) == 2);
  CHECK(app::kPresetOverlay.position(v) == 0);
  CHECK(app::kPresetOverlay.page() == app::kPresetRows);
  // the Pattern button does not leave: the list captures every key but its own
  rig.tap(Button::Pattern);
  CHECK(v.overlay == app::OverlayId::Preset);
  CHECK(v.screen == app::ScreenId::Instrument);
  rig.tap(Button::F8);
  CHECK(v.overlay == app::OverlayId::None);

}

// ---- OS-MODPRESET P4: the card presets on an insert's page (docs/specs/OS-MODPRESET.md section 4) ------------------------------------------------------------

TEST_CASE("OS-MODPRESET P4: on an insert's page Shift + F5 opens the PRESET NAME keyboard with the effect's name and F8 saves a card preset; the F5 list then has the CARD section and F1 loads it into the blob (no undo step)") {
  Rig rig;
  rig.inserts_page();
  CHECK(rig.mods->ui.insert_assign(*rig.editor, 1, 0, "pfx") == app::ModuleUi::InsertResult::Ok);  // T2 INS A, where 2
  rig.blocks(4);
  rig.cell(1, 0);
  rig.tap(Button::Params);
  const app::ViewState& v = rig.app->state();
  const app::Model& m = rig.app->model();
  NEED(v.params.insert == 2u);
  // A card preset of the effect: modules/presets/pfx/Wet.o3mp, naming P00 and P01 (an INT: 8.6 rounds to 9) and an unknown name.
  formats::preset::Preset card{};
  std::strcpy(card.module, "pfx");
  card.kind = O3M_KIND_EFFECT;
  card.count = 3;
  std::strcpy(card.entries[0].name, "P00");
  card.entries[0].value = 0.7f;
  std::strcpy(card.entries[1].name, "P01");
  card.entries[1].value = 8.6f;
  std::strcpy(card.entries[2].name, "Gone");
  card.entries[2].value = 1.0f;
  std::uint8_t buf[formats::preset::kMaxFileBytes];
  const std::size_t n = formats::preset::write(card, buf, sizeof buf);
  NEED(n != 0u);
  std::filesystem::create_directories(rig.card.root / "modules/presets/pfx");
  {
    std::ofstream f(rig.card.root / "modules/presets/pfx/Wet.o3mp", std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf), static_cast<std::streamsize>(n));
    CHECK(f.good());
  }
  const model::Project& p = rig.project();
  const std::size_t depth = rig.editor->journal(model::UndoArea::Sound).depth();
  // Shift + F5 on the insert's page: the keyboard, the effect's display name suggested; F8 saves the values in force.
  test::MemCard mem;
  app::SaveService saves;
  saves.attach(mem, mem);
  rig.app->attach_saves(&saves);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F5);
  rig.release(Button::Shift);
  rig.pass(1);
  NEED(v.overlay == app::OverlayId::TextEntry);
  CHECK(v.text.target == app::TextEntryView::Target::Preset);
  CHECK(std::string(v.text.entry.text()) == "pfx");  // the effect's display name
  rig.tap(Button::F8);
  CHECK(v.overlay == app::OverlayId::None);
  CHECK(rig.app->state().notify.head().text == "PRESET SAVED: PFX");
  for (unsigned i = 0; i < 400u && !mem.has("modules/presets/pfx/pfx.o3mp"); ++i) {
    rig.pass(20);
  }
  CHECK(mem.has("modules/presets/pfx/pfx.o3mp"));
  // The F5 list: the factory presets then the CARD section with the card's two files (Wet on this rig's card, pfx saved above is on the save card).
  CHECK(app::preset_list_count(m, v) == 2u);
  rig.tap(Button::F5);
  NEED(v.overlay == app::OverlayId::Preset);
  CHECK(app::preset_list_count(m, v) == 3u);  // Wide, Two, then CARD: Wet
  {
    const test::RowRead keys = test::read_owner_row(m, v);
    CHECK(keys.label[0] == "LOAD");
    CHECK(keys.label[7] == "BACK");
    CHECK(test::read_owner_row(m, v, true).label[4] == "SAVE");
  }
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // the CARD entry (index 2)
  rig.tap(Button::F1);
  CHECK(v.overlay == app::OverlayId::None);
  CHECK(rig.last_note() == "PRESET WET LOADED, 1 SKIPPED");
  const model::InsertRecord* rec = p.find_insert(1, 0);
  NEED(rec != nullptr);
  CHECK(rec->param[0] == 0.7f);
  CHECK(rec->param[1] == 9.0f);  // rounded for the INT
  for (unsigned i = 2; i < 32; ++i) {
    CHECK(rec->param[i] == 0.5f);  // the defaults, not what the page held
  }
  rig.blocks(2);
  NEED(rig.fx_state(2) != nullptr);
  CHECK(rig.fx_state(2)->values[0] == 0.7f);  // the instance heard it
  CHECK(rig.editor->journal(model::UndoArea::Sound).depth() == depth);  // an insert's load is not an undo step (D9)
}
