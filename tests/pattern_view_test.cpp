// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/pattern_source.h>
#include <o3os/app/pattern_view.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/track_state.h>
#include <o3os/app/view_state.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/model/project.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/sim/screens.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Column;
using app::PatternData;
using app::PatternLayout;
using app::PatternView;
using ui::Color;

namespace {

const ui::Theme& built_in(unsigned i) { return ui::theme(ui::theme_id_at(i)); }

std::vector<std::uint32_t> frame_words(const PatternView& view, const PatternData& data, const char* const* soft_keys) {
  platform::host::FakeDisplay sink;
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(sink, storage.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, built_in(0));
  const bool began = r.begin(Color::Pane);
  CHECK(began);
  view.draw(r, data, PatternView::Status{1, soft_keys});
  CHECK(r.end());
  return sink.last_frame();
}

// The pattern's first step as `step`, the rest the demo's: the words of a frame (OS-009: off, cut and fade show no instrument; C10 and above are
// tiles of the second note handle).
std::vector<std::uint32_t> frame_with_first_step(const app::Step& step) {
  auto data = std::make_unique<PatternData>();
  app::fill_demo(*data);
  data->steps[0][0] = step;
  return frame_words(PatternView{}, *data, app::App::kSoftKeyLabels);
}

app::Step note_step(std::uint8_t note, std::uint8_t instr) {
  app::Step s = model::kEmptyStep;
  s.note = note;
  s.instr = instr;
  return s;
}

// The view drawn into the simulated panel, assets in RAM_G.
struct Shot {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  ui::Theme theme;
  explicit Shot(const PatternView& view, unsigned theme_index = 0, bool translate = true, const app::Selection* selection = nullptr,
       void (*tweak)(PatternView::Status&) = nullptr)
      : theme(built_in(theme_index)) {
    CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
    auto data = std::make_unique<PatternData>();
    app::fill_demo(*data);
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, theme);
    r.use_translate(translate);
    CHECK(r.begin(Color::Pane));
    PatternView::Status status{1, app::App::kSoftKeyLabels};
    if (selection != nullptr) {
      status.selection = *selection;
    }
    if (tweak != nullptr) {
      tweak(status);
    }
    view.draw(r, *data, status);
    CHECK(r.end());
    CHECK(display.render_failures() == 0u);
  }
  std::uint32_t pixel(int x, int y) const {
    const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
  }
  std::uint32_t rgb(Color c) const {
    const ui::Rgb& v = theme.rgb(c);
    return (std::uint32_t{v.r} << 16) | (std::uint32_t{v.g} << 8) | v.b;
  }
  // Pixels of `c` in the rectangle.
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

}  // namespace

// The worst-case budget test of this view lives in budget_test.cpp, on the harness (sim/budget.h) and the screen
// registry (sim/screens.cpp): `budget: every worst-case state fits its limit`.

TEST_CASE("pattern view: the soft-key labels are the caller's table, not the view's own") {
  PatternData data{};
  const PatternView view;
  const char* const a[8] = {"AAAA", "B", "C", "D", "E", "F", "G", "H"};
  const char* const b[8] = {"ZZZZ", "B", "C", "D", "E", "F", "G", "H"};
  const auto fa = frame_words(view, data, a);
  const auto fb = frame_words(view, data, b);
  CHECK(fa != fb);                                           // F1's label changed the picture...
  CHECK(fa.size() == fb.size());                             // ...by glyphs, not by layout
  CHECK(frame_words(view, data, a) == fa);                   // and the same table gives the same words
  CHECK(fa != frame_words(view, data, app::App::kSoftKeyLabels));
}

TEST_CASE("pattern layout: the bands fill the panel and the fields fit their column, in every theme") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const PatternLayout l = app::make_pattern_layout(built_in(t), PatternView::kVisibleRows, PatternView::kVisibleTracks);
    CHECK_MESSAGE(l.fits, built_in(t).name);
    CHECK(l.strip.y == 0);
    CHECK(l.header.y == l.strip.y + l.strip.h);
    CHECK(l.grid.y == l.header.y + l.header.h);
    CHECK(l.info.y == l.grid.y + l.grid.h);
    CHECK(l.keys.y == l.info.y + l.info.h);
    CHECK(l.keys.y + l.keys.h == ui::kScreenH);
    CHECK(l.grid.h == PatternView::kVisibleRows * l.row_pitch);
    CHECK(l.track0_x + l.tracks * l.track_w <= ui::kScreenW);
    CHECK(l.tracks >= 5);  // as many tracks as the Data face allows, at least 5
    // Fields: in order, one gap apart, inside the column (after its separator, before the next one).
    const ui::Metrics& m = built_in(t).metrics;
    CHECK(l.field_x[0] >= m.border);
    for (int i = 0; i + 1 < app::kLayoutFields; ++i) {
      CHECK(l.field_x[i + 1] == l.field_x[i] + l.field_w[i] + m.gap);
    }
    CHECK(l.field_x[app::kLayoutFields - 1] + l.field_w[app::kLayoutFields - 1] + m.border <= l.track_w);
    // The Data cell lies in its row, and the header's pieces lie in the header, bottom to top without touching.
    CHECK((l.cell_dy >= 0 && l.cell_dy + built_in(t).geometry(ui::Face::Data).cell_h <= l.row_pitch));
    CHECK(l.underline_y + 2 * m.border == l.header.y + l.header.h);
    CHECK(l.meter_h == m.meter_h + 2 * m.border);  // the level bar and its frame
    CHECK(l.meter_y + l.meter_h < l.underline_y);
    CHECK(l.badge_y + l.badge_size < l.meter_y);
    CHECK(l.badge_y >= l.header.y);
  }
}

TEST_CASE("pattern layout: the Amiga numbers, worked by hand") {
  const PatternLayout l = app::make_pattern_layout(built_in(0), 16, 5);
  // 32 + 26 + 16 * 24 + 16 + 22 = 480.
  CHECK((l.strip.h == 32 && l.header.y == 32 && l.header.h == 26 && l.grid.y == 58 && l.grid.h == 384));
  CHECK((l.info.y == 442 && l.info.h == 16 && l.keys.y == 458 && l.keys.h == 22));
  // (800 - 30) / 5 = 154 per track; the 130 px of tiles and four 4 px gaps are 146, centred: 4 px on the left.
  CHECK((l.track0_x == 30 && l.track_w == 154 && l.track_x(4) == 646 && l.row_y(15) == 418));
  CHECK((l.field_x[0] == 4 && l.field_x[1] == 38 && l.field_x[2] == 62 && l.field_x[3] == 86 && l.field_x[4] == 120));
  CHECK((l.field_w[0] == 30 && l.field_w[1] == 20 && l.field_w[2] == 20 && l.field_w[3] == 30 && l.field_w[4] == 30));
  CHECK(l.index_x == 5);  // two 10 px digits centred in the 30 px column
  CHECK((l.cell_dy == 2 && l.cursor_pad_x == 2 && l.cursor_dy == 2 && l.cursor_h == 19 && l.cursor_outline == 1));
  // Header, bottom up: underline 56..57, a pixel of air, the 6 px meter (a 4 px bar and its frame) 49..54, air, the badge.
  CHECK((l.underline_y == 56 && l.meter_y == 49 && l.meter_h == 6 && l.badge_size == 14 && l.badge_y == 33));
  // The strip's badge: 32 less a pad and a border above and below; the dividers: 8 px of air above and below... a pad twice.
  CHECK((l.strip_badge_h == 22 && l.strip_badge_y == 5 && l.divider_y == 8 && l.divider_h == 16));
}

