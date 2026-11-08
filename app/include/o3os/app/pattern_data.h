// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/model/limits.h>
#include <o3os/model/step.h>

#include <cstdint>

namespace o3os::app {

// Static demo data for the pattern view (OS-001b): what the Pattern screen shows while no document is attached (OS-009 D1: read-only,
// no edit runs on it). Nothing is stored, loaded or saved, and nothing here is a persistent format. The steps are the model's
// (ADR 0004, model/step.h: note byte 1..128 = C-0.., 255 off, FX type 0 = none, volume 255 = not set); the track count follows ADR 0003.
inline constexpr int kTracks = 12;
inline constexpr int kSteps = 64;
inline constexpr int kInstruments = 48;  // the sample instruments (slots 1-48 of an OG project): what a pad can reach; a step may name up to model::kMaxInstruments
inline constexpr int kColumns = 5;

using Step = model::Step;

enum class Column : std::uint8_t { Note = 0, Instr, Vol, Fx1, Fx2 };

// 12 tracks x 64 steps x 7 bytes = 5376 bytes.
struct PatternData {
  Step steps[kTracks][kSteps];
};

// Display of one field, PROVISIONAL until the UI style guide:
//   Note  3 chars  "C-2" "D#3" (octave 0..9), "C10".."G10" for the notes 121..128 (the sharps lowercase: "c10"), "---" empty, "OFF" note off,
//                  "CUT", "FAD", "???" out of range (ui/assets.h says the tile names)
//   Instr 2 chars  "01".."64", "--" none (also on an off, cut or fade step), "??" out of range
//   Vol   2 chars  two hex digits (0..7F), "--" not set, "??" out of range
//   Fx    3 chars  type character + two hex digits, "---" not set (type 0). The type character is the base-36 id for
//                  OG types 1..35, "!#$%&*+" for 36..42, a lower-case letter for engine-declared types 128..
//                  (ADR 0004) and '?' for anything else
inline constexpr int kFieldChars[kColumns] = {3, 2, 2, 3, 3};
// Character column of each field inside a track cell "C-2 01 7F R40 a01" (one space between fields).
inline constexpr int kFieldCol[kColumns] = {0, 4, 7, 10, 14};
inline constexpr int kCellChars = 17;

// The note tile cell (ui/assets.h) of a note byte: 0 for none, the note itself up to B-9, 125.. for C10..G10, Off, Cut, Fade, and "???" for
// every other byte.
unsigned note_tile_cell(std::uint8_t note) noexcept;
// The same under the device setting MIDDLE C (OS-CFGWIRE, new Community OS behaviour; model::params.cpp kMiddleC): the names of the notes 1..128 are shifted by `octave_shift` octaves
// (the setting's index - 2: C-5, the name of MIDI note 60 under the default, is 0). A name that falls outside the tiles (below C-0, or past G-10) is the "???" tile. Other bytes are as above.
unsigned note_tile_cell(std::uint8_t note, int octave_shift) noexcept;

// The three characters of the note tile for an OG note 0..127 ("C-5", "C#3", "G10"), under the same MIDDLE C shift: what the grid, the info line and the Fill dialog's FROM and TO say. A note past 127 is "???".
void note_name(unsigned note, int octave_shift, char (&out)[4]) noexcept;

// The device setting NUMBERING (PAR-120; model::params.cpp kNumbering, in its order): how the Pattern view numbers its rows. This OS's default counts them from 1 (provisional: docs/formats/o3-settings.md).
enum class Numbering : std::uint8_t { From0 = 0, From1 = 1, Hex = 2 };
// The value of the row-number tile (two digits, ui::Renderer::hex_tile) of `row` (0-based). From0 and From1 are packed decimal (a number past 99 keeps its tens digit in the hex digit: A0 for 100);
// Hex is the row itself, from 0. An unknown `numbering` is From1.
unsigned row_number_tile(int row, unsigned numbering) noexcept;

// Writes the field text into `out` (NUL-terminated); returns true if the field is empty.
bool format_field(const Step& step, Column column, char (&out)[4]) noexcept;

// Fills `data` with the demo pattern (deterministic; the whole 12 x 64 grid, some tracks note-less).
void fill_demo(PatternData& data) noexcept;

// The character of an FX type: the base-36 id for OG types 0..35, "!#$%&*+" for 36..42, the core FX 46..62, 'w' 'y' 'z' for 96..98 (OS-CEWTENV W6b), a lower-case letter for
// engine-declared types 128.. and '?' for anything else. What the FX column shows in front of the value.
char fx_type_char(std::uint8_t type) noexcept;

// Track metadata for the header: name and type letter (A audio, M MIDI, E external audio; ADR 0003).
const char* track_name(int track) noexcept;
char track_type(int track) noexcept;
// Fake demo levels for the track-header meters (OS-003b): a fixed level and peak, 0..kMeterMax, per track. Only the demo (no document attached: the simulator's
// sweeps and the OS-001b tests) shows these; a document's headers read the engine's meters through PatternSource::meter_level / meter_peak (OS-PVU).
inline constexpr unsigned kMeterMax = 255;
unsigned track_level(int track) noexcept;
unsigned track_peak(int track) noexcept;
// Instrument 1..48; "" when the slot has no demo name.
const char* instrument_name(int instrument) noexcept;

}  // namespace o3os::app
