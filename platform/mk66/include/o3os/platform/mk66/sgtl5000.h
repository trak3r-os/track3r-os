// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The SGTL5000 codec: its registers, the power-up sequence, the unmute and mute plans, as data. The sequence is pure so the
// hardware-safety properties (no pop, never loud at boot) are unit-tested on the host and on QEMU against a model of the
// registers; src/audio.cpp only sends the steps over I2C.
//
// Sources.
//   * Register map and bit meanings: the SGTL5000 data sheet (Freescale/NXP, the revision PJRC hosts at pjrc.com/teensy/
//     SGTL5000.pdf), tables 9-32.
//   * Power-up order and values: the data sheet's "Chip Powerup and Supply Configurations" (p. 26-27), which says "all outputs
//     (LINEOUT, HP_OUT, I2S_OUT) are muted by default on power up. To avoid any pops/clicks, the outputs should remain muted
//     during these chip configuration steps". The values fit this board (VDDIO = VDDA = 3.3 V, VDDD driven externally).
//   * Unmuting: the data sheet's "Volume and Mute Control" example: headphone volume to its minimum (0x7F7F), unmute, then
//     ramp the volume up in 0.5 dB steps; the DAC's own ramp (VOL_RAMP_EN) is "automatically handled by the chip".
//   * Clocking: Table 8, synchronous SYS_MCLK = 256, 384 or 512 x Fs for Fs = 44.1 kHz among others; the PLL path (any clock
//     from 8 to 27 MHz) is not used. I2C: at most 400 kHz; communication may start 8 MCLK cycles after the rails are up with
//     SYS_MCLK present, so the master clock runs before the first write.
//
// The plan is built for a quiet start, quieter and earlier than the data sheet's example requires (each is a register the data sheet
// documents):
//   1. Every output is muted and every volume at its floor FIRST, before the supplies are configured and long before the analog
//      outputs are powered.
//   2. The DAC stays muted at volume 0xFCFC (muted) with VOL_RAMP_EN = 1 until unmute().
//   3. The sequence ends with the line out AND the headphone still muted; unmute() opens them, later.
//   4. The wait for the VAG ramp is 500 ms: with SMALL_POP the ramp takes about 400 ms.
//   5. The part id is read first: a part that does not answer as 0xA0xx stops the bring-up.

#include <o3os/hal/audio.h>

#include <cstdint>
#include <cstring>

