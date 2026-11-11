// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FILES part B (docs/specs/OS-FILES-B.md): the File screen's PROJECTS page on the App over an in-memory card (tests/support/mem_card.h, which applies the real path guard and logs every change):
// the Delete key (two presses, then an empty .deleted marker in the folder, which the list hides), the RECENT group and Settings/recent.o3rc, and Shift + F7 PURGE UNUSED (the document only). Nothing on the card is ever erased
// (`removed()` stays empty), the open project is never moved, and the guard is never asked for anything it refuses (`violations`). The guard's own rules are save_guard_test.cpp, the list's recent_file_test.cpp,
// what names an instrument instrument_use_test.cpp, and the marker over FatFs save_fatfs_test.cpp.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/key_row.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/static_slot.h"

#include <o3os/app/app.h>
#include <o3os/app/native_load.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/app/state_line.h>
#include <o3os/audio/voices.h>
#include <o3os/core/input_event.h>
#include <o3os/core/learn.h>
#include <o3os/model/editor.h>
#include <o3os/model/instrument_use.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::ScreenId;
using test::MemCard;
using Bytes = MemCard::Bytes;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];

class FakeProject final : public app::ProjectSink {
 public:
  std::string last_folder;
  bool is_busy = false;
  bool ok = true;
  unsigned opens = 0;
  Start open_result = Start::Started;
  Format last_format = Format::Og;
  std::string words;       // detail() after an open that ended badly
  bool was_refused = false;
  Start open(const char* folder, Format format) noexcept override {
    ++opens;
    last_format = format;
    if (open_result == Start::Started) {
      last_folder = folder;
      is_busy = true;
    }
    return open_result;
  }
  Start start_new() noexcept override { return Start::Started; }
  bool busy() const noexcept override { return is_busy; }
  const char* folder() const noexcept override { return last_folder.c_str(); }
  bool loaded() const noexcept override { return ok; }
  const char* detail() const noexcept override { return words.c_str(); }
  bool refused() const noexcept override { return was_refused; }
  bool newer = false;           // OS-MIDIREC-P1 S2: the loaded file had fields of a newer minor version
  std::uint32_t lanes_lost = 0;  // ... and MIDI lanes this build skipped
  bool newer_fields() const noexcept override { return newer; }
  std::uint32_t lanes_skipped() const noexcept override { return lanes_lost; }
};

audio::VoicePool& make_pool() {
  static o3os::test::StaticSlot<audio::VoicePool> slot;
  return slot.make();
}

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  MemCard card;
  app::SaveService saves;
  FakeProject project;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> bank_mem = std::make_unique<app::SampleBankMem>();
  std::vector<std::int16_t> memory = std::vector<std::int16_t>(40000);
  app::SampleBank bank;

  Rig() {
    model::ModelArena& arena = test::arena(1);
    arena.project.reset();
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena.project, journal);
    app->attach_document(editor.get());
    bank.init(make_pool(), arena, *bank_mem, memory.data(), static_cast<std::uint32_t>(memory.size()));
    app->attach_samples(&bank);
    app->attach_storage(&card);
    app->attach_project_sink(&project);
    saves.attach(card, card);
    app->attach_saves(&saves);
    card.make_dir_now("Projects");
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void passes(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      pass();
    }
  }
  void tap(Button b, bool shift = false) {
    if (shift) {
      input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
      pass(1);
    }
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
    if (shift) {
      input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
      pass(1);
    }
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
  }
  // Passes until the listing has ended and nothing is running (no job, no save, no move of purged samples) for a few passes in a row (a bound of the test's own): a write that follows another
  // (the RECENT file after a save) starts a pass after it.
  void settle() {
    unsigned quiet = 0;
    for (unsigned i = 0; i < 6000; ++i) {
      pass();
      const app::DirScan* scan = app->model().files;
      const bool listed = scan == nullptr || (scan->state() != app::DirScan::State::Scanning && scan->state() != app::DirScan::State::Idle);
      quiet = (listed && app->session().jobs.occupied() == 0u && !saves.busy()) ? quiet + 1u : 0u;
      if (quiet >= 8u) {
        return;
      }
    }
    FAIL_CHECK("did not settle");
  }
  void expire() {
    for (unsigned i = 0; i < 10 && !message().empty(); ++i) {
      clock.advance(5000000);
      pass();
    }
  }
  std::string message() const { return app->message(); }
  Bytes native_bytes() const {
    std::vector<std::uint8_t> out;
    (void)test::save(test::arena(1).project, out);
    return out;
  }
  const app::DirIndex& index() const { return app->model().files->index(); }
  std::string name_at_cursor() const {
    const int e = app->state().file.entry(index());
    return e < 0 ? std::string("<none>") : std::string(index().name(static_cast<unsigned>(e)));
  }
  bool on_recent_row() const { return app->state().file.pos() < app->state().file.lead(); }
  void add_project(const std::string& name) {
    card.make_dir_now(("Projects/" + name).c_str());
    card.put(("Projects/" + name + "/project.o3pj").c_str(), native_bytes());
  }
  // The File screen, listed.
  void open_file_screen() {
    go(ScreenId::File);
    settle();
  }
  // The cursor on the A-Z row of `name` (not its RECENT row), by the Down key, and the Up key when it is above.
  void cursor_to(const std::string& name, bool recent = false) {
    for (unsigned i = 0; i < 120; ++i) {
      if (name_at_cursor() == name && on_recent_row() == recent) {
        return;
      }
      tap(i < 50 ? Button::Down : Button::Up);
    }
    FAIL_CHECK("the cursor never reached " << name);
  }
  void typed(const char* name) { const_cast<app::TextEntry&>(app->state().text.entry).set(name); }
  // The project is "open": the platform's load said Started and ended well.
  void open_project(const std::string& name) {
    cursor_to(name);
    project.last_folder.clear();
    tap(Button::F1);
    project.is_busy = false;
    pass();
    settle();  // (the RECENT file is written after an open)
    expire();
    CHECK(app->state().screen == ScreenId::Pattern);  // OS-OPENTOPAT: a project that opened shows the Pattern view
    go(ScreenId::File);                               // ... and the tests go on from the File screen
  }
  std::string file_text(const char* path) const {
    const Bytes* b = card.get(path);
    return b == nullptr ? std::string("<none>") : std::string(b->begin(), b->end());
  }
  std::vector<std::string> names() const {
    std::vector<std::string> out;
    for (unsigned i = 0; i < index().count(); ++i) {
      out.push_back(index().name(i));
    }
    return out;
  }
};

using Names = std::vector<std::string>;

}  // namespace

// ---- RENAME is dropped (docs/specs/OS-FILES-B.md): F8 on the PROJECTS page is blank ------------------------------------------------------------------------------------------------

// OS-HILNAV: `diag state` names the project under the cursor on the PROJECTS page (sel=), so a HIL scenario walks to a project by name; `-` once the screen is left.
TEST_CASE("state line: sel= is the project under the cursor on File, spaces as underscores, and `-` on other screens") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta two");
  rig.open_file_screen();
  rig.settle();
  auto sel = [&]() -> std::string {
    char buf[384];
    (void)app::describe_state(*rig.app, buf, sizeof buf);
    const std::string line = buf;
    const std::size_t at = line.find(" sel=");
    if (at == std::string::npos) {
      return "<missing>";
    }
    const std::size_t end = line.find(' ', at + 1);
    return line.substr(at + 5, end == std::string::npos ? std::string::npos : end - at - 5);
  };
  const std::string first = sel();
  CHECK(first != "-");
  CHECK(first != "<missing>");
  rig.tap(Button::Down);
  const std::string second = sel();
  CHECK(second != first);
  CHECK(second.find(' ') == std::string::npos);
  CHECK((first == "alpha" || first == "beta_two" || second == "alpha" || second == "beta_two"));
  rig.go(ScreenId::Pattern);
  CHECK(sel() == "-");
}

