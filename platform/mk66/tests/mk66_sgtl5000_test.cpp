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

// ---- OS-MASTEREQ: the master EQ and bass boost in the DAP -------------------------------------------------------------------------------------
namespace {

// The DAP registers and the two the driver also writes (the routing and the DAC mute), with the log of every write and the checks the data sheet's click guidance asks for.
struct DapModel {
  std::uint16_t sss = sg::kSssIdle;
  std::uint16_t adcdac = sg::kAdcDacUnmuted;  // the outputs are open when the live driver runs
  std::uint16_t dap_ctrl = 0;
  std::uint16_t dap_eq = 0;
  std::uint16_t band[5] = {sg::kGeqZeroDb, sg::kGeqZeroDb, sg::kGeqZeroDb, sg::kGeqZeroDb, sg::kGeqZeroDb};
  std::uint16_t bass_en = sg::kBassEnhanceOff;
  std::uint16_t bass_ctrl = sg::bass_ctrl_value(sg::kBassLevelOff);
  unsigned writes = 0;
  unsigned fail_at = 0;   // 1-based write number that is not acknowledged (0: none)
  unsigned fail_at2 = 0;  // a second one (0: none): a double failure
  unsigned max_band_move = 0;
  unsigned max_bass_move = 0;
  bool en_toggled_off_zero = false;  // BASS_ENABLE changed while BASS_LEVEL was not 0x7F
  bool unknown = false;
  std::uint16_t log_reg[96] = {};
  std::uint16_t log_val[96] = {};

  bool write(const sg::Step& s) {
    ++writes;
    if (writes <= 96u) {
      log_reg[writes - 1u] = s.reg;
      log_val[writes - 1u] = s.value;
    }
    if ((fail_at != 0u && writes == fail_at) || (fail_at2 != 0u && writes == fail_at2)) {
      return false;
    }
    switch (s.reg) {
      case sg::kSssCtrl: sss = s.value; break;
      case sg::kAdcDacCtrl: adcdac = s.value; break;
      case sg::kDapControl: dap_ctrl = s.value; break;
      case sg::kDapAudioEq: dap_eq = s.value; break;
      case sg::kDapBassEnhance:
        // Only while the DAP feeds the DAC: a route in sets the bass registers before the routing write, when nothing of the DAP is audible.
        if (sss == sg::kSssDap && ((s.value ^ bass_en) & 1u) != 0u && (bass_ctrl & 0x7Fu) != sg::kBassLevelOff) {
          en_toggled_off_zero = true;
        }
        bass_en = s.value;
        break;
      case sg::kDapBassEnhanceCtrl: {
        const unsigned a = bass_ctrl & 0x7Fu;
        const unsigned b = s.value & 0x7Fu;
        const unsigned d = a > b ? a - b : b - a;
        max_bass_move = d > max_bass_move ? d : max_bass_move;
        bass_ctrl = s.value;
        break;
      }
      default: {
        bool known = false;
        for (unsigned i = 0; i < 5; ++i) {
          if (s.reg == sg::kDapGeqBand[i]) {
            known = true;
            const unsigned d = band[i] > s.value ? band[i] - s.value : s.value - band[i];
            max_band_move = d > max_band_move ? d : max_band_move;
            band[i] = s.value;
          }
        }
        if (!known) {
          unknown = true;
        }
        break;
      }
    }
    return true;
  }
};

haudio::OutputEq eq_of(int b0, int b1, int b2, int b3, int b4, unsigned bass) {
  haudio::OutputEq e{};
  e.band_db[0] = static_cast<std::int8_t>(b0);
  e.band_db[1] = static_cast<std::int8_t>(b1);
  e.band_db[2] = static_cast<std::int8_t>(b2);
  e.band_db[3] = static_cast<std::int8_t>(b3);
  e.band_db[4] = static_cast<std::int8_t>(b4);
  e.bass = static_cast<std::uint8_t>(bass);
  return e;
}

// The driver's write: the model behind a function pointer and a context.
sg::Writer eq_writer(DapModel& m) {
  return sg::Writer{[](void* ctx, const sg::Step& s) noexcept { return static_cast<DapModel*>(ctx)->write(s); }, &m};
}

// One pass at `now`; returns how many writes it made.
unsigned eq_pass(sg::EqDriver& d, DapModel& m, std::uint32_t now) {
  const unsigned before = m.writes;
  d.pump(now, eq_writer(m));
  return m.writes - before;
}

// Passes every millisecond until the driver is Routed (or Failed) and has nothing left to write; returns the last time used. No pass writes more than kWritesPerPass.
std::uint32_t eq_settle(sg::EqDriver& d, DapModel& m, std::uint32_t now, unsigned max_passes = 4000) {
  for (unsigned i = 0; i < max_passes; ++i, ++now) {
    const unsigned n = eq_pass(d, m, now);
    CHECK(n <= sg::EqDriver::kWritesPerPass);
    if (n == 0u && d.phase() != sg::EqDriver::Phase::RouteIn) {
      break;
    }
  }
  return now;
}

}  // namespace

