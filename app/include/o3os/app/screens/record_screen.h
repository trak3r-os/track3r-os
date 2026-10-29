// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/screens/screen.h>

namespace o3os::app {

class Recorder;
class SampleBank;

// F1..F8 of the Record screen (at most 8 characters): record or stop, the monitor, save to an instrument, throw the take away, the radio's seek, and the WAV that needs the card's
// write layer.
inline constexpr const char* kRecordSoftKeys[8] = {"REC/STOP", "MONITOR", "SAVE", "DISCARD", "SEEK-", "SEEK+", "SAVE WAV", "PREVIEW"};
// OS-LOOPER: under the review of a loop F1 is the overdub (L2c: drawn in force while a session is on), F5 puts the last layer back (drawn dim while there is none), F6 does nothing ("-") and F8 is the loop's switch, named by
// its state; the rest is as above.
inline constexpr const char* kRecordLoopKeys[8] = {"OVERDUB", "MONITOR", "SAVE", "DISCARD", "UNDO LYR", "-", "SAVE WAV", "LOOP ON"};
inline constexpr const char* kRecordLoopMutedKeys[8] = {"OVERDUB", "MONITOR", "SAVE", "DISCARD", "UNDO LYR", "-", "SAVE WAV", "MUTED"};
// While Shift is held (OS-SHIFTROW): on the RADIO page F5 / F6 are CH-10 / CH+10 (record.seek with a 10x argument: tune ten channels); the rest, and every key of the SOURCES and RESAMPLE
// pages, does nothing but F3 (below). Shift + Left / Right are the coarse step (OS-CFG-ARROWS).
// OS-RECCROP: Shift + F3 is CROP on every page: the take under review becomes its window (it asks first; with no take it says so).
inline constexpr const char* kRecordShiftKeys[8] = {"-", "-", "CROP", "-", "-", "-", "-", "-"};
inline constexpr const char* kRecordRadioShiftKeys[8] = {"-", "-", "CROP", "-", "CH-10", "CH+10", "-", "-"};

// "Record" (docs/design/product-ux.md section 4: "Sources (line L/R, mic, radio, USB); pages RESAMPLE, RADIO", the Sample recorder; OS-011c, new Community
// OS behaviour). The screen is the face of app::Recorder (recorder.h has the rules): three pages, the button
// again goes to the next.
//
//   SOURCES   SOURCE (line, line L, line R, mic low / high gain range, radio, USB), GAIN of that source (one gain is kept for each of line, mic low, mic high and radio),
//             MONITOR (the input through the engine into MAIN and CUE: off by default, the playback goes on either way), ALIGN (where a take starts while the pattern plays:
//             FREE, STEP, BAR, PATTERN; FREE while it is stopped), STOP AT (OS-REC2, new OS behaviour: MANUAL, or F1 ends the take at the next STEP / BAR / PATTERN start of the
//             sequencer's timeline, or a take ends by itself after 1..16 BARS; MANUAL while the transport is stopped; recorder.h RecStop).
//   RESAMPLE  TAP (what the engine plays: the MASTER, the REC BUS as the Routing page sets it, or one TRACK), ALIGN, STOP AT.
//   RADIO     FREQ (a channel a detent), REGION (Europe, USA, Australia, Japan: PAR-085), GAIN, ALIGN, STOP AT; the chip's state, signal and stereo flag, and F5 / F6 seek.
//
// Under review the 48 pads play the take chromatically (OS-REC2, new Community OS behaviour: the pads play the take, in a fixed chromatic layout; the layout is record_view.h's rec_pad_note: the NOTES layout, C-4 = the take at its own pitch on pad 12).
//
// F1 arms the take (it starts at once or waits for the step ALIGN says, to the sample) and stops it; the take is drawn as it records (an overview, take_overview.h). When it ends
// it is under REVIEW: TRIM START and TRIM END appear as rows, F3 SAVE puts the window into the first free instrument slot through the sample bank's handover, and F4 throws it away. Normalising and the rest are the Sample edit screen's, on the instrument. F7
// (a WAV file on the card) says what it needs: the card layer is read only.
//
// Up / down moves the row, the encoder changes it (Shift: coarse), Delete puts the row's default back, the Rec button is F1.
class RecordScreen final : public Screen {
 public:
  constexpr RecordScreen() noexcept = default;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::Record; }
  Context context() const noexcept override { return Context::Record; }
  unsigned pages() const noexcept override { return 3; }
  const char* const* soft_keys() const noexcept override { return kRecordSoftKeys; }
  const char* const* soft_keys_shifted() const noexcept override { return kRecordShiftKeys; }
  const char* const* shift_row(const ViewState& view) const noexcept override;
  Context field_context(const Model& model, const ViewState& view) const noexcept override;
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override;
  int page() const noexcept override { return 6; }
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  // OS-CFG-ARROWS: Up / Down move the row; Left / Right edit it (record.edit, bound in Record's context) and never turn the page (the screen button does).
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
  // OS-REC2: under review the 48 pads play the take, so they are all lit (Back), the one at the take's own pitch (C-4, record_view.h) brighter (Front); no opinion otherwise.
  bool pad_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) const noexcept override;
};
extern const RecordScreen kRecordScreen;

// OS-AUDIT-70 (E-SREC-06, new Community OS behaviour): the time a take can still run, in frames, as the card shows it ("TIME LEFT"): the free arena before a take (a plain take reserves all
// of it), the take's capacity minus its frames while it is armed or recording. False under review, with no recorder or no bank, and for a loop (its length is the bars').
bool record_room_frames(const Recorder* rec, const SampleBank* bank, std::uint32_t& frames) noexcept;

}  // namespace o3os::app
