// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FWCONFIG slice 2a (docs/specs/OS-FWCONFIG.md; Level C, the firmware update path): the App's side of the update dialog, on the fake sink (platform/host/fake_firmware.h): F8 UPDATE on Config's INFO
// page only; the list copied in one pass; question 1 and question 2, each answered by F1 and taken back by any other control or by the 30 s; the check's progress; every refusal; cancel() on every way
// out; the install reaching the port exactly once; nothing asked of the sink before the second question is answered. The client API behind the real port is platform/mk66/tests/mk66_update_client_test.cpp,
// the fake itself fake_firmware_test.cpp.
#include <doctest/doctest.h>

#include "support/key_row.h"
#include "support/mem_card.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/firmware_view.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/recorder.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/config_screen.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/core/input_event.h>
#include <o3os/core/job.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fake_firmware.h>
#include <o3os/platform/host/fake_storage.h>
#include <o3os/platform/host/fakes.h>

#include <cstring>
#include <memory>
#include <string>

using namespace o3os;
using app::Button;
using app::FirmwareView;
using app::OverlayId;
using app::ScreenId;
using hal::firmware::Refusal;
using platform::host::FakeFirmwareSink;
using Phase = FirmwareView::Phase;
using State = hal::firmware::CheckStatus::State;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  platform::host::FakeStorage card;
  test::MemCard project_card;
  app::SaveService saves;
  FakeFirmwareSink sink;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;

  explicit Rig(bool with_sink = true, bool with_project = false) {
    app->attach_storage(&card);
    app->set_build_info("Track3r test", "0123456789ab");
    if (with_project) {
      model::ModelArena& arena = test::arena(1);
      test::fill_small(arena.project);
      (void)arena.project.set_name("", 0);
      journal.begin(g_ring, sizeof g_ring);
      editor = std::make_unique<model::Editor>(arena.project, journal);
      app->attach_document(editor.get());
      saves.attach(project_card, project_card);
      app->attach_saves(&saves);
    }
    sink.add("TRACKER_1.PTF", 812345);
    sink.add("TRACKER_2.PTF", 799000);
    sink.add("OLD.PTF", 640000);
    if (with_sink) {
      app->attach_firmware(&sink);
    }
    tap(Button::Config);
    tap(Button::F4);  // the INFO page
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void passes(unsigned n, unsigned ms = 16) {
    for (unsigned i = 0; i < n; ++i) {
      pass(ms);
    }
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void tap(Button b) {
    press(b);
    release(b);
    pass();
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass();
  }
  void pad(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 100));
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass();
  }
  const FirmwareView& fw() const { return app->state().firmware; }
  OverlayId overlay() const { return app->state().overlay; }
  app::ViewState& view() { return const_cast<app::ViewState&>(app->state()); }  // (a test steers a state that only a running export or recording would)
  std::string keys() const {
    char buf[app::kKeysLineChars + 1];
    const std::size_t n = app::describe_keys(app->model(), app->state(), buf, sizeof buf);
    return std::string(buf, n);
  }
  std::string strip() const { return app->state().notify.last_text(); }

  // The path to question 2: open (unless the list is up), F1 (question 1), F1 (the check), passes until the verdict.
  void to_question_two() {
    if (!fw().open()) {
      tap(Button::F8);
    }
    tap(Button::F1);
    tap(Button::F1);
    passes(sink.kDefaultSlices + 1);
  }
};

}  // namespace

TEST_CASE("firmware: F8 on the INFO page opens the list, copied in one pass of the loop; no other page does, and a build with no port shows no key") {
  Rig rig;
  const app::Model& model = rig.app->model();
  CHECK(model.firmware);
  CHECK(std::string(app::config_key_row(model.firmware, app::kConfigPageInfo)[7]) == "UPDATE");
  CHECK(std::string(app::config_key_row(model.firmware, 0)[7]) == "--");  // F8 only, and on INFO only: F1..F6 stay the pages
  for (unsigned i = 0; i < 7; ++i) {
    CHECK(std::string(app::config_key_row(true, app::kConfigPageInfo)[i]) == app::kConfigSoftKeys[i]);
  }
  rig.tap(Button::F8);
  CHECK(rig.overlay() == OverlayId::FirmwareUpdate);
  CHECK(rig.fw().phase == Phase::List);
  NEED(rig.fw().n == 3);
  CHECK(std::string(rig.fw().files[0].name) == "TRACKER_1.PTF");
  CHECK(rig.fw().files[0].size == 812345u);
  CHECK(rig.fw().files[0].reset_target);
  CHECK_FALSE(rig.fw().files[1].reset_target);
  CHECK(std::string(rig.fw().files[2].name) == "OLD.PTF");
  CHECK(rig.sink.count("list_begin") == 1u);  // one pass: begin, every entry, end
  CHECK(rig.sink.count("list_end") == 1u);
  CHECK(rig.sink.calls.size() == 2u);          // nothing else was asked
  CHECK(rig.keys() == "F1:UPDATE,F8:BACK");
  // The same key on the other pages does nothing.
  rig.tap(Button::F8);  // BACK
  CHECK(rig.overlay() == OverlayId::None);
  rig.sink.calls.clear();
  for (const Button page : {Button::F1, Button::F2, Button::F3, Button::F5, Button::F6}) {
    rig.tap(page);
    rig.tap(Button::F8);
    CHECK(rig.overlay() == OverlayId::None);
  }
  CHECK(rig.sink.calls.empty());
  // A build with no update path: no label, no dialog.
  Rig none(false);
  CHECK_FALSE(none.app->model().firmware);
  CHECK(std::string(app::config_key_row(none.app->model().firmware, app::kConfigPageInfo)[7]) == "--");
  none.tap(Button::F8);
  CHECK(none.overlay() == OverlayId::None);
}

