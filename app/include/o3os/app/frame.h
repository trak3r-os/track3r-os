// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The bands every full screen shares, from the theme's metrics (docs/ui/style-guide.md section 2): the status strip and
// the header on top, the data pane, the info line and the F-key row below. The Song screen is laid out on them, and the
// overlays are placed against them (the notification strip is exactly the info line, a dialog is centred in the pane).
// The pattern layout (pattern_layout.h) derives the same bands; a test holds the two equal in every theme.

#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

namespace o3os::app {

struct Frame {
  ui::Rect strip;
  ui::Rect header;
  ui::Rect pane;
  ui::Rect info;
  ui::Rect keys;
  int row_pitch;
  // The status strip's transport badge (the strip less a pad and a border above and below) and its group dividers (a
  // pad's worth of air twice over, above and below).
  int badge_y;
  int badge_h;
  int divider_y;
  int divider_h;
};

// `rows` rows of the theme's pitch in the pane (16 on the 800 x 480 panel).
constexpr Frame make_frame(const ui::Theme& theme, int rows) noexcept {
  const ui::Metrics& m = theme.metrics;
  Frame f{};
  f.row_pitch = m.row_pitch;
  f.strip = {0, 0, ui::kScreenW, m.strip_h};
  f.header = {0, f.strip.y + f.strip.h, ui::kScreenW, m.header_h};
  f.pane = {0, f.header.y + f.header.h, ui::kScreenW, rows * m.row_pitch};
  f.info = {0, f.pane.y + f.pane.h, ui::kScreenW, m.info_h};
  f.keys = {0, f.info.y + f.info.h, ui::kScreenW, m.keys_h};
  f.badge_h = m.strip_h - 2 * (m.pad + m.border);
  f.badge_y = f.strip.y + m.pad + m.border;
  f.divider_y = f.strip.y + 2 * m.pad;
  f.divider_h = m.strip_h - 4 * m.pad;
  return f;
}

inline constexpr int kFrameRows = 16;  // rows of the pane on the 800 x 480 panel

// OS-PTOOLS2 S3p (docs/specs/OS-PTOOLS2.md 3.1 point 3, mock-ups A and B): the right-hand panel the F7 verb menu and the Pattern TOOLS dialog
// render in — x 500..800, the full height of the header and the data pane, docked to the right edge. The strip, the info line and the key row
// stay full width, and the grid is drawn in the 500 px left of it while the panel is up (PatternScreen::render passes the width to
// PatternView::draw), so three tracks stay visible at the 5-track zoom and the block and the cursor stay in view.
inline constexpr int kRightPanelX = 500;
inline constexpr int kRightPanelW = ui::kScreenW - kRightPanelX;  // 300

// True when the rectangle (x, y, w, h) lies entirely inside `outer`: the test of "the base screen skips what the overlay
// covers", made for every field of a grid, so it is four compares inlined at the call (no Rect to build, no call). An empty
// `outer` (no overlay) contains nothing, and so does an empty item.
[[gnu::always_inline]] inline constexpr bool covers(const ui::Rect& outer, int x, int y, int w, int h) noexcept {
  return outer.w > 0 && outer.h > 0 && w > 0 && h > 0 && x >= outer.x && y >= outer.y && x + w <= outer.x + outer.w &&
         y + h <= outer.y + outer.h;
}

// True when the rectangle (x, y, w, h) shares a pixel with `outer`. A base screen whose text runs wider than the dialog over it leaves that text out with this instead of `covers`, so
// that nothing depends on the dialog's fill hiding it: the unit's captures show every RECTS fill at about 0.55 of its colour (OS-SIMKEY, sim/README.md) and the text shows through
// (OS-BUG-OGOVERLAY). An empty `outer` (no overlay) touches nothing.
[[gnu::always_inline]] inline constexpr bool touches(const ui::Rect& outer, int x, int y, int w, int h) noexcept {
  return outer.w > 0 && outer.h > 0 && w > 0 && h > 0 && x < outer.x + outer.w && x + w > outer.x && y < outer.y + outer.h && y + h > outer.y;
}

constexpr bool rect_inside(const ui::Rect& inner, const ui::Rect& outer) noexcept {
  return covers(outer, inner.x, inner.y, inner.w, inner.h);
}

}  // namespace o3os::app
