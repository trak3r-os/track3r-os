// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MK66 audio path: I2S0 as the master of a 16-bit stereo I2S link to the SGTL5000, two eDMA channels moving 64-frame blocks
// between rings in SRAM_U and the I2S data registers, a slot-boundary ISR that only counts and pends, and the render line.
// ADR 0013 D1/D7, spec OS-005a. The bring-up order is the point of this file (H16: nothing is audible before the codec is
// configured and muted, and start() never unmutes):
//
//   1. amplifier key LOW (muted), whatever it was
//   2. I2S0 clocks and the master clock: MCLK = 180 MHz x FRACT / DIVIDE from the one rate constant, on its pin
//   3. I2C0, the codec's part id, the power-up plan (sgtl5000.h): outputs muted and volumes at their floor until step 12
//   4. I2S0 transmit/receive configured (not enabled), the eDMA channels' TCDs loaded and armed, the render pipeline initialised,
//      the two interrupt lines enabled
//   5. receiver then transmitter enabled: the frame clocks start, the DMA feeds digital silence, the boundaries begin
//   6. a bounded wait for the first boundaries: if none comes the hardware is stopped and start() fails, still muted
// unmute() (separate, later) raises the output stage; the amplifier key is its last step.
#include "internal.h"
#include "regs.h"

#include <cstring>

#include <o3os/audio/engine.h>
#include <o3os/audio/pipeline.h>
#include <o3os/hal/audio.h>
#include <o3os/hal/cycles.h>
#include <o3os/platform/mk66/audio.h>
#include <o3os/platform/mk66/audio_config.h>
#include <o3os/platform/mk66/board.h>
#include <o3os/platform/mk66/clock_config.h>
#include <o3os/platform/mk66/mem_map.h>
#include <o3os/platform/mk66/midi_config.h>
#include <o3os/platform/mk66/platform.h>
#include <o3os/platform/mk66/resources.h>
#include <o3os/platform/mk66/sections.h>
#include <o3os/platform/mk66/sgtl5000.h>

// The pipeline has C linkage and a plain name so that a probe or a map reader finds it (HW-A: until the console exists, the
// status is read from here; its AudioStatus is a member). Zero-initialised .bss until the static constructors and start().
extern "C" {
o3os::audio::Pipeline o3os_audio_pipeline;
// OS-MAINGUARD: SRAM_U has 24 B of margin over the 32 KiB floor (ADR 0013 D7). The guard's 18 bytes are packed by hand into the Pipeline's padding
// (audio/pipeline.h), so a new member, or a reordering, that grows the object by even a word shows here, not as a link error in someone else's slice.
static_assert(sizeof(o3os::audio::Pipeline) == 856u, "the Pipeline grew: SRAM_U has only 24 B of margin (ADR 0013 D7, amendment 7); repack or take it to the maintainer");
// Set by start(): where a debug probe finds the AudioStatus words (layout: audio/status.h).
const o3os::audio::AudioStatus* o3os_audio_status_ptr = nullptr;
}

