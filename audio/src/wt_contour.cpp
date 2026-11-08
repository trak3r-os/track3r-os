// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/wt_contour.h>

// OS-CEWTENV W2: the port of tracker-ce@7fd6c2c's `core/source/mtWavetableEnvelopes.cpp` (MIT; THIRD_PARTY.md). Only the procedural
// contours (initWavetableEnvelopes) and the warp law (evalWavetableEnvelopeFloat, unpackWarpX/Y, packWarpXY) are ported, with the maths
// identical: every float expression below is the original's, in the same order, single precision throughout (the original is float
// already; no double, no allocation, no exceptions). The `.wav` loader (loadUserWavetablesFromSD) and the Q30 fixed-point twin are not
// ported (slices W8 and the amp path of W4).

#include <math.h>

namespace o3os::audio {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

// OS-CEWTENV W3a: the bound bank (wt_contour.h). The pointer and the made-mask are the render's only state here (36 B of .bss); the bank
// itself is the caller's storage (the SDRAM arena on the MK66, a static in a host test). Main loop writes the pointer once, the render reads it.
WtBank* g_bank = nullptr;
WtMadeMask g_made{};
// OS-CEWTENV W3c: the bake pool (wt_contour.h): the pointer is the render's read, the rest main loop only (8 B of .bss).
WtBakeBank* g_bake = nullptr;
std::uint32_t g_bake_refused = 0;
// OS-CEWTENV W8: the user contour table (wt_contour.h; 4 B of .bss, the table itself is in SDRAM). Main loop binds it, the render reads the pointers in it.
WtUserBank* g_user = nullptr;

float clampf(float v, float lo, float hi) noexcept {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

}  // namespace

std::int8_t wt_warp_x(std::uint8_t bend) noexcept {
  const std::uint8_t nx = (bend >> 4) & 0x0Fu;
  if (nx <= 7u) {
    return static_cast<std::int8_t>(nx);
  }
  if (nx >= 9u) {
    return static_cast<std::int8_t>(nx - 16u);
  }
  return 0;
}

std::int8_t wt_warp_y(std::uint8_t bend) noexcept {
  const std::uint8_t ny = bend & 0x0Fu;
  if (ny <= 7u) {
    return static_cast<std::int8_t>(ny);
  }
  if (ny >= 9u) {
    return static_cast<std::int8_t>(ny - 16u);
  }
  return 0;
}

std::uint8_t wt_pack_warp(std::int8_t warp_x, std::int8_t warp_y) noexcept {
  if (warp_x < -7) {
    warp_x = -7;
  } else if (warp_x > 7) {
    warp_x = 7;
  }
  if (warp_y < -7) {
    warp_y = -7;
  } else if (warp_y > 7) {
    warp_y = 7;
  }
  const std::uint8_t nx = (warp_x >= 0) ? static_cast<std::uint8_t>(warp_x) : static_cast<std::uint8_t>(16 + warp_x);
  const std::uint8_t ny = (warp_y >= 0) ? static_cast<std::uint8_t>(warp_y) : static_cast<std::uint8_t>(16 + warp_y);
  return static_cast<std::uint8_t>(((nx & 0x0Fu) << 4) | (ny & 0x0Fu));
}

void wt_make_points(std::int16_t* dst, unsigned shape) noexcept {
  if (shape >= kWtContourCount) {
    shape = kWtContourCount - 1u;
  }
  const int group = static_cast<int>(shape) / 16;  // the original's `bank`: which family of 16 contours
  const int sub = static_cast<int>(shape) % 16;
  const float subf = static_cast<float>(sub);  // every int below enters float arithmetic exactly, as in the original

  for (unsigned i = 0; i < kWtContourLen; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kWtContourLen - 1u);  // 0.0 to 1.0
    float y = 0.0f;

    switch (group) {
      case 0: {  // Exponential & Logarithmic Decays / Percussive Plucks
        const float p = 0.5f + subf * 0.45f;
        y = powf(1.0f - t, p);
        break;
      }
      case 1: {  // Swells, S-Curves & Smooth Bells
        if (sub < 8) {
          const float p = 0.5f + subf * 0.5f;
          y = powf(sinf(t * kPi), p);
        } else {
          const float center = 0.1f + (subf - 8.0f) * 0.1f;
          const float sigma = 0.22f;
          const float diff = (t - center) / sigma;
          y = expf(-0.5f * diff * diff);
        }
        break;
      }
      case 2: {  // Double & Multi-Bounces
        const int bounces = 2 + (sub % 5);
        const float scaled_t = t * static_cast<float>(bounces);
        int b_idx = static_cast<int>(scaled_t);
        if (b_idx >= bounces) {
          b_idx = bounces - 1;
        }
        const float local_t = scaled_t - static_cast<float>(b_idx);
        const float arc = 4.0f * local_t * (1.0f - local_t);
        const float damp = powf(0.55f, static_cast<float>(b_idx));
        y = arc * damp;
        break;
      }
      case 3: {  // Rhythmic Gates & Trance Steppers
        static constexpr std::uint8_t kPulseCounts[16] = {2, 2, 3, 3, 4, 4, 6, 6, 8, 8, 12, 12, 16, 16, 8, 16};
        const int pulses = kPulseCounts[sub];
        const float duty = (sub % 2 == 0) ? 0.5f : 0.25f;
        const float cycle = t * static_cast<float>(pulses);
        const float frac = cycle - floorf(cycle);
        y = (frac < duty) ? 1.0f : 0.0f;
        break;
      }
      case 4: {  // Sharks, Fins & Asymmetric Triangles
        const float peak = 0.05f + subf * 0.06f;
        if (t < peak) {
          y = t / peak;
        } else {
          y = 1.0f - (t - peak) / (1.0f - peak);
        }
        break;
      }
      case 5: {  // Ripples & Damped Oscillations
        const float freq = 2.0f + subf;
        const float decay = expf(-3.5f * t);
        y = decay * (0.5f + 0.5f * cosf(kTwoPi * freq * t));
        break;
      }
      case 6: {  // Accelerating & Decelerating Trills
        const float chirp = (sub < 8) ? (t * t) : sqrtf(t);
        const int n = 3 + (sub % 8);
        const float mod_t = chirp * static_cast<float>(n);
        const float frac = mod_t - floorf(mod_t);
        y = (1.0f - t * 0.6f) * ((frac < 0.4f) ? 1.0f : 0.0f);
        break;
      }
      case 7: {  // Staircases Up, Down & Pyramids
        const int steps = 2 + sub;
        int cur_step = static_cast<int>(t * static_cast<float>(steps));
        if (cur_step >= steps) {
          cur_step = steps - 1;
        }
        if (sub < 8) {
          y = static_cast<float>(cur_step) / static_cast<float>(steps - 1);
        } else {
          y = 1.0f - (static_cast<float>(cur_step) / static_cast<float>(steps - 1));
        }
        break;
      }
      // --- Groups 8-15: Default Factory Fallbacks for User Slots ---
      case 8: {  // Wavefolded West-Coast
        const float w = sinf(t * kTwoPi * (1.0f + subf * 0.3f));
        y = fabsf(w);
        if (y > 0.6f) {
          y = 1.2f - y;
        }
        y *= 1.4f;
        break;
      }
      case 9: {  // Syncopated Pulses
        const std::uint16_t pat = static_cast<std::uint16_t>(0xAA55 ^ (sub * 0x1111));  // int arithmetic, truncated, as in the original
        int bit = static_cast<int>(t * 16.0f);
        if (bit > 15) {
          bit = 15;
        }
        y = ((pat >> bit) & 1u) ? 1.0f : 0.0f;
        break;
      }
      case 10: {  // Reverse Swells with Snappy Cutoffs
        const float p = 1.2f + subf * 0.3f;
        if (t < 0.92f) {
          y = powf(t / 0.92f, p);
        } else {
          y = (1.0f - t) / 0.08f;
        }
        break;
      }
      case 11: {  // Dual-Peak Camel / Formant Contours
        const float p1 = 0.25f;
        const float p2 = 0.75f;
        const float d1 = (t - p1) / 0.15f;
        const float d2 = (t - p2) / (0.15f + subf * 0.01f);
        y = 0.7f * expf(-d1 * d1) + (0.5f + subf * 0.03f) * expf(-d2 * d2);
        break;
      }
      case 12: {  // Growing Chaos / Rising Wobble
        const float wobble = 0.5f + 0.5f * sinf(kTwoPi * (1.0f + subf * 1.5f) * t * t);
        y = t * wobble;
        break;
      }
      case 13: {  // Karplus-Style Plucks
        const float decay = expf(-5.0f * t);
        const float ripple = 0.8f + 0.2f * sinf(kTwoPi * (10.0f + subf * 2.0f) * t);
        y = decay * ripple;
        break;
      }
      case 14: {  // Deterministic Stepped S&H
        std::uint32_t seed = static_cast<std::uint32_t>(static_cast<int>(shape) * 12345 + static_cast<int>(t * 16.0f) * 6789);
        seed = (seed * 1103515245u + 12345u) & 0x7FFFFFFFu;
        y = static_cast<float>(seed % 1000u) / 1000.0f;
        break;
      }
      case 15:   // Cinematic Morphing Risers
      default: {
        const float s = sinf(t * kPi * 0.5f);
        const float ramp = powf(t, 2.0f + subf * 0.2f);
        y = 0.5f * s + 0.5f * ramp;
        break;
      }
    }

    y = clampf(y, 0.0f, 1.0f);
    dst[i] = static_cast<std::int16_t>(y * 32767.0f);
  }
}

