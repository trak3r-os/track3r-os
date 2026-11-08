// SPDX-License-Identifier: GPL-3.0-or-later
// OS-FILES part A: the module import loader (app/module_import.h) over a card that holds generated MOD, S3M and OctaMED files (FakeStorage with bytes): the project the importer makes,
// the samples into the arena through the PCM feed, the engine option, a file cut short, a file that is not a module, a card without the file, an arena without room, steps of any size,
// and the texts of the strip and the report page. The importers' own rules are modimp_import_test.cpp and med_import_test.cpp.
#include <doctest/doctest.h>

#include "support/memory_io.h"
#include "support/mmd_fixture.h"
#include "support/modimp_fixture.h"
#include "support/module_blob.h"
#include "support/need.h"
#include "support/static_slot.h"

#include <o3os/formats/med/import.h>
#include <o3os/formats/med/mmd.h>
#include <o3os/formats/mod/import.h>
#include <o3os/formats/native.h>

#include <o3os/app/module_import.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/voices.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/fake_storage.h>

#include <dlfcn.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::ImportEngine;
using app::ModuleImport;
using app::ModuleSummary;
using app::OgSampleLoader;
using Bytes = std::vector<std::uint8_t>;

// The values behind ModuleImport's `static_cast<std::uint8_t>(engine_)` (app/src/module_import.cpp begin_import, into ImportModuleOptions::engine), which the
// importer module's imp_begin reads back as opts->engine (modules/importers/tracker/tracker.cpp: 0 Auto, 1 Amiga, 2 Modern).
static_assert(static_cast<std::uint8_t>(ImportEngine::Auto) == 0u && static_cast<std::uint8_t>(ImportEngine::Amiga) == 1u &&
                  static_cast<std::uint8_t>(ImportEngine::Modern) == 2u,
              "ImportEngine's values are the importer module's options engine: 0 Auto, 1 Amiga, 2 Modern");

namespace {

// A MOD that plays two patterns, cut inside the second: it holds room for one pattern (OS-BUG-IMPTRUNC), so only the reader finds it short.
Bytes mod_cut_in_second_pattern() {
  test::modimp::ModSpec m;
  m.name = "cut two";
  m.samples[0].name = "kick";
  m.samples[0].data.assign(64, 0x10);
  m.orders = {0, 1};
  m.patterns = {{{{0, 0}, test::modimp::note(12, 1)}}, {{{0, 0}, test::modimp::note(12, 1)}}};
  Bytes b = test::modimp::build(m);
  b.resize(1084 + 1024 + 500);
  return b;
}

Bytes mod_bytes() {
  test::modimp::ModSpec m;
  m.name = "kc test";
  m.samples[0].name = "kick";
  for (unsigned i = 0; i < 64; ++i) {
    m.samples[0].data.push_back(static_cast<std::uint8_t>((i * 3u) & 0x7Fu));  // 0..126, positive 8-bit
  }
  m.samples[0].volume = 64;
  m.patterns[0][{0, 0}] = test::modimp::note(12, 1);       // period 428, sample 1
  m.patterns[0][{4, 1}] = test::modimp::note(14, 1, 0xC, 0x20);
  m.patterns[0][{8, 2}] = test::modimp::note(16, 1, 0x1, 0x04);
  return test::modimp::build(m);
}

Bytes s3m_bytes() {
  test::modimp::S3mSpec m;
  m.name = "s3m test";
  m.instruments.resize(1);
  m.instruments[0].data = Bytes(48, 0);
  for (std::size_t i = 0; i < 48; ++i) {
    m.instruments[0].data[i] = static_cast<std::uint8_t>(128 + (i % 8) * 10);  // unsigned 8-bit
  }
  m.instruments[0].name = "unsigned";
  m.instruments[0].c2spd = 22050;
  test::modimp::S3mCell c;
  c.note = 0x40;
  c.inst = 1;
  c.vol = 64;
  m.patterns[0][{0, 0}] = c;
  test::modimp::S3mCell d;
  d.cmd = 1;  // A: speed
  d.info = 5;
  m.patterns[0][{2, 0}] = d;
  return test::modimp::build(m);
}

Bytes med_bytes() { return test::mmd::build(test::mmd::sample_module(1)); }

// The base of a buffer aligned up to `align` (a power of two): Arena::init wants both its views 64-aligned, and a heap buffer is only 16-aligned.
template <std::size_t align>
std::uint8_t* align_up(std::uint8_t* p) noexcept {
  const std::uintptr_t up = (reinterpret_cast<std::uintptr_t>(p) + (align - 1u)) & ~static_cast<std::uintptr_t>(align - 1u);
  return reinterpret_cast<std::uint8_t*>(up);
}

struct Rig {
  platform::host::FakeStorage card;
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::unique_ptr<ModuleImport> import = std::make_unique<ModuleImport>();
  std::unique_ptr<OgSampleLoader> loader = std::make_unique<OgSampleLoader>();
  std::vector<std::int16_t> pcm = std::vector<std::int16_t>(200000, std::int16_t{-1});
  audio::VoicePool* pool = nullptr;
  // OS-FLASHDIET I2/I3: the module host the platform hands over, with the tracker host object registered as a native importer entry (the module path).
  // with_module=false registers no native entry: with no importer file on the card either, begin() refuses the import (NO IMPORTER).
  module::ModuleSet mset;
  module::ModuleLoader mloader;
  // The arenas' target-side base: on the host the buffer is a 64-bit heap pointer, so a fixed 32-bit base stands in for the address the target
  // sees (the native importer entry never dereferences it; Arena only checks that base and mem are 64-aligned and that base + size fits in 32 bits).
  // OS-BUG-IMPFLAKE: the truncated heap pointer as the base let ASLR refuse a Rig's arena (addr + size over 4 GiB, or mem not 64-aligned), and the
  // import then failed with NO MODULE MEMORY; the buffers are 64 bytes larger so their base can be aligned up into them.
  static constexpr std::uint32_t kHotAddr = 0x20000000u;
  static constexpr std::uint32_t kColdAddr = 0x30000000u;
  std::vector<std::uint8_t> hot = std::vector<std::uint8_t>(8 * 1024 + 64);
  std::vector<std::uint8_t> cold = std::vector<std::uint8_t>(192 * 1024 + 64);
  void* so = nullptr;

  explicit Rig(bool with_module = true) {
    static test::StaticSlot<audio::VoicePool> slot;
    pool = &slot.make();
    arena->project.reset();
    std::uint8_t* const hot_mem = align_up<64>(hot.data());
    std::uint8_t* const cold_mem = align_up<64>(cold.data());
    mset.hot().init(hot_mem, kHotAddr, static_cast<std::uint32_t>(hot.size() - static_cast<std::size_t>(hot_mem - hot.data())));
    mset.cold().init(cold_mem, kColdAddr, static_cast<std::uint32_t>(cold.size() - static_cast<std::size_t>(cold_mem - cold.data())));
    if (with_module) {
      so = dlopen(O3M_TRACKER_HOST_SO, RTLD_NOW | RTLD_LOCAL);
      CHECK(so != nullptr);
      if (so != nullptr) {
        const o3m_module* entry = static_cast<const o3m_module*>(dlsym(so, "o3m_entry"));
        CHECK(entry != nullptr);
        if (entry != nullptr) {
          CHECK(mset.add_native("tracker", *entry));
        }
      }
      import->set_module_host(&mset, &mloader, nullptr, "tracker");
    }
  }
  ~Rig() {
    if (so != nullptr) {
      dlclose(so);
    }
  }

