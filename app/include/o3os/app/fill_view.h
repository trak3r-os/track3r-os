// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Fill dialog's rows and session state (OS-PTOOLS part 1, docs/specs/OS-PTOOLS.md; OS-FILL2, docs/specs/OS-FILL2.md; NEW COMMUNITY OS BEHAVIOUR: how the official firmware's popup compares is an open research question). Seven rows: WHAT (NOTE, INSTR,
// VOLUME, FX 1, FX 2), TYPE, N (EVERY N, SCALE) or EVENTS (EUCLID) or DENSITY (RANDOM), MODE (CONST, RAMP, RANDOM), KEY (a note fill: root and scale in one row) or FX TYPE (an FX fill), FROM (VALUE in CONST) and TO.
// OS-PTOOLS2 S3c (Q2 a): there is no SCOPE row — the selection is the target (a block, a column block, the cursor step; Shift + Up at row 1 the track, again the pattern), and the dialog's footer names it (fill_target_text).
// Row 6 and the captions of rows 2 and 6 follow WHAT, TYPE and MODE; a row that means nothing for the other rows is drawn dim, shows "-" and ignores the encoder.
// The values are session state (OS-009 D5), not the project's and not Learn targets. The generators are model/pattern_fill.h; the actions are fill_actions.cpp.

#include <o3os/app/pattern_edit.h>
#include <o3os/core/param.h>
#include <o3os/model/pattern_fill.h>

#include <cstdint>

namespace o3os::app {

enum class FillRow : std::uint8_t { What = 0, Type = 1, Count = 2, Mode = 3, Key = 4, From = 5, To = 6 };
inline constexpr unsigned kFillRows = 7;

struct FillView {
  model::fill::Plan plan;
  std::uint8_t row = 0;      // the row the cursor is on (FillRow)
  bool valid = false;        // a fill was made with the dialog: the action pattern.fill (no key since OS-QJUMP, it was Shift + Fx) repeats it, with the stored FROM and TO. Before that pattern.fill is the OS-CFGWIRE fill (every Nth step); tools.repeat (F6) says NOTHING TO REPEAT. Cleared when a document is loaded: the last fill is the session's, not the project's
  model::fill::Rng rng;      // advances with every random fill, so a repeated random fill draws again
  model::fill::Value open;   // what the cursor held when the dialog opened (OS-FILL2 Q2): the FROM and TO that a change of WHAT starts from
  // OS-PTOOLS2 S1 (spec 3.1 items 9 and 5): the rows are kept for the session. `primed`: the dialog has been opened once, so its FROM and TO are the user's (the first open takes them from the cursor, and a
  // CONST fill takes its VALUE from the cursor at every open). `vary_*`: the last fill the dialog made and the state of its pattern right after it, so that F2 VARY can take that fill back, and only that one,
  // before it draws again (fill_vary).
  bool primed = false;
  std::uint8_t vary_pattern = 0;      // the pattern the last fill wrote (0: none)
  std::uint32_t vary_generation = 0;  // its generation (Project::pattern_generation) right after: a later edit of that pattern moves it
};

const core::ParamDesc& fill_desc(FillRow row) noexcept;
// OS-PTOOLS2 S3c: the target of the fill, in the Song TOOLS footer's words (`BLOCK  TRACK 3  ROWS 1-16`): the selection — STEP for the cursor step (or a one-cell selection), BLOCK for a range, TRACK when the block is the
// whole column of Shift + Up at row 1, PATTERN when it covers every track. `steps` and `tracks` are the pattern's length and track count. Returns the length.
unsigned fill_target_text(const Selection& sel, int cursor_row, int cursor_track, int steps, int tracks, char* out, unsigned cap) noexcept;
// Does the plan draw at random (TYPE RANDOM, or MODE RANDOM where the mode is used)? F2 VARY is live then (OS-PTOOLS2 S1).
bool fill_random(const FillView& f) noexcept;
// The caption: row 4 is "N" for EVERY N and SCALE, "EVENTS" for EUCLID and "DENSITY" for RANDOM; row 6 "KEY" or, for an FX fill, "FX TYPE"; row 7 "VALUE" in CONST (one value), else "FROM".
const char* fill_caption(const FillView& f, FillRow row) noexcept;
// Whether the row means anything with the other rows as they are (an unused row is drawn dim and does not change): MODE is dim for a note fill of TYPE RANDOM or SCALE (they choose the notes), KEY for INSTR and VOLUME, TO in CONST (but not for that note window).
bool fill_row_used(const FillView& f, FillRow row) noexcept;
// The row's value: the enumerated rows' index, the count (N, EVENTS or DENSITY), KEY's scale, the FX TYPE's picker index (1..59), FROM and TO as the plan holds them.
float fill_get(const FillView& f, FillRow row) noexcept;
// Sets the row, clamped to its range. A change of WHAT puts FROM and TO (and FX TYPE) back at what the dialog opened with; a change of FX TYPE puts FROM and TO at the new type's default (OS-FILL2 D3).
void fill_set(FillView& f, FillRow row, float value) noexcept;
// The row `detents` places on: an enumerated row (WHAT, TYPE, MODE, KEY, FX TYPE) rolls over, the others stop at their ends; FROM and TO of INSTR walk the instrument order (64..49, then 1..48). False for an unused row or when nothing changed.
bool fill_step(FillView& f, FillRow row, int detents) noexcept;
// Shift + encoder (OS-SHIFTENC): the row's coarse step: N 4, DENSITY 10, FROM and TO 12 for a note and 10 for the others, KEY of a note fill the root (a ring of twelve). The other enumerated rows keep their fine step.
bool fill_step_coarse(FillView& f, FillRow row, int detents) noexcept;
// The value as text (no unit): the names of the enumerated rows, the note name (`octave_shift`: the device setting MIDDLE C, as the grid names it), "C# Dorian" for KEY, the picker's name for FX TYPE,
// the words of an FX value where it has some (fx_text.h), "-" for an unused row. Returns the length.
unsigned fill_text(const FillView& f, FillRow row, char* out, unsigned cap, int octave_shift = 0) noexcept;

}  // namespace o3os::app
