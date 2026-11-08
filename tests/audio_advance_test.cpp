// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-ODD11 slice 2, the closed-form advance (voices.h GATE, CLOSED-FORM ADVANCE): a voice of an inaudible strip that is a one shot or a forward loop does not run its sample loop; its position, its
// amplitude, its envelope and its end are advanced in closed form and must be, bit for bit, where the rendered voice is. Two pools get the same notes and events in the same spans; the second one is
// gated for some blocks. After every block the voice's state (VoicePool::Probe) is equal, a one shot stops in the same block, and after the gate lifts the output is the rendered pool's (but for the
// 32-sample re-entry ramp of the first block). The voices that are not of that kind (backward, ping-pong, granular, wavetable, a residual tail) are never advanced, and are equal too.
// What the closed form does NOT keep, by design (voices.h `last` is 0 while a voice is advanced): a gated stop or retrigger carries no residual tail, so a one shot that ends gated is dropped up to one block
// earlier than the rendered one, and a cut after a gated advance (the export's Engine::offline_quiet) leaves no tail in the stem's first block. Those cases are pinned below, not hidden.
#include <doctest/doctest.h>

#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os::audio;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr std::uint32_t kLen = 1500;
constexpr std::uint32_t kWin = 256;
constexpr unsigned kWindows = 4;

std::int16_t g_noise[kLen];  // white noise: a position that is off by a sample (or an interpolation fraction that is) changes the output

struct Lcg {
  std::uint32_t s;
  std::uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
};

void init_noise() {
  Lcg r{12345u};
  for (std::uint32_t i = 0; i < kLen; ++i) {
    g_noise[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(r.next() & 0xFFFFu) - 32768);
  }
}

o3os::test::StaticSlot<VoicePool> slot_x;  // the reference: never gated
o3os::test::StaticSlot<VoicePool> slot_y;  // the pool under test: gated in its windows

enum class Act : std::uint8_t { Off, On, Fade, Cut, Volume, LoopStart, LoopEnd, PerfOn, PerfOff, PerfOd, PerfBits, PerfShapeEnd };  // LoopStart / LoopEnd: a live instrument edit (OS-LIVEP / OS-MOD), `note` is the value; PerfOn / PerfOff: the Perform layer's low-pass at offset `note` (OS-PERFFX); PerfOd / PerfBits / PerfShapeEnd: its OVERDRV at `note` or BITDEPTH shift `note`, and the end of both (P1)
struct Ev {
  int block;
  std::uint32_t at;
  Act act;
  unsigned note;
};

constexpr unsigned kMaxEv = 6;
struct Plan {
  unsigned note = 60;
  unsigned vel = 127;
  int blocks = 40;
  int g0 = 2, g1 = 30;  // the blocks [g0, g1) run gated in the second pool
  int h0 = 0, h1 = 0;   // ... and a second window
  Ev evs[kMaxEv] = {};
  unsigned n_evs = 0;
  bool plain = true;    // the closed form is expected to be taken (when the voice sounds as the gate starts)
  std::uint32_t seed = 1;
  int pos = 0, endp = 0;  // OS-PERFFX P7: the layer's SMP POS and SMP END held (percent, 0: none) when the first note starts
  int adv_skip = -1;       // the one gated block (voice alive at its start) that may go without a closed-form span: the block that renders a residual tail
  void add(int block, std::uint32_t at, Act act, unsigned on_note = 0) {
    if (n_evs < kMaxEv) {
      evs[n_evs++] = Ev{block, at, act, on_note};
    }
  }
  bool gated(int b) const { return (b >= g0 && b < g1) || (b >= h0 && b < h1); }
};

void live_edit(VoicePool& p, unsigned field, unsigned value) {
  std::uint32_t pan = 0;
  CHECK(p.apply_param(0, field, static_cast<float>(value), pan));
}

// The Perform layer's ModSet of `group` on track 0 (OS-PERFFX P1): the layer's value, no step FX under it.
SeqEvent perf_shape(unsigned group, int value) {
  SeqEvent e{};
  e.kind = EventKind::ModSet;
  e.c = kLayerPerf;
  e.flags = static_cast<std::uint8_t>(group);
  e.value = value;
  return e;
}

void apply(VoicePool& p, const Ev& e) {
  switch (e.act) {
    case Act::Off: p.note_off(0); break;
    case Act::On: (void)p.note_on(0, e.note, 0, 127); break;
    case Act::Fade: p.fade(0); break;
    case Act::Cut: p.cut(0); break;
    case Act::Volume: live_edit(p, inst_param::Volume, e.note); break;
    case Act::LoopStart: live_edit(p, inst_param::LoopStart, e.note); break;
    case Act::LoopEnd: live_edit(p, inst_param::LoopEnd, e.note); break;
    case Act::PerfOn: p.set_filter_perf(0, kFilterLow, static_cast<int>(e.note)); break;
    case Act::PerfOff: p.clear_filter_perf(0); break;
    case Act::PerfOd: p.fx_event(0, perf_shape(kModGroupOverdrive, static_cast<int>(e.note))); break;
    case Act::PerfBits: p.fx_event(0, perf_shape(kModGroupBitDepth, static_cast<int>(e.note))); break;
    case Act::PerfShapeEnd:
      for (const unsigned group : {kModGroupOverdrive, kModGroupBitDepth}) {
        SeqEvent end{};
        end.kind = EventKind::ModEnd;
        end.flags = static_cast<std::uint8_t>(group);
        p.fx_event(0, end);
      }
      break;
  }
}

