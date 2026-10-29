// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/mixer_screen.h>

#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/audio/graph.h>
#include <o3os/ui/components.h>

#include <cstring>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

namespace tp = model::track_param;
namespace mp = model::master_param;

constexpr const char* kPageNames[kMixerPages] = {"MIXER", "ROUTING", "MASTER FX", "STEMS"};
constexpr const char* kBusNames[kMixBuses] = {"IN", "SNDA", "SNDB", "CUE", "MST"};

// The value of a model parameter as the project holds it, or the descriptor's default without one (the pages then say there is no project).
float value_of(const Model& model, core::ParamId id) noexcept {
  if (model.project != nullptr && model.project->has_param(id)) {
    return model.project->get_param(id);
  }
  const core::ParamDesc* d = model::param_desc(id);
  return d != nullptr ? d->def : 0.0f;
}

core::ParamId track_id(unsigned slot, unsigned field) noexcept { return core::param_id(core::ParamKind::Track, slot, field); }
core::ParamId master_id(unsigned field) noexcept { return core::param_id(core::ParamKind::Master, 0, field); }

// Writes a decimal into a char buffer (NUL terminated).
void append_dec_to(char* out, std::size_t cap, unsigned v) noexcept {
  core::FixedString<8> s;
  append_uint(s, v);
  std::strncpy(out, s.c_str(), cap - 1u);
  out[cap - 1u] = '\0';
}

unsigned whole(float v) noexcept { return v <= 0.0f ? 0u : (v >= 100.0f ? 100u : static_cast<unsigned>(v + 0.5f)); }

// What the board says of a strip's routing; the engine's default without a board.
std::uint8_t route_of(const Model& model, unsigned strip) noexcept {
  if (model.mix != nullptr && strip < kMixStrips) {
    return model.mix->route[strip];
  }
  return strip == audio::kInputStrip ? static_cast<std::uint8_t>(audio::route::kMain | audio::route::kCue)
                                     : static_cast<std::uint8_t>(audio::route::kMain | audio::route::kUsb);
}

// The level shown for a column of the MIXER page (0..100) and the pan (a track's only).
unsigned level_of(const Model& model, unsigned column) noexcept {
  if (mixer::is_track_column(model, column)) {
    return whole(value_of(model, track_id(column, tp::Volume)));
  }
  switch (static_cast<MixBus>(mixer::bus_of(model, column))) {
    case MixBus::In: return model.mix != nullptr ? model.mix->in_level : 0u;
    case MixBus::SndA: return whole(value_of(model, master_id(mp::Delay)));
    case MixBus::SndB: return whole(value_of(model, master_id(mp::Reverb)));
    case MixBus::Cue: return model.mix != nullptr ? model.mix->cue_level : 100u;
    case MixBus::Master: return whole(value_of(model, master_id(mp::Volume)));
    case MixBus::Count: break;
  }
  return 0;
}

// Mute and solo marks of a column: a track's flags, the DELAY / REVERB mute of the send buses (the master's DRY MUTE belongs to no column).
bool muted(const Model& model, unsigned column) noexcept {
  if (mixer::is_track_column(model, column)) {
    return value_of(model, track_id(column, tp::Mute)) != 0.0f;
  }
  switch (static_cast<MixBus>(mixer::bus_of(model, column))) {
    case MixBus::SndA: return value_of(model, master_id(mp::DelayMute)) != 0.0f;
    case MixBus::SndB: return value_of(model, master_id(mp::ReverbMute)) != 0.0f;
    default: return false;
  }
}
bool soloed(const Model& model, unsigned column) noexcept {
  return mixer::is_track_column(model, column) && value_of(model, track_id(column, tp::Solo)) != 0.0f;
}

// The engine channels (audio::meter_ch) a column's meters read: one, or the two sides of a stereo bus.
struct Channels {
  unsigned n;
  unsigned ch[2];
};
Channels channels_of(const Model& model, unsigned column) noexcept {
  if (mixer::is_track_column(model, column)) {
    return {1, {audio::meter_ch::kTrack0 + column, 0}};
  }
  switch (static_cast<MixBus>(mixer::bus_of(model, column))) {
    case MixBus::In: return {2, {audio::meter_ch::kInL, audio::meter_ch::kInR}};
    case MixBus::SndA: return {1, {audio::meter_ch::kSndA, 0}};
    case MixBus::SndB: return {1, {audio::meter_ch::kSndB, 0}};
    case MixBus::Cue: return {2, {audio::meter_ch::kCueL, audio::meter_ch::kCueR}};
    case MixBus::Master: return {2, {audio::meter_ch::kMasterL, audio::meter_ch::kMasterR}};
    case MixBus::Count: break;
  }
  return {0, {0, 0}};
}

// The MIXER page's geometry from the theme: the pane's rows, a column's width, the fader and the meters.
struct MixLayout {
  Frame frame;
  PatternLayout grid;
  unsigned cols;
  int col_w;
  int x0;
  int label_y;   // top of the caption row
  int meter_y;   // top of the fader and the meters
  int meter_h;
  int level_y;   // the rows of numbers: level, pan, marks, routing dots
  int pan_y;
  int mark_y;
  int dots_y;
  int fader_w;
  int bar_w;     // one meter's width
  int gap;
};

