// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S2b on the real Cortex-M4F of QEMU's `mps2-an386`: the call gate (hal/code_gate.h, hal/src/code_gate_m4.cpp)
// with our own Thumb test "plugins" written in this file — a plain RAM block stands in for the SDRAM region (no alias
// or LMEM there). Every containment path is exercised: a normal return (gain, setp), a hang and a `cpsid i` loop
// (Timeout, with interrupts still running during the cpsid), an SCS access and an unmapped write (Fault, with CFSR
// and the stacked PC recorded), a deep recursion past the stack (StackOverflow or Fault), and a register clobber
// (the caller's r4-r11, s16-s31 and FPSCR intact). After each case a canary above the plugin stack is intact and a
// following gain call is still exact. OS-CEPTFX S3b adds the run job (app/ptfx_job.h) through the same gate: the SDK gain
// shape (exact), a hang (timeout strip, quarantined, the next plugin still runs) and a crash (contained with its warning).
// Runs as `qemu_ptfx_gate`.
#include <doctest/doctest.h>

#include <o3os/app/ptfx_job.h>
#include <o3os/hal/code_gate.h>
#include <o3os/platform/qemu/clock.h>

#include <cstdint>
#include <cstring>

extern "C" {
extern std::uint32_t __guard_base[];  // the runner's stack guard (platform/qemu linker script): [__guard_base, __stack_limit)
extern std::uint32_t __stack_limit[];
}

namespace gate = o3os::hal::code_gate;
namespace qemu = o3os::platform::qemu;

