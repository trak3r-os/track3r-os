// SPDX-License-Identifier: GPL-3.0-or-later
//
// Startup of the QEMU `mps2-an386` target test layer (Cortex-M4F): vector table, reset sequence, and the fault
// reporter. The reset sequence is the one a bare-metal C++ program needs on any Cortex-M: enable the FPU, copy
// .data from its load image, zero .bss, paint the thread stack (for the high-water mark), arm the stack guard, move
// the program onto the thread stack (PSP; MSP stays for exceptions), run the static constructors, call main, and
// leave through exit() so the static destructors and stdio flush run.
#include <o3os/hal/code_gate.h>
#include <o3os/platform/qemu/runtime.h>
#include <o3os/platform/qemu/semihost.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

// Linker script symbols (mps2_an386.ld).
extern "C" {
extern std::uint32_t __data_load[];
extern std::uint32_t __data_start[];
extern std::uint32_t __data_end[];
extern std::uint32_t __bss_start[];
extern std::uint32_t __bss_end[];
extern std::uint32_t __stack_top[];         // initial MSP: top of the exception stack
extern std::uint32_t __thread_stack_top[];  // initial PSP
extern std::uint32_t __stack_limit[];       // lowest address of the thread stack
extern std::uint32_t __guard_base[];        // the MPU no-access region is [__guard_base, __stack_limit)
extern void (*__preinit_array_start[])();
extern void (*__preinit_array_end[])();
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void Reset_Handler();
void Default_Handler();
void SysTick_Handler();  // clock.cpp
void SVC_Handler();      // hal/src/code_gate_m4.cpp: the OS-CEPTFX call gate's return
// Replaceable by a strong definition (the fake I2S of the audio suite, src/fake_i2s.cpp): the render line and the block clock.
void PendSV_Handler();
void TIMER0_IRQHandler();
void STIR_IRQHandler();
void fault_report(const std::uint32_t* frame);
[[noreturn]] void target_run();
}

// `main` is reached through an asm label: C++ does not let a program call ::main by its own name.
extern "C" int o3os_target_main() __asm__("main");

namespace {

volatile std::uint32_t& reg(std::uintptr_t address) { return *reinterpret_cast<volatile std::uint32_t*>(address); }

constexpr std::uintptr_t kScbCpacr = 0xE000ED88;
constexpr std::uintptr_t kScbShcsr = 0xE000ED24;
constexpr std::uintptr_t kMpuType = 0xE000ED90;
constexpr std::uintptr_t kMpuCtrl = 0xE000ED94;
constexpr std::uintptr_t kMpuRnr = 0xE000ED98;
constexpr std::uintptr_t kMpuRbar = 0xE000ED9C;
constexpr std::uintptr_t kMpuRasr = 0xE000EDA0;
constexpr std::uintptr_t kScbCfsr = 0xE000ED28;
constexpr std::uintptr_t kScbHfsr = 0xE000ED2C;
constexpr std::uintptr_t kScbMmfar = 0xE000ED34;
constexpr std::uintptr_t kScbBfar = 0xE000ED38;
constexpr std::uint32_t kCpacrFullFpu = 0xFu << 20;  // CP10 and CP11 full access
constexpr std::uint32_t kShcsrMemFaultEnable = 1u << 16;
constexpr std::uint32_t kMpuCtrlEnable = 1u << 0;
constexpr std::uint32_t kMpuCtrlPrivDefEna = 1u << 2;  // privileged code keeps the default map outside the regions
constexpr std::uint32_t kMpuRasrEnable = 1u << 0;
constexpr std::uint32_t kMpuRasrXn = 1u << 28;         // AP field (bits 26:24) stays 0: no access at all
constexpr std::uint32_t kCfsrMmarValid = 1u << 7;

// Overwrites the first `count` runs of eight 'x' in `line` with the values, as hex.
void fill_hex(char* line, const std::uint32_t* values, std::size_t count) {
  char* p = line;
  for (std::size_t n = 0; n < count; ++n) {
    while (*p != 'x') {
      ++p;
    }
    std::uint32_t value = values[n];
    for (int i = 7; i >= 0; --i) {
      p[i] = "0123456789abcdef"[value & 0xF];
      value >>= 4;
    }
    p += 8;
  }
}

// An MPU no-access region over the guard below the thread stack. The first load or store into it is a MemManage
// fault, reported by fault_report() as a stack overflow. It catches a frame that is large but not larger than the
// guard; the paint check in the runner catches the rest after the fact, when the overflow stayed inside the stack.
void arm_stack_guard() {
  if (((reg(kMpuType) >> 8) & 0xFF) == 0) {
    return;  // this core has no MPU
  }
  const std::uint32_t guard_bytes = static_cast<std::uint32_t>(__stack_limit - __guard_base) * sizeof(std::uint32_t);
  const std::uint32_t size_field = 31u - static_cast<std::uint32_t>(__builtin_clz(guard_bytes)) - 1u;  // 2^(n+1) bytes
  reg(kMpuRnr) = 0;
  reg(kMpuRbar) = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__guard_base));
  reg(kMpuRasr) = kMpuRasrXn | (size_field << 1) | kMpuRasrEnable;
  reg(kMpuCtrl) = kMpuCtrlPrivDefEna | kMpuCtrlEnable;  // HFNMIENA stays clear: the HardFault handler runs unguarded
  reg(kScbShcsr) = reg(kScbShcsr) | kShcsrMemFaultEnable;
  asm volatile("dsb\n\tisb" ::: "memory");
}

}  // namespace

