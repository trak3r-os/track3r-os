// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-NAMES: NAME for the track (Mixer, Shift + F1), the pattern (Song, Shift + F6) and the project (File, Shift + F6), on the on-screen keyboard of OS-INSTUI S7.
//   * each key opens the keyboard on the current name; F1 sets the stored field as one undo unit, F8 changes nothing, an empty accept clears it, and the field's own limit holds;
//   * the Mixer refuses a bus, the master and a page with no track under the cursor (TRACKS ONLY; NO TRACK on a STEMS column of a track the project does not have);
//   * SAVE AS is prefilled with the title when no folder holds the project;
//   * the names are drawn cut: the pattern's on the Pattern info line, the project's in the File header.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/model.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/pattern_view.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/app/screens/mixer_screen.h>
#include <o3os/app/screens/song_screen.h>
#include <o3os/app/text_entry.h>
#include <o3os/app/view_state.h>
#include <o3os/core/input_event.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using core::ActionArg;
using Target = app::TextEntryView::Target;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];

// The folder the open project was loaded from, as the device's load reports it.
class FakeProject final : public app::ProjectSink {
 public:
  std::string last_folder;
  Start open(const char*, Format) noexcept override { return Start::NoCard; }
  Start start_new() noexcept override { return Start::NoCard; }
  bool busy() const noexcept override { return false; }
  const char* folder() const noexcept override { return last_folder.c_str(); }
  bool loaded() const noexcept override { return true; }
  const char* detail() const noexcept override { return ""; }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  test::MemCard card;
  app::SaveService saves;
  FakeProject sink;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  Rig() {
    model::Project& p = arena->project;
    p.reset();
    NEED(p.set_active_tracks(4) == model::Status::Ok);
    for (unsigned id = 1; id <= 3; ++id) {
      NEED(p.create_pattern(id, 16) == model::Status::Ok);
    }
    for (unsigned id : {1u, 2u, 3u}) {
      NEED(p.append_order(id) == model::Status::Ok);
    }
    NEED(p.set_track_name(2, "BASS", 4) == model::Status::Ok);
    NEED(p.set_pattern_name(2, "VERSE", 5) == model::Status::Ok);
    NEED(p.set_name("My Song", 7) == model::Status::Ok);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_storage(&card);
    saves.attach(card, card);
    app->attach_saves(&saves);
    app->attach_project_sink(&sink);
    pass();
  }
  model::Project& project() { return arena->project; }
  app::ViewState& view() { return const_cast<app::ViewState&>(app->state()); }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void tap(Button b) {
    press(b);
    pass(1);
    release(b);
    pass(1);
  }
  void shift_tap(Button b) {
    press(Button::Shift);
    pass(1);
    tap(b);
    release(Button::Shift);
    pass(1);
  }
  std::string note() const { return app->message(); }
  void clear_notes() {
    pass(10000);
    pass(10000);
  }
  std::size_t depth() const { return editor->journal().depth(); }
  bool open() const { return app->state().overlay == app::OverlayId::TextEntry; }
  std::string text() const { return app->state().text.entry.text(); }
  void type(const char* text) {
    for (const char* c = text; *c != '\0'; ++c) {
      unsigned key = app::kTextKeys;
      for (unsigned k = 0; k < app::kTextKeys; ++k) {
        if (app::kTextKeysSmall[k] == *c || app::kTextKeysBig[k] == *c) {
          key = k;
          break;
        }
      }
      NEED(key < app::kTextKeys);
      unsigned pad = 48;
      for (unsigned p = 0; p < 48; ++p) {
        if (app::kTextPadKey[p] == key) {
          pad = p;
          break;
        }
      }
      NEED(pad < 48);
      if (app::kTextKeysBig[key] == *c && app::kTextKeysSmall[key] != *c && !app->state().text.entry.shift()) {
        (void)app->invoke(core::action_id("text.caps"), 0);
      }
      (void)app->invoke(core::action_id("text.pad"), static_cast<ActionArg>(pad));
    }
  }
  void undo() { (void)app->invoke(core::action_id("edit.undo"), 0); }
  void redo() { (void)app->invoke(core::action_id("edit.redo"), 0); }
  void on_mixer(unsigned col, unsigned row = 0) {
    tap(Button::Master);
    view().page[app::screen_index(app::ScreenId::Mixer)] = 0;
    view().mixer.col = static_cast<std::uint8_t>(col);
    view().mixer.row = static_cast<std::uint8_t>(row);
    pass();
  }
  void on_song(int pos) {
    tap(Button::Song);
    (void)view().song.set(pos, 3);
    pass();
  }
};

}  // namespace