TEST_CASE("F8 on the PROJECTS page does nothing: no keyboard, no card call, the key row says so (RENAME was dropped)") {
  Rig rig;
  rig.add_project("alpha");
  rig.open_file_screen();
  rig.settle();
  rig.card.log.clear();
  rig.tap(Button::F8);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.message().empty());
  CHECK(rig.card.log.empty());
  CHECK(std::string(app::kFileSoftKeys[7]) == "--");
  CHECK(rig.saves.recent().count() == 0u);
  // the service has no way to rename a folder either: the guard refuses it before any card call
  CHECK(rig.card.violations == 0);
  CHECK(rig.names() == Names{"alpha"});
}

// ---- DELETE -----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("delete: the first press asks, a move or another key takes the question back, the second press marks the folder .deleted and the name leaves the list") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta");
  rig.card.put("Projects/beta/notes.txt", std::string("mine"));
  rig.open_file_screen();
  rig.cursor_to("beta");
  rig.tap(Button::Delete);
  CHECK(rig.message() == "DELETE BETA? DELETE AGAIN");
  CHECK(rig.app->state().file.confirm_delete);
  CHECK_FALSE(rig.card.has("Projects/beta/.deleted"));
  // a move takes it back: the next press asks again
  rig.tap(Button::Up);
  CHECK_FALSE(rig.app->state().file.confirm_delete);
  rig.tap(Button::Down);
  rig.tap(Button::Delete);
  CHECK(rig.app->state().file.confirm_delete);
  // another key (RESCAN, ...) and leaving the screen do too
  rig.tap(Button::F2);
  CHECK_FALSE(rig.app->state().file.confirm_delete);
  rig.settle();
  rig.cursor_to("beta");
  rig.tap(Button::Delete);
  CHECK(rig.app->state().file.confirm_delete);
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::File);
  CHECK_FALSE(rig.app->state().file.confirm_delete);
  rig.settle();
  rig.cursor_to("beta");
  rig.tap(Button::Delete);
  CHECK(rig.card.log.empty());  // nothing was touched by any question
  // the second press goes on
  rig.tap(Button::Delete);
  CHECK(rig.message() == "DELETED: BETA");
  CHECK_FALSE(rig.app->state().file.confirm_delete);
  rig.settle();
  CHECK(rig.card.has("Projects/beta/.deleted"));
  CHECK(rig.card.get("Projects/beta/.deleted")->empty());   // an empty marker
  CHECK(rig.card.has("Projects/beta/project.o3pj"));         // nothing was moved or erased
  CHECK(rig.file_text("Projects/beta/notes.txt") == "mine");
  CHECK(rig.names() == Names{"alpha"});                      // the list hides it
  CHECK(rig.card.violations == 0);
  CHECK(rig.card.removed().empty());
  CHECK_FALSE(rig.card.dirs.count("trash") != 0u);           // there is no Trash
  // and another press does not delete the next one without asking
  rig.expire();
  rig.tap(Button::Delete);
  CHECK(rig.message() == "DELETE ALPHA? DELETE AGAIN");
  CHECK_FALSE(rig.card.has("Projects/alpha/.deleted"));
}

TEST_CASE("delete: the second press acts on the entry the first press named: a listing that changed, or still coming in, takes the question back") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta");
  rig.open_file_screen();
  rig.cursor_to("beta");
  rig.tap(Button::Delete);
  NEED(rig.app->state().file.confirm_delete);
  // the listing is read again under the cursor (a card swap, a rescan from elsewhere): the generation moves, the question is asked again
  const_cast<app::ViewState&>(rig.app->state()).file.rescan = true;
  rig.pass();
  rig.settle();
  rig.cursor_to("beta");
  const_cast<app::ViewState&>(rig.app->state()).file.confirm_delete = true;  // (the flag survived: a scan that was running does not clear it)
  rig.expire();
  rig.tap(Button::Delete);
  CHECK(rig.message() == "DELETE BETA? DELETE AGAIN");
  CHECK_FALSE(rig.card.has("Projects/beta/.deleted"));
  // the same list, the same entry: the next press goes on
  rig.tap(Button::Delete);
  CHECK(rig.card.has("Projects/beta/.deleted"));
  // an entry the cursor reached another way: the confirm is the first one's, not this one's
  Rig other;
  other.add_project("alpha");
  other.add_project("beta");
  other.open_file_screen();
  other.cursor_to("alpha");
  other.tap(Button::Delete);
  NEED(other.app->state().file.confirm_delete);
  app::FileView& v = const_cast<app::ViewState&>(other.app->state()).file;
  v.target_key = static_cast<std::uint16_t>(v.target_key + 1u);  // (what a different entry under the cursor looks like)
  other.tap(Button::Delete);
  CHECK(other.message() == "DELETE ALPHA? DELETE AGAIN");
  CHECK_FALSE(other.card.has("Projects/alpha/.deleted"));
}

TEST_CASE("delete: the open project is refused, with no question; a long name is cut so that the question still ends in DELETE AGAIN") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("thirty one characters long nme");
  rig.open_file_screen();
  rig.open_project("alpha");
  rig.card.log.clear();  // (the RECENT file's write)
  rig.cursor_to("alpha");
  rig.tap(Button::Delete);
  CHECK(rig.message() == "CANNOT DELETE THE OPEN PROJECT");
  CHECK_FALSE(rig.app->state().file.confirm_delete);
  rig.tap(Button::Delete);
  CHECK(rig.message() == "CANNOT DELETE THE OPEN PROJECT");
  CHECK_FALSE(rig.card.has("Projects/alpha/.deleted"));
  CHECK(rig.card.log.empty());
  rig.expire();
  rig.cursor_to("thirty one characters long nme");
  rig.tap(Button::Delete);
  const std::string question = rig.message();
  CHECK(question.size() <= 48u);
  CHECK(question.find("? DELETE AGAIN") == question.size() - 14u);
  CHECK(question.rfind("DELETE THIRTY ONE", 0) == 0u);
}

TEST_CASE("delete: the service marks a folder once (a second DELETE is the same answer), refuses the open project and a missing one, and the project leaves the RECENT list") {
  Rig rig;
  rig.add_project("song");
  rig.add_project("alpha");
  rig.open_file_screen();
  rig.saves.note_opened("Projects/song");
  rig.settle();
  CHECK(rig.saves.recent().count() == 1u);
  CHECK(rig.saves.delete_project("", "song") == app::SaveService::Move::Done);
  CHECK(rig.card.has("Projects/song/.deleted"));
  CHECK(rig.card.has("Projects/song/project.o3pj"));
  CHECK(rig.saves.delete_project("", "Song") == app::SaveService::Move::Done);  // marked already (any case)
  CHECK(rig.saves.delete_project("", "missing") == app::SaveService::Move::NotFound);
  CHECK(rig.saves.delete_project("", "bad/name") == app::SaveService::Move::BadName);
  CHECK(rig.saves.delete_project("Projects/alpha", "alpha") == app::SaveService::Move::OpenProject);
  CHECK_FALSE(rig.card.has("Projects/alpha/.deleted"));
  CHECK(rig.saves.recent().count() == 0u);
  rig.settle();
  CHECK(rig.file_text("Settings/recent.o3rc") == "O3RC 1\n");
  rig.tap(Button::F2);
  rig.settle();
  CHECK(rig.names() == Names{"alpha"});
  // a Save As of that name is refused: the folder is there
  CHECK(rig.saves.save_project(rig.app->session().jobs, test::arena(1).project, "", true, "song", nullptr) == app::SaveService::Start::NameTaken);
  CHECK(rig.card.violations == 0);
  CHECK(rig.card.removed().empty());
}

