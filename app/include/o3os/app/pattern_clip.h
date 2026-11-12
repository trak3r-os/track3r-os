// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pattern clipboard (OS-PATCOPY, new Community OS behaviour): the block that Copy took, `tracks` columns of `rows` steps, kept for the session (a pattern switch, an undo or a new
// document do not clear it). Room for the whole of a pattern, 16 tracks x 128 steps of 7 bytes = 14,336 bytes: it is a member of the App, which the device builds in SDRAM
// (g_app_storage), so it costs no SRAM. Main loop only.
//
// PAT-OPS-B (new Community OS behaviour, docs/specs/PAT-OPS.md): the same clip also holds a WHOLE pattern, the Copy of a held F1 (the PATTERN chooser): all 16 tracks x 128 steps plus the 16 track lengths, the
// name and the number it was taken from (49 bytes more). `whole()` says which it is; the Copy of the grid (begin()) makes a block clip again.
//
// OS-MIDIREC-P1 S7c (A16): a Copy of whole steps also carries the MIDI lane events of the block's tracks and tick range, in a 1,024-event buffer (8 KiB, SDRAM with the rest of the clip; it was 2,048, but with the 16 KiB take buffer the SDRAM_STATIC window overflowed, OS-MIDIREC-P1-CLIPSDRAM):
// the events of the clip's track 0, then track 1 and so on (`lane_events(t)`, `lane_count(t)`), their ticks counted from the block's first step (a whole pattern: from tick 0).
// `has_lanes()` is false when the Copy carried none (a column block, no lane store, or more events than the buffer holds): a paste then leaves the target's lanes alone.

#include <o3os/model/lane_store.h>
#include <o3os/model/limits.h>
#include <o3os/model/step.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::app {

class PatternClip {
 public:
  static constexpr std::uint32_t kCapacity = model::kMaxTracks * model::kMaxPatternLength;  // the largest block there is
  static constexpr std::uint32_t kLaneCapacity = 1024;  // S7c, A16: lane events in a clip (8 KiB; SDRAM_STATIC budget)

  bool empty() const noexcept { return rows_ == 0 || tracks_ == 0; }
  int rows() const noexcept { return rows_; }
  int tracks() const noexcept { return tracks_; }