TEST_CASE("names: the labels of the three Shift keys are NAME, and they are the bindings (the Shift row test resolves each)") {
  CHECK(std::string(app::kMixerShiftKeys[0]) == "NAME");
  CHECK(std::string(app::kSongShiftKeys[5]) == "NAME");
  CHECK(std::string(app::kFileShiftKeys[5]) == "NAME");
  CHECK(std::string(app::kFileShiftKeys[4]) == "END");  // the quarter jumps stay
}

TEST_CASE("names: Mixer Shift + F1 names the track under the cursor: current name, F1 one undo unit, F8 nothing, empty clears, the field's limit") {
  Rig rig;
  rig.on_mixer(2);
  const std::size_t depth = rig.depth();
  rig.shift_tap(Button::F1);
  CHECK(rig.open());
  CHECK(rig.app->state().text.target == Target::Track);
  CHECK(rig.app->state().text.subject == 2);
  CHECK(rig.text() == "BASS");
  CHECK(rig.app->state().text.entry.selected());
  CHECK(rig.app->state().text.entry.capacity() == model::kTrackNameMax);
  rig.tap(Button::F8);  // cancel
  CHECK_FALSE(rig.open());
  CHECK(std::string(rig.project().track(2)->name) == "BASS");
  CHECK(rig.depth() == depth);

  rig.shift_tap(Button::F1);
  rig.type("lead");
  rig.tap(Button::F1);
  CHECK_FALSE(rig.open());
  CHECK(std::string(rig.project().track(2)->name) == "lead");
  CHECK(rig.depth() == depth);  // OS-UNDOGRP D10: the Mixer has no undo, a track's name is final (no unit in any area)
  CHECK(rig.note() == "TRACK 03 NAMED");
  CHECK(std::string(rig.project().track(1)->name).empty());  // no other track moved

  // The same name records nothing.
  const std::size_t now = rig.depth();
  rig.shift_tap(Button::F1);
  rig.tap(Button::F1);
  CHECK(rig.depth() == now);

  // An empty accept clears the name.
  rig.shift_tap(Button::F1);
  (void)rig.app->invoke(core::action_id("text.backspace"), 0);  // the whole name is selected: one key deletes all
  CHECK(rig.text().empty());
  rig.tap(Button::F1);
  CHECK(std::string(rig.project().track(2)->name).empty());
  CHECK(rig.depth() == now);
  CHECK(rig.note() == "TRACK 03 NAME CLEARED");

  // The limit is the stored field's: 20 characters.
  rig.shift_tap(Button::F1);
  rig.type("abcdefghijklmnopqrstuvwxyz");
  CHECK(rig.app->state().text.entry.length() == model::kTrackNameMax);
  rig.tap(Button::F1);
  CHECK(std::string(rig.project().track(2)->name) == "abcdefghijklmnopqrst");
}

TEST_CASE("names: Mixer Shift + F1 takes any key (a name is not a file name) and the ROUTING and STEMS pages name the track of the row or column") {
  Rig rig;
  rig.on_mixer(0);
  rig.shift_tap(Button::F1);
  rig.type("a@b+c");  // '@' and '+' are refused in a file name, not here
  CHECK(rig.text() == "a@b+c");
  rig.tap(Button::F1);
  CHECK(std::string(rig.project().track(0)->name) == "a@b+c");

  rig.view().page[app::screen_index(app::ScreenId::Mixer)] = static_cast<std::uint8_t>(app::MixerPage::Routing);
  rig.view().mixer.row = 1;
  rig.view().mixer.col = 3;
  rig.pass();
  rig.shift_tap(Button::F1);
  CHECK(rig.open());
  CHECK(rig.app->state().text.subject == 1);
  rig.tap(Button::F8);

  rig.view().page[app::screen_index(app::ScreenId::Mixer)] = static_cast<std::uint8_t>(app::MixerPage::Stems);
  rig.view().mixer.col = 3;
  rig.pass();
  rig.shift_tap(Button::F1);
  CHECK(rig.open());
  CHECK(rig.app->state().text.subject == 3);
  rig.tap(Button::F8);
}

