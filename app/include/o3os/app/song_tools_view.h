// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The TOOLS dialog of the Song screen (OS-SONGMAP, docs/specs/SONG-UX.md section 1.4; NEW COMMUNITY OS BEHAVIOUR): F6 opens it over the rectangle of the map (no selection: the cursor's cell; on the PAT column: the
// slots of the cursor with all their tracks). Rows in the shape of the Fill dialog's (core::ParamDesc rows, session state, not the project's): OP (TRANSPOSE, INSTRUMENT, CLEAR, EXPORT WAV, FORM, BRIDGE) and, by the OP, AMOUNT
// (-24..+24 semitones), TO (the instrument 1..64), WHAT (NOTES, VOLUME, FX 1, FX 2, ALL), FORM (LOOP>SONG, POP, TECHNO, with a third row, DRUMS 1..8) or SLOTS (BRIDGE, 1..16); EXPORT WAV has no value row. A footer says
// the scope (FORM and BRIDGE: the plan of song_build.h). F1 applies it as one undo unit (song_map_actions.cpp), F8 leaves.
// EXPORT WAV (OS-SONGSHARE S5) writes no edit: it renders the rectangle's slots to a WAV file (export_actions.cpp, export_slots).

#include <o3os/app/song_build.h>
#include <o3os/app/song_edit.h>
#include <o3os/core/param.h>

#include <cstdint>

namespace o3os::app {

enum class SongToolsRow : std::uint8_t { Op = 0, Value = 1, Drums = 2 };
inline constexpr unsigned kSongToolsRows = 3;  // the dialog is drawn with room for all three (Overlay::rect is static, OS-SONG-BUILD D20)

enum class SongTool : std::uint8_t { Transpose = 0, Instrument = 1, Clear = 2, ExportWav = 3, Form = 4, Bridge = 5 };
enum class SongWhat : std::uint8_t { Notes = 0, Volume = 1, Fx1 = 2, Fx2 = 3, All = 4 };

struct SongToolsView {
  SongTool tool = SongTool::Transpose;
  std::int8_t amount = 0;           // TRANSPOSE, -24..+24
  std::uint8_t instrument = 1;      // INSTRUMENT, 1..64: set when the dialog opens to the instrument of the cursor's first note
  SongWhat what = SongWhat::All;    // CLEAR
  std::uint8_t row = 0;             // the row the cursor is on (SongToolsRow)
  SongRect rect;                    // where it acts: made when the dialog opens
  bool whole = false;               // opened on PAT or in the map with nothing selected: EXPORT WAV renders every track (the project's own mixer), not the rectangle's
  std::uint8_t patterns = 0;        // the distinct patterns in it (the footer)
  SongForm form = SongForm::LoopSong;                                  // FORM
  std::uint8_t drums = static_cast<std::uint8_t>(kSongDrumsDefault);   // FORM: tracks 1..drums are the drums
  std::uint8_t bridge_slots = static_cast<std::uint8_t>(kSongBridgeDefault);  // BRIDGE: the new slots between A and B (1..16)
  SongBuildInfo plan;               // FORM and BRIDGE: what F1 would do, from the planner (song_tools_refresh): the footer's text
};

// Plans the tool in `v` over `v.rect` and keeps what the footer says (FORM only; the other tools clear it). Called when the dialog opens and after a value changes.
void song_tools_refresh(SongToolsView& v, const model::Project& project) noexcept;

// The rows the OP shows: EXPORT WAV the OP row alone, FORM all three, the others two.
unsigned song_tools_rows(const SongToolsView& v) noexcept;
// The descriptor of a row: the OP row, or the value row of the tool in `v` (AMOUNT, TO or WHAT).
const core::ParamDesc& song_tools_desc(const SongToolsView& v, SongToolsRow row) noexcept;
const char* song_tools_caption(const SongToolsView& v, SongToolsRow row) noexcept;
float song_tools_get(const SongToolsView& v, SongToolsRow row) noexcept;
void song_tools_set(SongToolsView& v, SongToolsRow row, float value) noexcept;
// The row `detents` places on: the OP and WHAT rows roll over, AMOUNT and TO stop at their ends. False when nothing changed.
bool song_tools_step(SongToolsView& v, SongToolsRow row, int detents) noexcept;
// The value as text (no unit): the tool's or the element's name, a signed number for AMOUNT, the instrument number. Returns the length.
unsigned song_tools_text(const SongToolsView& v, SongToolsRow row, char* out, unsigned cap) noexcept;
// The elements of the CLEAR row as model::edit::kEl* bits.
std::uint8_t song_tools_elements(SongWhat what) noexcept;

}  // namespace o3os::app
