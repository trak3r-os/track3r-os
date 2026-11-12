// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FILES-C slices C2a and C2b (Level C): a whole project in the OG format, part one (app/og_export.h, SaveService::start_og_export, docs/specs/OS-FILES-C.md sections 6, 10): the folder and file names
// (T11), the files the OG writers make for the patterns, their names file and project.mt, checked byte for byte against the synthetic fixture files and read back by the OS's own OG loader (T1), the
// order (project.mt last), the faults of the card at every mutating call and at every block of a real FAT32 volume (T6), the snapshot of the document (T12) and the pattern names file that is not
// given back when the set of patterns has changed (T15). The guard's rows are tests/save_guard_test.cpp (T7). C2b (the end of this file): the instruments with the PCM stage, read back by the OS's own
// loader with its samples (T1 whole, T8), the counters of every lossy row (T4), the boundaries (T5) and the faults over a set with instruments (T6).
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/og_fixture.h"
#include "support/project_fixture.h"

#include "../platform/mk66/tests/mk66_storage_fakes.h"

#include <o3os/app/og_export.h>
#include <o3os/app/og_folder.h>
#include <o3os/app/og_pcm.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/pti_load.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/save_service.h>
#include <o3os/audio/voices.h>
#include <o3os/core/job.h>
#include <o3os/formats/native.h>
#include <o3os/formats/og.h>
#include <o3os/hal/save_guard.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/mk66/save/fat_storage_writer.h>
#include <o3os/platform/mk66/storage/fat_storage.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::test::og;
using formats::Progress;
using o3os::test::MemCard;
namespace og_ns = o3os::formats::og;
namespace guard = o3os::hal::storage::save;
using OgState = app::SaveService::OgState;
using Start = app::SaveService::Start;
using Bytes = std::vector<std::uint8_t>;

namespace {

// ---- The project the tests export: the synthetic OG fixture files, imported ----------------------------------------------------------------------------------------------------------------------

struct Fixture {
  ProjectSpec spec;
  std::vector<std::pair<unsigned, PatternSpec>> patterns;
  std::vector<std::string> names;
};

Fixture make_fixture(std::size_t count = 4) {
  Fixture f;
  const unsigned ids[] = {1, 12, 2, 3, 40, 100, 255};
  std::uint32_t seed = 70;
  for (std::size_t i = 0; i < count && i < sizeof ids / sizeof ids[0]; ++i) {
    f.patterns.push_back({ids[i], rich_pattern(12, 5, seed++)});
  }
  f.spec.playlist = {1, 2, 2, 3, 12};
  for (const auto& p : f.patterns) {
    f.spec.bitmask[p.first - 1] = usage_byte(p.second);
  }
  f.names.assign(256, "");
  const char* titles[] = {"Intro", "Verse A", "Chorus", "Outro", "Bridge", "Coda", "Hidden"};
  for (std::size_t i = 0; i < f.patterns.size(); ++i) {
    f.names[f.patterns[i].first - 1] = titles[i];
  }
  return f;
}

// The files an import of `f` would be written back as: project.mt, the patterns and the names file.
void import_fixture(const Fixture& f, model::ModelArena& a) {
  static og_ns::ProjectReader project;
  static og_ns::PatternReader pattern;
  static og_ns::PamdReader pamd;
  NEED(run_reader(project, project_bytes(f.spec), a) == Progress::Done);
  for (const auto& p : f.patterns) {
    formats::ReadParams rp;
    rp.index = static_cast<std::uint16_t>(p.first);
    NEED(run_reader(pattern, pattern_bytes(p.second), a, rp) == Progress::Done);
  }
  NEED(run_reader(pamd, pamd_bytes(f.names), a) == Progress::Done);
}

std::string lower(std::string s) {
  for (char& c : s) {
    c = guard::detail::lower(c);
  }
  return s;
}

// The files the card must hold after a whole export of `f` into `folder` ("Export/Song OG"): path (lower case, as MemCard keys it) -> bytes.
std::map<std::string, Bytes> expected_files(const Fixture& f, const std::string& folder) {
  std::map<std::string, Bytes> out;
  const std::string base = lower(folder);
  out[base + "/project.mt"] = project_bytes(f.spec);
  out[base + "/patterns/patternsmetadata"] = pamd_bytes(f.names);
  for (const auto& p : f.patterns) {
    char name[24];
    CHECK(app::og_pattern_file_name(name, sizeof name, p.first) != 0u);
    out[base + "/patterns/" + lower(name)] = pattern_bytes(p.second);
  }
  return out;
}

// ---- The service over a MemCard, one pass of the main loop at a time --------------------------------------------------------------------------------------------------------------------------------

struct Rig {
  MemCard card;
  std::unique_ptr<app::SaveService> saves = std::make_unique<app::SaveService>();
  core::JobRunner jobs;
  platform::host::FakeClock clock;
  model::Settings settings;
  std::uint64_t now_ms = 0;
  unsigned frames_run = 0;

  Rig() {
    saves->attach(card, card);
    settings.init(core::ParamKind::Device);
    frame();
  }
  // One pass: the jobs get a slice, the service takes the ended one in and starts the next, a finished slot is released (App::report_jobs).
  void frame() {
    (void)jobs.run(clock, 2000);
    clock.advance(2000);
    now_ms += 16;
    saves->poll(jobs, settings, now_ms);
    for (unsigned slot = 0; slot < core::JobRunner::kSlots; ++slot) {
      const core::SlotState st = jobs.state(slot);
      if (st != core::SlotState::Free && st != core::SlotState::Running) {
        jobs.release(slot);
      }
    }
    ++frames_run;
  }
  Start start(const model::Project& p, const char* loaded = "Projects/Song", const app::SampleBank* bank = nullptr) { return saves->start_og_export(jobs, p, loaded, bank); }
  // Frames until the export has ended (the service idle, no slot occupied); `each` is called before every frame with its number. False when it never ended.
  bool run(const std::function<void(unsigned)>& each = {}, unsigned max = 200000) {
    for (unsigned i = 0; i < max; ++i) {
      if (each) {
        each(i);
      }
      frame();
      if (!saves->busy() && jobs.occupied() == 0u) {
        return true;
      }
    }
    return false;
  }
};

bool is_temp(const std::string& path) { return path.size() >= 11 && path.compare(path.size() - 11, 11, "_o3save.tmp") == 0; }

// What must hold on the card after ANY export, whole or cut: nothing but the expected OG files and temps in the folders of the export; every OG file under its final name is whole (the bytes the fault-free
// export made); project.mt is there only when every other file is, and the export says Done only then; no folder, no file of anything else; the guard never refused a call; only temps were removed.
void check_after(const MemCard& card, const std::map<std::string, Bytes>& expected, const std::string& folder, OgState state) {
  const std::string base = lower(folder);
  for (const auto& kv : card.files) {
    if (is_temp(kv.first)) {
      const std::string dir = kv.first.substr(0, kv.first.size() - 12);
      CHECK_MESSAGE((dir == base || dir == base + "/patterns" || dir == base + "/instruments"), "a temp outside the export's folders: " << kv.first);
      continue;
    }
    const auto it = expected.find(kv.first);
    CHECK_MESSAGE(it != expected.end(), "a file that is not part of the export: " << kv.first);
    if (it != expected.end()) {
      CHECK_MESSAGE(kv.second == it->second, "a partial or wrong file under its final name: " << kv.first);
    }
  }
  const bool project = card.files.count(base + "/project.mt") != 0u;
  if (project) {
    for (const auto& kv : expected) {
      CHECK_MESSAGE(card.files.count(kv.first) != 0u, "project.mt exists but " << kv.first << " does not");
    }
  }
  CHECK_MESSAGE((state == OgState::Done) == project, "state " << static_cast<int>(state) << ", project.mt " << project);
  for (const std::string& d : card.dirs) {
    CHECK_MESSAGE((d == "export" || d == base || d == base + "/patterns" || d == base + "/instruments"), "a folder that is not the export's: " << d);
  }
  CHECK(card.violations == 0u);
  for (const std::string& r : card.removed()) {
    CHECK_MESSAGE(is_temp(lower(r)), "removed: " << r);
  }
}

model::ModelArena& export_arena() { return arena(0); }

// A fault-free export of `f` on a fresh card: the number of mutating calls it takes, the bytes it writes in all, the frames it runs, the order of the renames.
struct Reference {
  std::size_t ops = 0;
  std::size_t bytes = 0;
  unsigned frames = 0;
  std::vector<std::string> renames;
  std::map<std::string, Bytes> files;
};

Reference reference(const Fixture& f) {
  Reference r;
  import_fixture(f, export_arena());
  Rig rig;
  CHECK(rig.start(export_arena().project) == Start::Started);
  const unsigned before = rig.frames_run;
  CHECK(rig.run());
  r.frames = rig.frames_run - before;
  r.ops = rig.card.ops;
  for (const std::string& l : rig.card.log) {
    if (l.rfind("rename ", 0) == 0) {
      r.renames.push_back(l.substr(7));
    }
  }
  for (const auto& kv : rig.card.files) {
    r.bytes += kv.second.size();
  }
  r.files = rig.card.files;
  return r;
}

}  // namespace

// ---- T11: names ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("og export T11: the folder is <project> OG, then OG 2 .. OG 99, cut to leave room, and always a name the guard takes") {
  char out[guard::kNameMax + 1];
  const auto name = [&](const char* project, unsigned n) {
    return app::og_folder_name(out, sizeof out, project, n) ? std::string(out) : std::string("<refused>");
  };
  CHECK(name("Song", 1) == "Song OG");
  CHECK(name("Song", 2) == "Song OG 2");
  CHECK(name("Song", 9) == "Song OG 9");
  CHECK(name("Song", 10) == "Song OG 10");
  CHECK(name("Song", 99) == "Song OG 99");
  CHECK(name("Song", 100) == "<refused>");
  CHECK(name("Song", 0) == "<refused>");
  CHECK(name(nullptr, 1) == "Untitled OG");
  CHECK(name("", 1) == "Untitled OG");
  CHECK(name(" ", 3) == "Untitled OG 3");
  CHECK(name("...", 1) == "Untitled OG");
  // a name of 31 bytes is cut to 25 for every n alike, so the folder name does not move from one n to the next
  const std::string long31(guard::kNameMax, 'x');
  CHECK(name(long31.c_str(), 1) == std::string(guard::kOgNameRoom, 'x') + " OG");
  CHECK(name(long31.c_str(), 99) == std::string(guard::kOgNameRoom, 'x') + " OG 99");
  CHECK(std::string(std::string(guard::kOgNameRoom, 'x') + " OG 99").size() == guard::kNameMax);
  // a cut that leaves a space or a dot at the end loses it (the guard takes neither)
  const std::string spaced = std::string(guard::kOgNameRoom - 1, 'a') + " tail of the name";
  CHECK(name(spaced.c_str(), 1) == std::string(guard::kOgNameRoom - 1, 'a') + " OG");
  const std::string dotted = std::string(guard::kOgNameRoom - 2, 'a') + "..b";
  CHECK(name(dotted.c_str(), 1) == std::string(guard::kOgNameRoom - 2, 'a') + " OG");
  // every result is a guard-valid Export/<name> folder and an OG folder
  for (const char* project : {"Song", "Track 01", "My-Song_2.v3", "x", "A B C", "Untitled", "REC_001", "a.b"}) {
    for (unsigned n = 1; n <= guard::kOgFolderMaxN; ++n) {
      NEED(app::og_folder_name(out, sizeof out, project, n));
      CHECK(guard::valid_name(out));
      CHECK(guard::og_export_folder(out));
      CHECK(guard::classify((std::string("Export/") + out).c_str()) == guard::Place::Folder);
    }
  }
  // a buffer that is too small is refused, not cut
  char tiny[7];  // ("Song OG" is 7 bytes and needs 8)
  CHECK_FALSE(app::og_folder_name(tiny, sizeof tiny, "Song", 1));
  CHECK(tiny[0] == '\0');
}

TEST_CASE("og export T11: pattern file names are pattern_NN.mtp for 1..255, guard-valid, and the OG classifier reads them back as the right pattern") {
  char name[24];
  CHECK(app::og_pattern_file_name(name, sizeof name, 0) == 0u);
  CHECK(app::og_pattern_file_name(name, sizeof name, 256) == 0u);
  CHECK(app::og_pattern_file_name(name, 10, 1) == 0u);
  CHECK(app::og_pattern_file_name(nullptr, sizeof name, 1) == 0u);
  NEED(app::og_pattern_file_name(name, sizeof name, 1) == 14u);
  CHECK(std::string(name) == "pattern_01.mtp");
  NEED(app::og_pattern_file_name(name, sizeof name, 9) == 14u);
  CHECK(std::string(name) == "pattern_09.mtp");
  NEED(app::og_pattern_file_name(name, sizeof name, 99) == 14u);
  CHECK(std::string(name) == "pattern_99.mtp");
  NEED(app::og_pattern_file_name(name, sizeof name, 100) == 15u);
  CHECK(std::string(name) == "pattern_100.mtp");
  NEED(app::og_pattern_file_name(name, sizeof name, 255) == 15u);
  CHECK(std::string(name) == "pattern_255.mtp");
  for (unsigned id = 1; id <= 255; ++id) {
    NEED(app::og_pattern_file_name(name, sizeof name, id) != 0u);
    const og_ns::FileName fn = og_ns::classify_file_name(name);
    CHECK(fn.file_class == og_ns::FileClass::Pattern);
    CHECK(fn.index == id);
    const std::string path = std::string("Export/Song OG/patterns/") + name;
    CHECK(guard::classify(path.c_str()) == guard::Place::Target);
  }
  // the other two files
  CHECK(og_ns::classify_file_name(guard::kProjectMtFile).file_class == og_ns::FileClass::Project);
  CHECK(og_ns::classify_file_name(guard::kOgPatternsMetadata).file_class == og_ns::FileClass::PatternNames);
  CHECK(guard::classify("Export/Song OG/project.mt") == guard::Place::Target);
  CHECK(guard::classify("Export/Song OG/patterns/patternsMetadata") == guard::Place::Target);
}

