// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Private to app/src: the pieces of song_edit.cpp that song_build.cpp (FORM and BRIDGE, OS-SONG-BUILD) reuses. Not a public interface.

#include <o3os/app/song_edit.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// The first pattern number (1..255) that no pattern has, no slot names (`named`) and that is not in `taken`; 0 when there is none (SONG-UX D15).
unsigned free_number(const model::Project& p, const SongIdSet& named, const SongIdSet& taken) noexcept;

// The journal bytes song_copy_pattern_masked writes for `from` (the same records in the same conditions: keep the two together), the unit's header and trailer not included.
// The lanes' insert records (OS-MIDIREC-P1 S7a) are counted when `lanes` is given.
std::size_t copy_pattern_body_masked(const model::Project& p, unsigned from, std::uint16_t cleared_tracks, const model::LaneStore* lanes = nullptr) noexcept;

// The lane pool a copy of `from` takes (OS-MIDIREC-P1 S7a): `events` and `lane_count` of the tracks in `mask` are ADDED to the caller's totals (a plan sums its copies); nothing without a store.
void lane_copy_counts(const model::Project& p, const model::LaneStore* lanes, unsigned from, std::uint16_t mask, std::uint32_t& events, std::uint32_t& lane_count) noexcept;

// True when `events` more events in `lane_count` more lanes fit the lane pool and the lane directory (always true for no lanes or none to add).
bool lane_pool_fits(const model::LaneStore* lanes, std::uint32_t events, std::uint32_t lane_count) noexcept;

}  // namespace o3os::app
