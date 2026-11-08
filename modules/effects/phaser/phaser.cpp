// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): PHASER, a mono phaser for a track insert, module 3 of the first effect pack (docs/roadmap/BACKLOG.md OS-INSFX-FX1), after DRIVE (modules/effects/drive)
// and CRUSH (modules/effects/crush). All the maths is this module's own: no third-party code and no formula note (THIRD_PARTY.md), float only, no division and no library call in the sample loop.
//
//   - The chain: Stages (4, 5 or 6) identical first-order all-pass sections in the normalised lattice form, y = a u + c z, z' = c u - a z with c = sqrt(1 - a^2): H(z) = (a + 1/z) / (1 + a/z),
//     |H| = 1 at every frequency and a phase that falls from 0 at DC to -180 degrees at Nyquist, -90 degrees at the section's own frequency f. The map (u, z) -> (y, z') is the reflection
//     [a c; c -a], orthogonal when a^2 + c^2 = 1: **a section can never gain, whatever the coefficient does**, which the cheaper two-multiplier forms cannot promise while the coefficient moves (the transposed
//     direct form gained 0.46 dB on a 150 Hz tone at the fastest full sweep at Centre 100 Hz, measured: 4 multiplies and 2 adds a section instead of 2 and 2 buy the exact level law). c is shrunk by 1e-6
//     so that a^2 + c^2 is at most 1 in float, and the linear ramp of (a, c) between two blocks is a chord of the circle (inside it): still no gain. The coefficient is the bilinear one,
//     a = (w - 1) / (w + 1) with w = tan(pi f / fs); tan is the Pade [3/2] x (15 - x^2) / (15 - 6 x^2) (error 2e-5 of the value for f up to 0.17 fs), so a = (P - Q) / (P + Q) with P = x (15 - x^2) and
//     Q = 15 - 6 x^2: one division and one square root (VSQRT: the packer builds -fno-math-errno), once a block. A notch is where the chain's phase is an odd multiple of -180 degrees:
//     N sections give N/2 notches (2 for 4, 3 for 6, and an odd N one exactly at Nyquist).
//   - The sweep: a sine-like LFO (a triangle through the smoothing p(t) = t (3 - t^2) / 2, which is flat at its peaks), the Period in ms (a unit parameter shows whole numbers, so the LFO is a period,
//     not a rate in Hz), moves the frequency f = Centre 2^(2 Depth LFO) (Depth 0..1, so up to two octaves each way), clamped to 40 Hz .. 0.17 fs. Once a block (1..128 frames) the module evaluates the
//     LFO, takes one host exp2 (only while Depth is over 0), one division and one square root, and the section coefficients ramp linearly sample by sample from the last block's to the new ones (no zipper).
//   - The feedback: the chain's output goes back to its input one sample late, scaled by g (Feedback, -90..+90 %, so |g| <= 0.9). The input is scaled by (1 - |g|) first, so the wet path is
//     W = (1 - |g|) A / (1 - g A / z) with |A| = 1: since |1 - g A / z| >= 1 - |g|, **|W| <= 1 at every frequency** and the output out = (1 - m) x + m W x has |H| <= (1 - m) + m = 1: at any FIXED setting the
//     effect can never be louder than bypass (the level law DRIVE failed at its defaults, +12 to +16 dB; tested on sines and noise at the defaults and over the whole parameter grid, the fastest sweep included).
//     The bound is per sample and takes the g of that sample in the loop equation: Feedback is a MOD target and glides, so while g moves the loop is time-varying and the law does not hold sample-wise -- an impulse
//     charged while g is near 0 and then a fast rise of Feedback recirculates the energy the sections stored, measured up to about 5x the input peak in the test; the hard clamp of the fed-back sample (below) still
//     bounds the signal. With g fixed the law also holds while the coefficient moves: the chain is a contraction in energy at every sample (above), the delay keeps the norm, so the loop's gain in energy is at most
//     gi / (1 - |g|) = 1.
//   - It cannot blow up: |g| <= 0.9 and the loop is a contraction, so it is stable as a filter; and, independent of the maths, the fed-back sample is hard-clamped to +-4. Each section maps the pair
//     (input, state) to (output, new state) without growing its norm, so for any finite input the states and outputs stay finite and no larger than the root of the summed squares of the input and the
//     clamped feedback (a coefficient schedule included: no assumption on the LFO); the clamp never acts on a signal in range. A NaN or an infinity in the INPUT (the OS cannot send one: its adapter
//     is Q15) poisons the sections for the rest of that block, and is flushed (all state to 0) at its end.
//   - Control: Centre, Depth, Feedback and Mix are targets; once a block they move k = frames / 256 of the way (about 6 ms at 64 frames) and the Period and Stages act at once. set_param does
//     almost nothing (a clamp, a store, one division for Period): a MOD parameter may be called every block. The first parameters after init() (the stored blob) are applied without a glide.
//     A change of Stages is a jump (the response changes shape at once): not a modulation target; the sections that drop out are cleared so that they restart from rest.
//   - Tail (OS-INSFX S2 contract): render() returns 1 while the input is live (its last sample is not 0) or the sections ring (the sum of |state| is at least 1e-5, a third of a Q15 step), and 0
//     when both are silent; the sections are then cleared (exact zeros afterwards). A ring can be long: the coefficient is near -1 at low frequencies (40 Hz: a = -0.994), so with high feedback a
//     tail at the sweep's low end lasts seconds (measured in the test).
//   - Parameters, in this order for ever: Stages (4..6), Period (100..20000 ms), Depth (0..100 %), Centre (100..4000 Hz), Feedback (-90..+90 %), Mix (0..100 %).
#include <o3module.h>

