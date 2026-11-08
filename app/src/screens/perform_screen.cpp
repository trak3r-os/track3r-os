// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/perform_screen.h>

#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/pad_lights.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/model/project.h>
#include <o3os/ui/components.h>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

constexpr int kCellPitches = 3;  // a row of a slot is three rows of the pane tall: a pad's worth of target

struct PerformLayout {
  Frame frame;
  PatternLayout grid;
  int col_w;
  int x0;
  int name_y;   // the slot's number and FX name
  int cells_y;  // the first of the four rows
  int cell_h;
  int tracks_y;
  int track_w;
};

PerformLayout make_layout(const ui::Theme& theme) noexcept {
  const ui::Metrics& m = theme.metrics;
  PerformLayout l{};
  l.frame = make_frame(theme, kFrameRows);
  l.grid = make_pattern_layout(theme, kFrameRows, PatternView::kVisibleTracks);
  l.col_w = (ui::kScreenW - 2 * m.border) / static_cast<int>(seq::kPerfSlots);
  l.x0 = (ui::kScreenW - l.col_w * static_cast<int>(seq::kPerfSlots)) / 2;
  const int pitch = l.frame.row_pitch;
  l.name_y = l.frame.pane.y;
  l.cells_y = l.frame.pane.y + 2 * pitch;
  l.cell_h = kCellPitches * pitch;
  l.tracks_y = l.cells_y + static_cast<int>(seq::kPerfRows) * l.cell_h;
  l.track_w = (ui::kScreenW - 2 * m.pad) / static_cast<int>(seq::kPerfTracks);
  return l;
}

