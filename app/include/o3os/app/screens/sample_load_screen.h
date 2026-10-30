// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/screens/screen.h>

namespace o3os::app {

// F1..F8 of the Sample load screen: at most 8 characters. F3 ENTER (enters a
// folder), F4 ADD (adds the file as the next instrument) and F5 PLAY (previews) are this screen's keys.
inline constexpr const char* kSampleLoadSoftKeys[8] = {"LOAD", "UP", "ENTER", "ADD", "PLAY", "RESCAN", "WT AUTO", "CH MIX"};  // F7: OS-GRAN, how a WAV becomes a wavetable (the label follows the mode: wavetable_key_label); F8: OS-LOADCH, the channel of a stereo file (CH MIX / CH LEFT / CH RIGHT / STEREO, channel_key_label; "CH MONO" while the file under the cursor is mono, "--" with no file)
// While Shift is held (OS-SHIFTROW): the quarter jumps and the end, then the target instrument one back / on (OS-SLDTARGET), bound in this screen's context; F6 does nothing.
inline constexpr const char* kSampleLoadShiftKeys[8] = {"0/4", "1/4", "2/4", "3/4", "END", "-", "INST -", "INST +"};

// "Sample load" (docs/design/product-ux.md section 4: "SD browser, preview, import, .pti library"; OS-011a, new Community OS behaviour after PAR-016, PAR-033 and
// BASE-17): the WAV files of the card, in folders. The listing is the File screen's browser (dir_index.h, dir_scan.h) with the .wav filter (kWavFilter), over the
// App's one DirScan: the two screens are never both scanning, and entering one while the other's listing is held starts its scan over. It starts at the card's
// root; folders come first. A name is what the card says it is: a folder called kick.wav is a folder (PAR-033).
//
//   Up / Down and the encoder (the Cursor)    Left / Right: the previous / next first letter (as the File screen)
//   a pad            the target instrument (1..48), as on PARAMS; Shift + F7 / F8 (INST - / INST +): one back / on, round (OS-SLDTARGET). The header reads TARGET INSTRUMENT nn.
//   F1 LOAD          the WAV under the cursor into the target instrument: the import job of OS-002c (WAV importer, the "IMPORT KICK.WAV 45%" strip, then the
//                    peak cache), the audio into the sample arena, the instrument made from the file; the engine takes it up when playback is stopped
//                    (sample_bank.h, THE HANDOVER). The card is read only: the change lives in memory until a later slice saves the project.
//   F2 UP / F3 ENTER the parent folder / the folder under the cursor     F4 ADD   as LOAD, into the first free instrument     F6 RESCAN   read the folder again
//   F7 WT            how a WAV is read as a wavetable (sample_load_view.h): AUTO (only a Serum-marked file), LEN (the frame size by the length: 2048, 1024, 256), 256 .. 2048, OFF; a .wt (Surge) is always one
//   F8 CH            OS-LOADCH: which channel of a STEREO file is loaded: MIX ((L + R) / 2, as always), LEFT, RIGHT or STEREO (the mix and the side signal, OS-STEREO (b)); the key shows "CH MIX" / "CH LEFT" / "CH RIGHT" / "STEREO" while the file under the cursor is stereo ("ST" in the
//                    list, from its header) and "--" otherwise. The audition follows it. The result is an ordinary mono sample. The choice stays for the session (WavView::channel), it is not saved.
//   F5 PLAY          the audition (OS-FU2, app/src/audition.cpp): while the key is held, the first seconds (4 s at 44.1 kHz, mono) of the file under the cursor play through the
//                    engine's live notes, on a descriptor that is no instrument's (sample_bank.h, "The audition"); the release, a cursor move or leaving the screen stops it
//
// The card on the right is the target instrument: name, length, rate, and its waveform once it is loaded; and the free room of the sample arena. A waveform
// preview of the file under the cursor is not here: it would read the whole file for each cursor stop.
class SampleLoadScreen final : public Screen {
 public:
  constexpr SampleLoadScreen() noexcept = default;
  void enter(const Model& model, ViewState& view) const noexcept override;
  void leave(const Model&, ViewState& view) const noexcept override {
    view.wav.back_to_params = false;
    if (view.wav.pti) {  // OS-INSTUI S7: the instrument list was a visit; the sample browser starts at the card's root again
      view.wav.pti = false;
      view.wav.folder[0] = '\0';
    }
  }  // OS-INSTUI S5: opened by the type chooser, a screen left some other way stays
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::SampleLoad; }
  Context context() const noexcept override { return Context::SampleLoad; }
  const char* const* soft_keys() const noexcept override { return kSampleLoadSoftKeys; }
  const char* const* soft_keys_shifted() const noexcept override { return kSampleLoadShiftKeys; }
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override { return view.wav.list.pos(); }
  int page() const noexcept override { return FileView::kVisibleRows; }
  bool accelerates() const noexcept override { return true; }  // OS-AUDIT-28
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
};
extern const SampleLoadScreen kSampleLoadScreen;

// The cursor and the window back inside the listing in view (the App calls it when the listing moves). True if the view moved.
bool sample_load_sync(const Model& model, ViewState& view) noexcept;

}  // namespace o3os::app
