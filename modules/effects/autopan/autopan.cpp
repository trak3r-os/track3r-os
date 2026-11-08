// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX2 X4 and X5 (new Community OS behaviour): AUTOPAN, a stereo auto-pan for a track insert, with the optional HEADPHONE mode (docs/specs/OS-INSFX-FX2.md section 4.4, Q1 b and Q5 a; ABI 1.12, a
// WIDENS effect, so it needs OS-INSFX-ABI2 A4 on the unit, and A3 for the lock to the pattern's beat). All the maths is this module's own: no third-party code and no formula note (THIRD_PARTY.md; the
// HEADPHONE cues are the textbook spherical-head idea of a time and a level-and-tone difference between the ears, no code or data taken), float only, no library call and no division in the sample loop.
//
//   - SPEAKERS, the law (Q5 a, linear): with M = (L + R) / 2 the mid of the input, p the pan (-1..+1) and S the side, S' = S + p M, that is L' = L + p M and R' = R - p M (L' = (1 + p) M + S_in,
//     R' = (1 - p) M - S_in). The effect ONLY ADDS TO THE SIDE: the same `a = p M` is added to one channel and taken from the other, so the mono sum (L' + R') / 2 is the input's mid to the rounding of two
//     float adds, at every setting; Depth 0 is the input bit for bit; a stereo input keeps its own S (a stereo voice, or an earlier stereo insert). The sides are 3 dB louder in power than the centre.
//   - The LFO: computed ONCE PER 64 FRAMES OF THE STREAM (a control chunk: the OS's block is 64 frames, a longer call runs several chunks, a shorter one continues the chunk in progress, so the pan never
//     depends on the block size the host calls with), the pan ramped linearly from where the last chunk ended to this chunk's end value, so a depth or phase move, a jump of the clock or a change of shape
//     makes a ramp over one chunk (1.45 ms), never a step. SINE is two parabolas refined by one correction step (0.1 % from sin, no libm), TRIANGLE and SQUARE start rising and high at phase 0, and
//     the SQUARE's value goes through a one-pole of about 5 ms run once a chunk (exact at the chunk's end: 1 - exp(-64 / (0.005 fs)) of the way, 25 % at 64 frames), so its ramp is a piece of an
//     exponential and its largest step in a sample is under 2 Depth x 0.25 / 64, about 0.8 % of Depth, at every block size.
//   - The phase. Synced (Sync 1..15, the cycle is 64, 32, 16, 12, 8, 6, 4, 3, 2, 3/2, 1, 3/4, 1/2, 1/3 or 1/4 steps): while the sequencer plays, the phase at the first sample of a block that starts a chunk is
//     frac(beat x 4 / steps) from `get_clock` (the beat counts from step 0 of the pattern that plays, so a bar-aligned pattern swings on the beat and a jump re-aligns at once: the block after a WRAPPED
//     flag is ramped to the new phase), plus Phase / 360; a chunk that starts mid-block keeps the phase the previous chunks advanced, so it does not run late. When the sequencer does not play (stopped, or an OS whose clock is still the stub), the phase free-runs at the tempo in force (the project's
//     while stopped; 120 BPM without one), as the stereo template does. FREE (Sync 0): a free-running phase at 1 cycle per `Period` ms. The spec's `Rate` 0.05..10 Hz is a PERIOD 100..20000 ms here:
//     the OS shows a parameter that has a unit as whole numbers, and a slow pan needs fractions of a Hz (module/sdk/README.md section 2; the stereo template and STEREO CHORUS do the same).
//   - HEADPHONE (Headphone ON, X5): the pan p drives two cues a block. The far ear (the one p points away from: R for p > 0) is delayed by 0.6 ms x |p| (0 to 26.5 frames at 44.1 kHz) by a linear-interpolated read
//     of a 32-frame float ring an ear (256 B in the state: in the 4 KiB SRAM_U arena when it has the room (a state of 404 B on the unit, 408 B in the 64-bit host build), else in SDRAM), and darkened by a one-pole low-pass whose corner falls from 20 kHz at the
//     centre to 1.8 kHz at |p| = 1 (log-linear in |p|: fc = 20000 (1800 / 20000)^|p|). The cue targets are derived every chunk (two host exp2) and stored in the state, and the cues ramp to them over the chunk; a switch-on mid-chunk primes the rings and the filters and snaps the cues to the stored targets, zero increments. The near ear keeps SPEAKERS' level law and the
//     centre's 20 kHz corner, so at p = 0 both ears are the same (L = R for a mono input). Each ear's delay and corner ramp linearly over the block, and they are continuous through p = 0 (the roles swap
//     where both cues are at their centre values). NOT mono-compatible: the delayed ear combs in the mono sum (the OS's parameter page has no help line: the parameter's name, the README and the manual say so).
//     Switching on primes the rings and the filters with the first frame, so there is no dropout; switching off is a hard switch to the direct pan.
//   - The mono `render` (the loader requires it; no host of this OS calls it for a WIDENS effect): the mid of render_st, which is the input itself (the added side cancels).
//   - Tail (OS-INSFX S2 contract): SPEAKERS holds no memory, so render_st returns 1 for a block whose input is not all zero, else 0. HEADPHONE returns 1 while the input lives, while the 32-frame ring may
//     still hold signal or while a low-pass rings (|lp| >= 1e-7, -140 dB), then 0; the low-pass is flushed to exact zero once the ring is dry, so the tail ends at exact zeros.
//   - Hostile input: a NaN, infinite or huge (over 1e20 summed over a chunk) input is not allowed to stay in the HEADPHONE ring or filters: they are primed again from the next chunk's first frame (the chunk's
//     own output is what the mix gives, a NaN in is a NaN out as the host mixes it), so the next chunk is clean. SPEAKERS has no state the input can reach. Parameters clamp (a NaN falls to the lower bound).
//   - Parameters, in this order for ever: Sync (INT 0..15: FREE, then the 15 cycle lengths), Period (100..20000 ms, log), Depth (0..100 %), Shape (INT 0 SINE, 1 TRI, 2 SQUARE), Phase (INT 0..360
//     degrees), Headphone (BOOL, OFF = the spec's MODE SPEAKERS, ON = HEADPHONE). set_param only stores a target (Period, Depth and Phase are modulation targets); the block's one control step does the rest.
#include <o3module.h>

