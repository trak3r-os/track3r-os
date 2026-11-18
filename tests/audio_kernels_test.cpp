// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-005b: the graph's sample kernels (audio/kernels.h) against their scalar references. Each production kernel is the paired loop of
// kernels_impl.h on `NativeOps`; here every one is also run on `EmuOps` (a portable model of the Cortex-M4 instructions), so that on the host
// the paired loops themselves (lane order, the odd tail, the unaligned fallback) are tested and not just a scalar copy, and on the target
// (`qemu_audio_kernels`) the real SMLAWB / SMLALD / SSUB16+SEL / SHADD16 / PKHBT / SSAT are compared with the model and with the references,
// bit for bit. Integer kernels must be bit-identical; the float conversion may differ by one LSB on exact ties only (stated at the case).
// Random lengths 0..kBlock, random alignments (a pointer on a 2-byte offset forces the scalar fallback), extreme samples and gains, canaries
// round every output.
#include <doctest/doctest.h>

#include <o3os/audio/kernels.h>
#include <o3os/audio/kernels_impl.h>
#include <o3os/audio/simd.h>
#include <o3os/hal/audio.h>

#include "support/rng.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

namespace k = o3os::audio::kernel;
namespace sm = o3os::audio::simd;
using o3os::test::Rng;

constexpr std::uint32_t kN = o3os::hal::audio::kBlock;
constexpr std::uint32_t kGuard = 8;
constexpr std::int16_t kCanary = 0x5A5A;
constexpr std::int32_t kCanary32 = 0x5A5A5A5A;

std::int16_t random_sample(Rng& rng, bool extreme) {
  if (extreme && rng.below(4) == 0) {
    static const std::int16_t special[] = {32767, -32768, 32767, -32768, 16384, -16384, 0, 1, -1, 32766, -32767};
    return special[rng.below(11)];
  }
  return static_cast<std::int16_t>(rng.next() & 0xFFFFu);
}

// A gain in [0, kMaxGain]: the ends, the neighbours of unity, and anywhere between (the Og12 level law reaches +24 dB).
std::int32_t random_gain(Rng& rng) {
  switch (rng.below(8)) {
    case 0: return 0;
    case 1: return k::kUnity;
    case 2: return 1;
    case 3: return k::kUnity - 1;
    case 4: return k::kMaxGain;
    case 5: return static_cast<std::int32_t>(rng.below(static_cast<std::uint32_t>(k::kMaxGain) + 1u));
    default: return static_cast<std::int32_t>(rng.below(static_cast<std::uint32_t>(k::kUnity) + 1u));
  }
}

// A buffer of samples with canaries, a random 0/1-sample offset (the paired path needs 4-byte alignment: odd offsets force the fallback).
struct Buf16 {
  alignas(8) std::int16_t store[kGuard + 2 + kN + kGuard];
  std::int16_t* data() { return store + kGuard + off; }
  std::uint32_t off = 0;
  void fill_canary() {
    for (std::int16_t& v : store) {
      v = kCanary;
    }
  }
  bool canaries_ok() const {
    for (std::uint32_t i = 0; i < kGuard + off; ++i) {
      if (store[i] != kCanary) return false;
    }
    for (std::uint32_t i = kGuard + off + kN; i < sizeof(store) / sizeof(store[0]); ++i) {
      if (store[i] != kCanary) return false;
    }
    return true;
  }
};

bool same(const std::int16_t* a, const std::int16_t* b, std::uint32_t n) { return std::memcmp(a, b, n * sizeof(std::int16_t)) == 0; }

constexpr int kIters = 3000;

}  // namespace

TEST_CASE("kernels: the ramp is the definition (last sample is `end`, the step is bounded, the shape is linear)") {
  CHECK(k::kRampShift == 6u);  // kBlock = 64
  const std::int32_t gains[] = {0, 1, 1000, 32768, 65535, 65536, 1000000, k::kMaxGain};  // (int32_t is long on the target: one type, spelt out)
  for (const std::int32_t start : gains) {
    for (const std::int32_t end : gains) {
      CHECK(k::ramp_gain20(start, end, kN - 1u) == 16 * end);  // the block ends exactly on the target (at Q20)
      const std::int32_t delta16 = 16 * (end > start ? end - start : start - end);
      const std::int32_t bound = (delta16 + static_cast<std::int32_t>(kN) - 1) / static_cast<std::int32_t>(kN);
      std::int32_t prev = 16 * start;
      for (std::uint32_t i = 0; i < kN; ++i) {
        const std::int32_t g = k::ramp_gain20(start, end, i);
        const std::int32_t step = g > prev ? g - prev : prev - g;
        INFO("start " << start << " end " << end << " i " << i);
        CHECK(step <= bound);                                                  // no zipper step: at most ceil(16 |delta| / kBlock) a sample
        CHECK(((g >= 16 * start && g <= 16 * end) || (g <= 16 * start && g >= 16 * end)));  // never overshoots
        if (end >= start) CHECK(g >= prev); else CHECK(g <= prev);              // monotone
        prev = g;
      }
    }
  }
  // A full-scale change of unity moves at most 1,024 Q16 (1.6 % of unity) per sample: 16,384 at Q20, the figure the header states.
  CHECK(((16 * k::kUnity + static_cast<std::int32_t>(kN) - 1) / static_cast<std::int32_t>(kN)) == 16384);
  // The top of the range does not overflow the ramp state, at the block lengths that are legal (64, and 128: the static_assert in kernels.h).
  CHECK(16 * static_cast<std::int64_t>(k::kMaxGain) * 128 < (static_cast<std::int64_t>(1) << 31));
}

