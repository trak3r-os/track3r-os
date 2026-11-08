// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEWTENV W2 (docs/specs/OS-CEWTENV.md sections 3.3, F3, F4): the wavetable contour bank and evaluator
// (audio/wt_contour.h), a port of tracker-ce@7fd6c2c's core/source/mtWavetableEnvelopes.cpp (MIT; THIRD_PARTY.md).
//
// The golden tests/data/wt_contour_golden.h was made ONCE on the host from tracker-ce's OWN code, as follows (the generator is a
// throwaway and is not committed, nor is its tracker-ce copy): copy build-ce/tracker-ce/core/source/mtWavetableEnvelopes.cpp into a
// scratch directory (e.g. build-wtgen/), strip from it the .wav loader (loadUserWavetablesFromSD), the Q30 twin
// (evalWavetableEnvelopeFixed30) and the platform macros (__NOINIT, AUDIO_CORE_FLASH, the TrackerImpl/wavHeaderReader includes),
// add a main() that calls initWavetableEnvelopes() and then prints, for each of the 256 contours, the CRC-32 (zlib, as
// formats::crc32) of its 256 bytes, and 64 spot values of evalWavetableEnvelopeFloat (8 shapes x 8 phase/warp tuples, warps
// -7, -3, 0, +4, +7 on X and on Y), and compile it with the host compiler as a one-off:
//   g++ -O2 -std=c++17 build-wtgen/wt_golden_gen.cpp build-wtgen/mtWavetableEnvelopes_gen.cpp -o build-wtgen/wt_golden_gen -lm
//   ./build-wtgen/wt_golden_gen > tests/data/wt_contour_golden.h
// The port here is bit-exact against that code on the host (checked over every shape, 1024 phases and all 256 bend bytes), so the
// CRCs and the spot values are compared exactly. Host only: the golden pins the host libm's float results (QEMU's newlib may differ
// in the last ulp), which is why this file is not in the qemu suites of tests/CMakeLists.txt.
//
// Against the golden: the CRC-32 of each of the 256 contours and the 64 spot evaluations; the nibble pack/unpack (round trip over
// all warps, the clamps, the 8-means-0 nibble); phase 0 and 1 exact (the contour's first and last point, also under a warp X, which
// leaves the ends alone); no NaN (and no value outside 0..1) for any phase in [0, 1] and any bend byte; the made-mask. Host only.
#include <doctest/doctest.h>

#include <o3os/audio/wt_contour.h>
#include <o3os/formats/crc32.h>

#include "data/wt_contour_golden.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

namespace audio = o3os::audio;
namespace golden = o3os::test::wt_golden;

// The bank is the caller's storage: on the host a static buffer (on the MK66 it is the SDRAM arena's wt_contour region).
audio::WtBank g_bank;
audio::WtMadeMask g_made{};

void make_all() {
  for (unsigned s = 0; s < audio::kWtContourCount; ++s) {
    audio::wt_make(g_bank, s, g_made);
  }
}

bool same_bits(float a, float b) noexcept { return std::memcmp(&a, &b, sizeof(float)) == 0; }

}  // namespace

TEST_CASE("wt_contour: the bank's layout and the made-mask") {
  CHECK(audio::kWtContourCount == 256u);
  CHECK(audio::kWtContourFactoryCount == 128u);
  CHECK(audio::kWtContourLen == 128u);
  CHECK(audio::kWtBankBytes == 65536u);
  CHECK(sizeof(audio::WtBank) == audio::kWtBankBytes);
  CHECK(sizeof(audio::WtMadeMask) == 32u);

  audio::WtMadeMask fresh{};
  for (unsigned s = 0; s < audio::kWtContourCount; ++s) {
    CHECK_FALSE(audio::wt_made(fresh, s));
  }
  CHECK_FALSE(audio::wt_made(fresh, 256u));  // out of range reads as not made

  audio::wt_make(g_bank, 0u, fresh);
  CHECK(audio::wt_made(fresh, 0u));
  CHECK_FALSE(audio::wt_made(fresh, 1u));
  audio::wt_make(g_bank, 255u, fresh);
  CHECK(audio::wt_made(fresh, 255u));
  CHECK(audio::wt_contour(g_bank, 7u) == &g_bank.contour[7u][0u]);

  audio::wt_make(g_bank, 300u, fresh);  // held to 255
  CHECK(audio::wt_made(fresh, 255u));
}

