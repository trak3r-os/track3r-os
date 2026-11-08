/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 the track3r-os authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice and
 * this permission notice shall be included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
 * "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef O3MODULE_H
#define O3MODULE_H

/* The module SDK of the Community OS (ADR 0002, "Tier 1": native modules from the SD card). This header is the whole
 * contract between the OS and a module; it is MIT licensed whatever the OS's licence is, so that anyone can write modules.
 *
 * A module is a C or freestanding C++ translation unit built for the Cortex-M4F (or, for testing, for the host) that defines
 * ONE exported object, `o3m_entry`, of type o3m_module. The OS never links against a module by symbol: it reads that object
 * (function pointers included) and calls through it. The OS hands the module a table of services, o3m_host, at init().
 *
 * Rules a module keeps (the packer, tools/o3m_pack.py, rejects what it can see; the rest is on the author):
 *   - float only: no `double` (the FPU is single precision; soft double is 50-130 cycles an operation), no libm, no libc.
 *     Services the OS offers (exp2, log2, sin, cos) are in o3m_host. The compiler may call memcpy / memset / memmove for large
 *     copies and initialisers: avoid them (loops, field by field); an undefined symbol is a packing error.
 *   - no writable globals and no statics: every mutable byte lives in the instance's state block (state_bytes, zeroed by the OS,
 *     16-byte aligned, in SDRAM). There is no .data/.bss in a module and no global constructors.
 *   - no heap, no I/O, no blocking, no unbounded loops in render(), set_param(), note_on() and note_off(): they run in the audio
 *     interrupt's render context (the engine renders 64-frame blocks: 1.45 ms at 44.1 kHz, about 261,000 cycles at 180 MHz, and the whole device shares them).
 *   - render() and the functions it calls are marked O3M_HOT: the OS places them in fast SRAM when it has room. Everything
 *     else (init, tables, helpers) stays in SDRAM. Hot code must not read big tables: SDRAM is slow, so keep per-sample data
 *     in registers and the state block, and compute coefficients at control rate.
 *   - the module is built with -mlong-calls (SDRAM and SRAM are too far apart for a direct BL) and without position-independent
 *     code: the only relocation it may need is a 32-bit absolute address (R_ARM_ABS32).
 *
 * Calling contexts. init() and shutdown(): main loop (may take a while, may call host->log). render(), set_param(), note_on(),
 * note_off(): the render context, one thread, never concurrently for one instance, never from the main loop.
 *
 * ABI versioning: a module loads when its major version equals the OS's and its minor is not above the OS's. A change that
 * only adds (a flag, a host function at the end of o3m_host, a new kind) bumps the minor; anything else bumps the major.
 *   1.0  OS-012a: instruments and effects, descriptor + parameters, the reserved UI table / view_count / host.draw.
 *   1.1  OS-012b: the draw API (views), input events and the app kind (ADR 0002 amendment 1); parameter flags for the generated controls
 *        (no learn, modulation target, unit). Every 1.0 module loads and runs unchanged: a module built for 1.0 declares no views, and what
 *        1.1 adds to o3m_param::flags is zero in it (zero means "the 1.0 behaviour").
 *   1.2  OS-MODVER: the file header carries the module's own version, o3m_file_header::module_version = O3M_VERSION(major, minor, patch) (0 = unversioned), taken from
 *        the first reserved word. Only the header changed: the descriptor, the services and the draw API are those of 1.1, and every 1.0 and 1.1 file loads unchanged
 *        (its word is zero: a file of ABI 1.0 or 1.1 with a value there is refused as a reserved field). The OS uses it to decide between a card copy and the built-in
 *        module of the same name (the card's copy must be NEWER). A module that carries a version declares ABI 1.2, since an OS of ABI 1.1 would refuse the file.
 *   1.3  OS-H1 (ADR 0014 "app host API v2", slice H1; APPS ONLY): the first reserved header word, reserved2[0], becomes the module's CAPABILITY mask (O3M_CAP_*; zero in every
 *        earlier file and in a module that wants none), and o3m_host grows three pointers at its end: read, edit and io (the tables below). A table is NULL unless the OS granted
 *        it: `read` needs O3M_CAP_READ_PROJECT, `edit` needs O3M_CAP_EDIT_PROJECT, `io` is lent to every app of ABI 1.3 and its midi_read member is NULL without O3M_CAP_MIDI_IN.
 *        Every 1.0, 1.1 and 1.2 module loads and runs unchanged (capability word 0, init() keeps its signature: nothing is passed to it that it did not get before). A module that
 *        declares ABI 1.3 but asks for nothing loads like a 1.2 one. ABI minors are claimed in merge order: 1.3 is this slice's; a later slice takes 1.4.
 *   1.4  OS-H2a (ADR 0014 slice H2, amendment E; APPS ONLY): O3M_CAP_AUDIO_TAP (4) is granted, and the io table grows four members at its end: tap_select, tap_latest, tap_stream and
 *        tap_levels (NULL without the bit). An app that uses them declares ABI 1.4 and the bit; an OS of ABI 1.3 refuses such a file (minor above its own). Every 1.0..1.3 module
 *        loads and runs unchanged: nothing it uses moved, and the bit is a reserved field in a file of ABI 1.3 or below.
 *   1.5  OS-H3A (ADR 0014 slice H3, amendment G; APPS ONLY): apps edit the project. The step POD (o3m_step), two members appended to the read table (pattern_info, get_steps), four to the edit
 *        table (edit_begin, edit_end, set_step, set_steps: one undo unit per group, hard limits, whole-group rollback) and one field to the launch context (block_tracks). set_tempo joins an
 *        open group. No new capability bit: the tables are the ones read_project and edit_project already earn. Every 1.0..1.4 module loads and runs unchanged: nothing it uses moved, the new
 *        members are appended after the old ones, and each table's `size` says which exist (an app of 1.5 on an OS of 1.4 is refused at load: NEEDS NEWER OS).
 *   1.6  OS-H3B (ADR 0014 slice H3, amendment H; APPS ONLY): apps read and write the parameters of a sample or module instrument. Three members appended to the read table (instrument_info,
 *        get_param_desc, get_instrument_param) and one to the edit table (set_instrument_param, which joins the open edit group: one undo unit). No new capability bit and no
 *        change to any older member. Only the parameters the descriptor marks O3M_PD_APP_WRITABLE can be written. An app that only reads or writes steps still declares 1.5.
 *   1.7  OS-H4 (ADR 0014 slice H4, amendment A; APPS ONLY): O3M_CAP_EXPORT_FILES (16) is granted, and the io table grows four members at its end: export_open, export_write, export_close and
 *        export_status (NULL without the bit). An app writes ONE file at a time into the card's folder AppExport/<app>/, never anywhere else: the extension must be on a short allowlist, the OS
 *        stages the bytes in a ring and commits the file atomically (a Temp, a read-back, a rename that never replaces), and the first launch of the app asks the user to confirm. An app that asks for
 *        the bit declares ABI 1.7; an OS of 1.6 refuses such a file (minor above its own). The instrument-info POD grows one member at its end (midi_channel); a 1.6 app's shorter struct gets the old
 *        bytes only. Every 1.0..1.6 module loads and runs unchanged: nothing it uses moved.
 *   1.8  OS-MODPITCH + OS-INSTUI S10 (INSTRUMENTS; one minor for two additions): the descriptor o3m_module grows from 64 to 80 bytes, four members appended after view_count:
 *        `pitch` (the host drives a note's pitch: the Slide and Glide FX, see PITCH below), and for the params page `pages` / `page_count` (page names) and `labels` (names for the values of
 *        an INT parameter). O3M_NOTE_RAMP is a new note_on() flag, sent only to a module that exports `pitch`. THE DESCRIPTOR'S SIZE IS PER MINOR: struct_size is O3M_MODULE_SIZE(minor),
 *        64 for a module that declares 1.7 or below and 80 for 1.8, and the OS refuses any other (a module rebuilt against this header must write its size with the macro, not sizeof). The members
 *        past byte 64 are read, and checked, only when the module declares 1.8. Every 1.0..1.7 module loads and runs unchanged: its 64-byte descriptor has none of the four, and it never gets the flag.
 *   1.9  OS-KEYROWDLG S5 (OS-APPUX 3.1; APPS ONLY): o3m_ui grows one member at its end, `keys` (the app says what its F1..F7 and Shift + F1..F7 do; the OS draws them on the page's key row). THE UI TABLE'S SIZE IS PER MINOR
 *        like the descriptor's: ui.struct_size is O3M_UI_SIZE(minor), 20 bytes up to 1.8 and 24 from 1.9 (the descriptor stays 80 bytes). With it the OS makes the page's F8 the BACK key and Play the transport's: neither
 *        is delivered to input() any more, for every app (F7 + F8 stays the exit chord). Every 1.1..1.8 app still loads and runs, with the row `-- -- -- -- -- -- -- BACK`, but it no longer gets F8 or Play: that part of
 *        the change is the page's, not the declared minor's, so it reaches the apps already on a card too.
 *   1.10 OS-APPUX S3 (APPS ONLY; io-table entries only, o3m_ui and the descriptor do not grow): the io table grows four members at its end. get_transport (the play position as the sequencer publishes it, free like
 *        now_ms) and set_own_param (the app stores one of its own declared rows in the project, without an echo to its set_param) are lent to every app of ABI 1.10; play_note and stop_note (an app is heard: up to four
 *        held notes, eight triggers a frame, through the project's instruments like a pad) need the new capability bit O3M_CAP_PLAY_NOTES (64), which asks no confirm. An app of 1.10 has no OS parameter
 *        column (it draws its own rows), gets every Shift key, and finds the rows its project remembers through set_param() BEFORE enter(); an app of 1.2 .. 1.9 still gets them after enter(). An app's settings
 *        are what set_own_param stored; a set_own_param call made inside exit() is not kept (the OS keeps the rows with the project while the page runs), and only transient data is lost on exit. An OS of 1.9
 *        refuses a 1.10 file (minor above its own).
 *        Every 1.0..1.9 module still loads and runs, and nothing it uses moved; bit 64 is a reserved field below 1.10. Two rules of the app page changed for every app already on a card, whatever its minor:
 *        a screen button leaves the page for its screen like F8 does (the page used to swallow them; the app never sees it), and an app that declares no rows now gets the Shift keys (the OS's Shift layer
 *        is its parameter column only for an app of 1.9 or below that declares rows).
 *   1.11 OS-FLASHDIET I1 (THE IMPORTER KIND, kind 3; FIRST-PARTY MODULES ONLY): the importer kind is published. The descriptor grows one member at its end, `importer` (offset 80, so the descriptor is
 *        84 bytes from 1.11: O3M_MODULE_SIZE(11) is 84: it was sizeof(o3m_module) until 1.12 appended four members), pointing at the module's o3m_importer table; every other member is where it was. The file header's word 44 (reserved2[1])
 *        becomes, FOR KIND 3 ONLY, the IMPORTER LAYOUT TAG: the packer writes the tag of the layout the module was built against, and the OS refuses a kind 3 file whose word is not its own tag (Status
 *        LayoutMismatch) -- the VERSION LOCK: an importer is compiled against the OS's own model headers, so it may only run on an OS whose step layout is the one it was built against, whatever its ABI minor
 *        says. For every other kind the word stays reserved and zero (a non-zero word 44 in an instrument, an effect or an app is refused as before), and a kind 3 file that declares a minor below 1.11 is
 *        refused as an unknown kind: the kind exists from 1.11 only. An importer has no parameters and no views, makes no sound (render, set_param, note_on and note_off may be NULL, as an app's), never
 *        binds to a track and does not count against the OS's module slots. The importer's table (o3m_importer, eight words) is refined within 1.11 by OS-FLASHDIET I2 before any
 *        release shipped a kind 3 file: begin is handed the opened file's read window (o3m_file_window), the OS's model arena and the import options, and report / close were appended
 *        after read_pcm. Every 1.0..1.10 module loads and runs unchanged: its descriptor is 64 or 80 bytes as its minor says, and its word 44 is zero.
 *   1.12 OS-INSFX-ABI2 A1 (EFFECTS: stereo, the clock, a sidechain key, factory presets; INSTRUMENTS and EFFECTS for the presets): the descriptor grows four members after `importer`, `render_st` (84),
 *        `fx_flags` (88), `presets` (92) and `preset_count` (96), so it is 100 bytes from 1.12 (O3M_MODULE_SIZE(12) is sizeof(o3m_module)); render_st makes an effect stereo (NULL: a mono effect), fx_flags
 *        says how (O3M_FX_WIDENS), presets lists named parameter sets (o3m_preset). O3M_PARAM_KEY (32) is a new parameter flag: an effect's INT parameter 0..16 that names another track whose level the effect
 *        reads (0 = none). o3m_host grows two members at its end, get_clock (the tempo and the beat) and track_level (a track's level, the sidechain key), so the table is 60 bytes; the importer's own
 *        table stays 52 bytes (O3M_HOST_HAS is false for them there). The new descriptor members are read, and checked, only when the module declares 1.12; an app and an importer carry them zero (presets
 *        are an instrument's and an effect's). THE LAYOUTS OF 1.0..1.11 DID NOT CHANGE: every module of 1.0 to 1.11 loads and runs bit for bit as before, its descriptor is 64, 80 or 84 bytes as its minor says and
 *        the new members are never read; an OS of 1.11 refuses a 1.12 file (minor above its own).
 *   1.13 OS-MODRATE S2 (INSTRUMENTS: a per-instrument RATE): fx_flags gets a second bit, O3M_FX_RATE (2), valid on an instrument of 1.13 or later only. It says "I read host->sample_rate and accept any
 *        frames": the OS then offers a RATE row (FULL, 1/2, 1/4) on the instrument and, below FULL, calls render() for frames/2 (frames/4) at a host->sample_rate of 22050 (11025) and holds each sample
 *        2x (4x) into the output, with no filter (the Amiga sound). The module is shut down and initialised again when RATE changes. No layout changed (the descriptor stays 100 bytes, the host table
 *        60): every module of 1.0..1.12 loads and runs unchanged and never gets a RATE row; an OS of 1.12 refuses a 1.13 file (minor above its own).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define O3M_ABI_MAJOR 1
#define O3M_ABI_MINOR 13

/* What the module is. The instrument (no input, one mono output) and the effect (one mono input, one mono output) since 1.0; the app
 * since 1.1 (it makes no sound: it owns the body of an OS page, draws a view and takes input; render, set_param, note_on and note_off
 * may be NULL). The importer since 1.11 (OS-FLASHDIET I1: it serves files, kind 3; a file that says importer with a minor below 1.11
 * is refused as an unknown kind). A module of ABI 1.0 cannot be an app. */
