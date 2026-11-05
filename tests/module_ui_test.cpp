// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012b: the SD modules' user interface (app/module_ui.h): the standard controls made from a module's descriptors (edit, undo, Learn), a module instrument's
// parameters in the project, the views drawn inside their rectangle and blanked over budget, the engine picker, the MODULES screen and an app's page (input,
// the Shift layer, the exit chord, a stopped app). Native test modules; the card is a temporary directory. Host only.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/screens/modules_screen.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/state_line.h>
#include <o3os/audio/module_voices.h>
#include <o3os/core/input_event.h>
#include <o3os/core/param.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/dir_storage.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/components.h>
#include <o3os/ui/renderer.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "support/need.h"

// The built-in modules' generated tables (tools/o3m_gen.py): the descriptors the loader takes, as the params page would describe them.
#include "../modules/chip/chip_gen.h"
#include "../modules/fm/fm_gen.h"
#include "../modules/tb303/tb303_gen.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::DispatchResult;
using core::ActionArg;
using core::ParamId;
using core::ParamKind;
namespace ip = model::instrument_param;
namespace fs = std::filesystem;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

// ---- native test modules ---------------------------------------------------------------------------------------------------------------------------
struct VState {
  std::uint32_t set_calls;
  std::uint32_t last_id;
  float last_value;
  float values[8];
  std::uint32_t enters, exits, inputs, shutdowns;
  o3m_ui_event last_event;
  std::uint32_t kind_seen[4];
};
std::uint32_t g_view_calls = 0;
std::uint32_t g_slow_input_us = 0;
std::uint32_t g_clock_us = 0;

int v_init(void* s, const o3m_host* host) {
  VState* v = static_cast<VState*>(s);
  v->set_calls = 0;
  return host->draw != nullptr ? 0 : 1;  // the draw API is lent to every module (ABI 1.1)
}
std::uint32_t v_render(void*, const float*, float*, std::uint32_t) { return 0; }
void v_set_param(void* s, std::uint32_t id, float value) {
  VState* v = static_cast<VState*>(s);
  ++v->set_calls;
  v->last_id = id;
  v->last_value = value;
  if (id < 8) {
    v->values[id] = value;
  }
}
void v_note_on(void*, std::uint32_t, std::uint32_t, std::uint32_t) {}
void v_note_off(void*, std::uint32_t) {}
void v_shutdown(void* s) { ++static_cast<VState*>(s)->shutdowns; }

// view 0: a block that is as wide as parameter A says; it also paints far outside its canvas (it must not show). view 1: a text and a polyline.
const o3m_draw_api* g_api = nullptr;
void viewer_draw(void*, std::uint32_t view, o3m_canvas* c) {
  ++g_view_calls;
  if (view == 0u) {
    const std::int32_t w = static_cast<std::int32_t>(c->params[0] * static_cast<float>(c->width));
    g_api->rect(c, 0, 0, w, c->height, O3M_COLOR_ACCENT);
    g_api->rect(c, -500, -500, 3000, 20, O3M_COLOR_FX);             // above the canvas and through it: only the part inside shows
    g_api->rect(c, c->width - 4, -100, 2000, 5000, O3M_COLOR_VOL);  // from the right edge on, and far past the bottom
  } else {
    g_api->text(c, 2, 2, "VIEW", O3M_FACE_DATA, O3M_COLOR_TEXT);
    const std::int16_t xy[6] = {0, 20, 40, 5, 80, 20};
    g_api->polyline(c, xy, 3, O3M_COLOR_INSTR);
  }
}
// a view that spends more than it may
std::uint32_t g_hog_calls = 0;
void hog_draw(void*, std::uint32_t, o3m_canvas* c) {
  ++g_hog_calls;
  for (unsigned i = 0; i < 200; ++i) {
    g_api->rect(c, 0, 0, 1, 1, O3M_COLOR_TEXT);
  }
}

void app_enter(void* s) { ++static_cast<VState*>(s)->enters; }
void app_exit(void* s) { ++static_cast<VState*>(s)->exits; }
void app_input(void* s, const o3m_ui_event* e) {
  VState* v = static_cast<VState*>(s);
  ++v->inputs;
  v->last_event = *e;
  if (e->kind < 4u) {
    ++v->kind_seen[e->kind];
  }
  g_clock_us += g_slow_input_us;
}
void app_draw(void* s, std::uint32_t, o3m_canvas* c) {
  const VState* v = static_cast<const VState*>(s);
  g_api->rect(c, 0, 0, c->width, c->height, O3M_COLOR_PANE);
  g_api->rect(c, 0, 0, 10 + static_cast<std::int32_t>(v->inputs), 10, O3M_COLOR_PLAY);
}

const o3m_param kViewerParams[3] = {
    {"Amount", 0.0f, 1.0f, 0.5f, 0},
    {"Whole Steps", 0.0f, 10.0f, 5.0f, O3M_PARAM_INT | O3M_PARAM_WITH_UNIT(O3M_UNIT_SEMITONES)},
    {"Switch", 0.0f, 1.0f, 1.0f, O3M_PARAM_BOOL | O3M_PARAM_NO_LEARN},
};
const o3m_ui kViewerUi = {sizeof(o3m_ui), nullptr, nullptr, viewer_draw, nullptr, nullptr};
const o3m_module kViewer = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "Viewer", sizeof(VState), 3, kViewerParams,
                            v_init, v_render, v_set_param, v_note_on, v_note_off, v_shutdown, &kViewerUi, 2};
const o3m_ui kHogUi = {sizeof(o3m_ui), nullptr, nullptr, hog_draw, nullptr, nullptr};
const o3m_module kHog = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "Hog", sizeof(VState), 3, kViewerParams,
                         v_init, v_render, v_set_param, v_note_on, v_note_off, v_shutdown, &kHogUi, 1};
const o3m_module kPlain = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "Plain", sizeof(VState), 3, kViewerParams,
                           v_init, v_render, v_set_param, v_note_on, v_note_off, v_shutdown, nullptr, 0};
const o3m_param kAppParams[2] = {{"Speed", 0.0f, 100.0f, 10.0f, O3M_PARAM_INT | O3M_PARAM_WITH_UNIT(O3M_UNIT_PERCENT)}, {"Tone", 1.0f, 100.0f, 10.0f, O3M_PARAM_LOG}};
const o3m_ui kAppUi = {sizeof(o3m_ui), app_enter, app_input, app_draw, app_exit, nullptr};
// "Tool" is an app of ABI 1.9 that declares two rows: the OS parameter column is its page (it was built before ABI 1.10, whose apps draw their own rows: OS-APPUX S3).
const o3m_module kTool = {O3M_ABI_MAJOR, 9, O3M_KIND_APP, sizeof(o3m_module), "Tool", sizeof(VState), 2, kAppParams,
                          v_init, nullptr, v_set_param, nullptr, nullptr, v_shutdown, &kAppUi, 1};
const o3m_module kTool10 = {O3M_ABI_MAJOR, 10, O3M_KIND_APP, sizeof(o3m_module), "Tool10", sizeof(VState), 2, kAppParams,
                            v_init, nullptr, v_set_param, nullptr, nullptr, v_shutdown, &kAppUi, 1};

std::uint32_t test_clock(void*) { return g_clock_us; }