MixLayout make_mix_layout(const ui::Theme& theme, unsigned cols) noexcept {
  const ui::Metrics& m = theme.metrics;
  MixLayout l{};
  l.frame = make_frame(theme, kFrameRows);
  l.grid = make_pattern_layout(theme, kFrameRows, PatternView::kVisibleTracks);
  l.cols = cols;
  l.col_w = static_cast<int>(cols) > 0 ? (ui::kScreenW - 2 * m.pad) / static_cast<int>(cols) : 0;
  if (l.col_w > 56) {
    l.col_w = 56;
  }
  l.x0 = (ui::kScreenW - l.col_w * static_cast<int>(cols)) / 2;
  const int pitch = l.frame.row_pitch;
  const Rect& pane = l.frame.pane;
  l.label_y = pane.y;
  l.meter_y = pane.y + pitch + m.pad;
  l.level_y = pane.y + 12 * pitch;
  l.pan_y = pane.y + 13 * pitch;
  l.mark_y = pane.y + 14 * pitch;
  l.dots_y = pane.y + 15 * pitch;
  l.meter_h = l.level_y - l.meter_y - m.pad;
  l.fader_w = 2 * m.gap + 2;
  l.bar_w = 2 * m.gap + 4;
  l.gap = m.gap;
  return l;
}

}  // namespace

// ---- the cursor --------------------------------------------------------------------------------------------------------------

void MixerScreen::enter(const Model& model, ViewState& view) const noexcept {
  const mixer::Cursor c = mixer::cursor(model, view);
  view.mixer.row = static_cast<std::uint8_t>(c.row);
  view.mixer.col = static_cast<std::uint8_t>(c.col);
}

core::ParamId MixerScreen::focus_param(const Model& model, const ViewState& view) const noexcept { return mixer::focus(model, view); }

int MixerScreen::count(const Model& model, const ViewState& view) const noexcept { return static_cast<int>(mixer::rows(model, view)); }
int MixerScreen::position(const ViewState& view) const noexcept { return view.mixer.row; }

bool MixerScreen::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int last = static_cast<int>(mixer::rows(model, view)) - 1;
  const int to = position < 0 ? 0 : (position > last ? last : position);
  const bool changed = to != view.mixer.row;
  view.mixer.row = static_cast<std::uint8_t>(to);
  return changed;
}

Context MixerScreen::field_context(const Model&, const ViewState& view) const noexcept { return mixer::page_of(view) == MixerPage::Stems ? Context::MixerStems : Context::None; }

bool MixerScreen::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  const mixer::Cursor now = mixer::cursor(model, view);
  if (mixer::page_of(view) == MixerPage::Stems && now.row == 0u && dx != 0 && dy == 0 && model.mix != nullptr && model.mix->stems != nullptr && model.mix->request_outputs != nullptr) {
    // OS-USBOUTS: on the OUTPUTS row Left / Right edit the pending value (the columns are for the slot rows), as the encoder does (mixer_actions.cpp outputs_turn)
    const std::uint8_t cur = static_cast<std::uint8_t>(model.mix->stems->channels());
    const std::uint8_t base = view.mixer.outputs_pending != 0u ? view.mixer.outputs_pending : cur;
    const std::uint8_t next = mixer::outputs_next(base, dx);
    const std::uint8_t pending = next == cur ? 0u : next;
    const bool changed = pending != view.mixer.outputs_pending;
    view.mixer.outputs_pending = pending;
    view.mixer.reset_asked = changed ? 0u : view.mixer.reset_asked;
    return changed;
  }
  int col_max = 1;
  switch (mixer::page_of(view)) {
    case MixerPage::Mixer: col_max = static_cast<int>(mixer::columns(model)); break;
    case MixerPage::Routing: col_max = static_cast<int>(kRouteCols); break;
    case MixerPage::MasterFx: col_max = 1; break;
    case MixerPage::Stems: col_max = static_cast<int>(kStemSources); break;
  }
  int row = static_cast<int>(now.row) + dy;
  int col = static_cast<int>(now.col) + dx;
  const int row_last = static_cast<int>(mixer::rows(model, view)) - 1;
  row = row < 0 ? 0 : (row > row_last ? row_last : row);
  col = col < 0 ? 0 : (col > col_max - 1 ? col_max - 1 : col);
  view.mixer.row = static_cast<std::uint8_t>(row);
  view.mixer.col = static_cast<std::uint8_t>(col);
  // The cursor lands where the page allows (a bus has no pan): the view says what was asked, cursor() what holds
  const mixer::Cursor held = mixer::cursor(model, view);
  const bool changed = held.row != now.row || held.col != now.col;
  view.mixer.row = static_cast<std::uint8_t>(held.row);
  view.mixer.col = static_cast<std::uint8_t>(held.col);
  return changed;
}

// cursor.track: whole columns on the MIXER page.
bool MixerScreen::track(const Model& model, ViewState& view, int delta) const noexcept {
  return step(model, view, delta, 0);
}

// ---- the MIXER page ----------------------------------------------------------------------------------------------------------

namespace {

void draw_mixer_page(const Model& model, const ViewState& view, Renderer& r, Rect covered, const MixLayout& l) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const mixer::Cursor cur = mixer::cursor(model, view);
  const int pitch = l.frame.row_pitch;
  const int cell_h = t.geometry(Face::Data).cell_h;
  const int cell_w = t.geometry(Face::Data).cell_w;
  const int cell_dy = l.grid.cell_dy;
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const auto col_x = [&](unsigned c) { return l.x0 + static_cast<int>(c) * l.col_w; };
  const unsigned tracks = mixer::tracks(model);
  const int inner_w = l.fader_w + l.gap + 2 * l.bar_w + 2;
  const auto strip_x = [&](unsigned c) { return col_x(c) + (l.col_w - inner_w) / 2; };