TEST_CASE("recent: a remount of the same card (same serial) reads Settings/recent.o3rc again: the computer may have changed it (OS-BUG-REMOUNT)") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta");
  rig.add_project("gamma");
  rig.card.make_dir_now("Settings");
  rig.card.put("Settings/recent.o3rc", std::string("O3RC 1\nbeta\nalpha\n"));
  rig.open_file_screen();
  CHECK(rig.saves.recent().count() == 2u);
  rig.card.put("Settings/recent.o3rc", std::string("O3RC 1\ngamma\n"));  // edited elsewhere while the card was out
  rig.open_file_screen();
  CHECK(rig.saves.recent().count() == 2u);  // (the control: with no card event the list that was read is believed)
  rig.app->card_changed(app::CardEvent::Removed);
  rig.app->card_changed(app::CardEvent::Reading);
  rig.app->card_changed(app::CardEvent::Ready);
  rig.open_file_screen();
  CHECK(rig.saves.recent().count() == 1u);
  CHECK(rig.name_at_cursor() == "gamma");
}

TEST_CASE("delete: a folder marked from outside (a computer) stays hidden, drops out of recent.o3rc, and a stale row of a project marked since refuses to load") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta");
  rig.add_project("gamma");
  rig.card.make_dir_now("Settings");
  rig.card.put("Settings/recent.o3rc", std::string("O3RC 1\nbeta\nalpha\n"));
  rig.card.put("Projects/beta/.deleted", std::string());  // made on a computer
  rig.open_file_screen();
  CHECK(rig.names() == Names{"alpha", "gamma"});            // hidden: not listed
  CHECK(rig.app->state().file.lead() == 1);                 // and not in the RECENT group (alpha is)
  CHECK(rig.saves.recent().count() == 1u);                  // beta was dropped from the list
  CHECK(rig.name_at_cursor() == "alpha");
  // alpha is marked after the list was read: its row is stale, F1 does not load it
  rig.card.put("Projects/alpha/.deleted", std::string());
  rig.tap(Button::F1);
  CHECK(rig.message() == "THIS PROJECT WAS DELETED");
  CHECK(rig.project.opens == 0u);
  CHECK_FALSE(rig.app->state().file.opening);
  rig.expire();
  rig.cursor_to("gamma");
  rig.tap(Button::F1);
  CHECK(rig.project.opens == 1u);  // an unmarked one still opens
  // the question every load asks, which the console's `diag load` asks as well (platform start_project_load)
  CHECK(app::folder_marked_deleted(rig.card, "Projects/alpha"));
  CHECK(app::folder_marked_deleted(rig.card, "Projects/Beta"));
  CHECK_FALSE(app::folder_marked_deleted(rig.card, "Projects/gamma"));
  CHECK_FALSE(app::folder_marked_deleted(rig.card, "Projects/missing"));
  CHECK_FALSE(app::folder_marked_deleted(rig.card, ""));
}

TEST_CASE("delete: a FOLDER called .deleted is not a marker: the project is listed and opens, and DELETE on it fails with the card's words, nothing created") {
  Rig rig;
  rig.add_project("alpha");
  rig.card.make_dir_now("Projects/alpha/.deleted");
  rig.card.put("Projects/alpha/.deleted/keep.txt", std::string("x"));
  rig.open_file_screen();
  CHECK(rig.names() == Names{"alpha"});
  CHECK_FALSE(app::folder_marked_deleted(rig.card, "Projects/alpha"));
  rig.tap(Button::Delete);
  rig.tap(Button::Delete);
  CHECK(rig.message() == "DELETE FAILED: NAME EXISTS");
  CHECK(rig.card.has("Projects/alpha/.deleted/keep.txt"));
  CHECK(rig.card.removed().empty());
  rig.expire();
  rig.tap(Button::F1);
  CHECK(rig.project.opens == 1u);
}

TEST_CASE("delete: the failure paths leave the folder as it was: a card that fails the marker, a read-only card, the firmware update using the card") {
  Rig rig;
  rig.add_project("alpha");
  rig.card.put("Projects/alpha/notes.txt", std::string("n"));
  rig.open_file_screen();
  // the marker's create fails with an I/O error (nothing was made)
  rig.card.fail_at = rig.card.ops;
  CHECK(rig.saves.delete_project("", "alpha") == app::SaveService::Move::Failed);
  CHECK(rig.saves.move_status() == hal::storage::WriteStatus::Io);
  CHECK_FALSE(rig.card.has("Projects/alpha/.deleted"));
  CHECK(rig.card.has("Projects/alpha/project.o3pj"));
  rig.card.fail_at = ~std::size_t{0};
  // the screen says it
  rig.cursor_to("alpha");
  rig.card.fail_at = rig.card.ops;
  rig.tap(Button::Delete);
  rig.tap(Button::Delete);
  CHECK(rig.message() == "DELETE FAILED: I/O ERROR");
  CHECK_FALSE(rig.card.has("Projects/alpha/.deleted"));
  rig.card.fail_at = ~std::size_t{0};
  rig.expire();
  rig.card.gate_status = hal::storage::WriteStatus::WriteProtected;
  rig.tap(Button::Delete);
  CHECK(rig.message() == "CARD IS WRITE PROTECTED");
  CHECK_FALSE(rig.app->state().file.confirm_delete);
  rig.card.gate_status = hal::storage::WriteStatus::Ok;
  rig.expire();
  rig.saves.attach_claim([]() noexcept { return true; });
  rig.tap(Button::Delete);
  rig.tap(Button::Delete);
  CHECK(rig.message() == "FIRMWARE UPDATE HAS THE CARD: TRY AGAIN");
  rig.saves.attach_claim(nullptr);
  CHECK_FALSE(rig.card.has("Projects/alpha/.deleted"));
  CHECK(rig.card.violations == 0);
  CHECK(rig.card.removed().empty());
}

TEST_CASE("delete: the Delete key does nothing on the IMPORT and EXPORT pages, and a RECENT row deletes its folder") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta");
  rig.saves.note_opened("Projects/beta");
  rig.open_file_screen();
  rig.settle();
  rig.card.log.clear();  // (the RECENT file has been written)
  rig.tap(Button::F6);  // IMPORT
  rig.expire();
  rig.tap(Button::Delete);
  rig.tap(Button::Delete);
  CHECK(rig.card.log.empty());
  CHECK(rig.message().empty());
  rig.tap(Button::F6);  // PROJECTS again
  rig.tap(Button::F7);  // EXPORT
  rig.tap(Button::Delete);
  rig.tap(Button::Delete);
  CHECK(rig.card.log.empty());
  rig.tap(Button::F7);
  rig.settle();
  NEED(rig.app->state().file.lead() == 1);
  CHECK(rig.on_recent_row());
  CHECK(rig.name_at_cursor() == "beta");
  rig.tap(Button::Delete);
  rig.tap(Button::Delete);
  rig.settle();
  CHECK(rig.card.has("Projects/beta/.deleted"));
  CHECK(rig.names() == Names{"alpha"});
  CHECK(rig.app->state().file.lead() == 0);
}