namespace o3os::platform::mk66::sgtl5000 {

inline constexpr std::uint8_t kPartId = 0xA0;  // CHIP_ID[15:8]

// ---- Registers (16-bit address, 16-bit value, big endian on the wire) ----------------------------------------------------
inline constexpr std::uint16_t kChipId = 0x0000;
inline constexpr std::uint16_t kDigPower = 0x0002;
inline constexpr std::uint16_t kClkCtrl = 0x0004;
inline constexpr std::uint16_t kI2sCtrl = 0x0006;
inline constexpr std::uint16_t kSssCtrl = 0x000A;
inline constexpr std::uint16_t kAdcDacCtrl = 0x000E;
inline constexpr std::uint16_t kDacVol = 0x0010;
inline constexpr std::uint16_t kAnaHpCtrl = 0x0022;
inline constexpr std::uint16_t kAnaAdcCtrl = 0x0020;
inline constexpr std::uint16_t kAnaCtrl = 0x0024;
inline constexpr std::uint16_t kLinregCtrl = 0x0026;
inline constexpr std::uint16_t kRefCtrl = 0x0028;
inline constexpr std::uint16_t kMicCtrl = 0x002A;
inline constexpr std::uint16_t kLineOutCtrl = 0x002C;
inline constexpr std::uint16_t kLineOutVol = 0x002E;
inline constexpr std::uint16_t kAnaPower = 0x0030;
inline constexpr std::uint16_t kShortCtrl = 0x003C;

// ---- Bits the safety checks look at ---------------------------------------------------------------------------------------
inline constexpr std::uint16_t kAnaCtrlMuteLineOut = 1u << 8;  // MUTE_LO
inline constexpr std::uint16_t kAnaCtrlMuteHeadphone = 1u << 4;  // MUTE_HP
inline constexpr std::uint16_t kAnaCtrlMuteAdc = 1u << 0;        // MUTE_ADC
inline constexpr std::uint16_t kAdcDacMuteBoth = 0x000C;          // DAC_MUTE_LEFT | DAC_MUTE_RIGHT
inline constexpr std::uint16_t kAdcDacVolRampEn = 1u << 9;
// ANA_POWER bits that make sound or start the output ramp: VAG (7), headphone (4), DAC (3), capless headphone (2), line out (0).
inline constexpr std::uint16_t kAnaPowerOutputs = (1u << 7) | (1u << 4) | (1u << 3) | (1u << 2) | (1u << 0);
inline constexpr std::uint16_t kDacVolMuted = 0xFC;  // CHIP_DAC_VOL fields >= 0xFC are muted; 0x3C is 0 dB, the loudest allowed
inline constexpr std::uint16_t kDacVolZeroDb = 0x3C;
inline constexpr std::uint8_t kHpVolMin = 0x7F;  // CHIP_ANA_HP_CTRL fields: 0x7F is -51.5 dB, 0x18 is 0 dB, 0x00 is +12 dB (0.5 dB per step)

// ---- CHIP_I2S_CTRL: the 32 Fs / 64 Fs question ----------------------------------------------------------------------------------
// 0x0130 = SCLKFREQ 1 (32 Fs), slave (MS 0), 16-bit data, I2S format: also the value PJRC's Audio library writes, proven on this board. The data sheet says of SCLKFREQ (CHIP_I2S_CTRL bit 8): "when in slave mode (MS=0)
// this field must be set appropriately to match SCLK input rate: 0x0 = 64Fs, 0x1 = 32Fs". The K66 drives BCLK at 64 Fs (two 32-bit words
// per frame, audio_config.h), so by the letter of the data sheet the value is 0x0030 (SCLKFREQ 0). 0x0130 works
// (the slave follows the clock it gets and reads the first 16 bits of each slot) and nothing proves 0x0030 does on this board, so
// the default stays the proven 0x0130 and the other value is an A/B item of the HW-A checklist (README, row E-1): build with
// -DO3OS_SGTL5000_I2S_CTRL=0x0030 (a cache variable of platform/mk66's CMakeLists) and compare noise floor, distortion and artefacts.
#ifndef O3OS_SGTL5000_I2S_CTRL
#define O3OS_SGTL5000_I2S_CTRL 0x0130
#endif
inline constexpr std::uint16_t kI2sCtrlValue = O3OS_SGTL5000_I2S_CTRL;
static_assert(kI2sCtrlValue == 0x0130 || kI2sCtrlValue == 0x0030, "CHIP_I2S_CTRL: the proven 32 Fs value or the data sheet's 64 Fs one, nothing else");
static_assert((kI2sCtrlValue & 0x0080u) == 0u && ((kI2sCtrlValue >> 4) & 3u) == 3u, "slave, 16-bit data, whichever SCLKFREQ");

// ---- The power-up plan ----------------------------------------------------------------------------------------------------
struct Step {
  std::uint16_t reg;
  std::uint16_t value;
  std::uint16_t wait_ms;  // settle time after the write
};

inline constexpr Step kPowerUp[] = {
    // Every output muted and every volume at its floor FIRST, before the supplies are touched and long before anything that can make
    // sound is powered: on a cold boot the chip is already like this (its reset state), after a reset in the middle of a run (the
    // codec keeps its registers, and may have been unmuted) this is what makes the following power-down silent.
    {kAnaCtrl, 0x0137, 0},      // MUTE_LO, MUTE_HP and MUTE_ADC set, zero-cross detectors on, ADC input = line in
    {kAnaHpCtrl, 0x7F7F, 0},    // headphone volume at its minimum, -51.5 dB
    {kAdcDacCtrl, 0x020C, 0},   // DAC muted, volume ramp enabled: any later change is a soft ramp
    {kDacVol, 0xFCFC, 0},       // DAC volume: muted
    // Supply and references, for VDDIO = VDDA = 3.3 V with VDDD external (data sheet "Power Supply Configuration").
    {kAnaPower, 0x4060, 0},     // linreg-simple and startup power-ups off (VDDD external); reference bias on; nothing else powered
    {kLinregCtrl, 0x006C, 0},   // charge pump from VDDIO (both rails above 3.1 V); VDDD programming for an external supply
    {kRefCtrl, 0x01F3, 0},      // VAG = 1.575 V (VDDA / 2), bias +12.5 %, SMALL_POP: the slow VAG ramp
    {kLineOutCtrl, 0x0F22, 0},  // line-out reference 1.65 V (VDDIO / 2), 0.54 mA
    {kShortCtrl, 0x4446, 0},    // headphone short detect, trip levels
    {kLineOutVol, 0x1D1D, 0},   // line-out level (about 1.3 V peak to peak at full scale); line out is muted
    // The analog outputs, the DAC, ADC and capless headphone are powered: the VAG ramp starts here. Muted all the way.
    {kAnaPower, 0x40FF, 0},
    {kDigPower, 0x0073, 500},   // ADC, DAC, DAP, I2S in and out; then wait out the VAG ramp (about 400 ms with SMALL_POP)
    // Clocking and routing, still muted.
    {kClkCtrl, 0x0004, 0},      // SYS_FS = 44.1 kHz, MCLK = 256 Fs
    {kI2sCtrl, kI2sCtrlValue, 0},  // I2S slave, 16 bit, I2S format; SCLKFREQ 32 Fs although BCLK is 64 Fs (see above)
    {kSssCtrl, 0x0010, 0},      // DAC from I2S in; I2S out from the ADC
    {kAnaCtrl, 0x0136, 0},      // the ADC unmuted (the RX path); line out and headphone STILL MUTED
};
inline constexpr unsigned kPowerUpCount = sizeof(kPowerUp) / sizeof(kPowerUp[0]);

// ---- Unmute and mute --------------------------------------------------------------------------------------------------------
// The headphone volume the unmute settles on: 0x40 is -20 dB of the codec's nominal headphone level, quiet with the test tone's
// -20 dBFS on top (about -40 dB below full scale, a few millivolts). Never louder than 0x30 (-12 dB): the floor of what any
// later image may set here without changing this constant and its test.
inline constexpr std::uint8_t kHpUnmuteCode = 0x40;
inline constexpr std::uint8_t kHpLoudestAllowedCode = 0x30;
static_assert(kHpUnmuteCode >= kHpLoudestAllowedCode && kHpUnmuteCode <= kHpVolMin, "the unmute headphone level is a quiet one");
inline constexpr std::uint16_t kHpRampStepMs = 2;       // the data sheet ramps the headphone volume in single 0.5 dB steps
inline constexpr std::uint16_t kDacRampWaitMs = 50;
inline constexpr std::uint16_t kAnaCtrlOutputsMuted = 0x0136;
inline constexpr std::uint16_t kAnaCtrlOutputsUnmuted = 0x0026;  // MUTE_LO and MUTE_HP clear; ADC unmuted, line in, zero-cross on
inline constexpr std::uint16_t kAdcDacMuted = 0x020C;
inline constexpr std::uint16_t kAdcDacUnmuted = 0x0200;  // volume ramp on, DAC unmuted: the chip ramps the DAC up by itself

constexpr std::uint16_t hp_ctrl(std::uint8_t code) noexcept { return static_cast<std::uint16_t>((code << 8) | code); }

// Step i of the unmute plan, or false past the end. The order is the data sheet's: the headphone volume is at its minimum when
// the headphone is unmuted, the DAC un-mutes with the chip's own soft ramp while the analog outputs are still muted, and only
// then do the line out and the headphone open (the headphone at its minimum), after which the headphone volume ramps up one
// 0.5 dB step at a time to kHpUnmuteCode. The amplifier key is raised by the caller after the last step, never before.
//   0     HP volume to the minimum (already there; written so the plan is self-contained)
//   1     DAC volume to 0 dB (the DAC is still muted)
//   2     DAC unmute, soft ramp; wait kDacRampWaitMs
//   3     line out and headphone unmuted
//   4..   headphone volume 0x7E, 0x7D, ... down to kHpUnmuteCode, one step every kHpRampStepMs
constexpr bool unmute_step(unsigned i, Step& out) noexcept {
  if (i == 0) { out = Step{kAnaHpCtrl, hp_ctrl(kHpVolMin), 0}; return true; }
  if (i == 1) { out = Step{kDacVol, static_cast<std::uint16_t>((kDacVolZeroDb << 8) | kDacVolZeroDb), 0}; return true; }
  if (i == 2) { out = Step{kAdcDacCtrl, kAdcDacUnmuted, kDacRampWaitMs}; return true; }
  if (i == 3) { out = Step{kAnaCtrl, kAnaCtrlOutputsUnmuted, 0}; return true; }
  const unsigned ramp = i - 4u;
  const unsigned steps = static_cast<unsigned>(kHpVolMin - kHpUnmuteCode);
  if (ramp >= steps) {
    return false;
  }
  out = Step{kAnaHpCtrl, hp_ctrl(static_cast<std::uint8_t>(kHpVolMin - 1u - ramp)), kHpRampStepMs};
  return true;
}

// The mute plan, applied after the amplifier key has been lowered: the outputs first, the DAC after.
inline constexpr Step kMute[] = {
    {kAnaCtrl, kAnaCtrlOutputsMuted, 0},
    {kAdcDacCtrl, kAdcDacMuted, 0},
};
inline constexpr unsigned kMuteCount = sizeof(kMute) / sizeof(kMute[0]);

// ---- The stop (OS-POWER P1) ---------------------------------------------------------------------------------------------------------------
// What Mk66Audio::stop() sends the codec before it stops the I2S and the DMA (the unit is powering off or restarting), click-free by construction and in this order:
//   1. the DAC is muted through the chip's own soft ramp (kSoftMute: VOL_RAMP_EN is on in kAdcDacMuted) and the ramp is waited out, kDacRampWaitMs: the sound is gone before anything is switched,
//      so the amplifier key's fall (which with sound playing could click: it is the one that mute() takes first, for the update path's quiet hand-over) is on silence;
//   2. the amplifier key goes LOW (the caller's, a GPIO write);
//   3. kMute: the line out and the headphone muted, the DAC muted again (a no-op when the ramp has done);
//   4. kPowerDown: the headphone volume to its floor, the DAC volume muted; then the VAG's own power-down ramp (OS-BUG-POWERPOP): CHIP_ANA_POWER with VAG_POWERUP (bit 7) cleared and the headphone, DAC,
//      capless headphone and line out still powered (kAnaPowerVagOff), and a wait for the ramp (kVagRampDownWaitMs); only then CHIP_ANA_POWER back to the value the power-up plan starts from
//      (kAnaPowerDown: the reference bias on, the headphone, DAC, capless headphone, line out and the VAG powered down), so the analogue stage is off with the key already LOW.
//   Why the VAG goes alone and first: the data sheet's CHIP_ANA_POWER VAG_POWERUP bit starts the output ramp when set and the power-down ramp when cleared, and CHIP_REF_CTRL SMALL_POP (bit 0, set by the
//   power-up plan: kRefCtrl 0x01F3) slows that ramp (about 400 ms) to keep it from popping. The headphone and line-out stages sit on the VAG: one write that drops the VAG together with the output stages
//   (0x4060 straight from 0x40FF) removes that level in a step, the pop of a power-off with headphones or the line out connected. The I2S stays on (zeros, the DAC muted) until the ramp has run.
//   Every wait is a constant (the soft mute's 50 ms, the VAG ramp's 500 ms).
// The only write before the key falls is the DAC's soft mute; from the key's fall on every step leaves the outputs muted, and the key is LOW before the first step that touches the analogue power.
// The order is one function, run_stop(), which the image and the test both run (tests/mk66_sgtl5000_test.cpp, on the register model above).
inline constexpr std::uint16_t kAnaPowerDown = 0x4060;  // the power-up plan's first CHIP_ANA_POWER value: "nothing else powered" (no output, no VAG, no ADC)
static_assert((kAnaPowerDown & kAnaPowerOutputs) == 0u, "the stop's CHIP_ANA_POWER powers none of the outputs or the VAG");
inline constexpr std::uint16_t kAnaPowerVag = 1u << 7;  // CHIP_ANA_POWER VAG_POWERUP
inline constexpr std::uint16_t kAnaPowerVagOff = 0x40FF & ~kAnaPowerVag;  // the power-up plan's 0x40FF with the VAG alone cleared: the power-down ramp starts, the output stages follow it
static_assert((kAnaPowerVagOff & kAnaPowerVag) == 0u && (kAnaPowerVagOff & kAnaPowerOutputs) != 0u, "the ramp step drops the VAG and keeps the output stages powered");
inline constexpr std::uint16_t kRefCtrlSmallPop = 1u << 0;  // CHIP_REF_CTRL SMALL_POP: the slow VAG ramp
inline constexpr std::uint16_t kVagRampDownWaitMs = 500;    // as the power-up's wait: about 400 ms with SMALL_POP
inline constexpr Step kSoftMute = {kAdcDacCtrl, kAdcDacMuted, kDacRampWaitMs};
inline constexpr Step kPowerDown[] = {
    {kAnaHpCtrl, hp_ctrl(kHpVolMin), 0},
    {kDacVol, static_cast<std::uint16_t>((kDacVolMuted << 8) | kDacVolMuted), 0},
    {kAnaPower, kAnaPowerVagOff, kVagRampDownWaitMs},
    {kAnaPower, kAnaPowerDown, 0},
};
inline constexpr unsigned kPowerDownCount = sizeof(kPowerDown) / sizeof(kPowerDown[0]);

// `write(const Step&) -> bool` sends a register (false: not acknowledged), `key_low()` lowers the amplifier key, `wait(ms)` waits. Every step is attempted whatever an earlier one answered (the unit is
// going down: a codec that does not answer must not stop the key or the I2S); the result is true when every write was acknowledged.
template <typename Write, typename KeyLow, typename Wait>
bool run_stop(Write&& write, KeyLow&& key_low, Wait&& wait) {
  bool ok = write(kSoftMute);
  wait(kSoftMute.wait_ms);
  key_low();
  for (const Step& step : kMute) {
    ok = write(step) && ok;
  }
  for (const Step& step : kPowerDown) {
    ok = write(step) && ok;
    if (step.wait_ms != 0u) {
      wait(step.wait_ms);
    }
  }
  return ok;
}

// ---- The clock the codec is told about ------------------------------------------------------------------------------------------
// CHIP_CLK_CTRL = 0x0004: SYS_FS = 0x1 (44.1 kHz), MCLK_FREQ = 0x0 (256 x Fs), no PLL. The codec derives its internal clocks from
// MCLK / 256, so the real sample rate must be near 44.1 kHz and MCLK within the synchronous range of Table 8 (256 x Fs for Fs =
// 44.1 kHz) and below the PLL range's top of 27 MHz: both asserted for whatever rate constant the build has (4080/1 gives
// 11.294 MHz = 256 x 44,117.6 Hz, +400 ppm; the 196/3125 retune gives 11.2896 MHz = 256 x 44,100 Hz exactly).
inline constexpr std::uint16_t kClkCtrlValue = 0x0004;
inline constexpr std::uint32_t kMclkHz = hal::audio::mclk_hz(hal::audio::kMclkRatio);
static_assert(((kClkCtrlValue >> 2) & 3u) == 1u && (kClkCtrlValue & 3u) == 0u, "CHIP_CLK_CTRL: SYS_FS 44.1 kHz, MCLK 256 Fs");
static_assert(kMclkHz >= 8000000u && kMclkHz <= 27000000u, "SGTL5000 SYS_MCLK range (data sheet, Clocking)");
static_assert(kMclkHz / hal::audio::kMclkPerSample >= 43659u && kMclkHz / hal::audio::kMclkPerSample <= 44541u,
              "MCLK / 256 within 1 % of 44.1 kHz: the SYS_FS = 44.1 kHz setting describes this clock");

// ---- The analogue input (OS-011c) ---------------------------------------------------------------------------------------------------
// What the sample recorder's source switch writes. The registers and the bits are the data sheet's (CHIP_ANA_CTRL table 22, CHIP_ANA_ADC_CTRL table 21,
// CHIP_MIC_CTRL table 36):
//   CHIP_ANA_CTRL bit 2 SELECT_ADC: 1 = line in, 0 = microphone. The other bits of the register are the output mutes and the zero-cross detectors, which the power-up,
//     unmute and mute plans own: the value written here is always composed with them (ana_ctrl_value).
//   CHIP_ANA_ADC_CTRL bits 7:4 ADC_VOL_RIGHT, 3:0 ADC_VOL_LEFT: 0 dB to +22.5 dB in 1.5 dB steps (0 = 3.12 Vpp full scale, 15 = 0.24 Vpp).
//   CHIP_MIC_CTRL bits 9:8 BIAS_RESISTOR (1 = 2 kOhm), 6:4 BIAS_VOLT (7 = the highest bias voltage), 1:0 GAIN (the preamp: 0, +20, +30, +40 dB).
// The microphone path is a preamp step plus ADC steps of 1.5 dB; the line path is the ADC steps alone. The recorder's gain is a percentage, 0..100:
// the line's is mapped to 0..15 (`gain * 15 / 100`), the low microphone range to 0..25 dB and the high one to 0..44 dB (`gain * 25 / 100`, `gain * 44 / 100`).
inline constexpr std::uint16_t kAnaCtrlSelectLine = 1u << 2;
inline constexpr std::uint16_t kMicCtrlBase = 0x0170;  // 2 kOhm bias resistor, the highest bias voltage, preamp 0 dB, before the preamp gain bits
inline constexpr unsigned kAdcVolMax = 15;

// CHIP_ANA_CTRL with SELECT_ADC set to `line` (true) or the microphone (false), the rest of `ana_ctrl` as it is.
constexpr std::uint16_t ana_ctrl_with_adc(std::uint16_t ana_ctrl, bool line) noexcept {
  return line ? static_cast<std::uint16_t>(ana_ctrl | kAnaCtrlSelectLine) : static_cast<std::uint16_t>(ana_ctrl & ~kAnaCtrlSelectLine);
}
// The plan's own CHIP_ANA_CTRL: the outputs muted (kAnaCtrlOutputsMuted) or open (kAnaCtrlOutputsUnmuted) and the ADC on the line or the microphone.
constexpr std::uint16_t ana_ctrl_value(bool outputs_muted, bool line) noexcept {
  return ana_ctrl_with_adc(outputs_muted ? kAnaCtrlOutputsMuted : kAnaCtrlOutputsUnmuted, line);
}

// The ADC steps of a line gain in percent (0..100, clamped).
constexpr unsigned line_adc_steps(unsigned gain_percent) noexcept { return (gain_percent > 100u ? 100u : gain_percent) * kAdcVolMax / 100u; }
// The microphone's gain in whole dB for a percentage, in the low range (0..25 dB) or the high one (0..44 dB).
constexpr unsigned mic_gain_db(bool high_range, unsigned gain_percent) noexcept {
  return (gain_percent > 100u ? 100u : gain_percent) * (high_range ? 44u : 25u) / 100u;
}
// The preamp setting and the ADC steps of a gain in dB: +40, +30 or +20 dB of preamp when the gain reaches them, the rest in ADC
// steps of 1.5 dB (`dB * 2 / 3`, at most 15).
struct MicSetting {
  std::uint8_t preamp;  // CHIP_MIC_CTRL GAIN, 0..3
  std::uint8_t adc;     // ADC_VOL, 0..15, both channels
};
constexpr MicSetting mic_setting(unsigned db) noexcept {
  unsigned preamp = 0;
  if (db >= 40u) {
    preamp = 3;
    db -= 40u;
  } else if (db >= 30u) {
    preamp = 2;
    db -= 30u;
  } else if (db >= 20u) {
    preamp = 1;
    db -= 20u;
  }
  unsigned adc = db * 2u / 3u;
  adc = adc > kAdcVolMax ? kAdcVolMax : adc;
  return MicSetting{static_cast<std::uint8_t>(preamp), static_cast<std::uint8_t>(adc)};
}
constexpr std::uint16_t adc_ctrl_value(unsigned steps) noexcept {
  return static_cast<std::uint16_t>(((steps > kAdcVolMax ? kAdcVolMax : steps) << 4) | (steps > kAdcVolMax ? kAdcVolMax : steps));
}

// The writes of a source switch, in this order (the microphone's bias and preamp first, then the ADC level, the input select last, so the new level is in place when the
// input changes). `ana_ctrl` is composed by the caller (it knows whether the outputs are muted).
struct InputPlan {
  Step step[3];
  unsigned count;
  bool line;  // SELECT_ADC after the plan
};
constexpr InputPlan line_input_plan(unsigned gain_percent, bool outputs_muted) noexcept {
  return InputPlan{{Step{kAnaAdcCtrl, adc_ctrl_value(line_adc_steps(gain_percent)), 0}, Step{kAnaCtrl, ana_ctrl_value(outputs_muted, true), 0}, Step{0, 0, 0}}, 2, true};
}
constexpr InputPlan mic_input_plan(bool high_range, unsigned gain_percent, bool outputs_muted) noexcept {
  const MicSetting m = mic_setting(mic_gain_db(high_range, gain_percent));
  return InputPlan{{Step{kMicCtrl, static_cast<std::uint16_t>(kMicCtrlBase | m.preamp), 0}, Step{kAnaAdcCtrl, adc_ctrl_value(m.adc), 0},
                    Step{kAnaCtrl, ana_ctrl_value(outputs_muted, false), 0}},
                   3, false};
}
// A gain change alone (the same registers without the input select).
constexpr InputPlan line_gain_plan(unsigned gain_percent) noexcept {
  return InputPlan{{Step{kAnaAdcCtrl, adc_ctrl_value(line_adc_steps(gain_percent)), 0}, Step{0, 0, 0}, Step{0, 0, 0}}, 1, true};
}
constexpr InputPlan mic_gain_plan(bool high_range, unsigned gain_percent) noexcept {
  const MicSetting m = mic_setting(mic_gain_db(high_range, gain_percent));
  return InputPlan{{Step{kMicCtrl, static_cast<std::uint16_t>(kMicCtrlBase | m.preamp), 0}, Step{kAnaAdcCtrl, adc_ctrl_value(m.adc), 0}, Step{0, 0, 0}}, 2, false};
}

// ---- The master output EQ and the bass boost, in the DAP (OS-MASTEREQ, docs/specs/OS-MASTEREQ.md) ------------------------------------------------
// Register facts: data sheet rev. 7 tables 17, 20, 40-53 (the spec's "Codec facts"). With every value at its default nothing is written and the codec is as the power-up plan leaves it
// (DAP not routed, CHIP_SSS_CTRL 0x0010). The first non-default value routes the DAC through the DAP (route in); a live edit only moves register values in small steps; the way back (route out)
// is taken only inside unmute, with the DAC muted. EqDriver is pure (it only calls `write(Step) -> bool`), so the host test runs it on the register model; Mk66Audio supplies the I2C write.
namespace haudio = o3os::hal::audio;
inline constexpr std::uint16_t kDapControl = 0x0100;           // bit 0: DAP enable
inline constexpr std::uint16_t kDapBassEnhance = 0x0104;       // EN bit 0, CUTOFF bits 6:4 (0 = 80 Hz), BYPASS_HPF bit 8
inline constexpr std::uint16_t kDapBassEnhanceCtrl = 0x0106;   // LR_LEVEL bits 13:8, BASS_LEVEL bits 6:0 (0x7F least, 0x00 most)
inline constexpr std::uint16_t kDapAudioEq = 0x0108;           // bits 1:0: 3 = five-band graphic EQ
inline constexpr std::uint16_t kDapGeqBand[5] = {0x0116, 0x0118, 0x011A, 0x011C, 0x011E};  // 115 Hz, 330 Hz, 990 Hz, 3 kHz, 9.9 kHz: bits 6:0, 0.25 dB per LSB
inline constexpr std::uint16_t kSssIdle = 0x0010;              // the DAC from I2S in, the DAP not in the path (as the power-up plan)
inline constexpr std::uint16_t kSssDap = 0x0070;               // the DAC from the DAP
inline constexpr std::uint16_t kDapEnabled = 0x0001;
inline constexpr std::uint16_t kDapGeq = 0x0003;
inline constexpr std::uint16_t kBassEnhanceOn = 0x0101;        // EN, CUTOFF 0, BYPASS_HPF
inline constexpr std::uint16_t kBassEnhanceOff = 0x0100;
inline constexpr std::uint8_t kBassLevelOff = 0x7F;
inline constexpr std::uint8_t kBassLrLevel = 0x0B;
inline constexpr std::uint8_t kGeqZeroDb = 0x2F;
inline constexpr int kEqDbMax = 12;
inline constexpr unsigned kBassMax = 100;
inline constexpr unsigned kEqBands = 5;

// OutputEq is five int8 and one uint8: no padding, so the comparisons are byte compares.
static_assert(sizeof(haudio::OutputEq) == kEqBands + 1u, "OutputEq has padding: compare field by field");
inline bool eq_equal(const haudio::OutputEq& a, const haudio::OutputEq& b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }
inline bool eq_is_default(const haudio::OutputEq& e) noexcept { return eq_equal(e, haudio::OutputEq{}); }
// 0x2F + 4 per dB, clamped to -12..+12 dB, the floor 0x00 (-12 dB gives -11.75 dB).
constexpr std::uint8_t geq_code(int db) noexcept {
  const int d = db < -kEqDbMax ? -kEqDbMax : (db > kEqDbMax ? kEqDbMax : db);
  const int code = static_cast<int>(kGeqZeroDb) + 4 * d;
  return static_cast<std::uint8_t>(code < 0 ? 0 : code);
}
// BASS_LEVEL of a bass boost 0..100: 0 is off (0x7F), 1..100 is 0x7F - round(v * 0.8 * 127 / 100) (100 gives 0x19).
constexpr std::uint8_t bass_level_code(unsigned v) noexcept {
  const unsigned c = v > kBassMax ? kBassMax : v;
  return c == 0u ? kBassLevelOff : static_cast<std::uint8_t>(kBassLevelOff - (c * 1016u + 500u) / 1000u);
}
constexpr std::uint16_t bass_ctrl_value(std::uint8_t level) noexcept { return static_cast<std::uint16_t>((kBassLrLevel << 8) | level); }

// The DAP's own writes of a route in, in this order: DAP enable, EQ select, the five bands, the bass level, the bass enable, and last the routing (CHIP_SSS_CTRL). The DAC is muted around them.
// A table of registers and fixed values; the bands, the bass level and the bass enable take theirs from the wanted values.
inline constexpr unsigned kEqDapSteps = 10;
inline constexpr std::uint16_t kEqDapReg[kEqDapSteps] = {kDapControl,      kDapAudioEq,          kDapGeqBand[0], kDapGeqBand[1], kDapGeqBand[2],
                                                         kDapGeqBand[3],   kDapGeqBand[4],       kDapBassEnhanceCtrl, kDapBassEnhance, kSssCtrl};
inline constexpr std::uint16_t kEqDapFixed[kEqDapSteps] = {kDapEnabled, kDapGeq, 0, 0, 0, 0, 0, 0, 0, kSssDap};
constexpr Step eq_dap_step(unsigned i, const haudio::OutputEq& w) noexcept {
  std::uint16_t v = kEqDapFixed[i];
  if (i >= 2u && i < 2u + kEqBands) {
    v = geq_code(w.band_db[i - 2u]);
  } else if (i == 7u) {
    v = bass_ctrl_value(bass_level_code(w.bass));
  } else if (i == 8u) {
    v = w.bass != 0u ? kBassEnhanceOn : kBassEnhanceOff;
  }
  return Step{kEqDapReg[i], v, 0};
}
// One bounded move of a register code toward its target (a band, the bass level): at most `max` codes.
constexpr std::uint8_t step_toward(std::uint8_t cur, std::uint8_t target, int max) noexcept {
  const int d = static_cast<int>(target) - static_cast<int>(cur);
  return static_cast<std::uint8_t>(static_cast<int>(cur) + (d > max ? max : (d < -max ? -max : d)));
}

// The I2C write the driver is given: a function pointer and its context (no template, so the driver is compiled once). Returns true when the write was acknowledged.
struct Writer {
  bool (*fn)(void*, const Step&) noexcept;
  void* ctx;
  bool operator()(const Step& s) const noexcept { return fn(ctx, s); }
};

// The driver. Main thread only (no ISR touches it). Phases: Idle (the DAP is not routed, or not known to be), RouteIn (a live route in, a few passes), Routed (live edits step the registers
// toward the wanted values), Failed (an I2C write was not acknowledged: nothing more is tried until the wanted values change once; only the recovery below goes on).
// Recovery of a failed live route in, one write per pass and never in the pass of the failure: the routing back to the idle value (sss_pending_, tried once), then the DAC unmute, which is
// retried every pass until the chip acknowledges it (dac_muted_), so the DAC is not left soft muted.
class EqDriver {
 public:
  enum class Phase : std::uint8_t { Idle, RouteIn, Routed, Failed };
  static constexpr unsigned kWritesPerPass = 3;  // the main loop is never held for long
  static constexpr int kBandStep = 2;            // LSB (0.5 dB) a band moves per write
  static constexpr int kBassStep = 4;            // BASS_LEVEL codes per write

