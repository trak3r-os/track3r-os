// SPDX-License-Identifier: GPL-3.0-or-later
// OS-FILES part A: the File screen's IMPORT page on the App (docs/specs/OS-FILES-IMPORT.md): the module filter of the listing, F6 and the folders (F1 ENTER, F3 UP), F1 IMPORT through the
// device's load (a ProjectSink) with the unsaved-changes question, F7's engine, how an import ends in the strip, F8's report page. The importer itself is module_import_test.cpp; the
// pixels are the gallery states file_import and file_import_report.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/module_import.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fake_storage.h>
#include <o3os/platform/host/fakes.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::ImportEngine;
using app::ScreenId;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

class FakeProject final : public app::ProjectSink {
 public:
  Start result = Start::Started;
  bool is_busy = false;
  bool ok = true;
  std::string words;
  std::string last_file;
  ImportEngine last_engine = ImportEngine::Auto;
  unsigned imports = 0;
  unsigned opens = 0;
  bool module = true;  // OS-IMPORT-ANY I4: what the sink's header read says
  unsigned probes = 0;
  std::string probed;
  app::ModuleSummary summary;
  Start start_new() noexcept override { return Start::NoDocument; }
  Start open(const char*, Format) noexcept override {
    ++opens;
    return Start::NoDocument;
  }
  Start import_module(const char* file, ImportEngine engine) noexcept override {
    ++imports;
    if (result == Start::Started) {
      last_file = file;
      last_engine = engine;
      is_busy = true;
    }
    return result;
  }
  bool module_file(const char* file) noexcept override {
    ++probes;
    probed = file;
    return module;
  }
  const app::ModuleSummary* import_summary() const noexcept override { return &summary; }
  bool busy() const noexcept override { return is_busy; }
  const char* folder() const noexcept override { return ""; }
  bool loaded() const noexcept override { return ok; }
  const char* detail() const noexcept override { return words.c_str(); }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  platform::host::FakeStorage card;
  test::MemCard save_card;
  app::SaveService saves;
  FakeProject project;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;

  Rig() {
    model::ModelArena& arena = test::arena(1);
    test::fill_small(arena.project);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena.project, journal);
    app->attach_document(editor.get());
    app->attach_storage(&card);
    app->attach_project_sink(&project);
    saves.attach(save_card, save_card);
    app->attach_saves(&saves);
    card.add_dir("MOD");
    card.add_dir("S3M");
    card.add_dir("MED");
    card.add_file("MOD/kc.mod", 2000);
    card.add_file("MOD/Other.MOD", 2000);
    card.add_file("MOD/notes.txt", 10);
    card.add_file("S3M/simpsons.s3m", 3000);
    card.add_file("MED/song.mmd1", 3000);
    card.add_file("MED/old.med", 3000);
    card.add_file("root.mod", 100);
    card.add_file("song.xm", 100);
  }
  void pass() {
    clock.advance(16000);
    app->run_once();
  }
  void settle() {
    for (unsigned i = 0; i < 2000; ++i) {
      pass();
      const app::DirScan::State st = app->model().files->state();
      if (st != app::DirScan::State::Scanning && st != app::DirScan::State::Idle) {
        return;
      }
    }
    FAIL_CHECK("the scan did not end");
  }
  void expire() {
    for (unsigned i = 0; i < 10 && !message().empty(); ++i) {
      clock.advance(5000000);
      pass();
    }
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
  }
  // The IMPORT page, listed.
  void import_page() {
    go(ScreenId::File);
    tap(Button::F6);
    settle();
  }
  std::string message() const { return app->message(); }
  const app::DirIndex& index() const { return app->model().files->index(); }
  std::string name_at_cursor() const { return index().name(static_cast<unsigned>(app->state().file.pos())); }
  std::string names() const {
    std::string out;
    for (unsigned i = 0; i < index().count(); ++i) {
      out += (i != 0u ? "," : "") + std::string(index().name(i));
    }
    return out;
  }
  // The cursor onto the entry called `name`.
  bool to(const std::string& name) {
    for (unsigned i = 0; i < 40; ++i) {
      if (name_at_cursor() == name) {
        return true;
      }
      tap(Button::Down);
    }
    return false;
  }
  void end_load() {
    project.is_busy = false;
    pass();
  }
};

}  // namespace

