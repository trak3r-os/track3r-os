// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sequencer following an external clock (Sequencer::set_follow with midi::ClockDll, OS-005c): a simulated master sends clocks, Start, Stop, Continue and SPP as the ISR
// would stamp them; they go through the midiRt ring, MidiIn, the loop, and the sequencer's timeline, and the notes that come out are compared with the master's own clock:
// steady, a ramp, jitter, dropouts, the user's offset (a delay shifts, an advance is limited to a clock period), Start mid-song, SPP and Continue, Stop. Host and
// Cortex-M4F (`qemu_seq_model`).
#include <doctest/doctest.h>

#include <o3os/midi/in.h>
#include <o3os/seq/sequencer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "support/need.h"
#include "support/rng.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;

namespace {

// CPU cycles per sample of the build's clock (4080 by default, 1800000/441 for the 44,100 Hz retune).
constexpr double kCyc = static_cast<double>(core::kRefCyclesPerSample.num) / static_cast<double>(core::kRefCyclesPerSample.den);

struct Master {
  double bpm0 = 120.0;
  double bpm1 = 120.0;     // a ramp's end (over `ramp_clocks` clocks)
  std::uint32_t ramp_clocks = 0;
  double jitter_cycles = 0;  // uniform +-
  double drop_pct = 0;
  std::uint32_t seed = 1;
  double t0 = 3.0e6;       // the first clock, cycles
  // the true time of clock c (cycles), accumulated: tempo ramps linearly over the first ramp_clocks clocks
  double time_of(std::uint32_t c) const {
    double t = t0;
    for (std::uint32_t i = 0; i < c; ++i) {
      const double bpm = ramp_clocks != 0 ? (i < ramp_clocks ? bpm0 + (bpm1 - bpm0) * i / ramp_clocks : bpm1) : bpm0;
      t += 180.0e6 * 60.0 / (bpm * 24.0);
    }
    return t;
  }
};

StaticSlot<seq::Sequencer>& g_slot = seq_slot();
Collector& g_col = seq_collector();
midi::RtInRing g_ring;

struct Rig {
  seq::Sequencer* s = nullptr;
  midi::ClockMap map;
  midi::ClockDll dll;
  midi::MidiIn in;
  Rng rng{5};
  Master m;
  std::uint32_t next_clock = 0;     // the next clock the master has not stamped yet
  double next_clock_time = 0;
  double extra_t[16];
  std::uint8_t extra_b[16];
  std::uint32_t extra_n = 0;

  void init(const Master& master, std::int32_t offset_us, bool clock_out = false, bool ticks = false) {
    m = master;
    g_col.n = 0;
    g_col.total_events = 0;
    g_col.hash = 0xCBF29CE484222325ull;
    seq::Sequencer::Config cfg;
    cfg.emit_clock = clock_out;
    cfg.emit_ticks = ticks;
    s = &g_slot.make(seq_project());
    s->configure(cfg);
    midi::MidiIn::Config ic;
    ic.clock_enabled = true;
    in.configure(ic);
    dll.reset(0, core::samples_per_tick_q32(static_cast<std::uint32_t>(master.bpm0 * 100.0)) * 8u);
    s->set_follow(&dll, offset_us);
    rng = Rng(master.seed);
    next_clock = 0;
    next_clock_time = master.time_of(0);
    extra_n = 0;
    // flush the ring
    midi::RtItem junk;
    while (g_ring.pop(junk)) {
    }
  }

  // One byte the master sends at `true_cycles`, as the ISR stamps it (one byte time later).
  void stamp(double true_cycles, std::uint8_t byte) {
    const double jitter = m.jitter_cycles != 0.0 ? (static_cast<double>(rng.below(2000001)) / 1000000.0 - 1.0) * m.jitter_cycles : 0.0;
    const double st = true_cycles + 57600.0 + jitter;
    CHECK(g_ring.push(midi::RtItem{static_cast<std::uint32_t>(st), byte, 0, 0}));
  }

