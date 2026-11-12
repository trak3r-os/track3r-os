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
// WT Resynth: turns the sample in the editor into a wavetable (EFFECT + WAVETABLE).
// Every 2,048 frames of the sample (46 ms) become one window of 2,048 samples that holds ONE cycle:
//
//   1. The slice is analysed (a Hann window and a Fourier transform).
//   2. Its pitch is found (NOTE = Auto, from the autocorrelation), or taken from NOTE and OCTAVE.
//   3. The energy around each harmonic of that pitch becomes the level of that harmonic.
//   4. The cycle is rebuilt from those levels with a phase that is the same in every window.
//
// So the wavetable has as many windows as the sample has slices, the pitch of the source is taken out (every
// window plays at the key), and the position scans the timbre of the source through time with no jump in phase:
// a spoken word becomes a talking wavetable, a filter sweep stays a sweep, a noise becomes a buzzing spectrum.
// What is left over at the end of the sample (less than one slice) is dropped.
//
// PTFX_NAME: WT Resynth
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: WAVETABLE, STEREO, CONVERT_TO_MONO
// PTFX_SCRATCH: 49152

#include "tracker_plugin.h"

#define WIN        2048u
#define HALF       1024u
#define SIN_N      4096u
#define SIN_MASK   4095u
#define QUARTER    1024u
#define MAX_H      128u
#define LAG_MIN    20u     // 2,205 Hz, the highest pitch Auto finds
#define LAG_MAX    900u    // 49 Hz, the lowest
#define TWO_PI_F   6.28318530718f
#define NEED_BYTES ((SIN_N + 4u + 3u * WIN + HALF) * 4u)

