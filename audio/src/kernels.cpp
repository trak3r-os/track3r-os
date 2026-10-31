// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/kernels.h>
#include <o3os/audio/kernels_impl.h>

namespace o3os::audio::kernel {
namespace {

// A lane for a centre kernel: `right` is not read, and is made harmless rather than left to whatever the caller put there.
O3OS_AUDIO_INLINE StripLane centre_lane(const StripLane& a) noexcept {
  StripLane c = a;
  c.right = GainRamp{0, 0};
  return c;
}

}  // namespace

// ---- The references: the specification, in the plainest code ----------------------------------------------------------------

void gain_ref(std::int16_t* dst, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = detail::gain_sample(src[i], ramp_gain20(start, end, i));
  }
}

void mac_ref(std::int32_t* acc, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    acc[i] = detail::add_wrap(acc[i], detail::prod_q19(src[i], ramp_gain20(start, end, i)));
  }
}

void gain_pan_ref(const StripLane& a, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept {
  gain_ref(a.x, a.x, n, a.level.start, a.level.end);
  mac_ref(acc_l, a.x, n, a.left.start, a.left.end);
  mac_ref(acc_r, a.x, n, a.right.start, a.right.end);
}

void gain_mac_ref(const StripLane& a, std::int32_t* acc, std::uint32_t n) noexcept {
  gain_ref(a.x, a.x, n, a.level.start, a.level.end);
  mac_ref(acc, a.x, n, a.left.start, a.left.end);
}

void gain_pan2_ref(const StripLane& a, const StripLane& b, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept {
  gain_pan_ref(a, acc_l, acc_r, n);
  gain_pan_ref(b, acc_l, acc_r, n);
}

void gain_mac2_ref(const StripLane& a, const StripLane& b, std::int32_t* acc, std::uint32_t n) noexcept {
  gain_mac_ref(a, acc, n);
  gain_mac_ref(b, acc, n);
}

void gain_pan_st_ref(const std::int16_t* s, GainRamp level, GainRamp left, GainRamp right, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int16_t y = detail::gain_sample(s[i], ramp_gain20(level.start, level.end, i));
    acc_l[i] = detail::add_wrap(acc_l[i], detail::prod_q19(y, ramp_gain20(left.start, left.end, i)));
    acc_r[i] = detail::add_wrap(acc_r[i], detail::prod_q19(y, -ramp_gain20(right.start, right.end, i)));
  }
}

void sat_clear_ref(std::int16_t* dst, std::int32_t* acc, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = detail::q19_to_q15(acc[i]);
    acc[i] = 0;
  }
}

void sat_clear_main_ref(std::int16_t* dst_l, std::int16_t* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t c = acc_c != nullptr ? acc_c[i] : 0;
    dst_l[i] = detail::q19_to_q15(detail::add_wrap(acc_l[i], c));
    dst_r[i] = detail::q19_to_q15(detail::add_wrap(acc_r[i], c));
    acc_l[i] = 0;
    acc_r[i] = 0;
    if (acc_c != nullptr) {
      acc_c[i] = 0;
    }
  }
}

void acc_to_float_main_ref(float* dst_l, float* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t c = acc_c != nullptr ? acc_c[i] : 0;
    dst_l[i] = static_cast<float>(detail::add_wrap(acc_l[i], c)) / 524288.0f;
    dst_r[i] = static_cast<float>(detail::add_wrap(acc_r[i], c)) / 524288.0f;
    acc_l[i] = 0;
    acc_r[i] = 0;
    if (acc_c != nullptr) {
      acc_c[i] = 0;
    }
  }
}

void mono_mix_ref(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(l[i]) + static_cast<std::int32_t>(r[i]) + 1) >> 1);
  }
}

void deinterleave2_ref(std::int16_t* l, std::int16_t* r, const std::int16_t* src, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    l[i] = src[2u * i];
    r[i] = src[2u * i + 1u];
  }
}

void interleave2_ref(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[2u * i] = l[i];
    dst[2u * i + 1u] = r[i];
  }
}

void meter_ref(const std::int16_t* src, std::uint32_t n, MeterAcc& acc) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t v = src[i];
    const std::uint32_t a = static_cast<std::uint32_t>(v < 0 ? -v : v);
    acc.peak = a > acc.peak ? a : acc.peak;
    acc.sumsq += static_cast<std::uint64_t>(static_cast<std::int64_t>(v) * v);
  }
}

