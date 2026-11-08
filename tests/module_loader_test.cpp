// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012a: the stepped loader (module/loader.h) over a card that is a host directory: a module is found under modules/ or modules/instruments/, comes out
// the same whatever the step size, the hot part goes to the slab (or, with no slab or no room, to SDRAM), a damaged or truncated file is refused with the
// arenas given back, and no single changed byte of a good file gets through. The relocation and descriptor rules themselves are module_image_test.cpp's.
#include <doctest/doctest.h>

// The build has no exceptions, so no REQUIRE: a failed precondition is recorded and ends the test case.
#define NEED(cond) \
  do {             \
    const bool need_ok_ = static_cast<bool>(cond); \
    CHECK(need_ok_); \
    if (!need_ok_) {  \
      return;         \
    }                 \
  } while (0)

#include "support/module_blob.h"

#include <o3os/module/loader.h>
#include <o3os/platform/host/dir_storage.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace o3os::module;
using namespace o3os::module::test;
namespace fs = std::filesystem;

// A card in a temporary directory.
struct Card {
  fs::path root;
  Card() {
    root = fs::temp_directory_path() / ("o3m_loader_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    fs::remove_all(root);
    fs::create_directories(root / "modules" / "instruments");
    fs::create_directories(root / "modules" / "effects");
  }
  ~Card() { std::error_code ec; fs::remove_all(root, ec); }
  void write(const std::string& rel, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(root / rel, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
};

struct Rig {
  Card card;
  o3os::platform::host::DirStorage storage{card.root.string()};
  ModuleSet set;
  ModuleLoader loader;
  alignas(64) std::uint8_t slab[1024];
  alignas(64) std::uint8_t sdram[4096];
  bool mounted;
  explicit Rig(bool with_slab = true, std::uint32_t sdram_size = sizeof sdram) {
    mounted = storage.mount() == o3os::hal::storage::Status::Ok;
    if (with_slab) {
      set.hot().init(slab, kHotAddr, sizeof slab);
    }
    set.cold().init(sdram, kColdAddr, sdram_size);
  }
  // Loads `stem` with `budget` bytes a step; the state it ends in.
  ModuleLoader::State run(const char* stem, std::size_t budget, unsigned* steps = nullptr) {
    unsigned n = 0;
    if (!loader.begin(storage, set, stem, nullptr)) {
      return ModuleLoader::State::Failed;
    }
    while (loader.state() == ModuleLoader::State::Running && n < 100000) {
      loader.step(budget);
      ++n;
    }
    if (steps != nullptr) {
      *steps = n;
    }
    return loader.state();
  }
};

}  // namespace

TEST_CASE("module loader: file stems") {
  CHECK(is_stem("tb303"));
  CHECK(is_stem("a"));
  CHECK(is_stem("Ladder v2"));
  CHECK(is_stem("12345678901234567890123456789012") == false);  // 32
  CHECK(is_stem("1234567890123456789012345678901"));            // 31
  CHECK(!is_stem(""));
  CHECK(!is_stem("."));
  CHECK(!is_stem(".."));
  CHECK(!is_stem("a/b"));
  CHECK(!is_stem("a\\b"));
  CHECK(!is_stem("c:x"));
  CHECK(!is_stem("a*"));
  CHECK(!is_stem("a\x01"));
  CHECK(!is_stem(nullptr));
}

TEST_CASE("module loader: a module loads whatever the step size, into the slab and the module region") {
  const Blob b = good_blob();
  for (std::size_t budget : {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{100}, std::size_t{4096}}) {
    Rig rig;
    NEED(rig.mounted);
    rig.card.write("modules/instruments/test.tmod", b.file());
    unsigned steps = 0;
    CAPTURE(budget);
    NEED(rig.run("test", budget, &steps) == ModuleLoader::State::Done);
    CHECK(rig.loader.status() == Status::Ok);
    CHECK(rig.set.count() == 1);
    const LoadedModule* m = rig.set.at(0);
    NEED(m != nullptr);
    CHECK(std::string(m->name) == "test");
    CHECK(!m->hot_in_sdram);
    CHECK(m->place.hot_addr == kHotAddr);
    CHECK(m->place.cold_addr == kColdAddr);
    CHECK(m->desc.name == moved(kHot + 104));
    CHECK(m->desc.param_count == 2);
    CHECK(std::string(reinterpret_cast<const char*>(m->place.cold_mem + (kHot + 104 - kHot))) == "Test");
    CHECK(rig.set.hot().used() == kHot);
    CHECK(rig.set.cold().used() == kImage - kHot);
    CHECK(rig.loader.read_bytes() == rig.loader.file_bytes());
    if (budget == 4096) {
      CHECK(steps <= 8);  // open, header, image, relocs, finish: a handful of slices
    }
    if (budget == 1) {
      CHECK(steps > kImage);  // a byte a slice: the work really is cut into slices
    }
  }
}

TEST_CASE("module loader: the file is found in modules/instruments/ first, then in modules/effects/") {
  const Blob b = good_blob();
  {
    Rig rig;
    rig.card.write("modules/effects/test.tmod", b.file());
    CHECK(rig.run("test", 4096) == ModuleLoader::State::Done);
  }
  {
    Rig rig;
    CHECK(rig.run("test", 4096) == ModuleLoader::State::Failed);
    CHECK(rig.loader.status() == Status::NotFound);
    CHECK(rig.set.count() == 0);
    CHECK(!rig.set.quarantined("test"));  // a missing file is not a bad file: it may be copied to the card later
  }
  {
    Rig rig;
    rig.card.write("modules/test.tmod", b.file());  // not a place the loader looks
    CHECK(rig.run("test", 4096) == ModuleLoader::State::Failed);
    CHECK(rig.loader.status() == Status::NotFound);
  }
  {
    Rig rig;
    Blob other = good_blob();
    other.image[0] ^= 0xFF;  // changes the CRC...
    other.seal();            // ... so seal it again: a different but valid file
    rig.card.write("modules/instruments/test.tmod", b.file());
    rig.card.write("modules/effects/test.tmod", other.file());
    NEED(rig.run("test", 4096) == ModuleLoader::State::Done);
    CHECK(rig.set.at(0)->place.hot_mem[0] == 0xA0);  // the instruments' one
  }
}

TEST_CASE("module loader: a file that fails validation is quarantined until reboot, the rest keep loading") {
  Rig rig;
  Blob bad = good_blob();
  bad.image[20] ^= 1;  // the CRC no longer matches
  rig.card.write("modules/instruments/bad.tmod", bad.file());
  rig.card.write("modules/instruments/good.tmod", good_blob().file());
  CHECK(rig.run("bad", 4096) == ModuleLoader::State::Failed);
  CHECK(rig.loader.status() == Status::BadCrc);
  CHECK(rig.set.quarantined("bad"));
  CHECK(rig.set.quarantine_count() == 1);
  CHECK(rig.set.quarantine_reason(0) == Status::BadCrc);
  CHECK(std::string(rig.set.quarantine_name(0)) == "bad");
  // not opened again, even after the file is mended (the card is not read at all)
  rig.card.write("modules/instruments/bad.tmod", good_blob().file());
  CHECK(!rig.loader.begin(rig.storage, rig.set, "bad", nullptr));
  CHECK(rig.loader.status() == Status::Quarantined);
  CHECK(rig.run("good", 4096) == ModuleLoader::State::Done);
  // clear() (a project change) keeps the quarantine; only a new ModuleSet (a reboot) forgets it
  rig.set.clear();
  CHECK(rig.set.quarantined("bad"));
  CHECK(rig.set.quarantine("bad", Status::Quarantined));  // twice is once
  CHECK(rig.set.quarantine_count() == 1);
  // the crash record's route: a stem named by the platform at boot
  CHECK(rig.set.quarantine("crashy", Status::Quarantined));
  CHECK(!rig.loader.begin(rig.storage, rig.set, "crashy", nullptr));
  CHECK(!rig.set.quarantine("a/b", Status::Quarantined));
  // a full list refuses more, and the ones in it stay
  for (unsigned i = 0; rig.set.quarantine_count() < ModuleSet::kMaxQuarantined; ++i) {
    CHECK(rig.set.quarantine(("q" + std::to_string(i)).c_str(), Status::BadMagic));
  }
  CHECK(!rig.set.quarantine("onemore", Status::BadMagic));
  CHECK(rig.set.quarantined("bad"));
  ModuleSet fresh;
  CHECK(!fresh.quarantined("bad"));
}

TEST_CASE("module loader: only a file that is wrong is quarantined, not a full arena or a card error") {
  {
    Rig rig(true, 64);
    rig.card.write("modules/instruments/big.tmod", good_blob().file());
    CHECK(rig.run("big", 4096) == ModuleLoader::State::Failed);
    CHECK(rig.loader.status() == Status::NoMemory);
    CHECK(!rig.set.quarantined("big"));
  }
  {
    Rig rig;
    CHECK(rig.run("nothing", 4096) == ModuleLoader::State::Failed);
    CHECK(!rig.set.quarantined("nothing"));
  }
  CHECK(is_validation_failure(Status::BadMagic));
  CHECK(is_validation_failure(Status::BadDescriptor));
  CHECK(is_validation_failure(Status::BadCrc));
  CHECK(!is_validation_failure(Status::NoMemory));
  CHECK(!is_validation_failure(Status::NotFound));
  CHECK(!is_validation_failure(Status::Io));
  CHECK(!is_validation_failure(Status::Quarantined));
  CHECK(fault_id("tb303") != 0);
  CHECK((fault_id("tb303") & 0x8000u) != 0);
  CHECK(fault_id("tb303") == fault_id("tb303"));
  CHECK(fault_id("tb303") != fault_id("sine"));
}

TEST_CASE("module loader: the hot part falls back to SDRAM when the slab is missing or too small") {
  const Blob b = good_blob();
  for (int variant = 0; variant < 2; ++variant) {
    Rig rig(variant == 1);
    if (variant == 1) {
      rig.set.hot().init(rig.slab, kHotAddr, 16);  // 16 bytes: the 32-byte hot part does not fit
    }
    rig.card.write("modules/instruments/test.tmod", b.file());
    NEED(rig.run("test", 4096) == ModuleLoader::State::Done);
    const LoadedModule* m = rig.set.at(0);
    CHECK(m->hot_in_sdram);
    CHECK(rig.set.hot().used() == 0);
    CHECK(rig.set.cold().used() == kImage);
    // one run of memory: hot first, the cold part right behind, every address consistent
    CHECK(m->place.cold_addr == m->place.hot_addr + kHot);
    CHECK(m->place.cold_mem == m->place.hot_mem + kHot);
    CHECK(m->desc.name == m->place.hot_addr + kHot + 104);  // link 136 -> hot_addr + 128: the image contiguous
    CHECK(m->desc.init == m->place.hot_addr + 1);
  }
}

TEST_CASE("module loader: no room gives the arenas back") {
  const Blob b = good_blob();
  Rig rig(true, 64);  // 64 bytes of SDRAM: the 160-byte cold part does not fit
  rig.card.write("modules/instruments/test.tmod", b.file());
  CHECK(rig.run("test", 4096) == ModuleLoader::State::Failed);
  CHECK(rig.loader.status() == Status::NoMemory);
  CHECK(rig.set.hot().used() == 0);   // the hot part had been taken: given back
  CHECK(rig.set.cold().used() == 0);
  CHECK(rig.set.count() == 0);
}

TEST_CASE("module loader: refusals leave the arenas and the set as they were") {
  struct Case {
    const char* what;
    void (*mutate)(Blob&);
    Status want;
  };
  const Case cases[] = {
      {"bad magic", [](Blob& b) { b.h.magic = 0; b.seal(); }, Status::BadMagic},
      {"newer minor", [](Blob& b) { b.h.abi_minor = static_cast<std::uint8_t>(kOsAbiMinor + 1); b.seal(); }, Status::AbiMinor},
      {"another major", [](Blob& b) { b.h.abi_major = 2; b.seal(); }, Status::AbiMajor},
      {"crc", [](Blob& b) { b.h.image_crc ^= 1; }, Status::BadCrc},
      {"an image byte changed", [](Blob& b) { b.image[20] ^= 1; }, Status::BadCrc},
      {"a relocation changed", [](Blob& b) { b.relocs[2] = 52; }, Status::BadCrc},
      {"reloc out of range", [](Blob& b) { b.relocs.back() = kImage + 8; b.seal(); }, Status::BadRelocOffset},
      {"reloc not ascending", [](Blob& b) { b.relocs[5] = 4; b.seal(); }, Status::BadRelocOrder},
      {"reloc value outside", [](Blob& b) { b.put(48, 4000); b.seal(); }, Status::BadRelocValue},
      {"descriptor wrong", [](Blob& b) { b.put(kDesc + 32, 0); b.seal(); }, Status::BadDescriptor},
      {"descriptor disagrees with the header", [](Blob& b) { b.h.state_bytes = 32; b.seal(); }, Status::BadDescriptor},
  };
  for (const Case& c : cases) {
    Rig rig;
    Blob b = good_blob();
    c.mutate(b);
    rig.card.write("modules/instruments/bad.tmod", b.file());
    CAPTURE(std::string(c.what));
    CHECK(rig.run("bad", 64) == ModuleLoader::State::Failed);
    CHECK(rig.loader.status() == c.want);
    CHECK(rig.set.count() == 0);
    CHECK(rig.set.hot().used() == 0);
    CHECK(rig.set.cold().used() == 0);
    // and the loader is ready for the next
    rig.card.write("modules/instruments/test.tmod", good_blob().file());
    CHECK(rig.run("test", 64) == ModuleLoader::State::Done);
  }
}

TEST_CASE("module loader: a file cut at any length is refused, never half loaded") {
  const std::vector<std::uint8_t> full = good_blob().file();
  for (std::size_t cut = 0; cut < full.size(); ++cut) {
    Rig rig;
    rig.card.write("modules/instruments/cut.tmod", std::vector<std::uint8_t>(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut)));
    CAPTURE(cut);
    NEED(rig.run("cut", 100) == ModuleLoader::State::Failed);
    CHECK(rig.loader.status() != Status::Ok);
    CHECK(rig.set.count() == 0);
    CHECK(rig.set.cold().used() == 0);
  }
  // trailing bytes are as wrong as missing ones
  std::vector<std::uint8_t> longer = full;
  longer.push_back(0);
  Rig rig;
  rig.card.write("modules/instruments/long.tmod", longer);
  CHECK(rig.run("long", 100) == ModuleLoader::State::Failed);
  CHECK(rig.loader.status() == Status::BadFileLength);
}

TEST_CASE("module loader: no single changed byte of a good file gets through") {
  const std::vector<std::uint8_t> full = good_blob().file();
  unsigned accepted = 0;
  for (std::size_t i = 0; i < full.size(); ++i) {
    for (std::uint8_t flip : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      std::vector<std::uint8_t> bytes = full;
      bytes[i] = static_cast<std::uint8_t>(bytes[i] ^ flip);
      Rig rig;
      rig.card.write("modules/instruments/fuzz.tmod", bytes);
      if (rig.run("fuzz", 4096) == ModuleLoader::State::Done) {
        ++accepted;
      }
      CHECK(rig.set.count() == (rig.loader.state() == ModuleLoader::State::Done ? 1u : 0u));
    }
  }
  CHECK(accepted == 0);  // the CRC covers the header too
}

TEST_CASE("module loader: begin() refusals and cancel") {
  Rig rig;
  rig.card.write("modules/instruments/test.tmod", good_blob().file());
  CHECK(!rig.loader.begin(rig.storage, rig.set, "a/b", nullptr));
  CHECK(rig.loader.status() == Status::BadName);
  CHECK(!rig.loader.begin(rig.storage, rig.set, "", nullptr));
  NEED(rig.loader.begin(rig.storage, rig.set, "test", nullptr));
  CHECK(!rig.loader.begin(rig.storage, rig.set, "test", nullptr));  // one at a time
  CHECK(rig.loader.status() == Status::Busy);
  rig.loader.step(8);
  rig.loader.step(8);
  rig.loader.cancel();
  CHECK(rig.loader.state() == ModuleLoader::State::Idle);
  CHECK(rig.set.hot().used() == 0);
  CHECK(rig.set.cold().used() == 0);
  CHECK(rig.storage.open_files() == 0);
  CHECK(rig.run("test", 4096) == ModuleLoader::State::Done);
  CHECK(rig.storage.open_files() == 0);
}

TEST_CASE("module loader: the set holds each name once, up to kMaxModules, and clear() gives the arenas back") {
  Rig rig;
  rig.card.write("modules/instruments/test.tmod", good_blob().file());
  NEED(rig.run("test", 4096) == ModuleLoader::State::Done);
  CHECK(rig.set.find("test") == 0);
  CHECK(rig.set.find("nope") == -1);
  CHECK(rig.set.find(nullptr) == -1);
  // the same name again: the load is refused at the end (the arenas it took are given back)
  const std::uint32_t hot = rig.set.hot().used();
  const std::uint32_t cold = rig.set.cold().used();
  CHECK(rig.run("test", 4096) == ModuleLoader::State::Failed);
  CHECK(rig.set.hot().used() == hot);
  CHECK(rig.set.cold().used() == cold);
  rig.set.clear();
  CHECK(rig.set.count() == 0);
  CHECK(rig.set.hot().used() == 0);
  CHECK(rig.set.cold().used() == 0);
  CHECK(rig.run("test", 4096) == ModuleLoader::State::Done);
}

// OS-PERF3: an instrument's state block comes from the state arena (SRAM on the MK66) while it has room, then from the cold arena, and a failed bind gives back what it took from both.
TEST_CASE("module set: take_state prefers the state arena, falls back to the cold arena, and rolls both back") {
  alignas(64) static std::uint8_t state_mem[256];
  alignas(64) static std::uint8_t cold_mem[1024];
  ModuleSet set;
  NEED(set.cold().init(cold_mem, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(cold_mem)), sizeof cold_mem));
  std::uint8_t* mem = nullptr;
  std::uint32_t addr = 0;

