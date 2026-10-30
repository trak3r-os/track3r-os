// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host benchmarks of the fixed audio graph (audio/graph.h, OS-005b): RELATIVE figures only. The host is not the MK66 and has no DSP extension
// (the production kernels run their instruction model here), so what counts is the shape: how the cost of one block divides over the graph's
// stages, how the paired loops compare with the scalar references, and what a ramp costs against a steady gain. The Cortex-M4F cycle figures
// are a hw-session item (docs/specs/OS-005-audio-engine.md 005b; the budget table is in platform/mk66/README.md, HW-A).
//
//   o3os_audio_bench [--smoke]    the full table (build with -DCMAKE_BUILD_TYPE=Release: the sanitizer build is for correctness); --smoke
//                                 runs every case a handful of times, as ctest does
#include <o3os/audio/bench_stereo.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/kernels.h>
#include <o3os/audio/loop_player.h>
#include <o3os/audio/master_limiter.h>
#include <o3os/audio/metro.h>
#include <o3os/audio/send_delay.h>
#include <o3os/audio/send_reverb.h>
#include <o3os/audio/tap_mem.h>
#include <o3os/audio/tracker_fx.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace o3os::audio;
namespace ha = o3os::hal::audio;
using Clock = std::chrono::steady_clock;

namespace {

bool g_smoke = false;
volatile std::int64_t g_sink = 0;

constexpr std::uint32_t kB = ha::kBlock;

// The graph's stage stamps in nanoseconds: the cycle counter is the steady clock.
std::uint32_t clock_ns() noexcept {
  return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

struct Lcg {
  std::uint32_t s = 12345u;
  std::uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
};

template <typename Fn>
double best_ns(unsigned reps, unsigned inner, Fn fn) {
  double best = 1e18;
  for (unsigned r = 0; r < (g_smoke ? 1u : reps); ++r) {
    const Clock::time_point t0 = Clock::now();
    for (unsigned i = 0; i < (g_smoke ? 3u : inner); ++i) {
      fn();
    }
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / static_cast<double>(g_smoke ? 3u : inner);
    best = ns < best ? ns : best;
  }
  return best;
}

void kernel_table() {
  alignas(8) std::int16_t src[kB], dst[kB], l[kB], r[kB];
  alignas(8) std::int32_t acc[kB];
  alignas(8) float fl[kB];
  alignas(8) std::int16_t inter[2 * kB];
  Lcg rng;
  for (std::uint32_t i = 0; i < kB; ++i) {
    src[i] = static_cast<std::int16_t>(rng.next());
    l[i] = static_cast<std::int16_t>(rng.next());
    r[i] = static_cast<std::int16_t>(rng.next());
    acc[i] = 0;
    fl[i] = static_cast<float>(src[i]) * (1.0f / 32768.0f);
  }
  for (std::uint32_t i = 0; i < 2 * kB; ++i) {
    inter[i] = static_cast<std::int16_t>(rng.next());
  }
  std::printf("kernels, ns per %u-frame block (best of runs; host, the paired loops on the instruction model):\n", static_cast<unsigned>(kB));
  std::printf("  %-26s %9s %9s %8s\n", "kernel", "reference", "paired", "ratio");
  auto row = [](const char* name, double ref, double fast) { std::printf("  %-26s %9.1f %9.1f %8.2f\n", name, ref, fast, ref / fast); };
  constexpr unsigned kReps = 40, kInner = 4000;
  row("gain steady", best_ns(kReps, kInner, [&] { kernel::gain_ref(dst, src, kB, 40000, 40000); g_sink += dst[3]; }),
      best_ns(kReps, kInner, [&] { kernel::gain(dst, src, kB, 40000, 40000); g_sink += dst[3]; }));
  row("gain ramp", best_ns(kReps, kInner, [&] { kernel::gain_ref(dst, src, kB, 65536, 20000); g_sink += dst[3]; }),
      best_ns(kReps, kInner, [&] { kernel::gain(dst, src, kB, 65536, 20000); g_sink += dst[3]; }));
  row("mac steady", best_ns(kReps, kInner, [&] { kernel::mac_ref(acc, src, kB, 40000, 40000); g_sink += acc[3]; }),
      best_ns(kReps, kInner, [&] { kernel::mac(acc, src, kB, 40000, 40000); g_sink += acc[3]; }));
  row("mac ramp", best_ns(kReps, kInner, [&] { kernel::mac_ref(acc, src, kB, 0, 65536); g_sink += acc[3]; }),
      best_ns(kReps, kInner, [&] { kernel::mac(acc, src, kB, 0, 65536); g_sink += acc[3]; }));
  alignas(8) std::int32_t acc2[kB] = {}, acc3[kB] = {};
  alignas(8) std::int16_t x1[kB], x2[kB];
  alignas(8) float fr[kB], fl2[kB];
  // The strip kernels work in place on the track's block, so each timed call is preceded by the copy of a fresh block (the same in every row:
  // the ratios are not affected, the absolute figure carries the 128-byte copy).
  const auto fresh = [&] {
    std::memcpy(x1, src, sizeof x1);
    std::memcpy(x2, l, sizeof x2);
  };
  using kernel::GainRamp;
  using kernel::StripLane;
  const GainRamp unity{65536, 65536}, steady{50000, 50000}, up{0, 65536}, down{65536, 30000};
  row("level + pan (steady)", best_ns(kReps, kInner, [&] { fresh(); kernel::gain_pan_ref(StripLane{x1, steady, unity, GainRamp{30000, 30000}}, acc, acc2, kB); g_sink += acc[3]; }),
      best_ns(kReps, kInner, [&] { fresh(); kernel::gain_pan(StripLane{x1, steady, unity, GainRamp{30000, 30000}}, acc, acc2, kB); g_sink += acc[3]; }));
  row("level + pan (ramping)", best_ns(kReps, kInner, [&] { fresh(); kernel::gain_pan_ref(StripLane{x1, down, up, GainRamp{65536, 0}}, acc, acc2, kB); g_sink += acc[3]; }),
      best_ns(kReps, kInner, [&] { fresh(); kernel::gain_pan(StripLane{x1, down, up, GainRamp{65536, 0}}, acc, acc2, kB); g_sink += acc[3]; }));
  row("level + centre (steady)", best_ns(kReps, kInner, [&] { fresh(); kernel::gain_mac_ref(StripLane{x1, steady, unity, unity}, acc3, kB); g_sink += acc3[3]; }),
      best_ns(kReps, kInner, [&] { fresh(); kernel::gain_mac(StripLane{x1, steady, unity, unity}, acc3, kB); g_sink += acc3[3]; }));
  row("2 x level + pan (steady)", best_ns(kReps, kInner, [&] { fresh(); kernel::gain_pan2_ref(StripLane{x1, steady, unity, GainRamp{30000, 30000}}, StripLane{x2, steady, GainRamp{20000, 20000}, unity}, acc, acc2, kB); g_sink += acc[3]; }),
      best_ns(kReps, kInner, [&] { fresh(); kernel::gain_pan2(StripLane{x1, steady, unity, GainRamp{30000, 30000}}, StripLane{x2, steady, GainRamp{20000, 20000}, unity}, acc, acc2, kB); g_sink += acc[3]; }));
  row("2 x level + centre (steady)", best_ns(kReps, kInner, [&] { fresh(); kernel::gain_mac2_ref(StripLane{x1, steady, unity, unity}, StripLane{x2, steady, unity, unity}, acc3, kB); g_sink += acc3[3]; }),
      best_ns(kReps, kInner, [&] { fresh(); kernel::gain_mac2(StripLane{x1, steady, unity, unity}, StripLane{x2, steady, unity, unity}, acc3, kB); g_sink += acc3[3]; }));
  {
    // the cost of the copy alone, to subtract by eye from the strip rows
    const double copy = best_ns(kReps, kInner, [&] { fresh(); g_sink += x1[3]; });
    std::printf("  %-26s %9.1f %9s %8s   (the per-call copy inside the strip rows)\n", "fresh-block copy", copy, "", "");
  }
  row("sat_clear", best_ns(kReps, kInner, [&] { kernel::sat_clear_ref(dst, acc, kB); g_sink += dst[3]; }),
      best_ns(kReps, kInner, [&] { kernel::sat_clear(dst, acc, kB); g_sink += dst[3]; }));
  row("sat_clear_main (centre)", best_ns(kReps, kInner, [&] { kernel::sat_clear_main_ref(dst, l, acc, acc2, acc3, kB); g_sink += dst[3]; }),
      best_ns(kReps, kInner, [&] { kernel::sat_clear_main(dst, l, acc, acc2, acc3, kB); g_sink += dst[3]; }));
  row("acc_to_float_main", best_ns(kReps, kInner, [&] { kernel::acc_to_float_main_ref(fr, fl2, acc, acc2, acc3, kB); g_sink += static_cast<std::int64_t>(fr[3]); }),
      best_ns(kReps, kInner, [&] { kernel::acc_to_float_main(fr, fl2, acc, acc2, acc3, kB); g_sink += static_cast<std::int64_t>(fr[3]); }));
  row("mono_mix", best_ns(kReps, kInner, [&] { kernel::mono_mix_ref(dst, l, r, kB); g_sink += dst[3]; }),
      best_ns(kReps, kInner, [&] { kernel::mono_mix(dst, l, r, kB); g_sink += dst[3]; }));
  row("deinterleave2", best_ns(kReps, kInner, [&] { kernel::deinterleave2_ref(l, r, inter, kB); g_sink += l[3]; }),
      best_ns(kReps, kInner, [&] { kernel::deinterleave2(l, r, inter, kB); g_sink += l[3]; }));
  row("meter (peak + sum sq)", best_ns(kReps, kInner, [&] { kernel::MeterAcc a{0, 0}; kernel::meter_ref(src, kB, a); g_sink += a.peak; }),
      best_ns(kReps, kInner, [&] { kernel::MeterAcc a{0, 0}; kernel::meter(src, kB, a); g_sink += a.peak; }));
  row("q15 -> float", best_ns(kReps, kInner, [&] { kernel::q15_to_float_ref(fl, src, kB); g_sink += static_cast<std::int64_t>(fl[3]); }),
      best_ns(kReps, kInner, [&] { kernel::q15_to_float(fl, src, kB); g_sink += static_cast<std::int64_t>(fl[3]); }));
  row("float -> q15", best_ns(kReps, kInner, [&] { kernel::float_to_q15_ref(dst, fl, kB); g_sink += dst[3]; }),
      best_ns(kReps, kInner, [&] { kernel::float_to_q15(dst, fl, kB); g_sink += dst[3]; }));
}

// OS-METRO: the click (audio/metro.h). A block with the click sounding is the most it costs (the first block of a click also starts the resonator: trigger() + mix() below includes that); a block
// without one is the busy() test Graph::render makes. The click sounds in 11 blocks of every beat, so a 120 BPM song pays the first figure in 3 % of its blocks.
void metro_table() {
  Metro m;
  (void)m.set_mode(1.0f);
  (void)m.set_level(100.0f);
  alignas(4) static std::int16_t blk[2 * kB];
  for (std::uint32_t i = 0; i < 2 * kB; ++i) {
    blk[i] = static_cast<std::int16_t>(i * 97u);
  }
  constexpr unsigned kReps = 40, kInner = 4000;
  const double on = best_ns(kReps, kInner, [&] { m.trigger(0, true); m.mix(blk, kB, true, true); g_sink += blk[3]; });
  const double off = best_ns(kReps, kInner, [&] { g_sink += m.busy() ? 1 : 0; });
  std::printf("metro (OS-METRO), ns per %u-frame block: click sounding (both channels, trigger included) %.1f, idle (the busy() test) %.1f\n", static_cast<unsigned>(kB), on, off);
}

// OS-LOOPER L1: the loop player (audio/loop_player.h). A block with a loop playing (restarted each block, so the whole block is audible: the most it costs; it includes the restart's bookkeeping) against a
// block with no loop (the needs_service() test Graph::render makes). The player is in flash on the MK66 and reads its take from SDRAM there: the host figure is a shape, not the unit's cycles.
void loop_table() {
  static std::vector<std::int16_t> take(1u << 17);
  RecTap tap;
  LoopPlayer loop;
  alignas(4) static std::int16_t blk[2 * kB];
  alignas(4) static std::int16_t l[kB];
  for (std::uint32_t i = 0; i < kB; ++i) {
    l[i] = static_cast<std::int16_t>(i * 311u);
  }
  loop.capture(1);
  loop.service(blk, kB, false, 65536, tap);
  (void)tap.arm(take.data(), static_cast<std::uint32_t>(take.size()), 1, RecTap::Align::Step, 1);
  tap.on_step(0, 0);
  for (unsigned b = 0; b < 400; ++b) {
    tap.push(l, l, kB);
  }
  tap.on_step(1, 5);
  tap.push(l, l, kB);
  loop.service(blk, kB, false, 65536, tap);
  constexpr unsigned kReps = 40, kInner = 4000;
  const double on = best_ns(kReps, kInner, [&] {
    loop.on_step(0, 0);
    for (std::uint32_t i = 0; i < 2 * kB; ++i) {
      blk[i] = 0;
    }
    loop.service(blk, kB, false, 65536, tap);
    g_sink += blk[7];
  });
  const double zero = best_ns(kReps, kInner, [&] {
    for (std::uint32_t i = 0; i < 2 * kB; ++i) {
      blk[i] = 0;
    }
    g_sink += blk[7];
  });
  LoopPlayer idle;
  const double off = best_ns(kReps, kInner, [&] { g_sink += idle.needs_service() ? 1 : 0; });
  std::printf("loop (OS-LOOPER L1), ns per %u-frame block: loop playing (restart, both channels, the block clear taken off) %.1f, idle (the needs_service() test) %.1f\n", static_cast<unsigned>(kB), on - zero, off);
}

void fx_copy(void*, const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    out_l[i] = in[i];
    out_r[i] = in[i];
  }
}

// OS-SENDFX: the two send effects (audio/send_delay.h, send_reverb.h) one block at a time. The host figure is the host's (no DSP extension, an out-of-order x86: RELATIVE, like everything
// here); the Cortex-M4F column is an ESTIMATE, not a measurement: the effects compiled for the MK66 at -O3 (arm-none-eabi-gcc 15.3, -mcpu=cortex-m4 -mfpu=fpv4-sp-d16), the instructions of each
// inner loop counted from the disassembly and priced (load 2, store 1, VADD/VMUL/VCVT/VMOV 1, VFMA 3, SSAT 1, VDIV and VSQRT 14), plus the staging copies at 2 to 3 cycles a sample, per
// frame; SDRAM wait states are NOT in it (unmeasured, HW-B). The DWT measurement on the unit is the hardware session's row. A block is 261,120 cycles at 44,117 Hz.
struct FxCost {
  const char* name;
  std::uint32_t m4_cycles_per_frame;  // the estimate, see above
};
constexpr FxCost kFxEstimates[] = {
    {"delay, ping-pong, steady", 85},        // 59 the loop (both channels) + 13 the lanes + 13 the two staging copies
    {"delay, a time cross-fading", 145},     // + the old taps' read and the blend, for the 1,024 frames of a fade
    {"reverb FULL (44 kHz), steady", 369},   // the plate: input block 88, the tank 2 x 92, the taps 84, the pre-delay and the cross feeds 13
    {"reverb HALF (22 kHz), steady", 237},   // half of the plate's 374 a half-rate sample, plus the converters: the decimator 20 and the interpolator 30 a frame
};

void sendfx_table() {
  static std::vector<std::uint32_t> ring(SendDelay::kRingFrames);
  static std::vector<std::int16_t> mem(SendReverb::kMemSamples);
  auto delay = std::make_unique<SendDelay>();
  auto reverb = std::make_unique<SendReverb>();
  Lcg rng;
  alignas(8) std::int16_t in[kB], l[kB], r[kB];
  for (std::int16_t& v : in) {
    v = static_cast<std::int16_t>(rng.next());
  }
  namespace sx = o3os::audio::sendfx;
  constexpr unsigned kReps = 30, kInner = 600;
  const double cycles_block = 261120.0;
  std::printf("\nsend effects, per %u-frame block (host ns; the Cortex-M4F column is an estimate, see the source; 100 %% = %.0f cycles):\n", static_cast<unsigned>(kB), cycles_block);
  std::printf("  %-32s %10s %12s %9s\n", "effect", "host ns", "M4F cycles", "of block");
  const auto row = [&](unsigned est_index, double ns) {
    const double cyc = static_cast<double>(kFxEstimates[est_index].m4_cycles_per_frame) * static_cast<double>(kB);
    std::printf("  %-32s %10.0f %12.0f %8.1f%%\n", kFxEstimates[est_index].name, ns, cyc, 100.0 * cyc / cycles_block);
  };
  delay->attach(ring.data(), SendDelay::kRingFrames);
  (void)delay->set_param(sx::delay::PingPong, 1.0f);
  (void)delay->set_param(sx::delay::Time, 400.0f);
  row(0, best_ns(kReps, kInner, [&] { delay->process(in, l, r, kB); g_sink += l[3]; }));
  bool flip = false;
  row(1, best_ns(kReps, kInner, [&] {
        flip = !flip;
        (void)delay->set_param(sx::delay::Time, flip ? 400.0f : 600.0f);  // a fade of 1,024 frames is never over before the next request
        delay->process(in, l, r, kB);
        g_sink += l[3];
      }));
  reverb->attach(mem.data(), SendReverb::kMemSamples);
  (void)reverb->set_param(sx::reverb::Quality, 1.0f);
  row(2, best_ns(kReps, kInner, [&] { reverb->process(in, l, r, kB); g_sink += l[3]; }));
  (void)reverb->set_param(sx::reverb::Quality, 0.0f);
  row(3, best_ns(kReps, kInner, [&] { reverb->process(in, l, r, kB); g_sink += l[3]; }));
  std::printf("  (the two together, the delay steady and the reverb HALF: %.1f %% of a block by the estimate; ADR 0013 D1 budgets 1.5 %% and 3 %%)\n",
              100.0 * static_cast<double>(kFxEstimates[0].m4_cycles_per_frame + kFxEstimates[3].m4_cycles_per_frame) * static_cast<double>(kB) / cycles_block);
}

// OS-LIMIT: the master limiter (audio/master_limiter.h) one block at a time. The host figure is the host's (RELATIVE, like everything here); the Cortex-M4F column is an ESTIMATE, not a
// measurement, made as the send effects' above: master_limiter.cpp compiled for the MK66 at -O3 (arm-none-eabi-gcc 15.3, -mcpu=cortex-m4 -mfpu=fpv4-sp-d16), the instructions of each loop counted
// from the disassembly and priced (load 2, store 1, VADD/VSUB/VMUL/VABS/VCVT/VMOV 1, VFMA 3, VCMP 1, VMRS 2, IT and its op 2, taken branch 3, VDIV 14), per frame, with the block's setup:
//   quiet, OFF   the peak pass 21 + the shifted delay line 22 = 43, + setup and report 2          (nothing is being reduced: the delay line only)
//   quiet, ON    + the follower 15                                                                (a block under the threshold and the ceiling)
//   active, OFF  the peak pass 21 + the fast stage's loop 65 + the suffix pass 12 + setup 2 = 100  (a window of the block needed a reduction, or the release is still running)
//   active, ON   + the follower and the slow stage's gain 11, and a division and its three products on a reduced sample (25) = 136
// SRAM is not the limiter's own cost here (its 1.1 KiB is SRAM_U, the code runs from flash) and the SDRAM is not touched. The first hardware session's DWT row measures it; the measured cycle model of the
// graph has been 1.6x the model so far (OS-STATUS), so read these times 1.6.
constexpr FxCost kLimiterEstimates[] = {
    {"limiter, quiet block, switch OFF", 45},
    {"limiter, quiet block, switch ON", 61},
    {"limiter, reducing, switch OFF", 100},
    {"limiter, reducing, ON (every sample reduced)", 136},
};

void limiter_table() {
  auto limiter = std::make_unique<MasterLimiter>();
  Lcg rng;
  alignas(8) float l[kB], r[kB], quiet_l[kB], quiet_r[kB], loud_l[kB], loud_r[kB];
  for (std::uint32_t i = 0; i < kB; ++i) {
    quiet_l[i] = (static_cast<float>(rng.next() & 0xFFFFu) / 32768.0f - 1.0f) * 0.3f;
    quiet_r[i] = (static_cast<float>(rng.next() & 0xFFFFu) / 32768.0f - 1.0f) * 0.3f;
    loud_l[i] = (static_cast<float>(rng.next() & 0xFFFFu) / 32768.0f - 1.0f) * 3.0f;  // +10 dB: every window needs a reduction
    loud_r[i] = (static_cast<float>(rng.next() & 0xFFFFu) / 32768.0f - 1.0f) * 3.0f;
  }
  constexpr unsigned kReps = 30, kInner = 600;
  const double cycles_block = 261120.0;
  std::printf("\nmaster limiter, per %u-frame block (host ns; the Cortex-M4F column is an estimate, see the source; 100 %% = %.0f cycles):\n", static_cast<unsigned>(kB), cycles_block);
  std::printf("  %-48s %10s %12s %9s %9s\n", "case", "host ns", "M4F cycles", "of block", "x 1.6");
  const auto row = [&](unsigned est_index, double ns) {
    const double cyc = static_cast<double>(kLimiterEstimates[est_index].m4_cycles_per_frame) * static_cast<double>(kB);
    std::printf("  %-48s %10.0f %12.0f %8.1f%% %8.1f%%\n", kLimiterEstimates[est_index].name, ns, cyc, 100.0 * cyc / cycles_block, 160.0 * cyc / cycles_block);
  };
  const auto run = [&](const float* in_l, const float* in_r) {
    return best_ns(kReps, kInner, [&] {
      std::memcpy(l, in_l, sizeof l);
      std::memcpy(r, in_r, sizeof r);
      limiter->process(l, r, kB);
      g_sink += static_cast<std::int64_t>(l[3] * 1000.0f);
    });
  };
  namespace lm = o3os::audio::limiter;
  limiter->reset();
  row(0, run(quiet_l, quiet_r));
  (void)limiter->set_param(lm::Switch, 1.0f);
  row(1, run(quiet_l, quiet_r));
  (void)limiter->set_param(lm::Switch, 0.0f);
  row(2, run(loud_l, loud_r));
  (void)limiter->set_param(lm::Switch, 1.0f);
  (void)limiter->set_param(lm::Threshold, 40.0f);
  row(3, run(loud_l, loud_r));
}

struct Scenario {
  const char* name;
  unsigned tracks;
  bool ramping;  // a pan target every block, so every strip's pan gains ramp
  bool taps;     // stems, REC, scope and spectrum on: every consumer asking
  bool centre;   // every track panned at the centre (one multiply a sample, the centre accumulator)
  bool sends;    // each track sends to both effects
  bool meters;   // all 28 meters measured (false: none: a screen that shows none)
};

void stage_table(const Scenario& sc) {
  static o3os::audio::TapMem taps;
  auto graph = std::make_unique<Graph>(&clock_ns, taps);
  Graph& g = *graph;
  Lcg rng;
  alignas(8) std::int16_t in[ha::kBlockSamples];
  alignas(8) std::int16_t out[ha::kBlockSamples];
  alignas(8) static std::int16_t feed[kTracks][kB];
  for (std::int16_t& v : in) {
    v = static_cast<std::int16_t>(rng.next());
  }
  namespace gps = graph_param::strip;
  auto push = [&](o3os::core::ParamId id, float v, std::uint32_t ramp) { (void)g.params().push(ParamChange{id, v, ramp}); };
  const std::uint32_t routes = route::kMain | route::kCue | (sc.sends ? route::kSndA | route::kSndB : 0u) | (sc.taps ? route::kRec | route::kUsb : 0u);
  for (unsigned t = 0; t < sc.tracks; ++t) {
    push(strip_param(t, gps::Source), 3.0f, 1);
    push(strip_param(t, gps::Route), static_cast<float>(routes), 1);
    push(strip_param(t, gps::SendA), sc.sends ? 60.0f : 0.0f, 1);
    push(strip_param(t, gps::SendB), sc.sends ? 40.0f : 0.0f, 1);
    push(strip_param(t, gps::Pan), sc.centre ? 50.0f : static_cast<float>((t * 17u) % 101u), 1);
    push(strip_param(t, gps::Volume), 80.0f, 1);
    g.feed(t, feed[t]);
  }
  push(strip_param(kInputStrip, gps::Volume), 90.0f, 1);
  push(strip_param(kInputStrip, gps::Route), static_cast<float>(route::kMain | route::kCue | (sc.sends ? route::kSndA : 0u)), 1);
  push(strip_param(kInputStrip, gps::SendA), sc.sends ? 50.0f : 0.0f, 1);
  if (sc.sends) {
    g.install_send_fx(0, &fx_copy, nullptr);
    g.set_send_fx_bypass(0, false);
    g.install_send_fx(1, &fx_copy, nullptr);
    g.set_send_fx_bypass(1, false);
  }
  g.configure(sc.tracks >= 32u ? 0xFFFFFFFFu : ((1u << sc.tracks) - 1u));
  g.set_meter_mask(sc.meters ? (1u << kMeterChannels) - 1u : 0u);
  if (sc.taps) {
    g.stems().set_enabled(true);
    g.set_scope_enabled(true);
    g.set_spectrum_enabled(true);
  }
  static std::vector<std::int16_t> take;
  take.assign(2u * 1000u * kB, 0);
  if (sc.taps) {
    (void)g.rec().arm(take.data(), 1000u * kB, 2);
  }

  const unsigned blocks = g_smoke ? 50u : 6000u;
  std::vector<double> total, stage[ha::kStageCommit];
  ha::BlockReport report{};
  for (unsigned b = 0; b < blocks; ++b) {
    for (unsigned t = 0; t < sc.tracks; ++t) {
      for (std::uint32_t i = 0; i < kB; ++i) {
        feed[t][i] = static_cast<std::int16_t>(rng.next());
      }
      if (sc.ramping) {
        push(strip_param(t, gps::Pan), static_cast<float>(((b + t) * 29u) % 101u), 0);
      }
    }
    if (sc.taps && g.rec().state() == RecTap::State::Done) {
      g.rec().release();
      (void)g.rec().arm(take.data(), 1000u * kB, 2);
    }
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0u, &report};
    g.render(io);
    g_sink += out[17];
    if (b >= 5) {  // let the setup settle
      std::uint32_t sum = 0;
      for (unsigned s = 0; s < ha::kStageCommit; ++s) {
        stage[s].push_back(static_cast<double>(report.stage_cycles[s]));
        sum += report.stage_cycles[s];
      }
      total.push_back(static_cast<double>(sum));
    }
  }
  auto median = [](std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
  };
  const double t_med = median(total);
  static const char* const names[] = {"input", "drain", "render (sources)", "strips", "buses", "master", "convert", "taps"};
  std::printf("\n%s: %u tracks, input monitor%s%s%s%s; ns per block by stage (median of %zu blocks)\n", sc.name, sc.tracks, sc.sends ? ", 2 send effects" : "",
              sc.centre ? ", all centred" : "", sc.taps ? ", stems + REC + scope + spectrum" : "", sc.meters ? ", 28 meters" : ", meters off", total.size());
  for (unsigned s = 0; s < ha::kStageCommit; ++s) {
    const double m = median(stage[s]);
    std::printf("  %-18s %9.0f ns  %5.1f %% of the render\n", names[s], m, t_med > 0 ? 100.0 * m / t_med : 0.0);
  }
  std::printf("  %-18s %9.0f ns  (the host's ratio between the rows is the figure, not the unit)\n", "total", t_med);
}

}  // namespace

