// SPDX-License-Identifier: MIT
//
// The voice of the SID module (OS-SID slice 1): written from scratch, no third-party code, no emulator source or data. One mono voice that imitates the MOS 6581 / 8580 voice without
// emulating it: saw, triangle, pulse (a width and a built-in PWM LFO), a 23-bit LFSR noise, four combined waves from the module's own tables, a modulator oscillator for hard sync (sub-sample,
// with a BLEP on the reset) and ring modulation, the stepped attack / decay / sustain / release of the Commodore 64 table, and Chip's control-rate pitch block (arpeggio, sweep, vibrato, the
// host's slide through O3M_NOTE_RAMP), and (slice 2) the filter: a zero-delay-feedback state-variable filter with a Mode mix, Resonance, a Cutoff curve per model, Key Track, Env Amount and a level-compensated Drive.
//
// Block based and float only. The oscillators run per sample; everything that needs an exp2 (the pitch) runs every kControl samples ("control rate", 1378 Hz at 44.1 kHz). The audio path is
// inlined into the glue's render() (the one O3M_HOT function); the combined-wave tables are const data placed in the hot part too, so the loop never reads SDRAM. All state is in Voice.
#pragma once

#include <o3module.h>

#include <stdint.h>

#include "sid_gen.h"

#define SID_INLINE __attribute__((always_inline)) inline
#define SID_COLD __attribute__((noinline))
#if defined(__arm__)
#define SID_HOT_RODATA __attribute__((section(".text.o3m_hot.tab")))  // matched by the linker script's *(.text.o3m_hot.*): the table lives in the SRAM slab with the loops
#else
#define SID_HOT_RODATA
#endif