TEST_CASE("module filter: folders to enter and the module files, any case; not the other files, not the hidden ones") {
  platform::host::FakeStorage card;
  card.add_dir("sub");
  card.add_dir("x.mod");  // a folder that is called like a module is a folder
  for (const char* n : {"a.mod", "B.MOD", "c.s3m", "d.S3m", "e.med", "f.mmd0", "g.mmd1", "h.MMD2", "i.mmd3", "j.xm", "k.it", "l.mmd", "m.mod.bak", "n.txt", ".hidden.mod", "._o.mod", "mod"}) {
    card.add_file(n, 10);
  }
  app::DirScan scan;
  NEED(scan.begin(card, "", app::kModuleFilter) == app::DirScan::Begin::Started);
  for (unsigned i = 0; i < 100 && scan.step(1000000) == core::JobStatus::Running; ++i) {
  }
  std::string listed;
  for (unsigned i = 0; i < scan.index().count(); ++i) {
    listed += std::string(scan.index().name(i)) + (scan.index().is_dir(i) ? "/" : "") + " ";
  }
  CHECK(scan.index().count() == 11);
  for (const char* want : {"sub/", "x.mod/", "a.mod ", "B.MOD ", "c.s3m ", "d.S3m ", "e.med ", "f.mmd0 ", "g.mmd1 ", "h.MMD2 ", "i.mmd3 "}) {
    CHECK_MESSAGE(listed.find(want) != std::string::npos, listed);
  }
  for (const char* not_want : {"j.xm", "k.it", "l.mmd ", "m.mod.bak", "n.txt", ".hidden", "._o", " mod "}) {
    CHECK_MESSAGE(listed.find(not_want) == std::string::npos, listed);
  }
}

// OS-IMPORT-ANY I2: the prefix names the Amiga programs use count as module names.
TEST_CASE("module filter: the prefix names (mod.title, MED.title, s3m.title, mmd0.title .. mmd3.title) are listed, any case; the bare word and look-alikes are not") {
  platform::host::FakeStorage card;
  for (const char* n : {"mod.title", "MOD.Other", "s3m.title", "S3M.x", "med.title", "MED.song", "mmd0.a", "mmd1.b", "MMD2.c", "mmd3.d", "mod.", "mod", "modx.y", "mmd4.z", "mmd.z", "xmod.y", "title.mod.bak", "song.xm"}) {
    card.add_file(n, 10);
  }
  app::DirScan scan;
  NEED(scan.begin(card, "", app::kModuleFilter) == app::DirScan::Begin::Started);
  for (unsigned i = 0; i < 100 && scan.step(1000000) == core::JobStatus::Running; ++i) {
  }
  std::string listed;
  for (unsigned i = 0; i < scan.index().count(); ++i) {
    listed += std::string(scan.index().name(i)) + " ";
  }
  CHECK(scan.index().count() == 10);
  for (const char* want : {"mod.title ", "MOD.Other ", "s3m.title ", "S3M.x ", "med.title ", "MED.song ", "mmd0.a ", "mmd1.b ", "MMD2.c ", "mmd3.d "}) {
    CHECK_MESSAGE(listed.find(want) != std::string::npos, listed);
  }
  CHECK(app::module_name_kind("mod.title") == app::ModuleName::Mod);
  CHECK(app::module_name_kind("S3M.x") == app::ModuleName::S3m);
  CHECK(app::module_name_kind("MMD2.c") == app::ModuleName::Med);
  CHECK(app::module_name_kind("song.mod") == app::ModuleName::Mod);  // the extension first
  CHECK(app::module_name_kind("mod.song.s3m") == app::ModuleName::S3m);
  CHECK(app::module_name_kind("mod.") == app::ModuleName::None);
  CHECK(app::module_name_kind("kick") == app::ModuleName::None);
  // ALL FILES lists every file but the hidden ones, and a scan of the same folder with the other filter is not the cache
  card.add_file(".hidden", 10);
  NEED(scan.begin(card, "", app::kAllFilesFilter) == app::DirScan::Begin::Started);
  for (unsigned i = 0; i < 100 && scan.step(1000000) == core::JobStatus::Running; ++i) {
  }
  CHECK(scan.index().count() == 18);
}