// OS-COREFX CF2: the tracker FX processor's tick pass (audio/tracker_fx.h) over 12 tracks that each hold a voice and an effect. It is per TICK (50 to 300 a second), not per sample: no kernel of the
// render changed. The host figure is the host's (RELATIVE); the Cortex-M4F column is an ESTIMATE made from CF1's own: a track with a pitch change (one log2, one exp2, a voice lookup, regain) was
// estimated at about 600 cycles in docs/specs/OS-COREFX.md section 4; the rows scale that figure by the host time of the case against the host time of the fine-porta + volume-slide row, with a floor
// of 80 cycles a track (the voice lookup, the compare and the switch). It is therefore an UPPER bound for the rows that do no log2 / exp2 (every case is priced at about CF1's cost a track). The tick pass of 12 tracks at 125 BPM (50 ticks a second) is then a fraction of a percent of the 180 MHz CPU.
void tracker_fx_table() {
  using o3os::audio::EventKind;
  using o3os::audio::SeqEvent;
  using o3os::audio::TrackerFx;
  namespace fx = o3os::audio;
  static std::int16_t pcm[4000];
  for (unsigned i = 0; i < 4000; ++i) {
    pcm[i] = static_cast<std::int16_t>(((i * 37u) & 0x3FFFu) - 8192);
  }
  auto pool = std::make_unique<VoicePool>();
  auto tfx = std::make_unique<TrackerFx>();
  SampleDesc d{};
  d.data = pcm;
  d.length = 4000;
  d.loop_start = 0;
  d.loop_end = 4000;
  d.gain = 65536;
  pool->set_sample(0, d);
  for (unsigned t = 0; t < 12; ++t) {
    pool->note_on(t, 60 + t, 0, 100);
  }
  const auto set_all = [&](unsigned type, unsigned value) {
    for (unsigned t = 0; t < 12; ++t) {
      SeqEvent e{};
      e.kind = EventKind::TrkFx;
      e.track = static_cast<std::uint8_t>(t);
      e.a = static_cast<std::uint8_t>(type);
      e.b = static_cast<std::uint8_t>(value);
      tfx->event(e, *pool);
    }
  };
  unsigned tick_no = 0;
  const auto tick = [&] {
    SeqEvent e{};
    e.kind = EventKind::Tick;
    e.track = fx::kNoTrack;
    e.a = static_cast<std::uint8_t>(tick_no++ % 6u);
    e.b = 6;
    tfx->event(e, *pool);
  };
  struct Row {
    const char* name;
    unsigned type, value;
    double ns;
  } rows[] = {
      {"idle (12 tracks, no effect)", 0, 0, 0.0},
      {"fine porta + volume slide (CF1)", 0, 0, 0.0},
      {"retrigger Q23 (restart + volume code)", fx::kFxTrkRetrig, 0x21, 0.0},
      {"tremor I11 (gate, volume 0 / back)", fx::kFxTrkTremor, 0x11, 0.0},
      {"pan slide P04 (no graph attached)", fx::kFxTrkPanSlide, 0x04, 0.0},
  };
  set_all(0, 0);
  rows[0].ns = best_ns(20, 4000, [&] { tick(); });
  set_all(fx::kFxTrkVolSlide, 0x01);  // (a volume change each tick: regain; the pitch row below adds the log2 and exp2 of a period change)
  {
    for (unsigned t = 0; t < 12; ++t) {
      SeqEvent e{};
      e.kind = EventKind::TrkFx;
      e.track = static_cast<std::uint8_t>(t);
      e.a = fx::kFxTrkFinePortaDown;
      e.b = 1;
      e.flags = 1;
      tfx->event(e, *pool);
    }
  }
  rows[1].ns = best_ns(20, 2000, [&] { tick(); });
  for (unsigned r = 2; r < 5; ++r) {
    set_all(0, 0);
    for (unsigned t = 0; t < 12; ++t) {  // (the second column's CF1 effect off again)
      SeqEvent e{};
      e.kind = EventKind::TrkFx;
      e.track = static_cast<std::uint8_t>(t);
      e.a = 0;
      e.flags = 1;
      tfx->event(e, *pool);
    }
    set_all(rows[r].type, rows[r].value);
    rows[r].ns = best_ns(20, 2000, [&] { tick(); });
  }
  std::printf("\ntracker FX tick pass (OS-COREFX), 12 tracks with a voice each, per Tick event (host ns; M4F cycles an estimate from CF1's ~600 a pitch-changing track, see the source):\n");
  std::printf("  %-42s %10s %12s %12s\n", "case", "host ns", "M4F cycles", "% CPU @50/s");
  for (const Row& r : rows) {
    const double cyc = std::max(12.0 * 80.0, 12.0 * 600.0 * r.ns / rows[1].ns);
    std::printf("  %-42s %10.0f %12.0f %11.3f%%\n", r.name, r.ns, cyc, 100.0 * cyc * 50.0 / 180e6);
  }
  g_sink += static_cast<std::int64_t>(tfx->volume(0));
}

