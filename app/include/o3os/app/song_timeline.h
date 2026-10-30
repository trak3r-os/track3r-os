// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Song screen's TIMELINE page (OS-SONG-TIMELINE, docs/specs/OS-SONG-VIEW.md): the arithmetic behind it, pure and main loop only. Which slots are in view, where each one's block starts
// and ends along the page, so a slot is as wide as it has bars. Nothing here allocates, draws or is saved.

#include <o3os/app/song_source.h>

#include <cstdint>

namespace o3os::app {

inline constexpr unsigned kTimelineBefore = 4;      // slots shown before the cursor's (D6)
inline constexpr unsigned kTimelineMaxSlots = 40;   // at most this many slots in view (D7)
inline constexpr unsigned kTimelineMaxBars = 48;    // and at most this many bars
// The display-list bound is structural: a slot costs its tracks' thumbnail rectangles (2 words each) plus a fixed overhead (its block, the colour change, the number, the energy column: measured
// 11.4 words at worst, counted as kTimelineSlotOverhead rectangles = 12 words), so slots x (tracks + kTimelineSlotOverhead) is held to kTimelineSlotBudget whatever the track count, and the page
// is at most about 400 (the rest of the page) + 2 x tracks + 2 x kTimelineSlotBudget = 1673 words of the 1792; measured worst 1500 after OS-SONG-VIEW2 V3/V5 (the blocks' passes go by colour; it was 1650; song/timeline_worst sweeps every track count, 1 to 16; song_timeline_test checks the arithmetic).
inline constexpr unsigned kTimelineSlotOverhead = 6;   // rectangle-equivalents a slot costs besides its tracks' rectangles
inline constexpr unsigned kTimelineSlotBudget = 620;   // slots x (tracks + kTimelineSlotOverhead) at most this: 16 tracks show 28 slots, 12 show 34, 9 or fewer 40
inline constexpr unsigned kTimelineMinBars = 16;    // the page is at least this many bars wide: a short song does not stretch its few blocks over the whole width

// The slots in view. `first` is the song position of the first, `count` how many follow (0 for an empty list). `start[i]` is the bar at which slot first+i begins, counted from the window's left
// edge, and start[count] is the window's length in bars; `den` is the width of the page in bars (the window's bars, at least kTimelineMinBars).
struct TimelineWindow {
  int first = 0;
  unsigned count = 0;
  unsigned den = kTimelineMinBars;
  unsigned start[kTimelineMaxSlots + 1] = {};
};

// The bars a slot of `steps` steps takes: ceil(steps / 16), at least 1.
constexpr unsigned timeline_bars(unsigned steps) noexcept { return steps <= 16u ? 1u : (steps + 15u) / 16u; }

// The most slots in view with `tracks` active tracks: kTimelineMaxSlots, or fewer so that slots x (tracks + kTimelineSlotOverhead) stays within kTimelineSlotBudget (16 tracks show 28 slots).
constexpr unsigned timeline_slot_cap(unsigned tracks) noexcept {
  const unsigned fit = kTimelineSlotBudget / (tracks + kTimelineSlotOverhead);
  return fit < kTimelineMaxSlots ? fit : kTimelineMaxSlots;
}

// The window for the cursor at `cursor` over the list `src` with `tracks` active tracks (at most timeline_slot_cap(tracks) slots). It starts kTimelineBefore slots before the cursor and runs to the end of the song or the first limit: the cursor's slot is always inside it. A pure function of its arguments.
TimelineWindow timeline_window(const SongSource& src, int cursor, unsigned tracks) noexcept;

// ---- the energy curve (second commit of OS-SONG-TIMELINE, D8, D10) -----------------------------------------------------------------------------------------------------------------------
// What a slot adds up to: for each active track, note density (0..15) x loudness (0..15) x the track's level (0..100), summed. A muted track counts nothing, and nothing but the soloed tracks
// count while one is soloed (the Mixer's rule). A pure sum: the biggest is 16 x 15 x 15 x 100 = 360,000, inside 32 bits.

// The level each of the first `tracks` tracks counts at: its volume, 0 when it is muted or another track is soloed. Tracks past `tracks` stay 0.
void timeline_levels(const model::Project& project, unsigned tracks, std::uint8_t (&level)[model::kMaxTracks]) noexcept;

// The energy of one slot from its pattern's packed densities and loudness (song_map.h, SongDensity: 4 bits a track) at the tracks' `level`.
constexpr std::uint32_t timeline_energy(std::uint64_t density, std::uint64_t loudness, const std::uint8_t (&level)[model::kMaxTracks]) noexcept {
  std::uint32_t sum = 0;
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    sum += static_cast<std::uint32_t>((density >> (4u * t)) & 15u) * static_cast<std::uint32_t>((loudness >> (4u * t)) & 15u) * level[t];
  }
  return sum;
}

// The height in pixels of a slot of `energy` in a strip `max_height` tall: in proportion to the window's `peak` (the biggest energy in view), so the curve always uses the strip and shows the shape of
// the song around the cursor. 0 for nothing (and for a window of nothing); at least 1 pixel for anything.
constexpr int timeline_energy_height(std::uint32_t energy, std::uint32_t peak, int max_height) noexcept {
  if (energy == 0u || peak == 0u || max_height <= 0) {
    return 0;
  }
  const std::uint32_t h = (energy * static_cast<std::uint32_t>(max_height)) / peak;  // (360,000 x a strip of a few hundred pixels: inside 32 bits)
  return h < 1u ? 1 : static_cast<int>(h > static_cast<std::uint32_t>(max_height) ? static_cast<std::uint32_t>(max_height) : h);
}

// The x of bar `bars` of the window on a page `width` pixels wide: width * bars / den, so slot i spans timeline_x(w, start[i], width) up to timeline_x(w, start[i + 1], width).
constexpr int timeline_x(const TimelineWindow& w, unsigned bars, int width) noexcept {
  return width <= 0 ? 0 : static_cast<int>((static_cast<unsigned>(width) * bars) / w.den);
}

}  // namespace o3os::app
