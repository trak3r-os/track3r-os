// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_fixture.h"

#include <o3os/formats/builtin.h>
#include <o3os/formats/crc32.h>
#include <o3os/formats/format.h>
#include <o3os/formats/og.h>
#include <o3os/model/params.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::test::og;
using formats::FormatDesc;
using formats::FormatError;
using formats::FormatKind;
using formats::FormatRegistry;
using formats::Progress;
namespace og_ns = o3os::formats::og;

namespace {

formats::BuiltinFormats& storage() {
  static formats::BuiltinFormats s;
  return s;
}

FormatRegistry& registry() {
  static FormatRegistry r;
  static const bool registered = formats::register_builtin_formats(r, storage());
  CHECK(registered);
  return r;
}

const FormatDesc* by_name(const char* name) { return registry().find_name(name); }

std::string id_of(const FormatDesc* d) { return d != nullptr ? d->id : "(none)"; }

Bytes head(std::initializer_list<std::uint8_t> bytes) { return Bytes(bytes); }

}  // namespace

TEST_CASE("format registry: the built-in formats, in order") {
  FormatRegistry& r = registry();
  NEED(r.count() == 13);
  const char* ids[13] = {"native", "og-project", "og-pattern", "og-pti", "og-mti", "og-pamd", "wav", "surge-wt", "iff-sample", "settings", "med", "mod", "s3m"};
  const FormatKind kinds[13] = {FormatKind::Project, FormatKind::ProjectSettings, FormatKind::Pattern,
                                FormatKind::Instrument, FormatKind::Instrument, FormatKind::PatternNames,
                                FormatKind::Sample, FormatKind::Sample, FormatKind::Sample, FormatKind::Settings, FormatKind::Project,
                                FormatKind::Project, FormatKind::Project};
  for (std::size_t i = 0; i < 13; ++i) {
    NEED(r.at(i) != nullptr);
    CHECK(std::string(r.at(i)->id) == ids[i]);
    CHECK(r.at(i)->kind == kinds[i]);
    CHECK(r.at(i)->reader != nullptr);
    CHECK(r.at(i)->sniff != nullptr);
    CHECK(r.find_id(ids[i]) == r.at(i));
  }
  CHECK(r.at(13) == nullptr);
  // what a project import dropped is nameable through the registry, whatever the format
  const FormatDesc* project = r.find_id("og-project");
  NEED(project != nullptr && project->group_name != nullptr);
  CHECK(std::string(project->group_name(0)) == "session");
  CHECK(std::string(project->group_name(og_ns::kDropGroupCount)) == "");
  CHECK(r.find_id("native")->group_name == nullptr);
  CHECK(r.find_id("og-mti")->writer == nullptr);  // Export writes .pti
  CHECK(r.find_id("wav")->writer == nullptr);     // a WAV is read into an instrument (OS-002c), never written by a format
  CHECK(r.find_id("og-pti")->writer != nullptr);
  CHECK(r.find_id("native")->writer != nullptr);
  CHECK(r.find_id("nope") == nullptr);
  CHECK(r.find_id(nullptr) == nullptr);
  CHECK(r.find_id("") == nullptr);
}

