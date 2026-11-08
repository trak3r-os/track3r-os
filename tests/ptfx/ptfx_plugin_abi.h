/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Our own small ABI header for the OS-CEPTFX S0 test plugins (tests/ptfx/*.c), written from the format facts of
 * docs/specs/OS-CEPTFX.md 2.1 (cited there from tracker-ce@7fd6c2c:editor-fx-sdk/tracker_plugin.h). No tracker-ce
 * code is copied: the types below restate the ABI in our words, and the offsets are asserted at compile time.
 *
 * Used only by tools/ptfx_pack.py, which cross-compiles the test plugins for the Cortex-M4F of the MK66.
 */
#ifndef O3OS_TESTS_PTFX_PLUGIN_ABI_H
#define O3OS_TESTS_PTFX_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

/* File header: magic "PTFX" at +0, format_version u16 at +4, flags u16 at +6 (spec 2.1). */
#define PTFX_TEST_MAGIC 0x58465450u
#define PTFX_TEST_VERSION 1u

/* Flags (spec 2.1): EFFECT is 0. */
#define PTFX_TEST_FLAG_GENERATOR 0x0001u
#define PTFX_TEST_FLAG_STEREO 0x0002u
#define PTFX_TEST_FLAG_WAVETABLE 0x0004u
#define PTFX_TEST_FLAG_CONVERT_TO_MONO 0x0008u
#define PTFX_TEST_FLAG_IMX_ONLY 0x0010u

/* One of the 12 parameter records of the header: 32 bytes at +88 + 32*i (offsets asserted below). */
typedef struct {
  char label[8];        /* +0x00: the row's caption, 7 chars + NUL */
  float min_val;        /* +0x08: lowest value the encoder reaches */
  float max_val;        /* +0x0C: highest value the encoder reaches */
  float default_val;    /* +0x10: value the plugin starts with */
  float step;           /* +0x14: one encoder tick's worth of change */
  char unit[6];         /* +0x18: trailing text after the value, 5 chars + NUL; for a list ('l') instead:
                           +0 u16 offset of the option strings inside the payload, +2 u8 how many */
  uint8_t display_type; /* +0x1E: how the row is drawn: 'f' two decimals, 'd' whole number, 'l' the list */
  uint8_t padding;      /* +0x1F: unused, keeps the record at 32 bytes */
} PtfxTestParamDef;

_Static_assert(sizeof(PtfxTestParamDef) == 32, "a parameter is 32 bytes");
_Static_assert(offsetof(PtfxTestParamDef, min_val) == 8, "min at +8");
_Static_assert(offsetof(PtfxTestParamDef, max_val) == 12, "max at +12");
_Static_assert(offsetof(PtfxTestParamDef, default_val) == 16, "default at +16");
_Static_assert(offsetof(PtfxTestParamDef, step) == 20, "step at +20");
_Static_assert(offsetof(PtfxTestParamDef, unit) == 24, "unit at +24");
_Static_assert(offsetof(PtfxTestParamDef, display_type) == 30, "display type at +30");

/*
 * What the loader passes to init() as its third argument, in the fixed field order of spec 2.1: first the
 * sample rate in Hz (0 meaning 44,100), then the single-precision math functions the plugin may call, then the
 * memory and rand primitives, and last the callback that extends the selection. 68 bytes on the target, where
 * a pointer is 4 bytes (asserted below).
 */
typedef struct {
  uint32_t sample_rate;
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
  void* (*memcpy)(void*, const void*, size_t);
  void* (*memset)(void*, int, size_t);
  int (*rand)(void);
  void (*set_extra_samples)(uint32_t prepend_samples, uint32_t append_samples);
} PtfxTestHostApi;

#if defined(__arm__)
_Static_assert(sizeof(void*) == 4, "the host table is laid out for the 32-bit target");
_Static_assert(sizeof(PtfxTestHostApi) == 68, "the host table is 68 bytes");
_Static_assert(offsetof(PtfxTestHostApi, sinf) == 4, "sinf at +4");
_Static_assert(offsetof(PtfxTestHostApi, rand) == 60, "rand at +60");
_Static_assert(offsetof(PtfxTestHostApi, set_extra_samples) == 64, "set_extra_samples at +64");
#endif

/* The three functions every plugin exports (spec 2.1); on the hard-float AAPCS the float argument of
   set_param travels in register s0, the rest in r0-r3. */
typedef void (*PtfxTestInitFn)(void* scratch, uint32_t scratch_size, const PtfxTestHostApi* host);
typedef void (*PtfxTestSetParamFn)(uint8_t index, float value);
typedef int32_t (*PtfxTestProcessBlockFn)(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels);

/* Clamps a wide value into the 16-bit sample range, for the test plugins that scale samples. */
static inline int16_t ptfx_test_sat16(int32_t x) {
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return (int16_t)x;
}

#endif /* O3OS_TESTS_PTFX_PLUGIN_ABI_H */