// OS-DJ A1: the DJ page. Twelve boxes (the tracks, coloured by group), the two groups by name, the curve and the two gains, and the crossfader as the big bar at the bottom.
void render_dj(const Model& model, const ViewState& view, Renderer& r, Rect covered, const PerformLayout& l) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Frame& f = l.frame;
  const DjState& dj = view.dj;
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const int pitch = f.row_pitch;
  const int left = m.pad;
  const int width = ui::kScreenW - 2 * m.pad;

  draw_chrome_bands(r, f, covered);
  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    strip.row().put(Face::Display, "PERFORM", Color::ChromeText);
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    strip.row().field("PAGE", Face::Data, "DJ", Color::ChromeDim, Color::ChromeText);
    strip.row().chip(Face::Data, dj.curve == DjCurve::Cut ? "CUT" : "SMOOTH", Color::Accent, Color::AccentText);
    // A4: the tempo the sequencer says while it plays (the nudge is in it), else the project's; a chip while an arrow nudges it.
    core::FixedString<8> bpm;
    append_bpm(bpm, view.play.playing && view.play.tempo_x100 != 0u ? view.play.tempo_x100 : (model.project != nullptr ? model.project->tempo_x100() : 0u));
    strip.row().field("BPM", Face::Data, bpm.c_str(), Color::ChromeDim, Color::ChromeText);
    if (dj.nudge_dir() != 0) {
      strip.row().chip(Face::Data, dj.nudge_dir() < 0 ? "NUDGE -4%" : "NUDGE +4%", Color::Accent, Color::AccentText);
    }
  }
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    r.text(m.index_w, cap_y, "DJ", Color::ChromeText, Face::Label);
    r.panel({m.index_w, f.header.y + f.header.h - 2 * m.border, r.text_width("DJ", Face::Label), 2 * m.border}, Color::Sel);
    const char* note = "encoder: crossfader   pads: 1 pattern, 4 jump   hold Shift: groups";
    r.text(ui::kScreenW - 2 * m.pad - r.text_width(note, Face::Label), cap_y, note, Color::ChromeDim, Face::Label);
  }

  // The tracks, one box each: T, and A / B / THRU.
  const int track_w = width / static_cast<int>(kDjTracks);
  for (unsigned tr = 0; tr < kDjTracks; ++tr) {
    const Rect box = {left + static_cast<int>(tr) * track_w + 2, f.pane.y + 2, track_w - 4, 2 * pitch - 4};
    if (hidden(box)) {
      continue;
    }
    const DjGroup g = dj.group_of(tr);
    r.panel(box, g == DjGroup::A ? Color::Play : (g == DjGroup::B ? Color::Accent : Color::Sel));
    const Color ink = g == DjGroup::A ? Color::PlayText : (g == DjGroup::B ? Color::AccentText : Color::SelText);
    core::FixedString<8> name;
    name.assign("T");
    append_uint(name, tr + 1u);
    r.text(box.x + (box.w - r.text_width(name.c_str(), Face::Label)) / 2, ui::caps_y(t, Face::Label, box.y, box.h / 2), name.c_str(), ink, Face::Label);
    const char* word = g == DjGroup::A ? "A" : (g == DjGroup::B ? "B" : "THRU");
    r.text(box.x + (box.w - r.text_width(word, Face::Label)) / 2, ui::caps_y(t, Face::Label, box.y + box.h / 2, box.h / 2), word, ink, Face::Label);
  }
  // The pattern row (A4): NOW and NEXT, the bank, then the twelve patterns of the bank under the pads' first row: the one that plays in the play colour, the queued one in the accent colour, the ones the project lacks dim.
  const Rect now_row = {left, f.pane.y + 2 * pitch, width, pitch};
  if (!hidden(now_row)) {
    const int text_y = ui::caps_y(t, Face::Label, now_row.y, now_row.h);
    core::FixedString<24> now;
    now.assign("NOW  PAT ");
    append_uint(now, view.dj_now());
    r.text(left, text_y, now.c_str(), Color::Text, Face::Label);
    core::FixedString<40> next;
    next.assign("NEXT  ");
    if (view.edit.queued_pattern != 0u) {
      next.append("PAT ");
      append_uint(next, view.edit.queued_pattern);
    } else {
      next.append("-");
    }
    if (view.dj.trans.kind != DjTrans::None) {  // B2: the transition in force, with how far the sweep is (ECHO: armed until the last beat)
      const DjTransition& tr = view.dj.trans;
      next.append(tr.kind == DjTrans::Hp ? "  (TRANS HP " : (tr.kind == DjTrans::Lp ? "  (TRANS LP " : "  (TRANS ECHO"));
      if (tr.kind == DjTrans::Echo) {
        next.append(tr.echo ? ")" : " ARMED)");
      } else {
        append_uint(next, tr.pct);
        next.append(" %)");
      }
    }
    r.text(ui::kScreenW / 3, text_y, next.c_str(), view.edit.queued_pattern != 0u || view.dj.trans.kind != DjTrans::None ? Color::Accent : Color::TextDim, Face::Label);
    core::FixedString<24> bank;
    bank.assign("BANK ");
    append_uint(bank, view.dj_bank() + 1u);
    r.text(left + width - r.text_width(bank.c_str(), Face::Label), text_y, bank.c_str(), Color::Text, Face::Label);
  }
  const Rect pats_row = {left, f.pane.y + 3 * pitch, width, pitch};
  if (!hidden(pats_row)) {
    const int text_y = ui::caps_y(t, Face::Label, pats_row.y, pats_row.h);
    for (unsigned col = 0; col < kDjBankSize; ++col) {
      const unsigned id = view.dj_bank() * kDjBankSize + col + 1u;
      if (id > 255u) {
        continue;
      }
      core::FixedString<8> num;
      num.assign(id < 10u ? "0" : "");
      append_uint(num, id);
      const bool exists = model.project != nullptr && model.project->pattern(id) != nullptr;
      const Color ink = id == view.edit.queued_pattern ? Color::Accent : (id == view.dj_now() ? Color::Play : (exists ? Color::Text : Color::TextDim));
      r.text(left + static_cast<int>(col) * track_w + (track_w - r.text_width(num.c_str(), Face::Label)) / 2, text_y, num.c_str(), ink, Face::Label);
    }
  }
  // The groups by name (the LEDs cannot say which is which), the curve and the two gains.
  for (unsigned side = 0; side < 2u; ++side) {
    const DjGroup g = side == 0u ? DjGroup::A : DjGroup::B;
    core::FixedString<48> line;
    line.assign(side == 0u ? "GROUP A  tracks" : "GROUP B  tracks");
    unsigned n = 0;
    for (unsigned tr = 0; tr < kDjTracks; ++tr) {
      if (dj.group_of(tr) == g) {
        line.append(" ");
        append_uint(line, tr + 1u);
        ++n;
      }
    }
    if (n == 0u) {
      line.append(" none");
    }
    const Rect row = {side == 0u ? left : ui::kScreenW / 2, f.pane.y + 4 * pitch, ui::kScreenW / 2 - m.pad, pitch};
    if (!hidden(row)) {
      r.text(row.x, ui::caps_y(t, Face::Label, row.y, row.h), line.c_str(), Color::Text, Face::Label);
    }
  }
  const unsigned pa = dj_level(DjGroup::A, dj.pos, dj.curve) * 100u / 255u;
  const unsigned pb = dj_level(DjGroup::B, dj.pos, dj.curve) * 100u / 255u;
  core::FixedString<48> gains;
  gains.assign("A ");
  append_uint(gains, pa);
  gains.append("%   B ");
  append_uint(gains, pb);
  gains.append("%   THRU tracks: never cut");
  const Rect gain_row = {left, f.pane.y + 5 * pitch, width, pitch};
  if (!hidden(gain_row)) {
    r.text(gain_row.x, ui::caps_y(t, Face::Label, gain_row.y, gain_row.h), gains.c_str(), Color::TextDim, Face::Label);
  }

  // The row 2 FX in force (OS-DJ B1), one line a group under the gains, and nothing while none is: "A  REP 1/16  ECHO  CUT".
  const Rect fx_row = {left, f.pane.y + 6 * pitch, width, pitch};
  if (!hidden(fx_row) && dj.fx_on() != 0u) {
    static constexpr const char* kName[kDjFxKinds] = {"REP 1/4", "REP 1/8", "REP 1/16", "REP 1/32", "ECHO", "CUT"};
    for (unsigned side = 0; side < 2u; ++side) {
      const DjGroup g = side == 0u ? DjGroup::A : DjGroup::B;
      core::FixedString<48> line;
      line.assign(side == 0u ? "A" : "B");
      DjFx rep = DjFx::Rep4;
      const bool has_rep = dj.rep_pick(g, rep);
      for (unsigned k = 0; k < kDjFxKinds; ++k) {
        if (dj.fx_in_force(g, static_cast<DjFx>(k)) && (k > static_cast<unsigned>(DjFx::Rep32) || (has_rep && static_cast<DjFx>(k) == rep))) {
          line.append("  ");
          line.append(kName[k]);
        }
      }
      r.text(side == 0u ? left : ui::kScreenW / 2, ui::caps_y(t, Face::Label, fx_row.y, fx_row.h), line.c_str(), Color::Accent, Face::Label);
    }
  }
  // The filter (OS-DJ A2): FILTER and its value (OFF, LP 50, HP 30; LATCH after it), then LP on the left and HP on the right of the trough, the knob where the value is (the centre is bypassed).
  const Rect filt_row = {left, f.pane.y + 7 * pitch, width, pitch};
  if (!hidden(filt_row)) {
    const int text_y = ui::caps_y(t, Face::Label, filt_row.y, filt_row.h);
    const int gap = m.pad;
    const int value_x = left + r.text_width("FILTER", Face::Label) + 2 * gap;
    const int lp_x = value_x + r.text_width("HP 100  LATCH", Face::Label) + 2 * gap;
    const int trough_x = lp_x + r.text_width("LP", Face::Label) + gap;
    const int hp_x = left + width - r.text_width("HP", Face::Label);
    const int trough_w = hp_x - gap - trough_x;
    const int filt = dj.filt_now();  // (a TRANS HP / LP sweep moves the filter: the bar follows it, the hand's value is what it returns to)
    const bool on = filt != 0 || dj.filt_pad;
    r.text(left, text_y, "FILTER", on ? Color::Text : Color::TextDim, Face::Label);
    core::FixedString<24> value;
    if (filt == 0) {
      value.assign("OFF");
    } else {
      value.assign(filt < 0 ? "LP " : "HP ");
      append_uint(value, static_cast<unsigned>(filt < 0 ? -filt : filt));
    }
    if (dj.latch) {
      value.append("  LATCH");
    }
    r.text(value_x, text_y, value.c_str(), filt == 0 ? Color::TextDim : Color::Text, Face::Label);
    r.text(lp_x, text_y, "LP", Color::TextDim, Face::Label);
    r.text(hp_x, text_y, "HP", Color::TextDim, Face::Label);
    r.panel({trough_x, filt_row.y + filt_row.h / 2 - m.border, trough_w, 2 * m.border}, Color::Sel);
    r.panel({trough_x + (trough_w - m.border) / 2, filt_row.y + 3 * m.border, m.border, filt_row.h - 6 * m.border}, Color::TextDim);
    const int knob_w = track_w / 4;
    const int knob_x = trough_x + (filt + kDjFiltRange) * (trough_w - knob_w) / (2 * kDjFiltRange);
    r.panel({knob_x, filt_row.y + 2 * m.border, knob_w, filt_row.h - 4 * m.border}, filt == 0 ? Color::Text : Color::Accent);
  }

  // The kills (OS-DJ A3): the three bands of the master EQ, the one in force in the accent colour.
  const Rect kill_row = {left, f.pane.y + 8 * pitch, width, pitch};
  if (!hidden(kill_row)) {
    const int text_y = ui::caps_y(t, Face::Label, kill_row.y, kill_row.h);
    const int gap = m.pad;
    static constexpr const char* kBand[3] = {"LOW", "MID", "HI"};
    int x = left;
    r.text(x, text_y, "KILL", dj.kill() != 0u ? Color::Text : Color::TextDim, Face::Label);
    x += r.text_width("KILL", Face::Label) + 2 * gap;
    for (unsigned b = 0; b < kDjKills; ++b) {
      const bool killed = ((dj.kill() >> b) & 1u) != 0u;
      r.text(x, text_y, kBand[b], killed ? Color::Accent : Color::TextDim, Face::Label);
      x += r.text_width(kBand[b], Face::Label) + 2 * gap;
    }
    if (dj.kill_latched != 0u) {
      r.text(x, text_y, "LATCHED", Color::TextDim, Face::Label);
    }
  }

  // The TAPE STOP's length (OS-DJSTOPLEN), between the A and the B above the crossfader, while the pad is held: the encoder sets it.
  const Rect stop_row = {left, f.pane.y + 9 * pitch, width, pitch};
  if (dj.scr == DjScr::Tape && !hidden(stop_row)) {
    core::FixedString<32> stop;
    stop.assign("TAPE STOP  ");
    stop.append(kDjStopLenName[dj.stop_len < kDjStopLens ? dj.stop_len : kDjStopLenDefault]);
    stop.append(dj.stop_len > kDjStopLenDefault ? " BEATS" : " BEAT");
    r.text(left + (width - r.text_width(stop.c_str(), Face::Label)) / 2, ui::caps_y(t, Face::Label, stop_row.y, stop_row.h), stop.c_str(), Color::Accent, Face::Label);
  }

  // The crossfader: A on the left, B on the right, the knob where the position is.
  const Rect bar = {left, f.pane.y + 10 * pitch, width, 4 * pitch};
  if (!hidden(bar) || !hidden({left, bar.y - pitch, width, pitch})) {
    r.text(left, ui::caps_y(t, Face::Label, bar.y - pitch, pitch), "A", Color::Play, Face::Label);
    r.text(left + width - r.text_width("B", Face::Label), ui::caps_y(t, Face::Label, bar.y - pitch, pitch), "B", Color::Accent, Face::Label);
    r.panel(bar, Color::Sel);  // the frame, the trough inside it, the centre line, the knob
    r.panel({bar.x + 3 * m.border, bar.y + 3 * m.border, bar.w - 6 * m.border, bar.h - 6 * m.border}, Color::Pane);
    r.panel({bar.x + (bar.w - 2 * m.border) / 2, bar.y + 3 * m.border, 2 * m.border, bar.h - 6 * m.border}, Color::TextDim);
    const int knob_w = 2 * track_w / 3;
    const int knob_x = bar.x + static_cast<int>(dj.pos) * (bar.w - knob_w) / static_cast<int>(kDjTravel);
    r.panel({knob_x, bar.y + 6, knob_w, bar.h - 12}, dj.pos == kDjCentre ? Color::Text : Color::Accent);
  }
  if (!hidden(f.info)) {
    draw_info_line(r, f, dj.pos == kDjCentre ? "crossfader centred" : (dj.pos < kDjCentre ? "crossfader towards A" : "crossfader towards B"), "hold FILTER + encoder: filter   arrows: nudge, Shift: tempo");
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    const std::uint32_t hot = (dj.curve == DjCurve::Cut ? 1u << 2 : 0u) | (dj.filt != 0 || dj.filt_pad ? 1u << 3 : 0u) | (dj.latch ? 1u << 5 : 0u) | (dj.scr != DjScr::Off ? 1u << 4 : 0u);
    ui::soft_keys(r, f.keys, key_row(kPerformScreen, view, kPerformDjSoftKeys), 8, 0u, hot);
  }
}

}  // namespace

