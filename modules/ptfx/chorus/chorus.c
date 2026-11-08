// SPDX-License-Identifier: MIT
//
// Chorus: our rebuild, as a .ptfx plugin, of the Sample Editor effect of the same name of Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce),
// commit 7fd6c2c3002a8f690b5341c486ba69606077b513, core/effector/mtEffectChorus.cpp/.h (parameter ranges: mtSampleEditorEngine.cpp, chorusParams). The algorithm is the
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
// What is the original's, kept as it is: a ring of L samples per channel (L = the Length value in 1/44.1 ms units divided by 2, the original's own arithmetic, so the Length shown in ms
// is twice the real span of the ring), V voices read from the ring at spacing int(L / (V - 1)) - 1 samples counting back from the newest sample, the V taps summed in 32 bits and divided
// by V (integer division); with one voice the sound passes unchanged. The ring starts empty (zeros) and its write index starts at 0 and is advanced before each write. What is changed:
// (1) the Length knob is in milliseconds (3..92, the original's 128..4096 units divided by 44.1, so 2.9..92.9) because the SDK has no display multiplier, and the units are
// int(ms * 44.1) as before; (2) the original processes whole groups of 128 frames and drops the last partial group of the selection (its result is shorter, up to 127 frames); here every
// frame is processed and the result is as long as the selection; (3) the ring of each channel is a part of the scratch (8192 bytes: two channels of 2048 samples); (4) the defaults
// (30 ms, 4 voices) are ours, the original has none.
//
// ---- end of the notice ----
// PTFX_NAME: Chorus
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: EFFECT, STEREO
// PTFX_SCRATCH: 8192

#include "tracker_plugin.h"

// Knobs: Length, Voices
const PluginParam PLUGIN_PARAMS[2] = {
    PTFX_PARAM_INT("Length", 3.0f, 92.0f, 30.0f, 1.0f, "ms"),
    PTFX_PARAM_INT("Voices", 1.0f, 8.0f, 4.0f, 1.0f, "")
};

#define CHORUS_MAX_RING 2048u   // samples per channel (the original's 4096-unit maximum / 2)

static int16_t*  s_ring = 0;          // scratch: channel ch uses [ch * cap, ch * cap + L)
static uint32_t  s_ring_total = 0;    // samples in the scratch
static uint32_t  s_len = 661;         // L, samples
static uint32_t  s_voices = 4;
static uint32_t  s_circ[2] = { 0, 0 };   // ring write index per channel

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    (void)host;
    s_len = 661;
    s_voices = 4;
    s_circ[0] = s_circ[1] = 0;
    s_ring = 0;
    s_ring_total = 0;
    if (scratch_mem && scratch_size >= 2 * 64u) {
        s_ring = (int16_t*)scratch_mem;   // the host zeroes the scratch before init
        s_ring_total = scratch_size / 2;
    }
}

void plugin_set_param(uint8_t index, float value) {
    if (index == 0) {
        int32_t ms = (int32_t)(value + 0.5f);
        if (ms < 3) ms = 3;
        if (ms > 92) ms = 92;
        s_len = (uint32_t)((int32_t)((float)ms * 44.1f) / 2);
    } else if (index == 1) {
        int32_t v = (int32_t)(value + 0.5f);
        if (v < 1) v = 1;
        if (v > 8) v = 8;
        s_voices = (uint32_t)v;
    }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!out || samples == 0) {
        return 0;
    }
    if (channels > 2) channels = 2;
    uint32_t cap = (channels > 0) ? s_ring_total / channels : 0;
    if (cap > CHORUS_MAX_RING) cap = CHORUS_MAX_RING;
    uint32_t len = (s_len < cap) ? s_len : cap;
    const uint32_t voices = s_voices;
    if (!s_ring || len < 2) {
        for (uint8_t ch = 0; ch < channels; ch++) {   // no scratch: pass through
            if (!out[ch]) continue;
            const int16_t* src = (in != 0) ? in[ch] : 0;
            for (uint32_t i = 0; i < samples; i++) out[ch][i] = src ? src[i] : 0;
        }
        return (int32_t)samples;
    }
    // the tap spacing; len >= 2 and voices <= 8, so (len / (voices - 1)) - 1 can be 0 for tiny rings only
    int32_t spacing = 0;
    if (voices > 1) {
        spacing = (int32_t)(len / (voices - 1)) - 1;
        if (spacing < 0) spacing = 0;
    }
    for (uint8_t ch = 0; ch < channels; ch++) {
        if (!out[ch]) continue;
        const int16_t* src = (in != 0) ? in[ch] : 0;
        int16_t* ring = s_ring + (uint32_t)ch * cap;
        uint32_t circ = s_circ[ch];
        if (circ >= len) circ = 0;
        for (uint32_t i = 0; i < samples; i++) {
            const int16_t x = src ? src[i] : 0;
            if (++circ >= len) circ = 0;
            ring[circ] = x;
            if (voices <= 1) {
                out[ch][i] = x;
                continue;
            }
            int32_t sum = 0;
            int32_t c = (int32_t)circ;
            for (uint32_t k = 0; k < voices; k++) {
                sum += ring[c];
                c -= spacing;
                if (c < 0) c += (int32_t)len;
            }
            out[ch][i] = (int16_t)(sum / (int32_t)voices);
        }
        s_circ[ch] = circ;
    }
    return (int32_t)samples;
}
