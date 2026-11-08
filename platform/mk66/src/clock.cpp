// SPDX-License-Identifier: GPL-3.0-or-later
//
// hal::Clock on SysTick (1 kHz from the 180 MHz core clock) and hal::cycles() on the DWT cycle counter. The clock is
// the method of the QEMU layer's (platform/qemu/clock.cpp): a millisecond count kept by the SysTick handler, and the
// down-counter inside the current millisecond for microseconds.
#include "internal.h"
#include "regs.h"

#include <o3os/hal/code_gate.h>
#include <o3os/hal/cycles.h>
#include <o3os/platform/mk66/clock_config.h>
#include <o3os/platform/mk66/platform.h>

namespace o3os::platform::mk66 {
namespace {

constexpr std::uint32_t kReload = clock::kSysTickReload + 1u;                 // counts per millisecond
constexpr std::uint32_t kCountsPerUs = clock::kCoreHz / 1'000'000u;
// If the counter wraps between the CVR read and the ICSR read in now(), CVR is still within a few cycles of zero.
constexpr std::uint32_t kWrapRaceCounts = 64;
static_assert(clock::kCoreHz % 1'000'000u == 0u, "whole counts per microsecond");
static_assert(kReload == clock::kCoreHz / clock::kSysTickHz, "one reload per millisecond");

// Written by SysTick_Handler only; read with interrupts masked (a 64-bit access is two words).
volatile std::uint64_t g_uptime_ms = 0;
// Last value now() returned; read and written only inside now()'s critical section.
core::Tick g_last_now = 0;

// PRIMASK save / restore: a critical section that also works when called from an ISR.
std::uint32_t irq_save() noexcept {
  std::uint32_t primask;
  asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
  return primask;
}
void irq_restore(std::uint32_t primask) noexcept { asm volatile("msr primask, %0" ::"r"(primask) : "memory"); }

}  // namespace

// The SysTick entry. The OS-CEPTFX call gate's check runs first, while it costs one load and one branch (the gate's
// active flag); only while a call runs is the hook called, with the EXC_RETURN of the interrupted context (in lr
// only at entry, so this is naked). Then the uptime. The gate may have rewritten the interrupted frame's PC to its
// abort stub; the return below still goes back to the (rewritten) context.
extern "C" [[gnu::naked]] void SysTick_Handler() {
  asm volatile(
      "push {r4, lr}\n\t"  // r4 keeps the 8-byte stack alignment of the calls below
      "ldr r0, =o3os_hal_gate_active\n\t"
      "ldr r0, [r0]\n\t"
      "cbz r0, 1f\n\t"
      "mov r0, lr\n\t"  // the hook's argument: the EXC_RETURN of the interrupted context
      "bl o3os_hal_gate_systick\n\t"
      "1:\n\t"
      "bl o3os_systick_uptime\n\t"
      "pop {r4, pc}\n\t"
      ".ltorg");
}
extern "C" void o3os_systick_uptime() { g_uptime_ms = g_uptime_ms + 1; }

core::Tick SysTickClock::now() const noexcept {
  const std::uint32_t primask = irq_save();
  std::uint32_t ms = static_cast<std::uint32_t>(g_uptime_ms);
  const std::uint32_t count = regs::syst_cvr();
  const bool wrapped = (regs::scb_icsr() & regs::kIcsrPendstset) != 0u;

  // The counter wrapped while interrupts were masked: its handler has not counted that millisecond yet. Not when the
  // wrap fell between the two register reads above (CVR was read just before it, so it is near zero).
  if (wrapped && count > kWrapRaceCounts) {
    ms += 1u;
  }
  const std::uint32_t elapsed_counts = (kReload - 1u) - count;  // counts since this millisecond began
  const core::Tick candidate = ms * core::kTicksPerMs + elapsed_counts / kCountsPerUs;

  // hal::Clock promises a non-decreasing now(); the reads above are exact on this core, but the guard costs two
  // instructions and makes the promise independent of how the pending flag and the counter reload interleave.
  const core::Tick result = core::tick_reached(candidate, g_last_now) ? candidate : g_last_now;
  g_last_now = result;
  irq_restore(primask);
  return result;
}

std::uint64_t SysTickClock::uptime_ms() const noexcept {
  const std::uint32_t primask = irq_save();
  const std::uint64_t ms = g_uptime_ms;
  irq_restore(primask);
  return ms;
}

SysTickClock& system_clock() noexcept {
  static SysTickClock clock;
  return clock;
}

namespace boot {
std::uint32_t uptime_ms_low() noexcept { return static_cast<std::uint32_t>(g_uptime_ms); }
}  // namespace boot

}  // namespace o3os::platform::mk66

namespace o3os::hal {
std::uint32_t cycles() noexcept { return platform::mk66::regs::dwt_cyccnt(); }
}  // namespace o3os::hal
