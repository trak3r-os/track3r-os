// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-002b: the screen registry and navigation, the overlay slot (capture and fall-through), the Navigator, Learn, the
// per-view modes and the navigation keys, the Song cursor. The notification queue itself is notify_test.cpp, the
// covered-rect and look checks are overlay_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/bindings.h>
#include <o3os/app/buttons.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/screens/mixer_screen.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/screens/song_screen.h>
#include <o3os/app/song_data.h>
#include <o3os/app/song_view.h>
#include <o3os/app/view_state.h>
#include <o3os/core/input_event.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::Column;
using app::OverlayId;
using app::ScreenId;

namespace {

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  Rig() { app->run_once(); }

  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void turn(int delta) { input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(delta))); }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  // A tap, then the pass that handles it (a press and a release one pass apart, as the scenario runner does).
  void tap(Button b) {
    press(b);
    pass(1);
    release(b);
    pass(1);
  }
  // Holds Shift, taps `b`, lets go of Shift.
  void shift_tap(Button b) {
    press(Button::Shift);
    pass(1);
    tap(b);
    release(Button::Shift);
    pass(1);
  }
  // OS-NAV2: a tap of Shift + Pattern opens the Navigator and it stays open after both keys are let go.
  void open_navigator() { shift_tap(Button::Pattern); }
  // The same by holding the two keys down for as long as the old hold took: it opens at the press, and Pattern and Shift stay down.
  void open_navigator_held() {
    press(Button::Shift);
    pass(1);
    press(Button::Pattern);
    pass(core::kGestureTimings.hold_ms + 1);
    release(Button::Pattern);  // OS-PATTEMPO: Pattern waits for a turn or a pad, so the Navigator opens at its release, Shift still down
    pass(1);
  }
  const app::ViewState& state() const { return app->state(); }
  ScreenId screen() const { return app->state().screen; }
  std::string message() const { return app->message(); }
  void invoke(const char* name, core::ActionArg arg = 0) { app->invoke(core::action_id(name), arg); }
};

// A screen that records what navigation does to it, for the order of leave and enter.
struct Recorder final : app::Screen {
  const char* name;
  std::vector<std::string>* log;
  ScreenId sid;
  unsigned page_count = 1;
  const char* const* labels = app::kPatternSoftKeys;
  Recorder(const char* n, std::vector<std::string>* l, ScreenId i) : name(n), log(l), sid(i) {}
  ScreenId id() const noexcept override { return sid; }
  void enter(const app::Model&, app::ViewState& v) const noexcept override {
    log->push_back(std::string("enter ") + name + " in " + std::to_string(static_cast<int>(v.screen)));
  }
  void leave(const app::Model&, app::ViewState& v) const noexcept override {
    log->push_back(std::string("leave ") + name + " in " + std::to_string(static_cast<int>(v.screen)));
  }
  void render(const app::Model&, const app::ViewState&, ui::Renderer&, ui::Rect) const noexcept override {}
  app::Context context() const noexcept override { return app::Context::Global; }
  unsigned pages() const noexcept override { return page_count; }
  const char* const* soft_keys() const noexcept override { return labels; }
};

}  // namespace

// ---- the registry -------------------------------------------------------------------------------------------------------

TEST_CASE("screens: the registry has one row per screen button, unique ids, names and buttons, and the pattern view first") {
  const app::ScreenTable table = app::screen_registry();
  CHECK(table.count == app::kScreenCount);
  CHECK(app::screens_valid(table.items, table.count));
  CHECK(table.items[0].id == ScreenId::Pattern);  // the first registered screen
  CHECK(table.items[0].screen == &app::kPatternScreen);
  CHECK(table.items[1].id == ScreenId::Song);
  CHECK(table.items[1].screen == &app::kSongScreen);
  unsigned implemented = 0;
  for (std::size_t i = 0; i < table.count; ++i) {
    const app::ScreenEntry& e = table.items[i];
    implemented += e.screen != nullptr ? 1u : 0u;
    CHECK_MESSAGE(app::find_screen(table, e.id) == &e, e.name);
    CHECK_MESSAGE((e.screen == nullptr || e.screen->id() == e.id), e.name << ": the screen and its row disagree on the id");
    CHECK(app::screen_at(table, static_cast<int>(i)) == &e);
    CHECK(app::screen_position(table, e.id) == static_cast<int>(i));
    for (std::size_t j = 0; j < i; ++j) {
      CHECK_MESSAGE(table.items[j].id != e.id, e.name);
      CHECK_MESSAGE(std::strcmp(table.items[j].name, e.name) != 0, e.name);
      CHECK_MESSAGE(std::strcmp(table.items[j].key, e.key) != 0, e.name);
      CHECK_MESSAGE((table.items[j].hidden || e.hidden || table.items[j].buttonless || e.buttonless || table.items[j].button != e.button), e.name);
    }
  }
  CHECK(implemented == 14u);  // Record (OS-011c), Pattern, Song, Instrument PARAMS (OS-002d), File (OS-010a), Sample load, Wave, Sample edit and Config (OS-011a), Mixer and Perform (OS-011b), the hidden INPUT TEST (OS-007d), MODULES and the hidden app page (OS-012b)
  CHECK(app::navigator_screens().count == app::kScreenCount - 2u);  // the Navigator lists all but the two hidden screens
  CHECK(app::screen_registry().items[app::kScreenCount - 1].hidden);
  // Every id of the enum has its row: nothing is missing, and an id out of range is nobody's.
  for (unsigned id = 0; id < app::kScreenCount; ++id) {
    CHECK(app::find_screen(table, static_cast<ScreenId>(id)) != nullptr);
  }
  CHECK(app::find_screen(table, static_cast<ScreenId>(app::kScreenCount)) == nullptr);
  CHECK(app::screen_at(table, -1) == nullptr);
  CHECK(app::screen_at(table, static_cast<int>(table.count)) == nullptr);
  CHECK(app::screen_position(table, static_cast<ScreenId>(200)) == -1);
  // Ids are what a binding carries as an argument: these numbers are stable.
  CHECK(static_cast<int>(ScreenId::Pattern) == 0);
  CHECK(static_cast<int>(ScreenId::Song) == 1);
  CHECK(static_cast<int>(ScreenId::Config) == 10);
  CHECK(app::screen_id_valid(0));
  CHECK(app::screen_id_valid(10));
  CHECK(static_cast<int>(ScreenId::InputTest) == 11);
  CHECK(app::screen_id_valid(11));
  CHECK(static_cast<int>(ScreenId::Modules) == 12);  // OS-012b
  CHECK(static_cast<int>(ScreenId::ModuleApp) == 13);
  CHECK(app::screen_id_valid(13));
  CHECK_FALSE(app::screen_id_valid(14));
  CHECK_FALSE(app::screen_id_valid(-1));
}

TEST_CASE("screens: the registry checker rejects what it must") {
  using ui::Icon;
  const app::ScreenEntry ok[] = {{ScreenId::Pattern, "A", nullptr, Button::Pattern, "A", Icon::Loop},
                                 {ScreenId::Song, "B", nullptr, Button::Song, "B", Icon::Play}};
  CHECK(app::screens_valid(ok, 2));
  const app::ScreenEntry same_id[] = {ok[0], {ScreenId::Pattern, "B", nullptr, Button::Song, "B", Icon::Play}};
  CHECK_FALSE(app::screens_valid(same_id, 2));
  const app::ScreenEntry same_name[] = {ok[0], {ScreenId::Song, "A", nullptr, Button::Song, "B", Icon::Play}};
  CHECK_FALSE(app::screens_valid(same_name, 2));
  const app::ScreenEntry same_button[] = {ok[0], {ScreenId::Song, "B", nullptr, Button::Pattern, "B", Icon::Play}};
  CHECK_FALSE(app::screens_valid(same_button, 2));
  const app::ScreenEntry empty_name[] = {{ScreenId::Pattern, "", nullptr, Button::Pattern, "A", Icon::Loop}};
  CHECK_FALSE(app::screens_valid(empty_name, 1));
  const app::ScreenEntry empty_key[] = {{ScreenId::Pattern, "A", nullptr, Button::Pattern, "", Icon::Loop}};
  CHECK_FALSE(app::screens_valid(empty_key, 1));
  const app::ScreenEntry long_name[] = {{ScreenId::Pattern, "TWELVE CHARS", nullptr, Button::Pattern, "A", Icon::Loop}};
  CHECK_FALSE(app::screens_valid(long_name, 1));  // a Navigator cell holds 11
  const app::ScreenEntry eleven[] = {{ScreenId::Pattern, "ELEVEN CHAR", nullptr, Button::Pattern, "ELEVEN CHAR", Icon::Loop}};
  CHECK(app::screens_valid(eleven, 1));
  const app::ScreenEntry bad_id[] = {{static_cast<ScreenId>(app::kScreenCount), "A", nullptr, Button::Pattern, "A", Icon::Loop}};
  CHECK_FALSE(app::screens_valid(bad_id, 1));
}

