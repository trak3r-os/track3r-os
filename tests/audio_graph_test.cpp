// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-005b: the fixed audio graph (audio/graph.h) driven block by block with known blocks, on the host and on the Cortex-M4F (`qemu_audio_graph`).
// The graph is a pure function of its input blocks and its parameter changes, so every case states what must come out:
//   * the routing truth table: every one of the 64 route masks of a track, observed at MAIN, CUE (through OUT MODE), both send buses, REC and the
//     USB stems, with the exact samples;
//   * the pan law and the two level laws against the laws of smoothing.h, this OS's own model (written out here as an independent double oracle)
//     and against PAR-023's anchors (documented in the release notes, not verified);
//   * ramps: a change lands on its target in the block, by a bounded step per sample, and multi-block ramps arrive exactly;
//   * the null tests (ADR 0013 D8): a track through the graph is the ideal product within one LSB, and a mix of twelve has no DC offset;
//   * the ParamChange ids and ranges, mute, solo (both readings of the sends: which one the official firmware does is Q, see graph.h Solo), sends (pre-fader, scaled by the master level);
//   * the reserved points: inserts, the send effects (bypass closes the input and lets the tail ring out) and the master chain, bypassed by default and
//     bit-transparent, and a chain that sees the sum before it is clipped;
//   * the leak of a closed route (mute, close MAIN, unmute), OUT MODE and the cue level; the input strip (monitoring), through the engine and through
//     the pipeline's slots (its latency in blocks); consumer-gated taps, maskable meters;
//   * the silence flag, configure(), the stage report.
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/pipeline.h>
#include <o3os/hal/audio.h>
#include <o3os/usb/stream.h>

#include "support/rng.h"
#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

namespace ha = o3os::hal::audio;
using namespace o3os::audio;
using o3os::test::Rng;
namespace gps = o3os::audio::graph_param::strip;
namespace gpm = o3os::audio::graph_param::master;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

Graph& make_graph() {
  static o3os::test::StaticSlot<Graph> slot;  // not on the target's stack
  static TapMem taps;
  g_cycles = 0;
  return slot.make(&fake_cycles, taps);
}

struct Rig {
  Graph& g;
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  alignas(8) std::int16_t feed_[kTracks][kB] = {};
  ha::BlockReport report = {};
  explicit Rig(Graph& graph) : g(graph) {}

  void render(std::uint32_t flags = 0) {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, flags, &report};
    g.render(io);
  }
  // A ParamChange; `ramp` 1 is instant, so a case does not wait for a ramp it is not about.
  void set(o3os::core::ParamId id, float v, std::uint32_t ramp = 1) { CHECK(g.params().push(ParamChange{id, v, ramp})); }
  void strip(unsigned slot, unsigned field, float v, std::uint32_t ramp = 1) { set(strip_param(slot, field), v, ramp); }
  void master(unsigned field, float v, std::uint32_t ramp = 1) { set(master_param(field), v, ramp); }
  // The feed block of a track (the graph copies it each block): switches the track's source to Feed and hands the block back to be filled.
  std::int16_t* fb(unsigned t) {
    g.feed(t, feed_[t]);
    return feed_[t];
  }
  std::int16_t l(unsigned i) const { return out[2u * i]; }
  std::int16_t r(unsigned i) const { return out[2u * i + 1u]; }
  bool silent() const {
    for (std::uint32_t i = 0; i < kBS; ++i) {
      if (out[i] != 0) return false;
    }
    return true;
  }
};

// A track fed from `rig.fb(t)`: the source set, the block returned.
std::int16_t* use_feed(Rig& rig, unsigned t) {
  rig.strip(t, gps::Source, 3.0f);
  return rig.fb(t);
}

// A block of random samples within +-amp.
void fill(std::int16_t* block, Rng& rng, std::int32_t amp) {
  for (std::uint32_t i = 0; i < kB; ++i) {
    block[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(static_cast<std::uint32_t>(2 * amp + 1))) - amp);
  }
}

// (x * g) >> 16 with g a Q16 gain: the arithmetic of the test doubles below (inserts and effects), not of the graph's kernels.
std::int32_t gained(std::int32_t x, std::int32_t g) { return static_cast<std::int32_t>((static_cast<std::int64_t>(x) * g) >> 16); }

// The oracle in double (this OS's pan law and level law). pan 0..100: the left jack's gain and the right jack's; level 0..100: 10^((v-100)/50).
double oracle_left(double pan) { return pan > 50.0 ? (100.0 - pan) / 50.0 : 1.0; }
double oracle_right(double pan) { return pan < 50.0 ? pan / 50.0 : 1.0; }
double oracle_level_1114(int v) { return v <= 0 ? 0.0 : std::pow(10.0, (static_cast<double>(v) - 100.0) / 50.0); }
double absd(double v) { return v < 0 ? -v : v; }
double rail(double v) { return v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v); }

// A send / insert effect that records what it was given.
struct FxLog {
  alignas(8) std::int16_t last[kB] = {};
  std::uint32_t calls = 0;
  std::int32_t ret_gain = 0;  // the return is last * ret_gain / 65536 on both channels (0: silence)
};
void fx_record(void* state, const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept {
  FxLog& f = *static_cast<FxLog*>(state);
  ++f.calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    f.last[i] = in[i];
    const std::int16_t v = static_cast<std::int16_t>(gained(in[i], f.ret_gain));
    out_l[i] = v;
    out_r[i] = v;
  }
}
// An effect with a memory of one block: its return is what it was given the block before (a tail that rings out for a block).
struct TailFx {
  alignas(8) std::int16_t prev[kB] = {};
  alignas(8) std::int16_t last_in[kB] = {};
  std::uint32_t calls = 0;
};
void fx_tail(void* state, const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept {
  TailFx& f = *static_cast<TailFx*>(state);
  ++f.calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    out_l[i] = f.prev[i];
    out_r[i] = f.prev[i];
    f.prev[i] = in[i];
    f.last_in[i] = in[i];
  }
}

struct InsertLog {
  std::uint32_t calls = 0;
  std::uint32_t my_order = 0;
  std::int32_t gain = 65536;
  std::uint32_t* shared = nullptr;
};
void insert_scale(void* state, std::int16_t* io, std::uint32_t n) noexcept {
  InsertLog& f = *static_cast<InsertLog*>(state);
  ++f.calls;
  if (f.shared != nullptr) {
    f.my_order = ++*f.shared;
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    io[i] = static_cast<std::int16_t>(gained(io[i], f.gain));
  }
}

struct StageLog {
  std::uint32_t calls = 0;
  std::uint32_t my_order = 0;
  std::uint32_t* shared = nullptr;
  float gain = 1.0f;
};
void stage_scale(void* state, float* l, float* r, std::uint32_t n) noexcept {
  StageLog& f = *static_cast<StageLog*>(state);
  ++f.calls;
  if (f.shared != nullptr) {
    f.my_order = ++*f.shared;
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    l[i] *= f.gain;
    r[i] *= f.gain;
  }
}

// The peak of a meter channel in the last published window, after enough blocks to fill one.
std::uint32_t window_peak(Rig& rig, unsigned channel) {
  MeterSnapshot s;
  CHECK(rig.g.meters().snapshot(s));
  return s.ch[channel].peak;
}
void fill_window(Rig& rig) {
  for (std::uint32_t i = 0; i < kMeterWindowBlocks; ++i) {
    rig.render();
  }
}

}  // namespace

TEST_CASE("graph: nothing live is digital silence, costs no kernel, and every stage but the sources is timed") {
  Rig rig(make_graph());
  for (int k = 0; k < 30; ++k) {
    rig.render();
    CHECK(rig.silent());
  }
  for (unsigned s = 0; s < ha::kStageCommit; ++s) {
    if (s == ha::kStageRender) {
      CHECK(rig.report.stage_cycles[s] == 0u);  // no source made a block: the sources' stage is nothing
    } else {
      CHECK(rig.report.stage_cycles[s] > 0u);   // the graph's other stages are stamped (the commit stage is the pipeline's)
    }
  }
  CHECK(rig.report.stage_cycles[ha::kStageCommit] == 0u);
  CHECK(rig.report.input_peak == 0u);
  MeterSnapshot s;
  CHECK(rig.g.meters().snapshot(s));
  CHECK(s.windows == 1u);  // 30 blocks: one window of 22 published
  for (unsigned c = 0; c < kMeterChannels; ++c) {
    CHECK(s.ch[c].peak == 0u);
  }
  // a live source makes the sources' stage cost something, and the strips' stage the rest
  Rng rng(20);
  fill(use_feed(rig, 0), rng, 1000);
  rig.render();
  CHECK(rig.report.stage_cycles[ha::kStageRender] > 0u);
  CHECK(rig.report.stage_cycles[ha::kStageStrips] > 0u);
}