TEST_CASE("format registry: add refuses what cannot work") {
  FormatRegistry r;
  FormatDesc d;
  d.id = "x";
  CHECK(!r.add(d));  // neither a reader nor a writer
  d.reader = &storage().og_pamd_reader;
  CHECK(r.add(d));
  CHECK(!r.add(d));  // the id is taken
  FormatDesc empty;
  empty.reader = d.reader;
  CHECK(!r.add(empty));
  empty.id = nullptr;
  CHECK(!r.add(empty));
  CHECK(r.count() == 1);
  // the table is bounded
  const char* ids[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q"};
  std::size_t added = 0;
  for (const char* id : ids) {
    FormatDesc e;
    e.id = id;
    e.writer = &storage().og_pamd_writer;
    if (r.add(e)) ++added;
  }
  CHECK(added == FormatRegistry::kMaxFormats - 1);
  CHECK(r.count() == FormatRegistry::kMaxFormats);
}

TEST_CASE("format registry: finding a format by file name") {
  struct Row { const char* name; const char* id; };
  const Row rows[] = {
      {"project.mt", "og-project"},
      {"PROJECT.MT", "og-project"},
      {"Projects/Song/project.mt", "og-project"},
      {"pattern_01.mtp", "og-pattern"},
      {"a.b.MTP", "og-pattern"},
      {"instrument_01.mti", "og-mti"},
      {"01 Kick.pti", "og-pti"},
      {"C:\\card\\02 Snare.PTI", "og-pti"},
      {"patternsMetadata", "og-pamd"},
      {"Projects/Song/patterns/patternsmetadata", "og-pamd"},
      {"song.o3pj", "native"},
      {"SONG.O3PJ", "native"},
      {".mt", "(none)"},            // a dot first is no extension
      {"mt", "(none)"},
      {"noext", "(none)"},
      {"x.mtpx", "(none)"},
      {"x.mtp/y", "(none)"},        // the extension belongs to a folder
      {"x.mtp\\y", "(none)"},
      {"patternsMetadata.bak", "(none)"},
      {"._project.mt", "(none)"},          // macOS AppleDouble files, whatever they end with
      {"Projects/Song/._project.mt", "(none)"},
      {"._pattern_01.mtp", "(none)"},
      {"._01 Kick.pti", "(none)"},
      {"._patternsMetadata", "(none)"},
      {"a._b.mt", "og-project"},           // only a name that starts with ._ is one
      {"", "(none)"},
      {"/", "(none)"},
  };
  for (const Row& r : rows) CHECK_MESSAGE(id_of(by_name(r.name)) == r.id, "'" << r.name << "'");
  CHECK(registry().find_name(nullptr) == nullptr);
}

TEST_CASE("format registry: sniffing the magic") {
  FormatRegistry& r = registry();
  auto sniff = [&r](const Bytes& h, std::uint32_t size) { return id_of(r.sniff(h.data(), h.size(), size)); };
  const Bytes native = head({'F', 'O', 'R', 'M', 0, 0, 0, 0x10, 'O', '3', 'P', 'J', 0, 0, 0, 0});
  CHECK(sniff(native, 100) == "native");
  CHECK(sniff(Bytes(native.begin(), native.begin() + 11), 100) == "(none)");  // too short to tell
  CHECK(sniff(head({'M', 'T', 0, 0, 1, 9, 0, 255, 16, 16, 16, 16, 0x10, 9, 0, 0}), 2320) == "og-project");
  CHECK(sniff(head({'M', 'T'}), 0) == "og-project");
  CHECK(sniff(head({'M', 'T', 1, 0}), 0) == "(none)");                // type 1 is an instrument
  CHECK(sniff(head({'K', 'S', 2, 0}), 6184) == "og-pattern");
  CHECK(sniff(head({'K', 'S', 2, 0}), 9260) == "og-pattern");
  CHECK(sniff(head({'K', 'S', 2, 0}), 12336) == "og-pattern");  // v5, 16 tracks (OS-CEPAT S1b)
  CHECK(sniff(head({'K', 'S', 2, 0}), 8232) == "og-pattern");   // v6, 8 tracks (OS-CEPAT S1)
  CHECK(sniff(head({'K', 'S', 2, 0}), 12332) == "og-pattern");  // v6, 12 tracks
  CHECK(sniff(head({'K', 'S', 2, 0}), 16432) == "og-pattern");  // v6, 16 tracks
  CHECK(sniff(head({'P', 'M', 2, 0}), 0) == "og-pattern");
  CHECK(sniff(head({'K', 'S', 2, 0}), 1000) == "(none)");
  CHECK(sniff(head({'T', 'I', 1, 0}), 392) == "og-mti");
  CHECK(sniff(head({'T', 'I', 1, 0}), 388) == "og-mti");
  CHECK(sniff(head({'T', 'I', 1, 0}), 0) == "og-pti");                // size unknown: an .mti has one of two sizes, so not an .mti
  CHECK(sniff(head({'T', 'I', 1, 0}), 392 + 2000) == "og-pti");
  CHECK(sniff(head({'T', 'I', 2, 0}), 392) == "(none)");
  CHECK(sniff(head({'P', 'A', 'M', 'D', 1, 0}), 12816) == "og-pamd");
  CHECK(sniff(head({'P', 'A', 'M'}), 0) == "(none)");
  CHECK(sniff(head({0, 0, 0, 0}), 0) == "(none)");
  CHECK(r.sniff(nullptr, 4, 0) == nullptr);
  CHECK(r.sniff(native.data(), 0, 0) == nullptr);
}

TEST_CASE("format registry: the name decides, the bytes must agree") {
  FormatRegistry& r = registry();
  const Bytes mt = head({'M', 'T', 0, 0});
  const Bytes ks = head({'K', 'S', 2, 0});
  CHECK(id_of(r.identify("project.mt", mt.data(), mt.size(), 0)) == "og-project");
  CHECK(id_of(r.identify("pattern_01.mtp", ks.data(), ks.size(), 6184)) == "og-pattern");
  CHECK(id_of(r.identify("pattern_01.mtp", mt.data(), mt.size(), 6184)) == "(none)");   // called .mtp, is not
  CHECK(id_of(r.identify("pattern_01.mtp", ks.data(), ks.size(), 100)) == "(none)");    // wrong size
  CHECK(id_of(r.identify("pattern_01.mtp", nullptr, 0, 0)) == "og-pattern");            // no bytes offered: the name
  CHECK(id_of(r.identify("whatever", ks.data(), ks.size(), 9260)) == "og-pattern");     // unknown name: the bytes
  CHECK(id_of(r.identify("whatever", mt.data(), mt.size(), 0)) == "og-project");
  CHECK(id_of(r.identify("whatever", head({1, 2, 3, 4}).data(), 4, 0)) == "(none)");
  CHECK(id_of(r.identify(nullptr, ks.data(), ks.size(), 6184)) == "og-pattern");
  // AppleDouble: neither the name nor the bytes may make it a project, even when the bytes look like one
  CHECK(id_of(r.identify("._project.mt", mt.data(), mt.size(), 2320)) == "(none)");
  CHECK(id_of(r.identify("Projects/Song/._project.mt", mt.data(), mt.size(), 2320)) == "(none)");
  CHECK(id_of(r.identify("._whatever", mt.data(), mt.size(), 0)) == "(none)");
  CHECK(id_of(r.identify("project.mt", mt.data(), mt.size(), 2320)) == "og-project");
  // a .pti of unknown size is never taken for an .mti; an .mti is told by its size
  const Bytes ti = head({'T', 'I', 1, 0});
  CHECK(id_of(r.identify("whatever", ti.data(), ti.size(), 0)) == "og-pti");
  CHECK(id_of(r.identify("whatever", ti.data(), ti.size(), 392)) == "og-mti");
  CHECK(id_of(r.identify("whatever", ti.data(), ti.size(), 4000)) == "og-pti");
  CHECK(id_of(r.identify("01 Kick.pti", ti.data(), ti.size(), 0)) == "og-pti");
  CHECK(id_of(r.identify("instrument_01.mti", ti.data(), ti.size(), 0)) == "og-mti");  // the name says .mti, the size is not known
  CHECK(id_of(r.identify("instrument_01.mti", ti.data(), ti.size(), 392)) == "og-mti");
}

namespace {

// A folder of synthetic OG files, as the app would walk it.
struct FolderFile {
  std::string name;
  Bytes bytes;
};

std::vector<FolderFile> synthetic_folder() {
  std::vector<FolderFile> f;
  ProjectSpec ps;
  ps.version = 12;
  ps.fill = ProjectSpec::Fill::Groups;
  PatternSpec p1 = rich_pattern(8, 4, 21);
  PatternSpec p2 = rich_pattern(12, 5, 22);
  ps.bitmask[0] = usage_byte(p1);
  ps.bitmask[1] = usage_byte(p2);
  ps.playlist = {1, 2, 2, 1};
  f.push_back({"project.mt", project_bytes(ps)});
  f.push_back({"pattern_01.mtp", pattern_bytes(p1)});
  f.push_back({"pattern_02.mtp", pattern_bytes(p2)});
  InstrumentSpec mti;
  mti.version = 4;
  mti.pti = false;
  f.push_back({"instrument_01.mti", instrument_bytes(mti)});
  InstrumentSpec pti;
  pti.frames = 321;
  pti.file_name = "Snare.wav";
  f.push_back({"02 Snare.pti", instrument_bytes(pti)});
  f.push_back({"patternsMetadata", pamd_bytes({"Intro", "Verse"})});
  return f;
}

}  // namespace

TEST_CASE("format registry: strategies drive an OG import, a native save and a native load") {
  FormatRegistry& r = registry();
  model::ModelArena& a = arena(0);
  a.project.reset();
  // the order of an import: project settings, patterns, instruments, names last
  const std::vector<FolderFile> folder = synthetic_folder();
  const og_ns::FileClass order[] = {og_ns::FileClass::Project, og_ns::FileClass::Pattern, og_ns::FileClass::Mti,
                                    og_ns::FileClass::Pti, og_ns::FileClass::PatternNames};
  unsigned imported = 0;
  for (og_ns::FileClass want : order) {
    for (const FolderFile& file : folder) {
      const og_ns::FileName n = og_ns::classify_file_name(file.name.c_str());
      if (n.file_class != want) continue;
      const FormatDesc* d = r.identify(file.name.c_str(), file.bytes.data(), file.bytes.size(),
                                       static_cast<std::uint32_t>(file.bytes.size()));
      NEED(d != nullptr);
      NEED(d->reader != nullptr);
      formats::ReadParams params;
      params.index = n.index;
      CHECK_MESSAGE(run_reader(*d->reader, file.bytes, a, params) == Progress::Done,
                    file.name << ": " << d->reader->report().detail);
      CHECK(d->reader->report().error == FormatError::None);
      if (n.file_class == og_ns::FileClass::Project) {  // the report through the common interface lists what was dropped
        CHECK(d->reader->report().dropped_groups != 0);
        CHECK(d->reader->report().sanitised == 0);
      }
      ++imported;
    }
  }
  CHECK(imported == 6);
  const model::Project& p = a.project;
  CHECK(p.pattern_count() == 2);
  CHECK(p.instrument(0) != nullptr);
  CHECK(p.instrument(1) != nullptr);
  CHECK(std::string(p.pattern(1)->name) == "Intro");
  CHECK(std::string(p.pattern(2)->name) == "Verse");
  CHECK(p.song_length() == 4);
  CHECK(p.instrument_count() == 18);  // two files and the 16 MIDI channels
  CHECK(p.check_invariants());

  // save natively through the strategy, load it back into the other arena, and save again: the same bytes
  const FormatDesc* native = r.find_id("native");
  NEED(native != nullptr);
  Bytes saved;
  NEED(run_writer(*native->writer, p, saved) == Progress::Done);
  CHECK(saved.size() > 100);
  CHECK(r.identify("song.o3pj", saved.data(), saved.size(), static_cast<std::uint32_t>(saved.size())) == native);
  model::ModelArena& b = arena(1);
  NEED(run_reader(*native->reader, saved, b, {}) == Progress::Done);
  Bytes again;
  NEED(run_writer(*native->writer, b.project, again) == Progress::Done);
  CHECK(again == saved);
  CHECK(native->reader->report().patterns == 2);
  CHECK(native->reader->report().instruments == 18);
}

TEST_CASE("format registry: the native strategy reports in the common terms") {
  FormatRegistry& r = registry();
  const FormatDesc* native = r.find_id("native");
  NEED(native != nullptr);
  model::ModelArena& a = arena(0);
  a.project.reset();
  Bytes saved;
  NEED(run_writer(*native->writer, a.project, saved) == Progress::Done);
  Bytes bad = saved;
  bad[saved.size() - 1] ^= 1;  // the CRC
  CHECK(run_reader(*native->reader, bad, arena(1), {}) == Progress::Failed);
  CHECK(native->reader->report().error == FormatError::BadCrc);
  CHECK(std::string(formats::format_error_name(native->reader->report().error)) == "bad CRC");
  bad = saved;
  bad[20] = 2;  // the PHDR chunk's version: a newer major
  CHECK(run_reader(*native->reader, bad, arena(1), {}) == Progress::Failed);
  CHECK(native->reader->report().error == FormatError::UnsupportedVersion);
  bad = saved;
  bad[0] = 'X';
  CHECK(run_reader(*native->reader, bad, arena(1), {}) == Progress::Failed);
  CHECK(native->reader->report().error == FormatError::BadMagic);
  CHECK(run_reader(*native->reader, Bytes(saved.begin(), saved.begin() + 40), arena(1), {}) == Progress::Failed);
  CHECK(native->reader->report().error == FormatError::Truncated);
  CHECK(native->reader->progress() == Progress::Failed);
  // every error has a name
  for (unsigned e = 0; e <= static_cast<unsigned>(FormatError::Internal); ++e) {
    CHECK(std::strlen(formats::format_error_name(static_cast<FormatError>(e))) > 0);
  }
  CHECK(std::string(formats::format_error_name(FormatError::Unsupported)) == "unsupported content");
}

#if !defined(__arm__)  // (a second 29 KB set of strategies: the target's heap is small, the host run covers it)
TEST_CASE("format registry: a strategy that was never begun is failed and harmless") {
  static formats::BuiltinFormats untouched;
  FormatRegistry r;
  CHECK(formats::register_builtin_formats(r, untouched));
  for (std::size_t i = 0; i < r.count(); ++i) {
    const FormatDesc* d = r.at(i);
    NEED(d != nullptr);
    if (d->reader != nullptr) {
      CHECK(d->reader->progress() == Progress::Failed);
      CHECK(d->reader->step(100) == Progress::Failed);
      CHECK(d->reader->step(0) == Progress::Failed);
    }
    if (d->writer != nullptr) {
      CHECK(d->writer->progress() == Progress::Failed);
      CHECK(d->writer->step(100) == Progress::Failed);
      CHECK(d->writer->step(0) == Progress::Failed);
    }
  }
}

#endif

TEST_CASE("format registry: a step budget of zero moves nothing and a run still finishes") {
  const Bytes f = pattern_bytes(rich_pattern(12, 5, 5));
  model::Project& proj = arena(0).project;
  proj.reset();
  formats::ReadParams params;
  params.index = 3;
  test::MemorySource src(f);
  formats::og::PatternReader& r = storage().og_pattern_reader;
  r.begin(src, arena(0), params);
  for (int i = 0; i < 10; ++i) CHECK(r.step(0) == Progress::Running);
  CHECK(src.position() == 0);
  while (r.step(100000) == Progress::Running) {}
  CHECK(r.progress() == Progress::Done);
  CHECK(proj.pattern(3) != nullptr);
}

// ---- the device settings file as a registered format (OS-002d) ---------------------------------------------------------------

TEST_CASE("format registry: the device settings file is registered, found by name and by its bytes, read and written through its strategies") {
  FormatRegistry& r = registry();
  const FormatDesc* d = r.find_id("settings");
  NEED(d != nullptr);
  CHECK(d->kind == FormatKind::Settings);
  NEED(d->reader != nullptr);
  NEED(d->writer != nullptr);
  CHECK(r.find_name("settings.o3st") == d);
  CHECK(r.find_name("/sd/SETTINGS.O3ST") == d);
  CHECK(r.find_name("settings.o3pj") == r.find_id("native"));

  model::Settings s;
  std::memset(&s, 0, sizeof s);
  s.init(core::ParamKind::Device);
  CHECK(s.set(model::device_setting::MetronomeVolume, 64.0f) == model::Status::Ok);
  CHECK(s.set(model::device_setting::ClockSync, -3.0f) == model::Status::Ok);
  CHECK(s.set_learn(0, core::LearnBinding{core::param_id(core::ParamKind::Track, 1, 0), 7, 2, 1, 0}) == model::Status::Ok);
  formats::WriteParams wp;
  wp.settings = &s;
  Bytes file;
  const model::Project& any_project = arena(0).project;  // the strategy does not look at it
  CHECK(run_writer(*d->writer, any_project, file, wp) == Progress::Done);
  CHECK(d->writer->report().error == FormatError::None);
  CHECK(d->writer->report().bytes == file.size());
  CHECK(file.size() == 222u + 7u * 8u + 8u + 4u + 8u);  // + the two entries of TRANSPORT IN and OUT (E-CFG-03), the one of PAT SWITCH (E-PAT-35), the one of CC MAP (OS-MIDICCMAP S1) the two of MTC OUT and MTC START (OS-MTC A3) and the one of MTC IN (OS-MTC B3)
  // Found by its bytes (the head the registry offers) and by name-and-bytes; not mistaken for a project, nor a project for it.
  CHECK(r.sniff(file.data(), 16, static_cast<std::uint32_t>(file.size())) == d);
  CHECK(r.identify("settings.o3st", file.data(), 16, static_cast<std::uint32_t>(file.size())) == d);
  CHECK(r.identify("whatever.bin", file.data(), 16, static_cast<std::uint32_t>(file.size())) == d);
  model::ModelArena& a = arena(0);
  a.project.reset();
  Bytes project_file;
  CHECK(run_writer(*r.find_id("native")->writer, a.project, project_file) == Progress::Done);
  CHECK(r.sniff(project_file.data(), 16, static_cast<std::uint32_t>(project_file.size())) == r.find_id("native"));
  CHECK(r.identify("settings.o3st", project_file.data(), 16, static_cast<std::uint32_t>(project_file.size())) == nullptr);

  // Read it back, whatever the budget, the size of a piece and the stalls of the source.
  for (const std::size_t budget : {std::size_t{1}, std::size_t{13}, std::size_t{4096}}) {
    for (const std::size_t piece : {std::size_t{1}, std::size_t{5}, static_cast<std::size_t>(-1)}) {
      model::Settings back;
      std::memset(&back, 0, sizeof back);
      formats::ReadParams rp;
      rp.settings = &back;
      CHECK(run_reader(*d->reader, file, a, rp, budget, piece, piece == 5 ? 3u : 0u) == Progress::Done);
      CHECK(d->reader->report().error == FormatError::None);
      CHECK(d->reader->report().sanitised == 0u);
      CHECK_FALSE(d->reader->report().unverified_version);
      CHECK(d->reader->report().bytes == file.size());
      CHECK(back.get(model::device_setting::MetronomeVolume) == 64.0f);
      CHECK(back.get(model::device_setting::ClockSync) == -3.0f);
      CHECK(back.learn(0).cc == 7);
      CHECK(back.check_invariants());
    }
  }
  // A file that fails leaves the defaults, said through the common report.
  const auto fails = [&](const Bytes& in, FormatError want, const char* what) {
    model::Settings back;
    std::memset(&back, 0, sizeof back);
    back.init(core::ParamKind::Device);
    (void)back.set(model::device_setting::MetronomeVolume, 3.0f);
    formats::ReadParams rp;
    rp.settings = &back;
    CHECK_MESSAGE(run_reader(*d->reader, in, a, rp) == Progress::Failed, what);
    CHECK_MESSAGE(d->reader->report().error == want, what << ": " << d->reader->report().detail);
    CHECK_MESSAGE(back.is_default(), what);
  };
  Bytes damaged = file;
  damaged[30] ^= 1;
  fails(damaged, FormatError::BadCrc, "one flipped bit");
  fails(Bytes(file.begin(), file.begin() + 50), FormatError::Truncated, "cut");
  fails(Bytes{}, FormatError::Truncated, "empty");
  fails(project_file, FormatError::BadMagic, "a project file");
  fails(Bytes(1100, 0x20), FormatError::LimitExceeded, "more bytes than a settings file can have");
  Bytes newer = file;  // the SETS chunk's major version raised, the CRC made good again
  newer[20] = 2;
  const std::uint32_t crc = formats::crc32(newer.data(), newer.size() - 14);
  for (int i = 0; i < 4; ++i) {
    newer[newer.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (24 - 8 * i));
  }
  fails(newer, FormatError::UnsupportedVersion, "a newer major");
  // The failures of the byte streams, and a call without a block.
  {
    model::Settings back;
    std::memset(&back, 0, sizeof back);
    formats::ReadParams rp;
    rp.settings = &back;
    test::MemorySource src(file);
    src.fail_at = 20;
    d->reader->begin(src, a, rp);
    Progress p = Progress::Running;
    for (int i = 0; i < 100 && p == Progress::Running; ++i) {
      p = d->reader->step(4096);
    }
    CHECK(p == Progress::Failed);
    CHECK(d->reader->report().error == FormatError::SourceFailed);
    test::MemorySource lying(file);
    lying.lie = test::MemorySource::Lie::TooMany;
    d->reader->begin(lying, a, rp);
    CHECK(d->reader->step(4096) == Progress::Failed);
    CHECK(d->reader->report().error == FormatError::SourceFailed);
    formats::ReadParams none;
    CHECK(run_reader(*d->reader, file, a, none) == Progress::Failed);
    CHECK(d->reader->report().error == FormatError::Internal);
  }
  {
    Bytes out;
    CHECK(run_writer(*d->writer, any_project, out, formats::WriteParams{}) == Progress::Failed);  // no block
    CHECK(d->writer->report().error == FormatError::Internal);
    model::Settings project_scope;
    std::memset(&project_scope, 0, sizeof project_scope);
    project_scope.init(core::ParamKind::Project);
    formats::WriteParams bad;
    bad.settings = &project_scope;
    CHECK(run_writer(*d->writer, any_project, out, bad) == Progress::Failed);  // not the device's
    CHECK(d->writer->report().error == FormatError::Malformed);
    test::VectorSink sink;
    sink.fail_after = 10;
    d->writer->begin(any_project, sink, wp);
    Progress p = Progress::Running;
    for (int i = 0; i < 100 && p == Progress::Running; ++i) {
      p = d->writer->step(4096);
    }
    CHECK(p == Progress::Failed);
    CHECK(d->writer->report().error == FormatError::SinkFailed);
    test::VectorSink stalling;
    stalling.block_every = 2;
    stalling.max_per_write = 7;
    d->writer->begin(any_project, stalling, wp);
    p = Progress::Running;
    for (int i = 0; i < 10000 && p == Progress::Running; ++i) {
      p = d->writer->step(11);
    }
    CHECK(p == Progress::Done);
    CHECK(stalling.bytes == file);  // however it was cut up, the same bytes
  }
}