// ---- the rig: an App over the demo project with the module system, a card and the editor -------------------------------------------------------------
struct Card {
  fs::path root;
  Card() {
    root = fs::temp_directory_path() / ("o3m_ui_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    fs::remove_all(root);
    fs::create_directories(root / "modules" / "instruments");
    fs::create_directories(root / "modules" / "apps");
    touch("modules/instruments/Viewer.tmod");
    touch("modules/instruments/plain.tmod");
    touch("modules/instruments/Hog.tmod");
    touch("modules/instruments/notes.txt");
    touch("modules/apps/Tool.tmod");
    touch("modules/apps/zzz_not_loaded.tmod");
  }
  ~Card() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  void touch(const std::string& rel) {
    std::ofstream f(root / rel, std::ios::binary);
    f << "x";
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

  Rig() {
    g_api = voices().host().draw;
    g_clock_us = 0;
    g_slow_input_us = 0;
    NEED(storage.mount() == hal::storage::Status::Ok);
    (void)voices().attach_scratch(mods->scratch, sizeof mods->scratch);
    (void)mods->set.cold().init(mods->sdram, 0x70000000u, sizeof mods->sdram);
    mods->ui.attach(mods->set, mods->loader, mods->voices);
    mods->ui.set_storage(&storage);
    module::ViewHost::Hooks hooks;
    hooks.now_us = test_clock;
    mods->ui.views().set_hooks(hooks);
    CHECK(mods->set.add_native("Viewer", kViewer));
    CHECK(mods->set.add_native("Hog", kHog));
    CHECK(mods->set.add_native("Tool", kTool));
    model::Project& p = project();
    sim::fill_demo_project(p);
    // instrument 41 (slot 40): the Viewer, with its blob; instrument 42 (slot 41): the Hog
    CHECK(p.add_module("Viewer", 6, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
    CHECK(p.add_module("Hog", 3, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
    CHECK(p.add_module("Gone", 4, 1, 0) == model::Status::Ok);  // named by the project, not on the card
    install(p, 40, 0, true);
    install(p, 41, 1, true);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_param_ring(&ring);
    app->attach_modules(&mods->ui);
    app->run_once();
  }

  audio::ModuleVoices& voices() { return mods->voices; }
  // A Module instrument in `slot` naming table entry `ref`; with `bind`, an instance of its module bound to it.
  void install(model::Project& p, unsigned slot, unsigned ref, bool bind) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Module);
    in.module_index = static_cast<std::uint8_t>(ref);
    std::strcpy(in.name, ref == 0 ? "VIEWER" : "HOG");
    const float blob[3] = {0.25f, 3.0f, 1.0f};
    std::memcpy(in.params, blob, sizeof blob);
    in.param_len = sizeof blob;
    CHECK(p.set_instrument(slot, in) == model::Status::Ok);
    if (bind) {
      const int idx = mods->set.find(ref == 0 ? "Viewer" : "Hog");
      NEED(idx >= 0);
      const o3m_module& mod = *mods->set.at(static_cast<unsigned>(idx))->entry;
      std::uint8_t* mem = nullptr;
      std::uint32_t addr = 0;
      NEED(mods->set.cold().take(mod.state_bytes, 16, mem, addr));
      std::memset(mem, 0, mod.state_bytes);
      CHECK(mods->voices.bind(slot, mod, module::fault_id(ref == 0 ? "Viewer" : "Hog"), mem, p.instrument(slot)->params, p.instrument(slot)->param_len) ==
            audio::ModuleVoices::Bind::Ok);
    }
  }
  VState* state_of(unsigned slot) { return static_cast<VState*>(mods->voices.state_of(slot)); }
  // The render's block start: the module hears what the main loop sent.
  void block() {
    mods->voices.begin_block();
    mods->voices.end_block();
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
  void pad(unsigned index, int velocity = 100) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), static_cast<std::int16_t>(velocity)));
    pass(1);
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass(1);
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  std::string last_note() const {
    const core::NotifyQueue& q = app->state().notify;
    return q.empty() ? std::string() : std::string(q.at(q.size() - 1).text.c_str());
  }
  void goto_instrument(unsigned slot) {
    tap(Button::Params);
    (void)invoke("instrument.select", static_cast<ActionArg>(slot + 1u));  // (a pad plays the instrument now; Shift + pad would select it: OS-INSTUI O3)
  }
  float value(ParamId id) { return project().get_param(id); }
};

ParamId mp(unsigned slot, unsigned index) { return core::param_id(ParamKind::Instrument, slot, ip::kModuleFirst + index); }

// One frame of `fn(renderer)` on a raster.
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

// ---- the descriptors --------------------------------------------------------------------------------------------------------------------------------


TEST_CASE("module ui: every parameter of the built-in modules is described, labelled ones included (FM Algorithm 1..8 has eight names)") {
  app::ParamScratch s;
  const auto walk = [&s](const o3m_param* params, const o3m_labels* labels, unsigned n) {
    unsigned named = 0;
    for (unsigned i = 0; i < n; ++i) {
      CAPTURE(params[i].name);
      CHECK(app::describe_module_param(params[i], s, labels != nullptr ? &labels[i] : nullptr));
      CHECK(core::param_desc_valid(s.desc));
      if (labels != nullptr && labels[i].names != nullptr) {
        ++named;
        CHECK(s.desc.unit == core::ParamUnit::Enum);  // the labels were taken, not ignored
        char text[core::kParamTextChars + 1];
        (void)core::param_format(s.desc, params[i].min, text, sizeof text);
        CHECK(std::strcmp(text, labels[i].names[0]) == 0);
        (void)core::param_format(s.desc, params[i].max, text, sizeof text);
        CHECK(std::strcmp(text, labels[i].names[labels[i].count - 1u]) == 0);
      }
    }
    return named;
  };
  CHECK(walk(tb303_params, nullptr, TB303_PARAM_COUNT) == 0u);
  CHECK(walk(chip_params, chip_labels, CHIP_PARAM_COUNT) == 2u);  // Wave, Duty
  CHECK(walk(fm_params, fm_labels, FM_PARAM_COUNT) == 1u);        // Algorithm: 1..8
  CHECK(fm_params[FM_PARAM_ALGORITHM].min == 1.0f);
}
TEST_CASE("module ui: a module's descriptor becomes a standard control") {
  app::ParamScratch s;
  // a linear float: shown as the position in its range, a hundred fine steps
  CHECK(app::describe_module_param(kViewerParams[0], s));
  CHECK(std::strcmp(s.desc.name, "AMOUNT") == 0);
  CHECK((s.desc.unit == core::ParamUnit::NormPct && s.desc.curve == core::ParamCurve::Linear && s.desc.detents == 100));
  CHECK((s.desc.flags & core::kParamLearnable) != 0);
  CHECK(!s.whole);
  // OS-SMALL2: that position is a percentage, and says so ("RANGE 0 .. 100 %", not a bare 0 .. 100 that reads as the declared 0..1 gone wrong): every float with no unit,
  // the TB-303's Waveform included, which is declared 0..1
  CHECK(std::strcmp(s.desc.unit_text, "%") == 0);
  char lo[core::kParamTextChars + 1], hi[core::kParamTextChars + 1];
  (void)core::param_format(s.desc, s.desc.min, lo, sizeof lo);
  (void)core::param_format(s.desc, s.desc.max, hi, sizeof hi);
  CHECK((std::strcmp(lo, "0") == 0 && std::strcmp(hi, "100") == 0));
  // whole numbers with a unit: Int, a step of one, "ST"
  CHECK(app::describe_module_param(kViewerParams[1], s));
  CHECK((s.desc.unit == core::ParamUnit::Int && s.desc.detents == 10 && std::strcmp(s.desc.unit_text, "ST") == 0 && s.whole));
  CHECK(std::strcmp(s.desc.name, "WHOLE STEPS") == 0);
  // a switch: OFF / ON, and the module said it is not for Learn
  CHECK(app::describe_module_param(kViewerParams[2], s));
  CHECK((s.desc.unit == core::ParamUnit::Enum && s.desc.list_count == 2 && std::strcmp(s.desc.names[1], "ON") == 0));
  CHECK((s.desc.flags & core::kParamLearnable) == 0);
  char text[core::kParamTextChars + 1];
  (void)core::param_format(s.desc, 1.0f, text, sizeof text);
  CHECK(std::strcmp(text, "ON") == 0);
  // a log parameter with a unit and a modulation target; the name is cut to the page's width
  const o3m_param log = {"A Very Long Parameter Name", 20.0f, 20000.0f, 1000.0f, O3M_PARAM_LOG | O3M_PARAM_MOD | O3M_PARAM_WITH_UNIT(O3M_UNIT_HZ)};
  CHECK(app::describe_module_param(log, s));
  CHECK((s.desc.curve == core::ParamCurve::Log && (s.desc.flags & core::kParamModulatable) != 0 && std::strcmp(s.desc.unit_text, "HZ") == 0));
  CHECK(std::strlen(s.desc.name) == core::kParamNameChars);
  CHECK(std::strcmp(s.desc.name, "A VERY LONG ") == 0);
  // a log flag on a range that includes zero is a linear control (a log curve needs min > 0); a unit the OS does not know is no unit
  const o3m_param odd = {"Odd", 0.0f, 10.0f, 1.0f, O3M_PARAM_LOG | (200u << O3M_PARAM_UNIT_SHIFT)};
  CHECK(app::describe_module_param(odd, s));
  CHECK((s.desc.curve == core::ParamCurve::Linear && s.desc.unit_text[0] == '\0'));
  // what no control can show
  const o3m_param empty = {"Empty", 1.0f, 1.0f, 1.0f, 0};
  CHECK(!app::describe_module_param(empty, s));
  const o3m_param nameless = {"", 0.0f, 1.0f, 0.0f, 0};
  CHECK(!app::describe_module_param(nameless, s));
  // every parameter of every test module is a valid descriptor
  for (const o3m_param& p : kAppParams) {
    CHECK(app::describe_module_param(p, s));
    CHECK(core::param_desc_valid(s.desc));
  }
}

TEST_CASE("module ui: a module instrument's parameters are floats of the blob the project already stores") {
  Rig rig;
  model::Project& p = rig.project();
  CHECK(app::is_module_param(mp(40, 0)));
  CHECK(!app::is_module_param(core::param_id(ParamKind::Instrument, 40, ip::Volume)));
  CHECK(!app::is_module_param(core::param_id(ParamKind::Track, 3, ip::kModuleFirst)));
  CHECK(!app::is_module_param(core::param_id(ParamKind::Instrument, 70, ip::kModuleFirst)));
  CHECK(p.has_param(mp(40, 0)));
  CHECK(p.has_param(mp(40, 2)));
  CHECK(!p.has_param(mp(40, 3)));    // the blob holds three
  CHECK(!p.has_param(mp(3, 0)));     // a sample instrument has none of these
  CHECK(p.get_param(mp(40, 0)) == 0.25f);
  CHECK(p.get_param(mp(40, 1)) == 3.0f);
  const std::uint32_t before = p.param_generation(mp(40, 0));
  CHECK(p.set_param(mp(40, 0), 0.75f) == model::Status::Ok);
  CHECK(p.get_param(mp(40, 0)) == 0.75f);
  CHECK(p.param_generation(mp(40, 0)) == before + 1u);
  CHECK(p.param_generation(mp(40, 1)) == p.param_generation(mp(40, 1)));
  CHECK(p.set_param(mp(40, 0), 0.75f) == model::Status::Ok);  // no change: no generation
  CHECK(p.param_generation(mp(40, 0)) == before + 1u);
  CHECK(p.set_param(mp(40, 0), std::nanf("")) == model::Status::BadValue);
  CHECK(p.set_param(mp(40, 0), INFINITY) == model::Status::BadValue);
  CHECK(p.set_param(mp(40, 5), 1.0f) == model::Status::BadIndex);       // beyond the blob
  CHECK(p.set_param(mp(3, 0), 1.0f) == model::Status::NotFound);        // not a module instrument
  CHECK(p.get_param(mp(40, 0)) == 0.75f);
  // the sampler's own parameters of the slot are not reachable through a module id, and the blob's bytes are the project's (nothing else changed)
  CHECK(p.instrument(40)->param_len == 12);
  CHECK(model::validate_instrument(*p.instrument(40), p.module_count()) == nullptr);
}

// ---- the page and its controls ------------------------------------------------------------------------------------------------------------------------

TEST_CASE("module ui: the Instrument page of a module slot has the module's controls: edit, undo, the module hears it") {
  Rig rig;
  rig.goto_instrument(40);
  const app::Model& m = rig.app->model();
  const app::ViewState& v = rig.app->state();
  CHECK(v.screen == app::ScreenId::Instrument);
  CHECK(v.instrument == 41);
  CHECK(app::kParamsScreen.count(m, v) == 3);
  CHECK(app::kParamsScreen.focus_param(m, v) == mp(40, 0));
  VState* st = rig.state_of(40);
  NEED(st != nullptr);
  const std::uint32_t base_calls = st->set_calls;  // bind() applied the three values of the blob
  CHECK(base_calls == 3u);
  // the encoder: a fine step of AMOUNT is 1/100 of its range; the project holds it, the module hears it at its next block start, the engine's table is not told
  rig.turn(10);
  CHECK(std::fabs(rig.value(mp(40, 0)) - 0.35f) < 1e-5f);
  core::ParamChange c;
  CHECK(!rig.ring.pop(c));
  CHECK(st->set_calls == base_calls);  // not yet: the render applies it at a block start
  rig.block();
  CHECK(st->set_calls == base_calls + 1u);
  CHECK((st->last_id == 0u && std::fabs(st->last_value - 0.35f) < 1e-5f));
  // Shift: the coarse step (ten fine ones); Delete: the default; the end of the range holds
  rig.press(Button::Shift);
  rig.pass(1);
  rig.turn(1);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(std::fabs(rig.value(mp(40, 0)) - 0.45f) < 1e-5f);
  rig.tap(Button::Delete);
  CHECK(rig.value(mp(40, 0)) == 0.5f);
  rig.turn(1000);
  CHECK(rig.value(mp(40, 0)) == 1.0f);
  rig.pass(1600);  // (OS-UNDOGRP: clicks within 1500 ms on one row are one undo unit)
  rig.turn(-1000);
  CHECK(rig.value(mp(40, 0)) == 0.0f);
  // undo: the value comes back, and the module hears that too
  rig.tap(Button::F8);
  CHECK(rig.value(mp(40, 0)) == 1.0f);
  rig.block();
  CHECK(st->last_value == 1.0f);
  rig.tap(Button::F8);
  CHECK(rig.value(mp(40, 0)) == 0.5f);
  // the second row: whole numbers with a step of one
  rig.tap(Button::Down);
  CHECK(app::kParamsScreen.focus_param(m, v) == mp(40, 1));
  rig.turn(2);
  CHECK(rig.value(mp(40, 1)) == 5.0f);
  rig.turn(100);
  CHECK(rig.value(mp(40, 1)) == 10.0f);
  // the third: a switch
  rig.tap(Button::Down);
  rig.turn(-1);
  CHECK(rig.value(mp(40, 2)) == 0.0f);
  rig.turn(1);
  CHECK(rig.value(mp(40, 2)) == 1.0f);
  // a row the module did not mark for Learn says so
  rig.invoke("learn.toggle");
  CHECK(v.overlay == app::OverlayId::Learn);
  rig.invoke("midi.cc", ActionArg((0u << 16) | (20u << 8) | 5u));
  CHECK(rig.last_note() == "NO TARGET TO LEARN");
  rig.invoke("learn.toggle");
  CHECK(v.overlay == app::OverlayId::None);
  CHECK(rig.app->failed_frames() == 0u);
}

// ---- OS-MODPAGEKEYS: F1..F4 are the pages the module has, the rest of the row is blank -----------------------------------------------------------------

// Modules with a page table of 1..4 pages (eight parameters a page, 32 parameters in all) and legacy ones (no table: eight to a page).
const o3m_param kPagedParams[32] = {
    {"P00", 0.0f, 1.0f, 0.5f, 0}, {"P01", 0.0f, 1.0f, 0.5f, 0}, {"P02", 0.0f, 1.0f, 0.5f, 0}, {"P03", 0.0f, 1.0f, 0.5f, 0}, {"P04", 0.0f, 1.0f, 0.5f, 0}, {"P05", 0.0f, 1.0f, 0.5f, 0},
    {"P06", 0.0f, 1.0f, 0.5f, 0}, {"P07", 0.0f, 1.0f, 0.5f, 0}, {"P08", 0.0f, 1.0f, 0.5f, 0}, {"P09", 0.0f, 1.0f, 0.5f, 0}, {"P10", 0.0f, 1.0f, 0.5f, 0}, {"P11", 0.0f, 1.0f, 0.5f, 0},
    {"P12", 0.0f, 1.0f, 0.5f, 0}, {"P13", 0.0f, 1.0f, 0.5f, 0}, {"P14", 0.0f, 1.0f, 0.5f, 0}, {"P15", 0.0f, 1.0f, 0.5f, 0}, {"P16", 0.0f, 1.0f, 0.5f, 0}, {"P17", 0.0f, 1.0f, 0.5f, 0},
    {"P18", 0.0f, 1.0f, 0.5f, 0}, {"P19", 0.0f, 1.0f, 0.5f, 0}, {"P20", 0.0f, 1.0f, 0.5f, 0}, {"P21", 0.0f, 1.0f, 0.5f, 0}, {"P22", 0.0f, 1.0f, 0.5f, 0}, {"P23", 0.0f, 1.0f, 0.5f, 0},
    {"P24", 0.0f, 1.0f, 0.5f, 0}, {"P25", 0.0f, 1.0f, 0.5f, 0}, {"P26", 0.0f, 1.0f, 0.5f, 0}, {"P27", 0.0f, 1.0f, 0.5f, 0}, {"P28", 0.0f, 1.0f, 0.5f, 0}, {"P29", 0.0f, 1.0f, 0.5f, 0},
    {"P30", 0.0f, 1.0f, 0.5f, 0}, {"P31", 0.0f, 1.0f, 0.5f, 0},
};
const o3m_page kPageTable[4] = {{"ALPHA", 0}, {"BETA", 8}, {"GAMMA", 16}, {"DELTA", 24}};
o3m_module paged_module(const char* name, unsigned params, const o3m_page* pages, unsigned page_count) {
  return {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), name, sizeof(VState), params, kPagedParams, v_init, v_render, v_set_param, v_note_on, v_note_off, v_shutdown, nullptr, 0,
          nullptr, pages, page_count, nullptr};
}

TEST_CASE("module ui: the key row shows a key for each page the module has and blanks the rest, not dims them (page counts 1..4, a module without a page table)") {
  struct Case {
    const char* name;
    unsigned params;
    unsigned pages;       // the page table's length, 0: no table
    unsigned keys_shown;  // the keys with a label
  };
  const Case cases[] = {{"Pg1", 8, 1, 1}, {"Pg2", 16, 2, 2}, {"Pg3", 24, 3, 3}, {"Pg4", 32, 4, 4}, {"Leg1", 5, 0, 1}, {"Leg2", 9, 0, 2}, {"Leg3", 20, 0, 3}, {"Leg4", 32, 0, 4}};
  static const char* const kNamed[4] = {"ALPHA", "BETA", "GAMMA", "DELTA"};
  static const char* const kNumbered[4] = {"1-8", "9-16", "17-24", "25-32"};
  constexpr unsigned slot = 30;  // (a slot a pad can select: the first 48; the demo project's instrument there is replaced)
  for (const Case& c : cases) {
    CAPTURE(c.name);
    Rig rig;  // (the engine holds a few instances: one rig for each module)
    const o3m_module mod = paged_module(c.name, c.params, c.pages != 0u ? kPageTable : nullptr, c.pages);
    CHECK(rig.mods->set.add_native(c.name, mod));
    model::Project& p = rig.project();
    CHECK(p.add_module(c.name, std::strlen(c.name), O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
    const unsigned ref = 3u;  // (after Viewer, Hog and Gone)
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Module);
    in.module_index = static_cast<std::uint8_t>(ref);
    std::strcpy(in.name, c.name);
    CHECK(p.set_instrument(slot, in) == model::Status::Ok);
    const int idx = rig.mods->set.find(c.name);
    NEED(idx >= 0);
    std::uint8_t* mem = nullptr;
    std::uint32_t addr = 0;
    NEED(rig.mods->set.cold().take(sizeof(VState), 16, mem, addr));
    std::memset(mem, 0, sizeof(VState));
    CHECK(rig.mods->voices.bind(slot, mod, module::fault_id(c.name), mem, p.instrument(slot)->params, p.instrument(slot)->param_len) == audio::ModuleVoices::Bind::Ok);
    rig.goto_instrument(slot);
    app::ParamsKeyRow row{};
    app::params_key_row(rig.app->model(), rig.app->state(), row);
    for (unsigned k = 0; k < 5; ++k) {
      CAPTURE(k);
      if (k < c.keys_shown) {
        CHECK(std::string(row.label[k]) == (c.pages != 0u ? kNamed[k] : kNumbered[k]));
        CHECK(((row.dim >> k) & 1u) == 0u);  // a page: a lit key
      } else {
        CHECK(std::string(row.label[k]).empty());  // the keys past the pages, and F5 (not bound for a module), are blank
        CHECK(((row.dim >> k) & 1u) == 0u);        // blank, not dimmed (no greyed "17-24")
      }
    }
    CHECK(rig.app->failed_frames() == 0u);
  }
}

TEST_CASE("module ui: a module with no parameters has no page key at all, F1 does nothing, and the info line says why (OS-MODPAGEKEYS follow-up)") {
  Rig rig;
  const o3m_module mod = paged_module("ZP", 0u, nullptr, 0u);  // bound, but with no parameter
  CHECK(rig.mods->set.add_native("ZP", mod));
  model::Project& p = rig.project();
  CHECK(p.add_module("ZP", 2, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  constexpr unsigned slot = 30;  // (a slot a pad can select: the first 48; the demo project's instrument there is replaced)
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Module);
  in.module_index = 3u;  // (after Viewer, Hog and Gone)
  std::strcpy(in.name, "ZP");
  CHECK(p.set_instrument(slot, in) == model::Status::Ok);
  const int idx = rig.mods->set.find("ZP");
  NEED(idx >= 0);
  std::uint8_t* mem = nullptr;
  std::uint32_t addr = 0;
  NEED(rig.mods->set.cold().take(sizeof(VState), 16, mem, addr));
  std::memset(mem, 0, sizeof(VState));
  CHECK(rig.mods->voices.bind(slot, mod, module::fault_id("ZP"), mem, p.instrument(slot)->params, p.instrument(slot)->param_len) == audio::ModuleVoices::Bind::Ok);
  rig.goto_instrument(slot);
  // the key row: no page key at all, blank like any page the module does not have (not dimmed)
  app::ParamsKeyRow row{};
  app::params_key_row(rig.app->model(), rig.app->state(), row);
  for (unsigned k = 0; k < 5; ++k) {
    CAPTURE(k);
    CHECK(std::string(row.label[k]).empty());
    CHECK(((row.dim >> k) & 1u) == 0u);
  }
  // F1 does nothing (no page to go to). The view starts on a page the module does not have, so the check also fails on the old code, where F1 brought the view back to page 0.
  app::ViewState& v = const_cast<app::ViewState&>(rig.app->state());  // (a test steers a state that only a key press would)
  const unsigned page_index = app::screen_index(app::ScreenId::Instrument);
  v.page[page_index] = 2u;
  CHECK(rig.invoke("instrument.page", ActionArg(0)) == app::DispatchResult::Unchanged);  // the dispatch reports no change
  CHECK(v.page[page_index] == 2u);                                                       // the page is unchanged
  // the info line says why the parameter area is empty: its band holds exactly the pixels of the line drawn with that text
  rig.pass(2500);  // let the last notice run out (it shares the info line)
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  const std::vector<std::uint8_t> frame = picture(theme, [&](ui::Renderer& r) { app::render_scene(rig.app->model(), rig.app->state(), r); });
  const char* kLeft = "MODULE ZP NO PARAMETERS: nothing to edit";
  const char* kRight = "delete: default   FX1 + FX2: learn";
  const std::vector<std::uint8_t> ref = picture(theme, [&](ui::Renderer& r) {
    const app::Frame f = app::make_frame(r.theme(), app::kFrameRows);
    app::draw_chrome_bands(r, f, ui::Rect{0, 0, ui::kScreenW, f.strip.h + f.header.h});  // the bottom band's fill, as the screen draws it
    const int y = ui::caps_y(r.theme(), ui::Face::Label, f.info.y, f.info.h);
    const int pad = r.theme().metrics.pad;
    r.text(2 * pad, y, kLeft, ui::Color::ChromeText, ui::Face::Label);
    r.text(ui::kScreenW - 2 * pad - r.text_width(kRight, ui::Face::Label), y, kRight, ui::Color::ChromeText, ui::Face::Label);
  });
  const app::Frame f = app::make_frame(theme, app::kFrameRows);
  bool same = true;
  for (int y = f.info.y; y < f.info.y + f.info.h && same; ++y) {
    for (int x = 0; x < ui::kScreenW; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * ui::kScreenW + static_cast<std::size_t>(x)) * 4;
      if (frame[i] != ref[i] || frame[i + 1] != ref[i + 1] || frame[i + 2] != ref[i + 2] || frame[i + 3] != ref[i + 3]) {
        same = false;
        break;
      }
    }
  }
  CHECK(same);
  CHECK(rig.app->failed_frames() == 0u);
}

TEST_CASE("module ui: a short blob is extended with the module's defaults by the first edit, as one undo unit") {
  Rig rig;
  model::Project& p = rig.project();
  // A project saved with a blob that holds only the first value: the controls show the defaults for the rest, and the first edit of one writes the whole blob.
  model::Instrument in = *p.instrument(40);
  in.param_len = 4;
  std::memset(in.params + 4, 0, sizeof in.params - 4);
  rig.editor->history_reset();
  CHECK(p.set_instrument(40, in) == model::Status::Ok);
  rig.editor->history_reset();
  CHECK(!p.has_param(mp(40, 1)));
  rig.goto_instrument(40);
  rig.tap(Button::Down);
  float shown = 0.0f;
  app::ParamScratch s;
  CHECK(rig.mods->ui.describe(p, 40, 1, s));
  rig.turn(1);
  CHECK(p.has_param(mp(40, 1)));
  CHECK(p.instrument(40)->param_len == 8);
  CHECK(p.get_param(mp(40, 1)) == 6.0f);  // the module's default (5), one step up
  shown = p.get_param(mp(40, 0));
  CHECK(shown == 0.25f);                  // the value that was there is kept
  CHECK(rig.editor->journal().depth() == 1u);
  rig.tap(Button::F8);                    // one undo puts both back
  CHECK(!p.has_param(mp(40, 1)));
  CHECK(p.instrument(40)->param_len == 4);
}

TEST_CASE("module ui: MIDI Learn binds a controller to a module parameter and it moves the module") {
  Rig rig;
  rig.goto_instrument(40);
  VState* st = rig.state_of(40);
  NEED(st != nullptr);
  CHECK(rig.app->model().modules != nullptr);
  rig.invoke("learn.toggle");
  CHECK(rig.app->state().overlay == app::OverlayId::Learn);
  rig.invoke("midi.cc", ActionArg((0u << 16) | (74u << 8) | 40u));  // the first message binds
  CHECK(rig.last_note() == "CC 74 > AMOUNT");
  rig.invoke("learn.toggle");
  const core::LearnBinding* slots = rig.project().settings().learn_slots();
  CHECK(core::learn_find_param(slots, core::kLearnSlots, mp(40, 0)) >= 0);
  // pickup: the knob has to reach the parameter (0.25 is 32 / 127) before it moves it
  rig.invoke("midi.cc", ActionArg((0u << 16) | (74u << 8) | 120u));
  CHECK(rig.value(mp(40, 0)) == 0.25f);
  rig.invoke("midi.cc", ActionArg((0u << 16) | (74u << 8) | 32u));
  rig.invoke("midi.cc", ActionArg((0u << 16) | (74u << 8) | 127u));
  CHECK(rig.value(mp(40, 0)) == 1.0f);
  rig.block();
  CHECK(st->last_value == 1.0f);
  // and the binding is undoable work like any other: the value goes back
  rig.tap(Button::F8);
  CHECK(rig.value(mp(40, 0)) != 1.0f);
}

// ---- the views ------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("module ui: a view is drawn inside its rectangle and nowhere else, and follows the parameters") {
  Rig rig;
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  const ui::Rect area = {200, 150, 160, 80};
  const std::vector<std::uint8_t> blank = picture(theme, [](ui::Renderer&) {});
  const auto outside_equal = [&](const std::vector<std::uint8_t>& a) {
    unsigned differ_outside = 0;
    unsigned differ_inside = 0;
    for (int y = 0; y < ui::kScreenH; ++y) {
      for (int x = 0; x < ui::kScreenW; ++x) {
        const std::size_t at = (static_cast<std::size_t>(y) * ui::kScreenW + static_cast<std::size_t>(x)) * 4;
        if (std::memcmp(&a[at], &blank[at], 4) != 0) {
          const bool inside = x >= area.x && x < area.x + area.w && y >= area.y && y < area.y + area.h;
          (inside ? differ_inside : differ_outside) += 1;
        }
      }
    }
    return std::pair<unsigned, unsigned>(differ_outside, differ_inside);
  };
  bool drawn = false;
  const std::vector<std::uint8_t> first = picture(theme, [&](ui::Renderer& r) { drawn = rig.mods->ui.draw_slot_view(r, rig.project(), 40, 0, area); });
  CHECK(drawn);
  const auto d0 = outside_equal(first);
  CHECK(d0.first == 0u);   // not one pixel outside the rectangle, although the view painted from -500 to 2500
  CHECK(d0.second > 3000u);
  CHECK(g_view_calls >= 1u);
  // AMOUNT 0.25: a quarter of the width is accent; with 0.75 three quarters
  const auto accent_pixels = [&](const std::vector<std::uint8_t>& img) {
    const ui::Rgb& a = theme.rgb(ui::Color::Accent);
    unsigned n = 0;
    for (int y = area.y; y < area.y + area.h; ++y) {
      for (int x = area.x; x < area.x + area.w; ++x) {
        const std::size_t at = (static_cast<std::size_t>(y) * ui::kScreenW + static_cast<std::size_t>(x)) * 4;
        n += img[at] == a.r && img[at + 1] == a.g && img[at + 2] == a.b ? 1u : 0u;
      }
    }
    return n;
  };
  const unsigned quarter = accent_pixels(first);
  CHECK(quarter > 2000u);
  CHECK(quarter < 3500u);
  CHECK(rig.project().set_param(mp(40, 0), 0.75f) == model::Status::Ok);
  const std::vector<std::uint8_t> second = picture(theme, [&](ui::Renderer& r) { CHECK(rig.mods->ui.draw_slot_view(r, rig.project(), 40, 0, area)); });
  CHECK(accent_pixels(second) > 2 * quarter);
  CHECK(outside_equal(second).first == 0u);
  // the second view: a text and a polyline, inside as well
  const std::vector<std::uint8_t> third = picture(theme, [&](ui::Renderer& r) { CHECK(rig.mods->ui.draw_slot_view(r, rig.project(), 40, 1, area)); });
  const auto d2 = outside_equal(third);
  CHECK(d2.first == 0u);
  CHECK(d2.second > 20u);
  // no view: out of range, too small, a module without views, a slot that is not a module's
  picture(theme, [&](ui::Renderer& r) {
    CHECK(!rig.mods->ui.draw_slot_view(r, rig.project(), 40, 2, area));
    CHECK(!rig.mods->ui.draw_slot_view(r, rig.project(), 40, 0, ui::Rect{0, 0, 10, 80}));
    CHECK(!rig.mods->ui.draw_slot_view(r, rig.project(), 3, 0, area));
    CHECK(!rig.mods->ui.draw_slot_view(r, rig.project(), 99, 0, area));
  });
  CHECK(rig.mods->ui.view_count(rig.project(), 40) == 2u);
  CHECK(rig.mods->ui.view_count(rig.project(), 3) == 0u);
}

TEST_CASE("module ui: the page draws the views of the slot's module and still fits the frame") {
  Rig rig;
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  rig.goto_instrument(40);
  const std::uint32_t before = rig.mods->ui.views_drawn();
  (void)picture(theme, [&](ui::Renderer& r) { app::render_scene(rig.app->model(), rig.app->state(), r); });  // picture() checks that the frame closed: it fit the display list
  CHECK(rig.mods->ui.views_drawn() == before + 2u);  // both views of the Viewer
  CHECK(rig.app->failed_frames() == 0u);
  // every page of a module slot, and the other themes
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    (void)picture(ui::theme(ui::theme_id_at(t)), [&](ui::Renderer& r) { app::render_scene(rig.app->model(), rig.app->state(), r); });
  }
}

