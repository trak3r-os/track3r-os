// SPDX-License-Identifier: GPL-3.0-or-later
#include "song_pages.h"

#include <o3os/app/song_stage.h>
#include <o3os/app/song_timeline.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>
#include <o3os/ui/theme.h>

#include <cstring>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

constexpr unsigned kDemoPatternSteps = 16;  // a pattern-mode playback with no project (the demo list): one bar

// The caption of the NEXT block: why it is next. Words, not colour (colour is never the only cue).
constexpr const char* next_caption(StageNext k) noexcept {
  return k == StageNext::Jump ? "JUMP" : (k == StageNext::Loop ? "LOOP" : "NEXT");
}

// "SLOT 05", or "--" for a block with no slot (Pattern mode, an empty list).
void slot_text(core::FixedString<24>& text, int slot) noexcept {
  text.clear();
  if (slot < 0) {
    text.assign("--");
    return;
  }
  text.assign("SLOT ");
  append_dec(text, static_cast<unsigned>(slot), slot > 99 ? 3u : 2u);
}

// One block: a caption in the Label face, the slot and the pattern's name in the Display face (the largest), and a line in the Label face under them. `ink` is the text, `dim` the caption.
void draw_block(Renderer& r, const Rect& box, const SongSource& src, int slot, unsigned pattern, const char* caption, unsigned steps, Color ink, Color dim) noexcept {
  const ui::Theme& t = r.theme();
  const int g = t.metrics.row_pitch / 3;
  const int lab_h = t.geometry(Face::Label).cell_h;
  const int disp_h = t.geometry(Face::Display).cell_h;
  const int disp_w = t.geometry(Face::Display).cell_w;
  const int x = box.x + 2 * g;
  int y = box.y + g;
  r.text(x, y, caption, dim, Face::Label);
  y += lab_h + g;
  core::FixedString<24> text;
  slot_text(text, slot);
  r.text(x, y, text.c_str(), ink, Face::Display);
  y += disp_h + g;
  const int fit = (box.w - 4 * g) / disp_w;  // characters of the Display face that fit
  text.clear();
  const char* name = slot >= 0 ? src.name(slot) : "";
  if (pattern == 0u) {
    text.assign("--");
  } else if (name[0] != '\0') {
    const std::size_t n = std::strlen(name);
    text.append(name, n < static_cast<std::size_t>(fit) ? n : static_cast<std::size_t>(fit));  // cut to the block
  } else {
    text.assign("PATTERN ");
    append_dec(text, pattern, pattern > 99u ? 3u : 2u);
  }
  r.text(x, y, text.c_str(), ink, Face::Display);
  y += disp_h + g;
  text.clear();
  if (pattern != 0u) {
    text.assign("PATTERN ");
    append_dec(text, pattern, pattern > 99u ? 3u : 2u);
    text.append("   ");
    append_uint(text, (steps + kStageStepsPerBar - 1u) / kStageStepsPerBar);
    text.append(steps <= kStageStepsPerBar ? " BAR" : " BARS");
  }
  r.text(x, y, text.c_str(), dim, Face::Label);
}

