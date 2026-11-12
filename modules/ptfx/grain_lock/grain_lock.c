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
// Grain Lock: a sound bed made for the GRANULAR play mode of one instrument (GENERATOR).
// Its first three parameters are the instrument's own: GRAIN LENGTH (in samples, the number the instrument page
// shows), GRAIN SHAPE and GRAIN LOOP. Copy them over and the bed fits the grain:
//
//   - The sample is exactly 65,535 frames long. The granular voice counts positions in 65,535ths of the sample,
//     so here one position step is one frame and the grain's loop is the same length at every position:
//     2 x (GRAIN LENGTH / 2) - 2 frames (track3r-os, audio/src/voices.cpp, grain_geom).
//   - Every partial of the bed repeats exactly in that loop length. A grain therefore closes on itself wherever
//     it stands: no click with the SQUARE shape, and a clean harmonic tone with the others. Only the levels of the
//     partials move along the sample, so GRAIN POS (and its LFO or envelope) scans the timbre like a wavetable
//     with 65,535 windows.
//   - AIR adds what does not repeat (detuned twins and breath). A SQUARE grain that loops FORWARD or BACKWARD would
//     click on it, so there AIR is left out; with TRIANGLE, GAUSS or PING-PONG it is safe and it is added.
//
// PTFX_NAME: Grain Lock
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: GENERATOR
// PTFX_SCRATCH: 20480

#include "tracker_plugin.h"

#define TOTAL      65535u   // frames written: one granular position unit a frame
#define SIN_N      4096u
#define MAX_P      64u      // partials
#define MAX_TWIN   24u
#define TICK       32u      // frames between two updates of the partial levels
#define TWO_PI_F   6.28318530718f
#define NEED_BYTES ((SIN_N + 4u) * 4u)

const PluginParam PLUGIN_PARAMS[9] = {
    PTFX_PARAM_INT  ("GrainLn", 44.0f, 44100.0f, 441.0f, 1.0f, "smp"),
    PTFX_PARAM_LIST ("Shape",   0, "Square|Triangle|Gauss"),
    PTFX_PARAM_LIST ("Loop",    0, "Forward|Backward|Ping-Pong"),
    PTFX_PARAM_LIST ("Texture", 0, "Drift|Vowels|Sweep|Glass|Odd-Even|Frozen"),
    PTFX_PARAM_LIST ("Root",    3, "Grain|C-1|C-2|C-3|C-4|C-5|C-6"),
    PTFX_PARAM_INT  ("Motion",  0.0f, 100.0f, 60.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Bright",  0.0f, 100.0f, 50.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Air",     0.0f, 100.0f, 0.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Seed",    0.0f, 999.0f, 1.0f, 1.0f, "")
};

static const PtfxHostApi* s_host = 0;
static float* s_sin = 0;

static uint32_t s_k[MAX_P];      // the partial's cycles in one grain loop
static uint32_t s_inc[MAX_P];    // its phase step (a turn is 2^32)
static uint32_t s_ph[MAX_P];
static uint32_t s_tinc[MAX_TWIN];  // the detuned twin's step and phase (AIR)
static uint32_t s_tph[MAX_TWIN];
static float s_cur[MAX_P];       // level now, and its change a frame until the next tick
static float s_da[MAX_P];
static float s_base[MAX_P];      // the partial's level from BRIGHT
static float s_lf[MAX_P];        // log2 of its frequency in Hz
static float s_hz[MAX_P];
static float s_r1[MAX_P];        // two random numbers a partial, 0..1
static float s_r2[MAX_P];

static uint8_t  s_ok = 0, s_ready = 0;
static uint32_t s_len = 441, s_shape = 0, s_loop = 0, s_tex = 0, s_root = 3, s_seed = 1;
static float    s_motion = 0.6f, s_bright = 0.5f, s_air = 0.0f;
static uint32_t s_n = 0;         // partials in use
static uint32_t s_pos = 0;       // frames written
static float    s_airlvl = 0.0f, s_noise_lp = 0.0f, s_noise_k = 0.2f;
static uint32_t s_rng = 1;

static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t x = (a + 1u) * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return x;
}

static float u01(uint32_t x) { return (float)(x >> 8) * (1.0f / 16777216.0f); }

