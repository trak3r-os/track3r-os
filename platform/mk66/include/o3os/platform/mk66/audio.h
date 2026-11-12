// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66's audio path (OS-005a): the hal::audio::Device on I2S0, two eDMA channels and the SGTL5000, with the pure engine
// skeleton (audio) behind it. Implemented in the MK66 image only (src/audio.cpp, src/i2c.cpp); the declarations compile
// everywhere so the header check passes on every platform.
//
// What runs where (ADR 0013 D1):
//   * eDMA channel 13 plays the TX ring into the I2S0 data register, channel 12 fills the RX ring; each major loop is one block
//     (64 stereo frames, 256 bytes). The TX channel interrupts at every block boundary: that ISR (priority kAudioDma, in SRAM_L)
//     numbers the boundary, checks that the block due now was rendered, and pends the render line.
//   * The render line is IRQ 64 (the K66's software interrupt, priority kRender, handler in SRAM_L): it calls the engine.
//   * start(), unmute() and mute() run in the main thread and block (the codec ramps).
// Nothing here is started by the reset sequence: main() calls start().

#include <o3os/audio/engine.h>
#include <o3os/audio/pipeline.h>
#include <o3os/audio/status.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>
#include <o3os/platform/mk66/sgtl5000.h>

#include <cstdint>

namespace o3os::platform::mk66 {

// The one audio device (an object in .bss, valid after the static constructors).
hal::audio::Device& audio_device() noexcept;
// OS-011c: writes the register steps of a source switch (sgtl5000.h InputPlan, composed by the caller with audio_device().outputs_enabled()) to the codec. Remembers the ADC's
// input select so that a later unmute() or mute() keeps it. False: the audio path is not started or a write was not acknowledged. Main thread only.
bool audio_input_apply(const sgtl5000::InputPlan& plan) noexcept;
// OS-POWER P1 (docs/specs/OS-POWER.md 4.1 A2, 4.6): the one stop of the audio path. The codec is muted click-free (the DAC through its own soft ramp, the amplifier key LOW, the outputs muted and the analogue
// stage powered down: sgtl5000.h run_stop), then the I2S transmitter and receiver, the two eDMA channels and the two interrupt lines are stopped; the path is "not started" afterwards. Main thread only;
// it blocks for about 70 ms (the caller keeps the display serviced). Plain when the path never started (the boot disk mode). The caller sets the power-off latch FIRST (appimg::power_off_latched):
// the block counter stands still from here, which the audio poll would call a stall, and nothing may reopen the outputs. Called by the shutdown only.
void audio_stop() noexcept;
// OS-MASTEREQ: one main-loop pass of the master EQ and bass boost (sgtl5000.h EqDriver): at most 3 codec writes, none while the outputs are closed or every value is at its default. The
// wanted values come from audio_device().set_output_eq(). Main thread only.
void audio_eq_pass(std::uint32_t now_ms) noexcept;
// The engine behind it: its ParamChange ring and the test tone switch.
o3os::audio::Engine& audio_engine() noexcept;
// The sampler voices (OS-005d lite): the pool the engine plays when the composition root attaches it (app_main.cpp), and the sample table the demo kit
// (and, later, the SD loader) fills. An object in SRAM_U .bss.
o3os::audio::VoicePool& audio_voices() noexcept;
// OS-005h: the Amiga voices (the instruments whose engine is Amiga); attached to the engine by the composition root.
o3os::audio::AmigaVoices& audio_amiga() noexcept;
// What the audio path publishes about itself (counters, stage cycles, jitter).
const o3os::audio::AudioStatus& audio_status() noexcept;
// OS-PERF3, `diag cpu reset`: zeroes the per-run figures (the maxima, the over-budget, late and fault counters: AudioStatus::reset_peaks); the fault LED's latch (StatusSnapshot::fault_seen) stays. Main thread only.
void audio_cpu_reset() noexcept;
// OS-MAINGUARD (audio/pipeline.h): arms the render's main-loop starvation guard, now and for every later start(); the main loop calls it once, before its loop.
// Off in every other image path (hosts, tests, a build whose loop does not call audio_main_pass). Main thread only.
void audio_arm_starve_guard() noexcept;
// The beat: the main loop calls it once per pass, and any long main-thread wait that measures the render (diag bench) per iteration, so those never count as a starved loop.
void audio_main_pass() noexcept;
// OS-MAINGUARD tuning, `guard n <1..255>`: the starvation window N (Pipeline::set_starve_blocks; clamped), taking effect at the render's next block. Main thread only.
void audio_set_starve_blocks(std::uint32_t n) noexcept;
// The pipeline itself, read-only: the guard's shed counters for `diag cpu`.
const o3os::audio::Pipeline& audio_pipeline() noexcept;
// OS-PERF3, `diag bench worst`: the stage figures of the slowest render since boot or the last reset (audio::Pipeline::worst_render).
o3os::audio::Pipeline::WorstRender audio_worst_render() noexcept;
// The codec bring-up's last error code for a probe or a later `diag`: 0 none, else a hal::audio::Device::Result.
std::uint8_t audio_last_result() noexcept;

// Where the last start() stopped on a ClockFault (OS-HW1): which wait ran out and the raw I2S0 MCR and MDR at that moment. `where` is
// None for every other outcome. Written by start() in the main thread; read by the main thread too.
struct AudioClockFault {
  enum Where : std::uint8_t { None, McrWait, MdrWait, NoBoundaries };
  std::uint8_t where = None;
  std::uint32_t mcr = 0;
  std::uint32_t mdr = 0;
};
AudioClockFault audio_clock_fault() noexcept;
// true once start() has succeeded (the block clock runs); start() may be called again until then (OS-HW1 `diag audio start`).
bool audio_running() noexcept;
// The cycle counter value at which the first sample of the block the render is working on is HEARD: the stamp of the slot boundary that started this render plus the TX
// ring's render-ahead (midi_config.h kAudioLeadCycles). The MIDI timing core's time base (OS-005c2); render context only, valid inside the block callback.
std::uint32_t audio_block_heard_at() noexcept;

}  // namespace o3os::platform::mk66
