// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-MIDIREC-P1 S4 (docs/specs/OS-MIDIREC-P1.md, "Grid mark of a lane"): what the Pattern grid says about the MIDI lane of one track,
// step by step. A step is in one of four states: "starts" (a lane note starts in it), "held" (a lane note that started earlier is still
// sounding in it), "CC" (it holds lane CC events) or "none". A CC has no length, so a step between two CC events is "none".
//
// The walk is bounded: it reads the lane events below the end of the visible rows (at most the lane's 4096) and touches only the
// visible steps; a note that started above the window marks the steps it still sounds in, at most the window's rows. Main loop only
// (the draw), never the audio path; no allocation.

#include <o3os/model/lane_store.h>
#include <o3os/model/limits.h>

#include <algorithm>
#include <cstdint>

namespace o3os::app {

// The grid's display-list budget (ADR 0010 section 9; sim::kBudgetWords pins the same number, tests/budget_test.cpp).
inline constexpr unsigned kPatternBudgetWords = 1792;

struct LaneMark {
  std::uint8_t starts = 0;  // lane notes that start in the step (saturates at 255; the grid shows at most 99)
  std::uint8_t ccs = 0;     // lane CC events in the step (same)
  bool held = false;        // a lane note that started in an earlier step still sounds in this one (and none starts here)

  bool any() const noexcept { return starts != 0 || ccs != 0 || held; }
  // The text "MID nn cc" is shown on a step that starts a note or holds a CC; a held step reads "MID" alone.
  bool counts() const noexcept { return starts != 0 || ccs != 0; }
};

// Fills out[0 .. rows) for the steps first_row .. first_row + rows - 1 of a track of `track_len` steps: a row outside 0 .. track_len - 1
// gets no mark (events at or past the track's length are deleted by a length cut, A17).
inline void compute_lane_marks(const model::LaneStore::View& lane, int first_row, int rows, int track_len, LaneMark* out) noexcept {
  for (int i = 0; i < rows; ++i) {
    out[i] = LaneMark{};
  }
  if (lane.count == 0 || lane.events == nullptr) {
    return;
  }
  constexpr std::uint32_t kTps = model::kLaneTicksPerStep;
  const int lo_row = std::max(first_row, 0);
  const int hi_row = std::min(first_row + rows, track_len);  // exclusive
  if (hi_row <= lo_row) {
    return;
  }
  const std::uint32_t t_lo = static_cast<std::uint32_t>(lo_row) * kTps;
  const std::uint32_t t_hi = static_cast<std::uint32_t>(hi_row) * kTps;
  for (std::uint32_t n = 0; n < lane.count; ++n) {
    const model::LaneEvent& e = lane.events[n];
    if (e.tick >= t_hi) {
      break;  // sorted by tick: nothing later is visible
    }
    const unsigned kind = e.kind_channel >> 4;
    if (kind == 1u) {
      if (e.tick >= t_lo) {
        LaneMark& m = out[static_cast<int>(e.tick / kTps) - first_row];
        m.ccs = static_cast<std::uint8_t>(std::min<unsigned>(m.ccs + 1u, 255u));
      }
    } else if (kind == 0u) {
      const std::uint32_t start = e.tick / kTps;
      const std::uint32_t last = (e.tick + std::max<std::uint32_t>(e.len, 1u) - 1u) / kTps;  // the step of its last sounding tick
      if (e.tick >= t_lo) {
        LaneMark& m = out[static_cast<int>(start) - first_row];
        m.starts = static_cast<std::uint8_t>(std::min<unsigned>(m.starts + 1u, 255u));
      }
      const int from = std::max(static_cast<int>(start) + 1, lo_row);
      const int to = std::min(static_cast<int>(last), hi_row - 1);
      for (int row = from; row <= to; ++row) {
        out[row - first_row].held = true;
      }
    }
  }
  for (int i = 0; i < rows; ++i) {
    if (out[i].starts != 0) {
      out[i].held = false;  // "starts" wins over "held"
    }
  }
}

}  // namespace o3os::app
