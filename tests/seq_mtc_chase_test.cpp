// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MTC B3, the chase (docs/specs/OS-MTC.md 4.3 to 4.6): seq::MidiLink with MTC IN on, a synthetic master that stamps quarter frames into the USB midiRt ring, the main loop's answer
// (app::ChaseSync) between blocks, and the sequencer's Step events as the ear. A start at the song's beginning waits for the master's pre-roll and every Step lands on its map time (the A1 song
// time map) within a few samples; a start in the middle of the song takes the tempo, the Tempo FX and the swing pair's parity of that place; a 5 ms phase step of the master is pulled in
// by the rate trim without a jump (no Stop, the step intervals stay within the trim's slew) and a master at rate 1.01 is followed, MIDI clock out included; a rate of 1.07 is refused; the
// song's end is silence until the master locates back (Q9 a); the main loop's Play and Stop do nothing while the chase runs (Q8 a) and a run of the unit's own clock is left alone until it
// stops; a late answer is asked for again; the settings (MTC IN: mode, mute of MTC OUT, the clock follow off). It holds a MidiLink (about 24 KB of statics), so it also runs in the target suite (QEMU's 4 MiB hold it) where the float maths is the target's.
#include <doctest/doctest.h>

#include <o3os/app/mtc_sync.h>
#include <o3os/midi/config.h>
#include <o3os/midi/mtc.h>
#include <o3os/model/params.h>
#include <o3os/model/settings.h>
#include <o3os/seq/midi_link.h>
#include <o3os/seq/song_time.h>

#include <cstdint>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace mtc = o3os::midi::mtc;

namespace {

constexpr bool kRate44100 = core::kRefCyclesPerSample.num == 1800000u && core::kRefCyclesPerSample.den == 441u;
constexpr double kCycD = static_cast<double>(core::kRefCyclesPerSample.num) / static_cast<double>(core::kRefCyclesPerSample.den);
constexpr double kHour = 3600.0 * 44100.0;                       // the master's time of 01:00:00:00, samples
constexpr std::uint32_t kUsbLatencyCycles = core::kCpuHz / 2000u;  // MidiIn's USB input latency: the stamp is 0.5 ms after the sender began

StaticSlot<seq::MidiLink> g_link_slot;
constexpr std::uint32_t kMaxSteps = 700;

struct StepRec {
  std::uint64_t t;
  std::uint8_t pattern;
  std::uint16_t order;
  std::uint16_t step;
};

struct Rig {
  seq::Sequencer* s = nullptr;
  seq::MidiLink* link = nullptr;
  app::ChaseSync sync;
  std::uint64_t sample = 0;
  // the master: its time at local sample `sample` is m0 + (sample - s0) x rate, in samples of its own day
  double m0 = kHour;
  double s0 = 0.0;
  double rate = 1.0;
  bool silent = false;
  unsigned fps = 25;                    // the master's frame rate as its labels count it (30 for drop-frame)
  std::uint8_t code = 1;                // the rate code the quarter frames carry in piece 7 (2 = 29.97 drop-frame)
  double start_off = kHour;             // the master's time of the song's start (MTC START), samples
  double jitter_us = 0.0;               // stamp jitter, uniform in +-this many microseconds, per frame (0: none)
  std::uint32_t rng = 12345u;
  std::uint32_t next_q = 0;
  std::uint32_t qf_skip = 0;           // a master that loses one quarter frame in `qf_skip` (0: none)
  double hold_answers_until = 0.0;     // the main loop does not answer before this sample
  std::uint32_t held_stamp = 0;
  StepRec steps[kMaxSteps];
  std::uint32_t n_steps = 0;
  std::uint32_t clocks = 0;
  std::uint64_t first_clock = 0;
  std::uint64_t last_clock = 0;
  std::uint32_t stops = 0;

  void init(const midi::MidiConfig& cfg) {
    s = &seq_slot().make(seq_project());
    seq::MidiLinkHooks hooks;
    link = &g_link_slot.make(*s, hooks);
    link->set_config(cfg);
    s->select(seq::PlayMode::Song, 0);  // what the app tells the sequencer while stopped: the chase plays what the Play key would
    sample = 0;
    m0 = kHour;
    s0 = 0.0;
    rate = 1.0;
    silent = false;
    fps = 25;
    code = 1;
    start_off = kHour;
    jitter_us = 0.0;
    rng = 12345u;
    next_q = static_cast<std::uint32_t>(m0 / qf_smp()) + 1u;  // the master's first quarter frame at or after its time now (master_jumps sets this too)
    while (static_cast<double>(next_q) * qf_smp() <= m0) {
      ++next_q;
    }
    qf_skip = 0;
    hold_answers_until = 0.0;
    n_steps = 0;
    clocks = 0;
    stops = 0;
    sync = app::ChaseSync{};
  }

  double qf_smp() const {  // real samples of a quarter frame at the master's rate (drop-frame runs 1001/1000 fast in labels)
    return 44100.0 / (4.0 * static_cast<double>(fps)) * (code == 2u ? 1001.0 / 1000.0 : 1.0);
  }
  // The data byte of quarter frame `q` (linear, past the day counts on; the label wraps): piece 7 carries the rate code.
  std::uint8_t qf_byte_now(std::uint64_t q) const {
    const std::uint64_t per_day = mtc::quarter_frames_per_day_of_code(code);
    const std::uint64_t n = q % per_day;
    const std::uint64_t cycle_start = n - (n & 7u);
    return mtc::qf_data(static_cast<std::uint32_t>(n & 7u), mtc::label_of_frames(static_cast<std::uint32_t>(cycle_start / 4u), code), code);
  }
  double master_time(double smp) const { return m0 + (smp - s0) * rate; }
  // The local sample at which the master's time is `t` (the master sends quarter frame q then).
  double local_of_master(double t) const { return s0 + (t - m0) / rate; }

