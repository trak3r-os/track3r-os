// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): ENV FILTER, a mono state-variable filter whose cutoff is driven by an envelope follower on the effect's own input (an auto-wah), for the first
// effect pack (docs/roadmap/BACKLOG.md OS-INSFX-FX1), after DRIVE (modules/effects/drive), CRUSH (modules/effects/crush) and the biquad of OS-INSFX S8 (modules/effects/biquad). All the maths is this
// module's own: no third-party code (THIRD_PARTY.md names the structure of the filter, a published one, and nothing else), float only, no division and no library call in the sample loop, and no
// host maths call in render() at all.
//
//   - Filter: the topology-preserving (trapezoidal, zero-delay feedback) state-variable filter. With g = tan(pi fc / fs) and k = 1 / Q (the damping):
//       a1 = 1 / (1 + g (g + k)),  a2 = g a1,  a3 = g a2
//       v3 = x - ic2;  v1 = a1 ic1 + a2 v3;  v2 = ic2 + a2 ic1 + a3 v3;  ic1 = 2 v1 - ic1;  ic2 = 2 v2 - ic2        (v1: band-pass, v2: low-pass)
//     The three modes are one weighted sum of the same states: LP = v2, BP = k v1 (peak gain 1 at the cutoff, not 1 / k), HP = x - k v1 - v2. The weights and the mix fold into three per-chunk
//     coefficients, out = A x + B v1 + C v2 with A = 1 - m + m wx, B = m wb k, C = m wl, so the mode, the mix and the resonance cost three multiplies a sample whatever they are.
//   - Stability (the Resonance bound): the bilinear transform maps the analogue poles, which are in the open left half plane for every k > 0, inside the unit circle for every g > 0, so the filter
//     is stable at every cutoff and damping the parameters can reach. Resonance 0..100 % is Q = 1/sqrt(2) (k = sqrt(2), the Butterworth: |H| <= 1 at every frequency in every mode, so the defaults
//     can never raise the level) up to Q = 10 (k = 0.1, a peak of +20 dB at the cutoff), on an exponential curve. The coefficients move at control rate with the states kept, which the trapezoidal
//     form takes without a transient; the host test measures the poles, the impulse response and a long run at the worst setting.
//   - Envelope follower: on the input itself (the audio sidechain comes with OS-INSFX-ABI2 A6): e += c (|x| - e), c = Attack's coefficient while |x| is above e and Release's below, each the one-pole
//     1 - exp(-1 / (T fs)) of the time T (the time to cover 63 % of a step). Linear: a compare and a branch between the attack and the release path a sample (the compiler emits it as a branch, not a predicated move).
//   - Sweep (control rate, once every kCtl = 16 samples): u = clamp(1 + (log2 e + Sens) / 8, 0, 1), a 48 dB window ending at the level that opens the filter fully (Sens, in dB, is how far under
//     full scale that level is: 0 dB opens it at a full-scale follower, +24 dB at -24 dBFS), and the cutoff is fc = Cutoff 2^(Range u), kept between 20 Hz and 0.4 fs (a Range under 0 sweeps down
//     from the base, over 0 up). log2 and 2^x are this module's own polynomials (1e-7 relative on 2^x, 2.4e-6 absolute on log2) and tan is a (5, 6) Pade (4e-8 relative up to 0.4 pi).
//   - Smoothing: Cutoff, Range, Resonance, Sens, Mode (a cross-fade of the three weights) and Mix move to their targets by 1/10 of the distance every chunk (3.6 ms), snapping when under 1e-5 away,
//     so a set_param never zippers or clicks and Mix 0 is the input bit for bit once it has arrived. Before the first block set_param snaps instead. Attack and Release act at once (the follower
//     has no audible state of its own). set_param stores and evaluates a polynomial or two at most, so a MOD parameter may be called every block.
//   - Tail (OS-INSFX S2 contract): render() returns 1 while the input is live (its last sample is not 0) or the filter rings (a state over 1e-6, -120 dBFS), and 0 when both are silent, flushing the
//     states and the follower to exactly 0 then. A NaN, an infinity or an absurd (over 1e18) state is flushed at the end of the block: a hostile input sample can taint the rest of that block's
//     output (the filter is recursive), never the next one.
//   - Parameters, in this order for ever: Mode (LP, BP, HP), Cutoff (20..20000 Hz), Range (-6..+6 octaves), Resonance (0..100 %), Attack (0.5..200 ms), Release (5..2000 ms), Sens (0..+36 dB),
//     Mix (0..100 %). Cutoff is the first modulation target, so the unit bench's moving-parameter row sweeps it, while its settled row runs the defaults.
#include <o3module.h>

