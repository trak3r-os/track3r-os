// SPDX-License-Identifier: GPL-3.0-or-later
// The step popup overlay (OS-FU3): the lists of a held Instr, Fx or Note button as a panel
// over the right of the pane (x 600..800): the 64 instruments ("n. name"), or the 60 FX with the description of the chosen one, or (OS-PADS-B) the note-entry settings. The choice is
// drawn on Sel, as the pattern's selection. The descriptions are this OS's own words (no text of the official firmware is reused).
#include <o3os/app/frame.h>
#include <o3os/app/fx_text.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/pattern_source.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/model/fx_table.h>

#include "dialog.h"

#include <cstring>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

constexpr int kPanelW = 320;

// An octave number as the Pattern view names it: "-1" below the first one.
template <std::size_t N>
void append_octave(core::FixedString<N>& text, int octave) noexcept {
  if (octave < 0) {
    text.append("-");
    octave = -octave;
  }
  append_uint(text, static_cast<unsigned>(octave));
}

constexpr int kFxDescRows = 3;  // lines of the chosen FX's description under its list

Dialog popup_dialog(const ui::Theme& t) noexcept {
  const ui::Metrics& m = t.metrics;
  const int edge = m.border + m.border;
  const Frame f = make_frame(t, kFrameRows);
  Dialog d{};
  d.outer = {ui::kScreenW - kPanelW, f.pane.y, kPanelW, f.pane.h};
  d.title = {d.outer.x + edge, d.outer.y + edge, kPanelW - 2 * edge, m.row_pitch};
  const int top = d.title.y + d.title.h + m.pad;
  d.body = {d.title.x, top, d.title.w, d.outer.y + d.outer.h - edge - top};
  return d;
}

// `text` as lines of at most `width` pixels (Label face), at most `max_lines`; returns the lines drawn from (x, y), one `pitch` apart.
void draw_wrapped(ui::Renderer& r, int x, int y, int pitch, int width, int max_lines, const char* text, Color ink) noexcept {
  const ui::Theme& t = r.theme();
  core::FixedString<64> line;
  int lines = 0;
  const char* p = text;
  while (*p != '\0' && lines < max_lines) {
    const char* end = std::strchr(p, ' ');
    const std::size_t n = end != nullptr ? static_cast<std::size_t>(end - p) : std::strlen(p);
    core::FixedString<64> trial = line;
    if (!trial.empty()) {
      trial.append(" ");
    }
    trial.append(p, n);
    if (!line.empty() && r.text_width(trial.c_str(), Face::Label) > width) {
      r.text(x, ui::caps_y(t, Face::Label, y + lines * pitch, pitch), line.c_str(), ink, Face::Label);
      ++lines;
      line.clear();
      continue;  // the word starts the next line
    }
    line = trial;
    p += n;
    if (*p == ' ') {
      ++p;
    }
  }
  if (!line.empty() && lines < max_lines) {
    r.text(x, ui::caps_y(t, Face::Label, y + lines * pitch, pitch), line.c_str(), ink, Face::Label);
  }
}

// The window of `total` rows of which `rows` show, the cursor in view and near the middle.
int first_row(int cursor, int total, int rows) noexcept {
  int first = cursor - rows / 2;
  if (first > total - rows) {
    first = total - rows;
  }
  return first < 0 ? 0 : first;
}

}  // namespace

Rect StepPopupOverlay::rect(const ui::Theme& theme) const noexcept { return popup_dialog(theme).outer; }

int step_popup_list_rows(const ui::Theme& theme, std::uint8_t kind) noexcept {
  if (kind == StepPopupView::Note) {
    return 0;
  }
  const bool is_fx = kind == StepPopupView::Fx || kind == StepPopupView::Vol;
  return popup_dialog(theme).body.h / theme.metrics.row_pitch - 1 - (is_fx ? kFxDescRows : 0);  // (the line of the foot, the FX description)
}

bool step_popup_pickable(const Model& model, const ViewState& view, unsigned slot) noexcept {
  if (!view.popup.samples_only) {
    return true;
  }
  const model::Instrument* in = model.project != nullptr ? model.project->instrument(slot) : nullptr;
  return in != nullptr && in->kind == model::InstrumentKind::Sample;
}

void step_popup_instrument_text(const model::Instrument* in, core::FixedString<40>& out) noexcept {
  out.clear();
  if (in == nullptr) {
    return;
  }
  if (in->kind == model::InstrumentKind::Midi) {
    out.assign("MIDI CH ");
    append_uint(out, in->midi_channel);
    return;
  }
  out.append(in->name);
}

bool StepPopupOverlay::track(const Model& model, ViewState& view, int delta) const noexcept {
  if (view.screen != ScreenId::Pattern) {
    return false;
  }
  const bool moved = view.pattern.move_track(delta, pattern_dims(model, view));
  const bool was = view.edit.selection.active;  // a plain move collapses the block, as the arrows do (PatternScreen::track)
  view.edit.selection.clear();
  return moved || was;
}

void StepPopupOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = popup_dialog(t);
  const int pitch = m.row_pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  const StepPopupView& pop = view.popup;
  const bool is_instr = pop.kind == StepPopupView::Instr;
  const bool is_fx = pop.kind == StepPopupView::Fx || pop.kind == StepPopupView::Vol;
  draw_dialog(r, d, is_instr ? "INSTRUMENTS" : (is_fx ? "FX" : "NOTE ENTRY"));
  const int x = d.body.x + m.pad;
  const int inner = d.body.w - 2 * m.pad;
  const int rows = d.body.h / pitch;
  const int foot = d.body.y + (rows - 1) * pitch;
  // What the release does: in Edit on Pattern it enters the choice in the cursor step.
  const bool enters = pop.kind != StepPopupView::Note && view.screen == ScreenId::Pattern && pattern_mode_edits(view.mode[screen_index(ScreenId::Pattern)]);
  const int used = draw_prompt_hints(r, x, foot, pitch, prompt_in_force(model, view), 2) + 2 * m.gap;
  r.text(x + used, ui::caps_y(t, Face::Label, foot, pitch), enters ? "LET GO TO ENTER" : "LET GO TO CLOSE", Color::ChromeDim, Face::Label);
  if (pop.kind == StepPopupView::Note && view.screen == ScreenId::Pattern) {  // OS-ZOOM: the held Note's keys, said where the Note popup is: two lines above the footer
    r.text(x, ui::caps_y(t, Face::Label, foot - 2 * pitch, pitch), "NOTE + INSTR: ZOOM 5 / 8 / 12 TRACKS", Color::ChromeDim, Face::Label);
    r.text(x, ui::caps_y(t, Face::Label, foot - pitch, pitch), "NOTE + LEFT / RIGHT: MOVE A TRACK", Color::ChromeDim, Face::Label);
  }

  if (pop.kind == StepPopupView::Note) {
    // OS-PADS-B: the settings of the note entry, as one line and the rows that the pads are (the pads' LEDs show each row's choice).
    const NoteEntry& e = view.edit.entry;
    core::FixedString<48> line;
    line.assign(e.root_name());
    line.append(" ");
    line.append(e.scale_name());
    line.append(" - Jump ");
    append_uint(line, view.edit.edit_step);
    line.append(" - Oct ");
    append_octave(line, static_cast<int>(e.octave) + pattern_octave_shift(model));  // the octave as the Pattern view names it (device setting MIDDLE C, OS-CFGWIRE)
    line.append(e.lock != 0u ? " - Lock ON" : " - Lock off");
    r.text(x, ui::caps_y(t, Face::Label, d.body.y, pitch), line.c_str(), Color::ChromeText, Face::Label);
    r.text(x, ui::caps_y(t, Face::Label, d.body.y + 2 * pitch, pitch), "PAD ROW 1  ROOT C..B", Color::ChromeDim, Face::Label);
    r.text(x, ui::caps_y(t, Face::Label, d.body.y + 3 * pitch, pitch), "PAD ROW 2  SCALE (ENCODER: ALL 39)", Color::ChromeDim, Face::Label);  // OS-SCALES28 S2: the encoder and Up / Down walk all 39 scales
    r.text(x, ui::caps_y(t, Face::Label, d.body.y + 4 * pitch, pitch), "PAD ROW 3  JUMP 0..11", Color::ChromeDim, Face::Label);
    core::FixedString<48> octaves;
    octaves.assign("PAD ROW 4  OCTAVE ");
    append_octave(octaves, pattern_octave_shift(model));
    octaves.append("..");
    append_octave(octaves, 9 + pattern_octave_shift(model));
    octaves.append(", LOCK OFF / ON");
    r.text(x, ui::caps_y(t, Face::Label, d.body.y + 5 * pitch, pitch), octaves.c_str(), Color::ChromeDim, Face::Label);
    return;
  }

  const int total = is_instr ? static_cast<int>(model::kMaxInstruments) : static_cast<int>(model::fx::kPickerCount);
  const int desc_rows = is_fx ? kFxDescRows : 0;
  const int list_rows = rows - 1 - desc_rows;  // (step_popup_list_rows)
  const int cursor = is_instr ? (view.instrument < 1 ? 0 : view.instrument - 1) : (pop.item < 0 ? 0 : pop.item);
  const int first = first_row(cursor, total, list_rows);
  const model::Project* project = model.project;
  for (int k = 0; k < list_rows && first + k < total; ++k) {
    const int index = first + k;
    const int y = d.body.y + k * pitch;
    const bool sel = index == cursor;
    const bool dim = is_instr && !step_popup_pickable(model, view, static_cast<unsigned>(index));  // OS-INSTPICK: not a sample, on a sample screen
    const Color ink = sel ? Color::SelText : (dim ? Color::ChromeDim : Color::ChromeText);
    if (sel) {
      r.panel({d.body.x, y, d.body.w, pitch}, Color::Sel);
    }
    core::FixedString<48> text;
    if (is_instr) {
      append_dec(text, static_cast<unsigned>(index + 1), 2);
      text.append(" ");
      const model::Instrument* in = project != nullptr ? project->instrument(static_cast<std::uint32_t>(index)) : nullptr;
      core::FixedString<40> label;
      step_popup_instrument_text(in, label);  // (OS-INSTRMIDI: a MIDI instrument is "MIDI CH n")
      append_clipped(text, in != nullptr ? label.c_str() : (project == nullptr ? instrument_name(index + 1) : ""), static_cast<std::size_t>(inner / cell - 3));
    } else {
      const char type[2] = {fx_type_char(model::fx::kPickerOrder[index]), '\0'};
      text.assign(type);
      text.append(" ");
      char name[kFxNameMax];
      fx_picker_name(static_cast<unsigned>(index), name, sizeof name);
      text.append(name);
    }
    r.text(x, ui::caps_y(t, Face::Data, y, pitch), text.c_str(), ink, Face::Data);
  }
  if (is_fx) {
    const int y = d.body.y + list_rows * pitch;
    r.rule(d.body.x, y, d.body.w, ui::Axis::Horizontal, Color::ChromeDim);
    char desc[kFxDescMax];
    fx_picker_desc(static_cast<unsigned>(cursor), desc, sizeof desc);
    draw_wrapped(r, x, y, pitch, inner, desc_rows, desc, Color::ChromeText);
  }
}

constexpr StepPopupOverlay kStepPopupOverlay{};

}  // namespace o3os::app