  // The project phase to its end in steps of `budget` bytes of the platform's budget; the state it ended in.
  ModuleImport::State run(const std::string& path, ImportEngine engine = ImportEngine::Auto, std::size_t budget = 16384) {
    const ModuleImport::Start began = import->begin(card, path.c_str(), *arena, engine);
    if (began == ModuleImport::Start::Failed) {
      return ModuleImport::State::Failed;  // refused at begin (OS-FLASHDIET I2, QA-FIL-139/140)
    }
    if (began != ModuleImport::Start::Started) {
      return ModuleImport::State::Idle;
    }
    for (unsigned i = 0; i < 2000000 && import->state() == ModuleImport::State::Running; ++i) {
      (void)import->step(budget);
    }
    return import->state();
  }
  // The samples phase, as the platform runs it after the project; false when it could not start.
  bool samples(std::size_t budget = 16384, std::uint32_t arena_samples = 0) {
    if (!loader->begin_feed(*import, arena->project, *pool, pcm.data(), arena_samples != 0u ? arena_samples : static_cast<std::uint32_t>(pcm.size()))) {
      return false;
    }
    for (unsigned i = 0; i < 2000000 && loader->state() == OgSampleLoader::State::Running; ++i) {
      (void)loader->step(budget);
    }
    import->samples_done(loader->report());
    return loader->state() == OgSampleLoader::State::Done;
  }
  // The samples phase with the sample bank the platform gives the loader (OS-BUG-MODENGINE: the engine of each slot reaches the voices at the bind).
  bool samples_with_bank() {
    mem = std::make_unique<app::SampleBankMem>();
    bank = std::make_unique<app::SampleBank>();  // (made here: a bank that was never init()ed cannot be destroyed, close_wav() reaches its memory)
    bank->init(*pool, *arena, *mem, pcm.data(), static_cast<std::uint32_t>(pcm.size()));
    if (!loader->begin_feed(*import, arena->project, *pool, pcm.data(), static_cast<std::uint32_t>(pcm.size()), audio::LevelLaw::Og12, bank.get())) {
      return false;
    }
    for (unsigned i = 0; i < 2000000 && loader->state() == OgSampleLoader::State::Running; ++i) {
      (void)loader->step(16384);
    }
    import->samples_done(loader->report());
    return loader->state() == OgSampleLoader::State::Done;
  }
  std::unique_ptr<app::SampleBankMem> mem;  // (declared before the bank: destroyed after it)
  std::unique_ptr<app::SampleBank> bank;
};

}  // namespace