  // From local sample `at` on the master's time is `t` (a relocation, or a phase step when `t` is near).
  void master_jumps(double at, double t) {
    m0 = t;
    s0 = at;
    next_q = static_cast<std::uint32_t>(t / qf_smp()) + 1u;
    while (static_cast<double>(next_q) * qf_smp() <= t) {
      ++next_q;
    }
  }

  // A full frame (SysEx) with the master's time now, at its rate: what a real master sends when it locates or changes rate.
  void push_full_frame_now() {
    const double t = master_time(static_cast<double>(sample));
    const mtc::Time tm = mtc::label_of_frames(static_cast<std::uint32_t>((t / qf_smp()) / 4.0), code);
    const std::uint32_t packed = mtc::pack_full(tm, mtc::rate_code(fps));
    std::uint8_t body[mtc::kFullFrameBodyBytes];
    mtc::full_frame_body(packed, body);
    const std::uint32_t cyc = static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<double>(sample) * kCycD) + kUsbLatencyCycles);
    CHECK(link->usb_in().push(midi::RtItem{cyc, 0xF0, 1, 0}));
    for (std::uint32_t i = 0; i < mtc::kFullFrameBodyBytes; ++i) {
      CHECK(link->usb_in().push(midi::RtItem{cyc, body[i], 1, 0}));
    }
    CHECK(link->usb_in().push(midi::RtItem{cyc, 0xF7, 1, 0}));
  }

  // The master changes its frame rate (a real master sends a full frame with the new rate code; the quarter frames carry it too).
  void set_fps(unsigned f) { set_rate(f, mtc::rate_code(f)); }
  void set_rate(unsigned f, std::uint8_t c) {
    fps = f;
    code = c;
    const double t_now = master_time(static_cast<double>(sample));
    next_q = static_cast<std::uint32_t>(t_now / qf_smp()) + 1u;
    while (static_cast<double>(next_q) * qf_smp() <= t_now) {
      ++next_q;
    }
  }

  void feed_master(double now_samples) {
    if (silent) {
      return;
    }
    for (;;) {
      const double due = local_of_master(static_cast<double>(next_q) * qf_smp());
      if (due + 22.0 >= now_samples) {  // its stamp (0.5 ms after it began) has not come yet
        break;
      }
      if (qf_skip != 0u && next_q % qf_skip == 0u) {
        ++next_q;
        continue;
      }
      double jit = 0.0;
      if (jitter_us != 0.0) {
        rng = rng * 1664525u + 1013904223u;
        const std::int64_t j = static_cast<std::int64_t>(rng % static_cast<std::uint32_t>(2.0 * jitter_us + 1.0)) - static_cast<std::int64_t>(jitter_us);
        jit = static_cast<double>(j) * 44100.0 / 1.0e6;  // microseconds to samples
      }
      const std::uint32_t cyc = static_cast<std::uint32_t>(static_cast<std::uint64_t>((due + jit) * kCycD) + kUsbLatencyCycles);
      CHECK(link->usb_in().push(midi::RtItem{cyc, 0xF1, 1, 0}));
      CHECK(link->usb_in().push(midi::RtItem{cyc, qf_byte_now(next_q), 1, 0}));
      ++next_q;
    }
  }

  void block() {
    audio::SeqEvent out[audio::kMaxEventsPerBlock];
    feed_master(static_cast<double>(sample));
    const std::uint32_t word = link->chase_request();
    if (static_cast<double>(sample) >= hold_answers_until || (word >> 24) == held_stamp) {
      (void)sync.step(seq_project(), word, link->chase_request_time(), s->commands());
    }
    const std::uint32_t c = link->step(sample, 64, out, audio::kMaxEventsPerBlock);
    for (std::uint32_t i = 0; i < c; ++i) {
      const audio::SeqEvent& e = out[i];
      const std::uint64_t t = sample + e.offset;
      if (e.kind == EventKind::Step && (e.flags & audio::kEvCountIn) == 0u && n_steps < kMaxSteps) {
        steps[n_steps++] = StepRec{t, e.a, static_cast<std::uint16_t>(e.b | (static_cast<std::uint16_t>(e.c) << 8)), static_cast<std::uint16_t>(e.value)};
      } else if (e.kind == EventKind::MidiClock) {
        if (clocks == 0u) {
          first_clock = t;
        }
        last_clock = t;
        ++clocks;
      } else if (e.kind == EventKind::Stopped) {
        ++stops;
      }
    }
    sample += 64;
  }
  void run_to(std::uint64_t to) {
    while (sample < to) {
      block();
    }
  }
};

Rig g_rig;

// Pattern 1: 16 steps. Pattern 2: 16 steps, a Swing FX (60) on step 0 and a Tempo FX (70: 140 BPM) on step 4. Song [1, 2], 120 BPM. One step of 120 BPM is 5512.5 samples.
void chase_project(unsigned tempo_x100 = 12000) {
  fresh_project(2, 2);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(tempo_x100) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 16) == model::Status::Ok);
  CHECK(p.set_step(2, 0, 0, fx_step(17, 60)) == model::Status::Ok);
  CHECK(p.set_step(2, 0, 4, fx_step(15, 70)) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
}