TEST_CASE("master EQ: the register codes (0.25 dB per LSB around 0x2F, bass level 0x7F down to 0x19)") {
  CHECK(sg::geq_code(0) == 0x2F);
  CHECK(sg::geq_code(1) == 0x33);
  CHECK(sg::geq_code(12) == 0x5F);
  CHECK(sg::geq_code(-1) == 0x2B);
  CHECK(sg::geq_code(-12) == 0x00);  // -11.75 dB: the floor
  CHECK(sg::geq_code(-100) == 0x00);
  CHECK(sg::geq_code(100) == 0x5F);
  CHECK(sg::bass_level_code(0) == 0x7F);
  CHECK(sg::bass_level_code(1) == 0x7E);
  CHECK(sg::bass_level_code(100) == 0x19);
  CHECK(sg::bass_level_code(255) == 0x19);
  for (unsigned v = 1; v <= 100; ++v) {
    CHECK(sg::bass_level_code(v) <= sg::bass_level_code(v - 1u));
  }
  CHECK(sg::bass_ctrl_value(0x19) == 0x0B19);
  CHECK(sg::eq_is_default(eq_of(0, 0, 0, 0, 0, 0)));
  CHECK_FALSE(sg::eq_is_default(eq_of(0, 0, 0, 0, 1, 0)));
  CHECK_FALSE(sg::eq_is_default(eq_of(0, 0, 0, 0, 0, 1)));
  CHECK(sg::eq_equal(eq_of(1, 2, 3, 4, 5, 6), eq_of(1, 2, 3, 4, 5, 6)));
  CHECK_FALSE(sg::eq_equal(eq_of(1, 2, 3, 4, 5, 6), eq_of(1, 2, 3, 4, 5, 7)));
}

TEST_CASE("master EQ: every value at its default writes nothing, with the outputs open or at unmute") {
  sg::EqDriver d;
  DapModel m;
  d.on_start();
  d.set(eq_of(0, 0, 0, 0, 0, 0));
  for (std::uint32_t t = 0; t < 500; ++t) {
    CHECK(eq_pass(d, m, t) == 0u);
  }
  d.apply_at_unmute(eq_writer(m));
  CHECK(m.writes == 0u);
  CHECK(d.phase() == sg::EqDriver::Phase::Idle);
  CHECK(m.sss == sg::kSssIdle);
}

