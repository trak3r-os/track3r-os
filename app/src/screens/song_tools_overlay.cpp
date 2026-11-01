// SPDX-License-Identifier: GPL-3.0-or-later
// The Song screen's TOOLS dialog (OS-SONGMAP, docs/specs/SONG-UX.md section 1.4; song_tools_view.h): the OP row, the value row of the OP, the scope of the edit and the keys. Its F-key row is the dialog's own (F1 APPLY,
// F8 BACK), painted on the frame band by render_scene (ViewState::overlay_keys), so the Song screen's labels do not show through it.
#include <o3os/app/frame.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/song_tools_view.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kToolsW = 520;

Dialog tools_dialog(const ui::Theme& theme) noexcept {
  return make_dialog(theme, kToolsW, (static_cast<int>(kSongToolsRows) + 3) * theme.metrics.row_pitch);  // the rows, a blank, the scope, the keys
}

// "SLOTS 03-10  TRACKS 2-3  5 PATTERNS": where the edit acts. A single slot or track says its number alone. EXPORT WAV with nothing selected in the map, or on PAT, renders every track: ALL TRACKS.
void scope_text(const SongToolsView& t, core::FixedString<48>& out) noexcept {
  out.assign(t.rect.slots() == 1 ? "SLOT " : "SLOTS ");
  append_dec(out, static_cast<unsigned>(t.rect.s0), 2);
  if (t.rect.slots() > 1) {
    out.append("-");
    append_dec(out, static_cast<unsigned>(t.rect.s1), 2);
  }
  out.append("  ");
  if (t.tool == SongTool::ExportWav && t.whole) {
    out.append("ALL TRACKS");
  } else {
    out.append(t.rect.tracks() == 1 ? "TRACK " : "TRACKS ");
    append_uint(out, static_cast<unsigned>(t.rect.t0 + 1));
    if (t.rect.tracks() > 1) {
      out.append("-");
      append_uint(out, static_cast<unsigned>(t.rect.t1 + 1));
    }
  }
  out.append("  ");
  append_uint(out, t.patterns);
  out.append(t.patterns == 1u ? " PATTERN" : " PATTERNS");
}

}  // namespace

Rect SongToolsOverlay::rect(const ui::Theme& theme) const noexcept { return tools_dialog(theme).outer; }

void SongToolsOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = tools_dialog(t);
  const int pitch = m.row_pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  const SongToolsView& v = view.tools;
  draw_dialog(r, d, "SONG TOOLS");
  const int x = d.body.x + 2 * m.pad;
  const int value_x = x + 12 * cell;
  for (unsigned i = 0; i < song_tools_rows(v); ++i) {
    const SongToolsRow row = static_cast<SongToolsRow>(i);
    const int y = d.body.y + static_cast<int>(i) * pitch;
    const bool sel = i == v.row;
    const Color ink = sel ? Color::SelText : Color::ChromeText;
    if (sel) {
      r.panel({d.body.x, y, d.body.w, pitch}, Color::Sel);
    }
    r.text(x, ui::caps_y(t, Face::Label, y, pitch), song_tools_caption(v, row), ink, Face::Label);
    char text[core::kParamTextChars + 4];
    const unsigned n = song_tools_text(v, row, text, sizeof text);
    core::FixedString<24> value;
    if (sel) {
      value.assign("< ");
    }
    value.append(text, n);
    if (row == SongToolsRow::Value && v.tool == SongTool::Transpose) {
      value.append(" ST");
    }
    if (sel) {
      value.append(" >");
    }
    r.text(value_x, ui::caps_y(t, Face::Data, y, pitch), value.c_str(), ink, Face::Data);
  }

  // The line under the rows says what F1 does (EXPORT WAV has no value row; FORM's third row is the last).
  const char* about = nullptr;
  switch (v.tool) {
    case SongTool::ExportWav: about = "A WAV FILE OF THE SLOTS, WITH ITS TAIL"; break;
    case SongTool::Form: about = "A SONG SHAPE BY SUBTRACTION"; break;
    case SongTool::Bridge: about = "A RUN OF SLOTS FROM A TO B"; break;
    case SongTool::Transpose:
    case SongTool::Instrument:
    case SongTool::Clear: break;
  }
  if (about != nullptr) {
    r.text(x, ui::caps_y(t, Face::Label, d.body.y + static_cast<int>(song_tools_rows(v)) * pitch, pitch), about, Color::ChromeDim, Face::Label);
  }

  // Where it acts (FORM: what it will make), and the keys.
  const int sy = d.body.y + (static_cast<int>(kSongToolsRows) + 1) * pitch;
  core::FixedString<48> scope;
  if (v.tool == SongTool::Form) {
    song_form_footer(v.plan, scope);
  } else if (v.tool == SongTool::Bridge) {
    song_bridge_footer(v.plan, scope);
  } else {
    scope_text(v, scope);
  }
  r.text(x, ui::caps_y(t, Face::Label, sy, pitch), scope.c_str(), Color::ChromeDim, Face::Label);
  const int foot = sy + pitch;
  (void)draw_prompt_hints(r, x, foot, pitch, prompt_in_force(model, view), 3);  // F1 APPLY, F8 BACK: the dialog line and the key row are one table (OS-KEYROWDLG)
  const char* help = v.tool == SongTool::ExportWav ? "LEFT RIGHT: OP" : "UP DOWN: ROW   LEFT RIGHT: VALUE";  // (EXPORT WAV has the OP row alone)
  r.text(d.body.x + d.body.w - 2 * m.pad - r.text_width(help, Face::Label), ui::caps_y(t, Face::Label, foot, pitch), help, Color::ChromeDim, Face::Label);
}

constexpr SongToolsOverlay kSongToolsOverlay{};

}  // namespace o3os::app
