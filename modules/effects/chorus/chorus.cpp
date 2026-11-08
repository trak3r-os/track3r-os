// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX2 X2 (new Community OS behaviour): STEREO CHORUS, a WIDENS insert for a track (ABI 1.12, docs/specs/OS-INSFX-FX2.md section 4.2). All the maths is this module's own: no third-party code and
// no formula note (THIRD_PARTY.md), float only, no libm, no division and no host call in the sample loop.
//
//   - One line of the mid M = (L + R) / 2 (2,048 floats, 46 ms, in the module's state: SDRAM), two taps read from it with linear interpolation, tap 1 at Delay + a LFO and tap 2 at Delay - the same LFO, so
//     the two delays always add up to 2 x Delay (one sine, the taps a half cycle apart). The output keeps the input's side and crossfades its mid:
//         L' = L + Mix (tap1 - M)        R' = R + Mix (tap2 - M)
//     so Mix 0 is the input bit for bit, Mix 100 % is the wet mid (VIBRATO), and the side of a stereo voice (L - R) / 2 is kept, with (tap1 - tap2) / 2 x Mix added to it. The mono sum is
//     M' = (1 - Mix) M + Mix (tap1 + tap2) / 2: a mild, even chorus, not exactly M (docs/specs/OS-INSFX-FX2.md section 3; the mode that keeps the mono sum exact is WIDE).
//   - The line is only read behind the block: the shortest tap is 66 frames (1.5 ms), so a 64-frame block reads what the blocks before it wrote. The span each tap reads (the block plus the tap's move over
//     it, at most 132 frames) is copied once a block into a scratch on the stack, the interpolation runs on the scratch, and the 64 new frames go into the line in one run (the send delay's rule,
//     OS-INSFX-FX2 section 3). Depth is limited to what Delay leaves: the swing is at most Delay - 66 frames (at Delay 5 ms: 3.5 ms of the 8 ms a Depth of 100 % asks for).
//   - Control once a block (render_st is cut into chunks of at most 64 frames): Period, Depth, Delay and Mix are targets that move toward them (Delay and Depth k = frames / 4096, about 93 ms: a Delay move is a
//     pitch glide, kept slow; Period and Mix k = frames / 256, about 6 ms); the LFO (a parabola refined to a sine, error 0.1 %) is evaluated at the block's first and last sample and the two delays and
//     the mix ramp linearly between them. The first parameters after init() are applied without a glide. set_param only clamps and stores: a MOD parameter may be called every block.
//   - Tail (OS-INSFX S2 contract): render returns 1 while the input is live (a mid peak of 1e-7, -140 dB, or more) or the line may still hold it (33 ms: the longest tap), then 0. A chunk whose peak is
//     under that writes exact zeros into the line, so once the last live frame is more than the longest tap behind the write head, every slot a tap can still reach holds a zero: the tail ends at exact zeros
//     with no one-shot clear of the 2,048 floats (the taps reach at most `tail` frames behind the write head, and quiet >= tail means all of those slots were written by quiet chunks).
//   - The input is finite by ABI: the only caller (InsertHost::stereo_block, audio/src/insert_host.cpp) builds the L and R planes from Q15 int16, so |x| <= 1 and never NaN, and the output side maps a NaN
//     to 0 (to_q15, the module rule) or scrubs it (the master insert's S6). No NaN or infinity can reach the line from the OS, so the sample loop keeps no per-sample guard for one.
//   - Parameters, in this order for ever: Period (200..20000 ms; a unit parameter shows whole numbers, so the LFO is a period and not a rate in Hz: the spec's 0.05..5 Hz), Depth (0..100 % of 8 ms),
//     Delay (5..25 ms), Mix (0..100 %).
#include <o3module.h>

#include "chorus_gen.h"

