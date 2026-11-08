// SPDX-License-Identifier: MIT
//
// The example module of the SDK (module/sdk/o3module.h): a sine voice with a linear attack and release. It is what the
// loader tests pack and load, and the smallest thing that shows every rule: all state in the instance block, the audio
// path marked O3M_HOT and free of libm, the parameters described for the OS's generated edit page.
//
// sine(p) for the phase p in [0, 1) is the parabola 4x(1-|x|) of x = 2p - 1 (negated, so that p = 0.25 is the peak) with the
// usual weight refinement y = 0.775 y + 0.225 y |y|; its error is about 1e-3: plenty for a test tone, and no table.
#include <o3module.h>

namespace {

enum Param : uint32_t { kLevel = 0, kRelease, kParamCount };

const o3m_param kParams[kParamCount] = {
    {"Level", 0.0f, 1.0f, 0.5f, 0},
    {"Release ms", 1.0f, 2000.0f, 50.0f, O3M_PARAM_LOG},
};

struct State {
  float phase;       // 0..1
  float inc;         // phase step a sample
  float env;         // 0..1
  float attack_step; // per sample
  float release_step;
  float level;
  float release_ms;
  float rate;        // Hz
  const o3m_host* host;
  uint32_t gate;     // 1 while the note is held
  uint32_t sounding; // 1 until the release has run out
};

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->host = host;
  s->rate = host->sample_rate;
  s->level = kParams[kLevel].def;
  s->release_ms = kParams[kRelease].def;
  s->attack_step = 1.0f / (0.002f * s->rate);  // 2 ms
  s->release_step = 1000.0f / (s->release_ms * s->rate);
  return 0;
}

void set_param(void* raw, uint32_t id, float v) {
  State* s = static_cast<State*>(raw);
  if (id == kLevel) {
    s->level = clampf(v, 0.0f, 1.0f);
  } else if (id == kRelease) {
    s->release_ms = clampf(v, 1.0f, 2000.0f);
    s->release_step = 1000.0f / (s->release_ms * s->rate);
  }
}

void note_on(void* raw, uint32_t note, uint32_t velocity, uint32_t flags) {
  State* s = static_cast<State*>(raw);
  (void)velocity;
  (void)flags;
  const uint32_t n = note > 127u ? 127u : note;
  // 440 Hz * 2^((n - 69) / 12), by the host's exp2 (this runs once a note, not once a sample)
  s->inc = 440.0f * s->host->exp2_f((static_cast<float>(n) - 69.0f) * (1.0f / 12.0f)) / s->rate;
  s->gate = 1;
  s->sounding = 1;
}

void note_off(void* raw, uint32_t note) {
  (void)note;
  static_cast<State*>(raw)->gate = 0;
}

void shutdown(void*) {}

O3M_HOT uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  State* s = static_cast<State*>(raw);
  (void)in;
  float phase = s->phase;
  float env = s->env;
  const float inc = s->inc;
  const float level = s->level;
  const bool gate = s->gate != 0u;
  const float up = s->attack_step;
  const float down = s->release_step;
  for (uint32_t i = 0; i < frames; ++i) {
    if (gate) {
      env = env + up > 1.0f ? 1.0f : env + up;
    } else {
      env = env - down < 0.0f ? 0.0f : env - down;
    }
    float x = 2.0f * phase - 1.0f;
    float ax = x < 0.0f ? -x : x;
    float y = -4.0f * x * (1.0f - ax);
    float ay = y < 0.0f ? -y : y;
    y = 0.775f * y + 0.225f * y * ay;
    out[i] = level * env * y;
    phase += inc;
    phase -= phase >= 1.0f ? 1.0f : 0.0f;
  }
  s->phase = phase;
  s->env = env;
  if (!gate && env <= 0.0f) {
    s->sounding = 0;
  }
  return s->sounding;
}

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, O3M_MODULE_SIZE(O3M_ABI_MINOR), "Sine", sizeof(State), kParamCount, kParams,
    init, render, set_param, note_on, note_off, shutdown, nullptr, 0,
    // ABI 1.8 to 1.12 members (pitch, pages, page_count, labels, importer, render_st), then fx_flags: it renders correctly at the half and quarter rate (OS-MODRATE).
    nullptr, nullptr, 0, nullptr, nullptr, nullptr, O3M_FX_RATE,
};
