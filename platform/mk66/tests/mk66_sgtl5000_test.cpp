// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SGTL5000 power-up, unmute and mute plans (sgtl5000.h, OS-005a) against a model of the codec's registers: the hardware-safety
// properties of the spec (no pop, never loud at boot, MCLK within the codec's limits, the right order) as assertions over every
// step. Host and Cortex-M4F (`qemu_mk66`). The data sheet facts used are cited in sgtl5000.h; what only a unit shows (a real pop,
// a real level) is the HW-A checklist.
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include <o3os/hal/audio.h>
#include <o3os/platform/mk66/sgtl5000.h>

#include <cstdint>
#include <initializer_list>

namespace sg = o3os::platform::mk66::sgtl5000;
namespace haudio = o3os::hal::audio;

namespace {

// The registers the plans touch, at their reset values (data sheet tables: CHIP_ANA_CTRL 0x0133 all muted, CHIP_ANA_HP_CTRL 0x1818 =
// 0 dB, CHIP_ADCDAC_CTRL 0x020C DAC muted with ramp, CHIP_DAC_VOL 0x3C3C, CHIP_ANA_POWER 0x7060, CHIP_LINE_OUT_VOL 0x0404).
struct Codec {
  std::uint16_t ana_ctrl = 0x0133;
  std::uint16_t hp = 0x1818;
  std::uint16_t adcdac = 0x020C;
  std::uint16_t dac_vol = 0x3C3C;
  std::uint16_t ana_power = 0x7060;
  std::uint16_t dig_power = 0x0000;
  std::uint16_t line_out_vol = 0x0404;
  std::uint16_t clk = 0x0008;
  std::uint16_t i2s = 0x0010;
  std::uint16_t sss = 0x0070;
  std::uint16_t ref = 0x0000;
  std::uint16_t linreg = 0x0000;
  std::uint16_t line_out_ctrl = 0x0000;
  std::uint16_t short_ctrl = 0x0000;
  unsigned writes = 0;

  void write(std::uint16_t reg, std::uint16_t value) {
    ++writes;
    switch (reg) {
      case sg::kAnaCtrl: ana_ctrl = value; break;
      case sg::kAnaHpCtrl: hp = value; break;
      case sg::kAdcDacCtrl: adcdac = value; break;
      case sg::kDacVol: dac_vol = value; break;
      case sg::kAnaPower: ana_power = value; break;
      case sg::kDigPower: dig_power = value; break;
      case sg::kLineOutVol: line_out_vol = value; break;
      case sg::kClkCtrl: clk = value; break;
      case sg::kI2sCtrl: i2s = value; break;
      case sg::kSssCtrl: sss = value; break;
      case sg::kRefCtrl: ref = value; break;
      case sg::kLinregCtrl: linreg = value; break;
      case sg::kLineOutCtrl: line_out_ctrl = value; break;
      case sg::kShortCtrl: short_ctrl = value; break;
      default: CHECK_MESSAGE(false, "a plan wrote a register this model does not know: " << reg); break;
    }
  }
  bool outputs_muted() const {
    return (ana_ctrl & (sg::kAnaCtrlMuteLineOut | sg::kAnaCtrlMuteHeadphone)) == (sg::kAnaCtrlMuteLineOut | sg::kAnaCtrlMuteHeadphone);
  }
  bool dac_muted() const { return (adcdac & sg::kAdcDacMuteBoth) == sg::kAdcDacMuteBoth; }
  std::uint8_t hp_left() const { return static_cast<std::uint8_t>(hp & 0x7F); }
  std::uint8_t hp_right() const { return static_cast<std::uint8_t>((hp >> 8) & 0x7F); }
  bool dac_vol_muted() const { return (dac_vol & 0xFF) >= sg::kDacVolMuted && (dac_vol >> 8) >= sg::kDacVolMuted; }
};

}  // namespace