TEST_CASE("module ui: a view that goes over budget is blanked, its module quarantined, and the others go on") {
  Rig rig;
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  const ui::Rect area = {200, 150, 160, 80};
  g_hog_calls = 0;
  const int hog = rig.mods->set.find("Hog");
  NEED(hog >= 0);
  const std::vector<std::uint8_t> blank = picture(theme, [](ui::Renderer&) {});
  const std::vector<std::uint8_t> frame = picture(theme, [&](ui::Renderer& r) { CHECK(!rig.mods->ui.draw_slot_view(r, rig.project(), 41, 0, area)); });
  CHECK(g_hog_calls == 1u);
  CHECK(frame == blank);  // blanked: not one pixel of the 96 rectangles it got through
  CHECK(rig.mods->set.views_blocked(static_cast<unsigned>(hog)));
  CHECK(rig.mods->set.quarantined("Hog"));
  // never called again
  (void)picture(theme, [&](ui::Renderer& r) { CHECK(!rig.mods->ui.draw_slot_view(r, rig.project(), 41, 0, area)); });
  CHECK(g_hog_calls == 1u);
  CHECK(rig.mods->ui.view_count(rig.project(), 41) == 0u);
  // the page of the quarantined module's slot still draws (without views), and the Viewer's views are untouched
  rig.goto_instrument(41);
  (void)picture(theme, [&](ui::Renderer& r) { app::render_scene(rig.app->model(), rig.app->state(), r); });
  rig.goto_instrument(40);
  bool drawn = false;
  (void)picture(theme, [&](ui::Renderer& r) { drawn = rig.mods->ui.draw_slot_view(r, rig.project(), 40, 0, area); });
  CHECK(drawn);
  // a slow view is the same: a clock that jumps while the view draws
  g_clock_us = 0;
}