TEST_CASE("wt_contour: the CRC-32 of each of the 256 contours (golden)") {
  make_all();
  for (unsigned s = 0; s < audio::kWtContourCount; ++s) {
    INFO("shape " << s);
    const std::uint32_t crc = o3os::formats::crc32(reinterpret_cast<const std::uint8_t*>(audio::wt_contour(g_bank, s)),
                                                   audio::kWtContourLen * 2u);
    CHECK(crc == golden::kContourCrc[s]);
  }
}

TEST_CASE("wt_contour: 64 spot evaluations against the golden (warps -7, -3, 0, +4, +7 on X and Y)") {
  make_all();
  static_assert(sizeof(golden::kSpot) / sizeof(golden::kSpot[0]) == 64u, "64 spots");
  for (const golden::Spot& sp : golden::kSpot) {
    INFO("shape " << unsigned{sp.shape} << " bend " << unsigned{sp.bend} << " phase " << sp.phase);
    const float v = audio::wt_eval(audio::wt_contour(g_bank, sp.shape), sp.phase, sp.bend);
    CHECK(same_bits(v, sp.value));
  }
  // the warp values the 64 spots cover: -7, -3, 0, +4, +7 on X and on Y
  bool seen_x[15] = {};
  bool seen_y[15] = {};  // warps -7..+7
  for (const golden::Spot& sp : golden::kSpot) {
    seen_x[audio::wt_warp_x(sp.bend) + 7] = true;
    seen_y[audio::wt_warp_y(sp.bend) + 7] = true;
  }
  for (int w : {-7, -3, 0, 4, 7}) {
    INFO("warp " << w);
    CHECK(seen_x[w + 7]);
    CHECK(seen_y[w + 7]);
  }
}

TEST_CASE("wt_contour: the nibble pack/unpack (F4)") {
  // decode: 0..7 -> +0..+7, 9..15 -> -7..-1, 8 -> 0
  CHECK(audio::wt_warp_x(0x00u) == 0);
  CHECK(audio::wt_warp_y(0x00u) == 0);
  CHECK(audio::wt_warp_x(0x77u) == 7);
  CHECK(audio::wt_warp_y(0x77u) == 7);
  CHECK(audio::wt_warp_x(0x99u) == -7);
  CHECK(audio::wt_warp_y(0x99u) == -7);
  CHECK(audio::wt_warp_x(0x88u) == 0);  // 8 is 0
  CHECK(audio::wt_warp_y(0x88u) == 0);
  CHECK(audio::wt_warp_x(0x80u) == 0);
  CHECK(audio::wt_warp_y(0x08u) == 0);
  CHECK(audio::wt_warp_y(0x7Fu) == -1);  // F is -1
  CHECK(audio::wt_warp_x(0x7Fu) == 7);
  for (unsigned b = 0; b < 256u; ++b) {
    const std::int8_t wx = audio::wt_warp_x(static_cast<std::uint8_t>(b));
    const std::int8_t wy = audio::wt_warp_y(static_cast<std::uint8_t>(b));
    INFO("bend " << b);
    CHECK(wx >= -7);
    CHECK(wx <= 7);
    CHECK(wy >= -7);
    CHECK(wy <= 7);
    const unsigned n = static_cast<unsigned>(b);
    CHECK(wx == static_cast<std::int8_t>((n >> 4) <= 7u ? (n >> 4) : ((n >> 4) >= 9u ? (n >> 4) - 16u : 0u)));
    CHECK(wy == static_cast<std::int8_t>((n & 0xFu) <= 7u ? (n & 0xFu) : ((n & 0xFu) >= 9u ? (n & 0xFu) - 16u : 0u)));
  }
  // pack: the inverse over the whole range, and the clamp outside it
  CHECK(audio::wt_pack_warp(0, 0) == 0x00u);
  CHECK(audio::wt_pack_warp(7, 7) == 0x77u);
  CHECK(audio::wt_pack_warp(-7, -7) == 0x99u);
  CHECK(audio::wt_pack_warp(-1, 1) == 0xF1u);
  CHECK(audio::wt_pack_warp(100, -100) == 0x79u);  // held to +7 / -7
  for (int x = -7; x <= 7; ++x) {
    for (int y = -7; y <= 7; ++y) {
      const std::uint8_t b = audio::wt_pack_warp(static_cast<std::int8_t>(x), static_cast<std::int8_t>(y));
      INFO("warps " << x << ", " << y);
      CHECK(audio::wt_warp_x(b) == x);
      CHECK(audio::wt_warp_y(b) == y);
    }
  }
}

