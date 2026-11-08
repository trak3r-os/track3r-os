// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio HAL contract's pure parts (hal/audio.h, hal/irq.h; OS-005a): the one rate constant as a rational number and
// everything derived from it (the I2S divider, MCLK, the block length in cycles, the USB feedback value), for the 4080/1 clock of
// today and the 44,100 Hz retune of D-OS-8 (1800000/441), and the interrupt priority table as data. Runs on the host and on the
// Cortex-M4F (`qemu_audio`).
#include <doctest/doctest.h>

#include <o3os/audio/master_limiter.h>
#include <o3os/audio/master_scratch.h>
#include <o3os/audio/svf.h>
#include <o3os/audio/voices.h>
#include <o3os/core/audio_clock.h>
#include <o3os/hal/audio.h>
#include <o3os/hal/irq.h>
#include <o3os/usb/rate.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace haudio = o3os::hal::audio;
namespace irq = o3os::hal::irq;
namespace usb = o3os::usb;

namespace {
constexpr haudio::CyclesPerSample kToday{4080u, 1u};
constexpr haudio::CyclesPerSample kRetune{1800000u, 441u};  // D-OS-8: 180 MHz x 196/3125 = 256 x 44,100 Hz
}  // namespace

TEST_CASE("audio rate: one constant, owned by core: hal and usb both see it, neither copies it") {
  CHECK(haudio::kCyclesPerSample.num == o3os::core::kRefCyclesPerSample.num);
  CHECK(haudio::kCyclesPerSample.den == o3os::core::kRefCyclesPerSample.den);
  CHECK(usb::kRefCyclesPerSample.num == o3os::core::kRefCyclesPerSample.num);
  CHECK(usb::kRefCyclesPerSample.den == o3os::core::kRefCyclesPerSample.den);
  CHECK(haudio::kCpuHz == o3os::core::kCpuHz);
  CHECK(usb::kCpuHz == o3os::core::kCpuHz);
  CHECK(haudio::kCpuHz == 180000000u);
  // The build's choice (-DO3OS_RATE_44100): 4080/1, or 1800000/441 for the D-OS-8 retune image, and nothing else.
#if defined(O3OS_RATE_44100) && O3OS_RATE_44100
  CHECK(haudio::kCyclesPerSample.num == 1800000u);
  CHECK(haudio::kCyclesPerSample.den == 441u);
#else
  CHECK(haudio::kCyclesPerSample.num == 4080u);
  CHECK(haudio::kCyclesPerSample.den == 1u);
#endif
}

TEST_CASE("audio rate: the I2S divider follows from the constant, for today's clock and the 44,100 Hz retune") {
  // 4080 cycles per sample = 256 MCLKs of 15.9375 cycles: FRACT / DIVIDE = 16 / 255 at 180 MHz (this OS's own rate; the official firmware's is an open research question, docs/hw/tracker-hardware-notes.md).
  const haudio::MclkRatio today = haudio::mclk_ratio(kToday);
  CHECK(today.fract == 16u);
  CHECK(today.divide == 255u);
  CHECK(haudio::mclk_hz(today) == 11294117u);            // 180 MHz x 16 / 255, rounded down
  CHECK(haudio::sample_rate_mhz(kToday) == 44117647u);   // 44,117.647 Hz

  // The retune: 180 MHz x 196/3125 = 11,289,600 Hz = 256 x 44,100 exactly (ADR 0012 amendment 2).
  const haudio::MclkRatio retune = haudio::mclk_ratio(kRetune);
  CHECK(retune.fract == 196u);
  CHECK(retune.divide == 3125u);
  CHECK(haudio::mclk_hz(retune) == 11289600u);
  CHECK(haudio::mclk_hz(retune) == 256u * 44100u);
  CHECK(haudio::sample_rate_mhz(kRetune) == 44100000u);

  // The build's own constant is the 4080/1 one until D-OS-8's hardware trial says otherwise.
  CHECK(haudio::kMclkRatio.fract == (haudio::kCyclesPerSample.num == 4080u ? 16u : 196u));
}