TEST_CASE("graph: the routing truth table, every route mask of a track, with the exact samples") {
  Rng rng(21);
  alignas(8) static std::int16_t take[8 * kB];
  for (std::uint32_t mask = 0; mask <= route::kAll; ++mask) {
    Rig rig(make_graph());
    FxLog fa, fb;
    rig.g.install_send_fx(0, &fx_record, &fa);
    rig.g.set_send_fx_bypass(0, false);
    rig.g.install_send_fx(1, &fx_record, &fb);
    rig.g.set_send_fx_bypass(1, false);
    rig.g.stems().set_enabled(true);  // default slots: P10, channel 0 = T1
    CHECK(rig.g.rec().arm(take, 4 * kB, 2));
    std::int16_t* x = use_feed(rig, 0);
    rig.strip(0, gps::Route, static_cast<float>(mask));
    rig.strip(0, gps::SendA, 100.0f);
    rig.strip(0, gps::SendB, 100.0f);
    rig.master(gpm::OutMode, 1.0f);  // MAIN (mono) left, CUE (mono) right: both observable at the output
    fill(x, rng, 20000);
    alignas(8) std::int16_t want[kB];
    std::memcpy(want, x, sizeof want);
    rig.render();
    rig.render();  // two blocks: a stem block is two
    INFO("route mask " << mask);
    for (std::uint32_t i = 0; i < kB; ++i) {
      // pan centre, level 100, dry 100: unity everywhere; the mean of two equal samples is the sample
      CHECK(rig.l(i) == ((mask & route::kMain) != 0u ? want[i] : 0));
      CHECK(rig.r(i) == ((mask & route::kCue) != 0u ? want[i] : 0));
    }
    // sends (the effect is called every block, the bus is silent when the route bit is clear)
    CHECK(fa.calls == 2u);
    CHECK(fb.calls == 2u);
    for (std::uint32_t i = 0; i < kB; ++i) {
      CHECK(fa.last[i] == ((mask & route::kSndA) != 0u ? want[i] : 0));
      CHECK(fb.last[i] == ((mask & route::kSndB) != 0u ? want[i] : 0));
    }
    // REC: the second block of the take, interleaved L R
    CHECK(rig.g.rec().frames() == 2u * kB);
    for (std::uint32_t i = 0; i < kB; ++i) {
      const std::int16_t v = (mask & route::kRec) != 0u ? want[i] : static_cast<std::int16_t>(0);
      CHECK(take[2u * (kB + i)] == v);
      CHECK(take[2u * (kB + i) + 1u] == v);
    }
    // USB: the stem block (128 frames), channel 0 is T1's post-fader tap, silence when the bit is clear
    o3os::usb::StemReader reader(rig.g.stems().ring());
    alignas(4) std::int16_t stems[o3os::usb::kStemBlockFrames * 10];
    CHECK(reader.read_interleaved(stems, o3os::usb::kStemBlockFrames, 10) == o3os::usb::kStemBlockFrames);
    for (std::uint32_t half = 0; half < 2; ++half) {
      for (std::uint32_t i = 0; i < kB; ++i) {
        CHECK(stems[(half * kB + i) * 10u] == ((mask & route::kUsb) != 0u ? want[i] : 0));
        CHECK(stems[(half * kB + i) * 10u + 1u] == 0);                                    // T2 is not live
        CHECK(stems[(half * kB + i) * 10u + 8u] == ((mask & route::kMain) != 0u ? want[i] : 0));  // Master L
      }
    }
  }
}

TEST_CASE("graph: the pan law and the level law are smoothing.h's (formulae written out as an independent oracle)") {
  Rng rng(22);
  Rig rig(make_graph());
  std::int16_t* x = use_feed(rig, 0);
  alignas(8) std::int16_t want[kB];
  fill(x, rng, 30000);
  std::memcpy(want, x, sizeof want);
  const float pans[] = {0, 1, 12, 25, 49, 50, 51, 75, 88, 99, 100};
  for (const float pan : pans) {
    rig.strip(0, gps::Pan, pan);
    rig.render();
    const double gl = oracle_left(static_cast<double>(pan));
    const double gr = oracle_right(static_cast<double>(pan));
    for (std::uint32_t i = 0; i < kB; ++i) {
      INFO("pan " << pan << " i " << i);
      CHECK(absd(static_cast<double>(rig.l(i)) - static_cast<double>(want[i]) * gl) <= 1.5);  // the Q16 gain and the roundings: under 1.5 LSB
      CHECK(absd(static_cast<double>(rig.r(i)) - static_cast<double>(want[i]) * gr) <= 1.5);
    }
  }
  // pan 0 is the left jack only, pan 100 the right jack only, the centre both at unity (no centre attenuation)
  rig.strip(0, gps::Pan, 0.0f);
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == want[i]);
    CHECK(rig.r(i) == 0);
  }
  rig.strip(0, gps::Pan, 50.0f);
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == want[i]);
    CHECK(rig.r(i) == want[i]);
  }
  // the level: a track volume 0..100 and the master dry level multiply, by the 1.1.14 table 10^((v - 100) / 50)
  const int levels[] = {0, 1, 2, 10, 33, 50, 51, 86, 99, 100};
  for (const int v : levels) {
    for (const int dry : {100, 50}) {
      rig.strip(0, gps::Volume, static_cast<float>(v));
      rig.master(gpm::Dry, static_cast<float>(dry));
      rig.render();
      const double g = oracle_level_1114(v) * oracle_level_1114(dry);
      for (std::uint32_t i = 0; i < kB; ++i) {
        INFO("volume " << v << " dry " << dry);
        CHECK(absd(static_cast<double>(rig.l(i)) - static_cast<double>(want[i]) * g) <= 2.0);  // two Q16 roundings on top of the half LSB
        CHECK(rig.l(i) == rig.r(i));                                                         // the centre
      }
    }
  }
}

TEST_CASE("graph: the mixer's levels are the Og1114 table only (OS-007i), the pan law, and the product of two gains") {
  LevelCurve c;
  c.init();
  // The 1.1.14 table: monotone, exact at both ends, 0.4 dB a step.
  CHECK(c.entry(0) == 0);
  CHECK(c.entry(100) == 65536);
  for (unsigned i = 1; i <= 100; ++i) {
    CHECK(c.entry(i) > c.entry(i - 1));
    CHECK(absd(static_cast<double>(c.entry(i)) - oracle_level_1114(static_cast<int>(i)) * 65536.0) <= 1.0);
  }
  CHECK(c.q16(100.0f) == 65536);
  CHECK(c.q16(250.0f) == 65536);
  CHECK(c.q16(-3.0f) == 0);
  CHECK(c.q16(0.0f) == 0);
  CHECK(absd(static_cast<double>(c.q16(50.0f)) - 6553.6) <= 1.0);  // level 50 is -20 dB, 0.1 (smoothing.h THE LEVEL LAWS)
  CHECK(c.q16(50.5f) > c.q16(50.0f));
  CHECK(c.q16(50.5f) < c.q16(51.0f));
  // Nothing of the mixer is above unity: the 1.2+ mixer law is not sourced (crosscheck P1-1), and the instrument law (level_gain_q16 Og12) is not its law.
  for (unsigned i = 0; i <= 100; ++i) {
    CHECK(c.entry(i) <= 65536);
  }
  // The pan law.
  const PanGains p0 = pan_gains_q16(0.0f);
  CHECK(p0.left == 65536);
  CHECK(p0.right == 0);
  const PanGains p50 = pan_gains_q16(50.0f);
  CHECK(p50.left == 65536);
  CHECK(p50.right == 65536);
  const PanGains p100 = pan_gains_q16(100.0f);
  CHECK(p100.left == 0);
  CHECK(p100.right == 65536);
  CHECK(pan_gains_q16(25.0f).right == 32768);
  CHECK(pan_gains_q16(75.0f).left == 32768);
  CHECK(pan_gains_q16(-1.0f).right == 0);    // out of range: the end
  CHECK(pan_gains_q16(101.0f).left == 0);
  // The product of two gains is clamped to what a kernel is specified for.
  CHECK(mul_q16(65536, 65536) == 65536);
  const std::int32_t top12 = level_gain_q16(100, LevelLaw::Og12);
  CHECK(mul_q16(top12, top12) == o3os::audio::kernel::kMaxGain);

  // Through the graph: a level of 50 on the 1.1.14 table plays 20 dB down.
  Rng rng(23);
  Rig rig(make_graph());
  std::int16_t* x = use_feed(rig, 0);
  alignas(8) std::int16_t want[kB];
  fill(x, rng, 3000);
  std::memcpy(want, x, sizeof want);
  rig.strip(0, gps::Volume, 50.0f);
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(absd(static_cast<double>(rig.l(i)) - static_cast<double>(want[i]) * 0.1) <= 1.0);
  }
}

TEST_CASE("graph: a v7+ project's mixer levels (86, the default of the track, dry, reverb and delay levels) are NOT boosted: the mixer has no 1.2+ law yet") {
  // Crosscheck P1-1: through the instrument law 86 would be +17.3 dB. Here it is the 1.1.14 table's 10^(-14 / 50) = 0.525 (-5.6 dB), however the project was saved.
  Rng rng(24);
  Rig rig(make_graph());
  std::int16_t* x = use_feed(rig, 0);
  alignas(8) std::int16_t want[kB];
  fill(x, rng, 20000);
  std::memcpy(want, x, sizeof want);
  const double g86 = oracle_level_1114(86);
  CHECK(g86 < 0.53);
  CHECK(g86 > 0.52);
  CHECK(rig.g.level_curve().q16(86.0f) < 65536);
  CHECK(absd(static_cast<double>(rig.g.level_curve().q16(86.0f)) - g86 * 65536.0) <= 1.0);
  // the track fader alone
  rig.strip(0, gps::Volume, 86.0f);
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(absd(static_cast<double>(rig.l(i)) - static_cast<double>(want[i]) * g86) <= 1.5);
    CHECK((rig.l(i) <= 0 ? -rig.l(i) : rig.l(i)) <= (want[i] <= 0 ? -want[i] : want[i]));  // never louder than the source
  }
  // and every mixer level a v7+ project stores, at once: track, dry and master volume
  rig.master(gpm::Dry, 86.0f);
  rig.master(gpm::Volume, 86.0f);
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(absd(static_cast<double>(rig.l(i)) - static_cast<double>(want[i]) * g86 * g86 * g86) <= 2.0);
  }
  // the engine has no switch for another law: the retired id is refused, and the levels are unchanged
  const std::uint32_t before = rig.g.params_rejected();
  rig.set(master_param(0x104), 1.0f);
  rig.render();
  CHECK(rig.g.params_rejected() == before + 1u);
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(absd(static_cast<double>(rig.l(i)) - static_cast<double>(want[i]) * g86 * g86 * g86) <= 2.0);
  }
}

