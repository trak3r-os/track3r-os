// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FLASHDIET I2 on the Cortex-M4F (the QEMU suite): the import through the packed tracker.tmod. The bytes of the built modules/tracker.tmod are
// embedded in the guest (tools/raw2c.py) and put on the test card as modules/importers/tracker.tmod; the real loader places and relocates it into
// RAM the core executes, the import runs inside the module (ModuleImport's proxy), and the project the native writer makes of the result is hashed
// and checked against the linked importer's over the same MOD fixture. The PCM of every slot is hashed the same way. On the host the module code
// is Thumb and cannot run, so this suite is the target's.
#include <doctest/doctest.h>

#include "support/mmd_fixture.h"
#include "support/need.h"
#include "support/static_slot.h"

#include <o3os/app/module_import.h>
#include <o3os/formats/format.h>
#include <o3os/formats/med/import.h>
#include <o3os/formats/mod/import.h>
#include <o3os/formats/native.h>
#include <o3os/hal/storage.h>
#include <o3os/model/import_layout.h>
#include <o3os/module/loader.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Embedded by tools/raw2c.py from the built modules/tracker.tmod (tests/CMakeLists.txt, the module_import_run suite).
extern const std::uint8_t k_tracker_tmod[];
extern const unsigned k_tracker_tmod_size;

namespace {

using namespace o3os;

// A card of two byte files: no heap, no std, so it runs on the target.
class BytesCard final : public hal::storage::Storage {
 public:
  void add(const char* path, const std::uint8_t* data, std::uint32_t size) {
    for (unsigned i = 0; i < kMaxFiles; ++i) {
      if (files_[i].data == nullptr) {
        std::snprintf(files_[i].path, sizeof files_[i].path, "%s", path);
        files_[i].data = data;
        files_[i].size = size;
        return;
      }
    }
  }
  hal::storage::Status mount() noexcept override { return hal::storage::Status::Ok; }
  bool mounted() const noexcept override { return true; }
  bool card_present() const noexcept override { return true; }
  hal::storage::VolumeInfo volume() const noexcept override { return {}; }
  hal::storage::Status exists(const char* path, bool* is_dir) noexcept override {
    if (is_dir != nullptr) {
      *is_dir = false;
    }
    return find(path) >= 0 ? hal::storage::Status::Ok : hal::storage::Status::NotFound;
  }
  hal::storage::Status open(const char* path, hal::storage::FileHandle& out) noexcept override {
    const int at = find(path);
    if (at < 0) {
      return hal::storage::Status::NotFound;
    }
    files_[static_cast<unsigned>(at)].pos = 0;
    out.slot = static_cast<std::uint8_t>(at);
    return hal::storage::Status::Ok;
  }
  hal::storage::Status read(hal::storage::FileHandle file, void* dst, std::size_t max, std::size_t& got) noexcept override {
    if (file.slot >= kMaxFiles || files_[file.slot].data == nullptr) {
      return hal::storage::Status::BadHandle;
    }
    File& f = files_[file.slot];
    const std::size_t left = f.size - f.pos;
    const std::size_t n = max < left ? max : left;
    std::memcpy(dst, f.data + f.pos, n);
    f.pos += static_cast<std::uint32_t>(n);
    got = n;
    return hal::storage::Status::Ok;
  }
  hal::storage::Status seek(hal::storage::FileHandle file, std::uint32_t offset) noexcept override {
    if (file.slot >= kMaxFiles || files_[file.slot].data == nullptr) {
      return hal::storage::Status::BadHandle;
    }
    files_[file.slot].pos = offset > files_[file.slot].size ? files_[file.slot].size : offset;
    return hal::storage::Status::Ok;
  }
  hal::storage::Status size(hal::storage::FileHandle file, std::uint32_t& out) const noexcept override {
    if (file.slot >= kMaxFiles || files_[file.slot].data == nullptr) {
      return hal::storage::Status::BadHandle;
    }
    out = files_[file.slot].size;
    return hal::storage::Status::Ok;
  }
  void close(hal::storage::FileHandle& file) noexcept override { file.slot = hal::storage::FileHandle::kNone; }
  hal::storage::Status opendir(const char*, hal::storage::DirHandle&) noexcept override { return hal::storage::Status::Unsupported; }
  hal::storage::Status readdir(hal::storage::DirHandle, hal::storage::DirEntry&) noexcept override { return hal::storage::Status::BadHandle; }
  void closedir(hal::storage::DirHandle&) noexcept override {}

