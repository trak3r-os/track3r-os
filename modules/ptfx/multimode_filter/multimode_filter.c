// SPDX-License-Identifier: MIT
//
// Taken as is from Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce), commit 7fd6c2c3002a8f690b5341c486ba69606077b513,
// file editor-fx-sdk/effects/multimode_filter.c. Component approved by the maintainer 2026-11-08 (OS-PTFXPACK); see THIRD_PARTY.md and LICENSE-tracker-ce.txt
// (Parts 1 and 2 of the upstream licence, which every copy must carry).
//
// Copyright (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors
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
// ---- end of the notice: from the next line the file is the original, byte for byte ----
// PTFX_NAME: Multi-Mode Filter
// PTFX_AUTHOR: Polyend
// PTFX_FLAGS: EFFECT, STEREO
// PTFX_SCRATCH: 0

#include "tracker_plugin.h"

// Filter & Drive parameters using String Lists + Float Bars
const PluginParam PLUGIN_PARAMS[4] = {
    PTFX_PARAM_LIST ("Mode",   0, "LPF 24dB|LPF 12dB|BPF 12dB|HPF 12dB|Notch|Peak"),
    PTFX_PARAM_INT  ("Cutoff", 40.0f, 16000.0f, 1200.0f, 50.0f, "Hz"),
    PTFX_PARAM_INT  ("Reso",   0.0f,  95.0f,    30.0f,   5.0f,  "%"),
    PTFX_PARAM_LIST ("Drive",  0, "Clean|SoftSat|WarmTube|HardClip|Foldback")
};

static const PtfxHostApi* s_host = 0;
static int   s_mode = 0;
static float s_cutoff = 1200.0f;
static float s_reso = 0.30f;
static int   s_drive = 0;

// Chamberlin / Cytomic SVF state per channel (2 cascaded stages for 24dB mode)
static float s_ic1eq[2][2];
static float s_ic2eq[2][2];

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    (void)scratch_mem;
    (void)scratch_size;
    s_host = host;
    s_mode = 0;
    s_cutoff = 1200.0f;
    s_reso = 0.30f;
    s_drive = 0;

    for (int ch = 0; ch < 2; ch++) {
        for (int st = 0; st < 2; st++) {
            s_ic1eq[ch][st] = 0.0f;
            s_ic2eq[ch][st] = 0.0f;
        }
    }
}

void plugin_set_param(uint8_t index, float value) {
    switch (index) {
        case 0:
            s_mode = (int)(value + 0.5f);
            break;
        case 1:
            s_cutoff = value;
            break;
        case 2:
            s_reso = value * 0.01f;
            break;
        case 3:
            s_drive = (int)(value + 0.5f);
            break;
        default:
            break;
    }
}

static inline float apply_drive(float x, int drive_mode, const PtfxHostApi* host) {
    switch (drive_mode) {
        case 0: // Clean
            return x;
        case 1: // SoftSat
        {
            float d = x * 1.8f;
            return (host && host->tanhf) ? host->tanhf(d) : (d / (1.0f + (d < 0.0f ? -d : d)));
        }
        case 2: // WarmTube (asymmetric saturation)
        {
            float d = x * 2.2f + 0.15f;
            float y = (host && host->tanhf) ? host->tanhf(d) : (d / (1.0f + (d < 0.0f ? -d : d)));
            return y - 0.14f;
        }
        case 3: // HardClip
        {
            float d = x * 2.5f;
            if (d > 0.85f) d = 0.85f;
            if (d < -0.85f) d = -0.85f;
            return d * 1.15f;
        }
        case 4: // Foldback
        {
            float d = x * 2.4f;
            if (d > 1.0f) d = 2.0f - d;
            else if (d < -1.0f) d = -2.0f - d;
            if (d > 1.0f) d = 1.0f;
            if (d < -1.0f) d = -1.0f;
            return d;
        }
        default:
            return x;
    }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!in || !out || !in[0] || !out[0]) {
        return 0;
    }

    float sr = (s_host && s_host->sample_rate > 0) ? (float)s_host->sample_rate : 44100.0f;
    float fc = s_cutoff;
    if (fc < 20.0f) fc = 20.0f;
    if (fc > sr * 0.45f) fc = sr * 0.45f;

    // Cytomic TPT SVF coefficients
    float g = 3.14159265f * (fc / sr);
    if (s_host && s_host->tanf) {
        g = s_host->tanf(g);
    }
    // Map resonance 0..0.95 to damping factor k = 2.0 .. 0.1
    float k = 2.0f - (s_reso * 1.9f);
    if (k < 0.1f) k = 0.1f;

    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;

    uint8_t num_ch = (channels > 2) ? 2 : channels;

    for (uint8_t ch = 0; ch < num_ch; ch++) {
        const int16_t* src = in[ch] ? in[ch] : in[0];
        int16_t* dst = out[ch];
        if (!dst) continue;

        float ic1_0 = s_ic1eq[ch][0];
        float ic2_0 = s_ic2eq[ch][0];
        float ic1_1 = s_ic1eq[ch][1];
        float ic2_1 = s_ic2eq[ch][1];

        for (uint32_t i = 0; i < samples; i++) {
            float v0 = (float)src[i] * (1.0f / 32768.0f);
            v0 = apply_drive(v0, s_drive, s_host);

            // Stage 1 SVF
            float v3 = v0 - ic2_0;
            float v1 = a1 * ic1_0 + a2 * v3;
            float v2 = ic2_0 + a2 * ic1_0 + a3 * v3;
            ic1_0 = 2.0f * v1 - ic1_0;
            ic2_0 = 2.0f * v2 - ic2_0;

            float y = v2;
            switch (s_mode) {
                case 0: // LPF 24dB (2nd cascaded SVF stage)
                {
                    float u3 = v2 - ic2_1;
                    float u1 = a1 * ic1_1 + a2 * u3;
                    float u2 = ic2_1 + a2 * ic1_1 + a3 * u3;
                    ic1_1 = 2.0f * u1 - ic1_1;
                    ic2_1 = 2.0f * u2 - ic2_1;
                    y = u2;
                    break;
                }
                case 1: // LPF 12dB
                    y = v2;
                    break;
                case 2: // BPF 12dB
                    y = v1;
                    break;
                case 3: // HPF 12dB
                    y = v0 - k * v1 - v2;
                    break;
                case 4: // Notch
                    y = v0 - k * v1;
                    break;
                case 5: // Peak / Bell
                    y = v0 + v1 * 1.5f;
                    break;
                default:
                    y = v2;
                    break;
            }

            int32_t pcm = (int32_t)(y * 32767.0f);
            dst[i] = SATURATE_16BIT(pcm);
        }

        s_ic1eq[ch][0] = ic1_0;
        s_ic2eq[ch][0] = ic2_0;
        s_ic1eq[ch][1] = ic1_1;
        s_ic2eq[ch][1] = ic2_1;
    }

    return samples;
}