TEST_CASE("graph: a gain change ramps across the block by a bounded step and lands on its target; multi-block ramps arrive exactly") {
  Rig rig(make_graph());
  std::int16_t* x = use_feed(rig, 0);
  const std::int16_t dc = 16384;
  for (std::uint32_t i = 0; i < kB; ++i) x[i] = dc;
  rig.render();
  CHECK(rig.l(0) == dc);
  CHECK(rig.l(kB - 1u) == dc);

  // Volume 100 -> 50 over one block (ramp 0 = the engine's smoothing): from 65536 to the level of 50.
  const std::int32_t target = rig.g.level_curve().q16(50.0f);
  rig.strip(0, gps::Volume, 50.0f, 0);
  rig.render();
  // sample i has the gain 65536 + (target - 65536) (i + 1) / 64 (a quarter of a Q16 unit of error at most, from the Q20 ramp)
  std::int32_t prev = dc;
  const std::int32_t delta = 65536 - target;
  const std::int32_t gain_bound = (delta + static_cast<std::int32_t>(kB) - 1) / static_cast<std::int32_t>(kB);
  const std::int32_t step_bound = static_cast<std::int32_t>(static_cast<std::int64_t>(dc) * gain_bound / 65536) + 2;
  for (std::uint32_t i = 0; i < kB; ++i) {
    const double g = 65536.0 + static_cast<double>(target - 65536) * static_cast<double>(i + 1u) / 64.0;
    CHECK(absd(static_cast<double>(rig.l(i)) - static_cast<double>(dc) * g / 65536.0) <= 0.6);
    const std::int32_t step = prev - rig.l(i);
    CHECK(step >= 0);                  // monotone
    CHECK(step <= step_bound);         // a bounded step: no zipper
    prev = rig.l(i);
  }
  CHECK(absd(static_cast<double>(rig.l(kB - 1u)) - static_cast<double>(dc) * static_cast<double>(target) / 65536.0) <= 0.6);  // the target at the block's end
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(absd(static_cast<double>(rig.l(i)) - static_cast<double>(dc) * static_cast<double>(target) / 65536.0) <= 0.6);  // and steady afterwards
  }

  // Instant (ramp 1) is a step, as asked.
  rig.strip(0, gps::Volume, 100.0f, 1);
  rig.render();
  CHECK(rig.l(0) == dc);

  // A multi-block ramp: 4 blocks (256 samples), each block linear, the end exact, the step a quarter of the single-block one.
  rig.strip(0, gps::Volume, 50.0f, 4u * kB);
  std::int32_t last_end = 65536;
  for (int blk = 0; blk < 4; ++blk) {
    rig.render();
    const std::int32_t expect_end = blk == 3 ? target : last_end + (target - last_end) / (4 - blk);
    CHECK(absd(static_cast<double>(rig.l(kB - 1u)) - static_cast<double>(dc) * static_cast<double>(expect_end) / 65536.0) <= 0.6);
    for (std::uint32_t i = 1; i < kB; ++i) {
      CHECK(rig.l(i - 1u) - rig.l(i) <= static_cast<std::int32_t>(static_cast<std::int64_t>(dc) * ((delta / 4 + 63) / 64) / 65536) + 2);
    }
    last_end = expect_end;
  }
  CHECK(rig.g.level_gain(0) == target);
  // The longest ramp is bounded, not wrapped: a ramp of 2^32 - 1 samples is kMaxRampBlocks long, never one block.
  rig.strip(0, gps::Volume, 100.0f, 0xFFFFFFFFu);
  rig.render();
  CHECK(rig.g.level_gain(0) > target);
  CHECK(rig.g.level_gain(0) < target + (65536 - target) / 100);  // barely moved off the target after one of 4,096 blocks
  // A new target during a ramp starts from where the gain is now.
  rig.strip(0, gps::Volume, 100.0f, 0);
  rig.render();
  CHECK(rig.g.level_gain(0) == 65536);

  // The pan sweep is smoothed the same way: hard left to hard right in one block, the right channel rises by at most one step a sample.
  rig.strip(0, gps::Pan, 0.0f, 1);
  rig.render();
  CHECK(rig.r(5) == 0);
  rig.strip(0, gps::Pan, 100.0f, 0);
  rig.render();
  CHECK(rig.r(kB - 1u) == dc);
  CHECK(rig.l(kB - 1u) == 0);
  for (std::uint32_t i = 1; i < kB; ++i) {
    CHECK(rig.r(i) >= rig.r(i - 1u));
    CHECK(rig.r(i) - rig.r(i - 1u) <= static_cast<std::int32_t>(static_cast<std::int64_t>(dc) * 1024 / 65536) + 2);
    CHECK(rig.l(i) <= rig.l(i - 1u));
  }
}

TEST_CASE("graph: null tests (D8): a track is the ideal product within one LSB; a mix of twelve has no DC offset and no drift") {
  Rng rng(24);
  Rig rig(make_graph());
  // One track, level and pan from the table, 64-sample blocks of a pseudo-random signal: |output - ideal| <= 1 LSB, in both channels.
  std::int16_t* x = use_feed(rig, 0);
  double worst = 0.0;
  for (const int vol : {100, 86, 70, 41, 7}) {
    for (const float pan : {0.0f, 20.0f, 50.0f, 63.0f, 100.0f}) {
      rig.strip(0, gps::Volume, static_cast<float>(vol));
      rig.strip(0, gps::Pan, pan);
      const double gain = static_cast<double>(rig.g.level_curve().q16(static_cast<float>(vol))) / 65536.0;
      const PanGains pg = pan_gains_q16(pan);
      for (int blk = 0; blk < 6; ++blk) {
        fill(x, rng, 32000);
        rig.render();
        for (std::uint32_t i = 0; i < kB; ++i) {
          const double ideal_l = static_cast<double>(x[i]) * gain * static_cast<double>(pg.left) / 65536.0;
          const double ideal_r = static_cast<double>(x[i]) * gain * static_cast<double>(pg.right) / 65536.0;
          worst = absd(static_cast<double>(rig.l(i)) - ideal_l) > worst ? absd(static_cast<double>(rig.l(i)) - ideal_l) : worst;
          worst = absd(static_cast<double>(rig.r(i)) - ideal_r) > worst ? absd(static_cast<double>(rig.r(i)) - ideal_r) : worst;
        }
      }
    }
  }
  CHECK_MESSAGE(worst <= 1.0, "worst error of one track: " << worst << " LSB");

  // Twelve tracks of zero-mean noise at assorted pans (centre ones among them: the centre accumulator and the pairs), levels and sends closed. The
  // level stage rounds to nearest, and what is left is the floor of each attenuating pan product in the Q19 accumulator: -1/32 LSB on average, once
  // per track per channel that the pan law attenuates (one channel of each track that is off the centre: 8 of 12 here), so the mean of the error over
  // both channels is about -8/32/2 = -0.125 LSB. A floor in the level and pan stages at Q15, the arithmetic this replaced, put it at about -6 LSB.
  // The bound: the mean within 0.15 LSB over 12,800 samples, and the worst sample within 4 (twelve independent roundings).
  Rig mix(make_graph());
  float pans[12];
  double gains[12];
  for (unsigned t = 0; t < 12; ++t) {
    pans[t] = t % 3u == 0u ? 50.0f : static_cast<float>(rng.below(101));
    const int vol = 60 + static_cast<int>(rng.below(41));
    gains[t] = static_cast<double>(mix.g.level_curve().q16(static_cast<float>(vol))) / 65536.0;
    fill(use_feed(mix, t), rng, 4000);
    mix.strip(t, gps::Volume, static_cast<float>(vol));
    mix.strip(t, gps::Pan, pans[t]);
  }
  double sum_err = 0.0, worst_mix = 0.0;
  std::uint32_t count = 0;
  for (int blk = 0; blk < 100; ++blk) {
    for (unsigned t = 0; t < 12; ++t) {
      fill(mix.fb(t), rng, 4000);
    }
    mix.render();
    for (std::uint32_t i = 0; i < kB; ++i) {
      double ideal_l = 0.0, ideal_r = 0.0;
      for (unsigned t = 0; t < 12; ++t) {
        const PanGains pg = pan_gains_q16(pans[t]);
        ideal_l += static_cast<double>(mix.feed_[t][i]) * gains[t] * static_cast<double>(pg.left) / 65536.0;
        ideal_r += static_cast<double>(mix.feed_[t][i]) * gains[t] * static_cast<double>(pg.right) / 65536.0;
      }
      const double el = static_cast<double>(mix.l(i)) - ideal_l;
      const double er = static_cast<double>(mix.r(i)) - ideal_r;
      sum_err += el + er;
      worst_mix = absd(el) > worst_mix ? absd(el) : worst_mix;
      worst_mix = absd(er) > worst_mix ? absd(er) : worst_mix;
      ++count;
    }
  }
  const double mean = sum_err / static_cast<double>(2u * count);
  CHECK_MESSAGE(absd(mean) <= 0.15, "DC offset of the mix: " << mean << " LSB");
  CHECK_MESSAGE(worst_mix <= 4.0, "worst error of the mix: " << worst_mix << " LSB");
}

TEST_CASE("graph: ParamChange ids and ranges: what is applied, what is refused, nothing is invented") {
  Rig rig(make_graph());
  const std::uint32_t before = rig.g.params_rejected();
  auto push = [&](o3os::core::ParamId id, float v) { CHECK(rig.g.params().push(ParamChange{id, v, 1u})); };
  const float nan = __builtin_nanf("");
  push(strip_param(0, gps::Volume), 101.0f);
  push(strip_param(0, gps::Volume), -1.0f);
  push(strip_param(0, gps::Volume), nan);
  push(strip_param(0, gps::Mute), 2.0f);
  push(strip_param(0, 0x7777), 1.0f);                               // no such field
  push(strip_param(17, gps::Volume), 50.0f);                        // no such strip
  push(strip_param(kInputStrip, gps::Pan), 10.0f);                  // the input has no pan
  push(strip_param(kInputStrip, gps::Solo), 1.0f);                  // nor solo
  push(strip_param(kInputStrip, gps::Source), 1.0f);                // nor a test source
  push(strip_param(0, gps::Route), 64.0f);
  push(strip_param(0, gps::Source), 4.0f);
  push(master_param(gpm::OutMode), 2.0f);
  push(master_param(0x104), 2.0f);                                  // the retired LevelLaw (OS-007i)
  push(master_param(0x7777), 1.0f);
  push(o3os::core::param_id(o3os::core::ParamKind::Master, 1, gpm::Volume), 50.0f);  // master has one slot
  push(o3os::core::param_id(o3os::core::ParamKind::Instrument, 0, 0), 50.0f);         // not the graph's kind
  push(o3os::core::param_id(o3os::core::ParamKind::Device, 0, 0), 50.0f);
  const std::uint32_t refused = 17;
  const std::uint32_t applied_before = rig.g.params_applied();
  rig.render();
  CHECK(rig.g.params_rejected() - before == refused);
  CHECK(rig.g.params_applied() - applied_before == refused);  // drained all the same (the figure is "pulled off the ring")
  // Accepted: the model's ids at their ranges' ends, the engine fields, the Bus field (accepted, ignored).
  const std::uint32_t r0 = rig.g.params_rejected();
  push(strip_param(0, gps::Volume), 0.0f);
  push(strip_param(0, gps::Volume), 100.0f);
  push(strip_param(0, gps::Pan), 37.5f);
  push(strip_param(15, gps::Mute), 1.0f);
  push(strip_param(0, gps::Bus), 16.0f);
  push(strip_param(kInputStrip, gps::Volume), 80.0f);
  push(strip_param(kInputStrip, gps::Route), 63.0f);
  push(master_param(gpm::Limiter), 1.0f);
  push(master_param(gpm::RetRouteB), 3.0f);
  rig.render();
  CHECK(rig.g.params_rejected() == r0);
  // The ring is bounded per block: a flood drains kMaxParamsPerBlock at a time.
  const std::uint32_t a0 = rig.g.params_applied();
  for (std::uint32_t i = 0; i < kParamRingSize; ++i) {
    push(strip_param(0, gps::Volume), static_cast<float>(i % 101u));
  }
  rig.render();
  CHECK(rig.g.params_applied() - a0 == kMaxParamsPerBlock);
}

