// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The status LED language of the skeleton image: the only output it has (no display, no console yet). One pure function
// says whether the LED is on at a given moment of a pattern, so the patterns are tested and the boot code only plays
// them. The codes the maintainer reads during the first hardware session are in README.md and mirror the constants below.
//
//   heartbeat (pulses == 0)  100 ms on, 900 ms off, forever: the main loop is alive.
//   n pulses (n >= 1)        n flashes of 200 ms on / 200 ms off, then a pause that brings the period to
//                            n * 400 + 1600 ms, repeated: a code, read by counting the flashes.
//   long flash               one 1000 ms on, played once between stages.

#include <cstdint>

namespace o3os::platform::mk66::blink {

// Failure codes: the image stops and repeats the code forever (nothing is left running). The exception is the audio failure (10), which since
// OS-006b is latched like the display's codes instead: the console must come up to say what failed.
inline constexpr std::uint8_t kClockFailure = 2;  // crystal did not start, HSRUN not entered, or the PLL did not lock
inline constexpr std::uint8_t kSdramFailure = 3;  // the SDRAM wiring or full-capacity test failed
// Boot reports, played once before the heartbeat starts.
inline constexpr std::uint8_t kDisplayFailure = 4;  // the FT812 did not come up (no chip answer, a stuck reset, no scan-out): headless
inline constexpr std::uint8_t kDisplayPolled = 5;   // the display works but its DMA path failed the self-test: everything goes by the slow path
inline constexpr std::uint8_t kInputDegraded = 13;  // OS-007d: a key scanner did not answer: the control buttons' at boot (played before the audio start), the pads' after the pad bus opened (OS-BUG-PADS: a second after the board's rail and the audio start; three refused tries, shown once from the loop); the OS runs with what did (INPUT TEST says which)
inline constexpr std::uint8_t kCrashSeen = 6;     // a valid crash record from an earlier run (an unexpected crash)
inline constexpr std::uint8_t kDrillRoundTrip = 7;  // the record is the deliberate fault of the drill build, fields as expected

// Safe-state codes (OS-007c): a fault found while running. The heartbeat is replaced by the code for good (the OS keeps running,
// the display is held dark): a failure that is shown over and over cannot be mistaken for a quiet black screen.
inline constexpr std::uint8_t kDisplayStalled = 8;  // the display stopped swapping frames and a re-initialisation did not bring it back
inline constexpr std::uint8_t kAppFailure = 9;      // the app could not start (its bitmaps did not go up): the display is held dark

// Audio codes (OS-005a), numbered after the display's (4, 5, 8 and 9 are the display's: both slices share one LED).
inline constexpr std::uint8_t kAudioFailure = 10;  // the audio bring-up failed (codec absent or not answering, MCLK divider, no I2S boundaries); still muted. Since OS-006b the image goes on (USB and `diag` come up, `diag stage` lists it) and the code replaces the heartbeat for good, as the display's do
// Run-time audio verdict, sticky: the heartbeat becomes this pattern for the rest of the run, so "heartbeat after ten minutes"
// is the HW-A pass for the counters (the numbers themselves are read from the status, README.md).
inline constexpr std::uint8_t kAudioFault = 11;       // an underrun, a late render, a missed boundary or an I2S/DMA error since boot
inline constexpr std::uint8_t kAudioOverBudget = 12;  // no fault, but a render exceeded the load governor's 60 % of the block

inline constexpr std::uint32_t kPulseOnMs = 200;
inline constexpr std::uint32_t kPulseOffMs = 200;
inline constexpr std::uint32_t kPauseMs = 1600;
inline constexpr std::uint32_t kHeartbeatOnMs = 100;
inline constexpr std::uint32_t kHeartbeatPeriodMs = 1000;
inline constexpr std::uint32_t kLongFlashMs = 1000;

constexpr std::uint32_t period_ms(std::uint8_t pulses) noexcept {
  return pulses == 0 ? kHeartbeatPeriodMs : pulses * (kPulseOnMs + kPulseOffMs) + kPauseMs;
}

// The LED level `ms` milliseconds after the pattern began.
constexpr bool led_on(std::uint8_t pulses, std::uint32_t ms) noexcept {
  const std::uint32_t t = ms % period_ms(pulses);
  if (pulses == 0) {
    return t < kHeartbeatOnMs;
  }
  if (t >= pulses * (kPulseOnMs + kPulseOffMs)) {
    return false;  // the pause
  }
  return t % (kPulseOnMs + kPulseOffMs) < kPulseOnMs;
}

// OS-STATUSLED: the LED level with the heartbeat option in force: with the heartbeat off (`heartbeat_on` false) the heartbeat
// (pattern 0) and the over-budget code (kAudioOverBudget, 12: no fault, a render over the load governor's share) keep the LED dark,
// while a fault code (any other pulses >= 1, the sticky codes that replace the heartbeat) still shows. With the heartbeat on nothing
// changes: code 12 keeps blinking as before.
constexpr bool led_on_status(std::uint8_t pulses, std::uint32_t ms, bool heartbeat_on) noexcept {
  if (!heartbeat_on && (pulses == 0u || pulses == kAudioOverBudget)) {
    return false;
  }
  return led_on(pulses, ms);
}

// OS-LEDBPM: the STATUS LED setting (model::device_setting::StatusLed; the values are the setting's, 0 Blink, 1 BPM, 2 Off).
enum class LedMode : std::uint8_t { Blink = 0, Bpm = 1, Off = 2 };

// The LED level in a mode. `beat_on` is BeatLed's level (the beat flash). Blink: the heartbeat as today; Off: led_on_status with the heartbeat off; BPM: the beat flash in place of the heartbeat
// (pulses 0) and of the over-budget code 12 (no fault). Every other code (a fault: shown_code >= 1) keeps blinking in every mode.
constexpr bool led_on_mode(LedMode mode, std::uint8_t pulses, std::uint32_t ms, bool beat_on) noexcept {
  if (mode == LedMode::Bpm && (pulses == 0u || pulses == kAudioOverBudget)) {
    return beat_on;
  }
  return led_on_status(pulses, ms, mode != LedMode::Off);
}

// OS-LEDBPM: the beat flash. A pure state machine, fed once per main-loop pass with one read of the sequencer's PlayPosition (audio/play_position.h, relaxed atomics, the render context its single
// writer: this runs in the main loop, takes no lock, allocates nothing and is nowhere near the audio path) and the millisecond clock.
//   Playing: the beat is the step with `step % 4 == 0` (4 steps per beat, seq::kStepsPerBeat; a pattern's step 0 is always a beat), so the flash lands on the step that starts each beat and
//   follows the sequencer: under an external MIDI clock or MTC the play position already follows it, so the LED does too. It fires on the first sample that sees a new step with `step % 4 == 0`,
//   or a new beat (a beat step the main loop was too slow to see: the flash is late, not lost); a play start in the middle of a bar flashes at the next beat step. During a count-in (`step` stays
//   0) the steps are numbered from the end (cpos = 512 - count_in), so the count-in beats fall on `count_in % 4 == 0` and the first step of the song flashes as a beat.
//   Stopped (or no position published yet): free-run, one flash per beat at the project tempo (period 6000000 / tempo_x100 ms, 120 BPM while no tempo is known); a tempo change applies from the
//   next flash; the first flash is immediate, at a stop and at power-up. Unsigned 32-bit time differences throughout (the ms clock wraps).
inline constexpr std::uint32_t kBeatFlashMs = 40;
inline constexpr std::uint32_t kStepsPerBeat = 4;
inline constexpr std::uint32_t kDefaultTempoX100 = 12000;
inline constexpr std::uint16_t kNoBeatPos = 0xFFFFu;

struct BeatSample {
  bool playing = false;
  std::uint16_t step = 0;        // PlayPosition::Value::step (0..127)
  std::uint8_t ustep = 0;        // its ustep (0..47)
  std::uint8_t count_in = 0;     // its count_in (steps left of the count-in, 0: none)
  std::uint32_t tempo_x100 = 0;  // its tempo_x100 (0: not known)
};

constexpr std::uint32_t beat_period_ms(std::uint32_t tempo_x100) noexcept {
  const std::uint32_t t = tempo_x100 == 0u ? kDefaultTempoX100 : (tempo_x100 < 1000u ? 1000u : (tempo_x100 > 99900u ? 99900u : tempo_x100));
  return 6000000u / t;
}

struct BeatLed {
  // Written and read by the main loop only (no sharing). `flashes` and `last_pos` are what `diag status` prints: the number of flashes since boot and the play step of the last one
  // (kNoBeatPos: a free-run flash; 512 - count_in for a count-in step).
  std::uint32_t flashes = 0;
  std::uint16_t last_pos = kNoBeatPos;