TEST_CASE("screens: every implemented screen declares its contexts, modes and F-key labels") {
  const app::ScreenTable table = app::screen_registry();
  std::vector<int> contexts;
  for (std::size_t i = 0; i < table.count; ++i) {
    const app::Screen* s = table.items[i].screen;
    if (s == nullptr) {
      continue;
    }
    const char* const* keys = s->soft_keys();
    CHECK(keys != nullptr);
    for (int k = 0; keys != nullptr && k < 8; ++k) {
      CHECK_MESSAGE(keys[k] != nullptr, table.items[i].name << " F" << k + 1);
      // Eight characters is what a key-cap of the row has room for (the F-number box and the label in 100 px).
      CHECK_MESSAGE((keys[k] != nullptr && std::strlen(keys[k]) <= 8), table.items[i].name << " F" << k + 1);
    }
    CHECK(s->pages() >= 1u);
    contexts.push_back(static_cast<int>(s->context()));
    const app::ModeTable modes = s->modes();
    for (unsigned m = 0; m < modes.count; ++m) {
      CHECK(modes.items[m].name != nullptr);
      CHECK(std::strlen(modes.items[m].name) >= 1u);
      contexts.push_back(static_cast<int>(modes.items[m].context));
      for (unsigned j = 0; j < m; ++j) {
        CHECK(std::strcmp(modes.items[j].name, modes.items[m].name) != 0);
      }
    }
  }
  // No two screens or modes share a binding context, and none is Global or an overlay's.
  for (std::size_t i = 0; i < contexts.size(); ++i) {
    CHECK(contexts[i] != static_cast<int>(app::Context::Global));
    CHECK(contexts[i] != static_cast<int>(app::Context::Learn));
    CHECK(contexts[i] != static_cast<int>(app::Context::Navigator));
    for (std::size_t j = 0; j < i; ++j) {
      CHECK(contexts[i] != contexts[j]);
    }
  }
  // The pattern view's modes are Edit, Follow (retired, OS-004d: its row keeps the numbering), Select (retired, OS-LIVEREC), Browse in the enum's order; the song has pages and no modes.
  const app::ModeTable pm = app::kPatternScreen.modes();
  CHECK(pm.count == app::kPatternModes);
  CHECK(std::string(pm.items[static_cast<unsigned>(app::PatternMode::Edit)].name) == "edit");
  CHECK(std::string(pm.items[static_cast<unsigned>(app::PatternMode::Follow)].name) == "follow");
  CHECK(pm.items[static_cast<unsigned>(app::PatternMode::Follow)].retired);
  CHECK_FALSE(pm.items[static_cast<unsigned>(app::PatternMode::Edit)].retired);
  CHECK(pm.items[static_cast<unsigned>(app::PatternMode::Select)].retired);
  CHECK_FALSE(pm.items[static_cast<unsigned>(app::PatternMode::Browse)].retired);
  CHECK(std::string(pm.items[static_cast<unsigned>(app::PatternMode::Select)].name) == "select");
  CHECK(app::kPatternScreen.pages() == 1u);
  CHECK(app::kSongScreen.modes().count == 0u);
  CHECK(app::kSongScreen.pages() == app::kSongPages);
  CHECK(app::kSongScreen.context() == app::Context::Song);
}

// ---- navigation ---------------------------------------------------------------------------------------------------------

TEST_CASE("screens: switching calls leave on the old screen, then enter on the new one, with view.screen changing in between") {
  std::vector<std::string> log;
  const Recorder a("A", &log, ScreenId::Pattern);
  const Recorder b("B", &log, ScreenId::Song);
  using ui::Icon;
  const app::ScreenEntry rows[] = {{ScreenId::Pattern, "A", &a, Button::Pattern, "A", Icon::Loop},
                                   {ScreenId::Song, "B", &b, Button::Song, "B", Icon::Play},
                                   {ScreenId::Instrument, "C", nullptr, Button::Params, "C", Icon::Module}};
  const app::ScreenTable table{rows, 3};
  app::Model model;
  app::ViewState view;  // starts on the pattern id: A

  CHECK(app::switch_screen(table, model, view, ScreenId::Song));
  CHECK(view.screen == ScreenId::Song);
  CHECK(log == std::vector<std::string>{"leave A in 0", "enter B in 1"});  // leave sees the old screen, enter the new
  log.clear();
  CHECK(app::switch_screen(table, model, view, ScreenId::Pattern));
  CHECK(log == std::vector<std::string>{"leave B in 1", "enter A in 0"});
  log.clear();

  // Nothing to do: the screen is already current, is not implemented, or is not in the table.
  CHECK_FALSE(app::switch_screen(table, model, view, ScreenId::Pattern));
  CHECK_FALSE(app::switch_screen(table, model, view, ScreenId::Instrument));
  CHECK_FALSE(app::switch_screen(table, model, view, ScreenId::Config));
  CHECK(view.screen == ScreenId::Pattern);
  CHECK(log.empty());
  // The overlay is not touched by a switch.
  view.overlay = OverlayId::Navigator;
  CHECK(app::switch_screen(table, model, view, ScreenId::Song));
  CHECK(view.overlay == OverlayId::Navigator);
}

TEST_CASE("screens: a screen button switches, its own button cycles the pages, a missing screen says so") {
  std::vector<std::string> log;
  Recorder a("A", &log, ScreenId::Pattern);
  Recorder b("B", &log, ScreenId::Song);
  b.page_count = 3;
  using ui::Icon;
  const app::ScreenEntry rows[] = {{ScreenId::Pattern, "A", &a, Button::Pattern, "A", Icon::Loop},
                                   {ScreenId::Song, "B", &b, Button::Song, "B", Icon::Play},
                                   {ScreenId::Instrument, "SOON", nullptr, Button::Params, "C", Icon::Module}};
  const app::ScreenTable table{rows, 3};
  app::Model model;
  app::ViewState view;
  const unsigned song = app::screen_index(ScreenId::Song);

  CHECK_FALSE(app::navigate_to(table, model, view, ScreenId::Pattern));  // A's own button: A has one page
  CHECK(view.page[0] == 0);
  CHECK(app::navigate_to(table, model, view, ScreenId::Song));
  CHECK(view.screen == ScreenId::Song);
  CHECK(view.page[song] == 0);
  for (const unsigned expect : {1u, 2u, 0u, 1u}) {  // B has three pages: its own button goes round
    CHECK(app::navigate_to(table, model, view, ScreenId::Song));
    CHECK(view.page[song] == expect);
  }
  CHECK(view.screen == ScreenId::Song);
  // The page is kept per screen: leaving and coming back finds it as it was left.
  CHECK(app::navigate_to(table, model, view, ScreenId::Pattern));
  CHECK(app::navigate_to(table, model, view, ScreenId::Song));
  CHECK(view.page[song] == 1);

  // A button whose screen does not exist: an info notification, nothing else changes.
  CHECK(view.notify.empty());
  CHECK(app::navigate_to(table, model, view, ScreenId::Instrument));
  CHECK(view.screen == ScreenId::Song);
  CHECK(view.notify.size() == 1u);
  CHECK(std::string(view.notify.head().text.c_str()) == "SOON IS NOT AVAILABLE YET");
  CHECK(view.notify.head().severity == core::Severity::Info);
  CHECK_FALSE(app::navigate_to(table, model, view, ScreenId::Config));  // not in this table at all
}

TEST_CASE("screens: the screen buttons take the base screen from Pattern to Song and back, and Song again cycles its pages") {
  Rig rig;
  CHECK(rig.screen() == ScreenId::Pattern);
  const std::uint32_t frames = rig.app->frames();
  rig.tap(Button::Song);
  CHECK(rig.screen() == ScreenId::Song);
  CHECK(rig.app->frames() == frames + 1u);
  CHECK(rig.state().page[app::screen_index(ScreenId::Song)] == 0);
  rig.tap(Button::Song);
  CHECK(rig.state().page[app::screen_index(ScreenId::Song)] == 1);
  rig.tap(Button::Song);
  CHECK(rig.state().page[app::screen_index(ScreenId::Song)] == 2);  // OS-SONG-TIMELINE: the third page
  rig.tap(Button::Song);
  CHECK(rig.state().page[app::screen_index(ScreenId::Song)] == 3);  // OS-SONG-STAGE: the fourth
  rig.tap(Button::Song);
  CHECK(rig.state().page[app::screen_index(ScreenId::Song)] == 0);  // four pages, wrapping
  rig.tap(Button::Pattern);
  CHECK(rig.screen() == ScreenId::Pattern);
  const std::uint32_t frames_back = rig.app->frames();
  rig.tap(Button::Pattern);  // Pattern's own button: one page, nothing to cycle, nothing to redraw
  CHECK(rig.screen() == ScreenId::Pattern);
  CHECK(rig.app->frames() == frames_back);
  CHECK(rig.app->failed_frames() == 0u);

  // The screens do not share a cursor: Song's keys move the song list, not the pattern.
  rig.tap(Button::Song);
  rig.tap(Button::Down);
  CHECK(rig.state().song.pos() == 1);
  CHECK(rig.app->view().row() == 0);
  rig.tap(Button::Pattern);
  rig.tap(Button::Down);
  CHECK(rig.app->view().row() == 1);
  CHECK(rig.state().song.pos() == 1);

  // The Record button opens the Record screen (OS-011c: every screen of the registry exists now).
  rig.tap(Button::SampleRec);
  CHECK(rig.screen() == ScreenId::Record);
  CHECK(rig.app->overlay() == OverlayId::None);

  // A screen that does not exist yet (a registry of the test's own has the hole): its button says so and the screen stays.
  const app::ScreenTable real = app::screen_registry();
  app::ScreenEntry rows[app::kScreenCount];
  for (std::size_t i = 0; i < real.count; ++i) {
    rows[i] = real.items[i];
    rows[i].screen = real.items[i].id == ScreenId::Record ? nullptr : real.items[i].screen;
  }
  app::ViewState view;
  CHECK(view.screen == ScreenId::Pattern);
  CHECK(app::navigate_to(app::ScreenTable{rows, real.count}, rig.app->model(), view, ScreenId::Record));  // the notification is the change
  CHECK(view.screen == ScreenId::Pattern);
  CHECK(std::string(view.shown_message()) == "RECORD IS NOT AVAILABLE YET");
  CHECK(view.shown_overlay() == OverlayId::Notify);
}