  // The selected column's background, then the faders and the meters.
  const Rect sel = {col_x(cur.col), l.frame.pane.y, l.col_w, 16 * pitch};
  if (!hidden(sel)) {
    r.panel(sel, Color::Sel);
  }
  for (unsigned c = 0; c < l.cols; ++c) {
    const Rect fader = {strip_x(c), l.meter_y, l.fader_w, l.meter_h};
    if (hidden(fader)) {
      continue;
    }
    r.panel(fader, Color::Empty);
    const int fill = static_cast<int>(level_of(model, c)) * l.meter_h / 100;
    if (fill > 0) {
      r.panel({fader.x, fader.y + l.meter_h - fill, fader.w, fill}, muted(model, c) ? Color::TextDim : Color::Text);
    }
  }
  // Meters: one bank, drawn in colour passes (ui/plot/meters.h). A column has one or two bars beside its fader.
  if (model.mix != nullptr && !hidden({0, l.meter_y, ui::kScreenW, l.meter_h})) {
    constexpr unsigned kMaxBars = kMixTracks + 2u * kMixBuses;
    ui::Rect rects[kMaxBars];
    ui::plot::MeterState states[kMaxBars];
    unsigned n = 0;
    for (unsigned c = 0; c < l.cols && n + 2u <= kMaxBars; ++c) {
      const Channels ch = channels_of(model, c);
      for (unsigned k = 0; k < ch.n; ++k) {
        rects[n] = {strip_x(c) + l.fader_w + l.gap + static_cast<int>(k) * (l.bar_w + 2), l.meter_y, l.bar_w, l.meter_h};
        states[n] = model.mix->meter[ch.ch[k]];
        ++n;
      }
    }
    ui::plot::MeterStyle style;
    style.slot = Color::Empty;
    ui::plot::draw_meters(r, rects, states, n, ui::Axis::Vertical, model.mix->scale, style);
  }

  // The captions, numbers and marks.
  core::FixedString<8> text;
  for (unsigned c = 0; c < l.cols; ++c) {
    const bool is_sel = c == cur.col;
    const int x = col_x(c);
    const bool track = mixer::is_track_column(model, c);
    text.clear();
    if (track) {
      text.assign("T");
      append_uint(text, c + 1u);
    } else {
      text.assign(kBusNames[mixer::bus_of(model, c)]);
    }
    const int cap_y = ui::caps_y(t, Face::Label, l.label_y, pitch);
    const int cap_w = r.text_width(text.c_str(), Face::Label);
    if (!hidden({x, l.label_y, l.col_w, pitch})) {
      r.text(x + (l.col_w - cap_w) / 2, cap_y, text.c_str(), is_sel ? Color::SelText : (muted(model, c) ? Color::TextDim : Color::Text), Face::Label);
    }
    // the level, with the cursor block when it is the field
    text.clear();
    append_uint(text, level_of(model, c));
    const int vx = x + (l.col_w - static_cast<int>(text.size()) * cell_w) / 2;
    const bool on_level = is_sel && cur.row == 0u;
    if (on_level) {
      const Rect block = {vx - l.grid.cursor_pad_x, l.level_y + l.grid.cursor_dy, static_cast<int>(text.size()) * cell_w + 2 * l.grid.cursor_pad_x, l.grid.cursor_h};
      if (!hidden(block)) {
        r.panel(block, Color::Accent);
      }
    }
    if (!hidden({vx, l.level_y + cell_dy, 3 * cell_w, cell_h})) {
      r.text(vx, l.level_y + cell_dy, text.c_str(), on_level ? Color::AccentText : (is_sel ? Color::SelText : Color::Text));
    }
    // the pan (tracks)
    if (track) {
      char buf[core::kParamTextChars + 1];
      const core::ParamDesc* d = model::param_desc(track_id(c, tp::Pan));
      (void)core::param_format(*d, value_of(model, track_id(c, tp::Pan)), buf, sizeof buf);
      const int px = x + (l.col_w - static_cast<int>(std::strlen(buf)) * cell_w) / 2;
      const bool on_pan = is_sel && cur.row == 1u;
      if (on_pan) {
        const Rect block = {px - l.grid.cursor_pad_x, l.pan_y + l.grid.cursor_dy, static_cast<int>(std::strlen(buf)) * cell_w + 2 * l.grid.cursor_pad_x, l.grid.cursor_h};
        if (!hidden(block)) {
          r.panel(block, Color::Accent);
        }
      }
      if (!hidden({px, l.pan_y + cell_dy, 4 * cell_w, cell_h})) {
        r.text(px, l.pan_y + cell_dy, buf, on_pan ? Color::AccentText : (is_sel ? Color::SelText : Color::TextDim));
      }
    }
    // M and S marks: filled badges when on
    const int badge = t.geometry(Face::Label).cap_h + 2 * (m.pad / 2);
    const int bx = x + (l.col_w - (2 * badge + l.gap)) / 2;
    const int by = l.mark_y + (pitch - badge) / 2;
    if (muted(model, c) && !hidden({bx, by, badge, badge})) {
      r.panel({bx, by, badge, badge}, Color::Accent);
      r.text(bx + (badge - t.geometry(Face::Label).cell_w) / 2, ui::caps_y(t, Face::Label, by, badge), "M", Color::AccentText, Face::Label);
    }
    if (soloed(model, c) && !hidden({bx + badge + l.gap, by, badge, badge})) {
      r.panel({bx + badge + l.gap, by, badge, badge}, Color::Play);
      r.text(bx + badge + l.gap + (badge - t.geometry(Face::Label).cell_w) / 2, ui::caps_y(t, Face::Label, by, badge), "S", Color::PlayText, Face::Label);
    }
    // the routing: one dot a destination (a track's or the input's; the buses have none of their own)
    if (track || mixer::bus_of(model, c) == static_cast<unsigned>(MixBus::In)) {
      const std::uint8_t route = route_of(model, track ? c : audio::kInputStrip);
      const int dot = m.gap + 1;
      const int dots_w = static_cast<int>(kRouteCols) * dot + static_cast<int>(kRouteCols - 1u) * m.border * 2;
      int dx = x + (l.col_w - dots_w) / 2;
      const int dy = l.dots_y + (pitch - dot) / 2;
      for (unsigned k = 0; k < kRouteCols; ++k) {
        if (!hidden({dx, dy, dot, dot})) {
          r.panel({dx, dy, dot, dot}, ((route >> k) & 1u) != 0u ? (is_sel ? Color::SelText : Color::Text) : Color::Empty);
        }
        dx += dot + m.border * 2;
      }
    }
  }
  (void)tracks;
}

