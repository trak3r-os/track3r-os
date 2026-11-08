// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MIDI drivers' logic (OS-005c2): the time base the interrupts share with the render (midi/time_ref.h), the DIN receive and transmit interrupts against a fake UART and a
// fake one-shot timer (midi/din_io.h), the USB MIDI binding around the USB interrupt (midi/usb_bind.h), the settings value and its sequence lock (midi/config.h). What only
// hardware can show (the registers, the interrupt latency, the wire) is the HW-B list of the spec and platform/mk66/tests/mk66_midi_config_test.cpp holds the register
// values. Host and Cortex-M4F (`qemu_midi`).
#include <doctest/doctest.h>

#include <o3os/core/qtime.h>
#include <o3os/midi/clock_dll.h>
#include <o3os/midi/config.h>
#include <o3os/midi/din.h>
#include <o3os/midi/din_io.h>
#include <o3os/midi/in.h>
#include <o3os/midi/time_ref.h>
#include <o3os/midi/usb_bind.h>
#include <o3os/midi/usb_out.h>

#include <cstdint>

#include "support/need.h"
#include "support/rng.h"

namespace midi = o3os::midi;
namespace core = o3os::core;
namespace usb = o3os::usb;
using o3os::test::Rng;

namespace {

constexpr std::uint64_t kByte = midi::din::kByteCycles;
constexpr std::uint32_t kCyc = core::kRefCyclesPerSample.num / core::kRefCyclesPerSample.den;  // 4080 in the default build (the tests below use whole-cycle samples)

#if defined(__arm__)
constexpr unsigned kScale = 4;
#else
constexpr unsigned kScale = 1;
#endif

// A fake UART4 and PIT0 with the register policy of midi/din_io.h. Time is a 64-bit cycle count the test moves; cycles() is its low word, as the DWT's.
struct FakeHw {
  static constexpr unsigned kCyclesPerTickShift = 1;

  std::uint64_t cyc = 0;
  std::uint32_t cycles() const { return static_cast<std::uint32_t>(cyc); }

  // RX: bytes waiting in the data register(s), each with the error flags S1 shows for it.
  static constexpr std::uint32_t kRxCap = 16;
  std::uint8_t rx_byte[kRxCap] = {};
  std::uint8_t rx_flags[kRxCap] = {};
  std::uint32_t rx_n = 0;
  std::uint32_t rx_head = 0;
  std::uint32_t data_reads_without_rdrf = 0;
  void rx_put(std::uint8_t b, std::uint8_t flags = 0) {
    rx_byte[(rx_head + rx_n) % kRxCap] = b;
    rx_flags[(rx_head + rx_n) % kRxCap] = flags;
    ++rx_n;
  }
  std::uint8_t status() const {
    std::uint8_t s = midi::uart_s1::kTdre;
    if (rx_n != 0) {
      s = static_cast<std::uint8_t>(s | midi::uart_s1::kRdrf | rx_flags[rx_head]);
    }
    return s;
  }
  std::uint8_t data() {
    if (rx_n == 0) {
      ++data_reads_without_rdrf;
      return 0;
    }
    const std::uint8_t b = rx_byte[rx_head];
    rx_head = (rx_head + 1) % kRxCap;
    --rx_n;
    return b;
  }

  // TX
  bool ready = true;
  bool tx_ready() const { return ready; }
  struct Written {
    std::uint8_t byte;
    std::uint64_t at;
  };
  static constexpr std::uint32_t kTxCap = 512;
  Written tx[kTxCap];
  std::uint32_t tx_n = 0;
  void tx_write(std::uint8_t b) {
    if (tx_n < kTxCap) {
      tx[tx_n++] = Written{b, cyc};
    }
  }

  // The one-shot timer: when it fires, in cycles of the test's clock.
  bool timer_on = false;
  std::uint64_t deadline = 0;
  std::uint32_t last_ticks = 0;
  void timer_stop() { timer_on = false; }
  void timer_start(std::uint32_t ticks) {
    timer_on = true;
    last_ticks = ticks;
    deadline = cyc + (static_cast<std::uint64_t>(ticks) << kCyclesPerTickShift);
  }
};

// Fires the timer until it is off or the next fire would be after `until`, the interrupt entered `latency` cycles after the compare.
std::uint32_t run_timer(FakeHw& hw, midi::DinEmitter& em, const midi::TimeRef& ref, midi::DinTxStats& st, std::uint64_t until, std::uint32_t latency = 0) {
  std::uint32_t fires = 0;
  while (hw.timer_on && hw.deadline <= until && fires < 100000u) {
    hw.cyc = hw.deadline + latency;
    midi::DinTx::on_irq(hw, em, ref, st);
    ++fires;
  }
  return fires;
}

}  // namespace

