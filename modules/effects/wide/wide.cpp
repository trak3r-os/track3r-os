// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX2 X1 (new Community OS behaviour): WIDE, a stereo widener for a track insert, the first module of the stereo pack (docs/specs/OS-INSFX-FX2.md section 4.1; ABI 1.12, a WIDENS effect, so it
// needs OS-INSFX-ABI2 A4 on the unit). All the maths is this module's own: no third-party code and no formula note (THIRD_PARTY.md), float only, no library call and no division in the sample loop.
//
//   - The law (a Lauridsen-style widening): with M = (L + R) / 2 the mid of the input, S = (L - R) / 2 its side, and h = HP(delay(M)) the mid delayed by Time and high-passed at Low cut, the output is
//     L' = L + w h and R' = R - w h, w = Width / 100. That is S' = S + w h and M' = M: the effect ONLY ADDS TO THE SIDE, so the mono sum (L' + R') / 2 is the input's mid to the rounding of two float
//     adds (the same `a = w h` is added to one channel and taken from the other), and Width 0 is the input bit for bit. A stereo input keeps its own S (a stereo voice, or an earlier stereo insert). The
//     two ears get complementary combs, and the mono sum cancels them. A mono input is the special case S = 0.
//   - The line: a power-of-two ring of 2,048 floats of M (8 KB in the instance block, which is in SDRAM: it does not fit the 4 KiB arena). The shortest delay is 64 frames (Time 2 ms is 88 at
//     44.1 kHz; the code never goes under 64), so a chunk of up to 64 frames reads only frames written by EARLIER chunks. render_st walks a block in chunks of 64: the read span is copied once from
//     the ring to a stack scratch, the arithmetic runs on SRAM, the chunk of M is written back in one burst (the send delay's rule; a block of up to 128 frames is two chunks).
//   - Time changes (a modulation source may move it every block): the delay in force changes at a chunk boundary and the chunk CROSS-FADES the old read span into the new one, so a Time move makes no
//     click, only the comb moving (it lands on the side, mono-compatible). The delay is a whole number of frames (22 us a step): no interpolation, no pitch glide.
//   - The high-pass is one pole, h = x - lp, lp += c (x - lp), c = 1 - e^(-2 pi Lowcut / fs): the coefficient is derived again (one host exp2) only when Low cut changed, at chunk rate; the filter's
//     memory is continuous, so a move is smooth. Width glides linearly over each chunk to its target.
//   - Mono `render` (the loader requires it; no host of this OS calls it for a WIDENS effect): the mid of render_st, which is the input itself (the added side cancels), after running the same chunk
//     so that the line and the filter stay coherent. The spec words it as "L written back"; that would put the comb into a mono output, against the mono-compatible promise, so it is not done.
//   - Tail (OS-INSFX S2 contract): render_st returns 1 while the input lives (a non-zero sum of |M| over the chunk), or while the last 30 ms (the longest Time) plus one chunk of the line may still hold
//     signal, or while the high-pass rings (|lp| >= 1e-7, -140 dB); then 0. lp is flushed to exact zero once the line is silent, so the tail ends at exact zeros. The ring-out of the high-pass comes
//     after the 30 ms: about 13 ms at the default Low cut of 200 Hz, about 130 ms at 20 Hz.
//   - Hostile input: a NaN, infinite or huge (over 1e20 summed over a chunk) input is written to the line as ZERO, so it never enters the delay or the filter; its own chunk comes out as the input does
//     (a NaN in is a NaN out, as the host mixes it), and the next chunk is clean. Parameters clamp (a NaN falls to the lower bound).
//   - Parameters, in this order for ever: Width (0..100 %), Time (2..30 ms), Low cut (20..1000 Hz, log). All three are modulation targets: set_param only stores a target.
#include <o3module.h>

#include "wide_gen.h"

