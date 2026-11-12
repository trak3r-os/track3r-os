// SPDX-License-Identifier: MIT
//
// A track3r-os plugin of its own: not taken or rebuilt from any other source.
// Copyright (c) 2026 the track3r-os contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
// to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of
// the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//
// WT Forge: a wavetable generator for the .ptfx Sample Editor plugin format (GENERATOR + WAVETABLE).
// It writes FRAMES windows of 2,048 samples, one cycle each. Every window is built as a spectrum (sine and cosine
// part of each harmonic up to HARM), so it is band-limited by construction, has no DC, and all windows share one
// phase reference: stepping the wavetable position never jumps in phase.
//
// Nine modes decide how the timbre moves from the first window to the last; START, END and CURVE pick the part of
// that path the table covers. The three time-domain modes (FM, Fold, Sync) are computed as a raw cycle, analysed
// into the same spectrum and rebuilt from it, so HARM and TILT act on every mode.
//
// PTFX_NAME: WT Forge
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: GENERATOR, WAVETABLE
// PTFX_SCRATCH: 32768

#include "tracker_plugin.h"

#define WIN        2048u
#define SIN_N      4096u
#define SIN_MASK   4095u
#define QUARTER    1024u
#define MAX_H      128u
#define MAX_FRAMES 43u   // 43 x 2048 = 88,064 frames, the most a generator run holds (88,200)
#define TWO_PI_F   6.28318530718f
#define NEED_BYTES ((SIN_N + 4u + WIN) * 4u)

