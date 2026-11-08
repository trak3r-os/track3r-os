// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX2 X3 (new Community OS behaviour): PING-PONG, a tempo-synced stereo echo for a track insert (docs/specs/OS-INSFX-FX2.md section 4.3), a WIDENS effect of ABI 1.12: the OS calls render_st on
// every block with the track's mono block on both inputs (plus the side signal a stereo voice or an earlier stereo insert folded in). All the maths is this module's own: no third-party code and no formula
// note (THIRD_PARTY.md), float only, no libm, no double, no division and no library call in the sample loop.
//
//   - The loop, as the send delay's ping-pong (audio/send_delay.h, facts only: the module has its own code): one line of interleaved stereo frames, a frame one 32-bit word (left in the low half, right in the high
//     half, two int16). With x the input's mid, (Lr, Rr) the line read D frames back, f the feedback filter and g the feedback gain:
//         write frame t:  Lw = x + g f(Rr)   Rw = g f(Lr)        so the first echo is on the left, the next on the right, and so on, D apart.
//     The output is out_L = in_L + Mix (a Lr + b Rr), out_R = in_R + Mix (b Lr + a Rr), a = (1 + Width) / 2 and b = (1 - Width) / 2, written as Mix (b (Lr + Rr) + Width Lr): Width 0 centres the echoes, and since
//     a + b = 1 the mono sum (L + R) / 2 is the input's mid plus Mix (Lr + Rr) / 2 whatever the Width (the Width only moves the echoes between the ears).
//   - f is a one-pole low-pass (High cut) followed by a one-pole high-pass (Low cut: the input less its own one-pole low-pass). Both have a gain of at most 1 at every frequency (the high-pass peaks at
//     2 (1 - c) / (2 - c) at Nyquist, under 1), g is at most 0.95, so the loop gain is under unity at every frequency, a time-varying coefficient included (each pole is a convex combination of its state and its input).
//   - The line holds int16 words: the written value is saturated to -32768..32767 and ROUNDED TOWARDS ZERO (a C cast), so a recirculating tail reaches exactly 0 (no limit cycle from rounding) and a clipped loop is
//     bounded, never wrapped. The values in the loop are in int16 units (the input is scaled by 16384 = 32768 / 2 for the mid, the output coefficients carry the 1 / 32768).
//   - Time. Synced: D = round(rate x fs x 15 / bpm), the rate a note length in sequencer steps (a step is a 16th note, "4" a quarter), the tempo from o3m_host.get_clock (120 BPM when the host has no clock, or
//     says it has none); a D over the longest time (1 s) is halved until it fits (it stays on the grid), and D is held to 64 frames at least. FREE: D = round(Time ms x fs / 1000). A change of D (a knob, the tempo,
//     the Sync) does not jump: the old tap and the new one are cross-faded over 1,024 frames (23 ms) with a linear pair of gains that sum to 1 (the repeats of one signal are correlated, so the level holds and
//     the loop gain stays under unity through the fade); a change that comes while a fade runs waits for it to end (the newest target wins).
//   - SDRAM. The line is the module's state (177 KB: the OS puts a state that big in SDRAM). Every delay is at least 64 frames, a block of 64 frames reads only what earlier blocks wrote: the read span of a tap
//     is copied once a block into a stack scratch (a word loop: a module has no libc), all the arithmetic runs on the scratch, the 64 new words are written back in one burst. A block of more than 64 frames is
//     processed in chunks of 64 (the rule needs a chunk no longer than the shortest delay).
//   - Control. Feedback, Low cut, High cut, Mix and Width are targets (set_param is a clamp and a store: every one is a MOD target); the host exp2 of the two poles runs only when one of them changed. Once a chunk
//     the five values move 0.2 of the way (about 7 ms at 64 frames, scaled by the chunk's length) and the three that act per sample (g and the two output coefficients) ramp linearly inside the chunk; the first
//     parameters after init() (the stored blob), and the first chunk after a long silence, are applied without a glide. The Time and Sync act through the cross-fade.
//   - Tail (OS-INSFX S2 contract): the module counts the consecutive zero words it has written; once that is the whole line (1 s: nothing a tail could still say is left, and no old audio is left to
//     show up when Time grows) the line is exactly 0: the filters are cleared, a silent block is copied through and the work is skipped. render returns 1 until then, and while the input is live (its last
//     sample is not 0 in either channel).
//   - Parameters, in this order for ever: Sync (0 FREE, 1..15 the 15 longest step lengths of the send delay: 8 6 4 3 2 3/2 1 3/4 1/2 3/8 1/3 1/4 3/16 1/6 1/8), Time (10..1000 ms), Feedback (0..95 %),
//     Low cut (20..2000 Hz), High cut (1000..20000 Hz), Mix (0..100 %), Width (0..100 %).
//   - The mono `render` the loader requires is the same code with the right input equal to the left one and the left output returned (a fallback no host of this OS calls for a WIDENS effect).
#include <o3module.h>

