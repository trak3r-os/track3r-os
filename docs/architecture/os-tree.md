# Community OS tree

GPL-3.0-or-later (`LICENSE`, ADR 0005). Spec: `docs/specs/OS-001-walking-skeleton.md`.

Layers, each a CMake library with headers in `<layer>/include/o3os/<layer>/` (include as `<o3os/core/ring.h>`):
`core` (containers, ring, input event, the pure parts of the input pipeline: gesture recogniser, binding
resolver, keymap parser; and the notification queue) <- `usb` (host-pure USB library, below) <- `hal` (contracts) <- `eve` (FT81x display lists) <- `ui` (renderer, colour tokens and
themes, faces, tile and icon assets, components) <- `app` (the screens and their registry, the overlays, action registry,
built-in bindings, demo data). `platform/<name>` (host fakes, SHA-256, PNG
writer; `platform/qemu`, the Cortex-M4F target test layer; `platform/mk66`, the MK66 platform layer and skeleton image)
and `sim` (the display-list rasteriser, `o3sim`,
scenarios) implement the HAL; nothing below `app` may include them. The one exception is a composition root, listed by file name in
`tools/check_layers.py` (`platform/mk66/src/app_main.cpp`, the MK66 image's wiring of the app to the real display).
Beside that chain: `model` (the project model, depends on `core`) and `formats` (byte-stream codecs, depends on
`core` and `model`). `app` (and `sim`, tests) may use both; `hal`, `eve` and `ui` may not. `audio` (the pure audio engine
skeleton, OS-005a, and the fixed graph, OS-005b: it reaches down to `core`, `usb` and `hal`, not to `model`: its track count and ParamIds are its own and `tests/audio_model_ids_test.cpp` holds them equal to the model's) sits beside the chain the same way:
`app`, `platform`, `sim` and tests may use it, `hal`, `eve` and `ui` may not. `midi` (the MIDI timing core, OS-005c: it may include `core`, `usb`, `hal` and `audio`'s event struct) and `seq` (sequencer v2:
`core`, `usb`, `hal`, `audio`, `model` and `midi`) are two more such libraries; `app`, `sim` and tests may use both, `platform` only `midi` (the MK66 MIDI drivers of OS-005c2 bind its emitter).
`tools/check_layers.py` enforces the order, the no-heap / no-`double` rules (also for `model`, `formats`, `audio`, `midi` and `seq`) and
SPDX headers (ctest: `layers`, `layers_selftest`). Every library (`ui` includes `ui/plot`) also builds for the MK66 (static libraries,
sizes printed).

Look: ADR 0001 and ADR 0010, written down in `docs/ui/style-guide.md` (grid and metrics, the type scale, the three
palettes with their contrast tables, components, icons, motion, the budget rule, how to add a theme).
`ui/include/o3os/ui/style.h` names the 16 colour tokens; a theme (`ui/theme.h`) gives them values, the four faces and the
spacing, and the built-in tables in `ui/src/themes.cpp` are the only place colour values are written. Three themes are
compiled in: **Amiga** (default), **Black**, **White**. Faces: Terminus Bold 16x32 (Display) and 10x20 (Data), SIL OFL 1.1
(`third_party/fonts/terminus`), Spleen 8x16 (Label) and 6x12 (Micro), BSD-2-Clause (`third_party/fonts/spleen`),
converted at build time into FT81x L1 data by `tools/font_convert.py`; the icon face is drawn in-house in
`tools/icon_convert.py`; the pattern grid's hex-pair and note tiles are composed from the Data face at boot
(`ui::upload_assets`). The generated sources live in the build tree. Flash for faces, icons and the tile code on the
MK66: about 13.7 KB (`THIRD_PARTY.md`, `ui` library sizes in the build output).

## Credits

Created by **trak3r-os**, creator and maintainer of track3r-os. `AUTHORS` lists the people that "the track3r-os authors" in the copyright and SPDX lines means (the file headers stay as
they are when the list grows). The code of other projects that this tree vendors, ports or borrows keeps its own authors and licence: `THIRD_PARTY.md`. The same credit is on the Config screen's INFO page (CREATED BY).

## Plots (OS-003d, `ui/plot`)

A plotting library inside `ui` (ADR 0010 amendment 2; the look is section 11 of the style guide, the spec is `docs/specs/OS-003-ui-toolkit.md`).
Headers `ui/plot/include/o3os/ui/plot/`, sources `ui/plot/src/`, drawn through `ui::Renderer`'s raw sections.

- `peak_cache.h` (+ `peak_kernel.h`): the min/max mipmap of int16 mono or stereo frames in caller-provided fixed storage:
  levels of 64, 256, 1024 ... frames an entry, **0.0833 bytes a sample** (4 bytes an entry), built in cooperative slices,
  updated after an edit in O(edit + levels), queried per column at any zoom in O(columns). The int16 min/max loop is written
  once over an `Ops` policy: `M4Ops` is `ssub16` + `sel` on the Cortex-M4, `EmuOps` a model of them the host tests run.
- `wave_bitmap.h`, `wave_view.h`: a wave lane drawn as an L1/L4 column bitmap (rendered row by row from the columns' run
  starts and ends, uploaded in 4 KB chunks into the idle one of two RAM_G slots per lane: about 14 words whatever the width) or as
  two `EDGE_STRIP_B` fills (the same pixels as L1, 2 words a column, bounded by grouping); marks: selection, loop, slices,
  playhead, zero line, clip bands.
- `curve.h`, `plot_math.h`: a series (float or Q15) mapped through a linear, log-frequency or dB axis, reduced to a point a
  column and by Ramer-Douglas-Peucker to at most 192 vertices, fill + line, axes and a grid with Micro-face labels, handles.
  The math (axes, dB, the biquad response in `sin^2(w/2)` so that single precision does not cancel at low frequencies, ticks,
  number formats) is pure.
- `fft.h`, `spectrum.h`: a real FFT of 16 to 512 points (CMSIS `arm_rfft_fast_f32` layout) and the analyser: log bars in dB,
  smoothing, peak hold. `scope.h`: the XY / mid-side scope with a phosphor trail. `meters.h`: peak, RMS, hold and clip bars on a
  dB scale from a LUT, vertical or horizontal.
- `ram_g_map.h`: the one map of RAM_G (faces, tiles and icons, four wave slots, spare), with its static_asserts.

Tests: `ctest -R plot` (kernels, drawing, raw sections, the word bounds), `qemu_plot` (the same, on the Cortex-M4F), `plot_budget` (the plot gallery in 1792 words, every theme, both modes),
`contrast` (the pairs of the plot pictures), `plot_bench_smoke`. Benches (relative figures; Release build for numbers):
`cmake -S . -B build-os-bench -DCMAKE_BUILD_TYPE=Release && cmake --build build-os-bench --target o3os_plot_bench &&
build-os-bench/tests/o3os_plot_bench`. The same unit tests (all but the ones that draw into the simulator's rasteriser) run on a Cortex-M4F as `qemu_plot`, below.

## The pattern view (OS-001b)

The pattern screen (`app/`): 5 tracks and 16 steps visible, restyled in OS-003b (two materials, Display-face tempo and
pattern number, key-caps, tile-drawn grid; see the style guide). Since OS-009 P1a.1 it shows the attached document's pattern and
edits it (next sections after the input pipeline: "Pattern editing"); with no document it is OS-001b's read-only demo (12 tracks x 64
steps of static data, nothing stored). The table is the keys of the demo and of Browse, the mode it starts in. Input,
in terms of the Tracker's buttons (the hardware numbering, names in
`app/buttons.h`; scenario scripts use the lower-case names). These are the built-in bindings (`app/bindings.cpp`);
the next sections say how they work and how to change them. The arrows, the encoder and the F-keys are bound in `global`:
they act on whatever screen is in view (the Cursor interface, below); the column buttons and the pads are the pattern
screen's own (`pattern`). The screens, overlays and modes of OS-002b follow after the table.

| Input | Action | Effect |
|---|---|---|
| Left / Right | `nav.left` / `nav.right` | previous / next column (Note, Instr, Vol, FX1, FX2); past either end of a track, into the neighbouring track; the window of 5 tracks follows |
| Up / Down | `nav.up` / `nav.down` | previous / next step; the window of 16 steps centres the cursor (OS-CENTER): the cursor's row goes down the page to the centre slot (the 8th, seven rows above it), then stays there while the pattern scrolls, also at the first and the last row (OS-PATCENTER, new Community OS behaviour: at step 1 the seven rows above the cursor are blank, at the last step the eight below it). The same for the encoder, the jumps and the playhead of Browse |
| Encoder | `nav.scroll` (detents) | moves the cursor by the detents turned (Browse: it scrolls the steps; in Edit and Select it edits the field, see "Pattern editing") |
| Note, Instr, Vol | `pattern.column` (0, 1, 2) | jump to that column of the current track |
| Fx | `pattern.fx_column` | FX1, and pressed again FX2, then FX1 again |
| Note + Instr | `pattern.zoom` | OS-ZOOM (new Community OS behaviour; `docs/specs/PAT-NAV.md`): the zoom of the Pattern grid, 5 tracks (every field, Data face) -> 8 (Note, Instr, Vol; Label face) -> 12 (Note; Micro face) -> 5. RAM-only (`PatternView::zoom`): 5 at boot, kept across a project load. The window is clamped to the tracks and holds the cursor; a column the level hides snaps to the nearest visible one on its left; the key of a hidden column says `FIELD HIDDEN: NOTE + INSTR ZOOMS` (the hold popups still open). Note is a held modifier (`kWantModifier`, `core/gesture.h`): any other key pressed with it cancels its tap and hold, so Note + Vol and Note + Fx are Vol and Fx. Also in the step popup |
| Note + Left / Right | `cursor.track` -1 / 1 | OS-ZOOM: the cursor a whole track, the column and the step kept; repeats while held; also in the step popup (the popup stays open) |
| Shift + Note / Instr / Vol / Fx | `cursor.jump_fraction` 0..3 | OS-QJUMP (new Community OS behaviour): 0, 1/4, 2/4, 3/4 of the pattern (a 64-step pattern: rows 1, 17, 33, 49 as numbered); the column stays |
| F1-F3 | `chooser.toggle` (tap), `chooser.hold` (hold) | the pattern, length and step-jump choosers (OS-009); with no document they say their label as below |
| F4 | `fill.open` | FILL (OS-PTOOLS, `docs/specs/OS-PTOOLS.md`; Edit; new behaviour): the Fill dialog, a modal overlay (context `fill`) of eight descriptor rows (SCOPE block / track / pattern, WHAT note / instr / volume / FX 1 / FX 2, TYPE every N / random / Euclid / scale, N or EVENTS or DENSITY, MODE const / ramp / random, KEY (root and scale) or FX TYPE, FROM, TO; OS-FILL2, `docs/specs/OS-FILL2.md`) with its own key row: Up / Down the row (`fill.move`), Left / Right / encoder the value (`fill.delta`), F1 FILL (`fill.apply`, one undo unit), F8 BACK. Outside Edit it says "FILL: TURN EDIT ON" |
| F7 | `ui.softkey` (6) | free on Pattern (TOOLS is OS-PTOOLS part 2): its label is "-" and a "-" or "--" key posts nothing (OS-CFGWIRE). Any other soft key with no function yet posts its label (`F2 PLAY SNG`) as an info notification, 2 s in the strip over the info line |
| F5 | `pattern.preview` | PREVIEW (OS-CFGWIRE): the step under the cursor sounds on the cursor's track while the key is down (`PreviewSink::hold_note`, the Preview key's voice), with its own instrument or the last used one; a step with no note says so. Playing the whole selection through the sequencer is OS-PTOOLS |
| F6 | `pattern.invert` | INVERT (OS-CFGWIRE; Edit): the steps of the block in the reverse order, each track on its own (whole steps); one step says "SELECT A BLOCK TO INVERT" |
| (no key since OS-QJUMP; it was Shift + Fx) | `pattern.fill` | FILL again (OS-PTOOLS): repeats the last fill of the dialog on its scope as it is now (a random fill draws again). Before any: the OS-CFGWIRE fill (Edit; new behaviour): the current note on every Nth step (N = the JUMP edit step, 0 counts as 1) of the block, or of the cursor's whole track; one undo unit. Outside Edit it says "FILL: TURN EDIT ON (EXPORT: F7 TOOLS)": the export is Pattern F7 TOOLS / EXPORT PATTERN now (OS-WAVEXP); `pattern.export` (no key) only says so |
| F8 | `edit.undo` (Pattern, Song) | UNDO: the last undo unit of the attached document, through the Project's own mutators (OS-002c, below); "NOTHING TO UNDO" when there is none or no document. In Learn and the Navigator it is Back |
| Shift + F8 | `edit.redo` (Pattern, Song) | REDO |
| Pad 0-47 | `pad.preview` (Browse), by field in Edit | Browse: previews (the engine hook is OS-005); in the Instr column it also chooses the instrument (pad + 1, shown in the strip). Edit: enters the field's value |
| Rec | `pattern.edit_toggle` | **STEP** on / off (OS-RECMODE2, replaces OS-RECSWAP's mapping): Edit at a free cursor, notes at the cursor even while playing, the cursor advances by JUMP; from LIVE it goes to STEP (the take ends, the cursor stays); from STEP back to WATCH (Browse, the view follows the playhead). Chips WATCH / STEP / LIVE in the info line |
| Shift + Rec | `pattern.rec_free` | **LIVE** on / off (the action keeps its OS-LIVEREC name): notes go to the playhead's step (OS-LIVEREC), the cursor and view follow it, and the take is one undo unit. Stopped it starts the transport with the COUNT-IN (OS-METRO) and LIVE together; LIVE ends, to WATCH, when the transport stops. Nothing on another screen |
| Play | `transport.play` (global) | OS-005d lite: stopped, plays the pattern being edited from its first step; playing, stops. The status strip's badge follows (PLAY / STOP). On the device the first press also opens the outputs (they are muted from boot until then); the simulator has no transport and the key does nothing |
| Vol + Fx together | `learn.toggle` (global) | opens the Learn overlay; the same chord, or F8, closes it |
| Shift + Master | `ui.theme.next` (global) | the next colour theme: Amiga, Black, White, Amiga, ... at once, no reboot (provisional until Config exists) |
| anything else | | ignored; so is every release (but the one that ends a hold, below) |

The selected row is the one strong highlight (ADR 0001); the cursor cell inside it is the one orange. The screen
is limited by the FT812's 2048-word display list (every state of every screen in every theme stays within 1792 words),
see `sim/README.md`.

## Screens, overlays, modes, notifications (OS-002b, ADR 0009 rev 1)

**Screens** (`app/screens/`). A `Screen` renders one full-panel view from `(Model, ViewState)`, names its binding
context, its F-key labels and, if it has them, its modes and pages. It has no `on_action`: what a key does is in the
feature files. Screens are static instances; the registry (`screens/registry.cpp`) is one constexpr table with a row for
every screen button of the device: `{id, name, Screen*, button, key, icon}`. Eight screens exist, **Pattern** (the first
registered one), **Song**, **Instrument PARAMS** (OS-002d, "Parameters" below), **File** (OS-010a, "The File screen" below) and **Sample load** **Wave / Chop**, **Sample edit** and **Config** (OS-011a, "The sample screens" below); the other three rows have `Screen* = nullptr`: the Navigator shows them disabled and their
button posts "<NAME> IS NOT AVAILABLE YET". `nav.screen <id>` (bound to each screen button, `ScreenId` in
`screens/ids.h`) switches the base screen, calling `leave` on the old one and then `enter` on the new; the current
screen's own button cycles its pages instead (Song has two: the order list, and an arrangement with start bar, length
in bars and a bar graphic; Pattern has one). The page and the mode are kept per screen.
*Adding a screen*: one class, one registry row, its default bindings (see the style guide, section 9).

**One overlay over the base screen.** An `Overlay` declares the rectangle it covers; the base screen is handed it as
`covered` and skips every row, field or component that lies entirely inside it (the overlay paints over it), while a
partly covered one is drawn and then painted over. The binding chain is `overlay, mode, screen, global`; a modal
overlay's context comes first and captures, so what it does not bind never reaches the screen under it.

| Overlay | Opened by | Chosen / closed by |
|---|---|---|
| **Learn** (modal) | Vol + Fx together (`learn.toggle`) | the chord again, or F8 (`ui.back`). Shows the title, "Move a control or send a MIDI CC.", the target (the parameter under the cursor of the screen below, a dash when it has none) with its binding, and the exit hint; with a target F1 unbinds it and F2 steps its mode (OS-002d, "Parameters" below) |
| **Navigator** (modal) | Shift + Pattern, tapped or held (`navigator.open`; OS-NAV2) | a grid of every registered screen (name, icon, the screen button; not-yet-existing ones disabled), generated from the registry. It **stays open** after the keys are let go (a release does nothing). Arrows or the encoder choose; **F8 GO** (or F1) opens the chosen screen (`navigator.accept`); **F7 CLOSE** or Pattern again closes it without changing the view (`ui.back`). F8 is Back in the other overlays, so the footer reads F7 CLOSE / F8 GO, and the screen's F-key row under it is replaced by the Navigator's own (F1 GO, F7 CLOSE, F8 GO, the rest blank; with Shift held the quarter jumps 0/4 .. END) until it closes. Shift + F1..F5 jump around the grid |
| **Notification strip** | a notification is queued | not modal: it is exactly the info line's rectangle while it shows, so nothing is lost under it and the screen's keys keep working; a modal overlay takes precedence and the queue waits |

