// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): CRUSH, a mono bit-depth and sample-rate reduction effect for a track insert, module 2 of the first effect pack (docs/roadmap/BACKLOG.md
// OS-INSFX-FX1), after DRIVE (modules/effects/drive) and the biquad of OS-INSFX S8 (modules/effects/biquad). All the maths is this module's own: no third-party code and no formula note
// (THIRD_PARTY.md), float only, no division and no library call in the sample loop.
//
//   - Bit depth: a quantiser with 2^(Bits-1) steps per unit of full scale (step 2^(1-Bits)): +-1 is exactly +-2^(Bits-1) steps, 0 is a level (silence in is silence out), and Bits 16 is the
//     converter's own Q15 grid, so it is the identity on a Q15 signal. The rounding is a dead-zone one: m = floor(|v| + 1/4), y = sign(x) m step, where v = x / step, a rounding with the threshold
//     at 3/4 of a step instead of 1/2. The plain mid-tread rounding makes a signal near one step LOUDER than it came in (+3 dB measured at 8 bits for a sine peaking at -46 dBFS), and the
//     insert must never be hotter than bypass at its defaults (DRIVE's level lesson): the 1/4 threshold keeps the output power under the input's for a signal spread over the steps (the mean of m^2 - |v|^2
//     over one step is negative at every level; the host test scans sines and noise from -60 to -6 dBFS), and costs 1.4 dB at most (at -36 dBFS peak at 8 bits, 0.5 dB at -30, less above). The float arithmetic is exact: the product by the power of two is exact, and
//     floor(t + 1/4) = round(t - 1/4) is done with the 1.5 * 2^23 trick (an add and a subtract, round to nearest even, no int conversion and no call), so the host oracle (double, nearbyint)
//     matches it bit for bit.
//     The quantiser has no state and no clamp: the rounding trick is exact for |x| below 128 (a Q15 signal never leaves +-1); beyond that it degrades to a nearly transparent pass-through, finite and
//     no worse than the dry path, and a NaN or infinity stays in the one sample it came in (and in the held value until the end of that block, when a non-finite or absurd held value is flushed).
//   - Rate reduction: a sample-and-hold with a fractional factor, the Rate parameter (1..64): a 32-bit phase accumulator advances by 2^32 / Rate a sample and a carry takes a new (quantised)
//     sample, so a held value lasts Rate samples on average (exactly Rate for a whole Rate). It is plain sample-and-hold: no filter before it, so it aliases, which is the effect. Rate 1 is no
//     hold at all (a separate loop, the default).
//   - Mix and Output: out = (1 - m) og x + m og held, m the Mix and og the output level; Mix 0 is the input bit for bit at Output 0 dB.
//   - Control rate: Bits and Rate are applied by set_param at once (a few integer operations and one division; no glide: the quantiser has no state to click with). Once per render() call
//     (a block, 1..128 frames) the smoothed mix and output (dB) move k = frames / 256 of the way to their targets (the output level is the only host exp2, and only while it moves), and the two
//     per-sample values ramp linearly from the last block's to the new ones in steps of kCtl samples. set_param does nothing heavy (a division at most, for Rate): a MOD parameter may be called every block.
//   - Tail (OS-INSFX S2 contract): render() returns 1 while the input is live (its last sample is not 0) or the held value is not 0, and 0 when both are silent (a held value is exactly 0
//     or at least one step; a non-finite one is flushed at the end of the block).
//   - Parameters, in this order for ever: Rate (1..64), Bits (1..16), Mix (0..100 %), Output (-24..+6 dB). Rate is first on purpose: the unit bench's moving-parameter row sweeps the first MOD target, so it
//     measures the hold loop and the division of set_param, the worst case, while its settled row runs the defaults (no hold).
#include <o3module.h>

#include "crush_gen.h"