midi::MidiConfig chase_cfg(std::uint8_t mtc_in = 2, std::uint8_t out_ports = 0) {
  midi::MidiConfig c;
  c.mtc_in = mtc_in;
  c.mtc_hour = 1;
  c.out_ports = out_ports;
  c.mode = out_ports != 0u ? midi::SyncMode::Master : midi::SyncMode::Off;
  return c;
}

// The A1 map's time of a Step event, whole samples, and where the master has it: the local sample at which the master reaches MTC START plus that time.
double expected_sample(const Rig& r, const StepRec& st) {
  const seq::SongPoint pt = seq::song_time_at(seq_project(), seq::PlayMode::Song, st.order, st.step);
  return r.local_of_master(r.start_off + static_cast<double>(core::q32_floor(pt.time)));
}

// qf_time_real(q, code) in whole+double samples (the real time of a label at a rate code).
double qf_real_samples(std::uint32_t q, std::uint8_t code) {
  return static_cast<double>(mtc::qf_time_real(q, code)) / 4294967296.0;
}

double abs_d(double v) { return v < 0.0 ? -v : v; }

}  // namespace

TEST_CASE("chase: the start waits for the master's pre-roll, then every Step lands on its map time (tempo FX and swing included) and the chase locks") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  g_rig.init(chase_cfg());
  g_rig.master_jumps(0.0, kHour - 40000.0);  // the master is 40,000 samples (0.9 s) before 01:00:00:00 and runs on
  g_rig.run_to(30000);
  CHECK(g_rig.n_steps == 0u);  // nothing before the master reaches MTC START
  g_rig.run_to(40000 + 2 * 44100);  // the run began at 40,000: 2 s into the song
  NEED(g_rig.n_steps > 14u);
  const midi::SyncStatus st = g_rig.link->status();
  CHECK(st.mtc_state == static_cast<std::uint8_t>(midi::MtcState::Locked));
  CHECK(g_rig.s->chase_run());
  CHECK(g_rig.link->stats().chase_starts == 1u);
  g_rig.run_to(40000 + 4 * 44100);  // and to the song's end (32 steps, 3.8 s): the chase does not loop
  CHECK(g_rig.n_steps == 32u);
  CHECK(g_rig.link->stats().chase_relocations == 0u);
  CHECK(g_rig.steps[0].order == 0u);
  CHECK(g_rig.steps[0].step == 0u);
  double worst = 0.0;
  for (std::uint32_t k = 0; k < g_rig.n_steps; ++k) {
    const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
    worst = abs_d(err) > worst ? abs_d(err) : worst;
    CHECK_MESSAGE(abs_d(err) <= 3.0, "step " << k << " order " << g_rig.steps[k].order << " step " << g_rig.steps[k].step << " off by " << err << " samples");
  }
  CHECK(worst <= 3.0);
  CHECK(g_rig.steps[g_rig.n_steps - 1].order == 1u);
}

TEST_CASE("chase: a master that is already running starts the run in the middle of the song, with that place's tempo and the swing pair's parity") {
  if (!kRate44100) {
    return;
  }
  // 01:00:02:14 and on in 15 ms offsets: slot 2 (it starts at 2.0 s), a little past step 4, where the Tempo FX makes 140 BPM, swing 60 from step 0. Twenty-four offsets: the first step at or after the master's time plus the
  // margin is sometimes odd in its swing pair and sometimes even; the run's steps land on the map's time either way.
  bool odd_seen = false;
  bool even_seen = false;
  for (unsigned k = 0; k < 24u; ++k) {  // fine offsets: the first step walks through both swing-pair parities
    chase_project();
    model::Project& p4 = seq_project();
    CHECK(p4.append_order(1) == model::Status::Ok);
    CHECK(p4.append_order(2) == model::Status::Ok);  // the song is 4 slots: every offset still has room for more than 8 steps in the window
    g_rig.init(chase_cfg());
    g_rig.master_jumps(0.0, kHour + (2.56 + (k >= 12u ? 0.1 : 0.0) + static_cast<double>(k % 12u) * 0.015) * 44100.0);  // 15 ms offsets on two bases (0.1 s apart), so the first step lands on both swing-pair parities
    g_rig.run_to(90000);
    NEED(g_rig.n_steps > 8u);
    CHECK(g_rig.steps[0].order == 1u);
    CHECK(g_rig.steps[0].step >= 5u);
    CHECK(g_rig.link->stats().chase_starts == 1u);
    ((16u + g_rig.steps[0].step) % 2u == 1u ? odd_seen : even_seen) = true;  // the start step's swing-pair parity
    for (std::uint32_t j = 0; j < g_rig.n_steps; ++j) {
      const double err = static_cast<double>(g_rig.steps[j].t) - expected_sample(g_rig, g_rig.steps[j]);
      CHECK_MESSAGE(abs_d(err) <= 3.0, "offset " << k << " step " << j << " order " << g_rig.steps[j].order << " step " << g_rig.steps[j].step << " off by " << err << " samples");
    }
  }
  CHECK(odd_seen);
  CHECK(even_seen);
}