namespace {

// The "region" stand-in: [sacrifice | plugin stack | canary]. The sacrifice is what a runaway recursion below the
// plugin stack smashes (harmless: it is ours); the canary sits directly above the stack top, where no plugin stack
// use can reach it, and is checked after every case.
constexpr unsigned kSacrificeWords = 32u * 1024u;  // 128 KiB
constexpr unsigned kStackWords = 1024u;             // the 4 KiB plugin stack
constexpr unsigned kCanaryWords = 16u;
constexpr std::uint32_t kCanaryBase = 0xC0FFEE00u;

alignas(8) std::uint32_t g_region[kSacrificeWords + kStackWords + kCanaryWords];
static_assert((kSacrificeWords + kStackWords) % 2u == 0u, "the plugin stack top must stay 8-byte aligned");
std::uint32_t* const g_stack_bottom = g_region + kSacrificeWords;
std::uint32_t* const g_stack_top = g_region + kSacrificeWords + kStackWords;  // 8-byte aligned
std::uint32_t* const g_canary = g_region + kSacrificeWords + kStackWords;

constexpr std::uint32_t kTimeoutMs = 100u;   // for the paths that must be aborted
constexpr std::uint32_t kLongTimeoutMs = 1000u;
constexpr std::uint16_t kModuleId = 0x8001u;

// ---- The test "plugins": our own Thumb code, entered through the gate -------------------------------

extern "C" {

// r0 * 2, exactly.
[[gnu::naked]] std::int32_t ptfx_gain(std::uint32_t, std::uint32_t,std::uint32_t, std::uint32_t, float) {
  asm volatile("adds r0, r0, r0\n\tbx lr");
}

// (int)(s0 * 1000): the float argument travels in s0, hard-float AAPCS.
[[gnu::naked]] std::int32_t ptfx_setp(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile(
      "movw r1, #1000\n\t"
      "vmov s1, r1\n\t"
      "vcvt.f32.s32 s1, s1\n\t"  // 1000.0f: vmov moves the bits, the convert makes them a float
      "vmul.f32 s0, s0, s1\n\t"
      "vcvt.s32.f32 s0, s0\n\t"
      "vmov r0, s0\n\t"
      "bx lr");
}

// Never returns: the deadline must abort it.
[[gnu::naked]] std::int32_t ptfx_hang(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile("1: b 1b");
}

// `cpsid i`, then a loop: unprivileged, the mask has no effect, so the deadline still aborts it.
[[gnu::naked]] std::int32_t ptfx_cpsid(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile("cpsid i\n\t1: b 1b");
}

// Reads SCB->CPUID (0xE000ED00) unprivileged: a fault.
[[gnu::naked]] std::int32_t ptfx_fault_scs(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile(
      "ldr r0, =0xE000ED00\n\t"
      "ldr r0, [r0]\n\t"
      "bx lr\n\t"
      ".ltorg");
}

// Writes an unmapped address: a bus fault with BFAR.
[[gnu::naked]] std::int32_t ptfx_fault_addr(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile(
      "ldr r0, =0x50000000\n\t"
      "str r0, [r0]\n\t"
      "bx lr\n\t"
      ".ltorg");
}

// Deep recursion past the stack into the sacrifice below.
[[gnu::naked]] std::int32_t ptfx_stackhog(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile(
      "push {r4, lr}\n\t"
      "sub sp, sp, #504\n\t"   // a big frame, then a spin of about a SysTick period, so a tick lands while the
      "ldr r0, =20000\n\t"     // PSP is below the stack (the sacrifice is 128 KiB: the recursion cannot outrun it)
      "1: subs r0, r0, #1\n\t"
      "bne 1b\n\t"
      "bl ptfx_stackhog\n\t"
      "add sp, sp, #504\n\t"
      "pop {r4, pc}\n\t"
      ".ltorg");
}

// Moves SP 8 KiB down (4 KiB below the 4 KiB plugin stack, into the sacrifice) and returns with no tick in between:
// only the guard in the resume (the PSP the return SVC left, against the stack bottom) can see it.
[[gnu::naked]] std::int32_t ptfx_bigsub(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile(
      "sub sp, sp, #8192\n\t"
      "bx lr");
}

// Overwrites r4-r11, s16-s31 and FPSCR, then returns 42: the gate must restore all of them.
[[gnu::naked]] std::int32_t ptfx_clobber(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile(
      "ldr r0, =0x41424344\n\t"
      "mov r4, r0\n\tmov r5, r0\n\tmov r6, r0\n\tmov r7, r0\n\t"
      "mov r8, r0\n\tmov r9, r0\n\tmov r10, r0\n\tmov r11, r0\n\t"
      "vmov s16, r0\n\tvmov s17, r0\n\tvmov s18, r0\n\tvmov s19, r0\n\t"
      "vmov s20, r0\n\tvmov s21, r0\n\tvmov s22, r0\n\tvmov s23, r0\n\t"
      "vmov s24, r0\n\tvmov s25, r0\n\tvmov s26, r0\n\tvmov s27, r0\n\t"
      "vmov s28, r0\n\tvmov s29, r0\n\tvmov s30, r0\n\tvmov s31, r0\n\t"
      "ldr r0, =0x00C00000\n\t"
      "vmsr fpscr, r0\n\t"
      "movs r0, #0x2A\n\t"
      "bx lr\n\t"
      ".ltorg");
}

std::int32_t ptfx_do_clobber_call();

// The round trip of the callee-saved state across one gate call, with nothing the compiler chose in between: sets
// known patterns in r4-r11 and s16-s31, captures them and FPSCR into out[0..24], calls ptfx_do_clobber_call (a gate
// call of ptfx_clobber), captures again into out[25..49] and stores the call's result in out[50].
[[gnu::naked]] void ptfx_roundtrip(std::uint32_t*) {
  asm volatile(
      "push {r0, r4-r11, lr}\n\t"  // 40 bytes: the stack stays 8-byte aligned
      "vpush {s16-s31}\n\t"        // 64 bytes: the saved r0 is at [sp, #64]
      "ldr r4, =0x10000000\n\t"
      "add r5, r4, #1\n\tadd r6, r4, #2\n\tadd r7, r4, #3\n\t"
      "add r8, r4, #4\n\tadd r9, r4, #5\n\tadd r10, r4, #6\n\tadd r11, r4, #7\n\t"
      "vmov s16, r4\n\tvmov s17, r5\n\tvmov s18, r6\n\tvmov s19, r7\n\t"
      "vmov s20, r8\n\tvmov s21, r9\n\tvmov s22, r10\n\tvmov s23, r11\n\t"
      "vmov s24, r4\n\tvmov s25, r5\n\tvmov s26, r6\n\tvmov s27, r7\n\t"
      "vmov s28, r8\n\tvmov s29, r9\n\tvmov s30, r10\n\tvmov s31, r11\n\t"
      "ldr r0, [sp, #64]\n\t"
      "stmia r0!, {r4-r11}\n\t"
      "vstmia r0!, {s16-s31}\n\t"
      "vmrs r1, fpscr\n\t"
      "str r1, [r0]\n\t"
      "bl ptfx_do_clobber_call\n\t"
      "ldr r1, [sp, #64]\n\t"
      "str r0, [r1, #200]\n\t"
      "add r0, r1, #100\n\t"
      "stmia r0!, {r4-r11}\n\t"
      "vstmia r0!, {s16-s31}\n\t"
      "vmrs r1, fpscr\n\t"
      "str r1, [r0]\n\t"
      "vpop {s16-s31}\n\t"
      "pop {r0, r4-r11, pc}\n\t"
      ".ltorg");
}

}  // extern "C"

// The "image": the plugin functions' span, for the bounds the gate keeps and the PC-in-image checks.
const std::uintptr_t kImageBase = reinterpret_cast<std::uintptr_t>(&ptfx_gain);
const std::uint32_t kImageBytes = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&ptfx_clobber) + 64u - kImageBase);

