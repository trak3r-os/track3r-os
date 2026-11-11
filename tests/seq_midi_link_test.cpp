// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI sync end to end through seq::MidiLink (OS-005c2): the sequencer, the router, the DIN emitter and USB out on one side, simulated masters stamping bytes into the
// midiRt rings on the other, driven block by block the way the engine does. Master: clock, Start, SPP + Continue and Stop on the enabled ports, the OUT OFFSET, the hang
// bookkeeping. Slave: the follow loop, the transport, the IN OFFSET, the port that is followed, a master that goes quiet, a mode change while following, the Play key under an
// external clock, the relay. The ISR side of the ports is the emitter's own poll() stepped through each block (the interrupt logic is midi_io_test.cpp's). Host and Cortex-M4F
// (`qemu_seq_model`).
#include <doctest/doctest.h>

#include <o3os/core/qtime.h>
#include <o3os/midi/config.h>
#include <o3os/midi/usb_bind.h>
#include <o3os/model/params.h>
#include <o3os/model/settings.h>
#include <o3os/seq/midi_link.h>
#include <o3os/usb/cdc.h>

#include "usb_rig.h"
#include "usb_test_util.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;

namespace {

constexpr std::uint64_t kCyc = core::kRefCyclesPerSample.num / core::kRefCyclesPerSample.den;  // 4080 in the default build
constexpr double kCycD = static_cast<double>(core::kRefCyclesPerSample.num) / static_cast<double>(core::kRefCyclesPerSample.den);
constexpr std::uint64_t kClockPeriod = 3750000;  // cycles: 24 ppqn at 120 BPM
constexpr std::uint64_t kByte = midi::din::kByteCycles;
constexpr std::uint64_t kFrame = midi::usb_out::kFrameCycles;

// What went out: DIN bytes with the cycle each started, USB event words with the frame that carried them.
struct Wires {
  static constexpr std::uint32_t kCap = 6000;
  std::uint8_t din[kCap];
  std::uint64_t din_at[kCap];
  std::uint32_t din_n = 0;
  std::uint32_t usb[kCap];
  std::uint64_t usb_at[kCap];
  std::uint32_t usb_n = 0;

  std::uint32_t count_din(std::uint8_t b) const {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < din_n; ++i) {
      c += din[i] == b ? 1u : 0u;
    }
    return c;
  }
  // The i-th DIN byte equal to b: its index, or din_n.
  std::uint32_t find_din(std::uint8_t b, std::uint32_t idx = 0) const {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < din_n; ++i) {
      if (din[i] == b) {
        if (c == idx) {
          return i;
        }
        ++c;
      }
    }
    return din_n;
  }
  std::uint32_t count_usb_byte0(std::uint8_t b) const {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < usb_n; ++i) {
      c += static_cast<std::uint8_t>((usb[i] >> 8) & 0xFFu) == b ? 1u : 0u;
    }
    return c;
  }
};

Wires g_wires;
Collector& g_col = seq_collector();
StaticSlot<seq::MidiLink> g_link_slot;
std::uint32_t g_kicks = 0;

// A master clock for the Slave tests: 120 BPM, one clock every 3,750,000 cycles from `t0`, stamped one byte time after its start as the UART interrupt would.
struct Master {
  double t0 = 3.0e6;
  double period = static_cast<double>(kClockPeriod);
  bool quiet = false;  // no clocks (a pulled cable)
  std::uint32_t next = 0;
  double time_of(std::uint32_t c) const { return t0 + static_cast<double>(c) * period; }
};

struct Rig {
  seq::Sequencer* s = nullptr;
  seq::MidiLink* link = nullptr;
  std::uint64_t sample = 0;
  std::uint64_t din_now = 0;
  std::uint64_t next_frame = 0;
  Master m;
  // bytes the master sends besides its clock: (true time in cycles, byte, port)
  struct Extra {
    double t;
    std::uint8_t byte;
    std::uint8_t port;
  };
  Extra extra[16];
  std::uint32_t extra_n = 0;

  void init(const midi::MidiConfig& cfg) {
    g_col.n = 0;
    g_col.total_events = 0;
    g_wires.din_n = 0;
    g_wires.usb_n = 0;
    g_kicks = 0;
    s = &seq_slot().make(seq_project());
    seq::MidiLinkHooks hooks;
    hooks.kick = [](void*) noexcept { ++g_kicks; };
    link = &g_link_slot.make(*s, hooks);  // no heard_at: the counter reads the timeline's own cycles
    link->set_config(cfg);
    sample = 0;
    din_now = 0;
    next_frame = 0;
    m = Master{};
    extra_n = 0;
  }

  void send(double true_cycles, std::uint8_t byte, std::uint8_t port = 0) {
    extra[extra_n++] = Extra{true_cycles, byte, port};
  }

  // The master's bytes whose stamps (a byte time after they started) have passed by `now`.
  void feed_master(double now_cycles) {
    for (;;) {
      std::uint32_t best = extra_n;
      for (std::uint32_t i = 0; i < extra_n; ++i) {
        if (best == extra_n || extra[i].t < extra[best].t) {
          best = i;
        }
      }
      const double clock_t = m.time_of(m.next) + 57600.0;
      const bool clock_ready = !m.quiet && clock_t < now_cycles;
      const bool extra_ready = best != extra_n && extra[best].t + 57600.0 < now_cycles;
      if (extra_ready && (!clock_ready || extra[best].t <= m.time_of(m.next))) {
        midi::RtInRing& ring = extra[best].port == 0 ? link->din_in() : link->usb_in();
        CHECK(ring.push(midi::RtItem{static_cast<std::uint32_t>(static_cast<std::uint64_t>(extra[best].t + 57600.0)), extra[best].byte, extra[best].port, 0}));
        extra[best] = extra[extra_n - 1];
        --extra_n;
        continue;
      }
      if (!clock_ready) {
        break;
      }
      CHECK(link->din_in().push(midi::RtItem{static_cast<std::uint32_t>(static_cast<std::uint64_t>(clock_t)), midi::kClock, 0, 0}));
      ++m.next;
    }
  }

  // One block of 64 samples: the master's bytes, the link's step, then the ports' interrupts as they would have run over the block.
  void block() {
    audio::SeqEvent out[audio::kMaxEventsPerBlock];
    feed_master(static_cast<double>(sample) * kCycD);
    const std::uint32_t c = link->step(sample, 64, out, audio::kMaxEventsPerBlock);
    for (std::uint32_t i = 0; i < c; ++i) {
      g_col.add(sample, out[i]);
    }
    // The ports' interrupts run from the time this block was rendered to the time its first sample is heard, one block later (the TX ring's render-ahead): events of the
    // block that are due before that (an OUT advance) leave at once, the others wait for later blocks' passes.
    const std::uint64_t end = core::cycles_from_samples(sample);
    midi::DinEmitter& em = link->din();
    for (std::uint32_t guard = 0; guard < 64; ++guard) {
      const std::uint64_t w = em.next_wake(din_now);
      if (w == midi::kNever || w >= end) {
        break;
      }
      din_now = w;
      const midi::DinEmitter::Tx tx = em.poll(w);
      if (tx.valid && g_wires.din_n < Wires::kCap) {
        g_wires.din[g_wires.din_n] = tx.byte;
        g_wires.din_at[g_wires.din_n] = tx.start;
        ++g_wires.din_n;
      } else if (!tx.valid) {
        ++din_now;
      }
    }
    din_now = din_now < end ? end : din_now;
    while (next_frame < end) {
      std::uint32_t ev[midi::usb_out::kEventsPerFrame];
      const std::uint32_t n = link->usb_out().frame(next_frame, ev, midi::usb_out::kEventsPerFrame);
      for (std::uint32_t i = 0; i < n && g_wires.usb_n < Wires::kCap; ++i) {
        g_wires.usb[g_wires.usb_n] = ev[i];
        g_wires.usb_at[g_wires.usb_n] = next_frame;
        ++g_wires.usb_n;
      }
      next_frame += kFrame;
    }
    sample += 64;
  }

  template <typename F>
  void run_to(std::uint64_t to_sample, F at_block) {
    while (sample < to_sample) {
      at_block(sample);
      block();
    }
  }
  void run_to(std::uint64_t to_sample) {
    run_to(to_sample, [](std::uint64_t) {});
  }
};

Rig g_rig;

// Pattern 1: 16 steps, a note on every step of track 0 played by the MIDI instrument (slot 10, channel 3 = wire channel 2), the step number as the note; one song position.
void midi_project(std::uint32_t tempo_x100 = 12000) {
  fresh_project(2, 2);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(tempo_x100) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(static_cast<std::uint8_t>(40 + k), 11)) == model::Status::Ok);
  }
  CHECK(p.append_order(1) == model::Status::Ok);
}

// A song of three 16-step patterns (48 steps in all), a note on every step of track 0, the global song step (0..47) as the note.
void song_project(std::uint32_t tempo_x100 = 12000) {
  fresh_project(2, 2);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(tempo_x100) == model::Status::Ok);
  for (std::uint32_t id = 1; id <= 3; ++id) {
    CHECK(p.create_pattern(id, 16) == model::Status::Ok);
    for (std::uint32_t k = 0; k < 16; ++k) {
      CHECK(p.set_step(id, 0, k, make_step(static_cast<std::uint8_t>(40 + (id - 1u) * 16u + k), 11)) == model::Status::Ok);
    }
    CHECK(p.append_order(id) == model::Status::Ok);
  }
}

midi::MidiConfig master_cfg(std::uint8_t out_ports) {
  midi::MidiConfig c;
  c.mode = midi::SyncMode::Master;
  c.out_ports = out_ports;
  c.note_ports = midi::kPortDin | midi::kPortUsb;
  return c;
}
midi::MidiConfig slave_cfg(midi::Port in_port, std::uint8_t out_ports = 0) {
  midi::MidiConfig c;
  c.mode = midi::SyncMode::Slave;
  c.in_port = in_port;
  c.out_ports = out_ports;
  return c;
}

double sample_of_cycles(double cycles) { return cycles / kCycD; }

// The first Step record at or after sample `from`, and the n-th after it: returns the sample, or -1.
double step_time(const Collector& c, std::uint32_t idx) {
  const Rec* r = c.nth(EventKind::Step, idx);
  return r != nullptr ? static_cast<double>(r->t) : -1.0;
}

}  // namespace

// ---- Master ---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi link master: Start, then a clock every 20.83 ms on the wire, the notes after the Start, on DIN and USB") {
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin | midi::kPortUsb));
  g_rig.s->play_pattern(1);
  g_rig.run_to(100000);  // 2.27 s: a little more than one pattern (2 s)

  // DIN: the Start byte is the first byte, ahead of every note
  NEED(g_wires.din_n > 20u);
  CHECK(g_wires.din[0] == midi::kStart);
  const std::uint32_t first_note = g_wires.find_din(0x92);
  NEED(first_note < g_wires.din_n);
  CHECK(g_wires.din_at[0] < g_wires.din_at[first_note]);
  CHECK(g_wires.din[first_note + 1] == 40);  // the pattern's first note
  // clocks: 24 a beat, a beat is 0.5 s at 120 BPM: 4 beats and a bit in 2.27 s
  const std::uint32_t clocks = g_wires.count_din(midi::kClock);
  CHECK(clocks >= 107u);  // the first at the first step (a block after the play), then every 919.1 samples: 109 in 100,000 samples, the last block's not yet heard
  CHECK(clocks <= 110u);
  // each clock is one period after the one before it; a note's bytes may hold one up by at most a byte time, and the next one is back on the grid
  std::uint64_t prev = 0;
  std::uint32_t seen = 0;
  std::uint32_t exact = 0;
  std::uint64_t first_clock = 0;
  for (std::uint32_t i = 0; i < g_wires.din_n; ++i) {
    if (g_wires.din[i] != midi::kClock) {
      continue;
    }
    if (seen == 0u) {
      first_clock = g_wires.din_at[i];
    }
    const std::uint64_t ideal = first_clock + static_cast<std::uint64_t>(seen) * kClockPeriod;
    const std::int64_t err = static_cast<std::int64_t>(g_wires.din_at[i]) - static_cast<std::int64_t>(ideal);
    CHECK(err >= -2);                                // never early (to a cycle's rounding)
    CHECK(err <= static_cast<std::int64_t>(kByte));  // at worst one byte in flight
    exact += err <= 2 ? 1u : 0u;
    (void)prev;
    prev = g_wires.din_at[i];
    ++seen;
  }
  CHECK(exact * 10u >= seen * 9u);  // nearly all of them are on the grid to the cycle: the wire is mostly idle
  // the clock follows the Start by less than a clock period; the first note is on the first microstep: the Start leads it by the start lead (one block)
  const std::uint32_t first_clock_idx = g_wires.find_din(midi::kClock);
  NEED(first_clock_idx < g_wires.din_n);
  CHECK(g_wires.din_at[first_clock_idx] - g_wires.din_at[0] <= 2 * 64 * kCyc);

  // USB: the same stream in event words: Start, clocks, the note-on of channel 2, none earlier than due, none later than a frame
  CHECK(g_wires.count_usb_byte0(midi::kStart) == 1u);
  CHECK(g_wires.count_usb_byte0(midi::kClock) + 3u >= clocks);  // a frame carries a clock at or after its time: the last few are in frames that have not come yet
  CHECK(g_wires.count_usb_byte0(midi::kClock) <= clocks);
  CHECK(g_wires.count_usb_byte0(0x92) >= 16u);
  NEED(g_wires.usb_n > 20u);
  CHECK(g_wires.usb[0] == midi::usb_event(0, midi::msg1(midi::kStart)));
  CHECK(g_kicks > 20u);  // the render pended the DIN emitter for every block that handed it something
}