// two views that are each a curve with axes: 416 words estimated apiece against the 700 the views of a page share
void greedy_draw(void*, std::uint32_t, o3m_canvas* c) {
  float y[100];
  for (unsigned i = 0; i < 100; ++i) {
    y[i] = std::sin(static_cast<float>(i) * 0.1f);
  }
  const o3m_curve cv = {nullptr, y, 100, O3M_AXIS_LINEAR, O3M_AXIS_LINEAR, 0.0f, 1.0f, -1.0f, 1.0f, O3M_CURVE_AXES, O3M_COLOR_INSTR};
  g_api->curve(c, &cv);
}

TEST_CASE("module ui: the views of a page share the display-list words: a view that takes more than is left is over budget") {
  Rig rig;
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  static const o3m_ui ui = {sizeof(o3m_ui), nullptr, nullptr, greedy_draw, nullptr, nullptr};
  static const o3m_module greedy = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "Greedy", sizeof(VState), 3, kViewerParams,
                                    v_init, v_render, v_set_param, v_note_on, v_note_off, v_shutdown, &ui, 2};
  CHECK(rig.mods->set.add_native("Greedy", greedy));
  model::Project& p = rig.project();
  CHECK(p.add_module("Greedy", 6, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Module);
  in.module_index = static_cast<std::uint8_t>(p.module_count() - 1u);
  const float blob[3] = {0.5f, 5.0f, 1.0f};
  std::memcpy(in.params, blob, sizeof blob);
  in.param_len = sizeof blob;
  CHECK(p.set_instrument(44, in) == model::Status::Ok);
  std::uint8_t* mem = nullptr;
  std::uint32_t addr = 0;
  NEED(rig.mods->set.cold().take(sizeof(VState), 16, mem, addr));
  std::memset(mem, 0, sizeof(VState));
  CHECK(rig.mods->voices.bind(44, greedy, module::fault_id("Greedy"), mem, in.params, in.param_len) == audio::ModuleVoices::Bind::Ok);
  const ui::Rect area = {200, 150, 160, 80};
  // one view of the two fits; the second is more than the page has left
  bool first = false;
  bool second = true;
  (void)picture(theme, [&](ui::Renderer& r) {
    rig.mods->ui.begin_view_frame();
    first = rig.mods->ui.draw_slot_view(r, p, 44, 0, area);
    second = rig.mods->ui.draw_slot_view(r, p, 44, 1, area);
  });
  CHECK(first);
  CHECK(!second);
  CHECK(rig.mods->set.quarantined("Greedy"));
  // the other module's page starts with all of the words again
  bool viewer = false;
  (void)picture(theme, [&](ui::Renderer& r) {
    rig.mods->ui.begin_view_frame();
    viewer = rig.mods->ui.draw_slot_view(r, p, 40, 0, area) && rig.mods->ui.draw_slot_view(r, p, 40, 1, area);
  });
  CHECK(viewer);
}

// ---- the engine picker -----------------------------------------------------------------------------------------------------------------------------------

// OS-BUGS3 (qa_instrument): F4 on an EMPTY slot is the engine picker, not the MOD page, and the picker captures every key but its own (the Pattern button included): the screen stays on
// Instrument until F8 closes it. A HIL scenario that walked F6 from a MIDI instrument to the empty slot below it (instead of to instrument 1) left the picker open across a project load
// and read the ignored Pattern button as a screen that would not change.
TEST_CASE("module ui: the engine picker of an empty slot captures the Pattern button until F8 closes it") {
  Rig rig;
  NEED(rig.project().instrument(42) == nullptr);
  rig.goto_instrument(42);
  rig.tap(Button::F4);
  NEED(rig.app->state().overlay == app::OverlayId::ModulePicker);
  rig.tap(Button::Pattern);
  CHECK(rig.app->state().screen == app::ScreenId::Instrument);
  CHECK(rig.app->state().overlay == app::OverlayId::ModulePicker);
  rig.tap(Button::F8);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  rig.tap(Button::Pattern);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);
}

TEST_CASE("module ui: the engine picker lists the card's modules and the project's missing ones, greyed, and binds the chosen one") {
  Rig rig;
  rig.card.touch("modules/instruments/Extra.tmod");
  CHECK(rig.mods->set.add_native("Extra", kPlain));
  model::Project& p = rig.project();
  NEED(p.instrument(42) == nullptr);
  rig.goto_instrument(42);
  rig.tap(Button::F4);
  CHECK(rig.app->state().overlay == app::OverlayId::ModulePicker);
  const app::ModuleUi& ui = rig.mods->ui;
  NEED(ui.instrument_count() == 5u);  // Extra, Hog, plain, Viewer (sorted, case-insensitively) and Gone (named by the project, not on the card)
  const char* names[5] = {"Extra", "Hog", "plain", "Viewer", "Gone"};
  for (unsigned i = 0; i < 5; ++i) {
    CHECK_MESSAGE(std::strcmp(ui.instrument_at(i)->stem, names[i]) == 0, names[i]);
  }
  using State = app::CatalogEntry::State;
  CHECK(ui.instrument_at(0)->state == State::Loaded);
  CHECK(ui.instrument_at(2)->state == State::Ready);
  CHECK(ui.instrument_at(4)->state == State::Missing);
  // the picker draws, and the cursor moves with the arrows
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  (void)picture(theme, [&](ui::Renderer& r) { app::render_scene(rig.app->model(), rig.app->state(), r); });
  CHECK(rig.app->state().modules.picker == 0);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.app->state().modules.picker == 2);
  rig.tap(Button::F8);  // back
  CHECK(rig.app->state().overlay == app::OverlayId::None);

  // a greyed one cannot be chosen
  rig.tap(Button::F4);
  rig.turn(4);
  CHECK(rig.app->state().modules.picker == 4);
  rig.tap(Button::F1);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.last_note() == "MODULE NOT ON THE CARD");
  CHECK(p.instrument(42) == nullptr);
  // a file that is not a module is refused and quarantined, and the slot is untouched
  rig.tap(Button::F4);
  rig.turn(2);
  rig.tap(Button::F1);
  CHECK(rig.last_note() == "MODULE REFUSED");
  CHECK(rig.mods->set.quarantined("plain"));
  CHECK(p.instrument(42) == nullptr);
  // a module the project does not name yet joins its table; the edit before it stays undoable (the table only grows)
  rig.goto_instrument(3);
  rig.turn(1);
  rig.goto_instrument(42);
  const unsigned modules_before = p.module_count();
  const std::size_t depth_before = rig.editor->journal().depth();
  rig.tap(Button::F4);
  CHECK(rig.app->state().modules.picker == 0);  // the slot has no module: the picker starts at the top
  rig.tap(Button::F1);                          // Extra
  CHECK(rig.last_note() == "ENGINE SET");
  NEED(p.instrument(42) != nullptr);
  CHECK(p.instrument(42)->kind == model::InstrumentKind::Module);
  CHECK(std::strcmp(p.module(p.instrument(42)->module_index)->name, "Extra") == 0);
  CHECK(p.module_count() == modules_before + 1u);
  CHECK(p.instrument(42)->param_len == 12);
  CHECK(p.get_param(mp(42, 0)) == 0.5f);   // the defaults
  CHECK(p.get_param(mp(42, 1)) == 5.0f);
  CHECK(rig.mods->voices.bound(42));
  CHECK(rig.editor->in_sync());
  CHECK(rig.editor->journal().depth() == depth_before + 1u);
  CHECK(rig.editor->can_undo());
  // the slot has an engine now: the picker cannot change it at run time (there is no unbind), and says so
  rig.goto_instrument(42);
  rig.tap(Button::F1);  // (OS-INSTUI D3: F1 of a module instrument is its first page, not the picker; the action stays for S5's chooser)
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  (void)rig.invoke("module.engine");
  CHECK(rig.app->state().modules.picker == 0);
  rig.tap(Button::F1);
  CHECK(rig.last_note() == "RELOAD TO PUT A MODULE HERE");
  // the instrument is undoable: one step takes it away again (the engine stays bound until the project is reloaded)
  rig.tap(Button::F8);
  CHECK(p.instrument(42) == nullptr);
  // without a document the pick says so
  rig.app->attach_document(nullptr);
  rig.goto_instrument(43);
  (void)rig.invoke("module.engine");  // (with no document there is no chooser: the action still says why)
  CHECK(rig.app->state().overlay == app::OverlayId::None);  // the picker does not open: it would pick for nothing
  CHECK(rig.last_note() == "NO PROJECT OPEN");
}