TEST_CASE("kernels: gain rounds to nearest and saturates: bit for bit against the reference, within half an LSB of the exact product") {
  Rng rng(11);
  for (int iter = 0; iter < kIters; ++iter) {
    const std::uint32_t n = rng.below(kN + 1u);
    Buf16 src, got, emu, want;
    src.off = rng.below(4) == 0 ? 1u : 0u;
    got.off = rng.below(4) == 0 ? 1u : 0u;
    emu.off = got.off;
    want.off = got.off;
    got.fill_canary();
    emu.fill_canary();
    want.fill_canary();
    for (std::int16_t& v : src.store) {
      v = random_sample(rng, true);
    }
    const std::int32_t start = random_gain(rng);
    const std::int32_t end = rng.below(3) == 0 ? start : random_gain(rng);
    k::gain(got.data(), src.data(), n, start, end);
    k::detail::gain_simd<sm::EmuOps>(emu.data(), src.data(), n, start, end);
    k::gain_ref(want.data(), src.data(), n, start, end);
    INFO("n " << n << " start " << start << " end " << end << " src.off " << src.off << " dst.off " << got.off);
    CHECK(same(got.data(), want.data(), n));
    CHECK(same(emu.data(), want.data(), n));
    CHECK(got.canaries_ok());
    CHECK(emu.canaries_ok());
    // Against the exact product, in double: the gain of the sample is 16 start + floor(16 delta (i + 1) / 64) at Q20. Unsaturated, the result is the
    // product rounded to the nearest integer within 1/32 LSB (the Q19 truncation before the final rounding); saturated, it is the rail.
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::int32_t g20 = 16 * start + ((16 * (end - start) * static_cast<std::int32_t>(i + 1u)) >> k::kRampShift);
      const double exact = static_cast<double>(src.data()[i]) * static_cast<double>(g20) / 1048576.0;
      const double y = static_cast<double>(want.data()[i]);
      if (exact > 32767.5) {
        CHECK(want.data()[i] == 32767);
      } else if (exact < -32768.5) {
        CHECK(want.data()[i] == -32768);
      } else {
        const double err = y - exact;
        CHECK((err <= 0.5 + 1.0 / 32.0 && err >= -0.5 - 1.0 / 32.0));
      }
    }
    // In place.
    Buf16 inplace;
    inplace.off = src.off;
    std::memcpy(inplace.store, src.store, sizeof inplace.store);
    k::gain(inplace.data(), inplace.data(), n, start, end);
    CHECK(same(inplace.data(), want.data(), n));
  }
}

TEST_CASE("kernels: unity gain is exact, zero gain is silence, a big gain clips at the rails and does not wrap") {
  Rng rng(12);
  Buf16 src, dst;
  for (std::int16_t& v : src.store) {
    v = random_sample(rng, true);
  }
  k::gain(dst.data(), src.data(), kN, k::kUnity, k::kUnity);
  CHECK(same(dst.data(), src.data(), kN));  // a unity strip changes no bit
  k::gain(dst.data(), src.data(), kN, 0, 0);
  for (std::uint32_t i = 0; i < kN; ++i) {
    CHECK(dst.data()[i] == 0);
  }
  // +24 dB (the top of our 1.2+ law) of a half-scale sample saturates; of a quiet one it is 15.85 times it; a negative one saturates at -32768.
  const std::int32_t plus24 = 1038676;  // 10^(24/20) at Q16, about
  src.data()[0] = 16384;
  src.data()[1] = 1000;
  src.data()[2] = -20000;
  src.data()[3] = -1000;
  src.data()[4] = 32767;
  src.data()[5] = -32768;
  k::gain(dst.data(), src.data(), 6, plus24, plus24);
  CHECK(dst.data()[0] == 32767);
  CHECK(dst.data()[1] > 15847);
  CHECK(dst.data()[1] < 15851);
  CHECK(dst.data()[2] == -32768);
  CHECK(dst.data()[3] < -15847);
  CHECK(dst.data()[4] == 32767);
  CHECK(dst.data()[5] == -32768);
}

TEST_CASE("kernels: mac adds the Q19 product to the accumulators, bit for bit, and unity adds 16 a sample exactly") {
  Rng rng(13);
  for (int iter = 0; iter < kIters; ++iter) {
    const std::uint32_t n = rng.below(kN + 1u);
    alignas(8) std::int32_t base[kN + 2];
    alignas(8) std::int32_t got[kN + 2], emu[kN + 2], want[kN + 2];
    for (std::uint32_t i = 0; i < kN + 2u; ++i) {
      base[i] = rng.below(4) == 0 ? static_cast<std::int32_t>(rng.next()) : static_cast<std::int32_t>(rng.below(1u << 21)) - (1 << 20);
      got[i] = emu[i] = want[i] = base[i];
    }
    Buf16 src;
    src.off = rng.below(4) == 0 ? 1u : 0u;
    for (std::int16_t& v : src.store) {
      v = random_sample(rng, true);
    }
    const std::int32_t start = random_gain(rng);
    const std::int32_t end = rng.below(3) == 0 ? start : random_gain(rng);
    k::mac(got, src.data(), n, start, end);
    k::detail::mac_simd<sm::EmuOps>(emu, src.data(), n, start, end);
    k::mac_ref(want, src.data(), n, start, end);
    INFO("n " << n << " start " << start << " end " << end);
    CHECK(std::memcmp(got, want, sizeof got) == 0);
    CHECK(std::memcmp(emu, want, sizeof emu) == 0);
    for (std::uint32_t i = n; i < kN + 2u; ++i) {
      CHECK(got[i] == base[i]);  // nothing past n is touched
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::int64_t g20 = 16 * static_cast<std::int64_t>(start) + ((16 * static_cast<std::int64_t>(end - start) * static_cast<std::int64_t>(i + 1u)) >> k::kRampShift);
      const std::int32_t add = static_cast<std::int32_t>((static_cast<std::int64_t>(src.data()[i]) * g20) >> 16);
      CHECK(static_cast<std::uint32_t>(want[i]) == static_cast<std::uint32_t>(base[i]) + static_cast<std::uint32_t>(add));
    }
  }
  alignas(8) std::int32_t acc[kN] = {};
  Buf16 s;
  Rng r2(14);
  for (std::int16_t& v : s.store) v = random_sample(r2, true);
  k::mac(acc, s.data(), kN, k::kUnity, k::kUnity);
  for (std::uint32_t i = 0; i < kN; ++i) {
    CHECK(acc[i] == 16 * static_cast<std::int32_t>(s.data()[i]));  // Q19: a Q15 sample is 16 units, no truncation at unity
  }
}

