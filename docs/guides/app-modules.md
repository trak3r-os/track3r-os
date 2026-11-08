# App modules: writing an app for the Tracker

Status: 2026-10-15. Sections 1 and 2 describe the app ABI up to **1.2**; section 3 describes **ABI 1.3, slice H1 of ADR 0014, ABI 1.4, slices H2a (the audio tap) and H2b (the Tuner and Scope Spectrum apps), ABI 1.5, slice H3a (steps, edit groups, F7 TOOLS) ABI 1.6, slice H3b (instrument parameters; the CHORDS, HUMANIZE, MUTATE and MORPH apps) and ABI 1.7, slice H4 (export writes; the SMF EXPORT app), which are BUILT and host-tested** (capabilities, the read, edit and io tables, the MIDI-in ring, `now_ms`, tempo, the audio tap, `get_steps` and the edit groups) except where a part is marked PLANNED (H3c routes and MOD RECIPES, OS-APPMIDI MIDI out); section 4 points to RHYTHM GEN (steps) and MUTATE (instrument parameters) as the worked examples of an editing app and keeps a PLANNED sketch for MIDI out; section 5 is design advice for both. Nothing here has run on a Tracker yet: the SDK is host-tested only. Do not say "works on the Tracker" from a host test.

Reference: [SDK guide](../../module/sdk/README.md), [ABI header](../../module/sdk/o3module.h), [ADR 0002](../adr/0002-sd-plugins.md), [ADR 0014](../adr/0014-app-host-api-v2.md). The SDK is MIT licensed: your module may have any licence.

## 1. What an app module is

An app is a `.tmod` file (kind APP) in `/modules/apps/` on the SD card. The OS lists it on the MODULES screen and gives it one page.

| An app is | An app is not |
|---|---|
| native code for the Cortex-M4F, loaded from the card | a script (tier 2 scripts were closed by the maintainer, ADR 0002 amendment 4) |
| run in the **main loop**, never in the audio interrupt | a sound source: `render`, `note_on`, `note_off` stay NULL |
| the maintainer of **one page body** (view 0) | a replacement for the page: status strip, soft-key labels, theme and exit chord stay the OS's |
| a drawer through a **budgeted draw API** | able to touch the display list, a register or a font |
| a reader of buttons, pads and the encoder | a reader of the project, MIDI or audio unless its file asks for the capability (ABI 1.3, section 3) |
| a holder of at most 32 standard parameters | a saver of anything: its parameters are not stored with the project |

Rules of the code itself:

- No writable globals, no statics, no constructors. All mutable bytes live in the **state block** the OS gives `init()` (zeroed, 16-byte aligned, in SDRAM, 1 to 262144 bytes).
- No libc, no libm, no heap, no `double`. The compiler may emit `memset`/`memcpy` for `= {}` or big copies: the packer refuses the module (undefined symbol). Fill by loops.
- The OS lends `log`, `exp2_f`, `log2_f`, `sin_f`, `cos_f` and the draw table. That is the whole host table in ABI 1.2. ABI 1.3 appends three more tables (section 3), each lent only for what the file asks for.

### Trust model

Modules are **trusted community code, not a sandbox** (ADR 0002). Anyone can drop a module on the card; there is no signing. The loader is robust against a damaged file and honest mistakes. It is **not** a defence against an attacker with a valid file: a module runs as the firmware does, on the same bus, with no memory protection for peripherals.

What the API gives an app (and so what it cannot do through the API):

- It cannot make sound or send MIDI. It cannot read MIDI, the tempo or the project, or edit it, unless its file asks for the capability (ABI 1.3, section 3); it cannot read audio at all yet.
- It cannot open, read, write, rename or delete a file, or format the card.
- It cannot raise its budgets, draw outside its canvas or swallow the exit chord F7 + F8. The chord always works, even for a hung app (it is handed the F7 press as an ordinary event, then the page closes; F8 is the page's BACK and is never handed to an app).
- If it draws over budget it is blanked and **quarantined** until reset. A module that crashes is quarantined at the next boot (crash record, boot counter).

Do not treat these as a security promise. Treat an unknown `.tmod` like an unknown firmware file.

## 2. Available today (ABI 1.2)

### 2.1 Toolchain

You need: Python 3 (tools use the standard library only), a host `g++` (host tests), `arm-none-eabi-g++` (Arm GNU Toolchain 15.3.Rel1 was used here; other versions [unverified]), and the SDK: `module/sdk/` (`o3module.h`, `o3module.ld`) plus `tools/o3m_gen.py` and `tools/o3m_pack.py`. Every command below runs from the repository root. `$SDK` stands for `module/sdk`.

### 2.2 The flow

    spec -> generate -> implement -> host test -> pack -> /modules/apps/ -> run

1. **Spec and template.** `python3 -I tools/o3m_gen.py new --kind app --id padcount --name "Pad Count" --out padcount/` writes `module.json`, `padcount.cpp` (a working template) and `padcount_gen.h`.
2. **Edit `module.json`**, then **regenerate**: `python3 -I tools/o3m_gen.py gen padcount/module.json --out padcount/padcount_gen.h`. `o3m_gen.py check module.json padcount_gen.h` finds a stale header.
3. **Implement** `padcount.cpp`.
4. **Host test** (2.6).
5. **Pack** (2.7).
6. **Install** (2.8) and **run**.

### 2.3 `module.json`

| key | meaning |
|---|---|
| `name` | display name, 1..23 printable ASCII |
| `id` | file stem and C prefix: `[a-z][a-z0-9_]*`, at most 31 characters |
| `kind` | `"app"` |
| `abi` | optional. Without it the generator writes minor 1 for an app. `"1.2"` is needed to ship a module version (2.9); `"1.5"` for an app that uses the step and edit-group entries and `"1.6"` for one that uses the instrument-parameter entries (3.3: they have no capability bit, so the manifest says it; declare the LOWEST minor your app needs) |
| `description` | one sentence, for people only |
| `state_bytes` | size of the state block, 1..262144; `static_assert(sizeof(State) <= <ID>_STATE_BYTES)` ties the source to it |
| `params[]` | at most 32: `name` (1..15), `min`, `max`, `default`, `type` (`float`, `int`, `bool`), `unit`, `curve`, `learnable`, `mod` |
| `views[]` | at most 8, an app needs at least one: `name`, `min_size`, `preferred_size` as `[w, h]` (16..800 by 16..480) |
| `voices` | must be 1 (ignored by an app) |
| `memory` | advice only |

Parameters of an app become standard OS controls on its page (name, value, bar, unit). Edit them with Shift + Up/Down and Shift + encoder (an app that declares none draws and moves its own rows: see "The grammar of an app page"). Learn, undo, Perform and saving do **not** work on them yet. Never reorder or remove parameters of a published module.

### 2.4 The code

Entry points (`o3m_ui`, the table you pass in `o3m_entry`):

| function | called when |
|---|---|
| `init(state, host)` | at launch. Return 0 or the app is refused |
| `enter(state)` | the page opens |
| `input(state, event)` | every button, pad and encoder event |
| `view_draw(state, 0, canvas)` | the page is drawn, 30 times a second |
| `exit(state)` | the page closes (not called on a quarantined app) |
| `set_param(state, id, value)` | a parameter changed. **You must clamp**: the OS does not, and NaN must fall to the lower bound |
| `shutdown(state)` | after `exit` |

Input is an `o3m_ui_event {kind, id, value, mods}`:

| kind | id | value |
|---|---|---|
| `O3M_EV_BUTTON` | `O3M_BTN_*` (F1..F8 = 0..7, Play 23, Copy 24, Insert 25, Up 26, Delete 27, Rec 28, Shift 29, Left 30, Down 31, Right 32) | 1 down, 0 up |
| `O3M_EV_PAD` | 0..47, pad 0 at top left | velocity 1..127, 0 on release |
| `O3M_EV_ENCODER` | 0 | signed detents |

`mods & O3M_MOD_SHIFT` is set while Shift is held. A release reaches you only if you got the press. The OS keeps the Shift layer of the page's own controls (Shift + Up/Down, Shift + encoder, Shift + Delete) **only while your app declares at least one `o3m_param` and was built below ABI 1.10** (the OS parameter column): then your `input()` does not see those. An app that declares none, or was built at 1.10 (its rows are storage: `set_own_param`), gets all of them, with `mods` set (OS-APPUX S2, S3). The eleven screen buttons (Pattern, Song, Params, Sample play/edit/load/rec, Master, Performance, File, Config) never reach `input()`: on an app page they leave the app and open their screen (OS-APPUX S3). **Shift + Config** is the OS's too: it opens the OS's help page for your app's page, and while that page is up you get no key, pad or encoder turn at all (the release of something you were given the press of still arrives). No ABI change: an app of any version has this rule, as it has the F8 and Play rule below. **The exit chord F7 + F8 is the OS's but you see half of it**: the F7 press arrives as an ordinary button event, the page closes in the same pass, and `exit()` runs; you never get its release. F8 never arrives at all (since ABI 1.9 it is BACK on every app page, a 1.1-1.8 app included, and Play is the transport's). You cannot cancel the chord, and you should not act on that F7 press as if the page stayed open. The chord uses the OS's global 40 ms chord window (F7 and F8 within 40 ms of each other; a hand test on 0.4.16 exits every time); if a missed exit is ever reported, the fallback is a 120 ms window for the exit chord only (ADR 0014, exit chord).