TEST_CASE("master EQ: the first non-default value routes in: soft mute, DAP, EQ select, bands, bass, routing, unmute, waited by timestamp") {
  sg::EqDriver d;
  DapModel m;
  d.on_start();
  d.set(eq_of(3, -2, 0, 12, -12, 40));
  // pass 1: the DAC soft mute alone, then its 50 ms ramp is waited (not blocked on)
  CHECK(eq_pass(d, m, 1000) == 1u);
  CHECK(m.log_reg[0] == sg::kAdcDacCtrl);
  CHECK(m.log_val[0] == sg::kAdcDacMuted);
  CHECK(eq_pass(d, m, 1001) == 0u);
  CHECK(eq_pass(d, m, 1049) == 0u);
  // from 1050: the DAP steps, at most 3 a pass
  CHECK(eq_pass(d, m, 1050) == 3u);
  CHECK(eq_pass(d, m, 1051) == 3u);
  CHECK(eq_pass(d, m, 1052) == 3u);
  CHECK(eq_pass(d, m, 1053) == 2u);  // the routing, then the DAC unmute (which waits its ramp)
  CHECK(d.phase() == sg::EqDriver::Phase::Routed);
  CHECK(m.writes == 12u);
  const std::uint16_t expect_reg[12] = {sg::kAdcDacCtrl,    sg::kDapControl,    sg::kDapAudioEq,     sg::kDapGeqBand[0], sg::kDapGeqBand[1], sg::kDapGeqBand[2],
                                        sg::kDapGeqBand[3], sg::kDapGeqBand[4], sg::kDapBassEnhanceCtrl, sg::kDapBassEnhance, sg::kSssCtrl,    sg::kAdcDacCtrl};
  for (unsigned i = 0; i < 12; ++i) {
    CHECK(m.log_reg[i] == expect_reg[i]);
  }
  CHECK(m.log_val[11] == sg::kAdcDacUnmuted);
  CHECK(m.log_val[1] == 1u);
  CHECK(m.log_val[2] == 3u);
  CHECK(m.log_val[3] == sg::geq_code(3));
  CHECK(m.log_val[7] == 0x00);
  CHECK(m.log_val[8] == sg::bass_ctrl_value(sg::bass_level_code(40)));
  CHECK(m.log_val[9] == sg::kBassEnhanceOn);
  CHECK(m.log_val[10] == sg::kSssDap);
  CHECK(m.sss == sg::kSssDap);
  CHECK(m.adcdac == sg::kAdcDacUnmuted);
  CHECK_FALSE(m.unknown);
  CHECK(d.routed());
  // settled: nothing more is written
  for (std::uint32_t t = 2000; t < 2100; ++t) {
    CHECK(eq_pass(d, m, t) == 0u);
  }
}

TEST_CASE("master EQ: a route in with a flat EQ and bass only enables the bass, bands stay at 0 dB") {
  sg::EqDriver d;
  DapModel m;
  d.on_start();
  d.set(eq_of(0, 0, 0, 0, 0, 100));
  eq_settle(d, m, 0);
  CHECK(d.phase() == sg::EqDriver::Phase::Routed);
  for (unsigned i = 0; i < 5; ++i) {
    CHECK(m.band[i] == sg::kGeqZeroDb);
  }
  CHECK(m.bass_en == sg::kBassEnhanceOn);
  CHECK((m.bass_ctrl & 0x7Fu) == 0x19u);
  CHECK((m.bass_ctrl >> 8) == sg::kBassLrLevel);
  CHECK_FALSE(m.en_toggled_off_zero);
}

TEST_CASE("master EQ: a live band change moves at most 2 LSB (0.5 dB) a write and at most 3 writes a pass") {
  sg::EqDriver d;
  DapModel m;
  d.on_start();
  d.set(eq_of(1, 0, 0, 0, 0, 0));
  std::uint32_t t = eq_settle(d, m, 0);
  m.max_band_move = 0;
  d.set(eq_of(12, -12, 12, -12, 12, 0));
  for (unsigned i = 0; i < 200; ++i, ++t) {
    CHECK(eq_pass(d, m, t) <= 3u);
  }
  CHECK(m.max_band_move <= 2u);
  CHECK(m.band[0] == sg::geq_code(12));
  CHECK(m.band[1] == sg::geq_code(-12));
  CHECK(m.band[2] == sg::geq_code(12));
  CHECK(m.band[3] == sg::geq_code(-12));
  CHECK(m.band[4] == sg::geq_code(12));
  CHECK(m.sss == sg::kSssDap);          // an edit never routes out
  CHECK(m.adcdac == sg::kAdcDacUnmuted);  // nor mutes the DAC
  // a step of one dB (4 LSB) takes two writes
  const unsigned before = m.writes;
  d.set(eq_of(11, -12, 12, -12, 12, 0));
  for (unsigned i = 0; i < 5; ++i, ++t) {
    (void)eq_pass(d, m, t);
  }
  CHECK(m.writes - before == 2u);
}

