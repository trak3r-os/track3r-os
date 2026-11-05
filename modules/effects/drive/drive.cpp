// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): DRIVE, a mono saturation effect for a track insert, module 1 of the first effect pack (docs/roadmap/BACKLOG.md OS-INSFX-FX1), after the
// biquad of OS-INSFX S8 (modules/effects/biquad). All the maths is this module's own: no third-party code and no formula note (THIRD_PARTY.md), float only, no division and no library
// call in the sample loop.
//
//   - Shaper: a polynomial soft clipper, smooth and bounded: v = clamp(kG g x + b, -1, 1), y = v (1.5 - 0.5 v^2) - b (1.5 - 0.5 b^2), with kG = 2/3 folded into the control-rate gain, so the
//     small-signal gain is 1.5 kG = 1 exactly: at Drive 0 dB a quiet signal passes at unity (full scale compresses to 0.85, -1.4 dB; above about 3.5 dB of drive it reaches the clamp and clips at
//     +-1), and |y| <= 1.37 whatever the drive. The slope is 0 at the knee (|v| = 1), so the curve joins the clamp smoothly (C1), and the bias's own value is subtracted so the curve passes through 0.
//     SHAPE is the bias b: 0 = SOFT (the curve is odd, symmetric), 0.25 = ASYM (no longer odd: even harmonics, and a DC offset on any signal, which the DC blocker below removes). No oversampling in
//     this version: a high drive aliases (known limit, README).
//   - After the shaper: a one-pole low-pass (TONE: ty += a (x - ty), a = 1 - e^(-2 pi fc / fs)), then a DC blocker (always on: y = x - x1 + R y1, R = 0.995, about 35 Hz at 44.1 kHz), then the
//     dry/wet mix with the output level folded in: out = (1 - m) og x + m og wet.
//   - Control rate: once per render() call (a block, 1..128 frames) the smoothed drive (dB), tone (log2 Hz), output (dB), mix and bias move a fraction of the way to their targets
//     (k = frames / 256, about 6 ms at 64 frames); the derived values (the input gain, the tone coefficient, the output level) are recomputed only while the drive, tone or output moves
//     (four host exp2 calls at most, never per sample), and the five per-sample values ramp linearly from the last block's to the new ones in steps of kCtl samples. set_param() only stores a
//     target: a MOD parameter may be called every block.
//   - Settled: the ramped values are the same every block and the loop is the same code (no branch in it).
//   - Tail (OS-INSFX S2 contract): render() returns 1 while the input is live (its last sample is not 0) or the state still rings (above kIdle), and 0 when both are silent; a state below kIdle
//     (about -140 dB) is flushed to exact zero at the end of the block (no denormals, an exact end of the tail), a NaN or infinite state too.
//   - Parameters, in this order for ever: Shape (0 SOFT, 1 ASYM), Drive (0..36 dB), Tone (500..20000 Hz), Mix (0..100 %), Output (-24..+6 dB).
#include <o3module.h>

#include "drive_gen.h"

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
constexpr float kBias = 0.25f;          // the ASYM bias
constexpr float kGainScale = 2.0f / 3.0f;  // folded into the control-rate gain: the shaper's small-signal slope 1.5 * kGainScale = 1 (unity at Drive 0 dB)
constexpr float kDcR = 0.995f;          // the DC blocker's pole

struct State {
  const o3m_host* host;
  float rate;
  float t_drive;   // targets (set_param): drive dB, log2 of the tone in Hz, output dB, the mix 0..1, the bias
  float t_lfc;
  float t_out;
  float t_mix;
  float t_bias;
  float drive;     // the smoothed values the derived ones follow
  float lfc;
  float out_db;
  float mix;
  float bias;
  uint32_t dirty;  // the derived values must be computed again (the drive, tone or output moved)
  uint32_t fresh;  // no render yet: set_param snaps instead of gliding (the blob a bind applies, the defaults)
  float c[5];      // the per-sample values the last block ended on: gain, bias, tone a, mix, output level
  float ty;        // the tone's memory
  float dx;        // the DC blocker's memory: its last input and output
  float dy;
};
static_assert(sizeof(State) <= DRIVE_STATE_BYTES, "module.json: state_bytes is too small for State");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN becomes lo
inline float absf(float x) { return __builtin_fabsf(x); }