// One block of track 0: the spans cut at the plan's events (applied at their offsets, in order) and at `extra`.
void play_block(VoicePool& p, const Plan& pl, int b, std::uint32_t gate, std::uint32_t extra) {
  struct Cut {
    std::uint32_t at;
    int ev;  // the event, or -1: a cut and nothing else
  };
  Cut cuts[kMaxEv + 1];
  unsigned n = 0;
  for (unsigned k = 0; k < pl.n_evs; ++k) {
    if (pl.evs[k].block == b) {
      cuts[n++] = Cut{pl.evs[k].at < kBlk ? pl.evs[k].at : kBlk - 1u, static_cast<int>(k)};
    }
  }
  cuts[n++] = Cut{extra, -1};
  for (unsigned i = 1; i < n; ++i) {  // by offset (stable: events at one offset keep their order)
    const Cut c = cuts[i];
    unsigned j = i;
    for (; j > 0 && cuts[j - 1].at > c.at; --j) {
      cuts[j] = cuts[j - 1];
    }
    cuts[j] = c;
  }
  p.set_gate(gate);
  p.begin_block();
  std::uint32_t cursor = 0;
  for (unsigned i = 0; i < n; ++i) {
    if (cuts[i].at > cursor) {
      p.render(cursor, cuts[i].at);
      cursor = cuts[i].at;
    }
    if (cuts[i].ev >= 0) {
      apply(p, pl.evs[cuts[i].ev]);
    }
  }
  if (cursor < kBlk) {
    p.render(cursor, kBlk);
  }
  p.end_block();
}

bool alive(const VoicePool& p, VoicePool::Probe& pr) { return p.probe(0, pr) && pr.playing; }

bool same(const VoicePool::Probe& a, const VoicePool::Probe& b) {
  return a.phase == b.phase && a.inc == b.inc && a.amp == b.amp && a.gain == b.gain && a.gain_prev == b.gain_prev && a.shape == b.shape && a.ramp_left == b.ramp_left && a.env_left == b.env_left &&
         a.env_stage == b.env_stage && a.releasing == b.releasing;
}

// Runs the plan: the descriptor is in slot 0 of both pools, track 0 plays it.
void run_plan(const SampleDesc& d, const Plan& pl, const char* what) {
  VoicePool& x = slot_x.make();
  VoicePool& y = slot_y.make();
  x.set_sample(0, d);
  y.set_sample(0, d);
  for (VoicePool* q : {&x, &y}) {
    q->perf_clear();
    if (pl.pos != 0) {
      q->fx_event(0, perf_shape(kModGroupPosition, pl.pos));
    }
    if (pl.endp != 0) {
      q->fx_event(0, perf_shape(kModGroupSampleEnd, pl.endp));
    }
  }
  NEED(x.note_on(0, pl.note, 0, pl.vel));
  NEED(y.note_on(0, pl.note, 0, pl.vel));
  Lcg rng{pl.seed * 2654435761u + 7u};
  bool was_alive = true;     // the voice sounded at the start of this block (in the reference)
  int bad = 0;
  for (int b = 0; b < pl.blocks && bad < 3; ++b) {
    const bool g = pl.gated(b);
    const std::uint32_t extra = 1u + rng.next() % (kBlk - 1u);
    play_block(x, pl, b, 0u, extra);
    const std::uint32_t spans0 = y.stats().advanced_spans;
    play_block(y, pl, b, g ? 1u : 0u, extra);
    if (pl.plain && g && was_alive && b != pl.adv_skip) {
      // Every gated block in which the voice sounded at its start took the closed form at least once (a voice that stops in the block stops in its span). Per block: a total over the run would pass
      // with half the blocks rendered, since every block has two spans or more (the cut at `extra`).
      const std::uint32_t took = y.stats().advanced_spans - spans0;
      if (took == 0u) {
        ++bad;
      }
      CHECK_MESSAGE(took >= 1u, what << ": block " << b << " is gated, the voice sounded at its start, and no span of it was advanced");
    }
    VoicePool::Probe px{};
    VoicePool::Probe py{};
    const bool ax = alive(x, px);
    const bool ay = alive(y, py);
    const bool ok = ax == ay && (!ax || same(px, py));
    if (!ok) {
      ++bad;
    }
    CHECK_MESSAGE(ok, what << ": block " << b << " (gated " << g << "): alive " << ax << "/" << ay << " phase " << px.phase << "/" << py.phase << " inc " << px.inc << "/" << py.inc << " amp " << px.amp << "/"
                           << py.amp << " ramp_left " << px.ramp_left << "/" << py.ramp_left << " env " << int(px.env_stage) << "/" << int(py.env_stage) << " left " << px.env_left << "/" << py.env_left
                           << " shape " << px.shape << "/" << py.shape << " resid " << px.resid << "/" << py.resid << " last " << px.last << "/" << py.last);
    const std::int16_t* const fy = y.feed(0);
    const std::int16_t* const fx = x.feed(0);
    if (g) {
      if (was_alive) {  // (a residual tail of a voice that stopped before the gate is fed, as in slice 1)
        CHECK_MESSAGE(fy == nullptr, what << ": block " << b << " is fed while gated");
      }
    } else if (was_alive) {
      // Rendered again: the output is the reference's; the first block after a gate has the re-entry ramp over its first 32 samples.
      const bool relifted = b > 0 && pl.gated(b - 1);
      CHECK_MESSAGE((fx != nullptr) == (fy != nullptr), what << ": block " << b << " fed " << (fx != nullptr) << "/" << (fy != nullptr));
      if (fx != nullptr && fy != nullptr) {
        const std::uint32_t from = relifted ? 32u : 0u;
        const bool same_out = std::memcmp(fx + from, fy + from, (kBlk - from) * sizeof(std::int16_t)) == 0;
        if (!same_out) {
          ++bad;
        }
        CHECK_MESSAGE(same_out, what << ": block " << b << " output differs from the reference (from sample " << from << ")");
      }
    }
    was_alive = ax;
  }
  CHECK(x.stats().advanced_spans == 0u);
  CHECK(x.stats().gated_blocks == 0u);
  if (!pl.plain) {
    CHECK_MESSAGE(y.stats().advanced_spans == 0u, what << ": a voice of another kind was advanced");
    CHECK_MESSAGE(y.stats().gated_blocks > 0u, what << ": never gated");
  }
}