  // No state arena: the cold one, as before.
  CHECK_FALSE(set.state().ready());
  const ModuleSet::StateMark before = set.state_mark();
  NEED(set.take_state(100, mem, addr));
  CHECK(mem >= cold_mem);
  CHECK(mem < cold_mem + sizeof cold_mem);
  CHECK(reinterpret_cast<std::uintptr_t>(mem) % 16u == 0u);
  set.state_rollback(before);
  CHECK(set.cold().used() == 0u);

  // With one: the block is in it, 16-byte aligned, and a second one follows.
  NEED(set.state().init(state_mem, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(state_mem)), sizeof state_mem));
  NEED(set.take_state(100, mem, addr));
  CHECK(mem >= state_mem);
  CHECK(mem < state_mem + sizeof state_mem);
  CHECK(reinterpret_cast<std::uintptr_t>(mem) % 16u == 0u);
  CHECK(addr == static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(mem)));
  CHECK(set.cold().used() == 0u);
  const std::uint8_t* first = mem;
  NEED(set.take_state(100, mem, addr));
  CHECK(mem >= first + 100);
  CHECK(mem + 100 <= state_mem + sizeof state_mem);

  // Full: the next one that does not fit goes to the cold arena; a mark taken before it takes both back.
  const ModuleSet::StateMark mark = set.state_mark();
  const std::uint32_t state_used = set.state().used();
  NEED(set.take_state(100, mem, addr));
  CHECK(mem >= cold_mem);
  CHECK(mem < cold_mem + sizeof cold_mem);
  CHECK(set.state().used() == state_used);
  CHECK(set.cold().used() >= 100u);
  set.state_rollback(mark);
  CHECK(set.cold().used() == 0u);
  CHECK(set.state().used() == state_used);

  // Neither has room.
  CHECK_FALSE(set.take_state(sizeof cold_mem + 1u, mem, addr));

  // clear() gives all three arenas back.
  set.clear();
  CHECK(set.state().used() == 0u);
  CHECK(set.cold().used() == 0u);
}

