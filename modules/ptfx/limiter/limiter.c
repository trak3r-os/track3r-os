// SPDX-License-Identifier: MIT
//
// Limiter: our rebuild, as a .ptfx plugin, of the Sample Editor effect of the same name of Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce),
// commit 7fd6c2c3002a8f690b5341c486ba69606077b513, core/effector/mtEffectLimiter.cpp/.h (parameter ranges: mtSampleEditorEngine.cpp, limiterParams). The algorithm is the
// original's (MIT, upstream LICENSE Part 1, component approved by the maintainer 2026-11-08, OS-PTFXPACK P2; THIRD_PARTY.md, LICENSE-tracker-ce.txt); this C file is a new port
// against the SDK header, not a copy.
//
// Copyright (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors (the original algorithm)
// Copyright (c) 2026 the track3r-os contributors (this port)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
// to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice, this permission notice, and the conditions in Part 2 of the upstream licence shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//
// What is the original's, kept as it is: the signal path (a one-pole average of |x| with coefficient 0.002, a peak follower on it with separate attack and release coefficients,
// a gain f = threshold / peak limited to 1, a smoothed gain g with the same two coefficients, out = g * x), the coefficient formula 1 - exp(-2.2 * 1000 / (44100 * ms)), the
// threshold 10^(dB / 20) * 32767, the parameter ranges (-60..0 dB, 0.3..10 ms, 1..2000 ms) and its 16-bit truncations: the average, the peak and the threshold are stored as unsigned
// 16-bit integers, so the average stops about 500 below a steady level and the release is never slower than one step per sample. That is the sound of the original and it is not
// "corrected". What is changed: (1) the original reads `xpeak` before it is ever written; here it starts at 0 and a peak of 0 gives f = 1 (no division by zero); (2) the original keeps the
// gain g of the previous run (a static); here every run starts at g = 1 (no gain change); (3) the exponentials are the host's single-precision expf and powf; (4) the defaults (-18 dB,
// 1.0 ms, 100 ms) are ours, the original has none. No scratch, no host extra samples: the result is as long as the selection.
//
// ---- end of the notice ----
// PTFX_NAME: Limiter
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: EFFECT, STEREO
// PTFX_SCRATCH: 0

#include "tracker_plugin.h"

// Knobs: Threshold, Attack, Release
const PluginParam PLUGIN_PARAMS[3] = {
    PTFX_PARAM_INT("Thresh", -60.0f, 0.0f, -18.0f, 1.0f, "dB"),
    PTFX_PARAM_FLOAT("Attack", 0.3f, 10.0f, 1.0f, 0.1f, "ms"),
    PTFX_PARAM_INT("Release", 1.0f, 2000.0f, 100.0f, 1.0f, "ms")
};

#define LIM_AVG_COEFF 0.002f   // the original's timeAverageCoeff

typedef struct {
    float    g;       // smoothed gain
    uint16_t xpeak;
    uint16_t a;
} LimChannel;

static const PtfxHostApi* s_host = 0;
static uint16_t   s_threshold = 4130;
static float      s_attack = 0.0f;
static float      s_release = 0.0f;
static LimChannel s_ch[2];

static float coeff_for_ms(float ms) {
    if (ms < 0.1f) ms = 0.1f;
    const float e = (s_host && s_host->expf) ? s_host->expf(-2.2f * (1000.0f / (44100.0f * ms))) : 0.0f;
    return 1.0f - e;
}

void plugin_set_param(uint8_t index, float value);

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    (void)scratch_mem;
    (void)scratch_size;
    s_host = host;
    for (int i = 0; i < 2; i++) {
        s_ch[i].g = 1.0f;
        s_ch[i].xpeak = 0;
        s_ch[i].a = 0;
    }
    plugin_set_param(0, -18.0f);
    plugin_set_param(1, 1.0f);
    plugin_set_param(2, 100.0f);
}

void plugin_set_param(uint8_t index, float value) {
    if (index == 0) {
        int32_t db = (int32_t)(value < 0.0f ? value - 0.5f : value + 0.5f);
        if (db < -60) db = -60;
        if (db > 0) db = 0;
        const float lin = (s_host && s_host->powf) ? s_host->powf(10.0f, (float)db / 20.0f) : 1.0f;
        s_threshold = (uint16_t)(lin * 32767.0f);
    } else if (index == 1) {
        s_attack = coeff_for_ms(value);
    } else if (index == 2) {
        s_release = coeff_for_ms(value);
    }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!out || samples == 0) {
        return 0;
    }
    if (channels > 2) channels = 2;
    const float attack = s_attack;
    const float release = s_release;
    const float threshold = (float)s_threshold;
    for (uint8_t ch = 0; ch < channels; ch++) {
        if (!out[ch]) continue;
        const int16_t* src = (in != 0) ? in[ch] : 0;
        LimChannel* c = &s_ch[ch];
        float g = c->g;
        uint16_t xpeak = c->xpeak;
        uint16_t a = c->a;
        for (uint32_t i = 0; i < samples; i++) {
            const int16_t x = src ? src[i] : 0;
            const uint16_t in_abs = (uint16_t)(x < 0 ? -(int32_t)x : (int32_t)x);
            a = (uint16_t)((1.0f - LIM_AVG_COEFF) * (float)a + LIM_AVG_COEFF * (float)in_abs);
            float coeff = (a > xpeak) ? attack : release;
            xpeak = (uint16_t)((1.0f - coeff) * (float)xpeak + coeff * (float)a);
            float f = (xpeak == 0) ? 1.0f : threshold / (float)xpeak;
            if (f > 1.0f) f = 1.0f;
            coeff = (f < g) ? attack : release;
            g = (1.0f - coeff) * g + coeff * f;
            out[ch][i] = (int16_t)SATURATE_16BIT((int32_t)(g * (float)x));
        }
        c->g = g;
        c->xpeak = xpeak;
        c->a = a;
    }
    return (int32_t)samples;
}
