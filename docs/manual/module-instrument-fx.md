<!-- manual-sync: c3ee4c54 -->
# Step FX on module instruments

Status: draft

A module instrument obeys fewer step FX than a sampler, and it obeys Glide, Slide and Arp in its own way: a slide is a legato note, not a restarted sample. This page lists which step FX the TB-303, Chip, FM and any instrument `.tmod` of the current ABI obey. Tune works on modules; Micro-tune, the Finetune LFO and tracker FX 46, 49 and 50 act on sampler or Amiga voices only. New in track3r-os. <!-- og: new --> <!-- docs/specs/OS-MODPITCH.md, lines 13 to 15 -->

## What it is for

Come here when a pattern with FX sounds different on a module instrument than on a sample. The FX stay in the pattern as written; nothing changes in them when the instrument is a module. Step FX themselves are in [Effects in the pattern](fx.md).

## What a module instrument does with step FX
<!-- sync: src=docs/specs/OS-MODPITCH.md qa=QA-MOD-099,QA-MOD-100,QA-MOD-101,QA-MOD-102,QA-MOD-103,QA-MOD-104 -->

The Sampler column is what the FX specs say; the module column is checked on the host (event stream and exact ramps). Not yet checked on the unit.


| FX | Sampler instrument | Module instrument | Evidence |
|---|---|---|---|
| Glide (19) | slides the pitch from the previous note | the note is **legato** (no new envelope) and its pitch slides from the previous note of the track in the Glide time; the first note of a track, a repeat of the same note after a release and a Glide of 0 are ordinary notes; the same note while it sounds is a tie | host tests; unit open |
| Slide up (41), Slide down (42) | slide the pitch | the pitch slides to the distance (sixteenths of a semitone) in one step of the tempo, as a legato note; on a row with no note the sounding note slides from where it is; a Slide beats a Glide; a Slide of 0 changes nothing; the end of the FX puts the note's pitch back | host tests; unit open |
| Arp (21) | plays the arpeggio | **plays the arpeggio**, but only with a Chord (14) in the other FX column (as on a sampler): each tone is a new, retriggered note. An Arp alone does nothing | host test of the event stream (OS-MODPITCH step 1); unit open. Earlier text that said Arp had no effect on a module was an FX-column mistake |
| Chord (14) | plays the root alone | plays the root alone (a module is one voice a track); use CHORDS in SPREAD mode for a chord | `QA-MOD-083` |
| Overdrive (38), Bit depth (39) | drive and bit reduction of the voice | the same curves over the module's output: the drive gain 1 + 15 (v / 100)^2, the depth cuts the low 16 minus bits bits; a value of 0 (Overdrive) or 16 (Bit depth) changes nothing; the Perform OVERDRV and BITDEPTH rows win over the FX | host tests (`QA-MOD-200..205`); unit open |
| Chance (4), Micro-move (2) | as in the Pattern chapter | not verified either way | open research question |

## Notes

- A module of ABI 1.7 or below, or a 1.8 module that does not export `pitch`, ignores Glide and Slide as before; the notes are ordinary notes.
- The TB-303's own Slide and Legato Slide parameters are for live legato keys; on a pattern note the slide time comes from the Glide value or the Slide speed.
- The FX stay in the pattern as written; nothing is changed by the instrument being a module.
- Overdrive (FX 38), Bit depth (FX 39) and the Perform OVERDRV and BITDEPTH rows shape a module instrument's sound exactly as they shape a sampler's: the same curves, applied to the module's output after its own filter. New in track3r-os. <!-- docs/specs/OS-BUG-MODDRIVE.md -->
- A module instrument has no OVERDRIVE or BIT DEPTH row of its own: its Params page shows only the module's parameters, so the amount comes from the FX or the Perform rows. Without one of them the sound is the module's, untouched.
- The shaping is applied once per audio block (about 1.45 ms) with the values in force at the end of the block, so an FX can start up to one block early, on the tail of the previous note. The Stopped state does not end a held FX 38 or 39 (as on a sampler); the next FxReset does.
- The shaping costs processor time for each shaped module track (about 0.2 to 0.5 % of the budget each, estimated; the measurement on the unit is open). It also narrows how many module notes the engine admits at once, for every module, while it runs. The export (PATT STEMS and the song export) hears the same shaping as playback, except that the Perform rows are off during an export, as for sampler tracks.

## See also

- [Effects in the pattern](fx.md): how to put an effect on a step.
- [Modules](modules.md): module instruments and apps.
- [TB-303, FM and Chip: new parameters](module-sounds.md): the built-in sounds.