TEST_CASE("master EQ: the bass enable changes only with BASS_LEVEL at 0x7F, the level in bounded steps") {
  sg::EqDriver d;
  DapModel m;
  d.on_start();
  d.set(eq_of(2, 0, 0, 0, 0, 0));
  std::uint32_t t = eq_settle(d, m, 0);
  CHECK(m.bass_en == sg::kBassEnhanceOff);
  m.max_bass_move = 0;
  d.set(eq_of(2, 0, 0, 0, 0, 100));
  for (unsigned i = 0; i < 100; ++i, ++t) {
    CHECK(eq_pass(d, m, t) <= 3u);
  }
  CHECK(m.bass_en == sg::kBassEnhanceOn);
  CHECK((m.bass_ctrl & 0x7Fu) == 0x19u);
  CHECK(m.max_bass_move <= static_cast<unsigned>(sg::EqDriver::kBassStep));
  d.set(eq_of(2, 0, 0, 0, 0, 30));
  for (unsigned i = 0; i < 100; ++i, ++t) {
    (void)eq_pass(d, m, t);
  }
  CHECK(m.bass_en == sg::kBassEnhanceOn);
  CHECK((m.bass_ctrl & 0x7Fu) == sg::bass_level_code(30));
  d.set(eq_of(2, 0, 0, 0, 0, 0));
  for (unsigned i = 0; i < 100; ++i, ++t) {
    (void)eq_pass(d, m, t);
  }
  CHECK(m.bass_en == sg::kBassEnhanceOff);
  CHECK((m.bass_ctrl & 0x7Fu) == sg::kBassLevelOff);
  CHECK_FALSE(m.en_toggled_off_zero);
  CHECK(m.sss == sg::kSssDap);
}

TEST_CASE("master EQ: values set while the outputs are muted are applied inside unmute, the DAC staying muted by the caller") {
  sg::EqDriver d;
  DapModel m;
  m.adcdac = sg::kAdcDacMuted;
  d.on_start();
  d.set(eq_of(-3, 0, 5, 0, 0, 10));
  CHECK(d.phase() == sg::EqDriver::Phase::Idle);  // recorded only
  CHECK(m.writes == 0u);
  d.apply_at_unmute(eq_writer(m));
  CHECK(m.writes == sg::kEqDapSteps);
  CHECK(m.log_reg[0] == sg::kDapControl);
  CHECK(m.log_reg[9] == sg::kSssCtrl);  // the routing is the last write
  CHECK(m.band[0] == sg::geq_code(-3));
  CHECK(m.band[2] == sg::geq_code(5));
  CHECK(m.adcdac == sg::kAdcDacMuted);  // the driver did not touch the DAC: the unmute plan does
  CHECK(d.phase() == sg::EqDriver::Phase::Routed);
  CHECK(d.routed());
  CHECK_FALSE(m.unknown);
}