// ---- the ROUTING page --------------------------------------------------------------------------------------------------------

// What a row of the matrix is: a source (a track or the line input), an effect return (it reaches MAIN and CUE), or OUT MODE.
enum class RowKind : std::uint8_t { Track, Input, Return, OutMode };
struct RowInfo {
  RowKind kind;
  unsigned index;  // the track, or the return 0 / 1
};
RowInfo row_info(const Model& model, unsigned row) noexcept {
  const unsigned n = mixer::tracks(model);
  if (row < n) {
    return {RowKind::Track, row};
  }
  if (row == n) {
    return {RowKind::Input, 0};
  }
  if (row == n + 1u || row == n + 2u) {
    return {RowKind::Return, row - n - 1u};
  }
  return {RowKind::OutMode, 0};
}

// A matrix cell: a filled square when the route is on, a small dim dot when it is off (two words either way); the cursor's cell sits on the accent block.
void draw_cell(Renderer& r, const Rect& cell, int mark, bool on, bool cursor, bool row_selected) noexcept {
  const int size = on ? mark : mark / 2;
  const Rect box = {cell.x + (cell.w - size) / 2, cell.y + (cell.h - size) / 2, size, size};
  if (cursor) {
    r.panel({cell.x + (cell.w - mark) / 2 - 3, cell.y + (cell.h - mark) / 2 - 3, mark + 6, mark + 6}, Color::Accent);
  }
  r.panel(box, cursor ? Color::AccentText : (on ? (row_selected ? Color::SelText : Color::Text) : (row_selected ? Color::SelText : Color::Empty)));
}

struct MatrixLayout {
  int name_x;
  int lvl_x;
  int cell_x0;
  int cell_w;
  int meter_x;
  int meter_w;
};
MatrixLayout make_matrix_layout(const ui::Theme& t) noexcept {
  const ui::Metrics& m = t.metrics;
  const int cell_w = t.geometry(Face::Data).cell_w;
  MatrixLayout l{};
  l.name_x = m.index_w + 2 * m.gap;
  l.lvl_x = l.name_x + 13 * cell_w;
  l.cell_x0 = l.lvl_x + 3 * cell_w + 4 * m.gap;
  l.cell_w = 6 * cell_w + 2 * m.gap;
  l.meter_x = l.cell_x0 + static_cast<int>(kRouteCols) * l.cell_w + 2 * m.gap;
  l.meter_w = ui::kScreenW - l.meter_x - 2 * m.pad;
  return l;
}