TEST_CASE("sgtl5000: register addresses are the data sheet's") {
  CHECK(sg::kChipId == 0x0000);
  CHECK(sg::kDigPower == 0x0002);
  CHECK(sg::kClkCtrl == 0x0004);
  CHECK(sg::kI2sCtrl == 0x0006);
  CHECK(sg::kSssCtrl == 0x000A);
  CHECK(sg::kAdcDacCtrl == 0x000E);
  CHECK(sg::kDacVol == 0x0010);
  CHECK(sg::kAnaHpCtrl == 0x0022);
  CHECK(sg::kAnaCtrl == 0x0024);
  CHECK(sg::kLinregCtrl == 0x0026);
  CHECK(sg::kRefCtrl == 0x0028);
  CHECK(sg::kLineOutCtrl == 0x002C);
  CHECK(sg::kLineOutVol == 0x002E);
  CHECK(sg::kAnaPower == 0x0030);
  CHECK(sg::kShortCtrl == 0x003C);
  CHECK(sg::kPartId == 0xA0);
}

TEST_CASE("sgtl5000 power-up: every output stays muted at every step, from reset to the end") {
  Codec codec;
  CHECK(codec.outputs_muted());   // the chip's reset state, per the data sheet
  CHECK(codec.dac_muted());
  for (unsigned i = 0; i < sg::kPowerUpCount; ++i) {
    codec.write(sg::kPowerUp[i].reg, sg::kPowerUp[i].value);
    INFO("after step " << i << " (register 0x" << sg::kPowerUp[i].reg << ")");
    CHECK(codec.outputs_muted());   // MUTE_LO and MUTE_HP never clear during the power-up
    CHECK(codec.dac_muted());       // nor the DAC's mute
    if (i >= 3u) {
      CHECK(codec.dac_vol_muted());   // from the step that writes it, the DAC volume register holds "muted"
    }
  }
  CHECK(codec.outputs_muted());
  CHECK(codec.dac_muted());
  CHECK(codec.dac_vol_muted());
  CHECK(codec.hp_left() == sg::kHpVolMin);
  CHECK(codec.hp_right() == sg::kHpVolMin);
  CHECK((codec.adcdac & sg::kAdcDacVolRampEn) != 0u);   // any later volume or mute change is a soft ramp
}

TEST_CASE("sgtl5000 power-up: the order that avoids pops") {
  // Find the first step that powers an output (VAG ramp, headphone, DAC, capless headphone, line out): everything that must be in
  // place before it has been written by then.
  Codec codec;
  unsigned first_power = sg::kPowerUpCount;
  for (unsigned i = 0; i < sg::kPowerUpCount; ++i) {
    if (sg::kPowerUp[i].reg == sg::kAnaPower && (sg::kPowerUp[i].value & sg::kAnaPowerOutputs) != 0u) {
      first_power = i;
      break;
    }
    codec.write(sg::kPowerUp[i].reg, sg::kPowerUp[i].value);
  }
  CHECK_MESSAGE(first_power < sg::kPowerUpCount, "the plan powers the outputs");
  if (first_power >= sg::kPowerUpCount) {
    return;  // no REQUIRE in a no-exceptions build
  }
  CHECK(codec.outputs_muted());                       // muted before anything that can make sound is powered
  CHECK(codec.dac_muted());
  CHECK(codec.dac_vol_muted());
  CHECK(codec.hp_left() == sg::kHpVolMin);            // headphone volume at the floor before the headphone amplifier is powered
  CHECK(codec.hp_right() == sg::kHpVolMin);
  CHECK((codec.ref & 1u) == 1u);                      // SMALL_POP: the slow VAG ramp, set before the ramp starts
  CHECK(codec.ana_power == 0x4060);                   // nothing but the reference bias is powered yet
  CHECK(codec.writes >= 10u);
  CHECK(codec.dig_power == 0u);
  CHECK(first_power >= 10u);

  // The wait that follows the digital power-up covers the VAG ramp (about 200-400 ms; 400 with SMALL_POP).
  std::uint32_t wait_after_power = 0;
  for (unsigned i = first_power; i < sg::kPowerUpCount; ++i) {
    wait_after_power += sg::kPowerUp[i].wait_ms;
  }
  CHECK(wait_after_power >= 400u);
  // Clocks and routing come after the ramp, and the plan never waits for no reason.
  std::uint32_t total_wait = 0;
  for (unsigned i = 0; i < sg::kPowerUpCount; ++i) {
    total_wait += sg::kPowerUp[i].wait_ms;
  }
  CHECK(total_wait == wait_after_power);
  CHECK(total_wait <= 1000u);
}