TEST_CASE("midi link master: only the ports enabled get the clock, Off sends none; notes go to the notes' ports either way") {
  // USB only
  midi_project();
  g_rig.init(master_cfg(midi::kPortUsb));
  g_rig.s->play_pattern(1);
  g_rig.run_to(40000);
  CHECK(g_wires.count_din(midi::kClock) == 0u);
  CHECK(g_wires.count_din(midi::kStart) == 0u);
  CHECK(g_wires.count_din(0x92) > 0u);
  CHECK(g_wires.count_usb_byte0(midi::kClock) > 20u);
  CHECK(g_wires.count_usb_byte0(midi::kStart) == 1u);
  // DIN only
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin));
  g_rig.s->play_pattern(1);
  g_rig.run_to(40000);
  CHECK(g_wires.count_usb_byte0(midi::kClock) == 0u);
  CHECK(g_wires.count_din(midi::kClock) > 20u);
  // Off: no clock, no Start, the notes still go out
  midi_project();
  midi::MidiConfig off;
  off.mode = midi::SyncMode::Off;
  off.out_ports = midi::kPortDin | midi::kPortUsb;  // set, but Off wins
  g_rig.init(off);
  g_rig.s->play_pattern(1);
  g_rig.run_to(40000);
  CHECK(g_wires.count_din(midi::kClock) == 0u);
  CHECK(g_wires.count_din(midi::kStart) == 0u);
  CHECK(g_wires.count_usb_byte0(midi::kClock) == 0u);
  CHECK(g_wires.count_din(0x92) > 0u);
  // the notes' ports can be moved: USB only
  midi_project();
  midi::MidiConfig n = master_cfg(midi::kPortDin);
  n.note_ports = midi::kPortUsb;
  g_rig.init(n);
  g_rig.s->play_pattern(1);
  g_rig.run_to(40000);
  CHECK(g_wires.count_din(0x92) == 0u);
  CHECK(g_wires.count_usb_byte0(0x92) > 0u);
}

TEST_CASE("midi link master: a play from the middle sends SPP and Continue, not Start; Stop goes out with the offs of the notes that were open") {
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin));
  g_rig.s->play_pattern(1, 8);
  g_rig.run_to(20000);
  NEED(g_wires.din_n > 6u);
  CHECK(g_wires.count_din(midi::kStart) == 0u);
  const std::uint32_t spp = g_wires.find_din(midi::kSongPosition);
  NEED(spp + 3u < g_wires.din_n);
  CHECK(g_wires.din[spp + 1] == 8);  // 8 MIDI beats = 8 steps, low 7 bits first
  CHECK(g_wires.din[spp + 2] == 0);
  CHECK(g_wires.din[spp + 3] == midi::kContinue);
  CHECK(spp < g_wires.find_din(0x92));  // the position goes before the first note
  // stop in the middle of a note: the sequencer's own off, then Stop; no note is left hanging
  g_rig.s->stop();
  g_rig.run_to(26000);
  const std::uint32_t ons = g_wires.count_din(0x92);
  const std::uint32_t offs = g_wires.count_din(0x82);
  CHECK(ons >= 1u);
  CHECK(offs == ons);
  CHECK(g_wires.count_din(midi::kStop) == 1u);
  CHECK(g_rig.link->out().book().owed() == 0u);
  const std::uint32_t before = g_wires.din_n;
  g_rig.run_to(60000);
  CHECK(g_wires.din_n == before);  // the wire is quiet after a stop: no clock, no note
}

TEST_CASE("midi link master: the OUT OFFSET delays everything on that port by its exact length, the other port does not move") {
  std::uint64_t base_din = 0;
  std::uint64_t base_usb = 0;
  for (const int dms_i : {0, 25, -10}) {
    const std::int16_t dms = static_cast<std::int16_t>(dms_i);
    midi_project();
    midi::MidiConfig c = master_cfg(midi::kPortDin | midi::kPortUsb);
    c.out_offset_dms[0] = dms;  // DIN only
    g_rig.init(c);
    g_rig.s->play_pattern(1);
    g_rig.run_to(30000);
    const std::uint32_t clk = g_wires.find_din(midi::kClock, 3);
    NEED(clk < g_wires.din_n);
    NEED(g_wires.usb_n > 6u);
    // the USB frame that carries the fourth clock
    std::uint32_t k = 0;
    std::uint64_t usb_at = 0;
    std::uint32_t seen = 0;
    for (k = 0; k < g_wires.usb_n; ++k) {
      if (static_cast<std::uint8_t>((g_wires.usb[k] >> 8) & 0xFFu) == midi::kClock && seen++ == 3u) {
        usb_at = g_wires.usb_at[k];
        break;
      }
    }
    if (dms == 0) {
      base_din = g_wires.din_at[clk];
      base_usb = usb_at;
    } else {
      CHECK(static_cast<std::int64_t>(g_wires.din_at[clk]) - static_cast<std::int64_t>(base_din) == static_cast<std::int64_t>(dms) * 18000);
      CHECK(usb_at == base_usb);
    }
  }
}

// ---- Slave ------------------------------------------------------------------------------------------------------------------------------------

namespace {
// A slave rig at 90 BPM project tempo (the master is at 120) playing the master's Start at clock `c_start`.
void slave_start(double start_before_clock_cycles = 540000.0, std::uint32_t c_start = 40) {
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  g_rig.send(g_rig.m.time_of(c_start) - start_before_clock_cycles, midi::kStart);
}
}  // namespace

TEST_CASE("midi link slave: the loop locks, the Start moves the transport, the steps sit on the master's clock, the tempo is the master's") {
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Din));
  const std::uint32_t c_start = 40;
  slave_start(540000.0, c_start);
  g_rig.run_to(400000);
  NEED(g_col.count(EventKind::Step) >= 20u);
  CHECK(g_rig.s->following());
  CHECK(g_rig.s->playing());
  // step k at the master's clock c_start + 6 k: the first is late by what a block and a byte add, the others are exact to a sample's rounding
  for (std::uint32_t k = 0; k < 20; ++k) {
    const double want = sample_of_cycles(g_rig.m.time_of(c_start + 6u * k));
    const double err = step_time(g_col, k) - want;
    if (k == 0) {
      CHECK(err >= -1.5);
      CHECK(err <= 64.0 + 16.0);
    } else {
      CHECK_MESSAGE(std::fabs(err) <= 1.5, k);
    }
  }
  const midi::SyncStatus st = g_rig.link->status();
  CHECK(st.following);
  CHECK(st.clock_present);
  CHECK(st.locked);
  CHECK(st.bpm_x100 == 12000u);
  CHECK(st.clocks > 100u);
  CHECK(st.transport_events == 1u);
  CHECK(g_rig.s->tempo_x100() == 12000u);
  CHECK(g_rig.link->stats().stale_releases == 0u);
}

TEST_CASE("midi link slave: Stop stops, SPP then Continue resumes at that step, the clock out is the relay of the followed clock") {
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Din, midi::kPortDin));
  const std::uint32_t c_start = 40;
  slave_start(540000.0, c_start);
  const double stop_at = g_rig.m.time_of(c_start + 6 * 5 + 3);
  const double spp_at = g_rig.m.time_of(c_start + 6 * 5 + 9);
  const double cont_at = g_rig.m.time_of(c_start + 6 * 5 + 11) - 1.0e6;
  g_rig.send(stop_at, midi::kStop);
  g_rig.send(spp_at, midi::kSongPosition);
  g_rig.send(spp_at + 60000.0, 9);
  g_rig.send(spp_at + 120000.0, 0);
  g_rig.send(cont_at, midi::kContinue);
  g_rig.run_to(160000);  // the Start is at 0.85 s, the Stop at 1.5 s, the Continue at 1.75 s: 3.6 s
  // after the Stop the transport stops (the relayed Stop on DIN too), then the Continue plays on from step 9
  CHECK(g_wires.count_din(midi::kStop) >= 1u);
  CHECK(g_wires.count_din(midi::kContinue) == 1u);
  CHECK(g_wires.find_din(midi::kSongPosition) < g_wires.din_n);
  NEED(g_col.count(EventKind::Stopped) >= 1u);
  const std::uint32_t steps = g_col.count(EventKind::Step);
  NEED(steps > 6u);
  // the first step after the Continue is step 9 (value), on the master's clock grid
  bool found = false;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::Stopped) {
      for (std::uint32_t j = i; j < g_col.n; ++j) {
        if (g_col.rec[j].ev.kind == EventKind::Step) {
          CHECK(g_col.rec[j].ev.value == 9);
          found = true;
          break;
        }
      }
      break;
    }
  }
  CHECK(found);
  // the relayed clock is on the followed grid: spacing one master period, to a few cycles, while it plays
  std::uint32_t seen = 0;
  std::uint64_t prev = 0;
  std::uint32_t good = 0;
  for (std::uint32_t i = 0; i < g_wires.din_n; ++i) {
    if (g_wires.din[i] != midi::kClock) {
      continue;
    }
    if (seen != 0u && g_wires.din_at[i] - prev <= kClockPeriod + 4000u && g_wires.din_at[i] - prev + 4000u >= kClockPeriod) {
      ++good;
    }
    prev = g_wires.din_at[i];
    ++seen;
  }
  CHECK(seen > 20u);
  CHECK(good * 10u >= seen * 8u);
}

// OS-MIDISYNC M2: a master can send Stop, SPP and Continue back to back (the Stop and the SPP are events at once, the Continue fires on the next
// clock), so the Stop and the SPP are applied in the same block, while the run has not ended yet. MIDI 1.0: the SPP is sent while stopped and the
// Continue that follows resumes at that position, not where the run stopped.
TEST_CASE("midi link slave: Stop + SPP in one block, then Continue resumes at the SPP step (Pattern 8, Song 20); Start from 0; a bare Continue where it stopped") {
  // The first Step record after the first Stopped record: its step value and order position (b), or false when there is none.
  const auto first_step_after_stop = [](std::int32_t& value, std::uint8_t& order) -> bool {
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      if (g_col.rec[i].ev.kind == EventKind::Stopped) {
        for (std::uint32_t j = i + 1; j < g_col.n; ++j) {
          if (g_col.rec[j].ev.kind == EventKind::Step) {
            value = g_col.rec[j].ev.value;
            order = g_col.rec[j].ev.b;
            return true;
          }
        }
        return false;
      }
    }
    return false;
  };
  // The last Step record before the first Stopped record: the step the run was on when the Stop came.
  const auto last_step_before_stop = []() -> std::int32_t {
    for (std::uint32_t i = g_col.n; i-- > 0u;) {
      if (g_col.rec[i].ev.kind == EventKind::Stopped) {
        for (std::uint32_t j = i; j-- > 0u;) {
          if (g_col.rec[j].ev.kind == EventKind::Step) {
            return g_col.rec[j].ev.value;
          }
        }
        return -1;
      }
    }
    return -1;
  };
  // Sends `bytes` in this order, all stamped into the block that starts at sample `t` (feed_master pushes them in that block's drain): each one
  // a little earlier than the next, all before the block's start, so the order survives and they land in one drain.
  const auto send_in_block = [](std::uint64_t t, std::initializer_list<std::uint8_t> bytes) {
    const double base = static_cast<double>(t) * kCycD - 57601.0;
    const double n = static_cast<double>(bytes.size());
    std::uint32_t k = 0;
    for (std::uint8_t b : bytes) {
      g_rig.send(base - (n - static_cast<double>(k)) * 1000.0, b);
      ++k;
    }
  };

  // Pattern mode, SPP 8: the Continue run resumes at step 8
  {
    midi_project(9000);
    g_rig.init(slave_cfg(midi::Port::Din, midi::kPortDin));
    const std::uint32_t c_start = 40;
    slave_start(540000.0, c_start);
    bool sent = false;
    g_rig.run_to(220000, [&](std::uint64_t t) {
      if (!sent && t >= 100000) {
        sent = true;
        send_in_block(t, {midi::kStop, midi::kSongPosition, 8, 0, midi::kContinue});
      }
    });
    NEED(g_col.count(EventKind::Stopped) >= 1u);
    std::int32_t value = -1;
    std::uint8_t order = 0xFF;
    NEED(first_step_after_stop(value, order));
    CHECK(value == 8);
    CHECK(g_rig.s->playing());
  }

  // Song mode (48 steps in three slots), SPP 20: the Continue run resumes at song step 20 = order 1, step 4
  {
    song_project(9000);
    g_rig.init(slave_cfg(midi::Port::Din, midi::kPortDin));
    g_rig.s->select(seq::PlayMode::Song, 1);
    g_rig.send(g_rig.m.time_of(40) - 540000.0, midi::kStart);
    bool sent = false;
    g_rig.run_to(220000, [&](std::uint64_t t) {
      if (!sent && t >= 100000) {
        sent = true;
        send_in_block(t, {midi::kStop, midi::kSongPosition, 20, 0, midi::kContinue});
      }
    });
    NEED(g_col.count(EventKind::Stopped) >= 1u);
    std::int32_t value = -1;
    std::uint8_t order = 0xFF;
    NEED(first_step_after_stop(value, order));
    CHECK(order == 1u);
    CHECK(value == 4);  // 16 steps of slot 0 + 4 = song step 20
    CHECK(g_rig.s->playing());
  }

  // A Start after the Stop begins at step 0
  {
    midi_project(9000);
    g_rig.init(slave_cfg(midi::Port::Din, midi::kPortDin));
    const std::uint32_t c_start = 40;
    slave_start(540000.0, c_start);
    bool sent = false;
    g_rig.run_to(220000, [&](std::uint64_t t) {
      if (!sent && t >= 100000) {
        sent = true;
        send_in_block(t, {midi::kStop, midi::kStart});
      }
    });
    NEED(g_col.count(EventKind::Stopped) >= 1u);
    std::int32_t value = -1;
    std::uint8_t order = 0xFF;
    NEED(first_step_after_stop(value, order));
    CHECK(value == 0);
  }

  // A bare Continue (no SPP) resumes where the run stopped
  {
    midi_project(9000);
    g_rig.init(slave_cfg(midi::Port::Din, midi::kPortDin));
    const std::uint32_t c_start = 40;
    slave_start(540000.0, c_start);
    bool sent = false;
    g_rig.run_to(220000, [&](std::uint64_t t) {
      if (!sent && t >= 100000) {
        sent = true;
        send_in_block(t, {midi::kStop, midi::kContinue});
      }
    });
    NEED(g_col.count(EventKind::Stopped) >= 1u);
    const std::int32_t stopped_at = last_step_before_stop();
    NEED(stopped_at >= 0);
    std::int32_t value = -1;
    std::uint8_t order = 0xFF;
    NEED(first_step_after_stop(value, order));
    CHECK(value == stopped_at);
  }
}

