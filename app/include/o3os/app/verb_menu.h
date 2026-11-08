// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The F7 verb menu of the Pattern screen (OS-PTOOLS2 S3, docs/specs/OS-PTOOLS2.md section 3.2 and mock-up A): two columns of eight rows, pads 1-16 pick a verb
// directly (pad n = entry n-1: the left column is pads 1-8, the right one pads 9-16, row by row), BACK closes. One constexpr table that the overlay
// (screens/verb_menu_overlay.cpp), the actions that run a verb (verb_actions.cpp) and the tests ask, so a new verb is one row here.
//
//   built    the verb exists on this head and its row runs what the verb runs today: FILL opens the F4 dialog (fill.open), TRANSPOSE and INSTRUMENT run the block's
//            encoder verbs (step.note_delta / step.instrument_delta, one detent), INVERT runs pattern.invert (pad 9), the length ops run pattern.op (as the TOOLS list's
//            DUPLICATE / EXPAND / SHRINK rows), CLEAR runs step.clear (the Delete key's block clear), the exports run export.pattern / export.selection (as the TOOLS list's
//            rows), APPS > opens the TOOLS list (tools.open, until the apps render in the panel, slice S6) and the five S4 verbs run model::verbs::apply_block over the block
//            (OS-PTOOLS2 S3b: INTERPOLATE, ROTATE UP / DOWN parameterless; RANDOMISE and HUMANISE with the Plan's fixed default amounts — no dialog in this slice).
//   dim      the verb is not built yet on this head: the row is drawn dim and picking it runs nothing (none on this head).
//
// Main loop only. No allocation.

#include <cstdint>

namespace o3os::app {

enum class Verb : std::uint8_t {
  Transpose = 0,
  Instrument = 1,
  Fill = 2,
  Interpolate = 3,
  Randomise = 4,
  Humanise = 5,
  RotateUp = 6,
  RotateDown = 7,
  Invert = 8,  // pad 9
  Duplicate = 9,
  Expand = 10,
  Shrink = 11,
  Clear = 12,
  ExportSelection = 13,
  ExportPattern = 14,
  Apps = 15,
};

inline constexpr unsigned kVerbCount = 16;              // pads 1-16
inline constexpr unsigned kVerbRows = kVerbCount / 2;   // eight rows, two columns

struct VerbEntry {
  Verb verb;
  const char* label;
  bool built;  // false: drawn dim, a pad pick runs nothing
};

// The verbs of spec 3.2 in menu order (= pad order). Mock-up A's labels, except three that the panel's column cannot hold, shortened to kVerbLabelMax cells (OS-PTOOLS2 S3p review:
// DUPLICATE x2 reads DUPLICATE, EXPORT SELECTION reads EXPORT SEL, EXPORT PATTERN reads EXPORT PAT); the verbs themselves are unchanged.
inline constexpr unsigned kVerbLabelMax = 11;  // the 300 px panel's column: 11 cells of the Data face after the pad number (verb_menu_overlay.cpp)
inline constexpr VerbEntry kVerbEntries[kVerbCount] = {
    {Verb::Transpose, "TRANSPOSE", true},   {Verb::Instrument, "INSTRUMENT", true}, {Verb::Fill, "FILL", true},           {Verb::Interpolate, "INTERPOLATE", true},
    {Verb::Randomise, "RANDOMISE", true},  {Verb::Humanise, "HUMANISE", true},   {Verb::RotateUp, "ROTATE UP", true},  {Verb::RotateDown, "ROTATE DOWN", true},
    {Verb::Invert, "INVERT", true},         {Verb::Duplicate, "DUPLICATE", true}, {Verb::Expand, "EXPAND x2", true},   {Verb::Shrink, "SHRINK /2", true},
    {Verb::Clear, "CLEAR", true},           {Verb::ExportSelection, "EXPORT SEL", true}, {Verb::ExportPattern, "EXPORT PAT", true}, {Verb::Apps, "APPS >", true},
};

constexpr bool verb_labels_fit() noexcept {
  for (const VerbEntry& e : kVerbEntries) {
    unsigned n = 0;
    while (e.label[n] != '\0') {
      ++n;
    }
    if (n > kVerbLabelMax) {
      return false;
    }
  }
  return true;
}
static_assert(verb_labels_fit(), "a verb label is longer than the panel's column holds (kVerbLabelMax)");

// Entry of pad `pad` (0-based 0..47; only 0..15 are the menu's pads).
inline bool verb_of_pad(unsigned pad, unsigned& index) noexcept {
  if (pad >= kVerbCount) {
    return false;
  }
  index = pad;
  return true;
}

}  // namespace o3os::app
