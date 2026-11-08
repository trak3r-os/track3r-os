// SPDX-License-Identifier: MIT
//
// Taken as is from Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce), commit 7fd6c2c3002a8f690b5341c486ba69606077b513,
// file editor-fx-sdk/effects/chiptune_gen.c. Component approved by the maintainer 2026-11-08 (OS-PTFXPACK); see THIRD_PARTY.md and LICENSE-tracker-ce.txt
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
// PTFX_NAME: Chiptune Voice Gen
// PTFX_AUTHOR: Polyend
// PTFX_FLAGS: GENERATOR
// PTFX_SCRATCH: 0

#include "tracker_plugin.h"

// 8 Parameters across Page 1 and Page 2 showcasing String Lists + Numeric Bars
const PluginParam PLUGIN_PARAMS[8] = {
    // Page 1
    PTFX_PARAM_LIST ("Voice",  0, "Kick|Snare|HiHat|Pulse12|Pulse25|Pulse50|Triangle|NESNoise"),
    PTFX_PARAM_INT  ("Note",   24.0f, 84.0f,   60.0f,  1.0f,  "st"),
    PTFX_PARAM_LIST ("Arp",    0, "Off|Octave|Fifth|Major|Minor|Sus4|Maj7|Min7"),
    PTFX_PARAM_INT  ("Speed",  10.0f, 60.0f,   50.0f,  5.0f,  "Hz"),
    // Page 2 (More)
    PTFX_PARAM_INT  ("Decay",  30.0f, 1500.0f, 350.0f, 10.0f, "ms"),
    PTFX_PARAM_FLOAT("Sweep", -24.0f, 24.0f,   0.0f,   1.0f,  "st"),
    PTFX_PARAM_LIST ("Crush",  0, "Off|8-Bit|6-Bit|4-Bit|2-Bit"),
    PTFX_PARAM_LIST ("Length", 0, "DecayLen|SingleCyc")
};

static const PtfxHostApi* s_host = 0;
static int      s_voice = 0;
static float    s_note = 60.0f;
static int      s_arp = 0;
static float    s_speed = 50.0f;
static float    s_decay_ms = 350.0f;
static float    s_sweep_st = 0.0f;
static int      s_crush = 0;
static int      s_length_mode = 0;

static uint32_t s_pos = 0;
static float    s_phase = 0.0f;
static uint16_t s_lfsr = 0x7FFF;

// Chord semitone tables (up to 4 notes per arpeggio pattern)
static const int8_t kArpChords[8][4] = {
    { 0,  0,  0,  0 }, // Off
    { 0, 12,  0, 12 }, // Octave
    { 0,  7, 12,  7 }, // Fifth
    { 0,  4,  7, 12 }, // Major
    { 0,  3,  7, 12 }, // Minor
    { 0,  5,  7, 12 }, // Sus4
    { 0,  4,  7, 11 }, // Maj7
    { 0,  3,  7, 10 }  // Min7
};

void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    (void)scratch_mem;
    (void)scratch_size;
    s_host = host;
    s_pos = 0;
    s_phase = 0.0f;
    s_lfsr = 0x7FFF;
}

void plugin_set_param(uint8_t index, float value) {
    switch (index) {
        case 0: s_voice       = (int)(value + 0.5f); break;
        case 1: s_note        = value; break;
        case 2: s_arp         = (int)(value + 0.5f); break;
        case 3: s_speed       = value; break;
        case 4: s_decay_ms    = value; break;
        case 5: s_sweep_st    = value; break;
        case 6: s_crush       = (int)(value + 0.5f); break;
        case 7: s_length_mode = (int)(value + 0.5f); break;
        default: break;
    }
}

static inline float note_to_freq(float note, const PtfxHostApi* host) {
    if (host && host->powf) {
        return 440.0f * host->powf(2.0f, (note - 69.0f) * (1.0f / 12.0f));
    }
    return 261.6256f;
}