TEST_CASE("master EQ: route out happens only at unmute, never on a live edit; a project load with all defaults stays routed and flat") {
  sg::EqDriver d;
  DapModel m;
  d.on_start();
  d.set(eq_of(4, 0, 0, 0, 0, 20));
  std::uint32_t t = eq_settle(d, m, 0);
  d.set(eq_of(0, 0, 0, 0, 0, 0));  // a new project: flat
  for (unsigned i = 0; i < 300; ++i, ++t) {
    (void)eq_pass(d, m, t);
  }
  CHECK(m.sss == sg::kSssDap);
  CHECK(m.dap_ctrl == 1u);
  for (unsigned i = 0; i < 5; ++i) {
    CHECK(m.band[i] == sg::kGeqZeroDb);
  }
  CHECK(m.bass_en == sg::kBassEnhanceOff);
  CHECK(d.routed());
  // the outputs close and open again: now it goes back to the idle state, the routing first
  d.on_mute();
  const unsigned before = m.writes;
  m.adcdac = sg::kAdcDacMuted;
  d.apply_at_unmute(eq_writer(m));
  CHECK(m.writes - before == 2u);
  CHECK(m.log_reg[before] == sg::kSssCtrl);
  CHECK(m.log_val[before] == sg::kSssIdle);
  CHECK(m.log_reg[before + 1u] == sg::kDapControl);
  CHECK(m.log_val[before + 1u] == 0u);
  CHECK_FALSE(d.routed());
  // and a second unmute with all defaults writes nothing
  const unsigned again = m.writes;
  d.on_mute();
  d.apply_at_unmute(eq_writer(m));
  CHECK(m.writes == again);
  // a restart forgets the routing: the power-up plan puts the idle value back
  d.set(eq_of(1, 0, 0, 0, 0, 0));
  d.apply_at_unmute(eq_writer(m));
  CHECK(d.routed());
  d.on_start();
  CHECK_FALSE(d.routed());
}

TEST_CASE("master EQ: a cut short route in (mute) is redone from the start inside the next unmute") {
  sg::EqDriver d;
  DapModel m;
  d.on_start();
  d.set(eq_of(6, 0, 0, 0, 0, 0));
  (void)eq_pass(d, m, 0);  // the soft mute
  (void)eq_pass(d, m, 50);  // some DAP steps
  CHECK(d.phase() == sg::EqDriver::Phase::RouteIn);
  d.on_mute();
  m.adcdac = sg::kAdcDacMuted;
  const unsigned before = m.writes;
  d.apply_at_unmute(eq_writer(m));
  CHECK(m.writes - before == sg::kEqDapSteps);
  CHECK(d.phase() == sg::EqDriver::Phase::Routed);
  CHECK(m.band[0] == sg::geq_code(6));
}

TEST_CASE("master EQ: a write that is not acknowledged abandons the route in, restores the idle routing, ends with the DAC unmuted, and is retried only after a value change") {
  for (unsigned fail = 1; fail <= 12; ++fail) {
    sg::EqDriver d;
    DapModel m;
    d.on_start();
    d.set(eq_of(5, 0, 0, 0, 0, 10));
    m.fail_at = fail;
    std::uint32_t t = eq_settle(d, m, 0);  // fail 4 is the third write of a pass: no pass goes over kWritesPerPass
    CAPTURE(fail);
    CHECK(d.phase() == sg::EqDriver::Phase::Failed);
    CHECK(m.sss == sg::kSssIdle);
    CHECK(m.adcdac == sg::kAdcDacUnmuted);
    CHECK_FALSE(d.routed());
    // never retried every pass
    const unsigned stuck = m.writes;
    m.fail_at = 0;
    for (unsigned i = 0; i < 200; ++i, ++t) {
      CHECK(eq_pass(d, m, t) == 0u);
    }
    CHECK(m.writes == stuck);
    // one retry on the next value change
    d.set(eq_of(6, 0, 0, 0, 0, 10));
    t = eq_settle(d, m, t);
    CHECK(d.phase() == sg::EqDriver::Phase::Routed);
    CHECK(m.sss == sg::kSssDap);
    CHECK(m.band[0] == sg::geq_code(6));
    CHECK(m.adcdac == sg::kAdcDacUnmuted);
  }
}

TEST_CASE("master EQ: a failed write inside unmute leaves the idle routing and the outputs can still open") {
  for (unsigned fail = 1; fail <= sg::kEqDapSteps; ++fail) {
    sg::EqDriver d;
    DapModel m;
    m.adcdac = sg::kAdcDacMuted;
    d.on_start();
    d.set(eq_of(0, 7, 0, 0, 0, 0));
    m.fail_at = fail;
    d.apply_at_unmute(eq_writer(m));
    CAPTURE(fail);
    CHECK(d.phase() == sg::EqDriver::Phase::Failed);
    CHECK(m.sss == sg::kSssIdle);
    CHECK(m.adcdac == sg::kAdcDacMuted);  // the caller's unmute plan unmutes
  }
}

