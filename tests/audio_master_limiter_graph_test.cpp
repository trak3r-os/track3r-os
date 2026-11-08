// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-LIMIT: the master limiter in the fixed graph (Graph::install_master_limiter), on the host and on the Cortex-M4F (`qemu_audio_limiter`):
//   * a graph with the limiter, fed a signal under the ceiling, is the graph without it, 63 samples late, within one LSB (the float round trip differs from the bus' own rounding on a tie);
//   * the evidence of a hardware session reproduced: twelve loud tracks make a graph without the limiter clip, and with it not one output sample is above the ceiling's code, the MAIN meters count
//     the overload and the master meters do not;
//   * the ParamChanges: the master LIMITER switch, the three controls and the project's LIMITER MODE reach the limiter, out-of-range values are refused and counted, and without a limiter
//     installed they are refused (the switch is only stored);
//   * the report for `diag mix`, and CUE (not in the chain) is not delayed;
//   * OS-DJ A3c: the master DJ filter and the kill EQ run AHEAD of the limiter (kMasterChain), so a limited, flat-topped master never goes over the ceiling while a kill is held or the filter is
//     near its open end, and with both at rest the graph is the graph without them, bit for bit.
#include <doctest/doctest.h>

#include <o3os/audio/graph.h>
#include <o3os/audio/master_dj_filter.h>
#include <o3os/audio/master_kill_eq.h>
#include <o3os/audio/master_limiter.h>
#include <o3os/hal/audio.h>

#include "support/rng.h"
#include "support/static_slot.h"

#include <cstdint>
#include <cstring>

namespace {

namespace ha = o3os::hal::audio;
using namespace o3os::audio;
using o3os::test::Rng;
namespace gps = o3os::audio::graph_param::strip;
namespace gpm = o3os::audio::graph_param::master;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;
constexpr std::int32_t kCeilingCode = 31658;  // 0.96605 * 32768 = 31,657.1, and the conversion's rounding

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

// Two graphs may be alive at once (the one without the limiter is the reference), so each has its own slot and its own tap memory.
Graph& make_graph(unsigned which) {
  static o3os::test::StaticSlot<Graph> slot[2];
  static TapMem taps[2];
  g_cycles = 0;
  return slot[which].make(&fake_cycles, taps[which]);
}
MasterLimiter& make_limiter() {
  static o3os::test::StaticSlot<MasterLimiter> slot;
  return slot.make();
}

struct Rig {
  Graph& g;
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  alignas(8) std::int16_t feed_[kTracks][kB] = {};
  ha::BlockReport report = {};
  explicit Rig(Graph& graph) : g(graph) {}
  void render() {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0u, &report};
    g.render(io);
  }
  void set(o3os::core::ParamId id, float v) { CHECK(g.params().push(ParamChange{id, v, 1})); }
  std::int16_t* use_feed(unsigned t) {
    set(strip_param(t, gps::Source), 3.0f);
    g.feed(t, feed_[t]);
    return feed_[t];
  }
};

// The same content for two graphs: per track a block of noise within +-amp.
void noise_block(std::int16_t* block, Rng& rng, std::int32_t amp) {
  for (std::uint32_t i = 0; i < kB; ++i) {
    block[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(static_cast<std::uint32_t>(2 * amp + 1))) - amp);
  }
}

}  // namespace

