// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FLASHDIET I1/I2: the importer kind (kind 3, ABI 1.11, Level C: published module ABI). The header's word 44 is the importer layout tag
// for kind 3 only and reserved zero for every other kind and minor; a kind 3 file with a wrong tag, a missing function, a format list out
// of bounds or a minor above the OS's is refused with its status; every earlier minor still loads unchanged with word 44 zero; the tag
// follows the whole import-facing model layout (a field of a struct the importer writes that moves changes it); and the set holds an
// importer apart from the playable modules, so it never binds to a track and does not count against kMaxModules. The packer's side (word 44 written for kind 3, zero for the others) is
// tools/o3m_pack.py --self-test; the built tracker.tmod is loaded here through the real loader.
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

#include <o3os/model/import_layout.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/dir_storage.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace o3os::module;
using namespace o3os::module::test;
namespace fs = std::filesystem;

// A header check of the blob's file as it stands.
Status header_status(const Blob& b) { return check_header(b.h, static_cast<std::uint32_t>(b.file().size())); }

// Places and relocates `b` into `mem` and reads its descriptor (with the 1.8 and 1.11 tails).
Status read_blob_desc(const Blob& b, Mem& mem, Desc32& d, DescExt32& ext, DescImp32& imp) {
  place_image(b, mem.place());
  Relocator r(b.h, mem.place());
  if (r.apply(b.relocs.data(), static_cast<std::uint32_t>(b.relocs.size())) != Status::Ok) {
    return Status::BadRelocOrder;
  }
  return read_desc(b.h, mem.place(), d, &ext, &imp);
}