void wt_make(WtBank& bank, unsigned shape, WtMadeMask& made) noexcept {
  if (shape >= kWtContourCount) {
    shape = kWtContourCount - 1u;
  }
  wt_make_points(bank.contour[shape], shape);
  made.word[shape / 32u] |= 1u << (shape % 32u);
}

bool wt_made(const WtMadeMask& made, unsigned shape) noexcept {
  if (shape >= kWtContourCount) {
    return false;
  }
  return (made.word[shape / 32u] & (1u << (shape % 32u))) != 0u;
}

const std::int16_t* wt_contour(const WtBank& bank, unsigned shape) noexcept {
  return bank.contour[shape];
}

float wt_eval(const std::int16_t* contour, float phase, std::uint8_t bend) noexcept {
  float p = clampf(phase, 0.0f, 1.0f);

  if (bend != 0u) {
    const std::int8_t wx = wt_warp_x(bend);
    if (wx != 0) {
      const float k = static_cast<float>(wx) * (1.0f / 9.0f);
      const float denom = p * 2.0f * k + 1.0f - k;
      if (denom != 0.0f) {
        p = (p * (1.0f + k)) / denom;
      }
      p = clampf(p, 0.0f, 1.0f);
    }
  }

  const float idx_f = p * static_cast<float>(kWtContourLen - 1u);
  int idx = static_cast<int>(idx_f);
  if (idx < 0) {
    idx = 0;
  }
  if (idx >= static_cast<int>(kWtContourLen) - 1) {
    idx = static_cast<int>(kWtContourLen) - 2;
  }
  const float frac = idx_f - static_cast<float>(idx);

  const float y0 = static_cast<float>(contour[idx]);
  const float y1 = static_cast<float>(contour[idx + 1]);
  float y = (y0 + frac * (y1 - y0)) * (1.0f / 32767.0f);

  if (bend != 0u) {
    const std::int8_t wy = wt_warp_y(bend);
    if (wy == -7) {
      y = 1.0f - y;
    } else if (wy != 0) {
      const float ky = static_cast<float>(wy) * (1.0f / 9.0f);
      const float denom = y * 2.0f * ky + 1.0f - ky;
      if (denom != 0.0f) {
        y = (y * (1.0f + ky)) / denom;
      }
    }
    return clampf(y, 0.0f, 1.0f);
  }

  return y;
}

