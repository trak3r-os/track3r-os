// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MK66 memory map (mem_map.h): ADR 0013 D7's table, pinned by value, and the properties that make it a map. Runs on the
// host and on QEMU (`qemu_mk66`). The static_asserts in the header already stop a build that breaks the sums; these tests
// pin the numbers so that a deliberate change to the table is a deliberate change to a test too.
#include <doctest/doctest.h>

#include <o3os/platform/mk66/mem_map.h>

#include <cstdint>
#include <string_view>

namespace mem = o3os::platform::mk66::mem;

TEST_CASE("mk66 mem_map: the chip's regions") {
  CHECK(mem::kFlashBase == 0x00000000u);
  CHECK(mem::kFlashSize == 1048576u);
  CHECK(mem::kSramLBase == 0x1FFF0000u);
  CHECK(mem::kSramLSize == 65536u);
  CHECK(mem::kSramUBase == 0x20000000u);
  CHECK(mem::kSramUSize == 196608u);
  CHECK(mem::kSdramBase == 0x70000000u);
  CHECK(mem::kSdramSize == 16777216u);
  // SRAM_L and SRAM_U are one contiguous block whose top is _estack.
  CHECK(mem::kSramLBase + mem::kSramLSize == mem::kSramUBase);
  CHECK(mem::kStackTop == 0x20030000u);
}

TEST_CASE("mk66 mem_map: ADR 0013 D7 SDRAM table, region by region (OS-004a: project 1,920 KiB, sample arena 10,816 KiB; OS-CEPTFX S2a: ptfx 768 KiB)") {
  struct Row {
    const char* name;
    std::uint32_t kib;
  };
  const Row table[] = {{"sample_arena", 10816}, {"delay", 768},     {"reverb", 192},     {"wt_contour", 64},
                       {"module_region", 768},  {"project", 1920},  {"undo", 384},      {"ptfx", 768},
                       {"ui_cache", 448},       {"diag_log", 64},   {"slack", 192}};
  CHECK_MESSAGE(mem::kSdramRegionCount == sizeof(table) / sizeof(table[0]), "eleven rows including the slack (file_job and lut merged into ptfx)");
  if (mem::kSdramRegionCount != sizeof(table) / sizeof(table[0])) {
    return;  // no REQUIRE in a no-exceptions build
  }
  std::uint32_t expect_base = mem::kSdramBase;
  std::uint64_t total_kib = 0;
  for (unsigned i = 0; i < mem::kSdramRegionCount; ++i) {
    const mem::Region& r = mem::kSdramRegions[i];
    INFO("region " << table[i].name);
    CHECK(std::string_view(r.name) == table[i].name);
    CHECK(r.size == table[i].kib * 1024u);
    CHECK(r.base == expect_base);          // follows the previous region, none overlaps, none leaves a hole
    CHECK(r.base % 32u == 0u);             // ADR 0013 D7: 32-byte aligned
    CHECK(r.size % 32u == 0u);
    expect_base = r.base + r.size;
    total_kib += table[i].kib;
  }
  CHECK(total_kib == 16384u);              // 16 MiB exactly
  CHECK(expect_base == mem::kSdramBase + mem::kSdramSize);
  CHECK(mem::sdram_regions_total() == mem::kSdramSize);
}

TEST_CASE("mk66 mem_map: the ptfx region (OS-CEPTFX S2a, A3): carved from file_job + lut + the first 64 KiB of ui_cache, its 3.2 sub-areas tile it") {
  CHECK(mem::kPtfxBase == 0x70E90000u);  // where file_job started
  CHECK(mem::kPtfxSize == 768u * 1024u);
  CHECK(mem::kPtfxBase == mem::kUndoBase + mem::kUndoSize);
  CHECK(mem::kPtfxBase + mem::kPtfxSize == mem::kUiCacheBase);
  CHECK(mem::kUiCacheBase == 0x70F50000u);
  CHECK(mem::kUiCacheSize == 448u * 1024u);  // ui_cache keeps its last 448 KiB
  CHECK(mem::kUiCacheBase + mem::kUiCacheSize == mem::kDiagLogBase);  // diag_log and the slack are unchanged
  // The 3.2 table, as offsets from the region's base.
  CHECK(mem::kPtfxImageOff == 0u);
  CHECK(mem::kPtfxImageSize == 32u * 1024u);
  CHECK(mem::kPtfxGuardOff == 32u * 1024u);
  CHECK(mem::kPtfxGuardSize == 4u * 1024u);
  CHECK(mem::kPtfxStackOff == 36u * 1024u);
  CHECK(mem::kPtfxStackSize == 12u * 1024u);
  CHECK(mem::kPtfxStackTopOff == 48u * 1024u);  // the PSP starts here
  CHECK((mem::kPtfxBase + mem::kPtfxStackTopOff) % 8u == 0u);  // 8-byte aligned (AAPCS)
  CHECK(mem::kPtfxStereoOff == 48u * 1024u);
  CHECK(mem::kPtfxStereoSize == 32u * 1024u);  // L and R, 8,192 frames each
  CHECK(mem::kPtfxScratchOff == 80u * 1024u);
  CHECK(mem::kPtfxScratchSize == 640u * 1024u);  // 655,360 B
  CHECK(mem::kPtfxMetaOff == 720u * 1024u);
  CHECK(mem::kPtfxMetaSize == 16u * 1024u);  // the catalogue (11 KiB), the params, the host table, the job's state
  CHECK(mem::kPtfxSpareOff == 736u * 1024u);
  CHECK(mem::kPtfxSpareSize == 32u * 1024u);
  CHECK(mem::kPtfxSpareOff + mem::kPtfxSpareSize == mem::kPtfxSize);  // the sub-areas tile the region
  // The catalogue of OS-CEPTFX S1 (11 KiB for 128 files) fits the meta sub-area.
  CHECK(128u * 88u <= mem::kPtfxMetaSize);
  // The region is 32-byte aligned as every SDRAM region, and the alias maps it inside the ICode window.
  CHECK(mem::kPtfxBase % 32u == 0u);
  CHECK(mem::kPtfxSize % 32u == 0u);
  CHECK(mem::sdram_alias(mem::kPtfxBase) == 0x08E90000u);
  CHECK(mem::sdram_alias(mem::kPtfxBase + mem::kPtfxSize - 1u) == 0x08F4FFFFu);
}