const PluginParam PLUGIN_PARAMS[9] = {
    PTFX_PARAM_LIST ("Mode",   0, "Classic|PWM|FM|Fold|Sync|Formant|Overtone|Spectral|Life"),
    PTFX_PARAM_INT  ("Start",  0.0f, 100.0f, 0.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("End",    0.0f, 100.0f, 100.0f, 1.0f, "%"),
    PTFX_PARAM_LIST ("Curve",  0, "Linear|Ease In|Ease Out|S-Curve|There+Back|4 Steps|8 Steps"),
    PTFX_PARAM_INT  ("Color",  0.0f, 100.0f, 50.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Harm",   4.0f, 128.0f, 48.0f, 1.0f, ""),
    PTFX_PARAM_INT  ("Tilt",   -100.0f, 100.0f, 0.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Frames", 2.0f, 43.0f, 32.0f, 1.0f, ""),
    PTFX_PARAM_INT  ("Seed",   0.0f, 999.0f, 1.0f, 1.0f, "")
};

static const PtfxHostApi* s_host = 0;
static float* s_sin = 0;   // SIN_N + 1 entries: one turn of a sine, and the first entry again
static float* s_buf = 0;   // one window
static float  s_S[MAX_H + 1u];     // sine part of harmonic h
static float  s_C[MAX_H + 1u];     // cosine part of harmonic h
static float  s_tilt[MAX_H + 1u];  // gain of harmonic h from TILT
static uint8_t s_cell[160];        // the Life mode's two rows of cells

static uint8_t  s_ok = 0, s_ready = 0;
static uint32_t s_mode = 0, s_curve = 0, s_harm = 48, s_frames = 32, s_seed = 1;
static float    s_start = 0.0f, s_end = 1.0f, s_color = 0.5f, s_tiltp = 0.0f;
static uint32_t s_win = 0, s_rd = WIN;

static float absf(float x) { return x < 0.0f ? -x : x; }

static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t x = (a + 1u) * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return x;
}

static float u01(uint32_t x) { return (float)(x >> 8) * (1.0f / 16777216.0f); }

// The sine of an angle given in turns (any sign), from the table with linear interpolation.
static float sin_turns(float t) {
    float x = t * (float)SIN_N;
    int32_t i = (int32_t)x;
    if ((float)i > x) i--;
    float fr = x - (float)i;
    uint32_t k = (uint32_t)i & SIN_MASK;
    float a = s_sin[k];
    return a + fr * (s_sin[k + 1u] - a);
}

static float bump(float x) {  // a bell: 1 at 0, falling to 0 both ways
    float d = 1.0f + x * x;
    return 1.0f / (d * d);
}

static void prepare(void) {
    for (uint32_t i = 0; i <= SIN_N; i++) s_sin[i] = s_host->sinf((float)i * (TWO_PI_F / (float)SIN_N));
    s_sin[SIN_N] = s_sin[0];
    for (uint32_t h = 1; h <= MAX_H; h++) s_tilt[h] = s_host->powf((float)h, s_tiltp);
    s_ready = 1;
}

// Where window w sits on the mode's path, 0..1, after CURVE, START and END.
static float path_pos(uint32_t w) {
    float t;
    if (s_curve == 4u) {
        t = (float)w / (float)s_frames;
        t = 1.0f - absf(2.0f * t - 1.0f);
    } else {
        t = s_frames > 1u ? (float)w / (float)(s_frames - 1u) : 0.0f;
    }
    if (s_curve == 1u) t = t * t;
    else if (s_curve == 2u) t = 1.0f - (1.0f - t) * (1.0f - t);
    else if (s_curve == 3u) t = t * t * (3.0f - 2.0f * t);
    else if (s_curve == 5u || s_curve == 6u) {
        uint32_t steps = s_curve == 5u ? 4u : 8u;
        uint32_t q = (uint32_t)(t * (float)steps);
        if (q >= steps) q = steps - 1u;
        t = (float)q / (float)(steps - 1u);
    }
    float s = s_start + (s_end - s_start) * t;
    return s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
}

static float shape_amp(uint32_t shape, uint32_t h) {
    float fh = (float)h;
    if (shape == 0u) return h == 1u ? 1.0f : 0.0f;                                   // sine
    if (shape == 1u) return (h & 1u) ? ((((h >> 1) & 1u) ? -0.8106f : 0.8106f) / (fh * fh)) : 0.0f;  // triangle
    if (shape == 2u) return ((h & 1u) ? 0.6366f : -0.6366f) / fh;                    // saw
    return (h & 1u) ? 1.2732f / fh : 0.0f;                                           // square
}

static void even_gain(uint32_t H) {  // COLOR on the additive shapes: the level of the even harmonics, 50 % as they are
    float g = s_color * 2.0f;
    for (uint32_t h = 2; h <= H; h += 2u) { s_S[h] *= g; s_C[h] *= g; }
}

static void fill_classic(float s, uint32_t H) {  // sine > triangle > saw > square
    float x = s * 3.0f;
    uint32_t seg = (uint32_t)x;
    if (seg > 2u) seg = 2u;
    float f = x - (float)seg;
    for (uint32_t h = 1; h <= H; h++) s_S[h] = (1.0f - f) * shape_amp(seg, h) + f * shape_amp(seg + 1u, h);
    even_gain(H);
}

static void fill_pwm(float s, uint32_t H) {  // a pulse centred in the window, from 50 % wide to 3 %
    float d = 0.5f - 0.47f * s;
    for (uint32_t h = 1; h <= H; h++) {
        float a = 0.6366f * sin_turns((float)h * d * 0.5f) / (float)h;
        s_C[h] = (h & 1u) ? -a : a;
    }
    even_gain(H);
}

static void raw_fm(float s) {  // a sine modulated in phase by a sine at COLOR's whole ratio (1..8), index 0..8
    uint32_t r = 1u + (uint32_t)(s_color * 7.0f + 0.5f);
    float idx = s * 1.2732f;  // 8 radians, in turns
    for (uint32_t n = 0; n < WIN; n++) {
        float m = s_sin[(2u * r * n) & SIN_MASK];
        s_buf[n] = sin_turns((float)n * (1.0f / (float)WIN) + idx * m);
    }
}

static void raw_fold(float s) {  // a sine folded by a sine shaper; COLOR is the bias (even harmonics)
    float g = 0.25f + s * 2.0f;
    float b = s_color * 0.25f;
    for (uint32_t n = 0; n < WIN; n++) s_buf[n] = sin_turns(g * s_sin[(2u * n) & SIN_MASK] + b);
}

static void raw_sync(float s) {  // a sine at 1..16 times the pitch under a window; COLOR narrows the window
    float r = 1.0f + s * 15.0f;
    uint32_t p = 1u + (uint32_t)(s_color * 2.99f);
    for (uint32_t n = 0; n < WIN; n++) {
        float w = 0.5f - 0.5f * s_sin[(2u * n + QUARTER) & SIN_MASK];
        float g = w;
        if (p > 1u) g *= w;
        if (p > 2u) g *= w;
        s_buf[n] = g * sin_turns(r * (float)n * (1.0f / (float)WIN));
    }
}

static void fill_formant(float s, uint32_t H) {  // the vowels A E I O U; COLOR is the size of the voice
    static const float kVow[5][3] = {
        { 730.0f, 1090.0f, 2440.0f }, { 530.0f, 1840.0f, 2480.0f }, { 270.0f, 2290.0f, 3010.0f },
        { 570.0f, 840.0f, 2410.0f },  { 300.0f, 870.0f, 2240.0f } };
    static const float kBw[3] = { 70.0f, 100.0f, 160.0f };
    static const float kGain[3] = { 1.0f, 0.7f, 0.35f };
    float x = s * 4.0f;
    uint32_t seg = (uint32_t)x;
    if (seg > 3u) seg = 3u;
    float f = x - (float)seg;
    float shift = 0.6f + 0.8f * s_color;
    float fm[3];
    for (uint32_t j = 0; j < 3u; j++) fm[j] = shift * ((1.0f - f) * kVow[seg][j] + f * kVow[seg + 1u][j]);
    for (uint32_t h = 1; h <= H; h++) {
        float hz = 110.0f * (float)h;
        float g = 0.03f;
        for (uint32_t j = 0; j < 3u; j++) {
            float d = (hz - fm[j]) / kBw[j];
            g += kGain[j] / (1.0f + d * d);
        }
        s_S[h] = g / (float)h;
    }
}

static void fill_overtone(float s, uint32_t H) {  // a peak that climbs the harmonics; COLOR is how sharp it is
    uint32_t top = H < 32u ? H : 32u;
    float c = 1.0f + s * (float)(top - 1u);
    float q = 2.0f + s_color * 30.0f;
    for (uint32_t h = 1; h <= H; h++) s_S[h] = (1.0f + q * bump(((float)h - c) * 1.1f)) / (float)h;
}

static float spectral_amp(uint32_t key, uint32_t h, float dens) {
    if (h == 1u) return 1.0f;
    if (u01(hash3(s_seed, key, h)) >= dens) return 0.0f;
    return u01(hash3(s_seed + 77u, key, h)) / s_host->sqrtf((float)h);
}

static void fill_spectral(float s, uint32_t H) {  // five random spectra from SEED, blended; COLOR is the density
    float x = s * 4.0f;
    uint32_t seg = (uint32_t)x;
    if (seg > 3u) seg = 3u;
    float f = x - (float)seg;
    f = f * f * (3.0f - 2.0f * f);
    float dens = 0.1f + 0.9f * s_color;
    for (uint32_t h = 1; h <= H; h++) {
        float a = (1.0f - f) * spectral_amp(seg, h, dens) + f * spectral_amp(seg + 1u, h, dens);
        float ph = u01(hash3(s_seed, 999u, h));  // one phase per harmonic for the whole table
        s_S[h] = a * sin_turns(ph + 0.25f);
        s_C[h] = a * sin_turns(ph);
    }
}

static void fill_life(float s, uint32_t H) {  // a one-dimensional cellular automaton switches the harmonics; COLOR picks the rule
    static const uint8_t kRules[8] = { 30, 90, 110, 150, 54, 60, 73, 105 };
    uint32_t n = H < 64u ? H : 64u;
    uint32_t rule = kRules[(uint32_t)(s_color * 7.0f + 0.5f) & 7u];
    uint8_t* a = s_cell;
    uint8_t* b = s_cell + 80;
    for (uint32_t i = 0; i < n; i++) a[i] = s_seed == 0u ? (uint8_t)(i == n / 2u) : (uint8_t)(u01(hash3(s_seed, 4242u, i)) < 0.3f);
    uint32_t gens = (uint32_t)(s * 42.0f + 0.5f);
    for (uint32_t g = 0; g < gens; g++) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t l = a[(i + n - 1u) % n], c = a[i], r = a[(i + 1u) % n];
            b[i] = (uint8_t)((rule >> ((l << 2) | (c << 1) | r)) & 1u);
        }
        uint8_t* t = a; a = b; b = t;
    }
    for (uint32_t i = 0; i < n; i++) if (a[i]) s_S[i + 1u] = 1.0f / s_host->sqrtf((float)(i + 1u));
    s_S[1] = 1.0f;  // the fundamental stays, so the pitch does
}