TEST_CASE("module import: a MOD becomes a project, its sample goes to the arena at the Paula rate, the commands go to the Amiga engine") {
  Rig r;
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(r.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  const ModuleSummary& s = r.import->summary();
  CHECK(s.valid);
  CHECK(s.ok);
  CHECK(s.format == ModuleSummary::Format::Mod);
  CHECK(s.channels == 4);
  CHECK(s.amiga_commands);  // AUTO: a MOD plays through the Amiga engine window
  CHECK(s.patterns >= 1);
  CHECK(s.instruments == 1);
  CHECK(std::string(s.name) == "KC_TEST.MOD");
  CHECK(r.arena->project.song_length() >= 1);
  const model::Instrument* in = r.arena->project.instrument(0);
  NEED(in != nullptr);
  CHECK(in->kind == model::InstrumentKind::Sample);

  NEED(r.samples());
  CHECK(r.loader->report().loaded == 1);
  CHECK(r.loader->report().no_room == 0);
  const audio::SampleDesc& d = r.pool->sample(0);
  CHECK(d.length != 0);
  CHECK(d.data == r.pcm.data());
  for (unsigned i = 0; i < 64; ++i) {
    CHECK(r.pcm[i] == static_cast<std::int16_t>(static_cast<int>((i * 3u) & 0x7Fu) * 256));  // 8-bit signed times 256
  }
  CHECK(r.import->summary().samples_loaded == 1);
  CHECK(r.import->summary().samples_kb == 0);  // (64 samples: under half a KB)
}

TEST_CASE("module import: an S3M keeps its own rate and the commands go to the OG / core FX; the F7 choice sends them elsewhere") {
  Rig r;
  r.card.add_file("S3M/test.s3m", s3m_bytes());
  NEED(r.run("S3M/test.s3m") == ModuleImport::State::Done);
  CHECK(r.import->summary().format == ModuleSummary::Format::S3m);
  CHECK_FALSE(r.import->summary().amiga_commands);  // AUTO: an S3M is on the Modern engine
  NEED(r.samples());
  const audio::SampleDesc& d = r.pool->sample(0);
  CHECK(d.length != 0);
  CHECK(r.pcm[0] == 0);  // unsigned 128 is silence
  CHECK(r.pcm[1] == 10 * 256);
  const model::Instrument* in = r.arena->project.instrument(0);
  NEED(in != nullptr);
  CHECK(in->sample_rate == 22050);

  Rig a;
  a.card.add_file("S3M/test.s3m", s3m_bytes());
  NEED(a.run("S3M/test.s3m", ImportEngine::Amiga) == ModuleImport::State::Done);
  CHECK(a.import->summary().amiga_commands);  // AMIGA forces the engine window
  Rig m;
  m.card.add_file("S3M/test.s3m", s3m_bytes());
  NEED(m.run("S3M/test.s3m", ImportEngine::Modern) == ModuleImport::State::Done);
  CHECK_FALSE(m.import->summary().amiga_commands);
}

// OS-BUG-MODENGINE: the engine of an imported instrument is the voices' at once, not after a rebind. A MOD or an OctaMED sample is AUTO at a Paula rate: the Amiga engine. An S3M
// imported for the Modern engine writes MODERN explicitly (QA-FIL-006), so it stays Modern even at a Paula rate (the AUTO rule would send it to Amiga).
TEST_CASE("module import: a MOD plays on the Amiga engine right after the import, an S3M imported for the Modern engine stays Modern (OS-BUG-MODENGINE)") {
  Rig r;
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(r.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  NEED(r.samples_with_bank());
  const model::Instrument* in = r.arena->project.instrument(0);
  NEED(in != nullptr);
  CHECK(in->engine == model::kInstEngineAuto);
  CHECK((in->sample_rate == 8287u || in->sample_rate == 8363u));
  CHECK(r.pool->sample(0).length != 0u);
  CHECK(r.pool->sample(0).engine == audio::kEngineAmiga);
  CHECK(r.pool->sample(0).engine == r.bank->engine_for(0, *in));

  // an OctaMED sample is AUTO at the Paula rate too
  Rig q;
  q.card.add_file("MED/song.med", med_bytes());
  NEED(q.run("MED/song.med") == ModuleImport::State::Done);
  NEED(q.samples_with_bank());
  const model::Instrument* mi = q.arena->project.instrument(0);
  NEED(mi != nullptr);
  CHECK(q.pool->sample(0).length != 0u);
  CHECK(q.pool->sample(0).engine == q.bank->engine_for(0, *mi));

  // An S3M sample at 8,363 Hz: imported for the Modern engine (Auto), MODERN is written and it stays Modern. Imported for the Amiga engine it is AUTO, and AUTO at 8,363 Hz is Amiga.
  const auto s3m_at_paula_rate = []() {
    test::modimp::S3mSpec m;
    m.name = "s3m paula";
    m.instruments.resize(1);
    m.instruments[0].data = Bytes(48, 0);
    for (std::size_t i = 0; i < 48; ++i) {
      m.instruments[0].data[i] = static_cast<std::uint8_t>(128 + (i % 8) * 10);
    }
    m.instruments[0].name = "paula";
    m.instruments[0].c2spd = 8363;
    test::modimp::S3mCell c;
    c.note = 0x40;
    c.inst = 1;
    c.vol = 64;
    m.patterns[0][{0, 0}] = c;
    return test::modimp::build(m);
  };
  Rig m;
  m.card.add_file("S3M/paula.s3m", s3m_at_paula_rate());
  NEED(m.run("S3M/paula.s3m") == ModuleImport::State::Done);
  NEED(m.samples_with_bank());
  const model::Instrument* si = m.arena->project.instrument(0);
  NEED(si != nullptr);
  CHECK(si->sample_rate == 8363u);
  CHECK(si->engine == model::kInstEngineModern);
  CHECK(m.pool->sample(0).length != 0u);
  CHECK(m.pool->sample(0).engine == audio::kEngineModern);

  Rig a;
  a.card.add_file("S3M/paula.s3m", s3m_at_paula_rate());
  NEED(a.run("S3M/paula.s3m", ImportEngine::Amiga) == ModuleImport::State::Done);
  NEED(a.samples_with_bank());
  const model::Instrument* ai = a.arena->project.instrument(0);
  NEED(ai != nullptr);
  CHECK(ai->engine == model::kInstEngineAuto);
  CHECK(a.pool->sample(0).engine == audio::kEngineAmiga);
}

// OS-BUG-MODENGINE (review fold): "Modern" in the import dialog is a promise that the song plays on the sampler with the OG / core commands (a MOD's are the tracker FX 46..50, which the Amiga
// voices do not run). The instruments are at the Paula rate, so AUTO would send them to Amiga at the bind: the importers write MODERN when the commands are not the engine window's.
TEST_CASE("module import: a MOD or an OctaMED module imported for the Modern engine plays on the Modern engine, at a Paula rate too (OS-BUG-MODENGINE)") {
  Rig r;
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(r.run("MOD/kc_test.mod", ImportEngine::Modern) == ModuleImport::State::Done);
  NEED(r.samples_with_bank());
  const model::Instrument* in = r.arena->project.instrument(0);
  NEED(in != nullptr);
  CHECK((in->sample_rate == 8287u || in->sample_rate == 8363u));
  CHECK(in->engine == model::kInstEngineModern);
  CHECK(r.pool->sample(0).length != 0u);
  CHECK(r.pool->sample(0).engine == audio::kEngineModern);
  CHECK(r.bank->engine_for(0, *in) == audio::kEngineModern);

  Rig q;
  q.card.add_file("MED/song.med", med_bytes());
  NEED(q.run("MED/song.med", ImportEngine::Modern) == ModuleImport::State::Done);
  NEED(q.samples_with_bank());
  const model::Instrument* mi = q.arena->project.instrument(0);
  NEED(mi != nullptr);
  CHECK((mi->sample_rate == 8287u || mi->sample_rate == 8363u));
  CHECK(mi->engine == model::kInstEngineModern);
  CHECK(q.pool->sample(0).length != 0u);
  CHECK(q.pool->sample(0).engine == audio::kEngineModern);

  // for the Amiga engine (the default) both stay AUTO and play on Amiga
  Rig a;
  a.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(a.run("MOD/kc_test.mod", ImportEngine::Amiga) == ModuleImport::State::Done);
  NEED(a.samples_with_bank());
  CHECK(a.arena->project.instrument(0)->engine == model::kInstEngineAuto);
  CHECK(a.pool->sample(0).engine == audio::kEngineAmiga);
  Rig b;
  b.card.add_file("MED/song.med", med_bytes());
  NEED(b.run("MED/song.med", ImportEngine::Amiga) == ModuleImport::State::Done);
  NEED(b.samples_with_bank());
  CHECK(b.arena->project.instrument(0)->engine == model::kInstEngineAuto);
  CHECK(b.pool->sample(0).engine == audio::kEngineAmiga);
}

TEST_CASE("module import: the engine option for a MOD and for an OctaMED module") {
  for (const ImportEngine e : {ImportEngine::Auto, ImportEngine::Amiga, ImportEngine::Modern}) {
    Rig r;
    r.card.add_file("MOD/kc_test.mod", mod_bytes());
    NEED(r.run("MOD/kc_test.mod", e) == ModuleImport::State::Done);
    CHECK(r.import->summary().amiga_commands == (e != ImportEngine::Modern));
    Rig q;
    q.card.add_file("MED/song.med", med_bytes());
    NEED(q.run("MED/song.med", e) == ModuleImport::State::Done);
    CHECK(q.import->summary().format == ModuleSummary::Format::Med);
    CHECK(q.import->summary().amiga_commands == (e != ImportEngine::Modern));
  }
}

TEST_CASE("module import: an OctaMED module imports, its samples are fed, and .mmd1 names are taken") {
  Rig r;
  r.card.add_file("MED/song.MMD1", med_bytes());
  NEED(r.run("MED/song.MMD1") == ModuleImport::State::Done);
  const ModuleSummary& s = r.import->summary();
  CHECK(s.ok);
  CHECK(s.format == ModuleSummary::Format::Med);
  CHECK(s.patterns >= 1);
  CHECK(s.instruments >= 1);
  NEED(r.samples());
  CHECK(r.loader->report().loaded >= 1);
  CHECK(r.import->summary().samples_loaded == r.loader->report().loaded);
}

TEST_CASE("module import: the steps' size changes nothing") {
  Rig big;
  big.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(big.run("MOD/kc_test.mod", ImportEngine::Auto, 1u << 20) == ModuleImport::State::Done);
  NEED(big.samples(1u << 20));
  Rig tiny;
  tiny.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(tiny.run("MOD/kc_test.mod", ImportEngine::Auto, 1) == ModuleImport::State::Done);
  NEED(tiny.samples(1));
  const ModuleSummary& a = big.import->summary();
  const ModuleSummary& b = tiny.import->summary();
  CHECK(a.patterns == b.patterns);
  CHECK(a.order == b.order);
  CHECK(a.cmd_approx == b.cmd_approx);
  CHECK(a.cmd_dropped == b.cmd_dropped);
  CHECK(a.dropped_groups == b.dropped_groups);
  CHECK(a.samples_loaded == b.samples_loaded);
  for (unsigned i = 0; i < 64; ++i) {
    CHECK(big.pcm[i] == tiny.pcm[i]);
  }
}

TEST_CASE("module import: a file cut short fails with its reason and the strip says so") {
  const Bytes whole = mod_bytes();
  Rig r;
  const Bytes cut = mod_cut_in_second_pattern();  // inside the second pattern (a cut inside the first is refused before the question: OS-BUG-IMPTRUNC)
  r.card.add_file("MOD/cut.mod", cut);
  CHECK(r.run("MOD/cut.mod") == ModuleImport::State::Failed);
  CHECK_FALSE(r.import->summary().ok);
  CHECK(r.import->summary().valid);
  CHECK(std::string(r.import->summary().error) == "TRUNCATED");
  app::ModuleReportText failed;
  app::module_report_text(r.import->summary(), failed);
  NEED(failed.count >= 2);
  CHECK(std::string(failed.line[1]) == "FAILED    TRUNCATED");
  char line[64];
  app::module_strip_text(r.import->summary(), line, sizeof line);
  CHECK(std::string(line) == "IMPORT CUT.MOD FAILED: TRUNCATED");
  // the file is let go: a second import works and nothing stays open
  r.card.add_file("MOD/kc_test.mod", whole);
  CHECK(r.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  CHECK(r.import->summary().ok);

  Rig s;
  const Bytes s3m = s3m_bytes();
  s.card.add_file("S3M/cut.s3m", Bytes(s3m.begin(), s3m.begin() + 60));
  CHECK(s.run("S3M/cut.s3m") == ModuleImport::State::Failed);
  CHECK_FALSE(std::string(s.import->summary().error).empty());
  Rig q;
  const Bytes med = med_bytes();
  q.card.add_file("MED/cut.med", Bytes(med.begin(), med.begin() + 40));
  CHECK(q.run("MED/cut.med") == ModuleImport::State::Idle);  // OS-IMPORT-ANY: too short to be a song, so the header refuses it (the registry's sniff floor)
  q.card.add_file("MED/mid.med", Bytes(med.begin(), med.begin() + 900));  // past the floor (header + song), inside the body
  CHECK(q.run("MED/mid.med") == ModuleImport::State::Failed);
  CHECK_FALSE(std::string(q.import->summary().error).empty());
}

TEST_CASE("module import: bytes that are no module, a missing file and a wrong extension") {
  Rig r;
  Bytes noise(5000);
  for (std::size_t i = 0; i < noise.size(); ++i) {
    noise[i] = static_cast<std::uint8_t>(i * 131u + 7u);
  }
  r.card.add_file("MOD/noise.mod", noise);
  // OS-IMPORT-ANY I3: the bytes are read before anything starts: noise under a module's name is refused outright (it used to fail after the reset, with UNKNOWN FILE TYPE)
  CHECK(r.run("MOD/noise.mod") == ModuleImport::State::Idle);
  CHECK(r.import->begin(r.card, "MOD/noise.mod", *r.arena, ImportEngine::Auto) == ModuleImport::Start::NotModule);
  CHECK_FALSE(r.import->summary().valid);

  // a card without the file: the document is not touched
  Rig m;
  (void)m.arena->project.create_pattern(1, 32);
  (void)m.arena->project.append_order(1);
  CHECK(m.run("MOD/absent.mod") == ModuleImport::State::Failed);
  CHECK(std::string(m.import->summary().error) == "FILE NOT FOUND");
  CHECK(m.arena->project.song_length() == 1);

  Rig w;
  w.card.add_file("MOD/readme.txt", Bytes(100, 1));
  CHECK(w.run("MOD/readme.txt") == ModuleImport::State::Idle);  // not a module name: nothing started
  CHECK(w.import->begin(w.card, "MOD/readme.txt", *w.arena, ImportEngine::Auto) == ModuleImport::Start::NotModule);
  CHECK(w.import->begin(w.card, "../x.mod", *w.arena, ImportEngine::Auto) == ModuleImport::Start::BadPath);

  // a refused start keeps the last import's report
  Rig k;
  k.card.add_file("MOD/readme.txt", Bytes(100, 1));
  k.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(k.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  CHECK(k.import->begin(k.card, "MOD/readme.txt", *k.arena, ImportEngine::Auto) == ModuleImport::Start::NotModule);
  CHECK(k.import->summary().ok);
  CHECK(std::string(k.import->summary().name) == "KC_TEST.MOD");
}

TEST_CASE("module import: begin() writes nothing to the document; the importer's reset waits for the first step (after the platform's Settle), and a failure from there leaves it empty") {
  for (unsigned kind = 0; kind < 3; ++kind) {
    Rig r;
    const char* path = kind == 0 ? "MOD/kc_test.mod" : (kind == 1 ? "S3M/test.s3m" : "MED/song.med");
    r.card.add_file(path, kind == 0 ? mod_bytes() : (kind == 1 ? s3m_bytes() : med_bytes()));
    model::Project& project = r.arena->project;
    NEED(project.create_pattern(1, 32) == model::Status::Ok);
    NEED(project.append_order(1) == model::Status::Ok);
    const std::uint32_t generation = project.generation();
    NEED(r.import->begin(r.card, path, *r.arena, ImportEngine::Auto) == ModuleImport::Start::Started);
    // This is the whole of what the platform does between begin() and the end of Settle: the render still reads the document.
    CHECK(r.import->state() == ModuleImport::State::Running);
    CHECK_FALSE(r.import->document_touched());
    CHECK(project.generation() == generation);
    CHECK(project.song_length() == 1);
    CHECK(project.pattern(1) != nullptr);
    (void)r.import->step(1);  // the first step: the reader begins, the project is reset
    CHECK(r.import->document_touched());
    CHECK(project.generation() != generation);
    for (unsigned i = 0; i < 2000000 && r.import->state() == ModuleImport::State::Running; ++i) {
      (void)r.import->step(16384);
    }
    CHECK(r.import->state() == ModuleImport::State::Done);
  }

  // A file that cannot be opened: Failed at begin(), the reader never starts, the document is as it was.
  Rig m;
  NEED(m.arena->project.create_pattern(1, 32) == model::Status::Ok);
  NEED(m.arena->project.append_order(1) == model::Status::Ok);
  const std::uint32_t before = m.arena->project.generation();
  CHECK(m.import->begin(m.card, "MOD/absent.mod", *m.arena, ImportEngine::Auto) == ModuleImport::Start::Failed);  // OS-FLASHDIET I2 (QA-FIL-139/140): not Started
  CHECK(m.import->state() == ModuleImport::State::Failed);
  CHECK_FALSE(m.import->document_touched());
  CHECK(m.import->step(16384) == ModuleImport::State::Failed);
  CHECK_FALSE(m.import->document_touched());
  CHECK(m.arena->project.generation() == before);
  CHECK(m.arena->project.song_length() == 1);

  // Noise under a module's name never reaches the reader (OS-IMPORT-ANY I3): begin() refuses it and the document is as it was.
  Rig n;
  n.card.add_file("MOD/noise.mod", Bytes(5000, 0x5A));
  NEED(n.arena->project.create_pattern(1, 32) == model::Status::Ok);
  NEED(n.arena->project.append_order(1) == model::Status::Ok);
  const std::uint32_t kept = n.arena->project.generation();
  CHECK(n.run("MOD/noise.mod") == ModuleImport::State::Idle);
  CHECK_FALSE(n.import->document_touched());
  CHECK(n.arena->project.generation() == kept);
  CHECK(n.arena->project.song_length() == 1);

  // A bad file the header cannot tell from a module is found by the reader, after the reset: the document is empty (and the platform empties the voices' table and the bank).
  Rig t;
  const Bytes cut = mod_cut_in_second_pattern();  // the signature and the first pattern are there, the second is cut
  t.card.add_file("MOD/cut.mod", cut);
  NEED(t.arena->project.create_pattern(1, 32) == model::Status::Ok);
  NEED(t.arena->project.append_order(1) == model::Status::Ok);
  CHECK(t.run("MOD/cut.mod") == ModuleImport::State::Failed);
  CHECK(t.import->document_touched());
  CHECK(t.arena->project.song_length() == 0);
}

// OS-IMPORT-ANY I3: the reader is chosen by the file's bytes, and the name only has to agree.
TEST_CASE("module import: a module under a wrong or empty name imports by its header; a name that disagrees with the bytes refuses (OS-IMPORT-ANY I3)") {
  struct Case {
    const char* path;
    int kind;  // 0 MOD, 1 S3M, 2 MED
    bool imports;
  };
  const Case cases[] = {
      {"X/song", 0, true},         {"X/song.dat", 0, true},     {"X/mod.title", 0, true},  {"X/MOD.title", 0, true},     {"X/s3m.title", 1, true}, {"X/song.s3m", 0, true},
      {"X/song.mod", 1, true},     {"X/noext", 1, true},        {"X/med.title", 2, true},  {"X/mmd1.title", 2, true},    {"X/song", 2, true},      {"X/song.med", 0, false},
      {"X/song.mod", 2, false},    {"X/mod.title", 2, false},   {"X/med.title", 0, false}, {"X/song.s3m", 2, false},
  };
  for (const Case& c : cases) {
    Rig r;
    r.card.add_file(c.path, c.kind == 0 ? mod_bytes() : (c.kind == 1 ? s3m_bytes() : med_bytes()));
    const ModuleImport::State end = r.run(c.path);
    INFO(std::string(c.path) << " kind " << c.kind);  // a std::string: a const char* would print as a pointer
    if (c.imports) {
      CHECK(end == ModuleImport::State::Done);
      CHECK(r.import->summary().ok);
    } else {
      CHECK(end == ModuleImport::State::Idle);
      CHECK(r.import->begin(r.card, c.path, *r.arena, ImportEngine::Auto) == ModuleImport::Start::NotModule);
    }
  }
}

TEST_CASE("module import: a file that is no module is refused by probe() whatever its name, and nothing is touched (OS-IMPORT-ANY I3, I4)") {
  Rig r;
  Bytes wav(2000, 0);
  std::memcpy(wav.data(), "RIFF", 4);
  std::memcpy(wav.data() + 8, "WAVE", 4);
  r.card.add_file("X/kick.mod", wav);   // a WAV renamed .mod
  r.card.add_file("X/kick", wav);       // no name
  r.card.add_file("X/empty.mod", Bytes{});
  r.card.add_file("X/tiny", Bytes(10, 'M'));
  Bytes mmd = med_bytes();
  mmd.resize(16);  // a bare MMD0 header, too short to be a song
  r.card.add_file("X/short.med", mmd);
  for (const char* p : {"X/kick.mod", "X/kick", "X/empty.mod", "X/tiny", "X/short.med"}) {
    INFO(p);
    CHECK(ModuleImport::probe(r.card, p) == ModuleImport::Probe::NotModule);
  }
  r.card.add_file("X/good.mod", mod_bytes());
  CHECK(ModuleImport::probe(r.card, "X/good.mod") == ModuleImport::Probe::Mod);
  CHECK(ModuleImport::probe(r.card, "X/absent.mod") == ModuleImport::Probe::Unreadable);
  CHECK(ModuleImport::probe(r.card, "../x.mod") == ModuleImport::Probe::NotModule);
  // a failed probe changes nothing: the document and the last report
  NEED(r.arena->project.create_pattern(1, 32) == model::Status::Ok);
  const std::uint32_t generation = r.arena->project.generation();
  CHECK(r.import->begin(r.card, "X/kick.mod", *r.arena, ImportEngine::Auto) == ModuleImport::Start::NotModule);
  CHECK_FALSE(r.import->summary().valid);
  CHECK(r.arena->project.generation() == generation);
}

TEST_CASE("module import: a tagged MOD with no room for one pattern is refused before the question, as a cut file is (OS-BUG-IMPTRUNC)") {
  // a header and a tag and nothing else: 1,084 bytes pass the tag check, the question would be asked and the import would fail TRUNCATED
  const auto tagged = [](const char* tag, std::size_t size) {
    Bytes b(size, 0);
    std::memcpy(b.data() + 1080, tag, 4);
    return b;
  };
  Rig r;
  r.card.add_file("X/bare.mod", tagged("M.K.", 1084));
  r.card.add_file("X/bare", tagged("M.K.", 1084));
  r.card.add_file("X/short.mod", tagged("M.K.", 1084 + 1023));
  r.card.add_file("X/one.mod", tagged("M.K.", 1084 + 1024));
  r.card.add_file("X/eight_short.mod", tagged("8CHN", 1084 + 2047));  // a pattern of 8 channels is 2,048 bytes
  r.card.add_file("X/eight.mod", tagged("8CHN", 1084 + 2048));
  for (const char* p : {"X/bare.mod", "X/bare", "X/short.mod", "X/eight_short.mod"}) {
    INFO(p);
    CHECK(ModuleImport::probe(r.card, p) == ModuleImport::Probe::NotModule);
  }
  for (const char* p : {"X/one.mod", "X/eight.mod"}) {
    INFO(p);
    CHECK(ModuleImport::probe(r.card, p) == ModuleImport::Probe::Mod);
  }
  NEED(r.arena->project.create_pattern(1, 32) == model::Status::Ok);
  const std::uint32_t generation = r.arena->project.generation();
  CHECK(r.import->begin(r.card, "X/bare.mod", *r.arena, ImportEngine::Auto) == ModuleImport::Start::NotModule);
  CHECK_FALSE(r.import->summary().valid);
  CHECK(r.arena->project.generation() == generation);
}

// OS-IMPORT-ANY I6, I7: a 15-sample Soundtracker file (no signature) imports when the name says MOD or says nothing, as a 4-channel module with 15 instruments.
TEST_CASE("module import: a 15-sample Soundtracker file imports under a MOD name or none, never under another format's name (OS-IMPORT-ANY I6)") {
  const Bytes mk = mod_bytes();
  Bytes st(mk.begin(), mk.begin() + 470);  // the title and 15 sample headers, then the song length, restart and orders, then the rest (the 16 later headers and the tag cut out)
  st.insert(st.end(), mk.begin() + 950, mk.begin() + 1080);
  st.insert(st.end(), mk.begin() + 1084, mk.end());
  st[471] = 120;
  struct Case {
    const char* path;
    bool imports;
  };
  const Case cases[] = {{"X/old.mod", true}, {"X/mod.old", true}, {"X/old", true},      {"X/old.dat", true},  {"X/MOD.OLD", true},
                        {"X/old.s3m", false}, {"X/s3m.old", false}, {"X/old.med", false}, {"X/mmd1.old", false}};
  for (const Case& c : cases) {
    Rig r;
    r.card.add_file(c.path, st);
    INFO(c.path);
    if (c.imports) {
      NEED(r.run(c.path) == ModuleImport::State::Done);
      CHECK(r.import->summary().ok);
      CHECK(r.import->summary().channels == 4);
      CHECK(r.import->summary().format == app::ModuleSummary::Format::Mod);
      CHECK(r.arena->project.song_length() == 1);
      CHECK(ModuleImport::probe(r.card, c.path) == ModuleImport::Probe::Mod);
    } else {
      CHECK(r.run(c.path) == ModuleImport::State::Idle);
      CHECK(ModuleImport::probe(r.card, c.path) == ModuleImport::Probe::NotModule);
    }
  }
  // the samples phase: the one sample is the file's, 64 frames
  Rig s;
  s.card.add_file("X/old.mod", st);
  NEED(s.run("X/old.mod") == ModuleImport::State::Done);
  CHECK(s.samples());
  CHECK(s.import->summary().samples_loaded == 1);
  // a header that fails the structure (song length 0) is not guessed: noise-like, refused under every name
  Bytes bad = st;
  bad[470] = 0;
  Rig n;
  n.card.add_file("X/bad.mod", bad);
  n.card.add_file("X/bad", bad);
  CHECK(n.run("X/bad.mod") == ModuleImport::State::Idle);
  CHECK(n.run("X/bad") == ModuleImport::State::Idle);
  // the 31-sample file under no name is a MOD as before
  Rig k;
  k.card.add_file("X/new", mk);
  NEED(k.run("X/new") == ModuleImport::State::Done);
  CHECK(k.import->summary().channels == 4);
}

namespace {
// A feed that claims a frame count near 2^32 for every slot (a hostile module): the rounding to an even count must not wrap to 0.
struct HugeFeed final : app::PcmFeed {
  std::uint32_t frames(unsigned) const noexcept override { return 0xFFFFFFFFu; }
  std::uint32_t rate(unsigned) const noexcept override { return 8363; }
  bool read(unsigned, std::uint32_t, std::uint32_t, std::int16_t*) noexcept override { return true; }
};
}  // namespace

TEST_CASE("module import: a feed that claims more frames than the arena holds is no room, never a wrapped count") {
  Rig r;
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(r.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  HugeFeed feed;
  NEED(r.loader->begin_feed(feed, r.arena->project, *r.pool, r.pcm.data(), static_cast<std::uint32_t>(r.pcm.size())));
  for (unsigned i = 0; i < 100000 && r.loader->state() == OgSampleLoader::State::Running; ++i) {
    (void)r.loader->step(16384);
  }
  CHECK(r.loader->state() == OgSampleLoader::State::Done);
  CHECK(r.loader->report().loaded == 0);
  CHECK(r.loader->report().no_room == 1);
  CHECK(r.pool->sample(0).length == 0);
}

TEST_CASE("module import: the report names the tracks cut on a line of its own, within the page's width") {
  ModuleSummary s;
  s.valid = true;
  s.ok = true;
  s.format = ModuleSummary::Format::Med;
  s.patterns = 65535;
  s.order = 65535;
  s.tracks_kept = 65535;
  s.tracks_dropped = 12;
  app::ModuleReportText text;
  app::module_report_text(s, text);
  bool cut = false;
  for (unsigned i = 0; i < text.count; ++i) {
    CHECK(std::strlen(text.line[i]) <= app::ModuleReportText::kChars);
    cut = cut || std::string(text.line[i]) == "          12 TRACKS CUT";
  }
  CHECK(cut);
  s.tracks_dropped = 0;
  app::module_report_text(s, text);
  for (unsigned i = 0; i < text.count; ++i) {
    CHECK(std::string(text.line[i]).find("CUT") == std::string::npos);
  }
}

TEST_CASE("module import: an arena without room leaves the instrument silent and the report counts it") {
  Rig r;
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(r.run("MOD/kc_test.mod") == ModuleImport::State::Done);
  NEED(r.samples(16384, 32));  // 32 samples of arena for 64 frames
  CHECK(r.loader->report().loaded == 0);
  CHECK(r.loader->report().no_room == 1);
  CHECK(r.pool->sample(0).length == 0);
  const ModuleSummary& s = r.import->summary();
  CHECK(s.samples_no_room == 1);
  char line[64];
  app::module_strip_text(s, line, sizeof line);
  CHECK(std::string(line).find("1 NO ROOM") != std::string::npos);
  CHECK(std::string(line).find("F8 REPORT") != std::string::npos);
}

TEST_CASE("module import: the strip line cuts the name, never the reason, and the report page lists what happened") {
  Rig r;
  r.card.add_file("MOD/a_very_long_module_name_that_goes_on_and_on.mod", mod_bytes());
  NEED(r.run("MOD/a_very_long_module_name_that_goes_on_and_on.mod") == ModuleImport::State::Done);
  NEED(r.samples());
  char line[49];
  app::module_strip_text(r.import->summary(), line, sizeof line);
  CHECK(std::strlen(line) <= 48);
  CHECK(std::string(line).rfind("IMPORTED A_VERY_LONG", 0) == 0);

  app::ModuleReportText text;
  app::module_report_text(r.import->summary(), text);
  NEED(text.count >= 6);
  CHECK(std::string(text.line[0]).rfind("A_VERY_LONG", 0) == 0);
  CHECK(std::string(text.line[1]) == "FORMAT    MOD, 4 CHANNELS");
  CHECK(std::string(text.line[2]) == "COMMANDS  AMIGA ENGINE (FX 128+)");
  CHECK(std::string(text.line[3]).rfind("SONG      ", 0) == 0);
  CHECK(std::string(text.line[4]).rfind("SAMPLES   1 MADE, 1 LOADED", 0) == 0);
  for (unsigned i = 0; i < text.count; ++i) {
    CHECK(std::strlen(text.line[i]) <= app::ModuleReportText::kChars);
  }
  ModuleSummary none;
  app::module_report_text(none, text);
  CHECK(text.count == 1);
  CHECK(std::string(text.line[0]) == "NO IMPORT YET");
}

TEST_CASE("module import: the drop groups are named in the report, upper case") {
  ModuleSummary s;
  s.valid = true;
  s.ok = true;
  s.format = ModuleSummary::Format::Mod;
  s.dropped_groups = 0x1FFFu;  // all thirteen groups of the MOD importer: more lines than the page holds
  app::ModuleReportText text;
  app::module_report_text(s, text);
  CHECK(text.count == app::ModuleReportText::kLines);  // capped, not overrun
  bool header = false;
  for (unsigned i = 0; i < text.count; ++i) {
    header = header || std::string(text.line[i]).rfind("NOT KEPT", 0) == 0;
  }
  CHECK(header);
  CHECK(std::string(s.group_name(8)) == "slide timing");
}

// The maintainer's test set (the SD card's /MOD /S3M /MED; PC copy build-sdtest/O3TEST, git-ignored, never committed): O3_SDTEST_DIR names that folder. Every file goes through the loader and the PCM feed with
// a roomy arena; each either imports (and every instrument the importer made has its samples bound) or says why not. Skipped without the folder.
TEST_CASE("module import: the maintainer's test files import, every sample instrument is bound") {
  const char* env = std::getenv("O3_SDTEST_DIR");
  if (env == nullptr || !std::filesystem::is_directory(env)) {
    MESSAGE("O3_SDTEST_DIR is not set: the maintainer's test set was not run");
    return;
  }
  unsigned imported = 0;
  for (const char* sub : {"MOD", "S3M", "MED"}) {
    const std::filesystem::path dir = std::filesystem::path(env) / sub;
    if (!std::filesystem::is_directory(dir)) {
      continue;
    }
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
      std::ifstream in(e.path(), std::ios::binary);
      const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      Rig r;
      r.pcm.assign(4u << 20, std::int16_t{0});
      const std::string path = std::string(sub) + "/" + e.path().filename().string();
      r.card.add_file(path, bytes);
      const ModuleImport::State st = r.run(path);
      const ModuleSummary& s = r.import->summary();
      if (st != ModuleImport::State::Done) {
        MESSAGE(path << ": " << s.error << " (" << std::string(s.detail) << ")");
        CHECK_FALSE(std::string(s.error).empty());
        continue;
      }
      NEED(r.samples());
      ++imported;
      MESSAGE(path << ": " << s.patterns << " patterns, " << s.instruments << " instruments, " << r.loader->report().loaded << " loaded, " << r.loader->report().missing << " silent, "
                   << r.loader->report().no_room << " no room, groups " << s.dropped_groups);
      CHECK(r.loader->report().loaded + r.loader->report().missing + r.loader->report().no_room >= s.instruments - s.instruments_refused);
      CHECK(r.loader->report().no_room == 0);
    }
  }
  CHECK(imported > 0);
}

namespace {

// OS-FLASHDIET I2: the project the native writer makes of `p` (the bytes a save would hold: the runtime counters are not in it).
Bytes native_project_bytes(const model::Project& p) {
  formats::native::Writer w;
  test::VectorSink sink;
  w.begin(p, sink);
  while (w.step(4096) == formats::Progress::Running) {
  }
  return sink.bytes;
}

// OS-FLASHDIET I2: the PCM of every slot the import reports, through the feed: rate, frames and the samples, flat.
Bytes pcm_of(app::ModuleImport& imp) {
  Bytes out;
  for (unsigned slot = 0; slot < 64; ++slot) {
    const std::uint32_t frames = imp.frames(slot);
    if (frames == 0u) {
      continue;
    }
    const std::uint32_t rate = imp.rate(slot);
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&rate), reinterpret_cast<const std::uint8_t*>(&rate) + 4);
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&frames), reinterpret_cast<const std::uint8_t*>(&frames) + 4);
    std::vector<std::int16_t> buf(frames, std::int16_t{-1});
    CHECK(imp.read(slot, 0, frames, buf.data()));
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(buf.data()), reinterpret_cast<const std::uint8_t*>(buf.data()) + frames * 2);
  }
  return out;
}