TEST_CASE("mk66 mem_map: the send effects' share of the delay and reverb regions (OS-SENDFX)") {
  CHECK(mem::kDelayRingBytes == 705872u);  // 176,468 frames x 4 B: 4.0 s of stereo int16 at 44,117 Hz
  CHECK(mem::kDelayRingBytes <= mem::kDelaySize);
  CHECK(mem::kDelaySize - mem::kDelayRingBytes == 80560u);
  CHECK(mem::kReverbMemBytes == 53264u);  // 26,632 int16 samples: the plate's 13 lines, each on a word boundary
  CHECK(mem::kReverbMemBytes <= mem::kReverbSize);
  CHECK(mem::kReverbSize == 192u * 1024u);  // OS-CEWTENV W2 gave the last 64 KiB of the former 256 KiB to the wt_contour region
  CHECK(mem::kReverbSize - mem::kReverbMemBytes == 143344u);
}

TEST_CASE("mk66 mem_map: the wt_contour region (OS-CEWTENV W2)") {
  CHECK(mem::kWtContourSize == 64u * 1024u);  // 256 contours x 128 int16
  CHECK(mem::kWtContourBase == 0x70B80000u);
  CHECK(mem::kWtContourBase == mem::kReverbBase + mem::kReverbSize);
  CHECK(mem::kWtContourBase + mem::kWtContourSize == mem::kModuleRegionBase);  // the bases after it are unchanged
  CHECK(mem::kModuleRegionBase == 0x70B90000u);
}

TEST_CASE("mk66 mem_map: the user contours sit after the bake pool in the reverb tail (OS-CEWTENV W8)") {
  CHECK(mem::kWtUserBase == mem::kWtBakeBase + mem::kWtBakeBytes);
  CHECK(mem::kWtUserBase % 4u == 0u);
  CHECK(mem::kWtUserBytes == 33296u + 24576u * 2u);  // WtUserBank + the PCM scratch
  CHECK(mem::kWtUserBase + mem::kWtUserBytes - mem::kReverbBase == 181012u);
  CHECK(mem::kWtUserBase + mem::kWtUserBytes <= mem::kReverbBase + mem::kReverbSize);
}

TEST_CASE("mk66 mem_map: named regions land where the linker script puts them") {
  CHECK(mem::kSampleArenaBase == 0x70000000u);
  CHECK(mem::kSampleArenaSize == 10u * 1024u * 1024u + 576u * 1024u);  // 10.56 MiB: ADR 0013 D7's 11.19 MiB minus 640 KiB
  CHECK(mem::kSdramSlackBase == 0x70FD0000u);                          // the static-object window is the top 192 KiB
  CHECK(mem::kSdramSlackBase + mem::kSdramSlackSize == mem::kSdramBase + mem::kSdramSize);
}

TEST_CASE("mk66 mem_map: SRAM_L is hot code, the taps' data and the module slab; SRAM_U has room") {
  CHECK(mem::kModuleSlabSize == 14u * 1024u);
  CHECK(mem::kRamfuncLSize == 50u * 1024u);
  CHECK(mem::kModuleSlabBase == 0x1FFFC800u);
  CHECK(mem::kModuleSlabBase + mem::kModuleSlabSize == mem::kSramLBase + mem::kSramLSize);
  // ADR 0013 D7: SRAM_U stacks 24 KiB; the crash block, the fault stack and the main stack leave well over half of SRAM_U.
  CHECK(mem::kStackSize == 24u * 1024u);
  CHECK(mem::kStackSize + mem::kFaultStackSize + mem::kCrashBlockSize < mem::kSramUSize / 2u);
  CHECK(mem::kStackSize % 8u == 0u);
  CHECK(mem::kFaultStackSize % 8u == 0u);
  CHECK(mem::kCrashBlockSize % 8u == 0u);
}

