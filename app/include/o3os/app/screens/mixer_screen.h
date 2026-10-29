// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/mixer_view.h>
#include <o3os/app/screens/screen.h>

namespace o3os::app {

// "Mixer" (docs/design/product-ux.md section 4: Mixer, Routing, Master FX; OS-011b, new Community OS behaviour, BASE-11). The Master button: the first press
// is the MIXER page, each press again the next page (F1..F4 pick one). The cursor is two numbers of the view's generated-page cursor and what they mean is mixer_view.h.
//
//   MIXER     one column per active track, then IN, SND A, SND B, CUE and MASTER: a fader (the level, 0..100), a meter (the engine's, ui::plot), the
//             level and the pan as numbers, the M and S marks and the track's routing as six dots. Left / Right pick the column, Up / Down the LEVEL or PAN field, the encoder edits
//             it (Shift: the coarse step), Delete puts the default back, F7 MUTE and F8 SOLO act on the track (on SND A / SND B they mute the effect: DELAY / REVERB MUTE).
//             Every edit of the model is a parameter: undoable, learnable, and sent to the engine as a ParamChange. IN and CUE are the engine's own levels (the board's, runtime only).
class MixerScreen final : public Screen {
 public:
  constexpr MixerScreen() noexcept = default;
  void enter(const Model& model, ViewState& view) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::Mixer; }
  Context context() const noexcept override { return Context::Mixer; }
  Context field_context(const Model& model, const ViewState& view) const noexcept override;  // OS-USBOUTS: the STEMS page (F6 APPLY)
  unsigned pages() const noexcept override { return kMixerPages; }
  const char* const* soft_keys() const noexcept override { return kMixerSoftKeys; }
  const char* const* soft_keys_shifted() const noexcept override { return kMixerShiftKeys; }
  core::ParamId focus_param(const Model& model, const ViewState& view) const noexcept override;
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override;
  int page() const noexcept override { return 1; }
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
  bool track(const Model& model, ViewState& view, int delta) const noexcept override;
};
extern const MixerScreen kMixerScreen;

}  // namespace o3os::app