There is no Back button on the device: **F8** is Back in an overlay's own context (maintainer decision, `product-ux.md`),
as the Chop mock-up's `F8 BACK` in section 4 of it. The one exception is the Navigator (OS-NAV2, maintainer): its **F8 is GO** and its Back is **F7**
(or Pattern again), and its footer and the F-key row say so (`F7 CLOSE`, `F8 GO`).

**Navigation keys.** Plain actions with an argument, so any gesture can call them (tap, hold, double tap, chord,
hold + encoder, Shift) and they work on the pattern grid, the song list and the Navigator alike: they move whatever owns the
input through the `Cursor` interface (`screens/cursor.h`: count, position, page, set position, step, and track, home, end)
that every `Screen` and `Overlay` is, and name no screen, so a new screen is navigable by implementing it:
`cursor.jump_fraction n` (n = 0..3: to n/4 of the pattern, list or grid), `cursor.jump_end`, `cursor.page +-1`
(a screenful), `cursor.track +-1` (pattern only), `cursor.home`, `cursor.end` (pattern: the first / last track; lists: the
first / last item). The built-in device bindings are **Shift + F1..F4 = 0/4, 1/4, 2/4, 3/4 and Shift + F5 = the end**, in
`global` (so on every screen) and, because a capturing context reaches nothing outside it, again in the Navigator's. Every Shift binding of the built-in layer is listed in `bindings.cpp` and in
`tests/builtin_bindings_test.cpp`, which checks that none conflicts with another and that none takes a plain key (a held
Shift still falls through to the plain binding; Shift + Play and Copy keep their meaning for later, the Shift rows of the pattern's
editing, OS-009, are in the list). Shift + F1..F8 were once planned as "mute track 1-8" on the pattern screen (`product-ux.md` section 3): the maintainer took F1..F5 for the jumps and moved mute and solo to Shift + the pads (`product-ux.md`), which OS-009 P1a.1 binds (the first two rows of pads).
Reserved: the OctaMED / ProTracker PC-keyboard layer, F6..F10 of the PC keyboard for the same actions
(`InputKind::PcKey`, nothing produces it yet).

**Modes and fields.** A screen's modes are the "mode" level of the chain and the field under its cursor the level after it
(OS-009 D3): Pattern has **Edit** (0), **Follow** (1, retired by OS-004d: never entered, number kept), **Select** (2, retired by OS-LIVEREC the same way: the block is Shift + the arrows in every mode) and **Browse** (3, the default: `Screen::default_mode`), each
with its own context (`pattern.edit`, `pattern.follow`, `pattern.select`, `pattern.browse` in a keymap; the retired ones bind nothing, a keymap that names them still loads), and four field contexts
(`pattern.note`, `.instr`, `.vol`, `.fx`: FX1 and FX2 share one; `Screen::field_context`). Rec toggles Browse and Edit, `view.mode n` picks one (not a retired one) and `view.mode_next` (no key) goes round the two; the info line's chip says which (WATCH, STEP, LIVE). The chain is overlay, mode, field, screen,
global (`core::ContextChain::kMax` 5): a mode's row overrides, a field's row specialises the screen's default. The gesture layer compares
the chain without the field level (`App::sync_context`), so moving the cursor from one column to another does not make it forget a held
button. Everything a mode does not bind falls through to the field's and then the screen's keys.

**Notifications** (`core/notify.h`). A queue of 4 `{severity info | warn | error, text (48 characters), source, ttl_ms}`;
nothing posts but actions (the F-key placeholders, a screen button with no screen). The head is shown for exactly its
TTL, counted by `hal::Clock::uptime_ms` only while it is on screen and from the update that first showed it (so the
simulator is deterministic and a notification that waited behind Learn still gets all its time). When the queue is
full the oldest entry of the lowest severity goes; an error is never dropped for an info (nor a warning), and a new
entry that is less severe than everything queued is the one refused. **The same news twice** (same text and same source)
while the first is still queued is one entry with a count, shown "F3 JUMP x3": the second post raises the count (to 99), keeps
the more severe of the two and, if the entry is the head, shows it for its whole time again from now; it costs no room, even
in a full queue. The severity is a small tile with a letter (`i`, `!`,
`X`) in front of the text: the palette has no red, so it is the tile's fill (ChromeText, ChromeDim, Sel).

## The input pipeline (OS-002a, ADR 0009 rev 1)

Every input takes one road: **HAL event -> gesture -> binding -> action**. `App` has no key logic; screens never
switch on actions.

- **Gestures** (`core/gesture.h`). A stateful recogniser turns timestamped `InputEvent`s into `Gesture{kind, input,
  modifier, value}`: `Tap`, `Hold`, `DoubleTap`, `HoldEncoder` (encoder turned while a button is held), `Chord` (two
  buttons together), `HoldEnd` (the release of a button whose hold fired: "hold to open, let go to pick", OS-002b; any
  other release is silent). All timings are one table (`kGestureTimings`: hold 400 ms, our choice; double tap 300 ms and
  chord 40 ms are provisional). It never reads a clock: time comes in as the events' timestamps and `advance(now)`,
  so a run is a pure function of its input. A button is only held back when a binding needs the wait (a button with
  no hold / double-tap / chord binding taps at the press): that is why Vol and Fx, which are in the Learn chord, tap
  at the release or after 40 ms rather than at the press (kept on purpose). A button that has a "held while the encoder
  turns" binding (`kWantHoldEncoder`, OS-009: Fx + encoder is the FX type) waits for its release too, so that a turn can still
  take it and the Fx that is held to change the type does not also cycle FX1 / FX2. Shift is a declared modifier
  (`kModifierButtons`): the next tap, pad hit or encoder turn carries it. A binding that names Shift wins; if none
  does, the plain binding runs, so a held Shift never kills a key (Shift+Left with no Shift binding is Left).
  What a held button means is forgotten when the context chain changes (a screen, a mode or an overlay: `App::reset_input`
  does it too, on demand), with two exceptions that are still physically down: **Shift** (held across a screen change, a
  mode change or an overlay opening it still modifies the next key: Shift + Rec twice goes to Edit following and then free, Shift +
  Pattern opening the Navigator keeps Shift for the jumps in it) and a button whose **hold has fired** (so the release that
  ends the hold is still seen after the hold itself opened an overlay). The price is that a Shift whose release was lost is
  not cleared by a context change: `reset_input()` clears it, and so does pressing or releasing Shift again. A `holdend`
  binding makes its button wait for the hold, as a `hold` binding does (without one the end of a hold would never be seen).
  Time only runs on in a pass once the input queue has been seen empty,
  so a pass cut short by the 64-event cap never lets a hold overtake the release still queued.
- **Bindings** (`core/binding.h`). `(context, gesture + input + modifier) -> action [+ argument]`. Three layers,
  **user > preset > built-in**; built-in is a constexpr table (`app/bindings.cpp`, conflict-checked at compile time),
  user and preset are fixed pools (32 and 64 bindings, 16 bytes each) loaded from text. The context chain runs
  innermost first (overlay, mode, screen, global; `App::context_chain`). Resolution is **context first, then
  layer**: the chain is walked from the innermost context out, and in each context the strongest layer wins; the
  first context with a binding for the gesture (`none` included) ends the walk. So a user or preset line in `global`
  never shadows a screen's own built-in binding: to change a screen's key, write the line in that screen's
  context. An overlay may **capture**: what it does not bind is swallowed and nothing outside it is consulted.
  Action `none` unbinds (within its context). A conflict is two bindings of one layer and context whose triggers
  overlap; the loader reports it with both lines, it is never resolved silently. Bindings shadowed by `none` do
  not make a button wait for a hold, double tap or chord nobody will answer.
- **Actions** (`app/actions.h`). `ActionId` is the FNV-1a hash of the lower-case name, `ActionArg` an `int32`, flags
  `undoable` / `repeatable` / `needs_focus`. The registry is one constexpr table in `app/actions.cpp`, checked at
  compile time; handlers are in feature files (`nav_actions.cpp`, `screen_actions.cpp`, `pattern_actions.cpp`,
  `ui_actions.cpp`) and work on the model plus a small `ViewState`. To add one: write the handler, declare it in `src/action_handlers.h`, add one
  `make_action()` line, bind it. `repeatable` is carried for key repeat; `undoable` (OS-002c) makes `dispatch()` hand the document's editor to the handler and make its edits one undo unit; `needs_focus` (the
  base screen owns the input: no modal overlay is up) and the `enabled` predicate are honoured by `dispatch()`. The cursor
  actions do not need focus: they move whatever owns the input, the Navigator's grid while it is up.
- **Keymaps** (`core/keymap.h`). Text, one binding per line, parsed from a buffer (the loader does no I/O), all or
  nothing:

  ```
  o3keymap 1
  pattern tap left nav.left               # context gesture trigger action [arg]
  pattern tap shift+left nav.down 2       # a modifier held
  pattern tap pad* instrument.select @id+1   # arg: integer, @id (pad index) or @value (detents), +/- offset
  global  hold f1 ui.softkey 7
  global  chord vol+fx learn.toggle
  pattern holdenc instr+enc0 pattern.column 3
  navigator tap f8 navigator.accept
  pattern.browse tap up cursor.page -1    # contexts: global, pattern, pattern.edit, pattern.follow, pattern.select,
                                          # pattern.browse, pattern.note, pattern.instr, pattern.vol, pattern.fx, chooser,
                                          # song, learn, navigator, inputtest, instrument
  ```

  `App::load_user_keymap` / `load_preset_keymap` replace a layer; a rejected keymap leaves that layer empty and the
  built-ins in force. The format is version 1 (new; no older loader exists yet). A `<modifier>+` prefix on a tap,
  hold, holdend or double must name a declared modifier (Shift today); `holdenc` and `chord` take any button. A leading
  UTF-8 byte order mark is skipped; a NUL inside a word is an error.

  Action ids are renamed without an alias while no user keymap loader exists on the device (a keymap naming a gone id is rejected as `unknown action`): `step.backspace` became `step.pull` this way (OS-DELPULL). From the day a loader reads a user's keymap file from the card, every rename needs an alias (the old id kept as a synonym), or the user's layer is lost.

Reserved for later slices: `InputKind::PcKey`, `Hid`, `MidiNote`, `MidiCc` (nothing produces or parses them yet).

Follow-ups recorded here:
- (Closed by OS-009 P1a.1: Select is now the block mark, the arrows extend a block, and Shift + arrows select in every mode (Follow, which could not select, was retired by OS-004d; Browse does not select while the sequencer plays).)
- Notifications queue first in, first out: eight different quick F-keys are 16 s of strips (the overflow policy drops the
  oldest infos); the same key again coalesces into a count.
- `core/keymap.cpp` parses the application's words (buttons, contexts, pad and encoder tokens), which is more than
  `core` should know: move it out of `core` (to its own layer, or `app`) before the PcKey / Hid / MIDI words land.

## Undo, jobs and the WAV importer (OS-002c, ADR 0009 rev 1)

Three pure parts, each with a QEMU suite, and their glue in `app`. `docs/specs/OS-002-extension-architecture.md` has the scope.

**The undo journal** (`model/undo.h`, `model/editor.h`). A fixed byte ring in a region the platform gives it (ADR 0013 D7: 384 KiB
of SDRAM, `kUndoRegionBytes`; a static array in the tests), of **units**: one unit is everything one undoable action changed.
`Editor` is the undoable face of a `Project`: the same mutators with the same checks and `Status`, which also write a record of
what they changed (old and new value, as model-level patches: `set_step`, `remove_order`, `delete_pattern`... 23 kinds, every
user edit of the header, tracks, master, song, patterns and instruments; modules and unknown chunks are load-time plumbing and are
not journalled). `undo()` / `redo()` replay the records through the Project's own mutators, so generations bump and screens
redraw like for a first edit. Not persisted and not versioned: RAM only.

| | bytes |
|---|---|
| unit | header 16 (`u32` length, `u32` action id, `i32` argument, `u16` records, `u16` check) + records + trailer 4 (`u32` length) |
| record | header 4 (`u8` op, 0, `u16` payload length) + payload padded to 4 + trailer 4 (`u32` length): the lengths at the ends let undo walk backwards |
| one `set_step` | 48 (payload 17: id, track, step, new step 7, old step 7) -> **8,192** in the ring |
| one song edit (`set_order`, `insert_order`, `remove_order`, `append_order`) | 32 -> **12,288** levels |
| `clear_order` of 255 positions / one instrument / `clear_pattern` / `delete_pattern` | 284 / 1,472 (1,464 before OS-STEREO step 2, 1,440 before OS-INSTUI S8b) / 14,368 / 14,412 -> 1,384 / 268 / 27 / **27** |

The ring is bounded by bytes, not levels: a unit that does not fit drops the **oldest units, whole** (`dropped_units()`;
`depth()` is what is left), so at least 27 levels of even the largest single edit and 12,288 of the smallest (PAR-068 records 10 on
Song). A new edit discards the redo units. A unit that cannot fit an empty ring (one huge group) cannot be undone, so
the whole history is cleared (it could no longer be applied to the changed project) and `lost_units()` counts it; the edit
stays done. Records are circular and wrap round the end of the ring (copies are split), so no space is lost at the end; every
length and a check word are verified before a unit is applied, a mismatch (a bug, a stray write) clears the history and
`undo()` answers `Failed`, never applies half a unit.

Groups: `begin_group(action, arg)` / `end_group()` nest and the outermost names the unit (`journal.newest_undo()` returns the
pair a macro replays); a mutator outside any group is a unit of its own; a group in which every edit was rejected, or that only
set things to what they were, leaves nothing and does not end the redo history. The journal remembers the Project's generation
after the last thing the Editor did: a Project changed behind its back (a load, a raw mutator) is `Stale`, the history is
cleared and nothing is applied (`history_reset()` after a deliberate load). *Exactness* is by value: every accessor reads as
before the unit; the generations only advance, and which pool slot a restored pattern lands in is not observable.

*Actions.* An action flagged `undoable` is handed `ActionContext::edit` by `dispatch()`, and nothing else is; its edits are one
unit named by `(ActionId, ActionArg)`; without a document it is `Disabled`. `edit.undo` and `edit.redo` (F8, Shift + F8 on Pattern
and Song; Back in the overlays) are plain actions. `project.tempo <BPM delta>` is the one undoable action of this slice (a proof of
the path, unbound: the screens still draw static demo data and the pattern and song editing actions come with OS-004). A document
is attached with `App::attach_document(&editor)`; the simulator has none yet. *Macros* are actions only: `macro.record` toggles,
and while it records `dispatch()` appends every action that ran as `(ActionId, ActionArg)` (64 steps; the history and macro
controls are left out; it stops by itself when full); `macro.play` dispatches them again as **one** undo unit.

**The job runner** (`core/job.h`). `Job::step(budget_us) -> Running | Done | Failed`, `progress()` 0..1000, `cancel()`, `name()`,
`detail()`; four static slots in a `JobRunner`, run by `App::run_once()` with `kJobBudgetUs` = 2,000 us per pass. `run()` steps the
Running jobs in turn, starting after the one that ran last, each with `remaining / jobs_left` microseconds (a job that finishes early
leaves its share to the next); nothing starts with less than 50 us left (a budget of 0 runs nothing); a job is called at most once per
pass. The time is any `Clock` with `now()` (`hal::Clock`), so the simulator's runs are deterministic. A step is as long as the job makes
it, the budget a request: the runner measures every step and counts those that took more than the slice plus a quarter plus 20 us
(`JobStats::overruns`, `max_step_us`); the jobs after an overrun find less left. A job is only stepped if it is still Running when its
turn comes (a step may cancel or release a sibling) and the pass divides what is left among the jobs that are. A finished slot stays
occupied until `release()`; a job that is not `cancellable()` (an import that has half changed the project) ignores `cancel()`.
`App::report_jobs()` turns the state into the notification path (`NotifyQueue::upsert`: one entry that is rewritten):
**IMPORT KICK.WAV 45%** while it runs (refreshed every pass, the screen redrawn when the percentage moves),
**IMPORT KICK.WAV DONE** / **CANCELLED** / **FAILED: TRUNCATED** (an error, 4 s) when it ends; a line that is too long loses characters of the
file name, never the percentage or the reason. `job.cancel` cancels every running job that can be.

**Imports as jobs** (`app::ImportJob`, `App::start_import`). Any reader of the format registry (`formats::FormatReader`: the WAV importer, the OG
files, a native project) runs as a job: 4 bytes of source per microsecond of budget (PROVISIONAL, `kBytesPerUs`), progress from the reader (or the
bytes read of the size the platform knew), the reason of a failure in words (`TRUNCATED`, `UNSUPPORTED FORMAT`, `TOO BIG FOR MEMORY`...). One import
at a time. The readers write into the project directly, not through the editor, so **whatever an import changed in the attached document ends its
undo history**: when it is over the app checks `Editor::in_sync()`, calls `Session::reset_history()` and, if there was anything to lose, says
"UNDO HISTORY CLEARED BY THE IMPORT" (`can_undo()` and `can_redo()` already say false the moment the project and the history part). Only a reader that
changes nothing before it is done can be cancelled (`FormatReader::can_cancel()`: the WAV reader); the others run to the end. A sample import that comes in
whole is chained to a second job, **PEAKS KICK.WAV 40%**, the cooperative build of its `ui::plot::PeakCache` (`app::PeakJob`, 8 reads per microsecond,
PROVISIONAL; storage from the request, `App::peak_cache()`), when the request gives storage for it.

