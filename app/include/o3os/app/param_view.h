// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/core/param.h>
#include <o3os/model/params.h>
#include <o3os/model/settings.h>

#include <cstdint>

namespace o3os::app {

// A value waiting to be applied (OS-USBAUDIO, a parameter whose descriptor says kParamApply): what the encoder chose and the setting does not hold yet.
struct PendingParam {
  core::ParamId id = core::ParamId::None;
  float value = 0.0f;
};

// The generated parameter page's cursor (screens/params_screen.h, OS-002d): the row it is on, and which of the six envelopes the
// ENV page shows. The page itself (the group of parameters) is the screen's page in ViewState::page. Nothing here knows a parameter:
// the rows of a page come from the descriptor tables.
//
// Press to apply (OS-USBAUDIO): the rows whose descriptor says kParamApply (USB AUDIO and USB RETURN of Config: a change re-enumerates the USB port) are not written by the encoder.
// A turn, a coarse turn or Delete only changes a pending value (the page marks it with a star); the first param.apply says what will happen (`armed`), the second writes every
// pending value into its setting. Moving the cursor, a turn, and any change of page or screen put the arming back (the page and screen changes also drop the pending values: leaving
// the page without applying discards them). The pending values are view state: nothing here is saved, and the setting is what the unit does until the second press.
struct ParamView {
  static constexpr int kMaxRows = 8;  // the most rows a page can have (the PATTERN page has all eight since OS-PADOFFSET)
  // E-CFG-03: the Config pages (device settings, screens/config_screen.cpp) held nine rows, for the MIDI page's TRANSPORT IN / OUT (and the PATTERN page's row 58); OS-MIDICCMAP S1's CC MAP made the MIDI page's ten and OS-MTC A3's MTC OUT and MTC START its twelve, OS-MTC B3's MTC IN its thirteen. The data pane is 16 rows of
  // 24 px high. The generated parameter pages keep kMaxRows: it is also the size of a module's numbered page (1-8, 9-16 ...) and of an instrument page.
  static constexpr int kConfigRows = 13;
  static constexpr unsigned kMaxPending = 4;
  int row = 0;
  int env = 0;
  // OS-INSFX S5: the target of the generated parameter page when it is an insert (the Mixer INSERTS page's Enter on a bound cell): the insert host's `where` (track * 2 + slot, 0..31), or kNoInsert for the
  // instrument in view (ViewState::instrument). Set by the Enter action, cleared when the page is left, an instrument is chosen, or the document is replaced (App::attach_document).
  static constexpr std::uint8_t kNoInsert = 0xFF;
  std::uint8_t insert = kNoInsert;
  PendingParam pending[kMaxPending] = {};
  std::uint8_t pending_n = 0;
  bool armed = false;
  // OS-SHIFTROW, config.page_default: the first press arms and notes the time, a second within kDefaultsWindowMs resets the page. Device settings have no undo, hence the second press.
  static constexpr std::uint32_t kDefaultsWindowMs = 2000;
  bool defaults_armed = false;
  std::uint32_t defaults_at = 0;
  // OS-INSTUI S1, instrument.turn: when Left / Right last turned a page of the Instrument screen. A press more than kTurnFreshMs after the last one is a fresh press (it may wrap); the
  // auto-repeats of a held arrow come every 400 ms or less, so they never are.
  static constexpr std::uint32_t kTurnFreshMs = 450;
  bool turn_seen = false;
  std::uint32_t turn_at = 0;

  const PendingParam* find_pending(core::ParamId id) const noexcept {
    for (unsigned i = 0; i < pending_n; ++i) {
      if (pending[i].id == id) {
        return &pending[i];
      }
    }
    return nullptr;
  }
  // Holds `value` for `id` (false: the list is full). Puts the arming back.
  bool set_pending(core::ParamId id, float value) noexcept {
    armed = false;
    for (unsigned i = 0; i < pending_n; ++i) {
      if (pending[i].id == id) {
        pending[i].value = value;
        return true;
      }
    }
    if (pending_n >= kMaxPending) {
      return false;
    }
    pending[pending_n++] = PendingParam{id, value};
    return true;
  }
  void clear_pending(core::ParamId id) noexcept {
    armed = false;
    for (unsigned i = 0; i < pending_n; ++i) {
      if (pending[i].id == id) {
        pending[i] = pending[--pending_n];
        pending[pending_n] = PendingParam{};
        return;
      }
    }
  }
  // Does a change of `id` re-enumerate the USB port, with the unit in AUDIO (`audio`) or not (OS-USBAUDIO)? USB AUDIO always (the mode changes); USB RETURN only while AUDIO is presented (STANDARD's
  // descriptors are the same with or without the return).
  static bool reenumerates(core::ParamId id, bool audio) noexcept {
    namespace ds = model::device_setting;
    return id == core::param_id(core::ParamKind::Device, 0, ds::UsbAudio) || (audio && id == core::param_id(core::ParamKind::Device, 0, ds::UsbReturn));
  }
  // Is the unit in AUDIO once the pending values are applied? The pending USB AUDIO, or the setting's.
  bool audio_after(const model::Settings& device) const noexcept {
    namespace ds = model::device_setting;
    const PendingParam* mode = find_pending(core::param_id(core::ParamKind::Device, 0, ds::UsbAudio));
    return (mode != nullptr ? mode->value : device.get(ds::UsbAudio)) != 0.0f;
  }
  // Will applying the pending values re-enumerate the port?
  bool will_reenumerate(const model::Settings& device) const noexcept {
    const bool audio = audio_after(device);
    for (unsigned i = 0; i < pending_n; ++i) {
      if (reenumerates(pending[i].id, audio)) {
        return true;
      }
    }
    return false;
  }
  // Leaving a page or a screen: nothing pending, nothing armed.
  void discard_pending() noexcept {
    for (unsigned i = 0; i < pending_n; ++i) {
      pending[i] = PendingParam{};
    }
    pending_n = 0;
    armed = false;
    defaults_armed = false;
  }
};

}  // namespace o3os::app
