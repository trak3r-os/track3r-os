// SPDX-License-Identifier: MIT
//
// TEMPLATE of a SIDECHAIN effect module (SDK guide: module/sdk/README.md, "Effects: stereo, clock, key, presets"; ABI 1.12). `o3m_gen.py new --kind effect_key --id <id>` copies this directory under your id;
// the descriptor header is generated from module.json ("stereo": "capable", a parameter of "type": "key").
//
// A ducker: the track this effect sits on is turned down while ANOTHER track plays. The `Key` parameter (OFF, T1 .. T16 on the page) names that track; every block the effect asks the OS for its level,
// host->track_level(track): the linear peak 0..1 of that track's block after its inserts and BEFORE its fader and mute, so a muted kick track still keys (the classic "ghost kick": mute the kick, keep the
// pumping). The gain is 1 - Depth x level, followed by an attack (the gain falls) and a release (it comes back), both one-pole smoothers at block rate, and ramped across the block so a step of the level
// is never a step of the sound.
//
// What the key is and is not: one number a block (the peak, not RMS, not the key's audio), at most one block (1.45 ms) late and exactly this block's when the key track is a lower track number than the
// one this effect is on (the strips run in track order), so an attack under about 3 ms is not exact and a key on a higher track is a block late. It is 0 for a silent key track and when Key is OFF.
// track_level is a member of the host table that exists from ABI 1.12: found with O3M_HOST_HAS, so an OS without it gives a ducker that never ducks, not a crash.
//
// Stereo-capable ("stereo": "capable"): render_st runs on a block where the track brings a side signal (a stereo sample), render on the others; both apply the same gain from the same state, so the
// switch between them is click-free (R8 of the spec: the module's job). Neither shape needs the other's data: the gain is one number.
//
// The rules the template follows: float only, no libm (the smoothers' coefficients use the host's exp2); render and render_st are O3M_HOT and hold the per-sample loop only, the rest of the work is once a
// block; every mutable byte is in the state block; set_param clamps (a NaN falls to the lower bound); an effect returns 0 when its input is silent and it has nothing left to say (the OS then skips it),
// and on that silent block it resets env and gain, so a note after a gap opens at unity. A track with no voice at all is never called, so no module-side code can see that gap (module/sdk/README.md, section 3b).
#include <o3module.h>

#include "example_key_gen.h"

namespace {

struct State {
  const o3m_host* host;
  float rate;      // sample rate
  float depth;     // 0..1
  float att_ms;
  float rel_ms;
  float env;       // the followed key level, 0..1
  float gain;      // the gain at the end of the last block
  uint32_t key;    // 0 none, 1..16 = the key track + 1
};
static_assert(sizeof(State) <= EXAMPLE_KEY_STATE_BYTES, "module.json: state_bytes is too small for State");

inline float clampf(float x, float lo, float hi) { return x >= lo ? (x <= hi ? x : hi) : lo; }  // a NaN is not >= lo: it becomes lo

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->rate = host->sample_rate;
  s->depth = example_key_params[EXAMPLE_KEY_PARAM_DEPTH].def * 0.01f;
  s->att_ms = example_key_params[EXAMPLE_KEY_PARAM_ATTACK].def;
  s->rel_ms = example_key_params[EXAMPLE_KEY_PARAM_RELEASE].def;
  s->key = static_cast<uint32_t>(example_key_params[EXAMPLE_KEY_PARAM_KEY].def);
  s->env = 0.0f;
  s->gain = 1.0f;
  return 0;
}

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  switch (id) {
    case EXAMPLE_KEY_PARAM_DEPTH: s->depth = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    case EXAMPLE_KEY_PARAM_KEY: s->key = static_cast<uint32_t>(clampf(value + 0.5f, 0.0f, 16.0f)); break;  // rounded: an int parameter only constrains the control
    case EXAMPLE_KEY_PARAM_ATTACK: s->att_ms = clampf(value, 1.0f, 200.0f); break;
    case EXAMPLE_KEY_PARAM_RELEASE: s->rel_ms = clampf(value, 10.0f, 2000.0f); break;
    default: break;
  }
}

void note_on(void*, uint32_t, uint32_t, uint32_t) {}
void note_off(void*, uint32_t) {}
void shutdown(void*) {}

// Once a block: the key's level through the smoothers, and the gain it asks for. `frames` of the block set the smoothers' step, so the times are right whatever the block size.
inline __attribute__((always_inline)) float block_gain(State* s, uint32_t frames) {  // (inlined: the hot render holds all of it)
  float level = 0.0f;
  if (s->key != 0u && O3M_HOST_HAS(s->host, track_level)) {
    level = clampf(s->host->track_level(s->key - 1u), 0.0f, 1.0f);
  }
  const float ms = level > s->env ? s->att_ms : s->rel_ms;  // the gain falls through the attack and comes back through the release
  const float a = 1.0f - s->host->exp2_f(-1.442695f * 1000.0f * static_cast<float>(frames) / (s->rate * ms));  // 1 - e^(-block / time)
  s->env += a * (level - s->env);
  return 1.0f - s->depth * s->env;
}

O3M_HOT uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  float any = 0.0f;
  for (uint32_t i = 0; i < frames; ++i) {
    any += in[i] != 0.0f ? 1.0f : 0.0f;
  }
  if (any == 0.0f) {  // silent input: forget the key (env 0, gain 1), so the next note opens at unity. The OS skips the effect on the silent blocks after this one, so this runs once per gap; a track with no voice at all never calls the effect (README 3b)
    s->env = 0.0f;
    s->gain = 1.0f;
    return 0;
  }
  const float g1 = block_gain(s, frames);
  float g = s->gain;
  const float dg = (g1 - g) / static_cast<float>(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    g += dg;
    out[i] = in[i] * g;
  }
  s->gain = g1;
  return 1;
}

O3M_HOT uint32_t render_st(void* raw, const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  float any = 0.0f;
  for (uint32_t i = 0; i < frames; ++i) {
    any += (in_l[i] != 0.0f || in_r[i] != 0.0f) ? 1.0f : 0.0f;  // read both before writing: in and out may be the same buffers
  }
  if (any == 0.0f) {  // silent input: the same reset as render (once per gap; see README 3b)
    s->env = 0.0f;
    s->gain = 1.0f;
    return 0;
  }
  const float g1 = block_gain(s, frames);
  float g = s->gain;
  const float dg = (g1 - g) / static_cast<float>(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    g += dg;
    const float l = in_l[i];
    const float r = in_r[i];
    out_l[i] = l * g;
    out_r[i] = r * g;
  }
  s->gain = g1;
  return 1;
}

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    EXAMPLE_KEY_ENTRY_HEAD, sizeof(State), EXAMPLE_KEY_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, EXAMPLE_KEY_ENTRY_VIEWS(nullptr),
    EXAMPLE_KEY_ENTRY_FX(render_st),
};
