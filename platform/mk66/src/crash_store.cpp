// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reset-surviving state of the platform layer: the crash record, the stage/module words a fault handler stamps into
// it, and the boot report built from them. All of it lives in one `.noinit` block at the bottom of SRAM_U, which the C
// runtime never touches, so a reset (SYSRESETREQ, watchdog) leaves it as it was; whether SRAM really holds through every
// reset source of this chip is an open hardware check (README.md), and a record that does not validate is "no record".
#include "internal.h"
#include "regs.h"

#include <o3os/hal/code_gate.h>
#include <o3os/platform/mk66/crash_record.h>
#include <o3os/platform/mk66/mem_map.h>
#include <o3os/platform/mk66/platform.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::platform::mk66 {
namespace {

constexpr std::size_t kRecordWords = sizeof(crash::CrashRecord) / sizeof(std::uint32_t);

// The block, as plain words so every access is a defined volatile word access (the record is built and checked as a
// struct elsewhere, and moved in and out through memcpy: no aliasing tricks). Declared alignment: the crash record is
// 8-aligned and so is the block.
struct alignas(8) NoinitBlock {
  volatile std::uint32_t record[kRecordWords];
  volatile std::uint32_t context;          // crash::make_context(module, stage)
  volatile std::uint32_t context_inverse;  // ~context: a context is believed only if the two agree
  std::uint8_t reserved[mem::kCrashBlockSize - sizeof(crash::CrashRecord) - 2 * sizeof(std::uint32_t)];
};
static_assert(sizeof(NoinitBlock) == mem::kCrashBlockSize, "the block is the size mem_map.h reserves (and the linker asserts)");
static_assert(alignof(NoinitBlock) == 8, "declared alignment");
static_assert(sizeof(crash::CrashRecord) % sizeof(std::uint32_t) == 0, "the record is whole words");

// OS-OUTON: the outputs are open from boot to reset, so every way out of the image drops the amplifier key first: one GPIO write, no I2C and no wait, as safe in a fault handler
// (interrupts off, fault stack) as anywhere. The codec behind it is NOT touched on a fault: it keeps its registers across the reset (unmuted) and the next boot's start() mutes it
// before it powers anything (the hardware list says so: D-1). A thread that can afford the codec's part (`reset`, the fw update's quiesce) calls audio_device().mute() before.
// The port clock gate is checked so that a fault before early_pins() (the gates are off at reset) touches no unclocked register.
void drop_amplifier_key() noexcept {
  static_assert(board::kAudioMute.port == Port::D, "the clock gate checked below is the amplifier key's port");
  if ((regs::sim_scgc5() & regs::kScgc5PortD) != 0u) {
    regs::gpio_pcor(board::kAudioMute) = regs::mask_of(board::kAudioMute);
  }
}

// `.bss.noinit.crash` is the first thing in `.noinit`, which is the first thing in SRAM_U (mk66.ld). A `.bss.` name makes
// the section NOBITS and rejects an initialiser at compile time (sections.h).
[[gnu::section(".bss.noinit.crash"), gnu::used]] NoinitBlock g_block;

// The record that was waiting at reset, copied aside at boot (.bss). Read-only after capture_boot_record().
crash::CrashRecord g_boot_record;
BootInfo g_boot_info = {0, false};

}  // namespace

namespace boot {

crash::CrashRecord load_stored_record() noexcept {
  std::uint32_t words[kRecordWords];
  for (std::size_t i = 0; i < kRecordWords; ++i) {
    words[i] = g_block.record[i];
  }
  crash::CrashRecord r;
  std::memcpy(&r, words, sizeof r);
  return r;
}

void store_record(const crash::CrashRecord& r) noexcept {
  std::uint32_t words[kRecordWords];
  std::memcpy(words, &r, sizeof words);
  for (std::size_t i = 0; i < kRecordWords; ++i) {
    g_block.record[i] = words[i];
  }
}

std::uint32_t load_context() noexcept {
  const std::uint32_t context = g_block.context;
  return context == ~g_block.context_inverse ? context : crash::kUnknownContext;
}

void reset_context() noexcept {
  const std::uint32_t context = crash::make_context(0, crash::Stage::Reset);
  g_block.context = context;
  g_block.context_inverse = ~context;
}

void capture_boot_record() noexcept {
  const crash::CrashRecord stored = load_stored_record();
  if (crash::valid(stored)) {
    g_boot_record = stored;
    g_boot_info.crash_record_found = true;
  }
}

void capture_reset_cause() noexcept {
  g_boot_info.reset_sources =
      static_cast<std::uint16_t>(regs::rcm_srs0() | (static_cast<std::uint16_t>(regs::rcm_srs1()) << 8));
}

[[noreturn]] void reset_chip() noexcept {
  drop_amplifier_key();  // OS-OUTON: every SYSRESETREQ of the image goes through here, so it is always dropped (a fault handler did it before the record; this is the structural one)
  asm volatile("dsb" ::: "memory");
  regs::scb_aircr() = regs::kAircrVectkey | regs::kAircrSysresetreq;
  asm volatile("dsb" ::: "memory");
  for (;;) {
    asm volatile("nop");
  }
}

}  // namespace boot