// OS-MIDISYNC M2b: on the unit the master's bytes arrive as USB-MIDI event packets, not as raw bytes. The Linux host driver (sound/usb/midi.c,
// snd_usbmidi_transmit_byte) packs every real-time byte (clock, Start, Continue, Stop) as a CIN 0xF single-byte event and the Song Position
// Pointer as CIN 0x3 (three-byte system common): [0x03, 0xF2, lsb, msb]. This feeds exactly those event words through the firmware's own
// intake — usb::midi_packet_to_ring (the bulk OUT token's handler, usb/device.h token_midi_out) then midi::usb_midi_from_host (the USB
// interrupt's pass, platform/mk66/usb/src/usb0.cpp usb_isr) into the USB port's midiRt ring — and steps the link block by block.
TEST_CASE("midi link slave over USB event packets (CIN framing): SPP then Continue resumes at the SPP step (Pattern 8, Song 20); Stop+SPP+Continue in one block") {
  usb::MidiRing from_host;
  midi::UsbBindStats bst;
  struct Pend {
    double t;
    std::uint32_t word;
  };
  Pend pend[460];
  std::uint32_t pend_n = 0;
  const auto word = [](std::uint8_t cin, std::uint8_t b0, std::uint8_t b1, std::uint8_t b2) {
    return static_cast<std::uint32_t>(cin) | (static_cast<std::uint32_t>(b0) << 8) | (static_cast<std::uint32_t>(b1) << 16) |
           (static_cast<std::uint32_t>(b2) << 24);
  };
  const auto rt = [&word](std::uint8_t b) { return word(0x0Fu, b, 0, 0); };  // CIN 0xF: the kernel's packet for a real-time byte
  const auto sppw = [&word](std::uint16_t v) {
    return word(0x3u, midi::kSongPosition, static_cast<std::uint8_t>(v & 0x7Fu), static_cast<std::uint8_t>((v >> 7) & 0x7Fu));  // CIN 0x3
  };
  // The master's pending event words, fed in time order through the two firmware functions, stamped as the interrupt would.
  const auto feed = [&](double now_cycles) {
    for (;;) {
      std::uint32_t best = pend_n;
      for (std::uint32_t i = 0; i < pend_n; ++i) {
        if (best == pend_n || pend[i].t < pend[best].t) {
          best = i;
        }
      }
      if (best == pend_n || pend[best].t > now_cycles) {
        break;
      }
      alignas(4) std::uint8_t pkt[4];
      std::memcpy(pkt, &pend[best].word, 4);
      const usb::MidiPacketResult r = usb::midi_packet_to_ring(from_host, pkt, 4);
      CHECK(r.accepted == 1u);
      (void)midi::usb_midi_from_host(from_host, g_rig.link->usb_in(), static_cast<std::uint32_t>(static_cast<std::uint64_t>(pend[best].t)), bst);
      pend[best] = pend[pend_n - 1u];
      --pend_n;
    }
  };
  // Start 1.3 clocks before clock c0, then one clock per period (all CIN 0xF), then the extra transport words at their times.
  const auto plan = [&](std::uint32_t c0, std::uint32_t c_count, std::initializer_list<Pend> tr) {
    pend_n = 0;
    pend[pend_n++] = {g_rig.m.time_of(c0) - 540000.0, rt(midi::kStart)};
    for (std::uint32_t c = 0; c < c_count; ++c) {
      pend[pend_n++] = {g_rig.m.time_of(c), rt(midi::kClock)};
    }
    for (const Pend& p : tr) {
      pend[pend_n++] = p;
    }
  };
  // The first Step record at or after sample `from`: its value and order, or false when there is none.
  const auto first_step_from = [](double from, std::int32_t& value, std::uint8_t& order) -> bool {
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      if (g_col.rec[i].ev.kind == EventKind::Step && static_cast<double>(g_col.rec[i].t) >= from) {
        value = g_col.rec[i].ev.value;
        order = g_col.rec[i].ev.b;
        return true;
      }
    }
    return false;
  };

  // Unit test 10 (Pattern, "start,120:10,spp:8,cont,120:10"): the SPP arrives while playing, the Continue right behind it: the first step
  // of the run after the Continue is step 8.
  {
    midi_project(9000);
    g_rig.init(slave_cfg(midi::Port::Usb));
    g_rig.m.quiet = true;  // the master is on USB only: no DIN clocks
    const std::uint32_t c0 = 40;
    const double spp_t = g_rig.m.time_of(c0 + 63) + 1000000.0;  // mid step 10 (the boundary is at clock c0+60, the next at c0+66)
    const double cont_t = spp_t + 50000.0;
    plan(c0, 420, {{spp_t, sppw(8)}, {cont_t, rt(midi::kContinue)}});
    double cont_block = -1.0;
    g_rig.run_to(200000, [&](std::uint64_t t) {
      const double now = static_cast<double>(t) * kCycD;
      feed(now);
      if (cont_block < 0.0 && now >= cont_t) {
        cont_block = static_cast<double>(t);
      }
    });
    NEED(cont_block > 0.0);
    CHECK(g_rig.link->status().transport_events >= 3u);  // the Start, the SPP and the Continue were all applied
    std::int32_t value = -1;
    std::uint8_t order = 0xFF;
    NEED(first_step_from(cont_block, value, order));
    CHECK(value == 8);
  }

  // Unit test 11 (Song, "start,120:10,spp:20,cont,120:10"): SPP 20 = order 1, step 4.
  {
    song_project(9000);
    g_rig.init(slave_cfg(midi::Port::Usb));
    g_rig.s->select(seq::PlayMode::Song, 1);
    g_rig.m.quiet = true;
    const std::uint32_t c0 = 40;
    const double spp_t = g_rig.m.time_of(c0 + 63) + 1000000.0;
    const double cont_t = spp_t + 50000.0;
    plan(c0, 420, {{spp_t, sppw(20)}, {cont_t, rt(midi::kContinue)}});
    double cont_block = -1.0;
    g_rig.run_to(200000, [&](std::uint64_t t) {
      const double now = static_cast<double>(t) * kCycD;
      feed(now);
      if (cont_block < 0.0 && now >= cont_t) {
        cont_block = static_cast<double>(t);
      }
    });
    NEED(cont_block > 0.0);
    std::int32_t value = -1;
    std::uint8_t order = 0xFF;
    NEED(first_step_from(cont_block, value, order));
    CHECK(order == 1u);
    CHECK(value == 4);  // 16 steps of slot 0 + 4 = song step 20
  }

  // Stop + SPP + Continue back to back as packets (all three words in one block's drain), then the Continue fires on the next clock:
  // the resume is at the SPP step, not where the run stopped.
  {
    midi_project(9000);
    g_rig.init(slave_cfg(midi::Port::Usb));
    g_rig.m.quiet = true;
    const std::uint32_t c0 = 40;
    const double t_all = g_rig.m.time_of(c0 + 63) + 1000000.0;
    plan(c0, 420, {{t_all, rt(midi::kStop)}, {t_all + 20000.0, sppw(8)}, {t_all + 40000.0, rt(midi::kContinue)}});
    g_rig.run_to(200000, [&](std::uint64_t t) { feed(static_cast<double>(t) * kCycD); });
    NEED(g_col.count(EventKind::Stopped) >= 1u);
    std::int32_t value = -1;
    std::uint8_t order = 0xFF;
    // the first Step after the first Stopped record
    bool found = false;
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      if (g_col.rec[i].ev.kind == EventKind::Stopped) {
        for (std::uint32_t j = i + 1; j < g_col.n; ++j) {
          if (g_col.rec[j].ev.kind == EventKind::Step) {
            value = g_col.rec[j].ev.value;
            order = g_col.rec[j].ev.b;
            found = true;
            break;
          }
        }
        break;
      }
    }
    NEED(found);
    CHECK(value == 8);
    CHECK(g_rig.s->playing());
    (void)order;
  }

  // OS-MIDISYNC M2b, the unit's wire format: the SPP packed as CIN 0x2 (two-byte system common) instead of CIN 0x3. The packet decoder
  // must still give the parser both data bytes of F2 (usb_event_bytes takes 3 for CIN 0x2 + F2); with only two the parser waits for the
  // second data byte forever — every byte after it here is real-time and passes through untouched — no Spp event is ever emitted and
  // the SPP is ignored, exactly what the unit did (tests 10/11: first step where the run stopped, clock/Start/Stop/Continue all fine).
  {
    midi_project(9000);
    g_rig.init(slave_cfg(midi::Port::Usb));
    g_rig.m.quiet = true;
    const std::uint32_t c0 = 40;
    const double spp_t = g_rig.m.time_of(c0 + 63) + 1000000.0;
    const double cont_t = spp_t + 50000.0;
    plan(c0, 420, {{spp_t, word(0x2u, midi::kSongPosition, 8, 0)}, {cont_t, rt(midi::kContinue)}});  // CIN 0x2
    double cont_block = -1.0;
    g_rig.run_to(200000, [&](std::uint64_t t) {
      const double now = static_cast<double>(t) * kCycD;
      feed(now);
      if (cont_block < 0.0 && now >= cont_t) {
        cont_block = static_cast<double>(t);
      }
    });
    NEED(cont_block > 0.0);
    CHECK(g_rig.link->status().transport_events >= 3u);  // the SPP must be an event too
    std::int32_t value = -1;
    std::uint8_t order = 0xFF;
    NEED(first_step_from(cont_block, value, order));
    CHECK(value == 8);
    (void)order;
  }
}

