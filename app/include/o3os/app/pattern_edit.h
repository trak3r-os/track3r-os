// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The session state of pattern editing (OS-009 P1a.1, D4 and D5): the pattern being edited, the edit step, the pad board, the last
// used FX, the piano octave and the one rectangular selection. It lives in the ViewState (view_state.h): most of it is
// not project data and is not saved (docs/formats/og.md treats the OG's equivalents as session state); the exceptions are the note-entry
// settings and the edit step (JUMP), which are the project's since OS-PERSIST2 (the NOTE chunk, as in the OG's project.mt values). Changes to it
// are not undo units (navigation, selection, modes and choosers are view state, only the edits of the project are the journal's).
//
// The selection is a rectangle of
// steps x tracks from a start to an end corner, `active` once a range was started, `extending` while Shift is still moving its end, `column` for the column steps of Shift + Up at step 0 (OS-SELCOL: the cursor's column, the track, every track). With no range, every edit works on
// the cursor step. OS-AUDIT-56 (new Community OS behaviour, column-level block): inside one track the block also has a column range, from the start column to the end column (Column values: Note, Instr, Vol, Fx1, Fx2, the cursor's
// columns); a block that spans tracks, and the whole-column block, is whole steps (every column).

#include <o3os/app/note_entry.h>
#include <o3os/app/pad_board.h>
#include <o3os/model/step_edit.h>

#include <cstdint>

namespace o3os::app {

struct Selection {
  static constexpr int kLastColumn = 4;  // Column::Fx2
  struct Block {
    int row0;
    int row1;
    int track0;
    int track1;
    int col0 = 0;  // the columns the block covers, first <= last; all five for whole steps
    int col1 = kLastColumn;
    bool whole_steps() const noexcept { return col0 == 0 && col1 == kLastColumn; }
  };

  bool active = false;     // a range was started (it may still be one step)
  bool extending = false;  // the end follows the cursor
  bool column = false;     // the whole-column step of Shift + Up at step 0
  bool follow = false;     // made while the cursor rides the playhead (OS-BUG-WATCHSEL): the arrows set the end row, the cursor does not, and the follow keeps the block
  bool end_lags = false;   // a downward quarter jump (OS-SELSHIFT) left the end one row above the cursor; Shift + Down keeps that lag (main loop only, no ISR)
  std::int16_t start_row = 0;
  std::int16_t end_row = 0;
  std::int8_t start_track = 0;
  std::int8_t end_track = 0;
  std::int8_t start_col = 0;  // the cursor's column where the block began and where its end is; they count only while both corners are in one track
  std::int8_t end_col = 0;

  // OS-SELCOL: the first press of Shift + Up at step 0 on the Instr, Vol, Fx1 or Fx2 column selects that one column of the track (`column` stays set while Shift is held, so the next press goes on to the whole track).
  bool column_only() const noexcept { return column && (start_col != 0 || end_col != kLastColumn); }

