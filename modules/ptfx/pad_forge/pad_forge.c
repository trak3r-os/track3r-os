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
// Pad Forge: makes a pad that loops with no seam (GENERATOR). Set the instrument's loop over the whole sample.
// Every harmonic of the note is not one sine but a small crowd of them, each a little off the pitch and with a
// phase of its own, as when many players hold the same note. All of them fit the loop a whole number of times,
// so the end runs into the start with no click and no crossfade. The loop is made a whole number of periods of
// the note long, so the note is in tune; the other notes of a chord sit on the nearest pitch the loop allows.
//
// The idea of spreading every harmonic over a band, with random phases, is Nasca Octavian Paul's PADsynth
// algorithm (public domain). This is an implementation of its own, and it adds the sines one by one instead of
// using one large transform, so that the work can be cut into short calls.
//
// PTFX_NAME: Pad Forge
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: GENERATOR
// PTFX_SCRATCH: 36864

#include "tracker_plugin.h"

#define SIN_N      4096u
#define MAX_LINES  1024u
#define MAX_NOTES  4u
#define MAX_HARM   64u
#define TICK       64u
#define WORK       200000u   // sines times frames in one call, about
#define TWO_PI_F   6.28318530718f
#define PI_F       3.14159265359f
#define TOP_HZ     14000.0f
#define OUT_RMS    0.20f