// ---- RECENT -----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("recent: the last five projects opened or saved are listed first, newest first, then the A-Z list; the group is hidden when there are none") {
  Rig rig;
  for (const char* n : {"a", "b", "c", "d", "e", "f", "g"}) {
    rig.add_project(n);
  }
  rig.open_file_screen();
  CHECK(rig.app->state().file.lead() == 0);  // none yet: no group
  CHECK(rig.app->state().file.pos() == 0);
  for (const char* n : {"A", "B", "C", "D", "E", "F"}) {
    rig.saves.note_opened((std::string("Projects/") + n).c_str());
  }
  rig.passes(3);
  NEED(rig.app->state().file.lead() == 5);  // the cap
  CHECK(rig.app->state().file.rows(rig.index()) == 5 + 7);
  Names shown;
  for (int row = 0; row < 5; ++row) {
    shown.push_back(rig.index().name(static_cast<unsigned>(rig.app->state().file.entry_at(row, rig.index()))));
  }
  CHECK(shown == Names{"f", "e", "d", "c", "b"});  // A left
  CHECK(rig.on_recent_row());
  CHECK(rig.name_at_cursor() == "f");
  CHECK(rig.app->state().file.pos() == 0);  // the cursor starts on the first of them
  // Down walks the group, then the A-Z list from its first row
  for (int i = 0; i < 4; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.name_at_cursor() == "b");
  rig.tap(Button::Down);
  CHECK_FALSE(rig.on_recent_row());
  CHECK(rig.name_at_cursor() == "a");
  CHECK(rig.app->state().file.pos() == 5);
  // saved: newest first
  rig.saves.note_opened("Projects/G");
  rig.passes(3);
  CHECK(rig.index().name(static_cast<unsigned>(rig.app->state().file.entry_at(0, rig.index()))) == std::string("g"));
  CHECK(rig.app->state().file.lead() == 5);
  CHECK(rig.name_at_cursor() == "a");  // the cursor stayed on its entry in the A-Z list
  CHECK(rig.card.violations == 0);
}

TEST_CASE("recent: a recent row is the same folder as its A-Z row: F1 on either opens it; the letter keys jump from the group to the list and back") {
  Rig rig;
  for (const char* n : {"alpha", "beta", "gamma"}) {
    rig.add_project(n);
  }
  rig.saves.note_opened("Projects/Gamma");
  rig.saves.note_opened("Projects/Beta");
  rig.open_file_screen();
  NEED(rig.app->state().file.lead() == 2);
  CHECK(rig.name_at_cursor() == "beta");
  rig.tap(Button::F1);
  CHECK(rig.project.last_folder == "Projects/beta");
  rig.project.is_busy = false;
  rig.pass();
  rig.expire();
  CHECK(rig.app->state().screen == ScreenId::Pattern);  // OS-OPENTOPAT
  rig.go(ScreenId::File);
  rig.tap(Button::Down);
  CHECK(rig.name_at_cursor() == "gamma");
  CHECK(rig.on_recent_row());
  rig.tap(Button::Right);  // from the group: the list's first row
  CHECK_FALSE(rig.on_recent_row());
  CHECK(rig.app->state().file.pos() == 2);
  CHECK(rig.name_at_cursor() == "alpha");
  rig.tap(Button::Right);  // the next letter of the list
  CHECK(rig.name_at_cursor() == "beta");
  rig.tap(Button::Left);  // (at the start of its group: the one before)
  CHECK(rig.name_at_cursor() == "alpha");
  rig.tap(Button::Left);  // from the list's first row: back to the group's first
  CHECK(rig.app->state().file.pos() == 0);
  CHECK(rig.on_recent_row());
  // A-Z row of the same folder
  rig.cursor_to("gamma");
  CHECK_FALSE(rig.on_recent_row());
  rig.tap(Button::F1);
  CHECK(rig.project.last_folder == "Projects/gamma");
}

TEST_CASE("recent: the file is read when the projects are listed; rows of folders that are gone are skipped and dropped") {
  Rig rig;
  for (const char* n : {"alpha", "beta"}) {
    rig.add_project(n);
  }
  rig.card.make_dir_now("Settings");
  rig.card.put("Settings/recent.o3rc", std::string("O3RC 1\nBeta\nGone\nAlpha\nNot A Project\n"));
  rig.card.make_dir_now("Projects/Not A Project");  // a folder with no project in it is not in the list
  rig.open_file_screen();
  NEED(rig.app->state().file.lead() == 2);
  Names shown;
  for (int row = 0; row < 2; ++row) {
    shown.push_back(rig.index().name(static_cast<unsigned>(rig.app->state().file.entry_at(row, rig.index()))));
  }
  CHECK(shown == Names{"beta", "alpha"});
  CHECK(rig.saves.recent().count() == 2u);  // Gone and Not A Project were dropped from the list
  CHECK(rig.file_text("Settings/recent.o3rc") == "O3RC 1\nBeta\nGone\nAlpha\nNot A Project\n");  // ... and the file is rewritten by the next change, not for this
  rig.saves.note_opened("Projects/Alpha");
  rig.settle();
  CHECK(rig.file_text("Settings/recent.o3rc") == "O3RC 1\nAlpha\nBeta\n");
}

TEST_CASE("recent: a damaged, foreign or newer file is an empty list: nothing is listed, nothing crashes, the projects are all there") {
  for (const char* junk : {"", "O3RC 2\nBeta\n", "junk", "O3RC 1\n\xFF\xFE\x01\n", "O3RC 1", "\x00\x00\x00"}) {
    Rig rig;
    rig.add_project("alpha");
    rig.card.make_dir_now("Settings");
    rig.card.put("Settings/recent.o3rc", std::string(junk));
    rig.open_file_screen();
    CHECK(rig.app->state().file.lead() == 0);
    CHECK(rig.names() == Names{"alpha"});
    CHECK(rig.saves.recent().count() == 0u);
  }
}

TEST_CASE("recent: the .bak is read when the file is missing (a write cut between its two renames); with neither, the list is empty") {
  {
    Rig rig;
    rig.add_project("alpha");
    rig.card.make_dir_now("Settings");
    rig.card.put("Settings/recent.o3rc.bak", std::string("O3RC 1\nAlpha\n"));
    rig.open_file_screen();
    CHECK(rig.app->state().file.lead() == 1);
  }
  Rig none;
  none.add_project("alpha");
  none.open_file_screen();
  CHECK(none.app->state().file.lead() == 0);
  CHECK(none.saves.recent().count() == 0u);
}

TEST_CASE("recent: a project opened is the newest, a project saved too; both are written through the atomic save, quietly, with a backup of the file before") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta");
  rig.open_file_screen();
  rig.open_project("beta");
  rig.settle();
  CHECK(rig.file_text("Settings/recent.o3rc") == "O3RC 1\nbeta\n");
  CHECK_FALSE(rig.card.has("Settings/recent.o3rc.bak"));
  CHECK(rig.message().empty());  // quiet
  rig.open_project("alpha");
  rig.settle();
  CHECK(rig.file_text("Settings/recent.o3rc") == "O3RC 1\nalpha\nbeta\n");
  CHECK(rig.file_text("Settings/recent.o3rc.bak") == "O3RC 1\nbeta\n");
  CHECK(rig.message().empty());
  // a Save As of a new project
  rig.tap(Button::F5);
  rig.typed("Mine");
  rig.tap(Button::F8);
  rig.settle();
  CHECK(rig.app->state().screen == ScreenId::File);  // OS-OPENTOPAT: a save does not switch (only an open and NEW do)
  CHECK(rig.file_text("Settings/recent.o3rc") == "O3RC 1\nMine\nalpha\nbeta\n");
  CHECK(rig.card.violations == 0);
  for (const std::string& removed : rig.card.removed()) {  // only our own previous backup and temps, as with the settings
    CHECK_MESSAGE((removed.find(".bak") != std::string::npos || removed.find("_o3save.tmp") != std::string::npos), removed);
  }
  // the list on screen
  rig.tap(Button::F2);
  rig.settle();
  CHECK(rig.app->state().file.lead() == 3);
}

