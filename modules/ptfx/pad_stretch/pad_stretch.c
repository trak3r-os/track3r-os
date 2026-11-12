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
// Pad Stretch: stretches the sample 1 to 16 times by smearing it (EFFECT, the result is longer than the source).
// The sample is read in overlapping slices. Of each slice only the loudness of every frequency is kept; the phases
// are thrown away and drawn at random, and the slices are laid over each other again, further apart than they
// were read. Nothing of the source's rhythm or attack survives, only its colour through time: a hit becomes a
// swell, a word a choir, a chord a drone. At x1 the length stays and the sound is only smeared.
//
// The result grows by at most ROOM seconds (asked from the host before the run); it ends earlier when the
// stretched source ends, and it is faded out when ROOM is too small for it. The source is kept in the plugin's
// memory while it is stretched: the first 6.3 seconds of it are used.
//
// One slice is made a call (at most 2,048 frames returned), so that no call is long.
//
// PTFX_NAME: Pad Stretch
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: EFFECT
// PTFX_SCRATCH: 655360

#include "tracker_plugin.h"

#define MAX_N      4096u
#define SIN_N      4096u
#define SIN_MASK   4095u
#define QUARTER    1024u
#define BLOCK      8192u   // the host's full block: a shorter one in the body is the body's last
#define FADE       4096u
#define TWO_PI_F   6.28318530718f
// Floats: the sine table, re, im, the overlap buffer, one hop, the smoothed spectrum, the tone table, the level table.
#define FLOATS     (SIN_N + 4u + 3u * MAX_N + MAX_N / 2u + (MAX_N / 2u + 4u) * 2u + MAX_N / 2u)