TEST_CASE("wt_contour: phase 0 and 1 are exact") {
  make_all();
  for (unsigned s = 0; s < audio::kWtContourCount; ++s) {
    const std::int16_t* c = audio::wt_contour(g_bank, s);
    INFO("shape " << s);
    // bend 0: the ends are the contour's first and last point, exactly
    CHECK(same_bits(audio::wt_eval(c, 0.0f, 0x00u), static_cast<float>(c[0]) * (1.0f / 32767.0f)));
    CHECK(same_bits(audio::wt_eval(c, 1.0f, 0x00u), static_cast<float>(c[audio::kWtContourLen - 1u]) * (1.0f / 32767.0f)));
    // a warp X leaves the ends alone (p = 0 and p = 1 are fixed points of the phase law), a warp Y of 0 does not touch the level
    for (std::uint8_t bend = 0x10u; bend <= 0x70u; bend += 0x10u) {
      CHECK(same_bits(audio::wt_eval(c, 0.0f, bend), static_cast<float>(c[0]) * (1.0f / 32767.0f)));
      CHECK(same_bits(audio::wt_eval(c, 1.0f, bend), static_cast<float>(c[audio::kWtContourLen - 1u]) * (1.0f / 32767.0f)));
    }
  }
  // a phase outside [0, 1] is held
  const std::int16_t* c = audio::wt_contour(g_bank, 42u);
  CHECK(same_bits(audio::wt_eval(c, -0.5f, 0x00u), audio::wt_eval(c, 0.0f, 0x00u)));
  CHECK(same_bits(audio::wt_eval(c, 1.5f, 0x00u), audio::wt_eval(c, 1.0f, 0x00u)));
}

TEST_CASE("wt_contour: no NaN, no value outside 0..1, for any phase in [0,1] and any bend byte") {
  make_all();
  for (unsigned s = 0; s < audio::kWtContourCount; s += 16u) {  // every family, factory and fallback
    const std::int16_t* c = audio::wt_contour(g_bank, s);
    for (unsigned pi = 0; pi <= 64u; ++pi) {
      const float phase = static_cast<float>(pi) / 64.0f;
      for (unsigned b = 0; b < 256u; ++b) {
        const float v = audio::wt_eval(c, phase, static_cast<std::uint8_t>(b));
        INFO("shape " << s << " phase " << phase << " bend " << b);
        CHECK(!std::isnan(v));
        CHECK(v >= 0.0f);
        CHECK(v <= 1.0f);
      }
    }
  }
  // every contour at a few phases and bends
  for (unsigned s = 0; s < audio::kWtContourCount; ++s) {
    const std::int16_t* c = audio::wt_contour(g_bank, s);
    for (float phase : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
      for (std::uint8_t bend : {std::uint8_t{0x00u}, std::uint8_t{0x99u}, std::uint8_t{0x77u}, std::uint8_t{0x4Du}, std::uint8_t{0xD0u}}) {
        const float v = audio::wt_eval(c, phase, bend);
        INFO("shape " << s << " phase " << phase << " bend " << unsigned{bend});
        CHECK(!std::isnan(v));
        CHECK(v >= 0.0f);
        CHECK(v <= 1.0f);
      }
    }
  }
}

// ---- OS-CEWTENV W3c: the baked warps (docs/specs/OS-CEWTENV.md Q3 a) ----------------------------------------------------------------------------------------

namespace {
audio::WtBank g_bound;       // the bound bank of the bake tests
audio::WtBakeBank g_pool;    // 33 KB: a static, never on a stack
}  // namespace

