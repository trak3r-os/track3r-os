// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Cortex-M4F implementation of hal::code_gate (docs/specs/OS-CEPTFX.md section 3.3), ONE source for the MK66 image
// and the QEMU target tests (hal/CMakeLists.txt compiles it into `hal` on every cross build): the gate's enter, the
// return stub every plugin returns through, the SVCall handler, the resume point, and the two handler hooks
// (o3os_hal_gate_systick, o3os_hal_gate_fault_contain) that platform/mk66 and platform/qemu's SysTick and fault
// stub call. About 60 instructions of assembly plus the hooks.
//
// Shape (spec 3.3):
//   enter    main loop, privileged: save r4-r11, s16-s31, FPSCR, LR and the host SP into the context (164 B of
//            .bss, i.e. SRAM_U on the MK66), arm the volatile gate state, DSB, PSP = stack top (8-byte aligned),
//            LR = return stub, CONTROL = SPSEL|!nPRIV, ISB, branch to the entry with r0-r3 and s0 (the call's
//            arguments travel in registers across the switch, so the context stays small).
//   return   the plugin's `bx lr` lands in the stub, which executes SVC. The SVCall handler builds a fresh basic
//            frame just below the saved host SP (PC = resume, r0 = the plugin's r0), points that SP register at
//            it, restores CONTROL (the host's SPSEL, nPRIV = 0), clears FPCCR.LSPACT (the plugin's lazy FP state is
//            dropped) and returns to thread mode on the host stack. Resume restores the context, checks the guard,
//            and returns to call() with the reason.
//   timeout  the SysTick hook: while a call runs, a tick that interrupted thread mode on PSP with the frame in the
//            plugin stack rewrites the stacked PC to the return stub (xPSR = Thumb bit only) and records the reason.
//   fault    the fault stub's hook: a fault from thread mode on PSP whose frame lies in the plugin stack (or below
//            it, or a stacking error) is recorded (CFSR, PC, BFAR/MMFAR, HFSR), CFSR/HFSR are write-1-to-cleared, a
//            fresh basic frame is forged at the plugin stack top with PC = the return stub, PSP is set to it and the
//            stub returns to thread mode on PSP (0xFFFFFFFD) — PRIMASK is not left set by that path.
//
// The gate state is volatile; call() is its only writer before the call (the main loop), the handlers write only
// the reason and the fault fields. No allocation: the context is one static block.
#include <o3os/hal/code_gate.h>

#include <cstddef>
#include <cstdint>

// Cortex-M4F only: hal/CMakeLists.txt compiles this file on the cross builds alone. On the host the header's inline
// implementation is the gate, and this file must not define the same symbols.
#if defined(__arm__)