void wt_bind(WtBank& bank) noexcept {
  g_bank = &bank;
  g_made = WtMadeMask{};
  g_user = nullptr;  // the user contours name rows of the previous bank: bind them again
}

void wt_unbind(const WtBank& bank) noexcept {
  if (g_bank == &bank) {
    g_bank = nullptr;
    g_user = nullptr;
  }
}

namespace {
// The contour the render reads for `shape` of the bound bank: a user slot's published pointer, else its bank row. One load of the pointer.
const std::int16_t* resolve(unsigned shape) noexcept {
  if (shape >= kWtContourFactoryCount && g_user != nullptr) {
    return __atomic_load_n(&g_user->ptr[shape - kWtContourFactoryCount], __ATOMIC_RELAXED);
  }
  return wt_contour(*g_bank, shape);
}
}  // namespace

void wt_user_bind(WtUserBank& user) noexcept {
  if (g_bank == nullptr) {
    return;
  }
  for (unsigned i = 0; i < kWtUserCount; ++i) {
    user.ptr[i] = g_bank->contour[kWtContourFactoryCount + i];
  }
  for (std::uint32_t& w : user.loaded) {
    w = 0u;
  }
  g_user = &user;
}

void wt_user_unbind(const WtUserBank& user) noexcept {
  if (g_user == &user) {
    g_user = nullptr;
  }
}

bool wt_user_is_file(unsigned n) noexcept {
  return g_user != nullptr && n < kWtUserCount && (g_user->loaded[n / 32u] & (1u << (n % 32u))) != 0u;
}

