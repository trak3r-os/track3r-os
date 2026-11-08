// SPDX-License-Identifier: MIT
//
// Contains a port of the CHIP synth engine of Polyend Tracker Community Edition (tracker-ce@7fd6c2c, core/synth/engines/ChiptuneSynthVoice.cpp), Copyright (c) 2020-2026 Polyend Sp. z o.o.
// and Polyend Tracker Community Edition Contributors, MIT licence with the conditions of its Part 2: see LICENSE-tracker-ce.txt.
//
// OS-CECHIP C2 to C6 (C4 and C5 are both in this file; C6 measured policy Fast against Std, see modules/chip/README.md): waves 4..11 of the Chip module, the engine's models, and the engine's post chain (sub oscillator, decimation, bitcrush, resonant lowpass) on all twelve waves. The pulse family: 2A03 PULSE (wave 4), ZAP/LASER (wave 9) and ARP CHORD (wave 10) share
// one kernel, a +-1 pulse with two PolyBLEPs and a PWM duty from a triangle LFO, exactly as the original computes them (the oracle test, tests/cechip_oracle_test.cpp, holds the kernel to the
// original, compiled unmodified from third_party/tracker-ce-chip, at policy Std: 1e-4 absolute). C4 adds SID SAW/TRI (wave 5), NOISE/LFSR (wave 6) and SUPER SQR (wave 8), C5 FM BELL (wave 7) and
// CHIP DRUM (wave 11), each a kernel of its own held to the original the same way.
// Where each part sits in the original (renderVoiceAudio of ChiptuneSynthVoice.cpp, step 6 models, steps 7 to 9 post chain): pulse family case labels at 321, SID SAW/TRI 332, NOISE/LFSR 344, FM BELL 357,
// SUPER SQR 380, CHIP DRUM 409; the post chain (C3) 434 to 476.
//
// What is Chip's and not the original's (docs/specs/OS-CECHIP.md 3.4: new Community OS behaviour): the envelope (the same per-sample code as waves 0..3, Vol Steps included), the pitch update
// every kControl samples, Sweep, vibrato, Glide, velocity and the level. What is the original's: the PWM LFO and duty, the kernels, the frequency clamp of 16..18000 Hz, Zap's pitch drop and
// the chord tables. The code runs cold (from SDRAM): render() below is noinline and outside O3M_HOT; the hot render() only tests Voice::path once a block.
//
// Included by chip_dsp.h only (it needs Voice, the envelope and the pitch sum defined there). Block based, float only, no allocation, no statics, bounded loops.
#pragma once

// A check that runs in the host build (the tests) and is nothing on the target: a trap, so that a test run that breaks it stops at once.
#ifndef CHIP_CE_ASSERT
#if defined(__arm__)
#define CHIP_CE_ASSERT(c) ((void)0)
#else
#define CHIP_CE_ASSERT(c) ((c) ? (void)0 : __builtin_trap())
#endif
#endif