TEST_CASE("graph: mute and solo silence a track everywhere, its sends too (the default reading of solo, graph.h); a solo does not mute the input") {
  Rng rng(26);
  Rig rig(make_graph());
  FxLog fa;
  rig.g.install_send_fx(0, &fx_record, &fa);
  rig.g.set_send_fx_bypass(0, false);
  std::int16_t* x0 = use_feed(rig, 0);
  std::int16_t* x1 = use_feed(rig, 1);
  for (unsigned t = 0; t < 2; ++t) {
    rig.strip(t, gps::Route, static_cast<float>(route::kMain | route::kSndA));
    rig.strip(t, gps::SendA, 100.0f);
  }
  alignas(8) std::int16_t w0[kB], w1[kB];
  auto refill = [&]() {
    fill(x0, rng, 10000);
    fill(x1, rng, 10000);
    std::memcpy(w0, x0, sizeof w0);
    std::memcpy(w1, x1, sizeof w1);
  };
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == w0[i] + w1[i]);
    CHECK(fa.last[i] == w0[i] + w1[i]);
  }
  rig.strip(0, gps::Mute, 1.0f);
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == w1[i]);       // track 1 alone
    CHECK(fa.last[i] == w1[i]);     // a muted track does not send either
  }
  rig.strip(0, gps::Mute, 0.0f);
  rig.strip(1, gps::Solo, 1.0f);  // solo track 2: the default mutes every other track, sends included
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == w1[i]);
    CHECK(fa.last[i] == w1[i]);
  }
  // The other reading (the export's half: the solo mutes the others' amplifiers, not their sends): their sends keep going.
  rig.g.set_solo_mutes_sends(false);
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == w1[i]);                // the dry mix is the soloed track alone either way
    CHECK(fa.last[i] == w0[i] + w1[i]);      // the sends are not muted by the solo
  }
  rig.g.set_solo_mutes_sends(true);
  // The input strip keeps sounding under a solo.
  rig.strip(kInputStrip, gps::Volume, 100.0f);
  for (std::uint32_t i = 0; i < kB; ++i) {
    rig.in[2u * i] = static_cast<std::int16_t>(1000 + static_cast<int>(i));
    rig.in[2u * i + 1u] = 0;
  }
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == w1[i] + 1000 + static_cast<int>(i));
  }
  rig.strip(1, gps::Solo, 0.0f);
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == w0[i] + w1[i] + 1000 + static_cast<int>(i));
  }
}

TEST_CASE("graph: a route closed while the track is silent does not leak when the track comes back (mute, close MAIN, unmute)") {
  Rng rng(27);
  for (int variant = 0; variant < 2; ++variant) {
    Rig rig(make_graph());
    std::int16_t* x = use_feed(rig, 0);
    fill(x, rng, 20000);
    rig.strip(0, gps::Route, static_cast<float>(route::kMain | route::kCue));
    rig.master(gpm::OutMode, 1.0f);  // MAIN (mono) on the left jack, CUE (mono) on the right: MAIN is what must stay shut
    rig.render();
    rig.render();
    CHECK_FALSE(rig.silent());
    // 1. mute with the engine's own ramp (a block), settle: the fader is shut; 2. close MAIN under the mute; 3. unmute (a ramp too)
    rig.strip(0, gps::Mute, 1.0f, 0);
    rig.render();
    rig.render();
    CHECK(rig.silent());
    if (variant == 0) {
      // the track keeps producing blocks: the fader is shut, so the strip pass takes its early exit
      rig.strip(0, gps::Route, static_cast<float>(route::kCue), 0);
      rig.render();
      rig.render();
    } else {
      // the track stops producing blocks (no source), and the route changes meanwhile
      rig.strip(0, gps::Source, 0.0f);
      rig.render();
      rig.strip(0, gps::Route, static_cast<float>(route::kCue), 0);
      rig.render();
      rig.render();
      rig.strip(0, gps::Source, 3.0f);
    }
    rig.strip(0, gps::Mute, 0.0f, 0);
    for (int blk = 0; blk < 4; ++blk) {
      rig.render();
      INFO("variant " << variant << " block " << blk);
      for (std::uint32_t i = 0; i < kB; ++i) {
        CHECK(rig.l(i) == 0);  // nothing reaches MAIN, not even a fading remnant of the route that was open before the mute
      }
      if (blk >= 1) {
        CHECK_FALSE(rig.r(5) == 0);  // while CUE, which stayed open, is back at full level
      }
    }
  }
}

TEST_CASE("graph: sends are pre-fader and scaled by the master level; the mutes and the route bit close them") {
  Rng rng(28);
  Rig rig(make_graph());
  FxLog fa, fb;
  rig.g.install_send_fx(0, &fx_record, &fa);
  rig.g.set_send_fx_bypass(0, false);
  rig.g.install_send_fx(1, &fx_record, &fb);
  rig.g.set_send_fx_bypass(1, false);
  std::int16_t* x = use_feed(rig, 0);
  rig.strip(0, gps::Route, static_cast<float>(route::kMain | route::kSndA | route::kSndB));
  rig.strip(0, gps::SendA, 100.0f);
  rig.strip(0, gps::SendB, 50.0f);
  rig.strip(0, gps::Volume, 0.0f);  // the fader is shut: the dry path is silent, the sends (pre-fader) are not
  alignas(8) std::int16_t want[kB];
  auto refill = [&]() {
    fill(x, rng, 30000);
    std::memcpy(want, x, sizeof want);
  };
  auto near = [&](std::int16_t got, std::int16_t src, std::int32_t g16) {
    return absd(static_cast<double>(got) - static_cast<double>(src) * static_cast<double>(g16) / 65536.0) <= 1.0;
  };
  refill();
  rig.render();
  const LevelCurve& c = rig.g.level_curve();
  CHECK(rig.silent());
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(near(fa.last[i], want[i], mul_q16(c.q16(100.0f), c.q16(100.0f))));
    CHECK(near(fb.last[i], want[i], mul_q16(c.q16(50.0f), c.q16(100.0f))));
  }
  // The master DELAY level (SNDA) and REVERB level (SNDB) scale the sends.
  rig.master(gpm::Delay, 50.0f);
  rig.master(gpm::Reverb, 20.0f);
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(near(fa.last[i], want[i], mul_q16(c.q16(100.0f), c.q16(50.0f))));
    CHECK(near(fb.last[i], want[i], mul_q16(c.q16(50.0f), c.q16(20.0f))));
  }
  // The master mutes, then the route bit.
  rig.master(gpm::DelayMute, 1.0f);
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(fa.last[i] == 0);
    CHECK(near(fb.last[i], want[i], mul_q16(c.q16(50.0f), c.q16(20.0f))));
  }
  rig.strip(0, gps::Route, static_cast<float>(route::kMain | route::kSndA));
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(fb.last[i] == 0);
  }
  // A track level of 100 no longer matters to the send: the same figures with the fader up.
  rig.master(gpm::DelayMute, 0.0f);
  rig.strip(0, gps::Volume, 100.0f);
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(near(fa.last[i], want[i], mul_q16(c.q16(100.0f), c.q16(50.0f))));
  }
}

TEST_CASE("graph: the send effects are reserved slots: no processor, no return; bypass closes the input and lets the tail ring out") {
  Rng rng(29);
  Rig rig(make_graph());
  FxLog fa;
  fa.ret_gain = 32768;  // the effect returns half of what it gets
  std::int16_t* x = use_feed(rig, 0);
  rig.strip(0, gps::Route, static_cast<float>(route::kSndA));  // send only: no dry
  rig.strip(0, gps::SendA, 100.0f);
  alignas(8) std::int16_t want[kB];
  fill(x, rng, 20000);
  std::memcpy(want, x, sizeof want);
  rig.render();
  CHECK(fa.calls == 0u);  // a slot with no processor is not run, and has no return
  CHECK(rig.silent());
  rig.g.install_send_fx(0, &fx_record, &fa);  // installed, still bypassed: its INPUT is closed, it is still run (a tail must be able to ring out)
  rig.render();
  CHECK(fa.calls == 1u);
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(fa.last[i] == 0);  // it was handed silence
  }
  CHECK(rig.silent());
  rig.g.set_send_fx_bypass(0, false);
  rig.render();
  CHECK(fa.calls == 2u);
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(fa.last[i] == want[i]);
    CHECK(rig.l(i) == gained(want[i], 32768));  // the return reaches MAIN at unity
    CHECK(rig.r(i) == rig.l(i));
  }
  // The return to CUE only, seen through OUT MODE.
  rig.master(gpm::RetRouteA, static_cast<float>(route::kCue));
  rig.master(gpm::OutMode, 1.0f);
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == 0);
    CHECK(rig.r(i) == gained(want[i], 32768));
  }

  // The tail: an effect that returns what it was given a block ago. Signal in, then bypass: the next block's return is the last signal that went in
  // (the tail rings out) and the input of that block is silence; the block after is silent.
  Rig tail(make_graph());
  TailFx t;
  tail.g.install_send_fx(0, &fx_tail, &t);
  tail.g.set_send_fx_bypass(0, false);
  std::int16_t* y = use_feed(tail, 0);
  tail.strip(0, gps::Route, static_cast<float>(route::kSndA));
  tail.strip(0, gps::SendA, 100.0f);
  fill(y, rng, 20000);
  alignas(8) std::int16_t sent[kB];
  std::memcpy(sent, y, sizeof sent);
  tail.render();
  CHECK(tail.silent());                               // block 1: the effect returns what it had (nothing)
  tail.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(tail.l(i) == sent[i]);                      // block 2: block 1's input
  }
  tail.g.set_send_fx_bypass(0, true);
  tail.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(t.last_in[i] == 0);                         // block 3: its input is closed ...
    CHECK(tail.l(i) == sent[i]);                      // ... and what block 2 put in rings out
  }
  tail.render();
  CHECK(tail.silent());                               // block 4: the tail is gone
}

