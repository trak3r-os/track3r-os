// SPDX-License-Identifier: GPL-3.0-or-later
//
// Startup of the MK66 image: the flash configuration field, the vector table with every handler named, and the reset
// sequence. Order, and why (see boot.cpp,
// clock_config.h, sdramc_config.h, board.h for the sources of the clock sequence and the pin facts):
//
//   1. interrupts off; the watchdog unlocked and switched off first, inside the window the chip allows after reset
//   2. FPU on (the compiler may use it anywhere after this)
//   3. pins that must be safe: audio muted, LED off
//   4. the C runtime: .data and .ramfunc_l copied from flash, .bss cleared (.noinit left alone: the crash record)
//   5. the cycle counter, then the clock tree (crystal -> HSRUN -> PLL), failing visibly if a step does not complete
//   6. interrupt priorities and fault handlers, SysTick, interrupts on
//   7. the SDRAM and its tests (failing visibly), then the eDMA's global configuration (DMA_CR, every DCHPRI)
//   8. static constructors, then main
#include "internal.h"
#include "regs.h"

#include <o3os/hal/code_gate.h>
#include <o3os/platform/mk66/crash_record.h>
#include <o3os/platform/mk66/flash_config.h>
#include <o3os/platform/mk66/irq_list.h>
#include <o3os/platform/mk66/mem_map.h>
#include <o3os/platform/mk66/platform.h>

#include <cstddef>
#include <cstdint>

#ifndef O3OS_CODE_CACHE
#define O3OS_CODE_CACHE 0
#endif

// Linker script symbols (mk66.ld).
extern "C" {
extern std::uint32_t __data_load[];
extern std::uint32_t __data_start[];
extern std::uint32_t __data_end[];
extern std::uint32_t __ramfunc_l_load[];
extern std::uint32_t __ramfunc_l_start[];
extern std::uint32_t __ramfunc_l_end[];
extern std::uint32_t __bss_start[];
extern std::uint32_t __bss_end[];
extern std::uint32_t __stack_top[];
extern std::uint32_t __fault_stack_top[];
extern void (*__preinit_array_start[])();
extern void (*__preinit_array_end[])();
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void Reset_Handler();
void Default_Handler();
[[noreturn]] void o3os_fault_entry(const std::uint32_t* frame, std::uint32_t exc_return);  // crash_store.cpp
}

// `main` is reached through an asm label: C++ does not let a program call ::main by its own name.
extern "C" int o3os_target_main() __asm__("main");

namespace mk66 = o3os::platform::mk66;

// ---- Flash configuration field --------------------------------------------------------------------------------------
// Sixteen bytes at 0x400: `ff ff ff ff ff ff ff ff ff ff ff ff de f9 ff ff`. See flash_config.h before touching anything.
[[gnu::section(".flashconfig"), gnu::used]] const mk66::FlashConfig g_flash_config = mk66::kFlashConfig;