// The derived targets from the smoothed drive, tone and output: the input gain, the tone coefficient, the output level. Control rate, never in a sample loop (four host exp2 calls).
inline void derive(State* s, float* t) {
  t[0] = kGainScale * s->host->exp2_f(s->drive * kDbToLog2);              // (2/3) 10^(drive/20): small-signal unity at 0 dB
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
  s->t_drive = drive_params[DRIVE_PARAM_DRIVE].def;
  s->t_lfc = host->log2_f(drive_params[DRIVE_PARAM_TONE].def);
  s->t_out = drive_params[DRIVE_PARAM_OUTPUT].def;
  s->t_mix = drive_params[DRIVE_PARAM_MIX].def * 0.01f;
  s->t_bias = drive_params[DRIVE_PARAM_SHAPE].def >= 0.5f ? kBias : 0.0f;
  s->drive = s->t_drive;
  s->lfc = s->t_lfc;
  s->out_db = s->t_out;
  s->mix = s->t_mix;
  s->bias = s->t_bias;
  s->fresh = 1;
  s->dirty = 1;
  float t[5];
  derive(s, t);
  t[1] = s->bias;
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
    case DRIVE_PARAM_SHAPE: s->t_bias = clampf(value, 0.0f, 1.0f) >= 0.5f ? kBias : 0.0f; break;
    case DRIVE_PARAM_DRIVE: s->t_drive = clampf(value, 0.0f, 36.0f); break;
    case DRIVE_PARAM_TONE: s->t_lfc = s->host->log2_f(clampf(value, kMinHz, kMaxHz)); break;
    case DRIVE_PARAM_MIX: s->t_mix = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    case DRIVE_PARAM_OUTPUT: s->t_out = clampf(value, -24.0f, 6.0f); break;
    default: return;
  }
  if (s->fresh != 0u) {  // before the first block: the value is where the effect starts, not a glide to it
    s->bias = s->t_bias;
    s->drive = s->t_drive;
    s->lfc = s->t_lfc;
    s->mix = s->t_mix;
    s->out_db = s->t_out;
    s->dirty = 1;
    float t[5];
    derive(s, t);
    t[1] = s->bias;
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
    const float dd = s->t_drive - s->drive;
    const float dl = s->t_lfc - s->lfc;
    const float doo = s->t_out - s->out_db;
    const float dm = s->t_mix - s->mix;
    const float db = s->t_bias - s->bias;
    if (dd != 0.0f) {
      s->drive = absf(dd) < 1e-3f ? s->t_drive : s->drive + dd * k;
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
    if (dm != 0.0f) {
      s->mix = absf(dm) < 1.0f / 512.0f ? s->t_mix : s->mix + dm * k;
    }
    if (db != 0.0f) {
      s->bias = absf(db) < 1.0f / 512.0f ? s->t_bias : s->bias + db * k;
    }
    float t[5];
    if (s->dirty != 0u) {
      derive(s, t);  // the gain, the tone coefficient, the output level from the moved values
    } else {
      for (uint32_t i = 0; i < 5u; ++i) {
        t[i] = s->c[i];
      }
    }
    t[1] = s->bias;  // the mix and the bias never need a derivation
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
      const float cm0 = (1.0f - c3) * c4;  // the mix with the output level folded in
      const float cm1 = c3 * c4;
      const float c0b = c1 * (1.5f - 0.5f * c1 * c1);  // the bias's own curve value: the shaper passes through 0 at zero input
      const uint32_t len = n - at < kCtl ? n - at : kCtl;
      for (uint32_t i = 0; i < len; ++i) {
        const float x = in[at + i];
        const float u = c0 * x;                              // the drive (the gain scale is folded into c0)
        const float v = clampf(u + c1, -1.0f, 1.0f);         // the shaper's input (the bias is the SHAPE)
        const float sh = v * (1.5f - 0.5f * v * v) - c0b;    // the soft clip: bounded, slope 0 at the knee, through 0
        zty += c2 * (sh - zty);                              // the tone, a one-pole low-pass
        const float dcv = zty - zdx + kDcR * zdy;            // the DC blocker, always on
        zdx = zty;
        zdy = dcv;
        out[at + i] = cm0 * x + cm1 * dcv;                   // the mix and the output level
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

// ---- the view: the shaper's transfer curve at the Drive and Shape the canvas carries ------------------------------------------------------------------
void view_draw(void* raw, uint32_t view, o3m_canvas* cv) {
  const State* s = static_cast<const State*>(raw);  // read only: the host table and the rate, which never change after init()
  if (view != DRIVE_VIEW_CURVE || cv->param_count < DRIVE_PARAM_COUNT) {
    return;
  }
  const o3m_host* host = s->host;
  const int w = cv->width;
  const int h = cv->height;
  if (w < 16 || h < 16) {
    return;
  }
  const float bias = clampf(cv->params[DRIVE_PARAM_SHAPE], 0.0f, 1.0f) >= 0.5f ? kBias : 0.0f;
  const float g = kGainScale * host->exp2_f(clampf(cv->params[DRIVE_PARAM_DRIVE], 0.0f, 36.0f) * kDbToLog2);
  const float c0b = bias * (1.5f - 0.5f * bias * bias);  // the curve passes through 0 at zero input
  constexpr uint32_t kN = 65;  // the curve: x from -1 to 1
  int16_t xy[2 * kN];
  const int ymid = (h - 1) / 2;
  const float yscale = static_cast<float>(h - 3) / 3.0f;  // y in [-1.5, 1.5] (the ASYM bias shifts the curve down by 0.4375)
  for (uint32_t i = 0; i < kN; ++i) {
    const float x = -1.0f + 2.0f * static_cast<float>(i) / 64.0f;
    const float v = clampf(g * x + bias, -1.0f, 1.0f);
    const float y = v * (1.5f - 0.5f * v * v) - c0b;
    xy[2 * i] = static_cast<int16_t>(2 + (w - 5) * static_cast<int>(i) / 64);
    xy[2 * i + 1] = static_cast<int16_t>(ymid - static_cast<int>(y * yscale));
  }
  cv->draw->outline(cv, 1, 1, w - 2, h - 2, O3M_COLOR_EMPTY);
  cv->draw->rect(cv, 1, ymid, w - 2, 1, O3M_COLOR_EMPTY);  // the zero line
  cv->draw->polyline(cv, xy, kN, O3M_COLOR_FX);
  cv->draw->text(cv, 4, 4, bias != 0.0f ? "ASYM" : "SOFT", O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
}

const o3m_ui kUi = {O3M_UI_SIZE(DRIVE_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    DRIVE_ENTRY_HEAD, sizeof(State), DRIVE_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, DRIVE_ENTRY_VIEWS(&kUi),
};
