// SPDX-License-Identifier: GPL-3.0-or-later

// OS-FLASHDIET P5/P8: the image's text pools (text.h): the inflated console pool (and, in a bench image, the bench pool) in the SDRAM slack window, filled on the
// first use. Main loop only, never the render, an ISR or SRAM_L (SDRAM, and a decode on the first call). The pools are written once, by the first call, and read
// after it; the state flags are in .bss (a noinit buffer's own contents are undefined at power-up, so they cannot say whether it was filled).

#include <o3os/platform/mk66/bench_text_ids.h>
#include <o3os/platform/mk66/console_text_ids.h>
#include <o3os/platform/mk66/sections.h>
#include <o3os/platform/mk66/text.h>

namespace o3os::platform::mk66 {
namespace {

enum class PoolState : std::uint8_t { Empty, Ready, Lost };

// A pool that did not decode answers with this (the blob is checked at build time, so only a flash fault gets here).
constexpr char kLost[] = "(text lost)";

const char* pooled(char* pool, std::size_t raw, PoolState& state, const std::uint8_t* z, std::size_t z_size, std::uint16_t id) noexcept {
  if (state == PoolState::Empty) {
    state = text_unpack(text_inflater(), z, z_size, pool, raw) ? PoolState::Ready : PoolState::Lost;
  }
  return state == PoolState::Ready && id < raw ? pool + id : kLost;
}

alignas(4) O3OS_SDRAM_NOINIT char g_console_pool[txt::kRawSize];
PoolState g_console_state = PoolState::Empty;

#if O3OS_BENCH
alignas(4) O3OS_SDRAM_NOINIT char g_bench_pool[btxt::kRawSize];
PoolState g_bench_state = PoolState::Empty;
#endif

}  // namespace

const char* text(std::uint16_t id) noexcept { return pooled(g_console_pool, txt::kRawSize, g_console_state, kConsoleTextZ, kConsoleTextZSize, id); }

#if O3OS_BENCH
const char* bench_text(std::uint16_t id) noexcept { return pooled(g_bench_pool, btxt::kRawSize, g_bench_state, kBenchTextZ, kBenchTextZSize, id); }
#endif

}  // namespace o3os::platform::mk66
