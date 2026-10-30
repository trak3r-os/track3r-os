// SPDX-License-Identifier: MIT
//
// The Chip module (OS-012c): the descriptor and the glue around chip_dsp.h (the voice), and the two views. Written from scratch against the SDK (module/sdk/o3module.h); the
// descriptor, the parameter table and the view ids come from module.json through chip_gen.h (tools/o3m_gen.py). One mono voice an instance.
#include <o3module.h>

#include "chip_dsp.h"
#include "chip_gen.h"

namespace {

using chip::Voice;

static_assert(sizeof(Voice) <= CHIP_STATE_BYTES, "module.json: state_bytes is too small for the voice");

int init(void* raw, const o3m_host* host) {
  if (host->size < sizeof(o3m_host)) {
    return 1;
  }
  chip::voice_init(*static_cast<Voice*>(raw), host);
  return 0;
}

void set_param(void* raw, uint32_t id, float value) { chip::voice_set_param(*static_cast<Voice*>(raw), id, value); }

void note_on(void* raw, uint32_t note, uint32_t velocity, uint32_t flags) {
  // A live legato key retriggers like any other: the voice is a chip's, not a slide synth's. A note the HOST ramps over the one that sounds (a Glide or Slide of the pattern, OS-MODPITCH) keeps
  // the envelope and only moves the pitch.
  const uint32_t ramp = O3M_NOTE_LEGATO | O3M_NOTE_RAMP;
  chip::voice_note_on(*static_cast<Voice*>(raw), note, velocity, (flags & ramp) == ramp);
}

// ABI 1.8: the Slide and Glide FX of a pattern (module.json "pitch").
void pitch(void* raw, float semitones, float per_frame) { chip::voice_pitch(*static_cast<Voice*>(raw), semitones, per_frame); }

void note_off(void* raw, uint32_t note) {
  (void)note;  // one voice: any note-off releases it
  chip::voice_note_off(*static_cast<Voice*>(raw));
}

void shutdown(void*) {}

O3M_HOT uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  (void)in;
  return chip::voice_render(*static_cast<Voice*>(raw), out, frames);
}

// ---- the views (ABI 1.1), drawn from the parameter values the page hands over (canvas->params), never from the voice --------------------------------------------------
// WAVE: two periods of the pulse (with its duty), the triangle, or 48 steps of the LFSR noise, and its name. ENVELOPE: the level against the time for the note held a while
// and released (in 4-bit steps when Vol Steps is on). Budget: the wave is one polyline (at most 97 points) and two texts; the envelope is one curve with axes (64 points).
// All views of a page share 700 estimated display-list words (module/view.h): this module's two cost about 450 together.

using chip::detail::clampf;

constexpr uint32_t kEnvPoints = 64;
constexpr uint32_t kNoiseSteps = 48;

const char* wave_label(uint32_t wave, uint32_t duty) {
  static const char* const kPulse[4] = {"PULSE 12.5%", "PULSE 25%", "PULSE 50%", "PULSE 75%"};
  switch (wave) {
    case chip::kPulse: return kPulse[duty & 3u];
    case chip::kTriangle: return "TRIANGLE";
    case chip::kNoiseLong: return "NOISE LONG";
    default: return "NOISE SHORT";
  }
}

