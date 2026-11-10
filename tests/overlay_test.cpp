// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-002b: the overlay slot's geometry, the covered rectangle (what the base screen skips, and that skipping changes no
// pixel), what an overlay paints, and the shared frame of the screens.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/frame.h>
#include <o3os/app/fill_view.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/model.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/pattern_view.h>
#include <o3os/app/screens/config_screen.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/song_data.h>
#include <o3os/app/view_state.h>
#include <o3os/core/scales.h>
#include <o3os/model/pattern_fill.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/sim/screens.h>
#include <o3os/ui/components.h>
#include <o3os/ui/font.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include "../app/src/screens/dialog.h"  // make_dialog (private to app/src): the Fill dialog's body, for the KEY row check

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::OverlayId;
using app::ScreenId;
using ui::Color;
using ui::Rect;

namespace {

const ui::Theme& built_in(unsigned i) { return ui::theme(ui::theme_id_at(i)); }

const app::Model& demo_model() {
  static const app::Model model = [] {
    app::Model m{};
    app::fill_demo(m.pattern);
    app::fill_song_demo(m.song);
    return m;
  }();
  return model;
}

// One scene of the app's frame, with the overlay (or without one) and with the base screen told what is covered or not,
// drawn into a frame whose words are counted and whose pixels can be read.
struct Scene {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  ui::Theme theme;
  std::size_t words = 0;
  // OS-SMALL2: the glyphs the frame's display list draws on the F-key row's line (VERTEX2II words of the Label face at the row's text y: the F-numbers and the labels, one per
  // character that is not a space). The display list, not the pixels: the simulator's fill hides what a label drawn under another leaves, the unit's did not.
  unsigned key_glyphs = 0;

  Scene(const app::ViewState& view, OverlayId overlay, bool skip, unsigned theme_index = 0, bool translate = true, const app::Model* model = nullptr)
      : theme(built_in(theme_index)) {
    const app::Model& m = model != nullptr ? *model : demo_model();
    CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, theme);
    r.use_translate(translate);
    CHECK(r.begin(Color::Pane));
    const app::ScreenEntry* e = app::find_screen(app::screen_registry(), view.screen);
    const app::Overlay* o = app::overlay_of(overlay);
    CHECK(e != nullptr);
    if (e != nullptr && e->screen != nullptr) {
      e->screen->render(m, view, r, (skip && o != nullptr) ? o->rect(theme) : Rect{0, 0, 0, 0});
    }
    if (o != nullptr) {
      o->render(m, view, r);
    }
    app::draw_owner_keys(r, m, view);  // the F-key row of the dialog that owns it, as render_scene paints it (OS-KEYROWDLG)
    CHECK(r.end());
    words = display.last_frame_words();  // as the budget counts: DISPLAY included
    CHECK(display.render_failures() == 0u);
    const app::Frame f = app::make_frame(theme, app::kFrameRows);
    const std::uint32_t key_y = static_cast<std::uint32_t>(ui::caps_y(theme, ui::Face::Label, f.keys.y, f.keys.h));
    const std::uint32_t label_handle = ui::font_face(theme.font(ui::Face::Label)).handle;
    for (std::size_t i = 0; i < words; ++i) {
      const std::uint32_t w = storage.words[i];
      key_glyphs += (w >> 30) == 2u && ((w >> 12) & 0x1FFu) == key_y && ((w >> 7) & 0x1Fu) == label_handle ? 1u : 0u;
    }
  }
  std::uint32_t pixel(int x, int y) const {
    const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
  }
  std::uint32_t rgb(Color c) const {
    const ui::Rgb& v = theme.rgb(c);
    return (std::uint32_t{v.r} << 16) | (std::uint32_t{v.g} << 8) | v.b;
  }
  int count(Color c, int x, int y, int w, int h) const {
    int n = 0;
    for (int j = y; j < y + h; ++j) {
      for (int i = x; i < x + w; ++i) {
        n += pixel(i, j) == rgb(c) ? 1 : 0;
      }
    }
    return n;
  }
};

bool same_pixels(const Scene& a, const Scene& b) {
  return std::memcmp(a.raster.rgba(), b.raster.rgba(), sim::EveRaster::kFramebufferBytes) == 0;
}

// The views the checks are made over: the pattern at several windows and the song on both pages, each with the overlay
// state of the case.
std::vector<app::ViewState> views() {
  std::vector<app::ViewState> v;
  for (const int row : {0, 5, 40}) {
    for (const int moves : {0, 6, 5 * 7}) {
      app::ViewState s;
      s.pattern.scroll_rows(row);
      for (int i = 0; i < moves; ++i) {
        s.pattern.move_right();
      }
      v.push_back(s);
    }
  }
  for (const unsigned page : {0u}) {  // (OS-SONG3 S1: page 1 is TIMELINE, which has no rows for a dialog to cover; the list is the one page of rows)
    for (const int pos : {0, 9, 23, 3, 14, 20}) {
      app::ViewState s;
      s.screen = ScreenId::Song;
      s.page[app::screen_index(ScreenId::Song)] = static_cast<std::uint8_t>(page);
      (void)s.song.set(pos, demo_model().song.length);
      v.push_back(s);
    }
  }
  return v;
}

app::ViewState with_overlay(app::ViewState v, OverlayId overlay) {
  if (overlay == OverlayId::Notify) {
    (void)v.notify.post(core::Severity::Info, "F3 JUMP", "ui", 2000);
  } else {
    v.overlay = overlay;
  }
  return v;
}

const OverlayId kOverlays[] = {OverlayId::Learn, OverlayId::Navigator, OverlayId::Notify};

}  // namespace

// ---- the frame ----------------------------------------------------------------------------------------------------------

TEST_CASE("frame: the shared bands are the pattern layout's, in every theme") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const app::Frame f = app::make_frame(built_in(t), app::kFrameRows);
    const app::PatternLayout l = app::make_pattern_layout(built_in(t), app::PatternView::kVisibleRows, app::PatternView::kVisibleTracks);
    const auto same = [](const Rect& a, const Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; };
    CHECK_MESSAGE(same(f.strip, l.strip), built_in(t).name);
    CHECK_MESSAGE(same(f.header, l.header), built_in(t).name);
    CHECK_MESSAGE(same(f.pane, l.grid), built_in(t).name);
    CHECK_MESSAGE(same(f.info, l.info), built_in(t).name);
    CHECK_MESSAGE(same(f.keys, l.keys), built_in(t).name);
    CHECK(f.row_pitch == l.row_pitch);
    CHECK(f.keys.y + f.keys.h == ui::kScreenH);
  }
}

TEST_CASE("frame: a rectangle is inside another only when it lies wholly within it, and an empty one contains nothing") {
  const Rect outer{10, 20, 100, 50};
  CHECK(app::rect_inside({10, 20, 100, 50}, outer));
  CHECK(app::rect_inside({11, 21, 98, 48}, outer));
  CHECK_FALSE(app::rect_inside({9, 20, 10, 10}, outer));
  CHECK_FALSE(app::rect_inside({10, 19, 10, 10}, outer));
  CHECK_FALSE(app::rect_inside({100, 30, 11, 10}, outer));  // sticks out by a pixel at the right
  CHECK_FALSE(app::rect_inside({20, 60, 10, 11}, outer));   // and at the bottom
  CHECK(app::rect_inside({109, 69, 1, 1}, outer));
  CHECK_FALSE(app::rect_inside({10, 20, 0, 5}, outer));     // nothing is drawn by an empty item: it is never "hidden"
  CHECK_FALSE(app::rect_inside({10, 20, 5, 5}, {0, 0, 0, 0}));  // no overlay: nothing is covered
  CHECK_FALSE(app::rect_inside({10, 20, 5, 5}, {0, 0, 100, 0}));
}

