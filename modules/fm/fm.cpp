// SPDX-License-Identifier: MIT
//
// The FM module (OS-012c): the descriptor and the glue around fm_dsp.h (the voice), and the two views. Written from scratch against the SDK (module/sdk/o3module.h); the descriptor,
// the parameter table and the view ids come from module.json through fm_gen.h (tools/o3m_gen.py). One mono voice an instance.
#include <o3module.h>

#include "fm_dsp.h"
#include "fm_gen.h"

namespace {

using fm::Voice;

static_assert(sizeof(Voice) <= FM_STATE_BYTES, "module.json: state_bytes is too small for the voice");

int init(void* raw, const o3m_host* host) {
  if (host->size < sizeof(o3m_host)) {
    return 1;
  }
  fm::voice_init(*static_cast<Voice*>(raw), host);
  return 0;
}

void set_param(void* raw, uint32_t id, float value) { fm::voice_set_param(*static_cast<Voice*>(raw), id, value); }

void note_on(void* raw, uint32_t note, uint32_t velocity, uint32_t flags) {
  // A live legato key retriggers like any other (the envelopes continue from where they are). A note the HOST ramps over the one that sounds (a Glide or Slide of the pattern, OS-MODPITCH) keeps
  // the envelopes and only moves the pitch.
  const uint32_t ramp = O3M_NOTE_LEGATO | O3M_NOTE_RAMP;
  fm::voice_note_on(*static_cast<Voice*>(raw), note, velocity, (flags & ramp) == ramp);
}

// ABI 1.8: the Slide and Glide FX of a pattern (module.json "pitch").
void pitch(void* raw, float semitones, float per_frame) { fm::voice_pitch(*static_cast<Voice*>(raw), semitones, per_frame); }

void note_off(void* raw, uint32_t note) {
  (void)note;  // one voice: any note-off releases it
  fm::voice_note_off(*static_cast<Voice*>(raw));
}

void shutdown(void*) {}

O3M_HOT uint32_t render(void* raw, const float* in, float* out, uint32_t frames) {
  (void)in;
  return fm::voice_render(*static_cast<Voice*>(raw), out, frames);
}

// ---- the views (ABI 1.1), drawn from the parameter values the page hands over (canvas->params), never from the voice --------------------------------------------------
// ALGORITHM: the selected algorithm as a diagram, one box a operator (its number, its ratio, a bar for its level), a line from each modulator down to the operator it modulates, a stub
// from each carrier to the output bus, and the feedback loop of operator 4 (lit when Feedback is on): about 180 estimated display-list words. ENVELOPES: the four envelopes in time, each scaled by
// its Level / 100, in one frame with a marker at the key release: four polylines of 48 points and a frame of rectangles (no curve primitive: each one with axes costs 200 words,
// four would be the whole page's 700), about 270 words. All the views of a page share 700 words (module/view.h); the two together are about 450.

using fm::detail::clampf;

constexpr uint32_t kEnvPoints = 48;
constexpr int32_t kBoxW = 36, kBoxH = 16;

// 2^x for the plot: a polynomial of the fraction (error 1e-5), no libm (a view has no host pointer).
float fexp2(float x) {
  x = x < -60.0f ? -60.0f : (x > 60.0f ? 60.0f : x);
  float whole = static_cast<float>(static_cast<int32_t>(x));
  whole = whole > x ? whole - 1.0f : whole;
  const float f = x - whole;
  const float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013333f))));
  return p * __builtin_bit_cast(float, static_cast<uint32_t>(static_cast<int32_t>(whole) + 127) << 23);
}

// Writes v in decimal at s; returns the characters written (no NUL).
uint32_t put_uint(char* s, uint32_t v) {
  char tmp[10];
  uint32_t n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u && n < 10u);
  for (uint32_t i = 0; i < n; ++i) {
    s[i] = tmp[n - 1u - i];
  }
  return n;
}

