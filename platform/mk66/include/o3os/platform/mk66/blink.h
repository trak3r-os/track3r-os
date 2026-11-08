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

// OS-STATUSLED: whether the heartbeat (pattern 0) blinks at all — the Config > DISPLAY screen's STATUS LED row (BLINK / OFF) and the
// `diag status` console command. Not saved: BLINK (true) at every boot, so no settings file changes. Single-writer flag: the writers are
// the app (its Config row, copied to the flag by appimg::pass(), app_main.cpp) and the `diag status` command, both from the main loop
// thread only; the reader is main.cpp's main loop, which drives the LED. The heartbeat is polled from that loop, not from an ISR or a
// timer, so the volatile flag needs no critical section.
inline volatile bool g_heartbeat_on = true;

// OS-STATUSLED: the LED level with the STATUS LED option in force: with the heartbeat off (`heartbeat_on` false) the heartbeat
// (pattern 0) and the over-budget code (kAudioOverBudget, 12: no fault, a render over the load governor's share) keep the LED dark,
// while a fault code (any other pulses >= 1, the sticky codes that replace the heartbeat) still shows. With the heartbeat on nothing
// changes: code 12 keeps blinking as before.
constexpr bool led_on_status(std::uint8_t pulses, std::uint32_t ms, bool heartbeat_on) noexcept {
  if (!heartbeat_on && (pulses == 0u || pulses == kAudioOverBudget)) {
    return false;
  }
  return led_on(pulses, ms);
}

}  // namespace o3os::platform::mk66::blink
