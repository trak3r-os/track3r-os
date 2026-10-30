<!-- manual-sync: cbfcba6 -->
<!--
Glossary of the user manual. One term per entry, as "term: definition", one entry per paragraph.
Terms come from the chapters and the QA catalogue. Add a term when a chapter first needs it.
-->
# Glossary
<!-- sync: src=audio/include/o3os/audio/graph.h,app/include/o3os/app/pad_lights.h,audio/include/o3os/audio/master_limiter.h,app/include/o3os/app/limiter_rows.h,app/src/screens/config_screen.cpp,app/src/bindings.cpp,docs/manual/sample_edit.md -->

Key row: the row of eight labels under the soft keys `F1` to `F8`.

Shift row: the row of labels that the unit draws while `Shift` is held. A key with no label does nothing.

Strip: the message line of the screen. Short messages appear there, for example **TRACK 03 NAMED**.

Track: one of the project's tracks. Each track has a name and a column on the Mixer.

Mute: a track setting that silences the track. Set it with `F7` (**MUTE**) on the Mixer.

Solo: a track setting, set and cleared with `F8` (**SOLO**) on the Mixer. While a track is soloed, every track that is not soloed is silent, and so are its sends. Any number of tracks can be soloed.

Master limiter: the one stage on the MASTER FX page. It is always on. It holds the loudest peaks of the master output under a ceiling, so they do not clip. Its rows are LIM THRESH, LIM ATTACK and LIM RELEASE.

On-screen keyboard: the keyboard that opens for a name, with a title such as **TRACK NAME**. It takes up to the length the name allows (20 characters for a track, 30 for a pattern).

Order list: the list of slots on the Song screen.

Slot: one entry of the order list. It holds one pattern.

Pattern: a numbered set of notes, edited on the Pattern screen. A slot on the Song screen holds one pattern.

Fill: writing notes on the steps of a track by a rule. Press `F4` (**FILL**) on the Pattern screen. The rules are EVERY N, RANDOM, EUCLID and SCALE.

Pattern name: the name you give a pattern, up to 30 characters. Set it on the Song screen with `Shift` + `F6` (**NAME**).

Pending value: a Config setting you changed but have not applied. It shows a star (\*) until `F7` (**APPLY**) writes it.

APPLY: the `F7` key on Config. On a USB row, the first press says what will happen and the second press writes the setting.

USB AUDIO: the Config row that shows an audio device to the computer. Off gives STANDARD; On is the default.

STANDARD: the USB mode of the unit with MIDI and serial only, and no audio device.

USB return: the stereo stream that the computer sends to the unit over USB audio. The USB RETURN row of Config turns it on or off.

RETURN LEVEL: the Config row for the level of the USB return at the outputs. It changes at once, with no APPLY.

METRO: the metronome. It plays a click with the beat. Set it on the PATTERN page of Config: Off, Cue or Main.

Cue: the METRO setting that plays the click on the CUE side of the line out. Main plays it on the MAIN side.

Count-in: the bars of click that play before a recording starts. Set it with COUNT-IN on the PATTERN page of Config: Off, 1 bar or 2 bars.

Live recording: recording the notes you play while the pattern plays. Turn it on with `Shift` + `Rec` on the Pattern screen.

REC QUANTIZE: the Config row that sets how a live note is placed on a step. Its values include Nearest step and Playing step.

Step FX: an effect written on one step, in one of its two Fx cells. The info line reads it in words.

FX 2: the second FX column of a step. A Micro-move is written there.

Micro-move: a command in FX 2 that records how late a live note was played inside its step. The grid shows it as m02.

Sample: a recording of sound, read from a file on the card and loaded into an instrument.

Instrument: a slot of the project that makes sound from a sample, a module or MIDI.

Card: the SD card. Sample load browses its folders and files.

Sample rate: the number of audio samples per second, in hertz (Hz). Each sample file records its own rate.

AIFF: an audio file format. Its files end in `.aif`, `.aiff` or `.aifc`.

8SVX: an 8-bit audio file format of the IFF family. Its files end in `.8svx` or `.svx`.

WAV: an audio file format. Its files end in `.wav`.

Export: writing the sound to WAV files in the `Export` folder on the card. Press `F7` (**EXPORT**) on the PROJECTS page of the File screen.

Stem: the sound of one track, or of the delay, the reverb or the whole mix, written to its own WAV file by a stem export.

OG PROJECT: the EXPORT row that writes a project folder in the original firmware's format. It is experimental.

Stereo file: a file with a left and a right channel. Sample load shows **ST** after its name.

Channel: one side of a stereo file. On Sample load, `F8` (**CH MIX**, **CH LEFT**, **CH RIGHT**, **STEREO**) chooses what loads.

Audition: playing a file without loading it. On Sample load, `F5` (**PLAY**) auditions the file under the cursor.

