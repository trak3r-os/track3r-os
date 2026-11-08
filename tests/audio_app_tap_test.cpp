// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H2a: the apps' audio tap (audio/app_tap.h) -- the ring and the per-block level stats against a plain model, the reader's cursors (new data only, lapped, wrapped), the graph feeding
// it from the master pair or the input bus only while a source is selected, the cost counters and their reset, and the diag line. Host and Cortex-M4F; the concurrent case is in
// audio_taps_thread_test.cpp (host, TSan).
#include <doctest/doctest.h>

#include <o3os/audio/app_tap.h>
#include <o3os/audio/diag.h>
#include <o3os/audio/graph.h>
#include <o3os/hal/audio.h>

#include "support/rng.h"
#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

using namespace o3os::audio;
using o3os::test::Rng;
namespace ha = o3os::hal::audio;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;

std::uint32_t g_ticks = 0;
std::uint32_t fake_cycles() noexcept { return g_ticks += 100; }  // every reading is 100 cycles on: the tap work of a block reads as exactly 100

// A block whose samples are `base + i` (wrapping), 4-byte aligned.
struct Block {
  alignas(8) std::int16_t s[kB];
  void fill(std::int32_t base) {
    for (std::uint32_t i = 0; i < kB; ++i) {
      s[i] = static_cast<std::int16_t>(base + static_cast<std::int32_t>(i));
    }
  }
};

}  // namespace

TEST_CASE("app tap: the ring and the level stats of every block are exact, in the order written") {
  static o3os::test::StaticSlot<AppTap> slot;
  AppTap& tap = slot.make();
  CHECK(tap.source() == AppTap::Source::Off);
  CHECK(tap.head() == 0u);
  Block b;
  std::uint32_t cursor = 0;
  AppTap::Levels l;
  CHECK(tap.levels_since(cursor, l));
  CHECK((l.frames == 0u && l.lost == 0u && l.peak == 0u && l.sumsq == 0u));
  // three blocks: a ramp, a block at full scale on one sample, a silent one
  b.fill(100);
  tap.push(b.s);
  std::uint64_t want = 0;
  for (std::uint32_t i = 0; i < kB; ++i) {
    want += static_cast<std::uint64_t>(static_cast<std::int64_t>(b.s[i]) * b.s[i]);
  }
  b.fill(-200);
  b.s[7] = -32768;  // |-32768| is 32768
  for (std::uint32_t i = 0; i < kB; ++i) {
    want += static_cast<std::uint64_t>(static_cast<std::int64_t>(b.s[i]) * b.s[i]);
  }
  tap.push(b.s);
  std::memset(b.s, 0, sizeof b.s);
  tap.push(b.s);
  CHECK(tap.head() == 3u * kB);
  CHECK(tap.levels_since(cursor, l));
  CHECK(l.frames == 3u * kB);
  CHECK(l.lost == 0u);
  CHECK(l.peak == 32768u);
  CHECK(l.sumsq == want);
  CHECK(l.clip_blocks == 1u);
  CHECK(cursor == 3u * kB);
  // nothing new: an empty answer, the cursor stays
  CHECK(tap.levels_since(cursor, l));
  CHECK((l.frames == 0u && l.peak == 0u && l.clip_blocks == 0u));
  // the newest window is the samples as written (the last block is silent, the one before it is the full-scale one)
  alignas(4) std::int16_t win[2 * kB];
  const AppTap::Ring::Window w = tap.read_latest(win, 2 * kB);
  CHECK(w.whole);
  CHECK(w.head == 3u * kB);
  CHECK(win[7] == -32768);
  CHECK(win[kB + 5] == 0);
  // 32767 is a clip too, 32766 is not
  b.fill(0);
  b.s[3] = 32766;
  tap.push(b.s);
  b.s[3] = 32767;
  tap.push(b.s);
  CHECK(tap.levels_since(cursor, l));
  CHECK((l.frames == 2u * kB && l.peak == 32767u && l.clip_blocks == 1u));
}