**The WAV importer** (`formats/wav/`, its own library `formats_wav`, depends on `core` only; resumable `begin(source, dst, capacity, keep_stereo) /
step(byte_budget) / report()` like the native reader). Reads PCM 8-bit (unsigned), 16-bit and 24-bit and IEEE float32, mono or stereo, any rate (kept
in `Info`, nothing is resampled), also in WAVE_FORMAT_EXTENSIBLE, into int16: 8 and 16 bit exactly; 24-bit and float with TPDF dither (+-1 LSB,
fixed-seed xorshift: a pure function of the input) on the samples that lose something, so a 16-bit file stored in 24 bits or float is bit-exact.
**A stereo file is mixed to mono, (L + R) / 2, by default** (this OS's choice; the engine's voices play mono until OS-005d): the mean is formed at 24-bit
precision and dithered where it is not a whole 16-bit step (L = R passes bit-exact); `keep_stereo` keeps it interleaved. `smpl`: the root note and
the first loop (forward, ping-pong, backward; further loops counted); `cue`: the cue offsets as slices (ascending, unique, inside the sample, the lowest
48; the rest counted); both before or after the data. Hostile input: sizes are checked against the RIFF, what the source really delivers and
the buffer before a byte is written (the data chunk that does not fit fails `LimitExceeded` with nothing written); typed errors (`Truncated`,
`BadMagic`, `Unsupported`, `LimitExceeded`, `Malformed`, `SourceFailed`, `Cancelled`) with a detail text and the offset; RIFX / RF64, 32-bit integer,
64-bit float, ADPCM and more than two channels are refused as unsupported; duplicate or missing `fmt` / `data`, `data` before `fmt`, a zero rate or depth,
a wrong block alignment, a chunk longer than the RIFF are `Malformed`; a malformed `smpl` or `cue` chunk is dropped and reported (`smpl_ignored`,
`cue_ignored`), a chunk gone wrong after the audio ends the import there (`trailing_ignored`): never silent, never fatal to good audio. The caller owns
`dst` and treats it as garbage after a failure.

**WAV as a registered format** (`formats/wav_sample.h`, `FormatKind::Sample`, id `wav`, `.wav`; the registry had seven then, eight with OS-002d's settings file: one row in
`register_builtin_formats`). `wav::SampleReader` is a `FormatReader` that reads into `ReadParams::pcm` (the sample arena slot) and makes the sample
instrument `params.index` **after** the file has been read and checked (a failed or cancelled import leaves the project and the slot as they were):
name = the file's name without directory and extension (printable ASCII, 32 characters); sample path `samples/instrNN.wav` (NN = slot + 1, this OS's
name for it: nothing in the file can point outside the project); `sample_rate` = the file's rate; `start` / `end` 0 and 65535; the first loop as
positions on the instrument file's 0..65535 scale (`frame * 65535 / frames`, rounded) in `loop_start` / `loop_end` with the play mode that loops it the way the file says (forward 1,
backward 2, ping-pong 3 (the instrument's play mode); no loop: one-shot; a loop shorter than one position is dropped); the cue points as `slices` (a frame 0
is the start, not a cut; two points one position apart are one slice) and `slice_count`, without changing the play mode. The root note of `smpl` is read
but not stored (no field; open question). The rate is the new model field: **`Instrument::sample_rate`** (u32, 0 = not recorded = 44,100 Hz, the rate of an OG `.pti`'s PCM),
`sizeof(Instrument)` 700 -> 704, `kModelArenaBytes` 1,904,856 -> 1,905,112. Native format: **INST chunk version 1.1** appends it after the module
blob (a 1.0 chunk reads as rate 0; a reader that only knows 1.0 skips the four bytes and reports `newer_minor`; the rule is per chunk now, 1 for INST and
0 for the others), `docs/formats/o3-project.md`, `tools/o3project.py` and the Python round trip follow.

MK66 size (`-Os`, text, against the build with OS-004b and OS-003d): `model` 4,550 -> 11,342 B (journal 1.7 KB, Editor 5.1 KB), `core` 6,326 -> 6,836
(job runner, `upsert`), `app` 18,743 -> 22,870 (history, job and import actions, `ImportJob`, `PeakJob`, the progress strip), `formats` 24,266 ->
25,800 (the WAV reader and its registration), `formats_wav` 4,363 (new); no data, no bss. Working memory on the MK66: `Editor` 24 B, `UndoJournal` 88 B
plus its region, `Session` 624 B (the macro is 512), `wav::Importer` 1,072 B, `wav::SampleReader` 1,208 B (inside `BuiltinFormats`, now 30.4 KB), `ImportJob`
60 B, `PeakJob` 80 B, `PeakCache` 172 B.

Tests (host with ASan/UBSan, and QEMU `qemu_jobs`, `qemu_wav`, `qemu_wav_sample`, `qemu_model_undo`, `qemu_mk66`): `undo_test.cpp` (every mutator undone and redone
exactly, groups, redo discard, ring overflow and wrap, oversize units, stale and damaged journals, the capacity arithmetic, and the
property test: random edits (valid and invalid), groups, undo, redo, tampering, over rings of 256 B to 384 KiB, each step against full
project snapshots, `O3OS_FUZZ_SCALE=<n>` for more, `O3OS_UNDO_REPORT=1` prints the totals), `job_test.cpp`, `wav_test.cpp` (variants,
truncation at every offset, damage fuzz, the stereo mix), `wav_sample_test.cpp` (the registered format and the instrument it makes), `history_app_test.cpp`
(F8 on Pattern and Song, the dispatcher, macros), `import_job_test.cpp` (a synthetic WAV imported through the simulator's runner with its progress strip
and then its peaks, cancel, failure, the overlay holding the strip back, an import that ends the undo history, a native project loaded as a job),
`native_format_test.cpp` (INST 1.0 / 1.1 / 1.2) and `mk66_undo_region_test.cpp` (the journal's region is the MK66 memory map's: it sees both, which the layers cannot; it also runs in `qemu_mk66`).
`tools/check_layers.py` also fails any file but `dispatch()` and the edit actions that includes `app/src/session_access.h`, the key to the document's editor.

## The project model and the native format (OS-004a, ADR 0003/0004/0007)

`model/` holds a whole project with no idea of screens, files or devices; `formats/native` reads and writes it as
an IFF-style chunked file. The byte-level specification, limits and a worked example are in
`docs/formats/o3-project.md`; `tools/o3project.py` is a stdlib-Python reference implementation of the same bytes.

- **Model** (`o3os::model`, `model/project.h`): name, tempo (BPM x 100), swing, default pattern length, 16 track
  slots (12 active; Audio / MIDI / External audio, name, mute / solo, volume, pan, output bus), master levels, a
  module table (name, ABI major.minor), the song (up to 255 pattern ids), a pool of patterns (16 tracks x 128 steps
  of the 7-byte `Step` with a length per track, ids 1..255, at most 128 at once), 64 instrument slots (sample with the OG sampler
  parameters, MIDI, or module with an opaque blob) and a bounded pool of unknown chunks. Reads are free; writes
  go through the `Project` mutators (`set_step`, `set_order`, `create_pattern`, `set_instrument`...), which check,
  change and bump generation counters (`generation()`, `song_generation()`, `pattern_generation(id)`...): the
  choke point OS-002c will route undo through. All limits are in `model/limits.h`.
- **Storage**: no heap. One `ModelArena`, a typed object the platform provides (a static on the host; SDRAM on the
  MK66). `kModelArenaBytes` is **1,933,760 B** worst case (1.84 MiB, 11.5 % of the 16 MiB SDRAM; 128 patterns
  1,841,152 + 64 instruments 46,080 (720 B each since OS-STEREO step 2) + unknown pool 16,384 + 2,520 + 13,248 for the parameters' generations and the settings block,
  OS-002d, + 1,144 for the project-level state, OS-004d, + 344 for the session state, OS-PERSIST2, + 2,304 for the nine source-page parameter slots of the 64 instruments, OS-INSTUI S3, + 6,144 for the 24 MOD-page slots, OS-INSTUI S6, + 3,072 for the twelve MIDI / VEL> slots, OS-INSTUI S8b + S9), identical on host and MK66 (`static_assert`).
  Pattern ids go to 255 but only 128 patterns exist at once; a file that needs more is refused ("limit exceeded").
- **Format** (`o3os::formats::native`, `formats/native.h`): `FORM` + `O3PJ`, then `PHDR`, `TRAK`, `MODR`, `SONG`,
  `PATT` (one per pattern), `INST` (one per instrument), unknown chunks, `CRC `; big-endian, chunks padded to even,
  every payload starts with a u16 version. `Reader` and `Writer` work over `core::ByteSource` / `ByteSink`
  (`core/byte_io.h`), no file I/O, no heap, and are resumable: `begin()`, then `step(byte_budget)` until `Done` or
  `Failed`; `report()` gives a typed error (`Truncated`, `BadMagic`, `BadCrc`, `NewerMajor`, `LimitExceeded`,
  `Malformed`...), its detail, offset and chunk. A failed load leaves a valid empty project. Unknown chunks are kept
  (16 chunks / 16 KiB) and re-emitted; overflow is counted (`Report::unknown_dropped`), never fatal.
- **Tests**: `model_test.cpp`, `native_format_test.cpp` (a maximal 1.8 MB project byte-exact, every error path,
  truncation at every offset, hostile lengths, odd sources and sinks), `native_fuzz_test.cpp` (deterministic
  mutations under ASan/UBSan; `O3OS_FUZZ_SCALE=<n>` for longer runs), and ctest `project_python` (C++ writes, Python
  reads and rewrites, C++ reads again: bytes and a text dump must match).
- **MK66 size** (`-Os`, text / data / bss): `model` 4,550 / 0 / 0 B, `formats` 9,177 / 0 / 0 B. A `Reader` is 2,728 B and a
  `Writer` 1,396 B of working memory, wherever the caller puts them.

## Parameters, ParamChange, the generated edit page and MIDI Learn (OS-002d, ADR 0009 rev 1)

Every automatable value is a **parameter**: a `ParamId` (kind, index, field) and a constexpr **descriptor** (range, default, unit and display, curve, steps, flags, page). The tables
are the one place a parameter is described (`model/params.cpp`: 49 per instrument, 5 per track, 8 master, 3 project settings, 7 device settings; the defaults of an instrument are this OS's own); the page, MIDI Learn, the settings files and
the undo journal read them, so a new parameter is one row. The byte-level specification, the tables of the settings, the Learn rules and the file formats are
`docs/formats/o3-settings.md`; the scope is `docs/specs/OS-002-extension-architecture.md` "002d".

- **`core/param.h`**: `ParamId`, `ParamChange {id, value, ramp}` and `ParamRing` (the SPSC ring to the audio side, 256 entries, at most 64 drained a block; it replaced OS-005a's placeholder
  event type, the engine drains this one), `ParamDesc` and the maths: `param_norm` / `param_denorm` through the curve (linear, log, exp, table of knots), `param_step`
  (encoder detents; coarse is a multiple), `param_format` and `param_parse` (whole numbers, the pan from the centre, a percentage of the range, seconds with three decimals for envelope
  times, enumerated names: only the displays a row uses; a Hz, dB or note display is one more case when a row needs it). `core/fast_math.h` is the libm-free `exp2_fast` / `log2_fast` that began in `ui/plot` and moved down for the log and exp curves.
- **`core/learn.h`**: `LearnBinding` (8 bytes), the rules (`learn_plan`: one controller moves one parameter in a store), the pickup / jump / relative logic (`learn_absolute`,
  `learn_relative_delta`), two stores by scope: the device's (Track, Master, Device parameters) and the project's (Instrument, Project): maintainer decision 8. Default mode **pickup**. The state between messages (`LearnRuntime`, 8 bytes, not persisted) is valid for one parameter at one generation: a change by anyone else, another instrument under a follow binding, or a loaded project (`Session::reset_history`) makes the knob catch the parameter again.
- **`model/params.h`, `settings.h`**: the tables and the storage access; `Settings`, the block of one scope (values, a generation each, the Learn store); `Project::get_param` / `set_param` /
  `param_generation` (a counter per parameter, advanced by that parameter's change alone, by whole-object mutators for the parameters they changed, by nothing else) and `Editor::set_param` /
  `set_learn_slot` (journalled: 40 and 48 bytes), with `applied()`, the parameters the last undo or redo set by value.
- **Persistence**: `SETS` and `LRNB` chunks in the native format (after the instruments; written only when something is not its default, so a project that uses neither is the file it was) and
  the device settings file `O3ST` (`formats/settings.h`: a buffer in, a buffer out, at most 1 KiB, the defaults after any error; also the registered format `settings`, `.o3st`, whose strategies read into `ReadParams::settings` and write from `WriteParams::settings`). Reading is tolerant: an entry this version cannot use is
  skipped and counted, what a file lacks is the default. The Python reference reads and writes the project's two chunks (`project_python` compares 29 damaged or foreign ones with C++).
- **The page** (`app/screens/params_screen.h`, the registry's `INSTRUMENT`, the **Params** button): the parameters of the selected instrument (a pad picks it) in four pages, F1..F4 or the button
  again: VOICE, FILTER, ENV (Left / Right choose one of the six envelopes), SENDS. A row is the descriptor's name, value, unit and a bar on its curve, with the controller bound to it if any;
  a card on the right says everything the descriptor knows about the row under the cursor; the FILTER page draws the filter's response (a 2-pole state variable filter; the cutoff law
  `20 Hz x 2^(9.54 c)` and the Q `0.7 + resonance` are this OS's, and whether they match the official firmware's filter is an open research question, Q) and the ENV page the envelope's shape, as `ui::plot` curves. Up / Down the row (the Cursor, whose `count` now takes the view: the pages have
  different lengths), the encoder `param.edit`, Shift + encoder `param.coarse`, Delete `param.reset`, F8 / Shift + F8 undo / redo. Without a project it shows the defaults and says so.
- **Actions** (`app/src/param_actions.cpp`; the flag `kActionUndoableIfOpen`: undoable when a document is open, not Disabled without one, so a device setting needs none): `param.edit`,
  `param.coarse`, `param.reset`, `param.group`, `learn.clear`, `learn.mode` and `midi.cc` (argument `channel << 16 | cc << 8 | value`; with the Learn overlay up it binds, otherwise it moves what
  is bound). A changed parameter is pushed to the ring attached with `App::attach_param_ring`, also by undo and redo; a full ring is counted (`Session::param_drops`), a unit that replaced a
  whole instrument is counted (`param_resyncs`) for the engine to re-read. Scenario words: `document [bindings]` (the demo project, its undo journal, a fake audio side) and `midicc`.
- **Tests**: `param_test` (ids, every table row valid and dense, defaults equal to `init_instrument`, the curves against libm, steps, display text and parsing back for every detent of every
  row, the settings block, the Learn rules, the ring under load), `settings_format_test` (the default file as a literal computed from the spec, round trips, default-fill, every refusal, every
  flipped bit, damage with a good CRC), `param_model_test` (the Project's parameters, a generation per parameter, undo and redo, a property test against snapshots, the chunks in the project
  file), `params_app_test` (the page driven by its keys, edits, undo, Learn in every mode), scenario `params`, the gallery's `params_*` states (VOICE, FILTER, ENV on two envelopes, SENDS,
  Learn over the page, no project: three themes each; `params_worst` sweeps 648 variants of page, row, content, controller tag and overlay: 793 of 1,792 words, 953 in the VERTEX2F fallback), `project_python`, and the QEMU suites
  `qemu_params` and `qemu_params_model`.

## Pattern editing (OS-009 P1a.1, `docs/specs/OS-009-pattern-editing.md`)

The Pattern screen reads and writes the attached document (`PatternSource`: `model.project`'s pattern `view.edit.pattern`, else the OS-001b demo, which is read-only: every
edit is Disabled and says "NO PROJECT" through the flag `kActionSaysNoProject`). Every edit is one `Editor` group, so one dispatch is one undo unit (the journal of OS-002c, unchanged); the
platform images attach a fresh document at boot (`mk66/app_main.cpp`: a `ModelArena` at `mem::kProjectBase`, pattern 1, one song position, the journal at `kUndoBase`).

- **Modes and fields.** Four modes (`PatternMode`, second level of the binding chain), two of them in use: Browse (the start: pads preview, the encoder scrolls, nothing edits) and Edit (Rec arms it; while the pattern plays it records); Follow (a placeholder until there was a transport) is retired (OS-004d), and Select (the block mark, a mode whose arrows extended the block) went with OS-LIVEREC. The **field** under the cursor is a context of its own (`Screen::field_context`: `pattern.note`, `.instr`, `.vol`, `.fx`, between the mode and the screen),
  so a pad or the encoder is bound per field, and moving between fields does not disturb a held button.
- **The rules** (`model/step_edit.h`, pure functions on a `model::Step`, no document): `note_set` / `note_delta` (Off, Cut and Fade step into the range and out of it), `instrument_set` / `instrument_delta` /
  `instrument_walk` (the order: MIDI channels first), `volume_*`, `fx_value_*` / `fx_pad` (the pad 0..47 scaled linearly onto the type's min..max in integers) / `fx_type_*` (through the picker order of the FX table), `clear` (by element mask; clearing never leaves an
  instrument behind). A real note made from Off / Cut / Fade gets instrument 1. The single-cell rules and the multi-step rules differ (`Rule`), and both are kept. The last-used-FX rule is kept: with no FX used yet nothing can be created by the encoder or a pad
  (only `step.fx_type` can, bound in P1a.2). `pattern_edit_rules_test` checks every rule exhaustively and over random sequences against the reference vectors of `tests/data/pattern_edit_vectors.h` (the output of our own reference model, maintainer 2026-10-18; whether the official firmware agrees is an open research question).
- **Tables.** `core/scales.h`: the 40 scales (degree lists) and names and `is_in_scale`; four masks (11, 17, 23, 36) are corrected to the standard definitions of their names (`scale_corrected`, OS-PTOOLS PAR-073: new Community OS behaviour). `model/fx_table.h`: the 43 FX rows (support, range, start) and the 34-entry picker order.
 Both tables are this OS's own (the scales as degree lists, the FX rows under their own names); their content agrees with the reference vectors of `tests/data/pattern_edit_vectors.h`, the output of our own reference model (new Community OS behaviour, with that file as its evidence; the official firmware's is an open research question), except the four corrected scale masks above. The **pad board** (`app/pad_board.h`) is this OS's pad layout (whether it matches the official firmware's is an open research question)
  (scale, offset, root; default scale 0, offset 12, root 36; at most 120), tested against the reference vectors over 41 x 15 x 63 boards.
