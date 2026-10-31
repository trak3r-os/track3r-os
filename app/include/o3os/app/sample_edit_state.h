// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/sample_dsp.h>

#include <cstdint>

namespace o3os::app {

// The Sample edit screen's state (OS-011a): the window on the sample, the selection (in frames; none when a == b: the effects then act on the whole sample), the
// edge the encoder moves, Amplify's percentage, and the hand-offs of an edit (the action asks, the App's pass runs the job, and its result waits here for the internal
// action sample.commit, which is an undo unit of the project and the step of the bank's history).
// The parameters of the FX page (OS-AUDIT-15, new Community OS behaviour: FILTER and COMPRESSOR). View state only, not saved with anything: they keep their values while the OS runs.
// `log`: a step is a sixteenth of the value (a quarter with Shift), at least 1; else `step` / `coarse` as they are.
enum FxParam : std::uint8_t { kFxType, kFxCutoff, kFxQ, kFxThreshold, kFxRatio, kFxAttack, kFxRelease, kFxMakeup, kFxDetector, kFxCount };
struct FxParamDef {
  const char* name;
  std::int16_t min;
  std::int16_t max;
  std::int16_t def;
  std::int16_t step;
  std::int16_t coarse;
  bool log;
};
inline constexpr FxParamDef kFxParams[kFxCount] = {
    {"TYPE", 0, 2, 0, 1, 1, false},                // LOW PASS, HIGH PASS, BAND PASS
    {"CUTOFF", 20, 20000, 1000, 1, 1, true},       // Hz (held to 0.45 of the sample's rate when run)
    {"RESONANCE", 5, 150, 7, 1, 10, false},        // Q times ten
    {"THRESHOLD", -60, 0, -18, 1, 6, false},       // dB
    {"RATIO", 11, 200, 40, 1, 10, false},          // times ten, to 1
    {"ATTACK", 1, 1000, 100, 1, 1, true},          // ms times ten
    {"RELEASE", 5, 2000, 100, 1, 1, true},         // ms
    {"MAKEUP", -1, 24, -1, 1, 3, false},           // dB, -1 is AUTO
    {"DETECTOR", 0, 1, 0, 1, 1, false},            // PEAK, RMS
};

struct SampleEditState {
  static constexpr std::uint16_t kAmpMin = 10;
  static constexpr std::uint16_t kAmpMax = 800;

  std::int16_t fx[kFxCount] = {0, 1000, 7, -18, 40, 100, 100, -1, 0};  // the FX page's values, in kFxParams' units
  std::uint8_t fx_sel = 0;  // the FX page's row

  std::uint32_t first = 0;  // the window: `span` frames from `first`; 0 is the whole sample
  std::uint32_t span = 0;
  std::uint32_t a = 0;      // the selection [a, b)
  std::uint32_t b = 0;
  std::uint8_t edge = 0;    // 0: the encoder moves a, 1: it moves b
  std::uint16_t amp = 200;  // Amplify's percentage
  bool go = false;          // an effect was asked for: the App starts the job
  bool running = false;     // OS-SEDIT-CANCEL: that job runs (set and cleared by the App, main loop only): F8 reads CANCEL then
  bool cancel = false;      // F8 asked to stop it: the App cancels the job (nothing is committed)
  EditOp op = EditOp::Amplify;
  std::uint16_t period = 0;  // Amiga (OS-AMG1): the Paula period asked for, 428 (8,287 Hz) or 214 (16,574 Hz)
  bool q8 = false;           // Amiga: also round to 8-bit levels
  bool ready = false;       // the job is done: the result waits in `staged` for sample.commit
  EditOp ready_op = EditOp::Amplify;
  std::uint16_t ready_period = 0;
  std::uint8_t ready_slot = 0;
  bool ready_stereo = false;  // OS-STEREO (d): the staged buffer is M then S (S at staged + even(staged_frames)): sample.commit swaps both in
  std::uint32_t ready_a = 0;  // the range it was made over
  std::uint32_t ready_b = 0;
  std::int16_t* staged = nullptr;
  std::uint32_t staged_frames = 0;
};

}  // namespace o3os::app