TEST_CASE("app tap: a reader that falls behind loses the oldest blocks and is told; the cursor wraps with the counter") {
  static o3os::test::StaticSlot<AppTap> slot;
  AppTap& tap = slot.make();
  Block b;
  // each block's peak is its own number + 1, so what was kept is visible
  auto write = [&](std::uint32_t n) {
    for (std::uint32_t k = 0; k < n; ++k) {
      std::memset(b.s, 0, sizeof b.s);
      b.s[0] = static_cast<std::int16_t>(tap.head() / kB + 1u);
      tap.push(b.s);
    }
  };
  std::uint32_t cursor = 0;
  AppTap::Levels l;
  write(10);
  CHECK(tap.levels_since(cursor, l));
  CHECK((l.frames == 10u * kB && l.lost == 0u && l.peak == 10u));
  // 100 blocks while the reader is away: the ring promises 63, the rest is counted
  write(100);
  CHECK(tap.levels_since(cursor, l));
  CHECK(l.frames == AppTap::kMaxFrames);
  CHECK(l.lost == 100u * kB - AppTap::kMaxFrames);
  CHECK(l.peak == 110u);  // the newest block is among those kept
  // exactly 63 blocks behind: nothing lost; 64: one block lost
  write(63);
  CHECK(tap.levels_since(cursor, l));
  CHECK((l.frames == 63u * kB && l.lost == 0u));
  write(64);
  CHECK(tap.levels_since(cursor, l));
  CHECK((l.frames == 63u * kB && l.lost == kB));
  // a cursor that is ahead of the head (a reader from another run) is treated as lapped, not as a hang
  cursor = tap.head() + 5u * kB;
  l.lost = 99;
  l.frames = 99;
  CHECK(tap.levels_since(cursor, l));
  CHECK(cursor == tap.head());
  CHECK((l.lost == 0u && l.frames == 0u && l.peak == 0u && l.clip_blocks == 0u));  // nothing lost, nothing measured: not a garbage count
  // exactly the lapped path: kMaxFrames + 64 behind loses exactly one block, 2 * kMaxFrames behind loses exactly kMaxFrames
  write(70);
  cursor = tap.head() - AppTap::kMaxFrames - kB;
  CHECK(tap.levels_since(cursor, l));
  CHECK((l.lost == kB && l.frames == AppTap::kMaxFrames));
  cursor = tap.head() - 2u * AppTap::kMaxFrames;
  CHECK(tap.levels_since(cursor, l));
  CHECK((l.lost == AppTap::kMaxFrames && l.frames == AppTap::kMaxFrames));
}

TEST_CASE("app tap: read_range gives the range the caller decided, whatever the writer publishes meanwhile, and refuses what is not whole") {
  static o3os::test::StaticSlot<AppTap> slot;
  AppTap& tap = slot.make();
  Block b;
  for (int n = 1; n <= 5; ++n) {
    b.fill(1000 * n);
    tap.push(b.s);
  }
  const std::uint32_t head = tap.head();  // the reader decides its range from this load: blocks 3..5
  b.fill(6000);
  tap.push(b.s);  // ... and the render publishes a block before the copy
  alignas(4) std::int16_t out[3 * kB];
  CHECK(tap.read_range(head - 3u * kB, out, 3u * kB));
  CHECK((out[0] == 3000 && out[kB] == 4000 && out[2 * kB] == 5000 && out[3 * kB - 1] == 5063));  // exactly blocks 3..5: the new one did not move the window
  // not yet written: refused
  CHECK(!tap.read_range(tap.head() - kB, out, 2u * kB));
  // lapped: a range older than the ring keeps
  for (int n = 0; n < 70; ++n) {
    b.fill(n);
    tap.push(b.s);
  }
  CHECK(!tap.read_range(head - 3u * kB, out, 3u * kB));
  CHECK(tap.read_range(tap.head() - 3u * kB, out, 3u * kB));
}

