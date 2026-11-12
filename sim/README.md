# sim/ — headless simulator and its fidelity

`o3sim` runs the OS application on the host: scripted input, a clock that only moves when the simulator moves it,
and a software interpreter of the FT81x display-list words (`EveRaster`) that the UI emits through the same
`hal::Display` contract a device will use. It writes PNGs and checks them against SHA-256 goldens. It is a tool
for looking at screens and for catching regressions, **not** a model of a particular FT812: see "Fidelity".

```sh
# from the repository root, after the host build in docs/architecture/os-tree.md
build-os-host/sim/o3sim --scenario sim/scenarios/pattern_basic.txt --out /tmp/pngs
build-os-host/sim/o3sim --scenario sim/scenarios/pattern_basic.txt --goldens sim/goldens.txt --check --twice
cmake --build build-os-host --target update_goldens      # rewrite goldens.txt (parallel, see below), then LOOK at build-os-host/sim/png/*/*.png
O3OS_GOLDEN_ONLY=pattern_basic cmake --build build-os-host --target update_goldens   # just that scenario and/or the gallery screens named pattern_basic*
build-os-host/sim/o3sim --gallery /tmp/gallery --goldens sim/goldens.txt --check   # every screen x state x theme, see "Gallery"
build-os-host/sim/o3sim --scenario sim/scenarios/pattern_basic.txt --out /tmp/pngs --theme black   # start in a theme
build-os-host/sim/o3sim --legibility --sheet /tmp/screens.png   # is every gallery picture readable? see "Legibility"
build-os-host/sim/o3sim --scenario sim/scenarios/e2e/e2e_audio.txt --twice --wav /tmp/wav   # a journey with sound: the renders as WAV files; see "Audio"
```

