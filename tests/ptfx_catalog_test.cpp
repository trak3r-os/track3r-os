// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S1: the catalogue scan (module/ptfx_catalog.h) over an in-memory card (tests/support/mem_card.h): the cases of
// docs/specs/OS-CEPTFX.md line 381 — 0, 1, 128 and 129 files; a case-insensitive extension; long names skipped and counted; a damaged file listed
// with its status; the sort order; a card error retried at the next scan. Host only.
#include <doctest/doctest.h>

#include <o3os/module/ptfx_catalog.h>

#include "support/mem_card.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os::module;

namespace {

void put16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t x) {
  v[at] = static_cast<std::uint8_t>(x);
  v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
  put16(v, at, static_cast<std::uint16_t>(x));
  put16(v, at + 2, static_cast<std::uint16_t>(x >> 16));
}

// A good file: header + 64 bytes of payload, entries 0, 8 and 16, no parameters.
std::vector<std::uint8_t> make_ptfx(const char* display_name, std::uint16_t flags = 0) {
  std::vector<std::uint8_t> v(kPtfxHeaderSize + 64, 0);
  put32(v, 0, kPtfxMagic);
  put16(v, 4, 1);
  put16(v, 6, flags);
  std::memcpy(&v[8], display_name, std::strlen(display_name));
  put32(v, 64, kPtfxHeaderSize);
  put32(v, 68, 64);
  put32(v, 72, 0);
  put32(v, 76, 0);
  put32(v, 80, 8);
  put32(v, 84, 16);
  return v;
}

// Steps the scan to its end (8 units a step, as a 2 ms pass would give it).
void run_to_end(PtfxCatalog& cat) {
  for (unsigned guard = 0; guard < 100000u && cat.step(2000) == o3os::core::JobStatus::Running; ++guard) {
  }
}

struct Rig {
  o3os::test::MemCard card;
  PtfxEntry entries[kPtfxCatalogMax];
  PtfxCatalog cat{entries};

  Rig() { card.make_dir_now("Effects"); }
  void add(const char* name, const std::vector<std::uint8_t>& bytes) { card.put((std::string("Effects/") + name).c_str(), bytes); }
  void add_good(const char* file_name, const char* display_name, std::uint16_t flags = 0) { add(file_name, make_ptfx(display_name, flags)); }
};

}  // namespace

TEST_CASE("ptfx_catalog: an entry is 88 bytes, the catalogue 11 KiB for 128 files") {
  CHECK(sizeof(PtfxEntry) == 88);
  CHECK(kPtfxCatalogBytes == 128u * 88u);
  CHECK(kPtfxCatalogBytes <= 16u * 1024u);  // it fits the `ptfx` region's 16 KiB at offset 720 KiB (3.2)
}

TEST_CASE("ptfx_catalog: 0 files") {
  Rig r;
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 0);
  CHECK(r.cat.progress() == o3os::core::kJobProgressMax);
  CHECK_FALSE(r.cat.truncated());
  CHECK(r.cat.error() == o3os::hal::storage::Status::Ok);
  CHECK(std::strcmp(r.cat.name(), "PTFX SCAN") == 0);
}

TEST_CASE("ptfx_catalog: 1 file, one directory page or one header read a step, with progress") {
  Rig r;
  r.add_good("gain.ptfx", "Gain", kPtfxFlagStereo);
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  // One unit of budget: the first step opens the directory and reads one entry, header included.
  CHECK(r.cat.step(PtfxCatalog::kUnitUs) == o3os::core::JobStatus::Running);
  CHECK(r.cat.state() == PtfxCatalog::State::Scanning);
  CHECK(r.cat.count() == 1);
  const unsigned p1 = r.cat.progress();
  CHECK(p1 < o3os::core::kJobProgressMax);
  // The next step reads the directory's end.
  CHECK(r.cat.step(PtfxCatalog::kUnitUs) == o3os::core::JobStatus::Done);
  CHECK(r.cat.progress() == o3os::core::kJobProgressMax);
  CHECK(r.cat.count() == 1);
  const PtfxEntry& e = r.cat.entry(0);
  CHECK(std::strcmp(e.file_name, "gain.ptfx") == 0);
  CHECK(std::strcmp(e.display_name, "Gain") == 0);
  CHECK(e.status == PtfxStatus::Ok);
  CHECK(e.flags == kPtfxFlagStereo);
  CHECK(e.id == ptfx_plugin_id("gain.ptfx"));
  CHECK((e.id & 0x8000u) != 0);
  CHECK_FALSE(e.quarantined);
  CHECK(r.cat.quarantine(e.id));
  CHECK(r.cat.entry(0).quarantined);
  CHECK_FALSE(r.cat.quarantine(0x8000u));  // no such entry
  CHECK(r.card.open_handles == 0);          // the header's file was closed
}

