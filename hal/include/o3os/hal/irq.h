// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// Interrupt priorities, the one table (ADR 0013 section D1). Numbers are on the 8-bit NVIC scale, lower = more
// urgent; a core that implements fewer priority bits ignores the low ones (the MK66's Cortex-M4 implements four, so
// every value here is a multiple of 16: platform/mk66 asserts it). Thread mode (the main loop: UI, SD, jobs) is below
// every one of them and never touches audio.
//
// A platform maps its own interrupt lines to a Role (platform/mk66/irq_map.h) and applies this table at boot; the
// layers above never name a device interrupt. Changing a priority is changing this one table.
namespace o3os::hal::irq {

enum class Role : std::uint8_t {
  Fault,         // MemManage, BusFault, UsageFault: they must preempt every other handler
  AudioDma,      // I2S TX/RX DMA half/complete: advance the slot index, stamp cycles(), pend the render; touches no data
  SysTick,       // 1 kHz time base
  MidiUart,      // UART4 RX (MIDI DIN): bytes timestamped into the MIDI rings
  Usb,           // USB0 (ADR 0012)
  Completion,    // SDHC DMA, SPI2 DMA (FT812), I2C: completion flags only
  MidiOutTimer,  // the one PIT channel that emits due MIDI-out bytes to the UART FIFO
  Render,        // the software-pended render line: sequencer tick and the whole audio graph
  PinChange,     // pad / button / encoder pin interrupts: flags for the main loop
  Unassigned,    // a line nobody has claimed: below everything with a role
  PluginGate,    // SVCall: a .ptfx plugin's return through the call gate (OS-CEPTFX S2b); still the lowest system handler
};

namespace prio {
inline constexpr std::uint8_t kFault = 0;
inline constexpr std::uint8_t kAudioDma = 16;
inline constexpr std::uint8_t kSysTick = 32;
inline constexpr std::uint8_t kMidiUart = 48;
inline constexpr std::uint8_t kUsb = 64;
inline constexpr std::uint8_t kCompletion = 80;
inline constexpr std::uint8_t kMidiOutTimer = 96;
inline constexpr std::uint8_t kRender = 128;
inline constexpr std::uint8_t kPinChange = 160;
inline constexpr std::uint8_t kUnassigned = 224;
inline constexpr std::uint8_t kPluginGate = 240;  // SVCall, the .ptfx call gate's return (OS-CEPTFX S2b)
}  // namespace prio

constexpr std::uint8_t priority(Role role) noexcept {
  switch (role) {
    case Role::Fault: return prio::kFault;
    case Role::AudioDma: return prio::kAudioDma;
    case Role::SysTick: return prio::kSysTick;
    case Role::MidiUart: return prio::kMidiUart;
    case Role::Usb: return prio::kUsb;
    case Role::Completion: return prio::kCompletion;
    case Role::MidiOutTimer: return prio::kMidiOutTimer;
    case Role::Render: return prio::kRender;
    case Role::PinChange: return prio::kPinChange;
    case Role::Unassigned: return prio::kUnassigned;
    case Role::PluginGate: return prio::kPluginGate;
  }
  return prio::kUnassigned;  // not reached: every enumerator is handled above
}

// The same table as data, for `diag` and for the checks below: one row per Role, most urgent first. `work` is the row of the
// ADR 0013 D1 table in the words the diagnostics print.
struct RoleInfo {
  Role role;
  std::uint8_t priority;
  const char* name;
  const char* work;
};
inline constexpr RoleInfo kTable[] = {
    {Role::Fault, prio::kFault, "fault", "MemManage, BusFault, UsageFault"},
    {Role::AudioDma, prio::kAudioDma, "audio_dma", "I2S DMA slot boundary: advance slot, stamp cycles, pend render; no data"},
    {Role::SysTick, prio::kSysTick, "systick", "1 kHz time base"},
    {Role::MidiUart, prio::kMidiUart, "midi_uart", "UART4 RX: timestamp bytes into the MIDI rings"},
    {Role::Usb, prio::kUsb, "usb", "USB0 (ADR 0012)"},
    {Role::Completion, prio::kCompletion, "completion", "SDHC, SPI2 DMA (FT812), I2C: completion flags only"},
    {Role::MidiOutTimer, prio::kMidiOutTimer, "midi_out", "one PIT channel: emit due MIDI-out bytes"},
    {Role::Render, prio::kRender, "render", "sequencer tick and the whole audio graph, at most 60 % of the block"},
    {Role::PinChange, prio::kPinChange, "pins", "pad, button and encoder pin interrupts: flags for main"},
    {Role::Unassigned, prio::kUnassigned, "unassigned", "a line nobody has claimed"},
    {Role::PluginGate, prio::kPluginGate, "plugin_gate", "SVCall: a .ptfx plugin's return through the call gate"},
};
inline constexpr unsigned kRoleCount = sizeof(kTable) / sizeof(kTable[0]);

constexpr const char* name(Role role) noexcept {
  for (const RoleInfo& row : kTable) {
    if (row.role == role) {
      return row.name;
    }
  }
  return "?";
}

// A handler of role `a` can interrupt one of role `b` (a lower number is more urgent; equal levels never nest).
constexpr bool preempts(Role a, Role b) noexcept { return priority(a) < priority(b); }

// The table is the function and the function is the table: same order as the enum, strictly increasing priorities.
constexpr bool table_matches_function() noexcept {
  for (unsigned i = 0; i < kRoleCount; ++i) {
    if (static_cast<unsigned>(kTable[i].role) != i || kTable[i].priority != priority(kTable[i].role)) {
      return false;
    }
    if (i > 0 && kTable[i].priority <= kTable[i - 1].priority) {
      return false;
    }
  }
  return true;
}
static_assert(kRoleCount == static_cast<unsigned>(Role::PluginGate) + 1u, "every Role has a row");
static_assert(prio::kUnassigned < prio::kPluginGate, "SVCall keeps the lowest priority of the system handlers");
static_assert(table_matches_function(), "kTable lists the Roles in enum order with strictly increasing priorities");

// The ADR 0013 order, checked where it is written: a reordering is a compile error, not a latent priority inversion.
static_assert(prio::kFault < prio::kAudioDma && prio::kAudioDma < prio::kSysTick && prio::kSysTick < prio::kMidiUart &&
                  prio::kMidiUart < prio::kUsb && prio::kUsb < prio::kCompletion && prio::kCompletion < prio::kMidiOutTimer &&
                  prio::kMidiOutTimer < prio::kRender && prio::kRender < prio::kPinChange &&
                  prio::kPinChange < prio::kUnassigned,
              "ADR 0013 D1: fault < audio DMA < SysTick < MIDI < USB < completions < MIDI-out timer < render < pins");

// The consequences the audio engine relies on, as separate checks so a failure names the broken one (ADR 0013 D1):
//   * the DMA slot-boundary ISR is preempted by nothing but a fault, so its timestamps are jitter-free;
//   * every ISR that must run on time during a render (USB isochronous at 1 ms, MIDI bytes, SD/FT812 completions, the
//     MIDI-out emitter) preempts it, so a 0.9 ms render cannot starve them;
//   * the render preempts only what has no real-time duty (pins, unclaimed lines) and the main loop;
//   * the render is below the DMA ISR that pends it, so the pend always lands on a quiet line.
static_assert(preempts(Role::Fault, Role::AudioDma) && !preempts(Role::SysTick, Role::AudioDma) &&
                  !preempts(Role::Usb, Role::AudioDma) && !preempts(Role::MidiUart, Role::AudioDma),
              "only a fault may preempt the audio DMA ISR");
static_assert(preempts(Role::SysTick, Role::Render) && preempts(Role::MidiUart, Role::Render) && preempts(Role::Usb, Role::Render) &&
                  preempts(Role::Completion, Role::Render) && preempts(Role::MidiOutTimer, Role::Render),
              "every real-time ISR preempts the render");
static_assert(preempts(Role::Render, Role::PinChange) && preempts(Role::Render, Role::Unassigned),
              "the render preempts pins and unclaimed lines");
static_assert(preempts(Role::AudioDma, Role::Render), "the DMA ISR preempts the render it pends");

}  // namespace o3os::hal::irq
