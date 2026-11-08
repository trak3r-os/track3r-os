// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// PREVIEW (OS-SONG3 S4, docs/specs/OS-SONG3.md section 3.4; new Community OS behaviour): the audition state of F1 in a TIMELINE lane. The slot's pattern loops with only the rectangle's tracks
// sounding, through the project's solo path; this class is the part that must never go wrong: it remembers the solo set that was in force when the audition began and hands it back when it ends,
// so the mute / solo state after the audition is the one before it, and a second end() (a screen change, Stop, F1 again) is a no-op, so no solo can be left stuck. It is not an undo unit: it holds
// no journal and writes nothing; the caller applies `mask()` and later `end()`'s result as plain (non-journaled) track solo flags. Sets are bit masks, bit n = track n (16 tracks). Main loop only, no heap.

#include <cstdint>

namespace o3os::app {

class SongPreview {
 public:
  // The solo set that makes tracks `first`..`last` (inclusive, 0-based, any order) the only ones that sound; bits past track 15 are dropped.
  static constexpr std::uint16_t mask_of(int first, int last) noexcept {
    if (first > last) {
      const int t = first;
      first = last;
      last = t;
    }
    if (first < 0) {
      first = 0;
    }
    if (last > 15) {
      last = 15;
    }
    std::uint16_t m = 0;
    for (int t = first; t <= last; ++t) {
      m = static_cast<std::uint16_t>(m | (1u << t));
    }
    return m;
  }

  bool active() const noexcept { return active_; }
  std::uint16_t mask() const noexcept { return mask_; }    // the solo set to apply while the audition runs
  std::uint16_t saved() const noexcept { return saved_; }  // the solo set to put back

  // Starts the audition with `current_solo` as the set in force now. Returns the set to apply. Only called while not active (F1 while an audition runs ends it, it never moves it),
  // so the saved set is always the one from before the audition.
  std::uint16_t begin(std::uint16_t current_solo, int first, int last) noexcept {
    saved_ = current_solo;
    active_ = true;
    mask_ = mask_of(first, last);
    return mask_;
  }

  // Ends the audition and returns the solo set to restore; `restore` says whether there is anything to restore (false when no audition ran: the caller must then leave the solo flags alone).
  std::uint16_t end(bool& restore) noexcept {
    restore = active_;
    active_ = false;
    mask_ = 0;
    return saved_;
  }

 private:
  bool active_ = false;
  std::uint16_t mask_ = 0;
  std::uint16_t saved_ = 0;
};

}  // namespace o3os::app