bool summary_same(const ModuleSummary& a, const ModuleSummary& b) {
  return a.ok == b.ok && a.format == b.format && a.amiga_commands == b.amiga_commands && a.channels == b.channels && a.patterns == b.patterns &&
         a.order == b.order && a.tracks_kept == b.tracks_kept && a.tracks_dropped == b.tracks_dropped && a.instruments == b.instruments &&
         a.instruments_refused == b.instruments_refused && a.cmd_approx == b.cmd_approx && a.cmd_dropped == b.cmd_dropped && a.clamped == b.clamped &&
         a.dropped_groups == b.dropped_groups && std::strcmp(a.name, b.name) == 0;
}

// OS-FLASHDIET I3: the reference side of the equality sweep drives the importers of formats_tracker directly (the image does not link
// them any more, and app::ModuleImport has no linked path: the module is the only one). The same readers the tracker module runs, over
// the fixture's bytes, in an arena of its own, with the same options the proxy passes the module.
class NoBytes final : public core::ByteSource {
 public:
  core::IoResult read(void*, std::size_t) noexcept override { return {0, core::IoStatus::Failed}; }
};

struct LinkedRef {
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  formats::mod::ImportReader mod_reader;
  formats::med::ImportReader med_reader;
  formats::FormatReader* reader = nullptr;
  bool is_med = false;
  ModuleSummary summary;
  // the readers keep the source they were begun with: it must outlive run() (the PCM is read after it)
  std::unique_ptr<formats::med::SpanSource> span;

