// SPDX-License-Identifier: MIT
//
// Overdrive: our rebuild, as a .ptfx plugin, of the Sample Editor effect of the same name of Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce),
// commit 7fd6c2c3002a8f690b5341c486ba69606077b513, core/effector/mtEffectOverdrive.cpp (parameter ranges: mtSampleEditorEngine.cpp, overdriveParams). The algorithm is the
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
// What is the original's: the three waveshapers (quadratic soft clip with integer maths, the exponential curve (e - exp(1 - 4 g |x|)) / e and the reciprocal curve 1 - 1 / (10 g |x| + 1)),
// the gain scaling per type (1, 4, 10) and the gain range 0.01..5. What is changed: (1) the original's Type knob (0..2) is a list here ("Quad", "Exp", "Recip": the same order);
// (2) |gain * x| of the reciprocal shaper is a float absolute value (the original calls abs(), which on some toolchains truncates to int); (3) exp() is the host's expf, a single-precision
// function (the original's `exp` of a float is the double-precision one); (4) the defaults (gain 2.0, Quad) are ours, the original has none; (5) the output is saturated and the channels
// are independent, so any channel count and any block size give the same result. No state, no scratch, no host extra samples: the result is as long as the selection.
//
// ---- end of the notice ----
// PTFX_NAME: Overdrive
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: EFFECT, STEREO
// PTFX_SCRATCH: 0

#include "tracker_plugin.h"

// Knobs: Gain, Type
const PluginParam PLUGIN_PARAMS[2] = {
    PTFX_PARAM_FLOAT("Gain", 0.01f, 5.0f, 2.0f, 0.01f, ""),
    PTFX_PARAM_LIST("Type", 0, "Quad|Exp|Recip")
};

#define OD_QUADRATIC   0
#define OD_EXPONENTIAL 1
#define OD_RECIPROCAL  2

#define OD_E 2.71828182845904523536f

static const PtfxHostApi* s_host = 0;
static float   s_gain = 2.0f;
static uint8_t s_type = OD_QUADRATIC;

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    (void)scratch_mem;
    (void)scratch_size;
    s_host = host;
    s_gain = 2.0f;
    s_type = OD_QUADRATIC;
}

void plugin_set_param(uint8_t index, float value) {
    if (index == 0) {
        if (value < 0.01f) value = 0.01f;
        if (value > 5.0f) value = 5.0f;
        s_gain = value;
    } else if (index == 1) {
        s_type = (value >= 1.5f) ? OD_RECIPROCAL : ((value >= 0.5f) ? OD_EXPONENTIAL : OD_QUADRATIC);
    }
}

static int16_t shape_quadratic(int16_t x, float gain) {
    const int32_t v = (int32_t)((float)x * gain);
    const int32_t a = v >= 0 ? v : -v;
    int32_t y;
    if (a > (int32_t)2 * 32767 / 3) {
        y = v >= 0 ? 32767 : -32768;                         // y = +-1
    } else if (a > 32767 / 3) {
        const int32_t t = 2 * 32767 - 3 * a;                 // y = (3 - (2 - 3x)^2) / 3
        y = (3 * 32767 - (t * t / 0x8000)) / 3;
        if (v < 0) y = -y;
    } else {
        y = 2 * v;                                           // y = 2x
    }
    return (int16_t)y;
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!out || samples == 0) {
        return 0;
    }
    const uint8_t type = s_type;
    float gain = s_gain;
    if (type == OD_EXPONENTIAL) gain *= 4.0f;
    else if (type == OD_RECIPROCAL) gain *= 10.0f;

    for (uint8_t ch = 0; ch < channels; ch++) {
        if (!out[ch]) continue;
        const int16_t* src = (in != 0) ? in[ch] : 0;
        for (uint32_t i = 0; i < samples; i++) {
            const int16_t x = src ? src[i] : 0;
            int16_t y;
            if (type == OD_QUADRATIC) {
                y = shape_quadratic(x, gain);
            } else {
                const float f = (float)x / 32767.0f;
                if (type == OD_EXPONENTIAL) {
                    const float sign = f >= 0.0f ? 1.0f : -1.0f;
                    const float arg = 1.0f - gain * f * sign;
                    const float e = (s_host && s_host->expf) ? s_host->expf(arg) : 0.0f;
                    y = (int16_t)SATURATE_16BIT((int32_t)((sign * (OD_E - e) / OD_E) * 32767.0f));
                } else {
                    const float sign = f == 0.0f ? 0.0f : (f >= 0.0f ? 1.0f : -1.0f);
                    const float m = gain * f;
                    const float a = m >= 0.0f ? m : -m;
                    y = (int16_t)SATURATE_16BIT((int32_t)(sign * (1.0f - 1.0f / (a + 1.0f)) * 32767.0f));
                }
            }
            out[ch][i] = y;
        }
    }
    return (int32_t)samples;
}