TEST_CASE("audio rate: the block in cycles") {
  CHECK(haudio::cycles_per_block(kToday) == 261120u);    // 64 x 4080
  CHECK(haudio::cycles_per_block(kRetune) == 261224u);   // 64 x 4081.63 rounded
  CHECK(haudio::kCyclesPerBlock == haudio::cycles_per_block());
  // 1.45 ms at either rate.
  CHECK(haudio::kCyclesPerBlock / (haudio::kCpuHz / 1000u) == 1u);
  CHECK(haudio::kCyclesPerBlock > 261000u);
}

TEST_CASE("audio rate: the divider is exact for every representable rate, and refuses the rest") {
  // fract / divide = 256 x den / num, reduced: cross-multiplied it is exact, so MCLK = cpu x fract / divide has a block of exactly
  // `num / den` cycles per sample.
  std::uint32_t representable = 0;
  for (std::uint32_t num = 300; num <= 9000; num += 7) {
    for (std::uint32_t den : {1u, 3u, 441u}) {
      const haudio::CyclesPerSample rate{num, den};
      const haudio::MclkRatio r = haudio::mclk_ratio(rate);
      if (r.fract == 0u) {
        // Refused for one of the three reasons: FRACT out of 1..256, DIVIDE out of 1..4096, FRACT above DIVIDE (MCLK above the
        // source).
        continue;
      }
      ++representable;
      CHECK(static_cast<std::uint64_t>(r.fract) * num == static_cast<std::uint64_t>(haudio::kMclkPerSample) * den * r.divide);
      CHECK(r.fract >= 1u);
      CHECK(r.fract <= 256u);
      CHECK(r.divide >= 1u);
      CHECK(r.divide <= 4096u);
      CHECK(r.fract <= r.divide);
    }
  }
  CHECK(representable > 100u);
  CHECK(haudio::mclk_ratio({100u, 1u}).fract == 0u);    // 2.56 cycles per MCLK: MCLK above the source
  CHECK(haudio::mclk_ratio({4099u, 1u}).fract == 0u);   // 4099 is prime: DIVIDE would be 4099 > 4096
}

TEST_CASE("audio rate: the USB feedback value follows from the same constant") {
  // ADR 0012 amendment 2: nominal FS feedback 722,824 (0x0B0788) at 4080 cycles per sample, 722,535 (0x0B0667) after the retune.
  CHECK(usb::feedback_value(usb::Speed::Full, usb::nominal_frames_q16(usb::Speed::Full, usb::CyclesPerSample{4080u, 1u})) == 722824u);
  CHECK(usb::feedback_value(usb::Speed::Full, usb::nominal_frames_q16(usb::Speed::Full, usb::CyclesPerSample{1800000u, 441u})) == 722535u);
}

TEST_CASE("audio hal: block and ring geometry") {
  CHECK(haudio::kBlock == 64u);
  CHECK(haudio::kBlockSamples == 128u);
  CHECK(haudio::kBlockBytes == 256u);
  CHECK(haudio::kTxSlots == 3u);  // OS-CPU10 slice 3
  CHECK(haudio::kTxPhysSlots == 4u);       // 3 render-ahead blocks need a 4-slot ring (the eDMA modulo)
  CHECK(haudio::kRxPhysSlots == 4u);
  CHECK(haudio::kTxRingBytes == 1024u);
  CHECK(haudio::kRxRingBytes == 1024u);
  // Together 2,048 B with three TX slots (ADR 0013 D7 had 1,536 B for two: OS-CPU10 slice 3 adds 512 B of SRAM_U).
  CHECK(haudio::kTxRingBytes + haudio::kRxRingBytes == 2048u);
  CHECK(haudio::pow2_ceil(3u) == 4u);
  CHECK(haudio::pow2_ceil(2u) == 2u);
  CHECK(haudio::pow2_ceil(5u) == 8u);
  CHECK(haudio::is_pow2(1024u));
  CHECK_FALSE(haudio::is_pow2(768u));
  CHECK_FALSE(haudio::is_pow2(0u));
  CHECK(haudio::kStageCount == 9u);  // input, drain, render, strips, buses, master, convert, taps, commit (OS-005b)
  CHECK(haudio::kStageCommit == haudio::kStageCount - 1u);
}