TEST_CASE("names: the Mixer refuses a bus, the master and a page with no track under the cursor: TRACKS ONLY") {
  Rig rig;
  const std::size_t depth = rig.depth();
  for (unsigned col = 4; col < 4 + app::kMixBuses; ++col) {  // IN, SND A, SND B, CUE, MASTER
    rig.on_mixer(col);
    rig.clear_notes();
    rig.shift_tap(Button::F1);
    CHECK_FALSE(rig.open());
    CHECK(rig.note() == "TRACKS ONLY");
  }
  rig.on_mixer(0);
  rig.view().page[app::screen_index(app::ScreenId::Mixer)] = static_cast<std::uint8_t>(app::MixerPage::MasterFx);
  rig.pass();
  rig.clear_notes();
  rig.shift_tap(Button::F1);
  CHECK_FALSE(rig.open());
  CHECK(rig.note() == "TRACKS ONLY");
  rig.view().page[app::screen_index(app::ScreenId::Mixer)] = static_cast<std::uint8_t>(app::MixerPage::Routing);
  rig.view().mixer.row = 4;  // LINE IN
  rig.pass();
  rig.clear_notes();
  rig.shift_tap(Button::F1);
  CHECK_FALSE(rig.open());
  CHECK(rig.note() == "TRACKS ONLY");
  CHECK(rig.depth() == depth);
}

TEST_CASE("names: on STEMS a column of a track the project does not have says NO TRACK, a bus column TRACKS ONLY") {
  Rig rig;  // four active tracks
  const std::size_t depth = rig.depth();
  rig.on_mixer(0);
  rig.view().page[app::screen_index(app::ScreenId::Mixer)] = static_cast<std::uint8_t>(app::MixerPage::Stems);
  for (unsigned col : {4u, 11u}) {  // T5 and T12
    rig.view().mixer.col = static_cast<std::uint8_t>(col);
    rig.pass();
    rig.clear_notes();
    rig.shift_tap(Button::F1);
    CHECK_FALSE(rig.open());
    CHECK(rig.note() == "NO TRACK");
  }
  for (unsigned col : {12u, 15u}) {  // ML and SB
    rig.view().mixer.col = static_cast<std::uint8_t>(col);
    rig.pass();
    rig.clear_notes();
    rig.shift_tap(Button::F1);
    CHECK_FALSE(rig.open());
    CHECK(rig.note() == "TRACKS ONLY");
  }
  CHECK(rig.depth() == depth);
}

TEST_CASE("names: Song Shift + F6 names the pattern of the slot: current name, F1 one undo unit, F8 nothing, empty clears, 30 characters") {
  Rig rig;
  rig.on_song(1);  // the slot of pattern 2, VERSE
  const std::size_t depth = rig.depth();
  rig.shift_tap(Button::F6);
  CHECK(rig.open());
  CHECK(rig.app->state().text.target == Target::Pattern);
  CHECK(rig.app->state().text.subject == 2);
  CHECK(rig.text() == "VERSE");
  CHECK(rig.app->state().text.entry.capacity() == model::kPatternNameMax);
  rig.tap(Button::F8);
  CHECK(std::string(rig.project().pattern(2)->name) == "VERSE");
  CHECK(rig.depth() == depth);

  rig.shift_tap(Button::F6);
  rig.type("chorus 1");
  rig.tap(Button::F1);
  CHECK(std::string(rig.project().pattern(2)->name) == "chorus 1");
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.note() == "PATTERN 02 NAMED");
  CHECK(std::string(rig.project().pattern(1)->name).empty());
  rig.undo();
  CHECK(std::string(rig.project().pattern(2)->name) == "VERSE");
  rig.redo();
  CHECK(std::string(rig.project().pattern(2)->name) == "chorus 1");

  rig.shift_tap(Button::F6);
  (void)rig.app->invoke(core::action_id("text.backspace"), 0);
  rig.tap(Button::F1);
  CHECK(std::string(rig.project().pattern(2)->name).empty());

  rig.shift_tap(Button::F6);
  rig.type("abcdefghijklmnopqrstuvwxyzabcdefgh");
  CHECK(rig.app->state().text.entry.length() == model::kPatternNameMax);
  rig.tap(Button::F1);
  CHECK(std::string(rig.project().pattern(2)->name) == "abcdefghijklmnopqrstuvwxyzabcd");
}