namespace o3os::platform::mk66 {
namespace {

namespace haudio = o3os::hal::audio;
namespace cfg = audio_cfg;

constexpr std::uint8_t kTxChannel = resources::kDmaTxChannel;
constexpr std::uint8_t kRxChannel = resources::kDmaRxChannel;
constexpr unsigned kRenderIrq = static_cast<unsigned>(resources::kRenderLine);
constexpr unsigned kTxDmaIrq = static_cast<unsigned>(resources::dma_irq(kTxChannel));
constexpr unsigned kSlotShift = cfg::dma::log2_pow2(haudio::kBlockBytes);
static_assert((1u << kSlotShift) == haudio::kBlockBytes, "a slot index is a shift of the DMA address");

// ---- The DMA buffers: alignment is declared, never placed (ADR 0011 lesson 1) ----------------------------------------------
// Each ring is aligned to its own size, which the eDMA's address modulo (ATTR[SMOD]/[DMOD]) needs, and which also makes every
// 16-bit sample, word and LDM access inside them aligned whatever the linker does. Zero-initialised (.bss) = digital silence in
// every TX slot when the DMA starts. SRAM_U, not SRAM_L: the DMA must not contend with instruction fetch on the code bus
// (ADR 0012 / 0013 D7).
struct alignas(haudio::kTxRingBytes) TxRing {
  std::int16_t samples[haudio::kTxPhysSlots * haudio::kBlockSamples];
};
struct alignas(haudio::kRxRingBytes) RxRing {
  std::int16_t samples[haudio::kRxPhysSlots * haudio::kBlockSamples];
};
static_assert(alignof(TxRing) == haudio::kTxRingBytes && sizeof(TxRing) == haudio::kTxRingBytes, "TX ring: aligned to its size, exactly its size");
static_assert(alignof(RxRing) == haudio::kRxRingBytes && sizeof(RxRing) == haudio::kRxRingBytes, "RX ring: aligned to its size, exactly its size");
static_assert(alignof(TxRing) >= 4 && alignof(RxRing) >= 4, "word and multi-word accesses are aligned by type");
static_assert(haudio::kTxRingBytes + haudio::kRxRingBytes <= mem::budget_bytes(mem::kSramUBudget, mem::kSramUBudgetCount, "audio_io_rings"),
              "the DMA rings fit their ADR 0013 D7 budget row (2 KiB: the 3-slot ring's 1 KiB and the 1 KiB RX ring)");
static_assert(haudio::kTxRingBytes <= mem::kDmaRingAlign && haudio::kRxRingBytes <= mem::kDmaRingAlign && haudio::kRxRingBytes == mem::kDmaRingAlign,
              "the linker section of the rings is aligned to the largest ring (mem_map.h kDmaRingAlign)");
O3OS_DMA_RING TxRing g_tx;
O3OS_DMA_RING RxRing g_rx;
std::int16_t* const g_tx_ring = g_tx.samples;
const std::int16_t* const g_rx_ring = g_rx.samples;

// The audio taps' memory (audio/tap_mem.h) lives in SRAM_L, in `.tapmem_l` behind the hot code (mem_map.h kSramLDataBudget): what the render writes a block
// at a time for the USB interrupt and the UI, away from the SRAM_U the DMA masters use. Defined before the engine: its constructor zeroes it, and the
// engine's takes a reference. The engine's own statics (the accumulators, the planes, the strips, the ParamChange ring) are SRAM_U.
O3OS_TAP_MEM_L o3os::audio::TapMem g_tap_mem;
static_assert(sizeof(o3os::audio::TapMem) <= mem::kSramLDataBudget, "the audio taps' memory outgrew its SRAM_L budget (mem_map.h kSramLDataBudget)");
o3os::audio::Engine g_engine(&o3os::hal::cycles, g_tap_mem);
// The engine's SRAM_U statics (OS-005b review: 13.8 KB, of which the ParamChange ring is 3.1 KB and the rest the working buffers and strips) have a cap that keeps growth visible:
// ADR 0013 D7's rows for them ("block_buffers" and "rings", 30 KiB between them, shared with the voices, MIDI, USB and events) are a plan, not a limit, and a
// change that raises the cap is a change of the plan.
static_assert(sizeof(o3os::audio::Engine) <= 16u * mem::kKiB, "the audio engine's SRAM_U statics outgrew the figure OS-005b reported: update the SRAM_U plan (mem_map.h) first");
// The sampler voices (OS-005d lite, 16 since OS-VOX): their blocks and the 65-slot sample table, inside the "voice_state" row of ADR 0013 D7
// (16 KiB for the finished voices). The composition root attaches them to the engine (Engine::attach_voices) when it has a project to play.
o3os::audio::VoicePool g_voices;
static_assert(sizeof(o3os::audio::VoicePool) <= mem::budget_bytes(mem::kSramUBudget, mem::kSramUBudgetCount, "voice_state"),
              "the sampler voices outgrew their SRAM_U row (mem_map.h voice_state)");
// The Amiga voices (OS-005h): 8 Paula channels with their filters, the 16 tracks' MED state, the voices' blocks and the BLEP table's copy, in the same row as the sampler's.
o3os::audio::AmigaVoices g_amiga;
static_assert(sizeof(o3os::audio::VoicePool) + sizeof(o3os::audio::AmigaVoices) <= mem::budget_bytes(mem::kSramUBudget, mem::kSramUBudgetCount, "voice_state"),
              "the sampler and Amiga voices outgrew their SRAM_U row (mem_map.h voice_state)");
std::uint8_t g_last_result = 0;
// OS-HW1: where the last start() stopped on a ClockFault, with the raw MCR and MDR then (main thread only).
AudioClockFault g_clock_fault;

// ISR only: boundaries to ignore error flags for after the I2S clocks start (the first words race the DMA's first transfers).
constexpr std::uint32_t kStartupBoundaries = 2;
std::uint32_t g_warmup = kStartupBoundaries;

// Settling times (bounded waits of start()).
constexpr std::uint32_t kMclkSettleMs = 5;           // after MCLK appears, before the first I2C write (the codec needs its clock before the first write)
constexpr std::uint32_t kFirstBoundariesWaitMs = 25;  // a block is 1.45 ms; two boundaries must arrive well within this
constexpr std::uint32_t kDividerUpdatePolls = 100000;

bool wait_divider_update() noexcept {
  for (std::uint32_t i = 0; i < kDividerUpdatePolls; ++i) {
    if ((regs::reg32(cfg::i2s::kMcr) & cfg::i2s::kMcrDivideUpdateFlag) == 0u) {
      return true;
    }
  }
  return false;
}

// Records the failure (which wait, the raw registers) and the result code; the caller returns Result::ClockFault.
void note_clock_fault(std::uint8_t where) noexcept {
  g_clock_fault.where = where;
  g_clock_fault.mcr = regs::reg32(cfg::i2s::kMcr);
  g_clock_fault.mdr = regs::reg32(cfg::i2s::kMdrAddress);
  g_last_result = static_cast<std::uint8_t>(haudio::Device::Result::ClockFault);
}

void amplifier_muted() noexcept { regs::gpio_pcor(board::kAudioMute) = regs::mask_of(board::kAudioMute); }
void amplifier_open() noexcept { regs::gpio_psor(board::kAudioMute) = regs::mask_of(board::kAudioMute); }

void pin_to_i2s(Pin pin) noexcept { regs::port_pcr(pin) = static_cast<std::uint32_t>(board::kI2sPinMux) << regs::kPcrMuxShift; }

void load_tcd(unsigned channel, const cfg::dma::Tcd& t) noexcept {
  volatile cfg::dma::Tcd& r = regs::dma_tcd(channel);
  r.csr = 0;
  r.saddr = t.saddr;
  r.soff = t.soff;
  r.attr = t.attr;
  r.nbytes = t.nbytes;
  r.slast = t.slast;
  r.daddr = t.daddr;
  r.doff = t.doff;
  r.citer = t.citer;
  r.dlastsga = t.dlastsga;
  r.biter = t.biter;
  r.csr = t.csr;
}

void quiesce_channel(unsigned channel) noexcept {
  regs::dma_cerq() = static_cast<std::uint8_t>(channel);
  regs::dma_ceei() = static_cast<std::uint8_t>(channel);
  regs::dma_cerr() = static_cast<std::uint8_t>(channel);
  regs::dma_cdne() = static_cast<std::uint8_t>(channel);
  regs::dma_cint() = static_cast<std::uint8_t>(channel);
}

// Every step that can fail returns here with the hardware stopped.
void stop_hardware() noexcept {
  regs::reg32(cfg::i2s::kTcsr) = 0;
  regs::reg32(cfg::i2s::kRcsr) = 0;
  quiesce_channel(kTxChannel);
  quiesce_channel(kRxChannel);
  regs::nvic_icer(kTxDmaIrq) = regs::irq_bit(kTxDmaIrq);
  regs::nvic_icer(kRenderIrq) = regs::irq_bit(kRenderIrq);
}

// The class implementing the contract. State is the main thread's (start/unmute/mute) except where noted.
class Mk66Audio final : public haudio::Device {
 public:
  Result start(const haudio::BlockCallback& render) noexcept override;
  bool unmute() noexcept override;
  void mute() noexcept override;
  void stop() noexcept;  // OS-POWER P1: the audio path is stopped for good (the unit powers off or restarts): see below
  bool outputs_enabled() const noexcept override { return enabled_; }
  bool running() const noexcept { return started_; }
  bool apply_input(const sgtl5000::InputPlan& plan) noexcept;  // OS-011c
  // OS-MASTEREQ: records the project's values only; eq_pass() writes them (main loop, outputs open) and unmute() applies them before the outputs come up.
  void set_output_eq(const haudio::OutputEq& eq) noexcept override { eq_.set(eq); }
  void eq_pass(std::uint32_t now_ms) noexcept {
    if (started_ && enabled_) {
      eq_.pump(now_ms, eq_writer());
    }
  }

