// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Record screen's cursor (OS-011c): which row of the page the up / down arrows are on, and the rows of each page. Pure; the screen draws them, the actions edit them.
//
//   SOURCES   SOURCE, GAIN, MONITOR, ALIGN, STOP AT, LOOP, COUNT-IN  (+ TRIM START, TRIM END while a take is under review)
//   RESAMPLE  TAP, ALIGN, STOP AT, LOOP, COUNT-IN                    (+ the trim rows)
//   RADIO     FREQ, REGION, GAIN, ALIGN, STOP AT, LOOP, COUNT-IN     (+ the trim rows)
// OS-SREC-LEVEL: with START ON = LEVEL (no LOOP, no take under review) THRESHOLD and PRE-ROLL follow ALIGN; a page then has nine rows at most, as it has under review.
// OS-LOOPER L1: LOOP (OFF, 1..16 BARS) is the row before COUNT-IN; under the review of a LOOP the trim rows give way to LOOP VOL (FEEDBACK, the overdub's, joins it in L2).
// OS-RECCOUNT: COUNT-IN (OFF, 1, 2, 4 BARS) is the last row of the page, right after LOOP: the bars a bar-aligned take counts in before it records (recorder.h).

#include <cstdint>

namespace o3os::app {

enum class RecMode : std::uint8_t { Sources = 0, Resample, Radio };  // the Record screen's three pages

enum class RecRow : std::uint8_t { Source, Gain, Monitor, Align, Tap, Freq, Region, TrimStart, TrimEnd, StopAt, Loop, LoopVol, Feedback, Threshold, PreRoll, CountIn };
inline constexpr unsigned kRecMaxRows = 9;

struct RecRows {
  RecRow row[kRecMaxRows] = {};
  unsigned n = 0;
};

// `level`: START ON is LEVEL and LOOP is off (the Recorder's cfg; level_rows() in recorder.h).
constexpr RecRows rec_rows(RecMode mode, bool review, bool loop_review = false, bool level = false) noexcept {
  RecRows r;
  const bool level_rows = level && !review;  // (a take under review shows its trim rows: there is no room for eleven)
  switch (mode) {
    case RecMode::Sources:
      r.row[r.n++] = RecRow::Source;
      r.row[r.n++] = RecRow::Gain;
      r.row[r.n++] = RecRow::Monitor;
      r.row[r.n++] = RecRow::Align;
      if (level_rows) {
        r.row[r.n++] = RecRow::Threshold;
        r.row[r.n++] = RecRow::PreRoll;
      }
      r.row[r.n++] = RecRow::StopAt;
      r.row[r.n++] = RecRow::Loop;
      r.row[r.n++] = RecRow::CountIn;  // OS-RECCOUNT: right after LOOP
      break;
    case RecMode::Resample:
      r.row[r.n++] = RecRow::Tap;
      r.row[r.n++] = RecRow::Align;
      if (level_rows) {
        r.row[r.n++] = RecRow::Threshold;
        r.row[r.n++] = RecRow::PreRoll;
      }
      r.row[r.n++] = RecRow::StopAt;
      r.row[r.n++] = RecRow::Loop;
      r.row[r.n++] = RecRow::CountIn;
      break;
    case RecMode::Radio:
      r.row[r.n++] = RecRow::Freq;
      r.row[r.n++] = RecRow::Region;
      r.row[r.n++] = RecRow::Gain;
      r.row[r.n++] = RecRow::Align;
      if (level_rows) {
        r.row[r.n++] = RecRow::Threshold;
        r.row[r.n++] = RecRow::PreRoll;
      }
      r.row[r.n++] = RecRow::StopAt;
      r.row[r.n++] = RecRow::Loop;
      r.row[r.n++] = RecRow::CountIn;
      break;
  }
  if (review && loop_review) {
    r.row[r.n++] = RecRow::LoopVol;
    r.row[r.n++] = RecRow::Feedback;  // OS-LOOPER L2c: what the layer keeps of the loop under it
  } else if (review) {
    r.row[r.n++] = RecRow::TrimStart;
    r.row[r.n++] = RecRow::TrimEnd;
  }
  return r;
}

struct RecordView {
  std::uint8_t row[3] = {0, 0, 0};  // the cursor of each page
};

// ---- The pads play the take (OS-REC2) ----
// NEW COMMUNITY OS BEHAVIOUR: the pads play the take too, while a take exists on the record screen: a note on starts it through the pad board over the
// trim window (start point .. end point), a note off ends it. The layout below is fixed and chromatic,
// and the voice is the engine's preview descriptor.
// While a take is under review the 48 pads play it chromatically. The layout is the NOTES layout of the Pattern screen at its defaults (PadBoard: scale 0, offset 12, root 36), fixed here
// so that the take is playable the same whatever scale or offset the Pattern pads were set to: four rows of twelve, pad 0 at the top left carries the highest notes and the bottom row the
// lowest, a row continuing the one below it by an octave. The take sounds at its own pitch on C-4 (note 60 = the engine's kRootNote), which is the first pad of the second row from
// the top (pad 12); the range is C-2 (the bottom left pad) .. B-5 (the top right one).
inline constexpr unsigned kRecPads = 48;
inline constexpr unsigned kRecPadCols = 12;
inline constexpr unsigned kRecPadLowest = 36;  // C-2: the bottom left pad
inline constexpr unsigned kRecRootPad = 12;    // the pad of note 60: the take at its own pitch

constexpr unsigned rec_pad_note(unsigned pad) noexcept {
  return pad < kRecPads ? kRecPadLowest + kRecPadCols * (3u - pad / kRecPadCols) + pad % kRecPadCols : 60u;
}
static_assert(rec_pad_note(kRecRootPad) == 60u && rec_pad_note(0) == 72u && rec_pad_note(36) == 36u && rec_pad_note(47) == 47u, "the NOTES layout at its defaults");

// The row under the cursor of a page, clamped to the rows it has now (a take under review adds two).
constexpr unsigned rec_cursor(const RecordView& v, RecMode mode, bool review, bool loop_review = false, bool level = false) noexcept {
  const unsigned n = rec_rows(mode, review, loop_review, level).n;
  const unsigned page = static_cast<unsigned>(mode);
  return v.row[page] < n ? v.row[page] : n - 1u;
}

}  // namespace o3os::app
