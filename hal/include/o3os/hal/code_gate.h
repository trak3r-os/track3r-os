// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The call gate of OS-CEPTFX (docs/specs/OS-CEPTFX.md section 3.3, assumptions A5-A7): the one way foreign
// position-independent Cortex-M4F code (a `.ptfx` plugin loaded into the SDRAM region) is ever executed. The plugin
// runs in unprivileged thread mode on its own stack (PSP) and returns through SVC; the host's callee-saved registers
// (r4-r11, s16-s31), FPSCR and its stack are saved and restored around every call, so the call comes back whatever
// the plugin does: it may return, hang (SysTick aborts it at the deadline), fault (the fault stub contains it) or
// run off its stack (the guard).
//
// Three implementations of the same contract:
//   * host (the tests and the simulator): the inline definition below calls the function pointer directly, with
//     no containment at all;
//   * MK66 and QEMU: one shared Cortex-M4F source, hal/src/code_gate_m4.cpp (enter, return stub, SVCall handler,
//     resume point, and the SysTick / fault-stub hooks the platform handlers call), compiled into `hal` on every
//     cross build.
//
// NEVER call the gate inside a critical section: SVCall has a fixed priority, so a PRIMASK/BASEPRI held around
// the call would mask the plugin's own return through SVC. The main loop is the gate state's only writer before
// the call; the handlers write only the reason and the fault fields.

#include <cstdint>

namespace o3os::hal::code_gate {

// Why the last call ended.
enum class GateResult : std::uint8_t {
  Ok,             // the plugin returned through SVC
  Fault,          // it faulted; last_fault() holds CFSR, the stacked PC, BFAR/MMFAR and HFSR
  Timeout,        // SysTick rewrote the interrupted PC to the abort stub at the deadline
  StackOverflow,  // it left its stack (the guard at resume, or SysTick saw the PSP below the stack)
  Unavailable,    // the arguments were unusable (no entry, not Thumb, bad stack bounds): nothing ran
};

// What the fault stub recorded of the last contained fault (all read before they are write-1-to-cleared).
struct GateFault {
  std::uint32_t cfsr = 0;   // the full CFSR (MMFSR + BFSR + UFSR)
  std::uint32_t pc = 0;     // the stacked PC of the faulting instruction
  std::uint32_t bfar = 0;   // SCB->BFAR (valid when CFSR.BFARVALID)
  std::uint32_t mmfar = 0;  // SCB->MMFAR (valid when CFSR.MMARVALID)
  std::uint32_t hfsr = 0;   // SCB->HFSR
};

// A plugin entry in the hard-float AAPCS shape every `.ptfx` entry has: r0-r3 and the float argument in s0 (the
// set_param float, for instance). init() returns void; the caller reads ret only for the calls that have one.
using Entry = std::int32_t (*)(std::uint32_t r0, std::uint32_t r1, std::uint32_t r2, std::uint32_t r3, float s0);

// Runs `entry` unprivileged on its own stack and returns its result through `ret`.
//   stack_top / stack_bottom  the plugin stack (PSP starts at stack_top, 8-byte aligned; the guard is everything
//                             below stack_bottom). Must satisfy stack_top > stack_bottom and stack_top % 8 == 0.
//   image_base / image_bytes  the loaded plugin image, kept in the gate state for the diagnostic that turns the
//                             fault PC into an offset in the image.
//   module_id                 the 16-bit id (A7: 0x8000 | hash) stamped with the platform's set_module while the
//                             call runs, so a crash record names the plugin; 0 is stamped again on the way out.
//   timeout_ms                the deadline of the call, checked in the SysTick handler.
GateResult call(Entry entry, std::uint32_t r0, std::uint32_t r1, std::uint32_t r2, std::uint32_t r3, float s0,
                std::uint32_t timeout_ms, std::uintptr_t stack_top, std::uintptr_t stack_bottom,
                std::uintptr_t image_base, std::uint32_t image_bytes, std::uint16_t module_id,
                std::int32_t* ret) noexcept;

// The fault the last contained call faulted with (zeroed while no call has faulted).
GateFault last_fault() noexcept;

}  // namespace o3os::hal::code_gate

// The hooks the target's handlers call. Defined by hal/src/code_gate_m4.cpp on the cross builds; declared here so
// the platform sources (SysTick, the fault stub) can name them. On the host nothing references them.
extern "C" {
// The gate's active flag: read by the SysTick trampoline for its one-load one-branch idle check.
extern volatile std::uint32_t o3os_hal_gate_active;
// SysTick entry, called with the EXC_RETURN of the interrupted context only while a call runs: aborts the plugin
// when the deadline passed (or its PSP left the stack) by rewriting the stacked PC to the abort stub.
void o3os_hal_gate_systick(std::uint32_t exc_return) noexcept;
// The fault stub's first question, with the frame pointer (MSP or PSP as EXC_RETURN bit 2 says) and EXC_RETURN:
// returns 1 when the fault is contained (a fault from thread mode on PSP whose frame lies in the plugin stack or
// below it, or a stacking error) — the frame is then forged at the plugin stack top, PSP is set to it, the fault
// fields are recorded and CFSR/HFSR are cleared, and the stub must return to thread mode on PSP (0xFFFFFFFD).
// Returns 0 for every other fault: the existing crash-record path runs unchanged.
int o3os_hal_gate_fault_contain(std::uint32_t* frame, std::uint32_t exc_return) noexcept;
// Stamps the module id of the running call (the platform's set_module; a weak no-op default lives in the
// Cortex-M source, platform/mk66 overrides it with mk66::set_module).
void o3os_hal_gate_stamp_module(std::uint16_t module) noexcept;
}

#if !defined(__arm__)
// Host implementation (tests and simulator): a direct call, no containment. The stack, image, module and timeout
// arguments are accepted and ignored; the module is not stamped (the host has no crash context to stamp).
namespace o3os::hal::code_gate {

inline GateResult call(Entry entry, std::uint32_t r0, std::uint32_t r1, std::uint32_t r2, std::uint32_t r3, float s0,
                       std::uint32_t /*timeout_ms*/, std::uintptr_t /*stack_top*/,
                       std::uintptr_t /*stack_bottom*/, std::uintptr_t /*image_base*/,
                       std::uint32_t /*image_bytes*/, std::uint16_t /*module_id*/,
                       std::int32_t* ret) noexcept {
  if (entry == nullptr) {
    return GateResult::Unavailable;
  }
  const std::int32_t result = entry(r0, r1, r2, r3, s0);
  if (ret != nullptr) {
    *ret = result;
  }
  return GateResult::Ok;
}

inline GateFault last_fault() noexcept { return GateFault{}; }

}  // namespace o3os::hal::code_gate
#endif