 private:
  bool codec_write(std::uint16_t reg, std::uint16_t value) noexcept {
    return i2c::write_reg(board::kCodecI2cAddress, reg, value) == i2c::Result::Ok;
  }
  // The codec write handed to the EQ driver (function pointer and context, OS-MASTEREQ).
  sgtl5000::Writer eq_writer() noexcept {
    return sgtl5000::Writer{[](void* self, const sgtl5000::Step& step) noexcept { return static_cast<Mk66Audio*>(self)->codec_write(step.reg, step.value); }, this};
  }
  // A step of the unmute or mute plan: CHIP_ANA_CTRL keeps the ADC's input select the recorder chose (sgtl5000.h "The analogue input").
  bool plan_write(const sgtl5000::Step& step) noexcept {
    const std::uint16_t value = step.reg == sgtl5000::kAnaCtrl ? sgtl5000::ana_ctrl_with_adc(step.value, adc_line_) : step.value;
    return codec_write(step.reg, value);
  }
  Result bring_up_codec() noexcept;
  bool started_ = false;
  bool enabled_ = false;
  // OS-011c: which input the codec's ADC listens to, the line in (the power-up default) or the microphone: CHIP_ANA_CTRL bit 2, which the unmute and mute plans also write, so
  // they keep it (the recorder's source switch, audio_input_apply). Main thread only, like every codec write. (It sits in the padding of this object: no bytes of SRAM.)
  bool adc_line_ = true;
  sgtl5000::EqDriver eq_;  // OS-MASTEREQ: the master EQ and bass boost in the DAP (sgtl5000.h); main thread only, about 20 bytes of .bss
};

Mk66Audio::Result Mk66Audio::bring_up_codec() noexcept {
  if (i2c::init() != i2c::Result::Ok) {
    return Result::CodecIo;
  }
  std::uint16_t id = 0;
  if (i2c::read_reg(board::kCodecI2cAddress, sgtl5000::kChipId, id) != i2c::Result::Ok) {
    return Result::CodecAbsent;
  }
  if ((id >> 8) != sgtl5000::kPartId) {
    return Result::CodecAbsent;
  }
  for (const sgtl5000::Step& step : sgtl5000::kPowerUp) {
    if (!codec_write(step.reg, step.value)) {
      return Result::CodecIo;
    }
    if (step.wait_ms != 0u) {
      boot::delay_ms(step.wait_ms);
    }
  }
  return Result::Ok;
}

}  // namespace

namespace {

Mk66Audio::Result Mk66Audio::start(const haudio::BlockCallback& render) noexcept {
  if (started_) {
    return Result::AlreadyStarted;
  }
  g_last_result = 0;
  g_clock_fault = AudioClockFault{};
  // Declared alignment checked against the real placement (the static_asserts only know the declaration).
  if ((reinterpret_cast<std::uintptr_t>(g_tx_ring) & (haudio::kTxRingBytes - 1u)) != 0u ||
      (reinterpret_cast<std::uintptr_t>(g_rx_ring) & (haudio::kRxRingBytes - 1u)) != 0u ||
      !mem::in_sram(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_tx_ring)), haudio::kTxRingBytes) ||
      !mem::in_sram(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_rx_ring)), haudio::kRxRingBytes)) {
    panic();  // an unaligned DMA ring is a bug in the image, not a condition to run in
  }

  // 1. H16: the output stage is muted before anything else happens.
  amplifier_muted();
  enabled_ = false;
  // The rings are not cleared by the reset sequence (.dma_rings is NOLOAD): digital silence in every slot before the DMA runs.
  std::memset(g_tx.samples, 0, sizeof(g_tx.samples));
  std::memset(g_rx.samples, 0, sizeof(g_rx.samples));

  // 2. Clocks: gates, then the master clock. MCR first (MOE, system clock), the divider after, the pin last: the codec never
  //    sees a clock of the wrong frequency.
  regs::sim_scgc6() = regs::sim_scgc6() | regs::kScgc6I2s;
  // The eDMA's clock gates, DMA_CR and every DCHPRI belong to boot::dma_init() (resources.h is the table): not written here.
  if ((regs::dma_cr() & cfg::dma::kCrValue) != cfg::dma::kCrValue) {
    panic();  // start() before the reset sequence's dma_init(): a bug in the image
  }
  regs::reg32(cfg::i2s::kTcsr) = cfg::i2s::kCsrSoftReset;
  regs::reg32(cfg::i2s::kRcsr) = cfg::i2s::kCsrSoftReset;
  regs::reg32(cfg::i2s::kTcsr) = 0;
  regs::reg32(cfg::i2s::kRcsr) = 0;
  regs::reg32(cfg::i2s::kMcr) = cfg::i2s::kMcrValue;
  if (!wait_divider_update()) {
    note_clock_fault(AudioClockFault::McrWait);
    return Result::ClockFault;
  }
  regs::reg32(cfg::i2s::kMdrAddress) = cfg::i2s::kMdr;
  if (!wait_divider_update()) {
    note_clock_fault(AudioClockFault::MdrWait);
    return Result::ClockFault;
  }
  pin_to_i2s(board::kI2sMclk);
  boot::delay_ms(kMclkSettleMs);

  // 3. The codec, muted.
  const Result codec = bring_up_codec();
  if (codec != Result::Ok) {
    g_last_result = static_cast<std::uint8_t>(codec);
    return codec;
  }

  // 4. I2S format, DMA, pipeline.
  regs::reg32(cfg::i2s::kTmr) = 0;
  regs::reg32(cfg::i2s::kRmr) = 0;
  regs::reg32(cfg::i2s::kTcr1) = cfg::i2s::kTcr1Value;
  regs::reg32(cfg::i2s::kTcr2) = cfg::i2s::kTcr2Value;
  regs::reg32(cfg::i2s::kTcr3) = cfg::i2s::kTcr3Value;
  regs::reg32(cfg::i2s::kTcr4) = cfg::i2s::kTcr4Value;
  regs::reg32(cfg::i2s::kTcr5) = cfg::i2s::kTcr5Value;
  regs::reg32(cfg::i2s::kRcr1) = cfg::i2s::kRcr1Value;
  regs::reg32(cfg::i2s::kRcr2) = cfg::i2s::kRcr2Value;
  regs::reg32(cfg::i2s::kRcr3) = cfg::i2s::kRcr3Value;
  regs::reg32(cfg::i2s::kRcr4) = cfg::i2s::kRcr4Value;
  regs::reg32(cfg::i2s::kRcr5) = cfg::i2s::kRcr5Value;

  quiesce_channel(kTxChannel);
  quiesce_channel(kRxChannel);
  const std::uint32_t tx_base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_tx_ring));
  const std::uint32_t rx_base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_rx_ring));
  load_tcd(kTxChannel, cfg::dma::tx_tcd(tx_base, haudio::kTxRingBytes));
  load_tcd(kRxChannel, cfg::dma::rx_tcd(rx_base, haudio::kRxRingBytes));
  regs::dmamux_chcfg(kTxChannel) = 0;
  regs::dmamux_chcfg(kRxChannel) = 0;
  regs::dmamux_chcfg(kTxChannel) = static_cast<std::uint8_t>(cfg::dma::kMuxEnable | cfg::dma::kSourceI2s0Tx);
  regs::dmamux_chcfg(kRxChannel) = static_cast<std::uint8_t>(cfg::dma::kMuxEnable | cfg::dma::kSourceI2s0Rx);

  o3os::audio::Pipeline::Config config{};
  config.tx = g_tx_ring;
  config.rx = g_rx_ring;
  config.render = render;
  config.cycles = &o3os::hal::cycles;
  config.cycles_per_block = haudio::kCyclesPerBlock;
  config.starve_guard = o3os_audio_pipeline.guard_armed();  // OS-MAINGUARD: a restart (`diag audio start`) keeps what the main loop armed
  o3os_audio_pipeline.init(config);
  o3os_audio_status_ptr = &o3os_audio_pipeline.status();
  g_warmup = kStartupBoundaries;

  regs::nvic_icpr(kTxDmaIrq) = regs::irq_bit(kTxDmaIrq);
  regs::nvic_icpr(kRenderIrq) = regs::irq_bit(kRenderIrq);
  regs::nvic_iser(kRenderIrq) = regs::irq_bit(kRenderIrq);
  regs::nvic_iser(kTxDmaIrq) = regs::irq_bit(kTxDmaIrq);

  regs::dma_serq() = kRxChannel;
  regs::dma_serq() = kTxChannel;

  // 5. The frame clocks start. Bit and frame clock pins and the data pins to I2S only now, next to the enable. The receiver is
  //    enabled first: it is synchronous to the transmitter, and the transmitter's enable is what starts the clocks.
  pin_to_i2s(board::kI2sLrclk);
  pin_to_i2s(board::kI2sBclk);
  pin_to_i2s(board::kI2sTxd);
  pin_to_i2s(board::kI2sRxd);
  regs::reg32(cfg::i2s::kRcsr) = cfg::i2s::kCsrEnable | cfg::i2s::kCsrBitClockEnable | cfg::i2s::kCsrFifoRequestDmaEnable | cfg::i2s::kCsrFifoReset;
  regs::reg32(cfg::i2s::kTcsr) = cfg::i2s::kCsrEnable | cfg::i2s::kCsrBitClockEnable | cfg::i2s::kCsrFifoRequestDmaEnable;

  // 6. The boundaries must come.
  bool running = false;
  for (std::uint32_t ms = 0; ms < kFirstBoundariesWaitMs && !running; ++ms) {
    boot::delay_ms(1);
    running = o3os_audio_pipeline.status().snapshot().boundaries >= 2u;
  }
  if (!running) {
    note_clock_fault(AudioClockFault::NoBoundaries);
    stop_hardware();
    return Result::ClockFault;
  }
  started_ = true;
  eq_.on_start();  // OS-MASTEREQ: the power-up plan left the DAP out of the path
  return Result::Ok;
}

