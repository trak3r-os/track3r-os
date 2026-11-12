// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/native_load.h (OS-007o): a native project folder of the card loaded into the document with the native reader, in slices: the project it makes is the one that was saved
// (a save of the loaded project is byte-for-byte the file), the backup form (project.o3pj.bak) loads the same way, and every way it can fail says why and leaves the document
// as the reader promises (untouched when there is no file, reset and valid when the file read badly).
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/mem_card.h"
#include "support/memory_io.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/dir_scan.h>
#include <o3os/app/native_load.h>
#include <o3os/formats/crc32.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace app = o3os::app;
namespace hs = o3os::hal::storage;
using o3os::test::MemCard;
using Bytes = MemCard::Bytes;
using State = app::NativeFolderLoader::State;
namespace nat = o3os::formats::native;

namespace {

Bytes native_file(unsigned arena_index) {
  o3os::model::ModelArena& a = o3os::test::arena(arena_index);
  o3os::test::fill_small(a.project);
  Bytes out;
  (void)o3os::test::save(a.project, out);
  return out;
}

// Steps to the end of the load (a bound of the test's own); returns the steps taken.
unsigned run(app::NativeFolderLoader& loader, std::size_t budget = 4096) {
  unsigned steps = 0;
  while (loader.step(budget) == State::Running) {
    if (++steps >= 100000u) {
      FAIL_CHECK("the load did not end");
      break;
    }
  }
  return steps;
}

Bytes saved(const o3os::model::Project& p) {
  Bytes out;
  (void)o3os::test::save(p, out);
  return out;
}

}  // namespace

TEST_CASE("native load: the names it reads are the names the save layer writes") {
  CHECK(std::string(o3os::hal::storage::save::kProjectFile) == "project.o3pj");
  CHECK(std::string(o3os::hal::storage::save::kBackupSuffix) == ".bak");
  CHECK(std::string(app::kProjectFilter.native_probe) == o3os::hal::storage::save::kProjectFile);
}

TEST_CASE("native load: a folder's project.o3pj loads in slices into the document, and saving it again gives the file back") {
  const Bytes file = native_file(0);
  MemCard card;
  card.put("Projects/Song/project.o3pj", file);
  o3os::model::ModelArena& target = o3os::test::arena(1);
  target.project.reset();
  app::NativeFolderLoader loader;
  CHECK(loader.state() == State::Idle);
  CHECK_FALSE(loader.active());
  NEED(loader.begin(card, "Projects/Song/", target));
  CHECK(loader.active());
  CHECK(loader.state() == State::Running);
  CHECK(std::string(loader.folder()) == "Projects/Song");
  const unsigned steps = run(loader, 512);
  CHECK(steps >= 3);  // in slices (a step reads at most about its budget)
  CHECK(loader.state() == State::Done);
  CHECK(loader.report().error == o3os::formats::native::Error::None);
  CHECK(loader.report().bytes_read == file.size());
  CHECK(loader.report().patterns == o3os::test::arena(0).project.pattern_count());
  CHECK(loader.report().instruments == o3os::test::arena(0).project.instrument_count());
  CHECK_FALSE(loader.report().backup);
  CHECK(saved(target.project) == file);
  char line[96];
  CHECK(app::describe_native_load(loader, line, sizeof line) > 0u);
  CHECK(std::string(line).find("Projects/Song: native, ") == 0u);
  loader.release();
  CHECK_FALSE(loader.active());
}