static_assert(audio::kWtBakeBytes == 33028u, "the platform's mirror of the pool's size (mem_map.h kWtBakeBytes) says 33,028");

TEST_CASE("wt bake: no warp or an all-zero warp byte bake nothing (id 0); the slot then applies its warps live, bit for bit") {
  audio::wt_bind(g_bound);
  audio::wt_bake_bind(g_pool);
  CHECK(audio::wt_bake(37u, 0x00u) == 0u);
  CHECK(audio::wt_bake(37u, 0x88u) == 0u);  // nibble 8 is a warp of 0 on both axes
  CHECK(audio::wt_bake_used() == 0u);
  for (const std::uint8_t bend : {std::uint8_t{0x00}, std::uint8_t{0x34}, std::uint8_t{0x9C}}) {
    for (const float p : {0.0f, 0.13f, 0.5f, 0.97f, 1.0f}) {
      CHECK(audio::wt_eval_slot(37u, 0u, bend, p) == audio::wt_eval_shape(37u, p, bend));  // id 0: the live evaluation
      CHECK(audio::wt_eval_slot(37u, 200u, bend, p) == audio::wt_eval_shape(37u, p, bend));  // an id past the pool: live too, never a wild read
    }
  }
}

TEST_CASE("wt bake: the same shape and warp share an id, ids count up from 1, the contour is made") {
  audio::wt_bind(g_bound);
  audio::wt_bake_bind(g_pool);
  const unsigned a = audio::wt_bake(37u, 0x34u);
  const unsigned b = audio::wt_bake(37u, 0x9Cu);
  const unsigned c = audio::wt_bake(38u, 0x34u);
  CHECK(a == 1u);
  CHECK(b == 2u);
  CHECK(c == 3u);
  CHECK(audio::wt_bake(37u, 0x34u) == a);  // deduplicated
  CHECK(audio::wt_bake(38u, 0x34u) == c);
  CHECK(audio::wt_bake_used() == 3u);
  CHECK(audio::wt_eval_shape(38u, 0.5f, 0u) == audio::wt_eval(audio::wt_contour(g_bound, 38u), 0.5f, 0u));  // the bank's contour was made
}

namespace {
// The difference between the baked slot and the live warps over `shapes` x `wxs` x `wys` (the no-warp pair is skipped), 256 phases a contour: the sum, the count, the number of points more
// than 0.1 off, a histogram of the difference in thousandths (for the 99th percentile), and the worst at phase 0 and 1.
struct BakeError {
  double sum = 0.0;
  unsigned long n = 0;
  unsigned long big = 0;
  unsigned long hist[1001] = {};
  double worst_end = 0.0;
  unsigned baked = 0;
  double mean() const { return sum / static_cast<double>(n); }
  double p99() const {
    unsigned long seen = 0;
    for (unsigned i = 0; i <= 1000u; ++i) {
      seen += hist[i];
      if (seen * 100u >= n * 99u) {
        return static_cast<double>(i) / 1000.0;
      }
    }
    return 1.0;
  }
};

void measure_bake(BakeError& e, const unsigned* shapes, unsigned shape_count, std::initializer_list<int> wxs, std::initializer_list<int> wys) {
  for (unsigned k = 0; k < shape_count; ++k) {
    const unsigned s = shapes[k];
    audio::wt_make_bound(s);
    audio::wt_bake_bind(g_pool);  // a fresh pool a shape: its 24 warps fit
    for (const int wx : wxs) {
      for (const int wy : wys) {
        const std::uint8_t bend = audio::wt_pack_warp(static_cast<std::int8_t>(wx), static_cast<std::int8_t>(wy));
        const unsigned id = audio::wt_bake(s, bend);
        if (id == 0u) {  // no warp: the live evaluation is the slot's
          continue;
        }
        ++e.baked;
        for (unsigned i = 0; i <= 255u; ++i) {
          const float p = static_cast<float>(i) / 255.0f;
          const double d = std::fabs(static_cast<double>(audio::wt_eval_slot(s, id, bend, p)) - static_cast<double>(audio::wt_eval_shape(s, p, bend)));
          e.sum += d;
          ++e.n;
          e.big += d > 0.1 ? 1u : 0u;
          unsigned bin = static_cast<unsigned>(d * 1000.0 + 0.5);
          bin = bin > 1000u ? 1000u : bin;
          ++e.hist[bin];
        }
        e.worst_end = std::fmax(e.worst_end, std::fabs(static_cast<double>(audio::wt_eval_slot(s, id, bend, 0.0f)) - static_cast<double>(audio::wt_eval_shape(s, 0.0f, bend))));
        e.worst_end = std::fmax(e.worst_end, std::fabs(static_cast<double>(audio::wt_eval_slot(s, id, bend, 1.0f)) - static_cast<double>(audio::wt_eval_shape(s, 1.0f, bend))));
      }
    }
  }
}
}  // namespace