bool Mk66Audio::unmute() noexcept {
  if (!started_) {
    return false;
  }
  // OS-MASTEREQ: the DAC and the outputs are muted here (power-up or mute plan): the codec is made to match the project's EQ first (all defaults: nothing at a fresh start, the way
  // back to the idle state after a route in). A failed write there leaves the idle routing and never stops the outputs from opening.
  eq_.apply_at_unmute(eq_writer());
  sgtl5000::Step step{};
  for (unsigned i = 0; sgtl5000::unmute_step(i, step); ++i) {
    if (!plan_write(step)) {
      mute();
      return false;
    }
    if (step.wait_ms != 0u) {
      boot::delay_ms(step.wait_ms);
    }
  }
  amplifier_open();  // the last step, after every codec output is stable
  enabled_ = true;
  return true;
}

void Mk66Audio::mute() noexcept {
  amplifier_muted();  // first: the amplifier, then the codec behind it
  enabled_ = false;
  eq_.on_mute();  // OS-MASTEREQ: a live route in cut short is redone inside the next unmute
  if (started_) {
    for (const sgtl5000::Step& step : sgtl5000::kMute) {
      plan_write(step);
    }
  }
}

// OS-POWER P1 (docs/specs/OS-POWER.md 4.1 A2, 4.6): the one stop of the audio path. There was none: stop_hardware() ran only when start() failed, and mute() leaves the I2S and the eDMA running.
// The codec first, click-free by construction (sgtl5000.h run_stop: the DAC through its own soft ramp, the 50 ms of it, then the amplifier key LOW, then the outputs and the DAC muted, the VAG ramped down alone (500 ms) and the
// analogue stage powered down), then the hardware: the I2S transmitter and receiver, the two eDMA channels, the two interrupt lines (a render block in flight has finished: this is the main
// thread, and a render that has not started is not pended again). Main thread only; it blocks for about 570 ms (the I2C writes, the DAC ramp and the VAG ramp: OS-BUG-POWERPOP), the caller keeps the display serviced.
// Idempotent, and plain when the path never started (the boot disk mode, a failed bring-up: the key is already LOW and nothing runs). It never touches the block callback, the DSP or the voices.
// The caller has set the power-off latch (appimg::power_off_latched) BEFORE this: with the I2S stopped the block counter stands still, which the audio poll of main.cpp would call a stall, and
// nothing may reopen the outputs (open_wanted).
void Mk66Audio::stop() noexcept {
  enabled_ = false;
  eq_.on_mute();
  if (!started_) {
    amplifier_muted();  // the path never started: nothing plays, the key is only made sure LOW
    return;
  }
  // Started: the key is NOT lowered here. run_stop soft-mutes the DAC and waits the ramp out first, the key falls on silence (spec 4.1 A2: key-first can click).
  (void)sgtl5000::run_stop([this](const sgtl5000::Step& step) { return plan_write(step); }, [] { amplifier_muted(); },
                           [](std::uint32_t ms) {
                             if (ms != 0u) {
                               boot::delay_ms(ms);
                             }
                           });
  stop_hardware();
  started_ = false;
}

