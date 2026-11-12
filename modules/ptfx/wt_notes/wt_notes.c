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
// WT Notes: a wavetable whose windows are NOTES (GENERATOR + WAVETABLE).
// A wavetable window is one loop of 2,048 samples that the instrument plays once per cycle of the note. A window
// that holds k whole cycles therefore sounds k times higher. This generator writes STEPS windows, each holding a
// whole number of cycles chosen from a chord or a scale, so moving the wavetable position plays an arpeggio, a
// melody or (STACK above 1) a chord sequence out of ONE held note: with the position LFO, the position envelope
// or the position step FX.
//
// The price is the octave: the root sounds 3, 4 or 5 octaves above the key played (TUNING), because the root
// needs 8, 16 or 32 cycles in the window for the other notes to land on whole numbers. Harmonics, Octaves and
// Fifths need no such room and sound at the key (Fifths: one octave up).
//
// PTFX_NAME: WT Notes
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: GENERATOR, WAVETABLE
// PTFX_SCRATCH: 32768

#include "tracker_plugin.h"

#define WIN        2048u
#define SIN_N      4096u
#define SIN_MASK   4095u
#define MAX_STEPS  43u
#define MAX_CYCLES 512u   // a note's own pitch, in cycles a window
#define MAX_PART   720u   // the highest partial written, in cycles a window (the window's limit is 1,024)
#define TWO_PI_F   6.28318530718f
#define NEED_BYTES ((SIN_N + 4u + WIN) * 4u)

const PluginParam PLUGIN_PARAMS[9] = {
    PTFX_PARAM_LIST ("Notes",  3, "Harmonics|Octaves|Fifths|Major|Minor|Maj7|Min7|Dom7|Sus4|Dim|PentaMaj|PentaMin|MajScale|MinScale|Dorian|Chromatic"),
    PTFX_PARAM_LIST ("Order",  0, "Up|Down|Up+Down|Random"),
    PTFX_PARAM_INT  ("Stack",  1.0f, 4.0f, 1.0f, 1.0f, ""),
    PTFX_PARAM_INT  ("Steps",  2.0f, 43.0f, 8.0f, 1.0f, ""),
    PTFX_PARAM_INT  ("Octaves", 1.0f, 4.0f, 1.0f, 1.0f, ""),
    PTFX_PARAM_LIST ("Wave",   2, "Sine|Triangle|Saw|Square|Organ"),
    PTFX_PARAM_INT  ("Bright", 1.0f, 32.0f, 8.0f, 1.0f, ""),
    PTFX_PARAM_LIST ("Tuning", 1, "x8 +3oct|x16 +4oct|x32 +5oct"),
    PTFX_PARAM_INT  ("Seed",   0.0f, 99.0f, 1.0f, 1.0f, "")
};

// The note sets from index 3 on, as semitones above the root.
static const uint8_t kSetLen[16] = { 0, 0, 0, 3, 3, 4, 4, 4, 3, 4, 5, 5, 7, 7, 7, 12 };
static const uint8_t kSet[16][12] = {
    { 0 }, { 0 }, { 0 },
    { 0, 4, 7 }, { 0, 3, 7 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 }, { 0, 4, 7, 10 }, { 0, 5, 7 }, { 0, 3, 6, 9 },
    { 0, 2, 4, 7, 9 }, { 0, 3, 5, 7, 10 },
    { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 3, 5, 7, 9, 10 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }
};
// 2^(n/12)
static const float kSemi[12] = { 1.0f, 1.059463f, 1.122462f, 1.189207f, 1.259921f, 1.334840f,
                                 1.414214f, 1.498307f, 1.587401f, 1.681793f, 1.781797f, 1.887749f };

static const PtfxHostApi* s_host = 0;
static float* s_sin = 0;
static float* s_buf = 0;

static uint8_t  s_ok = 0, s_ready = 0;
static uint32_t s_set = 3, s_order = 0, s_stack = 1, s_steps = 8, s_octs = 1, s_wave = 2, s_bright = 8, s_tuning = 1, s_seed = 1;
static uint32_t s_win = 0, s_rd = WIN;

static float absf(float x) { return x < 0.0f ? -x : x; }

static uint32_t hash2(uint32_t a, uint32_t b) {
    uint32_t x = (a + 1u) * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return x;
}

// How many notes one pass of the pattern has: the set over OCTAVES, and the root on top.
static uint32_t pattern_len(void) {
    if (s_set == 0u) return 8u * s_octs;        // harmonics 1..8, 1..16, ...
    if (s_set == 1u) return s_octs + 1u;        // 1, 2, 4, ...
    if (s_set == 2u) return 2u * s_octs + 1u;   // 2, 3, 4, 6, 8, ...
    return (uint32_t)kSetLen[s_set] * s_octs + 1u;
}

// The cycles a window holds for note `idx` of the set (0 = the root), counted on up past the pattern's top.
static uint32_t note_cycles(uint32_t idx) {
    uint32_t k;
    if (s_set == 0u) {
        k = idx + 1u;
    } else if (s_set == 1u) {
        k = 1u << (idx > 9u ? 9u : idx);
    } else if (s_set == 2u) {
        uint32_t o = idx >> 1;
        if (o > 8u) o = 8u;
        k = (idx & 1u) ? (3u << o) : (2u << o);
    } else {
        uint32_t len = kSetLen[s_set];
        uint32_t oct = idx / len;
        uint32_t semi = kSet[s_set][idx % len];
        uint32_t base = 8u << s_tuning;
        // The nearest whole number of cycles; the minor seventh takes the pure 7/4 where the equal one misses by a third of a semitone.
        k = (semi == 10u && s_tuning < 2u) ? (base * 7u) / 4u : (uint32_t)((float)base * kSemi[semi] + 0.5f);
        if (oct > 6u) oct = 6u;
        k <<= oct;
    }
    while (k > MAX_CYCLES) k >>= 1;  // too high for the window: down an octave
    return k < 1u ? 1u : k;
}