// OS-IMPORT-ANY I1: F2 on the IMPORT page switches the list between MODULES and ALL FILES.
TEST_CASE("file import: F2 switches the list between MODULES and ALL FILES, rescanning as it does; MODULES again at every visit") {
  Rig rig;
  rig.card.add_file("README", 10);
  rig.card.add_file("tune", 10);
  rig.card.add_file("mod.old", 10);
  rig.import_page();
  CHECK_FALSE(rig.app->state().import.all);
  CHECK(rig.names().find("mod.old") != std::string::npos);  // the prefix name is a module name
  CHECK(rig.names().find("README") == std::string::npos);
  rig.tap(Button::F2);
  rig.settle();
  CHECK(rig.app->state().import.all);
  CHECK(rig.names().find("README") != std::string::npos);
  CHECK(rig.names().find("tune") != std::string::npos);
  CHECK(rig.names().find("song.xm") != std::string::npos);
  rig.tap(Button::F2);
  rig.settle();
  CHECK_FALSE(rig.app->state().import.all);
  CHECK(rig.names().find("README") == std::string::npos);
  // a folder keeps the mode; leaving the page and coming back is MODULES
  rig.tap(Button::F2);
  rig.settle();
  NEED(rig.to("MOD"));
  rig.tap(Button::F1);
  rig.settle();
  CHECK(rig.app->state().import.all);
  CHECK(rig.names().find("notes.txt") != std::string::npos);
  rig.tap(Button::F6);  // PROJECTS
  rig.settle();
  rig.tap(Button::F6);  // IMPORT again
  rig.settle();
  CHECK_FALSE(rig.app->state().import.all);
  CHECK(rig.names().find("README") == std::string::npos);
}

// OS-IMPORT-ANY I3, I4, I5: a file that says nothing is imported by its bytes; one that is no module is refused before the NEW question.
TEST_CASE("file import: a file whose name says nothing is offered to the import by ALL FILES, and a file that is no module is refused before the question") {
  Rig rig;
  rig.card.add_file("tune", 100);
  rig.import_page();
  rig.tap(Button::F2);
  rig.settle();
  NEED(rig.to("tune"));
  model::Project& project = test::arena(1).project;
  CHECK(project.set_name("edited", 6) == model::Status::Ok);  // a change no save has taken: the question would be asked
  rig.project.module = false;
  rig.tap(Button::F1);
  CHECK(rig.project.probes == 1);
  CHECK(rig.project.probed == "tune");
  CHECK(rig.message() == "NOT A MODULE THIS OS READS");
  CHECK_FALSE(rig.app->state().file.confirm_new);  // no question
  CHECK(rig.project.imports == 0);
  rig.project.module = true;
  rig.expire();
  rig.tap(Button::F1);  // a module: now the question, and then the import
  CHECK(rig.message() == "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM");
  CHECK(rig.project.imports == 0);
  rig.tap(Button::F1);
  CHECK(rig.project.imports == 1);
  CHECK(rig.project.last_file == "tune");
}

TEST_CASE("file import: F6 opens the IMPORT page, which lists the card's root with the module filter; F6 again is the project list") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.settle();
  CHECK_FALSE(rig.app->state().import.page);
  CHECK(std::string(rig.app->model().files->folder()) == "Projects");
  rig.tap(Button::F6);
  rig.settle();
  CHECK(rig.app->state().import.page);
  CHECK(std::string(rig.app->model().files->folder()).empty());
  CHECK(rig.index().count() == 4);  // the folders MED MOD S3M and root.mod; song.xm is no module
  CHECK(rig.names().find("root.mod") != std::string::npos);
  CHECK(rig.names().find("song.xm") == std::string::npos);
  CHECK(rig.index().is_dir(0));
  rig.tap(Button::F6);
  rig.settle();
  CHECK_FALSE(rig.app->state().import.page);
  CHECK(std::string(rig.app->model().files->folder()) == "Projects");
}

TEST_CASE("file import: F1 on a folder enters it, F3 goes up one folder, at the root F3 says so") {
  Rig rig;
  rig.import_page();
  NEED(rig.to("MOD"));
  rig.tap(Button::F1);
  rig.settle();
  CHECK(std::string(rig.app->state().import.folder) == "MOD");
  CHECK(std::string(rig.app->model().files->folder()) == "MOD");
  CHECK(rig.names().find("kc.mod") != std::string::npos);
  CHECK(rig.names().find("Other.MOD") != std::string::npos);
  CHECK(rig.names().find("notes.txt") == std::string::npos);
  CHECK(rig.project.imports == 0);  // a folder is never imported
  rig.tap(Button::F3);
  rig.settle();
  CHECK(std::string(rig.app->state().import.folder).empty());
  rig.expire();
  rig.tap(Button::F3);
  CHECK(rig.message() == "ALREADY AT THE ROOT");
  // the page is left and come back to: the folder is kept
  NEED(rig.to("S3M"));
  rig.tap(Button::F1);
  rig.settle();
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::File);
  rig.settle();
  CHECK(std::string(rig.app->model().files->folder()) == "S3M");
}

