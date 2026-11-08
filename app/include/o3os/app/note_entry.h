// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The note-entry settings (OS-PADS-B; new Community OS behaviour, the maintainer's design of 2026-10-13): the root, the
// scale, the octave and the SCALE LOCK that the pads of the NOTES, PIANO and ISO modes and the MIDI keyboard share. The jump (the edit step) is the same setting as the F3 chooser's
// (PatternEdit::edit_step), not a second one. Held Note on the Pattern screen shows the panel: the 48 pads are the four rows ROOT, SCALE, JUMP, OCTAVE + LOCK.
//
// The project's since OS-PERSIST2 (the NOTE chunk: the four bytes and the jump; app/project_state.h note_state_of / note_apply). The PAD MODE is not (a performer's layout: a device
// preference, see pad_layer.h). Pure data, main loop only, no heap.

#include <o3os/core/scales.h>
#include <o3os/model/project_state.h>

#include <cstdint>
#include <type_traits>

namespace o3os::app {

// The twelve scales the panel's pad row 2 offers, in the panel's pad order; the model's map (model::kNotePanelScales, the same twelve as the official indices) says which pad is
// which. The names come from the one table of core::kScaleNames (OS-SCALES28 Q4c): no second name table here.
inline constexpr unsigned kNoteScales = 12;
inline constexpr char kRootNames[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

inline constexpr unsigned kEntryOctaves = 10;   // OCTAVE 0..9
inline constexpr unsigned kEntryMaxNote = 120;  // the pad board's kMaxNote (pad_board.h): no pad plays above it
inline constexpr std::uint8_t kNoNote = 0xFF;

struct NoteEntry {
  std::uint8_t root = 0;     // the pitch class 0..11 (C..B)
  std::uint8_t scale = 0;    // 0..model::kNoteScaleMax (38): the official scale numbering (OS-SCALES28); the panel's pad row 2 goes through model::kNotePanelScales
  std::uint8_t octave = 3;   // 0..9; with the C root, the 36 (C-3) that the pad board always started at
  std::uint8_t lock = 0;     // SCALE LOCK: 0 off, 1 on. The pads of the NOTES mode lay out the scale and the pads and the MIDI keyboard snap to it

  // OS-SCALES28: the core table's index of the scale (the official index + 1; core index 0 is "No scale", not offered here), and the one name table's name for it (Q4c).
  std::uint8_t core_scale() const noexcept { return scale <= model::kNoteScaleMax ? static_cast<std::uint8_t>(scale + 1u) : 1u; }
  const char* scale_name() const noexcept { return core::kScaleNames[core_scale()]; }
  const char* root_name() const noexcept { return kRootNames[root < 12u ? root : 0u]; }
  bool in_scale(unsigned note) const noexcept { return core::is_in_scale(static_cast<std::uint8_t>(note), root, core_scale()); }
  bool is_root(unsigned note) const noexcept { return note % 12u == root; }
  // The lowest pad of the NOTES board: the root in the octave (a note number, 0..kEntryMaxNote).
  unsigned board_root() const noexcept {
    const unsigned n = (octave < kEntryOctaves ? octave : kEntryOctaves - 1u) * 12u + (root < 12u ? root : 0u);
    return n > kEntryMaxNote ? kEntryMaxNote : n;
  }
  // The nearest in-scale note to `note` (0..127), the lower one on a tie; a note already in the scale is itself. Pitch classes only, so it holds for every octave.
  unsigned snap(unsigned note) const noexcept {
    if (note > 127u || in_scale(note)) {
      return note;
    }
    for (unsigned d = 1; d <= 6u; ++d) {
      if (note >= d && in_scale(note - d)) {
        return note - d;
      }
      if (note + d <= 127u && in_scale(note + d)) {
        return note + d;
      }
    }
    return note;  // unreachable: the root is in every scale and no gap is wider than 4 semitones (Hirajoshi, In sen, Iwato, Kumoi, Indian)
  }
  // The next in-scale note above (dir > 0) or below `note` (0..127); `note` itself when there is none (the end of the range).
  unsigned next(unsigned note, int dir) const noexcept {
    for (unsigned k = 1; k <= 12u; ++k) {
      if (dir > 0 ? note + k > 127u : note < k) {
        break;
      }
      const unsigned n = dir > 0 ? note + k : note - k;
      if (in_scale(n)) {
        return n;
      }
    }
    return note;
  }
  // The note a pad or the MIDI keyboard plays: snapped with SCALE LOCK on.
  unsigned locked(unsigned note) const noexcept { return lock != 0u ? snap(note) : note; }
};
static_assert(std::is_trivially_copyable_v<NoteEntry> && sizeof(NoteEntry) == 4, "the NOTE chunk's fields (OS-PERSIST2)");

}  // namespace o3os::app