bool Mk66Audio::apply_input(const sgtl5000::InputPlan& plan) noexcept {
  if (!started_) {
    return false;
  }
  for (unsigned i = 0; i < plan.count; ++i) {
    if (!codec_write(plan.step[i].reg, plan.step[i].value)) {
      return false;
    }
    if (plan.step[i].reg == sgtl5000::kAnaCtrl) {
      adc_line_ = plan.line;
    }
  }
  return true;
}

Mk66Audio g_device;

}  // namespace

hal::audio::Device& audio_device() noexcept { return g_device; }
bool audio_input_apply(const sgtl5000::InputPlan& plan) noexcept { return g_device.apply_input(plan); }
void audio_stop() noexcept { g_device.stop(); }
void audio_eq_pass(std::uint32_t now_ms) noexcept { g_device.eq_pass(now_ms); }
o3os::audio::Engine& audio_engine() noexcept { return g_engine; }
o3os::audio::VoicePool& audio_voices() noexcept { return g_voices; }
o3os::audio::AmigaVoices& audio_amiga() noexcept { return g_amiga; }
const o3os::audio::AudioStatus& audio_status() noexcept { return o3os_audio_pipeline.status(); }
void audio_cpu_reset() noexcept { o3os_audio_pipeline.reset_peaks(); }
void audio_arm_starve_guard() noexcept { o3os_audio_pipeline.arm_guard(); }
void audio_main_pass() noexcept { o3os_audio_pipeline.main_pass(); }
void audio_set_starve_blocks(std::uint32_t n) noexcept { o3os_audio_pipeline.set_starve_blocks(n); }
const o3os::audio::Pipeline& audio_pipeline() noexcept { return o3os_audio_pipeline; }
o3os::audio::Pipeline::WorstRender audio_worst_render() noexcept { return o3os_audio_pipeline.worst_render(); }
std::uint8_t audio_last_result() noexcept { return g_last_result; }
AudioClockFault audio_clock_fault() noexcept { return g_clock_fault; }
bool audio_running() noexcept { return g_device.running(); }
std::uint32_t audio_block_heard_at() noexcept { return o3os_audio_pipeline.render_stamp() + midi_cfg::kAudioLeadCycles; }

