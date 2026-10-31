// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The track map of the Song screen (OS-SONG1): what each row of the order list shows of its pattern, one small cell per track: a note in the track (filled), only other data in it (an
// instrument, a volume or FX: dim), nothing (empty). Computed on the main loop from the model, at most once per change of the pattern: the cache is keyed by the pattern's generation
// (model::Project::pattern_generation, which moves whenever a step of it, its name or its lengths change), so drawing a frame costs a table read per row, never a scan of the pattern.
// Nothing here allocates or writes outside the cache, and none of it is saved.

#include <o3os/model/project.h>

#include <cstdint>

namespace o3os::app {

class SongTrackMap {
 public:
  static constexpr unsigned kTracks = model::kMaxTracks;  // the map covers every track of the model; the screen shows the active ones
  enum Cell : std::uint8_t { Empty = 0, Fx = 1, Note = 2 };

  // Two bits a track (track t at bits 2t..2t+1, a Cell), over the steps that play (up to the track's own length). Pure: a note step (any non-empty note byte: a note, fade, cut or off)
  // makes the track a Note, else any other non-empty step makes it an Fx.
  static std::uint32_t compute(const model::Pattern& pattern) noexcept;
  static Cell cell(std::uint32_t map, unsigned track) noexcept { return track < kTracks ? static_cast<Cell>((map >> (2u * track)) & 3u) : Empty; }

  // The map of pattern `id` (1..255) of `project`: 0 for one that does not exist. Cached by the pattern's generation, and by the project (another project's patterns are never mistaken for these).
  std::uint32_t get(const model::Project& project, unsigned id) const noexcept;

  // How many maps were computed (a scan of a pattern) since this object was made: what a test counts to prove the cache.
  std::uint32_t computed() const noexcept { return computed_; }

 private:
  struct Entry {
    std::uint32_t generation;  // 0: nothing cached (an existing pattern's generation is never 0)
    std::uint32_t map;
  };
  mutable Entry entry_[model::kMaxPatternId + 1] = {};
  mutable const model::Project* project_ = nullptr;
  mutable std::uint32_t computed_ = 0;
};

// The note density of each track of a pattern, for the energy row of the Song screen's TIMELINE page (OS-SONG-TIMELINE, docs/specs/OS-SONG-VIEW.md; the thumbnails it drew once are the note map since OS-SONG-VIEW2 V4): 4 bits a track (0 nothing .. 15 full), packed in
// 64 bits (track t at bits 4t..4t+3). Computed from the pattern on the main loop at most once per change of it, keyed by the pattern's generation like the track map: drawing a frame reads a table.
// Main loop only, nothing allocated, none of it saved.
class SongDensity {
 public:
  static constexpr unsigned kTracks = model::kMaxTracks;
  static constexpr unsigned kMax = 15;
  static_assert(kTracks * 4u <= 64u, "a nibble a track in 64 bits");

  // 0 for a track with no notes; else ceil(30 * notes / length) capped at 15: a note on every other step is already full. 0 for a length of 0.
  static constexpr unsigned level(unsigned notes, unsigned length) noexcept {
    if (length == 0u || notes == 0u) {
      return 0u;
    }
    const unsigned v = (30u * notes + length - 1u) / length;
    return v < kMax ? v : kMax;
  }
  // Pure: the notes (note bytes 1..128: not a fade, cut or off) of each track's first `track_length` steps, and (the second word, the same packing) the loudness of them: the mean of the steps' volume
  // (an unset one is 127, the loudest) scaled to 0..15, 0 for a track with no notes (OS-SONG-TIMELINE, the energy curve).
  static void scan(const model::Pattern& pattern, std::uint64_t& density, std::uint64_t& loudness) noexcept;
  static std::uint64_t compute(const model::Pattern& pattern) noexcept;  // the densities alone
  static constexpr unsigned loud(unsigned volume_sum, unsigned notes) noexcept {
    return notes == 0u ? 0u : (((volume_sum / notes) * kMax + 63u) / 127u);
  }
  static unsigned nibble(std::uint64_t packed, unsigned track) noexcept { return track < kTracks ? static_cast<unsigned>((packed >> (4u * track)) & 15u) : 0u; }

  // The densities of pattern `id` (1..255) of `project`: 0 for one that does not exist. Cached by the pattern's generation and by the project.
  std::uint64_t get(const model::Project& project, unsigned id) const noexcept;
  // The loudness of the same pattern, from the same cached scan.
  std::uint64_t loudness(const model::Project& project, unsigned id) const noexcept;

  // How many patterns were scanned since this object was made: what a test counts to prove the cache.
  std::uint32_t computed() const noexcept { return computed_; }

 private:
  mutable std::uint32_t generation_[model::kMaxPatternId + 1] = {};  // 0: nothing cached (an existing pattern's generation is never 0)
  mutable std::uint64_t dens_[model::kMaxPatternId + 1] = {};
  mutable std::uint64_t loud_[model::kMaxPatternId + 1] = {};
  mutable const model::Project* project_ = nullptr;
  mutable std::uint32_t computed_ = 0;
  bool refresh(const model::Project& project, unsigned id) const noexcept;  // the entry of `id` current (a scan if its generation moved); false for a pattern that does not exist
};

// How many slots of the order list hold each pattern (OS-SONGSHARE S1, docs/specs/OS-SONGSHARE.md): the Song screen marks a pattern that two or more slots hold and says how many on the info line. A table of 256 bytes (one a pattern id; at
// most 255 positions, so a count fits a byte), rebuilt in one pass over the order list when the project's song_generation moves, so drawing a frame reads a byte. The next-use key (S4) walks the list by itself and does not need the table.
// Main loop only, like the map's cache; none of it is saved.
class SongUseCount {
 public:
  // The slots that hold pattern `id` (1..255) in `project`'s order list: 0 for an id no slot names (and for 0 or a number past 255). A pattern a slot names counts whether or not it exists yet.
  unsigned get(const model::Project& project, unsigned id) const noexcept;

  // How many times the table was rebuilt since this object was made: what a test counts to prove the cache.
  std::uint32_t built() const noexcept { return built_; }

 private:
  mutable std::uint8_t count_[model::kMaxPatternId + 1] = {};
  mutable const model::Project* project_ = nullptr;  // the project it was built for, with song_generation_: another project (or a moved generation) rebuilds
  mutable std::uint32_t generation_ = 0;
  mutable bool valid_ = false;
  mutable std::uint32_t built_ = 0;
};

}  // namespace o3os::app