// ---- ABI 1.8 (OS-MODPITCH, OS-INSTUI S10): the descriptor's size is per declared minor --------------------------------------------------------------------

TEST_CASE("module loader: a 1.7 file (64-byte descriptor) and a 1.8 file (80) both load, whatever the step size, and the 1.8 members come with the module") {
  for (const std::size_t budget : {std::size_t{7}, std::size_t{64}, std::size_t{4096}}) {
    {
      Rig rig;
      rig.card.write("modules/instruments/old.tmod", good_blob().file());
      NEED(rig.run("old", budget) == ModuleLoader::State::Done);
      const LoadedModule* m = rig.set.at(0);
      NEED(m != nullptr);
      CHECK(m->header.abi_minor == 7);
      CHECK(m->desc.struct_size == 64);
      CHECK((m->ext.pitch == 0 && m->ext.pages == 0 && m->ext.page_count == 0 && m->ext.labels == 0));  // a 1.7 module has none
    }
    {
      Rig rig;
      rig.card.write("modules/instruments/new.tmod", blob18().file());
      NEED(rig.run("new", budget) == ModuleLoader::State::Done);
      const LoadedModule* m = rig.set.at(0);
      NEED(m != nullptr);
      CHECK(m->header.abi_minor == 8);
      CHECK(m->desc.struct_size == 80);
      CHECK(m->ext.pitch != 0);
      CHECK(m->ext.page_count == 2);
      CHECK(m->ext.pages != 0);
      CHECK(m->ext.labels != 0);
    }
  }
}