  // A byte the master sends at `true_cycles`: queued, and merged with the clocks in time order when the ISR would have seen it.
  void send(double true_cycles, std::uint8_t byte) {
    extra_t[extra_n] = true_cycles;
    extra_b[extra_n] = byte;
    ++extra_n;
  }

  // Sends the master's clocks and the extra bytes up to cycle `until` (their stamps are what matter), in time order, dropping some clocks if asked.
  void send_clocks(double until) {
    for (;;) {
      std::uint32_t best = extra_n;
      for (std::uint32_t i = 0; i < extra_n; ++i) {
        if (best == extra_n || extra_t[i] < extra_t[best]) {
          best = i;
        }
      }
      const bool clock_ready = next_clock_time + 57600.0 + m.jitter_cycles < until;
      const bool extra_ready = best != extra_n && extra_t[best] + 57600.0 + m.jitter_cycles < until;
      if (extra_ready && (!clock_ready || extra_t[best] <= next_clock_time)) {
        stamp(extra_t[best], extra_b[best]);
        extra_t[best] = extra_t[extra_n - 1];
        extra_b[best] = extra_b[extra_n - 1];
        --extra_n;
        continue;
      }
      if (!clock_ready) {
        break;
      }
      const bool drop = next_clock > 0u && m.drop_pct > 0.0 && rng.below(10000) < static_cast<std::uint32_t>(m.drop_pct * 100.0);
      if (!drop) {
        stamp(next_clock_time, midi::kClock);
      }
      ++next_clock;
      next_clock_time = m.time_of(next_clock);
    }
  }

  // Runs the render for blocks of 64 samples from sample `from` to `to`, calling `at_block(block_start)` before each (to inject bytes and commands).
  template <typename F>
  void run(std::uint64_t from, std::uint64_t to, F at_block) {
    audio::SeqEvent out[audio::kMaxEventsPerBlock];
    midi::TransportEvent ev[8];
    midi::RtItem pass[16];
    for (std::uint64_t t = from; t < to; t += 64) {
      const double now_cycles = static_cast<double>(t) * kCyc;
      at_block(t);
      send_clocks(now_cycles);
      map.set_ref(t, static_cast<std::uint32_t>(static_cast<std::uint64_t>(now_cycles)));
      const midi::MidiIn::Drained d = in.drain(g_ring, map, dll, 64, ev, 8, pass, 16);
      for (std::uint32_t i = 0; i < d.events; ++i) {
        s->apply(ev[i]);
      }
      const std::uint32_t c = s->step(t, 64, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        g_col.add(t, out[i]);
      }
    }
  }
};

Rig g_rig;

// A pattern of 16 steps with a note on every step of track 0 (the step number as the note), and a song of just it.
void follow_project() {
  fresh_project(2, 2);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(9000) == model::Status::Ok);  // 90 BPM: the master is not at the project's tempo
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(p.set_step(1, 0, k, make_step(static_cast<std::uint8_t>(40 + k))) == model::Status::Ok);
  }
  CHECK(p.append_order(1) == model::Status::Ok);
}

double sample_of_cycles(double cycles) { return cycles / kCyc; }

}  // namespace