TEST_CASE("sgtl5000 power-up: the very first writes mute, whatever state the chip was left in by an earlier run") {
  // A reset in the middle of a run leaves the codec configured and possibly unmuted: its registers survive a reset of the K66.
  Codec codec;
  codec.ana_ctrl = 0x0026;      // line out and headphone open
  codec.hp = 0x2020;            // some volume
  codec.adcdac = 0x0200;        // DAC unmuted
  codec.dac_vol = 0x3C3C;       // 0 dB
  codec.ana_power = 0x40FF;     // everything powered
  CHECK_FALSE(codec.outputs_muted());
  // The first four writes bring it to the muted state, before any ANA_POWER write powers anything down or up.
  for (unsigned i = 0; i < 4u; ++i) {
    CHECK(sg::kPowerUp[i].reg != sg::kAnaPower);
    codec.write(sg::kPowerUp[i].reg, sg::kPowerUp[i].value);
  }
  CHECK(codec.outputs_muted());
  CHECK(codec.dac_muted());
  CHECK(codec.dac_vol_muted());
  CHECK(codec.hp_left() == sg::kHpVolMin);
  CHECK(sg::kPowerUp[4].reg == sg::kAnaPower);        // only now the supplies and the power-down/up begin
}

TEST_CASE("sgtl5000 power-up: the final configuration (before any unmuting)") {
  Codec codec;
  for (unsigned i = 0; i < sg::kPowerUpCount; ++i) {
    codec.write(sg::kPowerUp[i].reg, sg::kPowerUp[i].value);
  }
  CHECK(codec.ana_power == 0x40FF);                   // lineout, ADC, DAC, headphone, capless headphone, VAG, reference
  CHECK(codec.dig_power == 0x0073);                   // ADC, DAC, DAP, I2S in and out
  CHECK(codec.clk == sg::kClkCtrlValue);              // SYS_FS 44.1 kHz, MCLK = 256 Fs, no PLL
  CHECK(codec.clk == 0x0004);
  CHECK((codec.i2s & 0x0080) == 0u);                  // MS = 0: the codec is the slave, the K66 drives the clocks
  CHECK(((codec.i2s >> 4) & 3u) == 3u);               // 16-bit data
  CHECK(((codec.i2s >> 2) & 3u) == 0u);               // I2S mode, not right-justified or PCM
  CHECK(codec.sss == 0x0010);                         // DAC from I2S in (bits 5:4 = 1), I2S out from the ADC (bits 1:0 = 0)
  CHECK(codec.ref == 0x01F3);                         // VAG 1.575 V, bias +12.5 %, slow ramp
  CHECK(codec.linreg == 0x006C);
  CHECK(codec.line_out_ctrl == 0x0F22);
  CHECK(codec.line_out_vol == 0x1D1D);
  CHECK((codec.ana_ctrl & sg::kAnaCtrlMuteAdc) == 0u);  // the ADC is unmuted: the RX path hears the line in
  CHECK((codec.ana_ctrl & (1u << 2)) != 0u);            // and the ADC input is the line in, not the microphone
  CHECK(codec.ana_ctrl == 0x0136);
}

