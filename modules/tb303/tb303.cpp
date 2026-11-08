// SPDX-License-Identifier: MIT
//
// The TB-303 as a Community OS module (OS-012a, ADR 0002): the descriptor and the glue around tb303_dsp.h (the voice). Open303 (c) 2009 Robin
// Schmidt, MIT: LICENSE-Open303.txt. One mono voice an instance; the host runs it as an instrument on a track.
#include <o3module.h>

#include "tb303_dsp.h"
#include "tb303_gen.h"

namespace {

using tb303::Voice;

static_assert(sizeof(Voice) <= 1024, "an instance of the voice is under a kilobyte");

// The parameters (names, ranges, units, curves, flags), the views and the descriptor's fixed fields are the manifest's: module.json, turned into tb303_gen.h by
// tools/o3m_gen.py (the `o3m_gen` ctest finds a stale copy). The DSP's own ids (tb303::Param) must be the manifest's order: held here at compile time.
constexpr bool same_id(unsigned a, unsigned b) { return a == b; }
static_assert(same_id(TB303_PARAM_COUNT, tb303::kParamCount), "module.json has the parameters of tb303_dsp.h");
static_assert(same_id(TB303_PARAM_WAVEFORM, tb303::kWaveform) && same_id(TB303_PARAM_CUTOFF, tb303::kCutoff) && same_id(TB303_PARAM_RESONANCE, tb303::kResonance) &&
                  same_id(TB303_PARAM_ENV_MOD, tb303::kEnvMod) && same_id(TB303_PARAM_DECAY, tb303::kDecay) && same_id(TB303_PARAM_ACCENT, tb303::kAccent) &&
                  same_id(TB303_PARAM_SLIDE, tb303::kSlide) && same_id(TB303_PARAM_VOLUME, tb303::kVolume) && same_id(TB303_PARAM_LEGATO_SLIDE, tb303::kLegato) &&
                  same_id(TB303_PARAM_TUNE, tb303::kTune) && same_id(TB303_PARAM_TRANSPOSE, tb303::kTranspose) && same_id(TB303_PARAM_ACC_THRESH, tb303::kAccThresh) &&
                  same_id(TB303_PARAM_FILTER_TRACK, tb303::kFilterTrack) && same_id(TB303_PARAM_ACC_DECAY, tb303::kAccDecay),
              "the parameter ids of module.json are the DSP's");
static_assert(sizeof(Voice) <= TB303_STATE_BYTES, "module.json says how much state the instance block has");

int init(void* raw, const o3m_host* host) {
  if (host->size < sizeof(o3m_host)) {
    return 1;
  }
  tb303::Services sv = {host->exp2_f, host->log2_f, host->sin_f, host->cos_f};
  tb303::voice_init(*static_cast<Voice*>(raw), sv, host->sample_rate);
  return 0;
}

void set_param(void* raw, uint32_t id, float value) { tb303::voice_set_param(*static_cast<Voice*>(raw), id, value); }

void note_on(void* raw, uint32_t note, uint32_t velocity, uint32_t flags) {
  tb303::voice_note_on(*static_cast<Voice*>(raw), note, velocity, (flags & O3M_NOTE_LEGATO) != 0u, (flags & O3M_NOTE_RAMP) != 0u);
}

// ABI 1.8: the Slide and Glide FX of a pattern (module.json "pitch"): the host ramps, the frequency follows.
void pitch(void* raw, float semitones, float per_frame) { tb303::voice_pitch(*static_cast<Voice*>(raw), semitones, per_frame); }

void note_off(void* raw, uint32_t note) {
  (void)note;
  tb303::voice_note_off(*static_cast<Voice*>(raw));
}

void shutdown(void*) {}

O3M_HOT uint32_t render(void* raw, const float* in, float* __restrict out, uint32_t frames) {
  (void)in;
  return tb303::voice_render(*static_cast<Voice*>(raw), out, frames);
}

// ---- the views (ABI 1.1, OS-012b): the filter's response and the cutoff's envelope, drawn from the parameter values the page hands over (canvas->params) with the OS's
// draw API (canvas->draw); nothing here reads the voice. They are illustrations of the voice's own mapping (tb303_dsp.h env_mod_mapping: the same constants and
// the same octaves-above-the-cutoff law), with a 4-pole ladder's response for the shape: the real filter adds its soft clip, its feedback high-pass and the fit
// of the TB_303's cutoff, which a plot of a linear ladder does not claim to show. Float only, no libm: the two logarithms and the power are short polynomials
// (an error of 1e-4 is a tenth of a pixel here).
constexpr uint32_t kViewPoints = 40;  // the two views share the page's 700 display-list words: FILTER 2 x (2 x 40 + 16) + one axes 200, ENV 96 + 200 = 688

float flog2(float x) {  // x > 0: log2 by the exponent and a polynomial of the mantissa
  const uint32_t b = __builtin_bit_cast(uint32_t, x);
  const float e = static_cast<float>(static_cast<int32_t>((b >> 23) & 255u) - 127);
  const float m = __builtin_bit_cast(float, (b & 0x007FFFFFu) | 0x3F800000u);
  // the polynomial is ln(m) on [1, 2) (an error under 1e-4); the exponent and the factor make it log2
  return e + 1.442695f * (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
}

float fexp2(float x) {
  x = x < -60.0f ? -60.0f : (x > 60.0f ? 60.0f : x);
  float whole = static_cast<float>(static_cast<int32_t>(x));
  whole = whole > x ? whole - 1.0f : whole;  // the floor
  const float f = x - whole;
  const float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013333f))));
  return p * __builtin_bit_cast(float, static_cast<uint32_t>(static_cast<int32_t>(whole) + 127) << 23);
}

