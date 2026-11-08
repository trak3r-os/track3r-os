// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-FXREC S1 (docs/specs/OS-FXREC.md, section 3.1 and the column rule of 3.3; NEW Community OS behaviour, no parity claim: the official
// firmware's CC handlers write no step FX): the map from an instrument parameter's new value to the step FX that plays it, and the column
// of a step a recorded FX takes. Pure functions, no heap, no globals, no app state, so a host test walks them (tests/rec_fx_test.cpp). The
// wiring into apply_cc, the take rule and the strip are S2.
//
// THE MAP. rec_fx_map(field, filter_type, value, out) gives the step FX of an instrument parameter's new value, or false (nothing records):
//   CUTOFF (7)   27 Low-pass / 28 High-pass / 29 Band-pass by the instrument's FILTER TYPE 0 / 1 / 2, value round(cutoff * 100), 0..100
//   PANNING (1)  31, the pan, 0..100
//   DELAY SEND (9)   30, 0..100
//   REVERB SEND (10) 32, 0..100
//   OVERDRIVE (11)   38, 0..100
//   BIT DEPTH (12)   39, 4..16 (the FX takes 1..16; the parameter is 4..16)
// The value laws are the parameters' (the Cutoff FX value / 100 is the cutoff; the Panning FX replaces the track's pan while held; FX 30 / 32
// held replace the instrument's sends; Overdrive and Bit depth as the instrument's fields). Tune 40 is not in the map (Q2 a); every other
// parameter writes nothing.
//
// THE COLUMN. rec_fx_place(s, type, value) writes the FX into the first column that holds an FX of the same group (for the filter any of
// 27/28/29: the type is replaced by the current filter type), else into the first column whose type is 0, else nothing (-1: both hold another
// FX, a Micro-move included). It returns the column it wrote, or -1. Later messages on the same step find the same column and overwrite the
// value, so the step keeps the last value of the pass: one value per step. The step's note, instrument and volume are never touched, and a
// column that holds another FX is never written over.

#include <o3os/model/params.h>
#include <o3os/model/step.h>
#include <o3os/seq/fx.h>

#include <cstdint>

namespace o3os::app::rec {

// The FX value of a parameter's value: rounded to the nearest integer and held to [lo, hi]. A value the parameter cannot hold (and NaN)
// is clamped, never carried.
constexpr std::uint8_t rec_fx_value(float value, float lo, float hi) noexcept {
  if (!(value >= lo)) {  // below the range, or NaN
    return static_cast<std::uint8_t>(lo);
  }
  if (value > hi) {
    return static_cast<std::uint8_t>(hi);
  }
  return static_cast<std::uint8_t>(value + 0.5f);  // round half up; value is inside [lo, hi]
}

// The map of section 3.1: `field` is the instrument parameter's field id (model::instrument_param), `filter_type` the instrument's FILTER TYPE
// at the moment of the message (0 / 1 / 2), `value` the parameter's new value as read back from the parameter. On true, `out` is the step FX.
constexpr bool rec_fx_map(unsigned field, unsigned filter_type, float value, model::StepFx& out) noexcept {
  using namespace model::instrument_param;
  switch (field) {
    case Cutoff:
      if (filter_type > 2u) {
        return false;
      }
      out.type = static_cast<std::uint8_t>(seq::fx::kLowPass + filter_type);  // 27 / 28 / 29
      out.value = rec_fx_value(value * 100.0f, 0.0f, 100.0f);
      return true;
    case Panning:
      out.type = seq::fx::kPanning;
      out.value = rec_fx_value(value, 0.0f, 100.0f);
      return true;
    case DelaySend:
      out.type = seq::fx::kDelaySend;
      out.value = rec_fx_value(value, 0.0f, 100.0f);
      return true;
    case ReverbSend:
      out.type = seq::fx::kReverbSend;
      out.value = rec_fx_value(value, 0.0f, 100.0f);
      return true;
    case Overdrive:
      out.type = seq::fx::kOverdrive;
      out.value = rec_fx_value(value, 0.0f, 100.0f);
      return true;
    case BitDepth:
      out.type = seq::fx::kBitDepth;
      out.value = rec_fx_value(value, 4.0f, 16.0f);
      return true;
    default:
      return false;  // Tune 40 is not in the map (Q2 a); every other parameter records nothing
  }
}

// The column rule of section 3.3. Writes (type, value) into the chosen column of `s` and returns the column (0 or 1), or returns -1 and
// leaves `s` untouched when both columns hold another FX.
constexpr int rec_fx_place(model::Step& s, std::uint8_t type, std::uint8_t value) noexcept {
  const seq::FxGroup group = seq::fx_group(type, 0u);  // every mapped type is a table type: its group does not depend on the column
  for (unsigned c = 0; c < 2u; ++c) {
    const std::uint8_t held = s.fx[c].type;
    if (held != model::kFxNone && seq::fx_group(held, c) == group) {  // the column of the group wins: the value is overwritten, the type follows
      s.fx[c].type = type;
      s.fx[c].value = value;
      return static_cast<int>(c);
    }
  }
  for (unsigned c = 0; c < 2u; ++c) {
    if (s.fx[c].type == model::kFxNone) {
      s.fx[c].type = type;
      s.fx[c].value = value;
      return static_cast<int>(c);
    }
  }
  return -1;
}

}  // namespace o3os::app::rec