TEST_CASE("native load: the backup form reads project.o3pj.bak, and only that") {
  const Bytes old_file = native_file(0);
  MemCard card;
  card.put("Projects/Song/project.o3pj.bak", old_file);
  o3os::model::ModelArena& target = o3os::test::arena(1);
  target.project.reset();
  app::NativeFolderLoader loader;
  NEED(loader.begin(card, "Projects/Song", target, true));
  run(loader);
  CHECK(loader.state() == State::Done);
  CHECK(loader.report().backup);
  CHECK(saved(target.project) == old_file);
  char line[96];
  (void)app::describe_native_load(loader, line, sizeof line);
  CHECK(std::string(line).find(", from the backup") != std::string::npos);
  // The primary name is not there: the plain form says so and leaves the document alone.
  const Bytes before = saved(target.project);
  o3os::test::fill_small(target.project);
  const Bytes mark = saved(target.project);
  NEED(loader.begin(card, "Projects/Song", target, false));
  run(loader);
  CHECK(loader.state() == State::Failed);
  CHECK(std::string(loader.report().detail) == "NO PROJECT FILE");
  CHECK(saved(target.project) == mark);
  (void)before;
}

TEST_CASE("native load: a folder with no file, a card that will not open it, a bad path") {
  MemCard card;
  card.make_dir_now("Projects");
  card.make_dir_now("Projects/Empty");
  o3os::model::ModelArena& target = o3os::test::arena(1);
  o3os::test::fill_small(target.project);
  const Bytes mark = saved(target.project);
  app::NativeFolderLoader loader;
  NEED(loader.begin(card, "Projects/Empty", target));
  run(loader);
  CHECK(loader.state() == State::Failed);
  CHECK(std::string(loader.report().detail) == "NO PROJECT FILE");
  CHECK(saved(target.project) == mark);  // nothing was touched
  card.put("Projects/Empty/project.o3pj", native_file(0));
  o3os::test::fill_small(target.project);
  card.open_status = hs::Status::NoHandle;
  NEED(loader.begin(card, "Projects/Empty", target));
  run(loader);
  CHECK(loader.state() == State::Failed);
  CHECK(loader.report().storage_error == hs::Status::NoHandle);
  CHECK(saved(target.project) == mark);
  card.open_status = hs::Status::Ok;
  CHECK_FALSE(loader.begin(card, "", target));
  CHECK_FALSE(loader.begin(card, "a/../b", target));
  CHECK_FALSE(loader.begin(card, nullptr, target));
  CHECK_FALSE(loader.begin(card, std::string(520, 'x').c_str(), target));
  CHECK_FALSE(loader.begin(card, ("Projects/" + std::string(250, 'x')).c_str(), target));  // a folder path over 255 bytes
}

TEST_CASE("native load: a damaged file fails with the reader's words and leaves a valid, empty project") {
  Bytes file = native_file(0);
  file[file.size() / 2] ^= 0xFF;  // inside the chunks: the CRC no longer matches
  MemCard card;
  card.put("Projects/Song/project.o3pj", file);
  o3os::model::ModelArena& target = o3os::test::arena(1);
  o3os::test::fill_small(target.project);
  app::NativeFolderLoader loader;
  NEED(loader.begin(card, "Projects/Song", target));
  run(loader);
  CHECK(loader.state() == State::Failed);
  CHECK(loader.report().error != o3os::formats::native::Error::None);
  CHECK(loader.report().detail[0] != '\0');
  CHECK(target.project.check_invariants());
  CHECK(o3os::test::is_default_empty(target.project));
  char line[96];
  (void)app::describe_native_load(loader, line, sizeof line);
  CHECK(std::string(line).find("Projects/Song: FAILED ") == 0u);
  // A file cut short, and one that is not a project at all.
  card.put("Projects/Song/project.o3pj", Bytes(file.begin(), file.begin() + 40));
  NEED(loader.begin(card, "Projects/Song", target));
  run(loader);
  CHECK(loader.state() == State::Failed);
  card.put("Projects/Song/project.o3pj", std::string("this is not a project"));
  NEED(loader.begin(card, "Projects/Song", target));
  run(loader);
  CHECK(loader.state() == State::Failed);
  CHECK(loader.report().error == o3os::formats::native::Error::BadMagic);
}