 private:
  static constexpr unsigned kMaxFiles = 4;
  struct File {
    char path[64] = {};
    const std::uint8_t* data = nullptr;
    std::uint32_t size = 0;
    std::uint32_t pos = 0;
  };
  int find(const char* path) const {
    for (unsigned i = 0; i < kMaxFiles; ++i) {
      if (files_[i].data != nullptr && std::strcmp(files_[i].path, path) == 0) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  File files_[kMaxFiles];
};

// The importers never read a byte of the ByteSource of FormatReader::begin.
class NoBytes final : public core::ByteSource {
 public:
  core::IoResult read(void*, std::size_t) noexcept override { return {0, core::IoStatus::Failed}; }
};

// A random-access source over one buffer: the linked reader's window.
class BufSource final : public formats::med::RandomSource {
 public:
  BufSource(const std::uint8_t* data, std::uint32_t size) noexcept : data_(data), size_(size) {}
  std::uint32_t size() const noexcept override { return size_; }
  bool read_at(std::uint32_t offset, void* dst, std::uint32_t n) noexcept override {
    if (offset > size_ || n > size_ - offset) {
      return false;
    }
    std::memcpy(dst, data_ + offset, n);
    return true;
  }

 private:
  const std::uint8_t* data_;
  std::uint32_t size_;
};

// A byte sink that hashes what the native writer produces (FNV-1a): the project's hash, free of the runtime counters the writer does not save.
class HashSink final : public core::ByteSink {
 public:
  core::IoResult write(const void* src, std::size_t n) noexcept override {
    const std::uint8_t* b = static_cast<const std::uint8_t*>(src);
    for (std::size_t i = 0; i < n; ++i) {
      h_ = (h_ ^ b[i]) * 16777619u;
    }
    return {n, core::IoStatus::Ok};
  }
  std::uint32_t h_ = 2166136261u;
};

std::uint32_t project_hash(const model::Project& p) {
  formats::native::Writer w;
  HashSink sink;
  w.begin(p, sink);
  while (w.step(4096) == formats::Progress::Running) {
  }
  return sink.h_;
}

// One ProTracker module in a static buffer: "M.K.", 4 channels, one pattern of 64 rows, one 64-byte 8-bit sample, one order, a C-2 on row 0.
alignas(4) std::uint8_t g_mod[1084 + 1024 + 64];

void build_mod() noexcept {
  std::memset(g_mod, 0, sizeof g_mod);
  std::memcpy(g_mod, "I2 QEMU", 7);
  g_mod[42] = 0;      // sample 0's length, in words, big endian: 32 words = 64 bytes
  g_mod[43] = 32;
  g_mod[45] = 64;     // its volume
  g_mod[950] = 1;     // song length: one order
  g_mod[952] = 0;     // order 0: pattern 0
  std::memcpy(g_mod + 1080, "M.K.", 4);
  g_mod[1084] = 0x01;  // row 0, channel 0: period 428 (C-2) with sample 1
  g_mod[1085] = 0xAC;
  g_mod[1086] = 0x10;
  g_mod[1087] = 0x00;
  for (unsigned i = 0; i < 64; ++i) {
    g_mod[1084 + 1024 + i] = static_cast<std::uint8_t>((i * 3u) & 0x7Fu);
  }
}

std::uint32_t addr_of(const void* p) noexcept { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p)); }

// The test's memory, in .bss: one project (like wav_sample), the module arenas and the PCM scratch.
alignas(8) model::ModelArena g_arena;
alignas(64) std::uint8_t g_slab[16 * 1024];
alignas(64) std::uint8_t g_cold[192 * 1024];
std::int16_t g_pcm[1 << 16];

}  // namespace

