// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/screens/screen.h>
#include <o3os/app/wave_math.h>

namespace o3os::app {

inline constexpr const char* kSampleEditSoftKeys[8] = {"CROP", "NORM", "REVERSE", "FADE IN", "FADE OUT", "AMP", "PREVIEW", "UNDO"};
// The FX page (OS-AUDIT-15, new Community OS behaviour; the Sample edit button again): FILTER and COMPRESSOR, each over the selection or all of the sample, with the parameters of the page's rows. The Shift row is the edit page's.
inline constexpr const char* kSampleEditFxSoftKeys[8] = {"FILTER", "COMPRESS", "-", "-", "-", "-", "PREVIEW", "UNDO"};
// While an effect's job runs (OS-SEDIT-CANCEL, new Community OS behaviour; SampleEditState::running): F8 is CANCEL (UNDO is back when the job has ended). Drawn at run time, so not a manual table.
inline constexpr const char* kSampleEditBusySoftKeys[8] = {"CROP", "NORM", "REVERSE", "FADE IN", "FADE OUT", "AMP", "PREVIEW", "CANCEL"};
inline constexpr const char* kSampleEditFxBusySoftKeys[8] = {"FILTER", "COMPRESS", "-", "-", "-", "-", "PREVIEW", "CANCEL"};
// The PLUGINS page (OS-CEPTFX S4a, new Community OS behaviour; the Sample edit button once more): the plugins of /Effects with their parameters. F1 applies the plugin (S4b wires it), F2 reads the folder again, F3 puts
// every parameter back to its default. The Shift row is the edit page's.
inline constexpr const char* kSampleEditPlugSoftKeys[8] = {"APPLY", "RESCAN", "DEFAULTS", "-", "-", "-", "PREVIEW", "UNDO"};
inline constexpr const char* kSampleEditPlugBusySoftKeys[8] = {"APPLY", "RESCAN", "DEFAULTS", "-", "-", "-", "PREVIEW", "CANCEL"};
// While Shift is held (OS-SHIFTROW): F1 / F2 the AMIGA conversion at 8,287 / 16,574 Hz, F3 its 8-bit option (OS-AMG1), F4 DC REMOVE (OS-SEDFX), F6 the sample as a WAV, F7 PREVIEW from the selection's edge, F8 REDO.
inline constexpr const char* kSampleEditShiftKeys[8] = {"AMG 8K", "AMG 16K", "Q8", "DC REM", "NEXT INS", "SAVE WAV", "PRV EDGE", "REDO"};

// "Sample edit" (docs/design/product-ux.md section 4: "Offline crop, EQ, overdrive, Amiga prep, undo"; OS-011a, new Community OS behaviour after BASE-16, which lists
// normalise, cut, reverse, amplify and the fades among 13 effects, and undo snapshots): the instrument in view's sample, a selection
// and seven offline effects. Crop, normalise, reverse, fade in, fade out, amplify and (Shift + F4, OS-SEDFX) DC removal (sample_dsp.h) act on the selection, or on the whole sample when there is none.
//
//   Left / Right pick the edge of the selection (start or end), the encoder moves it (Shift: 16 columns a click), Up / Down zoom, Delete (ALL) clears the selection; F7 PREVIEW plays the sample while it is down (Shift: from the edge being moved) with a playhead (OS-FU3).
//   F1 CROP (needs a selection) F2 NORM F3 REVERSE F4 FADE IN F5 FADE OUT F6 AMP (the factor is the percentage on the screen, Shift + Up / Down sets it, 10..800)
//   F8 UNDO / Shift + F8 REDO: the sample's own history (three steps), which also puts back the instrument's points and slices a crop changed.
//   The Sample edit button again opens the FX page (OS-AUDIT-15): FILTER (one biquad: low, high or band pass, cutoff, resonance) and COMPRESSOR (threshold, ratio, attack, release, makeup, peak or RMS), F1 / F2, over the
//   selection like the others; Up / Down / Left / Right pick the row, the encoder (Shift: coarse) changes its value. The values stay while the OS runs.
//   Once more the PLUGINS page (OS-CEPTFX S4a): the plugins of /Effects on the left, the selected one's parameters on the right (Left / Right move the focus and the column; the values are kept for the last eight plugins).
//
// THE EDIT PROTOCOL (the one to review). An effect runs as a core::Job on the main loop, a slice of a pass at a time, never in the audio path, and it never
// writes the sample the voices may be reading: its result goes into another buffer (a spare of the bank's pool, sample_bank.h), so while it runs, and for as long as
// the engine has not been told, every voice reads what it always read. When it is done the internal action sample.commit (an undo unit of the project) makes the
// new buffer the slot's audio, keeps the old one as an undo step, remaps the instrument's points when the length changed (crop), and asks the bank for the
// handover, which swaps the voices' descriptor only with playback stopped and the pool quiet. Undo and redo are the same swap in the other direction.
// A buffer is reused only when nothing references it (a slot's audio, the engine's binding, a history step). The project's own undo (F8 elsewhere) does not
// know the audio: this screen's F8 is the history of the sample.
class SampleEditScreen final : public Screen {
 public:
  constexpr SampleEditScreen() noexcept = default;
  void enter(const Model& model, ViewState& view) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::SampleEdit; }
  Context context() const noexcept override { return Context::SampleEdit; }
  const char* const* soft_keys() const noexcept override { return kSampleEditSoftKeys; }
  unsigned pages() const noexcept override { return 3; }
  Context field_context(const Model& model, const ViewState& view) const noexcept override;
  const char* const* soft_keys_shifted() const noexcept override { return kSampleEditShiftKeys; }
  // The cursor: the two edges; Left / Right pick one, Up / Down zoom.
  // (On the FX page: the rows. On the PLUGINS page: the plugins of the list, or the parameters when they have the focus.)
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override;
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
};
extern const SampleEditScreen kSampleEditScreen;

// Whether the FX page is the one in view.
bool sedit_fx_page(const ViewState& view) noexcept;
// Whether the PLUGINS page is (OS-CEPTFX S4a).
bool sedit_plug_page(const ViewState& view) noexcept;
// The window of the state over a sample of `frames` (whole when span is 0).
Window sedit_window(const SampleEditState& state, std::uint32_t frames) noexcept;
// Zooms around the selected edge; false when there is no sample or nothing changed.
bool sedit_zoom(const Model& model, ViewState& view, int dir) noexcept;
// The selection and the window back inside the sample in view.
bool sedit_sync(const Model& model, ViewState& view) noexcept;

}  // namespace o3os::app
