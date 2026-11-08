// SPDX-License-Identifier: MIT
//
// Taken as is from Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce), commit 7fd6c2c3002a8f690b5341c486ba69606077b513,
// file editor-fx-sdk/effects/stereo_delay.c. Component approved by the maintainer 2026-11-08 (OS-PTFXPACK); see THIRD_PARTY.md and LICENSE-tracker-ce.txt
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
// PTFX_NAME: Stereo Delay
// PTFX_AUTHOR: Polyend
// PTFX_FLAGS: EFFECT, STEREO
// PTFX_SCRATCH: 176404

#include "tracker_plugin.h"

// Knobs: Delay Time, Feedback, Dry/Wet Mix, Tail Mode
const PluginParam PLUGIN_PARAMS[4] = {
    PTFX_PARAM_INT("Time", 10.0f, 1000.0f, 250.0f, 10.0f, "ms"),
    PTFX_PARAM_INT("Fdbk",  0.0f,   95.0f,  40.0f,  1.0f, "%"),
    PTFX_PARAM_INT("Mix",   0.0f,  100.0f,  50.0f,  1.0f, "%"),
    PTFX_PARAM_LIST("Tail", 0, "Ring Out|Cut Off")
};

// 1.0 second max stereo delay line @ 44.1 kHz = 44,100 samples * 2 channels = 176.4 KB
#define MAX_DELAY_SAMPLES 44100
#define MAX_TAIL_SAMPLES  220500 // 5.0 seconds max tail

typedef struct {
    int16_t  buffer[2][MAX_DELAY_SAMPLES];
    uint32_t write_idx;
} DelayState;

static const PtfxHostApi* s_host = 0;
static DelayState* s_state = 0;
static uint32_t    s_delay_len = 11025; // 250 ms default
static float       s_feedback = 0.4f;
static float       s_mix = 0.5f;
static uint8_t     s_tail_mode = 0;     // 0 = Ring Out, 1 = Cut Off

static void update_tail_length(void) {
    if (!s_host || !s_host->set_extra_samples) {
        return;
    }
    if (s_tail_mode != 0 || s_mix <= 0.0f || s_delay_len == 0) {
        s_host->set_extra_samples(0, 0);
        return;
    }

    uint32_t repeats = 1;
    float gain = s_mix;
    while (gain * s_feedback >= 0.01f && repeats < 16) {
        gain *= s_feedback;
        repeats++;
    }

    uint32_t tail_samples = repeats * s_delay_len;
    if (tail_samples > MAX_TAIL_SAMPLES) {
        tail_samples = MAX_TAIL_SAMPLES;
    }
    s_host->set_extra_samples(0, tail_samples);
}

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    s_host = host;
    s_delay_len = 11025;
    s_feedback = 0.4f;
    s_mix = 0.5f;
    s_tail_mode = 0;
    if (scratch_mem && scratch_size >= sizeof(DelayState)) {
        s_state = (DelayState*)scratch_mem;
        s_state->write_idx = 0;
        if (host && host->memset) {
            host->memset(s_state->buffer, 0, sizeof(s_state->buffer));
        }
    } else {
        s_state = 0; // Fallback / pass-through if scratch allocation failed
    }
    update_tail_length();
}

void plugin_set_param(uint8_t index, float value) {
    if (index == 0)      s_delay_len = (uint32_t)(value * 44.1f + 0.5f);
    else if (index == 1) s_feedback  = value * 0.01f;
    else if (index == 2) s_mix       = value * 0.01f;
    else if (index == 3) s_tail_mode = (value >= 0.5f) ? 1 : 0;

    if (s_delay_len >= MAX_DELAY_SAMPLES) s_delay_len = MAX_DELAY_SAMPLES - 1;
    if (s_delay_len < 1) s_delay_len = 1;

    update_tail_length();
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!out || !out[0] || samples == 0) {
        return 0;
    }

    if (!s_state) {
        for (uint8_t ch = 0; ch < channels; ch++) {
            if (!out[ch]) continue;
            const int16_t* src = (in != 0) ? in[ch] : 0;
            for (uint32_t i = 0; i < samples; i++) {
                out[ch][i] = src ? src[i] : 0;
            }
        }
        return (int32_t)samples;
    }

    uint32_t delay_len = s_delay_len;
    if (delay_len >= MAX_DELAY_SAMPLES) delay_len = MAX_DELAY_SAMPLES - 1;
    if (delay_len < 1) delay_len = 1;

    for (uint32_t i = 0; i < samples; i++) {
        uint32_t r_idx = (s_state->write_idx + MAX_DELAY_SAMPLES - delay_len) % MAX_DELAY_SAMPLES;

        for (uint8_t ch = 0; ch < channels && ch < 2; ch++) {
            if (!out[ch]) continue;
            const int16_t* src = (in != 0) ? in[ch] : 0;

            int16_t delayed_sample = s_state->buffer[ch][r_idx];
            int32_t wet = (int32_t)delayed_sample;
            int32_t dry = src ? (int32_t)src[i] : 0;

            int32_t output = (int32_t)((1.0f - s_mix) * (float)dry + s_mix * (float)wet);
            out[ch][i] = (int16_t)SATURATE_16BIT(output);

            int32_t fb_sample = dry + (int32_t)((float)wet * s_feedback);
            s_state->buffer[ch][s_state->write_idx] = (int16_t)SATURATE_16BIT(fb_sample);
        }

        s_state->write_idx = (s_state->write_idx + 1) % MAX_DELAY_SAMPLES;
    }
    return (int32_t)samples;
}