// A card in a temporary directory.
struct Card {
  fs::path root;
  Card() {
    root = fs::temp_directory_path() / ("o3m_importer_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    fs::remove_all(root);
    fs::create_directories(root / "modules" / "importers");
    fs::create_directories(root / "modules" / "instruments");
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
  Rig() {
    mounted = storage.mount() == o3os::hal::storage::Status::Ok;
    set.hot().init(slab, kHotAddr, sizeof slab);
    set.cold().init(sdram, kColdAddr, sizeof sdram);
  }
  ModuleLoader::State run(const char* stem, std::size_t budget = 512, bool importer = false) {
    if (!loader.begin(storage, set, stem, nullptr, importer)) {  // OS-FLASHDIET I2 review note 1: importer=true is the importer scan (modules/importers/ only)
      return ModuleLoader::State::Failed;
    }
    for (unsigned n = 0; n < 100000 && loader.state() == ModuleLoader::State::Running; ++n) {
      loader.step(budget);
    }
    return loader.state();
  }
};

std::vector<std::uint8_t> slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("module importer: the layout tag covers the whole import-facing layout, and a field of a copy of a struct that moves changes it") {
  using o3os::model::Step;
  using o3os::model::StepFx;
  using o3os::model::kImportLayoutTag;
  using o3os::model::kImportLayoutVersion;
  using o3os::model::layout_mix;
  using o3os::model::layout_mix_all;
  using o3os::model::model_import_layout_probe;
  using o3os::model::step_layout_tag;
  // pinned: model/import_layout_tag.txt (read by the packer and the generator) holds the same number, held to this one by ctest o3m_layout_tag_pin
  MESSAGE("kImportLayoutTag = 0x", std::hex, kImportLayoutTag);
  CHECK(kImportLayoutTag == 0xd008ff20u);  // I2: the tag covers the whole import-facing layout; update with the MESSAGE above and model/import_layout_tag.txt
  // I2: Step's facts alone are not the tag any more. Recompute it here from every group the header hashes — the arena, the Project's probe, TrackSlot,
  // Pattern, Step, Instrument and its SamplerParams / Envelope / Lfo, plus the manual version — and require equality, so a group dropped from the
  // header's hash fails this test.
  {
    using o3os::model::Envelope;
    using o3os::model::Instrument;
    using o3os::model::Lfo;
    using o3os::model::ModelArena;
    using o3os::model::Pattern;
    using o3os::model::SamplerParams;
    using o3os::model::TrackSlot;
    std::uint32_t h = 2166136261u;
    h = layout_mix(h, kImportLayoutVersion);
    h = layout_mix_all(h, {sizeof(ModelArena), offsetof(ModelArena, project)});
    h = model_import_layout_probe(h);
    h = layout_mix_all(h, {sizeof(TrackSlot), offsetof(TrackSlot, type), offsetof(TrackSlot, flags), offsetof(TrackSlot, volume), offsetof(TrackSlot, pan),
                           offsetof(TrackSlot, bus), offsetof(TrackSlot, name)});
    h = layout_mix_all(h, {sizeof(Pattern), offsetof(Pattern, id), offsetof(Pattern, track_length), offsetof(Pattern, name), offsetof(Pattern, steps)});
    h = layout_mix_all(h, {sizeof(Step), offsetof(Step, note), offsetof(Step, instr), offsetof(Step, vol), offsetof(Step, fx), sizeof(StepFx)});
    h = layout_mix_all(h, {sizeof(Instrument), offsetof(Instrument, kind), offsetof(Instrument, midi_channel), offsetof(Instrument, module_index),
                           offsetof(Instrument, engine), offsetof(Instrument, param_len), offsetof(Instrument, name), offsetof(Instrument, sample_path),
                           offsetof(Instrument, sample_rate), offsetof(Instrument, sampler), offsetof(Instrument, params),
                           offsetof(Instrument, midi_velocity), offsetof(Instrument, midi_program), offsetof(Instrument, midi_out),
                           offsetof(Instrument, midi_pb_range), offsetof(Instrument, midi_offset), offsetof(Instrument, midi_cc),
                           offsetof(Instrument, vel_sens), offsetof(Instrument, stereo), offsetof(Instrument, reserved)});
    h = layout_mix_all(h, {sizeof(SamplerParams), offsetof(SamplerParams, cutoff), offsetof(SamplerParams, resonance), offsetof(SamplerParams, envelope),
                           offsetof(SamplerParams, lfo), offsetof(SamplerParams, wavetable_window), offsetof(SamplerParams, start),
                           offsetof(SamplerParams, loop_start), offsetof(SamplerParams, loop_end), offsetof(SamplerParams, end),
                           offsetof(SamplerParams, slices), offsetof(SamplerParams, glide), offsetof(SamplerParams, wavetable_window_size),
                           offsetof(SamplerParams, grain_length), offsetof(SamplerParams, grain_position), offsetof(SamplerParams, sample_type),
                           offsetof(SamplerParams, play_mode), offsetof(SamplerParams, filter_type), offsetof(SamplerParams, filter_enable),
                           offsetof(SamplerParams, tune), offsetof(SamplerParams, fine_tune), offsetof(SamplerParams, volume),
                           offsetof(SamplerParams, panning), offsetof(SamplerParams, delay_send), offsetof(SamplerParams, reverb_send),
                           offsetof(SamplerParams, overdrive), offsetof(SamplerParams, bit_depth), offsetof(SamplerParams, slice_count),
                           offsetof(SamplerParams, selected_slice), offsetof(SamplerParams, grain_shape), offsetof(SamplerParams, grain_type)});
    h = layout_mix_all(h, {sizeof(Envelope), offsetof(Envelope, amount), offsetof(Envelope, delay), offsetof(Envelope, attack), offsetof(Envelope, hold),
                           offsetof(Envelope, decay), offsetof(Envelope, sustain), offsetof(Envelope, release), offsetof(Envelope, loop),
                           offsetof(Envelope, enable)});
    h = layout_mix_all(h, {sizeof(Lfo), offsetof(Lfo, shape), offsetof(Lfo, speed), offsetof(Lfo, amount)});
    CHECK(kImportLayoutTag == h);
  }
  // the Step-only tag of I1 is not the tag any more (the importer writes the whole project through the arena, not just the steps)
  const std::uint32_t step_tag = step_layout_tag(sizeof(Step), offsetof(Step, note), offsetof(Step, instr), offsetof(Step, vol), offsetof(Step, fx), sizeof(StepFx));
  CHECK(kImportLayoutTag != step_tag);
  // a copy of the struct with note and instr swapped: same size, different layout, different tag
  struct alignas(1) StepMoved {
    std::uint8_t instr;
    std::uint8_t note;
    std::uint8_t vol;
    StepFx fx[2];
  };
  static_assert(sizeof(StepMoved) == sizeof(Step), "the copy is the same size");
  CHECK(step_layout_tag(sizeof(StepMoved), offsetof(StepMoved, note), offsetof(StepMoved, instr), offsetof(StepMoved, vol), offsetof(StepMoved, fx),
                        sizeof(StepMoved::fx[0])) != step_tag);
  // a copy with one more byte: different tag too
  struct alignas(1) StepWide {
    std::uint8_t note;
    std::uint8_t instr;
    std::uint8_t vol;
    std::uint8_t pad;
    StepFx fx[2];
  };
  CHECK(step_layout_tag(sizeof(StepWide), offsetof(StepWide, note), offsetof(StepWide, instr), offsetof(StepWide, vol), offsetof(StepWide, fx),
                        sizeof(StepWide::fx[0])) != step_tag);
  // the descriptor's size is per minor: 84 from 1.11, 100 from 1.12 (OS-INSFX-ABI2 A1)
  CHECK(desc_size(10) == 80);
  CHECK(desc_size(11) == 84);
  CHECK(desc_size(12) == 100);
  CHECK(desc_size(7) == 64);
  if (sizeof(void*) == 4) {
    CHECK(O3M_MODULE_SIZE(10) == 80u);
    CHECK(O3M_MODULE_SIZE(11) == 84u);
    CHECK(O3M_MODULE_SIZE(12) == 100u);
    CHECK(offsetof(o3m_module, importer) == 80);
    CHECK(sizeof(o3m_module) == 100);
    CHECK(sizeof(o3m_importer) == 32);  // eight words since I2 (report and close appended)
    CHECK(sizeof(o3m_format_desc) == 4);
    CHECK(sizeof(o3m_file_window) == 16);
  }
}

TEST_CASE("module importer: header word 44 — every earlier minor loads unchanged with the word zero, a non-importer kind with a non-zero word is refused") {
  // a 1.x module of any other kind, at every minor below the OS's, with reserved2[1] == 0: loads (the level C rule)
  for (std::uint8_t minor = 0; minor < kOsAbiMinor; ++minor) {
    Blob b = good_blob();
    b.h.abi_minor = minor;
    b.h.reserved2[1] = 0;
    CAPTURE(int{minor});
    CHECK(header_status(b) == Status::Ok);
  }
  // the previous minor (1.10) explicitly, with the word zero and with a tag: the word is reserved there
  {
    Blob b = good_blob();
    b.h.abi_minor = 10;
    CHECK(header_status(b) == Status::Ok);
    b.h.reserved2[1] = o3os::model::kImportLayoutTag;
    CHECK(header_status(b) == Status::BadReserved);
  }
  struct Case {
    const char* what;
    void (*mutate)(o3m_file_header&);
    Status want;
  };
  const Case cases[] = {
      {"instrument of ABI 1.11, word 44 set", [](o3m_file_header& h) { h.abi_minor = 11; h.reserved2[1] = 1; }, Status::BadReserved},
      {"instrument of ABI 1.7, word 44 set", [](o3m_file_header& h) { h.reserved2[1] = 1; }, Status::BadReserved},
      {"effect of ABI 1.11, word 44 set", [](o3m_file_header& h) { h.kind = O3M_KIND_EFFECT; h.abi_minor = 11; h.reserved2[1] = o3os::model::kImportLayoutTag; }, Status::BadReserved},
      {"app of ABI 1.11, word 44 set", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 11; h.reserved2[1] = 1; }, Status::BadReserved},
      {"kind 3 at ABI 1.10 (the kind is published at 1.11)", [](o3m_file_header& h) { h.kind = O3M_KIND_IMPORTER; h.abi_minor = 10; }, Status::BadKind},
      {"kind 3 at ABI 1.0", [](o3m_file_header& h) { h.kind = O3M_KIND_IMPORTER; h.abi_minor = 0; }, Status::BadKind},
      {"kind 3 at ABI 1.11 with a wrong tag", [](o3m_file_header& h) { h.kind = O3M_KIND_IMPORTER; h.abi_minor = 11; h.reserved2[1] = o3os::model::kImportLayoutTag ^ 1u; }, Status::LayoutMismatch},
      {"kind 3 at ABI 1.11 with word 44 zero", [](o3m_file_header& h) { h.kind = O3M_KIND_IMPORTER; h.abi_minor = 11; }, Status::LayoutMismatch},
      {"kind 3 above the OS's minor", [](o3m_file_header& h) { h.kind = O3M_KIND_IMPORTER; h.abi_minor = static_cast<std::uint8_t>(kOsAbiMinor + 1); h.reserved2[1] = o3os::model::kImportLayoutTag; }, Status::AbiMinor},
      {"kind 3 asking for a capability (word 40 is an app's)", [](o3m_file_header& h) { h.kind = O3M_KIND_IMPORTER; h.abi_minor = 11; h.reserved2[1] = o3os::model::kImportLayoutTag; h.reserved2[0] = O3M_CAP_MIDI_IN; }, Status::BadReserved},
  };
  for (const Case& c : cases) {
    Blob b = good_blob();
    c.mutate(b.h);
    CAPTURE(c.what);
    CHECK(header_status(b) == c.want);
  }
  // a kind 3 file with the right tag passes the header-only check (the app can call check_header before anything is placed)
  {
    Blob b = blob_importer();
    CHECK(b.h.reserved2[1] == o3os::model::kImportLayoutTag);
    CHECK(header_status(b) == Status::Ok);
    b.h.reserved2[1] ^= 1u;
    CHECK(header_status(b) == Status::LayoutMismatch);
    b.h.reserved2[1] = o3os::model::kImportLayoutTag;
    b.h.abi_minor = static_cast<std::uint8_t>(kOsAbiMinor + 1);
    CHECK(header_status(b) == Status::AbiMinor);
  }
  // the new status is a validation failure (the file is wrong: quarantined for the boot) and has a name
  CHECK(is_validation_failure(Status::LayoutMismatch));
  CHECK(!is_validation_failure(Status::NoMemory));
  CHECK(std::string(status_name(Status::LayoutMismatch)) == "importer layout mismatch (word 44 is not this OS's tag)");
}

TEST_CASE("module importer: read_desc accepts a kind 3 blob with a valid importer table, and refuses every broken one") {
  {
    Blob b = blob_importer();
    Mem mem;
    Desc32 d;
    DescExt32 ext;
    DescImp32 imp;
    NEED(read_blob_desc(b, mem, d, ext, imp) == Status::Ok);
    CHECK(d.kind == O3M_KIND_IMPORTER);
    CHECK(d.abi_minor == 11);
    CHECK(d.struct_size == 84);
    CHECK(d.param_count == 0);
    CHECK(d.view_count == 0);
    CHECK(d.ui == 0);
    CHECK(imp.importer == moved(LayoutImp::table));
    Importer32 t;
    NEED(read_importer32(b.h, mem.place(), imp, t));
    CHECK(t.size == sizeof(Importer32));
    CHECK(t.format_count == 3);
    CHECK(t.formats == moved(LayoutImp::formats));
    CHECK((t.begin & 1u) != 0u);
    CHECK((t.step & 1u) != 0u);
    CHECK((t.read_pcm & 1u) != 0u);
    CHECK((t.report & 1u) != 0u);
    CHECK((t.close & 1u) != 0u);
    // the format list: three entries inside the image, each extension a string
    const std::uint8_t* fmts = resolve(b.h, mem.place(), t.formats, t.format_count * sizeof(Format32));
    NEED(fmts != nullptr);
    const char* want[3] = {"mod", "s3m", "med"};
    for (std::uint32_t i = 0; i < t.format_count; ++i) {
      std::uint32_t ext_addr;
      std::memcpy(&ext_addr, fmts + i * sizeof(Format32), sizeof ext_addr);
      const std::uint8_t* s = resolve(b.h, mem.place(), ext_addr, 1);
      NEED(s != nullptr);
      CHECK(std::string(reinterpret_cast<const char*>(s)) == want[i]);
    }
    // the sound entries may be NULL for an importer (as for an app), init and shutdown may not
    CHECK(d.init != 0u);
    CHECK(d.shutdown != 0u);
  }
  struct Case {
    const char* what;
    void (*mutate)(Blob&);
  };
  const Case cases[] = {
      {"no importer table (the word is 0)", [](Blob& b) { b.put(LayoutImp::desc + 80, 0); b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), LayoutImp::desc + 80), b.relocs.end()); }},
      {"the table one past the end of the image (a legal end pointer, but no table there)", [](Blob& b) { b.put(LayoutImp::desc + 80, LayoutImp::kImageImp); }},
      {"a wrong table size", [](Blob& b) { b.put(LayoutImp::table, 20); }},
      {"no formats", [](Blob& b) { b.put(LayoutImp::table + 8, 0); }},
      {"too many formats", [](Blob& b) { b.put(LayoutImp::table + 8, O3M_IMPORTER_FORMATS_MAX + 1); }},
      {"the format list outside the image", [](Blob& b) { b.put(LayoutImp::table + 4, LayoutImp::kImageImp); }},
      {"an extension not a string", [](Blob& b) { b.put(LayoutImp::formats, LayoutImp::kImageImp - 1); }},
      {"a missing function (begin)", [](Blob& b) { b.put(LayoutImp::table + 12, 0); b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), LayoutImp::table + 12), b.relocs.end()); }},
      {"a missing function (step)", [](Blob& b) { b.put(LayoutImp::table + 16, 0); b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), LayoutImp::table + 16), b.relocs.end()); }},
      {"a missing function (read_pcm)", [](Blob& b) { b.put(LayoutImp::table + 20, 0); b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), LayoutImp::table + 20), b.relocs.end()); }},
      {"a missing function (report)", [](Blob& b) { b.put(LayoutImp::table + 24, 0); b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), LayoutImp::table + 24), b.relocs.end()); }},
      {"a missing function (close)", [](Blob& b) { b.put(LayoutImp::table + 28, 0); b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), LayoutImp::table + 28), b.relocs.end()); }},
      {"a function not Thumb", [](Blob& b) { b.put(LayoutImp::table + 12, 2); }},
      {"parameters on an importer", [](Blob& b) { b.h.param_count = 1; b.put(LayoutImp::desc + 24, 1); }},
      {"views on an importer", [](Blob& b) { b.h.view_count = 1; b.put(LayoutImp::desc + 60, 1); }},
      {"the wrong struct size", [](Blob& b) { b.put(LayoutImp::desc + 12, 80); }},
  };
  for (const Case& c : cases) {
    Blob b = blob_importer();
    c.mutate(b);
    b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
    b.seal();
    Mem mem;
    Desc32 d;
    DescExt32 ext;
    DescImp32 imp;
    CAPTURE(c.what);
    CHECK(read_blob_desc(b, mem, d, ext, imp) == Status::BadDescriptor);
    // and the header check still passes: the file is well-formed at the header level (the tag is right), the descriptor is what refuses it
    CHECK(header_status(b) == Status::Ok);
  }
  // a non-importer kind with the importer word set is refused; an importer's sound entries may be NULL (checked above: they are 0)
  {
    Blob b = blob_importer();
    b.h.kind = O3M_KIND_INSTRUMENT;
    b.put(LayoutImp::desc + 8, O3M_KIND_INSTRUMENT);
    b.seal();
    Mem mem;
    Desc32 d;
    DescExt32 ext;
    DescImp32 imp;
    CHECK(read_blob_desc(b, mem, d, ext, imp) == Status::BadDescriptor);
  }
}

