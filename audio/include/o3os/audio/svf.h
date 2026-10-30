// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The instrument filter (OS-007l; voices.h FILTER): a per-track state variable filter, in single-precision float.
//
// BEHAVIOUR (a Chamberlin state variable filter run at twice the sample rate, the structure of the Teensy Audio library's
// state variable filter, musicdsp.org archive 92, MIT, (c) 2014 Paul Stoffregen). One filter a track, between the track's
// player (which has already applied the amp envelope and the gain) and its amplifier. A disabled filter copies its input.
//   per sample, x the input, f = 2 sin(pi fc / (2 fs)), d = 1 / Q:
//     lp += f bp;  hp = (x + prev) / 2 - lp - d bp;  prev = x;  bp += f hp;        // first half step, on the input interpolated
//     lp0 = lp, bp0 = bp, hp0 = hp;
//     lp += f bp;  hp = x - lp - d bp;  bp += f hp;                                  // second half step
//     out = (lp + lp0) / 2, (hp + hp0) / 2 or (bp + bp0) / 2, saturated to 16 bits   // type 0 low-pass, 1 high-pass, 2 band-pass
// CUTOFF (this filter's law): c in 0..1 -> step i = floor(c x 477) -> fc = 20 Hz x 2^(i / 50) (478 steps of 1/50 octave, 20 .. 14,888.68 Hz); fs is
// 44,117.647 Hz. RESONANCE: the instrument stores r in 0..4.3 and the filter uses Q = r + 0.7,
// held to 0.7..5 (resonance()). Whether the official firmware's filter has the same law is an open research question (Q). The coefficients are not recomputed per sample; here they are set per
// note and per Cutoff FX event.
//
// WHAT DIFFERS: float instead of Q31 (the output differs from the Q31 oracle's by at most one rounding LSB: the oracle test holds it to 2, tests/audio_svf_test.cpp); the output
// is rounded to nearest where the oracle floors it (a half LSB of DC); sin() is a polynomial (below), not sinf, so the render path has no libm and no flash call;
// the cutoff table is 50 entries and a shift (a cutoff index that is an exact multiple of 1/477 may land one step off in float, 1.4 % of a cutoff).
//
// Real time: no heap, no libm, no division in the sample loop, bounded (n samples); the only division is `1 / Q` in svf_set().

#include <o3os/audio/hot.h>
#include <o3os/core/audio_clock.h>

#include <cstdint>
#include <cstring>

#if defined(__ARM_FP) && defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#endif