const BootInfo& boot_info() noexcept { return g_boot_info; }

const crash::CrashRecord* boot_crash_record() noexcept { return g_boot_info.crash_record_found ? &g_boot_record : nullptr; }

void clear_crash_record() noexcept {
  for (std::size_t i = 0; i < kRecordWords; ++i) {
    g_block.record[i] = 0;
  }
  g_boot_info.crash_record_found = false;
}

void set_stage(crash::Stage stage) noexcept {
  const std::uint32_t current = boot::load_context();
  const std::uint16_t module = current == crash::kUnknownContext ? std::uint16_t{0} : crash::module_of(current);
  const std::uint32_t next = crash::make_context(module, stage);
  g_block.context = next;
  g_block.context_inverse = ~next;
}

void set_module(std::uint16_t module) noexcept {
  const std::uint32_t current = boot::load_context();
  const crash::Stage stage = current == crash::kUnknownContext ? crash::Stage::Unknown : crash::stage_of(current);
  const std::uint32_t next = crash::make_context(module, stage);
  g_block.context = next;
  g_block.context_inverse = ~next;
}

}  // namespace o3os::platform::mk66

// OS-CEPTFX S2b: the call gate (hal/code_gate.h) stamps the running plugin's id through this strong definition of
// the weak hook in hal/src/code_gate_m4.cpp, so a crash record names the plugin; the gate stamps 0 on the way out.
extern "C" void o3os_hal_gate_stamp_module(std::uint16_t module) noexcept {
  o3os::platform::mk66::set_module(module);
}

namespace o3os::platform::mk66 {
namespace {

// Collects what a handler can read of the core's state into a snapshot, writes the record and resets. `frame` is the
// exception frame (r0-r3, r12, lr, pc, xPSR) the hardware stacked, or the sentinel when there is none.
[[noreturn]] void record_and_reset(std::uint32_t exception, std::uint32_t exc_return, const std::uint32_t* frame,
                                   std::uint32_t pc_if_no_frame) noexcept {
  // Nothing may preempt the reporter: a software panic arrives here with interrupts on (the fault stub has masked them
  // already), and an ISR running on the fault stack's behalf would corrupt it.
  asm volatile("cpsid i" ::: "memory");
  drop_amplifier_key();  // OS-OUTON: first, before the snapshot and the record: a crash with the amplifier open must not go on playing the DMA's last blocks for longer than it has to
  const crash::FaultRegisters registers = {exception,       exc_return,           regs::scb_cfsr(), regs::scb_hfsr(),
                                           regs::scb_mmfar(), regs::scb_bfar(),   boot::load_context(), boot::uptime_ms_low()};
  const crash::FaultSnapshot s =
      crash::snapshot(registers, frame, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(frame)), pc_if_no_frame);
  boot::store_record(crash::encode(s, boot::load_stored_record()));
  boot::reset_chip();
}

}  // namespace

// The fault handlers (startup.cpp) switch to the fault stack and land here with the frame they found and EXC_RETURN.
extern "C" [[noreturn]] void o3os_fault_entry(const std::uint32_t* frame, std::uint32_t exc_return) {
  std::uint32_t ipsr;
  asm volatile("mrs %0, ipsr" : "=r"(ipsr));
  record_and_reset(ipsr & 0x1FFu, exc_return, frame, crash::kInvalidFrame);
}

[[noreturn]] void panic() noexcept {
  const auto caller = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  record_and_reset(crash::kPanicException, 0, nullptr, caller);
}

}  // namespace o3os::platform::mk66