#include "phaser_gen.h"

namespace {

constexpr uint32_t kBlockMax = O3M_BLOCK_MAX;
constexpr float kPi = 3.14159265358979f;
constexpr float kFbMax = 0.9f;       // |g| <= 0.9: the bound that makes the wet path |W| <= 1 and the loop a contraction
constexpr float kFbClamp = 4.0f;     // the fed-back sample never leaves +-4: the proof of boundedness does not rest on the filter maths
constexpr float kInsane = 1e18f;     // a state sum beyond this (or NaN, or infinite) is flushed at the end of the block
constexpr float kRingEnd = 1e-5f;    // a sum of |state| below this, with a silent input, is the end of the tail
constexpr float kFMin = 40.0f;       // the sweep's floor in Hz
constexpr float kFMaxOfFs = 0.17f;   // and its ceiling: x = pi f / fs stays under 0.54, where the Pade tan is good to 2e-5
constexpr float kOctaves = 2.0f;     // the sweep at Depth 100 %: +-2 octaves around Centre
constexpr float kLfoStart = 0.25f;   // the LFO starts at Centre, going down
constexpr float kShrink = 0.999999f; // c = sqrt(1 - a^2) kShrink: a^2 + c^2 stays at or under 1 in float

struct State {
  const o3m_host* host;
  float fs;
  float pi_fs;      // pi / fs
  float f_max;      // kFMaxOfFs fs
  float lfo_inc;    // LFO cycles a sample: 1 / (Period fs)
  float ph;         // the LFO phase, 0..1
  float t_cen;      // targets (set_param): Centre in Hz, Depth 0..1, the feedback g, the mix 0..1
  float t_dep;
  float t_fb;
  float t_mix;
  float cen;        // the smoothed values
  float dep;
  float fb_v;
  float mix;
  float c[4];       // the per-sample values the last block ended on: the coefficients a and c, the feedback g and the mix
  float z[6];       // the sections' states
  float fbs;        // the fed-back sample
  uint32_t stages;  // 4..6
  uint32_t fresh;   // no render yet: set_param snaps instead of gliding
};
static_assert(sizeof(State) <= PHASER_STATE_BYTES, "module.json: state_bytes is too small for State");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN becomes lo
inline float absf(float x) { return __builtin_fabsf(x); }

// tan(x) for x = pi f / fs under 0.54 (f up to 0.17 fs): the Pade [3/2] as a ratio P / Q, x (15 - x^2) / (15 - 6 x^2), good to 2e-5 of the value.
inline float tan_of(float x) {
  const float x2 = x * x;
  return x * (15.0f - x2) / (15.0f - 6.0f * x2);
}

// The all-pass coefficient of a section at f Hz: the bilinear a = (w - 1) / (w + 1) with w = tan(pi f / fs) = P / Q, so a = (P - Q) / (P + Q) with P = x (15 - x^2) and Q = 15 - 6 x^2: ONE division.
inline float coef_of(float f, float pi_fs) {
  const float x = f * pi_fs;
  const float x2 = x * x;
  const float p = x * (15.0f - x2);
  const float q = 15.0f - 6.0f * x2;
  return (p - q) / (p + q);
}

// One first-order all-pass section in the normalised lattice form: y = a u + c z, z' = c u - a z, c = sqrt(1 - a^2) (the reflection [a c; c -a]: it never gains). Inlined into the sample loop, z in a register.
inline __attribute__((always_inline)) float section(float a, float c, float& z, float u) {
  const float y = a * u + c * z;
  z = c * u - a * z;
  return y;
}

inline float glide(float v, float t, float k, float eps) {
  const float d = t - v;
  return d == 0.0f ? v : (absf(d) < eps ? t : v + d * k);
}

void snap(State* s) {  // before the first block the values are where the effect starts, not a glide to them
  s->cen = s->t_cen;
  s->dep = s->t_dep;
  s->fb_v = s->t_fb;
  s->mix = s->t_mix;
  s->c[2] = s->fb_v;
  s->c[3] = s->mix;
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->fs = host->sample_rate;
  s->pi_fs = kPi / host->sample_rate;
  s->f_max = kFMaxOfFs * host->sample_rate;
  s->lfo_inc = 1000.0f / (phaser_params[PHASER_PARAM_PERIOD].def * host->sample_rate);
  s->ph = kLfoStart;
  s->t_cen = phaser_params[PHASER_PARAM_CENTRE].def;
  s->t_dep = phaser_params[PHASER_PARAM_DEPTH].def * 0.01f;
  s->t_fb = phaser_params[PHASER_PARAM_FEEDBACK].def * 0.01f;
  s->t_mix = phaser_params[PHASER_PARAM_MIX].def * 0.01f;
  for (float& v : s->z) {
    v = 0.0f;
  }
  s->fbs = 0.0f;
  s->c[0] = 0.0f;
  s->c[1] = 0.0f;
  s->stages = static_cast<uint32_t>(phaser_params[PHASER_PARAM_STAGES].def);
  s->fresh = 1;
  snap(s);
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  switch (id) {
    case PHASER_PARAM_STAGES: {
      const uint32_t n = static_cast<uint32_t>(clampf(value, 4.0f, 6.0f) + 0.5f);
      for (uint32_t i = n; i < 6u; ++i) {  // a section that drops out restarts from rest when it comes back
        s->z[i] = 0.0f;
      }
      s->stages = n;
      return;
    }
    case PHASER_PARAM_PERIOD: s->lfo_inc = 1000.0f / (clampf(value, 100.0f, 20000.0f) * s->fs); return;  // (a division at most)
    case PHASER_PARAM_DEPTH: s->t_dep = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    case PHASER_PARAM_CENTRE: s->t_cen = clampf(value, 100.0f, 4000.0f); break;
    case PHASER_PARAM_FEEDBACK: s->t_fb = clampf(value, -90.0f, 90.0f) * 0.01f; break;
    case PHASER_PARAM_MIX: s->t_mix = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    default: return;
  }
  if (s->fresh != 0u) {
    snap(s);
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// The chain over n samples with the coefficients, the feedback and the mix ramping linearly (a, c, g, m plus their steps): every section's state in a register for the whole block.
template <uint32_t N>
inline __attribute__((always_inline)) void chain(State* s, const float* in, float* out, uint32_t n, float a, float da, float c, float dc, float g, float dg, float m, float dm) {
  float z0 = s->z[0], z1 = s->z[1], z2 = s->z[2], z3 = s->z[3], z4 = s->z[4], z5 = s->z[5];
  float fb = s->fbs;
  for (uint32_t i = 0; i < n; ++i) {
    a += da;
    c += dc;
    g += dg;
    m += dm;
    const float x = in[i];  // read before writing: in may be out
    float u = (1.0f - absf(g)) * x + g * fb;
    u = section(a, c, z0, u);
    u = section(a, c, z1, u);
    u = section(a, c, z2, u);
    u = section(a, c, z3, u);
    if (N >= 5u) {
      u = section(a, c, z4, u);
    }
    if (N >= 6u) {
      u = section(a, c, z5, u);
    }
    fb = clampf(u, -kFbClamp, kFbClamp);
    out[i] = x + m * (u - x);
  }
  s->z[0] = z0;
  s->z[1] = z1;
  s->z[2] = z2;
  s->z[3] = z3;
  s->z[4] = z4;
  s->z[5] = z5;
  s->fbs = fb;
}

// One call of 1..kBlockMax frames: the control step (smoothing, LFO, coefficient), the chain, then the tail check.
O3M_HOT uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  uint32_t live = 0;
  while (frames > 0u) {  // (the ABI promises at most kBlockMax; a host that gives more is served in pieces)
    const uint32_t n = frames < kBlockMax ? frames : kBlockMax;
    const float nf = static_cast<float>(n);
    const float in_last = in[n - 1u];  // before anything writes: in may be out
    // ---- control step ---------------------------------------------------------------------------------------------------------------------
    const float k = nf * (1.0f / 256.0f);
    s->cen = glide(s->cen, s->t_cen, k, 0.05f);
    s->dep = glide(s->dep, s->t_dep, k, 1.0f / 512.0f);
    s->fb_v = glide(s->fb_v, s->t_fb, k, 1.0f / 512.0f);
    s->mix = glide(s->mix, s->t_mix, k, 1.0f / 512.0f);
    float ph = s->ph + s->lfo_inc * nf;
    ph -= ph >= 1.0f ? 1.0f : 0.0f;
    s->ph = ph;
    const float t = 4.0f * absf(ph - 0.5f) - 1.0f;  // a triangle -1..1, smoothed into a sine-like wave flat at its peaks
    const float lfo = t * (1.5f - 0.5f * t * t);
    float f = s->cen;
    if (s->dep != 0.0f) {
      f *= s->host->exp2_f(kOctaves * s->dep * lfo);
    }
    f = clampf(f, kFMin, s->f_max);
    const float at = coef_of(f, s->pi_fs);
    const float ct = __builtin_sqrtf(1.0f - at * at) * kShrink;  // |at| < 1: the root of a positive number
    if (s->fresh != 0u) {
      s->c[0] = at;  // the first block starts at its own coefficients
      s->c[1] = ct;
      s->fresh = 0;
    }
    const float invn = 1.0f / nf;
    const float a0 = s->c[0], c0 = s->c[1], g0 = s->c[2], m0 = s->c[3];
    const float gt = s->fb_v, mt = s->mix;
    const float da = (at - a0) * invn, dc = (ct - c0) * invn, dg = (gt - g0) * invn, dm = (mt - m0) * invn;
    // ---- the samples ----------------------------------------------------------------------------------------------------------------------
    switch (s->stages) {
      case 4u: chain<4u>(s, in, out, n, a0, da, c0, dc, g0, dg, m0, dm); break;
      case 5u: chain<5u>(s, in, out, n, a0, da, c0, dc, g0, dg, m0, dm); break;
      default: chain<6u>(s, in, out, n, a0, da, c0, dc, g0, dg, m0, dm); break;
    }
    s->c[0] = at;
    s->c[1] = ct;
    s->c[2] = gt;
    s->c[3] = mt;
    // ---- the tail: a NaN or an absurd state is flushed; a silent input with the sections at rest ends the tail --------------------------------
    const float sum = absf(s->z[0]) + absf(s->z[1]) + absf(s->z[2]) + absf(s->z[3]) + absf(s->z[4]) + absf(s->z[5]) + absf(s->fbs);  // (a NaN or an infinity stays in the sum)
    if (!(sum <= kInsane) || (in_last == 0.0f && sum < kRingEnd)) {
      for (float& v : s->z) {
        v = 0.0f;
      }
      s->fbs = 0.0f;
    } else if (sum >= kRingEnd) {
      live = 1u;
    }
    live |= in_last != 0.0f ? 1u : 0u;
    in += n;
    out += n;
    frames -= n;
  }
  return live;  // 1 while the input is live or the sections ring, 0 when both are silent (the S2 tail contract)
}

// ---- the view: the response over the input (the notches) at the Centre, Stages, Feedback and Mix the canvas carries, 20 Hz to 0.17 fs on a log axis, power from 1 (top) to 0 (bottom) ----------
// It draws from the canvas's parameters only (no instance state, no host call, no trigonometry): a section's response is H = (1 - j r) / (1 + j r) with r = tan(pi f / fs) / tan(pi f_c / fs)
// (the bilinear form of the module's own coefficient: -90 degrees at r = 1, the Centre), the Pade tan again, the chain A = H^Stages, the feedback's sample delay D = e^-jw = (1 - j t) / (1 + j t) with
// t = tan(pi f / fs) (the same form), W = (1 - |g|) A / (1 - g A D), out / in = (1 - m) + m W.
// The sample rate is the OS's 44.1 kHz: it only places the axis.
const char* const kStagesName[3] = {"4 STAGES", "5 STAGES", "6 STAGES"};

void view_draw(void*, uint32_t view, o3m_canvas* cv) {
  if (view != PHASER_VIEW_NOTCHES || cv->param_count < PHASER_PARAM_COUNT) {
    return;
  }
  const int w = cv->width;
  const int h = cv->height;
  if (w < 16 || h < 16) {
    return;
  }
  constexpr float kViewPiFs = kPi / 44100.0f;
  constexpr float kAxisStep = 1.0970f;  // 20 Hz * 1.097^64 = 7.5 kHz = 0.17 * 44100
  const uint32_t ns = static_cast<uint32_t>(clampf(cv->params[PHASER_PARAM_STAGES], 4.0f, 6.0f) + 0.5f);
  const float g = clampf(cv->params[PHASER_PARAM_FEEDBACK], -90.0f, 90.0f) * 0.01f;
  const float m = clampf(cv->params[PHASER_PARAM_MIX], 0.0f, 100.0f) * 0.01f;
  const float gi = 1.0f - absf(g);
  const float inv_wc = 1.0f / tan_of(clampf(cv->params[PHASER_PARAM_CENTRE], 100.0f, 4000.0f) * kViewPiFs);
  constexpr uint32_t kN = 65;  // the curve: 65 frequencies
  int16_t xy[2 * kN];
  const float span = static_cast<float>(h - 5);  // power 1 at y = 2, 0 at y = h - 3
  float f = 20.0f;
  for (uint32_t i = 0; i < kN; ++i) {
    const float tf = tan_of(f * kViewPiFs);
    const float r = tf * inv_wc;
    const float d = 1.0f / (1.0f + r * r);
    const float rr = (1.0f - r * r) * d, ri = -2.0f * r * d;  // one section, |.| = 1
    float ar = 1.0f, ai = 0.0f;
    for (uint32_t j = 0; j < ns; ++j) {                       // the chain: the section to the power Stages
      const float tr = ar * rr - ai * ri;
      ai = ar * ri + ai * rr;
      ar = tr;
    }
    const float dt = 1.0f / (1.0f + tf * tf);                 // the feedback's one-sample delay, e^-jw = (1 - j t) / (1 + j t)
    const float er = (1.0f - tf * tf) * dt, ei = -2.0f * tf * dt;
    const float lr = ar * er - ai * ei, li = ar * ei + ai * er;  // A D
    const float dr = 1.0f - g * lr, di = -g * li;             // 1 - g A D
    const float dd = gi / (dr * dr + di * di);
    const float wr = (ar * dr + ai * di) * dd;                // W = gi A / (1 - g A D)
    const float wi = (ai * dr - ar * di) * dd;
    const float orr = (1.0f - m) + m * wr, oi = m * wi;       // the output over the input
    float p = orr * orr + oi * oi;
    p = p < 1.0f ? p : 1.0f;
    xy[2 * i] = static_cast<int16_t>(2 + (w - 5) * static_cast<int>(i) / 64);
    xy[2 * i + 1] = static_cast<int16_t>(2.0f + (1.0f - p) * span);
    f *= kAxisStep;
  }
  cv->draw->outline(cv, 1, 1, w - 2, h - 2, O3M_COLOR_EMPTY);
  cv->draw->polyline(cv, xy, kN, O3M_COLOR_FX);
  cv->draw->text(cv, 4, 4, kStagesName[ns - 4u], O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
}

const o3m_ui kUi = {O3M_UI_SIZE(PHASER_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    PHASER_ENTRY_HEAD, sizeof(State), PHASER_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, PHASER_ENTRY_VIEWS(&kUi),
};