const PluginParam PLUGIN_PARAMS[8] = {
    PTFX_PARAM_LIST ("Stretch", 3, "x1|x2|x3|x4|x6|x8|x12|x16"),
    PTFX_PARAM_LIST ("Window",  2, "Short|Medium|Long"),
    PTFX_PARAM_INT  ("Blur",    0.0f, 100.0f, 0.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Shimmer", 0.0f, 100.0f, 0.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Sub",     0.0f, 100.0f, 0.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Tone",    -100.0f, 100.0f, 0.0f, 1.0f, "%"),
    PTFX_PARAM_INT  ("Room",    1.0f, 30.0f, 8.0f, 1.0f, "s"),
    PTFX_PARAM_INT  ("Seed",    0.0f, 999.0f, 1.0f, 1.0f, "")
};

static const uint8_t kStretch[8] = { 1, 2, 3, 4, 6, 8, 12, 16 };

static const PtfxHostApi* s_host = 0;
static float*   s_sin = 0;
static float*   s_re = 0;
static float*   s_im = 0;
static float*   s_acc = 0;    // the overlap buffer, N long, in full-scale units
static float*   s_hop = 0;    // the hop being handed out, in 16-bit units
static float*   s_ms = 0;     // the spectrum, smoothed over the slices (BLUR)
static float*   s_tl = 0;     // TONE: the gain of each bin
static float*   s_comp = 0;   // what makes the loudness of two overlapping slices even
static int16_t* s_src = 0;    // the source, as far as it has been seen
static uint32_t s_cap = 0;    // frames of source that fit

static uint8_t  s_ok = 0, s_ready = 0, s_end_known = 0, s_tail_done = 0, s_finished = 0, s_first = 1;
static uint32_t s_stretch = 4, s_wsel = 2, s_room = 8, s_seed = 1;
static float    s_blur = 0.0f, s_up = 0.0f, s_dn = 0.0f, s_tone = 0.0f;
static uint32_t s_n = MAX_N, s_h = MAX_N / 2u;  // slice and hop, in frames
static uint32_t s_stored = 0;     // frames of source kept
static uint32_t s_emitted = 0;    // frames returned
static uint32_t s_frame = 0;      // the next slice
static uint32_t s_hop_rd = 0;     // read place in s_hop (s_h: empty)
static uint32_t s_append = 0, s_in_append = 0;
static uint32_t s_rng = 1;

// In place, n points (a power of two up to SIN_N), forward.
static void fft(float* re, float* im, uint32_t n) {
    uint32_t j = 0;
    for (uint32_t i = 0; i < n - 1u; i++) {
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
        uint32_t m = n >> 1;
        while (m != 0u && (j & m) != 0u) { j ^= m; m >>= 1; }
        j |= m;
    }
    uint32_t tstep = SIN_N / 2u;
    for (uint32_t len = 2u; len <= n; len <<= 1) {
        const uint32_t half = len >> 1;
        for (uint32_t k = 0; k < half; k++) {
            const uint32_t idx = k * tstep;
            const float wr = s_sin[(idx + QUARTER) & SIN_MASK];
            const float wi = -s_sin[idx];
            for (uint32_t i = k; i < n; i += len) {
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

static float hann(uint32_t n) { return 0.5f - 0.5f * s_sin[(n * (SIN_N / s_n) + QUARTER) & SIN_MASK]; }

static void prepare(void) {
    for (uint32_t i = 0; i <= SIN_N; i++) s_sin[i] = s_host->sinf((float)i * (TWO_PI_F / (float)SIN_N));
    s_sin[SIN_N] = s_sin[0];
    const float rate = s_host->sample_rate != 0u ? (float)s_host->sample_rate : 44100.0f;
    s_n = 1024u << s_wsel;
    s_h = s_n >> 1;
    for (uint32_t n = 0; n < s_n; n++) s_acc[n] = 0.0f;
    for (uint32_t k = 0; k <= s_h; k++) s_ms[k] = 0.0f;
    // Two slices overlap by half. Their phases are unrelated, so their powers add: this evens the sum out.
    for (uint32_t n = 0; n < s_h; n++) {
        const float a = hann(n), b = hann(n + s_h);
        s_comp[n] = 1.0f / s_host->sqrtf(a * a + b * b);
    }
    // TONE: up to 6 dB an octave around 1 kHz, held to +-24 dB.
    for (uint32_t k = 1; k < s_h; k++) {
        float g = s_host->powf((float)k * rate / ((float)s_n * 1000.0f), s_tone);
        s_tl[k] = g > 16.0f ? 16.0f : (g < 0.0625f ? 0.0625f : g);
    }
    s_rng = (s_seed + 1u) * 2654435761u | 1u;
    s_stored = 0; s_emitted = 0; s_frame = 0; s_hop_rd = s_h; s_in_append = 0;
    s_end_known = 0; s_tail_done = 0; s_finished = 0; s_first = 1;
    s_ready = 1;
}

// The next hop into s_hop. 0: there is none (the source is used up, or not enough of it has come yet).
static uint8_t next_hop(void) {
    if (s_finished) return 0;
    const uint32_t p = (s_frame * s_h) / s_stretch;  // where this slice starts in the source
    if (s_end_known && p >= s_stored) {
        if (s_tail_done) { s_finished = 1; return 0; }
        s_tail_done = 1;  // the second half of the last slice is still in the overlap buffer
    } else {
        if (!s_end_known && p + s_n > s_stored) return 0;
        float e = 0.0f;
        for (uint32_t n = 0; n < s_n; n++) {
            const float v = (p + n < s_stored) ? (float)s_src[p + n] * (1.0f / 32768.0f) : 0.0f;
            e += v * v;
            s_re[n] = v * hann(n);
            s_im[n] = 0.0f;
        }
        if (e > 1.0e-9f || !s_first) {  // nothing is drawn for silence before the first sound
            fft(s_re, s_im, s_n);
            const float keep = s_first ? 1.0f : 1.0f - 0.9f * s_blur;
            for (uint32_t k = 1; k < s_h; k++) {
                const float m = s_host->sqrtf(s_re[k] * s_re[k] + s_im[k] * s_im[k]);
                s_ms[k] += keep * (m - s_ms[k]);
            }
            s_first = 0;
            float e_in = 0.0f, e_out = 0.0f;
            for (uint32_t k = 1; k < s_h; k++) {
                float m = s_ms[k];
                e_in += m * m;
                if (s_up > 0.0f) {  // SHIMMER: what is an octave below, moved up here
                    const uint32_t j = k >> 1;
                    const float u = (k & 1u) ? 0.5f * (s_ms[j] + s_ms[j + 1u]) : s_ms[j];
                    m += s_up * 0.7071f * u;
                }
                if (s_dn > 0.0f && 2u * k + 1u < s_h) {  // SUB: what is an octave above, moved down here
                    const float a = s_ms[2u * k], b = s_ms[2u * k + 1u];
                    m += s_dn * (a > b ? a + 0.4f * b : b + 0.4f * a);
                }
                m *= s_tl[k];
                e_out += m * m;
                s_re[k] = m;
            }
            // SHIMMER, SUB and TONE move the sound; the slice stays as loud as it was.
            const float norm = e_out > 1.0e-20f ? s_host->sqrtf(e_in / e_out) : 0.0f;
            for (uint32_t k = 1; k < s_h; k++) {
                const float m = s_re[k] * norm;
                s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
                const uint32_t i = s_rng >> 20;
                const float re = m * s_sin[(i + QUARTER) & SIN_MASK], im = m * s_sin[i];
                s_re[k] = re; s_im[k] = im;
                s_re[s_n - k] = re; s_im[s_n - k] = -im;
            }
            s_re[0] = 0.0f; s_im[0] = 0.0f; s_re[s_h] = 0.0f; s_im[s_h] = 0.0f;
            fft(s_re, s_im, s_n);
            // 1 / N for the transform back, and sqrt(8 / 3) for the power the window took out.
            const float scale = 1.6329932f / (float)s_n;
            for (uint32_t n = 0; n < s_n; n++) s_acc[n] += s_re[n] * hann(n) * scale;
        }
    }
    for (uint32_t n = 0; n < s_h; n++) {
        float x = s_acc[n] * s_comp[n];
        // A soft ceiling: random phases make higher peaks than the source had.
        x = x > 1.5f ? 1.0f : (x < -1.5f ? -1.0f : x - x * x * x * (4.0f / 27.0f));
        s_hop[n] = x * 32767.0f;
        s_acc[n] = s_acc[n + s_h];
        s_acc[n + s_h] = 0.0f;
    }
    s_frame++;
    s_hop_rd = 0;
    return 1;
}

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    s_host = host;
    s_ok = 0; s_ready = 0;
    s_stretch = 4; s_wsel = 2; s_room = 8; s_seed = 1;
    s_blur = 0.0f; s_up = 0.0f; s_dn = 0.0f; s_tone = 0.0f;
    s_append = 0; s_cap = 0;
    if (!host || !scratch_mem || scratch_size < FLOATS * 4u + 2u * MAX_N * 2u) return;
    s_sin = (float*)scratch_mem;
    s_re = s_sin + SIN_N + 4u;
    s_im = s_re + MAX_N;
    s_acc = s_im + MAX_N;
    s_hop = s_acc + MAX_N;
    s_ms = s_hop + MAX_N / 2u;
    s_tl = s_ms + MAX_N / 2u + 4u;
    s_comp = s_tl + MAX_N / 2u + 4u;
    s_src = (int16_t*)(s_comp + MAX_N / 2u);
    s_cap = (scratch_size - FLOATS * 4u) / 2u;
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
    case 0: s_stretch = kStretch[to_range(value, 0u, 7u)]; break;
    case 1: s_wsel = to_range(value, 0u, 2u); break;
    case 2: s_blur = unit(value); break;
    case 3: s_up = unit(value); break;
    case 4: s_dn = unit(value); break;
    case 5: s_tone = (value < -100.0f ? -100.0f : (value > 100.0f ? 100.0f : value)) * 0.01f; break;
    case 6:
        s_room = to_range(value, 1u, 30u);
        if (s_host) {  // the host takes this during the parameter pass that follows init, and at no other time
            const uint32_t rate = s_host->sample_rate != 0u ? s_host->sample_rate : 44100u;
            s_append = s_room * rate;
            if (s_host->set_extra_samples) s_host->set_extra_samples(0u, s_append);
        }
        break;
    case 7: s_seed = to_range(value, 0u, 999u); break;
    default: break;
    }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!s_ok || !out || !out[0]) return 0;
    if (!s_ready) prepare();
    const int16_t* src = in ? in[0] : 0;
    if (src) {
        // This block is the source from frame s_emitted on: keep what is new of it.
        uint32_t have = s_emitted + samples;
        if (have > s_cap) have = s_cap;
        for (uint32_t i = s_stored; i < have; i++) s_src[i] = src[i - s_emitted];
        if (have > s_stored) s_stored = have;
        if (samples < BLOCK || s_stored >= s_cap) s_end_known = 1;
    } else {
        s_end_known = 1;  // past the body: frames the host added at the end
    }

    uint32_t done = 0;
    uint8_t made = 0;
    while (done < samples) {
        if (s_hop_rd >= s_h) {
            if (made || !next_hop()) break;
            made = 1;
        }
        uint32_t n = s_h - s_hop_rd;
        if (n > samples - done) n = samples - done;
        for (uint32_t i = 0; i < n; i++) {
            float v = s_hop[s_hop_rd + i];
            if (!src) {  // ROOM running out: fade the last frames of it
                const uint32_t left = s_append > s_in_append + i ? s_append - s_in_append - i : 0u;
                if (left < FADE) v *= (float)left * (1.0f / (float)FADE);
            }
            const int16_t pcm = (int16_t)SATURATE_16BIT((int32_t)v);
            for (uint8_t ch = 0; ch < channels; ch++) if (out[ch]) out[ch][done + i] = pcm;
        }
        if (!src) s_in_append += n;
        s_hop_rd += n;
        done += n;
    }
    s_emitted += done;
    return (int32_t)done;
}