TEST_CASE("kernels: mac2 (OS-PERF-R1, the two sends from one read) is mac then mac, bit for bit, steady or ramping, any length and alignment") {
  Rng rng(21);
  unsigned fused = 0;
  for (int iter = 0; iter < kIters; ++iter) {
    const std::uint32_t n = rng.below(3) == 0 ? kN : rng.below(kN + 1u);
    alignas(8) std::int32_t base_a[kN + 2], base_b[kN + 2];
    alignas(8) std::int32_t got_a[kN + 2], got_b[kN + 2], emu_a[kN + 2], emu_b[kN + 2], want_a[kN + 2], want_b[kN + 2];
    for (std::uint32_t i = 0; i < kN + 2u; ++i) {
      base_a[i] = rng.below(4) == 0 ? static_cast<std::int32_t>(rng.next()) : static_cast<std::int32_t>(rng.below(1u << 21)) - (1 << 20);
      base_b[i] = rng.below(4) == 0 ? static_cast<std::int32_t>(rng.next()) : static_cast<std::int32_t>(rng.below(1u << 21)) - (1 << 20);
      got_a[i] = emu_a[i] = want_a[i] = base_a[i];
      got_b[i] = emu_b[i] = want_b[i] = base_b[i];
    }
    Buf16 src;
    src.off = rng.below(5) == 0 ? 1u : 0u;
    for (std::int16_t& v : src.store) {
      v = random_sample(rng, true);
    }
    const bool steady_case = rng.below(3) != 0;  // the usual block is steady on both sends
    const std::int32_t sa = random_gain(rng);
    const std::int32_t sb = random_gain(rng);
    const k::GainRamp ra{sa, steady_case ? sa : random_gain(rng)};
    const k::GainRamp rb{sb, steady_case ? sb : random_gain(rng)};
    if (!k::mac2(got_a, got_b, src.data(), n, ra, rb)) {  // not the usual block: untouched, the strip runs mac twice
      CHECK(std::memcmp(got_a, base_a, sizeof got_a) == 0);
      CHECK(std::memcmp(got_b, base_b, sizeof got_b) == 0);
      k::mac(got_a, src.data(), n, ra.start, ra.end);
      k::mac(got_b, src.data(), n, rb.start, rb.end);
    }
    k::mac2_ref(want_a, want_b, src.data(), n, ra, rb);
    if (k::detail::mac2_steady<sm::EmuOps>(emu_a, emu_b, src.data(), n, ra, rb)) {
      ++fused;
    } else {
      k::detail::mac_simd<sm::EmuOps>(emu_a, src.data(), n, ra.start, ra.end);
      k::detail::mac_simd<sm::EmuOps>(emu_b, src.data(), n, rb.start, rb.end);
    }
    INFO("n " << n << " a " << ra.start << ".." << ra.end << " b " << rb.start << ".." << rb.end << " off " << src.off);
    CHECK(std::memcmp(got_a, want_a, sizeof got_a) == 0);
    CHECK(std::memcmp(got_b, want_b, sizeof got_b) == 0);
    CHECK(std::memcmp(emu_a, want_a, sizeof emu_a) == 0);
    CHECK(std::memcmp(emu_b, want_b, sizeof emu_b) == 0);
  }
  CHECK(fused > 100u);  // the fused loop is what ran for the steady, open, aligned, even blocks
  // A whole block at the production shape (aligned, 64 frames, both steady and open) takes the fused loop.
  alignas(8) std::int32_t a[kN] = {}, b[kN] = {};
  Buf16 s;
  Rng r2(22);
  for (std::int16_t& v : s.store) v = random_sample(r2, true);
  CHECK(k::detail::mac2_steady<sm::EmuOps>(a, b, s.data(), kN, k::GainRamp{k::kUnity, k::kUnity}, k::GainRamp{k::kUnity / 2, k::kUnity / 2}));
  for (std::uint32_t i = 0; i < kN; ++i) {
    CHECK(a[i] == 16 * static_cast<std::int32_t>(s.data()[i]));
  }
}

TEST_CASE("kernels: the strip kernels are gain then mac, bit for bit, one track or two, centre or stereo, steady or ramping") {
  Rng rng(20);
  for (int iter = 0; iter < kIters; ++iter) {
    const std::uint32_t n = rng.below(kN + 1u);
    const bool all_steady = rng.below(3) == 0;
    auto ramp = [&](bool steady) {
      const std::int32_t a = random_gain(rng);
      return k::GainRamp{a, steady ? a : random_gain(rng)};
    };
    auto pan_gain = [&](bool steady) {  // the pan law's gains never exceed unity
      const std::int32_t a = static_cast<std::int32_t>(rng.below(static_cast<std::uint32_t>(k::kUnity) + 1u));
      const std::int32_t b = static_cast<std::int32_t>(rng.below(static_cast<std::uint32_t>(k::kUnity) + 1u));
      return k::GainRamp{a, steady ? a : b};
    };
    // Two lanes with their own samples (a buffer each, copied two ways: the production kernel and the reference).
    Buf16 xa[2], xb[2];
    const std::uint32_t off = rng.below(5) == 0 ? 1u : 0u;
    for (int v = 0; v < 2; ++v) {
      xa[v].off = off;
      xb[v].off = off;
    }
    for (std::uint32_t i = 0; i < sizeof(xa[0].store) / sizeof(xa[0].store[0]); ++i) {
      xa[0].store[i] = xa[1].store[i] = random_sample(rng, true);
      xb[0].store[i] = xb[1].store[i] = random_sample(rng, true);
    }
    const k::GainRamp lva = ramp(all_steady || rng.below(2) == 0), lvb = ramp(all_steady || rng.below(2) == 0);
    const k::GainRamp la = pan_gain(all_steady || rng.below(2) == 0), lb = pan_gain(all_steady || rng.below(2) == 0);
    const k::GainRamp ra = rng.below(8) == 0 ? k::GainRamp{0, 0} : pan_gain(all_steady || rng.below(2) == 0);
    const k::GainRamp rb = rng.below(8) == 0 ? k::GainRamp{0, 0} : pan_gain(all_steady || rng.below(2) == 0);
    alignas(8) std::int32_t base[kN + 2];
    for (std::uint32_t i = 0; i < kN + 2u; ++i) {
      base[i] = static_cast<std::int32_t>(rng.below(1u << 20)) - (1 << 19);
    }
    alignas(8) std::int32_t l[2][kN + 2], r[2][kN + 2];
    for (int v = 0; v < 2; ++v) {
      for (std::uint32_t i = 0; i < kN + 2u; ++i) {
        l[v][i] = base[i];
        r[v][i] = ~base[i];
      }
    }
    auto lane = [&](Buf16& b, k::GainRamp lv, k::GainRamp left, k::GainRamp right) { return k::StripLane{b.data(), lv, left, right}; };
    INFO("n " << n << " off " << off << " steady " << all_steady);
    switch (rng.below(4)) {
      case 0:  // one track, stereo
        k::gain_pan(lane(xa[0], lva, la, ra), l[0], r[0], n);
        k::gain_pan_ref(lane(xa[1], lva, la, ra), l[1], r[1], n);
        break;
      case 1:  // one track, centre: the right accumulator is not touched
        k::gain_mac(lane(xa[0], lva, la, ra), l[0], n);
        k::gain_mac_ref(lane(xa[1], lva, la, ra), l[1], n);
        break;
      case 2:  // two tracks, stereo
        k::gain_pan2(lane(xa[0], lva, la, ra), lane(xb[0], lvb, lb, rb), l[0], r[0], n);
        k::gain_pan2_ref(lane(xa[1], lva, la, ra), lane(xb[1], lvb, lb, rb), l[1], r[1], n);
        break;
      default:  // two tracks, centre
        k::gain_mac2(lane(xa[0], lva, la, ra), lane(xb[0], lvb, lb, rb), l[0], n);
        k::gain_mac2_ref(lane(xa[1], lva, la, ra), lane(xb[1], lvb, lb, rb), l[1], n);
        break;
    }
    CHECK(std::memcmp(xa[0].store, xa[1].store, sizeof xa[0].store) == 0);  // the post-fader blocks, and nothing round them touched
    CHECK(std::memcmp(xb[0].store, xb[1].store, sizeof xb[0].store) == 0);
    CHECK(std::memcmp(l[0], l[1], sizeof l[0]) == 0);
    CHECK(std::memcmp(r[0], r[1], sizeof r[0]) == 0);
  }
}

