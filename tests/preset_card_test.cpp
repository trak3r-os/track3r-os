// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MODPRESET P3: ModuleUi::scan_presets and read_preset on the card (the list with its cap, the resolve by name, the refusals that touch nothing) and the round trip save -> list -> load on a module
// instrument and an insert (tests/preset_save_test.cpp has SaveService::save_preset). Host only: the card is the FAT32 volume in memory of tests/save_fatfs_test.cpp.
#include <doctest/doctest.h>

#include "../platform/mk66/tests/mk66_storage_fakes.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/module_ui.h>
#include <o3os/app/save_service.h>
#include <o3os/audio/module_voices.h>
#include <o3os/core/job.h>
#include <o3os/formats/crc32.h>
#include <o3os/formats/preset.h>
#include <o3os/hal/save_guard.h>
#include <o3os/hal/storage.h>
#include <o3os/model/editor.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/mk66/save/fat_storage_writer.h>
#include <o3os/platform/mk66/storage/fat_storage.h>
#include <o3os/sim/demo_project.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace app = o3os::app;
namespace hs = o3os::hal::storage;
namespace fp = o3os::formats::preset;
namespace st = o3os::platform::mk66::storage;
namespace sv = o3os::platform::mk66::save;
using fakes::Fat32Builder;
using fakes::Medium;
using namespace o3os;
using Bytes = std::vector<std::uint8_t>;

namespace {

// ---- a native test instrument module and effect module ----------------------------------------------------------------------------------------------------
struct PState {
  float v[8];
};
int p_init(void*, const o3m_host*) { return 0; }
std::uint32_t p_render(void*, const float*, float*, std::uint32_t) { return 0; }
void p_set_param(void* s, std::uint32_t id, float value) { static_cast<PState*>(s)->v[id < 8u ? id : 0u] = value; }
void p_note_on(void*, std::uint32_t, std::uint32_t, std::uint32_t) {}
void p_note_off(void*, std::uint32_t) {}
void p_shutdown(void*) {}

const o3m_param kTbParams[5] = {
    {"Cutoff", 0.0f, 1.0f, 0.5f, 0},
    {"Resonance", 0.0f, 1.0f, 0.2f, 0},
    {"Env Mod", 0.0f, 10.0f, 3.0f, O3M_PARAM_INT},
    {"Decay", 0.0f, 2.0f, 0.4f, O3M_PARAM_LOG},
    {"Filter Cutoff", 0.0f, 1.0f, 0.6f, 0},  // 13 characters: past core::kParamNameChars (12), so the page's caption cuts it
};
const o3m_module kTb303 = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "TB-303", sizeof(PState), 5, kTbParams,
                           p_init, p_render, p_set_param, p_note_on, p_note_off, p_shutdown, nullptr, 0};
const o3m_param kWideParams[2] = {
    {"Width", 0.0f, 1.0f, 0.0f, 0},
    {"Mix", 0.0f, 1.0f, 0.5f, 0},
};
const o3m_module kWidener = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), "WIDENER", sizeof(PState), 2, kWideParams,
                             p_init, p_render, p_set_param, p_note_on, p_note_off, p_shutdown, nullptr, 0};

// ---- the rig: the FatFs card behind the save service and the ModuleUi, a project with a bound tb303 instrument and a widener insert record ----
struct Rig {
  Medium& medium;
  st::FatStorage fat;
  sv::FatStorageWriter writer;
  app::SaveService saves;
  core::JobRunner jobs;
  platform::host::FakeClock clock;
  module::ModuleSet set;
  module::ModuleLoader loader;
  audio::ModuleVoices voices;
  app::ModuleUi ui;
  alignas(64) std::uint8_t scratch[audio::ModuleVoices::kScratchBytes] = {};
  alignas(64) std::uint8_t sdram[16384] = {};
  std::uint8_t ring[64 * 1024] = {};
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;