  // The wanted values (the project's), recorded only. A change after a failure allows one more attempt: the live stepping if the DAP may be routed, else a new route in.
  void set(const haudio::OutputEq& w) noexcept {
    if (!eq_equal(w, want_)) {
      want_ = w;
      if (phase_ == Phase::Failed) {
        phase_ = routed_ ? Phase::Routed : Phase::Idle;
      }
    }
  }
  // The codec was (re)started: the power-up plan leaves the DAP out of the path.
  void on_start() noexcept {
    phase_ = Phase::Idle;
    routed_ = false;
    dac_muted_ = false;
    sss_pending_ = false;
  }
  // The outputs were muted or the path stopped: nothing is stepped until unmute applies the values (a route in cut short is redone there, the unmute plan unmutes the DAC).
  void on_mute() noexcept {
    phase_ = Phase::Idle;
    dac_muted_ = false;
    sss_pending_ = false;
  }
  Phase phase() const noexcept { return phase_; }
  bool routed() const noexcept { return routed_; }
  const haudio::OutputEq& want() const noexcept { return want_; }

  // Inside unmute(), the DAC and the outputs muted: the codec is made to match the wanted values, blocking (like the rest of unmute). All defaults: back to the idle state if the DAP may be
  // routed (route out: the routing first, the DAP off only after the routing write was acknowledged, so the DAC is never fed from a disabled DAP); otherwise the full route in, trusting no
  // earlier state. A failed write restores the routing and the DAC is unmuted by the caller's plan anyway.
  void apply_at_unmute(Writer write) noexcept {
    phase_ = Phase::Idle;
    dac_muted_ = false;
    sss_pending_ = false;
    if (eq_is_default(want_)) {
      if (routed_) {
        routed_ = !write(Step{kSssCtrl, kSssIdle, 0});
        if (!routed_) {
          (void)write(Step{kDapControl, 0, 0});
        }
      }
      return;
    }
    for (unsigned i = 0; i < kEqDapSteps; ++i) {
      const Step s = eq_dap_step(i, want_);
      routed_ = routed_ || i == kEqDapSteps - 1u;  // the routing write is attempted: the chip may take it even if the acknowledge is lost
      if (!write(s)) {
        if (routed_ && write(Step{kSssCtrl, kSssIdle, 0})) {
          routed_ = false;
        }
        phase_ = Phase::Failed;
        return;
      }
      note(i, s);
    }
    phase_ = Phase::Routed;
  }