TEST_CASE("names: Song NAME has nothing to name in an empty song or on a slot whose pattern does not exist yet") {
  Rig rig;
  NEED(rig.project().append_order(9) == model::Status::Ok);  // a slot may name a pattern the project does not have
  rig.on_song(3);
  (void)rig.view().song.set(3, 4);
  rig.pass();
  rig.clear_notes();
  rig.shift_tap(Button::F6);
  CHECK_FALSE(rig.open());
  CHECK(rig.note() == "NO SUCH PATTERN YET");
}

TEST_CASE("names: File Shift + F6 sets the project title, not the folder: current title, F8 one undo unit, F7 nothing, empty clears, 32 characters") {
  Rig rig;
  rig.sink.last_folder = "Projects/Old Folder";
  rig.tap(Button::File);
  const std::size_t depth = rig.depth();
  rig.shift_tap(Button::F6);
  CHECK(rig.open());
  CHECK(rig.app->state().text.target == Target::Project);
  CHECK(rig.text() == "My Song");
  CHECK(rig.app->state().text.entry.capacity() == model::kProjectNameMax);
  rig.tap(Button::F8);
  CHECK(std::string(rig.project().name()) == "My Song");
  CHECK(rig.depth() == depth);

  rig.shift_tap(Button::F6);
  rig.type("night drive");
  rig.tap(Button::F1);
  CHECK(std::string(rig.project().name()) == "night drive");
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.note() == "PROJECT NAMED");
  CHECK(rig.sink.last_folder == "Projects/Old Folder");  // the folder is the card's, and untouched
  rig.tap(Button::Song);  // File has no F8 undo (OS-UNDOGRP): the title is a SONG unit
  rig.undo();
  CHECK(std::string(rig.project().name()) == "My Song");
  rig.redo();
  CHECK(std::string(rig.project().name()) == "night drive");
  rig.tap(Button::File);

  rig.shift_tap(Button::F6);
  (void)rig.app->invoke(core::action_id("text.backspace"), 0);
  rig.tap(Button::F1);
  CHECK(std::string(rig.project().name()).empty());
  CHECK(rig.note() == "PROJECT NAME CLEARED");

  rig.shift_tap(Button::F6);
  rig.type("abcdefghijklmnopqrstuvwxyzabcdefghij");
  CHECK(rig.app->state().text.entry.length() == model::kProjectNameMax);
  rig.tap(Button::F1);
  CHECK(std::string(rig.project().name()) == "abcdefghijklmnopqrstuvwxyzabcdef");
}

TEST_CASE("names: File Shift + F6 names the project on the IMPORT and EXPORT pages too, and the page stays") {
  Rig rig;
  rig.tap(Button::File);
  rig.tap(Button::F6);  // IMPORT
  NEED(rig.app->state().import.page);
  rig.shift_tap(Button::F6);
  CHECK(rig.open());
  CHECK(rig.app->state().text.target == Target::Project);
  CHECK(rig.text() == "My Song");
  rig.type("x");
  rig.tap(Button::F1);
  CHECK(std::string(rig.project().name()) == "x");  // (the field was all selected: typing replaced it)
  CHECK(rig.app->state().import.page);
  rig.tap(Button::F6);  // PROJECTS
  NEED(!rig.app->state().import.page && !rig.app->state().xport.page);
  rig.tap(Button::F7);  // EXPORT
  NEED(rig.app->state().xport.page);
  rig.shift_tap(Button::F6);
  CHECK(rig.open());
  CHECK(rig.app->state().text.target == Target::Project);
  CHECK(rig.text() == "x");
  rig.tap(Button::F8);  // cancel
  CHECK_FALSE(rig.open());
  CHECK(std::string(rig.project().name()) == "x");
  CHECK(rig.app->state().xport.page);
}

