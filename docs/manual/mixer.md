<!-- manual-sync: 571f81a -->
# Mixer
<!-- sync: src=app/src/screens/mixer_screen.cpp,app/include/o3os/app/mixer_view.h,audio/include/o3os/audio/graph.h spec=docs/specs/OS-005-audio-engine.md qa=QA-MIX-002,QA-MIX-010,QA-MIX-013,QA-MIX-030,QA-MIX-032,QA-MIX-033,QA-MIX-034,QA-MIX-035,QA-MIX-036 -->

Status: draft

The Mixer has four pages, and two are not a fader strip. **ROUTING** decides where each track goes, to the speakers, the cue output, the recorder or the USB stems. **STEMS** decides what each USB slot carries, and, on its first row, how many outputs the USB stream has (2, 4, 6, 8 or 10). ROUTING, STEMS, the cue output and the two output modes are new in track3r-os. Hold `Master` and turn the encoder to set the master volume from any screen, and `Shift` + `F1` names a track. The Mixer has no undo. <!-- og: open -->

## What it is for

Use the Mixer to set levels and pans, route tracks, mute and solo, and watch the master limiter. Press `Master`. `F1` to `F4` pick the page, with the cursor on its first row. `F6` has no function here. Pressing `Master` again turns to the next page. <!-- QA-MIX-031, QA-MIX-032, QA-MIX-033, QA-MIX-034, QA-MIX-036 -->

To set a level or a pan on the MIXER page, press `Left` or `Right` to pick the column and `Up` or `Down` to pick the field (level or pan). Turn the encoder to set the value. `Shift` makes the step coarse. `Delete` puts back the default.

`F5` goes to **CUE LEVEL** on the MASTER FX page, from any page. On the MIXER page, `F7` mutes and `F8` solos the track under the cursor. `Shift` + `F7` unmutes every track, and `Shift` + `F8` unsolos them. On Pattern and Perform, hold `Shift` and press a pad in the second row to mute its track. The mute shows on the strip, in the Pattern header and on the pads. <!-- QA-MIX-035, QA-MIX-003, QA-MIX-004, QA-MIX-020 -->

## How it works
<!-- sync: src=audio/include/o3os/audio/graph.h,app/include/o3os/app/mixer_view.h spec=docs/specs/OS-005-audio-engine.md qa=QA-MIX-002,QA-MIX-013,QA-MIX-020 -->

### Routing: where a track goes

Think of a mixing desk. Each track has a fader and a pan, and a set of switches for the buses it feeds. A bus is a shared sum. The buses are **MAIN** (the speakers), **CUE** (a second output for monitoring), **SND A** and **SND B** (the send effects), **REC** (what the recorder hears) and **USB** (the stems to the computer). New in track3r-os. <!-- QA-MIX-029 -->

On **ROUTING**, a row is a source and a column is a bus. Put the cursor on a cell and turn the encoder up to route, down to remove the route. Example: route a kick to CUE as well as MAIN, and you hear it on the cue output too.

Below the tracks are three more rows. **LINE IN** is the audio input. Its row has no USB cell. **RET A DELAY** and **RET B REVERB** are the returns of the two send effects, and each can go to MAIN, CUE or both. The last row, **OUT MODE**, is not a grid: turn the encoder up for **MAIN L / CUE R** (the speakers on the left, the cue on the right), down for **STEREO**. The header shows the mode as **OUT**. Delete puts back **STEREO**. <!-- QA-MIX-029 -->

![Signal flow: instruments feed track strips, buses and the master limiter to the outputs. USB audio goes in and out beside them.](img/concept_signal_flow.svg)

### Sends: a second volume knob

A send takes a copy of a track and feeds it to a shared effect. **SND A** is the delay and **SND B** is the reverb. The send amount is the instrument's **DELAY SEND** and **REVERB SEND**, from 0 to 100, on the SENDS page of Instrument. By default a send taps the track before its fader, so the fader does not change it. The project's SENDS setting can make the tap come after the fader. A return brings the effect's sound back to MAIN and CUE. <!-- audio/graph.h, model/src/params.cpp, OS-005 -->