#include "autopan_gen.h"

namespace {

constexpr uint32_t kChunk = 64;                  // frames a control step lasts (the OS's block): every chunk is kChunk frames OF THE STREAM, whatever the block size of the call, so the pan does not depend on it
constexpr float kChunkF = static_cast<float>(kChunk);
constexpr float kInvChunk = 1.0f / kChunkF;
constexpr uint32_t kRing = 32;                   // the far-ear ring, frames: the longest delay is 0.6 ms (26.5 frames at 44.1 kHz, 28.8 at 48 kHz) and the interpolation reads one more
constexpr uint32_t kRingMask = kRing - 1u;
constexpr float kIdle = 1e-7f;                   // |lp| below this is silence (-140 dBFS) and is flushed to 0
constexpr float kHuge = 1e20f;                   // a chunk whose summed |L| + |R| is not below this (or is NaN) is hostile
constexpr float kLog2E = 1.4426950408889634f;
constexpr float kTwoPi = 6.2831853f;
constexpr float kItdMs = 0.6f;                   // the far ear's delay at |p| = 1 (A-8: an average head)
constexpr float kNearHz = 20000.0f;              // the head-shadow corner at the centre (capped at 0.45 fs)
constexpr float kFarHz = 1800.0f;                // ... and at |p| = 1
constexpr float kSquareMs = 5.0f;                // the SQUARE's smoothing
constexpr uint32_t kShapeTri = 1u;
constexpr uint32_t kShapeSquare = 2u;
// cycles per beat of each Sync label: 4 / steps (the label is the cycle's length in steps; a beat is 4 steps). Index 0 (FREE) is unused.
constexpr float kCyclesPerBeat[16] = {0.0f, 0.0625f, 0.125f, 0.25f, 0.33333334f, 0.5f, 0.6666667f, 1.0f, 1.3333334f, 2.0f, 2.6666667f, 4.0f, 5.3333335f, 8.0f, 12.0f, 16.0f};

struct State {
  const o3m_host* host;
  float inv_fs;       // 1 / sample rate
  float sq_c;         // log2(e) / (5 ms fs): the SQUARE smoother's coefficient over n frames is 1 - exp2(-n sq_c)
  float hp_k;         // 2 pi log2(e) / fs: a one-pole coefficient is 1 - exp2(-hp_k fc)
  float itd_max;      // 0.6 ms in frames
  float fc_near;      // the corner at the centre, Hz
  float fc_ratio;     // log2(fc_near / 1800): the corner is fc_near exp2(-|p| fc_ratio)
  float c_near;       // the coefficient of the centre's corner
  float t_period;     // targets (set_param): Period in ms, Depth 0..1, Phase in cycles
  float t_depth;
  float t_phase;
  uint32_t t_sync;    // 0..15
  uint32_t t_shape;   // 0..2
  uint32_t t_mode;    // 0 SPEAKERS, 1 HEADPHONE
  float ph;           // the free-running phase, 0..1 (without Phase)
  float p_last;       // the pan the last chunk ended on (the SQUARE smoother's memory too)
  uint32_t cj;        // frames of the current control chunk still to render (0: the next call starts one)
  float p;            // the pan at the last rendered sample (the ramp's memory across calls)
  float dp;           // the current chunk's per-sample pan increment
  uint32_t primed;    // a chunk has run: the first one starts at the LFO's value instead of ramping from 0
  uint32_t hp_ready;  // the HEADPHONE rings and filters hold the signal (cleared while SPEAKERS runs)
  uint32_t pos;       // the rings' write position
  uint32_t sil;       // frames of silent input so far (saturating)
  float d_l;          // the HEADPHONE cues in force at the chunk's start: the delay (frames) and the low-pass coefficient of each ear, and the filter memories
  float d_r;
  float c_l;
  float c_r;
  float lp_l;
  float lp_r;
  float ddl;          // the current chunk's per-sample cue increments (HEADPHONE)
  float ddr;
  float dcl;
  float dcr;
  float h_dl;         // the current chunk's cue targets, stored every chunk: a switch-on mid-chunk primes from them (chunk)
  float h_dr;
  float h_cl;
  float h_cr;
  float ring_l[kRing];  // the pan's output (the ear's own signal before the delay)
  float ring_r[kRing];
};
static_assert(sizeof(State) <= AUTOPAN_STATE_BYTES, "module.json: state_bytes is too small for State");
static_assert((kRing & (kRing - 1u)) == 0u, "the ring is a power of two");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN becomes lo
inline float absf(float x) { return __builtin_fabsf(x); }

// The fractional part of x >= 0 (x is a phase: a few cycles at most, a beat's below 1e6 cycles).
inline float frac(float x) { return x - static_cast<float>(static_cast<int32_t>(x)); }

// The LFO, -1..1, at phase ph in [0, 1): zero at 0 and 1/2, rising from 0. SINE: two parabolas and one correction step (error under 0.0011); TRIANGLE; SQUARE: +1 for the first half cycle.
inline float lfo(float ph, uint32_t shape) {
  const float x = ph < 0.5f ? ph : ph - 1.0f;  // -0.5 .. 0.5
  if (shape == kShapeSquare) {
    return x >= 0.0f ? 1.0f : -1.0f;
  }
  if (shape == kShapeTri) {
    float y = 4.0f * x;
    y = y > 1.0f ? 2.0f - y : y;
    y = y < -1.0f ? -2.0f - y : y;
    return y;
  }
  const float y = 8.0f * x * (1.0f - 2.0f * absf(x));
  return y + 0.225f * (y * absf(y) - y);
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->inv_fs = 1.0f / host->sample_rate;
  s->sq_c = kLog2E / (host->sample_rate * kSquareMs * 0.001f);
  s->hp_k = kTwoPi * kLog2E / host->sample_rate;
  const float itd = kItdMs * 0.001f * host->sample_rate;
  s->itd_max = itd < static_cast<float>(kRing - 2u) ? itd : static_cast<float>(kRing - 2u);
  const float cap = 0.45f * host->sample_rate;
  s->fc_near = kNearHz < cap ? kNearHz : cap;
  s->fc_ratio = host->log2_f(s->fc_near / kFarHz);
  s->c_near = 1.0f - host->exp2_f(-s->hp_k * s->fc_near);
  s->t_period = autopan_params[AUTOPAN_PARAM_PERIOD].def;
  s->t_depth = autopan_params[AUTOPAN_PARAM_DEPTH].def * 0.01f;
  s->t_phase = autopan_params[AUTOPAN_PARAM_PHASE].def * (1.0f / 360.0f);
  s->t_sync = static_cast<uint32_t>(autopan_params[AUTOPAN_PARAM_SYNC].def);
  s->t_shape = static_cast<uint32_t>(autopan_params[AUTOPAN_PARAM_SHAPE].def);
  s->t_mode = static_cast<uint32_t>(autopan_params[AUTOPAN_PARAM_HEADPHONE].def);
  s->ph = 0.0f;
  s->p_last = 0.0f;
  s->cj = 0;
  s->p = 0.0f;
  s->dp = 0.0f;
  s->primed = 0;
  s->hp_ready = 0;
  s->pos = 0;
  s->sil = kRing;  // nothing in the rings
  s->d_l = s->d_r = 0.0f;
  s->c_l = s->c_r = s->c_near;
  s->lp_l = s->lp_r = 0.0f;
  s->ddl = s->ddr = s->dcl = s->dcr = 0.0f;
  s->h_dl = s->h_dr = 0.0f;
  s->h_cl = s->h_cr = s->c_near;
  for (uint32_t i = 0; i < kRing; ++i) {  // the block is zeroed by the OS, but a re-init (bypass off) must not hear the old rings
    s->ring_l[i] = 0.0f;
    s->ring_r[i] = 0.0f;
  }
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {  // targets only: the ramp, the cues and the clock are the render's
  State* s = static_cast<State*>(raw);
  switch (id) {
    case AUTOPAN_PARAM_SYNC: s->t_sync = static_cast<uint32_t>(clampf(value + 0.5f, 0.0f, 15.0f)); break;  // an int parameter only constrains the control: rounded here
    case AUTOPAN_PARAM_PERIOD: s->t_period = clampf(value, 100.0f, 20000.0f); break;
    case AUTOPAN_PARAM_DEPTH: s->t_depth = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    case AUTOPAN_PARAM_SHAPE: s->t_shape = static_cast<uint32_t>(clampf(value + 0.5f, 0.0f, 2.0f)); break;
    case AUTOPAN_PARAM_PHASE: s->t_phase = clampf(value, 0.0f, 360.0f) * (1.0f / 360.0f); break;
    case AUTOPAN_PARAM_HEADPHONE: s->t_mode = value >= 0.5f ? 1u : 0u; break;
    default: break;
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// One control step: kChunk frames OF THE STREAM, whatever the block size of the call (a block shorter than 64 continues the chunk in progress, a longer one runs several). Sets the pan's ramp for the chunk, and the HEADPHONE cues' targets (stored every chunk) and ramps. Out of line and cold: it runs once per chunk, and its host exp2 calls are cold anyway, so the hot part stays the sample loops.
__attribute__((noinline)) void start_chunk(State* s, float inc) {
  const uint32_t shape = s->t_shape;
  const float depth = s->t_depth;
  const float off = s->t_phase;
  const float ph0 = s->ph;  // the phase (without Phase) at the chunk's first sample: render_st sets it from the clock at the first chunk of a block
  const float ph1 = ph0 + kChunkF * inc;
  const float raw = depth * lfo(frac(ph1 + off), shape);
  s->ph = frac(ph1);
  if (s->primed == 0u) {  // the first chunk holds the pan (no ramp from an unrelated value)
    s->p_last = raw;
    s->primed = 1u;
  }
  const float p_from = s->p_last;
  // SQUARE: the LFO value goes through a one-pole of about 5 ms, run once a chunk (its step response is exact at the chunk's end: 1 - exp(-kChunk / (5 ms fs)))
  const float p1 = shape == kShapeSquare ? p_from + (1.0f - s->host->exp2_f(-kChunkF * s->sq_c)) * (raw - p_from) : raw;
  s->p_last = p1;
  s->p = p_from;
  s->dp = (p1 - p_from) * kInvChunk;
  s->cj = kChunk;
  {  // the HEADPHONE cues come from the pan at the chunk's end. The targets are stored every chunk (also in SPEAKERS), so a switch-on mid-chunk primes from them (chunk's prime branch).
    const float ap = absf(p1);
    const float dfar = ap * s->itd_max;
    const float cfar = 1.0f - s->host->exp2_f(-s->hp_k * (s->fc_near * s->host->exp2_f(-ap * s->fc_ratio)));
    const float dl_to = p1 < 0.0f ? dfar : 0.0f;  // p > 0 points to the left: R is the far ear
    const float dr_to = p1 > 0.0f ? dfar : 0.0f;
    const float cl_to = p1 < 0.0f ? cfar : s->c_near;
    const float cr_to = p1 > 0.0f ? cfar : s->c_near;
    s->h_dl = dl_to;
    s->h_dr = dr_to;
    s->h_cl = cl_to;
    s->h_cr = cr_to;
    if (s->t_mode != 0u) {
      if (s->hp_ready == 0u) {  // switched on, or after a hostile chunk: the cues start at their targets (the rings are primed in chunk)
        s->d_l = dl_to;
        s->d_r = dr_to;
        s->c_l = cl_to;
        s->c_r = cr_to;
        s->ddl = s->ddr = s->dcl = s->dcr = 0.0f;
      } else {  // the cues ramp over the chunk
        s->ddl = (dl_to - s->d_l) * kInvChunk;
        s->ddr = (dr_to - s->d_r) * kInvChunk;
        s->dcl = (cl_to - s->c_l) * kInvChunk;
        s->dcr = (cr_to - s->c_r) * kInvChunk;
      }
    }
  }
}

// n (1..64) frames of the control chunk in progress, as the OS drives an insert: both inputs are read before any output is written (in and out may be the same buffers). The ramp and the cues were set when the chunk started (start_chunk); here they only run. Returns the tail flag. Inlined into render_st.
inline __attribute__((always_inline)) uint32_t chunk(State* s, const float* il, const float* ir, float* ol, float* orr, uint32_t n) {
  const bool was_dry = s->sil >= kRing;  // the ring was already all zero at this call's start: every frame the ears can read (back kRing - 4 at most) is exactly 0
  // pass 1, the pan law: S' = S + p M (SPEAKERS' output, and HEADPHONE's input)
  float acc = 0.0f;
  float p = s->p;
  const float dp = s->dp;
  for (uint32_t j = 0; j < n; ++j) {
    const float l = il[j];  // read both before writing
    const float r = ir[j];
    p += dp;
    const float a = p * (0.5f * (l + r));
    ol[j] = l + a;
    orr[j] = r - a;
    acc += absf(l) + absf(r);
  }
  s->p = p;
  const bool bad = !(acc < kHuge);
  const bool live = bad || acc != 0.0f;
  if (s->t_mode == 0u) {  // SPEAKERS holds no memory
    s->hp_ready = 0;
    return live ? 1u : 0u;
  }
  // pass 2, HEADPHONE, in place on the output: each ear's delay and low-pass, the cues ramped over the chunk (set in start_chunk)
  {
    float* const rl = s->ring_l;
    float* const rr = s->ring_r;
    if (s->hp_ready == 0u) {  // switched on (also mid-chunk: start_chunk stored this chunk's targets), or after a hostile chunk: prime the rings and the filters with the first frame (no dropout) and snap the cues to the targets, zero increments
      const float x0 = ol[0];  // (a hostile first frame is primed too: the `bad` test below primes again at the next chunk)
      const float y0 = orr[0];
      for (uint32_t i = 0; i < kRing; ++i) {
        rl[i] = x0;
        rr[i] = y0;
      }
      s->d_l = s->h_dl;
      s->d_r = s->h_dr;
      s->c_l = s->h_cl;
      s->c_r = s->h_cr;
      s->ddl = s->ddr = s->dcl = s->dcr = 0.0f;
      s->lp_l = x0;
      s->lp_r = y0;
      s->hp_ready = 1u;
    }
    uint32_t pos = s->pos;
    float dl = s->d_l;
    float dr = s->d_r;
    float cl = s->c_l;
    float cr = s->c_r;
    float lpl = s->lp_l;
    float lpr = s->lp_r;
    const float ddl = s->ddl;
    const float ddr = s->ddr;
    const float dcl = s->dcl;
    const float dcr = s->dcr;
    for (uint32_t j = 0; j < n; ++j) {
      pos = (pos + 1u) & kRingMask;
      rl[pos] = ol[j];
      rr[pos] = orr[j];
      dl += ddl;
      dr += ddr;
      cl += dcl;
      cr += dcr;
      const int32_t kl = static_cast<int32_t>(dl);  // dl >= 0 up to the rounding of the ramp: a tiny negative truncates to 0
      const int32_t kr = static_cast<int32_t>(dr);
      const float fl = dl - static_cast<float>(kl);
      const float fr = dr - static_cast<float>(kr);
      const float xl = rl[(pos - static_cast<uint32_t>(kl)) & kRingMask];
      const float yl = rl[(pos - static_cast<uint32_t>(kl) - 1u) & kRingMask];
      const float xr = rr[(pos - static_cast<uint32_t>(kr)) & kRingMask];
      const float yr = rr[(pos - static_cast<uint32_t>(kr) - 1u) & kRingMask];
      lpl += cl * ((xl + fl * (yl - xl)) - lpl);
      lpr += cr * ((xr + fr * (yr - xr)) - lpr);
      ol[j] = lpl;
      orr[j] = lpr;
    }
    s->pos = pos;
    s->d_l = dl;
    s->d_r = dr;
    s->c_l = cl;
    s->c_r = cr;
    s->lp_l = lpl;
    s->lp_r = lpr;
  }
  // tail and hostile input
  if (bad) {  // keep the input's NaN out of the rings and the filters for good: the next chunk primes them again
    s->hp_ready = 0;
    s->lp_l = 0.0f;
    s->lp_r = 0.0f;
  }
  uint32_t sil = live ? 0u : s->sil + n;
  sil = sil > kRing ? kRing : sil;  // saturates at the ring's length
  s->sil = sil;
  if (was_dry && absf(s->lp_l) < kIdle && absf(s->lp_r) < kIdle) {  // the ears read only zeros this call and the low-pass has rung out: flush to exact zero
    s->lp_l = 0.0f;
    s->lp_r = 0.0f;
  }
  return (live || !was_dry || s->lp_l != 0.0f || s->lp_r != 0.0f) ? 1u : 0u;
}

O3M_HOT uint32_t render_st(void* raw, const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  // the clock, once a block: the cycles a sample, and (while the sequencer plays) the phase at the block's first sample
  const uint32_t sync = s->t_sync;
  float inc;
  float ph_clock = 0.0f;
  bool locked = false;
  if (sync == 0u) {
    inc = 1000.0f * s->inv_fs / s->t_period;
  } else {
    uint32_t tempo = 12000u;  // the fallback: 120 BPM
    o3m_clock clk;  // fill the members one by one: a struct initialiser may become a memset, and a module has no libc
    clk.size = sizeof(clk);
    clk.flags = 0;
    clk.tempo_x100 = 12000u;
    clk.beat = 0.0f;
    if (O3M_HOST_HAS(s->host, get_clock) && s->host->get_clock != nullptr) {
      const int32_t rc = s->host->get_clock(&clk);
      if (clk.tempo_x100 != 0u) {  // the tempo in force (the project's while stopped, also when the OS has no sequencer to ask)
        tempo = clk.tempo_x100;
      }
      if (rc == 0 && (clk.flags & O3M_CLOCK_PLAYING) != 0u && clk.beat >= 0.0f && clk.beat < 1.0e6f) {
        ph_clock = frac(clk.beat * kCyclesPerBeat[sync]);
        locked = true;
      }
    }
    inc = static_cast<float>(tempo) * (1.0f / 6000.0f) * kCyclesPerBeat[sync] * s->inv_fs;
  }
  if (locked && s->cj == 0u) {  // the sequencer plays: the phase at the block's first sample is the beat's — but only where a chunk starts there (a chunk in progress keeps the phase start_chunk advanced, else the chunk starting mid-call would run late)
    s->ph = ph_clock;
  }
  uint32_t busy = 0u;
  for (uint32_t at = 0; at < frames;) {
    if (s->cj == 0u) {  // a control chunk starts: kChunk frames of the stream, whatever the block size of this call
      start_chunk(s, inc);
    }
    const uint32_t n = frames - at < s->cj ? frames - at : s->cj;
    busy |= chunk(s, in_l + at, in_r + at, out_l + at, out_r + at, n);  // the tail flag ORs over the block's chunks
    s->cj -= n;
    at += n;
  }
  return busy;
}

// The mono render the loader requires: the mid of render_st, which is the input (see the header). Cold code: no host of this OS calls it.
uint32_t render(void*, const float* in, float* out, uint32_t frames) {
  float acc = 0.0f;
  for (uint32_t i = 0; i < frames; ++i) {
    const float x = in[i];
    acc += absf(x);
    out[i] = x;
  }
  return acc == 0.0f ? 0u : 1u;
}

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    AUTOPAN_ENTRY_HEAD, sizeof(State), AUTOPAN_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, AUTOPAN_ENTRY_VIEWS(nullptr),
    AUTOPAN_ENTRY_FX(render_st),
};