TEST_CASE("chase: a time before the song start arms step 1 for the sample the master reaches MTC START; a time past the song's end is silence until the master locates back") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  g_rig.init(chase_cfg());
  g_rig.master_jumps(0.0, kHour - 3.0 * 44100.0);  // 3 s of pre-roll
  g_rig.run_to(3 * 44100 - 2000);
  CHECK(g_rig.n_steps == 0u);
  g_rig.run_to(3 * 44100 + 4000);
  NEED(g_rig.n_steps >= 1u);
  CHECK(abs_d(static_cast<double>(g_rig.steps[0].t) - static_cast<double>(3 * 44100)) <= 3.0);
  // The song is 32 steps; with the Tempo FX 16 + 12 at 120 and 140 BPM it ends before 4 s. The master runs on to 8 s: silence, state PAST END, no second pass.
  g_rig.run_to(8 * 44100);
  const std::uint32_t n_end = g_rig.n_steps;
  CHECK(n_end == 32u);
  CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::PastEnd));
  CHECK_FALSE(g_rig.s->chase_run());
  CHECK_FALSE(g_rig.s->playing());
  g_rig.run_to(10 * 44100);
  CHECK(g_rig.n_steps == n_end);
  // The master locates back to 01:00:01:00: a relocation (the chaser tells), the question starts over and the song plays from there.
  const std::uint64_t reloc_at = g_rig.sample;
  g_rig.master_jumps(static_cast<double>(g_rig.sample), kHour + 44100.0);
  g_rig.run_to(14 * 44100);  // a window with room for the re-settled start and its steps (the fit needs about a hundred quarter frames after a relocation)
  NEED(g_rig.n_steps > n_end);
  CHECK(g_rig.link->stats().chase_relocations >= 1u);
  CHECK(g_rig.link->stats().chase_starts == 2u);
  CHECK(g_rig.steps[n_end].order <= 1u);  // from the relocated place (the settle wait lets the master run on about a second, so the start can be in slot 2)
  CHECK(g_rig.steps[n_end].t - reloc_at <= 80000u);  // the measured worst case from relocation to the re-start's first step is 79,852 samples (1.81 s): 160 quarter frames of re-convergence + the 100 ms settle hold + the ask margin
  for (std::uint32_t k = n_end; k < g_rig.n_steps; ++k) {
    const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
    CHECK_MESSAGE(abs_d(err) <= 3.0, "step " << k << " off by " << err << " samples");
  }
}

TEST_CASE("chase: a 5 ms phase step of the master is pulled in by the rate trim: no stop, no jump, in phase again within seconds") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  model::Project& p4 = seq_project();
  CHECK(p4.append_order(1) == model::Status::Ok);
  CHECK(p4.append_order(2) == model::Status::Ok);  // 4 slots: the window below stays inside the song, so the only stop would be the step's
  g_rig.init(chase_cfg());
  g_rig.master_jumps(0.0, kHour - 20000.0);
  g_rig.run_to(20000 + 88200);
  NEED(g_rig.n_steps > 12u);
  CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::Locked));
  const std::uint32_t before = g_rig.n_steps;
  // The master's phase moves by 5 ms (220.5 samples) at this sample, the quarter frames go on at the same rate.
  const double at = static_cast<double>(g_rig.sample);
  g_rig.master_jumps(at, g_rig.master_time(at) + 220.5);
  const std::uint64_t until = g_rig.sample + 3 * 44100;
  g_rig.run_to(until);
  NEED(g_rig.n_steps > before + 10u);
  CHECK(g_rig.link->stats().chase_relocations == 0u);
  CHECK(g_rig.link->stats().chase_starts == 1u);
  CHECK(g_rig.stops == 0u);
  // no jump: a step is never more than the 6 % of the trim's limit away from its nominal length (5512.5 samples; the Tempo FX changes the nominal, so compare with the map)
  for (std::uint32_t k = before; k < g_rig.n_steps; ++k) {
    const std::int64_t a = static_cast<std::int64_t>(g_rig.steps[k - 1].t);
    const std::int64_t b = static_cast<std::int64_t>(g_rig.steps[k].t);
    const seq::SongPoint p0 = seq::song_time_at(seq_project(), seq::PlayMode::Song, g_rig.steps[k - 1].order, g_rig.steps[k - 1].step);
    const seq::SongPoint p1 = seq::song_time_at(seq_project(), seq::PlayMode::Song, g_rig.steps[k].order, g_rig.steps[k].step);
    const double nominal = static_cast<double>(core::q32_floor(p1.time) - core::q32_floor(p0.time));
    CHECK_MESSAGE(static_cast<double>(b - a) <= nominal * 1.0601 + 2.0, "step " << k);
    CHECK_MESSAGE(static_cast<double>(b - a) >= nominal * 0.9399 - 2.0, "step " << k);
  }
  // in phase again: the last steps land on their map times within 1 ms (44 samples)
  const StepRec& last = g_rig.steps[g_rig.n_steps - 1];
  CHECK(abs_d(static_cast<double>(last.t) - expected_sample(g_rig, last)) <= 44.1);
}