// ---- Reset ----------------------------------------------------------------------------------------------------

extern "C" [[gnu::naked, noreturn]] void enter_thread();

extern "C" void Reset_Handler() {
  // FPU first: the compiler may use it anywhere, including in the copy loops below.
  reg(kScbCpacr) = reg(kScbCpacr) | kCpacrFullFpu;
  asm volatile("dsb\n\tisb" ::: "memory");

  // The loops below are the C runtime itself: keep the compiler from turning them back into memcpy / memset calls
  // that a libc could in principle expect to be initialised.
  const std::uint32_t* src = __data_load;
  for (std::uint32_t* dst = __data_start; dst < __data_end;) {
    *dst++ = *src++;
  }
  for (std::uint32_t* dst = __bss_start; dst < __bss_end;) {
    *dst++ = 0;
  }

  // Paint the whole thread stack so runtime.cpp can find the deepest point reached (this code runs on MSP, which
  // is outside it).
  for (std::uint32_t* p = __stack_limit; p < __thread_stack_top; ++p) {
    *p = o3os::platform::qemu::kStackPaint;
  }

  arm_stack_guard();

  // Everything from here on, constructors and main included, runs on the thread stack (PSP); MSP is kept for
  // exceptions. A naked function does the switch: nothing may use this frame's locals once SP has changed.
  enter_thread();
}

// Switches to PSP and never comes back to Reset_Handler.
extern "C" void enter_thread() {
  asm volatile(
      "ldr r0, =__thread_stack_top\n\t"
      "msr psp, r0\n\t"
      "movs r0, #2\n\t"  // CONTROL.SPSEL = 1: thread mode uses PSP
      "msr control, r0\n\t"
      "isb\n\t"
      "bl target_run");
}

extern "C" void target_run() {
  for (void (**f)() = __preinit_array_start; f < __preinit_array_end; ++f) {
    (*f)();
  }
  for (void (**f)() = __init_array_start; f < __init_array_end; ++f) {
    (*f)();
  }

  std::exit(o3os_target_main());
}

// ---- Faults ---------------------------------------------------------------------------------------------------

// Any exception without its own handler (all of them but SysTick, SVCall, Reset and the weak ones below) ends here. The hardware
// pushed the frame on the stack in use; pass it to the reporter. Naked: nothing may touch the stack before r0 is read.
//
// OS-CEPTFX S2b first: while a .ptfx call runs, a fault from thread mode on PSP whose frame lies in the plugin stack (or below
// it, or a stacking error) is contained instead — o3os_hal_gate_fault_contain records the fault fields, clears CFSR/HFSR, forges
// a fresh basic frame at the plugin stack top (PC = the gate's abort stub) and sets PSP to it, and this stub returns to thread
// mode on PSP (0xFFFFFFFD) so the abort stub runs. Every other fault goes to the reporter unchanged.
extern "C" [[gnu::naked]] void Default_Handler() {
  asm volatile(
      "tst lr, #4\n\t"
      "ite eq\n\t"
      "mrseq r0, msp\n\t"
      "mrsne r0, psp\n\t"
      "mov r1, lr\n\t"
      "push {r0, r1}\n\t"
      "bl o3os_hal_gate_fault_contain\n\t"
      "cmp r0, #0\n\t"
      "pop {r0, r1}\n\t"
      "beq fault_report\n\t"
      "ldr r0, =0xFFFFFFFD\n\t"  // contained: thread mode, PSP: pop the forged frame at the plugin stack top
      "bx r0\n\t"
      ".ltorg");
}