TEST_CASE("frame: a rectangle touches another when they share a pixel, and an empty one touches nothing (OS-BUG-OGOVERLAY)") {
  const Rect outer{10, 20, 100, 50};
  CHECK(app::touches(outer, 10, 20, 1, 1));
  CHECK(app::touches(outer, 0, 0, 11, 21));      // the bottom right pixel of it is the outer's first
  CHECK(app::touches(outer, 109, 69, 40, 40));   // the outer's last pixel
  CHECK(app::touches(outer, 20, 30, 10, 10));    // inside
  CHECK(app::touches(outer, 0, 0, 800, 480));    // around
  CHECK_FALSE(app::touches(outer, 0, 0, 10, 100));   // ends at x = 9
  CHECK_FALSE(app::touches(outer, 110, 20, 5, 5));   // starts at the outer's right edge
  CHECK_FALSE(app::touches(outer, 10, 70, 5, 5));    // starts at the outer's bottom edge
  CHECK_FALSE(app::touches(outer, 10, 0, 5, 20));    // ends at y = 19
  CHECK_FALSE(app::touches(outer, 20, 30, 0, 5));    // an empty item
  CHECK_FALSE(app::touches({0, 0, 0, 0}, 20, 30, 5, 5));  // no overlay
  CHECK_FALSE(app::touches({0, 0, 100, 0}, 20, 30, 5, 5));
  for (const Rect& item : {Rect{9, 20, 10, 10}, Rect{100, 30, 11, 10}, Rect{20, 60, 10, 11}}) {  // whatever sticks out of the outer and is not wholly inside it still touches it
    CHECK(app::touches(outer, item.x, item.y, item.w, item.h));
    CHECK_FALSE(app::rect_inside(item, outer));
  }
}

// ---- geometry -----------------------------------------------------------------------------------------------------------

TEST_CASE("overlay: the dialogs lie in the pane on whole rows and are centred, the notification strip is the info line") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const ui::Theme& theme = built_in(t);
    const app::Frame f = app::make_frame(theme, app::kFrameRows);
    for (const OverlayId id : {OverlayId::Learn, OverlayId::Navigator}) {
      const Rect r = app::overlay_of(id)->rect(theme);
      CHECK_MESSAGE((r.x >= 0 && r.x + r.w <= ui::kScreenW), theme.name);
      CHECK_MESSAGE((r.y >= f.pane.y && r.y + r.h <= f.pane.y + f.pane.h), theme.name);  // inside the data pane
      CHECK_MESSAGE((r.y - f.pane.y) % f.row_pitch == 0, theme.name);                     // starts on a row...
      CHECK_MESSAGE(r.h % f.row_pitch == 0, theme.name);                                  // ...and ends on one
      CHECK_MESSAGE(r.x == ui::kScreenW - (r.x + r.w), theme.name);                       // centred across
      CHECK(app::overlay_of(id)->modal());
    }
    const Rect n = app::overlay_of(OverlayId::Notify)->rect(theme);
    CHECK((n.x == f.info.x && n.y == f.info.y && n.w == f.info.w && n.h == f.info.h));
    CHECK_FALSE(app::overlay_of(OverlayId::Notify)->modal());
    // The Navigator is the wider one, and its context captures like Learn's.
    CHECK(app::overlay_of(OverlayId::Navigator)->rect(theme).w > app::overlay_of(OverlayId::Learn)->rect(theme).w);
    CHECK(app::overlay_of(OverlayId::Learn)->context() == app::Context::Learn);
    CHECK(app::overlay_of(OverlayId::Navigator)->context() == app::Context::Navigator);
  }
  CHECK(app::overlay_of(OverlayId::None) == nullptr);
}

// OS-PTOOLS2 S3p (docs/specs/OS-PTOOLS2.md 3.1 point 3, mock-ups A and B): the F7 verb menu and the Pattern TOOLS dialog are ONE right-hand
// panel — x 500..800, the full height of the header and the data pane — and while it is up the grid is drawn in the 500 px left of it, with
// the track columns that fit there (three at the 5-track zoom), scrolled so the cursor and the block stay in view.
TEST_CASE("overlay: OS-PTOOLS2 S3p: the verb menu and the TOOLS dialog are the right-hand panel, and the grid under them narrows to its 500 px") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const ui::Theme& theme = built_in(t);
    const app::Frame f = app::make_frame(theme, app::kFrameRows);
    const Rect want{app::kRightPanelX, f.header.y, app::kRightPanelW, f.header.h + f.pane.h};  // over the header and the pane, not the strip, info or keys
    for (const OverlayId id : {OverlayId::VerbMenu, OverlayId::PatternTools}) {
      const Rect r = app::overlay_of(id)->rect(theme);
      CHECK_MESSAGE((r.x == want.x && r.y == want.y && r.w == want.w && r.h == want.h), theme.name << ", overlay " << static_cast<int>(id));
      CHECK_MESSAGE(app::overlay_of(id)->modal(), theme.name);
    }
    // Three tracks of the 5-track zoom fit the 500 px (the assumption of spec section 5), and the fields fit a column.
    const app::PatternLayout narrow = app::make_pattern_layout(theme, app::PatternView::kVisibleRows, app::pattern_tracks_fit(theme, app::PatternView::kVisibleTracks, 0, app::kRightPanelX), 0, app::kRightPanelX);
    CHECK(narrow.tracks == 3);
    CHECK(narrow.fits);
    CHECK(narrow.track0_x + narrow.tracks * narrow.track_w <= app::kRightPanelX);
  }
  // The grid under the panel: a block on tracks 5..7 with the cursor on track 7 is scrolled so the block's first track is the window's
  // leftmost column, and no grid pixel lies under the panel.
  app::ViewState v;
  v.pattern.scroll_rows(app::PatternView::kCentreRow);
  (void)v.pattern.move_track(6);  // the cursor on track 7 (0-based 6)
  v.edit.selection.active = true;  // the block: rows 5..9 of tracks 5..7
  v.edit.selection.start_row = 5;
  v.edit.selection.end_row = 9;
  v.edit.selection.start_track = 4;
  v.edit.selection.end_track = 6;
  const Scene bare(v, OverlayId::None, true);
  v.overlay = OverlayId::VerbMenu;  // PatternScreen::render narrows the grid by the view's overlay, not by the Scene's argument
  const Scene panel(v, OverlayId::VerbMenu, true);
  const ui::Theme& theme = panel.theme;
  const app::PatternLayout full = app::make_pattern_layout(theme, app::PatternView::kVisibleRows, app::PatternView::kVisibleTracks);
  const int pane_tracks = app::pattern_tracks_fit(theme, app::PatternView::kVisibleTracks, 0, app::kRightPanelX);
  const app::PatternLayout narrow = app::make_pattern_layout(theme, app::PatternView::kVisibleRows, pane_tracks, 0, app::kRightPanelX);
  const int gy = full.grid.y + 5;
  CHECK(bare.pixel(full.track_x(4), gy) == bare.rgb(Color::Empty));                     // the bare screen's fifth column separator
  CHECK(panel.pixel(narrow.track_x(pane_tracks - 1), gy) == panel.rgb(Color::Empty));   // the panel's grid has the last fitting column there
  CHECK(panel.pixel(narrow.track_x(pane_tracks - 1) + narrow.track_w, gy) == panel.rgb(Color::Pane));  // and no column past it
  const int slot = 6 - 4;  // the cursor's slot: the block's first track (0-based 4) is the leftmost column of the narrowed window
  const int cx = narrow.track_x(slot) + narrow.field_x[0] - narrow.cursor_pad_x;
  CHECK(panel.count(Color::Accent, cx, full.grid.y, narrow.track_w, full.grid.h) > 0);  // the cursor block is drawn, inside the 500 px
  CHECK(panel.count(Color::Sel, narrow.track_x(0), full.grid.y, pane_tracks * narrow.track_w, full.grid.h) > 0);  // the block is painted in the narrowed grid
  CHECK(panel.count(Color::Accent, app::kRightPanelX, 0, ui::kScreenW - app::kRightPanelX, ui::kScreenH) == 0);     // nothing of the grid under the panel
  const int cx_bare = full.track_x(4) + full.field_x[0] - full.cursor_pad_x;
  CHECK(bare.count(Color::Accent, cx_bare, full.grid.y, full.track_w, full.grid.h) > 0);

  // A block wider than the pane (tracks 4..7, four columns for three) with the cursor at its far end: the block's first track cannot be the
  // leftmost column and keep the cursor in view, so the cursor wins (the `track_ >= left + pane_tracks` branch of PatternView::draw): the
  // window starts at track 5 (0-based 4) and the cursor, on track 7 (0-based 6), is the last fitting column, still drawn inside the 500 px.
  v.edit.selection.start_track = 3;
  const Scene wide(v, OverlayId::VerbMenu, true);
  const int last = pane_tracks - 1;  // the cursor's slot: the window's last column
  const int wx = narrow.track_x(last) + narrow.field_x[0] - narrow.cursor_pad_x;
  CHECK(wide.count(Color::Accent, wx, full.grid.y, narrow.track_w, full.grid.h) > 0);  // the cursor block is in the last fitting column
  CHECK(wide.count(Color::Accent, narrow.track_x(0), full.grid.y, (pane_tracks - 1) * narrow.track_w, full.grid.h) == 0);  // and not in an earlier one
  CHECK(wide.count(Color::Sel, narrow.track_x(0), full.grid.y, pane_tracks * narrow.track_w, full.grid.h) > 0);  // the block is painted in the columns that fit
  CHECK(wide.count(Color::Accent, app::kRightPanelX, 0, ui::kScreenW - app::kRightPanelX, ui::kScreenH) == 0);  // nothing of the grid under the panel
}