#include "envfilter_gen.h"

#define EF_INLINE inline __attribute__((always_inline))  // the per-chunk helpers fold into render(): left out of line they would run from cold SDRAM

namespace {

constexpr uint32_t kCtl = 16;           // samples a coefficient step lasts inside a block
constexpr uint32_t kBlockMax = O3M_BLOCK_MAX;
constexpr float kPi = 3.14159265358979f;
constexpr float kInsane = 1e18f;        // a state beyond this (or NaN, or infinite) is flushed to 0 at the end of the block
constexpr float kSilent = 1e-6f;        // a filter state under this is silent (-120 dBFS)
constexpr float kDbToLog2 = 0.16609640474436812f;  // log2(10)/20: 10^(db/20) = 2^(db * kDbToLog2)
constexpr float kMinHz = 20.0f;
constexpr float kMaxHz = 20000.0f;
constexpr float kLogMinHz = 4.321928094887362f;    // log2(20)
constexpr float kMaxFrac = 0.4f;        // the highest swept cutoff, a fraction of the sample rate (tan stays under 3.08)
constexpr float kSqrt2 = 1.4142135623730951f;
constexpr float kLogQSpan = 3.821928094887362f;    // log2(Qmax sqrt(2)) for Qmax = 10: k goes from sqrt(2) to 1/10 across Resonance
constexpr float kSmooth = 0.1f;         // the share of the distance a control step covers
constexpr float kSnap = 1e-5f;          // closer than this a smoothed value lands on its target

constexpr float kWx[3] = {0.0f, 0.0f, 1.0f};   // the mode's weights of x, k v1 and v2: LP, BP, HP
constexpr float kWb[3] = {0.0f, 1.0f, -1.0f};
constexpr float kWl[3] = {1.0f, 0.0f, -1.0f};

enum Slot : uint32_t { kL, kR, kK, kS, kWX, kWB, kWL, kM, kSlots };  // the smoothed values: log2 cutoff, range, damping, log2 sens gain, the three mode weights, the mix

struct State {
  const o3m_host* host;
  float rate;
  float pi_fs;        // pi / sample rate
  float l_max;        // log2 of the highest cutoff
  float ca;           // the follower's coefficients
  float cr;
  float ic1;          // the filter's integrator states
  float ic2;
  float env;          // the follower
  uint32_t moving;    // some smoothed value is not on its target
  uint32_t fresh;     // no render yet: set_param snaps instead of gliding
  float t[kSlots];    // targets (set_param)
  float s[kSlots];    // the smoothed values
};
static_assert(sizeof(State) <= ENVFILTER_STATE_BYTES, "module.json: state_bytes is too small for State");

EF_INLINE float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN becomes lo
EF_INLINE float absf(float x) { return __builtin_fabsf(x); }

EF_INLINE float pow2i(int e) {  // 2^e exactly, for -126..127
  const uint32_t u = static_cast<uint32_t>(127 + e) << 23;
  float f;
  __builtin_memcpy(&f, &u, sizeof f);
  return f;
}

// 2^x for any x (clamped to +-100): the integer part by the exponent field, the fraction by a degree 5 polynomial (1e-7 relative).
EF_INLINE float exp2p(float x) {
  x = clampf(x, -100.0f, 100.0f);
  int i = static_cast<int>(x);
  float f = x - static_cast<float>(i);
  if (f < 0.0f) {
    f += 1.0f;
    --i;
  }
  const float p = 0.9999999f + f * (0.69315449f + f * (0.24014182f + f * (0.05586034f + f * (0.00894959f + f * 0.00189375f))));
  return p * pow2i(i);
}

// log2 of a non-negative x: the exponent field and a degree 6 polynomial of the mantissa (2.4e-6 absolute). 0 and the subnormals give about -127, a NaN or an infinity a finite value.
EF_INLINE float log2p(float x) {
  uint32_t b;
  __builtin_memcpy(&b, &x, sizeof b);
  const int e = static_cast<int>((b >> 23) & 0xFFu) - 127;
  const uint32_t mb = (b & 0x007FFFFFu) | 0x3F800000u;
  float m;
  __builtin_memcpy(&m, &mb, sizeof m);
  const float p = -3.02915154f + m * (6.06687145f + m * (-5.26168251f + m * (3.21280029f + m * (-1.22929101f + m * (0.26502429f + m * -0.02456853f)))));
  return p + static_cast<float>(e);
}

// tan x, a (5, 6) Pade of the continued fraction, for 0 <= x <= 0.4 pi (4e-8 relative); one division, once a chunk.
EF_INLINE float tanp(float x) {
  const float x2 = x * x;
  return x * (10395.0f + x2 * (-1260.0f + x2 * 21.0f)) / (10395.0f + x2 * (-4725.0f + x2 * (210.0f - x2)));
}

// A time T in ms as the one-pole coefficient 1 - exp(-a), a = 1 / (T fs): by its series (a is under 0.05 for T >= 0.5 ms at 44.1 kHz, and a 1 - 2^x of a polynomial's 3.5e-6 would be
// 12 % off for a release of 800 ms), 1e-9 relative at the largest a.
EF_INLINE float pole_coef(const State* s, float ms) {
  const float a = 1.0f / (ms * 0.001f * s->rate);
  return a * (1.0f - a * (0.5f - a * (1.0f / 6.0f - a * (1.0f / 24.0f - a * (1.0f / 120.0f)))));
}

void target(State* s, uint32_t slot, float v) {
  s->t[slot] = v;
  s->moving = 1;
  if (s->fresh != 0u) {  // before the first block the value is where the effect starts, not a glide to it
    s->s[slot] = v;
  }
}

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  switch (id) {
    case ENVFILTER_PARAM_MODE: {
      const uint32_t m = static_cast<uint32_t>(clampf(value, 0.0f, 2.0f) + 0.5f);
      target(s, kWX, kWx[m]);
      target(s, kWB, kWb[m]);
      target(s, kWL, kWl[m]);
      return;
    }
    case ENVFILTER_PARAM_CUTOFF: target(s, kL, log2p(clampf(value, kMinHz, kMaxHz))); return;
    case ENVFILTER_PARAM_RANGE: target(s, kR, clampf(value, -6.0f, 6.0f)); return;
    case ENVFILTER_PARAM_RESONANCE: target(s, kK, kSqrt2 * exp2p(-clampf(value, 0.0f, 100.0f) * 0.01f * kLogQSpan)); return;
    case ENVFILTER_PARAM_ATTACK: s->ca = pole_coef(s, clampf(value, 0.5f, 200.0f)); return;
    case ENVFILTER_PARAM_RELEASE: s->cr = pole_coef(s, clampf(value, 5.0f, 2000.0f)); return;
    case ENVFILTER_PARAM_SENS: target(s, kS, clampf(value, 0.0f, 36.0f) * kDbToLog2); return;
    case ENVFILTER_PARAM_MIX: target(s, kM, clampf(value, 0.0f, 100.0f) * 0.01f); return;
    default: return;
  }
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->rate = host->sample_rate;
  s->pi_fs = kPi / s->rate;
  s->l_max = log2p(kMaxFrac * s->rate);
  s->fresh = 1;
  s->moving = 0;
  s->ic1 = 0.0f;
  s->ic2 = 0.0f;
  s->env = 0.0f;
  for (uint32_t i = 0; i < ENVFILTER_PARAM_COUNT; ++i) {
    set_param(s, i, envfilter_params[i].def);
  }
  return 0;
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// One call of 1..kBlockMax frames: per chunk of kCtl samples the control step (smoothing, the sweep, the coefficients), then the filter and the follower over the chunk's samples.
O3M_HOT uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  uint32_t live = 0;
  while (frames > 0u) {  // (the ABI promises at most kBlockMax; a host that gives more is served in pieces)
    const uint32_t n = frames < kBlockMax ? frames : kBlockMax;
    s->fresh = 0;
    uint32_t piece = in[n - 1u] != 0.0f ? 1u : 0u;  // before anything writes: in may be out
    const uint32_t chunks = (n + kCtl - 1u) / kCtl;
    const float ca = s->ca;  // the filter and the follower, in registers for the whole block
    const float cr = s->cr;
    float ic1 = s->ic1;
    float ic2 = s->ic2;
    float e = s->env;
    uint32_t at = 0;
    for (uint32_t j = 0; j < chunks; ++j) {
      // ---- control step ---------------------------------------------------------------------------------------------------------------------
      if (s->moving != 0u) {
        uint32_t still = 0;
        for (uint32_t q = 0; q < kSlots; ++q) {
          const float d = s->t[q] - s->s[q];
          if (absf(d) < kSnap) {
            s->s[q] = s->t[q];
          } else {
            s->s[q] += d * kSmooth;
            still = 1;
          }
        }
        s->moving = still;
      }
      const float u = clampf(1.0f + (log2p(e) + s->s[kS]) * 0.125f, 0.0f, 1.0f);  // where the follower is in its 48 dB window
      const float l = clampf(s->s[kL] + s->s[kR] * u, kLogMinHz, s->l_max);       // log2 of the cutoff
      const float g = tanp(exp2p(l) * s->pi_fs);
      const float k = s->s[kK];
      const float a1 = 1.0f / (1.0f + g * (g + k));
      const float a2 = g * a1;
      const float a3 = g * a2;
      const float m = s->s[kM];
      const float cA = 1.0f - m + m * s->s[kWX];
      const float cB = m * s->s[kWB] * k;
      const float cC = m * s->s[kWL];
      // ---- the samples ----------------------------------------------------------------------------------------------------------------------
      const uint32_t len = n - at < kCtl ? n - at : kCtl;
      for (uint32_t i = 0; i < len; ++i) {
        const float x = in[at + i];
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        out[at + i] = cA * x + cB * v1 + cC * v2;
        const float d = absf(x) - e;
        e += (d > 0.0f ? ca : cr) * d;  // attack above, release below (a compare and a branch)
      }
      at += len;
    }
    const float a1s = absf(ic1);
    const float a2s = absf(ic2);
    if (!(a1s <= kInsane && a2s <= kInsane && e <= kInsane)) {  // a NaN, an infinity or an absurd input got into the recursion (false for NaN): start again from rest
      ic1 = 0.0f;
      ic2 = 0.0f;
      e = 0.0f;
    } else if (a1s > kSilent || a2s > kSilent) {
      piece = 1u;  // the filter rings
    }
    if (piece == 0u) {  // silent in and silent out: exact zeros from here on, and the follower forgets
      ic1 = 0.0f;
      ic2 = 0.0f;
      e = 0.0f;
    }
    s->ic1 = ic1;
    s->ic2 = ic2;
    s->env = e;
    live |= piece;
    in += n;
    out += n;
    frames -= n;
  }
  return live;  // 1 while the input is live or the filter rings, 0 when both are silent (the S2 tail contract)
}