gate::GateResult run(gate::Entry entry, std::uint32_t r0, float s0, std::int32_t* ret, std::uint32_t timeout_ms) {
  return gate::call(entry, r0, 0u, 0u, 0u, s0, timeout_ms, reinterpret_cast<std::uintptr_t>(g_stack_top),
                    reinterpret_cast<std::uintptr_t>(g_stack_bottom), kImageBase, kImageBytes, kModuleId, ret);
}

// The QEMU runner keeps an MPU (region 0 = the stack guard, PRIVDEFENA): unprivileged code then has no access at all
// outside the regions, where the unit has no MPU at all in this role (the plugin sees the whole map except the PPB).
// This suite moves the guard to region 1 (higher regions win) and opens the whole map to unprivileged code in region
// 0, so a plugin faults only for the reasons the cases are about (the SCS is privileged-only whatever the MPU says).
void open_map_to_unprivileged() {
  volatile std::uint32_t* const mpu_type = reinterpret_cast<volatile std::uint32_t*>(0xE000ED90u);
  volatile std::uint32_t* const mpu_rnr = reinterpret_cast<volatile std::uint32_t*>(0xE000ED98u);
  volatile std::uint32_t* const mpu_rbar = reinterpret_cast<volatile std::uint32_t*>(0xE000ED9Cu);
  volatile std::uint32_t* const mpu_rasr = reinterpret_cast<volatile std::uint32_t*>(0xE000EDA0u);
  if (((*mpu_type >> 8) & 0xFFu) < 2u) {
    return;  // no MPU, or too few regions: nothing restricts the plugin
  }
  static bool done = false;
  if (done) {
    return;  // once: the guard below is rebuilt from the linker symbols, and region 0 is already open
  }
  done = true;
  const std::uint32_t guard_base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__guard_base));
  const std::uint32_t guard_bytes = static_cast<std::uint32_t>(__stack_limit - __guard_base) * sizeof(std::uint32_t);
  const std::uint32_t size_field = 31u - static_cast<std::uint32_t>(__builtin_clz(guard_bytes)) - 1u;  // 2^(n+1) bytes
  *mpu_rnr = 1u;
  *mpu_rbar = guard_base;
  *mpu_rasr = (1u << 28) | (size_field << 1) | 1u;  // the runner's guard: no access, XN, enabled
  *mpu_rnr = 0u;
  *mpu_rasr = 0u;  // disabled while the base moves: the old attributes over a base of 0 would cover this very code
  *mpu_rbar = 0u;
  *mpu_rasr = (3u << 24) | (31u << 1) | 1u;  // AP = full access, 4 GiB, enabled
  asm volatile("dsb\n\tisb" ::: "memory");
}