TEST_CASE("module importer: the loader loads a kind 3 file from modules/importers/ only for the importer scan, into the importer table and not the module slots") {
  Rig rig;
  NEED(rig.mounted);
  rig.card.write("modules/importers/tracker.tmod", blob_importer().file());
  // OS-FLASHDIET I2 review note 1: a project that names an importer's stem must not load the importer: the playable scan skips modules/importers/
  CHECK(rig.run("tracker") == ModuleLoader::State::Failed);
  CHECK(rig.loader.status() == Status::NotFound);
  CHECK(rig.set.importer_count() == 0);
  CHECK(rig.set.count() == 0);
  // the importer scan (ModuleImport's first step) looks there and only there
  NEED(rig.run("tracker", 512, true) == ModuleLoader::State::Done);
  CHECK(rig.loader.status() == Status::Ok);
  CHECK(rig.set.count() == 0);  // (an importer does not count against kMaxModules)
  CHECK(rig.set.importer_count() == 1);
  CHECK(rig.set.find("tracker") == -1);  // (it never binds to a track: the project's module refs never resolve to it)
  const LoadedModule* m = rig.set.importer_at(0);
  NEED(m != nullptr);
  CHECK(std::string(m->name) == "tracker");
  CHECK(m->header.kind == O3M_KIND_IMPORTER);
  CHECK(m->header.abi_minor == kImporterAbiMinor);
  CHECK(m->header.reserved2[1] == o3os::model::kImportLayoutTag);
  CHECK(m->desc.kind == O3M_KIND_IMPORTER);
  CHECK(m->desc.struct_size == 84);
  DescImp32 imp;
  Desc32 d = m->desc;
  DescExt32 ext = m->ext;
  CHECK(read_desc(m->header, m->place, d, &ext, &imp) == Status::Ok);
  Importer32 t;
  CHECK(read_importer32(m->header, m->place, imp, t));
  CHECK(t.format_count == 3);
  // a kind 3 file with a wrong tag is refused with LayoutMismatch and quarantined for the boot
  {
    Rig rig2;
    NEED(rig2.mounted);
    Blob b = blob_importer();
    b.h.reserved2[1] ^= 1u;
    b.seal();
    rig2.card.write("modules/importers/badtag.tmod", b.file());
    CHECK(rig2.run("badtag", 512, true) == ModuleLoader::State::Failed);
    CHECK(rig2.loader.status() == Status::LayoutMismatch);
    CHECK(rig2.set.quarantined("badtag"));
    CHECK(rig2.set.quarantine_reason(0) == Status::LayoutMismatch);
    CHECK(rig2.set.importer_count() == 0);
  }
}

