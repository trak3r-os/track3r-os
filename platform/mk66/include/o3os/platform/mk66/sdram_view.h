// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which address the CORE uses for the SDRAM it works in (OS-PERF3). The same bytes answer at 0x7000_0000 (the system bus: nothing caches it, 28 cycles a 32-bit load) and, once boot has
// widened the controller's address mask (sdramc_config.h kCm0WithAlias, O3OS_SDRAM_ICODE_ALIAS), at the ICode alias 0x0800_0000 (the code bus: the LMEM code cache holds it, 10 cycles a load
// on a hit; lmem.h has the facts and `diag bench mem` the numbers).
//
// THE SINGLE-ALIAS RULE (mem_map.h): a structure the core reads and writes through the alias is read and written through the alias by EVERY core access to it, for as long as the program
// runs; the cache is write-through and does not allocate on a write miss, so with one alias in use it is coherent by construction, and no other master touches SDRAM (the SDHC is polled, the
// display's eDMA reads SRAM staging, USB and I2S are SRAM). The users are named here, in one place: the base address of a region (or of a structure placed in one) is taken from
// core_view(); a pointer derived from it stays in the alias. A region that is NOT in this list is used at its 0x7000_0000 address only (the undo journal, the diagnostic log, the SDRAM
// slack window, the bench's scratch: a bounce through the alias would only add cache pollution).
//
// What does not belong in an alias region: an object that uses LDREX / STREX (a std::atomic read-modify-write: fetch_add, exchange, compare_exchange). The exclusive monitor of the
// Cortex-M4 is the core's own and the accesses go to the code bus unchanged, so it would very likely work, but nothing here has ever run one there; today every such object (the MIDI
// emitters' epochs, the USB stream's restart flag, the diagnostic log's head) is in SRAM_U. A plain std::atomic load or store is a plain access and is fine anywhere.
//
//   sample arena   (app_main.cpp sample_arena(): the PCM, and the boards placed behind it)    reads by the render (voices), reads/writes by the loaders and the UI
//   delay, reverb  (app_main.cpp: SendDelay::attach, SendReverb::attach)                       the render only
//   project        (app_main.cpp: the model arena)                                            the sequencer in the render, the UI and the loaders in the main loop
//   module region  (app_main.cpp: the module system's record and the cold arena)              the loader, the module views (main loop), the modules' cold code (set_param, note_on)
//   ptfx           (OS-CEPTFX S2a: the .ptfx plugin workspace, mem_map.h kPtfxBase)          the loader and the run job (main loop), the plugin's own code and data (unprivileged, through the alias)

#include <o3os/platform/mk66/mem_map.h>

#include <cstdint>

#ifndef O3OS_SDRAM_ICODE_ALIAS
#error "O3OS_SDRAM_ICODE_ALIAS is the CMake option of that name (platform/mk66/CMakeLists.txt, the one place of its default), passed to every target that links platform_mk66_pure"
#endif

namespace o3os::platform::mk66::mem {

inline constexpr bool kCoreUsesAlias = O3OS_SDRAM_ICODE_ALIAS != 0;

// The address the core uses for the SDRAM byte at `sdram_address` (0x7000_0000 + n).
constexpr std::uint32_t core_view(std::uint32_t sdram_address) noexcept { return kCoreUsesAlias ? sdram_alias(sdram_address) : sdram_address; }

}  // namespace o3os::platform::mk66::mem
