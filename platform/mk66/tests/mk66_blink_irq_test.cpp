// SPDX-License-Identifier: GPL-3.0-or-later
//
// The status LED language (blink.h), the interrupt priority table (hal/irq.h) and its mapping to MK66 lines (irq_map.h).
// Runs on the host and on QEMU's Cortex-M4F (`qemu_mk66`).
#include <doctest/doctest.h>

#include <o3os/hal/irq.h>
#include <o3os/platform/mk66/blink.h>
#include <o3os/platform/mk66/irq_list.h>
#include <o3os/platform/mk66/irq_map.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace blink = o3os::platform::mk66::blink;
namespace irq = o3os::hal::irq;
namespace mk66 = o3os::platform::mk66;

namespace {

// Number of separate on-pulses in one period of a pattern, and the total on-time, by sampling every millisecond.
struct Shape {
  unsigned pulses = 0;
  std::uint32_t on_ms = 0;
  std::uint32_t first_on_ms = 0xFFFFFFFFu;
  std::uint32_t longest_on = 0;
};

Shape measure(std::uint8_t code) {
  Shape shape;
  const std::uint32_t period = blink::period_ms(code);
  bool previous = false;
  std::uint32_t run = 0;
  for (std::uint32_t t = 0; t < period; ++t) {
    const bool on = blink::led_on(code, t);
    if (on) {
      ++shape.on_ms;
      ++run;
      if (shape.first_on_ms == 0xFFFFFFFFu) {
        shape.first_on_ms = t;
      }
      if (!previous) {
        ++shape.pulses;
      }
    } else {
      if (run > shape.longest_on) {
        shape.longest_on = run;
      }
      run = 0;
    }
    previous = on;
  }
  if (run > shape.longest_on) {
    shape.longest_on = run;
  }
  return shape;
}

}  // namespace

TEST_CASE("blink: heartbeat is 100 ms on in every second") {
  const Shape s = measure(0);
  CHECK(blink::period_ms(0) == 1000u);
  CHECK(s.pulses == 1u);
  CHECK(s.on_ms == 100u);
  CHECK(s.first_on_ms == 0u);
  CHECK(blink::led_on(0, 0));
  CHECK(blink::led_on(0, 99));
  CHECK_FALSE(blink::led_on(0, 100));
  CHECK(blink::led_on(0, 1000));        // periodic
  CHECK(blink::led_on(0, 5000000u));
  CHECK_FALSE(blink::led_on(0, 5000500u));
}

TEST_CASE("blink: n pulses means n flashes of 200 ms, then a pause") {
  for (std::uint8_t n = 1; n <= 12; ++n) {
    const Shape s = measure(n);
    INFO("code " << static_cast<int>(n));
    CHECK(blink::period_ms(n) == n * 400u + 1600u);
    CHECK(s.pulses == n);
    CHECK(s.on_ms == n * 200u);
    CHECK(s.longest_on == 200u);
    CHECK(s.first_on_ms == 0u);
    // The pause is dark.
    for (std::uint32_t t = n * 400u; t < blink::period_ms(n); t += 50) {
      CHECK_FALSE(blink::led_on(n, t));
    }
  }
}

TEST_CASE("blink: the codes are distinct patterns, display and audio share one LED") {
  // Every code of every slice in one list: a new slice that reuses a number fails here, not on the bench.
  const std::uint8_t codes[] = {blink::kClockFailure,  blink::kSdramFailure,  blink::kDisplayFailure, blink::kDisplayPolled,
                                blink::kCrashSeen,     blink::kDrillRoundTrip, blink::kDisplayStalled, blink::kAppFailure,
                                blink::kAudioFailure,  blink::kAudioFault,     blink::kAudioOverBudget};
  for (std::size_t i = 0; i < sizeof codes; ++i) {
    for (std::size_t j = i + 1; j < sizeof codes; ++j) {
      CHECK(codes[i] != codes[j]);
    }
    CHECK(codes[i] >= 1);   // 0 is the heartbeat
  }
  CHECK(blink::kClockFailure == 2);
  CHECK(blink::kSdramFailure == 3);
  CHECK(blink::kCrashSeen == 6);
  CHECK(blink::kDrillRoundTrip == 7);
  // The audio codes follow the display's (4, 5, 8, 9): 10 and up.
  CHECK(blink::kAudioFailure == 10);
  CHECK(blink::kAudioFault == 11);
  CHECK(blink::kAudioOverBudget == 12);
}

TEST_CASE("irq: ADR 0013 D1 priority table, value by value") {
  CHECK(irq::priority(irq::Role::Fault) == 0);
  CHECK(irq::priority(irq::Role::AudioDma) == 16);
  CHECK(irq::priority(irq::Role::SysTick) == 32);
  CHECK(irq::priority(irq::Role::MidiUart) == 48);
  CHECK(irq::priority(irq::Role::Usb) == 64);
  CHECK(irq::priority(irq::Role::Completion) == 80);
  CHECK(irq::priority(irq::Role::MidiOutTimer) == 96);
  CHECK(irq::priority(irq::Role::Render) == 128);
  CHECK(irq::priority(irq::Role::PinChange) == 160);
  CHECK(irq::priority(irq::Role::Unassigned) == 224);
  CHECK(irq::priority(irq::Role::PluginGate) == 240);  // OS-CEPTFX S2b: SVCall keeps the lowest system-handler priority
}

