// SPDX-License-Identifier: MIT
//
// The voice of the Chip module (OS-012c): written from scratch, no third-party code. One mono voice: a pulse with four duty cycles (12.5 / 25 / 50 / 75 %), a triangle and
// a 15-bit LFSR noise (long and short sequence), optionally in 4-bit volume steps, with a fast linear envelope, a three-step arpeggio, a pitch sweep and a vibrato.
//
// Block based and float only. The oscillators run per sample; everything that needs an exp2 (the pitch) runs every kControl samples ("control rate", 1378 Hz at 44.1 kHz).
// The pulse is band-limited with PolyBLEP unless "Raw Edges" is on (the naive pulse of a real chip, aliasing included). Noise is a sample-and-hold of the LFSR, clocked at
// 16 x the note's frequency (never faster than the sample rate). All state is in Voice (the instance block); there are no globals and no statics.
//
// Waves 4..11 (OS-CECHIP) are a port of tracker-ce's CHIP engine, in chip_ce.h (MIT, with the notice of LICENSE-tracker-ce.txt); they run on a cold path outside the loops below, which are
// those of waves 0..3, unchanged.
#pragma once

#include <o3module.h>

#include <stdint.h>

#include "chip_gen.h"

// The audio path is meant to be inlined into the glue's render() (the one O3M_HOT function), so that the hot part of the file is just that loop; control() and advance() are
// called a few times a block and stay wherever the compiler puts them.
#define CHIP_INLINE __attribute__((always_inline)) inline