TEST_CASE("wt bake: the baked slot matches the live warps: mean 0.54 % of full scale over the 256 contours and 5 x 5 warps, 99th percentile 0.14, exact at phase 0 and 1") {
  audio::wt_bind(g_bound);
  unsigned all[audio::kWtContourCount];
  for (unsigned s = 0; s < audio::kWtContourCount; ++s) {
    all[s] = s;
  }
  BakeError e;
  measure_bake(e, all, audio::kWtContourCount, {-7, -3, 0, 4, 7}, {-7, -2, 0, 3, 7});
  CHECK(e.baked == audio::kWtContourCount * 24u);  // 25 warps a shape, one of them none
  CHECK_MESSAGE(e.mean() < 0.0060, "mean difference " << e.mean());  // measured 0.00542
  CHECK_MESSAGE(e.p99() <= 0.15, "99th percentile " << e.p99());     // measured 0.139
  CHECK_MESSAGE(e.worst_end < 2e-4, "phase 0 / 1 difference " << e.worst_end);  // (the int16 point's rounding)
}

TEST_CASE("wt bake: warp X +7 on the stepped contours (groups 3, 6, 7, 9, 14) moves the jumps by up to one baked interval: the measured cost") {
  audio::wt_bind(g_bound);
  unsigned stepped[5 * 16];
  unsigned n = 0;
  for (const unsigned group : {3u, 6u, 7u, 9u, 14u}) {
    for (unsigned sub = 0; sub < 16u; ++sub) {
      stepped[n++] = group * 16u + sub;
    }
  }
  BakeError e;
  measure_bake(e, stepped, n, {7}, {0, 3, 7});
  CHECK(e.baked == n * 3u);
  CHECK_MESSAGE(e.mean() < 0.014, "mean difference " << e.mean());  // measured 0.01292
  CHECK_MESSAGE(static_cast<double>(e.big) / static_cast<double>(e.n) < 0.037, "points more than 0.1 off: " << static_cast<double>(e.big) / static_cast<double>(e.n));  // measured 0.0338
  CHECK_MESSAGE(e.worst_end < 2e-4, "phase 0 / 1 difference " << e.worst_end);  // the jumps move, the ends do not
}

TEST_CASE("wt bake: a full pool refuses the next (id 0, counted) and every id stays valid; a rebind empties it") {
  audio::wt_bind(g_bound);
  audio::wt_bake_bind(g_pool);
  unsigned last = 0;
  for (unsigned i = 0; i < audio::kWtBakeCount; ++i) {
    last = audio::wt_bake(i, 0x34u);
    CHECK(last == i + 1u);
  }
  CHECK(audio::wt_bake_used() == audio::kWtBakeCount);
  CHECK(audio::wt_bake_refused() == 0u);
  CHECK(audio::wt_bake(200u, 0x34u) == 0u);  // one more distinct (shape, warp)
  CHECK(audio::wt_bake_refused() == 1u);
  CHECK(audio::wt_bake(5u, 0x34u) == 6u);    // a known one still answers
  const float p = 0.37f;
  CHECK(audio::wt_eval_slot(200u, 0u, 0x34u, p) == audio::wt_eval_shape(200u, p, 0x34u));
  audio::wt_bake_bind(g_pool);
  CHECK(audio::wt_bake_used() == 0u);
  CHECK(audio::wt_bake_refused() == 0u);
}