TEST_CASE("pattern view: the selected row is the one strong highlight, the cursor the one orange, nothing else bands the grid") {
  PatternView view;
  view.scroll_rows(5);
  view.move_right();  // the Instr column of track 0
  const Shot s(view);
  const PatternLayout l = app::make_pattern_layout(s.theme, 16, 5);
  const int cs = PatternView::kCentreRow;  // the cursor's row is on the centre slot (OS-CENTER, OS-PATCENTER): row 5 has two blank slots above it
  const int sel_y = l.row_y(cs);
  CHECK(s.pixel(780, sel_y + 10) == s.rgb(Color::Sel));          // the bar spans the whole width...
  CHECK(s.pixel(2, sel_y) == s.rgb(Color::Sel));                  // ...from its first pixel
  CHECK(s.pixel(780, sel_y + l.row_pitch - 1) == s.rgb(Color::Sel));
  CHECK(s.pixel(780, sel_y - 1) == s.rgb(Color::Pane));            // no halo, no neighbour
  CHECK(s.pixel(780, sel_y + l.row_pitch) == s.rgb(Color::Pane));
  // The cursor block: the Instr field's rectangle grown by the pad, in the accent; orange nowhere else.
  const int cx = l.track_x(0) + l.field_x[1] - l.cursor_pad_x;
  CHECK(s.pixel(cx, sel_y + l.cursor_dy) == s.rgb(Color::Accent));
  CHECK(s.pixel(cx + l.field_w[1] + 2 * l.cursor_pad_x - 1, sel_y + l.cursor_dy + l.cursor_h - 1) == s.rgb(Color::Accent));
  // ...with a one-pixel AccentText outline all round, which is what sets it off the selected row.
  CHECK(s.pixel(cx - 1, sel_y + l.cursor_dy) == s.rgb(Color::AccentText));
  CHECK(s.pixel(cx, sel_y + l.cursor_dy - 1) == s.rgb(Color::AccentText));
  CHECK(s.pixel(cx + l.field_w[1] + 2 * l.cursor_pad_x, sel_y + l.cursor_dy + l.cursor_h - 1) == s.rgb(Color::AccentText));
  CHECK(s.pixel(cx - 2, sel_y + l.cursor_dy) == s.rgb(Color::Sel));
  CHECK(s.pixel(cx, sel_y + l.cursor_dy - 2) == s.rgb(Color::Sel));
  CHECK(s.count(Color::Accent, 0, 0, 800, 480) > 0);
  CHECK(s.count(Color::Accent, 0, 0, 800, sel_y) == 0);
  CHECK(s.count(Color::Accent, 0, sel_y + l.row_pitch, 800, 480 - sel_y - l.row_pitch) == 0);
  CHECK(s.count(Color::Accent, 0, sel_y, cx, l.row_pitch) == 0);  // none left of the block either
  // No banding: an empty spot of the pane is the same in a beat row (4) and a plain one (6), the slots either side of the cursor's row 5.
  CHECK(s.pixel(l.track_x(1) + 100, l.row_y(cs - 1) + 1) == s.rgb(Color::Pane));
  CHECK(s.pixel(l.track_x(1) + 100, l.row_y(cs + 1) + 1) == s.rgb(Color::Pane));
  // Beat row numbers are bright, the others dim: row 4 has Text ink and no TextDim, row 6 the opposite. The selected row
  // draws its number in SelText.
  CHECK(s.count(Color::Text, l.index_x, l.row_y(cs - 1), 20, l.row_pitch) > 20);
  CHECK(s.count(Color::TextDim, l.index_x, l.row_y(cs - 1), 20, l.row_pitch) == 0);
  CHECK(s.count(Color::TextDim, l.index_x, l.row_y(cs + 1), 20, l.row_pitch) > 20);
  CHECK(s.count(Color::Text, l.index_x, l.row_y(cs + 1), 20, l.row_pitch) == 0);
  CHECK(s.count(Color::SelText, l.index_x, sel_y, 20, l.row_pitch) > 20);
}

// OS-SELTINT: over several tracks the cursor's track keeps the Sel blue and the other tracks of the block are SelDim; one track, or a column block, is Sel throughout. The sample is
// the right-hand pad of a track cell, where no glyph is drawn.
TEST_CASE("pattern view: a block over several tracks tints the tracks other than the cursor's SelDim") {
  const auto block = [](int row0, int row1, int track0, int track1, bool column) {
    app::Selection sel;
    sel.active = true;
    sel.start_row = static_cast<std::int16_t>(row0);
    sel.end_row = static_cast<std::int16_t>(row1);
    sel.start_track = static_cast<std::int8_t>(track0);
    sel.end_track = static_cast<std::int8_t>(track1);
    sel.column = column;
    return sel;
  };
  PatternView view;
  view.scroll_rows(5);
  for (int i = 0; i < 5; ++i) {
    view.move_right();  // track 1, the middle of the block
  }
  const int cs = PatternView::kCentreRow;
  const auto probe = [](const Shot& s, const PatternLayout& l, int track, int slot) {
    return s.pixel(l.track_x(track) + l.track_w - 3, l.row_y(slot) + l.row_pitch / 2);
  };
  for (unsigned theme_index = 0; theme_index < 3; ++theme_index) {
    const app::Selection sel = block(3, 8, 0, 2, false);
    const Shot s(view, theme_index, true, &sel);
    const PatternLayout l = app::make_pattern_layout(s.theme, 16, 5);
    for (const int slot : {cs - 1, cs + 1, cs + 3}) {
      CHECK(probe(s, l, 0, slot) == s.rgb(Color::SelDim));
      CHECK(probe(s, l, 1, slot) == s.rgb(Color::Sel));  // the cursor's track
      CHECK(probe(s, l, 2, slot) == s.rgb(Color::SelDim));
      CHECK(probe(s, l, 3, slot) == s.rgb(Color::Pane));  // outside the block
    }
    CHECK(probe(s, l, 0, cs - 3) == s.rgb(Color::Pane));  // above the block (row 2)
    // The cursor's own row: its bar is Sel across the pane, and the block's other tracks are SelDim over it.
    CHECK(probe(s, l, 0, cs) == s.rgb(Color::SelDim));
    CHECK(probe(s, l, 1, cs) == s.rgb(Color::Sel));
    CHECK(probe(s, l, 2, cs) == s.rgb(Color::SelDim));
    CHECK(probe(s, l, 3, cs) == s.rgb(Color::Sel));  // the bar, outside the block
  }
  {  // the cursor at the block's first track: the rest are dimmed, the left of it is not drawn
    PatternView first;
    first.scroll_rows(5);
    const app::Selection sel = block(3, 8, 0, 2, false);
    const Shot s(first, 0, true, &sel);
    const PatternLayout l = app::make_pattern_layout(s.theme, 16, 5);
    CHECK(probe(s, l, 0, cs + 1) == s.rgb(Color::Sel));
    CHECK(probe(s, l, 1, cs + 1) == s.rgb(Color::SelDim));
    CHECK(probe(s, l, 2, cs + 1) == s.rgb(Color::SelDim));
  }
  {  // one track: Sel, no SelDim anywhere
    app::Selection one = block(3, 8, 1, 1, false);
    one.end_col = static_cast<std::int8_t>(app::Selection::kLastColumn);  // a block inside one track is whole steps only with every column
    const Shot s(view, 0, true, &one);
    const PatternLayout l = app::make_pattern_layout(s.theme, 16, 5);
    CHECK(probe(s, l, 1, cs + 1) == s.rgb(Color::Sel));
    CHECK(s.count(Color::SelDim, 0, 0, 800, 480) == 0);
  }
}

TEST_CASE("pattern view: a bar is a one-pixel rule every 16 steps, empty fields are quiet dashes") {
  PatternView view;
  view.scroll_rows(43);  // the cursor on the centre slot (7) of the window of steps 36..51 (OS-CENTER): step 48 starts a bar, slot 12
  const Shot s(view);
  const PatternLayout l = app::make_pattern_layout(s.theme, 16, 5);
  CHECK(s.pixel(10, l.row_y(12)) == s.rgb(Color::TextDim));    // the rule, across the index column too
  CHECK(s.pixel(10, l.row_y(12) + 1) == s.rgb(Color::Pane));   // one pixel thick
  CHECK(s.pixel(10, l.row_y(12) - 1) == s.rgb(Color::Pane));
  CHECK(s.pixel(780, l.row_y(12)) == s.rgb(Color::TextDim));
  // It is a clear line, not a hairline: as visible as the dim row numbers, well above the 2:1 of the quiet separators.
  CHECK(s.pixel(l.track_x(3) + 70, l.row_y(12)) == s.rgb(Color::TextDim));
  CHECK(s.pixel(10, l.row_y(4)) == s.rgb(Color::Pane));      // step 40 is a beat, not a bar: no rule
  // Track separators run the height of the pane.
  CHECK(s.pixel(l.track_x(2), l.grid.y + 3) == s.rgb(Color::Empty));
  CHECK(s.pixel(l.track_x(2), l.row_y(14) + l.row_pitch - 1) == s.rgb(Color::Empty));  // (the selected row's bar covers them)
  // The step 48 row of track 0 (KICK, every 4th step): a note (Text), its instrument and volume, then dashes.
  CHECK(s.count(Color::Text, l.track_x(0) + l.field_x[0], l.row_y(12), l.field_w[0], l.row_pitch) > 20);
  CHECK(s.count(Color::Instr, l.track_x(0) + l.field_x[1], l.row_y(12), l.field_w[1], l.row_pitch) > 20);
  CHECK(s.count(Color::Vol, l.track_x(0) + l.field_x[2], l.row_y(12), l.field_w[2], l.row_pitch) > 20);
  CHECK(s.count(Color::Empty, l.track_x(0) + l.field_x[3], l.row_y(12) + 5, l.field_w[3], 13) > 5);  // "---" of FX1
  // An empty step: dashes, in the dim colour, in the note field too.
  CHECK(s.count(Color::Empty, l.track_x(0) + l.field_x[0], l.row_y(13) + 5, l.field_w[0], 13) > 5);
  CHECK(s.count(Color::Text, l.track_x(0) + l.field_x[0], l.row_y(13), l.field_w[0], l.row_pitch) == 0);
}

