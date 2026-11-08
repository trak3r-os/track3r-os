// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/core/ptfx_math.h>

#include <o3os/core/fast_math.h>

#include <cstring>

namespace o3os::core::ptfx_math {

namespace {

constexpr float kLog2e = 1.44269504089f;
constexpr float kLn2 = 0.693147180560f;
constexpr float kLog10of2 = 0.301029995664f;

std::uint32_t bits_of(float x) noexcept {
  std::uint32_t b;
  std::memcpy(&b, &x, sizeof(b));
  return b;
}
float from_bits(std::uint32_t b) noexcept {
  float x;
  std::memcpy(&x, &b, sizeof(x));
  return x;
}
bool is_nan(float x) noexcept { return x != x; }
bool is_inf(float x) noexcept { return (bits_of(x) & 0x7FFFFFFFu) == 0x7F800000u; }
bool neg(float x) noexcept { return (bits_of(x) >> 31) != 0u; }
float inf_of(bool negative) noexcept { return from_bits(negative ? 0xFF800000u : 0x7F800000u); }
float nan_value() noexcept { return from_bits(0x7FC00000u); }

// 0: not an integer, 1: an even integer, 2: an odd integer.
int integer_kind(float y) noexcept {
  const float ay = from_bits(bits_of(y) & 0x7FFFFFFFu);
  if (!(ay < 8388608.0f)) {
    return is_inf(y) || is_nan(y) ? 0 : 1;  // every float of 2^23 and up is an even integer
  }
  const std::int32_t k = static_cast<std::int32_t>(ay);
  if (static_cast<float>(k) != ay) {
    return 0;
  }
  return (k & 1) != 0 ? 2 : 1;
}

std::uint64_t g_rand_state = 1u;

}  // namespace

float fabsf_(float x) noexcept { return from_bits(bits_of(x) & 0x7FFFFFFFu); }
float fminf_(float a, float b) noexcept {
  if (is_nan(a)) {
    return b;
  }
  if (is_nan(b)) {
    return a;
  }
  return b < a ? b : a;
}
float fmaxf_(float a, float b) noexcept {
  if (is_nan(a)) {
    return b;
  }
  if (is_nan(b)) {
    return a;
  }
  return b > a ? b : a;
}

float tanf_(float x) noexcept {
  if (!(x > -1.0e4f && x < 1.0e4f)) {
    return 0.0f;
  }
  const float s = core::sin_fast(x);
  const float c = core::cos_fast(x);
  if (c == 0.0f) {
    return s < 0.0f ? -1.0e30f : 1.0e30f;
  }
  return s / c;
}

float expf_(float x) noexcept {
  if (is_nan(x)) {
    return x;
  }
  if (x > 88.7228f) {
    return inf_of(false);
  }
  return core::exp2_fast(x * kLog2e);
}

float logf_(float x) noexcept {
  if (is_nan(x)) {
    return x;
  }
  if (x == 0.0f) {
    return inf_of(true);
  }
  if (x < 0.0f) {
    return nan_value();
  }
  if (is_inf(x)) {
    return x;
  }
  return core::log2_fast(x) * kLn2;
}

float log10f_(float x) noexcept {
  if (is_nan(x)) {
    return x;
  }
  if (x == 0.0f) {
    return inf_of(true);
  }
  if (x < 0.0f) {
    return nan_value();
  }
  if (is_inf(x)) {
    return x;
  }
  return core::log2_fast(x) * kLog10of2;
}

float powf_(float x, float y) noexcept {
  if (y == 0.0f || x == 1.0f) {
    return 1.0f;
  }
  if (is_nan(x) || is_nan(y)) {
    return nan_value();
  }
  const int kind = integer_kind(y);
  const bool odd = kind == 2;
  if (is_inf(y)) {
    const float ax = fabsf_(x);
    if (ax == 1.0f) {
      return 1.0f;
    }
    return (ax > 1.0f) == (y > 0.0f) ? inf_of(false) : 0.0f;
  }
  if (x == 0.0f) {
    const bool negative = neg(x) && odd;
    if (y < 0.0f) {
      return inf_of(negative);
    }
    return negative ? -0.0f : 0.0f;
  }
  if (is_inf(x)) {
    const bool negative = neg(x) && odd;
    if (y > 0.0f) {
      return inf_of(negative);
    }
    return negative ? -0.0f : 0.0f;
  }
  if (x < 0.0f && kind == 0) {
    return nan_value();
  }
  const float e = y * core::log2_fast(fabsf_(x));
  const float r = e > 128.0f ? inf_of(false) : core::exp2_fast(e);
  return (x < 0.0f && odd) ? -r : r;
}

float tanhf_(float x) noexcept {
  if (is_nan(x)) {
    return x;
  }
  const float ax = fabsf_(x);
  float r;
  if (ax > 9.0f) {
    r = 1.0f;
  } else if (ax < 0.2f) {
    const float x2 = ax * ax;  // x - x^3/3 + 2x^5/15 - 17x^7/315 + 62x^9/2835
    r = ax * (1.0f + x2 * (-1.0f / 3.0f + x2 * (2.0f / 15.0f + x2 * (-17.0f / 315.0f + x2 * (62.0f / 2835.0f)))));
  } else {
    const float e = core::exp2_fast(2.0f * ax * kLog2e);
    r = (e - 1.0f) / (e + 1.0f);
  }
  return neg(x) ? -r : r;
}

void rand_seed(std::uint32_t seed) noexcept { g_rand_state = seed; }

std::int32_t rand_() noexcept {
  g_rand_state = g_rand_state * 6364136223846793005ull + 1ull;
  return static_cast<std::int32_t>((g_rand_state >> 32) & 0x7FFFFFFFu);
}

}  // namespace o3os::core::ptfx_math
