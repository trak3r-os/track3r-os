// SPDX-License-Identifier: GPL-3.0-or-later
// The gallery and budget states of OS-012b: the SD modules' pages. A module instrument's page with the module's own controls and views, the worst page the
// budgets allow a module, the engine picker, the MODULES screen, and an app's page running and stopped. The modules are native ones written here against the
// SDK (sdk/o3module.h) and the card is an in-memory one (platform/host FakeStorage); the pages are drawn through the app's scene, so what is measured and shown is
// what the device draws.
#include "screens_app.h"

#include <o3os/app/model.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/view_state.h>
#include <o3os/audio/module_voices.h>
#include <o3os/model/params.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/fake_storage.h>
#include <o3os/sim/demo_project.h>

#include <cstring>

namespace o3os::sim {

namespace {

using app::Model;
using app::OverlayId;
using app::ScreenId;
using app::ViewState;
namespace ip = model::instrument_param;

// ---- the native modules ----------------------------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kLog = O3M_PARAM_LOG;
constexpr std::uint32_t kInt = O3M_PARAM_INT;
constexpr std::uint32_t kBool = O3M_PARAM_BOOL;
constexpr std::uint32_t unit(std::uint32_t u) { return O3M_PARAM_WITH_UNIT(u); }

const o3m_param kDemoParams[12] = {
    {"Level", -48.0f, 6.0f, -6.0f, unit(O3M_UNIT_DB)},
    {"Cutoff", 20.0f, 20000.0f, 1200.0f, kLog | unit(O3M_UNIT_HZ)},
    {"Resonance", 0.0f, 100.0f, 35.0f, unit(O3M_UNIT_PERCENT)},
    {"Drive", 0.0f, 100.0f, 10.0f, unit(O3M_UNIT_PERCENT)},
    {"Attack", 1.0f, 2000.0f, 5.0f, kLog | unit(O3M_UNIT_MS)},
    {"Decay", 1.0f, 2000.0f, 250.0f, kLog | unit(O3M_UNIT_MS)},
    {"Sustain", 0.0f, 100.0f, 70.0f, unit(O3M_UNIT_PERCENT)},
    {"Release", 1.0f, 5000.0f, 400.0f, kLog | unit(O3M_UNIT_MS)},
    {"Tune", -24.0f, 24.0f, 0.0f, kInt | unit(O3M_UNIT_SEMITONES)},
    {"Fine Tune", -100.0f, 100.0f, 0.0f, kInt | unit(O3M_UNIT_CENTS)},
    {"Glide", 0.0f, 500.0f, 0.0f, unit(O3M_UNIT_MS)},
    {"Mono", 0.0f, 1.0f, 1.0f, kBool},
};

std::uint32_t nop_render(void*, const float*, float*, std::uint32_t) { return 0; }
int nop_init(void*, const o3m_host*) { return 0; }
void nop_set_param(void*, std::uint32_t, float) {}
void nop_note_on(void*, std::uint32_t, std::uint32_t, std::uint32_t) {}
void nop_note_off(void*, std::uint32_t) {}
void nop_shutdown(void*) {}

float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// view 0: the envelope of the four time parameters; view 1: the level, cutoff, resonance and drive as bars.
void demo_view(void*, std::uint32_t view, o3m_canvas* c) {
  const o3m_draw_api* api = c->draw;
  if (view == 0u) {
    const float a = c->params[4], d = c->params[5], s = c->params[6] * 0.01f, r = c->params[7];
    const float total = a + d + r + 200.0f;
    const float xs[5] = {0.0f, a * 0.001f, (a + d) * 0.001f, (a + d + 200.0f) * 0.001f, total * 0.001f};
    const float ys[5] = {0.0f, 1.0f, s, s, 0.0f};
    const o3m_curve cv = {xs, ys, 5, O3M_AXIS_LINEAR, O3M_AXIS_LINEAR, 0.0f, total * 0.001f, 0.0f, 1.0f, O3M_CURVE_AXES | O3M_CURVE_FILL, O3M_COLOR_INSTR};
    api->curve(c, &cv);
  } else {
    static const char* const kNames[4] = {"LEVEL", "CUT", "RES", "DRIVE"};
    const float norm[4] = {(c->params[0] + 48.0f) / 54.0f, c->params[1] / 20000.0f, c->params[2] * 0.01f, c->params[3] * 0.01f};
    const int bar_w = c->width / 4 - 8;
    for (int i = 0; i < 4; ++i) {
      const int x = i * (c->width / 4) + 4;
      const int h = static_cast<int>(clampf(norm[i], 0.0f, 1.0f) * static_cast<float>(c->height - 24));
      api->outline(c, x, 2, bar_w, c->height - 22, O3M_COLOR_EMPTY);
      api->rect(c, x + 2, c->height - 20 - h, bar_w - 4, h, O3M_COLOR_INSTR);
      api->text(c, x, c->height - 14, kNames[i], O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
    }
  }
}

// The dearest views the budgets allow: view 0 a curve of 128 points with axes and fill, view 1 a polyline of 100 points, 40 characters and 15 rectangles (together
// within the page's words).
void stress_view(void*, std::uint32_t view, o3m_canvas* c) {
  const o3m_draw_api* api = c->draw;
  if (view == 0u) {
    float ys[128];
    for (unsigned i = 0; i < 128; ++i) {
      ys[i] = 0.5f + 0.4f * static_cast<float>((i * 7) % 13) / 13.0f;
    }
    const o3m_curve cv = {nullptr, ys, 128, O3M_AXIS_LINEAR, O3M_AXIS_LINEAR, 0.0f, 1.0f, 0.0f, 1.0f, O3M_CURVE_AXES | O3M_CURVE_FILL, O3M_COLOR_FX};
    api->curve(c, &cv);
  } else {
    std::int16_t xy[200];
    for (int i = 0; i < 100; ++i) {
      xy[2 * i] = static_cast<std::int16_t>(i * c->width / 100);
      xy[2 * i + 1] = static_cast<std::int16_t>(c->height / 2 + ((i * 5) % 17) - 8);
    }
    api->polyline(c, xy, 100, O3M_COLOR_VOL);
    api->text(c, 2, 2, "THE DEAREST VIEW THE BUDGETS ALLOW A MODULE", O3M_FACE_LABEL, O3M_COLOR_TEXT);
    for (int i = 0; i < 15; ++i) {
      api->rect(c, 4 + i * 6, c->height - 12, 4, 8, O3M_COLOR_ACCENT);
    }
  }
}

struct ScopeState {
  std::uint32_t events;
};
void scope_input(void* s, const o3m_ui_event*) { ++static_cast<ScopeState*>(s)->events; }
void scope_enter(void*) {}
void scope_exit(void*) {}
// A scope of two sine waves from the parameters, in the view; the event count as text.
void scope_view(void* s, std::uint32_t, o3m_canvas* c) {
  const o3m_draw_api* api = c->draw;
  const ScopeState* st = static_cast<const ScopeState*>(s);
  api->rect(c, 0, 0, c->width, c->height, O3M_COLOR_PANE);
  api->outline(c, 0, 0, c->width, c->height, O3M_COLOR_EMPTY);
  std::int16_t xy[128];
  const float cycles = 1.0f + c->params[0] * 0.1f;
  const float amp = 0.1f + c->params[1] * 0.004f;
  for (int i = 0; i < 64; ++i) {
    // a sine from a parabola (no libm): 4x(1 - |x|) of x = 2p - 1, p the phase in cycles
    float p = cycles * static_cast<float>(i) / 63.0f;
    p -= static_cast<float>(static_cast<int>(p));
    const float x = 2.0f * p - 1.0f;
    const float y = 4.0f * x * (1.0f - (x < 0.0f ? -x : x));
    xy[2 * i] = static_cast<std::int16_t>(i * (c->width - 1) / 63);
    xy[2 * i + 1] = static_cast<std::int16_t>(static_cast<float>(c->height) * 0.5f - y * amp * static_cast<float>(c->height) * 0.5f);
  }
  api->polyline(c, xy, 64, O3M_COLOR_PLAY);
  char text[24] = "EVENTS ";
  unsigned n = 7;
  unsigned v = st->events;
  char digits[10];
  unsigned k = 0;
  do {
    digits[k++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u && k < 9u);
  while (k != 0u) {
    text[n++] = digits[--k];
  }
  text[n] = '\0';
  api->text(c, 6, 6, text, O3M_FACE_DATA, O3M_COLOR_TEXT);
}

// ABI 1.8 (OS-INSTUI S10): the page table and the value labels. Ten parameters on two named pages; Wave is an INT with four labels.
const char* const kWaveLabels[4] = {"PULSE", "TRIANGLE", "NOISE LONG", "NOISE SHORT"};
const o3m_param kNamedParams[10] = {
    {"Wave", 0.0f, 3.0f, 1.0f, kInt},
    {"Duty", 0.0f, 3.0f, 1.0f, kInt},
    {"Volume", -48.0f, 6.0f, -12.0f, unit(O3M_UNIT_DB)},
    {"Transpose", -24.0f, 24.0f, 0.0f, kInt | unit(O3M_UNIT_SEMITONES)},
    {"Fine", -100.0f, 100.0f, 0.0f, kInt | unit(O3M_UNIT_CENTS)},
    {"Vel Sens", 0.0f, 100.0f, 50.0f, unit(O3M_UNIT_PERCENT)},
    {"Attack", 0.0f, 500.0f, 0.0f, unit(O3M_UNIT_MS)},
    {"Decay", 0.0f, 2000.0f, 250.0f, unit(O3M_UNIT_MS)},
    {"Sustain", 0.0f, 100.0f, 60.0f, unit(O3M_UNIT_PERCENT)},
    {"Release", 0.0f, 2000.0f, 80.0f, unit(O3M_UNIT_MS)},
};
const o3m_page kNamedPages[2] = {{"TONE", 0}, {"ENV", 6}};
const o3m_labels kNamedLabels[10] = {{kWaveLabels, 4}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}};

const o3m_ui kDemoUi = {sizeof(o3m_ui), nullptr, nullptr, demo_view, nullptr, nullptr};
const o3m_ui kStressUi = {sizeof(o3m_ui), nullptr, nullptr, stress_view, nullptr, nullptr};
const o3m_ui kScopeUi = {sizeof(o3m_ui), scope_enter, scope_input, scope_view, scope_exit, nullptr};
const o3m_param kScopeParams[2] = {{"Cycles", 0.0f, 100.0f, 30.0f, kInt | unit(O3M_UNIT_PERCENT)}, {"Gain", 0.0f, 100.0f, 60.0f, unit(O3M_UNIT_PERCENT)}};
const o3m_module kDemo = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "Demo Synth", 64, 12, kDemoParams,
                          nop_init, nop_render, nop_set_param, nop_note_on, nop_note_off, nop_shutdown, &kDemoUi, 2};
const o3m_module kNamed = {O3M_ABI_MAJOR, 8, O3M_KIND_INSTRUMENT, O3M_MODULE_SIZE(8), "Named Synth", 64, 10, kNamedParams,
                           nop_init, nop_render, nop_set_param, nop_note_on, nop_note_off, nop_shutdown, nullptr, 0, nullptr, kNamedPages, 2, kNamedLabels};
const o3m_module kStress = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "Stress", 64, 12, kDemoParams,
                            nop_init, nop_render, nop_set_param, nop_note_on, nop_note_off, nop_shutdown, &kStressUi, 2};
// The gallery's app is an ABI 1.9 app that declares two rows: its page is the OS parameter column (an app of 1.10 draws its own rows: OS-APPUX S3), so the pictures stay what they were.
const o3m_module kScope = {O3M_ABI_MAJOR, 9, O3M_KIND_APP, O3M_MODULE_SIZE(9), "Scope", sizeof(ScopeState), 2, kScopeParams,
                           nop_init, nullptr, nullptr, nullptr, nullptr, nop_shutdown, &kScopeUi, 1};

// ---- the world of a state ---------------------------------------------------------------------------------------------------------------------------

std::uint32_t slow_clock(void* ctx) {
  std::uint32_t& t = *static_cast<std::uint32_t*>(ctx);
  t += 6000;  // every call 6 ms later: any timed call goes over the 4 ms budget
  return t;
}

struct World {
  platform::host::FakeStorage card;
  module::ModuleSet set;
  module::ModuleLoader loader;
  audio::ModuleVoices voices;
  app::ModuleUi ui;
  alignas(64) std::uint8_t scratch[audio::ModuleVoices::kScratchBytes] = {};
  alignas(64) std::uint8_t arena[8192] = {};
  std::uint32_t fake_us = 0;

