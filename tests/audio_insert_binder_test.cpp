// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX S3b (docs/specs/OS-INSFX.md, section 7): the insert phase of the module binder (app/module_binder.h): the project's INSX records are bound after the instruments, unadmitted, with their
// parameters and bypass flag; the master's record (OS-INSFX S6) binds TWO instances, so it counts 2 of the cap of four; a module that is missing or not an effect, a record that does not fit and a bind that fails
// are skipped and counted and the run still ends Done; a project without records binds none
// after one that had some (the platform retires the old ones at the start of a load, begin_replace, and releases them in the Settle phase; the test does the same calls); the model written as the
// console writes it (the Editor's set_insert / clear_insert) is saved and loaded back and binds the same inserts. Host only; the module code is native, written here.
#include <doctest/doctest.h>

#include "support/memory_io.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/static_slot.h"

#include <o3os/app/module_binder.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/insert_host.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/taps.h>
#include <o3os/formats/native.h>
#include <o3os/hal/audio.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>
#include <o3os/model/undo.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/dir_storage.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using namespace o3os;
namespace fs = std::filesystem;
namespace ha = o3os::hal::audio;

// ---- native modules: two effects and an instrument --------------------------------------------------------------------------------------
struct alignas(16) FxState {
  float p[2];
  std::uint32_t pad[2];
};
int fx_init(void* s, const o3m_host*) {
  FxState* d = static_cast<FxState*>(s);
  d->p[0] = 1.0f;
  d->p[1] = 0.0f;
  return 0;
}
int fx_refuse(void*, const o3m_host*) { return 1; }
void fx_shutdown(void*) {}
void fx_set_param(void* s, std::uint32_t id, float v) {
  if (id < 2u) {
    static_cast<FxState*>(s)->p[id] = v;
  }
}
std::uint32_t fx_render(void* s, const float* in, float* out, std::uint32_t n) {
  const FxState* d = static_cast<const FxState*>(s);
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = in[i] * d->p[0];
  }
  return 1;
}
void fx_note_on(void*, std::uint32_t, std::uint32_t, std::uint32_t) {}
void fx_note_off(void*, std::uint32_t) {}
const o3m_param kParams[2] = {{"Gain", 0.0f, 4.0f, 1.0f, 0}, {"Mix", 0.0f, 4.0f, 0.0f, 0}};
#define FX(name, kind, init) \
  o3m_module { O3M_ABI_MAJOR, O3M_ABI_MINOR, kind, sizeof(o3m_module), name, sizeof(FxState), 2, kParams, init, fx_render, fx_set_param, fx_note_on, fx_note_off, fx_shutdown, nullptr, 0 }
const o3m_module kGain = FX("gain", O3M_KIND_EFFECT, fx_init);
const o3m_module kWide = FX("wide", O3M_KIND_EFFECT, fx_init);
const o3m_module kStubborn = FX("stubborn", O3M_KIND_EFFECT, fx_refuse);  // its init() refuses
const o3m_module kInst = FX("inst", O3M_KIND_INSTRUMENT, fx_init);
// OS-INSFX-ABI2 A7: a 1.12 stereo effect (render_st): on the master it binds ONE instance.
std::uint32_t fx_st_render(void*, const float* il, const float* ir, float* ol, float* orr, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    ol[i] = il[i];
    orr[i] = ir[i];
  }
  return 1;
}
#define FX_ST(name, render_st) \
  o3m_module { O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), name, sizeof(FxState), 2, kParams, fx_init, fx_render, fx_set_param, fx_note_on, fx_note_off, fx_shutdown, nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr, render_st, 0u }
const o3m_module kStWide = FX_ST("stwide", fx_st_render);

bool route_insert(void* self, const core::ParamChange& c) noexcept {
  return core::param_kind(c.id) == core::ParamKind::Insert && static_cast<audio::InsertHost*>(self)->apply_param(c);
}

// The graph, the module voices and the insert host: the render's side, driven by block().
struct Rig {
  audio::Graph* g = nullptr;
  audio::ModuleVoices* mv = nullptr;
  audio::InsertHost* h = nullptr;
  audio::InsertHost::Blobs blobs;
  alignas(8) std::int16_t in[ha::kBlockSamples] = {};
  alignas(8) std::int16_t out[ha::kBlockSamples] = {};
  ha::BlockReport report{};

