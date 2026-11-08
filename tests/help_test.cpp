// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-HELP H1 (docs/specs/OS-HELP.md): Shift + Config opens the help page of the screen in view (new Community OS behaviour). The gesture and the page it lands on, the keys inside the overlay
// (pages, lines, screenfuls, the scroll that is kept per page), the keys that close it and do nothing else, a pad's release across the open, the Navigator, and the text tables (ASCII, widths).
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/help_text.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/mixer_view.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/stem_map.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/model/editor.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/dir_storage.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::OverlayId;
using app::ScreenId;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

// What the live pads ask of the voices (as pattern_edit_app_test.cpp's LiveLog).
struct PadLog final : app::PreviewSink {
  std::vector<char> log;  // 'D' pad down, 'U' pad up, 'P' a preview, 'N' / 'F' a MIDI note on / off
  void preview(unsigned, unsigned) noexcept override { log.push_back('P'); }
  void pad_down(unsigned, unsigned, unsigned, unsigned) noexcept override { log.push_back('D'); }
  void pad_up(unsigned) noexcept override { log.push_back('U'); }
  void midi_note(bool on, unsigned, unsigned, unsigned, unsigned, unsigned) noexcept override { log.push_back(on ? 'N' : 'F'); }
  unsigned count(char c) const { unsigned n = 0; for (const char x : log) { n += x == c ? 1u : 0u; } return n; }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  PadLog sink;

  Rig() {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, 16);
    (void)p.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_preview(&sink);
    app->run_once();
  }
  const app::ViewState& state() const { return app->state(); }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); pass(); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); pass(); }
  void tap(Button b) { press(b); release(b); }
  void shift_tap(Button b) { press(Button::Shift); tap(b); release(Button::Shift); }
  void pad_down(unsigned pad) { input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 100)); pass(); }
  void pad_up(unsigned pad) { input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 0)); pass(); }
  void turn(int detents) { input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents))); pass(); }
  void go(ScreenId id) { (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id)); pass(); }
  void help() { shift_tap(Button::Config); }
  bool helping() const { return state().overlay == OverlayId::Help; }
  unsigned page() const { return state().help.page; }
  unsigned scroll() const { return state().help.scroll[state().help.page]; }
  int lines() const { return app::help_page(page()).count; }
  int max_scroll() const { return lines() > app::kHelpRows ? lines() - app::kHelpRows : 0; }
};

// The Perform screen on the rig: the board discards the engine commands, the slots are read back from the bank.
struct PerformRig : Rig {
  std::unique_ptr<app::PerformBoard> board = std::make_unique<app::PerformBoard>();
  PerformRig() {
    app->attach_perform(board.get());
    go(ScreenId::Perform);
  }
};

// A module app that counts what it is given (module_ui_test.cpp's Tool, cut down to the counters), launched from MODULES off a temporary card.
struct AppCount {
  std::uint32_t inputs;
  std::uint32_t last_kind, last_id, last_value;
};
int count_init(void*, const o3m_host*) { return 0; }
void count_nop(void*) {}
void count_set_param(void*, std::uint32_t, float) {}
void count_input(void* s, const o3m_ui_event* e) {
  AppCount* c = static_cast<AppCount*>(s);
  ++c->inputs;
  c->last_kind = e->kind;
  c->last_id = e->id;
  c->last_value = static_cast<std::uint32_t>(e->value);
}
void count_draw(void*, std::uint32_t, o3m_canvas* c) { c->draw->rect(c, 8, 8, 120, 40, O3M_COLOR_TEXT); }  // (a page with something on it: the base-screen tests below look for it under help)
const o3m_param kCountParams[1] = {{"Speed", 0.0f, 100.0f, 10.0f, O3M_PARAM_INT}};
const o3m_ui kCountUi = {sizeof(o3m_ui), count_nop, count_input, count_draw, count_nop, nullptr};
const o3m_module kCount = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_APP, sizeof(o3m_module), "Count", sizeof(AppCount), 1, kCountParams,
                           count_init, nullptr, count_set_param, nullptr, nullptr, count_nop, &kCountUi, 1};

