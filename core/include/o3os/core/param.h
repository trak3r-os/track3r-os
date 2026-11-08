// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Parameters (ADR 0009 rev 1, mechanism 5; OS-002d): every automatable value has a ParamId and a descriptor, and the
// things that work on values (the generated edit page, MIDI Learn, settings files, later the mod matrix and automation) work
// on descriptors, so a new parameter needs a table row and no code of its own.
//
//   ParamId       32 bits: owner kind (8), index (8: an instrument or track slot, 0 for the others), field (16). Stable: a
//                 value is never renumbered, it is part of a project file's Learn bindings.
//   ParamChange   {ParamId, value, ramp}: what goes to the audio side over an SPSC ring (ParamRing), drained in the graph's pass (after the block's voices).
//                 `value` is in the descriptor's own units, which are the model's: a cutoff is 0..1 as the instrument stores it.
//                 `ramp_samples` 0 means "the engine's smoothing for this parameter" (its `kParamSmooth` flag); other values are
//                 for callers that know better (a preset load wants an instant change: 1).
//   ParamDesc     what a value is: range, default, display (unit and formatter), curve, steps, flags. Constexpr tables; the
//                 tables themselves live where the storage does (model/params.h). This header is the type and the pure maths:
//                 normalising through the curve (the bar of a field, the position of a knob), stepping by encoder detents,
//                 formatting a value as text and parsing it back.
//
// Display: plain integers for levels and tune,
// the pan as its distance from the centre, envelope times as seconds with three decimals, cutoff / resonance / sustain /
// amount as a percentage of their range.
//
// No heap, no libm (the log and exp curves use core/fast_math.h), no printf. Everything is a pure function of its arguments.

#include <o3os/core/ring.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace o3os::core {

// Whose value it is. Instrument and Track are indexed (a slot); Master, Project and Device are one object each.
// Project settings and device settings are the two scopes of ADR 0009 rev 1 ("settings are parameters with scope device or
// project"): the scope IS the kind.
enum class ParamKind : std::uint8_t {
  None = 0,
  Instrument = 1,  // the sampler block of an instrument slot (index 0..63)
  Track = 2,       // the mixer part of a track slot (index 0..15)
  Master = 3,      // the master section's levels and switches
  Project = 4,     // settings that belong to the project (sends mode, limiter mode, ...): in the project file
  Device = 5,      // settings that belong to the device (brightness, MIDI clock source, ...): in the settings file
  Insert = 6,      // OS-INSFX: a parameter of an effect module bound as an insert; index = track * 2 + slot (0..31), field = the module's parameter. ParamChange::ramp_samples carries the binding's generation (low 16 bits)
};
inline constexpr unsigned kParamKindCount = 7;

enum class ParamId : std::uint32_t { None = 0 };

constexpr ParamId param_id(ParamKind kind, unsigned index, unsigned field) noexcept {
  return static_cast<ParamId>((static_cast<std::uint32_t>(kind) << 24) | ((index & 0xFFu) << 16) | (field & 0xFFFFu));
}
constexpr ParamKind param_kind(ParamId id) noexcept {
  return static_cast<ParamKind>(static_cast<std::uint32_t>(id) >> 24);
}
constexpr unsigned param_index(ParamId id) noexcept { return (static_cast<std::uint32_t>(id) >> 16) & 0xFFu; }
constexpr unsigned param_field(ParamId id) noexcept { return static_cast<std::uint32_t>(id) & 0xFFFFu; }

// OS-INSFX S7: the ids of an insert's parameters (ParamKind::Insert). The index is the insert's `where` * 2 + slot: a track's INS A / INS B are 0..31 (`track * 2 + slot`), and the master's
// insert (project record where 16, slot 0) is 32, so the same split (`index / 2`, `index % 2`) gives the project's record key for both. The field is the module's own parameter number,
// below the ABI's O3M_PARAMS_MAX (32; `model::kInsertParamsMax`, pinned by tests/audio_model_ids_test.cpp). The parameter's descriptor is the bound module's (it is not a row of
// model/params.h's tables, which is why model::find_param has no Insert ids, as it has none for a Module instrument's kModuleFirst + i fields): describe_param in app/ makes it.
inline constexpr unsigned kInsertIdMaster = 32;  // the master insert's index (where 16 * 2 + slot 0)
inline constexpr unsigned kInsertIdFields = 32;  // fields 0..31
constexpr unsigned insert_param_index(unsigned where, unsigned slot) noexcept { return where * 2u + slot; }
// Whether `id` is an Insert id this version has: the kind, an index 0..32 and a field 0..31. (The master has no slot 1: its index 33 is not an id.)
constexpr bool insert_param_valid(ParamId id) noexcept {
  return param_kind(id) == ParamKind::Insert && param_index(id) <= kInsertIdMaster && param_field(id) < kInsertIdFields;
}

// ---- ParamChange: main loop -> audio ---------------------------------------------------------------------------------

struct ParamChange {
  ParamId id;
  float value;
  std::uint32_t ramp_samples;  // 0: the engine's own smoothing for this parameter
};
static_assert(sizeof(ParamChange) == 12 && alignof(ParamChange) == 4 && std::is_trivially_copyable<ParamChange>::value,
              "ParamChange is three words: it is copied bytewise through the ring");

inline constexpr std::uint32_t kParamRingSize = 256;     // ADR 0013 D6
inline constexpr std::uint32_t kMaxParamsPerBlock = 64;  // a bound on the drain, so a flooded ring cannot stretch a block
// One producer (the main loop: edits, MIDI Learn), one consumer (the render context, in the graph's pass after the block's voices).
using ParamRing = SpscRing<ParamChange, kParamRingSize>;