TEST_CASE("limiter in the graph: under the ceiling it is the graph without it, kLatency samples late") {
  MasterLimiter& lim = make_limiter();
  Rig plain(make_graph(0));
  Rig limited(make_graph(1));
  limited.g.install_master_limiter(lim);
  CHECK(limited.g.master_stage_active(MasterStage::Limiter));  // on at once: the base limiter is always on
  CHECK_FALSE(plain.g.master_stage_active(MasterStage::Limiter));
  constexpr unsigned kBlocks = 40;
  static std::int16_t ref[kBlocks * kBS], got[kBlocks * kBS];
  Rng ra(5), rb(5);
  for (unsigned t = 0; t < 3; ++t) {
    (void)plain.use_feed(t);
    (void)limited.use_feed(t);
    plain.set(strip_param(t, gps::Pan), 20.0f + 30.0f * static_cast<float>(t));
    limited.set(strip_param(t, gps::Pan), 20.0f + 30.0f * static_cast<float>(t));
  }
  for (unsigned b = 0; b < kBlocks; ++b) {
    for (unsigned t = 0; t < 3; ++t) {
      noise_block(plain.feed_[t], ra, 9000);  // three of them cannot reach the ceiling
      noise_block(limited.feed_[t], rb, 9000);
    }
    plain.render();
    limited.render();
    std::memcpy(ref + b * kBS, plain.out, sizeof plain.out);
    std::memcpy(got + b * kBS, limited.out, sizeof limited.out);
  }
  std::uint32_t bad = 0;
  std::int32_t worst = 0;
  for (std::uint32_t n = 0; n < kBlocks * kB; ++n) {
    for (unsigned ch = 0; ch < 2; ++ch) {
      const std::int32_t want = n >= MasterLimiter::kLatency ? ref[2u * (n - MasterLimiter::kLatency) + ch] : 0;
      std::int32_t d = static_cast<std::int32_t>(got[2u * n + ch]) - want;
      d = d < 0 ? -d : d;
      worst = d > worst ? d : worst;
      bad += d > 1 ? 1u : 0u;
    }
  }
  CHECK(bad == 0u);
  CHECK(worst <= 1);
  CHECK(lim.gain() == 1.0f);
}

TEST_CASE("limiter in the graph: twelve loud tracks clip without it and do not with it; the MAIN meters count the overload") {
  MasterLimiter& lim = make_limiter();
  Rig plain(make_graph(0));
  Rig limited(make_graph(1));
  limited.g.install_master_limiter(lim);
  limited.g.configure(0x0FFFu);
  plain.g.configure(0x0FFFu);
  Rng ra(11), rb(11);
  for (unsigned t = 0; t < 12; ++t) {
    (void)plain.use_feed(t);
    (void)limited.use_feed(t);
    const float pan = static_cast<float>((t * 37u) % 101u);
    plain.set(strip_param(t, gps::Pan), pan);
    limited.set(strip_param(t, gps::Pan), pan);
  }
  std::uint32_t plain_full = 0, limited_full = 0;
  std::int32_t limited_peak = 0;
  for (unsigned b = 0; b < 400; ++b) {
    for (unsigned t = 0; t < 12; ++t) {
      noise_block(plain.feed_[t], ra, 20000);
      noise_block(limited.feed_[t], rb, 20000);
    }
    plain.render();
    limited.render();
    for (std::uint32_t i = 0; i < kBS; ++i) {
      plain_full += (plain.out[i] >= 32767 || plain.out[i] <= -32768) ? 1u : 0u;
      limited_full += (limited.out[i] >= 32767 || limited.out[i] <= -32768) ? 1u : 0u;
      const std::int32_t v = limited.out[i] < 0 ? -static_cast<std::int32_t>(limited.out[i]) : limited.out[i];
      limited_peak = v > limited_peak ? v : limited_peak;
    }
  }
  CHECK(plain_full > 100u);     // the unit's evidence: a hard clip of the master
  CHECK(limited_full == 0u);
  CHECK(limited_peak <= kCeilingCode);
  CHECK(limited_peak >= 30000);  // and the limiter does not give the level away

  MeterSnapshot sp, sl;
  CHECK(plain.g.meters().snapshot(sp));
  CHECK(limited.g.meters().snapshot(sl));
  CHECK(sp.ch[meter_ch::kMainL].clips > 0u);    // without a limiter MAIN is metered after the bus' saturation
  CHECK(sl.ch[meter_ch::kMainL].clips > 0u);    // with one, before it: the overload the limiter caught
  CHECK(sl.ch[meter_ch::kMainR].clips > 0u);
  CHECK(sl.ch[meter_ch::kMasterL].clips == 0u);  // what is heard does not clip
  CHECK(sl.ch[meter_ch::kMasterR].clips == 0u);
  CHECK(sl.ch[meter_ch::kMasterL].peak <= static_cast<std::uint32_t>(kCeilingCode));
  CHECK(sp.ch[meter_ch::kMasterL].clips > 0u);

  const LimiterReport r = limited.g.limiter_report();
  CHECK(r.windows > 10u);
  CHECK(r.reduced > 10u);
  CHECK(r.gain_hold_q16 < 40000u);  // the sum of twelve noise tracks is several times full scale at its peaks
  const LimiterReport none = plain.g.limiter_report();
  CHECK(none.windows == 0u);
  CHECK(none.gain_now_q16 == 65536u);
  CHECK(none.mode == 0u);
}

