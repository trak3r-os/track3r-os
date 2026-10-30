// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012d: a built-in module (module/builtin.h) and a file of the same stem on the card. The rule (loader.h): the card's copy is used when it is valid and NEWER (the same
// kind and ABI major, a module version in the header, ABI 1.2, above the built-in one's; OS-MODVER), and otherwise, or with no card, the built-in copy is the fallback, loaded
// through the same checks into the same arenas, with the card's file not quarantined. Hand-made .tmod blobs (support/module_blob.h) so that one field at a time can be changed; the real
// modules' loads and renders are module_builtin_run_test.cpp. Host only (a card in a temporary directory).
#include <doctest/doctest.h>

#include "support/module_blob.h"
#include "support/need.h"
#include "support/null_storage.h"

#include <o3os/app/module_binder.h>
#include <o3os/app/module_ui.h>
#include <o3os/audio/module_voices.h>
#include <o3os/model/project.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/dir_storage.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace o3os::module;
using namespace o3os::module::test;
namespace fs = std::filesystem;

struct Rig {
  fs::path root;
  std::vector<std::uint8_t> flash;  // the built-in file's bytes (the table points here)
  BuiltinModule table[1];
  o3os::platform::host::DirStorage card;
  ModuleSet set;
  ModuleLoader loader;
  alignas(64) std::uint8_t slab[1024];
  alignas(64) std::uint8_t sdram[4096];

  static fs::path make_root() {
    static unsigned n = 0;
    const fs::path p = fs::temp_directory_path() / ("o3m_builtin_test_" + std::to_string(++n) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(&n)));
    fs::remove_all(p);
    fs::create_directories(p / "modules" / "instruments");
    return p;
  }
  explicit Rig(const std::vector<std::uint8_t>& builtin_file) : root(make_root()), flash(builtin_file), table{{"test", flash.data(), static_cast<std::uint32_t>(flash.size())}}, card(root.string()) {
    (void)card.mount();
    set.set_builtins(table, 1);
    set.hot().init(slab, kHotAddr, sizeof slab);
    set.cold().init(sdram, kColdAddr, sizeof sdram);
  }
  ~Rig() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  void write(const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(root / "modules" / "instruments" / "test.tmod", std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  ModuleLoader::State run(std::size_t budget = 4096) {
    if (!loader.begin(card, set, "test", nullptr)) {
      return ModuleLoader::State::Failed;
    }
    for (unsigned n = 0; n < 200000 && loader.state() == ModuleLoader::State::Running; ++n) {
      (void)loader.step(budget);
    }
    return loader.state();
  }
  const LoadedModule* loaded() const { return set.at(0); }
};

}  // namespace

TEST_CASE("built-in: with no card file the built-in module loads, the same as a card's file would (same arenas, same placement)") {
  const std::vector<std::uint8_t> file = blob_minor(kBlobMinor).file();
  Rig r(file);
  NEED(r.run() == ModuleLoader::State::Done);
  const LoadedModule* m = r.loaded();
  NEED(m != nullptr);
  CHECK(m->builtin);
  CHECK(m->card_status == Status::NotFound);
  CHECK(!m->hot_in_sdram);
  CHECK(m->place.hot_addr == kHotAddr);
  CHECK(m->place.cold_addr == kColdAddr);
  CHECK(r.set.hot().used() == kHot);
  CHECK(r.set.cold().used() == kImage - kHot);
  CHECK(r.loader.read_bytes() == r.loader.file_bytes());
  // the same module from a card file: the same bytes in the same places
  Rig c(file);
  c.table[0].stem = "other";
  c.write(file);
  NEED(c.run() == ModuleLoader::State::Done);
  CHECK(!c.loaded()->builtin);
  CHECK(std::memcmp(c.loaded()->place.hot_mem, m->place.hot_mem, kHot) == 0);
  CHECK(std::memcmp(c.loaded()->place.cold_mem, m->place.cold_mem, kImage - kHot) == 0);
}

