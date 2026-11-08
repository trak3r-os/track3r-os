// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace o3os::model {

// One pattern step (ADR 0004): note, instrument, volume and two FX columns of (type, value). Seven bytes,
// alignment 1, no padding: the in-memory layout IS the on-disk layout of formats/native (docs/formats/
// o3-project.md), so arrays of steps are copied as bytes. Steps are untrusted data (PAR-003): every value
// is carried verbatim through load, edit and save, and sanitised only where it is used.
//
// note   0 empty | 1..128 = OG/MIDI note 0..127 plus one (1 = C-0) | 253 fade | 254 cut | 255 off
//        (129..252 are reserved and carried verbatim). OG import maps -1 -> 0, n -> n + 1, -2 -> 253,
//        -3 -> 254, -4 -> 255 (PAR-007: i8, -1 empty, -2 fade, -3 cut, -4 off).
// instr  0 none | 1..64 = instrument slot + 1
// vol    0..127, 255 not set
// fx     type 0 None, 1..42 OG types, 43..95 core FX (43..62 assigned by ADR 0004 amendments 1 and 2,
//        63..95 free), 96..127 tracker-ce FX (the OG id + kFxCeOffset, ADR 0004 amendment 3), 128..255
//        declared by the instrument's engine module (ADR 0004); value is a byte
inline constexpr std::uint8_t kNoteEmpty = 0;
inline constexpr std::uint8_t kNoteFirst = 1;
inline constexpr std::uint8_t kNoteLast = 128;
inline constexpr std::uint8_t kNoteFade = 253;
inline constexpr std::uint8_t kNoteCut = 254;
inline constexpr std::uint8_t kNoteOff = 255;

inline constexpr std::uint8_t kVolumeMax = 127;
inline constexpr std::uint8_t kVolumeUnset = 255;

inline constexpr std::uint8_t kFxNone = 0;
inline constexpr std::uint8_t kFxOgLast = 42;
inline constexpr std::uint8_t kFxOgCeLast = 74;   // the last OG id that maps to a tracker-ce id (74 + kFxCeOffset = 127)
inline constexpr std::uint8_t kFxCeFirst = 96;   // the first tracker-ce FX id in the model and a .o3pj (ADR 0004 amendment 3)
inline constexpr std::uint8_t kFxCeOffset = 53;  // a tracker-ce model id is the OG id + kFxCeOffset, for OG ids 43..74
inline constexpr std::uint8_t kFxReservedLast = 127;
inline constexpr std::uint8_t kFxEngineFirst = 128;

enum class FxClass : std::uint8_t { Og, Reserved, Engine };

constexpr FxClass fx_class(std::uint8_t type) noexcept {
  return type <= kFxOgLast ? FxClass::Og : (type <= kFxReservedLast ? FxClass::Reserved : FxClass::Engine);
}

struct alignas(1) StepFx {
  std::uint8_t type;
  std::uint8_t value;
};

struct alignas(1) Step {
  std::uint8_t note;
  std::uint8_t instr;
  std::uint8_t vol;
  StepFx fx[2];
};

static_assert(sizeof(StepFx) == 2 && alignof(StepFx) == 1, "StepFx is two packed bytes");
static_assert(sizeof(Step) == 7 && alignof(Step) == 1, "Step is seven packed bytes (ADR 0004, ADR 0011)");
static_assert(offsetof(Step, note) == 0 && offsetof(Step, instr) == 1 && offsetof(Step, vol) == 2 &&
                  offsetof(Step, fx) == 3,
              "Step field order is the on-disk order");

inline constexpr Step kEmptyStep = {kNoteEmpty, 0, kVolumeUnset, {{kFxNone, 0}, {kFxNone, 0}}};

constexpr bool is_empty(const Step& s) noexcept {
  return s.note == kEmptyStep.note && s.instr == kEmptyStep.instr && s.vol == kEmptyStep.vol &&
         s.fx[0].type == kFxNone && s.fx[0].value == 0 && s.fx[1].type == kFxNone && s.fx[1].value == 0;
}

}  // namespace o3os::model
