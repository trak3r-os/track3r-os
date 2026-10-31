// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The sample kernels of the fixed graph (OS-005b, ADR 0013 D2): everything the render does to a block of samples, as plain functions
// over int16 (Q15) planes and int32 accumulators. Each has a SCALAR REFERENCE (`*_ref`, the specification: what the block must become,
// in the plainest code, not on the real-time path) and a production variant (no suffix) that works on two samples per 32-bit word with
// the Cortex-M4's DSP instructions (simd.h): SMLAWB/SMLAWT for the gain and the mix-accumulate, SMLALD for the energy, SSUB16 + SEL for
// the peak, SHSUB16 + SSUB16 for the mono average, PKHBT/PKHTB to pack, SSAT (with its free shift) to round and saturate. The integer kernels are
// bit-identical to their references (tests/audio_kernels_test.cpp checks them at random lengths, alignments, gains and extreme samples, on the host
// with the instructions modelled, on QEMU with the real ones, and the two against each other); the float conversion is equal except on exact .5
// ties, where it differs by one LSB (stated at the kernel).
//
// FORMATS. A sample is Q15 (int16). A GAIN is Q16: 65536 is unity (0 dB); gains lie in [0, kMaxGain] (about +24 dB: the 1.2+ level law, as the release
// notes document it (PAR-023, not verified), reaches +24 dB, and a stack of two such faders is clamped to kMaxGain by the caller). Inside a kernel the gain is applied with four
// more fractional bits (Q20, `kGainBits`) and a product keeps them: an ACCUMULATOR is Q19 (a Q15 sample is 16 units), so a source's
// truncation error is 1/16 LSB, not 1 LSB. Every step that goes back to int16 ROUNDS (half up) and SATURATES in one instruction
// (SSAT ... ASR #4 after the rounding constant kAccRound): `gain` does it for the post-fader block, `sat_clear` and the float conversion at a
// bus's output. There is no floor anywhere: a floor of a block of tracks is a DC offset (about -0.5 LSB a source, -6 LSB for twelve), which this
// design does not have. Headroom: a source adds at most 2^19 x (gain / unity) to an accumulator, so 17 sources at +24 dB (2^23 each) fit int32.
//
// RAMPS. A gain that changes does so linearly across the block, per sample, never in a step (no zipper noise): the kernels take the gain at the
// block's start and at its end and sample i (0-based) is multiplied by
//     ramp_gain20(start, end, i) = 16 * start + ((16 * (end - start) * (i + 1)) >> kRampShift)       (arithmetic shift: floor)
// so the last sample of a block of kBlock frames has exactly 16 * end, the first is within one step of 16 * start, and one step is at most
// ceil(16 * |end - start| / kBlock) Q20, i.e. |end - start| / kBlock Q16 (1,024 for a full-scale change of unity: 1.6 % of unity per sample).
// start == end is the constant-gain case (no per-sample arithmetic). `n` is at most kBlock.
//
// Alignment: a pointer to int16 data is 4-byte aligned for the paired path (every graph buffer is declared alignas); a kernel handed an
// odd-aligned pointer (the tests do) takes the scalar loop for the whole call, never an unaligned word access (ADR 0011 lesson 1).
// `dst` may equal `src` (in place) but must not otherwise overlap.
// Real-time rules: no heap, no libm, no call out of the section (hot.h), every loop bounded by `n`.

#include <o3os/audio/hot.h>
#include <o3os/hal/audio.h>

#include <cstdint>