TEST_CASE("app tap: sequence counters wrap safely at 2^32 frames") {
  // The counter is a free-running u32 of frames and every cursor is used only as a difference: a cursor "behind zero" by 3 blocks, as a reader would be just before the counter wraps, sees
  // the distance, not a huge or negative number. (The wrap itself is 2^32 frames away, 27 hours of audio.)
  static o3os::test::StaticSlot<AppTap> slot;
  AppTap& tap = slot.make();
  Block b;
  for (int i = 0; i < 5; ++i) {
    b.fill(i);
    tap.push(b.s);
  }
  std::uint32_t cursor = 0u - 3u * kB;  // "3 blocks behind zero": head - cursor = 8 blocks, wrapping
  AppTap::Levels l;
  CHECK(tap.levels_since(cursor, l));
  CHECK(l.frames == 8u * kB);  // the unsigned difference is the distance
  CHECK(cursor == tap.head());
}

TEST_CASE("app tap: the graph feeds it from the master pair or the input bus, only while a source is selected") {
  static o3os::test::StaticSlot<Graph> gslot;
  static o3os::test::StaticSlot<AppTap> tslot;
  static TapMem taps;
  g_ticks = 0;
  Graph& g = gslot.make(&fake_cycles, taps);
  AppTap& tap = tslot.make();
  g.attach_app_tap(&tap);
  Rng rng(77);
  namespace gps = graph_param::strip;
  auto push = [&](o3os::core::ParamId id, float v) { CHECK(g.params().push(ParamChange{id, v, 1u})); };
  push(strip_param(0, gps::Source), 3.0f);  // a fed track so that the master pair has sound
  push(strip_param(0, gps::Pan), 20.0f);
  alignas(8) static std::int16_t feed[kB];
  g.feed(0, feed);
  alignas(8) static std::int16_t in[kBS];
  alignas(8) static std::int16_t out[kBS];
  ha::BlockReport report{};
  auto render = [&] {
    for (std::uint32_t i = 0; i < kB; ++i) {
      feed[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(20001)) - 10000);
    }
    for (std::uint32_t i = 0; i < kBS; ++i) {
      in[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(8001)) - 4000);
    }
    const ha::BlockIo io{in, out, 0u, &report};
    g.render(io);
  };
  // Off: nothing is written and nothing is timed
  for (int blk = 0; blk < 3; ++blk) {
    render();
  }
  CHECK(tap.head() == 0u);
  CHECK(tap.cost().blocks == 0u);
  // MASTER: the mono sum of what the block put out, rounded half up, as the spectrum ring has it
  tap.set_source(AppTap::Source::Master);
  alignas(4) std::int16_t expect[8 * kB];
  for (int blk = 0; blk < 8; ++blk) {
    render();
    for (std::uint32_t i = 0; i < kB; ++i) {
      expect[blk * kB + i] = static_cast<std::int16_t>((static_cast<std::int32_t>(out[2u * i]) + out[2u * i + 1u] + 1) >> 1);
    }
  }
  CHECK(tap.head() == 8u * kB);
  alignas(4) std::int16_t win[8 * kB];
  AppTap::Ring::Window w = tap.read_latest(win, 8 * kB);
  CHECK(w.whole);
  CHECK(std::memcmp(win, expect, sizeof win) == 0);
  // the stats of those blocks are those of the window
  std::uint32_t cursor = 0;
  AppTap::Levels l;
  CHECK(tap.levels_since(cursor, l));
  std::uint64_t sq = 0;
  std::uint32_t peak = 0;
  for (std::uint32_t i = 0; i < 8u * kB; ++i) {
    sq += static_cast<std::uint64_t>(static_cast<std::int64_t>(expect[i]) * expect[i]);
    const std::uint32_t a = static_cast<std::uint32_t>(expect[i] < 0 ? -static_cast<std::int32_t>(expect[i]) : expect[i]);
    peak = a > peak ? a : peak;
  }
  CHECK((l.frames == 8u * kB && l.sumsq == sq && l.peak == peak));
  CHECK(peak > 100u);  // the test signal is not silence
  // INPUT: the mono sum of the RX block (the input strip is closed by default: the planes are the raw RX)
  tap.set_source(AppTap::Source::Input);
  render();
  CHECK(tap.head() == 9u * kB);
  w = tap.read_latest(win, kB);
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(win[i] == static_cast<std::int16_t>((static_cast<std::int32_t>(in[2u * i]) + in[2u * i + 1u] + 1) >> 1));
  }
  // Off again: it stops at the next block
  tap.set_source(AppTap::Source::Off);
  render();
  render();
  CHECK(tap.head() == 9u * kB);
  // the cost: the fake counter is 100 cycles on at each reading, so a block's tap work (two readings) reads 100; published every 16 blocks
  tap.set_source(AppTap::Source::Master);
  for (int blk = 0; blk < 32; ++blk) {
    render();
  }
  AppTap::Cost c = tap.cost();
  CHECK(c.blocks >= 32u);
  CHECK(c.last > 0u);
  CHECK(c.max >= c.last);
  CHECK(c.sum / c.blocks == c.max);  // a constant cost: mean == max
  // reset: the render zeroes the figures at its next block, and starts counting again
  tap.reset_cost();
  render();
  c = tap.cost();
  CHECK(c.blocks == 0u);  // one block since the reset: not yet published (every 16)
  for (int blk = 0; blk < 16; ++blk) {
    render();
  }
  CHECK(tap.cost().blocks == 16u);
}