static void analyse(uint32_t H) {  // the raw cycle in s_buf to its first H harmonics
    const float norm = 2.0f / (float)WIN;
    for (uint32_t h = 1; h <= H; h++) {
        uint32_t step = (2u * h) & SIN_MASK, idx = 0;
        float as = 0.0f, ac = 0.0f;
        for (uint32_t n = 0; n < WIN; n++) {
            float v = s_buf[n];
            as += v * s_sin[idx];
            ac += v * s_sin[(idx + QUARTER) & SIN_MASK];
            idx = (idx + step) & SIN_MASK;
        }
        s_S[h] = as * norm;
        s_C[h] = ac * norm;
    }
}

static void render_window(uint32_t w) {
    uint32_t H = s_harm < 4u ? 4u : (s_harm > MAX_H ? MAX_H : s_harm);
    float s = path_pos(w);
    for (uint32_t h = 0; h <= MAX_H; h++) { s_S[h] = 0.0f; s_C[h] = 0.0f; }

    if (s_mode == 0u) fill_classic(s, H);
    else if (s_mode == 1u) fill_pwm(s, H);
    else if (s_mode == 2u) { raw_fm(s); analyse(H); }
    else if (s_mode == 3u) { raw_fold(s); analyse(H); }
    else if (s_mode == 4u) { raw_sync(s); analyse(H); }
    else if (s_mode == 5u) fill_formant(s, H);
    else if (s_mode == 6u) fill_overtone(s, H);
    else if (s_mode == 7u) fill_spectral(s, H);
    else fill_life(s, H);

    for (uint32_t n = 0; n < WIN; n++) s_buf[n] = 0.0f;
    for (uint32_t h = 1; h <= H; h++) {
        float a = s_S[h] * s_tilt[h], c = s_C[h] * s_tilt[h];
        if (a == 0.0f && c == 0.0f) continue;
        uint32_t step = (2u * h) & SIN_MASK, idx = 0;
        for (uint32_t n = 0; n < WIN; n++) {
            s_buf[n] += a * s_sin[idx] + c * s_sin[(idx + QUARTER) & SIN_MASK];
            idx = (idx + step) & SIN_MASK;
        }
    }

    // One loudness for every window: an RMS target, held under full scale.
    float peak = 0.0f, sq = 0.0f;
    for (uint32_t n = 0; n < WIN; n++) {
        float v = absf(s_buf[n]);
        if (v > peak) peak = v;
        sq += v * v;
    }
    float g = 0.0f;
    if (peak > 1.0e-9f) {
        float rms = s_host->sqrtf(sq / (float)WIN);
        g = 0.40f / rms;
        if (g * peak > 0.95f) g = 0.95f / peak;
    }
    g *= 32767.0f;
    for (uint32_t n = 0; n < WIN; n++) s_buf[n] *= g;
}

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    s_host = host;
    s_ok = 0; s_ready = 0;
    s_mode = 0; s_curve = 0; s_harm = 48; s_frames = 32; s_seed = 1;
    s_start = 0.0f; s_end = 1.0f; s_color = 0.5f; s_tiltp = 0.0f;
    s_win = 0; s_rd = WIN;
    if (!host || !scratch_mem || scratch_size < NEED_BYTES) return;
    s_sin = (float*)scratch_mem;
    s_buf = s_sin + SIN_N + 4u;
    s_ok = 1;
}