#include "pingpong_gen.h"

namespace {

constexpr uint32_t kChunk = 64;          // frames a chunk: not above the shortest delay
constexpr uint32_t kRing = 44200;        // frames of the line: 1 s at 44.1 kHz and a margin for the shortest read
constexpr uint32_t kMinDelay = 64;
constexpr uint32_t kXfade = 1024;
constexpr float kGlide = 0.2f;           // the way a value moves to its target in a chunk of 64 frames
constexpr float kInv64 = 1.0f / 64.0f;
constexpr float kTwoPiLog2e = 9.0647203f;  // 2 pi / ln 2: a one-pole's coefficient is 1 - 2^(-kTwoPiLog2e f / fs)
constexpr float kInvLsb = 1.0f / 32768.0f;
constexpr uint32_t kDefaultTempo = 12000u;  // 120 BPM x 100

// A note length in 1/96 of a sequencer step, the send delay's own table (audio/graph_params.h kDelayRates96); index 0 is FREE.
constexpr uint16_t kRates96[16] = {0, 768, 576, 384, 288, 192, 144, 96, 72, 48, 36, 32, 24, 18, 16, 12};

struct State {
  const o3m_host* host;
  float fs;
  float fs_ms;          // fs / 1000
  float sync_k;         // 1500 fs / 96: a delay in frames is rate96 x sync_k / (tempo x 100)
  float cap_f;          // the longest delay (1 s) as a float, and as frames
  uint32_t cap;
  // parameters as set (clamped)
  uint32_t sync;
  float time_ms, feedback, low_hz, high_hz, mix, width;
  uint32_t dirty;       // a pole, the feedback or the output coefficients are stale
  // the targets derived from them, and the values in force (they glide to the targets): g, the low-pass pole, the high-pass pole, the output coefficients on the side sum and on the same side's read
  float t[5];
  float c[5];
  uint32_t fresh;       // the next chunk takes the targets at once
  // the feedback filters: [0] feeds the left write (from the right read), [1] the right write
  float lp0, hp0, lp1, hp1;
  // the line
  uint32_t pos;         // the next frame to write
  uint32_t d_cur, d_old, fade_left;
  uint32_t zero_run;    // consecutive zero words written, at most kRing
  uint32_t line[kRing];
};
static_assert(sizeof(State) <= PINGPONG_STATE_BYTES, "module.json: state_bytes is too small for State");
static_assert(kChunk <= kMinDelay, "a chunk must not outrun the shortest delay");

// Saturates to int16: one SSAT on the unit.
inline int32_t sat16(int32_t v) {
#if defined(__ARM_FEATURE_SAT)
  return __builtin_arm_ssat(v, 16);
#else
  return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
#endif
}

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN is not >= lo: it becomes lo

// A value that moves a fraction k of the way to its target (and lands on it once it is within 1e-4 of it).
inline float slew(float cur, float target, float k) {
  const float d = target - cur;
  const float ad = d < 0.0f ? -d : d;
  const float at = target < 0.0f ? -target : target;
  return ad <= 1.0e-4f * at + 1.0e-12f ? target : cur + k * d;
}

// The targets from the parameters (the two host exp2 calls): cold code, run only when a parameter changed.
__attribute__((noinline)) void derive(State* s) {
  const float inv_fs = 1.0f / s->fs;
  s->t[1] = clampf(1.0f - s->host->exp2_f(-kTwoPiLog2e * s->high_hz * inv_fs), 0.0f, 1.0f);
  s->t[2] = clampf(1.0f - s->host->exp2_f(-kTwoPiLog2e * s->low_hz * inv_fs), 0.0f, 1.0f);
  s->t[0] = s->feedback * 0.01f;
  const float mix = s->mix * 0.01f;
  const float w = s->width * 0.01f;
  s->t[3] = mix * 0.5f * (1.0f - w) * kInvLsb;  // Mix b / 32768, on (Lr + Rr)
  s->t[4] = mix * w * kInvLsb;                  // Mix Width / 32768, on the same side's read
  s->dirty = 0;
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  float fs = host->sample_rate;
  if (!(fs >= 8000.0f && fs <= 96000.0f)) {
    fs = 44100.0f;
  }
  s->fs = fs;
  s->fs_ms = fs * 0.001f;
  s->sync_k = 1500.0f * fs / 96.0f;
  uint32_t cap = static_cast<uint32_t>(fs + 0.5f);
  cap = cap > kRing - kMinDelay ? kRing - kMinDelay : cap;
  s->cap = cap;
  s->cap_f = static_cast<float>(cap);
  s->sync = static_cast<uint32_t>(pingpong_params[PINGPONG_PARAM_SYNC].def);
  s->time_ms = pingpong_params[PINGPONG_PARAM_TIME].def;
  s->feedback = pingpong_params[PINGPONG_PARAM_FEEDBACK].def;
  s->low_hz = pingpong_params[PINGPONG_PARAM_LOW_CUT].def;
  s->high_hz = pingpong_params[PINGPONG_PARAM_HIGH_CUT].def;
  s->mix = pingpong_params[PINGPONG_PARAM_MIX].def;
  s->width = pingpong_params[PINGPONG_PARAM_WIDTH].def;
  derive(s);
  s->fresh = 1u;
  s->lp0 = s->hp0 = s->lp1 = s->hp1 = 0.0f;
  s->pos = 0u;
  s->d_cur = s->d_old = s->fade_left = 0u;
  s->zero_run = kRing;  // the OS hands init() a zeroed state: the line is silent
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  switch (id) {
    case PINGPONG_PARAM_SYNC: s->sync = static_cast<uint32_t>(clampf(value + 0.5f, 0.0f, 15.0f)); break;  // rounded: an int parameter only constrains the control
    case PINGPONG_PARAM_TIME: s->time_ms = clampf(value, 10.0f, 1000.0f); break;
    case PINGPONG_PARAM_FEEDBACK: s->feedback = clampf(value, 0.0f, 95.0f); s->dirty = 1u; break;
    case PINGPONG_PARAM_LOW_CUT: s->low_hz = clampf(value, 20.0f, 2000.0f); s->dirty = 1u; break;
    case PINGPONG_PARAM_HIGH_CUT: s->high_hz = clampf(value, 1000.0f, 20000.0f); s->dirty = 1u; break;
    case PINGPONG_PARAM_MIX: s->mix = clampf(value, 0.0f, 100.0f); s->dirty = 1u; break;
    case PINGPONG_PARAM_WIDTH: s->width = clampf(value, 0.0f, 100.0f); s->dirty = 1u; break;
    default: break;
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// n words between the line (from frame `at`) and buf, a word loop in runs that stop at the ring's end: out of the line (write 0) or into it. One function in the hot part, called, not inlined.
O3M_HOT void line_io(uint32_t* line, uint32_t at, uint32_t n, uint32_t* buf, uint32_t write) {
  while (n != 0u) {
    const uint32_t run = kRing - at < n ? kRing - at : n;
    if (write != 0u) {
      for (uint32_t i = 0; i < run; ++i) {
        line[at + i] = buf[i];
      }
    } else {
      for (uint32_t i = 0; i < run; ++i) {
        buf[i] = line[at + i];
      }
    }
    buf += run;
    n -= run;
    at = 0u;
  }
}

// The cross-fade of two taps, done on the words: a[i] becomes the old tap's (b) plus (new - old) x w / 1024, per channel, w = (done + i + 1) / 1024 at most 1, the quotient rounded toward zero (so
// that a pair of zeros stays zero and a one-LSB tail does not hold its weight from the first sample).
O3M_HOT void blend(uint32_t* a, const uint32_t* b, uint32_t n, uint32_t done) {
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t wi = done + i + 1u;
    wi = wi > kXfade ? kXfade : wi;
    const int32_t la = static_cast<int32_t>(a[i] << 16) >> 16, ra = static_cast<int32_t>(a[i]) >> 16;
    const int32_t lb = static_cast<int32_t>(b[i] << 16) >> 16, rb = static_cast<int32_t>(b[i]) >> 16;
    const int32_t l = lb + ((la - lb) * static_cast<int32_t>(wi)) / 1024;
    const int32_t r = rb + ((ra - rb) * static_cast<int32_t>(wi)) / 1024;
    a[i] = (static_cast<uint32_t>(l) & 0xFFFFu) | (static_cast<uint32_t>(r) << 16);
  }
}

// The frame `d` before the write position.
inline uint32_t behind(uint32_t pos, uint32_t d) {
  const uint32_t at = pos + kRing - d;
  return at >= kRing ? at - kRing : at;
}

// One chunk of n (1..64) frames: the loop, on the words of the read tap (ra) and the 64 new words (wr).
inline __attribute__((always_inline)) uint32_t loop(State* s, const float* il, const float* ir, float* ol, float* orr, uint32_t n, const uint32_t* ra, uint32_t* wr, float g, float dg, float cs, float dcs, float cd,
                                                    float dcd, float cl, float ch) {
  float lp0 = s->lp0, hp0 = s->hp0, lp1 = s->lp1, hp1 = s->hp1;
  uint32_t acc = 0u;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t wa = ra[i];
    const float lr = static_cast<float>(static_cast<int32_t>(wa << 16) >> 16);
    const float rr = static_cast<float>(static_cast<int32_t>(wa) >> 16);
    lp0 += cl * (rr - lp0);  // f(Rr) feeds the left write, f(Lr) the right one
    hp0 += ch * (lp0 - hp0);
    lp1 += cl * (lr - lp1);
    hp1 += ch * (lp1 - hp1);
    const float l = il[i];  // read both before writing: in and out may be the same buffers
    const float r = ir[i];
    // clamped into the int16 range before the cast: a float out of the int32 range has no defined conversion (x86 yields INT_MIN, which sat16 then clips to -32768, so a huge positive would write a negative word; the unit saturates instead); inside the range the cast still truncates toward zero
    const int32_t sl = sat16(static_cast<int32_t>(clampf((l + r) * 16384.0f + g * (lp0 - hp0), -32768.0f, 32767.0f)));
    const int32_t sr = sat16(static_cast<int32_t>(clampf(g * (lp1 - hp1), -32768.0f, 32767.0f)));
    const uint32_t word = (static_cast<uint32_t>(sl) & 0xFFFFu) | (static_cast<uint32_t>(sr) << 16);
    wr[i] = word;
    acc |= word;
    const float side = cs * (lr + rr);
    ol[i] = l + (side + cd * lr);
    orr[i] = r + (side + cd * rr);
    g += dg;
    cs += dcs;
    cd += dcd;
  }
  s->lp0 = lp0;
  s->hp0 = hp0;
  s->lp1 = lp1;
  s->hp1 = hp1;
  return acc;
}

inline __attribute__((always_inline)) void chunk(State* s, const float* il, const float* ir, float* ol, float* orr, uint32_t n, uint32_t tempo) {
  if (s->zero_run >= kRing) {  // the line is exactly 0: a sound after a long silence starts as it is set
    s->fresh = 1u;
  }
  // the delay in frames
  uint32_t d_new;
  if (s->sync == 0u) {
    d_new = static_cast<uint32_t>(s->time_ms * s->fs_ms + 0.5f);
  } else {
    float d = static_cast<float>(kRates96[s->sync]) * s->sync_k / static_cast<float>(tempo);
    while (d > s->cap_f) {  // a time over the line is halved until it fits: it stays on the grid
      d *= 0.5f;
    }
    d_new = static_cast<uint32_t>(d + 0.5f);
  }
  d_new = d_new < kMinDelay ? kMinDelay : (d_new > s->cap ? s->cap : d_new);
  if (s->fresh != 0u) {
    s->d_cur = d_new;
    s->d_old = 0u;
    s->fade_left = 0u;
#pragma GCC unroll 1
    for (uint32_t j = 0; j < 5u; ++j) {
      s->c[j] = s->t[j];
    }
    s->fresh = 0u;
  } else if (s->fade_left == 0u && d_new != s->d_cur) {
    s->d_old = s->d_cur;
    s->d_cur = d_new;
    s->fade_left = kXfade;
  }
  // the values glide: the pole coefficients are held over the chunk, g and the two output coefficients ramp
  const float k = kGlide * static_cast<float>(n) * kInv64;
  const float inv_n = 1.0f / static_cast<float>(n);
  const float g0 = s->c[0], cs0 = s->c[3], cd0 = s->c[4];
#pragma GCC unroll 1
  for (uint32_t j = 0; j < 5u; ++j) {
    s->c[j] = slew(s->c[j], s->t[j], k);
  }
  const float g1 = s->c[0], cl = s->c[1], ch = s->c[2], cs1 = s->c[3], cd1 = s->c[4];
  // the line: read the taps (cross-fading them while a fade runs), run the loop, write the 64 new words
  uint32_t ra[kChunk], wr[kChunk];
  const uint32_t pos = s->pos;
  line_io(s->line, behind(pos, s->d_cur), n, ra, 0u);
  if (s->fade_left != 0u) {
    uint32_t rb[kChunk];
    line_io(s->line, behind(pos, s->d_old), n, rb, 0u);
    blend(ra, rb, n, kXfade - s->fade_left);
    s->fade_left = s->fade_left > n ? s->fade_left - n : 0u;
    s->d_old = s->fade_left != 0u ? s->d_old : 0u;
  }
  const uint32_t acc = loop(s, il, ir, ol, orr, n, ra, wr, g0, (g1 - g0) * inv_n, cs0, (cs1 - cs0) * inv_n, cd0, (cd1 - cd0) * inv_n, cl, ch);
  line_io(s->line, pos, n, wr, 1u);
  s->pos = pos + n >= kRing ? pos + n - kRing : pos + n;
  if (acc == 0u) {
    const uint32_t z = s->zero_run + n;
    s->zero_run = z > kRing ? kRing : z;
    if (s->zero_run >= kRing) {  // the whole line is 0: nothing rings
      s->lp0 = s->hp0 = s->lp1 = s->hp1 = 0.0f;
    }
  } else {
    s->zero_run = 0u;
  }
}

O3M_HOT uint32_t render_st(void* raw, const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  if (s->dirty != 0u) {
    derive(s);
  }
  uint32_t tempo = kDefaultTempo;
  if (s->sync != 0u) {
    o3m_clock clk;  // fill the members one by one: a struct initialiser may become a memset, and a module has no libc
    clk.size = sizeof(clk);
    clk.flags = 0;
    clk.tempo_x100 = kDefaultTempo;
    clk.beat = 0.0f;
    if (O3M_HOST_HAS(s->host, get_clock) && s->host->get_clock(&clk) == 0 && clk.tempo_x100 != 0u) {
      tempo = clk.tempo_x100;
    }
  }
  uint32_t live = 0u;
  for (uint32_t at = 0; at < frames; at += kChunk) {
    const uint32_t n = frames - at < kChunk ? frames - at : kChunk;
    for (uint32_t i = 0; i < n; ++i) {  // the whole block is live, not only its last frame; read before the chunk writes: in and out may be the same buffers
      live |= (in_l[at + i] != 0.0f) | (in_r[at + i] != 0.0f);
    }
    chunk(s, in_l + at, in_r + at, out_l + at, out_r + at, n, tempo);
  }
  return s->zero_run < kRing || live != 0u ? 1u : 0u;
}

// The mono render the loader requires: render_st on one input twice, the left output kept.
uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  float right[O3M_BLOCK_MAX];
  uint32_t busy = 0u;
  for (uint32_t at = 0; at < frames; at += O3M_BLOCK_MAX) {
    const uint32_t n = frames - at < O3M_BLOCK_MAX ? frames - at : O3M_BLOCK_MAX;
    busy |= render_st(raw, in + at, in + at, out + at, right, n);
  }
  return busy;
}

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    PINGPONG_ENTRY_HEAD, sizeof(State), PINGPONG_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, PINGPONG_ENTRY_VIEWS(nullptr),
    PINGPONG_ENTRY_FX(render_st),
};
