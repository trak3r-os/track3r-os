// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/screens/screen.h>
#include <o3os/app/wave_math.h>

namespace o3os::app {

// F1..F8 of the two pages, at most 8 characters each (the key row shows the page in view; the page-0 table is the one an unbound key's notification quotes).
inline constexpr const char* kWaveSoftKeys[8] = {"START", "LOOP S", "LOOP E", "END", "ZC", "PREVIEW", "ZOOM-", "UNDO"};
inline constexpr const char* kChopSoftKeys[8] = {"AUTO", "EVEN", "ZC", "ADD", "DEL", "PREVIEW", "ZOOM-", "UNDO"};
// OS-INSTUI S4: the pages of a WAVETABLE and a GRANULAR instrument (the official manual describes Position and Window for a wavetable and Position, Length, Shape and Loop for a granular
// instrument, docs/ui/official-manual-map.md, not verified; the play mode is the Shift + F1 MODE key here). PREVIEW stays on F6 as on the other pages.
inline constexpr const char* kWtSoftKeys[8] = {"POSITION", "WINDOW", "-", "-", "-", "PREVIEW", "ZOOM-", "UNDO"};
inline constexpr const char* kGranSoftKeys[8] = {"POSITION", "LENGTH", "SHAPE", "LOOP", "-", "PREVIEW", "ZOOM-", "UNDO"};
// While Shift is held, on every page (OS-SHIFTROW, OS-INSTUI S4): F1 MODE (the list of the eight play modes), F5 REDO (OS-UXF-SMP item 11: F7 / F8 are INST - / INST + as on Sample load; REDO was F8), F6 PREVIEW from the selected point or slice
// (the window's edge when it is out of view), F7 / F8 INST - / INST + (the instrument in view, round); the rest does nothing.
inline constexpr const char* kWaveShiftKeys[8] = {"MODE", "-", "-", "-", "REDO", "PRV SEL", "INST -", "INST +"};
// The CHOP page's own Shift row (OS-UXF-SMP item 3c): F4 TO PAT writes the slices into the Pattern.
inline constexpr const char* kChopShiftKeys[8] = {"MODE", "-", "-", "TO PAT", "REDO", "PRV SEL", "INST -", "INST +"};

// The screen shows one of four views of the instrument in view (OS-INSTUI S4): a WAVETABLE instrument (play mode 6) and a GRANULAR one (7) have their own; the others show the WAVE page (the points) or the
// CHOP page (the slices), whichever the page says (the slice modes, 4 and 5, are put on CHOP when the mode is chosen).
enum class WaveView : std::uint8_t { Wave, Chop, Wavetable, Granular };

// "Wave / Chop" (docs/design/product-ux.md section 4: "Start, loop, end, zoom; CHOP page for slices"; SamplePlay, again: CHOP; OS-011a, new Community OS behaviour after
// PAR-061, PAR-091, PAR-142 and BASE-02). It works on the instrument in view (a pad chooses it) and its sample: a waveform from the App's peak cache at any zoom, the
// four points and the slice table of the instrument, edited as the model keeps them (the OG instrument file's 0..65535 points, at most 48 slices).
//
//   WAVE page   Left / Right pick START, LOOP S, LOOP E or END (F1..F4 too); the encoder moves it (Shift: 16 columns a click); the loop is pushed or pulled
//               as the points move, so that start < end and start <= loop start < loop end <= end always hold (the loop pushes ahead of the start). F5 ZC: the encoder snaps to a zero crossing, in the direction it moves.
//   CHOP page   Left / Right pick a slice, the encoder moves its start between its neighbours. F1 AUTO runs the transient slicer as a job (transient.h) and replaces
//               the slices; F2 EVEN makes n equal ones (Shift + Up / Down sets n, 2..48); F4 ADD splits the selected slice at the middle of its start and end (a slice at 0 first when there are none);
//               F5 DEL removes the selected one. Slicing puts the instrument in beat slice mode (play mode 5), the mode whose pads play slices, and says so.
//   both        Up / Down zoom around the selected point or slice (F7 zooms out too); F6 PREVIEW plays the instrument from its start point while it is down (Shift: from the selected point or slice, new OS behaviour), the playhead follows the voice the pads, the MIDI keyboard or Preview last played (OS-FU3); F8 UNDO, Shift + F8 REDO (the project's: every edit is one undo unit).
//   pads        pad n plays slice n of a beat slice instrument (PAR-091; product-ux: "Pad n plays slice n, Shift + pad selects without sounding") and, on the CHOP
//               page, selects it; the LEDs of the pads that have a slice are lit, the selected one brighter (PAR-061). Pad 0 is the top left pad. The note is held until the
//               pad is released and goes through the engine's live notes (PreviewSink::pad_down / pad_up, OS-005g: audio/live_notes.h).
//
// The engine takes an edit up when playback is stopped (sample_bank.h, THE HANDOVER): until then the screen says "SOUNDS WHEN STOPPED".
class WaveScreen final : public Screen {
 public:
  constexpr WaveScreen() noexcept = default;
  void enter(const Model& model, ViewState& view) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::Wave; }
  Context context() const noexcept override { return Context::Wave; }
  Context field_context(const Model& model, const ViewState& view) const noexcept override;
  unsigned pages() const noexcept override { return 2; }
  // The button again turns WAVE and CHOP; a WAVETABLE or GRANULAR instrument has the one page (its mode picks the view).
  bool repress(const Model& model, ViewState& view) const noexcept override;
  // The row under the cursor of the WAVETABLE and GRANULAR views is a parameter (the source page's rows): the encoder is param.edit there (Delete resets, Learn binds).
  core::ParamId focus_param(const Model& model, const ViewState& view) const noexcept override;
  const char* const* soft_keys() const noexcept override { return kWaveSoftKeys; }
  const char* const* soft_keys_shifted() const noexcept override { return kWaveShiftKeys; }
  const char* const* shift_row(const ViewState& view) const noexcept override;  // the CHOP view has TO PAT on F4
  bool pad_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) const noexcept override;
  // The cursor: the four points (WAVE) or the slices (CHOP); Left / Right move it, Up / Down zoom.
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override;
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
};
extern const WaveScreen kWaveScreen;

// The view in view: by the play mode of the instrument, then the page (see WaveView).
WaveView wave_view_of(const Model& model, const ViewState& view) noexcept;
// How many rows the cursor of the WAVETABLE (2: position, window size) and GRANULAR (4: position, length, shape, loop) views has; 0 for the others.
unsigned wave_source_rows(WaveView view) noexcept;

// The window of the screen's state over a sample of `frames`: the whole sample when span is 0, else the stored one made valid.
Window wave_window(const WaveState& state, std::uint32_t frames) noexcept;
// Zooms the window of the screen in view (dir > 0 in, < 0 out) around the selected point or slice; false when there is no sample or nothing changed.
bool wave_zoom(const Model& model, ViewState& view, int dir) noexcept;
// The instrument slot of the view (ViewState::instrument is 1-based).
unsigned wave_slot(const ViewState& view) noexcept;
// The instrument in view was changed on Wave or Sample edit (OS-INSTPICK): the windows, the selected slice and the selection are frames of the old sample, so the new one starts
// fitted with nothing selected. (The peak cache follows by itself: the App's pass rebuilds it when the slot in view or its version is not the cache's.)
void wave_view_reset(ViewState& view) noexcept;
// The cursor and the window back inside the sample and the tables of the instrument in view (the App calls it after anything that may have changed them).
bool wave_sync(const Model& model, ViewState& view) noexcept;

}  // namespace o3os::app