namespace sid {

constexpr uint32_t kControl = 32;  // samples between two pitch updates
constexpr float kClock = 985248.0f;  // the PAL clock of the chip, Hz: the unit the envelope's step periods are given in
constexpr float kNoiseClock = 16.0f;  // LFSR clock in units of the note's frequency: this module's own choice, not a measurement (open research question, README)
constexpr float kMinDuty = 0.01f;
constexpr float kMaxDuty = 0.99f;
constexpr float kMaxHostSemis = 127.0f;  // the host's slide / glide offset is held to this many semitones either way
constexpr float kMaxInc = 0.45f;  // phase step a sample: below Nyquist whatever the transposition
constexpr float kFilterMinE = 3.3219281f;  // log2(10 Hz): the lowest filter frequency
constexpr float kRingEnergy = 1e-9f;       // below this sum of squared integrator states a filter tail is over
constexpr float kClipKnee = 1.5f;          // the soft clip x (1 - x^2 / 6.75) peaks at 1 at this input and is held there

enum Stage : uint32_t { kOff = 0, kAttack, kDecay, kRelease };
enum Wave : uint32_t { kSaw = 0, kTriangle, kPulse, kNoise, kSawTri, kPulseSaw, kPulseTri, kPulseSawTri };
enum Loop : uint32_t { kLoopSaw = 0, kLoopTri, kLoopPulse, kLoopNoise, kLoopComb };

// ---- the C64 envelope table --------------------------------------------------------------------------------------------------------------------------------
// Commodore 64 Programmer's Reference Guide (Commodore, 1982), the SID chapter's attack and decay / release rate table (quoted on C64-Wiki's ADSR page): the time of a full-scale travel of the 8-bit envelope counter for each 4-bit rate value, in milliseconds. The attack column
// is 2 ms to 8 s; the decay / release column (three times the attack's) is 6 ms to 24 s. Decay and release go from the level they start at to the target at the same rate, so a fall from the top
// to a lower sustain level takes the proportional part of the time.
inline constexpr uint16_t kAttackMs[16] = {2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000};
inline constexpr uint16_t kDecayMs[16] = {6, 24, 48, 72, 114, 168, 204, 240, 300, 750, 1500, 2400, 3000, 9000, 15000, 24000};
constexpr uint32_t kCounterMax = 255;  // the envelope counter is 8 bits
constexpr uint32_t kSustainStep = 17;  // the 4-bit sustain value is compared with the counter as the byte (value x 0x11)
constexpr uint32_t kFrac = 24;         // the counter is held as 8.24 fixed point: a 24 s fall is 2.4e-4 steps a sample, below what a float adds to 255

// ---- the 23-bit noise ---------------------------------------------------------------------------------------------------------------------------------------
// A Fibonacci LFSR of 23 bits with the feedback bit 22 xor bit 17 (zero based; the polynomial x^23 + x^18 + 1, the maximal-length entry of the standard LFSR tap tables, period 2^23 - 1 =
// 8,388,607: tests/sid_test.cpp counts it). The output is 8 bits taken from fixed positions of the register. The positions are this module's own choice (open research question for parity
// with the chip's output taps; a different set changes only the colour of the noise, not its period).
constexpr uint32_t kLfsrMask = 0x7FFFFFu;
constexpr uint32_t kLfsrSeed = 0x5A5A5Au;

inline uint32_t lfsr_step(uint32_t r) { return ((r << 1) | (((r >> 22) ^ (r >> 17)) & 1u)) & kLfsrMask; }

SID_INLINE uint32_t noise_byte(uint32_t r) {
  return (((r >> 20) & 1u) << 7) | (((r >> 18) & 1u) << 6) | (((r >> 15) & 1u) << 5) | (((r >> 12) & 1u) << 4) | (((r >> 10) & 1u) << 3) | (((r >> 7) & 1u) << 2) | (((r >> 4) & 1u) << 1) |
         ((r >> 1) & 1u);
}

// ---- the combined waves -------------------------------------------------------------------------------------------------------------------------------------
// Four combinations (SAW+TRI, PULSE+SAW, PULSE+TRI, PULSE+SAW+TRI) x two models, 128 entries of 8 bits each (1 KiB; the loop interpolates linearly between them), generated at compile time by a documented formula; no table data, fitted
// parameter or constant from any emulator. For each phase index i (the top 8 bits of the phase):
//   saw = i;  tri = 2 i (i < 128) or 2 (255 - i);  both 8-bit.
//   1. the bits of the selected waveforms are ANDed:           SAW+TRI: saw & tri
//   2. a pulse in the mix pulls each bit down by its neighbour: PULSE+SAW: saw & (saw << 1);  PULSE+TRI: tri & (tri << 1);  PULSE+SAW+TRI: b & (b << 1) with b = saw & tri
//      (the pulse itself is not in the table: it is a gate, high for the duty and silent for the rest of the period, applied by the loop)
//   3. the 6581 pulls the bits down once more: a & (a << 1)
//   4. smoothing: a circular [1 2 1] / 4 filter, once for the 8580 and twice for the 6581 (the older chip is duller)
//   5. normalised so that the peak is 255 (8580) or 191 (6581: weaker, as the older chip's combined waves are).
// The formula runs on a 256-point phase grid and the table keeps the even points (the smoothing makes the odd ones redundant for a linear interpolation); the peak is taken over the kept points.
// They will not sound identical to a real chip: the numbers are not measurements. By-ear tuning is open.
constexpr uint32_t kTableN = 128;  // entries of a combined-wave table (a power of two: the loop wraps with a mask)

struct Tables {
  uint8_t t[8][kTableN];  // [model * 4 + combination][index]; model 0 = 6581, 1 = 8580
};

constexpr uint32_t tri8(uint32_t i) { return (i & 0x80u) != 0u ? ((~i & 0x7Fu) << 1) : ((i & 0x7Fu) << 1); }

constexpr Tables make_tables() {
  Tables out{};
  for (uint32_t m = 0; m < 2; ++m) {
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t v[256] = {};
      for (uint32_t i = 0; i < 256; ++i) {
        const uint32_t s = i, r = tri8(i);
        uint32_t a = 0;
        if (c == 0) {
          a = s & r;
        } else if (c == 1) {
          a = s & ((s << 1) & 0xFFu);
        } else if (c == 2) {
          a = r & ((r << 1) & 0xFFu);
        } else {
          const uint32_t b = s & r;
          a = b & ((b << 1) & 0xFFu);
        }
        if (m == 0) {
          a = a & ((a << 1) & 0xFFu);
        }
        v[i] = a << 8;  // 8 fractional bits for the smoothing
      }
      for (uint32_t pass = 0; pass < (m == 0 ? 2u : 1u); ++pass) {
        uint32_t w[256] = {};
        for (uint32_t i = 0; i < 256; ++i) {
          w[i] = (v[(i + 255u) & 255u] + 2u * v[i] + v[(i + 1u) & 255u] + 2u) >> 2;
        }
        for (uint32_t i = 0; i < 256; ++i) {
          v[i] = w[i];
        }
      }
      uint32_t peak = 1;
      for (uint32_t i = 0; i < 256; i += 2) {
        peak = v[i] > peak ? v[i] : peak;
      }
      const uint32_t top = m == 0 ? 191u : 255u;
      for (uint32_t i = 0; i < 256; i += 2) {
        out.t[m * 4 + c][i / 2u] = static_cast<uint8_t>((v[i] * top + peak / 2u) / peak);
      }
    }
  }
  return out;
}

alignas(4) inline constexpr Tables kTables SID_HOT_RODATA = make_tables();
static_assert(sizeof(Tables) == 8 * kTableN, "the tables are 1 KiB of bytes");
constexpr uint8_t table_peak(uint32_t n) {
  uint8_t p = 0;
  for (uint32_t i = 0; i < kTableN; ++i) {
    p = kTables.t[n][i] > p ? kTables.t[n][i] : p;
  }
  return p;
}
static_assert(table_peak(0) == 191 && table_peak(3) == 191 && table_peak(4) == 255 && table_peak(7) == 255, "the normalisation: 6581 peaks at 191, 8580 at 255");

// What the loop subtracts so that a combined wave has no DC: the integral of the table's value (0..1) from phase 0 to the pulse's width, since the pulse is a gate that lets the first part of the
// period through (SAW+TRI has no gate: the whole period). Sixteen knots a table (the running sum of 8 entries each), interpolated linearly at control rate; in cold flash, read only there.
struct Cum {
  uint16_t c[8][17];  // c[t][k] = the sum of the first 8 k entries of table t: at most 16 x 8 x 255 = 32,640
};
constexpr Cum make_cum() {
  Cum out{};
  for (uint32_t t = 0; t < 8; ++t) {
    uint32_t acc = 0;
    out.c[t][0] = 0;
    for (uint32_t k = 0; k < 16; ++k) {
      for (uint32_t i = 0; i < kTableN / 16u; ++i) {
        acc += kTables.t[t][(kTableN / 16u) * k + i];
      }
      out.c[t][k + 1] = static_cast<uint16_t>(acc);
    }
  }
  return out;
}
inline constexpr Cum kCum = make_cum();