static inline uint16_t step_lfsr(uint16_t lfsr) {
    uint16_t bit = ((lfsr >> 0) ^ (lfsr >> 1)) & 1u;
    return (uint16_t)((lfsr >> 1) | (bit << 14));
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    (void)in;
    if (!out || !out[0]) {
        return 0;
    }

    float sr = (s_host && s_host->sample_rate > 0) ? (float)s_host->sample_rate : 44100.0f;
    float base_freq = note_to_freq(s_note, s_host);

    uint32_t total_len;
    if (s_length_mode == 1 && s_voice >= 3 && s_voice <= 6) {
        // Single-cycle oscillator mode for pitched waveforms
        total_len = (uint32_t)(sr / base_freq + 0.5f);
        if (total_len < 16) total_len = 16;
    } else {
        total_len = (uint32_t)(s_decay_ms * (sr * 0.001f));
        if (total_len < 64) total_len = 64;
    }

    if (s_pos >= total_len) {
        return 0;
    }

    uint32_t to_gen = total_len - s_pos;
    if (to_gen > samples) {
        to_gen = samples;
    }

    uint32_t arp_step_samples = (s_speed > 1.0f) ? (uint32_t)(sr / s_speed) : 882;
    if (arp_step_samples < 64) arp_step_samples = 64;

    int arp_idx = (s_arp >= 0 && s_arp < 8) ? s_arp : 0;

    for (uint32_t i = 0; i < to_gen; i++) {
        uint32_t cur = s_pos + i;
        float progress = (float)cur / (float)total_len;
        float env = (s_length_mode == 1 && s_voice >= 3 && s_voice <= 6) ? 1.0f : (1.0f - progress);
        env = env * env; // Exponential-like amplitude decay

        float sample_f = 0.0f;

        if (s_voice == 0) {
            // 0: 8-bit Triangle Pitch-Swept Kick
            float freq = 52.0f + 220.0f * (1.0f - progress) * (1.0f - progress) * (1.0f - progress);
            s_phase += freq / sr;
            if (s_phase >= 1.0f) s_phase -= 1.0f;
            // Quantized 4-bit NES triangle
            float tri = (s_phase < 0.5f) ? (s_phase * 4.0f - 1.0f) : (3.0f - s_phase * 4.0f);
            tri = (float)((int)(tri * 7.5f)) / 7.5f;
            if (cur < 180) {
                s_lfsr = step_lfsr(s_lfsr);
                tri = ((s_lfsr & 1) ? 0.9f : -0.9f);
            }
            sample_f = tri * env;
        } else if (s_voice == 1) {
            // 1: Chiptune Snare (Body tone + clocked LFSR noise)
            float body_freq = 180.0f * (1.0f - progress * 0.5f);
            s_phase += body_freq / sr;
            if (s_phase >= 1.0f) s_phase -= 1.0f;
            if ((cur & 3) == 0) {
                s_lfsr = step_lfsr(s_lfsr);
            }
            float body = (s_phase < 0.5f ? 0.8f : -0.8f) * (1.0f - progress) * (1.0f - progress);
            float noise = ((s_lfsr & 1) ? 0.75f : -0.75f) * env;
            sample_f = (cur < total_len / 4) ? (body * 0.6f + noise * 0.6f) : noise;
        } else if (s_voice == 2) {
            // 2: Metallic Chiptune HiHat
            if ((cur & 1) == 0) {
                s_lfsr = step_lfsr(s_lfsr);
            }
            float h_env = (1.0f - progress);
            h_env = h_env * h_env * h_env;
            sample_f = ((s_lfsr & 1) ? 0.8f : -0.8f) * h_env;
        } else {
            // Pitched Oscillator Voices (Pulse 12.5%, Pulse 25%, Pulse 50%, Triangle, NES Noise)
            uint32_t step_num = (cur / arp_step_samples) & 3u;
            int8_t arp_semi = (s_length_mode == 1) ? 0 : kArpChords[arp_idx][step_num];
            float sweep_semi = (s_length_mode == 1) ? 0.0f : (s_sweep_st * progress);
            float inst_freq = note_to_freq(s_note + (float)arp_semi + sweep_semi, s_host);

            s_phase += inst_freq / sr;
            while (s_phase >= 1.0f) s_phase -= 1.0f;

            switch (s_voice) {
                case 3: // Pulse 12.5%
                    sample_f = (s_phase < 0.125f) ? 0.85f : -0.85f;
                    break;
                case 4: // Pulse 25%
                    sample_f = (s_phase < 0.25f) ? 0.85f : -0.85f;
                    break;
                case 5: // Pulse 50% (Square)
                    sample_f = (s_phase < 0.50f) ? 0.85f : -0.85f;
                    break;
                case 6: // NES 4-bit stepped Triangle
                {
                    float tri = (s_phase < 0.5f) ? (s_phase * 4.0f - 1.0f) : (3.0f - s_phase * 4.0f);
                    sample_f = (float)((int)(tri * 7.5f)) / 7.5f;
                    break;
                }
                case 7: // Clocked NES Pitch Noise
                default:
                    if (s_phase < (inst_freq / sr)) {
                        s_lfsr = step_lfsr(s_lfsr);
                    }
                    sample_f = (s_lfsr & 1) ? 0.8f : -0.8f;
                    break;
            }
            sample_f *= env;
        }

        int16_t pcm = SATURATE_16BIT((int32_t)(sample_f * 32767.0f));

        // Optional Bitcrush post-quantization
        if (s_crush == 1)      pcm &= (int16_t)0xFF00; // 8-bit
        else if (s_crush == 2) pcm &= (int16_t)0xFC00; // 6-bit
        else if (s_crush == 3) pcm &= (int16_t)0xF000; // 4-bit
        else if (s_crush == 4) pcm &= (int16_t)0xC000; // 2-bit

        for (uint8_t ch = 0; ch < channels; ch++) {
            if (out[ch]) {
                out[ch][i] = pcm;
            }
        }
    }

    s_pos += to_gen;
    return (int32_t)to_gen;
}