TEST_CASE("recent: a write that fails is silent and costs only the file; the list in memory stays, and the next change writes it") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta");
  rig.open_file_screen();
  rig.card.cut_at = rig.card.ops;  // the card is gone from now on
  rig.saves.note_opened("Projects/Alpha");
  rig.passes(40);
  CHECK(rig.message().empty());  // not a word on the strip
  CHECK(rig.saves.recent().count() == 1u);
  CHECK_FALSE(rig.card.has("Settings/recent.o3rc"));
  CHECK(rig.app->session().jobs.occupied() == 0u);
  rig.card.revive();
  rig.saves.note_opened("Projects/Beta");
  rig.settle();
  CHECK(rig.file_text("Settings/recent.o3rc") == "O3RC 1\nBeta\nAlpha\n");
}

TEST_CASE("recent: the write of the file never holds the user up: a save, a delete or an open that comes while it runs is not refused, the write stops and the list is written when the card is idle") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta");
  rig.open_file_screen();
  rig.saves.note_opened("Projects/alpha");
  rig.pass();  // the write of the list starts
  NEED(rig.app->session().jobs.occupied() == 1u);
  CHECK_FALSE(rig.saves.busy());  // nothing waits for it
  rig.tap(Button::F4);            // SAVE: the project is saved as the next free Track NN, the list's write is given up
  CHECK_FALSE(rig.message() == "A SAVE IS ALREADY RUNNING");
  rig.settle();
  CHECK(rig.message() == "SAVE TRACK 01 DONE");
  CHECK(rig.card.same("Projects/Track 01/project.o3pj", rig.native_bytes()));
  CHECK(rig.file_text("Settings/recent.o3rc") == "O3RC 1\nTrack 01\nalpha\n");  // written once the card was idle
  CHECK_FALSE(rig.card.has("Settings/_o3save.tmp"));  // the cancelled write left no temporary file
  CHECK(rig.card.violations == 0);
  CHECK(rig.message().find("CANCEL") == std::string::npos);
  // a delete asked for while the list is being written goes through
  rig.expire();
  rig.tap(Button::F3);  // NEW: the document has no folder any more, so alpha is not the open project and beta can go
  rig.project.is_busy = false;
  rig.pass();
  rig.expire();
  rig.saves.note_opened("Projects/beta");
  rig.pass();
  NEED(rig.app->session().jobs.occupied() >= 1u);
  rig.cursor_to("beta", true);
  rig.tap(Button::Delete);
  rig.tap(Button::Delete);
  CHECK(rig.card.has("Projects/beta/.deleted"));
  rig.settle();
  CHECK(rig.file_text("Settings/recent.o3rc") == "O3RC 1\nTrack 01\nalpha\n");  // beta was deleted and left the list
  CHECK(rig.card.violations == 0);
}

TEST_CASE("recent: a card that may not be written keeps the list in memory and writes nothing") {
  Rig ro;
  ro.add_project("alpha");
  ro.card.gate_status = hal::storage::WriteStatus::WriteProtected;
  ro.open_file_screen();
  ro.saves.note_opened("Projects/Alpha");
  ro.passes(40);
  CHECK(ro.saves.recent().count() == 1u);
  CHECK(ro.card.files.count("settings/recent.o3rc") == 0u);
  CHECK(ro.card.log.empty());
  CHECK(ro.message().empty());
}

// ---- PURGE ------------------------------------------------------------------------------------------------------------------------------------

namespace {

// A sample instrument in `slot` ("samples/instrNN.wav") whose audio sits in the bank.
void put_sample(Rig& rig, unsigned slot, std::uint32_t frames = 300, int seed = 100) {
  model::Project& project = test::arena(1).project;
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  std::snprintf(in.sample_path, sizeof in.sample_path, "samples/instr%02u.wav", slot + 1u);
  std::snprintf(in.name, sizeof in.name, "Sample %u", slot + 1u);
  NEED(project.set_instrument(slot, in) == model::Status::Ok);
  std::int16_t* pcm = rig.memory.data() + slot * 4000u;
  for (std::uint32_t i = 0; i < frames; ++i) {
    pcm[i] = static_cast<std::int16_t>(seed + static_cast<int>(i % 200u) * 7);
  }
  rig.bank.adopt(slot, pcm, frames);
}

void use(unsigned slot, unsigned track = 0, unsigned at = 0) {
  model::Project& project = test::arena(1).project;
  if (project.pattern(1) == nullptr) {
    NEED(project.create_pattern(1, 16) == model::Status::Ok);
  }
  model::Step s = model::kEmptyStep;
  s.note = model::kNoteFirst + 48;
  s.instr = static_cast<std::uint8_t>(slot + 1u);
  NEED(project.set_step(1, track, at, s) == model::Status::Ok);
}

Bytes wav_of(const Rig& rig, unsigned slot) {
  const app::SampleBank::Pcm pcm = rig.bank.pcm(slot);
  Bytes out(app::WavSaveSource::kHeaderBytes);
  app::WavSaveSource::make_header(out.data(), pcm.frames, 1, 44100);
  const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(pcm.data);
  out.insert(out.end(), raw, raw + pcm.frames * 2u);
  return out;
}

// Four sample instruments, slots 0 and 2 played, 1 and 3 not; saved as "Mine" (Save As), so its folder holds the four files; the history starts here. `expire_news` false leaves the save's message
// on the strip (OS-BUG-PROMPTQ).
void purge_setup(Rig& rig, bool expire_news = true) {
  for (unsigned slot = 0; slot < 4; ++slot) {
    put_sample(rig, slot, 300 + slot * 10, 100 * static_cast<int>(slot + 1));
  }
  use(0, 0, 0);
  use(2, 1, 4);
  rig.go(ScreenId::File);
  rig.tap(Button::F5);
  rig.typed("Mine");
  rig.tap(Button::F8);
  rig.settle();
  if (expire_news) {
    rig.expire();
  }
  NEED(rig.card.has("Projects/Mine/samples/instr04.wav"));
  rig.editor->history_reset();
}

bool instrument_there(unsigned slot) { return test::arena(1).project.instrument(slot) != nullptr; }

}  // namespace

