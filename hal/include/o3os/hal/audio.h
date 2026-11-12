// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The audio HAL contract (ADR 0013 D1/D2, ADR 0012 amendment 2; OS-005a): block geometry, the one rate constant, the
// block callback and the device that drives it. Platforms implement `Device`: platform/mk66 (I2S0 + eDMA + SGTL5000)
// and platform/qemu (a fake I2S whose block clock is a timer interrupt). The pure engine (audio) never touches
// hardware; it sees only what is declared here.
//
// Execution model (ADR 0013 D1, option B):
//   * A tiny DMA ISR at hal::irq::prio::kAudioDma advances the slot index, stamps the cycle counter and pends the render
//     line. It touches no audio data (the one exception, zeroing a stale slot after an underrun mute, is in audio/slot_ring.h).
//   * The render line runs at hal::irq::prio::kRender and calls the BlockCallback once per block: never from the DMA
//     ISR, never from the main loop.
//   * Everything a platform has to say about the codec's mute state is `Device::start()` (muted), `unmute()` and `mute()`.

#include <o3os/core/audio_clock.h>

#include <cstdint>

namespace o3os::hal::audio {

// ---- Block geometry ---------------------------------------------------------------------------------------------------
// 64 frames per block (ADR 0013 D1: latency and control rate; 128 stays legal, this is the one constant). A frame is one
// stereo pair of int16, interleaved left then right, the layout both I2S directions and the USB path use.
inline constexpr std::uint32_t kBlock = 64;
inline constexpr std::uint32_t kChannels = 2;
inline constexpr std::uint32_t kBlockSamples = kBlock * kChannels;
inline constexpr std::uint32_t kBlockBytes = kBlockSamples * static_cast<std::uint32_t>(sizeof(std::int16_t));

constexpr std::uint32_t pow2_ceil(std::uint32_t v) noexcept {
  std::uint32_t p = 1;
  while (p < v) {
    p <<= 1;
  }
  return p;
}
constexpr bool is_pow2(std::uint32_t v) noexcept { return v != 0u && (v & (v - 1u)) == 0u; }

// ---- The TX ring ------------------------------------------------------------------------------------------------------
// `kTxSlots` is the render-ahead depth in blocks: 2 means the block rendered at a boundary plays at the very next one, so
// the render's deadline is one block; 3 adds 1.45 ms of latency and doubles the deadline (ADR 0013 D1, amendment OS-CPU10
// slice 3: 3 is the shipping value, maintainer 2026-10-23). The DMA ring itself is a power of two number of slots (the eDMA
// wraps an address with a modulo, not a compare): `kTxPhysSlots`. With 2 slots that is 2 (512 B), with 3 it is 4 (1 KiB).
inline constexpr std::uint32_t kTxSlots = 3;
inline constexpr std::uint32_t kTxPhysSlots = pow2_ceil(kTxSlots);
// The RX ring is always 4 slots: the render reads the newest *complete* slot (the one behind the slot the DMA is writing),
// which the DMA does not touch again for two more boundaries, so no read can race a write (audio/pipeline.h).
inline constexpr std::uint32_t kRxPhysSlots = 4;
static_assert(kTxSlots >= 2 && kTxSlots <= kTxPhysSlots, "the render-ahead depth is 2 or 3 blocks");
static_assert(is_pow2(kTxPhysSlots) && is_pow2(kRxPhysSlots), "the eDMA address modulo needs power-of-two rings");

inline constexpr std::uint32_t kTxRingBytes = kTxPhysSlots * kBlockBytes;
inline constexpr std::uint32_t kRxRingBytes = kRxPhysSlots * kBlockBytes;

// ---- The rate constant ------------------------------------------------------------------------------------------------
// MCLK is derived from the CPU clock (ADR 0012 "Engine clock"), so one sample is an exactly known number of CPU cycles: cycles
// per sample as a rational number. There is ONE constant for the whole OS, core::kRefCyclesPerSample (core/audio_clock.h, ADR 0012
// amendment 2): 1800000/441 by default, the D-OS-8 clock (180 MHz x 196/3125 = 11.2896 MHz = 256 x 44,100 Hz exactly), 4080/1 with
// -DO3OS_RATE_44100=OFF (180 MHz x 16/255 = 11.294 MHz = 256 x 44,117.647 Hz). The I2S divider, the block length in cycles, the
// USB feedback value and the test tone all follow from it. It sits in `core` so that neither `hal` nor `usb` depends on the other
// for it; the MK66 image asserts that `kCpuHz` is its clock tree's core clock.
using CyclesPerSample = core::CyclesPerSample;
inline constexpr std::uint32_t kCpuHz = core::kCpuHz;
inline constexpr CyclesPerSample kCyclesPerSample = core::kRefCyclesPerSample;

// MCLK = 256 x Fs (the codec's MCLK_FREQ = 256 x Fs setting), so MCLK cycles per sample is 256 and the CPU-clock to MCLK
// ratio is cycles-per-sample / 256: fract / divide = 256 x den / num, reduced. The I2S0 MDR register divides its source
// by DIVIDE / FRACT: MCLK = source x FRACT / DIVIDE, FRACT 1..256 (8 bits, stored minus one) and DIVIDE 1..4096 (12
// bits, stored minus one), FRACT <= DIVIDE.
inline constexpr std::uint32_t kMclkPerSample = 256;
struct MclkRatio {
  std::uint32_t fract;   // numerator: 0 when the rate has no representation
  std::uint32_t divide;  // denominator
};
constexpr MclkRatio mclk_ratio(CyclesPerSample r) noexcept {
  const std::uint64_t n = static_cast<std::uint64_t>(kMclkPerSample) * r.den;  // fract before reduction
  const std::uint64_t d = r.num;                                              // divide before reduction
  std::uint64_t a = n;
  std::uint64_t b = d;
  while (b != 0u) {
    const std::uint64_t t = a % b;
    a = b;
    b = t;
  }
  const std::uint64_t fract = n / a;
  const std::uint64_t divide = d / a;
  if (fract < 1u || fract > 256u || divide < 1u || divide > 4096u || fract > divide) {
    return MclkRatio{0u, 0u};
  }
  return MclkRatio{static_cast<std::uint32_t>(fract), static_cast<std::uint32_t>(divide)};
}
inline constexpr MclkRatio kMclkRatio = mclk_ratio(kCyclesPerSample);
static_assert(kMclkRatio.fract != 0u, "the rate constant must be representable by I2S0_MDR (FRACT 1..256, DIVIDE 1..4096)");

constexpr std::uint32_t mclk_hz(MclkRatio ratio, std::uint32_t source_hz = kCpuHz) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(source_hz) * ratio.fract / ratio.divide);
}
// Sample rate in millihertz (44,117,647 for 4080/1; 44,100,000 for 1800000/441).
constexpr std::uint32_t sample_rate_mhz(CyclesPerSample r = kCyclesPerSample, std::uint32_t cpu_hz = kCpuHz) noexcept {
  return static_cast<std::uint32_t>((static_cast<std::uint64_t>(cpu_hz) * 1000u * r.den + r.num / 2u) / r.num);
}
// CPU cycles in one block, rounded (261,120 and 261,224). The load governor and the jitter figures are relative to it.
constexpr std::uint32_t cycles_per_block(CyclesPerSample r = kCyclesPerSample) noexcept {
  return static_cast<std::uint32_t>((static_cast<std::uint64_t>(kBlock) * r.num + r.den / 2u) / r.den);
}
inline constexpr std::uint32_t kCyclesPerBlock = cycles_per_block();