TEST_CASE("chase: a master at rate 1.01 is followed and the MIDI clock out follows the trimmed run; 1.07 is refused") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  g_rig.init(chase_cfg(2, midi::kPortUsb));
  g_rig.rate = 1.01;
  g_rig.master_jumps(0.0, kHour - 20000.0);
  g_rig.run_to(20000 + 83790);  // 1.9 s of the master's time into slot 1 (it lasts 2 s of the master's time: 1.98 s of ours)
  NEED(g_rig.n_steps > 10u);
  CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::Locked));
  const std::int32_t ppm = g_rig.link->status().mtc_rate_ppm;
  CHECK(ppm > 9000);
  CHECK(ppm < 11000);
  // clock out within slot 1 (no tempo change there): 24 ppqn on the trimmed grid. At 120 BPM a clock is 918.75 samples; at rate 1.01 it is 909.65
  NEED(g_rig.clocks > 40u);
  const double mean = static_cast<double>(g_rig.last_clock - g_rig.first_clock) / static_cast<double>(g_rig.clocks - 1u);
  CHECK(abs_d(mean - 918.75 / 1.01) <= 2.7);
  g_rig.run_to(20000 + 83790 + 44100);
  CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::Locked));
  // the steps are 1 % shorter than the nominal: the sample of each is its map time over 1.01 (the master's own samples)
  for (std::uint32_t k = 4; k < g_rig.n_steps; ++k) {
    const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
    CHECK_MESSAGE(abs_d(err) <= 6.0, "step " << k << " off by " << err);
  }
  // refused: 1.07 is outside 0.94 .. 1.06
  chase_project();
  g_rig.init(chase_cfg());
  g_rig.rate = 1.07;
  g_rig.master_jumps(0.0, kHour - 20000.0);
  g_rig.run_to(3 * 44100);
  CHECK(g_rig.n_steps == 0u);
  CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::Rate));
}

TEST_CASE("chase: while the chase runs the unit's Play, Stop and slot jumps do nothing; a run of its own clock is left alone and the master takes over once it is stopped") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  g_rig.init(chase_cfg());
  // The master is silent: the user's Play runs the song on the unit's own clock.
  g_rig.silent = true;
  g_rig.run_to(2000);
  CHECK(g_rig.s->commands().push(seq::Command{seq::Command::Kind::PlaySong, 0, 0, 0}));
  g_rig.run_to(30000);
  CHECK(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->chase_run());
  const std::uint32_t own = g_rig.n_steps;
  CHECK(own > 3u);
  // A master that begins now is not taken over: the run goes on, no question is asked.
  g_rig.silent = false;
  g_rig.master_jumps(static_cast<double>(g_rig.sample), kHour + 1.0 * 44100.0);
  g_rig.run_to(60000);
  CHECK(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->chase_run());
  CHECK(g_rig.link->stats().chase_requests == 0u);
  // The user stops: the master takes over (the chase asks, starts, and the run is the chase's).
  CHECK(g_rig.s->commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
  g_rig.run_to(60000 + 20000);
  CHECK(g_rig.link->stats().chase_starts == 1u);
  CHECK(g_rig.s->chase_run());
  CHECK(g_rig.s->playing());
  // Now Play, Stop and a slot jump do nothing; the run goes on, never restarts.
  const std::uint32_t steps_before = g_rig.n_steps;
  const std::uint32_t dropped = g_rig.s->stats().chase_dropped;
  CHECK(g_rig.s->commands().push(seq::Command{seq::Command::Kind::PlaySong, 0, 0, 0}));
  CHECK(g_rig.s->commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}));
  CHECK(g_rig.s->commands().push(seq::Command{seq::Command::Kind::QueueJump, 0, 0, 0}));
  CHECK(g_rig.s->commands().push(seq::Command{seq::Command::Kind::Locate, 0, 0, 0}));
  g_rig.run_to(g_rig.sample + 20000);
  CHECK(g_rig.s->stats().chase_dropped == dropped + 4u);
  CHECK(g_rig.s->playing());
  CHECK(g_rig.s->chase_run());
  CHECK(g_rig.n_steps > steps_before + 2u);
  CHECK(g_rig.stops == 1u);  // the user's own stop only
  // The master's stop is the chase's end: after 4 frames of silence the run stops and the buttons act again.
  g_rig.silent = true;
  g_rig.run_to(g_rig.sample + 20000);
  CHECK_FALSE(g_rig.s->playing());
  CHECK_FALSE(g_rig.s->chase_run());
  CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::Waiting));
}

TEST_CASE("chase: a late answer is asked for again with a longer margin") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  g_rig.init(chase_cfg());
  g_rig.master_jumps(0.0, kHour + 2.0 * 44100.0);
  g_rig.hold_answers_until = 12000.0;  // the main loop is busy (the card) for 12,000 samples: past the 50 ms margin, short of the 500 ms timeout
  g_rig.run_to(60000);
  CHECK(g_rig.link->stats().chase_retries >= 1u);
  CHECK(g_rig.link->stats().chase_requests >= 2u);
  CHECK(g_rig.link->stats().chase_starts == 1u);
  NEED(g_rig.n_steps > 4u);
  for (std::uint32_t k = 0; k < g_rig.n_steps; ++k) {
    const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
    CHECK_MESSAGE(abs_d(err) <= 3.0, "step " << k << " off by " << err);
  }
}

