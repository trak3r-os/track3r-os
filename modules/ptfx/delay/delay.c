// SPDX-License-Identifier: MIT
//
// Delay: our rebuild, as a .ptfx plugin, of the Sample Editor effect of the same name of Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce),
// commit 7fd6c2c3002a8f690b5341c486ba69606077b513, core/effector/mtEffectDelay.cpp/.h (parameter ranges: mtSampleEditorEngine.cpp, delayParams). The algorithm is the
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
// What is the original's: the sound. The original adds voices to the sample: voice i is the sample delayed and scaled by feedback^i, summed and clipped to 16 bits, as many voices as
// it takes for feedback^i to fall to 1% (at most 98 more), and the result is that much longer. Its onset has a quirk that is kept: voice 1 starts ONE sample after the dry sound
// (delay 1), voice 2 at T + 1, voice 3 at 2T + 1 and so on, where T = int(time ms * 44.1). The time range 5..5000 ms (step 5) and the feedback range 1..95 % are the original's.
// What is changed: (1) the original reads the whole selection once per voice; a plugin sees blocks, so this is the same sum written as the recursive comb
// y[n] = x[n] + fb * c[n], c[n] = x[n-1] + fb * c[n-T], whose voices have exactly the delays and gains above, with one ring of T samples per channel in the scratch (441000 bytes, 10 s of
// mono); (2) the ring stores 16-bit saturated samples, the original sums in 32 bits and clips once, so a very loud, very resonant input clips a little differently; (3) the recursion
// has no voice cap, the original stops at 98 more voices (the next voice is below -40 dB); (4) the tail asked from the host is (voices - 1) * T + 1 samples, as the original's length,
// but at most 882000 (20 s), the original is limited to 1966080 samples in all; (5) the original's minimum time is 1 ms in steps of 5 (1, 6, 11 ...), here 5, 10, 15 ...; (6) on a host with 2 channels each channel
// gets half of the ring, so T is limited to 110250 samples (2.5 s); MK66 has one channel.
//
// ---- end of the notice ----
// PTFX_NAME: Delay
// PTFX_AUTHOR: track3r-os
// PTFX_FLAGS: EFFECT, STEREO
// PTFX_SCRATCH: 441000

#include "tracker_plugin.h"

// Knobs: Feedback, Time
const PluginParam PLUGIN_PARAMS[2] = {
    PTFX_PARAM_INT("Fdbk", 1.0f, 95.0f, 50.0f, 1.0f, "%"),
    PTFX_PARAM_INT("Time", 5.0f, 5000.0f, 250.0f, 5.0f, "ms")
};

#define DELAY_MAX_RING 220500u   // samples in the scratch (441000 bytes)
#define DELAY_MAX_VOICES 99u     // the original's MAX_DELAY_VOICE_NUMBER
#define DELAY_MAX_TAIL 882000u   // 20 s

static const PtfxHostApi* s_host = 0;
static int16_t*  s_ring = 0;     // scratch: ring of c[n]; channel ch uses [ch * s_cap, ch * s_cap + T)
static uint32_t  s_ring_total = 0;
static uint32_t  s_t = 11025;    // T, samples
static float     s_fb = 0.5f;
static uint32_t  s_w = 0;        // write position inside a channel's ring, shared by the channels
static int16_t   s_prev[2] = { 0, 0 };  // x[n-1] per channel

static void update_tail(void) {
    if (!s_host || !s_host->set_extra_samples) {
        return;
    }
    // The original's calcLastVoiceIdx: voices until the gain is down to 1 %.
    uint32_t n = 0;
    float gain = 1.0f;
    while (gain > 0.01f) {
        gain *= s_fb;
        n++;
        if (n == DELAY_MAX_VOICES) break;
    }
    const uint32_t last = n - 1;   // n >= 1: the loop runs once
    uint32_t tail = 0;
    if (last >= 1) {
        tail = (last - 1) * s_t + 1;
        if (tail > DELAY_MAX_TAIL) tail = DELAY_MAX_TAIL;
    }
    s_host->set_extra_samples(0, tail);
}

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    s_host = host;
    s_t = 11025;
    s_fb = 0.5f;
    s_w = 0;
    s_prev[0] = 0;
    s_prev[1] = 0;
    s_ring = 0;
    s_ring_total = 0;
    if (scratch_mem && scratch_size >= 2 * 1024u) {
        s_ring = (int16_t*)scratch_mem;   // the host zeroes the scratch before init
        s_ring_total = scratch_size / 2;
        if (s_ring_total > DELAY_MAX_RING) s_ring_total = DELAY_MAX_RING;
    }
    update_tail();
}

void plugin_set_param(uint8_t index, float value) {
    if (index == 0) {
        float fb = (float)(int32_t)(value + 0.5f) * 0.01f;
        if (fb < 0.01f) fb = 0.01f;
        if (fb > 0.95f) fb = 0.95f;
        s_fb = fb;
    } else if (index == 1) {
        int32_t ms = (int32_t)(value + 0.5f);
        if (ms < 1) ms = 1;
        if (ms > 5000) ms = 5000;
        s_t = (uint32_t)((float)ms * 44.1f);
        if (s_t < 1) s_t = 1;
        if (s_t > DELAY_MAX_RING) s_t = DELAY_MAX_RING;
    } else {
        return;
    }
    update_tail();
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    if (!out || samples == 0) {
        return 0;
    }
    if (channels > 2) channels = 2;
    const uint32_t cap = (channels > 0) ? s_ring_total / channels : 0;
    if (!s_ring || cap == 0) {
        for (uint8_t ch = 0; ch < channels; ch++) {   // no scratch: pass through
            if (!out[ch]) continue;
            const int16_t* src = (in != 0) ? in[ch] : 0;
            for (uint32_t i = 0; i < samples; i++) out[ch][i] = src ? src[i] : 0;
        }
        return (int32_t)samples;
    }

    const uint32_t t = (s_t < cap) ? s_t : cap;
    const float fb = s_fb;
    uint32_t w_end = s_w;
    if (w_end >= t) w_end = 0;
    for (uint8_t ch = 0; ch < channels; ch++) {
        if (!out[ch]) continue;
        const int16_t* src = (in != 0) ? in[ch] : 0;
        int16_t* ring = s_ring + (uint32_t)ch * cap;
        int16_t prev = s_prev[ch];
        uint32_t w = (s_w < t) ? s_w : 0;
        for (uint32_t i = 0; i < samples; i++) {
            const int16_t x = src ? src[i] : 0;
            const int32_t c = (int32_t)prev + (int32_t)(fb * (float)ring[w]);   // c[n] = x[n-1] + fb * c[n-T]
            const int16_t cs = (int16_t)SATURATE_16BIT(c);
            ring[w] = cs;
            out[ch][i] = (int16_t)SATURATE_16BIT((int32_t)x + (int32_t)(fb * (float)cs));   // y[n] = x[n] + fb * c[n]
            prev = x;
            if (++w >= t) w = 0;
        }
        s_prev[ch] = prev;
        w_end = w;
    }
    s_w = w_end;
    return (int32_t)samples;
}