TEST_CASE("built-in: whatever the step size, and with a card that is not there at all") {
  const std::vector<std::uint8_t> file = blob_minor(kBlobMinor).file();
  for (std::size_t budget : {std::size_t{1}, std::size_t{13}, std::size_t{4096}}) {
    Rig r(file);
    CAPTURE(budget);
    NEED(r.run(budget) == ModuleLoader::State::Done);
    CHECK(r.loaded()->builtin);
  }
  Rig r(file);
  o3os::test::NullStorage none;  // no card layer answering at all (NoCard / NotMounted)
  NEED(r.loader.begin(none, r.set, "test", nullptr));
  while (r.loader.state() == ModuleLoader::State::Running) {
    (void)r.loader.step(512);
  }
  CHECK(r.loader.state() == ModuleLoader::State::Done);
  CHECK(r.loaded()->builtin);
}

TEST_CASE("built-in: a card copy with a higher module version wins (an update without flashing), by any one of the three numbers") {
  const std::uint32_t v100 = O3M_VERSION(1, 0, 0);
  for (std::uint32_t newer : {O3M_VERSION(1, 0, 1), O3M_VERSION(1, 1, 0), O3M_VERSION(2, 0, 0), O3M_VERSION(1, 0, 255)}) {
    CAPTURE(newer);
    Rig r(blob_version(v100).file());
    r.write(blob_version(newer).file());
    NEED(r.run(64) == ModuleLoader::State::Done);
    NEED(r.loaded() != nullptr);
    CHECK(!r.loaded()->builtin);
    CHECK(r.loaded()->card_status == Status::Ok);
    CHECK(r.loaded()->header.module_version == newer);  // what the loader read from the card's file
  }
  Rig un(blob_minor(1).file());  // an unversioned built-in module (built for ABI 1.1): any version is newer than none
  un.write(blob_version(O3M_VERSION(0, 0, 1)).file());
  NEED(un.run() == ModuleLoader::State::Done);
  CHECK(!un.loaded()->builtin);
}

TEST_CASE("built-in: a card copy that is not newer (older, equal, unversioned), of another kind or ABI-incompatible is ignored, the built-in module is used and nothing is quarantined") {
  struct Case {
    const char* what;
    Blob blob;
    Status why;
    std::uint32_t card_version;  // with Older: what the loader says it passed over
  };
  const std::uint32_t v100 = O3M_VERSION(1, 0, 0);
  Blob effect = blob_version(O3M_VERSION(2, 0, 0));
  effect.h.kind = O3M_KIND_EFFECT;
  effect.put(kDesc + 8, O3M_KIND_EFFECT);  // Desc32::kind
  effect.seal();
  Blob major = blob_version(O3M_VERSION(2, 0, 0));
  major.h.abi_major = 2;
  major.seal();
  Blob newer_than_os = blob_version(O3M_VERSION(2, 0, 0));
  newer_than_os.h.abi_minor = 11;
  newer_than_os.seal();
  const Case cases[] = {
      {"older", blob_version(O3M_VERSION(0, 9, 9)), Status::Older, O3M_VERSION(0, 9, 9)},
      {"equal: a tie is the same module", blob_version(v100), Status::Older, v100},
      {"unversioned (ABI 1.2, version 0)", blob_version(0), Status::Older, 0},
      {"unversioned (ABI 1.1: no version word)", blob_minor(1), Status::Older, 0},
      {"another kind, whatever its version", effect, Status::Older, O3M_VERSION(2, 0, 0)},
      {"another major", major, Status::AbiMajor, 0},
      {"minor above the OS's", newer_than_os, Status::AbiMinor, 0},
  };
  for (const Case& c : cases) {
    CAPTURE(c.what);
    Rig r(blob_version(v100).file());
    r.write(c.blob.file());
    NEED(r.run(100) == ModuleLoader::State::Done);
    const LoadedModule* m = r.loaded();
    NEED(m != nullptr);
    CHECK(m->builtin);
    CHECK(m->card_status == c.why);
    CHECK(m->card_version == c.card_version);
    CHECK(m->header.module_version == v100);  // the loaded module is the built-in one
    CHECK(!m->hot_in_sdram);
    CHECK(!r.set.quarantined("test"));
    CHECK(r.set.hot().used() == kHot);  // the arenas hold the built-in copy only
    CHECK(r.set.cold().used() == kImage - kHot);
  }
  CHECK(std::string(status_name(Status::Older)) == "not newer than the built-in module");
}

