// SPDX-License-Identifier: GPL-3.0-or-later
//
// `diag mem` for the MK66 (diag_report.h, audio/diag.h; OS-005a): the real memory map and SRAM budgets through the formatter, so a
// change to the map shows up here as a different line. Host and Cortex-M4F (`qemu_mk66`).
#include <doctest/doctest.h>

#include <o3os/audio/diag.h>
#include <o3os/platform/mk66/diag_report.h>

#include <cstdint>
#include <cstring>

namespace diag = o3os::audio::diag;
namespace mk66 = o3os::platform::mk66;
namespace mem = o3os::platform::mk66::mem;

TEST_CASE("diag mem: the SDRAM table, the SRAM budgets and the sums, from the real map") {
  static mk66::MemTables tables;
  mk66::fill_mem_tables(tables, 1660u, 6792u, 22336u);
  static char text[2400];
  const std::size_t n = diag::format_mem(tables.report, text, sizeof text);
  CHECK(n == std::strlen(text));
  CHECK(n < sizeof text - 1u);  // fits: a console buffer of 2.4 KB holds the whole report

  // Line by line, the first block exactly.
  const char* const sdram =
      "sdram 16384 KiB\n"
      "  sample_arena 0x70000000 10816 KiB\n"
      "  delay 0x70A90000 768 KiB\n"
      "  reverb 0x70B50000 256 KiB\n"
      "  module_region 0x70B90000 768 KiB\n"
      "  project 0x70C50000 1920 KiB\n"
      "  undo 0x70E30000 384 KiB\n"
      "  file_job 0x70E90000 512 KiB\n"
      "  lut 0x70F10000 192 KiB\n"
      "  ui_cache 0x70F40000 512 KiB\n"
      "  diag_log 0x70FC0000 64 KiB\n"
      "  slack 0x70FD0000 192 KiB\n"
      "  regions sum 16384 KiB (complete)\n";
  CHECK(std::strncmp(text, sdram, std::strlen(sdram)) == 0);

  const char* const sram =
      "sram_l 64 KiB: hot code 1660 B of 27 KiB, tap data 22336 B of 23 KiB\n"
      "  budget hot_kernels 23 KiB\n"
      "  budget audio_hal_isr 4 KiB\n"
      "  budget module_slab 14 KiB\n"
      "  budget tap_data 23 KiB\n"
      "sram_u 192 KiB: image 6792 B, stack 24 KiB, free 165240 B (at least 32 KiB wanted)\n";
  const char* const rest = text + std::strlen(sdram);
  CHECK(std::strncmp(rest, sram, std::strlen(sram)) == 0);
  CHECK(std::strstr(text, "  budget audio_io_rings 2 KiB\n") != nullptr);
  CHECK(std::strstr(text, "  budget crash_block_fault_stack 640 B\n") != nullptr);
  CHECK(std::strstr(text, "  budget sum 155200 B\n") != nullptr);  // OS-PERFFX P5: voice_state +64 B; OS-PERF-SMP: sequencer +64 B; OS-STEREO (c): voice_state +832 B
}

TEST_CASE("diag mem: a table that does not sum to the part is flagged") {
  static mk66::MemTables tables;
  mk66::fill_mem_tables(tables, 0u, 0u);
  tables.sdram[0].size -= 32u;   // one region short
  static char text[2400];
  diag::format_mem(tables.report, text, sizeof text);
  CHECK(std::strstr(text, "(MISMATCH)") != nullptr);
  CHECK(std::strstr(text, "(complete)") == nullptr);
}

TEST_CASE("diag mem: the SRAM_U free figure never underflows") {
  static mk66::MemTables tables;
  mk66::fill_mem_tables(tables, 0u, mem::kSramUSize);   // an image that fills SRAM_U: free is 0, not a wrapped number
  static char text[2400];
  diag::format_mem(tables.report, text, sizeof text);
  CHECK(std::strstr(text, "free 0 B") != nullptr);
}