TEST_CASE("sgtl5000 unmute: headphone at the floor first, the DAC through its soft ramp, the outputs open, then the volume creeps up") {
  Codec codec;
  for (unsigned i = 0; i < sg::kPowerUpCount; ++i) {
    codec.write(sg::kPowerUp[i].reg, sg::kPowerUp[i].value);
  }
  sg::Step step{};
  unsigned count = 0;
  unsigned ana_ctrl_unmute_at = 0;
  unsigned dac_unmute_at = 0;
  unsigned dac_vol_at = 0;
  std::uint32_t wait_total = 0;
  std::uint8_t previous_hp = sg::kHpVolMin;
  bool ramping = false;
  for (unsigned i = 0; sg::unmute_step(i, step); ++i) {
    ++count;
    wait_total += step.wait_ms;
    // Registers a plan may touch: never the power, the clocks or the routing.
    CHECK((step.reg == sg::kAnaHpCtrl || step.reg == sg::kDacVol || step.reg == sg::kAdcDacCtrl || step.reg == sg::kAnaCtrl));
    const bool was_muted = codec.outputs_muted();
    const bool dac_was_muted = codec.dac_muted();
    codec.write(step.reg, step.value);
    if (step.reg == sg::kDacVol) {
      dac_vol_at = i;
      CHECK(dac_was_muted);                            // the DAC volume is raised while the DAC is still muted
    }
    if (step.reg == sg::kAdcDacCtrl) {
      dac_unmute_at = i;
      CHECK((step.value & sg::kAdcDacVolRampEn) != 0u);  // the DAC unmutes through its soft ramp
      CHECK(was_muted);                                // while the analog outputs are still muted
    }
    if (step.reg == sg::kAnaCtrl) {
      ana_ctrl_unmute_at = i;
      CHECK_FALSE(dac_was_muted);                      // outputs open only after the DAC is unmuted (and ramped: kDacRampWaitMs)
      CHECK(codec.hp_left() == sg::kHpVolMin);         // the headphone opens at its minimum volume (data sheet)
      CHECK_FALSE(codec.outputs_muted());
      ramping = true;
    }
    if (step.reg == sg::kAnaHpCtrl) {
      CHECK(codec.hp_left() == codec.hp_right());
      CHECK(codec.hp_left() >= sg::kHpLoudestAllowedCode);          // never louder than the cap, at any step
      CHECK(previous_hp - codec.hp_left() <= 1);                    // volume moves up by one 0.5 dB step at most per write
      CHECK(codec.hp_left() <= previous_hp);                        // and never back down during the ramp
      if (ramping) {
        CHECK(step.wait_ms == sg::kHpRampStepMs);                   // paced
      }
      previous_hp = codec.hp_left();
    }
    CHECK((codec.dac_vol & 0xFF) >= sg::kDacVolZeroDb);             // the DAC volume is never above 0 dB
    CHECK((codec.dac_vol >> 8) >= sg::kDacVolZeroDb);
  }
  CHECK(count == 4u + (sg::kHpVolMin - sg::kHpUnmuteCode));        // 4 setup steps and the ramp: 67
  CHECK(dac_vol_at < dac_unmute_at);
  CHECK(dac_unmute_at < ana_ctrl_unmute_at);
  CHECK(codec.hp_left() == sg::kHpUnmuteCode);                      // settles on the quiet level
  CHECK(codec.hp_right() == sg::kHpUnmuteCode);
  CHECK(codec.hp_left() >= sg::kHpLoudestAllowedCode);
  CHECK_FALSE(codec.outputs_muted());
  CHECK_FALSE(codec.dac_muted());
  CHECK(codec.ana_ctrl == sg::kAnaCtrlOutputsUnmuted);
  CHECK(wait_total == sg::kDacRampWaitMs + sg::kHpRampStepMs * (sg::kHpVolMin - sg::kHpUnmuteCode));
  CHECK(wait_total < 400u);                                        // the whole unmute is a fraction of a second
  // The plan ends.
  CHECK_FALSE(sg::unmute_step(count, step));
  CHECK_FALSE(sg::unmute_step(count + 100u, step));
}