// ---- the covered rectangle ------------------------------------------------------------------------------------------------

TEST_CASE("overlay: with no overlay nothing is skipped: the pattern and song frames are the OS-003b ones (the pattern with its new info line)") {
  app::ViewState pattern;
  pattern.pattern.scroll_rows(app::PatternView::kCentreRow);  // (OS-PATCENTER: the cursor on row 0 has seven blank slots above it; on row 7 the window is full, as the typical state was)
  const Scene p(pattern, OverlayId::None, true);
  CHECK(p.words == 994u);  // (OS-INFOLINE: nine more, "pads NOTES" is said on the info line now; OS-AUDIT E-PAT-03: seven more, the FX names are a name tile and a Micro value, and the bitmap handle changes twice a FX column; OS-H3A: four more, the F7 label is TOOLS and not "-"; OS-RECMODE2: one fewer, the chip says WATCH and not BROWSE; OS-PTOOLS: three more, the F4 label is FILL and not "-"; OS-PADS-B: two more, the info line says "Chromatic" and not "No scale"; OS-PADS-A: eight fewer, the F4 and F7 labels FILL and EXPORT are "-" now) the pattern view's typical state (DISPLAY included): OS-003b's 967, the six words the info line grew by (OS-009), five for the strip's "--" and MEM (OS-FU3), eight for its two-line load group and eight fewer for the dropped SONG 05 placeholder (OS-VOX)
  app::ViewState song;
  song.screen = ScreenId::Song;
  const Scene s(song, OverlayId::None, true);
  CHECK(s.words == 572u);  // (OS-SONG3 S1: fifty-three more, the list has the BAR and BARS columns of the ARRANGEMENT page it folded in, NAME and BAR and BARS; OS-SONGMAP: three more, the F5 and F6 labels are UNIQUE and TOOLS, not PAT - and PAT +; OS-SHIFTROW: twelve more, the info line says SHIFT+INS DUP; OS-FU3: six more, the strip's "--" and MEM; OS-VOX: seven, the two-line load group; OS-SONG1: the BPM field)
}

TEST_CASE("overlay: the held Note's panel (the step popup, kind Note) is measured: within the standard budget in every theme and both modes, and over the bare screen (OS-SCALES28 S2)") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    for (const bool translate : {true, false}) {
      app::ViewState v;
      v.screen = ScreenId::Pattern;
      v.overlay = OverlayId::StepPopup;
      v.popup.kind = app::StepPopupView::Note;  // the panel: the top line, the four row captions (row 2's says the encoder walks all 39) and the footer
      const Scene popup(v, OverlayId::StepPopup, false, t, translate);
      const Scene plain(v, OverlayId::None, false, t, translate);
      CHECK_MESSAGE(popup.words <= sim::kBudgetWords, "theme " << t << (translate ? " translate" : " vtx2f") << ": " << popup.words << " words");
      CHECK_MESSAGE(popup.words > plain.words, "theme " << t << (translate ? " translate" : " vtx2f") << ": the panel draws over the screen");
    }
  }
}

TEST_CASE("strip: the voices indicator is drawn inverted (a ChromeText chip) while a steal flashes, on the pattern and the song screens (OS-VOX)") {
  for (const ScreenId screen : {ScreenId::Pattern, ScreenId::Song}) {
    app::ViewState view;
    view.screen = screen;
    app::Model model = demo_model();
    model.load.cpu = 31;
    model.load.mem = 12;
    model.load.voices = 9;
    model.load.pool = 16;
    const Scene calm(view, OverlayId::None, true, 0, true, &model);
    model.load.steal = true;
    const Scene stolen(view, OverlayId::None, true, 0, true, &model);
    // The voices line is the lower half of the strip, in the 136 pixels at the right edge (the Label face: 17 cells of 8): its ChromeText ink is the digits; inverted, it is the chip's fill.
    const int ink_calm = calm.count(Color::ChromeText, 654, 16, 140, 16);
    const int ink_stolen = stolen.count(Color::ChromeText, 654, 16, 140, 16);
    CHECK(ink_calm > 0);
    CHECK(ink_calm < 200);
    CHECK(ink_stolen > 400);  // the chip: 66 x 12 pixels less the knocked-out letters
    CHECK_FALSE(same_pixels(calm, stolen));
    // The percent line above it does not change with the steal.
    CHECK(calm.count(Color::ChromeText, 654, 0, 140, 16) == stolen.count(Color::ChromeText, 654, 0, 140, 16));
  }
}