TEST_CASE("kernels: the strip kernels on the instruction model equal the references (every shape)") {
  Rng rng(21);
  for (int iter = 0; iter < kIters; ++iter) {
    const std::uint32_t n = rng.below(kN + 1u);
    const bool all_steady = rng.below(3) == 0;
    auto gain_ramp = [&](bool steady, bool pan) {
      const std::uint32_t top = pan ? static_cast<std::uint32_t>(k::kUnity) : static_cast<std::uint32_t>(k::kMaxGain);
      const std::int32_t a = static_cast<std::int32_t>(rng.below(top + 1u));
      return k::GainRamp{a, steady ? a : static_cast<std::int32_t>(rng.below(top + 1u))};
    };
    Buf16 a_e, a_r, b_e, b_r;
    const std::uint32_t off = rng.below(5) == 0 ? 1u : 0u;
    a_e.off = a_r.off = b_e.off = b_r.off = off;
    for (std::uint32_t i = 0; i < sizeof(a_e.store) / sizeof(a_e.store[0]); ++i) {
      a_e.store[i] = a_r.store[i] = random_sample(rng, true);
      b_e.store[i] = b_r.store[i] = random_sample(rng, true);
    }
    const k::GainRamp la = gain_ramp(all_steady || rng.below(2) == 0, false), lb = gain_ramp(all_steady || rng.below(2) == 0, false);
    const k::GainRamp pa = gain_ramp(all_steady || rng.below(2) == 0, true), pb = gain_ramp(all_steady || rng.below(2) == 0, true);
    const k::GainRamp qa = gain_ramp(all_steady || rng.below(2) == 0, true), qb = gain_ramp(all_steady || rng.below(2) == 0, true);
    alignas(8) std::int32_t le[kN + 2], re[kN + 2], lr[kN + 2], rr[kN + 2];
    for (std::uint32_t i = 0; i < kN + 2u; ++i) {
      le[i] = lr[i] = static_cast<std::int32_t>(rng.below(1u << 20)) - (1 << 19);
      re[i] = rr[i] = ~le[i];
    }
    {
      const k::StripLane two_e[2] = {{a_e.data(), la, pa, qa}, {b_e.data(), lb, pb, qb}};
      k::detail::strip_simd<sm::EmuOps, 2, true>(two_e, le, re, n);
      k::gain_pan2_ref(k::StripLane{a_r.data(), la, pa, qa}, k::StripLane{b_r.data(), lb, pb, qb}, lr, rr, n);
    }
    CHECK(std::memcmp(a_e.store, a_r.store, sizeof a_e.store) == 0);
    CHECK(std::memcmp(b_e.store, b_r.store, sizeof b_e.store) == 0);
    CHECK(std::memcmp(le, lr, sizeof le) == 0);
    CHECK(std::memcmp(re, rr, sizeof re) == 0);
    // one lane, centre: the right gain is not read (a garbage ramp there must change nothing)
    {
      std::int32_t cl[kN + 2], cr[kN + 2];
      for (std::uint32_t i = 0; i < kN + 2u; ++i) cl[i] = cr[i] = le[i] & 0xFFFFF;
      Buf16 c_e, c_r;
      c_e.off = c_r.off = off;
      for (std::uint32_t i = 0; i < sizeof(c_e.store) / sizeof(c_e.store[0]); ++i) c_e.store[i] = c_r.store[i] = random_sample(rng, true);
      const k::StripLane one_e[1] = {{c_e.data(), la, pa, k::GainRamp{0x7FFFFFFF, -0x7FFFFFFF}}};
      k::detail::strip_simd<sm::EmuOps, 1, false>(one_e, cl, nullptr, n);
      k::gain_mac_ref(k::StripLane{c_r.data(), la, pa, qa}, cr, n);
      CHECK(std::memcmp(c_e.store, c_r.store, sizeof c_e.store) == 0);
      CHECK(std::memcmp(cl, cr, sizeof cl) == 0);
    }
  }
}