TEST_CASE("og export T11: the .pti names C2b will write (og_pti_stem) are all in the guard's row and read back as the right slot") {
  // (the instruments are written by C2b; the row and the names are fixed here so that the guard is reviewed once)
  const char* names[] = {"Kick", "Kick (hard) & snare's", "caf\xC3\xA9", "CON", "NUL.txt", "trailing.", "trailing ", "", "a/b\\c:d*e?f\"g<h>i|j", "Long Name With Spaces And More Characters",
                         "0123456789012345678901234567890123456789", "Kick", "x"};
  for (unsigned slot = 0; slot < 48; ++slot) {
    for (const char* n : names) {
      char stem[40];
      NEED(app::og_pti_stem(slot, n, stem, sizeof stem) != 0u);
      CHECK(guard::valid_name(stem));
      const std::string file = std::string(stem) + ".pti";
      const std::string path = std::string("Export/Song OG/instruments/") + file;
      CHECK_MESSAGE(guard::classify(path.c_str()) == guard::Place::Target, path);
      const og_ns::FileName fn = og_ns::classify_file_name(file.c_str());
      CHECK(fn.file_class == og_ns::FileClass::Pti);
      CHECK(fn.index == slot);
      CHECK(path.size() + 1u <= guard::kPathCap);
    }
  }
}

// ---- T1: the files, byte for byte, in order, and read back by the OS's own loader --------------------------------------------------------------------------------------------------------------

TEST_CASE("og export T1: the patterns, their names file and project.mt are the fixture files byte for byte, project.mt is renamed last, and the OS's own loader reads the folder back") {
  const Fixture f = make_fixture();
  import_fixture(f, export_arena());
  CHECK_FALSE(export_arena().project.names_stale());
  Rig rig;
  NEED(rig.start(export_arena().project) == Start::Started);
  CHECK(rig.saves->og_state() == OgState::Running);
  CHECK(rig.saves->og_files_total() == 4u + 2u);
  CHECK(std::string(rig.saves->og_folder()) == "Export/Song OG");
  NEED(rig.run());
  CHECK(rig.saves->og_state() == OgState::Done);
  CHECK(rig.saves->og_files_done() == 6u);
  const std::map<std::string, Bytes> expected = expected_files(f, "Export/Song OG");
  CHECK(expected.size() == 6u);
  for (const auto& kv : expected) {
    const auto it = rig.card.files.find(kv.first);
    INFO(kv.first);
    NEED(it != rig.card.files.end());
    CHECK_MESSAGE(it->second == kv.second, kv.first);
  }
  check_after(rig.card, expected, "Export/Song OG", OgState::Done);
  CHECK(rig.card.files.size() == expected.size());  // no temp left
  CHECK(rig.saves->og_drops().any_lost() == false);
  // the order: the patterns by id, then the names file, project.mt last
  std::vector<std::string> finals;
  for (const std::string& l : rig.card.log) {
    const std::size_t arrow = l.find('>');
    if (l.rfind("rename ", 0) == 0 && arrow != std::string::npos) {
      finals.push_back(l.substr(arrow + 1));
    }
  }
  NEED(finals.size() == 6u);
  CHECK(finals[0] == "Export/Song OG/patterns/pattern_01.mtp");
  CHECK(finals[1] == "Export/Song OG/patterns/pattern_02.mtp");
  CHECK(finals[2] == "Export/Song OG/patterns/pattern_03.mtp");
  CHECK(finals[3] == "Export/Song OG/patterns/pattern_12.mtp");
  CHECK(finals[4] == "Export/Song OG/patterns/patternsMetadata");
  CHECK(finals[5] == "Export/Song OG/project.mt");
  // the folder is made once, the OG folder before its patterns/
  std::size_t mkdirs = 0;
  for (const std::string& l : rig.card.log) {
    mkdirs += l.rfind("mkdir ", 0) == 0 ? 1u : 0u;
  }
  CHECK(mkdirs == 3u);

  // the OS's own loader: the same project comes back, every pattern, in the OG layout (names file read with the reader: the loader leaves it to the caller)
  model::ModelArena& back = arena(1);
  static app::OgFolderLoader loader;
  NEED(loader.begin(rig.card, "Export/Song OG", back));
  app::OgFolderLoader::State st = app::OgFolderLoader::State::Running;
  for (unsigned i = 0; i < 100000 && st == app::OgFolderLoader::State::Running; ++i) {
    st = loader.step(4096);
  }
  NEED(st == app::OgFolderLoader::State::Done);
  CHECK(loader.report().project_ok);
  CHECK(loader.report().version == 16);
  CHECK(loader.report().patterns == 4u);
  CHECK(loader.report().patterns_failed == 0u);
  CHECK(loader.report().instruments == 0u);
  CHECK(loader.report().files_ignored == 1u);  // patternsMetadata
  static og_ns::PamdReader pamd;
  NEED(run_reader(pamd, *rig.card.get("Export/Song OG/patterns/patternsMetadata"), back) == Progress::Done);
  CHECK(back.project.pattern_count() == 4u);
  CHECK(back.project.check_invariants());
  // every 1:1 field: the model after the folder is the model before it (the native bytes are the whole model)
  std::vector<std::uint8_t> a;
  std::vector<std::uint8_t> b;
  NEED(test::save(export_arena().project, a) == Progress::Done);
  NEED(test::save(back.project, b) == Progress::Done);
  CHECK(a == b);
  // and an export of what came back is the same files again (idempotent)
  Rig again;
  NEED(again.start(back.project) == Start::Started);
  NEED(again.run());
  for (const auto& kv : expected) {
    const auto it = again.card.files.find(kv.first);
    INFO(kv.first);
    NEED(it != again.card.files.end());
    CHECK_MESSAGE(it->second == kv.second, kv.first);
  }
}

TEST_CASE("og export T1: the writers are stepped in small slices and the files are the same") {
  const Fixture f = make_fixture(2);
  import_fixture(f, export_arena());
  const std::map<std::string, Bytes> expected = expected_files(f, "Export/Song OG");
  Rig rig;
  NEED(rig.start(export_arena().project) == Start::Started);
  unsigned frames = 0;
  NEED(rig.run([&](unsigned) { ++frames; }));
  CHECK(frames > 6u);  // (one file a pass at the least)
  for (const auto& kv : expected) {
    const auto it = rig.card.files.find(kv.first);
    INFO(kv.first);
    NEED(it != rig.card.files.end());
    CHECK_MESSAGE(it->second == kv.second, kv.first);
  }
}

TEST_CASE("og export T1: the folder takes the project's name, and the next export never goes into a folder that exists") {
  const Fixture f = make_fixture(2);
  import_fixture(f, export_arena());
  const model::Project& proj = export_arena().project;
  Rig rig;
  for (unsigned n = 1; n <= 3; ++n) {
    NEED(rig.start(proj, "Projects/My Song") == Start::Started);
    const std::string expect = n == 1 ? "Export/My Song OG" : "Export/My Song OG " + std::to_string(n);
    CHECK(std::string(rig.saves->og_folder()) == expect);
    NEED(rig.run());
    CHECK(rig.saves->og_state() == OgState::Done);
    for (const auto& kv : expected_files(f, expect)) {
      const auto it = rig.card.files.find(kv.first);
      INFO(kv.first);
    NEED(it != rig.card.files.end());
      CHECK(it->second == kv.second);
    }
  }
  // a project that came from outside Projects/ is "Untitled"
  NEED(rig.start(proj, "Workspace") == Start::Started);
  CHECK(std::string(rig.saves->og_folder()) == "Export/Untitled OG");
  NEED(rig.run());
  // a folder of the same name in another case, or one that holds anything at all, is taken
  rig.card.make_dir_now("Export/untitled og 2");
  NEED(rig.start(proj, "Workspace") == Start::Started);
  CHECK(std::string(rig.saves->og_folder()) == "Export/Untitled OG 3");
  NEED(rig.run());
  CHECK(rig.card.violations == 0u);
  CHECK(rig.saves->og_state() == OgState::Done);
}

TEST_CASE("og export T11: OG .. OG 99 taken, the 100th export of a name says no free name and writes nothing") {
  const Fixture f = make_fixture(1);
  import_fixture(f, export_arena());
  Rig rig;
  rig.card.make_dir_now("Export/Song OG");
  for (unsigned n = 2; n <= 99; ++n) {
    rig.card.make_dir_now(("Export/Song OG " + std::to_string(n)).c_str());
  }
  const std::size_t ops = rig.card.ops;
  CHECK(rig.start(export_arena().project) == Start::NoName);
  CHECK(rig.card.ops == ops);
  CHECK(rig.saves->og_state() == OgState::Idle);
  CHECK_FALSE(rig.saves->busy());
  CHECK(rig.card.log.empty());
  // one gap is found
  Rig gap;
  gap.card.make_dir_now("Export/Song OG");
  for (unsigned n = 2; n <= 99; ++n) {
    if (n != 57) {
      gap.card.make_dir_now(("Export/Song OG " + std::to_string(n)).c_str());
    }
  }
  NEED(gap.start(export_arena().project) == Start::Started);
  CHECK(std::string(gap.saves->og_folder()) == "Export/Song OG 57");
  NEED(gap.run());
}

// ---- refusals -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("og export: refused with the reason and nothing written: a write protected card, a save running, the firmware update path, no patterns, no write layer") {
  const Fixture f = make_fixture(2);
  import_fixture(f, export_arena());
  const model::Project& proj = export_arena().project;
  {
    Rig rig;
    rig.card.gate_status = hal::storage::WriteStatus::WriteProtected;
    CHECK(rig.start(proj) == Start::NotWritable);
    CHECK(rig.card.log.empty());
    CHECK(rig.card.files.empty());
    CHECK(rig.card.dirs.empty());
    CHECK(rig.saves->og_state() == OgState::Idle);
  }
  {
    Rig rig;
    NEED(rig.start(proj) == Start::Started);
    CHECK(rig.start(proj) == Start::Busy);  // one at a time
    CHECK(rig.saves->busy());
    NEED(rig.run());
    CHECK(rig.saves->og_state() == OgState::Done);
  }
  {
    Rig rig;
    static bool claimed;
    claimed = true;
    rig.saves->attach_claim([]() noexcept { return claimed; });
    CHECK(rig.start(proj) == Start::CardInUse);
    CHECK(rig.card.log.empty());
    claimed = false;
    CHECK(rig.start(proj) == Start::Started);
    NEED(rig.run());
  }
  {
    // the firmware update path takes the card between two files: the set ends, the files finished stay, no project.mt
    Rig rig;
    static bool claimed;
    claimed = false;
    rig.saves->attach_claim([]() noexcept { return claimed; });
    NEED(rig.start(proj) == Start::Started);
    NEED(rig.run([&](unsigned i) { claimed = i >= 3; }));
    CHECK(rig.saves->og_state() == OgState::Failed);
    CHECK(std::string(rig.saves->og_why()) == "FIRMWARE UPDATE HAS THE CARD");
    CHECK_FALSE(rig.card.has("Export/Song OG/project.mt"));
  }
  {
    // a project without patterns has nothing to export
    model::Project& empty = arena(1).project;
    empty.reset();
    Rig rig;
    CHECK(empty.pattern_count() == 0u);
    CHECK(rig.start(empty) == Start::Empty);
    CHECK(rig.card.log.empty());
    CHECK_FALSE(rig.saves->busy());
  }
  {
    app::SaveService bare;
    core::JobRunner jobs;
    CHECK(bare.start_og_export(jobs, proj, "", nullptr) == Start::NoWriter);
  }
  {
    // another save or export in hand: the OG export waits for the user, and a stems/WAV save is refused while it runs (busy())
    Rig rig;
    NEED(rig.start(proj) == Start::Started);
    CHECK(rig.saves->busy());
    CHECK(rig.saves->save_project(rig.jobs, proj, "", true) == Start::Busy);
    NEED(rig.run());
  }
}

// ---- T6: faults ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("og export T6: a power cut at every mutating call of every file leaves whole files only, project.mt only after all the others, and the next export takes the next folder") {
  const Fixture f = make_fixture(3);
  const Reference ref = reference(f);
  const std::map<std::string, Bytes> expected = expected_files(f, "Export/Song OG");
  const std::map<std::string, Bytes> expected2 = expected_files(f, "Export/Song OG 2");
  NEED(ref.ops > 20u);
  unsigned failed = 0;
  unsigned done = 0;
  for (std::size_t cut = 0; cut <= ref.ops + 1u; ++cut) {
    INFO("power cut at mutating call " << cut << " of " << ref.ops);
    import_fixture(f, export_arena());
    Rig rig;
    rig.card.cut_at = cut;
    const Start s = rig.start(export_arena().project);
    if (s == Start::Started) {
      NEED(rig.run());
    }
    const OgState st = rig.saves->og_state();
    CHECK((st == OgState::Done || st == OgState::Failed || st == OgState::Idle));
    failed += st == OgState::Failed ? 1u : 0u;
    done += st == OgState::Done ? 1u : 0u;
    check_after(rig.card, expected, "Export/Song OG", st);
    CHECK_FALSE(rig.saves->busy());
    // the next boot: the card as the cut left it, a new service
    Rig next;
    next.card.files = rig.card.files;  // (MemCard is not copyable: the files and folders are the card)
    next.card.dirs = rig.card.dirs;
    const bool folder_exists = rig.card.dirs.count("export/song og") != 0u;
    NEED(next.start(export_arena().project) == Start::Started);
    CHECK(std::string(next.saves->og_folder()) == (folder_exists ? "Export/Song OG 2" : "Export/Song OG"));
    NEED(next.run());
    CHECK(next.saves->og_state() == OgState::Done);
    const std::map<std::string, Bytes>& want = folder_exists ? expected2 : expected;
    for (const auto& kv : want) {
      const auto it = next.card.files.find(kv.first);
      CHECK_MESSAGE((it != next.card.files.end() && it->second == kv.second), kv.first);
    }
    // the first folder is exactly as the cut left it
    for (const auto& kv : rig.card.files) {
      const auto it = next.card.files.find(kv.first);
      CHECK_MESSAGE((it != next.card.files.end() && it->second == kv.second), kv.first);
    }
  }
  CHECK(failed > 10u);
  CHECK(done >= 1u);
}

TEST_CASE("og export T6: one failing call at every mutating call (a hiccup, a refused rename) ends the export with the files before it whole") {
  const Fixture f = make_fixture(3);
  const Reference ref = reference(f);
  const std::map<std::string, Bytes> expected = expected_files(f, "Export/Song OG");
  for (std::size_t at = 0; at < ref.ops; ++at) {
    for (const hal::storage::WriteStatus why : {hal::storage::WriteStatus::Io, hal::storage::WriteStatus::NoSpace}) {
      INFO("call " << at << " of " << ref.ops << " fails with " << static_cast<int>(why));
      import_fixture(f, export_arena());
      Rig rig;
      rig.card.fail_at = at;
      rig.card.fail_with = why;
      NEED(rig.start(export_arena().project) == Start::Started);
      NEED(rig.run());
      const OgState st = rig.saves->og_state();
      CHECK(st == OgState::Failed);
      check_after(rig.card, expected, "Export/Song OG", st);
      CHECK_FALSE(std::string(rig.saves->og_why()).empty());
      if (why == hal::storage::WriteStatus::NoSpace && at != 0u) {
        // (a card full while a file is written says so; a mkdir that fails is an I/O error)
        CHECK(!std::string(rig.saves->og_why()).empty());
      }
    }
  }
}