  explicit Rig(Medium& m) : medium(m), fat(m), writer(fat, m) {
    NEED(fat.mount() == hs::Status::Ok);
    saves.attach(fat, writer);
    (void)voices.attach_scratch(scratch, sizeof scratch);
    (void)set.cold().init(sdram, 0x70000000u, sizeof sdram);
    ui.attach(set, loader, voices);
    ui.set_storage(&fat);
    CHECK(set.add_native("tb303", kTb303));
    CHECK(set.add_native("widener", kWidener));
    model::Project& p = project();
    sim::fill_demo_project(p);
    CHECK(p.add_module("tb303", 5, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
    CHECK(p.add_module("widener", 7, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Module);
    in.module_index = 0;
    std::strcpy(in.name, "TB303");
    in.param_len = 4u * 5u;  // the whole blob
    for (unsigned i = 0; i < 5u; ++i) {
      std::memcpy(in.params + 4u * i, &kTbParams[i].def, 4u);
    }
    CHECK(p.set_instrument(0, in) == model::Status::Ok);
    const int idx = set.find("tb303");
    NEED(idx >= 0);
    const o3m_module& mod = *set.at(static_cast<unsigned>(idx))->entry;
    std::uint8_t* mem = nullptr;
    std::uint32_t addr = 0;
    NEED(set.cold().take(mod.state_bytes, 16, mem, addr));
    std::memset(mem, 0, mod.state_bytes);
    CHECK(voices.bind(0, mod, module::fault_id("tb303"), mem, p.instrument(0)->params, p.instrument(0)->param_len) == audio::ModuleVoices::Bind::Ok);
    model::InsertRecord rec{};
    rec.where = 2;
    rec.slot = 0;
    rec.module = 1;  // the widener
    rec.n = 2;
    rec.param[0] = 0.25f;
    rec.param[1] = 0.75f;
    CHECK(p.set_insert(rec) == model::Status::Ok);
    journal.begin(ring, sizeof ring);
    editor = std::make_unique<model::Editor>(p, journal);
    editor->history_reset();
  }

  // Steps the running jobs until none runs (the main loop's passes).
  void pump() {
    for (unsigned i = 0; i < 8192u && jobs.running() != 0u; ++i) {
      clock.advance(2000);
      jobs.run(clock, 2000);
    }
  }
  model::Project& project() { return test::arena(1).project; }
  const o3m_module* module_of(const char* stem) const {
    const int idx = set.find(stem);
    return idx >= 0 ? set.at(static_cast<unsigned>(idx))->entry : nullptr;
  }
};

bool exists(hs::Storage& s, const std::string& path) { return s.exists(path.c_str(), nullptr) == hs::Status::Ok; }

fp::Preset make_preset(const char* module, std::uint8_t kind, const std::pair<const char*, float>* entries, unsigned n) {
  fp::Preset p{};
  std::strcpy(p.module, module);
  p.kind = kind;
  p.module_version = 0x00010005u;
  p.count = static_cast<std::uint8_t>(n);
  for (unsigned i = 0; i < n; ++i) {
    std::strcpy(p.entries[i].name, entries[i].first);
    p.entries[i].value = entries[i].second;
  }
  return p;
}

Bytes preset_bytes(const fp::Preset& p) {
  Bytes b(fp::kMaxFileBytes, 0);
  const std::size_t n = fp::write(p, b.data(), b.size());
  b.resize(n);
  return b;
}

// The same file with the MPST major at 2 (and the CRC put right again): the reader's NewerMajor.
Bytes newer_major_bytes(const fp::Preset& p) {
  Bytes b = preset_bytes(p);
  b[20] = 0x02;  // the version's high byte: major 2
  const std::uint32_t crc = formats::crc32(b.data(), b.size() - 14u);  // every byte before the CRC chunk's header
  for (unsigned i = 0; i < 4u; ++i) {
    b[b.size() - 4u + i] = static_cast<std::uint8_t>(crc >> (24u - 8u * i));
  }
  return b;
}

core::ParamId mp(unsigned slot, unsigned index) { return core::param_id(core::ParamKind::Instrument, slot, model::instrument_param::kModuleFirst + index); }

std::string listed(const app::ModuleUi& ui, unsigned i) {
  const char* p = ui.card_preset_at(i);
  return p != nullptr ? std::string(p) : std::string();
}

bool same_set(const app::ParamSet& a, const app::ParamSet& b) {
  if (a.source != b.source || a.mask != b.mask || std::strcmp(a.name, b.name) != 0) {
    return false;
  }
  for (unsigned i = 0; i < app::kParamSetValues; ++i) {
    if (a.values[i] != b.values[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("preset card: scan_presets lists the folder's .o3mp sorted with the 128 cap, and read_preset resolves by name, counts unknown names and refuses the bad files without touching the set") {
  Medium medium{200'000};
  Fat32Builder fat(medium);
  const std::uint32_t mods = fat.make_dir(Fat32Builder::kRootCluster, "modules");
  const std::uint32_t pres = fat.make_dir(mods, "presets");
  const std::uint32_t tb = fat.make_dir(pres, "tb303");
  const std::uint32_t sid = fat.make_dir(pres, "sid");
  const std::pair<const char*, float> ents[4] = {{"Cutoff", 0.9f}, {"Resonance", 0.1f}, {"Env Mod", 7.0f}, {"Decay", 1.5f}};
  fat.add_file(tb, "Acid.o3mp", preset_bytes(make_preset("tb303", fp::kKindInstrument, ents, 4)));
  // Case-insensitive names, a clamped value, an INT-rounded one and an unknown name.
  const std::pair<const char*, float> ents2[4] = {{"cutoff", 5.0f}, {"RESONANCE", -3.0f}, {"No Such", 1.0f}, {"env mod", 6.4f}};
  fat.add_file(tb, "b.o3mp", preset_bytes(make_preset("tb303", fp::kKindInstrument, ents2, 4)));
  fat.add_file(tb, "Other.o3mp", preset_bytes(make_preset("sid", fp::kKindInstrument, ents, 1)));         // another module
  fat.add_file(tb, "Fx.o3mp", preset_bytes(make_preset("tb303", fp::kKindEffect, ents, 1)));             // another kind
  Bytes damaged = preset_bytes(make_preset("tb303", fp::kKindInstrument, ents, 2));
  damaged[damaged.size() - 1] ^= 0x01;  // the CRC off by one bit
  fat.add_file(tb, "Damaged.o3mp", damaged);
  fat.add_file(tb, "Newer.o3mp", newer_major_bytes(make_preset("tb303", fp::kKindInstrument, ents, 2)));
  fat.add_file(tb, "Big.o3mp", Bytes(fp::kMaxFileBytes + 1u, 0x41));  // over kMaxFileBytes
  fat.add_file(tb, "notes.txt", Bytes(10, 0x42));                     // not a preset: not listed
  for (unsigned i = 0; i < 129u; ++i) {  // 129 files: the list caps at 128 and says more
    char name[16];
    std::snprintf(name, sizeof name, "p%03u.o3mp", i);
    const std::pair<const char*, float> one[1] = {{"Cutoff", 0.3f}};
    fat.add_file(sid, name, preset_bytes(make_preset("sid", fp::kKindInstrument, one, 1)));
  }
  Rig rig(medium);
  bool more = false;
  const unsigned n = rig.ui.scan_presets("tb303", more);
  CHECK(n == 7u);
  CHECK_FALSE(more);
  CHECK(listed(rig.ui, 0) == "Acid");
  CHECK(listed(rig.ui, 1) == "b");
  CHECK(listed(rig.ui, 2) == "Big");
  CHECK(listed(rig.ui, 3) == "Damaged");
  CHECK(listed(rig.ui, 4) == "Fx");
  CHECK(listed(rig.ui, 5) == "Newer");
  CHECK(listed(rig.ui, 6) == "Other");
  CHECK(rig.ui.card_preset_at(7) == nullptr);
  CHECK(rig.ui.scan_presets("nosuchmodule", more) == 0u);  // no folder
  CHECK(rig.ui.scan_presets("sid", more) == app::ModuleUi::kCardPresetListMax);
  CHECK(more);  // "+ MORE ON CARD"
  const unsigned n_sid = rig.ui.scan_presets("sid", more);
  CHECK(n_sid == 128u);
  CHECK(listed(rig.ui, 0) == "p000");
  CHECK(listed(rig.ui, 127) == "p127");

  const o3m_module* m = rig.module_of("tb303");
  NEED(m != nullptr);
  app::ParamSet set;
  unsigned skipped = 0;
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_INSTRUMENT, m, "Acid", set, skipped) == app::ModuleUi::CardPreset::Ok);
  CHECK(skipped == 0u);
  CHECK(set.source == app::ParamSet::Source::Card);
  CHECK(std::string(set.name) == "Acid");
  CHECK(set.given() == 4u);
  CHECK(set.has(0));
  CHECK(set.values[0] == 0.9f);  // Cutoff
  CHECK(set.has(1));
  CHECK(set.values[1] == 0.1f);  // Resonance
  CHECK(set.has(2));
  CHECK(set.values[2] == 7.0f);  // Env Mod (an INT)
  CHECK(set.has(3));
  CHECK(set.values[3] == 1.5f);  // Decay
  // The resolve by name: ASCII case-insensitive, clamped to the range, rounded for an INT, an unknown name counted.
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_INSTRUMENT, m, "b", set, skipped) == app::ModuleUi::CardPreset::Ok);
  CHECK(skipped == 1u);
  CHECK(set.values[0] == 1.0f);  // "cutoff" 5.0 clamped to max 1
  CHECK(set.values[1] == 0.0f);  // "RESONANCE" -3 clamped to min 0
  CHECK(set.values[2] == 6.0f);  // "env mod" 6.4 rounded to 6
  CHECK_FALSE(set.has(3));       // "No Such": unmatched, its default at apply
  // The refusals, none of them touching the set.
  const app::ParamSet before = set;
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_INSTRUMENT, m, "Other", set, skipped) == app::ModuleUi::CardPreset::OtherModule);
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_INSTRUMENT, m, "Fx", set, skipped) == app::ModuleUi::CardPreset::OtherModule);
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_INSTRUMENT, m, "Damaged", set, skipped) == app::ModuleUi::CardPreset::Damaged);
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_INSTRUMENT, m, "Newer", set, skipped) == app::ModuleUi::CardPreset::NewerMajor);
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_INSTRUMENT, m, "Big", set, skipped) == app::ModuleUi::CardPreset::TooBig);
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_INSTRUMENT, m, "Missing", set, skipped) == app::ModuleUi::CardPreset::NotFound);
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_INSTRUMENT, m, "a/b", set, skipped) == app::ModuleUi::CardPreset::NotFound);
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_INSTRUMENT, m, std::string(31, 'x').c_str(), set, skipped) == app::ModuleUi::CardPreset::NotFound);
  CHECK(same_set(set, before));
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_EFFECT, m, "Acid", set, skipped) == app::ModuleUi::CardPreset::OtherModule);  // the kind is checked too
  CHECK(rig.ui.read_preset("nosuchmodule", O3M_KIND_INSTRUMENT, m, "Acid", set, skipped) == app::ModuleUi::CardPreset::NotFound);
  // The project form: no bound module at a free slot.
  CHECK(rig.ui.read_preset(rig.project(), app::ParamTarget(63), "Acid", set, skipped) == app::ModuleUi::CardPreset::NoModule);
}