TEST_CASE("screens: nav.screen takes a screen id and refuses anything else") {
  Rig rig;
  const auto id = [](ScreenId s) { return static_cast<core::ActionArg>(s); };
  CHECK(rig.app->invoke(core::action_id("nav.screen"), id(ScreenId::Song)) == app::DispatchResult::Changed);
  CHECK(rig.screen() == ScreenId::Song);
  CHECK(rig.app->invoke(core::action_id("nav.screen"), -1) == app::DispatchResult::Unchanged);
  CHECK(rig.app->invoke(core::action_id("nav.screen"), 14) == app::DispatchResult::Unchanged);
  CHECK(rig.app->invoke(core::action_id("nav.screen"), 2147483647) == app::DispatchResult::Unchanged);
  CHECK(rig.screen() == ScreenId::Song);
  CHECK(rig.app->invoke(core::action_id("nav.screen"), id(ScreenId::Pattern)) == app::DispatchResult::Changed);
  CHECK(rig.screen() == ScreenId::Pattern);
}

// ---- the overlay slot: capture and fall-through --------------------------------------------------------------------------

TEST_CASE("overlay: Learn captures: what it does not bind never reaches the screen, the theme or the screen buttons") {
  Rig rig;
  rig.invoke("learn.toggle");
  rig.pass();
  CHECK(rig.app->learn());
  const std::uint32_t frames = rig.app->frames();
  rig.tap(Button::Right);          // the screen's arrows
  rig.tap(Button::Down);
  rig.turn(4);                     // the encoder
  rig.input.push(core::make_pad_event(rig.clock.now(), 5, 100));  // a pad
  rig.pass(1);
  rig.tap(Button::Song);           // a screen button
  rig.tap(Button::F3);             // an F-key
  rig.shift_tap(Button::Master);   // the theme switch
  rig.shift_tap(Button::F2);       // a jump
  CHECK(rig.app->view().column() == Column::Note);
  CHECK(rig.app->view().row() == 0);
  CHECK(rig.app->instrument() == 1);
  CHECK(rig.screen() == ScreenId::Pattern);
  CHECK(rig.app->theme() == ui::ThemeId::Amiga);
  CHECK(rig.state().notify.empty());
  CHECK(rig.app->frames() == frames);  // nothing changed, so nothing was drawn
  CHECK(rig.app->learn());
}

TEST_CASE("overlay: the FX1 + FX2 chord opens Learn and closes it again, from either screen and in either order") {
  Rig rig;
  const auto chord = [&](Button first, Button second) {
    rig.press(first);
    rig.pass(1);
    rig.press(second);
    rig.pass(1);
    rig.release(first);
    rig.release(second);
    rig.pass(1);
  };
  CHECK_FALSE(rig.app->learn());
  chord(Button::Vol, Button::Fx);
  CHECK(rig.app->learn());
  CHECK(rig.app->overlay() == OverlayId::Learn);
  CHECK(rig.app->view().column() == Column::Note);  // neither button was a column tap
  chord(Button::Fx, Button::Vol);
  CHECK_FALSE(rig.app->learn());
  CHECK(rig.app->overlay() == OverlayId::None);
  CHECK(rig.app->view().column() == Column::Note);
  rig.tap(Button::Song);
  chord(Button::Vol, Button::Fx);
  CHECK(rig.app->learn());
  CHECK(rig.screen() == ScreenId::Song);  // over the song screen too
  chord(Button::Vol, Button::Fx);
  CHECK_FALSE(rig.app->learn());
}

TEST_CASE("overlay: F8 (Back) closes Learn, and does nothing when no overlay is up") {
  Rig rig;
  rig.invoke("learn.toggle");
  rig.pass();
  CHECK(rig.app->learn());
  rig.tap(Button::F8);
  CHECK_FALSE(rig.app->learn());
  CHECK(rig.state().notify.size() == 0u);  // it was Back, not the F8 label
  rig.tap(Button::F8);                       // with no overlay it is UNDO (OS-002c): and no document is attached
  CHECK(rig.message() == "NOTHING TO UNDO HERE");
  CHECK(rig.app->invoke(core::action_id("ui.back")) == app::DispatchResult::Unchanged);
}

TEST_CASE("overlay: a notification does not capture: the screen's keys work under its strip") {
  Rig rig;
  rig.tap(Button::F3);
  CHECK(rig.app->overlay() == OverlayId::Notify);
  rig.tap(Button::Right);
  rig.tap(Button::Down);
  rig.tap(Button::Song);
  CHECK(rig.screen() == ScreenId::Song);
  CHECK(rig.app->overlay() == OverlayId::Notify);
  CHECK(rig.message() == "F3 JUMP");  // still showing, over the other screen
  rig.tap(Button::Down);
  CHECK(rig.state().song.pos() == 1);
}

// ---- the Navigator --------------------------------------------------------------------------------------------------------

TEST_CASE("navigator: Shift + Pattern tapped opens it on the current screen and it stays open after the keys are let go (OS-NAV2)") {
  Rig rig;
  rig.open_navigator();  // Shift down, Pattern tapped, Shift up
  CHECK(rig.state().overlay == OverlayId::Navigator);
  CHECK(rig.state().navigator == 0);  // Pattern's own cell
  const std::uint32_t frames = rig.app->frames();
  rig.pass(5000);                      // nothing closes it, and nothing fires again
  CHECK(rig.state().overlay == OverlayId::Navigator);
  CHECK(rig.app->frames() == frames);
  CHECK(rig.screen() == ScreenId::Pattern);
  rig.tap(Button::Right);
  CHECK(rig.state().navigator == 1);   // Song
  CHECK(rig.screen() == ScreenId::Pattern);
  CHECK(rig.state().overlay == OverlayId::Navigator);
  rig.tap(Button::F8);                 // GO
  CHECK(rig.screen() == ScreenId::Song);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.app->failed_frames() == 0u);

  // Opened on the screen in view: from Song it starts on Song's cell, and F8 at once changes nothing.
  rig.open_navigator();
  CHECK(rig.state().overlay == OverlayId::Navigator);
  CHECK(rig.state().navigator == 1);
  rig.tap(Button::F8);
  CHECK(rig.screen() == ScreenId::Song);
  CHECK(rig.state().overlay == OverlayId::None);
}

TEST_CASE("navigator: Shift + Pattern held opens it too, and letting go of Pattern or of Shift, in either order, does nothing (no accept on a release)") {
  Rig rig;
  rig.open_navigator_held();
  CHECK(rig.state().overlay == OverlayId::Navigator);
  CHECK(rig.state().navigator == 0);
  rig.tap(Button::Right);             // Shift is still down
  CHECK(rig.state().navigator == 1);  // Song
  const std::uint32_t frames = rig.app->frames();
  rig.pass(5000);                     // still held: nothing fires again
  CHECK(rig.state().overlay == OverlayId::Navigator);
  CHECK(rig.app->frames() == frames);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Pattern);
  CHECK(rig.state().overlay == OverlayId::Navigator);
  CHECK(rig.app->failed_frames() == 0u);

  // Shift first this time, then Pattern, after a long hold.
  rig.tap(Button::F7);  // CLOSE
  CHECK(rig.state().overlay == OverlayId::None);
  rig.open_navigator_held();
  rig.tap(Button::Right);
  rig.release(Button::Shift);
  rig.pass(1);
  rig.pass(2000);
  CHECK(rig.screen() == ScreenId::Pattern);
  CHECK(rig.state().overlay == OverlayId::Navigator);
  rig.tap(Button::F1);  // the second GO
  CHECK(rig.screen() == ScreenId::Song);
  CHECK(rig.state().overlay == OverlayId::None);
}

TEST_CASE("navigator: a plain Pattern press, short or long, is the Pattern button and opens nothing") {
  Rig rig;
  rig.tap(Button::Song);
  CHECK(rig.screen() == ScreenId::Song);
  rig.press(Button::Pattern);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Song);  // it acts at the release (OS-PATTEMPO: the press waits for a turn or a pad)
  CHECK(rig.state().overlay == OverlayId::None);
  rig.pass(core::kGestureTimings.hold_ms + 600);  // far past the hold time: still a tap at the release, no hold
  rig.release(Button::Pattern);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Pattern);
  CHECK(rig.state().overlay == OverlayId::None);
  rig.tap(Button::Song);
  rig.tap(Button::Pattern);  // a tap, from another screen
  CHECK(rig.screen() == ScreenId::Pattern);
  CHECK(rig.state().overlay == OverlayId::None);
  rig.tap(Button::Pattern);  // and on Pattern itself (OS-ZOOM will give this one a meaning of its own): no overlay, no change of screen
  CHECK(rig.screen() == ScreenId::Pattern);
  CHECK(rig.state().overlay == OverlayId::None);
  rig.tap(Button::Song);
  rig.press(Button::Params);  // the other screen buttons act at the press (Song and Master wait for their release since OS-SONGPEEK / OS-MASTERVOL)
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Instrument);
  rig.release(Button::Params);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Instrument);
}

// ---- OS-SONGPEEK: a held Song shows Song until the key is let go ----------------------------------------------------------