#define O3M_KIND_INSTRUMENT 1u
#define O3M_KIND_EFFECT 2u
#define O3M_KIND_IMPORTER 3u
#define O3M_KIND_APP 4u

/* The most frames a single render() call may be given (the ABI's ceiling); any count from 1 to this is legal. This OS's engine renders 64 at most (hal::audio::kBlock), and fewer when a block is
 * split at the sample a note starts on: keep no state per call. */
#define O3M_BLOCK_MAX 128u

/* note_on() flags. */
#define O3M_NOTE_LEGATO 1u /* this note overlaps a held one on purpose (a live key pressed over another; no note_off in between). A pattern's note never carries it, the OS releases the held note first,
                              except (1.8) a pattern's Glide or Slide note on a module that exports `pitch`: it comes with O3M_NOTE_RAMP and without a note_off before it (see PITCH) */
#define O3M_NOTE_RAMP 2u   /* 1.8, only to a module that exports `pitch`: the host drives this note's pitch (a Glide or Slide FX of the pattern). The module keeps its envelope when O3M_NOTE_LEGATO comes with it, puts its
                              base pitch on `note` at once, applies NO portamento of its own and follows pitch() exactly until the next note_on() without O3M_NOTE_RAMP. A module's own portamento is for a live legato key
                              (O3M_NOTE_LEGATO without O3M_NOTE_RAMP) only; the pitch never slides twice */

/* o3m_param flags. */
#define O3M_PARAM_LOG 1u  /* the editing curve is exponential (cutoff, times) */
#define O3M_PARAM_INT 2u  /* whole numbers only */
#define O3M_PARAM_BOOL 4u /* 0 or 1 */
/* 1.1: how the OS builds the standard control of the parameter. All zero (a 1.0 module) = learnable, not a modulation target, no unit. */
#define O3M_PARAM_NO_LEARN 8u /* MIDI Learn may not bind a controller to it */
#define O3M_PARAM_MOD 16u     /* a modulation target (the mod matrix may route a source to it) */
/* 1.12 (OS-INSFX-ABI2): an effect's sidechain KEY: an INT parameter, min 0, max 16, no labels, whose value names the track the effect reads the level of (0 = no key, 1..16 = track 1..16; see
 * o3m_host.track_level). Effects only, at most two a module; the OS never lets MIDI Learn bind it. A module that declares 1.11 or below never has the flag honoured. */
#define O3M_PARAM_KEY 32u
#define O3M_PARAM_UNIT_SHIFT 8u
#define O3M_PARAM_UNIT_MASK 0xFF00u /* bits 8..15: an O3M_UNIT_* */
#define O3M_UNIT_NONE 0u
#define O3M_UNIT_PERCENT 1u
#define O3M_UNIT_HZ 2u
#define O3M_UNIT_MS 3u
#define O3M_UNIT_DB 4u
#define O3M_UNIT_SEMITONES 5u
#define O3M_UNIT_SECONDS 6u
#define O3M_UNIT_CENTS 7u
#define O3M_UNIT_COUNT 8u
#define O3M_PARAM_UNIT(flags) (((flags) & O3M_PARAM_UNIT_MASK) >> O3M_PARAM_UNIT_SHIFT)
#define O3M_PARAM_WITH_UNIT(unit) ((uint32_t)(unit) << O3M_PARAM_UNIT_SHIFT)

/* The longest module and parameter names, in bytes with the NUL. */
#define O3M_NAME_MAX 24u
#define O3M_PARAM_NAME_MAX 16u
#define O3M_PARAMS_MAX 32u
/* 1.8, the params page of an instrument (OS-INSTUI S10): at most O3M_PAGES_MAX pages (the F1..F4 keys) of at most O3M_PAGE_PARAMS_MAX parameters, a page name of at most 7 characters, labels for
 * an INT parameter of O3M_LABELS_MIN..O3M_LABELS_MAX values, each label at most 11 characters. The sizes are in bytes with the NUL. */
#define O3M_PAGES_MAX 4u
#define O3M_PAGE_PARAMS_MAX 8u
#define O3M_PAGE_NAME_MAX 8u
#define O3M_LABELS_MIN 2u
#define O3M_LABELS_MAX 16u
#define O3M_LABEL_NAME_MAX 12u
/* 1.12, factory presets (OS-INSFX-ABI2): at most O3M_PRESETS_MAX presets a module, a name of at most O3M_PRESET_NAME_MAX - 1 characters (bytes with the NUL). */
#define O3M_PRESETS_MAX 32u
#define O3M_PRESET_NAME_MAX 16u
/* 1.12, o3m_module.fx_flags (effects only; every other bit is 0). O3M_FX_WIDENS: the effect makes a stereo image from a mono input, so the OS calls render_st on every block (it needs render_st). Without
 * it a module that has render_st is stereo-capable: render_st on a block whose input has a side signal, render on every other block, one state for both. */
#define O3M_FX_WIDENS 1u
/* 1.13, o3m_module.fx_flags, an instrument's (OS-MODRATE S2): O3M_FX_RATE says the instrument reads host->sample_rate (never a constant) and accepts any frames (1..32: the OS calls it with at most 32 when RATE is not FULL), so the OS may render it at
 * half or a quarter of the engine rate and hold the samples (the RATE row). Valid on an instrument that declares 1.13 or later; WIDENS stays an effect's. */
#define O3M_FX_RATE 2u

/* The version of a module itself (not of the ABI), as the file header stores it (ABI 1.2): major in bits 16..31 (0..65535), minor in bits 8..15 and patch in bits 0..7 (each
 * 0..255). 0 is "unversioned" and the lowest there is; versions compare as plain numbers, so the order is major, then minor, then patch. */
#define O3M_VERSION(major, minor, patch) (((uint32_t)(major) << 16) | ((uint32_t)(minor) << 8) | (uint32_t)(patch))
#define O3M_VERSION_MAJOR(v) ((uint32_t)(v) >> 16)
#define O3M_VERSION_MINOR(v) (((uint32_t)(v) >> 8) & 0xFFu)
#define O3M_VERSION_PATCH(v) ((uint32_t)(v) & 0xFFu)

/* The target a file is built for: the Cortex-M4F, Thumb, hard float, 32-bit absolute relocations only. */
#define O3M_TARGET_ARMV7EM 1u

#if defined(__arm__)
#define O3M_HOT __attribute__((section(".text.o3m_hot"), noinline))
#else
#define O3M_HOT __attribute__((noinline))
#endif

#if defined(_WIN32)
#define O3M_EXPORT __declspec(dllexport)
#else
#define O3M_EXPORT __attribute__((used, visibility("default")))
#endif

/* One editable parameter. Parameter i is the i-th entry of o3m_module.params; its id in set_param() is i. A parameter blob
 * stored with an instrument is param_count little-endian float32 values in that order (the OS keeps it verbatim and does
 * not interpret it). */
typedef struct o3m_param {
  const char* name; /* up to O3M_PARAM_NAME_MAX - 1 characters, NUL terminated */
  float min;
  float max;
  float def;
  uint32_t flags;
} o3m_param;

/* VIEWS (ABI 1.1, OS-012b; maintainer decision 2026-10-12, ADR 0002 amendment 1). The OS owns every page; a module of ANY kind may draw VIEWS inside
 * it through one restricted draw API (rectangles, text, curves): the TB-303's filter and envelope curve on its instrument page, an app's page body.
 *   - o3m_module.view_count: how many views the module declares (0..O3M_VIEWS_MAX; mirrored in the file header; 0 = none),
 *   - o3m_module.ui: a table of entry points, NULL exactly when view_count is 0: view_draw(state, view_index, canvas) paints view `view_index`;
 *     enter, input and exit (each may be NULL) are for apps, which own the body of their page,
 *   - o3m_host.draw: the versioned table of drawing functions the OS lends (o3m_draw_api, below).
 * A view is drawn when its page is on screen, at the OS's pace (a few times a second at least, whenever a parameter changes, 30 times a second
 * for an app): view_draw() is called from the main loop and paints into `canvas`, a rectangle of the page of canvas->width x canvas->height pixels
 * whose top-left corner is (0, 0); everything outside it is cut off. It receives the current value of every parameter in canvas->params (the TB-303's
 * curve follows the cutoff the user is turning) and must draw from those, not from the instance state, which the audio thread may be changing: for an
 * instrument or an effect `state` is the instance's state block, READ ONLY and possibly mid-update; for an app it is the app's own block.
 *
 * Containment (the OS enforces it, a module cannot opt out): per frame a view may make O3M_VIEW_MAX_CALLS draw calls, give O3M_VIEW_MAX_POINTS points
 * to polylines and curves and O3M_VIEW_MAX_TEXT characters to text, and view_draw() may take O3M_VIEW_MAX_US microseconds (input() and enter() the
 * same). A view that goes over is blanked at once (nothing of that frame is drawn) and its module is quarantined for the rest of the boot: its views
 * are never called again, it is not loaded again until a reset (its audio keeps running until the project changes). The app page's exit chord
 * (F7 + F8) is the OS's and works whatever the module does. A call with an argument it cannot use (a NULL string, a colour out of range, a zero
 * size) is ignored and counts as a call. */
#define O3M_VIEWS_MAX 8u
#define O3M_DRAW_API_VERSION 1u