TEST_CASE("sgtl5000 mute: the outputs first, then the DAC, ending in the state the power-up leaves") {
  Codec codec;
  for (unsigned i = 0; i < sg::kPowerUpCount; ++i) {
    codec.write(sg::kPowerUp[i].reg, sg::kPowerUp[i].value);
  }
  sg::Step step{};
  for (unsigned i = 0; sg::unmute_step(i, step); ++i) {
    codec.write(step.reg, step.value);
  }
  CHECK_FALSE(codec.outputs_muted());
  CHECK(sg::kMuteCount == 2u);
  CHECK(sg::kMute[0].reg == sg::kAnaCtrl);
  CHECK(sg::kMute[1].reg == sg::kAdcDacCtrl);
  codec.write(sg::kMute[0].reg, sg::kMute[0].value);
  CHECK(codec.outputs_muted());
  CHECK_FALSE(codec.dac_muted());   // the DAC follows: the analog outputs close first
  codec.write(sg::kMute[1].reg, sg::kMute[1].value);
  CHECK(codec.dac_muted());
  CHECK(codec.ana_ctrl == 0x0136);  // the same word the power-up ends on
}

TEST_CASE("sgtl5000 stop (OS-POWER P1): the DAC ramps to silence first, the key falls on silence, then the outputs close and the analogue stage powers down") {
  Codec codec;
  for (unsigned i = 0; i < sg::kPowerUpCount; ++i) {
    codec.write(sg::kPowerUp[i].reg, sg::kPowerUp[i].value);
  }
  sg::Step step{};
  for (unsigned i = 0; sg::unmute_step(i, step); ++i) {
    codec.write(step.reg, step.value);  // the unit as it plays: the outputs open, the DAC unmuted
  }
  NEED(!codec.outputs_muted());
  bool key_low = false;
  unsigned writes = 0;
  unsigned waits = 0;
  std::uint32_t wait_ms = 0;
  unsigned key_at = 0;
  unsigned power_at = 0;
  // OS-BUG-POWERPOP: the VAG ramps down ALONE first (the output stages still powered, SMALL_POP on) and the ramp is waited out BEFORE the stages are powered down.
  unsigned vag_off_at = 0;
  unsigned stages_off_at = 0;
  bool ramp_waited = false;
  CHECK((codec.ref & sg::kRefCtrlSmallPop) != 0u);  // the slow ramp is on when the stop starts
  CHECK((codec.ana_power & sg::kAnaPowerVag) != 0u);
  const bool ok = sg::run_stop(
      [&](const sg::Step& s) {
        if (writes == 0) {
          // the first and only write before the key: the DAC through its own soft ramp
          CHECK(s.reg == sg::kAdcDacCtrl);
          CHECK((s.value & sg::kAdcDacMuteBoth) == sg::kAdcDacMuteBoth);
          CHECK((s.value & sg::kAdcDacVolRampEn) != 0u);
          CHECK_FALSE(key_low);
        } else {
          CHECK(key_low);  // everything after it is behind the amplifier key
        }
        if (key_low) {
          CHECK(codec.dac_muted());  // the DAC was ramped down before the key fell
        }
        const auto ana_before = codec.ana_power;
        codec.write(s.reg, s.value);
        if (s.reg == sg::kAnaPower) {
          power_at = writes;
          if (s.value == sg::kAnaPowerVagOff) {
            vag_off_at = writes;
            CHECK(ana_before == 0x40FF);  // what the power-up plan left before this write: everything up
            CHECK((s.value & sg::kAnaPowerOutputs) == (sg::kAnaPowerOutputs & ~sg::kAnaPowerVag));  // HP, DAC, capless HP, line out stay powered while the VAG ramps
            CHECK(s.wait_ms == sg::kVagRampDownWaitMs);
          } else {
            stages_off_at = writes;
            CHECK(vag_off_at != 0u);  // the VAG ramp started before the stages went
            CHECK(ramp_waited);       // and ran its time first
          }
          CHECK(key_low);                    // the key is LOW before the VAG goes
          CHECK(codec.outputs_muted());
          CHECK(codec.dac_muted());
        }
        if (key_low) {
          CHECK(codec.outputs_muted());  // muted from the first write after the key (kMute's first step) to the end
        }
        ++writes;
        return true;
      },
      [&] {
        key_low = true;
        key_at = writes;
      },
      [&](std::uint32_t ms) {
        ++waits;
        wait_ms += ms;
        if (waits == 1u) {
          CHECK_FALSE(key_low);  // the DAC's ramp is waited out before the key
          CHECK(writes == 1u);
          CHECK(ms == sg::kDacRampWaitMs);
        } else {
          CHECK(key_low);
          CHECK(codec.ana_power == sg::kAnaPowerVagOff);  // the VAG ramp runs now, the stages still on it
          CHECK(ms == sg::kVagRampDownWaitMs);
          ramp_waited = true;
        }
      });
  CHECK(ok);
  CHECK(writes == 1u + sg::kMuteCount + sg::kPowerDownCount);
  CHECK(waits == 2u);
  CHECK(wait_ms == sg::kDacRampWaitMs + sg::kVagRampDownWaitMs);
  CHECK(key_at == 1u);
  CHECK(vag_off_at == writes - 2u);   // the VAG alone first, then its ramp's wait
  CHECK(stages_off_at == writes - 1u);  // the final CHIP_ANA_POWER is the last write
  CHECK(power_at == writes - 1u);
  // where it ends
  CHECK(codec.outputs_muted());
  CHECK(codec.dac_muted());
  CHECK(codec.dac_vol_muted());
  CHECK(codec.hp_left() == sg::kHpVolMin);
  CHECK(codec.hp_right() == sg::kHpVolMin);
  CHECK(codec.ana_power == sg::kAnaPowerDown);
  CHECK((codec.ana_power & sg::kAnaPowerOutputs) == 0u);
  CHECK(codec.ana_ctrl == 0x0136);
}

