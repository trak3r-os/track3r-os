// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/song_timeline.h>

namespace o3os::app {

void timeline_levels(const model::Project& project, unsigned tracks, std::uint8_t (&level)[model::kMaxTracks]) noexcept {
  bool solo = false;
  for (unsigned t = 0; t < tracks && t < model::kMaxTracks; ++t) {
    const model::TrackSlot* slot = project.track(t);
    solo = solo || (slot != nullptr && (slot->flags & model::kTrackSolo) != 0u);
  }
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    const model::TrackSlot* slot = t < tracks ? project.track(t) : nullptr;
    level[t] = 0;
    if (slot != nullptr && (slot->flags & model::kTrackMute) == 0u && (!solo || (slot->flags & model::kTrackSolo) != 0u)) {
      level[t] = slot->volume < 100u ? slot->volume : std::uint8_t{100};
    }
  }
}

TimelineWindow timeline_window(const SongSource& src, int cursor, unsigned tracks) noexcept {
  TimelineWindow w;
  const int length = src.count();
  if (length <= 0) {
    return w;
  }
  const int at = cursor < 0 ? 0 : (cursor >= length ? length - 1 : cursor);
  w.first = at > static_cast<int>(kTimelineBefore) ? at - static_cast<int>(kTimelineBefore) : 0;
  const unsigned slots = timeline_slot_cap(tracks < model::kMaxTracks ? tracks : static_cast<unsigned>(model::kMaxTracks));
  unsigned bars = 0;
  while (w.count < slots && w.first + static_cast<int>(w.count) < length) {
    const int steps = src.steps(w.first + static_cast<int>(w.count));
    const unsigned b = timeline_bars(steps > 0 ? static_cast<unsigned>(steps) : 0u);
    if (w.count > 0u && bars + b > kTimelineMaxBars) {
      break;
    }
    w.start[w.count] = bars;
    bars += b;
    ++w.count;
  }
  w.start[w.count] = bars;
  w.den = bars > kTimelineMinBars ? bars : kTimelineMinBars;
  return w;
}

}  // namespace o3os::app