TEST_CASE("file import: F1 on a module hands its path and the engine to the device's load; the strip says IMPORTING, then how it ended") {
  Rig rig;
  rig.import_page();
  NEED(rig.to("MOD"));
  rig.tap(Button::F1);
  rig.settle();
  NEED(rig.to("kc.mod"));
  rig.tap(Button::F1);
  CHECK(rig.project.imports == 1);
  CHECK(rig.project.last_file == "MOD/kc.mod");
  CHECK(rig.project.last_engine == ImportEngine::Auto);
  CHECK(rig.message() == "IMPORTING KC.MOD");
  CHECK(rig.app->state().file.pending == app::FileView::Pending::Import);
  rig.pass();
  CHECK(rig.message() == "IMPORTING KC.MOD");  // the load still runs: nothing is told yet
  rig.project.summary.valid = true;
  rig.project.summary.ok = true;
  std::strcpy(rig.project.summary.name, "KC.MOD");
  rig.end_load();
  CHECK(rig.message() == "IMPORTED KC.MOD");
  CHECK_FALSE(rig.app->state().file.opening);
}

TEST_CASE("file import: a failed import is told with its reason; one with something to read says F8 REPORT") {
  Rig rig;
  rig.import_page();
  NEED(rig.to("root.mod"));
  rig.tap(Button::F1);
  rig.project.ok = false;
  rig.project.summary.valid = true;
  rig.project.summary.ok = false;
  std::strcpy(rig.project.summary.name, "ROOT.MOD");
  std::strcpy(rig.project.summary.error, "TRUNCATED");
  rig.end_load();
  CHECK(rig.message() == "IMPORT ROOT.MOD FAILED: TRUNCATED");
  rig.expire();

  rig.project.ok = true;
  rig.project.summary.ok = true;
  rig.project.summary.error[0] = '\0';
  rig.project.summary.dropped_groups = 1u << 8;
  rig.project.summary.samples_no_room = 2;
  rig.tap(Button::F1);
  rig.end_load();
  CHECK(rig.message() == "IMPORTED ROOT.MOD: 2 NO ROOM, F8 REPORT");
}

TEST_CASE("file import: the load's own refusals are told, and a running load or save blocks the key") {
  Rig rig;
  rig.import_page();
  NEED(rig.to("root.mod"));
  rig.project.result = app::ProjectSink::Start::Busy;
  rig.tap(Button::F1);
  CHECK(rig.message() == "A LOAD IS ALREADY RUNNING");
  rig.expire();
  rig.project.result = app::ProjectSink::Start::NoCard;
  rig.tap(Button::F1);
  CHECK(rig.message() == "THE CARD IS NOT MOUNTED");
  rig.expire();
  rig.project.result = app::ProjectSink::Start::NoDocument;
  rig.tap(Button::F1);
  CHECK(rig.message() == "NO DOCUMENT TO REPLACE");
  rig.expire();
  rig.project.result = app::ProjectSink::Start::BadPath;
  rig.tap(Button::F1);
  CHECK(rig.message() == "NOT A MODULE FILE PATH");
  rig.expire();
  rig.project.result = app::ProjectSink::Start::Started;
  rig.project.is_busy = true;  // a load is running
  const unsigned before = rig.project.imports;
  rig.tap(Button::F1);
  CHECK(rig.message() == "A LOAD IS ALREADY RUNNING");
  CHECK(rig.project.imports == before);
}

