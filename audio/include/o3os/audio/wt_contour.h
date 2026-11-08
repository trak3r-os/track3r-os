// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-CEWTENV W2 (docs/specs/OS-CEWTENV.md sections 3.3, F3, F4): the wavetable contour bank and its evaluator, a pure part of the
// audio library (no hardware, no heap, no double, no exceptions). A port of Polyend Tracker Community Edition's
// `core/source/mtWavetableEnvelopes.cpp` at tracker-ce@7fd6c2c (MIT, approved by the maintainer for this component only, Q1 a; the
// copyright line and the licence notice are in THIRD_PARTY.md): the 256 procedural contours and the warp law are ported with the
// maths identical, rewritten in this tree's style; the `.wav` contour loader of the original is NOT ported (that is slice W8).
//
// THE BANK (F3): 256 contours of 128 unipolar int16 points each (64 KiB), 0..127 the factory contours and 128..255 the user fallbacks,
// made procedurally by wt_make (the main loop only, about 40k cycles a contour). The storage is the CALLER'S: on the MK66 it lives in the
// SDRAM arena (the `wt_contour` region of platform/mk66 mem_map.h, kWtContourBase); on the host a static buffer in the test. wt_make
// fills one contour and marks it in the made-mask, so the render never reads an unmade contour (the main loop makes the contour before
// it stages a descriptor that names it).
//
// THE WARP LAW (F4): a bend byte packs two warp nibbles, X in the high one, Y in the low one (wt_pack_warp / wt_warp_x / wt_warp_y): a
// nibble 0..7 is the warp +0..+7, 9..15 is -7..-1, 8 is 0. Warp X bends the phase by p (1 + k) / (2 p k + 1 - k) with k = wx / 9 (one
// division); warp Y applies the same law to the level (a second division); a warp Y of -7 inverts the level. Between the contour's
// points the evaluator is the 128-point linear interpolation, the level scaled by 1/32767 to 0..1.
//
// PLACEMENT: wt_eval is render-context code and stays in FLASH (O3OS_AUDIO_COLD, never SRAM_L: ADR 0013 D7's hot budget has no room for
// it), called a few times per block per WT voice (docs/specs/OS-CEWTENV.md 3.10). wt_make runs in the main loop only, plain flash.

#include <o3os/audio/hot.h>

#include <cstdint>