TEST_CASE("built-in: prefers_card is a strict order of module versions, within one kind and one ABI major") {
  const auto head = [](std::uint8_t kind, std::uint8_t major, std::uint32_t version) {
    o3m_file_header x{};
    x.kind = kind;
    x.abi_major = major;
    x.module_version = version;
    return x;
  };
  const o3m_file_header built_in = head(O3M_KIND_INSTRUMENT, 1, O3M_VERSION(1, 2, 3));
  const auto card = [&](std::uint32_t version) { return prefers_card(head(O3M_KIND_INSTRUMENT, 1, version), built_in); };
  CHECK(card(O3M_VERSION(1, 2, 4)));
  CHECK(card(O3M_VERSION(1, 3, 0)));
  CHECK(card(O3M_VERSION(2, 0, 0)));
  CHECK(!card(O3M_VERSION(1, 2, 3)));    // equal: the built-in module stays
  CHECK(!card(O3M_VERSION(1, 2, 2)));
  CHECK(!card(O3M_VERSION(1, 1, 255)));  // a lower minor is lower whatever its patch
  CHECK(!card(O3M_VERSION(0, 255, 255)));
  CHECK(!card(0));                       // an unversioned card copy never replaces a versioned built-in module
  CHECK(!prefers_card(head(O3M_KIND_EFFECT, 1, O3M_VERSION(9, 0, 0)), built_in));      // another kind
  CHECK(!prefers_card(head(O3M_KIND_INSTRUMENT, 2, O3M_VERSION(9, 0, 0)), built_in));  // another ABI major
  const o3m_file_header unversioned = head(O3M_KIND_INSTRUMENT, 1, 0);
  CHECK(prefers_card(head(O3M_KIND_INSTRUMENT, 1, 1), unversioned));  // anything beats an unversioned built-in module
  CHECK(!prefers_card(unversioned, unversioned));                     // two unversioned files: the built-in one stays
}

TEST_CASE("built-in: the TB-303, chip, FM and SID are version 1.0.0 files of ABI 1.8 (the packer's --version, from the CMake rule; pitch, pages and labels need 1.8)") {
  NEED(kBuiltinModuleCount == 4u);
  for (unsigned i = 0; i < kBuiltinModuleCount; ++i) {
    CAPTURE(kBuiltinModules[i].stem);
    const o3m_file_header h = builtin_header(kBuiltinModules[i]);
    CHECK(check_header(h, kBuiltinModules[i].size) == Status::Ok);
    CHECK(h.abi_minor == 8);
    CHECK(h.module_version == O3M_VERSION(1, 0, 0));
  }
}

TEST_CASE("built-in: a corrupted card copy, at any point of the load, falls back to the built-in module with the arenas given back") {
  const std::vector<std::uint8_t> built_in = blob_version(O3M_VERSION(1, 0, 0)).file();
  const std::vector<std::uint8_t> good = blob_version(O3M_VERSION(1, 1, 0)).file();  // newer than the built-in one: the load goes on to the damaged part
  // a flipped byte in the header (the ABI minor, the module version, a reserved word), the image and the relocation table, and a truncated file
  const std::size_t where[] = {10, 36, 40, 80, sizeof(o3m_file_header) + 5, sizeof(o3m_file_header) + kImage - 3, good.size() - 6};
  for (std::size_t at : where) {
    CAPTURE(at);
    std::vector<std::uint8_t> bad = good;
    bad[at] = static_cast<std::uint8_t>(bad[at] ^ 0x55u);
    Rig r(built_in);
    r.write(bad);
    NEED(r.run(7) == ModuleLoader::State::Done);
    NEED(r.loaded() != nullptr);
    CHECK(r.loaded()->builtin);
    CHECK(r.loaded()->card_status != Status::Ok);
    CHECK(r.loaded()->card_status != Status::NotFound);
    CHECK(r.loaded()->card_status != Status::Older);  // it was the damage that was found, not the version
    CHECK(!r.set.quarantined("test"));
    CHECK(r.set.hot().used() == kHot);
    CHECK(r.set.cold().used() == kImage - kHot);
  }
  for (std::size_t cut : {std::size_t{0}, std::size_t{20}, sizeof(o3m_file_header) + 50, good.size() - 4}) {
    CAPTURE(cut);
    Rig r(built_in);
    r.write(std::vector<std::uint8_t>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut)));
    NEED(r.run(512) == ModuleLoader::State::Done);
    CHECK(r.loaded()->builtin);
    CHECK(r.set.hot().used() == kHot);
  }
}