// OS-LFOLIVE / OS-BUG-WTLFO: what the LFO modulators of the sampler voices cost a block (host ns; relative figures, the unit's are measured with `diag cpu`, docs/specs/OS-LFOLIVE.md), and what a live
// edit of an LFO field costs once. 12 voices of one looping instrument. Rows: no modulator; the five slots that run per voice (pan, finetune, tremolo, cutoff, wavetable window) as saw LFOs with the
// transport running; the same with it STOPPED (the clock runs on, OS-BUG-WTLFO: one extra compare a span); the same running with an edit of a field every block (nothing else is added to the block:
// an edit is one descriptor write and a loop over 12 voices, once per ParamChange).
void voice_lfo_table() {
  static std::int16_t pcm[4096];
  static std::int16_t stack[8 * 256];
  for (unsigned i = 0; i < 4096; ++i) {
    pcm[i] = static_cast<std::int16_t>(((i * 37u) & 0x3FFFu) - 8192);
  }
  for (unsigned i = 0; i < 8u * 256u; ++i) {
    stack[i] = static_cast<std::int16_t>((i / 256u + 1u) * 1000u);
  }
  const auto lfo = [](unsigned rate) {
    FilterMod m;
    m.kind = kFmodLfo;
    m.shape = 1;
    m.rate = static_cast<std::uint8_t>(rate);
    m.amount = 0.8f;
    m.lfo_amount = 0.8f;
    m.lfo = true;
    return m;
  };
  struct Row {
    const char* name;
    bool lfos;
    bool stopped;
    bool edit;
    double ns;
  } rows[] = {{"12 voices, no modulator", false, false, false, 0.0},
              {"12 voices, 5 LFOs each, transport running", true, false, false, 0.0},
              {"12 voices, 5 LFOs each, transport stopped", true, true, false, 0.0},
              {"... running, an LFO field edited every block", true, false, true, 0.0}};
  for (Row& r : rows) {
    auto pool = std::make_unique<VoicePool>();
    SampleDesc d{};
    d.data = pcm;
    d.length = 4096;
    d.loop_end = 4096;
    d.gain = 65536;
    d.filter.enable = d.filter.wanted = true;
    d.filter.cutoff = 0.6f;
    if (r.lfos) {
      d.filter.mod = lfo(7);
      d.pan_mod = lfo(8);
      d.tune_mod = lfo(9);
      d.amp_mod = lfo(5);
      d.wt_size = 256;
      d.wt_windows = 8;
      d.data = stack + 3 * 256;
      d.length = 256;
      d.loop_end = 256;
      d.wt_pos = 3;
      d.pos_mod = lfo(7);
    }
    pool->set_sample(0, d);
    pool->set_tempo_x100(12000);
    pool->restart_clock();
    if (r.stopped) {
      pool->transport_stopped();
    } else {
      pool->on_step(0);
    }
    for (unsigned t = 0; t < 12; ++t) {
      pool->note_on(t, 48 + t, 0, 100);
    }
    unsigned n = 0;
    r.ns = best_ns(15, 400, [&] {
      if (r.edit) {
        std::uint32_t pan = 0;
        pool->apply_param(0, inst_param::lfo(2, inst_param::LfoSpeed), static_cast<float>(5u + (n++ & 1u)), pan);
      }
      if (!r.stopped && (n & 31u) == 0u) {
        pool->on_step(n);
      }
      pool->begin_block();
      pool->render(0, kB);
      pool->end_block();
    });
    g_sink += pool->feed(0) != nullptr ? pool->feed(0)[0] : 0;
  }
  std::printf("\nvoice LFO modulators (OS-LFOLIVE, OS-BUG-WTLFO): ns a block of 12 voices (host; the M4F cost is measured on the unit)\n");
  std::printf("  %-48s %10s %10s\n", "case", "host ns", "vs none");
  for (const Row& r : rows) {
    std::printf("  %-48s %10.0f %9.2fx\n", r.name, r.ns, r.ns / rows[0].ns);
  }
}

