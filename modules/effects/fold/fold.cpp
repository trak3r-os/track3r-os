// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): FOLD, a mono wavefolder for a track insert, module 3 of the first effect pack (docs/roadmap/BACKLOG.md OS-INSFX-FX1), after DRIVE (modules/effects/drive),
// CRUSH (modules/effects/crush) and the biquad of OS-INSFX S8 (modules/effects/biquad). All the maths is this module's own: no third-party code and no formula note (THIRD_PARTY.md), float only,
// no library call in the sample loop and no division in it (one a chunk of 16 samples).
//
//   - The folder: a triangle fold. With g the Fold gain (10^(Fold/20)) and b the Symmetry offset (|b| <= 0.9), u = g x + b, fold1(u) = u for |u| <= 1 and is reflected at +-1 beyond (a triangle wave of period 4:
//     fold1(1 + d) = 1 - d). The wet signal is (fold1(g x + b) - b) / g, so the drive into the folder is LEVEL-COMPENSATED: the signal passes unchanged until it reaches the threshold 1/g of full scale (the
//     small-signal gain is exactly 1 whatever the Fold) and folds back above it. Fold is "how far down the threshold sits", not a gain stage that makes the signal louder: DRIVE came out +12..16 dB over bypass
//     at its own defaults, and a classic fold(g x) would be g times louder on every quiet signal; here Output (up to +6 dB) is the make-up for the level a hard fold loses.
//     fold1 is 1-Lipschitz (slope +-1 everywhere) and fold1(b) = b, so |wet| <= |x| for every sample and every setting: the folder never makes a sample bigger. The Symmetry b shifts the fold's origin, so one side's
//     peaks reach the threshold first and fold (even harmonics and a DC offset, which the blocker below removes); a signal that stays under the threshold on both sides never sees it, and the wet at x = 0 is exactly 0.
//     In float: t = g x - (1 - b) = u - 1, e = t - 4 round(t / 4) (the round is the add and subtract of 1.5 * 2^25, which leaves a multiple of 4: round to nearest even, exact, no int conversion and no call; e is in
//     [-2, 2]), fold1(u) - b = (1 - b) - |e|: seven operations a sample. Exact for |t| < 2^24 (a Q15 signal times at most 15.85); a larger one degrades to a bounded value (|e| <= 6 up to 2^26, then a constant), and
//     a NaN or infinity poisons the rest of its block only (the state is cleaned at the block's end, as DRIVE's).
//   - Anti-aliasing: none of the oversampling kind. A triangle fold has slope kinks (harmonics fall at 12 dB an octave, like a triangle wave), the ones that fold back are the high ones, and a 2x oversampled fold
//     would double the sample loop and add two half-band filters (about +70 % of the block's cost [est]) for a gain that only shows on a hot, bright source at a high Fold. TONE, a one-pole low-pass AFTER the fold
//     (a += 1 - e^(-2 pi fc / fs), 500..20000 Hz, default 12 kHz), takes the top of what aliased and the buzz of the kinks, for three operations a sample. Known limit (README): it does not remove an alias that
//     landed below the cutoff.
//   - After the filter: a DC blocker (always on: y = k (x - x1) + R y1, R = 0.995, k = (1 + R) / 2, the same corner as DRIVE's, about 35 Hz at 44.1 kHz). The k makes its gain 0 at DC and exactly 1 at its peak
//     (Nyquist), never above 1, so with the tone (also <= 1) the whole wet path has an l2 gain <= 1: AT OUTPUT 0 dB THE OUTPUT RMS IS NEVER ABOVE THE INPUT'S AT ANY SETTING (the host test scans it).
//     The filters run on the unscaled fold (|fold1(u) - b| <= 1), and the 1/g is applied after them, so a moving Fold does not mix two scales in their memory.
//   - Mix and Output: out = (1 - m) og x + m og wet, m the Mix and og the output level: both folded, with 1/g, into the two coefficients cm0 = (1 - m) og and cm1 = m og / g; Mix 0 is the input bit for bit at Output 0 dB.
//   - Control rate: as DRIVE. Once per render() call (a block, 1..128 frames) the smoothed fold (dB), symmetry, tone (log2 Hz), output (dB) and mix move k = frames / 256 of the way to their targets (about 6 ms at 64
//     frames); the derived values (g, the tone coefficient, og) are recomputed only while the fold, tone or output moves (four host exp2 at most, never per sample), and the five per-sample values ramp
//     linearly from the last block's to the new ones in steps of kCtl samples; 1/g is a division once a step of 16 samples, outside the sample loop. set_param() only stores a target: a MOD parameter may be
//     called every block.
//   - Tail (OS-INSFX S2 contract): render() returns 1 while the input is live (its last sample is not 0) or the state still rings (above kIdle), and 0 when both are silent; a state below kIdle (about -140 dB)
//     is flushed to exact zero at the end of the block (no denormals, an exact end of the tail), a NaN or infinite state too.
//   - Parameters, in this order for ever: Fold (0..24 dB), Symmetry (-100..+100 %, b = 0.009 per %), Tone (500..20000 Hz), Mix (0..100 %), Output (-24..+6 dB). Fold is first on purpose: the unit bench's
//     moving-parameter row sweeps the first MOD target, so it measures the derivation (four exp2) and the sample loop, the worst case, while its settled row runs the defaults.
#include <o3module.h>

