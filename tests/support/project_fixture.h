// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Deterministic projects for the tests, the fuzz loop and the Python round trip (tools/o3pj_tool.cpp), and
// convenience wrappers that run the resumable reader and writer to the end over memory.

#include <o3os/formats/native.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace o3os::test {

// Three arenas of 1.8 MiB each, static (they are far too big for a stack). Index 0..2.
model::ModelArena& arena(int index);

// A few of everything: all chunk kinds, odd and even sized texts, an odd-sized and an empty unknown chunk.
void fill_small(model::Project& p);
// fill_small plus the project-level state of OS-004d: instruments that chose their playback engine (INST 1.2) and one of every part (MIDI, USBS, MIXR, PERF, APPM).
void fill_state(model::Project& p);
// fill_small plus instruments of INST 1.3 (OS-INSTUI S8b + S9): slot 20 a MIDI instrument with all twelve fields away from their defaults, slot 21 one with the offset only, slot 0 a sample instrument on the
// Amiga engine with VEL> 7 (the engine byte and the twelve in one chunk).
void fill_midi13(model::Project& p);
// fill_small plus the session state of OS-PERSIST2: one of every part, none of the default values (NOTE, MUTE, SCEN with two scenes stored and LATCH off, SNFX).
void fill_persist2(model::Project& p);
// fill_persist2 with the NOTE scale outside the panel's twelve (Hirajoshi, the official 26, SCALE LOCK on), so a NOTE 1.1 chunk (OS-SCALES28).
void fill_scales(model::Project& p);
// fill_persist2 plus three inserts (OS-INSFX S3a: INSX 1.0): track 0 INS A (no parameters), track 3 INS B (32 parameters, bypassed), the master (three parameters).
void fill_inserts(model::Project& p);
// Every limit reached: 128 patterns (ids 1..127 and 255) of 16 x 128 non-empty steps, a song of 255
// positions, 64 instruments of all three kinds with maximal texts and blobs, 16 modules, the unknown-chunk
// pool exactly full, maximal names.
void fill_maximal(model::Project& p);

// Runs a Writer to Done over a vector sink. `budget` is the per-step byte budget.
formats::native::Progress save(const model::Project& p, std::vector<std::uint8_t>& out,
                               std::size_t budget = formats::native::kDefaultStepBudget,
                               const model::LaneStore* lanes = nullptr);
// Runs a Reader to Done or Failed over memory.
formats::native::Progress load(model::ModelArena& a, const std::vector<std::uint8_t>& bytes,
                               formats::native::Report* report = nullptr,
                               std::size_t budget = formats::native::kDefaultStepBudget,
                               model::LaneStore* lanes = nullptr);

// The project is the empty default: no patterns, instruments, modules, song or unknown chunks.
bool is_default_empty(const model::Project& p);

}  // namespace o3os::test