// OS-SMALLFIX6: `diag state` says the app under the cursor on MODULES (sel=), so a HIL scenario walks to an app by its stem instead of counting Down presses on a card that gains apps.
TEST_CASE("module ui: the state line names the app under the cursor on MODULES by its file stem, and `-` on other screens or with no app") {
  Rig rig;
  rig.card.touch("modules/apps/Pads.tmod");
  const auto sel = [&]() -> std::string {
    char buf[384];
    (void)app::describe_state(*rig.app, buf, sizeof buf);
    const std::string line = buf;
    const std::size_t at = line.find(" sel=");
    if (at == std::string::npos) {
      return "<missing>";
    }
    const std::size_t end = line.find(' ', at + 1);
    return line.substr(at + 5, end == std::string::npos ? std::string::npos : end - at - 5);
  };
  CHECK(sel() == "-");
  (void)rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  rig.pass(1);
  NEED(rig.app->state().screen == app::ScreenId::Modules);
  NEED(rig.mods->ui.app_entry_count() == 3u);
  CHECK(sel() == rig.mods->ui.app_entry_at(0)->stem);  // (a stem of the card as it is: Pads, Tool, zzz_not_loaded)
  rig.tap(Button::Down);
  CHECK(sel() == rig.mods->ui.app_entry_at(1)->stem);
  rig.tap(Button::Down);
  CHECK(sel() == "zzz_not_loaded");
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  CHECK(sel() == "Pads");
  (void)rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Pattern));
  rig.pass(1);
  CHECK(sel() == "-");
}

// ---- OS-NAV3: Shift + Up / Down page the MODULES list and the engine picker -------------------------------------------------------------------------

TEST_CASE("module ui: Shift + Up / Down page the MODULES list (the rows of the pane) and the engine picker (its ten rows), clamped, never wrapping") {
  Rig rig;
  for (unsigned i = 0; i < 20; ++i) {
    rig.card.touch(std::string("modules/apps/a") + std::to_string(10 + i) + ".tmod");
    rig.card.touch(std::string("modules/instruments/i") + std::to_string(10 + i) + ".tmod");
  }
  const auto shift_tap = [&](Button b) {
    rig.press(Button::Shift);
    rig.pass(1);
    rig.tap(b);
    rig.release(Button::Shift);
    rig.pass(1);
  };
  // MODULES: the catalogue holds sixteen entries, and sixteen rows are the pane, so a page is the whole list
  (void)rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  rig.pass(1);
  NEED(rig.app->state().screen == app::ScreenId::Modules);
  NEED(rig.mods->ui.app_entry_count() == app::kModuleCatalogMax);
  CHECK(app::kModulesScreen.page() == app::kFrameRows);
  CHECK(rig.app->state().modules.apps == 0);
  shift_tap(Button::Down);
  CHECK(rig.app->state().modules.apps == 15);  // the last entry
  shift_tap(Button::Down);
  CHECK(rig.app->state().modules.apps == 15);  // no wrap
  shift_tap(Button::Up);
  CHECK(rig.app->state().modules.apps == 0);
  shift_tap(Button::Up);
  CHECK(rig.app->state().modules.apps == 0);

  // the engine picker, on an empty slot
  rig.goto_instrument(42);
  rig.tap(Button::F4);
  NEED(rig.app->state().overlay == app::OverlayId::ModulePicker);
  NEED(rig.mods->ui.instrument_count() == app::kModuleCatalogMax);
  CHECK(app::kModulePickerOverlay.page() == app::kModulePickerRows);
  CHECK(rig.app->state().modules.picker == 0);
  shift_tap(Button::Down);
  CHECK(rig.app->state().modules.picker == app::kModulePickerRows);  // the first entry of the next page
  shift_tap(Button::Down);
  CHECK(rig.app->state().modules.picker == 15);  // stops on the last entry
  shift_tap(Button::Down);
  CHECK(rig.app->state().modules.picker == 15);
  shift_tap(Button::Up);
  CHECK(rig.app->state().modules.picker == 5);
  shift_tap(Button::Up);
  CHECK(rig.app->state().modules.picker == 0);
  shift_tap(Button::Up);
  CHECK(rig.app->state().modules.picker == 0);
  CHECK(rig.app->state().overlay == app::OverlayId::ModulePicker);  // (it did not close, and nothing was bound)
}

TEST_CASE("module ui: the MODULES list shorter than a page goes to its ends with Shift + Up / Down") {
  Rig rig;  // two apps
  const auto shift_tap = [&](Button b) {
    rig.press(Button::Shift);
    rig.pass(1);
    rig.tap(b);
    rig.release(Button::Shift);
    rig.pass(1);
  };
  (void)rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  rig.pass(1);
  NEED(rig.mods->ui.app_entry_count() == 2u);
  shift_tap(Button::Down);
  CHECK(rig.app->state().modules.apps == 1);
  shift_tap(Button::Up);
  CHECK(rig.app->state().modules.apps == 0);
}

// ---- OS-INSTUI S5: DELETE and INIT on a module slot ---------------------------------------------------------------------------------------------------

TEST_CASE("module ui: DELETE leaves the instance bound with its gate closed, refuses a new module there, allows a MIDI instrument, and undo and redo open and close the gate") {
  Rig rig;
  model::Project& p = rig.project();
  rig.goto_instrument(40);
  NEED(rig.mods->voices.routed(40));
  const std::size_t depth = rig.editor->journal().depth();
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F4);  // Shift + F4
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(p.instrument(40) == nullptr);
  CHECK(rig.last_note() == "INS 41 DELETED - F8 UNDO");
  CHECK(rig.editor->journal().depth() == depth + 1u);  // one undo unit
  CHECK(rig.mods->voices.bound(40));                    // there is no unbind
  CHECK(!rig.mods->voices.routed(40));                  // its gate is closed: no note starts (module_voices_test proves the sound)
  CHECK(!rig.mods->voices.note_on(0, 60, 40, 100));
  CHECK(!rig.mods->voices.audition(40, 0, 60, 100, true));
  CHECK(p.instrument(41) != nullptr);  // the neighbour is untouched
  CHECK(rig.mods->voices.routed(41));
  // the chooser is up; MODULE is refused on this slot until the project is reloaded
  rig.tap(Button::F4);
  CHECK(rig.app->state().overlay == app::OverlayId::ModulePicker);
  rig.tap(Button::F1);
  CHECK(rig.last_note() == "RELOAD TO PUT A MODULE HERE");
  CHECK(p.instrument(40) == nullptr);
  // a MIDI instrument is allowed (channel 1: slot 41 of 1..48), and the gate stays closed under it
  rig.tap(Button::F2);
  NEED(p.instrument(40) != nullptr);
  CHECK(p.instrument(40)->kind == model::InstrumentKind::Midi);
  CHECK(p.instrument(40)->midi_channel == 1);
  CHECK(!rig.mods->voices.routed(40));
  // undo the MIDI instrument, then the delete: the module sounds again; redo closes the gate again
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(p.instrument(40) == nullptr);
  CHECK(!rig.mods->voices.routed(40));
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  NEED(p.instrument(40) != nullptr);
  CHECK(p.instrument(40)->kind == model::InstrumentKind::Module);
  CHECK(rig.mods->voices.routed(40));
  CHECK(rig.mods->voices.note_on(0, 60, 40, 100));
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(p.instrument(40) == nullptr);
  CHECK(!rig.mods->voices.routed(40));
}

// OS-BUGS4 (BUG-01 of the v0.6.0 unit run): the console's `mod bind 61 tb303` binds an instance to a slot whose project instrument is free; the once-a-pass gate sync of the App (poll_modules)
// closed that gate within a pass, so the next `mod note` said "not bound, or the queue is full" (audition() asks routed()). A slot the console marked stays open.
TEST_CASE("module ui: a console bind on a free slot is not shut by the gate sync, so `mod note` plays it") {
  Rig rig;
  const unsigned slot = 55;
  NEED(rig.project().instrument(slot) == nullptr);
  const int idx = rig.mods->set.find("Hog");
  NEED(idx >= 0);
  const o3m_module& mod = *rig.mods->set.at(static_cast<unsigned>(idx))->entry;
  std::uint8_t* mem = nullptr;
  std::uint32_t addr = 0;
  NEED(rig.mods->set.cold().take(mod.state_bytes, 16, mem, addr));
  std::memset(mem, 0, mod.state_bytes);
  NEED(rig.mods->voices.bind(slot, mod, module::fault_id("Hog"), mem, nullptr, 0) == audio::ModuleVoices::Bind::Ok);
  CHECK(rig.mods->voices.routed(slot));  // a bind opens the gate
  rig.pass(2);
  CHECK_FALSE(rig.mods->voices.routed(slot));  // an unmarked bind is shut by the next pass: this is what the unit showed
  CHECK_FALSE(rig.mods->voices.audition(slot, audio::kAuditionTrack, 36, 110, true));
  rig.mods->ui.diag_keep_open(slot);  // what `mod bind` does after a successful bind
  rig.pass(2);
  CHECK(rig.mods->voices.routed(slot));
  CHECK(rig.mods->voices.audition(slot, audio::kAuditionTrack, 36, 110, true));
  // the project puts a MIDI instrument on the bench slot: the project decides again, the module is not routed (a sampler there would otherwise be silenced by it)
  {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Midi);
    in.midi_channel = static_cast<std::uint8_t>(slot - 47u);  // the MIDI slots are 48..63 for channels 1..16
    NEED(rig.project().set_instrument(slot, in) == model::Status::Ok);
    rig.pass(2);
    CHECK_FALSE(rig.mods->voices.routed(slot));
    CHECK_FALSE(rig.mods->voices.audition(slot, audio::kAuditionTrack, 36, 110, true));
    // and when the slot is free again the mark is gone: it stays closed (the project decided once)
    NEED(rig.project().clear_instrument(slot) == model::Status::Ok);
    rig.pass(2);
    CHECK_FALSE(rig.mods->voices.routed(slot));
  }
  // a project load (the platform's begin_replace calls diag_clear: platform-only, no host test; QA-MOD-116 checks "a load drops the binds" on the unit)
  rig.mods->ui.diag_keep_open(slot);
  rig.mods->ui.diag_clear();
  rig.pass(2);
  CHECK_FALSE(rig.mods->voices.routed(slot));
}

// OS-BUGS4: a bench slot that later holds a Module and is deleted closes like any other (the gate follows the project once the slot is not free).
TEST_CASE("module ui: a bench slot that holds a Module and is then deleted closes its gate") {
  Rig rig;
  const unsigned slot = 56;
  NEED(rig.project().instrument(slot) == nullptr);
  const int idx = rig.mods->set.find("Hog");
  NEED(idx >= 0);
  const o3m_module& mod = *rig.mods->set.at(static_cast<unsigned>(idx))->entry;
  std::uint8_t* mem = nullptr;
  std::uint32_t addr = 0;
  NEED(rig.mods->set.cold().take(mod.state_bytes, 16, mem, addr));
  std::memset(mem, 0, mod.state_bytes);
  NEED(rig.mods->voices.bind(slot, mod, module::fault_id("Hog"), mem, nullptr, 0) == audio::ModuleVoices::Bind::Ok);
  rig.mods->ui.diag_keep_open(slot);
  rig.pass(2);
  CHECK(rig.mods->voices.routed(slot));
  rig.install(rig.project(), slot, 1, false);  // a Module instrument of the project's table is there now
  rig.pass(2);
  CHECK(rig.mods->voices.routed(slot));
  CHECK(rig.project().clear_instrument(slot) == model::Status::Ok);  // deleted
  rig.pass(2);
  CHECK_FALSE(rig.mods->voices.routed(slot));
}

