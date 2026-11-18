// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The paired (two samples a word) loops behind kernels.h, written once over an `Ops` policy (simd.h). kernels.cpp instantiates them
// with NativeOps for the production functions; the tests instantiate them with EmuOps (every host, and the target for the comparison with
// the real instructions). Nothing here is part of the engine's interface.
//
// Every loop has the same shape: a paired body for the aligned part, a scalar tail (the odd last sample), and a scalar fallback for a
// pointer that is not 4-byte aligned; the scalar parts compute exactly what the reference does, so the three together are bit-identical to
// it. A word is read and written with memcpy of four bytes (one LDR / STR), never through a cast.
//
// The ramp state of a paired loop is `Gen`: for a steady gain the Q20 constant, for a ramp the running sum of ramp_gain20's definition held in
// UNSIGNED words (the sum after the last sample is not used but is formed, and a signed add there could overflow at the top of the range).

#include <o3os/audio/kernels.h>
#include <o3os/audio/simd.h>
#include <o3os/audio/test_signal.h>

#include <cstdint>
#include <cstring>

#if defined(__ARM_FP) && defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#endif

namespace o3os::audio::kernel::detail {

O3OS_AUDIO_INLINE bool aligned4(const void* p) noexcept { return (reinterpret_cast<std::uintptr_t>(p) & 3u) == 0u; }

O3OS_AUDIO_INLINE std::uint32_t load_pair(const std::int16_t* p) noexcept {
  std::uint32_t w;
  std::memcpy(&w, p, sizeof w);
  return w;
}
O3OS_AUDIO_INLINE void store_pair(std::int16_t* p, std::uint32_t w) noexcept { std::memcpy(p, &w, sizeof w); }

// ---- the scalar definitions (also the tails of the paired loops) -------------------------------------------------------------
// The Q19 product of a sample and a Q20 gain: floor(s * g20 / 65536). int64 so that the reference is defined for any gain.
O3OS_AUDIO_INLINE std::int32_t prod_q19(std::int16_t s, std::int32_t g20) noexcept {
  return static_cast<std::int32_t>((static_cast<std::int64_t>(s) * g20) >> 16);
}
O3OS_AUDIO_INLINE std::int32_t add_wrap(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
O3OS_AUDIO_INLINE std::int32_t sat16(std::int32_t v) noexcept { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }
// Q19 to Q15: rounded half up, saturated.
O3OS_AUDIO_INLINE std::int16_t q19_to_q15(std::int32_t v) noexcept { return static_cast<std::int16_t>(sat16(add_wrap(v, kAccRound) >> kGainBits)); }
// The post-fader sample: the Q19 product rounded to Q15.
O3OS_AUDIO_INLINE std::int16_t gain_sample(std::int16_t s, std::int32_t g20) noexcept { return q19_to_q15(prod_q19(s, g20)); }

// ---- ramp state -------------------------------------------------------------------------------------------------------------
// No member initialisers: an array of these in a kernel would be zeroed by a call to memset, which the SRAM_L path may not make (check_ramfunc.py);
// init() sets everything that is read.
template <bool Steady>
struct Gen;
template <>
struct Gen<true> {
  std::int32_t g;
  O3OS_AUDIO_INLINE void init(GainRamp r) noexcept { g = 16 * r.start; }
  O3OS_AUDIO_INLINE std::int32_t next() noexcept { return g; }
};
template <>
struct Gen<false> {
  std::uint32_t g;     // 16 * start * kBlock + 16 * (end - start) * (i + 1): the numerator of ramp_gain20, kBlock times the gain
  std::uint32_t step;  // 16 * (end - start), modulo 2^32
  O3OS_AUDIO_INLINE void init(GainRamp r) noexcept {
    step = static_cast<std::uint32_t>(16 * (r.end - r.start));
    g = static_cast<std::uint32_t>(16 * r.start) * hal::audio::kBlock + step;
  }
  O3OS_AUDIO_INLINE std::int32_t next() noexcept {
    const std::int32_t v = static_cast<std::int32_t>(g >> kRampShift);
    g += step;
    return v;
  }
};
O3OS_AUDIO_INLINE bool steady(GainRamp r) noexcept { return r.start == r.end; }

// ---- gain, mac --------------------------------------------------------------------------------------------------------------
// Two samples of the post-fader block: the Q19 products (the rounding constant rides in the accumulator operand of SMLAWB), then the
// shift and the saturation of SSAT, then one pack.
template <class Ops>
O3OS_AUDIO_INLINE std::uint32_t level_pair(std::int32_t g0, std::int32_t g1, std::uint32_t w) noexcept {
  return Ops::pack(Ops::ssat16_q4(Ops::smlawb(g0, w, kAccRound)), Ops::ssat16_q4(Ops::smlawt(g1, w, kAccRound)));
}

template <class Ops, bool Steady>
O3OS_AUDIO_INLINE void gain_pairs(std::int16_t* dst, const std::int16_t* src, std::uint32_t n, GainRamp r, std::uint32_t& i) noexcept {
  Gen<Steady> g;
  g.init(r);
  for (; i + 2u <= n; i += 2u) {
    const std::uint32_t w = load_pair(src + i);
    const std::int32_t g0 = g.next();
    const std::int32_t g1 = g.next();
    store_pair(dst + i, level_pair<Ops>(g0, g1, w));
  }
}

template <class Ops>
O3OS_AUDIO_INLINE void gain_simd(std::int16_t* dst, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept {
  std::uint32_t i = 0;
  if (aligned4(dst) && aligned4(src)) {
    if (start == end) {
      gain_pairs<Ops, true>(dst, src, n, GainRamp{start, end}, i);
    } else {
      gain_pairs<Ops, false>(dst, src, n, GainRamp{start, end}, i);
    }
  }
  for (; i < n; ++i) {
    dst[i] = gain_sample(src[i], ramp_gain20(start, end, i));
  }
}

template <class Ops, bool Steady>
O3OS_AUDIO_INLINE void mac_pairs(std::int32_t* acc, const std::int16_t* src, std::uint32_t n, GainRamp r, std::uint32_t& i) noexcept {
  Gen<Steady> g;
  g.init(r);
  for (; i + 2u <= n; i += 2u) {
    const std::uint32_t w = load_pair(src + i);
    const std::int32_t g0 = g.next();
    const std::int32_t g1 = g.next();
    acc[i] = Ops::smlawb(g0, w, acc[i]);
    acc[i + 1u] = Ops::smlawt(g1, w, acc[i + 1u]);
  }
}

template <class Ops>
O3OS_AUDIO_INLINE void mac_simd(std::int32_t* acc, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept {
  std::uint32_t i = 0;
  if (aligned4(src)) {
    if (start == end) {
      if (start == 0) {
        return;  // a closed route adds nothing
      }
      mac_pairs<Ops, true>(acc, src, n, GainRamp{start, end}, i);
    } else {
      mac_pairs<Ops, false>(acc, src, n, GainRamp{start, end}, i);
    }
  }
  for (; i < n; ++i) {
    acc[i] = add_wrap(acc[i], prod_q19(src[i], ramp_gain20(start, end, i)));
  }
}

// OS-PERF-R1: the two sends of a strip from one read of its block. The fused loop takes only the case of every strip at rest, both gains steady and open, an aligned block of an even length; it
// returns false (nothing touched) for any other, and the caller runs mac twice. In that case the two are the same sums as mac_pairs<Ops, true> makes, one pair at a time (integers: no order to keep).
// acc_a and acc_b are distinct arrays.
template <class Ops>
O3OS_AUDIO_INLINE bool mac2_steady(std::int32_t* acc_a, std::int32_t* acc_b, const std::int16_t* src, std::uint32_t n, GainRamp ra, GainRamp rb) noexcept {
  if (!aligned4(src) || (n & 1u) != 0u || ra.start != ra.end || rb.start != rb.end) {  // (a steady gain of 0 adds 0: the same sums as mac's early return)
    return false;
  }
  const std::int32_t ga = 16 * ra.start;
  const std::int32_t gb = 16 * rb.start;
  for (std::uint32_t i = 0; i < n; i += 2u) {
    const std::uint32_t w = load_pair(src + i);
    acc_a[i] = Ops::smlawb(ga, w, acc_a[i]);
    acc_a[i + 1u] = Ops::smlawt(ga, w, acc_a[i + 1u]);
    acc_b[i] = Ops::smlawb(gb, w, acc_b[i]);
    acc_b[i + 1u] = Ops::smlawt(gb, w, acc_b[i + 1u]);
  }
  return true;
}

// ---- the strip pass ---------------------------------------------------------------------------------------------------------
// N lanes (1 or 2), the pan law as two accumulators (Stereo) or one (centre). The accumulators are loaded and stored once per pair of samples
// whatever N. The lanes are written out, not looped over: at -Os the compiler leaves a loop over a constant two lanes as a loop that reloads the lane
// parameters from memory every pair, which made the fused two-track pass slower than two single ones on the Cortex-M4F.
template <bool Stereo, bool Steady>
struct LaneGen {
  std::int16_t* x;
  Gen<Steady> v;  // level
  Gen<Steady> l;  // the pan law's left gain
  Gen<Steady> r;  // and its right (not formed by a centre kernel: whatever is in the lane's right ramp is not read)
  O3OS_AUDIO_INLINE void init(const StripLane& s) noexcept {
    x = s.x;
    v.init(s.level);
    l.init(s.left);
    if constexpr (Stereo) {
      r.init(s.right);
    }
  }
};

// One lane's pair of samples: the level stage (stored back in place: the post-fader block a tap reads), then the pan products into the accumulators.
template <class Ops, bool Stereo, bool Steady>
O3OS_AUDIO_INLINE void lane_pair(LaneGen<Stereo, Steady>& g, std::uint32_t i, std::int32_t& l0, std::int32_t& l1, std::int32_t& r0, std::int32_t& r1) noexcept {
  const std::uint32_t w = load_pair(g.x + i);
  const std::int32_t v0 = g.v.next();
  const std::int32_t v1 = g.v.next();
  const std::uint32_t y = level_pair<Ops>(v0, v1, w);
  store_pair(g.x + i, y);
  const std::int32_t a0 = g.l.next();
  const std::int32_t a1 = g.l.next();
  l0 = Ops::smlawb(a0, y, l0);
  l1 = Ops::smlawt(a1, y, l1);
  if constexpr (Stereo) {
    const std::int32_t b0 = g.r.next();
    const std::int32_t b1 = g.r.next();
    r0 = Ops::smlawb(b0, y, r0);
    r1 = Ops::smlawt(b1, y, r1);
  }
}

template <class Ops, unsigned N, bool Stereo, bool Steady>
O3OS_AUDIO_INLINE void strip_pairs(const StripLane (&ln)[N], std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n, std::uint32_t& i) noexcept {
  static_assert(N == 1u || N == 2u, "one or two lanes");
  LaneGen<Stereo, Steady> a;
  a.init(ln[0]);
  [[maybe_unused]] LaneGen<Stereo, Steady> b;
  if constexpr (N > 1u) {
    b.init(ln[1]);
  }
  for (; i + 2u <= n; i += 2u) {
    std::int32_t l0 = acc_l[i];
    std::int32_t l1 = acc_l[i + 1u];
    std::int32_t r0 = 0;
    std::int32_t r1 = 0;
    if constexpr (Stereo) {
      r0 = acc_r[i];
      r1 = acc_r[i + 1u];
    }
    lane_pair<Ops>(a, i, l0, l1, r0, r1);
    if constexpr (N > 1u) {
      lane_pair<Ops>(b, i, l0, l1, r0, r1);
    }
    acc_l[i] = l0;
    acc_l[i + 1u] = l1;
    if constexpr (Stereo) {
      acc_r[i] = r0;
      acc_r[i + 1u] = r1;
    }
  }
}

template <class Ops, unsigned N, bool Stereo>
O3OS_AUDIO_INLINE void strip_simd(const StripLane (&ln)[N], std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept {
  std::uint32_t i = 0;
  bool ok = true;
  bool all_steady = true;
  for (unsigned t = 0; t < N; ++t) {
    ok = ok && aligned4(ln[t].x);
    all_steady = all_steady && steady(ln[t].level) && steady(ln[t].left) && (!Stereo || steady(ln[t].right));
  }
  if (ok) {
    if (all_steady) {  // every gain steady: the common block
      strip_pairs<Ops, N, Stereo, true>(ln, acc_l, acc_r, n, i);
    } else {
      strip_pairs<Ops, N, Stereo, false>(ln, acc_l, acc_r, n, i);
    }
  }
  for (; i < n; ++i) {
    for (unsigned t = 0; t < N; ++t) {
      const std::int16_t y = gain_sample(ln[t].x[i], ramp_gain20(ln[t].level.start, ln[t].level.end, i));
      ln[t].x[i] = y;
      acc_l[i] = add_wrap(acc_l[i], prod_q19(y, ramp_gain20(ln[t].left.start, ln[t].left.end, i)));
      if constexpr (Stereo) {
        acc_r[i] = add_wrap(acc_r[i], prod_q19(y, ramp_gain20(ln[t].right.start, ln[t].right.end, i)));
      }
    }
  }
}

// ---- sum and saturate -------------------------------------------------------------------------------------------------------
template <class Ops>
O3OS_AUDIO_INLINE void sat_clear_simd(std::int16_t* dst, std::int32_t* acc, std::uint32_t n) noexcept {
  std::uint32_t i = 0;
  if (aligned4(dst)) {
    for (; i + 2u <= n; i += 2u) {
      store_pair(dst + i, Ops::pack(Ops::ssat16_q4(add_wrap(acc[i], kAccRound)), Ops::ssat16_q4(add_wrap(acc[i + 1u], kAccRound))));
      acc[i] = 0;
      acc[i + 1u] = 0;
    }
  }
  for (; i < n; ++i) {
    dst[i] = q19_to_q15(acc[i]);
    acc[i] = 0;
  }
}

template <class Ops>
O3OS_AUDIO_INLINE void sat_clear_main_simd(std::int16_t* dst_l, std::int16_t* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c,
                                           std::uint32_t n) noexcept {
  if (acc_c == nullptr) {
    sat_clear_simd<Ops>(dst_l, acc_l, n);
    sat_clear_simd<Ops>(dst_r, acc_r, n);
    return;
  }
  std::uint32_t i = 0;
  if (aligned4(dst_l) && aligned4(dst_r)) {
    for (; i + 2u <= n; i += 2u) {
      const std::int32_t c0 = add_wrap(acc_c[i], kAccRound);
      const std::int32_t c1 = add_wrap(acc_c[i + 1u], kAccRound);
      store_pair(dst_l + i, Ops::pack(Ops::ssat16_q4(add_wrap(acc_l[i], c0)), Ops::ssat16_q4(add_wrap(acc_l[i + 1u], c1))));
      store_pair(dst_r + i, Ops::pack(Ops::ssat16_q4(add_wrap(acc_r[i], c0)), Ops::ssat16_q4(add_wrap(acc_r[i + 1u], c1))));
      acc_l[i] = acc_l[i + 1u] = 0;
      acc_r[i] = acc_r[i + 1u] = 0;
      acc_c[i] = acc_c[i + 1u] = 0;
    }
  }
  for (; i < n; ++i) {
    dst_l[i] = q19_to_q15(add_wrap(acc_l[i], acc_c[i]));
    dst_r[i] = q19_to_q15(add_wrap(acc_r[i], acc_c[i]));
    acc_l[i] = 0;
    acc_r[i] = 0;
    acc_c[i] = 0;
  }
}

template <class Ops>
O3OS_AUDIO_INLINE void acc_to_float_main_simd(float* dst_l, float* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c,
                                              std::uint32_t n) noexcept {
  constexpr float kScale = 1.0f / 524288.0f;  // 2^-19
  if (acc_c == nullptr) {  // (OS-PERF3: the test of the centre accumulator outside the loop; the sum with a missing one is the accumulator itself)
    for (std::uint32_t i = 0; i < n; ++i) {
      dst_l[i] = static_cast<float>(acc_l[i]) * kScale;
      dst_r[i] = static_cast<float>(acc_r[i]) * kScale;
      acc_l[i] = 0;
      acc_r[i] = 0;
    }
    return;
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t c = acc_c[i];
    dst_l[i] = static_cast<float>(add_wrap(acc_l[i], c)) * kScale;
    dst_r[i] = static_cast<float>(add_wrap(acc_r[i], c)) * kScale;
    acc_l[i] = 0;
    acc_r[i] = 0;
    acc_c[i] = 0;
  }
}

template <class Ops>
O3OS_AUDIO_INLINE void mono_mix_simd(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  std::uint32_t i = 0;
  if (aligned4(dst) && aligned4(l) && aligned4(r)) {
    for (; i + 2u <= n; i += 2u) {
      store_pair(dst + i, Ops::avg_round(load_pair(l + i), load_pair(r + i)));
    }
  }
  for (; i < n; ++i) {
    dst[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(l[i]) + static_cast<std::int32_t>(r[i]) + 1) >> 1);
  }
}

// dst[2 i] = l[i], dst[2 i + 1] = r[i]: two planes as stereo frames, two frames to the word pair (OS-PERF3: the render's one conversion to the TX layout, 2.4k cycles through the general
// usb::interleave, a few hundred here).
template <class Ops>
O3OS_AUDIO_INLINE void interleave2_simd(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  std::uint32_t i = 0;
  if (aligned4(dst) && aligned4(l) && aligned4(r)) {
    for (; i + 2u <= n; i += 2u) {
      const std::uint32_t wl = load_pair(l + i);  // [L(i), L(i+1)]
      const std::uint32_t wr = load_pair(r + i);
      store_pair(dst + 2u * i, Ops::pack_lo(wl, wr));        // frame i:     [L, R]
      store_pair(dst + 2u * i + 2u, Ops::pack_hi(wl, wr));   // frame i + 1
    }
  }
  for (; i < n; ++i) {
    dst[2u * i] = l[i];
    dst[2u * i + 1u] = r[i];
  }
}

template <class Ops>
O3OS_AUDIO_INLINE void deinterleave2_simd(std::int16_t* l, std::int16_t* r, const std::int16_t* src, std::uint32_t n) noexcept {
  std::uint32_t i = 0;
  if (aligned4(l) && aligned4(r) && aligned4(src)) {
    for (; i + 2u <= n; i += 2u) {
      const std::uint32_t w0 = load_pair(src + 2u * i);        // frame i:     [L, R]
      const std::uint32_t w1 = load_pair(src + 2u * i + 2u);   // frame i + 1
      store_pair(l + i, Ops::pack_lo(w0, w1));
      store_pair(r + i, Ops::pack_hi(w0, w1));
    }
  }
  for (; i < n; ++i) {
    l[i] = src[2u * i];
    r[i] = src[2u * i + 1u];
  }
}

template <class Ops>
O3OS_AUDIO_INLINE void meter_simd(const std::int16_t* src, std::uint32_t n, MeterAcc& acc) noexcept {
  std::uint32_t i = 0;
  std::int32_t peak = static_cast<std::int32_t>(acc.peak);
  std::uint64_t sumsq = acc.sumsq;
  if (aligned4(src)) {
    std::uint32_t hi = simd::pack_lanes(-32768, -32768);  // running lane-wise maximum
    std::uint32_t lo = simd::pack_lanes(32767, 32767);    // and minimum
    for (; i + 2u <= n; i += 2u) {
      const std::uint32_t w = load_pair(src + i);
      hi = Ops::max2(hi, w);
      lo = Ops::min2(lo, w);
      sumsq = Ops::smlald(w, w, sumsq);
    }
    if (i != 0u) {  // at least one pair was seen (an empty loop would leave the identities, which are not data)
      const std::int32_t a = simd::lane0(hi) > simd::lane1(hi) ? simd::lane0(hi) : simd::lane1(hi);
      const std::int32_t b = -(simd::lane0(lo) < simd::lane1(lo) ? simd::lane0(lo) : simd::lane1(lo));
      const std::int32_t m = a > b ? a : b;
      peak = m > peak ? m : peak;
    }
  }
  for (; i < n; ++i) {
    const std::int32_t v = src[i];
    const std::int32_t a = v < 0 ? -v : v;
    peak = a > peak ? a : peak;
    sumsq += static_cast<std::uint64_t>(static_cast<std::int64_t>(v) * v);
  }
  acc.peak = static_cast<std::uint32_t>(peak);
  acc.sumsq = sumsq;
}

// An exact conversion: the two lanes of a word to floats, scaled by 2^-15.
template <class Ops>
O3OS_AUDIO_INLINE void q15_to_float_simd(float* dst, const std::int16_t* src, std::uint32_t n) noexcept {
  constexpr float kScale = 1.0f / 32768.0f;
  std::uint32_t i = 0;
  if (aligned4(src)) {
    for (; i + 2u <= n; i += 2u) {
      const std::uint32_t w = load_pair(src + i);
      dst[i] = static_cast<float>(simd::lane0(w)) * kScale;
      dst[i + 1u] = static_cast<float>(simd::lane1(w)) * kScale;
    }
  }
  for (; i < n; ++i) {
    dst[i] = static_cast<float>(src[i]) * kScale;
  }
}

// x * 32768 clamped to [-32768, 32767] (the comparison form: NaN falls to the upper bound, as in q15_from_float), then the integer part of
// (v + 1.5 * 2^23): float addition rounds to nearest even at the unit place, and the mantissa then holds 0x400000 + the rounded value.
//
// OS-PERF3: on the Cortex-M4F the clamps of the portable form are two float compares with a transfer of the flags each (22 cycles a sample, 1.4k a block, four blocks' worth a render);
// the same integer comes from VCVTR (round to nearest even, the FPSCR's mode, saturating to int32 with +-inf at the ends) and SSAT, and a NaN, which VCVTR makes 0, is put back to
// +32767 by its bit pattern (exponent all ones and a mantissa: the doubled bits above 0xFF000000). Clamping after the rounding gives the same value as before it: a value in
// (32767, 32767.5] rounds to 32767 and a larger one saturates to it, below -32768 likewise.
O3OS_AUDIO_INLINE std::int32_t q15_round_even(float x) noexcept {
#if defined(__ARM_FP) && defined(__ARM_FEATURE_SAT)
  float t = x * 32768.0f;
  __asm__("vcvtr.s32.f32 %0, %0" : "+t"(t));
  std::int32_t r;
  std::memcpy(&r, &t, sizeof r);
  std::uint32_t bits;
  std::memcpy(&bits, &x, sizeof bits);
  r = (bits << 1) > 0xFF000000u ? 32767 : r;
  return __ssat(r, 16);
#else
  float v = x * 32768.0f;
  v = v < 32767.0f ? v : 32767.0f;
  v = v > -32768.0f ? v : -32768.0f;
  const float t = v + 12582912.0f;
  std::uint32_t bits;
  std::memcpy(&bits, &t, sizeof bits);
  return static_cast<std::int32_t>(bits - 0x4B400000u);
#endif
}

template <class Ops>
O3OS_AUDIO_INLINE void float_to_q15_simd(std::int16_t* dst, const float* src, std::uint32_t n) noexcept {
  std::uint32_t i = 0;
  if (aligned4(dst)) {
    for (; i + 2u <= n; i += 2u) {
      store_pair(dst + i, Ops::pack(q15_round_even(src[i]), q15_round_even(src[i + 1u])));
    }
  }
  for (; i < n; ++i) {
    dst[i] = static_cast<std::int16_t>(q15_round_even(src[i]));
  }
}

}  // namespace o3os::audio::kernel::detail
