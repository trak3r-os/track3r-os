// SPDX-License-Identifier: GPL-3.0-or-later
#include "song_pages.h"

#include <o3os/app/pattern_view.h>
#include <o3os/app/song_timeline.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/eve/dl.h>
#include <o3os/ui/plot/ram_g_map.h>
#include <o3os/ui/components.h>
#include <o3os/ui/theme.h>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;
namespace dl = eve::dl;

namespace {

// OS-SONG-TIMELINE: a colour per pattern, four by (pattern - 1) % 4, each a pair the contrast table lists against the pane (colour is never the only cue: the number is on the block). OS-SONG-VIEW2 V3: it
// is drawn as a strip of kStripH pixels at the foot of the block, not as its fill.
constexpr Color kPatternFill[4] = {Color::Sel, Color::Play, Color::Accent, Color::Rec};
constexpr int kStripH = 4;
// OS-SONG-VIEW2 V5: the energy curve is a thin row of this many pixels at the foot of the page.
constexpr int kEnergyH = 10;

}  // namespace

void draw_song_timeline(Renderer& r, const Frame& f, const Model& model, const ViewState& view, Rect covered) noexcept {
  if (covers(covered, f.pane.x, f.pane.y, f.pane.w, f.pane.h)) {
    return;
  }
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const SongSource src(model);
  const int g = m.row_pitch / 3;
  const int mx = ui::kScreenW / 32;
  const int width = ui::kScreenW - 2 * mx;
  // The tracks Pattern can show (at most 12): a project that declares more active tracks does not draw thumbnails Pattern cannot reach.
  const unsigned active = model.project != nullptr ? model.project->active_tracks() : 0u;
  const unsigned tracks = active < static_cast<unsigned>(PatternView::kMaxVisibleTracks) ? active : static_cast<unsigned>(PatternView::kMaxVisibleTracks);
  const int cursor = view.song.clamped_pos(src.count());
  const TimelineWindow w = timeline_window(src, cursor, tracks);
  if (w.count == 0u) {
    r.text(mx, f.pane.y + g, "NO SLOTS", Color::TextDim, Face::Label);
    return;
  }
  const int marker = src.marker(view.play);
  const int bh = 2 * m.row_pitch;
  const int y0 = f.pane.y + g;
  const int micro_h = t.geometry(Face::Micro).cell_h;
  core::FixedString<8> text;
  // Slot i of the window as a block: as wide as it has bars, one pixel from the next.
  const auto block_of = [&](unsigned i) {
    const int x0 = mx + timeline_x(w, w.start[i], width);
    const int x1 = mx + timeline_x(w, w.start[i + 1], width);
    return Rect{x0, y0, x1 - x0 > 2 ? x1 - x0 - 1 : 1, bh};
  };

  // The slots as blocks as wide as they have bars (OS-SONG-VIEW2 V3): a neutral fill (the pane's own) under a rule along the top, the pattern's number in Text and, under it, the pattern's colour as a
  // strip of kStripH pixels (the colour repeats are what it is for; the number is the cue). A slot whose pattern the project does not have has a dimmer rule, a dim number and no strip. The passes
  // go by colour (rules, strips, numbers) so the display list holds one colour change per colour, not per slot.
  const int strip_h = kStripH;
  for (unsigned pass = 0; pass < 2; ++pass) {  // the rules: existing patterns in TextDim, then the missing ones in Empty
    for (unsigned i = 0; i < w.count; ++i) {
      if (src.exists(w.first + static_cast<int>(i)) == (pass == 0)) {
        const Rect blk = block_of(i);
        r.panel(Rect{blk.x, blk.y, blk.w, 1}, pass == 0 ? Color::TextDim : Color::Empty);
      }
    }
  }
  for (unsigned colour = 0; colour < 4; ++colour) {  // the strips, one colour at a time
    for (unsigned i = 0; i < w.count; ++i) {
      const int slot = w.first + static_cast<int>(i);
      if (src.exists(slot) && (src.pattern(slot) + 3u) % 4u == colour) {
        const Rect blk = block_of(i);
        r.panel(Rect{blk.x, blk.y + bh - strip_h, blk.w, strip_h}, kPatternFill[colour]);
      }
    }
  }
  for (unsigned pass = 0; pass < 2; ++pass) {  // the numbers: on every block, whatever its colour, in Text (the missing ones dim); only a block narrower than the number itself (a one-bar slot of a 48-bar window with a 3-digit id) goes without
    for (unsigned i = 0; i < w.count; ++i) {
      const int slot = w.first + static_cast<int>(i);
      if (src.exists(slot) == (pass == 0)) {
        const Rect blk = block_of(i);
        text.clear();
        append_uint(text, src.pattern(slot));
        if (r.text_width(text.c_str(), Face::Micro) + 2 <= blk.w) {
          r.text(blk.x + 2, blk.y + 1 + (bh - strip_h - 1 - micro_h) / 2, text.c_str(), pass == 0 ? Color::Text : Color::TextDim, Face::Micro);
        }
      }
    }
  }

  // The playing slot (a green bar under it and the playhead), then the cursor's (two pixels in Text round its block).
  const int under = y0 + bh + 2;
  for (unsigned i = 0; i < w.count; ++i) {
    const int slot = w.first + static_cast<int>(i);
    const Rect blk = block_of(i);
    if (slot == marker && view.play.playing) {
      r.panel(Rect{blk.x, under, blk.w, 4}, Color::Play);
    }
    if (slot == cursor) {
      r.frame(blk, Color::Text);
      r.frame(Rect{blk.x + 1, blk.y + 1, blk.w > 2 ? blk.w - 2 : 1, blk.h - 2}, Color::Text);
    }
  }

  // The note map: a lane a track under the blocks, a tick where a step holds a note (the pictures are rendered once per pattern generation: a frame compares one number a slot). Then the energy curve.
  if (model.project != nullptr && tracks > 0u) {
    const int top = under + 4 + g;
    const int floor_y = f.pane.y + f.pane.h - g;                 // the bottom of the page: the energy curve stands on it
    const int curve_h = kEnergyH;                                // its tallest column: a thin row (V5)
    const int bottom = floor_y - curve_h - (micro_h + 2) - g;    // the lanes end above the curve's caption
    int lane = (bottom - top) / static_cast<int>(tracks);
    lane = lane > m.row_pitch ? m.row_pitch : lane;
    if (lane >= 4) {
      // The note map (V4): each slot's picture (song_notemap.h) is a bitmap over its block, scaled so that a step is the same width in every block (16 a bar) and a track's lane is `lane` pixels (a little
      // less: E is whole in 8.8, see notemap_row_top); one raw section for the page (the handle, the layout, E and the colour once), then BITMAP_SOURCE, SIZE, TRANSFORM_A and VERTEX2F a slot (4 words). A pattern
      // that does not exist, or whose picture the display has not taken yet (at most SongNoteMap::kMaxPerFrame uploads a frame, none after a Busy), has none: it draws as an empty map, never a stale picture.
      // At most 12 tracks and 128 steps are drawn: tracks past 12 are not in the map.
      const std::int32_t e = notemap_scale_y(lane);
      const int map_h = notemap_row_top(2u * tracks, e);
      bool open = false;
      for (unsigned i = 0; i < w.count; ++i) {
        const int slot = w.first + static_cast<int>(i);
        const unsigned id = src.pattern(slot);
        if (!src.exists(slot) || !view.notemap.ensure(r.display(), *model.project, id)) {
          continue;
        }
        if (!open) {
          open = true;
          r.begin_raw();
          r.raw(dl::bitmap_handle(ui::plot::kHandleNoteMap));
          r.raw(dl::bitmap_layout(eve::BitmapFormat::L1, SongNoteMap::kStride, SongNoteMap::kRows));
          r.raw(dl::bitmap_transform_e(e));
          r.raw(r.color_word(Color::Text));
          r.raw(dl::begin(eve::Primitive::Bitmaps));
          r.raw(dl::cell(0));
        }
        const Rect blk = block_of(i);
        const int span = timeline_x(w, w.start[i + 1], width) - timeline_x(w, w.start[i], width);
        const std::int32_t a = notemap_scale_x(timeline_bars(static_cast<unsigned>(src.steps(slot))), span);
        const int map_w = notemap_fit_width(a, blk.w < 511 ? blk.w : 511);  // never a pixel that samples past column 127
        r.raw(dl::bitmap_source(SongNoteMap::addr(id)));
        r.raw(dl::bitmap_size(eve::Filter::Nearest, eve::Wrap::Border, eve::Wrap::Border, static_cast<std::uint32_t>(map_w), static_cast<std::uint32_t>(map_h)));
        r.raw(dl::bitmap_transform_a(a));
        r.raw(dl::vertex2f(blk.x * 16, top * 16));
      }
      if (open) {
        r.raw(dl::end());
        r.end_raw();
      }
      for (unsigned tr = 0; tr < tracks; ++tr) {  // the track numbers in the margin, centred on the track's lane
        const int lane_top = top + notemap_row_top(2u * tr, e);
        const int lane_h = notemap_row_top(2u * tr + 2u, e) - notemap_row_top(2u * tr, e);
        if (lane_h >= micro_h) {
          text.clear();
          append_dec(text, tr + 1u, 2);
          r.text(2, lane_top + (lane_h - micro_h) / 2, text.c_str(), Color::TextDim, Face::Micro);
        }
      }
    }

    // The energy curve (D8, D10): one column a slot, aligned with its block, as high as note density x loudness x track level adds up to over the active tracks (a muted track counts nothing),
    // in proportion to the biggest slot in view.
    std::uint8_t level[model::kMaxTracks];
    timeline_levels(*model.project, tracks, level);
    std::uint32_t energy[kTimelineMaxSlots];
    std::uint32_t peak = 0;
    for (unsigned i = 0; i < w.count; ++i) {
      const unsigned pattern = src.pattern(w.first + static_cast<int>(i));
      energy[i] = timeline_energy(view.density.get(*model.project, pattern), view.density.loudness(*model.project, pattern), level);
      peak = energy[i] > peak ? energy[i] : peak;
    }
    r.text(mx, floor_y - curve_h - micro_h - 2, "ENERGY", Color::TextDim, Face::Micro);
    for (unsigned i = 0; i < w.count; ++i) {
      const int h = timeline_energy_height(energy[i], peak, curve_h);
      if (h > 0) {
        const Rect blk = block_of(i);
        r.panel(Rect{blk.x, floor_y - h, blk.w, h}, Color::Vol);
      }
    }
  }

  // The playhead: a line through the blocks and lanes at the playing step.
  if (view.play.playing && marker >= w.first && marker < w.first + static_cast<int>(w.count)) {
    const unsigned i = static_cast<unsigned>(marker - w.first);
    const Rect blk = block_of(i);
    const int steps = src.steps(marker);
    const int at = steps > 0 ? (view.play.step < steps ? view.play.step : steps - 1) : 0;
    const int px = blk.x + (steps > 0 ? blk.w * at / steps : 0);
    r.panel(Rect{px, y0, 2, f.pane.y + f.pane.h - g - y0}, Color::Text);
  }
}

}  // namespace o3os::app