TEST_CASE("purge: Shift + F7 counts the unused sample instruments, asks, and on the second press clears them all in ONE undo unit; undo brings every one back, redo does it again") {
  Rig rig;
  purge_setup(rig);
  CHECK(rig.app->state().screen == ScreenId::File);
  const unsigned depth = rig.journal.depth();
  rig.tap(Button::F7, true);
  CHECK(rig.message() == "PURGE 2 UNUSED INSTRUMENTS? SHIFT+F7 AGAIN");
  CHECK(rig.app->state().file.confirm_purge);
  CHECK(instrument_there(1));
  CHECK(instrument_there(3));
  CHECK(rig.journal.depth() == depth);  // asking records nothing
  {  // OS-KEYROWDLG: Shift + F7 reads PURGE? (inverted) on the Shift layer; the plain F7 is still EXPORT
    const test::RowRead shifted = test::read_row(*rig.app, true);
    CHECK(shifted.label[6] == "PURGE?");
    CHECK(shifted.hot == 0x40u);
    CHECK_FALSE(test::read_row(*rig.app).marked);
  }
  rig.expire();
  rig.tap(Button::F7, true);
  CHECK(rig.message() == "PURGED 2 INSTRUMENTS - F8 ON PARAMS UNDOES");
  CHECK_FALSE(rig.app->state().file.confirm_purge);
  CHECK(instrument_there(0));
  CHECK_FALSE(instrument_there(1));
  CHECK(instrument_there(2));
  CHECK_FALSE(instrument_there(3));
  CHECK(rig.journal.depth() == depth + 1u);  // one unit
  CHECK(rig.card.violations == 0);
  // undo: both come back, with their names and sample paths (F8 on Params: the File screen has no undo of its own)
  rig.tap(Button::Params);
  (void)rig.app->invoke(core::action_id("edit.undo"));
  CHECK(rig.journal.depth() == depth);
  NEED(instrument_there(1));
  NEED(instrument_there(3));
  CHECK(std::string(test::arena(1).project.instrument(1)->name) == "Sample 2");
  CHECK(std::string(test::arena(1).project.instrument(3)->sample_path) == "samples/instr04.wav");
  (void)rig.app->invoke(core::action_id("edit.redo"));
  CHECK_FALSE(instrument_there(1));
  CHECK_FALSE(instrument_there(3));
  // nothing of it reached the card: the files are all where they were
  for (unsigned n = 1; n <= 4; ++n) {
    char path[48];
    std::snprintf(path, sizeof path, "Projects/Mine/samples/instr%02u.wav", n);
    CHECK(rig.card.has(path));
  }
}

// OS-BUG-PROMPTQ: the question takes the head of the strip at once, ahead of the save's message, so the second press (and the key row) answer it without waiting the message out.
TEST_CASE("purge: the question is the head of the strip at once, ahead of the SAVE AS message; the key row marks Shift + F7 and the second press purges") {
  Rig rig;
  purge_setup(rig, false);
  NEED(!rig.message().empty());  // the save's message still stands
  CHECK(rig.message().rfind("PURGE", 0) != 0);
  rig.tap(Button::F7, true);
  CHECK(rig.message() == "PURGE 2 UNUSED INSTRUMENTS? SHIFT+F7 AGAIN");
  const test::RowRead shifted = test::read_row(*rig.app, true);
  CHECK(shifted.label[6] == "PURGE?");
  CHECK(shifted.hot == 0x40u);
  rig.tap(Button::F7, true);
  CHECK(rig.message() == "PURGED 2 INSTRUMENTS - F8 ON PARAMS UNDOES");
  CHECK_FALSE(instrument_there(1));
  CHECK_FALSE(instrument_there(3));
}

TEST_CASE("purge: nothing unused says so; a move or another key between the two presses takes the question back; module instruments are kept and counted; the IMPORT and EXPORT pages ignore the key") {
  Rig rig;
  for (unsigned slot = 0; slot < 2; ++slot) {
    put_sample(rig, slot);
    use(slot, slot, 0);
  }
  rig.go(ScreenId::File);
  rig.tap(Button::F7, true);
  CHECK(rig.message() == "NO UNUSED INSTRUMENTS");
  CHECK_FALSE(rig.app->state().file.confirm_purge);
  // a module instrument nothing names stays, and is counted
  {
    model::Project& project = test::arena(1).project;
    NEED(project.add_module("tb303", 5, 1, 0) == model::Status::Ok);
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Module);
    std::strcpy(in.name, "TB303");
    in.module_index = 0;
    NEED(project.set_instrument(5, in) == model::Status::Ok);
  }
  rig.expire();
  rig.tap(Button::F7, true);
  CHECK(rig.message() == "NO UNUSED INSTRUMENTS - 1 MODULE KEPT");
  // an unused sample instrument beside it
  put_sample(rig, 7);
  rig.expire();
  rig.tap(Button::F7, true);
  CHECK(rig.message() == "PURGE 1 UNUSED INSTRUMENT? SHIFT+F7 AGAIN");
  rig.tap(Button::Down);  // a move takes the question back
  CHECK_FALSE(rig.app->state().file.confirm_purge);
  rig.expire();
  rig.tap(Button::F7, true);
  rig.tap(Button::F2);  // another key does
  CHECK_FALSE(rig.app->state().file.confirm_purge);
  rig.expire();
  rig.tap(Button::F7, true);
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::File);
  CHECK_FALSE(rig.app->state().file.confirm_purge);
  CHECK(instrument_there(7));
  rig.expire();
  rig.tap(Button::F7, true);
  rig.tap(Button::F7, true);
  CHECK(rig.message() == "PURGED 1 INSTRUMENT - F8 ON PARAMS UNDOES");
  CHECK_FALSE(instrument_there(7));
  CHECK(instrument_there(5));  // the module stayed
  // the other pages: the key is not theirs
  put_sample(rig, 8);
  rig.tap(Button::F6);
  rig.expire();
  rig.tap(Button::F7, true);
  rig.tap(Button::F7, true);
  CHECK(instrument_there(8));
  rig.tap(Button::F6);
  rig.tap(Button::F7);  // EXPORT
  rig.tap(Button::F7, true);
  rig.tap(Button::F7, true);
  CHECK(instrument_there(8));
}

TEST_CASE("purge: what a step, a Random Instrument window and a MIDI Learn binding name is kept") {
  Rig rig;
  for (unsigned slot = 0; slot < 8; ++slot) {
    put_sample(rig, slot);
  }
  model::Project& project = test::arena(1).project;
  use(0, 0, 0);       // a step
  NEED(project.create_pattern(9, 4) == model::Status::Ok);  // a pattern the song does not play
  model::Step s = model::kEmptyStep;
  s.note = 60;
  s.instr = 3;         // slot 2, with a Random Instrument window of one: slots 1..3
  s.fx[0] = {6, 1};
  NEED(project.set_step(9, 4, 2, s) == model::Status::Ok);
  core::LearnBinding b{};
  b.param = core::param_id(core::ParamKind::Instrument, 6, model::instrument_param::Volume);
  b.cc = 30;
  b.mode = static_cast<std::uint8_t>(core::LearnMode::Pickup);
  NEED(project.set_learn_slot(0, b) == model::Status::Ok);
  rig.editor->history_reset();
  rig.go(ScreenId::File);
  rig.tap(Button::F7, true);
  CHECK(rig.message() == "PURGE 3 UNUSED INSTRUMENTS? SHIFT+F7 AGAIN");  // 4, 5 and 7
  rig.tap(Button::F7, true);
  for (unsigned slot : {0u, 1u, 2u, 3u, 6u}) {
    CHECK_MESSAGE(instrument_there(slot), slot);
  }
  for (unsigned slot : {4u, 5u, 7u}) {
    CHECK_MESSAGE(!instrument_there(slot), slot);
  }
}