// ---- TimeRef -----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("time ref: nothing before the first publish, then the pair maps the counter to timeline cycles, across the counter's wrap") {
  midi::TimeRef ref;
  std::uint64_t t = 123;
  CHECK_FALSE(ref.valid());
  CHECK_FALSE(ref.to_timeline(0, t));
  CHECK(t == 123u);  // untouched

  ref.publish(1000, 0xFFFFF000u);
  CHECK(ref.valid());
  NEED(ref.to_timeline(0xFFFFF000u, t));
  CHECK(t == core::cycles_from_samples(1000));
  NEED(ref.to_timeline(0x00000100u, t));  // 0x1100 cycles later: the counter wrapped
  CHECK(t == core::cycles_from_samples(1000) + 0x1100u);
  NEED(ref.to_timeline(0xFFFFE000u, t));  // 0x1000 cycles earlier
  CHECK(t == core::cycles_from_samples(1000) - 0x1000u);

  // The start of the timeline is the floor: a counter value before it is 0, never a wrapped huge number.
  ref.publish(0, 5000);
  NEED(ref.to_timeline(0, t));
  CHECK(t == 0u);
  NEED(ref.to_timeline(5000, t));
  CHECK(t == 0u);
  NEED(ref.to_timeline(5100, t));
  CHECK(t == 100u);
}

TEST_CASE("time ref: every publish replaces the pair, alternating slots, and the stamp map of MIDI in agrees with it") {
  midi::TimeRef ref;
  midi::ClockMap map;
  Rng rng(7);
  std::uint64_t sample = 5000;
  std::uint32_t cycles = 0x80001234u;
  for (int block = 0; block < 400; ++block) {
    ref.publish(sample, cycles);
    map.set_ref(sample, cycles);
    // Any counter value within a few blocks either way: the two maps (out: timeline cycles, in: Q32 samples) are one line.
    for (int k = 0; k < 4; ++k) {
      const std::uint32_t c = cycles + rng.below(1000000u) - 500000u;
      std::uint64_t t = 0;
      NEED(ref.to_timeline(c, t));
      const std::int64_t rel_cycles = static_cast<std::int64_t>(t) - static_cast<std::int64_t>(core::cycles_from_samples(sample));
      CHECK(rel_cycles == static_cast<std::int32_t>(c - cycles));
      // The input map in samples relative to `sample` itself (origin = sample): the same number of cycles, as samples.
      const core::SQ32 q = map.to_rel(c, sample);
      const double want = static_cast<double>(rel_cycles) * static_cast<double>(core::kRefCyclesPerSample.den) / static_cast<double>(core::kRefCyclesPerSample.num);
      const double got = static_cast<double>(q) / 4294967296.0;
      CHECK(got - want < 1e-6);
      CHECK(want - got < 1e-6);
    }
    sample += 64;
    cycles += static_cast<std::uint32_t>(64u * kCyc);
  }
}

// ---- DIN receive --------------------------------------------------------------------------------------------------------------------------

TEST_CASE("din rx: every byte is stamped with the counter at entry and queued for the DIN port") {
  FakeHw hw;
  midi::RtInRing ring;
  midi::DinRxStats st;
  hw.cyc = 0x1000;
  hw.rx_put(0xF8);
  midi::DinRx::isr(hw, ring, st);
  hw.cyc = 0x5000;
  hw.rx_put(0x90);
  midi::DinRx::isr(hw, ring, st);
  midi::RtItem it;
  NEED(ring.pop(it));
  CHECK(it.byte == 0xF8);
  CHECK(it.cycles == 0x1000u);
  CHECK(it.port == static_cast<std::uint8_t>(midi::Port::Din));
  NEED(ring.pop(it));
  CHECK(it.byte == 0x90);
  CHECK(it.cycles == 0x5000u);
  CHECK_FALSE(ring.pop(it));
  CHECK(st.isrs == 2u);
  CHECK(st.bytes == 2u);
  CHECK(st.errors == 0u);
  CHECK(hw.data_reads_without_rdrf == 0u);  // the data register is read only when S1 says it holds a byte
}