void draw_routing_page(const Model& model, const ViewState& view, Renderer& r, Rect covered, const MixLayout& l) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const mixer::Cursor cur = mixer::cursor(model, view);
  const MatrixLayout ml = make_matrix_layout(t);
  const Frame& f = l.frame;
  const int cell_h = t.geometry(Face::Data).cell_h;
  const int cell_w = t.geometry(Face::Data).cell_w;
  const unsigned rows = mixer::rows(model, view);
  const unsigned visible = static_cast<unsigned>(kFrameRows);
  const unsigned top = cur.row + 1u > visible ? cur.row + 1u - visible : 0u;  // the window follows the cursor down
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const auto row_y = [&](unsigned i) { return f.pane.y + static_cast<int>(i - top) * f.row_pitch; };
  const int mark = m.gap + 4;
  const unsigned last = rows < top + visible ? rows : top + visible;

  const Rect bar = {0, row_y(cur.row), ui::kScreenW, f.row_pitch};
  if (!hidden(bar)) {
    r.highlight_row(bar);
  }
  // The meters of the sources, one bank: the tracks and the line input (left side). The column's rectangle holds every bar, so a hidden column hides them all.
  if (model.mix != nullptr && !hidden({ml.meter_x, f.pane.y, ml.meter_w, static_cast<int>(visible) * f.row_pitch})) {
    constexpr unsigned kMaxBars = kMixTracks + 1u;
    ui::Rect rects[kMaxBars];
    ui::plot::MeterState states[kMaxBars];
    unsigned n = 0;
    for (unsigned i = top; i < last && n < kMaxBars; ++i) {
      const RowInfo ri = row_info(model, i);
      if (ri.kind != RowKind::Track && ri.kind != RowKind::Input) {
        continue;
      }
      const unsigned ch = ri.kind == RowKind::Track ? audio::meter_ch::kTrack0 + ri.index : audio::meter_ch::kInL;
      rects[n] = {ml.meter_x, row_y(i) + (f.row_pitch - (m.meter_h + 2 * m.border)) / 2, ml.meter_w, m.meter_h + 2 * m.border};
      states[n] = model.mix->meter[ch];
      ++n;
    }
    ui::plot::MeterStyle style;
    style.slot = Color::Empty;
    ui::plot::draw_meters(r, rects, states, n, ui::Axis::Horizontal, model.mix->scale, style);
  }
  core::FixedString<24> text;
  for (unsigned i = top; i < last; ++i) {
    const RowInfo ri = row_info(model, i);
    const bool sel = i == cur.row;
    const int y = row_y(i) + l.grid.cell_dy;
    text.clear();
    unsigned level = 0;
    bool has_level = true;
    std::uint8_t route = 0;
    bool has_route = true;
    switch (ri.kind) {
      case RowKind::Track: {
        text.assign("T");
        append_uint(text, ri.index + 1u);
        const model::TrackSlot* slot = model.project != nullptr ? model.project->track(ri.index) : nullptr;
        if (slot != nullptr && slot->name[0] != '\0') {
          text.append(" ");
          append_clipped(text, slot->name, 9);
        }
        level = level_of(model, ri.index);
        route = route_of(model, ri.index);
        break;
      }
      case RowKind::Input:
        text.assign("LINE IN");
        level = level_of(model, mixer::tracks(model) + static_cast<unsigned>(MixBus::In));
        route = route_of(model, audio::kInputStrip);
        break;
      case RowKind::Return:
        text.assign(ri.index == 0u ? "RET A DELAY" : "RET B REVERB");
        level = level_of(model, mixer::tracks(model) + static_cast<unsigned>(ri.index == 0u ? MixBus::SndA : MixBus::SndB));
        route = model.mix != nullptr ? model.mix->ret_route[ri.index] : static_cast<std::uint8_t>(audio::route::kMain);
        break;
      case RowKind::OutMode:
        text.assign("OUT MODE");
        has_level = false;
        has_route = false;
        break;
    }
    const bool dim = ri.kind == RowKind::Track && muted(model, ri.index);
    if (!hidden({ml.name_x, y, 13 * cell_w, cell_h})) {
      r.text(ml.name_x, y, text.c_str(), sel ? Color::SelText : (dim ? Color::TextDim : Color::Text));
    }
    if (has_level && !hidden({ml.lvl_x, y, 3 * cell_w, cell_h})) {
      text.clear();
      append_uint(text, level);
      r.text(ml.lvl_x, y, text.c_str(), sel ? Color::SelText : Color::TextDim);
    }
    if (has_route) {
      const unsigned cols = ri.kind == RowKind::Return ? 2u : kRouteCols;
      for (unsigned c = 0; c < cols; ++c) {
        if (ri.kind == RowKind::Input && c == kRouteCols - 1u) {
          continue;  // the input has no USB column
        }
        const Rect cell = {ml.cell_x0 + static_cast<int>(c) * ml.cell_w, row_y(i), ml.cell_w, f.row_pitch};
        if (!hidden(cell)) {
          draw_cell(r, cell, mark, ((route >> c) & 1u) != 0u, sel && c == cur.col, sel);
        }
      }
    } else {
      const unsigned mode = model.mix != nullptr ? model.mix->out_mode : 0u;
      const char* name = kOutModeNames[mode < 2u ? mode : 0u];
      if (!hidden({ml.cell_x0, y, 16 * cell_w, cell_h})) {
        r.text(ml.cell_x0, y, name, sel ? Color::AccentText : Color::Text);
      }
    }
  }
}

// ---- the MASTER FX page ------------------------------------------------------------------------------------------------------

