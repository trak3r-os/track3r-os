// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/screens/screen.h>

namespace o3os::app {

// F1..F6 are the six pages, F7 is APPLY (OS-USBAUDIO: the USB page's pending values); F8 is not used, except on the INFO page of a build that has a firmware update path (below).
inline constexpr const char* kConfigSoftKeys[8] = {"MIDI", "DISPLAY", "SD", "INFO", "USB", "PATTERN", "APPLY", "--"};
// OS-FWCONFIG: the INFO page's row where the platform attached a firmware update path (Model::firmware): F8 UPDATE opens the update dialog (firmware_view.h). F8 only, on that page only: F1..F6 stay the pages.
inline constexpr const char* kConfigInfoSoftKeys[8] = {"MIDI", "DISPLAY", "SD", "INFO", "USB", "PATTERN", "APPLY", "UPDATE"};
inline constexpr unsigned kConfigPageInfo = 3;  // the INFO page's number (F4)
// The plain row in force on page `page` (the screen's render draws it; a test reads it): the INFO page of a build with an update path labels F8 UPDATE, every other page and build "--".
inline const char* const* config_key_row(bool firmware, unsigned page) noexcept { return firmware && page == kConfigPageInfo ? kConfigInfoSoftKeys : kConfigSoftKeys; }
// While Shift is held (OS-SHIFTROW): F7 DEFAULTS (config.page_default, twice within 2 s); the rest does nothing.
inline constexpr const char* kConfigShiftKeys[8] = {"-", "-", "-", "-", "-", "-", "DEFAULTS", "-"};

// OS-USBDISK U2b: the USB page's two rows after its settings are not settings. CARD AS DISK is an action row (F7 twice hands the card to the computer; F7 twice again takes it back), READ ONLY a
// session choice (the encoder toggles it; RAM only, Q4). config_row() says which one the cursor is on (Param on every other row and page).
enum class ConfigRow : std::uint8_t { Param, CardAsDisk, ReadOnly };
ConfigRow config_row(const ViewState& view) noexcept;
// False on SD and INFO (no parameter row): F7 has no label there and does nothing (OS-QAFIX-7).
bool config_page_has_rows(const ViewState& view) noexcept;
// F7's label (the plain row): SHARE or EJECT on the CARD AS DISK row (WAIT while the card changes hands), APPLY everywhere else (docs/specs/OS-USBDISK.md 9). The armed press reads SHARE? or EJECT? (key_prompt.cpp).
inline const char* config_f7_label(const ViewState& view) noexcept {
  if (config_row(view) != ConfigRow::CardAsDisk) {
    return kConfigSoftKeys[6];
  }
  using Phase = ViewState::DiskView::Phase;
  return view.disk.phase == Phase::Out ? "EJECT" : (view.disk.phase == Phase::Working ? "WAIT" : "SHARE");
}
// What the READ ONLY row shows: while the card is out, the disk the computer has (usb0 DiskState::writable, told with the phase: the choice as it was at the share, and a card protected by itself); otherwise
// the choice for the next share. The row never shows a policy the computer does not have.
inline bool config_read_only_shown(const ViewState& view) noexcept {
  return view.disk.phase == ViewState::DiskView::Phase::Out ? !view.disk.writable : view.disk.read_only;
}

// "Config" (docs/design/product-ux.md section 4: "MIDI (clock, input routing), USB mode, display, SD, firmware"; OS-011a, new Community OS behaviour after PAR-116, PAR-120 and
// PAR-137, ADR 0005; the USB page is OS-USBRET, the PATTERN page OS-LIVEREC). Six pages, the button again goes to the next:
//
//   MIDI     the device settings of group 0 (model/params.cpp kDevice): CLOCK IN, CLOCK SYNC (PAR-119, described in the official manual; not verified) and the new CLOCK MODE (off / master / slave), MIDI IN and
//            MIDI OUT (DIN, USB or both), IN CHANNEL (all or 1..16) and OUT CHANNEL. Generated from the descriptors like the instrument page: up / down a row, the encoder edits
//            (Shift: coarse), Delete puts the default back, FX1 + FX2 binds a controller. The new ones drive the MIDI link (OS-013a: seq::sync_config_from_settings; CLOCK MODE,
//            MIDI OUT and OUT CHANNEL at boot and when they change) and the control-change input of MIDI Learn (MIDI IN and IN CHANNEL, App::poll_midi_cc).
//   DISPLAY  group 1: DISPLAY brightness (the FT812 backlight: Low 20, Medium 50, High 128 of 128) and PAD LIGHTS brightness (the pad LEDs at 1/3, 2/3, 3/3 of their levels), NUMBERING (PAR-120: from 0,
//            from 1, hex: the Pattern view's row numbers) and MIDDLE C (PAR-084: the name of MIDI note 60, C-3..C-6; C-5 is what note names that start at C-0 give for note 60, the default: the Pattern view's note names and the note popup follow), and THEME (Amiga, Black, White: the screen redraws in it at once;
//            Shift + Master still cycles it, and the setting follows).
//   SD       what the card layer knows (hal/storage.h VolumeInfo): present, mounted, the file system, its size and cluster. The volume label and the free space are not shown: no call of the
//            card layer gives them cheaply (FatFs f_getfree scans the whole FAT, seconds on a big card), and a page must not stall the main loop (OS-CFGWIRE dropped the two rows).
//   INFO     the product and the build id the platform tells the App (PAR-116: build number), the free sample memory, who made it (CREATED BY trak3r-os, AUTHORS), and the licences (ADR 0005: the core
//            GPL-3.0-or-later, the module SDK MIT, and the third-party code the image carries: Spleen, Terminus, FatFs).
//   USB      group 2, three rows (+ CARD AS DISK and READ ONLY, OS-USBDISK U2b). USB AUDIO (On, the default, or Off; OS-USBAUDIO, new Community OS behaviour, maintainer decision): the unit comes up as the USB audio device (the stems stream and
//            the return, plus MIDI and the serial console) and enumerates once, in that mode, at boot; Off is STANDARD (MIDI and serial). USB RETURN (On, the default, or Off) decides whether
//            the AUDIO mode carries the stereo return stream (the DAW playing into the Tracker, "2 IN"): Off leaves capture only ("10 OUT" with profile 10), which a host behind a hub with a
//            transaction translator can always open; On with profile 8 is "8 OUT / 2 IN", the duplex layout that opens on every host measured (ADR 0012 amendment 5). RETURN LEVEL (OS-006e)
//            is live and learnable. USB AUDIO and USB RETURN re-enumerate the port, so they are press to apply (kParamApply): the encoder (or Delete) only changes a pending value, shown
//            starred in the accent colour; F7 (APPLY) says "USB WILL RECONNECT: PRESS AGAIN" (when it will: not for a lone USB RETURN change in STANDARD) and the second press writes the settings (one re-enumeration for both); a move of the cursor
//            takes back the first press, and leaving the page (another page, another screen) throws the pending values away. The mode at runtime is the `usb mode` console command's, which
//            does not touch the setting; the profile and the stem map are the project's USB part (the setting decides the mode, the project the profile). Not learnable.
//   PATTERN  group 3: the live recording of the Pattern screen (OS-LIVEREC). REC POLY (Off, the default: a recorded note goes to the cursor's track only; Spill: notes held together go to that
//            track, then the next ones, one a voice) and REC QUANTIZE (Nearest step, the default: a note lands on the step nearest to when it was played; Playing step: on the step the
//            playhead is on). Learnable. Since OS-METRO (new Community OS behaviour, docs/specs/OS-METRO.md) three more rows, in this order: METRO (Off, Cue, Main; the default is Cue, which plays on
//            MAIN while OUT MODE is Stereo and CUE has no output: the row then reads "Cue (-> Main)"), METRO VOL (a row the official manual describes, docs/ui/official-manual-map.md; moved here from the MIDI page where it was dead) and COUNT-IN (Off, 1 bar, 2 bars:
//            the bars the transport counts in on row 1 before a recording that starts from stop, the pattern silent; then it starts at row 1). All three learnable. Since OS-RECMICRO (new Community OS behaviour, PAR-037 / PAR-064) two rows after REC QUANTIZE, so seven in all: REC MICRO (Off, the default; On: a note recorded late in its step also writes its offset as a
//            Micro-move in FX 2, when that column is free) and REC VELOCITY (Off; External: the velocity of a MIDI keyboard note goes to the volume column). Both learnable.
//            Since OS-PADOFFSET (new Community OS behaviour) an eighth and last row, PAD OFFSET (0..12, the default 12): where each row of the NOTES pads starts on the row below it (12: right after it, the
//            layout before the row; N: on its Nth note, so the rows overlap by 12 - N). The pads follow at once (App::poll_pad_offset). Learnable. Root, scale and octave stay the held Note panel's.
//
// The settings are saved to the card (Settings/device.o3st, OS-007n); the header says so.
class ConfigScreen final : public Screen {
 public:
  constexpr ConfigScreen() noexcept = default;
  void enter(const Model& model, ViewState& view) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::Config; }
  Context context() const noexcept override { return Context::Config; }
  unsigned pages() const noexcept override { return 6; }
  const char* const* soft_keys() const noexcept override { return kConfigSoftKeys; }
  const char* const* soft_keys_shifted() const noexcept override { return kConfigShiftKeys; }
  core::ParamId focus_param(const Model& model, const ViewState& view) const noexcept override;
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override;
  int page() const noexcept override { return ParamView::kMaxRows; }
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  // OS-CFG-ARROWS: Up / Down move the row; Left / Right edit it (param.edit, bound in Config's context) and never turn the page (the F-keys and the screen button do).
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
};
extern const ConfigScreen kConfigScreen;

}  // namespace o3os::app