TEST_CASE("module loader: the size is gated per minor: 64 bytes with minor 8, 80 with minor 7, a broken 1.8 table, are all refused with the arenas given back") {
  struct Case {
    const char* what;
    Blob blob;
    Status want;
  };
  Blob size64 = blob18();
  size64.put(Layout18::desc + 12, 64);
  size64.seal();
  Blob size80 = good_blob();
  size80.put(kDesc + 12, 80);
  size80.seal();
  Blob pitch_even = blob18();
  pitch_even.put(Layout18::desc + 64, 12);
  pitch_even.seal();
  Blob pages_no_count = blob18();
  pages_no_count.put(Layout18::desc + 72, 0);
  pages_no_count.seal();
  Blob big_label = blob18();
  big_label.put(Layout18::labels + 4, 17);
  big_label.seal();
  Blob minor9 = blob18();
  minor9.h.abi_minor = static_cast<std::uint8_t>(kOsAbiMinor + 1);
  minor9.seal();
  const Case cases[] = {{"size 64, minor 8", size64, Status::BadDescriptor},
                        {"size 80, minor 7", size80, Status::BadDescriptor},
                        {"pitch not Thumb", pitch_even, Status::BadDescriptor},
                        {"pages with no count", pages_no_count, Status::BadDescriptor},
                        {"17 labels", big_label, Status::BadDescriptor},
                        {"a minor newer than this OS", minor9, Status::AbiMinor}};
  for (const Case& c : cases) {
    Rig rig;
    rig.card.write("modules/instruments/bad.tmod", c.blob.file());
    CAPTURE(std::string(c.what));
    CHECK(rig.run("bad", 64) == ModuleLoader::State::Failed);
    CHECK(rig.loader.status() == c.want);
    CHECK(rig.set.count() == 0);
    CHECK(rig.set.hot().used() == 0);
    CHECK(rig.set.cold().used() == 0);
  }
}