TEST_CASE("firmware: an empty Firmware folder says so, F1 has nothing to choose, BACK closes it and cancels; more files than the list holds are flagged") {
  Rig rig;
  rig.sink.clear_files();
  rig.tap(Button::F8);
  CHECK(rig.fw().phase == Phase::List);
  CHECK(rig.fw().n == 0);
  CHECK(rig.keys() == "F8:BACK");  // UPDATE is not offered
  rig.tap(Button::F1);
  CHECK(rig.fw().phase == Phase::List);
  rig.sink.calls.clear();
  rig.tap(Button::F8);
  CHECK(rig.overlay() == OverlayId::None);
  CHECK_FALSE(rig.fw().open());
  CHECK(rig.sink.count("cancel") == 1u);  // (every way out of the dialog)

  for (unsigned i = 0; i < FirmwareView::kMax + 3; ++i) {
    rig.sink.add("F" + std::to_string(i) + ".PTF", 1000u + i);
  }
  rig.tap(Button::F8);
  CHECK(rig.fw().n == FirmwareView::kMax);
  CHECK(rig.fw().entries == FirmwareView::kMax + 3);  // (the window holds 12, the folder's count is known)
  CHECK(rig.sink.count("list_end") == 1u);  // (the pass is closed all the same; the calls were cleared before)
  rig.tap(Button::F8);
}