namespace {

constexpr uint32_t kChunk = 64;                 // frames a control step lasts; the shortest delay
constexpr uint32_t kLine = 2048;                // the ring (a power of two; 30 ms is 1,324 frames at 44.1 kHz and 1,440 at 48 kHz)
constexpr uint32_t kMask = kLine - 1u;
constexpr float kIdle = 1e-7f;                  // |lp| below this is silence (-140 dB of full scale) and is flushed to 0
constexpr float kHuge = 1e20f;                  // a chunk whose summed |M| is not below this (or is NaN) is hostile
constexpr float kLog2E = 1.4426950408889634f;
constexpr float kTwoPi = 6.2831853f;
constexpr float kTimeMaxMs = 30.0f;
constexpr uint32_t kSilMax = 0x40000000u;

struct State {
  const o3m_host* host;
  float frames_per_ms;   // the sample rate / 1000
  float hp_k;            // 2 pi log2(e) / fs: the high-pass coefficient is 1 - exp2(-hp_k * Lowcut)
  uint32_t d_max;        // the longest delay, in frames (30 ms), 64..kLine - 64
  float t_width;         // targets (set_param): Width 0..1, Time in ms, Low cut in Hz
  float t_time;
  float t_lowcut;
  float width;           // in force
  uint32_t d;            // the delay in force, in frames
  float lp;              // the high-pass memory
  float hpc;             // its coefficient
  float hpc_fc;          // the Low cut it was derived from (0: not yet)
  uint32_t pos;          // the ring's write position
  uint32_t sil;          // frames of silent input so far (saturating)
  uint32_t primed;       // a chunk has run: set_param before it snaps instead of gliding
  float line[kLine];
};
static_assert(sizeof(State) <= WIDE_STATE_BYTES, "module.json: state_bytes is too small for State");
static_assert((kLine & (kLine - 1u)) == 0u, "the ring is a power of two");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN becomes lo

inline uint32_t delay_frames(const State* s, float ms) {
  const uint32_t f = static_cast<uint32_t>(ms * s->frames_per_ms + 0.5f);
  return f < kChunk ? kChunk : (f > s->d_max ? s->d_max : f);
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->frames_per_ms = host->sample_rate * 0.001f;
  s->hp_k = kTwoPi * kLog2E / host->sample_rate;
  const uint32_t dm = static_cast<uint32_t>(kTimeMaxMs * s->frames_per_ms + 0.5f);
  s->d_max = dm < kChunk ? kChunk : (dm > kLine - kChunk ? kLine - kChunk : dm);
  s->t_width = wide_params[WIDE_PARAM_WIDTH].def * 0.01f;
  s->t_time = wide_params[WIDE_PARAM_TIME].def;
  s->t_lowcut = wide_params[WIDE_PARAM_LOW_CUT].def;
  s->width = s->t_width;
  s->d = delay_frames(s, s->t_time);
  s->lp = 0.0f;
  s->hpc = 0.0f;
  s->hpc_fc = 0.0f;
  s->pos = 0;
  s->sil = kSilMax;  // an empty line is silent
  s->primed = 0;
  for (uint32_t i = 0; i < kLine; ++i) {  // the block is zeroed by the OS, but a re-init (bypass off) must not hear the old line
    s->line[i] = 0.0f;
  }
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {  // targets only: the glide, the cross-fade and the derivation are the render's
  State* s = static_cast<State*>(raw);
  switch (id) {
    case WIDE_PARAM_WIDTH: s->t_width = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    case WIDE_PARAM_TIME: s->t_time = clampf(value, 2.0f, kTimeMaxMs); break;
    case WIDE_PARAM_LOW_CUT: s->t_lowcut = clampf(value, 20.0f, 1000.0f); break;
    default: break;
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// One chunk of n (1..64) frames, as the OS drives an insert: both inputs are read before any output is written (in and out may be the same buffers). Returns the tail flag. Inlined into the two renders.
inline __attribute__((always_inline)) uint32_t chunk(State* s, const float* il, const float* ir, float* ol, float* orr, uint32_t n) {
  // control: once a chunk
  const uint32_t d_to = delay_frames(s, s->t_time);
  const float w_to = s->t_width;
  if (s->primed == 0u) {
    s->width = w_to;
    s->d = d_to;
    s->primed = 1u;
  }
  if (s->t_lowcut != s->hpc_fc) {
    s->hpc_fc = s->t_lowcut;
    s->hpc = 1.0f - s->host->exp2_f(-s->hp_k * s->t_lowcut);
  }
  const float inv = 1.0f / static_cast<float>(n);
  const uint32_t pos = s->pos;
  const uint32_t d = s->d;
  float rd[kChunk];  // the delayed M of these frames
  float mm[kChunk];  // the M of these frames, for the write-back
  {
    const uint32_t at = (pos - d) & kMask;
    for (uint32_t j = 0; j < n; ++j) {
      rd[j] = s->line[(at + j) & kMask];
    }
  }
  if (d_to != d) {  // a Time move: cross-fade the old read span into the new one over the chunk
    const uint32_t at = (pos - d_to) & kMask;
    for (uint32_t j = 0; j < n; ++j) {
      const float t = static_cast<float>(j + 1u) * inv;
      rd[j] += t * (s->line[(at + j) & kMask] - rd[j]);
    }
    s->d = d_to;
  }
  float lp = s->lp;
  const float c = s->hpc;
  float w = s->width;
  const float dw = (w_to - w) * inv;
  float acc = 0.0f;
  for (uint32_t j = 0; j < n; ++j) {
    const float l = il[j];  // read both before writing
    const float r = ir[j];
    const float m = 0.5f * (l + r);
    mm[j] = m;
    acc += __builtin_fabsf(m);
    const float x = rd[j];
    lp += c * (x - lp);
    w += dw;
    const float a = w * (x - lp);  // the added side
    ol[j] = l + a;
    orr[j] = r - a;
  }
  s->width = w_to;
  // write-back: one burst; a hostile chunk writes zeros
  const bool bad = !(acc < kHuge);
  for (uint32_t j = 0; j < n; ++j) {
    s->line[(pos + j) & kMask] = bad ? 0.0f : mm[j];
  }
  s->pos = (pos + n) & kMask;
  // tail
  const bool live = bad || acc != 0.0f;
  uint32_t sil = live ? 0u : s->sil + n;
  if (sil > kSilMax) {
    sil = kSilMax;
  }
  s->sil = sil;
  const bool line_dry = sil >= s->d_max + kChunk;  // nothing the longest delay can read is left in the ring
  if (line_dry && __builtin_fabsf(lp) < kIdle) {
    lp = 0.0f;
  }
  s->lp = lp;
  return (live || !line_dry || lp != 0.0f) ? 1u : 0u;
}

O3M_HOT uint32_t render_st(void* raw, const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  uint32_t busy = 1u;
  for (uint32_t at = 0; at < frames; at += kChunk) {
    const uint32_t n = frames - at < kChunk ? frames - at : kChunk;
    busy = chunk(s, in_l + at, in_r + at, out_l + at, out_r + at, n);
  }
  return busy;
}

// The mono render the loader requires: the mid of render_st, which is the input (see the header). Cold code: no host of this OS calls it.
uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  float tl[kChunk];
  float tr[kChunk];
  uint32_t busy = 1u;
  for (uint32_t at = 0; at < frames; at += kChunk) {
    const uint32_t n = frames - at < kChunk ? frames - at : kChunk;
    busy = chunk(s, in + at, in + at, tl, tr, n);
    for (uint32_t j = 0; j < n; ++j) {
      out[at + j] = in[at + j];
    }
  }
  return busy;
}

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    WIDE_ENTRY_HEAD, sizeof(State), WIDE_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, WIDE_ENTRY_VIEWS(nullptr),
    WIDE_ENTRY_FX(render_st),
};
