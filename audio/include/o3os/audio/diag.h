// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// `diag cpu` and `diag mem` as pure text formatters (ADR 0011 item 1, ADR 0013 D6/D8). There is no console yet (the CDC
// `diag` is OS-006b/007); these turn the status and the memory map into the lines it will print, so they are unit-tested now
// and bind to the console later. No printf (it costs kilobytes of flash and may allocate): a bounded text builder with the
// few conversions needed. Nothing here runs on the real-time path.

#include <o3os/audio/master_limiter.h>
#include <o3os/audio/meters.h>
#include <o3os/audio/module_perf.h>
#include <o3os/audio/status.h>

#include <cstddef>
#include <cstdint>

namespace o3os::audio::diag {

// Appends to a caller-supplied buffer: never writes past `cap`, always leaves a NUL terminator (cap >= 1), and remembers
// that it ran out. Every method returns *this so a line is one expression.
class Text {
 public:
  Text(char* buf, std::size_t cap) noexcept;
  Text& str(const char* s) noexcept;
  Text& chr(char c) noexcept;
  Text& dec(std::uint32_t v) noexcept;
  Text& hex32(std::uint32_t v) noexcept;                 // 0x%08X
  Text& percent(std::uint64_t part, std::uint64_t whole) noexcept;  // "1.6 %": one decimal, rounded down; "-" if whole is 0
  Text& kib(std::uint32_t bytes) noexcept;               // "11456 KiB", or "1536 B" when not a whole KiB
  Text& nl() noexcept { return chr('\n'); }
  std::size_t size() const noexcept { return len_; }
  bool truncated() const noexcept { return truncated_; }