// PendSV and the first timer's interrupt (IRQ 8) are weak aliases of the default handler: a suite that links src/fake_i2s.cpp
// replaces them, every other suite still reports a stray one as a fault.
extern "C" void PendSV_Handler() __attribute__((weak, alias("Default_Handler")));
extern "C" void TIMER0_IRQHandler() __attribute__((weak, alias("Default_Handler")));
// IRQ 31 is the second render line of the audio fake: pended by NVIC_STIR, as the MK66's IRQ 64 is (src/fake_i2s.cpp).
extern "C" void STIR_IRQHandler() __attribute__((weak, alias("Default_Handler")));

// "o3os-qemu: FAULT ipsr=xx cfsr=xxxxxxxx hfsr=xxxxxxxx mmfar=xxxxxxxx bfar=xxxxxxxx pc=xxxxxxxx lr=xxxxxxxx" and exit
// status 2, so a crashing suite fails at once instead of waiting for the ctest timeout. A touch of the stack guard
// (MMFAR inside it, or the exception frame itself inside it because stacking onto the overflowed PSP failed) is
// reported as such and ends with status 3, like the runner's paint check. The frame is then not read: it is not there.
extern "C" void fault_report(const std::uint32_t* frame) {
  std::uint32_t ipsr;
  asm volatile("mrs %0, ipsr" : "=r"(ipsr));
  const std::uint32_t cfsr = reg(kScbCfsr);
  const std::uint32_t mmfar = reg(kScbMmfar);
  const std::uintptr_t guard_low = reinterpret_cast<std::uintptr_t>(__guard_base);
  const std::uintptr_t guard_high = reinterpret_cast<std::uintptr_t>(__stack_limit);
  const auto in_guard = [&](std::uintptr_t address) { return address >= guard_low && address < guard_high; };

  if (in_guard(reinterpret_cast<std::uintptr_t>(frame)) || ((cfsr & kCfsrMmarValid) != 0 && in_guard(mmfar))) {
    char line[] = "o3os-qemu: FAULT stack overflow: the stack guard was touched (ipsr=xxxxxxxx cfsr=xxxxxxxx mmfar=xxxxxxxx sp=xxxxxxxx)\n";
    const std::uint32_t values[] = {ipsr, cfsr, mmfar, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(frame))};
    fill_hex(line, values, 4);
    o3os::platform::qemu::console_write(line, sizeof line - 1);
    o3os::platform::qemu::exit_qemu(3);
  }

  char line[] = "o3os-qemu: FAULT ipsr=xxxxxxxx cfsr=xxxxxxxx hfsr=xxxxxxxx mmfar=xxxxxxxx bfar=xxxxxxxx pc=xxxxxxxx lr=xxxxxxxx\n";
  const std::uint32_t values[] = {ipsr, cfsr, reg(kScbHfsr), mmfar, reg(kScbBfar), frame[6], frame[5]};
  fill_hex(line, values, 7);
  o3os::platform::qemu::console_write(line, sizeof line - 1);
  o3os::platform::qemu::exit_qemu(2);
}

// ---- Vector table ---------------------------------------------------------------------------------------------

namespace {

using Handler = void (*)();

#define O3OS_X4 Default_Handler, Default_Handler, Default_Handler, Default_Handler
#define O3OS_X16 O3OS_X4, O3OS_X4, O3OS_X4, O3OS_X4

// ARMv7-M exceptions 1-15, then 32 external interrupts (nothing enables one; a stray one is reported, not run
// off the end of the table). Entry 0 is the initial stack pointer.
[[gnu::section(".isr_vector"), gnu::used]] const Handler kVectors[16 + 32] = {
    reinterpret_cast<Handler>(__stack_top),
    Reset_Handler,
    Default_Handler,  // NMI
    Default_Handler,  // HardFault
    Default_Handler,  // MemManage
    Default_Handler,  // BusFault
    Default_Handler,  // UsageFault
    nullptr,          // reserved
    nullptr,
    nullptr,
    nullptr,
    SVC_Handler,      // SVCall: the OS-CEPTFX call gate's return (hal/src/code_gate_m4.cpp)
    Default_Handler,  // DebugMonitor
    nullptr,          // reserved
    PendSV_Handler,
    SysTick_Handler,
    O3OS_X4, O3OS_X4,   // IRQ 0-7
    TIMER0_IRQHandler,  // IRQ 8: CMSDK timer 0 of the mps2 board
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, Default_Handler, Default_Handler, Default_Handler,  // IRQ 9-15
    O3OS_X4, O3OS_X4, O3OS_X4, Default_Handler, Default_Handler, Default_Handler,  // IRQ 16-30
    STIR_IRQHandler,  // IRQ 31: software-pended through NVIC_STIR
};

}  // namespace