uint32_t whole_param(const float* p, uint32_t id, uint32_t lo, uint32_t hi) {
  const float v = clampf(p[id], static_cast<float>(lo), static_cast<float>(hi));
  return static_cast<uint32_t>(v + 0.5f);
}

void draw_algorithm(o3m_canvas* c, const float* p) {
  const o3m_draw_api* api = c->draw;
  const int32_t W = c->width, H = c->height;
  const uint32_t a = whole_param(p, FM_PARAM_ALGORITHM, 1, fm::kAlgorithmCount) - 1u;
  const fm::Algorithm& alg = fm::kAlgorithms[a];
  char label[40];
  uint32_t n = 0;
  for (const char* s = "ALG "; *s != '\0'; ++s) {
    label[n++] = *s;
  }
  n += put_uint(label + n, a + 1u);
  label[n++] = ' ';
  label[n++] = ' ';
  for (const char* s = fm_labels_algorithm[a]; *s != '\0'; ++s) {
    label[n++] = *s;
  }
  label[n] = '\0';
  api->text(c, 4, 2, label, O3M_FACE_DATA, O3M_COLOR_TEXT);

  // rows: a carrier is on the bottom row, a modulator one row above the highest of the operators it modulates (a source has a higher number than its target)
  uint32_t depth[fm::kOps] = {0, 0, 0, 0};
  uint32_t rows = 1;
  for (uint32_t i = 0; i < fm::kOps; ++i) {
    for (uint32_t k = 0; k < i; ++k) {
      if (((alg.mods[k] >> i) & 1u) != 0u && depth[k] + 1u > depth[i]) {
        depth[i] = depth[k] + 1u;
      }
    }
    rows = depth[i] + 1u > rows ? depth[i] + 1u : rows;
  }
  const int32_t bottom_y = H - 6 - 6 - kBoxH;  // the bottom row; the output bus is under it
  const int32_t top_y = 20;
  const int32_t dy = rows > 1u ? (bottom_y - top_y) / static_cast<int32_t>(rows - 1u) : 0;
  int32_t cx[fm::kOps], cy[fm::kOps];  // box centres' x and top y
  const int32_t region = (W - 14 < 320 ? W - 14 : 320);
  const int32_t left = (W - region) / 2;
  for (uint32_t r = 0; r < rows; ++r) {
    uint32_t count = 0, k = 0;
    for (uint32_t i = 0; i < fm::kOps; ++i) {
      count += depth[i] == r ? 1u : 0u;
    }
    for (uint32_t i = 0; i < fm::kOps; ++i) {
      if (depth[i] == r) {
        cx[i] = left + region * static_cast<int32_t>(2u * k + 1u) / static_cast<int32_t>(2u * count);
        cy[i] = bottom_y - static_cast<int32_t>(r) * dy;
        ++k;
      }
    }
  }
  // the lines first (the boxes cover their ends): a modulator's bottom to its target's top
  for (uint32_t t = 0; t < fm::kOps; ++t) {
    for (uint32_t s = t + 1u; s < fm::kOps; ++s) {
      if (((alg.mods[t] >> s) & 1u) != 0u) {
        int16_t xy[4] = {static_cast<int16_t>(cx[s]), static_cast<int16_t>(cy[s] + kBoxH), static_cast<int16_t>(cx[t]), static_cast<int16_t>(cy[t])};
        api->polyline(c, xy, 2, O3M_COLOR_FX);
      }
    }
  }
  int32_t bus_lo = W, bus_hi = 0;
  for (uint32_t i = 0; i < fm::kOps; ++i) {
    if (((alg.carriers >> i) & 1u) != 0u) {
      int16_t xy[4] = {static_cast<int16_t>(cx[i]), static_cast<int16_t>(cy[i] + kBoxH), static_cast<int16_t>(cx[i]), static_cast<int16_t>(H - 6)};
      api->polyline(c, xy, 2, O3M_COLOR_INSTR);
      bus_lo = cx[i] < bus_lo ? cx[i] : bus_lo;
      bus_hi = cx[i] > bus_hi ? cx[i] : bus_hi;
    }
  }
  api->rect(c, bus_lo, H - 6, bus_hi - bus_lo + 1, 1, O3M_COLOR_INSTR);
  // the boxes: the operator's number, its ratio (0 is 0.5) and a bar for its level
  for (uint32_t i = 0; i < fm::kOps; ++i) {
    const uint32_t carrier = (alg.carriers >> i) & 1u;
    const uint32_t color = carrier != 0u ? O3M_COLOR_INSTR : O3M_COLOR_FX;
    const int32_t bx = cx[i] - kBoxW / 2, by = cy[i];
    api->rect(c, bx, by, kBoxW, kBoxH, O3M_COLOR_PANE);
    api->outline(c, bx, by, kBoxW, kBoxH, color);
    char txt[8];
    txt[0] = static_cast<char>('1' + i);
    txt[1] = '\0';
    api->text(c, bx + 4, by + 2, txt, O3M_FACE_DATA, color);
    const uint32_t ratio = whole_param(p, 8u * i + 0u, 0, 31);
    uint32_t m = 0;
    txt[m++] = 'x';
    if (ratio == 0u) {
      txt[m++] = '0';
      txt[m++] = '.';
      txt[m++] = '5';
    } else {
      m += put_uint(txt + m, ratio);
    }
    txt[m] = '\0';
    api->text(c, bx + 14, by + 3, txt, O3M_FACE_MICRO, O3M_COLOR_TEXT_DIM);
    const int32_t bar = (kBoxW - 6) * static_cast<int32_t>(whole_param(p, 8u * i + 2u, 0, 100)) / 100;
    api->rect(c, bx + 3, by + kBoxH - 4, bar, 2, color);
  }
  // operator 4's feedback loop on the right of its box: lit when Feedback is on
  const int32_t fx = cx[3] + kBoxW / 2, fy = cy[3];
  const int16_t loop[8] = {static_cast<int16_t>(fx), static_cast<int16_t>(fy + 4), static_cast<int16_t>(fx + 6), static_cast<int16_t>(fy + 4),
                           static_cast<int16_t>(fx + 6), static_cast<int16_t>(fy + kBoxH - 4), static_cast<int16_t>(fx), static_cast<int16_t>(fy + kBoxH - 4)};
  api->polyline(c, loop, 4, whole_param(p, FM_PARAM_FEEDBACK, 0, 7) != 0u ? O3M_COLOR_ACCENT : O3M_COLOR_EMPTY);
}