  bool run(const Bytes& bytes, const char* path, ImportEngine engine) {
    arena->project.reset();
    const std::size_t head = bytes.size() < formats::kSniffBytes ? bytes.size() : formats::kSniffBytes;
    const auto seen = ModuleImport::decide(path, bytes.data(), head, static_cast<std::uint32_t>(bytes.size()));
    is_med = seen == ModuleImport::Probe::Med;
    span = std::make_unique<formats::med::SpanSource>(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    NoBytes none;
    formats::ReadParams params;
    params.size_hint = static_cast<std::uint32_t>(bytes.size());
    params.random = span.get();
    if (is_med) {
      reader = &med_reader;
      med_reader.options() = formats::med::ImportReader::amiga_default();
      med_reader.options().engine_fx = engine != ImportEngine::Modern;  // as the proxy's begin_import did
    } else {
      reader = &mod_reader;
      mod_reader.options() = formats::mod::ImportOptions{};
      mod_reader.options().engine_fx = engine == ImportEngine::Auto ? formats::mod::EngineFx::Auto
                                       : (engine == ImportEngine::Amiga ? formats::mod::EngineFx::On : formats::mod::EngineFx::Off);
    }
    reader->begin(none, *arena, params);
    formats::Progress p = formats::Progress::Running;
    for (unsigned i = 0; i < 2000000 && p == formats::Progress::Running; ++i) {
      p = reader->step(ModuleImport::kMinCells);
    }
    if (p != formats::Progress::Done) {
      return false;
    }
    summary = ModuleSummary{};
    summary.valid = true;
    summary.ok = true;
    const formats::FormatReport& rep = reader->report();
    summary.dropped_groups = rep.dropped_groups;
    summary.clamped = rep.sanitised;
    if (is_med) {
      const formats::med::ImportReport& r = med_reader.importer().report();
      summary.format = ModuleSummary::Format::Med;
      summary.amiga_commands = med_reader.options().engine_fx;
      summary.channels = static_cast<std::uint8_t>(r.tracks_used);
      summary.patterns = r.patterns;
      summary.order = r.order;
      summary.tracks_kept = r.tracks_kept;
      summary.tracks_dropped = r.tracks_dropped;
      summary.instruments = r.instruments;
      summary.instruments_refused = r.instruments_refused;
      for (unsigned i = 0; i < 256u; ++i) {
        summary.cmd_approx += r.cmd_approx[i];
        summary.cmd_dropped += r.cmd_dropped[i];
      }
    } else {
      const formats::mod::ImportReport& r = mod_reader.importer().report();
      summary.format = r.kind == formats::mod::Kind::S3m ? ModuleSummary::Format::S3m : ModuleSummary::Format::Mod;
      summary.amiga_commands = r.engine_fx;
      summary.channels = r.channels;
      summary.patterns = r.med.patterns;
      summary.order = r.med.order;
      summary.tracks_kept = r.med.tracks_kept;
      summary.tracks_dropped = r.med.tracks_dropped;
      summary.instruments = r.med.instruments;
      summary.instruments_refused = static_cast<std::uint16_t>(r.med.instruments_refused + (r.instruments_beyond > 0xFFFFu ? 0xFFFFu : r.instruments_beyond));
      for (const formats::mod::FxTally& f : r.fx) {
        summary.cmd_approx += f.approx;
        summary.cmd_dropped += f.dropped;
      }
    }
    {  // the name the proxy writes: the base name, upper case
      const char* base = path;
      for (const char* q = path; *q != '\0'; ++q) {
        base = *q == '/' ? q + 1 : base;
      }
      std::size_t n = 0;
      for (const char* q = base; *q != '\0' && n < ModuleSummary::kNameChars; ++q) {
        const char c = *q;
        summary.name[n++] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
      }
      summary.name[n] = '\0';
    }
    return true;
  }

  std::uint32_t frames(unsigned slot) const {
    if (is_med) {
      return slot < formats::med::caps::kMaxSamples && med_reader.importer().report().instrument[slot].result == formats::med::InstResult::Sample
                 ? med_reader.importer().pcm_frames(slot)
                 : 0u;
    }
    return slot < formats::mod::caps::kSlots && mod_reader.importer().report().med.instrument[slot].result == formats::med::InstResult::Sample
               ? mod_reader.importer().pcm_frames(slot)
               : 0u;
  }
  std::uint32_t rate(unsigned slot) const { return is_med ? med_reader.importer().pcm_rate() : mod_reader.importer().pcm_rate(slot); }
  bool read(unsigned slot, std::uint32_t first, std::uint32_t n, std::int16_t* dst) const {
    return (is_med ? med_reader.importer().read_pcm(slot, first, n, dst) : mod_reader.importer().read_pcm(slot, first, n, dst)).ok();
  }
};

// The PCM of every slot the reference import reports, flat — the same shape pcm_of() gives the module path.
Bytes pcm_of_reader(const LinkedRef& r) {
  Bytes out;
  for (unsigned slot = 0; slot < 64; ++slot) {
    const std::uint32_t frames = r.frames(slot);
    if (frames == 0u) {
      continue;
    }
    const std::uint32_t rate = r.rate(slot);
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&rate), reinterpret_cast<const std::uint8_t*>(&rate) + 4);
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&frames), reinterpret_cast<const std::uint8_t*>(&frames) + 4);
    std::vector<std::int16_t> buf(frames, std::int16_t{-1});
    CHECK(r.read(slot, 0, frames, buf.data()));
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(buf.data()), reinterpret_cast<const std::uint8_t*>(buf.data()) + frames * 2);
  }
  return out;
}

}  // namespace