const PluginParam PLUGIN_PARAMS[9] = {
    PTFX_PARAM_LIST ("Note",    0, "Auto|C|C#|D|D#|E|F|F#|G|G#|A|A#|B"),
    PTFX_PARAM_INT  ("Octave",  1.0f, 6.0f, 3.0f, 1.0f, ""),
    PTFX_PARAM_INT  ("Harm",    4.0f, 128.0f, 64.0f, 1.0f, ""),
    PTFX_PARAM_INT  ("Formant", -12.0f, 12.0f, 0.0f, 1.0f, "st"),
    PTFX_PARAM_INT  ("Smooth",  0.0f, 100.0f, 30.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Tilt",    -100.0f, 100.0f, 0.0f, 1.0f, "%"),
    PTFX_PARAM_LIST ("Level",   0, "Even|Follow"),
    PTFX_PARAM_LIST ("Phase",   0, "Saw|Chirp|Random"),
    PTFX_PARAM_INT  ("Seed",    0.0f, 999.0f, 1.0f, 1.0f, "")
};

static const PtfxHostApi* s_host = 0;
static float* s_sin = 0;    // SIN_N + 1 entries: one turn of a sine
static float* s_re = 0;     // WIN
static float* s_im = 0;     // WIN
static float* s_buf = 0;    // WIN: the power spectrum of the slice, then the window being built
static float* s_wacf = 0;   // HALF: the autocorrelation of the Hann window, 1.0 at lag 0

static float    s_amp[MAX_H + 1u];    // the level of harmonic h, smoothed over the windows
static float    s_new[MAX_H + 1u];
static float    s_tilt[MAX_H + 1u];
static uint16_t s_pidx[MAX_H + 1u];   // the phase of harmonic h, as an index into the sine table

static uint8_t  s_ok = 0, s_ready = 0, s_have = 0;
static uint32_t s_note = 0, s_oct = 3, s_harm = 64, s_level = 0, s_phase = 0, s_seed = 1;
static float    s_formant = 0.0f, s_smooth = 0.3f, s_tiltp = 0.0f;
static float    s_f0 = 130.8128f;     // the pitch in use, in Hz
static float    s_rate = 44100.0f, s_inv_shift = 1.0f;

static float absf(float x) { return x < 0.0f ? -x : x; }

static uint32_t hash2(uint32_t a, uint32_t b) {
    uint32_t x = (a + 1u) * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return x;
}

// In place, WIN points, forward. The twiddles come from the sine table.
static void fft(float* re, float* im) {
    uint32_t j = 0;
    for (uint32_t i = 0; i < WIN - 1u; i++) {
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
        uint32_t m = WIN >> 1;
        while (m != 0u && (j & m) != 0u) { j ^= m; m >>= 1; }
        j |= m;
    }
    uint32_t tstep = SIN_N / 2u;
    for (uint32_t len = 2u; len <= WIN; len <<= 1) {
        const uint32_t half = len >> 1;
        for (uint32_t k = 0; k < half; k++) {
            const uint32_t idx = k * tstep;
            const float wr = s_sin[(idx + QUARTER) & SIN_MASK];
            const float wi = -s_sin[idx];
            for (uint32_t i = k; i < WIN; i += len) {
                const uint32_t b = i + half;
                const float tr = wr * re[b] - wi * im[b];
                const float ti = wr * im[b] + wi * re[b];
                re[b] = re[i] - tr; im[b] = im[i] - ti;
                re[i] += tr; im[i] += ti;
            }
        }
        tstep >>= 1;
    }
}

static float hann(uint32_t n) { return 0.5f - 0.5f * s_sin[(2u * n + QUARTER) & SIN_MASK]; }

// The power spectrum in s_buf[0..HALF] to its autocorrelation in s_re[0..HALF) (not scaled).
static void power_to_acf(void) {
    s_re[0] = 0.0f; s_re[1] = 0.0f; s_re[WIN - 1u] = 0.0f;  // no DC in the pitch search
    for (uint32_t k = 2; k < HALF; k++) { s_re[k] = s_buf[k]; s_re[WIN - k] = s_buf[k]; }
    s_re[HALF] = s_buf[HALF];
    for (uint32_t k = 0; k < WIN; k++) s_im[k] = 0.0f;
    fft(s_re, s_im);
}

static void prepare(void) {
    for (uint32_t i = 0; i <= SIN_N; i++) s_sin[i] = s_host->sinf((float)i * (TWO_PI_F / (float)SIN_N));
    s_sin[SIN_N] = s_sin[0];
    s_rate = s_host->sample_rate != 0u ? (float)s_host->sample_rate : 44100.0f;

    // The Hann window's own autocorrelation: the slice's is divided by it, so a long lag is not taken for a weak one.
    for (uint32_t n = 0; n < WIN; n++) { s_re[n] = hann(n); s_im[n] = 0.0f; }
    fft(s_re, s_im);
    for (uint32_t k = 0; k <= HALF; k++) s_buf[k] = s_re[k] * s_re[k] + s_im[k] * s_im[k];
    s_re[0] = s_buf[0]; s_re[HALF] = s_buf[HALF];
    for (uint32_t k = 1; k < HALF; k++) { s_re[k] = s_buf[k]; s_re[WIN - k] = s_buf[k]; }
    for (uint32_t k = 0; k < WIN; k++) s_im[k] = 0.0f;
    fft(s_re, s_im);
    const float r0 = s_re[0] > 1.0e-20f ? 1.0f / s_re[0] : 0.0f;
    for (uint32_t k = 0; k < HALF; k++) s_wacf[k] = s_re[k] * r0;

    if (s_harm < 4u) s_harm = 4u;
    if (s_harm > MAX_H) s_harm = MAX_H;
    for (uint32_t h = 1; h <= MAX_H; h++) {
        s_tilt[h] = s_host->powf((float)h, s_tiltp);  // up to 6 dB an octave, either way
        float turns = 0.0f;                           // Saw: every harmonic starts at its zero
        if (s_phase == 1u) {                          // Chirp: the phases that keep the peak low
            uint32_t q = (h * (h - 1u)) % (2u * s_harm);
            turns = 1.0f - (float)q / (float)(2u * s_harm);
        } else if (s_phase == 2u) {
            turns = (float)(hash2(s_seed, h) >> 8) * (1.0f / 16777216.0f);
        }
        s_pidx[h] = (uint16_t)((uint32_t)(turns * (float)SIN_N) & SIN_MASK);
        s_amp[h] = 0.0f;
    }
    s_inv_shift = s_host->powf(2.0f, -s_formant / 12.0f);
    if (s_note != 0u) {
        const float midi = (float)(12u * (s_oct + 1u) + (s_note - 1u));
        s_f0 = 440.0f * s_host->powf(2.0f, (midi - 69.0f) / 12.0f);
    } else {
        s_f0 = 130.8128f;
    }
    s_have = 0;
    s_ready = 1;
}

// The pitch of the slice whose power spectrum is in s_buf, in Hz; 0 when it has none that is clear.
static float find_pitch(void) {
    power_to_acf();
    const float r0 = s_re[0];
    if (r0 <= 1.0e-12f) return 0.0f;
    const float inv = 1.0f / r0;
    // The normalised autocorrelation, kept in s_im for the lags of interest.
    for (uint32_t t = LAG_MIN - 1u; t <= LAG_MAX + 1u; t++) s_im[t] = s_re[t] * inv / s_wacf[t];
    uint32_t t = LAG_MIN;
    while (t < LAG_MAX && s_im[t + 1u] < s_im[t]) t++;  // down the slope that starts at lag 0
    float top = -1.0f;
    for (uint32_t u = t; u <= LAG_MAX; u++) {
        if (s_im[u] > s_im[u - 1u] && s_im[u] >= s_im[u + 1u] && s_im[u] > top) top = s_im[u];
    }
    if (top < 0.40f) return 0.0f;
    // The first peak that is nearly as high as the highest: the period, not a multiple of it.
    for (uint32_t u = t; u <= LAG_MAX; u++) {
        if (s_im[u] > s_im[u - 1u] && s_im[u] >= s_im[u + 1u] && s_im[u] >= 0.88f * top) {
            const float a = s_im[u - 1u], b = s_im[u], c = s_im[u + 1u];
            const float den = a - 2.0f * b + c;
            float lag = (float)u;
            if (den < -1.0e-9f) lag += 0.5f * (a - c) / den;
            return s_rate / lag;
        }
    }
    return 0.0f;
}

// One slice in, one window out (s_buf, in 16-bit units).
static void make_window(const int16_t* l, const int16_t* r) {
    float sq = 0.0f;
    for (uint32_t n = 0; n < WIN; n++) {
        float v = (float)l[n];
        if (r) v = 0.5f * (v + (float)r[n]);
        v *= (1.0f / 32768.0f);
        sq += v * v;
        s_re[n] = v * hann(n);
        s_im[n] = 0.0f;
    }
    const uint8_t silent = (uint8_t)(sq < (float)WIN * 2.5e-7f);  // under -66 dB
    fft(s_re, s_im);
    for (uint32_t k = 0; k <= HALF; k++) s_buf[k] = s_re[k] * s_re[k] + s_im[k] * s_im[k];

    if (s_note == 0u && !silent) {
        const float f = find_pitch();
        if (f > 0.0f) s_f0 = f;  // no clear pitch (a noise, a consonant): the last one stays
    }

    // The level of each harmonic: the energy of the band around it. FORMANT moves where the band is read.
    const float fb = s_f0 * (float)WIN / s_rate;  // one harmonic step, in bins
    const float cal = 3.2659864f / (float)WIN;    // sqrt(32 / 3) / WIN: a band's energy to the amplitude of its sine
    for (uint32_t h = 1; h <= s_harm; h++) {
        const float c = (float)h * fb * s_inv_shift;
        float lo = c - 0.5f * fb, hi = c + 0.5f * fb;
        float e = 0.0f;
        if (lo < 1.5f) lo = 1.5f;
        if (hi <= (float)(HALF - 1u) && hi > lo) {
            uint32_t k0 = (uint32_t)(lo + 0.5f), k1 = (uint32_t)(hi + 0.5f);
            for (uint32_t k = k0; k <= k1; k++) {
                float a = (float)k - 0.5f, b = (float)k + 0.5f;
                if (a < lo) a = lo;
                if (b > hi) b = hi;
                if (b > a) e += s_buf[k] * (b - a);
            }
        }
        s_new[h] = s_host->sqrtf(e) * cal;
    }

    // SMOOTH blends each window with the ones before it. Level Even holds the last spectrum through a silence.
    if (!(silent && s_level == 0u)) {
        const float k = s_have ? 1.0f - 0.92f * s_smooth : 1.0f;
        for (uint32_t h = 1; h <= s_harm; h++) s_amp[h] += k * (s_new[h] - s_amp[h]);
        if (!silent) s_have = 1;
    }

    for (uint32_t n = 0; n < WIN; n++) s_buf[n] = 0.0f;
    for (uint32_t h = 1; h <= s_harm; h++) {
        const float a = s_amp[h] * s_tilt[h];
        if (a <= 1.0e-7f) continue;
        uint32_t step = (2u * h) & SIN_MASK, idx = s_pidx[h];
        for (uint32_t n = 0; n < WIN; n++) {
            s_buf[n] += a * s_sin[idx];
            idx = (idx + step) & SIN_MASK;
        }
    }

    float peak = 0.0f, e2 = 0.0f;
    for (uint32_t n = 0; n < WIN; n++) {
        float v = absf(s_buf[n]);
        if (v > peak) peak = v;
        e2 += v * v;
    }
    float g = 0.0f;
    if (peak > 1.0e-6f) {
        g = s_level == 0u ? 0.40f / s_host->sqrtf(e2 / (float)WIN) : 1.0f;  // Even: one loudness; Follow: the source's
        if (g * peak > 0.95f) g = 0.95f / peak;
    }
    g *= 32767.0f;
    for (uint32_t n = 0; n < WIN; n++) s_buf[n] *= g;
}

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    s_host = host;
    s_ok = 0; s_ready = 0; s_have = 0;
    s_note = 0; s_oct = 3; s_harm = 64; s_level = 0; s_phase = 0; s_seed = 1;
    s_formant = 0.0f; s_smooth = 0.3f; s_tiltp = 0.0f;
    if (!host || !scratch_mem || scratch_size < NEED_BYTES) return;
    s_sin = (float*)scratch_mem;
    s_re = s_sin + SIN_N + 4u;
    s_im = s_re + WIN;
    s_buf = s_im + WIN;
    s_wacf = s_buf + WIN;
    s_ok = 1;
}

