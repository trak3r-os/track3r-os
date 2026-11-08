// SPDX-License-Identifier: MIT
//
// Taken as is from Polyend Tracker Community Edition (tracker-ce, github.com/polyend/tracker-ce), commit 7fd6c2c3002a8f690b5341c486ba69606077b513,
// file editor-fx-sdk/tracker_plugin.h. Component approved by the maintainer 2026-11-08 (OS-PTFXPACK); see THIRD_PARTY.md and LICENSE-tracker-ce.txt
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
#ifndef TRACKER_PLUGIN_H_
#define TRACKER_PLUGIN_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTFX_MAGIC 0x58465450 // "PTFX" in Little Endian

// Plugin Type & Capabilities Flags
typedef enum {
    PTFX_FLAG_EFFECT          = 0x0000, // Transforms existing audio (in_buffer -> out_buffer)
    PTFX_FLAG_GENERATOR       = 0x0001, // Generates audio from scratch into out_buffer
    PTFX_FLAG_STEREO          = 0x0002, // Supports dual-channel processing (Tracker Mini/+)
    PTFX_FLAG_WAVETABLE       = 0x0004, // Specialized Polyend Tracker wavetable generator
    PTFX_FLAG_CONVERT_TO_MONO = 0x0008, // Converts stereo instrument to true mono (reclaims 50% RAM)
    PTFX_FLAG_IMX_ONLY        = 0x0010  // Code > 32 KB or requires Cortex-M7 / stereo
} PtfxFlags;

typedef enum {
    PARAM_FLOAT = 'f', // Rendered as decimal (e.g. 1.25)
    PARAM_INT   = 'd', // Rendered as integer (e.g. 42)
    PARAM_LIST  = 'l'  // Rendered as scrollable string list (cList)
} PtfxParamDisplayType;

// Exactly 32 bytes; all float fields are 4-byte aligned (offsets +8, +12, +16, +20)
// When display_type == PARAM_LIST ('l'):
//   unit[0..1] = uint16_t list_offset (little-endian offset into code payload)
//   unit[2]    = uint8_t  item_count  (number of null-terminated option strings)
typedef struct __attribute__((packed, aligned(4))) {
    char    label[8];      // +0x00: Knob label (max 7 chars + '\0', e.g. "Drive\0")
    float   min_val;       // +0x08: Minimum value (4-byte aligned!)
    float   max_val;       // +0x0C: Maximum value (4-byte aligned!)
    float   default_val;   // +0x10: Initial load value (4-byte aligned!)
    float   step;          // +0x14: Value delta per encoder tick (4-byte aligned!)
    char    unit[6];       // +0x18: Unit string (max 5 chars + '\0') or list_offset/item_count
    uint8_t display_type;  // +0x1E: 'f', 'd', or 'l' (PtfxParamDisplayType)
    uint8_t padding;       // +0x1F: Pad struct to 32 bytes
} PtfxParamDef;

// Exactly 472 bytes (88-byte prefix + 12 * 32-byte params)
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t     magic;                 // +0x00: Must equal PTFX_MAGIC ("PTFX")
    uint16_t     format_version;        // +0x04: Current version: 1
    uint16_t     flags;                 // +0x06: Bitfield of PtfxFlags
    char         display_name[32];      // +0x08: Pretty UI Name: e.g. "Stereo to Mono (Save RAM)\0"
    char         author[16];            // +0x28: Author/handle: e.g. "Polyend\0"
    uint8_t      num_params;            // +0x38: Number of active parameters (0 to 12)
    uint8_t      reserved[3];           // +0x39: Reserved / 4-byte alignment padding
    uint32_t     scratch_size;          // +0x3C: Requested working RAM in bytes (delays, reverbs, tables)
    uint32_t     code_offset;           // +0x40: Byte offset in file where machine code starts (472)
    uint32_t     code_size;             // +0x44: Byte size of binary payload (.text + .rodata + .data)
    uint32_t     bss_size;              // +0x48: Byte size of zero-initialized .bss following code_size
    uint32_t     init_offset;           // +0x4C: Byte offset of plugin_init() relative to code base
    uint32_t     set_param_offset;      // +0x50: Byte offset of plugin_set_param() relative to code base
    uint32_t     process_block_offset;  // +0x54: Byte offset of plugin_process_block() relative to code base
    PtfxParamDef params[12];            // +0x58: Up to 12 physical encoder parameters (384 bytes)
} PtfxHeader;

// Host API Function Pointer Table
typedef struct {
    uint32_t sample_rate; // 44100
    // Standard Math (Single-Precision Hardware FPU)
    float (*sinf)(float);
    float (*cosf)(float);
    float (*tanf)(float);
    float (*tanhf)(float);
    float (*powf)(float, float);
    float (*expf)(float);
    float (*sqrtf)(float);
    float (*logf)(float);
    float (*log10f)(float);
    float (*fminf)(float, float);
    float (*fmaxf)(float, float);
    float (*fabsf)(float);
    // Memory & Utility Primitives
    void* (*memcpy)(void*, const void*, size_t);
    void* (*memset)(void*, int, size_t);
    int   (*rand)(void);
    // Optional Sample Length Extension (prepend / append samples to selection)
    void  (*set_extra_samples)(uint32_t prepend_samples, uint32_t append_samples);
} PtfxHostApi;

// Plugin Function Pointer Types
typedef void (*PtfxInitFunc)(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host);
typedef void (*PtfxSetParamFunc)(uint8_t index, float value);
typedef int32_t (*PtfxProcessBlockFunc)(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels);

// Helper for saturating audio to 16-bit signed integer
#if defined(__arm__)
#define SATURATE_16BIT(x) __builtin_arm_ssat((x), 16)
#else
static inline int16_t SATURATE_16BIT(int32_t x) {
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}
#endif

// Convenience struct for plugin authors
typedef struct {
    const char*          label;
    float                min;
    float                max;
    float                default_val;
    float                step;
    const char*          unit;
    PtfxParamDisplayType type;
} PluginParam;

// Convenience macros for defining parameters in PLUGIN_PARAMS[]
#define PTFX_PARAM_FLOAT(lbl, min_v, max_v, def_v, stp, unt) \
    { (lbl), (min_v), (max_v), (def_v), (stp), (unt), PARAM_FLOAT }

#define PTFX_PARAM_INT(lbl, min_v, max_v, def_v, stp, unt) \
    { (lbl), (min_v), (max_v), (def_v), (stp), (unt), PARAM_INT }

// Pipe-separated option labels in the unit field when type == PARAM_LIST:
// e.g. PTFX_PARAM_LIST("Mode", 0, "LPF 24|LPF 12|BPF|HPF|Notch")
#define PTFX_PARAM_LIST(lbl, def_idx, options_pipe_sep) \
    { (lbl), 0.0f, 0.0f, (float)(def_idx), 1.0f, (options_pipe_sep), PARAM_LIST }

#ifdef __cplusplus
}
#endif

#endif /* TRACKER_PLUGIN_H_ */