TEST_CASE("limiter in the graph: the ParamChanges reach the limiter; out-of-range values are refused and counted") {
  MasterLimiter& lim = make_limiter();
  Rig rig(make_graph(1));
  rig.g.install_master_limiter(lim);
  CHECK_FALSE(lim.switched_on());
  rig.set(master_param(gpm::Limiter), 1.0f);
  rig.set(master_param(gpm::LimiterThreshold), 60.0f);
  rig.set(master_param(gpm::LimiterAttack), 25.0f);
  rig.set(master_param(gpm::LimiterRelease), 250.0f);
  rig.set(o3os::core::param_id(o3os::core::ParamKind::Project, 0, graph_param::project::LimiterMode), 1.0f);
  rig.render();
  CHECK(lim.switched_on());
  CHECK(lim.mode() == 1u);
  CHECK(rig.g.limiter_report().mode == 1u);  // (`diag mix` says it)
  CHECK(rig.g.params_applied() == 5u);
  CHECK(rig.g.params_rejected() == 0u);
  // refused: out of range, and not 0 or 1
  rig.set(master_param(gpm::LimiterThreshold), 0.0f);
  rig.set(master_param(gpm::LimiterThreshold), 101.0f);
  rig.set(master_param(gpm::LimiterAttack), 1001.0f);
  rig.set(master_param(gpm::LimiterRelease), 0.5f);
  rig.set(master_param(gpm::Limiter), 2.0f);
  rig.set(o3os::core::param_id(o3os::core::ParamKind::Project, 0, graph_param::project::LimiterMode), 2.0f);
  rig.render();
  CHECK(rig.g.params_rejected() == 6u);
  CHECK(lim.switched_on());
  CHECK(lim.mode() == 1u);
  // and back
  rig.set(master_param(gpm::Limiter), 0.0f);
  rig.set(o3os::core::param_id(o3os::core::ParamKind::Project, 0, graph_param::project::LimiterMode), 0.0f);
  rig.render();
  CHECK_FALSE(lim.switched_on());
  CHECK(lim.mode() == 0u);
}

TEST_CASE("limiter in the graph: the controls act: a threshold under the signal holds the output to it") {
  MasterLimiter& lim = make_limiter();
  Rig rig(make_graph(1));
  rig.g.install_master_limiter(lim);
  (void)rig.use_feed(0);
  rig.set(strip_param(0, gps::Pan), 50.0f);
  rig.set(master_param(gpm::Limiter), 1.0f);
  rig.set(master_param(gpm::LimiterThreshold), 40.0f);
  rig.set(master_param(gpm::LimiterAttack), 1.0f);
  for (std::uint32_t i = 0; i < kB; ++i) {
    rig.feed_[0][i] = 24000;  // DC at 0.73
  }
  std::int32_t last = 0;
  for (unsigned b = 0; b < 600; ++b) {
    rig.render();
    last = rig.out[2u * (kB - 1u)];
  }
  CHECK(last == doctest::Approx(0.4 * 32768.0).epsilon(0.01));
  rig.set(master_param(gpm::Limiter), 0.0f);  // OFF: the base limiter alone: 0.73 is under the ceiling and goes through
  for (unsigned b = 0; b < 100; ++b) {
    rig.render();
    last = rig.out[2u * (kB - 1u)];
  }
  CHECK(last == 24000);
}