TEST_CASE("graph: the inserts are reserved slots: bypassed by default, called in order before the sends and the fader when un-bypassed") {
  Rng rng(30);
  Rig rig(make_graph());
  std::uint32_t order = 0;
  InsertLog i0, i1;
  i0.shared = i1.shared = &order;
  i0.gain = 32768;  // halves
  i1.gain = 32768;  // halves again
  rig.g.install_insert(0, 0, &insert_scale, &i0);
  rig.g.install_insert(0, 1, &insert_scale, &i1);
  FxLog fa;
  rig.g.install_send_fx(0, &fx_record, &fa);
  rig.g.set_send_fx_bypass(0, false);
  std::int16_t* x = use_feed(rig, 0);
  rig.strip(0, gps::Route, static_cast<float>(route::kMain | route::kSndA));
  rig.strip(0, gps::SendA, 100.0f);
  alignas(8) std::int16_t want[kB];
  fill(x, rng, 20000);
  std::memcpy(want, x, sizeof want);
  rig.render();
  CHECK(i0.calls == 0u);  // installed, bypassed: never called
  CHECK(i1.calls == 0u);
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == want[i]);  // and the block is untouched
  }
  rig.g.set_insert_bypass(0, 0, false);
  rig.g.set_insert_bypass(0, 1, false);
  rig.render();
  CHECK(i0.calls == 1u);
  CHECK(i1.calls == 1u);
  CHECK(i0.my_order == 1u);  // insert 1 first
  CHECK(i1.my_order == 2u);
  for (std::uint32_t i = 0; i < kB; ++i) {
    const std::int32_t q = gained(gained(want[i], 32768), 32768);
    CHECK(rig.l(i) == q);       // the dry path after both
    CHECK(fa.last[i] == q);     // the send is post-insert, pre-fader
  }
}

TEST_CASE("graph: the master chain is a table of reserved stages, bypassed bit-transparently, in order when active") {
  CHECK(kMasterStages == 8u);
  CHECK(kMasterChain[0].id == MasterStage::Headroom);  // ADR 0013 D4, order B
  CHECK(kMasterChain[1].id == MasterStage::Saturation);
  CHECK(kMasterChain[2].id == MasterStage::Eq);  // OS-DJ A3c: the DJ page's stages (kill EQ, filter) run ahead of the limiter
  CHECK(kMasterChain[3].id == MasterStage::Bass);
  CHECK(kMasterChain[4].id == MasterStage::Insert);  // OS-INSFX S6: the master effect module sits after the DJ stages and ahead of the limiter, which catches what it adds
  CHECK(kMasterChain[5].id == MasterStage::Limiter);
  CHECK(kMasterChain[6].id == MasterStage::Space);
  CHECK(kMasterChain[7].id == MasterStage::BitDepth);
  for (unsigned s = 0; s < kMasterStages; ++s) {  // the table is the one source of order: the row index is the enum value, and the names are the diag's
    CHECK(static_cast<unsigned>(kMasterChain[s].id) == s);
    CHECK(kMasterChain[s].name != nullptr);
  }
  CHECK(std::strcmp(kMasterChain[4].name, "insert") == 0);

  alignas(8) std::int16_t reference[3][kBS];
  std::uint32_t order = 0;
  StageLog stages[kMasterStages];
  for (int pass = 0; pass < 3; ++pass) {
    Rig rig(make_graph());
    if (pass >= 1) {
      for (unsigned s = 0; s < kMasterStages; ++s) {
        stages[s] = StageLog{};
        stages[s].shared = &order;
        stages[s].gain = 0.0f;  // would silence the block if it ever ran
        rig.g.install_master_stage(static_cast<MasterStage>(s), &stage_scale, &stages[s]);  // installed: bypassed
        if (pass == 2) {
          rig.g.set_master_bypass(static_cast<MasterStage>(s), true);  // and explicitly so
        }
        CHECK_FALSE(rig.g.master_stage_active(static_cast<MasterStage>(s)));
      }
    }
    // Several tracks of random content, panned and levelled differently, summed into MAIN.
    Rng content(77);
    for (unsigned t = 0; t < 5; ++t) {
      use_feed(rig, t);
      rig.strip(t, gps::Pan, static_cast<float>(content.below(101)));
      rig.strip(t, gps::Volume, static_cast<float>(60 + content.below(41)));
    }
    for (int blk = 0; blk < 3; ++blk) {
      for (unsigned t = 0; t < 5; ++t) {
        fill(rig.fb(t), content, 32000);  // sums that clip: the saturation is part of what must be identical
      }
      rig.render();
    }
    std::memcpy(reference[pass], rig.out, sizeof rig.out);
  }
  CHECK(std::memcmp(reference[0], reference[1], sizeof reference[0]) == 0);  // installed but bypassed: not one bit differs
  CHECK(std::memcmp(reference[0], reference[2], sizeof reference[0]) == 0);
  for (unsigned s = 0; s < kMasterStages; ++s) {
    CHECK(stages[s].calls == 0u);
  }

  // An active identity stage: MAIN goes to float straight from the accumulators and back through the one saturation; an int16 over 2^15 and back is
  // exact, so the block of a single track (no clipping) is the same bit for bit.
  Rng content(78);
  alignas(8) std::int16_t base[kB];
  alignas(8) std::int16_t plain_out[kBS];
  {
    Rig plain(make_graph());
    fill(use_feed(plain, 0), content, 32767);
    std::memcpy(base, plain.fb(0), sizeof base);
    plain.render();
    std::memcpy(plain_out, plain.out, sizeof plain_out);
  }
  {
    Rig active(make_graph());
    order = 0;
    StageLog a, b;
    a.shared = b.shared = &order;
    a.gain = 1.0f;
    b.gain = 1.0f;
    StageLog c, d;  // the EQ and bass slots (the DJ page's) sit ahead of the limiter too (OS-DJ A3c)
    c.shared = d.shared = &order;
    c.gain = 1.0f;
    d.gain = 1.0f;
    StageLog e;  // the master insert (OS-INSFX S6): after bass, before the limiter
    e.shared = &order;
    e.gain = 1.0f;
    active.g.install_master_stage(MasterStage::Insert, &stage_scale, &e);
    active.g.set_master_bypass(MasterStage::Insert, false);
    active.g.install_master_stage(MasterStage::Limiter, &stage_scale, &b);
    active.g.install_master_stage(MasterStage::Bass, &stage_scale, &d);
    active.g.install_master_stage(MasterStage::Saturation, &stage_scale, &a);
    active.g.install_master_stage(MasterStage::Eq, &stage_scale, &c);
    active.g.set_master_bypass(MasterStage::Limiter, false);
    active.g.set_master_bypass(MasterStage::Bass, false);
    active.g.set_master_bypass(MasterStage::Saturation, false);
    active.g.set_master_bypass(MasterStage::Eq, false);
    CHECK(active.g.master_stage_active(MasterStage::Saturation));
    std::memcpy(use_feed(active, 0), base, sizeof base);
    active.render();
    CHECK(std::memcmp(active.out, plain_out, sizeof plain_out) == 0);
    CHECK(a.calls == 1u);
    CHECK(b.calls == 1u);
    CHECK(c.calls == 1u);
    CHECK(d.calls == 1u);
    CHECK(e.calls == 1u);
    CHECK(a.my_order == 1u);  // saturation, EQ, bass, insert, then the limiter: kMasterChain order, whatever the install order was
    CHECK(c.my_order == 2u);
    CHECK(d.my_order == 3u);
    CHECK(e.my_order == 4u);
    CHECK(b.my_order == 5u);
  }
  // A stage that does something is heard: half gain halves the block (within the conversion's rounding).
  {
    Rig half(make_graph());
    StageLog h;
    h.gain = 0.5f;
    half.g.install_master_stage(MasterStage::Eq, &stage_scale, &h);
    half.g.set_master_bypass(MasterStage::Eq, false);
    std::memcpy(use_feed(half, 0), base, sizeof base);
    half.render();
    for (std::uint32_t i = 0; i < kB; ++i) {
      CHECK(absd(static_cast<double>(half.l(i)) - static_cast<double>(base[i]) * 0.5) <= 1.0);
    }
  }
}

TEST_CASE("graph: sixteen full-scale tracks at unity sum without overflow and the bus clips at the rails (OS-007i headroom)") {
  // The voices hand the graph int16 blocks whatever the instrument's gain (+24 dB is saturated at the voice), and no mixer level is above unity, so the worst
  // the graph can be given is sixteen strips of full scale: 16 x 32768 x 16 at Q19 is 2^23 in the int32 accumulators. The bus saturates at its output.
  for (const bool pan_apart : {false, true}) {  // centred (the centre accumulator) and hard left / hard right alternately (the stereo accumulators)
    Rig rig(make_graph());
    rig.g.configure(0xFFFFu);
    alignas(8) std::int16_t block[kB];
    for (std::uint32_t i = 0; i < kB; ++i) {
      block[i] = static_cast<std::int16_t>(i % 2 == 0 ? 32767 : -32768);
    }
    for (unsigned t = 0; t < 16; ++t) {
      std::memcpy(use_feed(rig, t), block, sizeof block);
      rig.strip(t, gps::Volume, 100.0f);
      rig.strip(t, gps::Pan, pan_apart ? (t % 2u == 0u ? 0.0f : 100.0f) : 50.0f);
    }
    rig.render();
    for (std::uint32_t i = 0; i < kB; ++i) {
      CHECK(rig.l(i) == block[i]);
      CHECK(rig.r(i) == block[i]);
    }
  }
}

