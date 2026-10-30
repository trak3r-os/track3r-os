// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/song_screen.h>

#include "song_pages.h"

#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/song_stage.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>
#include <o3os/ui/theme.h>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

constexpr int kRows = SongView::kVisibleRows;
constexpr int kStepsPerBar = 16;   // a bar is four beats of four steps (the pattern view's bar rule)
constexpr int kBarGraphMax = 128;  // steps that fill the arrangement's bar graphic
constexpr unsigned kDemoPatternSteps = 16;  // a pattern-mode playback with no project (the demo list): one bar

// Two decimal digits in a hex tile: a position or a pattern number past 99 (a project's list has up to 255) shows its low two, as append_dec does.
constexpr unsigned bcd(int n) noexcept { return static_cast<unsigned>((((n % 100) / 10) << 4) | (n % 10)); }
// The digits a count or a position takes in the strip and on the info line: two, three past 99.
constexpr unsigned digits_of(int n) noexcept { return n > 99 ? 3u : 2u; }
constexpr int bars_of(int steps) noexcept { return (steps + kStepsPerBar - 1) / kStepsPerBar; }
// OS-SONGSHARE S1: the mark of a pattern that two or more slots hold, in the gap after NAME (Q4a); the count is on the info line.
constexpr const char* kSharedMark = "*";

// The columns of both pages, derived from the theme: the data cell, the pair tile, the gap. The bands are the frame's
// (frame.h) and the cursor block the pattern grid's (pattern_layout.h), so the two screens line up.
struct SongLayout {
  Frame frame;
  PatternLayout grid;  // the pattern layout at the same rows: the cursor block, the cell offset
  int hex_w;           // a two-digit tile
  int marker_x;        // the play marker's icon, centred in the row-number column
  int pos_x;
  int pat_x;
  int name_x;          // order page
  int len_x;
  int mark_x;          // OS-SONGSHARE S1: the shared-pattern mark, in the gap between NAME and LEN (the Label face)
  int mark_w;
  int mark_h;
  int start_x;         // arrangement page
  int bars_x;
  int graph_x;
  int graph_w;
  int graph_h;
  int map_order_x;     // the track map (song_map.h): one cell per track, at the right of the page's columns
  int map_arr_x;
  int map_pitch;
  int map_w;           // a cell
  int map_h;
  bool fits;
};

SongLayout make_song_layout(const ui::Theme& theme) noexcept {
  const ui::Metrics& m = theme.metrics;
  const int cell_w = theme.geometry(Face::Data).cell_w;
  SongLayout l{};
  l.frame = make_frame(theme, kRows);
  l.grid = make_pattern_layout(theme, kRows, PatternView::kVisibleTracks);
  l.hex_w = 2 * cell_w;
  l.marker_x = (m.index_w - static_cast<int>(ui::kIconSize)) / 2;
  l.pos_x = m.index_w + 2 * m.gap;
  l.pat_x = l.pos_x + l.hex_w + 4 * m.gap;
  l.name_x = l.pat_x + l.hex_w + 4 * m.gap;
  l.len_x = l.name_x + kSongNameChars * cell_w + 4 * m.gap;
  l.mark_x = l.name_x + kSongNameChars * cell_w + m.gap;
  l.mark_w = theme.geometry(Face::Label).cell_w;
  l.mark_h = theme.geometry(Face::Label).cell_h;
  l.start_x = l.name_x;
  l.bars_x = l.start_x + 3 * cell_w + 4 * m.gap;
  l.graph_x = l.bars_x + 3 * cell_w + 4 * m.gap;
  l.graph_w = 360;
  l.graph_h = 2 * m.meter_h;
  l.map_pitch = cell_w + m.gap;  // a cell as wide as a character of the Data face, a gap between
  l.map_w = cell_w;
  l.map_h = (m.row_pitch * 2) / 3;
  l.map_order_x = l.len_x + 3 * cell_w + 6 * m.gap;
  l.map_arr_x = l.graph_x + l.graph_w + 2 * m.gap;
  l.fits = l.len_x + 3 * cell_w <= ui::kScreenW && l.graph_x + l.graph_w <= ui::kScreenW && 4 * m.gap >= cell_w && l.mark_x + l.mark_w <= l.len_x &&  // (a fourth digit of the start bar fits the gap; the shared mark fits the gap before LEN)
           l.grid.strip.h == l.frame.strip.h && l.grid.info.y == l.frame.info.y;
  return l;
}