TEST_CASE("din rx: a framing, noise or parity error drops the byte (and clears the flag by reading it), an overrun is counted and the byte it came with is kept") {
  FakeHw hw;
  midi::RtInRing ring;
  midi::DinRxStats st;
  hw.cyc = 77;
  hw.rx_put(0x11, midi::uart_s1::kFraming);
  midi::DinRx::isr(hw, ring, st);
  hw.rx_put(0x12, midi::uart_s1::kNoise);
  midi::DinRx::isr(hw, ring, st);
  hw.rx_put(0x13, midi::uart_s1::kParity);
  midi::DinRx::isr(hw, ring, st);
  hw.rx_put(0x14, midi::uart_s1::kOverrun);
  midi::DinRx::isr(hw, ring, st);
  hw.rx_put(0x15);
  midi::DinRx::isr(hw, ring, st);
  CHECK(st.errors == 3u);
  CHECK(st.overruns == 1u);
  CHECK(st.bytes == 2u);
  midi::RtItem it;
  NEED(ring.pop(it));
  CHECK(it.byte == 0x14);
  NEED(ring.pop(it));
  CHECK(it.byte == 0x15);
  CHECK_FALSE(ring.pop(it));
  CHECK(hw.rx_n == 0u);  // nothing was left in the receiver: every flagged byte was read out
}

TEST_CASE("din rx: bounded work per interrupt, a full ring drops and counts, nothing blocks") {
  FakeHw hw;
  midi::RtInRing ring;
  midi::DinRxStats st;
  hw.cyc = 5;
  for (std::uint32_t i = 0; i < 10; ++i) {
    hw.rx_put(static_cast<std::uint8_t>(i));
  }
  midi::DinRx::isr(hw, ring, st);
  CHECK(st.bytes == midi::kMaxBytesPerIsr);  // the rest waits for the interrupt that is still pending (RDRF stays set)
  CHECK(hw.rx_n == 10u - midi::kMaxBytesPerIsr);
  CHECK(st.multi == midi::kMaxBytesPerIsr - 1u);
  // The render never drains: the ring fills and every further byte is counted, not lost silently.
  FakeHw h2;
  midi::RtInRing full;
  midi::DinRxStats s2;
  for (std::uint32_t i = 0; i < midi::kRtInRingSize + 20u; ++i) {
    h2.rx_put(0x42);
    midi::DinRx::isr(h2, full, s2);
  }
  CHECK(s2.bytes == midi::kRtInRingSize);
  CHECK(s2.ring_full == 20u);
}

// ---- DIN transmit -------------------------------------------------------------------------------------------------------------------------

TEST_CASE("din tx: a message goes out one byte per byte time from its due time, the timer is armed for each and stops when the queues are empty") {
  FakeHw hw;
  midi::DinEmitter em;
  midi::TimeRef ref;
  midi::DinTxStats st;
  ref.publish(0, 0);  // the counter reads the timeline's cycles
  const std::uint64_t due = 400000;
  CHECK(em.push_msg(due, midi::note_on(2, 60, 100)));
  hw.cyc = 1000;
  midi::DinTx::on_irq(hw, em, ref, st);  // the render's pend: nothing is due yet, the timer is armed for it
  CHECK(hw.tx_n == 0u);
  NEED(hw.timer_on);
  CHECK(hw.deadline == 1000 + (((due - 1000) >> 1) << 1));
  run_timer(hw, em, ref, st, 2000000);
  NEED(hw.tx_n == 3u);
  CHECK(hw.tx[0].byte == 0x92);
  CHECK(hw.tx[1].byte == 60);
  CHECK(hw.tx[2].byte == 100);
  CHECK(hw.tx[0].at == due);
  CHECK(hw.tx[1].at == due + kByte);
  CHECK(hw.tx[2].at == due + 2 * kByte);
  CHECK_FALSE(hw.timer_on);
  CHECK(st.bytes == 3u);
}