namespace chip {

constexpr uint32_t kControl = 32;  // samples between two pitch updates
constexpr float kNoiseClock = 16.0f;  // LFSR clock in units of the note's frequency
constexpr float kNoiseGain = 0.7f;
constexpr uint32_t kNoisePreroll = 200;  // steps the NOISE view runs before it draws: from the seed the first 14 outputs of the long register are all the same bit
constexpr float kMinRampMs = 1.0f;  // attack and release are never shorter: a hard edge in the amplitude is a click

enum Stage : uint32_t { kOff = 0, kAttack, kDecay, kRelease };
enum Wave : uint32_t { kPulse = 0, kTriangle, kNoiseLong, kNoiseShort };

// The 15-bit LFSR of the noise: the feedback is bit 0 xor bit 1 (long: period 32767) or bit 0 xor bit 6 (short: period 93 from the seed 1, the metallic one).
inline uint32_t lfsr_step(uint32_t r, bool short_mode) {
  const uint32_t fb = (r ^ (r >> (short_mode ? 6u : 1u))) & 1u;
  return (r >> 1) | (fb << 14);
}

// PolyBLEP residual of a unit step (height 2) at phase 0: t is the phase since the edge (0..1), dt the phase step of a sample, inv = 1 / dt.
inline float blep(float t, float dt, float inv) {
  if (t < dt) {
    const float x = t * inv;
    return x + x - x * x - 1.0f;
  }
  if (t > 1.0f - dt) {
    const float x = (t - 1.0f) * inv;
    return x * x + x + x + 1.0f;
  }
  return 0.0f;
}

// The post chain's state (OS-CECHIP C3, chip_ce.h): the sub oscillator's phase, the decimator's counter and held sample, the state-variable lowpass.
struct Post {
  float sub_ph, held, low, band;
  uint32_t cnt;
};

// The engine's models' own state (OS-CECHIP C4, C5): the original's oscillator phases beyond the main one, the noise clock and its register, the drum's pitch decay.
struct Osc {
  float p2, p3;          // Super Square's second and third pulse; FM BELL's modulator and CHIP DRUM's body (the original shares osc2Phase between them)
  float lfsr_ph;         // NOISE/LFSR: the phase of the register's clock
  uint32_t lfsr;         // the original's 15-bit register (NOISE/LFSR and the drum's noise), reseeded to 0x7FFF at a note
  float drum_d, drum_k;  // CHIP DRUM: the pitch decay exp(-age / (0.08 sr)) (policy Fast: from the host's exp2 at each control point, then the recurrence d *= k, k = exp(-1 / (0.08 sr)))
};

// What an instance holds (the manifest's state_bytes is the room: static_assert in chip.cpp).
struct Voice {
  const o3m_host* host;
  float rate_ms;    // samples in a millisecond
  float inc_unit;   // phase step a sample of "note 0": 440 * 2^(-69/12) / rate
  // oscillators
  float phase, inc, inv_inc;  // pulse / triangle: 0..1, step a sample, 1 / step
  float duty, dc;             // pulse: the fraction that is high, and the offset that removes the mean (2 duty - 1)
  float nphase, nclk, nval;   // noise: phase of the LFSR clock (0..1), its step a sample (at most 1), the held value
  uint32_t lfsr;
  // envelope: env moves to target by slope a sample; reaching it advances the stage
  float env, target, slope;
  uint32_t stage;
  float a_ms, d_ms, sus, r_ms;
  // gain
  float volume, velocity, vel_sens, gain;
  uint32_t wave, raw, steps4;
  // pitch
  float note, transpose, fine;  // the note being played; the transposition and the fine tune (cents / 100), in semitones
  float host_off, host_slope;   // OS-MODPITCH: the host's Slide / Glide offset from the note (semitones) and its change a control period; pitch() sets them, control() adds the offset
  float arp1, arp2, arp_acc, arp_step;
  uint32_t arp_len, arp_idx;
  float sweep, sweep_ms, sweep_cur, sweep_dec;
  float vib_depth, vib_step, vib_ph, vib_amp, vib_ramp;
  uint32_t ctl;  // samples until the next control update
  // OS-CECHIP: waves 4..11 and the parameters 20..29. path: the wave for 0..3 (today's loops), 4 for the engine's waves (chip_ce.h).
  uint32_t path, age;           // age: samples since the note started (Zap's drop)
  float inv_sr;                 // 1 / sample rate
  float ce_shape;               // Shape / 100: the original's Shape (duty base, Zap's depth)
  float pwm_base, pwm_k, pwm_step, pwm_ph;  // PWM: 0.125 + 0.75 shape, 0.38 depth, rate / sample rate, the LFO's phase
  float pwm_depth;              // PWM Depth / 100: the original's value (the noise's clock and the FM index read it)
  float sub, cutoff, resonance, env_amt;  // the post chain's parameters (Sub Osc / 100, Cutoff in Hz, Resonance / 100, Env Amt / 100; Bitcrush and Decimate are crush_scale and dec_n below)
  Post post;                    // its state
  float svf_f, svf_q;           // the lowpass's coefficient at the last control point (0: the filter is skipped, as the original skips it) and its damping
  float crush_scale, inv_crush;  // 2^(bits - 1) and its inverse when Bitcrush is below 15.5 bits
  uint32_t crush_on, dec_n;     // Bitcrush below 15.5 bits; the decimation factor 1..32
  uint32_t chord;               // Chord 0..7
  float arp_off[4];             // the arpeggio's offsets from the note: 0, Arp 1, Arp 2, 0 or a chord's four steps
  Osc osc;                      // the engine's models' own state
};

namespace detail {

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN is lo
inline float roundf_(float x) { return static_cast<float>(static_cast<int32_t>(x + (x >= 0.0f ? 0.5f : -0.5f))); }

}  // namespace detail

// ---- envelope --------------------------------------------------------------------------------------------------------------------------------------------
// Slopes are per sample; a time is milliseconds * rate_ms. Linear, like the hardware's counters, but per sample (no 60 Hz zipper).

inline void enter_attack(Voice& v) {
  v.stage = kAttack;
  v.target = 1.0f;
  v.slope = 1.0f / ((v.a_ms > kMinRampMs ? v.a_ms : kMinRampMs) * v.rate_ms);  // a full-scale ramp: from a level above zero it is proportionally shorter
}

inline void enter_decay(Voice& v) {
  v.stage = kDecay;
  v.target = v.sus;
  const float n = v.d_ms * v.rate_ms;
  if (n < 1.0f) {
    v.env = v.sus;  // no decay time: a step, as on the chip
    v.slope = 0.0f;
  } else {
    const float span = 1.0f - v.sus;
    v.slope = (span > 0.05f ? span : 0.05f) / n;  // the time is that of the fall from full scale to the sustain level
  }
  if (v.env == v.sus && v.sus <= 0.0f) {
    v.stage = kOff;  // decays to nothing: the note is over (a percussive sound)
  }
}

inline void enter_release(Voice& v) {
  if (v.env <= 0.0f) {
    v.stage = kOff;  // a note off before the first sample of the note: nothing to release (a release from 0 would never reach its target and keep the voice busy)
    return;
  }
  v.stage = kRelease;
  v.target = 0.0f;
  const float n = (v.r_ms > kMinRampMs ? v.r_ms : kMinRampMs) * v.rate_ms;
  v.slope = (v.env > 1.0e-4f ? v.env : 1.0e-4f) / n;  // from where it is to zero in the release time
}

// The envelope reached its target.
inline void advance(Voice& v) {
  switch (v.stage) {
    case kAttack: enter_decay(v); break;
    case kDecay:
      if (v.sus <= 0.0f) {
        v.stage = kOff;
      } else {
        v.slope = 0.0f;  // held at the sustain level until the note ends
      }
      break;
    default: v.stage = kOff; break;
  }
}

// ---- control rate -------------------------------------------------------------------------------------------------------------------------------------
// The pitch: note + tune + arpeggio (or chord) + sweep + vibrato, at control rate: the sum in semitones, advancing the arpeggio, the sweep and the vibrato. The host's exp2 is the caller's.
inline float pitch_semitones(Voice& v) {
  float off = 0.0f;
  if (v.arp_len > 1u) {
    v.arp_acc += v.arp_step;  // at most 120 steps a second at 32-sample ticks: never two steps in one tick
    if (v.arp_acc >= 1.0f) {
      v.arp_acc -= 1.0f;
      v.arp_idx = v.arp_idx + 1u >= v.arp_len ? 0u : v.arp_idx + 1u;
    }
    off = v.arp_off[v.arp_idx];  // [0] is 0, [1] Arp 1, [2] Arp 2 (a chord: its four steps)
  }
  float sw = v.sweep_cur;
  if (sw > v.sweep_dec) {
    sw -= v.sweep_dec;
  } else if (sw < -v.sweep_dec) {
    sw += v.sweep_dec;
  } else {
    sw = 0.0f;
  }
  v.sweep_cur = sw;
  v.vib_ph += v.vib_step;
  v.vib_ph -= v.vib_ph >= 1.0f ? 1.0f : 0.0f;
  float a = v.vib_ph - 0.5f;
  a = a < 0.0f ? -a : a;
  v.vib_amp += v.vib_ramp;
  v.vib_amp = v.vib_amp > 1.0f ? 1.0f : v.vib_amp;
  const float vib = (4.0f * a - 1.0f) * v.vib_depth * v.vib_amp;  // a triangle, -1..1, zero at phase 0.25
  const float host = v.host_off;  // the host's ramp (ABI 1.8): the offset now, then the next period's
  v.host_off = host + v.host_slope;
  return v.note + v.transpose + v.fine + off + sw + vib + host;
}

// The control update of waves 0..3, at the control rate: the pitch sum through the host's exp2. Out of line, as the compiler had it before OS-CECHIP (inlined into render() it would add 670 B to the
// hot part). The engine's waves have chip_ce.h's.
__attribute__((noinline)) inline void control(Voice& v) {
  float inc = v.inc_unit * v.host->exp2_f(pitch_semitones(v) * (1.0f / 12.0f));
  inc = inc > 0.45f ? 0.45f : inc;  // below Nyquist whatever the transposition
  v.inc = inc;
  v.inv_inc = 1.0f / inc;
  const float nclk = inc * kNoiseClock;
  v.nclk = nclk > 1.0f ? 1.0f : nclk;
}

}  // namespace chip