// ---- The block callback -----------------------------------------------------------------------------------------------
// Stages whose cycle counts a render reports (`diag cpu`), in execution order: the engine's eight (OS-005b: the fixed graph, ADR 0013
// D4) plus the commit of the block to the TX slot, which the pipeline times itself.
inline constexpr unsigned kStageCount = 9;
inline constexpr unsigned kStageInput = 0;    // capture: the RX block to planes, its peak
inline constexpr unsigned kStageDrain = 1;    // the ParamChange ring, drained in the graph's pass (after the block's voices), gain targets recomputed
inline constexpr unsigned kStageRender = 2;   // sources: voices (OS-005d) and the test sources, to Q15 at the voice-to-track point (D2)
inline constexpr unsigned kStageStrips = 3;   // track strips: inserts, sends, level, pan, the routing sums into the buses
inline constexpr unsigned kStageBuses = 4;    // send effects and their returns, the buses saturated once (D2)
inline constexpr unsigned kStageMaster = 5;   // master chain, master level, cue level, OUT MODE
inline constexpr unsigned kStageConvert = 6;  // the planar output block to the interleaved TX layout, written once per block (D2)
inline constexpr unsigned kStageTaps = 7;     // meters, scope and spectrum rings, StemTap, REC bus
inline constexpr unsigned kStageCommit = 8;   // the pipeline's copy into the TX slot

