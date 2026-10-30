# Effects in the pattern
<!-- manual-sync: f5e29c02 -->

Status: draft

Each step has two effect cells, FX1 and FX2, next to the note, and an effect changes how that note plays: a chord, an arpeggio, a slide, a drive or a volume change. The list holds effects 0 to 42, which are the original layout's ids (Chord, Arp, Tune, Slides, Overdrive and Bit Depth among them), and at its end 17 tracker effects. A tracker effect starts at a sensible default, and the info line reads the value in words. New in track3r-os: the 17 tracker effects. <!-- og: new, ids 43 on (docs/specs/OS-COREFX.md) --> Not checked against the original firmware: how the first 43 read and behave. <!-- og: open -->

<!-- Sources: docs/qa/catalogue/pattern.md, the Fx column rows QA-PAT-011..029, 096..117, 140..149, plus QA-PAT-015, 024, 101, 111, 145, 147, 224, 251. Walk-through: sim/scenarios/manual/fx_walk.txt (the effect is Reverse, the first the list reaches from an empty cell). -->
<!-- TODO: ask: how a pad enters an effect value (QA-PAT-011 says "encoder or pad"; the catalogue does not say which pad gives which value). -->

## What it is for

Use a step effect when one note needs its own treatment and a new instrument would be too much. You come here from [Pattern](pattern.md): press `Rec` for **STEP**, then `Fx`.

## How it works
<!-- sync: src=seq/include/o3os/seq/fx.h,app/include/o3os/app/fx_text.h spec=docs/specs/OS-PFX.md qa=QA-PAT-011,QA-PAT-016,QA-PAT-017,QA-PAT-104,QA-PAT-114,QA-PAT-117,QA-PAT-147 -->

### A step FX: live, stored and ended

A step FX is a mark on one step, like "slide here" on a score. Two cells mean two effects can work together on one note. <!-- QA-PAT-011 -->

- **Live:** the effect acts when the sequencer reaches the step, and the grid shows its name, letter or mark and its value in hex.
- **Stored:** in the pattern, on that step, exactly as written. The FX stay as written even when the instrument is a module. <!-- QA-PAT-104 -->
- **Ended:** most effects end with their step. **Panning** ends with its step. **Channel Pan** stays on the track until another Channel Pan, or until you stop and play again. Stop also gives channel and global volumes back, so the pads sound at full level. <!-- QA-PAT-114 QA-PAT-117 QA-PAT-147 -->

### The value

The value sets how strong the effect is. The grid shows it in hex and the info line reads it in words, for example `DRIVE 40 %`. A new effect starts at its own default: Tune starts at 0 semitones. If a cell has an effect and you change its type, the value stays, cut to the new range. <!-- QA-PAT-011 QA-PAT-104 -->

![Pattern, STEP mode: the info line reads CHORD 5A for the effect under the cursor, in words.](img/pattern_qa_pat_011.png)

### Two effects that work together: Chord and Arp

A **Chord** on a MIDI track sounds all its notes at once. An **Arp** plays them one at a time, a note every 8 steps down to every 1/8 step. Example: a C note with Chord 5 (major) in FX1 and Arp 6 in FX2 plays C, then E, then G, one a step. With no Chord, an Arp changes nothing. On a sample track the Arp restarts the sample at each new pitch. <!-- QA-PAT-016 QA-PAT-017 QA-PAT-018 QA-PAT-021 QA-PAT-212 -->

## Try it
<!-- sync: src=app/src/bindings.cpp,sim/scenarios/manual/fx_walk.txt qa=QA-PAT-011,QA-PAT-015,QA-PAT-251 -->

1. Press `Pattern`, then `Rec` (**STEP**), then `Fx`. The cursor is on FX1 of step 1. <!-- QA-PAT-011 -->

   ![Pattern, STEP mode: the cursor is on the first effect cell of step 1.](img/fx_walk_1.png)

2. Hold `Vol`. The effect list opens with a description under it. Turn the encoder 14 detents to **Reverse**. <!-- QA-PAT-015 -->

   ![Pattern: the FX list is open and Reverse is highlighted, with its description under the list.](img/fx_walk_2.png)

3. Let go of `Vol`. The cell shows the effect's three-letter name, **REV**. <!-- QA-PAT-316 -->
4. Turn the encoder up. The value stops at its last value, 01, and does not wrap. <!-- QA-PAT-251 -->

   ![Pattern: FX1 of step 1 reads REV01.](img/fx_walk_4.png)

5. Press `Fx` again. The cursor moves to FX2, and FX1 keeps its effect.

   ![Pattern: the cursor is on the second effect cell, FX1 reads REV01.](img/fx_walk_5.png)

6. Press `Play` and listen to step 1: Reverse sets the direction the sample plays in. Not yet checked on the unit.

