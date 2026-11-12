# Cheat sheet
<!-- manual-sync: 867c173c -->

The keys on one page. Each table names the chapter it comes from.

## Global keys
<!-- sync: src=docs/manual/panel.md,docs/manual/pattern.md qa=QA-GLB-130,QA-GLB-157 -->

| Key | What it does |
|---|---|
| `Play` | starts or stops the pattern, from any screen |
| `Rec` | on Pattern, turns STEP on or off; on Record, starts or stops the take |
| `Shift` + `Rec` | on Pattern, turns LIVE on or off |
| `Song` + `Pattern`, or `Master` + `Song` | opens the Navigator. `F1` (**GO**) opens the chosen screen; `Pattern` leaves it |
| `Master` + `Insert` | opens the Mixer on its INSERTS page, from any screen (hold `Master`, then tap `Insert` at any time) |
| `Shift` + `Pattern` | lists the module apps that are not tools. `F1` (**LAUNCH**) starts one; `Pattern` leaves it |
| `Shift` + `Config` | opens the help page of the screen you are on. `Left` and `Right` turn the pages, `Up`, `Down` and the encoder scroll, any other key closes it |
| `Shift` + `File` | saves the project from any screen, as `F4` (**SAVE**) does on File. A project with no folder yet opens the save-as keyboard. <!-- QA-GLB-243 --> |
| `Shift` + `Master` | steps to the next colour theme: Amiga, Black, White. <!-- QA-GLB-157 --> |
| `Shift` + `Power` | saves a screenshot to the card. `Power` held 2 seconds turns the unit off. <!-- QA-GLB-130 --> |
| `Master` held, then the encoder | changes the master volume, from any screen |
| `Pattern` held, then the encoder or a pad | the encoder changes the tempo, from any screen (a BPM a click, a tenth with `Shift` held first); pad taps tap the tempo. Letting go of `Pattern` after that opens nothing. <!-- QA-GLB-153, QA-GLB-154 --> |
| `Song` held | shows Song until you let go, then goes back to the screen you came from |
| `Vol` and `Fx` together | opens Learn |
| `F7` in Learn (`Vol` and `Fx`) | makes the row under the cursor the encoder's target, like a Learn binding with no MIDI controller; `Instr` and `Vol` held, then the encoder, moves it (see [Put one parameter on the encoder](panel.md#put-one-parameter-on-the-encoder)) |
| a screen button again | opens the next page of that screen: the FX and PLUGINS pages of Sample edit, the DJ page of Perform |
| a soft key again | steps that key's windows, such as the six MOD destinations on Instrument |

<!-- manual-tables: cheat buttons -->
| Screen button | Opens | Chapter |
|---|---|---|
| `Params` | Instrument | [Instrument](instrument.md) |
| `Perform` | Perform | [Perform](perform.md) |
| `File` | File | [File](file.md) |
| `Config` | Config | [Config](config.md) |
| `Master` | Mixer | [Mixer](mixer.md) |
| `Sample Loader` | Sample load | [Sample load](sample_load.md) |
| `Sample Playback` | Wave and Chop | [Wave and Chop](wave.md) |
| `Sample Editor` | Sample edit | [Sample edit](sample_edit.md) |
| `Sample Recorder` | Record | [Record](record.md) |
| `Song` | Song | [Song](song.md) |
| `Pattern` | Pattern | [Pattern](pattern.md) |

<!-- /manual-tables -->

Held at power-on, until the strip shows the mode:

| Keys | What happens |
|---|---|
| `Shift`, `F1` and `F7` | the card shows on the computer as a disk ([Getting started](getting_started.md)) |
| `Shift`, `F1` and `F8` | the INPUT TEST screen opens |

## Pattern editing
<!-- sync: src=docs/manual/pattern.md,docs/manual/panel.md -->

