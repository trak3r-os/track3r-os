<!-- manual-sync: dc51561d -->
# Advanced cheat sheet

Print it: 2-3 pages, A4 or Letter.

The advanced and slightly complex keys of track3r-os up to version 1.3.0, one area after another. Items marked **(new in 1.3.0)** are new since version 1.2.0. The basic keys are on the [Cheat sheet](cheat_sheet.md). Each area ends with a **More** line that links to the chapter that explains it in full. Keys are written as on the unit: `Shift` + `F1` is two keys pressed together, and hold `Instr` means press and keep it down.

## Pattern and editing

<!-- sync: src=docs/manual/pattern.md,docs/manual/pattern_tools.md -->

| Keys | What it does |
|---|---|
| `F7` (**TOOLS**) | Opens the verb menu. Pads 1 to 16 pick a verb: INVERT is pad 9, DUPLICATE x2 pad 10, EXPAND x2 pad 11, SHRINK /2 pad 12. Pad 16 (**APPS >**) lists the tool apps. |
| `F4` (**FILL**) | Opens the Fill dialog. TYPE is EVERY N, EUCLID, RANDOM or SCALE. MODE is CONST, RAMP or RANDOM. `F1` fills, `F8` leaves with nothing written. |
| `F2` (**VARY**) | In a random fill: takes the fill back and draws again. |
| `F7`, then `F6` (**RE FILL**) | Repeats the last fill or the last TOOLS apply. **(new in 1.3.0: `F6` on the key row now opens EVENTS, so the repeat sits behind `F7`.)** |
| `F6` (**EVENTS**) | The event list: notes, CC, pitch bends and lane rows, in time order. **(new in 1.3.0: opens straight from `F6`.)** |
| EVENTS: `F2`, `Insert`, `Delete`, `F7` | `F2` (**QUANT**) snaps a row to its step. `Insert` copies a row. `Delete` clears it. `F7` (**UNDO**) takes back a list edit; `Shift` + `F7` redoes it. |
| `Rec`, `Shift` + `Rec`, `Rec` then `Play` | STEP on, LIVE on, or LIVE on at once. Only a key ends a mode. `Shift` + `Rec` from stopped counts in first when COUNT-IN is on. |
| `Shift` + arrows, `Copy`, `Shift` + `Copy` | Select a block. Copy it. Paste at the cursor in STEP. Hold `F1` and press `Copy` to copy the whole pattern. |
| `F8` (**UNDO**), `Shift` + `F8` (**REDO**) | Take back or redo a Pattern change. A whole LIVE take is one step. Song edits undo on Song. |