const PluginParam PLUGIN_PARAMS[10] = {
    PTFX_PARAM_LIST ("Type",    0, "Strings|SuperSaw|Choir Ah|Choir Oo|Organ|Hollow|Brass|Glass"),
    PTFX_PARAM_LIST ("Note",    0, "C|C#|D|D#|E|F|F#|G|G#|A|A#|B"),
    PTFX_PARAM_INT  ("Octave",  1.0f, 6.0f, 3.0f, 1.0f, ""),
    PTFX_PARAM_LIST ("Chord",   0, "Single|Octave|Fifth|Major|Minor|Maj7|Min7|Sus4|Sus2|Add9|Min9|Fifths"),
    PTFX_PARAM_INT  ("Detune",  0.0f, 100.0f, 35.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Spread",  0.0f, 100.0f, 50.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Bright",  0.0f, 100.0f, 50.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Voices",  2.0f, 16.0f, 8.0f, 1.0f, ""),
    PTFX_PARAM_LIST ("Length",  2, "Short|Medium|Long|Max"),
    PTFX_PARAM_INT  ("Seed",    0.0f, 999.0f, 1.0f, 1.0f, "")
};

// Semitones above the note, 255: no note.
static const uint8_t kChord[12][MAX_NOTES] = {
    { 0, 255, 255, 255 }, { 0, 12, 255, 255 }, { 0, 7, 255, 255 }, { 0, 4, 7, 255 }, { 0, 3, 7, 255 },
    { 0, 4, 7, 11 }, { 0, 3, 7, 10 }, { 0, 5, 7, 255 }, { 0, 2, 7, 255 }, { 0, 4, 7, 14 }, { 0, 3, 10, 14 },
    { 0, 7, 14, 255 }
};
static const uint32_t kLength[4] = { 16384u, 32768u, 65536u, 88200u };
// ORGAN: the level of harmonics 1 to 16, in percent.
static const uint8_t kOrgan[17] = { 0, 100, 80, 60, 50, 0, 30, 0, 35, 0, 10, 0, 12, 0, 0, 0, 15 };
// GLASS: partials that are not harmonics (the first ones of a struck bar, and octaves between them), times 1000.
static const uint16_t kGlass[12] = { 1000, 2000, 2756, 4000, 5404, 6800, 8933, 11000, 13340, 16000, 18640, 22000 };

static const PtfxHostApi* s_host = 0;
static float*    s_sin = 0;
static uint32_t* s_inc = 0;
static uint32_t* s_ph = 0;
static float*    s_amp = 0;
static float*    s_e = 0;

static uint8_t  s_ok = 0, s_ready = 0;
static uint32_t s_type = 0, s_note = 0, s_oct = 3, s_chord = 0, s_voices = 8, s_lsel = 2, s_seed = 1;
static float    s_detune = 0.35f, s_spread = 0.5f, s_bright = 0.5f;
static uint32_t s_lines = 0, s_len = 0, s_pos = 0;
static uint32_t s_rng = 1;

static uint32_t rnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

// About normal, deviation 1.
static float gauss(void) {
    float s = 0.0f;
    for (int i = 0; i < 4; i++) s += (float)(rnd() >> 8) * (1.0f / 16777216.0f) - 0.5f;
    return s * 1.7320508f;
}

static float res(float f, float centre, float width) {
    const float d = (f - centre) / width;
    return 1.0f / (1.0f + d * d);
}

// The level of partial i (from 1) of a note at fn Hz, and how many times fn it lies. Below 0: no more partials.
static float partial(uint32_t i, float fn, float* ratio) {
    const float h = (float)i;
    const float f = fn * h;
    *ratio = h;
    switch (s_type) {
    case 0: return (1.0f / h) * (0.6f + 0.8f * res(f, 450.0f, 350.0f) + 0.6f * res(f, 2600.0f, 900.0f));
    case 1: return 1.0f / h;
    case 2: return (1.0f / h) * (0.10f + res(f, 700.0f, 110.0f) + 0.5f * res(f, 1150.0f, 120.0f)
                                 + 0.25f * res(f, 2700.0f, 180.0f));
    case 3: return (1.0f / h) * (0.06f + res(f, 320.0f, 90.0f) + 0.3f * res(f, 800.0f, 100.0f)
                                 + 0.06f * res(f, 2300.0f, 160.0f));
    case 4: return i > 16u ? -1.0f : (float)kOrgan[i] * 0.01f;
    case 5: return (i & 1u) ? 1.0f / h : 0.0f;
    case 6: {
        const float q = f * (1.0f / 2200.0f);
        return (1.0f / h) * (1.0f + 1.5f * res(f, 1000.0f, 600.0f)) / s_host->sqrtf(1.0f + q * q * q * q);
    }
    default:
        if (i > 12u) return -1.0f;
        *ratio = (float)kGlass[i - 1u] * 0.001f;
        return s_host->powf(h, -0.7f);
    }
}

static void prepare(void) {
    for (uint32_t i = 0; i <= SIN_N; i++) s_sin[i] = s_host->sinf((float)i * (TWO_PI_F / (float)SIN_N));
    s_sin[SIN_N] = s_sin[0];
    const float rate = s_host->sample_rate != 0u ? (float)s_host->sample_rate : 44100.0f;
    s_rng = (s_seed + 1u) * 2654435761u | 1u;

    // The loop: a whole number of periods of the note, as near the asked length as that allows.
    const float f0 = 440.0f * s_host->powf(2.0f, ((float)(12u * (s_oct + 1u) + s_note) - 69.0f) * (1.0f / 12.0f));
    const float want = (float)kLength[s_lsel];
    uint32_t periods = (uint32_t)(want * f0 / rate + (s_lsel == 3u ? 0.0f : 0.5f));
    if (periods < 1u) periods = 1u;
    s_len = (uint32_t)((float)periods * rate / f0 + 0.5f);
    if (s_len > 88200u) s_len = 88200u;
    const float bin = rate / (float)s_len;                // Hz between two pitches the loop allows
    uint32_t q = 0xFFFFFFFFu / s_len;                      // 2^32 / length, and what is left over
    uint32_t r = 0xFFFFFFFFu - q * s_len + 1u;
    if (r == s_len) { q++; r = 0u; }

    uint32_t notes = 0;
    while (notes < MAX_NOTES && kChord[s_chord][notes] != 255u) notes++;
    uint32_t per_note = MAX_LINES / (notes * s_voices);    // partials a note may have
    if (per_note > MAX_HARM) per_note = MAX_HARM;
    const float cents = 1.0f + 99.0f * s_detune * s_detune;
    const float width = s_host->powf(2.0f, cents * (1.0f / 1200.0f)) - 1.0f;
    const float grow = 0.5f + s_spread;                    // 1: every harmonic is as many cents wide
    const float tilt = (s_bright - 0.5f) * 2.0f;
    const float share = 1.0f / s_host->sqrtf((float)s_voices);
    const uint32_t k_top = (uint32_t)((float)s_len * 0.45f);

    s_lines = 0;
    float power = 0.0f;
    for (uint32_t n = 0; n < notes; n++) {
        const float fn = f0 * s_host->powf(2.0f, (float)kChord[s_chord][n] * (1.0f / 12.0f));
        uint32_t used = 0;
        for (uint32_t i = 1; i <= MAX_HARM && used < per_note; i++) {
            float ratio = 1.0f;
            float a = partial(i, fn, &ratio);
            if (a < 0.0f) break;
            const float f = fn * ratio;
            if (f > TOP_HZ) break;
            if (a <= 0.0f) continue;
            a *= s_host->powf(ratio, tilt) * share;
            const float sigma = 0.5f * width * fn * s_host->powf(ratio, grow);
            const uint32_t first = s_lines;
            for (uint32_t v = 0; v < s_voices; v++) {
                const float fl = f + gauss() * sigma;
                const uint32_t phase = rnd();
                if (fl < bin * 0.5f) continue;
                const uint32_t k = (uint32_t)(fl / bin + 0.5f);
                if (k < 1u || k > k_top) continue;
                const uint32_t inc = k * q + (k * r) / s_len;
                power += 0.5f * a * a;
                uint32_t same = first;
                while (same < s_lines && s_inc[same] != inc) same++;
                if (same < s_lines) {  // two of this harmonic on one pitch are one sine with the power of both
                    s_amp[same] = s_host->sqrtf(s_amp[same] * s_amp[same] + a * a);
                    continue;
                }
                s_inc[s_lines] = inc;
                s_ph[s_lines] = phase;
                s_amp[s_lines] = a;
                s_e[s_lines] = 2.0f * s_host->sinf(PI_F * (float)k / (float)s_len);
                s_lines++;
            }
            used++;
        }
    }
    const float gain = power > 0.0f ? OUT_RMS * 32767.0f / s_host->sqrtf(power) : 0.0f;
    for (uint32_t l = 0; l < s_lines; l++) s_amp[l] *= gain;
    while ((s_lines & 3u) != 0u) {  // the sines are made four at a time
        s_inc[s_lines] = 0u; s_ph[s_lines] = 0u; s_amp[s_lines] = 0.0f; s_e[s_lines] = 0.0f;
        s_lines++;
    }
    s_pos = 0;
    s_ready = 1;
}

// The sine of a phase where 2^32 is one turn.
static inline float sine(uint32_t ph) {
    const uint32_t i = ph >> 20;
    const float a = s_sin[i];
    return a + (s_sin[i + 1u] - a) * ((float)(ph & 0xFFFFFu) * (1.0f / 1048576.0f));
}

// n frames (up to TICK) of all the sines into acc, and the phases moved on.
static void render(float* acc, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) acc[i] = 0.0f;
    for (uint32_t l = 0; l < s_lines; l += 4u) {
        // Each sine is a point turning on a circle: two multiplies a frame, and no table inside the loop.
        // It starts each time from the exact phase, so nothing drifts.
        float y0 = sine(s_ph[l]),      x0 = sine(s_ph[l]      - (s_inc[l]      >> 1) + 0x40000000u);
        float y1 = sine(s_ph[l + 1u]), x1 = sine(s_ph[l + 1u] - (s_inc[l + 1u] >> 1) + 0x40000000u);
        float y2 = sine(s_ph[l + 2u]), x2 = sine(s_ph[l + 2u] - (s_inc[l + 2u] >> 1) + 0x40000000u);
        float y3 = sine(s_ph[l + 3u]), x3 = sine(s_ph[l + 3u] - (s_inc[l + 3u] >> 1) + 0x40000000u);
        const float e0 = s_e[l], e1 = s_e[l + 1u], e2 = s_e[l + 2u], e3 = s_e[l + 3u];
        const float a0 = s_amp[l], a1 = s_amp[l + 1u], a2 = s_amp[l + 2u], a3 = s_amp[l + 3u];
        for (uint32_t i = 0; i < n; i++) {
            acc[i] += a0 * y0 + a1 * y1 + a2 * y2 + a3 * y3;
            x0 -= e0 * y0; y0 += e0 * x0;
            x1 -= e1 * y1; y1 += e1 * x1;
            x2 -= e2 * y2; y2 += e2 * x2;
            x3 -= e3 * y3; y3 += e3 * x3;
        }
        s_ph[l] += s_inc[l] * n;
        s_ph[l + 1u] += s_inc[l + 1u] * n;
        s_ph[l + 2u] += s_inc[l + 2u] * n;
        s_ph[l + 3u] += s_inc[l + 3u] * n;
    }
}

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    s_host = host;
    s_ok = 0; s_ready = 0;
    s_type = 0; s_note = 0; s_oct = 3; s_chord = 0; s_voices = 8; s_lsel = 2; s_seed = 1;
    s_detune = 0.35f; s_spread = 0.5f; s_bright = 0.5f;
    if (!host || !scratch_mem || scratch_size < (SIN_N + 4u + 4u * (MAX_LINES + 4u)) * 4u) return;
    s_sin = (float*)scratch_mem;
    s_inc = (uint32_t*)(s_sin + SIN_N + 4u);
    s_ph = s_inc + MAX_LINES + 4u;
    s_amp = (float*)(s_ph + MAX_LINES + 4u);
    s_e = s_amp + MAX_LINES + 4u;
    s_ok = 1;
}