TEST_CASE("din tx: a clock on an idle wire leaves within a timer tick and an interrupt latency of its time") {
  FakeHw hw;
  midi::DinEmitter em;
  midi::TimeRef ref;
  midi::DinTxStats st;
  ref.publish(0, 0);
  const std::uint32_t latency = 40;  // cycles: 0.2 us of interrupt entry
  std::uint64_t worst = 0;
  Rng rng(11);
  std::uint64_t t = 100000;
  for (int i = 0; i < 200; ++i) {
    t += 300000 + rng.below(500000u);  // the wire is idle between them (a clock every 1.7..4.4 ms)
    CHECK(em.push_rt(t, midi::kClock));
    hw.cyc = t - 200000;
    midi::DinTx::on_irq(hw, em, ref, st);  // the render's pend
    const std::uint32_t before = hw.tx_n;
    run_timer(hw, em, ref, st, t + 100000, latency);
    NEED(hw.tx_n == before + 1u);
    const std::uint64_t late = hw.tx[hw.tx_n - 1].at - t;
    worst = late > worst ? late : worst;
  }
  CHECK(worst <= latency + 2u);  // one tick is two cycles; the target is 50 us = 9,000 cycles
  CHECK(st.rt_late_max <= latency + 2u);
}

TEST_CASE("din tx: clock is sacred: a clock due in the middle of a message goes at the next byte boundary, never later") {
  FakeHw hw;
  midi::DinEmitter em;
  midi::TimeRef ref;
  midi::DinTxStats st;
  ref.publish(0, 0);
  const std::uint64_t t0 = 500000;
  CHECK(em.push_msg(t0, midi::control_change(0, 7, 99)));
  CHECK(em.push_rt(t0 + kByte + 10000, midi::kClock));  // during the second byte
  hw.cyc = 1000;
  midi::DinTx::on_irq(hw, em, ref, st);
  run_timer(hw, em, ref, st, 3000000);
  NEED(hw.tx_n == 4u);
  CHECK(hw.tx[0].byte == 0xB0);
  CHECK(hw.tx[1].byte == 7);
  CHECK(hw.tx[2].byte == midi::kClock);
  CHECK(hw.tx[3].byte == 99);
  CHECK(hw.tx[2].at == t0 + 2 * kByte);
  CHECK(hw.tx[2].at - (t0 + kByte + 10000) <= kByte);
  CHECK(hw.tx[3].at == t0 + 3 * kByte);
}

TEST_CASE("din tx: no time base yet: nothing is polled, nothing lost, the timer looks again in a millisecond") {
  FakeHw hw;
  midi::DinEmitter em;
  midi::TimeRef ref;
  midi::DinTxStats st;
  CHECK(em.push_rt(100, midi::kStart));
  hw.cyc = 50;
  midi::DinTx::on_irq(hw, em, ref, st);
  CHECK(st.no_time == 1u);
  CHECK(hw.tx_n == 0u);
  NEED(hw.timer_on);
  CHECK(hw.last_ticks == midi::DinTx::kNoTimeTicks);
  CHECK(hw.deadline == 50 + 180000u);  // 90,000 ticks of two cycles: a millisecond
  ref.publish(0, 0);
  run_timer(hw, em, ref, st, 1000000);
  NEED(hw.tx_n == 1u);
  CHECK(hw.tx[0].byte == midi::kStart);
}

TEST_CASE("din tx: the data register is not empty: the step is postponed by two microseconds, nothing is popped, nothing is lost") {
  FakeHw hw;
  midi::DinEmitter em;
  midi::TimeRef ref;
  midi::DinTxStats st;
  ref.publish(0, 0);
  CHECK(em.push_msg(1000, midi::program_change(1, 5)));
  hw.ready = false;
  hw.cyc = 5000;
  midi::DinTx::on_irq(hw, em, ref, st);
  CHECK(st.not_ready == 1u);
  CHECK(hw.tx_n == 0u);
  CHECK(em.stats().msg_bytes == 0u);  // poll() was not asked: the emitter's state is as it was
  NEED(hw.timer_on);
  CHECK(hw.last_ticks == midi::DinTx::kNotReadyTicks);
  hw.ready = true;
  run_timer(hw, em, ref, st, 1000000);
  NEED(hw.tx_n == 2u);
  CHECK(hw.tx[0].byte == 0xC1);
  CHECK(hw.tx[1].byte == 5);
}