TEST_CASE("ptfx_catalog: 128 files, capped and sorted") {
  Rig r;
  for (unsigned i = 0; i < 128; ++i) {
    char name[32], display[32];
    std::snprintf(name, sizeof name, "p%03u.ptfx", i);
    std::snprintf(display, sizeof display, "P%03u", i);
    r.add_good(name, display);
  }
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 128);
  CHECK_FALSE(r.cat.truncated());
  for (unsigned i = 0; i < 128; ++i) {
    char display[32];
    std::snprintf(display, sizeof display, "P%03u", i);
    CHECK(std::strcmp(r.cat.entry(i).display_name, display) == 0);
  }
}

TEST_CASE("ptfx_catalog: 129 files, the one that sorts last is dropped") {
  Rig r;
  for (unsigned i = 0; i < 129; ++i) {
    char name[32], display[32];
    std::snprintf(name, sizeof name, "p%03u.ptfx", i);
    std::snprintf(display, sizeof display, "P%03u", i);
    r.add_good(name, display);
  }
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 128);
  CHECK(r.cat.truncated());
  for (unsigned i = 0; i < 128; ++i) {
    char display[32];
    std::snprintf(display, sizeof display, "P%03u", i);
    CHECK(std::strcmp(r.cat.entry(i).display_name, display) == 0);
  }
  // P128 sorted after the last kept: it is the dropped one, whatever order the directory gave.
  for (unsigned i = 0; i < r.cat.count(); ++i) {
    CHECK(std::strcmp(r.cat.entry(i).display_name, "P128") != 0);
  }
}

TEST_CASE("ptfx_catalog: 129 files, the one that sorts first displaces the last kept and is counted") {
  Rig r;
  for (unsigned i = 0; i < 128; ++i) {
    char name[32], display[32];
    std::snprintf(name, sizeof name, "p%03u.ptfx", i);
    std::snprintf(display, sizeof display, "P%03u", i);
    r.add_good(name, display);
  }
  r.add_good("zzz.ptfx", "A000");  // read last (the card lists by name), sorts first: it displaces P127
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 128);
  CHECK(r.cat.truncated());  // the displaced entry is counted too: no file is lost silently
  CHECK(std::strcmp(r.cat.entry(0).display_name, "A000") == 0);
  CHECK(std::strcmp(r.cat.entry(1).display_name, "P000") == 0);
  CHECK(std::strcmp(r.cat.entry(127).display_name, "P126") == 0);
  for (unsigned i = 0; i < r.cat.count(); ++i) {
    CHECK(std::strcmp(r.cat.entry(i).display_name, "P127") != 0);
  }
}

TEST_CASE("ptfx_catalog: one step reads at most one header") {
  Rig r;
  r.add_good("a.ptfx", "A");
  r.add_good("b.ptfx", "B");
  r.add_good("c.ptfx", "C");
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  // A full-budget step (32 units) still reads only one header: the read ends the step.
  CHECK(r.cat.step(PtfxCatalog::kUnitUs * PtfxCatalog::kMaxUnitsPerStep) == o3os::core::JobStatus::Running);
  CHECK(r.cat.count() == 1);
  CHECK(r.cat.step(PtfxCatalog::kUnitUs * PtfxCatalog::kMaxUnitsPerStep) == o3os::core::JobStatus::Running);
  CHECK(r.cat.count() == 2);
  CHECK(r.cat.step(PtfxCatalog::kUnitUs * PtfxCatalog::kMaxUnitsPerStep) == o3os::core::JobStatus::Running);
  CHECK(r.cat.count() == 3);
  CHECK(r.cat.step(PtfxCatalog::kUnitUs * PtfxCatalog::kMaxUnitsPerStep) == o3os::core::JobStatus::Done);
  CHECK(r.cat.count() == 3);
  CHECK(std::strcmp(r.cat.entry(0).display_name, "A") == 0);
  CHECK(std::strcmp(r.cat.entry(2).display_name, "C") == 0);
}

TEST_CASE("ptfx_catalog: the extension is case-insensitive, other files are skipped") {
  Rig r;
  r.add_good("a.PTFX", "A");
  r.add_good("b.pTfX", "B");
  r.add_good("c.ptfx", "C");
  r.add("d.txt", make_ptfx("D"));
  r.add("e.ptfx2", make_ptfx("E"));
  r.add(".ptfx", make_ptfx("F"));  // no stem before the extension
  r.card.make_dir_now("Effects/x.ptfx");  // a folder called *.ptfx is a folder
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 3);
  CHECK(std::strcmp(r.cat.entry(0).display_name, "A") == 0);
  CHECK(std::strcmp(r.cat.entry(1).display_name, "B") == 0);
  CHECK(std::strcmp(r.cat.entry(2).display_name, "C") == 0);
  CHECK(r.cat.skipped_other() == 4);  // d.txt, e.ptfx2, ".ptfx", the folder
  CHECK(r.cat.skipped_long() == 0);
  CHECK(PtfxCatalog::is_ptfx_name("x.PTFX"));
  CHECK(PtfxCatalog::is_ptfx_name("x.ptfx"));
  CHECK_FALSE(PtfxCatalog::is_ptfx_name(".ptfx"));
  CHECK_FALSE(PtfxCatalog::is_ptfx_name("x.ptfx2"));
  CHECK_FALSE(PtfxCatalog::is_ptfx_name("ptfx"));
}