static uint32_t to_range(float v, uint32_t lo, uint32_t hi) {
    if (v < (float)lo) return lo;
    uint32_t u = (uint32_t)(v + 0.5f);
    return u > hi ? hi : u;
}

static float unit(float v) { return v < 0.0f ? 0.0f : (v > 100.0f ? 1.0f : v * 0.01f); }

void plugin_set_param(uint8_t index, float value) {
    switch (index) {
    case 0: s_type = to_range(value, 0u, 7u); break;
    case 1: s_note = to_range(value, 0u, 11u); break;
    case 2: s_oct = to_range(value, 1u, 6u); break;
    case 3: s_chord = to_range(value, 0u, 11u); break;
    case 4: s_detune = unit(value); break;
    case 5: s_spread = unit(value); break;
    case 6: s_bright = unit(value); break;
    case 7: s_voices = to_range(value, 2u, 16u); break;
    case 8: s_lsel = to_range(value, 0u, 3u); break;
    case 9: s_seed = to_range(value, 0u, 999u); break;
    default: break;
    }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    (void)in;
    if (!s_ok || !out || !out[0]) return 0;
    if (!s_ready) prepare();
    float acc[TICK];
    uint32_t done = 0, work = 0;
    while (done < samples && s_pos < s_len && work < WORK) {
        uint32_t n = TICK;
        if (n > s_len - s_pos) n = s_len - s_pos;
        if (n > samples - done) n = samples - done;
        render(acc, n);
        for (uint32_t i = 0; i < n; i++) {
            const int16_t pcm = (int16_t)SATURATE_16BIT((int32_t)acc[i]);
            for (uint8_t ch = 0; ch < channels; ch++) if (out[ch]) out[ch][done + i] = pcm;
        }
        done += n;
        s_pos += n;
        work += n * s_lines;
    }
    return (int32_t)done;
}
