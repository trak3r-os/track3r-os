// SPDX-License-Identifier: MIT
//
// Taken as is from Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce), commit 7fd6c2c3002a8f690b5341c486ba69606077b513,
// file editor-fx-sdk/effects/stereo_to_mono.c. Component approved by the maintainer 2026-11-08 (OS-PTFXPACK); see THIRD_PARTY.md and LICENSE-tracker-ce.txt
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
// PTFX_NAME: Stereo to Mono
// PTFX_AUTHOR: Polyend
// PTFX_FLAGS: STEREO, CONVERT_TO_MONO
// PTFX_SCRATCH: 0

#include "tracker_plugin.h"

// Knobs: Source Channel (String List) & Gain Trim
const PluginParam PLUGIN_PARAMS[2] = {
    PTFX_PARAM_LIST("Source", 0, "Merge L+R|Left Only|Right Only|Side L-R"),
    PTFX_PARAM_FLOAT("Gain", -12.0f, 6.0f, 0.0f, 0.5f, "dB")
};

static const PtfxHostApi* s_host = 0;
static uint8_t s_mode = 0;
static float   s_gain = 1.0f;

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    (void)scratch_mem;
    (void)scratch_size;
    s_host = host;
    s_mode = 0;
    s_gain = 1.0f;
}

void plugin_set_param(uint8_t index, float value) {
    if (index == 0) {
        s_mode = (uint8_t)value;
    } else if (index == 1) {
        if (value == 0.0f) {
            s_gain = 1.0f;
        } else if (s_host && s_host->powf) {
            s_gain = s_host->powf(10.0f, value / 20.0f);
        }
    }
}

// Processes 1 channel on MK66, or converts 2 channels on Tracker Mini / Tracker+
int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!in || !out || !in[0] || !out[0]) {
        return 0;
    }

    // Single-channel source or identical input channels (mono)
    if (channels < 2 || !in[1] || in[0] == in[1]) {
        for (uint32_t i = 0; i < samples; i++) {
            int16_t s = (s_gain == 1.0f) ? in[0][i] : (int16_t)SATURATE_16BIT((int32_t)(in[0][i] * s_gain));
            out[0][i] = s;
            if (channels > 1 && out[1]) {
                out[1][i] = s;
            }
        }
        return samples;
    }

    const int16_t* left  = in[0];
    const int16_t* right = in[1];
    int16_t* out_l = out[0];
    int16_t* out_r = out[1] ? out[1] : out[0];

    for (uint32_t i = 0; i < samples; i++) {
        int32_t mono_val = left[i];

        switch (s_mode) {
            case 0: // Merge (L + R) / 2
                mono_val = ((int32_t)left[i] + (int32_t)right[i]) / 2;
                break;
            case 1: // Keep Left Only (discard Right channel)
                mono_val = left[i];
                break;
            case 2: // Keep Right Only (discard Left channel)
                mono_val = right[i];
                break;
            case 3: // Side / Difference (L - R) / 2
                mono_val = ((int32_t)left[i] - (int32_t)right[i]) / 2;
                break;
            default:
                mono_val = left[i];
                break;
        }

        int16_t pcm = (s_gain == 1.0f) ? (int16_t)mono_val : (int16_t)SATURATE_16BIT((int32_t)(mono_val * s_gain));
        out_l[i] = pcm;
        out_r[i] = pcm;
    }

    return samples;
}