  explicit World(bool with_card) {
    (void)voices.attach_scratch(scratch, sizeof scratch);
    (void)set.cold().init(arena, 0x70000000u, sizeof arena);
    ui.attach(set, loader, voices);
    if (with_card) {
      card.add_dir("modules");
      card.add_dir("modules/instruments");
      card.add_dir("modules/apps");
      for (const char* name : {"demo", "stress", "tb303"}) {
        card.add_file(std::string("modules/instruments/") + name + ".tmod", 7380);
      }
      for (const char* name : {"scope", "clock", "broken"}) {
        card.add_file(std::string("modules/apps/") + name + ".tmod", 3000);
      }
      (void)card.mount();
      ui.set_storage(&card);
    }
    (void)set.add_native("demo", kDemo);
    (void)set.add_native("stress", kStress);
    (void)set.add_native("scope", kScope);
    (void)set.quarantine("broken", module::Status::BadCrc);
  }

  // Instruments 6 and 7 (slots 5 and 6) of the project are the demo and the stress module, bound.
  void bind_instruments(model::Project& p) {
    static const char* const kNames[2] = {"demo", "stress"};
    static const o3m_module* const kMods[2] = {&kDemo, &kStress};
    for (unsigned i = 0; i < 2; ++i) {
      (void)p.add_module(kNames[i], std::strlen(kNames[i]), O3M_ABI_MAJOR, O3M_ABI_MINOR);
      model::Instrument in;
      model::init_instrument(in, model::InstrumentKind::Module);
      in.module_index = static_cast<std::uint8_t>(i);
      std::strcpy(in.name, i == 0 ? "DEMO SYNTH" : "STRESS");
      for (unsigned k = 0; k < 12; ++k) {
        std::memcpy(in.params + 4u * k, &kDemoParams[k].def, 4);
      }
      in.param_len = 48;
      (void)p.set_instrument(5 + i, in);
    }
    if (!bound_) {
      bound_ = true;
      for (unsigned i = 0; i < 2; ++i) {
        std::uint8_t* mem = nullptr;
        std::uint32_t addr = 0;
        if (set.cold().take(64, 16, mem, addr)) {
          std::memset(mem, 0, 64);
          (void)voices.bind(5 + i, *kMods[i], module::fault_id(kNames[i]), mem, p.instrument(5 + i)->params, p.instrument(5 + i)->param_len);
        }
      }
    }
  }
  bool bound_ = false;
};

model::Project& project() {
  static model::ModelArena arena;
  return arena.project;
}

World& page_world() {
  static World w(true);
  return w;
}
World& empty_world() {
  static World w(false);
  return w;
}
World& app_world() {
  static World w(true);
  static bool launched = false;
  if (!launched) {
    launched = true;
    w.ui.scan_apps();  // broken, clock, scope (sorted): the scope is the third
    for (unsigned i = 0; i < w.ui.app_entry_count(); ++i) {
      if (std::strcmp(w.ui.app_entry_at(i)->stem, "scope") == 0) {
        (void)w.ui.launch_app(i);
      }
    }
    (void)w.ui.app_input(O3M_EV_PAD, 3, 100, false);
    (void)w.ui.app_input(O3M_EV_PAD, 3, 0, false);
    (void)w.ui.app_input(O3M_EV_ENCODER, 0, 2, false);
  }
  return w;
}
World& stopped_world() {
  static World w(true);
  static bool launched = false;
  if (!launched) {
    launched = true;
    w.ui.scan_apps();
    for (unsigned i = 0; i < w.ui.app_entry_count(); ++i) {
      if (std::strcmp(w.ui.app_entry_at(i)->stem, "scope") == 0) {
        (void)w.ui.launch_app(i);
      }
    }
    module::ViewHost::Hooks hooks;
    hooks.now_us = slow_clock;
    hooks.ctx = &w.fake_us;
    w.ui.views().set_hooks(hooks);
    (void)w.ui.app_input(O3M_EV_PAD, 3, 100, false);  // over its time: stopped, quarantined
  }
  return w;
}

Model make_model(World& w, bool with_project) {
  Model m{};
  m.project = with_project ? &project() : nullptr;
  m.device.init(core::ParamKind::Device);
  m.modules = &w.ui;
  m.card = &w.card;
  return m;
}

ViewState instrument_view(int instrument, unsigned page, int row, OverlayId overlay) {
  ViewState v;
  v.screen = ScreenId::Instrument;
  v.instrument = instrument;
  v.page[app::screen_index(ScreenId::Instrument)] = static_cast<std::uint8_t>(page);
  v.params.row = row;
  v.overlay = overlay;
  return v;
}

void draw_module_page(ui::Renderer& r, int instrument, unsigned page, int row, OverlayId overlay, bool scan) {
  World& w = page_world();
  fill_demo_project(project());
  w.bind_instruments(project());
  if (scan) {
    w.ui.scan_instruments(&project());
  }
  ViewState v = instrument_view(instrument, page, row, overlay);
  v.modules.picker = 1;
  const Model m = make_model(w, true);
  app::render_scene(m, v, r);
}

// A world of its own (the modules above and their goldens are untouched): "Named Synth" bound to instrument 6, on its first named page (TONE).
void draw_named(ui::Renderer& r, unsigned) {
  static World w(false);
  static bool bound = false;
  model::Project& p = project();
  fill_demo_project(p);
  if (!bound) {
    bound = true;
    (void)w.set.add_native("named", kNamed);
  }
  (void)p.add_module("named", 5, O3M_ABI_MAJOR, 8);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Module);
  in.module_index = 0;
  std::strcpy(in.name, "NAMED SYNTH");
  for (unsigned k = 0; k < 10; ++k) {
    std::memcpy(in.params + 4u * k, &kNamedParams[k].def, 4);
  }
  in.param_len = 40;
  (void)p.set_instrument(5, in);
  static std::uint8_t* mem = nullptr;
  if (mem == nullptr) {
    std::uint32_t addr = 0;
    if (w.set.cold().take(64, 16, mem, addr)) {
      std::memset(mem, 0, 64);
      (void)w.voices.bind(5, kNamed, module::fault_id("named"), mem, p.instrument(5)->params, p.instrument(5)->param_len);
    }
  }
  const Model m = make_model(w, true);
  app::render_scene(m, instrument_view(6, 0, 0, OverlayId::None), r);
}