TEST_CASE("sgtl5000 stop: a codec that does not answer does not stop the sequence, the key still falls") {
  unsigned writes = 0;
  bool key_low = false;
  const bool ok = sg::run_stop([&](const sg::Step&) { ++writes; return false; }, [&] { key_low = true; }, [](std::uint32_t) {});
  CHECK_FALSE(ok);
  CHECK(key_low);
  CHECK(writes == 1u + sg::kMuteCount + sg::kPowerDownCount);
}

TEST_CASE("sgtl5000: CHIP_I2S_CTRL is the proven 0x0130 or the data sheet's 64 Fs 0x0030, and the plan writes that value") {
  // 0x0130 = SCLKFREQ 32 Fs, MS 0 (slave), DLEN 3 (16 bit), I2S mode; 0x0030 differs in SCLKFREQ alone (64 Fs: what the K66 drives).
  CHECK((0x0130 ^ 0x0030) == 0x0100);
  CHECK((sg::kI2sCtrlValue == 0x0130 || sg::kI2sCtrlValue == 0x0030));
#if !defined(O3OS_SGTL5000_I2S_CTRL)
  CHECK(sg::kI2sCtrlValue == 0x0130);       // the default build keeps the proven value
#endif
  unsigned writes = 0;
  for (unsigned i = 0; i < sg::kPowerUpCount; ++i) {
    if (sg::kPowerUp[i].reg == sg::kI2sCtrl) {
      ++writes;
      CHECK(sg::kPowerUp[i].value == sg::kI2sCtrlValue);
    }
  }
  CHECK(writes == 1u);
  CHECK((sg::kI2sCtrlValue & 0x0080) == 0);              // slave, whichever SCLKFREQ
  CHECK(((sg::kI2sCtrlValue >> 4) & 3) == 3);            // 16-bit data
}