void plugin_set_param(uint8_t index, float value) {
    switch (index) {
    case 0: s_mode = value < 0.0f ? 0u : (uint32_t)(value + 0.5f); if (s_mode > 8u) s_mode = 8u; break;
    case 1: s_start = value * 0.01f; break;
    case 2: s_end = value * 0.01f; break;
    case 3: s_curve = value < 0.0f ? 0u : (uint32_t)(value + 0.5f); if (s_curve > 6u) s_curve = 6u; break;
    case 4: s_color = value < 0.0f ? 0.0f : (value > 100.0f ? 1.0f : value * 0.01f); break;
    case 5: s_harm = value < 4.0f ? 4u : (uint32_t)(value + 0.5f); break;
    case 6: s_tiltp = (value < -100.0f ? -100.0f : (value > 100.0f ? 100.0f : value)) * 0.01f; break;
    case 7: s_frames = value < 2.0f ? 2u : (uint32_t)(value + 0.5f); if (s_frames > MAX_FRAMES) s_frames = MAX_FRAMES; break;
    case 8: s_seed = value < 0.0f ? 0u : (uint32_t)(value + 0.5f); break;
    default: break;
    }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    (void)in;
    if (!s_ok || !out || !out[0]) return 0;
    if (!s_ready) prepare();
    if (samples > WIN) samples = WIN;  // one window a call, so that no call is long: the host asks for the rest
    uint32_t done = 0;
    while (done < samples) {
        if (s_rd >= WIN) {
            if (s_win >= s_frames) break;
            render_window(s_win);
            s_win++;
            s_rd = 0;
        }
        uint32_t n = WIN - s_rd;
        if (n > samples - done) n = samples - done;
        for (uint32_t i = 0; i < n; i++) {
            int16_t pcm = (int16_t)SATURATE_16BIT((int32_t)s_buf[s_rd + i]);
            for (uint8_t ch = 0; ch < channels; ch++) if (out[ch]) out[ch][done + i] = pcm;
        }
        done += n;
        s_rd += n;
    }
    return (int32_t)done;
}