// ---- the view: the filter's magnitude response at the base cutoff (filled) and at the cutoff the follower reaches when Range is fully used (a line) ------------------------------------------
constexpr uint32_t kPts = 40;

// The response in dB of the current mode and mix at the frequencies f, for a cutoff fc: the bilinear map puts s = j r, r = tan(pi f / fs) / g, so D = 1 - r^2 + j k r, the low-pass is 1 / D, the
// band-pass (k v1) j k r / D, the high-pass -r^2 / D; the total is (1 - m) + m (wx + (wl + j wb k r) / D).
void response(const State* st, const float* params, float fc, const float* f, float* db) {
  const o3m_host* host = st->host;
  const uint32_t mode = static_cast<uint32_t>(clampf(params[ENVFILTER_PARAM_MODE], 0.0f, 2.0f) + 0.5f);
  const float k = kSqrt2 * host->exp2_f(-clampf(params[ENVFILTER_PARAM_RESONANCE], 0.0f, 100.0f) * 0.01f * kLogQSpan);
  const float m = clampf(params[ENVFILTER_PARAM_MIX], 0.0f, 100.0f) * 0.01f;
  const float a = m * kWl[mode];
  const float c0 = 1.0f - m + m * kWx[mode];
  fc = clampf(fc, kMinHz, kMaxFrac * st->rate);
  const float g = host->sin_f(fc * st->pi_fs) / host->cos_f(fc * st->pi_fs);
  for (uint32_t i = 0; i < kPts; ++i) {
    const float w = f[i] * st->pi_fs;
    const float r = host->sin_f(w) / host->cos_f(w) / g;
    const float dr = 1.0f - r * r;
    const float di = k * r;
    const float b = m * kWb[mode] * di;
    const float den = dr * dr + di * di;
    const float tr = c0 + (a * dr + b * di) / den;
    const float ti = (b * dr - a * di) / den;
    const float p = tr * tr + ti * ti;
    db[i] = clampf(3.0103f * host->log2_f(p > 1e-10f ? p : 1e-10f), -48.0f, 24.0f);
  }
}