TEST_CASE("graph: the master chain sees the sum unclipped: the bus is saturated once, after the chain (ADR 0013 D2)") {
  // Two tracks of 30000, centred: the sum is 60000, beyond full scale. Without a chain the bus clips at its output. With a stage that halves, the chain
  // gets 1.83 (not the clipped 1.0) and its half is 30000, not 16384: nothing clipped the bus before the chain.
  Rng rng(31);
  for (const bool pan_apart : {false, true}) {  // centred (the centre accumulator, a pair) and panned (the stereo accumulators)
    alignas(8) std::int16_t block[kB];
    for (std::uint32_t i = 0; i < kB; ++i) {
      block[i] = static_cast<std::int16_t>(i % 2 == 0 ? 30000 : -30000);
    }
    Rig plain(make_graph());
    for (unsigned t = 0; t < 2; ++t) {
      std::memcpy(use_feed(plain, t), block, sizeof block);
      plain.strip(t, gps::Pan, pan_apart ? 40.0f + 20.0f * static_cast<float>(t) : 50.0f);
    }
    plain.render();
    const std::int16_t clipped = plain.l(0);
    CHECK((clipped == 32767 || clipped > 30000));  // clipped (pan 40 and 60 take a little off the far side: the left sum is 30000 + 30000 * 0.8)
    Rig chain(make_graph());
    StageLog half;
    half.gain = 0.5f;
    chain.g.install_master_stage(MasterStage::Headroom, &stage_scale, &half);
    chain.g.set_master_bypass(MasterStage::Headroom, false);
    for (unsigned t = 0; t < 2; ++t) {
      std::memcpy(use_feed(chain, t), block, sizeof block);
      chain.strip(t, gps::Pan, pan_apart ? 40.0f + 20.0f * static_cast<float>(t) : 50.0f);
    }
    chain.render();
    const double left_sum = pan_apart ? 30000.0 * (1.0 + oracle_left(60.0)) : 60000.0;  // track 1 at pan 40 gives the left 1.0, track 2 at pan 60 gives 0.8
    CHECK(absd(static_cast<double>(chain.l(0)) - rail(left_sum * 0.5)) <= 1.5);
    CHECK(chain.l(0) > 20000);  // not 16384: the half of a clipped bus
    // and an identity stage clips once, at the end, like a plain bus
    Rig ident(make_graph());
    StageLog one;
    ident.g.install_master_stage(MasterStage::Headroom, &stage_scale, &one);
    ident.g.set_master_bypass(MasterStage::Headroom, false);
    for (unsigned t = 0; t < 2; ++t) {
      std::memcpy(use_feed(ident, t), block, sizeof block);
      ident.strip(t, gps::Pan, pan_apart ? 40.0f + 20.0f * static_cast<float>(t) : 50.0f);
    }
    ident.render();
    CHECK(ident.l(0) == plain.l(0));
    (void)rng;
  }
}

TEST_CASE("graph: OUT MODE and the cue level") {
  Rng rng(32);
  Rig rig(make_graph());
  std::int16_t* x0 = use_feed(rig, 0);
  std::int16_t* x1 = use_feed(rig, 1);
  rig.strip(0, gps::Route, static_cast<float>(route::kMain));
  rig.strip(1, gps::Route, static_cast<float>(route::kCue));
  rig.strip(0, gps::Pan, 0.0f);   // main: left only
  rig.strip(1, gps::Pan, 100.0f); // cue: right only
  alignas(8) std::int16_t w0[kB], w1[kB];
  auto refill = [&]() {
    fill(x0, rng, 30000);
    fill(x1, rng, 30000);
    std::memcpy(w0, x0, sizeof w0);
    std::memcpy(w1, x1, sizeof w1);
  };
  // Stereo: the line out is MAIN; the cue bus has no output.
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == w0[i]);
    CHECK(rig.r(i) == 0);
  }
  // MAIN L / CUE R: MAIN's mono sum on the left jack, CUE's on the right; the mean of each pair, half up.
  rig.master(gpm::OutMode, 1.0f);
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == ((w0[i] + 1) >> 1));  // main is (x, 0): the mono sum is x / 2
    CHECK(rig.r(i) == ((w1[i] + 1) >> 1));
  }
  // The cue level scales the CUE bus only (curve of 50: -20 dB).
  rig.master(gpm::CueLevel, 50.0f);
  refill();
  rig.render();
  const double g50 = static_cast<double>(rig.g.level_curve().q16(50.0f)) / 65536.0;
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == ((w0[i] + 1) >> 1));
    CHECK(absd(static_cast<double>(rig.r(i)) - static_cast<double>(w1[i]) * g50 / 2.0) <= 1.5);
  }
  // The master level scales MAIN (after the chain), not the cue.
  rig.master(gpm::Volume, 50.0f);
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(absd(static_cast<double>(rig.l(i)) - static_cast<double>(w0[i]) * g50 / 2.0) <= 1.5);
    CHECK(absd(static_cast<double>(rig.r(i)) - static_cast<double>(w1[i]) * g50 / 2.0) <= 1.5);
  }
  // The dry level and its mute scale the strips, not the master level.
  rig.master(gpm::DryMute, 1.0f);
  refill();
  rig.render();
  CHECK(rig.silent());
}

TEST_CASE("graph: the input strip monitors the codec RX through the engine, silent until raised, sample for sample, with sends, CUE and REC") {
  Rng rng(33);
  alignas(8) static std::int16_t take[4 * kB];
  Rig rig(make_graph());
  FxLog fa;
  rig.g.install_send_fx(0, &fx_record, &fa);
  rig.g.set_send_fx_bypass(0, false);
  alignas(8) std::int16_t rx[kBS];
  auto new_rx = [&]() {
    for (std::uint32_t i = 0; i < kBS; ++i) {
      rig.in[i] = rx[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(40001)) - 20000);
    }
  };
  new_rx();
  rig.render();
  CHECK(rig.silent());                      // level 0 at boot: a monitor is never audible by default
  CHECK(rig.report.input_peak > 0u);        // but the RX block is metered: the codec path is seen
  // Raise it (instant): MAIN and CUE are its default routes. Sample n of the RX block is sample n of the TX block.
  rig.strip(kInputStrip, gps::Volume, 100.0f);
  new_rx();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == rx[2u * i]);          // left to left, right to right: the two channels stay apart
    CHECK(rig.r(i) == rx[2u * i + 1u]);
  }
  // CUE through OUT MODE (mono sums of the pair), no MAIN
  rig.strip(kInputStrip, gps::Route, static_cast<float>(route::kCue | route::kRec | route::kSndA));
  rig.strip(kInputStrip, gps::SendA, 100.0f);
  rig.master(gpm::OutMode, 1.0f);
  CHECK(rig.g.rec().arm(take, 2 * kB, 2));
  new_rx();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    const std::int32_t mono = (static_cast<std::int32_t>(rx[2u * i]) + rx[2u * i + 1u] + 1) >> 1;
    CHECK(rig.l(i) == 0);
    CHECK(rig.r(i) == mono);
    CHECK(take[2u * i] == rx[2u * i]);       // REC: the stereo input as it came, at unity
    CHECK(take[2u * i + 1u] == rx[2u * i + 1u]);
    CHECK(fa.last[i] == mono);               // the send is the mono mix
  }
  // A level of 50 scales both channels by the curve; the level ramps in one block when asked to.
  rig.strip(kInputStrip, gps::Volume, 50.0f, 0);
  new_rx();
  rig.render();
  const double g50 = static_cast<double>(rig.g.level_curve().q16(50.0f)) / 65536.0;
  const std::uint32_t last = kB - 1u;
  CHECK(absd(static_cast<double>(rig.r(last)) - (static_cast<double>(rx[2u * last]) + rx[2u * last + 1u]) * g50 / 2.0) <= 1.5);  // at the end of the ramp
  new_rx();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(absd(static_cast<double>(rig.r(i)) - (static_cast<double>(rx[2u * i]) + rx[2u * i + 1u]) * g50 / 2.0) <= 1.5);
  }
  // The USB bit does nothing for the input; its mute silences it.
  rig.strip(kInputStrip, gps::Route, static_cast<float>(route::kAll));
  rig.strip(kInputStrip, gps::Mute, 1.0f);
  new_rx();
  rig.render();
  rig.render();
  CHECK(rig.silent());
}

TEST_CASE("graph: the silence flag writes silence, skips the feeds and the input, keeps the sources on their timelines") {
  Rng rng(34);
  Rig rig(make_graph());
  fill(use_feed(rig, 0), rng, 10000);
  rig.strip(kInputStrip, gps::Volume, 100.0f);
  for (std::uint32_t i = 0; i < kBS; ++i) rig.in[i] = 1000;
  rig.render();
  CHECK_FALSE(rig.silent());
  for (int k = 0; k < 3; ++k) {
    rig.render(ha::kFlagSilence);
    CHECK(rig.silent());
  }
  rig.render();
  CHECK_FALSE(rig.silent());
  // A Sine source stays on its timeline through the mute and fades in again after it: its ramp restarts from zero.
  rig.strip(1, gps::Source, 1.0f);
  rig.render();
  CHECK(rig.g.tone(1).ramp_pos() == kB);
  rig.render(ha::kFlagSilence);
  CHECK(rig.g.tone(1).ramp_pos() == 0u);
  rig.render();
  CHECK(rig.g.tone(1).ramp_pos() == kB);
}

TEST_CASE("graph: configure() decides which tracks sound: the mask") {
  Rng rng(35);
  Rig rig(make_graph());
  for (unsigned t = 0; t < 16; ++t) {
    fill(use_feed(rig, t), rng, 1000);
  }
  rig.render();
  // 12 tracks are active by default: tracks 13..16 are skipped whatever their parameters.
  MeterSnapshot s;
  fill_window(rig);
  CHECK(rig.g.meters().snapshot(s));
  for (unsigned t = 0; t < 12; ++t) CHECK(s.ch[meter_ch::kTrack0 + t].peak > 0u);
  for (unsigned t = 12; t < 16; ++t) CHECK(s.ch[meter_ch::kTrack0 + t].peak == 0u);
  rig.g.configure(0xFFFFu & ~(1u << 2));  // 16 tracks, the third a MIDI track (the app leaves it out of the mask)
  fill_window(rig);
  fill_window(rig);  // the last published window is then wholly after the change
  CHECK(rig.g.meters().snapshot(s));
  CHECK(s.ch[meter_ch::kTrack0 + 2].peak == 0u);   // a MIDI track makes no audio
  CHECK(s.ch[meter_ch::kTrack0 + 3].peak > 0u);
  CHECK(s.ch[meter_ch::kTrack0 + 15].peak > 0u);   // and 16 tracks are possible
  rig.g.configure(0x3u);
  fill_window(rig);
  fill_window(rig);
  CHECK(rig.g.meters().snapshot(s));
  CHECK(s.ch[meter_ch::kTrack0 + 1].peak > 0u);
  CHECK(s.ch[meter_ch::kTrack0 + 3].peak == 0u);
}

