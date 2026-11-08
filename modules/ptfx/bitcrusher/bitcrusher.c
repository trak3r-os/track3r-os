// SPDX-License-Identifier: MIT
//
// Bitcrusher: our rebuild, as a .ptfx plugin, of the Sample Editor effect of the same name of Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce),
// commit 7fd6c2c3002a8f690b5341c486ba69606077b513, core/effector/mtEffectBitcrusher.cpp (parameter ranges: mtSampleEditorEngine.cpp, bitcrusherParams). The algorithm is the
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
// What is the original's: bit reduction by an arithmetic shift pair ((x >> (16 - bits)) << (16 - bits), rounding toward minus infinity), sample-rate reduction by holding the first
// sample of each group of n frames, n = int(44117.64706 / rate + 0.5) limited to 1..64, bits 1..16 and rate 500..44100 Hz in steps of 100. What is changed: (1) the original restarts the
// groups at the start of each 8192-frame block it processes; here a group continues across blocks (the hold counter and the held value are kept per channel), so the result does not depend on
// the block size the host picks; (2) n is computed in single precision (the original uses a double constant); (3) the defaults (8 bits, 8000 Hz) are ours, the original has none. No scratch,
// no host extra samples: the result is as long as the selection.
//
// ---- end of the notice ----
// PTFX_NAME: Bitcrusher
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: EFFECT, STEREO
// PTFX_SCRATCH: 0

#include "tracker_plugin.h"

// Knobs: Bits, Rate
const PluginParam PLUGIN_PARAMS[2] = {
    PTFX_PARAM_INT("Bits", 1.0f, 16.0f, 8.0f, 1.0f, ""),
    PTFX_PARAM_INT("Rate", 500.0f, 44100.0f, 8000.0f, 100.0f, "Hz")
};

static uint8_t  s_bits = 8;
static uint32_t s_step = 6;               // n: frames per held sample
static int16_t  s_held[2] = { 0, 0 };     // the held sample per channel
static uint32_t s_left[2] = { 0, 0 };     // frames left to hold it

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    (void)scratch_mem;
    (void)scratch_size;
    (void)host;
    s_bits = 8;
    s_step = 6;
    s_held[0] = s_held[1] = 0;
    s_left[0] = s_left[1] = 0;
}

void plugin_set_param(uint8_t index, float value) {
    if (index == 0) {
        int32_t b = (int32_t)(value + 0.5f);
        if (b < 1) b = 1;
        if (b > 16) b = 16;
        s_bits = (uint8_t)b;
    } else if (index == 1) {
        float rate = (float)(int32_t)(value + 0.5f);
        if (rate < 1.0f) rate = 1.0f;
        int32_t n = (int32_t)(44117.64706f / rate + 0.5f);
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        s_step = (uint32_t)n;
    }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!out || samples == 0) {
        return 0;
    }
    if (channels > 2) channels = 2;
    const uint32_t shift = 16u - s_bits;
    const uint32_t step = s_step;
    for (uint8_t ch = 0; ch < channels; ch++) {
        if (!out[ch]) continue;
        const int16_t* src = (in != 0) ? in[ch] : 0;
        int16_t held = s_held[ch];
        uint32_t left = s_left[ch];
        for (uint32_t i = 0; i < samples; i++) {
            if (left == 0) {
                const int32_t x = src ? src[i] : 0;
                held = (int16_t)((x >> shift) * (int32_t)(1u << shift));   // a multiply: a left shift of a negative value is not defined in C
                left = step;
            }
            out[ch][i] = held;
            left--;
        }
        s_held[ch] = held;
        s_left[ch] = left;
    }
    return (int32_t)samples;
}
