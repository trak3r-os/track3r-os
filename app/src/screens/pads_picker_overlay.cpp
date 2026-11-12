// SPDX-License-Identifier: GPL-3.0-or-later
// The pads' mode list (OS-UXF-PAT 7): the six modes a key picks, opened by Shift + F2 PADS on the Pattern screen. The keys are the choice (F1..F6 = padmode.pick, F8 BACK), so there is no cursor.
#include <o3os/app/key_prompt.h>
#include <o3os/app/pad_layer.h>
#include <o3os/app/screens/overlay.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kPickW = 520;
constexpr int kPickRows = 6;

// The row order is the key order of the F-key row (key_prompt.cpp kPadsPickerOptions) and of the bindings (bindings.cpp kPadsPickerCtx).
constexpr PadMode kPickMode[kPickRows] = {PadMode::Notes, PadMode::Piano, PadMode::Iso, PadMode::Tracks, PadMode::Patterns, PadMode::Vu};
constexpr const char* kPickHelp[kPickRows] = {"the usual note pads", "two octaves, like a piano", "a semitone a column, a fourth a row", "a column a track: mute and solo",
                                              "a pad a pattern, 1 to 48", "a level meter a track, again: faders"};

Dialog pick_dialog(const ui::Theme& theme) noexcept {
  return make_dialog(theme, kPickW, (kPickRows + 2) * theme.metrics.row_pitch);  // a line of help, the modes, the keys
}

}  // namespace

Rect PadsPickerOverlay::rect(const ui::Theme& theme) const noexcept { return pick_dialog(theme).outer; }

void PadsPickerOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = pick_dialog(t);
  draw_dialog(r, d, "PADS");
  const int x = d.body.x + 2 * m.pad;
  const int right = d.body.x + d.body.w - 2 * m.pad;
  const int pitch = m.row_pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  r.text(x, ui::caps_y(t, Face::Label, d.body.y, pitch), "What the 48 pads do. Shift + F1 is the octave.", Color::ChromeText, Face::Label);
  for (int k = 0; k < kPickRows; ++k) {
    const int y = d.body.y + (1 + k) * pitch;
    const char key[3] = {'F', static_cast<char>('1' + k), '\0'};
    r.text(x, ui::caps_y(t, Face::Data, y, pitch), key, Color::ChromeText, Face::Data);
    r.text(x + 4 * cell, ui::caps_y(t, Face::Data, y, pitch), kPadModeNames[static_cast<unsigned>(kPickMode[k])], Color::ChromeText, Face::Data);
    r.text(x + 12 * cell, ui::caps_y(t, Face::Label, y, pitch), kPickHelp[k], Color::ChromeDim, Face::Label);
    if (kPickMode[k] == view.padmode.fixed()) {
      r.text(right - r.text_width("NOW", Face::Label), ui::caps_y(t, Face::Label, y, pitch), "NOW", Color::ChromeText, Face::Label);
    }
  }
  const int foot = d.body.y + (kPickRows + 1) * pitch;
  (void)draw_prompt_hints(r, x, foot, pitch, prompt_in_force(model, view), 2);
}

constexpr PadsPickerOverlay kPadsPickerOverlay{};

}  // namespace o3os::app
