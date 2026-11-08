// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The HW-A test signal and the one float-to-int16 conversion of the skeleton (ADR 0013 D2: voices are float32, converted
// once per block at the boundary; nothing converts per sample in between).
//
// The tone is a sine at a fixed frequency (kToneHz) from a phase accumulator. The increment is derived from the rate
// constant (cycles per sample, hal/audio.h), not from "44,100": a frequency counter on the line out then reads kToneHz
// times (true sample rate / rate constant), so the HW-A tone measures the MCLK divider directly (1000.000 Hz when the
// divider is what the constant says; 999.6 Hz if the 196/3125 retune were loaded under the 4080/1 constant).
//
// Phase. Inside a block the phase is a float in [0, 1) advanced per sample (the D2 convention). At each block start it is
// re-derived from an exact 32-bit integer accumulator (turns in Q32, wraps for free), so a float rounding error cannot
// accumulate across blocks: the long-term frequency is exact to the Q32 increment (1e-8 relative), and the host oracle can
// state the phase of any sample exactly.
//
// The sine is the Maclaurin series of sin(x) to x^9 on the folded range [-pi/2, pi/2] (the phase is folded into the first
// quadrant's neighbourhood first): the first omitted term is x^11/11! = 3.5e-6 at the edge, so the error is below 0.12 LSB
// at full scale (0.01 LSB at the 0.1 amplitude the tone uses), checked against double sin() on the host.

#include <o3os/audio/hot.h>
#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio {

// ---- Conversion ---------------------------------------------------------------------------------------------------------
// float in [-1, 1] to int16: scale by 32768, round half away from zero, saturate to [-32768, 32767]. NaN becomes +32767
// (the comparison form is deliberate: it is the branch NaN falls into). Defined in float arithmetic only, so the host and
// the Cortex-M4F give the same bits.
O3OS_AUDIO_INLINE std::int32_t q15_from_float(float x) noexcept {
  float v = x * 32768.0f;
  if (!(v < 32767.0f)) {
    v = 32767.0f;
  } else if (v < -32768.0f) {
    v = -32768.0f;
  }
  return static_cast<std::int32_t>(v + (v >= 0.0f ? 0.5f : -0.5f));
}

// OS-CEWTENV room: the four functions of this file (these two, generate and generate_noise below) are flash (O3OS_AUDIO_COLD), 694 B out of `.ramfunc_l`: the two conversions have no caller in the image
// (the tests use them), and the tone runs only in the O3OS_AUDIO_TEST image (or from a console test-source ParamChange). Mono float planar to interleaved stereo int16 with L = R, `frames` frames. `out` is 4-byte aligned (callers).
O3OS_AUDIO_COLD void float_to_q15_dup(const float* mono, std::int16_t* out, std::uint32_t frames) noexcept;
// Two float planes to interleaved stereo int16 (left first), `frames` frames. `out` is 4-byte aligned (callers).
O3OS_AUDIO_COLD void float_to_q15_stereo(const float* left, const float* right, std::int16_t* out, std::uint32_t frames) noexcept;

// ---- Tone -----------------------------------------------------------------------------------------------------------------
// The HW-A tone tells the channels apart (a swapped or mis-mapped pair must be visible on the bench): the LEFT channel (the first word
// of every I2S frame) plays 1 kHz alone for kToneLeftOnlyBlocks blocks (about 3 s), then the RIGHT channel joins with 1.5 kHz while the
// left goes on. Each channel has its own phase accumulator and its own fade; the right one runs silently from the start, so the
// whole script is a pure function of the block count.
inline constexpr std::uint32_t kToneHzLeft = 1000;
inline constexpr std::uint32_t kToneHzRight = 1500;
inline constexpr std::uint32_t kToneHz = kToneHzLeft;  // the name the single tone had
inline constexpr std::uint32_t kToneLeftOnlyBlocks = 2048;
inline constexpr float kToneAmplitude = 0.1f;          // 0.1 of full scale: -20 dBFS
inline constexpr std::uint32_t kToneRampSamples = 2205;  // 50 ms from silence to full tone and back: no click on or off