TEST_CASE("din tx: a wake farther than the ceiling is looked at again at the ceiling, and the byte still goes out on time") {
  FakeHw hw;
  midi::DinEmitter em;
  midi::TimeRef ref;
  midi::DinTxStats st;
  ref.publish(0, 0);
  const std::uint64_t due = 3ull * 180000000ull / 10ull;  // 0.3 s: three ceilings away
  CHECK(em.push_rt(due, midi::kStop));
  hw.cyc = 0;
  midi::DinTx::on_irq(hw, em, ref, st);
  CHECK(hw.last_ticks == midi::DinTx::kMaxTicks);
  CHECK(st.timer_ceiling == 1u);
  run_timer(hw, em, ref, st, due + 1000000);
  NEED(hw.tx_n == 1u);
  CHECK(hw.tx[0].byte == midi::kStop);
  CHECK(hw.tx[0].at >= due);
  CHECK(hw.tx[0].at - due <= 4u);
  CHECK(st.timer_ceiling == 2u);  // 27M ticks away, then 18M; the third wake is exactly the ceiling (9M), not beyond it
  // And the floor: a wake nearer than a microsecond is armed at the floor, not at zero (a zero LDVAL would never fire).
  CHECK(midi::DinTx::ticks_for(0, 1) == midi::DinTx::kMinTicks);
  CHECK(midi::DinTx::ticks_for(3, 1) == midi::DinTx::kMinTicks);
  CHECK(midi::DinTx::ticks_for(2ull * 28800u, 1) == 28800u);
  CHECK(midi::DinTx::ticks_for(~0ull, 1) == midi::DinTx::kMaxTicks);
}

TEST_CASE("din tx: fuzzed streams through the interrupt: bytes whole and in order, never before their time, a byte time apart, clocks within one byte time") {
  const unsigned scale = kScale;
  Rng rng(99);
  FakeHw hw;
  midi::DinEmitter em;
  midi::TimeRef ref;
  midi::DinTxStats st;
  ref.publish(0, 0);
  const std::uint32_t latency = 540;  // 3 us
  // Ordinary messages (notes, controllers) at random times and a clock every 20.8 ms (120 BPM); the render hands them over a block ahead and pends the line.
  const std::uint64_t end = 180000000ull / 2 / scale;  // half a second (a second on the host)
  std::uint64_t next_msg = 10000;
  std::uint64_t next_clock = 50000;
  std::uint32_t msgs = 0;
  std::uint32_t clocks = 0;
  static std::uint8_t expect[8192];  // static: the target's stack is small
  std::uint32_t expect_n = 0;
  std::uint32_t expect_rd = 0;
  std::uint64_t clock_due[64];
  std::uint32_t clock_head = 0;
  std::uint32_t clock_tail = 0;
  std::uint32_t errors = 0;
  std::uint64_t last_at = 0;
  bool have_last = false;
  const std::uint64_t block = 64u * kCyc;
  for (std::uint64_t now = 0; now < end; now += block) {
    // the render's block: hand over what is due in the next two blocks
    while (next_msg < now + 2 * block) {
      const std::uint32_t kind = rng.below(3);
      const std::uint8_t ch = static_cast<std::uint8_t>(rng.below(16));
      midi::Msg m = kind == 0 ? midi::note_on(ch, static_cast<std::uint8_t>(rng.below(128)), static_cast<std::uint8_t>(1 + rng.below(127)))
                    : kind == 1 ? midi::control_change(ch, static_cast<std::uint8_t>(rng.below(128)), static_cast<std::uint8_t>(rng.below(128)))
                                : midi::program_change(ch, static_cast<std::uint8_t>(rng.below(128)));
      if (em.push_msg(next_msg, m)) {
        for (std::uint32_t i = 0; i < m.len; ++i) {
          expect[expect_n++ & 8191u] = m.b[i];
        }
        ++msgs;
      }
      next_msg += 5000 + rng.below(250000u);
    }
    while (next_clock < now + 2 * block) {
      if (em.push_rt(next_clock, midi::kClock)) {
        clock_due[clock_tail++ & 63u] = next_clock;
        ++clocks;
      }
      next_clock += 3750000;
    }
    hw.cyc = now;
    midi::DinTx::on_irq(hw, em, ref, st);  // the pend
    run_timer(hw, em, ref, st, now + block - 1, latency);
    // check what was written in this block (the fake's record is then emptied)
    for (std::uint32_t seen = 0; seen < hw.tx_n; ++seen) {
      const FakeHw::Written& w = hw.tx[seen];
      if (w.byte == midi::kClock) {
        if (clock_head == clock_tail) {
          ++errors;
        } else {
          const std::uint64_t d = clock_due[clock_head++ & 63u];
          if (w.at < d || w.at - d > kByte + latency + 2) {
            ++errors;
          }
        }
      } else {
        if (expect_rd >= expect_n || expect[expect_rd++ & 8191u] != w.byte) {
          ++errors;
        }
      }
      if (have_last && w.at - last_at < kByte) {
        ++errors;  // two bytes closer than a byte time: one would have been cut on the wire
      }
      last_at = w.at;
      have_last = true;
    }
    hw.tx_n = 0;
  }
  run_timer(hw, em, ref, st, end + 10 * kByte, latency);
  CHECK(errors == 0u);
  CHECK(msgs > 20u);
  CHECK(clocks >= (end - 50000u) / 3750000u);  // every clock due before the end went through: 23 on the host, 5 on QEMU's quarter-length run
  CHECK(em.stats().rt_late_over_byte == 0u);
  CHECK(st.rt_late_max <= kByte + latency + 2u);
}