float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// The cutoff of the note's start and the law of its fall: Open303's measured mapping.
struct Sweep {
  float scaler;  // octaves of cutoff for the envelope's full range
  float offset;
};

Sweep sweep_of(float cutoff, float env_mod_percent) {
  const float c0 = 314.0f, c1 = 2394.0f;
  const float e = env_mod_percent * 0.01f;
  const float c = flog2(cutoff / c0) / flog2(c1 / c0);
  const float lo = 3.773996f * e + 0.736966f, hi = 4.194549f * e + 0.864345f;
  return {(1.0f - c) * lo + c * hi, 0.048293f * c + 0.294391f};
}

float peak_cutoff(float cutoff, const Sweep& s) { return clampf(cutoff * fexp2(s.scaler * (1.0f - s.offset)), 200.0f, 11000.0f); }

// Magnitude in dB of 1 / ((1 + jx)^4 + k), x = f / fc: four equal poles and the feedback k (4 is the edge of self-oscillation).
float ladder_db(float f, float fc, float k) {
  const float x = f / fc;
  const float x2 = x * x;
  const float re = 1.0f - 6.0f * x2 + x2 * x2 + k;
  const float im = 4.0f * x * (1.0f - x2);
  return -3.0103f * flog2(re * re + im * im);
}

void view_draw(void*, uint32_t view, o3m_canvas* c) {
  if (c == nullptr || c->draw == nullptr || c->params == nullptr || c->param_count < tb303::kLegato + 1u) {  // (the nine the views were written for; ids 9.. are the voice's, OS-SNDMOD S3)
    return;
  }
  const o3m_draw_api* api = c->draw;
  const float cutoff = clampf(c->params[tb303::kCutoff], 314.0f, 2394.0f);
  const float res = clampf(c->params[tb303::kResonance], 0.0f, 100.0f) * 0.01f;
  const float env_mod = clampf(c->params[tb303::kEnvMod], 0.0f, 100.0f);
  const float decay = clampf(c->params[tb303::kDecay], 200.0f, 2000.0f);
  const Sweep sw = sweep_of(cutoff, env_mod);
  float y0[kViewPoints];
  float y1[kViewPoints];
  if (view == 0u) {  // FILTER: the response at the cutoff the knob says, and at the cutoff the envelope opens it to
    const float k = 3.9f * (1.0f - fexp2(-3.0f * res * 1.442695f)) / (1.0f - 0.049787f);
    const float peak = peak_cutoff(cutoff, sw);
    const float lo = 20.0f, hi = 16000.0f;
    // The frequencies are the cutoff times 2^u, u from -5.5 to +5.5 octaves and crowded round 0: a resonant peak is a few percent wide, and a uniform grid of 48 points
    // over ten octaves steps over it. (Each curve has its own x; both are clipped to the plot's range, so the ends repeat.)
    float fx0[kViewPoints];
    float fx1[kViewPoints];
    for (uint32_t i = 0; i < kViewPoints; ++i) {
      const float t = 2.0f * static_cast<float>(i) / static_cast<float>(kViewPoints - 1) - 1.0f;  // -1 .. 1
      const float a = t < 0.0f ? -t : t;
      const float u = (a == 0.0f ? 0.0f : 5.5f * fexp2(1.6f * flog2(a))) * (t < 0.0f ? -1.0f : 1.0f);
      fx0[i] = clampf(cutoff * fexp2(u), lo, hi);
      fx1[i] = clampf(peak * fexp2(u), lo, hi);
      y0[i] = clampf(ladder_db(fx0[i], cutoff, k), -48.0f, 24.0f);
      y1[i] = clampf(ladder_db(fx1[i], peak, k), -48.0f, 24.0f);
    }
    o3m_curve peak_line;  // (o3m_curve_set: an initialiser list with zeros may become a memset, which a module cannot link)
    o3m_curve base_line;
    o3m_curve_set(&peak_line, fx1, y1, kViewPoints, O3M_AXIS_LOG_FREQ, O3M_AXIS_DB, lo, hi, -48.0f, 24.0f, 0u, O3M_COLOR_FX);  // no axes: base_line draws them
    o3m_curve_set(&base_line, fx0, y0, kViewPoints, O3M_AXIS_LOG_FREQ, O3M_AXIS_DB, lo, hi, -48.0f, 24.0f, O3M_CURVE_AXES | O3M_CURVE_FILL, O3M_COLOR_INSTR);
    api->curve(c, &peak_line);
    api->curve(c, &base_line);
  } else {  // ENV: the cutoff against the time after a note, from the envelope's decay
    const float span = 2.0f * decay;
    for (uint32_t i = 0; i < kViewPoints; ++i) {
      const float t = span * static_cast<float>(i) / static_cast<float>(kViewPoints - 1);
      const float y = fexp2(-t / decay * 1.442695f);  // the main envelope: 1 falling by e every `decay` ms
      y0[i] = clampf(cutoff * fexp2(sw.scaler * (y - sw.offset)), 200.0f, 11000.0f);
    }
    o3m_curve line;
    o3m_curve_set(&line, nullptr, y0, kViewPoints, O3M_AXIS_LINEAR, O3M_AXIS_LOG_FREQ, 0.0f, span * 0.001f, 100.0f, 12000.0f, O3M_CURVE_AXES | O3M_CURVE_FILL, O3M_COLOR_INSTR);
    api->curve(c, &line);
  }
}

const o3m_ui kUi = {O3M_UI_SIZE(TB303_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    TB303_ENTRY_HEAD, sizeof(Voice), TB303_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, TB303_ENTRY_VIEWS(&kUi), TB303_ENTRY_TAIL(pitch), TB303_ENTRY_FX,
};