TEST_CASE("chase: a full frame while stopped moves the cursor and plays nothing") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  g_rig.init(chase_cfg());
  g_rig.silent = true;  // the master is otherwise silent: the chaser stays Idle, so the full frame is a Locate
  g_rig.run_to(2000);
  // 01:00:02:20 (the song's slot 2, 2.5 s in: order 1 step 4 at 2.5 s): a full frame, the master otherwise silent.
  const mtc::Time t{1, 0, 2, 20};
  const std::uint32_t packed = mtc::pack_full(t, 1);
  std::uint8_t body[mtc::kFullFrameBodyBytes];
  mtc::full_frame_body(packed, body);
  const double at = static_cast<double>(g_rig.sample);
  const std::uint32_t cyc = static_cast<std::uint32_t>(static_cast<std::uint64_t>(at * kCycD) + kUsbLatencyCycles);
  auto push = [&](std::uint8_t b) { CHECK(g_rig.link->usb_in().push(midi::RtItem{cyc, b, 1, 0})); };
  push(0xF0);
  for (std::uint32_t i = 0; i < mtc::kFullFrameBodyBytes; ++i) {
    push(body[i]);
  }
  push(0xF7);
  g_rig.run_to(20000);
  CHECK(g_rig.link->stats().chase_locates == 1u);
  CHECK(g_rig.n_steps == 0u);
  CHECK_FALSE(g_rig.s->playing());
  const seq::Position pos = g_rig.s->position();
  CHECK(pos.order_pos == 1u);
  CHECK(pos.step >= 4u);
  CHECK(pos.step <= 8u);
}

TEST_CASE("chase: MTC IN in the settings: the port, the clock follow off, MTC OUT muted; off by default") {
  model::Settings dev;
  dev.init(core::ParamKind::Device);
  namespace ds = model::device_setting;
  CHECK(dev.get(ds::MtcIn) == 0.0f);
  midi::MidiConfig base;
  CHECK(seq::sync_config_from_settings(dev, base).mtc_in == 0u);
  CHECK(dev.set(ds::MtcIn, 1.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).mtc_in == 1u);
  CHECK(dev.set(ds::MtcIn, 2.0f) == model::Status::Ok);
  CHECK(seq::sync_config_from_settings(dev, base).mtc_in == 2u);
  CHECK(dev.set(ds::MtcIn, 3.0f) == model::Status::BadValue);  // 3 is past the last choice
  midi::MidiConfig wild = base;
  wild.mtc_in = 9;
  CHECK(midi::sanitized(wild).mtc_in == 0u);

  chase_project();
  midi::MidiConfig c = chase_cfg(2, midi::kPortUsb);
  c.mtc_fps = 25;
  c.mode = midi::SyncMode::Slave;
  c.in_port = midi::Port::Usb;
  g_rig.init(c);
  g_rig.run_to(64);
  CHECK(g_rig.link->out().config().mtc_ports == 0u);    // MTC OUT is muted while MTC IN is on
  CHECK_FALSE(g_rig.s->config().emit_mtc);
  CHECK(g_rig.link->out().config().clock_ports == midi::kPortUsb);  // a Slave with MTC IN on acts as a Master for the clock out
  CHECK_FALSE(g_rig.link->in().config().clock_enabled);  // the clock follow is off
  CHECK(g_rig.link->in().config().mtc_enabled);
  CHECK(g_rig.link->in().config().mtc_port == midi::Port::Usb);
  c.mtc_in = 0;
  g_rig.link->set_config(c);
  g_rig.run_to(256);
  CHECK(g_rig.link->out().config().mtc_ports == midi::kPortUsb);  // MTC IN off again: MTC OUT is back
  CHECK(g_rig.s->config().emit_mtc);
  CHECK(g_rig.link->in().config().clock_enabled);  // and the Slave follows the clock again
  CHECK_FALSE(g_rig.link->in().config().mtc_enabled);
}

TEST_CASE("chase: at 200 BPM (a step shorter than the settle window) the start still settles, locks and lands on the map") {
  if (!kRate44100) {
    return;
  }
  chase_project(20000);  // 200 BPM: a step is 3307.5 samples (75 ms), shorter than the 100 ms settle window
  g_rig.init(chase_cfg());
  g_rig.master_jumps(0.0, kHour - 20000.0);
  g_rig.run_to(20000 + 44100);  // mid-song: the start must have settled and locked by now (the song is about 2.3 s at 200 BPM)
  CHECK(g_rig.link->stats().chase_starts == 1u);
  CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::Locked));
  g_rig.run_to(20000 + 2 * 44100);
  NEED(g_rig.n_steps > 10u);
  for (std::uint32_t k = 0; k < g_rig.n_steps; ++k) {
    const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
    CHECK_MESSAGE(abs_d(err) <= 3.0, "step " << k << " order " << g_rig.steps[k].order << " step " << g_rig.steps[k].step << " off by " << err << " samples");
  }
}

TEST_CASE("chase: a master with +-2 ms stamp jitter still starts once, locks and rides the jitter out") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  g_rig.init(chase_cfg());
  g_rig.jitter_us = 2000.0;  // +-2 ms of stamp jitter, uniform per frame (as the B2 chaser test's master)
  g_rig.master_jumps(0.0, kHour - 20000.0);
  g_rig.run_to(20000 + 2 * 44100);
  CHECK(g_rig.link->stats().chase_starts == 1u);
  CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::Locked));
  g_rig.run_to(20000 + 4 * 44100);
  CHECK(g_rig.link->stats().chase_retries < 24u);  // no endless re-asking while the fit wobbles
  NEED(g_rig.n_steps > 12u);
  for (std::uint32_t k = 0; k < g_rig.n_steps; ++k) {
    const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
    CHECK_MESSAGE(abs_d(err) <= 120.0, "step " << k << " off by " << err << " samples");  // the stamps wobble +-2 ms; the steps stay within that of the map
  }
}