void view_draw(void* raw, uint32_t view, o3m_canvas* cv) {
  const State* s = static_cast<const State*>(raw);  // read only: the host table and the rate, which never change after init()
  if (view != ENVFILTER_VIEW_RESPONSE || cv->param_count < ENVFILTER_PARAM_COUNT) {
    return;
  }
  const o3m_host* host = s->host;
  const float cutoff = clampf(cv->params[ENVFILTER_PARAM_CUTOFF], kMinHz, kMaxHz);
  const float range = clampf(cv->params[ENVFILTER_PARAM_RANGE], -6.0f, 6.0f);
  float f[kPts];
  float base[kPts];
  float open[kPts];
  const float step = host->log2_f(kMaxHz / kMinHz) / static_cast<float>(kPts - 1u);
  for (uint32_t i = 0; i < kPts; ++i) {
    f[i] = kMinHz * host->exp2_f(step * static_cast<float>(i));
  }
  response(s, cv->params, cutoff, f, base);
  response(s, cv->params, cutoff * host->exp2_f(range), f, open);
  o3m_curve curve;
  o3m_curve_set(&curve, f, base, kPts, O3M_AXIS_LOG_FREQ, O3M_AXIS_DB, kMinHz, kMaxHz, -48.0f, 24.0f, O3M_CURVE_AXES | O3M_CURVE_FILL, O3M_COLOR_FX);
  cv->draw->curve(cv, &curve);
  o3m_curve_set(&curve, f, open, kPts, O3M_AXIS_LOG_FREQ, O3M_AXIS_DB, kMinHz, kMaxHz, -48.0f, 24.0f, 0u, O3M_COLOR_ACCENT);
  cv->draw->curve(cv, &curve);
  static const char* const kNames[3] = {"LOW-PASS", "BAND-PASS", "HIGH-PASS"};
  const uint32_t mode = static_cast<uint32_t>(clampf(cv->params[ENVFILTER_PARAM_MODE], 0.0f, 2.0f) + 0.5f);
  cv->draw->text(cv, 6, 4, kNames[mode], O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
}

const o3m_ui kUi = {O3M_UI_SIZE(ENVFILTER_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    ENVFILTER_ENTRY_HEAD, sizeof(State), ENVFILTER_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, ENVFILTER_ENTRY_VIEWS(&kUi),
};