## The screen at a glance

| Area | What it shows |
|---|---|
| Fx column | Two effect cells per step, FX1 and FX2. Each shows the effect and its value in hex. At the 5-track zoom the effect is its three-letter name, for example `CHD` for Chord. An effect without a name, and the cells of the older zooms, show a letter or a mark. <!-- QA-PAT-316 --> |
| Info line | The effect under the cursor, in words, after the mode chip. For example `CHORD 5A`. |
| Effect list | The list of effects, shown while you hold `Vol`. The newest effects are at the end. |

## More tasks

### Set a big value

1. Hold `Shift` and turn the encoder. The value moves 16 at a time. <!-- QA-PAT-251 -->

The value stops at its first and last value. It does not wrap around.

### Clear an effect

Start on the Pattern screen, in **STEP**, on the step with the effect.

1. Hold `Vol`. The effect list opens. <!-- QA-PAT-104 -->
2. Turn the encoder to **None**.
3. Let `Vol` go. The cell is empty.

To clear the effects of several steps at once, put the cursor on **FX1** and press `Shift` + `Down` for each extra step. Press `Shift` + `Right` once more to add **FX2**, then `Delete`. The note and volume stay, and one `F8` (**UNDO**) takes it back. New in track3r-os. Not yet checked on the unit. <!-- QA-PAT-319, QA-PAT-320 -->

### Play a chord as an arpeggio

Start on the Pattern screen, in **STEP**, on a step with a C note. Use a MIDI track or a sample track.

1. Put **Chord** in FX1 with the value 5. The value 5 is a major chord, 4 is a minor chord. <!-- QA-PAT-212 -->
2. Put **Arp** with the value 6 in the other effect cell. The value 6 plays the chord up, one note a step. <!-- QA-PAT-017 -->
3. Press `Play`. The notes C, E and G play one after another, one a step. <!-- QA-PAT-017 -->

### Look up an effect with words for its value

These effects read their value in words on the info line.

| Effect | What it does | Info line, with an example value |
|---|---|---|
| Chord | Plays the note as a chord. Value 5 is major, 4 is minor. | `CHORD 5A` for value 8 <!-- QA-PAT-011, QA-PAT-212 --> |
| Arp | Plays a chord one note at a time. It needs a Chord in the other cell. | `ARP DOWN 4 STEPS` for value 14 <!-- QA-PAT-012, QA-PAT-017 --> |
| Tune | Moves the note in semitones. Value 24 is no change. | `TUNE +7 ST` for value 31 <!-- QA-PAT-013, QA-PAT-019 --> |
| Slide Up, Slide Down | Slides the pitch over the step. The value is sixteenths of a semitone. | `SLIDE UP 2.19 ST` for value 35 <!-- QA-PAT-014, QA-PAT-020 --> |
| Glide | Slides from the last note on the track to this one. Value 0 is off. | `GLIDE 240 MS` for value 12 <!-- QA-PAT-025, QA-PAT-028 --> |
| Overdrive | Drives the sound. Value 0 is clean. | `DRIVE 40 %` for value 40 <!-- QA-PAT-022, QA-PAT-026 --> |
| Bit Depth | Lowers the bit depth. The range is 1 to 16, and 16 is off. | `BITS 8` for value 8 <!-- QA-PAT-023, QA-PAT-027 --> |
| Panning | Pans the note. Value 100 is hard right. It ends with its step. | <!-- QA-PAT-117 --> |
| Volume LFO, Panning LFO, Finetune LFO, Position LFO | Start the track's LFO for the volume (tremolo), the pan (auto-pan), the pitch (vibrato) or the wavetable window or granular position, on a sampler instrument. These four did nothing before; they act now, as Filter LFO does. The value is the LFO's speed in the same list as the instrument's own LFO speed, from the slowest at 0: 0 to 23 for Volume LFO, 0 to 28 for the others. The shape and amount are the instrument's LFO for that target (an instrument with none: triangle, 50 %). Position LFO moves only a wavetable or granular instrument and does nothing on a plain sample. While a Perform LFO row is held on the track its speed wins, and the FX comes back when the row ends or is at 0. Panning LFO and Panning (31) on one track: while the LFO runs it wins, because it sets the pan every block (a Panning step after it is overridden too); when the LFO ends the instrument's pan is back, and Panning acts again at its next step. The LFO stops at the next transport start. Stop does not end it: a pad note played after Stop still gets the LFO until the next Play. | <!-- QA-PRF-180..189 --> |

<!-- TODO: ask: the info line text for Panning. The catalogue gives only the behaviour. -->
<!-- TODO: ask: the core effects with no row here: Gate Length, Low-pass and MIDI Chord. -->

### Look up a tracker effect

