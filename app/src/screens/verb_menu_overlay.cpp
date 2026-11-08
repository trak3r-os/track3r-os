// SPDX-License-Identifier: GPL-3.0-or-later
// The verb menu of Pattern's F7 (OS-PTOOLS2 S3, docs/specs/OS-PTOOLS2.md mock-up A): the right-hand panel (x 500..800, frame.h's
// kRightPanelX..kScreenW) beside the narrowed grid, two columns of eight rows, the pads 1-16 pick a verb directly (pad n = entry n-1 of
// app/verb_menu.h: the left column is pads 1-8, the right one 9-16), the cursor is marked with ">" and a verb not built yet is drawn dim.
// The labels are at most kVerbLabelMax cells (verb_menu.h, a static_assert), so none is clipped; the clip below is a guard for a narrower theme.
#include <o3os/app/frame.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/text_format.h>
#include <o3os/app/tools_list.h>
#include <o3os/app/verb_menu.h>
#include <o3os/core/fixed_string.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

// The right-hand panel as a dialog: full height of the header and the pane, docked to the right edge (dialog.cpp).
Dialog verb_menu_panel(const ui::Theme& t) noexcept { return make_panel_dialog(t); }

// The APPS page (OS-PTOOLS2 S6b, the TOOLS list of OS-H3A in the panel): the tool apps only (OS-TOOLSDEDUP: the OS's own operations are the grid's verbs, EVENTS is F6); the cursor row is marked with ">", a quarantined app is dim. Below the
// rows a line of help, or the refusal the last F1 gave (wrapped on two lines), then the keys.
void render_apps_page(const Model& model, const ViewState& view, ui::Renderer& r, const Dialog& d) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const int x = d.body.x + 2 * m.pad;
  const int right = d.body.x + d.body.w - 2 * m.pad;
  const int pitch = m.row_pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  const int total = static_cast<int>(tools_count(model));
  const int cursor = view.modules.tools < total ? view.modules.tools : 0;
  const int first = cursor < kToolsRows ? 0 : cursor - kToolsRows + 1;
  if (total == 0) {  // no app: one dim line in the first row
    r.text(x + 2 * cell, ui::caps_y(t, Face::Label, d.body.y, pitch), model.modules == nullptr ? "NO MODULE SYSTEM" : "NO TOOL APPS ON THE CARD", Color::ChromeDim, Face::Label);
  }
  const int room = (right - x) / cell - 2;  // cells for a label after the cursor mark
  for (int k = 0; k < kToolsRows && first + k < total; ++k) {
    ToolItem item;
    if (!tools_item(model, static_cast<unsigned>(first + k), item)) {
      continue;
    }
    const int y = d.body.y + k * pitch;
    const Color ink = item.bad ? Color::ChromeDim : Color::ChromeText;
    if (first + k == cursor) {
      r.text(x, ui::caps_y(t, Face::Data, y, pitch), ">", Color::ChromeText, Face::Data);
    }
    core::FixedString<24> label;
    (void)append_clipped(label, item.label, static_cast<std::size_t>(room > 0 ? room : 0));
    r.text(x + 2 * cell, ui::caps_y(t, Face::Data, y, pitch), label.c_str(), ink, Face::Data);
  }
  const bool refused = view.modules.tools_msg != nullptr && view.modules.tools_msg[0] != '\0';
  const int foot = d.body.y + kToolsRows * pitch;
  if (refused) {
    draw_wrapped(r, x, foot, pitch, static_cast<std::size_t>((right - x) / t.geometry(Face::Label).cell_w), view.modules.tools_msg, Color::ChromeText, 2);
  } else {
    r.text(x, ui::caps_y(t, Face::Label, foot, pitch), "Run a tool on the block or cursor.", Color::ChromeDim, Face::Label);
  }
  (void)draw_prompt_hints(r, x, foot + 2 * pitch, pitch, prompt_in_force(model, view), 2);
}

}  // namespace

Rect VerbMenuOverlay::rect(const ui::Theme& theme) const noexcept { return verb_menu_panel(theme).outer; }

int VerbMenuOverlay::count(const Model& model, const ViewState& view) const noexcept {
  return view.verb_menu.apps ? static_cast<int>(tools_count(model)) : static_cast<int>(kVerbCount);
}