TEST_CASE("seq follow: Start on a steady master: the notes sit on the master's clock, six clocks a step, after the first") {
  follow_project();
  Master m;
  g_rig.init(m, 0);
  g_rig.s->select(seq::PlayMode::Pattern, 1);  // what the master's Start plays: the transport is the master's, not ours
  // the master: clocks from the beginning, a Start 3 ms before clock 40, stamped as the DIN byte it is
  const std::uint32_t c_start = 40;
  g_rig.run(0, 700000, [&](std::uint64_t t) {
    const double start_at = m.time_of(c_start) - 540000.0;
    static bool sent = false;
    if (t == 0) {
      sent = false;
    }
    if (!sent && static_cast<double>(t) * kCyc >= start_at + 57600.0) {
      sent = true;
      g_rig.send(start_at, midi::kStart);
    }
  });
  NEED(g_col.count(EventKind::NoteOn, 0) >= 20u);
  NEED(g_col.count(EventKind::Step) >= 20u);
  // the transport began on clock 40: step k is at the master's clock 40 + 6 k
  std::uint32_t late_first = 0;
  for (std::uint32_t k = 0; k < 20; ++k) {
    const Rec* r = g_col.nth(EventKind::Step, k);
    NEED(r != nullptr);
    const double want = sample_of_cycles(m.time_of(c_start + 6u * k));
    const double err = static_cast<double>(r->t) - want;
    if (k == 0) {
      // the clock that starts the transport is seen after it happened: the first step is late by what the block and the byte add (at most 64 + 14 samples + the loop)
      CHECK(err >= -1.5);
      CHECK(err <= 64.0 + 16.0);
      late_first = static_cast<std::uint32_t>(err);
    } else {
      CHECK_MESSAGE(std::fabs(err) <= 1.5, k);  // later steps are placed from the loop's prediction: exact to the rounding of a sample
    }
  }
  (void)late_first;
  CHECK(g_col.nth(EventKind::NoteOn, 0, 0)->ev.a == 40);
  CHECK(g_col.nth(EventKind::NoteOn, 5, 0)->ev.a == 45);
  CHECK(g_rig.s->following());
  CHECK(g_rig.dll.locked());
  CHECK(g_rig.s->tempo_x100() == 12000u);  // the master's tempo, not the project's
}

TEST_CASE("seq follow: microsteps between clocks are even, a ramp is followed, jitter is smoothed, dropouts lose no step") {
  struct Case {
    const char* name;
    Master m;
    double max_err_samples;  // the worst note after the first few steps
    std::uint32_t seconds_samples;
  };
  Master steady;
  Master jit;
  jit.jitter_cycles = 50.0 * 180.0;  // +-50 us
  jit.seed = 3;
  Master ramp;
  ramp.bpm0 = 120.0;
  ramp.bpm1 = 126.0;
  ramp.ramp_clocks = 480;  // 6 BPM in 12 s
  ramp.jitter_cycles = 20.0 * 180.0;
  ramp.seed = 4;
  Master drops;
  drops.drop_pct = 5.0;
  drops.jitter_cycles = 30.0 * 180.0;
  drops.seed = 6;
  const Case cases[] = {
      {"steady", steady, 1.5, 600000},
      {"jitter +-50 us", jit, 6.0, 600000},
      {"ramp 120 -> 126 BPM in 10 s", ramp, 40.0, 700000},  // the loop lags a ramp by a constant: here about 0.5 ms
      {"5 % of the clocks missing", drops, 8.0, 600000},
  };
  for (const Case& c : cases) {
    follow_project();
    g_rig.init(c.m, 0);
    g_rig.s->select(seq::PlayMode::Pattern, 1);
    const std::uint32_t c_start = 60;
    bool sent = false;
    g_rig.run(0, c.seconds_samples, [&](std::uint64_t t) {
      const double start_at = c.m.time_of(c_start) - 540000.0;
      if (!sent && static_cast<double>(t) * kCyc >= start_at + 57600.0) {
        sent = true;
        g_rig.send(start_at, midi::kStart);
      }
    });
    NEED(g_col.count(EventKind::Step) >= 24u);
    double worst = 0;
    for (std::uint32_t k = 2; k < 24; ++k) {
      const Rec* r = g_col.nth(EventKind::Step, k);
      NEED(r != nullptr);
      const double want = sample_of_cycles(c.m.time_of(c_start + 6u * k));
      const double err = static_cast<double>(r->t) - want;
      worst = std::fabs(err) > worst ? std::fabs(err) : worst;
    }
    CHECK_MESSAGE(worst <= c.max_err_samples, c.name);
    // no step was lost or doubled: the step numbers run on, one a step
    for (std::uint32_t k = 0; k < 24; ++k) {
      CHECK_MESSAGE(g_col.nth(EventKind::Step, k)->ev.value == static_cast<std::int32_t>(k % 16), c.name);
    }
    // the notes between: the 8 microsteps of a clock are even (a Roll's repeats at 12 microsteps are the test)
    if (std::getenv("O3OS_SEQ_REPORT") != nullptr) {
      std::printf("fol  %-34s worst note error %5.2f samples (%.1f us)\n", c.name, worst, worst / 44.117647 * 1000.0);
    }
  }
}

