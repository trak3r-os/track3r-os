// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pattern screen's layout (ADR 0010 section 8): named rectangles and offsets derived from the theme's metrics and
// the faces' geometry, so the screen holds no pixel literals of its own and a theme that changes a metric moves the
// screen. A screen renders from (model, layout, theme); actions never see any of this.
//
// Two chrome bands and the data pane between them, top to bottom (Amiga metrics, 800 x 480):
//
//   strip    32   status strip: Display face values, transport badge, Label captions   } chrome
//   header   26   track headers: type badge, name, 4 px meter (6 with its frame), underline }
//   grid    384   16 rows at a 24 px pitch, the Data face's 20 px cell inside it          data pane
//   info     16   hint line                                                              } chrome
//   keys     22   F1..F8 key-caps                                                        }
//
// A track column is `track_w` wide and starts with a one-pixel separator. Its five fields are centred in it, one
// `gap` apart: note tile (3 Data cells), instrument and volume pairs (2), FX1 and FX2 (a type glyph and a pair, 3).
// Everything is a whole number of Data cells plus gaps, which is what lets 5 tracks fit 800 px at 10 px per cell.

#include <o3os/ui/components.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

namespace o3os::app {

inline constexpr int kLayoutFields = 5;  // note, instrument, volume, FX1, FX2 (Column in pattern_data.h)

// OS-ZOOM (docs/specs/PAT-NAV.md; new Community OS behaviour): the three zoom levels of the Pattern grid. The track column is (800 - index_w) / tracks wide (154, 96 and 64 px); the fields shown are the first
// `fields` of the five, in the face's own cell size (Data 10x20, Label 8x16, Micro 6x12; the tiles of each are composed at boot, ui/assets.h). Level 0 is the layout of OS-003b, unchanged.
struct PatternZoom {
  int tracks;
  int fields;
  ui::Face face;
};
inline constexpr int kZoomLevels = 3;
inline constexpr PatternZoom kZoomTable[kZoomLevels] = {
    {5, 5, ui::Face::Data},    // every field
    {8, 3, ui::Face::Label},   // Note, Instr, Vol
    {12, 1, ui::Face::Micro},  // Note
};
constexpr const PatternZoom& pattern_zoom(int level) noexcept {
  return kZoomTable[level >= 0 && level < kZoomLevels ? level : 0];
}

// The narrowest track column a zoom level's fields fit in, the one-pixel separator included: the fields' tiles plus their gaps, plus a
// border each side. `make_pattern_layout` fits this in `track_w`; `pattern_tracks_fit` divides a pane's width by it.
constexpr int pattern_track_min_w(const ui::Theme& theme, int zoom) noexcept {
  const ui::Metrics& m = theme.metrics;
  const PatternZoom& z = pattern_zoom(zoom);
  const ui::FontGeometry face = theme.geometry(z.face);
  const int cells[kLayoutFields] = {3, 2, 2, 3, 3};
  int content = 0;
  for (int i = 0; i < z.fields; ++i) {
    content += cells[i] * face.cell_w + (i > 0 ? m.gap : 0);
  }
  return content + 2 * m.border;
}

// OS-PTOOLS2 S3p: the most track columns of `tracks` that fit a pane `width` wide at zoom level `zoom` (at least one). While the right-hand
// panel is up the grid is drawn 500 px wide, so the 5-track zoom shows three tracks (the assumption of spec section 5).
constexpr int pattern_tracks_fit(const ui::Theme& theme, int tracks, int zoom, int width) noexcept {
  const int n = (width - theme.metrics.index_w) / pattern_track_min_w(theme, zoom);
  return n < 1 ? 1 : (n > tracks ? tracks : n);
}

struct PatternLayout {
  ui::Rect strip;
  ui::Rect header;
  ui::Rect grid;
  ui::Rect info;
  ui::Rect keys;

  int rows;           // visible rows
  int tracks;         // visible track columns
  int zoom;           // the zoom level (0 = 5 tracks, 1 = 8, 2 = 12)
  int fields;         // the fields shown, the first `fields` of the five
  ui::Face face;      // the face of the fields (and so of their tiles)
  int row_pitch;
  int index_x;        // left edge of the two-digit row number
  int track0_x;       // left edge of the first track column (the row-number column is to its left)
  int track_w;

  int field_x[kLayoutFields];  // from the left edge of a track column
  int field_w[kLayoutFields];  // pixels the field's tiles cover
  int cell_dy;                 // top of a field's cell (the level's face) below the top of its row: capitals centred in the pitch
  int index_dy;                // the same for the row number, which is always a Data face tile
  int cursor_pad_x;            // the cursor block extends this far beyond its field...
  int cursor_dy;               // ...starts this far below the top of its row...
  int cursor_h;                // ...and is this high: the capitals of the row and a margin above and below
  int cursor_outline;          // the block has an outline this thick (AccentText) all round: the pixels that set it off the row

  // The status strip: the transport badge's height and top, and the group dividers' height and top.
  int strip_badge_y;
  int strip_badge_h;
  int divider_y;
  int divider_h;

  // Track header: the badge, name and meter of slot s. `meter_h` is the whole meter, frame included.
  int badge_size;
  int badge_y;
  int meter_y;
  int meter_h;
  int underline_y;