// OS-MODSILENT: activate() adds one strip to the mask in force and changes nothing else (the spare strip a module audition plays on, kAuditionTrack).
TEST_CASE("graph: activate() switches on one strip beyond the mask and leaves the others alone") {
  Rng rng(36);
  Rig rig(make_graph());
  for (unsigned t = 0; t < 16; ++t) {
    fill(use_feed(rig, t), rng, 1000);
  }
  rig.g.activate(kAuditionTrack);
  rig.g.activate(99);  // a track the graph does not have: ignored
  fill_window(rig);
  fill_window(rig);
  MeterSnapshot s;
  CHECK(rig.g.meters().snapshot(s));
  for (unsigned t = 0; t < 12; ++t) CHECK(s.ch[meter_ch::kTrack0 + t].peak > 0u);
  for (unsigned t = 12; t < 15; ++t) CHECK(s.ch[meter_ch::kTrack0 + t].peak == 0u);
  CHECK(s.ch[meter_ch::kTrack0 + kAuditionTrack].peak > 0u);
  CHECK(kAuditionTrack == kTracks - 1u);
  // configure() states the whole mask again: the strip is off when the new mask does not name it.
  rig.g.configure(kDefaultActiveMask);
  fill_window(rig);
  fill_window(rig);
  CHECK(rig.g.meters().snapshot(s));
  CHECK(s.ch[meter_ch::kTrack0 + kAuditionTrack].peak == 0u);
}

TEST_CASE("graph: the buses and the master are metered (per channel, exact peaks); the meter mask and the consumer gates") {
  Rig rig(make_graph());
  FxLog fa;
  rig.g.install_send_fx(0, &fx_record, &fa);
  rig.g.set_send_fx_bypass(0, false);
  std::int16_t* x = use_feed(rig, 0);
  rig.strip(0, gps::Route, static_cast<float>(route::kMain | route::kCue | route::kRec | route::kSndA));
  rig.strip(0, gps::SendA, 100.0f);
  rig.strip(0, gps::Pan, 0.0f);
  for (std::uint32_t i = 0; i < kB; ++i) x[i] = static_cast<std::int16_t>(i == 7 ? 12345 : 100);
  fill_window(rig);
  CHECK(window_peak(rig, meter_ch::kTrack0) == 12345u);
  CHECK(window_peak(rig, meter_ch::kMainL) == 12345u);
  CHECK(window_peak(rig, meter_ch::kMainR) == 0u);       // panned hard left
  CHECK(window_peak(rig, meter_ch::kCueL) == 12345u);
  CHECK(window_peak(rig, meter_ch::kRecL) == 12345u);
  CHECK(window_peak(rig, meter_ch::kSndA) == 12345u);
  CHECK(window_peak(rig, meter_ch::kSndB) == 0u);
  CHECK(window_peak(rig, meter_ch::kMasterL) == 12345u);
  CHECK(window_peak(rig, meter_ch::kMasterR) == 0u);

  // The mask: a channel nobody shows is not measured (and costs nothing); the others are.
  rig.g.set_meter_mask((1u << meter_ch::kTrack0) | (1u << meter_ch::kMasterL));
  fill_window(rig);
  fill_window(rig);
  CHECK(window_peak(rig, meter_ch::kTrack0) == 12345u);
  CHECK(window_peak(rig, meter_ch::kMasterL) == 12345u);
  CHECK(window_peak(rig, meter_ch::kMainL) == 0u);
  CHECK(window_peak(rig, meter_ch::kCueL) == 0u);
  CHECK(window_peak(rig, meter_ch::kSndA) == 0u);
  rig.g.set_meter_mask(0);
  fill_window(rig);
  fill_window(rig);
  CHECK(window_peak(rig, meter_ch::kTrack0) == 0u);
  CHECK(window_peak(rig, meter_ch::kMasterL) == 0u);
  CHECK(rig.g.meters().windows() > 4u);  // the windows still run: the UI sees silence, not a stall
  rig.g.set_meter_mask((1u << kMeterChannels) - 1u);

  // The scope and the spectrum are written only while somebody asks for them.
  const std::uint32_t scope0 = rig.g.scope().head();
  const std::uint32_t spec0 = rig.g.spectrum().head();
  rig.render();
  rig.render();
  CHECK(rig.g.scope().head() == scope0);
  CHECK(rig.g.spectrum().head() == spec0);
  rig.g.set_scope_enabled(true);
  rig.render();
  CHECK(rig.g.scope().head() == scope0 + kBS);
  CHECK(rig.g.spectrum().head() == spec0);
  rig.g.set_spectrum_enabled(true);
  rig.render();
  CHECK(rig.g.spectrum().head() == spec0 + kB);
  rig.g.set_scope_enabled(false);
  rig.g.set_spectrum_enabled(false);
}

TEST_CASE("graph: the input monitor's latency through the pipeline is the slots, sample-aligned") {
  // RX block b completes at boundary b; the render of boundary b reads it and writes the TX slot that plays from boundary b + kTxSlots - 1.
  // So a sample captured at position n of its block is heard at position n of the block that starts kTxSlots - 1 boundaries later: with the
  // capture interval (one block before its boundary) that is kTxSlots blocks from capture start to play start, kTxSlots * 64 frames, and
  // nothing inside the graph adds to it.
  struct alignas(ha::kTxRingBytes) Tx {
    std::int16_t s[ha::kTxPhysSlots * ha::kBlockSamples];
  };
  struct alignas(ha::kRxRingBytes) Rx {
    std::int16_t s[ha::kRxPhysSlots * ha::kBlockSamples];
  };
  static Tx tx;
  static Rx rx;
  static o3os::test::StaticSlot<Engine> slot;
  static TapMem taps;
  for (std::int16_t& v : tx.s) v = 0;
  for (std::int16_t& v : rx.s) v = 0;
  g_cycles = 0;
  Engine& engine = slot.make(&fake_cycles, taps);
  CHECK(engine.params().push(ParamChange{strip_param(kInputStrip, gps::Volume), 100.0f, 1u}));
  static Pipeline pipe;
  Pipeline::Config config{};
  config.tx = tx.s;
  config.rx = rx.s;
  config.render = engine.callback();
  config.cycles = &fake_cycles;
  config.cycles_per_block = ha::kCyclesPerBlock;
  pipe.init(config);
  constexpr std::uint32_t kImpulseAt = 21;
  constexpr std::uint32_t kImpulseBlock = 5;  // RX block 5 carries the impulse (L = 20000, R = -20000)
  std::int32_t heard_at = -1;
  std::uint32_t heard_pos = 0;
  for (std::uint32_t b = 1; b <= 16; ++b) {
    std::int16_t* slot_rx = rx.s + (b % ha::kRxPhysSlots) * ha::kBlockSamples;
    for (std::uint32_t i = 0; i < ha::kBlockSamples; ++i) slot_rx[i] = 0;
    if (b == kImpulseBlock) {
      slot_rx[2u * kImpulseAt] = 20000;
      slot_rx[2u * kImpulseAt + 1u] = -20000;
    }
    pipe.on_boundary(b % ha::kTxPhysSlots, (b + 1u) % ha::kRxPhysSlots, 1000u + b * ha::kCyclesPerBlock, 0);
    // what the DAC plays from this boundary on: the slot b mod P
    const std::int16_t* playing = tx.s + (b % ha::kTxPhysSlots) * ha::kBlockSamples;
    for (std::uint32_t i = 0; i < ha::kBlock; ++i) {
      if (playing[2u * i] == 20000 && playing[2u * i + 1u] == -20000 && heard_at < 0) {
        heard_at = static_cast<std::int32_t>(b);
        heard_pos = i;
      }
    }
    pipe.render();
  }
  CHECK(heard_at > 0);
  CHECK(heard_pos == kImpulseAt);                                              // sample-aligned: nothing in the graph delays it
  CHECK(static_cast<std::uint32_t>(heard_at) == kImpulseBlock + ha::kTxSlots - 1u);  // plays kTxSlots - 1 boundaries after its boundary
  CHECK(pipe.status().snapshot().underruns == 0u);
}

TEST_CASE("graph: a block's stem tap and REC take are what the buses carried (StemTap through the OS-006a reader, RecTap's state machine)") {
  Rng rng(36);
  Rig rig(make_graph());
  std::int16_t* x0 = use_feed(rig, 0);
  std::int16_t* x1 = use_feed(rig, 1);
  rig.strip(0, gps::Route, static_cast<float>(route::kMain | route::kUsb | route::kRec));
  rig.strip(1, gps::Route, static_cast<float>(route::kUsb));  // stems only: not in MAIN
  rig.g.stems().set_enabled(true);
  rig.g.stems().set_slot(2, (1u << 0) | (1u << 1));  // channel 2 = T1 + T2 (the saturating sum of the slot)
  rig.g.stems().set_channels(10);
  o3os::usb::StemReader reader(rig.g.stems().ring());
  alignas(4) static std::int16_t take[8 * kB];
  CHECK(rig.g.rec().state() == RecTap::State::Idle);
  CHECK_FALSE(rig.g.rec().arm(take + 1, 4 * kB, 2));  // not 4-byte aligned
  CHECK_FALSE(rig.g.rec().arm(take, 0, 2));
  CHECK_FALSE(rig.g.rec().arm(take, 4 * kB, 3));
  CHECK(rig.g.rec().arm(take, 3 * kB + 10, 1));     // mono take, 3.15 blocks
  CHECK_FALSE(rig.g.rec().arm(take, 4 * kB, 2));      // already armed
  alignas(8) std::int16_t w0[4][kB], w1[4][kB];
  for (int blk = 0; blk < 4; ++blk) {
    fill(x0, rng, 16000);
    fill(x1, rng, 16000);
    std::memcpy(w0[blk], x0, sizeof w0[blk]);
    std::memcpy(w1[blk], x1, sizeof w1[blk]);
    rig.render();
  }
  CHECK(rig.g.rec().state() == RecTap::State::Done);   // the region was full during the fourth block
  CHECK(rig.g.rec().frames() == 3u * kB + 10u);
  CHECK(rig.g.rec().overrun_frames() == kB - 10u);
  for (int blk = 0; blk < 3; ++blk) {
    for (std::uint32_t i = 0; i < kB; ++i) {
      CHECK(take[blk * kB + i] == w0[blk][i]);          // mono: the mean of a centred track's two channels is the track
    }
  }
  rig.g.rec().release();
  CHECK(rig.g.rec().state() == RecTap::State::Idle);
  // Two stem blocks (4 render blocks): channel 0 = T1, channel 1 = T2, channel 2 = their sum, channel 8 = Master L
  alignas(4) std::int16_t stems[o3os::usb::kStemBlockFrames * 10];
  for (int sb = 0; sb < 2; ++sb) {
    CHECK(reader.read_interleaved(stems, o3os::usb::kStemBlockFrames, 10) == o3os::usb::kStemBlockFrames);
    for (int half = 0; half < 2; ++half) {
      for (std::uint32_t i = 0; i < kB; ++i) {
        const std::uint32_t f = half * kB + i;
        CHECK(stems[f * 10u + 0u] == w0[sb * 2 + half][i]);
        CHECK(stems[f * 10u + 1u] == w1[sb * 2 + half][i]);
        CHECK(stems[f * 10u + 2u] == static_cast<std::int16_t>(w0[sb * 2 + half][i] + w1[sb * 2 + half][i]));  // |sum| < 32768 here
        CHECK(stems[f * 10u + 8u] == w0[sb * 2 + half][i]);  // MAIN carries T1 only
        CHECK(stems[f * 10u + 9u] == w0[sb * 2 + half][i]);
      }
    }
  }
  CHECK(reader.dropped_blocks() == 0u);
  // Disabled: nothing is published.
  rig.g.stems().set_enabled(false);
  const std::uint32_t published = rig.g.stems().published();
  rig.render();
  rig.render();
  CHECK(rig.g.stems().published() == published);
}