TEST_CASE("og export T6: the card full after every number of bytes, and a read-back that differs or is short") {
  const Fixture f = make_fixture(3);
  const Reference ref = reference(f);
  const std::map<std::string, Bytes> expected = expected_files(f, "Export/Song OG");
  std::size_t step = ref.bytes / 97u + 1u;
  for (std::size_t room = 0; room <= ref.bytes + 8192u; room += step) {
    INFO("room for " << room << " bytes of " << ref.bytes);
    import_fixture(f, export_arena());
    Rig rig;
    rig.card.space_left = room;
    NEED(rig.start(export_arena().project) == Start::Started);
    NEED(rig.run());
    const OgState st = rig.saves->og_state();
    check_after(rig.card, expected, "Export/Song OG", st);
    if (st == OgState::Failed) {
      CHECK(std::string(rig.saves->og_why()) == "CARD IS FULL");
      CHECK_FALSE(rig.card.has("Export/Song OG/project.mt"));
    }
  }
  for (const bool corrupt : {true, false}) {
    import_fixture(f, export_arena());
    Rig rig;
    rig.card.corrupt_readback = corrupt;
    rig.card.short_readback = !corrupt;
    NEED(rig.start(export_arena().project) == Start::Started);
    NEED(rig.run());
    CHECK(rig.saves->og_state() == OgState::Failed);
    CHECK(std::string(rig.saves->og_why()) == "VERIFY FAILED");
    check_after(rig.card, expected, "Export/Song OG", OgState::Failed);
    CHECK(rig.saves->og_files_done() == 0u);  // (the first file already failed: no final file at all)
  }
}

TEST_CASE("og export T6: a cancel at every pass leaves the files finished, no partial file, no project.mt, and the next export a new folder") {
  const Fixture f = make_fixture(3);
  const Reference ref = reference(f);
  const std::map<std::string, Bytes> expected = expected_files(f, "Export/Song OG");
  unsigned cancelled = 0;
  for (unsigned at = 0; at <= ref.frames + 2u; ++at) {
    INFO("cancel at pass " << at);
    import_fixture(f, export_arena());
    Rig rig;
    NEED(rig.start(export_arena().project) == Start::Started);
    NEED(rig.run([&](unsigned i) {
      if (i == at) {
        rig.saves->cancel_og_export(rig.jobs);
      }
    }));
    const OgState st = rig.saves->og_state();
    CHECK((st == OgState::Cancelled || st == OgState::Done));
    cancelled += st == OgState::Cancelled ? 1u : 0u;
    check_after(rig.card, expected, "Export/Song OG", st);
    CHECK_FALSE(rig.saves->busy());
    // cancel is idempotent and harmless after the end
    rig.saves->cancel_og_export(rig.jobs);
    CHECK(rig.saves->og_state() == st);
  }
  CHECK(cancelled >= ref.frames - 2u);
}

// ---- T6 over FatFs: the real stack, the cut at every block, the two-entry rename -------------------------------------------------------------------------------------------------------------

namespace {

namespace hs = o3os::hal::storage;
namespace st = o3os::platform::mk66::storage;
namespace sv = o3os::platform::mk66::save;
using fakes::Fat32Builder;
using fakes::Medium;

Bytes fat_content(std::size_t n, unsigned salt) {
  Bytes b(n);
  for (std::size_t i = 0; i < n; ++i) {
    b[i] = static_cast<std::uint8_t>((i * 7u + salt * 13u + 1u) & 0xFFu);
  }
  return b;
}

void build_card(Medium& medium) {
  Fat32Builder fat(medium);
  fat.add_file(Fat32Builder::kRootCluster, "README.TXT", fat_content(40, 1));
  const std::uint32_t projects = fat.make_dir(Fat32Builder::kRootCluster, "Projects");
  const std::uint32_t song = fat.make_dir(projects, "Song");
  fat.add_file(song, "project.mt", fat_content(2320, 10));
  const std::uint32_t patterns = fat.make_dir(song, "patterns");
  fat.add_file(patterns, "pattern_01.mtp", fat_content(1000, 11));
  fat.make_dir(Fat32Builder::kRootCluster, "Export");
}

Bytes read_all(hs::Storage& s, const std::string& path) {
  Bytes out;
  hs::FileHandle f;
  if (s.open(path.c_str(), f) != hs::Status::Ok) {
    return out;
  }
  std::uint8_t buf[512];
  std::size_t got = 0;
  while (s.read(f, buf, sizeof buf, got) == hs::Status::Ok && got != 0u) {
    out.insert(out.end(), buf, buf + got);
  }
  s.close(f);
  return out;
}

bool present(hs::Storage& s, const std::string& path) { return s.exists(path.c_str(), nullptr) == hs::Status::Ok; }

struct FatStack {
  st::FatStorage fat;
  sv::FatStorageWriter writer;
  std::unique_ptr<app::SaveService> saves = std::make_unique<app::SaveService>();
  core::JobRunner jobs;
  platform::host::FakeClock clock;
  model::Settings settings;
  std::uint64_t now_ms = 0;
  explicit FatStack(Medium& medium) : fat(medium), writer(fat, medium) {
    settings.init(core::ParamKind::Device);
    saves->attach(fat, writer);
  }
  bool mount() { return fat.mount() == hs::Status::Ok; }
  void frame() {
    (void)jobs.run(clock, 2000);
    clock.advance(2000);
    now_ms += 16;
    saves->poll(jobs, settings, now_ms);
    for (unsigned slot = 0; slot < core::JobRunner::kSlots; ++slot) {
      const core::SlotState state = jobs.state(slot);
      if (state != core::SlotState::Free && state != core::SlotState::Running) {
        jobs.release(slot);
      }
    }
  }
  bool run(unsigned max = 100000) {
    for (unsigned i = 0; i < max; ++i) {
      frame();
      if (!saves->busy() && jobs.occupied() == 0u) {
        return true;
      }
    }
    return false;
  }
};

bool user_files_intact(hs::Storage& s) {
  return read_all(s, "README.TXT") == fat_content(40, 1) && read_all(s, "Projects/Song/project.mt") == fat_content(2320, 10) &&
         read_all(s, "Projects/Song/patterns/pattern_01.mtp") == fat_content(1000, 11);
}

}  // namespace

