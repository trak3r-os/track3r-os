// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Song screen's STAGE page (OS-SONG-STAGE, docs/specs/OS-SONG-VIEW.md): the arithmetic behind it, pure and main loop only. What is playing, what comes next, how many bars are
// left before it does, and how far the playhead is through the pattern. Nothing here allocates, draws or is saved.

#include <o3os/app/song_source.h>
#include <o3os/audio/play_position.h>

#include <cstdint>

namespace o3os::app {

inline constexpr unsigned kStageStepsPerBar = 16;    // four beats of four steps (seq::kStepsPerBar; the app does not link the sequencer's header)
inline constexpr unsigned kStageMicrosteps = 48;     // a step (audio::PlayPosition::Value::ustep is 0..47)

// Bars left in a pattern of `steps` steps with the playhead on step `step` (0-based): the bars still to run, the one the playhead is in counted, so the last bar says 1. 0 for a pattern with no
// steps; a playhead past the end (a pattern shortened while it played) counts as the last bar.
constexpr unsigned stage_bars_left(unsigned steps, unsigned step) noexcept {
  if (steps == 0u) {
    return 0u;
  }
  const unsigned left = step < steps ? steps - step : 1u;
  return (left + kStageStepsPerBar - 1u) / kStageStepsPerBar;
}

// The bar the playhead is in, 1-based and counted from the pattern's start, so it agrees with the bar ticks: step / 16, and a playhead past the end is the last bar. 0 for a pattern with no steps.
constexpr unsigned stage_bar_of(unsigned steps, unsigned step) noexcept {
  if (steps == 0u) {
    return 0u;
  }
  const unsigned bars = (steps + kStageStepsPerBar - 1u) / kStageStepsPerBar;
  const unsigned bar = step / kStageStepsPerBar;
  return (bar < bars ? bar : bars - 1u) + 1u;
}

// The filled part of a bar `width` pixels wide: the playhead's place in the pattern, to the microstep (so it moves smoothly). 0..width; a pattern with no steps fills nothing.
constexpr unsigned stage_progress(unsigned steps, unsigned step, unsigned ustep, unsigned width) noexcept {
  if (steps == 0u || width == 0u) {
    return 0u;
  }
  const unsigned total = steps * kStageMicrosteps;
  const unsigned done = (step < steps ? step : steps) * kStageMicrosteps + (ustep < kStageMicrosteps ? ustep : kStageMicrosteps - 1u);
  const unsigned at = done < total ? done : total;
  return (at * width) / total;  // (at most 6144 x a width of a few hundred: inside 32 bits, so no software 64-bit divide a frame)
}

// What the page says. `Stopped`: the cursor's slot is NOW, nothing counts. `Song`: the song row that plays. `Pattern`: a single pattern loops (no song row, or a row the list no longer has).
enum class StageMode : std::uint8_t { Stopped, Song, Pattern };
// Why the next block is the next: the following slot, a live jump (Rec) that waits, or the song's end that loops to slot 0.
enum class StageNext : std::uint8_t { None, Next, Jump, Loop };

struct StageState {
  StageMode mode = StageMode::Stopped;
  StageNext next_kind = StageNext::None;
  int now_slot = -1;           // -1: a pattern-mode playback, or an empty list
  int next_slot = -1;          // -1: no next block (Pattern mode, an empty list)
  unsigned now_pattern = 0;    // pattern ids, 0 for none
  unsigned next_pattern = 0;
  unsigned steps = 0;          // the length of the pattern that plays, in steps
  unsigned step = 0;
  unsigned ustep = 0;
  unsigned bars_left = 0;      // stage_bars_left(); 0 when stopped
};

// The state for `play` (the sequencer's position) over the list `src`. `cursor` is the Song cursor's slot (what a stopped page shows); `pattern_steps` is the length of play.pattern in steps, for a
// pattern-mode playback (the caller reads it from the project: the list has no slot to ask).
StageState stage_state(const SongSource& src, const audio::PlayPosition::Value& play, int cursor, unsigned pattern_steps) noexcept;

}  // namespace o3os::app