namespace {

int n_init(void*, const o3m_host*) { return 0; }
std::uint32_t n_render(void*, const float*, float*, std::uint32_t) { return 0; }
void n_set_param(void*, std::uint32_t, float) {}
void n_note_on(void*, std::uint32_t, std::uint32_t, std::uint32_t) {}
void n_note_off(void*, std::uint32_t) {}
void n_shutdown(void*) {}
void n_pitch(void*, float, float) {}
const o3m_param kNParams[2] = {{"A", 0.0f, 1.0f, 0.0f, 0}, {"B", 0.0f, 1.0f, 0.0f, 0}};
const o3m_page kNPages[1] = {{"Page", 0}};

// A native module with the given minor, struct_size and 1.8 members.
o3m_module native(std::uint32_t minor, std::uint32_t size, bool pitch, const o3m_page* pages, std::uint32_t page_count) {
  o3m_module m = {O3M_ABI_MAJOR, minor, O3M_KIND_INSTRUMENT, size, "native", 16, 2, kNParams, n_init, n_render, n_set_param, n_note_on, n_note_off, n_shutdown, nullptr, 0};
  m.pitch = pitch ? &n_pitch : nullptr;
  m.pages = pages;
  m.page_count = page_count;
  return m;
}

}  // namespace

TEST_CASE("module set: a native entry's struct_size is per minor too (add_native), and its 1.8 table is shaped") {
  static ModuleSet set;
  set.clear();
  static const o3m_module ok17 = native(7, static_cast<std::uint32_t>(O3M_MODULE_SIZE(7)), false, nullptr, 0);
  static const o3m_module ok18 = native(8, static_cast<std::uint32_t>(O3M_MODULE_SIZE(8)), true, kNPages, 1);
  static const o3m_module bad17 = native(7, static_cast<std::uint32_t>(O3M_MODULE_SIZE(8)), false, nullptr, 0);   // an 80-byte struct that says 1.7
  static const o3m_module bad18 = native(8, static_cast<std::uint32_t>(O3M_MODULE_SIZE(7)), true, nullptr, 0);    // a 64-byte struct that says 1.8
  static const o3m_module pages_no_count = native(8, static_cast<std::uint32_t>(O3M_MODULE_SIZE(8)), true, kNPages, 0);
  static const o3m_module count_no_pages = native(8, static_cast<std::uint32_t>(O3M_MODULE_SIZE(8)), true, nullptr, 1);
  static const o3m_module five_pages = native(8, static_cast<std::uint32_t>(O3M_MODULE_SIZE(8)), true, kNPages, 5);
  CHECK(set.add_native("ok17", ok17));
  CHECK(set.add_native("ok18", ok18));
  CHECK(!set.add_native("bad17", bad17));
  CHECK(!set.add_native("bad18", bad18));
  CHECK(!set.add_native("nocount", pages_no_count));
  CHECK(!set.add_native("nopages", count_no_pages));
  CHECK(!set.add_native("five", five_pages));
  CHECK(set.count() == 2u);
  CHECK(set.at(0)->desc.struct_size == 64);
  CHECK(set.at(1)->desc.struct_size == 80);
}

// ---- ABI 1.12 (OS-INSFX-ABI2 A1, Level C): the loader and add_native take the fourth descriptor tail -------------------------------------------------------