TEST_CASE("og export T6 over FatFs: a power cut at every block of the whole export (the renames' two directory entries included, project.mt too) leaves whole files, a mountable card and a working next export") {
  // (seven patterns: with the temp's entry at the start of the directory and the final names' entries filling the sectors after it, the temp's entry and the final name's land in different 512-byte sectors,
  // and a cut between the two directory writes of a rename leaves both names: the case the counters below hold the sweep to)
  const Fixture f = make_fixture(7);
  import_fixture(f, export_arena());
  const std::map<std::string, Bytes> want1 = expected_files(f, "Export/Song OG");
  const std::map<std::string, Bytes> want2 = expected_files(f, "Export/Song OG 2");
  const auto path_of = [](const std::string& key) {
    if (key.find("patternsmetadata") != std::string::npos) {
      return std::string("patterns/patternsMetadata");
    }
    if (key.find("project.mt") != std::string::npos) {
      return std::string("project.mt");
    }
    return std::string("patterns/") + key.substr(key.rfind('/') + 1);
  };
  // the fault-free export: how many blocks it writes
  std::uint64_t blocks = 0;
  {
    Medium medium{70'000};
    build_card(medium);
    FatStack stack(medium);
    NEED(stack.mount());
    const std::uint64_t before = medium.blocks_written;
    NEED(stack.saves->start_og_export(stack.jobs, export_arena().project, "Projects/Song", nullptr) == Start::Started);
    NEED(stack.run());
    CHECK(stack.saves->og_state() == OgState::Done);
    blocks = medium.blocks_written - before;
    for (const auto& kv : want1) {
      const std::string rel = path_of(kv.first);
      CHECK_MESSAGE(read_all(stack.fat, "Export/Song OG/" + rel) == kv.second, rel);
    }
    // the names are kept as written, mixed case included (H16: patternsMetadata), and nothing else is in the folders
    const auto names_in = [&](const char* dir) {
      std::vector<std::string> v;
      hs::DirHandle d;
      if (stack.fat.opendir(dir, d) != hs::Status::Ok) {
        return v;
      }
      hs::DirEntry e;
      while (stack.fat.readdir(d, e) == hs::Status::Ok) {
        v.push_back(e.name);
      }
      stack.fat.closedir(d);
      std::sort(v.begin(), v.end());
      return v;
    };
    CHECK(names_in("Export/Song OG/patterns") ==
          std::vector<std::string>{"pattern_01.mtp", "pattern_02.mtp", "pattern_03.mtp", "pattern_100.mtp", "pattern_12.mtp", "pattern_255.mtp", "pattern_40.mtp", "patternsMetadata"});
    CHECK(names_in("Export/Song OG") == std::vector<std::string>{"patterns", "project.mt"});
    CHECK(user_files_intact(stack.fat));
  }
  NEED(blocks > 100u);
  unsigned refused_check = 0;
  unsigned complete = 0;
  unsigned two_names = 0;          // cuts that left the temp AND a final name holding the same whole file (the rename's two directory entries)
  unsigned two_names_project = 0;  // of them, project.mt's
  for (std::uint64_t cut = 0; cut <= blocks; ++cut) {
    INFO("power cut at block " << cut << " of " << blocks);
    Medium medium{70'000};
    build_card(medium);
    {
      FatStack stack(medium);
      NEED(stack.mount());
      medium.fail_writes_after = static_cast<std::uint32_t>(cut);
      if (stack.saves->start_og_export(stack.jobs, export_arena().project, "Projects/Song", nullptr) == Start::Started) {
        (void)stack.run();
      }
    }
    medium.fail_writes_after = 0xFFFFFFFFu;
    FatStack boot(medium);
    NEED(boot.mount());
    CHECK(user_files_intact(boot.fat));
    // every OG file under its final name is whole; project.mt only with every other file
    const std::string base = "Export/Song OG/";
    const bool project = present(boot.fat, base + "project.mt");
    for (const auto& kv : want1) {
      const std::string rel = path_of(kv.first);
      const Bytes got = read_all(boot.fat, base + rel);
      const bool exists_now = present(boot.fat, base + rel);
      CHECK_MESSAGE((got.empty() ? !exists_now : got == kv.second), rel);
      if (project) {
        CHECK_MESSAGE(exists_now, "project.mt exists but " << rel << " does not");
      }
    }
    complete += project ? 1u : 0u;
    // the two-entry case: the temp is still there beside a final name, and both hold the whole new file (accepted for the data; the next write refuses to remove either)
    for (const char* dir : {"Export/Song OG", "Export/Song OG/patterns"}) {
      const Bytes temp = read_all(boot.fat, std::string(dir) + "/_o3save.tmp");
      for (const auto& kv : want1) {
        const std::string rel = path_of(kv.first);
        const bool here = std::string(dir) == "Export/Song OG" ? rel.find('/') == std::string::npos : rel.rfind("patterns/", 0) == 0;
        if (here && !temp.empty() && present(boot.fat, base + rel) && read_all(boot.fat, base + rel) == temp && temp == kv.second) {
          ++two_names;
          two_names_project += rel == "project.mt" ? 1u : 0u;
        }
      }
    }
    // the next export (a new service on a fresh mount) takes the next folder and is whole, whatever was left; a card that needs a check says so and still lets another folder through
    if (boot.saves->start_og_export(boot.jobs, export_arena().project, "Projects/Song", nullptr) == Start::Started) {
      const bool folder_one_exists = present(boot.fat, "Export/Song OG");
      CHECK(std::string(boot.saves->og_folder()) == (folder_one_exists ? "Export/Song OG 2" : "Export/Song OG"));
      NEED(boot.run());
      if (boot.saves->og_state() == OgState::Done) {
        const std::string root = folder_one_exists ? "Export/Song OG 2/" : "Export/Song OG/";
        for (const auto& kv : want2) {
          INFO((root + path_of(kv.first)));
          CHECK(read_all(boot.fat, root + path_of(kv.first)) == kv.second);
        }
      } else {
        ++refused_check;  // ("card needs a check": cross-linked names in the folder the cut left; the export says why and stops)
      }
    }
    CHECK(user_files_intact(boot.fat));
  }
  CHECK(complete >= 1u);
  CHECK_MESSAGE(two_names >= 1u, "no cut left the temp and the final name side by side: the test does not reach the two-entry case");
  // (project.mt's rename cannot tear on FAT32: the OG folder's directory holds ., .., patterns, instruments, the temp and project.mt, under a dozen of the 16 entry slots of one sector, so its two directory
  // writes are one block write; two_names_project is reported, not required)
  MESSAGE("FatFs cuts: " << blocks + 1u << ", complete exports at a cut: " << complete << ", two names for one file: " << two_names << " (project.mt " << two_names_project << "), next exports that stopped on a card check: " << refused_check);
}

// ---- T12: the snapshot ------------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("og export T12: the document changing between two files (an edit, a MIDI take, a sample or instrument commit, an undo, a delete) ends the export, never a mix of two revisions") {
  const Fixture f = make_fixture(4);
  const Reference ref = reference(f);
  const std::map<std::string, Bytes> expected = expected_files(f, "Export/Song OG");
  struct Change {
    const char* what;
    std::function<void(model::Project&)> apply;
    bool ends;  // does it end the export
  };
  const std::vector<Change> changes = {
      {"a tempo edit", [](model::Project& p) { (void)p.set_tempo_x100(p.tempo_x100() + 100u); }, true},
      {"a MIDI record take into a step",
       [](model::Project& p) {
         model::Step s = p.pattern(1)->steps[0][0];
         s.note = static_cast<std::uint8_t>(s.note + 1u);
         (void)p.set_step(1, 0, 0, s);
       },
       true},
      {"a sample instrument committed (a recorder commit, a pad-recorded sample, a sample edit)",
       [](model::Project&) {
         static og_ns::InstrumentReader pti(true);  // (an instrument file read into the document: the same mutators a commit uses)
         InstrumentSpec spec;
         spec.frames = 10;
         const Bytes file = instrument_bytes(spec);
         formats::ReadParams rp;
         rp.index = 5;
         rp.size_hint = static_cast<std::uint32_t>(file.size());
         CHECK(run_reader(pti, file, export_arena(), rp) == Progress::Done);
       },
       true},
      {"a pattern created", [](model::Project& p) { (void)p.create_pattern(77, 16); }, true},
      {"a pattern deleted", [](model::Project& p) { (void)p.delete_pattern(3); }, true},
      {"a pattern renamed", [](model::Project& p) { (void)p.set_pattern_name(2, "Renamed", 7); }, true},
      {"nothing (an external clock start, a transport change: not the document)", [](model::Project&) {}, false},
  };
  for (const Change& c : changes) {
    unsigned ended = 0;
    unsigned modified = 0;
    for (unsigned at = 0; at <= ref.frames; ++at) {
      INFO(c.what << " at pass " << at);
      import_fixture(f, export_arena());
      model::Project& proj = export_arena().project;
      Rig rig;
      NEED(rig.start(proj) == Start::Started);
      bool applied = false;
      NEED(rig.run([&](unsigned i) {
        if (i == at && !applied) {
          applied = rig.saves->busy();
          if (applied) {
            c.apply(proj);
          }
        }
      }));
      const OgState st = rig.saves->og_state();
      if (!applied || !c.ends) {
        CHECK(st == OgState::Done);
        check_after(rig.card, expected, "Export/Song OG", st);  // one revision all through
        continue;
      }
      // The change landed while a file was being written (that file fails: its own writer saw the document move) or between two files (the plan saw it): either way it is over, with no project.mt, and
      // every file under its final name is a whole file of the revision the export began with.
      CHECK((st == OgState::Modified || st == OgState::Failed || st == OgState::Done));
      check_after(rig.card, expected, "Export/Song OG", st);
      ended += st != OgState::Done ? 1u : 0u;
      modified += st == OgState::Modified ? 1u : 0u;
      if (st == OgState::Done) {
        // (the change came after the last file's source had read the document: project.mt was already down, whole, of the first revision)
        CHECK(rig.card.has("Export/Song OG/project.mt"));
      }
    }
    if (c.ends) {
      INFO(c.what);
      CHECK(ended > ref.frames / 2u);  // (a change before the end of the export almost always ends it)
      CHECK(modified > 0u);            // (and the plan's own check, between two files, is reached)
    }
  }
}

TEST_CASE("og export T12: an undo after a file's last byte and before the next file ends the export with MODIFIED; the finished files stay, whole, of the first revision") {
  const Fixture f = make_fixture(4);
  import_fixture(f, export_arena());
  model::Project& proj = export_arena().project;
  alignas(8) static std::uint8_t ring[64 * 1024];
  model::UndoJournal journal;
  journal.begin(ring, sizeof ring);
  model::Editor editor(proj, journal);
  editor.begin_group(core::action_id("test.tempo"), 0);
  (void)editor.set_tempo_x100(proj.tempo_x100() + 500u);
  editor.end_group();
  std::map<std::string, Bytes> expected = expected_files(f, "Export/Song OG");
  {
    // the files of the revision the export starts from (tempo already edited): project.mt differs from the fixture's, the patterns do not
    Bytes pj;
    og_ns::ProjectWriter w;
    NEED(run_writer(w, proj, pj) == Progress::Done);
    expected["export/song og/project.mt"] = pj;
  }
  Rig rig;
  NEED(rig.start(proj) == Start::Started);
  bool undone = false;
  NEED(rig.run([&](unsigned) {
    // the second file has produced its last byte (it is being read back and renamed): the change comes between two files
    if (!undone && rig.saves->og_files_done() >= 1u && rig.saves->job().past_write()) {
      undone = editor.undo() == model::UndoResult::Ok;
    }
  }));
  CHECK(undone);
  CHECK(rig.saves->og_state() == OgState::Modified);
  CHECK(std::string(rig.saves->og_why()) == "THE PROJECT CHANGED");
  check_after(rig.card, expected, "Export/Song OG", OgState::Modified);
  CHECK_FALSE(rig.card.has("Export/Song OG/project.mt"));
  CHECK(rig.saves->og_files_done() == 2u);  // (the file whose last byte was out is finished, and no file after it)
}

TEST_CASE("og export T12: a change that lands in the pass after the plan started a file, before its writer began, still fails that file (the snapshot is the source's too)") {
  const Fixture f = make_fixture(3);
  import_fixture(f, export_arena());
  model::Project& proj = export_arena().project;
  Rig rig;
  NEED(rig.start(proj) == Start::Started);  // the first job is started, its source has not begun
  CHECK(rig.saves->job().running());
  CHECK(rig.card.files.empty());
  (void)proj.set_tempo_x100(proj.tempo_x100() + 100u);
  NEED(rig.run());
  CHECK(rig.saves->og_state() == OgState::Failed);
  CHECK(std::string(rig.saves->og_why()) == "CHANGED: TRY AGAIN");
  CHECK(rig.saves->og_files_done() == 0u);
  for (const auto& kv : rig.card.files) {
    CHECK_MESSAGE(is_temp(kv.first), kv.first);  // nothing under a final name
  }
  CHECK_FALSE(rig.card.has("Export/Song OG/project.mt"));
}

TEST_CASE("og export T12: a document that changed under a file being written fails that file with CHANGED: TRY AGAIN, and the temp is removed") {
  const Fixture f = make_fixture(3);
  import_fixture(f, export_arena());
  model::Project& proj = export_arena().project;
  Rig rig;
  NEED(rig.start(proj) == Start::Started);
  // the first file is a pattern of 9,260 bytes: written in slices; the change lands between two of them
  bool changed = false;
  NEED(rig.run([&](unsigned) {
    if (!changed && rig.card.files.count("export/song og/patterns/_o3save.tmp") != 0u) {
      (void)proj.set_tempo_x100(proj.tempo_x100() + 100u);
      changed = true;
    }
  }));
  NEED(changed);
  const OgState st = rig.saves->og_state();
  CHECK((st == OgState::Failed || st == OgState::Modified || st == OgState::Done));
  if (st == OgState::Failed) {
    CHECK(std::string(rig.saves->og_why()) == "CHANGED: TRY AGAIN");
  }
  CHECK_FALSE(rig.card.has("Export/Song OG/project.mt"));
  for (const auto& kv : rig.card.files) {
    if (is_temp(kv.first)) {
      CHECK(st != OgState::Failed);  // (a failed file's temp is removed by the job)
    }
  }
}

// ---- T15: the pattern names file ----------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("og export T15: the names file comes back byte for byte while the patterns are as imported, and is the empty file (counted) once one is added, removed or renumbered") {
  const Fixture f = make_fixture(4);
  const Bytes original = pamd_bytes(f.names);
  Bytes empty;
  {
    const std::vector<std::string> none(256, "");
    empty = pamd_bytes(none);
  }
  og_ns::PamdWriter w;
  Bytes out;
  // as imported
  import_fixture(f, export_arena());
  model::Project& proj = export_arena().project;
  CHECK_FALSE(proj.names_stale());
  NEED(run_writer(w, proj, out) == Progress::Done);
  CHECK(out == original);
  CHECK(w.export_report().drops.pattern_names == 0u);
  // a name edited: the file as it was, the name counted (the OS's names are not written)
  NEED(proj.set_pattern_name(2, "Renamed", 7) == model::Status::Ok);
  CHECK_FALSE(proj.names_stale());
  NEED(run_writer(w, proj, out) == Progress::Done);
  CHECK(out == original);
  CHECK(w.export_report().drops.pattern_names == 1u);

  const auto stale_case = [&](const char* what, const std::function<void(model::Project&)>& edit, std::uint32_t named_patterns) {
    INFO(what);
    import_fixture(f, export_arena());
    model::Project& p = export_arena().project;
    edit(p);
    CHECK(p.names_stale());
    NEED(run_writer(w, p, out) == Progress::Done);
    CHECK(out == empty);
    CHECK(out.size() == 12816u);
    CHECK(w.export_report().drops.pattern_names == named_patterns);
    CHECK(w.export_report().drops.any_lost());
    CHECK(w.export_report().drops.settings_ignored == 0u);
  };
  stale_case("a pattern added", [](model::Project& p) { NEED(p.create_pattern(77, 16) == model::Status::Ok); }, 4u);
  stale_case("a pattern removed", [](model::Project& p) { NEED(p.delete_pattern(3) == model::Status::Ok); }, 3u);
  stale_case("a pattern renumbered (a copy at a new id, the old one deleted)",
             [](model::Project& p) {
               NEED(p.create_pattern(5, 16) == model::Status::Ok);
               NEED(p.set_pattern_name(5, "Verse A", 7) == model::Status::Ok);
               NEED(p.delete_pattern(2) == model::Status::Ok);
             },
             4u);  // (One, Three, Chorus and the copy: Verse A moved to 5 and was named there)
  // an undo of such an edit goes through the same mutators: stale too (the safe side)
  stale_case("a pattern added and deleted again", [](model::Project& p) {
    NEED(p.create_pattern(77, 16) == model::Status::Ok);
    NEED(p.delete_pattern(77) == model::Status::Ok);
  }, 4u);
  // a native save and load of an import keeps the file; the mark is not stored, so the loaded project is not stale (and never was)
  import_fixture(f, export_arena());
  std::vector<std::uint8_t> native;
  NEED(test::save(export_arena().project, native) == Progress::Done);
  NEED(test::load(arena(1), native) == Progress::Done);
  CHECK_FALSE(arena(1).project.names_stale());
  NEED(run_writer(w, arena(1).project, out) == Progress::Done);
  CHECK(out == original);
  // a reset clears the mark
  import_fixture(f, export_arena());
  NEED(export_arena().project.delete_pattern(1) == model::Status::Ok);
  CHECK(export_arena().project.names_stale());
  export_arena().project.reset();
  CHECK_FALSE(export_arena().project.names_stale());
}

TEST_CASE("og export T15: the exported names file on the card is the original, or the empty one after an edit of the set of patterns") {
  const Fixture f = make_fixture(3);
  for (const bool edited : {false, true}) {
    import_fixture(f, export_arena());
    model::Project& proj = export_arena().project;
    if (edited) {
      NEED(proj.delete_pattern(12) == model::Status::Ok);
      NEED(proj.create_pattern(13, 16) == model::Status::Ok);
    }
    Rig rig;
    NEED(rig.start(proj) == Start::Started);
    NEED(rig.run());
    const Bytes* names = rig.card.get("Export/Song OG/patterns/patternsMetadata");
    NEED(names != nullptr);
    if (edited) {
      CHECK(*names == pamd_bytes(std::vector<std::string>(256, "")));
      CHECK(rig.saves->og_drops().pattern_names == 2u);  // (the two named patterns that are still there)
      CHECK(rig.saves->og_drops().any_lost());
    } else {
      CHECK(*names == pamd_bytes(f.names));
      CHECK(rig.saves->og_drops().pattern_names == 0u);
    }
  }
}

// ---- C2b: the instruments, with the PCM stage ---------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

// The sample bank over a model arena: the audio of the sample instruments the export reads (the same objects the App holds).
struct Audio {
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::vector<std::int16_t> memory;
  std::vector<std::int16_t> peaks = std::vector<std::int16_t>(20000);
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  model::ModelArena& arena;

  explicit Audio(model::ModelArena& a, std::size_t samples = 200000) : memory(samples), arena(a) {
    bank.init(*voices, a, *mem, memory.data(), static_cast<std::uint32_t>(memory.size()), peaks.data(), peaks.size());
  }
  // A sample instrument in `slot` recorded at `rate` (0: not recorded), with `tweak` applied; its audio is `pcm` (empty: the instrument has none).
  void add(unsigned slot, const char* name, const std::vector<std::int16_t>& pcm, std::uint32_t rate, const std::function<void(model::Instrument&)>& tweak = {}) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::strncpy(in.name, name, sizeof in.name - 1);
    std::strcpy(in.sample_path, "samples/instr00.wav");
    in.sample_path[13] = static_cast<char>('0' + (slot + 1) / 10);
    in.sample_path[14] = static_cast<char>('0' + (slot + 1) % 10);
    in.sample_rate = rate;
    if (tweak) {
      tweak(in);
    }
    NEED(arena.project.set_instrument(slot, in) == model::Status::Ok);
    if (pcm.empty()) {
      return;
    }
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED(p != nullptr);
    NEED(cap >= pcm.size());
    std::copy(pcm.begin(), pcm.end(), p);
    NEED(bank.commit_load(slot, p, static_cast<std::uint32_t>(pcm.size())));
    bank.rebind_now(slot);
  }
};

std::vector<std::int16_t> saw(std::uint32_t frames, std::uint32_t seed) {
  std::vector<std::int16_t> v(frames);
  for (std::uint32_t i = 0; i < frames; ++i) {
    v[i] = static_cast<std::int16_t>(((i * 37u + seed * 101u) % 2000u) * 10u - 10000);
  }
  return v;
}

// A sine of `hz` over `frames` samples recorded at `rate`.
std::vector<std::int16_t> tone(std::uint32_t frames, double hz, double rate) {
  std::vector<std::int16_t> v(frames);
  for (std::uint32_t i = 0; i < frames; ++i) {
    v[i] = static_cast<std::int16_t>(8000.0 * std::sin(6.283185307179586 * hz * i / rate));
  }
  return v;
}

// Rising zero crossings: a tone of f Hz over n frames at 44,100 Hz has f * n / 44,100 of them (+-1).
unsigned crossings(const std::int16_t* p, std::uint32_t n) {
  unsigned c = 0;
  for (std::uint32_t i = 1; i < n; ++i) {
    c += p[i - 1] < 0 && p[i] >= 0 ? 1u : 0u;
  }
  return c;
}

class VecSource final : public core::ByteSource {
 public:
  explicit VecSource(const std::vector<std::int16_t>& pcm) : bytes_(reinterpret_cast<const std::uint8_t*>(pcm.data())), size_(pcm.size() * 2u) {}
  core::IoResult read(void* dst, std::size_t max) noexcept override {
    if (pos_ >= size_) {
      return {0, core::IoStatus::End};
    }
    const std::size_t n = std::min(size_ - pos_, max);
    std::memcpy(dst, bytes_ + pos_, n);
    pos_ += n;
    return {n, core::IoStatus::Ok};
  }