// ---- descriptors ----------------------------------------------------------------------------------------------------

// How a value is shown (and typed back). The unit text of the descriptor is the constant part ("%", "st", "s"); the formatter
// makes the number or the name. Only the displays a table row uses are here (a Hz, dB or note display is one more case in
// param_format / param_parse the day a row needs it).
enum class ParamUnit : std::uint8_t {
  Int,      // the value rounded to a whole number ("86", "-5")
  Pan,      // its distance from the middle of the range, whole ("0", "-12", "37"): a pan of 0..100 shows -50..50 (our display)
  NormPct,  // where it is in its range, 0..100: a cutoff of 0..1, a resonance of 0..4.3, a sustain of 0..1
  Seconds,  // the value is milliseconds, shown as seconds with three decimals ("0.010", "10.000")
  Enum,     // names[round(value - min)]
  Program,  // OS-INSTUI S8b: a MIDI program stored as program + 1: 0 shows "NONE", 1..128 show 0..127
  CcSlot,   // OS-INSTUI S8b: what a CC slot of a MIDI instrument sends: 0 "CONFIG", 1..128 "CC 0".."CC 127", 129 "PROG", 130 "CH AT", 131 "POLY AT"
  WtWave,   // OS-CEWTENV W7a: a wavetable contour 0..255 shown "F001".."F128", "U001".."U128"
  OffInt,   // OS-CEWTENV W7a: a whole number whose 0 shows "OFF" (a WT sustain: OFF, 1..99)
};

// How the position of a knob or the length of a bar maps to a value. Table is piecewise linear through `knots`; Log is
// geometric between min and max (min > 0: a frequency); Exp is the mirror (slow at the low end).
enum class ParamCurve : std::uint8_t { Linear, Log, Exp, Table };

struct ParamKnot {
  float n;  // normalised position 0..1, ascending, first 0, last 1
  float v;  // the value there, ascending, first min, last max
};

inline constexpr std::uint8_t kParamModulatable = 1u << 0;  // a mod matrix may route a source to it (ADR 0013 D6)
inline constexpr std::uint8_t kParamLearnable = 1u << 1;    // MIDI Learn may bind a CC to it
inline constexpr std::uint8_t kParamSmooth = 1u << 2;       // the engine ramps it (volume, pan, cutoff: PAR-126 and the 1.7.0 notes)
inline constexpr std::uint8_t kParamApply = 1u << 3;        // OS-USBAUDIO: press to apply. The encoder only changes a pending value (the page marks it); the setting is written, once, by param.apply
                                                            // (pressed twice). A value whose change is costly (re-enumerating USB). Never learnable

inline constexpr unsigned kParamNameChars = 12;  // the longest caption a generated page makes room for
inline constexpr unsigned kParamTextChars = 15;  // the longest text param_format makes (a NUL follows)

struct ParamDesc {
  const char* name;           // upper case, at most kParamNameChars: the caption of a row
  const char* unit_text;      // "" or "%", "st", "ct", "s", "bit": shown after the value
  const char* const* names;   // Enum: list_count names, names[round(value - min)]; otherwise null
  const ParamKnot* knots;     // Table: list_count knots; otherwise null
  float min;
  float max;
  float def;                  // the reset value, and what a missing setting reads as
  std::uint16_t detents;      // encoder detents across the whole range at the fine step (at least 1)
  std::uint8_t coarse;        // a coarse step (Shift) is this many fine ones
  std::uint8_t list_count;    // the length of `names` (Enum: max - min + 1) or of `knots` (Table, at least 2); 0 for the others
  ParamUnit unit;
  ParamCurve curve;
  std::uint8_t flags;         // kParam*
  std::uint8_t group;         // the page of a generated edit page this row belongs to (0-based)
};

// True when the descriptor describes something a value can be: min < max, def inside, detents at least 1, an Enum with one name for each
// of its values, a Table with ascending knots from (0, min) to (1, max), a Log with a positive min, a name that fits. The tables are checked
// with this by the tests (and a constexpr table by a static_assert where it can).
bool param_desc_valid(const ParamDesc& d) noexcept;

// A number that is a possible value of the descriptor: finite and inside [min, max].
bool param_in_range(const ParamDesc& d, float value) noexcept;
// Clamped into [min, max] (NaN gives def).
float param_clamp(const ParamDesc& d, float value) noexcept;

// Through the curve: 0 at min, 1 at max, clamped; NaN is 0. param_denorm is the inverse (n clamped to 0..1; the result is in
// range). For a Table the curve is exact at the knots; for Log and Exp the round trip is exact to float precision.
float param_norm(const ParamDesc& d, float value) noexcept;
float param_denorm(const ParamDesc& d, float norm) noexcept;

// The value `detents` encoder detents (fine steps, negative = down) away: a linear parameter moves by detents x (range / d.detents),
// added to the value as it is (so whole numbers stay whole and a value between two steps keeps its offset), any other curve by
// detents / d.detents of the way along it; clamped to the range. The caller multiplies by `coarse` for a coarse step.
float param_step(const ParamDesc& d, float value, int detents) noexcept;

// Writes the number or name of the value (no unit text) into `out` (cap includes the NUL; at least kParamTextChars + 1 holds any
// of them) and returns the length. A value outside the range is shown at the nearest end for NormPct and Enum and as it is for
// the rest.
unsigned param_format(const ParamDesc& d, float value, char* out, unsigned cap) noexcept;
// The inverse: the text param_format makes (case-insensitive for names), optionally followed by the unit text, spaces allowed
// at both ends. False (out untouched) for anything else or for a value outside the range.
bool param_parse(const ParamDesc& d, const char* text, float& out) noexcept;

}  // namespace o3os::core