Context PerformScreen::field_context(const Model&, const ViewState& view) const noexcept {
  return view.dj_page() ? Context::PerformDj : Context::None;
}

const char* const* PerformScreen::shift_row(const ViewState& view) const noexcept {
  return view.dj_page() ? kPerformDjShiftKeys : soft_keys_shifted();
}

bool PerformScreen::pad_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) const noexcept {
  if (view.dj_page()) {  // OS-DJ A1: row 4 is the fader, the pad nearest the position Front; the FILT pad (A2) is Back, Front while the filter is away from 0; the other pads are the later slices'
    for (unsigned p = 0; p < 48u; ++p) {
      roles[p] = p < 36u ? PadRole::Off : (dj_pad_lit(p - 36u, view.dj.pos) ? PadRole::Front : PadRole::Back);
    }
    roles[kDjFiltPad] = view.dj.filt != 0 || view.dj.filt_pad ? PadRole::Front : PadRole::Back;
    for (unsigned k = 0; k < 3u; ++k) {  // B2: the TRANS pads, the one in force Front
      roles[kDjTransPad + k] = static_cast<unsigned>(view.dj.trans.kind) == k + 1u ? PadRole::Front : PadRole::Back;
    }
    roles[kDjTapePad] = view.dj.scr == DjScr::Tape ? PadRole::Front : PadRole::Back;  // C1: TAPE STOP, Front while it is held
    dj_pattern_roles(model, view, true, roles);  // A4: the pattern row (the App blinks the queued one)
    for (unsigned b = 0; b < kDjKills; ++b) {  // the kills (A3): Back, Front while the band is killed
      roles[kDjKillPad + b] = ((view.dj.kill() >> b) & 1u) != 0u ? PadRole::Front : PadRole::Back;
    }
    for (unsigned c = 0; c < kDjFxPads; ++c) {  // row 2 (B1): Back, Front while the FX is in force (the held pad, or a latched one)
      roles[kDjFxPad + c] = ((view.dj.fx_on() >> c) & 1u) != 0u ? PadRole::Front : PadRole::Back;
    }
    return true;
  }
  if (model.perform == nullptr) {
    return false;
  }
  const seq::PerformBank& bank = model.perform->bank;
  for (unsigned p = 0; p < seq::kPerfPads; ++p) {
    roles[p] = PadRole::Off;
  }
  for (unsigned s = 0; s < seq::kPerfSlots; ++s) {
    if (bank.fx(s) == seq::PerfFx::None || bank.cc_mode(s)) {
      continue;  // OS-MIDICCMAP S3: no pad of a slot in CC mode is lit
    }
    const unsigned pad = bank.row(s) * seq::kPerfSlots + s;
    roles[pad] = bank.held(s) ? PadRole::Front : PadRole::Back;
  }
  return true;
}

void PerformScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const PerformLayout l = make_layout(t);
  const Frame& f = l.frame;
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const seq::PerformBank* bank = model.perform != nullptr ? &model.perform->bank : nullptr;
  const int data_w = t.geometry(Face::Data).cell_w;
  const int data_h = t.geometry(Face::Data).cell_h;
  const int label_w = t.geometry(Face::Label).cell_w;

  if (view.dj_page()) {
    render_dj(model, view, r, covered, l);
    return;
  }
  draw_chrome_bands(r, f, covered);

  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    strip.row().put(Face::Display, "PERFORM", Color::ChromeText);
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    if (bank != nullptr) {
      core::FixedString<8> text;
      unsigned n = 0;
      for (unsigned tr = 0; tr < seq::kPerfTracks; ++tr) {
        n += bank->affected(tr) ? 1u : 0u;
      }
      append_uint(text, n);
      strip.row().field("TRACKS", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
      if (bank->edit()) {
        strip.row().chip(Face::Data, "EDIT", Color::Accent, Color::AccentText);
      }
    }
    strip.cpu(model.load);
  }
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    r.text(m.index_w, cap_y, "SLOTS", Color::ChromeText, Face::Label);
    r.panel({m.index_w, f.header.y + f.header.h - 2 * m.border, r.text_width("SLOTS", Face::Label), 2 * m.border}, Color::Sel);
    const char* note = bank == nullptr ? "no engine: the slots are not wired"
                       : (bank->latch() ? "pads: slot x row   encoder: the held value   latched until another row" : "pads: slot x row   encoder: the held value   LATCH off: acts while held");
    r.text(ui::kScreenW - 2 * m.pad - r.text_width(note, Face::Label), cap_y, note, Color::ChromeDim, Face::Label);
  }

  if (bank != nullptr) {
    const bool edit = bank->edit();
    // The slots: number and FX over four rows of target. The selected row of a slot is lit (green; red in EDIT), the others are dark.
    for (unsigned s = 0; s < seq::kPerfSlots; ++s) {
      const int x = l.x0 + static_cast<int>(s) * l.col_w;
      const seq::PerfFx fx = bank->fx(s);
      const seq::PerfFxInfo& info = seq::perf_info(fx);
      const bool is_edit_slot = edit && s == bank->edit_slot();
      const Rect head = {x + 1, l.name_y, l.col_w - 2, 2 * f.row_pitch};
      if (!hidden(head)) {
        if (is_edit_slot) {
          r.panel(head, Color::Accent);
        }
        core::FixedString<8> num;
        append_uint(num, s + 1u);
        const Color ink = is_edit_slot ? Color::AccentText : (fx == seq::PerfFx::None || !info.live ? Color::TextDim : Color::Text);
        r.text(x + (l.col_w - static_cast<int>(num.size()) * label_w) / 2, ui::caps_y(t, Face::Label, l.name_y, f.row_pitch), num.c_str(), is_edit_slot ? Color::AccentText : Color::TextDim, Face::Label);
        const int nw = r.text_width(info.name, Face::Label);
        r.text(x + (l.col_w - nw) / 2, ui::caps_y(t, Face::Label, l.name_y + f.row_pitch, f.row_pitch), info.name, ink, Face::Label);
      }
      for (unsigned row = 0; row < seq::kPerfRows; ++row) {
        const Rect cell = {x + 2, l.cells_y + static_cast<int>(row) * l.cell_h + 2, l.col_w - 4, l.cell_h - 4};
        if (hidden(cell)) {
          continue;
        }
        const bool sel = fx != seq::PerfFx::None && bank->row(s) == row;
        const bool on = sel && bank->active(s);
        const bool held = sel && bank->held(s);
        Color fill = Color::Pane;
        Color ink = Color::TextDim;
        if (sel) {
          fill = edit ? Color::Accent : (on ? Color::Play : Color::Sel);
          ink = edit ? Color::AccentText : (on ? Color::PlayText : Color::SelText);
        }
        r.panel(cell, fill);
        if (held) {
          r.panel({cell.x, cell.y, cell.w, m.border * 2}, ink);  // a pad is down in this slot: a bar over its row
        }
        char text[8];
        // row 0 is "off"; the others show the value (the live one on the selected row, the stored one otherwise); a slot in CC mode tags its selected cell "CC"
        // (OS-MIDICCMAP S3: the mapped CC drives the slot, no pad of it is lit)
        if (fx == seq::PerfFx::None) {
          text[0] = '-';
          text[1] = '-';
          text[2] = '-';
          text[3] = '\0';
        } else if (bank->cc_mode(s) && sel) {
          text[0] = 'C';
          text[1] = 'C';
          text[2] = '\0';
        } else if (row == 0u) {
          text[0] = 'O';
          text[1] = 'F';
          text[2] = 'F';
          text[3] = '\0';
        } else {
          seq::perf_value_text(fx, sel ? bank->live(s) : bank->stored(s, row), text);
        }
        unsigned n = 0;
        while (text[n] != '\0') {
          ++n;
        }
        r.text(cell.x + (cell.w - static_cast<int>(n) * data_w) / 2, cell.y + (cell.h - data_h) / 2 + l.grid.cell_dy - (f.row_pitch - data_h) / 2, text, sel ? ink : (row == 0u ? Color::TextDim : Color::Text));
      }
    }
    // The tracks the layer affects: a box each, filled when it does; M when the track is muted.
    for (unsigned tr = 0; tr < seq::kPerfTracks; ++tr) {
      const Rect box = {m.pad + static_cast<int>(tr) * l.track_w + 2, l.tracks_y + 2, l.track_w - 4, 2 * f.row_pitch - 4};
      if (hidden(box)) {
        continue;
      }
      const bool on = bank->affected(tr);
      const bool muted = model.project != nullptr && model.project->track(tr) != nullptr && (model.project->track(tr)->flags & model::kTrackMute) != 0u;
      r.panel(box, muted ? Color::Accent : (on ? Color::Play : Color::Sel));
      core::FixedString<8> name;
      name.assign("T");
      append_uint(name, tr + 1u);
      const Color ink = muted ? Color::AccentText : (on ? Color::PlayText : Color::SelText);
      r.text(box.x + (box.w - r.text_width(name.c_str(), Face::Label)) / 2, ui::caps_y(t, Face::Label, box.y, box.h / 2), name.c_str(), ink, Face::Label);
      const char* state = muted ? "MUTED" : (on ? "ON" : "OFF");
      // E-PERF-07: a track on another pattern shows it ("07"); one that waits for its loop end shows the pattern it goes to, blinking (">07", ">SG" for the song's)
      const std::uint32_t pw = view.play.playing ? view.track_pat[tr] : 0u;
      core::FixedString<8> pat;
      if (!muted && on && ((pw & 0x10000u) != 0u || (pw & 0xFFu) != 0u)) {
        const bool waits = (pw & 0x10000u) != 0u;
        const unsigned id = waits ? (pw >> 8) & 0xFFu : pw & 0xFFu;
        if (!waits || view.blink) {
          if (waits) {
            pat.assign(">");
          }
          if (id == 0u) {
            pat.append("SG");
          } else {
            pat.append(id < 10u ? "0" : "");
            append_uint(pat, id);
          }
          state = pat.c_str();
        }
      }
      r.text(box.x + (box.w - r.text_width(state, Face::Label)) / 2, ui::caps_y(t, Face::Label, box.y + box.h / 2, box.h / 2), state, ink, Face::Label);
    }
  }

  if (!hidden(f.info)) {
    core::FixedString<80> left;
    const char* right = "hold Shift: TRACKS  hold F1-F3: store the scene  Rec: edit";
    if (bank == nullptr) {
      left.assign("no engine");
    } else if (bank->edit()) {
      left.assign("EDIT: slot ");
      append_uint(left, bank->edit_slot() + 1u);
      left.append(" ");
      left.append(seq::perf_info(bank->fx(bank->edit_slot())).name);
      right = "left / right: slot   up / down: FX   encoder: value   Copy / Shift + Copy";
    } else {
      unsigned n = 0;
      for (unsigned tr = 0; tr < seq::kPerfTracks; ++tr) {
        n += bank->affected(tr) ? 1u : 0u;
      }
      left.assign(n == 0u ? "no track is on: hold Shift + top pads" : "FX act on the tracks that are on");  // (OS-BUG-PERFINFO: 37 cells of 8 px; the hint on the right begins at about cell 40, the 55 cells of the old text ran into it)
    }
    draw_info_line(r, f, left.c_str(), right);
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    std::uint32_t dim = 0;
    std::uint32_t hot = 0;
    if (bank != nullptr) {
      for (unsigned i = 0; i < seq::kPerfScenes; ++i) {
        dim |= bank->scene_stored(i) ? 0u : 1u << i;
        hot |= bank->scene_matches(i) ? 1u << i : 0u;
      }
      hot |= bank->rate() == 1u ? 1u << 3 : (bank->rate() == 2u ? 1u << 4 : 0u);
      hot |= bank->latch() ? 1u << 5 : 0u;
      hot |= view.play.playing && view.play.queued != audio::PlayPosition::kNoOrder && view.blink ? 1u << 6 : 0u;  // NEXT blinks while it waits for the pattern's end
    }
    ui::soft_keys(r, f.keys, key_row(*this, view, kPerformSoftKeys), 8, dim, hot);  // the marks stay while Shift is held: the scene in force, the latch
  }
}

constexpr PerformScreen kPerformScreen{};

}  // namespace o3os::app