TEST_CASE("module importer: a full set of instruments still takes an importer (kMaxModules does not count it)") {
  ModuleSet set;
  // kMaxModules playable modules fill the slots
  for (unsigned i = 0; i < kMaxModules; ++i) {
    LoadedModule m{};
    char name[8];
    std::snprintf(name, sizeof name, "m%u", i);
    std::memcpy(m.name, name, std::strlen(name) + 1);
    m.header.kind = O3M_KIND_INSTRUMENT;
    NEED(set.add(m));
  }
  CHECK(set.count() == kMaxModules);
  // an importer goes to its own table, however full the module slots are
  LoadedModule imp{};
  std::memcpy(imp.name, "tracker", 8);
  imp.header.kind = O3M_KIND_IMPORTER;
  imp.header.abi_minor = kImporterAbiMinor;
  imp.header.reserved2[1] = o3os::model::kImportLayoutTag;
  CHECK(set.add(imp));
  CHECK(set.count() == kMaxModules);  // unchanged
  CHECK(set.importer_count() == 1);
  CHECK(set.find("tracker") == -1);
  CHECK(set.find_importer("tracker") == 0);
  // a second importer fits too (up to kMaxImporters), a name that is taken does not
  LoadedModule imp2{};
  std::memcpy(imp2.name, "other", 6);
  imp2.header.kind = O3M_KIND_IMPORTER;
  imp2.header.abi_minor = kImporterAbiMinor;
  imp2.header.reserved2[1] = o3os::model::kImportLayoutTag;
  CHECK(set.add(imp2));
  CHECK(set.importer_count() == 2);
  LoadedModule dup{};
  std::memcpy(dup.name, "tracker", 8);
  dup.header.kind = O3M_KIND_IMPORTER;
  CHECK(!set.add(dup));
  LoadedModule clash{};
  std::memcpy(clash.name, "m0", 3);
  clash.header.kind = O3M_KIND_IMPORTER;
  CHECK(!set.add(clash));  // (the name is taken by a playable module)
  // a playable module does not fit any more, and does not clash with an importer's name either
  LoadedModule full{};
  std::memcpy(full.name, "m16", 4);
  full.header.kind = O3M_KIND_INSTRUMENT;
  CHECK(!set.add(full));
  // clear() forgets the importers too
  set.clear();
  CHECK(set.count() == 0);
  CHECK(set.importer_count() == 0);
}