| Key | What it does |
|---|---|
| `Up`, `Down`, `Left`, `Right` | move the cursor. `Up` on step 1 goes to the last step |
| pad | in STEP, writes a note at the cursor; in WATCH, plays the sound |
| MIDI CC bound by Learn | in LIVE, a knob bound to CUTOFF, PANNING, a send, OVERDRIVE or BIT DEPTH also writes its move as a step FX on the play line; any other parameter is recorded as a CC letter, CC A to CC F (QA-PAT-324, QA-GLB-242) |
| the encoder as a Learn knob | the encoder target records the same way, in LIVE and in STEP (on the cursor's step) (QA-GLB-231, QA-GLB-232) |
| `Delete` | clears the cursor's entry, or the highlighted columns of the block |
| `Shift` + `Delete` | pulls the steps below up, to close a gap |
| `Insert` | inserts an empty step at the cursor; the steps below move down |
| `Copy` | copies the block, or the step under the cursor |
| `Shift` + `Copy` | pastes the copy at the cursor, in STEP |
| `Shift` + `Up` or `Down` | selects a block |
| `Shift` + `Left` or `Right` | grows the block one column; past the last column it takes the next track whole, then a track at a time |
| `F4` (**FILL**) | opens the Fill dialog |
| `F5` (**PREVIEW**) | sounds the note under the cursor while held |
| `F7` then `F6` (**RE FILL**) | repeats the last fill of the `F4` dialog or the last APPLY of the TOOLS dialog (the key then reads **REPEAT**) |
| `F6` (**EVENTS**) | opens the event list |
| `F7` (**TOOLS**) | opens the verb menu: pads 1-16 pick a verb (INVERT is pad 9), its `F6` repeats the last fill or apply, **APPS >** (pad 16) lists the tool apps; DUPLICATE, EXPAND, SHRINK and the exports are pads 10 to 12, 14 and 15 |
| `F8` (**UNDO**), `Shift` + `F8` (**REDO**) | take back and redo a pattern change |
| `Note`, `Instr`, `Vol`, `Fx` | select the column; hold to open its popup |
| `Note` + `Instr` | cycles the zoom: 5, 8 or 12 tracks |
| `Note` + `Left` or `Right` | moves one whole track |
| `F1` (**PATTERN**) | tap or hold: the pattern chooser (a tap opens it until `F1` or `F8`, a hold until you let go), and while the transport plays a pick switches the playing pattern at once. With it open, `Copy` copies the whole pattern and `Shift` + `Copy` writes it over the one shown |
| `F2` (**LENGTH**), `F3` (**JUMP**) | tap toggles the chooser; hold opens it |

## Shift combinations
<!-- sync: src=docs/manual/pattern.md,docs/manual/instrument.md,docs/manual/sample_load.md,docs/manual/mixer.md,docs/manual/file.md,docs/manual/panel.md -->

The soft keys with `Shift` held. `-` means the key does nothing. On Pattern, `Shift` + `F1` (OCTAVE) moves the pads' lowest C up an octave, and the labels NOTES, PIANO, ISO, TRACKS, PATS and VU choose what the pads do.

<!-- manual-tables: cheat shift -->
| Key | Pattern | Instrument | Sample load | Mixer | File |
|---|---|---|---|---|---|
| `Shift` + `F1` | **OCTAVE** | **NAME** | **0/4** | **NAME** | **0/4** |
| `Shift` + `F2` | **PADS** | **COPY** | **1/4** | - | **1/4** |
| `Shift` + `F3` | - | **PASTE** | **2/4** | - | **2/4** |
| `Shift` + `F4` | - | **DELETE** | **3/4** | - | **3/4** |
| `Shift` + `F5` | - | **SAVE** | **END** | - | **END** |
| `Shift` + `F6` | - | **<USED** | **MARK** | - | **NAME** |
| `Shift` + `F7` | - | **USED>** | **INST -** | **UNMUTE** | **PURGE** |
| `Shift` + `F8` | **REDO** | **REDO** | **INST +** | **UNSOLO** | - |

<!-- /manual-tables -->

| Other `Shift` keys | What it does |
|---|---|
| `Shift` + `Play` | Pattern: starts at the cursor's row. It never counts in |
| `Shift` + `Rec` | Pattern: turns LIVE on or off |
| `Shift` + `Insert` | Pattern: goes to step 0 and clears the block |
| `Shift` + `Delete` | Pattern: pulls the steps up. Instrument: sets every row to its default. Song: cuts the slot, or the selected cells in the track map |
| `Shift` + `Up` or `Down` | on lists: moves one page of 16 rows |
| `Shift` + `Right` or `Left` | Sample load: marks the file under the cursor and steps down (Right), or clears the marks (Left). **LOAD** and **ADD** then load every marked file |
| `Shift` + `Right` or `Left` | Instrument: the row's coarse step up or down, as `Shift` + encoder. Plain `Right` and `Left` are one click |
| `Shift` + `Note`, `Instr`, `Vol`, `Fx` | Pattern: jumps to a quarter of the pattern. On Song `Shift` + `Note` jumps to the previous slot with the same pattern |
| `Shift` + pad | Instrument: plays the instrument in view, as a pad alone does (change the instrument with `F6` and `F7`) |
| `Shift`, held alone | Pattern: the pads show the TRACKS layout while held |

| `Shift` + encoder | One click moves |
|---|---|
| Pattern, Note column | one octave |
| Pattern, Instr column | 8 instruments |
| Pattern, FX1 (volume) column | 10 |
| Pattern, FX2 column | 16 |
| Pattern, browsing in WATCH | one page of 16 rows |
| Instrument, CHANNEL row | 4 |
| Instrument, other rows | a coarse step |
| Mixer | a coarse step |
| Song | a fine step |

## Screens
<!-- sync: src=docs/manual/sample_load.md,docs/manual/sample_edit.md,docs/manual/record.md,docs/manual/perform_dj.md,docs/manual/mixer.md,docs/manual/file.md,docs/manual/instrument.md,docs/manual/pattern.md,docs/manual/panel.md -->

The soft keys `F1` to `F8`, left to right, as the key row shows them.

<!-- manual-tables: cheat screens screens=pattern,instrument,sample_load,sample_edit,record,mixer,file -->
| Screen | Button | F1 to F8 |
|---|---|---|
| [Pattern](pattern.md) | `Pattern` | **PATTERN**, **LENGTH**, **JUMP**, **FILL**, **PREVIEW**, **EVENTS**, **TOOLS**, **UNDO** |
| [Instrument](instrument.md) | `Params` | **SAMPLE**, **VOICE**, **FILTER**, **MOD**, **SENDS**, **<INS**, **INS>**, **UNDO** |
| [Sample load](sample_load.md) | `Sample Loader` | **LOAD**, **UP**, **ENTER**, **ADD**, **PLAY**, **RESCAN**, **WT AUTO**, **CH MIX** |
| [Sample edit](sample_edit.md) | `Sample Editor` | **CROP**, **NORM**, **REVERSE**, **FADE IN**, **FADE OUT**, **AMP**, **PREVIEW**, **UNDO** |
| [Record](record.md) | `Sample Recorder` | **REC/STOP**, **MONITOR**, **SAVE**, **DISCARD**, **SEEK-**, **SEEK+**, **SAVE WAV**, **PREVIEW** |
| [Mixer](mixer.md) | `Master` | **MIXER**, **ROUTING**, **MST FX**, **STEMS**, **CUE LVL**, **INSERTS**, **MUTE**, **SOLO** |
| [File](file.md) | `File` | **OPEN**, **RESCAN**, **NEW**, **SAVE**, **SAVE AS**, **IMPORT**, **EXPORT**, - |

<!-- /manual-tables -->

| Screen | Changes to |
|---|---|
| Instrument | `F1` reads SAMPLE, SLICE, WT or GRAIN; `F6` and `F7` show the neighbours, such as `<06` and `08>` |
| Sample edit, FX page (`Sample Editor` again) | `F1` **FILTER**, `F2` **COMPRESS** |
| Record, loop review | `F1` **OVERDUB**, `F5` **UNDO LYR**, `F8` **LOOP ON** or **MUTED**; `F6` does nothing |
| Perform, slot page, `Shift` held | `Shift` + `F4` **PICK -**, `F5` **PICK +**, `F6` **TRK PAT**, `F7` **TRK NEXT**, `F8` **TRK SONG**: another pattern on the tracks that are ON, at each track's loop end (see [Perform](perform.md)) |
| Perform, DJ page | `F5` **SCR** (the encoder is the platter), `F6` **LATCH**, `F8` **RESET**; `Shift` + `F1` to `F8` do nothing |