TEST_CASE("sgtl5000 clocking: MCLK is within the codec's range for both rate constants") {
  for (const haudio::CyclesPerSample rate : {haudio::CyclesPerSample{4080u, 1u}, haudio::CyclesPerSample{1800000u, 441u}}) {
    const haudio::MclkRatio ratio = haudio::mclk_ratio(rate);
    const std::uint32_t mclk = haudio::mclk_hz(ratio);
    INFO("cycles per sample " << rate.num << "/" << rate.den << " mclk " << mclk);
    CHECK(mclk >= 8000000u);                 // data sheet: SYS_MCLK between 8 and 27 MHz (the PLL's range; the 256 Fs ratio is inside it)
    CHECK(mclk <= 27000000u);
    const std::uint32_t fs = mclk / haudio::kMclkPerSample;
    CHECK(fs >= 43659u);                     // within 1 % of the 44.1 kHz that CHIP_CLK_CTRL's SYS_FS says
    CHECK(fs <= 44541u);
  }
  CHECK(sg::kMclkHz == haudio::mclk_hz(haudio::kMclkRatio));
  CHECK(sg::kClkCtrlValue == 0x0004);        // SYS_FS = 1 (44.1 kHz), MCLK_FREQ = 0 (256 Fs)
  CHECK(sg::kHpUnmuteCode >= sg::kHpLoudestAllowedCode);
  CHECK(sg::kHpUnmuteCode == 0x40);
}

// ---- The analogue input (OS-011c) ------------------------------------------------------------------------------------------------------------

TEST_CASE("sgtl5000: the line gain is gain * 15 / 100 steps, the same in both ADC channels") {
  CHECK(sg::line_adc_steps(0) == 0);
  CHECK(sg::line_adc_steps(100) == 15);
  CHECK(sg::line_adc_steps(50) == 7);
  CHECK(sg::line_adc_steps(7) == 1);    // 105 / 100
  CHECK(sg::line_adc_steps(200) == 15);  // clamped
  CHECK(sg::adc_ctrl_value(0) == 0x0000);
  CHECK(sg::adc_ctrl_value(5) == 0x0055);   // a line-in value
  CHECK(sg::adc_ctrl_value(8) == 0x0088);   // a microphone value
  CHECK(sg::adc_ctrl_value(15) == 0x00FF);
  CHECK(sg::adc_ctrl_value(99) == 0x00FF);
}

TEST_CASE("sgtl5000: the microphone gain is a preamp step plus ADC steps of 1.5 dB") {
  // row by row.
  struct Row {
    unsigned db;
    unsigned preamp;
    unsigned adc;
  };
  const Row rows[] = {{0, 0, 0}, {1, 0, 0}, {2, 0, 1}, {19, 0, 12}, {20, 1, 0}, {25, 1, 3}, {29, 1, 6}, {30, 2, 0}, {39, 2, 6}, {40, 3, 0}, {44, 3, 2}, {99, 3, 15}};
  for (const Row& row : rows) {
    const sg::MicSetting m = sg::mic_setting(row.db);
    CHECK_MESSAGE(m.preamp == row.preamp, "dB " << row.db);
    CHECK_MESSAGE(m.adc == row.adc, "dB " << row.db);
  }
  // The recorder's percentages: the low range 0..25 dB, the high one 0..44 dB.
  CHECK(sg::mic_gain_db(false, 0) == 0);
  CHECK(sg::mic_gain_db(false, 100) == 25);
  CHECK(sg::mic_gain_db(false, 50) == 12);
  CHECK(sg::mic_gain_db(true, 100) == 44);
  CHECK(sg::mic_gain_db(true, 50) == 22);
  CHECK(sg::mic_gain_db(true, 250) == 44);
}