Input has no access to `canvas->params`. Cache a parameter in `set_param()`, and do not assume it ran before the first input: set a default in `init()`.

### 2.5 Drawing and its budgets

`view_draw` paints into `canvas` (width x height, top-left (0, 0)). Everything outside is cut off. Colours are the theme's 16 roles (`O3M_COLOR_PANE`, `_TEXT`, `_ACCENT`, `_INSTR`, `_SEL`, ...). You never see an RGB value. Draw from `canvas->params` and your state, nothing else.

| call | what | display-list words |
|---|---|---|
| `rect(c, x, y, w, h, color)` | filled rectangle | 4 |
| `outline(c, x, y, w, h, color)` | 1 px border | **12** |
| `text(c, x, y, s, face, color)` | up to 64 characters, fixed pitch (`O3M_FACE_DISPLAY/DATA/LABEL/MICRO`) | characters + 2 |
| `text_width(c, s, face)` | pixels; not a draw call | 0 |
| `polyline(c, xy, n, color)` | 2..128 int16 pairs, 1 px | n + 10 |
| `curve(c, &curve)` | `ui::plot` curve; fill with `o3m_curve_set` | 2n + 16, **+200 with axes** |

Budgets, enforced by the OS and not negotiable:

| budget | limit |
|---|---|
| draw calls per view and frame | 96 |
| points in polylines and curves per frame | 256 (one polyline at most 128) |
| characters of text per frame | 384 |
| time in `view_draw` (also `input`, `enter`, `set_param`) | 4 ms |
| display-list words, all views of a page together, **page's own words included** | 700 |

48 outlines alone would cost 576 of the 700. A call with a bad argument (NULL string, colour out of range, zero size) is ignored and still counts as a call. Calls are recorded and replayed only if the frame stayed inside budget. An app over budget is **blanked and quarantined**. Aim at 450 words or less. Use `rect` instead of `outline` where you can. The words are an estimate; the 4 ms is [unverified] on the MK66 (not yet measured on the unit).

### 2.6 Worked example: Pad Count

A grid of 48 cells (12 x 4) lights as pads are hit. Delete clears. The encoder moves a value by `Step`, a parameter. `module.json`:

    {"name": "Pad Count", "id": "padcount", "kind": "app", "abi": "1.2",
     "description": "Counts pad hits on a 12 x 4 grid and keeps a value the encoder moves in steps.",
     "state_bytes": 128,
     "params": [{"name": "Step", "min": 1, "max": 16, "default": 1, "type": "int"}],
     "views": [{"name": "Body", "min_size": [240, 160], "preferred_size": [500, 384]}],
     "voices": 1, "memory": {"hot_bytes": 0, "sdram_bytes": 0}}

`padcount.cpp` (compiled for the host and for the Cortex-M4F, see below):

```cpp
// SPDX-License-Identifier: MIT
// Pad Count: an app that counts pad hits (12 x 4 grid, pad 0 top left) and keeps a value the encoder moves by the STEP parameter. Delete clears it.
#include <o3module.h>

#include "padcount_gen.h"

namespace {

struct State {
  uint8_t hits[48];  // hits per pad, saturating at 255
  uint32_t total;
  int32_t value;     // the encoder's sum, -9999..9999
  int32_t step;      // the STEP parameter as set_param() last heard it
  uint32_t last_vel; // velocity of the last pad hit
};
static_assert(sizeof(State) <= PADCOUNT_STATE_BYTES, "module.json: state_bytes is too small for State");

void clear(State* s) {
  for (uint32_t i = 0; i < 48; ++i) {
    s->hits[i] = 0;
  }
  s->total = 0;
  s->last_vel = 0;
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->step = 1;  // do not rely on set_param() having run before the first input
  return host->draw != nullptr ? 0 : 1;
}

void set_param(void* raw, uint32_t id, float v) {
  if (id == PADCOUNT_PARAM_STEP) {
    const float x = v >= 1.0f ? (v <= 16.0f ? v : 16.0f) : 1.0f;  // clamp so that a NaN falls to the lower bound
    static_cast<State*>(raw)->step = static_cast<int32_t>(x + 0.5f);
  }
}

void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  clear(s);
  s->value = 0;
}

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  if (ev->kind == O3M_EV_PAD && ev->id < 48u && ev->value > 0) {
    if (s->hits[ev->id] < 255u) {
      ++s->hits[ev->id];
    }
    ++s->total;
    s->last_vel = static_cast<uint32_t>(ev->value);
  } else if (ev->kind == O3M_EV_BUTTON && ev->value != 0 && ev->id == O3M_BTN_DELETE) {
    clear(s);
  } else if (ev->kind == O3M_EV_ENCODER) {
    const int32_t v = s->value + ev->value * s->step;
    s->value = v < -9999 ? -9999 : (v > 9999 ? 9999 : v);
  }
}

// "<label><number>" with no libc; returns the new length.
uint32_t put(char* out, uint32_t n, const char* label, int32_t value) {
  while (*label != '\0') {
    out[n++] = *label++;
  }
  uint32_t u = value < 0 ? 0u - static_cast<uint32_t>(value) : static_cast<uint32_t>(value);
  if (value < 0) {
    out[n++] = '-';
  }
  char d[10];
  uint32_t k = 0;
  do {
    d[k++] = static_cast<char>('0' + u % 10u);
    u /= 10u;
  } while (u != 0u && k < 10u);
  while (k != 0u) {
    out[n++] = d[--k];
  }
  out[n] = '\0';
  return n;
}

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  const State* s = static_cast<const State*>(raw);
  const o3m_draw_api* api = c->draw;
  const int32_t cw = c->width / 12;
  const int32_t ch = (c->height - 48) / 4;
  api->rect(c, 0, 0, c->width, c->height, O3M_COLOR_PANE);                    // 1 call, 4 words
  for (int32_t pad = 0; pad < 48; ++pad) {                                    // 48 calls, 192 words
    const uint32_t h = s->hits[pad];
    const uint32_t colour = h == 0u ? O3M_COLOR_EMPTY : (h < 4u ? O3M_COLOR_INSTR : O3M_COLOR_ACCENT);
    api->rect(c, (pad % 12) * cw + 2, (pad / 12) * ch + 2, cw - 4, ch - 4, colour);
  }
  char line[40];
  put(line, 0, "HITS ", static_cast<int32_t>(s->total));
  api->text(c, 6, c->height - 40, line, O3M_FACE_DATA, O3M_COLOR_TEXT);       // characters + 2 words
  put(line, 0, "VALUE ", s->value);
  api->text(c, c->width / 2, c->height - 40, line, O3M_FACE_DATA, O3M_COLOR_TEXT);
  put(line, 0, "VEL ", static_cast<int32_t>(s->last_vel));
  api->text(c, 6, c->height - 20, line, O3M_FACE_DATA, O3M_COLOR_TEXT_DIM);
}

void leave(void*) {}
void shutdown(void*) {}

const o3m_ui kUi = {O3M_UI_SIZE(PADCOUNT_ABI_MINOR), enter, input, view_draw, leave};  // (the table's size follows the minor the manifest declares: 20 bytes at 1.2, 24 from 1.9)

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    PADCOUNT_ENTRY_HEAD, sizeof(State), PADCOUNT_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, PADCOUNT_ENTRY_VIEWS(&kUi),
};
```

**Host test.** The same source builds as a shared object that a test loads with `dlopen` and drives through the ABI, with a draw table that counts what the OS counts:

    g++ -std=c++17 -Wall -Wextra -shared -fPIC -I $SDK -I padcount padcount/padcount.cpp -o padcount.so
    g++ -std=c++17 -Wall -Wextra -I $SDK padcount/padcount_test.cpp -o padcount_test -ldl
    ./padcount_test ./padcount.so

The test (about 80 lines, plain C++, no framework) loads `o3m_entry` with `dlsym`, checks kind, ABI and counts, calls `init`, `enter` and `set_param`, sends events, calls `view_draw` through a draw table that counts calls, points, characters and words as the OS does, and checks the texts and the budgets. It is in the scratch directory of this guide's author, not in the repository; the commands above are all you need to write your own.

Result on this example (g++, 2026-10-14): `worst frame: calls 52/96 points 0/256 chars 25/384 words 227` and `OK`, exit code 0. The tests cover saturation at 255 hits, an out-of-range pad id, a release of Delete, NaN in `set_param`, and the worst frame (all pads lit, long numbers).

Inside the repository you can use the CMake helpers instead (`o3m_add_module`, `o3m_manifest`, `o3m_add_test`; SDK guide section 1b). They give you a doctest executable and the OS's own draw recorder (`module::ViewHost`), which reports the budgets the OS will apply. This example was **not** run through them [unverified].

