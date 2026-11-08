// SPDX-License-Identifier: GPL-3.0-or-later
// The on-screen keyboard overlay (OS-FU2): the name being typed above the four rows of keys (text_entry.h), the focus on one, the keys the actions bind below. The key grid is the
// pads' own: 12 columns by 4 rows, one cell a pad, so the backspace and caps keys are two cells wide and the space key five, as under the fingers.
#include <o3os/app/key_prompt.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/text_entry.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kCols = 12;

int cell_w(const ui::Theme& t) noexcept { return t.geometry(Face::Data).cell_w * 4; }

Dialog keyboard_dialog(const ui::Theme& t) noexcept {
  const ui::Metrics& m = t.metrics;
  return make_dialog(t, kCols * cell_w(t) + 6 * m.pad, 8 * m.row_pitch);  // the name, its message, four key rows of 1.25 pitch, the hints
}

const char* title_of(TextEntryView::Target target) noexcept {
  switch (target) {
    case TextEntryView::Target::SaveAs: return "SAVE AS";
    case TextEntryView::Target::Instrument: return "INSTRUMENT NAME";
    case TextEntryView::Target::Track: return "TRACK NAME";
    case TextEntryView::Target::Pattern: return "PATTERN NAME";
    case TextEntryView::Target::Project: return "PROJECT NAME";
    case TextEntryView::Target::Preset: return "PRESET NAME";  // OS-MODPRESET P4
    case TextEntryView::Target::None: break;
  }
  return "TEXT";
}

}  // namespace

Rect TextEntryOverlay::rect(const ui::Theme& theme) const noexcept { return keyboard_dialog(theme).outer; }

void TextEntryOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = keyboard_dialog(t);
  const TextEntry& e = view.text.entry;
  draw_dialog(r, d, title_of(view.text.target));
  const int pitch = m.row_pitch;
  const int x0 = d.body.x + 3 * m.pad;
  const int cw = cell_w(t);
  const int cell = t.geometry(Face::Data).cell_w;

  // The name: a box the width of the keys, the text in the Data face, the whole of it on Sel while it is selected, an Accent bar at the edit cursor while the focus is in it.
  const Rect box{x0, d.body.y, kCols * cw, pitch};
  r.frame(box, e.frame() ? Color::ChromeText : Color::ChromeDim);
  const int tx = box.x + m.pad;
  const int ty = ui::caps_y(t, Face::Data, box.y, pitch);
  if (e.selected() && e.length() != 0u) {
    r.panel({tx - 1, box.y + m.border + 1, static_cast<int>(e.length()) * cell + 2, pitch - 2 * m.border - 2}, Color::Sel);
    r.text(tx, ty, e.text(), Color::SelText, Face::Data);
  } else {
    r.text(tx, ty, e.text(), Color::ChromeText, Face::Data);
  }
  if (e.frame()) {
    r.panel({tx + static_cast<int>(e.edit()) * cell, box.y + m.border + 1, 2, pitch - 2 * m.border - 2}, Color::Accent);
  }
  // What the keyboard says: a refusal, or what the focus is on.
  const char* say = view.text.message[0] != '\0' ? view.text.message : (e.frame() ? "LEFT / RIGHT MOVE IN THE NAME, DOWN TO THE KEYS" : "");
  r.text(x0, ui::caps_y(t, Face::Label, d.body.y + pitch, pitch), say, view.text.message[0] != '\0' ? Color::ChromeText : Color::ChromeDim, Face::Label);

  // The keys: one cell a pad; a key's cells are the pads that name it.
  const int row_h = pitch * 5 / 4;
  const int y_keys = d.body.y + 2 * pitch;
  for (unsigned row = 0; row < kTextRows; ++row) {
    const int y = y_keys + static_cast<int>(row) * row_h;
    for (unsigned k = kTextRowStart[row]; k < kTextRowStart[row + 1]; ++k) {
      int first = -1;
      int span = 0;
      for (int c = 0; c < kCols; ++c) {
        if (kTextPadKey[row * kCols + static_cast<unsigned>(c)] == k) {
          first = first < 0 ? c : first;
          ++span;
        }
      }
      if (first < 0) {
        continue;
      }
      const Rect cellr{x0 + first * cw, y, span * cw - 2, row_h - 2};
      const bool focus = !e.frame() && e.key() == k;
      const bool allowed = e.key_allowed(k);
      const char ch = e.key_char(k);
      const char* label = ch == 0 ? "DEL" : (ch == 1 ? "CAPS" : (ch == ' ' ? "SPACE" : nullptr));
      char one[2] = {ch, '\0'};
      const char* s = label != nullptr ? label : one;
      const bool caps_on = ch == 1 && e.shift();
      if (focus) {
        r.panel(cellr, Color::Accent);
      } else if (caps_on) {
        r.panel(cellr, Color::ChromeText);
      } else {
        r.frame(cellr, Color::ChromeDim);
      }
      const Color ink = focus ? Color::AccentText : (caps_on ? Color::Chrome : (allowed ? Color::ChromeText : Color::ChromeDim));
      const Face face = label != nullptr ? Face::Label : Face::Data;
      r.text(cellr.x + (cellr.w - r.text_width(s, face)) / 2, ui::caps_y(t, face, cellr.y, cellr.h), s, ink, face);
    }
  }
  const int foot = d.body.y + 2 * pitch + 4 * row_h + m.pad;
  // TYPE, CAPS, AUTO (the automatic name is the Save As's: the next free "Track NN"), CANCEL, SAVE or OK: the prompt's live keys (OS-KEYROWDLG)
  (void)draw_prompt_hints(r, x0, foot, pitch, prompt_in_force(model, view), 1);
}

constexpr TextEntryOverlay kTextEntryOverlay{};

}  // namespace o3os::app
