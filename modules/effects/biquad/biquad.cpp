// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX S8 (new Community OS behaviour): the first sample effect module, a resonant biquad filter with a dry/wet mix for a track insert (SDK guide: module/sdk/README.md).
//
// The maths is the Audio EQ Cookbook's (Robert Bristow-Johnson, a published note of formulas): low-pass, high-pass, band-pass (constant 0 dB peak) and notch, written here from the formulas;
// no code of the note or of any library is used (THIRD_PARTY.md, "RBJ Audio EQ Cookbook"). Structure, smoothing, mix folding and tail rule are this module's own.
//
//   - One transposed direct form II section: y = b0 x + z1; z1 = b1 x - a1 y + z2; z2 = b2 x - a2 y. Float only, no division and no library call in the sample loop.
//   - The dry/wet mix is folded into the numerator: out = (1 - m) x + m H(z) x = ((1 - m) A + m B) / A x, so the effect is ONE biquad whatever the mix, with the denominator of the filter. Mix 0 is
//     the identity (b = 1, a1, a2 over the same A: bit for bit while the state is zero). The state keeps ringing under that identity (y = x + z1), so once the mix has settled at 0 render() fades the
//     ring out over one block (out = x + g z1, g 1 to 0), zeroes z1 and z2 and keeps them zero while the mix is 0: Mix 0 is the dry track, a plain copy, bit for bit.
//   - Control rate: once per render() call (a block, 1..128 frames) the smoothed cutoff (log2 Hz), resonance (log2 Q) and mix move a fraction of the way to their targets (k = frames / 256, about 6 ms
//     at 64 frames), the prototype (b, a1, a2) is rederived only while cutoff, resonance or mode move (two host trig calls and two exp2), and the five coefficients ramp linearly from the last
//     block's to the new ones in steps of kCtl samples. The poles of every mode are the same, so a mode change is a crossfade of numerators (the old and the new prototype
//     blended by xf, which moves to 1 at the same rate as the other smoothed values: about 25 ms, no step) and a ramp of denominators stays stable (the stability triangle is convex). set_param() only stores a target: a MOD parameter may be called every block.
//   - Settled: the coefficients are the same every block and the loop is the same code (no branch in it).
//   - Tail (OS-INSFX S2 contract): render() returns 1 while the input is live (its last sample is not 0) or the filter state still rings (above kIdle), and 0 when both are silent; a state below kIdle
//     (about -140 dB) is flushed to exact zero at the end of the block (no denormals, an exact end of the tail), a NaN or infinite state too.
//   - Parameters, in this order for ever: Mode (0 low-pass, 1 high-pass, 2 band-pass, 3 notch), Cutoff (20..20000 Hz), Resonance (0..100 %, Q = 0.5 * 2^(r / 20): 0.5 at 0, 0.707 at 10 (the default,
//     Butterworth), 16 at 100), Mix (0..100 %).
#include <o3module.h>

#include "biquad_gen.h"