namespace {
// Kills the baked entries of `shape` (key 0 never matches: shape 0 with bend 0 is never baked), so a later wt_bake of it bakes the new contour.
void kill_bakes(unsigned shape) noexcept {
  if (g_bake == nullptr) {
    return;
  }
  for (std::uint32_t i = 0; i < g_bake->used; ++i) {
    if ((g_bake->key[i] >> 8) == shape) {
      g_bake->key[i] = 0u;
    }
  }
}

// Writes the spare of {bank row, buf[n]} through `fill`, then publishes it with one aligned store.
template <typename Fill>
void publish(unsigned n, Fill fill) noexcept {
  const unsigned shape = kWtContourFactoryCount + n;
  std::int16_t* row = g_bank->contour[shape];
  std::int16_t* buf = g_user->buf[n];
  const std::int16_t* cur = g_user->ptr[n];
  std::int16_t* spare = (cur == buf) ? row : buf;
  fill(spare);
  g_made.word[shape / 32u] |= 1u << (shape % 32u);
  kill_bakes(shape);
  __atomic_store_n(&g_user->ptr[n], static_cast<const std::int16_t*>(spare), __ATOMIC_RELEASE);  // release: the fill's stores stay before the pointer's (a compiler order; one core)
}
}  // namespace

bool wt_user_set(unsigned n, const std::int16_t* points) noexcept {
  if (g_user == nullptr || g_bank == nullptr || n >= kWtUserCount) {
    return false;
  }
  publish(n, [points](std::int16_t* dst) {
    for (unsigned i = 0; i < kWtContourLen; ++i) {
      dst[i] = points[i];
    }
  });
  g_user->loaded[n / 32u] |= 1u << (n % 32u);
  return true;
}

bool wt_user_restore(unsigned n) noexcept {
  if (!wt_user_is_file(n)) {
    return false;
  }
  const unsigned shape = kWtContourFactoryCount + n;
  publish(n, [shape](std::int16_t* dst) { wt_make_points(dst, shape); });
  g_user->loaded[n / 32u] &= ~(1u << (n % 32u));
  return true;
}

void wt_bake_unbind(const WtBakeBank& pool) noexcept {
  if (g_bake == &pool) {
    g_bake = nullptr;
  }
}

void wt_make_bound(unsigned shape) noexcept {
  if (g_bank != nullptr && !wt_made(g_made, shape)) {
    wt_make(*g_bank, shape, g_made);
  }
}

void wt_bake_bind(WtBakeBank& pool) noexcept {
  pool.used = 0;
  g_bake_refused = 0;
  g_bake = &pool;
}

void wt_bake_reset() noexcept {
  if (g_bake != nullptr) {
    g_bake->used = 0;
  }
  g_bake_refused = 0;
}

unsigned wt_bake(unsigned shape, std::uint8_t bend) noexcept {
  if (shape >= kWtContourCount) {
    shape = kWtContourCount - 1u;
  }
  wt_make_bound(shape);
  if (g_bake == nullptr || g_bank == nullptr || !wt_made(g_made, shape) || (wt_warp_x(bend) == 0 && wt_warp_y(bend) == 0)) {
    return 0u;  // nothing to bake: no warp (the bank's contour is the slot's), no pool, no contour
  }
  bend = wt_pack_warp(wt_warp_x(bend), wt_warp_y(bend));  // the canonical pair: nibble 8 and nibble 0 are both a warp of 0
  WtBakeBank& pool = *g_bake;
  const std::uint16_t key = static_cast<std::uint16_t>((shape << 8) | bend);
  for (std::uint32_t i = 0; i < pool.used; ++i) {
    if (pool.key[i] == key) {
      return i + 1u;
    }
  }
  if (pool.used >= kWtBakeCount) {
    ++g_bake_refused;
    return 0u;  // the pool is full: this slot applies its warps live
  }
  const std::int16_t* src = resolve(shape);
  std::int16_t* dst = pool.contour[pool.used];
  for (unsigned i = 0; i < kWtContourLen; ++i) {
    const float v = wt_eval(src, static_cast<float>(i) * (1.0f / static_cast<float>(kWtContourLen - 1u)), bend);  // 0..1 (i = 127 is exactly phase 1)
    dst[i] = static_cast<std::int16_t>(v * 32767.0f + 0.5f);
  }
  pool.key[pool.used] = key;
  pool.used = pool.used + 1u;  // published after the entry is whole
  return pool.used;
}

unsigned wt_bake_used() noexcept { return g_bake != nullptr ? g_bake->used : 0u; }

unsigned wt_bake_refused() noexcept { return g_bake_refused; }

float wt_eval_slot(unsigned shape, unsigned bake, std::uint8_t bend, float phase) noexcept {
  if (bake != 0u && g_bake != nullptr && bake <= g_bake->used) {
    return wt_eval(g_bake->contour[bake - 1u], phase, 0u);
  }
  return wt_eval_shape(shape, phase, bend);
}

float wt_eval_shape(unsigned shape, float phase, std::uint8_t bend) noexcept {
  if (g_bank == nullptr || !wt_made(g_made, shape)) {
    return 0.0f;  // no bank bound, or the contour not made yet: level 0, never a wild read
  }
  return wt_eval(resolve(shape), phase, bend);
}

}  // namespace o3os::audio