/* The budgets above. A module may rely on being given at least these; the OS may raise them in a later minor version. */
#define O3M_VIEW_MAX_CALLS 96u
#define O3M_VIEW_MAX_POINTS 256u  /* all polylines and curves of a frame together; a single one at most O3M_VIEW_MAX_POLY_POINTS */
#define O3M_VIEW_MAX_POLY_POINTS 128u
#define O3M_VIEW_MAX_TEXT 384u    /* characters of all text calls of a frame */
#define O3M_VIEW_MAX_US 4000u

/* Colours are the theme's roles, in the order of the theme's 16 tokens (the module never sees an RGB value, so every theme draws every view). */
#define O3M_COLOR_CHROME 0u
#define O3M_COLOR_CHROME_TEXT 1u
#define O3M_COLOR_CHROME_DIM 2u
#define O3M_COLOR_PANE 3u       /* the page's background */
#define O3M_COLOR_TEXT 4u
#define O3M_COLOR_TEXT_DIM 5u
#define O3M_COLOR_EMPTY 6u      /* empty fields and rules */
#define O3M_COLOR_INSTR 7u
#define O3M_COLOR_VOL 8u
#define O3M_COLOR_FX 9u
#define O3M_COLOR_SEL 10u
#define O3M_COLOR_SEL_TEXT 11u
#define O3M_COLOR_ACCENT 12u    /* the one orange: a cursor, an armed thing */
#define O3M_COLOR_ACCENT_TEXT 13u
#define O3M_COLOR_PLAY 14u
#define O3M_COLOR_PLAY_TEXT 15u
#define O3M_COLOR_COUNT 16u

/* Text faces of the theme (fixed-pitch: every character advances one cell). */
#define O3M_FACE_DISPLAY 0u
#define O3M_FACE_DATA 1u
#define O3M_FACE_LABEL 2u
#define O3M_FACE_MICRO 3u
#define O3M_FACE_COUNT 4u

/* Curve axes (the ui::plot primitive): a linear axis, a logarithmic frequency axis (lo > 0), and a decibel axis (values already in dB). */
#define O3M_AXIS_LINEAR 0u
#define O3M_AXIS_LOG_FREQ 1u
#define O3M_AXIS_DB 2u
#define O3M_CURVE_AXES 1u /* draw the frame, the grid and the tick labels; the data area is then the canvas inset by O3M_CURVE_MARGIN_* */
#define O3M_CURVE_FILL 2u /* the translucent fill under the line */
#define O3M_CURVE_MARGIN_LEFT 28
#define O3M_CURVE_MARGIN_BOTTOM 16
#define O3M_CURVE_MARGIN_TOP 4
#define O3M_CURVE_MARGIN_RIGHT 4

/* Input events of an app (OS-012b): the Tracker's controls. Buttons by the hardware's numbers (the same as the OS's button table). Shift is a
 * button too (O3M_BTN_SHIFT) and is also in `mods` of every event while it is held. Pad `id` is 0..47 (pad 0 at the top left),
 * `value` its velocity 1..127 on press and 0 on release. */
#define O3M_EV_BUTTON 1u  /* id: O3M_BTN_*, value: 1 down, 0 up */
#define O3M_EV_PAD 2u     /* id: 0..47, value: velocity, 0 = release */
#define O3M_EV_ENCODER 3u /* id: 0, value: the signed detents of the turn */
#define O3M_MOD_SHIFT 1u
#define O3M_BTN_F1 0u /* F1..F8 = 0..7 */
#define O3M_BTN_F2 1u
#define O3M_BTN_F3 2u
#define O3M_BTN_F4 3u
#define O3M_BTN_F5 4u
#define O3M_BTN_F6 5u
#define O3M_BTN_F7 6u
#define O3M_BTN_F8 7u
#define O3M_BTN_PLAY 23u
#define O3M_BTN_COPY 24u
#define O3M_BTN_INSERT 25u
#define O3M_BTN_UP 26u
#define O3M_BTN_DELETE 27u
#define O3M_BTN_REC 28u
#define O3M_BTN_SHIFT 29u
#define O3M_BTN_LEFT 30u
#define O3M_BTN_DOWN 31u
#define O3M_BTN_RIGHT 32u
/* The exit chord F7 + F8 is the OS's and an app cannot swallow it: the OS recognises it on the raw events and closes the page whatever the module does. The app is still handed
 * the F7 press as an ordinary O3M_EV_BUTTON event just before the page closes, and never its release (exit() follows). Do not act on F7 as if the page stayed open.
 * Since 1.9 (for EVERY app, whatever it declares) F8 is the page's BACK key and Play is the transport's: the OS acts on them and never hands either to input(). Pads and Rec are still delivered. */

typedef struct o3m_ui_event {
  uint32_t kind; /* O3M_EV_* */
  uint32_t id;
  int32_t value;
  uint32_t mods; /* O3M_MOD_* held at the time */
} o3m_ui_event;

struct o3m_draw_api;

/* What view_draw() paints on. Created by the OS for one call; the fields are read-only for the module. */
typedef struct o3m_canvas {
  uint32_t struct_size;  /* sizeof(o3m_canvas) */
  uint32_t view_index;   /* the view being drawn */
  int32_t width;         /* pixels, the view's rectangle (at least 16 x 16, at most 800 x 480) */
  int32_t height;
  uint32_t param_count;  /* the module's parameters... */
  const float* params;   /* ...and their current values, in descriptor order (param_count floats) */
  const struct o3m_draw_api* draw; /* the drawing functions (the same table as o3m_host.draw), so that a view needs nothing from its state */
} o3m_canvas;

/* One curve: the series (x, y) of n points mapped through two axes into the canvas (or the data area of it, with O3M_CURVE_AXES), reduced and clipped by
 * the OS. `x` may be NULL: the points are then spread evenly over x_lo..x_hi. n is 2..O3M_VIEW_MAX_POLY_POINTS. */
typedef struct o3m_curve {
  const float* x;
  const float* y;
  uint32_t n;
  uint32_t x_axis; /* O3M_AXIS_* */
  uint32_t y_axis;
  float x_lo, x_hi, y_lo, y_hi; /* the range of each axis (lo < hi; > 0 for a LOG_FREQ axis) */
  uint32_t flags;               /* O3M_CURVE_* */
  uint32_t color;               /* O3M_COLOR_* of the line (and the fill) */
} o3m_curve;

/* Fills a curve field by field. Use it instead of an initialiser list: a list with zero members (a NULL x, a 0.0f bound) makes the compiler clear the whole struct with
 * memset, which a module has no libc to call (the packer refuses it as an undefined symbol). */
static inline void o3m_curve_set(o3m_curve* c, const float* x, const float* y, uint32_t n, uint32_t x_axis, uint32_t y_axis, float x_lo, float x_hi, float y_lo, float y_hi,
                                 uint32_t flags, uint32_t color) {
  c->x = x;
  c->y = y;
  c->n = n;
  c->x_axis = x_axis;
  c->y_axis = y_axis;
  c->x_lo = x_lo;
  c->x_hi = x_hi;
  c->y_lo = y_lo;
  c->y_hi = y_hi;
  c->flags = flags;
  c->color = color;
}

/* The drawing functions the OS lends (o3m_host.draw), version O3M_DRAW_API_VERSION. Check `version` and `size` before using a member a later
 * version added at the end. Coordinates are canvas pixels, signed (a part outside the canvas is cut off). All calls draw in the order made. */
typedef struct o3m_draw_api {
  uint32_t version; /* O3M_DRAW_API_VERSION */
  uint32_t size;    /* sizeof(o3m_draw_api) */
  void (*rect)(o3m_canvas* c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);    /* filled */
  void (*outline)(o3m_canvas* c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color); /* a 1 px border inside the rectangle */
  void (*text)(o3m_canvas* c, int32_t x, int32_t y, const char* s, uint32_t face, uint32_t color); /* (x, y) is the top-left corner; at most 64 characters */
  int32_t (*text_width)(const o3m_canvas* c, const char* s, uint32_t face);                  /* pixels; not a draw call */
  void (*polyline)(o3m_canvas* c, const int16_t* xy, uint32_t points, uint32_t color);       /* xy: x0, y0, x1, y1, ...; 2..O3M_VIEW_MAX_POLY_POINTS points, 1 px */
  void (*curve)(o3m_canvas* c, const o3m_curve* curve);
} o3m_draw_api;

typedef struct o3m_ui {
  uint32_t struct_size;  /* O3M_UI_SIZE(the module's minor): 5 words up to 1.8, 6 from 1.9 */
  void (*enter)(void* state);                                         /* apps: the page opens (may be NULL) */
  void (*input)(void* state, const o3m_ui_event* ev);                 /* apps: one input event (may be NULL) */
  void (*view_draw)(void* state, uint32_t view_index, o3m_canvas* canvas); /* paint view `view_index` < view_count: bounded work, no blocking, only through the draw API */
  void (*exit)(void* state);                                          /* apps: the page closes (may be NULL) */
  const struct o3m_keys* (*keys)(void* state);                        /* 1.9, apps: what the keys do, for the page's key row (may be NULL); read only when ui.struct_size is O3M_UI_SIZE(9) and the module declares 1.9 */
} o3m_ui;

/* The labels of an app's key row (ABI 1.9), what o3m_ui.keys returns: `f` for F1..F8 and `shift_f` for Shift + F1..F8, each a NUL-terminated string of at most O3M_KEY_LABEL_MAX (7) printable ASCII characters,
 * NULL or "" for a key that does nothing (the row then says "-"). F8 is the OS's BACK key: its slot, and Shift + F8's, are ignored. The OS calls keys() from the main loop once a frame, before the page draws,
 * and after every input(), under the same time budget as input(); it copies the 16 labels at once (up to 8 bytes each), so the table and the strings may differ on the next call. The labels are only a statement: the OS delivers the
 * key events to input() whatever they say. */
#define O3M_KEY_LABEL_MAX 7u
typedef struct o3m_keys {
  const char* f[8];
  const char* shift_f[8];
} o3m_keys;
/* o3m_ui.struct_size for a module that declares ABI 1.`minor`: write it with this macro, never sizeof(o3m_ui) (a module rebuilt against this header that declares 1.8 or below must still say 20 bytes). */
#define O3M_UI_SIZE(minor) ((minor) >= 9u ? sizeof(o3m_ui) : __builtin_offsetof(o3m_ui, keys))

/* HOST API v2 (ABI 1.3, ADR 0014 slice H1; apps only). What an app may ask of the OS beyond drawing. An app asks for CAPABILITIES in the file header (reserved2[0], the mask below;
 * the packer's --caps, module.json "capabilities"); the OS grants what it implements and lends the matching table, or NULL. Test every table and every member you use for NULL, and
 * check `size` before using a member a later version added at the end. All calls are made from the main loop (init, enter, input, view_draw, set_param, exit), never from the audio
 * interrupt; they are quick and copy out, so they cost the audio engine nothing.
 *
 * THE RULE OF THE TABLES: a table, and every io / read / edit entry that needs a capability bit (today midi_read and midi_lost, the four tap_* of 1.4 with O3M_CAP_AUDIO_TAP and the four
 * export_* of 1.7 with O3M_CAP_EXPORT_FILES; the two play_* of 1.10 with O3M_CAP_PLAY_NOTES; later midi_send with O3M_CAP_MIDI_OUT), is NULL in the table the OS lends unless the app's file asked for that bit and this OS grants it. Never call an entry without testing it for NULL.
 *
 * Capability bits. The six values up to 32 are FROZEN as published ABI (ADR 0014 amendment D, condition C4); they are never renumbered or reused. Only the ones marked "1.3", "1.4", "1.7" or "1.10" are
 * granted by an OS of ABI 1.10 (audio_tap needs a file of ABI 1.4 too, export_files a file of ABI 1.7, play_notes a file of ABI 1.10). An app that asks for a bit this OS does not grant (midi_out) is refused AT LOAD, before it runs, with a message naming the capability ("NEEDS
 * MIDI OUT (NEWER OS)"): it never runs with half its tables. A file that sets a bit above the seven, or any bit in a file of ABI 1.2 or below, or in a module that is not an app, is refused
 * as a reserved field. */