  // The block, normalised (first <= last on every axis). With no range it is the cursor step (every column: the edits then read the cursor's column themselves).
  Block block(int cursor_row, int cursor_track) const noexcept {
    if (!active) {
      return Block{cursor_row, cursor_row, cursor_track, cursor_track};
    }
    Block b{start_row < end_row ? start_row : end_row, start_row < end_row ? end_row : start_row,
            start_track < end_track ? start_track : end_track, start_track < end_track ? end_track : start_track};
    if (start_track == end_track && (!column || column_only())) {
      b.col0 = start_col < end_col ? start_col : end_col;
      b.col1 = start_col < end_col ? end_col : start_col;
    }
    return b;
  }
  void clear() noexcept {
    active = false;
    extending = false;
    column = false;
    follow = false;
    end_lags = false;
  }
};

// The STEP mode's held pad (OS-PADS-B): the step under the finger (an absolute row of the track, chosen when the pad went down: the page may move), whether the encoder or a fresh write
// has changed it (then the release does not clear it).
struct StepHold {
  std::int16_t row = -1;  // -1: none
  std::int8_t track = 0;
  std::uint8_t pad = 0;
  bool edited = false;
};

// OS-PATSWITCH: the step changes the App waits for a switch to show before it gives the wait up (a switch lands at the next boundary: one or two changes are seen; a command the sequencer never took must not hold the view for ever).
inline constexpr std::uint8_t kSwitchGuardSteps = 4;

struct PatternEdit {
  std::uint8_t pattern = 1;       // the pattern shown and edited (1..255); one that does not exist yet shows empty
  std::uint8_t edit_step = 1;     // steps the cursor moves after an entry (0..32)
  std::uint8_t last_fx = 0;       // last used FX as a picker index (0 is None)
  bool chooser_held = false;      // the open chooser was opened by a held F-key and closes on its release
  // OS-LIVEREC (rec_place.h; track arming, P1d / PAR-047, is dropped: nothing here, nothing in any file: it was session state and never saved). OS-RECSWAP: Rec enters Edit free, Shift + Rec follows: the cursor and the view do
  // not ride the playhead while it records (free); false is follow. Cleared when Edit is left.
  bool rec_free = false;
  // The take (OS-LIVEREC, rec_actions.cpp): the undo group that holds everything recorded since recording began is open on the editor. Main loop only.
  bool take_open = false;
  bool take_wrote = false;  // ... and holds at least one record: the info line counts it as an undo level (it is one the moment Undo is pressed)
  bool take_noticed = false;  // a take that grew too long was split, and the user was told (once for a recording)
  // OS-FXREC S2 (spec 3.5): the strip of a take that recorded an FX. The take's first recorded FX posted "REC FX: <PARAMETER>" once, and a write dropped for want of a free column posted
  // "REC FX: NO FREE FX COLUMN" once (a knob sends up to about 100 messages a second). Cleared when the take ends.
  bool rec_fx_said = false;
  bool rec_fx_full = false;
  // OS-ENCLEARN (main loop only): the hold of Instr + FX1 with the encoder turned in STEP (Edit free) keeps one undo group open, as a take does (rec_take_sync), until a key is let go; `enc_said` is the
  // gesture's "REC FX: <PARAMETER> HAS NO FX" strip line, once. Both clear when either key is released (App::run_once).
  bool enc_take = false;
  bool enc_said = false;
  // The tracks whose recorded note is still held (bit n = track n), the note each holds (0..127), the step it was written to and the playhead's step when it was played (the release's OFF
  // needs both). Main loop only. Cleared when the sequencer stops.
  std::uint16_t rec_open = 0;
  std::uint8_t rec_note[16] = {};
  std::uint8_t rec_row[16] = {};
  std::uint8_t rec_head[16] = {};
  PadBoard pads;                  // the NOTES mode's layout: scale, root from `entry` (apply_entry), the row offset from the device setting PAD OFFSET (App::poll_pad_offset; 12 until it moves)
  NoteEntry entry;                // OS-PADS-B: root, scale, octave and SCALE LOCK of the note entry (held Note, note_entry.h); the jump is edit_step
  std::uint8_t pad_held[48] = {};    // the note + 1 that the PIANO / ISO pad under the finger started (0: none), so its release ends that note even if the octave changed meanwhile
  StepHold step_hold;
  std::uint8_t queued_pattern = 0;   // the pattern a PATS pad queued and the sequencer has not started yet (0: none): the pad blinks
  // OS-PATSWITCH (runtime only, main loop only): the pattern a pick in the held PATTERN chooser asked the sequencer to switch to at the next step boundary and the published position has not shown yet (0: none).
  // While it is set the view is not pulled back to the playing pattern (App::poll_play_position clears it when the pattern shows, when the transport stops, and after kSwitchGuardSteps step changes without the landing).
  std::uint8_t switch_to = 0;
  std::uint8_t switch_age = 0;       // the step changes seen since the pick
  std::uint8_t last_note = kNoNote;  // the last note a pad, the MIDI keyboard or a preview played (0..127), kNoNote before any: the STEP mode writes it
  std::uint8_t last_velocity = 0;  // OS-RECMICRO: the velocity (1..127) of the MIDI keyboard note being handed to step.midi_note (App::poll_midi_notes); REC VELOCITY records it. 0: none
  Selection selection;

  // The NOTES board follows the settings: the scale only with SCALE LOCK on (off, every pad plays its chromatic note and the scale only lights PIANO / ISO / the info line).
  void apply_entry() noexcept { pads.set(entry.lock != 0u ? entry.core_scale() : std::uint8_t{1}, pads.offset(), static_cast<std::uint8_t>(entry.board_root())); }
  // The panel's pad (held Note): row 1 ROOT, row 2 SCALE, row 3 JUMP 0..11, row 4 OCTAVE 0..9 and the last two pads SCALE LOCK off / on. True when a setting changed.
  bool entry_pad(unsigned pad) noexcept {
    const unsigned col = pad % 12u;
    if (pad / 12u == 1u) {  // row 2 SCALE: the same twelve pads, through the model's map (model::kNotePanelScales: the pad -> the official scale index, OS-SCALES28)
      const std::uint8_t value = model::kNotePanelScales[col];
      if (entry.scale == value) {
        return false;
      }
      entry.scale = value;
      apply_entry();
      return true;
    }
    std::uint8_t* to = nullptr;
    switch (pad / 12u) {
      case 0: to = &entry.root; break;
      case 2: to = &edit_step; break;
      case 3: to = col < 10u ? &entry.octave : &entry.lock; break;
      default: return false;
    }
    const std::uint8_t value = static_cast<std::uint8_t>(pad / 12u == 3u && col >= 10u ? col - 10u : col);
    if (*to == value) {
      return false;
    }
    *to = value;
    apply_entry();
    return true;
  }
};

}  // namespace o3os::app