// ---- Exception entry stubs --------------------------------------------------------------------------------------------
// HardFault, MemManage, BusFault, UsageFault, NMI and every interrupt without a handler of its own end here. The stub
// finds the stack the exception frame was pushed on (EXC_RETURN bit 2), then moves to the fault stack before any C++
// runs, so that a stack overflow, the commonest cause of a crash, does not make the reporter itself fault, and passes the
// frame and EXC_RETURN to o3os_fault_entry (crash_store.cpp), which records and resets. Naked: nothing may touch the
// stack before r0 and r1 are read. It masks interrupts (PRIMASK) first, so no ISR can run on the fault stack's account;
// PRIMASK does not mask NMI or HardFault, so a fault inside the reporter re-enters this stub, which restarts on the same
// fault stack (the record may then be written twice; its CRC protects the reader).
//
// OS-CEPTFX S2b first: while a .ptfx call runs, a fault from thread mode on PSP whose frame lies in the plugin stack (or
// below it, or a stacking error) is contained instead — o3os_hal_gate_fault_contain records the fault fields, clears
// CFSR/HFSR, forges a fresh basic frame at the plugin stack top (PC = the gate's abort stub) and sets PSP to it. That
// path must NOT leave PRIMASK set (it survives the exception return) and must give MSP back as it was at entry (pending
// ISRs run on MSP after the return and must not find the 512 B fault stack), so it restores MSP from the word saved
// on the fault stack and re-enables interrupts before returning to thread mode on PSP (0xFFFFFFFD); every other
// fault takes the crash-record path unchanged. Unit check: a contained fault leaves MSP as before (the QEMU stub does
// not switch stacks, so only the unit exercises this).
extern "C" [[gnu::naked]] void Default_Handler() {
  asm volatile(
      "cpsid i\n\t"  // first: nothing of higher priority may preempt the reporter while it runs on the fault stack
      "tst lr, #4\n\t"
      "ite eq\n\t"
      "mrseq r0, msp\n\t"
      "mrsne r0, psp\n\t"
      "mov r1, lr\n\t"
      "mov r3, sp\n\t"  // the entry MSP, kept on the fault stack for the contained path
      "ldr r2, =__fault_stack_top\n\t"
      "mov sp, r2\n\t"
      "push {r0-r3}\n\t"  // four words: the fault stack stays 8-byte aligned
      "bl o3os_hal_gate_fault_contain\n\t"
      "cmp r0, #0\n\t"
      "bne 1f\n\t"
      "pop {r0-r3}\n\t"
      "b o3os_fault_entry\n\t"
      "1:\n\t"  // contained: give MSP back as it was at entry, then clear PRIMASK (set above)
      "ldr r3, [sp, #12]\n\t"
      "mov sp, r3\n\t"
      "cpsie i\n\t"
      "ldr r0, =0xFFFFFFFD\n\t"  // thread mode, PSP: pop the forged frame at the plugin stack top
      "bx r0\n\t"
      ".ltorg");
}

// The system handlers all share it; their names exist so the vector table and the symbol table say what they are.
extern "C" void NMI_Handler() __attribute__((weak, alias("Default_Handler")));
extern "C" void HardFault_Handler() __attribute__((weak, alias("Default_Handler")));
extern "C" void MemManage_Handler() __attribute__((weak, alias("Default_Handler")));
extern "C" void BusFault_Handler() __attribute__((weak, alias("Default_Handler")));
extern "C" void UsageFault_Handler() __attribute__((weak, alias("Default_Handler")));
extern "C" void SVC_Handler() __attribute__((weak, alias("Default_Handler")));  // weak: hal/src/code_gate_m4.cpp defines the real one (the OS-CEPTFX call gate)
extern "C" void DebugMon_Handler() __attribute__((weak, alias("Default_Handler")));
extern "C" void PendSV_Handler() __attribute__((weak, alias("Default_Handler")));
extern "C" void SysTick_Handler();  // clock.cpp

// One weak default per device interrupt: `<name>_IRQHandler`. A strong definition anywhere replaces it.
#define O3OS_WEAK_IRQ(number, name) extern "C" void name##_IRQHandler() __attribute__((weak, alias("Default_Handler")));
O3OS_MK66_IRQ_LIST(O3OS_WEAK_IRQ)
#undef O3OS_WEAK_IRQ