TEST_CASE("firmware: a folder of 19 files is paged through: every entry can be chosen, the window of 12 follows the cursor, and the position is shown") {
  Rig rig;
  rig.sink.clear_files();
  for (unsigned i = 0; i < 19; ++i) {
    rig.sink.add("F" + std::to_string(i) + ".PTF", 1000u + i);
  }
  rig.tap(Button::F8);
  NEED(rig.fw().n == FirmwareView::kMax);  // (no SRAM growth: the window stays 12)
  CHECK(rig.fw().entries == 19);
  CHECK(rig.fw().first == 0);
  CHECK(rig.sink.count("list_begin") == 1u);
  CHECK(rig.sink.count("list_end") == 1u);
  CHECK(rig.fw().files[0].reset_target);
  for (unsigned i = 0; i < 11; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.fw().cursor == 11);
  CHECK(rig.sink.count("list_begin") == 1u);  // (inside the window: no scan)
  rig.tap(Button::Down);  // past the last entry of a full window: the next window
  CHECK(rig.sink.count("list_begin") == 2u);
  CHECK(rig.sink.count("list_end") == 2u);
  CHECK(rig.fw().first == 12);
  CHECK(rig.fw().n == 7);
  CHECK(rig.fw().entries == 19);
  CHECK(rig.fw().cursor == 0);
  CHECK(std::string(rig.fw().chosen()) == "F12.PTF");
  CHECK_FALSE(rig.fw().files[0].reset_target);  // (only the first file of the folder is the reset target)
  for (unsigned i = 0; i < 5; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(std::string(rig.fw().chosen()) == "F17.PTF");  // entry 18
  rig.tap(Button::F1);
  CHECK(rig.fw().phase == Phase::AskUpdate);
  CHECK(std::string(rig.fw().chosen()) == "F17.PTF");
  rig.tap(Button::F1);
  CHECK(rig.fw().phase == Phase::Checking);
  CHECK(rig.sink.count("begin_check F17.PTF") == 1u);
  rig.tap(Button::F8);  // CANCEL: the list again, on the same entry
  CHECK(rig.fw().phase == Phase::List);
  CHECK(std::string(rig.fw().chosen()) == "F17.PTF");
  rig.tap(Button::Down);  // the last, and no further
  CHECK(std::string(rig.fw().chosen()) == "F18.PTF");
  rig.tap(Button::Down);
  CHECK(std::string(rig.fw().chosen()) == "F18.PTF");
  // Up past the first entry of the second window: the window before, on its last entry.
  for (unsigned i = 0; i < 6; ++i) {
    rig.tap(Button::Up);
  }
  CHECK(std::string(rig.fw().chosen()) == "F12.PTF");
  rig.tap(Button::Up);
  CHECK(rig.fw().first == 0);
  CHECK(rig.fw().n == FirmwareView::kMax);
  CHECK(std::string(rig.fw().chosen()) == "F11.PTF");
  // A big move reaches any entry at once, in either direction (the encoder accelerates in a list: any turn this size ends at the end).
  rig.turn(100);
  CHECK(std::string(rig.fw().chosen()) == "F18.PTF");
  rig.turn(-100);
  CHECK(std::string(rig.fw().chosen()) == "F0.PTF");
  CHECK(rig.fw().first == 0);
  CHECK(rig.fw().files[rig.fw().cursor].reset_target);
  rig.tap(Button::F8);
  CHECK(rig.overlay() == OverlayId::None);
}

TEST_CASE("firmware: a window that cannot be loaded keeps the list as it was and says why; a folder that shrank is followed") {
  Rig rig;
  rig.sink.clear_files();
  for (unsigned i = 0; i < 19; ++i) {
    rig.sink.add("F" + std::to_string(i) + ".PTF", 1000u + i);
  }
  rig.tap(Button::F8);
  rig.sink.list_refusal = Refusal::StorageBusy;
  rig.turn(100);  // the last entry is in the second window, which the busy card cannot give
  CHECK(rig.fw().phase == Phase::List);
  CHECK(rig.fw().first == 0);
  CHECK(rig.fw().n == FirmwareView::kMax);
  CHECK(std::string(rig.fw().msg.c_str()) == "THE CARD IS BUSY: TRY AGAIN");
  CHECK(rig.fw().bad);
  rig.sink.list_refusal = Refusal::None;
  rig.sink.clear_files();  // the folder shrank to 14 files while the dialog was up
  for (unsigned i = 0; i < 14; ++i) {
    rig.sink.add("G" + std::to_string(i) + ".PTF", 1000u + i);
  }
  rig.turn(100);
  CHECK(rig.fw().first == 12);
  CHECK(rig.fw().n == 2);
  CHECK(rig.fw().entries == 14);
  CHECK(std::string(rig.fw().chosen()) == "G13.PTF");  // the end it asked for (entry 19) is past the folder: the last entry that exists is 14
  rig.sink.clear_files();
  for (unsigned i = 0; i < 5; ++i) {
    rig.sink.add("H" + std::to_string(i) + ".PTF", 1000u + i);
  }
  rig.tap(Button::Up);  // (G12, the first of the window)
  rig.tap(Button::Up);  // the window before: read from the folder as it is now (5 files), the cursor on the last entry that exists
  CHECK(std::string(rig.fw().chosen()) == "H4.PTF");
  CHECK(rig.fw().entries == 5);
  CHECK(rig.fw().first == 0);
  CHECK(std::string(rig.fw().chosen()) == "H4.PTF");
  rig.tap(Button::F8);
}

TEST_CASE("firmware: Up, Down and the encoder choose the file; question 1 names it, and F1 YES starts the check on exactly that file") {
  Rig rig;
  rig.tap(Button::F8);
  rig.tap(Button::Down);
  CHECK(rig.fw().cursor == 1);
  rig.turn(1);
  CHECK(rig.fw().cursor == 2);
  rig.turn(5);
  CHECK(rig.fw().cursor == 2);  // the last
  rig.tap(Button::Up);
  CHECK(rig.fw().cursor == 1);
  rig.tap(Button::F1);  // UPDATE: question 1
  CHECK(rig.fw().phase == Phase::AskUpdate);
  CHECK(std::string(rig.fw().chosen()) == "TRACKER_2.PTF");
  CHECK(rig.keys() == "F1:YES,F8:CANCEL");
  CHECK(rig.sink.count("begin_check TRACKER_2.PTF") == 0u);  // asked, not started
  rig.tap(Button::F1);  // YES
  CHECK(rig.fw().phase == Phase::Checking);
  CHECK(rig.sink.count("begin_check TRACKER_2.PTF") == 1u);
  CHECK(rig.sink.last_name == "TRACKER_2.PTF");
  CHECK(rig.keys() == "F8:CANCEL");
  CHECK(rig.sink.installs == 0u);
}

TEST_CASE("firmware: question 1 is taken back by any other control (a key, Shift + a key, the encoder, a pad) and by F8; nothing was started") {
  Rig rig;
  const auto ask = [&rig]() {
    rig.tap(Button::F8);
    rig.tap(Button::F1);
    NEED(rig.fw().phase == Phase::AskUpdate);
  };
  const auto closed_again = [&rig](const char* what) {
    CHECK_MESSAGE(rig.fw().phase == Phase::List, std::string(what));
    CHECK_MESSAGE(rig.overlay() == OverlayId::FirmwareUpdate, std::string(what));
    CHECK_MESSAGE(rig.sink.count("begin_check TRACKER_1.PTF") == 0u, std::string(what));
    CHECK_MESSAGE(rig.sink.installs == 0u, std::string(what));
  };
  ask();
  rig.tap(Button::F2);
  closed_again("F2");
  rig.tap(Button::F1);
  CHECK(rig.fw().phase == Phase::AskUpdate);  // (the list answers again)
  rig.tap(Button::Left);
  closed_again("Left");
  rig.tap(Button::F1);
  rig.turn(1);
  closed_again("encoder");
  rig.tap(Button::F1);
  rig.pad(3);
  closed_again("pad");
  rig.tap(Button::F1);
  rig.press(Button::Shift);
  rig.pass(1);
  CHECK(rig.fw().phase == Phase::AskUpdate);  // Shift alone is a modifier, not another key
  rig.tap(Button::F2);                        // Shift + F2 is
  closed_again("Shift + F2");
  rig.tap(Button::F1);                        // (Shift is still down: the list's F1 is the plain key's, Shift + F1 is bound to nothing)
  CHECK(rig.fw().phase == Phase::List);
  CHECK(rig.sink.count("begin_check TRACKER_1.PTF") == 0u);
  rig.release(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F1);
  NEED(rig.fw().phase == Phase::AskUpdate);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F1);                        // Shift + F1 answers nothing: it takes the question back, it is not YES
  closed_again("Shift + F1");
  rig.release(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F1);
  rig.tap(Button::F8);  // CANCEL: back to the list, not out of the dialog
  closed_again("F8");
  rig.tap(Button::F8);
  CHECK(rig.overlay() == OverlayId::None);
}