TEST_CASE("pattern view: track headers carry a type badge, the name, a meter and the selected track's underline") {
  PatternView view;
  for (int i = 0; i < 5 * 2; ++i) {
    view.move_right();  // track 2
  }
  const Shot s(view);
  const PatternLayout l = app::make_pattern_layout(s.theme, 16, 5);
  const ui::Metrics& m = s.theme.metrics;
  for (int slot = 0; slot < 5; ++slot) {
    const int x = l.track_x(slot);
    CHECK(s.pixel(x + m.pad, l.badge_y) == s.rgb(Color::ChromeText));                    // the inverted badge
    CHECK(s.count(Color::Chrome, x + m.pad, l.badge_y, l.badge_size, l.badge_size) > 15);  // with its letter
    CHECK(s.pixel(x + m.pad, l.meter_y) == s.rgb(Color::ChromeDim));                      // the meter's frame...
    CHECK(s.pixel(x + m.pad + m.border, l.meter_y + m.border) == s.rgb(Color::Play));     // ...round the fill
    // The selected track's underline is the selection blue, the others' is bare chrome.
    CHECK(s.pixel(x + 40, l.underline_y) == s.rgb(slot == 2 ? Color::Sel : Color::Chrome));
    // The name is bright on the selected header, dim on the others.
    CHECK(s.count(slot == 2 ? Color::ChromeText : Color::ChromeDim, x + m.pad + l.badge_size + m.gap, l.badge_y, 100,
                  l.badge_size) > 30);
  }
  // Track 8 (index 7, PERC) is silent in the demo: no fill, only the slot.
  PatternView far;
  for (int i = 0; i < 5 * 7; ++i) {
    far.move_right();
  }
  const Shot t(far);
  const PatternLayout lf = app::make_pattern_layout(t.theme, 16, 5);
  CHECK(far.left_track() == 3);
  CHECK(t.pixel(lf.track_x(4) + m.pad + m.border, lf.meter_y + m.border) == t.rgb(Color::Pane));  // slot 4 is track 7 (PERC): level 0
}

TEST_CASE("pattern view: the strip shows the pattern and the tempo in the Display face, the transport as a badge") {
  const PatternView view;
  const Shot s(view);
  const PatternLayout l = app::make_pattern_layout(s.theme, 16, 5);
  // "03" and "128.0" are Display glyphs: 20 capital rows of ChromeText ink in the strip (6..25 of the 32 px cell).
  int tall_rows = 0;
  for (int y = 0; y < l.strip.h; ++y) {
    tall_rows += s.count(Color::ChromeText, 0, y, 200, 1) > 0 ? 1 : 0;
  }
  CHECK(tall_rows >= 20);
  CHECK(s.count(Color::ChromeDim, 0, 0, 800, l.strip.h) > 100);  // captions and the STOP badge, in the grey
  // The key-caps: the eight inverted boxes sit in the key row.
  for (int k = 0; k < 8; ++k) {
    CHECK(s.pixel(k * 100 + 4, l.keys.y + 2) == s.rgb(Color::ChromeText));
  }
}

// OS-SELTINT review: a block wider than the window (the cursor's track is the window's last or first, or not in the block at all) draws its runs inside the grid and fails nothing.
TEST_CASE("pattern view: a block wider than the window draws its Sel and SelDim runs inside the grid, whichever side the cursor's track is on (OS-SELTINT)") {
  const auto block = [](int track0, int track1) {
    app::Selection sel;
    sel.active = true;
    sel.start_row = 3;
    sel.end_row = 8;
    sel.start_track = static_cast<std::int8_t>(track0);
    sel.end_track = static_cast<std::int8_t>(track1);
    sel.column = false;
    return sel;
  };
  const int cs = PatternView::kCentreRow;
  const auto probe = [](const Shot& s, const PatternLayout& l, int track, int slot) {
    return s.pixel(l.track_x(track) + l.track_w - 3, l.row_y(slot) + l.row_pitch / 2);
  };
  struct Case {
    int cursor_track;  // where the cursor stands (the window follows it)
    int track0;
    int track1;
  };
  // 5 tracks are in the window. {9, 0, 9}: the cursor at the right end, the window scrolled to tracks 5..9 (the block starts left of it); {0, 0, 9}: the cursor at the left end, the block runs on to the right
  // of the window; {4, 6, 9}: the cursor left of the block (window 0..4, the block starts past it); {9, 0, 3}: the cursor right of the block (window 5..9, the block is past the left edge).
  for (const Case c : {Case{9, 0, 9}, Case{0, 0, 9}, Case{4, 6, 9}, Case{9, 0, 3}, Case{7, 2, 12}}) {
    PatternView view;
    view.scroll_rows(5);
    view.move_track(c.cursor_track);
    const app::Selection sel = block(c.track0, c.track1);
    const Shot s(view, 0, true, &sel);  // (Shot checks the list ends and no render failure)
    const PatternLayout l = app::make_pattern_layout(s.theme, 16, 5);
    const int left = c.cursor_track >= 5 ? c.cursor_track - 4 : 0;
    for (int slot = 0; slot < 5; ++slot) {
      const int track = left + slot;
      const bool in_block = track >= c.track0 && track <= c.track1;
      const std::uint32_t want = !in_block ? s.rgb(Color::Pane) : (track == c.cursor_track ? s.rgb(Color::Sel) : s.rgb(Color::SelDim));
      CHECK(probe(s, l, slot, cs + 1) == want);
    }
    // Nothing is painted right of the grid's last track or left of its first.
    CHECK(s.count(Color::SelDim, 0, 0, l.track_x(0), 480) == 0);
    CHECK(s.count(Color::SelDim, l.track_x(4) + l.track_w, 0, 800 - (l.track_x(4) + l.track_w), 480) == 0);
  }
}

// OS-INFOLINE review: the worst line the info text can be (the longest scale name, track 16, a 3-digit named pattern at the name's full room, pads TRACKS, METRO): it ends before the undo depth.
TEST_CASE("pattern view: the info text, at its worst-case width with METRO on, stays clear of the undo depth (OS-INFOLINE)") {
  PatternView view;
  view.move_track(15);
  const Shot plain(view, 0, true, nullptr, [](PatternView::Status& s) { s.undo = 0; });
  const PatternLayout l = app::make_pattern_layout(plain.theme, 16, 5);
  const int pad = plain.theme.metrics.pad;
  const int y0 = l.info.y + 1;
  const int h = l.info.h - 1;
  const std::uint32_t bg = plain.pixel(0, l.info.y + 1);
  // The undo depth's left edge on the plain line: the first column from the right half that has ink (the left text ends well before it).
  int undo_left = -1;
  for (int x = 400; x < 800 && undo_left < 0; ++x) {
    for (int y = y0; y < y0 + h; ++y) {
      if (plain.pixel(x, y) != bg) {
        undo_left = x;
        break;
      }
    }
  }
  CHECK(undo_left > 400);
  const Shot worst(view, 0, true, nullptr, [](PatternView::Status& s) {
    s.undo = 0;
    s.scale = "Hung. minor";  // the longest of core/scales.h (kScaleNameChars = 11)
    s.root = 49;              // a sharp: the widest note name
    s.pattern = 100;          // three digits
    s.pattern_name = "WWWWWWWWWWWWWWWW";  // the widest letters, more than kInfoNameChars: cut to 14
    s.pads = "TRACKS";
    s.metro = true;
  });
  // No ink in the pad before the undo text, and the undo text itself is the plain line's.
  for (int x = undo_left - pad; x < undo_left; ++x) {
    for (int y = y0; y < y0 + h; ++y) {
      CHECK(worst.pixel(x, y) == bg);
    }
  }
  bool same = true;
  for (int x = undo_left; x < 800 && same; ++x) {
    for (int y = y0; y < y0 + h && same; ++y) {
      same = worst.pixel(x, y) == plain.pixel(x, y);
    }
  }
  CHECK(same);
}