TEST_CASE("module ui: INIT puts a module's blob back to the module's defaults, keeps the module and the name, and is one undo unit") {
  Rig rig;
  model::Project& p = rig.project();
  rig.goto_instrument(40);
  CHECK(rig.value(mp(40, 0)) == 0.25f);  // the installed blob
  const std::size_t depth = rig.editor->journal().depth();
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Delete);  // Shift + Delete
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.last_note() == "INS 41 INIT - F8 UNDO");
  CHECK(rig.editor->journal().depth() == depth + 1u);
  CHECK(rig.value(mp(40, 0)) == 0.5f);
  CHECK(rig.value(mp(40, 1)) == 5.0f);
  CHECK(rig.value(mp(40, 2)) == 1.0f);
  NEED(p.instrument(40) != nullptr);
  CHECK(p.instrument(40)->kind == model::InstrumentKind::Module);
  CHECK(p.instrument(40)->module_index == 0);
  CHECK(std::strcmp(p.instrument(40)->name, "VIEWER") == 0);
  rig.block();
  CHECK(rig.state_of(40)->values[0] == 0.5f);  // the running instance was told
  // undo and redo of the whole-instrument unit reach the running instance too (not only the project)
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.value(mp(40, 0)) == 0.25f);
  rig.block();
  CHECK(rig.state_of(40)->values[0] == 0.25f);
  CHECK(rig.state_of(40)->values[1] == 3.0f);
  CHECK(rig.state_of(40)->values[2] == 1.0f);
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  rig.block();
  CHECK(rig.state_of(40)->values[0] == 0.5f);
  CHECK(rig.state_of(40)->values[1] == 5.0f);
  // again: already there, nothing to do (and no second undo unit)
  (void)rig.invoke("instrument.init");
  CHECK(rig.last_note() == "ALREADY AT THE DEFAULTS");
  CHECK(rig.editor->journal().depth() == depth + 1u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.value(mp(40, 0)) == 0.25f);
}

// ---- the MODULES screen and an app's page ---------------------------------------------------------------------------------------------------------------

TEST_CASE("module ui: MODULES is a cell of the Navigator with no button of its own") {
  const app::ScreenEntry* e = app::find_screen(app::navigator_screens(), app::ScreenId::Modules);
  NEED(e != nullptr);
  CHECK(e->buttonless);
  CHECK(!e->hidden);
  CHECK(e->screen == &app::kModulesScreen);
  const app::ScreenEntry* a = app::find_screen(app::navigator_screens(), app::ScreenId::ModuleApp);
  CHECK(a == nullptr);  // the app's page is hidden: reached by launching
  const app::ScreenEntry* all = app::find_screen(app::screen_registry(), app::ScreenId::ModuleApp);
  NEED(all != nullptr);
  CHECK(all->hidden);
  CHECK(all->screen->captures());
  CHECK(!e->screen->captures());
}

TEST_CASE("module ui: an app: launch from MODULES, input and drawing on its page, the Shift layer, the exit chord") {
  Rig rig;
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  // from the Navigator
  rig.invoke("navigator.open");
  rig.pass(1);
  const int at = app::screen_position(app::navigator_screens(), app::ScreenId::Modules);
  NEED(at >= 0);
  rig.app->invoke(core::action_id("cursor.jump_end"));
  CHECK(rig.app->state().navigator == at);
  rig.tap(Button::F1);  // open the chosen cell
  CHECK(rig.app->state().screen == app::ScreenId::Modules);
  CHECK(rig.mods->ui.app_entry_count() == 2u);  // Tool (native, loaded) and zzz_not_loaded (on the card)
  CHECK(std::strcmp(rig.mods->ui.app_entry_at(0)->stem, "Tool") == 0);
  CHECK(rig.mods->ui.app_entry_at(0)->state == app::CatalogEntry::State::Loaded);
  CHECK(rig.mods->ui.app_entry_at(1)->state == app::CatalogEntry::State::Ready);
  (void)picture(theme, [&](ui::Renderer& r) { app::render_scene(rig.app->model(), rig.app->state(), r); });
  rig.tap(Button::Down);
  CHECK(rig.app->state().modules.apps == 1);
  rig.tap(Button::F1);  // zzz_not_loaded: a file that is not a module
  CHECK(rig.app->state().screen == app::ScreenId::Modules);
  CHECK(rig.last_note() == "APP REFUSED");
  rig.tap(Button::Up);
  rig.tap(Button::F2);  // rescan keeps the cursor inside the list
  rig.tap(Button::F1);  // Tool
  CHECK(rig.app->state().screen == app::ScreenId::ModuleApp);
  CHECK(rig.mods->ui.app_running());
  VState* st = static_cast<VState*>(const_cast<void*>(rig.mods->ui.app_state()));
  NEED(st != nullptr);
  CHECK(st->enters == 1u);
  CHECK(rig.mods->ui.app_param_count() == 2u);
  CHECK(rig.mods->ui.app_param(0) == 10.0f);
  CHECK(st->values[0] == 10.0f);  // the app's set_param heard its defaults
  // input: pads, buttons and the encoder reach the app raw
  rig.pad(5, 99);
  CHECK(st->inputs == 2u);  // the press and the release; the release of the F1 that launched the app was not its business
  CHECK((st->last_event.kind == O3M_EV_PAD && st->last_event.id == 5u && st->last_event.value == 0 && st->last_event.mods == 0u));
  rig.tap(Button::F3);  // not an OS key on this page: the app sees it, the OS swallows it
  CHECK(st->inputs == 4u);
  CHECK((st->last_event.kind == O3M_EV_BUTTON && st->last_event.id == O3M_BTN_F3 && st->last_event.value == 0));
  CHECK(rig.app->state().screen == app::ScreenId::ModuleApp);
  rig.turn(3);
  CHECK((st->last_event.kind == O3M_EV_ENCODER && st->last_event.value == 3));
  CHECK(st->inputs == 5u);
  // (OS-APPUX S3: a screen button leaves the page, so the app never sees it: its own case below)
  // the Shift layer is the OS's: Shift + Down chooses the second parameter, Shift + the encoder edits it; the app does not hear either (it hears Shift itself)
  rig.press(Button::Shift);
  rig.pass(1);
  CHECK(st->inputs == 6u);
  CHECK((st->last_event.kind == O3M_EV_BUTTON && st->last_event.id == O3M_BTN_SHIFT && st->last_event.value == 1));
  rig.press(Button::Down);
  rig.pass(1);
  rig.release(Button::Down);
  rig.pass(1);
  CHECK(rig.app->state().modules.app_row == 1);
  rig.turn(10);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(st->inputs == 7u);  // only Shift's release more
  CHECK(rig.mods->ui.app_param(1) > 10.0f);
  CHECK(st->values[1] == rig.mods->ui.app_param(1));  // the app's set_param heard the edit
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Delete);  // Shift + Delete: the default
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.mods->ui.app_param(1) == 10.0f);
  // the page draws: the parameters, the view (a bar as long as the inputs it has seen), inside the frame
  const std::vector<std::uint8_t> frame = picture(theme, [&](ui::Renderer& r) { app::render_scene(rig.app->model(), rig.app->state(), r); });
  const ui::Rgb& play = theme.rgb(ui::Color::Play);
  unsigned green = 0;
  for (int y = 0; y < ui::kScreenH; ++y) {
    for (int x = 0; x < ui::kScreenW; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * ui::kScreenW + static_cast<std::size_t>(x)) * 4;
      green += frame[i] == play.r && frame[i + 1] == play.g && frame[i + 2] == play.b && y > 60 ? 1u : 0u;
    }
  }
  CHECK(green > 100u);
  // it is redrawn 30 times a second while it runs
  const std::uint32_t frames = rig.app->frames();
  rig.pass(40);
  CHECK(rig.app->frames() > frames);
  // the exit chord F7 + F8: back to MODULES, exit() and shutdown() called
  rig.press(Button::F7);
  rig.pass(1);
  rig.press(Button::F8);
  rig.pass(1);
  rig.release(Button::F7);
  rig.release(Button::F8);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Modules);
  CHECK(!rig.mods->ui.app_running());
  CHECK(st->exits == 1u);
  CHECK(st->shutdowns == 1u);
  // it can be launched again: a fresh state
  rig.tap(Button::F1);
  CHECK(rig.app->state().screen == app::ScreenId::ModuleApp);
  CHECK(st->enters == 1u);  // (the state was zeroed and init() run again: the counters started over)
  CHECK(rig.app->failed_frames() == 0u);
}

TEST_CASE("module ui: one state block serves every app launched, a larger app takes a new block (OS-FU1)") {
  static const o3m_module tool2 = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_APP, sizeof(o3m_module), "Tool2", sizeof(VState), 2, kAppParams,
                                   v_init, nullptr, v_set_param, nullptr, nullptr, v_shutdown, &kAppUi, 1};
  static const o3m_module big = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_APP, sizeof(o3m_module), "Big", 2u * sizeof(VState), 2, kAppParams,
                                 v_init, nullptr, v_set_param, nullptr, nullptr, v_shutdown, &kAppUi, 1};
  Rig rig;
  app::ModuleUi& ui = rig.mods->ui;
  CHECK(rig.mods->set.add_native("Tool2", tool2));
  CHECK(rig.mods->set.add_native("Big", big));
  rig.card.touch("modules/apps/Tool2.tmod");
  rig.card.touch("modules/apps/Big.tmod");
  ui.scan_apps();
  const auto entry_of = [&](const char* stem) {
    for (unsigned i = 0; i < ui.app_entry_count(); ++i) {
      if (std::strcmp(ui.app_entry_at(i)->stem, stem) == 0) {
        return i;
      }
    }
    return 999u;
  };
  const unsigned tool = entry_of("Tool");
  const unsigned t2 = entry_of("Tool2");
  const unsigned bigger = entry_of("Big");
  NEED(tool != 999u);
  NEED(t2 != 999u);
  NEED(bigger != 999u);
  const std::uint32_t before = rig.mods->set.cold().used();
  CHECK(ui.launch_app(tool) == app::ModuleUi::LaunchResult::Ok);
  const void* block = ui.app_state();
  NEED(block != nullptr);
  const std::uint32_t after_first = rig.mods->set.cold().used();
  CHECK(after_first > before);
  ui.app_exit();
  // Another app of the same size, and the first again, twice over: the same block, nothing more from the arena.
  for (unsigned round = 0; round < 3; ++round) {
    CHECK(ui.launch_app(t2) == app::ModuleUi::LaunchResult::Ok);
    CHECK(ui.app_state() == block);
    ui.app_exit();
    CHECK(ui.launch_app(tool) == app::ModuleUi::LaunchResult::Ok);
    CHECK(ui.app_state() == block);
    ui.app_exit();
  }
  CHECK(rig.mods->set.cold().used() == after_first);
  // An app that needs more takes a block of its own size; then the smaller ones use that one.
  CHECK(ui.launch_app(bigger) == app::ModuleUi::LaunchResult::Ok);
  const void* bigger_block = ui.app_state();
  CHECK(bigger_block != block);
  const std::uint32_t after_big = rig.mods->set.cold().used();
  CHECK(after_big > after_first);
  ui.app_exit();
  CHECK(ui.launch_app(tool) == app::ModuleUi::LaunchResult::Ok);
  CHECK(ui.app_state() == bigger_block);
  CHECK(rig.mods->set.cold().used() == after_big);
  ui.app_exit();
}

TEST_CASE("module ui: an app that goes over its time is stopped and quarantined, and the exit chord still works") {
  Rig rig;
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  rig.tap(Button::F1);
  NEED(rig.app->state().screen == app::ScreenId::ModuleApp);
  VState* st = static_cast<VState*>(const_cast<void*>(rig.mods->ui.app_state()));
  NEED(st != nullptr);
  g_slow_input_us = 6000;  // its input handler takes 6 ms: the budget is 4
  rig.pad(7);
  CHECK(rig.mods->ui.app_stopped());
  CHECK(rig.mods->ui.app_verdict() == module::ViewStatus::Time);
  CHECK(rig.mods->set.quarantined("Tool"));
  CHECK(rig.mods->set.views_blocked(static_cast<unsigned>(rig.mods->set.find("Tool"))));
  const std::uint32_t heard = st->inputs;
  CHECK(heard == 1u);  // the press: the release never reached it
  rig.pad(8);
  rig.turn(1);
  CHECK(st->inputs == heard);
  // the page says so and still draws
  g_slow_input_us = 0;
  (void)picture(theme, [&](ui::Renderer& r) { app::render_scene(rig.app->model(), rig.app->state(), r); });
  CHECK(rig.app->state().screen == app::ScreenId::ModuleApp);
  // the exit chord
  rig.press(Button::F7);
  rig.pass(1);
  rig.press(Button::F8);
  rig.pass(1);
  rig.release(Button::F7);
  rig.release(Button::F8);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Modules);
  CHECK(!rig.mods->ui.app_running());
  CHECK(st->exits == 0u);  // a stopped app is not called to say goodbye
  // and it cannot be launched again this boot
  rig.tap(Button::F1);
  CHECK(rig.app->state().screen == app::ScreenId::Modules);
  CHECK(rig.last_note() == "APP QUARANTINED UNTIL REBOOT");
}

