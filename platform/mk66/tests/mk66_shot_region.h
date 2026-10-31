// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// The RAM_G region the screenshot and the console's `shot` take their bands into, as the shot tests see it: ui/plot/ram_g_map.h kRamGShot and kRamGShotBytes (the spare region behind the note map, less
// the DMA self-test's 64 bytes). A platform test may not include ui (the layer check), so the values are written here once for both shot tests, and tests/plot_ram_g_test.cpp checks them against the map:
// when the map moves, that test fails and this header is the one place to change.
namespace o3os::platform::mk66::test {

inline constexpr std::uint32_t kShotRegion = 0x89000u;
inline constexpr std::uint32_t kShotRegionBytes = 487360u;

}  // namespace o3os::platform::mk66::test
