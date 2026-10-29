// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The USB stem slots of a profile and the user's mapping onto them (OS-006d, ADR 0012 Decision 3, slice S3 engine side). A slot of the USB stream sums
// a bitmask of the 16 sources of stem_src (T1..T12, Master L/R, Send A/B); the StemTap does the sum in the render. This is the pure table behind it:
// the FACTORY slots of each profile, the edit of one slot, and the text form `diag usb map` prints and reads ("T1+T9", "ML").
//
//   P10 (default)  S1..S8 = T1..T8, S9 = Master L, S10 = Master R      P8  S1..S6 = T1..T6, then Master L/R      (Windows shared-mode safe)
//   P11            S1..S9 = T1..T9, then Master L/R                    P2  Master L/R
//   P4 (OS-USBOUTS) S1..S2 = T1..T2, then Master L/R                    P6  S1..S4 = T1..T4, then Master L/R      (the per-project OUTPUTS choice of Mixer > STEMS)
//   P4S            four STEREO POST-PAN stems: not supported by the engine yet (the StemTap is mono, pre-pan): the profile is refused here.
// A track with no slot of its own is heard in the master pair, which every profile carries: "others heard in the master".
//
// PERSISTENCE (OS-004d, maintainer decision 2026-10-12): the map, with the USB mode and profile, is the project's USBS chunk (docs/formats/o3-project.md). The platform copies it to the project when it
// changes and applies it after a project load (app/project_state.h: usb_state_of, stem_map_apply, UsbApply); a project without the chunk leaves the driver as it is, and the map is the
// factory one after every boot. This header does not know the project.
//
// Contexts: main loop only (the diag console, later the Config and Routing screens). apply() hands the result to the StemTap's atomics, which the render
// reads at a block boundary; a change made while a stream runs may mix the old and new masks for one block.
#include <o3os/audio/taps.h>
#include <o3os/usb/usb_types.h>

#include <cstddef>
#include <cstdint>

namespace o3os::audio {

class StemMap {
 public:
  StemMap() noexcept { reset(usb::Profile::P10); }

  // Profiles the engine can feed: every profile but P4S.
  static constexpr bool supported(usb::Profile p) noexcept { return p != usb::Profile::P4S; }

  // The factory slots of `p`. False (and nothing changed) for an unsupported profile.
  bool reset(usb::Profile p) noexcept;
  usb::Profile profile() const noexcept { return profile_; }
  std::uint32_t channels() const noexcept { return usb::profile_channels(profile_); }

  // Slot `slot` (0-based, below channels()) sums the sources in `sources` (bit i = stem_src source i; bits above 15 are dropped). False if the slot is
  // not one of the profile's.
  bool set_slot(unsigned slot, std::uint32_t sources) noexcept;
  std::uint16_t slot(unsigned slot) const noexcept { return slot < usb::kMaxChannels ? mask_[slot] : std::uint16_t{0}; }

  // Hands the profile's channel count and masks to the tap (slots beyond the profile are cleared).
  void apply(StemTap& tap) const noexcept;

 private:
  usb::Profile profile_ = usb::Profile::P10;
  std::uint16_t mask_[usb::kMaxChannels] = {};
};

// "T1+T9", "ML+MR", "SA", "-" for none, written NUL-terminated into `out`; returns the length (the text is cut to `cap` - 1).
std::size_t format_sources(std::uint32_t mask, char* out, std::size_t cap) noexcept;
// The inverse: tokens T1..T12, ML, MR, SA, SB (either case) joined by '+', or "-" / "none" for the empty set. False for anything else (`mask` is then
// untouched).
bool parse_sources(const char* text, std::uint32_t& mask) noexcept;

}  // namespace o3os::audio