TEST_CASE("pattern view: the info line is the hint at full contrast, with the mode, and the undo depth at the right") {
  const PatternView view;
  const Shot hint(view);
  const PatternLayout l = app::make_pattern_layout(hint.theme, 16, 5);
  CHECK(hint.count(Color::ChromeText, 0, l.info.y + 1, 500, l.info.h - 1) > 100);  // full contrast, in the Label face
  CHECK(hint.count(Color::ChromeDim, 0, l.info.y + 1, 800, l.info.h - 1) == 0);    // nothing dim on the line
  CHECK(hint.count(Color::ChromeText, 700, l.info.y + 1, 100, l.info.h - 1) > 30); // the undo depth at the right edge
  CHECK(hint.count(Color::Chrome, 4, l.info.y + 1, 400, l.info.h - 1) > 1000);     // no chip: the line is bare chrome
}

// OS-INFOLINE: the mode chip stands first, at x = 2 * pad, whatever the text after it says; the text, METRO and the hint follow it and nothing overlaps the undo depth, even at the longest text.
TEST_CASE("pattern view: the mode chip is first on the info line at the left edge, and the longest line (named pattern, pads TRACKS, METRO, hint) keeps clear of the undo depth (OS-INFOLINE)") {
  const PatternView view;
  const Shot plain(view);
  const PatternLayout l = app::make_pattern_layout(plain.theme, 16, 5);
  const int pad = plain.theme.metrics.pad;
  const int chip_x = 2 * pad;
  const int y_mid = l.info.y + l.info.h / 2;
  // Left of the chip is bare chrome; the chip's own left edge is the ChromeText fill.
  CHECK(plain.pixel(chip_x - 1, y_mid) == plain.pixel(0, y_mid));
  CHECK(plain.pixel(chip_x, y_mid) != plain.pixel(0, y_mid));
  CHECK(plain.pixel(chip_x + 1, y_mid) != plain.pixel(0, y_mid));
  // The first ChromeText fill column from the left is the chip, nothing of the text sits left of it.
  CHECK(plain.count(Color::ChromeText, 0, l.info.y + 1, chip_x, l.info.h - 1) == 0);

  const Shot longest(view, 0, true, nullptr, [](PatternView::Status& s) {
    s.pattern_name = "ABCDEFGHIJKLMNOP";
    s.pads = "TRACKS";
    s.metro = true;
    s.hint = "BLOCK 16 STEPS  ENC: TRANSPOSE  F7: TOOLS  PAD: SET  DEL: CLEAR";
  });
  CHECK(longest.count(Color::ChromeText, 0, l.info.y + 1, chip_x, l.info.h - 1) == 0);
  // The undo depth at the right edge is intact: the same pixels as on the plain line, so nothing was drawn over it.
  bool same = true;
  for (int x = 700; x < 800 && same; ++x) {
    for (int y = l.info.y + 1; y < l.info.y + l.info.h && same; ++y) {
      same = longest.pixel(x, y) == plain.pixel(x, y);
    }
  }
  CHECK(same);
  // A NOTES pad mode is named too: the line with "pads NOTES" has more ink than the one with no pads text.
  const Shot notes(view, 0, true, nullptr, [](PatternView::Status& s) { s.pads = "NOTES"; });
  CHECK(notes.count(Color::ChromeText, 0, l.info.y + 1, 700, l.info.h - 1) > plain.count(Color::ChromeText, 0, l.info.y + 1, 700, l.info.h - 1));
}

// Plan B for the one thing the guide does not say (does VERTEX_TRANSLATE_X move VERTEX2II?): the renderer can reach x >= 512
// with BITMAP_HANDLE + CELL + VERTEX2F instead, with no translation at all. The two must draw the same picture. This is
// the whole view, not a glyph: every theme and several windows and cursors, compared pixel
// for pixel. (The budget tests hold both modes to the 1792 words.)
TEST_CASE("translate: the VERTEX2F fallback draws exactly the pixels of the VERTEX_TRANSLATE_X path, whole view, every theme") {
  struct State {
    int row;
    int moves;  // steps to the right: 5 per track
  };
  const State states[] = {{0, 0}, {5, 1}, {22, 17}, {40, 5 * 7}, {63, 59}, {31, 5 * 11 + 4}};
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    for (const State& st : states) {
      PatternView view;
      view.scroll_rows(st.row);
      for (int i = 0; i < st.moves; ++i) {
        view.move_right();
      }
      const Shot a(view, t, true);
      const Shot b(view, t, false);
      CHECK_MESSAGE(std::memcmp(a.raster.rgba(), b.raster.rgba(), sim::EveRaster::kFramebufferBytes) == 0,
                    built_in(t).name << ": row " << st.row << ", " << st.moves << " moves");
      // They really are two different lists: the fallback spends words (CELL, BITMAP_HANDLE) where the origin moved.
      CHECK(b.display.last_frame_words() > a.display.last_frame_words());
    }
  }
}

// ---- the contrast table is the pairs that are drawn, exactly ------------------------------------------------------------
//
// ui/theme.h lists the token pairs that get a contrast check. Written by hand it can miss one (it once missed the cursor
// on the selected row, the PLAY badge on chrome, the meter slot on chrome). So it is derived from the pictures: every two
// tokens that touch in a rendered frame (the pattern view in several windows and cursors, and the style
// specimen's components) must be in the table, or be a decoration named below; and every pair in the table must really
// occur. The Amiga theme is the one rendered: its sixteen colours are all different, so a pixel names its token.

namespace {

using Pair = unsigned;  // the two tokens, unordered: smaller << 5 | larger (17 tokens)
Pair pair_of(Color a, Color b) {
  const unsigned x = static_cast<unsigned>(a), y = static_cast<unsigned>(b);
  return (x < y ? x : y) << 5 | (x < y ? y : x);
}

// Decorations: a one-pixel rule, frame or separator running beside something. Their contrast is not an indicator's to
// meet (the rule is read against what it divides, which the table covers); the table says nothing of them on purpose.
//   ChromeDim beside Pane, Sel, Empty    the rule at the edge of a chrome band and a meter's frame, where they meet the pane,
//                                        the selected row (the cursor on the last row) or a track separator
//   ChromeDim beside Text, Play          the frame of a meter touches the green fill and, at a peak of 100 %, the tick; the
//                                        PLAY badge's frame touches its green (each is read against what is inside it)
//   Empty beside Sel, TextDim            a track separator running into the selected row, and crossing a bar rule
//   TextDim beside Sel                   a bar rule directly under or over the selected row
//   Empty beside any token               the frame round each swatch of the style specimen
//   Text beside Sel                      OS-MUTEVIS: the edge of a MUTE / SOLO tag (a bright chip across a track's column) against the selected row's bar, when the cursor is next to its slot; its letters
//                                        (Pane on Text) are the table's pair
//   ChromeDim, Sel beside Rec            Rec mode: the three-pixel Rec frame and the Rec playhead bar run into the rule at the edge of a chrome band and into the
//                                        selected row's bar (the cursor on the first or last row, or the row next to the playhead); each is read against the pane
//   Text beside Accent                   OS-SONG3 S2: the TIMELINE's selection band and cursor cell are Accent; a tick of the note map (Text) in the next slot, and the playhead's two-pixel line where it crosses the
//                                        band, lie beside it. The ticks inside the band are AccentText (the table's pair); the line is a marker, read against the pane like the Rec playhead
//   Sel beside SelDim                    OS-SELTINT: the cursor's track and the other tracks of a block lie side by side, a tint a step apart on purpose (the text is SelText on both)
//   Rec beside SelDim                    OS-SELTINT: as Sel beside Rec, for the other tracks of a block (the Rec frame and the Rec playhead bar run into their tint)
//   Text, TextDim beside Rec             OS-RECSIGN: the Rec frame round the grid is drawn in every Edit picture now (STEP too), so it runs beside a note at the
//                                        grid's edge and a bar rule; the frame is a sign, read against the pane like the rest. (Accent and AccentText, the cursor block, no longer touch it:
//                                        since OS-PATCENTER the cursor is always on the centre slot, never at the grid's top or bottom edge.)
const Pair kDecorations[] = {
    pair_of(Color::ChromeDim, Color::Pane), pair_of(Color::ChromeDim, Color::Sel), pair_of(Color::ChromeDim, Color::Empty),
    pair_of(Color::Empty, Color::Sel),      pair_of(Color::Empty, Color::TextDim), pair_of(Color::TextDim, Color::Sel),
    pair_of(Color::ChromeDim, Color::Text), pair_of(Color::ChromeDim, Color::Play),
    pair_of(Color::ChromeDim, Color::Rec),  pair_of(Color::Sel, Color::Rec),  pair_of(Color::Text, Color::Sel),
    pair_of(Color::Text, Color::Rec),       pair_of(Color::TextDim, Color::Rec),     pair_of(Color::Text, Color::Accent),
    pair_of(Color::Sel, Color::SelDim),     pair_of(Color::Rec, Color::SelDim),
};

struct Picture {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  template <typename Fn>
  explicit Picture(Fn draw) {
    CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, built_in(0));
    CHECK(r.begin(Color::Pane));
    draw(r);
    CHECK(r.end());
    CHECK(display.render_failures() == 0u);
  }
};