- **Stored:** routes, levels, pans, names, the mutes and solos, and the effect settings are saved with the project.
- **Live:** the meters.
- **Ended:** nothing ends them. The Mixer has no undo, so every change is final. <!-- QA-MIX-013 -->

Example: the pad sound is dry. Raise its **REVERB SEND** to 40 on Instrument. On **MASTER FX**, **REVERB** sets how loud the reverb is in the mix. Mute the reverb with **REVERB MUTE**, and the tail stops being added.

### Mute and solo

`F7` (**MUTE**) closes the track's strip, and `F8` (**SOLO**) mutes every other track. By default a solo also silences the other tracks' sends. Unmuting brings the note back where it would be, with no click. <!-- QA-MIX-020, QA-MIX-022 --> <!-- audio/graph.h; og: open -->

## Try it
<!-- sync: src=app/src/screens/mixer_screen.cpp,app/include/o3os/app/mixer_view.h qa=QA-MIX-003,QA-MIX-030 -->

1. Press `Master`. The **MIXER** page shows a column for each track.

   ![Mixer, MIXER page: a column for each track and bus with its level, pan and meter.](img/mixer_walk_1.png)

2. Press `F2` (**ROUTING**). A row for each track, a column for each bus.

   ![Mixer, ROUTING page: tracks down the side, MAIN, CUE, SND A, SND B, REC and USB across.](img/mixer_walk_2.png)

3. Press `Right` once to **CUE** and turn the encoder up. Track 1 now goes to the cue output as well as MAIN.

   ![Mixer, ROUTING page: the cursor is on the CUE cell of track 1, now routed.](img/mixer_walk_3.png)

4. Press `F3` (**MST FX**). The **MASTER FX** page lists the returns and the controls of the two send effects.

   ![Mixer, MASTER FX page: the master section, the returns and the limiter card, which reads ALWAYS ON.](img/mixer_walk_4.png)

5. Press `Left` to put the cursor back on track 1, because ROUTING and MIXER share the column. Press `F1`, then `F7` (**MUTE**) to mute track 1. Press `Shift` + `F7` (**UNMUTE**) to open every strip. <!-- QA-MIX-003 -->

   ![Mixer: track 1 is muted.](img/mixer_walk_5.png)

6. Go to Pattern. Hold `Master` and turn the encoder. The strip says **MASTER VOLUME**, and the screen does not change. <!-- QA-MIX-030 -->

   ![Pattern: the strip shows the master volume while Master is held.](img/mixer_walk_6.png)

Not yet checked on the unit.

## The screen at a glance

![Mixer, MIXER page: columns for tracks and buses; the key row lists the pages.](img/mixer_qa_mix_001.png)

| Area | What it shows |
|---|---|
| Status line | The page, the transport and the CPU load. |
| Header | The page names, the current one underlined. Meters use 32 ms windows. |
| Main area | A column for each track and bus, or a grid of routes. |
| Info line | What the cursor is on, for example **T3 level**, and the keys that work. |
| Strip | Messages, for example **TRACK 03 NAMED**. |
| Key row | `F1` to `F8`; see "Keys on this screen". |
| Shift row | **NAME** on `F1`, **UNMUTE** on `F7`, **UNSOLO** on `F8`. |