TEST_CASE("chase: a master at 30 fps is chased the same") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  g_rig.init(chase_cfg());
  g_rig.set_fps(30);
  g_rig.master_jumps(0.0, kHour - 20000.0);
  g_rig.run_to(20000 + 44100);
  CHECK(g_rig.link->stats().chase_starts == 1u);
  CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::Locked));
  g_rig.run_to(20000 + 3 * 44100);
  NEED(g_rig.n_steps > 10u);
  for (std::uint32_t k = 0; k < g_rig.n_steps; ++k) {
    const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
    CHECK_MESSAGE(abs_d(err) <= 3.0, "step " << k << " order " << g_rig.steps[k].order << " step " << g_rig.steps[k].step << " off by " << err << " samples");
  }
}

TEST_CASE("chase: the master changes frame rate under a locked run: a pause, a full frame with the new rate, a re-settled start on the map again") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  model::Project& p4 = seq_project();
  CHECK(p4.append_order(1) == model::Status::Ok);
  CHECK(p4.append_order(2) == model::Status::Ok);  // 4 slots: the master runs past the first two while the fit re-converges
  g_rig.init(chase_cfg());
  g_rig.master_jumps(0.0, kHour - 20000.0);
  g_rig.run_to(20000 + 2 * 44100);
  NEED(g_rig.n_steps > 8u);
  CHECK(g_rig.link->stats().chase_starts == 1u);
  const std::uint32_t n_before = g_rig.n_steps;
  const std::uint64_t at = g_rig.sample;
  // A real master pauses, announces the new rate with a full frame (a mid-stream rate change is not decodable), and runs on at the new rate.
  g_rig.silent = true;
  g_rig.run_to(at + 44100);  // the pause: the chaser freewheels, stops, goes idle
  g_rig.set_fps(30);
  g_rig.push_full_frame_now();
  g_rig.silent = false;
  g_rig.run_to(at + 12 * 44100);
  CHECK(g_rig.link->stats().chase_relocations >= 1u);
  CHECK(g_rig.link->status().mtc_code == 3u);  // 30 fps
  CHECK(g_rig.link->stats().chase_starts == 2u);
  NEED(g_rig.n_steps > n_before + 4u);
  for (std::uint32_t k = n_before; k < g_rig.n_steps; ++k) {
    const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
    CHECK_MESSAGE(abs_d(err) <= 3.0, "step " << k << " order " << g_rig.steps[k].order << " step " << g_rig.steps[k].step << " off by " << err << " samples");
  }
}

TEST_CASE("chase: 29.97 drop-frame at MTC START hour 1 and hour 23: the song starts at the drop-frame hour, within 1 ms") {
  if (!kRate44100) {
    return;
  }
  const unsigned hours[2] = {1u, 23u};
  for (unsigned hi = 0; hi < 2u; ++hi) {
    const unsigned hour = hours[hi];
    chase_project();
    midi::MidiConfig c = chase_cfg();
    c.mtc_hour = static_cast<std::uint8_t>(hour);
    g_rig.init(c);
    g_rig.set_rate(30, 2);  // 29.97 drop-frame: the labels count 30 a second, the code is 2
    const double hour_q = qf_real_samples(4u * mtc::frames_of_label(mtc::Time{static_cast<std::uint8_t>(hour), 0, 0, 0}, 2), 2);
    g_rig.start_off = hour_q;  // the song's start is the drop-frame label hh:00:00:00, 3.6 ms short of the hour at hour 1 per hour
    g_rig.master_jumps(0.0, hour_q - 20000.0);  // 20,000 samples of pre-roll before it
    g_rig.run_to(20000 + 3 * 44100);
    CHECK(g_rig.link->stats().chase_starts == 1u);
    NEED(g_rig.n_steps > 8u);
    CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::Locked));
    for (std::uint32_t k = 0; k < g_rig.n_steps; ++k) {
      const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
      CHECK_MESSAGE(abs_d(err) <= 44.1, "hour " << hour << " step " << k << " off by " << err << " samples");  // the 1 ms acceptance
    }
  }
}

TEST_CASE("chase: a master crossing 24:00:00:00 mid-run keeps its phase (the master's day wraps)") {
  if (!kRate44100) {
    return;
  }
  // A long song (255 slots of a 128-step pattern, over an hour at 120 BPM) so the run is still going when the master's day wraps.
  fresh_project(2, 2);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 128) == model::Status::Ok);
  for (unsigned i = 0; i < 254u; ++i) {
    CHECK(p.append_order(1) == model::Status::Ok);
  }
  midi::MidiConfig c = chase_cfg();
  c.mtc_hour = 23;  // the song starts at 23:00:00:00, one hour before the wrap
  g_rig.init(c);
  const double day = qf_real_samples(mtc::quarter_frames_per_day_of_code(1), 1);
  g_rig.start_off = 23.0 * kHour;
  g_rig.master_jumps(0.0, day - 10.0 * 44100.0);  // the master is 10 s before 24:00:00:00 and runs on across it
  g_rig.run_to(10 * 44100 + 2 * 44100);  // the wrap happens at local 10*44100
  CHECK(g_rig.link->stats().chase_starts == 1u);
  NEED(g_rig.n_steps > 8u);
  CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::Locked));
  for (std::uint32_t k = 0; k < g_rig.n_steps; ++k) {
    const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
    CHECK_MESSAGE(abs_d(err) <= 3.0, "step " << k << " order " << g_rig.steps[k].order << " step " << g_rig.steps[k].step << " off by " << err << " samples");
  }
}