#include "chip_ce.h"  // OS-CECHIP: waves 4..11 and the post chain (the engine's port); needs Voice, advance(), pitch_semitones() and control() above

namespace chip {

// The control update of the wave's path (a note's start; a sliding note).
inline void control_any(Voice& v) {
  if (v.path < 4u) {
    control(v);
  } else {
    ce::control(v);
  }
}

// ---- parameters ---------------------------------------------------------------------------------------------------------------------------------------
// The two noise waves (and no other: the engine's waves 4..11 have their own level) take the noise gain.
inline void derive_gain(Voice& v) {
  v.gain = v.volume * (1.0f - v.vel_sens * (1.0f - v.velocity)) * (v.wave == kNoiseLong || v.wave == kNoiseShort ? kNoiseGain : 1.0f);
}

// The path: the wave for 0..3 (today's loops) while the post chain is off and the wave is 0..3, else 4 (chip_ce.h). The post chain is off at the defaults: Sub Osc 0, Cutoff 18000 with Resonance 0 and
// Env Amt 0 (the original skips its filter then), Bitcrush 16, Decimate 1. A filter whose cutoff the envelope moves (Env Amt not 0) is on.
inline void refresh_path(Voice& v) {
  const bool post = v.sub > 0.001f || v.crush_on != 0u || v.dec_n > 1u || v.env_amt != 0.0f || v.resonance > 0.02f || v.cutoff < 17500.0f;
  v.path = (v.wave >= 4u || post) ? 4u : v.wave;
}

// The arpeggio's offsets and length: a chord's four steps when Chord is not Off or the wave is ARP CHORD (which plays Major while Chord is Off), else Arp 1 and Arp 2 as before.
inline void refresh_arp(Voice& v) {
  const uint32_t c = v.chord != 0u ? v.chord : (v.wave == ce::kArpChord ? ce::kChordMajor : 0u);
  if (c != 0u) {
    for (uint32_t k = 0; k < 4u; ++k) {
      v.arp_off[k] = static_cast<float>(ce::kChords[c][k]);
    }
    v.arp_len = 4u;
  } else {
    v.arp_off[0] = 0.0f;
    v.arp_off[1] = v.arp1;
    v.arp_off[2] = v.arp2;
    v.arp_off[3] = 0.0f;
    v.arp_len = v.arp2 != 0.0f ? 3u : (v.arp1 != 0.0f ? 2u : 1u);
  }
}

// The OS does not clamp: every value is held to the manifest's range here (a stored blob can hold anything, NaN included), and the whole-number ones rounded.
inline void voice_set_param(Voice& v, uint32_t id, float value) {
  if (id >= CHIP_PARAM_COUNT) {
    return;
  }
  const o3m_param& p = chip_params[id];
  value = detail::clampf(value, p.min, p.max);
  if ((p.flags & O3M_PARAM_INT) != 0u) {
    value = detail::roundf_(value);
  }
  const float rate_ms = v.rate_ms;
  switch (id) {
    case CHIP_PARAM_WAVE:
      v.wave = static_cast<uint32_t>(value);
      refresh_path(v);
      derive_gain(v);
      refresh_arp(v);
      break;
    case CHIP_PARAM_DUTY: {
      const uint32_t i = static_cast<uint32_t>(value);
      v.duty = i == 3u ? 0.75f : 0.125f * static_cast<float>(1u << i);  // 12.5, 25, 50, 75 %
      v.dc = 2.0f * v.duty - 1.0f;
      break;
    }
    case CHIP_PARAM_RAW_EDGES: v.raw = value >= 0.5f ? 1u : 0u; break;
    case CHIP_PARAM_VOL_STEPS: v.steps4 = value >= 0.5f ? 1u : 0u; break;
    case CHIP_PARAM_VOLUME:
      v.volume = v.host->exp2_f(value * 0.16609640f);  // dB to a ratio: 2^(dB / 6.0206)
      derive_gain(v);
      break;
    case CHIP_PARAM_VEL_SENS:
      v.vel_sens = value * 0.01f;
      derive_gain(v);
      break;
    case CHIP_PARAM_TRANSPOSE: v.transpose = value; break;
    case CHIP_PARAM_FINE: v.fine = value * 0.01f; break;
    case CHIP_PARAM_ATTACK: v.a_ms = value; break;
    case CHIP_PARAM_DECAY: v.d_ms = value; break;
    case CHIP_PARAM_SUSTAIN:
      v.sus = value * 0.01f;
      if (v.stage == kDecay) {
        enter_decay(v);  // the level the note holds moved: aim at the new one
      }
      break;
    case CHIP_PARAM_RELEASE: v.r_ms = value; break;
    case CHIP_PARAM_SWEEP: v.sweep = value; break;
    case CHIP_PARAM_SWEEP_TIME: v.sweep_ms = value; break;
    case CHIP_PARAM_VIB_DEPTH: v.vib_depth = value * 0.01f; break;
    case CHIP_PARAM_VIB_RATE: v.vib_step = value * static_cast<float>(kControl) * 0.001f / rate_ms; break;
    case CHIP_PARAM_VIB_DELAY: v.vib_ramp = value >= 1.0f ? static_cast<float>(kControl) / (value * rate_ms) : 1.0f; break;
    case CHIP_PARAM_ARP_1:
    case CHIP_PARAM_ARP_2:
      if (id == CHIP_PARAM_ARP_1) {
        v.arp1 = value;
      } else {
        v.arp2 = value;
      }
      refresh_arp(v);
      break;
    case CHIP_PARAM_ARP_SPEED: v.arp_step = value * static_cast<float>(kControl) * 0.001f / rate_ms; break;
    // OS-CECHIP: the original's parameters, in the original's units (the manifest's percent is /100). Shape, PWM and Chord shape the new waves (Chord every wave); the post chain's
    // parameters (Sub Osc, Cutoff, Resonance, Bitcrush, Env Amt, Decimate) are held here for slice C3.
    case CHIP_PARAM_SHAPE:
      v.ce_shape = value * 0.01f;
      v.pwm_base = 0.125f + 0.75f * v.ce_shape;
      break;
    case CHIP_PARAM_SUB_OSC:
      v.sub = value * 0.01f;
      refresh_path(v);
      break;
    case CHIP_PARAM_CUTOFF:
      v.cutoff = value;
      refresh_path(v);
      break;
    case CHIP_PARAM_RESONANCE:
      v.resonance = value * 0.01f;
      v.svf_q = detail::clampf(1.0f - 0.85f * v.resonance, 0.12f, 1.0f);
      refresh_path(v);
      break;
    case CHIP_PARAM_BITCRUSH:
      v.crush_on = value < 15.5f ? 1u : 0u;
      v.crush_scale = v.crush_on != 0u ? v.host->exp2_f(value - 1.0f) : 32768.0f;
      v.inv_crush = 1.0f / v.crush_scale;
      refresh_path(v);
      break;
    case CHIP_PARAM_PWM_DEPTH:
      v.pwm_depth = value * 0.01f;
      v.pwm_k = 0.38f * v.pwm_depth;
      break;
    case CHIP_PARAM_PWM_RATE: v.pwm_step = value * v.inv_sr; break;
    case CHIP_PARAM_ENV_AMT:
      v.env_amt = value * 0.01f;
      refresh_path(v);
      break;
    case CHIP_PARAM_DECIMATE:
      v.dec_n = static_cast<uint32_t>(value);  // a whole number 1..32 (the manifest's INT range)
      refresh_path(v);
      break;
    case CHIP_PARAM_CHORD:
      v.chord = static_cast<uint32_t>(value);
      refresh_arp(v);
      break;
    default: break;
  }
}

inline void voice_init(Voice& v, const o3m_host* host) {
  v.host = host;
  const float rate = host->sample_rate;
  v.rate_ms = rate * 0.001f;
  v.inv_sr = 1.0f / rate;
  v.inc_unit = 440.0f / rate * host->exp2_f(-69.0f / 12.0f);
  v.lfsr = 1u;
  v.osc.lfsr = ce::kLfsrSeed;
  v.osc.drum_d = 1.0f;
  v.osc.drum_k = host->exp2_f(-1.442695041f / (ce::kDrumTau * rate));  // exp(-1 / (0.08 sr)) = 2^(-log2(e) / (0.08 sr))
  v.nval = 1.0f;
  v.note = 69.0f;
  v.velocity = 1.0f;
  v.inv_inc = 1.0f;
  v.vib_ramp = 1.0f;
  v.vib_amp = 1.0f;
  for (uint32_t i = 0; i < CHIP_PARAM_COUNT; ++i) {
    voice_set_param(v, i, chip_params[i].def);
  }
}

// `slide`: a note the host ramps over the one that sounds (O3M_NOTE_LEGATO | O3M_NOTE_RAMP, OS-MODPITCH): the envelope, the arpeggio, the sweep and the vibrato go on, only the pitch moves to the note.
inline void voice_note_on(Voice& v, uint32_t note, uint32_t velocity, bool slide = false) {
  v.host_off = 0.0f;  // a note puts the host's offset back to 0
  v.host_slope = 0.0f;
  if (slide && v.stage != kOff) {
    v.note = static_cast<float>(note > 127u ? 127u : note);
    control_any(v);
    v.ctl = kControl;
    return;
  }
  v.note = static_cast<float>(note > 127u ? 127u : note);
  v.velocity = static_cast<float>(velocity < 1u ? 1u : (velocity > 127u ? 127u : velocity)) * (1.0f / 127.0f);
  derive_gain(v);
  v.arp_idx = 0;
  v.arp_acc = 0.0f;
  v.sweep_cur = v.sweep_ms > 0.0f ? v.sweep : 0.0f;
  const float sw = v.sweep < 0.0f ? -v.sweep : v.sweep;
  v.sweep_dec = v.sweep_ms > 0.0f ? sw * static_cast<float>(kControl) / (v.sweep_ms * v.rate_ms) : 0.0f;
  v.vib_ph = 0.25f;
  v.vib_amp = v.vib_ramp >= 1.0f ? 1.0f : 0.0f;
  v.age = 0u;
  v.osc.lfsr = ce::kLfsrSeed;  // the original reseeds its register at every note that is not a legato step (a slide returned above)
  v.osc.drum_d = 1.0f;
  if (v.stage == kOff) {
    v.env = 0.0f;
    v.phase = 0.0f;
    v.nphase = 0.0f;
    v.pwm_ph = 0.0f;
    v.osc.p2 = 0.0f;
    v.osc.p3 = 0.0f;
    v.osc.lfsr_ph = 0.0f;
    v.post = Post{};  // new Community OS behaviour: a fresh note starts the post chain clean (sub phase, decimator, held sample, lowpass); the original's onNoteOn never resets them
  }
  enter_attack(v);
  control_any(v);
  v.ctl = kControl;
}

inline void voice_note_off(Voice& v) {
  if (v.stage != kOff) {
    enter_release(v);
  }
}

// OS-MODPITCH (ABI 1.8): from the next control period the pitch is the note + `semitones`, and it moves by `per_frame` a frame (a control period is kControl frames). Just numbers: the exp2 is the control's.
inline void voice_pitch(Voice& v, float semitones, float per_frame) {
  v.host_off = semitones;
  v.host_slope = per_frame * static_cast<float>(kControl);
}

// ---- audio ----------------------------------------------------------------------------------------------------------------------------------------------
// One chunk of at most kControl samples with the pitch fixed: W is the waveform, so each loop is as tight as it can be.
template <uint32_t W>
CHIP_INLINE void run(Voice& v, float* out, uint32_t n) {
  float phase = v.phase, nph = v.nphase, nval = v.nval;
  float e = v.env, t = v.target, sl = v.slope;
  const float inc = v.inc, inv = v.inv_inc, duty = v.duty, dc = v.dc, nclk = v.nclk, gain = v.gain;
  const uint32_t raw = v.raw, steps4 = v.steps4;
  uint32_t lfsr = v.lfsr;
  for (uint32_t i = 0; i < n; ++i) {
    float s;
    if (W == kPulse) {
      phase += inc;
      phase -= phase >= 1.0f ? 1.0f : 0.0f;
      s = phase < duty ? 1.0f : -1.0f;
      if (raw == 0u) {
        float t2 = phase - duty;
        t2 += t2 < 0.0f ? 1.0f : 0.0f;
        s += blep(phase, inc, inv) - blep(t2, inc, inv);
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
    const float lvl = steps4 != 0u ? static_cast<float>(static_cast<int32_t>(e * 15.0f + 0.5f)) * (1.0f / 15.0f) : e;
    out[i] = s * lvl * gain;
  }
  v.phase = phase;
  v.nphase = nph;
  v.nval = nval;
  v.lfsr = lfsr;
  v.env = e;
}

// Fills `frames` samples; returns 1 while the voice may still sound. The engine's waves (path 4) are one cold call, ce::render; below that the loop is waves 0..3's exactly as it was, and
// the dispatch's last branch (NOISE SHORT) is reached by wave 3 alone.
CHIP_INLINE uint32_t voice_render(Voice& v, float* out, uint32_t frames) {
  if (v.stage == kOff) {
    for (uint32_t i = 0; i < frames; ++i) {
      out[i] = 0.0f;
    }
    return 0;
  }
  if (v.path >= 4u) {
    return ce::render(v, out, frames);
  }
  uint32_t done = 0;
  while (done < frames) {
    if (v.ctl == 0u) {
      control(v);
      v.ctl = kControl;
    }
    uint32_t n = frames - done;
    n = n < v.ctl ? n : v.ctl;
    const uint32_t w = v.wave;
    if (w == kPulse) {
      run<kPulse>(v, out + done, n);
    } else if (w == kTriangle) {
      run<kTriangle>(v, out + done, n);
    } else if (w == kNoiseLong) {
      run<kNoiseLong>(v, out + done, n);
    } else {
      run<kNoiseShort>(v, out + done, n);
    }
    v.ctl -= n;
    done += n;
  }
  return v.stage != kOff ? 1u : 0u;
}

}  // namespace chip