TEST_CASE("module loader: a 1.12 effect loads whatever the step size, with the 100-byte descriptor; a bad tail is refused with the arenas given back") {
  for (const std::size_t budget : {std::size_t{7}, std::size_t{64}, std::size_t{4096}}) {
    Rig rig;
    rig.card.write("modules/effects/fx.tmod", blob_fx12().file());
    NEED(rig.run("fx", budget) == ModuleLoader::State::Done);
    const LoadedModule* m = rig.set.at(0);
    NEED(m != nullptr);
    CHECK(m->header.abi_minor == 12);
    CHECK(m->header.kind == O3M_KIND_EFFECT);
    CHECK(m->desc.struct_size == 100);
    CHECK(m->desc.param_count == 3);
  }
  struct Case {
    const char* what;
    Blob blob;
  };
  Blob widens_no_render = blob_fx12();
  widens_no_render.relocs.erase(std::remove(widens_no_render.relocs.begin(), widens_no_render.relocs.end(), LayoutFx::desc + 84u), widens_no_render.relocs.end());
  widens_no_render.put(LayoutFx::desc + 84, 0);
  widens_no_render.h.reloc_count = static_cast<std::uint32_t>(widens_no_render.relocs.size());
  widens_no_render.seal();
  Blob big_count = blob_fx12();
  big_count.put(LayoutFx::desc + 96, 33);
  big_count.seal();
  Blob nan_value = blob_fx12();
  nan_value.put(LayoutFx::values0, 0x7FC00000u);  // a quiet NaN
  nan_value.seal();
  Blob size84 = blob_fx12();
  size84.put(LayoutFx::desc + 12, 84);
  size84.seal();
  Blob minor13 = blob_fx12();
  minor13.h.abi_minor = static_cast<std::uint8_t>(kOsAbiMinor + 1);
  minor13.seal();
  const Case cases[] = {{"WIDENS without render_st", widens_no_render}, {"33 presets", big_count}, {"a NaN preset value", nan_value}, {"size 84 with minor 12", size84}};
  for (const Case& c : cases) {
    Rig rig;
    rig.card.write("modules/effects/bad.tmod", c.blob.file());
    CAPTURE(std::string(c.what));
    CHECK(rig.run("bad", 64) == ModuleLoader::State::Failed);
    CHECK(rig.loader.status() == Status::BadDescriptor);
    CHECK(rig.set.count() == 0);
    CHECK(rig.set.hot().used() == 0);
    CHECK(rig.set.cold().used() == 0);
    CHECK(rig.set.quarantined("bad"));  // a validation failure: quarantined until the reboot
  }
  {  // a 1.12 file under an OS of 1.11 would be refused by the minor rule: here the same file one minor above this OS's
    Rig rig;
    rig.card.write("modules/effects/new.tmod", minor13.file());
    CHECK(rig.run("new", 64) == ModuleLoader::State::Failed);
    CHECK(rig.loader.status() == Status::AbiMinor);
  }
}

namespace {

std::uint32_t s_render_st(void*, const float*, const float*, float*, float*, std::uint32_t) { return 0; }
const o3m_param kFxParams[3] = {{"A", 0.0f, 1.0f, 0.5f, 0}, {"Key", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY}, {"B", -10.0f, 10.0f, 0.0f, 0}};
const float kV0[3] = {0.25f, 3.0f, -2.0f};
const float kV1[2] = {1.0f, 16.0f};
const float kVHigh[1] = {2.0f};
const float kVNan[1] = {std::numeric_limits<float>::quiet_NaN()};
const float kVInf[1] = {std::numeric_limits<float>::infinity()};
const o3m_preset kPresets[2] = {{"Warm", kV0, 3}, {"Wide", kV1, 2}};
const o3m_preset kPresetNoName[1] = {{nullptr, kV0, 3}};
const o3m_preset kPresetEmptyName[1] = {{"", kV0, 3}};
const o3m_preset kPresetLongName[1] = {{"0123456789ABCDEF", kV0, 3}};
const o3m_preset kPresetCtl[1] = {{"A\tB", kV0, 3}};
const o3m_preset kPresetNoValues[1] = {{"X", nullptr, 3}};
const o3m_preset kPresetZero[1] = {{"X", kV0, 0}};
const o3m_preset kPresetMany[1] = {{"X", kV0, 4}};  // four values for three parameters
const o3m_preset kPresetHigh[1] = {{"X", kVHigh, 1}};
const o3m_preset kPresetNan[1] = {{"X", kVNan, 1}};
const o3m_preset kPresetInf[1] = {{"X", kVInf, 1}};
const o3m_preset kPresetOne[1] = {{"A", kV0, 1}};
const o3m_preset kPresetMax[32] = {{"A", kV0, 1}, {"B", kV0, 1}, {"C", kV0, 1}, {"D", kV0, 1}, {"E", kV0, 1}, {"F", kV0, 1}, {"G", kV0, 1}, {"H", kV0, 1},
                                   {"I", kV0, 1}, {"J", kV0, 1}, {"K", kV0, 1}, {"L", kV0, 1}, {"M", kV0, 1}, {"N", kV0, 1}, {"O", kV0, 1}, {"P", kV0, 1},
                                   {"Q", kV0, 1}, {"R", kV0, 1}, {"S", kV0, 1}, {"T", kV0, 1}, {"U", kV0, 1}, {"V", kV0, 1}, {"W", kV0, 1}, {"X", kV0, 1},
                                   {"Y", kV0, 1}, {"Z", kV0, 1}, {"a", kV0, 1}, {"b", kV0, 1}, {"c", kV0, 1}, {"d", kV0, 1}, {"e", kV0, 1}, {"f", kV0, 1}};
// KEY shapes
const o3m_param kKeyRange8[1] = {{"Key", 0.0f, 8.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY}};
const o3m_param kKeyFloat[1] = {{"Key", 0.0f, 16.0f, 0.0f, O3M_PARAM_KEY}};
const o3m_param kKeyMin1[1] = {{"Key", 1.0f, 16.0f, 1.0f, O3M_PARAM_INT | O3M_PARAM_KEY}};
const o3m_param kKey2[2] = {{"K1", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY}, {"K2", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY}};
const o3m_param kKey3[3] = {{"K1", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY}, {"K2", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY},
                            {"K3", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY}};
const o3m_labels kLabelsKey[1] = {{nullptr, 2}};  // a key with labels (the table says it has two)
const char* const kNames2[2] = {"No", "Yes"};
const o3m_labels kLabelsWithNames[1] = {{kNames2, 2}};

// A native effect of ABI 1.12 (the 100-byte descriptor) with the given tail; tests change the fields they break.
o3m_module fx_native(std::uint32_t kind = O3M_KIND_EFFECT) {
  o3m_module m = {O3M_ABI_MAJOR, kStereoAbiMinor, kind, static_cast<std::uint32_t>(O3M_MODULE_SIZE(12)), "fx", 16, 3, kFxParams, n_init, n_render, n_set_param, n_note_on, n_note_off, n_shutdown, nullptr, 0};
  m.render_st = &s_render_st;
  m.fx_flags = O3M_FX_WIDENS;
  m.presets = kPresets;
  m.preset_count = 2;
  return m;
}

}  // namespace

