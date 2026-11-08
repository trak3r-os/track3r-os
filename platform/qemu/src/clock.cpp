// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/hal/code_gate.h>
#include <o3os/platform/qemu/clock.h>

namespace o3os::platform::qemu {
namespace {

// Cortex-M system control block and SysTick registers (ARMv7-M ARM, B3.3 and B3.4).
volatile std::uint32_t& reg(std::uintptr_t address) { return *reinterpret_cast<volatile std::uint32_t*>(address); }
constexpr std::uintptr_t kSysTickCsr = 0xE000E010;
constexpr std::uintptr_t kSysTickRvr = 0xE000E014;
constexpr std::uintptr_t kSysTickCvr = 0xE000E018;
constexpr std::uintptr_t kScbIcsr = 0xE000ED04;
constexpr std::uint32_t kCsrEnable = 1u << 0;
constexpr std::uint32_t kCsrTickInt = 1u << 1;
constexpr std::uint32_t kCsrClkSource = 1u << 2;  // processor clock
constexpr std::uint32_t kIcsrPendStSet = 1u << 26;

constexpr std::uint32_t kReload = SysTickClock::kCoreHz / SysTickClock::kTickHz;  // counts per millisecond
constexpr std::uint32_t kCountsPerUs = SysTickClock::kCoreHz / 1'000'000;
// If the counter wraps between the CVR read and the ICSR read in now(), CVR is still within a few cycles of zero.
constexpr std::uint32_t kWrapRaceCounts = 64;
static_assert(kReload - 1 <= 0x00FFFFFFu, "SysTick reload is 24 bits");
static_assert(SysTickClock::kCoreHz % 1'000'000 == 0, "whole counts per microsecond");
static_assert(SysTickClock::kCoreHz % SysTickClock::kTickHz == 0, "whole counts per millisecond");

// Written by SysTick_Handler only; read with interrupts masked.
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

// The SysTick entry, as on the MK66 (platform/mk66/src/clock.cpp): the OS-CEPTFX call gate's check first, one load
// and one branch while the gate is idle; the hook gets the EXC_RETURN of the interrupted context before anything
// clobbers lr, so this is naked. Then the uptime.
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

void SysTickClock::start() noexcept {
  const std::uint32_t primask = irq_save();
  g_uptime_ms = 0;
  g_last_now = 0;
  reg(kSysTickRvr) = kReload - 1;
  reg(kSysTickCvr) = 0;  // any write clears the counter
  reg(kSysTickCsr) = kCsrEnable | kCsrTickInt | kCsrClkSource;
  irq_restore(primask);
}

core::Tick SysTickClock::now() const noexcept {
  const std::uint32_t primask = irq_save();
  std::uint32_t ms = static_cast<std::uint32_t>(g_uptime_ms);
  const std::uint32_t count = reg(kSysTickCvr);
  const bool wrapped = (reg(kScbIcsr) & kIcsrPendStSet) != 0;

  // The counter wrapped while interrupts were masked: its handler has not counted that millisecond yet. Not when the
  // wrap fell between the two register reads above (CVR was read just before it, so it is near zero).
  if (wrapped && count > kWrapRaceCounts) {
    ms += 1;
  }
  const std::uint32_t elapsed_counts = (kReload - 1) - count;  // counts since this millisecond began
  const core::Tick candidate = ms * core::kTicksPerMs + elapsed_counts / kCountsPerUs;

  // hal::Clock promises a non-decreasing now(). On the MK66 the counter reload and the pending flag change together,
  // so the code above is exact. QEMU's SysTick sets the flag from a timer callback that can run after the guest has
  // already read the reloaded counter, and the clock then lags by up to a read (more under a loaded host, unless
  // QEMU runs with -icount, which O3osQemu.cmake does): never hand that out.
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

}  // namespace o3os::platform::qemu