bool silent(const std::int16_t* f) {
  for (std::uint32_t i = 0; f != nullptr && i < kBlk; ++i) {
    if (f[i] != 0) {
      return false;
    }
  }
  return true;
}

bool all_zero(const std::int16_t* f, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    if (f[i] != 0) {
      return false;
    }
  }
  return true;
}

// Both feeds are absent, or both are there and equal from sample `from` on.
bool feed_same(const std::int16_t* a, const std::int16_t* b, std::uint32_t from) {
  if (a == nullptr || b == nullptr) {
    return a == b;
  }
  return std::memcmp(a + from, b + from, (kBlk - from) * sizeof(std::int16_t)) == 0;
}

SampleDesc oneshot(std::uint32_t len) {
  SampleDesc d{g_noise, len, 0, 0, 65536};
  return d;
}

SampleDesc looped(std::uint32_t from, std::uint32_t to) {
  SampleDesc d{g_noise, kLen, from, to, 65536};
  return d;
}

}  // namespace

TEST_CASE("closed form: a one shot is where the rendered one is, and stops in the same block, at every pitch") {
  init_noise();
  const unsigned notes[] = {12, 24, 36, 48, 60, 66, 72, 84, 96, 108, 120};
  const std::uint32_t lens[] = {40, 300, 1500};
  for (const unsigned note : notes) {
    for (const std::uint32_t len : lens) {
      for (const int g0 : {0, 2}) {
        INFO("one shot: note " << note << ", length " << len << ", gate from " << g0);
        Plan pl;
        pl.note = note;
        pl.blocks = 48;
        pl.g0 = g0;
        pl.g1 = 40;  // a one shot that ends inside the window stays stopped past it
        pl.seed = note * 7u + len;
        run_plan(oneshot(len), pl, "one shot");
      }
    }
  }
}

TEST_CASE("closed form: a forward loop wraps as the rendered loop does, with a wrap of many loops a block") {
  init_noise();
  const std::uint32_t loops[][2] = {{0, kLen}, {0, 2}, {0, 3}, {5, 8}, {100, 117}, {400, kLen}, {kLen - 2, kLen}, {0, 64}, {0, 65}, {13, 1013}};
  const unsigned notes[] = {24, 36, 48, 55, 60, 64, 72, 84, 96, 120};
  for (const auto& lp : loops) {
    for (const unsigned note : notes) {
      Plan pl;
      pl.note = note;
      pl.blocks = 36;
      pl.g0 = 2;
      pl.g1 = 24;
      pl.h0 = 28;  // a second gate window: the state goes on across the first re-entry
      pl.h1 = 33;
      pl.seed = note + lp[1];
      run_plan(looped(lp[0], lp[1]), pl, "loop");
    }
  }
}

TEST_CASE("closed form: the note's attack ramp, a release and a retrigger run in the gated blocks") {
  init_noise();
  for (const std::uint32_t at : {0u, 1u, 17u, 31u, 32u, 63u}) {
    Plan pl;
    pl.blocks = 30;
    pl.g0 = 0;  // gated from the note's first sample: the attack ramp runs in closed form
    pl.g1 = 22;
    pl.add(5, at, Act::Off);
    pl.seed = at + 3u;
    run_plan(looped(10, 900), pl, "release");
  }
  for (const std::uint32_t at : {0u, 9u, 40u}) {
    Plan pl;
    pl.blocks = 30;
    pl.g0 = 1;
    pl.g1 = 20;
    pl.add(4, at, Act::Off);
    pl.add(8, at, Act::On, 67);  // a new note while gated: its own attack from silence, the old one's tail is not carried
    pl.add(12, at + 3u, Act::Off);
    pl.seed = at + 11u;
    run_plan(looped(0, kLen), pl, "retrigger");
  }
  // NoteFade: 300 ms, 207 blocks. start_release rounds the step up (1 Q16 unit is the least): 5 x 13230 samples is more than the 65536 it falls from, so the amplitude is 0 for the last 122 samples and
  // the fade's span that crosses 0 is the one where advance()'s final clamp holds it (a mutant without that clamp leaves a negative amplitude and fails the probe). Gated through the end (block 209).
  Plan fade;
  fade.blocks = 230;
  fade.g0 = 3;
  fade.g1 = 215;
  fade.add(2, 40, Act::Fade);
  run_plan(looped(0, kLen), fade, "fade");
  // A cut while gated stops the voice for good.
  Plan cut;
  cut.blocks = 20;
  cut.g0 = 1;
  cut.g1 = 15;
  cut.add(6, 22, Act::Cut);
  run_plan(looped(0, kLen), cut, "cut");
}

TEST_CASE("closed form: an enveloped voice keeps its stage, its level and its release through the gated blocks") {
  init_noise();
  struct Env {
    std::uint16_t delay, attack, hold, decay, release;
    float sustain;
  };
  const Env envs[] = {
      {1, 1, 1, 1, 1, 0.5f},          {3, 20, 5, 40, 60, 0.4f},      {50, 100, 30, 200, 300, 0.7f}, {8, 8, 8, 8, 8, 0.0f},
      {200, 400, 1, 1, 600, 1.0f},    {7, 3, 11, 5, 13, 0.25f},      {1, 90, 2, 90, 90, 0.9f},      {2, 2, 2, 700, 40, 0.1f},
  };
  unsigned k = 0;
  for (const Env& e : envs) {
    for (const bool off : {false, true}) {
      for (const unsigned vel : {127u, 50u}) {
        SampleDesc d = looped(20, 1200);
        d.env.enable = true;
        d.env.delay = e.delay;
        d.env.attack = e.attack;
        d.env.hold = e.hold;
        d.env.decay = e.decay;
        d.env.release = e.release;
        d.env.sustain = static_cast<std::uint32_t>(e.sustain * static_cast<float>(kEnvOne));
        d.env.amount = (k & 1u) != 0u ? 40000 : 65536;
        Plan pl;
        pl.note = 48u + (k % 5u) * 7u;
        pl.vel = vel;
        pl.blocks = 90;
        pl.g0 = 0;
        pl.g1 = 60;
        pl.h0 = 70;
        pl.h1 = 80;
        if (off) {
          pl.add(3 + static_cast<int>(k % 4u), (k * 13u) % kBlk, Act::Off);
        }
        pl.seed = k + 100u;
        run_plan(d, pl, "envelope");
        ++k;
      }
    }
  }
}