static uint32_t to_range(float v, uint32_t lo, uint32_t hi) {
    if (v < (float)lo) return lo;
    uint32_t u = (uint32_t)(v + 0.5f);
    return u > hi ? hi : u;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void plugin_set_param(uint8_t index, float value) {
    switch (index) {
    case 0: s_note = to_range(value, 0u, 12u); break;
    case 1: s_oct = to_range(value, 1u, 6u); break;
    case 2: s_harm = to_range(value, 4u, MAX_H); break;
    case 3: s_formant = clampf(value, -12.0f, 12.0f); break;
    case 4: s_smooth = clampf(value, 0.0f, 100.0f) * 0.01f; break;
    case 5: s_tiltp = clampf(value, -100.0f, 100.0f) * 0.01f; break;
    case 6: s_level = to_range(value, 0u, 1u); break;
    case 7: s_phase = to_range(value, 0u, 2u); break;
    case 8: s_seed = to_range(value, 0u, 999u); break;
    default: break;
    }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!s_ok || !in || !in[0] || !out || !out[0]) return 0;
    if (!s_ready) prepare();
    // Whole slices only: what is left at the end ends the run. One slice a call, so that no call is long.
    const uint32_t windows = samples >= WIN ? 1u : 0u;
    const int16_t* r = (channels > 1u && in[1]) ? in[1] : 0;
    for (uint32_t w = 0; w < windows; w++) {
        make_window(in[0] + w * WIN, r ? r + w * WIN : 0);
        int16_t* o0 = out[0] + w * WIN;
        int16_t* o1 = (channels > 1u && out[1]) ? out[1] + w * WIN : 0;
        for (uint32_t n = 0; n < WIN; n++) {
            int16_t pcm = (int16_t)SATURATE_16BIT((int32_t)s_buf[n]);
            o0[n] = pcm;
            if (o1) o1[n] = pcm;
        }
    }
    return (int32_t)(windows * WIN);
}
