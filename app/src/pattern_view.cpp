// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/pattern_view.h>

#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/lane_marks.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>

namespace o3os::app {

using ui::Axis;
using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

constexpr int kRows = PatternView::kVisibleRows;
constexpr int kTracksMax = PatternView::kMaxVisibleTracks;
constexpr int kFieldSlots = 25;  // the most fields drawn at once: tracks x fields of a zoom level (5 x 5, 8 x 3, 12 x 1)
static_assert(kZoomTable[0].tracks * kZoomTable[0].fields <= kFieldSlots && kZoomTable[1].tracks * kZoomTable[1].fields <= kFieldSlots && kZoomTable[2].tracks * kZoomTable[2].fields <= kFieldSlots,
              "the per-field colour slots hold every zoom level");
static_assert(kZoomLevels == PatternView::kZoomCount && kZoomTable[kZoomLevels - 1].tracks == PatternView::kMaxVisibleTracks && kZoomTable[0].tracks == PatternView::kVisibleTracks,
              "the zoom table and the view's constants agree");
constexpr int kBarRows = 16;   // a bar (4 beats): a one-pixel rule above every 16th step, in TextDim: a clear line
constexpr int kBeatRows = 4;   // a beat: a bright row number every 4th step, no line

constexpr int clamp_int(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// Packed decimal for the hex-pair tiles: 0..99 drawn as the two digits of the number ("07", "63").
constexpr unsigned bcd(int n) noexcept { return static_cast<unsigned>(((n / 10) << 4) | (n % 10)); }

}  // namespace

int PatternView::visible_tracks() const noexcept { return pattern_zoom(zoom_).tracks; }
int PatternView::visible_fields() const noexcept { return pattern_zoom(zoom_).fields; }

void PatternView::follow(const Dims&) noexcept {  // (the window no longer depends on the pattern's extent: OS-PATCENTER)
  top_ = window_top(row_);
  const int n = visible_tracks();
  if (track_ < left_) {
    left_ = track_;
  } else if (track_ >= left_ + n) {
    left_ = track_ - n + 1;
  }
}

bool PatternView::snap_column() noexcept {
  const int last = visible_fields() - 1;
  if (static_cast<int>(column_) <= last) {
    return false;
  }
  column_ = static_cast<Column>(last);
  return true;
}

bool PatternView::set_zoom(int level, const Dims& d) noexcept {
  const int z = clamp_int(level, 0, kZoomLevels - 1);
  if (z == zoom_) {
    return false;
  }
  zoom_ = z;
  (void)snap_column();
  const int n = visible_tracks();
  left_ = clamp_int(left_, 0, d.tracks > n ? d.tracks - n : 0);
  follow(d);
  return true;
}

bool PatternView::cycle_zoom(const Dims& d) noexcept { return set_zoom(zoom_ + 1 < kZoomLevels ? zoom_ + 1 : 0, d); }

bool PatternView::move_right(const Dims& d) noexcept {
  if (static_cast<int>(column_) < visible_fields() - 1) {
    column_ = static_cast<Column>(static_cast<unsigned>(column_) + 1u);
  } else if (track_ < d.tracks - 1) {
    ++track_;
    column_ = Column::Note;
  } else {
    return false;
  }
  follow(d);
  return true;
}

bool PatternView::move_left(const Dims& d) noexcept {
  if (column_ != Column::Note) {
    column_ = static_cast<Column>(static_cast<unsigned>(column_) - 1u);
    (void)snap_column();  // (a column the level hides, chosen by a hold popup, comes back in sight)
  } else if (track_ > 0) {
    --track_;
    column_ = static_cast<Column>(visible_fields() - 1);
  } else {
    return false;
  }
  follow(d);
  return true;
}

bool PatternView::scroll_rows(int delta, const Dims& d) noexcept {
  const int row = clamp_int(row_ + clamp_int(delta, -d.steps, d.steps), 0, d.steps - 1);
  if (row == row_) {
    return false;
  }
  row_ = row;
  follow(d);
  return true;
}

bool PatternView::move_up(const Dims& d) noexcept { return scroll_rows(-1, d); }
bool PatternView::move_down(const Dims& d) noexcept { return scroll_rows(1, d); }

bool PatternView::select_column(Column column) noexcept {
  if (column == column_) {
    return false;
  }
  column_ = column;
  return true;
}

bool PatternView::cycle_fx_column() noexcept {
  return select_column(column_ == Column::Fx1 ? Column::Fx2 : Column::Fx1);
}

bool PatternView::jump_row(int row, const Dims& d) noexcept { return scroll_rows(row - row_, d); }

bool PatternView::move_track(int delta, const Dims& d) noexcept {
  const int track = clamp_int(track_ + clamp_int(delta, -d.tracks, d.tracks), 0, d.tracks - 1);
  if (track == track_) {
    return false;
  }
  track_ = track;
  follow(d);
  return true;
}

bool PatternView::home(const Dims& d) noexcept {
  const bool changed = track_ != 0 || column_ != Column::Note;
  track_ = 0;
  column_ = Column::Note;
  follow(d);
  return changed;
}

bool PatternView::end(const Dims& d) noexcept {
  const Column last = static_cast<Column>(visible_fields() - 1);
  const bool changed = track_ != d.tracks - 1 || column_ != last;
  track_ = d.tracks - 1;
  column_ = last;
  follow(d);
  return changed;
}

bool PatternView::clamp_to(const Dims& d) noexcept {
  const int row = clamp_int(row_, 0, d.steps - 1);
  const int track = clamp_int(track_, 0, d.tracks - 1);
  const int top = window_top(row);
  const int left = clamp_int(left_, 0, d.tracks > visible_tracks() ? d.tracks - visible_tracks() : 0);
  const bool changed = row != row_ || track != track_ || top != top_ || left != left_;
  row_ = row;
  track_ = track;
  top_ = top;
  left_ = left;
  follow(d);
  return changed;
}

namespace {

Color column_color(Column c) noexcept {
  switch (c) {
    case Column::Note:
      return Color::Text;
    case Column::Instr:
      return Color::Instr;
    case Column::Vol:
      return Color::Vol;
    case Column::Fx1:
    case Column::Fx2:
      return Color::Fx;
  }
  return Color::Text;
}

// The colour a field is drawn in: the cursor's field on the accent, the rest of the selected row and the selected block on the
// selection, a step past its track's end and an empty field quiet, the others in their column's colour.
// The play row is a Play bar, so its text is PlayText whatever else the row is (`play_ink`: SelText on the Rec bar of Rec mode); a muted track's fields are dim (TextDim: its columns' own colours
// say "sounding"), and its empty fields stay as quiet as any.
Color field_color(Column column, bool empty, bool selected_row, bool cursor, bool in_block, bool past_end, bool play_row, bool muted, Color play_ink) noexcept {
  if (play_row) {
    return cursor ? Color::AccentText : play_ink;
  }
  if (selected_row) {
    return cursor ? Color::AccentText : Color::SelText;
  }
  if (in_block) {
    return Color::SelText;
  }
  if (empty || past_end) {
    return Color::Empty;
  }
  return muted ? Color::TextDim : column_color(column);
}

Color row_number_color(int row, bool selected_row, bool play_row, Color play_ink) noexcept {
  if (play_row) {
    return play_ink;
  }
  return selected_row ? Color::SelText : (row % kBeatRows == 0 ? Color::Text : Color::TextDim);
}

// One field of a step at (x, y), the top-left corner of its tiles, in colour `c`. One word for a note, an instrument or
// a volume; an FX is its type glyph and a pair; an empty field is its dashes (the note tile "---" serves FX too).
// `face` is the zoom level's (OS-ZOOM): the tiles and glyphs of the fields are its size.
// An FX is drawn in two sweeps over a column at the 5-track zoom (the Pattern loop): its names, then the rest (the values, the unnamed FX and the empty fields), so that the VERTEX2F fallback changes the bitmap handle twice a column, not twice a field.
enum class FieldPart : std::uint8_t { All, Name, Rest };

void draw_field(Renderer& r, const Step& step, Column column, int x, int y, Color c, int octave_shift, Face face, FieldPart part = FieldPart::All) noexcept {
  char text[4];
  const bool empty = format_field(step, column, text);
  const int cell_w = r.theme().geometry(face).cell_w;
  switch (column) {
    case Column::Note:
      r.note_tile(x, y, note_tile_cell(step.note, octave_shift), c, face);
      break;
    case Column::Instr:
      if (empty || text[0] == '?') {
        r.text(x, y, text, c, face);
      } else {
        r.hex_tile(x, y, bcd(step.instr), c, face);
      }
      break;
    case Column::Vol:
      if (empty || text[0] == '?') {
        r.text(x, y, text, c, face);
      } else {
        r.hex_tile(x, y, step.vol, c, face);
      }
      break;
    case Column::Fx1:
    case Column::Fx2:
      if (empty) {
        if (part != FieldPart::Name) {
          r.note_tile(x, y, 0, c, face);
        }
      } else {
        const model::StepFx& fx = step.fx[column == Column::Fx1 ? 0 : 1];
        // OS-AUDIT E-PAT-03 (new Community OS behaviour): at the 5-track zoom the FX is its 3-letter name and the value in the Micro face, the same 3 cells wide, the capitals centred on the Data face's;
        // a type without a name (and the 8 and 12 zooms, which do not draw FX at all) keeps the glyph.
        char name[4];
        if (face == Face::Data && ui::fx_tile_text(fx.type, name)) {
          const ui::FontGeometry data = r.theme().geometry(Face::Data);
          const ui::FontGeometry micro = r.theme().geometry(Face::Micro);
          const int dy = (data.ascent - data.cap_h) + (data.cap_h - micro.cap_h) / 2 - (micro.ascent - micro.cap_h);
          if (part != FieldPart::Rest) {
            r.fx_name_tile(x, y + dy, fx.type, c);
          }
          if (part != FieldPart::Name) {
            r.hex_tile(x + static_cast<int>(ui::kMicroNoteW), y + dy, fx.value, c, Face::Micro);
          }
        } else if (part != FieldPart::Name) {
          const char type[2] = {text[0], '\0'};  // the type glyph alone: the value is the tile after it
          r.text(x, y, type, c, face);
          r.hex_tile(x + cell_w, y, fx.value, c, face);
        }
      }
      break;
  }
}

// OS-MIDIREC-P1 S4: the text of a lane mark in the field of column `col` (0 note: "MID", 1 instrument: the notes that start, 2 volume: the CCs; two decimal digits, capped at 99).
enum LaneText : std::uint8_t { kLaneNone = 0, kLaneHeld = 1, kLaneCounts = 2 };  // held: "MID" alone; counts: "MID nn cc"

void draw_lane_text(Renderer& r, const LaneMark& m, int col, int x, int y, Color c, Face face) noexcept {
  if (col == 0) {
    r.text(x, y, "MID", c, face);
  } else {
    r.hex_tile(x, y, bcd(std::min<int>(col == 1 ? m.starts : m.ccs, 99)), c, face);
  }
}

// What a field costs in display-list words, at most (the estimate that decides whether the held steps' "MID" fits, see PatternView::draw).
unsigned field_word_bound(Column column, bool empty) noexcept {
  switch (column) {
    case Column::Note:
      return 1;
    case Column::Instr:
    case Column::Vol:
      return empty ? 2 : 1;
    case Column::Fx1:
    case Column::Fx2:
      return empty ? 1 : 2;
  }
  return 2;
}

// "C-3" for a note number (0 = C-0): the tile's own name, which is the model's note (the number plus one) and "???" past G10 (note_name, pattern_data.h: the Fill dialog says its notes the same way).
void append_note_name(core::FixedString<64>& text, unsigned note, int octave_shift) noexcept {
  char name[4];
  note_name(note, octave_shift, name);
  text.append(name);
}

}  // namespace

void PatternView::draw(Renderer& r, const PatternSource& src, const Status& status, Rect covered, int pane_w) const noexcept {
  // OS-PTOOLS2 S3p: while the right-hand panel is up the grid is drawn in the pane's left `pane_w` pixels, with the track columns that fit
  // there (three at the 5-track zoom). The window is scrolled locally to the block's first track (spec 3.1 point 3: the grid scrolls so the
  // block's first track is leftmost), or to the cursor when there is no block, so both stay in view; the stored left_ is the full width's.
  const int pane_tracks = pattern_tracks_fit(r.theme(), visible_tracks(), zoom_, pane_w);
  const PatternLayout l = make_pattern_layout(r.theme(), kRows, pane_tracks, zoom_, pane_w);
  const Selection& sel = status.selection;
  const Selection::Block block = sel.block(row_, track_);
  const int tracks_total = src.tracks();
  int left = left_;  // the stored window, as follow() keeps it
  if (pane_w < ui::kScreenW) {  // the right-hand panel is up: narrow the window to the block's first track (or the cursor), keeping both in view
    left = clamp_int(sel.active ? block.track0 : track_, 0, tracks_total > pane_tracks ? tracks_total - pane_tracks : 0);
    if (track_ < left) {
      left = track_;
    } else if (track_ >= left + pane_tracks) {
      left = track_ - pane_tracks + 1;
    }
  }
  // OS-CENTER: the window is where the cursor puts it (window_top), whatever the stored top_ says: a pattern that changed length under the view is drawn right until pattern_sync catches up.
  // OS-PATCENTER: the cursor's slot is the centre one at both ends, so the first slots can lie above row 0 and the last below the last row: such a slot is blank (`live` is false: no number, no fields, no rule, no bar).
  const int top = window_top(row_);
  const int cursor_slot = row_ - top;
  const int length = src.length();
  bool live[kRows];
  for (int i = 0; i < kRows; ++i) {
    live[i] = top + i >= 0 && top + i < length;
  }
  const int field = static_cast<int>(column_);
  const int cell_h = r.theme().geometry(l.face).cell_h;
  const int index_h = r.theme().geometry(Face::Data).cell_h;
  const int shown = clamp_int(tracks_total - left, 0, l.tracks);  // track columns that exist in the window
  // (a column the zoom level hides, chosen by a hold popup, has no field to put the cursor on: none is drawn)
  const bool cursor_in_window = cursor_slot >= 0 && cursor_slot < kRows && track_ >= left && track_ < left + shown && field < l.fields;
  const int play_slot = status.play_row - top;  // the play bar's row in the window; out of 0..kRows-1 when the sequencer's step is not shown
  const bool play_visible = status.play_row >= 0 && play_slot >= 0 && play_slot < kRows && live[play_slot];
  // What lies entirely inside the overlay's rectangle is skipped: the overlay paints over it (OS-002b). With no overlay
  // `covered` is empty and nothing is skipped, so the picture and the word count are exactly those of OS-003b.
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  // OS-MUTEVIS: the MUTE / SOLO tags. Where each is drawn (the same slot for every track, never the cursor's or the play bar's), and what it says; a pending quantised mute blinks (status.blink is its lit phase).
  int tag_at[kTracksMax];
  const char* tag_text[kTracksMax];
  const int tag_row = tag_slot(cursor_slot, play_visible ? play_slot : -1);
  for (int s = 0; s < kTracksMax; ++s) {
    tag_at[s] = -1;
    tag_text[s] = "";
    if (s < shown) {
      const bool pending = src.mute_pending(left + s);
      const bool mute = src.muted(left + s) || pending;  // (a pending unmute: the track still sounds muted until its boundary)
      if ((mute || src.soloed(left + s)) && (!pending || status.blink)) {
        tag_at[s] = tag_row;
        tag_text[s] = mute ? "MUTE" : "SOLO";
      }
    }
  }

  // Chrome bands first, then what lies on the pane back to front: the rules, the selected row, the block and the cursor.
  draw_chrome_bands(r, make_frame(r.theme(), kRows), covered);

  for (int s = 0; s < shown; ++s) {
    if (!hidden({l.track_x(s), l.grid.y, 1, l.grid.h})) {
      r.rule(l.track_x(s), l.grid.y, l.grid.h, Axis::Vertical, Color::Empty);
    }
  }
  for (int i = 1; i < kRows; ++i) {
    if (live[i] && top + i > 0 && (top + i) % kBarRows == 0 && i != cursor_slot && !hidden({0, l.row_y(i), l.grid.w, 1})) {
      r.rule(0, l.row_y(i), l.grid.w, Axis::Horizontal, Color::TextDim);
    }
  }
  if (cursor_slot >= 0 && cursor_slot < kRows && !(play_visible && play_slot == cursor_slot)) {  // the play bar takes the cursor's row when they meet
    const Rect row_bar = {0, l.row_y(cursor_slot), l.grid.w, l.row_pitch};
    if (!hidden(row_bar)) {
      r.highlight_row(row_bar);
    }
  }
  // The selected block: one Sel rectangle a row across the selected tracks (the cursor's row has its bar already). Same colour as the
  // bar, so no colour word between them: two words a row.
  if (sel.active && shown > 0) {
    const int s0 = clamp_int(block.track0 - left, 0, shown - 1);
    const int s1 = clamp_int(block.track1 - left, 0, shown - 1);
    const bool on_screen = block.track1 >= left && block.track0 < left + shown;
    // OS-AUDIT-56: a column block (inside one track) paints from its first column to its last only; whole steps, the whole track width.
    const int c0 = clamp_int(block.col0, 0, l.fields - 1);
    const int c1 = clamp_int(block.col1, 0, l.fields - 1);
    const int bar_x = block.whole_steps() ? l.track_x(s0) : l.track_x(s0) + l.field_x[c0];
    const int bar_w = block.whole_steps() ? (s1 - s0 + 1) * l.track_w : l.field_x[c1] + l.field_w[c1] - l.field_x[c0];
    // OS-SELTINT: over several tracks the cursor's track keeps Sel and the others are SelDim, a little duller, so the cursor's track reads in the block: at most three rectangles a row
    // (left tracks, the cursor's track, right tracks). The cursor's own row has its Sel bar already; its dimmed segments go over that bar.
    const int cs = track_ - left;
    const bool tinted = block.whole_steps() && s1 > s0;
    for (int i = 0; on_screen && i < kRows; ++i) {
      const int row = top + i;
      if (!live[i] || row < block.row0 || row > block.row1 || i == play_slot) {
        continue;
      }
      if (!tinted) {
        const Rect bar = {bar_x, l.row_y(i), bar_w, l.row_pitch};
        if (i != cursor_slot && !hidden(bar)) {
          r.panel(bar, Color::Sel);
        }
        continue;
      }
      const int lo_end = cs < s0 ? s0 - 1 : (cs > s1 ? s1 : cs - 1);       // the dim run before the cursor's track: s0..lo_end
      const int hi_start = cs > s1 ? s1 + 1 : (cs < s0 ? s0 : cs + 1);     // the dim run after it: hi_start..s1
      const Rect lo = {l.track_x(s0), l.row_y(i), (lo_end - s0 + 1) * l.track_w, l.row_pitch};
      const Rect mid = {l.track_x(cs), l.row_y(i), l.track_w, l.row_pitch};
      const Rect hi = {l.track_x(hi_start), l.row_y(i), (s1 - hi_start + 1) * l.track_w, l.row_pitch};
      if (lo_end >= s0 && !hidden(lo)) {
        r.panel(lo, Color::SelDim);
      }
      if (cs >= s0 && cs <= s1 && i != cursor_slot && !hidden(mid)) {
        r.panel(mid, Color::Sel);
      }
      if (hi_start <= s1 && !hidden(hi)) {
        r.panel(hi, Color::SelDim);
      }
    }
  }
  // The play bar (OS-009b): the step the sequencer is on, the full width like the cursor's bar, in the Play colour; the text on it is PlayText. In Rec mode (OS-REVIEW) the
  // bar is the Rec red and its text SelText.
  const Color play_ink = status.recording ? Color::SelText : Color::PlayText;
  if (play_visible) {
    const Rect play_bar = {0, l.row_y(play_slot), l.grid.w, l.row_pitch};
    if (!hidden(play_bar)) {
      r.panel(play_bar, status.recording ? Color::Rec : Color::Play);
    }
  }
  // The cursor: the orange block with a one-pixel AccentText outline, which is what sets it off the blue row (orange on
  // that blue is under 3:1 in every theme; the outline is above it in all three, see ui/theme.h).
  const Rect cursor_block = {l.track_x(track_ - left) + l.field_x[field] - l.cursor_pad_x,
                             l.row_y(cursor_slot) + l.cursor_dy, l.field_w[field] + 2 * l.cursor_pad_x, l.cursor_h};
  const int o = l.cursor_outline;
  const Rect outline = {cursor_block.x - o, cursor_block.y - o, cursor_block.w + 2 * o, cursor_block.h + 2 * o};
  if (cursor_in_window && !hidden(outline)) {
    r.panel(outline, Color::AccentText);
    r.panel(cursor_block, Color::Accent);
  }
  // Rec mode: a Rec frame round the grid, over the bars (three pixels, inside the pane; four panels in one colour). Under an overlay only the pieces it leaves show.
  if (status.rec) {
    const int w = 3 * r.theme().metrics.border;
    const Rect edges[4] = {{l.grid.x, l.grid.y, l.grid.w, w},
                           {l.grid.x, l.grid.y + l.grid.h - w, l.grid.w, w},
                           {l.grid.x, l.grid.y + w, w, l.grid.h - 2 * w},
                           {l.grid.x + l.grid.w - w, l.grid.y + w, w, l.grid.h - 2 * w}};
    for (const Rect& edge : edges) {
      if (!hidden(edge)) {
        r.panel(edge, Color::Rec);
      }
    }
  }

  if (!hidden(l.header) || !hidden(l.strip)) {
    draw_headers(r, l, src, status, shown, left);
    draw_strip(r, src, status);
  }
  if (!hidden(l.info)) {
    draw_info(r, status);
  }
  if (!hidden(l.keys)) {
    ui::soft_keys(r, l.keys, status.soft_keys, 8, status.soft_dim, status.soft_hot);
  }

  // Step numbers and fields, one colour pass at a time and track by track within a pass: each colour is set once, and
  // the tiles of a track are consecutive, so the origin (VERTEX_TRANSLATE_X, ui/renderer.h) moves a few times a pass.
  // What colour each field is drawn in is worked out once, not once a pass.
  // One slot per shown field: tracks x fields is 25, 24 and 12 at the three zoom levels, so 25 (not 12 x 5) keep the stack of the main loop small.
  Color field_colors[kFieldSlots][kRows];
  Step steps[kTracksMax][kRows];
  // OS-MIDIREC-P1 S4: the lane of each shown track as marks per row, and the text a step's empty columns get (LaneText). `est_words` is an upper bound of what the fields cost, for the held steps' "MID".
  LaneMark lane_mark[kTracksMax][kRows];
  std::uint8_t lane_text[kTracksMax][kRows] = {};
  unsigned est_words = 0;
  unsigned held_steps = 0;
  bool any_mark = false;
  for (int s = 0; s < shown; ++s) {
    const int track = left + s;
    compute_lane_marks(src.lane(track), top, kRows, src.track_length(track), lane_mark[s]);
    for (int i = 0; i < kRows; ++i) {
      const int row = top + i;
      if (!live[i]) {  // a blank slot: nothing of it is drawn, and `steps` and `field_colors` stay unset there (every pass loop tests live[i] before it reads either)
        lane_mark[s][i] = LaneMark{};
        continue;
      }
      const Step step = src.step(track, row);
      steps[s][i] = step;
      any_mark = any_mark || lane_mark[s][i].any();
      const bool in_block = sel.active && row >= block.row0 && row <= block.row1 && track >= block.track0 && track <= block.track1;
      const bool past_end = row >= src.track_length(track);
      const bool muted = src.muted(track);
      for (int col = 0; col < l.fields; ++col) {
        const Column column = static_cast<Column>(col);
        char unused[4];
        const bool empty = format_field(step, column, unused);
        const bool cursor = i == cursor_slot && track == track_ && col == field;
        field_colors[s * l.fields + col][i] = field_color(column, empty, i == cursor_slot, cursor, in_block && col >= block.col0 && col <= block.col1, past_end, play_visible && i == play_slot, muted, play_ink);
        est_words += field_word_bound(column, empty);
      }
      // A lane mark's text goes where the step's own note, instrument and volume are all empty (what the step holds wins; the bar is drawn all the same).
      char t0[4], t1[4], t2[4];
      if (lane_mark[s][i].any() && !past_end && format_field(step, Column::Note, t0) && format_field(step, Column::Instr, t1) && format_field(step, Column::Vol, t2)) {
        lane_text[s][i] = lane_mark[s][i].counts() ? kLaneCounts : kLaneHeld;
        held_steps += lane_text[s][i] == kLaneHeld ? 1u : 0u;
      }
    }
  }
  // The text replaces the dashes of the empty columns at no extra word ("MID" three glyphs for the note tile's one, the two counts one tile each for the dashes' two glyphs each) except on a held step, whose "MID" alone costs two words.
  // Those are the first to go when the grid is near its budget (the spec's order: the held steps' text, then the bars); the estimate is the words so far, an upper bound of the fields, the passes' colour and origin words (`kPassOverhead`) and a reserve for the bars.
  if (held_steps != 0) {
    constexpr unsigned kPassOverhead = 150;
    constexpr unsigned kBarReserve = 64;
    if (r.words() + est_words + kPassOverhead + 2u * held_steps + kBarReserve > kPatternBudgetWords) {
      for (int s = 0; s < shown; ++s) {
        for (int i = 0; i < kRows; ++i) {
          if (lane_text[s][i] == kLaneHeld) {
            lane_text[s][i] = kLaneNone;
          }
        }
      }
    }
  }
  // The text's colour: the lane colours on an otherwise empty (quiet) field; the selection, cursor and play colours stay.
  for (int s = 0; s < shown; ++s) {
    for (int i = 0; i < kRows; ++i) {
      if (lane_text[s][i] == kLaneNone) {
        continue;
      }
      const bool muted = src.muted(left + s);
      for (int col = 0; col < l.fields && col <= 2; ++col) {
        Color& c = field_colors[s * l.fields + col][i];
        if (c == Color::Empty && (col == 0 || lane_text[s][i] == kLaneCounts)) {
          c = muted ? Color::TextDim : (col == 0 ? Color::TextDim : col == 1 ? Color::Instr : Color::Vol);
        }
      }
    }
  }
  static constexpr Color kPasses[] = {Color::Empty, Color::Text, Color::TextDim,   Color::Instr,     Color::Vol,
                                      Color::Fx,    Color::SelText, Color::PlayText, Color::AccentText};
  for (const Color pass : kPasses) {
    for (int i = 0; i < kRows; ++i) {
      const int row = top + i;
      if (live[i] && row_number_color(row, i == cursor_slot, play_visible && i == play_slot, play_ink) == pass &&
          !hidden({l.index_x, l.row_y(i) + l.index_dy, static_cast<int>(ui::kHexTileW), index_h})) {
        r.hex_tile(l.index_x, l.row_y(i) + l.index_dy, row_number_tile(row, status.numbering), pass);
      }
    }
    // Track by track (the origin moves least), and within a track column by column: the selection's colour holds fields of every
    // column, and a column's tiles are one bitmap handle, which the VERTEX2F fallback then sets once instead of at every tile.
    for (int s = 0; s < shown; ++s) {
      for (int col = 0; col < l.fields; ++col) {
        const bool fx_names = l.face == Face::Data && col >= static_cast<int>(Column::Fx1);  // OS-AUDIT E-PAT-03: two sweeps, the names and then the rest
        for (int sweep = 0; sweep < (fx_names ? 2 : 1); ++sweep) {
          const FieldPart part = !fx_names ? FieldPart::All : (sweep == 0 ? FieldPart::Name : FieldPart::Rest);
          for (int i = 0; i < kRows; ++i) {
            if (live[i] && field_colors[s * l.fields + col][i] == pass && tag_at[s] != i &&  // (the tag of the track is over this row's fields)
                !hidden({l.track_x(s) + l.field_x[col], l.row_y(i) + l.cell_dy, l.field_w[col], cell_h})) {
              if (col <= 2 && lane_text[s][i] != kLaneNone && (col == 0 || lane_text[s][i] == kLaneCounts)) {
                draw_lane_text(r, lane_mark[s][i], col, l.track_x(s) + l.field_x[col], l.row_y(i) + l.cell_dy, pass, l.face);
              } else {
                draw_field(r, steps[s][i], static_cast<Column>(col), l.track_x(s) + l.field_x[col], l.row_y(i) + l.cell_dy, pass, status.octave_shift, l.face, part);
              }
            }
          }
        }
      }
    }
  }
  // The tags, over the dimmed rows: an inverted chip across the track's column, in two passes (the panels, then the letters: one colour word each). On the pane the inversion is the bright Text token with the letters knocked out
  // in the Pane's (the header's M / S chip is the same shape, ChromeText on Chrome, which on a dark pane would not stand out).
  const ui::Metrics& metrics = r.theme().metrics;
  const auto chip_of = [&](int s) { return Rect{l.track_x(s) + metrics.border, l.row_y(tag_at[s]), l.track_w - metrics.border, l.row_pitch}; };
  for (int s = 0; s < shown; ++s) {
    if (tag_at[s] >= 0 && !hidden(chip_of(s))) {
      r.panel(chip_of(s), Color::Text);
    }
  }
  for (int s = 0; s < shown; ++s) {
    if (tag_at[s] >= 0 && !hidden(chip_of(s))) {
      const Rect chip = chip_of(s);
      r.text(chip.x + (chip.w - r.text_width(tag_text[s], Face::Data)) / 2, chip.y + l.index_dy, tag_text[s], Color::Pane, Face::Data);
    }
  }
  // OS-MIDIREC-P1 S4: the lane bars, last (so the words so far are exact). 2 px at the left edge of the track's column: the top half for the notes (the lane-note colour: full width where a note starts, a thin 1 px
  // line where one is held), the bottom half for the CCs (the lane-CC colour); a CC-only step is the whole bar in the lane-CC colour. Runs of rows in one shape are one rectangle. A pass (one colour) stops
  // at the budget: what does not fit is not drawn (the text is already out; this is the last of the spec's degrade order). A tag's row has no bar (its chip covers the column).
  if (any_mark) {
    const int w = 2 * metrics.border;
    const int half = l.row_pitch / 2;
    for (const Color pass : {Color::Instr, Color::Vol}) {
      for (int s = 0; s < shown; ++s) {
        Rect run = {0, 0, 0, 0};
        const auto flush = [&]() {
          if (run.w != 0 && !hidden(run) && r.words() + 8u <= kPatternBudgetWords) {  // (a panel is 2 vertex words, a colour word, a BEGIN when the primitive changes and a VERTEX_TRANSLATE_X when the origin moves: 8 is a safe bound)
            r.panel(run, pass);
          }
          run = Rect{0, 0, 0, 0};
        };
        for (int i = 0; i < kRows; ++i) {
          const LaneMark& m = lane_mark[s][i];
          if (!live[i] || !m.any() || tag_at[s] == i) {
            flush();
            continue;
          }
          const int x = l.track_x(s) + metrics.border;
          const int y = l.row_y(i);
          for (int h = 0; h < 2; ++h) {
            Color c = Color::Vol;  // the bottom half and a CC-only step
            int seg_w = w;
            if (h == 0) {
              if (m.starts != 0) {
                c = Color::Instr;
              } else if (m.held) {
                c = Color::Instr;
                seg_w = metrics.border;
              } else if (m.ccs == 0) {
                seg_w = 0;
              }
            } else if (m.ccs == 0) {
              seg_w = 0;
            }
            const int seg_y = h == 0 ? y : y + half;
            const int seg_h = h == 0 ? half : l.row_pitch - half;
            if (seg_w == 0 || c != pass) {
              flush();
            } else if (run.w == seg_w && run.x == x && run.y + run.h == seg_y) {
              run.h += seg_h;
            } else {
              flush();
              run = Rect{x, seg_y, seg_w, seg_h};
            }
          }
        }
        flush();
      }
    }
  }
}

// The type badge, "<number> <name>", the marks and the meter of each visible track; the selected track's name is bright and gets
// an underline in the selection blue. The marks (OS-009: M muted, S soloed; the R of an armed track went with the arming, OS-LIVEREC) are plain letters at the right end of the badge
// row, each in a place of its own so that a track toggling one does not move the others; with Shift held, the ones that are set are
// drawn twice, a pixel apart (bold: the mark's own keys are Shift + the pads). A set M or S is an inverted chip (OS-009b: ChromeText fill,
// the letter knocked out in Chrome, the pair of the message chip), which a muted track's dimmed column then backs up; no colour is added.
void PatternView::draw_headers(Renderer& r, const PatternLayout& l, const PatternSource& src, const Status& status, int shown, int left) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const int cell_w = t.geometry(Face::Label).cell_w;
  // OS-ZOOM: a zoomed level has narrower columns (96 and 64 px): two mark slots (M, S) as close as their chips allow, instead of the default level's three, and the name takes what is left (none at 12 tracks).
  const int slots = l.zoom == 0 ? 3 : 2;
  const int mark_w = l.zoom == 0 ? cell_w + m.gap : cell_w + 2 * m.border;
  core::FixedString<24> text;
  for (int s = 0; s < shown; ++s) {
    const int track = left + s;
    const int x = l.track_x(s);
    ui::type_badge(r, x + m.pad, l.badge_y, l.badge_size, src.track_type(track));
    ui::meter(r, {x + m.pad, l.meter_y, l.track_w - 2 * m.pad, l.meter_h}, src.meter_level(track), src.meter_peak(track),
              kMeterMax, Color::ChromeDim, Color::Pane, Color::Play, Color::Text);
    if (track == track_) {
      r.panel({x, l.underline_y, l.track_w, 2 * m.border}, Color::Sel);
    }
  }
  const auto mark_x = [&](int s, int k) { return l.track_x(s) + l.track_w - m.pad - (slots - k) * mark_w; };
  const int text_y = ui::caps_y(t, Face::Label, l.badge_y, l.badge_size);
  const auto slot = [&](int s, int k) { return Rect{mark_x(s, k) - m.border, l.badge_y, cell_w + 2 * m.border, l.badge_size}; };
  for (int s = 0; s < shown; ++s) {  // the chips in one colour, one pass: the colour word is set once
    if (src.muted(left + s)) {
      r.panel(slot(s, 0), Color::ChromeText);
    }
    if (src.soloed(left + s)) {
      r.panel(slot(s, 1), Color::ChromeText);
    }
  }
  // Names: room for what is left of the column once the badge and the three marks have their places.
  const int room = (l.track_w - 2 * m.pad - l.badge_size - m.gap - slots * mark_w) / cell_w;
  for (int s = 0; s < shown; ++s) {
    const int track = left + s;
    text.clear();
    append_dec(text, static_cast<unsigned>(track + 1), 2);
    if (room > 3) {  // (the number, a space and what is left for the name; a zoomed column may have room for the number alone)
      text.append(" ");
      append_clipped(text, src.track_name(track), static_cast<std::size_t>(room - 3));
    }
    r.text(l.track_x(s) + m.pad + l.badge_size + m.gap, text_y, text.c_str(), track == track_ ? Color::ChromeText : Color::ChromeDim,
           Face::Label);
  }
  for (int s = 0; s < shown; ++s) {
    const int track = left + s;
    const bool bold = status.shift;
    if (src.muted(track)) {
      r.text(mark_x(s, 0), text_y, "M", Color::Chrome, Face::Label);
      if (bold) {
        r.text(mark_x(s, 0) + 1, text_y, "M", Color::Chrome, Face::Label);
      }
    }
    if (src.soloed(track)) {
      r.text(mark_x(s, 1), text_y, "S", Color::Chrome, Face::Label);
      if (bold) {
        r.text(mark_x(s, 1) + 1, text_y, "S", Color::Chrome, Face::Label);
      }
    }
  }
}