TEST_CASE("ptfx_catalog: a file name over 47 characters is skipped and counted") {
  Rig r;
  const std::string kept_name = std::string(42, 'k') + ".ptfx";    // 47 characters
  const std::string long_name = std::string(43, 'l') + ".ptfx";    // 48 characters
  r.add_good(kept_name.c_str(), "Kept");
  r.add_good(long_name.c_str(), "Long");
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 1);
  CHECK(r.cat.skipped_long() == 1);
  CHECK(std::strcmp(r.cat.entry(0).file_name, kept_name.c_str()) == 0);
  CHECK(r.cat.entry(0).file_name[kPtfxFileNameMax] == '\0');
}

TEST_CASE("ptfx_catalog: a damaged file is listed with its status") {
  Rig r;
  r.add_good("good.ptfx", "Good");
  auto damaged = make_ptfx("Broken");
  put32(damaged, 68, 0);  // code_size 0: PLUGIN FILE DAMAGED
  r.add("damaged.ptfx", damaged);
  auto notptfx = make_ptfx("Nope");
  notptfx[0] = 'X';  // magic broken: NOT A PTFX PLUGIN, the name was never read
  r.add("notptfx.ptfx", notptfx);
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 3);
  // Sorted by display name without case: "Broken" < "Good" < "notptfx.ptfx".
  CHECK(std::strcmp(r.cat.entry(0).display_name, "Broken") == 0);
  CHECK(r.cat.entry(0).status == PtfxStatus::FileDamaged);
  CHECK(std::strcmp(r.cat.entry(1).display_name, "Good") == 0);
  CHECK(r.cat.entry(1).status == PtfxStatus::Ok);
  CHECK(std::strcmp(r.cat.entry(2).display_name, "notptfx.ptfx") == 0);  // the refusal came before the name: the file name is the display name
  CHECK(r.cat.entry(2).status == PtfxStatus::NotPtfx);
  CHECK(r.cat.card_errors() == 0);  // a refusal is not a card error
}

TEST_CASE("ptfx_catalog: the sort is by display name without case") {
  Rig r;
  r.add_good("f1.ptfx", "zeta");
  r.add_good("f2.ptfx", "Alpha");
  r.add_good("f3.ptfx", "mid");
  r.add_good("f4.ptfx", "Bravo");
  r.add_good("f5.ptfx", "BRAVO2");  // ties on the fold break by file name: f4 before f5
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.count() == 5);
  const char* expected[] = {"Alpha", "Bravo", "BRAVO2", "mid", "zeta"};
  for (unsigned i = 0; i < 5; ++i) {
    CHECK(std::strcmp(r.cat.entry(i).display_name, expected[i]) == 0);
  }
  CHECK(PtfxCatalog::compare_names(r.cat.entry(1), r.cat.entry(2)) < 0);
}

TEST_CASE("ptfx_catalog: a card error is not a refusal, the file is retried at the next scan") {
  Rig r;
  r.add_good("gain.ptfx", "Gain");
  r.add_good("delay.ptfx", "Delay");
  r.card.read_fault = "effects/gain.ptfx";  // read() of this file fails with Io (the card's keys are lower case)
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);  // the scan goes on: the error is on one file, not the folder
  CHECK(r.cat.count() == 1);
  CHECK(r.cat.card_errors() == 1);
  CHECK(std::strcmp(r.cat.entry(0).display_name, "Delay") == 0);
  // The next scan retries the file: the fault is gone.
  r.card.read_fault.clear();
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 2);
  CHECK(r.cat.card_errors() == 0);
  CHECK(std::strcmp(r.cat.entry(0).display_name, "Delay") == 0);
  CHECK(std::strcmp(r.cat.entry(1).display_name, "Gain") == 0);
  CHECK(r.cat.entry(1).status == PtfxStatus::Ok);
  // An open() that fails the same way is a card error too.
  r.card.open_status = o3os::hal::storage::Status::Io;
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 0);
  CHECK(r.cat.card_errors() == 2);
  r.card.open_status = o3os::hal::storage::Status::Ok;
}