#define O3M_CAP_MIDI_IN 1u        /* 1.3: io.midi_read / io.midi_lost (the MIDI-in ring) */
#define O3M_CAP_READ_PROJECT 2u   /* 1.3: the read table */
#define O3M_CAP_AUDIO_TAP 4u      /* 1.4: the audio tap (io.tap_*); the app page lists it as a badge, there is no confirm (it can only listen) */
#define O3M_CAP_EDIT_PROJECT 8u   /* 1.3: the edit table. The OS asks the user to confirm the first launch of an app that holds it ("this tool edits the project; edits are undoable") */
#define O3M_CAP_EXPORT_FILES 16u  /* 1.7: io.export_open / export_write / export_close / export_status, files in AppExport/<app>/. The OS asks the user to confirm the first launch ("THIS TOOL WRITES FILES TO APPEXPORT/<APP>") */
#define O3M_CAP_MIDI_OUT 32u      /* assigned to a later slice (OS-APPMIDI) */
#define O3M_CAP_PLAY_NOTES 64u    /* 1.10: io.play_note / io.stop_note (the app plays the project's instruments). The first bit above the six frozen ones; no confirm: it only makes sound */
#define O3M_CAPS_ABI_1_3 (O3M_CAP_MIDI_IN | O3M_CAP_READ_PROJECT | O3M_CAP_EDIT_PROJECT)
#define O3M_CAPS_ABI_1_4 (O3M_CAPS_ABI_1_3 | O3M_CAP_AUDIO_TAP)
#define O3M_CAPS_ABI_1_7 (O3M_CAPS_ABI_1_4 | O3M_CAP_EXPORT_FILES)
#define O3M_CAPS_ABI_1_10 (O3M_CAPS_ABI_1_7 | O3M_CAP_PLAY_NOTES)

/* The launch context: what the app was opened on. An app opened from the MODULES list gets all zeros (flags 0). Pattern F7 TOOLS (ABI 1.5) fills it with the Pattern's selection and sets
 * O3M_LAUNCH_FROM_TOOLS; the other fields mean something only then (pattern 0 and track 0 are real). Read it with o3m_read_api.launch_ctx.
 *   pattern       the pattern id in view (1..255, the id the read and edit tables take).
 *   track         0-based: the first track of the block, or the cursor's track when there is no block.
 *   instrument    0-based instrument slot in use (the step byte for it is instrument + 1).
 *   block_start   the first row of the block, or the cursor's row when there is no block.
 *   block_length  rows in the block; 0 = no block (the app decides from the cursor).
 *   block_tracks  ABI 1.5: tracks the block covers, starting at `track`; 0 = no block. Absent (the OS wrote fewer bytes: see `size`) on an OS of ABI 1.4 or below, where it reads as 0. */
#define O3M_LAUNCH_FROM_TOOLS 1u
typedef struct o3m_launch_ctx {
  uint32_t size;         /* in: sizeof your struct. out: the bytes the OS wrote (the smaller of the two) */
  uint32_t flags;        /* O3M_LAUNCH_* */
  uint32_t pattern;      /* the pattern id in view */
  uint32_t track;        /* 0-based track */
  uint32_t instrument;   /* 0-based instrument slot of that track */
  uint32_t block_start;  /* first selected step of the block */
  uint32_t block_length; /* steps selected, 0 = no block */
  uint8_t block_tracks;  /* ABI 1.5: tracks selected, 0 = no block */
  uint8_t reserved[3];   /* 0 */
} o3m_launch_ctx;

/* ---- The step POD (ABI 1.5) ----
 * One pattern step as an app sees it, 8 bytes, the model's encodings. `note`: 0 empty, 1..128 the note (MIDI note + 1: 1 = C-0, 49 = C-4, 61 = C-5 as the Pattern's tile names it, ui/src/assets.cpp:66-73; the MIDDLE C setting, model/src/params.cpp:192, only renames the notes on screen, the byte is the same), 253 fade, 254 cut, 255 off (129..252 are reserved and refused on a
 * write). `instr`: 0 none, 1..64 the instrument as the Pattern shows it (slot + 1). `vol`: 0..127, 255 unset. FX: `type` 0 none, 1..42 the Tracker's FX, 128..255 an engine's own (43..127 are
 * reserved and refused on a write); `value` any byte. `reserved` is 0 on a read and ignored on a write. A write takes a FIELD MASK: only the masked fields change, the rest of the step stays. */
#define O3M_NOTE_EMPTY 0u
#define O3M_NOTE_FIRST 1u
#define O3M_NOTE_LAST 128u
#define O3M_NOTE_FADE 253u
#define O3M_NOTE_CUT 254u
#define O3M_NOTE_OFF 255u
#define O3M_INSTR_NONE 0u
#define O3M_INSTR_MAX 64u
#define O3M_VOL_MAX 127u
#define O3M_VOL_UNSET 255u
#define O3M_FX_NONE 0u
#define O3M_FX_OG_LAST 42u
#define O3M_FX_ENGINE_FIRST 128u
#define O3M_STEP_NOTE 1u  /* field mask bits of set_step / set_steps */
#define O3M_STEP_INSTR 2u
#define O3M_STEP_VOL 4u
#define O3M_STEP_FX1 8u
#define O3M_STEP_FX2 16u
#define O3M_STEP_ALL 31u
#define O3M_PATTERN_ROWS_MAX 128u /* the most rows a track has */
#define O3M_PATTERN_TRACKS_MAX 16u
typedef struct o3m_step {
  uint8_t note, instr, vol;
  uint8_t fx1_type, fx1_value, fx2_type, fx2_value;
  uint8_t reserved; /* 0 */
} o3m_step;

/* What pattern_info answers (ABI 1.5). `size`: in sizeof your struct, out the bytes written. `length` is the pattern's length in rows (the longest track), 1..128; `tracks` the project's active
 * tracks, 1..16; `track_length[t]` the rows track t PLAYS (1..128; tracks beyond `tracks` read 0). Reads and writes are accepted for rows 0..track_length[t]-1 only, the rule the pattern editor
 * itself keeps (a step past a track's end is never edited). */
typedef struct o3m_pattern_info {
  uint32_t size;
  uint32_t length;
  uint32_t tracks;
  uint8_t track_length[O3M_PATTERN_TRACKS_MAX];
} o3m_pattern_info;

/* ---- Instrument parameters (ABI 1.6) ----
 * `instrument` is the number the Pattern shows, 1..64 (the step byte). An instrument is EMPTY (nothing in the slot), a SAMPLE, a MIDI instrument or a MODULE (an SD module bound to the slot).
 *   instrument_info.kind    O3M_INSTRUMENT_*.
 *   instrument_info.engine  a SAMPLE: the play engine as the project stores it (0 auto, 1 modern, 2 Amiga, 3 12-bit, 4 8-bit); a MODULE: a 32-bit hash (FNV-1a of the module's file stem), equal for two
 *                           instruments of the same module; 0 for the others. Two instruments MATCH when kind and engine are equal.
 *   instrument_info.param_count  the parameters get_param_desc lists: 83 for a SAMPLE, the module's own (up to O3M_PARAMS_MAX, 0 while it is not loaded) for a MODULE, 0 for the others.
 * A parameter's `id` is the project's: a SAMPLE's 0..13 voice, filter and sends; 0x14..0x1C the source page; 0x20 + 8 * envelope + field the six envelopes (amount, attack, decay, sustain, release...); 0x60 + 4 * slot + field the
 * LFO and SOURCE of each slot; a MODULE's 0x100 + i for the module's parameter i. Ids are never renumbered (the project file's MIDI Learn bindings hold them). `group` is the page: for a SAMPLE an O3M_PGROUP_*, for a
 * MODULE the page of eight, i / 8. `min`, `max` and `def` are the parameter's own units (a cutoff 0..1, a tune in semitones, an attack in milliseconds). `name` is upper case.
 * FLAGS. STEPPED: whole numbers with few values (a tune in semitones, a bit depth, a module INT): a value between two steps means nothing, so an app that blends two values switches instead.
 * ENUM: the value picks a name (a filter type, a module BOOL). APP_WRITABLE: set_instrument_param accepts it. It is CLEAR on everything structural, listed in docs/guides/app-modules.md: the sample, the
 * play engine, the play mode, the wavetable and slice fields, the SOURCE of a modulation slot and the envelope ENABLE (they rebuild the voice), and every parameter of a MIDI instrument (it has none here).
 * A value is read and written as a float in the unit above; a whole-number field stores the value ROUNDED. */
#define O3M_INSTRUMENT_EMPTY 0u
#define O3M_INSTRUMENT_SAMPLE 1u
#define O3M_INSTRUMENT_MIDI 2u
#define O3M_INSTRUMENT_MODULE 3u
#define O3M_PGROUP_VOICE 0u
#define O3M_PGROUP_FILTER 1u
#define O3M_PGROUP_ENV 2u
#define O3M_PGROUP_SENDS 3u
#define O3M_PGROUP_SOURCE 4u
#define O3M_PGROUP_MOD 5u
#define O3M_PD_STEPPED 1u
#define O3M_PD_ENUM 2u
#define O3M_PD_APP_WRITABLE 4u
#define O3M_SAMPLE_PARAM_COUNT 83u
#define O3M_PARAM_DESC_NAME 16u
typedef struct o3m_instrument_info {
  uint32_t size; /* in: sizeof your struct; out: the bytes written */
  uint32_t kind; /* O3M_INSTRUMENT_* */
  uint32_t engine;
  uint32_t param_count;
  uint32_t midi_channel; /* ABI 1.7: 1..16 for a MIDI instrument (its CHANNEL), 0 for any other kind. A struct built for 1.6 is 16 bytes (`size` says so) and gets the first four members only */
} o3m_instrument_info;
typedef struct o3m_param_desc {
  uint32_t size; /* in: sizeof your struct; out: the bytes written */
  float min, max, def;
  uint16_t id;
  uint16_t flags; /* O3M_PD_* */
  uint8_t group;
  uint8_t reserved[3]; /* 0 */
  char name[O3M_PARAM_DESC_NAME]; /* upper case, NUL terminated */
} o3m_param_desc;

#define O3M_READ_API_VERSION 1u
/* The read table (o3m_host.read; needs O3M_CAP_READ_PROJECT). Copy-out: the OS's model layout never enters the ABI. Later versions add members at the end. */
typedef struct o3m_read_api {
  uint32_t version; /* O3M_READ_API_VERSION */
  uint32_t size;    /* sizeof(o3m_read_api) */
  /* The project's tempo in BPM x 100 (12000 = 120.00 BPM), 4000..80000; 0 when no project is open. If this app has asked for set_tempo and the request has not been applied yet
   * (it is applied by the main loop a moment later), the requested value is returned, so a read after a write sees the write. */
  uint32_t (*get_tempo)(void);
  /* Fills `*out` (set out->size first). Returns 1 when the app was opened with a selection (O3M_LAUNCH_FROM_TOOLS), 0 when it was opened from the app list (all zeros). */
  uint32_t (*launch_ctx)(o3m_launch_ctx* out);
  /* ABI 1.5 (absent when `size` is below the offset). Both return an O3M_EDIT_* code: OK, RANGE (no such pattern, `out` NULL, a count of 0 or outside the track, a track beyond the active ones),
   * NO_PROJECT. They copy out; the app never sees the model. What is read is the project as it is: the writes of a group that is still open apply when it closes (edit_end), not before. (get_tempo is the exception: inside a group it returns the group's pending tempo.) */
  int32_t (*pattern_info)(uint16_t pattern, o3m_pattern_info* out);
  /* Copies `count` steps (1..128) of `track` from `first_row` into `out` (count o3m_step). */
  int32_t (*get_steps)(uint16_t pattern, uint8_t track, uint8_t first_row, o3m_step* out, uint8_t count);
  /* ABI 1.6 (absent when `size` is below the offset). They return an O3M_EDIT_* code: OK, RANGE (an instrument outside 1..64, an index or id the instrument does not have, `out` NULL), NO_PROJECT. Like get_steps they
   * show the project as it is: a write of an open group is not seen until the group closes. instrument_info answers OK for an empty slot (kind EMPTY, no parameters). */
  int32_t (*instrument_info)(uint8_t instrument, o3m_instrument_info* out);
  /* The descriptor of parameter `index` (0 .. param_count - 1) of the instrument. */
  int32_t (*get_param_desc)(uint8_t instrument, uint16_t index, o3m_param_desc* out);
  /* The parameter `id`'s value, in its own unit. A module parameter the project has not written yet reads as the module's default. */
  int32_t (*get_instrument_param)(uint8_t instrument, uint16_t id, float* out);
} o3m_read_api;