// ---- ABI 1.12 (OS-INSFX-ABI2 A1): a kind 3 module may declare 1.12 with the four new members zero; the tracker importer itself stays at 1.11 ----------------------------------------

TEST_CASE("module importer: a kind 3 file of ABI 1.12 loads with the fx tail zero and word 44 the tag, and is refused with any of the four words set") {
  {
    Rig rig;
    NEED(rig.mounted);
    rig.card.write("modules/importers/new.tmod", blob_importer(12).file());
    NEED(rig.run("new", 64, true) == ModuleLoader::State::Done);
    const LoadedModule* m = rig.set.importer_at(0);
    NEED(m != nullptr);
    CHECK(m->header.abi_minor == 12);
    CHECK(m->desc.struct_size == 100);
    CHECK(m->header.reserved2[1] == o3os::model::kImportLayoutTag);
    Mem mem;
    Desc32 d;
    DescExt32 ext;
    DescImp32 imp;
    NEED(read_blob_desc(blob_importer(12), mem, d, ext, imp) == Status::Ok);
    Importer32 t;
    NEED(read_importer32(blob_importer(12).h, mem.place(), imp, t));
    CHECK(t.format_count == 3);
  }
  for (unsigned w = 0; w < 4; ++w) {
    Blob b = blob_importer(12);
    b.put(LayoutImp::desc + 84 + 4u * w, w == 0 ? 9u : w == 1 ? O3M_FX_WIDENS : w == 2 ? 200u : 1u);
    b.seal();
    Mem mem;
    Desc32 d;
    DescExt32 ext;
    DescImp32 imp;
    CAPTURE(w);
    CHECK(read_blob_desc(b, mem, d, ext, imp) == Status::BadDescriptor);
  }
  {  // the tag rule is the same at 1.12
    Blob b = blob_importer(12);
    b.h.reserved2[1] ^= 1u;
    b.seal();
    CHECK(header_status(b) == Status::LayoutMismatch);
    Blob c = blob_importer(12);
    c.h.reserved2[1] = 0;
    c.seal();
    CHECK(header_status(c) == Status::LayoutMismatch);
  }
  // the importer at 1.11 with the 100-byte size, and at 1.12 with the 84-byte size, are refused (the size is per minor)
  Blob s11 = blob_importer();
  s11.put(LayoutImp::desc + 12, 100);
  s11.seal();
  Blob s12 = blob_importer(12);
  s12.put(LayoutImp::desc + 12, 84);
  s12.seal();
  for (const Blob* b : {&s11, &s12}) {
    Mem mem;
    Desc32 d;
    DescExt32 ext;
    DescImp32 imp;
    CHECK(read_blob_desc(*b, mem, d, ext, imp) == Status::BadDescriptor);
  }
}

