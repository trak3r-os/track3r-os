// SPDX-License-Identifier: MIT
//
// TEMPLATE of a STEREO effect module (SDK guide: module/sdk/README.md, "Effects: stereo, clock, key, presets"; ABI 1.12). `o3m_gen.py new --kind effect_stereo --id <id>` copies this directory
// under your id; the descriptor header is generated from module.json ("stereo": "widens", the "presets", every parameter).
//
// A WIDENS effect makes a stereo image from a mono input: the OS calls render_st(in_l, in_r, out_l, out_r, frames) on EVERY block, with the track's mono block on both inputs (and the side signal
// a stereo voice or an earlier stereo insert brings folded in: L = M + S, R = M - S). This one is an auto-pan: the left and right gains swing in opposite directions, so a mono sound moves from
// side to side. The cycle is the length of one swing: `Beats` beats when Sync is on (locked to the sequencer through get_clock, so the swing lands on the downbeat), `Cycle` ms when it is off.
// `Cycle` is in milliseconds, not Hz, on purpose: the OS shows a parameter that has a unit as whole numbers, and a slow pan needs fractions of a Hz (README section 2).
//
// The rules the template follows: float only, no libm (the LFO is a parabola, not sin); render_st is O3M_HOT and holds nothing but the per-sample loop; every mutable byte is in the state block;
// set_param clamps (a NaN falls to the lower bound); a mono `render` exists because the loader requires it of every effect, but a WIDENS effect's is called by no host of this OS: it is the
// mid of render_st (the left and right gains averaged), so it is valid should one ever call it. A stereo-capable effect ("stereo": "capable") would use render for the blocks that bring no side
// signal and make the switch between the two click-free in its own state; this one never switches.
//
// The clock: get_clock is an ABI 1.12 member, found with O3M_HOST_HAS (an OS that lacks it, or answers 1 because it has no sequencer, gives this module 120 BPM). The beat it reports is the
// block's first sample, and an insert runs once a block, so the sync is re-locked at each call; a stopped sequencer gives the project tempo and beat 0, and the swing then free-runs at that tempo.
// The OS does not call render_st before OS-INSFX-ABI2 A4, and the clock is a stub until A3: the template builds, packs and loads, and sounds only after both.
#include <o3module.h>

#include "example_stereo_gen.h"

namespace {

struct State {
  const o3m_host* host;
  float rate;         // sample rate
  float smooth;       // the depth smoother's coefficient (about 10 ms)
  float phase;        // the swing, 0..1
  float depth;        // the depth in force, 0..1 (smoothed)
  float depth_to;     // the depth asked for
  float cycle_ms;
  uint32_t beats;     // the cycle's length in beats, 1..16
  uint32_t sync;      // 1: the swing follows the sequencer's beat
};
static_assert(sizeof(State) <= EXAMPLE_STEREO_STATE_BYTES, "module.json: state_bytes is too small for State");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN is not >= lo: it becomes lo

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->rate = host->sample_rate;
  s->smooth = 1.0f - host->exp2_f(-1.442695f / (host->sample_rate * 0.01f));  // 1 - e^(-1 / (fs * 10 ms))
  s->phase = 0.0f;
  s->sync = example_stereo_params[EXAMPLE_STEREO_PARAM_SYNC].def >= 0.5f ? 1u : 0u;
  s->beats = static_cast<uint32_t>(example_stereo_params[EXAMPLE_STEREO_PARAM_BEATS].def);
  s->cycle_ms = example_stereo_params[EXAMPLE_STEREO_PARAM_CYCLE].def;
  s->depth_to = example_stereo_params[EXAMPLE_STEREO_PARAM_DEPTH].def * 0.01f;
  s->depth = s->depth_to;
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  switch (id) {
    case EXAMPLE_STEREO_PARAM_SYNC: s->sync = value >= 0.5f ? 1u : 0u; break;
    case EXAMPLE_STEREO_PARAM_BEATS: s->beats = static_cast<uint32_t>(clampf(value + 0.5f, 1.0f, 16.0f)); break;  // rounded: an int parameter only constrains the control
    case EXAMPLE_STEREO_PARAM_CYCLE: s->cycle_ms = clampf(value, 50.0f, 10000.0f); break;
    case EXAMPLE_STEREO_PARAM_DEPTH: s->depth_to = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    default: break;
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// The mono render the loader requires: the mid of the stereo one, at the depth in force (no phase, no clock).
uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  const State* s = static_cast<const State*>(raw);
  const float mid = 1.0f - 0.5f * s->depth_to;
  for (uint32_t i = 0; i < frames; ++i) {
    out[i] = in[i] * mid;
  }
  return 1;  // an effect with a tail returns 0 when it is silent and idle; an auto-pan has none
}

// A sine from two parabolas: ph 0..1 -> -1..1, the error under 6 %: an LFO, not a tone.
inline float lfo(float ph) {
  const float x = ph < 0.5f ? ph : ph - 1.0f;  // -0.5 .. 0.5
  const float ax = x < 0.0f ? -x : x;
  return 8.0f * x * (1.0f - 2.0f * ax);
}

O3M_HOT uint32_t render_st(void* raw, const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  uint32_t tempo_x100 = 12000u;  // the fallback: 120 BPM
  float ph = s->phase;
  if (s->sync != 0u) {
    o3m_clock clk;  // fill the members one by one: a struct initialiser may become a memset, and a module has no libc
    clk.size = sizeof(clk);
    clk.flags = 0;
    clk.tempo_x100 = 12000u;
    clk.beat = 0.0f;
    if (O3M_HOST_HAS(s->host, get_clock) && s->host->get_clock(&clk) == 0 && clk.tempo_x100 != 0u) {
      tempo_x100 = clk.tempo_x100;
      if ((clk.flags & O3M_CLOCK_PLAYING) != 0u) {  // locked to the pattern's beat: the swing starts on the downbeat
        const float lock = clk.beat / static_cast<float>(s->beats);
        ph = lock >= 0.0f && lock < 1.0e6f ? lock - static_cast<float>(static_cast<int32_t>(lock)) : 0.0f;
      }
    }
  }
  // the phase step of one sample: beats a second (tempo / 60) over the cycle's beats, or one cycle in cycle_ms
  const float step = s->sync != 0u ? static_cast<float>(tempo_x100) * (1.0f / 6000.0f) / (static_cast<float>(s->beats) * s->rate) : 1000.0f / (s->cycle_ms * s->rate);
  float depth = s->depth;
  const float depth_to = s->depth_to;
  const float smooth = s->smooth;
  for (uint32_t i = 0; i < frames; ++i) {
    depth += smooth * (depth_to - depth);
    const float w = lfo(ph);  // -1 .. 1: the left gain falls as the right one rises
    const float gl = 1.0f - depth * 0.5f * (1.0f + w);  // the mid (gl + gr) / 2 is 1 - depth / 2 at any phase: the side (gl - gr) / 2 is the swing
    const float gr = 1.0f - depth * 0.5f * (1.0f - w);
    const float l = in_l[i];  // read both before writing: in and out may be the same buffers
    const float r = in_r[i];
    out_l[i] = l * gl;
    out_r[i] = r * gr;
    ph += step;
    ph -= ph >= 1.0f ? 1.0f : 0.0f;
  }
  s->phase = ph;
  s->depth = depth;
  return 1;
}

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    EXAMPLE_STEREO_ENTRY_HEAD, sizeof(State), EXAMPLE_STEREO_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, EXAMPLE_STEREO_ENTRY_VIEWS(nullptr),
    EXAMPLE_STEREO_ENTRY_FX(render_st),
};