TEST_CASE("OS-FLASHDIET I2: the import through the packed tracker.tmod on the Cortex-M4F is the linked importer's, project and PCM hash for hash") {
  build_mod();
  BytesCard card;
  card.add("MOD/ref.mod", g_mod, sizeof g_mod);
  card.add("modules/importers/tracker.tmod", k_tracker_tmod, k_tracker_tmod_size);

  model::ModelArena& arena = g_arena;
  arena.project.reset();
  module::ModuleSet set;
  module::ModuleLoader loader;
  set.hot().init(g_slab, addr_of(g_slab), sizeof g_slab);
  set.cold().init(g_cold, addr_of(g_cold), sizeof g_cold);

  // OS-FLASHDIET I3: ModuleImport is the small proxy now (the linked readers are gone from the image); the reader object below holds one
  // too: both go in static storage, the target suites run on a 60 KiB stack (tests/support/static_slot.h)
  static test::StaticSlot<app::ModuleImport> imp_slot;
  app::ModuleImport& imp = imp_slot.make();
  imp.set_module_host(&set, &loader, nullptr, "tracker");
  NEED(imp.begin(card, "MOD/ref.mod", arena, app::ImportEngine::Auto) == app::ModuleImport::Start::Started);
  for (unsigned n = 0; n < 100000 && imp.state() == app::ModuleImport::State::Running; ++n) {
    (void)imp.step(16384);
  }
  NEED(imp.state() == app::ModuleImport::State::Done);
  NEED(set.importer_count() == 1);  // loaded from the card, into the importer table
  const module::LoadedModule* m = set.importer_at(0);
  NEED(m != nullptr);
  CHECK(m->entry != nullptr);  // the target: the placement's memory is its address, the module's code runs
  CHECK(m->header.kind == O3M_KIND_IMPORTER);
  CHECK(m->header.reserved2[1] == model::kImportLayoutTag);
  const std::uint32_t hash_module = project_hash(arena.project);
  // the PCM of every slot, through the module's read_pcm
  std::uint32_t pcm_module = 2166136261u;
  for (unsigned slot = 0; slot < 64; ++slot) {
    const std::uint32_t frames = imp.frames(slot);
    if (frames == 0u) {
      continue;
    }
    std::int16_t* pcm = g_pcm;
    const std::uint32_t n = frames < (1u << 16) ? frames : (1u << 16);
    NEED(imp.read(slot, 0, n, pcm));
    const std::uint32_t rate = imp.rate(slot);
    for (unsigned i = 0; i < 4; ++i) {
      pcm_module = (pcm_module ^ reinterpret_cast<const std::uint8_t*>(&rate)[i]) * 16777619u;
    }
    for (std::uint32_t i = 0; i < n * 2u; ++i) {
      pcm_module = (pcm_module ^ reinterpret_cast<const std::uint8_t*>(pcm)[i]) * 16777619u;
    }
  }
  (void)imp.samples_done(app::SampleLoadReport{});
  CHECK(set.importer_count() == 0);  // close() unloaded it

  // the linked importer of formats_tracker (I3: the test build links it, the image does not) over the same file, into the same arena
  arena.project.reset();
  NoBytes no_bytes;
  BufSource src(g_mod, sizeof g_mod);
  static test::StaticSlot<formats::mod::ImportReader> reader_slot;
  formats::mod::ImportReader& reader = reader_slot.make();
  reader.options() = formats::mod::ImportOptions{};
  formats::ReadParams params;
  params.random = &src;
  params.size_hint = sizeof g_mod;
  reader.begin(no_bytes, arena, params);
  while (reader.step(4096) == formats::Progress::Running) {
  }
  NEED(reader.progress() == formats::Progress::Done);
  const std::uint32_t hash_linked = project_hash(arena.project);
  std::uint32_t pcm_linked = 2166136261u;
  for (unsigned slot = 0; slot < 64; ++slot) {
    if (slot < formats::mod::caps::kSlots && reader.importer().report().med.instrument[slot].result == formats::med::InstResult::Sample) {
      const std::uint32_t frames = reader.importer().pcm_frames(slot);
      if (frames == 0u) {
        continue;
      }
      std::int16_t* pcm = g_pcm;
      const std::uint32_t n = frames < (1u << 16) ? frames : (1u << 16);
      NEED(reader.importer().read_pcm(slot, 0, n, pcm).ok());
      const std::uint32_t rate = reader.importer().pcm_rate(slot);
      for (unsigned i = 0; i < 4; ++i) {
        pcm_linked = (pcm_linked ^ reinterpret_cast<const std::uint8_t*>(&rate)[i]) * 16777619u;
      }
      for (std::uint32_t i = 0; i < n * 2u; ++i) {
        pcm_linked = (pcm_linked ^ reinterpret_cast<const std::uint8_t*>(pcm)[i]) * 16777619u;
      }
    }
  }
  CHECK(hash_module == hash_linked);
  CHECK(pcm_module == pcm_linked);
}

