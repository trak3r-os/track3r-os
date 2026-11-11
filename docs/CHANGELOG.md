# Changelog

User-visible changes of the Community OS, newest first, one entry per change, in English. The next release notes link here. A change that touches a saved file or the level of what you hear says so in plain words.

## v1.3.0 (date to be set at publication)

**Copy the new modules zip to the card.** TB-303, Chip, FM and SID now load from the card (`/modules/instruments/`); without them those instruments are muted and say `MODULE ... NOT LOADED`.

Everything below is merged and tested on the host and in the emulator. Where a line says "checked on the unit" a Tracker ran it. Where a line says "Unit check: see release notes", no unit result is in yet (see "Not yet verified" in the [release notes](release/v1.3.0.md)).

### New

- **MIDI recording into lanes.** A MIDI track records into lanes: notes and chords, sub-step timing and length, external CC, a Learn knob and the encoder recorded as a CC letter (CC A..F), the REC STORE setting, and the EVENTS list over the lanes. One undo per take. Unit check: see release notes.
- **Automate any parameter from the pattern.** With REC on, move a MIDI Learn knob or the encoder's learned parameter and the pattern moves that parameter on playback (CC A..F steps). Up to six letters per project. Unit check: see release notes.
- **LEARN MAP: one screen with every MIDI Learn binding** and the encoder's target. Open it with `F6` MAP in the Learn dialog or `F8` on Config > MIDI. Unit check: see release notes.
- **STATUS LED BPM mode** on Config (BLINK, BPM or OFF), saved with the device settings. Checked on the unit.
- **A bar under the name of each track that has an insert** on the MIXER page. Unit check: see release notes.
- **Advanced cheat sheet**, [manual/advanced.md](manual/advanced.md): a printable page of the advanced keys.

### Changed

- **The four instrument synths are card modules.** TB-303, Chip, FM and SID are no longer inside the firmware image (about 36 KB of flash freed). Copy `modules/instruments/` and `LICENSES/` from the starter zip to the card. **Overwrite older copies on the card:** an older `chip.tmod` (1.0.0) would now be used and lacks Chip waves 4..11. Unit check: see release notes.
- **Saved files.** The project format gains an `MEVT` chunk for the MIDI lanes. It is additive: an older firmware keeps it as an unknown chunk. The instrument file is INST 1.9 (CC letters); an older firmware says "newer fields skipped", but nothing is lost.
- **Module ABI unchanged** (1.13).
- **Pattern: `F6` opens EVENTS** directly. RE FILL / REPEAT moves to the verb menu: `F7`, then `F6`. Checked on unit b11: `F6` opens EVENTS; `F7` then `F6` runs the repeat.
- **Pattern tools.** The TOOLS panel holds Humanize, and `F6` repeats the last FILL or TOOLS apply. Unit check: see release notes.
- **Chip.** The Wave view draws waves 4..11 and the Envelope view names the filter direction. Unit check: see release notes.
- **MIDI Time Code checked on a unit over USB.** MTC OUT at 24, 25 and 30 fps, and MTC IN, follow within about 1 ms once locked; a re-lock takes about 1.75 s. DIN and Logic Pro: see release notes.

### Fixed