// OS-GOV: the CPU figure sits on an amber chip (the theme's Accent) above 70 % and on a red one (Rec) above 90 %; the strip does not move and a normal figure is as it was.
TEST_CASE("strip: the CPU readout is on an amber chip above 70 % and a red one above 90 %, on the pattern and the song screens (OS-GOV)") {
  for (const ScreenId screen : {ScreenId::Pattern, ScreenId::Song}) {
    app::ViewState view;
    view.screen = screen;
    app::Model model = demo_model();
    model.load.mem = 12;
    model.load.voices = 9;
    model.load.pool = 16;
    struct Case {
      std::int8_t cpu;
      bool amber;
      bool red;
    };
    const Case cases[] = {{-1, false, false}, {0, false, false}, {70, false, false}, {71, true, false}, {90, true, false}, {91, false, true}, {100, false, true}};
    int normal_ink = -1;
    for (const Case& c : cases) {
      CAPTURE(static_cast<int>(c.cpu));
      model.load.cpu = c.cpu;
      model.load.level = app::cpu_level(c.cpu);  // (the App keeps the level with hysteresis: system_load_test.cpp)
      const Scene scene(view, OverlayId::None, true, 0, true, &model);
      // The percent line is the upper half of the strip in the 140 pixels at the right edge; the chip is about 8 cells of 8 by 12 pixels (less the letters knocked out of it)
      const int amber = scene.count(Color::Accent, 654, 0, 140, 16);
      const int red = scene.count(Color::Rec, 654, 0, 140, 16);
      CHECK((amber > 300) == c.amber);
      CHECK((red > 300) == c.red);
      CHECK((amber == 0 || !c.red));
      if (!c.amber && !c.red) {
        const int ink = scene.count(Color::ChromeText, 654, 0, 140, 16);  // the figure and MEM in the normal ink
        CHECK(amber == 0);
        CHECK(red == 0);
        if (c.cpu >= 0) {
          normal_ink = ink;
        }
        CHECK(ink > 0);
      }
      // the lower half (VOX) and the rest of the strip do not change with the figure
      CHECK(scene.count(Color::Accent, 654, 16, 140, 16) == 0);
    }
    CHECK(normal_ink > 0);
  }
}

// The Pattern strip's instrument name: how many characters of a long name fit between "INS nn " and the load group (OS-VOX: the SONG 05 placeholder is gone and the load group is
// two Label lines, so the name has room again; with the placeholder and the Data face's readouts it was 4).
TEST_CASE("strip: the Pattern strip shows at least 10 characters of an instrument name, and stops before the load group (OS-VOX)") {
  const auto ink = [](const char* name) {
    sim::EveRaster raster;
    sim::RasterDisplay display{raster};
    CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
    ui::Theme theme = built_in(0);
    ui::Renderer r(builder, theme);
    CHECK(r.begin(Color::Pane));
    static const char* const keys[8] = {"", "", "", "", "", "", "", ""};
    app::PatternView::Status status{1, keys};
    status.instrument_name = name;
    status.load.cpu = 100;
    status.load.mem = 100;
    status.load.voices = 16;
    status.load.pool = 16;
    const app::PatternView view;
    view.draw(r, demo_model().pattern, status, Rect{0, 0, 0, 0});
    CHECK(r.end());
    int left = 0;   // the name's own columns, between the number and the load group
    int gap = 0;    // the few pixels just before the load group: nothing of the name may reach them
    const ui::Rgb& ink_rgb = theme.rgb(Color::ChromeText);
    for (int y = 0; y < 32; ++y) {
      for (int x = 500; x < 656; ++x) {
        const std::uint8_t* q = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
        if (q[0] == ink_rgb.r && q[1] == ink_rgb.g && q[2] == ink_rgb.b) {
          (x < 640 ? left : gap) += 1;
        }
      }
    }
    return std::pair<int, int>(left, gap);
  };
  const auto n9 = ink("WWWWWWWWW");
  const auto n10 = ink("WWWWWWWWWW");
  const auto n11 = ink("WWWWWWWWWWW");
  const auto n32 = ink("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
  CHECK(n10.first > n9.first);   // the tenth character is drawn
  CHECK(n11.first >= n10.first);
  CHECK(n32.second == 0);        // however long, the name stops short of the load group
  CHECK(n32.first >= n10.first);
}

TEST_CASE("overlay: what lies inside the covered rectangle is not drawn, and skipping it changes not one pixel") {
  const std::vector<app::ViewState> base = views();
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    for (const OverlayId overlay : kOverlays) {
      for (std::size_t k = 0; k < base.size(); ++k) {
        // Every view in the default theme; the others, whose geometry is the same, a spread of them (each picture costs).
        if (t != 0 && k % 4 != 1) {
          continue;
        }
        const app::ViewState& b = base[k];
        const app::ViewState v = with_overlay(b, overlay);
        const Scene skipped(v, v.shown_overlay(), true, t);
        const Scene whole(v, v.shown_overlay(), false, t);
        const std::string what = std::string(built_in(t).name) + ", overlay " + std::to_string(static_cast<int>(overlay)) +
                                 (b.screen == ScreenId::Song ? ", song" : ", pattern");
        CHECK_MESSAGE(skipped.words < whole.words, what << ": " << skipped.words << " words against " << whole.words);
        CHECK_MESSAGE(same_pixels(skipped, whole), what);
      }
    }
  }
}

TEST_CASE("overlay: skipping holds in the VERTEX2F fallback too, and saves words there as well") {
  const std::vector<app::ViewState> base = views();
  for (const OverlayId overlay : kOverlays) {
    for (std::size_t i : {std::size_t{0}, std::size_t{4}, base.size() - 1}) {
      const app::ViewState v = with_overlay(base[i], overlay);
      const Scene skipped(v, v.shown_overlay(), true, 0, false);
      const Scene whole(v, v.shown_overlay(), false, 0, false);
      CHECK(skipped.words < whole.words);
      CHECK(same_pixels(skipped, whole));
    }
  }
}

TEST_CASE("overlay: the Navigator covers the most and saves the most, the notification strip the least") {
  const app::ViewState b;
  std::size_t saved[3] = {};
  for (std::size_t k = 0; k < 3; ++k) {
    const app::ViewState v = with_overlay(b, kOverlays[k]);
    const Scene skipped(v, v.shown_overlay(), true);
    const Scene whole(v, v.shown_overlay(), false);
    saved[k] = whole.words - skipped.words;
    CHECK(saved[k] > 0u);
  }
  CHECK(saved[1] > saved[0]);  // the Navigator over Learn
  CHECK(saved[0] > saved[2]);  // Learn over the strip
  CHECK(saved[2] >= 30u);      // the strip alone drops the info line's hint and its undo depth
}

