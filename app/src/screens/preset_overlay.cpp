// SPDX-License-Identifier: GPL-3.0-or-later
// The F5 PRESET list (OS-INSFX-ABI2 A9, docs/specs/OS-INSFX-ABI2.md section 3.6): the presets of the module on the parameter page in view, an instrument's or an insert's. Same shape as the engine picker.
#include <o3os/app/key_prompt.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/param_set.h>
#include <o3os/app/preset_list.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kPresetW = 520;
constexpr int kRows = kPresetRows;

Dialog preset_dialog(const ui::Theme& theme) noexcept {
  return make_dialog(theme, kPresetW, (kRows + 2) * theme.metrics.row_pitch);  // a line of help, the rows, the keys
}

}  // namespace

Rect PresetOverlay::rect(const ui::Theme& theme) const noexcept { return preset_dialog(theme).outer; }

int PresetOverlay::count(const Model& model, const ViewState& view) const noexcept { return static_cast<int>(preset_list_count(model, view)); }

bool PresetOverlay::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int last = count(model, view) - 1;
  const int to = position < 0 ? 0 : (position > last ? (last < 0 ? 0 : last) : position);
  const bool changed = to != view.modules.preset;
  view.modules.preset = static_cast<std::uint8_t>(to);
  return changed;
}

void PresetOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = preset_dialog(t);
  core::FixedString<40> title;
  title.assign("PRESET  ");
  if (model.project != nullptr) {
    title.append(ModuleUi::module_name_of(*model.project, params_target(view)));
  }
  draw_dialog(r, d, title.c_str());
  const int x = d.body.x + 2 * m.pad;
  const int right = d.body.x + d.body.w - 2 * m.pad;
  const int pitch = m.row_pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  r.text(x, ui::caps_y(t, Face::Label, d.body.y, pitch), "Choose a preset. It sets every parameter.", Color::ChromeText, Face::Label);
  if (view.modules.preset_more) {  // OS-MODPRESET P4: the card held more presets than the list shows
    static constexpr char kMore[] = "+ MORE ON CARD";
    r.text(right - r.text_width(kMore, Face::Label), ui::caps_y(t, Face::Label, d.body.y, pitch), kMore, Color::ChromeDim, Face::Label);
  }
  // OS-MODPRESET P4: the list is the module's factory presets, then a CARD section with the card's presets of the module (the last scan's names; no file is read here, a frame).
  const unsigned factory = model.modules != nullptr && model.project != nullptr ? model.modules->preset_count(*model.project, params_target(view)) : 0u;
  const int total = count(model, view);
  const bool has_card = static_cast<unsigned>(total) > factory;
  const int header_line = has_card ? static_cast<int>(factory) : -1;  // the line the CARD heading takes
  const int lines = total + (has_card ? 1 : 0);
  const int cursor = view.modules.preset < total ? view.modules.preset : 0;
  const int cursor_line = cursor + (has_card && cursor >= static_cast<int>(factory) ? 1 : 0);
  const int first = cursor_line < kRows ? 0 : cursor_line - kRows + 1;
  if (total == 0) {
    r.text(x, ui::caps_y(t, Face::Label, d.body.y + pitch, pitch), "NO PRESETS, SHIFT+F5 SAVES", Color::ChromeDim, Face::Label);
  }
  for (int k = 0; k < kRows && first + k < lines; ++k) {
    const int line = first + k;
    const int y = d.body.y + (1 + k) * pitch;
    if (line == header_line) {  // the CARD section's heading
      r.text(x, ui::caps_y(t, Face::Label, y, pitch), "CARD", Color::ChromeDim, Face::Label);
      continue;
    }
    const int entry = line - (has_card && line > header_line ? 1 : 0);
    if (static_cast<unsigned>(entry) < factory) {
      ParamSet set;
      if (!preset_list_get(model, view, static_cast<unsigned>(entry), set)) {
        break;
      }
      if (entry == cursor) {
        r.text(x, ui::caps_y(t, Face::Data, y, pitch), ">", Color::ChromeText, Face::Data);
      }
      r.text(x + 2 * cell, ui::caps_y(t, Face::Data, y, pitch), set.name, Color::ChromeText, Face::Data);
      core::FixedString<16> note;  // how many parameters the preset names: the others take their defaults
      append_uint(note, set.given());
      note.append(set.given() == 1u ? " VALUE" : " VALUES");
      r.text(right - r.text_width(note.c_str(), Face::Label), ui::caps_y(t, Face::Label, y, pitch), note.c_str(), Color::ChromeDim, Face::Label);
    } else {  // a card preset: the name the scan listed (its file is read when F1 loads it)
      const char* name = model.modules != nullptr ? model.modules->card_preset_at(static_cast<unsigned>(entry) - factory) : nullptr;
      if (name == nullptr) {
        break;
      }
      if (entry == cursor) {
        r.text(x, ui::caps_y(t, Face::Data, y, pitch), ">", Color::ChromeText, Face::Data);
      }
      r.text(x + 2 * cell, ui::caps_y(t, Face::Data, y, pitch), name, Color::ChromeText, Face::Data);
    }
  }
  const int foot = d.body.y + (kRows + 1) * pitch;
  (void)draw_prompt_hints(r, x, foot, pitch, prompt_in_force(model, view), 2);
}

constexpr PresetOverlay kPresetOverlay{};

}  // namespace o3os::app