namespace o3os::audio {

inline constexpr std::uint8_t kFilterLow = 0;   // the .pti's filter type
inline constexpr std::uint8_t kFilterHigh = 1;
inline constexpr std::uint8_t kFilterBand = 2;

inline constexpr float kFilterResonanceOffset = 0.7f;  // Q = stored resonance + this 
inline constexpr float kFilterMaxQ = 5.0f;

// pi x fc / (2 fs) for fc = 20 Hz x 2^(k / 50), k = 0..49, fs = 44117.64706 Hz; the cutoff index i has w = kSvfW[i % 50] x 2^(i / 50). Computed to 9 digits.
// kSvfRateScale = 44117.64706 / (the engine's rate) moves the table to the build's rate (w goes as 1 / fs; the scale is 1.0f exactly at 4080/1).
// The scaled table kSvfW is built at compile time: one table in flash, no multiply at run time.
inline constexpr float kSvfRateScale = 44117.64706f / core::kRefRateHz;
inline constexpr float kSvfW44117[50] = {
    0.000712094335f, 0.000722034801f, 0.000732114031f, 0.000742333963f, 0.000752696558f, 0.000763203811f, 0.000773857739f, 0.00078466039f,  0.000795613841f,
    0.000806720197f, 0.000817981591f, 0.000829400188f, 0.000840978184f, 0.000852717802f, 0.000864621299f, 0.000876690962f, 0.000888929112f, 0.0009013381f,
    0.000913920311f, 0.000926678163f, 0.000939614108f, 0.000952730632f, 0.000966030256f, 0.000979515536f, 0.000993189063f, 0.00100705347f,  0.00102111141f,
    0.00103536559f,  0.00104981876f,  0.00106447368f,  0.00107933318f,  0.00109440011f,  0.00110967737f,  0.00112516788f,  0.00114087464f,  0.00115680066f,
    0.001172949f,    0.00118932275f,  0.00120592508f,  0.00122275917f,  0.00123982825f,  0.00125713561f,  0.00127468457f,  0.0012924785f,   0.00131052083f,
    0.00132881501f,  0.00134736458f,  0.00136617309f,  0.00138524416f,  0.00140458144f};

struct SvfWTable {
  float v[50];
  constexpr SvfWTable() noexcept : v{} {
    for (unsigned k = 0; k < 50u; ++k) v[k] = kSvfW44117[k] * kSvfRateScale;
  }
};
inline constexpr SvfWTable kSvfW{};

// One voice's filter: the coefficients and the three state words (the previous input, the low-pass and the band-pass).
struct Svf {
  float f = 0.0f;  // 2 sin(w)
  float d = 0.0f;  // 1 / Q
  float lp = 0.0f;
  float bp = 0.0f;
  float prev = 0.0f;
  std::uint32_t idx = 0xFFFFFFFFu;  // the cutoff table index `f` was made for (OS-CPUOPT1: f depends on it alone, so a cutoff that stays in the same index keeps its coefficient); none yet
};

O3OS_AUDIO_INLINE void svf_reset(Svf& s) noexcept {
  s.lp = 0.0f;
  s.bp = 0.0f;
  s.prev = 0.0f;
}

// The frequency coefficient for a cutoff 0..1 (held to range; NaN counts as the low end). Alone it is what a modulated cutoff costs (OS-007m): Q does not move.
O3OS_AUDIO_INLINE void svf_set_cutoff(Svf& s, float cutoff) noexcept {
  const float c = cutoff > 1.0f ? 1.0f : (cutoff >= 0.0f ? cutoff : 0.0f);
  const std::uint32_t i = static_cast<std::uint32_t>(c * 477.0f);  // 0..477, truncated
  if (i == s.idx) {
    return;  // the same table index: the same coefficient (w and f below are functions of i alone)
  }
  s.idx = i;
  const std::uint32_t octave = i / 50u;                            // 0..9
  const float w = kSvfW.v[i - octave * 50u] * static_cast<float>(1u << octave);  // pi fc / (2 fs), at most 0.5301
  const float w2 = w * w;
  // sin(w) to w^7: the next term, w^9 / 9!, is under 1e-8 at the top of the range.
  s.f = 2.0f * (w * (1.0f + w2 * (-0.166666672f + w2 * (0.00833333377f + w2 * -0.000198412701f))));
}

// The coefficients for a cutoff 0..1 and a stored resonance 0..4.3 (both held to range; NaN counts as the low end). The state is left as it is.
O3OS_AUDIO_INLINE void svf_set(Svf& s, float cutoff, float resonance) noexcept {
  svf_set_cutoff(s, cutoff);
  const float r = resonance >= 0.0f ? (resonance > 4.3f ? 4.3f : resonance) : 0.0f;
  const float q = r + kFilterResonanceOffset;
  s.d = 1.0f / (q > kFilterMaxQ ? kFilterMaxQ : q);
}

// A float in sample units to int16: nearest (ties to even), then saturated. On the Cortex-M4F that is VCVTR (the FPSCR's rounding mode, nearest-even, saturating to int32) and SSAT, three
// instructions (OS-PERF3; it was the magic-number sum of 1.5 x 2^23 with a compare pair, seven). The two give the same integer for every finite v: the sum rounds to nearest-even the same
// way and is exact for |v| < 2^22; a larger value, which a stable filter does not make, saturates in both. Elsewhere (host, QEMU's model of it) the sum.
O3OS_AUDIO_INLINE std::int16_t svf_out(float v) noexcept {
#if defined(__ARM_FP) && defined(__ARM_FEATURE_SAT)
  float t;
  __asm__("vcvtr.s32.f32 %0, %1" : "=t"(t) : "t"(v));
  std::int32_t i;
  std::memcpy(&i, &t, sizeof i);
  return static_cast<std::int16_t>(__ssat(i, 16));
#else
  const float t = v + 12582912.0f;
  std::int32_t i;
  std::memcpy(&i, &t, sizeof i);
  i -= 0x4B400000;
  return static_cast<std::int16_t>(i > 32767 ? 32767 : (i < -32768 ? -32768 : i));
#endif
}

// A product the compiler must keep as a rounded value of its own (OS-PERF-HPBP): the empty asm stops GCC from folding a multiply and the add or subtract after it into VMLA / VMLS / VNMLS, which
// take 3 cycles; the separate VMUL and VADD take 1 + 1, and a product that does not depend on the step before issues ahead of it. The result is bit for bit the same: the M4's VMLA is not a fused
// multiply-add (it rounds the product first), so the operations and their order are those of the written expression. Elsewhere it is the identity.
O3OS_AUDIO_INLINE float svf_product(float p) noexcept {
#if defined(__ARM_FP)
  __asm__("" : "+t"(p));
#endif
  return p;
}

// n samples of `io` through the filter, in place. Type is kFilterLow, kFilterHigh or kFilterBand. The arithmetic is the BEHAVIOUR text above, operation for operation (the same expressions,
// left to right, no fused multiply-add: the library builds with -ffp-contract=off), in single-assignment form so that no register copy is needed; the loop is a do-while (the n == 0 case first).
template <std::uint8_t Type>
O3OS_AUDIO_INLINE void svf_run(Svf& s, std::int16_t* io, std::uint32_t n) noexcept {
  const float f = s.f;
  const float d = s.d;
  float lp = s.lp;
  float bp = s.bp;
  float prev = s.prev;
  if (n != 0u) {
    std::uint32_t k = 0;
    do {
      const float x = static_cast<float>(io[k]);
      const float lp0 = lp + svf_product(f * bp);                         // lp += f bp
      const float hp0 = svf_product(0.5f * (x + prev)) - lp0 - svf_product(d * bp);  // hp = (x + prev) / 2 - lp - d bp
      prev = x;
      const float bp0 = bp + svf_product(f * hp0);                        // bp += f hp
      const float lp1 = lp0 + svf_product(f * bp0);                       // the second half step
      const float hp1 = x - lp1 - svf_product(d * bp0);
      const float bp1 = bp0 + svf_product(f * hp1);
      const float y = Type == kFilterLow ? lp1 + lp0 : (Type == kFilterHigh ? hp1 + hp0 : bp1 + bp0);
      io[k] = svf_out(0.5f * y);
      lp = lp1;
      bp = bp1;
    } while (++k < n);
  }
  s.lp = lp;
  s.bp = bp;
  s.prev = prev;
}

}  // namespace o3os::audio