void collect_touching(const Picture& pic, std::set<Pair>& out) {
  const ui::Theme& th = built_in(0);
  std::map<std::uint32_t, Color> token;
  for (unsigned c = 0; c < ui::kColorCount; ++c) {
    const ui::Rgb& v = th.colour[c];
    const bool fresh = token.emplace((std::uint32_t{v.r} << 16) | (std::uint32_t{v.g} << 8) | v.b, static_cast<Color>(c)).second;
    CHECK_MESSAGE(fresh, "two Amiga tokens have one colour: a pixel could not name its token");
  }
  // Name every pixel's token once (runs of one colour are the rule), then compare neighbours.
  std::vector<std::uint8_t> names(800 * 480);
  const std::uint8_t* px = pic.raster.rgba();
  std::uint32_t last = 0xFFFFFFFFu;
  std::uint8_t last_name = 0;
  bool all_known = true;
  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::uint32_t c = (std::uint32_t{px[i * 4]} << 16) | (std::uint32_t{px[i * 4 + 1]} << 8) | px[i * 4 + 2];
    if (c != last) {
      const auto it = token.find(c);
      all_known = all_known && it != token.end();
      last_name = it != token.end() ? static_cast<std::uint8_t>(it->second) : 0;
      last = c;
    }
    names[i] = last_name;
  }
  CHECK(all_known);  // every pixel is a token's colour
  for (int y = 0; y < 480; ++y) {
    for (int x = 0; x < 800; ++x) {
      const std::uint8_t here = names[static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)];
      if (x + 1 < 800 && names[static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x) + 1] != here) {
        out.insert(pair_of(static_cast<Color>(here), static_cast<Color>(names[static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x) + 1])));
      }
      if (y + 1 < 480 && names[static_cast<std::size_t>(y + 1) * 800 + static_cast<std::size_t>(x)] != here) {
        out.insert(pair_of(static_cast<Color>(here), static_cast<Color>(names[static_cast<std::size_t>(y + 1) * 800 + static_cast<std::size_t>(x)])));
      }
    }
  }
}

}  // namespace

TEST_CASE("theme contrast: the table lists exactly the token pairs that touch in the pictures, apart from named decorations") {
  std::set<Pair> touching;
  // The pattern view: windows at both ends and in the middle, the cursor in each column and on a bar boundary.
  struct State {
    int row;
    int moves;
    int back;  // then this many steps back up: row 40 and 9 back is step 31, with the bar rule of step 32 just under it
  };
  const State states[] = {{0, 0, 0}, {16, 7, 0}, {22, 2, 0}, {40, 5 * 8 + 3, 0}, {63, 59, 0}, {31, 4, 0}, {15, 5 * 3, 0}, {40, 0, 9}};
  for (const State& st : states) {
    PatternView view;
    view.scroll_rows(st.row);
    for (int i = 0; i < st.moves; ++i) {
      view.move_right();
    }
    view.scroll_rows(-st.back);
    auto data = std::make_unique<PatternData>();
    app::fill_demo(*data);
    collect_touching(Picture([&](ui::Renderer& r) {
                       view.draw(r, *data, PatternView::Status{1, app::App::kSoftKeyLabels});
                     }),
                     touching);
  }
  // The style specimen of the gallery: the type scale, the tokens, the tiles and icons, and the components.
  const sim::Screen* style = nullptr;
  for (unsigned i = 0; i < sim::screen_count(); ++i) {
    style = std::string(sim::screens()[i].name) == "style" ? &sim::screens()[i] : style;
  }
  CHECK(style != nullptr);
  if (style != nullptr) {
    collect_touching(Picture([&](ui::Renderer& r) { style->states[0].draw(r, 0); }), touching);
  }

  // OS-009 P1a.1: the pattern over a document: the Edit mode with a block and the header marks, the block at the corners of the window, the Shift
  // layer, the dearest sweep's variants with every mark and the whole window selected, and the three choosers' strips. Drawn through the
  // simulator's states, so the test and the gallery see the same pictures.
  {
    const auto state = [](const char* screen_name, const char* state_name) -> const sim::ScreenState* {
      for (unsigned i = 0; i < sim::screen_count(); ++i) {
        if (std::string(sim::screens()[i].name) != screen_name) continue;
        for (unsigned k = 0; k < sim::screens()[i].state_count; ++k) {
          if (std::string(sim::screens()[i].states[k].name) == state_name) return &sim::screens()[i].states[k];
        }
      }
      return nullptr;
    };
    struct Pick {
      const char* screen;
      const char* name;
      unsigned variants[3];
    };
    const Pick picks[] = {{"pattern", "edit_on", {0, 0, 0}},           {"pattern", "shift_layer", {0, 0, 0}}, {"pattern", "rec", {0, 0, 0}},
                          {"pattern", "selection_worst", {0, 33, 301}}, {"pattern", "worst", {534 + 18, 534 + 30, 534 + 54}},
                          {"overlay", "pattern_chooser_typical", {0, 0, 0}}, {"overlay", "pattern_length", {0, 0, 0}},
                          {"overlay", "pattern_jump", {0, 0, 0}}};
    for (const Pick& pick : picks) {
      const sim::ScreenState* st = state(pick.screen, pick.name);
      CHECK_MESSAGE(st != nullptr, pick.name);
      if (st == nullptr) continue;
      for (const unsigned v : pick.variants) {
        collect_touching(Picture([&](ui::Renderer& r) { st->draw(r, v % st->variants); }), touching);
      }
    }
  }

  // OS-002b: the Song screen on both pages, and Learn, the Navigator (every cell chosen in turn) and the notification
  // strip (every severity) over the pattern and the song. The overlays' pictures are collected apart: a dialog's frame
  // (ChromeDim, one pixel) lies beside whatever its rectangle cuts through, which depends on where the window is.
  std::set<Pair> touching_overlay;
  {
    auto model = std::make_unique<app::Model>();
    app::fill_demo(model->pattern);
    app::fill_song_demo(model->song);
    const auto picture = [&](const app::ViewState& v, std::set<Pair>& into) {
      collect_touching(Picture([&](ui::Renderer& r) { app::render_scene(*model, v, r); }), into);
    };
    const auto song_at = [&](unsigned page, int pos) {
      app::ViewState v;
      v.screen = app::ScreenId::Song;
      v.page[app::screen_index(app::ScreenId::Song)] = static_cast<std::uint8_t>(page);
      (void)v.song.set(pos, model->song.length);
      return v;
    };
    const auto pattern_at = [&](int row, int moves) {
      app::ViewState v;
      v.pattern.scroll_rows(row);
      for (int i = 0; i < moves; ++i) {
        v.pattern.move_right();
      }
      return v;
    };
    for (const unsigned page : {0u, 1u}) {
      for (const int pos : {0, 5, 23}) {  // 5 is the play position: the marker on the cursor's row
        picture(song_at(page, pos), touching);
      }
    }
    const app::ViewState bases[] = {pattern_at(5, 6), pattern_at(46, 5 * 7 + 3), song_at(0, 3), song_at(1, 9)};  // (46: the bar rule of step 48 on slot 9, under the dialogs' frames, as step 32 was on slot 7 before OS-CENTER)
    for (std::size_t b = 0; b < sizeof(bases) / sizeof(bases[0]); ++b) {
      const app::ViewState& base = bases[b];
      app::ViewState learn = base;
      learn.overlay = app::OverlayId::Learn;
      picture(learn, touching_overlay);
      // Every cell chosen in turn over the first base (a disabled one, an enabled one, the first, the last); two over the others.
      for (int cell = 0; cell < static_cast<int>(app::kScreenCount); ++cell) {
        if (b != 0 && cell != 1 && cell != 6) {
          continue;
        }
        app::ViewState nav = base;
        nav.overlay = app::OverlayId::Navigator;
        nav.navigator = cell;
        picture(nav, touching_overlay);
      }
      for (const core::Severity severity : {core::Severity::Info, core::Severity::Warn, core::Severity::Error}) {
        if (b > 1) {
          continue;
        }
        app::ViewState note = base;
        (void)note.notify.post(severity, "SD CARD NOT FOUND", "sd", 2000);
        picture(note, touching_overlay);
      }
    }
  }

  std::set<Pair> table;
  for (unsigned i = 0; i < ui::kContrastPairCount; ++i) {
    table.insert(pair_of(ui::kContrastPairs[i].fg, ui::kContrastPairs[i].bg));
  }
  std::set<Pair> decoration(std::begin(kDecorations), std::end(kDecorations));
  const auto name = [](Pair p) {
    static const char* const names[] = {"Chrome", "ChromeText", "ChromeDim", "Pane",    "Text",   "TextDim",
                                        "Empty",  "Instr",      "Vol",       "Fx",      "Sel",    "SelText",
                                        "Accent", "AccentText", "Play",      "PlayText", "Rec",    "SelDim"};
    return std::string(names[p >> 5]) + " / " + names[p & 31];
  };
  const auto is_empty_frame = [](Pair p) {
    return (p >> 5) == static_cast<unsigned>(Color::Empty) || (p & 31) == static_cast<unsigned>(Color::Empty);
  };
  for (const Pair p : touching) {
    CHECK_MESSAGE((table.count(p) == 1 || decoration.count(p) == 1 || is_empty_frame(p)),
                  name(p) << " touch in a picture but are in neither the contrast table nor the decorations");
  }
  // An overlay's pictures: the same rule, and one more decoration. A dialog's outline is a one-pixel ChromeDim line, and
  // its rectangle cuts through the grid under it: where a glyph of the grid or the cursor block ends at the outline, that
  // ink lies beside the line (it is read against the pane, which the table covers, not against the outline).
  const Pair overlay_frame[] = {
      pair_of(Color::ChromeDim, Color::Instr),  pair_of(Color::ChromeDim, Color::Vol),
      pair_of(Color::ChromeDim, Color::TextDim), pair_of(Color::ChromeDim, Color::SelText),    pair_of(Color::ChromeDim, Color::Accent),
      pair_of(Color::ChromeDim, Color::AccentText),
  };
  const std::set<Pair> frame_beside(std::begin(overlay_frame), std::end(overlay_frame));
  for (const Pair p : touching_overlay) {
    CHECK_MESSAGE((table.count(p) == 1 || decoration.count(p) == 1 || is_empty_frame(p) || frame_beside.count(p) == 1),
                  name(p) << " touch in an overlay picture but are in neither the contrast table nor the decorations");
  }
  touching.insert(touching_overlay.begin(), touching_overlay.end());
  for (const Pair p : table) {
    CHECK_MESSAGE(touching.count(p) == 1, name(p) << " is in the contrast table but never touches in a picture");
  }
  for (const Pair p : decoration) {
    CHECK_MESSAGE(touching.count(p) == 1, name(p) << " is named a decoration but never touches in a picture");
  }
  for (const Pair p : frame_beside) {
    CHECK_MESSAGE(touching_overlay.count(p) == 1, name(p) << " is named an overlay frame decoration but never touches in a picture");
  }
  CHECK(touching.size() >= 20u);  // the pictures are not vacuous
}