TEST_CASE("firmware: the check runs a slice a pass with progress, F8 CANCEL drops it, a verdict of Checked asks question 2 with the length and the CRC") {
  Rig rig;
  rig.tap(Button::F8);
  rig.tap(Button::F1);
  rig.tap(Button::F1);
  NEED(rig.fw().phase == Phase::Checking);
  CHECK(rig.fw().phase == Phase::Checking);  // (the pass that took F1 also ran the first slice)
  CHECK(rig.fw().total == 812345u);
  const std::uint32_t first = rig.fw().done;
  CHECK(first > 0u);
  CHECK(first < rig.fw().total);
  rig.pass();
  CHECK(rig.fw().done > first);  // the bar moves, a slice a pass
  CHECK(rig.fw().phase == Phase::Checking);
  // F8 CANCEL.
  rig.sink.calls.clear();
  rig.tap(Button::F8);
  CHECK(rig.fw().phase == Phase::List);
  CHECK(rig.overlay() == OverlayId::FirmwareUpdate);
  CHECK(rig.sink.count("cancel") == 1u);
  CHECK(rig.fw().msg.empty());
  // Again, to the end.
  rig.tap(Button::F1);
  rig.tap(Button::F1);
  rig.passes(FakeFirmwareSink::kDefaultSlices);
  CHECK(rig.fw().phase == Phase::AskInstall);
  CHECK(rig.fw().image == 0x3F2A0u);
  CHECK(rig.fw().crc == 0x5EA1B0C4u);
  CHECK(rig.keys() == "F1:INSTALL,F8:CANCEL");
  CHECK(rig.sink.installs == 0u);  // nothing is installed by a good check
  CHECK(rig.sink.count("install") == 0u);
}

TEST_CASE("firmware: a failed check returns to the list with the fault and the offset; nothing is written") {
  Rig rig;
  FakeFirmwareSink::Script bad;
  bad.verdict = State::Failed;
  bad.fault = "flash configuration field is not the required one";
  bad.offset = 1040;
  rig.sink.script("TRACKER_1.PTF", bad);
  rig.tap(Button::F8);
  rig.tap(Button::F1);
  rig.tap(Button::F1);
  rig.passes(FakeFirmwareSink::kDefaultSlices);
  CHECK(rig.fw().phase == Phase::List);
  CHECK(rig.fw().bad);
  CHECK(std::string(rig.fw().msg.c_str()) == "NOT A VALID IMAGE: FLASH CONFIGURATION FIELD IS NOT THE REQUIRED ONE AT 1040");
  CHECK(rig.sink.installs == 0u);
  CHECK(rig.sink.count("install") == 0u);
  CHECK(rig.overlay() == OverlayId::FirmwareUpdate);
  rig.tap(Button::F8);
  CHECK(rig.overlay() == OverlayId::None);
}

