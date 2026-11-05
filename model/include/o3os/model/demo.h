// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The demo content a device starts with until the SD card loads a project (OS-005d lite): four sample instruments (KICK, SNARE, HAT, BASS in slots
// 0..3, the sounds of audio/demo_kit.h) and a two-bar pattern for them on tracks 1..4 at 120 BPM. Plain model writes (the Project's own mutators), so
// the model's invariants hold; no undo unit is made (the platform calls these before the document is attached).
//
// The pattern is 32 steps (a step is a sixteenth: 4 to a beat, 16 to a bar at 4/4, so two bars), every note at its natural velocity unless the step says:
//   track 1  KICK   C-5 on every beat, and a ghost kick (volume 80) on steps 10 and 26
//   track 2  SNARE  C-5 on beats 2 and 4 of each bar, and a ghost (volume 60) on step 30
//   track 3  HAT    C-5 on every second step, volume 100 on the off-beats and 60 on the others
//   track 4  BASS   a line in C minor (notes 48, 51, 53, 46 are C2, Eb2, F2, Bb1: the bass cycle is a C at note 60), each note ended by an Off two
//                   steps on (a note rings until the next one; the Off is what makes the loop stop)

#include <o3os/model/project.h>

#include <cstdint>

namespace o3os::model::demo {

inline constexpr unsigned kInstruments = 4;
inline constexpr std::uint32_t kTempoX100 = 12000;
inline constexpr std::uint32_t kPatternSteps = 32;

// KICK, SNARE, HAT and BASS into slots 0..3 (sample instruments, no sample path: the PCM is in the sound engine's table). `volume` is their level: the demo kit is bound at unity (SampleDesc's default
// gain), so it has to be the level that is 0 dB under the law the document is read through, or the first rebuild of a descriptor (any Params edit) moves it: 50 under the 1.2+ law (the default), 100 under the
// 1.1.14 table (OS-BUG-PASTE24: it was 100 for both, +24 dB at the first rebuild).
Status add_instruments(Project& project, std::uint8_t volume = 50) noexcept;

// True when pattern `id` has no step with anything in it (a pattern that does not exist counts as empty).
bool pattern_is_empty(const Project& project, std::uint32_t id) noexcept;

// Creates pattern `id` if it does not exist (32 steps on every track) and fills tracks 1..4 as described above. The
// pattern must be empty (Exists: it is not, nothing is written). If `set_tempo` is true (default), also sets the tempo to 120 BPM.
// OS-BUG-TEMPOLOAD: set_tempo=false for the load path to preserve the project's saved tempo.
Status fill_pattern(Project& project, std::uint32_t id, bool set_tempo = true) noexcept;

}  // namespace o3os::model::demo