namespace o3os::hal::code_gate {
namespace gate_detail {

// SCB and FPCCR (ARMv7-M ARM B3.3/B3.4; platform/mk66/src/regs.h names the same addresses for the MK66).
constexpr std::uintptr_t kScbCfsr = 0xE000ED28;
constexpr std::uintptr_t kScbHfsr = 0xE000ED2C;
constexpr std::uintptr_t kScbMmfar = 0xE000ED34;
constexpr std::uintptr_t kScbBfar = 0xE000ED38;
constexpr std::uintptr_t kFpccr = 0xE000EF34;  // FPCCR: LSPACT is bit 0
inline volatile std::uint32_t& reg(std::uintptr_t address) {
  return *reinterpret_cast<volatile std::uint32_t*>(address);
}

// EXC_RETURN bits used below.
constexpr std::uint32_t kExcThreadPsp = 0x0000000Du;  // bit0=1, bit2=PSP, bit3=thread mode
// CFSR bits that say the exception frame itself could not be stacked/unstacked (MMFSR.MSTKERR/MUNSTKERR,
// BFSR.STKERR/UNSTKERR).
constexpr std::uint32_t kCfsrStackingError = 0x00001818u;
constexpr std::uint32_t kXpsrThumb = 0x01000000u;  // a forged xPSR: Thumb bit only, IT/ICI clear

// The gate's context and state. The assembly below addresses it by the byte offsets asserted next to it; keep the
// two in step. The call's arguments are NOT stored here: gate_go carries them in registers across the switch.
struct GateContext {
  std::uint32_t r[8];        // +0   r4-r11
  std::uint32_t s[16];       // +32  s16-s31
  std::uint32_t fpscr;       // +96
  std::uint32_t lr;          // +100 the host's LR (the return address inside call())
  std::uint32_t main_sp;     // +104 the host SP at enter (MSP or PSP, as main_spsel says)
  std::uint32_t main_spsel;  // +108 CONTROL.SPSEL at enter (0: host on MSP, 2: host on PSP)
  volatile std::uint32_t reason;       // +112 GateResult, raw
  volatile std::uint32_t deadline;     // +116 the gate's own ms counter + timeout_ms at enter
  volatile std::uint32_t stack_top;    // +120
  volatile std::uint32_t stack_bottom; // +124 the guard is everything below this
  volatile std::uint32_t image_base;   // +128
  volatile std::uint32_t image_bytes;  // +132
  volatile GateFault fault;            // +136 cfsr, pc, bfar, mmfar, hfsr
  std::uint32_t psp_final;   // +156 the plugin PSP seen by the SVCall handler (the guard check at resume)
  std::uint32_t ret_value;   // +160 the plugin's r0, carried through the resume
};
static_assert(offsetof(GateContext, r) == 0 && offsetof(GateContext, s) == 32 && offsetof(GateContext, fpscr) == 96 &&
                  offsetof(GateContext, lr) == 100 && offsetof(GateContext, main_sp) == 104 &&
                  offsetof(GateContext, main_spsel) == 108 && offsetof(GateContext, reason) == 112 &&
                  offsetof(GateContext, deadline) == 116 && offsetof(GateContext, stack_top) == 120 &&
                  offsetof(GateContext, stack_bottom) == 124 && offsetof(GateContext, image_base) == 128 &&
                  offsetof(GateContext, image_bytes) == 132 && offsetof(GateContext, fault) == 136 &&
                  offsetof(GateContext, psp_final) == 156 && offsetof(GateContext, ret_value) == 160,
              "the gate assembly addresses GateContext by these offsets");
static_assert(sizeof(GateContext) == 164, "the context is the ~160 B of saved state plus the call's state");

}  // namespace gate_detail
}  // namespace o3os::hal::code_gate