namespace o3os::audio::kernel {

inline constexpr std::int32_t kUnity = 65536;  // Q16 gain of 1.0
// The largest gain a kernel is specified for: 16x, +24.08 dB (the Og12 level law tops out at 15.85x). 16 * kMaxGain * kBlock must fit
// int32 for kBlock up to 128, which it does by one part in 2^20: the ramp state is held in unsigned words so that its last step cannot overflow.
inline constexpr std::int32_t kMaxGain = (1 << 20) - 1;
inline constexpr unsigned kGainBits = 4;            // the multiplier's extra fractional bits: Q20
inline constexpr std::int32_t kAccRound = 8;        // half a Q15 LSB in the Q19 accumulator: the rounding constant of every step back to int16

constexpr unsigned log2_of(std::uint32_t v) noexcept {
  unsigned r = 0;
  while ((1u << r) < v) {
    ++r;
  }
  return r;
}
static_assert(hal::audio::is_pow2(hal::audio::kBlock) && hal::audio::kBlock <= 128, "the ramp divides by the block length with a shift, in 32 bits");
static_assert(16 * static_cast<std::int64_t>(kMaxGain) * hal::audio::kBlock < (static_cast<std::int64_t>(1) << 31), "the ramp state fits int32");
inline constexpr unsigned kRampShift = log2_of(hal::audio::kBlock);

// The gain of sample `i` of a ramp, in Q20 (see above). The definition every variant follows.
O3OS_AUDIO_INLINE constexpr std::int32_t ramp_gain20(std::int32_t start, std::int32_t end, std::uint32_t i) noexcept {
  return 16 * start + ((16 * (end - start) * static_cast<std::int32_t>(i + 1u)) >> kRampShift);
}

// The two gains a ramp takes: the gain at the start of the block and at its end (see above).
struct GainRamp {
  std::int32_t start;
  std::int32_t end;
};

// One track's share of a strip pass: its block (in place: the level is applied to it), the fader's ramp and the pan law's two gains. For the
// centre kernels `left` is the one gain into the single accumulator and `right` is not read.
struct StripLane {
  std::int16_t* x;
  GainRamp level;
  GainRamp left;
  GainRamp right;
};

// One channel's accumulators over a metering window: the largest |sample| (|-32768| is 32768) and the sum of squares.
struct MeterAcc {
  std::uint32_t peak;
  std::uint64_t sumsq;
};

// ---- gain ------------------------------------------------------------------------------------------------------------------
// dst[i] = saturate_int16(round(src[i] * g(i) / 65536)): the product at Q20, rounded half up to Q15 with the saturation SSAT gives for free.
void gain_ref(std::int16_t* dst, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept;
O3OS_AUDIO_HOT void gain(std::int16_t* dst, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept;

// ---- mix-accumulate ---------------------------------------------------------------------------------------------------------
// acc[i] += floor(src[i] * g20(i) / 65536), the Q19 product, modulo 2^32 (never reached: see the headroom above).
void mac_ref(std::int32_t* acc, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept;
O3OS_AUDIO_HOT void mac(std::int32_t* acc, const std::int16_t* src, std::uint32_t n, std::int32_t start, std::int32_t end) noexcept;

// ---- the strip pass: level and pan in one go -----------------------------------------------------------------------------
// The track strip's hot path: the fader and the pan law with one load of each sample instead of three, and (the two-track forms) one load and
// one store of each accumulator for two tracks instead of one each. In place on x, for each lane:
//     x[i] = gain(x[i], level)
//     acc_l[i] += mac of the NEW x[i] with `left`        (stereo forms: acc_r[i] likewise with `right`)
// which is exactly gain_ref then mac_ref (twice, for stereo), lane after lane; the integer sums commute, so the two-track forms equal the single
// ones in either order. A closed side is a ramp {0, 0} and adds nothing. The CENTRE forms (gain_mac, gain_mac2) add the one product into a single
// accumulator, for a track whose two pan gains are equal (the centre of the pan law: both unity): the bus adds that accumulator to both sides
// when it is saturated (sat_clear_main), so a centred track costs one multiply-accumulate, not two.
void gain_pan_ref(const StripLane& a, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept;
void gain_mac_ref(const StripLane& a, std::int32_t* acc, std::uint32_t n) noexcept;
void gain_pan2_ref(const StripLane& a, const StripLane& b, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept;
void gain_mac2_ref(const StripLane& a, const StripLane& b, std::int32_t* acc, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void gain_pan(const StripLane& a, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void gain_mac(const StripLane& a, std::int32_t* acc, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void gain_pan2(const StripLane& a, const StripLane& b, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void gain_mac2(const StripLane& a, const StripLane& b, std::int32_t* acc, std::uint32_t n) noexcept;

// ---- sum and saturate -------------------------------------------------------------------------------------------------------
// dst[i] = saturate_int16((acc[i] + kAccRound) >> 4), the Q19 sum rounded half up; acc[i] = 0 (the bus is empty again for the next block).
void sat_clear_ref(std::int16_t* dst, std::int32_t* acc, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void sat_clear(std::int16_t* dst, std::int32_t* acc, std::uint32_t n) noexcept;
// The MAIN bus: the two sides, each with the centre accumulator added (acc_c may be null: no centred track this block), all three emptied.
void sat_clear_main_ref(std::int16_t* dst_l, std::int16_t* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void sat_clear_main(std::int16_t* dst_l, std::int16_t* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c,
                                   std::uint32_t n) noexcept;
// The same bus handed to the master chain without saturating: dst[i] = (acc[i] + acc_c[i]) / 2^19 as float (+-1.0 = full scale, beyond it where the
// sum is beyond it: the chain's headroom stage and its final float_to_q15 are the only things that saturate, ADR 0013 D2), accumulators emptied.
void acc_to_float_main_ref(float* dst_l, float* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void acc_to_float_main(float* dst_l, float* dst_r, std::int32_t* acc_l, std::int32_t* acc_r, std::int32_t* acc_c, std::uint32_t n) noexcept;

// ---- the side signal of a stereo track (OS-STEREO (c)) -----------------------------------------------------------------------
// One track's S through its strip into MAIN: y[i] = gain(s[i], level) (the post-fader S, rounded to Q15 as gain_ref rounds the track's M, so S follows the strip's level ramp), then
//     acc_l[i] += Q19 product of y[i] with `left`        (L = M + S)
//     acc_r[i] -= Q19 product of y[i] with `right`       (R = M - S; the balance law's two gains: the louder side stays at unity)
// `s` is only read. A closed side is a ramp {0, 0}. With S = 0 nothing is added: a stereo voice with no side signal is the mono render bit for bit. The right side's product is that of the NEGATED gain
// (floor(-y g / 2^16)). gain_pan_st_ref is the specification (scalar), gain_pan_st the paired loop on the DSP instructions, bit for bit; the production one is in flash (O3OS_AUDIO_COLD: one call per track that
// has S in the block, hot.h), tests/audio_stereo_test.cpp holds the two to each other. OS-STEREO (f): the steady block (the three gains constant) takes a loop of its own, LDRD / STRD on the accumulators, when
// acc_r is acc_l + kBlock and acc_l is 8-byte aligned (MAIN's two accumulators are one array of two rows); any other call takes the ramped loop (the same numbers, slower).
void gain_pan_st_ref(const std::int16_t* s, GainRamp level, GainRamp left, GainRamp right, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept;
O3OS_AUDIO_COLD void gain_pan_st(const std::int16_t* s, GainRamp level, GainRamp left, GainRamp right, std::int32_t* acc_l, std::int32_t* acc_r, std::uint32_t n) noexcept;

// ---- mono sum ---------------------------------------------------------------------------------------------------------------
// dst[i] = (l[i] + r[i] + 1) >> 1: the mono mix of a stereo pair (a send of the input, the spectrum tap, OUT MODE, a mono take), rounded half up.
void mono_mix_ref(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void mono_mix(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept;

// ---- deinterleave ---------------------------------------------------------------------------------------------------------
// l[i] = src[2 i], r[i] = src[2 i + 1] for i < n: the RX block (interleaved stereo frames, left first) as two planes. The TX side is
// usb::interleave (the OS-006a kernel); the receive side is here because that kernel's deinterleave takes its input as words, and a
// block of int16 is not accessed as words through a cast (strict aliasing).
void deinterleave2_ref(std::int16_t* l, std::int16_t* r, const std::int16_t* src, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void deinterleave2(std::int16_t* l, std::int16_t* r, const std::int16_t* src, std::uint32_t n) noexcept;

// dst[2 i] = l[i], dst[2 i + 1] = r[i] for i < n: the output planes as the TX block (interleaved stereo frames, left first). OS-PERF3: the general usb::interleave does the same through a
// function pointer in flash for any number of channels; the render's one conversion uses this.
void interleave2_ref(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void interleave2(std::int16_t* dst, const std::int16_t* l, const std::int16_t* r, std::uint32_t n) noexcept;

// ---- meter ------------------------------------------------------------------------------------------------------------------
// acc.peak = max(acc.peak, max |src[i]|); acc.sumsq += sum src[i]^2.
void meter_ref(const std::int16_t* src, std::uint32_t n, MeterAcc& acc) noexcept;
O3OS_AUDIO_HOT void meter(const std::int16_t* src, std::uint32_t n, MeterAcc& acc) noexcept;

// ---- Q15 <-> float (the D2 conversion points) -------------------------------------------------------------------------------
// dst[i] = src[i] / 32768 as float. Exact (an int16 and a power of two): the two variants are bit-identical.
void q15_to_float_ref(float* dst, const std::int16_t* src, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void q15_to_float(float* dst, const std::int16_t* src, std::uint32_t n) noexcept;
// dst[i] = round(src[i] * 32768) saturated to int16 (o3os::audio::q15_from_float: half away from zero, NaN to +32767). The production
// variant adds 1.5 * 2^23 and reads the integer out of the mantissa (round to nearest EVEN, no branch per sample): it differs from the
// reference by one LSB on an exact .5 tie and nowhere else; NaN gives +32767 in both.
void float_to_q15_ref(std::int16_t* dst, const float* src, std::uint32_t n) noexcept;
O3OS_AUDIO_HOT void float_to_q15(std::int16_t* dst, const float* src, std::uint32_t n) noexcept;

}  // namespace o3os::audio::kernel