// OS-FLASHDIET I2/I3: the import through the importer module (the native entry of the tracker host object here, the packed tracker.tmod on the unit)
// makes the same project and the same PCM as the linked importers, byte for byte, for every fixture and every engine choice (F7): the generated MOD, S3M and
// OctaMED modules and, when the fetched test data is in build-fixtures/, every module file of it. I3: the linked side comes from formats_tracker in the
// test build (the image does not link it any more).
TEST_CASE("module import (OS-FLASHDIET I2): through the module the project and the PCM are the linked importer's, byte for byte") {
  struct Fixture {
    std::string path;
    Bytes bytes;
  };
  std::vector<Fixture> fixtures = {
      {"MOD/kc.mod", mod_bytes()},
      {"S3M/test.s3m", s3m_bytes()},
      {"MED/v0.mmd0", test::mmd::build(test::mmd::sample_module(0))},
      {"MED/v1.mmd1", test::mmd::build(test::mmd::sample_module(1))},
      {"MED/v2.mmd2", test::mmd::build(test::mmd::sample_module(2))},
      {"MED/v3.mmd3", test::mmd::build(test::mmd::sample_module(3))},
  };
  {
    const std::filesystem::path dir(O3_BUILD_FIXTURES_DIR);
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) {
      unsigned added = 0;
      for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
          continue;
        }
        std::string ext = it->path().extension().string();
        for (char& c : ext) {
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext != ".mod" && ext != ".s3m" && ext != ".med" && ext != ".mmd0" && ext != ".mmd1" && ext != ".mmd2" && ext != ".mmd3") {
          continue;
        }
        if (it->file_size(ec) > 4u * 1024u * 1024u || ec) {
          continue;
        }
        std::ifstream in(it->path(), std::ios::binary);
        Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        fixtures.push_back({"FIX/" + it->path().filename().string(), bytes});
        if (++added >= 40) {
          break;
        }
      }
      MESSAGE("build-fixtures: ", added, " module file(s) added to the equality sweep");
    }
  }
  for (const Fixture& f : fixtures) {
    for (const ImportEngine engine : {ImportEngine::Auto, ImportEngine::Amiga, ImportEngine::Modern}) {  // the F7 choice: all three sweep the equality
      INFO(f.path << " engine " << static_cast<unsigned>(engine));
      Rig a;  // the module path (the native importer entry)
      a.card.add_file(f.path, f.bytes);
      NEED(a.run(f.path, engine) == ModuleImport::State::Done);
      LinkedRef b;  // the linked readers of formats_tracker, driven directly (I3): the reference
      NEED(b.run(f.bytes, f.path.c_str(), engine));
      CHECK(native_project_bytes(a.arena->project) == native_project_bytes(b.arena->project));
      CHECK(pcm_of(*a.import) == pcm_of_reader(b));
      CHECK(summary_same(a.import->summary(), b.summary));
    }
  }
}