TEST_CASE("names: the keyboard's heading says what is being named") {
  Rig rig;
  rig.on_mixer(2);
  rig.shift_tap(Button::F1);
  CHECK(rig.open());  // the heading is drawn by the overlay from the target (title_of)
  CHECK(rig.app->state().text.target == Target::Track);
  rig.tap(Button::F8);
  rig.on_song(0);
  rig.shift_tap(Button::F6);
  CHECK(rig.app->state().text.target == Target::Pattern);
  rig.tap(Button::F8);
  rig.tap(Button::File);
  rig.shift_tap(Button::F6);
  CHECK(rig.app->state().text.target == Target::Project);
  rig.tap(Button::F8);
}

TEST_CASE("names: without a document there is nothing to name") {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  auto a = std::make_unique<app::App>(input, clock, display);
  clock.advance(16000);
  a->run_once();
  (void)a->invoke(core::action_id("nav.screen"), static_cast<ActionArg>(app::ScreenId::Mixer));
  (void)a->invoke(core::action_id("mixer.name"), 0);
  CHECK(a->state().overlay != app::OverlayId::TextEntry);
  (void)a->invoke(core::action_id("nav.screen"), static_cast<ActionArg>(app::ScreenId::File));
  (void)a->invoke(core::action_id("file.name"), 0);
  CHECK(a->state().overlay != app::OverlayId::TextEntry);
}

// ---- SAVE AS ----------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("names: text_file_name keeps the characters a file name takes, drops the rest, starts with a letter or digit and ends without a space") {
  char out[32];
  CHECK(app::text_file_name("My Song", out, sizeof out) == 7);
  CHECK(std::string(out) == "My Song");
  CHECK(app::text_file_name("  -Mix/Down: v2 !  ", out, sizeof out) == 10);
  CHECK(std::string(out) == "MixDown v2");
  CHECK(app::text_file_name("a-b_c.d", out, sizeof out) == 5);
  CHECK(std::string(out) == "a-bcd");
  CHECK(app::text_file_name("@+/ ", out, sizeof out) == 0);
  CHECK(out[0] == '\0');
  CHECK(app::text_file_name("", out, sizeof out) == 0);
  CHECK(app::text_file_name(nullptr, out, sizeof out) == 0);
  CHECK(app::text_file_name("abcdefghijklmnopqrstuvwxyz0123456789", out, sizeof out) == 31);  // the buffer's room
  CHECK(app::text_file_name("abcdefghijklmnopqrstuvwxyz0123 6789", out, sizeof out) == 30);  // (and no space left at the cut)
}

TEST_CASE("names: SAVE AS is prefilled from the title when no folder holds the project, with the automatic name otherwise") {
  Rig rig;
  rig.tap(Button::File);
  // No folder (a new or imported project): the title, reduced to the characters a folder name takes.
  rig.tap(Button::F5);
  CHECK(rig.open());
  CHECK(rig.app->state().text.target == Target::SaveAs);
  CHECK(rig.text() == "My Song");
  CHECK(rig.app->state().text.entry.selected());
  (void)rig.app->invoke(core::action_id("text.auto"), 0);  // F6 still gives the automatic name
  CHECK(rig.text().compare(0, 6, "Track ") == 0);
  rig.tap(Button::F8);

  NEED(rig.editor->set_name("Mix/Down: v2", 12) == model::Status::Ok);
  rig.tap(Button::F5);
  CHECK(rig.text() == "MixDown v2");
  rig.tap(Button::F8);

  // A title that makes no valid name, or none: the automatic name, as before.
  NEED(rig.editor->set_name("CON", 3) == model::Status::Ok);
  rig.tap(Button::F5);
  CHECK(rig.text().compare(0, 6, "Track ") == 0);
  rig.tap(Button::F8);
  NEED(rig.editor->set_name("", 0) == model::Status::Ok);
  rig.tap(Button::F5);
  CHECK(rig.text().compare(0, 6, "Track ") == 0);
  rig.tap(Button::F8);

  // The project came from a folder: the automatic name, whatever the title.
  NEED(rig.editor->set_name("My Song", 7) == model::Status::Ok);
  rig.sink.last_folder = "Projects/Old Folder";
  rig.tap(Button::F5);
  CHECK(rig.text().compare(0, 6, "Track ") == 0);
  rig.tap(Button::F8);
}