TEST_CASE("limiter in the graph: CUE is not in the chain, so it is not delayed; without a limiter the controls are refused") {
  MasterLimiter& lim = make_limiter();
  Rig rig(make_graph(1));
  rig.g.install_master_limiter(lim);
  (void)rig.use_feed(0);
  rig.set(strip_param(0, gps::Route), static_cast<float>(route::kCue));  // CUE only
  rig.set(strip_param(0, gps::Pan), 50.0f);
  rig.set(master_param(gpm::OutMode), 1.0f);  // MAIN (mono) on the left jack, CUE on the right
  for (std::uint32_t i = 0; i < kB; ++i) {
    rig.feed_[0][i] = i == 5 ? 20000 : 0;
  }
  rig.render();
  CHECK(rig.out[2u * 5u + 1u] == 20000);  // the right jack, in the block the sample came in
  CHECK(rig.out[2u * 5u] == 0);

  Rig bare(make_graph(0));  // no limiter installed
  bare.set(master_param(gpm::Limiter), 1.0f);
  bare.set(master_param(gpm::LimiterThreshold), 50.0f);
  bare.set(o3os::core::param_id(o3os::core::ParamKind::Project, 0, graph_param::project::LimiterMode), 1.0f);
  bare.render();
  CHECK(bare.g.params_applied() == 3u);
  CHECK(bare.g.params_rejected() == 2u);  // the switch is stored; the threshold and the mode have nowhere to go
  CHECK_FALSE(bare.g.master_stage_active(MasterStage::Limiter));
}

// ---- OS-DJ A3c: the DJ filter and the kill EQ run ahead of the limiter ---------------------------------------------------------------------------------------------------------------------------
// The kill EQ's all-pass sum raises the peaks of a limited mix (analytically up to +6.6 dB on a ceiling-level square) and the DJ filter rings 4 to 31 % over an edge near its open end; behind the
// limiter the converter would clip that. A hot master is four tracks of one full-scale waveform at the centre (the sum is about 2.8 x full scale, so the limiter holds flat tops at the ceiling);
// the waveforms have no libm in them (squares, a saw, two squares summed), so the test runs on the Cortex-M4F too.

namespace {

enum class Wave : std::uint8_t { Square100, Square400, Square1k, Saw, TwoSquares };

std::int16_t wave_at(Wave w, std::uint32_t n) {
  switch (w) {
    case Wave::Square100: return (n % 441u) < 220u ? 32767 : -32767;
    case Wave::Square400: return (n % 110u) < 55u ? 32767 : -32767;
    case Wave::Square1k: return (n % 44u) < 22u ? 32767 : -32767;
    case Wave::Saw: return static_cast<std::int16_t>(-32767 + static_cast<std::int32_t>((n % 100u) * 655u));
    case Wave::TwoSquares: {  // about 180 Hz and 2.8 kHz, each half scale and the sum clipped at full scale by a gain: flat tops
      const std::int32_t sum = ((n % 245u) < 122u ? 22000 : -22000) + ((n % 16u) < 8u ? 22000 : -22000);
      return static_cast<std::int16_t>(sum > 32767 ? 32767 : (sum < -32767 ? -32767 : sum));
    }
  }
  return 0;
}

struct HotMaster {
  Rig rig;
  std::uint32_t n = 0;
  explicit HotMaster(Graph& g, Wave w) : rig(g), wave(w) {
    for (unsigned t = 0; t < 4; ++t) {
      (void)rig.use_feed(t);
      rig.set(strip_param(t, gps::Pan), 50.0f);
    }
  }
  // One block; the largest |sample| of both channels is returned.
  std::int32_t block() {
    for (std::uint32_t i = 0; i < kB; ++i) {
      const std::int16_t v = wave_at(wave, n++);
      for (unsigned t = 0; t < 4; ++t) {
        rig.feed_[t][i] = v;
      }
    }
    rig.render();
    std::int32_t peak = 0;
    for (std::uint32_t i = 0; i < kBS; ++i) {
      const std::int32_t v = rig.out[i] < 0 ? -static_cast<std::int32_t>(rig.out[i]) : rig.out[i];
      peak = v > peak ? v : peak;
    }
    return peak;
  }
  Wave wave;
};

constexpr unsigned kHotBlocks = 120;  // 174 ms: the DJ stage's ramp, the limiter's settling, many periods of the lowest waveform

}  // namespace