// ---- Interrupts ---------------------------------------------------------------------------------------------------------
// The slot-boundary ISR (eDMA channel 13, priority kAudioDma, runs from SRAM_L). Order matters: stamp first (jitter-free: only a
// fault can preempt it), then clear the interrupt request and wait for the write to land (a posted write could re-enter the ISR),
// then read the two DMA positions, poll the error flags, and hand over to the pipeline, which touches no audio data. The render
// is pended last.
static_assert(resources::kDmaTxChannel == 13, "the handler below is named for the transmit channel's interrupt line, DMA13_DMA29");
extern "C" O3OS_RAMFUNC_L void DMA13_DMA29_IRQHandler() {
  const std::uint32_t stamp = regs::dwt_cyccnt();
  regs::dma_cint() = kTxChannel;
  asm volatile("dsb" ::: "memory");

  const std::uint32_t tx_saddr = regs::dma_tcd(kTxChannel).saddr;
  const std::uint32_t rx_daddr = regs::dma_tcd(kRxChannel).daddr;
  const std::uint32_t tx_base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_tx_ring));
  const std::uint32_t rx_base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_rx_ring));
  const std::uint32_t tx_slot = ((tx_saddr - tx_base) >> kSlotShift) & (haudio::kTxPhysSlots - 1u);
  const std::uint32_t rx_slot = ((rx_daddr - rx_base) >> kSlotShift) & (haudio::kRxPhysSlots - 1u);

  // Error flags: an I2S FIFO error or sync error (the DMA did not keep up, or the clocks glitched) and any eDMA error. Counted,
  // cleared; the first boundaries after the start are not counted (the first words race the first transfers).
  std::uint32_t faults = 0;
  const std::uint32_t tcsr = regs::reg32(cfg::i2s::kTcsr);
  const std::uint32_t rcsr = regs::reg32(cfg::i2s::kRcsr);
  if ((tcsr & cfg::i2s::kCsrErrorFlags) != 0u) {
    regs::reg32(cfg::i2s::kTcsr) = tcsr;  // write-1-to-clear flags; the enable and DMA bits are written back as they were
    ++faults;
  }
  if ((rcsr & cfg::i2s::kCsrErrorFlags) != 0u) {
    regs::reg32(cfg::i2s::kRcsr) = rcsr;
    ++faults;
  }
  if (regs::dma_err() != 0u) {
    regs::dma_cerr() = regs::kDmaClearAll;
    ++faults;
  }
  if (g_warmup != 0u) {
    --g_warmup;
    faults = 0;
  }

  o3os_audio_pipeline.on_boundary(tx_slot, rx_slot, stamp, faults);
  regs::nvic_stir() = kRenderIrq;
}

// The render line (IRQ 64, priority kRender, SRAM_L).
extern "C" O3OS_RAMFUNC_L void SWI_IRQHandler() { o3os_audio_pipeline.render(); }

}  // namespace o3os::platform::mk66