// Turns per sample in Q32 for a tone of `hz` at the given rate: hz x cycles-per-sample / cpu_hz, rounded to nearest, by long
// division (exact, no 128-bit intermediate). 0 if the tone would not be below the sample rate.
constexpr std::uint32_t tone_phase_inc_q32(std::uint32_t hz, hal::audio::CyclesPerSample r = hal::audio::kCyclesPerSample,
                                           std::uint32_t cpu_hz = hal::audio::kCpuHz) noexcept {
  const std::uint64_t num = static_cast<std::uint64_t>(hz) * r.num;
  const std::uint64_t den = static_cast<std::uint64_t>(r.den) * cpu_hz;
  if (num >= den) {
    return 0u;
  }
  std::uint64_t rem = num;
  std::uint32_t result = 0;
  for (int bit = 31; bit >= 0; --bit) {
    rem <<= 1;
    if (rem >= den) {
      rem -= den;
      result |= 1u << bit;
    }
  }
  if (rem * 2u >= den && result != 0xFFFFFFFFu) {
    ++result;
  }
  return result;
}

// sin(2 pi p) for p in [0, 1), float, see the header comment for the error.
O3OS_AUDIO_INLINE float sin_turns(float p) noexcept {
  float t = p;
  if (p > 0.75f) {
    t = p - 1.0f;
  } else if (p > 0.25f) {
    t = 0.5f - p;
  }
  const float x = t * 6.28318531f;
  const float x2 = x * x;
  // x (1 - x^2/3! + x^4/5! - x^6/7! + x^8/9!)
  return x * (1.0f + x2 * (-0.166666667f + x2 * (0.00833333333f + x2 * (-0.000198412698f + x2 * 2.75573192e-6f))));
}

class TestTone {
 public:
  // Not on: the gain is 0 and stays there until set_on(true). `phase_inc` is tone_phase_inc_q32().
  void configure(std::uint32_t phase_inc, float amplitude, std::uint32_t ramp_samples) noexcept;

  // Render context only (the one place that calls generate()). The flag is the target the gain ramps toward.
  void set_on(bool on) noexcept { on_ = on; }
  bool on() const noexcept { return on_; }
  // The gain is above zero (a tone is audible, or fading).
  bool active() const noexcept { return ramp_pos_ != 0u || on_; }
  // The ramp is an integer position (0 = silent .. ramp_samples = full), one step per sample toward the target, and the gain is
  // amplitude x position / ramp_samples, a single float multiply: no error builds up, so a test can state the gain of any
  // sample exactly.
  std::uint32_t ramp_pos() const noexcept { return ramp_pos_; }
  float gain() const noexcept { return gain_of(ramp_pos_); }
  std::uint32_t phase_q32() const noexcept { return phase_q32_; }
  void reset_gain() noexcept { ramp_pos_ = 0; }

  // `n` (at most kBlock) mono float samples of the tone, the gain ramping linearly toward its target, the phase advanced by
  // `n` samples whatever the gain. Writes exactly n samples.
  O3OS_AUDIO_COLD void generate(float* out, std::uint32_t n) noexcept;  // flash: once a block per strip whose test tone is audible (hot.h lists the exception)
  // The same, with the same fade and gain, but white noise instead of the sine: a xorshift32 `state` (never 0) read as a signed fraction in
  // [-1, 1). The phase accumulator is not used (the noise has none).
  O3OS_AUDIO_COLD void generate_noise(float* out, std::uint32_t n, std::uint32_t& state) noexcept;
  // Advances the phase by n samples without producing any (a silenced block keeps the tone on its timeline).
  void skip(std::uint32_t n) noexcept { phase_q32_ += n * phase_inc_q32_; }

 private:
  float gain_of(std::uint32_t pos) const noexcept { return pos >= ramp_samples_ ? amplitude_ : static_cast<float>(pos) * ramp_step_; }

  std::uint32_t phase_inc_q32_ = 0;
  std::uint32_t phase_q32_ = 0;     // turns, Q32: the exact phase of the next sample
  float phase_inc_ = 0.0f;          // the same increment as a float, for the in-block steps
  float amplitude_ = 0.0f;
  float ramp_step_ = 1.0f;          // amplitude / ramp_samples: the gain of one ramp position
  std::uint32_t ramp_samples_ = 1;
  std::uint32_t ramp_pos_ = 0;
  bool on_ = false;
};

}  // namespace o3os::audio