// OS-MIDISYNC M2c: the unit's real test 10/11 sequence (MIDISYNC.md, midi_clock_tool.py build_plan() with the default lead_ms=2.0):
// Start, then 10 s of clocks at 120 BPM (480 clocks = 80 sixteenths), then — with NO Stop, while the slave is PLAYING — the SPP and
// the Continue stamped before the next clock (build_plan's flush(): the SPP 2*lead = 4 ms before it, the Continue lead = 2 ms before it;
// that next clock, 480 = 80 x 6, is a step boundary), and the clocks go on. The first Step after the Continue must be the SPP step: 8 in
// Pattern, song step 20 (order 1, step 4) in Song. On the unit the first step heard was 0 / 32 (= 80 mod 16 / 80 mod 48): the SPP was
// ignored while playing. Same intake as the case above: CIN-framed USB event packets through usb::midi_packet_to_ring and
// midi::usb_midi_from_host.
TEST_CASE("midi link slave over USB event packets: the unit's 10/11 sequence - Start, 480 clocks, then SPP + Continue while playing, 48 clocks") {
  usb::MidiRing from_host;
  midi::UsbBindStats bst;
  struct Pend {
    double t;
    std::uint32_t word;
  };
  Pend pend[560];
  std::uint32_t pend_n = 0;
  const auto word = [](std::uint8_t cin, std::uint8_t b0, std::uint8_t b1, std::uint8_t b2) {
    return static_cast<std::uint32_t>(cin) | (static_cast<std::uint32_t>(b0) << 8) | (static_cast<std::uint32_t>(b1) << 16) |
           (static_cast<std::uint32_t>(b2) << 24);
  };
  const auto rt = [&word](std::uint8_t b) { return word(0x0Fu, b, 0, 0); };  // CIN 0xF: the kernel's packet for a real-time byte
  const auto sppw = [&word](std::uint16_t v) {
    return word(0x3u, midi::kSongPosition, static_cast<std::uint8_t>(v & 0x7Fu), static_cast<std::uint8_t>((v >> 7) & 0x7Fu));  // CIN 0x3
  };
  // The master's pending event words, fed in time order through the two firmware functions, stamped as the interrupt would.
  const auto feed = [&](double now_cycles) {
    for (;;) {
      std::uint32_t best = pend_n;
      for (std::uint32_t i = 0; i < pend_n; ++i) {
        if (best == pend_n || pend[i].t < pend[best].t) {
          best = i;
        }
      }
      if (best == pend_n || pend[best].t > now_cycles) {
        break;
      }
      alignas(4) std::uint8_t pkt[4];
      std::memcpy(pkt, &pend[best].word, 4);
      const usb::MidiPacketResult r = usb::midi_packet_to_ring(from_host, pkt, 4);
      CHECK(r.accepted == 1u);
      (void)midi::usb_midi_from_host(from_host, g_rig.link->usb_in(), static_cast<std::uint32_t>(static_cast<std::uint64_t>(pend[best].t)), bst);
      pend[best] = pend[pend_n - 1u];
      --pend_n;
    }
  };
  // build_plan()'s plan: the Start before clock 0 (it fires on clock 0, the run begins there), `c_count` clocks, then the SPP and the
  // Continue stamped before the next clock (flush() with lead = 2 ms = 360000 cycles: the SPP at 2*lead, the Continue at lead before it),
  // then `after` more clocks.
  const auto plan = [&](std::uint32_t c_count, std::uint16_t spp, std::uint32_t after) {
    pend_n = 0;
    pend[pend_n++] = {g_rig.m.time_of(0) - 540000.0, rt(midi::kStart)};
    for (std::uint32_t c = 0; c < c_count; ++c) {
      pend[pend_n++] = {g_rig.m.time_of(c), rt(midi::kClock)};
    }
    const double t_next = g_rig.m.time_of(c_count);  // the next clock: c_count = 480 = 80 x 6, a step boundary
    pend[pend_n++] = {t_next - 720000.0, sppw(spp)};
    pend[pend_n++] = {t_next - 360000.0, rt(midi::kContinue)};
    for (std::uint32_t c = c_count; c < c_count + after; ++c) {
      pend[pend_n++] = {g_rig.m.time_of(c), rt(midi::kClock)};
    }
  };
  // The first Step record at or after sample `from`: its value and order, or false when there is none.
  const auto first_step_from = [](double from, std::int32_t& value, std::uint8_t& order) -> bool {
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      if (g_col.rec[i].ev.kind == EventKind::Step && static_cast<double>(g_col.rec[i].t) >= from) {
        value = g_col.rec[i].ev.value;
        order = g_col.rec[i].ev.b;
        return true;
      }
    }
    return false;
  };
  const auto steps_before = [](double from) {
    std::uint32_t c = 0;
    for (std::uint32_t i = 0; i < g_col.n; ++i) {
      if (g_col.rec[i].ev.kind == EventKind::Step && static_cast<double>(g_col.rec[i].t) < from) {
        ++c;
      }
    }
    return c;
  };

  // Unit test 10 (Pattern, "start,120:10,spp:8,cont,120:10"): no Stop, the slave is playing when the SPP and the Continue arrive.
  {
    midi_project(9000);
    g_rig.init(slave_cfg(midi::Port::Usb));
    g_rig.s->select(seq::PlayMode::Pattern, 1);  // pattern 1, 16 steps: the cycle of the unit's test 10
    g_rig.m.quiet = true;  // the master is on USB only: no DIN clocks
    plan(480, 8, 48);
    const double cont_t = g_rig.m.time_of(480) - 360000.0;
    double cont_block = -1.0;
    g_rig.run_to(500000, [&](std::uint64_t t) {
      const double now = static_cast<double>(t) * kCycD;
      feed(now);
      if (cont_block < 0.0 && now >= cont_t) {
        cont_block = static_cast<double>(t);
      }
    });
    NEED(cont_block > 0.0);
    CHECK(g_rig.link->status().transport_events >= 3u);  // the Start, the SPP and the Continue were all applied
    CHECK(g_rig.s->playing());
    CHECK(steps_before(cont_block) == 80u);  // 480 clocks = 80 sixteenths ran before the Continue
    std::int32_t value = -1;
    std::uint8_t order = 0xFF;
    NEED(first_step_from(cont_block, value, order));
    CHECK(value == 8);  // the first step after the Continue is the SPP step, not 80 mod 16 = 0
    (void)order;
  }

  // Unit test 11 (Song, "start,120:10,spp:20,cont,120:10" --cycle 48): SPP 20 = order 1, step 4.
  {
    song_project(9000);
    g_rig.init(slave_cfg(midi::Port::Usb));
    g_rig.s->select(seq::PlayMode::Song, 1);
    g_rig.m.quiet = true;
    plan(480, 20, 48);
    const double cont_t = g_rig.m.time_of(480) - 360000.0;
    double cont_block = -1.0;
    g_rig.run_to(500000, [&](std::uint64_t t) {
      const double now = static_cast<double>(t) * kCycD;
      feed(now);
      if (cont_block < 0.0 && now >= cont_t) {
        cont_block = static_cast<double>(t);
      }
    });
    NEED(cont_block > 0.0);
    CHECK(g_rig.link->status().transport_events >= 3u);
    CHECK(g_rig.s->playing());
    CHECK(steps_before(cont_block) == 80u);
    std::int32_t value = -1;
    std::uint8_t order = 0xFF;
    NEED(first_step_from(cont_block, value, order));
    CHECK(order == 1u);
    CHECK(value == 4);  // 16 steps of slot 0 + 4 = song step 20, not 80 mod 48 = 32
  }
}

// OS-MIDISYNC M2b, full device level: the same packets through the whole USB device driver — the fake controller's bulk OUT token runs
// UsbDevice::isr() -> on_token -> token_midi_out -> usb::midi_packet_to_ring into the from-host ring — and then the interrupt's other half,
// midi::usb_midi_from_host, expands the words into the USB midiRt ring, exactly as platform/mk66/usb/src/usb0.cpp usb_isr() does.
TEST_CASE("midi link slave over the full USB device (bulk OUT tokens with CIN-framed packets): SPP then Continue resumes at the SPP step") {
  usb_rig::Rig& ur = usb_rig::fresh();
  USB_REQUIRE(ur.enumerate());
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Usb));
  g_rig.m.quiet = true;  // the master is on USB only
  midi::UsbBindStats bst;
  struct Pend {
    double t;
    std::uint8_t pkt[4];
  };
  Pend pend[460];
  std::uint32_t pend_n = 0;
  const auto rt_pkt = [](std::uint8_t b) { return Pend{0.0, {0x0Fu, b, 0, 0}}; };  // CIN 0xF: the kernel's packet for a real-time byte
  const auto spp_pkt = [](std::uint16_t v) {
    return Pend{0.0, {0x03u, midi::kSongPosition, static_cast<std::uint8_t>(v & 0x7Fu), static_cast<std::uint8_t>((v >> 7) & 0x7Fu)}};  // CIN 0x3
  };
  const auto feed = [&](double now_cycles) {
    for (;;) {
      std::uint32_t best = pend_n;
      for (std::uint32_t i = 0; i < pend_n; ++i) {
        if (best == pend_n || pend[i].t < pend[best].t) {
          best = i;
        }
      }
      if (best == pend_n || pend[best].t > now_cycles) {
        break;
      }
      // The host's bulk OUT packet: the fake controller writes the BD buffer and runs the device's interrupt, which decodes the packet
      // into the from-host ring (token_midi_out); then the interrupt's second half expands the words into stamped bytes.
      CHECK(ur.host.bulk_out(3, pend[best].pkt, 4) == usb_rig::Reply::Ack);
      (void)midi::usb_midi_from_host(ur.midi_out, g_rig.link->usb_in(), static_cast<std::uint32_t>(static_cast<std::uint64_t>(pend[best].t)), bst);
      pend[best] = pend[pend_n - 1u];
      --pend_n;
    }
  };
  const std::uint32_t c0 = 40;
  const double spp_t = g_rig.m.time_of(c0 + 63) + 1000000.0;
  const double cont_t = spp_t + 50000.0;
  pend[pend_n++] = {g_rig.m.time_of(c0) - 540000.0, {0x0Fu, midi::kStart, 0, 0}};
  for (std::uint32_t c = 0; c < 130; ++c) {
    pend[pend_n++] = {g_rig.m.time_of(c), {0x0Fu, midi::kClock, 0, 0}};
  }
  {
    const Pend p = spp_pkt(8);
    pend[pend_n++] = {spp_t, {p.pkt[0], p.pkt[1], p.pkt[2], p.pkt[3]}};
  }
  {
    const Pend p = rt_pkt(midi::kContinue);
    pend[pend_n++] = {cont_t, {p.pkt[0], p.pkt[1], p.pkt[2], p.pkt[3]}};
  }
  double cont_block = -1.0;
  g_rig.run_to(200000, [&](std::uint64_t t) {
    const double now = static_cast<double>(t) * kCycD;
    feed(now);
    if (cont_block < 0.0 && now >= cont_t) {
      cont_block = static_cast<double>(t);
    }
  });
  NEED(cont_block > 0.0);
  CHECK(g_rig.link->status().transport_events >= 3u);  // the Start, the SPP and the Continue were all applied
  CHECK(ur.dev.counters().midi_out_events >= 132u);    // every packet's event was accepted by the packet decoder
  std::int32_t value = -1;
  std::uint8_t order = 0xFF;
  bool found = false;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::Step && static_cast<double>(g_col.rec[i].t) >= cont_block) {
      value = g_col.rec[i].ev.value;
      order = g_col.rec[i].ev.b;
      found = true;
      break;
    }
  }
  NEED(found);
  CHECK(value == 8);
  (void)order;
}

TEST_CASE("midi link slave: the IN OFFSET shifts the steps by its exact length; the followed port is the only one that counts") {
  for (const int dms_i : {20, 50}) {
    const std::int16_t dms = static_cast<std::int16_t>(dms_i);
    midi_project(9000);
    midi::MidiConfig c = slave_cfg(midi::Port::Din);
    c.in_offset_dms[0] = dms;
    g_rig.init(c);
    const std::uint32_t c_start = 40;
    slave_start(540000.0, c_start);
    g_rig.run_to(300000);
    NEED(g_col.count(EventKind::Step) >= 8u);
    const double shift = static_cast<double>(dms) * 100.0 * 180.0 / kCycD;  // samples
    for (std::uint32_t k = 3; k < 8; ++k) {
      const double want = sample_of_cycles(g_rig.m.time_of(c_start + 6u * k)) + shift;
      CHECK_MESSAGE(std::fabs(step_time(g_col, k) - want) <= 2.0, dms);
    }
  }
  // the master is on DIN but the tracker follows USB: its clock and Start are ignored, nothing follows, nothing plays
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Usb));
  slave_start();
  g_rig.run_to(200000);
  CHECK_FALSE(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->following());
  CHECK(g_col.count(EventKind::Step) == 0u);
  CHECK(g_rig.link->in().stats().ignored_port > 50u);
  CHECK(g_rig.link->in().stats().clocks == 0u);
}

TEST_CASE("midi link slave: a master that goes quiet stops the transport and is let go; it is followed again when it returns") {
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Din));
  const std::uint32_t c_start = 40;
  slave_start(540000.0, c_start);
  // 100 clocks, then the cable is pulled: no Stop, no clock
  const std::uint64_t pulled = static_cast<std::uint64_t>(g_rig.m.time_of(100) / kCycD);
  g_rig.run_to(pulled);
  CHECK(g_rig.s->playing());
  g_rig.m.quiet = true;
  // 48 clocks of silence (about a second) plus the blocks it takes to stop and let go
  g_rig.run_to(pulled + 90000);
  CHECK_FALSE(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->following());
  CHECK(g_col.count(EventKind::Stopped) == 1u);
  CHECK(g_rig.link->stats().stale_releases == 1u);
  const midi::SyncStatus st = g_rig.link->status();
  CHECK_FALSE(st.following);
  CHECK_FALSE(st.clock_present);
  CHECK(st.bpm_x100 == 0u);
  // it comes back: clocks, then a Start; the transport follows it again on its clock
  g_rig.m.quiet = false;
  g_rig.m.next = 0;  // a new run of clocks: its first comes 4e6 cycles from now
  g_rig.m.t0 = static_cast<double>(g_rig.sample) * kCycD + 4.0e6;
  const std::uint32_t steps_before = g_col.count(EventKind::Step);
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  g_rig.send(g_rig.m.time_of(30) - 540000.0, midi::kStart);
  g_rig.run_to(g_rig.sample + 200000);
  CHECK(g_rig.s->playing());
  CHECK(g_rig.s->following());
  CHECK(g_col.count(EventKind::Step) > steps_before + 8u);
}

