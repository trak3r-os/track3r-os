// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX S4 (docs/specs/OS-INSFX.md, sections 4 and 11): the Mixer's INSERTS page through the App with a real InsertHost behind the module system: the page is the fifth of the Mixer (F6), Insert opens
// the picker of modules/effects (the card's modules, the one the project names and the card lacks greyed), a pick binds the effect and writes the project's record, F7 bypasses (host and record), Delete
// removes both, and every refusal is a toast (INSERTS FULL, MODULE MISSING, DELETE THE INSERT FIRST, the master's). The cell reads (cost, forced) come from the host. Native effect modules; the card is a
// temporary directory. Host only. (The simulator has no insert host: the scenario qa_inserts.txt shows the records and the picker.)
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/project_fixture.h"
#include "support/static_slot.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/governor_watch.h>
#include <o3os/app/mixer_view.h>
#include <o3os/app/module_ui.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/insert_host.h>
#include <o3os/audio/module_governor.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/taps.h>
#include <o3os/core/input_event.h>
#include <o3os/core/param.h>
#include <o3os/hal/audio.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>
#include <o3os/model/undo.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/dir_storage.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

using namespace o3os;
using app::Button;
namespace fs = std::filesystem;

alignas(8) std::uint8_t g_ring[64 * 1024];

// ---- native modules: two effects and an instrument ---------------------------------------------------------------------------------------
struct alignas(16) FxState {
  float gain;
  std::uint32_t pad[3];
};
int fx_init(void* s, const o3m_host*) {
  static_cast<FxState*>(s)->gain = 1.0f;
  return 0;
}
void fx_shutdown(void*) {}
void fx_set_param(void* s, std::uint32_t id, float v) {
  if (id == 0u) {
    static_cast<FxState*>(s)->gain = v;
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

// The insert host's own clock: every call moves it by g_step, so one run of an effect measures exactly g_step cycles (0: the host's default clock, which never moves).
std::uint32_t g_ins_clock = 0;
std::uint32_t g_step = 0;
std::uint32_t ins_clock() noexcept { return g_ins_clock += g_step; }
namespace gps = audio::graph_param::strip;

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
    app::append_insert_status(s, (rec->flags & model::kInsertFlagBypass) != 0u, bound, live.forced, mods->ui.has_inserts(), live.cost);
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
};

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

TEST_CASE("mixer inserts: the refusals are toasts: the greyed module, INSERTS FULL, an instrument module, the master's row") {
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
  // the master's row is shown and not assignable (S6)
  rig.cell(12, 0);
  rig.tap(Button::Insert);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.last_note() == "MASTER INSERT: NOT YET");
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
  CHECK(text(ui.insert_assign(ed, 16, 0, "biquad")) == "NO SUCH INSERT");
  CHECK(text(ui.insert_assign(ed, 1, 2, "biquad")) == "NO SUCH INSERT");
  CHECK(rig.project().find_insert(1, 0) == nullptr);  // the refusals changed nothing
  CHECK(ui.insert_bypass(ed, 0, 0, true) == R::Ok);  // `ins bypass 1 1 on`
  CHECK((rig.project().find_insert(0, 0)->flags & model::kInsertFlagBypass) != 0u);
  CHECK(text(ui.insert_bypass(ed, 3, 0, true)) == "NO INSERT HERE");
  CHECK(ui.insert_remove(ed, 0, 0) == R::Ok);  // `ins unbind 1 1`
  CHECK(rig.project().find_insert(0, 0) == nullptr);
  CHECK(text(ui.insert_remove(ed, 0, 0)) == "NO INSERT HERE");
  CHECK(text(ui.insert_remove(ed, 16, 0)) == "NO SUCH INSERT");
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