// OS-FLASHDIET I2 (3.6): no modules/importers/tracker.tmod on the card: the import fails at begin() with NO IMPORTER, nothing is reset, the strip says
// so and the REPORT page names the file to copy.
TEST_CASE("module import (OS-FLASHDIET I2): no importer file on the card: NO IMPORTER at begin, the document untouched, the report names the file") {
  Rig r(false);  // no native entry, and the card holds no importer file
  r.import->set_module_host(&r.mset, &r.mloader, nullptr, "tracker");
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  NEED(r.arena->project.create_pattern(1, 32) == model::Status::Ok);
  NEED(r.arena->project.append_order(1) == model::Status::Ok);
  const std::uint32_t generation = r.arena->project.generation();
  CHECK(r.import->begin(r.card, "MOD/kc_test.mod", *r.arena, ImportEngine::Auto) == ModuleImport::Start::Failed);  // QA-FIL-139/140: refused at begin, not Started
  CHECK(r.import->state() == ModuleImport::State::Failed);
  CHECK_FALSE(r.import->document_touched());
  CHECK(r.arena->project.generation() == generation);
  CHECK(r.arena->project.song_length() == 1);
  CHECK(std::string(r.import->summary().error) == "NO IMPORTER");
  char line[64];
  app::module_strip_text(r.import->summary(), line, sizeof line);
  CHECK(std::string(line) == "IMPORT KC_TEST.MOD FAILED: NO IMPORTER");
  app::ModuleReportText text;
  app::module_report_text(r.import->summary(), text);
  NEED(text.count >= 4);
  CHECK(std::string(text.line[1]) == "FAILED    NO IMPORTER");
  CHECK(std::string(text.line[2]) == "          NEEDS MODULES/IMPORTERS/TRACKER.TMOD");
  CHECK(std::string(text.line[3]) == "          FROM THE STARTER ZIP OF THIS RELEASE");
  for (unsigned i = 0; i < text.count; ++i) {
    CHECK(std::strlen(text.line[i]) <= app::ModuleReportText::kChars);
  }
}

// OS-FLASHDIET I2 (QA-FIL-139/140): the refusal at begin() is a Failed start, and the platform's one decision on it (import_replaces_document) says NO replace: with no
// importer file on the card, the open project is unchanged and its module instruments stay loaded in the set (on the target the Settle's set.clear() would have unloaded
// them, and the binder's Modules phase never ran: the document survived but its instruments and inserts stayed silent). This fails on the code that returned Started
// here and let start_module_import run begin_replace anyway.
TEST_CASE("module import (OS-FLASHDIET I2, QA-FIL-139/140): no importer file: a Failed start, no replace, the open project and its loaded modules untouched") {
  Rig r(false);  // no native importer entry, and the card holds no importer file
  r.import->set_module_host(&r.mset, &r.mloader, nullptr, "tracker");
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  r.card.add_file("modules/instruments/test.tmod", module::test::good_blob().file());  // the open project's SD instrument
  NEED(r.mloader.begin(r.card, r.mset, "test", nullptr));
  while (r.mloader.state() == module::ModuleLoader::State::Running) {
    (void)r.mloader.step(4096);
  }
  NEED(r.mloader.state() == module::ModuleLoader::State::Done);
  CHECK(r.mset.count() == 1);  // the module instrument is loaded in the set
  CHECK(r.mset.find("test") >= 0);
  NEED(r.arena->project.create_pattern(1, 32) == model::Status::Ok);
  NEED(r.arena->project.append_order(1) == model::Status::Ok);
  const std::uint32_t generation = r.arena->project.generation();
  const ModuleImport::Start began = r.import->begin(r.card, "MOD/kc_test.mod", *r.arena, ImportEngine::Auto);
  CHECK(began == ModuleImport::Start::Failed);  // (the bug returned Started here)
  CHECK_FALSE(app::import_replaces_document(began));  // the decision of start_module_import / SimProject::import_module: no begin_replace, show the strip directly
  CHECK(r.import->state() == ModuleImport::State::Failed);
  CHECK_FALSE(r.import->document_touched());
  CHECK(std::string(r.import->summary().error) == "NO IMPORTER");
  CHECK(r.arena->project.generation() == generation);  // the document is unchanged
  CHECK(r.arena->project.song_length() == 1);
  CHECK(r.arena->project.pattern(1) != nullptr);
  CHECK(r.mset.count() == 1);  // the open project's module instrument is still loaded
  CHECK(r.mset.find("test") >= 0);
  CHECK(r.mset.importer_count() == 0);  // nothing was loaded or dropped
}

