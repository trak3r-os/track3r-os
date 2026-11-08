// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/song_view.h>

namespace o3os::app {

bool SongView::set(int pos, int count) noexcept {
  const int last = count > 0 ? count - 1 : 0;
  pos = pos < 0 ? 0 : (pos > last ? last : pos);
  const int before_pos = pos_;
  const int before_top = top_;
  pos_ = pos;
  if (pos_ < top_) {
    top_ = pos_;
  } else if (pos_ >= top_ + kVisibleRows) {
    top_ = pos_ - kVisibleRows + 1;
  }
  return pos_ != before_pos || top_ != before_top;
}

bool SongView::move(int delta, int count) noexcept {
  // Clamp the step first so a huge delta (an encoder fed from data) cannot overflow the sum.
  const int d = delta < -kMaxStep ? -kMaxStep : (delta > kMaxStep ? kMaxStep : delta);
  return set(pos_ + d, count);
}

bool SongView::fit(int count) noexcept {
  const int before_pos = pos_;
  const int before_top = top_;
  const int last = count > 0 ? count - 1 : 0;
  top_ = top_ > last - kVisibleRows + 1 ? last - kVisibleRows + 1 : top_;  // not scrolled past the end of the list
  top_ = top_ < 0 ? 0 : top_;
  (void)set(pos_, count);  // the cursor inside the list, the window on the cursor
  const bool range_gone = anchor_ >= count;
  if (range_gone) {
    anchor_ = -1;  // the list got shorter than the range's far end: the range is gone
  }
  return pos_ != before_pos || top_ != before_top || range_gone;
}

// ---- the track column (OS-SONGMAP) ------------------------------------------------------------------------------------------

void SongView::follow_track(int cells) noexcept {
  const int most = cells > kLanes ? cells - kLanes : 0;
  if (col_ <= 0) {
    tbase_ = 0;  // on PAT (or the block row) the window is back at the first track
  } else if (col_ - 1 < tbase_) {
    tbase_ = col_ - 1;
  } else if (col_ - 1 >= tbase_ + kLanes) {
    tbase_ = col_ - kLanes;
  }
  tbase_ = tbase_ > most ? most : tbase_;
}

bool SongView::step_col(int delta, int cells) noexcept {
  const bool had = leave_edit();
  const int before = col_;
  const int top = cells > 0 ? cells : 0;
  const int d = delta < -1 ? -1 : (delta > 1 ? 1 : delta);  // one cell a press
  col_ = col_ + d < 0 ? 0 : (col_ + d > top ? top : col_ + d);
  follow_track(cells);
  return col_ != before || had;
}

bool SongView::set_track(int track, int cells) noexcept {
  if (track < 0 || track >= cells) {
    return false;
  }
  const bool had = leave_edit();
  const int before = col_;
  col_ = track + 1;
  follow_track(cells);
  return col_ != before || had;
}

bool SongView::extend_tracks(int delta, int cells) noexcept {
  if (col_ <= 0 || cells <= 0) {
    return false;  // on PAT: nothing to extend
  }
  const bool started = tanchor_ < 0;
  if (started) {
    tanchor_ = col_ - 1;
  }
  const int d = delta < -1 ? -1 : (delta > 1 ? 1 : delta);
  const int before = col_;
  col_ = col_ + d < 1 ? 1 : (col_ + d > cells ? cells : col_ + d);
  tempo_focus_ = false;
  follow_track(cells);
  return col_ != before || started;
}

bool SongView::fit_cols(int cells) noexcept {
  const int top = cells > 0 ? cells : 0;
  const bool moved = col_ > top;
  col_ = moved ? top : col_;
  const bool gone = tanchor_ >= top;
  if (gone || col_ == 0) {
    tanchor_ = -1;
  }
  const int window = tbase_;
  follow_track(cells);
  return moved || gone || tbase_ != window;
}

bool SongView::shift_range(int delta, int count) noexcept {
  if (count <= 0) {
    return false;
  }
  const int d = delta < -1 ? -1 : (delta > 1 ? 1 : delta);  // one position a step
  if (anchor_ >= 0) {
    anchor_ = anchor_ + d < 0 ? 0 : (anchor_ + d >= count ? count - 1 : anchor_ + d);
  }
  tempo_focus_ = false;
  (void)set(pos_ + d, count);
  return true;
}

bool SongView::extend(int delta, int count) noexcept {
  if (count <= 0) {
    return false;
  }
  const bool started = anchor_ < 0;
  if (started) {
    anchor_ = pos_ < count ? pos_ : count - 1;
  }
  return move(delta, count) || started;
}

}  // namespace o3os::app