TEST_CASE("file import: with unsaved changes the first F1 only asks, the second goes on; a move or another key takes the question back") {
  Rig rig;
  rig.import_page();
  NEED(rig.to("root.mod"));
  model::Project& project = test::arena(1).project;
  CHECK(project.set_name("edited", 6) == model::Status::Ok);  // a change no save has taken
  rig.tap(Button::F1);
  CHECK(rig.message() == "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM");
  CHECK(rig.project.imports == 0);
  CHECK(rig.app->state().file.confirm_new);
  rig.expire();
  rig.tap(Button::Down);  // a move takes it back
  CHECK_FALSE(rig.app->state().file.confirm_new);
  NEED(rig.to("root.mod"));
  rig.tap(Button::F1);
  CHECK(rig.project.imports == 0);
  rig.tap(Button::F8);  // REPORT is another key (F2 is the MODULES / ALL switch on this page)
  CHECK_FALSE(rig.app->state().file.confirm_new);
  rig.settle();
  NEED(rig.to("root.mod"));
  rig.tap(Button::F1);
  rig.expire();
  rig.tap(Button::F1);
  CHECK(rig.project.imports == 1);  // the second press
  rig.end_load();
  rig.app->document_replaced(true);  // the platform made the document the import's: new work on no card, so a second import asks (OS-FILES review)
  rig.expire();
  rig.tap(Button::F1);
  CHECK(rig.project.imports == 1);
  CHECK(rig.message() == "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM");
  rig.tap(Button::Down);  // the question is taken back
  CHECK_FALSE(rig.app->state().file.confirm_new);
  NEED(rig.to("root.mod"));
  rig.app->document_replaced();  // (a clean document: e.g. a project opened afterwards)
  rig.expire();
  rig.tap(Button::F1);
  CHECK(rig.project.imports == 2);  // nothing unsaved: no question
  CHECK(rig.message() == "IMPORTING ROOT.MOD");
}

TEST_CASE("file import: F7 cycles the engine and says it; the next import carries it; it is the session's, not the page's") {
  Rig rig;
  rig.import_page();
  CHECK(rig.app->state().import.engine == ImportEngine::Auto);
  rig.tap(Button::F7);
  CHECK(rig.app->state().import.engine == ImportEngine::Amiga);
  CHECK(rig.message() == "IMPORT COMMANDS FOR THE AMIGA ENGINE");
  rig.expire();
  rig.tap(Button::F7);
  CHECK(rig.app->state().import.engine == ImportEngine::Modern);
  CHECK(rig.message() == "IMPORT COMMANDS FOR THE MODERN ENGINE");
  NEED(rig.to("root.mod"));
  rig.tap(Button::F1);
  CHECK(rig.project.last_engine == ImportEngine::Modern);
  rig.end_load();
  rig.expire();
  rig.tap(Button::F6);  // to the project list and back
  rig.tap(Button::F6);
  rig.settle();
  CHECK(rig.app->state().import.engine == ImportEngine::Modern);
  rig.expire();
  rig.tap(Button::F7);
  CHECK(rig.app->state().import.engine == ImportEngine::Auto);
  CHECK(std::string(app::engine_key_label(ImportEngine::Auto)) == "FX AUTO");
  CHECK(std::string(app::engine_key_label(ImportEngine::Amiga)) == "FX AMIGA");
  CHECK(std::string(app::engine_key_label(ImportEngine::Modern)) == "FX MODRN");
}

TEST_CASE("file import: F8 shows the last import's report in place of the list, and back; with none it says so") {
  Rig rig;
  rig.import_page();
  rig.tap(Button::F8);
  CHECK(rig.message() == "NO IMPORT YET");
  CHECK_FALSE(rig.app->state().import.report);
  rig.expire();
  NEED(rig.to("root.mod"));
  rig.tap(Button::F1);
  rig.project.summary.valid = true;
  rig.project.summary.ok = true;
  rig.end_load();
  rig.expire();
  CHECK(rig.app->model().import_report == &rig.project.summary);
  rig.tap(Button::F8);
  CHECK(rig.app->state().import.report);
  rig.tap(Button::F8);
  CHECK_FALSE(rig.app->state().import.report);
  rig.tap(Button::F8);
  rig.tap(Button::F6);  // leaving the page ends the report view
  CHECK_FALSE(rig.app->state().import.report);
}

TEST_CASE("file import: the project page is as it was: F1 opens, F8 does nothing and F7 is the EXPORT page (OS-WAVEXP), not the import's engine, no import is started") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.settle();
  rig.tap(Button::F8);
  rig.tap(Button::F7);  // OS-WAVEXP: EXPORT on the PROJECTS page; F7 again is PROJECTS
  CHECK(rig.app->state().xport.page);
  rig.tap(Button::F7);
  CHECK_FALSE(rig.app->state().xport.page);
  CHECK(rig.app->state().import.engine == ImportEngine::Auto);
  CHECK(rig.message().empty());
  rig.tap(Button::F1);  // nothing listed in Projects/: the file open says so
  CHECK(rig.project.imports == 0);
  CHECK(rig.message() == "NOTHING TO OPEN");
}