static uint32_t pattern_note(uint32_t w) {
    uint32_t n = pattern_len();
    if (n < 2u) return 0u;
    if (s_order == 0u) return w % n;
    if (s_order == 1u) return n - 1u - (w % n);
    if (s_order == 2u) {
        uint32_t period = 2u * n - 2u;
        uint32_t m = w % period;
        return m < n ? m : period - m;
    }
    return hash2(s_seed, w) % n;
}

static float partial_amp(uint32_t h) {
    static const float kOrgan[8] = { 1.0f, 0.7f, 0.5f, 0.35f, 0.0f, 0.25f, 0.0f, 0.2f };
    float fh = (float)h;
    if (s_wave == 0u) return h == 1u ? 1.0f : 0.0f;
    if (s_wave == 1u) return (h & 1u) ? ((((h >> 1) & 1u) ? -1.0f : 1.0f) / (fh * fh)) : 0.0f;
    if (s_wave == 2u) return ((h & 1u) ? 1.0f : -1.0f) / fh;
    if (s_wave == 3u) return (h & 1u) ? 1.0f / fh : 0.0f;
    return h <= 8u ? kOrgan[h - 1u] : 0.0f;
}

static void add_note(uint32_t cycles, float gain) {
    for (uint32_t h = 1; h <= s_bright; h++) {
        uint32_t kh = cycles * h;
        if (kh > MAX_PART) break;
        float a = partial_amp(h) * gain;
        if (a == 0.0f) continue;
        uint32_t step = (2u * kh) & SIN_MASK, idx = 0;
        for (uint32_t n = 0; n < WIN; n++) {
            s_buf[n] += a * s_sin[idx];
            idx = (idx + step) & SIN_MASK;
        }
    }
}

static void render_window(uint32_t w) {
    static const float kStackGain[4] = { 1.0f, 0.8f, 0.7f, 0.6f };
    for (uint32_t n = 0; n < WIN; n++) s_buf[n] = 0.0f;
    uint32_t note = pattern_note(w);
    // STACK: scales and pentatonics pile up every other note (thirds, so each step is the chord of its degree),
    // the chromatic set piles fifths, the chords and series their next notes.
    uint32_t skip = (s_set >= 10u && s_set <= 14u) ? 2u : (s_set == 15u ? 7u : 1u);
    for (uint32_t j = 0; j < s_stack; j++) add_note(note_cycles(note + j * skip), kStackGain[j]);

    float peak = 0.0f, sq = 0.0f;
    for (uint32_t n = 0; n < WIN; n++) {
        float v = absf(s_buf[n]);
        if (v > peak) peak = v;
        sq += v * v;
    }
    float g = 0.0f;
    if (peak > 1.0e-9f) {
        float rms = s_host->sqrtf(sq / (float)WIN);
        g = 0.38f / rms;
        if (g * peak > 0.95f) g = 0.95f / peak;
    }
    g *= 32767.0f;
    for (uint32_t n = 0; n < WIN; n++) s_buf[n] *= g;
}

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    s_host = host;
    s_ok = 0; s_ready = 0;
    s_set = 3; s_order = 0; s_stack = 1; s_steps = 8; s_octs = 1; s_wave = 2; s_bright = 8; s_tuning = 1; s_seed = 1;
    s_win = 0; s_rd = WIN;
    if (!host || !scratch_mem || scratch_size < NEED_BYTES) return;
    s_sin = (float*)scratch_mem;
    s_buf = s_sin + SIN_N + 4u;
    s_ok = 1;
}

static uint32_t to_range(float v, uint32_t lo, uint32_t hi) {
    if (v < (float)lo) return lo;
    uint32_t u = (uint32_t)(v + 0.5f);
    return u > hi ? hi : u;
}

void plugin_set_param(uint8_t index, float value) {
    switch (index) {
    case 0: s_set = to_range(value, 0u, 15u); break;
    case 1: s_order = to_range(value, 0u, 3u); break;
    case 2: s_stack = to_range(value, 1u, 4u); break;
    case 3: s_steps = to_range(value, 2u, MAX_STEPS); break;
    case 4: s_octs = to_range(value, 1u, 4u); break;
    case 5: s_wave = to_range(value, 0u, 4u); break;
    case 6: s_bright = to_range(value, 1u, 32u); break;
    case 7: s_tuning = to_range(value, 0u, 2u); break;
    case 8: s_seed = to_range(value, 0u, 99u); break;
    default: break;
    }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    (void)in;
    if (!s_ok || !out || !out[0]) return 0;
    if (!s_ready) {
        for (uint32_t i = 0; i <= SIN_N; i++) s_sin[i] = s_host->sinf((float)i * (TWO_PI_F / (float)SIN_N));
        s_sin[SIN_N] = s_sin[0];
        s_ready = 1;
    }
    if (samples > WIN) samples = WIN;  // one window a call, so that no call is long: the host asks for the rest
    uint32_t done = 0;
    while (done < samples) {
        if (s_rd >= WIN) {
            if (s_win >= s_steps) break;
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
