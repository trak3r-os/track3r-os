// SPDX-License-Identifier: GPL-3.0-or-later
// The engine picker overlay (OS-012b): the SD instrument modules to bind to the instrument in view.
#include <o3os/app/governor_watch.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>

#include "dialog.h"

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kPickerW = 520;
constexpr int kPickerRows = kModulePickerRows;

Dialog picker_dialog(const ui::Theme& theme) noexcept {
  const ui::Metrics& m = theme.metrics;
  return make_dialog(theme, kPickerW, (kPickerRows + 2) * m.row_pitch);  // a line of help, the rows, the keys
}

const char* state_text(const CatalogEntry& e) noexcept {
  switch (e.state) {
    case CatalogEntry::State::Ready: return catalog_ready_text(e);
    case CatalogEntry::State::Loaded: return "LOADED";
    case CatalogEntry::State::Quarantined: return "QUARANTINED";
    case CatalogEntry::State::Missing: return "MISSING FROM THE CARD";
  }
  return "";
}

}  // namespace

Rect ModulePickerOverlay::rect(const ui::Theme& theme) const noexcept { return picker_dialog(theme).outer; }

int ModulePickerOverlay::count(const Model& model, const ViewState&) const noexcept {
  return model.modules != nullptr ? static_cast<int>(model.modules->instrument_count()) : 0;
}

bool ModulePickerOverlay::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int last = count(model, view) - 1;
  const int to = position < 0 ? 0 : (position > last ? (last < 0 ? 0 : last) : position);
  const bool changed = to != view.modules.picker;
  view.modules.picker = static_cast<std::uint8_t>(to);
  return changed;
}

void ModulePickerOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = picker_dialog(t);
  core::FixedString<24> title;
  title.assign("ENGINE  INSTRUMENT ");
  append_dec(title, static_cast<unsigned>(view.instrument < 1 ? 1 : view.instrument), 2);
  draw_dialog(r, d, title.c_str());
  const int x = d.body.x + 2 * m.pad;
  const int right = d.body.x + d.body.w - 2 * m.pad;
  const int pitch = m.row_pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  r.text(x, ui::caps_y(t, Face::Label, d.body.y, pitch), "Choose the module that plays this instrument.", Color::ChromeText, Face::Label);
  const ModuleUi* mu = model.modules;
  const int total = mu != nullptr ? static_cast<int>(mu->instrument_count()) : 0;
  const int cursor = view.modules.picker < total ? view.modules.picker : 0;
  const int first = cursor < kPickerRows ? 0 : cursor - kPickerRows + 1;
  if (total == 0) {
    r.text(x, ui::caps_y(t, Face::Label, d.body.y + pitch, pitch), mu == nullptr ? "NO MODULE SYSTEM" : "NO MODULES IN modules/instruments ON THE CARD", Color::ChromeDim, Face::Label);
  }
  for (int k = 0; k < kPickerRows && first + k < total; ++k) {
    const CatalogEntry& e = *mu->instrument_at(static_cast<unsigned>(first + k));
    const int y = d.body.y + (1 + k) * pitch;
    const bool sel = first + k == cursor;
    const bool bad = e.state == CatalogEntry::State::Missing || e.state == CatalogEntry::State::Quarantined;  // greyed: not available
    const Color ink = bad ? Color::ChromeDim : Color::ChromeText;
    if (sel) {
      r.text(x, ui::caps_y(t, Face::Data, y, pitch), ">", Color::ChromeText, Face::Data);
    }
    r.text(x + 2 * cell, ui::caps_y(t, Face::Data, y, pitch), e.stem, ink, Face::Data);
    if (e.state == CatalogEntry::State::Loaded) {  // OS-GOV: what a voice of the module cost when it last sounded (the render measures it); "UNMEASURED" until it has
      core::FixedString<24> cost;
      append_voice_cost(cost, mu->voice_cost_of(e.stem));
      r.text(x + 15 * cell, ui::caps_y(t, Face::Label, y, pitch), cost.c_str(), Color::ChromeDim, Face::Label);
    }
    const char* state = state_text(e);
    r.text(right - r.text_width(state, Face::Label), ui::caps_y(t, Face::Label, y, pitch), state, ink, Face::Label);
  }
  const int foot = d.body.y + (kPickerRows + 1) * pitch;
  (void)draw_prompt_hints(r, x, foot, pitch, prompt_in_force(model, view), 2);
}

constexpr ModulePickerOverlay kModulePickerOverlay{};

}  // namespace o3os::app