// The master section: its parameters in table order (not the LIMITER switch: OS-SMALL3), then the CUE level (the engine's, the board's), and beside them the limiter's card (the chain's reserved stages are not shown: OS-MIXHIDE).
void draw_master_page(const Model& model, const ViewState& view, Renderer& r, Rect covered, const MixLayout& l) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const mixer::Cursor cur = mixer::cursor(model, view);
  const Frame& f = l.frame;
  const int cell_h = t.geometry(Face::Data).cell_h;
  const int cell_w = t.geometry(Face::Data).cell_w;
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const int name_x = m.index_w + 2 * m.gap;
  const int value_x = name_x + (static_cast<int>(core::kParamNameChars) + 2) * cell_w;
  const int bar_x = value_x + 8 * cell_w;
  const int bar_w = 16 * cell_w;
  const int card_x = bar_x + bar_w + 4 * m.gap;
  const auto row_y = [&](unsigned i) { return f.pane.y + static_cast<int>(i) * f.row_pitch; };

  // More rows than the pane has (OS-SENDFX): the window follows the cursor, which stays on its last row once it has gone past the pane.
  const unsigned visible = static_cast<unsigned>(kFrameRows);
  const unsigned first = cur.row >= visible ? cur.row - visible + 1u : 0u;
  const Rect hl = {0, row_y(cur.row - first), card_x - 2 * m.gap, f.row_pitch};
  if (!hidden(hl)) {
    r.highlight_row(hl);
  }
  char text[core::kParamTextChars + 1];
  for (unsigned i = first; i < kMasterFxRows && i < first + visible; ++i) {
    const bool sel = i == cur.row;
    const int y = row_y(i - first) + l.grid.cell_dy;
    const char* name = "CUE LEVEL";
    unsigned level = 0;
    bool bar = false;
    const char* unit = "";
    if (i < kMasterShownParams) {
      const core::ParamId id = core::param_id(core::ParamKind::Master, 0, i);
      const core::ParamDesc* d = model::param_desc(id);
      name = d->name;
      (void)core::param_format(*d, value_of(model, id), text, sizeof text);
      bar = d->unit == core::ParamUnit::Int;
      level = bar ? whole(value_of(model, id)) : 0u;
      unit = d->unit_text;
    } else if (i == kMasterCueRow) {
      level = model.mix != nullptr ? model.mix->cue_level : 100u;
      text[0] = '\0';
      append_dec_to(text, sizeof text, level);
      bar = true;
      unit = "%";
    } else {
      // a send effect's or the limiter's control: the board's value (the table's default without a board), as text, and a bar for what has a range to show
      const SendFxRow& row = master_fx_row(i - kMasterFirstFxRow);
      const int value = model.mix != nullptr ? static_cast<int>(model.mix->control(i - kMasterFirstFxRow)) : static_cast<int>(row.def);
      name = row.name;
      (void)send_fx_text(row, value, text, sizeof text);
      unit = row.unit;
      bar = row.kind == SendFxKind::Number || row.kind == SendFxKind::Time || row.kind == SendFxKind::Rate || row.kind == SendFxKind::Ms;
      level = row.max > row.min ? static_cast<unsigned>(((value - static_cast<int>(row.min)) * 100) / (static_cast<int>(row.max) - static_cast<int>(row.min))) : 0u;
    }
    if (hidden({name_x, y, 40 * cell_w, cell_h})) {
      continue;
    }
    r.text(name_x, y, name, sel ? Color::SelText : Color::TextDim);
    r.text(value_x, y, text, sel ? Color::SelText : Color::Text);
    if (unit[0] != '\0') {
      r.text(value_x + static_cast<int>(std::strlen(text) + 1u) * cell_w, y, unit, sel ? Color::SelText : Color::TextDim);
    }
    if (bar) {
      ui::bar(r, {bar_x, row_y(i - first) + (f.row_pitch - m.meter_h) / 2, bar_w, m.meter_h}, level, 100, sel ? Color::Sel : Color::Empty, sel ? Color::SelText : Color::Text);
    }
  }
  // The limiter's card (OS-MIXHIDE): the chain's other stages are reserved slots with no DSP and no control, so the page does not show them (graph.h kMasterChain keeps them,
  // the stored values and the OG import are untouched); the limiter is the one stage that runs, always on, and its controls are the rows at the left.
  if (card_x + 16 * t.geometry(Face::Label).cell_w <= ui::kScreenW - m.pad && !hidden({card_x, f.pane.y, ui::kScreenW - card_x, 3 * f.row_pitch})) {
    int y = f.pane.y + l.grid.cell_dy;
    r.text(card_x, y, "MASTER LIMITER", Color::Text, Face::Label);
    y += f.row_pitch;
    r.text(card_x, y, "ALWAYS ON", Color::TextDim, Face::Label);
  }
}

// ---- the STEMS page ----------------------------------------------------------------------------------------------------------

constexpr const char* kStemSourceNames[kStemSources] = {"T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8", "T9", "T10", "T11", "T12", "ML", "MR", "SA", "SB"};
constexpr const char* kStemBusNames[kStemSources - 12] = {"Master L", "Master R", "Send A", "Send B"};  // the full names of the columns after the twelve tracks (the strip's SRC field)