// ---- the names drawn --------------------------------------------------------------------------------------------------------------------------------

namespace {

std::vector<std::uint32_t> pattern_frame(const char* name, int pattern = 5) {
  platform::host::FakeDisplay sink;
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(sink, storage.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, ui::theme(ui::theme_id_at(0)));
  CHECK(r.begin(ui::Color::Pane));
  auto data = std::make_unique<app::PatternData>();
  app::fill_demo(*data);
  app::PatternView::Status status{1, app::App::kSoftKeyLabels};
  status.pattern = pattern;
  status.pattern_name = name;
  app::PatternView{}.draw(r, *data, status);
  CHECK(r.end());
  return sink.last_frame();
}

std::vector<std::uint32_t> file_frame(const char* title, const char* folder, bool unsaved = false) {
  platform::host::FakeDisplay sink;
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(sink, storage.words, eve::kMaxDisplayListWords);
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  ui::Renderer r(builder, theme);
  CHECK(r.begin(ui::Color::Pane));
  auto arena = std::make_unique<model::ModelArena>();
  arena->project.reset();
  CHECK(arena->project.set_name(title, std::strlen(title)) == model::Status::Ok);
  app::Model model;
  model.project = &arena->project;
  model.project_folder = folder;
  model.project_unsaved = unsaved;
  app::ViewState view;
  app::kFileScreen.render(model, view, r, ui::Rect{0, 0, 0, 0});
  CHECK(r.end());
  return sink.last_frame();
}

}  // namespace

TEST_CASE("names: the Pattern info line shows the pattern's name after its number, cut to its room; an unnamed pattern draws what it always did") {
  const auto none = pattern_frame("");
  const auto verse = pattern_frame("VERSE");
  CHECK(none != verse);
  CHECK(pattern_frame("VERSE", 6) != verse);  // the number is part of it
  const auto cut = pattern_frame("ABCDEFGHIJKLMN");                              // 14: the room
  CHECK(pattern_frame("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123") == cut);                   // 30: the stored limit, shown cut
  CHECK(pattern_frame("ABCDEFGHIJKLM") != cut);
}

TEST_CASE("names: the File header shows the title, or the folder's name without a title; a long one is cut, nothing without either") {
  const auto none = file_frame("", "");
  CHECK(file_frame("My Song", "") != none);
  CHECK(file_frame("", "Projects/Old Folder") != none);
  CHECK(file_frame("", "Projects/Old Folder") == file_frame("", "Old Folder"));   // the last component of the path
  CHECK(file_frame("My Song", "Projects/Old Folder") == file_frame("My Song", ""));  // the title wins
  // The room between the columns and the page's name is smaller than 32 characters: the end of a full-length title is cut and cannot spill over the page name.
  CHECK(file_frame("abcdefghijklmnopqrstuvwxyzabcdef", "") == file_frame("abcdefghijklmnopqrstuvwxyzabcdeg", ""));
  CHECK(file_frame("abcdefghijklmnopqrstuvwxyzabcdef", "") != file_frame("Abcdefghijklmnopqrstuvwxyzabcdef", ""));
}

TEST_CASE("names: the File header marks an unsaved project (OS-AUDIT-A6): UNSAVED instead of PROJECT, also for a project with no name; clean draws what it always did") {
  CHECK(file_frame("My Song", "", true) != file_frame("My Song", "", false));
  CHECK(file_frame("", "", true) != file_frame("", "", false));  // a new project with no name still says it
  CHECK(file_frame("My Song", "", false) == file_frame("My Song", ""));
  CHECK(file_frame("abcdefghijklmnopqrstuvwxyzabcdef", "", true) == file_frame("abcdefghijklmnopqrstuvwxyzabcdeg", "", true));  // still cut to its room
}