namespace {

int i_init(void*, const o3m_host*) { return 0; }
void i_shutdown(void*) {}
int i_begin(void*, const o3m_file_window*, std::uint32_t, void*, const void*, std::uint32_t) { return 0; }
std::uint32_t i_step(void*, std::uint32_t) { return O3M_IMPORT_DONE; }
std::int32_t i_read_pcm(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::int16_t*) { return 0; }
std::uint32_t i_report(void*, void*, std::uint32_t) { return 0; }
void i_close(void*) {}
const o3m_format_desc kIFormats[1] = {{"mod"}};
const o3m_importer kITable = {sizeof(o3m_importer), kIFormats, 1, i_begin, i_step, i_read_pcm, i_report, i_close};
const float kIValues[1] = {0.0f};
const o3m_preset kIPreset[1] = {{"X", kIValues, 1}};

o3m_module importer_entry(std::uint32_t minor) {
  o3m_module m = {O3M_ABI_MAJOR, minor, O3M_KIND_IMPORTER, static_cast<std::uint32_t>(O3M_MODULE_SIZE(minor)), "imp", 16, 0, nullptr, i_init, nullptr, nullptr, nullptr, nullptr, i_shutdown, nullptr, 0};
  m.importer = &kITable;
  return m;
}

}  // namespace