// OS-PERF-CPU2 (A1): what a reversed voice (PLAYBACK, Voice::mode 4: run_dir) costs against a forward one (run_steady), 12 one-shot voices of a plain sample played at the root and a fifth above (host
// ns a block, relative figures; the unit's are measured with `diag cpu` and `diag bench voice_rev`). The voices are started again when they have ended, so every block renders 12 sounding voices.
void voice_reverse_table() {
  static std::int16_t pcm[65536];
  for (unsigned i = 0; i < 65536u; ++i) {
    pcm[i] = static_cast<std::int16_t>(((i * 37u) & 0x3FFFu) - 8192);
  }
  struct Row {
    const char* name;
    bool reversed;
    unsigned note;
    double ns;
  } rows[] = {{"12 forward one-shot voices, root", false, 60, 0.0},
              {"12 reversed one-shot voices, root", true, 60, 0.0},
              {"12 forward one-shot voices, a fifth up", false, 67, 0.0},
              {"12 reversed one-shot voices, a fifth up", true, 67, 0.0}};
  for (Row& r : rows) {
    auto pool = std::make_unique<VoicePool>();
    SampleDesc d{};
    d.data = pcm;
    d.length = 65536;
    d.gain = 65536;
    pool->set_sample(0, d);
    if (r.reversed) {
      SeqEvent e{};
      e.kind = EventKind::ModSet;
      e.flags = kModGroupReverse;
      e.c = kLayerPerf;
      e.value = 1;
      for (unsigned t = 0; t < 12; ++t) {
        e.track = static_cast<std::uint8_t>(t);
        pool->fx_event(t, e);
      }
    }
    const auto start = [&] {
      for (unsigned t = 0; t < 12; ++t) {
        pool->note_on(t, r.note, 0, 100);
      }
    };
    start();
    r.ns = best_ns(15, 400, [&] {
      if (!pool->track_playing(0)) {
        start();
      }
      pool->begin_block();
      pool->render(0, kB);
      pool->end_block();
    });
    g_sink += pool->feed(0) != nullptr ? pool->feed(0)[0] : 0;
  }
  std::printf("\nreversed voices (OS-PERF-CPU2 A1): ns a block of 12 one-shot voices (host; the M4F cost is measured on the unit)\n");
  std::printf("  %-48s %10s %12s\n", "case", "host ns", "vs forward");
  for (unsigned i = 0; i < 4u; ++i) {
    std::printf("  %-48s %10.0f %11.2fx\n", rows[i].name, rows[i].ns, rows[i].ns / rows[i & 2u].ns);
  }
}