TEST_CASE("mk66 mem_map: flash layout") {
  CHECK(mem::kFlashConfigOffset == 0x400u);
  CHECK(mem::kFlashConfigSize == 16u);
  CHECK(mem::kVectorTableBytes == (16u + 100u) * 4u);
  CHECK(mem::kVectorTableBytes <= mem::kFlashConfigOffset);
}

TEST_CASE("mk66 mem_map: in_sram() accepts what is in SRAM and nothing else") {
  CHECK(mem::in_sram(0x1FFF0000u, 4));
  CHECK(mem::in_sram(0x2002FFFCu, 4));
  CHECK(mem::in_sram(0x20000000u, 32));
  CHECK(mem::in_sram(0x2002FFE0u, 32));
  CHECK_FALSE(mem::in_sram(0x2002FFE4u, 32));   // runs off the end
  CHECK_FALSE(mem::in_sram(0x1FFEFFFCu, 4));    // just below
  CHECK_FALSE(mem::in_sram(0x20030000u, 4));    // just past
  CHECK_FALSE(mem::in_sram(0x00000000u, 4));    // flash
  CHECK_FALSE(mem::in_sram(0x70000000u, 4));    // SDRAM
  CHECK_FALSE(mem::in_sram(0xFFFFFFFCu, 8));    // wraps around 32 bits: must not pass
  CHECK_FALSE(mem::in_sram(0xFFFFFFFFu, 0xFFFFFFFFu));
}

TEST_CASE("mk66 mem_map: the project region holds the model's worst-case arena (OS-004a)") {
  CHECK(mem::kModelArenaWorstCaseBytes == 1934344u);  // tests/mem_map_model_test.cpp checks this against o3os::model::kModelArenaBytes
  CHECK(mem::kProjectSize == 1920u * 1024u);
  CHECK(mem::kProjectSize >= mem::kModelArenaWorstCaseBytes);
  CHECK(mem::kProjectSize - mem::kModelArenaWorstCaseBytes < 64u * 1024u);  // little waste: the region is the arena plus a margin
  CHECK(mem::kProjectBase % 8u == 0u);                                     // the arena is alignas(8)
  // The 640 KiB the project region gained came from the sample arena; the slack and every other region are as in the ADR.
  CHECK(mem::kSampleArenaSize + mem::kProjectSize == (11456u + 1280u) * 1024u);
  CHECK(mem::kSdramSlackSize == 192u * 1024u);
}

TEST_CASE("mk66 mem_map: SRAM_U and SRAM_L budgets hold (ADR 0013 D7)") {
  std::uint32_t u_sum = 0;
  for (unsigned i = 0; i < mem::kSramUBudgetCount; ++i) {
    INFO("row " << mem::kSramUBudget[i].name);
    CHECK(mem::kSramUBudget[i].bytes != 0u);
    u_sum += mem::kSramUBudget[i].bytes;
  }
  CHECK(u_sum == mem::budget_sum(mem::kSramUBudget));
  CHECK(u_sum + mem::kSramUMinFree <= mem::kSramUSize);       // at least 32 KiB free (H14 out)
  CHECK(mem::kSramUSize - u_sum >= 32u * 1024u);
  CHECK(mem::kSramUMinFree == 32u * 1024u);
  CHECK(mem::budget_bytes(mem::kSramUBudget, mem::kSramUBudgetCount, "audio_io_rings") == 2048u);  // the ADR's 2 x 768 B as powers of two: room for the 3-slot fallback
  CHECK(mem::budget_bytes(mem::kSramUBudget, mem::kSramUBudgetCount, "stack") == mem::kStackSize);
  CHECK(mem::budget_bytes(mem::kSramUBudget, mem::kSramUBudgetCount, "nope") == 0u);

  std::uint32_t l_sum = 0;
  for (unsigned i = 0; i < mem::kSramLBudgetCount; ++i) {
    l_sum += mem::kSramLBudget[i].bytes;
  }
  CHECK(l_sum == 64u * 1024u);                                 // hot 20 + audio HAL/ISR 4 + tap data 24 + slab 16: all of SRAM_L (OS-005b review)
  CHECK(l_sum <= mem::kSramLSize);
  CHECK(mem::kSramLHotBudget == 27u * 1024u);
  CHECK(mem::kSramLHotBudget <= mem::kRamfuncLSize);
  CHECK(mem::kSramLDataBudget == 23u * 1024u);                 // the stem pool and the scope, spectrum and meter rings (audio/tap_mem.h)
  CHECK(mem::kSramLHotBudget + mem::kSramLDataBudget <= mem::kRamfuncLSize);
  CHECK(l_sum - mem::kModuleSlabSize - mem::kSramLDataBudget == mem::kSramLHotBudget);
  CHECK(mem::budget_bytes(mem::kSramLBudget, mem::kSramLBudgetCount, "tap_data") == mem::kSramLDataBudget);
}