TEST_CASE("midi link slave: a change of mode while following stops the transport first, then the sequencer is back on its own clock") {
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Din));
  slave_start(540000.0, 40);
  g_rig.run_to(120000);
  NEED(g_rig.s->playing());
  NEED(g_rig.s->following());
  g_rig.link->set_config(master_cfg(midi::kPortDin));
  g_rig.run_to(120000 + 640);
  CHECK_FALSE(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->following());
  CHECK(g_col.count(EventKind::Stopped) == 1u);
  // as a master it plays on its own tempo (the project's, 90 BPM) and sends the clock for it
  g_rig.m.quiet = true;
  const std::uint32_t clocks_before = g_wires.count_din(midi::kClock);
  g_rig.s->play_pattern(1);
  g_rig.run_to(g_rig.sample + 30000);
  CHECK(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->following());
  CHECK(g_wires.count_din(midi::kClock) > clocks_before + 20u);
  CHECK(g_rig.s->tempo_x100() == 9000u);
}

TEST_CASE("midi link slave: the Play key under an external clock starts on the master's next clock, never in the past") {
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Din));
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  // the master has been running clocks alone for a while (no Start): the tracker is attached and idle
  g_rig.run_to(120000);
  NEED(g_rig.s->following());
  CHECK_FALSE(g_rig.s->playing());
  const std::uint32_t late_before = g_rig.s->stats().late_events;
  const std::uint32_t overrun_before = g_rig.s->stats().microstep_overrun;
  // the Play key, between two clocks
  g_rig.s->play_pattern(1);
  g_rig.run_to(g_rig.sample + 60000);
  NEED(g_col.count(EventKind::Step) >= 6u);
  CHECK(g_rig.s->playing());
  // the first step is on a master clock (to the rounding of a sample), the next ones six clocks apart
  const double t0 = step_time(g_col, 0);
  bool on_grid = false;
  for (std::uint32_t c = 0; c < 400; ++c) {
    if (std::fabs(sample_of_cycles(g_rig.m.time_of(c)) - t0) <= 1.5) {
      on_grid = true;
      CHECK(std::fabs(step_time(g_col, 1) - sample_of_cycles(g_rig.m.time_of(c + 6u))) <= 1.5);
    }
  }
  CHECK(on_grid);
  CHECK(g_rig.s->stats().late_events == late_before);        // nothing was placed in the past
  CHECK(g_rig.s->stats().microstep_overrun == overrun_before);
}

TEST_CASE("midi link: the device settings give the mode, the followed port, the correction, the output ports and the channel; the runtime-only rest is kept") {
  namespace ds = model::device_setting;
  model::Settings dev;
  dev.init(core::ParamKind::Device);
  midi::MidiConfig base;
  base.in_offset_dms[1] = 30;
  base.out_offset_dms[0] = -20;
  base.cc_number[2] = 11;
  base.follow = midi::FollowTightness::Tight;
  base.note_ports = midi::kPortUsb;  // overwritten: MIDI OUT owns it
  // The defaults of the file: CLOCK MODE off, MIDI OUT DIN + USB, OUT CHANNEL 1: no clock, both ports, channel 0 on the wire
  midi::MidiConfig c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mode == midi::SyncMode::Off);
  CHECK(c.out_ports == (midi::kPortDin | midi::kPortUsb));
  CHECK(c.note_ports == (midi::kPortDin | midi::kPortUsb));
  CHECK(c.notes_channel == 0u);
  CHECK(c.in_offset_dms[1] == 30);  // the runtime's own, kept
  CHECK(c.out_offset_dms[0] == -20);
  CHECK(c.cc_number[2] == 11);
  CHECK(c.follow == midi::FollowTightness::Tight);
  // CLOCK MODE
  CHECK(dev.set(ds::ClockMode, 1.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).mode == midi::SyncMode::Master);
  CHECK(dev.set(ds::ClockMode, 2.0f) == model::Status::Ok);  // Slave follows the first port of MIDI IN: DIN + USB and DIN are DIN, USB alone is USB
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mode == midi::SyncMode::Slave);
  CHECK(c.in_port == midi::Port::Din);
  CHECK(dev.set(ds::MidiInPort, 1.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).in_port == midi::Port::Usb);
  CHECK(dev.set(ds::MidiInPort, 0.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).in_port == midi::Port::Din);
  CHECK(dev.set(ds::ClockMode, 0.0f) == model::Status::Ok);
  // MIDI OUT and OUT CHANNEL
  CHECK(dev.set(ds::MidiOutPort, 1.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.out_ports == midi::kPortUsb);
  CHECK(c.note_ports == midi::kPortUsb);
  CHECK(dev.set(ds::MidiOutPort, 0.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).out_ports == midi::kPortDin);
  CHECK(dev.set(ds::MidiOutChannel, 16.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).notes_channel == 15u);
  // CLOCK IN (our rule): USB and MIDI In jack are a slave on that port, whatever CLOCK MODE says
  CHECK(dev.set(ds::ClockIn, 1.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mode == midi::SyncMode::Slave);
  CHECK(c.in_port == midi::Port::Usb);
  CHECK(dev.set(ds::ClockIn, 2.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mode == midi::SyncMode::Slave);
  CHECK(c.in_port == midi::Port::Din);
  CHECK(dev.set(ds::ClockMode, 1.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).mode == midi::SyncMode::Slave);
  // PAR-119
  CHECK(dev.set(ds::ClockSync, -3.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.sync_correction == -3);
  CHECK(dev.set(ds::ClockSync, 6.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).sync_correction == 6);
}

TEST_CASE("midi link: control changes of both ports reach MIDI Learn's ring, in order, with their port and channel; timing and notes do not, and without a ring nothing breaks") {
  midi_project();
  static midi::CcInput cc;
  midi::MidiConfig cfg;  // Off
  g_rig.init(cfg);
  g_rig.link->attach_cc(&cc);
  // DIN: a control change, a note on in between, a clock inside the next message and running status; USB: another channel
  g_rig.send(1.0e6, 0xB0);
  g_rig.send(1.0e6 + 1.0e5, 74);
  g_rig.send(1.0e6 + 2.0e5, 100);
  g_rig.send(1.0e6 + 3.0e5, 0x90);
  g_rig.send(1.0e6 + 4.0e5, 60);
  g_rig.send(1.0e6 + 5.0e5, 127);
  g_rig.send(1.0e6 + 6.0e5, 0xBF);
  g_rig.send(1.0e6 + 7.0e5, 7);
  g_rig.send(1.0e6 + 8.0e5, 11);
  g_rig.send(1.0e6 + 9.0e5, 8);
  g_rig.send(1.0e6 + 1.0e6, 22);
  g_rig.send(4.0e6, 0xB2, 1);
  g_rig.send(4.0e6 + 1.0e5, 120, 1);  // all sound off: not a control
  g_rig.send(4.0e6 + 2.0e5, 0, 1);
  g_rig.send(4.0e6 + 3.0e5, 21, 1);
  g_rig.send(4.0e6 + 4.0e5, 64, 1);
  g_rig.run_to(6000);
  midi::CcMsg m{};
  NEED(cc.pop(m));
  CHECK((m.port == 0 && m.channel == 1 && m.cc == 74 && m.value == 100));
  NEED(cc.pop(m));
  CHECK((m.port == 0 && m.channel == 16 && m.cc == 7 && m.value == 11));
  NEED(cc.pop(m));
  CHECK((m.port == 0 && m.channel == 16 && m.cc == 8 && m.value == 22));
  NEED(cc.pop(m));
  CHECK((m.port == 1 && m.channel == 3 && m.cc == 21 && m.value == 64));
  CHECK_FALSE(cc.pop(m));
  CHECK(cc.dropped() == 0u);
  CHECK(g_rig.link->stats().passed_dropped == 0u);  // handed on, not dropped
  // detached: the bytes are counted and dropped as before
  g_rig.link->attach_cc(nullptr);
  g_rig.send(static_cast<double>(g_rig.sample) * kCycD + 1.0e5, 0xB0);
  g_rig.send(static_cast<double>(g_rig.sample) * kCycD + 2.0e5, 1);
  g_rig.send(static_cast<double>(g_rig.sample) * kCycD + 3.0e5, 2);
  g_rig.run_to(g_rig.sample + 3000);
  CHECK_FALSE(cc.pop(m));
  CHECK(g_rig.link->stats().passed_dropped == 3u);
}

// OS-MIDIREC-P1 S3 (A8): the render stamps a block's MIDI with the recording track's position, only while the main loop has a take open on a track that plays.
TEST_CASE("midi link: with a take open the keyboard's messages are stamped with the track's pattern and tick; stopped, on a track that is not playing, or with no take nothing is") {
  midi_project();  // 120 BPM: a step is 125 ms, 48 ticks
  static midi::CcInput cc;
  midi::MidiConfig cfg;  // Off
  g_rig.init(cfg);
  g_rig.link->attach_cc(&cc);
  midi::StampMsg s{};
  // stopped, take open: the track has no position
  cc.open_take(0);
  g_rig.send(1.0e6, 0x92);
  g_rig.send(1.0e6 + 1.0e5, 60);
  g_rig.send(1.0e6 + 2.0e5, 100);
  g_rig.run_to(2000);
  CHECK_FALSE(cc.pop_stamped(s));
  // playing, no take open: nothing, and the other rings are as before
  cc.close_take();
  g_rig.s->play_pattern(1);
  const double t0 = static_cast<double>(g_rig.sample) * kCycD;
  g_rig.send(t0 + 1.0e6, 0x92);
  g_rig.send(t0 + 1.1e6, 61);
  g_rig.send(t0 + 1.2e6, 100);
  g_rig.run_to(g_rig.sample + 2000);
  CHECK_FALSE(cc.pop_stamped(s));
  // playing, a take open on track 0: a note at the start and a control change a second later; one on an inactive track is not stamped
  cc.open_take(0);
  const std::uint64_t base = g_rig.sample;
  const double t1 = static_cast<double>(base) * kCycD;
  g_rig.send(t1 + 1.0e6, 0x92);
  g_rig.send(t1 + 1.1e6, 62);
  g_rig.send(t1 + 1.2e6, 90);
  g_rig.send(t1 + 1.8e8, 0xB2);
  g_rig.send(t1 + 1.8e8 + 1.0e5, 74);
  g_rig.send(t1 + 1.8e8 + 2.0e5, 33);
  g_rig.run_to(base + 60000);  // 1.36 s
  NEED(cc.pop_stamped(s));
  CHECK((s.kind == midi::StampKind::On && s.pattern == 1 && s.port == 0 && s.channel == 3 && s.a == 62 && s.b == 90));
  CHECK((s.tick >= 5 && s.tick <= 60));  // the play began 2000 samples (45 ms) before the take: a few ticks in
  NEED(cc.pop_stamped(s));
  CHECK((s.kind == midi::StampKind::Cc && s.pattern == 1 && s.a == 74 && s.b == 33));
  CHECK((s.tick >= 370 && s.tick <= 440));  // a second later: 8 steps of 48 ticks and the start's offset
  CHECK_FALSE(cc.pop_stamped(s));
  CHECK(cc.stamp_dropped() == 0u);
  cc.open_take(9);
  const std::uint64_t base2 = g_rig.sample;
  const double t2 = static_cast<double>(base2) * kCycD;
  g_rig.send(t2 + 1.0e6, 0x92);
  g_rig.send(t2 + 1.1e6, 63);
  g_rig.send(t2 + 1.2e6, 90);
  g_rig.run_to(base2 + 2000);
  CHECK_FALSE(cc.pop_stamped(s));
  cc.close_take();
}

TEST_CASE("midi link: in a count-in nothing is stamped but the last half-step, which goes in as the start point's first tick (OS-MIDIREC-P1 S3)") {
  midi_project();  // 120 BPM: a step is 125 ms, a bar of 16 steps 2 s
  static midi::CcInput cc;
  midi::MidiConfig cfg;
  g_rig.init(cfg);
  g_rig.link->attach_cc(&cc);
  midi::StampMsg s{};
  g_rig.s->play_pattern(1, 0, 1);  // one bar of count-in, then the pattern from step 0
  cc.open_take(0);
  const std::uint64_t base = g_rig.sample;
  const double t1 = static_cast<double>(base) * kCycD;
  g_rig.send(t1 + 0.5 * 1.8e8, 0x92);  // early in the count-in: not stamped (it is not in the pattern)
  g_rig.send(t1 + 0.5 * 1.8e8 + 1.0e5, 60);
  g_rig.send(t1 + 0.5 * 1.8e8 + 2.0e5, 100);
  g_rig.send(t1 + 1.96 * 1.8e8, 0x92);  // in the last half-step before the start: the start's first tick
  g_rig.send(t1 + 1.96 * 1.8e8 + 1.0e5, 63);
  g_rig.send(t1 + 1.96 * 1.8e8 + 2.0e5, 90);
  g_rig.send(t1 + 2.5 * 1.8e8, 0xB2);  // half a second into the pattern: about 4 steps in
  g_rig.send(t1 + 2.5 * 1.8e8 + 1.0e5, 74);
  g_rig.send(t1 + 2.5 * 1.8e8 + 2.0e5, 33);
  g_rig.run_to(base + 120000);  // 2.72 s
  NEED(cc.pop_stamped(s));
  CHECK((s.kind == midi::StampKind::On && s.pattern == 1 && s.a == 63 && s.b == 90));
  CHECK(s.tick == 0u);
  NEED(cc.pop_stamped(s));
  CHECK((s.kind == midi::StampKind::Cc && s.a == 74 && s.b == 33));
  CHECK((s.tick >= 150 && s.tick <= 240));
  CHECK_FALSE(cc.pop_stamped(s));
  cc.close_take();
}

TEST_CASE("midi link: a configuration is clamped, taken at the next block, and the latest of several wins") {
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin));
  g_rig.run_to(640);
  CHECK(g_rig.link->stats().configs == 1u);
  midi::MidiConfig c = master_cfg(midi::kPortUsb);
  c.out_offset_dms[1] = 30000;  // far out of range
  g_rig.link->set_config(midi::MidiConfig{});
  g_rig.link->set_config(c);
  CHECK(g_rig.link->config().out_offset_dms[1] == midi::kMaxOffsetDms);  // the main loop's view is the clamped value
  g_rig.run_to(1280);
  CHECK(g_rig.link->stats().configs == 2u);  // two writes, one application: the last
  CHECK(g_rig.link->out().config().clock_ports == midi::kPortUsb);
  CHECK(g_rig.link->out().config().offset_cycles[1] == midi::offset_cycles(midi::kMaxOffsetDms));
  CHECK(g_rig.s->config().emit_clock);
  CHECK(g_rig.s->config().start_lead == seq::MidiLink::kStartLead);
  // Off takes the clock out switch down again
  g_rig.link->set_config(midi::MidiConfig{});
  g_rig.run_to(1920);
  CHECK_FALSE(g_rig.s->config().emit_clock);
  CHECK(g_rig.s->config().start_lead == 0u);
  CHECK(g_rig.link->out().config().clock_ports == 0u);
}

// OS-MTC A3 (docs/specs/OS-MTC.md 3.7, A-1): the device settings MTC OUT and MTC START reach MidiConfig, and the time code goes to the ports CLOCK OUT names whatever CLOCK MODE says.
TEST_CASE("midi link: sync_config_from_settings carries MTC OUT and MTC START; the MTC ports follow MIDI OUT, with any CLOCK MODE") {
  namespace ds = model::device_setting;
  model::Settings dev;
  dev.init(core::ParamKind::Device);
  midi::MidiConfig base;
  base.mtc_fps = 30;  // overwritten: the settings own them
  base.mtc_hour = 9;
  midi::MidiConfig c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mtc_fps == 0u);   // Off, the default
  CHECK(c.mtc_hour == 1u);  // 01:00:00:00, the default
  const std::uint8_t fps_of[4] = {0, 24, 25, 30};
  for (unsigned v = 0; v <= 3u; ++v) {
    CHECK(dev.set(ds::MtcOut, static_cast<float>(v)) == model::Status::Ok);
    c = seq::sync_config_from_settings(dev, base);
    CHECK(c.mtc_fps == fps_of[v]);
  }
  for (unsigned h : {0u, 1u, 12u, 23u}) {
    CHECK(dev.set(ds::MtcStart, static_cast<float>(h)) == model::Status::Ok);
    c = seq::sync_config_from_settings(dev, base);
    CHECK(c.mtc_hour == h);
  }
  // independent of the clock rows
  CHECK(dev.set(ds::ClockMode, 2.0f) == model::Status::Ok);
  CHECK(dev.set(ds::TransportOut, 0.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mtc_fps == 30u);
  CHECK(c.mtc_hour == 23u);
  // sanitised: a value outside the range is held to it
  midi::MidiConfig wild;
  wild.mtc_fps = 29;
  wild.mtc_hour = 77;
  wild = midi::sanitized(wild);
  CHECK(wild.mtc_fps == 0u);
  CHECK(wild.mtc_hour == 23u);
}

TEST_CASE("midi link: with MTC OUT on the router's MTC ports are the output ports, with the clock Off too; with it off there are none") {
  midi_project();
  midi::MidiConfig c;  // CLOCK MODE Off: the clock has no port
  c.out_ports = midi::kPortUsb;
  c.mtc_fps = 25;
  g_rig.init(c);
  g_rig.run_to(640);
  CHECK(g_rig.link->out().config().clock_ports == 0u);
  CHECK(g_rig.link->out().config().mtc_ports == midi::kPortUsb);
  CHECK(g_rig.s->config().start_lead == seq::MidiLink::kStartLead);  // A-8: a Play begins one block late with MTC OUT on, as with clock out
  c.mtc_fps = 0;
  g_rig.link->set_config(c);
  g_rig.run_to(1280);
  CHECK(g_rig.link->out().config().mtc_ports == 0u);
  CHECK(g_rig.s->config().start_lead == 0u);
  c.mtc_fps = 30;
  c.out_ports = midi::kPortDin | midi::kPortUsb;
  g_rig.link->set_config(c);
  g_rig.run_to(1920);
  CHECK(g_rig.link->out().config().mtc_ports == (midi::kPortDin | midi::kPortUsb));
  // The router takes the time code kinds from the block: a quarter frame and a full frame on the sequencer's output reach the ports (the generator that makes them is the sequencer's).
  CHECK(g_rig.link->out().send(audio::SeqEvent{0, audio::EventKind::MidiMtcQf, audio::kNoTrack, 0x12, 0, 0, 0, 0}, 1920));
  CHECK(g_rig.link->out().staged() == 2u);
}

// ---- After an export (OS-WAVEXP) -------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi link slave: after an export (the link not stepped, the interrupts stamping on) nothing stale is applied - no Start, the rings emptied - and the loop locks on the master again") {
  for (const bool resume : {true, false}) {
    CAPTURE(resume);
    midi_project(9000);
    g_rig.init(slave_cfg(midi::Port::Din));
    g_rig.s->select(seq::PlayMode::Pattern, 1);
    g_rig.run_to(200000);  // the master's clocks, no Start: locked and followed, the tracker stopped
    NEED(g_rig.link->status().clock_present);
    CHECK_FALSE(g_rig.s->playing());
    // The export: the engine renders offline for tens of seconds and does not step the link. What the UART's interrupt stamped meanwhile is in the ring: clocks, a Start, an SPP's first byte.
    g_rig.m.quiet = resume;  // (with resume() the master is quiet here and comes back below; without it the control lets it run on)
    // (Stamped as the last of them would be, the ones just before the export ends: the worst case, each maps to a recent time. The older ones map to nothing sensible at all.)
    midi::RtInRing& ring = g_rig.link->din_in();
    const std::uint64_t now = g_rig.sample * kCyc;
    for (std::uint32_t k = 0; k < 100; ++k) {
      CHECK(ring.push(midi::RtItem{static_cast<std::uint32_t>(now - (101u - k) * kClockPeriod / 4u), midi::kClock, 0, 0}));
    }
    CHECK(ring.push(midi::RtItem{static_cast<std::uint32_t>(now - 100000u), midi::kStart, 0, 0}));
    const seq::MidiLink::Stats before = g_rig.link->stats();
    if (resume) {
      g_rig.link->resume();  // Engine::offline_leave, through EventSource::resume
    }
    g_rig.run_to(g_rig.sample + 64 * 50);
    const seq::MidiLink::Stats after = g_rig.link->stats();
    if (resume) {
      CHECK_FALSE(g_rig.s->playing());
      CHECK(after.events_applied == before.events_applied);  // the stale Start was not applied
      CHECK(after.rings_discarded - before.rings_discarded >= 101u);
      CHECK_FALSE(g_rig.s->following());  // the loop let go, to be acquired afresh
      // The master is still there: its clocks come back, the loop locks again, and still nothing starts by itself.
      g_rig.m.quiet = false;
      g_rig.m.next = 0;
      g_rig.m.t0 = static_cast<double>(g_rig.sample) * kCycD + 4.0e6;
      g_rig.run_to(g_rig.sample + 200000);
      const midi::SyncStatus st = g_rig.link->status();
      CHECK(st.clock_present);
      CHECK(st.locked);
      CHECK(st.bpm_x100 == 12000u);
      CHECK_FALSE(g_rig.s->playing());
    } else {
      g_rig.m.quiet = false;  // the control: without resume() the stale Start is armed and applied on the master's next clock
      g_rig.run_to(g_rig.sample + 64 * 400);
      CHECK(g_rig.link->stats().events_applied > before.events_applied);
    }
  }
}