TEST_CASE("song peek: the Tap of Song acts at the release now, not at the press") {
  Rig rig;
  rig.press(Button::Song);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Pattern);  // the press waits: Song is bound to a hold
  rig.release(Button::Song);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Song);
  CHECK_FALSE(rig.state().peeking);  // a tap is not a peek
}

TEST_CASE("song peek: Song held shows Song, the release goes back to the screen it came from, from any screen") {
  for (const Button from : {Button::Pattern, Button::Params, Button::Master, Button::File}) {
    Rig rig;
    rig.tap(from);
    const ScreenId before = rig.screen();
    NEED(before != ScreenId::Song);
    rig.press(Button::Song);
    rig.pass(1);
    CHECK(rig.screen() == before);  // not yet: a tap is still possible
    rig.pass(core::kGestureTimings.hold_ms + 1);
    CHECK(rig.screen() == ScreenId::Song);
    CHECK(rig.state().peeking);
    rig.pass(600);  // the peek lasts as long as the key is down
    CHECK(rig.screen() == ScreenId::Song);
    rig.release(Button::Song);
    rig.pass(1);
    CHECK(rig.screen() == before);
    CHECK_FALSE(rig.state().peeking);
    rig.tap(Button::Song);  // the next tap is a plain tap
    CHECK(rig.screen() == ScreenId::Song);
    rig.tap(Button::Song);   // and again on Song: its next page, no way back to anywhere
    CHECK(rig.screen() == ScreenId::Song);
  }
}

TEST_CASE("song peek: held on Song itself it does nothing, and a screen button pressed under the held key ends the peek there") {
  Rig rig;
  rig.tap(Button::Song);
  rig.press(Button::Song);
  rig.pass(core::kGestureTimings.hold_ms + 1);
  CHECK_FALSE(rig.state().peeking);  // already Song: nothing to peek at
  rig.release(Button::Song);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Song);

  rig.tap(Button::Master);  // the Mixer
  rig.press(Button::Song);
  rig.pass(core::kGestureTimings.hold_ms + 1);
  CHECK(rig.screen() == ScreenId::Song);
  rig.tap(Button::Params);  // another screen under the held Song: the user goes there
  CHECK(rig.screen() == ScreenId::Instrument);
  CHECK_FALSE(rig.state().peeking);
  rig.release(Button::Song);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Instrument);  // the release leaves it where the user went
}

TEST_CASE("song peek: a reset of the input under the held key (a lost release) leaves the screen where it is") {
  Rig rig;
  rig.tap(Button::Master);
  rig.press(Button::Song);
  rig.pass(core::kGestureTimings.hold_ms + 1);
  CHECK(rig.screen() == ScreenId::Song);
  CHECK(rig.state().peeking);
  rig.app->reset_input();
  CHECK_FALSE(rig.state().peeking);
  rig.release(Button::Song);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Song);
}

TEST_CASE("song peek: a modal overlay opened under the held key takes the release, and a later hold on Song does not jump back") {
  Rig rig;
  rig.tap(Button::Master);  // the Mixer
  rig.press(Button::Song);
  rig.pass(core::kGestureTimings.hold_ms + 1);
  NEED(rig.screen() == ScreenId::Song);
  CHECK(rig.state().peeking);
  rig.invoke("navigator.open");  // an overlay appears under the held key: its context captures the HoldEnd below
  NEED(rig.state().overlay == OverlayId::Navigator);
  rig.release(Button::Song);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Song);  // the overlay took the release: nothing went back
  rig.invoke("ui.back");
  NEED(rig.state().overlay == OverlayId::None);
  rig.press(Button::Song);  // a later hold on Song itself: nothing to peek at, and no way back to the Mixer
  rig.pass(core::kGestureTimings.hold_ms + 1);
  CHECK_FALSE(rig.state().peeking);
  rig.release(Button::Song);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Song);
}

// Accepted limit (QA-SNG-060 / 061, the CHANGELOG): a Shift that joins after the Song press ends the tap, as for Master. Pinned so that a change of the recogniser shows here.
TEST_CASE("song peek: a Shift joined after the Song press loses the tap") {
  Rig rig;
  rig.press(Button::Song);
  rig.pass(1);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.release(Button::Song);
  rig.pass(1);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Pattern);
  CHECK_FALSE(rig.state().peeking);
}

TEST_CASE("song peek: the peek keeps the pages and cursors of both screens as they were") {
  Rig rig;
  rig.tap(Button::Master);
  rig.tap(Button::Master);  // the Mixer's second page
  const std::uint8_t mixer_page = rig.state().page[app::screen_index(ScreenId::Mixer)];
  NEED(mixer_page != 0);
  rig.tap(Button::Song);
  rig.tap(Button::Down);
  const int song_pos = rig.state().song.pos();
  NEED(song_pos > 0);
  rig.tap(Button::Master);
  rig.press(Button::Song);
  rig.pass(core::kGestureTimings.hold_ms + 1);
  CHECK(rig.screen() == ScreenId::Song);
  CHECK(rig.state().song.pos() == song_pos);
  rig.release(Button::Song);
  rig.pass(1);
  CHECK(rig.screen() == ScreenId::Mixer);
  CHECK(rig.state().page[app::screen_index(ScreenId::Mixer)] == mixer_page);
}

TEST_CASE("navigator: F7 and Pattern CLOSE it without changing the view, whatever is chosen") {
  Rig rig;
  rig.tap(Button::Song);
  rig.open_navigator();
  rig.tap(Button::Down);  // Sample load (a row of four below)
  CHECK(rig.state().navigator == 5);
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.screen() == ScreenId::Song);  // not the chosen cell, not Pattern

  rig.open_navigator();
  CHECK(rig.state().navigator == 1);  // opens on the screen in view again
  rig.tap(Button::Right);
  rig.tap(Button::Pattern);  // Pattern again
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.screen() == ScreenId::Song);

  // Shift + Pattern again is Pattern again: it closes (it does not reopen, nor go to Pattern's screen).
  rig.open_navigator();
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Pattern);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.screen() == ScreenId::Song);
  rig.pass(2000);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.app->failed_frames() == 0u);
}

TEST_CASE("navigator: the arrows and the encoder choose, F8 and F1 GO, F7 CLOSE, every cell can be opened") {
  Rig rig;
  rig.open_navigator();
  const auto sel = [&] { return rig.state().navigator; };
  CHECK(sel() == 0);
  rig.tap(Button::Down);  // a row of four cells
  CHECK(sel() == 4);
  rig.tap(Button::Right);
  CHECK(sel() == 5);
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  CHECK(sel() == 3);
  rig.turn(2);            // the encoder: two cells on
  rig.pass(1);
  CHECK(sel() == 5);
  rig.turn(-100);
  rig.pass(1);
  CHECK(sel() == 0);      // clamped
  CHECK(rig.state().overlay == OverlayId::Navigator);
  rig.tap(Button::F8);    // GO on Pattern's own cell: it simply closes
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.screen() == ScreenId::Pattern);

  // F1 is a second GO.
  rig.open_navigator();
  CHECK(rig.state().overlay == OverlayId::Navigator);
  rig.tap(Button::Right);
  rig.tap(Button::F1);
  CHECK(rig.screen() == ScreenId::Song);
  CHECK(rig.state().overlay == OverlayId::None);

  // F8 is GO, F7 is CLOSE: the encoder moved the selection to Pattern's cell, F7 leaves the view alone.
  rig.open_navigator();
  rig.tap(Button::Left);
  CHECK(sel() == 0);
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.screen() == ScreenId::Song);

  // Every cell can be opened since OS-011c (a disabled cell is the registry's nullptr: screens_test's navigate_to case): F8 on Record opens it.
  rig.open_navigator();
  rig.tap(Button::Down);   // a row of four cells below Song: Sample load
  rig.tap(Button::Right);  // Record
  CHECK(sel() == 6);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.screen() == ScreenId::Record);
}

TEST_CASE("navigator: Shift going down or up while it is up redraws its key row (the layer with the jumps), and the screen's own row needs no restore") {
  Rig rig;
  rig.open_navigator();
  const std::uint32_t frames = rig.app->frames();
  rig.press(Button::Shift);
  rig.pass(1);
  CHECK(rig.app->frames() > frames);  // the key row shows the Shift layer
  const std::uint32_t held = rig.app->frames();
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.app->frames() > held);    // and gives it back
  const std::uint32_t before_close = rig.app->frames();
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.app->frames() > before_close);  // closing redraws the screen, whose own key row is drawn again
  CHECK(rig.app->failed_frames() == 0u);
}

TEST_CASE("navigator: no quarter jumps inside it (OS-SHIFTROW): Shift + F1..F5 do nothing, and never go or close") {
  Rig rig;
  rig.open_navigator();
  const int at = rig.state().navigator;
  const Button keys[5] = {Button::F1, Button::F2, Button::F3, Button::F4, Button::F5};
  for (int n = 0; n < 5; ++n) {
    rig.shift_tap(keys[n]);
    CHECK_MESSAGE(rig.state().navigator == at, "Shift+F" << n + 1);
    CHECK(rig.state().overlay == OverlayId::Navigator);
    CHECK(rig.screen() == ScreenId::Pattern);
  }
  // The same with Shift still held from the opening (Shift + Pattern held, the jump with the other hand).
  rig.tap(Button::F7);
  rig.open_navigator_held();
  rig.tap(Button::F5);  // Shift is down, so this is Shift + F5: nothing
  CHECK(rig.state().navigator == at);
  CHECK(rig.state().overlay == OverlayId::Navigator);
  rig.release(Button::Pattern);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().overlay == OverlayId::Navigator);
}