TEST_CASE("master EQ: the plans of power-up, unmute, mute and stop do not write a DAP register or the DAP routing") {
  for (const sg::Step& s : sg::kMute) {
    CHECK(s.reg < sg::kDapControl);
    CHECK(s.reg != sg::kSssCtrl);
  }
  sg::Step step{};
  for (unsigned i = 0; sg::unmute_step(i, step); ++i) {
    CHECK(step.reg < sg::kDapControl);
    CHECK(step.reg != sg::kSssCtrl);
  }
  for (const sg::Step& s : sg::kPowerDown) {
    CHECK(s.reg < sg::kDapControl);
    CHECK(s.reg != sg::kSssCtrl);
  }
  // the power-up plan leaves the routing at the idle value the driver assumes
  std::uint16_t last_sss = 0xFFFF;
  for (const sg::Step& s : sg::kPowerUp) {
    if (s.reg == sg::kSssCtrl) {
      last_sss = s.value;
    }
  }
  CHECK(last_sss == sg::kSssIdle);
}

namespace {
// FNV-1a over the bytes of reg, value and wait of each step: pins the whole content of a plan, not only its registers.
std::uint32_t plan_hash(std::uint32_t h, const sg::Step& s) {
  for (const unsigned v : {static_cast<unsigned>(s.reg), static_cast<unsigned>(s.value), static_cast<unsigned>(s.wait_ms)}) {
    h = (h ^ (v & 0xFFu)) * 16777619u;
    h = (h ^ (v >> 8)) * 16777619u;
  }
  return h;
}
}  // namespace

TEST_CASE("master EQ: the existing plans are unchanged, register, value and wait of every step (hashes taken from the tree before OS-MASTEREQ, b9e29ad7)") {
  constexpr std::uint32_t kBasis = 2166136261u;
  std::uint32_t h = kBasis;
  for (const sg::Step& s : sg::kPowerUp) {
    h = plan_hash(h, s);
  }
  CHECK(h == 0x9D839639u);
  h = kBasis;
  for (const sg::Step& s : sg::kMute) {
    h = plan_hash(h, s);
  }
  CHECK(h == 0xBC778500u);
  h = kBasis;
  sg::Step step{};
  for (unsigned i = 0; sg::unmute_step(i, step); ++i) {
    h = plan_hash(h, step);
  }
  CHECK(h == 0x4C523417u);
  h = kBasis;
  for (const sg::Step& s : sg::kPowerDown) {
    h = plan_hash(h, s);
  }
  CHECK(h == 0x797EF421u);
  CHECK(plan_hash(kBasis, sg::kSoftMute) == 0x15D79B57u);
}

TEST_CASE("master EQ: a route out whose routing write is not acknowledged does not switch the DAP off (the DAC is never fed from a disabled DAP)") {
  sg::EqDriver d;
  DapModel m;
  m.adcdac = sg::kAdcDacMuted;
  d.on_start();
  d.set(eq_of(5, 0, 0, 0, 0, 10));
  d.apply_at_unmute(eq_writer(m));
  CHECK(d.phase() == sg::EqDriver::Phase::Routed);
  CHECK(m.sss == sg::kSssDap);
  CHECK(m.dap_ctrl == sg::kDapEnabled);
  d.on_mute();
  d.set(eq_of(0, 0, 0, 0, 0, 0));
  m.fail_at = m.writes + 1u;  // the routing write
  const unsigned before = m.writes;
  d.apply_at_unmute(eq_writer(m));
  CHECK(m.writes - before == 1u);  // no DAP-off write after the failed routing write
  CHECK(m.sss == sg::kSssDap);
  CHECK(m.dap_ctrl == sg::kDapEnabled);
  CHECK(d.routed());
  // the next unmute does the whole route out
  m.fail_at = 0;
  d.apply_at_unmute(eq_writer(m));
  CHECK(m.sss == sg::kSssIdle);
  CHECK(m.dap_ctrl == 0u);
  CHECK_FALSE(d.routed());
}