// OS-PERF-CPU2 (P1): what each LFO destination costs through the whole engine (the voices, then Engine::apply_pan_mods and the graph's pan retarget for the pan), 12 voices of one looping instrument with
// one modulator each, the transport running (host ns a block, relative figures; the unit's are `diag bench voice_pan` and `diag cpu`). The same table before and after "the pan is sent only when it
// changed" is the figure of P1: the pan row's pan value is a whole number 0..100 that repeats for several blocks at this rate.
void voice_lfo_dest_table() {
  static std::int16_t pcm[4096];
  static std::int16_t stack[8 * 256];
  for (unsigned i = 0; i < 4096; ++i) {
    pcm[i] = static_cast<std::int16_t>(((i * 37u) & 0x3FFFu) - 8192);
  }
  for (unsigned i = 0; i < 8u * 256u; ++i) {
    stack[i] = static_cast<std::int16_t>((i / 256u + 1u) * 1000u);
  }
  const auto lfo = [](unsigned rate) {
    FilterMod m;
    m.kind = kFmodLfo;
    m.shape = 1;
    m.rate = static_cast<std::uint8_t>(rate);
    m.amount = 0.8f;
    m.lfo_amount = 0.8f;
    m.lfo = true;
    return m;
  };
  enum Dest : unsigned { kNone, kAmp, kPan, kTune, kCut, kPos, kPosNone };
  struct Row {
    const char* name;
    Dest dest;
    double ns;
  } rows[] = {{"12 voices, no modulator", kNone, 0.0},
              {"... VOL LFO (tremolo)", kAmp, 0.0},
              {"... PAN LFO", kPan, 0.0},
              {"... TUNE LFO (finetune)", kTune, 0.0},
              {"... FLT LFO (cutoff)", kCut, 0.0},
              {"12 wavetable voices, no modulator", kPosNone, 0.0},
              {"... POS LFO (window)", kPos, 0.0}};
  static o3os::audio::TapMem taps;
  alignas(8) static std::int16_t in[ha::kBlockSamples];
  alignas(8) static std::int16_t out[ha::kBlockSamples];
  for (Row& r : rows) {
    auto pool = std::make_unique<VoicePool>();
    auto engine = std::make_unique<Engine>(&clock_ns, taps);
    SampleDesc d{};
    d.data = pcm;
    d.length = 4096;
    d.loop_end = 4096;
    d.gain = 65536;
    d.filter.enable = d.filter.wanted = true;
    d.filter.cutoff = 0.6f;
    if (r.dest == kPos || r.dest == kPosNone) {
      d.wt_size = 256;
      d.wt_windows = 8;
      d.data = stack + 3 * 256;
      d.length = 256;
      d.loop_end = 256;
      d.wt_pos = 3;
    }
    switch (r.dest) {
      case kAmp: d.amp_mod = lfo(5); break;
      case kPan: d.pan_mod = lfo(8); break;
      case kTune: d.tune_mod = lfo(9); break;
      case kCut: d.filter.mod = lfo(7); break;
      case kPos: d.pos_mod = lfo(7); break;
      default: break;
    }
    pool->set_sample(0, d);
    pool->set_tempo_x100(12000);
    pool->restart_clock();
    pool->on_step(0);
    engine->attach_voices(pool.get());
    for (unsigned t = 0; t < 12; ++t) {
      pool->note_on(t, 48 + t, 0, 100);
    }
    unsigned n = 0;
    r.ns = best_ns(15, 200, [&] {
      if ((++n & 31u) == 0u) {
        pool->on_step(n);
      }
      ha::BlockReport report{};
      const ha::BlockIo io{in, out, 0u, &report};
      engine->render_block(io);
      g_sink += out[17];
    });
  }
  std::printf("\nvoice LFO destinations through the engine (OS-PERF-CPU2 P1): ns a block of 12 voices (host; the M4F cost is measured on the unit)\n");
  std::printf("  %-48s %10s %10s\n", "case", "host ns", "vs none");
  for (const Row& r : rows) {
    std::printf("  %-48s %10.0f %9.2fx\n", r.name, r.ns, r.ns / rows[r.dest == kPos || r.dest == kPosNone ? 5 : 0].ns);
  }
}

