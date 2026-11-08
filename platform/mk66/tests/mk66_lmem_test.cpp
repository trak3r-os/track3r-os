// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LMEM code cache's register facts (lmem.h; OS-PERF3): the enable word, the region table of the K66 RM (Table 30-1) and the reset value of PCCRMR, the bus an address takes and
// what the cache can hold. Host and Cortex-M4F (`qemu_mk66`). What the cache does on the unit is the HIL item (`diag cache`, `diag bench`).
#include <doctest/doctest.h>

#include <o3os/platform/mk66/lmem.h>

namespace lmem = o3os::platform::mk66::lmem;

TEST_CASE("lmem: the enable word is one store, and the commands are the RM's bits") {
  CHECK(lmem::kEnableAll == 0x85000003u);  // LMEM_PCCCR = 0x85000003
  CHECK(lmem::kInvalidateAll == 0x85000000u);
  CHECK(lmem::kGo == 0x80000000u);
  CHECK(lmem::kInvW0 == 0x01000000u);
  CHECK(lmem::kPushW0 == 0x02000000u);
  CHECK(lmem::kInvW1 == 0x04000000u);
  CHECK(lmem::kPushW1 == 0x08000000u);
  CHECK(lmem::kEncache == 1u);
  CHECK(lmem::kEnwrbuf == 2u);
  CHECK(lmem::kForceWt == 4u);
  CHECK(lmem::kNoAllocate == 8u);
}

TEST_CASE("lmem: register addresses and the cache geometry") {
  CHECK(lmem::kPcccr == 0xE0082000u);
  CHECK(lmem::kPcclcr == 0xE0082004u);
  CHECK(lmem::kPccsar == 0xE0082008u);
  CHECK(lmem::kPcccvr == 0xE008200Cu);
  CHECK(lmem::kPccrmr == 0xE0082020u);
  CHECK(lmem::kCacheBytes == lmem::kSets * lmem::kWays * lmem::kLineBytes);
  CHECK(lmem::kSets == 256u);
  CHECK(lmem::kWays == 2u);
  CHECK(lmem::kLineBytes == 16u);
  // The line command: LCMD (25:24) = 01, LADSEL (26) = 1, and LGO is bit 0 of PCCSAR.
  CHECK(lmem::kLineCmdInvalidate == 0x05000000u);
  CHECK(lmem::kLineGo == 1u);
}

TEST_CASE("lmem: PCCRMR reset value decodes to the RM's table, and a region mode can be set and read back") {
  using lmem::Mode;
  const std::uint32_t r = lmem::kResetRegionModes;
  CHECK(r == 0xAA0FA000u);
  CHECK(lmem::region_mode(r, 0) == Mode::WriteThrough);   // program flash
  CHECK(lmem::region_mode(r, 1) == Mode::WriteThrough);   // the SDRAM alias
  CHECK(lmem::region_mode(r, 2) == Mode::WriteThrough);   // FlexNVM
  CHECK(lmem::region_mode(r, 3) == Mode::WriteThrough);   // the FlexBus alias
  CHECK(lmem::region_mode(r, 4) == Mode::NonCacheable);   // SRAM_L
  CHECK(lmem::region_mode(r, 5) == Mode::NonCacheable);   // SRAM_U
  CHECK(lmem::region_mode(r, 6) == Mode::WriteBack);
  CHECK(lmem::region_mode(r, 7) == Mode::WriteBack);
  CHECK(lmem::region_mode(r, 8) == Mode::WriteThrough);
  CHECK(lmem::region_mode(r, 9) == Mode::WriteThrough);
  for (unsigned i = 10; i < lmem::kRegionCount; ++i) {
    CHECK(lmem::region_mode(r, i) == Mode::NonCacheable);
  }
  CHECK(lmem::region_shift(0) == 30u);
  CHECK(lmem::region_shift(15) == 0u);
  std::uint32_t m = lmem::with_region_mode(r, 1, Mode::NonCacheable);
  CHECK(lmem::region_mode(m, 1) == Mode::NonCacheable);
  CHECK(lmem::region_mode(m, 0) == Mode::WriteThrough);  // the others stay
  CHECK(lmem::region_mode(m, 2) == Mode::WriteThrough);
  CHECK(m == 0x8A0FA000u);
  m = lmem::with_region_mode(m, 1, Mode::WriteThrough);
  CHECK(m == r);
}