void arm_canary() {
  open_map_to_unprivileged();
  for (unsigned i = 0; i < kCanaryWords; ++i) {
    g_canary[i] = kCanaryBase + i;
  }
}

// Armed once by a static constructor (the target runs them before main) and again after every check.
const bool g_armed = (arm_canary(), true);

// After every case: the canary above the plugin stack is intact and a following gain call is still exact.
void check_tail() {
  CHECK(g_armed);
  for (unsigned i = 0; i < kCanaryWords; ++i) {
    CHECK(g_canary[i] == kCanaryBase + i);
  }
  arm_canary();
  std::int32_t ret = 0;
  CHECK(run(&ptfx_gain, 21u, 0.0f, &ret, kLongTimeoutMs) == gate::GateResult::Ok);
  CHECK(ret == 42);
}

bool pc_in(gate::Entry fn) {
  const std::uintptr_t pc = gate::last_fault().pc;
  const std::uintptr_t lo = reinterpret_cast<std::uintptr_t>(fn);
  return pc >= lo && pc < lo + 32u;
}

}  // namespace

// The call ptfx_roundtrip brackets: ptfx_clobber through the gate, its result or -1.
extern "C" [[gnu::noinline, gnu::used]] std::int32_t ptfx_do_clobber_call() {
  std::int32_t ret = 0;
  const gate::GateResult r = run(&ptfx_clobber, 0u, 0.0f, &ret, kLongTimeoutMs);
  return r == gate::GateResult::Ok ? ret : -1;
}

TEST_CASE("gate: bad arguments are Unavailable and nothing runs") {
  std::int32_t ret = 0;
  CHECK(gate::call(nullptr, 0u, 0u, 0u, 0u, 0.0f, kLongTimeoutMs, reinterpret_cast<std::uintptr_t>(g_stack_top),
                   reinterpret_cast<std::uintptr_t>(g_stack_bottom), kImageBase, kImageBytes, kModuleId, &ret) ==
        gate::GateResult::Unavailable);
  check_tail();
}

TEST_CASE("gate: gain and setp return Ok with exact results") {
  std::int32_t ret = 0;
  CHECK(run(&ptfx_gain, 12345u, 0.0f, &ret, kLongTimeoutMs) == gate::GateResult::Ok);
  CHECK(ret == 24690);
  CHECK(run(&ptfx_gain, 0x40000001u, 0.0f, &ret, kLongTimeoutMs) == gate::GateResult::Ok);
  CHECK(ret == static_cast<std::int32_t>(0x80000002u));  // wraps like int32_t
  CHECK(run(&ptfx_setp, 0u, 2.5f, &ret, kLongTimeoutMs) == gate::GateResult::Ok);
  CHECK(ret == 2500);  // s0 * 1000, exact in float
  CHECK(run(&ptfx_setp, 0u, -1.25f, &ret, kLongTimeoutMs) == gate::GateResult::Ok);
  CHECK(ret == -1250);
  check_tail();
}

TEST_CASE("gate: a hang is aborted with Timeout at the deadline") {
  const std::uint64_t t0 = qemu::system_clock().uptime_ms();
  std::int32_t ret = 0;
  CHECK(run(&ptfx_hang, 0u, 0.0f, &ret, kTimeoutMs) == gate::GateResult::Timeout);
  const std::uint64_t elapsed = qemu::system_clock().uptime_ms() - t0;
  CHECK(elapsed >= kTimeoutMs);       // the deadline was kept
  CHECK(elapsed <= kTimeoutMs + 50u);  // and the abort landed within a tick (2.1 s at the 2 s default)
  check_tail();
}

TEST_CASE("gate: cpsid i has no effect unprivileged: interrupts still run and the deadline still aborts") {
  const std::uint64_t t0 = qemu::system_clock().uptime_ms();
  std::int32_t ret = 0;
  CHECK(run(&ptfx_cpsid, 0u, 0.0f, &ret, kTimeoutMs) == gate::GateResult::Timeout);
  const std::uint64_t elapsed = qemu::system_clock().uptime_ms() - t0;
  CHECK(elapsed >= kTimeoutMs);  // SysTick fired while the plugin had "masked" interrupts
  CHECK(elapsed <= kTimeoutMs + 50u);
  check_tail();
}