### 2.7 Packing

    python3 -I tools/o3m_pack.py build --cxx arm-none-eabi-g++ --src padcount/padcount.cpp -I padcount --out padcount.tmod --version 1.0.0
    python3 -I tools/o3m_gen.py verify padcount/module.json padcount.tmod

The packer compiles with the ABI's flags (Cortex-M4F, hard float, `-mlong-calls`, no PIC, no libc), links with `o3module.ld`, and refuses a writable global, a constructor, an undefined symbol or any relocation but a 32-bit absolute address. `verify` reads the file back and lists every difference from the manifest. `--opt` sets the `-O` level (default `-Os`).

Result for Pad Count: `Pad Count, 828 B file: hot 0 B, cold 704 B, 15 relocations, state 64 B, 1 parameters, 1.0.0`, exit code 0; `verify` exit code 0. This used the Arm GNU Toolchain 15.3.Rel1 from the repository's `.toolchains/`. An app has no hot code, so `hot` is 0.

### 2.8 Install and run

1. Copy `padcount.tmod` to `/modules/apps/padcount.tmod` on the card. The file name without `.tmod` is the module's name: at most 31 characters, none of `/ \ : * ? " < > |`.
2. Put the card in the Tracker. Press Shift + Pattern for the list of module apps and press **F1** on yours (or press Song + Pattern together for the Navigator, pick MODULES, move to the app, press **F1**). Exit with **F8** (BACK; **F7 + F8** also works, and is the way out of a hung app).
3. `diag mod` on the USB console lists the loaded modules, the memory in use and the quarantine. Its `mod load <name>` searches `instruments/`, `effects/` and then `apps/` (the loader's order), so it loads and checks an app file too; it never launches it (`mod bind` refuses an app) and **MODULES, F1 is the only way to launch**. Whether `diag mod` also lists a launched app is [unverified]; the MODULES screen shows its state.

### 2.9 Versioning and the ABI rules

- **ABI version** (1.2 now) says what OS and module must agree on. A module loads when its **major equals the OS's and its minor is not above the OS's**. Additions (a flag, a trailing member of `o3m_host`, a new kind, a function at the end of the draw API) bump the minor; anything else bumps the major.
- **Module version** (`X.Y.Z`, `--version`, needs `"abi": "1.2"`) is yours. The OS reads it for one decision: a card copy replaces a built-in module of the same name only if it is strictly newer. No built-in app exists today, so for an app it is informational. Raise it on every release.
- Before using a member a later version added, check `o3m_host.size` and `o3m_draw_api.size`/`version`.
- Parameter order is frozen once published. Add at the end, or ship a new name.
- Your 1.0, 1.1 and 1.2 file keeps loading and running under 1.3 (built and tested in H1): its capability word is 0, an app that declares less than ABI 1.3 is lent no new table, and `init()` has the signature it always had. The Pads example is packed as ABI 1.2 for exactly this test.
- ABI minors are claimed in **merge order**: 1.3 is slice H1's; whichever slice merges next takes 1.4 (ADR 0014 amendment D).
- Hardware checks still open (SDK guide section 10): speed of code from SDRAM, `view_draw` cost, fault handlers and the watchdog.

## 3. Host API v2 (ABI 1.3 slice H1, 1.4 slice H2a, 1.5 slice H3a, 1.6 slice H3b and 1.7 slice H4 built; the rest PLANNED)

Until 1.3 an app could not read the tempo, MIDI or audio, nor edit anything; a tap-tempo app could only count UI frames (about 33 ms). ABI 1.3 (ADR 0014, slice H1, amendment D) adds what H1 promised: a capability word in the file, the **read**, **edit** and **io** tables, the MIDI-in ring, a clock, the tempo and the launch context. It is additive: every 1.2 module keeps working.

> **Built (H1, H2a, H2b, H3a and H3b, host-tested; nothing here has run on a Tracker yet):** sections 3.1, 3.2, 3.3 (steps, instrument parameters and edit groups; the routes are H3c), 3.4 (the tap and the three apps that use it: Loudness VU, Tuner, Scope Spectrum), 3.5, 3.6 (the call and Pattern F7 TOOLS) and 3.8. **PLANNED:** the route entries of 3.3 (H3c, after OS-MODMX), 3.7 (H4), 3.9 (OS-APPMIDI). The authority for names and types is `module/sdk/o3module.h`.

Slices: **H1** capability word, read table, launch context, `now_ms`, `get_tempo`, `set_tempo`, MIDI-in ring (built). **H2a** audio tap and the LOUDNESS VU app (ABI 1.4, built); **H2b** TUNER and SCOPE SPECTRUM (built, ABI 1.4). **H3a** (ABI 1.5, built): steps, the edit groups, F7 TOOLS and the RHYTHM GEN app; **H3b** (ABI 1.6, built): instrument parameters and the CHORDS, HUMANIZE, MUTATE and MORPH apps. **H3c** routes and MOD RECIPES (after OS-MODMX). **H4** (ABI 1.7, built): export writes (`export_files`) and the SMF EXPORT app. **C1/C2** MIDI out and SysEx (OS-APPMIDI, end of the queue).

### 3.1 Capability bits and first-launch confirm (H1)

A bitmask in the file header word `reserved2[0]` (byte 40) tells the OS what the app wants. A file of ABI 1.2 and below has 0, and so does an ABI 1.3 app that wants nothing. Set it with `module.json` `"capabilities": ["midi_in", "read_project", "edit_project"]` (the generator then writes ABI minor 3; with `audio_tap` it writes **4**; add `"abi": "1.5"` for the 1.5 entries) and pack with `o3m_pack.py --caps midi_in,read_project,edit_project` (CMake: `o3m_add_module(... CAPS midi_in,read_project ...)`); `o3m_gen.py verify` compares the file with the manifest. Only an **app of ABI 1.3** may ask (and `audio_tap` only an app of **ABI 1.4**: in a 1.3 file that bit is a reserved field). A bit above the six below, or any bit in a file of ABI 1.2 or lower or in an instrument or effect, is refused as a reserved field and the module is quarantined.

**The six values are frozen as published ABI** (ADR 0014 amendment D, C4): midi_in 1, read_project 2, audio_tap 4, edit_project 8, export_files 16, midi_out 32. They are never renumbered or reused. An OS of ABI 1.3 grants 1, 2 and 8; **an OS of ABI 1.4 also grants 4** (audio_tap); **an OS of ABI 1.7 also grants 16** (export_files, and only to a file of ABI 1.7: in an older file that bit is a reserved field). A file that asks for 32, or for 16 on an OS older than 1.7 (a later OS's capability), is **refused at load**, before it runs, with a message that names the capability on the MODULES screen (`NEEDS MIDI OUT (NEWER OS)`, and on an OS older than 1.7 `NEEDS EXPORT FILES (NEWER OS)`; `NEEDS AUDIO TAP (NEWER OS)` is also what a build with no tap says, and what a 1.3 OS says of a 1.4 file, whose minor it refuses first). It is not quarantined (the file is fine, the OS is old) and it never runs with half its tables. The packer and the generator only accept the names of the bits the current ABI grants until the OS that grants the others exists.

| capability | macro (value) | unlocks | slice | confirm on first launch |
|---|---|---|---|---|
| midi_in | `O3M_CAP_MIDI_IN` (1) | `io.midi_read`, `io.midi_lost` | H1, built | no |
| read_project | `O3M_CAP_READ_PROJECT` (2) | the read table | H1, built | no |
| audio_tap | `O3M_CAP_AUDIO_TAP` (4) | `io.tap_select`, `io.tap_latest`, `io.tap_stream`, `io.tap_levels` (3.4) | H2a, built (ABI 1.4) | no (it can only listen) |
| edit_project | `O3M_CAP_EDIT_PROJECT` (8) | the edit table | H1 (`set_tempo`), H3a (steps and groups, ABI 1.5) | yes: see below |
| export_files | `O3M_CAP_EXPORT_FILES` (16) | `io.export_open`, `io.export_write`, `io.export_close`, `io.export_status` (3.7) | H4, built (ABI 1.7) | yes: "WRITES FILES TO APPEXPORT/<APP>: F1 AGAIN" |
| midi_out | `O3M_CAP_MIDI_OUT` (32) | `midi_send`, SysEx-in | C1, planned | "This tool sends MIDI, including SysEx, to your devices." |

**A table is NULL without its bit** (the rule, C1): every table and every entry of the io / read / edit tables that needs a capability (today `midi_read` and `midi_lost`, the four `tap_*` of 1.4 and the four `export_*` of 1.7 with export_files; later `midi_send` with midi_out) is NULL in the table the OS lends unless your file asked for that bit and the OS grants it. Test for NULL before every call.

Which tables you are lent: `host->read` and `host->edit` are NULL unless the file asked; `host->io` is lent to every app that **declares ABI 1.3** (it holds the clock, which needs no capability) and its `midi_read` / `midi_lost` are NULL without midi_in (or in a build with no MIDI ring), its four `tap_*` members (ABI 1.4) are NULL without audio_tap, and its four `export_*` members (ABI 1.7) are NULL without export_files (or in an app that declares less than 1.7). A 1.3 app is lent the same, larger io table with the tap members NULL: the members of 1.3 sit where they did. An app that declares ABI 1.2 is lent none of the three, and its host table is the 1.2 one: `host->size` ends at `draw` and `abi_minor` reads 2, so a 1.2 module that checks `size` finds no 1.3 member. Check `O3M_HOST_HAS(host, io)` (the host's `size`), then the table, then the member.

**The confirm.** The first time per boot that an app holding edit_project is launched, MODULES answers "EDITS THE PROJECT (UNDOABLE): F1 AGAIN" and does not start it; launching the same app straight after starts it, and it is not asked again until the next boot. The answer is **not stored** (a stored record would be a settings-format decision) and is a **ring of four app names**: confirming a fifth editing app in one boot makes the OS forget the oldest, which asks again. The app is told its launch went through before its edit applies: `set_tempo` returns `O3M_EDIT_OK` for "queued", not "applied". Read-only capabilities are not confirmed. **An app that holds export_files (ABI 1.7) is confirmed the same way** with its own line, "WRITES FILES TO APPEXPORT/<APP>: F1 AGAIN" (the folder it writes to, in capitals); an app that holds both bits gets ONE line that says both ("EDITS (UNDOABLE) + WRITES FILES: F1 AGAIN"). The answer is remembered **with the bits it covered**: replacing the app's file by a version that asks for more (an edit app that now also writes files) asks again.

### 3.2 Read table (H1 and H3a, built)

`host->read` (needs read_project), copy-out: the model never enters the ABI. `o3m_read_api` has `get_tempo` and `launch_ctx` (H1) and, from **ABI 1.5**, `pattern_info` and `get_steps`, and from **ABI 1.6** `instrument_info`, `get_param_desc` and `get_instrument_param`; the transport and routes the ADR names are added at its end by later slices (check the one member you call: `O3M_TABLE_HAS(host->read, get_steps)`, never `sizeof` of the whole table). Enough for 60 Hz polling.

- `pattern_info(pattern, &info)` fills `{size, length, tracks, track_length[16]}`: the pattern's length in rows (the longest track), the project's active tracks and the rows each track plays. Set `info.size = sizeof info` first. A pattern that does not exist answers `O3M_EDIT_RANGE`; the Pattern shows such a pattern empty and creates it on the first hand edit, but an app cannot create one (H3c may add that).
- `get_steps(pattern, track, first_row, out, count)` copies `count` (1..128) steps into `out` as `o3m_step`. The rows must lie inside the track's own length (`track_length[track]`), the rule the pattern editor keeps for its own edits. What you read is the project as it is: the writes of an edit group that is still open apply when it closes (`get_steps` shows the project before the group; `get_tempo` is the exception and returns the tempo the open group set).
- **Instrument parameters (ABI 1.6).** `instrument` is the number the Pattern shows, 1..64. `instrument_info(n, &info)` fills `{size, kind, engine, param_count}` (and, from ABI 1.7, `midi_channel`: 1..16 for a MIDI instrument, 0 for any other kind): `kind` is `O3M_INSTRUMENT_EMPTY`, `SAMPLE`, `MIDI` or `MODULE` (an empty slot answers OK with kind EMPTY); `engine` is a sample's play engine (0 auto, 1 modern, 2 Amiga, 3 12-bit, 4 8-bit) or, for a module, a 32-bit hash of its name that is the same for every instrument of that module; two instruments **match** when kind and engine are equal (this is how MORPH offers only compatible sources). `param_count` is 83 for a sample, the module's own number for a module (0 while the module is not loaded), 0 for the others. `get_param_desc(n, index, &d)` fills `{size, min, max, def, id, flags, group, name[16]}` for index 0 .. `param_count` - 1, and `get_instrument_param(n, id, &v)` reads the value in the descriptor's unit (a module parameter the project has not written yet reads as the module's default). Set `size` first, as for `pattern_info`; a struct shorter than ours gets as much as fits. `id` is the project's own field id and is never renumbered: a sample's are 0..13 (VOLUME, PANNING, TUNE, FINE TUNE, GLIDE, FILTER, FILTER TYPE, CUTOFF, RESONANCE, DELAY SEND, REVERB SEND, OVERDRIVE, BIT DEPTH, PLAY ENGINE), 0x14..0x1C the source page, `0x20 + 8 * envelope + field` the six envelopes (ENABLE, AMOUNT, ATTACK, DECAY, SUSTAIN, RELEASE) and `0x60 + 4 * slot + field` each modulation slot (SOURCE, LFO SHAPE, LFO SPEED, LFO AMOUNT); a module's are `0x100 + i`. `group` is `O3M_PGROUP_VOICE`, `FILTER`, `ENV`, `SENDS`, `SOURCE` or `MOD` for a sample and the page of eight (`i / 8`) for a module. `flags`: `O3M_PD_ENUM` (the value picks a name), `O3M_PD_STEPPED` (a whole number with few values: a tune in semitones, a bit depth, a small module INT; **switch** between two such values, do not blend them) and `O3M_PD_APP_WRITABLE`. Like `get_steps`, the reads show the project as it is: a write of an open group is not seen until it closes.
- `o3m_step` is 8 bytes in the model's encodings: `note` 0 empty, 1..128 (the model's note: MIDI note + 1; the Pattern's tile names 1 as C-0, so 49 is C-4 and 61 is C-5: `ui/src/assets.cpp:66-73`; the MIDDLE C display setting, `model/src/params.cpp:192`, only renames notes on screen and the byte never changes. RHYTHM GEN's own note names ignore the MIDDLE C setting: it always shows 49 as C-4), 253 fade, 254 cut, 255 off; `instr` 0 none, 1..64 (the instrument as the Pattern shows it, slot + 1); `vol` 0..127 or 255 unset; FX `type` 0 none, 1..42 the Tracker's, 128..255 an engine's own (43..127 are reserved) and `value`; `reserved` 0. `o3module.h` names every value (`O3M_NOTE_OFF`, `O3M_VOL_UNSET`, ...).

### 3.3 Typed edit table (set_tempo in H1; steps and groups in H3a, ABI 1.5; instrument parameters in H3b, ABI 1.6; routes H3c, PLANNED)

`host->edit` (needs edit_project) has `set_tempo(bpm_x100)` (3.8) and, from ABI 1.5, `edit_begin`, `edit_end`, `set_step` and `set_steps`, and from ABI 1.6 `set_instrument_param`. Generators think in steps and parameters, not cursor moves. The rules (all tested: `tests/module_edit_test.cpp`):

- **A group is the unit.** `edit_begin` ... `edit_end` is **one undo group per apply**. The writes are **checked** when you make them and **applied together** when you call `edit_end`, as one undo unit named after your app: the user's UNDO takes the whole group and the notice reads `UNDO <APP NAME>`; REDO gives it back. The project's unsaved mark, the sequencer and the undo history follow exactly as for a hand edit; edits while the song plays are allowed. Your reads see the project as it was until `edit_end`. An empty group, or one that writes what is already there, leaves no undo unit.
- **There are no implicit units.** A write outside a group answers `O3M_EDIT_NO_GROUP`; a second `edit_begin` while one is open, `O3M_EDIT_GROUP_OPEN`.
- **A group is made from `input()`, one per call.** A second `edit_begin` in the same call answers `O3M_EDIT_RATE`, and so does one from `view_draw`, `enter`, `exit` or `set_param`. The group never spans two calls: if you return with one still open the OS closes it, **keeps and applies** the changes (so the user can take them back with UNDO), counts it and logs the first one. The same happens when the OS stops your app for going over its time budget (the group is applied first, then the app is quarantined) and when you leave the page. UNDO is never blocked by an open group.
- **Limits per group:** at most **4,096 write calls** (refused ones count) and **2,048 steps** (16 KB of `o3m_step`, the journal's cap). The write that crosses either limit, and every write after it, answers `O3M_EDIT_LIMIT` and stores nothing; `edit_end` then answers `O3M_EDIT_LIMIT` and **drops the whole group**: the project is exactly as it was before `edit_begin`, there is no undo unit and the redo history is untouched. Split a big job in several groups, one per call (several key presses).
- **Bad arguments** (a pattern that does not exist or is not 1..255, a track at or above the active tracks, a row or count outside the track's rows, a count of 0, a NULL pointer, a field mask of 0 or with an unknown bit, an invalid value: a note 129..252, an instrument above 64, a volume 128..254, an FX type 43..127) answer `O3M_EDIT_RANGE` and store nothing; the group stays valid. Only the masked fields are checked.
- **The field mask** (`O3M_STEP_NOTE` 1, `INSTR` 2, `VOL` 4, `FX1` 8, `FX2` 16) says which fields of the step you pass are written; the others keep what the project has. A generator that writes notes passes `NOTE | INSTR | VOL` and the FX stay. `set_step` writes one step, `set_steps` `count` consecutive steps of a track (one call, `count` steps against the limit).
- **`set_instrument_param(instrument, id, value)` (ABI 1.6) joins the open group.** It runs through the same path as the Params page (the engine and a module's running instance hear it as they hear a hand edit), is checked when called, applied at `edit_end` as part of the group's one undo unit, and UNDO restores every float **bit for bit**. It answers `O3M_EDIT_NO_GROUP` outside a group and `O3M_EDIT_LIMIT` past a limit, and `O3M_EDIT_RANGE` (nothing written, the group stays valid) for an instrument that is empty, MIDI, or a module that is not loaded, an unknown `id`, **a parameter that is not `O3M_PD_APP_WRITABLE`**, NaN or infinity, and a value outside `min`..`max`: **nothing is clamped for you**. A field that holds whole numbers stores the value rounded. A call counts as one write call and takes one of the 2,048 buffer slots of the group, and a group accepts at most **`O3M_EDIT_MAX_PARAM_WRITES` = 64** parameter writes (the rings that carry a change to the engine, 256, and to a module instance, 64, would otherwise drop one silently): the 65th answers `O3M_EDIT_LIMIT` and the group is dropped. A whole sample instrument is 61 writes. Repeating a parameter in a group is allowed: the last value wins.
- **The APP_WRITABLE rule (conservative).** A **sample's** writable parameters are the sound ones: VOLUME, PANNING, TUNE, FINE TUNE, GLIDE, FILTER, FILTER TYPE, CUTOFF, RESONANCE, DELAY SEND, REVERB SEND, OVERDRIVE, BIT DEPTH (13), each envelope's AMOUNT, ATTACK, DECAY, SUSTAIN and RELEASE (30) and each modulation slot's LFO SHAPE, LFO SPEED and LFO AMOUNT (18): 61 of the 83. **Never writable (22):** PLAY ENGINE, the whole source page (play mode, wavetable position and size, grain position, length, shape and loop, the slice count and the selected slice: they go through validating actions or rebuild the voice), each envelope's ENABLE (6) and each slot's SOURCE (6). The sample itself, the slices, the module binding and a MIDI instrument's fields are not reachable at all. **Every parameter of a module instrument is writable**: the module declared it and the host holds the value to its range. The set is one function, `model::instrument_field_app_writable`, and the descriptors say it (`O3M_PD_APP_WRITABLE`), so an app should read the flag rather than copy this list; a later minor may widen it.
- **Declare the lowest ABI you need.** An app that only reads and writes steps declares `"abi": "1.5"` (CHORDS and HUMANIZE do) and runs on an OS of 1.5; one that uses an instrument-parameter entry declares `"1.6"` (MUTATE, MORPH). The OS lends every app the same table (the newest it has), so test the member you call, not the whole table: `O3M_TABLE_HAS(host->edit, set_steps)`. An app may use a newer entry **if present** without declaring it (CHORDS asks `instrument_info`, when the table has it, only to choose its default MODE; on an OS of 1.5 it chooses the mode that sounds everywhere).
- **`set_tempo` joins an open group** (ADR 0014 amendment D.6): inside a group it is part of the group's unit and applied with it at `edit_end` (a later `get_tempo` in the group returns it); outside a group it stays queued as in 1.3 and is its own unit.
- Return codes (also those of `pattern_info` and `get_steps`): `O3M_EDIT_OK` 0, `RANGE` 1, `NO_PROJECT` 2, `NO_GROUP` 3, `GROUP_OPEN` 4, `LIMIT` 5, `RATE` 6. The values are published ABI.
- **ABI.** Every new entry is appended after the old ones, in the order above: `set_tempo` and the 1.3 read and io entries are where they were, and the table's `size` says which exist (`O3M_TABLE_HAS(e, set_steps)`). An app of 1.7 on an OS of 1.6 is refused at load (NEEDS NEWER OS); an app of 1.3 .. 1.6 on an OS of 1.7 keeps working: `tests/module_edit_test.cpp` pins the byte offset of every member up to 1.6 in a literal copy of that layout, so a later change cannot shift them. `o3module.h` ends with the byte offset of every member as compile-time assertions for the 32-bit target.
- There is **no generic `run_action(id)`**. File, save, format, update and WAV-save are absent by construction. The project is not saved by the app: the user saves it (File SAVE).

Storing gear patches inside the project would be a persistent-format change (Level C), a separate decision. **H3c (PLANNED):** the route entries (after OS-MODMX; there is no route in the model yet), creating a pattern, and MOD RECIPES.

### 3.4 Audio tap and peak/RMS (H2a, ABI 1.4, built)

`host->io->tap_select`, `tap_latest`, `tap_stream` and `tap_levels` (all need audio_tap and an app that declares ABI 1.4; all NULL otherwise). **One tap, shared**: the mono mix ((L + R + 1) >> 1, int16, 44.1 kHz) of the **INPUT** bus (after the input strip: raw while the strip is closed) or of the **MASTER** output (what is heard, the USB return included). The render writes it a block of 64 frames at a time into a ring of **64 blocks (93 ms, 8 KiB of SDRAM)** and never waits for you; the main loop reads it. The tap is **off** until your app calls `tap_select` and is switched off again when the app stops. Everything below is what `o3module.h` says; the file is the authority.

| call | what it does |
|---|---|
| `int tap_select(uint32_t source)` | `O3M_TAP_OFF` 0, `O3M_TAP_INPUT` 1 or `O3M_TAP_MASTER` 2. Returns 0, or 1 for any other value (nothing changes). A select starts your cursors "now", and everything the render wrote in the next two blocks (it may still be the old point's) is withheld. |
| `uint32_t tap_latest(int16_t* out, uint32_t frames)` | the newest `frames` (even, 2..`O3M_TAP_LATEST_MAX` = 2048; `out` 4-byte aligned: `alignas(4)`) oldest first. A window for a scope or a spectrum. Returns `frames`; right after a select the front is zero and the return is the number of real frames at the end. 0 for a bad argument, tap off, nothing yet. **The first call of `tap_latest` or `tap_stream` starts the sample feed** (the render stores no samples for an app that reads only `tap_levels`), so it returns 0 and the real frames begin two blocks (2.9 ms) after it. |
| `uint32_t tap_stream(int16_t* out, uint32_t max_frames, uint32_t* lost)` | everything NEW since your last `tap_stream` call (or the first call of `tap_stream` / `tap_latest`, which starts the sample feed two blocks on and returns 0), oldest first, up to `max_frames` (rounded down to even, at most `O3M_TAP_STREAM_MAX` = 4032). If more is waiting, the **oldest are dropped** and `*lost` says how many (a gap in the signal there). Called once a frame with room for 2048 frames, it never loses any: a frame is 16..33 ms, the ring 93 ms. This is the call for a **gapless** signal. |
| `void tap_levels(o3m_tap_levels* out)` | peak (linear, 1.0 = full scale), RMS (linear), the frames measured, the frames lost, and `clip_blocks` (blocks with a sample at \|x\| >= 32767) for everything since your last call. Set `out->size`. The render measures every block whether or not you read the samples, so a meter on a 33 ms frame never misses a peak between two frames (it keeps 63 blocks of statistics; a longer gap is reported in `lost_frames`). |

How the views the ADR names are built: **TUNER** (about 2048 frames at 11 kHz) reads `tap_stream` every frame and decimates by 4 in its own state (low-pass first: the OS gives you the raw 44.1 kHz signal, the decimation is yours); **SCOPE** and **SPECTRUM** (512 to 1024 frames) read a window with `tap_latest` and window / FFT it themselves; **LOUDNESS VU** needs only `tap_levels` (the `loudness` app is the worked example: peak with fall and hold, 300 ms RMS, clip lamp, history). All of this runs in your `view_draw`, never in the render: your code is never called from the audio interrupt.

**The two H2b apps** (`modules/apps/tuner/`, `modules/apps/scope/`; new Community OS behaviour, host-tested, nothing measured on the unit yet) are the worked examples of the heavy readers. Both share `modules/apps/common/` (an FFT, text helpers: headers, `INCLUDE apps/common` in CMake), are written from scratch, single precision, no libc, and keep their work and tables in the state block (Tuner 38.6 KB, Scope Spectrum 17.1 KB; the `.tmod` files are 5.7 KB and 4.8 KB).

- **Tuner** (`tap_stream`, parameters A4 415..466 Hz and SOURCE, default INPUT). A 33-tap low-pass at 4.2 kHz and a decimation by 4 (11,025 Hz) on the gapless stream; a window of 1,536 decimated frames (139 ms, in a ring of 2,048) with the mean removed and a Hann window; the autocorrelation through two 2,048-point FFTs, divided by the window's own autocorrelation (Boersma) so a periodic signal peaks at 1.0; the pitch is the first peak that reaches 0.9 of the highest and at least 0.6, refined on the peaks at 2, 4, 8 ... periods; a median of the last 5 readings (3 valid needed) and an 8-cent hysteresis on the note name. Range 30 Hz to 2.1 kHz (27 and 29 Hz are refused: NO SIGNAL); NO SIGNAL under -50 dBFS, with no clear pitch, or with the strongest spectral line above 3.6 kHz. A `lost` gap, a source change, or 500 ms without samples restarts the window and the readings. Host test: sines at 55, 110, 440, 1000 and 1760 Hz (and 30.9 Hz to 2 kHz) within 2 cents (measured on the prototype: under 0.1 cent), saw / square / two-harmonic / full-scale / clipped / DC-offset notes read their fundamental, A4 at 415, 432 and 466.
- **Scope Spectrum** (`tap_latest`, parameters VIEW 1 SCOPE / 2 SPECTRUM, SOURCE default MASTER, TIMEBASE 1..5). SCOPE: the newest 2,048 frames, a rising zero-crossing trigger with hysteresis (below -1/8 of the window's peak first) and a free-run fallback, spans of 128 to 1,536 frames (2.9 to 34.8 ms), peak-decimated to at most 248 points in two polylines. SPECTRUM: the newest 1,024 frames, mean removed, Hann, a 1,024-point FFT, 48 bars of 1/5.35 octave from 40 Hz to 20 kHz (below about 200 Hz a bar is narrower than a bin of 43 Hz and shares the lobe), dBFS with 60 dB of range (a full-scale sine on a bin is 0 dB), bars falling 60 dB/s, a peak-hold line falling 12 dB/s, and the loudest bin as text.
- **The pattern for a heavy reader** (the 4 ms limit of `view_draw` is the constraint): an app's state is in SDRAM, uncached (about 28 cycles a load), so split the work into chunks of a few hundred array elements or butterflies (`o3fft::step` takes a budget), copy what a hot loop reads (a filter's taps and input) to the stack, bound the chunks a frame by a count AND by the host clock (`io->now_ms()`: `kChunksPerFrame`, `kGuardMs`) and check the clock BEFORE every chunk, the first too, carry unfinished input to the next frame, keep the last result on the screen while the next one is computed, and never let the work depend on how much audio arrived. Drawing budgets: 96 calls a frame (Spectrum uses at most 75), 256 polyline points (Scope 248).
- **Estimated cost** (from the compiled loops, not measured; QA-MOD-033 owes the unit's figures): a butterfly is about 40 to 50 cycles at `-Os`, so a 1,024-point transform is about 1.4 ms and a 2,048-point one about 3 ms of CPU, spread over frames (Tuner: 20 chunks a frame, one reading per 6 frames, about 1.3 ms a frame; Spectrum: 12 chunks a frame, one transform per 3 frames; both figures are estimates, and the clock guard stretches them instead of overrunning if the SDRAM makes the chunks slower).

What it costs the OS: one relaxed load a block while the tap is off. With it on, every block pays a mono mix of 64 frames, the peak and sum of squares of the block and three stats words; **the 64 samples (32 words of SDRAM) are stored only once an app reads samples** (`tap_latest` or `tap_stream`; Loudness VU reads only `tap_levels` and never causes them). Unit figures, QA-MOD-023: with the samples stored (image int.6) the whole tap work of a block read mean 1,765 / max 6,077 cycles (9.8 µs / 33.7 µs; 0.7 % of a block at the mean, 2.3 % at the worst); the Loudness VU run without the stores is to be re-measured on the next image, `diag cpu` prints the line `tap: master, ... write cycles last/mean/max`. ADR 0014 amendment J has the reasoning and the options for the sample readers. The protocol (single writer: the render; single reader: the main loop; the reader re-checks the sequence counter after a copy and says when it was lapped) is in `audio/include/o3os/audio/app_tap.h`.

### 3.5 MIDI-in ring (H1, built)

`host->io->midi_read(out, max)` (needs midi_in) copies up to `max` messages into an array of `o3m_midi_event {port, status, data1, data2}` and returns how many: **whole channel messages**, oldest first, from DIN (`port` 0) and USB (1). The ring holds 64; drain it once a frame, from `view_draw` (the OS calls it 30 times a second while your page is up), as the MIDI Monitor does. `io->midi_lost(&overflow, &sysex)` gives the counts since your app started: messages that did not fit, and SysEx messages (not delivered, only counted; real-time bytes and system common are not delivered either). The ring is switched on only while an app that holds midi_in runs, and what arrived before launch is thrown away. The core MIDI parser stays the only reader of the ports. C1 adds a per-app SysEx-in buffer (4 KB by default, chunked, flags for start / continue / end).

### 3.6 Launch context and F7 TOOLS (H1 and H3a, built)

The launch context is **not** an argument of `init`: `init(state, host)` keeps its 1.2 signature. You read it with `host->read->launch_ctx(&ctx)` after setting `ctx.size = sizeof ctx`; the OS fills `{size, flags, pattern, track, instrument, block_start, block_length, block_tracks}` (copying at most the size you gave, so a later field never overruns an older app: `block_tracks` is the one ABI 1.5 appended, and reads 0 on an OS that does not write it) and returns 1 when `flags & O3M_LAUNCH_FROM_TOOLS`, else 0. Opened from the MODULES list everything is zero.

**Pattern F7 TOOLS (H3a).** F7 on Pattern opens the TOOLS list: EXPORT first, then under a rule the installed apps that hold edit_project or export_files, sorted by name (a dim `NO TOOL APPS ON THE CARD` when there is none; CHORDS, HUMANIZE, MORPH, MUTATE, RHYTHM GEN and SMF EXPORT are tool apps like any other). Up / Down choose, F1 runs, F8 goes back. It is refused silently while the view follows the play line (LIVE, or WATCH while playing), like selecting a block. F1 launches the app with the Pattern as its context: `pattern` the pattern in view (1..255), `track` the block's first track (the cursor's with no block), `block_tracks` how many tracks the block covers, `block_start` and `block_length` its rows, `instrument` the 0-based instrument in use, `flags = O3M_LAUNCH_FROM_TOOLS`. **With no block** `block_length` and `block_tracks` are 0 and `track` / `block_start` are the cursor's: the app decides from the cursor. The first launch of an app that edits the project or writes files per boot asks first (F1 again, 3.1). The exit chord F7 + F8 returns to Pattern with the cursor and the block as they were (one level of return); an app opened from the MODULES list still returns to MODULES.

### 3.7 `export_files` (H4, ABI 1.7, built)

`host->io->export_open`, `export_write`, `export_close` and `export_status` (all need export_files and an app that declares **ABI 1.7**; all NULL otherwise; absent when `host->io->size` is below their offset). An app makes **new files** in one folder of the card and nothing else. Everything below is what `o3module.h` says; the file is the authority, and `tests/app_export_test.cpp` tests every rule.

```c
int32_t  export_open(const char* name, const char* ext);     /* AppExport/<app>/<name>.<ext> */
int32_t  export_write(const void* buf, uint32_t n);          /* 1 .. O3M_EXPORT_RING_BYTES (16 KiB) */
int32_t  export_close(void);                                  /* asks for the commit, returns at once */
uint32_t export_status(char* final_name, uint32_t cap);       /* IDLE, WRITING, DONE, FAILED */
```

- **The folder is `AppExport/<app>/`**, `<app>` being the file name of your `.tmod` without the extension (the OS chooses it, never your code). It is made when the first file is written. Files already there from an earlier launch are never opened, replaced, renamed or removed.
- **Names.** `name` is the stem: letters, digits, space, `_`, `-` and `.` (1..31 bytes, the first a letter or digit, no `..`, no Windows device name), with no path component. `ext` is one of **`mid`, `syx`, `txt`, `csv`, `json`**, lower case, no dot. Anything else is `O3M_EXPORT_RANGE`: no `.ptf`, `.tmod`, `.o3pj`, `.wav` or any other kind of file can come from an app. A name that is taken becomes `<name>_001`, `_002` ... `_999` (the stem is cut so that the suffix fits 31 bytes). The OS looks for the number with a bounded number of directory lookups (at most about 25 inside the `export_open` call): with the taken numbers in one run it gives the lowest free one, with holes some free one, and `LIMIT` when the last probe at 999 is taken; `export_status` returns the name as written.
- **Caps.** 4 MiB a file (the write that would pass it answers `O3M_EXPORT_LIMIT` and aborts the file), at most 16 opens per launch of the app (the 17th is `LIMIT`; the count starts again when the app is launched again), one file open at a time.
- **An open file holds the card.** The file's save job takes the card's one job slot from `export_open` until the commit has ended, so the OS gives up a file that is open with no write, no drain to the card and no close for **20 seconds** (`AppExport::kIdleMs`): the app reads `FAILED` and the Temp is removed. Open a file when you have bytes to write, and close it when you are done.
- **Nothing blocks.** `export_write` copies into a **16 KiB staging ring** and answers `O3M_EXPORT_BUSY` when it does not fit: nothing was taken, call again on a later frame with **the same bytes**. The OS drains the ring to the card in the main loop's job pass while you go on writing, so a 4 MiB file needs no more RAM than the ring. **A single call larger than the ring is `RANGE`** (never `BUSY`): feed it in chunks of about 1 KiB, as SMF EXPORT does. `export_close` only asks for the commit; poll `export_status` once a frame: `WRITING` until the OS has drained the ring, synced, read the file back and renamed it, then `DONE` (the file is whole on the card under its final name) or `FAILED` (nothing of it is left). Reading `DONE` or `FAILED` once returns the status to `IDLE`.
- **Codes.** `OK` 0, `BUSY` 1, `RANGE` 2 (a bad name or extension, a NULL buffer, n of 0 or over the ring), `LIMIT` 3, `STATE` 4 (an open while a file is open or committing; a write or close with no file open), `NO_CARD` 5 (no card, a card that may not be written, such as a write-protected one, or the firmware update or another save holds the card).
- **No half files.** The bytes go to a Temp file in the folder and are renamed to the final name only after the read-back; a full card, a pulled card, a power cut or a failure leaves nothing under the final name (a stale Temp is removed by the next export's heal). A file still open or committing when the app exits, is stopped for its time budget or faults is **aborted** (its Temp removed); files already `DONE` stay. A save, export or DELETE the user starts while your file is still being written cancels it (you read `FAILED`), once that request really starts: the core is never refused because of an app. A file that has begun its read-back and rename is let finish, and the user's request answers busy for that moment.
- **No reads.** An app cannot read any card file, including its own earlier exports (C2, later), and cannot see the project folder, `/modules` or `Firmware/`.
- **The confirm.** The first launch per boot says `WRITES FILES TO APPEXPORT/<APP>: F1 AGAIN` (3.1).
- **Other H4 change.** `o3m_instrument_info` (3.2) grows one member at its end, `midi_channel` (1..16 for a MIDI instrument, 0 for any other kind). A 1.6 app's 16-byte struct is filled with the old four members only.

Worked example: **SMF EXPORT** (`modules/apps/smfexport/`, section 4).

### 3.8 `now_ms`, `get_tempo`, `set_tempo` (H1, amendment B, built)

- `host->io->now_ms()`: a monotonic millisecond clock (the device's uptime, low 32 bits). Read-only, **no capability bit**. It wraps after 49.7 days: take differences with an unsigned subtraction (`now - then`), never compare two readings. It is as steady as the main loop that calls it: time a tap from the `input()` call that carries it, 16 to 33 ms of jitter at worst, and average over several taps.
  **Accuracy of a tap tempo.** The mean over N intervals is (last tap - first tap) / N, so only the two end taps count and they keep their jitter; the jitter of the taps in between cancels. An end tap 33 ms off moves the tempo by about 2 BPM over 4 intervals and about 1 BPM over 8, at 120 BPM (both ends off in opposite directions double it). Use a wide window (Tap Calc defaults to 8) and say the tempo is approximate.
- `host->read->get_tempo()`: the project's tempo as an integer, **BPM x 100** (12000 = 120.00 BPM), 4000..80000; 0 with no project. After your own `set_tempo` it returns the value you asked for even before it is applied.
- `host->edit->set_tempo(bpm_x100)`: returns `O3M_EDIT_OK`, `O3M_EDIT_RANGE` or `O3M_EDIT_NO_PROJECT`. It **queues** the change and the OS applies it a moment later (the next pass of the main loop) as one undoable unit, so calls in one frame coalesce into the last: **at most one tempo is queued per app frame, the last write wins, and each apply is one undo unit** (a refused call, out of range, does not replace the queued value). The user undoes it with the normal undo. You never save the project.
- Use: tap tempo times taps with `now_ms` (not by counting frames), shows its result, and applies one `set_tempo` when the user says so (the Tap Calc app does it on F2).

### 3.9 MIDI out and SysEx (C1, C2; PLANNED, OS-APPMIDI, "not started")

**C1.** Capability midi_out. `midi_send(port, bytes, n)` for DIN, USB or both; short messages and complete SysEx frames. They go through the OS MidiOut queue, **rate-limited per frame so the sequencer's timing wins**, with a per-app byte cap per second. Port values, the limits and return codes: [TBD].

**C2, later.** Read and list access to your own `AppExport/<app>/` (for librarians and Sample Dump), and a **send-at-tick queue** for timed output, scheduled by the render-side MidiOut, never by app code. Call names: [TBD].

Storing gear patches inside the project would be a persistent-format change (Level C), a separate decision.

## 4. Worked example and a sketch

**An app that edits the project: RHYTHM GEN** (`modules/apps/rhythmgen/`, ABI 1.5, read_project + edit_project, OS-H3A). Read it with this guide open: `init` checks that the read and edit tables have the 1.5 entries (an older OS gets a clean refusal), `enter` reads the launch context and `pattern_info`, the pure arithmetic is a header (`rhythmgen_math.h`, shared with its host test and checked against the Fill dialog's Euclid), `input` handles the keys and APPLY is one `edit_begin`, one `get_steps` + `set_steps` per lane, one `edit_end`. Its host test (`apps/rhythmgen/tests/rhythmgen_test.cpp`) drives the module on `o3m_test::FakeApi`, whose fake project has `edit_begin` / `set_steps` / `edit_end` with the OS's rules; `tests/module_edit_test.cpp` runs it through the real host and checks the undo.

**An app that edits instruments: MUTATE** (`modules/apps/mutate/`, ABI 1.6, OS-H3B). `init` insists on the 1.6 entries; `enter` reads the launch context (the current instrument), `instrument_info` and every `get_param_desc`, keeping the writable ones (12 bytes each); APPLY is one `edit_begin`, a `get_instrument_param` and, when the new value differs, a `set_instrument_param` per parameter of the chosen GROUP, one `edit_end`. The rule is `mutate_math.h` (a header the host test includes). **MORPH** (`apps/morph/`) is the same shape with A0 kept in its state; **CHORDS** and **HUMANIZE** (`apps/chords/`, `apps/humanize/`, ABI 1.5) are the step editors of this slice, and all four draw their page with `apps/common/o3m_edit.h`. What each does is in their source header and in ADR 0014 amendment H: HUMANIZE delays the noted steps of the block by a seeded Micro-move (FX 2, up to TIME) and jitters the volume of those that carry one (up to VOL either way; a step with no volume stays unset and plays at the instrument's volume; the jitter is applied to the volume it finds, so repeated APPLYs or VARYs random-walk it); CHORDS writes a progression (8 presets, a key, MAJOR / MINOR, SEVENTHS) either as the root with Chord FX 14 (CHORD FX: only a MIDI instrument plays the whole chord, every other instrument plays the root alone: `seq/src/sequencer.cpp` `pfx_note`) or as one tone per track (SPREAD, which sounds on every instrument); MUTATE moves each writable parameter of a group by a seeded random amount of its range; MORPH blends the instrument towards another of the same kind and engine (stepped and enum parameters switch at 50 %), with no live sweep. Each F1 is one undo unit named after the app.

**An app that writes a file: SMF EXPORT** (`modules/apps/smfexport/`, ABI 1.7, read_project + export_files, OS-H4). It writes the pattern in view, or the F7 TOOLS block, as a Standard MIDI File (type 1, 96 PPQ, one row = 24 ticks): the tempo and 4/4 in the first track, then one track per track that has a note, named `Track N`. The file is `PAT<nnn>.mid` (`PAT<nnn>_SEL.mid` for a block) in `AppExport/smfexport/`. The writer has no seek, so each track is built whole in a buffer (`smfexport_math.h`, a header with no OS state that its host test includes) before its length is written; the app feeds `export_write` in chunks of at most 1 KiB, retries BUSY on the next frame with the same bytes, calls `export_close`, then polls `export_status`. **Every call into an app has 4 ms, `input()` too, and an app's code and state run from SDRAM**, so F1 only starts the job and `view_draw` does it in units (scan one track, open the file, build 32 rows of a track, write one chunk, close), none once the host's millisecond clock has moved (the first version did the scan, the open and three tracks inside the F1 call and was quarantined on the unit: OS-FINALQA FQA-10b1). Micro-move (FX 2) moves the note-on; the channel is a MIDI instrument's own CHANNEL, else the track number mod 16 plus 1; a note ends at the next note, at an Off, Cut or Fade step (v1 writes all three as a note-off at that row: a Fade is not a gradual release) or at the end of the range; the velocity is the step's volume, else the instrument's default; every other FX is counted by number and shown on the page, not written (Chord FX 14 is not expanded). Its host test reads every file back with a small SMF parser, and `tests/module_edit_test.cpp` runs the whole stack (Pattern F7, the confirm, F1, the file on a fake card).

### 4.1 Sketch: a synth-editor page (PLANNED, will not compile until OS-APPMIDI C1 lands)

Pseudo-code only. Every `[TBD]` is a name the ADR does not give. `midi_send` is the ADR's and is not built; `now_ms` is real (section 3.8) and lives in `host->io`, the same table `midi_send` is planned to join.

```cpp
// Capability asked: midi_out.  (the module.json "capabilities" name for it: "midi_out", value 32; an OS of ABI 1.3 refuses the app at load, "NEEDS MIDI OUT (NEWER OS)")
// `io` below is host->io (section 3.1); `midi_send` is a planned member of it.
struct State { uint8_t cc_value[8]; uint8_t dirty; uint32_t last_send_ms; uint8_t sel; };

int init(void* raw, const o3m_host* host) {
  if (/* io table is NULL: midi_out not granted */) return 1;        // refuse, or show a note on the page
  return 0;
}

void input(void* raw, const o3m_ui_event* ev) {     // the encoder turns the selected control, a pad selects one
  if (ev->kind == O3M_EV_ENCODER) { s->cc_value[s->sel] = clamp7(s->cc_value[s->sel] + ev->value); s->dirty |= 1u << s->sel; }
}

void pump(State* s, const o3m_host* h) {            // once per frame; where this runs from is [TBD] (view_draw must only draw)
  uint32_t now = io->now_ms();
  if (s->dirty == 0 || now - s->last_send_ms < 20) return;            // rate limit: one message per 20 ms
  uint8_t msg[3] = {0xB0 | channel, cc_number[first_dirty], s->cc_value[first_dirty]};
  io->midi_send(PORT_DIN, msg, 3);                  // port constant: [TBD]
  clear_bit(first_dirty); s->last_send_ms = now;
}

void send_patch_dump(State* s, const o3m_host* h) { // one complete SysEx frame, never a loop of small ones
  uint8_t frame[160];                               // F0 <manufacturer id> <device id> ... <7-bit data> F7
  n = build_frame(frame, s->params);                // every data byte below 0x80
  if (io->midi_send(PORT_DIN, frame, n) != OK) show("DUMP REFUSED");   // return codes: [TBD]
}
```

## 5. Design guidance

**Timing.** The main loop runs at 16 to 33 ms jitter. An app is for **configuration and editing**, not for playing in time. A note that must land on a beat belongs in the sequencer, not in `midi_send`. If you need timed output, wait for the C2 send-at-tick queue.

**SysEx safety.**
- A bad dump can change or erase a synth's memory. Make sending explicit: a key press, never automatic on launch or on page change.
- Build the whole frame, check `F0 ... F7` and that every data byte is below 0x80, check the length against the buffer, then send once.
- Do not send a bulk dump while the sequencer plays unless the user asked.
- Show what was sent (a name, a byte count). Expect no reply unless you asked for one and `midi_in`/SysEx-in is granted.

**Rate limits.** Assume the OS will cut you off: there is a per-frame rate limit and a per-app byte cap per second (values [TBD]). Send changes, not every frame. Coalesce an encoder turn into one message per interval. Edits: at most one group per call, 4,096 write calls and 2,048 steps per group (a group over a limit is dropped whole). Exports: 4 MiB a file.

**One modal at a time.** The OS allows one overlay and one level of return (UI map rule 8). Your page has no overlay API: if you show a confirm or a list, draw it in your body, one at a time, and let F8 or the exit chord back out. F7 + F8 always leaves.

**UI budget rules** (`docs/ui/ui-map.md` section 5): at most 8 rows per parameter page, at most 5 F pages, F8 is BACK or UNDO, one meaning per key (Delete resets, Shift + Delete is the bigger reset, pad is sound, Shift + pad selects), and any mode that changes what keys do is visible on the page. Your soft-key labels belong to the OS; keep your own key hints in the body. Apps get their parameters as generated controls: prefer them to hand-drawn ones (a drawn control gets no Learn or undo).

**Testing.**
- Host tests as in 2.6, with a fake host. For ABI 1.3, give the fake host each table both populated and NULL, and test that your app copes with a refused call. The repository's `modules/tests/o3m_test_support.h` has `FakeApi` and `make_app_host(api, caps)` for exactly that (the clock, the tempo, a MIDI queue, the recorded `set_tempo` calls); `modules/apps/midimon/tests/midimon_test.cpp` is a worked example.
- Test `set_param` with NaN, infinities and out-of-range values.
- Keep logic that makes MIDI or edits as pure functions (frame builders, checksums, generators) and test them against known vectors.
- What the OS tests for you (ADR 0014): the undo property test (any app edit sequence, then undo, restores the snapshot); a fuzz of every host entry (no file action reachable, no mutator outside a group); capability gating (a table is NULL without its bit); a 1.2 fixture loading under 1.3; a fuzz of `midi_send` (malformed SysEx, oversize, flood); a test that the sequencer's note timing does not move under an app flood; fuzz of the export calls (no path escape, no overwrite); a card test on a card; a MIDI loopback check on the unit. Your own tests should not depend on these.

## The key row (ABI 1.9)

An app that declares ABI 1.9 puts `keys` at the end of its `o3m_ui` (write `struct_size` with `O3M_UI_SIZE(<ID>_ABI_MINOR)`, the minor your manifest declares and the generated header names, never `sizeof(o3m_ui)` and never `O3M_ABI_MINOR`, which is the SDK's newest and would give an app that declares 1.8 a table of 24 bytes, which the OS refuses): `const o3m_keys* keys(void* state)` returns `f[8]` and `shift_f[8]`, each a string of up to 7 printable ASCII characters (NULL or `""` for a key that does nothing; the row then says `-`). The OS asks once a frame, before the page draws, and after every `input()`, under the same time budget as `input()` (an overrun stops and quarantines the app), copies the 16 labels (inside the call, under your crash stamp: a wild pointer is your fault, not the OS's) and draws them on the page's key row, the Shift labels while Shift is held. The labels are a statement only: the OS hands every key to `input()` whatever they say, so label what `input()` does. Shift + F1..F7 reach `input()` as the same key with `mods & O3M_MOD_SHIFT`: an app that does not read `mods` runs Fn on Shift + Fn, so its Shift row repeats the plain row (the shipped apps do). **F8 is BACK on every app page** (declared or not): the OS acts on it and never hands F8 or Play to `input()`; Play runs the transport. Pads and Rec are still delivered. F7 + F8 stays the exit chord. An app that declares ABI 1.8 or lower shows `-- -- -- -- -- -- -- BACK` and no longer gets F8 or Play either. New Community OS behaviour; unit check open (QA-MOD-120).

## The grammar of an app page (OS-APPUX S2)

The OS puts no cursor over an app's rows: the app draws its own rows and moves them with the keys it gets, and every shipped app uses the same keys, so the user learns them once. Use `modules/apps/common/o3m_edit.h` (source you compile in: header-only, integer, no libc, no ABI):

| Key | Meaning |
|---|---|
| Up / Down | the previous / next row (stops at the ends, no wrap) |
| Left / Right | the previous / next page (an app with one page ignores them) |
| Encoder | the selected row's value, one step per detent |
| Shift + encoder | the same in coarse steps (a row with no coarse step reads as the plain encoder) |
| Delete | the row's default |
| F1-F7, Shift + F1-F7 | the app's own actions, named by `keys()` |
| F8, Play | the OS's (BACK, the transport); the app never sees them |

`o3edit::grammar(ev, &row, rows, &page, pages)` reads one event, moves the row and the page, and answers `kMoveRow`, `kMovePage`, `kMoveValue` (with the detents and whether Shift was held), `kMoveDefault` or `kMoveNone`; `o3edit::steps(in, coarse)` turns a `kMoveValue` into the row's units (`coarse` is the number of fine steps one coarse detent stands for). Shift + an arrow, Shift + Delete, Copy, Insert and the pads answer `kMoveNone`: nothing but these keys changes a value. `o3edit::draw_page` draws the title, a line of facts, the rows and the answer; it has no lines of keys, because the key row names them. Test your app with the same checks as the shipped ones: `tests/support/app_grammar.h` drives the page through the whole stack and reads it back.

## ABI 1.10: keeping your rows, the transport, being heard (OS-APPUX S3)

- **Keep a setting: `io->set_own_param(index, value)`.** Declare each setting as an `o3m_param` (`module.json` `params`, at most 32), draw it yourself and, when the user changes it, store the new value. The OS keeps it with the project (`APPM`, up to twelve apps) and hands it back through `set_param(index, value)` at the next launch, **before** `enter()`: your `enter()` must not reset a declared value (reset what is transient: a list, taps, a reading). It does not call your `set_param()` for a value you stored. A value that follows the target (a rate from the block's rows, the instrument the page opened on) is a row with an AUTO: declare the range from 0 and let 0 mean "follow"; Chords and Rhythm Gen are the examples. Delete is the row's default, which for such a row is AUTO.
- **Read the transport: `io->get_transport(&t)`** (set `t.size = sizeof t`): `playing`, `pattern`, `row` (the Pattern's play row, 0 based), `ustep`, `order` (0xFFFF in Pattern mode), `tempo_x100`, `count_in`, `queued`. No capability.
- **Be heard: `io->play_note(instrument, note, velocity)` and `io->stop_note(instrument)`** (capability `play_notes`). `instrument` 1..64, `note` 0..127 (the step byte minus 1), `velocity` 1..127. Four notes held at most and eight calls a frame (`O3M_PLAY_LIMIT`); all of them end when the page closes. Use the page's draw as your clock (30 Hz) for anything timed: Rhythm Gen plays a lane's hits at eight steps a second this way. Do not call `play_note` from anywhere but your entry points, and stop what you start.
- **A screen button leaves your app**, whatever its minor; set what must survive with `set_own_param` as the user changes it, not at `exit()`.