TEST_CASE("kernels: sat_clear rounds half up, saturates once and empties the accumulators") {
  Rng rng(14);
  for (int iter = 0; iter < kIters; ++iter) {
    const std::uint32_t n = rng.below(kN + 1u);
    alignas(8) std::int32_t a[kN + 2], b[kN + 2], c[kN + 2];
    for (std::uint32_t i = 0; i < kN + 2u; ++i) {
      std::int32_t v;
      switch (rng.below(5)) {
        case 0: v = static_cast<std::int32_t>(rng.next()); break;
        case 1: v = 16 * 32767 + static_cast<std::int32_t>(rng.below(33)) - 16; break;
        case 2: v = -16 * 32768 + static_cast<std::int32_t>(rng.below(33)) - 16; break;
        default: v = static_cast<std::int32_t>(rng.below(1u << 22)) - (1 << 21); break;
      }
      a[i] = b[i] = c[i] = v;
    }
    Buf16 got, emu, want;
    got.off = rng.below(4) == 0 ? 1u : 0u;
    emu.off = want.off = got.off;
    got.fill_canary();
    emu.fill_canary();
    want.fill_canary();
    k::sat_clear(got.data(), a, n);
    k::detail::sat_clear_simd<sm::EmuOps>(emu.data(), b, n);
    k::sat_clear_ref(want.data(), c, n);
    CHECK(same(got.data(), want.data(), n));
    CHECK(same(emu.data(), want.data(), n));
    CHECK(got.canaries_ok());
    CHECK(emu.canaries_ok());
    CHECK(std::memcmp(a, c, sizeof a) == 0);
    CHECK(std::memcmp(b, c, sizeof b) == 0);
    for (std::uint32_t i = 0; i < n; ++i) {
      CHECK(c[i] == 0);
    }
  }
  // The rounding, by hand: a Q15 LSB is 16 units, half of it is 8 and rounds up; the rails.
  alignas(8) std::int32_t acc[8] = {7, 8, 9, -8, -9, 16 * 32767 + 7, 16 * 32767 + 8, -16 * 32768 - 9};
  alignas(8) std::int16_t out[8];
  k::sat_clear(out, acc, 8);
  CHECK(out[0] == 0);
  CHECK(out[1] == 1);
  CHECK(out[2] == 1);
  CHECK(out[3] == 0);
  CHECK(out[4] == -1);
  CHECK(out[5] == 32767);
  CHECK(out[6] == 32767);   // 32767.5 rounds up and saturates
  CHECK(out[7] == -32768);
}

TEST_CASE("kernels: sat_clear_main adds the centre accumulator to both sides; acc_to_float_main hands the same sum to the chain unsaturated") {
  Rng rng(15);
  for (int iter = 0; iter < kIters / 2; ++iter) {
    const std::uint32_t n = rng.below(kN + 1u);
    const bool with_centre = rng.below(3) != 0;
    alignas(8) std::int32_t base_l[kN + 2], base_r[kN + 2], base_c[kN + 2];
    for (std::uint32_t i = 0; i < kN + 2u; ++i) {
      const bool big = rng.below(6) == 0;
      base_l[i] = big ? static_cast<std::int32_t>(rng.below(1u << 26)) - (1 << 25) : static_cast<std::int32_t>(rng.below(1u << 20)) - (1 << 19);
      base_r[i] = big ? static_cast<std::int32_t>(rng.below(1u << 26)) - (1 << 25) : static_cast<std::int32_t>(rng.below(1u << 20)) - (1 << 19);
      base_c[i] = big ? static_cast<std::int32_t>(rng.below(1u << 26)) - (1 << 25) : static_cast<std::int32_t>(rng.below(1u << 20)) - (1 << 19);
    }
    alignas(8) std::int32_t l[3][kN + 2], r[3][kN + 2], c[3][kN + 2];
    for (int v = 0; v < 3; ++v) {
      std::memcpy(l[v], base_l, sizeof base_l);
      std::memcpy(r[v], base_r, sizeof base_r);
      std::memcpy(c[v], base_c, sizeof base_c);
    }
    Buf16 gl, gr, el, er, wl, wr;
    gl.off = rng.below(5) == 0 ? 1u : 0u;
    gr.off = gl.off;
    el.off = er.off = wl.off = wr.off = gl.off;
    for (Buf16* b : {&gl, &gr, &el, &er, &wl, &wr}) b->fill_canary();
    k::sat_clear_main(gl.data(), gr.data(), l[0], r[0], with_centre ? c[0] : nullptr, n);
    k::detail::sat_clear_main_simd<sm::EmuOps>(el.data(), er.data(), l[1], r[1], with_centre ? c[1] : nullptr, n);
    k::sat_clear_main_ref(wl.data(), wr.data(), l[2], r[2], with_centre ? c[2] : nullptr, n);
    CHECK(same(gl.data(), wl.data(), n));
    CHECK(same(gr.data(), wr.data(), n));
    CHECK(same(el.data(), wl.data(), n));
    CHECK(same(er.data(), wr.data(), n));
    CHECK(gl.canaries_ok());
    CHECK(gr.canaries_ok());
    CHECK(std::memcmp(l[0], l[2], sizeof l[0]) == 0);
    CHECK(std::memcmp(r[0], r[2], sizeof r[0]) == 0);
    CHECK(std::memcmp(c[0], c[2], sizeof c[0]) == 0);
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::int32_t sum_l = base_l[i] + (with_centre ? base_c[i] : 0);
      const std::int32_t want_l = (sum_l + 8) >> 4;
      CHECK(wl.data()[i] == (want_l > 32767 ? 32767 : (want_l < -32768 ? -32768 : want_l)));
      CHECK(l[2][i] == 0);
      CHECK((!with_centre || c[2][i] == 0));
    }
    // the float form: the same sum over 2^19, no saturation, accumulators emptied
    alignas(8) float fl_[2][kN + 2], fr_[2][kN + 2];
    alignas(8) std::int32_t m_l[2][kN + 2], m_r[2][kN + 2], m_c[2][kN + 2];
    for (int v = 0; v < 2; ++v) {
      std::memcpy(m_l[v], base_l, sizeof base_l);
      std::memcpy(m_r[v], base_r, sizeof base_r);
      std::memcpy(m_c[v], base_c, sizeof base_c);
    }
    k::acc_to_float_main(fl_[0], fr_[0], m_l[0], m_r[0], with_centre ? m_c[0] : nullptr, n);
    k::acc_to_float_main_ref(fl_[1], fr_[1], m_l[1], m_r[1], with_centre ? m_c[1] : nullptr, n);
    CHECK(std::memcmp(fl_[0], fl_[1], n * sizeof(float)) == 0);
    CHECK(std::memcmp(fr_[0], fr_[1], n * sizeof(float)) == 0);
    for (std::uint32_t i = 0; i < n; ++i) {
      CHECK(fl_[1][i] == static_cast<float>(base_l[i] + (with_centre ? base_c[i] : 0)) / 524288.0f);
      CHECK(m_l[0][i] == 0);
      CHECK(m_r[0][i] == 0);
    }
  }
  // full scale is 2^19 units: 1.0; a sum of two full-scale sources is 2.0, which the chain gets whole
  alignas(8) std::int32_t al[2] = {1 << 19, 2 << 19}, ar[2] = {-(1 << 19), 0}, ac[2] = {0, 0};
  alignas(8) float fl2[2], fr2[2];
  k::acc_to_float_main(fl2, fr2, al, ar, ac, 2);
  CHECK(fl2[0] == 1.0f);
  CHECK(fl2[1] == 2.0f);
  CHECK(fr2[0] == -1.0f);
}