TEST_CASE("native load: the fresh form resets the document to the model's defaults in one step, with no folder") {
  o3os::model::ModelArena& target = o3os::test::arena(1);
  o3os::test::fill_small(target.project);
  app::NativeFolderLoader loader;
  loader.begin_fresh(target);
  CHECK(loader.active());
  CHECK(loader.fresh());
  CHECK(loader.state() == State::Running);
  CHECK(loader.step(4096) == State::Done);
  CHECK(std::string(loader.folder()).empty());
  const o3os::model::Project& p = target.project;
  CHECK(p.check_invariants());
  CHECK(p.pattern_count() == 1u);
  CHECK(p.pattern_length(1) == o3os::model::kDefaultPatternLength);
  CHECK(p.song_length() == 1u);
  CHECK(p.order(0) == 1u);
  CHECK(p.active_tracks() == 12u);  // ADR 0003
  CHECK(p.instrument_count() == 0u);
  CHECK(p.module_count() == 0u);
  CHECK(p.note().scale == 2u);  // OS-DEFSCALE: a new project starts in C major, the NOTE part present
  CHECK(p.note().root == 0u);
  CHECK((p.state_present() & o3os::model::Project::kStateNote) != 0);
  char line[32];
  (void)app::describe_native_load(loader, line, sizeof line);
  CHECK(std::string(line) == "new project");
  // A file load after it is not fresh any more.
  MemCard card;
  card.put("Projects/Song/project.o3pj", native_file(0));
  NEED(loader.begin(card, "Projects/Song", target));
  CHECK_FALSE(loader.fresh());
  run(loader);
  CHECK(loader.state() == State::Done);
}

// ---- OS-QAFIX-4 (finding 4): the verify phase of an open --------------------------------------------------------------------------------------------------------------------------------------

namespace {

// The verdict of the Verifier over `bytes`, in 700-byte pieces (a read ends inside chunks).
nat::Progress verify(const Bytes& bytes, nat::Report* report = nullptr) {
  o3os::test::MemorySource src(bytes);
  src.max_per_read = 700;
  nat::Verifier v;
  v.begin(src);
  unsigned steps = 0;
  while (v.step(300) == nat::Progress::Running) {
    if (++steps > 100000u) {
      FAIL_CHECK("the verify did not end");
      break;
    }
  }
  if (report != nullptr) {
    *report = v.report();
  }
  return v.progress();
}

// The file's CRC chunk (the last 14 bytes: header 8, version 2, CRC 4) made right again after an edit of the bytes before it.
void fix_crc(Bytes& file) {
  const std::size_t n = file.size();
  const std::uint32_t crc = o3os::formats::crc32(file.data(), n - 14u);
  for (unsigned i = 0; i < 4; ++i) {
    file[n - 4u + i] = static_cast<std::uint8_t>(crc >> (24u - 8u * i));  // (big-endian, like every field of the format)
  }
}

// The Reader's verdict on the same bytes (the arena is clobbered: that is the point of the verify).
nat::Progress read_all(const Bytes& bytes, nat::Report& report) {
  return o3os::test::load(o3os::test::arena(2), bytes, &report);
}

bool framing_error(nat::Error e) {
  return e == nat::Error::Truncated || e == nat::Error::BadMagic || e == nat::Error::BadCrc || e == nat::Error::NewerMajor;
}

}  // namespace