TEST_CASE("navigator: while it is up everything else is swallowed, and CLOSE with the opening buttons still down is harmless") {
  Rig rig;
  rig.open_navigator_held();  // Shift and Pattern both still down
  rig.release(Button::Shift);
  rig.pass(1);  // (the release of Shift redraws the Navigator's key row: the layer goes)
  const std::uint32_t frames = rig.app->frames();
  rig.input.push(core::make_pad_event(rig.clock.now(), 3, 100));
  rig.pass(1);
  rig.tap(Button::Song);      // a screen button: not while choosing
  rig.tap(Button::F4);
  CHECK(rig.app->frames() == frames);  // nothing swallowed redraws
  rig.shift_tap(Button::Master);       // (Shift itself redraws the key row of the Navigator, going down and up: two frames)
  CHECK(rig.app->frames() == frames + 2u);
  CHECK(rig.app->instrument() == 1);
  CHECK(rig.screen() == ScreenId::Pattern);
  CHECK(rig.state().notify.empty());
  CHECK(rig.app->theme() == ui::ThemeId::Amiga);
  CHECK(rig.state().overlay == OverlayId::Navigator);
  // The Learn chord does not open Learn over the Navigator, and the Navigator's own context has no chord.
  rig.press(Button::Vol);
  rig.pass(1);
  rig.press(Button::Fx);
  rig.pass(1);
  rig.release(Button::Vol);
  rig.release(Button::Fx);
  rig.pass(1);
  CHECK_FALSE(rig.app->learn());
  CHECK(rig.state().overlay == OverlayId::Navigator);

  rig.tap(Button::F7);  // CLOSE, with Pattern still down
  CHECK(rig.state().overlay == OverlayId::None);
  rig.release(Button::Pattern);  // the release of the opening button now belongs to the screen: nobody binds it
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.screen() == ScreenId::Pattern);
  CHECK(rig.state().notify.empty());
}

TEST_CASE("navigator: the grid moves by cell and by row and nothing moves past either end") {
  Rig rig;
  rig.invoke("navigator.open");
  const auto run = [&](const char* name, core::ActionArg arg = 0) { return rig.app->invoke(core::action_id(name), arg); };
  const auto sel = [&] { return rig.state().navigator; };
  CHECK(sel() == 0);
  CHECK(run("nav.up") == app::DispatchResult::Unchanged);   // no row above
  CHECK(run("nav.left") == app::DispatchResult::Unchanged);
  CHECK(run("nav.down") == app::DispatchResult::Changed);
  CHECK(sel() == 4);
  CHECK(run("nav.down") == app::DispatchResult::Changed);
  CHECK(sel() == 8);
  CHECK(run("nav.down") == app::DispatchResult::Unchanged);  // 12 cells (OS-012b added MODULES), rows 0-3, 4-7, 8-11: no fourth row
  run("nav.right");
  run("nav.right");
  CHECK(sel() == 10);
  CHECK(run("nav.right") == app::DispatchResult::Changed);
  CHECK(sel() == 11);
  CHECK(run("nav.right") == app::DispatchResult::Unchanged);  // the last cell
  CHECK(run("nav.up") == app::DispatchResult::Changed);
  CHECK(sel() == 7);
  CHECK(run("nav.down") == app::DispatchResult::Changed);
  CHECK(sel() == 11);
  CHECK(run("nav.scroll", 0) == app::DispatchResult::Unchanged);
  CHECK(run("nav.scroll", -3) == app::DispatchResult::Changed);
  CHECK(sel() == 8);
  CHECK(run("nav.scroll", 2147483647) == app::DispatchResult::Changed);  // a huge detent count cannot overflow
  CHECK(sel() == 11);
  CHECK(run("nav.scroll", -2147483647 - 1) == app::DispatchResult::Changed);
  CHECK(sel() == 0);
  // The pattern grid under it did not move.
  CHECK((rig.app->view().row() == 0 && rig.app->view().column() == Column::Note));
}

// ---- jumps and the navigation keys ---------------------------------------------------------------------------------------

TEST_CASE("pads mode keys: Shift + F2..F7 choose the pads' mode on Pattern (F2 NOTES, F3 PIANO, F4 ISO, F5 TRACKS, F6 PATS, F7 VU; F1 is the octave, OS-PADOCT), none of them jumps; plain F-keys still say their labels") {
  Rig rig;
  const auto fixed = [&] { return rig.app->state().padmode.fixed(); };
  CHECK(fixed() == app::PadMode::Notes);
  rig.shift_tap(Button::F5);
  CHECK(fixed() == app::PadMode::Tracks);
  rig.shift_tap(Button::F3);
  CHECK(fixed() == app::PadMode::Piano);
  rig.shift_tap(Button::F2);
  CHECK(fixed() == app::PadMode::Notes);
  rig.shift_tap(Button::F4);
  CHECK(fixed() == app::PadMode::Iso);
  rig.shift_tap(Button::F1);
  CHECK(fixed() == app::PadMode::Iso);  // OS-PADOCT: Shift + F1 is the octave, the mode stays
  rig.shift_tap(Button::F6);
  CHECK(fixed() == app::PadMode::Patterns);
  rig.shift_tap(Button::F5);
  rig.shift_tap(Button::F7);
  CHECK(fixed() == app::PadMode::Vu);
  CHECK(rig.app->view().row() == 0);  // (the quarter jumps of Shift + F1..F5 are the Song list's, File's and Sample load's own now)
  CHECK(rig.state().notify.empty());  // a Shift + F-key is not the F-key's label
  rig.tap(Button::F2);
  CHECK(rig.message() == "F2 LENGTH");
  CHECK(rig.app->view().row() == 0);
}

TEST_CASE("jumps: the same keys do the same on the song list and in the Navigator, over its own length") {
  Rig rig;
  rig.tap(Button::Song);
  const Button keys[5] = {Button::F1, Button::F2, Button::F3, Button::F4, Button::F5};
  const int song[5] = {0, 6, 12, 18, 23};  // a quarter of 24 positions, and the last
  rig.shift_tap(Button::F5);
  CHECK(rig.state().song.pos() == 23);
  CHECK(rig.state().song.top() == 8);
  for (int n = 0; n < 5; ++n) {
    rig.shift_tap(keys[n]);
    CHECK_MESSAGE(rig.state().song.pos() == song[n], "song Shift+F" << n + 1);
  }
  const int song_pos = rig.state().song.pos();
  rig.open_navigator();
  const int at = rig.state().navigator;
  for (int n = 0; n < 5; ++n) {
    rig.shift_tap(keys[n]);
    CHECK_MESSAGE(rig.state().navigator == at, "navigator Shift+F" << n + 1);  // OS-SHIFTROW: the Navigator has no jumps
  }
  CHECK(rig.state().overlay == OverlayId::Navigator);
  CHECK(rig.state().song.pos() == song_pos);  // the list under it did not move
}

TEST_CASE("jumps: the navigation actions take their arguments, refuse the impossible and clamp") {
  Rig rig;
  const auto run = [&](const char* name, core::ActionArg arg = 0) { return rig.app->invoke(core::action_id(name), arg); };
  CHECK(run("cursor.jump_fraction", 2) == app::DispatchResult::Changed);
  CHECK(rig.app->view().row() == 32);
  CHECK(run("cursor.jump_fraction", 2) == app::DispatchResult::Unchanged);  // already there
  CHECK(run("cursor.jump_fraction", 4) == app::DispatchResult::Unchanged);  // 0..3 only
  CHECK(run("cursor.jump_fraction", -1) == app::DispatchResult::Unchanged);
  CHECK(rig.app->view().row() == 32);
  CHECK(run("cursor.page", 1) == app::DispatchResult::Changed);
  CHECK(rig.app->view().row() == 48);
  CHECK(run("cursor.page", 5) == app::DispatchResult::Changed);  // clamped to the end
  CHECK(rig.app->view().row() == 63);
  CHECK(run("cursor.page", 1) == app::DispatchResult::Unchanged);
  CHECK(run("cursor.page", -2) == app::DispatchResult::Changed);
  CHECK(rig.app->view().row() == 31);
  CHECK(run("cursor.page", 2147483647) == app::DispatchResult::Changed);  // no overflow
  CHECK(rig.app->view().row() == 63);
  CHECK(run("cursor.jump_end") == app::DispatchResult::Unchanged);
  CHECK(run("cursor.jump_fraction", 0) == app::DispatchResult::Changed);
  CHECK(rig.app->view().row() == 0);

  CHECK(run("cursor.track", 1) == app::DispatchResult::Changed);
  CHECK(rig.app->view().track() == 1);
  CHECK(rig.app->view().column() == Column::Note);
  CHECK(run("pattern.column", 3) == app::DispatchResult::Changed);
  CHECK(run("cursor.track", 3) == app::DispatchResult::Changed);
  CHECK((rig.app->view().track() == 4 && rig.app->view().column() == Column::Fx1));  // the column stays
  CHECK(run("cursor.track", 100) == app::DispatchResult::Changed);
  CHECK(rig.app->view().track() == 11);
  CHECK(run("cursor.track", 1) == app::DispatchResult::Unchanged);
  CHECK(run("cursor.track", -2147483647 - 1) == app::DispatchResult::Changed);
  CHECK(rig.app->view().track() == 0);
  CHECK(run("cursor.end") == app::DispatchResult::Changed);
  CHECK((rig.app->view().track() == 11 && rig.app->view().column() == Column::Fx2));
  CHECK(run("cursor.end") == app::DispatchResult::Unchanged);
  CHECK(run("cursor.home") == app::DispatchResult::Changed);
  CHECK((rig.app->view().track() == 0 && rig.app->view().column() == Column::Note));
  CHECK(run("cursor.home") == app::DispatchResult::Unchanged);
  CHECK(rig.app->view().row() == 0);  // home and end are along the tracks: the step stays

  // On the song list there are no tracks: that action does nothing there; home and end are the ends of the list.
  rig.app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(ScreenId::Song));
  CHECK(run("cursor.track", 1) == app::DispatchResult::Unchanged);
  CHECK(run("cursor.end") == app::DispatchResult::Changed);
  CHECK(rig.state().song.pos() == 23);
  CHECK(run("cursor.page", -1) == app::DispatchResult::Changed);
  CHECK(rig.state().song.pos() == 7);
  CHECK(run("cursor.home") == app::DispatchResult::Changed);
  CHECK(rig.state().song.pos() == 0);
  CHECK(run("nav.left") == app::DispatchResult::Unchanged);  // one axis
  CHECK(run("nav.right") == app::DispatchResult::Unchanged);
}