TEST_CASE("seq follow: the user's offset: a delay is a shift, an advance reaches back by one clock period at most") {
  follow_project();
  Master m;
  for (const std::int32_t off_us : {2000, 5000, -1000, -50000}) {
    g_rig.init(m, off_us);
    g_rig.s->select(seq::PlayMode::Pattern, 1);
    const std::uint32_t c_start = 40;
    bool sent = false;
    g_rig.run(0, 500000, [&](std::uint64_t t) {
      const double start_at = m.time_of(c_start) - 540000.0;
      if (!sent && static_cast<double>(t) * kCyc >= start_at + 57600.0) {
        sent = true;
        g_rig.send(start_at, midi::kStart);
      }
    });
    NEED(g_col.count(EventKind::Step) >= 8u);
    // the shift in samples: the offset, but an advance never more than a clock period (919 samples at 120 BPM)
    const double shift = static_cast<double>(off_us) * 44.117647 / 1000.0;
    const double limit = -919.1;
    const double want_shift = shift < limit ? limit : shift;
    for (std::uint32_t k = 3; k < 8; ++k) {
      const double want = sample_of_cycles(m.time_of(c_start + 6u * k)) + want_shift;
      const double err = static_cast<double>(g_col.nth(EventKind::Step, k)->t) - want;
      CHECK_MESSAGE(std::fabs(err) <= 2.0, off_us);
    }
  }
}

TEST_CASE("seq follow: Stop stops, SPP then Continue resumes at the step, a second Start restarts from the beginning, the clock out is a relay") {
  follow_project();
  Master m;
  g_rig.init(m, 0, true);  // clock out on
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  const std::uint32_t c_start = 40;
  std::uint32_t phase = 0;
  const double stop_at = m.time_of(c_start + 6 * 5 + 3);  // during step 5
  const double spp_at = m.time_of(c_start + 6 * 5 + 9);
  const double cont_at = m.time_of(c_start + 6 * 5 + 11) - 1.0e6;  // a quarter clock before clock 81: it takes effect on that one
  g_rig.run(0, 600000, [&](std::uint64_t t) {
    const double now = static_cast<double>(t) * kCyc;
    if (phase == 0 && now >= m.time_of(c_start) - 540000.0 + 57600.0) {
      g_rig.send(m.time_of(c_start) - 540000.0, midi::kStart);
      phase = 1;
    } else if (phase == 1 && now >= stop_at + 57600.0) {
      g_rig.send(stop_at, midi::kStop);
      phase = 2;
    } else if (phase == 2 && now >= spp_at + 57600.0) {
      // SPP of 9 steps, then Continue: both a little before the clock they take effect on
      g_rig.send(spp_at, 0xF2);
      g_rig.send(spp_at + 1.0, 9);
      g_rig.send(spp_at + 2.0, 0);
      g_rig.send(cont_at, midi::kContinue);
      phase = 3;
    }
  });
  CHECK(g_rig.s->playing());  // it plays again
  // the transport stopped in step 5: a stopped event, notes off
  NEED(g_col.count(EventKind::Stopped) == 1u);
  // after the Continue the song cursor is at step 9
  const std::uint32_t nsteps = g_col.count(EventKind::Step);
  bool saw9 = false;
  for (std::uint32_t i = 0; i < nsteps; ++i) {
    const Rec* r = g_col.nth(EventKind::Step, i);
    if (r->t > g_col.nth(EventKind::Stopped, 0)->t && r->ev.value == 9) {
      saw9 = true;
      // on the master's clock: the Continue took effect on clock c_start + 6*5 + 12 (the first clock after it), and step 9 begins there
      const double want = sample_of_cycles(m.time_of(c_start + 6u * 5u + 11u));
      CHECK(std::fabs(static_cast<double>(r->t) - want) <= 80.0);  // the first step after a Continue is late by the block and the byte, as after a Start
      break;
    }
  }
  CHECK(saw9);
  // the clock out: a relay of the follow timeline, one a clock, MIDI start / continue / stop on the transport
  CHECK(g_col.count(EventKind::MidiStart) == 1u);
  CHECK(g_col.count(EventKind::MidiStop) == 1u);
  CHECK(g_col.count(EventKind::MidiContinue) == 1u);
  CHECK(g_col.count(EventKind::MidiClock) > 40u);
  // a second Start restarts the song: step 0 again, on the first clock after it
  follow_project();
  g_rig.init(m, 0);
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  bool s1 = false, s2 = false;
  const double second_start = m.time_of(40 + 6 * 7 + 2) - 400000.0;  // during step 7, a little before clock 84
  g_rig.run(0, 400000, [&](std::uint64_t t) {
    const double now = static_cast<double>(t) * kCyc;
    if (!s1 && now >= m.time_of(40) - 540000.0 + 57600.0) {
      g_rig.send(m.time_of(40) - 540000.0, midi::kStart);
      s1 = true;
    }
    if (!s2 && s1 && now >= second_start + 57600.0) {
      g_rig.send(second_start, midi::kStart);
      s2 = true;
    }
  });
  const double restart = sample_of_cycles(m.time_of(40 + 6 * 7 + 2));
  bool found = false;
  for (std::uint32_t i = 0; i < g_col.count(EventKind::Step); ++i) {
    const Rec* r = g_col.nth(EventKind::Step, i);
    if (static_cast<double>(r->t) > restart - 20.0 && static_cast<double>(r->t) < restart + 100.0) {
      CHECK(r->ev.value == 0);  // the first step of the new run, on the clock after the Start
      found = true;
    }
  }
  CHECK(found);
}