bool VerbMenuOverlay::set_position(const Model& model, ViewState& view, int position) const noexcept {
  if (view.verb_menu.apps) {  // the APPS page: a list, the cursor is the TOOLS row (ModulesView::tools)
    const int last = count(model, view) - 1;
    const int at = position < 0 ? 0 : (position > last ? (last < 0 ? 0 : last) : position);
    const bool moved = at != view.modules.tools;
    view.modules.tools = static_cast<std::uint8_t>(at);
    return moved;
  }
  const int to = position < 0 ? 0 : (position >= static_cast<int>(kVerbCount) ? static_cast<int>(kVerbCount) - 1 : position);
  const bool changed = to != view.verb_menu.cursor;
  view.verb_menu.cursor = static_cast<std::uint8_t>(to);
  return changed;
}

// The 2x8 grid: entry i is row i % 8, column i / 8 (pad 1..8 the left column, 9..16 the right one); left / right a column, up / down a row.
bool VerbMenuOverlay::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  if (view.verb_menu.apps) {  // a list: Up / Down move a row (no wrap, as the list always did), Left / Right do nothing
    return dy != 0 && set_position(model, view, view.modules.tools + dy);
  }
  const int cur = view.verb_menu.cursor;
  const int row = cur % static_cast<int>(kVerbRows);
  const int col = cur / static_cast<int>(kVerbRows);
  int nrow = row + dy;
  int ncol = col + dx;
  if (nrow < 0) {
    nrow = static_cast<int>(kVerbRows) - 1;
  }
  if (nrow >= static_cast<int>(kVerbRows)) {
    nrow = 0;
  }
  if (ncol < 0) {
    ncol = 1;
  }
  if (ncol > 1) {
    ncol = 0;
  }
  return set_position(model, view, ncol * static_cast<int>(kVerbRows) + nrow);
}

void VerbMenuOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = verb_menu_panel(t);
  draw_dialog(r, d, "TOOLS");
  if (view.verb_menu.apps) {
    render_apps_page(model, view, r, d);
    return;
  }
  const int x = d.body.x + 2 * m.pad;
  const int right = d.body.x + d.body.w - 2 * m.pad;
  const int pitch = m.row_pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  const int col_w = (right - x) / 2;  // the two columns of pads 1-8 / 9-16
  // A label is clipped at its column's right edge (the panel is 300 px wide, so a column holds fewer cells than the longest label).
  const int label_x_off = 3 * cell;
  const int label_room = col_w - label_x_off > 0 ? (col_w - label_x_off) / cell : 0;
  const int cursor = view.verb_menu.cursor < kVerbCount ? view.verb_menu.cursor : 0;
  for (unsigned i = 0; i < kVerbCount; ++i) {
    const VerbEntry& e = kVerbEntries[i];
    const int row = static_cast<int>(i % kVerbRows);
    const int col = static_cast<int>(i / kVerbRows);
    const int cx = x + col * col_w;
    const int y = d.body.y + row * pitch;
    const Color ink = e.built ? Color::ChromeText : Color::ChromeDim;
    if (static_cast<int>(i) == cursor) {
      r.text(cx, ui::caps_y(t, Face::Data, y, pitch), ">", Color::ChromeText, Face::Data);
    }
    char num[3];  // the pad's number, 1..16 (no stdio in app: the digits by hand)
    num[0] = static_cast<char>('0' + (i + 1u) / 10u);
    num[1] = static_cast<char>('0' + (i + 1u) % 10u);
    num[2] = '\0';
    r.text(cx + cell, ui::caps_y(t, Face::Label, y, pitch), num, ink, Face::Label);
    core::FixedString<20> label;
    (void)append_clipped(label, e.label, static_cast<std::size_t>(label_room));
    r.text(cx + label_x_off, ui::caps_y(t, Face::Data, y, pitch), label.c_str(), ink, Face::Data);
  }
  const int foot = d.body.y + static_cast<int>(kVerbRows) * pitch;
  const char* pads = "PAD 1-16: PICK   F7+PAD: RUN";
  r.text(x, ui::caps_y(t, Face::Label, foot, pitch), pads, Color::ChromeDim, Face::Label);
  (void)draw_prompt_hints(r, x, foot + pitch, pitch, prompt_in_force(model, view), 2);
}

constexpr VerbMenuOverlay kVerbMenuOverlay{};

}  // namespace o3os::app