- **Actions** (`app/src/step_actions.cpp`, `chooser_actions.cpp`, `pattern_actions.cpp`; registry names): `step.note`, `.note_pad`, `.note_delta`, `.note_octave`, `step.instrument`, `.instrument_delta`, `step.vol`, `.vol_pad`, `.vol_delta`, `step.fx_type`,
  `.fx_type_delta`, `.fx_value`, `.fx_value_delta`, `.fx_pad`, `step.clear`, `step.insert`, `step.pull`; `select.extend`, `.column_all`, `.clear`; `track.mute`, `track.solo`; `chooser.toggle`, `.hold`, `.release`, `.delta`, `.defined`, `.pad`;
  `pad.preview` (a `PreviewSink` hook; the engine side is OS-005), `pattern.edit_toggle`, `pattern.rec_free` (Shift + Rec), `rec.note_off` (the release of a recorded note), `pattern.preview` (F5), `pattern.invert` (F6), `pattern.fill` (no key since OS-QJUMP) and `pattern.export` (no key: a label until OS-WAVEXP). An edit past the end of its track is refused ("PAST TRACK END"); a track with no pattern yet gets one on its first edit.
- **Keys** (Pattern context unless noted; hardware numbering). Pads and the encoder by field in Edit; F1-F3 tap toggles a chooser and a hold opens it until the release; Shift + arrows extend the block in every mode (`select.extend`; not while the cursor rides the playhead);
  Insert / Delete (clear, always advances) / Shift + Delete (backspace); Shift + Insert jumps to the start; Shift + pad 0-11 mute and 12-23 solo the track (the `M` / `S` marks); Shift + encoder in the Note field is an octave; Fx held + encoder changes the FX type
  (a hold-encoder gesture: `kWantHoldEncoder`, so Fx now acts at its release, not after 40 ms, because the turn must be able to consume it). Browse (and Follow, until OS-004d retired it) bound the same
  gesture to `nav.scroll`, so there a held Fx and the encoder scroll (the type edit is Edit's) and Fx taps at its release on every field.
- **MUTE / SOLO tag** (OS-MUTEVIS, new Community OS behaviour; `PatternView::tag_slot`): a muted track says MUTE and a soloed one SOLO inside its own column, in the top slot of the grid: a bright chip with the letters knocked out in the pane's colour (the shape of the header's M / S chip) over the dimmed rows, fixed to the slot (not to a row), so it stays while the pattern scrolls under it. The header keeps the track name and the small M / S chip. A track whose quantised mute (MUTE MODE Bar / Pattern) is pending blinks its tag (`App::poll_mute_blink`, ~2 Hz, a redraw each half period only while one waits). The tag never hides the cursor's row or the play bar: while either is in the top slot it takes the bottom one (or the next free); since OS-PATCENTER the cursor is always on the centre slot, so it is the play bar that moves it, and at the first and last step the tag sits over a blank slot.
- **Selection** (`app/pattern_edit.h`): a rectangle of steps (rows by tracks, one column or all), the cursor's row excluded from the highlight; edits apply to the whole block as one unit (per-track spans written through `set_steps`), and Shift's release ends the extension.
  The cursor keys collapse it in every mode. While the cursor rides the playhead (Browse playing, Edit following) it cannot be made (Shift + the arrows do nothing, and say nothing: the chip shows the state) and the playhead drops it (PAR-093, documented in the release notes, not verified); free Edit (Rec) can.
- **Live recording** (OS-LIVEREC, the maintainer's design, new Community OS behaviour; `app/rec_place.h` has the rules, `src/rec_actions.cpp` the actions; it replaced the earlier placement of OS-FU2 and the track arming of OS-009 P1d, which is gone: no armed tracks, no `R` mark). Edit while the pattern plays records the notes of the pads (Note column) and of the MIDI keyboard.
  **Track**: always the cursor's, no search for an empty step; the note (and the selected instrument, the one the strip shows) goes over what the step holds, and the volume and FX of the step stay. **The take is one undo unit** (the whole take): one undo group is open from the moment the Pattern screen records (Edit while it plays, following or free) to the moment it stops (Rec off, Stop, another screen) and holds every note and OFF; one Undo takes the whole take, one Redo gives it back; a take with nothing recorded leaves no entry; an edit of another kind during the take (a manual step edit, a mute, a controller), Undo and a macro close it first and are units of their own, and the next note opens a new take (`rec_take_sync`, `kActionRecords`; the info line counts the take as an undo level once it holds a note that changed something). A take that has grown to half the undo journal is closed and a new one begun, with the notice "UNDO: LONG TAKE IS SPLIT" (once for a recording): one longer than the whole ring would be lost, and the history before it with it. The App's destructor closes an open take (the display recovery rebuilds the App on the surviving editor). The step after the pattern's last is not wrapped to when a song or a queued pattern leaves the pattern. Device setting **REC POLY** (Config, PATTERN page): Off (default) is that track only (a second note held on it overwrites the same step); Spill sends the notes held together on to the next tracks, one a voice (past the last track: "NO FREE TRACK TO SPILL TO").
  **Step**: the playhead's (a track shorter than the pattern: the step modulo its length), rounded by device setting **REC QUANTIZE**: Nearest step (default: the next step from the microstep 24 of 48 on; the sequencer publishes the one it is in in `audio::PlayPosition`; in a song the pattern's last step does not wrap to row 0, the note stays there) or Playing step (the step the playhead is on). **Free** (Play + Rec; OS-RECSWAP reversed the OS-LIVEREC keys): the cursor and the view stay, you move freely, notes still go to the playhead's step on the cursor's track, Left / Right change the recording track; Rec stopped does the same, for the next Play. **Follow** (Play + Shift + Rec): the cursor rides the playhead and the view keeps the playing row centred (OS-CENTER); in Edit Shift + Rec toggles the two.
  **Release**: an OFF at the playhead on the track that holds the note, or the step after the note when it is released within the step it was played in; never over a step that holds a note (that note ends it). Main loop only: the position is read at the top of every pass and again when the keyboard is polled; nothing runs in the audio context. Tests: `rec_test`, `settings_format_test`, `config_screen_test`, scenario `pattern_record` (the simulator's `playhead <step> [<ustep>]`).
- **Choosers** (overlay ids 4-6, `ChooserOverlay`: a strip over the info line): pattern (1-based, created on first use), length (1-128, with the defined values {1,2,4,8,16,32,48,64,80,96,128} on Left / Right and the pad scaled linearly onto 3..191 on a pad) and step jump (0,1,2,4,8,16,32).
  Chooser contexts capture the input, so the pad rows and arrows mean something else while one is open.
- **Display**: the block, the header marks, the info line (`scale <name> <root>   track n`, the mode chip, `undo n`), the Shift layer and the chooser strip are described in `docs/ui/style-guide.md` section 5c and add no colour pair. The worst states in
  all three themes are 1,281 words (translate) / 1,616 (VERTEX2F fallback) for the pattern sweep of 909 variants and 1,338 / 1,703 for a chooser over it, limit 1,792 (`sim/README.md`).
- **Note tiles** (133 cells, composed at boot, no flash): the 128 notes by the reference table's names ("C-2" for its "C2" in a fixed width, "C10".."G10" for notes 121..128), `---`, `OFF`, `CUT`, `FAD`, `???`; an off, cut or fade step shows
  no instrument. Cells past the 128th use a second bitmap handle (VERTEX2II's cell field is 7 bits) that the renderer binds in the first frame that draws one (4 words, nothing otherwise). `pattern_edit_rules_test` compares every name
  with the reference table.
- **Known limits and open items.** the sharps of the tenth octave are lowercase ("c10" for C#10: a tile is three characters and the reference name has four, Q-P1-19); MIDI instruments 49..64 show as decimal pairs until P1e.1 gives them `M01..M16`; the four unverified scales; the per-FX "Pad Range" table (Q-P1-9); `step.fx_type` has no key yet (P1a.2); the engine side of preview.
- **Size and tests.** The MK66 image grew from 125,672 B to 144,876 B (+19,204 B; sha256 `d4c6ae57...1ed01`, identical from two build directories), `.bss` 51,016 to 51,344 B (+328 B), `.data` unchanged at 168 B; the document and its undo journal live in SDRAM, not in `.bss`. Tests: `pattern_edit_rules_test` (the rules against the reference vectors, exhaustive and random), `pad_board_test`, `pattern_edit_app_test` (the actions through the registry and the bindings, an undo property test against full snapshots, journal bytes, selection, choosers, mute / solo, every disabled path), the gesture and binding-resolver cases of `kWantHoldEncoder`, scenario `pattern_edit` (27 snaps), the gallery states, the note names against the reference vectors' (`pattern_edit_rules_test`), the second note handle (`ui_test`, `pattern_view_test`, `assets_test`), and Fx held in Browse and Follow; the QEMU suite `qemu_pattern_rules` runs the first two on the Cortex-M4F. `ci.sh host`: 58 ctest tests (56 before), 1,343 doctest cases; `qemu`: 25; `mk66`: 11; all with 0 warnings.

## The USB library (OS-006a, ADR 0012 S1 + S2)

`usb/` is everything of the USB device that does not touch hardware; the MK66 USB0 driver (OS-006b) will only move
bytes and call it. `core` only, no heap, no `double`, no 64-bit division at run time; about 4.7 KB of flash for the
device-side code (`descriptors`, `uac2`, `rate`, `kernels`, `stream`; the validator is a test and bring-up tool).

- **Descriptors** (`descriptors.h`). `Config{mode, speed, profile, serial}` -> device, configuration and string
  descriptors built into a caller buffer at boot. STANDARD (CDC `diag` + MIDI) is 175 bytes, AUDIO (+ UAC2: stems IN,
  USB RETURN OUT with an explicit feedback endpoint) 384 bytes; Full speed, High as a parameter (packet sizes and
  intervals only); profiles `P2 P8 P10 P11 P4S` (2, 8, 10, 11, 10 channels). Endpoints 1..5 are fixed; interfaces
  are 0 CDC comm, 1 CDC data, then STANDARD 2,3 MIDI, AUDIO 2 UAC2 control, 3 stems IN, 4 return OUT, 5,6 MIDI (the UAC2
  function comes first because Linux resolves `bTerminalLink` only in the first AudioControl interface; `usb_types.h`, `layout::`). The IN stream's `wMaxPacketSize` is channels x 2 B x 45 (990 at 11
  channels, the largest that fits 1,023). No Feature Unit on purpose. `identity.h` holds the VID/PID (the official firmware's pair, kept by the maintainer's decision (2026-10-16, confirmed 2026-10-18), publication included; the OS is told apart by `bcdDevice` and the strings), the strings and the
  serial placeholder (the serial stays PROVISIONAL, maintainer item).
- **Validator** (`validate.h`) parses the bytes, not the builder's inputs; the tests run it on every configuration
  and on dozens of hand-broken ones. Golden bytes: `tests/usb_golden.h`.
- **Class requests** (`uac2.h`): clock source GET CUR / GET RANGE (one discrete 44,100 Hz), pure functions on a
  `SetupPacket`; AS alternate-setting state.
- **Rate and feedback** (`rate.h`). `RateEstimator` is a second-order tracking loop on the SOF phase (CYCCNT per SOF),
  not the 32 ms average of the ADR text: with +/-150 us of ISR jitter a 32 ms average has a noise floor of thousands of
  ppm, the loop is under 50 ppm after about 4 s and at 22.5 ppm worst case in the 1.5 million simulated SOFs. **The reference clock is one constant**, `kRefCyclesPerSample`
  (an alias of `core::kRefCyclesPerSample`, `core/audio_clock.h`: 1800000/441 by default, 4080/1 with `-DO3OS_RATE_44100=OFF`, D-OS-8);
  nominal feedback 0x0B0788 follows from it (0x0B0667 at the retuned clock). FS feedback is Q10.14 in
  3 bytes, HS Q16.16 in 4.
- **Streams** (`stream.h`). `InPacketiser` (always 44 or 45 frames, 5 or 6 at HS, plus a guard on the smoothed ring
  fill), `InStream` (priming, silence, underrun re-prime), `OutStream` / `ReturnReader` and `FeedbackControl` (a dead-zone
  trim of the return ring fill). Rings are `OverwriteRing` (`ring.h`): the producer never waits or fails and the
  consumer skips and counts what it lost (that is why it is not `core::SpscRing`, whose producer cannot drop the
  oldest). Engine contract: stem blocks are 128 frames, 4-byte aligned planes; a block must stay valid for 4 further
  writes (ring of 3 + the block being read + the one in production, 5 per slot). Counters for `diag usb`: `counters.h`.
- **Kernels** (`kernels.h`): interleave, stereo deinterleave and the slot-mask saturating sum, each a scalar reference
  plus a word-at-a-time variant (PKHBT/PKHTB/QADD16 on the M4, portable on the host), bit-identical.

Tests: `tests/usb_*_test.cpp` and the shared harness `tests/usb_sim_harness.h`. The property test (`usb_sim_test.cpp`,
`usb sim:`) runs the real classes against a simulated host at +/-500 ppm with +/-150 us of jitter for 1.5 million SOFs
(about 15 s under ASan), and again at the D-OS-8 clock (1800000 / 441 cycles per sample); building the whole tree with
`-DO3OS_RATE_44100=OFF` (the pre-1.0 clock; host, QEMU and MK66) compiles and passes everything. The same files except the threaded ring case and the long simulation also
run on the Cortex-M4F as `qemu_usb` (the PKHBT/PKHTB/QADD16 paths, a kernel run with `CCR.UNALIGN_TRP` set so any
unaligned word access faults, and a 40,000-SOF closed-loop run at both clocks, about 15 s of QEMU). Ring cases are
labelled for TSan (`usb_ring_tsan`).

### The device logic and the `diag` console (OS-006b, ADR 0012 S4 first part, ADR 0011 item 1)

The driver for the MK66's USB0 (`platform/mk66/usb`, see its README) moves bytes; the rules it follows are here in `usb/` and tested without hardware, against
a model of the controller (`tests/usb_fake_controller.h`: the BDT with OWN/DTS/toggle checking and even/odd ping-pong, a 4-deep STAT FIFO, the freeze after SETUP, NAK and STALL
answers) and a host that keeps real toggles (`Host`, `tests/usb_rig.h`):

- `control.h`: endpoint 0 and the standard device state as a pure state machine (SETUP in, a step out: send this packet with this DATA PID, or stall, plus at most one effect for the driver:
  the address after the status stage, a configuration, a halt). Every standard request, the CDC ACM requests, the UAC2 clock requests, the zero-length-packet rule (the AUDIO configuration is 6 x 64
  bytes), STALL for everything else.
- `endpoint.h`: the buffer-descriptor word, `PingPong` (the even/odd and DATA0/DATA1 bookkeeping, including what a configuration event and ClearFeature(HALT) do to it) and the endpoint table of a mode,
  checked against the descriptors the builder writes.
- `device.h`: `UsbDevice<Hw>`, the whole USB interrupt over a register policy (bus reset, tokens, SOF, suspend, errors), bounded at 16 passes, with the CDC and MIDI streams, drop-oldest rings and
  counters (`diag usb`). `cdc.h`: line coding, `CdcTxWriter` (63-byte chunks: no packet is ever full, so no zero-length packet), USB-MIDI event packets.
- `diag_shell.h` (line editor with backspace, Ctrl-U/C, escape sequences and a bounded length; a dispatcher whose table is keyed by `core::action_id("diag.<name>")`, so a command is an action, not a second
  registry; streams: any-key-stops ones and pagers that keep the terminal; `feed` takes input only while the output has room for a reply), `diag_pump.h` (the main loop's side: receive ring to shell with back-pressure),
  `diag_log.h` (a lock-free multi-writer log ring for boot code and ISRs). The receive descriptors are held back (the host is NAKed) while the receive ring is nearly full, so a paste loses nothing.

The tests (`usb_endpoint_`, `usb_control_`, `usb_device_`, `usb_cdc_`, `usb_diag_shell_`, `usb_diag_log_test.cpp`, plus the host-only `usb_diag_log_thread_test.cpp`) run on the host under ASan/UBSan and, all but the
threaded one, on the Cortex-M4F as `qemu_usb_device` (where `sizeof(BdtEntry) == 8` is checked). The MK66 side: `platform/mk66/tests/mk66_usb_soc_test.cpp` (register values, the clock proof, the serial number) and
`mk66_usb_text_test.cpp` (golden text of the platform's diag commands and the command table), `tests/mk66_usb_console_test.cpp` (the console's back-pressure end to end: a pasted burst of `log 48` x3, a host that stops reading, a flood of empty lines).

## The OG files and the format registry (OS-004b, ADR 0006/0009)

`formats/` also reads and writes the Tracker OG's own files, and every format meets in a registry. The byte-level
mapping (what is kept, normalised and dropped, the defaults Export writes, the version policy, the hostile-input rules)
is `docs/formats/og.md`; the layouts are in that file.

- **Strategy and registry** (`formats/format.h`, `builtin.h`): `FormatReader` / `FormatWriter` (`begin`, `step(budget)`,
  `report()`; virtual at format granularity), `FormatRegistry` (16 entries; find by id, by file name, by sniffed magic;
  `identify` lets the name decide and the bytes agree; `._` files are never matched), `register_builtin_formats()` for
  `native`, `og-project`, `og-pattern`, `og-pti`, `og-mti`, `og-pamd`. `Progress` is one type for all formats
  (`formats/progress.h`; `native::Progress` is an alias of it). The strategy objects live in a `BuiltinFormats` the
  caller owns (29 KB on the MK66).
- **OG readers** (`formats/og.h`, `ProjectReader`, `PatternReader`, `InstrumentReader`, `PamdReader`): project.mt v2-v7,
  v11-v14, v16, v17 (official 1.9.2, OS-004c: the `observed_only` class, no file; open research question, unverified); patterns v1-v5 of 8 and 12 tracks; `.mti` v1-v9; `.pti` v4-v9; PAMD v1. An import is one read per
  file of the OG folder, project first (it resets the project), names last; a file that fails leaves the project as it
  was. `FormatReport` says what a read kept (`kept_groups`), dropped (`dropped_groups`, named by
  `FormatDesc::group_name`), replaced because the model cannot hold it (`sanitised`) and how many PCM frames follow a
  `.pti` (`pcm_frames`). Newer or unobserved versions load as `unverified_version`, never guessed.
  `og::classify_file_name()` names the files of a folder. An OG step has no volume byte (this OS carries the volume as FX 18): the FX columns
  are kept verbatim and the model's volume column stays empty; per-track lengths map one to one.
- **Lossless OG round trip** (ADR 0006): the settings of an OG project that the model has no field for (delay, limiter,
  reverb, inputs, performance, the 1.3 and 1.7 blocks, MIDI velocity) are kept verbatim in an `OGPM` chunk of the
  project, and the PAMD file as read in an `OGPN` chunk, both in the model's pool of unknown chunks (so the native
  format and `o3project.py` carry them); Export writes them back, byte for byte. Session state, the pattern bitmask
  and uninitialised bytes are rebuilt or defaulted. Names given to patterns in the OS are not written to PAMD until the
  record <-> pattern mapping is confirmed on a device (a hardware gate, counted as dropped).
- **Export -> OG** (`ProjectWriter` v16, or v17 when `WriteParams::target` is `og::kTargetOg192` (official 1.9.2; the default
  `kTargetOg190` is v16; the byte at 0x80C that a v17 project carries goes into OGPM 1.1), `PatternWriter` 12 tracks v5, `InstrumentWriter` `.pti` v9 with the PCM from a
  `ByteSource`, `PamdWriter`): each writer's `export_report().drops` (`ExportDrops`) counts what had no home (tracks
  13-16, engine FX, module instruments, volume columns that found no free FX column, track mute / volume / type that
  OG tracks 9-12 cannot hold, OS pattern names...) and lists the groups written as defaults; the app adds them.
- **Sizes** (MK66, `-Os`): `formats` 24,266 B (was 9,177: the OG codec, the settings chunks and the registry cost
  15,089 B), no data, no bss. Working memory of a strategy on the MK66: project reader 2,488 B, pattern reader 1,768,
  instrument reader 1,456, PAMD reader 12,904 (it stages the whole file, so a bad one changes nothing), project writer
  2,716, pattern writer 912, instrument writer 1,064, PAMD writer 164, registry 516; a `BuiltinFormats` is 29,148 B.
  OS-004c (project v17, the Export target): `formats` +376 B of flash (43,952 -> 44,328 B at that time), no data or bss; the
  project writer's image holds the 2,324 bytes of v17 (+4 B: 2,720 -> 2,724 B).
- **Tests**: `og_pattern_test.cpp`, `og_instrument_test.cpp`, `og_project_test.cpp`, `og_pamd_test.cpp` (and file
  names), `og_settings_test.cpp` (the OGPM chunk: non-default values in every group, through the native format, and
  its validation), `og_v17_test.cpp` (project v17: layout, the carried byte, OGPM 1.1, the Export target), `og_roundtrip_test.cpp` (byte-identical v16 / v17 / v5 / v9 folders, and the proof of the normalised
  bytes), `og_hostile_test.cpp` (deterministic mutations under ASan/UBSan, `O3OS_FUZZ_SCALE=<n>`),
  `format_registry_test.cpp`, ctest `project_python` (the OGPM / OGPN chunks against `o3project.py`), and
  `og_fixture_survey_test.cpp`, which imports, exports and imports again every project of the maintainer's card when
  `fixtures/sd` is found (`O3_FIXTURES_DIR` to point elsewhere) and skips otherwise; it prints counts only. The synthetic
  files come from `tests/support/og_fixture.h`, written from the byte maps and sharing no code with the readers.

Style of this tree: types `PascalCase`, functions `snake_case`, constants `kPascalCase`, members `trailing_`.

## OctaMED / MED files: reader, timing, flow (OS-008a, docs/formats/med.md slices M0 and M1)

`formats/med/` (headers in `formats/med/include/o3os/formats/med/`, built into the `formats` library) reads MMD0 to MMD3
modules and answers the two questions the importer (slice M2) needs before it touches the model. Nothing is registered in
the format registry yet: the reader cannot produce a project until M2 maps cells and instruments, and `FormatRegistry::add`
refuses a descriptor with neither reader nor writer.

- **Reader** (`med/mmd.h`, `Reader`): a validator and a pointer-graph reader over a random-access `RandomSource`
  (`size()`, `read_at(offset, dst, n)`; `SpanSource` for memory). `open()` reads the 52-byte header, the song record, the
  expansion prefix and the section table (0.7 KB of state on the MK66); everything else is read on demand a line at a time:
  `block()` and `read_line()` (3-byte MMD0 and 4-byte MMD1+ cells, 16-bit track and line counts, extra command pages, the
  SoundStudio command-extension byte, block names), `instrument()` (sample, five multi-octave types, EXTSAMPLE, 16-bit,
  stereo, synth, hybrid, InstrExt and the instrument name by stride, the loop in frames clamped into the sample, the PCM the
  sample arena would need) with `synth_waveform()`, `play_entry()` (the flattened play list of MMD0/1, or of the sections of
  MMD2+ with their Stop and Jump commands), `track_volume()` / `track_pan()`. `validate()` walks everything the song reaches
  once and returns the first problem or a `Summary` (referenced blocks, unsupported and oversize instruments by number, PCM
  total, chained songs, the data that is present but ignored). Every limit of docs/formats/med.md 2.6 is in one `caps::`
  table; errors are typed (`Truncated`, `BadMagic`, `BadPointer`, `LimitExceeded`, `Malformed`, `Unsupported`,
  `SourceFailed`) with the offset of the first problem; pointers are checked in 64 bits, nothing recurses, nothing allocates.
  Not read, only noted: annotation, notation, MIDI dumps, MMDInfo, ARexx, MIDI 3x, echo and stereo mixing fields.
  `MMDC`, instrument types -3 and -4, delta and packed data are `Unsupported`.
- **Timing** (`med/tempo.h`): `tempo_to_bpm(model, tempo, spd)` gives BPM x 100 on the 192 PPQN grid in 64-bit integer
  arithmetic, clamped to the model's 4000..80000 with the clamp named: speed mode `1500 x CIA x tempo / (474,326 x SPD)` (PAL
  709,379 Hz by default, NTSC an option), SoundTracker-compatible tempos 1-10 `440,550 / (tempo x SPD)`, the 8-channel table
  (179 .. 99; it wins over BPM mode), BPM mode `150 x BPM x lines per beat / SPD`, and SoundStudio's fixed 157.86 for mixing
  songs below tempo 8. 33 / SPD 6 / PAL is 123.38 BPM, 125 BPM / 4 lines per beat / SPD 6 is 125.00.