struct AppRig : Rig {
  struct Modules {
    module::ModuleSet set;
    module::ModuleLoader loader;
    audio::ModuleVoices voices;
    app::ModuleUi ui;
    alignas(64) std::uint8_t scratch[audio::ModuleVoices::kScratchBytes] = {};
    alignas(64) std::uint8_t sdram[16384] = {};
  };
  std::filesystem::path root = std::filesystem::temp_directory_path() / ("o3_help_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
  platform::host::DirStorage storage{root.string()};
  std::unique_ptr<Modules> mods = std::make_unique<Modules>();

  AppRig() {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "modules" / "apps");
    std::ofstream(root / "modules" / "apps" / "Count.tmod", std::ios::binary) << "x";
    NEED(storage.mount() == hal::storage::Status::Ok);
    (void)mods->voices.attach_scratch(mods->scratch, sizeof mods->scratch);
    (void)mods->set.cold().init(mods->sdram, 0x70000000u, sizeof mods->sdram);
    mods->ui.attach(mods->set, mods->loader, mods->voices);
    mods->ui.set_storage(&storage);
    CHECK(mods->set.add_native("Count", kCount));
    app->attach_modules(&mods->ui);
    mods->ui.scan_apps();
    go(ScreenId::Modules);
  }
  ~AppRig() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  AppCount* count() { return static_cast<AppCount*>(const_cast<void*>(mods->ui.app_state())); }
  // F1 on the app under the cursor; the F1 of the launch is not the app's business, so its inputs start at 0.
  bool launch() {
    tap(Button::F1);
    return state().screen == ScreenId::ModuleApp && mods->ui.app_running() && count() != nullptr;
  }
};

// OS-HELP follow-up: the vertices (glyphs, fills, lines) that the BASE screen alone puts inside `box` when it is told `covered`: the display list is looked at, not the pixels, because the simulator's fill hides what
// the unit's does not (OS-SIMKEY, as OS-BUG-OGOVERLAY). Every vertex is a VERTEX2II (x < 512) or a VERTEX2F in sixteenths, as the translate is off.
unsigned base_vertices_in(const app::App& app, const ui::Rect& covered, const ui::Rect& box, unsigned theme_index) {
  const ui::Theme& theme = ui::theme(ui::theme_id_at(theme_index));
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, theme);
  r.use_translate(false);
  CHECK(r.begin(ui::Color::Pane));
  const app::ScreenEntry* e = app::find_screen(app::screen_registry(), app.state().screen);
  if (e == nullptr || e->screen == nullptr) {
    FAIL("no screen to draw");
    return 0u;
  }
  e->screen->render(app.model(), app.state(), r, covered);
  CHECK(r.end());
  unsigned inside = 0;
  bool drawing = false;
  for (std::size_t i = 0; i < display.last_frame_words(); ++i) {
    const std::uint32_t w = storage.words[i];
    int x = -1;
    int y = -1;
    if ((w >> 24) == (eve::dl::begin(eve::Primitive::Bitmaps) >> 24)) {
      drawing = (w & 0xFu) != 0u;
    } else if ((w >> 30) == eve::opcode::kVertex2iiTag) {
      x = static_cast<int>((w >> 21) & 0x1FFu);
      y = static_cast<int>((w >> 12) & 0x1FFu);
    } else if ((w >> 30) == eve::opcode::kVertex2fTag) {
      const std::int32_t sx = static_cast<std::int32_t>((w >> 15) & 0x7FFFu);
      x = (sx >= 0x4000 ? sx - 0x8000 : sx) / 16;
      y = static_cast<int>(w & 0x7FFFu) / 16;
    }
    if (drawing && x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h) {
      ++inside;
    }
  }
  return inside;
}

// What the help page covers must be bare under it, in every theme: nothing of the screen below is drawn inside the rectangle the overlay declares.
void check_bare_under_help(const app::App& app, const std::string& what) {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const ui::Rect covered = app::overlay_of(OverlayId::Help)->rect(ui::theme(ui::theme_id_at(t)));
    CHECK_MESSAGE(base_vertices_in(app, ui::Rect{0, 0, 0, 0}, covered, t) > 0u, what << ", theme " << t << ": the check sees the page's drawing when nothing is skipped");
    CHECK_MESSAGE(base_vertices_in(app, covered, covered, t) == 0u, what << ", theme " << t << ": the page draws under the help text");
  }
}

}  // namespace

