// SPDX-License-Identifier: MIT
//
// The SID module (OS-SID): the descriptor and the glue around sid_dsp.h (the voice), and the two views. Written from scratch against the SDK (module/sdk/o3module.h); the descriptor, the
// parameter table and the view ids come from module.json through sid_gen.h (tools/o3m_gen.py). One mono voice an instance.
#include <o3module.h>

#include "sid_dsp.h"
#include "sid_gen.h"

namespace {

using sid::Voice;

static_assert(sizeof(Voice) <= SID_STATE_BYTES, "module.json: state_bytes is too small for the voice");

int init(void* raw, const o3m_host* host) {
  if (host->size < sizeof(o3m_host)) {
    return 1;
  }
  sid::voice_init(*static_cast<Voice*>(raw), host);
  return 0;
}

void set_param(void* raw, uint32_t id, float value) { sid::voice_set_param(*static_cast<Voice*>(raw), id, value); }

void note_on(void* raw, uint32_t note, uint32_t velocity, uint32_t flags) {
  // A live legato key retriggers like any other: the voice is a chip's, not a slide synth's. A note the HOST ramps over the one that sounds (a Glide or Slide of the pattern, OS-MODPITCH) keeps
  // the envelope and only moves the pitch.
  const uint32_t ramp = O3M_NOTE_LEGATO | O3M_NOTE_RAMP;
  sid::voice_note_on(*static_cast<Voice*>(raw), note, velocity, (flags & ramp) == ramp);
}

// ABI 1.8: the Slide and Glide FX of a pattern (module.json "pitch").
void pitch(void* raw, float semitones, float per_frame) { sid::voice_pitch(*static_cast<Voice*>(raw), semitones, per_frame); }

void note_off(void* raw, uint32_t note) {
  (void)note;  // one voice: any note-off releases it
  sid::voice_note_off(*static_cast<Voice*>(raw));
}

void shutdown(void*) {}

O3M_HOT uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  (void)in;
  return sid::voice_render(*static_cast<Voice*>(raw), out, frames);
}

// ---- the views (ABI 1.1), drawn from the parameter values the page hands over (canvas->params), never from the voice --------------------------------------------------
// WAVE: two periods of the chosen wave (the pulse with its width, the combined waves from the module's table, 48 steps of the noise register) and its name. ENVELOPE: the level against the time
// for the note held a while and released, with the times of the C64 table the A, D and R values mean. Budget: the wave is one polyline (at most 97 points) and one text; the envelope is one
// curve with axes (64 points). All views of a page share 700 estimated display-list words (module/view.h).

using sid::detail::clampf;

constexpr uint32_t kEnvPoints = 64;
constexpr uint32_t kNoiseSteps = 48;
constexpr uint32_t kCombPoints = 97;

uint32_t whole(float x, float hi) { return static_cast<uint32_t>(clampf(x, 0.0f, hi) + 0.5f); }