TEST_CASE("gate: an unprivileged SCS access is contained as Fault with CFSR and the stacked PC") {
  std::int32_t ret = 0;
  CHECK(run(&ptfx_fault_scs, 7u, 0.0f, &ret, kLongTimeoutMs) == gate::GateResult::Fault);
  const gate::GateFault f = gate::last_fault();
  const std::uint32_t any_status = f.cfsr | f.hfsr;
  CHECK(any_status != 0u);
  CHECK(pc_in(&ptfx_fault_scs));  // the PC of the faulting read, inside the image
  check_tail();
}

TEST_CASE("gate: a write to an unmapped address is contained as Fault with BFAR") {
  std::int32_t ret = 0;
  CHECK(run(&ptfx_fault_addr, 0u, 0.0f, &ret, kLongTimeoutMs) == gate::GateResult::Fault);
  const gate::GateFault f = gate::last_fault();
  CHECK(f.cfsr != 0u);
  if ((f.cfsr & 0x8000u) != 0u) {  // BFARVALID
    CHECK(f.bfar == 0x50000000u);
  }
  CHECK(pc_in(&ptfx_fault_addr));
  check_tail();
}

TEST_CASE("gate: deep recursion past the stack is StackOverflow (the sacrifice below it is mapped: no Fault)") {
  std::int32_t ret = 0;
  // Deterministic here: the sacrifice is plain RAM, each frame spins about a tick, and the deadline is far later
  // than the first tick that finds the PSP below the stack bottom, so the SysTick hook reports StackOverflow.
  CHECK(run(&ptfx_stackhog, 0u, 0.0f, &ret, kTimeoutMs) == gate::GateResult::StackOverflow);
  check_tail();
}

TEST_CASE("gate: a plugin that returns with SP below the stack bottom is StackOverflow from the resume guard") {
  std::int32_t ret = 0;
  CHECK(run(&ptfx_bigsub, 0u, 0.0f, &ret, kLongTimeoutMs) == gate::GateResult::StackOverflow);
  check_tail();
}

// Teeth: gate_go and the resume reuse r4-r7 as scratch, so a missing restore of those would also show, but the
// round trip is a strong check mainly for r8-r11, which nothing else in the gate writes.
TEST_CASE("gate: a plugin that clobbers r4-r11, s16-s31 and FPSCR leaves the caller's intact") {
  constexpr unsigned kState = 8u + 16u + 1u;  // r4-r11, s16-s31, FPSCR
  std::uint32_t out[2 * kState + 1] = {};
  ptfx_roundtrip(out);
  CHECK(out[2 * kState] == 0x2Au);  // the plugin ran and returned
  for (unsigned i = 0; i < kState; ++i) {
    CHECK(out[i] == out[kState + i]);  // all restored
  }
  check_tail();
}