static float sin_turns(float t) {
    float x = t * (float)SIN_N;
    int32_t i = (int32_t)x;
    if ((float)i > x) i--;
    float fr = x - (float)i;
    uint32_t k = (uint32_t)i & (SIN_N - 1u);
    float a = s_sin[k];
    return a + fr * (s_sin[k + 1u] - a);
}

static float sin_phase(uint32_t ph) {
    uint32_t k = ph >> 20;
    float fr = (float)(ph & 0xFFFFFu) * (1.0f / 1048576.0f);
    float a = s_sin[k];
    return a + fr * (s_sin[k + 1u] - a);
}

static float bump(float x) {
    float d = 1.0f + x * x;
    return 1.0f / (d * d);
}

static void prepare(void) {
    for (uint32_t i = 0; i <= SIN_N; i++) s_sin[i] = s_host->sinf((float)i * (TWO_PI_F / (float)SIN_N));
    s_sin[SIN_N] = s_sin[0];

    const float rate = s_host->sample_rate != 0u ? (float)s_host->sample_rate : 44100.0f;
    // The grain's loop in frames, as the granular voice makes it for a sample of 65,535 frames.
    uint32_t period = 2u * (s_len / 2u) - 2u;
    if (period < 8u) period = 8u;

    // The highest partial: under 0.36 of the rate, and under what BRIGHT lets through.
    float top_hz = 2500.0f + 12000.0f * s_bright;
    uint32_t kmax = (uint32_t)(top_hz * (float)period / rate);
    uint32_t klim = (period * 36u) / 100u;
    if (kmax > klim) kmax = klim;
    if (kmax < 1u) kmax = 1u;

    // ROOT: which multiple of the grain's own rate is the fundamental.
    static const float kRootHz[7] = { 0.0f, 32.7032f, 65.4064f, 130.8128f, 261.6256f, 523.2511f, 1046.5023f };
    uint32_t k0 = 1u;
    if (s_root != 0u) {
        k0 = (uint32_t)(kRootHz[s_root] * (float)period / rate + 0.5f);
        if (k0 < 1u) k0 = 1u;
        if (k0 > kmax) k0 = kmax;
    }

    // The partials: whole multiples of the root, or for Glass the bar-like series, each on the loop's grid.
    static const float kGlass[16] = { 1.0f, 2.0f, 2.76f, 4.07f, 5.40f, 6.80f, 8.93f, 11.0f,
                                      13.34f, 16.0f, 18.64f, 22.0f, 24.81f, 28.0f, 31.87f, 36.0f };
    s_n = 0;
    uint32_t last = 0;
    for (uint32_t h = 1; h <= MAX_P; h++) {
        uint32_t k;
        if (s_tex == 3u) {
            if (h > 16u) break;
            k = (uint32_t)((float)k0 * kGlass[h - 1u] + 0.5f);
        } else {
            k = k0 * h;
        }
        if (k > kmax) break;
        if (k <= last) continue;
        last = k;
        s_k[s_n++] = k;
    }
    if (s_n == 0u) { s_k[0] = 1u; s_n = 1u; }

    // 2^32 = q x period + r; a partial of k cycles a loop steps k x 2^32 / period a frame.
    uint32_t q = 0xFFFFFFFFu / period;
    uint32_t r = 0xFFFFFFFFu % period + 1u;
    if (r == period) { q++; r = 0u; }
    const float slope = 1.5f - 1.1f * s_bright;  // the fall of the levels, as a power of the partial's number
    for (uint32_t i = 0; i < s_n; i++) {
        uint32_t k = s_k[i];
        s_inc[i] = q * k + (r * k) / period;
        s_r1[i] = u01(hash3(s_seed, 11u, i));
        s_r2[i] = u01(hash3(s_seed, 23u, i));
        s_ph[i] = (uint32_t)(u01(hash3(s_seed, 37u, i)) * 4294967040.0f);
        s_hz[i] = (float)k * rate / (float)period;
        s_lf[i] = s_host->logf(s_hz[i]) * 1.442695f;
        s_base[i] = s_host->powf((float)k / (float)s_k[0], -slope);
        s_cur[i] = 0.0f;
        s_da[i] = 0.0f;
        if (i < MAX_TWIN) {
            float det = (0.0015f + 0.0045f * s_r2[i]) * ((i & 1u) ? 1.0f : -1.0f);
            s_tinc[i] = (uint32_t)((float)s_inc[i] * (1.0f + det));
            s_tph[i] = (uint32_t)(u01(hash3(s_seed, 53u, i)) * 4294967040.0f);
        }
    }

    // AIR does not repeat in the loop: a square grain looping one way would click on it, so it is left out there.
    const uint8_t strict = (uint8_t)(s_shape == 0u && s_loop != 2u);
    s_airlvl = strict ? 0.0f : s_air;
    s_noise_k = 0.04f + 0.5f * s_bright;
    s_noise_lp = 0.0f;
    s_rng = (s_seed + 1u) * 2654435761u | 1u;
    s_pos = 0;
    s_ready = 1;
}

