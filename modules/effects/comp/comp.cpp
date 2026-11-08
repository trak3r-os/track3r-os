// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): COMP, a mono compressor with a TRANSIENT mode for a track insert, module 3 of the first effect pack (docs/roadmap/BACKLOG.md OS-INSFX-FX1), after
// DRIVE (modules/effects/drive), CRUSH (modules/effects/crush) and the biquad of OS-INSFX S8. All the maths is this module's own: no third-party code and no formula note (THIRD_PARTY.md), float
// only, no division and no library or host call in the sample loop (nor anywhere in render(): the log and the exp are the module's own short polynomials, at control rate).
//
// The structure of both modes: a gain-only effect. The sample loop follows the input's envelope (peak followers, one for COMP, a fast and a slow one for TRANSIENT) and multiplies the input by a gain
// that ramps in a straight line across each control step of kCtl = 16 samples; the step's target is computed at control rate from the followers' state. The mix is parallel compression and costs
// nothing per sample: with no latency in the wet path, (1 - m) x + m g x = x (1 - m + m g), so the target is G = og (1 - m + m g), og the make-up (output) level.
//   - COMP: a feed-forward compressor, soft knee of 1 log2 unit (6 dB) centred on the threshold. In log2 of the peak envelope: over = L - T, gain reduction gr = 0 under -1/2, s over above +1/2 and
//     s (over + 1/2)^2 / 2 between (continuous, with a continuous slope), s = 1 - 1/Ratio, and g = 2^-gr. The gain is never above 1 (it only reduces), so at Makeup 0 dB the output is never above the input
//     sample by sample, and rms never above the input's: the lesson of DRIVE, which came out +12 to +16 dB over bypass at its defaults. A signal under the knee (-21 dB peak at the default threshold -18)
//     passes bit for bit. The detector is a peak follower with an attack and a release (a one-pole each, 1 - e^(-1/(t fs))); the gain follows it with a control step's delay (16 samples, 0.36 ms)
//     and no look-ahead, so a very fast attack overshoots for that long: a character of the module, not a click (the gain ramps).
//   - TRANSIENT: a transient shaper from two envelope followers of |x|, a fast one (attack Attack / 8, at least 0.1 ms, release Release / 3) and a slow one (attack Attack, release Release). d = log2(fast) -
//     log2(slow) (each with a small floor of 3e-4 added, about -70 dBFS, so the noise floor is left alone) is positive on an onset (the fast one rises first) and negative on a decay (the fast one falls first);
//     the gain is 2^(Punch d) for d >= 0 and 2^(-Sustain d) for d < 0, in the range of Punch and Sustain -100..+100 % = factors -1..+1, limited to 2^-2..2^2 (+-12 dB). It is level independent (a ratio of two
//     followers). At Punch 0 and Sustain 0 (the defaults) the gain is exactly 1 and the effect is the input bit for bit. A steady tone gains at most about 0.5 dB (the followers' ripple counts on both sides).
//     Threshold and Ratio are COMP's; Attack and Release set the followers' times (Attack: how long an onset lasts, Release: how long a decay is called a decay).
//   - Mix and Makeup: Mix 0 is the input bit for bit at Makeup 0 dB. Once per render() call (a block, 1..128 frames) the smoothed mix and make-up (dB) move k = frames / 256 of the way to their targets,
//     and G ramps across each step: no zipper. Threshold, Ratio, Attack, Release, Punch and Sustain act at once in set_param (a division for Ratio, two for Attack and for Release, a few multiplies otherwise); the gain moves at the next step, ramped.
//     A MOD parameter may be called every block. A change of Mode re-seeds the followers from the next input sample (no jump).
//   - The control step is counted across blocks, so the output is the same at any block size for static parameters (a moving Mix or Makeup glides once a block).
//   - Tail (OS-INSFX S2 contract): render() returns 1 while the input is live (its last sample is not 0) or a follower is still above -80 dBFS (1e-4: the gain may still be moving), 0 when both are
//     silent; at 0 the followers are flushed to exactly 0 and the gain to neutral, so the first note after an idle stretch is treated as by a fresh instance. A NaN, an infinity or an absurd
//     input (over 1e18) is NOT confined to its own sample: it poisons the follower for the rest of the block, so in COMP the gain falls to 2^-16 (-96 dB, ramped) until the follower is flushed to 0 at the end of
//     the block (in TRANSIENT both followers read the same clamped value, d is 0 and the gain stays 1); the next block is clean. The sample itself comes out as NaN or infinity (x G).
//   - Parameters, in this order for ever (ten, ABI 1.12): Threshold (-60..0 dB), Ratio (1..20), Attack (0.1..100 ms), Release (10..1000 ms), Makeup (-24..+24 dB), Mix (0..100 %), Mode (0 COMP,
//     1 TRANSIENT), Punch (-100..100 %), Sustain (-100..100 %), Key (0..16: OFF, T1..T16 — the sidechain key; not a MOD target and never a Learn target). The unit bench's moving-parameter row
//     sweeps the first MOD target, Threshold.
//   - Stereo-capable (ABI 1.12): render_st applies one linked gain to the peak of L and R (on L = R it is the mono render bit for bit); render_st is the only hot body, render a cold thunk that
//     calls it on L = R. Keyed (COMP with Key set): the detector input is the key track's block peak, host->track_level(key - 1) read once a block in an out-of-line cold function, not this
//     track's input; TRANSIENT ignores the Key; Key OFF is the 1.0.0 arithmetic bit for bit.
#include <o3module.h>