#define O3M_EDIT_API_VERSION 1u
/* Return codes of the edit table (and of the read table's pattern_info / get_steps). */
#define O3M_EDIT_OK 0
#define O3M_EDIT_RANGE 1      /* the value is outside what the project accepts: a pattern that does not exist, a track or row outside the pattern, a count of 0, a NULL pointer, an invalid value, an unknown field bit. The call writes nothing and the group stays valid */
#define O3M_EDIT_NO_PROJECT 2 /* no project is open */
#define O3M_EDIT_NO_GROUP 3   /* ABI 1.5: a write outside edit_begin / edit_end (there are no implicit units), or edit_end with no group open */
#define O3M_EDIT_GROUP_OPEN 4 /* ABI 1.5: edit_begin while a group is open */
#define O3M_EDIT_LIMIT 5      /* ABI 1.5: the group went over O3M_EDIT_MAX_CALLS or O3M_EDIT_MAX_STEPS: from that write on every write returns LIMIT, and edit_end returns LIMIT with the WHOLE group undone (the project is exactly as before it) */
#define O3M_EDIT_RATE 6       /* ABI 1.5: edit_begin when this call already made a group, or from view_draw / enter / exit / set_param (a group is made from input() only) */
#define O3M_EDIT_MAX_CALLS 4096u /* write calls (set_step, set_steps, set_tempo) in one group */
#define O3M_EDIT_MAX_STEPS 2048u /* steps written in one group (16 KB of o3m_step, the undo journal's cap) */
#define O3M_EDIT_MAX_PARAM_WRITES 64u /* ABI 1.6: set_instrument_param calls that were accepted in one group (the rings that carry a change to the engine and to a module instance take no more without dropping one). The 65th is LIMIT and the whole group is dropped. A sample has 61 writable parameters, a module at most 32 */
/* The edit table (o3m_host.edit; needs O3M_CAP_EDIT_PROJECT). An app never saves the project: the user does.
 *
 * GROUPS (ABI 1.5). A write is made inside a group: edit_begin, any number of set_step / set_steps / set_tempo, edit_end. The writes are CHECKED when called (a bad argument returns an error and
 * writes nothing) and APPLIED TOGETHER when edit_end is called, as ONE undo unit named after the app (the user's UNDO takes the whole group; REDO gives it back): the project's dirty flag, the sequencer
 * and the undo history follow exactly as for a hand edit. Edits while the song plays are allowed. At most one group is open at a time and at most one group is made per call into the app (a group
 * is made from input()); a group that goes over the limits is dropped whole. A group left open when the app's call returns is closed by the OS, its changes kept and applied, and it is counted
 * (never blocks the user's UNDO). Without a group set_tempo keeps its H1 meaning: queued, its own undo unit. Inside a group set_tempo is part of the group's unit.
 *
 * The old (ABI 1.3) member set_tempo is first and does not move. */
typedef struct o3m_edit_api {
  uint32_t version; /* O3M_EDIT_API_VERSION */
  uint32_t size;    /* sizeof(o3m_edit_api) */
  /* Sets the project's tempo, BPM x 100, 4000..80000. Returns an O3M_EDIT_* code (OK: queued, or taken into the open group). */
  int (*set_tempo)(uint32_t bpm_x100);
  /* ABI 1.5 (absent when `size` is below the offset). */
  int32_t (*edit_begin)(void);
  /* Closes the group and applies it. OK: applied (or empty: no undo unit is made). LIMIT: dropped whole. NO_GROUP: none was open. */
  int32_t (*edit_end)(void);
  /* Writes the masked fields (O3M_STEP_*) of `*s` to the step at (pattern, track, row). Pattern ids are 1..255 and must exist; `track` is below the project's active tracks; `row` below the
   * track's length. Returns OK, RANGE, NO_GROUP or LIMIT. */
  int32_t (*set_step)(uint16_t pattern, uint8_t track, uint8_t row, const o3m_step* s, uint8_t fields);
  /* The same for `count` consecutive steps (1..128, inside the track's length) from `first_row`. One call counts as one write and `count` steps. */
  int32_t (*set_steps)(uint16_t pattern, uint8_t track, uint8_t first_row, const o3m_step* s, uint8_t count, uint8_t fields);
  /* ABI 1.6 (absent when `size` is below the offset). Sets a parameter of a SAMPLE or MODULE instrument, inside the open group: the same path as the Params page (the engine hears it as it hears a hand edit), part of the
   * group's one undo unit, applied at edit_end (a read before then still shows the old value). Returns OK, NO_GROUP (no group open), LIMIT (the group is over a limit, also O3M_EDIT_MAX_PARAM_WRITES accepted parameter writes: this call counts as a write and takes one of the
   * O3M_EDIT_MAX_STEPS slots of the group's buffer), or RANGE and writes nothing: an instrument that is empty, MIDI or a module that is not loaded, an unknown id, a parameter that is not O3M_PD_APP_WRITABLE,
   * NaN, or a value outside min..max (nothing is clamped for you). A whole-number field stores the value rounded. Undo restores the exact float bits. */
  int32_t (*set_instrument_param)(uint8_t instrument, uint16_t id, float value);
} o3m_edit_api;

/* One MIDI message that came in on DIN or USB, as io.midi_read returns it. `port` 0 is DIN, 1 is USB. `status` is the status byte (0x80..0xEF: the channel is its low nibble),
 * `data1` and `data2` the data bytes; a message with one data byte (program change, channel pressure) has data2 = 0. A note on with velocity 0 is delivered as it came (status 0x9n). */
typedef struct o3m_midi_event {
  uint8_t port;
  uint8_t status;
  uint8_t data1;
  uint8_t data2;
} o3m_midi_event;

#define O3M_IO_API_VERSION 1u
/* The audio tap (ABI 1.4, O3M_CAP_AUDIO_TAP, ADR 0014 amendment E). ONE mono tap, shared by whoever asks (one app runs at a time): the mono mix, ((L + R + 1) >> 1), of the INPUT
 * bus (after the input strip's level) or of the MASTER output (what is heard). Samples are int16 at host->sample_rate (44100). The render writes it a block of O3M_TAP_BLOCK frames at a
 * time into a ring of 64 blocks (93 ms) and never waits for you; you read it from the main loop. A call made while the tap is off, or without the data asked for, returns 0.
 * The tap is switched on by tap_select and off again by the OS when the app exits. Decimated views (a tuner at 11 kHz, a spectrum) are yours to compute from these frames, in your
 * own frame: use tap_stream for a gapless signal, tap_latest for a window. */
#define O3M_TAP_OFF 0u
#define O3M_TAP_INPUT 1u
#define O3M_TAP_MASTER 2u
#define O3M_TAP_BLOCK 64u      /* frames the render writes at a time */
#define O3M_TAP_LATEST_MAX 2048u /* the most frames one tap_latest call returns */
#define O3M_TAP_STREAM_MAX 4032u /* the most frames one tap_stream call returns (63 blocks: all the ring can promise) */
typedef struct o3m_tap_levels {
  uint32_t size;        /* in: sizeof your struct. The OS fills the members that fit */
  uint32_t frames;      /* frames measured since your last tap_levels call (or since tap_select): 0 = nothing new, the other members are 0 */
  uint32_t lost_frames; /* frames before those that you were too slow for (a gap of more than 90 ms between calls) */
  uint32_t clip_blocks; /* of the measured blocks (64 frames each), how many had a sample at full scale (|x| >= 32767) */
  float peak;           /* the largest |sample| in them, linear, 1.0 = full scale (32768) */
  float rms;            /* sqrt(mean of the squares) over them, linear, 1.0 = full scale */
} o3m_tap_levels;

/* ---- Export writes (ABI 1.7, needs O3M_CAP_EXPORT_FILES; ADR 0014 amendment A, docs/specs/OS-H4.md) ----
 * An app writes files into the folder AppExport/<app>/ of the card (<app> is the app's file name without .tmod), one file at a time, and nowhere else. The four calls NEVER BLOCK: export_write copies
 * into a 16 KiB staging ring and answers BUSY when it is full (call again on a later frame with the same bytes: nothing was taken), the OS drains the ring to the card in the main loop's job pass, and
 * export_close only asks for the commit. The file is written under a temporary name, synced, read back and checked, and only then renamed to <name>.<ext>: a power cut, a pulled card or a failure
 * leaves no half-written file under the final name, and a name that is taken is never replaced (the new file becomes <name>_001.<ext>, ... _999). export_status tells you when it is done.
 * LIMITS. The extension must be one of mid, syx, txt, csv, json (lower case, no dot); <name> follows the project's file-name rules (letters, digits, space, '_', '-' and '.', 1..31 bytes, no path).
 * A file is at most 4 MiB, an app launch writes at most 16 files, and a file that is still open or committing when the app exits (or is stopped for its time budget) is aborted: nothing of it is left.
 * export_write refuses (RANGE) a single call larger than the ring, so feed it in chunks of 1 KiB or so. Files that are already DONE stay. Call the four from input() or view_draw(): they are quick. */
#define O3M_EXPORT_OK 0
#define O3M_EXPORT_BUSY 1    /* the staging ring is full: call again on a later frame with the same bytes; nothing was taken */
#define O3M_EXPORT_RANGE 2   /* a bad name, an extension not on the list, a NULL buffer, n == 0, or a write larger than the ring */
#define O3M_EXPORT_LIMIT 3   /* over a cap: the file's 4 MiB (the file is aborted), the 16 files of a launch, or no free suffix (999 taken) */
#define O3M_EXPORT_STATE 4   /* export_open while a file is open or committing; export_write / export_close with no file open */
#define O3M_EXPORT_NO_CARD 5 /* no card, a card that may not be written (write protected, exFAT), or the save layer is busy with a save it cannot share */
#define O3M_EXPORT_IDLE 0    /* export_status: nothing open, nothing committing (also after the app has read DONE or FAILED once) */
#define O3M_EXPORT_WRITING 1 /* open, or closed and still committing */
#define O3M_EXPORT_DONE 2    /* the last file is whole on the card under its final name */
#define O3M_EXPORT_FAILED 3  /* the last file is not on the card (nothing of it is left: the Temp was removed) */
#define O3M_EXPORT_RING_BYTES 16384u /* the staging ring: the most one export_write call may take */
#define O3M_EXPORT_MAX_FILE_BYTES 4194304u
#define O3M_EXPORT_MAX_FILES 16u

/* ---- Transport and being heard (ABI 1.10; OS-APPUX S3) ----
 * get_transport copies the play position the sequencer publishes (the same word the Pattern's play row follows), read at the call: `out->size` first, set to sizeof(o3m_transport); the OS fills the
 * members that fit. Returns 0, or 1 when this OS has no sequencer position (the members are then 0, `order` and `queued` 0xFFFF). It reveals no project content beyond get_tempo.
 * play_note starts a note of `instrument` (1..64) at `note` (0..127) and `velocity` (1..127) through the project's instruments, the way a pad does, and stop_note ends every note of the app on that instrument.
 * LIMITS: four notes held at once, eight play_note / stop_note calls a frame (the OS's pass); beyond them the answer is O3M_PLAY_LIMIT and nothing happens. Every note of the app ends when its page closes
 * (F8, a screen button, F7 + F8, a stop for the time budget, a project change). A note sounds on a sample or a module instrument; a MIDI instrument does not sound from an app (it is not routed). */