// ---- TRANSPORT IN / OUT (E-CFG-03) -----------------------------------------------------------------------------------------------------------

TEST_CASE("midi link master: TRANSPORT OUT Off sends the clock but no Start, SPP, Continue or Stop") {
  for (const std::uint32_t from_step : {0u, 8u}) {  // a Start, and a play from the middle (SPP + Continue)
    CAPTURE(from_step);
    midi_project();
    midi::MidiConfig c = master_cfg(midi::kPortDin | midi::kPortUsb);
    c.transport_out = false;
    g_rig.init(c);
    g_rig.s->play_pattern(1, from_step);
    g_rig.run_to(20000);
    CHECK_FALSE(g_rig.s->config().emit_transport);  // the render thread applies the config at a block start
    CHECK(g_rig.s->config().start_lead == 0u);      // nothing to put on the wire first
    CHECK(g_wires.count_din(midi::kClock) > 10u);  // the clock still goes out
    CHECK(g_wires.count_usb_byte0(0xF8) > 10u);
    CHECK(g_wires.count_din(0x92) >= 1u);  // and the notes
    g_rig.s->stop();
    g_rig.run_to(30000);
    for (const std::uint8_t b : {midi::kStart, midi::kContinue, midi::kStop, midi::kSongPosition}) {
      CHECK(g_wires.count_din(b) == 0u);
    }
    for (const std::uint8_t b : {midi::kStart, midi::kContinue, midi::kStop, midi::kSongPosition}) {
      CHECK(g_wires.count_usb_byte0(b) == 0u);
    }
    CHECK(g_col.count(EventKind::Stopped) == 1u);
    CHECK(g_rig.link->out().book().owed() == 0u);
  }
  // The control: with the switch On (the default) the same run sends them.
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin));
  CHECK(g_rig.s->config().emit_transport);
  g_rig.s->play_pattern(1);
  g_rig.run_to(20000);
  g_rig.s->stop();
  g_rig.run_to(30000);
  CHECK(g_wires.count_din(midi::kStart) == 1u);
  CHECK(g_wires.count_din(midi::kStop) == 1u);
}

TEST_CASE("midi link master: the switch is latched at Play, so turning TRANSPORT OUT Off mid-run keeps the Stop of the Start that went out, and On mid-run adds none") {
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin));
  g_rig.s->play_pattern(1);
  g_rig.run_to(10000);
  CHECK(g_wires.count_din(midi::kStart) == 1u);
  midi::MidiConfig off = master_cfg(midi::kPortDin);
  off.transport_out = false;
  g_rig.link->set_config(off);
  g_rig.run_to(14000);
  CHECK_FALSE(g_rig.s->config().emit_transport);  // taken at a block start
  CHECK(g_rig.s->playing());
  g_rig.s->stop();
  g_rig.run_to(20000);
  CHECK(g_wires.count_din(midi::kStop) == 1u);  // the Stop of the run's Start
  // The other way round: a run that began with the switch Off has no Start, and no Stop when it is turned On meanwhile.
  g_rig.link->set_config(off);
  g_rig.run_to(24000);
  g_rig.s->play_pattern(1);
  g_rig.run_to(30000);
  g_rig.link->set_config(master_cfg(midi::kPortDin));
  g_rig.run_to(34000);
  g_rig.s->stop();
  g_rig.run_to(40000);
  CHECK(g_wires.count_din(midi::kStart) == 1u);
  CHECK(g_wires.count_din(midi::kStop) == 1u);
}