void draw_wave(o3m_canvas* c, const float* p) {
  const o3m_draw_api* api = c->draw;
  const uint32_t wave = whole(p[SID_PARAM_WAVE], 7.0f);
  const uint32_t model = whole(p[SID_PARAM_MODEL], 1.0f);
  const float pw = clampf(p[SID_PARAM_PULSE_WIDTH], 1.0f, 99.0f) * 0.01f;
  const int32_t left = 6, right = c->width - 6, top = 16, bottom = c->height - 6;
  if (right - left < 16 || bottom - top < 8) {
    return;
  }
  const int32_t mid = (top + bottom) / 2;
  api->rect(c, left, mid, right - left, 1, O3M_COLOR_EMPTY);
  api->text(c, left, 3, sid_labels_wave[wave], O3M_FACE_DATA, O3M_COLOR_TEXT);
  int16_t xy[2u * (2u * kNoiseSteps + 1u)];
  uint32_t n = 0;
  const auto pt = [&](int32_t x, int32_t y) {
    xy[n++] = static_cast<int16_t>(x);
    xy[n++] = static_cast<int16_t>(y);
  };
  const int32_t span = right - left;
  const int32_t period = span / 2;  // two periods
  if (wave == sid::kSaw) {
    pt(left, bottom);
    pt(left + period, top);
    pt(left + period, bottom);
    pt(right, top);
  } else if (wave == sid::kTriangle) {
    pt(left, top);
    pt(left + period / 2, bottom);
    pt(left + period, top);
    pt(left + period + period / 2, bottom);
    pt(right, top);
  } else if (wave == sid::kPulse) {
    const int32_t dw = static_cast<int32_t>(static_cast<float>(period) * pw);
    pt(left, bottom);
    for (int32_t k = 0; k < 2; ++k) {
      const int32_t x0 = left + k * period;
      pt(x0, top);
      pt(x0 + dw, top);
      pt(x0 + dw, bottom);
      if (k == 0) {
        pt(x0 + period, bottom);
      }
    }
    pt(right, bottom);
  } else if (wave == sid::kNoise) {  // the register's steps from the seed, each held for one cell, the 8-bit output as the height
    uint32_t r = sid::kLfsrSeed;
    for (uint32_t k = 0; k < kNoiseSteps; ++k) {
      r = sid::lfsr_step(r);
      const int32_t y = bottom - static_cast<int32_t>(sid::noise_byte(r)) * (bottom - top) / 255;
      pt(left + span * static_cast<int32_t>(k) / static_cast<int32_t>(kNoiseSteps), y);
      pt(left + span * static_cast<int32_t>(k + 1u) / static_cast<int32_t>(kNoiseSteps), y);
    }
  } else {  // a combined wave: the table's value at each point, gated by the pulse in all but SAW+TRI
    const uint8_t* tab = sid::kTables.t[model * 4u + (wave - sid::kSawTri)];
    for (uint32_t k = 0; k < kCombPoints; ++k) {
      const float ph2 = 2.0f * static_cast<float>(k) / static_cast<float>(kCombPoints - 1u);  // 0..2
      const float ph = ph2 >= 1.0f ? ph2 - 1.0f : ph2;
      const uint32_t idx = static_cast<uint32_t>(ph * static_cast<float>(sid::kTableN - 1u) + 0.5f);
      const bool gated = wave != sid::kSawTri && ph >= pw;
      const int32_t h = gated ? 0 : static_cast<int32_t>(tab[idx]);
      pt(left + span * static_cast<int32_t>(k) / static_cast<int32_t>(kCombPoints - 1u), bottom - h * (bottom - top) / 255);
    }
  }
  api->polyline(c, xy, n / 2u, O3M_COLOR_INSTR);
}

void draw_envelope(o3m_canvas* c, const float* p) {
  const uint32_t a = whole(p[SID_PARAM_ATTACK], 15.0f);
  const uint32_t d = whole(p[SID_PARAM_DECAY], 15.0f);
  const uint32_t s = whole(p[SID_PARAM_SUSTAIN], 15.0f);
  const uint32_t r = whole(p[SID_PARAM_RELEASE], 15.0f);
  const float sus = static_cast<float>(s) * (1.0f / 15.0f);  // the level as a fraction (the counter's byte is s x 0x11 of 255)
  // the C64 table's times: the decay and the release are the table's time for a full-scale fall, so a fall to the sustain level (or from it to 0) takes its share of that
  const float ta = static_cast<float>(sid::kAttackMs[a]);
  const float td = static_cast<float>(sid::kDecayMs[d]) * (1.0f - sus);
  const float tr = static_cast<float>(sid::kDecayMs[r]) * sus;
  float hold = 0.25f * (ta + td + tr);
  hold = hold < 50.0f ? 50.0f : (hold > 1000.0f ? 1000.0f : hold);
  const float t_off = ta + td + hold;  // the note ends here (ms)
  const float total = t_off + (tr > 1.0f ? tr : 1.0f);
  float y[kEnvPoints];
  for (uint32_t i = 0; i < kEnvPoints; ++i) {
    const float t = total * static_cast<float>(i) / static_cast<float>(kEnvPoints - 1u);
    float level;
    if (t < ta) {
      level = t / ta;
    } else if (t < ta + td) {
      level = 1.0f - (1.0f - sus) * (t - ta) / td;  // (td > 0 here)
    } else if (t < t_off) {
      level = sus;
    } else {
      level = sus * (1.0f - (t - t_off) / (total - t_off));
    }
    y[i] = clampf(level, 0.0f, 1.0f);
  }
  o3m_curve curve;
  o3m_curve_set(&curve, nullptr, y, kEnvPoints, O3M_AXIS_LINEAR, O3M_AXIS_LINEAR, 0.0f, total * 0.001f, 0.0f, 1.0f, O3M_CURVE_AXES | O3M_CURVE_FILL, O3M_COLOR_VOL);
  c->draw->curve(c, &curve);
}

// 2^x and log2(x) for the filter view (a canvas carries no host table): the Taylor series of 2^f on 0..1 (error below 2e-6), and log2 by the exponent and the series of atanh on the mantissa (error below 3e-6).
float fexp2(float x) {
  x = clampf(x, -60.0f, 60.0f);
  float whole = static_cast<float>(static_cast<int32_t>(x));
  whole = whole > x ? whole - 1.0f : whole;
  const float f = x - whole;
  const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * (0.00133336f + f * 0.00015403f)))));
  return p * __builtin_bit_cast(float, static_cast<uint32_t>(static_cast<int32_t>(whole) + 127) << 23);
}