// OS-FLASHDIET I2 (3.6): an importer file for another firmware (wrong kind, ABI or layout tag) fails at begin() the same way, with IMPORTER FOR OTHER FIRMWARE.
TEST_CASE("module import (OS-FLASHDIET I2): an importer file of another firmware: IMPORTER FOR OTHER FIRMWARE at begin, the document untouched") {
  Rig r(false);
  r.import->set_module_host(&r.mset, &r.mloader, nullptr, "tracker");
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  module::test::Blob bad = module::test::blob_importer();
  bad.h.reserved2[1] ^= 1u;  // the layout tag of another build
  bad.seal();
  r.card.add_file("modules/importers/tracker.tmod", bad.file());
  NEED(r.arena->project.create_pattern(1, 32) == model::Status::Ok);
  NEED(r.arena->project.append_order(1) == model::Status::Ok);
  const std::uint32_t generation = r.arena->project.generation();
  CHECK(r.import->begin(r.card, "MOD/kc_test.mod", *r.arena, ImportEngine::Auto) == ModuleImport::Start::Failed);
  CHECK(r.import->state() == ModuleImport::State::Failed);
  CHECK_FALSE(r.import->document_touched());
  CHECK(r.arena->project.generation() == generation);
  CHECK(r.arena->project.song_length() == 1);
  CHECK(std::string(r.import->summary().error) == "IMPORTER FOR OTHER FIRMWARE");
  char line[64];
  app::module_strip_text(r.import->summary(), line, sizeof line);
  CHECK(std::string(line) == "IMPORT KC_TEST.MOD FAILED: IMPORTER FOR OTHER FIRMWARE");
  app::ModuleReportText text;
  app::module_report_text(r.import->summary(), text);
  NEED(text.count >= 4);
  CHECK(std::string(text.line[1]) == "FAILED    IMPORTER FOR OTHER FIRMWARE");
  CHECK(std::string(text.line[2]) == "          NEEDS MODULES/IMPORTERS/TRACKER.TMOD");
  CHECK(std::string(text.line[3]) == "          FROM THE STARTER ZIP OF THIS RELEASE");
  // a kind 3 file of the wrong ABI minor is the same refusal
  Rig q(false);
  q.import->set_module_host(&q.mset, &q.mloader, nullptr, "tracker");
  q.card.add_file("MOD/kc_test.mod", mod_bytes());
  module::test::Blob old = module::test::blob_importer();
  old.h.abi_minor = 10;  // the kind exists from 1.11: an unknown kind for the header
  old.seal();
  q.card.add_file("modules/importers/tracker.tmod", old.file());
  CHECK(q.import->begin(q.card, "MOD/kc_test.mod", *q.arena, ImportEngine::Auto) == ModuleImport::Start::Failed);
  CHECK(q.import->state() == ModuleImport::State::Failed);
  CHECK(std::string(q.import->summary().error) == "IMPORTER FOR OTHER FIRMWARE");
}

// OS-FLASHDIET I3 review: begin() with no module host at all (set_module_host never called) is refused at the top, even when the card holds a
// valid modules/importers/tracker.tmod whose header check would pass: NO IMPORTER at begin, the document untouched (the first step would have
// dereferenced the null host).
TEST_CASE("module import (OS-FLASHDIET I3): no module host: NO IMPORTER at begin even with tracker.tmod on the card, the document untouched") {
  Rig r(false);  // no set_module_host: modules_ and loader_ stay null
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  r.card.add_file("modules/importers/tracker.tmod", module::test::blob_importer().file());  // a valid kind 3 file: the header check would pass
  NEED(r.arena->project.create_pattern(1, 32) == model::Status::Ok);
  NEED(r.arena->project.append_order(1) == model::Status::Ok);
  const std::uint32_t generation = r.arena->project.generation();
  CHECK(r.import->begin(r.card, "MOD/kc_test.mod", *r.arena, ImportEngine::Auto) == ModuleImport::Start::Failed);
  CHECK(r.import->state() == ModuleImport::State::Failed);
  CHECK_FALSE(r.import->document_touched());
  CHECK(r.arena->project.generation() == generation);
  CHECK(r.arena->project.song_length() == 1);
  CHECK(r.arena->project.pattern(1) != nullptr);
  CHECK(std::string(r.import->summary().error) == "NO IMPORTER");
  char line[64];
  app::module_strip_text(r.import->summary(), line, sizeof line);
  CHECK(std::string(line).find("FAILED: NO IMPORTER") != std::string::npos);  // (the refusal is at the top of begin(), before name_from: no name on the strip)
}

// OS-FLASHDIET I2 (3.6): no room in the module region can only be found after the reset: the import fails with NO MODULE MEMORY and leaves the document
// empty (valid), as any failure after the first step does.
TEST_CASE("module import (OS-FLASHDIET I2): no room in the module region: NO MODULE MEMORY after the reset, the document empty") {
  Rig r(false);
  r.import->set_module_host(&r.mset, &r.mloader, nullptr, "tracker");
  r.cold.assign(128 + 64, 0);  // the importer's cold part does not fit the 128 aligned bytes
  r.mset.cold().init(align_up<64>(r.cold.data()), Rig::kColdAddr, 128);
  r.card.add_file("MOD/kc_test.mod", mod_bytes());
  r.card.add_file("modules/importers/tracker.tmod", module::test::blob_importer().file());  // a valid kind 3 file: the header check passes
  NEED(r.arena->project.create_pattern(1, 32) == model::Status::Ok);
  NEED(r.arena->project.append_order(1) == model::Status::Ok);
  CHECK(r.import->begin(r.card, "MOD/kc_test.mod", *r.arena, ImportEngine::Auto) == ModuleImport::Start::Started);
  CHECK(r.import->state() == ModuleImport::State::Running);
  CHECK_FALSE(r.import->document_touched());
  (void)r.import->step(1);  // the first step: the document is reset, then the load finds no room
  CHECK(r.import->document_touched());
  for (unsigned i = 0; i < 2000000 && r.import->state() == ModuleImport::State::Running; ++i) {
    (void)r.import->step(16384);
  }
  CHECK(r.import->state() == ModuleImport::State::Failed);
  CHECK(r.arena->project.song_length() == 0);  // empty, valid
  CHECK(std::string(r.import->summary().error) == "NO MODULE MEMORY");
  char line[64];
  app::module_strip_text(r.import->summary(), line, sizeof line);
  CHECK(std::string(line) == "IMPORT KC_TEST.MOD FAILED: NO MODULE MEMORY");
  CHECK(r.mset.importer_count() == 0);  // nothing is left loaded
}

// OS-FLASHDIET I2 (3.6): the proxy's unload and reload order through the module: close() unloads the importer (its close() and shutdown(), the state
// block, the arenas back to the marks taken before the load), and the next import begins it again at the first step after Settle and writes the same
// project. (A card-loaded importer is dropped from the set at close() too; the QEMU suite checks that on the real tracker.tmod.)
TEST_CASE("module import (OS-FLASHDIET I2): close() unloads the importer, the next import reloads it and writes the same project") {
  Rig a;  // the module path (the tracker host object as a native importer entry)
  a.card.add_file("MOD/kc.mod", mod_bytes());
  NEED(a.run("MOD/kc.mod") == ModuleImport::State::Done);
  const Bytes first = native_project_bytes(a.arena->project);
  CHECK(a.mset.importer_count() == 1);  // loaded at the first step after Settle
  a.import->close();
  CHECK(a.mset.importer_count() == 1);  // a native entry stays registered (only a card-loaded importer is dropped, see unload_importer)
  NEED(a.run("MOD/kc.mod") == ModuleImport::State::Done);
  CHECK(a.mset.importer_count() == 1);  // the second import began it again
  CHECK(native_project_bytes(a.arena->project) == first);
}
