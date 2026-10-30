// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/song_stage.h>

namespace o3os::app {

StageState stage_state(const SongSource& src, const audio::PlayPosition::Value& play, int cursor, unsigned pattern_steps) noexcept {
  StageState s;
  const int count = src.count();
  if (!play.playing) {
    // Stopped: the cursor's slot (inside the list) is NOW and the one after it NEXT, as a preview; nothing counts.
    if (count > 0) {
      s.now_slot = cursor < 0 ? 0 : (cursor >= count ? count - 1 : cursor);
      s.now_pattern = src.pattern(s.now_slot);
      s.steps = static_cast<unsigned>(src.steps(s.now_slot));
      s.next_slot = s.now_slot + 1 < count ? s.now_slot + 1 : 0;
      s.next_pattern = src.pattern(s.next_slot);
      s.next_kind = s.now_slot + 1 < count ? StageNext::Next : StageNext::Loop;
    }
    return s;
  }
  s.step = play.step;
  s.ustep = play.ustep;
  if (play.order == audio::PlayPosition::kNoOrder || static_cast<int>(play.order) >= count) {
    s.mode = StageMode::Pattern;  // Pattern mode: it loops
    s.now_pattern = play.pattern;
    s.steps = pattern_steps;
    s.next_pattern = play.pattern;
    s.next_kind = StageNext::Loop;
    s.bars_left = stage_bars_left(s.steps, s.step);
    return s;
  }
  s.mode = StageMode::Song;
  s.now_slot = static_cast<int>(play.order);
  s.now_pattern = src.pattern(s.now_slot);
  s.steps = static_cast<unsigned>(src.steps(s.now_slot));
  if (play.queued != audio::PlayPosition::kNoOrder && static_cast<int>(play.queued) < count) {
    s.next_slot = static_cast<int>(play.queued);
    s.next_kind = StageNext::Jump;
  } else if (s.now_slot + 1 < count) {
    s.next_slot = s.now_slot + 1;
    s.next_kind = StageNext::Next;
  } else {
    s.next_slot = 0;
    s.next_kind = StageNext::Loop;
  }
  s.next_pattern = src.pattern(s.next_slot);
  s.bars_left = stage_bars_left(s.steps, s.step);
  return s;
}

}  // namespace o3os::app