namespace {

constexpr uint32_t kLine = 2048;          // frames of the line (a power of two): the longest tap, 25 + 8 ms, is 1,456 frames at 44.1 kHz
constexpr uint32_t kMask = kLine - 1u;
constexpr uint32_t kChunk = 64;           // frames a block step takes
constexpr float kMinDelay = 66.0f;        // the shortest tap in frames: 64 for the block plus two for the interpolation
constexpr uint32_t kSpanMax = 136;        // a tap's read span (chunk + its move + 4); the move is clamped to kMoveMax
constexpr float kMoveMax = 64.0f;         // frames a tap may move over a chunk (a Delay step moves it about 14 at most, the LFO 16, Depth 6)
constexpr float kDepthMs = 8.0f;          // Depth 100 % swings a tap by 8 ms
constexpr float kTailMs = 33.0f;          // the longest tap: Delay 25 + Depth 8
constexpr float kLive = 1e-7f;            // a mid peak under this is silence (-140 dB)

struct State {
  float line[kLine];  // the mid, newest at (pos - 1) & kMask
  uint32_t pos;       // the slot of the next frame
  uint32_t quiet;     // frames since the mid was last live
  uint32_t tail;      // the frames after which the line may be considered silent
  uint32_t fresh;     // no render yet: the first block snaps to the targets
  float ms;           // frames in a millisecond
  float t_period;     // targets (set_param): the period in ms, depth 0..1, the delay in ms, the mix 0..1
  float t_depth;
  float t_delay;
  float t_mix;
  float inc;          // the smoothed values: the LFO's cycles a frame, the depth 0..1, the delay in frames, the mix
  float depth;
  float delay;
  float mix;
  uint32_t ph;        // the LFO phase at the next block's first frame, in 2^-32 of a cycle (an integer: a float phase drifts by its own rounding, 4e-3 frame of delay in 4,096 frames at the fastest swing)
  float d1;           // the taps' delays and the mix the last block ended on (the next block starts there)
  float d2;
  float mx;
};
static_assert(sizeof(State) <= CHORUS_STATE_BYTES, "module.json: state_bytes is too small for State");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN becomes lo
inline float absf(float x) { return __builtin_fabsf(x); }
// kMinDelay at a tap itself, not only through the swing: a tap never reads the chunk's own slots (kChunk frames plus two for the interpolation)
inline float min_delay(float d) { return d < kMinDelay ? kMinDelay : d; }

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->ms = host->sample_rate * 0.001f;
  s->tail = static_cast<uint32_t>(kTailMs * s->ms) + 8u;
  s->quiet = s->tail;  // a fresh line is silent (the state block is zeroed at bind)
  s->fresh = 1;
  s->t_period = chorus_params[CHORUS_PARAM_PERIOD].def;
  s->t_depth = chorus_params[CHORUS_PARAM_DEPTH].def * 0.01f;
  s->t_delay = chorus_params[CHORUS_PARAM_DELAY].def;
  s->t_mix = chorus_params[CHORUS_PARAM_MIX].def * 0.01f;
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  switch (id) {
    case CHORUS_PARAM_PERIOD: s->t_period = clampf(value, 200.0f, 20000.0f); break;
    case CHORUS_PARAM_DEPTH: s->t_depth = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    case CHORUS_PARAM_DELAY: s->t_delay = clampf(value, 5.0f, 25.0f); break;
    case CHORUS_PARAM_MIX: s->t_mix = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    default: break;
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// A sine from a parabola and one refinement: ph 0..1 -> -1..1, the error under 0.1 %.
inline float lfo(float ph) {
  const float x = ph < 0.5f ? ph : ph - 1.0f;  // -0.5 .. 0.5
  const float p = 8.0f * x * (1.0f - 2.0f * absf(x));
  return p * (0.775f + 0.225f * absf(p));
}

// a 32-bit phase as 0..1 (its top 24 bits: exact in a float)
inline float phase_of(uint32_t p) { return static_cast<float>(p >> 8) * (1.0f / 16777216.0f); }

// floor of x, for |x| under 2^30
inline int32_t floori(float x) {
  const int32_t k = static_cast<int32_t>(x);
  return static_cast<float>(k) > x ? k - 1 : k;
}

// One tap over a chunk: the line's frames from `lo` behind the write head (the chunk's first slot) are copied to a scratch, and the tap reads them at u0 + i (1 - dd) = i - delay(i), delay ramping
// from d_s to d_e; the output is in + mix (tap - m), the mix ramping from mx to mx + n dmx.
O3M_HOT void tap_pass(const State* s, uint32_t w0, float d_s, float d_e, const float* m, const float* in, float* out, float mx, float dmx, uint32_t n) {
  const float nf = static_cast<float>(n);
  const float dd = (d_e - d_s) * (1.0f / nf);  // (the one reciprocal of the chunk: 1 / 64 on the usual chunk)
  const float step = 1.0f - dd;
  const float u0 = -d_s;
  const float u1 = u0 + (nf - 1.0f) * step;
  const float umin = u0 < u1 ? u0 : u1;
  const float umax = u0 < u1 ? u1 : u0;
  const int32_t lo = floori(umin);
  const int32_t hi = static_cast<int32_t>(umax);  // toward zero: at or above floor(umax)
  uint32_t len = static_cast<uint32_t>(hi - lo) + 3u;  // the span, one spare for the rounding of the position
  len = len < kSpanMax ? len : kSpanMax;
  float sp[kSpanMax];
  uint32_t at = w0 + static_cast<uint32_t>(lo);
  for (uint32_t j = 0; j < len; ++j) {
    sp[j] = s->line[(at + j) & kMask];
  }
  const float ts0 = u0 - static_cast<float>(lo);
  float fi = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    const float ts = ts0 + fi * step;
    const int32_t k = static_cast<int32_t>(ts);
    const float a = sp[k];
    const float tap = a + (ts - static_cast<float>(k)) * (sp[k + 1] - a);
    out[i] = in[i] + (mx + fi * dmx) * (tap - m[i]);
    fi += 1.0f;
  }
}

// One chunk of 1..64 frames: out_r may be null (the mono render: the left output only).
O3M_HOT uint32_t chunk(State* s, const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t n) {
  float m[kChunk];
  float peak = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    const float v = 0.5f * (in_l[i] + in_r[i]);
    m[i] = v;
    const float a = absf(v);
    peak = a > peak ? a : peak;
  }
  const float nf = static_cast<float>(n);
  const float amax = kDepthMs * s->ms;
  const float tinc = 1.0f / (s->t_period * s->ms);  // LFO cycles a frame (ms = frames per millisecond)
  const float td = min_delay(s->t_delay * s->ms);    // the delay's target is at least kMinDelay at every rate and parameter value, so the swing's clamp below (delay - kMinDelay) holds too
  if (s->fresh != 0u) {  // the first block: the targets at once
    s->inc = tinc;
    s->depth = s->t_depth;
    s->delay = td;
    s->mix = s->t_mix;
    const float a = s->depth * amax;
    const float e = a < s->delay - kMinDelay ? a : s->delay - kMinDelay;
    const float sw = (e > 0.0f ? e : 0.0f) * lfo(phase_of(s->ph));
    s->d1 = min_delay(s->delay + sw);
    s->d2 = min_delay(s->delay - sw);
    s->mx = s->mix;
    s->fresh = 0;
  } else {
    const float kf = nf * (1.0f / 256.0f);
    const float ks = nf * (1.0f / 4096.0f);
    s->inc += kf * (tinc - s->inc);
    s->depth += ks * (s->t_depth - s->depth);
    s->delay += ks * (td - s->delay);
    s->mix += kf * (s->t_mix - s->mix);
    s->mix = absf(s->t_mix - s->mix) < 1e-4f ? s->t_mix : s->mix;  // Mix 0 must be reached: the input bit for bit
  }
  const uint32_t ph = s->ph + static_cast<uint32_t>(s->inc * nf * 4294967296.0f);  // (under 0.0073 of a cycle a chunk at 5 Hz)
  const float a = s->depth * amax;
  const float e = a < s->delay - kMinDelay ? a : s->delay - kMinDelay;
  const float sw = (e > 0.0f ? e : 0.0f) * lfo(phase_of(ph));
  float d1e = s->delay + sw;
  float d2e = s->delay - sw;
  // (a tap never moves more than kMoveMax a chunk, which the scratch is sized for; the clamp is the memory bound, no setting reaches it)
  d1e = min_delay(clampf(d1e, s->d1 - kMoveMax, s->d1 + kMoveMax));
  d2e = min_delay(clampf(d2e, s->d2 - kMoveMax, s->d2 + kMoveMax));
  const float mx_s = s->mx;
  const float mx_e = s->mix;
  const uint32_t w0 = s->pos;
  if (mx_s == 0.0f && mx_e == 0.0f) {  // the input, bit for bit
    for (uint32_t i = 0; i < n; ++i) {
      const float l = in_l[i];
      const float r = in_r[i];
      out_l[i] = l;
      if (out_r != nullptr) {
        out_r[i] = r;
      }
    }
  } else {
    const float dmx = (mx_e - mx_s) * (1.0f / nf);
    tap_pass(s, w0, s->d1, d1e, m, in_l, out_l, mx_s, dmx, n);
    if (out_r != nullptr) {
      tap_pass(s, w0, s->d2, d2e, m, in_r, out_r, mx_s, dmx, n);
    }
  }
  if (peak >= kLive) {  // a live chunk writes the mid; a quiet one writes zeros, so once quiet >= tail every slot a tap can reach is zero: the tail ends at exact zeros, no one-shot clear
    for (uint32_t i = 0; i < n; ++i) {
      s->line[(w0 + i) & kMask] = m[i];
    }
  } else {
    for (uint32_t i = 0; i < n; ++i) {
      s->line[(w0 + i) & kMask] = 0.0f;
    }
  }
  s->pos = w0 + n;
  s->ph = ph;
  s->d1 = d1e;
  s->d2 = d2e;
  s->mx = mx_e;
  if (peak >= kLive) {
    s->quiet = 0;
  } else if (s->quiet < s->tail) {
    s->quiet += n;
  }
  if (s->quiet >= s->tail) {  // the taps reach at most `tail` frames behind the write head, all written by quiet chunks: exact zeros from here
    return 0;
  }
  return 1;
}

// The stereo render: the inputs and the outputs may be the same buffers (frames 1..64 from the OS; any count is cut into chunks).
O3M_HOT uint32_t render_st(void* raw, const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  uint32_t busy = s->quiet < s->tail ? 1u : 0u;
  for (uint32_t at = 0; at < frames; at += kChunk) {
    const uint32_t n = frames - at < kChunk ? frames - at : kChunk;
    busy = chunk(s, in_l + at, in_r + at, out_l + at, out_r + at, n);
  }
  return busy;
}

// The mono render the loader requires: the left output of the same code with the mono input on both sides (no host of this OS calls it for a WIDENS effect).
uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  uint32_t busy = s->quiet < s->tail ? 1u : 0u;
  for (uint32_t at = 0; at < frames; at += kChunk) {
    const uint32_t n = frames - at < kChunk ? frames - at : kChunk;
    busy = chunk(s, in + at, in + at, out + at, nullptr, n);
  }
  return busy;
}

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    CHORUS_ENTRY_HEAD, sizeof(State), CHORUS_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, CHORUS_ENTRY_VIEWS(nullptr),
    CHORUS_ENTRY_FX(render_st),
};
