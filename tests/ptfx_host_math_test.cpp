// SPDX-License-Identifier: GPL-3.0-or-later
//
// The math of the .ptfx host table (core/ptfx_math.h, OS-CEPTFX S2) against the host's libm, and the shared sin/cos (core/fast_math.h).
#include <doctest/doctest.h>

#include <o3os/core/fast_math.h>
#include <o3os/core/ptfx_math.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace pm = o3os::core::ptfx_math;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
const float kNan = std::numeric_limits<float>::quiet_NaN();

// Relative 2e-6 plus absolute 2e-6.
bool close(float got, double want) { return std::fabs(static_cast<double>(got) - want) <= 2e-6 + 2e-6 * std::fabs(want); }

}  // namespace

TEST_CASE("sin_fast and cos_fast: absolute error under 1e-6 and the out-of-range rule") {
  for (float x = -100.0f; x <= 100.0f; x += 0.0137f) {
    CHECK(std::fabs(static_cast<double>(o3os::core::sin_fast(x)) - std::sin(static_cast<double>(x))) < 1e-6);
    CHECK(std::fabs(static_cast<double>(o3os::core::cos_fast(x)) - std::cos(static_cast<double>(x))) < 1e-6);
  }
  CHECK(o3os::core::sin_fast(kNan) == 0.0f);
  CHECK(o3os::core::cos_fast(kInf) == 0.0f);
}

TEST_CASE("exp, log, log10 against libm") {
  for (float x = -10.0f; x <= 10.0f; x += 0.01f) {
    CHECK(close(pm::expf_(x), std::exp(static_cast<double>(x))));
  }
  for (float x = 0.001f; x <= 1000.0f; x *= 1.013f) {
    CHECK(close(pm::logf_(x), std::log(static_cast<double>(x))));
    CHECK(close(pm::log10f_(x), std::log10(static_cast<double>(x))));
  }
  CHECK(pm::expf_(-200.0f) == 0.0f);
  CHECK(std::isinf(pm::expf_(100.0f)));
  CHECK(std::isinf(pm::logf_(0.0f)));
  CHECK(pm::logf_(0.0f) < 0.0f);
  CHECK(std::isnan(pm::logf_(-1.0f)));
  CHECK(std::isnan(pm::log10f_(-1.0f)));
  CHECK(std::isinf(pm::logf_(kInf)));
  CHECK(std::isnan(pm::expf_(kNan)));
}

TEST_CASE("tan and tanh against libm") {
  for (float x = -1.5f; x <= 1.5f; x += 0.0071f) {
    CHECK(close(pm::tanf_(x), std::tan(static_cast<double>(x))));
  }
  for (float x = -12.0f; x <= 12.0f; x += 0.0043f) {
    CHECK(close(pm::tanhf_(x), std::tanh(static_cast<double>(x))));
  }
  CHECK(pm::tanhf_(50.0f) == 1.0f);
  CHECK(pm::tanhf_(-50.0f) == -1.0f);
  CHECK(pm::tanhf_(0.0f) == 0.0f);
  CHECK(pm::tanf_(kInf) == 0.0f);
}

TEST_CASE("pow against libm, and its special cases") {
  for (float x = 0.01f; x <= 50.0f; x *= 1.17f) {
    for (float y = -3.0f; y <= 3.0f; y += 0.37f) {
      CHECK(close(pm::powf_(x, y), std::pow(static_cast<double>(x), static_cast<double>(y))));
    }
  }
  CHECK(pm::powf_(7.0f, 0.0f) == 1.0f);
  CHECK(pm::powf_(kNan, 0.0f) == 1.0f);
  CHECK(pm::powf_(1.0f, kNan) == 1.0f);
  CHECK(std::isnan(pm::powf_(kNan, 2.0f)));
  CHECK(pm::powf_(0.0f, 2.0f) == 0.0f);
  CHECK(std::isinf(pm::powf_(0.0f, -1.0f)));
  CHECK(close(pm::powf_(-2.0f, 3.0f), -8.0));  // odd integer: negative
  CHECK(close(pm::powf_(-2.0f, 2.0f), 4.0));   // even integer: positive
  CHECK(std::isnan(pm::powf_(-2.0f, 0.5f)));   // fractional exponent of a negative base
  CHECK(std::isinf(pm::powf_(kInf, 2.0f)));
  CHECK(pm::powf_(kInf, -2.0f) == 0.0f);
  CHECK(std::isinf(pm::powf_(2.0f, kInf)));
  CHECK(pm::powf_(0.5f, kInf) == 0.0f);
  CHECK(pm::powf_(-1.0f, kInf) == 1.0f);
  CHECK(std::isinf(pm::powf_(10.0f, 100.0f)));  // overflow
  CHECK(pm::powf_(10.0f, -100.0f) == 0.0f);
}

TEST_CASE("fmin, fmax and fabs follow libm's NaN rule") {
  CHECK(pm::fminf_(1.0f, 2.0f) == 1.0f);
  CHECK(pm::fmaxf_(1.0f, 2.0f) == 2.0f);
  CHECK(pm::fminf_(kNan, 2.0f) == 2.0f);
  CHECK(pm::fmaxf_(1.0f, kNan) == 1.0f);
  CHECK(std::isnan(pm::fmaxf_(kNan, kNan)));
  CHECK(pm::fabsf_(-3.5f) == 3.5f);
  CHECK(pm::fabsf_(3.5f) == 3.5f);
  CHECK(std::isinf(pm::fabsf_(-kInf)));
}

TEST_CASE("rand is newlib's LCG: deterministic, in range, repeatable after a reseed") {
  pm::rand_seed();
  std::uint64_t s = 1u;
  for (int i = 0; i < 1000; ++i) {
    s = s * 6364136223846793005ull + 1ull;
    const std::int32_t want = static_cast<std::int32_t>((s >> 32) & 0x7FFFFFFFu);
    const std::int32_t got = pm::rand_();
    CHECK(got == want);
    CHECK(got >= 0);
  }
  pm::rand_seed();
  const std::int32_t first = pm::rand_();
  pm::rand_seed();
  CHECK(pm::rand_() == first);
}