TEST_CASE("holdend: a binding on the end of a hold makes its button wait for the hold, with no Hold binding of its own") {
  Rig rig;
  const std::string keymap = "o3keymap 1\npattern holdend f5 cursor.jump_end\n";
  CHECK(rig.app->load_user_keymap(keymap.data(), keymap.size()).ok());
  // A short press is still F5 PREVIEW (the bare app has no engine and says so), now delivered at the release (the button waits for a hold that may come).
  rig.press(Button::F5);
  rig.pass(100);
  CHECK(rig.message() == "");
  rig.release(Button::F5);
  rig.pass(1);
  CHECK(rig.message() == "PREVIEW: NO AUDIO OUTPUT");
  CHECK(rig.app->view().row() == 0);
  rig.pass(2000);
  // Held past the hold time: nothing runs while it is down (the hold itself is unbound), the release runs the end.
  rig.press(Button::F5);
  rig.pass(core::kGestureTimings.hold_ms + 100);
  CHECK(rig.app->view().row() == 0);
  CHECK(rig.state().notify.empty());
  rig.release(Button::F5);
  rig.pass(1);
  CHECK(rig.app->view().row() == 63);
  CHECK(rig.state().notify.empty());  // and it was not also a tap
}

TEST_CASE("jumps: any gesture can call them: a user keymap puts them on a hold, a double tap, a chord and hold + encoder") {
  Rig rig;
  const std::string keymap =
      "o3keymap 1\n"
      "pattern hold f6 cursor.jump_end\n"
      "pattern double f7 cursor.jump_fraction 1\n"
      "pattern chord play+rec cursor.jump_fraction 2\n"
      "pattern holdenc instr+enc0 cursor.page @value\n"
      "pattern tap shift+f8 cursor.home\n"
      "song tap f6 cursor.jump_end\n";
  CHECK(rig.app->load_user_keymap(keymap.data(), keymap.size()).ok());
  // hold
  rig.press(Button::F6);
  rig.pass(core::kGestureTimings.hold_ms);
  CHECK(rig.app->view().row() == 63);
  rig.release(Button::F6);
  rig.pass(1);
  rig.invoke("cursor.jump_fraction", 0);
  // double tap
  rig.tap(Button::F7);
  rig.tap(Button::F7);
  CHECK(rig.app->view().row() == 16);
  // chord
  rig.press(Button::Play);
  rig.pass(1);
  rig.press(Button::Rec);
  rig.pass(1);
  rig.release(Button::Play);
  rig.release(Button::Rec);
  rig.pass(1);
  CHECK(rig.app->view().row() == 32);
  // hold + encoder
  rig.press(Button::Instr);
  rig.pass(1);
  rig.turn(1);
  rig.pass(1);
  CHECK(rig.app->view().row() == 48);
  rig.turn(-2);
  rig.pass(1);
  CHECK(rig.app->view().row() == 16);
  rig.release(Button::Instr);
  rig.pass(1);
  // Shift
  rig.shift_tap(Button::F8);
  CHECK((rig.app->view().track() == 0 && rig.app->view().column() == Column::Note));
  CHECK(rig.app->view().row() == 16);
  // a context of its own: the song's F6 is bound there, and not in the pattern's one
  rig.tap(Button::Song);
  rig.tap(Button::F6);
  CHECK(rig.state().song.pos() == 23);
}

// ---- a held Shift ---------------------------------------------------------------------------------------------------------

TEST_CASE("shift: a Shift held down survives a mode change, a screen change and an overlay opening and closing") {
  Rig rig;
  const unsigned pattern = app::screen_index(ScreenId::Pattern);
  rig.press(Button::Shift);
  rig.pass(1);

  // A mode change (the context chain changes under the held Shift): Edit following by view.mode (Shift + Rec from stopped starts a transport, which this rig has none of), then STEP by Shift + REC (OS-RECMODE2, OS-LIVEEXIT).
  CHECK(rig.app->invoke(core::action_id("view.mode"), 0) == app::DispatchResult::Changed);
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(app::PatternMode::Edit));
  CHECK_FALSE(rig.state().edit.rec_free);
  CHECK(rig.app->invoke(core::action_id("pattern.rec_free")) == app::DispatchResult::Changed);
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(app::PatternMode::Edit));
  CHECK(rig.state().edit.rec_free);

  // A screen change: Shift + F4 is the jump on the song screen as well, not the F4 key of its own.
  rig.tap(Button::Song);
  CHECK(rig.screen() == ScreenId::Song);
  rig.tap(Button::F4);
  CHECK(rig.state().song.pos() == 18);
  CHECK(rig.state().notify.empty());
  rig.tap(Button::Pattern);  // Shift + Pattern is the Navigator (OS-NAV2), opened on Song's cell; Shift stays held through its opening
  CHECK(rig.state().overlay == OverlayId::Navigator);
  rig.tap(Button::Left);
  CHECK(rig.state().navigator == 0);
  rig.tap(Button::F8);  // GO, with Shift still held (F8 has no Shift binding there: the plain one runs)
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.screen() == ScreenId::Pattern);
  rig.tap(Button::F5);  // on Pattern Shift + F5 is the TRACKS pads' mode key (OS-PADS-A, OS-PADOCT)
  CHECK(rig.state().padmode.fixed() == app::PadMode::Tracks);
  CHECK(rig.state().notify.empty());

  // An overlay opening and closing: Learn is opened by the chord with Shift still held, and closes by it.
  rig.press(Button::Vol);
  rig.pass(1);
  rig.press(Button::Fx);
  rig.pass(1);
  rig.release(Button::Vol);
  rig.release(Button::Fx);
  rig.pass(1);
  CHECK(rig.app->learn());
  rig.press(Button::Fx);
  rig.pass(1);
  rig.press(Button::Vol);
  rig.pass(1);
  rig.release(Button::Fx);
  rig.release(Button::Vol);
  rig.pass(1);
  CHECK_FALSE(rig.app->learn());
  rig.tap(Button::F3);
  CHECK(rig.state().padmode.fixed() == app::PadMode::Piano);  // Shift is still held after Learn came and went

  // The Navigator: the tap of Pattern opens it, Shift still modifies inside it (Shift + F2 jumps the grid), CLOSE (F7 has no Shift binding there:
  // the plain one runs) closes it, and Shift still modifies afterwards.
  rig.tap(Button::Pattern);
  CHECK(rig.state().overlay == OverlayId::Navigator);
  rig.tap(Button::F2);
  CHECK(rig.state().navigator == 0);  // (OS-SHIFTROW: Shift + F2 is nothing there; no jump)
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.screen() == ScreenId::Pattern);
  rig.tap(Button::F2);  // (OS-PADOCT: Shift + F2 is NOTES; Shift + F1 is the octave)
  CHECK(rig.state().padmode.fixed() == app::PadMode::Notes);
  CHECK(rig.state().notify.empty());

  // Letting go of Shift gives the plain keys back.
  rig.release(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F2);  // (F2 is the LENGTH chooser, which with no document says its label)
  CHECK(rig.message() == "F2 LENGTH");
  CHECK(rig.state().padmode.fixed() == app::PadMode::Notes);
}

// ---- modes ---------------------------------------------------------------------------------------------------------------