TEST_CASE("limiter in the graph (OS-DJ A3c): the kill EQ and the DJ filter sit ahead of the limiter in the chain") {
  Graph& g = make_graph(1);
  MasterLimiter& lim = make_limiter();
  g.install_master_limiter(lim);
  CHECK(kMasterChain[2].id == MasterStage::Eq);
  CHECK(kMasterChain[3].id == MasterStage::Bass);
  CHECK(kMasterChain[4].id == MasterStage::Insert);  // OS-INSFX S6: the master insert, also ahead of the limiter
  CHECK(kMasterChain[5].id == MasterStage::Limiter);
  CHECK_FALSE(g.master_stage_active(MasterStage::Eq));  // installed, bypassed: nothing runs at rest
  CHECK_FALSE(g.master_stage_active(MasterStage::Bass));
}

TEST_CASE("limiter in the graph (OS-DJ A3c): a limited flat-topped master never goes over the ceiling with a kill held or the DJ filter near open") {
  struct Setting {
    unsigned kill;  // the KillEq mask
    float knob;     // the DjFilter knob
  };
  // every kill alone and all together, the low-pass from just open to a quarter way, the high-pass from just open to half way, and a kill with a filter
  const Setting settings[] = {{1u, 0.0f},  {2u, 0.0f},  {4u, 0.0f},  {7u, 0.0f},  {0u, -1.0f}, {0u, -5.0f},
                              {0u, -10.0f}, {0u, -20.0f}, {0u, -30.0f}, {0u, 5.0f},  {0u, 50.0f},  {2u, -10.0f}};
  std::int32_t overall = 0;
  const Wave waves[] = {Wave::Square100, Wave::Square400, Wave::Square1k, Wave::Saw, Wave::TwoSquares};
  for (const Wave w : waves) {
    for (const Setting& s : settings) {
      MasterLimiter& lim = make_limiter();
      Graph& g = make_graph(1);
      g.install_master_limiter(lim);
      HotMaster hot(g, w);
      for (unsigned b = 0; b < 8; ++b) {
        (void)hot.block();  // the limiter settles on the hot master with the stages at rest
      }
      hot.rig.set(master_param(gpm::KillEq), static_cast<float>(s.kill));
      hot.rig.set(master_param(gpm::DjFilter), s.knob);
      std::int32_t peak = 0;
      for (unsigned b = 0; b < kHotBlocks; ++b) {
        const std::int32_t p = hot.block();
        peak = p > peak ? p : peak;
        if (b == 3u) {
          CHECK(g.master_stage_active(MasterStage::Eq) == (s.kill != 0u));  // (the stage is really in while it is held)
          CHECK(g.master_stage_active(MasterStage::Bass) == (s.knob != 0.0f));
        }
      }
      CHECK_MESSAGE(peak <= kCeilingCode, "wave " << static_cast<int>(w) << " kill " << s.kill << " knob " << s.knob << " peak " << peak);
      CHECK_MESSAGE(peak >= 1000, "wave " << static_cast<int>(w) << " kill " << s.kill << " knob " << s.knob << " peak " << peak);  // (something sounded)
      CHECK(g.params_rejected() == 0u);
      overall = peak > overall ? peak : overall;
    }
  }
  CHECK(overall >= 20000);  // the hot master did reach the ceiling's neighbourhood somewhere
}