// OS-FLASHDIET I2: the same equality for an OctaMED file: a small synthetic MMD0 (support/mmd_fixture.h, the fixture the MED suites run on the target too)
// goes through the packed tracker.tmod, and the project and PCM hashes are the linked MED importer's over the same bytes.
TEST_CASE("OS-FLASHDIET I2: the MED import through the packed tracker.tmod on the Cortex-M4F is the linked MED importer's, hash for hash") {
  const std::vector<std::uint8_t> med = test::mmd::build(test::mmd::sample_module(0));  // MMD0, a few KB
  BytesCard card;
  card.add("MED/ref.med", med.data(), static_cast<std::uint32_t>(med.size()));
  card.add("modules/importers/tracker.tmod", k_tracker_tmod, k_tracker_tmod_size);

  model::ModelArena& arena = g_arena;
  arena.project.reset();
  module::ModuleSet set;
  module::ModuleLoader loader;
  set.hot().init(g_slab, addr_of(g_slab), sizeof g_slab);
  set.cold().init(g_cold, addr_of(g_cold), sizeof g_cold);

  static test::StaticSlot<app::ModuleImport> imp_slot;
  app::ModuleImport& imp = imp_slot.make();
  imp.set_module_host(&set, &loader, nullptr, "tracker");
  NEED(imp.begin(card, "MED/ref.med", arena, app::ImportEngine::Auto) == app::ModuleImport::Start::Started);
  for (unsigned n = 0; n < 100000 && imp.state() == app::ModuleImport::State::Running; ++n) {
    (void)imp.step(16384);
  }
  NEED(imp.state() == app::ModuleImport::State::Done);
  NEED(set.importer_count() == 1);  // loaded from the card, into the importer table
  const std::uint32_t hash_module = project_hash(arena.project);
  std::uint32_t pcm_module = 2166136261u;
  for (unsigned slot = 0; slot < 64; ++slot) {
    const std::uint32_t frames = imp.frames(slot);
    if (frames == 0u) {
      continue;
    }
    std::int16_t* pcm = g_pcm;
    const std::uint32_t n = frames < (1u << 16) ? frames : (1u << 16);
    NEED(imp.read(slot, 0, n, pcm));
    const std::uint32_t rate = imp.rate(slot);
    for (unsigned i = 0; i < 4; ++i) {
      pcm_module = (pcm_module ^ reinterpret_cast<const std::uint8_t*>(&rate)[i]) * 16777619u;
    }
    for (std::uint32_t i = 0; i < n * 2u; ++i) {
      pcm_module = (pcm_module ^ reinterpret_cast<const std::uint8_t*>(pcm)[i]) * 16777619u;
    }
  }
  (void)imp.samples_done(app::SampleLoadReport{});
  CHECK(set.importer_count() == 0);  // close() unloaded it

  // the linked MED importer over the same bytes, into the same arena; Auto gives the module path amiga_default() (engine_fx on)
  arena.project.reset();
  NoBytes no_bytes;
  BufSource src(med.data(), static_cast<std::uint32_t>(med.size()));
  static test::StaticSlot<formats::med::ImportReader> reader_slot;
  formats::med::ImportReader& reader = reader_slot.make();
  reader.options() = formats::med::ImportReader::amiga_default();
  formats::ReadParams params;
  params.random = &src;
  params.size_hint = static_cast<std::uint32_t>(med.size());
  reader.begin(no_bytes, arena, params);
  while (reader.step(4096) == formats::Progress::Running) {
  }
  NEED(reader.progress() == formats::Progress::Done);
  const std::uint32_t hash_linked = project_hash(arena.project);
  std::uint32_t pcm_linked = 2166136261u;
  const formats::med::ImportReport& rep = reader.importer().report();
  for (unsigned slot = 0; slot < formats::med::caps::kMaxSamples; ++slot) {
    if (rep.instrument[slot].result != formats::med::InstResult::Sample) {
      continue;
    }
    const std::uint32_t frames = reader.importer().pcm_frames(slot);
    if (frames == 0u) {
      continue;
    }
    std::int16_t* pcm = g_pcm;
    const std::uint32_t n = frames < (1u << 16) ? frames : (1u << 16);
    NEED(reader.importer().read_pcm(slot, 0, n, pcm).ok());
    const std::uint32_t rate = reader.importer().pcm_rate();
    for (unsigned i = 0; i < 4; ++i) {
      pcm_linked = (pcm_linked ^ reinterpret_cast<const std::uint8_t*>(&rate)[i]) * 16777619u;
    }
    for (std::uint32_t i = 0; i < n * 2u; ++i) {
      pcm_linked = (pcm_linked ^ reinterpret_cast<const std::uint8_t*>(pcm)[i]) * 16777619u;
    }
  }
  CHECK(hash_module == hash_linked);
  CHECK(pcm_module == pcm_linked);
}
