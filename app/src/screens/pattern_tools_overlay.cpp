// SPDX-License-Identifier: GPL-3.0-or-later
// The Pattern screen's TOOLS dialog (OS-PTOOLS2 S2, docs/specs/OS-PTOOLS2.md section 3.1 point 3 and mock-up B without its PREVIEW lines — S5 was dropped; S3p: the
// right-hand panel, x 500..800 beside the narrowed grid, so the block stays visible while the dialog is open): the Song TOOLS dialog's shape
// (song_tools_overlay.cpp) over the selection's block — the OP row (TRANSPOSE, INSTRUMENT, CLEAR; Left / Right flips it), the OP's value row
// (AMOUNT -24..+24 st, Shift + encoder an octave; TO the instrument 1..64; WHAT for CLEAR), and a footer naming the target (BLOCK TRACK 1
// ROWS 01-16, or the cursor step) with the count (16 STEPS  5 NOTES). F1 APPLY runs the tool as one undo unit through pe::edit_selection;
// F8 BACK leaves the project untouched. Six rows, inside the 8-row rule. Its F-key row is the dialog's own (F1 APPLY, F8 BACK), painted on the
// frame band by render_scene (ViewState::overlay_keys).
#include <o3os/app/frame.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/pattern_tools_view.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/text_format.h>
#include <o3os/app/view_state.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

// The right-hand panel as a dialog: full height of the header and the pane, docked to the right edge (dialog.cpp).
Dialog tools_panel(const ui::Theme& theme) noexcept { return make_panel_dialog(theme); }

// The footer: where F1 acts (the block or the cursor step), and how many steps and notes that is: two lines from `fy`.
void draw_target_footer(ui::Renderer& r, int x, int fy, int pitch, const Model& model, const ViewState& view) noexcept {
  const ui::Theme& t = r.theme();
  const Selection::Block b = view.edit.selection.block(view.pattern.row(), view.pattern.track());
  core::FixedString<48> target;
  pattern_tools_target(view.edit.selection, view.pattern.row(), view.pattern.track(), target);
  r.text(x, ui::caps_y(t, Face::Label, fy, pitch), target.c_str(), Color::ChromeDim, Face::Label);
  unsigned steps = 0;
  unsigned notes = 0;
  pattern_tools_count(model.project, view.edit.pattern, b, steps, notes);
  core::FixedString<48> count;
  append_uint(count, steps);
  count.append(steps == 1u ? " STEP  " : " STEPS  ");
  append_uint(count, notes);
  count.append(notes == 1u ? " NOTE" : " NOTES");
  r.text(x, ui::caps_y(t, Face::Label, fy + pitch, pitch), count.c_str(), Color::ChromeDim, Face::Label);
}

// Row `i` of the panel: the caption at the left (the Label face, 11 cells at most) and the value from the 12th cell in the Data face, "< value unit >" on the cursor row, which is on the selection bar.
void draw_panel_row(ui::Renderer& r, const Dialog& d, int i, bool sel, const char* name, const char* text, const char* unit) noexcept {
  const ui::Theme& t = r.theme();
  const int pitch = t.metrics.row_pitch;
  const int x = d.body.x + 2 * t.metrics.pad;
  const int y = d.body.y + i * pitch;
  const Color ink = sel ? Color::SelText : Color::ChromeText;
  if (sel) {
    r.panel({d.body.x, y, d.body.w, pitch}, Color::Sel);
  }
  core::FixedString<12> caption;
  (void)append_clipped(caption, name, 11);
  r.text(x, ui::caps_y(t, Face::Label, y, pitch), caption.c_str(), ink, Face::Label);
  core::FixedString<28> value;
  if (sel) {
    value.append("< ");
  }
  value.append(text);
  if (unit[0] != '\0') {
    value.append(" ");
    value.append(unit);
  }
  if (sel) {
    value.append(" >");
  }
  r.text(x + 12 * t.geometry(Face::Data).cell_w, ui::caps_y(t, Face::Data, y, pitch), value.c_str(), ink, Face::Data);
}

}  // namespace

Rect PatternToolsOverlay::rect(const ui::Theme& theme) const noexcept { return tools_panel(theme).outer; }

void PatternToolsOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = tools_panel(t);
  const int pitch = m.row_pitch;
  const PatternToolsView& v = view.ptools;
  draw_dialog(r, d, "TOOLS");
  const int x = d.body.x + 2 * m.pad;
  const unsigned rows = pattern_tools_rows(v);
  for (unsigned i = 0; i < rows; ++i) {
    const PatternToolsRow row = static_cast<PatternToolsRow>(i);
    char text[core::kParamTextChars + 4];
    const unsigned n = pattern_tools_text(v, row, text, sizeof text);
    text[n < sizeof text ? n : sizeof text - 1u] = '\0';
    draw_panel_row(r, d, static_cast<int>(i), i == v.row, pattern_tools_caption(v, row), text,
                   row == PatternToolsRow::Value && v.tool == PTool::Transpose ? "ST" : "");
  }

  const int fy = d.body.y + (static_cast<int>(kPatternToolsRows) + 1) * pitch;  // after the rows and the blank
  draw_target_footer(r, x, fy, pitch, model, view);

  // How to move, on its own line (the panel is 300 px wide: the hints and this line do not share one), then the keys (F1 APPLY, F8 BACK:
  // the dialog line and the key row are one table, OS-KEYROWDLG).
  const char* help = "UP DOWN: ROW   LEFT RIGHT: VALUE";
  r.text(x, ui::caps_y(t, Face::Label, fy + 2 * pitch, pitch), help, Color::ChromeDim, Face::Label);
  const int foot = fy + 3 * pitch;
  (void)draw_prompt_hints(r, x, foot, pitch, prompt_in_force(model, view), 3);
}

constexpr PatternToolsOverlay kPatternToolsOverlay{};

// OS-PTOOLS2 S6: a small tool app (at most kPanelRows declared rows) in the TOOLS panel. The host draws the rows the app declared (no ABI change: the app's own page is not shown), the target
// is the selected block (the footer is the dialog's own). The row cursor is ViewState::modules.app_row, kept inside the declared rows by App::feed_module_app.
void draw_tools_app_panel(const Model& model, const ViewState& view, ui::Renderer& r) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = tools_panel(t);
  const int pitch = m.row_pitch;
  const ModuleUi* mu = model.modules;
  draw_dialog(r, d, "TOOLS");
  const int x = d.body.x + 2 * m.pad;
  if (mu == nullptr || mu->app_stopped()) {
    r.text(x, ui::caps_y(t, Face::Label, d.body.y, pitch), "APP STOPPED", Color::Accent, Face::Label);  // (the key row names F8 BACK)
    return;
  }
  const unsigned rows = mu->app_param_count() < ModuleUi::kPanelRows ? mu->app_param_count() : ModuleUi::kPanelRows;
  const unsigned cursor = view.modules.app_row < rows ? view.modules.app_row : 0u;
  for (unsigned i = 0; i < rows; ++i) {
    ParamScratch s;
    if (!mu->app_describe(i, s)) {
      continue;
    }
    char text[core::kParamTextChars + 1];
    (void)core::param_format(s.desc, mu->app_param(i), text, sizeof text);
    draw_panel_row(r, d, static_cast<int>(i), i == cursor, s.desc.name, text, s.desc.unit_text);
  }
  const int fy = d.body.y + (static_cast<int>(rows) + 1) * pitch;  // after the rows and the blank
  draw_target_footer(r, x, fy, pitch, model, view);
  // What the app said to the last F1 (the result, or why it did nothing), the answer line of its own page: two lines under the footer.
  char answer[72];
  if (mu->app_answer(answer, sizeof answer)) {
    draw_wrapped(r, x, fy + 2 * pitch, pitch, static_cast<std::size_t>((d.body.x + d.body.w - 2 * m.pad - x) / t.geometry(Face::Label).cell_w), answer, Color::ChromeText, 2);
  }
  r.text(x, ui::caps_y(t, Face::Label, fy + 4 * pitch, pitch), "UP DOWN: ROW   ENCODER: VALUE", Color::ChromeDim, Face::Label);
}

}  // namespace o3os::app