// An overlay paints only inside the rectangle it declares, with one exception: a modal one owns the F-key row (OS-NAV2: the Navigator's F8 is GO; OS-KEYROWDLG: every dialog's keys are its own), which the scene
// paints after it, on the frame band that rect() does not cover. That is sound for skipping: only what lies inside rect() is skipped, and the screen under a dialog draws no row (ViewState::overlay_keys).
TEST_CASE("overlay: an overlay paints only inside the rectangle it declares (and a modal one the F-key row)") {
  const std::vector<app::ViewState> base = views();
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    for (const OverlayId overlay : kOverlays) {
      for (const std::size_t i : {std::size_t{1}, std::size_t{7}, base.size() - 2}) {
        const app::ViewState v = with_overlay(base[i], overlay);
        const Scene bare(base[i], OverlayId::None, false, t);
        const Scene over(v, v.shown_overlay(), false, t);
        const Rect r = app::overlay_of(overlay)->rect(built_in(t));
        const Rect keys = app::make_frame(built_in(t), app::kFrameRows).keys;
        int outside = 0;
        int inside = 0;
        for (int y = 0; y < ui::kScreenH; ++y) {
          for (int x = 0; x < ui::kScreenW; ++x) {
            const bool differs = bare.pixel(x, y) != over.pixel(x, y);
            const bool in = (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) ||
                            (app::overlay_of(overlay)->modal() && y >= keys.y && y < keys.y + keys.h);
            outside += (differs && !in) ? 1 : 0;
            inside += (differs && in) ? 1 : 0;
          }
        }
        CHECK_MESSAGE(outside == 0, built_in(t).name << ", overlay " << static_cast<int>(overlay) << ": " << outside
                                                    << " pixels outside its rectangle");
        CHECK(inside > 100);  // and it does paint
      }
    }
  }
}

// OS-SCALES28 S2 (Q4c): the Fill dialog's KEY row shows "< A# Neap. minor >" (18 characters; it was 16 with the old names). The display list is looked at, not the pixels: the Data-face glyphs the Fill
// overlay puts on the KEY row's baseline, in the VERTEX2II / VERTEX2F words (no translate), and the right edge of the last one against the dialog's body.
namespace {
struct KeyRow {
  int glyphs = 0;      // the glyphs on the row's line (the caption "KEY" is a Label glyph at its own line, so these are the value's)
  int right = 0;       // the right edge of the last glyph
  int body_right = 0;  // the dialog body's right edge
  int cell = 0;
  int body_x = 0;
};

KeyRow fill_key_row(unsigned theme_index, unsigned scale, unsigned root) {
  const ui::Theme& theme = built_in(theme_index);
  app::ViewState v;
  v.screen = ScreenId::Pattern;
  v.overlay = OverlayId::Fill;
  v.fill.row = static_cast<std::uint8_t>(app::FillRow::Key);  // the selected row wears the "< >" arrows
  v.fill.plan.what = model::fill::What::Note;
  v.fill.plan.root = static_cast<std::uint8_t>(root);
  app::fill_set(v.fill, app::FillRow::Key, static_cast<float>(scale));
  CHECK(app::fill_row_used(v.fill, app::FillRow::Key));
  const app::Overlay* o = app::overlay_of(OverlayId::Fill);
  CHECK(o != nullptr);
  KeyRow out;
  if (o == nullptr) {
    return out;
  }
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, theme);
  r.use_translate(false);  // every glyph is a VERTEX2II (x < 512) or a VERTEX2F in sixteenths, never moved by VERTEX_TRANSLATE_X
  CHECK(r.begin(Color::Pane));
  o->render(demo_model(), v, r);
  CHECK(r.end());
  // The dialog as the overlay builds it (make_dialog with the rows, the preview and its count, the target and the keys): the same rectangle as the one the overlay declares, or this check is out of date.
  const ui::Metrics& m = theme.metrics;
  const app::Dialog d = app::make_dialog(theme, o->rect(theme).w, (static_cast<int>(app::kFillRows) + 4) * m.row_pitch);
  const Rect declared = o->rect(theme);
  CHECK((d.outer.x == declared.x && d.outer.y == declared.y && d.outer.w == declared.w && d.outer.h == declared.h));
  const int y = d.body.y + static_cast<int>(app::FillRow::Key) * m.row_pitch;
  const int key_y = ui::caps_y(theme, ui::Face::Data, y, m.row_pitch);
  out.cell = theme.geometry(ui::Face::Data).cell_w;
  out.body_x = d.body.x;
  out.body_right = d.body.x + d.body.w;
  bool bitmaps = false;
  for (std::size_t i = 0; i < display.last_frame_words(); ++i) {
    const std::uint32_t w = storage.words[i];
    int x = -1;
    int gy = -1;
    if ((w >> 24) == (eve::dl::begin(eve::Primitive::Bitmaps) >> 24)) {
      bitmaps = (w & 0xFu) == static_cast<std::uint32_t>(eve::Primitive::Bitmaps);
    } else if ((w >> 30) == eve::opcode::kVertex2iiTag) {
      x = static_cast<int>((w >> 21) & 0x1FFu);
      gy = static_cast<int>((w >> 12) & 0x1FFu);
    } else if ((w >> 30) == eve::opcode::kVertex2fTag) {
      const std::int32_t sx = static_cast<std::int32_t>((w >> 15) & 0x7FFFu);
      x = (sx >= 0x4000 ? sx - 0x8000 : sx) / 16;
      gy = static_cast<int>(w & 0x7FFFu) / 16;
    }
    if (bitmaps && gy == key_y && x >= d.body.x + 12 * out.cell) {  // the value column: from the 12th cell of the body (the caption sits in the first)
      ++out.glyphs;
      out.right = x + out.cell > out.right ? x + out.cell : out.right;
    }
  }
  return out;
}
}  // namespace

TEST_CASE("overlay: the Fill dialog's KEY row fits the dialog with every scale name under the longest root, in every theme (OS-SCALES28 S2: '< A# Neap. minor >' is 18 characters)") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const int pad = built_in(t).metrics.pad;
    int widest = 0;
    unsigned widest_scale = 0;
    for (unsigned scale = 0; scale < core::kScaleCount; ++scale) {
      app::FillView f;
      f.plan.what = model::fill::What::Note;
      f.plan.root = 10;  // A#: the root names are one or two characters, A# is the widest of them
      app::fill_set(f, app::FillRow::Key, static_cast<float>(scale));
      char text[32];
      const int chars = static_cast<int>(app::fill_text(f, app::FillRow::Key, text, sizeof text)) + 4;  // "< " and " >" around it
      const KeyRow k = fill_key_row(t, scale, 10);
      CHECK_MESSAGE(chars <= 18, built_in(t).name << ", scale " << scale << ": " << chars << " characters");
      CHECK_MESSAGE(k.glyphs > 0, built_in(t).name << ", scale " << scale << ": the check sees the row's glyphs");
      // The row's right edge is where the text says: the value column starts at the 12th cell after the body's padding, one cell a character.
      CHECK_MESSAGE(k.right == k.body_x + 2 * pad + (12 + chars) * k.cell, built_in(t).name << ", scale " << scale << " (" << text << "): the row ends at x " << k.right);
      CHECK_MESSAGE(k.right <= k.body_right, built_in(t).name << ", scale " << scale << " (" << text << "): the row ends at x " << k.right << ", the dialog's body at " << k.body_right);
      if (k.right > widest) {
        widest = k.right;
        widest_scale = scale;
      }
    }
    const KeyRow last = fill_key_row(t, widest_scale, 10);
    MESSAGE(built_in(t).name << ": the widest KEY row is scale " << widest_scale << " (" << core::kScaleNames[widest_scale] << "), ends at x " << last.right << " of " << last.body_right << " ("
                             << (last.body_right - last.right) << " px to spare, " << (last.body_right - last.right) / last.cell << " cells)");
  }
}

