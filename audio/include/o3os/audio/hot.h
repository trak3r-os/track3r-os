// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Placement of the real-time path (ADR 0013 D7: the hot kernels and the audio HAL/ISR path run from SRAM_L, zero wait
// states on the code bus). platform/mk66 defines O3OS_AUDIO_RAMFUNC for this library, and a function marked O3OS_AUDIO_HOT
// then lands in `.ramfunc_l`, which the reset sequence copies from flash before any interrupt is enabled (the same
// placement as O3OS_RAMFUNC_L in platform/mk66/sections.h, spelled here because this library may not include a platform).
// `long_call` because flash and SRAM_L are too far apart for a plain BL; `noinline` so the placement survives optimisation.
// On the host and on QEMU the macro is empty.
#if defined(O3OS_AUDIO_RAMFUNC) && O3OS_AUDIO_RAMFUNC
#define O3OS_AUDIO_HOT [[gnu::section(".ramfunc_l"), gnu::noinline, gnu::long_call]]
#else
#define O3OS_AUDIO_HOT
#endif

// A hot function that also inlines every call it makes (GCC's `flatten`, except the callees that are themselves noinline): for ISR-path code that calls small helpers of
// a lower layer (core::SpscRing::pop, size) which -Os would otherwise leave out of line, in flash, where the check of tools/check_ramfunc.py rightly refuses a call
// from SRAM_L. Where this is used the helpers it flattens are copied into the function (a few instructions each).
#if defined(O3OS_AUDIO_RAMFUNC) && O3OS_AUDIO_RAMFUNC
#define O3OS_AUDIO_HOT_FLAT [[gnu::section(".ramfunc_l"), gnu::noinline, gnu::long_call, gnu::flatten]]
#else
#define O3OS_AUDIO_HOT_FLAT
#endif

// OS-MEM1: render-context code that runs per EVENT (a parameter change, a note start), not per sample or per block, and so lives in flash to keep SRAM_L for
// the per-block kernels. `long_call` is required: the caller is in SRAM_L and flash is too far for a plain BL, so the call is made through a register (`blx rN`),
// which tools/check_ramfunc.py allows as it does any indirect call. THE EXCEPTION LIST of the "SRAM_L calls nothing in flash" rule is exactly the functions
// marked O3OS_AUDIO_COLD (grep for it); each states its call rate beside the declaration, and nothing marked COLD may be called more than once per event or
// per block per strip/voice that changed. Per-sample and per-block kernels stay O3OS_AUDIO_HOT. On the host and on QEMU the macro is empty.
// Named exceptions to the call rate (a flash call on the per-block path, each with its rate): VoicePool::shape_span (once a span a sampler voice that shapes; calls shape_pcm), and, OS-BUG-MODDRIVE,
// Engine::shape_modules (once a block, only while some track holds the Overdrive or Bit depth FX or the layer's rows: the gate is one inline test of VoicePool::shape_mask) with VoicePool::shape_track
// and VoicePool::shape_pcm (once each per shaped module track in that call). OS-CEWTENV room: TestTone::generate and TestTone::generate_noise (flash; once a block per strip whose test tone is audible: the O3OS_AUDIO_TEST image, or a console
// test-source ParamChange; Graph::render pays one `blx` while the tone is faded in, and nothing in a release image), and Graph::script_tone (at a change of the tone script's target only). OS-STEREO (c): VoicePool::render_voice_st (once per span of a voice that holds a side block; the stereo loops, S's tail and
// the calls below it are flash), VoicePool::end_block_st (once a block while a voice holds a block or the last block fed one), Engine::feed_sides (the same condition) and kernel::gain_pan_st (once per track that has S
// in the block); nothing is called while no voice is stereo. OS-MODRATE: ModuleVoices::render_rated (once per segment of an instrument module whose RATE is not FULL: the hold expansion; a FULL instance pays one
// byte test in render()).
#if defined(O3OS_AUDIO_RAMFUNC) && O3OS_AUDIO_RAMFUNC
#define O3OS_AUDIO_COLD [[gnu::noinline, gnu::long_call]]
#else
#define O3OS_AUDIO_COLD
#endif

// For the few tiny ISR-path methods that must be folded into their caller (which is itself in SRAM_L). The GNU spelling, not [[gnu::]], so that it
// may stand anywhere in the specifiers (`static O3OS_AUDIO_INLINE ...`).
#define O3OS_AUDIO_INLINE __attribute__((always_inline)) inline