TEST_CASE("module importer: add_native takes a kind 3 entry at 1.11 and at 1.12 (the four members zero) and refuses presets, render_st and flags on it") {
  static ModuleSet set;
  set.clear();
  static const o3m_module at11 = importer_entry(11);
  static const o3m_module at12 = importer_entry(12);
  CHECK(set.add_native("imp11", at11));
  CHECK(set.add_native("imp12", at12));
  CHECK(set.importer_count() == 2);
  CHECK(set.importer_at(0)->desc.struct_size == 84);
  CHECK(set.importer_at(1)->desc.struct_size == 100);
  CHECK(set.count() == 0u);
  struct Case {
    const char* what;
    o3m_module m;
  };
  std::vector<Case> bad;
  {
    o3m_module m = importer_entry(12);
    m.presets = kIPreset;
    m.preset_count = 1;
    bad.push_back({"presets on an importer", m});
    m = importer_entry(12);
    m.presets = kIPreset;
    bad.push_back({"a preset table with no count on an importer", m});
    m = importer_entry(12);
    m.render_st = [](void*, const float*, const float*, float*, float*, std::uint32_t) -> std::uint32_t { return 0; };
    bad.push_back({"render_st on an importer", m});
    m = importer_entry(12);
    m.fx_flags = O3M_FX_WIDENS;
    bad.push_back({"WIDENS on an importer", m});
    m = importer_entry(12);
    m.struct_size = static_cast<std::uint32_t>(O3M_MODULE_SIZE(11));
    bad.push_back({"1.12 with the 84-byte size", m});
    m = importer_entry(11);
    m.struct_size = static_cast<std::uint32_t>(O3M_MODULE_SIZE(12));
    bad.push_back({"1.11 with the 100-byte size", m});
  }
  for (const Case& c : bad) {
    static ModuleSet fresh;
    fresh.clear();
    CAPTURE(std::string(c.what));
    CHECK(!fresh.add_native("bad", c.m));
    CHECK(fresh.importer_count() == 0);
  }
}