// ---- OS-CEPTFX S3b: the run job (app/ptfx_job.h) through the real gate ------------------------------------------------
// The SDK `gain` shape: init and set_param return 0, process_block writes out[0][i] = in[0][i] * 2 and returns n. A hang and an SCS read stand in as
// process_block for the two containment paths: the job quarantines the plugin, fails with its strip text, and a run of another plugin is exact after it.
extern "C" {
// r0 = in (an array of channel pointers), r1 = out, r2 = n frames; returns n.
[[gnu::naked]] std::int32_t ptfx_job_proc(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile(
      "ldr r0, [r0]\n\t"
      "ldr r1, [r1]\n\t"
      "mov r3, r2\n\t"
      "cbz r2, 2f\n\t"
      "1: ldrsh r12, [r0], #2\n\t"
      "lsl r12, r12, #1\n\t"
      "strh r12, [r1], #2\n\t"
      "subs r2, r2, #1\n\t"
      "bne 1b\n\t"
      "2: mov r0, r3\n\t"
      "bx lr");
}
[[gnu::naked]] std::int32_t ptfx_job_nop(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile("movs r0, #0\n\tbx lr");
}
// S5: the same shape over `channels` (r3) channels: channel c is multiplied by 2 << c (L by 2, R by 4); returns n.
[[gnu::naked]] std::int32_t ptfx_job_proc2(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, float) {
  asm volatile(
      "push {r4-r7}\n\t"
      "movs r4, #1\n\t"
      "mov r7, r2\n\t"
      "cbz r3, 9f\n\t"
      "0: ldr r5, [r0], #4\n\t"
      "ldr r6, [r1], #4\n\t"
      "mov r2, r7\n\t"
      "cbz r2, 3f\n\t"
      "1: ldrsh r12, [r5], #2\n\t"
      "lsl r12, r12, r4\n\t"
      "strh r12, [r6], #2\n\t"
      "subs r2, r2, #1\n\t"
      "bne 1b\n\t"
      "3: adds r4, r4, #1\n\t"
      "subs r3, r3, #1\n\t"
      "bne 0b\n\t"
      "9: mov r0, r7\n\t"
      "pop {r4-r7}\n\t"
      "bx lr");
}
}  // extern "C"

namespace {

constexpr std::uint32_t kJobFrames = 3000;
alignas(4) std::uint8_t g_job_scratch[1024];
std::int16_t g_job_in[kJobFrames];
std::int16_t g_job_side[kJobFrames];                      // S5: the stereo source's S block
std::int16_t g_job_out[2 * kJobFrames + 8];                // mono result, or M then S (kJobFrames is even)
alignas(4) std::int16_t g_job_stage[2 * o3os::app::kPtfxBlockFrames];  // the stereo staging (S5)

std::int16_t* job_alloc(void*, std::uint32_t frames, bool stereo) noexcept {
  return frames <= kJobFrames && (!stereo || frames % 2u == 0u) ? g_job_out : nullptr;
}

// Runs a job with `process` as the plugin's process_block; the final status (Failed also for a refusal at begin). `flags`: the plugin's flags
// (kPtfxFlagStereo makes the source stereo, with g_job_side as its S block).
o3os::core::JobStatus run_job(std::uintptr_t process, std::uint16_t id, o3os::module::PtfxQuarantine& q, o3os::app::PtfxJob& job, std::uint16_t flags = 0) {
  static o3os::module::PtfxInfo info;
  static o3os::module::PtfxHostTableStorage store;
  static o3os::module::PtfxHostTable host(store.bytes);
  info = o3os::module::PtfxInfo{};
  info.status = o3os::module::PtfxStatus::Ok;
  info.num_params = 0;
  info.flags = flags;
  info.scratch_size = sizeof g_job_scratch;
  const std::uintptr_t nop = reinterpret_cast<std::uintptr_t>(&ptfx_job_nop);
  const std::uintptr_t fns[] = {nop, process};
  std::uintptr_t lo = fns[0];
  std::uintptr_t hi = fns[0];
  for (const std::uintptr_t f : fns) {
    lo = f < lo ? f : lo;
    hi = f > hi ? f : hi;
  }
  o3os::app::PtfxJob::Plugin p;
  p.info = &info;
  p.entries = {nop, nop, process};
  p.host = &host;
  p.host_table = reinterpret_cast<std::uintptr_t>(store.bytes);
  p.scratch = g_job_scratch;
  p.scratch_cap = sizeof g_job_scratch;
  p.stack_top = reinterpret_cast<std::uintptr_t>(g_stack_top);
  p.stack_bottom = reinterpret_cast<std::uintptr_t>(g_stack_bottom);
  p.image_base = lo;
  p.image_bytes = static_cast<std::uint32_t>(hi + 64u - lo);
  p.id = id;
  p.quarantine = &q;
  p.stage = g_job_stage;
  o3os::app::PtfxJob::Source s;
  s.pcm = g_job_in;
  s.frames = kJobFrames;
  s.stereo = flags != 0u;
  s.side = g_job_side;
  s.alloc = &job_alloc;
  if (job.begin(p, s) != nullptr) {
    return o3os::core::JobStatus::Failed;
  }
  for (unsigned n = 0; n < 10000u; ++n) {
    const o3os::core::JobStatus st = job.step(1000);
    if (st != o3os::core::JobStatus::Running) {
      return st;
    }
  }
  return o3os::core::JobStatus::Running;
}

void fill_job_in() {
  for (std::uint32_t i = 0; i < kJobFrames; ++i) {
    g_job_in[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(i % 200u) - 100);
    g_job_out[i] = -1;
  }
}

}  // namespace