#include "comp_gen.h"

namespace {

constexpr uint32_t kCtl = 16;            // samples a gain step lasts, counted across blocks
constexpr uint32_t kBlockMax = O3M_BLOCK_MAX;
constexpr float kInsane = 1e18f;         // a follower beyond this (or NaN, or infinite) is flushed to 0 at the end of the block
constexpr float kIdle = 1e-4f;           // -80 dBFS: both followers under it and a silent input: idle
constexpr float kMagic = 12582912.0f;    // 1.5 * 2^23: (t + kMagic) - kMagic rounds t to the nearest integer for |t| < 2^22
constexpr float kDbToLog2 = 0.16609640474436812f;  // log2(10)/20: 10^(db/20) = 2^(db * kDbToLog2), and a level in dB is db * kDbToLog2 log2 units
constexpr float kLog2ToDb = 6.020599913279624f;
constexpr float kTiny = 1e-9f;           // COMP: added to the envelope before the log
constexpr float kFloor = 3e-4f;          // TRANSIENT: added to both followers before the log
constexpr float kMaxCut = -16.0f;        // COMP: the deepest gain, 2^-16 (-96 dB)
constexpr float kShapeMax = 2.0f;        // TRANSIENT: the gain is limited to 2^-2 .. 2^2
constexpr float kSqrt2 = 1.41421356f;
constexpr float kRampMin = 3e-5f;        // a step whose gain moves by less than this does not ramp (it holds, then lands on the target): a ramp of rounding-sized slope could overshoot a target of 1 by an ulp

struct State {
  const o3m_host* host;
  float att_ms, rel_ms;  // the Attack and Release in ms (the keyed detector's block-rate coefficients)
  uint32_t key;    // 0 none, 1..16 = the key track + 1 (COMP mode only)
  float k_ms;      // 1000 / sample rate: the one-pole's x = k_ms / time_ms
  float thr;       // threshold in log2 units
  float slope;     // 1 - 1 / ratio
  float ca, cr;    // COMP: the detector's attack and release coefficients
  float fa, fr;    // TRANSIENT: the fast follower's
  float sa, sr;    // TRANSIENT: the slow follower's
  float kp, ks;    // Punch and Sustain as factors -1..1
  float t_mix;     // targets (set_param): the mix 0..1, the make-up in dB
  float t_out;
  float mix;       // the smoothed values
  float out_db;
  float og;        // the make-up level of the smoothed make-up
  float e0, e1;    // the followers (COMP: e0 only)
  float G;         // the gain now, the gain this step ramps to, and the step's slope per sample
  float Gt;
  float dG;
  uint32_t left;   // samples left in this control step (0: the next sample starts one)
  uint32_t mode;   // 0 COMP, 1 TRANSIENT
  uint32_t seed;   // the followers start from the next input sample (a Mode change)
  uint32_t dirty;  // og must be computed again (the make-up moved)
  uint32_t fresh;  // no render yet: set_param snaps instead of gliding (the blob a bind applies, the defaults)
};
static_assert(sizeof(State) <= COMP_STATE_BYTES, "module.json: state_bytes is too small for State");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN becomes lo
inline float absf(float x) { return __builtin_fabsf(x); }
inline float maxf(float a, float b) { return a > b ? a : b; }

inline float pow2i(int e) {  // 2^e exactly, for -126..127
  const uint32_t u = static_cast<uint32_t>(127 + e) << 23;
  float f;
  __builtin_memcpy(&f, &u, sizeof f);
  return f;
}

// 2^x, relative error under 1e-7 (a fitted polynomial of 2^f on [-1/2, 1/2], f = x - round(x)); exactly 1 at x = 0. Control rate only.
O3M_HOT float exp2p(float x) {  // (hot and out of line: called once or twice per gain step, so one copy serves the whole module)
  x = clampf(x, -60.0f, 30.0f);
  const float r = (x + kMagic) - kMagic;
  const float f = x - r;
  const float p = 1.0f + f * (0.6931472148513547f + f * (0.24022234543216514f + f * (0.0555030850662476f + f * (0.009671878790875747f + f * 0.0013407206067358596f))));
  return p * pow2i(static_cast<int>(r));
}

// log2(v) for a normal v > 0, absolute error under 3e-6 (the mantissa folded into [1/sqrt 2, sqrt 2) and a fitted polynomial of log2(1 + t)). Control rate only.
O3M_HOT float log2p(float v) {
  uint32_t b;
  __builtin_memcpy(&b, &v, sizeof b);
  int e = static_cast<int>(b >> 23) - 127;
  b = (b & 0x7FFFFFu) | 0x3F800000u;
  float m;
  __builtin_memcpy(&m, &b, sizeof m);
  if (m > kSqrt2) {
    m *= 0.5f;
    ++e;
  }
  const float t = m - 1.0f;
  const float p = t * (1.442713470869811f + t * (-0.721131707960554f + t * (0.4793486562756306f + t * (-0.3674935893616021f + t * (0.32214907461514214f + t * -0.20657013765315937f)))));
  return static_cast<float>(e) + p;
}

// The one-pole coefficient 1 - e^-x of a time in ms, x = 1 / (ms fs / 1000) limited to 0.3 (0.075 ms at 44.1 kHz), by its series (no exp call: x is small, the series is good to 1e-6).
inline float pole(const State* s, float ms) {
  float x = s->k_ms / ms;
  x = x < 0.3f ? x : 0.3f;
  return x * (1.0f - x * (0.5f - x * (0.16666667f - x * (0.041666668f - x * 0.008333334f))));
}

// The one-pole coefficient 1 - e^-(n k_ms / ms) of a whole block of n frames (the keyed detector steps once a block), exact through the module's own exp2.
float pole_block(const State* s, float ms, uint32_t n) {
  const float x = s->k_ms * static_cast<float>(n) / ms;
  return 1.0f - exp2p(-1.4426950408889634f * x);
}

void apply_attack(State* s, float ms) {
  ms = clampf(ms, 0.1f, 100.0f);
  s->att_ms = ms;
  const float fast = ms * 0.125f;
  s->ca = pole(s, ms);
  s->sa = s->ca;
  s->fa = pole(s, fast > 0.1f ? fast : 0.1f);
}

void apply_release(State* s, float ms) {
  ms = clampf(ms, 10.0f, 1000.0f);
  s->rel_ms = ms;
  s->cr = pole(s, ms);
  s->sr = s->cr;
  s->fr = pole(s, ms * (1.0f / 3.0f));
}

// The slope of the ramp from the gain G to the target Gt over a control step (the step's 16 samples).
inline float ramp(float Gt, float G) {
  const float d = Gt - G;
  return absf(d) < kRampMin ? 0.0f : d * (1.0f / 16.0f);
}

inline void derive_out(State* s) {
  s->og = exp2p(s->out_db * kDbToLog2);  // 10^(out/20)
  s->dirty = 0;
}

// The compressor's gain reduction in log2 units for a level over the threshold (the soft knee): 0 under -1/2, s over above +1/2, s (over + 1/2)^2 / 2 between.
inline float reduction(float over, float slope) {
  if (over >= 0.5f) {
    return slope * over;
  }
  if (over > -0.5f) {
    const float k = over + 0.5f;
    return slope * k * k * 0.5f;
  }
  return 0.0f;
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  const float fs = host->sample_rate >= 8000.0f ? host->sample_rate : 44100.0f;
  s->host = host;
  s->key = 0;
  s->k_ms = 1000.0f / fs;
  s->thr = comp_params[COMP_PARAM_THRESHOLD].def * kDbToLog2;
  s->slope = 1.0f - 1.0f / comp_params[COMP_PARAM_RATIO].def;
  apply_attack(s, comp_params[COMP_PARAM_ATTACK].def);
  apply_release(s, comp_params[COMP_PARAM_RELEASE].def);
  s->kp = comp_params[COMP_PARAM_PUNCH].def * 0.01f;
  s->ks = comp_params[COMP_PARAM_SUSTAIN].def * 0.01f;
  s->t_mix = comp_params[COMP_PARAM_MIX].def * 0.01f;
  s->t_out = comp_params[COMP_PARAM_MAKEUP].def;
  s->mix = s->t_mix;
  s->out_db = s->t_out;
  derive_out(s);
  s->e0 = 0.0f;
  s->e1 = 0.0f;
  s->G = s->og;
  s->Gt = s->og;
  s->dG = 0.0f;
  s->left = 0;
  s->mode = 0;
  s->seed = 0;
  s->fresh = 1;
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {  // everything acts at once but the mix and the make-up: those are targets the render glides to
  State* s = static_cast<State*>(raw);
  switch (id) {
    case COMP_PARAM_THRESHOLD: s->thr = clampf(value, -60.0f, 0.0f) * kDbToLog2; return;
    case COMP_PARAM_RATIO: s->slope = 1.0f - 1.0f / clampf(value, 1.0f, 20.0f); return;
    case COMP_PARAM_ATTACK: apply_attack(s, value); return;
    case COMP_PARAM_RELEASE: apply_release(s, value); return;
    case COMP_PARAM_PUNCH: s->kp = clampf(value, -100.0f, 100.0f) * 0.01f; return;
    case COMP_PARAM_SUSTAIN: s->ks = clampf(value, -100.0f, 100.0f) * 0.01f; return;
    case COMP_PARAM_MODE: {
      const uint32_t m = value >= 0.5f ? 1u : 0u;
      if (m != s->mode) {
        s->mode = m;
        s->seed = 1;  // the followers of the other mode are old: start them from the input
      }
      return;
    }
    case COMP_PARAM_KEY: s->key = static_cast<uint32_t>(clampf(value + 0.5f, 0.0f, 16.0f)); return;  // rounded; a 1.0.0 blob never sets it: OFF
    case COMP_PARAM_MIX: s->t_mix = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    case COMP_PARAM_MAKEUP: s->t_out = clampf(value, -24.0f, 24.0f); break;
    default: return;
  }
  if (s->fresh != 0u) {  // before the first block: the value is where the effect starts, not a glide to it
    s->mix = s->t_mix;
    s->out_db = s->t_out;
    derive_out(s);
    s->G = s->og;  // the followers are 0: the first gain is 1, so the first target is the make-up
    s->Gt = s->og;
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// The keyed block, out of line (cold, one copy for both render shapes): COMP with a Key set reads the key track's level once a block, steps the follower through the
// Attack or the Release, and ramps the gain in a straight line across the block. (Kept out of the hot slab: render_core is compiled twice and the SRAM_L slab is shared.)
__attribute__((noinline)) void keyed_block(State* s, const float* in, const float* inr, float* out, float* outr, uint32_t n, bool st, float tm, float og, float& e0, float& G, float& Gt) {
  float level = 0.0f;
  if (O3M_HOST_HAS(s->host, track_level)) {
    level = clampf(s->host->track_level(s->key - 1u), 0.0f, 1.0f);
  }
  const float c = pole_block(s, level > e0 ? s->att_ms : s->rel_ms, n);
  e0 += c * (level - e0);
  const float gr = reduction(log2p(e0 + kTiny) - s->thr, s->slope);
  float g = exp2p(gr > -kMaxCut ? kMaxCut : -gr);
  g = g < 1.0f ? g : 1.0f;
  Gt = og * ((1.0f - tm) + tm * g);
  const float dgb = (Gt - G) * (1.0f / static_cast<float>(n));
  for (uint32_t i = 0; i < n; ++i) {
    const float x = in[i];
    const float xr = st ? inr[i] : 0.0f;
    out[i] = x * G;
    if (st) {
      outr[i] = xr * G;
    }
    G += dgb;
  }
  G = Gt;
}

// One call of 1..kBlockMax frames: the control step of the mix and the make-up, then the samples in gain steps of kCtl (counted across calls).
template <bool ST>
inline uint32_t render_core(State* s, const float* in, const float* inr, float* out, float* outr, uint32_t frames) {  // ST: stereo, one linked gain on the peak of L and R
  uint32_t live = 0;
  while (frames > 0u) {  // (the ABI promises at most kBlockMax; a host that gives more is served in pieces)
    const uint32_t n = frames < kBlockMax ? frames : kBlockMax;
    s->fresh = 0;
    // ---- control step of the block: the mix and the make-up glide to their targets ----------------------------------------------------------
    const float k = n >= 256u ? 1.0f : static_cast<float>(n) * (1.0f / 256.0f);
    const float dm = s->t_mix - s->mix;
    const float doo = s->t_out - s->out_db;
    if (dm != 0.0f) {
      s->mix = absf(dm) < 1.0f / 512.0f ? s->t_mix : s->mix + dm * k;
    }
    if (doo != 0.0f) {
      s->out_db = absf(doo) < 1e-3f ? s->t_out : s->out_db + doo * k;
      s->dirty = 1;
    }
    if (s->dirty != 0u) {
      derive_out(s);
    }
    const float tm = s->mix;
    const float og = s->og;
    live |= in[n - 1u] != 0.0f ? 1u : 0u;  // before anything writes: in may be out
    if (ST) {
      live |= inr[n - 1u] != 0.0f ? 1u : 0u;
    }
    float e0 = s->e0, e1 = s->e1;
    if (s->seed != 0u) {  // a Mode change re-seeds the followers from the next input sample — unless keyed: keyed, e0 follows the key track's level, not this input
      if (s->mode == 0u && s->key != 0u) {
        e0 = 0.0f;  // from nothing: the keyed block pulls it to the key's level through the attack
      } else {
        const float a = ST ? maxf(absf(in[0]), absf(inr[0])) : absf(in[0]);
        e0 = a;
        e1 = a;
      }
      s->seed = 0;
    }
    float G = s->G, Gt = s->Gt, dG = s->dG;
    uint32_t left = s->left;
    uint32_t at = 0;
    // ---- the samples ----------------------------------------------------------------------------------------------------------------------
    const bool keyed = s->mode == 0u && s->key != 0u;
    if (keyed) {  // COMP keyed: the detector input is the key track's block peak, once a block; the gain ramps linearly across the block (the work is out of line, cold)
      keyed_block(s, in, inr, out, outr, n, ST, tm, og, e0, G, Gt);
      dG = 0.0f;
      left = 0;
    } else if (s->mode == 0u) {
      const float ca = s->ca, cr = s->cr, thr = s->thr, slope = s->slope;
      while (at < n) {
        if (left == 0u) {  // a gain step: the target from the follower
          const float a = e0 <= kInsane ? e0 : kInsane;  // NaN becomes kInsane
          const float gr = reduction(log2p(a + kTiny) - thr, slope);
          float g = exp2p(gr > -kMaxCut ? kMaxCut : -gr);
          g = g < 1.0f ? g : 1.0f;
          Gt = og * ((1.0f - tm) + tm * g);
          dG = ramp(Gt, G);
          left = kCtl;
        }
        const uint32_t len = n - at < left ? n - at : left;
        for (uint32_t i = 0; i < len; ++i) {
          const float x = in[at + i];
          const float xr = ST ? inr[at + i] : 0.0f;
          const float d = (ST ? maxf(absf(x), absf(xr)) : absf(x)) - e0;
          e0 += (d > 0.0f ? ca : cr) * d;
          out[at + i] = x * G;
          if (ST) {
            outr[at + i] = xr * G;
          }
          G += dG;
        }
        left -= len;
        at += len;
        if (left == 0u) {
          G = Gt;  // the ramp's end value exactly
        }
      }
    } else {
      const float fa = s->fa, fr = s->fr, sa = s->sa, sr = s->sr, kp = s->kp, ks = s->ks;
      while (at < n) {
        if (left == 0u) {
          const float a = e0 <= kInsane ? e0 : kInsane;
          const float b = e1 <= kInsane ? e1 : kInsane;
          const float d = log2p(a + kFloor) - log2p(b + kFloor);
          float gl = d >= 0.0f ? kp * d : -ks * d;
          gl = gl < -kShapeMax ? -kShapeMax : (gl > kShapeMax ? kShapeMax : gl);
          const float g = exp2p(gl);
          Gt = og * ((1.0f - tm) + tm * g);
          dG = ramp(Gt, G);
          left = kCtl;
        }
        const uint32_t len = n - at < left ? n - at : left;
        for (uint32_t i = 0; i < len; ++i) {
          const float x = in[at + i];
          const float xr = ST ? inr[at + i] : 0.0f;
          const float a = ST ? maxf(absf(x), absf(xr)) : absf(x);
          const float df = a - e0;
          e0 += (df > 0.0f ? fa : fr) * df;
          const float ds = a - e1;
          e1 += (ds > 0.0f ? sa : sr) * ds;
          out[at + i] = x * G;
          if (ST) {
            outr[at + i] = xr * G;
          }
          G += dG;
        }
        left -= len;
        at += len;
        if (left == 0u) {
          G = Gt;
        }
      }
    }
    // ---- the end of the block: a poisoned follower is flushed, an idle effect goes neutral ---------------------------------------------------
    if (!(e0 <= kInsane)) {  // a NaN, an infinity or an absurd input was followed (false for NaN)
      e0 = 0.0f;
    }
    if (!(e1 <= kInsane)) {
      e1 = 0.0f;
    }
    const uint32_t ring = (e0 >= kIdle || e1 >= kIdle) ? 1u : 0u;
    if (ring == 0u) {  // idle: nothing to follow, so the gain is the make-up and the next sample starts a step (the same as a fresh instance)
      e0 = 0.0f;
      e1 = 0.0f;
      G = og;
      Gt = og;
      dG = 0.0f;
      left = 0;
    }
    live |= ring;
    s->e0 = e0;
    s->e1 = e1;
    s->G = G;
    s->Gt = Gt;
    s->dG = dG;
    s->left = left;
    in += n;
    out += n;
    if (ST) {
      inr += n;
      outr += n;
    }
    frames -= n;
  }
  return live;  // 1 while the input is live or a follower rings, 0 when both are silent (the S2 tail contract)
}

// The only hot body: render_st holds render_core<true>. The mono shape runs the same body on L = R — the detector's peak of L and R is then |x|, so the bits are the mono
// render's (pinned by the host tests) — through a cold thunk below. One body twice was over the hot cap: the SRAM_L slab (14 KiB) is shared by every loaded module.
O3M_HOT uint32_t render_st(void* raw, const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t frames) {
  return render_core<true>(static_cast<State*>(raw), in_l, in_r, out_l, out_r, frames);
}

uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {  // cold thunk: all the sample work runs in the hot render_st
  return render_st(raw, in, in, out, out, frames);
}

// ---- the view: COMP's transfer law, or TRANSIENT's gain law ------------------------------------------------------------------------------------
const char* const kModeName[2] = {"COMP", "TRANSIENT"};

void view_draw(void*, uint32_t view, o3m_canvas* cv) {
  if (view != COMP_VIEW_CURVE || cv->param_count < COMP_PARAM_COUNT) {
    return;
  }
  const int w = cv->width;
  const int h = cv->height;
  if (w < 16 || h < 16) {
    return;
  }
  const uint32_t mode = cv->params[COMP_PARAM_MODE] >= 0.5f ? 1u : 0u;
  const int ybase = h - 3;
  const float yspan = static_cast<float>(h - 4);
  const int xmid = w / 2;
  const int ymid = (h - 1) / 2;
  cv->draw->outline(cv, 1, 1, w - 2, h - 2, O3M_COLOR_EMPTY);
  if (mode == 0u) {  // input level (-60..0 dB) against output level, with the 1:1 line
    constexpr uint32_t kN = 65;
    int16_t xy[2 * kN];
    const float thr = clampf(cv->params[COMP_PARAM_THRESHOLD], -60.0f, 0.0f) * kDbToLog2;
    const float slope = 1.0f - 1.0f / clampf(cv->params[COMP_PARAM_RATIO], 1.0f, 20.0f);
    for (uint32_t i = 0; i < kN; ++i) {
      const float din = -60.0f + 60.0f * static_cast<float>(i) / 64.0f;
      const float dout = clampf(din - reduction(din * kDbToLog2 - thr, slope) * kLog2ToDb, -60.0f, 0.0f);
      xy[2 * i] = static_cast<int16_t>(2 + (w - 5) * static_cast<int>(i) / 64);
      xy[2 * i + 1] = static_cast<int16_t>(ybase - static_cast<int>((dout + 60.0f) * (1.0f / 60.0f) * yspan));
    }
    const int16_t diag[4] = {static_cast<int16_t>(2), static_cast<int16_t>(ybase), static_cast<int16_t>(w - 3), static_cast<int16_t>(ybase - static_cast<int>(yspan))};
    cv->draw->polyline(cv, diag, 2, O3M_COLOR_EMPTY);
    cv->draw->polyline(cv, xy, kN, O3M_COLOR_FX);
  } else {  // the gain (log2, +-2 = +-12 dB) against d = log2(fast / slow), -2..2: Sustain on the left, Punch on the right
    const float ks = clampf(cv->params[COMP_PARAM_SUSTAIN], -100.0f, 100.0f) * 0.01f;
    const float kp = clampf(cv->params[COMP_PARAM_PUNCH], -100.0f, 100.0f) * 0.01f;
    const float gl[3] = {clampf(2.0f * ks, -kShapeMax, kShapeMax), 0.0f, clampf(2.0f * kp, -kShapeMax, kShapeMax)};
    int16_t xy[6];
    for (uint32_t i = 0; i < 3; ++i) {
      xy[2 * i] = static_cast<int16_t>(2 + (w - 5) * static_cast<int>(i) / 2);
      xy[2 * i + 1] = static_cast<int16_t>(ybase - static_cast<int>((gl[i] + kShapeMax) * (1.0f / 4.0f) * yspan));
    }
    cv->draw->rect(cv, 1, ymid, w - 2, 1, O3M_COLOR_EMPTY);
    cv->draw->rect(cv, xmid, 1, 1, h - 2, O3M_COLOR_EMPTY);
    cv->draw->polyline(cv, xy, 3, O3M_COLOR_FX);
  }
  cv->draw->text(cv, 4, 4, kModeName[mode], O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
}

const o3m_ui kUi = {O3M_UI_SIZE(COMP_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    COMP_ENTRY_HEAD, sizeof(State), COMP_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, COMP_ENTRY_VIEWS(&kUi),
    COMP_ENTRY_FX(render_st),
};