namespace {

constexpr uint32_t kCtl = 16;           // samples a coefficient step lasts inside a block
constexpr uint32_t kBlockMax = O3M_BLOCK_MAX;
constexpr float kInsane = 1e18f;        // a held value beyond this (or NaN, or infinite) is flushed to 0 at the end of the block
constexpr float kMagic = 12582912.0f;   // 1.5 * 2^23: (t + kMagic) - kMagic rounds t to the nearest integer (even on a tie) for |t| < 2^22
constexpr float kDeadZone = 0.25f;      // floor(|v| + 1/4) = round(|v| - 1/4)
constexpr float kDbToLog2 = 0.16609640474436812f;  // log2(10)/20: 10^(db/20) = 2^(db * kDbToLog2)
constexpr float kTwo32 = 4294967296.0f;
constexpr uint32_t kIncSlack = 128u;    // the float division of 2^32 / Rate is off by up to 64: this keeps the phase step at or over the ideal one, so a whole Rate wraps on its exact sample

struct State {
  const o3m_host* host;
  float inv;       // the quantiser: steps per unit (2^(Bits-1)) and the step (2^(1-Bits)), both exact
  float step;
  uint32_t inc;    // the hold's phase step, 2^32 / Rate
  uint32_t hold;   // the hold is on (Rate over 1)
  uint32_t ph;     // the hold's phase accumulator
  float held;      // the held (quantised) value
  float t_mix;     // targets (set_param): the mix 0..1, the output in dB
  float t_out;
  float mix;       // the smoothed values
  float out_db;
  float og;        // the output level of the smoothed output
  uint32_t dirty;  // og must be computed again (the output moved)
  uint32_t fresh;  // no render yet: set_param snaps instead of gliding (the blob a bind applies, the defaults)
  float c[2];      // the per-sample values the last block ended on: the mix and the output level
};
static_assert(sizeof(State) <= CRUSH_STATE_BYTES, "module.json: state_bytes is too small for State");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN becomes lo
inline float absf(float x) { return __builtin_fabsf(x); }

inline float pow2i(int e) {  // 2^e exactly, for -126..127
  const uint32_t u = static_cast<uint32_t>(127 + e) << 23;
  float f;
  __builtin_memcpy(&f, &u, sizeof f);
  return f;
}

// The quantiser of one sample: the dead-zone rounding, inlined into the loops.
inline float quant(float x, float inv, float step) {
  const float a = absf(x * inv);
  const float m = ((a - kDeadZone) + kMagic) - kMagic;
  uint32_t xb;
  __builtin_memcpy(&xb, &x, sizeof xb);
  const float ns = -step;
  return m * ((xb >> 31) != 0u ? ns : step);  // the sign bit picks the step: an integer test, so a predicated move and no data-dependent branch in the loop (and no copysign call: the module links no libm)
}

void apply_bits(State* s, float value) {
  const uint32_t b = static_cast<uint32_t>(clampf(value, 1.0f, 16.0f) + 0.5f);
  s->inv = pow2i(static_cast<int>(b) - 1);
  s->step = pow2i(1 - static_cast<int>(b));
}

void apply_rate(State* s, float value) {
  const float h = clampf(value, 1.0f, 64.0f);
  if (h <= 1.0f) {
    s->hold = 0;
    return;
  }
  const float f = kTwo32 / h;
  s->inc = f < 4294967040.0f ? static_cast<uint32_t>(f) + kIncSlack : 0xFFFFFFFFu;  // the largest float below 2^32 that leaves room for the slack
  if (s->hold == 0u) {
    s->ph = 0u - s->inc;  // a hold that starts takes a new sample at once (the first add carries, leaving 0), then one every Rate samples
  }
  s->hold = 1;
}

inline void derive_out(State* s) {
  s->og = s->host->exp2_f(s->out_db * kDbToLog2);  // 10^(out/20)
  s->dirty = 0;
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  apply_bits(s, crush_params[CRUSH_PARAM_BITS].def);
  s->hold = 0;
  s->inc = 0;
  s->ph = 0;
  apply_rate(s, crush_params[CRUSH_PARAM_RATE].def);
  s->held = 0.0f;
  s->t_mix = crush_params[CRUSH_PARAM_MIX].def * 0.01f;
  s->t_out = crush_params[CRUSH_PARAM_OUTPUT].def;
  s->mix = s->t_mix;
  s->out_db = s->t_out;
  s->fresh = 1;
  derive_out(s);
  s->c[0] = s->mix;
  s->c[1] = s->og;
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {  // Bits and Rate act at once (cheap); the mix and the output are targets the render glides to
  State* s = static_cast<State*>(raw);
  switch (id) {
    case CRUSH_PARAM_BITS: apply_bits(s, value); return;
    case CRUSH_PARAM_RATE: apply_rate(s, value); return;
    case CRUSH_PARAM_MIX: s->t_mix = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    case CRUSH_PARAM_OUTPUT: s->t_out = clampf(value, -24.0f, 6.0f); break;
    default: return;
  }
  if (s->fresh != 0u) {  // before the first block: the value is where the effect starts, not a glide to it
    s->mix = s->t_mix;
    s->out_db = s->t_out;
    derive_out(s);
    s->c[0] = s->mix;
    s->c[1] = s->og;
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// One call of 1..kBlockMax frames: the control step, then the chain in steps of kCtl samples with the mix and the output level ramping.
O3M_HOT uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  uint32_t live = 0;
  while (frames > 0u) {  // (the ABI promises at most kBlockMax; a host that gives more is served in pieces)
    const uint32_t n = frames < kBlockMax ? frames : kBlockMax;
    s->fresh = 0;
    // ---- control step ---------------------------------------------------------------------------------------------------------------------
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
      derive_out(s);  // the output level from the moved dB (one host exp2)
    }
    const float tm = s->mix;
    const float tg = s->og;
    live |= in[n - 1u] != 0.0f ? 1u : 0u;  // before anything writes: in may be out
    const uint32_t chunks = (n + kCtl - 1u) / kCtl;
    const float invc = 1.0f / static_cast<float>(chunks);
    const float dmix = (tm - s->c[0]) * invc;
    const float dgain = (tg - s->c[1]) * invc;
    // ---- the samples ----------------------------------------------------------------------------------------------------------------------
    const float inv = s->inv;  // the quantiser and the hold, in registers for the whole block
    const float step = s->step;
    const uint32_t inc = s->inc;
    const uint32_t hold = s->hold;
    uint32_t zph = s->ph;
    float zheld = s->held;
    float c0 = s->c[0], c1 = s->c[1];
    uint32_t at = 0;
    for (uint32_t j = 0; j < chunks; ++j) {
      if (j + 1u == chunks) {  // the last step lands exactly on the target
        c0 = tm;
        c1 = tg;
      } else {
        c0 += dmix;
        c1 += dgain;
      }
      const float cm0 = (1.0f - c0) * c1;  // the mix with the output level folded in
      const float cm1 = c0 * c1;
      const uint32_t len = n - at < kCtl ? n - at : kCtl;
      if (hold != 0u) {
        for (uint32_t i = 0; i < len; ++i) {
          const float x = in[at + i];
          const float q = quant(x, inv, step);
          zph += inc;
          zheld = zph < inc ? q : zheld;  // a carry of the phase takes a new sample
          out[at + i] = cm0 * x + cm1 * zheld;
        }
      } else {
        for (uint32_t i = 0; i < len; ++i) {
          const float x = in[at + i];
          out[at + i] = cm0 * x + cm1 * quant(x, inv, step);
        }
        zheld = 0.0f;
      }
      at += len;
    }
    s->c[0] = c0;
    s->c[1] = c1;
    s->ph = zph;
    if (!(absf(zheld) <= kInsane)) {  // a NaN, an infinity or an absurd input was held (false for NaN): the hold restarts from 0 with the next block
      zheld = 0.0f;
    }
    s->held = zheld;
    live |= zheld != 0.0f ? 1u : 0u;  // a held value is still sounding
    in += n;
    out += n;
    frames -= n;
  }
  return live;  // 1 while the input is live or the hold rings, 0 when both are silent (the S2 tail contract)
}

// ---- the view: the quantiser's transfer staircase at the Bits the canvas carries ---------------------------------------------------------------------
const char* const kBitsName[16] = {"1 BIT", "2 BIT", "3 BIT", "4 BIT", "5 BIT", "6 BIT", "7 BIT", "8 BIT", "9 BIT", "10 BIT", "11 BIT", "12 BIT", "13 BIT", "14 BIT", "15 BIT", "16 BIT"};

void view_draw(void*, uint32_t view, o3m_canvas* cv) {
  if (view != CRUSH_VIEW_CURVE || cv->param_count < CRUSH_PARAM_COUNT) {
    return;
  }
  const int w = cv->width;
  const int h = cv->height;
  if (w < 16 || h < 16) {
    return;
  }
  const uint32_t b = static_cast<uint32_t>(clampf(cv->params[CRUSH_PARAM_BITS], 1.0f, 16.0f) + 0.5f);
  const float inv = pow2i(static_cast<int>(b) - 1);
  const float step = pow2i(1 - static_cast<int>(b));
  constexpr uint32_t kN = 65;  // the curve: x from -1 to 1
  int16_t xy[2 * kN];
  const int ymid = (h - 1) / 2;
  const float yscale = static_cast<float>(h - 3) / 2.0f;  // y in [-1, 1]
  for (uint32_t i = 0; i < kN; ++i) {
    const float x = -1.0f + 2.0f * static_cast<float>(i) / 64.0f;
    const float y = quant(x, inv, step);
    xy[2 * i] = static_cast<int16_t>(2 + (w - 5) * static_cast<int>(i) / 64);
    xy[2 * i + 1] = static_cast<int16_t>(ymid - static_cast<int>(y * yscale));
  }
  cv->draw->outline(cv, 1, 1, w - 2, h - 2, O3M_COLOR_EMPTY);
  cv->draw->rect(cv, 1, ymid, w - 2, 1, O3M_COLOR_EMPTY);  // the zero line
  cv->draw->polyline(cv, xy, kN, O3M_COLOR_FX);
  cv->draw->text(cv, 4, 4, kBitsName[b - 1u], O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
}

const o3m_ui kUi = {O3M_UI_SIZE(CRUSH_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    CRUSH_ENTRY_HEAD, sizeof(State), CRUSH_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, CRUSH_ENTRY_VIEWS(&kUi),
};