 private:
  const std::uint8_t* bytes_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

// The .pti the OG instrument writer makes for `slot` of `p` over `pcm` as it is (the standalone save of C1b writes the same bytes: it is the same writer).
Bytes reference_pti(const model::Project& p, unsigned slot, const std::vector<std::int16_t>& pcm) {
  VecSource src(pcm);
  formats::WriteParams wp;
  wp.index = static_cast<std::uint16_t>(slot);
  wp.pcm = &src;
  wp.pcm_frames = static_cast<std::uint32_t>(pcm.size());
  wp.standalone = true;
  og_ns::InstrumentWriter w;
  Bytes out;
  CHECK(run_writer(w, p, out, wp) == Progress::Done);
  return out;
}

std::string pti_path(const model::Project& p, unsigned slot, const char* folder = "Export/Song OG") {
  char stem[40] = {};
  CHECK(app::og_pti_stem(slot, p.instrument(slot)->name, stem, sizeof stem) != 0u);
  return lower(std::string(folder) + "/instruments/" + stem + ".pti");
}

std::vector<std::int16_t> pcm_of(const Bytes& file) {
  std::vector<std::int16_t> v((file.size() - og_ns::kInstrumentBytes) / 2u);
  std::memcpy(v.data(), file.data() + og_ns::kInstrumentBytes, v.size() * 2u);
  return v;
}

// The OS's own loader over a folder of the card: the project, then the samples of its .pti files into `store` and the voice table of `pool`.
struct Loaded {
  static app::OgFolderLoader& folder_loader() {
    static app::OgFolderLoader l;
    return l;
  }
  static app::OgSampleLoader& sample_loader() {
    static app::OgSampleLoader l;
    return l;
  }
  std::vector<std::int16_t> store = std::vector<std::int16_t>(300000, std::int16_t{0});
  bool ok = false;

  Loaded(MemCard& card, const char* folder, model::ModelArena& into, audio::VoicePool& pool) {
    app::OgFolderLoader& pl = folder_loader();
    if (!pl.begin(card, folder, into)) {
      return;
    }
    app::OgFolderLoader::State st = app::OgFolderLoader::State::Running;
    for (unsigned i = 0; i < 100000 && st == app::OgFolderLoader::State::Running; ++i) {
      st = pl.step(4096);
    }
    if (st != app::OgFolderLoader::State::Done) {
      return;
    }
    app::OgSampleLoader& sl = sample_loader();
    if (!sl.begin(card, pl.folder(), into.project, pl.pti_slots(), pool, store.data(), static_cast<std::uint32_t>(store.size()), app::instrument_law(pl.report().version))) {
      return;
    }
    for (unsigned i = 0; i < 1000000 && sl.state() == app::OgSampleLoader::State::Running; ++i) {
      (void)sl.step(4096);
    }
    ok = sl.state() == app::OgSampleLoader::State::Done;
  }
};

std::unique_ptr<audio::VoicePool>& loaded_pool() {
  static std::unique_ptr<audio::VoicePool> pool = std::make_unique<audio::VoicePool>();
  return pool;
}

}  // namespace

TEST_CASE("og export T1 whole, T8: the instruments are written first, resampled where they play at another rate, the folder is read back by the OS's own loader (instruments, samples, pitch, gain) and a second export of the result is the same files") {
  const Fixture f = make_fixture(4);
  import_fixture(f, export_arena());
  Audio au(export_arena());
  const std::vector<std::int16_t> kick = saw(3000, 1);
  const std::vector<std::int16_t> snare = tone(22050, 1000.0, 22050.0);  // one second of 1 kHz recorded at 22,050 Hz
  const std::vector<std::int16_t> last = saw(1000, 2);
  au.add(0, "Kick", kick, 44100);
  au.add(2, "Snare", snare, 22050);
  au.add(47, "Last/Name: x", last, 0);
  Rig rig;
  NEED(rig.start(export_arena().project, "Projects/Song", &au.bank) == Start::Started);
  CHECK(rig.saves->og_files_total() == 3u + 4u + 2u);
  CHECK(rig.saves->og_drops().resampled == 1u);
  CHECK_FALSE(rig.saves->og_drops().any_lost());
  NEED(rig.run());
  CHECK(rig.saves->og_state() == OgState::Done);
  CHECK(rig.saves->og_files_done() == 9u);
  CHECK(std::string(rig.saves->og_levels()) == "LEVELS WRITTEN AS STORED");
  CHECK(rig.saves->og_clipped() == 0u);
  CHECK_FALSE(rig.saves->og_drops().any_lost());

  // the files: the patterns, the names file and project.mt as without instruments, the three .pti under their names
  std::map<std::string, Bytes> expected = expected_files(f, "Export/Song OG");
  const model::Project& proj = export_arena().project;
  expected[pti_path(proj, 0)] = reference_pti(proj, 0, kick);
  expected[pti_path(proj, 47)] = reference_pti(proj, 47, last);
  const std::string snare_path = pti_path(proj, 2);
  NEED(rig.card.files.count(snare_path) != 0u);
  expected[snare_path] = rig.card.files[snare_path];
  CHECK(pti_path(proj, 0) == "export/song og/instruments/01 kick.pti");
  CHECK(pti_path(proj, 2) == "export/song og/instruments/03 snare.pti");
  CHECK(pti_path(proj, 47) == "export/song og/instruments/48 last_name_ x.pti");
  for (const auto& kv : expected) {
    const auto it = rig.card.files.find(kv.first);
    INFO(kv.first);
    NEED(it != rig.card.files.end());
    CHECK_MESSAGE(it->second == kv.second, kv.first);
  }
  check_after(rig.card, expected, "Export/Song OG", OgState::Done);
  CHECK(rig.card.files.size() == expected.size());
  // the snare: 1 s at 44,100 Hz, its 1 kHz tone kept (T2/T8: the pitch)
  const std::vector<std::int16_t> snare_out = pcm_of(rig.card.files[snare_path]);
  CHECK(snare_out.size() == 44100u);
  CHECK(std::abs(static_cast<int>(crossings(snare_out.data(), 44100)) - 1000) <= 1);
  // the order: the instruments by slot, the patterns by id, the names file, project.mt last
  std::vector<std::string> finals;
  for (const std::string& l : rig.card.log) {
    const std::size_t arrow = l.find('>');
    if (l.rfind("rename ", 0) == 0 && arrow != std::string::npos) {
      finals.push_back(lower(l.substr(arrow + 1)));
    }
  }
  NEED(finals.size() == 9u);
  CHECK(finals[0] == pti_path(proj, 0));
  CHECK(finals[1] == pti_path(proj, 2));
  CHECK(finals[2] == pti_path(proj, 47));
  CHECK(finals[3] == "export/song og/patterns/pattern_01.mtp");
  CHECK(finals[7] == "export/song og/patterns/patternsmetadata");
  CHECK(finals[8] == "export/song og/project.mt");

  // T8: the OS's own loader reads the folder: instruments, their audio, the points and the gain
  model::ModelArena& back = arena(1);
  audio::VoicePool& pool = *loaded_pool();
  Loaded loaded(rig.card, "Export/Song OG", back, pool);
  NEED(loaded.ok);
  CHECK(Loaded::folder_loader().report().instruments == 3u);
  CHECK(Loaded::folder_loader().report().patterns == 4u);
  CHECK(Loaded::sample_loader().report().loaded == 3u);
  CHECK(Loaded::sample_loader().report().missing == 0u);
  const unsigned slots[] = {0, 2, 47};
  for (unsigned slot : slots) {
    const model::Instrument* a = export_arena().project.instrument(slot);
    const model::Instrument* b = back.project.instrument(slot);
    NEED(a != nullptr);
    NEED(b != nullptr);
    CHECK(std::string(a->name) == std::string(b->name));
    CHECK(a->kind == b->kind);
    CHECK(std::memcmp(&a->sampler, &b->sampler, sizeof a->sampler) == 0);  // the 1:1 fields of the OG instrument, all of them
    const audio::SampleDesc& d = pool.sample(slot);
    const audio::SampleDesc& s = au.voices->sample(slot);
    CHECK(d.gain == s.gain);  // the same law, the same level: the same gain
    CHECK(d.tune == s.tune);
    CHECK(d.fine_tune == s.fine_tune);
  }
  CHECK(pool.sample(0).length == 3000u);
  CHECK(std::memcmp(pool.sample(0).data, kick.data(), 3000u * 2u) == 0);
  CHECK(pool.sample(47).length == 1000u);
  CHECK(std::memcmp(pool.sample(47).data, last.data(), 1000u * 2u) == 0);
  CHECK(pool.sample(2).length == 44100u);
  CHECK(std::abs(static_cast<int>(crossings(pool.sample(2).data, 44100)) - 1000) <= 1);
  CHECK(back.project.pattern_count() == 4u);

  // a second export of what came back (its audio put in a bank) is the same files: nothing is resampled twice, nothing is added
  static og_ns::PamdReader pamd;
  NEED(run_reader(pamd, *rig.card.get("Export/Song OG/patterns/patternsMetadata"), back) == Progress::Done);
  Audio au2(back);
  for (unsigned slot : slots) {
    const audio::SampleDesc& d = pool.sample(slot);
    std::uint32_t cap = 0;
    std::int16_t* p = au2.bank.reserve(cap);
    NEED(p != nullptr);
    std::memcpy(p, d.data, d.length * 2u);
    NEED(au2.bank.commit_load(slot, p, d.length));
  }
  Rig again;
  NEED(again.start(back.project, "Projects/Song", &au2.bank) == Start::Started);
  NEED(again.run());
  CHECK(again.saves->og_state() == OgState::Done);
  CHECK(again.saves->og_drops().resampled == 0u);
  for (const auto& kv : expected) {
    const auto it = again.card.files.find(kv.first);
    INFO(kv.first);
    NEED(it != again.card.files.end());
    CHECK_MESSAGE(it->second == kv.second, kv.first);
  }
}

TEST_CASE("og export T8: a project of the 1.1.14 level law is written with the levels converted so that the gain stays, clamped under -24 dB, and the result line says so") {
  const Fixture f = make_fixture(1);
  import_fixture(f, export_arena());
  export_arena().project.set_volume_law(1);
  Audio au(export_arena());
  au.bank.set_law(audio::LevelLaw::Og1114);
  const auto vol = [](std::uint8_t v) { return [v](model::Instrument& in) { in.sampler.volume = v; }; };
  au.add(0, "Full", saw(500, 1), 44100, vol(100));
  au.add(1, "Half", saw(500, 2), 44100, vol(70));
  au.add(2, "Silent", saw(500, 3), 44100, vol(0));
  au.add(3, "Quiet", saw(500, 4), 44100, vol(30));  // -48 dB: nothing that quiet in the 1.2+ law
  Rig rig;
  NEED(rig.start(export_arena().project, "Projects/Song", &au.bank) == Start::Started);
  const og_ns::ExportDrops& d = rig.saves->og_drops();
  CHECK(d.levels_converted == 2u);  // 100 and 70 (0 stays 0)
  CHECK(d.levels_clamped == 1u);
  CHECK(d.any_lost());
  NEED(rig.run());
  CHECK(rig.saves->og_state() == OgState::Done);
  CHECK(std::string(rig.saves->og_levels()) == "LEVELS WRITTEN AS PLAYED (1.1.14 -> 1.2+)");
  // the files hold the 1.2+ levels: 100 -> 50 (0 dB), 70 -> 50 + round(0.4 * -30 / 0.48) = 25, 0 -> 0, 30 -> 1
  const unsigned want[] = {50, 25, 0, 1};
  for (unsigned slot = 0; slot < 4; ++slot) {
    const std::string path = pti_path(export_arena().project, slot);
    NEED(rig.card.files.count(path) != 0u);
    const Bytes& file = rig.card.files[path];
    CHECK_MESSAGE(file[272] == want[slot], path);  // (the volume byte of the instrument structure: og.md section 5, the offset the fixture tests use)
  }
  // and the OS's own loader (the file is version 9: the 1.2+ law) plays the converted levels where the 1.1.14 table played the originals, within 0.5 dB
  audio::VoicePool& pool = *loaded_pool();
  Loaded loaded(rig.card, "Export/Song OG", arena(1), pool);
  NEED(loaded.ok);
  for (unsigned slot = 0; slot < 2; ++slot) {
    const double a = static_cast<double>(pool.sample(slot).gain);
    const double b = static_cast<double>(au.voices->sample(slot).gain);
    INFO("slot " << slot);
    CHECK(std::abs(20.0 * std::log10(a / b)) <= 0.5);
  }
  CHECK(pool.sample(2).gain == 0);
}