- **Flow** (`med/flow.h`): `linearise()` simulates the song once and lists it as `PatternDef`s (a piece of a block: lines
  first..last, a replay flag, and the tempo and speed it is entered in when a line of it sets them) plus an order of at most 255
  entries over at most 128 patterns. It is a `FlowJob` (`begin`, `step(budget)` -> Running / Done / Failed, `status()`) that a
  main-loop job can drive in bounded steps; `linearise()` and `structural()` run it to the end. Bounds: 32,640 line visits
  (255 order entries of 128 lines is all a song can hold), 2^21 cells decoded (tracks x (1 + pages) per line, so a block of 64
  tracks and 16 pages is refused after 1,928 lines), and as many play-list steps as visits. Rules: 0F00 and 1D xx break to the
  next position, 0B xx jumps within the section being played, 16 loops per track (00 marks the start, up to 255 repeats), 1E
  repeats a line (up to 255) with notes cleared, 09 sets the speed, 0F tempo, 0FFE and a play-sequence Stop end the song, a
  play-sequence Jump continues at its target, entries that name nothing are skipped and counted, a return to a position and
  line entered before is the song's loop point (reported: the model cannot store one yet), blocks are cut at multiples of the
  largest multiple of the lines per beat that is <= 128. `structural()` is the fallback when that does not fit (patterns are
  blocks, the order is the play list, what it drops is counted); `FlowOptions::truncate` stops at a limit instead of failing.
  `FlowReport` counts everything per visit for the import report (docs/formats/med.md 3.6). `Cell::note_hi` carries bit 7 of an
  MMD1 note byte (OpenMPT's note cut) for the mapping to interpret.
- **Sizes** (MK66, `-Os`): `formats` 38,099 B (was 24,266: reader 9,943, flow 3,520, tempo 370), no data, no bss. State: a
  `Reader` is 668 B, a `LinearSong` 4,492 B (128 patterns, 255 order entries, the loop memo, the report), a `FlowJob` 916 B, a
  `Summary` 88 B, a `FlowReport` 128 B; stack frames (`-fstack-usage`): `open` 1,008 B (the song record is read onto the stack),
  `validate` 712, `linearise` and `structural` 952 (they hold a `FlowJob`), a tick's `scan_line` 456, `read_line` 328; about 2 KB
  at the deepest.
- **Tests**: `med_reader_test.cpp` (every field of every version on synthetic modules), `med_tempo_test.cpp` (the spec's
  worked examples, the table, every tempo x speed against the formula in double, the libxmp / OpenMPT divergence),
  `med_flow_test.cpp` (goldens for every rule, 16 20 and 1E 40 as well as the 255 that overflows the order, the cell
  count, the structural fallback, the limits, `FlowJob` at budgets 1 to 5,000 against the one-shot result, 1,500 random songs
  against the invariants), `med_hostile_test.cpp` (a file cut at every offset of each version, the vectors of med.md 2.6, a failing
  source, a sweep of every byte of the header and song record, deterministic mutation fuzzing under ASan/UBSan with
  `O3OS_FUZZ_SCALE=<n>`), `med_oversize_test.cpp` (host only, files too big for the target: the sample-arena budget on a real 11 MB sample, and the block built to be slow - 64 tracks, 9,999 lines, 16 shared pages - refused within the cell budget, by `linearise`, by a `FlowJob` in steps and by `structural`). The files come
  from `tests/support/mmd_fixture.h` (C++, written from the Rev 6 layouts) and from `tools/mmd_build.py` (stdlib Python, a
  second implementation of the same bytes): ctest `med_python` writes six modules there with the dump it expects of each and
  compares it with `tests/tools/o3med_tool dump`; `med_python_selftest` checks the generator's own structure.
  `o3med_tool survey FILE...` and `o3med_tool flow FILE` are for looking at a module by hand. No real module is committed.

## Build (from the repository root)

```sh
# host, with ASan + UBSan; unit tests and layer check
cmake -S . -B build-os-host -DO3OS_SANITIZE=ON && cmake --build build-os-host
ctest --test-dir build-os-host --output-on-failure

# MK66 cross build: the static libraries (size per library) and the skeleton image o3os.elf/.hex/.bin (OS-007b, section
# below) behind its static checks; finds .toolchains/ itself, no PATH change
cmake -S . -B build-os-mk66 -DO3OS_PLATFORM=mk66 -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/mk66-toolchain.cmake
cmake --build build-os-mk66 && ctest --test-dir build-os-mk66    # HEX validity, address range, vectors, flash configuration

# target tests: the unit suites cross-built for the Cortex-M4F and run on QEMU (OS-007a), see below
cmake -S . -B build-os-qemu -DO3OS_PLATFORM=qemu -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/mk66-toolchain.cmake
cmake --build build-os-qemu && ctest --test-dir build-os-qemu --output-on-failure

# run a scenario headless and look at the result (PNG per `snap` line), see sim/README.md
build-os-host/sim/o3sim --scenario sim/scenarios/pattern_basic.txt --out /tmp/o3sim-png
build-os-host/sim/o3sim --scenario sim/scenarios/pattern_basic.txt --out /tmp/o3sim-png --theme black   # start in a theme
build-os-host/sim/o3sim --gallery /tmp/o3sim-gallery    # every screen x state x theme, contact sheets, the word budget
cmake --build build-os-host --target update_goldens   # after a deliberate UI change; then view the PNGs (parallel; O3OS_GOLDEN_ONLY=<name> for one)

# host, ThreadSanitizer (exclusive with O3OS_SANITIZE); `ctest -L tsan` runs just the ring cases
cmake -S . -B build-os-tsan -DO3OS_TSAN=ON && cmake --build build-os-tsan && ctest --test-dir build-os-tsan
```

TSan on this kernel (Linux 6.8) needs address randomisation off or GCC 13's runtime aborts with
"unexpected memory mapping". The TSan ctest entries already run under `setarch <arch> -R`; by hand:
`setarch "$(uname -m)" -R build-os-tsan/tests/o3os_tests`.

## CI (OS-CI, `.github/workflows/ci.yml`, `docs/specs/OS-CI.md`)

`tools/ci.sh <host|qemu|mk66>` is the one place that holds the configure, build and test commands; the workflow only calls
it (a matrix of the three), so a person runs exactly what CI runs: `tools/ci.sh host` (ASan + UBSan), `qemu` (needs
`qemu-system-arm`), `mk66`. Each starts from a fresh `build-os-ci-<mode>/` (`O3OS_CI_KEEP=1` reuses it; `O3OS_CI_JOBS` sets the
parallelism) and exits non-zero on a configure or build failure, on **any warning** in the configure or build output, and on a
failing or empty ctest. The cross modes use the pinned **Arm GNU Toolchain 15.3.Rel1** in `.toolchains/`; `tools/ci.sh
toolchain` downloads it from Arm's package registry (docs/toolchain.md) and unpacks it only after its SHA-256 matches the one
written in the script; the workflow caches the archive under a key that contains that hash (`toolchain-key`), and the hash is
checked on every run, so a cache cannot change what is unpacked. The workflow runs on a pushed release tag `v*`, on a pull request into `dev` or `main`, and by hand (`workflow_dispatch`); day to day `tools/ci.sh` is run locally. It
has read-only `contents` permission, uses no secret, uses each action at a major-version tag (`@v4`), not a commit SHA, and uploads nothing but the ctest log of a
failed run (no `.elf`, `.hex` or `.bin`: CI never makes a flashable candidate).

`fixtures/` (the maintainer's SD files) is never in the repository. The tests that read it skip when it is absent
(`og_fixture_survey_test.cpp`), and `ci.sh` points `O3_FIXTURES_DIR` at a directory that does not exist so that a run does not
depend on whether the checkout sits inside a tree that has one; `O3OS_CI_FIXTURES=1` leaves the variable alone.

The host unit binary runs as several ctests instead of one (a single run was 157 s under ASan + UBSan): `unit_fuzz`, `unit_usb`,
`unit_model`, `unit_formats`, `unit_plot`, `unit_audio`, `unit_seq`, `unit_ui`, `unit_apps` (each a list of source-file patterns,
`tests/CMakeLists.txt`; a pattern that matches no file stops the configure) and `unit_rest`, every case no pattern names, so a new
`*_test.cpp` joins it by itself, and `unit_cover`, which checks with doctest's `--count` that the groups hold every case exactly once
and that none is empty. The verdict ctests `contrast`, `budget`, `translate_fallback`, `mk66_memmap`, `plot` and `plot_budget` still
re-run their cases on their own, as before.

**The gallery in shards (OS-CIFAST).** `o3sim --gallery` has `--only <prefix>` (the screens whose registry name starts with it; an
empty prefix or one that names no screen is exit 2; with `--check` the goldens of the other screens are neither required nor reported
missing, a golden that no screen of the registry produces is still stale), `--shard <i>/<n>` (every n-th shot of what `--theme` and
`--only` leave) and `--list` (the golden keys of the selection, nothing drawn). The check is `gallery_1of6` ... `gallery_6of6`
(`--check --twice`, each in its own directory; shard 1 also measures the display-list budget table, `--only` measures the screens it
names) and `gallery_shards_cover`, which adds the shards' `--list` up to the whole gallery, each shot once. `update_goldens` (OS-CIFAST2)
runs the scenarios and the same 6 shards as parallel `o3sim` processes (at most 4, `nice 10`, `tools/update_goldens.py`), merges their
partial goldens files into the bytes of the serial run, and with `O3OS_GOLDEN_ONLY=<scenario or screen prefix>` refreshes just that one
(sim/README.md "Goldens"; the contact sheets of a sharded run are per shard). The two negative tests (`o3sim_gallery_no_goldens_exit1`, `gallery_stale_diff_list`) run
`--only boot` (3 pictures): the same proof, a wrong goldens file is exit 1 and a stale `diff_list.txt` does not survive, in 2 s instead
of 160 s and 214 s. With `-j8` the full host run's ctest took 114 s wall and 739 s of test time (it was 234 s and 1,126 s).

**Risk scope (OS-CIFAST).** Every ctest has an area label, set in one table (`cmake/O3osLabels.cmake`, the first rule that matches the
test's name wins; a test that matches none stops the configure): `core model formats seq audio app platform python fuzz`, and
`static` (layers, the wiring audit, `unit_cover`: cheap, repo-wide, in every scope). `O3OS_CI_SCOPE=changed tools/ci.sh host`
(also `qemu` and `mk66`; the default is `all`) builds as usual and runs `ctest -L` only for the areas that the changed files map to:
`git diff --name-only <base>...HEAD` plus the staged, unstaged and untracked files, `<base>` being `O3OS_CI_BASE` (default `git
merge-base HEAD main`). The map (`scope_resolve` in `ci.sh`): `core`, `model`, `hal`, any `CMakeLists.txt`, `cmake`, `tools/ci.sh`
and every file with no rule (including `tests/*.cpp`, `usb`, `modules`) run everything; `formats` runs formats + fuzz +
app; `seq` seq + app; `audio` audio + seq; `app`, `ui`, `sim`, `eve` app; `platform` platform; `tools/*.py` and
`tests/*.py` python; `docs/qa/*` and `tools/qa_cover.py` static (the catalogue gate, OS-E2E-3); `docs/` and `*.md` only runs nothing ("docs only, no tests", before the build). The chosen labels and the
reason for each file are printed. **The scope is for intermediate runs: every release image (a release candidate) and the
GitHub workflow run the full CI** (no `O3OS_CI_SCOPE`).

## Target tests on QEMU (OS-007a)

The host run proves the logic; the target run proves it on the Cortex-M4F code generation the MK66 will run (`-mcpu=cortex-m4
-mfpu=fpv4-sp-d16 -mfloat-abi=hard`, `-Os`, no RTTI, no exceptions, the same warnings as errors). `-DO3OS_PLATFORM=qemu`
cross-builds each registered **suite**, an ELF of the same doctest `*_test.cpp` files the host build runs, and ctest runs it as
`qemu_<name>` on `qemu-system-arm -machine mps2-an386` with semihosting. A host build skips all of it. Today:

| ctest | what it runs |
|---|---|
| `qemu_core` | the `core` tests (not `ring_test.cpp`: it needs `std::thread`; the host and TSan runs cover it) |
| `qemu_eve` | the `eve` tests: the display-list encoders and the frame builder (with the host `FakeDisplay`, cross-built) |
| `qemu_plot` | the `plot` tests (OS-003d): the peak cache with the M4 SSUB16 + SEL loop, the math, FFT and spectrum, the wave bitmap's rows and uploads, the curve reduction, meters, scope, the RAM_G map and the word bound of every plot (all but the tests that draw into the simulator's rasteriser: `plot_wave_test`, `plot_*_pixels_test`, `plot_raster_test`, `plot_budget_test`, `plot_contrast_test`) |
| `qemu_boot` | OS-003f: the boot screen's words, the logo's flash budget and data, its RAM_G upload, and the boot hold (`boot_screen_test.cpp`; the pixels and the driver run on the host only) |
| `qemu_usb` | the `usb` tests (not `usb_ring_thread_test.cpp`: `std::thread`; not `usb_sim_test.cpp`: millions of SOFs), see the USB section |
| `qemu_usb_device` | OS-006b: the USB device logic against the fake controller (control pipe, endpoints, the interrupt, CDC and MIDI streams), the `diag` line shell and log (not `usb_diag_log_thread_test.cpp`) |
| `qemu_jobs`, `qemu_wav`, `qemu_wav_sample`, `qemu_model_undo` | OS-002c: the job runner; the WAV importer (variants, truncation at every offset, damage fuzz, the stereo mix); the importer as a registered format (the instrument a WAV makes); the undo journal, the Editor and the property test against snapshots (a 1.9 MB project in `.bss` each, so their own suites) |
| `qemu_platform` | the layer itself: `.data` copy, `.bss` zero, static constructors, FPU, SysTick clock, heap and stack measurement |
| `qemu_mk66` | the pure parts of `platform/mk66` (OS-007b, OS-007c): memory map, crash record, clock and SDRAM register values, memory tests, LED codes, interrupt map; the FT812 display driver (register facts, panel, SPI and DMA values, the chip bring-up against a model of the chip, `hal::Display` with its staging, swap, fall-back and backlight); the input drivers (OS-007d: the key matrices, the TCA8418 protocol and the scan against modelled chips, the lockout debounce, the encoder's detents, the IS31FL3731 pad LED driver, the board's pin and register facts) |
| `qemu_audio` | the OS-005a audio engine's pure parts: the rate and interrupt-table contract, the slot protocol against its reference model, the tone against a double-precision reference, `AudioStatus` and the diag text, the pipeline driven by hand (see "The audio engine skeleton") |
| `qemu_audio_fake` | the same engine behind interrupts: a fake I2S whose block clock is a timer interrupt (`platform/qemu/fake_i2s.h`), with the render on PendSV; run with `VIRTUAL_IDLE` |
| `qemu_audio_kernels`, `qemu_audio_graph` | OS-005b: every sample kernel of the audio graph against its scalar reference and against the model of the M4 instructions (the real SMLAWB/SMLAWT, SMLALD, SSUB16 + SEL, SHSUB16, PKHBT/PKHTB and SSAT run here), with `CCR.UNALIGN_TRP` set; the graph itself: the routing truth table, pan law and level curve, ramps, the reserved points, OUT MODE, the input strip and its latency, meters and taps (not the threaded `audio_taps_thread_test.cpp`, not the level-curve vector test) |
| `qemu_midi`, `qemu_seq`, `qemu_seq_model` | OS-005c: the MIDI timing core (Q32 time, messages, queue, hang book, the DIN emitter fuzzed against its lateness bounds, USB out, the router, MIDI in, the clock-follow loop against simulated masters), the FX table and control layers, and (one project in .bss) the sequencer: rows, behaviour, the reference vectors, determinism and goldens, following an external clock |
| `qemu_formats_og_files`, `_hostile`, `_registry` | the OG readers and writers, the settings chunks, the format registry and the byte-exact round trips on the Cortex-M4F (OS-004b); two 1.8 MiB arenas leave about 230 KB of heap, so the files are grouped three ways |
| `qemu_formats_med` | the MED reader, tempo conversion, flow linearisation and their hostile-input loops (OS-008a; the loops are thinned by 40 on the target; not `med_oversize_test.cpp`: an 11 MB sample) |
| `qemu_probe`, `qemu_probe_stack` | the harness probes, see below |

**Adding a library's suite is one line** in the target block at the top of `tests/CMakeLists.txt` (the library must already
build for the target; its test files may be wildcarded):

```cmake
o3os_target_suite(usb DEPENDS usb SOURCES usb_*_test.cpp)       # name, the libraries under test, the test files
```

It builds `usb_suite.elf` (strict warnings on the tests, the runner and startup linked in, size printed) and registers
`qemu_usb` with a 60 s timeout (`TIMEOUT <s>` to change it). A suite that idles in `WFI` between interrupts (a block clock, a
deadline) adds `VIRTUAL_IDLE`: QEMU then runs with `-icount shift=5,sleep=off`, idle time costs no real time and the run is exactly
repeatable whatever the host does (with the default `sleep=on`, virtual time follows the host's clock while the guest sleeps, and a
host stall shows up as a late or doubled tick). Leave out of the line a file that needs a host-only facility
(threads, files, the simulator); the host run still covers it. A suite that no longer fits the target's memory fails to link (`.bss` is in no load segment, so QEMU does not count it against the code RAM, and the script's `ASSERT`s name an overflow of either RAM; before OS-004b a large `.bss` killed a suite at load, silently):
split it into two lines, do not trim tests. `REQUIRE` is unavailable (no exceptions), as on the host: `CHECK` and return.