  // One pass: the level the LED has now.
  constexpr bool update(const BeatSample& s, std::uint32_t ms) noexcept {
    bool flash = false;
    std::uint16_t pos = kNoBeatPos;
    if (!s.playing) {
      const std::uint32_t period = beat_period_ms(s.tempo_x100);
      if (was_playing_ || !started_) {
        flash = true;  // power-up, or a stop: the free-run starts with a flash
        beat_ms_ = ms;
      } else if (ms - beat_ms_ >= period) {
        beat_ms_ += period;
        if (ms - beat_ms_ >= period) {
          beat_ms_ = ms;  // late by more than a beat (a long pass): resynchronise instead of flashing in a burst
        }
        flash = true;
      }
      was_playing_ = false;
      prev_pos_ = kNoBeatPos;
    } else {
      pos = s.count_in != 0u ? static_cast<std::uint16_t>(512u - s.count_in) : s.step;
      if (!was_playing_ || prev_pos_ == kNoBeatPos) {
        flash = pos % kStepsPerBeat == 0u;  // a start: only a beat step flashes
      } else {
        const bool new_step = pos != prev_pos_ || s.ustep < prev_ustep_;  // (the ustep part: a pattern of one or two steps loops on the same position)
        flash = new_step && (pos % kStepsPerBeat == 0u || pos / kStepsPerBeat != prev_pos_ / kStepsPerBeat);
      }
      was_playing_ = true;
      prev_pos_ = pos;
      prev_ustep_ = s.ustep;
    }
    started_ = true;
    if (flash) {
      flash_ms_ = ms;
      lit_ = true;
      ++flashes;
      last_pos = pos;
    }
    if (lit_ && ms - flash_ms_ >= kBeatFlashMs) {
      lit_ = false;
    }
    return lit_;
  }

 private:
  std::uint32_t flash_ms_ = 0;  // when the lit flash began
  std::uint32_t beat_ms_ = 0;   // free-run: when the last beat was due
  std::uint16_t prev_pos_ = kNoBeatPos;
  std::uint8_t prev_ustep_ = 0;
  bool was_playing_ = false;
  bool started_ = false;
  bool lit_ = false;
};

}  // namespace o3os::platform::mk66::blink