// How many cells of the map a page draws: the project's active tracks (at most the model's) when they fit at the right of the page's columns, else none (render and the cursor ask the same: OS-SONGMAP).
int map_cells_of(const SongLayout& l, bool arrangement, const Model& model) noexcept {
  int cells = model.project != nullptr ? static_cast<int>(model.project->active_tracks()) : 0;
  cells = cells > static_cast<int>(SongTrackMap::kTracks) ? static_cast<int>(SongTrackMap::kTracks) : cells;
  const int x = arrangement ? l.map_arr_x : l.map_order_x;
  return x + cells * l.map_pitch > ui::kScreenW ? 0 : cells;
}

// The page in view; the pages that draw the order list (and its track map) are ORDER and ARRANGEMENT, the others draw a pane of their own (song_pages.cpp).
constexpr SongPage page_of(const ViewState& view) noexcept { return static_cast<SongPage>(view.page[screen_index(ScreenId::Song)]); }
constexpr bool list_page(const ViewState& view) noexcept { return page_of(view) == SongPage::Order || page_of(view) == SongPage::Arrangement; }

// The colour of a row's text: the selected row is all SelText (the cursor's own field AccentText, chosen by the caller).
constexpr Color ink_of(bool selected, Color normal) noexcept { return selected ? Color::SelText : normal; }

}  // namespace

void SongScreen::enter(const Model& model, ViewState& view) const noexcept { (void)song_sync(model, view); }

// The cells of the map the cursor can enter: those the page draws, none for an empty song or no project (the map is only there while it is drawn).
int song_map_cells(const Model& model, const ViewState& view) noexcept {
  if (model.project == nullptr || SongSource(model).count() <= 0) {
    return 0;
  }
  if (!list_page(view)) {
    return 0;  // TIMELINE: no list, no map
  }
  const bool arrangement = view.page[screen_index(ScreenId::Song)] == static_cast<std::uint8_t>(SongPage::Arrangement);
  return map_cells_of(make_song_layout(ui::theme(view.theme)), arrangement, model);
}

bool song_sync(const Model& model, ViewState& view) noexcept {
  const bool fitted = view.song.fit(SongSource(model).count());
  if (!view.song.in_map() && !view.song.track_selecting()) {
    return fitted;  // (no column to keep inside the map: the layout is not asked)
  }
  return view.song.fit_cols(song_map_cells(model, view)) || fitted;
}

Context SongScreen::field_context(const Model&, const ViewState& view) const noexcept { return song_question_live(view) ? Context::SongAsk : Context::None; }

bool SongScreen::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  if (page_of(view) == SongPage::Timeline) {
    return (dx + dy) != 0 && set_position(model, view, position(view) + dx + dy);  // OS-SONG-TIMELINE: the slots run along the page, so Left / Right walk them as Up / Down do
  }
  if (dx == 0) {
    return dy != 0 && set_position(model, view, position(view) + dy);
  }
  return view.song.step_col(dx, song_map_cells(model, view));  // OS-SONGMAP: Left / Right walk PAT and the tracks of the map (the selection collapses either way)
}

bool SongScreen::pad_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) const noexcept {
  static_assert(SongView::kPadSlots == static_cast<int>(sizeof(roles) / sizeof(roles[0])), "one pad a slot of the page");
  if (!view.padmode.song_slots()) {
    return false;
  }
  const SongSource src(model);
  const int count = src.count();
  const int cursor = view.song.clamped_pos(count);  // (no copy of the view: this runs every pass in SLOTS mode)
  const int base = view.song.pad_base(count);
  const bool running = view.playing || view.play.playing;
  const int marker = src.marker(view.play);
  const int queued = view.play.playing && view.play.queued != audio::PlayPosition::kNoOrder ? static_cast<int>(view.play.queued) : -1;
  for (int pad = 0; pad < SongView::kPadSlots; ++pad) {
    const int slot = base + pad;
    PadRole role = slot < count ? PadRole::Back : PadRole::Off;
    if (slot < count && (slot == marker || (!running && slot == cursor))) {
      role = PadRole::Front;
    }
    if (slot < count && slot == queued) {
      role = view.blink ? PadRole::Front : PadRole::Back;  // the jump that waits blinks (view.blink is the screen's own phase, App::poll_play_position)
    }
    roles[pad] = role;
  }
  return true;
}

void SongScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const SongLayout l = make_song_layout(t);
  const Frame& f = l.frame;
  const SongSource src(model);
  const int length = src.count();
  SongView sv = view.song;
  (void)sv.fit(length);  // drawn inside the list whatever the view says: the project may have changed since the cursor last moved
  const bool timeline = page_of(view) == SongPage::Timeline;
  const bool lists = list_page(view);  // the TIMELINE page has a pane of its own: it draws no rows, cursor block or map
  const int map_cells = lists ? map_cells_of(l, view.page[screen_index(ScreenId::Song)] == static_cast<std::uint8_t>(SongPage::Arrangement), model) : 0;
  (void)sv.fit_cols(length > 0 ? map_cells : 0);  // the column inside the map as it is drawn
  const bool in_map = sv.in_map();
  const int marker = src.marker(view.play);
  // OS-SONGSHARE S1: how many slots hold a pattern (the view's cache, rebuilt when the order list changes). The cursor's pattern, when another slot holds it too, shows in the other slots' PAT tiles in a second ink (D6, Fx).
  const auto holders = [&](unsigned id) { return model.project != nullptr ? view.song.use.get(*model.project, id) : 0u; };
  const unsigned cursor_pattern = length > 0 ? src.pattern(sv.pos()) : 0u;
  const unsigned cursor_uses = holders(cursor_pattern);
  // The slot a live jump waits for (Rec while playing) blinks its position and pattern in the playing colour until the pattern ends and the song gets there.
  const int queued = view.play.playing && view.play.queued != audio::PlayPosition::kNoOrder ? static_cast<int>(view.play.queued) : -1;
  // The tempo the strip says: the sequencer's while it plays (a Tempo FX moves it), else the project's, the demo's fixed one without a project.
  const unsigned tempo_x100 = view.play.playing && view.play.tempo_x100 != 0u ? view.play.tempo_x100 : (model.project != nullptr ? model.project->tempo_x100() : PatternView::kDemoTempoX100);
  const bool arrangement = view.page[screen_index(ScreenId::Song)] == static_cast<std::uint8_t>(SongPage::Arrangement);
  const int cell_h = t.geometry(Face::Data).cell_h;
  const int cell_w = t.geometry(Face::Data).cell_w;
  const int visible = !lists ? 0 : (length - sv.top() < kRows ? length - sv.top() : kRows);
  const int slot = sv.pos() - sv.top();
  const int cursor_slot = slot < 0 ? 0 : (slot >= kRows ? kRows - 1 : slot);  // inside the window whatever the view says
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const auto row_y = [&](int i) { return f.pane.y + i * f.row_pitch; };
  const auto cell_y = [&](int i) { return row_y(i) + l.grid.cell_dy; };

  // Chrome bands, then the selected row and the cursor on the pane (as the pattern grid does).
  draw_chrome_bands(r, f, covered);
  // The selected range (Shift + Up / Down) is every row of it highlighted; the cursor's block is on the cursor's row only.
  const auto selected = [&](int i) { return i == cursor_slot || (sv.selecting() && sv.in_range(sv.top() + i)); };
  for (int i = 0; i < visible; ++i) {
    const Rect bar = {0, row_y(i), ui::kScreenW, f.row_pitch};
    if (length > 0 && selected(i) && !hidden(bar)) {  // an empty list has nothing selected
      r.highlight_row(bar);
    }
  }
  const Rect block = {l.pat_x - l.grid.cursor_pad_x, row_y(cursor_slot) + l.grid.cursor_dy, l.hex_w + 2 * l.grid.cursor_pad_x,
                       l.grid.cursor_h};
  const int o = l.grid.cursor_outline;
  const Rect outline = {block.x - o, block.y - o, block.w + 2 * o, block.h + 2 * o};
  if (lists && length > 0 && !in_map && !hidden(outline)) {  // (in the map the cursor is a cell's: the PAT block is drawn as unselected)
    r.panel(outline, Color::AccentText);
    r.panel(block, Color::Accent);
  }

  // The track map (OS-SONG1): a cell per active track of each row's pattern, filled where the track has a note, dim where it has only other data, nothing where it is empty (a faint tick in the map). The maps come
  // from the cache of the view (song_map.h), keyed by the pattern's generation: a frame reads a table, it scans nothing. The demo list has no patterns to look into: no map.
  // OS-SONGMAP: the cursor may be in it. The cell under the cursor is the cursor's (an Accent box with the PAT block's outline, one gap wider than the cell) and the cells of a selected rectangle of slots x tracks
  // are on an Accent band, one rectangle a row; what a selected cell holds is drawn in AccentText on it (a note the cell's height, other data half of it).
  const int map_x = arrangement ? l.map_arr_x : l.map_order_x;
  const int pad = m.gap / 2;
  const int t0 = in_map ? sv.tfirst() : 0;
  const int t1 = in_map ? sv.tlast() : -1;
  const bool band = in_map && (sv.selecting() || sv.track_selecting()) && !(sv.first() == sv.last() && t0 == t1);
  const auto in_rect = [&](int i, int track) { return in_map && track >= t0 && track <= t1 && sv.in_range(sv.top() + i) && (sv.selecting() ? true : i == cursor_slot); };
  for (int i = 0; i < visible && map_cells > 0; ++i) {
    const int y = row_y(i) + (f.row_pitch - l.map_h) / 2;
    if (band && in_rect(i, t0)) {
      const Rect b = {map_x + t0 * l.map_pitch - pad, y - pad, (t1 - t0) * l.map_pitch + l.map_w + 2 * pad, l.map_h + 2 * pad};
      if (!hidden(b)) {
        r.panel(b, Color::Accent);
      }
    }
  }
  if (in_map && length > 0 && map_cells > 0) {
    const Rect cell = {map_x + sv.track() * l.map_pitch, row_y(cursor_slot) + (f.row_pitch - l.map_h) / 2, l.map_w, l.map_h};
    const Rect box = {cell.x - pad, cell.y - pad, cell.w + 2 * pad, cell.h + 2 * pad};
    const Rect frame = {box.x - o, box.y - o, box.w + 2 * o, box.h + 2 * o};
    if (!hidden(frame)) {
      r.panel(frame, Color::AccentText);
      r.panel(box, Color::Accent);
    }
  }
  for (int i = 0; i < visible && map_cells > 0; ++i) {
    const int pos = sv.top() + i;
    const std::uint32_t map = view.song.map.get(*model.project, src.pattern(pos));
    const int y = row_y(i) + (f.row_pitch - l.map_h) / 2;
    for (int track = 0; track < map_cells; ++track) {
      const SongTrackMap::Cell cell = SongTrackMap::cell(map, static_cast<unsigned>(track));
      const Rect rect = {map_x + track * l.map_pitch, y, l.map_w, l.map_h};
      if (hidden(rect)) {
        continue;
      }
      if (cell == SongTrackMap::Empty) {
        // OS-BUG-SONGMAPCUR: with the cursor in the map an empty cell is a faint tick, so that every track the cursor reaches has a cell (the empty tracks at the right drew no map and the cursor floated over
        // blank space). A cell on the band, and the cursor's own, need none.
        if (in_map && !in_rect(i, track)) {
          r.panel(Rect{rect.x, rect.y + l.map_h / 2 - 1, rect.w, 2}, Color::Empty);
        }
        continue;
      }
      if (in_rect(i, track)) {
        const Rect ink = cell == SongTrackMap::Note ? rect : Rect{rect.x, rect.y + l.map_h / 4, rect.w, l.map_h / 2};
        r.panel(ink, Color::AccentText);
      } else {
        r.panel(rect, cell == SongTrackMap::Note ? (selected(i) ? Color::SelText : Color::Instr) : Color::TextDim);
      }
    }
  }

  // The arrangement's bars: the pattern's length against the longest the graphic holds.
  if (arrangement) {
    for (int i = 0; i < visible; ++i) {
      const Rect area = {l.graph_x, row_y(i) + (f.row_pitch - l.graph_h) / 2, l.graph_w, l.graph_h};
      if (!hidden(area)) {
        const bool sel = selected(i);
        const int pos = sv.top() + i;
        ui::bar(r, area, static_cast<unsigned>(src.steps(pos)), kBarGraphMax, sel ? Color::Sel : Color::Empty,
                sel ? Color::SelText : (src.exists(pos) ? Color::Instr : Color::TextDim));
      }
    }
  }

  if (timeline) {
    draw_song_timeline(r, f, model, view, covered);
  }

  // The status strip: the title in the Display face, the transport badge, fields in the Data face, CPU at the right edge.
  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    ui::FieldRow& row = strip.row();
    core::FixedString<24> text;
    row.put(Face::Display, "SONG", Color::ChromeText);
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    append_dec(text, static_cast<unsigned>(length), digits_of(length));
    row.field("LEN", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    text.clear();
    append_dec(text, static_cast<unsigned>(sv.pos()), digits_of(length));
    row.field("POS", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    text.clear();
    if (marker >= 0) {  // the song row that plays (a document stopped has none)
      append_dec(text, static_cast<unsigned>(marker), digits_of(length));
    } else {
      text.assign("--");
    }
    row.field("PLAY", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    text.clear();
    if (length > 0) {
      append_dec(text, src.pattern(sv.pos()), digits_of(static_cast<int>(src.pattern(sv.pos()))));
      text.append(" ");
      text.append(src.name(sv.pos()));
    } else {
      text.assign("--");  // an empty list
    }
    row.field("PAT", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    text.clear();
    append_bpm(text, tempo_x100);
    row.field("BPM", Face::Data, text.c_str(), sv.tempo_focus() ? Color::AccentText : Color::ChromeDim, sv.tempo_focus() ? Color::AccentText : Color::ChromeText);  // the encoder is on it (F7)
    strip.cpu(model.load);
  }

  // The header: the columns' names (the cursor's column bright, with the selection underline) and the page at the right.
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    const auto caption = [&](int x, const char* s, bool cursor_column) {
      r.text(x, cap_y, s, cursor_column ? Color::ChromeText : Color::ChromeDim, Face::Label);
    };
    const int under_x = in_map ? map_x : l.pat_x - l.grid.cursor_pad_x;  // the selection underline of the cursor's column
    const int under_w = in_map ? map_cells * l.map_pitch - m.gap : l.hex_w + 2 * l.grid.cursor_pad_x;
    if (lists) {
      r.panel({under_x, f.header.y + f.header.h - 2 * m.border, under_w, 2 * m.border}, Color::Sel);
      caption(l.pos_x, "POS", false);
      caption(l.pat_x, "PAT", !in_map);
      if (arrangement) {
        caption(l.start_x, "START", false);
        caption(l.bars_x, "BARS", false);
        caption(l.graph_x, "LENGTH", false);
      } else {
        caption(l.name_x, "NAME", false);
        caption(l.len_x, "LEN", false);
      }
      if (map_cells > 0) {
        caption(map_x, "TRACKS", in_map);
      }
    }
    core::FixedString<24> page;
    page.assign(arrangement ? "ARRANGEMENT 2/3" : (timeline ? "TIMELINE 3/3" : "ORDER 1/3"));
    r.text(ui::kScreenW - 2 * m.pad - r.text_width(page.c_str(), Face::Label), cap_y, page.c_str(), Color::ChromeText,
           Face::Label);
  }

  // The info line and the key row.
  if (!hidden(f.info)) {
    core::FixedString<64> text;
    // OS-SONGSHARE S1: "pattern 03 in 4 slots", for the cursor's slot when another slot holds its pattern too (the order page's line and the arrangement's).
    const auto append_uses = [&]() {
      if (cursor_uses >= 2u) {
        text.append("   pattern ");
        append_dec(text, cursor_pattern, digits_of(static_cast<int>(cursor_pattern)));
        text.append(" in ");
        append_uint(text, cursor_uses);
        text.append(" slots");
      }
    };
    if (timeline) {  // OS-SONG-TIMELINE: the cursor's slot, where it starts and how long it is; OS-SONG-VIEW2 V2: while it plays, the bars to the next slot first
      if (length <= 0) {
        text.assign("timeline   no slots");
      } else if (view.play.playing) {
        unsigned pattern_steps = kDemoPatternSteps;  // a pattern-mode playback: the pattern's own length (the demo list has no project)
        if (model.project != nullptr) {
          const std::uint32_t len = model.project->pattern_length(view.play.pattern);
          pattern_steps = len != 0u ? len : model.project->default_pattern_length();
        }
        const StageState s = stage_state(src, view.play, sv.pos(), pattern_steps);
        text.assign("timeline   ");
        text.append(s.next_kind == StageNext::Jump ? "JUMP IN " : (s.next_kind == StageNext::Loop ? "LOOP IN " : "NEXT IN "));
        append_uint(text, s.bars_left);
        text.append(s.bars_left == 1u ? " BAR" : " BARS");
        text.append("   cursor slot ");
        append_dec(text, static_cast<unsigned>(sv.pos()), digits_of(length));
      } else {
        int start = 0;
        for (int i = 0; i < sv.pos(); ++i) {
          start += bars_of(src.steps(i));
        }
        const int bars = bars_of(src.steps(sv.pos()));
        text.assign("timeline   slot ");
        append_dec(text, static_cast<unsigned>(sv.pos()), digits_of(length));
        text.append("   bar ");
        append_uint(text, static_cast<unsigned>(start + 1));
        text.append("   pattern ");
        append_dec(text, cursor_pattern, digits_of(static_cast<int>(cursor_pattern)));
        text.append("   ");
        append_uint(text, static_cast<unsigned>(bars));
        text.append(bars == 1 ? " bar" : " bars");
      }
    } else if (in_map) {
      text.assign("track ");
      append_dec(text, static_cast<unsigned>(sv.track() + 1), 2);
      if (sv.selecting() || sv.track_selecting()) {
        text.assign("slots ");
        append_dec(text, static_cast<unsigned>(sv.first()), digits_of(length));
        text.append(" to ");
        append_dec(text, static_cast<unsigned>(sv.last()), digits_of(length));
        text.append("   tracks ");
        append_dec(text, static_cast<unsigned>(sv.tfirst() + 1), 2);
        text.append(" to ");
        append_dec(text, static_cast<unsigned>(sv.tlast() + 1), 2);
        text.append(" selected   Copy, Delete");
      } else {
        text.append("   slot ");
        append_dec(text, static_cast<unsigned>(sv.pos()), digits_of(length));
        text.append("   Copy, Delete");
      }
    } else if (arrangement) {
      int bars = 0;
      for (int i = 0; i < length; ++i) {
        bars += bars_of(src.steps(i));
      }
      text.assign("arrangement   ");
      append_dec(text, static_cast<unsigned>(bars), bars > 999 ? 4u : 3u);
      text.append(" bars in all");
      append_uses();
    } else if (sv.selecting()) {
      text.assign("slots ");
      append_dec(text, static_cast<unsigned>(sv.first()), digits_of(length));
      text.append(" to ");
      append_dec(text, static_cast<unsigned>(sv.last()), digits_of(length));
      text.append(" selected   Copy, Delete");
    } else if (sv.tempo_focus()) {
      text.assign("tempo   encoder: 1 BPM, Shift: 0.1   F7 again: tap");
    } else {
      text.assign("order list   position ");
      append_dec(text, static_cast<unsigned>(sv.pos()), digits_of(length));
      text.append(" of ");
      append_dec(text, static_cast<unsigned>(length), digits_of(length));
      if (cursor_uses >= 2u) {
        append_uses();  // OS-SONGSHARE S1: how many slots hold this slot's pattern
      } else {
        text.append("   SHIFT+INS DUP");  // OS-SHIFTROW: the slot with a copy of the pattern
      }
    }
    // While the song plays the right side is the live jump's key.
    draw_info_line(r, f, text.c_str(),
                   queued >= 0 && queued == sv.pos() ? "REC: cancel the jump" : (view.play.playing ? "REC: jump here at the pattern end" : "press SONG again: next page"));
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    draw_question_keys(r, f.keys, kSongScreen, model, view, kSongSoftKeys);  // the Shift layer while it is held, as the pattern's; OS-SONGMAP: the question of 1.6 marks F1 ALL and F2 UNIQUE
  }

  // The rows' text, one colour pass at a time: the position, the pattern, the play marker, then the page's columns.
  // A bar is a bright position every fourth row, as in the pattern grid.
  static constexpr Color kPasses[] = {Color::Text, Color::TextDim, Color::Instr, Color::Fx, Color::Play, Color::SelText,
                                      Color::AccentText};
  int starts[kRows] = {};  // each visible row's start bar (1-based): the bars of everything before it
  bool real[kRows] = {};   // its pattern exists (the rows of one that does not are dimmed)
  bool shared[kRows] = {};  // another slot holds its pattern too (OS-SONGSHARE S1)
  core::FixedString<kSongNameChars> names[kRows];  // its name, cut to the column (a project's names are longer)
  int start = 1;
  for (int k = 0; k < sv.top() + visible; ++k) {
    if (k >= sv.top()) {
      starts[k - sv.top()] = start;
      real[k - sv.top()] = src.exists(k);
      shared[k - sv.top()] = holders(src.pattern(k)) >= 2u;
      names[k - sv.top()].assign(src.name(k));
    }
    start += bars_of(src.steps(k));
  }
  core::FixedString<8> number;
  for (const Color pass : kPasses) {
    for (int i = 0; i < visible; ++i) {
      const int pos = sv.top() + i;
      const bool sel = selected(i);
      const int y = cell_y(i);
      const auto dim = [&](Color normal) { return real[i] ? normal : Color::TextDim; };  // a pattern that does not exist yet
      if (pos == marker && (sel ? Color::SelText : Color::Play) == pass &&
          !hidden({l.marker_x, row_y(i) + (f.row_pitch - static_cast<int>(ui::kIconSize)) / 2, static_cast<int>(ui::kIconSize),
                   static_cast<int>(ui::kIconSize)})) {
        r.icon(l.marker_x, row_y(i) + (f.row_pitch - static_cast<int>(ui::kIconSize)) / 2, ui::Icon::Play, pass);
      }
      const bool flash = pos == queued && view.blink;
      if ((flash ? Color::Play : ink_of(sel, pos % 4 == 0 ? Color::Text : Color::TextDim)) == pass && !hidden({l.pos_x, y, l.hex_w, cell_h})) {
        r.hex_tile(l.pos_x, y, bcd(pos), pass);
      }
      const bool same = cursor_uses >= 2u && i != cursor_slot && src.pattern(pos) == cursor_pattern;  // D6: the slots that hold the cursor's pattern, in the Fx ink
      const Color pat = flash ? Color::Play : (i == cursor_slot && !in_map ? Color::AccentText : (sel ? Color::SelText : dim(same ? Color::Fx : Color::Instr)));
      if (pat == pass && !hidden({l.pat_x, y, l.hex_w, cell_h})) {
        r.hex_tile(l.pat_x, y, bcd(static_cast<int>(src.pattern(pos))), pass);  // pos < length
      }
      if (arrangement) {
        // A start bar past 999 (255 positions of up to 8 bars: at most 2041) takes a fourth digit, into the gap before the BARS column (4 * gap >= a cell), as the total's
        // line does; before OS-FU1 only its low three digits showed.
        const unsigned start_digits = starts[i] > 999 ? 4u : 3u;
        if (ink_of(sel, dim(Color::Text)) == pass && !hidden({l.start_x, y, static_cast<int>(start_digits) * cell_w, cell_h})) {
          number.clear();
          append_dec(number, static_cast<unsigned>(starts[i]), start_digits);
          r.text(l.start_x, y, number.c_str(), pass);
        }
        if (ink_of(sel, dim(Color::Fx)) == pass && !hidden({l.bars_x, y, 3 * cell_w, cell_h})) {
          number.clear();
          append_dec(number, static_cast<unsigned>(bars_of(src.steps(pos))), 3);
          r.text(l.bars_x, y, number.c_str(), pass);
        }
      } else {
        if (ink_of(sel, dim(Color::Text)) == pass && !hidden({l.name_x, y, kSongNameChars * cell_w, cell_h})) {
          r.text(l.name_x, y, names[i].c_str(), pass);
        }
        if (ink_of(sel, dim(Color::Fx)) == pass && !hidden({l.len_x, y, 3 * cell_w, cell_h})) {
          number.clear();
          append_dec(number, static_cast<unsigned>(src.steps(pos)), 3);
          r.text(l.len_x, y, number.c_str(), pass);
        }
        // OS-SONGSHARE S1 (Q4a): one mark in the gap after NAME for a pattern that two or more slots hold; the slots of the cursor's pattern wear the second ink with their PAT tile.
        const int mark_y = ui::caps_y(t, Face::Label, row_y(i), f.row_pitch);
        if (shared[i] && ink_of(sel, same ? Color::Fx : Color::TextDim) == pass && !hidden({l.mark_x, mark_y, l.mark_w, l.mark_h})) {
          r.text(l.mark_x, mark_y, kSharedMark, pass, Face::Label);
        }
      }
    }
  }
}

constexpr SongScreen kSongScreen{};

}  // namespace o3os::app