// extern "C": the assembly and the platform hooks below reference these by their plain names.
extern "C" {

using o3os::hal::code_gate::gate_detail::GateContext;
using o3os::hal::code_gate::gate_detail::kCfsrStackingError;
using o3os::hal::code_gate::gate_detail::kExcThreadPsp;
using o3os::hal::code_gate::gate_detail::kFpccr;
using o3os::hal::code_gate::gate_detail::kScbBfar;
using o3os::hal::code_gate::gate_detail::kScbCfsr;
using o3os::hal::code_gate::gate_detail::kScbHfsr;
using o3os::hal::code_gate::gate_detail::kScbMmfar;
using o3os::hal::code_gate::gate_detail::kXpsrThumb;
using o3os::hal::code_gate::gate_detail::reg;

GateContext o3os_gate_ctx;
volatile std::uint32_t o3os_hal_gate_active = 0;

// Milliseconds counted by the SysTick hook while a call runs (the hook is called only while the gate is active, so
// this measures the call's own duration; the deadline is this counter + timeout at enter).
volatile std::uint32_t o3os_gate_ms = 0;

// The plugin's return path, and the PC every abort path rewrites the stacked frame to: execute SVC, which the
// SVCall handler turns into the resume. Never returns.
[[gnu::naked]] void gate_return_stub() {
  asm volatile("dsb\n\tsvc #0\n\tb .\n\t");  // dsb: an imprecise BusFault of the plugin must land before the SVC
}

// Enter: save the host context, then switch to the plugin (PSP, unprivileged) and branch to the entry with the
// call's arguments in r0-r3 and s0. Hard-float AAPCS, so the signature below delivers entry in r0, the four r-args
// in r1-r3 and the stack, and the float arg in s0. Naked: runs on the host stack until CONTROL changes and touches
// no stack after that (the arguments are popped into r4-r7, which the context save above already stored).
[[gnu::naked]] void gate_go(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile(
      "push {r0-r3}\n\t"                 // stash entry, a0, a1, a2 on the host stack
      "ldr r12, [sp, #16]\n\t"           // a3, the first stack argument
      "ldr r0, =o3os_gate_ctx\n\t"
      "stmia r0!, {r4-r11}\n\t"          // +0
      "vstmia r0!, {s16-s31}\n\t"        // +32 (s0, the float argument, is below s16: untouched)
      "vmrs r2, fpscr\n\t"
      "str r2, [r0], #4\n\t"             // +96 fpscr
      "str lr, [r0], #4\n\t"             // +100 lr
      "mrs r2, control\n\t"
      "and r3, r2, #2\n\t"               // the host thread's SPSEL
      "cmp r3, #0\n\t"
      "beq 1f\n\t"
      "mrs r2, psp\n\t"
      "b 2f\n\t"
      "1:\n\t"
      "mrs r2, msp\n\t"
      "2:\n\t"
      "add r2, r2, #16\n\t"              // undo the push above: the host SP as call() had it
      "str r2, [r0], #4\n\t"             // +104 main_sp
      "str r3, [r0], #4\n\t"             // +108 main_spsel (r0 = +112)
      "pop {r4-r7}\n\t"                  // r4=entry, r5=a0, r6=a1, r7=a2 (r4-r11 are already in the context)
      "ldr r2, [r0, #8]\n\t"             // +120 stack_top
      "bic r2, r2, #7\n\t"               // PSP 8-byte aligned
      "msr psp, r2\n\t"
      "ldr r2, =gate_return_stub\n\t"
      "mov lr, r2\n\t"
      "movs r2, #7\n\t"                  // CONTROL: FPCA = 1 (s0 is live: an ISR using the FPU stacks it instead of
                                         // clobbering it), SPSEL = 1 (PSP), nPRIV = 1 (unprivileged)
      "msr control, r2\n\t"
      "isb\n\t"
      "mov r0, r5\n\t"                   // the plugin's r0-r3
      "mov r1, r6\n\t"
      "mov r2, r7\n\t"
      "mov r3, r12\n\t"                  // a3
      "mov r12, r4\n\t"                  // entry
      "bx r12\n\t"                       // s0 still holds the float argument
      ".ltorg");
}

// The resume point: entered by the SVCall handler's exception return, in thread mode on the host stack,
// privileged, with r0 = the plugin's r0. Restores everything, checks the guard and returns into call().
[[gnu::naked]] void gate_resume() {
  asm volatile(
      "ldr r1, =o3os_gate_ctx\n\t"
      "str r0, [r1, #160]\n\t"           // +160 the plugin's return value
      "ldr r2, [r1, #156]\n\t"           // +156 psp_final
      "ldr r3, [r1, #124]\n\t"           // +124 stack_bottom
      "cmp r2, r3\n\t"
      "bhs 1f\n\t"
      "ldr r2, [r1, #112]\n\t"           // +112 reason: only an Ok is downgraded
      "cmp r2, #0\n\t"
      "bne 1f\n\t"
      "movs r2, #3\n\t"                  // StackOverflow
      "str r2, [r1, #112]\n\t"
      "1:\n\t"
      "add r2, r1, #32\n\t"              // s16-s31
      "vldmia r2!, {s16-s31}\n\t"
      "ldr r3, [r1, #96]\n\t"            // fpscr
      "vmsr fpscr, r3\n\t"
      "ldmia r1, {r4-r11}\n\t"           // r4-r11
      "ldr r0, [r1, #112]\n\t"           // the reason
      "ldr lr, [r1, #100]\n\t"           // the host's LR: back into call()
      "bx lr\n\t"
      ".ltorg");
}

// SVCall: the plugin (thread mode, PSP, unprivileged) returned through the stub's SVC. Build a fresh basic frame
// just below the saved host SP, with PC = the resume point and r0 = the plugin's r0; point the host's SP register
// at it, restore CONTROL (the host's SPSEL, nPRIV = 0), clear FPCCR.LSPACT, and return to thread mode on the host
// stack. Naked: no stack use, so the frame being forged is the only thing below the host SP.
[[gnu::naked]] void SVC_Handler() {
  asm volatile(
      "ldr r1, =o3os_hal_gate_active\n\t"  // no call in flight: not ours, no stale context to resume
      "ldr r1, [r1]\n\t"
      "cbnz r1, 3f\n\t"
      "ldr r1, =Default_Handler\n\t"     // lr is untouched: the crash path sees the real EXC_RETURN
      "bx r1\n\t"
      "3:\n\t"
      "mrs r0, psp\n\t"                  // the plugin's frame, pushed on the plugin PSP
      "ldr r1, =o3os_gate_ctx\n\t"
      "str r0, [r1, #156]\n\t"           // psp_final, for the guard check at resume
      "ldr r2, [r0]\n\t"                 // the plugin's r0 (frame[0])
      "ldr r3, [r1, #104]\n\t"           // main_sp
      "sub r3, r3, #32\n\t"              // the fresh basic frame, just below the saved host SP
      "str r2, [r3, #0]\n\t"             // r0 = the plugin's return value
      "movs r2, #0\n\t"
      "str r2, [r3, #4]\n\t"
      "str r2, [r3, #8]\n\t"
      "str r2, [r3, #12]\n\t"
      "str r2, [r3, #16]\n\t"
      "str r2, [r3, #20]\n\t"
      "ldr r2, =gate_resume\n\t"
      "str r2, [r3, #24]\n\t"            // PC = the resume point
      "ldr r2, =0x01000000\n\t"
      "str r2, [r3, #28]\n\t"            // xPSR = Thumb bit only
      "ldr r2, [r1, #108]\n\t"           // main_spsel
      "cmp r2, #0\n\t"
      "beq 1f\n\t"
      "msr psp, r3\n\t"
      "ldr r12, =0xFFFFFFFD\n\t"         // thread mode, PSP, basic frame
      "b 2f\n\t"
      "1:\n\t"
      "msr msp, r3\n\t"
      "ldr r12, =0xFFFFFFF9\n\t"         // thread mode, MSP, basic frame
      "2:\n\t"
      "ldr r3, [r1, #108]\n\t"
      "msr control, r3\n\t"              // the host's SPSEL, nPRIV = 0 (privileged thread)
      "isb\n\t"
      "ldr r3, =0xE000EF34\n\t"          // FPCCR
      "ldr r2, [r3]\n\t"
      "bic r2, r2, #1\n\t"               // clear LSPACT: the plugin's lazy FP state is dropped
      "str r2, [r3]\n\t"
      "isb\n\t"
      "bx r12\n\t"
      ".ltorg");
}

// The SysTick hook, called from the SysTick trampoline (platform clock.cpp) only while a call runs, with the
// EXC_RETURN of the interrupted context. A tick that interrupted another ISR does nothing; the next one acts.
void o3os_hal_gate_systick(std::uint32_t exc_return) noexcept {
  o3os_gate_ms = o3os_gate_ms + 1u;
  if ((exc_return & kExcThreadPsp) != kExcThreadPsp) {
    return;  // not thread mode on PSP: the plugin is not the interrupted context
  }
  std::uint32_t psp;
  asm volatile("mrs %0, psp" : "=r"(psp));
  auto& g = o3os_gate_ctx;
  if (psp >= g.stack_top) {
    return;  // the frame is not the plugin's (the host thread's own PSP, before the switch)
  }
  if (psp < g.stack_bottom) {
    g.reason = static_cast<std::uint32_t>(o3os::hal::code_gate::GateResult::StackOverflow);  // it left its stack
  } else if (static_cast<std::int32_t>(o3os_gate_ms - g.deadline) >= 0) {  // wrap-safe
    g.reason = static_cast<std::uint32_t>(o3os::hal::code_gate::GateResult::Timeout);
  } else {
    return;
  }
  // Rewrite the stacked PC to the abort stub (the return stub), xPSR = Thumb bit only. The basic frame (r0-r3, r12,
  // lr, pc, xpsr) is at the lowest addresses in both frame shapes: with the FP state stacked (EXC_RETURN bit 4
  // clear) s0-s15 and FPSCR follow it (ARMv7-M ARM B1.5.7), so PC and xPSR are words 6 and 7 either way.
  auto* frame = reinterpret_cast<std::uint32_t*>(psp);
  frame[6] = reinterpret_cast<std::uint32_t>(&gate_return_stub);
  frame[7] = kXpsrThumb;
}

// The fault stub's question. Returns 1 when the fault is contained: from thread mode on PSP, with the frame in the
// plugin stack or below it (or a stacking error) — then the fault is recorded, CFSR/HFSR are cleared (write-1-to-
// clear), LSPACT is dropped, a fresh basic frame is forged at the plugin stack top with PC = the abort stub, PSP is
// set to it, and the stub returns to thread mode on PSP. Returns 0 for every other fault.
int o3os_hal_gate_fault_contain(std::uint32_t* frame, std::uint32_t exc_return) noexcept {
  if (o3os_hal_gate_active == 0u) {
    return 0;
  }
  if ((exc_return & kExcThreadPsp) != kExcThreadPsp) {
    return 0;  // a fault in handler mode, or from thread mode on MSP: the existing crash-record path runs
  }
  auto& g = o3os_gate_ctx;
  const std::uint32_t cfsr = reg(kScbCfsr);
  const std::uint32_t hfsr = reg(kScbHfsr);
  const bool stacking_error = (cfsr & kCfsrStackingError) != 0u;
  const std::uintptr_t sp = reinterpret_cast<std::uintptr_t>(frame);
  const bool frame_below_top = sp < g.stack_top;  // the plugin's PSP only ever moves down from stack_top
  if (!frame_below_top && !stacking_error) {
    return 0;
  }
  g.fault.cfsr = cfsr;
  // The stacked PC is word 6 in both frame shapes; after a stacking error the frame was not written (and may not be
  // readable), so it is not read.
  g.fault.pc = (frame_below_top && !stacking_error) ? frame[6] : 0u;
  g.fault.bfar = reg(kScbBfar);
  g.fault.mmfar = reg(kScbMmfar);
  g.fault.hfsr = hfsr;
  g.reason = static_cast<std::uint32_t>(o3os::hal::code_gate::GateResult::Fault);
  reg(kScbCfsr) = cfsr;  // write-1-to-clear
  reg(kScbHfsr) = hfsr;  // write-1-to-clear
  reg(kFpccr) = reg(kFpccr) & ~1u;  // drop the lazy FP state on this exit path too
  // Forge a fresh basic frame at the plugin stack top, PC = the abort stub, and point PSP at it: the stub returns
  // to thread mode on PSP (0xFFFFFFFD) and lands in the abort stub, which SVCs back through the resume.
  auto* forged = reinterpret_cast<std::uint32_t*>(g.stack_top & ~std::uintptr_t{7u}) - 8;
  for (int i = 0; i < 6; ++i) {
    forged[i] = 0u;
  }
  forged[6] = reinterpret_cast<std::uint32_t>(&gate_return_stub);
  forged[7] = kXpsrThumb;
  asm volatile("msr psp, %0\n\tisb" ::"r"(forged) : "memory");
  return 1;
}

// Weak default: the platform overrides this with its set_module (platform/mk66/src/crash_store.cpp).
__attribute__((weak)) void o3os_hal_gate_stamp_module(std::uint16_t /*module*/) noexcept {}

}  // extern "C"