TEST_CASE("module ui: a project change under a running app takes its page away") {
  Rig rig;
  rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  rig.tap(Button::F1);
  NEED(rig.app->state().screen == app::ScreenId::ModuleApp);
  rig.mods->ui.reset();  // the platform cleared the modules for another project
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Modules);
  CHECK(!rig.mods->ui.app_running());
  CHECK(rig.mods->ui.app_state() == nullptr);
}

TEST_CASE("module ui: without a module system the screens say so and nothing breaks") {
  Rig rig;
  rig.app->attach_modules(nullptr);
  rig.goto_instrument(40);
  (void)rig.invoke("module.engine");  // (F1 of a module instrument is its first page now)
  CHECK(rig.last_note() == "NO MODULE SYSTEM IN THIS BUILD");
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(app::kParamsScreen.count(rig.app->model(), rig.app->state()) == 0);  // a module instrument with no module system: no rows
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  (void)picture(theme, [&](ui::Renderer& r) { app::render_scene(rig.app->model(), rig.app->state(), r); });
  rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  (void)picture(theme, [&](ui::Renderer& r) { app::render_scene(rig.app->model(), rig.app->state(), r); });
  rig.tap(Button::F1);
  CHECK(rig.app->state().screen == app::ScreenId::Modules);
}

// OS-004d: an app's parameters are the project's (the APPM chunk): kept when they change, applied when the app is launched again, only for the same module name and ABI major.
TEST_CASE("module ui: an app's parameters are stored in the project, applied at the next launch, and ignored for another ABI major (OS-004d)") {
  Rig rig;
  app::ModuleUi& ui = rig.mods->ui;
  model::Project& p = rig.project();
  const std::uint32_t apps_before = p.app_params_count();
  rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  rig.tap(Button::F1);
  NEED(rig.app->state().screen == app::ScreenId::ModuleApp);
  CHECK(ui.app_param(0) == 10.0f);  // the defaults
  rig.pass(40);
  CHECK(p.app_params_count() == apps_before);  // launching an app, and leaving its values at the defaults, writes nothing
  const std::uint32_t generation = p.generation();
  CHECK(ui.app_set_param(0, 55.0f));
  CHECK(ui.app_set_param(1, 7.0f));
  rig.pass(40);  // the pass copies them into the project
  const model::AppParams* saved = p.find_app_params("Tool", 4);
  NEED(saved != nullptr);
  CHECK(saved->count == 2);
  CHECK(saved->value[0] == 55.0f);
  CHECK(saved->value[1] == 7.0f);
  CHECK(saved->abi_major == O3M_ABI_MAJOR);
  CHECK(saved->abi_minor == 9);
  CHECK(p.generation() != generation);
  CHECK(rig.editor->in_sync());  // not an edit behind the editor's back: the undo history stays
  const std::uint32_t after = p.generation();
  rig.pass(40);
  CHECK(p.generation() == after);  // nothing changed since: nothing written

  // leave the app and launch it again: its values are the project's, not the defaults
  rig.press(Button::F7);
  rig.pass(1);
  rig.press(Button::F8);
  rig.pass(1);
  rig.release(Button::F7);
  rig.release(Button::F8);
  rig.pass(1);
  NEED(!ui.app_running());
  rig.tap(Button::F1);
  NEED(ui.app_running());
  CHECK(ui.app_param(0) == 55.0f);
  CHECK(ui.app_param(1) == 7.0f);
  const VState* st = static_cast<const VState*>(ui.app_state());
  CHECK(st->values[0] == 55.0f);  // and the app itself was told
  CHECK(st->values[1] == 7.0f);

  // the entry of another ABI major is not applied (the module may mean something else by its parameters)
  const auto relaunch = [&]() {  // the MODULES screen's F1: the action that applies the project's values
    ui.app_exit();
    rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
    rig.tap(Button::F1);
    return ui.app_running();
  };
  const float other[2] = {99.0f, 5.0f};
  CHECK(rig.editor->set_app_params("Tool", 4, O3M_ABI_MAJOR + 1u, 0, other, 2) == model::Status::Ok);
  CHECK(relaunch());
  CHECK(ui.app_param(0) == 10.0f);
  // fewer values than the module has: the rest keep their defaults; more than it has: the extra are ignored
  const float three[3] = {20.0f, 30.0f, 40.0f};
  CHECK(rig.editor->set_app_params("Tool", 4, O3M_ABI_MAJOR, 0, three, 3) == model::Status::Ok);
  CHECK(relaunch());
  CHECK(ui.app_param(0) == 20.0f);
  CHECK(ui.app_param(1) == 30.0f);
  const float one[1] = {33.0f};
  CHECK(rig.editor->set_app_params("Tool", 4, O3M_ABI_MAJOR, 0, one, 1) == model::Status::Ok);
  CHECK(relaunch());
  CHECK(ui.app_param(0) == 33.0f);
  CHECK(ui.app_param(1) == 10.0f);
  // an entry for a module that is not installed stays in the project untouched
  const float gone[1] = {1.0f};
  CHECK(rig.editor->set_app_params("Missing", 7, 1, 0, gone, 1) == model::Status::Ok);
  rig.pass(40);
  CHECK(p.find_app_params("Missing", 7) != nullptr);
  CHECK(p.find_app_params("Missing", 7)->value[0] == 1.0f);
}

// ---- OS-KEYROWDLG S5, ABI 1.9: the app page's key row, F8 BACK, Play withheld ----------------------------------------------------------------------
namespace {
const o3m_keys* g_keys_answer = nullptr;
std::uint32_t g_keys_calls = 0;
std::uint32_t g_keys_cost_us = 0;
const o3m_keys* keyed_keys(void*) {
  ++g_keys_calls;
  g_clock_us += g_keys_cost_us;
  return g_keys_answer;
}
const o3m_ui kKeyedUi = {sizeof(o3m_ui), app_enter, app_input, app_draw, app_exit, keyed_keys};
const o3m_module kKeyed = {O3M_ABI_MAJOR, 9, O3M_KIND_APP, sizeof(o3m_module), "Keyed", sizeof(VState), 2, kAppParams, v_init, nullptr, v_set_param, nullptr, nullptr, v_shutdown, &kKeyedUi, 1};
// the same table declared as 1.8: the keys member is not read (its size says 24 on the host, but the module says 1.8)
const o3m_module kKeyed18 = {O3M_ABI_MAJOR, 8, O3M_KIND_APP, sizeof(o3m_module), "Keyed18", sizeof(VState), 2, kAppParams, v_init, nullptr, v_set_param, nullptr, nullptr, v_shutdown, &kKeyedUi, 1};

unsigned launch_native(Rig& rig, const char* stem) {
  rig.card.touch((std::string("modules/apps/") + stem + ".tmod").c_str());
  rig.mods->ui.scan_apps();
  for (unsigned i = 0; i < rig.mods->ui.app_entry_count(); ++i) {
    if (std::strcmp(rig.mods->ui.app_entry_at(i)->stem, stem) == 0) {
      return i;
    }
  }
  return 999u;
}
}  // namespace

TEST_CASE("module ui: keys(): the 16 labels are copied and validated, F8's slots are ignored, a NULL table is a blank row, a 1.8 app has no keys (ABI 1.9)") {
  Rig rig;
  app::ModuleUi& ui = rig.mods->ui;
  CHECK(rig.mods->set.add_native("Keyed", kKeyed));
  CHECK(rig.mods->set.add_native("Keyed18", kKeyed18));
  const unsigned keyed = launch_native(rig, "Keyed");
  const unsigned old = launch_native(rig, "Keyed18");
  NEED(keyed != 999u);
  NEED(old != 999u);
  static const o3m_keys table = {{"APPLY", "", nullptr, "ABCDEFGHIJ", "A\x01" "B", "TOO", "7CHARS!", "NOPE"},
                                 {"SHIFT1", nullptr, "tab\t", "OK", "", "", "", "NOPE"}};
  g_keys_answer = &table;
  g_keys_calls = 0;
  g_keys_cost_us = 0;
  CHECK(ui.launch_app(keyed) == app::ModuleUi::LaunchResult::Ok);
  CHECK(g_keys_calls == 1u);  // asked once at the launch, before the page draws
  CHECK(ui.app_keys_declared());
  CHECK(std::strcmp(ui.app_key_label(0, false), "APPLY") == 0);
  CHECK(std::strcmp(ui.app_key_label(1, false), "") == 0);
  CHECK(std::strcmp(ui.app_key_label(2, false), "") == 0);               // NULL: blank
  CHECK(std::strcmp(ui.app_key_label(3, false), "ABCDEFG") == 0);        // cut to 7
  CHECK(std::strcmp(ui.app_key_label(4, false), "") == 0);               // a control character: the label is blank
  CHECK(std::strcmp(ui.app_key_label(6, false), "7CHARS!") == 0);        // exactly 7, printable
  CHECK(std::strcmp(ui.app_key_label(7, false), "") == 0);               // F8 is the OS's
  CHECK(std::strcmp(ui.app_key_label(0, true), "SHIFT1") == 0);
  CHECK(std::strcmp(ui.app_key_label(2, true), "") == 0);                // a tab is not printable
  CHECK(std::strcmp(ui.app_key_label(3, true), "OK") == 0);
  CHECK(std::strcmp(ui.app_key_label(7, true), "") == 0);                // Shift + F8 stays blank
  CHECK(std::strcmp(ui.app_key_label(8, false), "") == 0);               // out of range
  // once after every input() and once a frame (the page's draw)
  CHECK(ui.app_input(O3M_EV_PAD, 3, 100, false));
  CHECK(g_keys_calls == 2u);
  rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  ui.app_exit();
  CHECK(!ui.app_keys_declared());
  // a NULL table: declared, every key blank
  g_keys_answer = nullptr;
  CHECK(ui.launch_app(keyed) == app::ModuleUi::LaunchResult::Ok);
  CHECK(ui.app_keys_declared());
  for (unsigned i = 0; i < 8u; ++i) {
    CHECK(std::strcmp(ui.app_key_label(i, false), "") == 0);
    CHECK(std::strcmp(ui.app_key_label(i, true), "") == 0);
  }
  ui.app_exit();
  // an app that declares 1.8 is never asked, whatever its table says
  g_keys_answer = &table;
  g_keys_calls = 0;
  CHECK(ui.launch_app(old) == app::ModuleUi::LaunchResult::Ok);
  CHECK(!ui.app_keys_declared());
  CHECK(g_keys_calls == 0u);
  ui.app_exit();
}

TEST_CASE("module ui: a keys() that overruns the time budget stops and quarantines the app like any other call (ABI 1.9)") {
  Rig rig;
  app::ModuleUi& ui = rig.mods->ui;
  CHECK(rig.mods->set.add_native("Keyed", kKeyed));
  const unsigned keyed = launch_native(rig, "Keyed");
  NEED(keyed != 999u);
  static const o3m_keys table = {{"GO"}, {}};
  g_keys_answer = &table;
  g_keys_cost_us = 0;
  CHECK(ui.launch_app(keyed) == app::ModuleUi::LaunchResult::Ok);
  CHECK(ui.app_keys_declared());
  g_keys_cost_us = 6000;  // the budget is 4 ms
  CHECK(ui.app_input(O3M_EV_PAD, 3, 100, false));
  CHECK(ui.app_stopped());
  CHECK(ui.app_verdict() == module::ViewStatus::Time);
  CHECK(!ui.app_keys_declared());  // a stopped app shows the legacy row, BACK on F8
  CHECK(rig.mods->set.quarantined("Keyed"));
  g_keys_cost_us = 0;
  ui.app_exit();
}

