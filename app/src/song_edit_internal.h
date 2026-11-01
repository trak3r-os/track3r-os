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
std::size_t copy_pattern_body_masked(const model::Project& p, unsigned from, std::uint16_t cleared_tracks) noexcept;

}  // namespace o3os::app