  Rig() {
    static test::StaticSlot<audio::Graph> gslot;
    static test::StaticSlot<audio::ModuleVoices> mslot;
    static test::StaticSlot<audio::InsertHost> hslot;
    static audio::TapMem taps;
    g = &gslot.make(+[]() noexcept -> std::uint32_t { return 0u; }, taps);
    mv = &mslot.make();
    h = &hslot.make();
    std::memset(blobs, 0, sizeof blobs);
    h->attach(*g, *mv, &blobs);
    g->set_param_hook(&route_insert, h);
  }
  void block() {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0, &report};
    g->render(io);
    h->begin_block();
    h->service();
  }
  // What begin_replace and the Settle phase do (the render here is driven by the test): every insert leaves, the render passes its boundaries, the host releases what is left.
  void retire_and_release() {
    h->retire_all();
    for (unsigned i = 0; i < 64u && !h->settled(); ++i) {
      block();
    }
    CHECK(h->settled());
    h->release_all();
  }
  // The instance that holds `where` (track * 2 + slot), or -1.
  int inst(unsigned where) const {
    for (unsigned k = 0; k < audio::InsertHost::kInstances; ++k) {
      audio::InsertHost::Info in_info{};
      if (h->info(k, in_info) && in_info.where == where) {
        return static_cast<int>(k);
      }
    }
    return -1;
  }
  unsigned count() const {
    unsigned n = 0;
    for (unsigned k = 0; k < audio::InsertHost::kInstances; ++k) {
      audio::InsertHost::Info in_info{};
      n += h->info(k, in_info) ? 1u : 0u;
    }
    return n;
  }
  bool bypassed(unsigned where) const {
    const int k = inst(where);
    audio::InsertHost::Info in_info{};
    return k >= 0 && h->info(static_cast<unsigned>(k), in_info) && in_info.user_bypass;
  }
};

model::InsertRecord record(unsigned where, unsigned slot, unsigned module, bool bypass, std::initializer_list<float> v) {
  model::InsertRecord r;
  std::memset(&r, 0, sizeof r);
  r.where = static_cast<std::uint8_t>(where);
  r.slot = static_cast<std::uint8_t>(slot);
  r.module = static_cast<std::uint8_t>(module);
  r.flags = bypass ? model::kInsertFlagBypass : 0u;
  r.n = static_cast<std::uint8_t>(v.size());
  unsigned i = 0;
  for (float f : v) {
    r.param[i++] = f;
  }
  return r;
}

// A card with an empty modules/effects, the module set with the natives and the loader.
struct Card {
  fs::path root;
  platform::host::DirStorage dir;
  explicit Card(const char* tag) : root(fs::temp_directory_path() / (std::string("o3m_insbind_") + tag)), dir((fs::remove_all(root), fs::create_directories(root / "modules" / "effects"), root.string())) {
    CHECK(dir.mount() == hal::storage::Status::Ok);
  }
  ~Card() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

alignas(64) std::uint8_t g_slab[1024];
alignas(64) std::uint8_t g_sdram[16384];
module::ModuleSet g_set;
module::ModuleLoader g_loader;

void fresh_set() {
  g_set.clear();
  std::memset(g_sdram, 0, sizeof g_sdram);
  g_set.hot().init(g_slab, 0x1FFFC000u, sizeof g_slab);
  g_set.cold().init(g_sdram, 0x70000000u, sizeof g_sdram);
  CHECK(g_set.add_native("gain", kGain));
  CHECK(g_set.add_native("wide", kWide));
  CHECK(g_set.add_native("stwide", kStWide));  // OS-INSFX-ABI2 A7: a stereo effect, native only
  CHECK(g_set.add_native("stubborn", kStubborn));
  CHECK(g_set.add_native("inst", kInst));
}

// Runs the binder to the end; the number of steps is bounded.
bool run(app::ModuleBinder& b, Card& card, const model::Project& p, Rig& r, bool with_inserts = true) {
  if (!b.begin(card.dir, p, g_set, g_loader, *r.mv, nullptr, with_inserts ? r.h : nullptr)) {
    return false;
  }
  for (unsigned n = 0; n < 1000u && b.state() == app::ModuleBinder::State::Running; ++n) {
    (void)b.step(256);
  }
  return b.state() == app::ModuleBinder::State::Done;
}

model::Project& project() {
  static model::Project p;
  p.reset();
  return p;
}

}  // namespace

