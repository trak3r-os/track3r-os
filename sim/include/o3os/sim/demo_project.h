// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A demo project for the parameter screens, the scenarios and the gallery (OS-002d): ten sample instruments named like the demo
// pattern's (KICK, SNARE, HAT, ...) with sampler values that make the pages worth looking at (filters of the three types at different
// cutoffs, envelopes, sends). Deterministic. Nothing here is a format or a default: it is data for pictures.

#include <o3os/model/project.h>

#include <cstdint>

namespace o3os::sim {

inline constexpr unsigned kDemoInstruments = 10;

// project.reset(), then pattern 1 = the Pattern screen's demo (OS-009), then instruments 0..kDemoInstruments-1 (instrument numbers 1..10).
void fill_demo_project(model::Project& project) noexcept;

// Pattern 1 of the demo alone: 12 tracks of 64 steps as app::fill_demo has them, the track slots named and typed as the demo's headers, and
// one song position. Called on a reset project; fill_demo_project does.
void fill_demo_pattern(model::Project& project) noexcept;

// OS-E2E-2 (scenario `document sound`): the document of the audio journeys. project.reset(), an empty pattern 1 of 16 steps in a one-position song at 120 BPM, and four sample instruments with every
// parameter neutral (volume 50 = 0 dB under the 1.2+ law, pan centre, no tune, no glide, no filter, no amp envelope, no sends, no overdrive, 16 bit): SINE and NOISE and the sine's twin loop, PLUCK plays once.
// They are the test bank's slots 0..3 (sim_audio.h), so that what the engine plays is the sample as it is until a journey turns a parameter.
inline constexpr unsigned kSoundInstruments = 4;
void fill_sound_project(model::Project& project) noexcept;

// OS-E2E-9 (scenario `document tb303`): the document of the TB-303 journey. project.reset(), a pattern 1 of 16 steps in a one-position song at 150 BPM (a step is 100 ms) with the acid line of
// kTb303Line, and instrument 1 a MODULE instrument that names the module "tb303" with the parameters of kTb303Params (the module's own order, modules/tb303/module.json). The notes are the
// sequencer's own (note on at the step, a note off where the line says so); the velocity of a step is its volume (96 plays without the accent, 127 with it).
struct Tb303Step {
  std::uint8_t row;       // the step, 0..15
  std::uint8_t midi;      // the note (the step holds midi + 1); 0: a note off
  std::uint8_t velocity;  // 0..127 (the step's volume byte); a note off has none
};
inline constexpr Tb303Step kTb303Line[] = {{0, 36, 96}, {2, 36, 96}, {3, 48, 127}, {4, 0, 0}, {6, 39, 96}, {8, 43, 127}, {10, 36, 96}, {11, 0, 0}, {12, 44, 96}, {14, 0, 0}};
inline constexpr float kTb303Params[9] = {0.0f, 600.0f, 70.0f, 60.0f, 400.0f, 80.0f, 60.0f, -18.0f, 1.0f};  // waveform, cutoff Hz, resonance %, env mod %, decay ms, accent %, slide ms, volume dB, legato slide
void fill_tb303_project(model::Project& project) noexcept;

// OS-INSFX S4 (`document inserts`): the demo project with three insert records for the Mixer's INSERTS page: T1 INS A "biquad", T1 INS B "reverb" (bypassed; the card of `apps on` has no such file, so the picker greys it), T3 INS A "biquad".
void fill_insert_project(model::Project& project) noexcept;

// The same, then the project's Learn store: CC 74 bound to the cutoff of instrument 1 (pickup) and CC 71 to its resonance (jump).
void fill_demo_project_with_bindings(model::Project& project) noexcept;

}  // namespace o3os::sim
