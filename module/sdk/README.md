# Module SDK guide (ADR 0002, ABI 1.13)

Native modules for the Community OS of the Polyend Tracker hardware (unofficial): **instruments**, **effects** and **apps** that live as `.tmod` files on the SD card and are loaded by the OS. This
directory is the whole SDK and is **MIT licensed** (`LICENSE`), whatever licence the OS has: write a module under any licence you like. Written for OS-012b
(2026-10-12); the design background is [ADR 0002](../../docs/adr/0002-sd-plugins.md) (the decision, its amendments, the containment rules) and
[docs/guides/module-sdk.md](../../docs/guides/module-sdk.md) (where this guide is linked from the project's docs).

- `o3module.h` is the C ABI: the descriptor your module exports (`o3m_entry`), the services the OS lends it (`o3m_host`), the draw API of its views, the file format.
- `o3module.ld` is the linker script that gives the module its two parts: `.o3m_hot` (code marked `O3M_HOT`, run from fast SRAM when there is room) and `.o3m_cold`
  (everything else, read-only, in SDRAM).
- `../../tools/o3m_gen.py` makes the descriptor header from the module's manifest (`module.json`) and checks a built file against it; `../../tools/o3m_pack.py` builds and packs.
- `../../modules/templates/{instrument,effect,effect_stereo,effect_key,app}/` are working starting points (`effect_stereo`: a WIDENS auto-pan, `effect_key`: a sidechain ducker, ABI 1.12, section 3b); `../../modules/tb303/` is the worked example (a real engine with a manifest, a DSP header, two
  views and oracle tests); `../../modules/chip/` (a chip-tune voice) and `../../modules/fm/` (a four-operator FM voice) are two more, written from scratch by following this guide, each
  with its own host test executable (section 1b); `../../modules/apps/pads/` is an app (ABI 1.2, kept as the proof that an old file runs), `../../modules/apps/midimon/` another (ABI 1.3, a MIDI monitor that holds midi_in: the worked example of the host tables); `../../modules/apps/loudness/` a level meter on the audio tap (ABI 1.4, audio_tap: the worked example of the tap, OS-H2a); `../../modules/apps/rhythmgen/` (ABI 1.5, steps and edit groups, OS-H3A) and, in the same shape, `../../modules/apps/chords/`, `humanize/`, `mutate/` and `morph/` (OS-H3B: the first two use only the step entries and declare 1.5, the last two use the instrument parameters of ABI 1.6; they share `../../modules/apps/common/o3m_edit.h`); `../../modules/apps/tuner/` and `../../modules/apps/scope/` the two heavy readers of the tap (OS-H2b: a pitch detector on `tap_stream` and a scope / spectrum on `tap_latest`, with the FFT and the text helpers shared in `../../modules/apps/common/` through CMake `INCLUDE`; the worked examples of a main-loop job split into bounded units); `../../modules/apps/tapcalc/` a third (tap tempo and a BPM -> note-length calculator that sets the project's tempo, OS-TAPCALC; ABI 1.3 with read_project and edit_project; a manifest, a header of pure arithmetic shared with its host test, and the host clock `now_ms`); `../../modules/apps/rhythmgen/` is the first app that edits the project (ABI 1.5, read_project + edit_project: a polyrhythm generator; the worked example of the step POD and the edit groups, OS-H3A); `../../modules/sine/` is the smallest instrument.

## What is real and what is not

**Verified by tests on the host** (the doctest suite and ctest of `os/`; QEMU runs the loader's pure parts): the file format and the loader (relocation, every refusal), the
packer, the generator, the draw API and its budgets, the screens, the picker and the apps' pages, the TB-303 against Open303. **Not yet verified on a Tracker**: nothing of this has
run on the device. The hardware checks that are still open are listed at the end (section 10); do not claim a module "works on the Tracker" from host tests.

| | State |
|---|---|
| ABI 1.7, `.tmod` format (with the module's version and an app's capability word), loader with quarantine, packer | built, host-tested |
| ABI 1.8 (OS-MODPITCH, OS-INSTUI S10): `pitch()` for the Slide and Glide FX, `O3M_NOTE_RAMP`, params page names and INT value labels, the descriptor sized per minor (`O3M_MODULE_SIZE`); the built-in TB-303, Chip, FM and SID use them | built, host-tested; nothing has run on a Tracker yet (QA-MOD-099..106, `platform/mk66/tools/hil/qa_modules.json`) |
| ABI 1.9 (OS-KEYROWDLG S5, OS-APPUX 3.1; apps): `o3m_ui.keys` returns the labels of F1..F7 and Shift + F1..F7 (`o3m_keys`, up to 7 printable ASCII characters each, NULL or `""` = blank) for the app page's key row; the table is sized per minor (`O3M_UI_SIZE(minor)`: 20 bytes up to 1.8, 24 from 1.9; never `sizeof(o3m_ui)`); F8 is the page's BACK and Play the transport's for EVERY app and neither reaches `input()`; F7 + F8 stays the exit chord. A 1.1-1.8 app still loads, with the row `-- -- -- -- -- -- -- BACK`, but it no longer gets F8 or Play (the page's change, not the declared minor's: it reaches the apps already on a card). | `module/sdk/o3module.h`, `docs/specs/OS-KEYROWDLG.md` |
| ABI 1.10 (OS-APPUX S3; apps; io-table entries only, `o3m_ui` and the descriptor do not grow): `io.get_transport` (the play position the Pattern's play row follows: `o3m_transport`, nine words), `io.set_own_param(index, value)` (the app keeps one of its declared `o3m_param` rows with the project: clamped and rounded by the descriptor, handed back to `set_param()` BEFORE `enter()` at the next launch, no echo to the caller's `set_param`), `io.play_note` / `io.stop_note` (capability bit 64 `play_notes`, no confirm: up to four held notes and eight calls a frame through the project's instruments like a pad, all ended when the page closes; a MIDI instrument does not sound from an app). A screen button on an app page leaves the app for every app, whatever its minor (it is never delivered), and an app that declares no rows now gets the Shift keys; an app built at 1.10 has no OS parameter column: its declared rows are storage, it draws them itself in the grammar of `modules/apps/common/o3m_edit.h` and receives every Shift key. Its remembered rows reach `set_param()` before `enter()` (an app of 1.2 to 1.9 still gets them after it); a `set_own_param` made inside `exit()` is not kept. The project keeps the rows of up to twelve apps (`APPM` 1.1, `docs/formats/o3-project.md`). A 1.10 file on an OS of 1.9 is refused `NEEDS NEWER OS`; bit 64 is a reserved field below 1.10 | built, host-tested; nothing has run on a Tracker yet (QA-MOD-126..130) |
| ABI 1.11 (OS-FLASHDIET I1/I2; **importers, kind 3 — first-party only**): the importer table (`o3m_entry.importer`: the served formats and `begin` / `step` / `read_pcm` / `report` / `close`), the descriptor 84 bytes from 1.11 (`O3M_MODULE_SIZE`), and header word 44 the **importer layout tag** for kind 3 only (the packer writes it; an OS refuses a kind 3 file whose word is not its own tag, `LayoutMismatch` — the version lock: an importer is compiled against the OS's own model headers, so it only runs on an OS with the same import-facing model layout). An importer has no parameters, no views, makes no sound, never binds to a track and does not count against the module slots; the loader reads it from `/modules/importers/` only for the importer scan (a project that names an importer's stem never loads it) | built, host- and target-tested (`tests/module_importer_test.cpp`, `tests/module_import_test.cpp` byte-equality, the QEMU suite `qemu_module_import_run`, the packer's self-test, the built `tracker.tmod` loaded through the loader); nothing has run on a Tracker yet |
| ABI 1.12 (OS-INSFX-ABI2; **effects**, and factory presets for instruments too): the descriptor grows `render_st`, `fx_flags`, `presets`, `preset_count` (100 bytes from 1.12, `O3M_MODULE_SIZE(12)`), `O3M_PARAM_KEY` marks a sidechain KEY parameter, `o3m_host` grows `get_clock` and `track_level` (60 bytes; the importer's own table stays 52). Section 3b | **the surface and the tooling are built, host-tested** (A1: header, loader and image checks, packer; A2: the generator's `"stereo"`, `"presets"` and `"type": "key"`, the `effect_stereo` template; **A3: `get_clock` is live**, the sequencer's tempo, beat and flags, for instruments and effects). A4 is live: on a track the OS calls `render_st` (WIDENS on every block, stereo-capable on a block whose input has S, or while its stereo tail rings; the master is A7). **Not live yet**: `track_level` is live (A6: the key store of `audio/insert_host.h`, the `effect_key` template ducks), and nothing lists or applies a preset (A8, A9); a 1.12 module loads and an ordinary effect runs as before. Nothing has run on a Tracker |
| ABI 1.13 (OS-MODRATE S2; **instruments**; the descriptor does not grow): the `fx_flags` bit `O3M_FX_RATE` (2) says an instrument renders correctly at any `host->sample_rate`; the OS may then offer a per-instrument RATE of FULL, 1/2 or 1/4 (the MIX page's RATE row), call `render` for `frames / 2` or `frames / 4` frames at `sample_rate` 22050 or 11025 and hold each result sample 2 or 4 times (zero-order hold, no filter). Section 8 | **built, host-tested**; RATE is a session value (nothing is saved yet, S3). Built-ins that carry the bit: Sine, FM, TB-303, SID, Chip (S5: its frequency clamp, LFSR bound and filter ceiling follow the rate). Nothing has run on a Tracker |
| App host API v2 (ABI 1.3, ADR 0014 slice H1): capabilities, the read / edit / io tables, `now_ms`, tempo, MIDI in, the launch context | built, host-tested; see `docs/guides/app-modules.md` section 3 (MIDI out is planned) |
| Audio tap (ABI 1.4, ADR 0014 slice H2a, amendment E): `io.tap_select` / `tap_latest` / `tap_stream` / `tap_levels` for an app that holds audio_tap; the Loudness VU app (H2a), the Tuner and Scope Spectrum apps (H2b, QA-MOD-024..033) | built, host-tested; the tap's cost on the unit is not measured yet (`diag cpu` prints it; QA-MOD-023), nor are the apps' per-frame times (QA-MOD-033) |
| Steps and edit groups (ABI 1.5, ADR 0014 slice H3a, amendment G): `o3m_step`, `pattern_info` / `get_steps` in the read table, `edit_begin` / `edit_end` / `set_step` / `set_steps` in the edit table, `block_tracks` in the launch context, Pattern F7 TOOLS; the Rhythm Gen app | built, host-tested; nothing has run on a Tracker yet (QA-MOD-070..079, QA-PAT-160..169) |
| Instrument parameters (ABI 1.6, ADR 0014 slice H3b, amendment H): `instrument_info` / `get_param_desc` / `get_instrument_param` in the read table, `set_instrument_param` in the edit table (joins the group; only `O3M_PD_APP_WRITABLE` parameters); the Chords, Humanize, Mutate and Morph apps | built, host-tested; nothing has run on a Tracker yet (QA-MOD-080..089, QA-PAT-210..213) |
| Export writes (ABI 1.7, ADR 0014 slice H4, amendment A): `io.export_open` / `export_write` / `export_close` / `export_status` for an app that holds export_files (new files in `AppExport/<app>/`, an extension allowlist, a 16 KiB staging ring, 4 MiB and 16 files caps, atomic commit, first-launch confirm), `midi_channel` in `o3m_instrument_info`; the SMF Export app | built, host-tested; nothing has run on a Tracker yet (QA-MOD rows and `platform/mk66/tools/hil/qa_h4.json` for the unit check on a marked test card) |
| Instruments: bound to an instrument slot, rendered by the engine | built, host-tested |
| **Effects** (OS-INSFX) | built, host-tested: an effect module (kind 2) runs as a track insert, bound from the console (`diag ins bind <track> <slot> <module>`) after `mod load`: mono in place before the sends and the fader, up to four at once, a tail rule (`render` returns 1 while the input is live or the effect rings, 0 when both are silent), a CPU net that forces a heavy one to dry (S1, S2: unit PASS on 2026-11-01). The first sample effects are `../../modules/effects/biquad/` (S8: a biquad filter with mix; host tests against a double-precision reference; its cost on the unit measured, QA-GLB-170) and `../../modules/effects/drive/` (OS-INSFX-FX1: saturation with soft/asymmetric shape, drive, tone, DC blocker, mix and output; host tests against a double-precision reference of the same maths; **its cost on the unit is not measured yet**, QA-GLB-176) and `../../modules/effects/crush/` (OS-INSFX-FX1: bit-depth and sample-rate reduction with mix and output; host tests against a double-precision reference; **its cost on the unit is not measured yet**, QA-GLB-192), `../../modules/effects/fold/` (OS-INSFX-FX1: a level-compensated triangle wavefolder with symmetry, tone, DC blocker, mix and output; host tests against a double-precision reference; **its cost on the unit is not measured yet**, QA-GLB-204), `../../modules/effects/envfilter/` (OS-INSFX-FX1: a state-variable filter swept by an envelope follower on the track, with resonance, attack, release, sensitivity and mix; host tests against a double-precision reference; **its cost on the unit is not measured yet**, QA-GLB-207), `../../modules/effects/comp/` (OS-INSFX-FX1: a compressor with a TRANSIENT mode, make-up and mix; host tests against a double-precision reference; **its cost on the unit is not measured yet**, QA-GLB-196) `../../modules/effects/phaser/` (OS-INSFX-FX1: a 4 to 6 stage all-pass phaser with an LFO, a bounded feedback and mix; host tests against a double-precision reference and the exact response; **its cost on the unit is not measured yet**, QA-GLB-200) and `../../modules/effects/pingpong/` (OS-INSFX-FX2: a tempo-synced stereo ping-pong echo, a WIDENS effect of ABI 1.12 with Sync labels and five factory presets; host tests against a double-precision reference; **its cost on the unit is not measured yet and the bench cannot time its 177 KB state**, QA-GLB-223) and `../../modules/effects/wide/` (OS-INSFX-FX2 X1: the first stereo effect, a WIDENS widener of ABI 1.12 that adds a delayed, high-passed copy of the mid to the side, mono-compatible, with four factory presets; host tests against a double-precision reference and a mono-sum check; needs OS-INSFX-ABI2 A4 on the unit; **its cost on the unit is not measured yet**, QA-GLB-213) and `../../modules/effects/chorus/` (OS-INSFX-FX2: a stereo chorus, a WIDENS effect of ABI 1.12 with factory presets; host tests against a double-precision reference; **its cost on the unit is not measured yet**, QA-GLB-217) and `../../modules/effects/autopan/` (OS-INSFX-FX2: a stereo auto-pan, a WIDENS effect of ABI 1.12 with factory presets, synced to the beat through `get_clock` or free, with an optional HEADPHONE binaural mode; host tests against a double-precision reference; **its cost on the unit is not measured yet**, QA-GLB-226). **Still design-only**: saving an insert in the project (S3), the Mixer's INSERTS page and picker (S4), the parameter page of an insert (S5, so no console or screen sets an insert's parameters yet), the master insert (S6), MIDI Learn (S7); no effect is built into the image: an effect is a `.tmod` in `/modules/effects/` (`docs/specs/OS-INSFX.md`) |
| Standard controls from the parameter descriptors, MIDI Learn, undo | built, host-tested (instruments); apps get the controls, not Learn/undo (section 6) |
| Views (draw API v1), budgets, quarantine | built, host-tested |
| Apps (MODULES screen, page, input, exit chord) | built, host-tested |
| Engine picker (F1 on the Instrument page) | built, host-tested; changing the engine of a slot that has one needs a project reload |
| Crash containment (fault handlers, crash record, boot counter) | partly: the crash record names the module that was running and the next boot quarantines it; watchdog and fault handlers are the platform's open items |
| Running module code from SDRAM at speed, the code cache | **not validated** (ADR 0002 validation 1 to 6) |

## 1. The flow

    spec  ->  generate  ->  implement  ->  host oracle tests  ->  pack  ->  /modules/<kind>/  ->  diag mod

1. **Spec.** Copy a template (`o3m_gen.py new --kind instrument --id acid --name "Acid Bass" --out my_acid/`) and edit `module.json`: the name, the parameters, the views, the
   memory (section 2). It is the one place the module says what it is.
2. **Generate.** `o3m_gen.py gen module.json --out acid_gen.h` writes the C header: the parameter table (`o3m_param[]` with ranges, units, curves, flags), the parameter and view
   ids as enums (`ACID_PARAM_CUTOFF`, `ACID_VIEW_FILTER`), the ABI minor the module needs and the macros that fill the fixed part of the descriptor. `new` has already done it
   once. Regenerate after every edit of the manifest; `o3m_gen.py check module.json acid_gen.h` (and the `o3m_gen_check_*` ctest) finds a stale header.
3. **Implement.** Your source is a freestanding C++ (or C) file that includes the header and defines `o3m_entry`: the `State`, `init`, `render`, `set_param`, `note_on`,
   `note_off`, `shutdown`, and a `view_draw` for each view (section 4 and 5). The template of your kind is a complete small module.
4. **Host oracle tests.** The same source builds for the host as a shared object (`g++ -shared -fPIC -I module/sdk -I <gen dir> acid.cpp -o acid.so`; CMake does it with
   `o3m_add_module`) that a doctest loads with `dlopen` and drives through the ABI. Test the DSP against a reference (the TB-303's tests compare every stage with the original in double
   precision: `tests/tb303_oracle_test.cpp`), and the views through `module::ViewHost` (`tests/tb303_view_test.cpp`: the curves the view hands the draw API, against formulae).
5. **Pack.** `o3m_pack.py build --cxx arm-none-eabi-g++ --src acid.cpp -I my_acid --out acid.tmod` compiles with the flags the ABI promises (Cortex-M4F, hard float, `-mlong-calls`,
   no PIC, no libc), links with `o3module.ld`, and packs. It refuses, naming the cause: a writable global, a constructor, an undefined symbol (a libc call), any relocation but a
   32-bit absolute address. Then `o3m_gen.py verify module.json acid.tmod` reads the file back with the generator's own reader and says every way it differs from the manifest.
6. **Install.** Copy the file to the card: `/modules/instruments/acid.tmod`, `/modules/effects/`, `/modules/apps/` or, for an importer (kind 3, ABI 1.11), `/modules/importers/`. The file name (without `.tmod`) is the module's name
   in the project and everywhere (at most 31 characters, no `/ \ : * ? " < > |`); a name is one module on a card (the OS looks in `instruments/`, `effects/`, `apps/` in that order).
   **Built-in modules (OS-012d) and the module version (OS-MODVER).** The firmware image carries the TB-303, chip, FM and SID as `.tmod` bytes in flash and loads them with the same loader (same
   checks, same slab and SDRAM arenas), so they are in the ENGINE picker with no card. A file of the same name on the card is tried first and **wins only when it is valid, of the same kind and
   has a higher module version than the built-in one** (the version is in the file's header, see below; the four built-ins are 1.0.0). An equal version, an older one and an unversioned file
   (version 0) all keep the built-in module, so a copy left on the card does not shadow a fixed built-in module after a firmware update; to override one, ship a higher version. Otherwise, or when
   the card's file is broken, the built-in copy is used and the card's file is not quarantined (`mod` lists "card tb303 v1.0.0 not newer than built-in v1.0.0", `mod load` says the same).
   Delete the card's file to get the built-in module back. `diag mod notice` prints the licence notices the image carries.
   **Giving a module a version.** `o3m_pack.py build ... --version 1.2.0` (CMake: `o3m_add_module(acid SOURCES ... VERSION 1.2.0)`) writes it into the header: `X.Y.Z` with whole numbers, major 0..65535,
   minor and patch 0..255, no leading zeros; the default `0.0.0` is "unversioned". Versions compare as numbers (major, then minor, then patch). A module that carries a version declares ABI 1.2
   (`"abi": "1.2"` in `module.json`, then regenerate; by hand, build against this SDK): the packer refuses a version on a module that declares less, because an OS of ABI 1.1 would refuse the file
   anyway. Raise the version every time you publish a changed module; the OS reads it, it never writes it.
7. **Run.** An instrument: Instrument page, F1 ENGINE, choose it. An app: MODULES screen (from the Navigator), F1 (the only way to launch one; `diag mod load` also finds apps but only loads and checks them). `diag mod` on the USB console lists, loads, binds and auditions
   (`mod load acid`, `mod bind 3 acid`, `mod note 3 60`).

## 1b. Building and testing a module in this repository

`o3m_gen.py new` writes the module's directory; for the build to know it, register it in `modules/CMakeLists.txt` (three lines):

```cmake
o3m_add_module(acid SOURCES acid/acid.cpp OPT -O2)   # host shared object o3m_acid_host AND the packed acid.tmod (o3m_acid_tmod, part of `all`); OPT is the packer's -O level (default -Os)
o3m_manifest(acid acid/module.json)                  # the committed acid_gen.h is checked against the manifest (ctest o3m_gen_check_acid); the built file is read back (o3m_gen_verify_acid)
o3m_add_test(acid acid/tests/acid_test.cpp)          # the module's own test executable o3m_acid_test (ctest o3m_acid_test)
```

The packed file is rebuilt when any `*.h` next to the sources or in an `INCLUDE` directory changes (the packer is a custom command, so CMake does not see included headers by itself).
`o3m_manifest(... GENERATE)` (the templates) keeps the header in the build tree instead of the source tree: no committed copy to go stale, but a test or tool that includes the header must
use the build directory. A module with a DSP header of its own (`acid_dsp.h`) includes `acid_gen.h` from it, so that the parameter ids have one source.

`o3m_add_test` builds a **standalone doctest executable** (preferred over adding to the big `o3os_tests` binary: a module's tests are built and run on their own). The helpers are in
`modules/tests/o3m_test_support.h`:

| helper | does |
|---|---|
| `Lib` | `dlopen`s the module's host shared object (`O3M_<ID>_HOST_SO`, defined by CMake) and finds `o3m_entry` |
| `Instance` | one instance driven as the OS drives it: `start()` (init, then every parameter at its default), `set(id, v)`, `on(note, vel)`, `off(note)`, `render(frames, block)` and `busy` (the last render's return value) |
| `frequency()`, `hz_of_note()` | pitch of a signal from its rising zero crossings; the equal-tempered pitch of a note |
| `Drawing` | what a view drew (`ViewHost::replay` into rectangles, texts, polylines, curves) after `ViewHost::draw`, which also reports the budgets (`host.words()`) |
| `load_tmod(O3M_<ID>_TMOD)` | the packed file as the OS's loader reads it: header checks, CRC, relocation to the slab and SDRAM addresses, descriptor read back (skipped when the cross compiler is missing; `O3_CI=1` makes that a failure) |

Incremental commands (nothing here needs a clean build):

    cmake -S . -B build-os-host -DO3OS_SANITIZE=ON                       # once
    cmake --build build-os-host --target o3m_acid_test o3m_acid_tmod o3m_acid_raw o3m_acid_gen
    ctest --test-dir build-os-host -R acid --output-on-failure            # the unit test, the manifest check, the file read back
    cmake -S . -B build-os-mk66 -DO3OS_PLATFORM=mk66 && cmake --build build-os-mk66 --target o3m_acid_tmod    # the .tmod the cross build makes; prints hot / cold / state sizes

The packer's last line is the number to watch: `hot N B` is what the module takes of the SRAM_L slab (see the sizes in section 3).

## 2. The manifest (`module.json`)

| key | meaning | carried by the file? |
|---|---|---|
| `name` | display name, 1..23 printable ASCII | yes (`o3m_module.name`) |
| `id` | file stem and C prefix: `[a-z][a-z0-9_]*`, at most 31 | the file name |
| `kind` | `instrument`, `effect`, `app` | yes (header and descriptor) |
| `abi` | optional: `"1.0"`, `"1.1"`, `"1.2"`, `"1.3"`, `"1.4"`, `"1.5"`, `"1.6"`, `"1.7"`, `"1.8"`, `"1.9"`, `"1.10"`, `"1.11"`, `"1.12"` or `"1.13"`. An importer (`"kind": "importer"`) is always ABI 1.11, whatever is declared. The generator writes the lowest ABI the module needs (0 with no views and not an app, else 1); `"1.2"` declares a module that ships with a version (section 1, step 6); `"1.3"`, or any `capabilities`, makes it 3, and `audio_tap` (or `"1.4"`) makes it 4; an app that uses the step and edit-group entries declares `"1.5"` and one that uses the instrument-parameter entries `"1.6"` (there is no capability bit for them; declare the lowest minor you need); `export_files` makes it 7; `play_notes` or `"1.10"` makes it 10 (the io table's `get_transport`, `set_own_param`, `play_note`, `stop_note`); `"1.11"` is the importer kind's minor (kind 3, OS-FLASHDIET I1); an instrument with `"pitch": true`, `"pages"` or a parameter's `"labels"` makes it 8 (section 8, ABI 1.8); an effect with `"stereo"` or a `key` parameter, or an instrument or effect with `"presets"`, makes it 12 (section 3b), `"rate": true` makes an instrument 13 (section 8), and a lower `abi` with one of them is refused | yes (the descriptor's minor, repeated in the header) |
| `capabilities` | optional, apps only (ABI 1.3): a list of `midi_in`, `read_project`, `edit_project` (ABI 1.3) and `audio_tap` (ABI 1.4). The matching tables are lent to the app (`docs/guides/app-modules.md` section 3). Pack with `o3m_pack.py --caps` / CMake `CAPS` | yes (header word at byte 40) |
| `description` | one sentence; free text, read by people only | no |
| `state_bytes` | the instance block `init()` gets: zeroed, 16-byte aligned, in SDRAM, 1..262144 | yes; `static_assert(sizeof(State) <= <ID>_STATE_BYTES)` holds the source to it |
| `params[]` | at most 32; `name` (1..15), `min`, `max`, `default`, `type` (`float`, `int`, `bool`, and `key` for an effect's sidechain key: 0..16, section 3b), `unit` (`none`, `percent`, `hz`, `ms`, `db`, `semitones`, `seconds`, `cents`), `curve` (`linear`, `log`: min > 0), `learnable` (default true), `mod` (default false: a modulation target) | yes: `o3m_param` (name, range, default, flags) |
| `stereo` | optional, effects only (ABI 1.12): `"widens"` or `"capable"` (section 3b); the module then passes its stereo render to `<ID>_ENTRY_FX(render_st)` | yes (`render_st` and `fx_flags` of the descriptor) |
| `presets[]` | optional, instruments and effects that have parameters (ABI 1.12): at most 32 of `{"name": "Slow", "values": {"Depth": 80}}`: the name 1..15 printable ASCII (each once), `values` maps parameter names to values (finite, inside the parameter's range, whole for `int`, `bool` and `key`); a parameter a preset does not name takes its default. The generator refuses a name that is no parameter and a value out of range | yes (`presets`, `preset_count`) |
| `rate` | optional, instruments only (ABI 1.13, OS-MODRATE): `true` sets `O3M_FX_RATE` and needs `"abi": "1.13"` (the generator refuses it otherwise, and on any other kind). A module that sets it promises to work at `host->sample_rate` 44100, 22050 and 11025 and at 1..32 frames per call: derive every coefficient and limit from `host->sample_rate` at `init`, no 44100 literal, no absolute-Hz clamp | yes (`fx_flags`) |
| `views[]` | at most 8; `name`, `min_size` and `preferred_size` as `[w, h]` (16..800 by 16..480) | the **number** of views only; names and sizes are emitted as `<ID>_VIEW_<NAME>_MIN_W` etc. for your source |
| `voices` | must be 1: an instance is mono and there is one per instrument slot | design-only (more voices need a later ABI minor) |
| `memory` | `hot_bytes`, `sdram_bytes`: advice | design-only (the packer measures the real sizes) |

The generated C names are the id or the name upper-cased with every run of other characters turned into one `_` (`Env Mod` is `TB303_PARAM_ENV_MOD`, `Op1 Ratio` is `FM_PARAM_OP1_RATIO`); two names that give the same C name are refused.
Parameter `i` is id `i` in `set_param(state, i, value)` and `canvas->params[i]`; a parameter blob stored with an instrument is one little-endian `float32` per parameter in this order, and
the OS keeps it verbatim: **never reorder or remove parameters of a published module** (add at the end, or version the module under a new name). The generator
refuses what the loader would refuse (a default outside its range, a log curve through zero, two parameters whose C names collide ...), with the cause.

**Choosing a parameter's type** (the control is generated, so the type decides what the user sees): there are no list labels unless the module is an instrument or an effect (ABI 1.8 labels; for an effect they make the module ABI 1.12, OS-INSFX-FX2: the Ping-Pong's Sync), so a stepped choice (a waveform, a duty cycle, an algorithm) is an `int`
and shows as a number: say what it means in a view, as the Chip's wave view names the wave and the FM's diagram draws the algorithm. A whole-number parameter has one step an integer, which is how a ratio, a
detune in cents or an algorithm number stay exact (a float cannot be set to exactly 3.0 with 100 steps over 0.5..16). Any parameter with a unit **shows whole numbers**, however the value is stored, so
pick a unit in which whole numbers are fine enough (cents, ms, percent) rather than Hz or seconds with decimals. A plain float with no unit shows only its position in the range, which tells the user nothing:
give every real quantity a unit or an `int`. Anything that must be exactly representable (a ratio of 14) is an `int`.

How the OS builds the standard control of a parameter (app/module_ui.h `describe_module_param`): the name upper-cased and cut to 12 characters on the page; `bool` is an OFF/ON list;
`int`, or any parameter with a unit, shows whole numbers followed by the unit; a plain float with no unit shows its position in its range (0 to 100); a `log` curve is what the encoder
follows; 100 fine steps across a float's range (one step for an `int`), ten times that with Shift.

## 3. The descriptor and the rules

```cpp
#include <o3module.h>
#include "acid_gen.h"
...
extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    ACID_ENTRY_HEAD, sizeof(State), ACID_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, ACID_ENTRY_VIEWS(&kUi),
};
```

- **Float only.** No `double` (the FPU is single precision: a software double is 50 to 130 cycles), no libm, no libc. The OS lends `exp2`, `log2`, `sin`, `cos` in `o3m_host` for
  control-rate maths (they are cold: a few dozen calls at set-up are fine, per-sample calls are not). Views have no host pointer: they use their own short polynomials (the TB-303's
  and the templates' `fexp2` / `flog2`) or read the host from the state.
- **Do not let the compiler call `memset` or `memcpy`**: a struct initialiser with zeros, a big copy, `= {}` on an array, may become one, and a module has no libc (the packer
  refuses an undefined symbol). Fill by loops and field by field; for a curve use `o3m_curve_set`.
- **No writable globals, no statics, no constructors.** Every mutable byte lives in the instance's state block. Constants (tables, strings, descriptors) are read-only and live in SDRAM.
- **`render`, `set_param`, `note_on`, `note_off`** run in the audio interrupt's render context (the engine renders **64-frame blocks**: 1.45 ms at 44.1 kHz, about 261,000 cycles at 180 MHz, and the whole device shares
  them; `O3M_BLOCK_MAX` = 128 is the ABI's ceiling, not what the engine uses): no heap, no I/O, no blocking, bounded loops. **`render` may be given any number of frames from 1 to 64**: a block is split at the sample a note
  starts on, so keep nothing per call (count samples, not calls: a control-rate tick of 32 samples must run at the same samples whatever the split; the Chip and FM tests render the same notes with five block sizes and demand identical bits). `init` and `shutdown` run in the main loop (and may call `host->log`). `render` and what it calls are `O3M_HOT` (placed in SRAM when the OS has room: keep per-sample data in
  registers and the state; do not read big tables from hot code, SDRAM is slow).
- **Output level**: an instrument writes floats with **1.0 = full scale** (the engine scales by 32768, clips at the 16-bit range and counts the clips; NaN becomes silence). Leave headroom: the Chip's narrow
  pulses peak at 1.75 with the mean removed, so its Volume defaults to -12 dB.
- **Notes and polyphony**: an instance is one mono voice and there is one instance per instrument slot (`voices` is 1); play chords on several tracks or slots. A pattern's note is always a new note: the OS
  sends the held note's `note_off` first and then `note_on` with flags 0 (a tracker note on a track replaces the previous one; the 303 would otherwise take it for a slide and never retrigger). A `note_on` that
  overlaps a held note on purpose (a live key pressed while another live key is down, `mod note` without `mod off`; new Community OS behaviour) comes with `O3M_NOTE_LEGATO` and **no `note_off` in between**; a module that does not glide (the Chip, the FM) simply retriggers, and starts the new note's envelopes from the level the old one was
  at (starting from zero would click). `note_off(note)` carries the note being released; a mono voice releases whatever it plays. A note's velocity is 1..127 (96 when the pattern names none).
- **`render`** returns 1 while the module may still sound (a note is held, a tail runs) and 0 when it is silent and idle, which lets the OS skip it until the next note.
- **The CPU governor** (OS-GOV, new Community OS behaviour; `audio/module_governor.h`): the OS times every instance's `render` (the DWT cycle counter, never a number the module declares) and keeps its cost for one voice for a
  whole 64-frame block (the smaller of two blocks in a row, so one long block does not count; the estimate ages by half every 370 ms, idle time included). A note for an instrument that already sounds is a retrigger of its
  one voice and always starts. A note that would START a voice is **dropped** when the predicted block (what the render takes besides the modules, plus the voices that sound, plus this one) passes **90 %** of the 261,120 cycles;
  an instance never measured is assumed to cost 20 % of the block until it has been. An instance that takes more than **50 %** of a block for **4 blocks in a row** is **muted** (alone; its note ends, its last block fades out,
  it is not rendered) until a note arrives after about 1.5 s, or the project is reloaded. So a module that costs more than a few percent a voice gets fewer simultaneous voices, never an underrun: keep `render` cheap
  and bounded (the TB-303 is 16 %). The Instrument page shows the measured cost ("~16% PER VOICE", "UNMEASURED" before a note has sounded) and the engine picker shows it for each loaded module; `diag mod gov` prints the governor's
  state and `diag mod gov thr <50..95>` / `budget <10..100>` move the two limits for a tuning session (runtime only).
- The OS does not clamp parameters: **`set_param` must** (a stored blob can hold any finite number, and a damaged one NaN). Write the clamp so that a NaN falls to the lower bound, `x >= lo ? (x <= hi ? x : hi) : lo`:
  the obvious `x < lo ? lo : (x > hi ? hi : x)` lets a NaN through, and a NaN in a filter coefficient is silence until the next reset. Round the whole-number parameters yourself too (`type: int` only constrains the control).
- **How big**: the hot part of every loaded module shares one SRAM_L slab of **14 KiB** (`platform/mk66/mem_map.h` kModuleSlabSize; a module whose hot part does not fit runs from SDRAM, which is slower by an
  estimated 10 to 30 times, ADR 0002). The TB-303's is 2.3 KB, the Chip's 2.4 KB, the FM's 1.9 KB: aim at 3 KB or less. Inline the per-sample path into `render` (`always_inline` on the helpers, as chip_dsp.h and fm_dsp.h do) so
  that the hot part is that loop and nothing else; `control()`-style code that runs a few times a block may stay in SDRAM. The packer prints the sizes.
- An **instrument** has no input and one mono output (`in` is NULL); an **effect** has one mono input and one mono output (`in` and `out` may be the same buffer), plus, from ABI 1.12, a stereo render
  (section 3b); an **app** makes no sound (`render`, `note_on`, `note_off` may be NULL; it needs at least one view).

## 3b. Effects: stereo, clock, key, presets (ABI 1.12, OS-INSFX-ABI2)

*New Community OS behaviour* (the official firmware has no insert effects and no user sidechain). One additive minor gives an effect four things. **Status:** the descriptor, the loader's checks, the packer, the generator and the template
exist and are tested on the host; the OS does not yet act on them (table in "What is real", row ABI 1.12): write against this section, build and pack, and expect the sound only once the slices named below are in an image.
A module of 1.11 or below never has any of it honoured, and every older module loads unchanged.

- **Stereo** (`"stereo": "widens" | "capable"`; descriptor `render_st` and `fx_flags`). Without `"stereo"` an effect is mono, as before. With it the module has `render_st(state, in_l, in_r, out_l, out_r, frames)` (frames 1..64, the inputs and outputs may be the same
  buffers, the return value is `render`'s: 1 while it may sound, 0 when silent and idle) and a mono `render` too (the loader requires one of every effect).
  - **WIDENS** makes a stereo image from a mono input, so the OS calls `render_st` on every block, with the track's mono block M on both inputs (and the side signal S a stereo voice or an earlier stereo insert brings, as `L = M + S`, `R = M - S`); it writes L and R, and the OS takes back
    `M' = (L + R) / 2` and `S' = (L - R) / 2`. The mono `render` of a WIDENS effect is called by no host of this OS: make it valid (the template's is the mid of `render_st`). A WIDENS insert reserves one of the four stereo side blocks (the cap, OS-INSFX-ABI2 Q4 a): with four of them, new stereo samples play mono.
  - **Stereo-capable** (`"capable"`) keeps its stereo only when it has any: the OS calls `render_st` on a block whose input has S, `render` on every other block, and the module keeps one state for both and makes the switch **click-free** (that is the module's job). It holds no slot.
  - S reaches the MAIN bus only; the sends, CUE, REC and the USB track stems carry M'. A mono insert before a stereo one scales S away by its dry part (a steady mono insert gives none).
  - An effect's parameter may carry `"labels"` (OS-INSFX-FX2: PING-PONG's Sync, 16 note lengths): the generator then makes the module ABI 1.12 and `<ID>_ENTRY_FX` fills the label member of the 1.8 tail (the pitch and page members stay NULL); the parameter page shows the label of the value.
  - `<ID>_ENTRY_FX(render_st)` writes the descriptor's tail (positionally, with the 1.8 members and `importer` as NULL: C++17 has no designated initialisers; in C list them yourself). The generator emits it for a manifest with `"stereo"` or `"presets"`.
- **The clock** (`host->get_clock(&clk)`, every kind but the importer, instruments included). Test `O3M_HOST_HAS(host, get_clock)`: the table is 60 bytes from 1.12. Set `clk.size = sizeof(clk)` first (fill the members one by one: a struct initialiser may become a `memset`, and a module has no libc); `flags` is `O3M_CLOCK_PLAYING`, `O3M_CLOCK_STARTED`
  (a run started in this block) and `O3M_CLOCK_WRAPPED` (the beat went back: a new pattern or a jump); `tempo_x100` is the tempo in force (what plays, half and double time included; the project's while stopped); `beat` is the playhead at **the block's first sample**, in beats of 4 steps from step 0 of the pattern that plays
  (0 while stopped). It returns 0, or 1 when the OS has no sequencer. Use a fallback (the template uses 120 BPM); render context only. An insert runs once a block, so a synced effect re-locks to `beat` at each call and counts samples from there. As built (A3, new Community OS behaviour): the OS reads the sequencer once a block, at the block boundary, from the state the previous block left, so the values are the playhead extrapolated to this block's first sample and a Play, a Stop or a jump shows **one block (1.45 ms) late**. Inside a step the beat is linear in the sample (swing, a Tempo FX and half or double time are already in the step's length), step `n` of the pattern is exactly `n / 4`; a **count-in** holds the start point's beat; `STARTED` is set in the first block of every run (Play, Play over a run, Continue), `WRAPPED` in the first block whose beat is lower than the last block's (the pattern's end or restart, a Break pattern, a jump back; a jump forward is not flagged); stopped it is flags 0, beat 0 and the project's tempo (the Perform tempo and the step rate are not in it). An export gives the same values block by block, from its own sequencer. Under an external clock the beat moves on the master's microstep grid.
- **The sidechain key** (a parameter `"type": "key"`; flag `O3M_PARAM_KEY`; `host->track_level(track)`). A key is an INT parameter, 0 = no key and 1..16 = track 1..16, never learnable (the OS shows `OFF`, `T1`..`T16`); at most two a module; effects only; `min` 0, `max` 16 and `default` 0 may be left out. The effect asks `track_level(n - 1)`
  every block: the linear peak, 0..1, of that track's mono block after its inserts and **before the fader**, so a muted or faded key track still keys (the OS keeps its voices rendering while it is a key, at a CPU cost); the value is this block's when that track's strip already ran, else the previous block's (**at most one block, 1.45 ms, late**),
  so an attack under about 3 ms is not exact. It is 0 for a silent track, a bad index, and a track no KEY parameter names. **How it is measured (A6):** the OS measures a keyed track in its insert slot B (the host's tap when the slot is empty or holds a bypassed effect, the effect itself when it holds one, so the
  key is the output of slot B); a track whose strip does not run in a block (nothing feeds it) reads 0 from the next block; the master insert always reads this block's levels. **A key never names the effect's own track**: the OS treats that value as OFF, because the effect would read its own post-insert output back (a feedback loop that pumps).
- **Idle effects are not called.** The OS skips an effect whose input is silent and which returned 0, and a track with no voice at all never reaches its inserts (its strip settles): in both cases the module's `render` is not called, so its state (an envelope, a smoother) **freezes across the gap** — no `render`, no `note_on` reaches an insert effect, and without an ABI change no module can see that gap. What a module can do: act on the first silent block it does see (the OS calls it once before skipping) — the ducker template resets its key state (`env = 0`, `gain = 1`) there, so a note after a silent gap opens at unity instead of at the frozen gain (`tests/audio_insert_key_test.cpp`, the idle-ducker case).
- **Factory presets** (`"presets"`; descriptor `presets`, `preset_count`). Instruments and effects that have parameters, at most 32, names 1..15 printable characters. A preset sets **every** parameter: `values[count]` in parameter order, the parameters past `count` take their default
  (the generator fills a gap before the last named one with the default too). The OS applies one through `set_param` and writes the parameter blob; an app and an importer carry none (`BadDescriptor`).
- **What reads what.** The loader checks all of it when the module declares 1.12 (`render_st` is code inside the image and an effect's, WIDENS needs `render_st`, a KEY is an effect's INT 0..16 with no label, at most two, every preset's name and values); the packer repeats the descriptor checks, and `o3m_gen.py verify` compares the built file's stereo shape and presets with the manifest.
  Slices: the stereo track adapter and the shared S block are A4, the cap A5, the clock A3, the key A6, the factory-preset list and F5 PRESET A8 and A9 (`docs/specs/OS-INSFX-ABI2.md`).
- **Template:** `../../modules/templates/effect_stereo/` (`o3m_gen.py new --kind effect_stereo --id <id> --out DIR`): a WIDENS auto-pan, `Sync`, `Beats`, `Cycle` (ms), `Depth`, synced through `get_clock` with a 120 BPM fallback, two presets. It is float-only with no libm, its `render_st` is the only `O3M_HOT` code, and
  its round trip (manifest, packed file, loader) is `tests/module_gen_test.cpp` and `tests/o3m_gen_scaffold_test.py`; its maths has no host oracle test (the OS-side adapter is tested in `tests/audio_insert_test.cpp`, `insert host: stereo:`).
- **Template, sidechain:** `../../modules/templates/effect_key/` (`o3m_gen.py new --kind effect_key --id <id> --out DIR`): a stereo-capable ducker, `Depth`, `Key` (a `"type": "key"` parameter), `Attack`, `Release` (ms): the gain is `1 - Depth x level` through an attack and a release at block rate, ramped across the block, and it shows
  the pattern for the switch between `render` and `render_st` (one gain, one state: nothing to click). `track_level` is found with `O3M_HOST_HAS`, so an OS without it gives a ducker that never ducks. Its round trip is `tests/module_gen_test.cpp` and `tests/o3m_gen_scaffold_test.py`; the OS side is `tests/audio_insert_key_test.cpp`.

## 4. Views and the draw API (ABI 1.1)

The OS owns every page; a module draws **views** inside it through one restricted, versioned API. A view is a rectangle the page gives it: `view_draw(state, view_index, canvas)` paints
into `canvas` (width x height pixels, top-left (0, 0), at least 16 x 16, at most 800 x 480) and everything outside is cut off. It is called from the main loop whenever its page is
drawn (the Instrument page when it changes, an app's page 30 times a second). It receives the **current value of every parameter** in `canvas->params` (so the TB-303's curve follows the
cutoff as it turns): draw from those, not from the state, which the audio thread may be changing (the state pointer of an instrument's view is read-only by contract).

`canvas->draw` is the `o3m_draw_api` (the same table as `o3m_host.draw`):

| call | draws |
|---|---|
| `rect(c, x, y, w, h, color)` | a filled rectangle |
| `outline(c, x, y, w, h, color)` | a 1 px border inside the rectangle |
| `text(c, x, y, s, face, color)` | text with its top-left at (x, y), at most 64 characters; the faces are the theme's (`O3M_FACE_DISPLAY/DATA/LABEL/MICRO`), fixed pitch; characters not wholly inside the canvas are dropped |
| `text_width(c, s, face)` | pixels (not a draw call) |
| `polyline(c, xy, points, color)` | a 1 px line through 2..128 int16 (x, y) pairs; cut by the canvas |
| `curve(c, &curve)` | the `ui::plot` curve primitive: a series (`x` may be NULL: evenly spaced) mapped through a linear, log-frequency or dB axis, simplified to what a pixel column can show, with optional frame, grid, tick labels (`O3M_CURVE_AXES`: the data area is the canvas inset by `O3M_CURVE_MARGIN_*`) and translucent fill (`O3M_CURVE_FILL`); fill the struct with `o3m_curve_set` |

**Colours are the theme's roles** (`O3M_COLOR_TEXT`, `_INSTR`, `_ACCENT`, `_SEL`, `_PANE` ...): a module never sees an RGB value, so every theme draws every view. There is no raw display
list and no register access.

**What a call costs in display-list words** (the 700 are shared by all the views the page draws, and the page's own words come out of them; `module/view.h` `word_cost_*`): `rect` 4, `outline` 12, `text` characters + 2,
`polyline` points + 10, `curve` 2 x points + 16 **+ 200 with `O3M_CURVE_AXES`**. So a curve with axes is a view's whole budget in practice: **the TB-303's filter view costs 624 and its envelope view 312, 936 together: more than the page's 700 (open item: measured by OS-012c with the draw recorder; the second view would be refused for want of words and the module quarantined; see section 10)**.
To plot several series on one set of axes draw your own frame (an `outline`, a few `rect` rules, two `text` labels) and the series as `polyline`s, as the FM's envelope view does (four envelopes, about 270 words); keep the
views of a module to about 450 words in all so that the page's own words fit beside them (the Chip's two views cost 380 to 470, the FM's 470).

**Budgets** (enforced by `module/view.h`; a module cannot opt out): per view and frame **96 draw calls**, **256 points** in polylines and curves (a polyline at most 128), **384 characters**
of text, and **4 ms** from the call into `view_draw` to its return (checked at every draw call and when it returns); and **700 estimated display-list words for all the views of one page
together** (the frame buffer is one, and the page's own words are in it too; the Instrument page draws all of a module's views side by side, in order, each given what the earlier ones left, and **a view that is given less than it needs counts as over budget and quarantines the module**: so the sum of your views' words must stay under 700). The calls of a view are recorded and replayed to the renderer only when the
view came back inside its budgets: **a view that goes over is blanked** (nothing of its frame is drawn), its module's views are blocked for the rest of the boot and the module is
**quarantined** (not loaded again until a reset; its audio keeps running until the project changes). A call with an argument it cannot use (a NULL string, a colour out of range, a zero size)
is ignored and counts as a call. A module that neither returns nor draws cannot be stopped from here: the platform's watchdog (an open item) and the crash record catch it, and the next boot
quarantines the module that was running.

## 5. Apps

An app owns the body of an OS page (**MODULES** in the Navigator lists `/modules/apps`; F1 launches the one under the cursor). Entry points (`o3m_ui`): `enter(state)` when the page opens,
`input(state, event)`, `view_draw(state, 0, canvas)` (view 0 is the page body), `exit(state)` when it closes; `init` runs at launch and `shutdown` after `exit`. The page is the OS's: the
status strip, the soft-key labels, the theme, and the **exit chord F7 + F8, which always works** (since ABI 1.9 F8 alone is BACK and Play is the transport's: the OS acts on them and the app never sees either) whatever the module does (a stopped or hung app included: it is not called to say
goodbye). The app cannot swallow the chord but it does see half of it: the F7 press arrives as an ordinary button press just before the page closes (the OS reads the raw events itself) and its release never does; F8 never arrives (it is the page's BACK).

**Input.** Every button, pad and encoder event reaches `input()` as an `o3m_ui_event` {kind, id, value, mods}: `O3M_EV_BUTTON` (id `O3M_BTN_*`, the hardware's button numbers; value 1 down, 0 up),
`O3M_EV_PAD` (id 0..47, value the velocity, 0 on release), `O3M_EV_ENCODER` (value the signed detents); `mods` has `O3M_MOD_SHIFT` while Shift is held. A release is passed on only if the app was given the press
(the key that launched it does not leak a release). **What the OS keeps**: F8 and Play (BACK and the transport: neither reaches `input()`), Shift + Config (the OS's help page for the app's page: the app is not given the Config press, and while the help page is up it is given no key, pad or encoder turn; a release of something it was given the press of still arrives), the exit chord (see above: closing the page is the OS's, though the F7 press reaches `input()`), and the Shift layer of
the page's own controls (Shift + Up / Down, Shift + encoder, Shift + Delete) while the app declares at least one `o3m_param`: they edit the app's parameters, and the app does not hear them (an app that declares none gets all of them, OS-APPUX S2; the shipped apps draw their own rows in the one grammar of `modules/apps/common/o3m_edit.h`: Up / Down the row, Left / Right the page, the encoder and Shift + the encoder the value, Delete the default). `input()`, `enter()` and `set_param()` are held to the same 4 ms budget as drawing.

**Parameters.** An app's parameters are standard OS controls on its page (value, bar, unit); it reads their values from `canvas->params` when it draws and hears them in `set_param()`. Learn,
undo and the Perform layer do not work on them yet: they need a place to be saved, which is a persistent-format decision (section 10). An app may draw a control of its own and handle its
input, but that control gets no Learn or undo; parameters are the recommended way.

## 6. Instruments in the OS

A project stores a Module instrument as its kind, the module's file stem (the project's module table, with the ABI the project was saved with) and the parameter blob (the floats above): the
existing native format, unchanged. Opening a project loads the modules its instruments name (a slice of the main loop at a time) and binds an instance to each instrument; a module that is
missing, refused or quarantined leaves its instruments **muted** (they stay in the project and say which file is missing). The Instrument page of a module instrument is the generated page
(eight parameters a page, F2 to F5 the pages, F1 ENGINE) with the module's views below: MIDI Learn (FX1 + FX2), undo (F8), saving with the project work on every parameter with no module
code; a parameter marked `learnable: false` is not offered to Learn. **F1 ENGINE** opens the picker: the instrument modules on the card, and the ones the project names that the card does not have,
greyed. Choosing one on a free or sample slot writes the instrument and its default parameters (one undo unit) and binds an instance. A slot that already has an instance cannot change
engine without reloading the project (the voices have no unbind yet), and the picker says so; undoing a pick restores the instrument but leaves the instance bound until the project is reloaded.

## 7. Worked example: the TB-303

`modules/tb303/` is `module.json` (fourteen parameters with units and curves, two views), `tb303_gen.h` (generated, committed: the ctest `o3m_gen_check_tb303` finds a stale one), `tb303_dsp.h`
(the voice: a float, block-based port of Open303, MIT, tested against the original) and `tb303.cpp` (the glue and the views). Its two views are drawn from the parameter values:

- **Filter**: the magnitude response of a four-pole ladder at the cutoff the knob says, and at the cutoff the envelope opens it to (`Env Mod`, with Open303's measured mapping from
  `tb303_dsp.h`), for the Resonance; the frequencies crowd round the cutoff so that a resonant peak is not stepped over.
- **Envelope**: the cutoff against the time after a note, from `Decay`, `Env Mod` and `Cutoff`.

They illustrate the voice's own mapping with a linear ladder's shape (the real filter adds its soft clip and the TB_303 fit); the plots do not claim to be the voice's exact response. Sizes
(`modules/tb303/README.md`): the file is about 7.4 KB (hot 2.3 KB, cold 4.9 KB), the instance 380 B.

## 7b. More worked examples: the Chip and the FM (OS-012c)

Two instruments written from scratch by following this guide (no third-party code, so no licence gate to pass: `THIRD_PARTY.md` says so), each a directory with `module.json`, a committed `*_gen.h`, a DSP header (`chip_dsp.h`,
`fm_dsp.h`), the glue and views (`chip.cpp`, `fm.cpp`), `tests/*_test.cpp` and a README with the parameter table, the sizes and the cycle estimate.

- `modules/chip/`: pulse (12.5 / 25 / 50 / 75 %, PolyBLEP unless Raw Edges), triangle, 15-bit LFSR noise (long and short), optional 4-bit volume steps, a linear envelope, a three-step arpeggio, a sweep and a vibrato. Views: the
  waveform with its duty and the envelope (a curve with axes). The tests measure duty, pitch, aliasing, LFSR periods, envelope timing, steps, arpeggio, sweep, vibrato, block-size independence and hostile parameters.
- `modules/fm/`: four sine operators, eight algorithms, per-operator ratio / detune / level / ADSR, operator-4 feedback; 32 parameters in four pages of eight (an operator's seven and one global each). Views: the algorithm diagram (rectangles,
  text and lines, from the module's own table) and the four envelopes (polylines in a frame). The oracle is an independent double-precision model with its own transcription of the eight routings, compared sample by sample, and the
  sidebands against the Bessel functions of the index.

## 8. Versioning

Two numbers are not the same thing. The **ABI version** (1.2 here) says what the module and the OS must agree on: the descriptor, the services, the draw API, the file format. The **module version**
(`X.Y.Z`, ABI 1.2) is the author's own number for the module; the OS uses it for one decision only, "is this card copy newer than the built-in module of the same name" (section 1, step 6).

The file header (ABI 1.8, unchanged since 1.3; 64 bytes, little endian, `o3module.h` has it as `o3m_file_header`):

| offset | size | field | |
|---|---|---|---|
| 0 | 4 | `magic` | `"O3MD"` (0x444D334F) |
| 4 | 2 | `header_size` | 64 |
| 6 | 2 | `target` | 1: the Cortex-M4F |
| 8 | 1 | `kind` | 1 instrument, 2 effect, 3 importer (1.11, OS-FLASHDIET I1), 4 app (1.1) |
| 9 | 1 | `abi_major` | 1 |
| 10 | 1 | `abi_minor` | 0..12: the ABI the module was built against (equal to its descriptor's); kind 3 exists from 1.11 only |
| 11 | 1 | `flags` | 0 |
| 12 | 4 | `hot_size` | multiple of 4, within `image_size` |
| 16 | 4 | `image_size` | multiple of 4, at most 256 KiB |
| 20 | 4 | `desc_offset` | image offset of `o3m_entry` |
| 24 | 4 | `state_bytes` | the descriptor's, repeated |
| 28 | 4 | `reloc_count` | entries in the relocation table |
| 32 | 2 | `param_count` | the descriptor's, repeated |
| 34 | 2 | `view_count` | the descriptor's, repeated |
| **36** | **4** | **`module_version`** | **1.2: `O3M_VERSION(major, minor, patch)` = major << 16 \| minor << 8 \| patch; 0 = unversioned. Reserved (0) before 1.2: a 1.0 or 1.1 file with a value here is refused** |
| **40** | **4** | **`capabilities`** | **1.3, apps only: `O3M_CAP_*` mask. The six values are FROZEN published ABI: midi_in 1, read_project 2, audio_tap 4, edit_project 8, export_files 16, midi_out 32. An OS of 1.3 grants 1, 2 and 8, an OS of 1.4 also 4 (audio_tap, in a file of ABI 1.4: in a 1.3 file that bit is a reserved field), an OS of 1.7 also 16 (export_files, in a file of ABI 1.7: in an older file that bit is a reserved field); a file that asks for a bit its OS does not grant (32; 16 on an OS below 1.7) is refused at load with a message naming it ("NEEDS MIDI OUT (NEWER OS)"), never run with half its tables. A bit above 32 is a reserved field. Reserved (0) before 1.3, and for an instrument or effect: a file with a value there is refused** |
| **44** | **4** | **`reserved2[1]`** | **1.11, kind 3 only: the importer layout tag** (the packer writes it: a hash of the OS-side import-facing model layout the module was built against; `model/import_layout.h` and `model/import_layout_tag.txt` on the OS, read by `tools/o3m_pack.py` and `tools/o3m_gen.py`). The OS refuses a kind 3 file whose word is not its own tag (`LayoutMismatch`, quarantined). Reserved (0) in every other file: an instrument, an effect, an app, or a kind 3 file of a minor below 1.11, with a value here is refused |
| 48 | 4 | `image_crc` | CRC-32 (zlib) of the whole file with this field read as zero: the version is under it |
| 52 | 12 | `reserved3` | 0 |

A module loads when its ABI **major** equals the OS's and its **minor** is not above the OS's. Additions only (a flag, a trailing member of `o3m_host`, a new kind, a function at the end of the
draw API) bump the minor; anything else bumps the major. ABI 1.0 (OS-012a) modules still load and run unchanged on 1.1: their reserved view table is validated and never called, and the
parameter flags 1.1 added are zero in them (zero meaning the 1.0 behaviour). A module with no views and no 1.1-only kind declares minor 0 (the generator does this) and so also loads on an
OS of ABI 1.0. Check `o3m_draw_api.version` and `size`, and `o3m_host.size`, before using a member a later version added.

ABI 1.3 (OS-H1, ADR 0014) gives an app's header the capability word (byte 40) and appends `read`, `edit` and `io` to `o3m_host`; `init()` keeps its signature. Every 1.0, 1.1 and 1.2 file loads and runs
unchanged on an OS of 1.3: its capability word is 0 and an app that declares less than 1.3 is lent none of the new tables. ABI minors are claimed in merge order (1.3 is H1's). A table is NULL without its bit (`read`, `edit`, `io.midi_read` / `io.midi_lost`, the `io.tap_*` and the `io.export_*` of 1.7; later `midi_send`): test every table and member for NULL. An app that declares 1.2 is lent the 1.2 host table (`size` ends at `draw`, `abi_minor` 2).

ABI 1.4 (OS-H2a, ADR 0014 amendment E) grants the audio_tap capability and appends four members to the io table (`tap_select`, `tap_latest`, `tap_stream`, `tap_levels`; NULL without the bit): the header is unchanged. A module that uses them declares ABI 1.4 and sets the bit; an OS of 1.3 refuses the file (its minor is above its own), and in a file of 1.3 or below the bit is a reserved field. Every 1.0..1.3 module loads and runs unchanged. The tap is one mono int16 stream, the input bus or the master output, 44.1 kHz; the frames and the cursors are yours to use from `view_draw`:

```c
/* once a frame, in view_draw */
if (io->tap_select != NULL) {                         /* NULL: no audio_tap bit, or a build with no tap */
  if (!selected) { selected = io->tap_select(O3M_TAP_MASTER) == 0; }
  o3m_tap_levels lv;                                  /* peak, rms (linear, 1.0 = full scale), frames, clip_blocks since the last call */
  lv.size = sizeof lv;                                /* (field by field: a partial initialiser may call memset, which a module has no libc for) */
  io->tap_levels(&lv);
  alignas(4) static int16_t win[1024];                /* 4-byte aligned, an even number of frames */
  uint32_t n = io->tap_latest(win, 1024);             /* the newest 1024 frames, for a scope or a spectrum */
}
```

`tap_stream` is the gapless variant (everything new since the last call, the oldest dropped and counted when you were away more than the ring holds, 93 ms), for a tuner that decimates to 11 kHz. The OS stores the samples only once an app reads them (OS-BUG-TAPWRITE; `tap_levels` never needs them): the first `tap_latest` or `tap_stream` call returns 0 and there is no history before it, the real frames begin two blocks (2.9 ms) later, so poll them every frame and treat 0 as "not yet". Section 3.4 of `docs/guides/app-modules.md` has the table of calls and the cost.

ABI 1.5 (OS-H3A, ADR 0014 amendment G) lets an app edit the project. It appends the step POD `o3m_step` (8 bytes: note, instr, vol, fx1 type/value, fx2 type/value, reserved), `pattern_info` and `get_steps` to the read table, `edit_begin`, `edit_end`, `set_step` and `set_steps` to the edit table, and `block_tracks` to the launch context; the header and the capability bits are unchanged (the tables are the ones `read_project` and `edit_project` already earn). An app that uses them declares ABI 1.5; an OS of 1.4 refuses the file (NEEDS NEWER OS). Every 1.0..1.4 app loads and runs unchanged: every published member is where it was (`o3module.h` ends with the byte offsets of the 32-bit target as compile-time assertions, which every module build checks), and each table's `size` says which entries exist. A write names the fields it changes with a mask and the writes of a group apply together as one undo unit:

```c
const o3m_edit_api* e = host->edit;                 /* NULL without edit_project */
if (e != NULL && O3M_TABLE_HAS(e, set_step) && e->edit_begin != NULL) {   /* the member you call; absent on an OS of 1.4 */
  o3m_step s = {61, 3, 100, 0, 0, 0, 0, 0};          /* the model's note 61 (C-5 in the Pattern's names), instrument 3, volume 100 */
  if (e->edit_begin() == O3M_EDIT_OK) {              /* from input() only; one group per call */
    (void)e->set_step(pattern, track, row, &s, O3M_STEP_NOTE | O3M_STEP_INSTR | O3M_STEP_VOL);  /* FX stay as they were */
    if (e->edit_end() == O3M_EDIT_LIMIT) { /* too big: the whole group was dropped, the project is as it was */ }
  }
}
```

The limits are 4,096 write calls and 2,048 steps per group; a group over either is dropped whole at `edit_end` (LIMIT). A group the app leaves open when its call returns is closed and applied by the OS. Section 3.3 of `docs/guides/app-modules.md` has the table of calls, the codes and the rules.

ABI 1.6 (OS-H3B, ADR 0014 amendment H) lets an app read and write the parameters of a sample or module instrument. It appends `instrument_info`, `get_param_desc` and `get_instrument_param` to the read table, `set_instrument_param` to the edit table, and the PODs `o3m_instrument_info` (16 bytes) and `o3m_param_desc` (40 bytes: `size`, `min`, `max`, `def`, `id`, `flags`, `group`, `name[16]`); nothing older moves, no capability bit is added. `set_instrument_param` joins the open edit group (one undo unit; UNDO restores the exact float bits), writes only parameters whose descriptor carries `O3M_PD_APP_WRITABLE` (the sound ones; a sample's engine, source page, envelope ENABLEs and modulation SOURCEs are not) and answers `O3M_EDIT_RANGE` for anything else, a value outside `min`..`max` and NaN; nothing is clamped for you. An app of 1.2..1.5 runs unchanged on an OS of 1.6, and an app that only uses steps still declares 1.5:

```c
o3m_instrument_info info = {sizeof info, 0, 0, 0};
const o3m_read_api* r = host->read;
const o3m_edit_api* e = host->edit;
if (r != NULL && e != NULL && O3M_TABLE_HAS(r, get_instrument_param) && O3M_TABLE_HAS(e, set_instrument_param) &&
    r->instrument_info(instrument, &info) == O3M_EDIT_OK && (info.kind == O3M_INSTRUMENT_SAMPLE || info.kind == O3M_INSTRUMENT_MODULE)) {
  if (e->edit_begin() == O3M_EDIT_OK) {                 /* from input() only; one group per call: every parameter below is one undo unit */
    for (uint32_t i = 0; i < info.param_count; ++i) {
      o3m_param_desc d = {sizeof d};
      float v;
      if (r->get_param_desc(instrument, (uint16_t)i, &d) == O3M_EDIT_OK && (d.flags & O3M_PD_APP_WRITABLE) && r->get_instrument_param(instrument, d.id, &v) == O3M_EDIT_OK) {
        (void)e->set_instrument_param(instrument, d.id, v < d.max ? v + 0.01f * (d.max - d.min) : d.min);   /* inside min..max: nothing is clamped for you */
      }
    }
    (void)e->edit_end();
  }
}
```

Section 3.2 and 3.3 of `docs/guides/app-modules.md` have the ids, the groups, the flags and the APP_WRITABLE rule.

ABI 1.7 (OS-H4, ADR 0014 slice H4, amendment A; apps only) grants `O3M_CAP_EXPORT_FILES` (16) and is strictly additive: the io table grows four members at its end (`export_open`, `export_write`, `export_close`, `export_status`, offsets 36, 40, 44 and 48, size 52; NULL without the bit and absent when `size` is below their offset), and `o3m_instrument_info` grows `midi_channel` (offset 16, size 20; a 1.6 app's 16-byte struct gets the first four members). Nothing older moved: the offsets of every 1.2 .. 1.6 member are pinned by a literal copy of that layout in `tests/module_edit_test.cpp`, and a 1.6 app loads and runs unchanged. An app that asks for the bit declares ABI 1.7 (`module.json` `"capabilities": ["export_files"]` makes the generator write it); an OS of 1.6 refuses that file by its minor. The calls never block, write only new files into `AppExport/<app>/` with an extension from `mid`, `syx`, `txt`, `csv`, `json`, and the first launch of such an app asks the user to confirm. The codes, the caps and the BUSY rule are in `docs/guides/app-modules.md` section 3.7:

ABI 1.10 (OS-APPUX S3; apps only) is strictly additive: the io table grows four members at its end and the header gains one capability bit; `o3m_ui`, the descriptor and the host table do not grow. `get_transport(o3m_transport* out)` (every app of 1.10; set `out->size` first) copies `{playing, pattern, order, row, ustep, tempo_x100, count_in, queued}` as the sequencer published them; it reveals nothing the project does not (like `get_tempo`). `set_own_param(index, value)` (every app of 1.10) keeps the value of your declared row `index` with the project and hands it back to `set_param()` before `enter()` at the next launch; it does NOT call your `set_param()` (you hold the value), the OS clamps it to the row's range and rounds an INT row, `O3M_OWN_RANGE` for an index past your rows or a NaN. A launch from Pattern F7 TOOLS starts at the cursor (an app keeps its settings, not its target); the project keeps the rows of up to twelve apps, keyed by the file stem and the ABI major. Declare every row you want kept as an `o3m_param` (`module.json` `params`; the 32-row cap holds), draw them yourself with `o3edit::draw_rows` and answer the grammar with `o3edit::grammar()` (`modules/apps/common/o3m_edit.h`), and never reset a declared value in `enter()`. `play_note(instrument, note, velocity)` and `stop_note(instrument)` need the capability `play_notes` (64): the notes go through the project's instruments on the live-note path like a pad (a MIDI instrument is not routed), at most four are held and eight calls act a frame (`O3M_PLAY_LIMIT` beyond), and every note ends when your page closes. An app of 1.10 gets no OS parameter column and receives every Shift key; a screen button on any app page leaves the app (it is never delivered). An OS of 1.9 refuses a 1.10 file (minor above its own); bit 64 is a reserved field below 1.10.

```c
/* io = host->io, tested once in init(): NULL without export_files, and absent on an OS below 1.7 */
if (io != NULL && O3M_TABLE_HAS(io, export_status) && io->export_open != NULL && io->export_open("notes", "txt") == O3M_EXPORT_OK) {
  /* in view_draw() or input(), once a frame: the ring drains in the main loop between frames, so BUSY is retried on the NEXT frame with the same bytes (never in a loop) */
  if (io->export_write(text, text_len) == O3M_EXPORT_OK) {      /* text_len <= O3M_EXPORT_RING_BYTES */
    (void)io->export_close();                                    /* then poll io->export_status(name, sizeof name) once a frame until DONE or FAILED */
  }
}
```


ABI 1.11 (OS-FLASHDIET I1) publishes **the importer kind (kind 3), for first-party modules only** — the modules the OS itself ships, compiled against its own model headers. It is strictly additive: the descriptor grows one member at its end, `importer` (offset 80; the descriptor is 84 bytes from 1.11, `O3M_MODULE_SIZE(11)`), and the header's word 44 becomes the importer layout tag for kind 3 only. Every 1.0..1.10 file loads and runs unchanged: its descriptor is 64 or 80 bytes as its minor says and its word 44 is zero (a non-zero word 44 in any other kind is refused as a reserved field, at every minor).

- **The importer table** (`o3m_importer`, eight words, pointed to by `o3m_entry.importer`): `size`, the `formats` list (1..8 entries, each the file extension it serves, e.g. `"mod"`, `"s3m"`, `"med"`), `format_count`, and five calls: `begin(state, file, format, model_arena, options, options_size)` (start a job on the opened file of that format — `file` is the OS's read window over it, an `o3m_file_window`; `model_arena` is the OS's own arena, which the first-party module writes through the model headers it was built against, under the word 44 lock; `options` is the OS's import-options blob, the members your build knows), `step(state, budget)` (pump it; returns `O3M_IMPORT_RUNNING` / `_DONE` / `_FAILED`; main loop only, never blocking), `read_pcm(state, slot, first, frames, dst)` (the PCM of an imported instrument, mono int16), `report(state, out, size)` (copy the import's report out, at most `size` bytes; the shape is what both sides compiled against under the lock) and `close(state)` (the PCM phase is over; release the window). The loader checks the table whole: its size, the format list inside the image with printable extensions, and every call a Thumb function inside the image (a missing function refuses the file). An importer has no parameters, no views and no sound entries (`render`, `set_param`, `note_on`, `note_off` may be NULL, as an app's); it never binds to a track and does not count against the OS's module slots. (I2 refined the table within 1.11 before any release shipped a kind 3 file: `begin` grew its arguments and `report` / `close` were appended after `read_pcm`.)
- **The version lock (word 44).** An importer is compiled against the OS's own model headers and writes the project directly through the arena pointer `begin` is given (the Project mutators and the Pattern / Step / Instrument accessors are inline: nothing goes through a host call), so the bytes it writes are laid out as the OS's model structs are. The packer writes the tag of the layout the module was built against into word 44 — a hash over `ModelArena`, `Project` (every member's offset) and the structs the importer writes field by field (`TrackSlot`, `Pattern`, `Step`, `Instrument` and its `SamplerParams`), plus a manual version (`model/import_layout.h`; the value lives in `model/import_layout_tag.txt`, read by `tools/o3m_pack.py` and `tools/o3m_gen.py`, held to `model::kImportLayoutTag` by ctest `o3m_layout_tag_pin`) — and the OS refuses a kind 3 file whose word is not its own tag with `LayoutMismatch` (a validation failure: quarantined for the boot). The tag changes when a field of any of those structs moves (`tests/module_importer_test.cpp` checks it with copies of the structs). A kind 3 file that declares a minor below 1.11 is refused as an unknown kind.
- The loader reads importers from `/modules/importers/<stem>.tmod` only for the importer scan (the OS's own import flow); every other load (a project's module refs, `diag mod load`) skips that directory, so a project that names an importer's stem never loads the importer. The set holds importers apart from the playable modules. Worked example: `../../modules/importers/tracker/` (kind 3, ABI 1.11, serving mod / s3m / med); tests `tests/module_importer_test.cpp`, the packer's self-test, `o3m_gen.py verify` (the tag is checked there too).

ABI 1.12 (OS-INSFX-ABI2; effects, and factory presets for instruments too) is strictly additive. **The descriptor grows four members after `importer`** (offsets 84, 88, 92, 96; 100 bytes from 1.12, `O3M_MODULE_SIZE(12)` is `sizeof(o3m_module)`, and the macro's 1.11 branch is the 84): `render_st` (the stereo render, NULL = a mono effect), `fx_flags` (`O3M_FX_WIDENS`, 1) and `presets` / `preset_count`
(`o3m_preset { name, values, count }`, 12 bytes, at most `O3M_PRESETS_MAX` 32, names up to `O3M_PRESET_NAME_MAX` 16 with the NUL). **A parameter flag** `O3M_PARAM_KEY` (32) marks the sidechain key. **`o3m_host` grows two members** at its end, `get_clock` (offset 52) and `track_level` (56): 60 bytes, found with `O3M_HOST_HAS`; the importer's own table stays 52.
The members are read, and checked, only when the module declares 1.12 (in C++ they default to NULL / 0; in C list them: an effect's `importer` is NULL, `<ID>_ENTRY_FX` does it). An app and an importer carry them zero. **Every 1.0..1.11 module loads and runs bit for bit as before**: its descriptor is 64, 80 or 84 bytes as its minor says and the new members are never read; an OS of 1.11 refuses a 1.12 file
(`NEEDS NEWER OS`, the minor rule). A module that can live without the clock or the key may declare a lower minor and test `O3M_HOST_HAS`; one that has `render_st`, presets or a KEY parameter declares 1.12 (the generator writes it). The meaning of each member is section 3b; the layout asserts are `module_1_12`, `host_1_12`, `preset_pod` and `clock_pod` in `o3module.h`.

ABI 1.13 (OS-MODRATE S2; instruments) is strictly additive and the descriptor stays 100 bytes. **One bit of the existing `fx_flags` word**: `O3M_FX_RATE` (2), valid for an instrument that declares 1.13 or later (the WIDENS bit stays an effect's; the loader and `o3m_pack.py` apply one rule per kind and minor: any other bit, or RATE on an effect or on a 1.12 instrument, is refused). A module that sets it promises to be correct at `host->sample_rate` 44100, 22050 or 11025 and at any `frames` from 1 to 32 (the OS calls `render` with `ceil(frames / R)` frames, and a note that starts mid-block still starts on its own frame). The OS owns the rest: it changes the RATE only at idle (shutdown, `init` with a host table whose `sample_rate` is the reduced one, the parameter blob applied again), never in `render`, and the three host tables (`host`, and one per reduced rate) have stable addresses. FULL is the engine's own path, bit for bit. Effects are untouched. Every 1.0..1.12 module loads and runs as before; an OS of 1.12 refuses a 1.13 file (`NEEDS NEWER OS`).

ABI 1.2 (OS-MODVER) changed the file header only (the word at byte 36 above): the descriptor, `o3m_host` and the draw API are 1.1's. Every 1.0 and 1.1 file still loads on an OS of 1.2 and reads as
unversioned. A file that carries a version needs an OS of 1.2 (an OS of 1.1 refuses its ABI minor). The project format is untouched: a project's module table still records the stem and the
ABI major.minor the module was saved with, not its version.

ABI 1.8 (OS-MODPITCH + OS-INSTUI S10; instruments; one minor for two features, ADR 0002 amendment 5) is additive and sizes the descriptor PER MINOR:

- **The descriptor is 64 bytes up to 1.7 and 80 from 1.8.** `struct_size` must be `O3M_MODULE_SIZE(minor)` for the minor the module declares; the loader refuses any other. The generated `<ID>_ENTRY_HEAD` writes it: never write `sizeof(o3m_module)` by hand for a module that declares less than 1.8 (an old minor with the new size is refused on purpose; existing binaries are unaffected). A real 1.7 `.tmod` loads and plays exactly as before; the OS reads nothing past byte 64 of it.
- **Four members at the end** (offsets 64, 68, 72, 76): `pitch`, `pages`, `page_count`, `labels`. In C++ they default to NULL / 0, so a descriptor written as the first 16 members still builds under `-Wextra`. **In C they do not**: a C module rebuilt against this header with the first 16 initialisers only now trips `-Wmissing-field-initializers` under `-Wextra` (`-Werror` makes it an error); list the four members (`NULL, NULL, 0, NULL`, or `<ID>_ENTRY_TAIL(NULL)`) or give the descriptor designated initialisers. `<ID>_ENTRY_TAIL(pitch_fn)` writes them for a manifest that asks.
- **`pitch(state, semitones, per_frame)`** (manifest `"pitch": true`; use it or leave it NULL: a module that ignores the FX stays valid). The host owns the Slide (41 / 42) and Glide (19) FX and ramps; the module follows a number. From the next frame the offset from the note is `semitones` and it moves by `per_frame` each frame until the next `pitch()` or `note_on()` (a module may ignore `per_frame`: a staircase of one block). `|semitones|` and `|per_frame|` are at most 128 and finite; clamp your own frequency anyway. `note_on()` puts the offset back to 0. The call is made in the render context, once a block while a ramp moves (plus one to settle and one per FX event or note), none when nothing is held.
- **`O3M_NOTE_RAMP` (2)**, sent only to a module that exports `pitch`. A Slide or Glide note on a module is legato, as a TB-303 slide: the host does not release the held note; it sends `note_on(note, O3M_NOTE_LEGATO | O3M_NOTE_RAMP)` and then `pitch()` (RAMP alone when nothing sounds). On `RAMP` keep your envelope when `LEGATO` is also set, put your base pitch on `note` at once, apply no portamento of your own and follow `pitch()` exactly; otherwise the pitch would slide twice. Your own portamento is for a live legato key (`LEGATO` without `RAMP`). A Slide or Glide to the sounding note is a tie: you get no call. A module that exports no `pitch` is never called and never sees the flag; the FX are ignored for it.
- **Pages and labels** (S10; manifest `"pages": [{"name", "first"}]`, a parameter's `"labels": [...]`). At most 4 pages (the F1..F4 keys) of at most 8 parameters, together covering all parameters, the first at 0, names of 1..7 characters; labels only on an INT parameter of an instrument or (OS-INSFX-FX2, ABI 1.12: the generator then writes the table into `<ID>_ENTRY_FX`'s label member) an effect, one per value from min to max, 2..16 of them, 1..11 characters. The OS shows the key row and the pane heading from the table and the value text from the labels; without a table the pages stay `1-8`, `9-16` ... Write names and labels in capitals, as the OS's own text is. The loader, `o3m_gen.py` and `o3m_pack.py` check every limit.
- Worked examples: TB-303 (`pitch`, two pages), Chip (`pitch`, four pages, labels on Wave and Duty), FM (`pitch`, four pages, labels on Algorithm); tests `tests/module_pitch_test.cpp`, `tests/tb303_oracle_test.cpp`, `modules/{chip,fm}/tests`.

## 9. Trust model

Modules are **trusted community code, not a sandbox** (ADR 0002, maintainer decision 2026-10-10: anyone can drop modules on the card, no signing). The loader is robust against a damaged
or truncated file and an honest mistake (every field is range-checked, every address must be inside the image, a bad file is quarantined for the boot), not against an attacker with
a valid file: a module runs as the firmware does. The draw API cannot reach the display list or a register; that is containment of the *views*, not of the module.

## 10. Still design-only or open

- Hardware: the SDRAM code-bus alias and the speed of code from SDRAM (ADR 0002 validation 1 to 6), cache invalidation after a load, the cost of `view_draw` on the MK66, DWT cycle budgets
  for `render`. Fault handlers and the crash record's reach, the watchdog.
- Effects run as console-bound track inserts only (OS-INSFX S1, S2); the project record, the Mixer page, the parameter page, the master insert and Learn are later slices (S3 to S7); the biquad's unit cost is measured (S8, QA-GLB-170), the Drive's, the Crush's, the Fold's, the Env Filter's, the Comp's and the Phaser's are open (OS-INSFX-FX1, QA-GLB-176, QA-GLB-192, QA-GLB-196, QA-GLB-200, QA-GLB-204 and QA-GLB-207); the Wide's is open too (OS-INSFX-FX2 X1, QA-GLB-213).
- The TB-303's two views together cost 936 estimated words, over the page's 700 (section 4): on the Instrument page the second one is given what the first left and is refused, which quarantines the module. Fix in the module (polylines in a frame, as the FM does, or one axes-curve view), or give
  curves with axes a cheaper estimate in `module/view.h`. Found by OS-012c, not changed there (the module and the draw recorder are outside that task).
- Apps' parameters are not saved, learnable or undoable; a place for them (a per-project app record, or the device settings) is a persistent-format decision (ADR 0002 amendment 2).
- `ModuleVoices` has no unbind: changing the engine of a bound slot needs a project reload. Adding it needs the retire/quiesce protocol per instance.
- The picker's load is synchronous (a few KB: a few SD reads); the project-load path is the sliced one.
- More than one voice per instance (a polyphonic engine would run several instances on several tracks today), list labels for stepped parameters (an `int` shows as a number), view names and sizes in the file, scripts (Tier 2).