// ---- USB MIDI binding ----------------------------------------------------------------------------------------------------------------------

TEST_CASE("usb bind: an event word's message bytes by code index number, the reserved ones are nothing") {
  std::uint8_t b[3] = {9, 9, 9};
  struct Case {
    midi::Msg m;
    std::uint32_t len;
  };
  const Case cases[] = {{midi::note_on(1, 60, 100), 3}, {midi::note_off(0, 5), 3},          {midi::control_change(15, 1, 2), 3},
                        {midi::program_change(3, 9), 2}, {midi::msg1(midi::kClock), 1},       {midi::msg1(midi::kStop), 1},
                        {midi::song_position(300), 3},   {midi::msg2(0xF1, 0x22), 2},        {midi::msg2(0xD2, 0x33), 2}};
  for (const Case& c : cases) {
    const std::uint32_t ev = midi::usb_event(0, c.m);
    const std::uint32_t n = midi::usb_event_bytes(ev, b);
    CHECK(n == c.len);
    for (std::uint32_t i = 0; i < n; ++i) {
      CHECK(b[i] == c.m.b[i]);
    }
    // the cable number is not looked at
    CHECK(midi::usb_event_bytes(midi::usb_event(7, c.m), b) == c.len);
  }
  CHECK(midi::usb_event_bytes(0x00000000u, b) == 0u);  // padding
  CHECK(midi::usb_event_bytes(0x00AABB01u, b) == 0u);  // cable events
  // Two-byte system common (CIN 0x2): MTC and Song Select take two bytes, but the SPP (F2) is a three-byte message — a host that packs
  // it with CIN 0x2 still means all three bytes, and the second data byte must reach the parser (OS-MIDISYNC M2b).
  CHECK(midi::usb_event_bytes(0x02 | (0xF1u << 8) | (0x22u << 16), b) == 2u);
  CHECK(b[0] == 0xF1);
  CHECK(b[1] == 0x22);
  CHECK(midi::usb_event_bytes(0x02 | (midi::kSongPosition << 8) | (0x08u << 16) | (0x00u << 24), b) == 3u);
  CHECK(b[0] == midi::kSongPosition);
  CHECK(b[1] == 0x08);
  CHECK(b[2] == 0x00);
  // SysEx on the wire: 0x4 starts or continues (three data bytes), 0x5 ends with one, 0x6 with two, 0x7 with three.
  CHECK(midi::usb_event_bytes(0x04 | (0xF0u << 8) | (1u << 16) | (2u << 24), b) == 3u);
  CHECK(midi::usb_event_bytes(0x05 | (0xF7u << 8), b) == 1u);
  CHECK(midi::usb_event_bytes(0x06 | (5u << 8) | (0xF7u << 16), b) == 2u);
  CHECK(midi::usb_event_bytes(0x07 | (5u << 8) | (6u << 16) | (0xF7u << 24), b) == 3u);
}