// The level of every partial at the place t (0..1) of the sample, into `to`, already scaled to the output.
static void levels_at(float t, float* to) {
    static const float kVow[5][3] = {
        { 730.0f, 1090.0f, 2440.0f }, { 530.0f, 1840.0f, 2480.0f }, { 270.0f, 2290.0f, 3010.0f },
        { 570.0f, 840.0f, 2410.0f },  { 300.0f, 870.0f, 2240.0f } };
    static const float kBw[3] = { 80.0f, 110.0f, 170.0f };
    static const float kGain[3] = { 1.0f, 0.7f, 0.4f };
    const float m = s_motion;
    float fm[3] = { 0.0f, 0.0f, 0.0f };
    float lfc = 0.0f, even = 0.0f, kf = 0.0f;
    uint32_t key = 0;

    if (s_tex == 1u) {  // Vowels: A E I O U along the sample
        float x = t * m * 4.0f;
        uint32_t seg = (uint32_t)x;
        if (seg > 3u) seg = 3u;
        float f = x - (float)seg;
        for (uint32_t j = 0; j < 3u; j++) fm[j] = (1.0f - f) * kVow[seg][j] + f * kVow[seg + 1u][j];
    } else if (s_tex == 2u) {  // Sweep: a peak that climbs, five octaves at full MOTION, centred on 800 Hz
        lfc = 9.644f + (t - 0.5f) * m * 5.0f;
    } else if (s_tex == 4u) {  // Odd-Even: hollow to full
        even = 0.5f + (t - 0.5f) * 2.0f * m;
        if (even < 0.0f) even = 0.0f;
    } else if (s_tex == 5u) {  // Frozen: 2..8 random spectra, blended
        uint32_t keys = 2u + (uint32_t)(m * 6.0f);
        float x = t * (float)(keys - 1u);
        key = (uint32_t)x;
        if (key > keys - 2u) key = keys - 2u;
        kf = x - (float)key;
        kf = kf * kf * (3.0f - 2.0f * kf);
    }

    float sq = 0.0f;
    for (uint32_t i = 0; i < s_n; i++) {
        float a = s_base[i];
        if (s_tex == 0u) {  // Drift: every partial breathes at its own pace
            float v = 0.5f + 0.5f * sin_turns(s_r1[i] + t * m * (0.5f + 6.0f * s_r2[i]));
            a *= 0.12f + 0.88f * v * v;
        } else if (s_tex == 1u) {
            float g = 0.03f;
            for (uint32_t j = 0; j < 3u; j++) {
                float d = (s_hz[i] - fm[j]) / kBw[j];
                g += kGain[j] / (1.0f + d * d);
            }
            a *= g;
        } else if (s_tex == 2u) {
            a *= 0.06f + bump((s_lf[i] - lfc) * 2.8f);
        } else if (s_tex == 3u) {  // Glass: each partial swells at its own place
            a *= (1.0f - m) + 0.04f + m * 1.6f * bump((t - s_r1[i]) * 7.0f);
        } else if (s_tex == 4u) {
            if (i & 1u) a *= even;
        } else {
            float a0 = u01(hash3(s_seed, 100u + key, i));
            float a1 = u01(hash3(s_seed, 101u + key, i));
            float v = (1.0f - kf) * a0 * a0 + kf * a1 * a1;
            a *= 0.02f + v;
        }
        to[i] = a;
        sq += a * a;
    }
    // One loudness at every position: 0.22 RMS of full scale.
    float g = 0.0f;
    if (sq > 1.0e-12f) g = 0.22f * 32767.0f / s_host->sqrtf(sq * 0.5f);
    for (uint32_t i = 0; i < s_n; i++) to[i] *= g;
}