TEST_CASE("verify: a file the Reader loads is accepted, whole and in pieces, and the counts are the Reader's") {
  Bytes files[3];
  o3os::test::fill_small(o3os::test::arena(0).project);
  (void)o3os::test::save(o3os::test::arena(0).project, files[0]);
  o3os::test::fill_persist2(o3os::test::arena(0).project);
  (void)o3os::test::save(o3os::test::arena(0).project, files[1]);
  o3os::test::fill_maximal(o3os::test::arena(0).project);
  (void)o3os::test::save(o3os::test::arena(0).project, files[2]);
  for (const Bytes& f : files) {
    nat::Report rv;
    nat::Report rr;
    CHECK(verify(f, &rv) == nat::Progress::Done);
    CHECK(read_all(f, rr) == nat::Progress::Done);
    CHECK(rv.patterns == rr.patterns);
    CHECK(rv.instruments == rr.instruments);
    CHECK(rv.bytes == f.size());
  }
  // What the Reader ignores, the verify ignores: bytes after the FORM.
  Bytes tail = files[0];
  tail.push_back(0x55);
  tail.push_back(0x66);
  CHECK(verify(tail) == nat::Progress::Done);
}

TEST_CASE("verify: the file that cannot load is refused with the Reader's words, and never where the Reader accepts") {
  o3os::test::fill_small(o3os::test::arena(0).project);
  Bytes file;
  (void)o3os::test::save(o3os::test::arena(0).project, file);
  nat::Report rv;
  // every cut of the first and last 64 bytes, and one in 37 of the rest: Truncated (or a framing error the Reader meets first), never accepted
  for (std::size_t cut = 0; cut < file.size(); cut += (cut < 64u || cut + 64u > file.size()) ? 1u : 37u) {
    const Bytes part(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(cut));
    CHECK_MESSAGE(verify(part, &rv) == nat::Progress::Failed, "cut at " << cut);
  }
  CHECK(verify(Bytes(file.begin(), file.end() - 1), &rv) == nat::Progress::Failed);
  CHECK(std::string(rv.detail) == "the file ends before its FORM does");
  CHECK(rv.error == nat::Error::Truncated);
  // a bit flipped: the Reader and the verify agree on what is a framing error (and the verify never refuses what the Reader takes)
  unsigned flipped = 0;
  for (std::size_t at = 0; at < file.size(); at += (at < 80u || at + 40u > file.size()) ? 1u : 23u) {
    Bytes bad = file;
    bad[at] ^= 0x04;
    nat::Report rr;
    const nat::Progress pr = read_all(bad, rr);
    const nat::Progress pv = verify(bad, &rv);
    ++flipped;
    if (pr == nat::Progress::Done) {
      CHECK_MESSAGE(pv == nat::Progress::Done, "flip at " << at);
    }
    if (pv == nat::Progress::Done && pr == nat::Progress::Failed) {
      CHECK_MESSAGE(!framing_error(rr.error), "flip at " << at << ": the Reader says " << nat::error_name(rr.error) << " and the verify accepted");
    }
    if (pr == nat::Progress::Failed && framing_error(rr.error)) {
      CHECK_MESSAGE(pv == nat::Progress::Failed, "flip at " << at << ": the Reader says " << nat::error_name(rr.error));
    }
  }
  CHECK(flipped > 100u);
  // the same edits with the CRC made right: only what is not framing is left to the Reader (a valid-CRC file the verify takes may still fail the Reader's content checks), and what the Reader loads is taken
  for (std::size_t at = 0; at + 14u < file.size(); at += (at < 80u) ? 1u : 29u) {
    Bytes bad = file;
    bad[at] ^= 0x81;
    fix_crc(bad);
    nat::Report rr;
    const nat::Progress pr = read_all(bad, rr);
    const nat::Progress pv = verify(bad, &rv);
    if (pr == nat::Progress::Done) {
      CHECK_MESSAGE(pv == nat::Progress::Done, "edit at " << at);
    }
    if (pv == nat::Progress::Done && pr == nat::Progress::Failed) {
      CHECK_MESSAGE(!framing_error(rr.error), "edit at " << at << ": the Reader says " << nat::error_name(rr.error) << " and the verify accepted");
      const bool header_limit = rr.error == nat::Error::LimitExceeded && std::string(rr.detail).find("header announces") != std::string::npos;
      CHECK_MESSAGE(!header_limit, "edit at " << at << ": the header announces more than the model holds and the verify accepted");
    }
  }
  // a chunk of a newer major version, with a good CRC: a project from a newer OS is refused before it empties the document
  Bytes newer = file;
  newer[12 + 8] = 0x02;  // PHDR's version (big-endian), high byte: 2.x
  fix_crc(newer);
  CHECK(verify(newer, &rv) == nat::Progress::Failed);
  CHECK(rv.error == nat::Error::NewerMajor);
  // the PHDR announces one pattern more than the file carries, with a good CRC: refused (the counts are checked against the chunks)
  Bytes counts = file;
  counts[12 + 8 + 14 + 1] = static_cast<std::uint8_t>(counts[12 + 8 + 14 + 1] + 1u);  // PHDR payload: version 2, tracks, length, tempo 4, swing, master 5, then the pattern count (u16)
  fix_crc(counts);
  CHECK(verify(counts, &rv) == nat::Progress::Failed);
  CHECK(rv.error == nat::Error::Malformed);
  CHECK(std::string(rv.detail) == "chunk counts differ from the header");
  // no stricter than the Reader on framing it takes: an odd-length chunk of an unknown id with its pad byte, and a CRC chunk of a newer minor longer than 6 bytes (with its pad byte, its FORM length
  // and its CRC made right), are both loaded by the Reader and accepted by the verify
  {
    const std::size_t body = file.size() - 14u;  // up to the CRC chunk
    Bytes odd(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(body));
    const std::uint8_t unknown[12] = {'Z', 'Z', 'Z', 'Z', 0, 0, 0, 3, 1, 2, 3, 0};  // id, length 3, three bytes, one pad
    odd.insert(odd.end(), unknown, unknown + sizeof unknown);
    const std::uint8_t crc_head[8] = {file[body], file[body + 1], file[body + 2], file[body + 3], 0, 0, 0, 9};  // the CRC chunk's id, length 9 = 6 + 3 appended bytes
    odd.insert(odd.end(), crc_head, crc_head + sizeof crc_head);
    const std::uint8_t crc_version[2] = {0x01, 0x01};  // 1.1: newer than the reader knows
    odd.insert(odd.end(), crc_version, crc_version + 2);
    odd.insert(odd.end(), 4, 0);                        // (the CRC, below)
    const std::uint8_t appended[4] = {7, 8, 9, 0};      // three bytes and the pad
    odd.insert(odd.end(), appended, appended + sizeof appended);
    const std::uint32_t form_len = static_cast<std::uint32_t>(odd.size() - 8u);
    for (unsigned i = 0; i < 4; ++i) {
      odd[4 + i] = static_cast<std::uint8_t>(form_len >> (24u - 8u * i));
    }
    const std::uint32_t crc = o3os::formats::crc32(odd.data(), body + sizeof unknown);
    for (unsigned i = 0; i < 4; ++i) {
      odd[body + sizeof unknown + 10u + i] = static_cast<std::uint8_t>(crc >> (24u - 8u * i));
    }
    nat::Report rr;
    CHECK(read_all(odd, rr) == nat::Progress::Done);
    CHECK(verify(odd, &rv) == nat::Progress::Done);
  }
  // not a project at all
  CHECK(verify(Bytes(300, 0x5A), &rv) == nat::Progress::Failed);
  CHECK(rv.error == nat::Error::BadMagic);
  // a card that fails is the source's failure, not the file's
  o3os::test::MemorySource src(file);
  src.fail_at = 100;
  nat::Verifier v;
  v.begin(src);
  while (v.step(4096) == nat::Progress::Running) {
  }
  CHECK(v.report().error == nat::Error::SourceFailed);
}