TEST_CASE("sgtl5000: a source switch writes the level before the select, composes CHIP_ANA_CTRL with the output mutes, and never touches them") {
  for (bool muted : {true, false}) {
    const sg::InputPlan line = sg::line_input_plan(50, muted);
    CHECK(line.count == 2);
    CHECK(line.step[0].reg == sg::kAnaAdcCtrl);
    CHECK(line.step[0].value == 0x0077);
    CHECK(line.step[1].reg == sg::kAnaCtrl);
    CHECK((line.step[1].value & sg::kAnaCtrlSelectLine) != 0u);
    CHECK(line.line);
    const sg::InputPlan mic = sg::mic_input_plan(true, 100, muted);
    CHECK(mic.count == 3);
    CHECK(mic.step[0].reg == sg::kMicCtrl);
    CHECK(mic.step[0].value == (sg::kMicCtrlBase | 3u));  // 44 dB: the +40 dB preamp
    CHECK(mic.step[1].reg == sg::kAnaAdcCtrl);
    CHECK(mic.step[1].value == 0x0022);                   // the remaining 4 dB: 2 steps
    CHECK(mic.step[2].reg == sg::kAnaCtrl);
    CHECK((mic.step[2].value & sg::kAnaCtrlSelectLine) == 0u);
    CHECK_FALSE(mic.line);
    // The two writes of ANA_CTRL differ in bit 2 and nothing else, and carry the mutes of the state they were composed for.
    CHECK((line.step[1].value ^ mic.step[2].value) == sg::kAnaCtrlSelectLine);
    for (const sg::InputPlan* p : {&line, &mic}) {
      const std::uint16_t v = p->step[p->count - 1].value;
      const bool both = (v & (sg::kAnaCtrlMuteLineOut | sg::kAnaCtrlMuteHeadphone)) == (sg::kAnaCtrlMuteLineOut | sg::kAnaCtrlMuteHeadphone);
      const bool none = (v & (sg::kAnaCtrlMuteLineOut | sg::kAnaCtrlMuteHeadphone)) == 0u;
      CHECK(muted ? both : none);
      CHECK((v & sg::kAnaCtrlMuteAdc) == 0u);  // the ADC stays unmuted: the recorder listens
    }
  }
  // The gain-only plans have no select.
  const sg::InputPlan lg = sg::line_gain_plan(100);
  CHECK(lg.count == 1);
  CHECK(lg.step[0].reg == sg::kAnaAdcCtrl);
  const sg::InputPlan mg = sg::mic_gain_plan(false, 100);
  CHECK(mg.count == 2);
  CHECK(mg.step[0].value == (sg::kMicCtrlBase | 1u));  // 25 dB: +20 preamp, 5 dB = 3 steps
  CHECK(mg.step[1].value == 0x0033);
}

TEST_CASE("sgtl5000: the unmute and mute plans keep the ADC's input select the recorder chose") {
  // The ANA_CTRL words the plans write, with the select forced the way audio.cpp does it.
  for (bool line : {true, false}) {
    sg::Step step{};
    for (unsigned i = 0; sg::unmute_step(i, step); ++i) {
      if (step.reg == sg::kAnaCtrl) {
        const std::uint16_t v = sg::ana_ctrl_with_adc(step.value, line);
        CHECK(((v & sg::kAnaCtrlSelectLine) != 0u) == line);
        CHECK((v & ~sg::kAnaCtrlSelectLine) == (step.value & ~sg::kAnaCtrlSelectLine));
      }
    }
    for (const sg::Step& m : sg::kMute) {
      if (m.reg == sg::kAnaCtrl) {
        CHECK(((sg::ana_ctrl_with_adc(m.value, line) & sg::kAnaCtrlSelectLine) != 0u) == line);
      }
    }
  }
  CHECK(sg::ana_ctrl_value(true, true) == sg::kAnaCtrlOutputsMuted);
  CHECK(sg::ana_ctrl_value(false, true) == sg::kAnaCtrlOutputsUnmuted);
}