**More:** [Fill and tools](pattern_tools.md), [Pattern](pattern.md), [Edit the events of a take lane](pattern_tools.md#edit-the-events-of-a-take-lane).

## MIDI

<!-- sync: src=docs/manual/panel.md,docs/manual/config.md,docs/manual/pattern.md -->

| Keys or setting | What it does |
|---|---|
| Config, PATTERN, **REC STORE** `Lane` (default `Grid`) **(new in 1.3.0)** | With `Lane`, LIVE records a MIDI instrument's keys and controller moves into a lane beside the steps. Nothing reaches the step grid, and the lane plays with the steps. `Grid` keeps Spill and Micro-move as before. A lane records only while the track plays forward: if a Perform PLAYMODE (Backward, PingPong, Random or C1 to C20) is on for the track, the strip says `LANE REC: FORWARD ONLY`, no take is made and the keys go to the step grid; a take that is under way when the order changes is dropped. Export to `.mtp` drops lane events and the report counts them. |
| Lane notes, LIVE (`Shift` + `Rec`) **(new in 1.3.0)** | Each note keeps the tick it was played at and its held length. A chord is one NOTE row per key, at its own tick. |
| Lane notes, STEP (`Rec`) **(new in 1.3.0)** | With one cell selected, a chord held together goes in at the cursor step's first tick, each note one step long. |
| Controllers on the track's channel **(new in 1.3.0)** | A mod wheel or knob box sends its controller number into the lane. A controller on another channel is not recorded. |
| `Instr` and `Vol` held, then the encoder, on a CC A to CC F row **(new in 1.3.0)** | The encoder or a Learn knob on a CC letter records that controller's value. The strip reads `CC <L> <value>`. The first move of a letter in a take starts at 64. |
| The count-in **(new in 1.3.0)** | Nothing is recorded during the count-in. The strip says `LANE REC: NOT RECORDED: COUNT-IN`. |
| `F8` (**UNDO**), then `Shift` + `F8` (**REDO**) **(new in 1.3.0)** | A whole take is one undo step. Redo brings it back. |
| The grid mark `MID nn cc` **(new in 1.3.0)** | A step where a lane note starts shows `MID`, then the number of notes and of controller moves. A bar at the left edge: top half for notes, bottom half for controllers. The mark is for reading. |
| `Vol` + `Fx` (**Learn**) | Opens Learn for the row under the cursor. `F1` unbinds. `F2` sets MODE: pickup, jump or relative. `F5` (**FOLLOW**) makes the binding follow the selected instrument. |
| `F6` (**MAP**) in Learn, or `F8` (**MAP**) on Config, MIDI **(new in 1.3.0)** | **LEARN MAP**, a list of every binding. `F1` NEW, `F2` MODE, `F3` DELETE, `F4` CLR ALL (press twice), `F5` FOLLOW, `F7` RELEARN. |
| Step CC A to CC F on a sample or module instrument **(new in 1.3.0, PARAMAUTO)** | The step moves the parameter that owns the letter. The value is scaled from 0 to 127 over its range. A MIDI instrument's CC steps work as before. |
| LIVE or STEP, a move of a row with no FX of its own **(new in 1.3.0)** | The strip reads `REC FX: VOLUME -> CC A`. The row keeps its letter. Six letters only: a seventh says `REC FX: NO FREE CC LETTER`. On a MIDI track it says `REC FX: MIDI TRACK` and writes nothing. |
| Config, MIDI, **CC MAP** `On` | The official fixed CC numbers drive the volume of tracks 1 to 8 (71 to 78), the master levels (79 to 82), Perform slots (51 to 62) and performance patterns (41 to 48). Off by default. |
| Config, MIDI, **CLOCK MODE**, **TRANSP IN** and **TRANSP OUT** | Master or Slave for the clock. TRANSP IN and OUT turn off the Start, Stop and Song Position messages, not the clock. |
| Config, MIDI, **MTC OUT** and **MTC IN** | MTC OUT sends time code (24, 25 or 30 fps) for a program such as Logic Pro, from **MTC START** `01`. MTC IN follows a time code master: the row reads LOCKED when it runs with it. |

**More:** [Panel: Learn](panel.md#learn-a-midi-control), [LEARN MAP](panel.md#see-every-binding-learn-map), [Put one parameter on the encoder](panel.md#put-one-parameter-on-the-encoder), [Pattern](pattern.md#good-to-know), [Make Logic Pro follow the unit](config.md#make-logic-pro-follow-the-unit), [Let the unit follow a time code](config.md#let-the-unit-follow-a-time-code), [Control the unit with the official CC numbers](config.md#control-the-unit-with-the-official-cc-numbers).

## Perform and DJ

<!-- sync: src=docs/manual/perform.md,docs/manual/perform_dj.md -->

| Keys | What it does |
|---|---|
| Hold a pad, turn the encoder | A live effect over the tracks that are ON. It ends at the release. The pattern is not changed. `F6` (**LATCH**) keeps a value on after the release, until you press another row or the top pad. |
| `Shift` + `F1`, `F2`, `F3` (**STORE 1** to **3**) | Store the twelve slots as a scene. `F1` to `F3` recall it. |
| `F4` (**HALF**), `F5` (**DOUBLE**) | Half or double speed while held. `F8` (**RESET**) ends every effect. |
| `Shift` + `F4` / `F5` (**PICK -**, **PICK +**), then `Shift` + `F6` (**TRK PAT**) | Queues a picked pattern on the tracks that are ON, at each track's loop end. |
| `Shift` + `F7` (**TRK NEXT**), `Shift` + `F8` (**TRK SONG**) | Queues the next song pattern on the ON tracks, or sends them back to the song. |
| DJ page, `F3` (**CURVE**) | SMOOTH fades with equal power. CUT keeps a group at full level to the end. |
| DJ page, `Shift` + top-row pad | Sets the track's group: THRU, A or B. The encoder moves the crossfader. |
| DJ page, hold `F4` (**FILTER**), turn the encoder | Closes a low-pass or high-pass filter over the mix. It springs back at the release. |
| DJ page, `F5` (**SCR**) | The encoder becomes the platter for scratching. `F5` again ends it. |
| DJ page, pads 25 to 27, pads 29 to 32 | KILL LOW, MID and HI cut a band. TRANS HP, TRANS LP and TRANS ECHO end on the next pattern. Pad 32 is TAPE STOP. |

**More:** [Perform](perform.md#hold-or-latch), [Scenes](perform.md#scenes), [DJ page](perform_dj.md#groups-who-gets-faded), [Scratch and tape stop](perform_dj.md#scratch-and-tape-stop).

## Song

<!-- sync: src=docs/manual/song.md -->

| Keys | What it does |
|---|---|
| `F3` (**ADD SLOT**) | Adds a slot with the same pattern. Both slots play one pattern, so an edit in Pattern is heard in both. |
| `F5` (**UNIQUE**) | Gives a shared slot its own copy of the pattern. |
| Hold `Copy` + `F3` (**ADD EMPTY**) | Adds a slot on the first free, empty pattern. |
| `Rec` while the song plays | Queues a jump to the cursor's slot. Press `Rec` again to cancel. |
| Hold `F2` (**DUP PAT**) with slots selected | Loops the selected slots. The info line reads LOOP. |
| `F6` (**TOOLS**) | Block dialog: TRANSPOSE, INSTRUMENT, CLEAR, EXPORT WAV, FORM and BRIDGE. FORM builds a song shape from one to three patterns. BRIDGE puts new slots between two slots. |
| `Shift` + `F7` (**PADS**), `F7` (**TEMPO**) | PADS switches the 48 pads between SLOTS and TRACKS. `F7` puts the encoder on the tempo. Two taps under 1.5 s apart set it. |

**More:** [Sharing a pattern](song.md#sharing-a-pattern), [The track map](song.md#the-track-map), [Timeline page](song.md#read-the-song-on-the-timeline-page).

## Instruments and modules

<!-- sync: src=docs/manual/instrument.md,docs/manual/modules.md,docs/manual/module-sounds.md,docs/manual/module_app.md -->

| Keys or setting | What it does |
|---|---|
| The synths are card modules **(new in 1.3.0)** | TB-303, Chip, FM and SID are `.tmod` files. Copy `modules/instruments/` and `LICENSES/` from the starter zip to the card. A project whose instrument has no module on the card shows `MODULE NOT ON THE CARD`. Replace older copies. |
| `F4` (**MODULE**) in an empty instrument slot | Lists the instrument modules on the card: ON CARD, or LOADED once running. |
| TB-303, `F2` (**MODS**) | TUNE (cents), TRANSPOSE (semitones), ACC THRESH, FILTER TRACK (0 to 100 %) and ACC DECAY (30 to 2000 ms). |
| FM, operator pages | ALGORITHM runs 1 to 16. DETUNE reaches plus or minus 1200 cents. |
| Chip, WAVE and DUTY | Waves 4 to 11 come from the CHIP synth. WAVE and DUTY can be learned. The WAVE view draws the picked wave. **(new in 1.3.0: the view.)** |
| SID, four pages `F1` to `F4` | OSC, ENV, FILTER and PITCH. SID plays one note at a time, so play chords on several tracks. |
| `Shift` + `F2` (**MIX**) on a module | PANNING, DELAY SEND, REVERB SEND, and RATE: FULL, 1/2 or 1/4. A lower rate is cheaper and crunchier. |
| `Shift` + `F5` (**SAVE**) on a module page | Saves the parameter set as a preset in `modules/presets/<module>/`. `F5` lists presets; `F1` loads one. |
| `Shift` + pad, on Instrument **(new in 1.3.0)** | Plays the instrument in view. `F6` and `F7` change the instrument. |
| `Shift` + `Pattern`, `F1` (**LAUNCH**) | Lists the apps that are not tools and starts one. `F7` + `F8` leaves any app. |
| `F7` (**TOOLS**), pad 16 on Pattern | The APPS page of the tool apps, such as Rhythm Gen and Humanize. |

**More:** [Modules and apps](modules.md#three-kinds-of-module), [TB-303, FM and Chip](module-sounds.md#the-tb-303s-mods-page), [SID](module-sounds.md#sid-a-chip-voice-with-four-pages), [Presets](modules.md#presets-load-and-save), [App pages](module_app.md#go-back-to-where-you-opened-the-app).

## Mixer and inserts

<!-- sync: src=docs/manual/mixer.md,docs/manual/insert_effects.md -->

| Keys | What it does |
|---|---|
| Hold `Master`, tap `Insert` **(new in 1.3.0)** | Opens the Mixer on INSERTS from any screen. |
| `Insert` on an empty INSERTS cell | Opens the picker. Two insert slots a track, one on the master: DRIVE, CRUSH, COMP, ENV FILTER, FOLD, PHASER, BIQUAD, PING-PONG, WIDE, CHORUS, AUTOPAN. |
| `F7` on a taken cell | Bypass on or off, with no click. OVER BUDGET means the CPU guard forced the insert dry. |
| `Delete` on a taken cell | Removes the insert. The Mixer has no undo, so this is final. |
| `Params`, or `F6` (**PARAMS**) on INSERTS | The insert's settings. Learn works on its rows with `Vol` + `Fx`. |
| `F2` (**ROUTING**), encoder | Turn up to route a track to a bus (MAIN, CUE, SND A, SND B), down to remove. |
| `F4` (**STEMS**), `F6` (**APPLY**) | On STEMS, `F6` sets the number of USB outputs after a second press within two seconds. |

**More:** [Insert effects](insert_effects.md#presets-and-the-cpu-guard), [Put an effect on a track](mixer.md#put-an-effect-on-a-track), [Routing](mixer.md#routing-where-a-track-goes), [Sends](mixer.md#sends-a-second-volume-knob).

## Sample tools and recording

<!-- sync: src=docs/manual/sample_edit.md,docs/manual/sample_load.md,docs/manual/wave.md,docs/manual/record.md -->

| Keys | What it does |
|---|---|
| `Sample Editor` again | Opens the FX page (`F1` FILTER, `F2` COMPRESS), then the PLUGINS page. |
| PLUGINS, `F1` (**APPLY**) | Runs a `.ptfx` plugin from `Effects/` on the selection or the sample, with undo. A plugin that faults is contained. |
| Sample edit, `F1` to `F6` | CROP, NORM, REVERSE, FADE IN, FADE OUT and AMP. Each acts on the selection, or on the whole sample when none is marked. |
| `Shift` + `Right` or `Left` (Sample load) | Marks a file and steps down (`Shift` + `F6`, **MARK**, does the same), or clears the marks. `F1` (**LOAD**) and `F4` (**ADD**) load every marked file. `Shift` + `F7` / `F8` moves the target instrument. |
| Wave and Chop, `Shift` + `F1` (**MODE**) | Opens the list of play modes. `Shift` + `F4` (**TO PAT**, CHOP page) writes the slices into the Pattern from its cursor, `Shift` + `F6` (**PRV SEL**) previews from the selected point, `Shift` + `F7` / `F8` (**INST -** / **INST +**) pick the instrument, as on Sample load and Sample edit. |
| `Sample Recorder` again | Turns the pages SOURCES, RESAMPLE and RADIO. |
| `F1` (**OVERDUB**) under a loop review | Records a layer. `F5` (**UNDO LYR**) takes the last layer back. |
| `F8` (**LOOP ON** or **MUTED**) | Mutes or unmutes the loop. |
| `F3` (**SAVE**) on a loop, `F4` (**DISCARD**) | Saves the loop as an instrument. DISCARD asks first: press `F4` again. |
| `Shift` + `F3` (**CROP**) on a take | Cuts the take down to its window. It asks first. |

**More:** [Record](record.md#a-loop-live-stored-and-ended), [Count a take in](record.md#count-a-take-in), [Start a take on a level](record.md#start-a-take-on-a-level), [Sample edit](sample_edit.md), [Wave and chop](wave.md), [Sample load](sample_load.md).

## Config

<!-- sync: src=docs/manual/config.md -->

| Keys or setting | What it does |
|---|---|
| PATTERN page, **REC STORE** **(new in 1.3.0)** | `Grid` (default) or `Lane`. See MIDI above. |
| PATTERN page, **PAD OFFSET** | 0 to 12. Sets where each pad row starts on the row below it. 12 is the default. |
| PATTERN page, **PAT SWITCH** | Step (default) or Pattern end: when a pattern picked with `F1` held starts. |
| PATTERN page, **COUNT-IN**, metronome | Count-in of 1 or 2 bars. The metronome can click on the Main outputs. |
| DISPLAY page, **STATUS LED** | BLINK is the heartbeat, OFF turns it off. **BPM** flashes on every beat **(new in 1.3.0)**. |
| USB page, `F7` (**APPLY**) twice | Changes USB AUDIO or USB RETURN. `Shift` + `F7` puts a page back to its defaults. |
| `F5` (**USB**), **CARD AS DISK** | Hands the card to the computer while the unit keeps playing. `F7` twice to start. |
| INFO page, `F8` (**UPDATE**) | Installs a `.ptf` from the `Firmware` folder after two confirmations. |

**More:** [Config](config.md#set-the-count-in), [Keep the timing and velocity of live notes](config.md#keep-the-timing-and-velocity-of-live-notes), [Update from the unit](config.md#update-from-the-unit).

## Files, USB disk and export

<!-- sync: src=docs/manual/file.md,docs/manual/usb_disk.md,docs/manual/export.md -->

| Keys | What it does |
|---|---|
| `F3` (**NEW**), then `F3` again (**ALL**) or `F1` (**KEEP INSTRUMENTS**) | Starts a new project. |
| `F6` (**IMPORT**) on File | Imports a MOD, S3M or MED file. It needs `modules/importers/tracker.tmod` from the content zip. |
| `F7` (**EXPORT**) on PROJECTS | Export page: WAV of the song, a pattern, a block, or stems. `F8` (**CANCEL**) stops it; finished files stay. Pattern TOOLS pads 14 and 15 export the block and the pattern. |
| Power on, hold `Shift`, `F1` and `F7` | The unit shows its card as a disk and does nothing else. |

**More:** [File](file.md), [USB disk](usb_disk.md), [Export](export.md).

## Computer tools

<!-- sync: src=docs/manual/computer_tools.md -->

| Command | What it does |
|---|---|
| `cpu`, `guard`, `ins` | The audio load, underruns and skipped blocks; each bound insert with its cost. |
| `mem`, `crash`, `log 30` | Memory map and free SRAM; the last crash record; the last 30 log lines. |
| `sd`, `ls Projects`, `fw` | The card's state; a read-only folder listing; firmware files on the card. Use the tool to put or arm firmware. |
| `key <button> tap` | Presses a button as a test. `state` prints the screen, cursor and play state. |

**More:** [The diag console](computer_tools.md#the-diag-console-o3consolepy), [Take a screenshot](computer_tools.md#take-a-screenshot), [Measure the audio](computer_tools.md#measure-the-audio-o3audiopy), [Input test](input_test.md).