TEST_CASE("wt bake: the key is the canonical (shape, warp X, warp Y): nibble 8 and nibble 0 are one warp, one entry") {
  audio::wt_bind(g_bound);
  audio::wt_bake_bind(g_pool);
  const unsigned a = audio::wt_bake(37u, 0x03u);  // warp X 0, warp Y +3
  CHECK(a == 1u);
  CHECK(audio::wt_bake(37u, 0x83u) == a);         // warp X nibble 8: also 0
  CHECK(audio::wt_bake(37u, 0x03u) == a);
  const unsigned b = audio::wt_bake(37u, 0x30u);  // warp X +3, warp Y 0
  CHECK(b == 2u);
  CHECK(audio::wt_bake(37u, 0x38u) == b);         // warp Y nibble 8: also 0
  CHECK(audio::wt_bake(37u, 0x88u) == 0u);        // both zero: nothing to bake
  CHECK(audio::wt_bake_used() == 2u);
  // the entry is the same contour whichever byte asked for it
  for (const float p : {0.0f, 0.21f, 0.5f, 0.8f, 1.0f}) {
    CHECK(audio::wt_eval_slot(37u, a, 0x03u, p) == audio::wt_eval_slot(37u, a, 0x83u, p));
  }
}

TEST_CASE("wt bake: wt_eval_slot trusts an id only up to the entries made; a reset empties the pool and the ids it gave go live") {
  audio::wt_bind(g_bound);
  audio::wt_bake_bind(g_pool);
  const unsigned a = audio::wt_bake(37u, 0x34u);
  const unsigned b = audio::wt_bake(38u, 0x34u);
  CHECK(a == 1u);
  CHECK(b == 2u);
  const float p = 0.43f;
  CHECK(audio::wt_eval_slot(40u, 3u, 0x34u, p) == audio::wt_eval_shape(40u, p, 0x34u));    // id 3: not made yet: live, not a stale read of an unwritten entry
  CHECK(audio::wt_eval_slot(40u, 128u, 0x34u, p) == audio::wt_eval_shape(40u, p, 0x34u));  // the last slot of the pool, unmade
  CHECK(audio::wt_eval_slot(37u, a, 0x34u, p) != audio::wt_eval_shape(37u, p, 0x34u));     // a made id: the baked contour (the resampled composite differs a little)
  audio::wt_bake_reset();
  CHECK(audio::wt_bake_used() == 0u);
  CHECK(audio::wt_eval_slot(37u, a, 0x34u, p) == audio::wt_eval_shape(37u, p, 0x34u));     // after the reset the old ids are live again: nothing reads a recycled entry
  CHECK(audio::wt_bake(41u, 0x34u) == 1u);                                                  // and the pool hands out ids from 1 again
}

TEST_CASE("wt bake: exhausted by a long sweep, then a project load (the reset) gives the whole pool back and the refused count restarts") {
  audio::wt_bind(g_bound);
  audio::wt_bake_bind(g_pool);
  unsigned refused_calls = 0;
  for (unsigned i = 0; i < audio::kWtBakeCount + 20u; ++i) {  // a knob sweep of warp and shape: every step a new pair
    const unsigned id = audio::wt_bake(i % 200u, audio::wt_pack_warp(static_cast<std::int8_t>(1 + (i / 200u)), static_cast<std::int8_t>(1 + static_cast<int>(i % 7u))));
    refused_calls += id == 0u ? 1u : 0u;
  }
  CHECK(audio::wt_bake_used() == audio::kWtBakeCount);
  CHECK(refused_calls == 20u);
  CHECK(audio::wt_bake_refused() == 20u);  // calls, not contours
  audio::wt_bake_reset();                  // the project load (app_main.cpp clear_sample_table)
  CHECK(audio::wt_bake_used() == 0u);
  CHECK(audio::wt_bake_refused() == 0u);
  for (unsigned i = 0; i < audio::kWtBakeCount; ++i) {
    CHECK(audio::wt_bake(i, 0x34u) == i + 1u);  // all 128 fit again
  }
  CHECK(audio::wt_bake_refused() == 0u);
}