// OS-STEREO step 1: the sample loop of a stereo voice (M and S, one position, L = M + S, R = M - S) against the plain voice's mono loop (audio/bench_stereo.h; the unit's rows are `diag bench voice_st`:
// voice_mono and voice_st, both in flash). Host ns a block of 64 frames, relative figures: the host has no DSP extension and no wait states. Exits 1 when a kernel differs from the plain
// reference, so the smoke run is also the host proof of the loops.
__attribute__((noinline)) void st_mono_block(const std::int16_t* m, std::int16_t* ol, bench_st::Pos& p, std::uint64_t inc) {
  bench_st::resample<false>(m, nullptr, ol, nullptr, kB, p, inc, 10066329);
}
__attribute__((noinline)) void st_stereo_block(const std::int16_t* m, const std::int16_t* s, std::int16_t* ol, std::int16_t* orr, bench_st::Pos& p, std::uint64_t inc) {
  bench_st::resample<true>(m, s, ol, orr, kB, p, inc, 10066329);
}

void voice_stereo_table() {
  constexpr std::uint32_t kFrames = 32768;
  constexpr std::uint32_t kMaxRead = 2u * kB + 2u;
  constexpr std::uint64_t kInc = (std::uint64_t{1} << 32) + 0x12345678u;
  constexpr std::int32_t kAmp = 10066329;
  static std::int16_t m[kFrames];
  static std::int16_t s[kFrames];
  Lcg rng;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    m[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.next() & 0xFFFFu) / 2 - 16384);
    s[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.next() & 0xFFFFu) / 2 - 16384);
  }
  alignas(8) std::int16_t ol[kB];
  alignas(8) std::int16_t orr[kB];
  alignas(8) std::int16_t rl[kB];
  alignas(8) std::int16_t rr[kB];
  for (unsigned round = 0; round < 64u; ++round) {
    const bench_st::Pos at{(round * 397u) % (kFrames - kMaxRead), round * 0x9E3779B9u};
    bench_st::Pos q = at;
    bench_st::reference(m, nullptr, rl, nullptr, kB, at, kInc, kAmp);
    st_mono_block(m, ol, q, kInc);
    bool ok = std::memcmp(rl, ol, sizeof rl) == 0;
    q = at;
    bench_st::reference(m, s, rl, rr, kB, at, kInc, kAmp);
    st_stereo_block(m, s, ol, orr, q, kInc);
    ok = ok && std::memcmp(rl, ol, sizeof rl) == 0 && std::memcmp(rr, orr, sizeof rr) == 0;
    if (!ok) {
      std::fprintf(stderr, "voice_stereo_table: a stereo-voice kernel differs from its reference (round %u)\n", round);
      std::exit(1);
    }
  }
  bench_st::Pos p{0u, 0u};
  const auto advance = [&] {
    if (p.idx + kMaxRead >= kFrames) {
      p = bench_st::Pos{0u, 0u};
    }
  };
  const double mono = best_ns(15, 4000, [&] {
    advance();
    st_mono_block(m, ol, p, kInc);
  });
  g_sink += ol[0];
  p = bench_st::Pos{0u, 0u};
  const double stereo = best_ns(15, 4000, [&] {
    advance();
    st_stereo_block(m, s, ol, orr, p, kInc);
  });
  g_sink += ol[0] + orr[0];
  std::printf("\nstereo voice loop (OS-STEREO step 1): ns a block of 64 frames, kernel against kernel (host; the M4F cycles are `diag bench voice_st`)\n");
  std::printf("  %-48s %10s %12s\n", "case", "host ns", "vs mono");
  std::printf("  %-48s %10.0f %11.2fx\n", "mono: M to the output (the plain voice's loop)", mono, 1.0);
  std::printf("  %-48s %10.0f %11.2fx\n", "stereo: M and S, one position, L = M+S, R = M-S", stereo, stereo / mono);
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    g_smoke = g_smoke || std::strcmp(argv[i], "--smoke") == 0;
  }
  kernel_table();
  metro_table();
  loop_table();
  sendfx_table();
  limiter_table();
  tracker_fx_table();
  voice_lfo_table();
  voice_reverse_table();
  voice_stereo_table();
  voice_lfo_dest_table();
  //                    name                     tracks ramping taps   centre sends  meters
  stage_table(Scenario{"input monitor only",          0,     false,  false, false, false, true});
  stage_table(Scenario{"steady, no sends, no consumers", 12, false,  false, false, false, false});
  stage_table(Scenario{"steady, meters on",           12,    false,  false, false, false, true});
  stage_table(Scenario{"steady, centred",             12,    false,  false, true,  false, true});
  stage_table(Scenario{"steady, with sends",          12,    false,  false, false, true,  true});
  stage_table(Scenario{"steady, every consumer",      12,    false,  true,  false, true,  true});
  stage_table(Scenario{"every pan ramping",           12,    true,   true,  false, true,  true});
  stage_table(Scenario{"16 tracks, ramping",          16,    true,   true,  false, true,  true});
  std::printf("\n(sink %lld)\n", static_cast<long long>(g_sink));
  return 0;
}
