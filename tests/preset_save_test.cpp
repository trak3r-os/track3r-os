// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MODPRESET P3: SaveService::save_preset, the .o3mp save on the card: modules/presets/<stem>/<name>.o3mp as a NewOnly save (never replaced, no Backup form, nothing removed), a taken name getting
// the next free "<name> 2" .. "<name> 99", the refusals that say why, and a power cut at every block of the save leaving the old state or the new file, never a damaged Target. Host only: the card is the
// FAT32 volume in memory of tests/save_fatfs_test.cpp.
#include <doctest/doctest.h>

#include "../platform/mk66/tests/mk66_storage_fakes.h"
#include "support/need.h"

#include <o3os/app/save_service.h>
#include <o3os/core/job.h>
#include <o3os/formats/preset.h>
#include <o3os/hal/storage.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/mk66/save/fat_storage_writer.h>
#include <o3os/platform/mk66/storage/fat_storage.h>

#include <cstdint>
#include <cstring>
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

// The save service over a fresh mount of the medium, with the job runner the app's passes would step.
struct Rig {
  Medium& medium;
  st::FatStorage fat;
  sv::FatStorageWriter writer;
  app::SaveService saves;
  core::JobRunner jobs;
  platform::host::FakeClock clock;

  explicit Rig(Medium& m) : medium(m), fat(m), writer(fat, m) {
    NEED(fat.mount() == hs::Status::Ok);
    saves.attach(fat, writer);
  }
  void pump() {
    for (unsigned i = 0; i < 8192u && jobs.running() != 0u; ++i) {
      clock.advance(2000);
      jobs.run(clock, 2000);
    }
  }
};

Bytes read_file(hs::Storage& s, const std::string& path) {
  Bytes out;
  hs::FileHandle f;
  if (s.open(path.c_str(), f) != hs::Status::Ok) {
    return out;
  }
  std::uint8_t buf[300];
  std::size_t got = 0;
  while (s.read(f, buf, sizeof buf, got) == hs::Status::Ok && got != 0u) {
    out.insert(out.end(), buf, buf + got);
  }
  s.close(f);
  return out;
}

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

}  // namespace