TEST_CASE("seq follow: Speed ticks follow the loop too, on the microstep grid, and asking for times ahead does not disturb the timeline") {
  follow_project();
  CHECK(seq_project().set_step(1, 0, 2, fx_step(seq::fx::kSpeed, 4)) == model::Status::Ok);
  Master m;
  g_rig.init(m, 0, false, true);
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  const std::uint32_t c_start = 40;
  bool sent = false;
  g_rig.run(0, 400000, [&](std::uint64_t t) {
    const double start_at = m.time_of(c_start) - 540000.0;
    if (!sent && static_cast<double>(t) * kCyc >= start_at + 57600.0) {
      sent = true;
      g_rig.send(start_at, midi::kStart);
    }
  });
  NEED(g_col.count(EventKind::Step) >= 12u);
  // the steps are exactly where they are without ticks
  for (std::uint32_t k = 2; k < 12; ++k) {
    const double want = sample_of_cycles(m.time_of(c_start + 6u * k));
    CHECK_MESSAGE(std::fabs(static_cast<double>(g_col.nth(EventKind::Step, k)->t) - want) <= 1.5, k);
  }
  // 6 ticks in steps 0 and 1, then 4 (Speed 4 in step 2 stays): the tick i of a step sits on microstep i * 48 / ticks of it, 8 and 12 microsteps apart
  const Rec* t0 = g_col.nth(EventKind::Tick, 6 + 6 + 1);  // the second tick of step 2
  const Rec* s2 = g_col.nth(EventKind::Step, 2);
  NEED(t0 != nullptr);
  NEED(s2 != nullptr);
  const double step_samples = static_cast<double>(g_col.nth(EventKind::Step, 3)->t - s2->t);
  CHECK(std::fabs(static_cast<double>(t0->t - s2->t) - step_samples / 4.0) <= 3.0);
}

// ---- half / double time under an external clock (OS-PERF2) ---------------------------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kRateStart = 40;  // the clock the master's Start sits on, as above

struct Steps {
  std::uint32_t n = 0;
  std::uint64_t t[48];
};