namespace {

constexpr uint32_t kCtl = 16;           // samples a coefficient step lasts inside a block
constexpr uint32_t kBlockMax = O3M_BLOCK_MAX;
constexpr float kIdle = 1e-7f;          // |state| below this on both words is silence (-140 dB of full scale: under the converter's 16 bits) and is flushed to 0
constexpr float kInsane = 1e18f;        // beyond this (or NaN) the state is reset
constexpr float kMinHz = 20.0f;
constexpr float kMaxHz = 20000.0f;
constexpr float kTwoPi = 6.2831853f;

struct State {
  const o3m_host* host;
  float rate;
  float t_lfc;     // targets (set_param): log2 of the cutoff in Hz, log2 of Q, the mix 0..1, the mode
  float t_lq;
  float t_mix;
  uint32_t mode;
  uint32_t mode_old;  // the mode before the last change, and how far the numerator has moved from it to `mode` (1 = arrived): a mode change is a crossfade of numerators
  float xf;
  float lfc;       // the smoothed values the coefficients follow
  float lq;
  float mix;
  uint32_t dirty;  // the prototype must be derived again (a smoothed value or the mode moved)
  uint32_t fresh;  // no render yet: set_param snaps instead of gliding (the blob a bind applies, the defaults)
  float pb[3];     // the prototype: numerator b0..b2 and denominator a1, a2 of the filter alone (a0 = 1)
  float pa[2];
  float c[5];      // the coefficients the last block ended on: b0, b1, b2, a1, a2 (the mix folded in)
  float z1;
  float z2;
};
static_assert(sizeof(State) <= BIQUAD_STATE_BYTES, "module.json: state_bytes is too small for State");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN becomes lo
inline float absf(float x) { return __builtin_fabsf(x); }

// The prototype of a mode at cutoff 2^lfc Hz and Q 2^lq: the cookbook's formulas in the half-angle (1 - cos w = 2 sin^2(w/2), 1 + cos w = 2 cos^2(w/2)), because the host's cos has an absolute error of
// 1e-6, which at 20 Hz (1 - cos w = 4e-6) would move the cutoff by 10 %; sin(w/2) has a relative one. Control rate, never in a sample loop.
struct Proto {
  float b[3];
  float a[2];
};

Proto prototype(const o3m_host* host, float rate, uint32_t mode, float lfc, float lq) {
  const float fc = clampf(host->exp2_f(lfc), kMinHz, rate * 0.45f);
  const float h = fc * (kTwoPi * 0.5f) / rate;  // w0 / 2
  const float sh = host->sin_f(h);
  const float ch = host->cos_f(h);
  const float s2 = sh * ch * 2.0f;              // sin w0
  const float alpha = s2 * 0.5f * host->exp2_f(-lq);  // sin w0 / (2 Q)
  const float cosw = 1.0f - 2.0f * sh * sh;
  const float inv = 1.0f / (1.0f + alpha);      // 1 / a0
  Proto p;
  switch (mode) {
    case 1: p.b[0] = ch * ch; p.b[1] = -2.0f * ch * ch; p.b[2] = ch * ch; break;  // high-pass: (1 + cos w) / 2, -(1 + cos w), (1 + cos w) / 2
    case 2: p.b[0] = alpha; p.b[1] = 0.0f; p.b[2] = -alpha; break;                // band-pass, constant 0 dB peak
    case 3: p.b[0] = 1.0f; p.b[1] = -2.0f * cosw; p.b[2] = 1.0f; break;           // notch
    default: p.b[0] = sh * sh; p.b[1] = 2.0f * sh * sh; p.b[2] = sh * sh; break;  // low-pass: (1 - cos w) / 2, 1 - cos w, (1 - cos w) / 2
  }
  p.b[0] *= inv;
  p.b[1] *= inv;
  p.b[2] *= inv;
  p.a[0] = -2.0f * cosw * inv;
  p.a[1] = (1.0f - alpha) * inv;
  return p;
}

// The five coefficients with the mix folded in: numerator (1 - m) A + m B over the filter's own denominator.
inline void fold(const State* s, float m, float* t) {
  const float d = 1.0f - m;
  t[0] = d + m * s->pb[0];
  t[1] = d * s->pa[0] + m * s->pb[1];
  t[2] = d * s->pa[1] + m * s->pb[2];
  t[3] = s->pa[0];
  t[4] = s->pa[1];
}

void derive(State* s) {
  const Proto p = prototype(s->host, s->rate, s->mode, s->lfc, s->lq);
  s->pb[0] = p.b[0];
  s->pb[1] = p.b[1];
  s->pb[2] = p.b[2];
  if (s->xf < 1.0f) {  // the same poles in every mode: only the numerator differs, so a blend of the two numerators is a crossfade of the two outputs
    const Proto o = prototype(s->host, s->rate, s->mode_old, s->lfc, s->lq);
    for (uint32_t i = 0; i < 3u; ++i) {
      s->pb[i] = o.b[i] + s->xf * (p.b[i] - o.b[i]);
    }
  }
  s->pa[0] = p.a[0];
  s->pa[1] = p.a[1];
  s->dirty = 0;
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->rate = host->sample_rate;
  s->t_lfc = host->log2_f(biquad_params[BIQUAD_PARAM_CUTOFF].def);
  s->t_lq = -1.0f + biquad_params[BIQUAD_PARAM_RESONANCE].def * 0.05f;
  s->t_mix = biquad_params[BIQUAD_PARAM_MIX].def * 0.01f;
  s->mode = static_cast<uint32_t>(biquad_params[BIQUAD_PARAM_MODE].def);
  s->mode_old = s->mode;
  s->xf = 1.0f;
  s->lfc = s->t_lfc;
  s->lq = s->t_lq;
  s->mix = s->t_mix;
  s->fresh = 1;
  derive(s);
  fold(s, s->mix, s->c);
  s->z1 = 0.0f;
  s->z2 = 0.0f;
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {  // targets only: the glide and the derivation are the render's
  State* s = static_cast<State*>(raw);
  switch (id) {
    case BIQUAD_PARAM_MODE: {
      const uint32_t m = static_cast<uint32_t>(clampf(value, 0.0f, 3.0f) + 0.5f);
      if (m != s->mode) {
        s->mode_old = s->mode;  // known limit: a second change while a crossfade is under way starts from `mode` (the unfinished target), not from the blend, so the numerator steps once (documented in the README)
        s->mode = m;
        s->xf = s->fresh != 0u ? 1.0f : 0.0f;  // before the first block the mode simply is; later it fades in
        s->dirty = 1;
      }
      break;
    }
    case BIQUAD_PARAM_CUTOFF: s->t_lfc = s->host->log2_f(clampf(value, kMinHz, kMaxHz)); break;
    case BIQUAD_PARAM_RESONANCE: s->t_lq = -1.0f + clampf(value, 0.0f, 100.0f) * 0.05f; break;
    case BIQUAD_PARAM_MIX: s->t_mix = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    default: return;
  }
  if (s->fresh != 0u) {  // before the first block: the value is where the filter starts, not a glide to it
    s->lfc = s->t_lfc;
    s->lq = s->t_lq;
    s->mix = s->t_mix;
    s->dirty = 1;
    derive(s);
    fold(s, s->mix, s->c);
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// One call of 1..kBlockMax frames: the control step, then the biquad in steps of kCtl samples with the coefficients ramping.
O3M_HOT uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  uint32_t live = 0;
  while (frames > 0u) {  // (the ABI promises at most kBlockMax; a host that gives more is served in pieces)
    const uint32_t n = frames < kBlockMax ? frames : kBlockMax;
    s->fresh = 0;
    // ---- control step ---------------------------------------------------------------------------------------------------------------------
    const float k = n >= 256u ? 1.0f : static_cast<float>(n) * (1.0f / 256.0f);
    const float dl = s->t_lfc - s->lfc;
    const float dq = s->t_lq - s->lq;
    const float dm = s->t_mix - s->mix;
    if (dl != 0.0f) {
      s->lfc = absf(dl) < 1e-3f ? s->t_lfc : s->lfc + dl * k;
      s->dirty = 1;
    }
    if (dq != 0.0f) {
      s->lq = absf(dq) < 1e-3f ? s->t_lq : s->lq + dq * k;
      s->dirty = 1;
    }
    if (s->xf < 1.0f) {
      s->xf = 1.0f - s->xf < 1.0f / 512.0f ? 1.0f : s->xf + (1.0f - s->xf) * k;
      s->dirty = 1;
    }
    if (dm != 0.0f) {
      s->mix = absf(dm) < 1.0f / 512.0f ? s->t_mix : s->mix + dm * k;
    }
    if (s->dirty != 0u) {
      derive(s);
    }
    float t[5];
    fold(s, s->mix, t);
    live |= in[n - 1u] != 0.0f ? 1u : 0u;  // before anything writes: in may be out
    if (s->mix == 0.0f) {  // settled dry (the glide snaps to exactly 0): the numerator is the identity, which keeps the state ringing in y = x + z1 for seconds at a high Q, so the ring is faded out
      float z1 = s->z1;  // over this one block (out = x + g z1, g from 1 to 0, z1 the free ring of the denominator), then the state is 0 and stays 0: the output is the input bit for bit
      float z2 = s->z2;
      if (absf(z1) <= kInsane && absf(z2) <= kInsane && (z1 != 0.0f || z2 != 0.0f)) {
        const float step = 1.0f / static_cast<float>(n);
        for (uint32_t i = 0; i < n; ++i) {
          const float g = static_cast<float>(n - 1u - i) * step;  // (n-1)/n down to exactly 0 at the last sample
          const float x = in[i];
          out[i] = x + g * z1;
          const float zn = z2 - t[3] * z1;
          z2 = -t[4] * z1;
          z1 = zn;
        }
        live = 1;  // the ring was audible in this block
      } else if (in != out) {
        for (uint32_t i = 0; i < n; ++i) {
          out[i] = in[i];
        }
      }
      s->z1 = 0.0f;
      s->z2 = 0.0f;
      for (uint32_t i = 0; i < 5u; ++i) {  // the next block ramps from the identity of the current poles
        s->c[i] = t[i];
      }
      in += n;
      out += n;
      frames -= n;
      continue;
    }
    const uint32_t chunks = (n + kCtl - 1u) / kCtl;
    const float inv = 1.0f / static_cast<float>(chunks);
    float d[5];
    for (uint32_t i = 0; i < 5u; ++i) {
      d[i] = (t[i] - s->c[i]) * inv;
    }
    // ---- the samples ----------------------------------------------------------------------------------------------------------------------
    float z1 = s->z1;
    float z2 = s->z2;
    float c0 = s->c[0], c1 = s->c[1], c2 = s->c[2], c3 = s->c[3], c4 = s->c[4];
    uint32_t at = 0;
    for (uint32_t j = 0; j < chunks; ++j) {
      if (j + 1u == chunks) {  // the last step lands exactly on the target
        c0 = t[0]; c1 = t[1]; c2 = t[2]; c3 = t[3]; c4 = t[4];
      } else {
        c0 += d[0]; c1 += d[1]; c2 += d[2]; c3 += d[3]; c4 += d[4];
      }
      const uint32_t len = n - at < kCtl ? n - at : kCtl;
      for (uint32_t i = 0; i < len; ++i) {
        const float x = in[at + i];
        const float y = c0 * x + z1;
        z1 = c1 * x - c3 * y + z2;
        z2 = c2 * x - c4 * y;
        out[at + i] = y;
      }
      at += len;
    }
    s->c[0] = c0; s->c[1] = c1; s->c[2] = c2; s->c[3] = c3; s->c[4] = c4;
    // ---- the end of the block: silence is exact zero ---------------------------------------------------------------------------------------
    const float a1 = absf(z1);
    const float a2 = absf(z2);
    const bool sane = a1 <= kInsane && a2 <= kInsane;  // false for NaN and infinity
    if (!(sane && (a1 > kIdle || a2 > kIdle))) {
      z1 = 0.0f;
      z2 = 0.0f;
    } else {
      live = 1;
    }
    s->z1 = z1;
    s->z2 = z2;
    in += n;
    out += n;
    frames -= n;
  }
  return live;  // 1 while the input is live or the filter rings, 0 when both are silent (the S2 tail contract)
}

// ---- the view: the magnitude response in dB, from the values the canvas carries (the mode, the cutoff, the resonance and the mix) ------------------------------------------------------------------
void view_draw(void* raw, uint32_t view, o3m_canvas* cv) {
  const State* s = static_cast<const State*>(raw);  // read only: the host table and the rate, which never change after init()
  if (view != BIQUAD_VIEW_RESPONSE || cv->param_count < BIQUAD_PARAM_COUNT) {
    return;
  }
  const o3m_host* host = s->host;
  const uint32_t mode = static_cast<uint32_t>(clampf(cv->params[BIQUAD_PARAM_MODE], 0.0f, 3.0f) + 0.5f);
  const float lfc = host->log2_f(clampf(cv->params[BIQUAD_PARAM_CUTOFF], kMinHz, kMaxHz));
  const float lq = -1.0f + clampf(cv->params[BIQUAD_PARAM_RESONANCE], 0.0f, 100.0f) * 0.05f;
  const float m = clampf(cv->params[BIQUAD_PARAM_MIX], 0.0f, 100.0f) * 0.01f;
  const Proto p = prototype(host, s->rate, mode, lfc, lq);
  const float d = 1.0f - m;
  const float n0 = d + m * p.b[0];
  const float n1 = d * p.a[0] + m * p.b[1];
  const float n2 = d * p.a[1] + m * p.b[2];
  float f[40];
  float db[40];
  const float step = host->log2_f(kMaxHz / kMinHz) / 39.0f;
  for (uint32_t i = 0; i < 40; ++i) {
    f[i] = kMinHz * host->exp2_f(step * static_cast<float>(i));
    const float w = kTwoPi * f[i] / s->rate;
    const float c1 = host->cos_f(w), s1 = host->sin_f(w);  // z^-1 = c1 - j s1, z^-2 = c2 - j s2
    const float c2 = host->cos_f(2.0f * w), s2 = host->sin_f(2.0f * w);
    const float nr = n0 + n1 * c1 + n2 * c2, ni = -(n1 * s1 + n2 * s2);
    const float dr = 1.0f + p.a[0] * c1 + p.a[1] * c2, di = -(p.a[0] * s1 + p.a[1] * s2);
    const float nn = nr * nr + ni * ni, dd = dr * dr + di * di;
    db[i] = clampf(3.0103f * (host->log2_f(nn > 1e-10f ? nn : 1e-10f) - host->log2_f(dd > 1e-10f ? dd : 1e-10f)), -48.0f, 24.0f);
  }
  o3m_curve curve;
  o3m_curve_set(&curve, f, db, 40, O3M_AXIS_LOG_FREQ, O3M_AXIS_DB, kMinHz, kMaxHz, -48.0f, 24.0f, O3M_CURVE_AXES | O3M_CURVE_FILL, O3M_COLOR_FX);
  cv->draw->curve(cv, &curve);
  static const char* const kNames[4] = {"LOW-PASS", "HIGH-PASS", "BAND-PASS", "NOTCH"};
  cv->draw->text(cv, 6, 4, kNames[mode & 3u], O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
}

const o3m_ui kUi = {O3M_UI_SIZE(BIQUAD_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    BIQUAD_ENTRY_HEAD, sizeof(State), BIQUAD_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, BIQUAD_ENTRY_VIEWS(&kUi),
};