// Rec mode (OS-REVIEW): Edit on a document while the sequencer plays shows the Rec frame, the REC badge and the Rec playhead bar; OS-RECSIGN: the frame and the badge show in STEP too
// (whenever the pattern can be edited), the bar is the take's alone; Play without Edit looks as it did. The gallery's "rec" state draws the three (variant 0 is Rec); the Amiga Rec red is no other token's, so its pixels are the Rec ink.
TEST_CASE("pattern view: the REC signs draw whenever the pattern can be edited, the playhead bar only while recording") {
  const sim::ScreenState* rec = nullptr;
  for (unsigned i = 0; i < sim::screen_count(); ++i) {
    if (std::string(sim::screens()[i].name) != "pattern") continue;
    for (unsigned k = 0; k < sim::screens()[i].state_count; ++k) {
      if (std::string(sim::screens()[i].states[k].name) == "rec") rec = &sim::screens()[i].states[k];
    }
  }
  CHECK(rec != nullptr);
  if (rec == nullptr) return;
  CHECK(rec->variants == 4u);  // (variant 0 is LIVE, variant 3 STEP playing: the chip says LIVE in the Rec ink / STEP in the Accent)
  const ui::Rgb red = built_in(0).rgb(Color::Rec);
  std::size_t counts[4] = {};
  bool corner[4] = {};  // the pane's top-left pixel is the frame
  for (unsigned v = 0; v < 4; ++v) {
    const Picture pic([&](ui::Renderer& r) { rec->draw(r, v); });
    const std::uint8_t* px = pic.raster.rgba();
    for (std::size_t i = 0; i < 800u * 480u; ++i) {
      counts[v] += px[i * 4] == red.r && px[i * 4 + 1] == red.g && px[i * 4 + 2] == red.b ? 1u : 0u;
    }
    const std::size_t at = (static_cast<std::size_t>(app::make_pattern_layout(built_in(0), 16, 5).grid.y) + 1u) * 800u;
    corner[v] = px[at * 4] == red.r && px[at * 4 + 1] == red.g && px[at * 4 + 2] == red.b;
  }
  // OS-RECSIGN: the signs (frame and badge) show whenever the pattern can be edited: LIVE (0), STEP stopped (1) and STEP playing (3); the Rec playhead bar is the take's, LIVE's only.
  CHECK(counts[0] > counts[3] + 15000u);  // LIVE adds the playhead bar (800 x 24) to the frame and the badge
  CHECK(counts[1] > 4000u);               // STEP stopped: the frame (3 x 2 x 1400 px) and the badge
  CHECK(counts[3] > 4000u);               // STEP playing: the same, no bar
  CHECK(counts[2] == 0u);                 // WATCH, playing: none
  CHECK(corner[0]);
  CHECK(corner[1]);
  CHECK_FALSE(corner[2]);
  CHECK(corner[3]);
  const Picture step_stopped([&](ui::Renderer& r) { rec->draw(r, 1); });
  const Picture step_playing([&](ui::Renderer& r) { rec->draw(r, 3); });
  const Picture watch([&](ui::Renderer& r) { rec->draw(r, 2); });
  std::size_t differing = 0;
  for (std::size_t i = 0; i < 800u * 480u * 4u; ++i) {
    differing += watch.raster.rgba()[i] != step_playing.raster.rgba()[i] ? 1u : 0u;
  }
  CHECK(differing > 0u);  // the chip and the signs: WATCH, STEP
  std::size_t chip = 0;
  for (std::size_t i = 0; i < 800u * 480u * 4u; ++i) {
    chip += step_stopped.raster.rgba()[i] != step_playing.raster.rgba()[i] ? 1u : 0u;
  }
  CHECK(chip > 0u);  // STEP stopped and playing differ by the PLAY badge and the bar, not by the signs
}

TEST_CASE("pattern view: an off, cut or fade step draws no instrument, whatever it holds, and a note does") {
  const auto with_instr = frame_with_first_step(note_step(50, 5));
  const auto without = frame_with_first_step(note_step(50, 0));
  CHECK(with_instr != without);  // for a note the instrument is drawn
  for (const std::uint8_t special : {model::kNoteOff, model::kNoteCut, model::kNoteFade}) {
    CHECK_MESSAGE(frame_with_first_step(note_step(special, 5)) == frame_with_first_step(note_step(special, 0)), "note byte " << unsigned{special});
  }
  // And the three are three different tiles.
  CHECK(frame_with_first_step(note_step(model::kNoteCut, 0)) != frame_with_first_step(note_step(model::kNoteFade, 0)));
  CHECK(frame_with_first_step(note_step(model::kNoteCut, 0)) != frame_with_first_step(note_step(model::kNoteOff, 0)));
}

