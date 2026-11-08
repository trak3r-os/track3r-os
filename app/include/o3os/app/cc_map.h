// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-MIDICCMAP (docs/specs/OS-MIDICCMAP.md, section 3.1): the official firmware's fixed MIDI CC map as a static table — no RAM, no Learn slots. The numbers, the
// targets and the value laws are verified parity from Polyend's published source (spec F4 to F6); how the table sits beside MIDI Learn and its CC MAP setting is new
// Community OS behaviour. Pure: no include but <cstdint>, constexpr only, host-tested (tests/cc_map_test.cpp).
//
//   CC 71..78   the volume of track 1..8            CC 79 / 80 / 81   the master DELAY / REVERB / DRY level   CC 82   the INPUT strip's level
//   CC 51..62   the value of Perform slot 1..12      (S3)             CC 41..48   the performance pattern of track 1..8  (S4)
//
// The mixer CCs act on every screen, the Perform ones only on the Perform screen's slot page (Q5 a; applied in cc_map.cpp, not here).

#include <cstdint>

namespace o3os::app {

enum class CcMapKind : std::uint8_t {
  None = 0,
  Track,         // 71..78, index 0..7: track 1..8's VOLUME
  MasterDelay,   // 79
  MasterReverb,  // 80
  MasterDry,     // 81
  Input,         // 82: the INPUT strip's level (MixBoard::in_level)
  PerfSlot,      // 51..62, index 0..11 (S3)
  PerfTrack,     // 41..48, index 0..7 (S4)
};

struct CcTarget {
  CcMapKind kind;
  std::uint8_t index;  // Track / PerfTrack / PerfSlot: 0-based; the master's targets and Input: 0
};

// The one lookup of the table: 128 bytes of flash as a switch, no RAM.
constexpr CcTarget cc_map_target(unsigned cc) noexcept {
  if (cc >= 71u && cc <= 78u) {
    return {CcMapKind::Track, static_cast<std::uint8_t>(cc - 71u)};
  }
  if (cc == 79u) {
    return {CcMapKind::MasterDelay, 0u};
  }
  if (cc == 80u) {
    return {CcMapKind::MasterReverb, 0u};
  }
  if (cc == 81u) {
    return {CcMapKind::MasterDry, 0u};
  }
  if (cc == 82u) {
    return {CcMapKind::Input, 0u};
  }
  if (cc >= 51u && cc <= 62u) {
    return {CcMapKind::PerfSlot, static_cast<std::uint8_t>(cc - 51u)};
  }
  if (cc >= 41u && cc <= 48u) {
    return {CcMapKind::PerfTrack, static_cast<std::uint8_t>(cc - 41u)};
  }
  return {CcMapKind::None, 0u};
}

// The value laws, integer, as the official firmware (F4, F5): a level 0..100 of the CC 0..127, a Perform value inside the FX's range, a pattern number.
constexpr unsigned cc_map_level(unsigned v) noexcept { return v * 100u / 127u; }
constexpr int cc_map_perf(unsigned v, int min, int max) noexcept { return min + static_cast<int>(v) * (max - min) / 127; }
constexpr unsigned cc_map_pattern(unsigned v) noexcept { return v + 1u; }

}  // namespace o3os::app