TEST_CASE("firmware: question 2 is answered by F1 alone; INSTALL reaches the port exactly once and the hand-over shows DO NOT POWER OFF with no key acting") {
  Rig rig;
  rig.to_question_two();
  NEED(rig.fw().phase == Phase::AskInstall);
  rig.sink.calls.clear();
  rig.tap(Button::F1);
  CHECK(rig.fw().phase == Phase::Installing);
  CHECK(rig.sink.installs == 1u);
  CHECK(rig.sink.count("install") == 1u);
  CHECK(rig.keys().empty());  // the dialog owns the row and no key acts
  // Progress: the re-check runs, the state stays Installing, no key does anything.
  rig.pass();
  CHECK(rig.fw().phase == Phase::Installing);
  CHECK(rig.fw().done > 0u);
  for (const Button b : {Button::F1, Button::F8, Button::F2, Button::Up, Button::Config}) {
    rig.tap(b);
    CHECK(rig.fw().phase == Phase::Installing);
    CHECK(rig.overlay() == OverlayId::FirmwareUpdate);
  }
  rig.turn(1);
  CHECK(rig.fw().phase == Phase::Installing);
  CHECK(rig.sink.installs == 1u);   // (a second F1 did not install again)
  CHECK(rig.sink.count("cancel") == 0u);  // nothing took the install back
  // The chip would have reset; the dialog stays up for as long as the port says Running.
  rig.passes(50);
  CHECK(rig.fw().phase == Phase::Installing);
  CHECK(rig.overlay() == OverlayId::FirmwareUpdate);
}

TEST_CASE("firmware: an install the bootloader never takes, or that fails before the pin, comes back to the list with the reason") {
  Rig rig;
  FakeFirmwareSink::Script s;
  s.install_end = State::DidNotStart;
  rig.sink.script("TRACKER_1.PTF", s);
  rig.to_question_two();
  rig.tap(Button::F1);
  NEED(rig.fw().phase == Phase::Installing);
  rig.passes(FakeFirmwareSink::kDefaultSlices + 1);
  CHECK(rig.fw().phase == Phase::List);
  CHECK(std::string(rig.fw().msg.c_str()) == "UPDATE DID NOT START");
  CHECK(rig.fw().bad);
  CHECK(rig.keys() == "F1:UPDATE,F8:BACK");

  FakeFirmwareSink::Script f;
  f.install_end = State::Failed;
  f.install_fault = "read-back mismatch";
  rig.sink.script("TRACKER_1.PTF", f);
  rig.to_question_two();
  rig.tap(Button::F1);
  rig.passes(FakeFirmwareSink::kDefaultSlices + 1);
  CHECK(rig.fw().phase == Phase::List);
  CHECK(std::string(rig.fw().msg.c_str()) == "UPDATE FAILED: READ-BACK MISMATCH");
  CHECK(rig.sink.installs == 2u);
}

TEST_CASE("firmware: question 2 is taken back by another control, by F8, by the 30 s and by the card going to a computer; the port is cancelled and install() is never called") {
  const auto fresh = [](Rig& rig) {
    rig.to_question_two();
    NEED(rig.fw().phase == Phase::AskInstall);
    rig.sink.calls.clear();
  };
  const auto back_in_list = [](Rig& rig, const char* what) {
    CHECK_MESSAGE(rig.fw().phase == Phase::List, what);
    CHECK_MESSAGE(rig.overlay() == OverlayId::FirmwareUpdate, what);
    CHECK_MESSAGE(rig.sink.count("install") == 0u, what);
    CHECK_MESSAGE(rig.sink.installs == 0u, what);
    CHECK_MESSAGE(rig.sink.count("cancel") >= 1u, what);
  };
  {
    Rig rig;
    fresh(rig);
    rig.tap(Button::F3);
    back_in_list(rig, "another key");
  }
  {
    Rig rig;
    fresh(rig);
    rig.turn(-1);
    back_in_list(rig, "the encoder");
  }
  {
    Rig rig;
    fresh(rig);
    rig.pad(0);
    back_in_list(rig, "a pad");
  }
  {
    Rig rig;
    fresh(rig);
    rig.tap(Button::F8);
    back_in_list(rig, "F8");
    CHECK(rig.fw().msg.empty());
  }
  {  // Shift + F1 is not INSTALL: only F1 alone answers (spec risk 6); it takes the question back like Shift + F2
    for (const Button other : {Button::F1, Button::F2}) {
      Rig rig;
      fresh(rig);
      rig.press(Button::Shift);
      rig.pass(1);
      rig.tap(other);
      back_in_list(rig, other == Button::F1 ? "Shift + F1" : "Shift + F2");
      CHECK(rig.fw().msg.empty());
      rig.release(Button::Shift);
      rig.pass(1);
    }
  }
  {
    Rig rig;
    fresh(rig);
    rig.passes(10, 2900);  // 29 s of waiting: still good
    CHECK(rig.fw().phase == Phase::AskInstall);
    rig.passes(2, 1000);
    back_in_list(rig, "the 30 s");
    CHECK(std::string(rig.fw().msg.c_str()) == "THE CHECK EXPIRED: CHECK AGAIN");
    rig.tap(Button::F1);  // a late F1 asks question 1 again, it does not install
    CHECK(rig.fw().phase == Phase::AskUpdate);
    CHECK(rig.sink.installs == 0u);
  }
  {
    Rig rig;
    fresh(rig);
    rig.sink.lapse("card changed");  // the card went to a computer: the port says Failed
    rig.pass();
    back_in_list(rig, "the card going to a computer");
    CHECK(std::string(rig.fw().msg.c_str()) == "CHECK AGAIN");  // (the service lets the check go for a reason the App cannot tell: a neutral line, not "the card changed")
  }
}