TEST_CASE("verify: ProjectVerify checks the files of a folder before anything is replaced, and never touches an arena") {
  const Bytes good = native_file(0);
  MemCard card;
  card.put("Projects/Good/project.o3pj", good);
  Bytes cut(good.begin(), good.end() - 20);
  card.put("Projects/Cut/project.o3pj", cut);
  card.put("Projects/Bak/project.o3pj.bak", good);
  card.make_dir_now("Projects/Empty");
  app::ProjectVerify v;
  auto run_verify = [&](const char* folder, bool og, bool backup) {
    CHECK(v.begin(card, folder, og, backup));
    unsigned steps = 0;
    while (v.step(600) == app::ProjectVerify::State::Running && ++steps < 10000u) {
    }
    return v.state();
  };
  CHECK(run_verify("Projects/Good", false, false) == app::ProjectVerify::State::Ok);
  CHECK(std::string(v.folder()) == "Projects/Good");
  CHECK_FALSE(v.og());
  CHECK(run_verify("Projects/Cut", false, false) == app::ProjectVerify::State::Refused);
  CHECK(std::string(v.detail()) == "THE FILE IS CUT SHORT");
  CHECK(run_verify("Projects/Empty", false, false) == app::ProjectVerify::State::Refused);
  CHECK(std::string(v.detail()) == "NO PROJECT FILE");
  CHECK(run_verify("Projects/Bak", false, false) == app::ProjectVerify::State::Refused);  // (the plain form reads the primary name only)
  CHECK(run_verify("Projects/Bak", false, true) == app::ProjectVerify::State::Ok);
  CHECK(v.backup());
  card.open_status = hs::Status::NoHandle;
  CHECK(run_verify("Projects/Good", false, false) == app::ProjectVerify::State::Refused);
  card.open_status = hs::Status::Ok;
  CHECK_FALSE(v.begin(card, "", false, false));
  CHECK_FALSE(v.begin(card, "a/../b", false, false));
  CHECK_FALSE(v.begin(card, std::string(300, 'x').c_str(), false, false));
  // an OG folder: project.mt of the right size and header is taken, a stub of another size is not
  Bytes og(2324, 0);
  og[0] = 'M';
  og[1] = 'T';
  og[8] = 17;
  card.put("Projects/Og/project.mt", og);
  card.put("Projects/OgShort/project.mt", Bytes(100, 0));
  Bytes wrong = og;
  wrong.resize(2000);
  card.put("Projects/OgWrongLen/project.mt", wrong);
  // an older OG version has its own length (the Reader's, formats/src/og_codec.h: v6 is 1284 bytes): its own length is accepted, another version's is refused
  Bytes og6(1284, 0);
  og6[0] = 'M';
  og6[1] = 'T';
  og6[8] = 6;
  card.put("Projects/Og6/project.mt", og6);
  Bytes og6_long = og6;
  og6_long.resize(2324);
  card.put("Projects/Og6Long/project.mt", og6_long);
  CHECK(run_verify("Projects/Og", true, false) == app::ProjectVerify::State::Ok);
  CHECK(run_verify("Projects/Og6", true, false) == app::ProjectVerify::State::Ok);
  CHECK(run_verify("Projects/Og6Long", true, false) == app::ProjectVerify::State::Refused);
  CHECK(run_verify("Projects/OgShort", true, false) == app::ProjectVerify::State::Refused);
  CHECK(run_verify("Projects/OgWrongLen", true, false) == app::ProjectVerify::State::Refused);
  CHECK(run_verify("Projects/Empty", true, false) == app::ProjectVerify::State::Refused);
}