TEST_CASE("irq: the audio DMA ISR preempts everything, the render is preempted by every I/O ISR") {
  const irq::Role below_render[] = {irq::Role::Fault,      irq::Role::AudioDma, irq::Role::SysTick, irq::Role::MidiUart,
                                    irq::Role::Usb,        irq::Role::Completion, irq::Role::MidiOutTimer};
  for (const irq::Role role : below_render) {
    CHECK(irq::priority(role) < irq::priority(irq::Role::Render));
  }
  CHECK(irq::priority(irq::Role::PinChange) > irq::priority(irq::Role::Render));  // pad ISRs never delay audio
  CHECK(irq::priority(irq::Role::Unassigned) > irq::priority(irq::Role::PinChange));
  for (const irq::Role role : {irq::Role::SysTick, irq::Role::MidiUart, irq::Role::Usb, irq::Role::Completion}) {
    CHECK(irq::priority(irq::Role::AudioDma) < irq::priority(role));
  }
}

TEST_CASE("mk66 irq map: 100 lines, numbered 0..99, the ones with roles") {
  CHECK(mk66::kIrqCount == 100u);
  CHECK(static_cast<unsigned>(mk66::Irq::DMA0_DMA16) == 0u);
  CHECK(static_cast<unsigned>(mk66::Irq::I2S0_Tx) == 28u);
  CHECK(static_cast<unsigned>(mk66::Irq::I2S0_Rx) == 29u);
  CHECK(static_cast<unsigned>(mk66::Irq::PIT0) == 48u);
  CHECK(static_cast<unsigned>(mk66::Irq::USB0) == 53u);
  CHECK(static_cast<unsigned>(mk66::Irq::PORTA) == 59u);
  CHECK(static_cast<unsigned>(mk66::Irq::SWI) == 64u);
  CHECK(static_cast<unsigned>(mk66::Irq::SPI2) == 65u);
  CHECK(static_cast<unsigned>(mk66::Irq::UART4_RX_TX) == 66u);
  CHECK(static_cast<unsigned>(mk66::Irq::SDHC) == 81u);
  CHECK(static_cast<unsigned>(mk66::Irq::CAN1_Wake_Up) == 99u);

  CHECK(mk66::role_of(mk66::Irq::I2S0_Tx) == irq::Role::AudioDma);
  CHECK(mk66::role_of(mk66::Irq::UART4_RX_TX) == irq::Role::MidiUart);
  CHECK(mk66::role_of(mk66::Irq::USB0) == irq::Role::Usb);
  CHECK(mk66::role_of(mk66::Irq::SDHC) == irq::Role::Completion);
  CHECK(mk66::role_of(mk66::Irq::SPI2) == irq::Role::Completion);
  CHECK(mk66::role_of(mk66::Irq::PIT0) == irq::Role::MidiOutTimer);
  CHECK(mk66::role_of(mk66::Irq::SWI) == irq::Role::Render);
  CHECK(mk66::role_of(mk66::Irq::PORTC) == irq::Role::PinChange);
  // A line nobody claimed is Unassigned, so a peripheral enabled early cannot preempt audio.
  CHECK(mk66::role_of(mk66::Irq::CAN0_Error) == irq::Role::Unassigned);
  CHECK(mk66::role_of(mk66::Irq::ENET_Receive) == irq::Role::Unassigned);
  CHECK(mk66::role_of(mk66::Irq::DMA3_DMA19) == irq::Role::Unassigned);
  // OS-005a: the two I2S eDMA channels (resources.h: 13 and 12) are in the audio DMA row; the display's receive channel 15 (OS-007c) in the
  // completion row; its transmit channel 14 raises no interrupt and the other DMA lines are not claimed.
  CHECK(mk66::role_of(mk66::Irq::DMA13_DMA29) == irq::Role::AudioDma);
  CHECK(mk66::role_of(mk66::Irq::DMA12_DMA28) == irq::Role::AudioDma);
  CHECK(mk66::role_of(mk66::Irq::DMA15_DMA31) == irq::Role::Completion);
  CHECK(mk66::role_of(mk66::Irq::DMA14_DMA30) == irq::Role::Unassigned);
  CHECK(mk66::role_of(mk66::Irq::DMA11_DMA27) == irq::Role::Unassigned);
  CHECK(mk66::role_of(mk66::Irq::PIT1) == irq::Role::Unassigned);
}

TEST_CASE("mk66 irq map: no line is listed twice, and every priority fits the four implemented bits") {
  for (unsigned i = 0; i < mk66::kLineRoleCount; ++i) {
    for (unsigned j = i + 1; j < mk66::kLineRoleCount; ++j) {
      CHECK(mk66::kLineRoles[i].line != mk66::kLineRoles[j].line);
    }
  }
  for (unsigned line = 0; line < mk66::kIrqCount; ++line) {
    const std::uint8_t p = irq::priority(mk66::role_of(static_cast<mk66::Irq>(line)));
    CHECK((p & 0x0Fu) == 0u);
  }
  CHECK(mk66::kMemManageRole == irq::Role::Fault);
  CHECK(mk66::kBusFaultRole == irq::Role::Fault);
  CHECK(mk66::kUsageFaultRole == irq::Role::Fault);
  CHECK(mk66::kSysTickRole == irq::Role::SysTick);
  CHECK(mk66::kPendSvRole == irq::Role::Render);
}