TEST_CASE("help: Shift + Config opens the page of the screen in view, Config included, and plain Config is unchanged") {
  struct Row { ScreenId screen; unsigned page; };
  const Row rows[] = {{ScreenId::Pattern, 1}, {ScreenId::Song, 2}, {ScreenId::Instrument, 3}, {ScreenId::Wave, 4}, {ScreenId::SampleEdit, 5}, {ScreenId::SampleLoad, 6}, {ScreenId::Record, 7},
                      {ScreenId::Mixer, 8}, {ScreenId::Perform, 9}, {ScreenId::File, 11}, {ScreenId::Config, 12}, {ScreenId::Modules, 13}};
  for (const Row& row : rows) {
    Rig rig;
    rig.go(row.screen);
    NEED(rig.state().screen == row.screen);
    rig.help();
    CHECK(rig.helping());
    CHECK(rig.page() == row.page);
    CHECK(rig.state().screen == row.screen);  // the screen under it is where it was
    CHECK(rig.app->overlay() == OverlayId::Help);
  }
  Rig rig;
  rig.tap(Button::Config);  // plain Config: the Config screen, no overlay
  CHECK(rig.state().screen == ScreenId::Config);
  CHECK_FALSE(rig.helping());
  rig.tap(Button::Config);  // and again: its next page, as before
  CHECK(rig.state().screen == ScreenId::Config);
  CHECK(rig.state().page[app::screen_index(ScreenId::Config)] == 1);
  CHECK_FALSE(rig.helping());
  CHECK(app::help_page_of(ScreenId::InputTest) == 0);  // the hidden diagnostic has no page (it never opens help)
  CHECK(app::help_page_of(ScreenId::ModuleApp) == 14);
  CHECK(app::help_page_of(ScreenId::Perform) == 9);  // Perform has two pages: the slots, and the DJ page (OS-DJ)
  CHECK(app::help_page_of(ScreenId::Perform, true) == 10);
  CHECK(app::help_page_of(ScreenId::Song, true) == 2);  // (the flag is Perform's)
  CHECK(std::string(app::help_page(9).title) == "HELP: PERFORM");
  CHECK(std::string(app::help_page(10).title) == "HELP: PERFORM DJ");
}

TEST_CASE("help: on the DJ page of Perform it opens the DJ page, and Left is the slots' page") {
  PerformRig rig;
  rig.help();
  CHECK(rig.page() == 9);  // the slots page: Perform opens on its page 1
  rig.tap(Button::F8);
  CHECK_FALSE(rig.helping());
  rig.tap(Button::Performance);
  NEED(rig.state().dj_page());
  rig.help();
  CHECK(rig.helping());
  CHECK(rig.page() == 10);
  CHECK(rig.state().screen == ScreenId::Perform);
  rig.tap(Button::Left);
  CHECK(rig.page() == 9);
  // the keys and pads the page names (bindings.cpp perform.dj; docs/specs/OS-DJ.md section 2)
  const app::HelpPage& page = app::help_page(10);
  for (const char* key : {"ENCODER", "F1 BANK -, F2 BANK +", "F3 CURVE", "F4 FILTER", "F5 SCR", "F6 LATCH", "F8 RESET", "LEFT / RIGHT", "ROW 1", "SHIFT, HELD, ROW 1", "ROW 2", "ROW 3, KILL", "ROW 3, FILT",
                          "ROW 3, TRANS", "ROW 3, TAPE STOP", "ROW 4"}) {
    bool found = false;
    for (unsigned n = 0; n < page.count; ++n) {
      found = found || (page.lines[n].key != nullptr && std::strcmp(page.lines[n].key, key) == 0);
    }
    CHECK_MESSAGE(found, key);
  }
  unsigned bytes = 0;  // the page budget: about 1.2 KB a page
  for (unsigned n = 0; n < page.count; ++n) {
    bytes += static_cast<unsigned>(std::strlen(page.lines[n].text) + (page.lines[n].key != nullptr ? std::strlen(page.lines[n].key) : 0u));
  }
  CHECK(bytes <= 1500u);
}

TEST_CASE("help: Left and Right change the page and stop at the ends") {
  Rig rig;
  rig.go(ScreenId::Song);
  rig.help();
  NEED(rig.page() == 2);
  rig.tap(Button::Left);
  CHECK(rig.page() == 1);
  rig.tap(Button::Left);
  CHECK(rig.page() == 0);
  rig.tap(Button::Left);  // no wrap
  CHECK(rig.page() == 0);
  for (unsigned i = 0; i < 20; ++i) {
    rig.tap(Button::Right);
  }
  CHECK(rig.page() == app::kHelpPages - 1u);  // no wrap at the other end
  CHECK(rig.helping());
}