  // One main-loop pass while the outputs are open: the recovery of a failed route in, the live route in (the DAC's soft mute and its ramp waited by `now_ms`, never blocking), then the live
  // stepping. At most kWritesPerPass writes.
  void pump(std::uint32_t now_ms, Writer write) noexcept {
    if (sss_pending_ || dac_muted_) {
      if (sss_pending_) {
        sss_pending_ = false;
        if (write(Step{kSssCtrl, kSssIdle, 0})) {
          routed_ = false;
          if (phase_ == Phase::Routed) {
            phase_ = Phase::Idle;  // a value change came before the recovery: the route in is redone
          }
        }
      } else if (write(Step{kAdcDacCtrl, kAdcDacUnmuted, 0})) {
        dac_muted_ = false;
      }
      return;
    }
    if (phase_ == Phase::Failed) {
      return;
    }
    if (phase_ == Phase::Idle) {
      if (eq_is_default(want_)) {
        return;  // the idle state: no write at all
      }
      phase_ = Phase::RouteIn;
      idx_ = 0;
      ready_ = now_ms;
    }
    if (waiting_) {  // the compare is only valid inside the wait: after 2^31 ms it would read as "not yet"
      if (static_cast<std::int32_t>(now_ms - ready_) < 0) {
        return;
      }
      waiting_ = false;
    }
    unsigned budget = kWritesPerPass;
    while (phase_ == Phase::RouteIn && budget != 0u) {
      // idx_ 0: DAC soft mute (waits its ramp); 1..kEqDapSteps: the DAP steps; then the DAC unmute (waits its ramp); then done
      const bool last = idx_ == kEqDapSteps + 1u;
      const Step s = idx_ == 0u ? kSoftMute : (last ? Step{kAdcDacCtrl, kAdcDacUnmuted, kDacRampWaitMs} : eq_dap_step(idx_ - 1u, want_));
      routed_ = routed_ || idx_ == kEqDapSteps;  // the routing write (DAP step 9) is attempted: as above
      if (!write(s)) {
        phase_ = Phase::Failed;  // the recovery writes follow in the next passes
        sss_pending_ = routed_;
        dac_muted_ = true;
        return;
      }
      if (idx_ != 0u && !last) {
        note(idx_ - 1u, s);
      }
      --budget;
      if (last) {
        phase_ = Phase::Routed;
      } else {
        ++idx_;
      }
      if (s.wait_ms != 0u) {
        ready_ = now_ms + s.wait_ms;
        waiting_ = true;
        return;
      }
    }
    if (phase_ != Phase::Routed || budget == 0u) {
      return;
    }
    for (unsigned i = 0; i < kEqBands && budget != 0u; ++i) {
      const std::uint8_t next = step_toward(cur_band_[i], geq_code(want_.band_db[i]), kBandStep);
      if (next != cur_band_[i]) {
        if (!write(Step{kDapGeqBand[i], next, 0})) {
          phase_ = Phase::Failed;
          return;
        }
        cur_band_[i] = next;
        --budget;
      }
    }
    // The bass: the enable only with the level at 0x7F (off), the level in bounded steps.
    const bool want_on = want_.bass != 0u;
    if (budget != 0u && !bass_on_ && want_on) {
      if (!write(Step{kDapBassEnhance, kBassEnhanceOn, 0})) {
        phase_ = Phase::Failed;
        return;
      }
      bass_on_ = true;
      --budget;
    }
    if (budget != 0u && bass_on_) {
      const std::uint8_t next = step_toward(bass_level_, bass_level_code(want_.bass), kBassStep);
      if (next != bass_level_) {
        if (!write(Step{kDapBassEnhanceCtrl, bass_ctrl_value(next), 0})) {
          phase_ = Phase::Failed;
          return;
        }
        bass_level_ = next;
      } else if (!want_on) {
        if (!write(Step{kDapBassEnhance, kBassEnhanceOff, 0})) {
          phase_ = Phase::Failed;
          return;
        }
        bass_on_ = false;
      }
    }
  }