TEST_CASE("closed form: a finetune LFO, a finetune ADSR and a glide move the increment through the gated blocks") {
  init_noise();
  for (const unsigned rate : {0u, 3u, 8u, 14u}) {
    SampleDesc d = looped(0, kLen);
    d.tune_mod.kind = kFmodLfo;
    d.tune_mod.shape = static_cast<std::uint8_t>(rate % 5u);
    d.tune_mod.rate = static_cast<std::uint8_t>(rate);
    d.tune_mod.amount = 1.0f;
    Plan pl;
    pl.blocks = 40;
    pl.g0 = 2;
    pl.g1 = 33;
    pl.seed = rate;
    run_plan(d, pl, "tune lfo");
  }
  {
    SampleDesc d = looped(0, kLen);
    d.tune_mod.kind = kFmodEnv;
    d.tune_mod.amount = 1.0f;
    d.tune_mod.sustain = 0.3f;
    d.tune_mod.attack = 40;
    d.tune_mod.decay = 60;
    d.tune_mod.release = 50;
    Plan pl;
    pl.blocks = 40;
    pl.g0 = 1;
    pl.g1 = 30;
    pl.add(20, 10, Act::Off);
    run_plan(d, pl, "tune adsr");
  }
  {
    SampleDesc d = looped(0, kLen);
    d.glide = 800;  // ms: the second note slides from the first one's pitch
    Plan pl;
    pl.note = 48;
    pl.blocks = 36;
    pl.g0 = 1;
    pl.g1 = 30;
    pl.add(4, 9, Act::On, 72);
    pl.add(14, 30, Act::On, 40);
    run_plan(d, pl, "glide");
  }
}

TEST_CASE("closed form: the voices of the other kinds are never advanced, and are where the reference is") {
  init_noise();
  for (const unsigned mode : {1u, 2u}) {  // backward, ping-pong
    SampleDesc d = looped(40, 700);
    d.loop_mode = static_cast<std::uint8_t>(mode);
    for (const unsigned note : {48u, 60u, 84u}) {
      Plan pl;
      pl.note = note;
      pl.blocks = 30;
      pl.g0 = 2;
      pl.g1 = 24;
      pl.plain = false;
      run_plan(d, pl, mode == 1u ? "backward" : "ping-pong");
    }
  }
  for (const unsigned type : {0u, 1u, 2u}) {  // granular, the three grain types
    SampleDesc d = oneshot(kLen);
    d.granular = true;
    d.gr_len = 300;
    d.gr_pos = 20000;
    d.gr_shape = 2;
    d.loop_mode = static_cast<std::uint8_t>(type);
    Plan pl;
    pl.blocks = 30;
    pl.g0 = 2;
    pl.g1 = 24;
    pl.plain = false;
    run_plan(d, pl, "granular");
  }
  {  // a wavetable
    SampleDesc d{g_noise + kWin, kWin, 0, kWin, 65536};  // window 1 of a stack of four
    d.wt_size = kWin;
    d.wt_windows = kWindows;
    d.wt_pos = 1;
    Plan pl;
    pl.blocks = 30;
    pl.g0 = 2;
    pl.g1 = 24;
    pl.plain = false;
    run_plan(d, pl, "wavetable");
  }
}

TEST_CASE("closed form: a voice that holds a residual tail renders until the tail is out, then is advanced") {
  init_noise();
  // The retrigger at 40 of block 5 leaves a residual (the old note's last sample fading over 64 samples) that runs into block 6, where the gate starts.
  Plan pl;
  pl.blocks = 24;
  pl.g0 = 6;
  pl.g1 = 20;
  pl.add(5, 40, Act::On, 64);
  pl.adv_skip = 6;  // block 6 (the gate's first) may run entirely under the tail
  run_plan(looped(0, kLen), pl, "residual");
  // Its gated blocks were taken by the closed form once the tail was out (the first gated block rendered).
  VoicePool& y = slot_y.make();
  y.set_sample(0, looped(0, kLen));
  NEED(y.note_on(0, 60, 0, 127));
  for (int b = 0; b < 5; ++b) {
    y.set_gate(0);
    y.begin_block();
    y.render(0, kBlk);
    y.end_block();
  }
  y.set_gate(0);
  y.begin_block();
  y.render(0, 40);
  NEED(y.note_on(0, 64, 0, 127));  // the retrigger: the old note's last sample is a residual
  y.render(40, kBlk);
  y.end_block();
  y.set_gate(1u);
  y.begin_block();
  y.render(0, kBlk);
  y.end_block();
  CHECK(y.stats().advanced_spans == 0u);  // the tail was still running at the start of this block: rendered
  y.set_gate(1u);
  y.begin_block();
  y.render(0, kBlk);
  y.end_block();
  CHECK(y.stats().advanced_spans == 1u);  // ... and the next block is advanced
  CHECK(y.feed(0) == nullptr);
}

TEST_CASE("closed form: a gated voice's block is not touched, and the voice is not fed") {
  init_noise();
  VoicePool& y = slot_y.make();
  y.set_sample(0, looped(0, kLen));
  NEED(y.note_on(0, 60, 0, 127));
  y.set_gate(0);
  y.begin_block();
  y.render(0, kBlk);
  y.end_block();
  CHECK(y.feed(0) != nullptr);
  for (int b = 0; b < 4; ++b) {
    y.set_gate(1u);
    y.begin_block();
    y.render(0, 20);  // two spans: the pan / finetune modulators and the block's bookkeeping are once a block
    y.render(20, kBlk);
    y.end_block();
    CHECK(y.feed(0) == nullptr);
  }
  CHECK(y.stats().advanced_spans == 8u);
  CHECK(y.stats().gated_blocks == 4u);
  CHECK(y.voices_in_use() == 1u);  // a gated voice still counts
  y.set_gate(0);
  y.begin_block();
  y.render(0, kBlk);
  y.end_block();
  CHECK(y.feed(0) != nullptr);  // the gate lifted: rendered and fed again
}

