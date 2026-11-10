// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3os::model {

// Every capacity and value range of the project model (OS-004a), in one place. Raising a limit changes
// sizeof(ModelArena) (project.h states and static_asserts it) and, for the ones the native format stores,
// the maxima in docs/formats/o3-project.md; tools/o3project.py carries the same table (ctest compares
// the two). Counts are characters or items; text fields add one byte for the terminator.

// Tracks (ADR 0003): 16 slots exist in the model and the format, 12 are active in a new project.
inline constexpr std::uint32_t kMaxTracks = 16;
inline constexpr std::uint32_t kDefaultActiveTracks = 12;
inline constexpr std::uint32_t kMaxOutputBus = 16;  // 0 = master mix, 1..16 = stem buses (USB stems)

// Patterns. Each track of a pattern has its own length; steps past a track's length are kept (an OG pattern file
// holds such steps too).
inline constexpr std::uint32_t kMinPatternLength = 1;
inline constexpr std::uint32_t kMaxPatternLength = 128;
inline constexpr std::uint32_t kDefaultPatternLength = 32;
// Pattern ids are 1..255 (the OG numbering, the song refers to them); kMaxPatterns is how many can exist at
// once (the pool). 128 patterns of 14,384 bytes are 1.76 MiB: with 255 the pool alone would be 3.5 MiB and
// leave little of the 16 MiB SDRAM next to this OS's sample memory budget of 11.25 MiB. A file or import that
// needs more than kMaxPatterns distinct patterns is refused with "limit exceeded". Raising this to 255 costs
// 1.76 MiB more; nothing else depends on it.
inline constexpr std::uint32_t kMaxPatternId = 255;
inline constexpr std::uint32_t kMaxPatterns = 128;
inline constexpr std::uint32_t kMaxSongPositions = 255;

// Instruments: slot index 0..63, a step names one as index + 1 (0 = none). 64 = the OG's 48 samples + 16
// MIDI channels.
inline constexpr std::uint32_t kMaxInstruments = 64;
inline constexpr std::uint32_t kMaxModules = 16;
inline constexpr std::uint32_t kModuleParamBytes = 256;  // opaque blob per module instrument

// Sampler parameter arrays (the OG's instrument layout, docs/formats/og.md)
inline constexpr std::uint32_t kEnvelopes = 6;
inline constexpr std::uint32_t kLfos = 6;
inline constexpr std::uint32_t kSlices = 48;

// Text, in characters (without the terminator)
inline constexpr std::uint32_t kProjectNameMax = 32;
inline constexpr std::uint32_t kPatternNameMax = 30;  // the PAMD limit
inline constexpr std::uint32_t kTrackNameMax = 20;
inline constexpr std::uint32_t kInstrumentNameMax = 32;
inline constexpr std::uint32_t kSamplePathMax = 96;  // relative to the project folder
inline constexpr std::uint32_t kModuleNameMax = 31;

// Chunks this version does not know are kept verbatim, within these bounds (ADR 0007).
inline constexpr std::uint32_t kMaxUnknownChunks = 16;
inline constexpr std::uint32_t kUnknownPoolBytes = 16384;

// MIDI take lanes (OS-MIDIREC-P1, the MEVT chunk): a lane is the recorded events of one track of one pattern, in
// ticks of 1/48 of a step (6144 = the longest pattern). 24576 events of 8 bytes are the 192 KiB pool.
inline constexpr std::uint32_t kLaneTicks = 6144;           // 128 steps x 48 ticks; an event's tick is below it
inline constexpr std::uint32_t kLaneTicksPerStep = 48;
static_assert(kLaneTicks == kLaneTicksPerStep * kMaxPatternLength, "the lane timeline is the longest pattern");
inline constexpr std::uint32_t kMaxLaneEvents = 4096;      // in one lane
inline constexpr std::uint32_t kMaxLanePoolEvents = 24576;  // in all lanes of a project (192 KiB)
inline constexpr std::uint32_t kMaxLanes = 2048;            // lanes with events in one project
inline constexpr std::uint32_t kLaneEventKinds = 6;         // kind 0..5 (docs/specs/OS-MIDIREC-P1.md A3)

// Value ranges
inline constexpr std::uint32_t kMinTempoX100 = 4000;  // BPM x 100: 40..800, our range (an OG import clamps to it)
inline constexpr std::uint32_t kMaxTempoX100 = 80000;
inline constexpr std::uint32_t kDefaultTempoX100 = 13000;
inline constexpr std::uint32_t kMinSwing = 25;  // percent, 50 = straight
inline constexpr std::uint32_t kMaxSwing = 75;
inline constexpr std::uint32_t kDefaultSwing = 50;
inline constexpr std::uint32_t kMaxPercent = 100;   // volumes, pans, sends
inline constexpr std::uint32_t kDefaultVolume = 86;  // 86: the value every v7+ OG project file holds (PAR-006)
inline constexpr std::uint32_t kDefaultPan = 50;

}  // namespace o3os::model