TEST_CASE("midi link slave: TRANSPORT IN Off ignores Start and Stop and SPP from the master, the clock is still followed and the Play key starts on the next clock") {
  midi_project(9000);
  midi::MidiConfig c = slave_cfg(midi::Port::Din);
  c.transport_in = false;
  g_rig.init(c);
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  // The master: clocks from the start, a Start at clock 50, an SPP and a Continue, a Stop.
  g_rig.send(g_rig.m.time_of(50) - 540000.0, midi::kStart);
  g_rig.send(g_rig.m.time_of(60) - 540000.0, midi::kSongPosition);
  g_rig.send(g_rig.m.time_of(60) - 480000.0, 4);
  g_rig.send(g_rig.m.time_of(60) - 420000.0, 0);
  g_rig.send(g_rig.m.time_of(62) - 540000.0, midi::kContinue);
  g_rig.send(g_rig.m.time_of(70) - 540000.0, midi::kStop);
  g_rig.run_to(120000);
  CHECK_FALSE(g_rig.s->playing());  // none of it moved the transport
  CHECK(g_col.count(EventKind::Step) == 0u);
  const seq::MidiLink::Stats st = g_rig.link->stats();
  CHECK(st.events_applied == 0u);
  CHECK(st.transport_ignored >= 4u);
  CHECK(g_rig.link->status().transport_events == 0u);
  // the clock is followed: the loop is locked and the sequencer attached
  const midi::SyncStatus ss = g_rig.link->status();
  CHECK(ss.clock_present);
  CHECK(ss.locked);
  CHECK(ss.bpm_x100 == 12000u);
  NEED(g_rig.s->following());
  // the Play key starts on the master's next clock, on its grid
  g_rig.s->play_pattern(1);
  g_rig.run_to(g_rig.sample + 60000);
  NEED(g_col.count(EventKind::Step) >= 6u);
  CHECK(g_rig.s->playing());
  const double t0 = step_time(g_col, 0);
  bool on_grid = false;
  for (std::uint32_t k = 0; k < 400; ++k) {
    on_grid = on_grid || std::fabs(sample_of_cycles(g_rig.m.time_of(k)) - t0) <= 1.5;
  }
  CHECK(on_grid);
  // a Stop of the master does not stop it; the master's clock keeps it going
  g_rig.send(static_cast<double>(g_rig.sample) * kCycD + 1.0e6, midi::kStop);
  g_rig.run_to(g_rig.sample + 20000);
  CHECK(g_rig.s->playing());
  CHECK(g_rig.link->stats().transport_ignored >= 5u);
  CHECK(g_col.count(EventKind::Stopped) == 0u);
}

TEST_CASE("midi link slave: TRANSPORT OUT Off relays no transport, the clock is relayed and TRANSPORT IN still moves the transport") {
  midi_project(9000);
  midi::MidiConfig c = slave_cfg(midi::Port::Din, midi::kPortDin);
  c.transport_out = false;
  g_rig.init(c);
  const std::uint32_t c_start = 40;
  slave_start(540000.0, c_start);
  g_rig.send(g_rig.m.time_of(c_start + 6 * 5 + 3), midi::kStop);
  g_rig.send(g_rig.m.time_of(c_start + 6 * 5 + 9), midi::kSongPosition);
  g_rig.send(g_rig.m.time_of(c_start + 6 * 5 + 9) + 60000.0, 9);
  g_rig.send(g_rig.m.time_of(c_start + 6 * 5 + 9) + 120000.0, 0);
  g_rig.send(g_rig.m.time_of(c_start + 6 * 5 + 11) - 1.0e6, midi::kContinue);
  g_rig.run_to(160000);
  // the transport followed the master (the Stop, then the Continue at step 9) ...
  NEED(g_col.count(EventKind::Stopped) >= 1u);
  CHECK(g_rig.link->stats().events_applied >= 4u);
  CHECK(g_rig.link->stats().transport_ignored == 0u);
  CHECK(g_rig.s->playing());
  // ... and none of it went out; the relayed clock did
  CHECK(g_wires.count_din(midi::kClock) > 20u);
  for (const std::uint8_t b : {midi::kStart, midi::kContinue, midi::kStop, midi::kSongPosition}) {
    CHECK(g_wires.count_din(b) == 0u);
  }
}

TEST_CASE("midi link: sync_config_from_settings carries both transport switches") {
  namespace ds = model::device_setting;
  model::Settings dev;
  dev.init(core::ParamKind::Device);
  midi::MidiConfig base;
  base.transport_in = false;  // overwritten: the settings own them
  base.transport_out = false;
  midi::MidiConfig c = seq::sync_config_from_settings(dev, base);
  CHECK(c.transport_in);  // the default is On
  CHECK(c.transport_out);
  CHECK(dev.set(ds::TransportIn, 0.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK_FALSE(c.transport_in);
  CHECK(c.transport_out);
  CHECK(dev.set(ds::TransportOut, 0.0f) == model::Status::Ok);
  CHECK(dev.set(ds::TransportIn, 1.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.transport_in);
  CHECK_FALSE(c.transport_out);
  // they are independent of the mode and of the other MIDI rows
  CHECK(dev.set(ds::ClockMode, 1.0f) == model::Status::Ok);
  CHECK(dev.set(ds::MidiOutPort, 0.0f) == model::Status::Ok);
  c = seq::sync_config_from_settings(dev, base);
  CHECK(c.mode == midi::SyncMode::Master);
  CHECK(c.out_ports == midi::kPortDin);
  CHECK_FALSE(c.transport_out);
}

// ---- OS-MIDISYNC 07: a tempo step to half speed must not play a step early -----------------------------------------------------------------
// The unit battery's test 07 (platform/mk66/tools/hil/MIDISYNC.md: 120 -> 180 -> 90 BPM over USB, each jump on a step boundary) left 1 missing and 1 extra step on the
// unit: the step after the 180 -> 90 jump sounded 83 ms early, the length of a step at 180 BPM. To the clock loop (midi/clock_dll.cpp) a 2:1 slowdown is a dropout of every
// other clock; each "missing" observation adds a phantom tick, so the tick runs up to three clocks ahead of the master's clock index until the 4th in a row re-acquires, and the
// sequencer used to play the step the tick had crossed (and the steps the old, fast period predicted) a clock of the new tempo early for every phantom. The sequencer now waits
// for a real clock (ClockDll::settled_tick). Reproduced here at the MidiLink level with the tool's own plan (the clock of the jump is the last one at the old tempo; every later
// interval is slow), the clocks stamped by the USB frame (1 ms). The jump may fall anywhere against the step grid: six alignments. Host only: 24 s of blocks.
// What no follower can know: the first slow clock is due one fast period after the jump clock, the second two, and the third three; the loop has not yet seen that the first one
// is late when the blocks that hold a step on the first two are rendered, and the third (the next clock after the first observation, which may be one lost clock) is played on
// the prediction so that a single dropout stays in time. A step on the n-th clock after the jump may sound up to n fast periods early (13.9 ms each at 180 BPM); every other step
// must sound within the same 3 ms band as at a steady tempo.
#if !defined(__arm__)
namespace {

// The steps of a run whose clocks are `truth` (the cycle each is sent at; a clock whose `sent` is false is lost on the way), as the Start and the clocks reach the USB
// interrupt. step_err_ms[k] is how far step k sounded from the clock that starts it (6k), signed (negative: early); returns the steps played.
constexpr std::uint32_t kMaxPlanClocks = 1248;
double g_step_err_ms[kMaxPlanClocks / 6u];

// The same, with `tail_samples` samples run past the last clock (a master that falls silent) and `after_block(next_block_start)` called after each block, to watch
// the loop's settled_tick() and the transport mid-run.
template <typename F>
std::uint32_t play_clock_plan(const double* truth, const bool* sent, std::uint32_t n_clocks, std::uint64_t tail_samples, F after_block) {
  midi_project(9000);
  g_rig.init(slave_cfg(midi::Port::Usb));
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  g_rig.m.quiet = true;  // the master is on USB only
  const double start_at = truth[0] - 540000.0;
  bool start_sent = false;
  std::uint32_t next = 0;
  // the USB interrupt stamps a clock at the end of the 1 ms frame that carried it
  const auto stamp = [&](std::uint32_t c) { return std::ceil(truth[c] / 180000.0) * 180000.0; };
  const std::uint64_t end_sample = static_cast<std::uint64_t>(truth[n_clocks - 1u] / kCycD) + tail_samples;
  while (g_rig.sample < end_sample) {
    const double now = static_cast<double>(g_rig.sample) * kCycD;
    if (!start_sent && start_at <= now) {
      CHECK(g_rig.link->usb_in().push(midi::RtItem{static_cast<std::uint32_t>(static_cast<std::uint64_t>(start_at)), midi::kStart, 1, 0}));
      start_sent = true;
    }
    while (next < n_clocks && stamp(next) <= now) {
      if (sent[next]) {
        CHECK(g_rig.link->usb_in().push(midi::RtItem{static_cast<std::uint32_t>(static_cast<std::uint64_t>(stamp(next))), midi::kClock, 1, 0}));
      }
      ++next;
    }
    g_rig.block();
    after_block(g_rig.sample);
  }
  std::uint32_t played = 0;
  for (std::uint32_t i = 0; i < g_col.n && played < n_clocks / 6u; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::Step) {
      g_step_err_ms[played] = (static_cast<double>(g_col.rec[i].t) - truth[6u * played] / kCycD) / 44.1;
      ++played;
    }
  }
  return played;
}

std::uint32_t play_clock_plan(const double* truth, const bool* sent, std::uint32_t n_clocks) {
  return play_clock_plan(truth, sent, n_clocks, 20000u, [](std::uint64_t) {});
}

// 120 -> 180 -> 90 BPM, 8 s a segment (64, 96 and 48 steps), the second jump `align` clocks after a step boundary. The interval AFTER clock c has c's tempo (the tool's
// build_plan()), so the jump clock is the last one at the old tempo. The first slow clock is `jump + 1`.
constexpr std::uint32_t kSeg0 = 384, kSeg1 = 576, kSeg2 = 288;  // clocks: 8 s at 120, 180 and 90 BPM
constexpr std::uint32_t kJumpClocks = kSeg0 + kSeg1 + kSeg2;
constexpr double kFastPeriodMs = 60000.0 / 180.0 / 24.0;

}  // namespace

TEST_CASE("midi link slave over USB: every step of the 120 -> 180 -> 90 tempo jumps sounds within 3 ms of its clock, but the three a slowdown cannot be known at (OS-MIDISYNC 07)") {
  constexpr double kTol = 3.0;
  static double truth[kJumpClocks];
  static bool sent[kJumpClocks];
  for (std::uint32_t align = 0; align < 6u; ++align) {  // align 0: the jump on a step boundary, the unit battery's 07
    const std::uint32_t jump = kSeg0 + kSeg1 + align;     // the last clock at 180 BPM
    double t = 3.0e6 + 540000.0;
    for (std::uint32_t c = 0; c < kJumpClocks; ++c) {
      truth[c] = t;
      sent[c] = true;
      const double bpm = c < kSeg0 ? 120.0 : (c < jump ? 180.0 : 90.0);
      t += 450.0e6 / bpm;
    }
    const std::uint32_t played = play_clock_plan(truth, sent, kJumpClocks);
    CHECK_MESSAGE(played == 208u, "align " << align << ": steps played " << played);
    std::uint32_t bad = 0;
    double worst = 0.0;
    std::uint32_t first_bad = 0;
    for (std::uint32_t k = 1; k < played; ++k) {  // step 0 is the Start's own (late by a block and a byte, checked by the slave test above)
      const std::uint32_t clock = 6u * k;
      const double early_ok = (clock > jump && clock <= jump + 3u) ? static_cast<double>(clock - jump) * kFastPeriodMs : 0.0;
      const double e = g_step_err_ms[k];
      if (e < -(kTol + early_ok) || e > kTol) {
        if (bad++ == 0u) {
          first_bad = k;
        }
        if (std::fabs(e) > std::fabs(worst)) {
          worst = e;
        }
      }
    }
    CHECK_MESSAGE(bad == 0u, "align " << align << ": " << bad << " step(s) off the clock grid, the first is step " << first_bad << ", the worst " << worst << " ms");
  }
}

// One clock lost on the way (a dropout the loop must bridge in time) and two in a row, at every alignment of the lost clock against the step grid, at 120 BPM: no step moves.
TEST_CASE("midi link slave over USB: a lost clock is bridged in time, a step on the lost clock or the next one sounds on its grid (OS-MIDISYNC 07)") {
  constexpr std::uint32_t kN = 720;  // 15 s at 120 BPM: 120 steps
  static double truth[kN];
  static bool sent[kN];
  for (std::uint32_t len = 1; len <= 2u; ++len) {
    for (std::uint32_t align = 0; align < 6u; ++align) {
      for (std::uint32_t c = 0; c < kN; ++c) {
        truth[c] = 3.0e6 + 540000.0 + 3750000.0 * c;  // 120 BPM: 3.75 M cycles a clock
        sent[c] = !(c >= 300u + align && c < 300u + align + len);
      }
      const std::uint32_t played = play_clock_plan(truth, sent, kN);
      CHECK_MESSAGE(played == 120u, "lost " << len << " at +" << align << ": steps played " << played);
      std::uint32_t bad = 0;
      double worst = 0.0;
      for (std::uint32_t k = 1; k < played; ++k) {
        if (std::fabs(g_step_err_ms[k]) > 3.0) {
          ++bad;
          worst = std::fabs(g_step_err_ms[k]) > std::fabs(worst) ? g_step_err_ms[k] : worst;
        }
      }
      CHECK_MESSAGE(bad == 0u, "lost " << len << " at +" << align << ": " << bad << " step(s) off the grid, the worst " << worst << " ms");
    }
  }
}

// OS-MIDISYNC 07a (the review's Level A/S): a step held on a slowdown's re-acquire must not hang when the master then goes silent. 24 clocks at the fast tempo
// lock the loop, then the interval after each of 5 more clocks is twice as long (a 2:1 slowdown): the last four of those clocks each read as one clock missing, and
// the 4th re-acquires on the leaky drop score, which sets slow_hold_ (settled_tick() == tick()) until a clock refits the period - and none comes: the plan ends, the
// master is silent, no Stop. The held step waits for a real clock, but manage_follow's stale() (kStaleClocks = 48 clock periods of the period the loop still holds,
// the fast one) releases the follow within those 48 clocks, and the release stops the run. The periods are multiples of the 1 ms USB frame so every stamp is exact and
// the re-acquire is deterministically on the last clock of the plan.
TEST_CASE("midi link slave over USB: a step held on a slowdown's re-acquire (settled_tick() == tick()), then the master goes silent with no Stop: the run stops within the 48 clocks of manage_follow/stale (OS-MIDISYNC 07a)") {
  constexpr std::uint32_t kFast = 24u, kSlow = 5u, kN = kFast + kSlow;
  constexpr double kFastPeriod = 2700000.0;  // cycles a clock at the fast tempo (166.7 BPM), a multiple of the 180000-cycle USB frame
  static double truth[kN];
  static bool sent[kN];
  double t = 3600000.0;  // also a multiple of the frame
  for (std::uint32_t c = 0; c < kN; ++c) {
    truth[c] = t;
    sent[c] = true;
    t += c < kFast ? kFastPeriod : 2.0 * kFastPeriod;  // the interval AFTER clock c: a 2:1 slowdown from clock 25 on
  }
  bool hold_seen = false;            // settled_tick() == tick(): slow_hold_ is on
  bool started_seen = false;         // the run was playing at least once
  std::uint64_t stopped_sample = 0;  // the first block after which the run is no longer playing
  const auto after = [&](std::uint64_t smp) {
    const midi::ClockDll& d = g_rig.link->dll();
    if (!hold_seen && d.has_phase() && d.settled_tick() == d.tick()) {
      hold_seen = true;
    }
    if (g_rig.s->playing()) {
      started_seen = true;
    } else if (started_seen && stopped_sample == 0u) {
      stopped_sample = smp;
    }
  };
  // 48 clock periods of the fast period the loop still holds, plus slack for the blocks the stop and the detach take
  const std::uint64_t stale_samples = static_cast<std::uint64_t>(48.0 * kFastPeriod / kCycD);
  const std::uint64_t tail = stale_samples + 512u;
  play_clock_plan(truth, sent, kN, tail, after);
  CHECK(hold_seen);  // the 2:1 slowdown re-acquired and the hold was on (fails if settled_tick() loses its slow_hold_ branch)
  // Not pinned as checks: how many re-acquires and how many steps this plan plays depend on the loop's tuning (the leaky drop score, the re-acquire on the 4th missing
  // clock, the 6-clock step grid), not on a spec promise. What OS-MIDISYNC 07a promises is checked below: the hold was seen, the run stopped, exactly one stale release.
  MESSAGE("reacquires " << g_rig.link->dll().stats().reacquires << ", steps " << g_col.count(EventKind::Step) << " (loop tuning, reported only)");
  CHECK(stopped_sample != 0u);                 // the run did stop - nothing hangs (fails if manage_follow loses the stale release)
  const std::uint64_t last_clock_sample = static_cast<std::uint64_t>(truth[kN - 1u] / kCycD);
  // within the 48 clocks, and not before them: the hold alone does not stop the run, the stop is the stale
  const std::int64_t stop_delta = stopped_sample != 0u ? static_cast<std::int64_t>(stopped_sample - last_clock_sample) : -1;
  CHECK(stop_delta <= static_cast<std::int64_t>(stale_samples) + 400);
  CHECK(stop_delta >= static_cast<std::int64_t>(stale_samples) - 400);
  CHECK_FALSE(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->following());
  CHECK(g_col.count(EventKind::Stopped) == 1u);
  CHECK(g_rig.link->stats().stale_releases == 1u);
  const midi::SyncStatus st = g_rig.link->status();
  CHECK_FALSE(st.following);
  CHECK_FALSE(st.clock_present);
  CHECK(st.bpm_x100 == 0u);
}

// OS-MIDISYNC 07a (the review's Level A/S): a Start that arrives while a step is held. 120 BPM, clocks 303 and 305 lost: two single dropouts in consecutive
// observations, the second puts run_n_ at 2 (settled_tick() == tick() - 2) and the step due on the real clock 306 waits for a real clock. The master sends a Start in that
// window: the run restarts at step 0 on the clock the Start begins on (the first sent clock at or after the Start byte: 306..313 in this plan, where the block-boundary push lands), and nothing is held any more - the held step went away
// with the run it belonged to.
TEST_CASE("midi link slave over USB: a Start while a step is held restarts the run at step 0 on the Start's clock and the hold is gone (OS-MIDISYNC 07a)") {
  constexpr std::uint32_t kN = 720;
  constexpr std::uint32_t kLost0 = 303u, kLost1 = 305u;
  static double truth[kN];
  static bool sent[kN];
  for (std::uint32_t c = 0; c < kN; ++c) {
    truth[c] = 3.0e6 + 540000.0 + 3750000.0 * c;  // 120 BPM: 3.75 M cycles a clock
    sent[c] = c != kLost0 && c != kLost1;
  }
  bool run2_seen = false;      // settled_tick() == tick() - 2: run_n_ reached 2
  bool held_seen = false;      // a step was held at that moment (follow_unsettled())
  bool start_pushed = false;
  std::uint32_t steps_at_push = 0;
  bool restart_seen = false;
  bool held_after_restart = false;
  const auto after = [&](std::uint64_t smp) {
    const midi::ClockDll& d = g_rig.link->dll();
    if (!run2_seen && d.has_phase() && d.tick() > 100u && d.settled_tick() + 2u == d.tick()) {
      run2_seen = true;
      held_seen = g_rig.s->follow_unsettled();  // the step due on the real clock 306 waits
    }
    if (run2_seen && !start_pushed) {
      start_pushed = true;
      steps_at_push = g_col.count(EventKind::Step);
      CHECK(g_rig.link->usb_in().push(midi::RtItem{static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<double>(smp) * kCycD)), midi::kStart, 1, 0}));
    }
    if (start_pushed && !restart_seen && g_col.count(EventKind::Step) > steps_at_push) {
      restart_seen = true;
    }
    if (restart_seen && g_rig.s->follow_unsettled()) {
      held_after_restart = true;
    }
  };
  play_clock_plan(truth, sent, kN, 20000u, after);
  CHECK(run2_seen);
  CHECK(held_seen);  // a step was really held when the Start arrived (fails if settled_tick() loses its run_n_ >= 2 bound)
  CHECK(start_pushed);
  CHECK(restart_seen);               // the Start moved the transport (fails if a Start while held is not applied)
  CHECK_FALSE(held_after_restart);   // the hold is gone after the restart
  // the first step of the restarted run is step 0, on the clock the Start began on: the first sent clock at or after the Start byte. Which of the sent clocks 306..313 that is
  // depends on the block boundary the push lands on (no OS-MIDISYNC spec pins the exact one), so the loop accepts that whole range within 3 ms, not a single clock
  const Rec* r = g_col.nth(EventKind::Step, steps_at_push);
  NEED(r != nullptr);
  CHECK(r->ev.value == 0);
  bool on_grid = false;
  for (std::uint32_t c = 306; c < 314u; ++c) {
    if (sent[c] && std::fabs(static_cast<double>(r->t) - truth[c] / kCycD) <= 3.0 * 44.1) {
      on_grid = true;
    }
  }
  CHECK(on_grid);
  CHECK(g_rig.s->playing());
  CHECK(g_rig.s->following());
  CHECK(g_rig.link->stats().stale_releases == 0u);
}