Selection: the range that the Sample edit processes act on. With no selection, a process acts on the whole sample. Turn the encoder on Sample edit to make one, and move its edges with the encoder. `Delete` clears it.

Normalise: scaling a sample so that its loudest point is at full scale. On Sample edit, `F2` (**NORM**).

DC offset: a sample whose average is not at its centre line. Removing it centres the sample without clipping. On Sample edit, `Shift` + `F4` (**DC REM**).

Amiga conversion: converting a sample to the Amiga rate of 8,287 Hz (**AMG 8K**) or 16,574 Hz (**AMG 16K**), with an 8-bit option (**Q8**). On Sample edit, `Shift` + `F1`, `F2` and `F3`.

Amiga engine: one of the two sample voices. The other is the Modern engine.

Undo: taking back the last Sample edit process. `F8` (**UNDO**) undoes it, and `Shift` + `F8` (**REDO**) does it again.

Play mode: how an instrument plays its sample. The modes are ONE SHOT, LOOP FWD, LOOP BWD, PING-PONG, SLICE, BEAT SLICE, WAVETABLE and GRANULAR. Press `Shift` + `F1` (**MODE**) on the Wave screen.

Slice: a part of a sample that one pad plays. The slice modes, SLICE and BEAT SLICE, use the CHOP page.

CHOP page: the page of the Wave screen for the slice modes. It holds the slice keys.

Wavetable: a WAV file of many short windows, one after another. The WAVETABLE play mode plays one window at a time. The POSITION row chooses the window.

Window: the part of a wavetable that plays. It is at most 2048 samples, and never longer than the sample.

Granular: the GRANULAR play mode. It plays short pieces of a sample, called grains, that you set with POSITION, LENGTH, SHAPE and LOOP.

Project: the work saved as one unit: patterns, song order, instruments and their samples. Each project has its own folder on the card.

Title: the name of a project, shown in the header. It is not the folder name, and it can have up to 32 characters.

RECENT: the first rows of the PROJECTS list. They are the last five projects you opened or saved, newest first.

Purge: removing the instruments that no step, FX or Learn binding uses. Press `Shift` + `F7` (**PURGE**) on the PROJECTS page, twice.

USB disk mode: the mode in which the unit shows its card to a computer as a disk. Hold `Shift`, `F1` and `F7` together while you turn the unit on, or use the CARD AS DISK row of Config. See [USB disk](usb_disk.md).

Scene: one of the three stored scenes, SCENE 1 to SCENE 3, on the Perform screen. Keys `F1` to `F3` recall them.

Perform slot: one of the 12 columns of pads on Perform. It holds one effect and has four rows: **OFF** on top and three values.

LATCH (slots page): the switch on `F6` of the **SLOTS** page of Perform. It is on by default and saved with the project. On, a slot's row that holds a value stays on after you let go of its pad. Off, the effect ends at the release.

LATCH (DJ page): a second switch, on `F6` of the DJ page. It is off when you open the page, and leaving the page ends it. On, the filter stays where you left it, and a kill, **REP**, **ECHO** or **CUT** pad stays on after a tap until you tap it again.

EDIT: on Perform, `Rec` turns EDIT on. While it is on, the encoder stores the value in the row you hold, and `Left`, `Right`, `Up` and `Down` pick a slot and change its effect.

Crossfader: the bar on the DJ page that fades between group A and group B. The encoder moves it.

Group (DJ page): the set of tracks that the crossfader moves together: **A**, **B**, or **THRU** for tracks it never touches.

Module: a file with the extension `.tmod` on the card. It is an instrument module (a sound engine such as the TB-303, FM or Chip) or an app.

Module instrument: an instrument slot whose sound comes from a module, not from a sample or MIDI.

Engine picker: the list that opens with `F4` (**MODULE**) on a free instrument slot. It lists the instrument modules on the card.

Accent: on the TB-303, a note played harder than the rest. It opens the filter further, is louder, and lets the filter fall back on its own time. A note is an accent when its velocity reaches **ACC THRESH**.

Cent: a hundredth of a semitone. 1200 cents are one octave.

Operator: one of the four sine waves of the FM module. Each has its own ratio, detune, level and envelope.

Modulator: an FM operator that bends the pitch of another operator. You do not hear it by itself. A carrier is an operator you hear.

Algorithm: on the FM module, the choice of which operators modulate which and which are carriers. `4>3>2>1` means 4 modulates 3, 3 modulates 2, 2 modulates 1, and only 1 is heard.

Detune: a small shift of an FM operator's pitch from its ratio, in cents.

App: a module you run from the Modules screen, for example MIDI Monitor or Tuner. Apps are experimental.

Navigator: the list of screens that opens with `Shift` + `Pattern`. Turn the encoder to a screen and press `F8` (**GO**).

TOOLS: the `F7` key on Pattern. It opens a list of tools, among them EXPORT PATTERN and the apps that edit the project or write files.