TEST_CASE("help: Up, Down, the encoder and Shift scroll and clamp; a short page does not scroll") {
  Rig rig;
  rig.help();  // Pattern's page
  NEED(rig.page() == 1);
  for (int i = 0; i < 12; ++i) {
    rig.tap(Button::Right);  // to Modules, a page shorter than the body
  }
  NEED(rig.page() == 13);
  CHECK(rig.lines() <= app::kHelpRows);
  rig.tap(Button::Down);
  rig.turn(5);
  CHECK(rig.scroll() == 0);
  for (int i = 0; i < 11; ++i) {
    rig.tap(Button::Left);  // back to Song
  }
  NEED(rig.page() == 2);
  NEED(rig.max_scroll() > app::kHelpRows + 3);
  rig.tap(Button::Up);
  CHECK(rig.scroll() == 0);  // the top
  rig.tap(Button::Down);
  CHECK(rig.scroll() == 1);
  rig.turn(3);  // one line a detent
  CHECK(rig.scroll() == 4);
  rig.turn(-1);
  CHECK(rig.scroll() == 3);
  rig.press(Button::Shift);
  rig.tap(Button::Down);  // a screenful
  CHECK(rig.scroll() == 3u + app::kHelpRows);
  rig.tap(Button::Up);
  CHECK(rig.scroll() == 3);
  rig.turn(1);  // Shift + the encoder, a screenful a detent
  CHECK(rig.scroll() == 3u + app::kHelpRows);
  rig.turn(-1);
  CHECK(rig.scroll() == 3);
  rig.release(Button::Shift);
  CHECK(rig.helping());  // Shift alone neither closes nor scrolls
  rig.turn(1000);  // clamps: the last line is at the bottom
  CHECK(rig.scroll() == static_cast<unsigned>(rig.max_scroll()));
  rig.tap(Button::Down);
  CHECK(rig.scroll() == static_cast<unsigned>(rig.max_scroll()));
  rig.turn(-1000);
  CHECK(rig.scroll() == 0);
}

TEST_CASE("help: Shift pressed again while it is up neither closes nor scrolls") {
  Rig rig;
  rig.help();
  rig.press(Button::Shift);
  CHECK(rig.helping());
  rig.release(Button::Shift);
  CHECK(rig.helping());
  CHECK(rig.page() == 1);
  CHECK(rig.scroll() == 0);
}

TEST_CASE("help: each page keeps its own scroll, and Shift + Config comes back to it") {
  Rig rig;
  rig.go(ScreenId::Song);
  rig.help();
  rig.turn(7);
  NEED(rig.scroll() == 7);
  rig.tap(Button::Left);  // Pattern's page, then the Global page: its own scroll
  rig.tap(Button::Left);
  NEED(rig.page() == 0);
  CHECK(rig.scroll() == 0);
  rig.turn(4);
  rig.tap(Button::Right);
  rig.tap(Button::Right);  // Song again, at 7
  CHECK(rig.scroll() == 7);
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  CHECK(rig.scroll() == 4);
  rig.tap(Button::Right);
  rig.tap(Button::Right);
  rig.tap(Button::F8);  // closes
  CHECK_FALSE(rig.helping());
  rig.help();  // the screen's page, at the scroll it was left at (not the page the user had walked to)
  CHECK(rig.page() == 2);
  CHECK(rig.scroll() == 7);
  rig.tap(Button::Config);  // closes
  CHECK_FALSE(rig.helping());
  rig.go(ScreenId::Pattern);
  rig.help();
  CHECK(rig.page() == 1);
  rig.tap(Button::Left);
  CHECK(rig.scroll() == 4);  // the Global page as it was left
}