TEST_CASE("pattern view: the second note handle is bound by a frame that shows C10..G10 and by no other") {
  const auto has_hi_bind = [](const std::vector<std::uint32_t>& f) {
    for (const std::uint32_t w : f) {
      if (w == 0x0500000Cu) {  // BITMAP_HANDLE 12
        return true;
      }
    }
    return false;
  };
  CHECK_FALSE(has_hi_bind(frame_with_first_step(note_step(120, 1))));                      // B-9: the last of the first handle
  CHECK_FALSE(has_hi_bind(frame_with_first_step(note_step(model::kNoteFade, 0))));          // FAD, CUT and OFF are in it too
  CHECK_FALSE(has_hi_bind(frame_with_first_step(note_step(model::kNoteOff, 0))));
  CHECK(has_hi_bind(frame_with_first_step(note_step(128, 1))));                             // G10
  CHECK(has_hi_bind(frame_with_first_step(note_step(124, 1))));                             // the first of the second handle's cells
  CHECK_FALSE(has_hi_bind(frame_with_first_step(note_step(121, 1))));                       // C10, C#10 and D10 still fit the first
  CHECK_FALSE(has_hi_bind(frame_with_first_step(note_step(123, 1))));
}

// ---- OS-MUTEVIS: the MUTE / SOLO tag inside the track's column ----------------------------------------------------------------------------------------------------------------

namespace {

struct PendingSink final : app::TrackSink {
  std::uint16_t pending = 0;
  bool playing() const noexcept override { return true; }
  bool mute_defer(unsigned, app::MuteMode, bool) noexcept override { return true; }
  bool mute_cancel(unsigned) noexcept override { return true; }
  std::uint16_t mute_pending() const noexcept override { return pending; }
  std::uint8_t note_count(unsigned) const noexcept override { return 0; }
};

// A document: pattern 1 of 64 steps with a note in every row of every one of the first three tracks; track 1 muted, track 2 soloed (flags set per test).
struct Doc {
  std::unique_ptr<app::Model> model = std::make_unique<app::Model>();
  app::MixBoard mix;
  PendingSink sink;
  Doc(std::uint8_t track1_flags, std::uint8_t track2_flags, std::uint32_t length = 64) {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, length);
    model::Step st = model::kEmptyStep;
    st.note = 61;
    st.instr = 1;
    for (std::uint32_t t = 0; t < 3; ++t) {
      for (std::uint32_t r = 0; r < length; ++r) {
        (void)p.set_steps(1, t, r, &st, 1);
      }
    }
    model::TrackSlot slot = *p.track(1);
    slot.flags = track1_flags;
    (void)p.set_track(1, slot);
    slot = *p.track(2);
    slot.flags = track2_flags;
    (void)p.set_track(2, slot);
    model->project = &p;
    mix.tracks = &sink;
    model->mix = &mix;
  }
};

// The picture of `view` over the document; `play_row` is the sequencer's row (-1 none), `blink` the lit phase of a pending tag.
struct DocShot {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  ui::Theme theme;
  DocShot(const PatternView& view, const Doc& doc, int play_row = -1, bool blink = false) : theme(built_in(0)) {
    CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, theme);
    CHECK(r.begin(Color::Pane));
    PatternView::Status status{1, app::App::kSoftKeyLabels};
    status.play_row = play_row;
    status.blink = blink;
    status.playing = play_row >= 0;
    view.draw(r, app::PatternSource(*doc.model, 1), status);
    CHECK(r.end());
    CHECK(display.render_failures() == 0u);
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

}  // namespace

TEST_CASE("pattern view: the tag takes the top slot, then the bottom, then the next free one, and is never on the cursor's row or the play bar (OS-MUTEVIS)") {
  using V = PatternView;
  CHECK(V::tag_slot(7, -1) == 0);    // the cursor is mid-window: the top slot
  CHECK(V::tag_slot(15, -1) == 0);
  CHECK(V::tag_slot(0, -1) == 15);   // the cursor in the top slot: the bottom one
  CHECK(V::tag_slot(7, 0) == 15);    // the play bar in it
  CHECK(V::tag_slot(0, 15) == 1);    // both taken: the next free
  CHECK(V::tag_slot(15, 0) == 1);
  CHECK(V::tag_slot(1, 0) == 15);
  CHECK(V::tag_slot(-5, -1) == 0);   // a cursor outside the window takes nothing
  for (int cursor = -2; cursor < 20; ++cursor) {
    for (int play = -2; play < 20; ++play) {
      const int slot = V::tag_slot(cursor, play);
      CHECK_MESSAGE((slot >= 0 && slot < V::kVisibleRows && slot != cursor && slot != play), cursor << " " << play);
    }
  }
}

TEST_CASE("pattern view: a muted track's tag is an inverted chip in the grid's top slot, fixed while the pattern scrolls, over the track's own rows only (OS-MUTEVIS, OS-PATCENTER)") {
  const Doc doc(model::kTrackMute, 0);
  const PatternLayout l = app::make_pattern_layout(built_in(0), 16, 5);
  const auto tag_pixel = [&](const DocShot& s, int track, int slot) { return s.pixel(l.track_x(track) + 4, l.row_y(slot) + 3); };  // the chip's left padding, inside its column
  PatternView view;
  const PatternView::Dims d{64, 12};
  (void)view.scroll_rows(20, d);  // mid-window: the cursor on the centre slot
  {
    const DocShot s(view, doc);
    CHECK(tag_pixel(s, 1, 0) == s.rgb(Color::Text));   // MUTE, top slot, in track 2's column
    CHECK(tag_pixel(s, 0, 0) != s.rgb(Color::Text));   // not in the others
    CHECK(tag_pixel(s, 2, 0) != s.rgb(Color::Text));
    CHECK(tag_pixel(s, 1, 5) != s.rgb(Color::Text));   // and no other row of the column
    CHECK(tag_pixel(s, 1, 15) != s.rgb(Color::Text));
    // The row's fields of the muted track are under the tag (not drawn); the next row's are there, dim.
    CHECK(s.count(Color::TextDim, l.track_x(1) + l.field_x[0], l.row_y(0), l.field_w[0], l.row_pitch) == 0);
    CHECK(s.count(Color::TextDim, l.track_x(1) + l.field_x[0], l.row_y(1), l.field_w[0], l.row_pitch) > 5);
    // The tag is a bright chip with the letters knocked out in the pane's colour: Pane ink inside it, and a bright field around.
    CHECK(s.count(Color::Pane, l.track_x(1) + 4, l.row_y(0), l.track_w - 8, l.row_pitch) > 20);
    CHECK(s.count(Color::Text, l.track_x(1) + 4, l.row_y(0), l.track_w - 8, l.row_pitch) > 500);
  }
  (void)view.scroll_rows(15, d);  // scrolled: the tag does not move
  {
    const DocShot s(view, doc);
    CHECK(view.top_row() == 28);
    CHECK(tag_pixel(s, 1, 0) == s.rgb(Color::Text));
    CHECK(tag_pixel(s, 1, 7) != s.rgb(Color::Text));
  }
  (void)view.jump_row(63, d);  // the last row: the cursor on the centre slot (OS-PATCENTER), the slots below it blank, the tag still on top
  {
    const DocShot s(view, doc);
    CHECK(tag_pixel(s, 1, 0) == s.rgb(Color::Text));
  }
  (void)view.jump_row(0, d);   // the first row: the cursor on the centre slot, the seven slots above it blank; the tag stays on the top slot, over a blank one, and the first row shows its data
  {
    const DocShot s(view, doc);
    CHECK(tag_pixel(s, 1, 0) == s.rgb(Color::Text));
    CHECK(tag_pixel(s, 1, PatternView::kCentreRow + 1) != s.rgb(Color::Text));
    CHECK(s.count(Color::SelText, l.track_x(1) + l.field_x[0], l.row_y(PatternView::kCentreRow), l.field_w[0], l.row_pitch) > 5);  // (the cursor's row: its fields are SelText)
  }
  (void)view.jump_row(30, d);  // the play bar in the top slot (row 23 is slot 0 of the window of 23..38): the tag goes to the bottom
  {
    const DocShot s(view, doc, 23);
    CHECK(tag_pixel(s, 1, 15) == s.rgb(Color::Text));
    CHECK(tag_pixel(s, 1, 0) != s.rgb(Color::Text));
  }
  // (The cursor is never in the top slot now, so the tag moves for the play bar only; the choice for every pair of slots is tag_slot's test above.)
}

