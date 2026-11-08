// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/amiga_convert.h>

#include <cmath>

namespace o3os::app {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPow32 = 4294967296.0f;

// Modified Bessel function of the first kind, order 0: the power series (x/2)^2k / (k!)^2, which converges fast for the window's beta of 8 (k < 40).
float bessel_i0(float x) noexcept {
  const float q = 0.25f * x * x;
  float term = 1.0f;
  float sum = 1.0f;
  for (unsigned k = 1; k < 40u; ++k) {
    term *= q / static_cast<float>(k * k);
    sum += term;
    if (term < sum * 1e-9f) {
      break;
    }
  }
  return sum;
}

}  // namespace

// sin(2 pi r) for any r of the kernel's reach (|r| < 32), without libm (sinf's argument reduction alone is 3.5 KB of flash on the MK66, and nothing else of the image calls it): r is reduced
// to a quarter turn, then a Taylor series to theta^11 (the next term is 6e-8 at the quarter turn, below a float's own rounding of the table). Declared in amiga_convert.h: the Sample edit filter reuses it.
float sin_turns(float r) noexcept {
  r -= static_cast<float>(static_cast<std::int32_t>(r + (r < 0.0f ? -0.5f : 0.5f)));  // the fraction of a turn, -0.5 .. 0.5
  r = r > 0.25f ? 0.5f - r : (r < -0.25f ? -0.5f - r : r);                               // sin(pi - x) = sin(x)
  const float th = 2.0f * kPi * r;
  const float t2 = th * th;
  return th * (1.0f + t2 * (-1.0f / 6.0f + t2 * (1.0f / 120.0f + t2 * (-1.0f / 5040.0f + t2 * (1.0f / 362880.0f + t2 * (-1.0f / 39916800.0f))))));
}

namespace {

// x rounded to the nearest integer, halves away from zero (|x| is a sample level: far inside int32).
std::int32_t iround(float x) noexcept { return static_cast<std::int32_t>(x + (x < 0.0f ? -0.5f : 0.5f)); }

}  // namespace

bool AmigaKernel::build(unsigned n) noexcept {
  const float i0_beta = bessel_i0(kBeta);
  for (; n != 0u && built_ < kSize; --n, ++built_) {
    const float t = static_cast<float>(built_) * (1.0f / static_cast<float>(kRes));  // output frames from the centre
    float sinc = 2.0f * kCutoff;
    if (built_ != 0u) {
      sinc = sin_turns(kCutoff * t) / (kPi * t);
    }
    const float x = t * (1.0f / static_cast<float>(kHalf));
    const float arg = 1.0f - x * x;
    const float window = arg > 0.0f ? bessel_i0(kBeta * std::sqrt(arg)) / i0_beta : 0.0f;
    tab_[built_] = built_ >= kLimit ? 0.0f : sinc * window;  // (the guard entries past the reach are 0)
  }
  return built_ >= kSize;
}

void amiga_resample(const AmigaKernel& kernel, const std::int16_t* src, std::uint32_t frames, std::uint64_t step_q32, bool q8, std::int16_t* dst, std::uint32_t from,
                    std::uint32_t n) noexcept {
  const std::uint32_t total = amiga_frames(frames, step_q32);
  if (from >= total || !kernel.ready()) {
    return;
  }
  n = n > total - from ? total - from : n;
  const float* const tab = kernel.table();
  // source frames per output frame, the Q32.32 step as its two 32-bit halves (a uint64 -> float conversion would call __aeabi_ul2f)
  const float rho = static_cast<float>(static_cast<std::uint32_t>(step_q32 >> 32)) + static_cast<float>(static_cast<std::uint32_t>(step_q32)) * (1.0f / kTwoPow32);
  const float scale = static_cast<float>(AmigaKernel::kRes) / (rho > 1.0f ? rho : 1.0f);  // table steps per source frame
  const float limit = static_cast<float>(AmigaKernel::kLimit);
  const std::int64_t count = static_cast<std::int64_t>(frames);
  for (std::uint32_t k = 0; k < n; ++k) {
    const std::uint64_t pos = static_cast<std::uint64_t>(from + k) * step_q32;  // Q32.32: no drift
    const std::int64_t base = static_cast<std::int64_t>(pos >> 32);
    const float frac = static_cast<float>(static_cast<std::uint32_t>(pos)) * (1.0f / kTwoPow32);
    float acc = 0.0f;
    float wsum = 0.0f;
    // The left taps (the source frames at and before `base`), then the right ones: u is the table position of the distance, the same for both sides.
    float u = frac * scale;
    for (std::int64_t i = base; u < limit; --i, u += scale) {
      const unsigned j = static_cast<unsigned>(u);
      const float f = u - static_cast<float>(j);
      const float w = tab[j] + f * (tab[j + 1] - tab[j]);
      wsum += w;
      if (i >= 0 && i < count) {
        acc += w * static_cast<float>(src[i]);
      }
    }
    u = (1.0f - frac) * scale;
    for (std::int64_t i = base + 1; u < limit; ++i, u += scale) {
      const unsigned j = static_cast<unsigned>(u);
      const float f = u - static_cast<float>(j);
      const float w = tab[j] + f * (tab[j + 1] - tab[j]);
      wsum += w;
      if (i >= 0 && i < count) {
        acc += w * static_cast<float>(src[i]);
      }
    }
    const float y = wsum > 1e-6f ? acc / wsum : 0.0f;
    std::int32_t v;
    if (q8) {
      v = iround(y * (1.0f / 256.0f));
      v = (v > 127 ? 127 : (v < -128 ? -128 : v)) * 256;
    } else {
      v = iround(y);
      v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
    }
    dst[k] = static_cast<std::int16_t>(v);
  }
}

std::uint32_t amiga_frames_per_budget(std::uint64_t step_q32, std::uint32_t budget_us) noexcept {
  const std::uint64_t reach = (step_q32 >> 32) + 1u;                                 // source frames per output frame, rounded up
  const std::uint64_t cycles = 2u * AmigaKernel::kHalf * reach * 14u + 80u;           // a frame
  const std::uint64_t n = static_cast<std::uint64_t>(budget_us) * 180u / cycles;
  return n < 8u ? 8u : static_cast<std::uint32_t>(n > 0x10000u ? 0x10000u : n);
}

}  // namespace o3os::app
