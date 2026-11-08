// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The modal dialog the Learn and Navigator overlays are drawn in (private to app/src/screens): chrome material, a
// one-pixel ChromeDim frame, an inverted title bar. A ring of Chrome lies between the frame and the title bar so that the
// frame meets the title only through a token pair of the contrast table. Everything is derived from the theme's metrics.

#include <o3os/ui/components.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

namespace o3os::app {

struct Dialog {
  ui::Rect outer;  // the frame included: what the overlay declares as covered
  ui::Rect title;  // the inverted bar
  ui::Rect body;   // under the title, the width of the title bar
};

// A dialog `w` wide with `body_h` pixels of content, centred horizontally and in the data pane vertically.
Dialog make_dialog(const ui::Theme& theme, int w, int body_h) noexcept;

// OS-PTOOLS2 S3p: the right-hand panel (frame.h's kRightPanelX..kScreenW) as a dialog: docked to the right edge, the full height of the
// header and the data pane, with the title bar at its top and the body under it. What the overlay declares as covered is its `outer`.
Dialog make_panel_dialog(const ui::Theme& theme) noexcept;

// Frame, interior and title bar with `title` in the Data face (inverted).
void draw_dialog(ui::Renderer& r, const Dialog& dialog, const char* title) noexcept;

// A key-cap and its label, as in the soft-key row: an inverted box with `key` ("F8") and `label` beside it, vertically
// centred in the row [y, y + h). Returns the width drawn.
int draw_key_hint(ui::Renderer& r, int x, int y, int h, const char* key, const char* label) noexcept;

// OS-PTOOLS2 S6: `text` in the Label face on at most `lines` lines of `room` cells (at most 40), one row pitch apart from `y`, each line cut at its last space; what does not fit is dropped.
void draw_wrapped(ui::Renderer& r, int x, int y, int pitch, std::size_t room, const char* text, ui::Color ink, unsigned lines) noexcept;

}  // namespace o3os::app