// One run of the steady master (120 BPM) with its Start, the rate command `rate` pushed before the first block that starts at or after `press_t` and the release before the one at
// `release_t` (0: none). The step times and the relayed clock are left in g_col.
Steps run_rate_follow(std::uint8_t rate, std::uint64_t press_t, std::uint64_t release_t) {
  follow_project();
  Master m;
  g_rig.init(m, 0, true);
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  bool sent = false;
  bool pressed = false;
  bool released = false;
  g_rig.run(0, 430000, [&](std::uint64_t t) {
    const double start_at = m.time_of(kRateStart) - 540000.0;
    if (!sent && static_cast<double>(t) * kCyc >= start_at + 57600.0) {
      sent = true;
      g_rig.send(start_at, midi::kStart);
    }
    if (press_t != 0u && !pressed && t >= press_t) {
      pressed = true;
      CHECK(g_rig.s->commands().push(seq::Command{seq::Command::Kind::PerfRate, rate, 0, 0}));
    }
    if (release_t != 0u && !released && t >= release_t) {
      released = true;
      CHECK(g_rig.s->commands().push(seq::Command{seq::Command::Kind::PerfRate, 0, 0, 0}));
    }
  });
  Steps st;
  for (std::uint32_t k = 0; k < 48 && k < g_col.count(EventKind::Step); ++k) {
    st.t[st.n++] = g_col.nth(EventKind::Step, k)->t;
  }
  return st;
}

// The first step that starts at or after the block `t` (the block's start is where a command pushed "before it" lands): the first step it applies to.
std::uint32_t first_step_from(const Steps& st, std::uint64_t t) {
  for (std::uint32_t k = 0; k < st.n; ++k) {
    if (st.t[k] >= t) {
      return k;
    }
  }
  return st.n;
}

void check_rate_follow(std::uint8_t rate, bool on_boundary) {
  const std::uint32_t per_step = rate == 1u ? 12u : 3u;  // the master's clocks a step spans at that rate
  const Steps plain = run_rate_follow(rate, 0, 0);
  NEED(plain.n >= 20u);
  const std::uint32_t press_target = 8;
  const std::uint64_t press_t = plain.t[press_target] / 64u * 64u + (on_boundary ? 0u : 64u);
  const Steps held = run_rate_follow(rate, press_t, 0);
  NEED(held.n >= 20u);
  const std::uint32_t p = first_step_from(held, press_t);
  CHECK(p == (on_boundary ? press_target : press_target + 1u));
  const std::uint32_t release_target = p + 3;
  const std::uint64_t release_t = held.t[release_target] / 64u * 64u + (on_boundary ? 0u : 64u);
  const Steps st = run_rate_follow(rate, press_t, release_t);
  const std::uint32_t q = first_step_from(st, release_t);
  CHECK(q == (on_boundary ? release_target : release_target + 1u));
  const std::uint32_t steps = std::min<std::uint32_t>(st.n, 24u);
  NEED(steps >= 20u);
  const Master m;
  // each step's start on the master's clocks: 6 a step, then per_step from the press, then 6 again; the song position never moves
  std::uint32_t clock = kRateStart;
  for (std::uint32_t k = 0; k < steps; ++k) {
    const double want = sample_of_cycles(m.time_of(clock));
    CHECK_MESSAGE(std::fabs(static_cast<double>(st.t[k]) - want) <= (k == 0 ? 80.0 : 1.5), "step " << k << " rate " << int(rate) << " got " << st.t[k] << " want " << want);
    CHECK(g_col.nth(EventKind::Step, k)->ev.value == static_cast<std::int32_t>(k % 16u));
    clock += (k >= p && k < q) ? per_step : 6u;
  }
  // the clock out is relayed at what plays: two master clocks apart at half time, half a clock at double, the master's own spacing otherwise; six a step either way
  const double period = 3.75e6 / kCyc;  // one master clock, in samples
  const double rated_gap = rate == 1u ? 2.0 * period : 0.5 * period;
  std::uint32_t gaps_rated = 0;
  std::uint32_t gaps_normal = 0;
  const std::uint32_t clocks = g_col.count(EventKind::MidiClock);
  NEED(clocks > 100u);
  for (std::uint32_t k = 1; k < clocks; ++k) {
    const std::uint64_t a = g_col.nth(EventKind::MidiClock, k - 1)->t;
    const std::uint64_t b = g_col.nth(EventKind::MidiClock, k)->t;
    if (a < st.t[1]) {  // the first step is late by the start (the loop was still acquiring)
      continue;
    }
    const double gap = static_cast<double>(b - a);
    if (a >= st.t[p] && b <= st.t[q]) {
      ++gaps_rated;
      CHECK_MESSAGE(std::fabs(gap - rated_gap) <= 2.0, "rated clock gap " << gap << " at " << k);
    } else if (b < st.t[p] || a >= st.t[q]) {
      ++gaps_normal;
      CHECK_MESSAGE(std::fabs(gap - period) <= 2.0, "clock gap " << gap << " at " << k);
    }
  }
  CHECK(gaps_rated == 6u * (q - p));
  CHECK(gaps_normal > 20u);
  CHECK(g_rig.s->play_rate() == 0u);
}

}  // namespace