// ---- OS-MIDIREC-P1 S2: the MIDI take lanes of the document ---------------------------------------------------------------------------------------------------------------------------------------

namespace {

unsigned first_pattern_id(const o3os::model::Project& p) {
  for (unsigned id = 1; id <= o3os::model::kMaxPatternId; ++id) {
    if (p.pattern(id) != nullptr) return id;
  }
  return 0;
}

o3os::model::LaneEvent lane_note(unsigned tick, unsigned note) {
  o3os::model::LaneEvent e{};
  e.tick = static_cast<std::uint16_t>(tick);
  e.len = 12;
  e.a = static_cast<std::uint8_t>(note);
  e.b = 100;
  return e;
}

}  // namespace

TEST_CASE("native load: the lanes of a file load into the attached store, a file with none empties it, a stale store is cleared") {
  o3os::model::ModelArena& src = o3os::test::arena(0);
  o3os::test::fill_small(src.project);
  const unsigned id = first_pattern_id(src.project);
  NEED(id != 0);
  static o3os::model::LaneStore from;
  static o3os::model::LaneStore doc;
  from.clear();
  from.adopt(src.project.load_epoch());
  o3os::model::LaneEvent* g = from.open_gap(id, 2, 0, 3);
  NEED(g != nullptr);
  g[0] = lane_note(0, 60);
  g[1] = lane_note(100, 62);
  g[2] = lane_note(400, 64);
  Bytes with_lanes;
  NEED(o3os::test::save(src.project, with_lanes, o3os::formats::native::kDefaultStepBudget, &from) == nat::Progress::Done);
  const Bytes without = native_file(0);
  CHECK(with_lanes.size() > without.size());

  MemCard card;
  card.put("Projects/Lanes/project.o3pj", with_lanes);
  card.put("Projects/Plain/project.o3pj", without);
  o3os::model::ModelArena& target = o3os::test::arena(1);
  target.project.reset();
  app::NativeFolderLoader loader;
  loader.attach_lanes(&doc);
  doc.clear();

  // the file's lanes arrive, in the store's epoch of the loaded project, and a save of the document gives the file back
  NEED(loader.begin(card, "Projects/Lanes", target));
  run(loader);
  CHECK(loader.state() == State::Done);
  CHECK(loader.report().mevt_skipped == 0u);
  CHECK(doc.in_sync(target.project.load_epoch()));
  CHECK(doc.lane_count() == 1u);
  const o3os::model::LaneStore::View v = doc.lane(id, 2);
  NEED(v.count == 3u);
  CHECK(std::memcmp(v.events, g, 3 * sizeof(o3os::model::LaneEvent)) == 0);
  Bytes again;
  CHECK(o3os::test::save(target.project, again, o3os::formats::native::kDefaultStepBudget, &doc) == nat::Progress::Done);
  CHECK(again == with_lanes);

  // a file with no MEVT chunk leaves the document with no lane (the lanes of the first one do not stay)
  NEED(loader.begin(card, "Projects/Plain", target));
  run(loader);
  CHECK(loader.state() == State::Done);
  CHECK(doc.total_events() == 0u);
  CHECK(doc.in_sync(target.project.load_epoch()));

  // the fresh form (NEW) drops lanes at once, and the store follows the project's new epoch
  doc.adopt(target.project.load_epoch());
  o3os::model::LaneEvent* h = doc.open_gap(id, 0, 0, 1);
  NEED(h != nullptr);
  h[0] = lane_note(1, 1);
  loader.begin_fresh(target);
  CHECK(loader.step(4096) == State::Done);
  CHECK(doc.total_events() == 0u);
  CHECK(doc.in_sync(target.project.load_epoch()));
}