typedef struct o3m_transport {
  uint32_t size;       /* sizeof(o3m_transport): the app sets it, the OS fills the members up to it */
  uint32_t playing;    /* 1 while the sequencer plays, else 0 */
  uint32_t pattern;    /* the pattern the playhead is in (0 before the first publish) */
  uint32_t order;      /* its position in the song's order list, 0..254; 0xFFFF in Pattern mode */
  uint32_t row;        /* the playhead's step in that pattern, 0..127 (the Pattern's row, 0 based) */
  uint32_t ustep;      /* the microstep of that step the playhead is in, 0..47 */
  uint32_t tempo_x100; /* BPM x 100 in force (0 before the first publish) */
  uint32_t count_in;   /* the steps of the count-in still to run, the running one included; 0: not counting in */
  uint32_t queued;     /* the song position a live jump waits for; 0xFFFF: none */
} o3m_transport;

#define O3M_PLAY_OK 0
#define O3M_PLAY_RANGE 1     /* a bad instrument, note or velocity */
#define O3M_PLAY_LIMIT 2     /* four notes are held, or eight calls were made this frame */
#define O3M_PLAY_NO_AUDIO 3  /* the note did not go: this OS has no live-note path behind it (a build with no engine), or the OS could not queue it now (the audio is stopped, the live ring is full); nothing is held */
#define O3M_PLAY_MAX_HELD 4u
#define O3M_PLAY_MAX_PER_FRAME 8u
#define O3M_OWN_OK 0
#define O3M_OWN_RANGE 1  /* an index past the app's declared rows, or a NaN or an infinity */

/* The io table (o3m_host.io; lent to every app that declares ABI 1.3). */
typedef struct o3m_io_api {
  uint32_t version; /* O3M_IO_API_VERSION */
  uint32_t size;    /* sizeof(o3m_io_api) */
  /* A monotonic clock in milliseconds since the device started. It WRAPS every 49.7 days: measure spans with an unsigned subtraction (now - then), never compare two readings with <.
   * The resolution is the clock's (a millisecond); the call is only as steady as the main loop that makes it (taps are timed by their input event's call, 16..33 ms of jitter at worst). */
  uint32_t (*now_ms)(void);
  /* NULL without O3M_CAP_MIDI_IN. Copies up to `max` of the messages that arrived since the last call (channel messages, oldest first; real-time bytes, system common and SysEx are not
   * delivered) into `out` and returns how many. The ring holds 64: drain it at least once a frame (view_draw is the natural place). Messages are collected only while the app runs. */
  uint32_t (*midi_read)(o3m_midi_event* out, uint32_t max);
  /* NULL without O3M_CAP_MIDI_IN. What was lost since the app started: `*overflow` messages that did not fit in the ring, `*sysex` SysEx messages that were dropped (counted, never
   * delivered). Either pointer may be NULL. */
  void (*midi_lost)(uint32_t* overflow, uint32_t* sysex);
  /* ABI 1.4. All four are NULL without O3M_CAP_AUDIO_TAP (or in a build with no tap), and absent when `size` is below their offset. */
  /* Chooses the tap point: O3M_TAP_OFF, O3M_TAP_INPUT or O3M_TAP_MASTER. Returns 0, or 1 for any other value (nothing changes). Selecting a point restarts your cursors two blocks (2.9 ms)
   * after "now" (the render may still have been writing the old point): tap_levels reports from there, tap_latest hands back zeros in front of it, and tap_stream reports from there once
   * the sample feed runs; until your first tap_latest / tap_stream call it begins at that call (see tap_stream). Selecting the same point again restarts the cursors too. */
  int (*tap_select)(uint32_t source);
  /* The newest `frames` frames of the tap into `out`, oldest first (a scope, a spectrum window). `frames` is even and 2..O3M_TAP_LATEST_MAX, `out` 4-byte aligned (alignas(4)), else 0 is
   * returned and nothing written. Returns `frames` when the window is whole; before that many frames exist since tap_select the front is zero and the return is the number of real
   * frames at the end. 0: tap off, nothing yet, or a bad argument. The OS stores the samples only once an app asks for them: the first tap_latest or tap_stream call starts that and returns
   * 0, there is no history before it, and the real frames begin two blocks (2.9 ms) after it. Poll every frame and treat 0 as "not yet" (an app that reads only tap_levels never costs
   * the render the stores). This is an observable change of ABI 1.4 with no minor bump: a first call returning 0 was already legal. */
  uint32_t (*tap_latest)(int16_t* out, uint32_t frames);
  /* Everything new since your last tap_stream call (for the first call: since the sample feed started, see below), oldest first, up to `max_frames` (rounded down to an even number, at most
   * O3M_TAP_STREAM_MAX), into `out` (4-byte aligned). Returns the frames written. If more than that is waiting, the OLDEST are dropped and `*lost_frames` (may be NULL) gets how many (they
   * are not returned, the signal has a gap there); a reader that calls every frame with room for 2048 frames never loses any. Use this for a gapless signal (a tuner decimating to 11 kHz).
   * The first call (of this or tap_latest) starts the sample feed and returns 0: the stream begins two blocks (2.9 ms) after it, not at tap_select, and nothing older is kept; poll every frame. */
  uint32_t (*tap_stream)(int16_t* out, uint32_t max_frames, uint32_t* lost_frames);
  /* Peak, RMS and clip blocks of everything since your last tap_levels call (or tap_select), measured by the render on every block whether or not you read the samples. `out->size` first. */
  void (*tap_levels)(o3m_tap_levels* out);
  /* ABI 1.7. All four are NULL without O3M_CAP_EXPORT_FILES (or in a build with no card writer), and absent when `size` is below their offset. They return an O3M_EXPORT_* code (export_status a state). */
  /* Starts the file AppExport/<app>/<name>.<ext>. `name` is the stem, `ext` one of "mid", "syx", "txt", "csv", "json". A name that is taken gets "_001" ... "_999" (the stem is cut to fit 31 bytes).
   * OK: the file is open and you may write. RANGE, LIMIT, STATE and NO_CARD as above. */
  int32_t (*export_open)(const char* name, const char* ext);
  /* Copies `n` bytes (1 .. O3M_EXPORT_RING_BYTES) of the open file. BUSY: nothing was taken, call again later with the same bytes. LIMIT: this would take the file past 4 MiB, the file is aborted (FAILED). */
  int32_t (*export_write)(const void* buf, uint32_t n);
  /* The file is complete: asks for the commit and returns at once. Poll export_status until it is DONE or FAILED. */
  int32_t (*export_close)(void);
  /* IDLE, WRITING, DONE or FAILED. Reading DONE or FAILED returns the state to IDLE. `final_name` (may be NULL, `cap` bytes) gets "<stem>.<ext>" as written, on DONE. */
  uint32_t (*export_status)(char* final_name, uint32_t cap);
  /* ABI 1.10 (OS-APPUX S3). NULL for an app that declares a lower minor; absent when `size` is below their offset. play_note and stop_note are also NULL without O3M_CAP_PLAY_NOTES. */
  /* The play position (see o3m_transport). Free: no capability. 0, or 1 when there is no sequencer position to read. */
  int32_t (*get_transport)(o3m_transport* out);
  /* Stores the value of your declared row `index` (the o3m_param at that index of the descriptor) for this project: the OS clamps it to the row's range, rounds an INT row, keeps it with the project
   * and hands it back to set_param() the next time the app is launched, before enter(). It does NOT call your set_param() (you hold the value: that is the echo it leaves out). O3M_OWN_OK, or O3M_OWN_RANGE. The OS keeps the rows with the project while the page runs: a call made inside exit() is not kept. */
  int32_t (*set_own_param)(uint32_t index, float value);
  /* Plays `note` of `instrument` at `velocity` until stop_note() (see the limits above). An O3M_PLAY_* code. A note you already hold with the same instrument and note is started again. */
  int32_t (*play_note)(uint32_t instrument, uint32_t note, uint32_t velocity);
  /* Ends every note you hold on `instrument` (1..64). O3M_PLAY_OK, also when none is held; O3M_PLAY_RANGE for a bad instrument; O3M_PLAY_LIMIT past the frame's eight calls. */
  int32_t (*stop_note)(uint32_t instrument);
} o3m_io_api;

/* THE CLOCK (ABI 1.12, OS-INSFX-ABI2). What o3m_host.get_clock fills: `size` first, set by the caller to sizeof(o3m_clock). `flags` is O3M_CLOCK_*. `tempo_x100` is the BPM x 100 in force (what plays, half and
 * double time included; the project's tempo while stopped). `beat` is the playhead at the block's first sample, in beats of 4 steps counted from step 0 of the pattern that plays (0 while stopped). */
#define O3M_CLOCK_PLAYING 1u /* the sequencer plays */
#define O3M_CLOCK_STARTED 2u /* a run started (the first block that sees it: the OS reads the sequencer at the previous block's end) */
#define O3M_CLOCK_WRAPPED 4u /* the beat is lower than the previous block's: a new pattern or a jump back */
typedef struct o3m_clock {
  uint32_t size;       /* sizeof(o3m_clock): the caller sets it, the OS fills the members up to it */
  uint32_t flags;      /* O3M_CLOCK_* */
  uint32_t tempo_x100; /* BPM x 100 */
  float beat;          /* beats since step 0 of the pattern that plays */
} o3m_clock;

/* The services the OS gives a module (the pointer passed to init() stays valid until shutdown()). New members are only ever
 * added at the end; check `size` before using one the version you built against lacks. */
typedef struct o3m_host {
  uint32_t abi_major; /* of the OS */
  uint32_t abi_minor;
  uint32_t size;      /* sizeof(o3m_host) as the OS built it */
  float sample_rate;  /* Hz of every render() */
  void (*log)(const char* text); /* one line to the device's log; init() and shutdown() only */
  float (*exp2_f)(float x);      /* 2^x, relative error < 1e-6 for x in [-126, 127] */
  float (*log2_f)(float x);      /* log2(x), absolute error < 2e-6 for x > 0 */
  float (*sin_f)(float x);       /* radians; absolute error < 1e-6 for |x| < 1000, relative < 1e-6 for small angles; 0 for NaN / infinity */
  float (*cos_f)(float x);
  const struct o3m_draw_api* draw; /* the drawing table lent to modules (see above); NULL in ABI 1.0 OSes, always set from 1.1 */
  /* ABI 1.3 (apps; each NULL when not granted, and the members do not exist when `size` is below their offset: use O3M_HOST_HAS). */
  const struct o3m_read_api* read; /* O3M_CAP_READ_PROJECT */
  const struct o3m_edit_api* edit; /* O3M_CAP_EDIT_PROJECT */
  const struct o3m_io_api* io;     /* every app of ABI 1.3 */
  /* ABI 1.12 (OS-INSFX-ABI2; every kind but the importer, whose table is the 52 bytes above). The members do not exist when `size` is below their offset: use O3M_HOST_HAS. */
  /* get_clock: the tempo in force and the beat position, as of this block (see o3m_clock); `out->size` is set by the caller to sizeof(o3m_clock) and the OS fills the members that fit. Returns 0, or 1 when
   * the OS has no sequencer to ask (the members are then 0 and tempo_x100 is the project's). Render context. */
  int32_t (*get_clock)(o3m_clock* out);
  /* track_level: the linear peak, 0.0 .. 1.0, of track `track` (0..15), the sidechain key (see O3M_PARAM_KEY): 0 for a silent track, a bad index or a track no effect names as its key. Render context. */
  float (*track_level)(uint32_t track);
} o3m_host;

/* Whether the OS's host table has the member `m` at all (an OS older than the SDK you built with has a smaller table). A table can still be NULL: not granted. */
#define O3M_HOST_HAS(host, m) ((host)->size >= __builtin_offsetof(o3m_host, m) + sizeof((host)->m))
/* The same for a member of the read or edit table (ABI 1.5 and later add members, so check the one you call, never sizeof of the whole table: an app that uses only the 1.5 members must still run on an
 * OS of 1.5). `t` is the table pointer: O3M_TABLE_HAS(host->read, get_steps). */
#define O3M_TABLE_HAS(t, m) ((t)->size >= __builtin_offsetof(__typeof__(*(t)), m) + sizeof((t)->m))