TEST_CASE("usb bind: at a start of frame the events due go to the host's ring, real-time first, and the host not listening discards them") {
  midi::UsbMidiOut out;
  midi::TimeRef ref;
  usb::MidiRing to_host;
  midi::UsbBindStats st;
  ref.publish(0, 0);
  CHECK(out.push(1000, midi::usb_event(0, midi::note_on(0, 60, 90))));
  CHECK(out.push(2000, midi::usb_event(0, midi::msg1(midi::kClock))));
  CHECK(out.push(500000, midi::usb_event(0, midi::note_on(0, 62, 90))));  // not due at the first frame
  CHECK(midi::usb_midi_sof(out, ref, 180000, to_host, true, st) == 2u);
  std::uint32_t ev[16];
  NEED(to_host.read(ev, 16) == 2u);
  CHECK(ev[0] == midi::usb_event(0, midi::msg1(midi::kClock)));  // real-time lane first
  CHECK(ev[1] == midi::usb_event(0, midi::note_on(0, 60, 90)));
  CHECK(st.frames == 1u);
  CHECK(st.events_out == 2u);
  // the next frame, 1 ms later: the third event is due
  CHECK(midi::usb_midi_sof(out, ref, 180000 + 360000, to_host, true, st) == 1u);
  NEED(to_host.read(ev, 16) == 1u);
  CHECK(ev[0] == midi::usb_event(0, midi::note_on(0, 62, 90)));
  // a host that is not listening: the frame is consumed and thrown away, counted, so that nothing piles up for the next connection
  CHECK(out.push(10, midi::usb_event(0, midi::note_on(0, 1, 1))));
  CHECK(midi::usb_midi_sof(out, ref, 900000, to_host, false, st) == 0u);
  CHECK(st.discarded == 1u);
  CHECK(to_host.read(ev, 16) == 0u);
  CHECK(midi::usb_midi_sof(out, ref, 1000000, to_host, true, st) == 0u);  // nothing was left to arrive late
}

TEST_CASE("usb bind: no time base yet: the frame waits, nothing is taken from the core's rings") {
  midi::UsbMidiOut out;
  midi::TimeRef ref;
  usb::MidiRing to_host;
  midi::UsbBindStats st;
  CHECK(out.push(10, midi::usb_event(0, midi::msg1(midi::kStart))));
  CHECK(midi::usb_midi_sof(out, ref, 12345, to_host, true, st) == 0u);
  CHECK(st.no_time == 1u);
  ref.publish(0, 0);
  CHECK(midi::usb_midi_sof(out, ref, 12345, to_host, true, st) == 1u);  // still there
}

TEST_CASE("usb bind: what the host sent becomes stamped bytes of the USB port, in order, with one stamp for the pass; a full ring counts") {
  usb::MidiRing from_host;
  midi::RtInRing in;
  midi::UsbBindStats st;
  const std::uint32_t words[] = {midi::usb_event(0, midi::msg1(midi::kClock)), midi::usb_event(0, midi::note_on(2, 64, 99)),
                                 midi::usb_event(0, midi::song_position(33)), 0u /* padding */, midi::usb_event(0, midi::program_change(1, 7))};
  from_host.write(words, 5);
  CHECK(midi::usb_midi_from_host(from_host, in, 0xDEADBEEFu, st) == 1u + 3u + 3u + 2u);
  const std::uint8_t want[] = {midi::kClock, 0x92, 64, 99, midi::kSongPosition, 33, 0, 0xC1, 7};
  for (const std::uint8_t b : want) {
    midi::RtItem it;
    NEED(in.pop(it));
    CHECK(it.byte == b);
    CHECK(it.cycles == 0xDEADBEEFu);
    CHECK(it.port == static_cast<std::uint8_t>(midi::Port::Usb));
  }
  midi::RtItem it;
  CHECK_FALSE(in.pop(it));
  CHECK(st.events_in == 5u);
  CHECK(st.bytes_in == 9u);
  // the render does not drain: past the ring's size the bytes are dropped and counted
  for (std::uint32_t round = 0; round < 20; ++round) {
    std::uint32_t burst[usb::kMidiEventsPerPacket];
    for (std::uint32_t i = 0; i < usb::kMidiEventsPerPacket; ++i) {
      burst[i] = midi::usb_event(0, midi::note_on(0, static_cast<std::uint8_t>(i), 1));
    }
    from_host.write(burst, usb::kMidiEventsPerPacket);
    midi::usb_midi_from_host(from_host, in, 1, st);
  }
  CHECK(st.in_ring_full > 0u);
}

// ---- the settings value and its sequence lock ---------------------------------------------------------------------------------------------

