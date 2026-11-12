// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What each of the 48 pads is in the PIANO and ISOMORPHIC modes (OS-PADS-B; new Community OS behaviour, the maintainer's design of 2026-10-12/13). Pad 0 is the top left, pad = 12 * row + col,
// col 0 the leftmost of the twelve columns (the maintainer counts them from 1). Pure maps, no state: the octave comes from the note-entry settings (note_entry.h) and the keys' LEDs from
// pad_lights.h.
//
//   PIANO  col 1 (0) rows 1-4: the pattern's start, 1/4, 1/2, 3/4. Cols 2..9 (1..8): the white keys C D E F G A B C on rows 2 and 4, the black keys C# D# F# G# A# on the cols 3, 4, 6, 7, 8 (2, 3, 5, 6, 7)
//          of rows 1 and 3, as a piano is drawn; rows 1-2 are the octave above rows 3-4. Cols 10-11 (9, 10): the bend strip (OS-PIANOBEND, new Community OS behaviour):
//          rows 1 to 4 bend +2, +1, -1, -2 semitones while held, both columns of a row alike. Col 12 (11): row 1 octave up, row 4 octave down.
//   ISO    every column is a semitone and every row up is a fourth (+5 semitones): the same shape of a chord on every key.
//   A pad plays `12 * octave + semitone` (the octave of the note-entry settings, C based whatever the root is); one above the pad board's 120 does not exist and its pad is dead.

#include <o3os/app/note_entry.h>

#include <cstdint>

namespace o3os::app {

// STEP: the pads are the steps of the cursor's track, 48 to a page (pad = 12 * row + col, left to right, top to bottom); the page is the one the cursor's row is in, which in Browse follows the
// playhead. A step past the track's length has no pad.
inline constexpr int kStepPage = 48;
constexpr int step_row(int cursor_row, unsigned pad) noexcept { return cursor_row / kStepPage * kStepPage + static_cast<int>(pad); }

enum class PadKey : std::uint8_t { None, Note, Nav, OctUp, OctDown, Bend };
struct PadFn {
  PadKey kind = PadKey::None;
  std::uint8_t value = 0;  // Note: the semitones above the C of the settings' octave; Nav: n of n/4 of the pattern; Bend: the bend in semitones + 2 (bend_semitones)
};

constexpr PadFn piano_pad(unsigned pad) noexcept {
  constexpr std::uint8_t kWhite[8] = {0, 2, 4, 5, 7, 9, 11, 12};  // cols 1..8
  constexpr std::int8_t kBlack[12] = {-1, -1, 1, 3, -1, 6, 8, 10, -1, -1, -1, -1};  // by col: the semitone of the black key above, -1 none
  const unsigned row = pad / 12u;
  const unsigned col = pad % 12u;
  if (pad >= 48u) {
    return {};
  }
  if (col == 0u) {
    return {PadKey::Nav, static_cast<std::uint8_t>(row)};
  }
  if (col == 11u) {
    return row == 0u ? PadFn{PadKey::OctUp, 0} : (row == 3u ? PadFn{PadKey::OctDown, 0} : PadFn{});
  }
  if (col > 8u) {  // cols 10-11 (9, 10): the bend strip, rows 1..4 = +2, +1, -1, -2 semitones (value = semitones + 2)
    return col <= 10u ? PadFn{PadKey::Bend, static_cast<std::uint8_t>(row == 0u ? 4u : (row == 1u ? 3u : (row == 2u ? 1u : 0u)))} : PadFn{};
  }
  const unsigned upper = row < 2u ? 12u : 0u;  // rows 1-2 the octave above rows 3-4
  if ((row & 1u) != 0u) {                      // rows 2 and 4: the white keys
    return {PadKey::Note, static_cast<std::uint8_t>(kWhite[col - 1u] + upper)};
  }
  return kBlack[col] >= 0 ? PadFn{PadKey::Note, static_cast<std::uint8_t>(kBlack[col] + upper)} : PadFn{};  // rows 1 and 3: the black keys
}

// A Bend pad's pitch bend in semitones (-2, -1, +1, +2); 0 for any other pad.
constexpr int bend_semitones(PadFn fn) noexcept { return fn.kind == PadKey::Bend ? static_cast<int>(fn.value) - 2 : 0; }

// OS-PIANOBEND: the bend pads held, last pressed wins. `order` lists the held pads oldest first; the active bend is the last one's. Pure state, main loop only.
struct BendHold {
  std::uint8_t order[8] = {};
  std::uint8_t n = 0;
  std::int8_t semis[8] = {};  // each held pad's bend, in the same order

  constexpr int active() const noexcept { return n == 0u ? 0 : semis[n - 1u]; }
  constexpr bool holds(unsigned pad) const noexcept {
    for (unsigned i = 0; i < n; ++i) {
      if (order[i] == pad) {
        return true;
      }
    }
    return false;
  }
  // A bend pad went down (a pad already held moves to the end: a repeated press); returns the bend now in force.
  constexpr int press(unsigned pad, int semitones) noexcept {
    (void)release(pad);
    if (n < 8u) {
      order[n] = static_cast<std::uint8_t>(pad);
      semis[n] = static_cast<std::int8_t>(semitones);
      ++n;
    }
    return active();
  }
  // A pad came up; returns the bend now in force (the previous held pad's, or 0). A pad that is not held changes nothing.
  constexpr int release(unsigned pad) noexcept {
    for (unsigned i = 0; i < n; ++i) {
      if (order[i] == pad) {
        for (unsigned j = i + 1u; j < n; ++j) {
          order[j - 1u] = order[j];
          semis[j - 1u] = semis[j];
        }
        --n;
        break;
      }
    }
    return active();
  }
  constexpr void clear() noexcept { n = 0; }
};

constexpr PadFn iso_pad(unsigned pad) noexcept {
  if (pad >= 48u) {
    return {};
  }
  return {PadKey::Note, static_cast<std::uint8_t>(pad % 12u + 5u * (3u - pad / 12u))};
}

// The note a Note pad plays (a note number 0..120) or kNoNote when it would be above the board's last.
constexpr unsigned pad_fn_note(const NoteEntry& e, PadFn fn) noexcept {
  if (fn.kind != PadKey::Note) {
    return kNoNote;
  }
  const unsigned n = (e.octave < kEntryOctaves ? e.octave : kEntryOctaves - 1u) * 12u + fn.value;
  return n <= kEntryMaxNote ? n : kNoNote;
}

}  // namespace o3os::app