TEST_CASE("preset save: save_preset writes modules/presets/<stem>/<name>.o3mp as a NewOnly save; a taken name gets <name> 2; the refusals say why") {
  Medium medium{200'000};
  Fat32Builder fat(medium);
  fat.add_file(Fat32Builder::kRootCluster, "README.TXT", Bytes(40, 7));
  Rig rig(medium);
  const std::pair<const char*, float> ents[4] = {{"Cutoff", 0.9f}, {"Resonance", 0.1f}, {"Env Mod", 7.0f}, {"Decay", 1.5f}};
  const fp::Preset p = make_preset("tb303", fp::kKindInstrument, ents, 4);
  char final[40] = {};
  CHECK(rig.saves.save_preset(rig.jobs, p, "Acid", final, sizeof final) == app::SaveService::Start::Started);
  CHECK(std::string(final) == "Acid.o3mp");
  rig.pump();
  CHECK(rig.jobs.running() == 0u);
  const Bytes got = read_file(rig.fat, "modules/presets/tb303/Acid.o3mp");
  CHECK(got == preset_bytes(p));
  fp::Preset back{};
  fp::Report rep{};
  CHECK(fp::read(got.data(), got.size(), back, rep) == fp::Error::None);
  CHECK(std::string(back.module) == "tb303");
  CHECK(back.kind == fp::kKindInstrument);
  CHECK(back.count == 4u);
  CHECK(back.entries[2].value == 7.0f);
  CHECK_FALSE(exists(rig.fat, "modules/presets/tb303/Acid.o3mp.bak"));  // no Backup form exists
  CHECK(read_file(rig.fat, "README.TXT") == Bytes(40, 7));              // nothing outside modules/presets/ moved
  // A taken name: the next free "<name> 2" (the .pti library's rule); the first file is untouched.
  const fp::Preset q = make_preset("tb303", fp::kKindInstrument, ents, 2);
  char final2[40] = {};
  CHECK(rig.saves.save_preset(rig.jobs, q, "Acid", final2, sizeof final2) == app::SaveService::Start::Started);
  CHECK(std::string(final2) == "Acid 2.o3mp");
  rig.pump();
  CHECK(read_file(rig.fat, "modules/presets/tb303/Acid 2.o3mp") == preset_bytes(q));
  CHECK(read_file(rig.fat, "modules/presets/tb303/Acid.o3mp") == preset_bytes(p));
  // Another module's presets go to its own folder.
  const fp::Preset sid = make_preset("sid", fp::kKindInstrument, ents, 1);
  CHECK(rig.saves.save_preset(rig.jobs, sid, "Bass", final2, sizeof final2) == app::SaveService::Start::Started);
  rig.pump();
  CHECK(exists(rig.fat, "modules/presets/sid/Bass.o3mp"));
  // The names the guard refuses.
  char f3[40] = {};
  CHECK(rig.saves.save_preset(rig.jobs, p, "", f3, sizeof f3) == app::SaveService::Start::BadName);
  CHECK(rig.saves.save_preset(rig.jobs, p, "..", f3, sizeof f3) == app::SaveService::Start::BadName);
  CHECK(rig.saves.save_preset(rig.jobs, p, "CON", f3, sizeof f3) == app::SaveService::Start::BadName);
  CHECK(rig.saves.save_preset(rig.jobs, p, "a/b", f3, sizeof f3) == app::SaveService::Start::BadName);
  CHECK(rig.saves.save_preset(rig.jobs, p, std::string(31, 'x').c_str(), f3, sizeof f3) == app::SaveService::Start::BadName);
  // A module stem is_stem takes but the guard's valid_name refuses ("my+synth"): BadName up front, not NoName from the job.
  const fp::Preset plus = make_preset("my+synth", fp::kKindInstrument, ents, 1);
  CHECK(rig.saves.save_preset(rig.jobs, plus, "X", f3, sizeof f3) == app::SaveService::Start::BadName);
  // A non-finite value in the blob (the writer never writes one): its own reason, not BadName.
  fp::Preset nanp = make_preset("tb303", fp::kKindInstrument, ents, 2);
  nanp.entries[1].value = __builtin_nanf("");
  CHECK(rig.saves.save_preset(rig.jobs, nanp, "X", f3, sizeof f3) == app::SaveService::Start::BadValues);
  fp::Preset infp = make_preset("tb303", fp::kKindInstrument, ents, 2);
  infp.entries[0].value = __builtin_huge_valf();
  CHECK(rig.saves.save_preset(rig.jobs, infp, "X", f3, sizeof f3) == app::SaveService::Start::BadValues);
  // An empty parameter name in the module: the module's fault, BadValues, not BadName.
  fp::Preset empty_name = make_preset("tb303", fp::kKindInstrument, ents, 2);
  empty_name.entries[0].name[0] = '\0';
  CHECK(rig.saves.save_preset(rig.jobs, empty_name, "X", f3, sizeof f3) == app::SaveService::Start::BadValues);
  // Busy: a save in flight refuses another one.
  CHECK(rig.saves.save_preset(rig.jobs, p, "Busy", f3, sizeof f3) == app::SaveService::Start::Started);
  CHECK(rig.saves.save_preset(rig.jobs, p, "Busy2", f3, sizeof f3) == app::SaveService::Start::Busy);
  rig.pump();
  // No write layer, and a card that refuses writes.
  app::SaveService bare;
  CHECK(bare.save_preset(rig.jobs, p, "X", f3, sizeof f3) == app::SaveService::Start::NoWriter);
  rig.medium.protect = true;
  CHECK(rig.saves.save_preset(rig.jobs, p, "Locked", f3, sizeof f3) == app::SaveService::Start::NotWritable);
  rig.medium.protect = false;
}

TEST_CASE("preset save: a power cut at every block of the save leaves the old state or the new file, never a damaged Target") {
  const std::pair<const char*, float> ents[3] = {{"Cutoff", 0.9f}, {"Resonance", 0.1f}, {"Decay", 1.5f}};
  const fp::Preset p = make_preset("tb303", fp::kKindInstrument, ents, 3);
  const Bytes bytes = preset_bytes(p);
  const auto build = [](Medium& medium) {  // a card that also holds a user file and the module's own file
    Fat32Builder fat(medium);
    fat.add_file(Fat32Builder::kRootCluster, "README.TXT", Bytes(40, 7));
    const std::uint32_t mods = fat.make_dir(Fat32Builder::kRootCluster, "modules");
    const std::uint32_t inst = fat.make_dir(mods, "instruments");
    fat.add_file(inst, "tb303.tmod", Bytes(500, 9));
  };
  std::uint64_t blocks = 0;
  {
    Medium medium{200'000};
    build(medium);
    Rig rig(medium);
    char final[40];
    CHECK(rig.saves.save_preset(rig.jobs, p, "Acid", final, sizeof final) == app::SaveService::Start::Started);
    const std::uint64_t before = medium.blocks_written;
    rig.pump();
    blocks = medium.blocks_written - before;
  }
  CHECK(blocks > 4u);
  for (std::uint64_t cut = 0; cut <= blocks; ++cut) {
    INFO("power cut at block " << cut << " of " << blocks);
    Medium medium{200'000};
    build(medium);
    {
      Rig rig(medium);
      char final[40];
      CHECK(rig.saves.save_preset(rig.jobs, p, "Acid", final, sizeof final) == app::SaveService::Start::Started);
      medium.fail_writes_after = static_cast<std::uint32_t>(cut);
      rig.pump();  // the job runs into the cut and fails
    }
    medium.fail_writes_after = 0xFFFFFFFFu;
    Rig boot(medium);  // the next boot: a fresh mount of what the cut left
    CHECK(read_file(boot.fat, "README.TXT") == Bytes(40, 7));
    CHECK(read_file(boot.fat, "modules/instruments/tb303.tmod") == Bytes(500, 9));
    const Bytes got = read_file(boot.fat, "modules/presets/tb303/Acid.o3mp");
    if (exists(boot.fat, "modules/presets/tb303/Acid.o3mp")) {
      CHECK(got == bytes);  // the new file, whole
      fp::Preset back;
      fp::Report rep;
      CHECK(fp::read(got.data(), got.size(), back, rep) == fp::Error::None);
    } else {
      CHECK(got.empty());  // the old state: no file
    }
    CHECK_FALSE(exists(boot.fat, "modules/presets/tb303/Acid.o3mp.bak"));
    // The next save succeeds whatever was left (a stale Temp goes), under the next free name when the cut was after the rename.
    char final2[40] = {};
    CHECK(boot.saves.save_preset(boot.jobs, p, "Acid", final2, sizeof final2) == app::SaveService::Start::Started);
    boot.pump();
    CHECK(read_file(boot.fat, std::string("modules/presets/tb303/") + final2) == bytes);
    CHECK(read_file(boot.fat, "README.TXT") == Bytes(40, 7));
  }
}