// OS-PATCENTER (maintainer, 2026-10-17; new Community OS behaviour): the cursor's row is on the centre slot at both ends of the pattern; the slots past the ends are blank (no number, no field), at every zoom level.
namespace {
// The grid's slot `slot` is blank: the row-number cell and the first field of track 0 hold the pane and nothing else. Tracks 0..2 of a Doc hold a note on every step, so a live slot has ink in both.
bool slot_blank(const DocShot& s, const PatternLayout& l, int slot) {
  const int field_x = l.track_x(0) + l.field_x[0];
  return s.count(Color::Pane, 0, l.row_y(slot), l.track_x(0), l.row_pitch) == l.track_x(0) * l.row_pitch &&
         s.count(Color::Pane, field_x, l.row_y(slot), l.field_w[0], l.row_pitch) == l.field_w[0] * l.row_pitch;
}
}  // namespace

TEST_CASE("pattern view: the cursor's row is on the centre slot at the first and the last row, the slots past the ends are blank, at every zoom level (OS-PATCENTER)") {
  for (const std::uint32_t length : {64u, 40u, 16u, 8u, 1u}) {
    const Doc doc(0, 0, length);
    const PatternView::Dims d{static_cast<int>(length), 12};
    for (int zoom = 0; zoom < PatternView::kZoomCount; ++zoom) {
      for (const int row : {0, 1, 6, 7, 8, static_cast<int>(length) / 2, static_cast<int>(length) - 9, static_cast<int>(length) - 2, static_cast<int>(length) - 1}) {
        if (row < 0 || row >= static_cast<int>(length)) {
          continue;
        }
        PatternView view;
        (void)view.set_zoom(zoom, d);
        (void)view.jump_row(row, d);
        const DocShot s(view, doc);
        const PatternLayout l = app::make_pattern_layout(built_in(0), PatternView::kVisibleRows, view.visible_tracks(), zoom);
        for (int slot = 0; slot < PatternView::kVisibleRows; ++slot) {
          const int shown = row - PatternView::kCentreRow + slot;
          const bool live = shown >= 0 && shown < static_cast<int>(length);
          CHECK_MESSAGE(slot_blank(s, l, slot) == !live, "length " << length << " zoom " << zoom << " row " << row << " slot " << slot);
        }
        // The cursor's row is the selected one: the full-width bar is on the centre slot, and only there.
        for (int slot = 0; slot < PatternView::kVisibleRows; ++slot) {
          const bool bar = s.pixel(790, l.row_y(slot) + l.row_pitch / 2) == s.rgb(Color::Sel);
          CHECK_MESSAGE(bar == (slot == PatternView::kCentreRow), "length " << length << " zoom " << zoom << " row " << row << " slot " << slot);
        }
      }
    }
  }
}

TEST_CASE("pattern view: a pattern of 128 steps, the longest, draws the window on the cursor, the bar rules only over steps that exist, and nothing past step 128 (OS-PATCENTER)") {
  const Doc doc(0, 0, 128);
  const PatternView::Dims d{128, 12};
  const PatternLayout l = app::make_pattern_layout(built_in(0), PatternView::kVisibleRows, PatternView::kVisibleTracks);
  for (const int row : {0, 9, 63, 64, 112, 120, 121, 127}) {
    PatternView view;
    (void)view.jump_row(row, d);
    CHECK(view.top_row() == row - PatternView::kCentreRow);
    const DocShot s(view, doc);
    for (int slot = 0; slot < PatternView::kVisibleRows; ++slot) {
      const int shown = row - PatternView::kCentreRow + slot;
      const bool live = shown >= 0 && shown < 128;
      CHECK_MESSAGE(slot_blank(s, l, slot) == !live, "row " << row << " slot " << slot);
      if (slot == PatternView::kCentreRow) {
        CHECK_MESSAGE(s.pixel(790, l.row_y(slot) + l.row_pitch / 2) == s.rgb(Color::Sel), "row " << row);  // the cursor's row: the one bar, no rule over it
        continue;
      }
      // A rule every 16 steps, over the steps that exist: step 128 would be one (128 % 16 == 0) and is past the end.
      const bool rule = live && shown > 0 && shown % 16 == 0;
      CHECK_MESSAGE((s.pixel(10, l.row_y(slot)) == s.rgb(Color::TextDim)) == rule, "row " << row << " slot " << slot);
    }
  }
}

TEST_CASE("pattern view: the play bar is drawn on the centre slot where the cursor follows it at both ends, and not at all in a blank slot (OS-PATCENTER)") {
  const Doc doc(0, 0);
  const PatternView::Dims d{64, 12};
  const PatternLayout l = app::make_pattern_layout(built_in(0), PatternView::kVisibleRows, PatternView::kVisibleTracks);
  const auto bar_at = [&](const DocShot& s, int slot) { return s.pixel(790, l.row_y(slot) + l.row_pitch / 2) == s.rgb(Color::Play); };
  for (const int row : {0, 63}) {  // the view follows the playhead: the cursor is on its row (OS-009c)
    PatternView view;
    (void)view.jump_row(row, d);
    const DocShot s(view, doc, row);
    CHECK(bar_at(s, PatternView::kCentreRow));
    for (int slot = 0; slot < PatternView::kVisibleRows; ++slot) {
      CHECK_MESSAGE((slot == PatternView::kCentreRow || !bar_at(s, slot)), "row " << row << " slot " << slot);
    }
  }
  // A free cursor (STEP) while the playhead moves: its bar is in a slot only while that row exists; past the end (a pattern that is shorter than the sequencer thinks) it is not drawn.
  PatternView view;
  (void)view.jump_row(63, d);
  const DocShot past(view, doc, 64);
  for (int slot = 0; slot < PatternView::kVisibleRows; ++slot) {
    CHECK_MESSAGE(!bar_at(past, slot), "slot " << slot);
  }
  const DocShot before(view, doc, 60);
  CHECK(bar_at(before, PatternView::kCentreRow - 3));
  PatternView first;
  const DocShot first_shot(first, doc, 3);  // the cursor on row 0: the bar of row 3 is three slots under it
  CHECK(bar_at(first_shot, PatternView::kCentreRow + 3));
}

TEST_CASE("pattern view: a soloed track has the tag too, a pending quantised mute blinks it, and a plain track has none (OS-MUTEVIS)") {
  const PatternLayout l = app::make_pattern_layout(built_in(0), 16, 5);
  PatternView view;
  const PatternView::Dims d{64, 12};
  (void)view.scroll_rows(20, d);
  // Track 3 (index 2) soloed: SOLO in its column.
  {
    const Doc doc(0, model::kTrackSolo);
    const DocShot s(view, doc);
    CHECK(s.pixel(l.track_x(2) + 4, l.row_y(0) + 3) == s.rgb(Color::Text));
    CHECK(s.pixel(l.track_x(1) + 4, l.row_y(0) + 3) != s.rgb(Color::Text));
  }
  // No flags: no tag anywhere.
  {
    const Doc doc(0, 0);
    const DocShot s(view, doc);
    for (int track = 0; track < 5; ++track) {
      CHECK(s.pixel(l.track_x(track) + 4, l.row_y(0) + 3) != s.rgb(Color::Text));
    }
  }
  // A pending mute of track 2 (index 1): the flag is set already; the tag is there in the lit phase and gone in the other.
  {
    Doc doc(model::kTrackMute, 0);
    doc.sink.pending = 1u << 1;
    const DocShot lit(view, doc, -1, true);
    CHECK(lit.pixel(l.track_x(1) + 4, l.row_y(0) + 3) == lit.rgb(Color::Text));
    const DocShot dark(view, doc, -1, false);
    CHECK(dark.pixel(l.track_x(1) + 4, l.row_y(0) + 3) != dark.rgb(Color::Text));
    CHECK(dark.count(Color::TextDim, l.track_x(1) + l.field_x[0], l.row_y(0), l.field_w[0], l.row_pitch) > 5);  // the data shows between the blinks
  }
  // A pending UNMUTE (the flag is already off, the track still sounds muted until the boundary): it blinks as well.
  {
    Doc doc(0, 0);
    doc.sink.pending = 1u << 1;
    const DocShot lit(view, doc, -1, true);
    CHECK(lit.pixel(l.track_x(1) + 4, l.row_y(0) + 3) == lit.rgb(Color::Text));
    const DocShot dark(view, doc, -1, false);
    CHECK(dark.pixel(l.track_x(1) + 4, l.row_y(0) + 3) != dark.rgb(Color::Text));
  }
}
