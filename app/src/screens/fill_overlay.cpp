// SPDX-License-Identifier: GPL-3.0-or-later
// The Fill dialog (OS-PTOOLS part 1; docs/specs/OS-PTOOLS.md; OS-FILL2): seven rows (fill_view.h; OS-PTOOLS2 S3c: no SCOPE row, the selection is the target), a line that shows which steps the rule chooses in the range, a footer
// that names the target, and the keys. The F-key row is the dialog's own (F1 FILL, F8 BACK), painted on the frame band as the Navigator's is (ViewState::overlay_keys), so the Pattern screen's labels do not show through it.
#include <o3os/app/frame.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/pattern_source.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kFillW = 520;
constexpr int kPreviewMax = 64;  // the longest line of x and . the dialog draws

Dialog fill_dialog(const ui::Theme& theme) noexcept {
  return make_dialog(theme, kFillW, (static_cast<int>(kFillRows) + 4) * theme.metrics.row_pitch);  // the rows, the preview and its count, the target, the keys
}

// The steps of the range the preview describes: the selection's, on the cursor's track, cut at the track's length (a block or the cursor step; TRACK and PATTERN selections are the whole range, every track cut at its own length).
int preview_length(const Model& model, const ViewState& view) noexcept {
  const PatternSource src(model, view.edit.pattern);
  const int len = src.track_length(view.pattern.track());
  const Selection::Block b = view.edit.selection.block(view.pattern.row(), view.pattern.track());
  const int last = b.row1 < len - 1 ? b.row1 : len - 1;
  return last >= b.row0 ? last - b.row0 + 1 : 0;
}

}  // namespace

Rect FillOverlay::rect(const ui::Theme& theme) const noexcept { return fill_dialog(theme).outer; }

void FillOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = fill_dialog(t);
  const int pitch = m.row_pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  const FillView& f = view.fill;
  const int octave_shift = pattern_octave_shift(model);  // the notes of FROM and TO are named as the grid names them (MIDDLE C)
  draw_dialog(r, d, "FILL");
  const int x = d.body.x + 2 * m.pad;
  const int value_x = x + 12 * cell;
  for (unsigned i = 0; i < kFillRows; ++i) {
    const FillRow row = static_cast<FillRow>(i);
    const int y = d.body.y + static_cast<int>(i) * pitch;
    const bool sel = i == f.row;
    const bool used = fill_row_used(f, row);
    const Color ink = sel ? Color::SelText : (used ? Color::ChromeText : Color::ChromeDim);
    if (sel) {
      r.panel({d.body.x, y, d.body.w, pitch}, Color::Sel);
    }
    r.text(x, ui::caps_y(t, Face::Label, y, pitch), fill_caption(f, row), ink, Face::Label);
    char text[32];
    const unsigned n = fill_text(f, row, text, sizeof text, octave_shift);
    core::FixedString<36> value;
    if (sel && used) {
      value.assign("< ");
    }
    value.append(text, n);
    if (row == FillRow::Count && f.plan.type == model::fill::Type::Random) {
      value.append(" %");
    }
    if (sel && used) {
      value.append(" >");
    }
    r.text(value_x, ui::caps_y(t, Face::Data, y, pitch), value.c_str(), ink, Face::Data);
  }

  // Which steps the rule chooses, over the range (the first steps of it when it is longer than the line): "x" a step that is written. RANDOM is a draw, so it only says the chance.
  const int py = d.body.y + static_cast<int>(kFillRows) * pitch;
  const int len = preview_length(model, view);
  const int inner = d.body.w - 4 * m.pad;
  int shown = len < kPreviewMax ? len : kPreviewMax;
  shown = shown < inner / cell - 1 ? shown : inner / cell - 1;
  core::FixedString<kPreviewMax + 1> line;
  core::FixedString<32> count;
  if (f.plan.type == model::fill::Type::Random) {
    count.assign("ABOUT ");
    append_uint(count, f.plan.density);
    count.append(" IN 100 STEPS");
  } else {
    model::fill::Rng rng;
    bool hit[model::kMaxPatternLength];
    const int n = len < static_cast<int>(model::kMaxPatternLength) ? len : static_cast<int>(model::kMaxPatternLength);
    const unsigned hits = model::fill::select(f.plan, static_cast<unsigned>(n), rng, hit);
    for (int i = 0; i < shown; ++i) {
      line.append(hit[i] ? "x" : ".");
    }
    append_uint(count, hits);
    count.append(" OF ");
    append_uint(count, static_cast<unsigned>(len));
    count.append(" STEPS");
  }
  r.text(x, ui::caps_y(t, Face::Data, py, pitch), line.c_str(), Color::ChromeText, Face::Data);
  r.text(x, ui::caps_y(t, Face::Label, py + pitch, pitch), count.c_str(), Color::ChromeDim, Face::Label);

  // The footer names the target, in the Song TOOLS way (`BLOCK  TRACK 3  ROWS 1-16`; OS-PTOOLS2 S3c: the selection is the scope).
  const int ty = py + 2 * pitch;
  const PatternSource src(model, view.edit.pattern);
  char target[48];
  (void)fill_target_text(view.edit.selection, view.pattern.row(), view.pattern.track(), src.length(), src.tracks(), target, sizeof target);
  r.text(x, ui::caps_y(t, Face::Label, ty, pitch), target, Color::ChromeDim, Face::Label);

  const int foot = py + 3 * pitch;
  (void)draw_prompt_hints(r, x, foot, pitch, prompt_in_force(model, view), 3);  // F1 FILL, F8 BACK: the dialog line and the key row are one table (OS-KEYROWDLG)
  const char* help = "UP DOWN: ROW   LEFT RIGHT: VALUE";
  r.text(d.body.x + d.body.w - 2 * m.pad - r.text_width(help, Face::Label), ui::caps_y(t, Face::Label, foot, pitch), help, Color::ChromeDim, Face::Label);
  // (The F-key row is the dialog's prompt, painted by render_scene: F8 is BACK, not the screen's UNDO.)
}

constexpr FillOverlay kFillOverlay{};

}  // namespace o3os::app