TEST_CASE("module importer: the packed tracker.tmod (built by the cross compiler) loads: its word 44 is this OS's tag") {
#if defined(O3M_FIXTURE_DIR)
  const std::vector<std::uint8_t> file = slurp(std::string(O3M_FIXTURE_DIR) + "/tracker.tmod");
#else
  const std::vector<std::uint8_t> file;
#endif
  if (file.empty()) {
    const char* ci = std::getenv("O3_CI");
    CHECK_MESSAGE(!(ci != nullptr && ci[0] == '1'), "tracker.tmod was not built and O3_CI=1");
    MESSAGE("tracker.tmod not built (no arm-none-eabi-g++): skipped");
    return;
  }
  o3m_file_header h;
  NEED(file.size() >= sizeof h);
  std::memcpy(&h, file.data(), sizeof h);
  CHECK(h.kind == O3M_KIND_IMPORTER);
  CHECK(h.abi_minor == kImporterAbiMinor);
  // the packer's tag is the OS's tag: the version lock holds
  CHECK(h.reserved2[1] == o3os::model::kImportLayoutTag);
  CHECK(check_header(h, static_cast<std::uint32_t>(file.size())) == Status::Ok);
  // tracker.tmod is tens of KB: the arenas must be big enough (NoMemory is not what this tests)
  Card card;
  o3os::platform::host::DirStorage storage(card.root.string());
  NEED(storage.mount() == o3os::hal::storage::Status::Ok);
  ModuleSet set;
  ModuleLoader loader;
  alignas(64) static std::uint8_t slab[8192];
  alignas(64) static std::uint8_t sdram[192 * 1024];
  set.hot().init(slab, kHotAddr, sizeof slab);
  set.cold().init(sdram, kColdAddr, sizeof sdram);
  card.write("modules/importers/tracker.tmod", file);
  NEED(loader.begin(storage, set, "tracker", nullptr, true));  // the importer scan
  // and a playable scan of the same stem does not find it (review note 1: a missing file is reported at step, not at begin)
  {
    ModuleLoader l2;
    CHECK(l2.begin(storage, set, "tracker", nullptr));
    for (unsigned n = 0; n < 100 && l2.state() == ModuleLoader::State::Running; ++n) {
      (void)l2.step(1024);
    }
    CHECK(l2.state() == ModuleLoader::State::Failed);
    CHECK(l2.status() == Status::NotFound);
  }
  for (unsigned n = 0; n < 200000 && loader.state() == ModuleLoader::State::Running; ++n) {
    loader.step(1024);
  }
  NEED(loader.state() == ModuleLoader::State::Done);
  CHECK(set.importer_count() == 1);
  CHECK(set.count() == 0);
  const LoadedModule* m = set.importer_at(0);
  NEED(m != nullptr);
  CHECK(m->header.reserved2[1] == o3os::model::kImportLayoutTag);
  CHECK(m->desc.struct_size == 84);
  DescImp32 imp;
  Desc32 d = m->desc;
  DescExt32 ext = m->ext;
  CHECK(read_desc(m->header, m->place, d, &ext, &imp) == Status::Ok);
  Importer32 t;
  CHECK(read_importer32(m->header, m->place, imp, t));
  CHECK(t.format_count >= 1);
  CHECK(t.size == sizeof(Importer32));
}