#include "fold_gen.h"

namespace {

constexpr uint32_t kCtl = 16;           // samples a coefficient step lasts inside a block
constexpr uint32_t kBlockMax = O3M_BLOCK_MAX;
constexpr float kIdle = 1e-7f;          // |state| below this on all words is silence (-140 dB of full scale: under the converter's 16 bits) and is flushed to 0
constexpr float kInsane = 1e18f;        // beyond this (or NaN) the state is reset
constexpr float kMinHz = 500.0f;
constexpr float kMaxHz = 20000.0f;
constexpr float kTwoPi = 6.2831853f;
constexpr float kLog2E = 1.4426950408889634f;
constexpr float kDbToLog2 = 0.16609640474436812f;  // log2(10)/20: 10^(db/20) = 2^(db * kDbToLog2)
constexpr float kMagic4 = 50331648.0f;  // 1.5 * 2^25: (t + kMagic4) - kMagic4 rounds t to the nearest multiple of 4 (even on a tie) for |t| < 2^24
constexpr float kBiasPerPercent = 0.009f;  // Symmetry +-100 % is b = +-0.9: 1 - b stays in 0.1..1.9, where the wet at x = 0 is exactly 0
constexpr float kDcR = 0.995f;          // the DC blocker's pole
constexpr float kDcK = 0.9975f;         // (1 + R) / 2: the blocker's peak gain is exactly 1, at Nyquist

struct State {
  const o3m_host* host;
  float rate;
  float t_fold;    // targets (set_param): fold dB, symmetry b, log2 of the tone in Hz, output dB, the mix 0..1
  float t_bias;
  float t_lfc;
  float t_out;
  float t_mix;
  float fold;      // the smoothed values the derived ones follow
  float bias;
  float lfc;
  float out_db;
  float mix;
  uint32_t dirty;  // the derived values must be computed again (the fold, tone or output moved)
  uint32_t fresh;  // no render yet: set_param snaps instead of gliding (the blob a bind applies, the defaults)
  float c[5];      // the per-sample values the last block ended on: g, 1 - b, tone a, mix, output level
  float ty;        // the tone's memory
  float dx;        // the DC blocker's memory: its last input and output
  float dy;
};
static_assert(sizeof(State) <= FOLD_STATE_BYTES, "module.json: state_bytes is too small for State");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN becomes lo
inline float absf(float x) { return __builtin_fabsf(x); }

// The derived targets from the smoothed fold, tone and output: g, the tone coefficient, the output level. Control rate, never in a sample loop (four host exp2 calls).
inline void derive(State* s, float* t) {
  t[0] = s->host->exp2_f(s->fold * kDbToLog2);                              // g = 10^(fold/20)
  float hi = s->rate * 0.45f;
  if (hi > kMaxHz) {
    hi = kMaxHz;
  }
  const float fc = clampf(s->host->exp2_f(s->lfc), kMinHz, hi);            // the tone's cutoff
  t[2] = 1.0f - s->host->exp2_f(-kTwoPi * fc / s->rate * kLog2E);           // a = 1 - e^(-2 pi fc / fs)
  t[4] = s->host->exp2_f(s->out_db * kDbToLog2);                            // og = 10^(out/20)
  s->dirty = 0;
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->rate = host->sample_rate;
  s->t_fold = fold_params[FOLD_PARAM_FOLD].def;
  s->t_bias = fold_params[FOLD_PARAM_SYMMETRY].def * kBiasPerPercent;
  s->t_lfc = host->log2_f(fold_params[FOLD_PARAM_TONE].def);
  s->t_out = fold_params[FOLD_PARAM_OUTPUT].def;
  s->t_mix = fold_params[FOLD_PARAM_MIX].def * 0.01f;
  s->fold = s->t_fold;
  s->bias = s->t_bias;
  s->lfc = s->t_lfc;
  s->out_db = s->t_out;
  s->mix = s->t_mix;
  s->fresh = 1;
  s->dirty = 1;
  float t[5];
  derive(s, t);
  t[1] = 1.0f - s->bias;
  t[3] = s->mix;
  for (uint32_t i = 0; i < 5u; ++i) {
    s->c[i] = t[i];
  }
  s->ty = 0.0f;
  s->dx = 0.0f;
  s->dy = 0.0f;
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {  // targets only: the glide and the derivation are the render's
  State* s = static_cast<State*>(raw);
  switch (id) {
    case FOLD_PARAM_FOLD: s->t_fold = clampf(value, 0.0f, 24.0f); break;
    case FOLD_PARAM_SYMMETRY: s->t_bias = clampf(value, -100.0f, 100.0f) * kBiasPerPercent; break;
    case FOLD_PARAM_TONE: s->t_lfc = s->host->log2_f(clampf(value, kMinHz, kMaxHz)); break;
    case FOLD_PARAM_MIX: s->t_mix = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    case FOLD_PARAM_OUTPUT: s->t_out = clampf(value, -24.0f, 6.0f); break;
    default: return;
  }
  if (s->fresh != 0u) {  // before the first block: the value is where the effect starts, not a glide to it
    s->fold = s->t_fold;
    s->bias = s->t_bias;
    s->lfc = s->t_lfc;
    s->mix = s->t_mix;
    s->out_db = s->t_out;
    s->dirty = 1;
    float t[5];
    derive(s, t);
    t[1] = 1.0f - s->bias;
    t[3] = s->mix;
    for (uint32_t i = 0; i < 5u; ++i) {
      s->c[i] = t[i];
    }
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// One call of 1..kBlockMax frames: the control step, then the chain in steps of kCtl samples with the five per-sample values ramping.
O3M_HOT uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  uint32_t live = 0;
  while (frames > 0u) {  // (the ABI promises at most kBlockMax; a host that gives more is served in pieces)
    const uint32_t n = frames < kBlockMax ? frames : kBlockMax;
    s->fresh = 0;
    // ---- control step ---------------------------------------------------------------------------------------------------------------------
    const float k = n >= 256u ? 1.0f : static_cast<float>(n) * (1.0f / 256.0f);
    const float df = s->t_fold - s->fold;
    const float db = s->t_bias - s->bias;
    const float dl = s->t_lfc - s->lfc;
    const float doo = s->t_out - s->out_db;
    const float dm = s->t_mix - s->mix;
    if (df != 0.0f) {
      s->fold = absf(df) < 1e-3f ? s->t_fold : s->fold + df * k;
      s->dirty = 1;
    }
    if (dl != 0.0f) {
      s->lfc = absf(dl) < 1e-3f ? s->t_lfc : s->lfc + dl * k;
      s->dirty = 1;
    }
    if (doo != 0.0f) {
      s->out_db = absf(doo) < 1e-3f ? s->t_out : s->out_db + doo * k;
      s->dirty = 1;
    }
    if (db != 0.0f) {
      s->bias = absf(db) < 1.0f / 512.0f ? s->t_bias : s->bias + db * k;
    }
    if (dm != 0.0f) {
      s->mix = absf(dm) < 1.0f / 512.0f ? s->t_mix : s->mix + dm * k;
    }
    float t[5];
    if (s->dirty != 0u) {
      derive(s, t);  // g, the tone coefficient, the output level from the moved values
    } else {
      for (uint32_t i = 0; i < 5u; ++i) {
        t[i] = s->c[i];
      }
    }
    t[1] = 1.0f - s->bias;  // the symmetry and the mix never need a derivation
    t[3] = s->mix;
    live |= in[n - 1u] != 0.0f ? 1u : 0u;  // before anything writes: in may be out
    const uint32_t chunks = (n + kCtl - 1u) / kCtl;
    const float inv = 1.0f / static_cast<float>(chunks);
    float d[5];
    for (uint32_t i = 0; i < 5u; ++i) {
      d[i] = (t[i] - s->c[i]) * inv;
    }
    // ---- the samples ----------------------------------------------------------------------------------------------------------------------
    float zty = s->ty;  // the state of the two one-poles, in registers for the whole block
    float zdx = s->dx;
    float zdy = s->dy;
    float c0 = s->c[0], c1 = s->c[1], c2 = s->c[2], c3 = s->c[3], c4 = s->c[4];
    uint32_t at = 0;
    for (uint32_t j = 0; j < chunks; ++j) {
      if (j + 1u == chunks) {  // the last step lands exactly on the target
        c0 = t[0]; c1 = t[1]; c2 = t[2]; c3 = t[3]; c4 = t[4];
      } else {
        c0 += d[0]; c1 += d[1]; c2 += d[2]; c3 += d[3]; c4 += d[4];
      }
      const float cm0 = (1.0f - c3) * c4;         // the mix with the output level folded in
      const float cm1 = c3 * c4 * (1.0f / c0);    // and the fold's own 1/g: the wet is the fold of the g-scaled signal back at the input's scale (a division a step, g >= 1)
      const uint32_t len = n - at < kCtl ? n - at : kCtl;
      for (uint32_t i = 0; i < len; ++i) {
        const float x = in[at + i];
        const float tt = c0 * x - c1;                             // u - 1 with u = g x + b, c1 = 1 - b
        const float e = tt - ((tt + kMagic4) - kMagic4);          // tt - 4 round(tt / 4): the triangle's phase, in [-2, 2]
        const float y = c1 - absf(e);                             // fold1(u) - b: u inside +-1 stays u - b, beyond it reflects; 0 at x = 0
        zty += c2 * (y - zty);                                    // the tone, a one-pole low-pass
        const float dcv = kDcK * (zty - zdx) + kDcR * zdy;        // the DC blocker, always on
        zdx = zty;
        zdy = dcv;
        out[at + i] = cm0 * x + cm1 * dcv;                        // the mix, the output level and 1/g
      }
      at += len;
    }
    s->c[0] = c0; s->c[1] = c1; s->c[2] = c2; s->c[3] = c3; s->c[4] = c4;
    // ---- the end of the block: silence is exact zero ---------------------------------------------------------------------------------------
    const float a1 = absf(zty);
    const float a2 = absf(zdx);
    const float a3 = absf(zdy);
    const bool sane = a1 <= kInsane && a2 <= kInsane && a3 <= kInsane;  // false for NaN and infinity
    if (!(sane && (a1 > kIdle || a2 > kIdle || a3 > kIdle))) {
      zty = 0.0f;
      zdx = 0.0f;
      zdy = 0.0f;
    } else {
      live = 1;
    }
    s->ty = zty;
    s->dx = zdx;
    s->dy = zdy;
    in += n;
    out += n;
    frames -= n;
  }
  return live;  // 1 while the input is live or the effect rings, 0 when both are silent (the S2 tail contract)
}

// ---- the view: the wet transfer curve (the fold, level-compensated) at the Fold and Symmetry the canvas carries -----------------------------------------
void view_draw(void* raw, uint32_t view, o3m_canvas* cv) {
  const State* s = static_cast<const State*>(raw);  // read only: the host table, which never changes after init()
  if (view != FOLD_VIEW_CURVE || cv->param_count < FOLD_PARAM_COUNT) {
    return;
  }
  const int w = cv->width;
  const int h = cv->height;
  if (w < 16 || h < 16) {
    return;
  }
  const float g = s->host->exp2_f(clampf(cv->params[FOLD_PARAM_FOLD], 0.0f, 24.0f) * kDbToLog2);
  const float b = clampf(cv->params[FOLD_PARAM_SYMMETRY], -100.0f, 100.0f) * kBiasPerPercent;
  const float c1 = 1.0f - b;
  const float ig = 1.0f / g;
  constexpr uint32_t kN = 65;  // the curve: x from -1 to 1
  int16_t xy[2 * kN];
  const int ymid = (h - 1) / 2;
  const float yscale = static_cast<float>(h - 3) / 2.0f;  // y in [-1, 1]: |wet| <= |x|
  for (uint32_t i = 0; i < kN; ++i) {
    const float x = -1.0f + 2.0f * static_cast<float>(i) / 64.0f;
    const float tt = g * x - c1;
    const float e = tt - ((tt + kMagic4) - kMagic4);
    const float y = (c1 - absf(e)) * ig;
    xy[2 * i] = static_cast<int16_t>(2 + (w - 5) * static_cast<int>(i) / 64);
    xy[2 * i + 1] = static_cast<int16_t>(ymid - static_cast<int>(y * yscale));
  }
  cv->draw->outline(cv, 1, 1, w - 2, h - 2, O3M_COLOR_EMPTY);
  cv->draw->rect(cv, 1, ymid, w - 2, 1, O3M_COLOR_EMPTY);  // the zero line
  cv->draw->polyline(cv, xy, kN, O3M_COLOR_FX);
  cv->draw->text(cv, 4, 4, b != 0.0f ? "ASYM" : "FOLD", O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
}

const o3m_ui kUi = {O3M_UI_SIZE(FOLD_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    FOLD_ENTRY_HEAD, sizeof(State), FOLD_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, FOLD_ENTRY_VIEWS(&kUi),
};