// OS-BUG-OGOVERLAY: the unit's captures show every RECTS fill at about 0.55 of its colour (OS-SIMKEY), so the dialog's fill does not hide what the page draws under it, and the progress box
// of an export read through ("FILE", "WHAT" and "SIZE" lines over its own text). The display list is looked at, not the pixels: the glyphs the BASE screen alone puts under the dialog.
namespace {
unsigned base_glyphs_in(const app::ViewState& view, const Rect& covered, const Rect& box, unsigned theme_index) {
  const ui::Theme& theme = built_in(theme_index);
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, theme);
  r.use_translate(false);  // every glyph is then a VERTEX2II (x < 512) or a VERTEX2F, in pixels or sixteenths, never moved by VERTEX_TRANSLATE_X
  CHECK(r.begin(Color::Pane));
  const app::ScreenEntry* e = app::find_screen(app::screen_registry(), view.screen);
  CHECK((e != nullptr && e->screen != nullptr));
  if (e == nullptr || e->screen == nullptr) {
    return 0u;
  }
  e->screen->render(demo_model(), view, r, covered);
  CHECK(r.end());
  const std::size_t words = display.last_frame_words();
  unsigned inside = 0;
  bool bitmaps = false;
  for (std::size_t i = 0; i < words; ++i) {
    const std::uint32_t w = storage.words[i];
    int x = -1;
    int y = -1;
    if ((w >> 24) == (eve::dl::begin(eve::Primitive::Bitmaps) >> 24)) {
      bitmaps = (w & 0xFu) == static_cast<std::uint32_t>(eve::Primitive::Bitmaps);
    } else if ((w >> 30) == eve::opcode::kVertex2iiTag) {
      x = static_cast<int>((w >> 21) & 0x1FFu);
      y = static_cast<int>((w >> 12) & 0x1FFu);
    } else if ((w >> 30) == eve::opcode::kVertex2fTag) {
      const std::int32_t sx = static_cast<std::int32_t>((w >> 15) & 0x7FFFu);
      x = (sx >= 0x4000 ? sx - 0x8000 : sx) / 16;
      y = static_cast<int>(w & 0x7FFFu) / 16;
    }
    if (bitmaps && x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h) {
      ++inside;
    }
  }
  return inside;
}
}  // namespace

TEST_CASE("overlay: the File EXPORT page draws no glyph under the progress dialog of a WAV, a stems or an OG export, in every theme (OS-BUG-OGOVERLAY)") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const Rect dialog = app::overlay_of(OverlayId::ExportProgress)->rect(built_in(t));
    for (const std::uint8_t kind : {app::ExportView::Mix, app::ExportView::Stems, app::ExportView::Og}) {
      app::ViewState v;
      v.screen = ScreenId::File;
      app::ExportView& x = v.xport;
      x.page = true;
      x.row = kind == app::ExportView::Og ? app::ExportView::OgProject : (kind == app::ExportView::Stems ? app::ExportView::PatternStems : app::ExportView::Pattern);
      x.pattern = 3;
      x.fresh = true;
      std::strcpy(x.preview, "EXPORT/T-V16/T-V16_P3_0001.WAV");
      x.og_valid = true;
      std::strcpy(x.og.folder, "Song OG");
      x.og.files = 29;
      x.og.bytes = 7'600'000u;
      x.og.drops.instruments_not_written = 5;
      x.og.drops.module_instruments = 3;
      x.og.drops.core_fx = 11;
      x.running = true;
      x.kind = kind;
      v.overlay = OverlayId::ExportProgress;
      const std::string what = std::string(built_in(t).name) + ", kind " + std::to_string(static_cast<int>(kind));
      CHECK_MESSAGE(base_glyphs_in(v, dialog, dialog, t) == 0u, what << ": the page draws text under the dialog");
      CHECK_MESSAGE(base_glyphs_in(v, Rect{0, 0, 0, 0}, dialog, t) > 0u, what << ": the check can see the page's text when nothing is skipped");
    }
  }
}

// ---- what they look like ---------------------------------------------------------------------------------------------------

TEST_CASE("overlay: Learn is a dialog with an inverted title bar, a frame, the instruction, the dash placeholder and the exit hint") {
  const app::ViewState v = with_overlay(app::ViewState{}, OverlayId::Learn);
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const Scene s(v, OverlayId::Learn, true, t);
    const Rect r = app::overlay_of(OverlayId::Learn)->rect(s.theme);
    CHECK(s.pixel(r.x, r.y) == s.rgb(Color::ChromeDim));                          // the frame, all round
    CHECK(s.pixel(r.x + r.w - 1, r.y + r.h - 1) == s.rgb(Color::ChromeDim));
    CHECK(s.pixel(r.x + 1, r.y + 1) == s.rgb(Color::Chrome));                     // a ring of chrome inside it
    CHECK(s.pixel(r.x + 2, r.y + 2) == s.rgb(Color::ChromeText));                 // then the inverted title bar...
    CHECK(s.count(Color::Chrome, r.x + 2, r.y + 2, 100, s.theme.metrics.row_pitch) > 30);  // ...with its title knocked out
    CHECK(s.pixel(r.x + r.w / 2, r.y + r.h - 3) == s.rgb(Color::Chrome));         // the body is chrome
    CHECK(s.count(Color::ChromeText, r.x, r.y + s.theme.metrics.row_pitch + 6, r.w, s.theme.metrics.row_pitch) > 100);  // the instruction
    CHECK(s.count(Color::ChromeDim, r.x, r.y, r.w, r.h) > 100);                   // the TARGET caption and the hint
    // Nothing orange: the accent is the cursor's alone (the cursor itself lies above the dialog here).
    CHECK(s.count(Color::Accent, r.x, r.y, r.w, r.h) == 0);
    CHECK(s.count(Color::Sel, r.x + 2, r.y + 2, r.w - 4, r.h - 4) == 0);
  }
}