TEST_CASE("purge touches the document only: the card is not written by it, a SAVE after it writes the project and leaves every file of samples/ where it is, and the saved project no longer names the purged ones") {
  Rig rig;
  purge_setup(rig);
  rig.card.put("Projects/Mine/samples/notes.wav", std::string("a file of the user's, in no instrument"));
  const Bytes kept0 = wav_of(rig, 0);
  const Bytes gone1 = wav_of(rig, 1);
  rig.card.log.clear();
  rig.tap(Button::F7, true);
  rig.tap(Button::F7, true);
  NEED(!instrument_there(1));
  CHECK(rig.card.log.empty());  // not one call on the card
  rig.passes(20);
  CHECK(rig.card.log.empty());
  rig.expire();
  rig.tap(Button::F4);  // SAVE
  rig.settle();
  CHECK(rig.message() == "SAVE MINE DONE");  // no second line about files: nothing is moved
  CHECK(rig.card.same("Projects/Mine/project.o3pj", rig.native_bytes()));
  CHECK(rig.card.same("Projects/Mine/samples/instr01.wav", kept0));
  CHECK(rig.card.same("Projects/Mine/samples/instr02.wav", gone1));  // the purged instruments' files stay (OS-TRASHUI)
  CHECK(rig.card.has("Projects/Mine/samples/instr03.wav"));
  CHECK(rig.card.has("Projects/Mine/samples/instr04.wav"));
  CHECK(rig.card.has("Projects/Mine/samples/notes.wav"));
  CHECK(test::arena(1).project.instrument(1) == nullptr);  // ... and the project does not name them
  CHECK(test::arena(1).project.instrument(3) == nullptr);
  CHECK(rig.card.violations == 0);
  for (const std::string& removed : rig.card.removed()) {
    CHECK_MESSAGE(removed.find(".wav") == std::string::npos, removed);
  }
  for (const std::string& line : rig.card.log) {
    CHECK_MESSAGE(line.find("Trash") == std::string::npos, line);
    CHECK_MESSAGE(line.find("rename Projects/Mine/samples/instr0") == std::string::npos, line);
  }
}

TEST_CASE("purge, undo, SAVE: the instruments are back and the project names their files again; a SAVE AS copies only what the project names") {
  Rig rig;
  purge_setup(rig);
  rig.tap(Button::F7, true);
  rig.tap(Button::F7, true);
  rig.tap(Button::Params);
  (void)rig.app->invoke(core::action_id("edit.undo"));
  NEED(instrument_there(1));
  rig.tap(Button::File);
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE MINE DONE");
  for (unsigned n = 1; n <= 4; ++n) {
    char path[48];
    std::snprintf(path, sizeof path, "Projects/Mine/samples/instr%02u.wav", n);
    CHECK(rig.card.has(path));
  }
  // purge again, then SAVE AS: the new folder gets the audio of the instruments the project still has
  rig.expire();
  rig.tap(Button::F7, true);
  rig.tap(Button::F7, true);
  rig.expire();
  rig.tap(Button::F5);
  rig.typed("Copy");
  rig.tap(Button::F8);
  rig.settle();
  CHECK(rig.card.has("Projects/Copy/samples/instr01.wav"));
  CHECK(rig.card.has("Projects/Copy/samples/instr03.wav"));
  CHECK_FALSE(rig.card.has("Projects/Copy/samples/instr02.wav"));
  CHECK(rig.card.has("Projects/Mine/samples/instr02.wav"));  // the old folder is as it was
  CHECK(rig.card.violations == 0);
}

TEST_CASE("purge: a slot whose audio the instrument clipboard holds is kept (a paste would find it gone); a project that was never saved is purged the same") {
  Rig rig;
  for (unsigned slot = 0; slot < 3; ++slot) {
    put_sample(rig, slot, 300, 50 * static_cast<int>(slot + 1));
  }
  use(0);
  rig.editor->history_reset();
  rig.bank.clip_set(*test::arena(1).project.instrument(1), 1);  // S7 COPY of slot 1's instrument and a reference to its audio
  NEED(rig.bank.clip_valid());
  rig.go(ScreenId::File);
  rig.tap(Button::F7, true);
  CHECK(rig.message() == "PURGE 1 UNUSED INSTRUMENT? SHIFT+F7 AGAIN");  // slot 2 only
  rig.tap(Button::F7, true);
  CHECK(instrument_there(0));
  CHECK(instrument_there(1));
  CHECK_FALSE(instrument_there(2));
  CHECK(rig.card.log.empty());
}


// OS-DELREPEAT: a held Delete is one press: it asks (DELETE AGAIN) and a repeat does not answer its own question. Two presses mark exactly one folder.
TEST_CASE("delete: a held Delete only asks; marking takes a second press, and it marks one folder") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta");
  rig.open_file_screen();
  rig.cursor_to("beta");
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Delete), true));
  for (unsigned t = 0; t < 1500; t += 16) {
    rig.pass();
  }
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Delete), false));
  rig.pass();
  rig.settle();
  CHECK(rig.app->state().file.confirm_delete);
  CHECK_FALSE(rig.card.has("Projects/beta/.deleted"));
  CHECK_FALSE(rig.card.has("Projects/alpha/.deleted"));
  rig.tap(Button::Delete);  // the second press answers
  rig.settle();
  CHECK(rig.card.has("Projects/beta/.deleted"));
  CHECK_FALSE(rig.card.has("Projects/alpha/.deleted"));
}

// ---- OS-QAFIX-E-FILE-10: the project the unit opens at power-on ---------------------------------------------------------------------------------------------------------------------

namespace {
// boot_recent_project on the rig's card: the folder it names ("" for false) and whether it is a native project.
std::string boot_pick(Rig& rig, bool* native = nullptr, std::size_t cap = 48) {
  char folder[48] = "unset";
  bool is_native = true;
  const bool ok = app::boot_recent_project(rig.card, folder, cap, is_native);
  if (native != nullptr) {
    *native = is_native;
  }
  return ok ? std::string(folder) : std::string();
}
}  // namespace

TEST_CASE("boot open: the first RECENT project that is on the card is the one the unit opens; native or OG; nothing otherwise") {
  Rig rig;
  rig.add_project("alpha");
  rig.add_project("beta");
  rig.card.make_dir_now("Settings");
  CHECK(boot_pick(rig).empty());  // no list: the Workspace, as before
  rig.card.put("Settings/recent.o3rc", std::string("O3RC 1\nbeta\nalpha\n"));
  bool native = false;
  CHECK(boot_pick(rig, &native) == "Projects/beta");
  CHECK(native);
  // an OG project (project.mt only)
  rig.card.make_dir_now("Projects/og");
  rig.card.put("Projects/og/project.mt", Bytes(16, 0));
  rig.card.put("Settings/recent.o3rc", std::string("O3RC 1\nog\nalpha\n"));
  CHECK(boot_pick(rig, &native) == "Projects/og");
  CHECK_FALSE(native);
  // both files: the native one is read, as the listing's tag says
  rig.card.put("Projects/og/project.o3pj", rig.native_bytes());
  CHECK(boot_pick(rig, &native) == "Projects/og");
  CHECK(native);
  // a name too long for the caller's buffer
  CHECK(boot_pick(rig, nullptr, 8).empty());
  // the pick reads the card and writes nothing
  CHECK(rig.card.ops == 0u);
}

TEST_CASE("boot open: a first entry that is gone, hidden by a DELETE, or without a project file gives no pick (the Workspace opens), a list that is damaged is empty, a .bak is read only when the file is missing") {
  Rig rig;
  rig.add_project("alpha");
  rig.card.make_dir_now("Settings");
  rig.card.put("Settings/recent.o3rc", std::string("O3RC 1\ngone\nalpha\n"));
  CHECK(boot_pick(rig).empty());  // entry 0 names nothing: not the second one
  rig.card.make_dir_now("Projects/empty");
  rig.card.put("Settings/recent.o3rc", std::string("O3RC 1\nempty\nalpha\n"));
  CHECK(boot_pick(rig).empty());
  rig.card.put("Projects/alpha/.deleted", std::string());
  rig.card.put("Settings/recent.o3rc", std::string("O3RC 1\nalpha\n"));
  CHECK(boot_pick(rig).empty());
  rig.card.files.erase("projects/alpha/.deleted");
  CHECK(boot_pick(rig) == "Projects/alpha");
  // a list that is damaged next to a good .bak: empty (the .bak is read only when the file is NotFound)
  rig.card.put("Settings/recent.o3rc", std::string("not a list \x01\x02"));
  rig.card.put("Settings/recent.o3rc.bak", std::string("O3RC 1\nalpha\n"));
  CHECK(boot_pick(rig).empty());
  rig.card.files.erase("settings/recent.o3rc");  // missing: the .bak stands
  CHECK(boot_pick(rig) == "Projects/alpha");
  // the card cannot say: false, nothing picked
  rig.card.open_status = hal::storage::Status::NoHandle;
  CHECK(boot_pick(rig).empty());
}