TEST_CASE("help: any other button, a pad, Play and Shift + Config close it and do nothing else") {
  const Button keys[] = {Button::F1, Button::F8, Button::Play, Button::Song, Button::Config, Button::Pattern, Button::Rec, Button::Insert, Button::Delete, Button::Copy, Button::Note, Button::Vol};
  for (const Button key : keys) {
    Rig rig;
    rig.go(ScreenId::Song);
    rig.help();
    NEED(rig.helping());
    const std::uint8_t screen_page = rig.state().page[app::screen_index(ScreenId::Song)];
    const std::uint32_t posts = rig.state().notify.posts();
    rig.tap(key);
    CHECK_FALSE(rig.helping());
    CHECK(rig.state().overlay == OverlayId::None);
    CHECK(rig.state().screen == ScreenId::Song);  // Song, Config and Pattern did not go anywhere
    CHECK(rig.state().page[app::screen_index(ScreenId::Song)] == screen_page);
    CHECK_FALSE(rig.state().playing);  // Play did not start the transport
    CHECK(rig.state().notify.posts() == posts);  // an F key did not post its label
  }
  {
    Rig rig;  // Shift + Config closes it too
    rig.help();
    NEED(rig.helping());
    rig.help();
    CHECK_FALSE(rig.helping());
    CHECK(rig.state().screen == ScreenId::Pattern);
  }
  {
    Rig rig;  // a pad closes it and plays nothing
    rig.help();
    NEED(rig.helping());
    rig.pad_down(5);
    CHECK_FALSE(rig.helping());
    rig.pad_up(5);
    CHECK(rig.sink.count('D') == 0u);
    CHECK(rig.sink.count('P') == 0u);
  }
}

TEST_CASE("help: a pad held when it opened still ends its note at the release") {
  Rig rig;
  rig.pad_down(7);  // Pattern / Browse: the pad previews and the voice keeps the pad
  NEED(rig.sink.count('D') == 1u);
  rig.help();
  NEED(rig.helping());
  rig.pad_up(7);
  CHECK(rig.sink.count('U') == 1u);  // the release reached the voices through the open overlay
  CHECK(rig.helping());              // and a release is not a press: it closes nothing
  rig.pad_down(8);
  CHECK_FALSE(rig.helping());
  CHECK(rig.sink.count('D') == 1u);  // the press that closed it started no note
}

// OS-HELP review fold: the pad that closes help is taken at its press, so its release is the overlay's and does not run the Perform slot's pad_up (LATCH off: it would end the effect the slot still holds).
TEST_CASE("help: the release of the pad that closes it does not end a Perform effect") {
  PerformRig rig;
  NEED(rig.state().screen == ScreenId::Perform);
  const unsigned pad = 1u * seq::kPerfSlots + 0u;  // slot 0's row 1 (pad = row * slots + slot)
  rig.pad_down(pad);
  rig.pad_up(pad);
  NEED(rig.board->bank.row(0) == 1);  // latched (the default)
  rig.tap(Button::F6);                // LATCH off: the effect stays until a pad of its row is let go
  NEED(!rig.board->bank.latch());
  NEED(rig.board->bank.row(0) == 1);
  rig.help();
  NEED(rig.helping());
  rig.pad_down(pad);  // closes help and acts on nothing
  CHECK_FALSE(rig.helping());
  CHECK(rig.board->bank.row(0) == 1);
  rig.pad_up(pad);
  CHECK(rig.board->bank.row(0) == 1);  // the release was the overlay's
  rig.pad_down(pad);                   // and an ordinary press and release still end it
  rig.pad_up(pad);
  CHECK(rig.board->bank.row(0) == 0);
}

// OS-HELP review fold: a module app gets neither Shift + Config nor any input while help is up (a documented rule of the OS, no ABI change; module/sdk/README.md section 5).
TEST_CASE("help: on a module app's page Shift + Config opens it, the app hears nothing while it is up, and hears input again after") {
  AppRig rig;
  NEED(rig.launch());
  AppCount* app_state = rig.count();
  CHECK(app_state->inputs == 0u);
  rig.tap(Button::F3);
  CHECK(app_state->inputs == 2u);  // a plain key reaches the app: the press and the release
  rig.help();
  CHECK(rig.helping());
  CHECK(rig.state().screen == ScreenId::ModuleApp);
  CHECK(rig.page() == app::help_page_of(ScreenId::ModuleApp));
  CHECK(app_state->inputs == 4u);  // Shift's press and release, and nothing of the Config press or its release
  CHECK((app_state->last_kind == O3M_EV_BUTTON && app_state->last_id == O3M_BTN_SHIFT && app_state->last_value == 0u));
  rig.turn(1);  // scrolls the page (or stops at its end)
  rig.tap(Button::Down);
  rig.tap(Button::Right);  // the next page
  CHECK(rig.helping());
  CHECK(app_state->inputs == 4u);  // none of it reached the app
  rig.pad_down(5);                 // closes help
  rig.pad_up(5);
  CHECK_FALSE(rig.helping());
  CHECK(app_state->inputs == 4u);  // neither the press that closed it nor its release
  rig.help();
  NEED(rig.helping());
  rig.tap(Button::F3);  // another key closes it as well
  CHECK_FALSE(rig.helping());
  CHECK(app_state->inputs == 6u);  // (only Shift again)
  rig.tap(Button::F3);
  rig.turn(2);
  rig.pad_down(5);
  rig.pad_up(5);
  CHECK(app_state->inputs == 11u);  // after closing the app gets input again: F3 (2), the encoder (1), the pad (2)
  CHECK(rig.state().screen == ScreenId::ModuleApp);
}