// ---- what the closed form leaves out on purpose: no tail from a gated stop or retrigger (`last` is 0 while advanced) ----------------------------------------------------------------------

TEST_CASE("closed form: a one shot that ends while gated leaves no tail (by design): silent when the gate lifts, resid 0, the next note is the reference's") {
  init_noise();
  unsigned reference_tails = 0;  // configurations whose rendered reference held a residual tail after the end
  unsigned heard = 0;            // ... and was still audible in the block the gate lifted: the closed form drops it (up to one block earlier)
  for (const unsigned note : {36u, 60u, 84u}) {
    for (const std::uint32_t len : {200u, 700u, 1400u}) {
      INFO("one shot: note " << note << ", length " << len);
      int e = -1;  // the block in which the reference ends
      {
        VoicePool& x = slot_x.make();
        x.set_sample(0, oneshot(len));
        NEED(x.note_on(0, note, 0, 127));
        const Plan none;
        for (int b = 0; b < 200 && e < 0; ++b) {
          play_block(x, none, b, 0u, 1u + (static_cast<std::uint32_t>(b) * 7u + note) % (kBlk - 1u));
          VoicePool::Probe px{};
          if (!alive(x, px)) {
            e = b;
          }
        }
      }
      NEED(e >= 0);
      const int g0 = e >= 1 ? 1 : 0;  // gated from the second block when there is one: the voice sounded (and put its samples out) before it was muted
      Plan pl;
      pl.note = note;
      pl.add(e + 3, 5, Act::On, note);  // a new note two blocks after the lift: both pools sound it from the same state
      VoicePool& x = slot_x.make();
      VoicePool& y = slot_y.make();
      x.set_sample(0, oneshot(len));
      y.set_sample(0, oneshot(len));
      NEED(x.note_on(0, note, 0, 127));
      NEED(y.note_on(0, note, 0, 127));
      for (int b = 0; b <= e + 5; ++b) {
        const std::uint32_t extra = 1u + (static_cast<std::uint32_t>(b) * 7u + note) % (kBlk - 1u);
        play_block(x, pl, b, 0u, extra);
        play_block(y, pl, b, b >= g0 && b <= e ? 1u : 0u, extra);  // gated through the block the one shot ends in, open from the next
        VoicePool::Probe px{};
        VoicePool::Probe py{};
        (void)x.probe(0, px);
        (void)y.probe(0, py);
        if (b == e) {
          CHECK_FALSE(px.playing);
          CHECK_FALSE(py.playing);
          CHECK_MESSAGE(py.resid == 0, "the gated stop carries no tail: resid " << py.resid);
          CHECK(py.last == 0);
          reference_tails += px.resid != 0 ? 1u : 0u;
        } else if (b == e + 1) {  // the gate lifts: the reference still fades out, the advanced voice has nothing to say
          CHECK_MESSAGE(y.feed(0) == nullptr, "the lift block is fed, resid " << py.resid);
          CHECK(py.resid == 0);
          heard += silent(x.feed(0)) ? 0u : 1u;
        } else if (b >= e + 3) {
          const bool state_ok = px.playing == py.playing && (!px.playing || same(px, py));  // (a short one shot may have ended again)
          CHECK_MESSAGE(state_ok, "block " << b << ": the new note differs");
          CHECK_MESSAGE(feed_same(x.feed(0), y.feed(0), 0u), "block " << b << ": the new note's output differs from the reference");
        }
      }
    }
  }
  CHECK_MESSAGE(reference_tails > 0u, "the reference never held a tail: the case pins nothing");
  CHECK_MESSAGE(heard > 0u, "the reference's tail was never audible in the lift block: the case pins nothing");
}

TEST_CASE("closed form: a retrigger in the block the gate lifts carries no tail when it falls on the first sample (by design), and the new note is the reference's") {
  init_noise();
  for (const std::uint32_t at : {0u, 1u, 17u, 33u, 63u}) {
    INFO("retrigger at " << at << " of the lift block");
    Plan pl;
    pl.add(10, at, Act::On, 67);
    VoicePool& x = slot_x.make();
    VoicePool& y = slot_y.make();
    x.set_sample(0, looped(0, kLen));
    y.set_sample(0, looped(0, kLen));
    NEED(x.note_on(0, 60, 0, 127));
    NEED(y.note_on(0, 60, 0, 127));
    for (int b = 0; b < 14; ++b) {
      const std::uint32_t extra = 1u + (static_cast<std::uint32_t>(b) * 11u + at) % (kBlk - 1u);
      play_block(x, pl, b, 0u, extra);
      play_block(y, pl, b, b >= 1 && b < 10 ? 1u : 0u, extra);  // gated 1..9, the lift is block 10
      VoicePool::Probe px{};
      VoicePool::Probe py{};
      NEED(x.probe(0, px));
      NEED(y.probe(0, py));
      if (b == 10) {
        CHECK_MESSAGE(same(px, py), "the new note's state differs after the retrigger");
        if (at == 0u) {
          // The reference's tail (the previous block's last sample, 64 samples) is spent inside this block; the advanced voice had put nothing out, so there is none: the block differs, by design.
          CHECK_MESSAGE(!feed_same(x.feed(0), y.feed(0), 0u), "the reference's tail is missing from the closed form's block 10: the case pins nothing");
          CHECK(px.resid == 0);
          CHECK(py.resid == 0);
        } else {
          // The span before the retrigger was rendered (under the re-entry ramp): its last sample is carried by both, and fades out into the next block.
          CHECK_MESSAGE(px.resid != 0, "the reference's tail");
          CHECK_MESSAGE(py.resid != 0, "the rendered span's last sample is carried (resid " << py.resid << ")");
        }
      } else if (b == 11) {  // the tails fade out over the first `at` samples of this block (they differ in size, not in kind)
        CHECK_MESSAGE(same(px, py), "block 11 state");
        CHECK_MESSAGE(feed_same(x.feed(0), y.feed(0), at), "block 11 output differs from sample " << at);
      } else if (b > 11) {
        CHECK_MESSAGE((same(px, py) && feed_same(x.feed(0), y.feed(0), 0u)), "block " << b << " differs from the reference");
      }
    }
  }
}