void draw_wave(o3m_canvas* c, const float* p) {
  const o3m_draw_api* api = c->draw;
  const uint32_t wave = static_cast<uint32_t>(clampf(p[CHIP_PARAM_WAVE], 0.0f, 3.0f) + 0.5f);
  const uint32_t duty = static_cast<uint32_t>(clampf(p[CHIP_PARAM_DUTY], 0.0f, 3.0f) + 0.5f);
  // The name takes the rows 3..22 (the Data face is 20 pixels high); the trace starts below it, so that the first edge never runs through the letters (OS-BUG-CHIPWAVELABEL; the Wave view's min height is 64, so the trace keeps 32 rows).
  const int32_t left = 6, right = c->width - 6, top = 26, bottom = c->height - 6;
  if (right - left < 16 || bottom - top < 8) {
    return;
  }
  const int32_t mid = (top + bottom) / 2;
  api->rect(c, left, mid, right - left, 1, O3M_COLOR_EMPTY);
  api->text(c, left, 3, wave_label(wave, duty), O3M_FACE_DATA, O3M_COLOR_TEXT);
  int16_t xy[2u * (2u * kNoiseSteps + 1u)];
  uint32_t n = 0;
  const auto pt = [&](int32_t x, int32_t y) {
    xy[n++] = static_cast<int16_t>(x);
    xy[n++] = static_cast<int16_t>(y);
  };
  if (wave == chip::kPulse) {
    const int32_t pw = (right - left) / 2;  // two periods
    const int32_t dw = static_cast<int32_t>(static_cast<float>(pw) * (duty == 3u ? 0.75f : 0.125f * static_cast<float>(1u << duty)));
    pt(left, bottom);
    for (int32_t k = 0; k < 2; ++k) {
      const int32_t x0 = left + k * pw;
      pt(x0, top);
      pt(x0 + dw, top);
      pt(x0 + dw, bottom);
      if (k == 0) {
        pt(x0 + pw, bottom);
      }
    }
    pt(right, bottom);
  } else if (wave == chip::kTriangle) {
    const int32_t pw = (right - left) / 2;
    pt(left, top);
    pt(left + pw / 2, bottom);
    pt(left + pw, top);
    pt(left + pw + pw / 2, bottom);
    pt(right, top);
  } else {  // the LFSR's steps after a pre-roll (the seed starts with a flat run), each held for one cell
    uint32_t r = 1;
    for (uint32_t k = 0; k < chip::kNoisePreroll; ++k) {
      r = chip::lfsr_step(r, wave == chip::kNoiseShort);
    }
    const int32_t span = right - left;
    for (uint32_t k = 0; k < kNoiseSteps; ++k) {
      r = chip::lfsr_step(r, wave == chip::kNoiseShort);
      const int32_t y = (r & 1u) != 0u ? bottom : top;
      pt(left + span * static_cast<int32_t>(k) / static_cast<int32_t>(kNoiseSteps), y);
      pt(left + span * static_cast<int32_t>(k + 1u) / static_cast<int32_t>(kNoiseSteps), y);
    }
  }
  api->polyline(c, xy, n / 2u, O3M_COLOR_INSTR);
}

void draw_envelope(o3m_canvas* c, const float* p) {
  const float a = clampf(p[CHIP_PARAM_ATTACK], 0.0f, 500.0f);
  const float d = clampf(p[CHIP_PARAM_DECAY], 0.0f, 2000.0f);
  const float sus = clampf(p[CHIP_PARAM_SUSTAIN], 0.0f, 100.0f) * 0.01f;
  const float r = clampf(p[CHIP_PARAM_RELEASE], 0.0f, 2000.0f);
  const bool steps = p[CHIP_PARAM_VOL_STEPS] >= 0.5f;
  // the attack and release the voice really has (it never goes below 1 ms), the sustain held for a quarter of the rest, at least 50 ms
  const float am = a > chip::kMinRampMs ? a : chip::kMinRampMs;
  const float rm = r > chip::kMinRampMs ? r : chip::kMinRampMs;
  float hold = 0.25f * (am + d + rm);
  hold = hold < 50.0f ? 50.0f : (hold > 600.0f ? 600.0f : hold);
  const float t_off = am + d + hold;  // the note ends here (ms)
  const float total = t_off + rm;
  float y[kEnvPoints];
  for (uint32_t i = 0; i < kEnvPoints; ++i) {
    const float t = total * static_cast<float>(i) / static_cast<float>(kEnvPoints - 1u);
    float level;
    if (t < am) {
      level = t / am;
    } else if (t < am + d) {
      level = 1.0f - (1.0f - sus) * (t - am) / d;  // (d > 0 here)
    } else if (t < t_off) {
      level = sus;
    } else {
      level = sus * (1.0f - (t - t_off) / rm);
    }
    y[i] = steps ? static_cast<float>(static_cast<int32_t>(level * 15.0f + 0.5f)) * (1.0f / 15.0f) : level;
  }
  o3m_curve curve;
  o3m_curve_set(&curve, nullptr, y, kEnvPoints, O3M_AXIS_LINEAR, O3M_AXIS_LINEAR, 0.0f, total * 0.001f, 0.0f, 1.0f, O3M_CURVE_AXES | O3M_CURVE_FILL, O3M_COLOR_VOL);
  c->draw->curve(c, &curve);
}

void view_draw(void*, uint32_t view, o3m_canvas* c) {
  if (c == nullptr || c->draw == nullptr || c->params == nullptr || c->param_count < CHIP_PARAM_COUNT) {
    return;
  }
  if (view == CHIP_VIEW_WAVE) {
    draw_wave(c, c->params);
  } else if (view == CHIP_VIEW_ENVELOPE) {
    draw_envelope(c, c->params);
  }
}

const o3m_ui kUi = {O3M_UI_SIZE(CHIP_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    CHIP_ENTRY_HEAD, sizeof(Voice), CHIP_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, CHIP_ENTRY_VIEWS(&kUi), CHIP_ENTRY_TAIL(pitch),
};
