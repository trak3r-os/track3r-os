// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-MTC slice A1: the song time map (docs/specs/OS-MTC.md section 3.2). Pure, main-loop context: it reads the project the main loop owns
// and walks the order list exactly as the sequencer plays it, so that every step of the song has one exact time (Q32 samples from the
// song's first step) and one timing state (the tempo and swing in force there). No allocation, no double, no I/O; a walk is bounded by
// kWalkCap steps and costs at most 16 tracks x 2 FX reads a step.
//
// A walk starts where a run starts (Song mode: order position 0, step 0; Pattern mode: step 0 of the pattern), with the project's tempo and
// swing (a run's start state: a start resets the Tempo FX and the Swing FX, and the performance override went at the last stop). The walk
// does NOT know what is live while the song plays: the performance tempo, half / double time, the tempo knob during a run, live switches
// and jumps (assumption A-4). Song mode loops the order list as the sequencer's default does (Config::song_loop); Pattern mode loops the
// pattern and the time runs on through the loops (A-5, Q10).
//
// The walk is the sequencer's own: a slot's pattern length (Project::pattern_length, the longest of the active tracks, the default length
// for a missing pattern, as Sequencer::song_pattern_len), every track's row k mod len_t (seq/rows.h track_length, polymetric tracks), the
// timing FX of the two FX columns of each row as pre_fx runs them (per active track, column 1 then column 0, values sanitised as read_row
// does), the step length through step_length_q32 (sequencer.h) on core::samples_per_tick_q32, the swing pair parity from the run start,
// and Break (13) and Order jump (45) followed as pattern_end follows them. One thing is run state and not in the map: a Random value FX (16)
// rewrites the values of both columns of its row before they run, so a timing FX sharing a row with one draws its value from the run's RNG.

#include <o3os/audio/play_position.h>
#include <o3os/core/qtime.h>
#include <o3os/model/project.h>
#include <o3os/seq/fx.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>

namespace o3os::seq {

// A walk is bounded: a song that loops on itself through an Order jump is cut here (the answer is then the last step walked, `capped`).
// The bound also keeps the Q32 time inside 64 bits: the longest step (40 BPM, the widest swing factor 150/100) is about 1.07e14 in Q32,
// so kWalkCap steps sum to about 7.0e18 < 2^64 (1.8e19); raising kWalkCap must re-check that (the limit is about 173,000 steps at that tempo).
inline constexpr std::uint32_t kWalkCap = 65536u;

// A step of the song: its exact time from the song start and the timing state there.
struct SongPoint {
  core::Q32 time;            // Q32 samples from the song start to the start of this step (Song mode: order 0 step 0; Pattern mode: step 0 of the pattern)
  std::uint32_t tempo_x100;  // the tempo in force at this step, BPM x 100 (after its row's FX, as the sequencer reads them: the step that holds a Tempo FX already plays at it)
  std::uint8_t swing;        // the swing in force at this step, 25..75
  std::uint16_t order;       // Song mode: the position in the order list; Pattern mode: audio::PlayPosition::kNoOrder
  std::uint8_t pattern;      // the pattern id
  std::uint16_t step;        // the step in the pattern, 0..len-1
  // OS-MTC B3 (the chase starts a run AT this step, so it needs the state the step's own row will change, not the one after): the tempo and swing in force before this step's row FX run, and the
  // swing pair's parity (true: the step is the second of its pair, so the run's first step lasts the odd factor). Added after `capped`; every field above is as A1 had it.
  std::uint32_t tempo_in_x100;
  std::uint8_t swing_in;
  bool odd;
  bool past_end;             // song_position_at with loop = false: the walk reached the end of the order list before `samples` (the fields are the last step walked; not a match)
  bool capped;               // NOT a match: the walk hit kWalkCap (the fields are the last step walked) or the target is one the song never plays
                             // (the fields echo the request, time 0). Callers must check `capped` before using the point (seeking, anchoring): a capped
                             // answer is never a position of the song.
};

// The time of a step, walked from the song start as a run from the song start plays it. Song mode: `order` is the position in the order
// list and `step` the step in that position's pattern. Pattern mode: `order` carries the pattern id (the render's request word has the
// order position 0xFF in Pattern mode) and the time counts from the pattern's step 0; a caller with a raw from_step wraps it by the
// pattern's length itself, as begin_run and locate() do. A target the song never plays — Song mode: an order past the list's end;
// Pattern mode: a pattern id that does not exist; either mode: a step past the target pattern's length — answers at once, without
// walking, with `capped`; a target the walk does not reach within kWalkCap answers the last step walked, `capped`.
SongPoint song_time_at(const model::Project& project, PlayMode mode, std::uint32_t order, std::uint32_t step) noexcept;

// The first step that starts at or after `samples`, with its exact time and state. The walk is the same one; Song mode loops the order
// list, Pattern mode loops the pattern named by `order` (the pattern id, as in song_time_at). A time past what kWalkCap steps reach
// answers the last step walked, `capped`.
SongPoint song_position_at(const model::Project& project, PlayMode mode, std::uint32_t order, core::Q32 samples, bool loop = true) noexcept;
// OS-MTC B3: `loop` false is the chase's walk (Q9 a: a time past the end of the song is silence, the unit waits for the master to locate back): the walk does not loop the order list (only the end of the
// list ends it, an Order jump back is the song's flow); a time at or past the end of the last slot answers `past_end`. Pattern mode loops the pattern whatever `loop` says (Q10).

// The map's classification of the Row FX of kFxInfo (seq/fx.h): does the type move the timeline? Every Row FX must be known to the map, so
// that a new one fails the guard test (tests/seq_song_time_test.cpp) until the map says what it does to time.
constexpr bool map_fx_is_timing(std::uint8_t type) noexcept {
  switch (type) {
    case fx::kBreakPattern:  // 13: the pattern ends at this step's boundary
    case fx::kTempo:         // 15: the tempo from this row on
    case fx::kSwing:         // 17: the swing from this row on
    case fx::kTempoFine:     // 44: added to the tempo of its row, and it stays
    case fx::kOrderJump:     // 45: the song carries on at that position at the pattern's end
      return true;
    default:
      return false;
  }
}
constexpr bool map_fx_known(std::uint8_t type) noexcept {
  switch (type) {
    case fx::kOff:
    case fx::kMicroMove:
    case fx::kRoll:
    case fx::kChance:
    case fx::kRandomNote:
    case fx::kRandomInstrument:
    case fx::kBreakPattern:
    case fx::kChord:
    case fx::kTempo:
    case fx::kRandomValue:
    case fx::kSwing:
    case fx::kGate:
    case fx::kArp:
    case fx::kTune:
    case fx::kSpeed:  // a timing FX of F6, but no length change: the ticks a step divides into, not the map's business
    case fx::kTempoFine:
    case fx::kOrderJump:
      return true;
    default:
      return type >= fx::kTrkArp && type <= fx::kCoreLast;  // 46..62, the tracker FX: a row's sound for its length, not its time
  }
}

}  // namespace o3os::seq