// What a render tells the pipeline besides the audio: cycles per stage (DWT on the MK66) and the input peak. Optional for a
// callback to fill; the pipeline zeroes it before each call.
struct BlockReport {
  std::uint32_t stage_cycles[kStageCount];
  std::uint32_t input_peak;  // largest |sample| of the input block, 0..32768
};

inline constexpr std::uint32_t kFlagSilence = 1u;  // the pipeline asks for digital silence (underrun mute, ADR 0013 D1)

struct BlockIo {
  const std::int16_t* in;  // kBlock frames, interleaved L,R; a private copy, valid and unchanging for the call
  std::int16_t* out;       // kBlock frames, interleaved L,R; the callback fills all of it
  std::uint32_t flags;     // kFlagSilence
  BlockReport* report;     // never null
};

// Block callback contract.
//   Context   the render line (priority hal::irq::prio::kRender; PendSV on a platform with no spare line). Once per block, in
//             boundary order. May be preempted by every ISR above it. Never called from the DMA ISR or the main loop.
//   Deadline  the TX slot rendered at boundary b plays at boundary b + kTxSlots - 1: with 2 slots the call must finish within
//             one block, with 3 within two; the load governor budgets 60 % of one block. A late block is dropped and counted, never waited for.
//   Rules     no heap, no blocking, no SD or file I/O, no printf, bounded loops (ADR 0013 D1). It runs on the one ISR stack.
struct BlockCallback {
  void (*fn)(void* user, const BlockIo& io) noexcept;
  void* user;
};

// OS-MASTEREQ: the master output EQ and bass boost as the project holds them (model::MasterEq, same ranges): five band gains in dB (-12..+12: 115 Hz, 330 Hz, 990 Hz, 3 kHz, 9.9 kHz)
// and the bass boost 0..100. All zero is "off": the platform keeps its codec as it was before the feature.
struct OutputEq {
  std::int8_t band_db[5];
  std::uint8_t bass;
};

// ---- The device -------------------------------------------------------------------------------------------------------
// Implemented by a platform. All three calls are for the main thread, once the platform is up; none may be called from an
// ISR or the render line (they block: the codec ramps in hundreds of milliseconds).
class Device {
 public:
  enum class Result : std::uint8_t {
    Ok,
    AlreadyStarted,
    CodecAbsent,   // the codec did not acknowledge or reported another part id
    CodecIo,       // a register write was not acknowledged
    ClockFault,    // the MCLK divider did not update in time
  };

  // Brings the audio hardware up and starts calling `render` once per block (H16: never before the codec is muted).
  //   1. The output stage is forced muted FIRST (the amplifier key low, the codec outputs muted), whatever it was before.
  //   2. The master clock runs, then the codec is powered up and configured with every output muted and the DAC at its
  //      lowest volume, then the frame clocks and the DMA start. The blocks are digital silence until the callback writes
  //      something else.
  //   3. start() NEVER unmutes. The outputs stay muted until unmute().
  // Blocking (about a second: the codec's reference ramp); bounded: a hardware wait that runs out returns an error and
  // leaves the outputs muted.
  virtual Result start(const BlockCallback& render) noexcept = 0;

  // Raises the output stage in the order that avoids a pop: the headphone volume at its minimum, the DAC unmuted through the
  // codec's own soft ramp, the codec outputs opened (the headphone at its minimum), the headphone volume ramped up one 0.5 dB step
  // at a time to a quiet level, the amplifier key LAST. Call it only after the blocks have been running for a while without
  // underruns. Returns false (outputs left muted, amplifier closed) if the device has not been started or a codec write failed.
  virtual bool unmute() noexcept = 0;

  // Mutes in the reverse order: the amplifier key first, then the codec. Safe to call at any time, idempotent.
  virtual void mute() noexcept = 0;

  // True between a successful unmute() and the next mute().
  virtual bool outputs_enabled() const noexcept = 0;

  // OS-MASTEREQ: the master output EQ and bass boost of the project (the codec's DAP; analogue outputs only). Records the wanted values: it writes nothing, the platform
  // applies them from its main loop and inside unmute(). Called by the main loop only, with the project's values. The default does nothing (the simulator and QEMU).
  virtual void set_output_eq(const OutputEq& eq) noexcept { static_cast<void>(eq); }

 protected:
  Device() = default;
  ~Device() = default;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
};

}  // namespace o3os::hal::audio