// ---- Reset sequence -------------------------------------------------------------------------------------------------
extern "C" void Reset_Handler() {
  namespace regs = mk66::regs;
  asm volatile("cpsid i" ::: "memory");

  // 1. Watchdog: unlock (two writes, back to back, within 20 bus cycles) and switch off, within 256 bus cycles of the
  //    unlock: the chip's own rule (ALLOWUPDATE only is written, which leaves WDOGEN clear but the
  //    control registers writable, so a later slice can enable the watchdog deliberately).
  regs::wdog_unlock() = regs::kWdogUnlock1;
  regs::wdog_unlock() = regs::kWdogUnlock2;
  asm volatile("nop\n\tnop" ::: "memory");
  regs::wdog_stctrlh() = regs::kWdogAllowUpdate;

  // 2. FPU: CP10 and CP11 full access.
  regs::scb_cpacr() = regs::scb_cpacr() | regs::kCpacrFullFpu;
  asm volatile("dsb\n\tisb" ::: "memory");

  // 3. Pins that must be safe from the first instruction on: the power key PTA24 first, the mute key at once, a bounded wait of
  //    board::kRailSettleUs on the cycle counter (the rail's ramp is not measured), then the other quiet pins (boot.cpp early_pins,
  //    OS-BUG-PADS step 3). Then the crash context: stage Reset, no module, so that a crash
  //    this early does not carry the previous run's stage and module (it touches `.noinit` only, no C runtime needed).
  mk66::boot::early_pins();
  mk66::boot::reset_context();

  // 4. C runtime. The loops must stay loops (startup.cpp is compiled with -fno-tree-loop-distribute-patterns).
  {
    const std::uint32_t* src = __data_load;
    for (std::uint32_t* dst = __data_start; dst < __data_end;) {
      *dst++ = *src++;
    }
    src = __ramfunc_l_load;
    for (std::uint32_t* dst = __ramfunc_l_start; dst < __ramfunc_l_end;) {
      *dst++ = *src++;
    }
    for (std::uint32_t* dst = __bss_start; dst < __bss_end;) {
      *dst++ = 0;
    }
    // Code was just written as data: make the instruction stream see it.
    asm volatile("dsb\n\tisb" ::: "memory");
  }
#if O3OS_CODE_CACHE
  // 4b. The code cache (lmem.h): flash code and constants then run at cache speed. Both ways invalidated and the cache on, one store.
  (void)mk66::cache::enable();
#endif
  mk66::set_stage(mk66::crash::Stage::Memory);
  mk66::boot::capture_reset_cause();
  mk66::boot::capture_boot_record();

  // 5. Clocks.
  mk66::set_stage(mk66::crash::Stage::Clocks);
  mk66::boot::enable_dwt();
  mk66::boot::init_clocks();

  // 6. Interrupts.
  mk66::set_stage(mk66::crash::Stage::Interrupts);
  mk66::boot::init_interrupts();
  mk66::boot::start_systick();
  asm volatile("cpsie i" ::: "memory");

  // 7. SDRAM.
  mk66::set_stage(mk66::crash::Stage::Sdram);
  mk66::boot::init_sdram();
  mk66::boot::dma_init();  // the eDMA's global configuration, once, before any driver (the display, the audio engine)

  // 8. Constructors and main.
  for (void (**f)() = __preinit_array_start; f < __preinit_array_end; ++f) {
    (*f)();
  }
  for (void (**f)() = __init_array_start; f < __init_array_end; ++f) {
    (*f)();
  }
  mk66::set_stage(mk66::crash::Stage::Running);
  o3os_target_main();

  // main never returns; if it ever does, that is a bug worth a crash record, not a silent spin.
  mk66::panic();
}

// ---- C++ runtime hooks ----------------------------------------------------------------------------------------------
// This image has no heap, no exceptions and no atexit. A pure virtual call can only be a bug: record it and reset.
extern "C" void __cxa_pure_virtual() { mk66::panic(); }
// Objects with destructors are never registered for exit: static storage lives for the whole run.
extern "C" {
void* __dso_handle = nullptr;
}
extern "C" int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }

// ---- Vector table ---------------------------------------------------------------------------------------------------
namespace {

using Handler = void (*)();

// Entry 0 is the initial stack pointer; 1..15 the Cortex-M4 system handlers (ARMv7-M ARM, B1.5.2; 7-10 and 13 are
// reserved); then the device interrupts in the order of irq_list.h.
#define O3OS_VECTOR_IRQ(number, name) name##_IRQHandler,

[[gnu::section(".vectors"), gnu::used]] const Handler kVectors[16 + mk66::kIrqCount] = {
    reinterpret_cast<Handler>(__stack_top),
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    nullptr,  // reserved
    nullptr,
    nullptr,
    nullptr,
    SVC_Handler,
    DebugMon_Handler,
    nullptr,  // reserved
    PendSV_Handler,
    SysTick_Handler,
    O3OS_MK66_IRQ_LIST(O3OS_VECTOR_IRQ)
};
#undef O3OS_VECTOR_IRQ

static_assert(sizeof(kVectors) == mk66::mem::kVectorTableBytes, "the table is the size the linker script reserves below the flash config");

}  // namespace
