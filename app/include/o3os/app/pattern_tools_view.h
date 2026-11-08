// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The TOOLS dialog of the Pattern screen (OS-PTOOLS2 S2, docs/specs/OS-PTOOLS2.md section 3.1 point 3 and the S2 row of the slice plan): the Song TOOLS dialog's shape
// (song_tools_view.h's row model) over the selection's block — the block, a column block or the cursor step. The menu's TRANSPOSE and INSTRUMENT rows (pads 1 and 2, or F7 held
// + the pad) open it. Rows: OP (TRANSPOSE, INSTRUMENT, CLEAR — Left / Right flips) and the OP's value row: AMOUNT (-24..+24 st, Shift + encoder an octave), TO (the instrument 1..64)
// or WHAT (CLEAR). A footer names the target (BLOCK TRACK 1 ROWS 01-16, or the cursor step) and counts its steps and notes (16 STEPS  5 NOTES). F1 APPLY runs the tool as ONE undo
// unit through pe::edit_selection; F8 BACK leaves the project untouched. The rows are session state (Q6: no format change), kept between opens.

#include <o3os/app/pattern_edit.h>
#include <o3os/app/song_tools_view.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/param.h>

#include <cstdint>

namespace o3os::model {
class Project;
}

namespace o3os::app {

enum class PatternToolsRow : std::uint8_t { Op = 0, Value = 1 };
inline constexpr unsigned kPatternToolsRows = 2;  // OP and the value row; the dialog is drawn with room for the footer's two lines and the keys

enum class PTool : std::uint8_t { Transpose = 0, Instrument = 1, Clear = 2 };

struct PatternToolsView {
  PTool tool = PTool::Transpose;
  std::int8_t amount = 0;          // TRANSPOSE, -24..+24 semitones (kept for the session)
  std::uint8_t instrument = 1;     // INSTRUMENT, 1..64: set when the dialog opens to the block's first note's instrument
  SongWhat what = SongWhat::All;   // CLEAR
  std::uint8_t row = 0;            // the row the cursor is on (PatternToolsRow)
};

// The rows the dialog shows (always two: OP and the value row).
unsigned pattern_tools_rows(const PatternToolsView& v) noexcept;
// The descriptor of a row: the OP row, or the value row of the tool in `v` (AMOUNT, TO or WHAT).
const core::ParamDesc& pattern_tools_desc(const PatternToolsView& v, PatternToolsRow row) noexcept;
const char* pattern_tools_caption(const PatternToolsView& v, PatternToolsRow row) noexcept;
float pattern_tools_get(const PatternToolsView& v, PatternToolsRow row) noexcept;
void pattern_tools_set(PatternToolsView& v, PatternToolsRow row, float value) noexcept;
// The row `detents` places on: the OP and WHAT rows roll over, AMOUNT and TO stop at their ends. False when nothing changed.
bool pattern_tools_step(PatternToolsView& v, PatternToolsRow row, int detents) noexcept;
// The value as text (no unit): the tool's or the element's name, a signed number for AMOUNT, the instrument number. Returns the length.
unsigned pattern_tools_text(const PatternToolsView& v, PatternToolsRow row, char* out, unsigned cap) noexcept;

// The footer's first line: where F1 acts — "BLOCK  TRACK 1  ROWS 01-16" (TRACKS t0-t1 over several), a column block with its column ("BLOCK  TRACK 1  VOL  ROWS 01-16"),
// or the cursor step alone ("STEP  TRACK 1  ROW 01"). Rows and tracks are 1-based, as the grid shows them.
void pattern_tools_target(const Selection& sel, int cursor_row, int cursor_track, core::FixedString<48>& out) noexcept;
// The footer's second line's counts: the steps of the block that lie inside their track, and of those the ones that hold a note. A pattern that does not exist yet counts
// its whole range, with no notes.
void pattern_tools_count(const model::Project* project, unsigned pattern, const Selection::Block& b, unsigned& steps, unsigned& notes) noexcept;

}  // namespace o3os::app