// OS-MIDISYNC 07a (the review's Level A/S), known and accepted: two single dropouts in consecutive observations (clocks c and c+2 lost, at every alignment of c against
// the step grid) put run_n_ at 2, so settled_tick() = tick() - 2 and a step due on a real clock inside that window waits for the next real one - it sounds about one
// clock period late. Pinned here: a step off the grid is never early and never later than 1.5 periods, and at least one alignment has one (with the run_n_ >= 2 bound of
// settled_tick() removed every step would sit on the grid within 3 ms and this test would fail).
TEST_CASE("midi link slave over USB: two single dropouts in consecutive observations (clocks c and c+2 lost) - run_n_ reaches 2 and a step due on a real clock sounds about one period late, known and accepted (OS-MIDISYNC 07a)") {
  constexpr std::uint32_t kN = 720;                       // 15 s at 120 BPM: 120 steps
  constexpr double kPeriodMs = 60000.0 / 120.0 / 24.0;    // 20.83 ms a clock
  static double truth[kN];
  static bool sent[kN];
  std::uint32_t late_total = 0;
  for (std::uint32_t align = 0; align < 6u; ++align) {
    const std::uint32_t c0 = 300u + align;
    for (std::uint32_t c = 0; c < kN; ++c) {
      truth[c] = 3.0e6 + 540000.0 + 3750000.0 * c;
      sent[c] = c != c0 && c != c0 + 2u;
    }
    bool run2_seen = false;
    const auto after = [&](std::uint64_t) {
      const midi::ClockDll& d = g_rig.link->dll();
      if (!run2_seen && d.has_phase() && d.tick() > 100u && d.settled_tick() + 2u == d.tick()) {
        run2_seen = true;  // run_n_ == 2: settled_tick() = tick() - 2
      }
    };
    const std::uint32_t played = play_clock_plan(truth, sent, kN, 20000u, after);
    CHECK_MESSAGE(played == 120u, "align " << align << ": steps played " << played);
    CHECK_MESSAGE(run2_seen, "align " << align << ": run_n_ never reached 2");
    CHECK_MESSAGE(g_rig.link->dll().stats().dropouts == 2u, "align " << align << ": dropout observations " << g_rig.link->dll().stats().dropouts);
    std::uint32_t late = 0;
    for (std::uint32_t k = 1; k < played; ++k) {
      const double e = g_step_err_ms[k];
      // |e| <= 3 ms passes. The band's early side is measurement granularity, not a tolerance for early steps: a step is recorded at its block's start, up to 64 samples
      // (1.45 ms) before the clock's USB frame stamp, itself up to 1 ms after the true clock, so an on-grid step can measure up to about 2.5 ms "early" - tightening the
      // early side below that would fail by granularity alone. "Never early" is enforced where it matters, on the held step below (in_band: e >= half a period).
      if (std::fabs(e) > 3.0) {
        // the held step: about one period late - never early, never more than 1.5 periods
        const bool in_band = e >= 0.5 * kPeriodMs && e <= 1.5 * kPeriodMs;
        CHECK_MESSAGE(in_band, "align " << align << ": step " << k << " err " << e << " ms");
        ++late;
      }
    }
    late_total += late;
  }
  CHECK(late_total >= 1u);  // the behaviour is there: at least one alignment holds a step about one period late
}
#endif

// OS-INSFX-ABI2 A3 fold: the unit attaches MidiLink::event_source, not seq::event_source, so the module clock has to come out of this one too. The source's `clock` is asked the way the engine asks it,
// once a block with the next block's first sample, after MidiLink::step has run the block: stopped it says no flags and the project tempo, playing it follows the sequencer's own clock_at.
TEST_CASE("midi link: the event source carries the module clock (clock is set, stopped then playing, the beat follows the transport)") {
  midi_project();
  g_rig.init(master_cfg(midi::kPortDin | midi::kPortUsb));
  static audio::SeqEvent buffer[audio::kMaxEventsPerBlock];
  const audio::EventSource src = g_rig.link->event_source(buffer, audio::kMaxEventsPerBlock);
  NEED(src.clock != nullptr);
  audio::ClockPos c = src.clock(src.self, 0);
  CHECK((c.flags == 0u && c.beat_q16 == 0u && c.tempo_x100 == 12000u));
  g_rig.s->play_pattern(1);
  g_rig.run_to(30000);
  NEED(g_col.count(EventKind::Step) >= 1u);
  const std::uint64_t origin = g_col.nth(EventKind::Step, 0)->t;
  const double spb = samples_per_second() * 0.5;  // 120 BPM
  const std::uint64_t at = g_rig.sample;           // the first sample of the next block
  c = src.clock(src.self, at);
  CHECK((c.flags & 1u) != 0u);
  CHECK(c.tempo_x100 == 12000u);
  const double want = static_cast<double>(at - origin) / spb;
  CHECK_MESSAGE(std::fabs(static_cast<double>(c.beat_q16) / 65536.0 - want) < 1e-3, c.beat_q16 << " vs " << want);
  CHECK(c.beat_q16 == g_rig.s->clock_at(at).beat_q16);  // the one the sequencer gives
}