TEST_CASE("firmware: every refusal says why and starts nothing: the port's own (computer, busy storage, a console job), an export, an unsaved project") {
  struct Case {
    Refusal why;
    const char* text;
  };
  const Case ports[] = {{Refusal::OnComputer, "THE CARD IS ON THE COMPUTER"},   {Refusal::StorageBusy, "THE CARD IS BUSY: TRY AGAIN"}, {Refusal::Busy, "ANOTHER UPDATE STEP IS RUNNING"},
                        {Refusal::NoCard, "NO CARD"},                           {Refusal::NoFolder, "NO FIRMWARE FOLDER ON THE CARD"},   {Refusal::NotWritable, "THE CARD CANNOT BE USED FOR AN UPDATE"}};
  for (const Case& c : ports) {
    Rig rig;
    rig.sink.list_refusal = c.why;
    rig.tap(Button::F8);
    CHECK_MESSAGE(rig.overlay() == OverlayId::None, c.text);
    CHECK_MESSAGE(rig.strip() == c.text, c.text);
    CHECK_MESSAGE(rig.sink.count("list_end") == 0u, c.text);  // (a list that did not open has nothing to close: list_end() follows a successful list_begin())
    CHECK_MESSAGE(rig.sink.count("begin_check TRACKER_1.PTF") == 0u, c.text);
  }
  {  // an export is running
    Rig rig;
    rig.view().xport.running = true;
    rig.tap(Button::F8);
    CHECK(rig.overlay() == OverlayId::None);
    CHECK(rig.strip() == "AN EXPORT OR A RECORDING IS RUNNING");
    CHECK(rig.sink.calls.empty());  // refused before the port was asked anything
  }
  {  // a recording is armed (a bare recorder that says it waits: the sim's own hook)
    Rig rig;
    auto rec = std::make_unique<app::Recorder>();
    rec->sim_wait(audio::RecTap::Align::Bar);
    NEED(rec->state() == app::Recorder::State::Armed);
    rig.app->attach_recorder(rec.get());
    rig.tap(Button::F8);
    CHECK(rig.overlay() == OverlayId::None);
    CHECK(rig.strip() == "AN EXPORT OR A RECORDING IS RUNNING");
    CHECK(rig.sink.calls.empty());
  }
  {  // an OG export is running (its job is the save layer's)
    Rig rig(true, true);
    core::JobRunner jobs;
    NEED(rig.saves.start_og_export(jobs, *rig.app->model().project, "", nullptr) == app::SaveService::Start::Started);
    NEED(rig.saves.og_state() == app::SaveService::OgState::Running);
    rig.tap(Button::F8);
    CHECK(rig.overlay() == OverlayId::None);
    CHECK(rig.strip() == "AN EXPORT OR A RECORDING IS RUNNING");
    CHECK(rig.sink.calls.empty());
  }
  {  // the project has unsaved changes
    Rig rig(true, true);
    rig.tap(Button::F8);
    CHECK(rig.overlay() == OverlayId::FirmwareUpdate);  // (a saved, untouched project is no reason)
    rig.tap(Button::F8);
    (void)test::arena(1).project.set_tempo_x100(test::arena(1).project.tempo_x100() + 100u);
    rig.sink.calls.clear();
    rig.tap(Button::F8);
    CHECK(rig.overlay() == OverlayId::None);
    CHECK(rig.strip() == "SAVE THE PROJECT FIRST");
    CHECK(rig.sink.calls.empty());
  }
  {  // a step is refused later: the dialog says so on its own line and goes back to the list
    Rig rig;
    rig.sink.check_refusal = Refusal::Busy;
    rig.tap(Button::F8);
    rig.tap(Button::F1);
    rig.tap(Button::F1);
    CHECK(rig.fw().phase == Phase::List);
    CHECK(std::string(rig.fw().msg.c_str()) == "ANOTHER UPDATE STEP IS RUNNING");
    CHECK(rig.fw().bad);
    CHECK(rig.overlay() == OverlayId::FirmwareUpdate);
    rig.sink.check_refusal = Refusal::None;
    rig.to_question_two();
    rig.sink.install_refusal = Refusal::Expired;  // INSTALL is refused: nothing was touched, the good check is let go
    rig.sink.calls.clear();
    rig.tap(Button::F1);
    CHECK(rig.fw().phase == Phase::List);
    CHECK(std::string(rig.fw().msg.c_str()) == "THE CHECK EXPIRED: CHECK AGAIN");
    CHECK(rig.sink.installs == 0u);
    CHECK(rig.sink.count("cancel") == 1u);
    rig.sink.install_refusal = Refusal::Busy;
    rig.to_question_two();
    rig.tap(Button::F1);
    CHECK(rig.fw().phase == Phase::List);
    CHECK(std::string(rig.fw().msg.c_str()) == "ANOTHER UPDATE STEP IS RUNNING");
    CHECK(rig.sink.installs == 0u);
    rig.sink.install_refusal = Refusal::NotChecked;
    rig.to_question_two();
    rig.tap(Button::F1);
    CHECK(rig.fw().phase == Phase::List);
    CHECK(std::string(rig.fw().msg.c_str()) == "CHECK AGAIN");
    CHECK(rig.sink.installs == 0u);
  }
}