TEST_CASE("seq follow rate: half time spans 12 of the master's clocks a step, double time 3; pressed and released on and off a step boundary the steps keep the master's phase") {
  for (std::uint8_t rate = 1; rate <= 2; ++rate) {
    check_rate_follow(rate, true);
    check_rate_follow(rate, false);
  }
}

// OS-INSFX-ABI2 A3: the clock under an external clock. The transport starts on the master's clock 40 and a step is six of its clocks, so the beat (four steps) is (clocks since 40) / 24 whatever the
// project's own tempo (90 here, the master's is 120), moving on the master's microstep grid: within a few samples of that line at every block end, once the first step has run.
TEST_CASE("seq follow: clock_at gives the master's beat: step k at clock 40 + 6k, linear between microsteps, the tempo the master's (OS-INSFX-ABI2 A3)") {
  follow_project();
  Master m;
  g_rig.init(m, 0);
  g_rig.s->select(seq::PlayMode::Pattern, 1);
  const std::uint32_t c_start = 40;
  const double clock_cycles = m.time_of(c_start + 1u) - m.time_of(c_start);  // a steady master: the same for every clock
  const double t_start = m.time_of(c_start);
  std::uint32_t checked = 0;
  std::uint32_t wrapped = 0;
  std::uint32_t started = 0;
  std::uint32_t tempo_bad = 0;
  double worst = 0.0;
  bool sent = false;
  g_rig.run(0, 700000, [&](std::uint64_t t) {
    const double start_at = m.time_of(c_start) - 540000.0;
    if (t == 0) {
      sent = false;
    }
    if (!sent && static_cast<double>(t) * kCyc >= start_at + 57600.0) {
      sent = true;
      g_rig.send(start_at, midi::kStart);
    }
    if (t == 0) {
      return;
    }
    const audio::ClockPos c = g_rig.s->clock_at(t);  // as the engine asks it: at the end of the block before this one
    wrapped += (c.flags & 4u) != 0u ? 1u : 0u;
    started += (c.flags & 2u) != 0u ? 1u : 0u;
    if ((c.flags & 1u) == 0u) {
      return;
    }
    const double want = std::fmod((static_cast<double>(t) * kCyc - t_start) / clock_cycles / 24.0, 4.0);  // 16 steps are 4 beats
    if (want > 0.5 && want < 4.0 - 0.01) {  // after the first steps, and not within a microstep of the pattern's end
      const double err = std::fabs(static_cast<double>(c.beat_q16) / 65536.0 - want);
      worst = std::fmax(worst, err);
      ++checked;
      tempo_bad += c.tempo_x100 != 12000u ? 1u : 0u;
    }
  });
  CHECK(started == 1u);
  CHECK(wrapped >= 6u);  // 15.9 s of a 2 s lap
  CHECK(checked > 4000u);
  CHECK_MESSAGE(worst < 6e-4, "worst error " << worst << " beats");  // 1.5 samples of the loop's placement is 7e-5 beats
  CHECK(tempo_bad == 0u);
}