namespace o3os::audio {

inline constexpr unsigned kWtContourCount = 256u;        // 0..127 factory, 128..255 user fallbacks (F3)
inline constexpr unsigned kWtContourFactoryCount = 128u;
inline constexpr unsigned kWtContourLen = 128u;          // points per contour
inline constexpr unsigned kWtBankBytes = kWtContourCount * kWtContourLen * 2u;  // 64 KiB (int16 points)

// The bank: caller-provided storage (the SDRAM arena on the MK66, a static buffer in a host test). 256 x 128 int16.
struct WtBank {
  std::int16_t contour[kWtContourCount][kWtContourLen];
};

// The made-mask: one bit per contour, set by wt_make (32 B).
struct WtMadeMask {
  std::uint32_t word[kWtContourCount / 32u];
};

// Fills bank.contour[shape] (shape held to 0..255) with the procedural contour and sets its bit in `made`. Main loop only.
void wt_make(WtBank& bank, unsigned shape, WtMadeMask& made) noexcept;

// Whether wt_make has made this contour.
bool wt_made(const WtMadeMask& made, unsigned shape) noexcept;

// The contour of `bank` for `shape` (no bounds check: the caller holds shape to 0..255).
const std::int16_t* wt_contour(const WtBank& bank, unsigned shape) noexcept;

// F4: the level 0..1 of `contour` (128 int16 points, as made by wt_make) at `phase` 0..1 (held) with the warp byte `bend`
// (X high nibble, Y low nibble). Flash (O3OS_AUDIO_COLD); a few calls per block per WT voice.
O3OS_AUDIO_COLD float wt_eval(const std::int16_t* contour, float phase, std::uint8_t bend) noexcept;

// The bound bank (OS-CEWTENV W3a): the render's view of the bank. The main loop binds the bank's storage once (wt_bind, on the MK66 the
// SDRAM arena's `wt_contour` region) and makes the contour a descriptor names before it stages the descriptor (wt_make_bound); the render
// evaluates through wt_eval_shape. An unbound bank, or a contour not made yet, evaluates to 0.0f: never a wild read.
void wt_bind(WtBank& bank) noexcept;  // main loop, once; clears the made-mask
void wt_unbind(const WtBank& bank) noexcept;  // OS-CEWTENV W3c fold: when `bank` is the bound one, unbinds it (a host object that is going away; the unit's lives forever)
void wt_make_bound(unsigned shape) noexcept;  // main loop: makes `shape` (held to 0..255) into the bound bank when not made yet
O3OS_AUDIO_COLD float wt_eval_shape(unsigned shape, float phase, std::uint8_t bend) noexcept;  // render (flash): the bound bank's contour

// ---- OS-CEWTENV W3c: the baked contours (docs/specs/OS-CEWTENV.md Q3 a, 3.3) --------------------------------------------------------------------------------
// A slot's warps are applied ONCE, when its descriptor is made (the main loop): wt_bake evaluates the contour at the warp byte at 128 points (phase i / 127) into a
// baked contour of the same format and returns its id, 1..kWtBakeCount; the render then evaluates it with no warp (no division). The same (shape, warp X, warp Y) is baked
// once (the key is the canonical pair, so nibble 8 and nibble 0 share an entry); id 0 is "not baked": no warp, or the pool is full, or no bake pool is bound, and the render
// applies the warps live (wt_eval_shape), so the sound is right whatever the pool holds. The baked table is the composite resampled at the contour's own 128 points: new
// Community OS behaviour (Q3 a). Measured against the live evaluation (tests/audio_wt_contour_test.cpp, 256 phases a contour): over the 256 contours and 5 x 5 warps the mean
// difference is 0.54 % of full scale and the 99th percentile 0.14; a contour that jumps (stepped, saw, pulses) moves the jump by up to one baked interval, a difference of up to
// full scale there: with warp X +7 on the stepped groups (3, 6, 7, 9, 14) the mean is 1.3 % and 3.4 % of the phase points are more than 0.1 off.
// RECLAIM: an entry is never rewritten while it may be read, so the render may read any made entry at any time. The pool is emptied (wt_bake_reset) only where no descriptor
// holds an id: the project load, after the voices' table is cleared (app_main.cpp clear_sample_table, the simulator's loads). Between two loads every distinct (shape, warp) a live
// edit bakes stays, so a long sweep of a WT shape or warp can fill the pool; the slot then plays its warps live (correct, slower) and wt_bake_refused counts the CALLS that found
// the pool full (a rebuild of an instrument counts once a slot, not once a contour; `diag cpu` shows it).
inline constexpr unsigned kWtBakeCount = 128u;
struct WtBakeBank {
  std::uint16_t key[kWtBakeCount];                          // (shape << 8) | bend of each entry; main loop only
  std::uint32_t used;                                       // entries made; main loop only
  std::int16_t contour[kWtBakeCount][kWtContourLen];        // the baked contours, as the bank's
};
inline constexpr unsigned kWtBakeBytes = sizeof(WtBakeBank);  // 33,028 B

void wt_bake_bind(WtBakeBank& pool) noexcept;  // main loop, once, after wt_bind; empties the pool
void wt_bake_unbind(const WtBakeBank& pool) noexcept;  // when `pool` is the bound one, unbinds it (see wt_unbind)
void wt_bake_reset() noexcept;  // main loop: empties the pool (and the refused count); only where no descriptor holds an id (the project load, the voices' table cleared)
// Main loop only. The id (1..kWtBakeCount) of `shape` with the warps of `bend` baked, made now when it is new; 0 when `bend` has no warp, the pool is full or unbound. Makes
// the contour into the bound bank too (wt_make_bound).
unsigned wt_bake(unsigned shape, std::uint8_t bend) noexcept;
unsigned wt_bake_used() noexcept;  // entries made (diag)
unsigned wt_bake_refused() noexcept;  // wt_bake CALLS that found the pool full since the last reset (diag cpu): a rebuild counts once a slot, not once a contour

// The level of a WT slot at `phase`: the baked contour `bake` with no warp when it is made, else the bound bank's `shape` with the warp byte live (wt_eval_shape).
// Render (flash, O3OS_AUDIO_COLD). `bake` is the slot descriptor's id (FilterMod::delay of a WT kind); 0 or above the entries made: live.
O3OS_AUDIO_COLD float wt_eval_slot(unsigned shape, unsigned bake, std::uint8_t bend, float phase) noexcept;

// ---- OS-CEWTENV W8: the user contours (docs/specs/OS-CEWTENV.md F3, 3.9) ----------------------------------------------------------------------------------
// A contour of `/Envelopes/NNN.wav` replaces the fallback U001..U128 (bank rows 128..255). The bank is flat, so each user slot keeps a pointer to its current contour: the bank
// row (the fallback) or its own spare buffer `buf[i]` (the file). The main loop writes the one the pointer does NOT name, then publishes it with ONE aligned 32-bit store
// (__atomic_store_n, release: no tearing on the Cortex-M4, the fill is ordered before it, single writer = the main loop); the render loads the pointer once per evaluation (wt_eval_shape, wt_bake) and so
// reads either the whole old or the whole new contour, never a mix. The struct is the CALLER'S storage in SDRAM (reverb region's spare tail, `kWtUserBase`; a member of the
// simulator): never SRAM. A baked entry of a replaced contour is killed (key 0 is never a valid key) so a new bake of that shape is made again; a descriptor that holds the old id keeps
// playing the old bake until the next project load empties the pool (wt_bake_reset): new Community OS behaviour, an accepted edge.
inline constexpr unsigned kWtUserCount = kWtContourCount - kWtContourFactoryCount;  // 128: U001..U128
struct WtUserBank {
  const std::int16_t* ptr[kWtUserCount];                 // the current contour of each slot; the render reads it, the main loop stores it (one aligned 32-bit store)
  std::uint32_t loaded[kWtUserCount / 32u];              // main loop only: the slots that hold a file (not the fallback)
  alignas(4) std::int16_t buf[kWtUserCount][kWtContourLen];  // the file's contours (the spare of the pair {bank row, buf})
};
static_assert(alignof(WtUserBank) >= 4, "the pointers are word aligned: one 32-bit store");
inline constexpr unsigned kWtUserBytes = sizeof(WtUserBank);

// Fills dst[0..127] with the procedural contour of `shape` (held to 0..255): what wt_make writes, without the bank. Main loop only.
void wt_make_points(std::int16_t* dst, unsigned shape) noexcept;

void wt_user_bind(WtUserBank& user) noexcept;  // main loop, once, after wt_bind: every slot names its bank row, no file held. Bind the bank again, bind this again.
void wt_user_unbind(const WtUserBank& user) noexcept;  // when `user` is the bound one, unbinds it (see wt_unbind)
// Main loop: contour `n` (0..127 = U001..U128) becomes the 128 points `points` (0..32767, unipolar, as the bank's). False: not bound, or n out of range.
bool wt_user_set(unsigned n, const std::int16_t* points) noexcept;
// Main loop: contour `n` goes back to its procedural fallback. False when it holds no file (nothing to do), or not bound.
bool wt_user_restore(unsigned n) noexcept;
bool wt_user_is_file(unsigned n) noexcept;  // main loop: slot `n` holds a file

// The warp nibbles of a bend byte (F4): 0..7 -> +0..+7, 9..15 -> -7..-1, 8 -> 0.
std::int8_t wt_warp_x(std::uint8_t bend) noexcept;
std::int8_t wt_warp_y(std::uint8_t bend) noexcept;

// The bend byte of two warps, each held to -7..+7.
std::uint8_t wt_pack_warp(std::int8_t warp_x, std::int8_t warp_y) noexcept;

}  // namespace o3os::audio