namespace {
// A label the app points at lives in a buffer that the crash-stamp hook overwrites the moment the app's stamp ends: an OS that read the table or the strings after the call would show "XXXXXXX" (and, with a
// wild pointer, fault outside the app's stamp, where the quarantine cannot name the module).
char g_live_label[8];
bool g_stamped = false;
bool g_keys_ran_stamped = false;
const o3m_keys* live_keys(void*) {
  g_keys_ran_stamped = g_stamped;
  std::memcpy(g_live_label, "APPLY", 6);
  static const o3m_keys table = {{g_live_label, g_live_label}, {g_live_label}};
  return &table;
}
std::uint16_t stamp_hook(std::uint16_t id) {
  if (id != 0u) {
    g_stamped = true;
  } else {
    g_stamped = false;
    std::memcpy(g_live_label, "XXXXXXX", 8);
  }
  return 0;
}
const o3m_ui kLiveUi = {sizeof(o3m_ui), app_enter, app_input, app_draw, app_exit, live_keys};
const o3m_module kLive = {O3M_ABI_MAJOR, 9, O3M_KIND_APP, sizeof(o3m_module), "Live", sizeof(VState), 2, kAppParams, v_init, nullptr, v_set_param, nullptr, nullptr, v_shutdown, &kLiveUi, 1};
}  // namespace

TEST_CASE("module ui: keys() labels are read while the app's crash stamp is still on: nothing the app's pointers hold after the call is used (ABI 1.9)") {
  Rig rig;
  app::ModuleUi& ui = rig.mods->ui;
  CHECK(rig.mods->set.add_native("Live", kLive));
  const unsigned live = launch_native(rig, "Live");
  NEED(live != 999u);
  module::ViewHost::Hooks hooks;
  hooks.now_us = test_clock;
  hooks.enter = stamp_hook;
  ui.views().set_hooks(hooks);
  g_stamped = g_keys_ran_stamped = false;
  CHECK(ui.launch_app(live) == app::ModuleUi::LaunchResult::Ok);
  CHECK(g_keys_ran_stamped);  // keys() ran under the stamp
  CHECK(ui.app_keys_declared());
  CHECK(std::strcmp(ui.app_key_label(0, false), "APPLY") == 0);
  CHECK(std::strcmp(ui.app_key_label(1, false), "APPLY") == 0);
  CHECK(std::strcmp(ui.app_key_label(0, true), "APPLY") == 0);
  CHECK(std::strcmp(g_live_label, "XXXXXXX") == 0);  // by the time the call returned to the OS, the app's own strings were something else
  CHECK(ui.app_input(O3M_EV_PAD, 3, 100, false));    // asked again after an input: the same
  CHECK(std::strcmp(ui.app_key_label(0, false), "APPLY") == 0);
  CHECK(std::strcmp(ui.app_key_label(0, true), "APPLY") == 0);
  ui.app_exit();
}

namespace {
// What the transport was asked: the Play of an app page is the transport's (the action transport.play), so a sink behind it counts the starts and stops.
struct CountingTransport final : app::TransportSink {
  unsigned plays = 0, songs = 0, stops = 0;
  bool play(unsigned) noexcept override {
    ++plays;
    return true;
  }
  bool play_song(unsigned) noexcept override {
    ++songs;
    return true;
  }
  bool stop() noexcept override {
    ++stops;
    return true;
  }
};
}  // namespace

TEST_CASE("module ui: on an app page F8 is BACK and Play the transport's: neither reaches the app, Pads and Rec still do; the legacy row reads BACK (ABI 1.9)") {
  CHECK(std::strcmp(app::kModuleAppSoftKeys[7], "BACK") == 0);
  for (unsigned i = 0; i < 7u; ++i) {
    CHECK(std::strcmp(app::kModuleAppSoftKeys[i], "--") == 0);
  }
  Rig rig;
  CountingTransport transport;
  rig.app->attach_transport(&transport);
  rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  rig.tap(Button::F1);  // Tool, a legacy-shaped app (it declares no keys)
  NEED(rig.app->state().screen == app::ScreenId::ModuleApp);
  CHECK(!rig.mods->ui.app_keys_declared());
  VState* st = static_cast<VState*>(const_cast<void*>(rig.mods->ui.app_state()));
  NEED(st != nullptr);
  rig.pass(1);
  rig.pad(3);
  const std::uint32_t after_pad = st->inputs;
  CHECK(after_pad >= 1u);                    // a pad is delivered
  rig.tap(Button::Rec);
  const std::uint32_t after_rec = st->inputs;
  CHECK(after_rec > after_pad);              // Rec is delivered
  rig.tap(Button::Play);
  rig.pass(1);
  CHECK(st->inputs == after_rec);            // Play never reaches the app ...
  CHECK(rig.app->state().screen == app::ScreenId::ModuleApp);
  CHECK(transport.plays == 1u);              // ... it drives the transport, which starts (the pattern in view) and shows it
  CHECK(rig.app->state().playing);
  rig.tap(Button::Play);
  rig.pass(1);
  CHECK(st->inputs == after_rec);
  CHECK((transport.stops == 1u && transport.plays == 1u));  // the second Play stops it
  CHECK(!rig.app->state().playing);
  // Shift + F1 is not the OS's on this page: the app gets F1 with the Shift flag (the Shift row of the shipped apps says what F1 does because of this)
  rig.press(Button::Shift);
  rig.pass(1);
  const std::uint32_t shift_down = st->inputs;  // (the app sees Shift itself)
  rig.tap(Button::F1);
  CHECK(st->inputs == shift_down + 2u);         // the press and the release
  CHECK((st->last_event.kind == O3M_EV_BUTTON && st->last_event.id == O3M_BTN_F1 && st->last_event.value == 0 && st->last_event.mods == O3M_MOD_SHIFT));
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::ModuleApp);
  rig.press(Button::Shift);                  // (the app sees Shift itself)
  rig.pass(1);
  const std::uint32_t with_shift = st->inputs;
  rig.tap(Button::F8);                       // Shift + F8 reads nothing and does nothing
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::ModuleApp);
  CHECK(st->inputs == with_shift);
  rig.release(Button::Shift);
  rig.pass(1);
  const std::uint32_t before_back = st->inputs;
  rig.tap(Button::F8);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Modules);  // BACK: one level, MODULES
  CHECK(!rig.mods->ui.app_running());
  CHECK(st->exits == 1u);
  CHECK(st->inputs == before_back);          // F8 never reached the app, nor its release
}

// ---- OS-APPUX S2: the Shift layer is the OS's parameter column only while the app declares a parameter ----------------------------------------------
namespace {
const o3m_module kBare = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_APP, sizeof(o3m_module), "Bare", sizeof(VState), 0, nullptr, v_init, nullptr, nullptr, nullptr, nullptr, v_shutdown, &kAppUi, 1};
}  // namespace

TEST_CASE("module ui: Shift + the arrows, Shift + Delete and Shift + the encoder reach an app that declares no parameter, and stay the OS's while it declares one (OS-APPUX S2)") {
  Rig rig;
  CHECK(rig.mods->set.add_native("Bare", kBare));
  rig.card.touch("modules/apps/Bare.tmod");
  rig.mods->ui.scan_apps();
  rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  CHECK(std::strcmp(rig.mods->ui.app_entry_at(0)->stem, "Bare") == 0);
  rig.tap(Button::F1);
  NEED(rig.app->state().screen == app::ScreenId::ModuleApp);
  CHECK(rig.mods->ui.app_param_count() == 0u);
  VState* st = static_cast<VState*>(const_cast<void*>(rig.mods->ui.app_state()));
  NEED(st != nullptr);
  rig.press(Button::Shift);
  rig.pass(1);
  std::uint32_t seen = st->inputs;
  for (Button b : {Button::Up, Button::Down, Button::Left, Button::Right, Button::Delete}) {
    rig.tap(b);
    CHECK_MESSAGE(st->inputs == seen + 2u, "Shift + key " << static_cast<int>(b) << ": the press and the release reach the app");
    CHECK(st->last_event.mods == O3M_MOD_SHIFT);
    seen = st->inputs;
  }
  rig.turn(3);
  CHECK(st->inputs == seen + 1u);
  CHECK((st->last_event.kind == O3M_EV_ENCODER && st->last_event.value == 3 && st->last_event.mods == O3M_MOD_SHIFT));
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::ModuleApp);
  rig.tap(Button::F8);
  NEED(rig.app->state().screen == app::ScreenId::Modules);
  // an app with a declared row: the same keys stay the OS's, the app hears only Shift itself
  rig.tap(Button::Down);
  rig.tap(Button::F1);
  NEED(rig.app->state().screen == app::ScreenId::ModuleApp);
  CHECK(rig.mods->ui.app_param_count() == 2u);
  st = static_cast<VState*>(const_cast<void*>(rig.mods->ui.app_state()));
  NEED(st != nullptr);
  rig.press(Button::Shift);
  rig.pass(1);
  seen = st->inputs;
  for (Button b : {Button::Up, Button::Down, Button::Left, Button::Right, Button::Delete}) {
    rig.tap(b);
    CHECK_MESSAGE(st->inputs == seen, "Shift + key " << static_cast<int>(b) << " is withheld from an app with a declared row");
  }
  rig.turn(3);
  CHECK(st->inputs == seen);
  rig.release(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F8);
  NEED(rig.app->state().screen == app::ScreenId::Modules);
  // an app of 1.10 that declares rows (as storage) has no OS column: every Shift key reaches it, and the page lists no parameter
  CHECK(rig.mods->set.add_native("Tool10", kTool10));
  rig.card.touch("modules/apps/Tool10.tmod");
  rig.mods->ui.scan_apps();
  rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  for (unsigned i = 0; i < rig.mods->ui.app_entry_count(); ++i) {
    if (std::strcmp(rig.mods->ui.app_entry_at(i)->stem, "Tool10") == 0) {
      CHECK(rig.mods->ui.launch_app(i) == app::ModuleUi::LaunchResult::Ok);
    }
  }
  rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::ModuleApp));
  NEED(rig.mods->ui.app_running());
  CHECK(rig.mods->ui.app_param_count() == 2u);
  CHECK(!rig.mods->ui.app_os_column());
  st = static_cast<VState*>(const_cast<void*>(rig.mods->ui.app_state()));
  NEED(st != nullptr);
  rig.press(Button::Shift);
  rig.pass(1);
  seen = st->inputs;
  for (Button b : {Button::Up, Button::Down, Button::Left, Button::Right, Button::Delete}) {
    rig.tap(b);
    CHECK_MESSAGE(st->inputs == seen + 2u, "Shift + key " << static_cast<int>(b) << " reaches an app of 1.10 with declared rows");
    seen = st->inputs;
  }
  rig.turn(3);
  CHECK((st->last_event.kind == O3M_EV_ENCODER && st->last_event.mods == O3M_MOD_SHIFT));
  CHECK(rig.mods->ui.app_param(0) == 10.0f);  // (the OS did not edit it)
  rig.release(Button::Shift);
  rig.pass(1);
}

TEST_CASE("module ui: a screen button on an app's page leaves the app for its screen: exit() and shutdown() run, the app never hears the button, each of the eleven goes where the global binding goes (OS-APPUX S3)") {
  struct Case {
    Button b;
    app::ScreenId screen;
  };
  const Case cases[] = {{Button::Pattern, app::ScreenId::Pattern},   {Button::Song, app::ScreenId::Song},           {Button::Params, app::ScreenId::Instrument}, {Button::SamplePlay, app::ScreenId::Wave},
                        {Button::SampleEdit, app::ScreenId::SampleEdit}, {Button::SampleLoad, app::ScreenId::SampleLoad}, {Button::SampleRec, app::ScreenId::Record}, {Button::Master, app::ScreenId::Mixer},
                        {Button::Performance, app::ScreenId::Perform}, {Button::File, app::ScreenId::File},         {Button::Config, app::ScreenId::Config}};
  for (const Case& c : cases) {
    Rig rig;
    rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
    rig.tap(Button::F1);  // Tool
    NEED(rig.app->state().screen == app::ScreenId::ModuleApp);
    VState* st = static_cast<VState*>(const_cast<void*>(rig.mods->ui.app_state()));
    NEED(st != nullptr);
    rig.pass(1);
    const std::uint32_t heard = st->inputs;
    CAPTURE(static_cast<int>(c.b));
    rig.tap(c.b);
    rig.pass(1);
    CHECK(rig.app->state().screen == c.screen);
    CHECK(!rig.mods->ui.app_running());
    CHECK(st->inputs == heard);  // neither the press nor the release reached the app
    CHECK(st->exits == 1u);
    CHECK(st->shutdowns == 1u);
  }
  // Shift + a screen button leaves as well: a held Shift falls back to the plain key
  Rig rig;
  rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Modules));
  rig.tap(Button::F1);
  NEED(rig.app->state().screen == app::ScreenId::ModuleApp);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Song);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Song);  // (a held Shift falls back to the plain key, as everywhere: it leaves too)
}