- **Master + Insert opens the Mixer's INSERTS page** from any screen (issue #5). Checked on the unit.
- **Shift + pad plays the instrument** on the Instrument screen instead of selecting it (issue #9). Checked on the unit.
- **Slice pads play their own slice** (issue #7). Pads checked on the unit; the Slice FX in SLICE mode needs more testing.
- **Quieter power-off** (issue #8): the codec's reference voltage ramps down on its own first. Checked by ear on the unit.


## v1.2.0 (date to be set at publication)

Everything below is merged and tested on the host and in the emulator. Where a line says "checked on the unit" a Tracker ran it. More testing is needed on units (see "More testing needed" in the [release notes](release/v1.2.0.md)).

### New

- **Insert effects, per track and on the master.** The Mixer has an INSERTS page: each track has two insert slots and the master has one. Pick an effect module, open its parameter page (`F6` PARAMS), set its values by hand or bind them with MIDI Learn; a small mark by the track name on Master shows which tracks have an insert. Inserts are saved with the project (an optional chunk; a project without inserts is saved as before) and come back after a reload. A heavy insert is forced to dry by the CPU guard instead of causing underruns. Checked on the unit: an insert on Dense8 (about 1.2 % of a block), the tail after Stop, save and load, the INSERTS page. New Community OS behaviour.
- **Eleven effect modules for inserts, on the card.** DRIVE, CRUSH, COMP (with a TRANSIENT mode and a sidechain KEY from another track), ENV FILTER, FOLD, PHASER, BIQUAD, and the stereo effects PING-PONG (tempo-synced), WIDE, CHORUS and AUTOPAN. They are `.tmod` files in `modules/effects/` of the content zip, with factory presets. Their cost on the unit is measured for DRIVE (1.9 % of a block), BIQUAD and a keyed COMP track (about 740 cycles); the others are pending, and CHORUS measured 5.4 % of a block on the unit, above its 4-point budget. Listening checks are not done.
- **Stereo effects and sidechain.** An effect can be stereo (a track can come out wider than its mono source) and can read the level of another track as a key. A stereo insert counts against the four-stereo-voice cap of stereo samples.
- **Module presets.** Shift+F5 SAVE on a module or insert page writes the parameter set to `modules/presets/<module>/<name>.o3mp` (a taken name gets ` 2`..`99`); the F5 list has a CARD section and `F1` loads a preset by parameter name. Factory presets for TB-303, FM, Chip, SID and the effects come in the content zip. A new file format (`.o3mp`, version 1.0).
- **RATE for instrument modules: FULL, 1/2, 1/4.** On the MIX page (Shift+F2) of an instrument module, the module renders at half or a quarter of the sample rate and each sample is held: crunchy, Amiga-like, and cheaper in CPU. Sine, FM, TB-303, SID and Chip offer it; a module that does not say it can run at any rate is never offered the row. The rate is saved with the instrument (INST 1.8, see below). Changing it waits for a quiet moment so a held note is not cut. What you hear changes with this setting. New Community OS behaviour.
- **PANNING, DELAY SEND and REVERB SEND on module instruments** (TB-303, FM, Chip, SID and any instrument module), on the Shift+F2 MIX page.
- **Chip: eight more waves.** The Chip engine adds waves 4 to 11 from the CHIP synth of Polyend's published source (among them a SID-style wave, LFSR noise, SuperSquare, FM bell and a chip drum), a fourth page, and a post chain. Chip's older waves and saved projects sound as before. MIT component, credited in `THIRD_PARTY.md`.
- **Sample Editor plugins (`.ptfx`).** The Sample Editor has a PLUGINS page that lists `Effects/*.ptfx`, shows a plugin's parameters and applies it (undo works, stereo samples are handled). The plugins run unprivileged: a fault, a hang or a crash inside one is contained and the plugin is quarantined, not the unit. Thirteen plugins come in the content zip: Add Silence, Analog Kick Gen, Chiptune Voice Gen, Multi-Mode Filter, Stereo Delay and Stereo to Mono (the examples of Polyend's published `editor-fx-sdk`), and Overdrive, Delay, Bitcrusher, Chorus, Flanger, Limiter and a three-band EQ (rebuilt from Polyend's `core/effector`). Checked on the unit: nine plugins run, undo, a fault and a hang are contained, Stereo Delay and Stereo to Mono on a stereo sample.
- **Wavetable envelopes and LFOs.** The MOD page has WT ENV and WT LFO sources (on amplitude, filter cutoff, pan, finetune and wavetable position), a contour drawing, and user contours from `/Envelopes/*.wav`; granular length can be a modulation destination. New step effects `w` (WT amp envelope), `y` (WT filter envelope) and `z` (granular length). Contour shapes are Polyend's `mtWavetableEnvelopes`, bit-exact. A project that uses a WT slot is saved with INST 1.6; any other is as before. Checked on the unit: the worst CPU arm was +0.72 points; the listening is not done.
- **Play Thru for Slice and Beat Slice.** A slice keeps playing through the next slices until note off or the sample end, as an option on the instrument and in OG files (INST 1.7 only when the flag is set).
- **Chance conditions beyond 0..100.** FX 4 takes 101 (group with last), 102 (not last) and 103 and up for "play N, skip M" and "skip N, play M" (N, M from 1 to 6), with the values of the official format, so OG files keep their meaning.
- **Perform: custom play orders C1 to C20.** PLAYMODE takes 0 to 23. The orders are Polyend's published data, and the sequencer plays them with their rolls. PERF and SCEN are written as 1.2 only when a slot uses one; any other project is as before.
- **MIDI Time Code, out and in.** Config has MTC OUT (Off, 24, 25, 30 fps) and MTC IN (Off, DIN, USB). The unit sends quarter-frame messages from the song timeline on the ports CLOCK OUT names, and follows an incoming MTC with a re-lock after a jump, so a DAW that syncs by MTC (Logic Pro) can follow it. MTC carries no tempo: set the same tempo in the DAW. The settings file grows by appended bytes with defaults Off. The official firmware has no MTC. New Community OS behaviour.
- **MIDI clock follow, tighter.** The clock DLL settles a tempo jump in about 0.1 s in simulation, an SPP that arrives with a Stop is honoured on Continue, an SPP received while playing relocates at the next step boundary, a step waits for a real clock, and the master's priority and lateness are handled. The loop wrap of a Song loop relocates MTC. A battery with the computer as master passed 14 of 17 tests on the unit before these fixes; the rerun is pending.
- **MIDI event list (EVENTS) on Pattern.** An edit-through list of the steps of a MIDI track: notes, CC A..F and micro-moves, with FILTER, a play marker, PREVIEW, QUANT, and the FX1 and FX2 columns on NOTE rows (type and value, editable, one undo per edit). No format change.
- **Official MIDI CC map.** A CC MAP device setting gives the 32 CCs the official manual documents for the mixer, Perform and levels, and the CC row of a Perform slot. Documented behaviour of the official firmware.
- **MIDI Learn: follow and encoder.** In the MIDI synth mode Learn follows the selected instrument (F5 FOLLOW); the chord Instr+Fx on a row puts that parameter on the encoder (one target), and a recorded Learn move or encoder move is written into the FX columns of a LIVE or STEP take. Checked on the unit: ENCLEARN in LIVE and STEP recording.
- **Pattern tools in one grammar.** Select, then `F7`, then a verb. A right-hand panel holds the verb menu and the TOOLS dialog; TRANSPOSE and INSTRUMENT dialogs; Fill works on the selection; `F6` repeats the last FILL or TOOLS apply; CLEAR is refused on a partial column; the APPS page lists only the tool apps. Checked on the unit: the TRANSPOSE and INSTRUMENT dialogs, refusals and `F6` repeat.
- **Apps list and Navigator.** Shift+Pattern opens one list of the module apps that are not tools (`F1` launches one, `F8` is BACK on an empty list); the Navigator opens with Master+Song or Song+Pattern.
- **Pattern selection.** Shift+Up at step 0 selects the cursor's column first, then the track, then every track; Shift+Note, Instr, Vol and Fx in edit mode also select the rows they jump over; Shift+Delete follows JUMP; a selection across tracks is drawn dimmer outside the cursor's track.
- **Song: editable TIMELINE.** A cursor and rectangle on the lanes, PREVIEW (`F1` in a lane auditions it), ADD EMPTY slot, ORDER and ARRANGEMENT folded into LIST, and a loop of a range while editing. 12 lanes show on both pages, the window scrolls to tracks 13 to 16.
- **tracker-ce files.** The load strip names what a tracker-ce folder lost; 16-track version 5 and version 6 pattern files are read (read only); the official scale numbering and 39 scales on the held-Note panel (NOTE 1.1).
- **Record: COUNT-IN and status chips.** A COUNT-IN row (OFF, 1, 2, 4 bars) clicks with the music before a take, also with the metronome off; the top strip shows MONITOR, REC, OVERDUB and LOOP chips while each is on.
- **Status LED option.** Config can turn the heartbeat off (session only; it also hides the over-budget code 12). Checked on the unit.
- **Importers moved to the card.** MOD, S3M and MED import is now `modules/importers/tracker.tmod`: **copy it to the card or IMPORT refuses.** With the module missing, the project that is open and its modules stay loaded. Checked on the unit: MOD, S3M and MED import with the module, and the refusal without it.

### Changed

- **Flash diet.** Text, glyphs, help pages, key bindings, the boot logo and built-in modules are stored compressed and unpacked at boot, which freed enough flash for this release. Nothing here changes how a screen behaves. Checked on the unit up to the FX picker and custom play orders; the unpack of key bindings and console text is pending.
- **Master scratch.** The scratch follows the hand: held speed between detents, 70 ms of inertia, a gentler gate near standstill and an anti-alias filter above 1x. What you hear changes.
- **Module ABI 1.13** (was 1.10 at v1.1.0): effects with stereo, the tempo and a KEY (1.12), importers (1.11), per-instrument RATE (1.13). Modules of ABI 1.0 to 1.10 still load; a module of a newer minor than the OS says NEEDS NEWER OS.

### Fixed

- **A loaded project keeps its tempo** (OS-BUG-TEMPOLOAD): a project saved with no instrument opened at 120 BPM. Merged and covered by a test.
- **An image that fills the last flash sector hung the unit** (OS-BUG-LASTSECTOR): an image ending at 0x100000 left the unit not coming back after the update. The device validator and the image check now refuse an image that reaches sector 0xFF000.
- **A `.ptfx` whose init is at payload offset 0 is a valid entry**, and the console names the reason when it refuses one.
- **An export no longer carries live effect tails**, and a project open or NEW clears the old toasts.
- **Scratch** (see Changed).

### Formats

- **INST 1.6, 1.7, 1.8** append the WT bytes, the play-thru byte and the module RATE; each is written only when the instrument uses it, so projects that do not are byte for byte as before. A project saved with them opens in v1.1.0 but loses those values on its next save. **PERF and SCEN 1.2** hold C1 to C20 only when used. **NOTE 1.1** carries the extra scales. New files: `.o3mp` presets (1.0). The settings file grows by appended bytes (MIDI CC map, MTC OUT, MTC START, MTC IN) with default fill.

### For developers

- Module ABI 1.11 to 1.13 and the SDK: effect templates `effect_stereo` and `effect_key`, preset lists in `module.json`, `"rate": true`. The `.ptfx` tools and packer (`tools/ptfx_pack.py`, `tools/ptfx_inspect.py`) are rebuilt from the format facts; the content zip is built by `tools/release/make_starter.py`.

## v1.1.0 (date to be set at publication)

### New

- **SID-style instrument, built in.** A new sound engine next to TB-303, FM and Chip, in the ENGINE picker with no card file: saw, triangle, pulse with PWM, noise and combined waves, sync and ring from an internal modulator, a stepped ADSR and a low-pass, band-pass or high-pass filter with a 6581 or 8580 model switch. Its pages and views were checked on the unit and its CPU cost is about 4 to 5 % of a block a voice (measured on a test build). New Community OS behaviour.
- **Stereo samples, opt-in per instrument.** Load a stereo file with STEREO on Sample load, then set the instrument's STEREO row to STEREO: the note plays its left and right sides. At most four stereo voices play at once; the next ones play the mix. Stereo plays only forward, on the Modern engine, with no filter, Overdrive, Bit depth or tremolo. The sample edit page works on both sides; COMPRESSOR and the Amiga conversion ask you to make it mono first, and a `.pti` export keeps the mix only. Four stereo voices cost about 4 percentage points of the CPU on the unit. A project that uses it is saved with a newer instrument version; any other project is byte for byte as before.
- **USB audio outputs of 2, 4, 6, 8 or 10 channels, per project.** On Mixer, STEMS, the OUTPUTS row sets the number the computer sees; `F6` twice applies it and the unit reconnects. The USB return stays only below 10 channels. Checked on Linux; macOS and Windows are not checked.
- **Song: STAGE and TIMELINE pages.** TIMELINE draws the song as blocks as wide as the slots are long, with the pattern's color, a lane per track with a tick for every step that holds a note (the note map), and an ENERGY row; at most 12 tracks are drawn. STAGE shows the playing slot and the next in large type with a countdown; it is no longer in the Song key cycle, and the countdown is on TIMELINE's info line while the song plays.
- **Song: BRIDGE puts transition slots between two patterns.** In the Song screen's TOOLS dialog (`F6`) the OP row has one more value, BRIDGE: put the cursor on a slot (or select two), set SLOTS from 1 to 16 and press `F1`. The new slots go between the slot and the next one, each a new pattern in which more and more steps are the next pattern's, so the music moves from one to the other step by step; the same two patterns always give the same bridge. Nothing is written when the song, the pattern pool or the undo memory cannot take it, and one undo takes it all back. Checked on the unit except by ear.
- **Song: FORM builds a song shape from 1 to 3 patterns.** In the Song screen's TOOLS dialog (`F6`) the OP row has a new value, FORM: choose LOOP>SONG, POP or TECHNO and how many tracks are drums, and `F1` adds the form's 8, 9 or 10 slots after your selection. The first slot is the pattern with some tracks cleared (the music in LOOP>SONG, the drums in POP, all but the kick in TECHNO), a break the pattern without the kick, and so on; the changed versions are new patterns, your own are not touched. The footer shows the plan; when the song, the pattern pool or the undo memory cannot take it, the strip says so and nothing is written. One undo takes it all back.
- **Import: module files are recognized by their header, and 15-sample Soundtracker MODs load.** The File screen reads the first bytes of a file, not its name, to know what it is; a file no reader takes says NOT A MODULE THIS OS READS and changes nothing.
- **Step effects 23, 24, 33 and 36 start their LFO.** As FX 35 does, on a sampler track. Checked on the unit by capture for 23, 24 and 33; FX 36 on wavetable and granular instruments is not checked by ear.
- **Config and Record: Left and Right edit the value under the cursor** (`Shift` for the coarse step). The pages change only with the F-keys or the screen button.
- **Update from the unit pads an older image after asking.** An image that does not end on a 4 KiB boundary may crash at boot (see Fixed); v1.0.1 and v1.0.0 are such images. When that is the only fault, the Update screen (Config, INFO, `F8`) says NOT PADDED. TRACK3R-OS WILL PAD A COPY OF IT? and `F1` (**PAD**) writes a padded copy, `pad-<name>.ptf`, next to the file, checks it and offers to install it. The file you chose is not changed, and an existing copy of that name is never replaced. The `diag` console takes `fw arm <name> <crc> pad`, and `o3console.py --pad` pads on the computer. Any other fault is still refused. New Community OS behaviour. On the unit the pad job made a padded v1.0.1 in about 15 seconds (through the console), the copy booted, and the unit came back to v1.1.0.

### Changed

- **Cheaper reversed voices.** PLAYBACK and the Reverse step effect (26) take a faster path with the same sound (bit-identical on the host): six reversed voices cost +0.23 percentage points on the unit, against +6.19 before.

### Fixed

- **Overdrive and Bit depth shape module instruments** (issue #2). Step effects 38 and 39 and the Perform OVERDRIVE and BIT DEPTH rows did nothing on TB-303, FM and Chip tracks; they now shape them with the sampler's curve.
- **The update dialog reaches every file of `Firmware/`.** It pages through them twelve at a time and shows `FILE n / total`; the old limit of the first 12 files is gone.
- **Update file names over 30 characters are refused.** With a longer `.ptf` name the card update could install the first `.ptf` of the `Firmware` folder instead, with no error. The Update screen now shows `NAME TOO LONG: 30 MAX` for such a file and asks nothing; rename the file to 30 characters or fewer (`.ptf` included). `o3console.py` refuses the same names.
- **A boot crash caused by the image's last flash sector.** The SD bootloader leaves a short final 4 KiB sector as it was, so a development image once crashed at boot on stale bytes. Images are now padded to a whole sector and the image gate checks it; the Update screen offers to pad an unpadded one (see New).
- **Saving after a card remount.** A slot skipped during revalidation keeps its check pending, so a save never points a project at a missing or changed WAV.
- **The demo kit no longer overwrites a project's instruments.** A project of only module or MIDI instruments lost slots 1 to 4 at boot; the kit now loads only when the project has no sample or module instrument and nothing in slots 1 to 48. New Community OS behaviour.
- **MOD import.** A 15-sample Soundtracker file's loop start is read in bytes, and a MOD with no room for one pattern is refused before the NEW question. Both passed on the unit.
- **Chip and SID labels.** Chip's Wave view draws its trace below the wave's name, as SID's does; the label on SID's Wave page is right.
- **Config: the `APPLY?` mark** on `F7` is honored only while its question is on the strip, and a later `F7` asks again.
- **TIMELINE draws at most the 12 tracks that Pattern can show.**

### For developers

- **Effect inserts, console only (no screen yet).** An effect module (kind 2) can run as a track insert, bound from the `diag` console after `mod load`: mono, up to four at once, with a tail rule and a CPU guard that forces a heavy one to dry. Inserts are saved in the project as an optional chunk and come back after a reload. `biquad` is the first sample effect module (a filter with mix; its cost on the unit is not measured). Nothing in this is a feature for users yet.

## v1.0.1 (date to be set at publication)

### Fixed

- **Pads on later Trackers (plastic back, boards Control 4.0 / Grid 2.0).** On those units the pad grid did not answer after the install and the status LED flashed 13 times at power-on. The pad board is now started later and more gently: its power comes up first with a short wait, the pad chips are first contacted about a second after power-on, the pad bus (I2C2) has a glitch filter, a refused request no longer resets the bus, and the pad chips are asked again once a second until they answer. A user with an affected unit confirmed on issue #1 that the pads and note entry work. New Community OS behaviour; no saved file changes. On a first-batch unit it behaves as before.

## v1.0.0 (date to be set at publication)

### New

- **Perform: PLAYBACK acts.** Hold a pad on `<<<` and the next notes of the affected tracks play backward, from the last frame of the sample (or of its slice, or of the SMP POS / SMP END window) to the first, and stop there; a sounding note is not turned round. Sampler one-shots and beat slices only. The step effect Reverse (26) does the same; held when you press Stop, it stays on the track until the next fresh Play, as the other held step effects do. A new project stores PLAYBACK as 1 / 1 / 1 where it stored 0 / 0 / 0; no version bump, older projects read as before.
- **Perform: VOL LFO, PAN LFO, TUNE LFO, FLT LFO and POS LFO act.** Put them on a slot, hold a pad and turn the encoder: the value moves the speed of the track's LFO for that target up or down the instrument's speed list. Where the instrument has its own LFO, its shape and amount stay and only the speed moves; where the slot is Off or an envelope, an LFO runs with the shape and amount stored on the instrument's LFO rows (a new instrument: triangle, 50 %), and as a triangle at 50 % when that stored amount is under 5 % (older projects and imported instruments often store 0). FLT LFO needs a filter that is on; POS LFO moves wavetable and granular instruments only; the other four are for sampler instruments. A new project and a changed effect start these rows with -8, -3 and +3 (slow, medium, fast) in place of three empty values; no version bump, saved files and exports are unchanged. With all five held on twelve tracks the CPU rises by an estimated 6 to 7 % of a block; with them off it costs nothing.
- **Perform: SMP POS and SMP END act.** They set where the next notes start and stop in the sample, as percentages from the start or the end. A looped sample ignores SMP END. Sampler instruments only: granular and wavetable instruments ignore both. New slots start with 25 / 50 / 75 (SMP POS) and 75 / 50 / 25 (SMP END). No saved file changes.
- **Perform: each track can queue its own pattern.** The change lands at the end of that track's own loop.
- **Perform: preset values on the three rows of every slot.**
- **DJ: TAPE STOP length.** Turn the encoder while the pad is held, from 1/8 to 4 beats.
- **Update from the unit.** Config, INFO, `F8` (UPDATE) installs a `.ptf` that is in the card's `Firmware` folder, with two questions and no computer.
- **Sample edit: FILTER and COMPRESSOR** on the FX page. `F8` is CANCEL while an effect runs and leaves the sample unchanged.
- **Sample edit: the free sample memory in KB, and the instrument name in the header.**
- **Sample load: mark several files and load them in one go.**
- **Sample load: the target instrument is on screen** and changes with `Shift` + `F7` / `F8`; the pads choose it and play it.
- **Sample load: a bar for the sample memory, with KB used and free.**
- **A replaced sample gives its memory back.**
- **Record: START ON LEVEL,** with THRESHOLD and PRE-ROLL rows.
- **Record: TIME LEFT,** the longest take the sample memory still holds.
- **Pattern: select a block of columns.**
- **Pattern: `Shift` + `F1` chooses the octave of the pads.**
- **Config, MIDI: TRANSP IN and TRANSP OUT** (default On).
- **Config, PATTERN: PAT SWITCH** (Step or Pattern end) for the held PATTERN chooser.
- **Instrument: Left and Right edit the highlighted parameter,** `Shift` for the coarse step.
- **Instrument: a Sine LFO shape.** A project that uses it is saved as instrument version 1.4; any other project is byte for byte as before. An export to the original (OG) format writes it as Triangle and lists the loss.
- **The LFO picture shows the speed** over a 16-step window.
- **File: the header says UNSAVED,** the list marks the open project with a bullet, and RESCAN tells the count.
- **File: the last project opens at power-on;** a damaged one is refused with a message.

### Changed

- **The audio clock is exactly 44,100 Hz** (the old clock ran about 0.04 % faster).
- **DJ: the scratch platter** follows the encoder by its timing, with a short inertia and a smoother read, and costs nothing while the hand is still.
- **DJ: the pads of row 4** move in steps of 10 %, and both centre pads sit at 50 %.
- **Pattern: the pad modes moved one key along** to make room for the octave on `Shift` + `F1` (NOTES `F2`, PIANO `F3`, ISO `F4`, TRACKS `F5`, PATS `F6`).
- **Pattern: `Rec` + `Play` together** run LIVE recording, as `Shift` + `Rec`.
- **Pattern: 3-letter FX names** at the 5-track zoom.
- **Instrument: CUTOFF and RESONANCE are dim** while the filter is off; the loop marks show only in the loop modes.
- **Mixer, Config and File: the pads play the current instrument.**
- **Long file and folder names are cut in the middle.**
- **Less CPU in the sampler, the Amiga engine and the sequencer.** When a project is too heavy, the unit keeps answering its keys, and Stop works.
- **Song: inserting, deleting or moving slots while the song plays keeps the play position,** and a DJ transition ends when the order list is edited.

### Fixed

- **Audio:** a single late audio block no longer causes an underrun.
- **`Play` no longer waits for the `Rec` + `Play` chord,** so the transport starts aligned with the pads.
- **Sample load:** a file over a used slot asks REPLACE first; a module slot and a MIDI slot refuse; the `F5` preview starts at once and follows the cursor; the cursor stays over a visit to File; the multi-load summary stays on the strip.
- **Slice mode:** pads played the slices at the wrong octave; ADD SLICE added at the wrong place.
- **Pattern:** Shift + the arrows now select while the cursor follows the play line; a pad note or a note change no longer touches the pattern outside the selection; a note written over a note replaces the instrument too; `F6` INVERT outside Edit says TURN EDIT ON; `F7` TOOLS while playing says why it does not open; Insert and Pull leave a track alone that does not reach the block.
- **Record:** SAVE opens the name keyboard; DISCARD and a new take ask over an unsaved take; a take counts as saved only when its WAV is written; in the loop review `F6` is inert.
- **Sample edit:** the FILTER and COMPRESSOR effects warm up from the loop end on a looped sample.
- **Perform:** a pad held across Master or Song is let go at its release.
- **Mixer:** the STEMS Delete question closes with the page; ROUTING rows past the tracks say TRACKS ONLY.
- **File:** a corrupt project leaves the open project untouched.
- **Messages where a key looked broken:** TYPE A NAME on an empty name; NOTHING RECORDED on Record SAVE or DISCARD with no take.

### Earlier in development

The changes below were made before the final round of this release. They are part of v1.0.0.

#### Module apps remember their settings with the project (OS-APPUX S3, ABI 1.10)

- **The shipped apps keep the values of their rows per project.** An app stores a row with the new `set_own_param` call; the project file keeps the rows of the last twelve apps used (it was six) and hands them back to the app at its next launch. An app built at ABI 1.10 gets them before `enter()`; an app of 1.2 to 1.9 gets them after it, as before.
- **Compatibility: a saved file changes only when more than six apps are stored.** With six apps or fewer the project is written exactly as before and an older OS opens it. From the seventh app the `APPM` chunk is written as version 1.1, which an older OS refuses with `APPM: bad app count`, and it then refuses the whole project: such a project needs this OS or a newer one (maintainer decision, option B).
- **For app authors:** a screen button on an app page leaves the page for its screen, whatever the app's ABI, and an app that declares no rows now receives the Shift keys. `set_own_param` called inside `exit()` is not kept.

#### Help pages: Shift + Config (OS-HELP)

- **Shift + Config opens a help page for the screen in view.** There are 15 pages (the keys that work everywhere, then one for each screen, with the DJ page of Perform); it opens at the page of the screen you are on. Up and Down (or the encoder) scroll a line, Shift with either a screenful, Left and Right turn the pages; F8, any other button or a pad closes it, and the press that closes it does nothing else (a pad that closes it plays nothing, and its release does not end a latched Perform effect). Plain Config is unchanged. No saved file changes.
- **Behaviour change for module apps: the OS keeps Shift + Config, and an app gets no input while help is up.** On an app's page the chord opens the help page, and the app is not given the Config press; while the page is up it is given no key, pad or encoder turn (a key held when it opened still has its release delivered). Like F8 and Play in ABI 1.9 this is a rule of the OS, not an ABI change: an app of any ABI version keeps loading and running.

#### Perform: TUNE (OS-PERFFX P5)

- **TUNE, the third effect of the Perform screen, now moves the pitch.** Put it on a slot (it is the third slot by default), hold a pad and turn the encoder: the value is a number of semitones from -48 to +48, `---` at 0, and the tracks that are on play that much higher or lower. A note that is sounding follows at the next block, with no glide; a note you start later starts at the new pitch; 0 gives each note its own pitch back. The slot is no longer dim.
- **It adds to what the note already has.** The instrument's tune and fine tune stay, and the Slide and Glide step effects move the pitch around the held value and end on it. It acts on sampler instruments; Amiga-engine instruments and modules do not follow it yet.
- **Saved projects and exports.** Nothing changes in the files: the slot and its values were already saved. Like the other Perform effects it stays through Play and Stop, and an export is not moved by it.

#### Perform: BITDEPTH and OVERDRV (OS-PERFFX P1)

- **Two new Perform effects, BITDEPTH and OVERDRV, which act on the tracks that are on.** Put them on a slot with `Up` / `Down` in EDIT (they are the last two of the list); a pad or the encoder sets the value, like any other effect. **OVERDRV** is 1 to 100: the notes of the track are driven harder as it rises, with the curve of the instrument's OVERDRIVE parameter. **BITDEPTH** is 1 to 15, the number of bits taken away (16 minus the bits that stay): 1 is a trace of grain, 15 leaves one bit; 0 is the clean sound and ends the effect, as a 0 does for every Perform effect. The Perform value wins over the Overdrive and Bit depth step effects (38, 39) of a pattern and over the instrument's own setting, and when the pad ends the step effect comes back, then the instrument's value. Play, Stop and the pattern's own effect reset leave the held effect on; a WAV export is not shaped by it (byte for byte what Perform off makes), and the live notes after the export are; opening a project lets every pad go. Like the step effects 38 and 39 they shape the tracks the sampler voices play (a MIDI or module instrument is not shaped), and an instrument on the 12-bit or the 8-bit engine caps BITDEPTH: its notes keep at most 12 or 8 bits whatever the value, so BITDEPTH can only crush it further and a value that leaves more bits than the engine does changes nothing on it (the engine's rounding stays). New Community OS behaviour; the cost on the unit is pending (a few per cent of a block with the effect on all 12 tracks, nothing with it off).
- **Saved projects.** The two effects are ids 20 and 21 of the Perform slots and of the scenes in the project file, in the byte that was always there. The PERF and SCEN parts of the file are written as version 1.1 only when a slot uses one of the two; every other project is written as version 1.0, byte for byte what it always was. This build opens both. An older build opens a project that uses them (its load line says "newer fields skipped"), shows those slots empty, and saves them empty: a downgrade through a save is not lossless (`docs/formats/o3-project.md`). Host tests only so far: the check on the unit is pending.

#### Shift then Power saves a screenshot (OS-SCREENSHOT)

- **Hold Shift, press Power, let go of both: the screen is saved to the card as `Screenshots/SHOT_0001.bmp`.** The picture is the first frame drawn after both keys are up, so the key row shows its plain labels, not the Shift ones. The strip says `SCREENSHOT SAVED: SHOT_0001.BMP`; the next one is `SHOT_0002.bmp`, up to 9999, and a file is never overwritten. The file is a plain 800 x 480 bitmap (16-bit colour, 768,066 bytes) that any computer opens.
- **The screen freezes for about one to two seconds while it is written; the sound goes on.** A key press does not stop it, and the keys, pads and encoder do nothing meanwhile (a key already held still ends normally when you let go). It stops, deletes the half-written file and says `SCREENSHOT FAILED` if the card goes away, the card is handed to the computer, or nothing moves for 3 s.
- **It says `NO SCREENSHOT: <why>` and does nothing** with no card, with the card on the computer, with a card that cannot be written, while a save, an export or a load holds the card, while the console's `shot` runs, or when `Screenshots/` is full. Power held on its own still switches the unit off after 2 s, and Shift pressed during that hold still cancels it.
- Known: a capture reads flat fills at about half of their colour (the display chip's snapshot, OS-SIMKEY); the layout and text are right, the shades of large filled areas are darker than on the screen.
- Card file: a new folder, `Screenshots/`, and the files in it. Nothing else on the card is read or changed.

#### Master volume and Song peek from any screen (OS-MASTERVOL, OS-SONGPEEK)

- **Hold Master and turn the encoder to change the master volume, on any screen.** One detent is one step of the Mixer's MASTER level (0 to 100); the engine hears it at once, and the strip says `MASTER VOLUME 79%` (also at either end of the range). It is the same value as the MST column of the Mixer, which has no undo; Shift + the encoder is not a coarse step here. A turn during a LIVE recording does not end the take. Pressing Shift after Master ends the gesture (the tap is lost and the encoder turns as usual for the screen); a Shift held before Master is ignored. New Community OS behaviour.
- **Hold Song to look at Song, let go to be back where you were.** After 0.4 s the Song screen comes up and stays while the key is down; the release returns to the screen you came from, one level, with its page and cursor as they were. Held on Song itself it does nothing; a screen key pressed under the held Song ends the peek and you stay on that screen; pressing Shift after Song loses the tap. The peek is a real screen change and back: a question open on File (F3) is closed, a USB audio value waiting to be applied is dropped and the Sample Load list is fitted to the card again. New Community OS behaviour.
- **Behaviour change: the tap of Master (and of Shift + Master, the theme) and the tap of Song now act when the key is let go, not when it is pressed.** The keys wait to learn whether the encoder is turned or the key is held; a quick tap feels the same, one frame later. On Song, a press held 0.4 s or longer no longer turns the screen's next page (it is the peek). No saved file changes.

#### Module apps: the key row says what the keys do; F8 is BACK (OS-KEYROWDLG S5, ABI 1.9)

- **An app's page now labels its keys.** Tap Calc, MIDI Monitor, Loudness VU, Rhythm Gen, Humanize, Chords, Mutate, Morph and SMF Export show their F1-F7 words on the key row (and Shift+F1-F7 while Shift is held); a key an app does not label reads `-`. Tuner, Scope and the Pads example keep the plain row `-- -- -- -- -- -- -- BACK`. MIDI Monitor gains F1 = pause and F2 = clear (the same as Insert and Delete), Loudness VU F1 = clear (the same as Delete), so their labels are true.
- **F8 is BACK on every app page, and Play runs the transport.** F8 returns one level (MODULES, or Pattern when the app was opened from TOOLS); Play starts and stops the sequencer and the app no longer sees it. The chord F7 + F8 still leaves, and is the way out of a hung app. Pads and Rec still reach the app. **For app authors (ABI 1.9, additive):** `o3m_ui` has a sixth member `keys`; write `struct_size` with `O3M_UI_SIZE(minor)` (20 bytes up to 1.8, 24 from 1.9), not `sizeof(o3m_ui)`; an app of 1.1-1.8 loads and runs as before except that it is never handed F8 or Play. An app of 1.9 needs this OS or newer.

#### The audio tap stores samples only for an app that reads them (OS-BUG-TAPWRITE)

- **An app that reads only `tap_levels` (the Loudness VU meter) no longer makes the render store the tap's samples**, which was the largest part of the tap's cost (image int.6 read a mean of 1,765 and a worst case of 6,077 cycles a block, 0.7 % and 2.3 % of a block). The levels are measured exactly as before. TUNER and SCOPE SPECTRUM still read samples and still pay what int.6 measured; that cost is accepted as measured. The new figure of Loudness VU is to be measured on the next image.
- **For app authors (ABI 1.4, no minor bump): the first `tap_latest` or `tap_stream` call of an app returns 0, there is no history before it, and the real frames begin two blocks (2.9 ms) later.** A stream no longer begins at `tap_select` but at its first call. Poll every frame and treat 0 as "not yet"; the shipped TUNER and SCOPE SPECTRUM already do. Nothing in a saved file changes.

#### Perform: DELAY and REVERB, and the step FX 30 and 32 (OS-PERFFX P3)

- **The DELAY and REVERB effects of Perform now act, on the tracks that are on, and are no longer dimmed.** The value is added to the track's delay (or reverb) send as a share of a full send: +30 on an instrument whose send is 40 sends as 70, +100 is a full send whatever the instrument's, -100 closes the send, and 0 gives the instrument's back. Like the instrument's own send it works on a default route and is not gated by the routing bit; the master DELAY and REVERB levels and mutes, the track's mute and a solo elsewhere still close it. **The step FX 30 (Delay send) and 32 (Reverb send) now act as well:** the value 0 to 100 is the track's send in place of the instrument's, from its step until the FX ends (the next note step, FX Off, another FX in its column, or the next Play; a Stop does not end it, so a note you play by hand after a Stop still takes it until the next Play, as the Cutoff FX does, and an export ends it), and a Perform slot adds to it. Both Perform values stay on through Play, Stop and a new transport start, are set apart for an export (the WAV is what the project makes with Perform off, byte for byte) and come back at once afterwards. New Community OS behaviour; no saved file changes. Host tests only so far: the check by ear and the CPU on the unit are pending.

#### Perform: VOLUME and PANNING (OS-PERFFX P2)

- **The VOLUME and PANNING effects of Perform now act, on the tracks that are on, and are no longer dimmed.** **VOLUME** cuts: -100 is silence (the track's delay and reverb sends too), -50 is a quarter of the level (-12 dB), and 0 and anything above is the track as it was; it never boosts, because a track's loudest notes are limited before the master and a boost would only clip them. **PANNING** is added to the track's pan in pan units, so +25 halves the left channel and +50 and above is the right side only (negative values go left). Both stay on through Play, Stop and a new transport start, are set apart for an export, and come back at once afterwards. The tracker effects (channel volume, the Panning FX, the panbrello) keep working under them: the volumes multiply, the pans add. New Community OS behaviour; no saved file changes (the effects were always in the file). Host tests only so far: the check on the unit is pending.

#### Perform and exports (OS-PERFFX P0)

- **A Perform filter you left on no longer colours an export, and an export no longer drops it.** Before, a LOWPASS, HIPASS or BANDPASS slot held in Master or Song filtered the notes of a WAV export or of the stems, and a Cutoff FX in the exported song could switch the held filter off for good. Now an export is what the same project makes with Perform off, byte for byte, and the held filter acts again as soon as the export ends. New Community OS behaviour; no saved file changes. (Host tests only so far: the check on the unit is pending.)

#### The fine portamento follows the sample's rate (OS-BUG-MODPLAY, F4)

- **A fine portamento (tracker FX 49 and 50: S3M `FFx` and `EFx`, and MOD `E1x` and `E2x` imported for the Modern engine) now moves the pitch by Scream Tracker's step for the sample it plays.** Before, the period of every note was sized at C-4 = 1712 whatever the sample, so `FF4` (16 units) always moved 16.2 cents. Scream Tracker and libopenmpt size the period from the sample's own C2SPD, so the same slide on a sample at 11,025 Hz is 21.5 cents; the Community OS now matches at that rate (21.46 cents against libopenmpt's 21.5 in the unit test; in the S3M journey of the simulator's reference test, libopenmpt tick by tick, the two players stay within 1.74 cents of each other on every tick of this case, where the same journey's plain note with no slide differs by 1.41; the test's tolerance is 1.5 cents a tick and 3 on a note's first tick). What changes is the size of the step for a sample **whose recorded rate is not one of these three, which keep today's steps exactly: 8,287 Hz and 8,363 Hz (every MOD: the Paula rates), and a sample with no recorded rate or at 44,100 Hz (the original firmware's samples and the unit's own recordings)**. On any other rate the table's period is scaled by 8363 / rate: a higher rate takes a larger step, a lower rate a smaller one (an S3M sample at 22,050 Hz: 43 cents for `FF4`), also for a WAV you imported at such a rate and gave a fine portamento. A project saved earlier keeps its FX values; how those notes sound changes only on such samples. No file changes. Evidence: verified parity measured at 11,025 Hz only (libopenmpt 0.7.3, `e2e_ref_openmpt_s3m`, `FF4` at C2SPD 11,025); the other rates use the same formula and are not measured against libopenmpt; new Community OS behaviour for a native sample with a recorded rate. Where the step stays the table's although Scream Tracker would scale it: see the known differences below.

#### Known differences of imported modules (OS-BUG-MODPLAY, F2, F3 and the limits of F4)

These are not fixed; they are written down. F2 and F3 were found by the libopenmpt comparison of the simulator (OS-E2E-9) and stay pinned by it.

- **MOD slides also run on tick 0 (F2).** A MOD imported for the Amiga engine runs `1xx`, `2xx`, `3xx` and `Axy` on tick 0 as OctaMED does, one tick of slide more on each row than ProTracker and libopenmpt: `104` is +16.3 cents on its first row and +34 cents on its second, `305` about +21 cents a row, and `A03` takes 3/64 of the level a row (0.047). An OctaMED song is not affected (it agrees with libopenmpt to 0.4 cent, tick 0 slides included). Why it stays: the switch exists (`AmigaVoices::set_slide_tick0`), but setting it for one song needs a saved flag, which is a change of the project format, and rescaling the slides at import cannot be exact (Fxx changes the speed during the song and `3xx` stops at its target). The import report names it ("slide timing").
- **A volume on a row with no note does not change the sounding note (F3).** MOD `Cxx` alone, the S3M volume column alone and MED `0C` alone change the note that is sounding in libopenmpt (to 0.5, 0.25 and 0.5 of its level in the three test songs); here they change nothing, so the note stays 6 dB (0.5) or 12 dB (0.25) louder than the original plays it. The step's volume is read only where a note starts, and acting on it would change how native projects play, because the Vol column is the Community OS's own. A per-import switch would need a saved flag as well.
- **The fine portamento keeps the table's step at three kinds of rate (limits of F4).** (1) An S3M sample whose C2SPD is exactly 44,100 Hz keeps the table (C-4 = 1712): 44,100 Hz is also the rate of the original firmware's samples and of the unit's own recordings, which carry no recorded rate, so it cannot be told from them without changing the sound of saved projects; Scream Tracker would take a step about five times larger for it. (2) An S3M sample at exactly 8,287 Hz keeps the table too (the PAL Paula rate, exempt so that every MOD keeps its slides); Scream Tracker's step for it is under 1 % smaller. (3) A sample above 96,000 Hz is scaled as if it were at 96,000 Hz (the rate is held, `audio/include/o3os/audio/tracker_fx.h`), so an S3M at C2SPD 192,000 Hz, the importer's own limit, takes about half of Scream Tracker's step. A sample at 8,363 Hz is the table's base, so it agrees. None of the three is measured against libopenmpt here.

#### The engine an instrument was saved with plays at once (OS-BUG-MODENGINE)

- **A project you open, or a module you import, now plays every instrument on the engine it names, right away.** Before, a load put every sample on the Modern engine and the instrument's PLAY ENGINE took effect only after something rebound the instrument (stepping the PLAY ENGINE row, a parameter edit, an undo), so the same project sounded different before and after an unrelated edit. An imported MOD or OctaMED song played on the sampler, with its arpeggio and slides unheard, until you stepped PLAY ENGINE to AMIGA on each instrument; it now plays on the Amiga engine straight after the import, and again after Save and reopen. Three cases sound different from before, and only these: **(a)** an instrument whose PLAY ENGINE you set to AMIGA, 12-BIT or 8-BIT now loads with that engine; **(b)** an AUTO instrument whose sample is at exactly 8,287 or 8,363 Hz, in play mode one shot or forward loop, now loads on the Amiga engine, as the AUTO rule says (so a MOD or OctaMED song you imported for the Modern engine with an earlier build and saved is stored as AUTO and now loads on Amiga instead of Modern, where the commands written for the Modern engine, a MOD's tracker FX 46..50, are not heard: set its PLAY ENGINE to MODERN, or import it again); **(c)** a MOD or an OctaMED song imported for the Modern engine (the import dialog's choice) now writes its instruments as MODERN, as an S3M always did, so it stays on the Modern engine for good: before, it played Modern only until something rebound the instrument, and then switched to Amiga at its Paula rate. The level can change with the engine (the Amiga engine reads the instrument level through the law, see OS-BUG-PASTE24 below). An instrument set to MODERN stays Modern, and so does an AUTO instrument at any other rate or in a play mode Paula does not play (backward loop, ping-pong, slices, wavetable, granular). No change of the file format: the engine byte was already saved in the project and is now honoured when the project loads; a MOD or OctaMED song imported for the Modern engine saves that byte as MODERN (the INST 1.2 chunk the writer already uses for an S3M). Evidence: new Community OS behaviour (a defect fixed), host tests `save_app_test`, `module_import_test`, and the E2E-9 journeys, which no longer step PLAY ENGINE.


#### Instrument volume, imports and the Amiga engine (OS-BUG-PASTE24)

- **An imported MOD, S3M or MED sample now has the instrument level that is 0 dB for the module's volume 64 (level 50), and the lower volumes follow the instrument law in dB** (MOD volume 32 is level 37, -6.2 dB; volumes 1 to 4 are level 1, -24 dB). Before, volume 64 became level 100, which plays 24 dB louder than the module. A MED MIDI instrument and the track volumes are not changed. **The Roll's start velocity is the instrument volume (volume x 127 / 100), so on an import it goes from 127 to 63**: a Roll that rises or falls (VolUp, VolDown) peaks about 6 dB lower than before, and the ratio of the Roll to a plain note on the same instrument goes from -24 dB to -6 dB; a MIDI-out Roll sends about half the velocity.
- **The tracker volume command (Cxx) changes meaning for every project, imported or not, on the Modern engine** (the tracker effects act on the sampler voices of the Modern engine; on the Amiga engine only the default-volume reading below changes). Its reference was "level 100 through the instrument law" (an absolute +24 dB at C40 under the 1.2+ law, whatever the instrument's own level). It is now **unity**: C40 is 0 dB, C20 is -6 dB. A note with Cxx on an instrument at level 80 played +24 dB and now plays 0 dB; on an instrument at level 30 it played +24 dB and now plays 0 dB too. Songs that use Cxx on notes without a velocity change by up to 24 dB. For the same reason, above level 50 the first tick of a volume slide, a tremolo or a retrigger drops the note to the unity scale (the instrument's own volume is held at 64, which is unity): a slide on an instrument at level 80 steps down by up to 14 dB at its first tick, and one at level 55 by 2.4 dB.
- **The Amiga engine reads the instrument level through the law**: a level of 50 or more is full volume (64), level 25 reads 16, level 30 reads 21. **A project you saved with an earlier build is not converted** (the level is all the file holds): its instruments keep their stored levels, so on the Amiga engine every level from 50 to 100 now reads full volume, where an import saved earlier at level 50 (volume 32) read half. Re-import the module to get the new levels, and check the Cxx commands of your own songs. Play with the master low.

#### The instrument volume law is saved in the project (OS-BUG-PASTE24)

- **A native copy of an OG project below version 7 now keeps its volume law.** Such a project plays its instrument levels through the 1.1.14 table (level 100 is 0 dB); the Community OS file did not record that, so after Save and load the same levels played through the 1.2+ law (level 100 is +24 dB). The project file now carries a new `LAW` chunk (12 bytes) when, and only when, the law is the 1.1.14 table. Every other project is saved as the same bytes as before, and a file saved by an earlier build (no `LAW` chunk) loads as the 1.2+ law, exactly as it always played. Nothing is converted. Both laws are this OS's model, named after the file versions they follow; what an official unit plays is an open research question.
- **A sample instrument you add to a project that plays the 1.1.14 table (a recording, a loop capture, an IFF sample, a wavetable) is born at level 100, which is 0 dB under that table**, instead of level 50, which is about 20 dB below it. In every other project it is still born at level 50 (0 dB).
- **Downgrade hearing risk: do not open a project saved with the `LAW` chunk in a build older than the one that wrote it, with the master up.** An older build ignores the chunk (it keeps its bytes and writes them back) and plays the levels of an OG project below version 7 about 24 dB louder at VOLUME 100 than they should be (+24 dB instead of 0 dB). This is the maintainer's decision of 2026-10-16: no major version bump of the format, so older builds are not stopped from opening these files. See `docs/formats/o3-project.md`, "The `LAW` chunk".