TEST_CASE("firmware: cancel() on every way out of the dialog: BACK, a card removal and the overlay lost to another path; an install in the hand-over is not touched") {
  {  // a running check, the card is pulled
    Rig rig;
    rig.tap(Button::F8);
    rig.tap(Button::F1);
    rig.tap(Button::F1);
    NEED(rig.fw().phase == Phase::Checking);
    rig.sink.calls.clear();
    rig.app->card_changed(app::CardEvent::Removed);
    CHECK_FALSE(rig.fw().open());
    CHECK(rig.overlay() == OverlayId::None);
    CHECK(rig.sink.count("cancel") == 1u);
    rig.passes(5);
    CHECK(rig.sink.installs == 0u);
  }
  {  // a good check that waits for INSTALL, the card is pulled
    Rig rig;
    rig.to_question_two();
    rig.sink.calls.clear();
    rig.app->card_changed(app::CardEvent::Removed);
    CHECK_FALSE(rig.fw().open());
    CHECK(rig.overlay() == OverlayId::None);
    CHECK(rig.sink.count("cancel") == 1u);
  }
  {  // the list, the card comes back
    Rig rig;
    rig.tap(Button::F8);
    rig.sink.calls.clear();
    rig.app->card_changed(app::CardEvent::Ready);
    CHECK_FALSE(rig.fw().open());
    CHECK(rig.sink.count("cancel") == 1u);
  }
  {  // the overlay is closed by some other path (a screen change, a switch-off): the next pass cancels
    Rig rig;
    rig.to_question_two();
    rig.sink.calls.clear();
    rig.view().overlay = OverlayId::None;
    rig.pass();
    CHECK_FALSE(rig.fw().open());
    CHECK(rig.sink.count("cancel") == 1u);
    CHECK(rig.sink.installs == 0u);
  }
  {  // the hand-over: a card event does not close the dialog or take anything back
    Rig rig;
    rig.to_question_two();
    rig.tap(Button::F1);
    NEED(rig.fw().phase == Phase::Installing);
    rig.sink.calls.clear();
    rig.app->card_changed(app::CardEvent::Removed);
    CHECK(rig.fw().phase == Phase::Installing);
    CHECK(rig.overlay() == OverlayId::FirmwareUpdate);
    CHECK(rig.sink.count("cancel") == 0u);
  }
}

TEST_CASE("firmware: the dialog owns the row in every phase, the keys it names are the keys it binds, and it renders") {
  Rig rig;
  rig.tap(Button::F8);
  const test::RowRead list = test::read_owner_row(rig.app->model(), rig.app->state());
  CHECK(list.marked);
  CHECK(list.label[0] == "UPDATE");
  CHECK(list.label[7] == "BACK");
  CHECK(list.hot == 0u);
  rig.tap(Button::F1);
  const test::RowRead ask = test::read_owner_row(rig.app->model(), rig.app->state());
  CHECK(ask.label[0] == "YES");
  CHECK(ask.label[7] == "CANCEL");
  CHECK(ask.hot == 0x01u);  // the answer is inverted, as a question's
  rig.tap(Button::F1);
  CHECK(test::read_owner_row(rig.app->model(), rig.app->state()).label[0].empty());
  rig.passes(FakeFirmwareSink::kDefaultSlices);
  const test::RowRead ask2 = test::read_owner_row(rig.app->model(), rig.app->state());
  CHECK(ask2.label[0] == "INSTALL");
  CHECK(ask2.hot == 0x01u);
  rig.tap(Button::F1);
  const test::RowRead going = test::read_owner_row(rig.app->model(), rig.app->state());
  CHECK(going.marked);
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(going.label[i].empty());
  }
  CHECK(app::overlay_of(OverlayId::FirmwareUpdate) == &app::kFirmwareOverlay);
  CHECK(app::kFirmwareOverlay.modal());
  CHECK(app::kFirmwareOverlay.context() == app::Context::FirmwareUpdate);
  CHECK(rig.app->frames() >= 1u);  // (every phase above was drawn by the pass that followed it)
}