TEST_CASE("overlay: the Navigator draws a cell for every registered screen, the chosen one in the selection blue") {
  const app::ScreenTable table = app::navigator_screens();  // OS-007d: the hidden diagnostic has no cell
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    int blue_cells_seen = 0;
    for (int chosen = 0; chosen < static_cast<int>(table.count); ++chosen) {
      app::ViewState v;
      v.overlay = OverlayId::Navigator;
      v.navigator = chosen;
      const Scene s(v, OverlayId::Navigator, true, t);
      const Rect r = app::overlay_of(OverlayId::Navigator)->rect(s.theme);
      // The selection blue fills one cell: about a quarter of the dialog's width by a few rows, and no other region of it.
      const int blue = s.count(Color::Sel, r.x + 2, r.y + 2, r.w - 4, r.h - 4);
      CHECK_MESSAGE((blue > 3000 && blue < 9000), built_in(t).name << " cell " << chosen << ": " << blue);
      blue_cells_seen += blue > 3000 ? 1 : 0;
      // Disabled cells use the dim ink, enabled ones the bright: with a cell of each kind in view both are present (the chosen cell is drawn in the selection's ink, so
      // when the only cell that is not there is the chosen one, no dim ink is left to see).
      unsigned disabled = 0;
      for (std::size_t k = 0; k < table.count; ++k) {
        disabled += table.items[k].screen == nullptr && static_cast<int>(k) != chosen ? 1u : 0u;
      }
      CHECK(s.count(Color::Text, r.x, r.y, r.w, r.h) > 100);
      if (disabled != 0u) {
        CHECK(s.count(Color::TextDim, r.x, r.y, r.w, r.h) > 100);
      }
    }
    CHECK(blue_cells_seen == static_cast<int>(table.count));
  }
  // The footer says whether the chosen cell can be opened: F8 GO for a screen (OS-NAV2), the other text for one that is not there. Every screen is there since OS-011c (Record was the
  // last one), so the footer is the same for every cell.
  app::ViewState song;
  song.overlay = OverlayId::Navigator;
  song.navigator = 1;
  app::ViewState record = song;
  record.navigator = 6;
  const Scene a(song, OverlayId::Navigator, true);
  const Scene b(record, OverlayId::Navigator, true);
  const Rect r = app::overlay_of(OverlayId::Navigator)->rect(a.theme);
  const int foot_y = r.y + r.h - 2 * a.theme.metrics.row_pitch;
  CHECK(a.count(Color::ChromeText, r.x, foot_y, r.w, 2 * a.theme.metrics.row_pitch) ==
        b.count(Color::ChromeText, r.x, foot_y, r.w, 2 * a.theme.metrics.row_pitch));
}

// The F-key row alone, as ui::soft_keys draws it on the chrome band: what the Navigator's row (and a screen's own) is compared with, pixel for pixel.
struct KeyRowRef {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  ui::Theme theme;

  KeyRowRef(const char* const* labels, unsigned theme_index) : theme(built_in(theme_index)) {
    CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, theme);
    r.use_translate(true);
    CHECK(r.begin(Color::Pane));
    const app::Frame f = app::make_frame(theme, app::kFrameRows);
    r.panel(f.keys, Color::Chrome);
    ui::soft_keys(r, f.keys, labels, 8);
    CHECK(r.end());
    CHECK(display.render_failures() == 0u);
  }
  std::uint32_t pixel(int x, int y) const {
    const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
  }
};

// True when the key row of the scene is the reference's, pixel for pixel.
bool key_row_is(const Scene& s, const KeyRowRef& ref) {
  const app::Frame f = app::make_frame(s.theme, app::kFrameRows);
  for (int y = f.keys.y; y < f.keys.y + f.keys.h; ++y) {
    for (int x = f.keys.x; x < f.keys.x + f.keys.w; ++x) {
      if (s.pixel(x, y) != ref.pixel(x, y)) {
        return false;
      }
    }
  }
  return true;
}

const char* const kNavigatorPlain[8] = {"GO", "", "", "", "", "", "CLOSE", "GO"};
const char* const kNavigatorShifted[8] = {"-", "-", "-", "-", "-", "-", "CLOSE", "GO"};

TEST_CASE("overlay: while the Navigator is up the F-key row is its own (F1 GO, F7 CLOSE, F8 GO, the rest blank; no jumps with Shift held, F7 / F8 stay CLOSE / GO), and the screen's own row is back when it closes (OS-NAV2)") {
  // The labels: F8 is GO here, not the screen's UNDO.
  // (The rows the Navigator had before its prompt table: its key row is byte-identical with them, except Shift + F6, which reads "-" like the five before it: OS-KEYROWDLG, Fable's gap (f).)
  const char* const* plain = kNavigatorPlain;
  const char* const* shifted = kNavigatorShifted;
  const char* const want[8] = {"GO", "", "", "", "", "", "CLOSE", "GO"};
  for (unsigned i = 0; i < 8; ++i) {
    CHECK_MESSAGE(std::string(plain[i]) == want[i], "F" << i + 1);
    CHECK_MESSAGE(std::string(shifted[i]) == (i < 6 ? std::string("-") : std::string(want[i])), "Shift F" << i + 1);  // Shift + F1..F6 are all bound to nothing
  }

  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const KeyRowRef nav_plain(plain, t);
    const KeyRowRef nav_shift(shifted, t);
    for (const ScreenId id : {ScreenId::Pattern, ScreenId::Song, ScreenId::Mixer, ScreenId::Config}) {
      const app::ScreenEntry* e = app::find_screen(app::screen_registry(), id);
      CHECK((e != nullptr && e->screen != nullptr));
      if (e == nullptr || e->screen == nullptr) {
        continue;
      }
      app::ViewState v;
      v.screen = id;
      // Closed: the screen's own row (nothing of the Navigator in it).
      // (Config opens on its MIDI page, whose F8 reads MAP: OS-LEARNMAP)
      const KeyRowRef own(id == ScreenId::Config ? app::config_key_row(false, app::kConfigPageMidi) : e->screen->soft_keys(), t);
      const Scene closed(v, OverlayId::None, true, t);
      CHECK_MESSAGE(key_row_is(closed, own), e->name << " theme " << t << " closed");
      CHECK_FALSE(key_row_is(closed, nav_plain));
      // Open: the Navigator's row, whatever the screen below says.
      v.overlay = OverlayId::Navigator;
      v.navigator = app::screen_position(app::navigator_screens(), id);
      const Scene open(v, OverlayId::Navigator, true, t);
      CHECK_MESSAGE(key_row_is(open, nav_plain), e->name << " theme " << t << " open");
      CHECK_FALSE(key_row_is(open, own));
      // With Shift held: the layer with the jumps.
      v.shift = true;
      const Scene open_shift(v, OverlayId::Navigator, true, t);
      CHECK_MESSAGE(key_row_is(open_shift, nav_shift), e->name << " theme " << t << " open, Shift");
      CHECK_FALSE(key_row_is(open_shift, nav_plain));
      // Closed again: the screen's own row once more.
      v.overlay = OverlayId::None;
      v.shift = false;
      const Scene closed_again(v, OverlayId::None, true, t);
      CHECK_MESSAGE(key_row_is(closed_again, own), e->name << " theme " << t << " closed again");
    }
  }
}

// Glyphs the F-key row draws for a table of labels: "F1".."F8" in the caps, then each label's characters but the spaces.
unsigned key_row_glyphs(const char* const* labels) {
  unsigned n = 16;
  for (unsigned i = 0; i < 8; ++i) {
    for (const char* c = labels[i]; *c != '\0'; ++c) {
      n += *c != ' ' ? 1u : 0u;
    }
  }
  return n;
}