void draw_stems_page(const Model& model, const ViewState& view, Renderer& r, Rect covered, const MixLayout& l) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const mixer::Cursor cur = mixer::cursor(model, view);
  const Frame& f = l.frame;
  const int cell_h = t.geometry(Face::Data).cell_h;
  const int cell_w = t.geometry(Face::Data).cell_w;
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const audio::StemMap* map = model.mix != nullptr ? model.mix->stems : nullptr;
  const int name_x = m.index_w + 2 * m.gap;
  const int x0 = name_x + 4 * cell_w;
  const int col_w = (ui::kScreenW - x0 - 2 * m.pad) / static_cast<int>(kStemSources);
  const int mark = m.gap + 4;
  if (map == nullptr) {
    constexpr const char* kNoMap = "NO STEM MAP: THE USB STREAM IS NOT ON THIS BUILD";
    if (!hidden({name_x, f.pane.y + l.grid.cell_dy, static_cast<int>(std::strlen(kNoMap)) * cell_w, cell_h})) {
      r.text(name_x, f.pane.y + l.grid.cell_dy, kNoMap, Color::TextDim);
    }
    return;
  }
  const unsigned slots = map->channels();
  const Rect bar = {0, f.pane.y + static_cast<int>(cur.row) * f.row_pitch, ui::kScreenW, f.row_pitch};
  if (!hidden(bar)) {
    r.highlight_row(bar);
  }
  core::FixedString<24> text;
  {
    // OS-USBOUTS: row 0, the number of USB outputs: "OUTPUTS  8", or "OUTPUTS  6*" with the pending value starred (applied by F6 twice)
    const bool sel = cur.row == 0u;
    const unsigned shown = view.mixer.outputs_pending != 0u ? view.mixer.outputs_pending : slots;
    text.assign("OUTPUTS  ");
    append_uint(text, shown);
    if (view.mixer.outputs_pending != 0u) {
      text.append("*   F6 APPLY");
    }
    const int row_y = f.pane.y;
    if (!hidden({name_x, row_y + l.grid.cell_dy, static_cast<int>(text.size()) * cell_w, cell_h})) {
      r.text(name_x, row_y + l.grid.cell_dy, text.c_str(), sel ? Color::SelText : Color::Text);
    }
  }
  for (unsigned s = 0; s < slots && s + 1u < static_cast<unsigned>(kFrameRows); ++s) {
    const bool sel = s + 1u == cur.row;
    const int row_y = f.pane.y + static_cast<int>(s + 1u) * f.row_pitch;
    text.assign("S");
    append_uint(text, s + 1u);
    if (!hidden({name_x, row_y + l.grid.cell_dy, 3 * cell_w, cell_h})) {
      r.text(name_x, row_y + l.grid.cell_dy, text.c_str(), sel ? Color::SelText : Color::Text);
    }
    const std::uint32_t mask = map->slot(s);
    for (unsigned c = 0; c < kStemSources; ++c) {
      const Rect cell = {x0 + static_cast<int>(c) * col_w, row_y, col_w, f.row_pitch};
      if (!hidden(cell)) {
        draw_cell(r, cell, mark, ((mask >> c) & 1u) != 0u, sel && c == cur.col, sel);
      }
    }
  }
}

}  // namespace

void MixerScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const MixLayout l = make_mix_layout(t, mixer::columns(model));
  const Frame& f = l.frame;
  const MixerPage page = mixer::page_of(view);
  const mixer::Cursor cur = mixer::cursor(model, view);
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };

  draw_chrome_bands(r, f, covered);

  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    strip.row().put(Face::Display, kPageNames[static_cast<unsigned>(page)], Color::ChromeText);  // the page: the matrix pages use the header for their columns
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    core::FixedString<24> text;
    switch (page) {
      case MixerPage::Mixer:
        if (mixer::is_track_column(model, cur.col)) {
          text.assign("T");
          append_uint(text, cur.col + 1u);
        } else {
          text.assign(kBusNames[mixer::bus_of(model, cur.col)]);
        }
        strip.row().field("SEL", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
        text.clear();
        append_uint(text, level_of(model, cur.col));
        strip.row().field("LEVEL", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
        break;
      case MixerPage::Routing:
        strip.row().field("OUT", Face::Data, kOutModeNames[model.mix != nullptr && model.mix->out_mode < 2u ? model.mix->out_mode : 0u], Color::ChromeDim, Color::ChromeText);
        break;
      case MixerPage::Stems:
        if (model.mix != nullptr && model.mix->stems != nullptr) {
          append_uint(text, model.mix->stems->channels());
          strip.row().field("CH", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
          // The column under the cursor in full: "ML = Master L", "T3 = Track 3".
          const unsigned col = cur.col < kStemSources ? cur.col : kStemSources - 1u;  // (the cursor is clamped to the page; belt and braces for the table)
          text.assign(kStemSourceNames[col]);
          text.append(" = ");
          if (col < 12u) {
            text.append("Track ");
            append_uint(text, col + 1u);
          } else {
            text.append(kStemBusNames[col - 12u]);
          }
          strip.row().field("SRC", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
        }
        break;
      case MixerPage::MasterFx:
        break;
    }
    strip.cpu(model.load);
  }
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    if (page == MixerPage::Mixer || page == MixerPage::MasterFx) {
      int x = m.index_w;
      for (unsigned p = 0; p < pages(); ++p) {
        r.text(x, cap_y, kPageNames[p], p == static_cast<unsigned>(page) ? Color::ChromeText : Color::ChromeDim, Face::Label);
        if (p == static_cast<unsigned>(page)) {
          r.panel({x, f.header.y + f.header.h - 2 * m.border, r.text_width(kPageNames[p], Face::Label), 2 * m.border}, Color::Sel);
        }
        x += r.text_width(kPageNames[p], Face::Label) + 4 * m.gap;
      }
    }
    // The columns' names of the matrix pages, over their columns.
    if (page == MixerPage::Routing) {
      const MatrixLayout ml = make_matrix_layout(t);
      for (unsigned c = 0; c < kRouteCols; ++c) {
        const int w = r.text_width(kRouteColNames[c], Face::Label);
        r.text(ml.cell_x0 + static_cast<int>(c) * ml.cell_w + (ml.cell_w - w) / 2, cap_y, kRouteColNames[c], Color::ChromeDim, Face::Label);
      }
      r.text(ml.lvl_x, cap_y, "LVL", Color::ChromeDim, Face::Label);
      r.text(ml.meter_x, cap_y, "METER", Color::ChromeDim, Face::Label);
    } else if (page == MixerPage::Stems) {
      const int name_x = m.index_w + 2 * m.gap;
      const int x0 = name_x + 4 * t.geometry(Face::Data).cell_w;
      const int col_w = (ui::kScreenW - x0 - 2 * m.pad) / static_cast<int>(kStemSources);
      for (unsigned c = 0; c < kStemSources; ++c) {
        const int w = r.text_width(kStemSourceNames[c], Face::Label);
        r.text(x0 + static_cast<int>(c) * col_w + (col_w - w) / 2, cap_y, kStemSourceNames[c], c == cur.col ? Color::ChromeText : Color::ChromeDim, Face::Label);
      }
    } else {
      const char* note = model.mix != nullptr ? "meters: the engine's, 32 ms windows" : "no engine: meters are off";
      if (page == MixerPage::Mixer) {
        r.text(ui::kScreenW - 2 * m.pad - r.text_width(note, Face::Label), cap_y, note, Color::ChromeDim, Face::Label);
      }
    }
  }

  switch (page) {
    case MixerPage::Mixer: draw_mixer_page(model, view, r, covered, l); break;
    case MixerPage::Routing: draw_routing_page(model, view, r, covered, l); break;
    case MixerPage::MasterFx: draw_master_page(model, view, r, covered, l); break;
    case MixerPage::Stems: draw_stems_page(model, view, r, covered, l); break;
  }

  if (!hidden(f.info)) {
    core::FixedString<80> left;
    const char* right = "";
    switch (page) {
      case MixerPage::Mixer:
        if (mixer::is_track_column(model, cur.col)) {
          left.assign("T");
          append_uint(left, cur.col + 1u);
          left.append(cur.row == 0u ? " level" : " pan");
        } else {
          left.assign(kBusNames[mixer::bus_of(model, cur.col)]);
          left.append(" level");
        }
        right = "left / right: column  up / down: field  encoder: change  F7 mute  F8 solo";
        break;
      case MixerPage::Routing: {
        const RowInfo ri = row_info(model, cur.row);
        if (ri.kind == RowKind::OutMode) {
          left.assign("OUT MODE   encoder up: main L / cue R   down: stereo");
          right = "Delete: default";  // not the cells' route hint: the left text already says what the encoder does here, and both together are wider than the line (OS-BUG-MIXINFO)
        } else {
          left.assign(ri.kind == RowKind::Return ? "return" : (ri.kind == RowKind::Input ? "line in" : "track"));
          left.append(" > ");
          left.append(kRouteColNames[cur.col]);
          right = "encoder up: route   down: no route   Delete: default";
        }
        break;
      }
      case MixerPage::MasterFx:
        left.assign(cur.row == kMasterCueRow ? "CUE LEVEL: the cue bus's level" : (cur.row < kMasterCueRow ? "the master section" : (cur.row - kMasterFirstFxRow < kSendFxDelayRows ? "SND A: the delay (new sound, the OG's controls)" : (cur.row < kMasterFirstLimRow ? "SND B: the reverb (new sound, the OG's controls)" : "LIMITER: always on (new sound, not saved)"))));
        right = "up / down: row  encoder: change  Delete: default";
        break;
      case MixerPage::Stems: {
        const audio::StemMap* map = model.mix != nullptr ? model.mix->stems : nullptr;
        right = "Insert: add  Delete: remove  Shift+Delete: factory map  encoder: add/remove";
        if (map == nullptr) {
          left.assign("no stem map");
        } else if (cur.row == 0u) {
          // OS-USBOUTS: the OUTPUTS row: what the number does, and what is waiting
          right = "Left/Right: choose  F6 twice: apply  Delete: drop";
          left.assign(view.mixer.outputs_pending != 0u ? "OUTPUTS: USB reconnects, map to factory" : "OUTPUTS: channels to the computer");
        } else {
          // "S3 -> USB ch 3 = T3+T9": the slot, where it goes and what is in it. The hint on the right takes ~600 px of the line (the column's full name is the strip's SRC field), so
          // what the left side cannot spare is cut from the list of sources (the one text that grows), with a ".." at the cut.
          left.assign("S");
          append_uint(left, cur.row);
          left.append(" -> USB ch ");
          append_uint(left, cur.row);
          left.append(" = ");
          char buf[40];
          std::size_t n = audio::format_sources(map->slot(cur.row - 1u), buf, sizeof buf);
          const int room = ui::kScreenW - 4 * m.pad - r.text_width(right, Face::Label) - 2 * m.gap - r.text_width(left.c_str(), Face::Label);
          if (r.text_width(buf, Face::Label) > room) {
            while (n > 0u && r.text_width(buf, Face::Label) + r.text_width("..", Face::Label) > room) {
              buf[--n] = '\0';
            }
            if (n > 0u) {
              buf[n] = '.';
              buf[n + 1u] = '.';
              buf[n + 2u] = '\0';
            }
          }
          left.append(buf);
        }
        break;
      }
    }
    if (model.project == nullptr && page != MixerPage::Stems) {
      left.append("   no project");  // (the Mixer page's hint takes 576 px of the line: the longer "no project: defaults" ran into it, OS-BUG-MIXINFO)
    }
    draw_info_line(r, f, left.c_str(), right);
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    ui::soft_keys(r, f.keys, key_row(*this, view, page == MixerPage::Stems ? kMixerStemKeys : kMixerSoftKeys), 8);
  }
}

constexpr MixerScreen kMixerScreen{};

}  // namespace o3os::app
