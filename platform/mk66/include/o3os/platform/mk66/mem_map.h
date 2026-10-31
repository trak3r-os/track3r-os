// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The memory map of the MK66FX1M0 Tracker OG: the single source of every region address and size (ADR 0013 section D7,
// ADR 0011 lesson 1). `mk66.ld` is generated from the constants below by tools/gen_ld.py, so the linker script cannot
// drift from this header; a ctest regenerates it and checks the result.
//
// What keeps the generator trivial: it reads every `inline constexpr std::uint32_t kName = <expression>;` statement in this
// file, the expression made of integer literals, earlier constants and + - * / % << >> & | ^ ( ) only, and fails on one it
// cannot evaluate (or that would wrap in 32 bits). Nothing the linker needs may be computed any other way. The rest of the
// file (the region table, the functions, the static_asserts) is for the compiler and the host tests.
//
// Chip facts: K66 Sub-Family Reference Manual, "Memory map" (program flash 0x0000_0000, SRAM_L 0x1FFF_0000 on the code
// bus, SRAM_U 0x2000_0000 on the system bus, SDRAM through the FlexBus SDRAM controller at 0x7000_0000). The stack top is 0x2003_0000, the end of SRAM_U.

#include <cstdint>

namespace o3os::platform::mk66::mem {

inline constexpr std::uint32_t kKiB = 1024u;
inline constexpr std::uint32_t kMiB = 1024u * 1024u;

// ---- Chip ----------------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kFlashBase = 0x00000000u;
inline constexpr std::uint32_t kFlashSize = 1u * kMiB;
// Erase unit of the program flash. The SD bootloader leaves a final sector that the image fills only in part neither erased nor
// programmed (OS-BUG-COSMBOOT, docs/hw/tracker-hardware-notes.md section 3), so the linker pads the image to a multiple of it.
inline constexpr std::uint32_t kFlashSectorSize = 4096u;
// Flash configuration field (FSEC, FOPT, ...): sixteen bytes the chip reads at reset to decide its own protection.
// A wrong value can permanently lock the MCU. See flash_config.h; the linker places it here and asserts so.
inline constexpr std::uint32_t kFlashConfigOffset = 0x400u;
inline constexpr std::uint32_t kFlashConfigSize = 16u;
// Cortex-M4 exception table (16 system vectors) plus the MK66's 100 device interrupts, one word each. It must end
// below the flash configuration field.
inline constexpr std::uint32_t kVectorTableBytes = (16u + 100u) * 4u;

inline constexpr std::uint32_t kSramLBase = 0x1FFF0000u;
inline constexpr std::uint32_t kSramLSize = 64u * kKiB;
inline constexpr std::uint32_t kSramUBase = 0x20000000u;
inline constexpr std::uint32_t kSramUSize = 192u * kKiB;

inline constexpr std::uint32_t kSdramBase = 0x70000000u;
inline constexpr std::uint32_t kSdramSize = 16u * kMiB;
// OS-PERF3: the SDRAM's second address, the ICode alias (K66 RM Table 5-1: 0x0800_0000-0x0FFF_FFFF "SDRAM (Aliased area)"). A core access there takes the code bus, so the LMEM code
// cache can hold it (lmem.h), where an access to 0x7000_0000 takes the system bus and nothing caches it. The same bytes answer at both (the controller ignores address bits 30:27 once
// sdramc_config.h kCm0WithAlias is written at boot). THE SINGLE-ALIAS RULE: a byte that is cached must be read AND written through the alias, by every core access, for as long as the
// cache holds it; the cache is write-through and does not allocate on a write miss, so with one alias in use it is always coherent, and nothing but the core touches SDRAM (the SDHC
// is polled, the display's eDMA reads SRAM staging, USB and I2S are in SRAM_U). A write through 0x7000_0000 behind a cached line leaves the line stale (the perf notes).
inline constexpr std::uint32_t kSdramAliasBase = 0x08000000u;
constexpr std::uint32_t sdram_alias(std::uint32_t sdram_address) noexcept { return sdram_address - kSdramBase + kSdramAliasBase; }

// ---- SRAM_L: hot code, and the taps' data (ADR 0013 D7, amended by the OS-005b review) ---------------------------------
// The top 16 KiB is the module slab of ADR 0002 / OS-005i (hot `render` code of loaded modules); the linker script's
// SRAM_L region ends below it, so nothing else can land there. Below it: `.ramfunc_l`, the hot kernels (voices, buses,
// filters, sequencer, the audio ISR path), budgeted 24 KiB, and `.tapmem_l`, the data the render writes a block at a time for readers
// in other contexts (the stem pool and its ring, the scope, spectrum and meter rings: audio/tap_mem.h), budgeted 24 KiB, together the
// 48 KiB of the region. The render's working buffers (the accumulators, the planes, the strips) stay in SRAM_U, where it reads and writes
// them sample by sample.
inline constexpr std::uint32_t kModuleSlabSize = 14u * kKiB;  // 16 KiB until the batch with OS-005g + OS-012a (1 KiB to the hot code; the TB-303's hot part is 2.7 KB), 15 until OS-PERF3: 1 KiB more to the hot code (a module whose hot part does not fit the slab runs from the SDRAM, cached through the ICode alias since OS-PERF3)
inline constexpr std::uint32_t kRamfuncLSize = kSramLSize - kModuleSlabSize;
inline constexpr std::uint32_t kModuleSlabBase = kSramLBase + kRamfuncLSize;

// ---- SRAM_U: data, bss, noinit, stack (ADR 0013 D7: about 160 of 192 KiB, at least 32 free) ------------------------
// Low to high: `.noinit` (the crash record, the stage word, the fault stack), `.data`, `.bss`, free, the main stack
// growing down from the top. One stack serves thread mode and every ISR (MSP only, no RTOS), 24 KiB as in D7.
inline constexpr std::uint32_t kStackSize = 24u * kKiB;
inline constexpr std::uint32_t kStackTop = kSramUBase + kSramUSize;
// A separate small stack the fault handlers switch to, so a handler survives the very overflow it is reporting.
inline constexpr std::uint32_t kFaultStackSize = 512u;
// The crash record (crash_record.h: 64 bytes) and the stage word pair that survive a reset, in `.noinit`.
inline constexpr std::uint32_t kCrashBlockSize = 128u;
// The two SRAM budgets of ADR 0013 D7 that the linker script enforces on the real image (the per-row tables are below):
// at least 32 KiB of SRAM_U between the end of `.bss` and the stack stay free, and `.ramfunc_l` (hot kernels 20 KiB plus the
// audio HAL/ISR path 4 KiB) stays within 24 KiB of the 48 KiB region.
// The audio DMA rings sit in their own SRAM_U section, aligned to the largest of them (the RX ring, 1 KiB: hal::audio::kRxRingBytes;
// audio.cpp static_asserts it): the eDMA's address modulo needs a ring on a multiple of its own size.
inline constexpr std::uint32_t kDmaRingAlign = 1024u;
inline constexpr std::uint32_t kSramUMinFree = 32u * kKiB;
inline constexpr std::uint32_t kSramLHotBudget = 27u * kKiB;  // 23,468 B of hot code after OS-MEM1; 26,364 B at 0.4.6 (260 B of room); 27 KiB since OS-PERF3, which took 1 KiB from the module slab for the sampler's check-free loop and the worst-render record. Two KiB came from the slab so far (OS-005g + OS-012a, OS-PERF3)
inline constexpr std::uint32_t kSramLDataBudget = 23u * kKiB;  // 22,272 B in use (TapMem): 1,280 B of room

// ---- SDRAM: the ADR 0013 D7 table, in order, sizes in KiB -----------------------------------------------------------
// Every region starts where the previous one ends. The sizes are the table's, to the KiB, except the two the OS-004a model
// moved (below); the ones marked tunable there are one constant each.
//
// Sample arena: 10,816 KiB (10.56 MiB), 640 KiB less than ADR 0013 D7's 11,456, which went to the project region (the
// orchestrator's correction after OS-004a: the model arena's worst case does not fit 1,280 KiB). The slack is unchanged.
// A recording is limited to the free space of the arena (ADR 0013 acceptance), so the 133 s of an empty project (PAR-134)
// becomes 125 s here: the record length follows what is free, never an overlap (D-OS-10).
inline constexpr std::uint32_t kSampleArenaBase = kSdramBase;
inline constexpr std::uint32_t kSampleArenaSize = 10816u * kKiB;
inline constexpr std::uint32_t kDelayBase = kSampleArenaBase + kSampleArenaSize;
inline constexpr std::uint32_t kDelaySize = 768u * kKiB;
inline constexpr std::uint32_t kReverbBase = kDelayBase + kDelaySize;
inline constexpr std::uint32_t kReverbSize = 256u * kKiB;
// OS-SENDFX (ADR 0013 amendment 3): what the send effects use of their two regions. The platform layer may not include the audio library, so the figures are mirrored here and
// tests/audio_send_fx_test.cpp (the host) and audio.cpp (the image) check the mirrors against audio::SendDelay::kRingBytes and audio::SendReverb::kMemBytes.
//   delay: the stereo ring, 176,468 frames of one 32-bit word = 4.0 s at 44,117 Hz = 705,872 B, at the start of the region; the rest of the 768 KiB (80,560 B) is spare.
//   reverb: the plate's 13 int16 lines (the FULL layout, HALF uses less), 26,632 samples = 53,264 B, at the start of the region; the rest of the 256 KiB (208,880 B) is spare.
inline constexpr std::uint32_t kDelayRingBytes = 705872u;
inline constexpr std::uint32_t kReverbMemBytes = 53264u;
inline constexpr std::uint32_t kModuleRegionBase = kReverbBase + kReverbSize;
inline constexpr std::uint32_t kModuleRegionSize = 768u * kKiB;
// Project model arena: 1,920 KiB. ADR 0013 D7 budgeted 1,280 KiB; the OS-004a model's worst case, everything full, is
// o3os::model::kModelArenaBytes = 1,933,760 B (1,888.4 KiB; OS-STEREO step 2 added the stereo byte to each of the 64 instruments, 256 B, before that 1,933,504 B; E-SONG-07 added the order list edit journal, 80 B; E-CFG-03 widened the settings block to 26 fields, 8 B; before that 1,933,416 B: OS-APPUX S3 raised the app parameter sets from six to twelve, 984 B; before that 1,932,432 B: OS-INSTUI S8b + S9 added the twelve bytes of INST 1.3 to each of the 64 instruments (768 B) and their twelve parameter slots (3,072 B); before that OS-002c added the instrument sample rate, OS-002d the parameters' generations and the settings block: 13,248 B, OS-004d the project-level state: 1,144 B, OS-FU5 the PLAY ENGINE parameter slot of the 64 instruments: 256 B, OS-PERSIST2 the session state: 344 B, OS-006e the settings blocks' room for 24 fields: 40 B, OS-INSTUI S3 the nine source-page parameter slots of the 64 instruments: 2,304 B, OS-INSTUI S6 the 24 MOD-page slots (SOURCE and the LFO of six destinations): 6,144 B; model/include/o3os/model/limits.h and project.h, static-asserted
// there). The platform layer may not include the model, so the figure is mirrored below and tests/mem_map_model_test.cpp
// checks the mirror against the real constant on the host; the static_assert below keeps the region at least that big.
inline constexpr std::uint32_t kProjectBase = kModuleRegionBase + kModuleRegionSize;
inline constexpr std::uint32_t kProjectSize = 1920u * kKiB;
inline constexpr std::uint32_t kModelArenaWorstCaseBytes = 1933760u;  // mirrors o3os::model::kModelArenaBytes
inline constexpr std::uint32_t kUndoBase = kProjectBase + kProjectSize;
inline constexpr std::uint32_t kUndoSize = 384u * kKiB;
inline constexpr std::uint32_t kFileJobBase = kUndoBase + kUndoSize;
inline constexpr std::uint32_t kFileJobSize = 512u * kKiB;
inline constexpr std::uint32_t kLutBase = kFileJobBase + kFileJobSize;
inline constexpr std::uint32_t kLutSize = 192u * kKiB;
inline constexpr std::uint32_t kUiCacheBase = kLutBase + kLutSize;
inline constexpr std::uint32_t kUiCacheSize = 512u * kKiB;
inline constexpr std::uint32_t kDiagLogBase = kUiCacheBase + kUiCacheSize;
inline constexpr std::uint32_t kDiagLogSize = 64u * kKiB;
// The last 192 KiB is the table's slack (1.2 %). Objects the linker places in SDRAM (`.noinit_sdram`, attribute
// O3OS_SDRAM_NOINIT in sections.h) live there until a slice claims a proper region; the image build reports how much of
// it they use and fails the link if they overflow it.
inline constexpr std::uint32_t kSdramSlackBase = kDiagLogBase + kDiagLogSize;
inline constexpr std::uint32_t kSdramSlackSize = 192u * kKiB;

// ---- For the compiler and the tests ----------------------------------------------------------------------------------
struct Region {
  const char* name;
  std::uint32_t base;
  std::uint32_t size;
};

constexpr std::uint32_t end_of(const Region& r) noexcept { return r.base + r.size; }

inline constexpr Region kSdramRegions[] = {
    {"sample_arena", kSampleArenaBase, kSampleArenaSize},
    {"delay", kDelayBase, kDelaySize},
    {"reverb", kReverbBase, kReverbSize},
    {"module_region", kModuleRegionBase, kModuleRegionSize},
    {"project", kProjectBase, kProjectSize},
    {"undo", kUndoBase, kUndoSize},
    {"file_job", kFileJobBase, kFileJobSize},
    {"lut", kLutBase, kLutSize},
    {"ui_cache", kUiCacheBase, kUiCacheSize},
    {"diag_log", kDiagLogBase, kDiagLogSize},
    {"slack", kSdramSlackBase, kSdramSlackSize},
};
inline constexpr unsigned kSdramRegionCount = sizeof(kSdramRegions) / sizeof(kSdramRegions[0]);

// Region i+1 starts where region i ends, the first at the base of SDRAM, every base and size a multiple of 32 bytes
// (ADR 0013 D7: aligned for the SDRAM controller's bursts and the 16-byte cache lines), and the sum of the sizes.
constexpr bool sdram_regions_contiguous() noexcept {
  std::uint32_t expect = kSdramBase;
  for (unsigned i = 0; i < kSdramRegionCount; ++i) {
    if (kSdramRegions[i].base != expect || kSdramRegions[i].size == 0u) {
      return false;
    }
    expect = end_of(kSdramRegions[i]);
  }
  return true;
}
constexpr bool sdram_regions_aligned(std::uint32_t alignment) noexcept {
  for (unsigned i = 0; i < kSdramRegionCount; ++i) {
    if (kSdramRegions[i].base % alignment != 0u || kSdramRegions[i].size % alignment != 0u) {
      return false;
    }
  }
  return true;
}
constexpr std::uint64_t sdram_regions_total() noexcept {
  std::uint64_t total = 0;
  for (unsigned i = 0; i < kSdramRegionCount; ++i) {
    total += kSdramRegions[i].size;
  }
  return total;
}

// A word-sized access at [address, address + length) lies wholly inside internal SRAM (SRAM_L then SRAM_U are
// contiguous). The fault handler uses it before it trusts an exception frame pointer.
constexpr bool in_sram(std::uint32_t address, std::uint32_t length) noexcept {
  const std::uint64_t first = address;
  const std::uint64_t last = first + length;  // one past the end
  return first >= kSramLBase && last <= static_cast<std::uint64_t>(kSramUBase) + kSramUSize;
}

// ---- SRAM budgets, row by row (ADR 0013 D7), for `diag mem` and the tests -----------------------------------------------
// Estimates of the finished engine, not measurements: the image of a slice only has to stay inside its row (the audio
// engine skeleton's statics are checked against theirs in audio_buffers.h), and `diag mem` prints the rows beside the real
// use. Deviations from the ADR table: the vector table stays in flash (OS-007b), so SRAM_L has no vectors row; the crash block
// and the fault stack (640 B, OS-007b) get a row of their own, paid for by 1 KiB off the ADR's "UI/app .bss about 40 KiB"
// estimate, so the sum still leaves at least kSramUMinFree free. The audio rings are 2 KiB, not the ADR's 1.5: the eDMA wraps an address
// with a modulo, so a ring is a power of two bytes; the 3-slot TX ring (OS-CPU10 slice 3) fills the budget (1 KiB + 1 KiB).
struct Budget {
  const char* name;
  std::uint32_t bytes;
};
inline constexpr Budget kSramLBudget[] = {
    {"hot_kernels", 23u * kKiB},          // voices, module voices (OS-012a), buses, filters, sequencer
    {"audio_hal_isr", 4u * kKiB},         // the DMA ISR, the render entry, the pipeline and status writers
    {"module_slab", kModuleSlabSize},     // ADR 0002 / OS-005i, reserved at the top of SRAM_L
    {"tap_data", kSramLDataBudget},       // OS-005b review: the stem pool, scope, spectrum and meter rings (TapMem, 22.3 KB today)
};
inline constexpr unsigned kSramLBudgetCount = sizeof(kSramLBudget) / sizeof(kSramLBudget[0]);
inline constexpr Budget kSramUBudget[] = {
    {"audio_io_rings", 2048u},            // TX ring 1 KiB (3 slots, 4 physical; OS-CPU10 slice 3) + RX ring 1 KiB: the ADR's 2 x 768 B, as powers of two for the eDMA modulo
    {"block_buffers", 14u * kKiB},        // 16 voice float blocks 4 KiB, tracks and buses in int16 about 10 KiB
    {"voice_state", 28u * kKiB + 64u},   // OS-PERF-LFO: PerfSlots 90 -> 180 B (+90 B, five LFO slots); g_voices 23,192 B + g_amiga 5,376 B = 28,568 B of the 28,736 B, 168 B of slack left (nm on the mk66 image, 2026-10-25). OS-PERFFX P5: +64 B from the ADR slack (9.3 KiB are still over the 32 KiB floor) for the third PerfSlots slot (TUNE, +18 B; P0 left 4 B). OS-VOX: 16 sampler voices (11,536 B) + the Amiga voices (5,632 B) = 17,168 B; was 16 KiB for 8 voices (the 2 KiB come from the budget's slack, the real floor is the linker's 32 KiB). OS-MOD: 12 voices and 65 descriptors with the pan, finetune and amp modulators (3 x FilterMod a slot, 3 more generators a voice) + the Amiga voices = 23.3 KB, 6 KiB more from the same slack. OS-GRAN: the granular voice's state (12 x 48 B), the wavetable window and the granular fields a descriptor (65 x 20 B), the position modulator (a FilterMod a descriptor, one more generator a voice) = ~27 KB, 4 KiB more from the same slack
    {"module_hot_state", 16u * kKiB},
    {"sequencer", 8u * kKiB + 64u},      // +64 B for OS-PERF-SMP: the control table's per-group Perform values grow by one group (FxGroup::SampleEnd, +32 B; the image holds 8,856 B). app_main.cpp allows 640 B above it (OS-INSTUI S8b: the MIDI instrument's output state, 72 B); the image held 8,704 B (E-SONG-07: +24 B and the song list's copy, 656 B, is in SDRAM: g_seq_order)
    {"rings", 16u * kKiB},                // MIDI, ParamChange, events, StemRing 10 x 5 blocks, USB return and packets; OS-USBDISK U1c: and the Mass Storage link (MscLink, 1,104 B, two rings of eight 64-byte packets) inside the same 16 KiB; its engine with the two block buffers is in the SDRAM slack window
    {"sd_ft812_staging", 24u * kKiB},     // SD sector buffers, FT812 display-list staging
    {"ui_app_bss", 18u * kKiB},           // ADR: about 40 KiB; OS-MEM1: the App (28 KB) and the project and sample loaders (10.5 KB) moved to the SDRAM slack window (SdramBox, sdram_box.h); OS-SRAMU: and the plots' CurveScratch (4,480 B)
    {"stack", kStackSize},                // thread mode and every ISR (one stack, no RTOS)
    {"crash_block_fault_stack", kCrashBlockSize + kFaultStackSize},
};
inline constexpr unsigned kSramUBudgetCount = sizeof(kSramUBudget) / sizeof(kSramUBudget[0]);

template <unsigned N>
constexpr std::uint32_t budget_sum(const Budget (&rows)[N]) noexcept {
  std::uint32_t total = 0;
  for (unsigned i = 0; i < N; ++i) {
    total += rows[i].bytes;
  }
  return total;
}
constexpr std::uint32_t budget_bytes(const Budget* rows, unsigned count, const char* name) noexcept {
  for (unsigned i = 0; i < count; ++i) {
    const char* a = rows[i].name;
    const char* b = name;
    while (*a != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    if (*a == '\0' && *b == '\0') {
      return rows[i].bytes;
    }
  }
  return 0u;
}

static_assert(kFlashBase == 0u, "the K66 fetches its initial SP and PC from address 0 (and mk66.ld.in puts the table there)");
static_assert(kSramLBase + kSramLSize == kSramUBase, "SRAM_L and SRAM_U are one contiguous 256 KiB block");
static_assert(kStackTop == 0x20030000u, "_estack: initial SP at the end of SRAM_U");
static_assert(kSramLSize + kSramUSize == 256u * kKiB, "MK66FX1M0 has 256 KiB of SRAM");
static_assert(kFlashSize == 1024u * kKiB, "MK66FX1M0 has 1 MiB of program flash");
static_assert(kFlashSectorSize == 4096u && kFlashSize % kFlashSectorSize == 0u, "MK66FX1M0 program flash: 4 KiB sectors");
static_assert(kRamfuncLSize + kModuleSlabSize == kSramLSize, "SRAM_L is hot code plus the module slab");
static_assert(kModuleSlabBase + kModuleSlabSize == kSramLBase + kSramLSize, "the module slab is the top of SRAM_L");
static_assert(kVectorTableBytes <= kFlashConfigOffset, "the vector table must end below the flash configuration field");
static_assert(kFlashConfigOffset + kFlashConfigSize <= kFlashSize, "flash configuration field inside flash");
static_assert(kFlashConfigOffset == 0x400u && kFlashConfigSize == 16u, "the field is 0x400..0x40F (K66 RM: flash configuration field)");
static_assert(kStackSize + kFaultStackSize + kCrashBlockSize < kSramUSize / 2u, "stacks and the crash block are a minor part of SRAM_U");
static_assert(kCrashBlockSize % 8u == 0u && kFaultStackSize % 8u == 0u && kStackSize % 8u == 0u, "AAPCS: 8-byte aligned stacks");

static_assert(kSdramRegionCount == 11u, "ADR 0013 D7 lists ten regions and the slack");
static_assert(sdram_regions_contiguous(), "SDRAM regions follow one another from the base of SDRAM, none empty");
static_assert(sdram_regions_aligned(32u), "every SDRAM region base and size is 32-byte aligned (ADR 0013 D7)");
static_assert(sdram_regions_total() == kSdramSize, "ADR 0013 D7: the regions and the slack sum to exactly 16 MiB");
static_assert(end_of(kSdramRegions[kSdramRegionCount - 1]) == kSdramBase + kSdramSize, "the slack ends at the end of SDRAM");
static_assert(kSdramSlackBase == 0x70FD0000u && kSdramSlackSize == 192u * kKiB, "ADR 0013 D7 slack: 192 KiB at the top");
static_assert(kSampleArenaSize == 10816u * kKiB, "sample arena: 10,816 KiB (ADR 0013 D7's 11,456 minus the 640 KiB the model arena needed)");
static_assert(kDelayRingBytes <= kDelaySize && kDelayBase % 4u == 0u && kDelayRingBytes % 4u == 0u, "the delay's ring (OS-SENDFX) fits the delay region, word aligned");
static_assert(kReverbMemBytes <= kReverbSize && kReverbBase % 4u == 0u, "the reverb's lines (OS-SENDFX) fit the reverb region");
static_assert(kProjectSize >= kModelArenaWorstCaseBytes, "the project region must hold the model's worst-case arena (model limits.h)");
static_assert(kProjectSize == 1920u * kKiB && kSampleArenaSize + kProjectSize == (11456u + 1280u) * kKiB,
              "the project region took exactly what the sample arena gave up: ADR 0013 D7's two rows still sum to 12,736 KiB");

static_assert(kSramLHotBudget <= kRamfuncLSize, "the hot-code budget fits the SRAM_L code region");
static_assert(budget_sum(kSramLBudget) - kModuleSlabSize - kSramLDataBudget == kSramLHotBudget, "SRAM_L rows: hot kernels + audio HAL/ISR are the hot-code budget");
static_assert(kSramLHotBudget + kSramLDataBudget <= kRamfuncLSize, "SRAM_L: the hot code and the tap data both fit the region below the module slab");
static_assert(budget_sum(kSramLBudget) <= kSramLSize, "the SRAM_L budget fits SRAM_L");
static_assert(budget_sum(kSramUBudget) + kSramUMinFree <= kSramUSize, "ADR 0013 D7: the SRAM_U rows leave at least 32 KiB free");
static_assert(budget_bytes(kSramUBudget, kSramUBudgetCount, "stack") == kStackSize, "the stack row is the stack");

}  // namespace o3os::platform::mk66::mem