// ABI 1.13 (OS-MODRATE S2, Level C): the RATE bit of fx_flags is an instrument's, from minor 13 (the rule of read_desc, applied to a native entry).
TEST_CASE("module set: add_native takes O3M_FX_RATE on an instrument of 1.13 and refuses it on an effect, below 1.13 and with WIDENS") {
  static const o3m_param kPlain[2] = {{"A", 0.0f, 1.0f, 0.0f, 0}, {"B", 0.0f, 1.0f, 0.0f, 0}};
  const auto instrument = [](std::uint32_t minor, std::uint32_t flags) {
    o3m_module m = fx_native(O3M_KIND_INSTRUMENT);
    m.abi_minor = minor;
    m.render_st = nullptr;
    m.fx_flags = flags;
    m.params = kPlain;
    m.param_count = 2;
    m.presets = nullptr;
    m.preset_count = 0;
    return m;
  };
  static ModuleSet set;
  set.clear();
  static const o3m_module rate = instrument(13, O3M_FX_RATE);
  static const o3m_module plain = instrument(13, 0);
  CHECK(set.add_native("rate", rate));
  CHECK(set.add_native("plain", plain));
  CHECK(set.at(0)->entry == &rate);
  CHECK(set.count() == 2u);
  set.clear();
  CHECK_FALSE(set.add_native("old", instrument(12, O3M_FX_RATE)));  // the bit did not exist at 1.12
  CHECK_FALSE(set.add_native("wide", instrument(13, O3M_FX_WIDENS)));
  CHECK_FALSE(set.add_native("both", instrument(13, O3M_FX_RATE | O3M_FX_WIDENS)));
  CHECK_FALSE(set.add_native("other", instrument(13, 4u)));
  o3m_module effect = fx_native();
  effect.abi_minor = 13;
  effect.fx_flags = O3M_FX_RATE;
  CHECK_FALSE(set.add_native("fx", effect));
  effect.fx_flags = O3M_FX_WIDENS;
  CHECK(set.add_native("fx", effect));  // WIDENS is still an effect's at 1.13
}

