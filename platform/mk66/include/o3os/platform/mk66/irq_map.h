// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which device interrupt line has which role in the priority table of ADR 0013 section D1 (hal/irq.h). The table of
// priorities is the HAL's; this file only says which MK66 lines belong to which row, and `apply_priorities()`
// (src/nvic.cpp) writes it into the NVIC at boot. A line not listed here is Unassigned (below everything with a role),
// so a peripheral enabled before its row is written here can never preempt the audio path.
//
// The DMA channels and PIT channels each line belongs to are named in resources.h (OS-005a), which checks them against this
// table.

#include <o3os/hal/irq.h>
#include <o3os/platform/mk66/irq_list.h>

#include <cstdint>

namespace o3os::platform::mk66 {

struct LineRole {
  Irq line;
  hal::irq::Role role;
};

inline constexpr LineRole kLineRoles[] = {
    // I2S0 peripheral interrupts (FIFO errors and the like; none is enabled: the boundary ISR polls the flags) and the two
    // eDMA channels that move the I2S buffers (resources.h: channel 13 transmit, 12 receive). Only the transmit channel's line
    // is enabled; it is the slot-boundary interrupt.
    {Irq::I2S0_Tx, hal::irq::Role::AudioDma},
    {Irq::I2S0_Rx, hal::irq::Role::AudioDma},
    {Irq::DMA12_DMA28, hal::irq::Role::AudioDma},
    {Irq::DMA13_DMA29, hal::irq::Role::AudioDma},
    // eDMA channels 14 and 15 are the display's (OS-007c: DSPI2 TX and RX; resources.h). The FT812 bus's receive channel 15 raises
    // DMA15_DMA31 when a buffer has gone out (display/edma_config.h): completion flags only, at the completion priority. The
    // transmit channel 14 raises nothing, so its line stays Unassigned (lowest). Each line appears once in this table.
    {Irq::DMA15_DMA31, hal::irq::Role::Completion},
    // MIDI DIN is UART4 (ADR 0013 D1).
    {Irq::UART4_RX_TX, hal::irq::Role::MidiUart},
    {Irq::UART4_ERR, hal::irq::Role::MidiUart},
    {Irq::USB0, hal::irq::Role::Usb},
    // Completion flags only: SDHC DMA, SPI2 (the FT812), the four I2C buses.
    {Irq::SDHC, hal::irq::Role::Completion},
    {Irq::SPI2, hal::irq::Role::Completion},
    {Irq::I2C0, hal::irq::Role::Completion},
    {Irq::I2C1, hal::irq::Role::Completion},
    {Irq::I2C2, hal::irq::Role::Completion},
    {Irq::I2C3, hal::irq::Role::Completion},
    // The one PIT channel of the MIDI-out emitter (ADR 0013 D5): PIT0 (resources.h; the other three PIT lines stay Unassigned).
    {Irq::PIT0, hal::irq::Role::MidiOutTimer},
    // The software-pended render line (D1): the K66's dedicated software interrupt (IRQ 64, the line PJRC's Audio library also uses as
    // `IRQ_SOFTWARE`). PendSV is the fallback.
    {Irq::SWI, hal::irq::Role::Render},
    // Pad, button and encoder pin interrupts.
    {Irq::PORTA, hal::irq::Role::PinChange},
    {Irq::PORTB, hal::irq::Role::PinChange},
    {Irq::PORTC, hal::irq::Role::PinChange},
    {Irq::PORTD, hal::irq::Role::PinChange},
    {Irq::PORTE, hal::irq::Role::PinChange},
};
inline constexpr unsigned kLineRoleCount = sizeof(kLineRoles) / sizeof(kLineRoles[0]);

constexpr hal::irq::Role role_of(Irq line) noexcept {
  for (const LineRole& entry : kLineRoles) {
    if (entry.line == line) {
      return entry.role;
    }
  }
  return hal::irq::Role::Unassigned;
}

// Cortex-M4 system handlers (exception numbers 4..15) the boot code gives a priority to, per the same table.
// MemManage, BusFault and UsageFault are Fault (they must preempt any ISR that causes them); SysTick has its row;
// PendSV is the render's fallback line; SVCall is the .ptfx call gate's return (OS-CEPTFX S2b) and keeps the lowest
// priority of the system handlers. Any level works from thread mode, but the gate must never be called inside a
// critical section: a PRIMASK/BASEPRI held around the call would mask the plugin's own return through SVC.
inline constexpr hal::irq::Role kMemManageRole = hal::irq::Role::Fault;
inline constexpr hal::irq::Role kBusFaultRole = hal::irq::Role::Fault;
inline constexpr hal::irq::Role kUsageFaultRole = hal::irq::Role::Fault;
inline constexpr hal::irq::Role kSvCallRole = hal::irq::Role::PluginGate;
inline constexpr hal::irq::Role kPendSvRole = hal::irq::Role::Render;
inline constexpr hal::irq::Role kSysTickRole = hal::irq::Role::SysTick;

// The Cortex-M4 of the MK66 implements four priority bits: only the top nibble of each 8-bit field counts, so every
// priority in use must be a multiple of 16 or two rows could collapse into one.
inline constexpr unsigned kPriorityBits = 4;
constexpr bool priorities_representable() noexcept {
  constexpr std::uint8_t kLowMask = (1u << (8u - kPriorityBits)) - 1u;
  const hal::irq::Role all[] = {hal::irq::Role::Fault,        hal::irq::Role::AudioDma, hal::irq::Role::SysTick,
                                hal::irq::Role::MidiUart,     hal::irq::Role::Usb,      hal::irq::Role::Completion,
                                hal::irq::Role::MidiOutTimer, hal::irq::Role::Render,   hal::irq::Role::PinChange,
                                hal::irq::Role::Unassigned,   hal::irq::Role::PluginGate};
  for (const hal::irq::Role role : all) {
    if ((hal::irq::priority(role) & kLowMask) != 0u) {
      return false;
    }
  }
  return true;
}
static_assert(priorities_representable(), "MK66 implements 4 priority bits: every ADR 0013 D1 priority is a multiple of 16");

}  // namespace o3os::platform::mk66