// The DC of table t gated to the first fraction d (0..1) of the period, in output units before the factor 2.
inline float table_dc(uint32_t t, float d) {
  const float x = d * 16.0f;
  uint32_t k = static_cast<uint32_t>(x);
  k = k > 15u ? 15u : k;
  const float fr = x - static_cast<float>(k);
  return (static_cast<float>(kCum.c[t][k]) + fr * static_cast<float>(kCum.c[t][k + 1] - kCum.c[t][k])) * (1.0f / (static_cast<float>(kTableN) * 255.0f));
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

// What an instance holds (the manifest's state_bytes is the room: static_assert in sid.cpp).
struct Voice {
  const o3m_host* host;
  float rate_ms;         // samples in a millisecond
  float inc_unit;        // phase step a sample of "note 0": 440 * 2^(-69/12) / rate
  float clk_per_sample;  // chip clock cycles in a sample
  // oscillator
  float phase, inc, inv_inc;  // 0..1, step a sample, 1 / step
  // modulator (sync and ring)
  float mph, minc, inv_minc;  // phase, step a sample, 1 / step
  float mratio, mod_semi, mod_fine;
  float half_j;  // half the jump the next sync reset makes in the waveform (set one sample ahead, so that the BLEP can be applied on both sides of the reset)
  // pulse width: the duty ramps to its target over a control period; the PWM LFO moves the target
  float duty, dstep, pw_base, pwm_depth, pwm_ph, pwm_step;
  // noise
  float nphase, nclk, nval;
  uint32_t lfsr;
  // the combined wave's table
  const uint8_t* tab;
  uint32_t tab_idx;      // which of the eight tables
  float dcm, dcstep;     // the DC the combined wave subtracts, and its change a sample (it follows the duty's ramp)
  // envelope: the 8-bit counter, 8.24 fixed point, moves to target by slope a sample (integer adds: no float rounding on a slow rate); reaching it advances the stage
  uint32_t env, target, slope;
  uint32_t stage;
  uint32_t att_slope, dec_slope, rel_slope, sus;  // counter steps a sample (attack, decay, release) and the sustain level, all 8.24
  // gain
  float volume, velocity, vel_sens, gain;
  uint32_t wave, sync, ring, model;
  uint32_t sync_act;  // Sync as the last control period took it: the two increments below swap roles with it, so the audio loop reads this one and a toggle lands on a control boundary
  // pitch
  float note, transpose, fine;  // the note being played; the transposition and the fine tune (cents / 100), in semitones
  float host_off, host_slope;   // OS-MODPITCH: the host's Slide / Glide offset from the note (semitones) and its change a control period
  float arp1, arp2, arp_acc, arp_step;
  uint32_t arp_len, arp_idx;
  float sweep, sweep_ms, sweep_cur, sweep_dec;
  float vib_depth, vib_step, vib_ph, vib_amp, vib_ramp;
  uint32_t ctl;  // samples until the next control update
  // filter (a zero-delay-feedback state-variable filter after the envelope; one per instance)
  float fs1, fs2;                         // the two integrator states
  float fa1, fa2, fa3;                    // its coefficients (from the cutoff g and the damping k)
  float fcx, fc1, fc2;                    // the mix: out = fcx x + fc1 v1 + fc2 v2 (v1 the band, v2 the low pass)
  float fk, fpre, fpost;                  // damping, input gain (Drive over the model's headroom), output gain (the headroom times the Drive compensation)
  float fcut, fkt, fenv;                  // Cutoff 0..1, Key Track 0..1, Env Amount -1..1
  float flo, frange, fe_max, fe_last;     // log2 of the curve's lowest frequency, the octaves it spans, log2 of the highest frequency allowed, the exponent the coefficients were made for
  float fdrive;                           // Drive 0..1
  uint32_t fmode;                         // Mode 0..7
};

namespace detail {

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN is lo
inline float roundf_(float x) { return static_cast<float>(static_cast<int32_t>(x + (x >= 0.0f ? 0.5f : -0.5f))); }

}  // namespace detail

// ---- envelope --------------------------------------------------------------------------------------------------------------------------------------------
// The counter is 0..255; the output is its whole part (the chip's 8-bit staircase). A rate value r is a step period of time(r) x clock / 255 clock cycles (time: the table above); a sample is
// clk_per_sample cycles, so the counter moves clk_per_sample / period steps a sample. A new note starts its attack from the level the counter is at (the chip never resets the counter).

inline uint32_t counts_per_sample(const Voice& v, uint32_t ms) {
  const float period = static_cast<float>(ms) * (kClock * 0.001f / static_cast<float>(kCounterMax));  // clock cycles a counter step
  const uint32_t q = static_cast<uint32_t>(v.clk_per_sample / period * static_cast<float>(1u << kFrac) + 0.5f);  // (at most 2.9 steps a sample)
  return q > 0u ? q : 1u;
}

inline void enter_attack(Voice& v) {
  v.stage = kAttack;
  v.target = kCounterMax << kFrac;
  v.slope = v.att_slope;
}

inline void enter_decay(Voice& v) {
  v.stage = kDecay;
  v.target = v.sus;
  v.slope = v.dec_slope;
  if (v.env == v.sus && v.sus == 0u) {
    v.stage = kOff;  // a sustain of 0: the note is over once the decay is (a percussive sound)
  }
}

inline void enter_release(Voice& v) {
  if (v.env == 0u) {
    v.stage = kOff;  // a note off before the first sample of the note: nothing to release
    return;
  }
  v.stage = kRelease;
  v.target = 0u;
  v.slope = v.rel_slope;
}

// The envelope reached its target.
SID_COLD void advance(Voice& v) {
  switch (v.stage) {
    case kAttack: enter_decay(v); break;
    case kDecay:
      if (v.sus == 0u) {
        v.stage = kOff;
      } else {
        v.slope = 0u;  // held at the sustain level until the note ends
      }
      break;
    default: v.stage = kOff; break;
  }
}

// ---- the filter ----------------------------------------------------------------------------------------------------------------------------------------------
// A zero-delay-feedback state-variable filter (the trapezoidal-integrator form of the textbook state-variable filter: g = tan(pi fc / fs), damping k, a1 = 1 / (1 + g (g + k)), a2 = g a1,
// a3 = g a2; low pass v2, band pass v1, high pass x - k v1 - v2), unconditionally stable for k > 0. Mode mixes the three (the band pass weighted by k, so its peak is 1 at any resonance);
// Resonance 0..15 gives k = sqrt(2) (a Butterworth low pass, -3 dB at the cutoff) down to sqrt(2) / 20 in equal steps of the ratio. The cutoff is a curve over Cutoff 0..100 % per model
// (the module's own: the 8580 an exponential from 30 Hz to 12 kHz, the 6581 an S-shaped one from 150 Hz to 12 kHz) times 2^(Key Track x (key - 60) / 12), the Env Amount moves the position by the
// envelope level. Model 6581 clips its input and output hard (a soft clip that peaks at 1), the 8580 has four times the headroom; Drive raises the input into the clip and takes the gain back after it (level-compensated: full Drive is at most +6 dB over Mode OFF). New Community OS
// behaviour: it imitates the character of the chip's filter, it is not an emulation, and the chip's one filter shared by three voices is one filter per track here.
constexpr uint8_t kModeBits[8] = {0, 1, 2, 4, 5, 3, 6, 7};  // Mode -> bit 0 low pass, bit 1 band pass, bit 2 high pass: OFF, LP, BP, HP, NOTCH (LP + HP), LP+BP, BP+HP, ALL

SID_INLINE float sclip(float x) {
  x = x > kClipKnee ? kClipKnee : (x < -kClipKnee ? -kClipKnee : x);
  return x * (1.0f - (1.0f / (kClipKnee * kClipKnee * 3.0f)) * x * x);
}

// Remake the coefficients for the exponent E = log2(frequency); the host's exp2 / sin / cos are called here and nowhere else, at control rate and only when E moved, or when Mode or Resonance did.
inline void filter_coefs(Voice& v, float e) {
  const float f = v.host->exp2_f(e);
  const float ang = f * (3.14159265f / (v.rate_ms * 1000.0f));  // below 0.45 pi: the cosine stays above 0.15
  const float g = v.host->sin_f(ang) / v.host->cos_f(ang);
  const float k = v.fk;
  const float a1 = 1.0f / (1.0f + g * (g + k));
  v.fa1 = a1;
  v.fa2 = g * a1;
  v.fa3 = g * v.fa2;
  const uint32_t bits = kModeBits[v.fmode & 7u];
  const float lp = static_cast<float>(bits & 1u), bp = static_cast<float>((bits >> 1) & 1u), hp = static_cast<float>((bits >> 2) & 1u);
  v.fcx = hp;
  v.fc1 = k * (bp - hp);
  v.fc2 = lp - hp;
  v.fe_last = e;
}

// At control rate: the cutoff's exponent from the knob, the envelope, the key; the coefficients only when it changed by more than a tenth of a cent.
inline void filter_control(Voice& v, float key_semis) {
  float c = v.fcut + v.fenv * static_cast<float>(v.env >> kFrac) * (1.0f / static_cast<float>(kCounterMax));
  c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
  const float u = v.model == 0u ? c * c * (3.0f - 2.0f * c) : c;
  float e = v.flo + u * v.frange + v.fkt * (key_semis - 60.0f) * (1.0f / 12.0f);
  e = e < kFilterMinE ? kFilterMinE : (e > v.fe_max ? v.fe_max : e);
  const float d = e - v.fe_last;
  if (d > 1.5e-4f || d < -1.5e-4f) {
    filter_coefs(v, e);
  }
}

// ---- control rate -------------------------------------------------------------------------------------------------------------------------------------
// The pitch: note + tune + arpeggio + sweep + vibrato, at control rate; the modulator's step, the PWM LFO and the duty ramp. The host's exp2 is the only call out.
inline void control(Voice& v) {
  float off = 0.0f;
  if (v.arp_len > 1u) {
    v.arp_acc += v.arp_step;  // at most 120 steps a second at 32-sample ticks: never two steps in one tick
    if (v.arp_acc >= 1.0f) {
      v.arp_acc -= 1.0f;
      v.arp_idx = v.arp_idx + 1u >= v.arp_len ? 0u : v.arp_idx + 1u;
    }
    off = v.arp_idx == 0u ? 0.0f : (v.arp_idx == 1u ? v.arp1 : v.arp2);
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
  v.host_off = detail::clampf(host + v.host_slope, -kMaxHostSemis, kMaxHostSemis);
  const float key_semis = v.note + v.transpose + v.fine + off + sw + vib + host;
  float key = v.inc_unit * v.host->exp2_f(key_semis * (1.0f / 12.0f));  // the key's pitch: the phase step a sample
  key = key > kMaxInc ? kMaxInc : key;
  float other = key * v.mratio;  // the key plus Mod Pitch and Mod Fine
  other = other > kMaxInc ? kMaxInc : other;
  // Sync on: the modulator is the sync source and runs at the key's pitch (the pitch you hear), the oscillator at key + Mod Pitch, so that sweeping Mod Pitch sweeps the timbre at a fixed pitch.
  // Sync off: the oscillator is at the key and the modulator (ring) at key + Mod Pitch.
  v.sync_act = v.sync;
  if (v.fmode != 0u) {
    filter_control(v, key_semis);
  }
  const float inc = v.sync_act != 0u ? other : key;
  const float minc = v.sync_act != 0u ? key : other;
  v.inc = inc;
  v.inv_inc = 1.0f / inc;
  v.minc = minc;
  v.inv_minc = 1.0f / minc;
  const float nclk = key * kNoiseClock;
  v.nclk = nclk > 1.0f ? 1.0f : nclk;
  // the PWM LFO (a triangle, zero at phase 0.25) moves the duty's target; the duty ramps there over the period, by at most half a phase step a sample, so that an edge never runs back over
  // the phase and makes a second edge (a click)
  v.pwm_ph += v.pwm_step;
  v.pwm_ph -= v.pwm_ph >= 1.0f ? 1.0f : 0.0f;
  float p = v.pwm_ph - 0.5f;
  p = p < 0.0f ? -p : p;
  const float target = detail::clampf(v.pw_base + v.pwm_depth * (4.0f * p - 1.0f), kMinDuty, kMaxDuty);
  float ds = (target - v.duty) * (1.0f / static_cast<float>(kControl));
  const float lim = 0.5f * inc;
  ds = ds > lim ? lim : (ds < -lim ? -lim : ds);
  v.dstep = ds;
  // the combined wave's DC at the end of this period's duty ramp (the whole period when there is no gate), reached linearly
  const float d_end = v.wave == kSawTri ? 1.0f : v.duty + ds * static_cast<float>(kControl);
  v.dcstep = (table_dc(v.tab_idx, d_end > 1.0f ? 1.0f : (d_end < 0.0f ? 0.0f : d_end)) - v.dcm) * (1.0f / static_cast<float>(kControl));
}

// ---- parameters ---------------------------------------------------------------------------------------------------------------------------------------
inline void derive_gain(Voice& v) { v.gain = v.volume * (1.0f - v.vel_sens * (1.0f - v.velocity)) * (1.0f / static_cast<float>(kCounterMax)); }

inline void select_table(Voice& v) {
  const uint32_t c = v.wave >= kSawTri ? v.wave - kSawTri : 0u;
  v.tab = kTables.t[v.model * 4u + c];
  v.tab_idx = v.model * 4u + c;
  v.dcm = table_dc(v.tab_idx, v.wave == kSawTri ? 1.0f : v.duty);
  v.dcstep = 0.0f;
}

// Drive 0..1 multiplies the input by m = 1 + 15 Drive before the clip and the output is multiplied by head (1 + Drive) / m: the small-signal gain is 1 + Drive (at most +6 dB over Mode OFF at the same
// note and Volume) and sclip(x) <= x keeps every level at or under that, so Drive adds saturation and not level, on both models (head: the 6581's 1, the 8580's 4).
inline void derive_filter_input(Voice& v) {
  const float head = v.model == 0u ? 1.0f : 4.0f;
  const float m = 1.0f + 15.0f * v.fdrive;
  v.fpre = m / head;
  v.fpost = head * (1.0f + v.fdrive) / m;
}

inline void derive_mod_ratio(Voice& v) { v.mratio = v.host->exp2_f((v.mod_semi + v.mod_fine) * (1.0f / 12.0f)); }

// The OS does not clamp: every value is held to the manifest's range here (a stored blob can hold anything, NaN included), and the whole-number ones rounded.
inline void voice_set_param(Voice& v, uint32_t id, float value) {
  if (id >= SID_PARAM_COUNT) {
    return;
  }
  const o3m_param& p = sid_params[id];
  value = detail::clampf(value, p.min, p.max);
  if ((p.flags & O3M_PARAM_INT) != 0u) {
    value = detail::roundf_(value);
  }
  const float rate_ms = v.rate_ms;
  switch (id) {
    case SID_PARAM_WAVE:
      v.wave = static_cast<uint32_t>(value);
      select_table(v);
      break;
    case SID_PARAM_PULSE_WIDTH: v.pw_base = value * 0.01f; break;
    case SID_PARAM_PWM_DEPTH: v.pwm_depth = value * 0.005f; break;  // +-depth / 2 of the period: 100 % sweeps the whole range
    case SID_PARAM_PWM_RATE: v.pwm_step = value * static_cast<float>(kControl) * 0.001f / rate_ms; break;
    case SID_PARAM_SYNC: v.sync = value >= 0.5f ? 1u : 0u; break;
    case SID_PARAM_RING: v.ring = value >= 0.5f ? 1u : 0u; break;
    case SID_PARAM_MOD_PITCH:
      v.mod_semi = value;
      derive_mod_ratio(v);
      break;
    case SID_PARAM_MOD_FINE:
      v.mod_fine = value * 0.01f;
      derive_mod_ratio(v);
      break;
    case SID_PARAM_ATTACK: v.att_slope = counts_per_sample(v, kAttackMs[static_cast<uint32_t>(value)]); break;
    case SID_PARAM_DECAY: v.dec_slope = counts_per_sample(v, kDecayMs[static_cast<uint32_t>(value)]); break;
    case SID_PARAM_SUSTAIN:
      v.sus = static_cast<uint32_t>(value) * kSustainStep << kFrac;
      if (v.stage == kDecay) {
        enter_decay(v);  // the level the note holds moved: aim at the new one
      }
      break;
    case SID_PARAM_RELEASE: v.rel_slope = counts_per_sample(v, kDecayMs[static_cast<uint32_t>(value)]); break;
    case SID_PARAM_VOLUME:
      v.volume = v.host->exp2_f(value * 0.16609640f);  // dB to a ratio: 2^(dB / 6.0206)
      derive_gain(v);
      break;
    case SID_PARAM_VEL_SENS:
      v.vel_sens = value * 0.01f;
      derive_gain(v);
      break;
    case SID_PARAM_TRANSPOSE: v.transpose = value; break;
    case SID_PARAM_FINE: v.fine = value * 0.01f; break;
    case SID_PARAM_MODEL:
      v.model = static_cast<uint32_t>(value);
      select_table(v);
      v.flo = v.model == 0u ? 7.2288187f : 4.9068906f;      // log2(150) and log2(30)
      v.frange = v.model == 0u ? 6.3219281f : 8.6438562f;  // log2(80) and log2(400): to 12 kHz
      derive_filter_input(v);
      break;
    case SID_PARAM_SWEEP: v.sweep = value; break;
    case SID_PARAM_SWEEP_TIME: v.sweep_ms = value; break;
    case SID_PARAM_VIB_DEPTH: v.vib_depth = value * 0.01f; break;
    case SID_PARAM_VIB_RATE: v.vib_step = value * static_cast<float>(kControl) * 0.001f / rate_ms; break;
    case SID_PARAM_VIB_DELAY: v.vib_ramp = value >= 1.0f ? static_cast<float>(kControl) / (value * rate_ms) : 1.0f; break;
    case SID_PARAM_ARP_1:
    case SID_PARAM_ARP_2:
      if (id == SID_PARAM_ARP_1) {
        v.arp1 = value;
      } else {
        v.arp2 = value;
      }
      v.arp_len = v.arp2 != 0.0f ? 3u : (v.arp1 != 0.0f ? 2u : 1u);
      break;
    case SID_PARAM_ARP_SPEED: v.arp_step = value * static_cast<float>(kControl) * 0.001f / rate_ms; break;
    case SID_PARAM_MODE:
      v.fmode = static_cast<uint32_t>(value);
      if (v.fmode == 0u) {
        v.fs1 = 0.0f;  // OFF: the filter is skipped, and starts from rest when it is switched on
        v.fs2 = 0.0f;
      }
      filter_coefs(v, v.fe_last);
      v.fe_last = -100.0f;  // fe_last is only kept up while Mode is on: the next control tick remakes the coefficients for the cutoff the knobs and the key say now
      break;
    case SID_PARAM_CUTOFF: v.fcut = value * 0.01f; break;
    case SID_PARAM_RESONANCE:
      v.fk = 1.41421356f * v.host->exp2_f(-value * 0.28814f);  // sqrt(2) / 20^(value / 15)
      filter_coefs(v, v.fe_last);
      break;
    case SID_PARAM_KEY_TRACK: v.fkt = value * 0.01f; break;
    case SID_PARAM_ENV_AMOUNT: v.fenv = value * 0.01f; break;
    case SID_PARAM_DRIVE:
      v.fdrive = value * 0.01f;
      derive_filter_input(v);
      break;
    default: break;
  }
}

inline void voice_init(Voice& v, const o3m_host* host) {
  v.host = host;
  const float rate = host->sample_rate;
  v.rate_ms = rate * 0.001f;
  v.inc_unit = 440.0f / rate * host->exp2_f(-69.0f / 12.0f);
  v.clk_per_sample = kClock / rate;
  v.lfsr = kLfsrSeed;
  v.nval = 0.0f;
  v.note = 69.0f;
  v.velocity = 1.0f;
  v.inv_inc = 1.0f;
  v.inv_minc = 1.0f;
  v.vib_ramp = 1.0f;
  v.vib_amp = 1.0f;
  v.mratio = 1.0f;
  v.fk = 1.41421356f;
  v.fe_max = host->log2_f(0.45f * rate);
  v.fe_last = 8.0f;
  v.fs1 = 0.0f;
  v.fs2 = 0.0f;
  for (uint32_t i = 0; i < SID_PARAM_COUNT; ++i) {
    voice_set_param(v, i, sid_params[i].def);
  }
  v.duty = detail::clampf(v.pw_base, kMinDuty, kMaxDuty);
}

// `slide`: a note the host ramps over the one that sounds (O3M_NOTE_LEGATO | O3M_NOTE_RAMP, OS-MODPITCH): the envelope, the arpeggio, the sweep and the vibrato go on, only the pitch moves to the note.
inline void voice_note_on(Voice& v, uint32_t note, uint32_t velocity, bool slide = false) {
  v.host_off = 0.0f;  // a note puts the host's offset back to 0
  v.host_slope = 0.0f;
  if (slide && v.stage != kOff) {
    v.note = static_cast<float>(note > 127u ? 127u : note);
    control(v);
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
  v.pwm_ph = 0.25f;  // the LFO starts at its zero: the note starts at the width the parameter says
  v.duty = detail::clampf(v.pw_base, kMinDuty, kMaxDuty);
  v.dstep = 0.0f;
  v.dcm = table_dc(v.tab_idx, v.wave == kSawTri ? 1.0f : v.duty);
  v.half_j = 0.0f;
  if (v.stage == kOff) {
    v.env = 0u;
    v.phase = 0.0f;
    v.mph = 0.0f;
    v.nphase = 0.0f;
  }
  enter_attack(v);
  control(v);
  v.ctl = kControl;
}

inline void voice_note_off(Voice& v) {
  if (v.stage != kOff) {
    enter_release(v);
  }
}

// OS-MODPITCH (ABI 1.8): from the next control period the pitch is the note + `semitones`, and it moves by `per_frame` a frame (a control period is kControl frames).
inline void voice_pitch(Voice& v, float semitones, float per_frame) {
  v.host_off = detail::clampf(semitones, -kMaxHostSemis, kMaxHostSemis);  // finite and bounded whatever the host passes (a NaN is the lower bound): the pitch feeds the phase and the table index
  v.host_slope = detail::clampf(per_frame * static_cast<float>(kControl), -kMaxHostSemis, kMaxHostSemis);
}

// ---- audio ----------------------------------------------------------------------------------------------------------------------------------------------
// The waveform's value at phase p with no band-limiting (what a sync reset jumps between): saw, triangle, the pulse (its DC aside), or the combined table gated by the pulse. The combined wave is
// scaled by 2 like the output, so a jump is in output units.
template <uint32_t L>
SID_INLINE float naive(float p, float d, const uint8_t* tab, bool gate) {
  if (L == kLoopSaw) {
    return 2.0f * p - 1.0f;
  } else if (L == kLoopTri) {
    const float a = p - 0.5f;
    return 4.0f * (a < 0.0f ? -a : a) - 1.0f;
  } else if (L == kLoopPulse) {
    return p < d ? 1.0f : -1.0f;
  } else {
    const float pf = p * static_cast<float>(kTableN);
    const uint32_t k = static_cast<uint32_t>(pf) & (kTableN - 1u);
    const float fr = pf - static_cast<float>(k);
    const float a = static_cast<float>(tab[k]);
    const float b = static_cast<float>(tab[(k + 1u) & (kTableN - 1u)]);
    const float u = (a + fr * (b - a)) * (1.0f / 255.0f);
    return gate && p >= d ? 0.0f : 2.0f * u;
  }
}

// One chunk of at most kControl samples with the pitch fixed: L is the kind of loop, so each one is as tight as it can be.
template <uint32_t L>
SID_INLINE void run(Voice& v, float* out, uint32_t n) {
  float phase = v.phase, mph = v.mph, d = v.duty, half_j = v.half_j, dcm = v.dcm;
  uint32_t e = v.env, t = v.target, sl = v.slope;
  float nph = v.nphase, nval = v.nval;
  const float inc = v.inc, inv = v.inv_inc, minc = v.minc, inv_minc = v.inv_minc, thr = 1.0f - v.minc, dstep = v.dstep, dcstep = v.dcstep, gain = v.gain;
  const bool sync = v.sync_act != 0u, ring = v.ring != 0u, use_mod = sync || ring;
  const bool gate = v.wave != kSawTri;
  const uint8_t* const tab = v.tab;
  const float nclk = v.nclk;
  uint32_t lfsr = v.lfsr;
  for (uint32_t i = 0; i < n; ++i) {
    float s;
    if (L == kLoopNoise) {
      nph += nclk;
      if (nph >= 1.0f) {
        nph -= 1.0f;
        lfsr = lfsr_step(lfsr);
        nval = static_cast<float>(noise_byte(lfsr)) * (2.0f / 255.0f) - 1.0f;
      }
      s = nval;
    } else {
      if (L == kLoopPulse || L == kLoopComb) {
        d += dstep;
      }
      float ringm = 1.0f, corr = 0.0f, bp;
      if (use_mod) {
        mph += minc;
        const bool mw = mph >= 1.0f;
        mph -= mw ? 1.0f : 0.0f;
        if (ring) {
          ringm = mph < 0.5f ? 1.0f : -1.0f;
        }
        phase += inc;
        if (sync && mw) {  // the modulator wrapped within this sample, x of the sample ago: the oscillator restarts then, so it has run x of a step
          const float x = mph * inv_minc;
          corr = half_j * (x * (2.0f - x) - 1.0f);  // the BLEP's after-side
          half_j = 0.0f;
          phase = x * inc;
          bp = 0.5f;  // (no natural edge to band-limit in this sample: the jump above is the whole of it)
        } else {
          phase -= phase >= 1.0f ? 1.0f : 0.0f;
          bp = phase;
        }
        if (sync && mph >= thr) {  // the modulator wraps within the next sample, tau samples from now: the jump of that reset, and the BLEP's before-side
          const float tau = (1.0f - mph) * inv_minc;
          float pw = phase + tau * inc;
          pw -= pw >= 1.0f ? 1.0f : 0.0f;
          half_j = 0.5f * (naive<L>(0.0f, d, tab, gate) - naive<L>(pw, d, tab, gate));
          const float y = 1.0f - tau;
          corr += half_j * y * y;
        }
      } else {
        phase += inc;
        phase -= phase >= 1.0f ? 1.0f : 0.0f;
        bp = phase;
      }
      if (L == kLoopSaw) {
        s = 2.0f * phase - 1.0f - blep(bp, inc, inv);
      } else if (L == kLoopTri) {
        const float a = phase - 0.5f;
        s = (4.0f * (a < 0.0f ? -a : a) - 1.0f) * ringm;
      } else if (L == kLoopPulse) {
        float t2 = phase - d;
        t2 += t2 < 0.0f ? 1.0f : 0.0f;
        s = (phase < d ? 1.0f : -1.0f) + blep(bp, inc, inv) - blep(t2, inc, inv) - (2.0f * d - 1.0f);
      } else {
        const float pf = phase * static_cast<float>(kTableN);
        const uint32_t k = static_cast<uint32_t>(pf) & (kTableN - 1u);
        const float fr = pf - static_cast<float>(k);
        const float a = static_cast<float>(tab[k]);
        const float b = static_cast<float>(tab[(k + 1u) & (kTableN - 1u)]);
        float u = (a + fr * (b - a)) * (1.0f / 255.0f);
        if (gate) {
          u = phase < d ? u : 0.0f;
        }
        dcm += dcstep;
        s = 2.0f * (u - dcm);
      }
      s += corr;
    }
    if (e != t) {
      // move by one slope, never past the target (8.24 near 255 is within 2 steps of the word's end: no add may overflow)
      const uint32_t gap = e < t ? t - e : e - t;
      if (gap <= sl) {
        e = t;
        v.env = e;
        advance(v);
        e = v.env;
        t = v.target;
        sl = v.slope;
      } else {
        e = e < t ? e + sl : e - sl;
      }
    }
    out[i] = s * static_cast<float>(e >> kFrac) * gain;
  }
  v.phase = phase;
  v.mph = mph;
  v.duty = d;
  v.dcm = dcm;
  v.half_j = half_j;
  v.nphase = nph;
  v.nval = nval;
  v.lfsr = lfsr;
  v.env = e;
}

// The filter over a block, in place: its state and coefficients in registers, no host call, no branch but the loop. The input is the voice's output (envelope and gain applied).
SID_INLINE void filter_run(Voice& v, float* out, uint32_t n) {
  float s1 = v.fs1, s2 = v.fs2;
  const float a1 = v.fa1, a2 = v.fa2, a3 = v.fa3, cx = v.fcx, c1 = v.fc1, c2 = v.fc2, pre = v.fpre, post = v.fpost;
  for (uint32_t i = 0; i < n; ++i) {
    const float x = sclip(out[i] * pre);
    const float v3 = x - s2;
    const float v1 = a1 * s1 + a2 * v3;
    const float v2 = s2 + a2 * s1 + a3 * v3;
    s1 = v1 + v1 - s1;
    s2 = v2 + v2 - s2;
    out[i] = post * sclip(cx * x + c1 * v1 + c2 * v2);
  }
  v.fs1 = s1;
  v.fs2 = s2;
}

// Fills `frames` samples; returns 1 while the voice may still sound (the note, or the ring of the filter after it). The filter runs on each chunk of at most kControl samples with the
// coefficients that chunk's control tick made, so the result does not depend on the block size; once the note is over it goes on over silence until its state has died away.
SID_INLINE uint32_t voice_render(Voice& v, float* out, uint32_t frames) {
  uint32_t done = 0;
  while (done < frames) {
    uint32_t n = frames - done;
    if (v.stage != kOff) {
      if (v.ctl == 0u) {
        control(v);
        v.ctl = kControl;
      }
      n = n < v.ctl ? n : v.ctl;
      switch (v.wave) {
        case kSaw: run<kLoopSaw>(v, out + done, n); break;
        case kTriangle: run<kLoopTri>(v, out + done, n); break;
        case kPulse: run<kLoopPulse>(v, out + done, n); break;
        case kNoise: run<kLoopNoise>(v, out + done, n); break;
        default: run<kLoopComb>(v, out + done, n); break;
      }
      v.ctl -= n;
    } else {
      for (uint32_t i = 0; i < n; ++i) {
        out[done + i] = 0.0f;
      }
    }
    if (v.fmode != 0u && (v.stage != kOff || v.fs1 * v.fs1 + v.fs2 * v.fs2 > kRingEnergy)) {
      filter_run(v, out + done, n);  // (Mode OFF: no pass at all)
    }
    done += n;
  }
  if (v.stage != kOff) {
    return 1u;
  }
  if (v.fmode != 0u && v.fs1 * v.fs1 + v.fs2 * v.fs2 > kRingEnergy) {
    return 1u;
  }
  v.fs1 = 0.0f;  // the tail is over: rest
  v.fs2 = 0.0f;
  return 0u;
}

}  // namespace sid