/* PAGES AND LABELS (ABI 1.8, OS-INSTUI S10). The params screen shows a module's parameters eight to a page. Without a page table (`pages` NULL) the pages are called "1-8", "9-16", ... as before;
 * with one, the module names them and chooses where each starts. `pages` is `page_count` entries (1..O3M_PAGES_MAX, zero exactly when `pages` is NULL, and zero when the module has no parameters):
 * entry k names the page that starts at parameter `first_param`. The first page starts at 0, the pages ascend strictly, each holds 1..O3M_PAGE_PARAMS_MAX parameters, and together they cover the
 * module's parameters exactly. `labels` is NULL or `param_count` entries parallel to `params`: the entry of an O3M_PARAM_INT parameter whose values are names (a waveform, a mode) has `count`
 * strings, one for each whole value from min to max (count = max - min + 1, O3M_LABELS_MIN..O3M_LABELS_MAX), `names[v - min]` is the text of value v; every other entry is {NULL, 0}. */
typedef struct o3m_page {
  const char* name;      /* 1..O3M_PAGE_NAME_MAX - 1 printable ASCII characters */
  uint32_t first_param;  /* the page's first parameter */
} o3m_page;

typedef struct o3m_labels {
  const char* const* names; /* count strings of 1..O3M_LABEL_NAME_MAX - 1 printable ASCII characters, or NULL when count is 0 */
  uint32_t count;
} o3m_labels;

/* FACTORY PRESETS (ABI 1.12, OS-INSFX-ABI2): `presets` is `preset_count` entries (0..O3M_PRESETS_MAX, zero exactly when `presets` is NULL, and zero for an app, an importer and a module with no parameters). A preset
 * sets EVERY parameter: `values` holds `count` floats in parameter order (1..param_count), each finite and within its parameter's range, and the parameters past `count` take their default. The OS applies a
 * preset through set_param(). Instruments and effects only. */
typedef struct o3m_preset {
  const char* name;     /* 1..O3M_PRESET_NAME_MAX - 1 printable ASCII characters */
  const float* values;  /* `count` floats, in the order of o3m_module.params */
  uint32_t count;       /* 1..param_count */
} o3m_preset;

/* THE IMPORTER (ABI 1.11, OS-FLASHDIET I1/I2, kind 3). An importer reads tracker module files (MOD, S3M, OctaMED ...) and turns them into a project: it is not heard and not
 * seen, it has no parameters and no views, it never binds to a track. Its table, `o3m_module.importer`, says which files it serves and holds its calls: begin is handed the opened
 * file's read window (below), the OS's model arena and the import options, step drives the job, read_pcm serves the samples, report copies the import's report out and close ends it.
 * FIRST-PARTY: the importer is compiled against the OS's own model headers, so the OS version-locks it through the file header's word 44 (the layout tag the packer writes; see the file header
 * below): a kind 3 file whose tag is not the OS's own is refused (LayoutMismatch), never run. */
#define O3M_IMPORTER_FORMATS_MAX 8u  /* entries in the importer's format list */
#define O3M_EXT_MAX 8u                /* bytes of a served file extension, with the NUL */

/* step()'s verdicts. */
#define O3M_IMPORT_RUNNING 0u         /* the job goes on */
#define O3M_IMPORT_DONE 1u            /* the project is in */
#define O3M_IMPORT_FAILED 2u          /* the file was refused; the report says why */

typedef struct o3m_format_desc {
  const char* ext;  /* the file extension it serves, 1..O3M_EXT_MAX - 1 printable ASCII characters, no dot: "mod", "s3m", "med" */
} o3m_format_desc;

/* The read window over the opened file that the OS lends the importer at begin (its `file` argument): `read(ctx, offset, dst, n)` copies n bytes at `offset`
 * into `dst` and returns 0 on success, negative on a card error; `size(ctx)` is the file's length in bytes. The OS keeps the file open until close(). */
typedef struct o3m_file_window {
  uint32_t struct_size;  /* sizeof(o3m_file_window) */
  void* ctx;             /* the OS's window object */
  int32_t (*read)(void* ctx, uint32_t offset, void* dst, uint32_t n);
  uint32_t (*size)(void* ctx);
} o3m_file_window;

typedef struct o3m_importer {
  uint32_t size;                      /* sizeof(o3m_importer) */
  const o3m_format_desc* formats;     /* format_count entries; format k is the k-th the OS offers the file to */
  uint32_t format_count;              /* 1..O3M_IMPORTER_FORMATS_MAX */
  /* begin: start importing the opened file of format `format` (an index into `formats`) into `model_arena` (the OS's own arena: the module is first-party and
     version-locked, so it writes the project through the OS's model headers it was built against). `file` is the OS's o3m_file_window over the opened file,
     `options` / `options_size` the OS's import options blob (the module reads the members its build knows). Returns 0, or refuses the import. */
  int (*begin)(void* state, const o3m_file_window* file, uint32_t format, void* model_arena, const void* options, uint32_t options_size);
  /* step: do about `budget` units of work; returns O3M_IMPORT_*. Main loop only, never blocking. */
  uint32_t (*step)(void* state, uint32_t budget);
  /* read_pcm: `frames` samples of instrument slot `slot` from frame `first` into `dst` (mono int16); negative: refused. */
  int32_t (*read_pcm)(void* state, uint32_t slot, uint32_t first, uint32_t frames, int16_t* dst);
  /* report: copy the import's report out into `out`, at most `size` bytes (the shape both sides compiled against under the word 44 lock). Returns the bytes written. */
  uint32_t (*report)(void* state, void* out, uint32_t size);
  /* close: the PCM phase is over: the module releases the file window. The OS unloads the module itself. */
  void (*close)(void* state);
} o3m_importer;

/* PITCH (ABI 1.8, OS-MODPITCH). The host owns the Slide (41 / 42) and Glide (19) FX of a pattern: it holds the tempo, the glide time and the previous note, and it RAMPS. The module only follows a
 * number. `pitch(state, semitones, per_frame)` is called in the render context (like note_on()). From the next frame of the module's render() the pitch offset is `semitones` and it moves by `per_frame`
 * each frame, linearly, until the next pitch() or note_on(): the OFFSET FROM THE NOTE given to the last note_on(), in semitones, positive up. A module that does not want the slope may ignore
 * `per_frame` (a staircase of one block). The host guarantees finite numbers with |semitones| <= 128 and |per_frame| <= 128; the module still clamps its own frequency. A note_on() puts the offset back to 0;
 * a note that starts on a ramp is followed by a pitch() call at once (a Glide from 60 to 67 starts at -7.0). A Glide or Slide note on a module is LEGATO, as a TB-303 slide: the host does not release the
 * held note first; it calls note_on(note, O3M_NOTE_LEGATO | O3M_NOTE_RAMP) and then pitch() (with no note held, note_on(note, O3M_NOTE_RAMP)). One call a block for a ramp in progress; none when nothing
 * sounds. A module that exports no `pitch` (NULL, or any module of ABI 1.7 or below) is never called and never gets O3M_NOTE_RAMP: the FX are ignored for it as before. */

#ifdef __cplusplus
#define O3M_DEFAULT(value) = value
#else
#define O3M_DEFAULT(value)
#endif

/* The size of the descriptor a module of ABI 1.`minor` has: 64 bytes up to 1.7 (the members up to pitch), 80 from 1.8 (pitch, pages, page_count, labels), 84 from 1.11
 * (`importer`, the importer kind's table; it is NULL in every other kind), 100 from 1.12 (render_st, fx_flags, presets, preset_count). The OS refuses a descriptor whose struct_size is another number (the
 * loader and add_native). Use this macro, not sizeof(o3m_module), for the struct_size of an entry: the generated headers do. Evaluates to a constant on the host as on the target. */
#define O3M_MODULE_SIZE(minor) \
  ((minor) >= 12u ? sizeof(o3m_module) : ((minor) >= 11u ? __builtin_offsetof(o3m_module, render_st) : ((minor) >= 8u ? __builtin_offsetof(o3m_module, importer) : __builtin_offsetof(o3m_module, pitch))))

/* The module's descriptor: `o3m_entry`, a const object with C linkage. */
typedef struct o3m_module {
  uint32_t abi_major; /* O3M_ABI_MAJOR / O3M_ABI_MINOR of the SDK the module was built with */
  uint32_t abi_minor;
  uint32_t kind;      /* O3M_KIND_* */
  uint32_t struct_size;  /* sizeof(o3m_module) */
  const char* name;      /* up to O3M_NAME_MAX - 1 characters */
  uint32_t state_bytes;  /* per instance: the block init() and everything after it work in */
  uint32_t param_count;  /* 0 to O3M_PARAMS_MAX */
  const o3m_param* params;
  /* init: `state` is state_bytes of zeroed, 16-byte aligned memory. Returns 0 on success; anything else refuses the instance. */
  int (*init)(void* state, const o3m_host* host);
  /* render: writes `frames` samples to `out` (an instrument: `in` is NULL; an effect: `in` holds the input; in and out may be
   * the same buffer). Returns 1 while the module may still sound (a note is held, a tail is running) and 0 when it is silent
   * and idle, which lets the OS skip it until the next note_on (the first call after a note_on is always made). An instrument's samples are floats with 1.0 = full scale: the OS scales them by
   * 32768, clips them to 16 bits (and counts the clips) and turns a NaN into silence. */
  uint32_t (*render)(void* state, const float* in, float* out, uint32_t frames);
  /* set_param: `id` is an index into params; `value` is as stored (the OS does not clamp: the module must). */
  void (*set_param)(void* state, uint32_t id, float value);
  /* note_on: `note` 0..127, `velocity` 1..127, `flags` O3M_NOTE_*. */
  void (*note_on)(void* state, uint32_t note, uint32_t velocity, uint32_t flags);
  void (*note_off)(void* state, uint32_t note);
  void (*shutdown)(void* state);
  const o3m_ui* ui;      /* the views' entry points: NULL exactly when view_count is 0 (see above) */
  uint32_t view_count;   /* 0..O3M_VIEWS_MAX */
  /* ---- 64 bytes up to here: the descriptor of ABI 1.0..1.7. From here on the members exist only when the module declares ABI 1.8 (struct_size 80); in a lower minor they are not read. ---- */
  /* In C++ the four default to NULL / 0, so that a descriptor written as a list of the first 16 members (every module of 1.7 and below) still compiles warning-free under -Wextra; in C list them. */
  void (*pitch)(void* state, float semitones, float per_frame) O3M_DEFAULT(nullptr); /* the host's Slide / Glide ramp (see PITCH); NULL = no pitch input */
  const o3m_page* pages O3M_DEFAULT(nullptr);  /* the params pages (see PAGES AND LABELS); NULL = the pages "1-8", "9-16", ... */
  uint32_t page_count O3M_DEFAULT(0);          /* 0..O3M_PAGES_MAX, zero exactly when `pages` is NULL */
  const o3m_labels* labels O3M_DEFAULT(nullptr); /* NULL, or param_count entries parallel to `params` */
  /* ---- 80 bytes up to here: the descriptor of ABI 1.8..1.10. From here on the member exists only when the module declares ABI 1.11 (struct_size 84); in a lower minor it is not read. ---- */
  const o3m_importer* importer O3M_DEFAULT(nullptr); /* ABI 1.11, kind 3 only: the importer's table (see THE IMPORTER); NULL in every other kind */
  /* ---- 84 bytes up to here: the descriptor of ABI 1.11. From here on the members exist only when the module declares ABI 1.12 (struct_size 100); in a lower minor they are not read. ---- */
  /* In C++ the four default to NULL / 0, so a descriptor written as a list that ends at `importer` or earlier still compiles; in C list them (an effect's `importer` is NULL). */
  uint32_t (*render_st)(void* state, const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t frames) O3M_DEFAULT(nullptr);
                                                  /* ABI 1.12, effects only: the stereo render (`in_*` and `out_*` may be the same buffers; the return value is render()'s; frames 1..64). NULL: a mono effect */
  uint32_t fx_flags O3M_DEFAULT(0);                /* ABI 1.12: O3M_FX_*; an effect's O3M_FX_WIDENS (needs render_st), an instrument's O3M_FX_RATE (1.13); every other bit 0 */
  const o3m_preset* presets O3M_DEFAULT(nullptr);  /* ABI 1.12, instruments and effects: the factory presets (see FACTORY PRESETS); NULL when none */
  uint32_t preset_count O3M_DEFAULT(0);            /* 0..O3M_PRESETS_MAX, zero exactly when `presets` is NULL */
} o3m_module;