TEST_CASE("native load: a lane the reader skips is counted in the report and named by describe_native_load") {
  o3os::model::ModelArena& src = o3os::test::arena(0);
  o3os::test::fill_small(src.project);
  static o3os::model::LaneStore from;
  static o3os::model::LaneStore doc;
  from.clear();
  from.adopt(src.project.load_epoch());
  const unsigned id = first_pattern_id(src.project);
  NEED(id != 0);
  o3os::model::LaneEvent* g = from.open_gap(id, 0, 0, 1);
  NEED(g != nullptr);
  g[0] = lane_note(0, 60);
  Bytes file;
  NEED(o3os::test::save(src.project, file, o3os::formats::native::kDefaultStepBudget, &from) == nat::Progress::Done);
  // the MEVT chunk with its one lane moved to pattern 0, which no project has
  std::vector<o3os::test::RawChunk> chunks = o3os::test::split_chunks(file);
  bool found = false;
  for (o3os::test::RawChunk& c : chunks) {
    if (c.id == nat::kChunkMevt) {
      c.payload[4] = 0;  // the first lane's pattern byte (after the version and the lane count)
      found = true;
    }
  }
  NEED(found);
  MemCard card;
  card.put("Projects/Skip/project.o3pj", o3os::test::build_file(chunks));
  o3os::model::ModelArena& target = o3os::test::arena(1);
  target.project.reset();
  app::NativeFolderLoader loader;
  loader.attach_lanes(&doc);
  NEED(loader.begin(card, "Projects/Skip", target));
  run(loader);
  CHECK(loader.state() == State::Done);
  CHECK(loader.report().mevt_skipped == 1u);
  CHECK(doc.lane_count() == 0u);
  char line[128];
  CHECK(app::describe_native_load(loader, line, sizeof line) > 0u);
  CHECK(std::string(line).find("1 MIDI lane skipped") != std::string::npos);
}