TEST_CASE("built-in: a stem with no built-in copy behaves as before, and a broken built-in copy is refused and quarantined like any file") {
  Rig none(blob_minor(1).file());
  none.table[0].stem = "elsewhere";
  none.write(std::vector<std::uint8_t>(100, 7));
  CHECK(none.run() == ModuleLoader::State::Failed);
  CHECK(none.loader.status() == Status::BadMagic);
  CHECK(none.set.quarantined("test"));
  Rig missing(blob_minor(1).file());
  missing.table[0].stem = "elsewhere";
  CHECK(missing.run() == ModuleLoader::State::Failed);
  CHECK(missing.loader.status() == Status::NotFound);
  std::vector<std::uint8_t> broken = blob_minor(1).file();
  broken[sizeof(o3m_file_header) + 16] ^= 1u;  // the built-in copy itself fails its CRC (a byte no relocation looks at)
  Rig bad(broken);
  CHECK(bad.run() == ModuleLoader::State::Failed);
  CHECK(bad.loader.status() == Status::BadCrc);
  CHECK(bad.set.quarantined("test"));
  CHECK(bad.set.hot().used() == 0);
  CHECK(bad.set.cold().used() == 0);
}

TEST_CASE("built-in: cancel() of a running built-in load gives the arenas back and does not restart it") {
  Rig r(blob_minor(1).file());
  NEED(r.loader.begin(r.card, r.set, "test", nullptr));
  (void)r.loader.step(64);  // open: not on the card, the built-in copy takes over
  (void)r.loader.step(64);  // its header
  (void)r.loader.step(64);  // some of the image
  r.loader.cancel();
  CHECK(r.loader.state() == ModuleLoader::State::Idle);
  CHECK(r.set.hot().used() == 0);
  CHECK(r.set.cold().used() == 0);
  CHECK(r.set.count() == 0);
}

TEST_CASE("built-in: the ENGINE picker lists the TB-303, chip, FM and SID with no card and with an empty modules/ directory, and a card file of the same stem is one entry") {
  NEED(kBuiltinModuleCount == 4u);
  static o3os::audio::ModuleVoices voices;
  static o3os::app::ModuleUi ui;
  ModuleSet set;
  set.set_builtins(kBuiltinModules, kBuiltinModuleCount);
  ModuleLoader loader;
  ui.attach(set, loader, voices);
  const auto names = [&] {
    std::string out;
    for (unsigned i = 0; i < ui.instrument_count(); ++i) {
      out += std::string(i != 0 ? " " : "") + ui.instrument_at(i)->stem + (ui.instrument_at(i)->state == o3os::app::CatalogEntry::State::Ready ? "" : "!");
    }
    return out;
  };
  ui.scan_instruments(nullptr);  // no card layer at all
  CHECK(names() == "chip fm sid tb303");
  o3os::test::NullStorage none;
  ui.set_storage(&none);  // a card layer with no card in the slot
  ui.scan_instruments(nullptr);
  CHECK(names() == "chip fm sid tb303");
  Rig r(blob_minor(1).file());  // a mounted card with an empty modules/instruments/
  ui.set_storage(&r.card);
  ui.scan_instruments(nullptr);
  CHECK(names() == "chip fm sid tb303");
  std::ofstream(r.root / "modules" / "instruments" / "tb303.tmod", std::ios::binary) << "x";  // the card's copy (any content: the picker only lists)
  std::ofstream(r.root / "modules" / "instruments" / "acid.tmod", std::ios::binary) << "x";
  ui.scan_instruments(nullptr);
  CHECK(names() == "acid chip fm sid tb303");
  CHECK(!set.quarantined("tb303"));
}