TEST_CASE("og export T4: one counter for every lossy row, exactly those, and the project files still byte for byte what the writers make") {
  const Fixture f = make_fixture(2);
  import_fixture(f, export_arena());
  model::Project& proj = export_arena().project;
  og_ns::ExportDrops base;  // what the fixture's own patterns count (their volume columns, tracks 9-12): not losses, but counted
  {
    Rig plain;
    NEED(plain.start(proj, "Projects/Song", nullptr) == Start::Started);
    NEED(plain.run());
    base = plain.saves->og_drops();
  }
  // the project-level rows: a module, core, tracker-ce and engine FX, tracks 13-16, pan / solo / bus, swing, a moved MIDI instrument, a MIDI instrument with an INST 1.3 field, a sample beyond 48
  NEED(proj.add_module("tb303", 5, 1, 0) == model::Status::Ok);
  model::Instrument mod;
  model::init_instrument(mod, model::InstrumentKind::Module);
  std::strcpy(mod.name, "ACID");
  mod.module_index = 0;
  NEED(proj.set_instrument(5, mod) == model::Status::Ok);
  model::Instrument midi;
  model::init_instrument(midi, model::InstrumentKind::Midi);
  midi.midi_channel = 3;
  NEED(proj.set_instrument(10, midi) == model::Status::Ok);  // not the slot of channel 3
  midi.midi_channel = 1;
  midi.midi_program = 5;
  NEED(proj.set_instrument(48, midi) == model::Status::Ok);  // channel 1 in its own slot, with a program: an OG pattern's instrument byte holds the channel only
  model::Instrument beyond;
  model::init_instrument(beyond, model::InstrumentKind::Sample);
  NEED(proj.set_instrument(50, beyond) == model::Status::Ok);
  model::Step s = model::kEmptyStep;
  s.note = 60;
  s.instr = 1;
  s.fx[0] = {50, 1};   // a core FX
  s.fx[1] = {200, 1};  // an engine FX
  NEED(proj.set_step(1, 0, 3, s) == model::Status::Ok);
  s.fx[0] = {96, 1};  // a tracker-ce FX (OG id 43 + 53, ADR 0004 amendment 3): dropped as ce_fx, not core_fx
  s.fx[1] = {127, 2};
  NEED(proj.set_step(1, 0, 4, s) == model::Status::Ok);
  model::Step far = model::kEmptyStep;
  far.note = 61;
  far.instr = 1;
  NEED(proj.set_step(1, 13, 4, far) == model::Status::Ok);  // track 14
  NEED(proj.set_swing(60) == model::Status::Ok);
  model::TrackSlot t{};
  t.volume = 100;
  t.pan = 20;
  NEED(proj.set_track(0, t) == model::Status::Ok);
  t.pan = 50;
  t.flags = model::kTrackSolo;
  NEED(proj.set_track(1, t) == model::Status::Ok);
  t.flags = 0;
  t.bus = 3;
  NEED(proj.set_track(2, t) == model::Status::Ok);
  NEED(proj.unknown_begin(0x5A5A5A5Au, 4) == model::Status::Ok);
  NEED(proj.unknown_append(reinterpret_cast<const std::uint8_t*>("abcd"), 4) == model::Status::Ok);
  Rig rig;
  app::OgSurvey survey;  // OS-FILES-C C2c: the question before the export counts what the export will end with, the writers' counters included
  NEED(rig.saves->og_survey(proj, nullptr, "Projects/Song", survey) == Start::Started);
  NEED(rig.start(proj, "Projects/Song", nullptr) == Start::Started);
  NEED(rig.run());
  CHECK(rig.saves->og_state() == OgState::Done);
  CHECK(app::og_loss_signature(survey.drops) == app::og_loss_signature(rig.saves->og_drops()));
  CHECK(survey.drops.module_instruments == 1u);
  CHECK(survey.drops.tracks_beyond_12 == 1u);
  og_ns::ExportDrops want = base;
  want.module_instruments = 1;
  want.core_fx = 1;
  want.ce_fx = 2;
  want.engine_fx = 1;
  want.tracks_beyond_12 = 1;
  want.track_settings = 3;
  want.project_settings = 1;  // swing
  want.midi_instruments = 1;
  want.midi_settings = 1;
  want.sample_slots_beyond_48 = 1;
  want.unknown_chunks = 1;
  const og_ns::ExportDrops& got = rig.saves->og_drops();
  const auto row = [](const og_ns::ExportDrops& d) {
    return std::vector<std::uint32_t>{d.tracks_beyond_12, d.engine_fx,        d.core_fx,       d.ce_fx,         d.volume_as_fx18,        d.volume_dropped, d.track_settings,    d.audio_on_midi_tracks,
                                      d.track_names_cut,  d.project_settings, d.module_instruments, d.midi_instruments, d.sample_slots_beyond_48, d.unknown_chunks, d.instrument_names_cut,
                                      d.pattern_names,    d.settings_ignored, d.midi_settings, d.resampled,             d.rate_ignored,   d.amiga_engine,      d.bit_engine,
                                      d.pcm_too_long,     d.instruments_not_written, d.levels_converted, d.levels_clamped, d.lane_events};
  };
  const auto text = [&](const og_ns::ExportDrops& d) {
    std::string out;
    for (const std::uint32_t v : row(d)) {
      out += std::to_string(v) + " ";
    }
    return out;
  };
  CHECK_MESSAGE(row(got) == row(want), "got [" << text(got) << "] want [" << text(want) << "] in the order of ExportDrops");
  CHECK(got.any_lost());
  CHECK(std::string(rig.saves->og_levels()) == "LEVELS WRITTEN AS STORED");
}

TEST_CASE("og export T4: the PCM stage counts what it resamples, what it cannot say and what it does not write") {
  const Fixture f = make_fixture(1);
  import_fixture(f, export_arena());
  export_arena().project.set_volume_law(1);
  Audio au(export_arena(), 1'300'000);
  au.bank.set_law(audio::LevelLaw::Og1114);
  const auto mode = [](std::uint8_t m) { return [m](model::Instrument& in) { in.sampler.play_mode = m; }; };
  const auto engine = [](std::uint8_t e, std::uint8_t m = 0) {
    return [e, m](model::Instrument& in) {
      in.engine = e;
      in.sampler.play_mode = m;
    };
  };
  au.add(0, "Modern22", saw(400, 1), 22050);                                                       // resampled; level converted
  au.add(1, "Wave", saw(4096, 2), 22050, mode(6));                                                 // wavetable: the rate is dropped, the frames kept
  au.add(2, "Amiga", saw(400, 3), 0, engine(model::kInstEngineAmiga));                             // Paula's rate: resampled, the engine gone
  au.add(3, "AmigaBack", saw(400, 4), 0, engine(model::kInstEngineAmiga, 2));                      // a mode Paula does not play: not written
  au.add(4, "Bits", saw(400, 5), 44100, engine(model::kInstEngineBits12));                         // 12 BIT: a .pti cannot say it: not written
  au.add(5, "Huge", saw(1'110'000, 6), 0, engine(model::kInstEngineAmiga));                        // x5.32: more than a .pti holds: not written
  au.add(6, "NoAudio", {}, 44100);                                                                 // no audio: no file
  au.add(7, "Quiet", saw(400, 7), 44100, [](model::Instrument& in) { in.sampler.volume = 30; });   // -48 dB: clamped
  Rig rig;
  app::OgSurvey survey;
  NEED(rig.saves->og_survey(export_arena().project, &au.bank, "Projects/Song", survey) == Start::Started);
  NEED(rig.start(export_arena().project, "Projects/Song", &au.bank) == Start::Started);
  const og_ns::ExportDrops& d = rig.saves->og_drops();
  CHECK(d.resampled == 2u);
  CHECK(d.rate_ignored == 1u);
  CHECK(d.amiga_engine == 3u);  // 2, 3 and 5
  CHECK(d.bit_engine == 1u);
  CHECK(d.pcm_too_long == 1u);
  CHECK(d.instruments_not_written == 4u);  // 3, 4, 5, 6
  CHECK(d.levels_converted == 3u);         // 0, 1, 2 (volume 100 -> 50)
  CHECK(d.levels_clamped == 1u);           // 7
  CHECK(d.any_lost());
  CHECK(rig.saves->og_files_total() == 4u + 1u + 2u);  // 0, 1, 2, 7
  NEED(rig.run());
  CHECK(rig.saves->og_state() == OgState::Done);
  const auto has = [&](unsigned slot) { return rig.card.files.count(pti_path(export_arena().project, slot)) != 0u; };
  CHECK(has(0));
  CHECK(has(1));
  CHECK(has(2));
  CHECK_FALSE(has(3));
  CHECK_FALSE(has(4));
  CHECK_FALSE(has(5));
  CHECK_FALSE(has(6));
  CHECK(has(7));
  // lengths: 400 frames at 22,050 Hz -> 800; the wavetable as it is; 400 frames at 8,287 Hz -> 2,129 (ceil)
  const auto frames_of = [&](unsigned slot) { return (rig.card.files[pti_path(export_arena().project, slot)].size() - og_ns::kInstrumentBytes) / 2u; };
  CHECK(frames_of(0) == 800u);
  CHECK(frames_of(1) == 4096u);
  CHECK(frames_of(2) == (400u * 44100u + 8286u) / 8287u);
  CHECK(frames_of(7) == 400u);
  CHECK(std::string(rig.saves->og_levels()) == "LEVELS WRITTEN AS PLAYED (1.1.14 -> 1.2+)");
  CHECK(app::og_loss_signature(survey.drops) == app::og_loss_signature(rig.saves->og_drops()));  // C2c: the survey counted what the export ended with
}

TEST_CASE("og export T5: 48 sample instruments, rates 1,000 and 384,000, a one-frame sample and one pattern are a whole export the loader reads back; 128 patterns are 130 files") {
  {
    const Fixture f = make_fixture(1);
    import_fixture(f, export_arena());
    Audio au(export_arena());
    for (unsigned slot = 0; slot < 48; ++slot) {
      const std::string name = slot == 47 ? std::string("A very long name of an instrument, 47") : "Inst " + std::to_string(slot);
      const std::uint32_t rate = slot == 1 ? 1000u : slot == 2 ? 384000u : 44100u;
      au.add(slot, name.c_str(), slot == 3 ? saw(1, 3) : saw(64 + slot, slot), rate);
    }
    Rig rig;
    NEED(rig.start(export_arena().project, "Projects/Song", &au.bank) == Start::Started);
    CHECK(rig.saves->og_files_total() == 48u + 1u + 2u);
    NEED(rig.run());
    CHECK(rig.saves->og_state() == OgState::Done);
    CHECK(rig.saves->og_drops().resampled == 2u);
    unsigned ptis = 0;
    for (const auto& kv : rig.card.files) {
      ptis += kv.first.size() > 4 && kv.first.compare(kv.first.size() - 4, 4, ".pti") == 0 ? 1u : 0u;
    }
    CHECK(ptis == 48u);
    CHECK(rig.card.violations == 0u);
    const auto frames_of = [&](unsigned slot) { return (rig.card.files[pti_path(export_arena().project, slot)].size() - og_ns::kInstrumentBytes) / 2u; };
    CHECK(frames_of(1) == (65u * 44100u + 999u) / 1000u);  // x44.1
    CHECK(frames_of(2) == (66u * 44100u + 383999u) / 384000u);
    CHECK(frames_of(3) == 1u);
    Loaded loaded(rig.card, "Export/Song OG", arena(1), *loaded_pool());
    NEED(loaded.ok);
    CHECK(Loaded::folder_loader().report().instruments == 48u);
    CHECK(Loaded::sample_loader().report().loaded >= 47u);  // (a one-frame sample may have no window to play)
  }
  {
    model::ModelArena& a = export_arena();
    a.project.reset();
    for (unsigned id = 1; id <= 128; ++id) {
      NEED(a.project.create_pattern(id, 16) == model::Status::Ok);
    }
    Rig rig;
    NEED(rig.start(a.project, "Projects/Song", nullptr) == Start::Started);
    CHECK(rig.saves->og_files_total() == 130u);
    NEED(rig.run());
    CHECK(rig.saves->og_state() == OgState::Done);
    CHECK(rig.card.files.size() == 130u);
    CHECK(rig.card.violations == 0u);
  }
}

TEST_CASE("og export T6 (C2b): a power cut or a failing call at every mutating call of a set with instruments leaves whole files only, the instruments before the patterns, project.mt last") {
  const Fixture f = make_fixture(2);
  import_fixture(f, export_arena());
  Audio au(export_arena());
  au.add(0, "Kick", saw(900, 1), 44100);
  au.add(1, "Pad", tone(2205, 441.0, 22050.0), 22050);
  std::map<std::string, Bytes> expected;
  std::size_t ops = 0;
  {
    Rig ref;
    NEED(ref.start(export_arena().project, "Projects/Song", &au.bank) == Start::Started);
    NEED(ref.run());
    NEED(ref.saves->og_state() == OgState::Done);
    expected = ref.card.files;
    ops = ref.card.ops;
  }
  NEED(expected.size() == 2u + 2u + 2u);
  NEED(ops > 20u);
  unsigned failed = 0;
  unsigned done = 0;
  for (std::size_t cut = 0; cut <= ops + 1u; ++cut) {
    for (int hiccup = 0; hiccup < 2; ++hiccup) {
      INFO("cut at mutating call " << cut << " of " << ops << (hiccup != 0 ? " (a failing call)" : " (a power cut)"));
      Rig rig;
      if (hiccup != 0) {
        rig.card.fail_at = cut;
      } else {
        rig.card.cut_at = cut;
      }
      const Start s = rig.start(export_arena().project, "Projects/Song", &au.bank);
      if (s == Start::Started) {
        NEED(rig.run());
      }
      const OgState st = rig.saves->og_state();
      failed += st == OgState::Failed ? 1u : 0u;
      done += st == OgState::Done ? 1u : 0u;
      check_after(rig.card, expected, "Export/Song OG", st);
      // an instrument is never missing when a pattern is there: the order is the plan's
      if (rig.card.files.count("export/song og/patterns/pattern_01.mtp") != 0u) {
        CHECK(rig.card.files.count(pti_path(export_arena().project, 0)) != 0u);
        CHECK(rig.card.files.count(pti_path(export_arena().project, 1)) != 0u);
      }
    }
  }
  CHECK(failed > 10u);
  CHECK(done >= 1u);
}

