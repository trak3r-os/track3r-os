// SPDX-License-Identifier: MIT
//
// Taken as is from Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce), commit 7fd6c2c3002a8f690b5341c486ba69606077b513,
// file editor-fx-sdk/effects/analog_kick_gen.c. Component approved by the maintainer 2026-11-08 (OS-PTFXPACK); see THIRD_PARTY.md and LICENSE-tracker-ce.txt
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
// PTFX_NAME: Analog Kick Gen
// PTFX_AUTHOR: Polyend
// PTFX_FLAGS: GENERATOR
// PTFX_SCRATCH: 0

#include "tracker_plugin.h"

static const PtfxHostApi* s_host = 0;
static float s_start_pitch = 140.0f;
static float s_end_pitch   = 45.0f;
static float s_decay_ms    = 350.0f;
static float s_click_lvl   = 0.7f;

const PluginParam PLUGIN_PARAMS[4] = {
    { "TopFreq",  60.0f, 250.0f, 140.0f, 1.0f, "Hz", PARAM_INT },
    { "SubFreq",  30.0f,  80.0f,  45.0f, 1.0f, "Hz", PARAM_INT },
    { "Decay",   100.0f, 900.0f, 350.0f, 10.0f,"ms", PARAM_INT },
    { "Click",     0.0f,   1.0f,   0.7f, 0.05f,"",   PARAM_FLOAT }
};

static uint32_t s_current_sample = 0;
static float s_phase = 0.0f;

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    (void)scratch_mem;
    (void)scratch_size;
    s_host = host;
    s_current_sample = 0;
    s_phase = 0.0f;
}

void plugin_set_param(uint8_t index, float value) {
    if (index == 0) s_start_pitch = value;
    else if (index == 1) s_end_pitch = value;
    else if (index == 2) s_decay_ms = value;
    else if (index == 3) s_click_lvl = value;
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    (void)in;
    if (!s_host) return 0;

    const float sample_rate = (float)s_host->sample_rate;
    float decay_samples = (s_decay_ms * 0.001f) * sample_rate;

    for (uint32_t i = 0; i < samples; i++) {
        float t = (float)s_current_sample / decay_samples;
        float sample_val = 0.0f;

        if (t <= 1.0f) {
            float pitch_env = s_host->expf(-t * 6.0f);
            float freq = s_end_pitch + (s_start_pitch - s_end_pitch) * pitch_env;
            float amp_env = s_host->expf(-t * 3.5f);

            float click = 0.0f;
            if (s_current_sample < (uint32_t)(0.005f * sample_rate)) {
                click = ((float)(s_host->rand() % 2000) / 1000.0f - 1.0f) * s_click_lvl;
            }

            s_phase += (freq * 2.0f * 3.14159265f) / sample_rate;
            if (s_phase > 2.0f * 3.14159265f) s_phase -= 2.0f * 3.14159265f;

            sample_val = (s_host->sinf(s_phase) + click) * amp_env;
        }

        int16_t pcm = (int16_t)SATURATE_16BIT((int32_t)(sample_val * 32767.0f));
        for (uint8_t ch = 0; ch < channels; ch++) {
            out[ch][i] = pcm;
        }
        s_current_sample++;
    }
    return samples;
}
