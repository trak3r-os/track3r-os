// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the Pattern screen reads (OS-009 P1a.1, D1): one read-only view of a pattern, backed by the attached document or, when there
// is none, by the demo of OS-001b. The screen, the cursor and the edit actions ask this and never look at where the steps are.
//
//   document  Project::pattern(id): the steps and the length of each track, the track slots (type, name, mute and solo) and the
//             number of active tracks (12 in a new project, at most 16). A pattern that does not exist yet shows empty with the
//             project's default length, and the first edit creates it in its own undo unit.
//   demo      the 12 x 64 grid of fill_demo(): read-only, no edit runs on it.
//
// Rows past a track's own length are still readable (the model keeps them); the screen draws them dimmed and the edit actions refuse
// them (D7). Nothing here allocates or writes.

#include <o3os/app/mix_board.h>
#include <o3os/app/model.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/track_state.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/project.h>

#include <cstdint>

namespace o3os::app {

class PatternSource {
 public:
  static constexpr int kDemoPattern = 3;   // what the demo strip says: PAT 03/12
  static constexpr int kDemoPatterns = 12;
  static constexpr int kDemoJump = 4;      // and JUMP 04

  // The model's document when one is attached, else the demo.
  PatternSource(const Model& model, unsigned pattern_id) noexcept
      : project_(model.project),
        pattern_(model.project != nullptr ? model.project->pattern(pattern_id) : nullptr),
        demo_(&model.pattern),
        mix_(model.mix),
        lanes_(model.lanes),
        id_(pattern_id) {}
  // The demo alone (the tests and the sweeps of the simulator).
  explicit PatternSource(const PatternData& demo) noexcept : demo_(&demo) {}

  bool is_demo() const noexcept { return project_ == nullptr; }
  // The document's pattern exists (always false for the demo).
  bool exists() const noexcept { return pattern_ != nullptr; }
  unsigned id() const noexcept { return id_; }
  // OS-MIDIREC-P1 S4: the MIDI lane of one track of this pattern ({nullptr, 0} for the demo, a missing pattern or an empty lane).
  model::LaneStore::View lane(int track) const noexcept {
    if (lanes_ == nullptr || pattern_ == nullptr || track < 0 || track >= static_cast<int>(model::kMaxTracks)) {
      return {nullptr, 0};
    }
    return lanes_->lane(id_, static_cast<std::uint32_t>(track));
  }
  const model::Project* project() const noexcept { return project_; }

  // Tracks the user can reach: the project's active tracks, 12 for the demo.
  int tracks() const noexcept { return project_ != nullptr ? static_cast<int>(project_->active_tracks()) : kTracks; }
  // The pattern's length in steps: the longest of the active tracks, the project's default for a pattern that does not exist yet.
  int length() const noexcept {
    if (project_ == nullptr) {
      return kSteps;
    }
    return pattern_ != nullptr ? static_cast<int>(project_->pattern_length(id_)) : static_cast<int>(project_->default_pattern_length());
  }
  // The steps of one track that play: its own length (the model allows tracks of different lengths).
  int track_length(int track) const noexcept {
    if (project_ == nullptr) {
      return kSteps;
    }
    if (pattern_ == nullptr) {
      return static_cast<int>(project_->default_pattern_length());
    }
    return track >= 0 && track < static_cast<int>(model::kMaxTracks) ? pattern_->track_length[track] : 0;
  }
  // The step at (track, row): empty for a track or a row the model does not have, and for a pattern that does not exist.
  model::Step step(int track, int row) const noexcept {
    if (track < 0 || row < 0 || row >= static_cast<int>(model::kMaxPatternLength)) {
      return model::kEmptyStep;
    }
    if (project_ == nullptr) {
      return track < kTracks && row < kSteps ? demo_->steps[track][row] : model::kEmptyStep;
    }
    return pattern_ != nullptr && track < static_cast<int>(model::kMaxTracks) ? pattern_->steps[track][row] : model::kEmptyStep;
  }

  // The track header: type letter (A audio, M MIDI, E external audio; ADR 0003), name, and the mute and solo flags.
  char track_type(int track) const noexcept {
    const model::TrackSlot* t = slot(track);
    if (project_ == nullptr) {
      return o3os::app::track_type(track);
    }
    return t == nullptr ? '?' : (t->type == model::TrackType::Audio ? 'A' : (t->type == model::TrackType::Midi ? 'M' : 'E'));
  }
  const char* track_name(int track) const noexcept {
    const model::TrackSlot* t = slot(track);
    return project_ == nullptr ? o3os::app::track_name(track) : (t != nullptr ? t->name : "");
  }
  bool muted(int track) const noexcept {
    const model::TrackSlot* t = slot(track);
    return t != nullptr && (t->flags & model::kTrackMute) != 0;
  }
  bool soloed(int track) const noexcept {
    const model::TrackSlot* t = slot(track);
    return t != nullptr && (t->flags & model::kTrackSolo) != 0;
  }

  // A quantised MUTE toggle of the track waits for its boundary (OS-MUTE): the sequencer's own published bit, through the mixer board's sink. The Pattern view's MUTE tag blinks while it does (OS-MUTEVIS).
  bool mute_pending(int track) const noexcept {
    return mix_ != nullptr && mix_->tracks != nullptr && track >= 0 && track < 16 && ((mix_->tracks->mute_pending() >> track) & 1u) != 0u;
  }

  // The header's meter of a track, 0..kMeterMax (OS-PVU): the engine's, from the mixer board (the same ballistics the Mixer page draws), flat when the app has no board; the demo's fixed
  // levels for the demo alone. `meter_peak` is the tick, never below the level.
  unsigned meter_level(int track) const noexcept {
    if (project_ == nullptr) {
      return track_level(track);
    }
    return mix_ != nullptr && track >= 0 ? mix_->track_bar(static_cast<unsigned>(track)).level : 0u;
  }
  unsigned meter_peak(int track) const noexcept {
    if (project_ == nullptr) {
      return track_peak(track);
    }
    return mix_ != nullptr && track >= 0 ? mix_->track_bar(static_cast<unsigned>(track)).peak : 0u;
  }

  // The strip's PAT nn/nn: the pattern shown and how many exist (the demo's placeholders for the demo).
  int strip_pattern() const noexcept { return project_ == nullptr ? kDemoPattern : static_cast<int>(id_); }
  const char* strip_name() const noexcept { return pattern_ != nullptr ? pattern_->name : ""; }  // OS-NAMES: "" for the demo, an unnamed or a missing pattern
  int strip_patterns() const noexcept { return project_ == nullptr ? kDemoPatterns : static_cast<int>(project_->pattern_count()); }

 private:
  const model::TrackSlot* slot(int track) const noexcept {
    return project_ != nullptr && track >= 0 ? project_->track(static_cast<std::uint32_t>(track)) : nullptr;
  }

  const model::Project* project_ = nullptr;
  const model::Pattern* pattern_ = nullptr;
  const PatternData* demo_ = nullptr;
  const MixBoard* mix_ = nullptr;
  const model::LaneStore* lanes_ = nullptr;  // OS-MIDIREC-P1 S4: the grid's lane marks; nullptr for the demo
  unsigned id_ = 0;
};

}  // namespace o3os::app