The tracker effects are at the end of the list, after Bit Depth. Each has a mark in the grid. Their values are hex. <!-- QA-PAT-024, QA-PAT-096 -->

| Effect | Mark | Value | Info line, with an example value |
|---|---|---|---|
| Tracker Arp | `@` | 37 | `ARPEG +3 +7`. Value 00 is `ARPEG OFF`. <!-- QA-PAT-096 --> |
| Volume Slide | `^` | 04 | `VSLIDE -4`. Value 40 is `VSLIDE +4`. <!-- QA-PAT-097 --> |
| Fine Volume Slide | `~` | 30 | `FVOL +3` <!-- QA-PAT-098 --> |
| Fine Porta Up | `<` | 10 | `FINE UP 4.00 PER` <!-- QA-PAT-099 --> |
| Fine Porta Down | `>` | 01 | `FINE DOWN 0.25 PER` <!-- QA-PAT-100 --> |
| Pan Slide | `=` | 04 | `PANSL R 4` <!-- QA-PAT-105 --> |
| Vibrato Wave | `"` | 02 | `VIB SQUARE`. Waves are 0 sine, 1 ramp, 2 square, 3 random. <!-- QA-PAT-106 --> |
| Tremolo Wave | `'` | 07 | `TREM RANDOM NORETRIG` <!-- QA-PAT-107 --> |
| Glissando | `;` | 01 | `GLISS ON`. Value 00 is `GLISS OFF`. <!-- QA-PAT-108 --> |
| Retrigger | `/` | 23 | `RETRIG EVERY 3 VOL -2` <!-- QA-PAT-109 --> |
| Tremor | `:` | 21 | `TREMOR ON 3 OFF 2` <!-- QA-PAT-110 --> |
| Channel Pan | `\|` | 4B | `CHPAN 75`. 50 is the centre. <!-- QA-PAT-116 --> |
| Channel Volume | `[` | 20 | `CHVOL 32`. 64 is full. <!-- QA-PAT-140 --> |
| Channel Vol Slide | `]` | 04 | `CHVSL -4` <!-- QA-PAT-141 --> |
| Global Volume | `(` | 30 | `GVOL 48`. 64 is full. <!-- QA-PAT-142 --> |
| Global Vol Slide | `)` | 40 | `GVSL +4` <!-- QA-PAT-143 --> |
| Panbrello | `{` | 3A | `PANBR SPD 3 DEP 10` <!-- QA-PAT-144 --> |

## Keys on this screen

| Key | What it does |
|---|---|
| `Fx` | Selects the Fx column on FX1. Press it again to move to FX2. <!-- QA-PAT-011 --> |
| hold `Vol` | Opens the effect list. Let go to enter the effect that is highlighted. <!-- QA-PAT-015 --> |
| encoder | Changes the value of the effect in the cell. While `Vol` is held, it moves through the list. <!-- QA-PAT-011, QA-PAT-015 --> |
| `Shift` + encoder | Steps the value by 16. It stops at the first and last value. <!-- QA-PAT-251 --> |
| `F8` (**UNDO**) | Takes back the last change, with the other Pattern edits. A Song or Params edit stays. When nothing is left, the strip says **NOTHING TO UNDO HERE**. <!-- QA-PAT-246 --> |

## Good to know

- Not yet checked on the unit: how the effects sound. The simulator and host tests pass for the effects in this chapter. The unit check is still open.
- New in track3r-os: the tracker effects, from Tracker Arp to Panbrello. <!-- QA-PAT-024, QA-PAT-101, QA-PAT-145 -->
<!-- TODO: ask: confirm that the original firmware has none of the tracker effects, so the New in track3r-os label is backed by evidence. -->
- Writing a note by hand over a step that has a note replaces the note and its instrument with the ones in force. The volume and both effect cells stay. New in track3r-os. Not yet checked on the unit. <!-- QA-PAT-315 -->
- The tracker effects run once a tick and are built to add only a few tenths of a percent of processor time. Not yet measured on the unit. <!-- QA-PAT-103, QA-PAT-115, QA-PAT-149 -->
- Stop gives the channel and global volumes back, so the pads sound at full level after a stop. <!-- QA-PAT-147 -->
- The effect list has 60 entries. <!-- QA-PAT-145 -->
- Not checked against the original firmware: the Chord and Arp readings. This is an open research question.
<!-- TODO: ask: parity of Chord and Arp against the original firmware. OS-PFX tags the chord table Q and no row gives verified parity. -->

## See also

- [Pattern](pattern.md): notes, the record modes and the Fx column keys.
- [Fill and tools](pattern_tools.md): fill a track with notes by a rule.
- [Modules and apps](modules.md): which effects a module instrument obeys is in [Step FX on module instruments](module-instrument-fx.md).