TEST_CASE("built-in: OS-BUG-ONCARD the ENGINE and MODULES lists say BUILT IN for a module that lives only in the image and ON CARD for one that has a card file") {
  using o3os::app::catalog_ready_text;
  static o3os::audio::ModuleVoices voices;
  static o3os::app::ModuleUi ui;
  ModuleSet set;
  set.set_builtins(kBuiltinModules, kBuiltinModuleCount);
  ModuleLoader loader;
  ui.attach(set, loader, voices);
  const auto labels = [&] {
    std::string out;
    for (unsigned i = 0; i < ui.instrument_count(); ++i) {
      out += std::string(i != 0 ? " " : "") + ui.instrument_at(i)->stem + "=" + catalog_ready_text(*ui.instrument_at(i));
    }
    return out;
  };
  ui.scan_instruments(nullptr);  // no card: the four are the image's
  CHECK(labels() == "chip=BUILT IN fm=BUILT IN sid=BUILT IN tb303=BUILT IN");
  Rig r(blob_minor(1).file());
  ui.set_storage(&r.card);
  std::ofstream(r.root / "modules" / "instruments" / "tb303.tmod", std::ios::binary) << "x";  // a card copy of a built-in stem, and a card-only module
  std::ofstream(r.root / "modules" / "instruments" / "acid.tmod", std::ios::binary) << "x";
  ui.scan_instruments(nullptr);
  CHECK(labels() == "acid=ON CARD chip=BUILT IN fm=BUILT IN sid=BUILT IN tb303=ON CARD");
  CHECK(std::strlen(catalog_ready_text(*ui.instrument_at(0))) <= 8u);  // the style guide's width
  CHECK(std::strlen(catalog_ready_text(*ui.instrument_at(1))) <= 8u);
}

TEST_CASE("built-in: a project that names the TB-303 and a module nobody has opens with no card: the first is loaded from the image, the other is reported") {
  using namespace o3os;
  static model::Project project;
  project.reset();
  CHECK(project.add_module("tb303", 5, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);  // the project names the module by its stem, as always
  CHECK(project.add_module("ghost", 5, 1, 0) == model::Status::Ok);
  for (unsigned i = 0; i < 2; ++i) {
    model::Instrument in;
    std::memset(&in, 0, sizeof in);
    model::init_instrument(in, model::InstrumentKind::Module);
    in.module_index = static_cast<std::uint8_t>(i);
    CHECK(project.set_instrument(i, in) == model::Status::Ok);
  }
  alignas(64) static std::uint8_t scratch[audio::ModuleVoices::kScratchBytes];
  alignas(64) static std::uint8_t slab[8192];
  alignas(64) static std::uint8_t sdram[32768];
  static audio::ModuleVoices voices;
  NEED(voices.attach_scratch(scratch, sizeof scratch));
  static ModuleSet set;
  set.clear();
  set.set_builtins(kBuiltinModules, kBuiltinModuleCount);
  set.hot().init(slab, kHotAddr, sizeof slab);
  set.cold().init(sdram, kColdAddr, sizeof sdram);
  static ModuleLoader loader;
  o3os::test::NullStorage none;
  app::ModuleBinder binder;
  NEED(binder.begin(none, project, set, loader, voices, nullptr));
  for (unsigned n = 0; n < 1000 && binder.state() == app::ModuleBinder::State::Running; ++n) {
    binder.step(256);
  }
  CHECK(binder.state() == app::ModuleBinder::State::Done);
  const app::ModuleReport& rep = binder.report();
  CHECK(rep.instruments == 2);
  CHECK(rep.loaded == 1);  // tb303, from flash
  NEED(set.find("tb303") >= 0);
  CHECK(set.at(static_cast<unsigned>(set.find("tb303")))->builtin);
  CHECK(rep.abi_differs == 0);
  CHECK(rep.muted >= 1);   // ghost: not on the card, not built in
  CHECK(!set.quarantined("tb303"));
  bool ghost_missing = false;
  for (unsigned i = 0; i < rep.problem_count; ++i) {
    ghost_missing = ghost_missing || (std::string(rep.problems[i].name) == "ghost" && rep.problems[i].kind == app::ModuleProblem::Kind::CardError);  // no card: the read fails, as it always did
  }
  CHECK(ghost_missing);
  // (On the host the module code cannot be called: its instrument is muted as "cannot run in this build", `bound` is 0. On the Cortex-M4F it is bound: module_builtin_run_test.)
  CHECK(rep.bound + rep.muted == 2);
  voices.retire_all();
  voices.release_all();
}