TEST_CASE("irq table: ADR 0013 D1 as data, in the same order as the function") {
  CHECK(irq::kRoleCount == 11u);
  for (unsigned i = 0; i < irq::kRoleCount; ++i) {
    CHECK(static_cast<unsigned>(irq::kTable[i].role) == i);
    CHECK(irq::kTable[i].priority == irq::priority(irq::kTable[i].role));
    CHECK(irq::kTable[i].name != nullptr);
    CHECK(std::strlen(irq::kTable[i].work) > 8u);
    if (i > 0) {
      CHECK(irq::kTable[i].priority > irq::kTable[i - 1].priority);   // strictly increasing: no two rows share a level
    }
    CHECK((irq::kTable[i].priority & 0x0Fu) == 0u);                   // the MK66's four implemented bits
  }
  CHECK(std::strcmp(irq::name(irq::Role::AudioDma), "audio_dma") == 0);
  CHECK(std::strcmp(irq::name(irq::Role::Render), "render") == 0);
  CHECK(std::strcmp(irq::name(irq::Role::Unassigned), "unassigned") == 0);
  CHECK(std::strcmp(irq::name(irq::Role::PluginGate), "plugin_gate") == 0);
  CHECK(irq::priority(irq::Role::PluginGate) == 240);  // SVCall, the call gate's return, stays the lowest
}

TEST_CASE("irq table: what the audio engine relies on") {
  using R = irq::Role;
  // Only a fault preempts the DMA boundary ISR.
  for (const R role : {R::SysTick, R::MidiUart, R::Usb, R::Completion, R::MidiOutTimer, R::Render, R::PinChange, R::Unassigned,
                       R::PluginGate}) {
    CHECK_FALSE(irq::preempts(role, R::AudioDma));
    CHECK(irq::preempts(R::AudioDma, role));
  }
  CHECK(irq::preempts(R::Fault, R::AudioDma));
  // Every real-time ISR preempts the render; pins and unclaimed lines do not.
  for (const R role : {R::SysTick, R::MidiUart, R::Usb, R::Completion, R::MidiOutTimer}) {
    CHECK(irq::preempts(role, R::Render));
  }
  CHECK_FALSE(irq::preempts(R::PinChange, R::Render));
  CHECK(irq::preempts(R::Render, R::PinChange));
  CHECK_FALSE(irq::preempts(R::Render, R::Render));  // equal levels never nest
}

TEST_CASE("audio rate: the DSP constants follow the clock (OS-RATE): the limiter's and the scratch's rate, the cutoff table's scale, the envelope times") {
  namespace core = o3os::core;
  namespace audio = o3os::audio;
  CHECK(audio::MasterLimiter::kRateHz == core::kRefRateHz);
  CHECK(audio::MasterScratch::kRate == core::kRefRateHz);
  CHECK(core::samples_in_ms(1000) == haudio::sample_rate_mhz() / 1000u);  // whole samples in a second
  CHECK(audio::kSvfRateScale * core::kRefRateHz == doctest::Approx(44117.64706f).epsilon(1e-6));  // the table is for 44,117.64706 Hz: w goes as 1 / fs
  CHECK(audio::kSvfW.v[7] == doctest::Approx(audio::kSvfW44117[7] * audio::kSvfRateScale).epsilon(1e-7));
#if defined(O3OS_RATE_44100) && O3OS_RATE_44100
  CHECK(core::kRefRateHz == 44100.0f);
  CHECK(core::samples_in_ms(300) == 13230u);
  CHECK(audio::kFadeEnvUnits == 1654u);
  CHECK(audio::env_units(300) == 1654u);
#else
  // At the pre-1.0 clock every one is the value the code had pinned before they derived from the clock.
  CHECK(core::kRefRateHz == 44117.65f);
  CHECK(audio::kSvfRateScale == 1.0f);
  CHECK(core::samples_in_ms(300) == 13235u);
  CHECK(audio::kFadeEnvUnits == 1655u);
  CHECK(audio::env_units(300) == 1655u);
#endif
}