// OS-MIDIREC-P1 S2 fold: the reader keeps a write window open over the lane it fills; a load that is dropped part way (a new begin, begin_fresh, abort) must shut it, or the render skips every lane for good.
TEST_CASE("native load: a load dropped part way through a lane leaves the store's write window shut; clear and sync shut a leaked one") {
  o3os::model::ModelArena& src = o3os::test::arena(0);
  o3os::test::fill_small(src.project);
  const unsigned id = first_pattern_id(src.project);
  NEED(id != 0);
  static o3os::model::LaneStore from;
  static o3os::model::LaneStore doc;
  from.clear();
  from.adopt(src.project.load_epoch());
  o3os::model::LaneEvent* g = from.open_gap(id, 2, 0, 1000);  // a lane of 8 KB: many 512-byte slices
  NEED(g != nullptr);
  for (unsigned i = 0; i < 1000; ++i) {
    g[i] = lane_note(i * 3u / 4u, 40u + i % 60u);
  }
  Bytes file;
  NEED(o3os::test::save(src.project, file, o3os::formats::native::kDefaultStepBudget, &from) == nat::Progress::Done);
  MemCard card;
  card.put("Projects/Long/project.o3pj", file);
  o3os::model::ModelArena& target = o3os::test::arena(1);
  target.project.reset();
  app::NativeFolderLoader loader;
  loader.attach_lanes(&doc);
  doc.clear();

  for (int how = 0; how < 3; ++how) {  // abort, a new begin, begin_fresh
    NEED(loader.begin(card, "Projects/Long", target));
    bool open = false;
    for (int i = 0; i < 200 && loader.state() == State::Running && !open; ++i) {
      (void)loader.step(512);
      open = (doc.seq() & 1u) != 0u;
    }
    NEED(open);  // part way through the lane the window is open
    if (how == 0) {
      loader.abort();
    } else if (how == 1) {
      NEED(loader.begin(card, "Projects/Long", target));
    } else {
      loader.begin_fresh(target);
    }
    CHECK((doc.seq() & 1u) == 0u);
    loader.abort();
  }

  // a window leaked by any other writer ends with the next clear, and with a sync that moves the epoch
  doc.begin_write();
  doc.begin_write();
  CHECK((doc.seq() & 1u) == 1u);
  doc.clear();
  CHECK((doc.seq() & 1u) == 0u);
  doc.adopt(target.project.load_epoch());
  doc.begin_write();
  doc.sync(target.project.load_epoch() + 1u);
  CHECK((doc.seq() & 1u) == 0u);
  doc.begin_write();  // the normal nesting still works after it
  doc.end_write();
  CHECK((doc.seq() & 1u) == 0u);
}