TEST_CASE("closed form: cut() of a gated plain voice carries no tail, so the first block lacks the reference's fade-out (pool level; Engine::offline_enter drops both)") {
  init_noise();
  VoicePool& x = slot_x.make();  // the baseline: the same notes, never gated
  VoicePool& y = slot_y.make();  // track 0 muted from block 2 to block 7
  x.set_sample(0, looped(0, kLen));
  y.set_sample(0, looped(0, kLen));
  NEED(x.note_on(0, 60, 0, 127));
  NEED(y.note_on(0, 60, 0, 127));
  const Plan none;
  for (int b = 0; b < 8; ++b) {
    play_block(x, none, b, 0u, 20);
    play_block(y, none, b, b >= 2 ? 1u : 0u, 20);  // (it sounded for two blocks first: its last sample is not 0 when the mute starts)
  }
  CHECK(y.stats().advanced_spans == 12u);
  // Engine::offline_quiet(): every track is cut. Then the export's block 0, which has no gate (Engine::render_offline).
  x.cut(0);
  y.cut(0);
  VoicePool::Probe px{};
  VoicePool::Probe py{};
  NEED(x.probe(0, px));  // (the baseline's voice still holds a tail, so the track has a voice)
  (void)y.probe(0, py);  // (the advanced voice holds none: no voice, py stays 0)
  CHECK_FALSE(px.playing);
  CHECK_FALSE(py.playing);
  CHECK(px.resid != 0);  // the baseline carries the last sample it put out into a tail ...
  CHECK(py.resid == 0);  // ... the advanced voice put nothing out
  play_block(x, none, 8, 0u, kBlk);
  play_block(y, none, 8, 0u, kBlk);
  CHECK_FALSE(silent(x.feed(0)));  // the first block of the stem differs from the baseline's, by the tail (at most kDeclickSamples = one block)
  CHECK(y.feed(0) == nullptr);
  play_block(x, none, 9, 0u, kBlk);
  play_block(y, none, 9, 0u, kBlk);
  CHECK(x.feed(0) == nullptr);  // the tail is out within that block: from block 1 on the stem is the baseline's
  CHECK(y.feed(0) == nullptr);
}

// ---- the edges of the closed form: live edits, and a voice taken in the middle of a block ------------------------------------------------------------------------------------------------

TEST_CASE("closed form: a live loop edit (apply_loop) on a gated looped voice leaves the voice where the rendered one is") {
  init_noise();
  Plan pl;
  pl.note = 72;  // two samples a sample: the position is well inside the loop and past a shortened end
  pl.blocks = 44;
  pl.g0 = 1;
  pl.g1 = 34;
  pl.h0 = 38;
  pl.h1 = 42;
  pl.add(5, 0, Act::LoopEnd, 600);    // shorter than the position: the lazy wrap takes the voice back
  pl.add(10, 20, Act::LoopStart, 300);
  pl.add(15, 0, Act::LoopEnd, 1400);  // longer again
  pl.add(20, 40, Act::LoopStart, 100);
  run_plan(looped(100, 1400), pl, "loop edit");
  Plan back;  // the loop edits while the gate is open for a block in between
  back.note = 60;
  back.blocks = 30;
  back.g0 = 2;
  back.g1 = 12;
  back.h0 = 14;
  back.h1 = 26;
  back.add(8, 0, Act::LoopEnd, 80);
  back.add(13, 31, Act::LoopEnd, 900);
  back.add(18, 0, Act::LoopStart, 5);
  run_plan(looped(0, 500), back, "loop edit 2");
  {  // the edit is not vacuous: a gated voice that is past the shortened end is back inside the loop one block later
    VoicePool& y = slot_y.make();
    y.set_sample(0, looped(100, 1400));
    NEED(y.note_on(0, 72, 0, 127));
    const Plan none;
    for (int b = 0; b < 8; ++b) {
      play_block(y, none, b, 1u, 30);
    }
    VoicePool::Probe before{};
    VoicePool::Probe after{};
    NEED(y.probe(0, before));
    CHECK_MESSAGE(before.phase >= (std::uint64_t{600} << 32), "the voice is not past 600 yet: " << (before.phase >> 32));
    live_edit(y, inst_param::LoopEnd, 600);
    play_block(y, none, 8, 1u, 30);
    NEED(y.probe(0, after));
    CHECK_MESSAGE(after.phase < (std::uint64_t{600} << 32), "the voice did not wrap to the new end: " << (after.phase >> 32));
  }
}