Firmware file: the file with the `.ptf` extension that holds a firmware image. The unit's bootloader flashes it from the `Firmware/` folder of the card.

Workspace: the card folder that holds the project the unit loads at boot. The project is in the original firmware format.

Demo kit: four instruments and pattern 1, built in. The unit uses them when no project loads, or when the loaded project has no sample or module instrument and no instrument at all in slots 1 to 48 (the MIDI channels in slots 49 to 64 do not count). A project with a module instrument anywhere, or a MIDI instrument in slots 1 to 48, keeps its instruments, tempo and pattern 1. This is new Community OS behaviour, a default the maintainer still has to confirm.

Starter zip: an optional zip that goes next to the firmware file, with folders, samples, wavetables and demo projects. It is not part of this release.

Diag console: the text console of the unit, on its USB serial port. You type a command and it prints the reply. The computer tools send their commands to it.

Python 3: the programming language that the computer tools are written in. Install it on your computer before you use the tools.

Take: one recording made on the Record screen, from its start to its end.

Armed: the state of a take that waits for its start point, such as the next bar. The header says **ARMED**.

Review: the screen after a take ends. For a loop, you save, discard, mute or overdub it from here.

Loop: a take of a set number of bars that repeats on the bars of the pattern. Set its length on the LOOP row.

Layer: one overdub pass, added on top of a loop.

Overdub: recording on a loop that is already playing, so that each pass adds a layer. Press `F1` under a loop review.

Feedback: the Record setting, from 0 to 100, for how much of the old sound is kept on each pass of an overdub. At 100 every layer stays.

Undo layer: `F5` (**UNDO LYR**) on the Record screen. It puts back the sound from before the last layer.

Transport: the playing of the pattern. `Play` starts and stops it, and a loop follows it.

Sample memory: the memory that holds samples. A take that fills it ends.

Column: one of the four fields of a track on the Pattern grid: **Note**, **Instr**, **Vol** and **Fx**. Tap its key to select it.

Step: one row of the Pattern grid. A pattern has up to 128 steps.

Cursor: the highlighted cell that you edit. The arrow keys move it.

Block: the steps and tracks you mark with `Shift` and an arrow. Copy works on it.

Clip: the steps last copied with `Copy`. The clip stays until the next copy.

JUMP: how many steps the cursor moves after an entry or a delete. Set it on the JUMP chooser, `F3`, from 0 to 32.

LENGTH: the number of steps in the pattern, from 1 to 128. Set it on the LENGTH chooser, `F2`.

Playhead: the step that the pattern is playing now. In LIVE it has a red bar.

WATCH: the record mode in which the pads only preview and nothing is written. The chip reads **WATCH**. Browse is the same mode.

STEP: the record mode in which notes are written at the cursor. The chip reads **STEP**, in amber.

LIVE: the record mode in which notes are written at the playhead while the pattern plays. The chip reads **LIVE**, in red.

Zoom: how many tracks the Pattern grid shows: 5, 8 or 12. Hold `Note` and tap `Instr` to change it.

Undo area: the changes that one screen can take back. `F8` takes back the last change of the screen you are on. Pattern and Song have separate undo areas.

Page: one of the views of the Instrument screen. `F1` to `F5` open them on a sample instrument.

Source page: the first page of a sample instrument. It holds PLAY MODE, PLAY ENGINE and the rows of the play mode.

Play engine: the PLAY ENGINE row, which picks the engine that plays a sample: Auto, or a fixed engine. The 12-bit and 8-bit presets cap the bit depth.

Volume law: the rule that turns an instrument's VOLUME into decibels (dB). The law belongs to the project, so an older project keeps its loudness.

MIDI instrument: an instrument that sends MIDI notes and controller messages to a device, instead of playing a sample.

LFO: a low-frequency oscillator, a slow wave that moves a destination up and down, at the tempo's rate.

Destination: a setting that an LFO can move. The MOD page lists six: Volume, Panning, Finetune, Cutoff, WT Position and Gran Position.

Velocity: how hard a note is played, as a MIDI value up to 127. The VEL> row on the VOICE page scales it.

Type chooser: the screen of an empty slot. It offers SAMPLE, MIDI, PASTE, MODULE and LOAD.

Instrument copy: the instrument copied with `Shift` + `F2`. The unit keeps one copy at a time, and `Shift` + `F3` pastes it.

Instrument file: a saved instrument, with the extension `.pti`, in the `instruments` folder of the card.

Euclid rhythm: a rhythm of E hits spread as evenly as possible over the steps of a track, with gaps that differ by at most one. The FILL dialog writes it with TYPE **EUCLID**.

Ramp: values that move in a straight line from one end to the other, such as a volume from 127 down to 0 over a track. The FILL dialog writes it with MODE **RAMP**.

Voice: one sound that is playing. Each note that sounds uses one voice, and the unit has a fixed number of voices.