TEST_CASE("modes: Shift + Rec is not a cycle of modes (OS-LIVEREC): stopped with no transport it changes nothing; Rec is STEP, then WATCH again (OS-RECMODE2); the mode is the screen's, and kept") {
  Rig rig;
  const unsigned pattern = app::screen_index(ScreenId::Pattern);
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(app::PatternMode::Browse));  // the default
  rig.tap(Button::Rec);
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(app::PatternMode::Edit));
  CHECK(rig.state().edit.rec_free);  // OS-RECMODE2: from WATCH, STEP
  rig.shift_tap(Button::Rec);  // stopped, no transport to start: nothing changes
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(app::PatternMode::Edit));  // (not Select, which is gone)
  CHECK(rig.state().edit.rec_free);
  rig.tap(Button::Song);
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(app::PatternMode::Edit));  // kept while away
  const std::uint32_t frames = rig.app->frames();
  rig.shift_tap(Button::Rec);  // the song has no modes and no recording: nothing to change, nothing to draw but the key row's Shift layer (OS-SONG1: it shows it, going and coming)
  CHECK(rig.app->frames() == frames + 2u);
  CHECK(rig.state().edit.rec_free);  // (the Pattern's state, untouched)
  rig.tap(Button::Pattern);
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(app::PatternMode::Edit));
  rig.tap(Button::Rec);
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(app::PatternMode::Browse));

  // view.mode picks one by number, refuses what is not a mode or is retired (Follow, 1, and Select, 2), and needs the base screen's focus.
  CHECK(rig.app->invoke(core::action_id("view.mode"), 1) == app::DispatchResult::Unchanged);
  CHECK(rig.app->invoke(core::action_id("view.mode"), 2) == app::DispatchResult::Unchanged);
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(app::PatternMode::Browse));
  CHECK(rig.app->invoke(core::action_id("view.mode"), 0) == app::DispatchResult::Changed);
  CHECK(rig.state().mode[pattern] == 0);
  CHECK(rig.app->invoke(core::action_id("view.mode"), 0) == app::DispatchResult::Unchanged);
  CHECK(rig.app->invoke(core::action_id("view.mode"), 4) == app::DispatchResult::Unchanged);
  CHECK(rig.app->invoke(core::action_id("view.mode"), -1) == app::DispatchResult::Unchanged);
  CHECK(rig.state().mode[pattern] == 0);
  // the action that cycles is still there for a keymap; it skips what is retired (Edit and Browse)
  CHECK(rig.app->invoke(core::action_id("view.mode_next")) == app::DispatchResult::Changed);
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(app::PatternMode::Browse));
  CHECK(rig.app->invoke(core::action_id("view.mode_next")) == app::DispatchResult::Changed);
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(app::PatternMode::Edit));
  rig.invoke("learn.toggle");
  CHECK(rig.app->invoke(core::action_id("view.mode"), 0) == app::DispatchResult::Disabled);
  CHECK(rig.app->invoke(core::action_id("view.mode_next")) == app::DispatchResult::Disabled);
}

TEST_CASE("modes: the same key does something else in another mode, and only there") {
  Rig rig;
  rig.invoke("view.mode", static_cast<core::ActionArg>(app::PatternMode::Edit));
  rig.tap(Button::Down);  // Edit: one step
  CHECK(rig.app->view().row() == 1);
  rig.tap(Button::Right);
  CHECK((rig.app->view().track() == 0 && rig.app->view().column() == Column::Instr));
  rig.invoke("view.mode", static_cast<core::ActionArg>(app::PatternMode::Follow));  // retired (OS-004d): refused, the mode is still Edit
  CHECK(rig.state().mode[app::screen_index(ScreenId::Pattern)] == static_cast<unsigned>(app::PatternMode::Edit));
  rig.tap(Button::Down);
  CHECK(rig.app->view().row() == 2);

  rig.invoke("view.mode", static_cast<core::ActionArg>(app::PatternMode::Select));  // retired (OS-LIVEREC): refused, the mode is still Edit
  CHECK(rig.state().mode[app::screen_index(ScreenId::Pattern)] == static_cast<unsigned>(app::PatternMode::Edit));
  rig.turn(3);  // Edit: the encoder is the field's (Instr: an edit, which is not Browse's scroll)
  rig.pass(1);
  CHECK(rig.app->view().row() == 2);
  rig.tap(Button::Note);  // ...and the column buttons are the screen's
  CHECK(rig.app->view().column() == Column::Note);

  rig.invoke("view.mode", static_cast<core::ActionArg>(app::PatternMode::Browse));
  rig.turn(3);  // Browse: the mode's own row, the encoder scrolls the steps
  rig.pass(1);
  CHECK(rig.app->view().row() == 5);
  rig.invoke("view.mode", static_cast<core::ActionArg>(app::PatternMode::Edit));
  rig.tap(Button::Down);
  CHECK(rig.app->view().row() == 6);
}

TEST_CASE("modes: a user or preset line in a mode's context overrides the built-in there and nowhere else") {
  Rig rig;
  const std::string keymap = "o3keymap 1\npattern.browse tap up nav.scroll 2\npattern.edit tap left nav.scroll 3\n";
  CHECK(rig.app->load_user_keymap(keymap.data(), keymap.size()).ok());
  rig.invoke("view.mode", static_cast<core::ActionArg>(app::PatternMode::Edit));
  rig.tap(Button::Left);  // Edit: the user's line
  CHECK(rig.app->view().row() == 3);
  rig.tap(Button::Up);    // Edit has no line for Up: the screen's
  CHECK(rig.app->view().row() == 2);
  rig.invoke("view.mode", 3);
  rig.tap(Button::Up);    // Browse: the user's line, not the screen's
  CHECK(rig.app->view().row() == 4);
  rig.tap(Button::Left);  // Browse: no line for Left, the screen's (already at the first track and column)
  CHECK(rig.app->view().track() == 0);
  // a keymap of the retired Select (or Follow) mode still loads: the context is named, nothing is bound to it, and nothing in the app is ever in it
  const std::string old = "o3keymap 1\npattern.select tap up nav.scroll 5\npattern.follow tap up nav.scroll 5\n";
  CHECK(rig.app->load_user_keymap(old.data(), old.size()).ok());
  rig.tap(Button::Up);    // (Browse's user line from before: the old ones replaced the whole keymap, so the screen's Up)
  CHECK(rig.app->view().row() == 3);
  // The loader knows every mode's context by its name, and an unknown one is an error.
  const std::string bad = "o3keymap 1\npattern.nowhere tap up nav.down\n";
  CHECK(rig.app->load_preset_keymap(bad.data(), bad.size()).error == core::KeymapError::UnknownContext);
}

// ---- notifications through the app ----------------------------------------------------------------------------------------

TEST_CASE("notifications: eight quick F-keys overflow the queue of four, the oldest info goes first, and they run out in order") {
  Rig rig;
  const Button keys[8] = {Button::F1, Button::F2, Button::F3, Button::F4, Button::F5, Button::F6, Button::F7, Button::F8};
  for (const Button k : keys) {
    rig.tap(k);
  }
  // F6 INVERT is an edit (Disabled in Browse): it says nothing (OS-CFGWIRE). F1, F2, F3, F4 (FILL says it needs Edit: OS-PTOOLS), F5 (no engine), F7 (TOOLS: this App has no project, OS-H3A) and F8 do: seven notices, a queue of four.
  CHECK(rig.state().notify.size() == 4u);
  CHECK(rig.state().notify.dropped() == 3u);
  CHECK(rig.message() == "FILL: TURN EDIT ON");  // F1, F2 and F3 were the oldest
  // Each lasts 2000 ms from when it is first shown.
  rig.pass(1980);
  CHECK(rig.message() == "FILL: TURN EDIT ON");
  rig.pass(40);
  CHECK(rig.message() == "PREVIEW: NO AUDIO OUTPUT");
  rig.pass(2000);
  CHECK(rig.message() == "NO PROJECT");
  rig.pass(2000);
  CHECK(rig.message() == "NOTHING TO UNDO HERE");  // F8 is the UNDO key now: its notification is the history's, not its label
  rig.pass(2000);
  CHECK(rig.message() == "");
  CHECK(rig.state().notify.empty());
  CHECK(rig.app->overlay() == OverlayId::None);
  CHECK(rig.app->failed_frames() == 0u);
}

TEST_CASE("notifications: the labels are the screen in view's own") {
  Rig rig;
  rig.tap(Button::Song);
  rig.tap(Button::F6);  // (the Song keys have actions since OS-SONG1: with no project they say so; no transport, F1 is Disabled)
  CHECK(rig.message() == "NO PROJECT");
  rig.pass(2000);
  rig.tap(Button::F8);
  CHECK(rig.message() == "NOTHING TO UNDO HERE");  // UNDO on Song too (OS-002c)
}

// ---- the song screen's cursor --------------------------------------------------------------------------------------------

TEST_CASE("song view: the cursor clamps to the list and the 16-row window follows it") {
  app::SongView v;
  CHECK((v.pos() == 0 && v.top() == 0));
  CHECK_FALSE(v.move(-1, 24));
  CHECK(v.move(1, 24));
  CHECK(v.move(14, 24));
  CHECK((v.pos() == 15 && v.top() == 0));  // the 16th row: no scroll yet
  CHECK(v.move(1, 24));
  CHECK((v.pos() == 16 && v.top() == 1));
  CHECK(v.move(100, 24));
  CHECK((v.pos() == 23 && v.top() == 8));
  CHECK_FALSE(v.move(1, 24));
  CHECK(v.move(-9, 24));
  CHECK((v.pos() == 14 && v.top() == 8));  // back inside the window: it stays
  CHECK(v.move(-7, 24));
  CHECK((v.pos() == 7 && v.top() == 7));   // above it: the window moves up with the cursor
  CHECK(v.move(-2147483647 - 1, 24));      // a huge move does not overflow
  CHECK((v.pos() == 0 && v.top() == 0));
  CHECK(v.move(2147483647, 24));
  CHECK(v.pos() == 23);
  CHECK(v.set(5, 24));
  CHECK((v.pos() == 5 && v.top() == 5));
  CHECK_FALSE(v.set(5, 24));
  // A shorter list: fit() pulls the cursor and the window back inside it.
  v.set(23, 24);
  CHECK(v.fit(10));
  CHECK((v.pos() == 9 && v.top() == 0));
  CHECK_FALSE(v.fit(10));
  (void)v.set(3, 0);  // an empty list: the cursor is 0 whatever is asked (no out-of-range index)
  CHECK(v.pos() == 0);
}