TEST_CASE("boot open: App::boot_open starts the load as File's F1 does, tells OPENED, leaves recent.o3rc byte for byte and keeps the other four entries") {
  Rig rig;
  const char* names[] = {"one", "two", "three", "four", "five"};
  for (const char* n : names) {
    rig.add_project(n);
  }
  rig.card.make_dir_now("Settings");
  const std::string list = "O3RC 1\none\ntwo\nthree\nfour\nfive\n";
  rig.card.put("Settings/recent.o3rc", list);
  char folder[48];
  bool native = false;
  NEED(app::boot_recent_project(rig.card, folder, sizeof folder, native));
  CHECK(rig.app->boot_open(folder, native));
  CHECK(rig.project.last_folder == "Projects/one");
  CHECK(rig.project.last_format == app::ProjectSink::Format::Native);
  CHECK(rig.app->state().file.opening);
  rig.passes(3);  // the load runs
  CHECK(rig.app->state().file.opening);
  rig.project.is_busy = false;
  rig.pass();
  CHECK_FALSE(rig.app->state().file.opening);
  CHECK(rig.message() == "PROJECT one OPENED");
  rig.passes(400);  // not settle(): the boot opens on the Pattern screen, where no listing ever scans; 400 passes are far more than a recent write needs
  rig.expire();
  CHECK(rig.file_text("Settings/recent.o3rc") == list);  // the newest was the newest already: no write
  CHECK_FALSE(rig.card.has("Settings/recent.o3rc.bak"));
  CHECK(rig.saves.recent().count() == 5u);
  // a load already running, or no sink: nothing starts
  rig.project.is_busy = false;
  rig.project.open_result = app::ProjectSink::Start::Busy;
  CHECK_FALSE(rig.app->boot_open(folder, native));
  CHECK_FALSE(rig.app->state().file.opening);
}

TEST_CASE("boot open: a last project that cannot load is refused and the strip says why; the open one is not replaced") {
  Rig rig;
  rig.add_project("broken");
  Bytes cut = rig.native_bytes();
  cut.resize(cut.size() - 20);
  rig.card.put("Projects/broken/project.o3pj", cut);
  rig.card.make_dir_now("Settings");
  rig.card.put("Settings/recent.o3rc", std::string("O3RC 1\nbroken\n"));
  const Bytes before = rig.native_bytes();
  char folder[48];
  bool native = false;
  NEED(app::boot_recent_project(rig.card, folder, sizeof folder, native));  // a damaged file is not a reason to skip the entry: the maintainer's rule, and the load says why
  // the load the platform runs checks the file before it replaces anything (OS-QAFIX-4): this file is refused, with these words
  app::ProjectVerify verify;
  NEED(verify.begin(rig.card, folder, !native, false));
  for (unsigned i = 0; i < 10000u && verify.step(600) == app::ProjectVerify::State::Running; ++i) {
  }
  CHECK(verify.state() == app::ProjectVerify::State::Refused);
  NEED(rig.app->boot_open(folder, native));
  rig.project.is_busy = false;
  rig.project.ok = false;
  rig.project.was_refused = true;
  rig.project.words = verify.detail();
  rig.pass();
  CHECK(rig.message() == "PROJECT broken FAILED: THE FILE IS CUT SHORT");
  CHECK(rig.native_bytes() == before);  // the document is as it booted
  // the app without a sink: nothing starts
  auto bare = std::make_unique<app::App>(rig.input, rig.clock, rig.display);
  CHECK_FALSE(bare->boot_open("Projects/broken", true));
}

// OS-BUG-LATENT-1106: the toast of a project that was replaced did not leave the strip with it. QA-INS-281 (a Kit card opened after instrument 2 of the first project said BEAT SLICE: NO SLICES) showed
// that one over instrument 3 of the new project, which has three slices: the text was true when it was posted, about a project that is gone.
TEST_CASE("open: news posted about the project that is replaced does not outlive the load; the strip says OPENED alone") {
  Rig rig;
  rig.add_project("alpha");
  rig.open_file_screen();
  rig.cursor_to("alpha");
  rig.project.last_folder.clear();
  auto& notify = const_cast<core::NotifyQueue&>(rig.app->state().notify);
  CHECK(notify.post(core::Severity::Info, "BEAT SLICE: NO SLICES, AUTO OR EVEN ON WAVE", "playmode", app::ViewState::kMessageMs));
  rig.tap(Button::F1);
  rig.project.is_busy = false;
  rig.pass();
  CHECK(rig.message() == "PROJECT alpha OPENED");
  CHECK(notify.size() == 1u);  // nothing of the old project is queued behind it either
}

// OS-MIDIREC-P1 S2: a load that dropped newer fields or MIDI lanes tells the user (the next SAVE will not write them back), as a warning.
TEST_CASE("open: newer fields or skipped MIDI lanes are said after OPENED, as a warning") {
  struct Case {
    bool newer;
    std::uint32_t lanes;
    const char* want;
  };
  const Case cases[] = {{false, 0, "PROJECT alpha OPENED"},
                        {true, 0, "PROJECT alpha OPENED, NEWER FIELDS LOST"},
                        {false, 1, "PROJECT alpha OPENED, 1 MIDI LANE LOST"},
                        {false, 3, "PROJECT alpha OPENED, MIDI LANES LOST"},
                        {true, 2, "PROJECT alpha OPENED, NEWER DATA LOST"}};
  for (const Case& c : cases) {
    Rig rig;
    rig.add_project("alpha");
    rig.open_file_screen();
    rig.cursor_to("alpha");
    rig.project.newer = c.newer;
    rig.project.lanes_lost = c.lanes;
    rig.tap(Button::F1);
    rig.project.is_busy = false;
    rig.pass();
    CHECK(rig.message() == c.want);
    const auto& notify = rig.app->state().notify;
    NEED(notify.size() >= 1u);
    CHECK((notify.head().severity == core::Severity::Warn) == (c.newer || c.lanes != 0u));
  }
}

// OS-PTOOLS2 S2 review fold: as NEW (save_app_test.cpp): F6 REPEAT does not carry project A's last fill or TOOLS apply over to project B.
TEST_CASE("open: the last fill and the last TOOLS apply (what F6 repeats) do not outlive the project that is replaced") {
  Rig rig;
  rig.add_project("alpha");
  rig.open_file_screen();
  rig.cursor_to("alpha");
  rig.project.last_folder.clear();
  auto& view = const_cast<app::ViewState&>(rig.app->state());
  view.fill.valid = true;
  view.verb_menu.last_verb = app::VerbMenuView::LastVerb::Tool;
  view.verb_menu.tool_last.amount = 12;
  rig.tap(Button::F1);
  rig.project.is_busy = false;
  rig.pass();
  CHECK(rig.message() == "PROJECT alpha OPENED");
  CHECK_FALSE(rig.app->state().fill.valid);
  CHECK(rig.app->state().verb_menu.last_verb == app::VerbMenuView::LastVerb::None);
  CHECK(rig.app->state().verb_menu.tool_last.amount == 0);
}