TEST_CASE("graph: with no routing edit T1..T8 reach S1..S8 (ADR 0012's default mapping) and the other tracks are heard in the master only") {
  Rng rng(61);
  Rig rig(make_graph());
  std::int16_t* x0 = use_feed(rig, 0);   // T1 -> S1
  std::int16_t* x7 = use_feed(rig, 7);   // T8 -> S8
  std::int16_t* x9 = use_feed(rig, 9);   // T10: no slot of its own in profile P10
  rig.g.stems().set_enabled(true);       // the defaults only: P10, the track strips' default route
  o3os::usb::StemReader reader(rig.g.stems().ring());
  alignas(8) std::int16_t w0[4][kB], w7[4][kB], w9[4][kB];
  for (int blk = 0; blk < 4; ++blk) {
    fill(x0, rng, 8000);
    fill(x7, rng, 8000);
    fill(x9, rng, 8000);
    std::memcpy(w0[blk], x0, sizeof w0[blk]);
    std::memcpy(w7[blk], x7, sizeof w7[blk]);
    std::memcpy(w9[blk], x9, sizeof w9[blk]);
    rig.render();
  }
  alignas(4) std::int16_t stems[o3os::usb::kStemBlockFrames * 10];
  for (int sb = 0; sb < 2; ++sb) {
    CHECK(reader.read_interleaved(stems, o3os::usb::kStemBlockFrames, 10) == o3os::usb::kStemBlockFrames);
    for (int half = 0; half < 2; ++half) {
      for (std::uint32_t i = 0; i < kB; ++i) {
        const std::uint32_t f = half * kB + i;
        const int blk = sb * 2 + half;
        CHECK(stems[f * 10u + 0u] == w0[blk][i]);
        CHECK(stems[f * 10u + 7u] == w7[blk][i]);
        CHECK(stems[f * 10u + 1u] == 0);  // T2 is silent
        CHECK(stems[f * 10u + 8u] == w0[blk][i] + w7[blk][i] + w9[blk][i]);  // Master L: every track, T10 included
        CHECK(stems[f * 10u + 9u] == stems[f * 10u + 8u]);
      }
    }
  }
}

TEST_CASE("graph: USB RETURN joins the input strip (saturating), is pulled only while enabled, and a muted block still drains it") {
  Rig rig(make_graph());
  o3os::usb::Counters counters;
  static o3os::usb::ReturnRing ring;
  o3os::usb::ReturnReader reader(ring, counters);
  rig.g.attach_return(&reader);
  rig.strip(kInputStrip, gps::Volume, 100.0f);
  // Prime the ring (288 frames: the first pull trims it to 256 by dropping the oldest 32): frame k is L = 1000 + k, R = -(1000 + k).
  std::uint32_t chunk[48];
  for (unsigned c = 0; c < 6u; ++c) {
    for (unsigned i = 0; i < 48u; ++i) {
      const std::int32_t v = 1000 + static_cast<std::int32_t>(c * 48u + i);
      chunk[i] = static_cast<std::uint16_t>(v) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-v)) << 16);
    }
    ring.write(chunk, 48);
  }
  CHECK_FALSE(rig.g.return_enabled());
  rig.render();  // disabled: the ring is untouched and nothing is added
  CHECK(rig.silent());
  CHECK(ring.size() == 288u);
  rig.g.set_return_enabled(true);
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == 1000 + 32 + static_cast<int>(i));
    CHECK(rig.r(i) == -(1000 + 32 + static_cast<int>(i)));
  }
  // Added to the line-in, saturating at the rails.
  for (std::uint32_t i = 0; i < kB; ++i) {
    rig.in[2u * i] = 32000;
    rig.in[2u * i + 1u] = -32000;
  }
  rig.render();  // the second pull of the stream: the first took frames 32..95
  for (std::uint32_t i = 0; i < kB; ++i) {
    const int ret = 1000 + 32 + static_cast<int>(kB) + static_cast<int>(i);
    CHECK(rig.l(i) == (32000 + ret > 32767 ? 32767 : 32000 + ret));
    CHECK(rig.r(i) == (-32000 - ret < -32768 ? -32768 : -32000 - ret));
  }
  // The underrun mute (kFlagSilence) renders no sound, but the return is still pulled: the ring must not overrun while the render is muted.
  const std::uint32_t before = ring.size();
  rig.render(ha::kFlagSilence);
  CHECK(rig.silent());
  CHECK(ring.size() < before);
  rig.g.set_return_enabled(false);
}

TEST_CASE("graph: the tracker FX's strip gain (channel x global volume) scales the fader, and the panbrello's offset is added to the pan in force (OS-COREFX CF3)") {
  Rng rng(41);
  Rig rig(make_graph());
  std::int16_t* x = use_feed(rig, 0);
  alignas(8) std::int16_t want[kB];
  fill(x, rng, 3000);
  std::memcpy(want, x, sizeof want);
  rig.strip(0, gps::Volume, 100.0f);
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(rig.l(i) == want[i]);
  }
  CHECK(rig.g.track_gain(0) == 1.0f);
  rig.g.set_track_gain(0, 0.25f);  // ramp 0: the engine's smoothing over the block, then steady
  rig.render();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(absd(static_cast<double>(rig.l(i)) - static_cast<double>(want[i]) * 0.25) <= 1.0);
    CHECK(absd(static_cast<double>(rig.r(i)) - static_cast<double>(want[i]) * 0.25) <= 1.0);
  }
  CHECK(rig.g.track_gain(0) == 0.25f);
  rig.g.set_track_gain(0, 7.0f);  // held to 0..1
  CHECK(rig.g.track_gain(0) == 1.0f);
  rig.g.set_track_gain(0, 1.0f);
  rig.g.set_track_pan_mod(0, 25.0f);  // the strip's pan is the centre: 75 puts the left at 1/2
  rig.render();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(absd(static_cast<double>(rig.l(i)) - static_cast<double>(want[i]) * 0.5) <= 1.0);
    CHECK(absd(static_cast<double>(rig.r(i)) - static_cast<double>(want[i])) <= 1.0);
  }
  rig.g.set_track_pan_mod(0, -500.0f);  // held to -100: hard left, the law clamps the sum
  CHECK(rig.g.track_pan_mod(0) == -100.0f);
  rig.g.set_track_pan_mod(0, 0.0f);
  rig.g.set_track_gain(16, 0.5f);  // a track that does not exist: left alone
  CHECK(rig.g.track_gain(16) == 1.0f);
}

TEST_CASE("graph: the tracker FX's strip gain also scales the sends, pre- and post-fader; a fade to 0 leaves the bus inputs silent (OS-COREFX CF3, D3)") {
  Rng rng(43);
  Rig rig(make_graph());
  FxLog fa;
  rig.g.install_send_fx(0, &fx_record, &fa);
  rig.g.set_send_fx_bypass(0, false);
  std::int16_t* x = use_feed(rig, 0);
  rig.strip(0, gps::Route, static_cast<float>(route::kMain | route::kSndA));
  rig.strip(0, gps::SendA, 100.0f);
  rig.strip(0, gps::Volume, 50.0f);
  alignas(8) std::int16_t want[kB];
  auto refill = [&]() {
    fill(x, rng, 30000);
    std::memcpy(want, x, sizeof want);
  };
  const LevelCurve& c = rig.g.level_curve();
  auto expect = [&](std::int32_t g16) {
    for (std::uint32_t i = 0; i < kB; ++i) {
      CHECK(absd(static_cast<double>(fa.last[i]) - static_cast<double>(want[i]) * static_cast<double>(g16) / 65536.0) <= 1.0);
    }
  };
  const std::int32_t full = mul_q16(c.q16(100.0f), c.q16(100.0f));
  refill();
  rig.render();
  expect(full);  // pre-fader (the default): the fader is not in the send
  rig.g.set_track_gain(0, 0.5f);
  rig.render();  // (the engine smooths over the block)
  refill();
  rig.render();
  expect(full / 2);  // the tracker's gain is: it is the note's
  rig.g.set_sends_post_fader(true);
  rig.render();
  refill();
  rig.render();
  expect(mul_q16(full, mul_q16(c.q16(50.0f), 32768)));  // post-fader: the fader and the gain
  rig.g.set_sends_post_fader(false);
  rig.g.set_track_gain(0, 0.0f);  // a global fade to 0: the bus input is silent
  rig.render();
  refill();
  rig.render();
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(fa.last[i] == 0);
  }
}