TEST_CASE("master EQ: a failed live route in whose DAC unmute is also not acknowledged retries the unmute, one write a pass, until the DAC is open") {
  sg::EqDriver d;
  DapModel m;
  d.on_start();
  d.set(eq_of(5, 0, 0, 0, 0, 10));
  m.fail_at = 5;   // a DAP step of the route in
  m.fail_at2 = 6;  // the DAC unmute of the recovery
  std::uint32_t t = 0;
  while (d.phase() != sg::EqDriver::Phase::Failed && t < 1000u) {
    (void)eq_pass(d, m, t++);
  }
  CHECK(d.phase() == sg::EqDriver::Phase::Failed);
  CHECK(m.adcdac == sg::kAdcDacMuted);              // the soft mute of the route in is still there
  CHECK(eq_pass(d, m, t++) == 1u);                  // the recovery unmute: not acknowledged
  CHECK(m.adcdac == sg::kAdcDacMuted);
  CHECK(eq_pass(d, m, t++) == 1u);                  // retried: acknowledged
  CHECK(m.adcdac == sg::kAdcDacUnmuted);
  CHECK(eq_pass(d, m, t++) == 0u);                  // and no more
  CHECK(d.phase() == sg::EqDriver::Phase::Failed);
  CHECK_FALSE(d.routed());
}

TEST_CASE("master EQ: a live edit after 2^31 ms of uptime without an edit is not held by the ramp wait compare") {
  sg::EqDriver d;
  DapModel m;
  d.on_start();
  d.set(eq_of(5, 0, 0, 0, 0, 0));
  std::uint32_t t = eq_settle(d, m, 0) + 2 * sg::kDacRampWaitMs;
  (void)eq_pass(d, m, t);  // a pass after the wait: the pending wait is over
  CHECK(d.phase() == sg::EqDriver::Phase::Routed);
  t += 0x80000001u;  // a signed compare against the old ready_ would now read as "not yet" for 24.9 days
  d.set(eq_of(9, 0, 0, 0, 0, 0));
  t = eq_settle(d, m, t);
  CHECK(m.band[0] == sg::geq_code(9));
}

TEST_CASE("master EQ: a failure while stepping a live edit stops, and a value change steps on (a reset to flat steps the bands and the bass back)") {
  sg::EqDriver d;
  DapModel m;
  d.on_start();
  d.set(eq_of(5, 0, 0, 0, 0, 10));
  std::uint32_t t = eq_settle(d, m, 0) + 2 * sg::kDacRampWaitMs;  // past the DAC unmute's ramp wait, which also holds the live stepping
  CHECK(d.phase() == sg::EqDriver::Phase::Routed);
  d.set(eq_of(9, 3, 0, 0, 0, 10));
  m.fail_at = m.writes + 2u;
  t = eq_settle(d, m, t);
  CHECK(d.phase() == sg::EqDriver::Phase::Failed);
  CHECK(d.routed());
  const unsigned stuck = m.writes;
  m.fail_at = 0;
  for (unsigned i = 0; i < 200; ++i, ++t) {
    CHECK(eq_pass(d, m, t) == 0u);
  }
  CHECK(m.writes == stuck);
  d.set(eq_of(0, 0, 0, 0, 0, 0));
  CHECK(d.phase() == sg::EqDriver::Phase::Routed);  // the DAP is still routed: the registers are stepped back, not a new route in
  t = eq_settle(d, m, t);
  CHECK(d.phase() == sg::EqDriver::Phase::Routed);
  for (unsigned i = 0; i < 5; ++i) {
    CHECK(m.band[i] == sg::kGeqZeroDb);
  }
  CHECK((m.bass_ctrl & 0x7Fu) == sg::kBassLevelOff);
  CHECK(m.bass_en == sg::kBassEnhanceOff);
  CHECK(m.sss == sg::kSssDap);
  CHECK_FALSE(m.en_toggled_off_zero);
}