TEST_CASE("og export T6 over FatFs (C2b): a cut at every block of a set with instruments of 31-byte names leaves whole files, and the sweep reaches the two-entry rename in instruments/ and patterns/") {
  const Fixture f = make_fixture(5);
  import_fixture(f, export_arena());
  Audio au(export_arena());
  for (unsigned slot = 0; slot < 6; ++slot) {
    char name[40];
    std::snprintf(name, sizeof name, "Instrument with a long name no %u", slot);  // (cut to 28 bytes in the file name: "NN " + 28 + ".pti" is four directory slots)
    au.add(slot, name, saw(60 + slot, slot), 44100);
  }
  std::map<std::string, Bytes> want1;
  {
    Rig ref;
    NEED(ref.start(export_arena().project, "Projects/Song", &au.bank) == Start::Started);
    NEED(ref.run());
    NEED(ref.saves->og_state() == OgState::Done);
    want1 = ref.card.files;
  }
  NEED(want1.size() == 6u + 5u + 2u);
  const std::string root1 = "export/song og/";
  const std::string root2 = "export/song og 2/";
  const auto rel = [&](const std::string& key) { return key.substr(root1.size()); };  // (FatFs finds names case-insensitively: the lower-case key does)
  std::uint64_t blocks = 0;
  {
    Medium medium{70'000};
    build_card(medium);
    FatStack stack(medium);
    NEED(stack.mount());
    const std::uint64_t before = medium.blocks_written;
    NEED(stack.saves->start_og_export(stack.jobs, export_arena().project, "Projects/Song", &au.bank) == Start::Started);
    NEED(stack.run());
    CHECK(stack.saves->og_state() == OgState::Done);
    blocks = medium.blocks_written - before;
    for (const auto& kv : want1) {
      CHECK_MESSAGE(read_all(stack.fat, "Export/Song OG/" + rel(kv.first)) == kv.second, kv.first);
    }
    CHECK(user_files_intact(stack.fat));
  }
  NEED(blocks > 100u);
  unsigned complete = 0;
  unsigned two_names = 0;          // cuts that left the temp AND a final name holding the same whole file
  unsigned two_names_instruments = 0;
  unsigned refused_check = 0;
  for (std::uint64_t cut = 0; cut <= blocks; ++cut) {
    INFO("power cut at block " << cut << " of " << blocks);
    Medium medium{70'000};
    build_card(medium);
    {
      FatStack stack(medium);
      NEED(stack.mount());
      medium.fail_writes_after = static_cast<std::uint32_t>(cut);
      if (stack.saves->start_og_export(stack.jobs, export_arena().project, "Projects/Song", &au.bank) == Start::Started) {
        (void)stack.run();
      }
    }
    medium.fail_writes_after = 0xFFFFFFFFu;
    FatStack boot(medium);
    NEED(boot.mount());
    CHECK(user_files_intact(boot.fat));
    const bool project = present(boot.fat, "Export/Song OG/project.mt");
    for (const auto& kv : want1) {
      const std::string path = "Export/Song OG/" + rel(kv.first);
      const Bytes got = read_all(boot.fat, path);
      CHECK_MESSAGE((got.empty() ? !present(boot.fat, path) : got == kv.second), path);
      if (project) {
        CHECK_MESSAGE(present(boot.fat, path), "project.mt exists but " << path << " does not");
      }
    }
    complete += project ? 1u : 0u;
    for (const char* dir : {"Export/Song OG", "Export/Song OG/patterns", "Export/Song OG/instruments"}) {
      const Bytes temp = read_all(boot.fat, std::string(dir) + "/_o3save.tmp");
      for (const auto& kv : want1) {
        const std::string r = rel(kv.first);
        const std::string d = r.find('/') == std::string::npos ? "Export/Song OG" : "Export/Song OG/" + r.substr(0, r.find('/'));
        if (d == dir && !temp.empty() && temp == kv.second && present(boot.fat, "Export/Song OG/" + r) && read_all(boot.fat, "Export/Song OG/" + r) == temp) {
          ++two_names;
          two_names_instruments += r.rfind("instruments/", 0) == 0 ? 1u : 0u;
        }
      }
    }
    if (boot.saves->start_og_export(boot.jobs, export_arena().project, "Projects/Song", &au.bank) == Start::Started) {
      const bool folder_one_exists = present(boot.fat, "Export/Song OG");
      NEED(boot.run());
      if (boot.saves->og_state() == OgState::Done) {
        const std::string root = folder_one_exists ? root2 : root1;
        for (const auto& kv : want1) {
          const std::string path = (folder_one_exists ? "Export/Song OG 2/" : "Export/Song OG/") + rel(kv.first);
          CHECK_MESSAGE(read_all(boot.fat, path) == kv.second, path << " (" << root << ")");
        }
      } else {
        ++refused_check;
      }
    }
    CHECK(user_files_intact(boot.fat));
  }
  CHECK(complete >= 1u);
  CHECK_MESSAGE(two_names >= 1u, "no cut left a temp and a final name side by side: the sweep does not reach the two-entry case");
  CHECK_MESSAGE(two_names_instruments >= 1u, "none of them in instruments/");
  MESSAGE("FatFs cuts: " << blocks + 1u << ", complete: " << complete << ", two names for one file: " << two_names << " (instruments " << two_names_instruments << "), next exports stopped on a card check: " << refused_check);
}

// ---- OS-FILES-C C2c: what the user is told ---------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("og export C2c: with no sample memory every sample instrument is skipped under its own reason, not as having no audio") {
  const Fixture f = make_fixture(1);
  import_fixture(f, export_arena());
  Audio au(export_arena());
  au.add(0, "One", saw(400, 1), 44100);
  au.add(1, "Two", saw(400, 2), 44100);
  au.add(2, "NoAudio", {}, 44100);
  {
    Rig rig;  // the bank is there: the one with no audio is the only one skipped, and not for want of memory
    NEED(rig.start(export_arena().project, "Projects/Song", &au.bank) == Start::Started);
    CHECK(rig.saves->og_drops().instruments_not_written == 1u);
    CHECK(rig.saves->og_drops().no_sample_memory == 0u);
  }
  Rig rig;  // no bank at all (the platform's sample memory is not up): all three, under the reason that says so
  NEED(rig.start(export_arena().project, "Projects/Song", nullptr) == Start::Started);
  const og_ns::ExportDrops& d = rig.saves->og_drops();
  CHECK(d.instruments_not_written == 3u);
  CHECK(d.no_sample_memory == 3u);
  CHECK(d.any_lost());
  app::OgLossItem items[app::kOgLossMax];
  const unsigned n = app::og_loss_items(d, items);
  NEED(n >= 1u);
  CHECK(std::string(items[0].brief) == "NO SAMPLE MEM");  // (the kind "NOT WRITTEN" is the ones skipped for another reason: none here)
  CHECK(items[0].n == 3u);
  NEED(rig.run());
  CHECK(rig.saves->og_state() == OgState::Done);
}


// OS-CEWTENV W3a review: kOgLossMax went to 17 with the WT MOD kind — the loss strip (app/src/screens/file_screen.cpp) draws into an array of kOgLossMax, so every kind the
// table has must fit: all 18 at once (LANES since OS-MIDIREC-P1 S7d), each once, WT MOD among them.
TEST_CASE("og export: the loss list holds all kOgLossMax (18) kinds at once, WT MOD among them") {
  og_ns::ExportDrops d{};
  d.instruments_not_written = 30;  // above no_sample_memory, so the "NOT WRITTEN" kind (the rest) is non-zero too
  d.no_sample_memory = 2;
  d.bit_engine = 3;
  d.amiga_engine = 4;
  d.stereo_mid = 5;
  d.pcm_too_long = 6;
  d.levels_clamped = 7;
  d.module_instruments = 8;
  d.engine_fx = 9;
  d.core_fx = 10;
  d.ce_fx = 11;
  d.volume_dropped = 12;
  d.tracks_beyond_12 = 13;
  d.track_settings = 14;
  d.midi_instruments = 15;
  d.midi_settings = 16;
  d.sample_slots_beyond_48 = 17;
  d.lfo_sine = 18;
  d.wt_mod = 19;
  d.track_names_cut = 20;
  d.instrument_names_cut = 21;
  d.project_settings = 22;
  d.unknown_chunks = 23;
  d.pattern_names = 24;
  d.settings_ignored = 25;
  d.rate_ignored = 26;
  d.lane_events = 27;
  app::OgLossItem items[app::kOgLossMax];
  const unsigned n = app::og_loss_items(d, items);
  CHECK(app::kOgLossMax == 18u);
  CHECK(n == app::kOgLossMax);  // every kind fits the strip's array
  bool saw_wt = false;
  for (unsigned i = 0; i < n; ++i) {
    CHECK(items[i].n != 0u);
    for (unsigned j = 0; j < i; ++j) {
      CHECK(std::string(items[i].brief) != std::string(items[j].brief));  // each kind once
    }
    saw_wt = saw_wt || std::string(items[i].brief) == "WT MOD";
  }
  CHECK(saw_wt);
}

// OS-MIDIREC-P1 S7d (QA-PAT-496): the OG .mtp has no place for the MIDI take lanes, so the writer leaves them out and the report counts their events like the core FX it drops. The files do not change.
TEST_CASE("og export, OS-MIDIREC-P1 S7d: the lane events are counted as not exported, the survey counts the same, and the files are those of an export without lanes") {
  static model::LaneStore store;
  const Fixture f = make_fixture(1);
  struct Out {
    std::map<std::string, Bytes> files;
    og_ns::ExportDrops drops;
    std::uint32_t survey_lane_events;
    bool same_signature;
  };
  // `epoch_shift` 0: the store belongs to the project (adopted after the import, as the Editor does at a load); 1: it is another document's.
  const auto run = [&](const model::LaneStore* lanes, std::uint32_t epoch_shift) -> Out {
    import_fixture(f, export_arena());
    store.adopt(export_arena().project.load_epoch() + epoch_shift);
    Rig rig;
    rig.saves->attach_lanes(lanes);
    app::OgSurvey survey;
    CHECK(rig.saves->og_survey(export_arena().project, nullptr, "Projects/Song", survey) == Start::Started);
    CHECK(rig.saves->start_og_export(rig.jobs, export_arena().project, "Projects/Song", nullptr) == Start::Started);
    CHECK(rig.run());
    CHECK(rig.saves->og_state() == OgState::Done);
    return {rig.card.files, rig.saves->og_drops(), survey.drops.lane_events, app::og_loss_signature(survey.drops) == app::og_loss_signature(rig.saves->og_drops())};
  };
  store.clear();
  {
    model::LaneEvent* g = store.open_gap(1, 0, 0, 3);
    NEED(g != nullptr);
    g[0] = model::LaneEvent{0, 48, 0, 0, 60, 100};
    g[1] = model::LaneEvent{0, 48, 0, 0, 64, 100};
    g[2] = model::LaneEvent{96, 0, 1, 0, 74, 20};
    g = store.open_gap(1, 2, 0, 2);
    NEED(g != nullptr);
    g[0] = model::LaneEvent{10, 20, 0, 0, 40, 90};
    g[1] = model::LaneEvent{300, 0, 1, 0, 1, 5};
  }
  NEED(store.total_events() == 5u);
  const Out none = run(nullptr, 0u);
  CHECK(none.drops.lane_events == 0u);
  CHECK_FALSE(none.drops.any_lost());
  const Out some = run(&store, 0u);
  CHECK(some.drops.lane_events == 5u);
  CHECK(some.drops.any_lost());
  CHECK(some.survey_lane_events == 5u);
  CHECK(some.same_signature);
  CHECK(some.files == none.files);  // the lanes change no file
  app::OgLossItem items[app::kOgLossMax];
  const unsigned n = app::og_loss_items(some.drops, items);
  NEED(n == 1u);
  CHECK(std::string(items[0].brief) == "LANES");
  CHECK(items[0].n == 5u);
  CHECK(app::og_loss_signature(some.drops) != app::og_loss_signature(none.drops));

  // A store of another document (its epoch is not the project's) is not this project's lanes: nothing counted.
  const Out stale = run(&store, 1u);
  CHECK(stale.drops.lane_events == 0u);
}

// OS-STEREO (e): a stereo instrument is written as its mid M, a mono .pti; the OG export has no place for S, so it is counted (and told in the loss line), and nothing else of the export changes.
TEST_CASE("og export, OS-STEREO (e): a stereo instrument writes the very files of the mono one and is counted as MID WRITTEN") {
  const Fixture f = make_fixture(1);
  const std::vector<std::int16_t> m = saw(500, 7);
  struct Out {
    std::map<std::string, Bytes> files;
    og_ns::ExportDrops drops;
  };
  const auto run = [&](bool stereo) -> Out {
    import_fixture(f, export_arena());
    Audio au(export_arena());
    au.add(0, "Same", m, 44100);
    au.add(1, "Other", saw(300, 3), 44100);
    if (stereo) {  // slot 0 again, with a side block behind the same M, and the instrument's STEREO row on
      const std::uint32_t frames = static_cast<std::uint32_t>(m.size());
      const std::uint32_t even = (frames + 1u) & ~1u;
      std::uint32_t cap = 0;
      std::int16_t* p = au.bank.reserve(cap);
      CHECK(p != nullptr);
      std::copy(m.begin(), m.end(), p);
      for (std::uint32_t i = 0; i < frames; ++i) {
        p[even + i] = static_cast<std::int16_t>(-static_cast<int>(i % 7) - 1);
      }
      model::Instrument in = *export_arena().project.instrument(0);
      in.stereo = 1;
      CHECK(export_arena().project.set_instrument(0, in) == model::Status::Ok);
      CHECK(au.bank.commit_load(0, p, frames, p + even));
      au.bank.rebind_now(0);
      CHECK(au.bank.edit_stereo(0));
    }
    Rig rig;
    CHECK(rig.start(export_arena().project, "Projects/Song", &au.bank) == Start::Started);
    Out out;
    out.drops = rig.saves->og_drops();
    CHECK(rig.run());
    CHECK(rig.saves->og_state() == OgState::Done);
    out.files = rig.card.files;
    return out;
  };
  const Out mono = run(false);
  const Out st = run(true);
  CHECK(mono.drops.stereo_mid == 0u);
  CHECK(st.drops.stereo_mid == 1u);
  CHECK(st.files == mono.files);  // every file, byte for byte: the .pti of the stereo instrument is its mid
  CHECK_FALSE(mono.drops.any_lost());
  CHECK(st.drops.any_lost());
  char line[48];
  CHECK_FALSE(app::og_loss_line(mono.drops, line, sizeof line));
  CHECK(app::og_loss_line(st.drops, line, sizeof line));
  CHECK(std::string(line) == "1 STEREO");
  CHECK(app::og_loss_signature(st.drops) != app::og_loss_signature(mono.drops));
}

TEST_CASE("og export C2c: the loss line names every C2b counter, fits the strip, cuts with .. and changes its signature with any counter") {
  og_ns::ExportDrops d;
  char line[48];
  CHECK_FALSE(app::og_loss_line(d, line, sizeof line));
  CHECK(line[0] == '\0');
  d.resampled = 4;  // a conversion, not a loss: the question is not asked for it, nor does the line name it
  d.levels_converted = 2;
  CHECK_FALSE(d.any_lost());
  CHECK_FALSE(app::og_loss_line(d, line, sizeof line));
  d.instruments_not_written = 5;
  d.bit_engine = 2;
  d.amiga_engine = 3;
  d.pcm_too_long = 1;
  d.levels_clamped = 2;
  CHECK(d.any_lost());
  char wide[160];
  CHECK(app::og_loss_line(d, wide, sizeof wide));
  CHECK(std::string(wide) == "5 NOT WRITTEN, 2 12/8 BIT, 3 AMIGA, 1 TOO LONG, 2 CLAMPED");
  CHECK(app::og_loss_line(d, line, sizeof line));
  CHECK(std::strlen(line) < sizeof line);
  CHECK(std::string(line) == "5 NOT WRITTEN, 2 12/8 BIT, 3 AMIGA, ..");  // the strip's room: as many as fit, then ".."
  d.module_instruments = 3;
  d.core_fx = 10;
  d.engine_fx = 4;
  CHECK(app::og_loss_line(d, line, 30));
  CHECK(std::strlen(line) < 30u);
  CHECK(std::string(line).substr(std::strlen(line) - 4) == ", ..");
  // every cap holds: never past it, always terminated
  for (unsigned cap = 1; cap <= 60; ++cap) {
    char buf[64];
    std::memset(buf, 'x', sizeof buf);
    (void)app::og_loss_line(d, buf, cap);
    CHECK(std::strlen(buf) < cap);
  }
  // the NO SAMPLE MEM kind comes out of NOT WRITTEN, not on top of it
  og_ns::ExportDrops m;
  m.instruments_not_written = 6;
  m.no_sample_memory = 6;
  CHECK(app::og_loss_line(m, wide, sizeof wide));
  CHECK(std::string(wide) == "6 NO SAMPLE MEM");
  // the signature moves with every counter
  const std::uint32_t base = app::og_loss_signature(d);
  og_ns::ExportDrops e = d;
  e.levels_clamped += 1;
  CHECK(app::og_loss_signature(e) != base);
  e = d;
  e.no_sample_memory += 1;
  CHECK(app::og_loss_signature(e) != base);
  CHECK(app::og_loss_signature(d) == base);
}

TEST_CASE("og export C2c minors: the survey in slices - one small piece a step, the same counters as the whole run, a change restarts it, a full card fails it") {
  const Fixture f = make_fixture(2);
  import_fixture(f, export_arena());
  Audio au(export_arena());
  au.add(0, "Saw", saw(1000, 1), 22050);
  au.add(1, "Bits", saw(400, 2), 44100, [](model::Instrument& in) { in.engine = model::kInstEngineBits12; });
  Rig rig;
  model::Project& project = export_arena().project;
  app::OgSurvey whole;
  NEED(rig.saves->og_survey(project, &au.bank, "Projects/Song", whole) == Start::Started);
  CHECK_FALSE(rig.saves->og_survey_running());  // (the whole run left nothing running)

  using Step = app::SaveService::SurveyStep;
  NEED(rig.saves->og_survey_begin(project, &au.bank, "Projects/Song") == Start::Started);
  CHECK(rig.saves->og_survey_running());
  CHECK(rig.card.log.empty());
  unsigned steps = 0;
  Step st = Step::More;
  while (st == Step::More && steps < 100u) {
    const std::size_t before = rig.card.exists_calls;
    st = rig.saves->og_survey_step();
    CHECK(rig.card.exists_calls - before <= 4u);  // the folder name: a few lookups a step
    ++steps;
  }
  NEED(st == Step::Done);
  CHECK(steps >= whole.files + 1u);  // a step a file at least (the folder and the plan are steps of their own)
  CHECK(steps <= whole.files + 4u);
  CHECK_FALSE(rig.saves->og_survey_running());
  CHECK(rig.saves->og_survey_current());
  const app::OgSurvey& sliced = rig.saves->og_survey_result();
  CHECK(std::string(sliced.folder) == std::string(whole.folder));
  CHECK(sliced.files == whole.files);
  CHECK(sliced.bytes == whole.bytes);
  CHECK(app::og_loss_signature(sliced.drops) == app::og_loss_signature(whole.drops));
  CHECK(sliced.drops.bit_engine == 1u);

  // a change of the document says the result is no longer current; a run in progress starts again and ends on the new project's counts
  NEED(rig.saves->og_survey_begin(project, &au.bank, "Projects/Song") == Start::Started);
  for (unsigned i = 0; i < 4; ++i) {
    NEED(rig.saves->og_survey_step() == Step::More);
  }
  NEED(project.create_pattern(9, 16) == model::Status::Ok);
  st = Step::More;
  for (unsigned i = 0; st == Step::More && i < 100u; ++i) {
    st = rig.saves->og_survey_step();
  }
  NEED(st == Step::Done);
  CHECK(rig.saves->og_survey_result().files == whole.files + 1u);
  CHECK(rig.saves->og_survey_current());
  NEED(project.delete_pattern(9) == model::Status::Ok);
  CHECK_FALSE(rig.saves->og_survey_current());

  // cancel
  NEED(rig.saves->og_survey_begin(project, &au.bank, "Projects/Song") == Start::Started);
  rig.saves->og_survey_cancel();
  CHECK_FALSE(rig.saves->og_survey_running());
  CHECK_FALSE(rig.saves->og_survey_current());

  // a card with every folder name taken: the run fails with NoName, a few lookups a step
  Rig full;
  full.card.make_dir_now("Export/Song OG");
  for (unsigned n = 2; n <= 99; ++n) {
    full.card.make_dir_now(("Export/Song OG " + std::to_string(n)).c_str());
  }
  NEED(full.saves->og_survey_begin(project, &au.bank, "Projects/Song") == Start::Started);
  st = Step::More;
  steps = 0;
  while (st == Step::More && steps < 100u) {
    const std::size_t before = full.card.exists_calls;
    st = full.saves->og_survey_step();
    CHECK(full.card.exists_calls - before <= 4u);
    ++steps;
  }
  CHECK(st == Step::Failed);
  CHECK(full.saves->og_survey_why() == Start::NoName);
  CHECK(steps >= 99u / 4u);
  CHECK_FALSE(full.saves->og_survey_running());
  CHECK_FALSE(full.saves->og_survey_current());
}

TEST_CASE("og export C2c: the survey says the folder, the files and the size the export will have, and the counters it will start with") {
  const Fixture f = make_fixture(2);
  import_fixture(f, export_arena());
  Audio au(export_arena());
  au.add(0, "Saw", saw(1000, 1), 22050);  // resampled: 2,000 frames out
  au.add(1, "Bits", saw(400, 2), 44100, [](model::Instrument& in) { in.engine = model::kInstEngineBits12; });
  Rig rig;
  app::OgSurvey sv;
  NEED(rig.saves->og_survey(export_arena().project, &au.bank, "Projects/Song", sv) == Start::Started);
  CHECK(std::string(sv.folder) == "Song OG");
  CHECK(sv.drops.bit_engine == 1u);
  CHECK(sv.drops.instruments_not_written == 1u);
  CHECK(sv.drops.resampled == 1u);
  CHECK(sv.files == 1u + 2u + 2u);  // the instrument, 2 patterns, patternsMetadata, project.mt
  const std::uint32_t floor = og_ns::kInstrumentBytes + 2000u * 2u + 2u * og_ns::kPatternBytes12 + og_ns::kProjectBytes16;
  CHECK(sv.bytes >= floor);
  CHECK(sv.bytes <= floor + og_ns::kPamdBytes);
  CHECK(rig.card.log.empty());  // a survey writes nothing, and starts nothing
  CHECK(rig.saves->og_state() == OgState::Idle);
  // the survey and the start count the same
  NEED(rig.start(export_arena().project, "Projects/Song", &au.bank) == Start::Started);
  CHECK(rig.saves->og_files_total() == sv.files);
  NEED(rig.run());
  CHECK(app::og_loss_signature(rig.saves->og_drops()) == app::og_loss_signature(sv.drops));
  // a second survey takes the next free folder, and a card with all taken says so
  NEED(rig.saves->og_survey(export_arena().project, &au.bank, "Projects/Song", sv) == Start::Started);
  CHECK(std::string(sv.folder) == "Song OG 2");
  // no patterns: nothing to export
  model::ModelArena& empty = arena(1);  // (the project name of the survey above is Song: the folder name comes from "Projects/Song")
  empty.project.reset();
  CHECK(rig.saves->og_survey(empty.project, nullptr, "Projects/Song", sv) == Start::Empty);
  CHECK(sv.folder[0] == '\0');
  CHECK(std::string(app::SaveService::og_start_text(Start::Empty, hal::storage::WriteStatus::Ok)) == "NOTHING TO EXPORT");
}

// ---- OS-FILES-C: the work of one step is bounded --------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

// A sink that counts what each step wrote.
class StepSink final : public core::ByteSink {
 public:
  core::IoResult write(const void*, std::size_t n) noexcept override {
    since += n;
    total += n;
    return core::IoResult{n, core::IoStatus::Ok};
  }
  std::size_t since = 0;
  std::size_t total = 0;
};

}  // namespace