// PAT and BPM in the Display face, the transport badge, the other captions and values in the Data face, CPU at the
// right edge; one-pixel dividers between the groups (the skeleton is chrome.h's).
void PatternView::draw_strip(Renderer& r, const PatternSource& src, const Status& status) const noexcept {
  const ui::Metrics& m = r.theme().metrics;
  const Frame f = make_frame(r.theme(), kRows);
  StatusStrip strip(r, f);
  ui::FieldRow& row = strip.row();
  core::FixedString<24> text;

  row.put(Face::Label, "PAT", Color::ChromeDim);
  row.skip(m.gap);
  text.clear();
  append_dec(text, static_cast<unsigned>(clamp_int(status.pattern, 0, 999)), status.pattern >= 100 ? 3u : 2u);
  row.put(Face::Display, text.c_str(), Color::ChromeText);
  text.assign("/");
  append_uint(text, static_cast<unsigned>(clamp_int(status.patterns, 0, 999)));
  row.put(Face::Label, text.c_str(), Color::ChromeDim);
  strip.divider();
  row.put(Face::Label, "BPM", Color::ChromeDim);
  row.skip(m.gap);
  text.clear();
  append_bpm(text, status.tempo_x100);
  row.put(Face::Display, text.c_str(), Color::ChromeText);
  strip.divider();
  strip.transport(status.playing);
  if (status.rec) {
    strip.rec();
  }
  strip.divider();

  text.clear();
  append_dec(text, static_cast<unsigned>(src.length()), 3);
  row.field("LEN", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
  text.clear();
  append_dec(text, static_cast<unsigned>(clamp_int(status.jump, 0, 99)), 2);
  row.field("JUMP", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
  // The instrument: its number and name. A document's names are up to 32 characters, so the name is clipped to what lies between here and
  // the load readouts at the right edge (OS-VOX: the SONG 05 placeholder is gone and the readouts are two Label lines, so the name has room again).
  const int cell_w = r.theme().geometry(Face::Data).cell_w;
  const int cpu_x = ui::kScreenW - 2 * m.pad - StatusStrip::load_width(r);
  const int value_x = row.x() + r.text_width("INS", Face::Label) + m.gap;
  const int room = (cpu_x - 4 * m.gap - value_x) / cell_w;  // cells for the value, number and space included
  text.clear();
  append_dec(text, static_cast<unsigned>(clamp_int(status.instrument, 0, static_cast<int>(model::kMaxInstruments))), 2);
  text.append(" ");
  append_clipped(text, *status.instrument_name != '\0' ? status.instrument_name : "--", static_cast<std::size_t>(room > 3 ? room - 3 : 0));
  row.field("INS", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
  strip.cpu(status.load);
}

// The info line (OS-009): the mode as an inverted chip at the left edge (OS-INFOLINE), then the pad board's scale and root, the cursor's track, the pattern's name and the pad mode, then METRO and the
// block hint or the FX reading; the journal's depth at the right edge.
void PatternView::draw_info(Renderer& r, const Status& status) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Frame f = make_frame(t, kRows);
  core::FixedString<64> text;
  text.assign("scale ");
  std::size_t name_len = 0;  // the names are fixed width: "Phrygian " has a space to lose
  while (status.scale[name_len] != '\0') {
    ++name_len;
  }
  while (name_len > 0 && status.scale[name_len - 1] == ' ') {
    --name_len;
  }
  text.append(status.scale, name_len);
  text.append(" ");
  append_note_name(text, status.root, status.octave_shift);
  text.append("   track ");
  append_uint(text, static_cast<unsigned>(track_ + 1));
  if (status.pattern_name[0] != '\0') {  // OS-NAMES: a named pattern says so: "P05 VERSE"; with no name today's text stays
    text.append("   P");
    append_dec(text, static_cast<unsigned>(clamp_int(status.pattern, 0, 999)), status.pattern >= 100 ? 3u : 2u);
    text.append(" ");
    append_clipped(text, status.pattern_name, kInfoNameChars);
  }
  if (status.pads[0] != '\0') {  // OS-PADS-A: a pad mode that stays (Shift + F1..F3) is said here
    text.append("   pads ");
    text.append(status.pads);
  }

  const int pad = m.pad;
  const int y = ui::caps_y(t, Face::Label, f.info.y, f.info.h);

  // The chip: the notification's own (ChromeText fill, Chrome text, as overlays.cpp), as high as the capitals plus a border each side. OS-INFOLINE: it stands first, at the left edge, so the
  // mode is always at the same place whatever the text after it says; the text follows it, then the METRO chip and the hint.
  const int chip_h = t.geometry(Face::Label).cap_h + 2 * m.border;
  const int chip_y = f.info.y + (f.info.h - chip_h) / 2;
  const int chip_x = 2 * pad;
  // OS-RECMODE2: WATCH is the plain chip, STEP the Accent token, LIVE the Rec token with a dot (the font has no round bullet: a square of the capitals' half height) in front of its letters.
  const bool live = status.chip_tone == Status::kToneLive;
  const Color chip_fill = live ? Color::Rec : (status.chip_tone == Status::kToneStep ? Color::Accent : Color::ChromeText);
  const Color chip_ink = live ? Color::SelText : (status.chip_tone == Status::kToneStep ? Color::AccentText : Color::Chrome);
  const int dot = live ? t.geometry(Face::Label).cap_h / 2 + 1 : 0;
  const int chip_w = r.text_width(status.chip, Face::Label) + 2 * pad + (live ? dot + m.gap : 0);
  r.panel({chip_x, chip_y, chip_w, chip_h}, chip_fill);
  if (live) {
    r.panel({chip_x + pad, chip_y + (chip_h - dot) / 2, dot, dot}, chip_ink);
  }
  r.text(chip_x + pad + (live ? dot + m.gap : 0), y, status.chip, chip_ink, Face::Label);
  const int text_x = chip_x + chip_w + 3 * m.gap;
  r.text(text_x, y, text.c_str(), Color::ChromeText, Face::Label);
  int chip_end = text_x + r.text_width(text.c_str(), Face::Label);  // where the last item ends (the hint and the FX reading start after it)
  if (status.metro) {  // OS-METRO: the metronome clicks while the transport runs (UI map rule 7: a mode that stays says so); a second chip of the same shape
    const int metro_x = chip_end + 3 * m.gap;
    r.panel({metro_x, chip_y, r.text_width("METRO", Face::Label) + 2 * pad, chip_h}, Color::ChromeText);
    r.text(metro_x + pad, y, "METRO", Color::Chrome, Face::Label);
    chip_end = metro_x + r.text_width("METRO", Face::Label) + 2 * pad;
  }

  text.assign("undo ");
  append_uint(text, status.undo);
  const int undo_x = ui::kScreenW - 2 * pad - r.text_width(text.c_str(), Face::Label);
  r.text(undo_x, y, text.c_str(), Color::ChromeText, Face::Label);
  // OS-PTOOLS2 S1: the block hint, in the same place and by the same rule: whole when it fits, else without its last parts ("  "-separated), down to the first. It names the encoder's verb, which nothing on screen said.
  int after_hint = chip_end + 3 * m.gap;
  if (status.hint[0] != '\0') {
    const int hint_x = chip_end + 3 * m.gap;
    char shown[96];
    std::size_t n = 0;
    while (status.hint[n] != '\0' && n + 1 < sizeof shown) {
      shown[n] = status.hint[n];
      ++n;
    }
    shown[n] = '\0';
    for (;;) {
      if (hint_x + r.text_width(shown, Face::Label) + 3 * m.gap <= undo_x) {
        r.text(hint_x, y, shown, Color::ChromeText, Face::Label);
        after_hint = hint_x + r.text_width(shown, Face::Label) + 3 * m.gap;
        break;
      }
      std::size_t cut = n;  // the last "  " (a part's start): the text before it stays
      while (cut >= 2 && !(shown[cut - 1] == ' ' && shown[cut - 2] == ' ')) {
        --cut;
      }
      if (cut < 2) {
        break;
      }
      n = cut - 2;
      shown[n] = '\0';
    }
  }
  // OS-PFX: the FX under the cursor in words, between the chip and the undo depth, when it fits (it is a reading, never in the way of the state). After the block hint when there is one: on an FX
  // column the hint does not replace the FX reading (review fold of OS-PTOOLS2 S1).
  if (status.fx_read[0] != '\0') {
    const int fx_x = after_hint;
    if (fx_x + r.text_width(status.fx_read, Face::Label) + 3 * m.gap <= undo_x) {
      r.text(fx_x, y, status.fx_read, Color::ChromeText, Face::Label);
    }
  }
}

}  // namespace o3os::app