TEST_CASE("kernels: mono_mix and deinterleave2") {
  Rng rng(15);
  for (int iter = 0; iter < kIters; ++iter) {
    const std::uint32_t n = rng.below(kN + 1u);
    Buf16 l, r, got, emu, want;
    l.off = rng.below(5) == 0 ? 1u : 0u;
    r.off = rng.below(5) == 0 ? 1u : 0u;
    got.off = rng.below(5) == 0 ? 1u : 0u;
    emu.off = want.off = got.off;
    got.fill_canary();
    emu.fill_canary();
    want.fill_canary();
    for (std::int16_t& v : l.store) v = random_sample(rng, true);
    for (std::int16_t& v : r.store) v = random_sample(rng, true);
    k::mono_mix(got.data(), l.data(), r.data(), n);
    k::detail::mono_mix_simd<sm::EmuOps>(emu.data(), l.data(), r.data(), n);
    k::mono_mix_ref(want.data(), l.data(), r.data(), n);
    CHECK(same(got.data(), want.data(), n));
    CHECK(same(emu.data(), want.data(), n));
    CHECK(got.canaries_ok());
    for (std::uint32_t i = 0; i < n; ++i) {
      CHECK(want.data()[i] == static_cast<std::int16_t>((static_cast<std::int32_t>(l.data()[i]) + r.data()[i] + 1) >> 1));  // the exact mean, half up
    }

    // deinterleave: an interleaved frame buffer of 2n samples
    alignas(8) std::int16_t inter[2 * kN + 4];
    for (std::int16_t& v : inter) v = random_sample(rng, true);
    const std::uint32_t io = rng.below(5) == 0 ? 1u : 0u;
    Buf16 gl, gr, el, er, wl, wr;
    gl.off = rng.below(5) == 0 ? 1u : 0u;
    gr.off = rng.below(5) == 0 ? 1u : 0u;
    el.off = wl.off = gl.off;
    er.off = wr.off = gr.off;
    for (Buf16* b : {&gl, &gr, &el, &er, &wl, &wr}) b->fill_canary();
    k::deinterleave2(gl.data(), gr.data(), inter + io, n);
    k::detail::deinterleave2_simd<sm::EmuOps>(el.data(), er.data(), inter + io, n);
    k::deinterleave2_ref(wl.data(), wr.data(), inter + io, n);
    CHECK(same(gl.data(), wl.data(), n));
    CHECK(same(gr.data(), wr.data(), n));
    CHECK(same(el.data(), wl.data(), n));
    CHECK(same(er.data(), wr.data(), n));
    CHECK(gl.canaries_ok());
    CHECK(gr.canaries_ok());
    for (std::uint32_t i = 0; i < n; ++i) {
      CHECK(wl.data()[i] == inter[io + 2u * i]);
      CHECK(wr.data()[i] == inter[io + 2u * i + 1u]);
    }

    // interleave2 (OS-PERF3): two planes to frames, against the reference and the instruction model, and the exact inverse of deinterleave2
    alignas(8) std::int16_t go[2 * kN + 4];
    alignas(8) std::int16_t eo[2 * kN + 4];
    alignas(8) std::int16_t wo[2 * kN + 4];
    for (std::int16_t* b : {go, eo, wo}) {
      for (std::uint32_t i = 0; i < 2 * kN + 4; ++i) b[i] = 0x5A5A;  // canary
    }
    const std::uint32_t oo = rng.below(5) == 0 ? 1u : 0u;
    k::interleave2(go + oo, wl.data(), wr.data(), n);
    k::detail::interleave2_simd<sm::EmuOps>(eo + oo, wl.data(), wr.data(), n);
    k::interleave2_ref(wo + oo, wl.data(), wr.data(), n);
    for (std::uint32_t i = 0; i < 2 * kN + 4; ++i) {
      CHECK(go[i] == wo[i]);  // including the guards before and after the n frames
      CHECK(eo[i] == wo[i]);
    }
    for (std::uint32_t i = 0; i < 2u * n; ++i) {
      CHECK(wo[oo + i] == inter[io + i]);  // deinterleave2 then interleave2 is the identity
    }
  }
}

TEST_CASE("kernels: meter gives the peak and the sum of squares, bit for bit, and accumulates") {
  Rng rng(16);
  for (int iter = 0; iter < kIters; ++iter) {
    k::MeterAcc got{0, 0}, emu{0, 0}, want{0, 0};
    std::uint32_t peak = 0;  // the definition once more, plainly, over every call
    std::uint64_t sumsq = 0;
    const int calls = 1 + static_cast<int>(rng.below(3));
    for (int c = 0; c < calls; ++c) {
      const std::uint32_t n = rng.below(kN + 1u);
      Buf16 src;
      src.off = rng.below(5) == 0 ? 1u : 0u;
      const bool all_min = rng.below(10) == 0;
      for (std::int16_t& v : src.store) v = all_min ? static_cast<std::int16_t>(-32768) : random_sample(rng, true);
      k::meter(src.data(), n, got);
      k::detail::meter_simd<sm::EmuOps>(src.data(), n, emu);
      k::meter_ref(src.data(), n, want);
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::int32_t v = src.data()[i];
        const std::uint32_t a = static_cast<std::uint32_t>(v < 0 ? -v : v);
        peak = a > peak ? a : peak;
        sumsq += static_cast<std::uint64_t>(static_cast<std::int64_t>(v) * v);
      }
    }
    CHECK(want.peak == peak);
    CHECK(want.sumsq == sumsq);
    CHECK(got.peak == want.peak);
    CHECK(emu.peak == want.peak);
    CHECK(got.sumsq == want.sumsq);
    CHECK(emu.sumsq == want.sumsq);
  }
  // |-32768| is 32768 and a full block of it is 64 * 2^30.
  alignas(8) std::int16_t all_min[kN];
  for (std::int16_t& v : all_min) v = -32768;
  k::MeterAcc a{0, 0};
  k::meter(all_min, kN, a);
  CHECK(a.peak == 32768u);
  CHECK(a.sumsq == static_cast<std::uint64_t>(kN) * (1ull << 30));
  // An empty block leaves the accumulators alone (the identities of the lane search are not data).
  k::MeterAcc e{7, 9};
  k::meter(all_min, 0, e);
  CHECK(e.peak == 7u);
  CHECK(e.sumsq == 9u);
}