namespace o3os::hal::code_gate {

GateResult call(Entry entry, std::uint32_t r0, std::uint32_t r1, std::uint32_t r2, std::uint32_t r3, float s0,
                std::uint32_t timeout_ms, std::uintptr_t stack_top, std::uintptr_t stack_bottom,
                std::uintptr_t image_base, std::uint32_t image_bytes, std::uint16_t module_id,
                std::int32_t* ret) noexcept {
  if (entry == nullptr || (reinterpret_cast<std::uintptr_t>(entry) & 1u) == 0u || o3os_hal_gate_active != 0u ||
      stack_top <= stack_bottom || (stack_top & 7u) != 0u) {
    return GateResult::Unavailable;  // nothing ran
  }
  // With PRIMASK or BASEPRI raised the plugin's SVC (priority 0) would escalate to HardFault and the abort path
  // would loop: refuse the call instead.
  std::uint32_t primask;
  std::uint32_t basepri;
  asm volatile("mrs %0, primask\n\tmrs %1, basepri" : "=r"(primask), "=r"(basepri));
  if ((primask | basepri) != 0u) {
    return GateResult::Unavailable;  // nothing ran
  }
  auto& g = o3os_gate_ctx;
  g.stack_top = stack_top;
  g.stack_bottom = stack_bottom;
  g.image_base = image_base;
  g.image_bytes = image_bytes;
  g.reason = static_cast<std::uint32_t>(GateResult::Ok);
  g.fault.cfsr = 0u;
  g.fault.pc = 0u;
  g.fault.bfar = 0u;
  g.fault.mmfar = 0u;
  g.fault.hfsr = 0u;
  g.deadline = o3os_gate_ms + timeout_ms;
  o3os_hal_gate_stamp_module(module_id);
  o3os_hal_gate_active = 1u;  // the volatile gate state is complete before the DSB
  asm volatile("dsb" ::: "memory");
  gate_go(reinterpret_cast<std::uint32_t>(entry), r0, r1, r2, r3, s0);  // returns through the resume point
  asm volatile("" ::: "memory");  // the handlers' writes to the state are visible now
  o3os_hal_gate_active = 0u;
  o3os_hal_gate_stamp_module(0);
  const GateResult result = static_cast<GateResult>(g.reason);
  if (ret != nullptr) {
    *ret = static_cast<std::int32_t>(g.ret_value);
  }
  return result;
}

GateFault last_fault() noexcept {
  GateFault f;
  const auto& g = o3os_gate_ctx;
  f.cfsr = g.fault.cfsr;
  f.pc = g.fault.pc;
  f.bfar = g.fault.bfar;
  f.mmfar = g.fault.mmfar;
  f.hfsr = g.fault.hfsr;
  return f;
}

}  // namespace o3os::hal::code_gate

#endif  // defined(__arm__)
