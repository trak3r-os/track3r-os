// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/modules_screen.h>

#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

const char* state_text(const CatalogEntry& e) noexcept {
  switch (e.state) {
    case CatalogEntry::State::Ready: return catalog_ready_text(e);
    case CatalogEntry::State::Loaded: return "LOADED";
    case CatalogEntry::State::Quarantined: return "QUARANTINED";
    case CatalogEntry::State::Missing: return "MISSING";
  }
  return "";
}

constexpr int kListW = 400;       // the apps' list; the quarantine list is to its right
constexpr int kNameCols = 20;     // characters of an app's name the list shows (a stem may be 31)
constexpr int kQuarantineCols = 14;
constexpr int kParamColW = 300;   // an app page's parameter column

}  // namespace

// ---- MODULES ------------------------------------------------------------------------------------------------------------------

void ModulesScreen::enter(const Model& model, ViewState& view) const noexcept {
  if (model.modules != nullptr) {
    model.modules->scan_apps();
    const int last = static_cast<int>(model.modules->app_entry_count()) - 1;
    view.modules.apps = static_cast<std::uint8_t>(view.modules.apps > last ? (last < 0 ? 0 : last) : view.modules.apps);
  }
}

int ModulesScreen::count(const Model& model, const ViewState&) const noexcept {
  return model.modules != nullptr ? static_cast<int>(model.modules->app_entry_count()) : 0;
}

bool ModulesScreen::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int last = count(model, view) - 1;
  const int to = position < 0 ? 0 : (position > last ? (last < 0 ? 0 : last) : position);
  const bool changed = to != view.modules.apps;
  view.modules.apps = static_cast<std::uint8_t>(to);
  return changed;
}

void ModulesScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Frame f = make_frame(t, kFrameRows);
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const ModuleUi* mu = model.modules;
  const int apps = mu != nullptr ? static_cast<int>(mu->app_entry_count()) : 0;
  const int cursor = view.modules.apps < apps ? view.modules.apps : 0;
  const auto row_y = [&](int i) { return f.pane.y + i * f.row_pitch; };
  const int cell_w = t.geometry(Face::Data).cell_w;

  draw_chrome_bands(r, f, covered);
  if (apps != 0) {
    const Rect bar = {0, row_y(cursor), kListW, f.row_pitch};
    if (!hidden(bar)) {
      r.highlight_row(bar);
    }
  }
  for (int i = 0; i < apps; ++i) {
    const CatalogEntry& e = *mu->app_entry_at(static_cast<unsigned>(i));
    const bool sel = i == cursor;
    const bool bad = e.state == CatalogEntry::State::Quarantined || e.state == CatalogEntry::State::Missing;
    if (hidden({0, row_y(i), ui::kScreenW, f.row_pitch})) {
      continue;  // (OS-HELP: the page covers the pane; what lies wholly inside is left out, as the unit's fills do not hide it)
    }
    const int y = row_y(i) + (f.row_pitch - t.geometry(Face::Data).cell_h) / 2;
    core::FixedString<kNameCols + 1> name;
    append_clipped(name, e.stem, kNameCols);
    r.text(2 * m.pad, y, name.c_str(), sel ? Color::SelText : (bad ? Color::TextDim : Color::Text));
    r.text(2 * m.pad + (kNameCols + 1) * cell_w, ui::caps_y(t, Face::Label, row_y(i), f.row_pitch), state_text(e),
           sel ? Color::SelText : (e.state == CatalogEntry::State::Loaded ? Color::Instr : Color::TextDim), Face::Label);
  }
  if (apps == 0 && !hidden({0, row_y(1), ui::kScreenW, t.geometry(Face::Display).cell_h + m.pad + t.geometry(Face::Label).cell_h})) {
    const char* headline = mu == nullptr ? "NO MODULE SYSTEM" : (view.card_on_computer ? "CARD ON COMPUTER" : (model.card == nullptr || !model.card->mounted() ? "NO SD CARD" : "NO APPS"));
    r.text(2 * m.pad, row_y(1), headline, Color::Text, Face::Display);
    r.text(2 * m.pad, row_y(1) + t.geometry(Face::Display).cell_h + m.pad, "an app is a .tmod of kind app in modules/apps on the card", Color::TextDim, Face::Label);
  }
  // What this boot refused: file errors, budget overruns, crashes.
  const int qx = kListW + 4 * m.pad;
  const auto q_hidden = [&](int line) { return hidden({qx, row_y(line), ui::kScreenW - qx, f.row_pitch}); };
  if (!q_hidden(0)) {
    r.text(qx, ui::caps_y(t, Face::Label, row_y(0), f.row_pitch), "QUARANTINED THIS BOOT", Color::TextDim, Face::Label);
  }
  const module::ModuleSet* set = mu != nullptr ? mu->set() : nullptr;
  const unsigned q = set != nullptr ? set->quarantine_count() : 0u;
  if (q == 0u && !q_hidden(1)) {
    r.text(qx, ui::caps_y(t, Face::Label, row_y(1), f.row_pitch), "NOTHING", Color::TextDim, Face::Label);
  }
  const int reason_x = qx + (kQuarantineCols + 1) * cell_w;
  const int reason_cols = (ui::kScreenW - reason_x - 2 * m.pad) / t.geometry(Face::Label).cell_w;
  for (unsigned i = 0; i < q && i < 8u; ++i) {
    if (q_hidden(1 + static_cast<int>(i))) {
      continue;
    }
    core::FixedString<kQuarantineCols + 1> name;
    append_clipped(name, set->quarantine_name(i), kQuarantineCols);
    core::FixedString<48> reason;
    append_clipped(reason, module::status_name(set->quarantine_reason(i)), static_cast<std::size_t>(reason_cols > 0 ? (reason_cols < 47 ? reason_cols : 47) : 0));
    r.text(qx, row_y(1 + static_cast<int>(i)) + (f.row_pitch - t.geometry(Face::Data).cell_h) / 2, name.c_str(), Color::Fx);
    r.text(reason_x, ui::caps_y(t, Face::Label, row_y(1 + static_cast<int>(i)), f.row_pitch), reason.c_str(), Color::TextDim, Face::Label);
  }

  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    ui::FieldRow& row = strip.row();
    core::FixedString<16> text;
    row.put(Face::Display, "MODULES", Color::ChromeText);
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    append_uint(text, static_cast<unsigned>(apps));
    row.field("APPS", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    strip.cpu(model.load);
  }
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    r.text(2 * m.pad, cap_y, "APP", Color::ChromeDim, Face::Label);
    r.text(2 * m.pad + (kNameCols + 1) * cell_w, cap_y, "STATE", Color::ChromeDim, Face::Label);
  }
  if (!hidden(f.info)) {
    draw_info_line(r, f, "F1 launches the app on its own page", "exit an app with F8");
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    draw_question_keys(r, f.keys, *this, model, view, kModulesSoftKeys);  // (OS-KEYROWDLG: LAUNCH? while the confirm waits)
  }
}

// ---- an app's page --------------------------------------------------------------------------------------------------------------

void ModuleAppScreen::leave(const Model& model, ViewState& view) const noexcept {
  view.modules.app_keys = false;
  if (model.modules != nullptr) {
    model.modules->app_exit();
  }
}

int ModuleAppScreen::count(const Model& model, const ViewState&) const noexcept {
  return model.modules != nullptr && model.modules->app_os_column() ? static_cast<int>(model.modules->app_param_count()) : 0;
}

bool ModuleAppScreen::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int last = count(model, view) - 1;
  const int to = position < 0 ? 0 : (position > last ? (last < 0 ? 0 : last) : position);
  const bool changed = to != view.modules.app_row;
  view.modules.app_row = static_cast<std::uint8_t>(to);
  return changed;
}

void ModuleAppScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Frame f = make_frame(t, kFrameRows);
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  ModuleUi* mu = model.modules;
  const unsigned params = mu != nullptr && mu->app_os_column() ? mu->app_param_count() : 0u;  // (an app of ABI 1.10 draws its own rows: no OS column)
  const int cursor = view.modules.app_row < static_cast<int>(params) ? view.modules.app_row : 0;
  const int cell_w = t.geometry(Face::Data).cell_w;
  const auto row_y = [&](int i) { return f.pane.y + i * f.row_pitch; };

  draw_chrome_bands(r, f, covered);
  const int body_x = params != 0u ? kParamColW + 2 * m.gap : 0;
  const Rect body = {body_x, f.pane.y, ui::kScreenW - body_x, f.pane.h};
  const bool stopped = mu != nullptr && mu->app_stopped();
  if (mu != nullptr && !stopped && !hidden(body)) {
    (void)mu->draw_app_view(r, body);
  }
  if ((mu == nullptr || mu->app_stopped()) && !hidden(body)) {  // (a budget overrun in the draw above is seen here)
    r.text(body.x + 2 * m.pad, row_y(1), "APP STOPPED", Color::Accent, Face::Display);
    r.text(body.x + 2 * m.pad, row_y(1) + t.geometry(Face::Display).cell_h + m.pad, mu != nullptr ? module::view_status_name(mu->app_verdict()) : "no module system", Color::Text,
           Face::Label);
    r.text(body.x + 2 * m.pad, row_y(2) + t.geometry(Face::Display).cell_h + 2 * m.pad, "it is quarantined until reboot: leave with F8", Color::TextDim, Face::Label);
  }
  // The parameters: the standard controls (name, value, unit, bar), edited with Shift.
  for (unsigned i = 0; i < params && i < ParamView::kMaxRows; ++i) {
    ParamScratch s;
    if (!mu->app_describe(i, s)) {
      continue;
    }
    const bool sel = static_cast<int>(i) == cursor;
    char text[core::kParamTextChars + 1];
    (void)core::param_format(s.desc, mu->app_param(i), text, sizeof text);
    const Rect line = {0, row_y(static_cast<int>(i)), kParamColW, f.row_pitch};
    if (hidden(line)) {
      continue;  // (under the help page: nothing of it is drawn)
    }
    if (sel) {
      r.highlight_row(line);
    }
    const int y = line.y + (f.row_pitch - t.geometry(Face::Data).cell_h) / 2;
    r.text(2 * m.pad, y, s.desc.name, sel ? Color::SelText : Color::Text);
    const int tw = r.text_width(text, Face::Data);
    r.text(2 * m.pad + static_cast<int>(core::kParamNameChars + 1) * cell_w + 8 * cell_w - tw, y, text, sel ? Color::SelText : Color::Instr);
    r.text(2 * m.pad + static_cast<int>(core::kParamNameChars + 1) * cell_w + 8 * cell_w + m.gap, ui::caps_y(t, Face::Label, line.y, f.row_pitch), s.desc.unit_text,
           sel ? Color::SelText : Color::TextDim, Face::Label);
  }
  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    ui::FieldRow& row = strip.row();
    row.put(Face::Display, "APP", Color::ChromeText);
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    row.field("NAME", Face::Data, mu != nullptr ? mu->app_name() : "", Color::ChromeDim, Color::ChromeText);
    if (stopped) {
      row.field("STATE", Face::Data, "STOPPED", Color::ChromeDim, Color::Accent, Color::AccentText);  // (on a chip: the orange is not 3:1 on Chrome)
    } else {
      row.field("STATE", Face::Data, "RUNNING", Color::ChromeDim, Color::ChromeText);
    }
    strip.cpu(model.load);
  }
  if (!hidden(f.header) && params != 0u) {
    r.text(2 * m.pad, ui::caps_y(t, Face::Label, f.header.y, f.header.h), "PARAMETERS", Color::ChromeDim, Face::Label);
  }
  if (!hidden(f.info)) {
    draw_info_line(r, f, params != 0u ? "shift + up / down: parameter   shift + encoder: edit   shift + delete: default" : "the app has the input", "F8: back");
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    if (mu != nullptr && mu->app_keys_declared()) {  // ABI 1.9: the app's labels (Shift + F1..F7 while Shift is held and the page follows it); a blank key reads "-", F8 is BACK, Shift + F8 reads nothing
      const bool shifted = view.shift && follows_shift(*this, view);
      const char* row[8];
      for (unsigned i = 0; i < 8u; ++i) {
        const char* label = mu->app_key_label(i, shifted);
        row[i] = i == 7u ? (shifted ? "" : kModuleAppSoftKeys[7]) : (label[0] != '\0' ? label : "-");
      }
      ui::soft_keys(r, f.keys, row, 8);
    } else {
      ui::soft_keys(r, f.keys, kModuleAppSoftKeys, 8);
    }
  }
}

constexpr ModulesScreen kModulesScreen{};
constexpr ModuleAppScreen kModuleAppScreen{};

}  // namespace o3os::app