TEST_CASE("midi config: every field is forced into its range, offsets are exact in microseconds and cycles, FOLLOW maps to the loop's bandwidths") {
  midi::MidiConfig c;
  c.mode = static_cast<midi::SyncMode>(9);
  c.in_port = static_cast<midi::Port>(7);
  c.out_ports = 0xFF;
  c.note_ports = 0xFF;
  c.notes_channel = 0x3A;
  c.follow = static_cast<midi::FollowTightness>(77);
  c.sync_correction = -100;
  c.cc_number[0] = 200;
  c.cc_number[1] = 64;
  c.in_offset_dms[0] = 30000;
  c.in_offset_dms[1] = -30000;
  c.out_offset_dms[0] = 5;
  c.out_offset_dms[1] = -5;
  const midi::MidiConfig s = midi::sanitized(c);
  CHECK(s.mode == midi::SyncMode::Off);
  CHECK(s.in_port == midi::Port::Din);
  CHECK(s.out_ports == (midi::kPortDin | midi::kPortUsb));
  CHECK(s.note_ports == (midi::kPortDin | midi::kPortUsb));
  CHECK(s.notes_channel == 0x0A);
  CHECK(s.follow == midi::FollowTightness::Normal);
  CHECK(s.sync_correction == -6);
  CHECK(s.cc_number[0] == midi::kCcUnassigned);
  CHECK(s.cc_number[1] == 64);
  CHECK(s.in_offset_dms[0] == midi::kMaxOffsetDms);
  CHECK(s.in_offset_dms[1] == -midi::kMaxOffsetDms);
  CHECK(s.out_offset_dms[0] == 5);
  CHECK(s.out_offset_dms[1] == -5);
  // a value in range is untouched
  const midi::MidiConfig d;
  const midi::MidiConfig sd = midi::sanitized(d);
  CHECK(sd.mode == d.mode);
  CHECK(sd.note_ports == midi::kPortDin);
  CHECK(sd.out_ports == 0u);
  CHECK(midi::offset_us(10) == 1000);
  CHECK(midi::offset_us(-25) == -2500);
  CHECK(midi::offset_cycles(10) == 180000);  // 1 ms
  CHECK(midi::offset_cycles(-1) == -18000);  // 100 us
  CHECK(midi::follow_mhz(midi::FollowTightness::Tight) == 1000u);
  CHECK(midi::follow_mhz(midi::FollowTightness::Normal) == 500u);
  CHECK(midi::follow_mhz(midi::FollowTightness::Loose) == 250u);
}

TEST_CASE("seq lock box: the latest write wins, an unchanged value is not read twice, a write in progress is not read") {
  struct Value {
    std::uint32_t a;
    std::uint32_t b;
  };
  midi::SeqLockBox<Value> box;
  std::uint32_t seen = 0;
  Value v{0, 0};
  CHECK_FALSE(box.read_if_changed(v, seen));  // never written
  box.write(Value{1, 2});
  NEED(box.read_if_changed(v, seen));
  CHECK(v.a == 1u);
  CHECK(v.b == 2u);
  CHECK_FALSE(box.read_if_changed(v, seen));  // nothing new
  box.write(Value{3, 4});
  box.write(Value{5, 6});  // two writes before the next read: only the last is seen
  NEED(box.read_if_changed(v, seen));
  CHECK(v.a == 5u);
  CHECK(v.b == 6u);
  CHECK_FALSE(box.read_if_changed(v, seen));
  CHECK(box.last_written().a == 5u);

  // A reader that preempts the writer finds an odd count and keeps what it has, whichever step the writer is at; it takes the value once the write is whole.
  struct TestBox : midi::SeqLockBox<Value> {
    using midi::SeqLockBox<Value>::begin_write;
    using midi::SeqLockBox<Value>::put;
    using midi::SeqLockBox<Value>::end_write;
  };
  TestBox tb;
  std::uint32_t tseen = 0;
  Value tv{7, 7};
  tb.write(Value{10, 11});
  NEED(tb.read_if_changed(tv, tseen));
  CHECK(tv.a == 10u);
  tb.begin_write();
  Value untouched{99, 98};
  const std::uint32_t seen_before = tseen;
  CHECK_FALSE(tb.read_if_changed(untouched, tseen));  // the writer is in its first step
  tb.put(Value{20, 21});
  CHECK_FALSE(tb.read_if_changed(untouched, tseen));  // and in its second: the value is half there
  CHECK(untouched.a == 99u);
  CHECK(untouched.b == 98u);
  CHECK(tseen == seen_before);
  tb.end_write();
  NEED(tb.read_if_changed(tv, tseen));
  CHECK(tv.a == 20u);
  CHECK(tv.b == 21u);
  for (std::uint32_t i = 0; i < 1000; ++i) {
    box.write(Value{i, ~i});
    NEED(box.read_if_changed(v, seen));
    CHECK(v.b == ~v.a);
  }
}