/* The file on the card (<name>.tmod), little endian, built by tools/o3m_pack.py from the module's linked ELF:
 *
 *   o3m_file_header (64 bytes) | image (image_size bytes) | relocation table (reloc_count x u32)
 *
 * The image is the module as linked at address 0: the hot code first (hot_size bytes, the .o3m_hot section: the OS may place it
 * in SRAM), then everything else, read-only (.o3m_cold: code, constants, the descriptor). A relocation table entry is the
 * image offset of a 32-bit word that holds an absolute link-time address (image offset + the Thumb bit for a function); the
 * loader adds the place the part it points into was loaded at. The table is strictly ascending, 4-byte aligned offsets.
 * image_crc is the CRC-32 (zlib) of the whole file with the image_crc field read as zero.
 *
 *   offset  size  field
 *      0     4    magic
 *      4     2    header_size
 *      6     2    target
 *      8     1    kind
 *      9     1    abi_major
 *     10     1    abi_minor
 *     11     1    flags (0)
 *     12     4    hot_size
 *     16     4    image_size
 *     20     4    desc_offset
 *     24     4    state_bytes
 *     28     4    reloc_count
 *     32     2    param_count
 *     34     2    view_count
 *     36     4    module_version (1.2; reserved, 0, in a file of ABI 1.0 and 1.1)
 *     40     4    capabilities (1.3: O3M_CAP_*, an app's request; reserved, 0, in a file of ABI 1.2 and below)
 *     44     4    reserved2[1]: 1.11, KIND 3 ONLY, the importer layout tag (written by the packer; the OS refuses a kind 3 file whose word is not its own
 *                tag, LayoutMismatch). Reserved, 0, in every other file: a non-zero word 44 in an instrument, an effect, an app or a kind 3 file of a minor below 1.11 is refused
 *     48     4    image_crc
 *     52    12    reserved3 (0) */
#define O3M_FILE_MAGIC 0x444D334Fu /* "O3MD" */
#define O3M_FILE_HEADER_SIZE 64u

typedef struct o3m_file_header {
  uint32_t magic;
  uint16_t header_size; /* O3M_FILE_HEADER_SIZE */
  uint16_t target;      /* O3M_TARGET_* */
  uint8_t kind;
  uint8_t abi_major;
  uint8_t abi_minor;
  uint8_t flags;        /* 0 */
  uint32_t hot_size;    /* multiple of 4, at most image_size */
  uint32_t image_size;  /* multiple of 4 */
  uint32_t desc_offset; /* image offset of o3m_entry (in the cold part, 4-byte aligned) */
  uint32_t state_bytes; /* the descriptor's, repeated so that the loader can refuse before it places anything */
  uint32_t reloc_count;
  uint16_t param_count; /* likewise */
  uint16_t view_count;  /* the descriptor's, repeated (0..O3M_VIEWS_MAX) */
  uint32_t module_version; /* 1.2: O3M_VERSION(major, minor, patch) of the module, 0 = unversioned; must be 0 when abi_minor is below 2 (a reserved word until then) */
  uint32_t reserved2[2]; /* [0]: capabilities (ABI 1.3, apps only, O3M_CAP_*; must be 0 when abi_minor is below 3); [1]: the importer layout tag (ABI 1.11, kind 3 only; must be 0 otherwise) */
  uint32_t image_crc;
  uint32_t reserved3[3]; /* 0 */
} o3m_file_header;

/* THE ABI'S LAYOUT, written down (OS-H3A). On the target (32-bit pointers: every module is built here) the byte offset of every member of the host tables, and the size of every table and POD, is
 * a compile-time fact, so a change that moves a published member, or grows a struct that is not allowed to, stops the build of every module instead of corrupting an app on the unit. New members
 * are appended after the last one and get their line here; none of the numbers below changes. (The same offsets on a 64-bit host are checked by tests/module_edit_test.cpp.) */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4
#define O3M_ABI_ASSERT(name, cond) typedef char o3m_abi_assert_##name[(cond) ? 1 : -1]
#define O3M_OFFSET(type, member) __builtin_offsetof(type, member)
O3M_ABI_ASSERT(host_1_2, O3M_OFFSET(o3m_host, draw) == 36);                                     /* 1.0..1.2: log 16, exp2 20, log2 24, sin 28, cos 32, draw 36 */
O3M_ABI_ASSERT(host_1_3, O3M_OFFSET(o3m_host, read) == 40 && O3M_OFFSET(o3m_host, edit) == 44 && O3M_OFFSET(o3m_host, io) == 48);  /* (1.3 ends at 52) */
O3M_ABI_ASSERT(read_1_3, O3M_OFFSET(o3m_read_api, get_tempo) == 8 && O3M_OFFSET(o3m_read_api, launch_ctx) == 12);
O3M_ABI_ASSERT(read_1_5, O3M_OFFSET(o3m_read_api, pattern_info) == 16 && O3M_OFFSET(o3m_read_api, get_steps) == 20);
O3M_ABI_ASSERT(read_1_6, O3M_OFFSET(o3m_read_api, instrument_info) == 24 && O3M_OFFSET(o3m_read_api, get_param_desc) == 28 && O3M_OFFSET(o3m_read_api, get_instrument_param) == 32 && sizeof(o3m_read_api) == 36);
O3M_ABI_ASSERT(edit_1_3, O3M_OFFSET(o3m_edit_api, set_tempo) == 8);
O3M_ABI_ASSERT(edit_1_5, O3M_OFFSET(o3m_edit_api, edit_begin) == 12 && O3M_OFFSET(o3m_edit_api, edit_end) == 16 && O3M_OFFSET(o3m_edit_api, set_step) == 20 && O3M_OFFSET(o3m_edit_api, set_steps) == 24);
O3M_ABI_ASSERT(edit_1_6, O3M_OFFSET(o3m_edit_api, set_instrument_param) == 28 && sizeof(o3m_edit_api) == 32);
O3M_ABI_ASSERT(io_1_3, O3M_OFFSET(o3m_io_api, now_ms) == 8 && O3M_OFFSET(o3m_io_api, midi_read) == 12 && O3M_OFFSET(o3m_io_api, midi_lost) == 16);
O3M_ABI_ASSERT(io_1_4, O3M_OFFSET(o3m_io_api, tap_select) == 20 && O3M_OFFSET(o3m_io_api, tap_latest) == 24 && O3M_OFFSET(o3m_io_api, tap_stream) == 28 && O3M_OFFSET(o3m_io_api, tap_levels) == 32); /* (1.4 ends at 36) */
O3M_ABI_ASSERT(io_1_7, O3M_OFFSET(o3m_io_api, export_open) == 36 && O3M_OFFSET(o3m_io_api, export_write) == 40 && O3M_OFFSET(o3m_io_api, export_close) == 44 && O3M_OFFSET(o3m_io_api, export_status) == 48); /* (1.7 ends at 52) */
O3M_ABI_ASSERT(io_1_10, O3M_OFFSET(o3m_io_api, get_transport) == 52 && O3M_OFFSET(o3m_io_api, set_own_param) == 56 && O3M_OFFSET(o3m_io_api, play_note) == 60 && O3M_OFFSET(o3m_io_api, stop_note) == 64 &&
                          sizeof(o3m_io_api) == 68);
O3M_ABI_ASSERT(transport_pod, O3M_OFFSET(o3m_transport, playing) == 4 && O3M_OFFSET(o3m_transport, pattern) == 8 && O3M_OFFSET(o3m_transport, order) == 12 && O3M_OFFSET(o3m_transport, row) == 16 &&
                              O3M_OFFSET(o3m_transport, ustep) == 20 && O3M_OFFSET(o3m_transport, tempo_x100) == 24 && O3M_OFFSET(o3m_transport, count_in) == 28 && O3M_OFFSET(o3m_transport, queued) == 32 &&
                              sizeof(o3m_transport) == 36);
O3M_ABI_ASSERT(launch_ctx, O3M_OFFSET(o3m_launch_ctx, block_length) == 24 && O3M_OFFSET(o3m_launch_ctx, block_tracks) == 28 && sizeof(o3m_launch_ctx) == 32); /* 1.3..1.4: 28 bytes, 1.5 appends block_tracks */
O3M_ABI_ASSERT(step_pods, sizeof(o3m_step) == 8 && sizeof(o3m_pattern_info) == 28);
O3M_ABI_ASSERT(module_1_8, O3M_OFFSET(o3m_module, view_count) == 60 && O3M_OFFSET(o3m_module, pitch) == 64 && O3M_OFFSET(o3m_module, pages) == 68 && O3M_OFFSET(o3m_module, page_count) == 72 &&
                           O3M_OFFSET(o3m_module, labels) == 76);  /* 1.0..1.7: 64 bytes (O3M_MODULE_SIZE), 1.8 appends pitch, pages, page_count, labels (80 bytes up to 1.10) */
O3M_ABI_ASSERT(module_1_11, O3M_OFFSET(o3m_module, importer) == 80 && O3M_OFFSET(o3m_module, render_st) == 84);  /* 1.8..1.10: 80 bytes, 1.11 appends importer (kind 3's table, NULL in the others) and ends at 84 */
O3M_ABI_ASSERT(module_1_12, O3M_OFFSET(o3m_module, render_st) == 84 && O3M_OFFSET(o3m_module, fx_flags) == 88 && O3M_OFFSET(o3m_module, presets) == 92 && O3M_OFFSET(o3m_module, preset_count) == 96 &&
                            sizeof(o3m_module) == 100);  /* 1.12 appends render_st, fx_flags, presets, preset_count (100 bytes) */
O3M_ABI_ASSERT(host_1_12, O3M_OFFSET(o3m_host, get_clock) == 52 && O3M_OFFSET(o3m_host, track_level) == 56 && sizeof(o3m_host) == 60);  /* 1.12 appends get_clock and track_level to the 52-byte table */
O3M_ABI_ASSERT(preset_pod, sizeof(o3m_preset) == 12 && O3M_OFFSET(o3m_preset, values) == 4 && O3M_OFFSET(o3m_preset, count) == 8);
O3M_ABI_ASSERT(clock_pod, sizeof(o3m_clock) == 16 && O3M_OFFSET(o3m_clock, flags) == 4 && O3M_OFFSET(o3m_clock, tempo_x100) == 8 && O3M_OFFSET(o3m_clock, beat) == 12);
O3M_ABI_ASSERT(importer_pods, sizeof(o3m_format_desc) == 4 && sizeof(o3m_file_window) == 16 && sizeof(o3m_importer) == 32);  /* 1.11: the importer's table is eight words */
O3M_ABI_ASSERT(ui_1_9, O3M_OFFSET(o3m_ui, exit) == 16 && O3M_OFFSET(o3m_ui, keys) == 20 && sizeof(o3m_ui) == 24 && sizeof(o3m_keys) == 64 && O3M_OFFSET(o3m_keys, shift_f) == 32); /* 1.1..1.8: 20 bytes (O3M_UI_SIZE), 1.9 appends keys */
O3M_ABI_ASSERT(page_pods, sizeof(o3m_page) == 8 && O3M_OFFSET(o3m_page, first_param) == 4 && sizeof(o3m_labels) == 8 && O3M_OFFSET(o3m_labels, count) == 4);
O3M_ABI_ASSERT(param_pods, sizeof(o3m_instrument_info) == 20 && O3M_OFFSET(o3m_instrument_info, midi_channel) == 16 && sizeof(o3m_param_desc) == 40 && O3M_OFFSET(o3m_param_desc, id) == 16 && O3M_OFFSET(o3m_param_desc, group) == 20 && O3M_OFFSET(o3m_param_desc, name) == 24);
#endif

#ifdef __cplusplus
}
#endif

#endif /* O3MODULE_H */