TEST_CASE("preset card: save, list, load on a module instrument and an insert: the values land where apply_preset writes them (one undo unit for an instrument, none for an insert)") {
  Medium medium{200'000};
  Fat32Builder fat(medium);
  Rig rig(medium);
  model::Project& p = rig.project();
  const unsigned slot = 0;
  // Move the knobs (one undo unit of its own), then capture what is in force and save it.
  app::ParamSet move;
  move.set_name("move");
  move.set_value(0, 0.11f);
  move.set_value(1, 0.22f);
  move.set_value(2, 4.0f);
  move.set_value(3, 0.9f);
  move.set_value(4, 0.77f);  // "Filter Cutoff": 13 characters, past the page caption's 12
  unsigned dropped = 0;
  CHECK(rig.ui.apply_preset(*rig.editor, app::ParamTarget(slot), move, &dropped) == app::ModuleUi::PresetResult::Ok);
  fp::Preset cap{};
  CHECK(rig.ui.capture_preset(p, app::ParamTarget(slot), cap));
  CHECK(std::string(cap.module) == "tb303");
  CHECK(cap.kind == fp::kKindInstrument);
  CHECK(cap.count == 5u);
  CHECK(std::string(cap.entries[0].name) == "Cutoff");
  CHECK(cap.entries[0].value == 0.11f);
  CHECK(cap.entries[2].value == 4.0f);
  CHECK(std::string(cap.entries[4].name) == "Filter Cutoff");  // saved under the module's raw name, all 13 characters
  CHECK(cap.entries[4].value == 0.77f);
  char final[40] = {};
  CHECK(rig.saves.save_preset(rig.jobs, cap, "My Sound", final, sizeof final) == app::SaveService::Start::Started);
  rig.pump();
  CHECK(std::string(final) == "My Sound.o3mp");
  CHECK(exists(rig.fat, "modules/presets/tb303/My Sound.o3mp"));
  bool more = false;
  CHECK(rig.ui.scan_presets("tb303", more) == 1u);
  CHECK(listed(rig.ui, 0) == "My Sound");
  // Move the knobs again, then load the saved preset back.
  app::ParamSet other;
  other.set_value(0, 0.99f);
  other.set_value(1, 0.01f);
  other.set_value(2, 9.0f);
  other.set_value(3, 0.1f);
  other.set_value(4, 0.13f);
  CHECK(rig.ui.apply_preset(*rig.editor, app::ParamTarget(slot), other) == app::ModuleUi::PresetResult::Ok);
  const std::size_t depth = rig.editor->journal().depth();
  app::ParamSet set;
  unsigned skipped = 0;
  CHECK(rig.ui.read_preset(p, app::ParamTarget(slot), "My Sound", set, skipped) == app::ModuleUi::CardPreset::Ok);
  CHECK(skipped == 0u);  // "Filter Cutoff" resolves by its full raw name, not the cut caption
  CHECK(set.source == app::ParamSet::Source::Card);
  CHECK(rig.ui.apply_preset(*rig.editor, app::ParamTarget(slot), set, &dropped) == app::ModuleUi::PresetResult::Ok);
  CHECK(p.get_param(mp(slot, 0)) == 0.11f);
  CHECK(p.get_param(mp(slot, 1)) == 0.22f);
  CHECK(p.get_param(mp(slot, 2)) == 4.0f);
  CHECK(p.get_param(mp(slot, 3)) == 0.9f);
  CHECK(p.get_param(mp(slot, 4)) == 0.77f);  // the 13-character name matched and its value came back
  CHECK(rig.editor->journal().depth() == depth + 1u);              // the load is one undo unit
  CHECK(rig.editor->undo(model::UndoArea::Sound) == model::UndoResult::Ok);  // and one undo puts the moved values back
  CHECK(p.get_param(mp(slot, 0)) == 0.99f);
  CHECK(p.get_param(mp(slot, 2)) == 9.0f);
  CHECK(p.get_param(mp(slot, 4)) == 0.13f);
  // The insert: capture, save under its module's folder, change the record, load back (no undo step).
  const app::ParamTarget it = app::ParamTarget::of_insert(2u * 2u + 0u);  // track 2, INS A
  fp::Preset icap{};
  CHECK(rig.ui.capture_preset(p, it, icap));
  CHECK(std::string(icap.module) == "widener");
  CHECK(icap.kind == fp::kKindEffect);
  CHECK(icap.count == 2u);
  CHECK(icap.entries[0].value == 0.25f);
  CHECK(icap.entries[1].value == 0.75f);
  char final2[40] = {};
  CHECK(rig.saves.save_preset(rig.jobs, icap, "Wide", final2, sizeof final2) == app::SaveService::Start::Started);
  rig.pump();
  CHECK(exists(rig.fat, "modules/presets/widener/Wide.o3mp"));
  CHECK(rig.ui.scan_presets("widener", more) == 1u);
  CHECK(listed(rig.ui, 0) == "Wide");
  model::InsertRecord r = *p.find_insert(2, 0);
  r.param[0] = 0.0f;
  r.param[1] = 1.0f;
  CHECK(rig.editor->set_insert(r) == model::Status::Ok);
  const std::size_t depth2 = rig.editor->journal().depth();
  app::ParamSet iset;
  CHECK(rig.ui.read_preset(p, it, "Wide", iset, skipped) == app::ModuleUi::CardPreset::Ok);
  CHECK(skipped == 0u);
  CHECK(iset.given() == 2u);
  CHECK(rig.ui.apply_preset(*rig.editor, it, iset) == app::ModuleUi::PresetResult::Ok);
  const model::InsertRecord* after = p.find_insert(2, 0);
  NEED(after != nullptr);
  CHECK(after->param[0] == 0.25f);
  CHECK(after->param[1] == 0.75f);
  CHECK(rig.editor->journal().depth() == depth2);  // an insert's preset is no undo step (OS-INSFX D9)
  // A file that names another module is refused on load (read against the insert's module, it is another module's file).
  const std::pair<const char*, float> one[1] = {{"Width", 0.8f}};
  const fp::Preset wrong = make_preset("tb303", fp::kKindInstrument, one, 1);  // a tb303 file, saved under tb303/
  char f3[40] = {};
  CHECK(rig.saves.save_preset(rig.jobs, wrong, "Wrong", f3, sizeof f3) == app::SaveService::Start::Started);
  rig.pump();
  CHECK(exists(rig.fat, "modules/presets/tb303/Wrong.o3mp"));
  const app::ParamSet kept = iset;
  app::ParamSet wset;
  CHECK(rig.ui.read_preset("tb303", O3M_KIND_EFFECT, rig.module_of("widener"), "Wrong", wset, skipped) == app::ModuleUi::CardPreset::OtherModule);
  CHECK(same_set(wset, kept) == false);  // (the refusal left the set it was given, not the last good one)
  CHECK(rig.ui.read_preset(p, it, "Wrong", wset, skipped) == app::ModuleUi::CardPreset::NotFound);  // not in the widener's folder
}

TEST_CASE("preset card: the scan list is exactly the files the save guard takes as a Target (a corpus of tricky names, held equal to classify())") {
  Medium medium{200'000};
  Fat32Builder fat(medium);
  const std::uint32_t root = fat.make_dir(Fat32Builder::kRootCluster, "modules");
  const std::uint32_t pr = fat.make_dir(root, "presets");
  const std::uint32_t tb = fat.make_dir(pr, "tb303");
  const char* names[] = {"ok.o3mp",       "CON.o3mp",  "com1.o3mp", "a..b.o3mp",        "nul.x.o3mp", "A B-C_d.o3mp",
                         "x.o3mp.bak",    "x.wav",     ".o3mp",     "31-chars-aaaaaaaaaaaaaaaaaaaaa.o3mp",
                         "30-chars-aaaaaaaaaaaaaaaaaaaa.o3mp", "UPPER.O3MP", "sp ace.o3mp", "lpt9.o3mp", "aux.o3mp",
                         "abc 2.o3mp",    "abc.o3mp",  "abc-2.o3mp"};  // the last three pin the order: by the stem, "abc" < "abc 2" < "abc-2"
  const std::pair<const char*, float> one[1] = {{"Cutoff", 0.3f}};
  for (const char* nm : names) {
    fat.add_file(tb, nm, preset_bytes(make_preset("tb303", fp::kKindInstrument, one, 1)));
  }
  // A folder whose first 256 entries are junk: the presets behind them must still set "+ MORE ON CARD".
  const std::uint32_t many = fat.make_dir(pr, "many");
  for (unsigned i = 0; i < 256u; ++i) {
    char junk[24];
    std::snprintf(junk, sizeof junk, "junk%03u.txt", i);
    fat.add_file(many, junk, Bytes(8, 0x55));
  }
  fat.add_file(many, "z1.o3mp", preset_bytes(make_preset("many", fp::kKindInstrument, one, 1)));
  fat.add_file(many, "z2.o3mp", preset_bytes(make_preset("many", fp::kKindInstrument, one, 1)));
  // A folder just under the 256-entry bound: 254 junk files, then two presets.
  const std::uint32_t few = fat.make_dir(pr, "few");
  for (unsigned i = 0; i < 254u; ++i) {
    char junk[24];
    std::snprintf(junk, sizeof junk, "junk%03u.txt", i);
    fat.add_file(few, junk, Bytes(8, 0x55));
  }
  fat.add_file(few, "z1.o3mp", preset_bytes(make_preset("few", fp::kKindInstrument, one, 1)));
  fat.add_file(few, "z2.o3mp", preset_bytes(make_preset("few", fp::kKindInstrument, one, 1)));
  Rig rig(medium);  // one rig: FatFs mounts one volume at a time
  // The names as the card holds them (a long or odd name may come back differently than it was written).
  std::vector<std::string> on_disk;
  {
    hs::DirHandle d;
    NEED(rig.fat.opendir("modules/presets/tb303", d) == hs::Status::Ok);
    hs::DirEntry e;
    while (rig.fat.readdir(d, e) == hs::Status::Ok) {
      on_disk.push_back(e.name);
    }
    rig.fat.closedir(d);
  }
  std::vector<std::string> targets;  // the on-disk stems (suffix off) the guard classifies as a Target
  for (const std::string& nm : on_disk) {
    char full[96];
    std::snprintf(full, sizeof full, "modules/presets/tb303/%s", nm.c_str());
    if (hal::storage::save::classify(full) == hal::storage::save::Place::Target) {
      targets.push_back(nm.substr(0, nm.size() - 5u));
    }
  }
  const auto ci = [](const std::string& a, const std::string& b) {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
      const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
      const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
      if (x != y) {
        return x < y;
      }
    }
    return a.size() < b.size();
  };
  std::sort(targets.begin(), targets.end(), ci);  // the same order the scan lists them in
  bool more = false;
  const unsigned got = rig.ui.scan_presets("tb303", more);
  CHECK(got == targets.size());
  CHECK_FALSE(more);
  for (unsigned i = 0; i < got && i < targets.size(); ++i) {
    CHECK(listed(rig.ui, i) == targets[i]);
  }
  // The 256-entry readdir bound with junk first: the two presets behind it are beyond the bound (not listed), but the list says more instead of vanishing silently.
  bool more_many = false;
  CHECK(rig.ui.scan_presets("many", more_many) == 0u);
  CHECK(more_many);
  // Just under the bound: both presets are listed and nothing is said about more.
  bool more_few = false;
  CHECK(rig.ui.scan_presets("few", more_few) == 2u);
  CHECK_FALSE(more_few);
  CHECK(listed(rig.ui, 0) == "z1");
  CHECK(listed(rig.ui, 1) == "z2");
}
