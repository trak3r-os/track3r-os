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

}  // namespace o3os::platform::mk66::sgtl5000