What a run reports, after doctest's own summary, in one line (`qemu_core` today):
`o3os-qemu: suite=core exit=0 rom=342352 data=704 bss=7652 heap_peak=46204/4120412 stack_peak=1840/65536 guest_ms=729`
(code + rodata + `.data` image, `.data`, `.bss`, the `_sbrk` high-water mark of the heap and its limit, the deepest stack
use of the 60 KiB thread stack, guest milliseconds). The exit status of QEMU is the suite's, through semihosting
`SYS_EXIT_EXTENDED`: 0 all tests passed, 1 a test failed, 2 a fault (the handler prints `o3os-qemu: FAULT` with CFSR, HFSR,
MMFAR, BFAR, PC and LR), 3 the stack overflowed (the MPU guard was touched, or the paint at the bottom of the stack was
overwritten), 4 `abort()` (a failed allocation, `std::terminate`). A suite that ran
no test case also fails (`test cases: 0`), so a renamed or emptied suite cannot pass silently.

**Failure propagation is itself tested** (label `probe`, `ctest -L probe`). `qemu_probe` runs an image whose only test case
fails on purpose, and passes only if QEMU exits with status exactly 1 and the output is that test case's failure.
`qemu_probe_stack` runs an 80 KiB recursion of 1 KiB frames that each touch one byte, and passes only with status 3 and
`FAULT stack overflow` (without the guard that run exits 0 with a corrupted heap). If failures ever stopped reaching ctest,
a probe would fail. The failing cases are in no real suite, so they never fail a normal run.