// OS-SONG-TIMELINE: a colour per pattern, four fills by (pattern - 1) % 4, with the text of the pair the contrast table lists for each (colour is never the only cue: the number is on the block).
constexpr Color kPatternFill[4] = {Color::Sel, Color::Play, Color::Accent, Color::Rec};
constexpr Color kPatternInk[4] = {Color::SelText, Color::PlayText, Color::AccentText, Color::SelText};

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
  const unsigned tracks = model.project != nullptr ? model.project->active_tracks() : 0u;
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

  // The slots as blocks as wide as they have bars; one that names a pattern the project does not have is an outline.
  for (unsigned i = 0; i < w.count; ++i) {
    const int slot = w.first + static_cast<int>(i);
    const unsigned pattern = src.pattern(slot);
    const Rect blk = block_of(i);
    const unsigned colour = (pattern + 3u) % 4u;
    const bool exists = src.exists(slot);
    if (exists) {
      r.panel(blk, kPatternFill[colour]);
    } else {
      r.frame(blk, Color::Empty);
    }
    text.clear();
    append_uint(text, pattern);
    if (r.text_width(text.c_str(), Face::Micro) + 6 <= blk.w) {  // the number when it fits
      r.text(blk.x + 3, y0 + (bh - micro_h) / 2, text.c_str(), exists ? kPatternInk[colour] : Color::TextDim, Face::Micro);
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

  // The thumbnails: a lane a track, in each slot one bar as high as the pattern's note density in that track (densities are cached by the pattern's generation: a frame reads a table).
  if (model.project != nullptr && tracks > 0u) {
    const int top = under + 4 + g;
    const int floor_y = f.pane.y + f.pane.h - g;                 // the bottom of the page: the energy curve stands on it
    const int curve_h = 2 * m.row_pitch;                         // its tallest column
    const int bottom = floor_y - curve_h - (micro_h + 2) - g;    // the lanes end above the curve's caption
    int lane = (bottom - top) / static_cast<int>(tracks);
    lane = lane > m.row_pitch ? m.row_pitch : lane;
    if (lane >= 3) {
      for (unsigned i = 0; i < w.count; ++i) {
        const Rect blk = block_of(i);
        const std::uint64_t dens = view.density.get(*model.project, src.pattern(w.first + static_cast<int>(i)));  // (0 for a pattern that does not exist)
        for (unsigned tr = 0; tr < tracks; ++tr) {
          const unsigned d = SongDensity::nibble(dens, tr);
          if (d != 0u) {
            const int h = 1 + static_cast<int>(d) * (lane - 2) / static_cast<int>(SongDensity::kMax);
            r.panel(Rect{blk.x, top + static_cast<int>(tr) * lane + (lane - 1 - h), blk.w, h}, Color::Text);
          }
        }
      }
      if (lane >= micro_h) {  // the track numbers in the margin
        for (unsigned tr = 0; tr < tracks; ++tr) {
          text.clear();
          append_dec(text, tr + 1u, 2);
          r.text(2, top + static_cast<int>(tr) * lane + (lane - micro_h) / 2, text.c_str(), Color::TextDim, Face::Micro);
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

void draw_song_stage(Renderer& r, const Frame& f, const Model& model, const ViewState& view, Rect covered) noexcept {
  if (covers(covered, f.pane.x, f.pane.y, f.pane.w, f.pane.h)) {
    return;
  }
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const SongSource src(model);
  unsigned pattern_steps = kDemoPatternSteps;
  if (model.project != nullptr) {
    const std::uint32_t len = model.project->pattern_length(view.play.pattern);
    pattern_steps = len != 0u ? len : model.project->default_pattern_length();
  }
  const StageState s = stage_state(src, view.play, view.song.clamped_pos(src.count()), pattern_steps);
  const bool running = s.mode != StageMode::Stopped;

  const int g = m.row_pitch / 3;
  const int lab_h = t.geometry(Face::Label).cell_h;
  const int disp_h = t.geometry(Face::Display).cell_h;
  const int mx = ui::kScreenW / 32;
  const int bw = (ui::kScreenW - 3 * mx) / 2;
  const int bh = 5 * g + 2 * lab_h + 2 * disp_h;
  const int y0 = f.pane.y + g;

  // NOW: a solid block in the playing green (the transport's colour) while it plays, in the selection blue as the cursor's preview while stopped.
  const Rect now = {mx, y0, bw, bh};
  const Color now_fill = running ? Color::Play : Color::Sel;
  const Color now_ink = running ? Color::PlayText : Color::SelText;
  r.panel(now, now_fill);
  draw_block(r, now, src, s.now_slot, s.now_pattern, running ? (s.mode == StageMode::Pattern ? "PATTERN" : "NOW") : "CURSOR", s.steps, now_ink, now_ink);

  // NEXT: an outlined block on the pane (two pixels), orange for a live jump that waits.
  const Rect next = {2 * mx + bw, y0, bw, bh};
  const Color edge = s.next_kind == StageNext::Jump ? Color::Accent : Color::Sel;
  r.frame(next, edge);
  r.frame(Rect{next.x + 1, next.y + 1, next.w - 2, next.h - 2}, edge);
  const unsigned next_steps = s.next_slot >= 0 ? static_cast<unsigned>(src.steps(s.next_slot)) : s.steps;
  draw_block(r, next, src, s.next_slot, s.next_pattern, next_caption(s.next_kind), next_steps, Color::Text, Color::TextDim);

  // The countdown: the bars left in Display type, in words. Stopped it says so.
  int y = y0 + bh + 2 * g;
  r.text(mx, y, running ? (s.next_kind == StageNext::Jump ? "JUMP IN" : (s.next_kind == StageNext::Loop ? "LOOP IN" : "NEXT IN")) : "TRANSPORT", Color::TextDim, Face::Label);
  y += lab_h + g;
  core::FixedString<24> text;
  if (running) {
    append_uint(text, s.bars_left);
    text.append(s.bars_left == 1u ? " BAR" : " BARS");
  } else {
    text.assign("STOPPED");
  }
  r.text(mx, y, text.c_str(), Color::Text, Face::Display);
  y += disp_h + 2 * g;

  // The progress bar over the pattern that plays, a tick at every bar line, and under it the bar the playhead is in.
  const int bar_w = ui::kScreenW - 2 * mx;
  const Rect bar = {mx, y, bar_w, disp_h};
  const unsigned fill = running ? stage_progress(s.steps, s.step, s.ustep, static_cast<unsigned>(bar_w)) : 0u;
  ui::bar(r, bar, fill, static_cast<unsigned>(bar_w), Color::Empty, Color::Play);
  const unsigned bars = (s.steps + kStageStepsPerBar - 1u) / kStageStepsPerBar;
  for (unsigned b = 1; b < bars; ++b) {
    r.panel(Rect{mx + static_cast<int>(static_cast<unsigned>(bar_w) * b * kStageStepsPerBar / s.steps), y, 2, disp_h}, Color::Pane);
  }
  y += disp_h + g;
  text.clear();
  if (running && bars > 0u) {
    text.assign("BAR ");
    append_uint(text, stage_bar_of(s.steps, s.step));
    text.append(" OF ");
    append_uint(text, bars);
  }
  r.text(mx, y, text.c_str(), Color::TextDim, Face::Label);
}

}  // namespace o3os::app
