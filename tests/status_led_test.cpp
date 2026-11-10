// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-STATUSLED: the status LED's pattern with the STATUS LED option in force (blink.h led_on_status, the pure function main.cpp's LED
// loop drives): heartbeat on, no fault -> the heartbeat blinks; heartbeat off, no fault -> the LED stays dark; heartbeat off, fault ->
// the fault code still shows; heartbeat off, over-budget (code 12, no fault) -> the LED stays dark too. Runs on the host (and the pure
// header is also cross-built for QEMU with the mk66 suite).
#include <doctest/doctest.h>

#include <o3os/platform/mk66/blink.h>

#include <cstdint>
#include <initializer_list>

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

// ---- OS-LEDBPM: the STATUS LED setting's modes and the beat flash (blink.h led_on_mode, BeatLed) ----

namespace {
using blink::BeatLed;
using blink::BeatSample;
using blink::LedMode;

BeatSample at(std::uint16_t step, std::uint8_t ustep = 0, std::uint32_t tempo_x100 = 12000, std::uint8_t count_in = 0) {
  BeatSample s;
  s.playing = true;
  s.step = step;
  s.ustep = ustep;
  s.count_in = count_in;
  s.tempo_x100 = tempo_x100;
  return s;
}
BeatSample stopped(std::uint32_t tempo_x100 = 12000) {
  BeatSample s;
  s.tempo_x100 = tempo_x100;
  return s;
}
}  // namespace

TEST_CASE("status led: the modes - Blink is the heartbeat, Off is dark, BPM shows the beat flash; a fault code blinks in all three") {
  for (std::uint32_t ms = 0; ms < blink::kHeartbeatPeriodMs; ++ms) {
    CHECK(blink::led_on_mode(LedMode::Blink, 0, ms, true) == blink::led_on(0, ms));  // the beat level is ignored
    CHECK(blink::led_on_mode(LedMode::Blink, 0, ms, false) == blink::led_on(0, ms));
    CHECK(!blink::led_on_mode(LedMode::Off, 0, ms, true));
    CHECK(blink::led_on_mode(LedMode::Bpm, 0, ms, true));
    CHECK(!blink::led_on_mode(LedMode::Bpm, 0, ms, false));  // the beat level, not the heartbeat
  }
  const std::uint32_t period = blink::period_ms(blink::kAudioFault);
  for (std::uint32_t ms = 0; ms < period; ++ms) {
    for (const LedMode m : {LedMode::Blink, LedMode::Bpm, LedMode::Off}) {
      CHECK(blink::led_on_mode(m, blink::kAudioFault, ms, false) == blink::led_on(blink::kAudioFault, ms));  // a fault wins
      CHECK(blink::led_on_mode(m, blink::kAudioFault, ms, true) == blink::led_on(blink::kAudioFault, ms));
    }
  }
  // Over budget (12, no fault): Blink blinks as before, Off is dark, BPM keeps the beat.
  for (std::uint32_t ms = 0; ms < blink::period_ms(blink::kAudioOverBudget); ++ms) {
    CHECK(blink::led_on_mode(LedMode::Blink, blink::kAudioOverBudget, ms, true) == blink::led_on(blink::kAudioOverBudget, ms));
    CHECK(!blink::led_on_mode(LedMode::Off, blink::kAudioOverBudget, ms, true));
    CHECK(blink::led_on_mode(LedMode::Bpm, blink::kAudioOverBudget, ms, true));
    CHECK(!blink::led_on_mode(LedMode::Bpm, blink::kAudioOverBudget, ms, false));
  }
}

TEST_CASE("beat led: the period follows the tempo, 120 BPM when none is known, clamped") {
  CHECK(blink::beat_period_ms(12000) == 500u);
  CHECK(blink::beat_period_ms(6000) == 1000u);
  CHECK(blink::beat_period_ms(18000) == 333u);
  CHECK(blink::beat_period_ms(0) == 500u);        // before the first publish
  CHECK(blink::beat_period_ms(1) == 6000u);       // clamped to 10 BPM
  CHECK(blink::beat_period_ms(4000000) == 60u);   // clamped to 999 BPM
}

TEST_CASE("beat led: playing - one flash per beat, on the step that starts it, 40 ms long") {
  BeatLed led;
  std::uint32_t ms = 1000;
  CHECK(!led.update(at(1), ms));  // a start in the middle of a bar: no flash until the next beat step
  CHECK(led.flashes == 0u);
  // 16 steps of 125 ms (120 BPM, 4 steps per beat), sampled every 5 ms
  std::uint32_t beat_steps = 0;
  for (std::uint32_t step = 2; step < 18; ++step) {
    const std::uint16_t s = static_cast<std::uint16_t>(step % 16u);
    const std::uint32_t before = led.flashes;
    for (std::uint32_t t = 0; t < 125; t += 5, ms += 5) {
      const bool lit = led.update(at(s, static_cast<std::uint8_t>(t * 48u / 125u)), ms);
      if (t == 0 && s % 4u == 0u) {
        CHECK(lit);  // lit at the first sample of the beat step
      }
      if (t >= blink::kBeatFlashMs && s % 4u == 0u) {
        CHECK(!lit);  // 40 ms later it is dark
      }
      if (s % 4u != 0u) {
        CHECK(!lit);  // never lit on the other steps (the flash of the beat step before is over: 125 ms > 40 ms)
      }
    }
    if (s % 4u == 0u) {
      CHECK(led.flashes == before + 1u);
      CHECK(led.last_pos == s);
      ++beat_steps;
    } else {
      CHECK(led.flashes == before);
    }
  }
  CHECK(beat_steps == 4u);
}