  // Starts a new clip of tracks x rows; false (the old clip is kept) when it does not fit.
  bool begin(int tracks, int rows) noexcept {
    if (tracks < 1 || rows < 1 || static_cast<std::uint32_t>(tracks) * static_cast<std::uint32_t>(rows) > kCapacity) {
      return false;
    }
    tracks_ = tracks;
    rows_ = rows;
    whole_ = false;
    col0_ = 0;
    col1_ = 4;
    lanes_known_ = false;
    lane_total_ = 0;
    std::memset(lane_n_, 0, sizeof lane_n_);
    return true;
  }
  // The lane events (S7c). lanes_add puts the `n` events of clip track `t` (ascending, each track once) in the buffer, their ticks lowered by `origin`; false, nothing added, when the
  // buffer would pass kLaneCapacity (the caller then gives the lanes up). lanes_commit says the Copy carried lanes (the tracks not added have none).
  bool lanes_add(int t, const model::LaneEvent* events, std::uint32_t n, std::uint32_t origin) noexcept {
    if (t < 0 || t >= tracks_ || lane_total_ + n > kLaneCapacity) {
      return false;
    }
    model::LaneEvent* out = lane_ + lane_total_;
    for (std::uint32_t i = 0; i < n; ++i) {
      out[i] = events[i];
      out[i].tick = static_cast<std::uint16_t>(events[i].tick - origin);
    }
    lane_n_[t] = static_cast<std::uint16_t>(n);
    lane_total_ += n;
    return true;
  }
  void lanes_commit() noexcept { lanes_known_ = true; }
  void lanes_drop() noexcept {  // the Copy gives its lanes up (more events than the buffer holds)
    lanes_known_ = false;
    lane_total_ = 0;
    std::memset(lane_n_, 0, sizeof lane_n_);
  }
  bool has_lanes() const noexcept { return lanes_known_; }
  std::uint32_t lane_count(int t) const noexcept { return t >= 0 && t < static_cast<int>(model::kMaxTracks) ? lane_n_[t] : 0u; }
  // Every event's tick moved by `delta` (modulo 65536: a delta and its negative give the events back). The paste moves them to the target rows and back around its writes, so no copy is needed.
  void lanes_shift(int delta) noexcept {
    for (std::uint32_t i = 0; i < lane_total_; ++i) {
      lane_[i].tick = static_cast<std::uint16_t>(lane_[i].tick + delta);
    }
  }
  // The events of clip track `t` (lane_count(t) of them); the caller may move their ticks and must put them back (the paste does, around its writes).
  model::LaneEvent* lane_events(int t) noexcept {
    std::uint32_t at = 0;
    for (int i = 0; i < t && i < static_cast<int>(model::kMaxTracks); ++i) {
      at += lane_n_[i];
    }
    return lane_ + at;
  }
  // OS-AUDIT-56 (new Community OS behaviour): the clip of a column block holds whole steps but only the columns col0..col1 (Column values) are meant; a paste writes those and leaves the rest of the
  // target steps. begin() makes it all five again. Memory only, like the whole clip (no file or project holds it).
  void set_columns(int col0, int col1) noexcept {
    col0_ = static_cast<std::int8_t>(col0);
    col1_ = static_cast<std::int8_t>(col1);
  }
  int col0() const noexcept { return col0_; }
  int col1() const noexcept { return col1_; }
  bool whole_steps() const noexcept { return col0_ == 0 && col1_ == 4; }
  // The clip is a whole pattern (PAT-OPS-B): call after begin(kMaxTracks, kMaxPatternLength) and filling the columns. `lengths` are the 16 track lengths, `name` the pattern's (kPatternNameMax + 1 bytes,
  // NUL-terminated), `source` the pattern number it was taken from.
  void set_whole(const std::uint8_t* lengths, const char* name, std::uint8_t source) noexcept {
    std::memcpy(lengths_, lengths, sizeof lengths_);
    std::memcpy(name_, name, sizeof name_);
    name_[model::kPatternNameMax] = '\0';
    source_ = source;
    whole_ = true;
  }
  bool whole() const noexcept { return whole_; }  // (begin() ends it)
  const std::uint8_t* lengths() const noexcept { return lengths_; }  // (a whole clip)
  const char* name() const noexcept { return name_; }                // (a whole clip)
  unsigned source() const noexcept { return source_; }               // (a whole clip) the pattern number it was copied from
  // The column `t` of the clip: `rows()` steps.
  model::Step* column(int t) noexcept { return steps_ + static_cast<std::size_t>(t) * static_cast<std::size_t>(rows_); }
  const model::Step* column(int t) const noexcept { return steps_ + static_cast<std::size_t>(t) * static_cast<std::size_t>(rows_); }

 private:
  model::Step steps_[kCapacity];  // (only the rows x tracks that begin() set and Copy wrote are ever read)
  alignas(model::LaneEvent) model::LaneEvent lane_[kLaneCapacity];  // S7c: main loop only, never read by the render
  std::uint32_t lane_total_ = 0;
  std::uint16_t lane_n_[model::kMaxTracks] = {};
  bool lanes_known_ = false;
  int rows_ = 0;
  int tracks_ = 0;
  bool whole_ = false;
  std::int8_t col0_ = 0;
  std::int8_t col1_ = 4;
  std::uint8_t source_ = 0;
  std::uint8_t lengths_[model::kMaxTracks] = {};
  char name_[model::kPatternNameMax + 1] = {};
};

static_assert(sizeof(model::LaneEvent) * PatternClip::kLaneCapacity == 8192, "A16: the clip's lane buffer is 8 KiB (SDRAM_STATIC budget)");

}  // namespace o3os::app