TEST_CASE("insert binder: the project's inserts are bound with their parameters and bypass flag; the master's record needs two instances, so with three track inserts it does not fit: skipped and counted") {
  Card card("bind");
  fresh_set();
  Rig r;
  model::Project& p = project();
  NEED(p.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(p.add_module("wide", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(p.set_insert(record(0, 0, 0, false, {0.5f, 2.5f})) == model::Status::Ok);
  NEED(p.set_insert(record(3, 1, 1, true, {3.0f})) == model::Status::Ok);
  NEED(p.set_insert(record(15, 0, 0, false, {})) == model::Status::Ok);
  NEED(p.set_insert(record(model::kInsertWhereMaster, 0, 1, false, {1.0f})) == model::Status::Ok);
  CHECK(p.inserts().count == 4);

  app::ModuleBinder b;
  NEED(run(b, card, p, r));
  const app::ModuleReport& rep = b.report();
  CHECK(rep.inserts == 4);
  CHECK(rep.inserts_bound == 3);
  CHECK(rep.inserts_skipped == 1);
  CHECK(rep.inserts_master == 1);  // (skipped: the cap counts instances, and the three track inserts are three of the four)
  CHECK(rep.problem_count == 1);
  CHECK(r.count() == 3);
  CHECK_FALSE(r.h->bound(audio::InsertHost::kMasterWhere));
  CHECK(r.h->bound(0));          // track 1, INS A
  CHECK(r.h->bound(3 * 2 + 1));  // track 4, INS B
  CHECK(r.h->bound(15 * 2));     // track 16, INS A
  CHECK(!r.h->bound(1));
  // the stored parameters reached the instance's copy (the host's own blob), and the bypass flag is the user's
  const int a = r.inst(0);
  const int w = r.inst(7);
  NEED(a >= 0);
  NEED(w >= 0);
  CHECK(r.blobs[a][0] == 0.5f);
  CHECK(r.blobs[a][1] == 2.5f);
  CHECK(r.blobs[w][0] == 3.0f);
  CHECK_FALSE(r.bypassed(0));
  CHECK(r.bypassed(7));
  CHECK_FALSE(r.bypassed(30));
  // the render takes them: the activations are applied at the boundaries (the strips are silent here, so the ramp-in of a transition block does not run: In, not yet On)
  for (unsigned i = 0; i < 4; ++i) {
    r.block();
  }
  audio::InsertHost::Info info{};
  NEED(r.h->info(static_cast<unsigned>(a), info));
  CHECK(info.phase != audio::InsertHost::Phase::Off);
  NEED(r.h->info(static_cast<unsigned>(w), info));
  CHECK(info.phase != audio::InsertHost::Phase::Off);
  r.retire_and_release();
}

TEST_CASE("insert binder: a project without INSX loaded after one with inserts binds none") {
  Card card("leak");
  fresh_set();
  Rig r;
  model::Project& p = project();
  NEED(p.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(p.set_insert(record(1, 0, 0, false, {2.0f})) == model::Status::Ok);
  NEED(p.set_insert(record(2, 1, 0, true, {})) == model::Status::Ok);
  app::ModuleBinder b;
  NEED(run(b, card, p, r));
  CHECK(r.count() == 2);
  for (unsigned i = 0; i < 4; ++i) {
    r.block();
  }
  // the next project: the platform retires every insert at the start of the load and releases them in Settle; the module set is cleared, the new project's modules are loaded again
  r.retire_and_release();
  fresh_set();
  CHECK(r.count() == 0);
  model::Project& q = project();  // no inserts (a file with no INSX chunk, or count 0)
  NEED(q.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  CHECK(q.inserts().count == 0);
  NEED(run(b, card, q, r));
  CHECK(b.report().inserts == 0);
  CHECK(b.report().inserts_bound == 0);
  CHECK(r.count() == 0);
  for (unsigned k = 0; k < audio::InsertHost::kWhere; ++k) {
    CHECK_FALSE(r.h->bound(k));
  }
}

TEST_CASE("insert binder: a missing module, one that is not an effect and a refused init skip their records and the load goes on") {
  Card card("skip");
  fresh_set();
  Rig r;
  model::Project& p = project();
  NEED(p.add_module("ghost", 5, 1, 0) == model::Status::Ok);      // 0: on no card, built in nowhere
  NEED(p.add_module("inst", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);  // 1: an instrument
  NEED(p.add_module("stubborn", 8, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);  // 2: init() refuses
  NEED(p.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);  // 3
  NEED(p.set_insert(record(0, 0, 0, false, {})) == model::Status::Ok);
  NEED(p.set_insert(record(1, 0, 1, false, {})) == model::Status::Ok);
  NEED(p.set_insert(record(2, 0, 2, false, {})) == model::Status::Ok);
  NEED(p.set_insert(record(3, 0, 3, true, {0.25f})) == model::Status::Ok);

  app::ModuleBinder b;
  NEED(run(b, card, p, r));  // the run ends Done: an insert never fails a load
  const app::ModuleReport& rep = b.report();
  CHECK(rep.inserts == 4);
  CHECK(rep.inserts_bound == 1);
  CHECK(rep.inserts_skipped == 3);
  CHECK(r.count() == 1);
  CHECK(r.h->bound(6));
  CHECK(r.bypassed(6));
  CHECK(r.blobs[r.inst(6)][0] == 0.25f);
  bool missing = false;
  bool wrong = false;
  bool refused = false;
  for (unsigned i = 0; i < rep.problem_count; ++i) {
    const app::ModuleProblem& pr = rep.problems[i];
    char line[160];
    (void)app::describe_module_problem(pr, line, sizeof line);
    const std::string name = pr.name;
    if (name == "ghost") {
      missing = pr.kind == app::ModuleProblem::Kind::NotFound && pr.slot == 0xFF && pr.insert && std::string(line).find("modules/effects/ghost.tmod") != std::string::npos;
    } else if (name == "inst") {
      wrong = pr.kind == app::ModuleProblem::Kind::WrongKind && pr.insert && pr.slot == 1 && std::string(line).find("mod: insert track 2 slot 1 skipped: inst is not an effect") == 0 &&
              std::string(line).find("is not an effect") != std::string::npos;
    } else if (name == "stubborn") {
      refused = pr.kind == app::ModuleProblem::Kind::BindFailed && pr.insert && pr.ibind == audio::InsertHost::Bind::InitFailed;
    }
  }
  CHECK(missing);
  CHECK(wrong);
  CHECK(refused);
  char summary[200];
  (void)app::describe_modules(rep, summary, sizeof summary);
  CHECK(std::string(summary).find("; inserts: 4, 1 bound, 3 skipped") != std::string::npos);
  char own[200];
  (void)app::describe_inserts(rep, own, sizeof own);  // the log's own line: a log line holds 55 characters
  CHECK(std::string(own) == "inserts: 4, 1 bound, 3 skipped");
  CHECK(std::string(own).size() <= 55u);
  r.retire_and_release();
}

TEST_CASE("insert binder: four records (both slots of two tracks, one module in two of them) fill the host, each with its own state and values") {
  Card card("four");
  fresh_set();
  Rig r;
  model::Project& p = project();
  NEED(p.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);  // 0: in two records
  NEED(p.add_module("wide", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);  // 1: in two records
  NEED(p.set_insert(record(0, 0, 0, false, {0.5f})) == model::Status::Ok);
  NEED(p.set_insert(record(0, 1, 1, false, {1.5f})) == model::Status::Ok);
  NEED(p.set_insert(record(1, 0, 0, true, {2.5f})) == model::Status::Ok);
  NEED(p.set_insert(record(1, 1, 1, false, {3.5f})) == model::Status::Ok);
  NEED(p.inserts().count == model::kMaxInserts);

  app::ModuleBinder b;
  NEED(run(b, card, p, r));
  const app::ModuleReport& rep = b.report();
  CHECK(rep.inserts == 4);
  CHECK(rep.inserts_bound == 4);
  CHECK(rep.inserts_skipped == 0);
  CHECK(rep.problem_count == 0);
  CHECK(r.count() == 4);
  const float want[4] = {0.5f, 1.5f, 2.5f, 3.5f};
  const void* states[4] = {};
  for (unsigned w = 0; w < 4; ++w) {
    CHECK(r.h->bound(w));
    const int k = r.inst(w);
    NEED(k >= 0);
    audio::InsertHost::Info info{};
    NEED(r.h->info(static_cast<unsigned>(k), info));
    states[w] = info.state;
    CHECK(static_cast<const FxState*>(info.state)->p[0] == want[w]);
    CHECK(r.blobs[k][0] == want[w]);
  }
  CHECK(r.bypassed(2));
  CHECK_FALSE(r.bypassed(0));
  for (unsigned i = 0; i < 4; ++i) {  // the same module twice is two instances, never one state block shared
    for (unsigned j = i + 1; j < 4; ++j) {
      CHECK(states[i] != states[j]);
    }
  }
  // the host is full: a fifth bind has no instance
  CHECK(r.h->bind(8, kGain, module::fault_id("gain"), nullptr, nullptr, 0, true) == audio::InsertHost::Bind::NoInstance);
  r.retire_and_release();
}

TEST_CASE("insert binder: a record with more values than the module has parameters binds the ones it has; with fewer, the rest keep the module's defaults") {
  Card card("nparams");
  fresh_set();
  Rig r;
  model::Project& p = project();
  NEED(p.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(p.add_module("wide", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(p.set_insert(record(0, 0, 0, false, {0.5f, 2.5f, 9.0f, 9.0f, 9.0f})) == model::Status::Ok);  // n = 5 > param_count 2
  NEED(p.set_insert(record(1, 0, 1, false, {3.0f})) == model::Status::Ok);                          // n = 1 < param_count 2
  CHECK(p.inserts().rec[0].n == 5);
  CHECK(p.inserts().rec[1].n == 1);

  app::ModuleBinder b;
  NEED(run(b, card, p, r));
  const app::ModuleReport& rep = b.report();
  CHECK(rep.inserts_bound == 2);
  CHECK(rep.inserts_skipped == 0);
  CHECK(rep.problem_count == 0);
  const int more = r.inst(0);
  const int fewer = r.inst(2);
  NEED(more >= 0);
  NEED(fewer >= 0);
  audio::InsertHost::Info info{};
  NEED(r.h->info(static_cast<unsigned>(more), info));
  CHECK(static_cast<const FxState*>(info.state)->p[0] == 0.5f);
  CHECK(static_cast<const FxState*>(info.state)->p[1] == 2.5f);
  CHECK(r.blobs[more][0] == 0.5f);
  CHECK(r.blobs[more][1] == 2.5f);
  CHECK(r.blobs[more][2] == 0.0f);  // the values past the module's parameters went nowhere
  NEED(r.h->info(static_cast<unsigned>(fewer), info));
  CHECK(static_cast<const FxState*>(info.state)->p[0] == 3.0f);
  CHECK(static_cast<const FxState*>(info.state)->p[1] == 0.0f);  // fx_init's default
  CHECK(r.blobs[fewer][1] == 0.0f);
  r.retire_and_release();
}

TEST_CASE("insert binder: the binder does not own the inserts unless it is given the host") {
  Card card("nohost");
  fresh_set();
  Rig r;
  model::Project& p = project();
  NEED(p.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(p.set_insert(record(0, 0, 0, false, {})) == model::Status::Ok);
  app::ModuleBinder b;
  NEED(run(b, card, p, r, false));
  CHECK(b.report().inserts == 0);
  CHECK(r.count() == 0);
}

TEST_CASE("insert binder: a console bind, unbind and bypass written through the Editor are saved, loaded and bound again") {
  alignas(8) static std::uint8_t ring[64 * 1024];
  Card card("console");
  fresh_set();
  Rig r;
  model::Project& p = test::arena(0).project;
  p.reset();
  model::UndoJournal journal;
  NEED(journal.begin(ring, sizeof ring));
  model::Editor ed(p, journal);
  CHECK(ed.create_pattern(1, 16) == model::Status::Ok);
  CHECK(ed.can_undo());
  // `ins bind 3 2 gain`: the module joins MODR, the record holds the module's defaults; `ins bind 1 1 wide`; `ins bypass 3 2 on`
  NEED(ed.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(ed.add_module("wide", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(ed.set_insert(record(2, 1, 0, false, {1.0f, 0.0f})) == model::Status::Ok);
  NEED(ed.set_insert(record(0, 0, 1, false, {1.0f, 0.0f})) == model::Status::Ok);
  const model::InsertRecord* cur = p.find_insert(2, 1);
  NEED(cur != nullptr);
  model::InsertRecord next = *cur;
  next.flags = model::kInsertFlagBypass;
  NEED(ed.set_insert(next) == model::Status::Ok);
  CHECK(ed.can_undo());  // not an undo step (D9), and the history of the pattern is still in sync
  CHECK(ed.undo() == model::UndoResult::Ok);
  CHECK(p.pattern(1) == nullptr);
  CHECK(p.inserts().count == 2);  // (the undo took the pattern, not the inserts)
  // ... and an unbind that is written back as well
  NEED(ed.set_insert(record(15, 1, 0, false, {})) == model::Status::Ok);
  CHECK(ed.clear_insert(15, 1) == model::Status::Ok);
  CHECK(ed.clear_insert(15, 1) == model::Status::NotFound);
  CHECK(p.inserts().count == 2);

  std::vector<std::uint8_t> file;
  NEED(test::save(p, file) == formats::native::Progress::Done);
  formats::native::Report rep;
  NEED(test::load(test::arena(1), file, &rep) == formats::native::Progress::Done);
  model::Project& q = test::arena(1).project;
  CHECK(rep.settings_skipped == 0u);
  CHECK(q.inserts().count == 2);
  CHECK(std::memcmp(&q.inserts(), &p.inserts(), sizeof(model::InsertState)) == 0);

  app::ModuleBinder b;
  NEED(run(b, card, q, r));
  CHECK(b.report().inserts_bound == 2);
  CHECK(r.h->bound(0));
  CHECK(r.h->bound(2 * 2 + 1));
  CHECK(r.bypassed(5));
  CHECK_FALSE(r.bypassed(0));
  CHECK(r.blobs[r.inst(0)][0] == 1.0f);
  r.retire_and_release();
}

// OS-INSFX S6: the master's record is bound as TWO instances of its module (dual mono: L and R, each with its own state block and the stored parameters), and the pair counts 2 of the cap.
TEST_CASE("insert binder: the master record binds two instances with its parameters and bypass flag, and two more track inserts fill the cap") {
  Card card("master");
  fresh_set();
  Rig r;
  model::Project& p = project();
  NEED(p.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(p.set_insert(record(0, 0, 0, false, {0.5f})) == model::Status::Ok);
  NEED(p.set_insert(record(5, 1, 0, false, {})) == model::Status::Ok);
  NEED(p.set_insert(record(model::kInsertWhereMaster, 0, 0, true, {0.25f, 1.5f})) == model::Status::Ok);
  CHECK(model::insert_instances(p.inserts()) == 4);

  app::ModuleBinder b;
  NEED(run(b, card, p, r));
  const app::ModuleReport& rep = b.report();
  CHECK(rep.inserts == 3);
  CHECK(rep.inserts_bound == 3);
  CHECK(rep.inserts_skipped == 0);
  CHECK(rep.inserts_master == 0);
  CHECK(rep.problem_count == 0);
  CHECK(r.count() == 4);  // two track inserts and the master's two instances
  CHECK(r.h->bound(audio::InsertHost::kMasterWhere));
  CHECK_FALSE(r.h->bound(audio::InsertHost::kMasterWhere + 1u));  // (the right instance is internal)
  const int left = r.inst(audio::InsertHost::kMasterWhere);
  const int right = r.inst(audio::InsertHost::kMasterWhere + 1u);
  NEED(left >= 0);
  NEED(right >= 0);
  CHECK(left != right);
  CHECK(r.blobs[left][0] == 0.25f);
  CHECK(r.blobs[left][1] == 1.5f);
  CHECK(r.blobs[right][0] == 0.25f);
  CHECK(r.blobs[right][1] == 1.5f);
  CHECK(r.bypassed(audio::InsertHost::kMasterWhere));  // the flag is the pair's
  CHECK(r.bypassed(audio::InsertHost::kMasterWhere + 1u));
  CHECK_FALSE(r.bypassed(0));
  for (unsigned i = 0; i < 6; ++i) {
    r.block();
  }
  audio::InsertHost::Info info{};
  NEED(r.h->info(static_cast<unsigned>(left), info));
  CHECK(info.phase == audio::InsertHost::Phase::Bypassed);
  NEED(r.h->info(static_cast<unsigned>(right), info));
  CHECK(info.phase == audio::InsertHost::Phase::Bypassed);
  CHECK(r.g->master_stage_active(audio::MasterStage::Insert) == false);  // both bypassed: the stage is out of the chain
  r.retire_and_release();
  CHECK(r.count() == 0);
  CHECK(r.g->master_stage_active(audio::MasterStage::Insert) == false);
}

TEST_CASE("insert binder: a master record alone, and the next project without INSX leaves nothing of it") {
  Card card("master_leak");
  fresh_set();
  Rig r;
  model::Project& p = project();
  NEED(p.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(p.set_insert(record(model::kInsertWhereMaster, 0, 0, false, {2.0f})) == model::Status::Ok);
  app::ModuleBinder b;
  NEED(run(b, card, p, r));
  CHECK(b.report().inserts_bound == 1);
  CHECK(r.count() == 2);
  for (unsigned i = 0; i < 4; ++i) {
    r.block();
  }
  CHECK(r.g->master_stage_active(audio::MasterStage::Insert));
  r.retire_and_release();
  fresh_set();
  CHECK(r.count() == 0);
  CHECK_FALSE(r.g->master_stage_active(audio::MasterStage::Insert));
  model::Project& q = project();
  NEED(q.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(run(b, card, q, r));
  CHECK(b.report().inserts_bound == 0);
  CHECK(r.count() == 0);
}

TEST_CASE("insert binder: a STEREO master record from a full project binds ONE instance beside three track inserts [OS-INSFX-ABI2 A7]") {
  Card card("stmaster");
  fresh_set();
  Rig r;
  model::Project& p = project();
  NEED(p.add_module("gain", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(p.add_module("stwide", 6, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
  NEED(p.set_insert(record(0, 0, 0, false, {0.5f})) == model::Status::Ok);
  NEED(p.set_insert(record(3, 1, 0, false, {})) == model::Status::Ok);
  NEED(p.set_insert(record(15, 0, 0, false, {})) == model::Status::Ok);
  NEED(p.set_insert(record(model::kInsertWhereMaster, 0, 1, true, {0.25f, 1.5f})) == model::Status::Ok);  // the stereo module on the master
  CHECK(p.inserts().count == 4);

  app::ModuleBinder b;
  NEED(run(b, card, p, r));
  const app::ModuleReport& rep = b.report();
  CHECK(rep.inserts == 4);
  CHECK(rep.inserts_bound == 4);  // the stereo master fits: ONE instance beside the three tracks (a mono one is skipped, the case above)
  CHECK(rep.inserts_skipped == 0);
  CHECK(rep.inserts_master == 0);
  CHECK(rep.problem_count == 0);
  CHECK(r.count() == 4);  // three track inserts + ONE master record
  CHECK(r.h->bound(audio::InsertHost::kMasterWhere));
  CHECK_FALSE(r.h->bound(audio::InsertHost::kMasterWhere + 1u));  // not the pair's second record
  const int m = r.inst(audio::InsertHost::kMasterWhere);
  NEED(m >= 0);
  CHECK(r.blobs[m][0] == 0.25f);
  CHECK(r.blobs[m][1] == 1.5f);
  CHECK(r.bypassed(audio::InsertHost::kMasterWhere));  // the record's bypass flag is the one instance's
  for (unsigned i = 0; i < 6; ++i) {
    r.block();
  }
  audio::InsertHost::Info info{};
  NEED(r.h->info(static_cast<unsigned>(m), info));
  CHECK(info.phase == audio::InsertHost::Phase::Bypassed);
  CHECK(r.g->master_stage_active(audio::MasterStage::Insert) == false);  // bypassed: the stage is out of the chain
  r.retire_and_release();
  CHECK(r.count() == 0);
  CHECK_FALSE(r.h->bound(audio::InsertHost::kMasterWhere));
}