TEST_CASE("app tap: with the samples off a block is measured and counted but no sample is stored; on again, the next block is stored") {
  // OS-BUG-TAPWRITE: an app that reads only the statistics does not make the render pay for the 32 SDRAM stores.
  static o3os::test::StaticSlot<AppTap> slot;
  AppTap& tap = slot.make();
  CHECK(tap.samples());  // the default: whoever uses the tap without asking gets the samples
  Block b;
  // 64 blocks with the samples: block n holds n + i, so every slot of the ring holds a known block
  for (int n = 1; n <= 64; ++n) {
    b.fill(1000 * n);
    tap.push(b.s);
  }
  CHECK(tap.head() == 64u * kB);
  tap.set_samples(false);
  CHECK(!tap.samples());
  // three blocks: a ramp, a block with one full-scale sample, a silent one. They land on the ring slots 0..2, which still hold blocks 1..3.
  std::uint32_t cursor = tap.head();
  b.fill(100);
  std::uint64_t want = 0;
  for (std::uint32_t i = 0; i < kB; ++i) {
    want += static_cast<std::uint64_t>(static_cast<std::int64_t>(b.s[i]) * b.s[i]);
  }
  tap.push(b.s);
  b.fill(-200);
  b.s[7] = -32768;
  for (std::uint32_t i = 0; i < kB; ++i) {
    want += static_cast<std::uint64_t>(static_cast<std::int64_t>(b.s[i]) * b.s[i]);
  }
  tap.push(b.s);
  std::memset(b.s, 0, sizeof b.s);
  tap.push(b.s);
  CHECK(tap.head() == 67u * kB);  // the counter moved on, as for a stored block
  AppTap::Levels l;
  CHECK(tap.levels_since(cursor, l));  // and the statistics are exact
  CHECK((l.frames == 3u * kB && l.lost == 0u && l.peak == 32768u && l.sumsq == want && l.clip_blocks == 1u));
  CHECK(cursor == 67u * kB);
  // nothing was stored: the ring slots of those blocks hold what they held (blocks 1..3 of the first lap: 1000 n + i), the newest block's window is NOT the silent block
  alignas(4) std::int16_t win[kB];
  const AppTap::Ring::Window w = tap.read_latest(win, kB);
  CHECK(w.whole);
  CHECK(w.head == 67u * kB);
  CHECK((win[0] == 3000 && win[kB - 1] == 3063));
  // on again: the next block is stored, whole, and the statistics go on from the same counter
  tap.set_samples(true);
  b.fill(7000);
  tap.push(b.s);
  CHECK(tap.read_latest(win, kB).whole);
  CHECK((win[0] == 7000 && win[kB - 1] == 7063));
  CHECK(tap.levels_since(cursor, l));
  CHECK((l.frames == kB && l.peak == 7063u));
  // reset() puts the default back
  tap.set_samples(false);
  tap.reset();
  CHECK(tap.samples());
}