Output is one line per snapshot: `<scenario>/<snap> <sha256> dl=<words>/2048 [verdict]`. Exit 0 ok; 1 a golden or
determinism check failed; 2 bad usage, an invalid scenario, or a frame the display could not take (it did not fit
the display list, or the rasteriser refused it). `--display-words <n>` shrinks the display list the display accepts
(default 2048) so that last case can be exercised; ctest does (`o3sim_*_exit<code>`, `negative/`).
`--cards <dir>` is the folder where a relative `card <fixture>` of the scenario is looked up (default: the scenario file's folder). `--cards <image>` (a FAT32 image FILE, OS-QANET D1) makes that image the card of every `card` line of the run. The card of the journeys,
`e2e_card`, is generated into `<build>/sim/cards/` by the target `e2e_card_fixture` (`o3pj_tool gen small`, part of `all`, never committed), so a journey run by hand takes
`--cards build-os-host/sim/cards`; ctest and the `update_goldens` target pass it.

`--card-rate <bytes-per-ms>`, `--card-latency <us>` and `--card-write-rate <bytes-per-ms>` (OS-QANET D2) turn the slow-card model on from the start of the run (any one of them; the others keep the defaults, 17700 bytes a ms read, 257 us a call, 442 bytes a ms write: a fit to the unit's block bench, PROVISIONAL until OS-QANET D4 measures the unit's file reads): every call of the app to the card then charges latency plus bytes over the rate to the simulated clock, in the pass where it happens, and the audio engine renders through the stretched pass. The scenario line `card slow <bytes-per-ms> <latency-us> [<write-bytes-per-ms>]` (before `card` or after it) sets it from there, `card slow off` stops it. Without any of them nothing is charged and a run is what it was.

The scenario line `card ptfx` (OS-CEPTFX S4a, after `card <folder>`, a folder card only) writes seven synthetic header-only plugins (a chorus with a list parameter, a generator, a wavetable generator, a mono tool, one with twelve parameters, a refused one of a future version and one with a name that is too long) into the card's `Effects/`, so the PLUGINS page of Sample edit has a listing; they are made in code (`src/sim_ptfx.cpp`), never run, and no binary is committed.
`--wav <dir>` writes every `render` of a scenario as `<dir>/<scenario>_<name>.wav` (also when the run fails: what the failing assertion heard is what to listen to); ctest passes `<build>/sim/wav`, and `tools/ci.sh e2e` copies the WAVs to `build-e2e/<date>/wav/`.
With `--twice` the second run's renders must be the same samples as the first's (their SHA-256, printed as `audio <scenario>/<name> <sha256> frames=<n>`: for comparing two builds by eye, never a golden).
`--gallery <dir>` is the other mode, see "Gallery" below; there exit 1 also means a display-list budget breach.

## Scenarios

`scenarios/*.txt` and `scenarios/<folder>/*.txt` (`qa/`, `e2e/`), one command per line, `#` starts a comment; a word in double quotes may hold spaces. The grammar (parser: `src/scenario.cpp`,
documented in `include/o3os/sim/scenario.h`):

| Command | Meaning |
|---|---|
| `wait <ms>` | advance time by `<ms>` (0..3600000), one 1 ms tick at a time |
| `press <button>`, `release <button>` | button down / up |
| `tap <button> [<count>]` | press and release, `<count>` times (1..1000). The count is an addition to the spec's grammar: scrolling 12 tracks of 5 columns takes 56 presses |
| `pad <0-47> [hold <ms> \| down \| up]` | hit and release a pad (two ticks). OS-E2E-2: `hold <ms>` (1..60000) keeps it down for `<ms>` of simulated time (`<ms>` + 1 ticks in all), `down` and `up` are its two halves (a tick each) with any commands between: a note held while a knob turns |
| `encoder <n> [x<k> [every <ms>]]` | turn by `<n>` detents (`+8`, `-2`; not 0). OS-E2E-2: `x<k>` (2..1000) turns it `<k>` times, `every <ms>` (2..255) puts `<ms>` of simulated time between two turns (a tick each, the clock runs on in between): a sweep, `encoder -1 x60 every 5` is 296 ticks |
| `action <name> [<arg>]` | run a registered action directly (`nav.scroll 5`, `learn.toggle`): the registry, no gesture and no binding; the name is checked when the scenario is parsed, `<arg>` is an integer (default 0) |
| `document [bindings] [new] [sound] [tb303] [inserts] [tracks <n>]` | open the demo document (`sim/demo_project.h`: ten instruments with sampler values, an undo journal, and a fake audio side that drains the ParamChange ring at every tick): the Instrument PARAMS page edits it; `bindings` adds two MIDI Learn bindings (CC 74 on instrument 1's cutoff, pickup; CC 71 on its resonance, jump); `new` (OS-NEWKEEP) also attaches a project sink that does File > NEW on the document at once (F3 asks, then F3 is ALL and F1 KEEP INSTRUMENTS; the simulator has no card, so OPEN does nothing); `tracks <n>` (OS-ZOOM, 1..16) gives the project n active tracks instead of the demo's twelve (the zoom levels of a short project); `sound` (OS-E2E-2) is the document of the audio journeys instead of the demo: an empty pattern of 16 steps in a one-position song at 120 BPM and four neutral sample instruments (SINE, NOISE, PLUCK, SINE2: volume 50 = 0 dB under the 1.2+ law, pan centre, no tune, no filter, no amp envelope, no sends, 16 bit; `fill_sound_project`), the test bank's slots 0..3 (see "Audio"); `tb303` (OS-E2E-9) is the document of the TB-303 journey: an acid line of 10 steps at 150 BPM (a step is 100 ms) on a MODULE instrument that names the module `tb303`, with the parameters of `kTb303Params` (`fill_tb303_project`); `audio on` binds the module; `inserts` (OS-INSFX S4) is the demo with three insert records (T1 INS A and T3 INS A "biquad", T1 INS B "reverb" bypassed: the card has no reverb, so the effect picker greys it) for the Mixer INSERTS page (`fill_insert_project`; the simulator has no insert host, so no cost and no OVER BUDGET, and an assign says NO SOUND IN THIS BUILD; S5: `reverb` is native in the sim's module set, so Enter on T1 INS B opens its generated parameter page). Without it the app has no project (OS-002d) |
| `midicc <channel> <cc> <value>` | a MIDI control change (channel 1..16, cc and value 0..127): the action `midi.cc` as MIDI input will enqueue it. With the Learn overlay up it binds the cc to the row under the cursor, otherwise it moves what is bound |
| `midinote <channel> <note> <velocity \| off>` | OS-QANET C2: a MIDI note on or off (channel 1..16, note 0..127, velocity 1..127) on port 0 through the render's own path: `midi::CcInput::feed` (the parser and the notes' ring), then `App::poll_midi_notes` once, then one tick. The keyboard plays the selected track's instrument; in Edit on Pattern a note on also writes the cursor step. Needs `audio on` to be heard |
| `playhead <step> [<ustep>]` | the sequencer is playing the pattern in view at `<step>` (0..127), `<ustep>` of the step's 48 microsteps played (0..47, default 0): the word the engine publishes (`audio/play_position.h`), which the app reads once a pass; `playhead stop` stops it (OS-LIVEREC: `scenarios/pattern_record.txt` records a pass with it); `playhead <step> [<ustep>] countin <steps>` (OS-METRO) says the transport is counting in, `<steps>` (1..32) of it still to run, the running one included: the strip counts the beats and the pads flash (`scenarios/qa/qa_pattern.txt`); `playhead <step> [<ustep>] wait <step\|bar\|pattern>` (OS-METROALIGN) says a take armed on this running transport waits for that alignment (the sim gives the app a recorder with no engine behind it): the same countdown from one beat on |
| `power down`, `power up`, `power hold <ms>`, `power unsaved` | OS-POWER P1: the Power switch (`core::PowerHold`, active low) as the platform's pin read feeds it: `down` / `up` change the level, `hold <ms>` is down, `<ms>` of time (1..60000), up. The switch arms after 100 ms of release, so a scenario waits first. A hold shows the line from 150 ms and commits at 2000 ms, when the strip says POWERING OFF and the app reads no more input (`App::power_begin`); the platform's stages are not simulated (`platform/mk66/tests/mk66_power_service_test.cpp` is their proof). `unsaved` makes the open document differ from the save layer's baseline (a save layer with no card is attached), so the strip warns UNSAVED WORK; it needs `document` first (`scenarios/qa/qa_global.txt`). OS-SCREENSHOT: Shift then Power (`press shift`, `power down`, `release shift`, `power up`) saves `Screenshots/SHOT_nnnn.bmp` on the scenario's `card` with the platform's own `ScreenshotService` over the simulator's frame buffer (RGB565, without the chip's colour artefact), the app drawing no frame while it is written; with no card the strip says NO SCREENSHOT: NO CARD (`scenarios/e2e/e2e_screenshot.txt`) |
| `theme <name>` | switch the colour theme (`amiga`, `black`, `white`; `ui/theme.h`) as `Shift + Master` (`ui.theme.next`) would, then redraw; the name is checked when the scenario is parsed. `o3sim --theme <name>` makes a scenario start in a theme (not with `--check` / `--update-goldens`: the scenario goldens are the default theme's) |
| `snap <name>` | capture the framebuffer as `<name>.png` and hash it |
| `disk idle`, `disk working`, `disk out`, `disk audio` | OS-USBDISK U2b: the platform's word to the Config > USB page's CARD AS DISK row (`App::set_disk_state`): Idle, the hand-over running, the card on the computer (`out` also tells `set_card_on_computer`), or Idle with USB AUDIO presented (F7 then says USB AUDIO PAUSES). The simulator has no USB: a golden shows the page and the strips, nothing more. |
| `card <fixture-dir>`, `card pull`, `card insert` | OS-E2E-1b: the simulator gets a card. **A fixture that is a FILE is a FAT32 image** (OS-QANET D1: `card name.img`, or `card name` when the `--cards` folder has `name.img` and no folder `name`): the card is the unit's own FatFs (`platform/host/fat/include/o3os/platform/host/fat_fs_storage.h`: FatStorage and FatStorageWriter over the image, the unit's `ffconf.h`: long names, UTF-8, code page 437) instead of a host folder, so the card scenarios also run on long names, FatFs's directory order and the FAT write order; the image itself is never written (a private mapping), `card copy` rewrites the volume, `apps on` is refused on it. `tests/tools/o3fatimg.cpp` writes a folder into a sparse 64 MiB image (no mkfs, no root); the build makes `<build>/sim/cards_fat/<card>.img` for every card above, and ctest `scenario_fatfs_<journey>` (label `e2e`) runs each `e2e/` journey that uses one on it, with the same asserts, goldens and fingerprints. `<fixture-dir>` (a relative one is looked up in the folder `o3sim --cards <dir>` names, else in the scenario file's own folder; in quotes when it has a space; a symbolic link or a pipe inside it, or a folder that contains the scratch directory, is refused) is copied into a fresh scratch directory (removed when the run ends, so `--twice` starts clean) and mounted: the File screen lists it, SAVE, SAVE AS and EXPORT write to it through the real save layer and path guard (`platform/host/dir_card.h`), and File > OPEN and NEW work through the simulator's own project loader (`src/sim_project.cpp`: the native or OG loader, the sample loader, then `document_replaced` and the first pattern, as the platform's `finish_load` does; the app's frames are held while it runs, so `wait` after the F1). It needs `document` first (the load replaces it) and no `document` after it; one card a run. `card pull` takes it out of the slot (the strip says CARD REMOVED, every card call fails, a file being written stays torn), `card insert` puts the same card back (READING CARD, then CARD READY). `sim/cards/e2e_card/` is the card of the e2e journey: one native project, `Projects/Small Song`, which is `o3pj_tool gen small` (`tests/e2e_card_fixture_test.py` keeps it so). With `audio on` after the card the engine renders the card's voice pool, so a project's samples sound, and a load starts as the platform's does (a Stop on the transport, the live notes let go, the voices let to die away). IMPORT of a module (OS-E2E-9): File > IMPORT works on the card (`SimProject::import_module`: the platform's `start_module_import` and its phases, the sample feed, the song handed over as unsaved work; F7 chooses the engine for the commands). Not simulated: the project's MIDI and USB parts, the SD modules, the demo kit (a later slice each) |
| `assert state <key>=<value> ...` | OS-E2E-1a: the line the unit prints for `diag state` (`app/state_line.h`) holds every pair. `<value>` is the exact text, or `~<regex>` (POSIX extended, searched in the value: `o3hil.py`'s `state` step has the same rule; an empty `~` is refused: it matches anything). Several pairs on one line all have to hold; a key the line lacks reads as the empty text. `assert state screen=SONG page=1`, `assert state overlay=~^(NONE\|NOTIFY)$` |
| `assert model <path>=<value>` | OS-E2E-1a: one value of the open document (`document` first). A whole-number read (`tempo`, `order.*`, a step's `note`, `instr` and `vol`, `inst.<n>.channel`, an Int, Pan or Enum parameter) is compared exactly (`tempo=13001` fails when the tempo is 13000, and the number is written as it prints: `13000`, not `13000.0`), a float parameter as a number (1e-4 of tolerance, relative above 1), a text exactly. Paths: `tempo` (BPM x 100, as `diag state` says it), `name` (the title), `order.<n>` (0-based song position) and `order.len`, `pat.<p>.<t>.<s>.note\|instr\|vol\|fx1\|fx2` (pattern id 1..255, track 0..15 and step 0..127 both 0-based; the step's bytes, an FX as `<type>:<value>`), `inst.<n>.name` (instrument 1..64), `inst.<n>.<param>` (the caption of the parameter's row in lower case: `volume`, `panning`, `fine_tune`, `cutoff`, `bit_depth`; the envelope and modulation rows are `env<e>.<caption>` and `mod<e>.<caption>`, e 0..5: `inst.1.env0.attack`) |
| `mark model <label>`, `assert model digest=@<label>` | the SHA-256 of the document as `formats/native` writes it (into a hashing sink: nothing is written anywhere). `mark` remembers it under `<label>` (unique per scenario), `digest=@<label>` says the document is that one again: an edit and its undo, a save and a reload. `digest=<64 hex digits>` compares with a given one |
| `assert file <path> <test>` | OS-E2E-1a: a file of the simulated card (`card`, above), `<path>` relative to the card's root (in double quotes when it holds a space: `"Projects/Track 01/project.o3pj"`). Tests: `exists`, `absent`, `size=<n>`, `size><n>`, `size<<n>`, `sha=<64 hex digits>`. Without a card it fails with `no card`; with one pulled, `the card is pulled` (not `absent` either) |
| `firmware on` | OS-FWCONFIG: the firmware update dialog (Config's INFO page, F8 UPDATE) gets a port with no flash behind it: `platform/host/fake_firmware.h` (`FakeFirmwareSink`, a member of `Simulator`), with four scripted files in this order (`Simulator::firmware_on`): `TRACKER_1.PTF` (the reset target; a good image whose check takes 3 passes; the hand-over ends in "did not start" after 3), `SLOW_CHECK.PTF` (good; the check takes 600 passes, 600 ms, so its bar can be snapped), `BAD_IMAGE.PTF` (the check fails after 2 passes: "flash configuration field is not the required one" at byte 1104) and `NEVER_ENDS.PTF` (good; the hand-over never ends, as the unit's, whose chip resets). Needs nothing before it, once a run. Nothing is read from a card, no pin exists and no image is flashed: the sizes and the CRCs are the script's. The walk is `qa/qa_config_update.txt` (`qa/qa_config_update_refused.txt` for the unsaved-project refusal); the six states are also gallery pictures (`config_fw_*`) and the widest list is `extreme_firmware` |
| `apps on` | OS-APPUX S0: the card carries the shipped apps (`sim/include/o3os/sim/sim_apps.h`): the 11 apps with a manifest in `modules/apps` are loaded as the host shared objects the per-app tests dlopen (`o3m_<name>_host`, built with the simulator) and registered as native modules with the capabilities of their manifest; `modules/apps/<stem>.tmod` placeholders are written to the card's scratch copy (the folder is how MODULES lists apps); the app gets the module system over the card, with the simulator's clock, an audio tap that carries nothing (the Tuner says NO SIGNAL) and a MIDI-in ring nothing feeds. MODULES (`action nav.screen 12`) lists them, F1 launches the one under the cursor (an app that edits the project or writes files asks first, as on the unit), F8 and F7 + F8 leave. Needs `card` before it; refused after `audio on` and the other way round (the tap is not the engine's); `document` and `card` are refused after it. Pads (the SDK example) is not carried. The list is `O3SIM_APPS` in `sim/CMakeLists.txt`, which fails the configure when it differs from the manifests. Not simulated: the `.tmod` loader's checks of a real file, the cost of a call on the Cortex-M4F |
| `audio on` | OS-E2E-2: the real engine renders (`sim/sim_audio.h`; see "Audio"): the sequencer over the open document as the engine's event source, the sampler voices, the graph with the send effects and the master limiter, rendered by the simulator's clock. It needs a `document` before it (and the `card`, if any: the voices are the card's pool then), and `document`, `card <fixture>` and `playhead` are refused after it (the parser says so), as is `audio on` after `document new` (that File > NEW rewrites the project under the engine, with no Stop and no wait for the voices; a `card`'s NEW does both) |
| `wav <name> <path>` | OS-E2E-9: a WAV file of the card (16-bit PCM, two channels: the file an EXPORT wrote, `EXPORT/<project>/<name>.WAV`) becomes the finished render `<name>`, at the file's own rate, for `assert audio` and `o3sim --wav`. Needs `audio on` and a `card` before it, and the file on the card when the line runs (the export takes `wait`: two passes of the song and its tail). A name once, like a render's |
| `render <name> <ms> [stems]` | OS-E2E-2: a window of `<ms>` (1..10000) of simulated time from now whose blocks are the render: the master as the codec gets it, interleaved 16-bit L and R at the engine's rate (44,100 Hz by default; 44,117.647 Hz with `-DO3OS_RATE_44100=OFF`). It closes by itself when its time is up, whatever commands move the clock in between; the script has to run past its end before an assert (`assert audio` says `render 'x' is still running (N ms more)`), and a window open when the script ends is an error. One at a time; a name once (letters, digits and `_`). `stems` also captures the tracks T1..T8 (post-fader, mono) and the stem tap's master pair as `<name>.t1` .. `.t8` and `<name>.m`; a stems render is a whole number of 128-frame stem blocks |
| `assert audio <name> [in=<from>-<to>] <check> ...` | OS-E2E-2: measurements of a finished render, with tolerances: `pitch=<Hz>±<n>c`, `rms=` / `peak=` (dB re full scale), `dc<`, `silent`, `sound`, `clicks=`, `onset=<ms>±<d>`, `envelope=adsr(a=,d=,s=,r=,off=)±<e>`, `centroid<` / `>`, `pan=L` / `R` / `C±<d>` / `<dB>±<d>`, `zipper=`, `alias<` / `>`, `null=<other>[+<other>...],<dB>[,lag=<n>]`. `in=` limits the line to a part of the render; a value may be `@<other>` (the same measurement on another render) with `+n`, `-n` or `*n`; `±` is written `+-`. The grammar, the maths and every tolerance with its reason are at the top of `include/o3os/sim/audio_check.h`; `tests/audio_check_test.cpp` has each on synthetic signals with a pass and a fail plant. A passed line prints what it measured (`audio <scenario> line N: <render>: pitch 441.0 Hz, ...`) |
| `fingerprint <render> [nof0]` | OS-E2E-4: the finished render (a `render`, a `wav`, or a stems part `<name>.t1` .. `.m`) takes part in the scenario's audio fingerprint, `tests/audio_fingerprints/<scenario>.json`. Read only, no tick (no golden moves). `o3sim --fingerprints <dir> --check-fingerprints` compares it with the stored one (ctest `scenario_e2e_*` pass it), `--update-fingerprints` rewrites it; `nof0` keeps the render without an f0 track (several notes at once, a resonant voice). A render once |

**Assertions** read the simulator and change nothing: they never tick the clock, so a scenario with assertions draws the pictures it drew without them (the goldens do not move). A failed one ends the run with exit 2 and `line N: assertion failed: <the line>: <why>` (for `assert state` the why has the whole state line when the key is missing); a mistake in one (a path the grammar lacks, a digest label no `mark` made, a regular expression that does not compile) is refused when the script is read. The checks are `src/scenario_assert.cpp` (`include/o3os/sim/scenario_assert.h`: the grammar of the paths is at its top); the cases are `tests/scenario_test.cpp` and `negative/assert_*.txt`. The QA scenarios carry a few (`pattern_edit.txt`, `params.txt`, `screens.txt`), and `scenarios/e2e/` holds the journeys: files named `e2e_<name>.txt`, ctest label `e2e` (only `tools/ci.sh e2e` runs them; the file names carry their folder's prefix, as `qa/qa_<screen>.txt` does, because a name is also the ctest name and the goldens prefix). `scenarios/manual/<screen>_walk.txt` (OS-MANUAL2 M2-4) is the manual chapter's "Try it" walk-through: one `snap walk_<step>` for each step that changes the screen, the pictures `docs/manual/images.txt` lists as `<screen>_walk_<step>.png manual/<screen>_walk walk_<step>`; ctest `scenario_<screen>_walk` runs it like any scenario, and `o3sim_manual_images` holds the committed pictures to it.

Button names are `app/buttons.h`: `f1`..`f8`, `left`, `right`, `up`, `down`, `note`, `instr`, `vol`, `fx`,
`pattern`, `play`, `rec`, `shift`, ... (the hardware numbering of the unit's buttons).
Action names are the registry's (`app/actions.cpp`; see the table in `docs/architecture/os-tree.md`).

Gestures run on the simulated clock too: `press vol` and `press fx` one tick apart are the Learn chord, and
`press f1` / `wait 400` / `release f1` is a hold of 400 ms (each tick is 1 ms). A button that something is bound
to hold, double-tap or chord is delivered late (at the release, or when the window has passed), so a scenario that
presses such a button and snaps at once sees nothing yet: `tap` it, or `wait` first. `scenarios/actions.txt` shows
actions by name and the Learn chord, `scenarios/themes.txt` the theme switch (by key and by name), `scenarios/params.txt` the OS-002d parameter page (no project, the demo document, the four pages, edits, undo, pickup, Learn, a jump binding), `scenarios/screens.txt` the
OS-002b screens: Pattern, Song and its pages, the quarter jumps (Shift + F1..F5) on the Song list, the pads' mode picker (Shift + F1..F3: OS-PADS-A), free Edit (Rec) and following Edit (Shift + Rec, OS-RECSWAP), the
Navigator (the Master + Song or Song + Pattern chord, it stays open with its own F-key row, choose Song, F1 GO; F8 BACK, Pattern BACK), Learn (open, captured keys, close), a
notification shown and expired after its 2 s (snaps before and after), a queue overflow (eight F-keys) and the button
of a screen that does not exist yet. A hold on the simulated clock needs the time to run: `press f1`, `wait 450`
is a hold of F1 (the Navigator needs no hold: OS-APPSNAV opens it with the Master + Song or Song + Pattern chord; Shift + Pattern lists the module apps).

**Determinism.** No wall-clock anywhere: the simulator advances a `FakeClock` by a fixed 1 ms tick and runs one
pass of the app's main loop per tick, so a run is a pure function of the script. `--twice` runs the scenario on a
second fresh simulator and requires identical hashes; the goldens then prove it across processes and builds. The
raster uses integer arithmetic only (lines included), so hashes do not depend on the compiler's floating point.

**Goldens.** `goldens.txt` holds the SHA-256 of each snapshot's RGBA bytes (800 x 480 x 4, alpha 255), keyed
`<scenario>/<snap>`. ctest runs every scenario with `--check --twice` (`scenario_<name>`); the unit tests decode
the PNG the encoder writes (CRCs, inflate, all filters) and compare it with the hashed pixels. A hash says only that
something changed: after `update_goldens`, view the PNGs before committing the diff. These PNGs are not committed; the manual's screenshots are (`docs/manual/img/`, made by `tools/manual_images.py`, checked by ctest `o3sim_manual_images`).
The six `gallery/record_review_*` and `gallery/record_worst_*` goldens changed with OS-RATE (the 44,100 Hz default clock) and only in their time labels: the Record screen turns the take's frames into seconds with the engine's rate (`app/src/recorder.cpp`, `hal::audio::sample_rate_mhz()`), and the simulator gathers 0.04 % fewer frames per simulated second, so the take length, the window and the trim figures read a few thousandths of a second differently; the waveform and the layout are the same.

**The goldens are simulator truth, not device truth.** They pin what this rasteriser draws for the words the UI
emits. Until a `CMD_SNAPSHOT2` capture of the same screens from a Tracker unit has been compared with them, nobody
knows how far the FT812's output differs (the `[A]` rows below). That capture is a `hw-session` item: see
"Calibration".

## Fidelity

What the raster implements is exactly the subset in `docs/specs/OS-001-walking-skeleton.md` plus the words OS-003a
added for the graphics toolkit (`docs/adr/0010-ui-rendering-toolkit.md`: POINTS / POINT_SIZE, LINE_STRIP,
EDGE_STRIP_A/B, VERTEX_TRANSLATE_X/Y, BLEND_FUNC); OS-003b's UI uses VERTEX_TRANSLATE_X, VERTEX2II, RECTS and L1 bitmaps only; OS-003d's plots add LINE_STRIP, EDGE_STRIP_A/B, POINTS, COLOR_A, SAVE / RESTORE_CONTEXT, scissors, CELL and wide L1 / L4 bitmaps (BITMAP_SIZE_H), all words 003a already had: **003d needed no new word in the encoders or the rasteriser**; any other word fails the frame loudly (`render()` returns false,
`error()` names the word) instead of being skipped. Guide citations are "FT81X Series Programmers Guide" v1.2
(BRT_000031), section and page; **[A]** marks an assumption: something the guide does not say and the simulator
had to decide, to be settled by the `CMD_SNAPSHOT2` calibration below.

| Area | The simulator | The FT812 (known or assumed) | Effect on the UI |
|---|---|---|---|
| **LINE_WIDTH and RECTS** | A rectangle grows by `LINE_WIDTH / 16` px on every side (guide section 4.32 p.130, section 2.5.4 pp.21-22; the range is 0..4095); the corners stay square | Same growth, and the corners are rounded by that radius | The UI draws every rectangle, border and rule as RECTS at `LINE_WIDTH` 8 (half a pixel) with the two vertices on the centres of the first and last pixel covered, so the grown edges fall exactly on pixel boundaries. The chip's default width of 16 would grow every rectangle by a full pixel, which `ui::Renderer::begin()` never leaves on |
| **Antialiasing and corners** | RECTS and bitmaps: none. A pixel is covered when its centre lies inside the shape (a centre exactly on an edge counts as covered; with the UI's numbers none does). LINES, LINE_STRIP and POINTS: coverage antialiasing, see "Antialiasing: the coverage model". Edge strips: none | [A] primitives are antialiased. Edges on pixel boundaries are then fully covered or not at all, but the half-pixel corner radius leaves the four corner pixels of each rectangle about 95 % covered. **A 0.4.8 unit differs on every rectangle (fill at 0.545, edges on pixel centres): see Calibration, OS-SIMKEY** | The corner pixels of rectangles, about 5 % lighter on the chip. To be calibrated (below) |
| **LINES** | A capsule: the segment thickened by `LINE_WIDTH / 16` px on both sides with **round ends** (guide section 2.5.4 p.21, which says the width is "from the centre of the line to the boundary", and its figure on that page, which shows rounded ends). Antialiased [A] (coverage model below); exact integer arithmetic (the per-pixel decision compares squares of 64-bit values, no square root); vertices beyond +-4096 px fail the frame. `LINE_WIDTH` 0 draws nothing [A] (not even a sample that happens to lie on the line; RECTS and edge strips at width 0 are meaningful and unchanged) | Rounded ends as in the guide's figure; the antialiasing method is [A]; the guide does not say what a width of 0 draws | The UI draws no LINES yet; OS-003 uses them for envelopes, curves and waveforms |
| **LINE_STRIP** | `BEGIN(LINE_STRIP)` (Table 6, p.94): every vertex after the first adds the capsule from the previous one. The strip is blended **once**, at `END` / the next `BEGIN` / `DISPLAY`, with the context of its first vertex (colour, `COLOR_A`, `BLEND_FUNC`, `LINE_WIDTH`, scissor) and covers a pixel as the union of its segments does (the sample masks are OR-ed, see below): a joint or an overlap is not covered twice at any `COLOR_A` [A]. One vertex draws nothing | [A] whether a strip merges its segments; if the chip draws them one after the other the joints are about 5 % heavier on it (measured on the model: an L-shaped joint pixel is 243 merged, 253 as two segments) | As LINES |
| **POINTS** | `BEGIN(POINTS)`, `POINT_SIZE` (section 4.36 p.133): a disc, centre the vertex, radius `POINT_SIZE / 16` px (0..511), the guide's "distance from the centre of the point to the outermost drawn pixel". Antialiased [A]; a radius of 0 draws nothing [A] | Same radius; the antialiasing method is [A]; the guide does not say what a radius of 0 draws | The UI draws none yet |
| **EDGE_STRIP_A / _B** | The area above / below the polyline to the top / bottom of the screen (section 2.5.4 pp.22-23: the guide shows it only in figures). Per segment, every pixel column whose centre lies between the two vertices (ends included) is filled from the segment to row 0 (A) or the last row (B). `LINE_WIDTH` moves the boundary that many sixteenths away from the fill, so the line's own thickness is part of it [A] (the guide says only that it applies, p.130). Not antialiased: the guide's figure on p.22 shows a stair-stepped slope [A]. The strip is filled **once**, at `END` / the next `BEGIN` / `DISPLAY`, with the context of its first vertex, so segments that overlap blend once even when translucent [A]. The scissor clips it. `EDGE_STRIP_R` / `_L` are not in the subset and fail the frame | [A] the line-width rule, the missing antialiasing, the single fill and the context at the first vertex | Used with LINE_STRIP for envelopes and filter curves |
| **VERTEX_TRANSLATE_X / _Y** | Section 4.50 p.148, 4.51 p.149: a signed 17-bit offset in 1/16 px, added to every vertex of VERTEX2F **and VERTEX2II** [A], **always in 1/16 px whatever `VERTEX_FORMAT` says** [A]; part of the saved graphics context; zero at the start of a frame. Bitmaps then floor to a whole pixel as before | [A] the guide gives the unit ("signed x-coordinate in 1/16 pixel") and says only "added to vertex X coordinates": not whether `VERTEX_FORMAT` rescales it, nor whether VERTEX2II is translated; ADR 0010 section 5 (one translate per track column, so the cheap VERTEX2II reaches past x = 511) relies on the latter | **OS-003b depends on it now**: the renderer moves an origin (`VERTEX_TRANSLATE_X`, 0 / 256 / 512 on an 800 px panel) so that every bitmap past x = 511, tiles and text alike, is one VERTEX2II word. A hardware check before this reaches a device; `Renderer::use_translate(false)` is the fallback that needs nothing of it (`BITMAP_HANDLE` + `CELL` + `VERTEX2F`, about a quarter more words for the pattern view): the whole view draws the same pixels in both modes (`ctest translate_fallback`) and both fit the 1792-word budget |
| **Blending** | `BLEND_FUNC(src, dst)` (section 4.18 pp.114-115, Table 8): `colour * src + destination * dst` per channel, each product rounded to nearest in 8 bit and the sum held at 255. ZERO, ONE, SRC_ALPHA and ONE_MINUS_SRC_ALPHA are modelled for both factors; the default (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) is the source over of the goldens, bit for bit. The source colour is `COLOR_RGB` itself and the source alpha is `COLOR_A` x coverage, so a factor of ONE or ZERO ignores the coverage of an antialiased edge [A]. A bitmap texel of alpha 0 is never drawn, whichever function [A]. CLEAR is not blended. `DST_ALPHA` / `ONE_MINUS_DST_ALPHA` (and factors 6, 7) fail the frame: the picture has no alpha plane here. Part of the saved context; the default again at the start of a frame | [A] its own 8-bit rounding, possibly off by 1 level; the handling of coverage under ONE; transparent texels under a non-default function | None: the UI draws opaque colours, a bitmap's 0 or 255 alpha and, in OS-003, translucent fills and additive glows |
| **Bitmaps** | L1 (bit = alpha 0/255, MSB leftmost), L2 (two bits x 85, leftmost in bits 7-6, Figure 6 p.100: OS-003f), L4 (nibble x 17, high nibble first), L8; `NEAREST`, `BORDER` or `REPEAT` wrap, cell n at `source + n * stride * height`; positions floor to a whole pixel | Same; other formats and `BILINEAR` with a transform exist but are outside the subset and fail loudly here | The font is L1 at integer positions |
| **BITMAP_TRANSFORM_A-F** (OS-003f) | Sections 2.5.5 (p.24) and 4.12-4.17 (pp.108-113): x' = A x + B y + C, y' = D x + E y + F, A B D E signed 8.8 (17 bits), C F signed 15.8 (24 bits), initial A = E = 256; the guide's example (A = 128 shows the bitmap double width) is a test. Each pixel of the BITMAP_SIZE rectangle shows the source pixel its **centre** maps to, floored, all in integers [A]; outside the layout BORDER is transparent and REPEAT wraps. Part of the saved context; the identity (the default) takes the old path, so no existing golden moved. `BITMAP_SIZE`'s filter bit matters only here: BILINEAR fails the frame | [A] the sampling point: the guide gives the matrix, not where in the pixel it is applied. For a scale by a power of two (A = 128, 64) the corner and the centre give the same pixels; for the boot logo's A = 85 (a third is 85.33) they differ by at most one column: block 42 of the 124 is 4 px wide and the last is 2 px (`tests/raster_transform_test.cpp`), where the chip puts them is a hardware question. Bilinear is not modelled | The boot screen scales its logo by 3 with A = E = 85 (`ui/boot_screen.h`) |
| **Graphics context** | `SAVE/RESTORE_CONTEXT` hold COLOR_RGB, COLOR_A, CLEAR_COLOR_RGB, LINE_WIDTH, POINT_SIZE, BLEND_FUNC, VERTEX_TRANSLATE_X/Y, VERTEX_FORMAT, scissor, CELL, the current BITMAP_HANDLE and the bitmap transform; four deep, an extra save drops the oldest, an extra restore loads the defaults | The guide's list is longer (blend, stencil, tag, `CLEAR_COLOR_A`, ...) but the rest is outside the subset | None: the UI never saves or restores |
| **Bitmap handles** | BITMAP_SOURCE/LAYOUT/SIZE (+ `_H`) belong to a handle, are not part of the saved context (guide section 4.1) and **persist across frames**; the `_H` bits persist until set again | [A] persistence across frames and of the `_H` bits is not stated | The renderer re-binds all eight handles (four faces, the two hex-pair tile halves, the note tiles, the icons: 32 words) at the start of every frame and does not use `_H` |
| **VERTEX2II** | Uses the handle and cell in its own word and leaves the BITMAP_HANDLE and CELL registers alone; with any other primitive it is a vertex at whole pixels | [A] not stated in the guide | The renderer assumes nothing: after a VERTEX2II draw it re-emits BITMAP_HANDLE and CELL before the next VERTEX2F draw (a few words per frame) |
| **Frame start** | Every frame starts black with the default context; a list without CLEAR shows black where it does not draw | Undefined without CLEAR | The UI always clears |
| **CLEAR** | Colour buffer only, clipped by the scissor, opaque (`CLEAR_COLOR_A` is not in the subset); stencil and tag clears are no-ops | The scissor applies to CLEAR too [A] | None |
| **BITMAP_SIZE** | A width or height of 0 means 2048 (guide section 4.9) | Same | None: every face, tile set and icon face sets its size |
| **Invalid state** | A zero stride or layout height, a bitmap cell outside RAM_G, or a vertex outside BEGIN/END fails the frame | Undefined or silently ignored | Fails loudly so a UI bug cannot hide |
| **RAM_G upload** | `hal::Display::upload(addr, bytes, count)` (contract in `hal/display.h`: RAM_G is 0x000000..0x0FFFFF, 1024 KB, Memory Map p.253; at most `kMaxUploadBytes` = 4096 bytes per call, callers chunk; word-aligned address and source pointer; all or nothing; ordered against committed frames; the result says why a call was refused: TooLarge, NoData, Misaligned, BadRange or Busy) writes into the simulated RAM_G at once. The simulator's own assets go in this way at start-up (`load_assets` = `ui::upload_assets`: the four faces and the icons chunked from the word-aligned bitmaps `font_convert.py` and `icon_convert.py` generate, and the 379 tiles composed from the Data face into a word-aligned staging buffer and sent in 1.9 KB chunks; 38 KB in all). The display counts uploads and keeps none unless a test asks (`record_uploads(max_bytes)`, capped). The finished picture is not redrawn: the new bytes show from the next committed frame | The chip re-reads RAM_G on every scan-out, so an upload over a bitmap the visible picture uses changes that picture at once and may tear | None yet; a UI replaces bitmap data in use by uploading elsewhere and committing a frame that points there |
| **Not modelled** | Scan-out timing and tearing (and so an upload showing mid-frame), the `REG_DLSWAP` swap (the display contract abstracts it), touch, the coprocessor, flash | | |

### Antialiasing: the coverage model [A]

**Status: [A], an assumption until a `CMD_SNAPSHOT2` calibration.** The guide says nothing about antialiasing (its text
never mentions it; only the figures on pp.20-23 show smooth lines and discs), so the chip's method is unknown.
ADR 0010 asks the simulator to antialias LINES, LINE_STRIP and POINTS "like the chip, calibrated later"; this is the
simulator's own model, chosen to be exact, deterministic and easy to check by hand. Nothing in the UI depends on
its details yet: only the goldens of states that draw those primitives do (none of the scenario goldens does).

- **Shape.** Every antialiased primitive is a *capsule*: the set of points whose distance to a segment is at most
  `r`, with `r = LINE_WIDTH / 16` px for LINES and LINE_STRIP segments (round ends) and `r = POINT_SIZE / 16` px
  for POINTS (a zero-length segment, i.e. a disc). Vertices are in 1/16 px after `VERTEX_TRANSLATE`.
- **Coverage.** Each pixel is sampled at 8 x 8 points, at the odd sixteenths 1, 3, .. 15 of the pixel in x and y.
  A sample counts when it lies inside the capsule, **boundary included**. With `n` samples inside, the coverage is
  `(n * 255 + 32) / 64`, 0..255 (rounded to nearest). A pixel with coverage 0 is not touched.
- **Exactness.** The test is integer-only: a sample against the segment's ends compares squared distances; beside
  the segment it compares `|across|` with `isqrt(r^2 * |d|^2)`, which is exact because `across` is an integer.
  So a picture does not depend on the compiler's floating point (the hashes in `goldens.txt` hold across builds).
  The vertices must lie within +-4096 px (65,536 sixteenths) after the translation or the frame fails: beyond that
  the squares would not fit 64 bits. (Pixels whose centre is more than `r + 12` sixteenths from the segment are
  skipped and those within `r - 12` are taken as covered without sampling; a pixel centre is at most 11.3
  sixteenths from any point of its pixel, so neither shortcut changes a result: a unit test compares every pixel
  of random capsules with a reference that has no shortcuts and uses 128-bit squares.)
- **Strips.** A LINE_STRIP keeps, per pixel, the OR of the 64-bit sample masks of its segments and blends the
  count of the union once, so a joint or an overlap is not covered twice (an L-shaped joint pixel is 61 of 64
  samples, 243; blended segment by segment it would be 231 then 253). A radius of 0 (`LINE_WIDTH` 0 for lines,
  `POINT_SIZE` 0 for points) draws nothing [A].
- **Blending.** The coverage multiplies `COLOR_A` to give the source alpha, which then goes through `BLEND_FUNC`
  (see the table). So under the default function a half-covered pixel is the colour at half strength over what
  was there, and under `ONE` it is the whole colour (the factor does not look at the alpha).
- **What it gets right.** Edges on pixel boundaries are fully covered or not at all: a 1 px line (`LINE_WIDTH` 8)
  between pixel centres is exactly one row of pixels, its ends are 231/255 (the round cap), and a line on a pixel
  boundary splits 128 + 128 over two rows. The sum of the coverages equals the capsule's area `2 r L + pi r^2` to
  within 0.1 % for lines off the sample lattice (unit test `antialiased lines conserve area`).
- **What it gets wrong, knowingly.** A regular 8 x 8 lattice is biased for an edge exactly in step with it: a
  45 degree line whose ends are on whole sixteenths of the pixel centres is 2.5 % heavy. The chip may use another sample count or
  an analytic coverage, other caps, or no antialiasing at all on some of these; the calibration decides.

The display list **is** limited like the chip's RAM_DL: 2048 words (`eve::kMaxDisplayListWords`). A frame that does
not fit is never drawn truncated: the builder drops the words past its buffer, `end_frame()` then aborts the frame
(`hal::Display::abort_frame()`, the 001a contract) and reports it, and the same happens when the sink's `write()`
refuses a list. The previous picture stays visible, `App::failed_frames()` counts it, `Simulator::healthy()` turns
false with `failure()` saying what, `run_scenario` stops, and `o3sim` exits 2. The app does not retry the same screen
in a loop; the next change redraws it. So an over-full screen is an error here, not a surprise on the device.

### Display-list budget: what it means for dense screens

Text is one glyph bitmap per character, so a character costs one display-list word where its x and y fit VERTEX2II's 9
bits and up to two elsewhere (`CELL` + `VERTEX2F`; a `CELL` word is skipped when the previous glyph has the same cell).
Colour changes cost one word each and are grouped by the UI. The first pattern view (OS-001), drawn glyph by glyph, needed
**1856** words in its worst case (every field of every visible step filled with glyphs that all differ, over every track
window, several cursor rows, every instrument name, with and without a message: 534 variants) and passed only on an interim
limit of 1920; 16 rows cost about 93 words each and a 24-row, 6-track grid could not have fitted.

OS-003b ended that exception with the tile faces of ADR 0010 section 5: a note, an instrument, a volume or an FX value is one
tile, one word (an FX is a type glyph and a tile; an empty instrument or volume is its two dash glyphs, so two words), the
renderer moves an origin (`VERTEX_TRANSLATE_X`) so that nothing needs `CELL` + `VERTEX2F` on the way, and the pattern
view's worst case measured **1,193 words** in each of the three themes (typical 967, the style specimen 924), from a
2,136-variant sweep that includes the dearest content (a note and both FX filled, the instrument and volume empty), every
instrument name and every message the info line could carry. OS-002b took the message chip off the info line (the app says
things with notifications now), so the sweep is 534 variants and the worst case **1,186**; with
`Renderer::use_translate(false)`, the fallback that needs nothing of `VERTEX_TRANSLATE_X`, it is **1,496** (typical 1,125;
it was 1,502). The standard budget, **1792 words** (`sim::kBudgetWords`,
256 spare), holds for every worst-case state of every screen in every theme in both modes, with no exception;
`budget_test.cpp` fails if one reappears.

#### Budget harness

`src/budget.cpp` (`sim/budget.h`) is the reusable helper: it renders a screen state in a theme through the real renderer
into a builder (no rasteriser, so it is fast), counts the words with DISPLAY, and compares the largest of a
state's variants with its limit. The screens and their states are one registry (`src/screens.cpp`,
`sim/screens.h`): a state is a function that draws into a begun `ui::Renderer` (whose theme it is drawn in); a worst-case
state may be a sweep over `variants` sub-states and has a limit (`kBudgetWords`). Every state is measured in every
built-in theme. A list of 8192 words is used for measuring, so a screen above the chip's 2048 reports its real size. ctest
runs it as the unit tests `budget: ...` (`tests/budget_test.cpp`, also the ctest entry `budget`) and `o3sim --gallery` prints
the table (some of the Amiga rows; Black and White follow each, and every state is measured in both modes: `translate` is
`VERTEX_TRANSLATE_X`, the default, and `vtx2f` the fallback):

    screen     state        theme   mode      variants   words   limit  verdict
    pattern    typical      amiga   translate        1     967       -  ok
    pattern    typical      amiga   vtx2f            1    1125       -  ok
    pattern    worst        amiga   translate      534    1186    1792  ok  [largest: variant ...]
    pattern    worst        amiga   vtx2f          534    1496    1792  ok  [largest: variant ...]
    pattern    cursor_end   amiga   translate        1     999       -  ok
    style      specimen     amiga   translate        1     924    1792  ok
    song       worst        amiga   translate       36     571    1792  ok  [largest: variant 12]
    overlay    pattern_navigator_worst amiga translate  440  1199    1792  ok  [largest: variant 299]
    overlay    pattern_notify_worst    amiga translate  120  1234    1792  ok  [largest: variant 81]
    worst case per theme, translate: amiga 1234 black 1234 white 1234
    worst case per theme, vtx2f fallback: amiga 1541 black 1541 white 1541

To add a screen: add its states to the registry; the budget test and the gallery pick them up.

OS-002b adds the Song screen and the overlays (`src/screens_app.cpp`, drawn through `app::render_scene`, the same
function the app draws a frame with, so the base screen is told what the overlay covers exactly as on the device). Every
state is a budgeted one (limit 1792): the typical states are one-variant worst cases, and the overlays are also measured
as sweeps over the dearest content under them: the pattern's own worst grids (every field filled, and the sparse mix) at
windows spread over the pattern, the Navigator with each of its 11 cells chosen, a notification of 48 characters with an
8-character source and a count of 99 in each severity, the song with 64 positions of 12-character names and 3-digit lengths, on both pages.

| Words (DISPLAY included, the same in every theme) | translate | VERTEX2F fallback |
|---|---|---|
| Song, typical / arrangement page / worst sweep (36) | 463 / 528 / 571 | 505 / 575 / 616 |
| Pattern + Learn, typical / worst sweep (40) | 966 / 1,146 | 1,110 / 1,434 |
| Pattern + Navigator, typical / worst sweep (440) | 1,091 / 1,199 | 1,244 / 1,452 |
| Pattern + notification, typical / worst sweep (120) | 964 / 1,234 | 1,114 / 1,541 |
| Song + notification, typical / worst sweep (36) | 452 / 606 | 475 / 638 |
| Song over a document with the cursor in the track map, typical / worst sweep (8): 16 tracks, 16 slots selected (OS-SONGMAP) | 1,199 / 1,246 | 1,269 / 1,316 |
| Song over a document, the cursor on the last track of a map whose last tracks are empty (their faint ticks): 12 tracks / 16 tracks / the dearest sweep (8): 16 tracks, a note, FX only or nothing side by side (OS-BUG-SONGMAPCUR) | 1,029 / 1,123 / 1,393 | 1,097 / 1,201 / 1,463 |
| Song TOOLS dialog over it, typical / worst sweep (24) (OS-SONGMAP) | 854 / 1,137 | 928 / 1,223 |

OS-009 P1a.1 (`src/screens_pattern.cpp`, `screens_app.cpp`) draws the pattern from a **document** (a `model::Project` with a pattern 1 the demo fills, `fill_demo_pattern`)
and adds states: `edit_on`, `selection_worst` (540 variants of block position, size, column and content), `shift_layer`, and the choosers
`overlay` `pattern_chooser_typical` / `_worst` (180), `pattern_length`, `pattern_jump`. The pattern's `worst` sweep is now 909 variants (the 801 of the demo and 108 over the
document with every header mark, the Shift layer and a block). Every sweep draws three contents: filled, sparse, and (the first review's fix, when fade, cut, off and C10..G10 got tiles)
the sparse mix with the dearest note tiles, notes 124..128 (a second bitmap handle, bound once in a frame that shows one) alternating row by row with notes, fade, cut and off. Scenario `pattern_edit` (27 snaps: Browse, Edit, notes by pad and encoder, instrument, volume, FX, clear, insert,
backspace, a block with transpose and clear, undo and redo, marks, the choosers) runs on a `document`.

| Words (DISPLAY included, the same in every theme) | translate | VERTEX2F fallback |
|---|---|---|
| Pattern, typical / edit on / cursor on the last step | 973 / 983 / 1,005 | 1,075 / 1,082 / 1,081 |
| Pattern, worst sweep (909) | 1,281 | 1,616 |
| Pattern, selection worst sweep (540) / Shift layer | 1,245 / 975 | 1,591 / 1,064 |
| Pattern + chooser, typical / worst sweep (180) | 995 / 1,338 | 1,115 / 1,703 |
| Pattern + Learn, worst sweep (120) | 1,251 | 1,576 |
| Pattern + Navigator, worst sweep (1,320) | 1,304 | 1,574 |
| Pattern + notification, worst sweep (360) | 1,324 | 1,661 |
| Style specimen (its tile row has CUT, FAD, C10, G10) | 934 | 1,040 |

Skipping what the overlay covers is what keeps the dialogs cheap: a dialog is a whole number of rows tall, so it cuts the
grid only at its sides, and the fields inside it are not drawn at all (`tests/overlay_test.cpp` counts the words with and
without skipping, and holds the pictures equal pixel for pixel).

### Audio (OS-E2E-2): the real engine in the simulator

`audio on` wires the same libraries the unit's composition root does (`platform/mk66/src/app_main.cpp`, firmware: nothing is shared with it, as with the card's loader), the way `tests/audio_demo_test.cpp` does on the host: `audio::Engine` over a `TapMem`, the sampler voices (`VoicePool`: the card's when the simulator has a card), the sequencer as the engine's event source (`seq::event_source`; no MIDI link), the live-note box, the two send effects (`SendDelay`, `SendReverb`) and the master limiter in their slots, the engine's ParamChange ring as the app's param ring, the sequencer's position as the app's play position, and the app's sinks: the transport, the track and perform sides (the sequencer's command ring) and the live preview (the live queue), the Mixer's board with the engine's meters. `src/sim_audio.cpp` copies the unit's transport and preview sinks without the outputs that open late.

| Area | The simulator | On the unit |
|---|---|---|
| **Time** | An integer accumulator adds the engine's rate (`hal::audio::sample_rate_mhz()`, 44,100,000 mHz by default) every 1 ms tick and renders a block of 64 frames each time 64 frames' worth has gathered: 0 or 1 block a tick, 689 blocks in 1,000 ticks, real time on average, no wall clock. The app's pass comes first in a tick and the audio after it | The codec's DMA at the same rate; the render runs at its own priority, the main loop between blocks |
| **A note's time** | A live note (a pad) is taken at the next block start and sounds from the block after (about 1.5 ms); a sequencer note at its sample offset in the block that has its step. Both the same every run | The same code; the unit adds the codec's latency (`diag`'s input and output slots) |
| **The sound** | Without a card, a test bank bound through `app::make_sample_desc` from the document's instruments, so every parameter the project holds acts as on the unit: slot 0 a sine of 100 samples a period (441.00 Hz at the root note at 44,100 Hz; 441.18 Hz with `-DO3OS_RATE_44100=OFF`, 0.5 of full scale, looped), slot 1 white noise (xorshift32, looped), slot 2 a one shot pluck (0.8 decaying by e every 0.12 s), slots 3.. the sine again. Built by integer arithmetic, no libm and no file. With a card: what the project's loader bound; no test bank | The project's samples from the card |
| **Reference players (OS-E2E-9)** | The Amiga voices (`AmigaVoices`, with the sequencer's Tick events on), the WAV export (`OfflineRender` and `ExportSource` wired into the save layer; the live callback is the engine's `trampoline`, so an export parks it), and the TB-303 module (the host shared object of the module bound to the Module instrument that names it) | The same libraries, the same wiring (`app_main.cpp`); the module binder reads the module from the card or the image |
| **Not simulated** | The other SD modules and the module UI (E2E-7), the MIDI link and its ports, the recorder and the capture hardware (the Record screen), the apps' audio tap, the USB stem map (the stem tap itself is captured), the strip's CPU figures (the fake cycle counter moves 11 a call) | Everything |
| **Determinism** | A run is a pure function of the script: the pacing is integer, the bank is integer, the sequencer and the engine have no other input. `o3sim --twice` requires the same SHA-256 of every render both times. The float results (the graph's gains, the filters) are reproducible on one machine and compiler, not promised across them, so the checks carry tolerances and no sample hash is ever a golden | |

The journeys are `scenarios/e2e/e2e_audio*.txt` (label `e2e`: `tools/ci.sh e2e`): the first note (pitch, level, onset, silence after the release, five pitches), the sequencer through the keys and Play (two notes on the grid to the sample, the stems), the VOICE and SENDS pages (the volume law, the pan law, the bit depth), the FILTER and MOD pages (a filter closes the noise, an envelope follows the engine's ADSR), a swept knob without a zipper, two tracks whose stems add up to the master, and the demo document under the limiter's ceiling. Their failure paths are the small `o3sim_audio_*_exit2` tests of `ci.sh host`. What the engine does that the journeys found: see the spec's Built note.

**Audio fingerprints (OS-E2E-4).** A journey opts a render in with `fingerprint <render>`; `tests/audio_fingerprints/<scenario>.json` (3 to 6 KB: about 150 bytes a frame) holds, per 100 ms frame, 24 log-band levels (30 Hz to 20 kHz), the f0, the peak, the RMS, the stereo balance, the DC and the click count, and the onsets of the render (rules and definitions: top of `include/o3os/sim/audio_fingerprint.h`; docs/specs/OS-E2E.md "E2E-4"). Every `scenario_<name>` ctest compares its own in the process that runs the journey (`--check-fingerprints`), so a fingerprint costs a journey nothing more; `--twice` requires the same text both times. After an intended change of the sound: `cmake --build build-os-host --target update_audio_fingerprints` (`tools/update_audio_fingerprints.py`: one `o3sim` per opted-in scenario, at most 4 at once) rewrites the files and writes `build-os-host/sim/fingerprint/fingerprint_diff.md` (per key and render: the frames and bands that moved, the onsets, the f0, the levels) and `.../wav/` (the renders to listen to; never committed). The guard `tools/check_fingerprint_guard.py` (ctest `fingerprint_guard`, in `ci.sh host`) rejects a commit that changes a fingerprint file unless the same commit changes a firmware source directory (`app/`, `audio/`, `core/`, `model/`, `seq/`, `formats/`, `modules/`, `module/`), the scenario of that key or the measuring rig (`audio_fingerprint.*`, `audio_check.*`, `sim_audio.*`), and unless its message has a body of two lines at least (the reason); the uncommitted changes count as one more commit.

**Reference players (OS-E2E-9).** `scenarios/e2e/e2e_ref_*.txt` are journeys whose capture is compared with a reference player's render of the same bytes (docs/specs/OS-E2E.md "E2E-9"): libopenmpt for a ProTracker, a Scream Tracker 3 and an OctaMED module and for the Paula sound (`tests/e2e_openmpt_ref.py`), scipy for the instrument filter, the master limiter and the 44.1 kHz export (`tools/dsp_ref.py`, `tests/e2e_dsp_ref.py`), the original Open303 for the TB-303 module (`tests/e2e_open303_ref.py`, `tests/tools/o3ref303_tool.cpp`). The modules are generated (`tools/e2e_ref_cards.py` into `<build>/sim/cards/`, the target `e2e_ref_fixture`), the tolerances are in each test's header and in the spec, and 19 `_break_*` tests perturb the REFERENCE and must fail (far, `_break_near_*` at about twice the tolerance, and a hard clipper in place of the limiter's two-voice take). Needs libopenmpt 0.7.x (`libopenmpt0t64`; another series is an "untested version"), numpy and scipy (`python3-numpy python3-scipy`); a missing or untested one skips (exit 77) and fails under `O3_CI=1`.

### Gallery

`o3sim --gallery <dir> [--goldens goldens.txt (--check | --update-goldens)] [--twice] [--theme <name>]` renders every screen
x state x theme of the registry (the pattern view: `typical`, `worst` (the variant of the sweep that needs the most words) and
`cursor_end`, the cursor on the last step of the last track; the `style` specimen: the type scale, the 16 tokens, the tile
and icon faces and the components; OS-002b: `song` `typical` and `worst`, and `overlay` `pattern_learn`, `pattern_navigator`,
`pattern_notify` and `song_notify`: the Pattern and Song screens with Learn, the Navigator and a notification over them,
in the app's own way of drawing; the `plot` screen of OS-003d, ten states, below) through the renderer and the rasteriser, and writes into `<dir>`:
`<screen>_<state>_<theme>.png` per state and theme, `contact_sheet.png` (quarter-size thumbnails, a row per state and a
column per theme, with their names and word counts), `contact_sheet_<theme>.png` for each theme and, with `--check`,
`diff_list.txt` (`ok` / `CHANGED` / `NEW` / `STALE` per golden key; a run without `--check` removes one left by an earlier
run, test `gallery_stale_diff_list`). With `--check` each thumbnail is framed green for unchanged, red for changed, amber
for new. `gallery` is a reserved scenario name (its goldens keys would collide; `o3sim` refuses it with exit 2). The hashes
are checked against `goldens.txt` under the keys `gallery/<screen>_<state>_<theme>` (`update_goldens` rewrites them),
`--twice` renders again and requires identical hashes, and the budget table is printed (a breach is exit 1). `--theme`
renders only one theme and cannot be combined with `--check` / `--update-goldens`. ctest runs the whole thing as `gallery`.

The `boot` screen (OS-003f, `boot` `logo`: the boot screen of `ui/boot_screen.h`, 76 of 2,048 words) looks the same in every theme: its registry row
names a `fixed_theme` (`ui::boot_theme()`, the Black table), which the gallery and the budget harness draw with whichever theme they sweep, so
`boot_logo_amiga.png`, `_black` and `_white` are one picture and the three goldens one hash. The build id on its line is a placeholder here.
The `typical` state in the Amiga theme is the app's first frame: its hash equals `pattern_basic/initial`.
A state with `in_gallery = false` (`ScreenState`, `screens.h`) is measured by the budget harness but not rendered: the
sweeps of the overlays and the song's arrangement page, because a picture costs about 0.65 s under the sanitizers and a
measurement about a millisecond.

After a UI change: `cmake --build build-os-host --target update_goldens`, then **look** at the PNGs in
`build-os-host/sim/gallery/` (the contact sheets are only quarter size) before committing the golden diff.

`update_goldens` (`tools/update_goldens.py`, OS-CIFAST2) runs one `o3sim` per scenario and one per gallery shard (the 6 of ctest
`gallery_<i>of6`) as parallel processes: at most 4 at a time (`O3OS_GOLDEN_JOBS` can only lower that), at `nice 10`, each writing a
partial goldens file that the driver merges into `goldens.txt` afterwards: the same bytes as the old serial run (sorted keys, a
re-rendered scenario's entries replaced, every `gallery/` entry dropped and rewritten). If any process fails, `goldens.txt` is left
as it was. The PNGs are where they always were (`sim/png/<scenario>/`, `sim/gallery/<screen>_<state>_<theme>.png`); only the contact
sheets differ: a sheet needs all the pictures in one process, so they are per shard (`sim/gallery/shard<i>of6/contact_sheet*.png`)
and the old ones in `sim/gallery/` are removed. To refresh only what a task changed, `O3OS_GOLDEN_ONLY=<name>`: `<name>` is a
scenario (`scenarios/<name>.txt` or `scenarios/<folder>/<name>.txt`: its entries are replaced) and/or a prefix of gallery screen names
(`o3sim --gallery --only <name>`: the entries of the rendered shots are replaced, the other gallery entries stay, the contact sheets
cover just those screens); a name that is neither stops with exit 2. `ctest update_goldens_selftest` checks the merge.
`--watch` (regenerate the gallery whenever a source changes) is not implemented; it is a follow-up together with `--layout`.

### Legibility (OS-E2E-5): is every picture readable

`o3sim --legibility [--sheet <file.png>] [--theme <name>] [--only <prefix>] [--shard <i>/<n>]` draws the shots of the gallery (the same
`--theme`, `--only` and `--shard`; no PNG per shot, no goldens, no second pass) and judges the text of each one. The rasteriser logs every
bitmap it draws from the faces, tiles and icons (`EveRaster::record_glyphs`, off unless asked, so a gallery run is as fast as before): which
bitmap, where, its colour, the scissor in force, which of its pixels have ink, the colour under each of them, and whose ink that was. A shot has
a finding when

| Finding | Meaning | What it usually is |
|---|---|---|
| `clipped` | some ink is cut by the scissor or by the screen edge | a label or a name that runs off the panel |
| `overlap` | a bitmap's ink lies on the ink of another one that is still on the picture | two texts that collide (an info line that meets the key hints, a strip field that meets the CPU readout) |
| `contrast` | the ink's colour on the colour under it is not a pair of `ui::kContrastPairs`, the table `tests/theme_test.cpp` holds to its ratios | a text colour no theme test has looked at; also text that runs out of its own panel onto the pane |

Ink that a later opaque panel covers (the base screen's text under a dialog) is not on the picture and is not judged; a translucent tint
does not cover (the text shows through it, so it is judged). A bitmap that a later panel hides only in part is not a finding either, but the
run counts it per picture ("covered in part"). One overlap is on purpose and is allowed by rule: a **faux bold**, the Label face's M or S
twice in one colour, a pixel apart (the pattern header's marks while Shift is held); the run prints how many it let through. Any other deliberate exception is an entry of the named allow list at the top of
`src/legibility.cpp`, with the reason; the run prints those too. An allow-list entry is not how a run is made green: a finding is a fix, or a
backlog row (`OS-BUG-*`) that the entry's reason names. Today's entries are four frames' Accent mode word on the Chrome strip (EDIT, REC, STOPPED; 1.2:1 in Amiga, which no theme can fix without making the orange unlike the cursor's), on the unit check. The check is reported per shot: `FINDING gallery/<screen>_<state>_<theme>: <kind>: <glyph> at (x, y): ...`
(at most 12 lines a shot), then `legibility: <n> pictures, <n> findings, <n> allowed`; exit 1 when there is a finding. `--sheet` also writes a contact
sheet of every shot, nine thumbnails across (three states, a column per theme).

The `extreme` screen (`src/screens_legibility.cpp`) holds the gallery's widest content: every limit of the model at once, so that a name that
is too long for its field shows up: the longest project, pattern, instrument and track names, 800 BPM, instrument 64, pattern 255, both FX
cells of every step, and the longest strip message (`pattern`, `pattern_notice`, `params`, `file`).

ctest `legibility_<i>of6` runs the sweep in six shards. They carry the label `e2e` beside `app`: `tools/ci.sh e2e` runs them (and writes
`build-e2e/<date>/report.md` and `screens.png`); `tools/ci.sh host`, the scoped runs and the GitHub workflow leave the label out. Direct:
`ctest --test-dir build-os-host -L e2e --output-on-failure`. The checks themselves are tested by `tests/legibility_test.cpp` (a planted clipped
label, overlap and low-contrast pair fail; clean text passes).

### Plots (OS-003d): what `ui::plot` asks of the rasteriser

`ui/plot` draws through `Renderer::begin_raw()` / `end_raw()` (SAVE_CONTEXT ... RESTORE_CONTEXT, two words; a third when the
renderer had moved its VERTEX_TRANSLATE_X origin, which the section zeroes so that its vertices are screen coordinates).
`tests/plot_raster_test.cpp` pins what rests on the simulator, each also a hardware question (`[A]` rows above):

- **Edge strips with vertices on pixel centres are exact.** With `LINE_WIDTH` 8 an `EDGE_STRIP_B` vertex at the centre of pixel
  (x, y) fills column x from row y down, `EDGE_STRIP_A` from row y up, and the line width moves the boundary by half a pixel
  which, from a pixel centre, is the pixel's own edge. The vector wave (two `EDGE_STRIP_B` fills, the second in the lane's
  background from just under each column's minimum) and the L1 bitmap are the same picture because of it
  (`tests/plot_wave_test.cpp` compares whole frames). If the chip rounds the other way the vector wave is a row off.
- **A strip is ended before the colour changes** (an END after each), because the simulator blends a strip once, at END, with the
  context of its first vertex; what a chip does with a colour change inside a strip is not in the guide.
- **A wide L1 / L4 bitmap**: BITMAP_LAYOUT with a stride up to 400 bytes, BITMAP_SIZE with its `_H` command for widths past 511,
  L4 nibbles expanded by 17 (15 -> 255) [A: that the chip expands them so], drawn with VERTEX2F and an explicit CELL 0, because
  VERTEX2F draws whatever cell the CELL register holds (a bug the fallback mode's tile words exposed).
- **SAVE / RESTORE_CONTEXT** bring back COLOR_RGB, COLOR_A, scissor, LINE_WIDTH, POINT_SIZE, BLEND_FUNC and the translation (guide
  section 4.1); the renderer additionally forgets its cached colour, bitmap handle and cell at `end_raw()`, so it does not depend
  on the chip restoring them.
- **Strip and point words the plots do not use fail the frame**: `EDGE_STRIP_R` / `_L`, a destination-alpha blend factor, an
  unknown primitive, a vertex outside BEGIN / END (`plot raster: primitives the toolkit does not use fail the frame loudly`).

The plot gallery (screen `plot`, `sim/plot_screens.cpp`; header `sim/plot_screens.h`) has ten states, each in the standard chrome
and held to the 1792 words in every theme and both translate modes: `wave_l1`, `wave_l4`, `wave_list`, `curve_env`,
`curve_filter`, `scope`, `spectrum`, `meters`, `page` and `worst` (a sweep of seven variants: the widest vector wave with every
mark, the longest zoomed line, a curve with a full-size fill and sixteen handles, the scope as lines, 64 bars, sixteen meters,
the bitmap wave with every mark). The signals are built with integer arithmetic only (a parabolic sine, xorshift noise, a decaying
drum), so the pictures do not depend on a libm for their input; the plots themselves use single-precision `sinf` / `cosf` / `log2f`
for a ring, the response curves and the spectrum, and the hashes were identical between the sanitizer Debug build and a Release
build on this toolchain (a different libm could in principle move a vertex by a sixteenth: regenerate the goldens then, and look).
A state draws its bitmaps through `Renderer::display()` (RAM_G uploads are allowed inside an open frame), so a state is a pure
function of its variant: it uploads what it draws in the frame it draws it.

### Calibration (hardware session)

ADR 0001 plans a comparison against on-device `CMD_SNAPSHOT2` captures; until one exists the goldens are only
what the simulator says. **`hw-session` item:** capture the three screens of `scenarios/pattern_basic.txt`
(`initial`, `cursor_moved`, `tracks_8_12`) from a Tracker unit running these display lists and diff them against the
simulator's PNGs. What to look at: the corner pixels of rectangles (about 5 % expected), glyph positions at x >= 512
(the `CELL` + `VERTEX2F` path), the two `[A]` assumptions about VERTEX2II and handle persistence, scissor and
CLEAR behaviour, and the time to build and send a ~1600-word list (about 6.4 KB over SPI).

**First measurement (OS-SIMKEY, 2026-10-14; a 0.4.8 unit, the Pattern screen, an `8888` capture, Amiga theme): the unit does not draw RECTS as this rasteriser does,
and the difference is on every rectangle, not on the key row.** The key-cap row only showed it first (`play_meters_vu`: Chrome 178,186,202 against 99,105,115 in a 565 capture, 0.56 x).
- *Fills.* A pixel inside a rectangle is the colour at an alpha of **139 / 255 (0.545)** over what was under it: Chrome over the Pane (10,13,20) reads 102,107,119, Sel (28,92,208) over the Pane 20,56,122, a ChromeText
  key-cap over the Chrome strip 54,59,69, the Play badge (Play over ChromeDim over the strip) 82,150,99 in a 565 capture. The CLEAR colour and every L1 glyph and other bitmap pixel are exact (the Chrome F-number inside its
  key-cap reads 178,186,202, the Text colour 230,234,242), so it is the RECTS path, not the palette, the format or a dimmed row: the same colour token is 0.545 as a fill and whole as a glyph.
- *Edges.* The UI puts its vertices on pixel centres (`centre_q4`, `LINE_WIDTH` 8) and the chip puts the edges on pixel centres too, not on the boundaries: the key-cap designed as rows 460..477 reads rows 460 and 478 at an
  alpha of 0.375 and rows 461..477 at 0.545, a 1 px rule is two rows (or columns) at 0.375 each, a corner pixel about 0.25, and the strip's first column at x = 0 is 0.375 over the Pane. The numbers fit a rectangle grown by 11/16 px
  instead of 8/16 under the 0.545 ceiling, or a pixel centre on the integer coordinate (the shape moved half a pixel) and an alpha of 0.545 / 0.375 / 0.25 at a coverage of 1 / 1/2 / 1/4.
- *Not in the list.* The display list carries no `COLOR_A`, `BLEND_FUNC`, stencil or tag (the sim's `pattern_start`, 972 words: `CLEAR_COLOR_RGB`, `CLEAR`, `LINE_WIDTH 8`, 32 bitmap binds, `COLOR_RGB`, `BEGIN` / `END`,
  `VERTEX2F` / `VERTEX2II`) and the display driver writes the words as they are (`display.cpp` `write`). Nothing in the app dims a row.
- *Open.* Whether it is the chip's RECTS (a state at the start of a list, the vertex convention) or the snapshot (the pixel clock runs during `CMD_SNAPSHOT2`: H-2 in `platform/mk66/README.md`), and so whether the sim, the
  Renderer or the shot path should change. **Nothing is modelled and no golden moved**: modelling it would move every golden that draws a rectangle and would put a possible snapshot artefact into the reference. To separate
  the causes on a unit: does the screen itself (by eye, next to the sim's PNG) show the light Chrome? a `shot` with the pixel clock stopped; a frame with an explicit `COLOR_A(255)` after `CLEAR`.
  Until it is settled a dialog must not depend on its fill hiding the page: the File EXPORT page draws nothing that touches the progress dialog (OS-BUG-OGOVERLAY, `touches` in `app/include/o3os/app/frame.h`), after the unit's shots showed the page's lines through the box (the other screens' dialogs: not yet).

OS-003b adds: the tile faces and the origin move rest on **VERTEX_TRANSLATE_X moving VERTEX2II** (every glyph, tile and icon
past x = 511 in the pattern view, see the Fidelity row); a card that draws the 379 tiles and the icons and compares them with
the glyphs they stand for would settle that, the L1 bitmap strides of 3 and 4 bytes, and the 4 KB `upload` timing for the 25 KB
of tiles. The fallback is `Renderer::use_translate(false)`.

OS-003d adds, for a card that draws a wave both ways: the vector wave (two `EDGE_STRIP_B` fills with vertices on pixel centres)
against the L1 bitmap of the same columns, which must match pixel for pixel (the edge-strip row rule above), an L4 bitmap 740 pixels
wide (BITMAP_SIZE_H, nibble expansion), a `POINTS` disc at `POINT_SIZE` 22 / 40 / 80 and the 1 px `LINE_STRIP` at width 8 against
the simulator's coverage, a context restore of scissor and alpha after a raw section, and the time of the plots' uploads (a 13 KB
L1 picture is four 4 KB uploads, the largest L4 one 24).

OS-003a adds more `[A]` items, for a calibration card that draws known primitives: the coverage of LINES, LINE_STRIP and
POINTS (sample count, caps, whether `ONE` sees the coverage, whether a strip merges its segments), what a radius or
`LINE_WIDTH` of 0 draws, the line-width rule and missing antialiasing of edge strips, **whether `VERTEX_TRANSLATE` is always in 1/16 px whatever
`VERTEX_FORMAT` says**, whether it moves VERTEX2II, whether a transparent texel is drawn under `BLEND_FUNC(ONE, ONE)`, and
the time of a 4 KB `upload` (the SPI figure of ADR 0010). The card itself is a follow-up.