The layer (`platform/qemu`, implements the HAL; nothing below `app` includes it): `mps2_an386.ld` (4 MiB code RAM at 0,
4 MiB data RAM at 0x20000000: `.data`, `.bss`, the heap, a 32 KiB MPU guard, the 60 KiB thread stack, a 4 KiB exception
stack on top), `startup.cpp` (vector table, reset sequence: FPU, `.data` copy, `.bss` zero, stack paint, MPU guard with
MemManage enabled, the switch to PSP so exceptions have their own stack, constructors, `main`, `exit`; faults reported, not
hung; a frame larger than the 32 KiB guard can still jump over it, the paint check then catches what it can), `clock.h` (`SysTickClock`, a
`hal::Clock` on the 1 kHz SysTick with microsecond resolution from the down-counter), `semihost.h` (console, exit),
`runtime.h` (memory use), `syscalls.cpp` (newlib stubs: no libnosys), `test_runner.cpp` (doctest's `main`). QEMU runs with
`-icount shift=5`, so virtual time follows executed instructions: a test that waits on SysTick gives the same answer on a loaded
machine. Without `qemu-system-arm` on PATH the `qemu_*` tests are still registered and fail; `-DO3OS_QEMU=<path>` names it.

## The MK66 skeleton image (OS-007b)

`platform/mk66` (full notes, memory map, boot sequence, LED codes and the first-boot hardware checklist in
`platform/mk66/README.md`) brings the Tracker OG's MCU up from reset: a vector table with every handler named, the watchdog
unlocked and off, the 180 MHz HSRUN clock tree, the 16 MiB SDRAM with a wiring and capacity test, the ADR 0013 D1 interrupt
priority table (`hal/irq.h`), SysTick at 1 kHz behind `hal::Clock`, the DWT cycle counter behind `hal::cycles()`, fault handlers
that write a CRC-protected crash record into `.noinit` RAM and reset, and a `main` that idles in WFI with a heartbeat on the
status LED. (Display, audio and input followed in OS-007c, OS-005a and OS-007d, USB with the `diag` console in OS-006b; there is still no SD and no update path.) It had none of them at first, and no update path: **nothing is flashed or handed over**, and a
candidate needs the firmware gate check and the maintainer (the flashing-safety rules).

- **One source for the map.** `platform/mk66/include/o3os/platform/mk66/mem_map.h` holds every region as `constexpr` with
  `static_assert` sums (ADR 0013 D7); `mk66.ld` is generated from it at build time (`tools/gen_ld.py`) and checked by the
  `mk66_ld` ctest. SRAM_L is hot code only (`O3OS_RAMFUNC_L`), 48 KiB below a reserved 16 KiB module slab; SDRAM objects the
  linker places go to its 192 KiB slack (`O3OS_SDRAM_NOINIT`).
- **The flash configuration field** (0x400..0x40F) must read `ff ff ff ff ff ff ff ff ff ff ff ff de f9 ff ff`; a wrong value
  can permanently lock the MCU. It is guarded by compile-time asserts, linker asserts, the post-build HEX check (a failing HEX
  is deleted) and its own ctest (`mk66_flashconfig`), which share no constant.
- **Tests.** The pure parts (`platform_mk66_pure`: memory map, crash record, memory tests, clock and SDRAM register values,
  LED codes, interrupt map) have host tests (`unit`) and a QEMU suite (`qemu_mk66`, added with the one-line
  `o3os_target_suite()` of the section above). The python tools have self-tests (`mk66_gen_ld_selftest`,
  `mk66_image_selftest`) that break the inputs on purpose; the flash image must be one run with no hole (`contiguous`). What only hardware can show (the reset sequence, the clocks, the
  SDRAM timings, the fault entry, SRAM across a reset) is the checklist in `platform/mk66/README.md`.
- **Options** for hardware-session images: `-DO3OS_MK66_FAULT_DRILL=ON` (one deliberate fault 10 s after power-up, to prove the
  crash-record round trip), `-DO3OS_MK66_SDRAM_FULL_TEST=OFF` (skip the full-capacity test at boot; default ON).

## The audio engine skeleton (OS-005a)

ADR 0013 D1, D2, D7, D8 and `docs/specs/OS-005-audio-engine.md` 005a. **There is no audio engine on the PC** (maintainer): the host
runs the pure parts, QEMU the Cortex-M4 paths with a fake I2S, the MK66 image the hardware (gate HW-A, flashed only by the maintainer).

- **`hal/audio.h`**: block geometry (64 stereo int16 frames, a 3-slot TX ring (2 before OS-CPU10 slice 3) behind the one constant
  `kTxSlots`, a power-of-two physical ring for the eDMA address modulo), the one rate constant (`core::kRefCyclesPerSample` in
  `core/audio_clock.h`, cycles per sample as a rational: 1800000/441 by default, 4080/1 with `-DO3OS_RATE_44100=OFF`, the D-OS-8
  retune; `hal` and `usb` both take it from `core`, neither depends on the other for it) and everything derived from it (the I2S0 MDR
  divider 16/255 or 196/3125, MCLK, the block in cycles, the USB feedback value), the block callback contract, and `Device`
  (`start()` mutes first and never unmutes, `unmute()`, `mute()`). `hal/irq.h` gained the priority table as data with names and the
  compile-time checks the engine relies on.
- **`audio/`** (pure, no hardware, no heap, no `double`): `SlotRing` (which slot a render fills, the underrun check, three in a row
  mute and zero the stale slot, 32 on-time blocks to recover, late blocks dropped), `Pipeline` (the two entry points the
  platform's interrupts call: `on_boundary` in the DMA ISR, `render` on the render line), `Engine` (the render loop skeleton:
  input meter, bounded ParamChange drain, silence or the HW-A tone, one float to int16 conversion per block; the tone plays the left
  channel alone and then the right at another frequency so a swapped pair shows; DWT-style stamps per
  stage), the test tone (a Q32 block phase so the long-term frequency is exact, float inside the block, a sine of 3.5e-6 maximum
  error), `AudioStatus` (single-writer atomics, the render group behind a generation counter) and the `diag cpu` / `diag mem` text
  formatters (no printf). The hot functions are marked `O3OS_AUDIO_HOT` and run from SRAM_L on the MK66.
- **`platform/mk66`**: I2S0 + two eDMA channels + the SGTL5000 over I2C0, written from the K66 RM and the SGTL5000 data sheet as a
  behaviour reference (all in `platform/mk66/README.md`, with the HW-A checklist). **`platform/qemu`**: the fake I2S
  (`fake_i2s.h`).
- **Tests.** Host: `audio_rate_test`, `audio_slot_ring_test` (a reference model, 20,000 boundaries of random render timing for four
  ring geometries), `audio_test_signal_test`, `audio_status_test` (+ `audio_status_thread_test` under TSan: `audio_status_tsan`),
  `audio_pipeline_test` (10,000 blocks, late blocks, the mute), `mem_map_model_test`, and the `platform/mk66/tests/mk66_audio_*`,
  `mk66_sgtl5000_test` (the power-up order as assertions over a register model), `mk66_resources_test`, `mk66_diag_report_test`. The
  same files except the threaded one run on the Cortex-M4F (`qemu_audio`, `qemu_mk66`). `qemu_audio_fake` proves, with the real
  pipeline behind timer interrupts: the render runs on PendSV and never in the DMA ISR (the ISR preempts it when forced slow), slot
  ownership over 10,000 blocks in both phases of the RX completion, one slow render is exactly one underrun with the stale slot
  replayed, three in a row mute and recover after 32 blocks, and the tone against a double-precision oracle within 1 LSB.
- **MK66 image variants**: the default image runs the audio path and renders silence with the outputs held muted; `-DO3OS_AUDIO_TEST=ON`
  adds the unmute and the test tone (-20 dBFS) for HW-A; `-DO3OS_RATE_44100` (an option of the whole tree, ON by default since v1.0.0) is the D-OS-8 retune. All pass
  the static image checks and are reproducible; `platform/mk66/README.md` lists the six HW-A images.

## The audio graph (OS-005b)

ADR 0013 D2, D4, D6 and `docs/specs/OS-005-audio-engine.md` 005b. The skeleton's silence-or-tone became the fixed graph of D4, still a pure
block-based renderer (`audio::Graph::render`): the same code runs in the MK66's render interrupt, on QEMU and, later, in an offline-render job.

- **Kernels** (`audio/kernels.h`): level gain with saturation, mix-accumulate with a per-sample ramp, the fused strip passes (level + pan for one or two
  tracks, stereo or centre), round-and-saturate (`sat_clear`, `sat_clear_main`), the MAIN-to-float handover for an active master chain, mono sum,
  deinterleave, peak + sum of squares, Q15 <-> float. Each has a scalar reference (the specification) and a paired variant over `simd::NativeOps`
  (SMLAWB/SMLAWT, SMLALD, SSUB16 + SEL, SHSUB16, PKHBT/PKHTB, SSAT with a shift; `EmuOps` models them in portable C++, so the host tests exercise the
  paired loops themselves and the target run compares the real instructions with the model). Integer kernels are bit-identical to their references;
  `float_to_q15` differs by one LSB on exact ties only. Q15 samples, Q16 gains up to +24 dB applied at Q20, int32 accumulators at Q19 (a Q15
  sample is 16 units: the products keep four more fractional bits), rounded to nearest and saturated once per bus (D2); a gain above unity clips at
  the rails and never wraps.
- **Graph** (`audio/graph.h`): 16 track strips (a `configure(mask)` of the tracks that sound; 12 by default) plus the input strip (the codec RX as a
  stereo source row), each with level, pan (this OS's level and pan laws, `smoothing.h`), routing bits to MAIN / CUE / SNDA / SNDB / REC / USB,
  pre-fader sends, two reserved insert slots; the SNDA / SNDB effect slots (bypass closes the effect's input and lets its tail ring out); the master
  chain table `kMasterChain[]` (headroom, saturation, EQ, bass, limiter, space, bit depth: every stage a reserved slot, bypassed, bit-transparent;
  with a stage active MAIN reaches it as float from the accumulators, unclipped, and `float_to_q15` is the only saturation); the cue level and OUT
  MODE. Every mixer level goes through the 1.1.14 table (both laws are this OS's model, maintainer answer Q4 of OS-CLEANROOM; the 1.2+ mixer law is unsourced: crosscheck P1-1); the 1.2+ -24..+24 dB law (50 at 0 dB) is the
  INSTRUMENT volume's, applied by the voices from the project file's version (`level_gain_q16`, OS-007i). Solo mutes the other tracks, their sends included by default (the official firmware's live behaviour is Q; `set_solo_mutes_sends(false)` is the
  export's reading). Parameters are ParamChanges applied after the block's voices have rendered (the ids are `audio::graph_param::*`, equal to the model's Track and Master
  ids by `audio_model_ids_test`, plus engine fields from 0x100); every gain moves by a per-sample ramp (`smoothing.h`, the contract OS-005c
  reuses), and the gains of a strip that carries no signal snap to their targets, so a route closed under a mute stays closed.
- **Taps** (`audio/meters.h`, `audio/taps.h`, `audio/tap_mem.h`): meters per track, bus and master as exact windowed integers behind a seqlock snapshot
  (peak, hold, clips, sum of squares) for `ui::plot`, measured only for the channels in `set_meter_mask()` (all by default); scope and spectrum sample
  rings with an overrun policy, written only while `set_scope_enabled` / `set_spectrum_enabled` say a screen shows them (off by default); the StemTap
  into the OS-006a stem ring (`usb::sum_slots` per profile channel, staged only while enabled); the REC bus into a caller-provided region of the
  sample arena (the sequential write; no recording UI yet). Their bulk memory is one `TapMem`, placed by the platform (SRAM_L on the MK66).
- **Tests.** Host and QEMU: `audio_kernels_test`, `audio_graph_test` (including the null tests of D8: one track within 1 LSB of the ideal product, a
  mix of twelve with no DC offset), `audio_meters_test`, `audio_taps_test`; host only: `audio_model_ids_test`, `audio_taps_thread_test`
  (+ TSan `audio_taps_tsan`), the level curve against an independent oracle written in the test (`smoothing.h`'s laws, this OS's own model),
  `tests/bench/audio_bench.cpp` (`o3os_audio_bench`: stages of a block over eight scenarios and paired against reference; on the host the kernels run
  their instruction model, so only the shape of a table means anything, never its unit or a comparison between two builds).
- **Memory and cost (bytes measured by the build; cycle figures are a model until HW-A).** Default MK66 image, review fixes against the first
  OS-005b commit: flash 137,532 B to 142,300 B (+4,768: the specialised strip kernels live in `.ramfunc_l`); SRAM_U 86,376 B to 64,360 B (-22,016: the
  engine is 13.8 KB instead of 36.3 KB; 85.7 KB to 107.7 KB free after the 24 KiB stack); SRAM_L 10,388 B to 36,860 B (`.ramfunc_l` 14,588 B of
  its 24 KiB budget, `.tapmem_l` 22,272 B of its 24 KiB budget: the stem pool and ring 14.5 KB, the meters 2.4 KB, the scope and spectrum rings 2 KB
  each; 12.3 KB of the 48 KiB region still free). No SDRAM. The cost model (platform/mk66/README.md G-2: the disassembled loops weighted for the
  M4's load, store and branch cycles; an estimate, not a measurement): a 12-track project (all centred, a send each) is about 29 k cycles a block
  (11 %) with every meter measured and about 22 k (8.3 %) with the master's only, against about 38 k (14.4 %) before the fixes (the centre kernel,
  the two-track pass and the gated scope and spectrum rings); HW-A row G-2 measures it.
- **MK66.** The render interrupt runs the graph; `-DO3OS_AUDIO_TEST=ON` routes the HW-A tone through tracks 1 and 2 to MAIN and sweeps level and
  pan; `diag mix` and `diag mon` read the meters and raise the line-in monitor. The default image stays silent. HW-A rows G-1..G-7 in
  `platform/mk66/README.md`.


## The sequencer and the MIDI timing core (OS-005c, ADR 0013 D5)

`docs/specs/OS-005-audio-engine.md` 005c ("005c as built" there: the decisions, the open items with their defaults, the figures). Pure and host-tested; the MK66 MIDI drivers are 005c2.

- **`core/qtime.h`**: sample time as Q32.32 integers (a tempo is samples per 192-PPQN tick, exact rational arithmetic, no drift), the cycle / sample / microsecond conversions of the one rate
  constant. **`audio/events.h`**: `SeqEvent`, the 12-byte event the voices (005d), the MIDI out stage and the UI get, with a sample offset 0..n-1 and a documented order.
- **`seq`** (`seq/sequencer.h`): `step(block_start, n) -> events[]` over a `uint64` sample timeline. 192 PPQN, 48 microsteps a step, the swing pair arithmetic (`(150 - s)%` and `(50 + s)%`
  of a step), the Tempo / Swing / Speed / Tempo-fine / Order-jump / Break FX, Micro-move, Roll (four directions), Gate, the velocity FX, the random FX with a seeded generator, MIDI notes
  and CC A..F, the song cursor with per-track cursors and lengths, pattern and mode switching at the pattern end (PAR-021), transport (play, stop, resume, locate), the performance
  layer (per-track source pattern, length, play mode, stutter; tempo, pattern length), the control layers (`seq/control.h`: the first FX column over the second, performance over both, per
  voice-parameter group) and the row reader with the model's generation check (`seq/rows.h`). Following an external clock: `set_follow()`.
- **`midi`**: `din.h` (the byte-time model of DIN and the emitter's "clock is sacred" arbitration: a real-time byte goes between the bytes of any message, never waits for a SysEx), `usb_out.h`
  (events quantised to the 1 ms frame, never early), `out.h` (the router: events to bytes and due cycles, the CC table, per-port offsets, hang bookkeeping `hang.h`: stop and mute owe exactly the
  note-offs that are owed), `in.h` + `clock_dll.h` (stamped bytes through the `midiRt` ring, the parser, Start / Continue on the next clock, SPP, and a second-order delay-locked loop that
  slews and never jumps, started by a least-squares fit of the first twelve clocks).
- **Tests.** Host and QEMU: `qemu_midi` (time, messages, queue, hang book, emitter fuzz with the lateness bounds, USB, router, MIDI in, the loop against simulated masters), `qemu_seq` (the FX
  table and the control layers), `qemu_seq_model` (one project in .bss: rows, the sequencer, the sequencer following a simulated master, the reference vectors, determinism
  over block sizes and four golden event dumps). `O3OS_SEQ_REPORT=1 build-os-host/tests/o3os_tests --test-case="din:*,clock dll*,usb out*,seq follow*"` prints the figures.

## The FT812 display on the MK66 (OS-007c)

`platform/mk66/display/` drives the Tracker's FT812 (full notes, pins, init sequence, the frame and double-buffering scheme, the
DMA ownership and the hardware checklist DSP-1..12 in `platform/mk66/README.md`): DSPI2 as the bus (1.125 MHz until the chip is up,
18 MHz after), two eDMA channels (14 transmit, 15 receive) moving each display list and each RAM_G upload in one chip-select cycle
from `alignas(8)` staging buffers, a polled fall-back, the bring-up that survives a warm chip (probe, PD# pulse, reset, bounded
polls, a proof that the scan-out runs, backlight only after the first frame), and the `hal::Display` on top. The image boots to the
OS-002b pattern view with the demo data and the default theme; `-DO3OS_DEMO_CYCLE=ON` walks the cursor and cycles the themes;
`-DO3OS_DISPLAY_ROTATE` sets the orientation (0 until the bench says otherwise, ADR 0011 amendment 1).

- **Tests.** Host and QEMU (`mk66_display_*_test.cpp`): the facts and the panel as values, the SPI dividers and limits (every
  combination enumerated), the descriptors, the init as a recorded transcript against a chip model, every failure path with its time
  bound, the warm-start probe, the command FIFO and its fault recovery, the display contract (upload refusals in the contract's order,
  frames, the 2048-word budget, queueing behind an upload, uploads inside an open frame), the DMA fall-backs (refuse, fail, hang) and
  self-test, the backlight rule, and a **bus monitor** in the chip fake that fails any test in which a polled access meets a DMA
  transfer on the bus. Host only: `eve_riedel` (the pinned Riedel library run as an oracle against the same model: constants, panel,
  the order of `EVE_init()`, the fault recovery and the command burst, every difference listed) and `display app` (the OS app over the
  driver, three themes and a moving cursor, its picture rasterised from what the chip holds equal to the simulator's own run).
- **Vendored.** `third_party/eve-riedel` (MIT, tag 5.0.9, unmodified): host-only, see `THIRD_PARTY.md`.
- **Image.** Default image 47,648 B of the 1 MiB flash (the skeleton was 4,100 B: the UI's faces and the `ui` and `app` code are most
  of it; the display driver itself is 5.6 KB), 29,232 B of SRAM_U (the skeleton 744 B); all variants pass the static image checks and
  the build is reproducible (the same hash from two build trees).

## Input on the MK66 (OS-007d)

`platform/mk66/input/` makes the device interactive (full notes, the hardware as found with its sources, the checklist INP-1..13 and the open questions in
`platform/mk66/README.md`, "Input"). The 48 pads and 33 buttons are two **TCA8418** key scanners (the pads on I2C2, the buttons on I2C0, which the codec
shares: one polled I2C master for both, `src/i2c.cpp`, behind the `I2cBus` seam), the encoder is the **FTM2 quadrature decoder** read as a 16-bit counter
(24 detents, four counts each, no push), the pad backlight an **IS31FL3731** with one monochrome LED a pad (`hal::Leds`; the buttons have none). The scan is a pass of the
main loop with bounded work (no timer, no interrupt, no DMA channel claimed), the debounce a lockout (no added latency on a press), the queue the core SPSC ring with a
publish-by-comparison policy (a full queue loses no state), the chips are retried with a backoff and their configuration read back. `hal::Input` gained `stats()` (health
counters) and `hal` a `Leds` contract.

The app gained one hidden screen, **INPUT TEST** (`app/screens/input_test_screen.h`; the registry row has `hidden = true`, the screen's context captures): every button, pad and the
encoder live from the raw events, press counts (a bounce shows), the key scanners' health and the scan-to-app age of the last event. It is opened by holding **Shift + F1 + F8** at power-up
(or always, with `-DO3OS_INPUT_TEST=ON`), left with F4 + F5, cleared with F2 + F3. The default pad palette (`app/pad_palette.h`) is drawn from the theme's Accent and Sel tokens.
The gallery gains `inputtest_typical` / `inputtest_worst` in the three themes (999 of 1,792 words in the dearest state).

## Boot logo (OS-003f)

The first frame the display shows is the maintainer's logo: a pixel-art floppy disk with the Tracker on it, greyscale with a checkerboard dither, centred on
black, with `Track3r  o3os <build id>` in the Micro face under it, the same in every theme (`ui::boot_theme()`); the backlight comes up behind it and it
stays 800 ms from its first light (`core::BootHold`), after which the app's first screen replaces it (notes and the hardware checklist DSP-13..16 in
`platform/mk66/README.md`, "The display"; the look in `docs/ui/style-guide.md` section 12).

- **Source and converter.** `ui/assets/boot_logo.png` is the maintainer's `logo.png` byte for byte (1254 x 1254 RGB): the one source.
  `tools/logo_convert.py` (stdlib only) decodes it (non-interlaced 8-bit gray, gray + alpha, RGB, RGBA, every filter type), crops it to the picture,
  looks for a pixel grid (none: this picture's "pixels" are 8 px in one place and 11 in another), box-filters it to 124 x 124 over the central half of
  each cell, picks the smallest of L1 / L2 / L4 that keeps the dither (**L4**: RMS error 4.5 grey levels against 19.7 for L2 and 55 for L1) and writes
  `ui/src/boot_logo_data.cpp`, a word-aligned `const` array with `static_assert`s that tie it to `ui/boot_logo.h`. That file is committed; the ctest
  `logo_convert_fresh` regenerates it from the PNG and fails when it differs, `logo_convert_selftest` tests the decoder (synthetic PNGs of every colour type
  and filter), the grid detector and the quantiser. `--report` says what it decided, `--preview` writes the bitmap at screen size.
- **Cost.** 124 x 124 L4 uncompressed: **7,688 bytes of flash** (the target was 8 KB; no `CMD_INFLATE`), 8 KB of RAM_G (`kRamGBootLogo` 0x6F000, handle 11, in the
  RAM_G map with its `static_assert`s). MK66 image: flash +8,628 B in all, SRAM_U +32 B.
- **Shown.** x3 through `BITMAP_TRANSFORM_A/E` = 85 (8.8 fixed point, nearest) at 372 x 372, exactly centred. `eve` gained the six `BITMAP_TRANSFORM` encoders and
  the L2 format; the simulator gained the transform (every pixel at the matrix's centre, [A], see `sim/README.md`) and L2.
- **Boot path.** `appimg::start_app()` commits the boot frame right after the assets are up; the audio start's blocking codec waits (`delay_ms()`;
  the USB start does not use it) service the display through an idle hook, so the logo swaps in and the backlight ramps under them (the rules of the hook,
  the hold and the driver flag are `BootGate` and `IdleHook`, tested against the real driver in `mk66_boot_gate_test.cpp`); the app's first frame waits for
  the 800 ms and not longer.
- **Tests.** `boot_screen_test.cpp` (host and QEMU `qemu_boot`: the hold to the millisecond, the logo's data and budget, the upload, the frame's words),
  `boot_screen_pixels_test.cpp` (every pixel of the 372 x 372 picture, centring, the line, the same hash in every theme), `display_boot_test.cpp` (over the
  FT812 driver model: first frame, no light before it, the picture the simulator's, the hold), `raster_transform_test.cpp` and `dl_test.cpp` for the
  transform; gallery state `boot` `logo` (76 words) in the three theme slots.

## The File screen (OS-010a)

New Community OS behaviour (nothing here claims OG parity), designed after `docs/design/product-ux.md` section 4 and the triage row PAR-108 (32-character names,
folders, Recent). Page one, **OPEN**, lists the projects of the card's `Projects/` folder (an OG project is a folder that holds a `project.mt`) and opens the one
under the cursor. No New, Save, Delete, Rename, Recent, IMPORT or MODULES yet: writes need a storage contract that has them (`hal/storage.h` is read only), each its own
reviewed slice.

| Input | Action | Effect |
|---|---|---|
| Up / Down, encoder, Shift + F1..F5 | `nav.*`, `cursor.*` | as on every list (the screen is a `Cursor`) |
| Left / Right | `nav.left` / `nav.right` | previous / next first letter: the start of the group of names that begin with the same letter (folders and files are groups of their own) |
| F1 OPEN | `file.open` | hands `Projects/<name>` to the device's project load (`app::ProjectSink`, the same `start_project_load` that `diag load` calls: one loader); the strip says `OPENING <name>`, and, once the load has ended (its frames are held while it runs), `PROJECT <name> OPENED` or `PROJECT <name> FAILED: <why>`; a project that opened (or was recovered) shows the Pattern view then (OS-OPENTOPAT, new Community OS behaviour; NEW does the same, a failed open or NEW stays on File, a save never switches). Disabled in the simulator (no load attached) |
| F2 RESCAN | `file.rescan` | forgets the listing and reads the folder again |
| Delete | `file.delete` | OS-FILES part B: the project under the cursor is marked **deleted**. The first press asks in the strip (`DELETE <NAME>? DELETE AGAIN`) and remembers the entry; the second press (over the same entry of the same list: a move, another key, leaving the screen or a list still being read takes the question back) creates an empty file `.deleted` in the folder and says `DELETED: <NAME>`. The File list and RECENT hide a folder that holds it. Refused for the open project (`CANNOT DELETE THE OPEN PROJECT`). Nothing is moved or erased: see below |
| Shift + F7 PURGE | `file.purge` | OS-FILES part B: the open project's sample and MIDI instruments that nothing uses (no step in any pattern, no Random Instrument window, no MIDI Learn binding, not the audio of the instrument clipboard: `model/instrument_use.h`) leave their slots, in one undo unit, after a second press (`PURGE <N> UNUSED INSTRUMENTS? SHIFT+F7 AGAIN`); module instruments stay. F8 UNDO on Pattern, Song or Instrument brings them all back. **The card is not touched**: their files stay in `samples/` |
| the RECENT rows | | The first rows of the PROJECTS list are the last five projects opened or saved, newest first (`RECENT` at the right edge): the same folders as their A-Z rows, so F1 and Delete act on either. The list is `Settings/recent.o3rc` (`app/recent_list.h`): the text `O3RC 1` and up to five folder names, written quietly after each change, read when the projects are listed; a damaged or newer file is an empty list |

**What DELETE leaves on the card.** The card layer never erases a project folder or a file the user made (`hal/save_guard.h`: `may_remove` is a temporary or one of our own `.bak` files only), and since the measurements of OS-FILES part B it never renames across folders either (FAT's rename is up to three block writes, and a card pulled between them leaves one folder under two names that share their data; deleting one of them on a computer would corrupt the other). So DELETE only creates the empty marker `Projects/<name>/.deleted` (one directory entry, no data). **To free the space, delete the folder on a computer; to bring the project back, delete `.deleted`.** The device cannot empty or restore it yet (IDEA OS-TRASHUI). A marked folder is never loaded (the File screen, a RECENT row and `diag load` refuse it), so a SAVE never writes into it. **The device does not rename project folders** (even inside one folder FAT writes two directory entries that can sit in different sectors; a card pulled between them leaves two names on one chain, and deleting one of them on a computer corrupts the other). A project's title is renamed through OS-NAMES; a new folder name comes from opening the project, SAVE AS under the new name, then DELETE of the old one.

**The browser core** (`app/dir_index.h`, `app/dir_scan.h`, reusable: the Sample load screen takes it with `kWavFilter`). `DirIndex` is a sorted listing with a fixed capacity and no heap
(9.2 KB): **256 entries**, **8 KB of names** (each stored whole, up to 255 bytes: the path an entry gives back is the one on the card; 256 names of 31 characters fit, longer ones
fill the pool first); order is folders first, then names compared byte by byte with the ASCII letters folded, so it is sorted while it is still being read. A list that fills up keeps
what arrived first and says so (`full()`, "list full" on the screen). `DirScan` is a `core::Job` that fills it from a `hal::storage::Storage`: **a step spends units of its slice, 1
for a `readdir` and 8 for a probe, 250 us a unit (PROVISIONAL, until the hardware session says what a step costs), at least one entry and at most 32 units a step**, so the App's 2 ms
pass reads about one project (a probe is `exists(Projects/<name>/project.mt)`, three directory levels) or a few dozen plain entries. A `ListFilter` says what is listed: `kProjectFilter`
(folders that hold a `project.mt`), `kWavFilter` (every folder, and the `.wav` files); the kind of an entry is what the card says (`DirEntry::is_dir`), never what its name looks like
(PAR-033: a folder called `kick.wav` is a folder); hidden names (a leading `.`: macOS `._x`) are not listed, a name that FAT cannot hold fails the probe and is skipped. A scan that is Done
is a cache (`begin()` says `Cached` for the same folder and filter). One directory handle is open while it runs and it is closed on every way out (done, failure, cancel, the App's
destructor).

**On the App.** `attach_storage` and `attach_project_sink` (the device attaches both in `app_main.cpp`). While File is in view and nothing is listed the App's pass starts the scan as a job
(`files_`, quiet: the notification strip does not show it, the screen does: "reading Projects  8 so far"), keeps the cursor on its entry while entries sort in around it (a cursor at the
top stays at the top; below it, the entry keeps its line of the screen), and retries a scan that found no card, no folder or a card error when the screen is entered again; a listing that
was read is kept. Names are shown as printable ASCII, a UTF-8 character as one `?`, 32 characters at most (31 and `~` when longer). Size: the App grows by about 9.6 KB of SRAM (`.bss`);
the gallery's `file` screen (`typical`, `scanning`, `worst`, `no_folder`, `no_card`, over `platform/host` `FakeStorage`) peaks at 791 of 1,792 display-list words. Tests:
`dir_index_test.cpp` (also on QEMU, `qemu_dir_index`), `dir_scan_test.cpp`, `file_screen_test.cpp`.

## The sample screens (OS-011a)

New Community OS behaviour (nothing here claims OG parity unless a row says so), designed after `docs/design/product-ux.md` section 4 and the triage rows PAR-016, -033, -061, -091,
-113, -142, BASE-02, -16, -17. Storage is read only (`hal/storage.h`): every change lives in memory until a later slice saves the project.

**The sample bank** (`app/sample_bank.h`, one per device, owned by the platform: `app_main.cpp` `g_bank`, attached with `App::attach_samples`). It knows which PCM each instrument slot has in
the sample arena (SDRAM; the voices' `SampleDesc` only knows the window the instrument's start and end cut out of it), reserves the free tail of the arena for a WAV being read
(`reserve` / `commit_load`), and is the one way the sample screens tell the voices about a change. The arena is a bump allocator that only grows (a project load clears it, the loaders and the demo kit
`adopt` what they place); the last 350 KB of the 10.8 MB (`kPeakSamples`) are the memory of the peak cache of the sample in view, so the loaders see 10.45 MB.
**The handover protocol** (the part to review): the voices read a slot's descriptor in the render context and `set_sample()` writes it in several words, so the bank never writes at once. `request_rebind(slot)`
marks the slot; `poll()` (once a pass, main loop) applies the marks only when (1) the transport is stopped (the App's flag and the engine's published position agree: nothing can start a note) and (2) `voices_in_use() == 0` has held for
`kSettleMs` = 5 ms (longer than a block plus a margin, the OS-007f Settle rule). Until then the engine keeps its old descriptor, which points at memory nothing ever frees or overwrites. An edit therefore reaches the sound
when playback is stopped (the card says "SOUNDS WHEN STOPPED"); a render-side swap through a ring would remove the wait but needs a change in `audio/` (follow-up). While a WAV is being read nothing is applied (a slice table would take the tail).

**Sample load** (`screens/sample_load_screen.h`): the WAV browser, on the File screen's `DirIndex` / `DirScan` with `kWavFilter`, over the App's one `DirScan` (`files_for_` says whose listing it holds: entering the other of the
two screens cancels the scan and starts that one's over; they are never both scanning). Starts at the card's root, folders first, F3 ENTER / F2 UP, Left / Right letter jump, a pad chooses the target instrument. **F1 LOAD** (F4 ADD: the first free
instrument) starts the existing import job of OS-002c (`formats::wav::SampleReader`, the `IMPORT KICK.WAV 45%` strip) into the target slot with the audio read straight into the arena's tail, then the peak job; when the import is done `App::finish_import` makes
the audio the slot's (`commit_load`) and asks for the handover. A file that is not 44.1 kHz is loaded as it is and the strip warns (nothing is resampled). The undo history is cleared by the import, as the existing import
policy says. F5 PLAY (preview of a card file) is a TODO: it needs a preview voice that plays a buffer, which `PreviewSink` (instruments only) is not. A waveform preview of the file under the cursor is left out (it would read the whole file for every cursor
stop); the card on the right is the *target instrument*: name, length, rate, its waveform once loaded, the free arena. Evidence: the 48-slice limit (the instrument file holds 48 slice points); the keys (enter, add as next instrument, preview) are this OS's design, and the official sample importer's keys are an open research question. Tests: `sample_bank_test.cpp`, `sample_load_test.cpp`; gallery `sampleload` (`typical`, `worst`, `empty`, `no_card`).

**Wave / Chop** (`screens/wave_screen.h`, SamplePlay; the button again is the CHOP page; the field level of the chain carries the CHOP page's keys). A peak-cache waveform of the instrument in view (the App builds the cache as a job when the sample in view is not the cache's, `poll_peaks`),
the four points and the slice table as the model keeps them (0..65535 points, `app/wave_math.h`; at most 48 slices). `set_point` keeps start < end and start <= loop start < loop end <= end (the loop conditions of a file; moving the start pushes the loop ahead
of the start). The encoder moves the selected point or slice a column of the view a click (Shift: 16), with an optional zero-crossing snap that goes the way the encoder turns. Every edit is `Editor::set_instrument`: one undo unit, F8 / Shift + F8, and a
`request_rebind` (the engine follows when stopped; the screen says so). CHOP: AUTO is the transient slicer (`app/transient.h`, a `core::Job`, new Community OS behaviour (whether the official firmware's slicer behaves the same is an open research question); strongest hits kept, slice 0 at 0), EVEN n, ADD (at the window's
middle), DEL; slicing switches the instrument to beat slice (play mode 5) and says so. Pad n plays slice n (preview note 24 + n through `PreviewSink`; the device has none yet) and selects it, Shift + pad selects silently; `App::pad_lights` / `Screen::pad_roles` give the LED roles (the slices'
pads, the selected one brighter), which `app_main.cpp` flushes. Tests: `wave_math_test`, `transient_test`, `wave_screen_test`; gallery `wave`.

**Sample edit** (`screens/sample_edit_screen.h`): a selection (Left / Right pick the edge, encoder moves it) over the sample in view and six offline effects, F1 CROP (needs a selection), F2 NORM, F3 REVERSE, F4 FADE IN, F5 FADE OUT, F6 AMP (Shift + Up / Down sets the percentage), F7 ALL, F8 UNDO / Shift + F8 REDO
(BASE-16 lists these among 13 effects and undo snapshots). The kernels are pure (`app/sample_dsp.h`, int16, chunked, tested), run by `SampleEditJob` on the main loop, never in the audio path.
**AMIGA conversion (OS-AMG1, new Community OS behaviour; `app/amiga_convert.h` has the design).** Shift + F1 / Shift + F2 resample the WHOLE sample (a selection means nothing to it) to 8,287 Hz (Paula period 428, the Amiga voice's unity note) or 16,574 Hz (period 214, one octave up: more top end) with a Kaiser windowed-sinc low-pass (flat to 0.01 dB up to 0.40 of the new rate, -6 dB at 0.45, 90 dB or more above 0.52; both rates, measured in `tests/amiga_convert_test.cpp`), as one `SampleEditJob` (the kernel table is built in slices first, then output frames in bounded slices, the strip shows the progress), into a spare buffer; commit is one undo unit (`PointsRec::conv`: the audio and the instrument's recorded rate, engine and tune go back together). The instrument becomes engine AMIGA with sample_rate 8,287 / 16,574 (the saved WAV carries it; no format change) so "CONVERT TO AMIGA FIRST" is satisfied. THE PITCH STAYS: the Modern voice plays a sample at the engine's rate (44,117.6 Hz) at C-4 whatever its recorded rate, the Amiga voice plays period 428 (8,287.14 Hz), so the ratio is 5.3236 (1 or 0.5 for an instrument that already is Amiga); at 16,574 Hz the instrument's TUNE gets +12 (the Amiga voice adds tune to the note: period 214 at note 72), refused when the tune is above +12. Points, loop and slices are 0..65535 fractions of the length, so they follow by themselves. Shift + F3 toggles the 8-BIT option: round to the nearest 8-bit level, plain, no dither (the Amiga voice plays the top byte of each sample, truncating, so an unquantised sample is floored with half a step of DC; the quantised one is exact and kept as level * 256). Mono only (the bank holds mono).
**The edit protocol (the part to review).** An effect never writes the audio the voices may be reading: its result goes into another buffer, a spare of the bank's pool (`SampleBank::edit_begin`: at most 4 buffers as large as the sample, taken from the arena's tail once and reused for ever, a buffer reused only when
no slot's audio, no engine binding (`Slot::bound`: the descriptor may still point at it until the handover) and no history step references it). When the job is done the App stages the result and invokes the internal undoable action `sample.commit`: `edit_commit` makes the buffer the slot's audio, keeps the old one as an undo step
(three deep, redo steps dropped by a new edit), a crop remaps the instrument's points and slices (`crop_points`, one project undo unit), and the handover swaps the voices' descriptor only when playback is stopped and the pool quiet. Undo and redo are the same pointer swap backwards (`sedit.undo`, which also restores the points).
The sample's history is separate from the project's journal (audio cannot be journalled); the project's own F8 undoes the points, not the audio. A result for a sample that changed while the job ran is thrown away. Tests: `sample_dsp_test`, `sample_history_test` (including: the engine's bound buffer and the original are never overwritten), `sample_edit_screen_test`; gallery `sampleedit`.

**Config** (`screens/config_screen.h`): four pages, the button again goes on, F1..F4 pick one. MIDI and DISPLAY are the device settings, generated from the descriptors like PARAMS (so they are editable, learnable and reset the same way, with no project): `model/params.cpp` kDevice now has two groups, and
six settings were **appended** (fields 7..12: CLOCK MODE off / master / slave, MIDI IN and MIDI OUT DIN / USB / both, IN CHANNEL all or 1..16, OUT CHANNEL 1..16, THEME). The MIDI ones are wired by OS-013a: CLOCK MODE, MIDI OUT and OUT CHANNEL drive the MIDI link (`seq::sync_config_from_settings`, main loop, whenever a generation moves), MIDI IN and IN CHANNEL filter the control changes MIDI Learn takes (`App::poll_midi_cc`).
**Persistent format note** (`docs/formats/o3-settings.md` sections 2 and 8): the device settings file's SETS chunk grows from 7 to 13 entries (default file 94 -> 142 bytes), by the section's own rule (a row is appended, no new chunk, no version bump); a file written before reads with no entry skipped and the six at their
defaults (`settings_format_test`: the old 94-byte file is a literal), and the previous build skips and counts the six it does not know. The settings block keeps its 348 bytes (it had room for 16 fields). THEME and the screen's theme follow each other (`App::poll_theme`, `set_theme`, Shift + Master). SD page: card present, mounted, file system,
capacity, cluster (`hal::storage::VolumeInfo`); the label and the free space are not shown (no cheap call: FatFs `f_getfree` scans the whole FAT), the rows were dropped (OS-CFGWIRE). INFO: product and build id (`App::set_build_info`, the platform passes `usb::kProductString` and `O3OS_BUILD_ID`), the free sample memory, and the licences of ADR 0005 (core GPL-3.0-or-later, module SDK MIT, Spleen,
Terminus, FatFs). The header says that the settings are saved to the card (Settings/device.o3st). Tests: `config_screen_test`; gallery `config`.

**SRAM.** The sample screens' state is kept small: `app::SampleBankMem` (the slot table, the edit history, the WAV reader, the slicer's hits and scratch, the two jobs, the edit pool's buffer table, and since OS-INSTUI S7 the clipboard, the swap records of PASTE / LOAD and the `.pti` reader: 21,160 B, 18,248 B before S7; the older figure here, about 7 KB, was the first slice's) is a separate object that the platform places in the tail of the sample arena (SDRAM, behind the peak-cache memory); the App and the bank
keep a pointer. Measured on the mk66 image: .data + .bss +432 bytes over the previous build (132,648 -> 133,080).