TEST_CASE("app tap: the graph honours the samples flag: the statistics are measured, the ring is left alone") {
  static o3os::test::StaticSlot<Graph> gslot;
  static o3os::test::StaticSlot<AppTap> tslot;
  static TapMem taps;
  g_ticks = 0;
  Graph& g = gslot.make(&fake_cycles, taps);
  AppTap& tap = tslot.make();
  g.attach_app_tap(&tap);
  Rng rng(78);
  namespace gps = graph_param::strip;
  CHECK(g.params().push(ParamChange{strip_param(0, gps::Source), 3.0f, 1u}));
  CHECK(g.params().push(ParamChange{strip_param(0, gps::Pan), 20.0f, 1u}));
  alignas(8) static std::int16_t feed[kB];
  g.feed(0, feed);
  alignas(8) static std::int16_t in[kBS];
  alignas(8) static std::int16_t out[kBS];
  ha::BlockReport report{};
  auto render = [&] {
    for (std::uint32_t i = 0; i < kB; ++i) {
      feed[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(20001)) - 10000);
    }
    const ha::BlockIo io{in, out, 0u, &report};
    g.render(io);
  };
  tap.set_samples(false);
  tap.set_source(AppTap::Source::Master);
  for (int blk = 0; blk < 16; ++blk) {
    render();
  }
  CHECK(tap.head() == 16u * kB);
  AppTap::Levels l;
  std::uint32_t cursor = 0;
  CHECK(tap.levels_since(cursor, l));
  CHECK((l.frames == 16u * kB && l.peak > 100u && l.sumsq != 0u));  // measured: the test signal is not silence
  alignas(4) std::int16_t win[16 * kB];
  CHECK(tap.read_latest(win, 16 * kB).whole);
  bool all_zero = true;
  for (const std::int16_t v : win) {
    all_zero = all_zero && v == 0;
  }
  CHECK(all_zero);                   // the ring was never written
  CHECK(tap.cost().blocks == 16u);   // the cost is still timed and published
  // on: the next block is in the ring, as the master pair put it out
  tap.set_samples(true);
  render();
  CHECK(tap.head() == 17u * kB);
  CHECK(tap.read_latest(win, kB).whole);
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(win[i] == static_cast<std::int16_t>((static_cast<std::int32_t>(out[2u * i]) + out[2u * i + 1u] + 1) >> 1));
  }
}

TEST_CASE("app tap: the cost line of diag cpu") {
  char buf[160];
  std::size_t n = diag::format_app_tap(2, 0, 0, 0, 0, 180000000u, buf, sizeof buf);
  CHECK(std::strcmp(buf, "tap: master, no block timed") == 0);
  CHECK(n == std::strlen(buf));
  n = diag::format_app_tap(1, 160, 410, 160u * 400u, 930, 180000000u, buf, sizeof buf);
  CHECK(std::strcmp(buf, "tap: input, 160 blocks, write cycles last 410 mean 400 max 930 (max 5.1 us)") == 0);
  CHECK(n == std::strlen(buf));
  n = diag::format_app_tap(0, 16, 1, 16, 1, 180000000u, buf, sizeof buf);
  CHECK(std::strcmp(buf, "tap: off, 16 blocks, write cycles last 1 mean 1 max 1 (max 0.0 us)") == 0);
  char small[20];
  diag::format_app_tap(2, 16, 1, 16, 1, 180000000u, small, sizeof small);
  CHECK(std::strlen(small) < sizeof small);  // truncated, terminated
}

