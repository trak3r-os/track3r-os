// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-METRO: the device settings METRO and METRO VOL carried to the engine (docs/specs/OS-METRO.md), the way RETURN LEVEL is (usb_return.h UsbReturnLevelSync): over the ParamChange ring that the main
// loop produces, when a setting's generation moved (Config, a bound controller, the settings file's value at the first pass), with ramp 0 (the engine's own smoothing; the click has no ramp of its own).
// Three changes, two consumers:
//   graph master::MetroMode   0 Off, 1 Cue, 2 Main      the click's side of the line out (audio/metro.h; the graph resolves Cue to MAIN while OUT MODE is Stereo, D1)
//   graph master::MetroLevel  0..100                    METRO VOL, a linear amplitude
//   sequencer seq_param::Metro  0 or 1                  whether the sequencer emits the Metro events at all: off while METRO is Off, so an unused metronome costs the sequencer no event
// A change the ring had no room for is sent again on a later pass (a bit of `owed_` stays set). Main loop only, once a pass: two generation reads and a compare.
// OS-RECCOUNT RC2: while a Record count-in runs, the App sets the count-in override (set_count_in_override): with METRO Off the engine gets Main (2) and the sequencer's
// Metro on, at the device's METRO VOL, until the count-in ends (the take arms or is given up, or the Record screen is left). The override re-sends the mode and the sequencer's
// switch through the same ring pushes when the effective mode changes; the device setting itself is never written, and METRO Cue or Main are their own value (no extra pushes).

#include <o3os/audio/graph_params.h>
#include <o3os/core/param.h>
#include <o3os/model/params.h>
#include <o3os/model/settings.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>

namespace o3os::app {

class MetroSync {
 public:
  // OS-RECCOUNT RC2: true while a Record count-in (the Record card's COUNT-IN row) is running. The App sets and clears it; with METRO Off the effective mode below is Main.
  void set_count_in_override(bool on) noexcept { count_in_override_ = on; }

  // True when everything owed has been pushed (nothing is owed after it).
  bool step(const model::Settings& device, core::ParamRing& ring) noexcept {
    namespace ds = model::device_setting;
    const std::uint32_t mode_gen = device.generation(ds::MetroMode);
    const std::uint32_t level_gen = device.generation(ds::MetronomeVolume);
    if (!seen_ || mode_gen != mode_gen_) {
      owed_ |= kMode | kSeq;
      mode_gen_ = mode_gen;
    }
    if (!seen_ || level_gen != level_gen_) {
      owed_ |= kLevel;
      level_gen_ = level_gen;
    }
    seen_ = true;
    const float device_mode = device.get(ds::MetroMode);
    // OS-RECCOUNT RC2: the count-in clicks even with METRO Off: Main then, at the device's METRO VOL (the level is not touched). Cue and Main are their own value.
    const float mode = count_in_override_ && device_mode == 0.0f ? 2.0f : device_mode;
    if (mode != sent_mode_) {  // the override came or went: the mode and the sequencer's switch go again through the same pushes
      owed_ |= kMode | kSeq;
    }
    if ((owed_ & kMode) != 0u && ring.push(core::ParamChange{audio::master_param(audio::graph_param::master::MetroMode), mode, 0})) {
      owed_ &= ~kMode;
      sent_mode_ = mode;
    }
    if ((owed_ & kLevel) != 0u && ring.push(core::ParamChange{audio::master_param(audio::graph_param::master::MetroLevel), device.get(ds::MetronomeVolume), 0})) {
      owed_ &= ~kLevel;
    }
    if ((owed_ & kSeq) != 0u &&
        ring.push(core::ParamChange{core::param_id(core::ParamKind::Project, 0, seq::seq_param::Metro), mode != 0.0f ? 1.0f : 0.0f, 0})) {
      owed_ &= ~kSeq;
    }
    return owed_ == 0u;
  }

 private:
  static constexpr std::uint8_t kMode = 1u << 0;
  static constexpr std::uint8_t kLevel = 1u << 1;
  static constexpr std::uint8_t kSeq = 1u << 2;
  std::uint32_t mode_gen_ = 0;
  std::uint32_t level_gen_ = 0;
  std::uint8_t owed_ = 0;
  bool seen_ = false;
  float sent_mode_ = -1.0f;    // the mode value last pushed to the ring (-1: none yet), so a change of the override re-sends only when the mode really moves
  bool count_in_override_ = false;  // OS-RECCOUNT RC2: a Record count-in is running (the App sets and clears this)
};

}  // namespace o3os::app