TEST_CASE("ptfx_catalog: a failed size() is a card error, retried at the next scan") {
  Rig r;
  r.add_good("gain.ptfx", "Gain");
  r.add_good("delay.ptfx", "Delay");
  r.card.size_fault = "effects/gain.ptfx";  // the card's keys are lower case
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);  // the scan ends Done: the owner must check card_errors()
  CHECK(r.cat.count() == 1);
  CHECK(r.cat.card_errors() == 1);
  CHECK(std::strcmp(r.cat.entry(0).display_name, "Delay") == 0);
  r.card.size_fault.clear();
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 2);
  CHECK(r.cat.card_errors() == 0);
  CHECK(r.cat.entry(1).status == PtfxStatus::Ok);
  CHECK(std::strcmp(r.cat.entry(1).display_name, "Gain") == 0);
}

TEST_CASE("ptfx_catalog: a card error in the middle of the listing fails the scan, the next scan retries it") {
  Rig r;
  r.add_good("a.ptfx", "A");
  r.add_good("b.ptfx", "B");
  r.add_good("c.ptfx", "C");
  r.card.readdir_fail_at = 1;  // the second readdir answers Io, after one file was read
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Failed);
  CHECK(r.cat.error() == o3os::hal::storage::Status::Io);
  CHECK(std::strcmp(r.cat.detail(), "CARD ERROR") == 0);
  CHECK(r.cat.count() == 1);          // what was read stays listed
  CHECK(r.cat.card_errors() == 0);    // the error was on the directory, not on a file
  CHECK(r.cat.step(2000) == o3os::core::JobStatus::Failed);
  r.card.readdir_fail_at = ~std::size_t{0};
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 3);
}

TEST_CASE("ptfx_catalog: a missing folder, a cancel") {
  Rig r;
  o3os::test::MemCard empty;
  CHECK(r.cat.begin(empty) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::NoFolder);
  CHECK(r.cat.error() == o3os::hal::storage::Status::NotFound);
  CHECK(std::strcmp(r.cat.detail(), "NO FOLDER") == 0);
  CHECK(r.cat.step(2000) == o3os::core::JobStatus::Failed);

  r.add_good("a.ptfx", "A");
  r.add_good("b.ptfx", "B");
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  CHECK(r.cat.step(PtfxCatalog::kUnitUs) == o3os::core::JobStatus::Running);  // one unit: one entry read, the scan goes on
  const unsigned before = r.cat.count();
  r.cat.cancel();
  CHECK(r.cat.state() == PtfxCatalog::State::Cancelled);
  CHECK(r.cat.count() == before);  // what was read stays listed
  CHECK(r.cat.step(2000) == o3os::core::JobStatus::Failed);
  CHECK(std::strcmp(r.cat.detail(), "") == 0);
  r.cat.invalidate();
  CHECK(r.cat.state() == PtfxCatalog::State::Idle);
  CHECK(r.cat.count() == 0);
  CHECK(r.card.open_handles == 0);
  CHECK(r.cat.begin(r.card, "") == PtfxCatalog::Begin::Started);  // the root is a folder too
  run_to_end(r.cat);
  CHECK(r.cat.state() == PtfxCatalog::State::Done);
  CHECK(r.cat.count() == 0);  // the root holds no .ptfx file, only the Effects folder
}

TEST_CASE("ptfx_catalog: an attached quarantine set flags the listed entries whose id it holds (S2c)") {
  Rig r;
  r.add_good("gain.ptfx", "Gain");
  r.add_good("echo.ptfx", "Echo");
  PtfxQuarantine q;
  CHECK(q.quarantine(ptfx_plugin_id("gain.ptfx"), PtfxQuarantine::Why::Crash) == PtfxQuarantine::Add::Added);
  r.cat.set_quarantine(&q);
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.count() == 2);
  // Sorted by display name: Echo, Gain.
  CHECK(std::strcmp(r.cat.entry(0).file_name, "echo.ptfx") == 0);
  CHECK_FALSE(r.cat.entry(0).quarantined);
  CHECK(std::strcmp(r.cat.entry(1).file_name, "gain.ptfx") == 0);
  CHECK(r.cat.entry(1).quarantined);
  // A later quarantine (S3: a timeout) shows at the next scan.
  CHECK(q.quarantine(ptfx_plugin_id("echo.ptfx"), PtfxQuarantine::Why::Timeout) == PtfxQuarantine::Add::Added);
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.count() == 2);
  CHECK(r.cat.entry(0).quarantined);
  CHECK(r.cat.entry(1).quarantined);
}

TEST_CASE("ptfx_catalog: without a quarantine set no entry is flagged") {
  Rig r;
  r.add_good("gain.ptfx", "Gain");
  CHECK(r.cat.begin(r.card) == PtfxCatalog::Begin::Started);
  run_to_end(r.cat);
  CHECK(r.cat.count() == 1);
  CHECK_FALSE(r.cat.entry(0).quarantined);
}