TEST_CASE("ptfx job: a gain plugin runs through the real gate and the result is exactly twice the input") {
  arm_canary();
  fill_job_in();
  o3os::module::PtfxQuarantine q;
  o3os::app::PtfxJob job;
  CHECK(run_job(reinterpret_cast<std::uintptr_t>(&ptfx_job_proc), 0x8101u, q, job) == o3os::core::JobStatus::Done);
  CHECK(job.result_frames() == kJobFrames);
  bool exact = true;
  for (std::uint32_t i = 0; i < kJobFrames; ++i) {
    exact = exact && g_job_out[i] == g_job_in[i] * 2;
  }
  CHECK(exact);
  check_tail();
}

// S5: a STEREO plugin runs once with channels = 2 through the real gate (L doubled, R times four): the result is the bank's M and S, converted back with
// the importer's rounding. Small values, so nothing saturates.
TEST_CASE("ptfx job: a stereo plugin gets channels = 2 through the real gate and the result is M and S as expected") {
  arm_canary();
  fill_job_in();
  for (std::uint32_t i = 0; i < kJobFrames; ++i) {
    g_job_side[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(i % 40u) - 20);
  }
  o3os::module::PtfxQuarantine q;
  o3os::app::PtfxJob job;
  CHECK(run_job(reinterpret_cast<std::uintptr_t>(&ptfx_job_proc2), 0x8105u, q, job, o3os::module::kPtfxFlagStereo) == o3os::core::JobStatus::Done);
  CHECK(job.result_frames() == kJobFrames);
  CHECK(job.stereo_result());
  bool exact = true;
  for (std::uint32_t i = 0; i < kJobFrames; ++i) {
    const std::int32_t l = g_job_in[i] + g_job_side[i];
    const std::int32_t r = g_job_in[i] - g_job_side[i];
    const std::int32_t l2 = l * 2;
    const std::int32_t r2 = r * 4;
    const std::int32_t m = (l2 + r2 + 1) >> 1;
    exact = exact && g_job_out[i] == m && g_job_out[kJobFrames + i] == l2 - m;
  }
  CHECK(exact);
  check_tail();
}

TEST_CASE("ptfx job: a hung plugin times out through the job, is quarantined, and the next plugin still runs") {
  arm_canary();
  fill_job_in();
  o3os::module::PtfxQuarantine q;
  o3os::app::PtfxJob job;
  CHECK(run_job(reinterpret_cast<std::uintptr_t>(&ptfx_hang), 0x8102u, q, job) == o3os::core::JobStatus::Failed);
  CHECK(std::strcmp(job.detail(), "PLUGIN TIMED OUT: QUARANTINED") == 0);
  CHECK(q.contains(0x8102u));
  o3os::app::PtfxJob again;
  CHECK(run_job(reinterpret_cast<std::uintptr_t>(&ptfx_job_proc), 0x8103u, q, again) == o3os::core::JobStatus::Done);
  CHECK(again.result_frames() == kJobFrames);
  check_tail();
}

TEST_CASE("ptfx job: a crashing plugin is contained through the job with the warning, and the unit stays alive") {
  arm_canary();
  fill_job_in();
  o3os::module::PtfxQuarantine q;
  o3os::app::PtfxJob job;
  CHECK(run_job(reinterpret_cast<std::uintptr_t>(&ptfx_fault_scs), 0x8104u, q, job) == o3os::core::JobStatus::Failed);
  CHECK(std::strcmp(job.detail(), "PLUGIN CRASHED: SAVE, THEN RESTART") == 0);
  CHECK(q.contains(0x8104u));
  check_tail();
}