namespace chip {
namespace ce {

// The model of a wave (waves 4..11 are CE's models 0..7 in CE's order).
enum Model : uint32_t {
  k2A03 = 4, kSid = 5, kNoise = 6, kFmBell = 7, kSuperSquare = 8, kZap = 9, kArpChord = 10, kDrum = 11,
};

// CE's frequency clamp (Hz) and the pulse's level.
constexpr float kMinHz = 16.0f;
constexpr float kMaxHz = 18000.0f;
constexpr float kPulseLevel = 0.75f;

// The step a sample is held at (OS-MODRATE S5): kMaxHz at 44100 Hz is 0.408, but at 22050 or 11025 Hz (O3M_FX_RATE) it would pass Nyquist, so the clamp is also 0.45 of the rate, as waves 0..3 have it.
constexpr float kMaxStep = 0.45f;
// The original's register: 15 bits, reseeded to 0x7FFF at a note. The noise's clock runs at most 0.45 x 16 = 7.2 steps a sample (the frequency clamp: 6.53 at 44100 Hz, 7.2 at the reduced rates),
// so a sample takes at most kLfsrMax = 8 steps; the loop is bounded by it and the host build asserts it was never the bound that ended the loop.
constexpr uint32_t kLfsrSeed = 0x7FFFu;
constexpr uint32_t kLfsrMax = 8;
// FM BELL's modulator ratios (the original's table, picked by Shape x 7.99) and CHIP DRUM's pitch decay time in seconds (exp(-age / (0.08 sr))).
constexpr float kFmRatios[8] = {1.0f, 2.0f, 3.0f, 3.5f, 4.0f, 5.0f, 7.0f, 9.0f};
constexpr float kDrumTau = 0.08f;

// sin(x) for x in radians, float only: the quadrant n = round(x 2 / pi) and the remainder r = x - n pi / 2 (pi / 2 in three parts, so that n x part is exact: the remainder is good to a few ulps up to |x|
// of 1000), then a degree-7 sine or a degree-8 cosine polynomial on |r| <= pi / 4 (Cephes' coefficients). The error against std::sin is below 2e-6 over [-1000, 1000] (the oracle test measures it).
inline float sin_rad(float x) {
  const float fn = detail::roundf_(x * 0.636619772f);
  const float r = ((x - fn * 1.5703125f) - fn * 4.837512969970703125e-4f) - fn * 7.54978995489188216e-8f;
  const int32_t n = static_cast<int32_t>(fn);
  const float z = r * r;
  float y;
  if ((n & 1) != 0) {
    y = ((2.443315711809948e-5f * z - 1.388731625493765e-3f) * z + 4.166664568298827e-2f) * z * z - 0.5f * z + 1.0f;  // cos r
  } else {
    y = ((-1.9515295891e-4f * z + 8.3321608736e-3f) * z - 1.6666654611e-1f) * z * r + r;  // sin r
  }
  return (n & 2) != 0 ? -y : y;
}

// The four-step chords, in semitones above the note: Off, Octave, Fifth, Major, Minor, Sus4, Maj7, Min7 (the original's tables). The ARP CHORD model plays Major when Chord is Off.
constexpr int8_t kChords[8][4] = {
    {0, 0, 0, 0}, {0, 12, 0, 12}, {0, 7, 12, 7}, {0, 4, 7, 12}, {0, 3, 7, 12}, {0, 5, 7, 12}, {0, 4, 7, 11}, {0, 3, 7, 10},
};
constexpr uint32_t kChordMajor = 3;

// ---- the maths policies ---------------------------------------------------------------------------------------------------------------------------------
// The kernels are templates on a policy. Std evaluates the original's expressions as the original writes them (a division, fmod); the oracle tests use it. Fast is what the module uses: no
// division and no fmod in the per-sample code (the reciprocal of the phase step is kept per chunk; a phase in [1, 2) is wrapped by a subtraction, which is exact).
struct Std {
  static float blep(float t, float dt, float /*inv*/) {
    if (dt <= 0.0f) {
      return 0.0f;
    }
    if (t < dt) {
      t /= dt;
      return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
      t = (t - 1.0f) / dt;
      return t * t + t + t + 1.0f;
    }
    return 0.0f;
  }
  static float wrap(float x) { return __builtin_fmodf(x, 1.0f); }
  static float round(float x) { return __builtin_roundf(x); }
  static float pow2(const o3m_host*, float x) { return __builtin_powf(2.0f, x); }
  static float sin(const o3m_host*, float x) { return __builtin_sinf(x); }
  // the original's fastSine (a phase in cycles), floor and the FM carrier's 6-bit quantiser, and the drum's pitch decay (the original's exp of the note's age)
  static float sine(float cycles) { return __builtin_sinf(cycles * 2.0f * 3.14159274f); }
  static float floor(float x) { return __builtin_floorf(x); }
  static float bits6(float x) { return __builtin_roundf(x * 31.0f) / 31.0f * 0.85f; }
  static float decay(Osc&, uint32_t age, float tau) { return __builtin_expf(-static_cast<float>(age) / tau); }
};

struct Fast {
  static CHIP_INLINE float blep(float t, float dt, float inv) { return chip::blep(t, dt, inv); }
  static CHIP_INLINE float wrap(float x) { return x - (x >= 1.0f ? 1.0f : 0.0f); }  // x is in [0.06, 1.94] here: one subtraction is the fmod
  static CHIP_INLINE float round(float x) { return detail::roundf_(x); }  // half away from zero, as std::round
  static float pow2(const o3m_host* h, float x) { return h->exp2_f(x); }  // at control points only
  static float sin(const o3m_host* h, float x) { return h->sin_f(x); }
  // the inline polynomial sine (the same argument as the original's: cycles x 2 x pi), the truncation for a floor of a positive phase, a multiply for the quantiser's division, the recurrence d *= k for the decay
  static CHIP_INLINE float sine(float cycles) { return sin_rad(cycles * 2.0f * 3.14159274f); }
  static CHIP_INLINE float floor(float x) { return static_cast<float>(static_cast<int32_t>(x)); }  // x >= 0 here
  static CHIP_INLINE float bits6(float x) { return detail::roundf_(x * 31.0f) * (0.85f / 31.0f); }
  static CHIP_INLINE float decay(Osc& o, uint32_t, float) {
    const float d = o.drum_d;  // anchored at the control point (drum_anchor), then the recurrence
    o.drum_d = d * o.drum_k;
    return d;
  }
};

// ---- the kernels ----------------------------------------------------------------------------------------------------------------------------------------
// The PWM LFO (a triangle in -1..1) and the duty: duty = clamp(0.125 + 0.75 shape + 0.38 depth lfo, 0.06, 0.94), with `base` = 0.125 + 0.75 shape and `k` = 0.38 depth (the same products
// the original forms, so the same floats). `ph` is the LFO's phase, `step` its rate / sample rate.
CHIP_INLINE float pwm_lfo(float& ph, float step) {
  ph += step;
  if (ph >= 1.0f) {
    ph -= 1.0f;
  }
  return ph < 0.5f ? ph * 4.0f - 1.0f : 3.0f - ph * 4.0f;
}
CHIP_INLINE float pwm_duty(float& ph, float step, float base, float k) {
  const float lfo = pwm_lfo(ph, step);
  return detail::clampf(base + k * lfo, 0.06f, 0.94f);
}

// One sample of the pulse family: the phase advances by dt, the pulse is +-1 with a BLEP at each edge, times 0.75. `inv` is 1 / dt (Fast reads it, Std divides).
template <class P>
CHIP_INLINE float pulse(float& phase, float dt, float inv, float duty) {
  phase += dt;
  if (phase >= 1.0f) {
    phase -= 1.0f;
  }
  float s = phase < duty ? 1.0f : -1.0f;
  s += P::blep(phase, dt, inv);
  s -= P::blep(P::wrap(phase - duty + 1.0f), dt, inv);
  return s * kPulseLevel;
}

// ---- the post chain (C3): the original's steps 7 to 9, in its order ----------------------------------------------------------------------------------
// 7. The sub oscillator: a 4-bit stepped triangle an octave down (its phase advances by dt / 2 always; SID reads it), mixed in when Sub Osc is above 0.001.
CHIP_INLINE void sub_advance(float& ph, float dt) {
  ph += dt * 0.5f;
  if (ph >= 1.0f) {
    ph -= 1.0f;
  }
}
template <class P>
CHIP_INLINE float sub_step(float ph) {
  const float tri = ph < 0.5f ? ph * 4.0f - 1.0f : 3.0f - ph * 4.0f;
  return P::round(tri * 7.5f) / 7.5f;
}
template <class P>
CHIP_INLINE float sub_mix(float sig, float ph, float sub) {
  return sig * (1.0f - 0.35f * sub) + sub_step<P>(ph) * (0.75f * sub);
}

// 8. Decimation (hold a sample for `factor` samples) and bitcrush (when on, round to 2^(bits - 1) levels; `scale` is that and `inv` its inverse).
template <class P>
CHIP_INLINE float hold_crush(Post& s, float sig, uint32_t factor, bool crush_on, float scale, float inv) {
  if (++s.cnt >= factor) {
    s.cnt = 0;
    s.held = crush_on ? P::round(sig * scale) * inv : sig;
  }
  return s.held;
}

// 9. The Chamberlin state-variable lowpass: f = min(0.85, 2 sin(pi fc / sr)) with fc = clamp(Cutoff 2^(4 Env Amt env), 40, 18000); skipped (returns 0 here) while fc is at least 17500 Hz and Resonance at most 0.02.
// Called at a control point with the envelope there; per sample the filter is svf_step.
template <class P>
CHIP_INLINE float svf_coef(const o3m_host* h, float cutoff, float env_amt, float env, float resonance, float inv_sr) {
  float fc = cutoff * P::pow2(h, env_amt * env * 4.0f);
  float top = 18000.0f;  // and at most 0.45 of a reduced sample rate (OS-MODRATE S5: 2 sin(pi fc / sr) turns over past a quarter of 2 pi, a negative coefficient at 11025 Hz); 44100 Hz keeps 18000
  if (kMaxStep < 18000.0f * inv_sr) {
    top = kMaxStep / inv_sr;
  }
  fc = fc < 40.0f ? 40.0f : (fc > top ? top : fc);
  if (fc < top - 500.0f || resonance > 0.02f) {
    const float f = 2.0f * P::sin(h, 3.14159274f * fc * inv_sr);
    return f < 0.85f ? f : 0.85f;
  }
  return 0.0f;
}
CHIP_INLINE float svf_step(Post& s, float x, float f, float q) {
  s.low += f * s.band;
  const float high = x - s.low - q * s.band;
  s.band += f * high;
  s.band = detail::clampf(s.band, -2.0f, 2.0f);
  s.low = detail::clampf(s.low, -2.0f, 2.0f);
  return s.low;
}

// The main phase of the models other than the pulse family (which folds it into pulse()): advances by dt and wraps; true when it wrapped (the drum reads that).
CHIP_INLINE bool advance_phase(float& phase, float dt) {
  phase += dt;
  if (phase >= 1.0f) {
    phase -= 1.0f;
    return true;
  }
  return false;
}

// ---- C4: SID SAW/TRI, NOISE/LFSR, SUPER SQR ------------------------------------------------------------------------------------------------------------
// SID: a band-limited saw, a triangle and the saw ring-modulated by the sub oscillator's square, blended by the duty (the PWM's), times 0.8. `sub_ph` is the sub phase as the original reads it: the one
// of the sample before (its own step comes after the model, in the post chain).
template <class P>
CHIP_INLINE float sid(float& phase, float sub_ph, float dt, float inv, float duty) {
  advance_phase(phase, dt);
  float saw = 2.0f * phase - 1.0f;
  saw -= P::blep(phase, dt, inv);
  const float tri = phase < 0.5f ? phase * 4.0f - 1.0f : 3.0f - phase * 4.0f;
  const float ring = saw * (sub_ph < 0.5f ? 1.0f : -1.0f);
  return ((1.0f - duty) * tri + duty * (0.65f * saw + 0.35f * ring)) * 0.8f;
}

// NOISE/LFSR: the register is clocked `dt x rate` times a sample with rate = 2 + 14 (1 - PWM Depth) (a sample takes at most kLfsrMax = 7 steps), the output is +-0.8; Shape 0.5 and up is the short sequence.
CHIP_INLINE float noise(Osc& o, float dt, float rate, bool short_mode) {
  o.lfsr_ph += dt * rate;
  uint32_t steps = 0;
  while (o.lfsr_ph >= 1.0f && steps < kLfsrMax) {
    o.lfsr_ph -= 1.0f;
    o.lfsr = lfsr_step(o.lfsr, short_mode);
    ++steps;
  }
  CHIP_CE_ASSERT(!(o.lfsr_ph >= 1.0f));  // the bound was not what ended the loop (dt is clamped to 18000 Hz)
  return (o.lfsr & 1u) != 0u ? 0.8f : -0.8f;
}

// SUPER SQR: three pulses, the second and third detuned by +-(0.002 + 0.018 Shape): dt2 = dt (1 + spread), dt3 = dt (1 - spread) with their reciprocals, mixed (p1 + 0.8 p2 + 0.8 p3) x 0.34.
template <class P>
CHIP_INLINE float super_square(Osc& o, float& phase, float dt, float inv, float dt2, float inv2, float dt3, float inv3, float duty) {
  advance_phase(phase, dt);
  o.p2 += dt2;
  if (o.p2 >= 1.0f) {
    o.p2 -= 1.0f;
  }
  o.p3 += dt3;
  if (o.p3 >= 1.0f) {
    o.p3 -= 1.0f;
  }
  float p1 = phase < duty ? 1.0f : -1.0f;
  p1 += P::blep(phase, dt, inv) - P::blep(P::wrap(phase - duty + 1.0f), dt, inv);
  float p2 = o.p2 < duty ? 1.0f : -1.0f;
  p2 += P::blep(o.p2, dt2, inv2) - P::blep(P::wrap(o.p2 - duty + 1.0f), dt2, inv2);
  float p3 = o.p3 < duty ? 1.0f : -1.0f;
  p3 += P::blep(o.p3, dt3, inv3) - P::blep(P::wrap(o.p3 - duty + 1.0f), dt3, inv3);
  return (p1 + 0.8f * p2 + 0.8f * p3) * 0.34f;
}

// ---- C5: FM BELL, CHIP DRUM -----------------------------------------------------------------------------------------------------------------------------
// FM BELL: a sine modulator at `ratio` x the carrier's phase step (its phase, o.p2, wraps by a floor: the step is up to 9 x 0.41), phase-modulated by the PWM's LFO (x 0.15); the carrier is a sine
// whose phase is moved by index x modulator x 0.5, quantised to 6 bits, x 0.85. `index_k` = 0.4 + 3.5 (0.35 + 0.65 PWM Depth), `env` the level the note has now (Chip's envelope): the index follows it.
template <class P>
CHIP_INLINE float fm_bell(Osc& o, float& phase, float dt, float ratio, float index_k, float env, float lfo) {
  advance_phase(phase, dt);
  o.p2 += dt * ratio;
  if (o.p2 >= 1.0f) {
    o.p2 -= P::floor(o.p2);
  }
  const float index = index_k * env;
  const float mod = P::sine(o.p2 + 0.15f * lfo);
  const float carrier = P::sine(phase + index * mod * 0.5f);
  return P::bits6(carrier);
}

// CHIP DRUM's decay at a control point: d = exp(-age / (0.08 sr)) from the host's exp2 (zero once it is below 1e-6: d^2 no longer moves the pitch), the start of a period of kControl samples in which Fast
// runs the recurrence d *= k. Anchoring every period keeps the recurrence's rounding (k is within 3e-8 of 1 - 2.9e-4) from adding up: it would have been 4e-4 of d after 3000 samples, and the stepped body
// flips with it. Called from control(), so the anchor falls at the same ages whatever the block size.
inline void drum_anchor(Osc& o, uint32_t age, float inv_sr, const o3m_host* h) {
  const float x = static_cast<float>(age) * (inv_sr * (1.442695041f / kDrumTau));
  o.drum_d = x < 20.0f ? h->exp2_f(-x) : 0.0f;
}

// The ratio FM BELL's Shape picks: the original's table at int(Shape x 7.99), held to 0..7.
inline float fm_ratio(float shape) {
  int32_t i = static_cast<int32_t>(shape * 7.99f);
  i = i < 0 ? 0 : (i > 7 ? 7 : i);
  return kFmRatios[i];
}

// CHIP DRUM: a stepped-triangle body whose pitch falls from `f (1 + 3.5 (1 - 0.7 Shape) d^2)` (d = exp(-age / (0.08 sr)), policy decay) to `f`, plus register noise stepped on every even age and on every wrap
// of the main phase (Shape above 0.75: the short sequence); noise 0.7 of the mix for the first 180 samples, then clamp(1.25 Shape). The body's phase step is f x (the decay's factor) x s: the original
// passes its frequency in Hz and 1 / sample rate (f, s), the module the phase step and 1.
template <class P>
CHIP_INLINE float drum(Osc& o, bool wrapped, uint32_t age, float f, float s, float shape, float tau) {
  const float d = P::decay(o, age, tau);
  const float drum_freq = f * (1.0f + 3.5f * (1.0f - shape * 0.7f) * d * d);
  o.p2 += drum_freq * s;
  if (o.p2 >= 1.0f) {
    o.p2 -= P::floor(o.p2);
  }
  if (wrapped || (age & 1u) == 0u) {
    o.lfsr = lfsr_step(o.lfsr, shape > 0.75f);
  }
  const float body = sub_step<P>(o.p2);  // the original's nesSteppedTriangle: the sub oscillator's stepped triangle
  const float noise_v = (o.lfsr & 1u) != 0u ? 0.85f : -0.85f;
  const float mix = age < 180u ? 0.7f : detail::clampf(shape * 1.25f, 0.0f, 1.0f);
  return (1.0f - mix) * body + mix * noise_v;
}

// Zap's pitch drop in semitones (added to the note): (12 + 24 shape) (z^3 - 1), z = 1 - p, p = the note's age over max(64 samples, Sweep Time), at most 1.
inline float zap_semitones(float shape, uint32_t age, float sweep_samples) {
  const float span = sweep_samples > 64.0f ? sweep_samples : 64.0f;
  float p = static_cast<float>(age) / span;
  p = p < 1.0f ? p : 1.0f;
  const float z = 1.0f - p;
  const float depth = 12.0f + 24.0f * shape;
  return depth * (z * z * z) - depth;
}

// Control rate for a wave of the new path: Chip's pitch sum (arpeggio or chord, Sweep, vibrato, the host's ramp), Zap's drop, then CE's clamp to 16..18000 Hz.
inline void control(Voice& v) {
  if (v.wave < 4u) {
    chip::control(v);  // waves 0..3 on this path (the post chain is on): their own pitch, clamp and noise clock
  } else {
    float semis = pitch_semitones(v);
    if (v.wave == kZap) {
      semis += zap_semitones(v.ce_shape, v.age, v.sweep_ms * v.rate_ms);
    }
    float inc = v.inc_unit * v.host->exp2_f(semis * (1.0f / 12.0f));
    const float lo = kMinHz * v.inv_sr;
    float hi = kMaxHz * v.inv_sr;
    hi = hi > kMaxStep ? kMaxStep : hi;  // 44100 Hz: 0.408, unchanged; at 22050 and 11025 Hz: below Nyquist
    inc = inc < lo ? lo : (inc > hi ? hi : inc);
    v.inc = inc;
    v.inv_inc = 1.0f / inc;
  }
  if (v.wave == kDrum) {
    drum_anchor(v.osc, v.age, v.inv_sr, v.host);  // one more exp2_f a period, on this wave only
  }
  // the lowpass: its coefficient from the envelope at this control point (one exp2_f and one sin_f)
  v.svf_f = svf_coef<Fast>(v.host, v.cutoff, v.env_amt, v.env, v.resonance, v.inv_sr);
}

// The source of waves 0..3 on this path: run<W>'s oscillator without the envelope and the gain (the post chain comes between them), line for line.
template <uint32_t W>
inline void raw(Voice& v, float* x, uint32_t n) {
  float phase = v.phase, nph = v.nphase, nval = v.nval;
  const float inc = v.inc, inv = v.inv_inc, duty = v.duty, dc = v.dc, nclk = v.nclk;
  const uint32_t rawedges = v.raw;
  uint32_t lfsr = v.lfsr;
  for (uint32_t i = 0; i < n; ++i) {
    float s;
    if (W == kPulse) {
      phase += inc;
      phase -= phase >= 1.0f ? 1.0f : 0.0f;
      s = phase < duty ? 1.0f : -1.0f;
      if (rawedges == 0u) {
        float t2 = phase - duty;
        t2 += t2 < 0.0f ? 1.0f : 0.0f;
        s += chip::blep(phase, inc, inv) - chip::blep(t2, inc, inv);
      }
      s -= dc;
    } else if (W == kTriangle) {
      phase += inc;
      phase -= phase >= 1.0f ? 1.0f : 0.0f;
      const float a = phase - 0.5f;
      s = 4.0f * (a < 0.0f ? -a : a) - 1.0f;
    } else {
      nph += nclk;
      if (nph >= 1.0f) {
        nph -= 1.0f;
        lfsr = lfsr_step(lfsr, W == kNoiseShort);
        nval = (lfsr & 1u) != 0u ? -1.0f : 1.0f;
      }
      s = nval;
    }
    x[i] = s;
  }
  v.phase = phase;
  v.nphase = nph;
  v.nval = nval;
  v.lfsr = lfsr;
}

// ---- the sources of waves 4..11: one loop of its own a model over a chunk (at most kControl samples, the pitch fixed), the state in locals, the result in out[] -------------------------------
// PWM's LFO phase, the main phase and the models' own state are written back at the end. Each is out of line, so that its loop has the registers to itself.
__attribute__((noinline)) inline void src_pulse(Voice& v, float* out, uint32_t n) {  // 2A03 PULSE, ZAP/LASER, ARP CHORD
  float phase = v.phase, pwm = v.pwm_ph;
  const float inc = v.inc, inv = v.inv_inc, step = v.pwm_step, base = v.pwm_base, k = v.pwm_k;
  for (uint32_t i = 0; i < n; ++i) {
    const float duty = pwm_duty(pwm, step, base, k);
    out[i] = pulse<Fast>(phase, inc, inv, duty);
  }
  v.phase = phase;
  v.pwm_ph = pwm;
}

__attribute__((noinline)) inline void src_sid(Voice& v, float* out, uint32_t n) {
  float phase = v.phase, pwm = v.pwm_ph, sub_ph = v.post.sub_ph;  // sub_ph: advanced here as the post chain advances it, to read the phase of the sample before
  const float inc = v.inc, inv = v.inv_inc, step = v.pwm_step, base = v.pwm_base, k = v.pwm_k;
  for (uint32_t i = 0; i < n; ++i) {
    const float duty = pwm_duty(pwm, step, base, k);
    out[i] = sid<Fast>(phase, sub_ph, inc, inv, duty);
    sub_advance(sub_ph, inc);
  }
  v.phase = phase;
  v.pwm_ph = pwm;
}

__attribute__((noinline)) inline void src_noise(Voice& v, float* out, uint32_t n) {
  Osc o = v.osc;
  const float inc = v.inc, rate = 2.0f + 14.0f * (1.0f - v.pwm_depth);
  const bool short_mode = v.ce_shape >= 0.5f;
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = noise(o, inc, rate, short_mode);
  }
  v.osc = o;
}

__attribute__((noinline)) inline void src_super(Voice& v, float* out, uint32_t n) {
  float phase = v.phase, pwm = v.pwm_ph;
  Osc o = v.osc;
  const float dt = v.inc, inv = v.inv_inc, step = v.pwm_step, base = v.pwm_base, k = v.pwm_k;
  const float spread = 0.002f + 0.018f * v.ce_shape;
  const float dt2 = dt * (1.0f + spread), dt3 = dt * (1.0f - spread);
  const float inv2 = 1.0f / dt2, inv3 = 1.0f / dt3;  // twice a chunk, not a sample
  for (uint32_t i = 0; i < n; ++i) {
    const float duty = pwm_duty(pwm, step, base, k);
    out[i] = super_square<Fast>(o, phase, dt, inv, dt2, inv2, dt3, inv3, duty);
  }
  v.phase = phase;
  v.pwm_ph = pwm;
  v.osc = o;
}

__attribute__((noinline)) inline void src_fm(Voice& v, float* out, const float* lvl, uint32_t n) {
  float phase = v.phase, pwm = v.pwm_ph;
  Osc o = v.osc;
  const float inc = v.inc, step = v.pwm_step;
  const float ratio = fm_ratio(v.ce_shape), index_k = 0.4f + 3.5f * (0.35f + 0.65f * v.pwm_depth);
  for (uint32_t i = 0; i < n; ++i) {
    const float lfo = pwm_lfo(pwm, step);
    out[i] = fm_bell<Fast>(o, phase, inc, ratio, index_k, lvl[i], lfo);
  }
  v.phase = phase;
  v.pwm_ph = pwm;
  v.osc = o;
}

__attribute__((noinline)) inline void src_drum(Voice& v, float* out, uint32_t n) {
  float phase = v.phase;
  Osc o = v.osc;
  const float inc = v.inc, shape = v.ce_shape;
  const uint32_t age = v.age;
  for (uint32_t i = 0; i < n; ++i) {
    const bool wrapped = advance_phase(phase, inc);
    out[i] = drum<Fast>(o, wrapped, age + i, inc, 1.0f, shape, 0.0f);  // Fast's decay is the recurrence: no tau
  }
  v.phase = phase;
  v.osc = o;
}

// A chunk of at most kControl samples with the pitch fixed: Chip's envelope per sample (Vol Steps included, the same stage changes as run<W>), then the model's source with its PWM, times the level.
inline void chunk(Voice& v, float* out, uint32_t n) {
  float lvl[kControl];
  float e = v.env, t = v.target, sl = v.slope;
  const uint32_t steps4 = v.steps4;
  for (uint32_t i = 0; i < n; ++i) {
    if (e != t) {
      bool reached;
      if (e < t) {
        e += sl;
        reached = e >= t;
      } else {
        e -= sl;
        reached = e <= t;
      }
      if (reached) {
        e = t;
        v.env = e;
        advance(v);
        e = v.env;
        t = v.target;
        sl = v.slope;
      }
    }
    lvl[i] = steps4 != 0u ? static_cast<float>(static_cast<int32_t>(e * 15.0f + 0.5f)) * (1.0f / 15.0f) : e;
  }
  v.env = e;

  // the source, into out[]: the engine's pulse family with its PWM (waves 4..11), or today's oscillator of waves 0..3 (the post chain is on)
  const float inc = v.inc;
  if (v.wave >= 4u) {
    switch (v.wave) {
      case kSid: src_sid(v, out, n); break;
      case kNoise: src_noise(v, out, n); break;
      case kFmBell: src_fm(v, out, lvl, n); break;
      case kSuperSquare: src_super(v, out, n); break;
      case kDrum: src_drum(v, out, n); break;
      default: src_pulse(v, out, n); break;  // 2A03 PULSE, ZAP/LASER, ARP CHORD
    }
  } else if (v.wave == kPulse) {
    raw<kPulse>(v, out, n);
  } else if (v.wave == kTriangle) {
    raw<kTriangle>(v, out, n);
  } else if (v.wave == kNoiseLong) {
    raw<kNoiseLong>(v, out, n);
  } else {
    raw<kNoiseShort>(v, out, n);
  }

  // the post chain, each step skipped when it is off, then the level: x * envelope * gain
  Post p = v.post;
  const float sub = v.sub, gain = v.gain, f = v.svf_f, q = v.svf_q, scale = v.crush_scale, inv_scale = v.inv_crush;
  const bool sub_on = sub > 0.001f, crush_on = v.crush_on != 0u, hold = v.dec_n > 1u || crush_on;
  const uint32_t dec_n = v.dec_n;
  for (uint32_t i = 0; i < n; ++i) {
    float x = out[i];
    sub_advance(p.sub_ph, inc);
    if (sub_on) {
      x = sub_mix<Fast>(x, p.sub_ph, sub);
    }
    if (hold) {
      x = hold_crush<Fast>(p, x, dec_n, crush_on, scale, inv_scale);
    }
    if (f != 0.0f) {
      x = svf_step(p, x, f, q);
    }
    out[i] = x * lvl[i] * gain + 0.0f;  // + 0.0f: a negative sample at level 0 is -0.0, which a block that starts idle writes as +0.0 (block-size independence is bit for bit)
  }
  v.post = p;
  v.age += n;
}

// A block for the engine's waves: voice_render's chunk loop, with this path's control update and chunk. Cold: not inlined, not O3M_HOT (the hot render() tests Voice::path once a block and calls this).
// Returns 1 while the voice may still sound.
__attribute__((noinline)) inline uint32_t render(Voice& v, float* out, uint32_t frames) {
  uint32_t done = 0;
  while (done < frames) {
    if (v.ctl == 0u) {
      ce::control(v);
      v.ctl = kControl;
    }
    uint32_t n = frames - done;
    n = n < v.ctl ? n : v.ctl;
    chunk(v, out + done, n);
    v.ctl -= n;
    done += n;
  }
  return v.stage != kOff ? 1u : 0u;
}

}  // namespace ce
}  // namespace chip