TEST_CASE("help: Shift + Config under the Navigator or another dialog does nothing") {
  Rig rig;
  rig.press(Button::Song);  // the Navigator: the chord Song + Pattern (OS-APPSNAV)
  rig.press(Button::Pattern);
  rig.release(Button::Pattern);
  rig.release(Button::Song);
  NEED(rig.state().overlay == OverlayId::Navigator);
  rig.help();
  CHECK(rig.state().overlay == OverlayId::Navigator);
  rig.tap(Button::F7);  // CLOSE
  NEED(rig.state().overlay == OverlayId::None);
  rig.press(Button::Vol);  // Learn (FX1 + FX2)
  rig.press(Button::Fx);
  rig.release(Button::Fx);
  rig.release(Button::Vol);
  NEED(rig.state().overlay == OverlayId::Learn);
  rig.help();
  CHECK(rig.state().overlay == OverlayId::Learn);
}

TEST_CASE("help: the overlay is a modal Cursor over the lines of the page in view") {
  const app::Overlay* overlay = app::overlay_of(OverlayId::Help);
  NEED(overlay != nullptr);
  CHECK(overlay->modal());
  CHECK(overlay->closes_on_press());
  CHECK(overlay->context() == app::Context::Help);
  CHECK(overlay->page() == app::kHelpRows);
  CHECK_FALSE(app::overlay_of(OverlayId::Navigator)->closes_on_press());
  CHECK_FALSE(app::overlay_of(OverlayId::Learn)->closes_on_press());
  CHECK(app::help_page(app::kHelpPages + 5).count == app::help_page(app::kHelpPages - 1u).count);  // out of range: the last
}

TEST_CASE("help: the dialog fills the pane in every theme, and every line is ASCII and fits") {
  for (unsigned i = 0; i < ui::kThemeCount; ++i) {
    const ui::Theme& t = ui::theme(ui::theme_id_at(i));
    const ui::Rect r = app::overlay_of(OverlayId::Help)->rect(t);
    const int pane_top = t.metrics.strip_h + t.metrics.header_h;
    const int pane_h = 16 * t.metrics.row_pitch;
    CHECK(r.y >= pane_top);
    CHECK(r.y + r.h <= pane_top + pane_h);
    CHECK(r.x >= 0);
    CHECK(r.x + r.w <= ui::kScreenW);
    const int cell = t.geometry(ui::Face::Label).cell_w;
    const int bar_x = r.w - 2 * t.metrics.border - t.metrics.pad - (2 * t.metrics.border + t.metrics.pad / 2);  // where the scroll bar starts, in the body
    const int x = 2 * t.metrics.pad;
    for (unsigned p = 0; p < app::kHelpPages; ++p) {
      const app::HelpPage& page = app::help_page(p);
      CHECK(page.count > 0);
      for (unsigned n = 0; n < page.count; ++n) {
        const app::HelpLine& line = page.lines[n];
        NEED(line.text != nullptr);
        for (const char* c = line.text; *c != '\0'; ++c) {
          CHECK((*c >= 32 && *c < 127));
        }
        if (line.key == nullptr) {
          CHECK(x + static_cast<int>(std::strlen(line.text)) * cell <= bar_x);
          continue;
        }
        for (const char* c = line.key; *c != '\0'; ++c) {
          CHECK((*c >= 32 && *c < 127));
        }
        CHECK(static_cast<int>(std::strlen(line.key)) < app::kHelpKeyCols);
        CHECK(x + (app::kHelpKeyCols + static_cast<int>(std::strlen(line.text))) * cell <= bar_x);
        CHECK(static_cast<int>(std::strlen(line.text)) <= app::kHelpTextCols);
      }
    }
  }
}