TEST_CASE("closed form: a live volume edit (regain) on a gated voice: the ramp, an envelope's next segment and a voice in its release") {
  init_noise();
  // (the notes take the instrument's own velocity: a note with a velocity of its own keeps its level, regain's rule, and the edit would do nothing)
  Plan up;
  up.vel = kVelocityDefault;
  up.blocks = 30;
  up.g0 = 2;
  up.g1 = 26;
  up.add(4, 33, Act::Volume, 30);  // down, mid-block: the voice ramps over kDeclickSamples from where it is, and the ramp straddles the block's end (the cap is the old level)
  up.add(9, 0, Act::Volume, 100);  // and up again
  up.add(14, 17, Act::Volume, 55);
  run_plan(looped(0, kLen), up, "volume");
  {  // the edit is not vacuous: the gain moves and the ramp over kDeclickSamples starts (and runs out in the gated blocks)
    VoicePool& y = slot_y.make();
    y.set_sample(0, looped(0, kLen));
    NEED(y.note_on(0, 60, 0, kVelocityDefault));
    const Plan none;
    for (int b = 0; b < 4; ++b) {
      play_block(y, none, b, 1u, 30);
    }
    VoicePool::Probe before{};
    VoicePool::Probe after{};
    NEED(y.probe(0, before));
    live_edit(y, inst_param::Volume, 30);
    NEED(y.probe(0, after));
    CHECK_MESSAGE(after.gain < before.gain, "gain " << before.gain << " -> " << after.gain);
    CHECK(after.ramp_left == kDeclickSamples);
    play_block(y, none, 4, 1u, 30);
    NEED(y.probe(0, after));
    CHECK(after.ramp_left == 0u);
  }
  Plan rel;  // a release in progress keeps its level (regain's rule)
  rel.vel = kVelocityDefault;
  rel.blocks = 30;
  rel.g0 = 2;
  rel.g1 = 26;
  rel.add(5, 10, Act::Off);
  rel.add(5, 40, Act::Volume, 20);
  run_plan(looped(0, kLen), rel, "volume in release");
  SampleDesc d = looped(20, 1200);  // an enveloped voice: the edit moves the target of the next segment
  d.env.enable = true;
  d.env.delay = 3;
  d.env.attack = 20;
  d.env.hold = 5;
  d.env.decay = 40;
  d.env.release = 60;
  d.env.sustain = static_cast<std::uint32_t>(0.4f * static_cast<float>(kEnvOne));
  d.env.amount = 65536;
  Plan env;
  env.vel = kVelocityDefault;
  env.blocks = 70;
  env.g0 = 1;
  env.g1 = 60;
  env.add(3, 0, Act::Volume, 40);
  env.add(12, 17, Act::Volume, 90);
  env.add(30, 0, Act::Volume, 10);
  env.add(36, 5, Act::Off);
  env.add(40, 0, Act::Volume, 70);
  run_plan(d, env, "volume enveloped");
}

TEST_CASE("closed form: a gated voice taken in the middle of a block (a steal, a note of another kind) leaves the new note the reference's, and no block of its own to the old") {
  init_noise();
  constexpr unsigned kThief = kMaxVoices;  // one track past the pool: a note on it steals the oldest voice, track 0's
  static_assert(kThief < kVoiceTracks, "the thief's track exists");
  struct Kind {
    const char* what;
    bool victim_pingpong;  // the gated voice is rendered (slice 1), not advanced
    unsigned thief_mode;   // 0: a forward loop (advanced when gated), 2: ping-pong
  };
  const Kind kinds[] = {{"loop takes the voice of a loop", false, 0}, {"ping-pong takes the voice of a loop", false, 2}, {"loop takes the voice of a ping-pong", true, 0}};
  for (const Kind& k : kinds) {
    for (const std::uint32_t at : {1u, 17u, 63u}) {
      INFO(k.what << ", at " << at);
      SampleDesc victim = looped(40, 900);
      victim.loop_mode = static_cast<std::uint8_t>(k.victim_pingpong ? 2u : 0u);
      SampleDesc thief = looped(10, 700);
      thief.loop_mode = static_cast<std::uint8_t>(k.thief_mode);
      for (SampleDesc* d : {&victim, &thief}) {  // a finetune LFO each: mod_block runs once a block for the voice, and the new note's first span must not be taken for a later one of the victim's block
        d->tune_mod.kind = kFmodLfo;
        d->tune_mod.shape = 1;
        d->tune_mod.rate = 8;
        d->tune_mod.amount = 1.0f;
      }
      VoicePool& x = slot_x.make();
      VoicePool& y = slot_y.make();
      for (VoicePool* p : {&x, &y}) {
        p->set_sample(0, victim);
        p->set_sample(1, thief);
        for (unsigned t = 0; t < kMaxVoices; ++t) {
          NEED(p->note_on(t, 48u + t * 2u, 0, 127));  // track 0 first: the oldest
        }
      }
      for (int b = 0; b < 10; ++b) {
        for (VoicePool* p : {&x, &y}) {
          p->set_gate(p == &y && b >= 2 ? 1u : 0u);  // y: track 0 muted from block 2 (its voice's block holds the samples of blocks 0 and 1: the steal must not feed them)
          p->begin_block();
          if (b == 5) {
            p->render(0, at);
            NEED(p->note_on(kThief, 60, 1, 127));
            p->render(at, kBlk);
          } else {
            p->render(0, kBlk);
          }
          p->end_block();
        }
        for (unsigned t = 1; t < kMaxVoices; ++t) {  // the other tracks are the reference's, whatever happens to track 0
          CHECK_MESSAGE(feed_same(x.feed(t), y.feed(t), 0u), "block " << b << ": track " << t << " differs");
        }
        if (b >= 5) {
          VoicePool::Probe px{};
          VoicePool::Probe py{};
          NEED(x.probe(kThief, px));
          NEED(y.probe(kThief, py));
          CHECK_MESSAGE((same(px, py) && px.playing && py.playing), "block " << b << ": the thief's state differs");
          CHECK_MESSAGE(y.feed(kThief) != nullptr, "block " << b << ": the thief is not fed");
          CHECK_MESSAGE(feed_same(x.feed(kThief), y.feed(kThief), b == 5 ? at : 0u), "block " << b << ": the thief's output differs");
          if (b == 5 && !k.victim_pingpong) {
            const std::int16_t* const fthief = y.feed(kThief);
            if (fthief == nullptr) {  // (also failed above; the block cannot be read, and a null must not crash the run)
              FAIL_CHECK("block 5: the thief has no feed, so the block before the steal cannot be checked");
            } else {
              CHECK_MESSAGE(all_zero(fthief, at), "the advanced victim wrote into the block before the steal");
            }
          }
          const bool track0_sounds = y.probe(0, py) && py.playing;
          CHECK_FALSE(track0_sounds);  // track 0 lost its voice
        }
      }
      CHECK(x.stats().steals == 1u);
      CHECK(y.stats().steals == 1u);
    }
  }
}