void q15_to_float_ref(float* dst, const std::int16_t* src, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = static_cast<float>(src[i]) / 32768.0f;
  }
}

void float_to_q15_ref(std::int16_t* dst, const float* src, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = static_cast<std::int16_t>(q15_from_float(src[i]));
  }
}

// ---- Production: the paired loops on the native instructions (the model of them where there are none) ---------------------

void gain(std::int16_t* dst, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept {
  detail::gain_simd<simd::NativeOps>(dst, src, n, start, end);
}
void mac(std::int32_t* acc, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept {
  detail::mac_simd<simd::NativeOps>(acc, src, n, start, end);
}
void gain_pan(const StripLane& a, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept {
  const StripLane lanes[1] = {a};
  detail::strip_simd<simd::NativeOps, 1, true>(lanes, acc_l, acc_r, n);
}
void gain_mac(const StripLane& a, std::int32_t* acc, std::uint32_t n) noexcept {
  const StripLane lanes[1] = {centre_lane(a)};
  detail::strip_simd<simd::NativeOps, 1, false>(lanes, acc, nullptr, n);
}
void gain_pan2(const StripLane& a, const StripLane& b, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept {
  const StripLane lanes[2] = {a, b};
  detail::strip_simd<simd::NativeOps, 2, true>(lanes, acc_l, acc_r, n);
}
void gain_mac2(const StripLane& a, const StripLane& b, std::int32_t* acc, std::uint32_t n) noexcept {
  const StripLane lanes[2] = {centre_lane(a), centre_lane(b)};
  detail::strip_simd<simd::NativeOps, 2, false>(lanes, acc, nullptr, n);
}
// OS-STEREO (c), (f): the side signal's strip pass, the paired loop of gain_pan's (level, then the two pan products), the right one subtracted (the gain is negated after the ramp: Gen holds the ramp unsigned), in flash.
// Slice (f): this loop is the ramped block's (its Gen<false> costs six shifts and six adds a pair and the registers that spilled it: 44 instructions a pair); the steady block (all three gains constant, the
// common one) has the loop below (21 instructions a pair).
O3OS_AUDIO_INLINE void gain_pan_st_pairs(const std::int16_t* s, GainRamp level, GainRamp left, GainRamp right, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n, std::uint32_t& i) noexcept {
  detail::Gen<false> gv;
  detail::Gen<false> gl;
  detail::Gen<false> gr;
  gv.init(level);
  gl.init(left);
  gr.init(right);
  for (; i + 2u <= n; i += 2u) {
    const std::int32_t v0 = gv.next();  // (in order: the ramp is a sequence)
    const std::int32_t v1 = gv.next();
    const std::uint32_t y = detail::level_pair<simd::NativeOps>(v0, v1, detail::load_pair(s + i));
    acc_l[i] = simd::NativeOps::smlawb(gl.next(), y, acc_l[i]);
    acc_l[i + 1u] = simd::NativeOps::smlawt(gl.next(), y, acc_l[i + 1u]);
    acc_r[i] = simd::NativeOps::smlawb(-gr.next(), y, acc_r[i]);
    acc_r[i + 1u] = simd::NativeOps::smlawt(-gr.next(), y, acc_r[i + 1u]);
  }
}
// The steady loop reads and writes each accumulator pair as one doubleword (LDRD / STRD: three cycles for two words where two LDR and two STR cost four each with the products between them); the pair
// is at an even index of an array of 8-byte alignment (the caller's test: LDRD / STRD need a word, and the pair is two adjacent words, so a doubleword access is exactly what two word accesses are).
O3OS_AUDIO_INLINE void load2(const std::int32_t* p, std::int32_t& a, std::int32_t& b) noexcept {
#if defined(__arm__)
  __asm__("ldrd %0, %1, [%2]" : "=&r"(a), "=&r"(b) : "r"(p), "m"(*reinterpret_cast<const std::int32_t(*)[2]>(p)));
#else
  a = p[0];
  b = p[1];
#endif
}
O3OS_AUDIO_INLINE void store2(std::int32_t* p, std::int32_t a, std::int32_t b) noexcept {
#if defined(__arm__)
  __asm__("strd %1, %2, [%3]" : "=m"(*reinterpret_cast<std::int32_t(*)[2]>(p)) : "r"(a), "r"(b), "r"(p));
#else
  p[0] = a;
  p[1] = b;
#endif
}
// A function of its own with few values: inlined into gain_pan_st (which also holds the ramped loop and the tails) the allocator spilled the pointers and reloaded them every pair. The right accumulator is
// the left one's kBlock words on (MAIN's two accumulators are one array of two rows: the caller's test), so one pointer walks both and the right pair is an immediate offset of the same LDRD / STRD.
[[gnu::noinline]] void gain_pan_st_steady(std::int32_t* pl, const std::int16_t* sp, std::uint32_t pairs, std::int32_t gv, std::int32_t gl, std::int32_t gr) noexcept {
  constexpr std::uint32_t kRow = hal::audio::kBlock;
  for (std::uint32_t k = 0; k < pairs; ++k) {
    const std::uint32_t y = detail::level_pair<simd::NativeOps>(gv, gv, detail::load_pair(sp));
    std::int32_t l0;
    std::int32_t l1;
    std::int32_t r0;
    std::int32_t r1;
    load2(pl, l0, l1);
    load2(pl + kRow, r0, r1);
    l0 = simd::NativeOps::smlawb(gl, y, l0);
    l1 = simd::NativeOps::smlawt(gl, y, l1);
    r0 = simd::NativeOps::smlawb(gr, y, r0);
    r1 = simd::NativeOps::smlawt(gr, y, r1);
    store2(pl, l0, l1);
    store2(pl + kRow, r0, r1);
    sp += 2;
    pl += 2;
  }
}
void gain_pan_st(const std::int16_t* s, GainRamp level, GainRamp left, GainRamp right, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept {
  std::uint32_t i = 0;
  const bool rows = (reinterpret_cast<std::uintptr_t>(acc_l) & 7u) == 0u && acc_r == acc_l + hal::audio::kBlock && n <= hal::audio::kBlock;
  if (detail::aligned4(s)) {
    if (rows && detail::steady(level) && detail::steady(left) && detail::steady(right)) {
      const std::uint32_t pairs = n / 2u;
      gain_pan_st_steady(acc_l, s, pairs, 16 * level.start, 16 * left.start, -(16 * right.start));
      i = 2u * pairs;
    } else {
      gain_pan_st_pairs(s, level, left, right, acc_l, acc_r, n, i);
    }
  }
  for (; i < n; ++i) {
    const std::int16_t y = detail::gain_sample(s[i], ramp_gain20(level.start, level.end, i));
    acc_l[i] = detail::add_wrap(acc_l[i], detail::prod_q19(y, ramp_gain20(left.start, left.end, i)));
    acc_r[i] = detail::add_wrap(acc_r[i], detail::prod_q19(y, -ramp_gain20(right.start, right.end, i)));
  }
}
void sat_clear(std::int16_t* dst, std::int32_t* acc, std::uint32_t n) noexcept { detail::sat_clear_simd<simd::NativeOps>(dst, acc, n); }
void sat_clear_main(std::int16_t* dst_l, std::int16_t* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c, std::uint32_t n) noexcept {
  detail::sat_clear_main_simd<simd::NativeOps>(dst_l, dst_r, acc_l, acc_r, acc_c, n);
}
void acc_to_float_main(float* dst_l, float* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c, std::uint32_t n) noexcept {
  detail::acc_to_float_main_simd<simd::NativeOps>(dst_l, dst_r, acc_l, acc_r, acc_c, n);
}
void mono_mix(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  detail::mono_mix_simd<simd::NativeOps>(dst, l, r, n);
}
void deinterleave2(std::int16_t* l, std::int16_t* r, const std::int16_t* src, std::uint32_t n) noexcept {
  detail::deinterleave2_simd<simd::NativeOps>(l, r, src, n);
}
void interleave2(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept {
  detail::interleave2_simd<simd::NativeOps>(dst, l, r, n);
}
void meter(const std::int16_t* src, std::uint32_t n, MeterAcc& acc) noexcept { detail::meter_simd<simd::NativeOps>(src, n, acc); }
void q15_to_float(float* dst, const std::int16_t* src, std::uint32_t n) noexcept { detail::q15_to_float_simd<simd::NativeOps>(dst, src, n); }
void float_to_q15(std::int16_t* dst, const float* src, std::uint32_t n) noexcept { detail::float_to_q15_simd<simd::NativeOps>(dst, src, n); }

}  // namespace o3os::audio::kernel
