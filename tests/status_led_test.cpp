// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-STATUSLED: the status LED's pattern with the STATUS LED option in force (blink.h led_on_status, the pure function main.cpp's LED
// loop drives): heartbeat on, no fault -> the heartbeat blinks; heartbeat off, no fault -> the LED stays dark; heartbeat off, fault ->
// the fault code still shows; heartbeat off, over-budget (code 12, no fault) -> the LED stays dark too. Runs on the host (and the pure
// header is also cross-built for QEMU with the mk66 suite).
#include <doctest/doctest.h>

#include <o3os/platform/mk66/blink.h>

#include <cstdint>

namespace blink = o3os::platform::mk66::blink;

TEST_CASE("status led: heartbeat on, no fault -> the heartbeat blinks") {
  for (std::uint32_t ms = 0; ms < blink::kHeartbeatPeriodMs; ++ms) {
    CHECK(blink::led_on_status(0, ms, true) == blink::led_on(0, ms));  // the option changes nothing while it is BLINK
  }
  CHECK(blink::led_on_status(0, 0, true));     // on at the start of the period
  CHECK(!blink::led_on_status(0, 500, true));  // dark in the long pause
}

TEST_CASE("status led: heartbeat off, no fault -> the LED stays dark") {
  for (std::uint32_t ms = 0; ms < 3u * blink::kHeartbeatPeriodMs; ++ms) {
    CHECK(!blink::led_on_status(0, ms, false));
  }
}

TEST_CASE("status led: heartbeat off, fault -> the fault code still shows, exactly as without the option") {
  const std::uint8_t codes[] = {blink::kAudioFault, blink::kAudioFailure, blink::kAppFailure, blink::kDisplayStalled, blink::kInputDegraded};
  for (const std::uint8_t code : codes) {
    const std::uint32_t period = blink::period_ms(code);
    bool any_on = false;
    for (std::uint32_t ms = 0; ms < period; ++ms) {
      const bool on = blink::led_on_status(code, ms, false);
      any_on = any_on || on;
      CHECK(on == blink::led_on(code, ms));  // a fault code is not touched by the option
    }
    CHECK(any_on);  // the code really shows (it has on-pulses)
  }
}

TEST_CASE("status led: over-budget (code 12, no fault) -> dark at every ms with the option off, exactly led_on(12, ms) with it on") {
  const std::uint32_t period = blink::period_ms(blink::kAudioOverBudget);
  bool any_on = false;
  for (std::uint32_t ms = 0; ms < period; ++ms) {
    CHECK(!blink::led_on_status(blink::kAudioOverBudget, ms, false));  // code 12 is hidden with the option off
    any_on = any_on || blink::led_on(blink::kAudioOverBudget, ms);
    CHECK(blink::led_on_status(blink::kAudioOverBudget, ms, true) == blink::led_on(blink::kAudioOverBudget, ms));  // BLINK changes nothing
  }
  CHECK(any_on);  // the code really has on-pulses (it blinks with the option on)
}

TEST_CASE("status led: heartbeat on, fault -> the fault code shows (the option is BLINK, today's behaviour)") {
  const std::uint32_t period = blink::period_ms(blink::kAudioFault);
  bool any_on = false;
  for (std::uint32_t ms = 0; ms < period; ++ms) {
    any_on = any_on || blink::led_on_status(blink::kAudioFault, ms, true);
  }
  CHECK(any_on);
}