TEST_CASE("lmem: Table 30-1, the region an address falls in") {
  CHECK(lmem::region_of(0x00000000u) == 0);
  CHECK(lmem::region_of(0x000FFFFFu) == 0);
  CHECK(lmem::region_of(0x07FFFFFFu) == 0);
  CHECK(lmem::region_of(0x08000000u) == 1);
  CHECK(lmem::region_of(0x0FFFFFFFu) == 1);
  CHECK(lmem::region_of(0x10000000u) == 2);
  CHECK(lmem::region_of(0x13FFFFFFu) == 2);
  CHECK(lmem::region_of(0x14000000u) == -1);  // FlexRAM: no region in the table
  CHECK(lmem::region_of(0x18000000u) == 3);
  CHECK(lmem::region_of(0x1BFFFFFFu) == 3);
  CHECK(lmem::region_of(0x1C000000u) == 4);
  CHECK(lmem::region_of(0x1FFF0000u) == 4);  // SRAM_L
  CHECK(lmem::region_of(0x20000000u) == 5);  // SRAM_U
  CHECK(lmem::region_of(0x2002FFFFu) == 5);
  CHECK(lmem::region_of(0x40000000u) == -1);
  CHECK(lmem::region_of(0x60000000u) == 6);
  CHECK(lmem::region_of(0x70000000u) == 7);  // the SDRAM as the OS maps it
  CHECK(lmem::region_of(0x70FFFFFFu) == 7);
  CHECK(lmem::region_of(0x80000000u) == 8);
  CHECK(lmem::region_of(0x88000000u) == 8);
  CHECK(lmem::region_of(0x90000000u) == 9);
  CHECK(lmem::region_of(0xA0000000u) == -1);
  CHECK(lmem::region_of(0xE0082000u) == -1);
}

TEST_CASE("lmem: what the code cache can hold is the flash and the two ICode aliases, not SRAM and not the SDRAM at 0x7000_0000") {
  CHECK(lmem::cacheable_address(0x00000000u));
  CHECK(lmem::cacheable_address(0x00085AC7u));
  CHECK(lmem::cacheable_address(0x08000000u));    // the SDRAM alias
  CHECK(lmem::cacheable_address(0x08FFFFFFu));
  CHECK(lmem::cacheable_address(0x10000000u));
  CHECK(lmem::cacheable_address(0x18000000u));    // the FlexBus alias
  CHECK_FALSE(lmem::cacheable_address(0x1C000000u));
  CHECK_FALSE(lmem::cacheable_address(0x1FFF0000u));  // SRAM_L is routed before the cache
  CHECK_FALSE(lmem::cacheable_address(0x20000000u));  // SRAM_U: the system bus
  CHECK_FALSE(lmem::cacheable_address(0x70000000u));  // SDRAM through its system-bus address: no cache (the K66 has no system cache)
  CHECK_FALSE(lmem::cacheable_address(0x88000000u));
  CHECK(lmem::on_code_bus(0x1FFFFFFFu));
  CHECK_FALSE(lmem::on_code_bus(0x20000000u));
  CHECK_FALSE(lmem::on_code_bus(0x70000000u));
}

TEST_CASE("lmem: lines and sets") {
  CHECK(lmem::line_base(0x08000000u) == 0x08000000u);
  CHECK(lmem::line_base(0x0800000Fu) == 0x08000000u);
  CHECK(lmem::line_base(0x08000010u) == 0x08000010u);
  CHECK(lmem::line_base(0x08000013u) == 0x08000010u);
  CHECK(lmem::set_of(0x08000000u) == 0u);
  CHECK(lmem::set_of(0x08000010u) == 1u);
  CHECK(lmem::set_of(0x08000FF0u) == 255u);
  CHECK(lmem::set_of(0x08001000u) == 0u);  // 4 KiB later: the same set (two ways: two such lines coexist)
  CHECK(lmem::set_of(0x00000FFFu) == 255u);
}

// OS-PERF3: the core's view of the SDRAM (sdram_view.h): with the alias on, the regions the core works in are at 0x0800_0000 + n, which the code cache can hold; each stays inside the alias window.
#include <o3os/platform/mk66/mem_map.h>
#include <o3os/platform/mk66/sdram_view.h>

TEST_CASE("sdram view: core_view maps the SDRAM byte n to the alias or leaves it, and every region stays inside the window") {
  namespace mem = o3os::platform::mk66::mem;
  for (const mem::Region& r : mem::kSdramRegions) {
    const std::uint32_t v = mem::core_view(r.base);
    CHECK(v - mem::core_view(mem::kSdramBase) == r.base - mem::kSdramBase);  // the same offset from the start of the SDRAM
    CHECK(lmem::cacheable_address(v) == mem::kCoreUsesAlias);
    CHECK(lmem::cacheable_address(mem::core_view(r.base + r.size - 1u)) == mem::kCoreUsesAlias);
  }
  CHECK(mem::core_view(mem::kSampleArenaBase) == (mem::kCoreUsesAlias ? 0x08000000u : 0x70000000u));
  // OS-CEPTFX S2a: the ptfx region is an alias region like the others: the whole region, sub-areas included, maps inside the window.
  CHECK(mem::core_view(mem::kPtfxBase) == (mem::kCoreUsesAlias ? 0x08E90000u : 0x70E90000u));
  CHECK(mem::core_view(mem::kPtfxBase + mem::kPtfxStackTopOff) - mem::core_view(mem::kPtfxBase) == mem::kPtfxStackTopOff);
  CHECK(lmem::cacheable_address(mem::core_view(mem::kPtfxBase + mem::kPtfxSize - 1u)) == mem::kCoreUsesAlias);
}