 private:
  char* buf_;
  std::size_t cap_;
  std::size_t len_ = 0;
  bool truncated_ = false;
};

// `diag cpu`. `cycles_per_block` is the block period in the units of the status' cycle figures.
std::size_t format_cpu(const StatusSnapshot& s, std::uint32_t cycles_per_block, char* out, std::size_t cap) noexcept;

// `diag cpu`, the line after format_cpu (OS-MAINGUARD): the main-loop starvation guard's shed blocks since boot or `cpu reset` (the render zeroes them at its next block), how many of
// them were late (their slot was already playing), the shed runs, and the longest run since boot (the main pass's cost in blocks when the guard is working).
// `diag cpu` and the HIL gates read "guard  shed 0 blocks": anything else under a normal load is a finding. The line ends with the window in force, "n <window>" (Pipeline::starve_blocks(), `guard n`).
std::size_t format_shed(std::uint32_t blocks, std::uint32_t late, std::uint32_t runs, std::uint32_t longest, std::uint32_t window, char* out, std::size_t cap) noexcept;

// `diag cpu`, a line after the guard's, only once the bake pool has an entry or a refusal (OS-CEWTENV W3c): `wtbake  used <u>/<n>  refused <r> calls` (no newline, as format_app_tap). `refused` counts wt_bake CALLS that found
// the pool full (a rebuild counts once a slot): those slots play their warps live, the cost the baking saves comes back.
std::size_t format_wt_bake(std::uint32_t used, std::uint32_t capacity, std::uint32_t refused, char* out, std::size_t cap) noexcept;

// `diag mix` (OS-005b): the meters' last window as dBFS lines, one per channel that has sounded (a hold above zero) and always the master
// pair. The names are the routing matrix's: T01..T16, IN L/R, MAIN, CUE, SNDA, SNDB, REC, MST.
std::size_t format_mix(const MeterSnapshot& s, char* out, std::size_t cap) noexcept;
// The same with the master limiter's gain reduction (OS-LIMIT) as the line right after the header, whatever the meters have measured. THE FORMAT IS STABLE (the HIL scenarios grep it):
//   limiter: gr <x.x> dB max <y.y> dB, mode <subtle|hard>, ceiling -0.3 dBFS
// `gr` is the reduction of the last meter window (the lowest gain in it) and `max` the largest of any window since boot, in dB as positive numbers with one decimal (0.0: none; held to 99.9);
// `mode` is the project's LIMITER MODE (hard is the UI's "Extreme"); the ceiling is MasterLimiter::kCeiling. At most 64 columns. MAIN's lines then mean the sum before the limiter.
std::size_t format_mix(const MeterSnapshot& s, const LimiterReport& limiter, char* out, std::size_t cap) noexcept;

// `diag voices` (OS-VOX): the voices in use of `pool`, the most in use at the end of a block since boot, the steals since boot, and (OS-ODD11 slice 2) the voice spans the closed-form advance took since boot
// (a muted plain voice advanced instead of rendered; 0 when no track was muted, a counter that rises while one is). THE LINE'S START IS STABLE (the HIL parser reads it): `voices  in use <n>/<pool>  peak <p>  steals <s>`,
// then `  advanced <a>`.
std::size_t format_voices(std::uint32_t in_use, std::uint32_t pool, std::uint32_t peak, std::uint32_t steals, std::uint32_t advanced, char* out, std::size_t cap) noexcept;

// A line of `diag cpu` (OS-H2a): the apps' audio tap, no trailing newline. THE FORMAT IS STABLE (the HIL scenarios grep it):
//   tap: <off|input|master>, <n> blocks, write cycles last <a> mean <b> max <c> (max <x.x> us)
// `source` is AppTap::Source as a number; the cycles are those of the whole tap work of a block (mix, level stats, ring write) as the render timed it, over the blocks since boot or `cpu reset`;
// the figures are published every 16 blocks. With none yet: `tap: <source>, no block timed`. About 90 characters.
std::size_t format_app_tap(unsigned source, std::uint32_t blocks, std::uint32_t last, std::uint32_t sum, std::uint32_t max, std::uint32_t cpu_hz, char* out, std::size_t cap) noexcept;

// A line of `diag mod` (OS-303PERF), no trailing newline: what the render measured of one module instance, full blocks only (module_perf.h). THE FORMAT IS STABLE (the HIL scenarios grep it):
//   mod: perf <instrument 1..64>: <n> blocks, loop min <a> mean <b> max <c>, render min <a> mean <b> max <c>, out mean <b>, hooks <b>
// All figures are cycles of the 180 MHz counter. `loop` is the instance's whole iteration in the render, `render` the module's render() alone, `out` the zero-fill and the conversion, `hooks` the
// rest (enter, leave). With no full block yet: `mod: perf <instrument>: no full block measured`. About 130 characters with five-digit figures (the console's line buffer is 160).
std::size_t format_module_perf(unsigned instrument, const ModulePerf& p, char* out, std::size_t cap) noexcept;

struct MemRow {  // a region with an address (an SDRAM region)
  const char* name;
  std::uint32_t base;
  std::uint32_t size;
};
struct BudgetRow {  // a line of an SRAM budget
  const char* name;
  std::uint32_t bytes;
};
struct MemReport {
  const MemRow* sdram;
  unsigned sdram_count;
  std::uint32_t sdram_total;       // bytes the part has
  const BudgetRow* sram_l;         // the ADR 0013 D7 budget of the code bus SRAM
  unsigned sram_l_count;
  std::uint32_t sram_l_size;
  std::uint32_t sram_l_hot_used;   // bytes of .ramfunc_l the image has
  std::uint32_t sram_l_hot_budget;
  std::uint32_t sram_l_data_used;  // bytes of .tapmem_l the image has (the audio taps' memory)
  std::uint32_t sram_l_data_budget;
  const BudgetRow* sram_u;         // the ADR 0013 D7 budget of the system bus SRAM
  unsigned sram_u_count;
  std::uint32_t sram_u_size;
  std::uint32_t sram_u_used;       // .noinit + .data + .bss the image has
  std::uint32_t sram_u_stack;
  std::uint32_t sram_u_min_free;
};

// `diag mem`.
std::size_t format_mem(const MemReport& m, char* out, std::size_t cap) noexcept;

}  // namespace o3os::audio::diag