TEST_CASE("app tap: linear levels") {
  AppTap::Levels l;
  CHECK(AppTap::linear_rms(l) == 0.0f);
  l.frames = 128;
  l.peak = 16384;
  l.sumsq = 128ull * 8192ull * 8192ull;  // a constant 8192
  CHECK(AppTap::linear_peak(l) == doctest::Approx(0.5f));
  CHECK(AppTap::linear_rms(l) == doctest::Approx(0.25f).epsilon(1e-6));
}

TEST_CASE("app tap: linear_rms reads the 64-bit sum as two 32-bit halves, within 1 ulp of the direct conversion (OS-FLASHDIET T3)") {
  // The old expression: one uint64 -> float conversion (correctly rounded), then the same sqrt. The new one converts each 32-bit half (a single FPU conversion each, no
  // __aeabi_ul2f); the result may differ by one ulp at most.
  const auto old_rms = [](std::uint64_t sumsq, std::uint32_t frames) {
    return std::sqrt(static_cast<float>(sumsq) / static_cast<float>(frames)) * (1.0f / 32768.0f);
  };
  const auto bits = [](float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
  };
  const std::uint32_t frames_list[] = {1u, 2u, 63u, 64u, 65u, 128u, 1000u, AppTap::kMaxFrames, AppTap::kMaxFrames + 1u, 100000u};
  const std::uint64_t max_sumsq = static_cast<std::uint64_t>(AppTap::kMaxFrames) * 32768ull * 32768ull;  // the largest sum a reader can fold
  const std::uint64_t sumsq_list[] = {
      0ull, 1ull, 2ull, 63ull, 64ull, 65ull, 0xffffffull, 0x1000000ull, 0x1000001ull, 0x7ffffffeull, 0x7fffffffell, 0x80000000ull, 0x80000001ull,
      0xfffffffeull, 0xffffffffull, 0x100000000ull, 0x100000001ull, 0x1000000000000ull, max_sumsq / 2u, max_sumsq - 1u, max_sumsq, ~0ull,
  };
  Rng rng(0x7a9u);
  std::uint64_t bad = 0;
  std::uint64_t cases = 0;
  const auto check = [&](std::uint64_t sumsq, std::uint32_t frames) {
    AppTap::Levels l;
    l.frames = frames;
    l.sumsq = sumsq;
    const float got = AppTap::linear_rms(l);
    const float want = old_rms(sumsq, frames);
    ++cases;
    const std::uint32_t g = bits(got);
    const std::uint32_t w = bits(want);
    if (g != w && g != w + 1u && g + 1u != w) {  // positive floats: the bit patterns are the rank, one ulp apart at most
      if (bad == 0u) {
        MESSAGE("mismatch: sumsq=" << sumsq << " frames=" << frames << " got=" << got << " want=" << want);
      }
      ++bad;
    }
  };
  for (const std::uint64_t sumsq : sumsq_list) {
    for (const std::uint32_t frames : frames_list) {
      check(sumsq, frames);
    }
  }
  for (unsigned r = 0; r < 8192u; ++r) {  // random sums over the whole 64-bit range (the reachable ones are below max_sumsq, the rest only widen the proof)
    const std::uint64_t sumsq = (static_cast<std::uint64_t>(rng.next()) << 32) | rng.next();
    check(sumsq, frames_list[r % (sizeof frames_list / sizeof frames_list[0])]);
    check(sumsq % (max_sumsq + 1u), AppTap::kMaxFrames);
  }
  MESSAGE("cases=" << cases << " beyond-1-ulp=" << bad);
  CHECK(bad == 0u);
}