The pages: **MIXER** (tracks, then **IN**, **SNDA**, **SNDB**, **CUE**, **MST**; level and pan), **ROUTING**, **MASTER FX** (volume, dry, reverb, delay, three mutes, cue level, the send effects and the limiter's threshold, attack and release) and **STEMS** (a row for each USB slot **S1** on, a column for each source: **T1** to **T12**, **ML**, **MR**, **SA**, **SB**). On STEMS, `Insert` adds the source and `Delete` removes it. New in track3r-os. `Shift` + `Delete` restores the factory map. Both are final, so each asks first (**DELETE AGAIN** or **SHIFT+DELETE AGAIN**): press the same key again within two seconds to do it. <!-- QA-MIX-039 --> The encoder changes a value; `Shift` is coarse; `Delete` sets the default.

## More tasks

### Name a track

Start on the MIXER page, with the cursor on a track.

1. Press `Shift` + `F1`. The keyboard opens, titled **TRACK NAME**. <!-- QA-MIX-010 -->
2. Type with the pads, up to 20 characters. `F8` (**OK**) keeps it. `F7` (**CANCEL**) leaves the old name.

You should see **TRACK 03 NAMED**. The name shows on the Mixer, ROUTING and the Pattern header, and is saved. On IN, SND A, SND B, CUE, MASTER or the MASTER FX page the strip says **TRACKS ONLY**. <!-- QA-MIX-011, QA-MIX-012 -->

### Clear every mute or solo

1. Press `Shift` + `F7` (**UNMUTE**) to open every strip. <!-- QA-MIX-003 -->
2. Press `Shift` + `F8` (**UNSOLO**) to clear every solo. <!-- QA-MIX-004 -->

### Mute a track from the pads

On Pattern or Perform, hold `Shift` and press the track's pad in the second row. Press it again to unmute. <!-- QA-MIX-020 -->

### Choose how many outputs the USB stream has

The USB audio device can present 2, 4, 6, 8 or 10 outputs to the computer. A project keeps its own number. This is new in track3r-os.

1. Press `F4` (**STEMS**). The first row is **OUTPUTS**, with the number the computer sees now. <!-- QA-MIX-050 -->
2. Turn the encoder, or press `Left` or `Right`, to the number you want. The row shows a star (\*). Nothing has changed yet. <!-- QA-MIX-051 -->
3. Press `F6` (**APPLY**). The strip says **USB WILL RECONNECT: PRESS AGAIN**. <!-- QA-MIX-052 -->
4. Press `F6` again within two seconds. The computer drops the unit for a moment and finds it again with that many outputs. The slots go back to the factory layout for that number: with 4 outputs, the first two tracks and the master pair. Re-map them on the same page if you want. <!-- QA-MIX-052 -->

`Delete` on the **OUTPUTS** row drops the starred number, and so does leaving the page. If the strip says **USB BUSY: TRY AGAIN**, the USB port is still changing: wait a moment and press `F6` twice again. With nothing starred, `F6` says **NOTHING TO APPLY**. <!-- QA-MIX-053 --> <!-- QA-MIX-054 -->

The number is saved with the project (File > SAVE). Projects saved with 2, 8 or 10 outputs are the same files as before. <!-- QA-MIX-057 -->

A project saved before this version has no USB settings: when you open it, the unit keeps the outputs of the project you had open before it. On STEMS the built-in `F6` (**APPLY**) wins over an `F6` in a user keymap. A keymap line that names `mixer.stems` is refused by older firmware.

Not yet checked on the unit: that a Linux, macOS or Windows computer sees each number. <!-- QA-MIX-056 -->

### Play the current instrument from the pads

On any page of the Mixer, hold a pad: the current instrument sounds at the pad's note until you let go. Nothing on the screen changes. Config and File do the same. This is new in the Community OS. <!-- QA-MIX-040 -->

### Set the master volume from any screen

1. Hold `Master` and turn the encoder. It moves from 0 to 100.
2. Let go of `Master`. Nothing else happens. <!-- QA-MIX-030 -->

## Keys on this screen

<!-- manual-tables: keys screen=mixer -->
The key row, and the row drawn while `Shift` is held. `-` means the key does nothing.

| Key | Label | With `Shift` |
|---|---|---|
| `F1` | **MIXER** | **NAME** |
| `F2` | **ROUTING** | - |
| `F3` | **MST FX** | - |
| `F4` | **STEMS** | - |
| `F5` | **CUE LVL** | - |
| `F6` | - | - |
| `F7` | **MUTE** | **UNMUTE** |
| `F8` | **SOLO** | **UNSOLO** |

<!-- /manual-tables -->

| Key | What it does | Ref |
|---|---|---|
| `Master` | opens the Mixer, when you let go of the key | QA-MIX-001, QA-MIX-031 |
| `Master`, held, with the encoder | changes the master volume on any screen. New in track3r-os. | QA-MIX-030 |
| `F1` (**MIXER**) | the MIXER page, one column for each track, with the cursor on its first row | QA-MIX-010, QA-MIX-032 |
| `F2` (**ROUTING**) | the ROUTING page, one row for each track, with the cursor on the first source | QA-MIX-033 |
| `F3` (**MST FX**) | the MASTER FX page. The card reads **MASTER LIMITER**, **ALWAYS ON**. | QA-MIX-002 |
| `F4` (**STEMS**) | the STEMS page, one column for each source. The first row is **OUTPUTS**, the slots follow. With no stem map the page says so. | QA-MIX-034, QA-MIX-050 |
| `F5` (**CUE LVL**) | goes to the CUE LEVEL row of the MASTER FX page, from any page; on that row it changes nothing | QA-MIX-035 |
| `F6` | on STEMS, **APPLY**: the number of outputs you chose, after a second press within two seconds. On the other pages nothing: the key reads `--` | QA-MIX-036, QA-MIX-052, QA-MIX-055 |
| `F7` (**MUTE**) | mutes the track under the cursor, on MIXER, ROUTING and STEMS (the track of the column). On **SNDA** and **SNDB** it mutes the effect; on MASTER FX it mutes the bus of the DRY, REVERB or DELAY row; on STEMS **SA** and **SB** mute their effect. Anywhere else the strip says why. | QA-MIX-003, QA-MIX-037, QA-MIX-038 |
| `F8` (**SOLO**) | solos the track under the cursor, on MIXER, ROUTING and STEMS. On MASTER FX, and on a bus, the strip says **TRACKS ONLY**. | QA-MIX-004, QA-MIX-037, QA-MIX-038 |
| `Shift` + `F1` (**NAME**) | names the track under the cursor. On IN, SND A, SND B, CUE or MASTER, or on the MASTER FX page, the strip says **TRACKS ONLY**. | QA-MIX-010, QA-MIX-012 |
| `Shift` + `F7` (**UNMUTE**) | unmutes every track | QA-MIX-003 |
| `Shift` + `F8` (**UNSOLO**) | clears the solo of every track | QA-MIX-004 |
| `Shift` + `F2` to `F6` | nothing | QA-MIX-001 |
| the pads, on the MIXER, ROUTING, STEMS and MASTER FX pages | play the current instrument while held | QA-MIX-040 |
| `Shift` + the track's pad in the second row (Pattern or Perform) | mutes the track while it plays; again to unmute | QA-MIX-020 |
| the pads, on the on-screen keyboard | type the name | QA-MIX-010 |
| `F7` (**CANCEL**), on the on-screen keyboard | keeps the old name | QA-MIX-010 |
| `F8` (**OK**), on the on-screen keyboard | keeps the new name | QA-MIX-011 |

## Good to know

> **Careful**: The Mixer has no undo. `F8` here is **SOLO**, and `F8` on Pattern, Song or Params does not bring a Mixer change back. <!-- QA-MIX-013 -->

- The master has no bit-depth stage. The MST FX page lists only the limiter. Bit depth is a step effect and a Perform slot. <!-- QA-MIX-002, QA-PRF-014 -->
- `Master` acts when you let go of it, because it waits to see whether you turn the encoder. If `Shift` joins after `Master`, the tap is lost. <!-- QA-MIX-031 -->
- The master limiter is always on. The page lists no headroom, saturation, EQ, bass, space or bit-depth stages. <!-- QA-MIX-002 -->
- A muted track still exports as a stem, with its sound in it. <!-- QA-MIX-024 -->
- The first hit after an unmute can be soft: it loses up to 1.45 ms to a short ramp. It never clicks. <!-- QA-MIX-025 -->
- Not yet checked on the unit: mute and unmute mid-note, the master volume, naming, stems. Not checked against the original firmware: the pages and keys. <!-- og: open -->

## See also

- [Instrument](instrument.md): the DELAY SEND and REVERB SEND of each sound.
- [Record](record.md): the REC bus is what the recorder hears.
- [Export](export.md): stems to the USB or the card.

<!-- Open rows, not written: QA-MIX-021, QA-MIX-023, QA-MIX-026, QA-MIX-027, QA-MIX-028 -->