TEST_CASE("og export: a step of any file's source writes about its byte budget, not the whole file (the bound the job's slice relies on)") {
  const Fixture f = make_fixture(4);
  import_fixture(f, export_arena());
  const model::Project& project = export_arena().project;
  app::OgExportPlan plan;
  NEED(plan.begin(project, nullptr));
  constexpr std::size_t kBudget = 512;
  unsigned files = 0;
  std::size_t worst = 0;
  std::size_t biggest_file = 0;
  app::OgFile file;
  while (plan.next(file)) {
    app::OgFileSource src;
    src.set(project, file, plan.generation());
    StepSink sink;
    src.begin(sink);
    formats::Progress p = formats::Progress::Running;
    unsigned steps = 0;
    while (p == formats::Progress::Running && steps < 200000u) {
      sink.since = 0;
      p = src.step(kBudget);
      worst = std::max(worst, sink.since);
      ++steps;
    }
    CHECK(p == formats::Progress::Done);
    biggest_file = std::max(biggest_file, sink.total);
    ++files;
    plan.advance();
  }
  CHECK(files >= 4u);
  CHECK(biggest_file > 4u * kBudget);  // the control: the files are many budgets long
  MESSAGE("worst bytes of one step with a budget of " << kBudget << ": " << worst << " (largest file " << biggest_file << ")");
  CHECK(worst <= 2u * kBudget);
}

TEST_CASE("og export: every write of the files' jobs is at most the save job's step bound (AtomicSave::kMaxStepBytes), the instruments' included") {
  const Fixture f = make_fixture(4);
  import_fixture(f, export_arena());
  Audio au(export_arena());
  au.add(0, "Saw", saw(6000, 1), 22050);  // resampled: 12,000 frames, 24 KB of PCM
  Rig rig;
  NEED(rig.start(export_arena().project, "Projects/Song", &au.bank) == Start::Started);
  NEED(rig.run());
  unsigned writes = 0;
  std::size_t biggest = 0;
  for (const std::string& l : rig.card.log) {
    if (l.rfind("write ", 0) == 0) {
      ++writes;
      biggest = std::max<std::size_t>(biggest, std::stoul(l.substr(6)));
    }
  }
  CHECK(writes > 24u);
  CHECK(biggest <= app::AtomicSave::kMaxStepBytes);
  MESSAGE("writes " << writes << ", the biggest " << biggest << " bytes");
}

// ---- T16: the per-stage timing of the export (app/og_timing.h), with a stub counter ---------------------------------------------------------------------------------------------------------

namespace {

// A counter that advances one tick at every read: every piece of work between two reads costs the reads it makes, so each stage that runs has a non-zero, repeatable cost.
std::uint32_t g_stub_ticks = 0;
std::uint32_t stub_counter() noexcept { return ++g_stub_ticks; }

}  // namespace

TEST_CASE("og export T16: the timing table fills for every stage that ran, the worst step names its stage, the survey's row survives the start and the next export starts clean") {
  const Fixture f = make_fixture(4);
  import_fixture(f, export_arena());
  using Stage = app::OgTiming::Stage;
  Rig rig;
  rig.saves->attach_cycles(&stub_counter, 1);
  const app::OgTiming& t = rig.saves->og_timing();
  for (unsigned s = 0; s < app::OgTiming::kStages; ++s) {
    CHECK(t.count(static_cast<Stage>(s)) == 0u);  // nothing before the first piece
  }
  char text[80];
  CHECK(t.line(0, text, sizeof text) == 0u);

  app::OgSurvey whole;
  NEED(rig.saves->og_survey(export_arena().project, nullptr, "Projects/Song", whole) == Start::Started);
  const std::uint32_t survey_pieces = t.count(Stage::Survey);
  CHECK(survey_pieces > 0u);
  CHECK(t.max_us(Stage::Survey) > 0u);

  NEED(rig.start(export_arena().project) == Start::Started);
  CHECK(t.count(Stage::Survey) == survey_pieces);  // the start clears the export's rows, not the question's
  NEED(rig.run());
  NEED(rig.saves->og_state() == OgState::Done);
  const unsigned files = rig.saves->og_files_total();
  CHECK(files == 6u);
  for (const Stage s : {Stage::Plan, Stage::Convert, Stage::Write, Stage::Sync, Stage::Rename, Stage::Dir, Stage::Verify, Stage::Other}) {
    INFO(app::OgTiming::name(s));
    CHECK(t.count(s) > 0u);
    CHECK(t.max_us(s) > 0u);
    CHECK(t.sum_us(s) >= t.max_us(s));
  }
  CHECK(t.count(Stage::Sync) == files);    // one finish() a file
  CHECK(t.count(Stage::Verify) >= files);  // an open, at least a read and the closing read a file
  CHECK(t.count(Stage::Write) > files);    // several f_write calls a file
  CHECK(t.worst_us() > 0u);
  CHECK(t.worst_us() >= t.max_us(Stage::Sync));  // (a step is at least its sync)
  std::uint32_t all = 0;
  for (unsigned i = 0; i < app::OgTiming::kStages; ++i) {
    all += t.sum_us(static_cast<Stage>(i));
  }
  CHECK(t.worst_us() < all);  // a step is a piece of the whole export, not more than all of it
  const Stage worst = t.worst_stage();
  CHECK(t.max_us(worst) > 0u);  // the worst step names a stage that ran

  // the console's lines: the worst step, then one line a stage that ran, all short enough for the ring
  unsigned lines = 0;
  for (unsigned i = 0; i < app::OgTiming::kLines; ++i) {
    const std::size_t n = t.line(i, text, sizeof text);
    if (n != 0u) {
      ++lines;
      CHECK(n == std::strlen(text));
      CHECK(std::strncmp(text, "time ", 5) == 0);
      CHECK(n < 60u);
    }
  }
  CHECK(lines == 1u + app::OgTiming::kStages);  // the worst and every stage (the survey's included)
  NEED(t.line(0, text, sizeof text) > 0u);
  CHECK(std::string(text).rfind("time worst ", 0) == 0);
  CHECK(std::string(text).find(std::string(" us in ") + app::OgTiming::name(worst)) != std::string::npos);
  CHECK(t.line(app::OgTiming::kLines, text, sizeof text) == 0u);

  // a second export starts clean: the counts are those of one export again, not the sum of two
  const std::uint32_t writes = t.count(Stage::Write);
  NEED(rig.start(export_arena().project) == Start::Started);
  NEED(rig.run());
  CHECK(t.count(Stage::Sync) == files);
  CHECK(t.count(Stage::Write) == writes);
  CHECK(t.count(Stage::Survey) == survey_pieces);

  // no counter attached: nothing is noted, and the export is the same
  Rig plain;
  NEED(plain.start(export_arena().project) == Start::Started);
  NEED(plain.run());
  CHECK(plain.saves->og_state() == OgState::Done);
  for (unsigned s = 0; s < app::OgTiming::kStages; ++s) {
    CHECK(plain.saves->og_timing().count(static_cast<Stage>(s)) == 0u);
  }
  CHECK(plain.saves->og_timing().line(0, text, sizeof text) == 0u);
  CHECK(plain.card.files.at("export/song og/project.mt") == rig.card.files.at("export/song og/project.mt"));  // (the timing changes no byte)
}
