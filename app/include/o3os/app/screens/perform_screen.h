// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/screens/screen.h>

namespace o3os::app {

// F1..F8 (OS-PERF2, new Community OS behaviour: they were the eight tracks the layer affects, which OS-PADS-A moved to the TRACKS layer of the pads, a lone Shift hold): SCENE 1..3 (a tap
// recalls the twelve slots, a hold of 1 s stores them), HALF and DOUBLE time (while held), LATCH on / off, NEXT pattern (queued for the end of the pattern, blinks while it waits) and RESET
// (every FX off). The chip under a label is the key's state: the scene in force, the rate held, LATCH on, NEXT waiting; the label of a scene nobody stored is dim.
inline constexpr const char* kPerformSoftKeys[8] = {"SCENE 1", "SCENE 2", "SCENE 3", "HALF", "DOUBLE", "LATCH", "NEXT PAT", "RESET"};
// While Shift is held (OS-SHIFTROW): F1..F3 STORE 1..3 (the scene stored at once; the 1 s hold of the plain key still does), the rest does nothing. The chips under the keys stay.
// E-PERF-07: F4 / F5 pick a pattern (PICK -, PICK +), F6 queues it on the tracks that are ON (TRK PAT), F7 queues the next song pattern there (TRK NEXT), F8 sends them back to the song (TRK SONG).
inline constexpr const char* kPerformShiftKeys[8] = {"STORE 1", "STORE 2", "STORE 3", "PICK -", "PICK +", "TRK PAT", "TRK NEXT", "TRK SONG"};

// OS-DJ A1 (docs/specs/OS-DJ.md; new Community OS behaviour): the DJ page, the Performance button again. The encoder is the crossfader between the tracks of group A and group B (Shift: fine), the pads' row 4 jump
// it, F3 CURVE (SMOOTH / CUT) and F8 RESET are live; A2: F4 FILTER (held, with the encoder; springs back at the release) and F6 LATCH (the filter stays); the other keys are the later slices' and read "-". Shift (row 1) cycles the group of the pad's track: THRU, A, B. Leaving the page ends every DJ effect.
inline constexpr const char* kPerformDjSoftKeys[8] = {"BANK -", "BANK +", "CURVE", "FILTER", "SCR", "LATCH", "-", "RESET"};
inline constexpr const char* kPerformDjShiftKeys[8] = {"-", "-", "-", "-", "-", "-", "-", "-"};

// "Perform" (docs/design/product-ux.md section 4: "Live pattern triggers and FX"; Performance button; OS-011b, the performance mode, triage BASE-20, rules in
// seq/perform.h). Twelve slots, each an FX and four rows: row 0 is off, rows 1..3 are values; the 48 pads are the slots' rows (the top row of pads is row 0), so a pad turns a
// slot's FX on at that row's value on the AFFECTED tracks, and the FX stays on (latched) after the release until another row of the slot, or row 0, is pressed. While a pad is held
// the encoder changes its live value. A lone Shift hold turns the pads into the TRACKS layer (OS-PADS-A, pad_lights.h): its first row, AFFECTED, makes a track join or leave the layer (the
// twelve boxes under the slots), the others mute, set the mute mode of and solo the track of the pad's column. Rec is the EDIT
// state (the rows light red): Left / Right pick a slot (Shift: swap it with its neighbour), Up / Down change its FX, the encoder stores a value in the held row, Copy and Shift +
// Copy copy and paste a slot. Play and the screen buttons are Global's; Shift + Play falls through to the plain Play here (Pattern binds it, OS-PLAYCUR).
//
// The layer is the third of the control layers (sequencer, sequencer 2, performance: seq/control.h): it wins over the pattern's FX while it holds a parameter and the
// parameter returns to the pattern's value (or the instrument's) when it ends. Leaving the screen for any but the Master and Song ones ends every FX. Slots whose
// name is dim are FX the engine does not act on yet (the filters, VOLUME, PANNING, DELAY, REVERB, the stutter, the play mode and the pattern length are live): they are still arbitrated, nothing sounds. The slots
// are the project's (the PERF chunk), loaded and saved with it.
class PerformScreen final : public Screen {
 public:
  constexpr PerformScreen() noexcept = default;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::Perform; }
  Context context() const noexcept override { return Context::Perform; }
  const char* const* soft_keys() const noexcept override { return kPerformSoftKeys; }
  // No Cursor: Global's Shift + F1..F5 jumps resolve here, shadow the scene keys and move nothing ("-"); F6..F8 have no Shift binding and fall through to LATCH, NEXT and RESET (OS-UIFIX).
  const char* const* soft_keys_shifted() const noexcept override { return kPerformShiftKeys; }
  unsigned pages() const noexcept override { return 2; }  // SLOTS, DJ (the third, REMIX, was removed by OS-NOREMIX)
  // OS-DJ A1: the DJ page's keys and Shift row are its own, and so is its field level (the crossfader's bindings).
  Context field_context(const Model& model, const ViewState& view) const noexcept override;
  const char* const* shift_row(const ViewState& view) const noexcept override;
  // The selected row of each slot is lit and, while a pad is down, that pad brighter.
  bool pad_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) const noexcept override;
};
extern const PerformScreen kPerformScreen;

}  // namespace o3os::app