TEST_CASE("beat led: play from step 0 flashes at once; a loop back to step 0 flashes; a pass that skips the beat step flashes late, not never") {
  BeatLed led;
  CHECK(led.update(at(0), 100));
  CHECK(led.last_pos == 0u);
  CHECK(led.flashes == 1u);
  CHECK(!led.update(at(0, 20), 160));  // 60 ms on: the 40 ms flash is over, and no new flash (same step)
  CHECK(led.flashes == 1u);
  CHECK(!led.update(at(3), 300));
  CHECK(led.update(at(5), 600));  // step 4 was never seen: step 5 is in a new beat, so it flashes (late)
  CHECK(led.flashes == 2u);
  CHECK(!led.update(at(7), 900));
  CHECK(led.update(at(0), 1000));  // a pattern of 8 steps loops: its step 0 is a beat
  CHECK(led.flashes == 3u);
  CHECK(led.update(at(0, 47), 1010));  // still the same flash (10 ms), nothing new
  CHECK(led.flashes == 3u);
}

TEST_CASE("beat led: patterns of one and two steps flash at each loop of step 0") {
  BeatLed one;
  std::uint32_t ms = 0;
  for (int loop = 0; loop < 4; ++loop) {
    for (std::uint8_t us = 0; us < 48; us = static_cast<std::uint8_t>(us + 12), ms += 10) {
      one.update(at(0, us), ms);
    }
  }
  CHECK(one.flashes == 4u);  // the ustep wrapping on step 0 is a new step 0
  BeatLed two;
  ms = 0;
  for (int loop = 0; loop < 3; ++loop) {
    for (std::uint16_t st = 0; st < 2; ++st) {
      for (std::uint8_t us = 0; us < 48; us = static_cast<std::uint8_t>(us + 24), ms += 10) {
        two.update(at(st, us), ms);
      }
    }
  }
  CHECK(two.flashes == 3u);  // step 1 is no beat step
}

TEST_CASE("beat led: a count-in beats on count_in % 4 == 0 and the first step of the song flashes") {
  BeatLed led;
  std::uint32_t ms = 0;
  std::uint32_t flashes = 0;
  for (int left = 8; left >= 1; --left, ms += 125) {  // a count-in of 8 steps: `step` stays 0 while it runs
    led.update(at(0, 0, 12000, static_cast<std::uint8_t>(left)), ms);
    if (left == 8 || left == 4) {
      ++flashes;
    }
    CHECK(led.flashes == flashes);
  }
  CHECK(led.last_pos == 508u);  // 512 - 4
  led.update(at(0, 0), ms);  // the count-in is over: step 0 of the song
  CHECK(led.flashes == flashes + 1u);
  CHECK(led.last_pos == 0u);
}

TEST_CASE("beat led: stopped - free-run at the project tempo, the first flash at once, a tempo change from the next flash") {
  BeatLed led;
  CHECK(led.update(stopped(), 0));  // power-up: the first flash is immediate
  CHECK(led.last_pos == blink::kNoBeatPos);
  CHECK(!led.update(stopped(), 40));
  CHECK(!led.update(stopped(), 499));
  CHECK(led.update(stopped(), 500));  // 120 BPM: a beat every 500 ms
  CHECK(led.flashes == 2u);
  CHECK(!led.update(stopped(), 540));
  CHECK(!led.update(stopped(6000), 1000));  // 60 BPM now: the next beat is 1000 ms after the last one (500)
  CHECK(led.update(stopped(6000), 1500));
  CHECK(led.flashes == 3u);
  // a pass late by more than a beat resynchronises (no burst of flashes)
  CHECK(led.update(stopped(6000), 9000));
  CHECK(led.flashes == 4u);
  CHECK(!led.update(stopped(6000), 9500));
  CHECK(led.update(stopped(6000), 10000));
  CHECK(led.flashes == 5u);
}

TEST_CASE("beat led: stop -> free-run (a flash at the stop), play -> locked again") {
  BeatLed led;
  CHECK(led.update(at(0), 0));
  CHECK(!led.update(at(2), 300));
  CHECK(led.update(stopped(), 400));  // a stop: the free-run starts with a flash
  CHECK(led.last_pos == blink::kNoBeatPos);
  CHECK(led.flashes == 2u);
  CHECK(!led.update(stopped(), 800));
  CHECK(led.update(stopped(), 900));
  CHECK(led.update(at(0), 950));  // play start aligned to step 0: a flash on the step, not on the free-run's grid
  CHECK(led.last_pos == 0u);
  CHECK(led.flashes == 4u);
  CHECK(!led.update(at(1), 1100));
}

TEST_CASE("beat led: the millisecond clock wrapping is harmless") {
  BeatLed led;
  const std::uint32_t t0 = 0xFFFFFF00u;
  CHECK(led.update(stopped(), t0));
  CHECK(!led.update(stopped(), t0 + 100u));
  CHECK(led.update(stopped(), t0 + 500u));  // t0 + 500 wraps past zero
  CHECK(led.flashes == 2u);
  CHECK(!led.update(stopped(), t0 + 540u));
}