static float noise(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (float)(int32_t)s_rng * (1.0f / 2147483648.0f);
}

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    s_host = host;
    s_ok = 0; s_ready = 0;
    s_len = 441; s_shape = 0; s_loop = 0; s_tex = 0; s_root = 3; s_seed = 1;
    s_motion = 0.6f; s_bright = 0.5f; s_air = 0.0f;
    s_n = 0; s_pos = 0;
    if (!host || !scratch_mem || scratch_size < NEED_BYTES) return;
    s_sin = (float*)scratch_mem;
    s_ok = 1;
}

static uint32_t to_range(float v, uint32_t lo, uint32_t hi) {
    if (v < (float)lo) return lo;
    uint32_t u = (uint32_t)(v + 0.5f);
    return u > hi ? hi : u;
}

static float to_unit(float v) { return v < 0.0f ? 0.0f : (v > 100.0f ? 1.0f : v * 0.01f); }

void plugin_set_param(uint8_t index, float value) {
    switch (index) {
    case 0: s_len = to_range(value, 44u, 44100u); break;
    case 1: s_shape = to_range(value, 0u, 2u); break;
    case 2: s_loop = to_range(value, 0u, 2u); break;
    case 3: s_tex = to_range(value, 0u, 5u); break;
    case 4: s_root = to_range(value, 0u, 6u); break;
    case 5: s_motion = to_unit(value); break;
    case 6: s_bright = to_unit(value); break;
    case 7: s_air = to_unit(value); break;
    case 8: s_seed = to_range(value, 0u, 999u); break;
    default: break;
    }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    (void)in;
    if (!s_ok || !out || !out[0]) return 0;
    if (!s_ready) prepare();

    float acc[TICK];
    float target[MAX_P];
    if (samples > 2048u) samples = 2048u;  // a short piece a call, so that no call is long: the host asks for the rest
    uint32_t done = 0;
    while (done < samples && s_pos < TOTAL) {
        uint32_t in_tick = s_pos % TICK;
        if (in_tick == 0u) {
            if (s_pos == 0u) {
                levels_at(0.0f, target);
                for (uint32_t i = 0; i < s_n; i++) s_cur[i] = target[i];
            }
            levels_at((float)(s_pos + TICK) * (1.0f / (float)TOTAL), target);
            for (uint32_t i = 0; i < s_n; i++) s_da[i] = (target[i] - s_cur[i]) * (1.0f / (float)TICK);
        }
        uint32_t n = TICK - in_tick;
        if (n > samples - done) n = samples - done;
        if (n > TOTAL - s_pos) n = TOTAL - s_pos;

        for (uint32_t j = 0; j < n; j++) acc[j] = 0.0f;
        for (uint32_t i = 0; i < s_n; i++) {
            uint32_t ph = s_ph[i];
            const uint32_t inc = s_inc[i];
            float a = s_cur[i];
            const float da = s_da[i];
            if (s_airlvl > 0.0f && i < MAX_TWIN) {
                uint32_t tp = s_tph[i];
                const uint32_t ti = s_tinc[i];
                const float tw = 0.7f * s_airlvl;
                for (uint32_t j = 0; j < n; j++) {
                    acc[j] += a * (sin_phase(ph) + tw * sin_phase(tp));
                    ph += inc; tp += ti; a += da;
                }
                s_tph[i] = tp;
            } else {
                for (uint32_t j = 0; j < n; j++) {
                    acc[j] += a * sin_phase(ph);
                    ph += inc; a += da;
                }
            }
            s_ph[i] = ph;
            s_cur[i] = a;
        }
        if (s_airlvl > 0.0f) {
            const float lvl = s_airlvl * 0.10f * 32767.0f;
            for (uint32_t j = 0; j < n; j++) {
                s_noise_lp += s_noise_k * (noise() - s_noise_lp);
                acc[j] += lvl * s_noise_lp;
            }
        }
        for (uint32_t j = 0; j < n; j++) {
            int16_t pcm = (int16_t)SATURATE_16BIT((int32_t)acc[j]);
            for (uint8_t ch = 0; ch < channels; ch++) if (out[ch]) out[ch][done + j] = pcm;
        }
        done += n;
        s_pos += n;
    }
    return (int32_t)done;
}