// OS-PERFFX P0: the Perform layer's filter arrives while the voice is advanced in closed form (a muted track is still under Perform). The closed form does not run the filter, so the state it keeps
// (position, amplitude, envelope) must be the rendered voice's whatever the layer does meanwhile; a layer that is gone again before the gate lifts leaves the output the reference's.
TEST_CASE("closed form: a performance filter held and released inside the gated window leaves the advance exact, and the output after the lift the reference's") {
  init_noise();
  for (const unsigned note : {48u, 60u, 72u}) {
    Plan pl;
    pl.note = note;
    pl.blocks = 40;
    pl.g0 = 2;
    pl.g1 = 30;
    pl.add(5, 0, Act::PerfOn, 30);
    pl.add(9, 40, Act::PerfOn, 70);  // the offset changes under the gate
    pl.add(20, 11, Act::PerfOff);
    pl.seed = note;
    run_plan(looped(10, 900), pl, "perf filter inside the window");
  }
  Plan pl;
  pl.blocks = 40;
  pl.g0 = 2;
  pl.g1 = 30;
  pl.add(5, 0, Act::PerfOn, 30);
  pl.add(20, 0, Act::PerfOff);
  run_plan(oneshot(kLen), pl, "perf filter on a one shot");
}

TEST_CASE("closed form: a performance filter held through the lift: the state is the reference's all along, and after the lift the voice is fed through the filter at the reference's cutoff") {
  init_noise();
  VoicePool& x = slot_x.make();
  VoicePool& y = slot_y.make();
  const SampleDesc d = looped(0, kLen);
  x.set_sample(0, d);
  y.set_sample(0, d);
  NEED(x.note_on(0, 60, 0, 127));
  NEED(y.note_on(0, 60, 0, 127));
  Plan pl;
  pl.add(6, 0, Act::PerfOn, 30);  // both pools get it at the same sample
  Lcg rng{99u};
  for (int b = 0; b < 26; ++b) {
    const bool gated = b >= 2 && b < 16;
    const std::uint32_t extra = 1u + rng.next() % (kBlk - 1u);
    play_block(x, pl, b, 0u, extra);
    play_block(y, pl, b, gated ? 1u : 0u, extra);
    VoicePool::Probe px{};
    VoicePool::Probe py{};
    NEED(alive(x, px));
    NEED(alive(y, py));
    CHECK_MESSAGE(same(px, py), "block " << b << ": the state differs from the reference's (gated " << gated << ")");
    if (b >= 17) {
      CHECK(y.feed(0) != nullptr);                      // the voice is fed (no longer gated) ...
      CHECK(y.filter_cutoff(0) == x.filter_cutoff(0));  // ... through the filter, at the reference's cutoff
    }
  }
}

// OS-PERFFX P1: the Perform layer's OVERDRV and BITDEPTH arrive while the voice is advanced in closed form (a muted track is still under Perform). The closed form skips the shaper, so the state it keeps
// is the rendered voice's whatever the layer does meanwhile; a layer that is gone again before the gate lifts leaves the reference's output, and one that is still there when it lifts is in the output
// (the same shaped block as the reference's, but for the 32-sample re-entry ramp).
TEST_CASE("closed form: OVERDRV and BITDEPTH set and released under the gate leave the advance exact, and the output after the lift the reference's, shaped as the reference is") {
  init_noise();
  for (const unsigned note : {48u, 60u, 72u}) {
    Plan pl;
    pl.note = note;
    pl.blocks = 40;
    pl.g0 = 2;
    pl.g1 = 30;
    pl.add(5, 0, Act::PerfOd, 70);
    pl.add(7, 40, Act::PerfBits, 9);
    pl.add(20, 11, Act::PerfShapeEnd);  // gone before the lift: the unshaped note comes back at block 30
    pl.seed = note;
    run_plan(looped(10, 900), pl, "perf shape inside the window");
  }
  Plan held;
  held.blocks = 40;
  held.g0 = 2;
  held.g1 = 30;
  held.add(1, 0, Act::PerfOd, 90);  // still there when the gate lifts, and set before it started
  held.add(5, 20, Act::PerfBits, 12);
  run_plan(looped(10, 900), held, "perf shape held through the lift");
  Plan shot;
  shot.blocks = 40;
  shot.g0 = 2;
  shot.g1 = 30;
  shot.add(5, 0, Act::PerfOd, 50);
  shot.add(20, 0, Act::PerfShapeEnd);
  run_plan(oneshot(kLen), shot, "perf shape on a one shot");
}

// OS-PERFFX P7: a note that starts at the layer's SMP POS (and ends at its SMP END) is a plain phase < end: the closed form takes it as it takes a note from frame 0, bit for bit.
TEST_CASE("closed form: a note started at the layer's SMP POS and SMP END is advanced exactly, a one shot stops where the rendered one does, a loop wraps from a start inside it") {
  init_noise();
  for (const unsigned note : {48u, 60u, 72u}) {
    Plan pl;
    pl.note = note;
    pl.blocks = 40;
    pl.pos = 20;
    pl.endp = 80;
    pl.seed = note;
    run_plan(oneshot(kLen), pl, "smp pos and end on a one shot");
    Plan lp;
    lp.note = note;
    lp.blocks = 40;
    lp.pos = -60;  // the last 60 %: inside the loop of 10..900 (loop_end 900, start at 40 % of it)
    lp.seed = note + 1u;
    run_plan(looped(10, 900), lp, "smp pos on a loop");
  }
  Plan late;
  late.blocks = 40;
  late.pos = -5;  // the last 5 % of the one shot: it stops in the first blocks, while gated
  late.add(10, 0, Act::On, 60);
  run_plan(oneshot(kLen), late, "smp pos near the end");
}