void draw_page(ui::Renderer& r, unsigned) { draw_module_page(r, 6, 0, 1, OverlayId::None, false); }
void draw_page_two(ui::Renderer& r, unsigned) { draw_module_page(r, 6, 1, 0, OverlayId::None, false); }
void draw_stress(ui::Renderer& r, unsigned) { draw_module_page(r, 7, 0, 5, OverlayId::None, false); }
void draw_picker(ui::Renderer& r, unsigned) { draw_module_page(r, 6, 0, 1, OverlayId::ModulePicker, true); }

void draw_list(ui::Renderer& r, unsigned) {
  World& w = page_world();
  w.ui.scan_apps();
  ViewState v;
  v.screen = ScreenId::Modules;
  v.modules.apps = 2;
  const Model m = make_model(w, true);
  app::render_scene(m, v, r);
}

void draw_list_empty(ui::Renderer& r, unsigned) {
  World& w = empty_world();
  w.ui.scan_apps();
  ViewState v;
  v.screen = ScreenId::Modules;
  const Model m = make_model(w, true);
  app::render_scene(m, v, r);
}

void draw_app(ui::Renderer& r, unsigned) {
  World& w = app_world();
  ViewState v;
  v.screen = ScreenId::ModuleApp;
  const Model m = make_model(w, true);
  app::render_scene(m, v, r);
}

void draw_app_stopped(ui::Renderer& r, unsigned) {
  World& w = stopped_world();
  ViewState v;
  v.screen = ScreenId::ModuleApp;
  v.modules.app_row = 1;
  const Model m = make_model(w, true);
  app::render_scene(m, v, r);
}

}  // namespace

const ScreenState kModulesStates[9] = {
    {"page", false, 1, 0, draw_page},
    {"page_two", false, 1, 0, draw_page_two},
    {"stress", true, 1, kBudgetWords, draw_stress},
    {"picker", false, 1, 0, draw_picker},
    {"list", false, 1, 0, draw_list},
    {"list_empty", false, 1, 0, draw_list_empty},
    {"app", false, 1, 0, draw_app},
    {"app_stopped", false, 1, 0, draw_app_stopped},
    {"named", false, 1, 0, draw_named},  // OS-INSTUI S10 (ABI 1.8): a module that names its pages and the values of an INT parameter (the demo and stress modules above are legacy: "1-8")
};

}  // namespace o3os::sim