TEST_CASE("song screen: the arrows and the encoder move the list, the left and right arrows and the column buttons (but Note) do nothing") {
  Rig rig;
  rig.tap(Button::Song);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.state().song.pos() == 2);
  rig.tap(Button::Up);
  CHECK(rig.state().song.pos() == 1);
  // (Shift + F2: the quarter jumps are the keys that go far; the encoder is the slot's pattern since OS-SONG1: song_edit_test.cpp)
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F2);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().song.pos() == 6);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  CHECK(rig.state().song.pos() == 0);
  const std::uint32_t frames = rig.app->frames();
  rig.tap(Button::Left);  // one axis
  rig.tap(Button::Right);
  rig.tap(Button::Instr);  // the column buttons belong to the pattern grid (Note is NEXT USE on Song since OS-SONGSHARE S4: song_edit_test.cpp)
  rig.tap(Button::Vol);
  rig.tap(Button::Fx);
  rig.tap(Button::Up);    // at the top
  CHECK(rig.state().song.pos() == 0);
  CHECK(rig.app->frames() == frames);
  rig.tap(Button::Down);
  CHECK(rig.state().song.pos() == 1);
  CHECK(rig.app->failed_frames() == 0u);
}

TEST_CASE("song data: the demo order list is deterministic and its lookups never leave the tables") {
  app::SongData a, b;
  app::fill_song_demo(a);
  app::fill_song_demo(b);
  CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
  CHECK(a.length == 24);
  CHECK((a.play >= 0 && a.play < a.length));
  for (int i = 0; i < a.length; ++i) {
    CHECK(a.order[i] >= 1);
    CHECK(a.order[i] <= app::kSongPatterns);
    CHECK(app::song_pattern(a, i).steps >= 1);
    CHECK(std::strlen(app::song_pattern(a, i).name) <= static_cast<std::size_t>(app::kSongNameChars));
  }
  // Out-of-range positions and pattern numbers give pattern 1's data, not a read past the table.
  CHECK(&app::song_pattern(a, -1) == &a.patterns[0]);
  CHECK(&app::song_pattern(a, app::kSongMaxLength) == &a.patterns[0]);
  a.order[3] = 0;
  CHECK(&app::song_pattern(a, 3) == &a.patterns[0]);
  a.order[3] = 200;
  CHECK(&app::song_pattern(a, 3) == &a.patterns[0]);
}

// ---- a random walk over every input -------------------------------------------------------------------------------------

namespace {

struct Walk {
  std::uint64_t seed = 0x9E3779B97F4A7C15ull;
  std::uint32_t next() {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(seed >> 33);
  }
};

// The invariants of the view state, whatever was pressed: nothing the user does leaves the cursor, the page, the mode, the
// overlay or the queue outside what they can hold (the sanitisers watch the rest: the display lists, the tables).
void check_invariants(const app::App& a) {
  const app::ViewState& v = a.state();
  CHECK((v.overlay == OverlayId::None || v.overlay == OverlayId::Learn || v.overlay == OverlayId::Navigator || v.overlay == OverlayId::StepPopup || v.overlay == OverlayId::Help));  // (a held Instr, Fx or Note: OS-FU3; Shift + Config: OS-HELP)
  CHECK(v.help.page < app::kHelpPages);
  for (unsigned p = 0; p < app::kHelpPages; ++p) {  // a scroll never leaves its page: the last line stays in the body
    CHECK((v.help.scroll[p] == 0 || v.help.scroll[p] + app::kHelpRows <= app::help_page(p).count));
  }
  CHECK((v.navigator >= 0 && v.navigator < static_cast<int>(app::kScreenCount)));
  CHECK((v.pattern.row() >= 0 && v.pattern.row() < app::kSteps));
  CHECK((v.pattern.track() >= 0 && v.pattern.track() < app::kTracks));
  CHECK((v.pattern.row() >= v.pattern.top_row() && v.pattern.row() < v.pattern.top_row() + app::PatternView::kVisibleRows));
  CHECK((v.song.pos() >= 0 && v.song.pos() < a.model().song.length));
  CHECK((v.song.pos() >= v.song.top() && v.song.pos() < v.song.top() + app::SongView::kVisibleRows));
  CHECK(v.notify.size() <= core::NotifyQueue::kCapacity);
  CHECK(v.mode[app::screen_index(ScreenId::Pattern)] < app::kPatternModes);
  CHECK(v.page[app::screen_index(ScreenId::Song)] < app::kSongPages);
  const app::ScreenEntry* shown = app::find_screen(app::screen_registry(), v.screen);
  CHECK((shown != nullptr && shown->screen != nullptr));  // only screens that exist are ever switched to
  CHECK((v.file.pos() >= 0 && v.file.pos() >= v.file.top() && v.file.pos() < v.file.top() + app::FileView::kVisibleRows));  // OS-010a: the File screen's cursor (no card here: an empty list)
  CHECK(v.file.pos() == 0);
  CHECK((v.wav.list.pos() >= 0 && v.wav.list.pos() >= v.wav.list.top() && v.wav.list.pos() < v.wav.list.top() + app::FileView::kVisibleRows));  // OS-011a: Sample load's cursor
  CHECK(v.wav.list.pos() == 0);
  CHECK(v.page[app::screen_index(ScreenId::Instrument)] < app::kParamsScreen.pages());  // OS-002d: the parameter page's cursor stays on its page
  CHECK((v.params.row >= 0 && v.params.row < app::ParamView::kConfigRows && v.params.env >= 0 && v.params.env < 6));
  CHECK(v.page[app::screen_index(ScreenId::Mixer)] < app::kMixerScreen.pages());  // OS-011b: the Mixer's page and cursor stay on the page (the cursor is clamped when read)
  CHECK((v.instrument >= 1 && v.instrument <= static_cast<int>(model::kMaxInstruments)));  // (the step popup lists all 64: OS-FU3)
  CHECK(a.failed_frames() == 0u);
}

struct WalkResult {
  std::uint64_t digest = 0;
  bool learn = false;      // each of these was seen at least once: the walk really reaches them
  bool navigator = false;
  bool song = false;
  bool notify = false;
  bool selected = false;   // a mode other than Edit
};

// Runs `steps` random events and time steps; the digest folds in what the user could see change.
WalkResult random_walk(unsigned steps, std::uint64_t seed) {
  Rig rig;
  WalkResult out;
  Walk w;
  w.seed = seed;
  std::uint64_t digest = 1469598103934665603ull;
  const auto fold = [&](std::uint64_t x) { digest = (digest ^ x) * 1099511628211ull; };
  for (unsigned i = 0; i < steps; ++i) {
    const std::uint32_t r = w.next();
    switch (r % 8) {
      case 0:
      case 1:
      case 2:
        rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>((r >> 4) % app::kButtonCount), true));
        break;
      case 3:
      case 4:
        // A release for any button, down or not: releases that never had a press and presses that never get one.
        rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>((r >> 4) % app::kButtonCount), false));
        break;
      case 5:
        rig.input.push(core::make_encoder_event(rig.clock.now(), 0, static_cast<std::int16_t>(static_cast<int>((r >> 4) % 7) - 3)));
        break;
      case 6:
        rig.input.push(core::make_pad_event(rig.clock.now(), static_cast<std::uint8_t>((r >> 4) % 48), (r >> 12) % 2 == 0 ? 100 : 0));
        break;
      default:
        break;
    }
    // Time: mostly a few milliseconds, sometimes past the chord, double-tap and hold windows, now and then a long wait.
    const std::uint32_t t = (r >> 16) % 16;
    rig.pass(t < 10 ? t : (t < 14 ? 50 + (r >> 20) % 400 : 2500));
    check_invariants(*rig.app);
    const app::ViewState& v = rig.state();
    out.learn = out.learn || v.overlay == OverlayId::Learn;
    out.navigator = out.navigator || v.overlay == OverlayId::Navigator;
    out.song = out.song || v.screen == ScreenId::Song;
    out.notify = out.notify || rig.app->overlay() == OverlayId::Notify;
    out.selected = out.selected || v.mode[app::screen_index(ScreenId::Pattern)] != 0;
    fold(static_cast<std::uint64_t>(v.screen) | static_cast<std::uint64_t>(v.overlay) << 4 |
         static_cast<std::uint64_t>(v.pattern.row()) << 8 | static_cast<std::uint64_t>(v.pattern.track()) << 16 |
         static_cast<std::uint64_t>(v.song.pos()) << 24 | static_cast<std::uint64_t>(v.navigator) << 32 |
         static_cast<std::uint64_t>(v.notify.size()) << 40);
  }
  fold(rig.app->frames());
  out.digest = digest;
  return out;
}

}  // namespace

TEST_CASE("screens: a random walk over every button, pad, encoder turn and pause keeps every invariant, and replays the same") {
  bool learn = false, navigator = false, song = false, notify = false, mode = false;
  for (const std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{0xC0FFEE}}) {
    const WalkResult first = random_walk(6000, seed);
    CHECK(random_walk(6000, seed).digest == first.digest);  // the same inputs and times, the same pictures: no real clock
    learn = learn || first.learn;
    navigator = navigator || first.navigator;
    song = song || first.song;
    notify = notify || first.notify;
    mode = mode || first.selected;
  }
  // The walk is not vacuous: it opens both overlays, visits the song, shows a notification and changes a mode.
  CHECK(learn);
  CHECK(navigator);
  CHECK(song);
  CHECK(notify);
  CHECK(mode);
  CHECK(random_walk(3000, 1).digest != random_walk(3000, 2).digest);
}