float flog2(float x) {  // x > 0
  const uint32_t b = __builtin_bit_cast(uint32_t, x);
  const float m = __builtin_bit_cast(float, (b & 0x007FFFFFu) | 0x3F800000u);  // 1 .. 2
  const float t = (m - 1.0f) / (m + 1.0f);
  const float t2 = t * t;
  return static_cast<float>(static_cast<int32_t>(b >> 23) - 127) + 2.8853901f * t * (1.0f + t2 * (0.33333333f + t2 * (0.2f + t2 * 0.14285714f)));
}

// FILTER: the magnitude of the mix of low, band and high pass at the cutoff the knob says (the key's and the envelope's shifts are not drawn), 20 Hz to 20 kHz, -48 to +24 dB, 97 points
// crowded round the cutoff (a resonant peak is a few percent wide). The page's three views share 700 estimated display-list words (wave 122, envelope 339, this one 217: 678), so the curve has
// no axes (they cost 200 words) and only 97 points. With w = f / fc and the damping k the three responses share the denominator (1 - w^2) + j k w; the numerator is
// lp - hp w^2 + j bp k w (the band pass is weighted by k): the analogue prototype of the filter, which the filter's own prewarping makes exact at the cutoff. Mode OFF is a flat line.
constexpr uint32_t kFilterPoints = 97;  // odd: the middle point is exactly the cutoff

void draw_filter(o3m_canvas* c, const float* p) {
  const uint32_t mode = whole(p[SID_PARAM_MODE], 7.0f);
  const uint32_t model = whole(p[SID_PARAM_MODEL], 1.0f);
  const float cut = clampf(p[SID_PARAM_CUTOFF], 0.0f, 100.0f) * 0.01f;
  const float res = clampf(p[SID_PARAM_RESONANCE], 0.0f, 15.0f);
  const float u = model == 0u ? cut * cut * (3.0f - 2.0f * cut) : cut;
  const float fc = fexp2((model == 0u ? 7.2288187f : 4.9068906f) + u * (model == 0u ? 6.3219281f : 8.6438562f));
  const float k = 1.41421356f * fexp2(-res * 0.28814f);
  const uint32_t bits = sid::kModeBits[mode];
  const float lp = static_cast<float>(bits & 1u), bp = static_cast<float>((bits >> 1) & 1u), hp = static_cast<float>((bits >> 2) & 1u);
  const float lo = 20.0f, hi = 20000.0f;
  float fx[kFilterPoints];
  float db[kFilterPoints];
  for (uint32_t i = 0; i < kFilterPoints; ++i) {
    const float t = 2.0f * static_cast<float>(i) / static_cast<float>(kFilterPoints - 1u) - 1.0f;  // -1 .. 1
    const float f = clampf(fc * fexp2(5.5f * t * (t < 0.0f ? -t : t)), lo, hi);
    fx[i] = f;
    if (mode == 0u) {
      db[i] = 0.0f;
      continue;
    }
    const float w = f / fc;
    const float w2 = w * w;
    const float re = lp - hp * w2, im = bp * k * w;
    const float den = (1.0f - w2) * (1.0f - w2) + k * k * w2;
    const float m2 = (re * re + im * im) / den;
    db[i] = clampf(3.0103f * flog2(m2 > 1e-12f ? m2 : 1e-12f), -48.0f, 24.0f);
  }
  o3m_curve curve;
  o3m_curve_set(&curve, fx, db, kFilterPoints, O3M_AXIS_LOG_FREQ, O3M_AXIS_DB, lo, hi, -48.0f, 24.0f, O3M_CURVE_FILL, O3M_COLOR_INSTR);
  c->draw->curve(c, &curve);
  c->draw->text(c, 6, 3, sid_labels_mode[mode], O3M_FACE_DATA, O3M_COLOR_TEXT);
}

void view_draw(void*, uint32_t view, o3m_canvas* c) {
  if (c == nullptr || c->draw == nullptr || c->params == nullptr || c->param_count < SID_PARAM_COUNT) {
    return;
  }
  if (view == SID_VIEW_WAVE) {
    draw_wave(c, c->params);
  } else if (view == SID_VIEW_ENVELOPE) {
    draw_envelope(c, c->params);
  } else if (view == SID_VIEW_FILTER) {
    draw_filter(c, c->params);
  }
}

const o3m_ui kUi = {O3M_UI_SIZE(SID_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    SID_ENTRY_HEAD, sizeof(Voice), SID_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, SID_ENTRY_VIEWS(&kUi), SID_ENTRY_TAIL(pitch),
};
