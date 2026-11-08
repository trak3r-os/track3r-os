// SPDX-License-Identifier: MIT
//
// Three-Band EQ: our rebuild, as a .ptfx plugin, of the Sample Editor effect of the same name of Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce),
// commit 7fd6c2c3002a8f690b5341c486ba69606077b513, core/effector/mtEffectEQ.cpp/.h (parameter ranges: mtSampleEditorEngine.cpp, eqParams). The algorithm is the
// original's (MIT, upstream LICENSE Part 1, component approved by the maintainer 2026-11-08, OS-PTFXPACK P3; THIRD_PARTY.md, LICENSE-tracker-ce.txt); this C file is a new port
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
// What is the original's, kept as it is: three bands in series (low, mid, high), each a Regalia-Mitra tunable equaliser filter (the eqfil of Soundpipe and Csound, as the original cites it:
// an all-pass section with c = tan(pi * bandwidth / fs) and d = cos(2 * pi * freq / fs), output 0.5 * (y + x + g * (x - y))), the level map (+-12 dB to a gain of 1..4 for a boost and 1..0.2
// for a cut), the bandwidth rule bw = freq / (Q + 3 * freq / 20000) limited to 0..20000 Hz, the input scaling by 32767, the clip at +-1 and the parameter ranges (levels +-12 dB; low 10..200 Hz,
// mid 100..8000 Hz, high 8000..20000 Hz; Q 0.5..5). What is changed: (1) the original skips a band when its level is exactly 1 (a dB value, so +1 dB is skipped and 0 dB runs the filter as an
// identity); here a band at 0 dB is skipped, which is the same sound without the work, and +1 dB is applied; (2) the original has no default level; here every band starts at 0 dB (flat) with
// the frequencies 100 Hz, 1000 Hz and 12000 Hz and Q 1; (3) single precision throughout (the original mixes in double literals) and the host's cosf and tanf; (4) the filter state is cleared while a band is skipped, a host table without cosf or tanf bypasses the bands, and it starts at zero
// in every run, and the coefficients are made when a parameter changes. The labels are cut to the 7 characters of the header. No scratch, no host extra samples: the result is as long as the selection.
//
// ---- end of the notice ----
// PTFX_NAME: Three-Band EQ
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: EFFECT, STEREO
// PTFX_SCRATCH: 0

#include "tracker_plugin.h"

// Knobs, per band (low, mid, high): level, centre frequency, width (Q)
const PluginParam PLUGIN_PARAMS[9] = {
    PTFX_PARAM_FLOAT("Low", -12.0f, 12.0f, 0.0f, 0.05f, "dB"),
    PTFX_PARAM_INT("L Freq", 10.0f, 200.0f, 100.0f, 1.0f, "Hz"),
    PTFX_PARAM_FLOAT("L Width", 0.5f, 5.0f, 1.0f, 0.05f, ""),
    PTFX_PARAM_FLOAT("Mid", -12.0f, 12.0f, 0.0f, 0.05f, "dB"),
    PTFX_PARAM_INT("M Freq", 100.0f, 8000.0f, 1000.0f, 10.0f, "Hz"),
    PTFX_PARAM_FLOAT("M Width", 0.5f, 5.0f, 1.0f, 0.05f, ""),
    PTFX_PARAM_FLOAT("High", -12.0f, 12.0f, 0.0f, 0.05f, "dB"),
    PTFX_PARAM_INT("H Freq", 8000.0f, 20000.0f, 12000.0f, 100.0f, "Hz"),
    PTFX_PARAM_FLOAT("H Width", 0.5f, 5.0f, 1.0f, 0.05f, "")
};

#define EQ_RATE     44117.64706f   // the hardware's exact sample rate (the original's AUDIO_SAMPLE_RATE_EXACT)
#define EQ_PI       3.14159265f
#define EQ_FREQ_MAX 20000.0f
#define EQ_Q_OFFSET 3.0f

typedef struct {
    float level;   // dB
    float freq;
    float q;
    float gain;    // 1..4 boost, 1..0.2 cut
    float c;       // tan(pi * bw / fs)
    float d;       // cos(2 * pi * freq / fs)
    int   on;      // 0: no cosf/tanf in the host table, the band is bypassed (its filter would be a double pole at 1)
} EqBand;

static const PtfxHostApi* s_host = 0;
static EqBand s_band[3];
static float  s_z[2][3][2];   // channel, band, z1 and z2

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void band_update(int b) {
    EqBand* e = &s_band[b];
    const float freq = clampf(e->freq, 0.0f, EQ_FREQ_MAX);
    const float q = e->q < 0.1f ? 0.1f : e->q;
    const float bw = clampf(freq / (q + EQ_Q_OFFSET * (freq / EQ_FREQ_MAX)), 0.0f, EQ_FREQ_MAX);
    e->gain = e->level >= 0.0f ? (1.0f + e->level * 3.0f / 12.0f) : (1.0f + e->level * 0.8f / 12.0f);
    if (s_host && s_host->cosf && s_host->tanf) {
        e->d = s_host->cosf(2.0f * EQ_PI * freq / EQ_RATE);
        e->c = s_host->tanf(EQ_PI * bw / EQ_RATE);
        e->on = 1;
    } else {
        e->d = 1.0f;
        e->c = 1.0f;   // a finite value; the band is skipped
        e->on = 0;
    }
}

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    (void)scratch_mem;
    (void)scratch_size;
    s_host = host;
    for (int c = 0; c < 2; c++) {
        for (int b = 0; b < 3; b++) {
            s_z[c][b][0] = 0.0f;
            s_z[c][b][1] = 0.0f;
        }
    }
    static const float freq0[3] = { 100.0f, 1000.0f, 12000.0f };
    for (int b = 0; b < 3; b++) {
        s_band[b].level = 0.0f;
        s_band[b].freq = freq0[b];
        s_band[b].q = 1.0f;
        band_update(b);
    }
}

void plugin_set_param(uint8_t index, float value) {
    if (index >= 9) {
        return;
    }
    const int b = index / 3;
    if (index % 3 == 0) {
        s_band[b].level = value;
    } else if (index % 3 == 1) {
        s_band[b].freq = value;
    } else {
        s_band[b].q = value;
    }
    band_update(b);
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!out || samples == 0) {
        return 0;
    }
    if (channels > 2) channels = 2;
    for (uint8_t ch = 0; ch < channels; ch++) {
        if (!out[ch]) continue;
        const int16_t* src = (in != 0) ? in[ch] : 0;
        for (uint32_t i = 0; i < samples; i++) {
            float x = (src ? (float)src[i] : 0.0f) / 32767.0f;
            for (int b = 0; b < 3; b++) {
                const EqBand* e = &s_band[b];
                float* z = s_z[ch][b];
                if (e->level == 0.0f || !e->on) {   // skipped: the state is cleared, so a later level change starts without a transient
                    z[0] = 0.0f;
                    z[1] = 0.0f;
                    continue;
                }
                const float a = (1.0f - e->c) / (1.0f + e->c);
                const float dd = e->d * (1.0f + a);
                const float w = x + dd * z[0] - a * z[1];
                const float y = w * a - dd * z[0] + z[1];
                z[1] = z[0];
                z[0] = w;
                x = 0.5f * (y + x + e->gain * (x - y));
            }
            x = clampf(x, -1.0f, 1.0f);
            out[ch][i] = (int16_t)(x * 32767.0f);
        }
    }
    return (int32_t)samples;
}
