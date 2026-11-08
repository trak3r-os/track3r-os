// SPDX-License-Identifier: MIT
//
// Flanger: our rebuild, as a .ptfx plugin, of the Sample Editor effect of the same name of Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce),
// commit 7fd6c2c3002a8f690b5341c486ba69606077b513, core/effector/mtEffectFlanger.cpp/.h (parameter ranges: mtSampleEditorEngine.cpp, flangerParams). The algorithm is the
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
// is twice the real span of the ring); a read tap at offset O = Offset% of L (integer) behind the newest sample, moved by +-Depth samples by a sine LFO whose phase is a 31-bit
// accumulator (Rate Hz times 2^31 / 44117.64706 per sample, wrapping at 2^31); the output is (dry + tap) / 2 where the tap is a linear interpolation between two neighbouring ring
// samples with a fraction taken from bits 1..15 of the phase accumulator (that is the original's, odd as it is: the fraction runs fast and is not tied to the LFO); an offset of 0
// (Offset% of a short ring rounds down to 0) passes the sound unchanged; the ring starts empty and its write index is advanced before each write. What is changed: (1) the Length knob is
// in milliseconds (3..92: the original's 128..4096 units divided by 44.1) because the SDK has no display multiplier; (2) the original processes whole groups of 128 frames and drops the
// last partial group of the selection; here every frame is processed and the result is as long as the selection; (3) the LFO sine is the host's sinf, not the 16-bit CMSIS table sine, and the
// rate increment is computed in single precision; (4) the original wraps the tap index once, which for an offset near L plus a negative sweep reads before the start of its buffer;
// here the index is wrapped fully into 0..L-1; (5) the ring of each channel is a part of the scratch (8192 bytes: two channels of 2048 samples); (6) the defaults (10 ms, 50 %, depth 4,
// 0.5 Hz) are ours, the original has none.
//
// ---- end of the notice ----
// PTFX_NAME: Flanger
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: EFFECT, STEREO
// PTFX_SCRATCH: 8192

#include "tracker_plugin.h"

// Knobs: Length, Offset, Depth, Rate
const PluginParam PLUGIN_PARAMS[4] = {
    PTFX_PARAM_INT("Length", 3.0f, 92.0f, 10.0f, 1.0f, "ms"),
    PTFX_PARAM_INT("Offset", 1.0f, 100.0f, 50.0f, 1.0f, "%"),
    PTFX_PARAM_INT("Depth", 1.0f, 8.0f, 4.0f, 1.0f, ""),
    PTFX_PARAM_FLOAT("Rate", 0.1f, 5.0f, 0.5f, 0.1f, "Hz")
};

#define FLANGER_MAX_RING 2048u   // samples per channel (the original's 4096-unit maximum / 2)
#define FLANGER_PI2 6.28318530717958647692f

static const PtfxHostApi* s_host = 0;
static int16_t*  s_ring = 0;          // scratch: channel ch uses [ch * cap, ch * cap + L)
static uint32_t  s_ring_total = 0;    // samples in the scratch
static int32_t   s_len = 220;         // L, samples
static int32_t   s_offset_pct = 50;
static int32_t   s_depth = 4;
static int32_t   s_rate_incr = 0;     // LFO phase step per sample, 31-bit accumulator
static uint32_t  s_circ[2] = { 0, 0 };    // ring write index per channel
static uint32_t  s_phase[2] = { 0, 0 };   // LFO phase accumulator per channel

static int32_t rate_to_incr(float hz) {
    return (int32_t)(hz * (2147483648.0f / 44117.64706f));
}

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    s_host = host;
    s_len = 220;
    s_offset_pct = 50;
    s_depth = 4;
    s_rate_incr = rate_to_incr(0.5f);
    s_circ[0] = s_circ[1] = 0;
    s_phase[0] = s_phase[1] = 0;
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
        s_len = (int32_t)((int32_t)((float)ms * 44.1f) / 2);
    } else if (index == 1) {
        int32_t p = (int32_t)(value + 0.5f);
        if (p < 1) p = 1;
        if (p > 100) p = 100;
        s_offset_pct = p;
    } else if (index == 2) {
        int32_t d = (int32_t)(value + 0.5f);
        if (d < 1) d = 1;
        if (d > 8) d = 8;
        s_depth = d;
    } else if (index == 3) {
        if (value < 0.1f) value = 0.1f;
        if (value > 5.0f) value = 5.0f;
        s_rate_incr = rate_to_incr(value);
    }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!out || samples == 0) {
        return 0;
    }
    if (channels > 2) channels = 2;
    uint32_t cap = (channels > 0) ? s_ring_total / channels : 0;
    if (cap > FLANGER_MAX_RING) cap = FLANGER_MAX_RING;
    int32_t len = (s_len < (int32_t)cap) ? s_len : (int32_t)cap;
    if (!s_ring || len < 2 || !s_host || !s_host->sinf) {
        for (uint8_t ch = 0; ch < channels; ch++) {   // no scratch: pass through
            if (!out[ch]) continue;
            const int16_t* src = (in != 0) ? in[ch] : 0;
            for (uint32_t i = 0; i < samples; i++) out[ch][i] = src ? src[i] : 0;
        }
        return (int32_t)samples;
    }
    int32_t offset = (int32_t)(((float)s_offset_pct * (float)len) / 100.0f);
    if (offset >= len) offset = len - 1;
    const int32_t depth = s_depth;
    const uint32_t incr = (uint32_t)s_rate_incr;
    for (uint8_t ch = 0; ch < channels; ch++) {
        if (!out[ch]) continue;
        const int16_t* src = (in != 0) ? in[ch] : 0;
        int16_t* ring = s_ring + (uint32_t)ch * cap;
        int32_t circ = (int32_t)s_circ[ch];
        uint32_t phase = s_phase[ch];
        if (circ >= len) circ = 0;
        for (uint32_t i = 0; i < samples; i++) {
            const int16_t x = src ? src[i] : 0;
            if (++circ >= len) circ = 0;
            ring[circ] = x;
            if (offset == 0) {
                out[ch][i] = x;
                continue;
            }
            const float s = s_host->sinf(FLANGER_PI2 * (float)((phase >> 16) & 0x7fffu) / 32768.0f);
            int32_t sweep = (int32_t)(s * 32767.0f);                    // Q15 sine
            const int32_t shift = (sweep * depth) >> 15;                // arithmetic shift, as the original
            int32_t idx = circ - (offset + shift);
            while (idx < 0) idx += len;
            while (idx >= len) idx -= len;
            int32_t idx1 = (sweep < 0) ? idx - 1 : idx + 1;
            if (idx1 < 0) idx1 += len;
            if (idx1 >= len) idx1 -= len;
            const int32_t frac = (int32_t)((phase >> 1) & 0x7fffu);
            const int32_t interp = (((int32_t)ring[idx1] - (int32_t)ring[idx]) * frac) >> 15;
            out[ch][i] = (int16_t)(((int32_t)ring[circ] + (int32_t)ring[idx] + interp) / 2);
            phase += incr;
            if (phase & 0x80000000u) phase &= 0x7fffffffu;
        }
        s_circ[ch] = (uint32_t)circ;
        s_phase[ch] = phase;
    }
    return (int32_t)samples;
}