  // False when the fields do not fit a track column or the bands do not add up to the screen: a metric is wrong.
  bool fits;

  constexpr int track_x(int slot) const noexcept { return track0_x + slot * track_w; }
  constexpr int row_y(int slot) const noexcept { return grid.y + slot * row_pitch; }
};

// `rows` x `tracks` is the visible window (PatternView::kVisibleRows, kVisibleTracks); `zoom` the level of the Pattern grid (kZoomTable) whose face and fields the track column holds: the other screens pass the default.
// `width` is the pane's width the grid is drawn in: the full screen by default, the 500 px left of the right-hand panel while that panel is
// up (OS-PTOOLS2 S3p); the chrome bands stay full width, only the grid (and so the track columns) narrow. Pass `tracks` from
// pattern_tracks_fit() when `width` is narrower, so the fields still fit a column.
constexpr PatternLayout make_pattern_layout(const ui::Theme& theme, int rows, int tracks, int zoom = 0, int width = ui::kScreenW) noexcept {
  const ui::Metrics& m = theme.metrics;
  const ui::FontGeometry data = theme.geometry(ui::Face::Data);
  const ui::FontGeometry label = theme.geometry(ui::Face::Label);
  const PatternZoom& z = pattern_zoom(zoom);
  const ui::FontGeometry face = theme.geometry(z.face);  // the fields' cells; the row numbers and the cursor's height stay the Data face's
  PatternLayout l{};

  l.rows = rows;
  l.tracks = tracks;
  l.zoom = zoom >= 0 && zoom < kZoomLevels ? zoom : 0;
  l.fields = z.fields;
  l.face = z.face;
  l.row_pitch = m.row_pitch;
  l.strip = {0, 0, ui::kScreenW, m.strip_h};
  l.header = {0, l.strip.y + l.strip.h, ui::kScreenW, m.header_h};
  l.grid = {0, l.header.y + l.header.h, width, rows * m.row_pitch};
  l.info = {0, l.grid.y + l.grid.h, ui::kScreenW, m.info_h};
  l.keys = {0, l.info.y + l.info.h, ui::kScreenW, m.keys_h};

  l.track0_x = m.index_w;
  l.track_w = (width - m.index_w) / tracks;
  l.index_x = (m.index_w - 2 * data.cell_w) / 2;

  // Field tiles, in cells of the level's face: note 3, instrument 2, volume 2, FX1 3, FX2 3. The fields past `fields` are not shown: they take no room.
  const int cells[kLayoutFields] = {3, 2, 2, 3, 3};
  int content = 0;
  for (int i = 0; i < z.fields; ++i) {
    l.field_w[i] = cells[i] * face.cell_w;
    content += l.field_w[i] + (i > 0 ? m.gap : 0);
  }
  int x = (l.track_w - content) / 2;
  for (int i = 0; i < z.fields; ++i) {
    l.field_x[i] = x;
    x += l.field_w[i] + m.gap;
  }

  // Capitals centred in the pitch: the cell sits `cell_dy` below the top of the row (ui::caps_y, without a theme).
  l.cell_dy = (m.row_pitch - face.cap_h) / 2 - (face.ascent - face.cap_h);
  l.index_dy = (m.row_pitch - data.cap_h) / 2 - (data.ascent - data.cap_h);
  l.cursor_pad_x = m.gap / 2;
  const int margin_y = m.pad - 1;  // air above and below the capitals inside the cursor block
  l.cursor_h = data.cap_h + 2 * margin_y;  // the Data face's block at every level, centred on the capitals of the level's face
  l.cursor_dy = l.cell_dy + (face.ascent - face.cap_h) - (l.cursor_h - face.cap_h) / 2;
  l.cursor_outline = m.border;

  // The strip's transport badge: the strip less a pad and a border above and below. Dividers: a pad's worth of air
  // twice over, above and below.
  l.strip_badge_h = m.strip_h - 2 * (m.pad + m.border);
  l.strip_badge_y = l.strip.y + m.pad + m.border;
  l.divider_y = l.strip.y + 2 * m.pad;
  l.divider_h = m.strip_h - 4 * m.pad;

  // The header, from its bottom up: the selection underline (two borders), a border of air, the meter (its level
  // bar and a border of frame above and below), a border of air, and the badge row in what is left.
  l.underline_y = l.header.y + l.header.h - 2 * m.border;
  l.meter_h = m.meter_h + 2 * m.border;
  l.meter_y = l.underline_y - m.border - l.meter_h;
  l.badge_size = label.cap_h + 2 * (m.pad / 2);
  l.badge_y = l.header.y + (l.meter_y - m.border - l.header.y - l.badge_size) / 2;

  l.fits = content + 2 * m.border <= l.track_w &&  // the fields fit a column, with the separator
           l.keys.y + l.keys.h == ui::kScreenH &&  // and the bands fill the panel exactly
           l.cell_dy >= 0 && l.cell_dy + face.cell_h <= m.row_pitch && l.index_dy >= 0 && l.index_dy + data.cell_h <= m.row_pitch;
  return l;
}

}  // namespace o3os::app