// The F-key row is drawn once. With the Navigator's labels painted over the screen's, the screen's glyphs showed through on the unit wherever the Navigator's did not reach
// ("F1 GOTTERN" over PATTERN, F8 "GOOO" over UNDO, "1/4GTH" over LENGTH with Shift held) although the simulator, whose fill hides them, showed a clean row: so this counts the
// glyph words in the display list and does not look at pixels. Whichever screen is below, the Navigator's row is the F-numbers and its own labels and not one glyph more.
TEST_CASE("overlay: the display list has one F-key row while the Navigator is up, whichever screen is below (OS-SMALL2)") {
  // The counter sees a screen's own row when nothing covers it (so it would see an overprint): the Pattern screen's, 16 numbers and its labels.
  {
    app::ViewState v;
    const Scene closed(v, OverlayId::None, true);
    CHECK(closed.key_glyphs == key_row_glyphs(app::kPatternSoftKeys));
    CHECK(key_row_glyphs(app::kPatternSoftKeys) > key_row_glyphs(kNavigatorPlain));
  }
  const app::ScreenTable table = app::screen_registry();
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    for (std::size_t i = 0; i < table.count; ++i) {
      if (table.items[i].screen == nullptr) {
        continue;
      }
      app::ViewState v;
      v.screen = table.items[i].id;
      v.overlay = OverlayId::Navigator;
      const int cell = app::screen_position(app::navigator_screens(), table.items[i].id);
      v.navigator = cell >= 0 ? cell : 0;
      const Scene plain(v, OverlayId::Navigator, true, t);
      CHECK_MESSAGE(plain.key_glyphs == key_row_glyphs(kNavigatorPlain), table.items[i].name << " theme " << t << " open");
      v.shift = true;  // the quarter jumps: the Pattern and Song screens would show their own layer here
      const Scene shifted(v, OverlayId::Navigator, true, t);
      CHECK_MESSAGE(shifted.key_glyphs == key_row_glyphs(kNavigatorShifted), table.items[i].name << " theme " << t << " open, Shift");
    }
  }
}

// OS-KEYROWDLG S3: the boot disk's strip is a notice on the app's own Pattern screen (no overlay), so what stops the screen from drawing PATTERN .. UNDO under it is the one predicate, and the display list says so:
// the F-numbers and the disk row's words, and not one glyph of the screen's.
TEST_CASE("overlay: the display list has one F-key row while the boot disk strip is up (OS-KEYROWDLG)") {
  const struct {
    app::DiskKeys keys;
    unsigned glyphs;  // 16 for F1..F8, then the label's letters on each of the eight keys
  } cases[] = {{app::DiskKeys::Restart, 16u + 8u * 7u}, {app::DiskKeys::Confirm, 16u + 8u * 6u}, {app::DiskKeys::Finishing, 16u}};
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    {
      app::ViewState v;
      const Scene closed(v, OverlayId::None, true, t);
      CHECK(closed.key_glyphs == key_row_glyphs(app::kPatternSoftKeys));  // the row the disk's replaces
    }
    for (const auto& c : cases) {
      app::ViewState v;
      v.disk_keys = c.keys;
      (void)v.notify.post(core::Severity::Info, "USB DISK - EJECT ON THE COMPUTER OR PRESS A KEY", "disk", 2500);
      const Scene s(v, OverlayId::Notify, true, t);
      CHECK_MESSAGE(s.key_glyphs == c.glyphs, "theme " << t << " disk keys " << static_cast<unsigned>(c.keys));
      v.shift = true;  // Shift does not make a layer of it
      const Scene shifted(v, OverlayId::Notify, true, t);
      CHECK_MESSAGE(shifted.key_glyphs == c.glyphs, "theme " << t << " disk keys " << static_cast<unsigned>(c.keys) << ", Shift");
    }
  }
}

TEST_CASE("overlay: the notification strip replaces the info line: a severity tile, the text as an inverted chip, the source") {
  const struct {
    core::Severity severity;
    Color tile;
  } kinds[] = {{core::Severity::Info, Color::ChromeText}, {core::Severity::Warn, Color::ChromeDim}, {core::Severity::Error, Color::Sel}};
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    for (const auto& k : kinds) {
      app::ViewState v;
      (void)v.notify.post(k.severity, "SD CARD NOT FOUND", "sd", 2000);
      const Scene s(v, OverlayId::Notify, true, t);
      const app::Frame f = app::make_frame(s.theme, app::kFrameRows);
      CHECK(s.pixel(0, f.info.y) == s.rgb(Color::ChromeDim));   // the band's own top rule, as the info line has it
      CHECK(s.pixel(2, f.info.y + 4) == s.rgb(Color::Chrome));  // chrome: the tile starts a pad further in
      const int tile_x = 2 * s.theme.metrics.pad + 2;
      CHECK_MESSAGE(s.pixel(tile_x, f.info.y + f.info.h / 2) == s.rgb(k.tile), built_in(t).name << " severity "
                                                                                 << static_cast<int>(k.severity));
      // The chip behind the text is ChromeText, the text knocked out of it in Chrome; the source is dim at the right edge.
      CHECK(s.count(Color::ChromeText, 40, f.info.y + 2, 200, f.info.h - 3) > 400);
      CHECK(s.count(Color::Chrome, 40, f.info.y + 2, 200, f.info.h - 3) > 150);
      CHECK(s.count(Color::ChromeDim, ui::kScreenW - 40, f.info.y + 2, 40, f.info.h - 3) > 10);
      // The info line's own hint ("scale A min ...") is gone: the strip replaced it.
      CHECK(s.count(Color::ChromeText, 600, f.info.y + 2, 150, f.info.h - 3) == 0);
    }
  }
}

TEST_CASE("overlay: a coalesced notification shows its count after the text") {
  app::ViewState once;
  (void)once.notify.post(core::Severity::Info, "F3 JUMP", "ui", 2000);
  app::ViewState thrice = once;
  (void)thrice.notify.post(core::Severity::Info, "F3 JUMP", "ui", 2000);
  (void)thrice.notify.post(core::Severity::Info, "F3 JUMP", "ui", 2000);
  CHECK(thrice.notify.size() == 1u);
  app::ViewState many = thrice;
  for (int i = 0; i < 20; ++i) {
    (void)many.notify.post(core::Severity::Info, "F3 JUMP", "ui", 2000);
  }
  const Scene a(once, OverlayId::Notify, true);
  const Scene b(thrice, OverlayId::Notify, true);
  const Scene c(many, OverlayId::Notify, true);
  const app::Frame f = app::make_frame(a.theme, app::kFrameRows);
  // The chip grows by " x3" (three Label cells of 8 px), and by one digit more at ten and above.
  const auto chip_width = [&](const Scene& s) {  // the right edge of the chip, along its top row
    int x = 40;
    while (x < ui::kScreenW && s.pixel(x, f.info.y + 2) == s.rgb(Color::ChromeText)) {
      ++x;
    }
    return x;
  };
  CHECK(chip_width(b) == chip_width(a) + 24);
  CHECK(chip_width(c) == chip_width(a) + 32);
  CHECK(a.words < b.words);
  CHECK(b.words < c.words);
}

TEST_CASE("overlay: the scene falls back to the pattern screen when the screen in view does not exist, and draws nothing twice") {
  app::ViewState v;
  v.screen = static_cast<ScreenId>(app::kScreenCount);  // no such screen (every real one is implemented since OS-011c): navigation never lands here, but a stray state must not draw nothing
  const Scene s(app::ViewState{}, OverlayId::None, true);
  platform::host::FakeDisplay sink;
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(sink, storage.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, built_in(0));
  CHECK(r.begin(Color::Pane));
  app::render_scene(demo_model(), v, r);
  CHECK(r.end());
  CHECK(sink.last_frame().size() == s.words);  // the same frame as the pattern screen's
}
