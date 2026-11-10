// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-PARAMAUTO S1 (docs/specs/OS-PARAMAUTO.md; NEW COMMUNITY OS BEHAVIOUR): the letter map of the "CC A..F moves a parameter" feature. On a Sample or Module instrument the six bytes
// Instrument::midi_cc[0..5] (always 0 before this item; a MIDI instrument's are its controller map and are not touched) hold the PARAMETER CODE of the field of that instrument that owns the letter
// (A..F = 0..5), 0 = no letter. INST 1.9 is the version that says so (formats/native.h kInstVersionAutoCc): the same layout as 1.8, no new byte. A chunk older than 1.9 has no letters (the reader zeroes
// the bytes of a non-MIDI instrument), so an old project behaves as it did.
//
//   Sample  code = field + 1 of a learnable instrument field 0x00..0x77 (1..120)
//   Module  code = module parameter index + 1 (field kModuleFirst + i): 1..32
//
// Letters are project wide: one instrument at most owns a letter. A hand-edited file that has a letter twice is not rewritten: the lowest instrument slot owns it, the later byte is ignored.
// Pure functions of the project; no heap.

#include <o3os/core/param.h>
#include <o3os/model/instrument.h>
#include <o3os/model/project.h>

#include <cstdint>

namespace o3os::model {

inline constexpr unsigned kAutoLetters = kMidiCcSlotsPerInstrument;  // CC A..F
inline constexpr unsigned kAutoSampleFields = 0x78;                  // the Sample fields a code can name: 0x00..0x77

// The code of an Instrument-kind parameter id, 0 when it cannot be coded (a Sample field that is not a learnable table row: VelSens, the wavetable rows, the virtual fields; a module field past
// kModuleMax; any other kind). The kind of the instrument is not known here: a Sample field and a Module field never overlap (0x00..0x77, 0x100..0x11F).
std::uint8_t auto_code(core::ParamId id) noexcept;

// The parameter a code names on instrument `slot` of kind `kind`; ParamId::None for a MIDI instrument, code 0 or a code out of the kind's range.
core::ParamId auto_param(std::uint32_t slot, InstrumentKind kind, std::uint8_t code) noexcept;

// Whether an instrument holds a letter (a non-MIDI instrument with a non-zero byte): the writer's gate for INST 1.9.
bool inst_has_auto_letter(const Instrument& in) noexcept;

// What each letter is mapped to; ParamId::None for a free letter.
struct AutoMap {
  core::ParamId id[kAutoLetters];
};

// Fills `out` from the project: letter L is owned by the lowest non-MIDI slot whose midi_cc[L] decodes (auto_param); a byte that does not decode owns nothing.
void auto_scan(const Project& p, AutoMap& out) noexcept;
// The letter (0..5) that owns `id`, or -1.
int auto_find(const AutoMap& m, core::ParamId id) noexcept;
// The first free letter (0..5), or -1 when all six are owned.
int auto_first_free(const AutoMap& m) noexcept;

}  // namespace o3os::model