// The envelope of one operator at t ms after the key went down, the key released at t_off: the attack is linear, the decay and the release fall by 60 dB per D and R.
float envelope_at(float t, float t_off, float a, float d, float sus, float r) {
  const float am = a > fm::kMinAttackMs ? a : fm::kMinAttackMs;
  const auto held = [&](float tt) {  // up to the release
    if (tt < am) {
      return tt / am;
    }
    return sus + (1.0f - sus) * fexp2(-fm::kLog2Thousand * (tt - am) / d);
  };
  if (t <= t_off) {
    return held(t);
  }
  return held(t_off) * fexp2(-fm::kLog2Thousand * (t - t_off) / r);
}

void draw_envelopes(o3m_canvas* c, const float* p) {
  const o3m_draw_api* api = c->draw;
  const int32_t W = c->width, H = c->height;
  float a[fm::kOps], d[fm::kOps], s[fm::kOps], r[fm::kOps], lvl[fm::kOps];
  float on_ms = 0.0f, tail_ms = 0.0f;
  for (uint32_t i = 0; i < fm::kOps; ++i) {
    const uint32_t base = 8u * i;
    a[i] = clampf(p[base + 3u], 1.0f, 4000.0f);
    d[i] = clampf(p[base + 4u], 5.0f, 8000.0f);
    s[i] = clampf(p[base + 5u], 0.0f, 100.0f) * 0.01f;
    r[i] = clampf(p[base + 6u], 5.0f, 8000.0f);
    lvl[i] = clampf(p[base + 2u], 0.0f, 100.0f) * 0.01f;
    if (lvl[i] > 0.0f) {  // a silent operator does not set the scale
      on_ms = a[i] + 0.7f * d[i] > on_ms ? a[i] + 0.7f * d[i] : on_ms;
      tail_ms = 0.8f * r[i] > tail_ms ? 0.8f * r[i] : tail_ms;
    }
  }
  on_ms = clampf(on_ms, 100.0f, 2000.0f);
  tail_ms = clampf(tail_ms, 100.0f, 2000.0f);
  const float total = on_ms + tail_ms;
  const int32_t x0 = 4, y0 = 16, x1 = W - 5, y1 = H - 14;  // the frame: inclusive pixels
  if (x1 - x0 < 16 || y1 - y0 < 8) {
    return;
  }
  api->outline(c, x0, y0, x1 - x0 + 1, y1 - y0 + 1, O3M_COLOR_EMPTY);
  const int32_t x_off = x0 + static_cast<int32_t>(static_cast<float>(x1 - x0) * on_ms / total);
  api->rect(c, x_off, y0, 1, y1 - y0 + 1, O3M_COLOR_TEXT_DIM);  // the key goes up here
  static const uint32_t kColors[fm::kOps] = {O3M_COLOR_INSTR, O3M_COLOR_FX, O3M_COLOR_VOL, O3M_COLOR_ACCENT};
  for (uint32_t i = 0; i < fm::kOps; ++i) {
    int16_t xy[2u * kEnvPoints];
    for (uint32_t k = 0; k < kEnvPoints; ++k) {
      const float t = total * static_cast<float>(k) / static_cast<float>(kEnvPoints - 1u);
      const float y = envelope_at(t, on_ms, a[i], d[i], s[i], r[i]) * lvl[i];
      xy[2u * k] = static_cast<int16_t>(x0 + static_cast<int32_t>(static_cast<float>(x1 - x0) * static_cast<float>(k) / static_cast<float>(kEnvPoints - 1u)));
      xy[2u * k + 1u] = static_cast<int16_t>(y1 - static_cast<int32_t>(clampf(y, 0.0f, 1.0f) * static_cast<float>(y1 - y0)));
    }
    api->polyline(c, xy, kEnvPoints, kColors[i]);
    char txt[2] = {static_cast<char>('1' + i), '\0'};
    api->text(c, 4 + 14 * static_cast<int32_t>(i), 2, txt, O3M_FACE_DATA, kColors[i]);
  }
  char label[16];
  uint32_t n = put_uint(label, static_cast<uint32_t>(total + 0.5f));
  label[n++] = ' ';
  label[n++] = 'm';
  label[n++] = 's';
  label[n] = '\0';
  api->text(c, x1 - 6 * static_cast<int32_t>(n) + 1, y1 + 3, label, O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
  api->text(c, x0, y1 + 3, "0", O3M_FACE_LABEL, O3M_COLOR_TEXT_DIM);
}

void view_draw(void*, uint32_t view, o3m_canvas* c) {
  if (c == nullptr || c->draw == nullptr || c->params == nullptr || c->param_count < FM_PARAM_COUNT) {
    return;
  }
  if (view == FM_VIEW_ALGORITHM) {
    draw_algorithm(c, c->params);
  } else if (view == FM_VIEW_ENVELOPES) {
    draw_envelopes(c, c->params);
  }
}

const o3m_ui kUi = {O3M_UI_SIZE(FM_ABI_MINOR), nullptr, nullptr, view_draw, nullptr, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    FM_ENTRY_HEAD, sizeof(Voice), FM_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, FM_ENTRY_VIEWS(&kUi), FM_ENTRY_TAIL(pitch), FM_ENTRY_FX,
};