TEST_CASE("kernels: q15_to_float is exact; float_to_q15 equals the reference except by one LSB on exact ties") {
  Rng rng(17);
  for (int iter = 0; iter < kIters; ++iter) {
    const std::uint32_t n = rng.below(kN + 1u);
    Buf16 src;
    src.off = rng.below(5) == 0 ? 1u : 0u;
    for (std::int16_t& v : src.store) v = random_sample(rng, true);
    alignas(8) float got[kN + 2], emu[kN + 2], want[kN + 2];
    k::q15_to_float(got, src.data(), n);
    k::detail::q15_to_float_simd<sm::EmuOps>(emu, src.data(), n);
    k::q15_to_float_ref(want, src.data(), n);
    CHECK(std::memcmp(got, want, n * sizeof(float)) == 0);
    CHECK(std::memcmp(emu, want, n * sizeof(float)) == 0);
    for (std::uint32_t i = 0; i < n; ++i) {
      CHECK(want[i] * 32768.0f == static_cast<float>(src.data()[i]));  // exactly the sample over 2^15
    }

    // float -> Q15: random values over and beyond the range, ties, and the special values
    alignas(8) float f[kN + 2];
    for (std::uint32_t i = 0; i < kN + 2u; ++i) {
      switch (rng.below(6)) {
        case 0: f[i] = (static_cast<float>(static_cast<std::int32_t>(rng.below(65536)) - 32768) + 0.5f) / 32768.0f; break;  // an exact tie
        case 1: f[i] = static_cast<float>(static_cast<std::int32_t>(rng.next())) * (1.0f / 1073741824.0f); break;            // -2 .. 2
        case 2: f[i] = (static_cast<float>(rng.below(4096)) - 2048.0f) * 1024.0f; break;                                      // far outside
        default: f[i] = (static_cast<float>(static_cast<std::int32_t>(rng.below(2000000)) - 1000000)) * (1.0f / 1000000.0f); break;
      }
    }
    Buf16 qg, qe, qw;
    qg.off = rng.below(5) == 0 ? 1u : 0u;
    qe.off = qw.off = qg.off;
    qg.fill_canary();
    qe.fill_canary();
    qw.fill_canary();
    k::float_to_q15(qg.data(), f, n);
    k::detail::float_to_q15_simd<sm::EmuOps>(qe.data(), f, n);
    k::float_to_q15_ref(qw.data(), f, n);
    CHECK(same(qg.data(), qe.data(), n));  // the instructions and their model agree exactly
    CHECK(qg.canaries_ok());
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::int32_t d = static_cast<std::int32_t>(qg.data()[i]) - qw.data()[i];
      const float scaled = f[i] * 32768.0f;
      const bool tie = scaled > -32768.0f && scaled < 32767.0f && (scaled - static_cast<float>(static_cast<std::int32_t>(scaled))) == 0.5f * (scaled >= 0.0f ? 1.0f : -1.0f);
      INFO("x " << f[i] << " got " << qg.data()[i] << " want " << qw.data()[i]);
      if (tie) {
        CHECK((d == 1 || d == -1 || d == 0));  // nearest-even against away-from-zero
      } else {
        CHECK(d == 0);
      }
    }
  }
  // The special values: NaN is +32767 in both (the comparison form), infinities and out-of-range values saturate.
  alignas(8) float special[8] = {__builtin_nanf(""), 1e30f, -1e30f, 1.0f, -1.0f, 0.99999f, -0.00001f, 0.0f};
  alignas(8) std::int16_t a[8], b[8];
  k::float_to_q15(a, special, 8);
  k::float_to_q15_ref(b, special, 8);
  CHECK(a[0] == 32767);
  CHECK(b[0] == 32767);
  CHECK(a[1] == 32767);
  CHECK(a[2] == -32768);
  CHECK(a[3] == 32767);
  CHECK(a[4] == -32768);
  CHECK(same(a, b, 8));
}

TEST_CASE("kernels: the model of each instruction matches its definition") {
  // The Emu model is what the host tests lean on; its lanes and roundings are pinned here against hand-worked values (the target run compares
  // the real instructions with it through every kernel test above).
  const std::uint32_t w = sm::pack_lanes(-3, 5);
  CHECK(sm::lane0(w) == -3);
  CHECK(sm::lane1(w) == 5);
  CHECK(sm::EmuOps::smlawb(65536, w, 10) == 10 - 3);           // unity gain: the lane itself
  CHECK(sm::EmuOps::smlawt(65536, w, 10) == 10 + 5);
  CHECK(sm::EmuOps::smlawb(32768, w, 0) == -2);                // floor(-1.5)
  CHECK(sm::EmuOps::smlawt(32768, w, 0) == 2);                 // floor(2.5)
  CHECK(sm::EmuOps::smlald(sm::pack_lanes(-32768, -32768), sm::pack_lanes(-32768, -32768), 1) == 1u + (1ull << 31));
  CHECK(sm::EmuOps::max2(sm::pack_lanes(1, -5), sm::pack_lanes(-2, -7)) == sm::pack_lanes(1, -5));
  CHECK(sm::EmuOps::min2(sm::pack_lanes(1, -5), sm::pack_lanes(-2, -7)) == sm::pack_lanes(-2, -7));
  CHECK(sm::EmuOps::avg_round(sm::pack_lanes(32767, -32768), sm::pack_lanes(32767, -32768)) == sm::pack_lanes(32767, -32768));
  CHECK(sm::EmuOps::avg_round(sm::pack_lanes(3, -3), sm::pack_lanes(0, 0)) == sm::pack_lanes(2, -1));  // half up: 1.5 -> 2, -1.5 -> -1
  CHECK(sm::EmuOps::avg_round(sm::pack_lanes(32767, -32768), sm::pack_lanes(32766, -32767)) == sm::pack_lanes(32767, -32767));
  CHECK(sm::EmuOps::pack_lo(sm::pack_lanes(1, 2), sm::pack_lanes(3, 4)) == sm::pack_lanes(1, 3));
  CHECK(sm::EmuOps::pack_hi(sm::pack_lanes(1, 2), sm::pack_lanes(3, 4)) == sm::pack_lanes(2, 4));
  CHECK(sm::EmuOps::ssat16(40000) == 32767);
  CHECK(sm::EmuOps::ssat16(-40000) == -32768);
  CHECK(sm::EmuOps::ssat16(-5) == -5);
  CHECK(sm::EmuOps::ssat16_q4(16 * 100) == 100);
  CHECK(sm::EmuOps::ssat16_q4(16 * 100 + 15) == 100);                // the shift is a floor: the rounding constant is added by the caller
  CHECK(sm::EmuOps::ssat16_q4(-1) == -1);
  CHECK(sm::EmuOps::ssat16_q4(16 * 40000) == 32767);
  CHECK(sm::EmuOps::ssat16_q4(-16 * 40000) == -32768);
}