TEST_CASE("module set: add_native takes a 1.12 entry (render_st, WIDENS, presets, a KEY) and applies the rules of read_desc to its tail") {
  static ModuleSet set;
  set.clear();
  const o3m_module ok = fx_native();
  CHECK(set.add_native("ok", ok));
  CHECK(set.at(0)->desc.struct_size == 100);
  CHECK(set.at(0)->entry == &ok);
  // shapes that load (the set keeps a pointer to each entry: they are static)
  static const o3m_module capable = [] {
    o3m_module m = fx_native();
    m.fx_flags = 0;  // stereo-capable: render_st, no WIDENS
    return m;
  }();
  static const o3m_module mono = [] {
    o3m_module m = fx_native();
    m.render_st = nullptr;
    m.fx_flags = 0;
    m.presets = nullptr;
    m.preset_count = 0;  // a plain mono effect at 1.12 (a KEY parameter may still be there)
    return m;
  }();
  static const o3m_param kPlain[2] = {{"A", 0.0f, 1.0f, 0.0f, 0}, {"B", 0.0f, 1.0f, 0.0f, 0}};
  static const float kVHalf[2] = {0.5f, 1.0f};
  static const o3m_preset kHalf[1] = {{"Half", kVHalf, 2}};
  static const o3m_module inst = [] {
    o3m_module m = fx_native(O3M_KIND_INSTRUMENT);  // presets on an instrument are allowed (Q8 a)
    m.render_st = nullptr;
    m.fx_flags = 0;
    m.params = kPlain;
    m.param_count = 2;
    m.presets = kHalf;
    m.preset_count = 1;
    return m;
  }();
  static const o3m_module two_keys = [] {
    o3m_module m = fx_native();
    m.params = kKey2;
    m.param_count = 2;
    m.presets = nullptr;
    m.preset_count = 0;
    return m;
  }();
  static const o3m_module max32 = [] {
    o3m_module m = fx_native();
    m.presets = kPresetMax;
    m.preset_count = 32;
    return m;
  }();
  static const o3m_module one = [] {
    o3m_module m = fx_native();
    m.presets = kPresetOne;
    m.preset_count = 1;
    return m;
  }();
  CHECK(set.add_native("capable", capable));
  CHECK(set.add_native("mono", mono));
  CHECK(set.add_native("inst", inst));
  CHECK(set.add_native("keys2", two_keys));
  CHECK(set.add_native("p32", max32));
  CHECK(set.add_native("p1", one));
  CHECK(set.count() == 7u);
  // every refusal (a fresh set each time: add_native must not have touched it)
  struct Case {
    const char* what;
    o3m_module m;
  };
  std::vector<Case> bad;
  const auto add = [&bad](const char* what, const o3m_module& m) { bad.push_back({what, m}); };
  {
    o3m_module m = fx_native(O3M_KIND_INSTRUMENT);
    m.params = kPlain;
    m.param_count = 2;
    m.presets = nullptr;
    m.preset_count = 0;
    add("render_st on an instrument", m);
    m.render_st = nullptr;
    m.presets = kPresetMany;  // an instrument's presets are checked like an effect's: a preset with more values than parameters
    m.preset_count = 1;
    m.fx_flags = 0;
    add("a preset of four values on a module with two parameters", m);
  }
  {
    o3m_module m = fx_native();
    m.fx_flags = 2;
    add("an unknown fx_flags bit", m);
  }
  {
    o3m_module m = fx_native();
    m.render_st = nullptr;
    add("WIDENS without render_st", m);
  }
  {
    o3m_module m = fx_native();
    m.presets = nullptr;
    add("a preset count with no table", m);
  }
  {
    o3m_module m = fx_native();
    m.preset_count = 0;
    add("a table with no preset count", m);
  }
  {
    o3m_module m = fx_native();
    m.presets = kPresetMax;
    m.preset_count = 33;
    add("33 presets", m);
  }
  {
    o3m_module m = fx_native();
    m.params = nullptr;
    add("params NULL with parameters", m);
  }
  for (const auto& [what, table] : std::initializer_list<std::pair<const char*, const o3m_preset*>>{{"a preset with no name", kPresetNoName},
                                                                                                    {"a preset with an empty name", kPresetEmptyName},
                                                                                                    {"a preset name of 16 characters", kPresetLongName},
                                                                                                    {"a preset name with a control character", kPresetCtl},
                                                                                                    {"a preset with no values", kPresetNoValues},
                                                                                                    {"a preset with a count of 0", kPresetZero},
                                                                                                    {"a preset with more values than parameters", kPresetMany},
                                                                                                    {"a value above its range", kPresetHigh},
                                                                                                    {"a NaN value", kPresetNan},
                                                                                                    {"an infinite value", kPresetInf}}) {
    o3m_module m = fx_native();
    m.presets = table;
    m.preset_count = 1;
    add(what, m);
  }
  for (const auto& [what, table, count] : std::initializer_list<std::tuple<const char*, const o3m_param*, std::uint32_t>>{
           {"a KEY of 0..8", kKeyRange8, 1}, {"a KEY that is not INT", kKeyFloat, 1}, {"a KEY of 1..16", kKeyMin1, 1}, {"three KEYs", kKey3, 3}}) {
    o3m_module m = fx_native();
    m.params = table;
    m.param_count = count;
    m.presets = nullptr;
    m.preset_count = 0;
    add(what, m);
  }
  {
    o3m_module m = fx_native(O3M_KIND_INSTRUMENT);
    m.render_st = nullptr;
    m.fx_flags = 0;
    m.params = kKey2;
    m.param_count = 2;
    m.presets = nullptr;
    m.preset_count = 0;
    add("a KEY on an instrument", m);
  }
  {
    o3m_module m = fx_native();
    m.params = kKeyRange8;  // placeholder replaced below: a KEY with a label entry
    static const o3m_param key17[1] = {{"Key", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY}};
    m.params = key17;
    m.param_count = 1;
    m.labels = kLabelsKey;
    m.presets = nullptr;
    m.preset_count = 0;
    add("a KEY with a label entry", m);
    m.labels = kLabelsWithNames;
    add("a KEY with labels", m);
  }
  {
    o3m_module m = fx_native();
    m.struct_size = static_cast<std::uint32_t>(O3M_MODULE_SIZE(11));
    add("1.12 with the 1.11 size", m);
    m = fx_native();
    m.abi_minor = 11;
    add("1.11 with the 1.12 size", m);
    m = fx_native();
    m.abi_minor = kOsAbiMinor + 1;
    m.struct_size = static_cast<std::uint32_t>(sizeof(o3m_module));
    add("a minor newer than the OS's", m);
  }
  for (const Case& c : bad) {
    static ModuleSet fresh;
    fresh.clear();
    CAPTURE(std::string(c.what));
    CHECK(!fresh.add_native("bad", c.m));
    CHECK(fresh.count() == 0u);
  }
}

TEST_CASE("module set: a native entry of 1.11 or below with the 1.12 members set loads as before (they are not read below 1.12)") {
  static ModuleSet set;
  set.clear();
  o3m_module m = fx_native();
  m.abi_minor = 11;
  m.struct_size = static_cast<std::uint32_t>(O3M_MODULE_SIZE(11));  // an 84-byte descriptor: the four words past it are not part of the module
  m.fx_flags = 0xFFFFFFFFu;  // junk that 1.12 would refuse
  m.preset_count = 99;
  CHECK(set.add_native("old11", m));
  o3m_module a = fx_native(O3M_KIND_INSTRUMENT);  // an instrument at 1.8 whose parameter has the KEY bit: not honoured, not refused
  a.abi_minor = 8;
  a.struct_size = static_cast<std::uint32_t>(O3M_MODULE_SIZE(8));
  CHECK(set.add_native("old8", a));
  CHECK(set.at(0)->desc.struct_size == 84);
  CHECK(set.at(1)->desc.struct_size == 80);
}