TEST_CASE("chase: a locate, then the master starting more than 2 s later, still settles before the start") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  g_rig.init(chase_cfg());
  g_rig.silent = true;
  g_rig.run_to(2000);
  // A full frame while stopped: a locate (it asks too, and must not anchor the settle max).
  g_rig.push_full_frame_now();
  g_rig.run_to(2000 + 3 * 44100);  // more than 2 s of silence after the locate
  CHECK(g_rig.link->stats().chase_locates == 1u);
  // The master starts now (a relocation for the idle chaser) and runs: the start must go through the settle window, not the stale max.
  g_rig.silent = false;
  g_rig.master_jumps(static_cast<double>(g_rig.sample), kHour - 20000.0);
  g_rig.run_to(g_rig.sample + 20000 + 3 * 44100);
  CHECK(g_rig.link->stats().chase_starts == 1u);
  NEED(g_rig.n_steps > 8u);
  CHECK(g_rig.link->status().mtc_state == static_cast<std::uint8_t>(midi::MtcState::Locked));
  for (std::uint32_t k = 0; k < g_rig.n_steps; ++k) {
    const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
    CHECK_MESSAGE(abs_d(err) <= 3.0, "step " << k << " off by " << err << " samples");
  }
}

TEST_CASE("chase: a relocation after a parked master re-anchors the settle max, the 100 ms hold still runs") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  model::Project& p4 = seq_project();
  CHECK(p4.append_order(1) == model::Status::Ok);
  CHECK(p4.append_order(2) == model::Status::Ok);  // 4 slots (7.6 s): room for the re-started run
  g_rig.init(chase_cfg());
  g_rig.master_jumps(0.0, kHour - 20000.0);
  g_rig.run_to(20000 + 2 * 44100);
  NEED(g_rig.n_steps > 8u);
  CHECK(g_rig.link->stats().chase_starts == 1u);
  // Relocate, then park the master (silent) for more than the 2 s settle max: no start goes out while parked, the anchor stays on the first relocation.
  g_rig.master_jumps(static_cast<double>(g_rig.sample), kHour + 1.0 * 44100.0);
  g_rig.silent = true;
  g_rig.run_to(g_rig.sample + 3 * 44100);
  const std::uint32_t n1 = g_rig.n_steps;  // the first run has stopped by now
  // The master comes back at a new time: a second relocation. Its settle must re-anchor, not ride the stale max.
  const std::uint64_t r2 = g_rig.sample;
  g_rig.silent = false;
  g_rig.master_jumps(static_cast<double>(r2), kHour + 3.0 * 44100.0);
  g_rig.run_to(r2 + 20000 + 4 * 44100);
  CHECK(g_rig.link->stats().chase_starts == 2u);
  NEED(g_rig.n_steps > n1 + 4u);
  // The hold ran: the re-start is at least the 160 frames of re-convergence plus the 100 ms hold after the second relocation.
  CHECK(g_rig.steps[n1].t - r2 >= 76000u);
  for (std::uint32_t k = n1; k < g_rig.n_steps; ++k) {
    const double err = static_cast<double>(g_rig.steps[k].t) - expected_sample(g_rig, g_rig.steps[k]);
    CHECK_MESSAGE(abs_d(err) <= 3.0, "step " << k << " order " << g_rig.steps[k].order << " step " << g_rig.steps[k].step << " off by " << err << " samples");
  }
}

TEST_CASE("chase: the rate trim is the identity at 1.0 and divides the step by the rate; swing keeps its ratios") {
  if (!kRate44100) {
    return;
  }
  chase_project();
  seq::Sequencer::Config cfg;
  cfg.start_lead = 64;
  seq::Sequencer& a = seq_slot().make(seq_project());
  a.configure(cfg);
  CHECK(a.rate_trim() == seq::Sequencer::kTrimOne);
  a.set_rate_trim(10);        // clamped to 0.94
  CHECK(a.rate_trim() == seq::Sequencer::kTrimMin);
  a.set_rate_trim(1000000);   // clamped to 1.06
  CHECK(a.rate_trim() == seq::Sequencer::kTrimMax);
  a.set_rate_trim(seq::Sequencer::kTrimOne);
  Collector& col = seq_collector();
  col.n = 0;
  a.play_pattern(1);
  col.run(a, 0, 3 * 5513, 64);
  const Rec* s1 = col.nth(EventKind::Step, 1);
  NEED(s1 != nullptr);
  CHECK(s1->t == 64u + 5513u);  // 5512.5 rounds to 5513 (the second step starts at 5512.5 after the first)
  // 2 % faster: the second step starts at 5512.5 / 1.02 = 5404.4
  seq::Sequencer& b = seq_slot().make(seq_project());
  b.configure(cfg);
  b.set_rate_trim(static_cast<std::uint32_t>(65536.0 * 1.02));
  col.n = 0;
  b.play_pattern(1);
  col.run(b, 0, 3 * 5513, 64);
  const Rec* t1 = col.nth(EventKind::Step, 1);
  NEED(t1 != nullptr);
  const double dt = static_cast<double>(t1->t) - 64.0;
  CHECK(abs_d(dt - 5512.5 / 1.02) <= 1.0);
}
