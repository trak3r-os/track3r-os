// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-007e: the host directory as a read-only card (platform/host DirStorage) and a project loaded from it with the OG readers (app/og_folder.h).
// DirStorage is held to the same contract the FatFs-backed storage is (hal/storage.h); then one OG project folder, built from the byte maps of
// support/og_fixture.h, is written to a temporary directory, loaded in slices, and the model it makes is compared with the same files read straight
// through the formats readers. Host only (it makes files). The same folder through FatFs is og_folder_fat_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_folder_check.h"

#include <o3os/app/og_folder.h>
#include <o3os/formats/og.h>
#include <o3os/hal/storage.h>
#include <o3os/model/project.h>
#include <o3os/platform/host/dir_storage.h>

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace hs = o3os::hal::storage;
using o3os::app::OgFolderLoader;
using o3os::platform::host::DirStorage;
using namespace o3os::test::og;
namespace formats = o3os::formats;

namespace {

// A temporary directory that is removed with the object.
struct TempDir {
  fs::path path;
  TempDir() {
    static unsigned counter = 0;
    path = fs::temp_directory_path() / ("o3os_og_folder_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

void write_file(const fs::path& root, const std::string& rel, const Bytes& bytes) {
  const fs::path p = root / rel;
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_folder(const fs::path& root, const std::string& folder, const std::vector<FolderFile>& files) {
  for (const FolderFile& f : files) {
    write_file(root, folder + "/" + f.path, f.bytes);
  }
}

// Every file and directory under `root` with its content: what "nothing was written" is compared against.
std::map<std::string, std::string> snapshot(const fs::path& root) {
  std::map<std::string, std::string> out;
  for (const fs::directory_entry& e : fs::recursive_directory_iterator(root)) {
    const std::string rel = fs::relative(e.path(), root).generic_string();
    if (e.is_directory()) {
      out[rel + "/"] = "";
    } else {
      std::ifstream in(e.path(), std::ios::binary);
      out[rel] = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
  }
  return out;
}

}  // namespace

// ---- DirStorage honours the contract ----------------------------------------------------------------------------------------------------------
TEST_CASE("dir storage: mount, exists, read, seek, size, close") {
  TempDir tmp;
  Bytes data(2000);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::uint8_t>(i * 13u + 5u);
  }
  write_file(tmp.path, "Workspace/project.mt", data);
  write_file(tmp.path, "README.TXT", text_bytes("hello"));
  DirStorage s(tmp.path.string());
  CHECK(!s.mounted());
  CHECK(s.card_present());
  hs::FileHandle f;
  CHECK(s.open("README.TXT", f) == hs::Status::NotMounted);
  NEED(s.mount() == hs::Status::Ok);
  CHECK(s.mounted());
  CHECK(s.volume().kind == hs::FsKind::HostDirectory);

  bool dir = false;
  CHECK(s.exists("workspace", &dir) == hs::Status::Ok);  // FAT does not tell the cases apart, and neither does this
  CHECK(dir);
  CHECK(s.exists("WORKSPACE/Project.MT", &dir) == hs::Status::Ok);
  CHECK(!dir);
  CHECK(s.exists("/", &dir) == hs::Status::Ok);
  CHECK(dir);
  CHECK(s.exists("Workspace/nothing", &dir) == hs::Status::NotFound);
  CHECK(!fs::exists(tmp.path / "Workspace" / "nothing"));  // a question never creates

  NEED(s.open("workspace/PROJECT.MT", f) == hs::Status::Ok);
  std::uint32_t size = 0;
  CHECK(s.size(f, size) == hs::Status::Ok);
  CHECK(size == 2000u);
  Bytes got(100);
  std::size_t n = 0;
  NEED(s.seek(f, 1950) == hs::Status::Ok);
  CHECK(s.read(f, got.data(), 100, n) == hs::Status::Ok);
  CHECK(n == 50u);
  CHECK(std::memcmp(got.data(), data.data() + 1950, 50) == 0);
  CHECK(s.read(f, got.data(), 100, n) == hs::Status::Ok);
  CHECK(n == 0u);  // the end
  NEED(s.seek(f, 0xFFFFFFFFu) == hs::Status::Ok);
  CHECK(s.read(f, got.data(), 100, n) == hs::Status::Ok);
  CHECK(n == 0u);  // clamped to the end
  NEED(s.seek(f, 0) == hs::Status::Ok);
  CHECK(s.read(f, got.data(), 100, n) == hs::Status::Ok);
  CHECK(n == 100u);
  CHECK(std::memcmp(got.data(), data.data(), 100) == 0);
  CHECK(s.open_files() == 1u);
  s.close(f);
  CHECK(!f.is_open());
  CHECK(s.open_files() == 0u);
  CHECK(s.read(f, got.data(), 1, n) == hs::Status::BadHandle);
  CHECK(s.seek(f, 0) == hs::Status::BadHandle);
  CHECK(s.size(f, size) == hs::Status::BadHandle);
}

TEST_CASE("dir storage: listing, errors and handle limits, as the FAT storage's") {
  TempDir tmp;
  write_file(tmp.path, "a.txt", text_bytes("aaa"));
  write_file(tmp.path, "Dir/inner.bin", Bytes(300, 1));
  write_file(tmp.path, "Dir/Sub/deep.bin", Bytes(5, 2));
  DirStorage s(tmp.path.string());
  NEED(s.mount() == hs::Status::Ok);

  hs::DirHandle d;
  NEED(s.opendir("Dir", d) == hs::Status::Ok);
  std::map<std::string, std::pair<std::uint32_t, bool>> seen;
  hs::DirEntry e;
  hs::Status r;
  while ((r = s.readdir(d, e)) == hs::Status::Ok) {
    seen[e.name] = {e.size, e.is_dir};
  }
  CHECK(r == hs::Status::End);
  CHECK(s.readdir(d, e) == hs::Status::End);  // and again
  s.closedir(d);
  CHECK(!d.is_open());
  CHECK(seen.size() == 2u);  // never . or ..
  CHECK(seen["inner.bin"] == std::make_pair(300u, false));
  CHECK(seen["Sub"] == std::make_pair(0u, true));

  hs::FileHandle f;
  CHECK(s.open("Dir", f) == hs::Status::NotAFile);
  CHECK(s.open("nothing.txt", f) == hs::Status::NotFound);
  CHECK(s.open("a.txt/x", f) == hs::Status::NotADirectory);
  CHECK(s.opendir("a.txt", d) == hs::Status::NotADirectory);
  CHECK(s.opendir("Nope", d) == hs::Status::NotFound);
  CHECK(s.open("../a.txt", f) == hs::Status::BadPath);
  CHECK(s.open("Dir/./inner.bin", f) == hs::Status::BadPath);
  CHECK(s.open("a|b", f) == hs::Status::BadPath);
  CHECK(s.open("C:/x", f) == hs::Status::BadPath);
  CHECK(s.open(nullptr, f) == hs::Status::BadPath);
  CHECK(s.opendir("../x", d) == hs::Status::BadPath);
  CHECK(s.exists("../x", nullptr) == hs::Status::BadPath);
  CHECK(s.open("", f) == hs::Status::NotAFile);
  CHECK(s.open_files() == 0u);
  CHECK(s.open_dirs() == 0u);
  CHECK(s.readdir(hs::DirHandle{}, e) == hs::Status::BadHandle);

  hs::FileHandle f1, f2, f3;
  NEED(s.open("a.txt", f1) == hs::Status::Ok);
  NEED(s.open("Dir/inner.bin", f2) == hs::Status::Ok);
  CHECK(f1.slot != f2.slot);
  CHECK(s.open("Dir/Sub/deep.bin", f3) == hs::Status::NoHandle);
  hs::DirHandle d1, d2, d3;
  NEED(s.opendir("Dir", d1) == hs::Status::Ok);
  NEED(s.opendir("Dir/Sub", d2) == hs::Status::Ok);
  CHECK(s.opendir("/", d3) == hs::Status::NoHandle);
  s.close(f1);
  s.close(f2);
  s.closedir(d1);
  s.closedir(d2);
  CHECK(s.open_files() == 0u);
  CHECK(s.open_dirs() == 0u);

  // mount() closes every handle, a missing root is no card.
  NEED(s.open("a.txt", f1) == hs::Status::Ok);
  CHECK(s.mount() == hs::Status::Ok);
  CHECK(s.open_files() == 0u);
  DirStorage missing((tmp.path / "no-such-card").string());
  CHECK(!missing.card_present());
  CHECK(missing.mount() == hs::Status::NoCard);
  CHECK(!missing.mounted());
}

// ---- The loader ---------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("og folder: a project folder loads in slices and makes the model the readers make; nothing on the card is written") {
  TempDir tmp;
  const std::vector<FolderFile> files = synthetic_folder();
  write_folder(tmp.path, "Workspace", files);
  const std::map<std::string, std::string> before = snapshot(tmp.path);

  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  OgFolderLoader& l = loader();
  o3os::model::ModelArena& arena = o3os::test::og::arena(0);
  arena.project.reset();
  NEED(l.begin(card, "Workspace", arena));
  CHECK(l.state() == OgFolderLoader::State::Running);
  unsigned steps = 0;
  NEED(run_loader(l, 600, &steps) == OgFolderLoader::State::Done);
  CHECK(steps > 10u);  // a project this size is many passes of the main loop, not one

  const o3os::app::OgLoadReport& rep = l.report();
  CHECK(rep.project_ok);
  CHECK(rep.version == 16);
  CHECK(!rep.unverified_version);
  CHECK(rep.patterns == kFolderPatternsGood);
  CHECK(rep.patterns_failed == kFolderPatternsBad);
  CHECK(rep.instruments_failed == kFolderInstrumentsBad);
  // The .mti of slot 5 (instrument_05) is skipped when the .pti was seen first and applied then overwritten when it was not: both orders end the same.
  CHECK(rep.instruments + rep.instruments_superseded == kFolderPtiFiles + kFolderMtiFiles);
  CHECK(rep.instruments_superseded <= 1u);
  CHECK(rep.pcm_frames == kFolderPcmFrames);  // declared by the .pti headers, not loaded (a later slice)
  CHECK(rep.files_ignored == kFolderIgnored);
  CHECK(rep.error != formats::FormatError::None);  // the first file that did not read is reported ...
  CHECK(std::strlen(rep.detail) > 0u);
  CHECK(rep.storage_error == hs::Status::Ok);      // ... but the card never failed
  CHECK(rep.bytes_read > 2320u + 4u * 9260u);
  CHECK(card.open_files() == 0u);  // nothing left open
  CHECK(card.open_dirs() == 0u);

  const o3os::model::Project& p = arena.project;
  CHECK(std::string(p.name()) == "Synthetic Project");
  NEED(p.pattern(1) != nullptr);
  CHECK(p.pattern(2) != nullptr);
  CHECK(p.pattern(3) != nullptr);
  CHECK(p.pattern(12) != nullptr);
  CHECK(p.pattern(7) == nullptr);  // the cut file left nothing behind
  CHECK(p.pattern(9) == nullptr);  // the one in a subdirectory was never read
  NEED(p.instrument(4) != nullptr);
  CHECK(std::string(p.instrument(4)->name) == "New Clap");  // the .pti beside the .mti
  CHECK(p.instrument(5) == nullptr);                        // the one with the bad CRC
  CHECK(p.instrument(0) != nullptr);
  CHECK(p.instrument(1) != nullptr);
  CHECK(p.instrument(2) != nullptr);
  CHECK(std::string(p.instrument(2)->name) == "Workspace Hat.wav");

  // The model is the one the readers make from the same bytes.
  o3os::model::ModelArena& direct = o3os::test::og::arena(1);
  load_direct(files, direct);
  check_same_model(p, direct.project);

  // The card is as it was: not one byte written, nothing created, nothing removed.
  CHECK(snapshot(tmp.path) == before);
}

TEST_CASE("og folder: a folder with v6 patterns loads them, a damaged one is counted and the rest loads (OS-CEPAT S1)") {
  TempDir tmp;
  std::vector<FolderFile> files;
  ProjectSpec ps;  // v16, "Synthetic Project"
  files.push_back({"project.mt", project_bytes(ps)});
  files.push_back({"patterns/pattern_01.mtp", pattern6_bytes(rich_pattern6(16, 6, 71))});  // 16 tracks
  files.push_back({"patterns/pattern_02.mtp", pattern6_bytes(rich_pattern6(8, 6, 72))});   // 8 tracks
  Bytes cut = pattern6_bytes(rich_pattern6(12, 6, 73));
  cut.resize(5000);  // ends inside the rows: refused, counted
  files.push_back({"patterns/pattern_05.mtp", cut});
  write_folder(tmp.path, "Workspace", files);
  const std::map<std::string, std::string> before = snapshot(tmp.path);

  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  OgFolderLoader& l = loader();
  o3os::model::ModelArena& arena = o3os::test::og::arena(0);
  arena.project.reset();
  NEED(l.begin(card, "Workspace", arena));
  NEED(run_loader(l, 4096) == OgFolderLoader::State::Done);

  const o3os::app::OgLoadReport& rep = l.report();
  CHECK(rep.project_ok);
  CHECK(rep.version == 16);
  CHECK(rep.patterns == 2);
  CHECK(rep.patterns_failed == 1);  // the cut v6 file
  CHECK(rep.error != formats::FormatError::None);  // the first file that did not read is reported ...
  CHECK(std::strlen(rep.detail) > 0u);
  CHECK(rep.storage_error == hs::Status::Ok);
  CHECK(card.open_files() == 0u);
  CHECK(card.open_dirs() == 0u);

  const o3os::model::Project& p = arena.project;
  NEED(p.pattern(1) != nullptr);
  CHECK(p.pattern(1)->track_length[15] == 32);  // the 16-track v6 file holds all 16 lengths
  NEED(p.pattern(2) != nullptr);
  CHECK(p.pattern(2)->track_length[15] == 32);  // the 8-track one: tracks 9-16 take track 1's length
  CHECK(p.pattern(5) == nullptr);               // the damaged one left nothing behind

  // The model is the one the readers make from the same bytes.
  o3os::model::ModelArena& direct = o3os::test::og::arena(1);
  load_direct(files, direct);
  check_same_model(p, direct.project);

  // The card is as it was.
  CHECK(snapshot(tmp.path) == before);
}

TEST_CASE("og folder: the v6 counters reach the load report and the strip names them (OS-CEPAT S2)") {
  TempDir tmp;
  std::vector<FolderFile> files;
  ProjectSpec ps;  // v16, "Synthetic Project"
  files.push_back({"project.mt", project_bytes(ps)});
  PatternSpec6 p6 = empty_pattern6(8, 6);
  auto st_in = [](PatternSpec6& pat, unsigned s, std::uint8_t t0, std::uint8_t v0, std::uint8_t t1, std::uint8_t v1, std::int8_t vol, std::int8_t m) {
    std::uint8_t* e = pat.step[0][s];
    e[0] = 60; e[1] = 1;
    e[2] = t0; e[3] = v0; e[4] = t1; e[5] = v1;
    e[6] = static_cast<std::uint8_t>(vol); e[7] = static_cast<std::uint8_t>(m);
  };
  auto st = [&p6, &st_in](unsigned s, std::uint8_t t0, std::uint8_t v0, std::uint8_t t1, std::uint8_t v1, std::int8_t vol, std::int8_t m) { st_in(p6, s, t0, v0, t1, v1, vol, m); };
  st(0, 0, 0, 0, 0, 50, -1);     // volume 50 -> vol 63
  st(1, 0, 0, 0, 0, 101, -1);    // above 100: full volume, counted in values_clamped
  st(2, 0, 0, 0, 0, -1, 50);     // micromove into the first free column as FX 2
  st(3, 4, 50, 5, 60, -1, 100);  // no free column: micromove dropped and counted
  st(4, 75, 10, 43, 11, -1, -1); // OG 75 cleared and counted, OG 43 -> model 96
  st(5, 74, 9, 128, 200, -1, -1);  // the boundaries: 74 -> 127 and 128 verbatim, neither counted
  files.push_back({"patterns/pattern_01.mtp", pattern6_bytes(p6)});
  // A second good pattern with counts of its own, so the report's totals are sums over the patterns and not the last pattern's (or the first's): 1 micromove dropped, 2 volumes
  // above 100, 2 OG ids 75..127 cleared (76 and 100).
  PatternSpec6 p6b = empty_pattern6(8, 6);
  st_in(p6b, 0, 6, 1, 7, 2, -1, 20);      // no free column: micromove dropped and counted
  st_in(p6b, 1, 0, 0, 0, 0, 120, -1);     // above 100: counted
  st_in(p6b, 2, 0, 0, 0, 0, 101, -1);     // above 100: counted
  st_in(p6b, 3, 76, 3, 100, 4, -1, -1);   // two ids cleared
  files.push_back({"patterns/pattern_03.mtp", pattern6_bytes(p6b)});
  Bytes cut = pattern6_bytes(rich_pattern6(8, 6, 99));
  cut.resize(5000);  // ends inside the rows: refused, counted
  files.push_back({"patterns/pattern_02.mtp", cut});
  write_folder(tmp.path, "Workspace", files);

  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  OgFolderLoader& l = loader();
  o3os::model::ModelArena& arena = o3os::test::og::arena(0);
  arena.project.reset();
  NEED(l.begin(card, "Workspace", arena));
  NEED(run_loader(l, 4096) == OgFolderLoader::State::Done);

  const o3os::app::OgLoadReport& rep = l.report();
  CHECK(rep.patterns == 2);
  CHECK(rep.patterns_failed == 1);
  CHECK(rep.micromoves_dropped == 2);  // step 3 of pattern 1, step 0 of pattern 3
  CHECK(rep.values_clamped == 3);      // step 1 of pattern 1, steps 1 and 2 of pattern 3
  CHECK(rep.ce_fx_cleared == 3);       // step 4 of pattern 1 (OG 75 only: 74 and 128 are not counted), step 3 of pattern 3 (76 and 100)
  CHECK(rep.storage_error == hs::Status::Ok);

  const o3os::model::Project& p = arena.project;
  NEED(p.pattern(1) != nullptr);
  CHECK(p.pattern(2) == nullptr);  // the damaged one left nothing behind
  const o3os::model::Step* s = p.pattern(1)->steps[0];
  CHECK(s[0].vol == 63);
  CHECK(s[1].vol == 127);
  CHECK(s[2].fx[0].type == 2);
  CHECK(s[2].fx[0].value == 50);
  CHECK(s[3].fx[0].type == 4);
  CHECK(s[3].fx[1].type == 5);  // the micromove was not stored
  CHECK(s[4].fx[0].type == 0);
  CHECK(s[4].fx[1].type == 96);
  CHECK(s[4].fx[1].value == 11);
  CHECK(s[5].fx[0].type == 127);
  CHECK(s[5].fx[1].type == 128);
  CHECK(s[5].fx[1].value == 200);
  NEED(p.pattern(3) != nullptr);
  const o3os::model::Step* t = p.pattern(3)->steps[0];
  CHECK(t[0].fx[0].type == 6);
  CHECK(t[0].fx[1].type == 7);  // the micromove was not stored
  CHECK(t[1].vol == 127);
  CHECK(t[2].vol == 127);
  CHECK(t[3].fx[0].type == 0);
  CHECK(t[3].fx[1].type == 0);

  // The strip names what the folder lost (the summary line the unit's diag sd and log show).
  char line[100];
  const std::size_t n = o3os::app::describe_load(l, line, sizeof line);
  CHECK(std::string(line, n) == "Workspace: v16, 2 patterns, 0 instruments, 1 failed, 2 MICRO-MOVES NOT KEPT, 6 VALUES CHANGED");
  CHECK(n == std::strlen(line));
}

TEST_CASE("og folder: the folder is a path, with spaces, either case, a slash at either end") {
  TempDir tmp;
  const std::vector<FolderFile> files = synthetic_folder();
  write_folder(tmp.path, "Projects/My Song", files);
  write_folder(tmp.path, "WORKSPACE", files);  // an upper-case folder name
  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  OgFolderLoader& l = loader();
  o3os::model::ModelArena& arena = o3os::test::og::arena(0);
  for (const char* folder : {"Projects/My Song", "/Projects/My Song/", "projects/my song", "Workspace", "workspace/"}) {
    arena.project.reset();
    NEED(l.begin(card, folder, arena));
    NEED(run_loader(l, 4096) == OgFolderLoader::State::Done);
    INFO(folder);
    CHECK(l.report().project_ok);
    CHECK(l.report().patterns == kFolderPatternsGood);
  }
  CHECK(std::string(l.folder()) == "workspace");  // the trailing slash is gone
  // Not a path: nothing starts.
  CHECK(!l.begin(card, "../x", arena));
  CHECK(!l.begin(card, "", arena));
  CHECK(!l.begin(card, "/", arena));
  CHECK(!l.begin(card, "a|b", arena));
  CHECK(!l.begin(card, nullptr, arena));
  const std::string too_long(OgFolderLoader::kFolderMax + 1, 'x');
  CHECK(!l.begin(card, too_long.c_str(), arena));
}

TEST_CASE("og folder: a folder without a project file, with a bad one, without patterns or instruments") {
  TempDir tmp;
  const std::vector<FolderFile> files = synthetic_folder();
  DirStorage card(tmp.path.string());
  OgFolderLoader& l = loader();
  o3os::model::ModelArena& arena = o3os::test::og::arena(0);

  // No such folder, or a folder without project.mt.
  fs::create_directories(tmp.path / "Empty");
  NEED(card.mount() == hs::Status::Ok);
  for (const char* folder : {"Nothing", "Empty"}) {
    arena.project.reset();
    NEED(l.begin(card, folder, arena));
    CHECK(run_loader(l, 4096) == OgFolderLoader::State::Failed);
    CHECK(std::strcmp(l.report().detail, "NO PROJECT FILE") == 0);
    char line[100];
    const std::size_t n = o3os::app::describe_load(l, line, sizeof line);
    CHECK(std::string(line, n) == std::string(folder) + ": FAILED NO PROJECT FILE");
    CHECK(l.report().storage_error == hs::Status::NotFound);
    CHECK(!l.report().project_ok);
    CHECK(card.open_files() == 0u);
  }

  // A project file that is not one (the magic is wrong): the reader's own error, and no patterns were looked for.
  Bytes bad_project = files[0].bytes;
  bad_project[0] = 'X';
  write_file(tmp.path, "Bad/project.mt", bad_project);
  write_file(tmp.path, "Bad/patterns/pattern_01.mtp", files[1].bytes);
  arena.project.reset();
  NEED(l.begin(card, "Bad", arena));
  CHECK(run_loader(l, 4096) == OgFolderLoader::State::Failed);
  CHECK(l.report().error == formats::FormatError::BadMagic);
  CHECK(!l.report().project_ok);
  CHECK(l.report().patterns == 0u);
  CHECK(arena.project.pattern(1) == nullptr);
  CHECK(card.open_files() == 0u);

  // A folder with only a project file (no patterns/, no instruments/): a project with no patterns, which is not an error.
  write_file(tmp.path, "Bare/project.mt", files[0].bytes);
  arena.project.reset();
  NEED(l.begin(card, "Bare", arena));
  CHECK(run_loader(l, 4096) == OgFolderLoader::State::Done);
  CHECK(l.report().project_ok);
  CHECK(l.report().patterns == 0u);
  CHECK(l.report().instruments == 0u);
  CHECK(l.report().error == formats::FormatError::None);
  CHECK(std::string(arena.project.name()) == "Synthetic Project");

  // A step that is not running says what it was.
  CHECK(l.step(4096) == OgFolderLoader::State::Done);
  char line[100];
  const std::size_t n = o3os::app::describe_load(l, line, sizeof line);
  CHECK(std::string(line, n) == "Bare: v16, 0 patterns, 0 instruments");
  l.abort();  // a finished load is not abandoned: abort() leaves Done as it was
  CHECK(l.state() == OgFolderLoader::State::Done);
}

TEST_CASE("og folder: abort closes everything, and a loader can be started again") {
  TempDir tmp;
  write_folder(tmp.path, "Workspace", synthetic_folder());
  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  OgFolderLoader& l = loader();
  o3os::model::ModelArena& arena = o3os::test::og::arena(0);
  arena.project.reset();
  NEED(l.begin(card, "Workspace", arena));
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(l.step(700) == OgFolderLoader::State::Running);
  }
  CHECK((card.open_files() + card.open_dirs()) > 0u);  // in the middle of a file or a directory
  l.abort();
  CHECK(l.state() == OgFolderLoader::State::Idle);
  CHECK(card.open_files() == 0u);
  CHECK(card.open_dirs() == 0u);
  CHECK(l.step(4096) == OgFolderLoader::State::Idle);  // nothing running: nothing done
  // Again from the start (begin() also abandons a load that is running).
  arena.project.reset();
  NEED(l.begin(card, "Workspace", arena));
  (void)l.step(700);
  NEED(l.begin(card, "Workspace", arena));
  CHECK(run_loader(l, 4096) == OgFolderLoader::State::Done);
  CHECK(l.report().patterns == kFolderPatternsGood);
  CHECK(card.open_files() == 0u);
  CHECK(card.open_dirs() == 0u);
}

namespace {

// A card that fails: forwards to a DirStorage and answers Io to every read after `budget` bytes.
class FlakyStorage final : public hs::Storage {
 public:
  FlakyStorage(DirStorage& inner, std::size_t budget) : inner_(inner), budget_(budget) {}
  hs::Status mount() noexcept override { return inner_.mount(); }
  bool mounted() const noexcept override { return inner_.mounted(); }
  bool card_present() const noexcept override { return inner_.card_present(); }
  hs::VolumeInfo volume() const noexcept override { return inner_.volume(); }
  hs::Status exists(const char* p, bool* d) noexcept override { return inner_.exists(p, d); }
  hs::Status open(const char* p, hs::FileHandle& o) noexcept override { return inner_.open(p, o); }
  hs::Status read(hs::FileHandle f, void* dst, std::size_t max, std::size_t& got) noexcept override {
    if (budget_ == 0) {
      return hs::Status::Io;
    }
    const std::size_t n = max < budget_ ? max : budget_;
    const hs::Status s = inner_.read(f, dst, n, got);
    budget_ -= got;
    return s;
  }
  hs::Status seek(hs::FileHandle f, std::uint32_t o) noexcept override { return inner_.seek(f, o); }
  hs::Status size(hs::FileHandle f, std::uint32_t& o) const noexcept override { return inner_.size(f, o); }
  void close(hs::FileHandle& f) noexcept override { inner_.close(f); }
  hs::Status opendir(const char* p, hs::DirHandle& o) noexcept override { return inner_.opendir(p, o); }
  hs::Status readdir(hs::DirHandle d, hs::DirEntry& e) noexcept override { return inner_.readdir(d, e); }
  void closedir(hs::DirHandle& d) noexcept override { inner_.closedir(d); }

 private:
  DirStorage& inner_;
  std::size_t budget_;
};

}  // namespace

TEST_CASE("og folder: a card that fails in the middle ends the load with the card's error and leaves nothing open") {
  TempDir tmp;
  write_folder(tmp.path, "Workspace", synthetic_folder());
  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  OgFolderLoader& l = loader();
  o3os::model::ModelArena& arena = o3os::test::og::arena(0);
  for (const std::size_t good_bytes : {std::size_t{0}, std::size_t{1000}, std::size_t{2320}, std::size_t{2320 + 9260 + 50}, std::size_t{40000}}) {
    FlakyStorage flaky(card, good_bytes);
    arena.project.reset();
    NEED(l.begin(flaky, "Workspace", arena));
    INFO(good_bytes);
    CHECK(run_loader(l, 4096) == OgFolderLoader::State::Failed);
    CHECK(l.report().storage_error == hs::Status::Io);
    CHECK(l.report().error == formats::FormatError::SourceFailed);
    CHECK(card.open_files() == 0u);
    CHECK(card.open_dirs() == 0u);
  }
}