TEST_CASE("limiter in the graph (OS-DJ A3c): with the kill EQ and the DJ filter at rest the graph is the graph without them, bit for bit; released, they put themselves away") {
  const Wave waves[] = {Wave::Square400, Wave::Saw, Wave::TwoSquares};
  constexpr unsigned kRestBlocks = 40;  // 58 ms (the buffers stay small for the Cortex-M4 target suite)
  static std::int16_t ref[kRestBlocks * kBS];
  for (const Wave w : waves) {
    {
      MasterLimiter& lim = make_limiter();
      Graph& g = make_graph(0);
      g.install_master_limiter(lim);
      g.install_master_stage(MasterStage::Eq, nullptr, nullptr);  // the two slots empty: the chain as it was before the DJ page
      g.install_master_stage(MasterStage::Bass, nullptr, nullptr);
      HotMaster hot(g, w);
      for (unsigned b = 0; b < kRestBlocks; ++b) {
        (void)hot.block();
        std::memcpy(ref + b * kBS, hot.rig.out, sizeof hot.rig.out);
      }
    }
    MasterLimiter& lim = make_limiter();
    Graph& g = make_graph(1);
    g.install_master_limiter(lim);
    HotMaster hot(g, w);  // the stages as `init_defaults` leaves them: installed, bypassed
    unsigned differing = 0;
    for (unsigned b = 0; b < kRestBlocks; ++b) {
      (void)hot.block();
      differing += std::memcmp(ref + b * kBS, hot.rig.out, sizeof hot.rig.out) != 0 ? 1u : 0u;
    }
    CHECK_MESSAGE(differing == 0u, "wave " << static_cast<int>(w) << ": " << differing << " blocks differ");
  }
  // held and let go: both stages run down and put themselves in bypass again
  MasterLimiter& lim = make_limiter();
  Graph& g = make_graph(1);
  g.install_master_limiter(lim);
  HotMaster hot(g, Wave::Square400);
  hot.rig.set(master_param(gpm::KillEq), 2.0f);
  hot.rig.set(master_param(gpm::DjFilter), -10.0f);
  for (unsigned b = 0; b < 20; ++b) {
    (void)hot.block();
  }
  CHECK(g.master_stage_active(MasterStage::Eq));
  CHECK(g.master_stage_active(MasterStage::Bass));
  hot.rig.set(master_param(gpm::KillEq), 0.0f);
  hot.rig.set(master_param(gpm::DjFilter), 0.0f);
  for (unsigned b = 0; b < 40; ++b) {
    (void)hot.block();
  }
  CHECK_FALSE(g.master_stage_active(MasterStage::Eq));
  CHECK_FALSE(g.master_stage_active(MasterStage::Bass));
  CHECK(g.kill_eq() == 0u);
  CHECK(g.dj_filter() == 0.0f);
}

TEST_CASE("limiter in the graph (OS-DJ rt): the DJ page open at rest - every strip at the fader's centre cut, the filter and the kill told 0 - calls no master stage but the limiter, and the strips settle") {
  MasterLimiter& lim = make_limiter();
  Graph& g = make_graph(1);
  g.install_master_limiter(lim);
  HotMaster hot(g, Wave::Saw);
  for (unsigned t = 0; t < kTracks; ++t) {
    hot.rig.set(strip_param(t, gps::XfGain), 180.0f);  // dj_level(A or B, centre, Smooth): what the page told when it came into view
  }
  hot.rig.set(master_param(gpm::DjFilter), 0.0f);
  hot.rig.set(master_param(gpm::KillEq), 0.0f);
  for (unsigned b = 0; b < 40; ++b) {
    (void)hot.block();
  }
  for (unsigned t = 0; t < kTracks; ++t) {
    CHECK(g.xf_gain(t) == 180);
  }
  for (unsigned s = 0; s < kMasterStages; ++s) {
    const auto id = static_cast<MasterStage>(s);
    CHECK_MESSAGE(g.master_stage_active(id) == (id == MasterStage::Limiter), "master stage " << s);
  }
}