TEST_CASE("kernels: the real instructions agree with the model on random words") {
  // On the host NativeOps IS EmuOps (the case is then trivially true and says so); on the Cortex-M4 it compares each instruction with its model
  // over random operands, extremes included.
  Rng rng(18);
  for (int i = 0; i < 20000; ++i) {
    const std::uint32_t a = rng.next();
    const std::uint32_t b = rng.next();
    const std::int32_t g = static_cast<std::int32_t>(rng.below(static_cast<std::uint32_t>(k::kUnity) + 1u));
    const std::int32_t acc = static_cast<std::int32_t>(rng.next());
    CHECK(sm::NativeOps::smlawb(g, a, acc) == sm::EmuOps::smlawb(g, a, acc));
    CHECK(sm::NativeOps::smlawt(g, a, acc) == sm::EmuOps::smlawt(g, a, acc));
    const std::uint64_t acc64 = (static_cast<std::uint64_t>(rng.next()) << 32) | rng.next();
    CHECK(sm::NativeOps::smlald(a, b, acc64) == sm::EmuOps::smlald(a, b, acc64));
    CHECK(sm::NativeOps::max2(a, b) == sm::EmuOps::max2(a, b));
    CHECK(sm::NativeOps::min2(a, b) == sm::EmuOps::min2(a, b));
    CHECK(sm::NativeOps::avg_round(a, b) == sm::EmuOps::avg_round(a, b));
    CHECK(sm::NativeOps::pack_lo(a, b) == sm::EmuOps::pack_lo(a, b));
    CHECK(sm::NativeOps::pack_hi(a, b) == sm::EmuOps::pack_hi(a, b));
    CHECK(sm::NativeOps::pack(acc, g) == sm::EmuOps::pack(acc, g));
    CHECK(sm::NativeOps::ssat16(acc) == sm::EmuOps::ssat16(acc));
    CHECK(sm::NativeOps::ssat16_q4(acc) == sm::EmuOps::ssat16_q4(acc));
  }
  CHECK(sm::kNativeIsHardware == (
#if defined(__ARM_FEATURE_SIMD32)
                                     true
#else
                                     false
#endif
                                     ));
}

#if defined(__ARM_ARCH_7EM__)
// On the Cortex-M4 target: with CCR.UNALIGN_TRP set an unaligned LDR/STR raises a UsageFault (the platform's fault handler then fails the whole
// suite), so this case proves the kernels never rely on unaligned access (ADR 0011 lesson 1) in any shape: odd-offset pointers take the scalar
// loop, aligned ones the paired loop.
TEST_CASE("kernels: no unaligned word access on the target (CCR.UNALIGN_TRP set)") {
  volatile std::uint32_t* const ccr = reinterpret_cast<volatile std::uint32_t*>(0xE000ED14);
  const std::uint32_t saved = *ccr;
  *ccr = saved | (1u << 3);
  Rng rng(19);
  bool ran = false;
  for (int iter = 0; iter < 400; ++iter) {
    const std::uint32_t n = rng.below(kN + 1u);
    Buf16 a, b, c;
    a.off = rng.below(3) == 0 ? 1u : 0u;
    b.off = rng.below(3) == 0 ? 1u : 0u;
    c.off = rng.below(3) == 0 ? 1u : 0u;
    for (std::int16_t& v : a.store) v = random_sample(rng, true);
    for (std::int16_t& v : b.store) v = random_sample(rng, true);
    alignas(8) std::int32_t acc[kN + 2] = {};
    alignas(8) std::int16_t inter[2 * kN + 4] = {};
    alignas(8) float fl[kN + 2] = {};
    const std::int32_t start = random_gain(rng);
    const std::int32_t end = random_gain(rng);
    k::gain(c.data(), a.data(), n, start, end);
    k::mac(acc, a.data(), n, start, end);
    const k::StripLane la{a.data(), k::GainRamp{start, end}, k::GainRamp{end, start}, k::GainRamp{start, start}};
    const k::StripLane lb{b.data(), k::GainRamp{end, end}, k::GainRamp{start, end}, k::GainRamp{0, 0}};
    k::gain_pan(la, acc, acc, n);
    k::gain_mac(lb, acc, n);
    k::gain_pan2(la, lb, acc, acc, n);
    k::gain_mac2(la, lb, acc, n);
    k::sat_clear(c.data(), acc, n);
    k::sat_clear_main(c.data(), b.data(), acc, acc, acc, n);
    k::acc_to_float_main(fl, fl + 2, acc, acc, nullptr, n);
    k::mono_mix(c.data(), a.data(), b.data(), n);
    k::deinterleave2(a.data(), b.data(), inter + rng.below(2), n);
    k::MeterAcc m{0, 0};
    k::meter(a.data(), n, m);
    k::q15_to_float(fl, a.data(), n);
    k::float_to_q15(c.data(), fl, n);
    ran = true;
  }
  *ccr = saved;
  CHECK(ran);  // reaching this line is the test: no UsageFault
}
#endif