TEST_CASE("firmware: the refusals of the opening hold at INSTALL too: an export, a recording or an unsaved change that came while the question waited cancel the check, and install() is not called") {
  const auto refused = [](Rig& rig, const char* text) {
    CHECK(rig.fw().phase == Phase::List);
    CHECK(rig.overlay() == OverlayId::FirmwareUpdate);
    CHECK(std::string(rig.fw().msg.c_str()) == text);
    CHECK(rig.fw().bad);
    CHECK(rig.sink.count("install") == 0u);
    CHECK(rig.sink.installs == 0u);
    CHECK(rig.sink.count("cancel") >= 1u);  // (the good check is let go: a late F1 asks question 1 again, it cannot install)
  };
  {  // an export started (a MIDI-triggered one, a script)
    Rig rig;
    rig.to_question_two();
    NEED(rig.fw().phase == Phase::AskInstall);
    rig.sink.calls.clear();
    rig.view().xport.running = true;
    rig.tap(Button::F1);
    refused(rig, "AN EXPORT OR A RECORDING IS RUNNING");
  }
  {  // a recording armed
    Rig rig;
    auto rec = std::make_unique<app::Recorder>();
    rig.app->attach_recorder(rec.get());
    rig.to_question_two();
    NEED(rig.fw().phase == Phase::AskInstall);
    rig.sink.calls.clear();
    rec->sim_wait(audio::RecTap::Align::Bar);
    rig.tap(Button::F1);
    refused(rig, "AN EXPORT OR A RECORDING IS RUNNING");
  }
  {  // the project was edited
    Rig rig(true, true);
    rig.to_question_two();
    NEED(rig.fw().phase == Phase::AskInstall);
    rig.sink.calls.clear();
    (void)test::arena(1).project.set_tempo_x100(test::arena(1).project.tempo_x100() + 100u);
    rig.tap(Button::F1);
    refused(rig, "SAVE THE PROJECT FIRST");
  }
  {  // nothing changed: INSTALL goes through (the control for the three above)
    Rig rig(true, true);
    rig.to_question_two();
    rig.tap(Button::F1);
    CHECK(rig.fw().phase == Phase::Installing);
    CHECK(rig.sink.installs == 1u);
  }
}

TEST_CASE("firmware: the overlay lost while the hand-over runs is put back and nothing is cancelled; lost before INSTALL the pass cancels") {
  {
    Rig rig;
    rig.to_question_two();
    rig.tap(Button::F1);
    NEED(rig.fw().phase == Phase::Installing);
    rig.sink.calls.clear();
    rig.view().overlay = OverlayId::None;  // some path closed it
    rig.pass();
    CHECK(rig.fw().phase == Phase::Installing);                // not closed
    CHECK(rig.overlay() == OverlayId::FirmwareUpdate);         // "DO NOT POWER OFF" is on the screen again
    CHECK(rig.sink.count("cancel") == 0u);                     // and the job was not touched
    CHECK(rig.sink.installs == 1u);
    rig.pass();
    CHECK(rig.fw().phase == Phase::Installing);
    CHECK(rig.sink.count("cancel") == 0u);
  }
  {  // a failure after the overlay was put back still comes back to the list with its reason
    Rig rig;
    FakeFirmwareSink::Script bad;
    bad.install_end = State::Failed;
    bad.install_slices = 2;
    rig.sink.script("TRACKER_1.PTF", bad);
    rig.to_question_two();
    rig.tap(Button::F1);
    NEED(rig.fw().phase == Phase::Installing);
    rig.view().overlay = OverlayId::None;
    rig.passes(6);
    CHECK(rig.fw().phase == Phase::List);
    CHECK(rig.overlay() == OverlayId::FirmwareUpdate);
    CHECK(std::string(rig.fw().msg.c_str()) == "UPDATE FAILED: I/O ERROR");
  }
}

TEST_CASE("firmware: the power switch-off cancels a check and closes the dialog, but does not touch the hand-over") {
  const auto off = [](Rig& rig) { rig.app->power_begin(); };
  {  // the list
    Rig rig;
    rig.tap(Button::F8);
    rig.sink.calls.clear();
    off(rig);
    CHECK_FALSE(rig.fw().open());
    CHECK(rig.overlay() == OverlayId::None);
    CHECK(rig.sink.count("cancel") == 1u);
  }
  {  // a running check holds the card the shutdown's save needs
    Rig rig;
    rig.tap(Button::F8);
    rig.tap(Button::F1);
    rig.tap(Button::F1);
    NEED(rig.fw().phase == Phase::Checking);
    rig.sink.calls.clear();
    off(rig);
    CHECK_FALSE(rig.fw().open());
    CHECK(rig.overlay() == OverlayId::None);
    CHECK(rig.sink.count("cancel") == 1u);
    rig.passes(5);
    CHECK(rig.sink.installs == 0u);
  }
  {  // a good check that waits
    Rig rig;
    rig.to_question_two();
    rig.sink.calls.clear();
    off(rig);
    CHECK_FALSE(rig.fw().open());
    CHECK(rig.sink.count("cancel") == 1u);
  }
  {  // the hand-over is past the point of no return
    Rig rig;
    rig.to_question_two();
    rig.tap(Button::F1);
    NEED(rig.fw().phase == Phase::Installing);
    rig.sink.calls.clear();
    off(rig);
    CHECK(rig.fw().phase == Phase::Installing);
    CHECK(rig.overlay() == OverlayId::FirmwareUpdate);
    CHECK(rig.sink.count("cancel") == 0u);
  }
}