TEST_CASE("help: every page has a paragraph, a KEYS part and a PADS part, in that order") {
  for (unsigned p = 0; p < app::kHelpPages; ++p) {
    const app::HelpPage& page = app::help_page(p);
    int para = -1;
    int keys = -1;
    int pads = -1;
    for (unsigned n = 0; n < page.count; ++n) {
      const app::HelpLine& l = page.lines[n];
      if (l.key == nullptr && para < 0) {
        para = static_cast<int>(n);
      } else if (l.key != nullptr && std::strcmp(l.key, "KEYS") == 0 && l.text[0] == '\0') {
        keys = static_cast<int>(n);
      } else if (l.key != nullptr && std::strcmp(l.key, "PADS") == 0 && l.text[0] == '\0') {
        pads = static_cast<int>(n);
      }
    }
    CHECK(para == 0);
    CHECK(keys > para);
    CHECK(pads > keys);
  }
}

// OS-HELP follow-up (int.15): on the unit the dialog's fill did not hide the Perform slots and a module app's page behind the help text, and the dialog left 10 px of the page at each side. The overlay now
// covers the full width of the pane, so every screen leaves out what lies inside it (a screen's text that runs under it is left out whole, as under the export box).
TEST_CASE("help: the base screens draw nothing inside the rectangle the page covers, in every theme (Perform slots and DJ, a module app, every other screen)") {
  Rig rig;
  for (unsigned id = 0; id < app::kScreenCount; ++id) {
    const ScreenId screen = static_cast<ScreenId>(id);
    if (screen == ScreenId::InputTest || screen == ScreenId::ModuleApp || screen == ScreenId::Perform) {
      continue;  // (no help page; the module app and Perform need their engines: their own rigs below)
    }
    rig.go(screen);
    NEED(rig.state().screen == screen);
    check_bare_under_help(*rig.app, "screen " + std::to_string(id));
  }
  PerformRig perform;
  check_bare_under_help(*perform.app, "Perform, the slots");
  perform.tap(Button::Performance);
  NEED(perform.state().dj_page());
  check_bare_under_help(*perform.app, "Perform, the DJ page");
  AppRig module;
  NEED(module.launch());
  check_bare_under_help(*module.app, "a module app");
}

// Review of OS-HELP (rev-sf11): the Mixer has five pages, and what they draw depends on the model (the board gives the Routing page its meters, a stem map the Stems page its rows, the cursor on PAN a block under
// the number), so the loop above, which sees the first page of a bare model, is not enough. Each page is looked at bare, with the board and without a stem map, and with both; the first page also with the cursor on PAN.
// Before the fix the Routing page's meters (board), the PAN cursor block (board or bare) and the Stems page's "NO STEM MAP" line (bare, board without a map) were drawn under the help text.
TEST_CASE("help: the Mixer pages draw nothing inside the rectangle the page covers (bare model, with the board, with a stem map, the cursor on PAN)") {
  const char* const variants[3] = {"bare", "board, no stem map", "board and stem map"};
  for (unsigned variant = 0; variant < 3; ++variant) {
    Rig rig;
    auto board = std::make_unique<app::MixBoard>();
    audio::StemMap stems;
    if (variant >= 1u) {
      board->stems = variant == 2u ? &stems : nullptr;
      rig.app->attach_mix(board.get());
    }
    rig.go(ScreenId::Mixer);
    NEED(rig.state().screen == ScreenId::Mixer);
    for (unsigned page = 0; page < app::kMixerPages; ++page) {
      for (unsigned row = 0; row < (page == 0u ? 2u : 1u); ++row) {  // the first page: the cursor on LEVEL, then on PAN
        rig.go(ScreenId::Mixer);
        rig.tap(page < 4u ? static_cast<Button>(static_cast<unsigned>(Button::F1) + page) : Button::F6);  // (F5 is CUE LVL; the fifth page, INSERTS, is F6)
        NEED(static_cast<unsigned>(app::mixer::page_of(rig.state())) == page);
        if (row == 1u) {
          rig.tap(Button::Down);
          NEED(app::mixer::cursor(rig.app->model(), rig.state()).row == 1u);
        }
        check_bare_under_help(*rig.app, std::string("Mixer page ") + std::to_string(page) + (row == 1u ? " (PAN)" : "") + ", " + variants[variant]);
      }
    }
  }
}