 private:
  // What the codec now holds after DAP step i.
  void note(unsigned i, const Step& s) noexcept {
    if (i >= 2u && i < 2u + kEqBands) {
      cur_band_[i - 2u] = static_cast<std::uint8_t>(s.value);
    } else if (i == 7u) {
      bass_level_ = static_cast<std::uint8_t>(s.value & 0x7Fu);
    } else if (i == 8u) {
      bass_on_ = s.value == kBassEnhanceOn;
    }
  }

  haudio::OutputEq want_{};           // the project's values
  std::uint32_t ready_ = 0;           // a live route in waits for this millisecond (wrap-safe compare, only while waiting_)
  bool waiting_ = false;              // a ramp wait is pending until ready_
  std::uint8_t cur_band_[kEqBands] = {kGeqZeroDb, kGeqZeroDb, kGeqZeroDb, kGeqZeroDb, kGeqZeroDb};  // the band registers as written (valid when routed)
  std::uint8_t bass_level_ = kBassLevelOff;
  std::uint8_t idx_ = 0;
  Phase phase_ = Phase::Idle;
  bool bass_on_ = false;
  bool routed_ = false;               // CHIP_SSS_CTRL may be 0x0070: set when that write is made, cleared when 0x0010 is acknowledged
  bool sss_pending_ = false;          // recovery: the routing back to 0x0010 is still to be written
  bool dac_muted_ = false;            // recovery: the DAC may still be soft muted by a failed live route in; the unmute is retried until acknowledged
};

}  // namespace o3os::platform::mk66::sgtl5000
