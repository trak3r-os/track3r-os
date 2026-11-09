<!-- manual-sync: 571f81aa -->
# Input test
<!-- sync: src=app/include/o3os/app/screens/input_test_screen.h qa=QA-INP-001,QA-INP-002,QA-INP-003,QA-INP-004,QA-INP-005,QA-INP-006,QA-INP-007,QA-INP-008 -->

Status: draft

Input test is a hidden screen that draws every button, pad and turn of the encoder as the unit reads it, and counts them. No screen button and no Navigator entry opens it: you hold three keys while the unit turns on. While it is up, nothing acts on a key, so you can press everything, `Play` and `Song` included. This screen is new in track3r-os. <!-- og: new -->

## What it is for

Use Input test to check that every key and pad of your unit answers, and how fast. Switch the unit off. Hold `Shift`, `F1` and `F8`, switch the unit on, and keep them down until the strip reads **INPUT TEST**. The picture starts clean, with every count at zero. <!-- QA-INP-001 -->

## The screen at a glance

| Area | What it shows |
|---|---|
| Strip, right side | `PADS OK  KEYS OK  ERR n  DROP n  FLT n  RSYNC n`. **OK** reads **NO** when a scanner does not answer. The other four are counters taken over from the input path. <!-- QA-INP-005 --> |
| Buttons | 33 cells in 11 columns of 3, named as the keycaps: `F1`, `PAT`, `SHIFT`, `LEFT`. <!-- QA-INP-002 --> |
| Button caption | `AGE x.x MS  MAX x.x MS  EVENTS n`: the age of the last event, the longest age seen, and the events counted. <!-- QA-INP-005 --> |
| Pads | 48 cells in 12 columns of 4, each with its number in the corner. The caption reads `PADS  (0 = top left, 11 = top right)`. <!-- QA-INP-003 --> |
| Encoder line | `ENCODER  TOTAL +n  LAST +n  EVENTS n`, dim until the first turn. <!-- QA-INP-004 --> |
| Key row | `F2` and `F3` read **CLEAR**, `F4` and `F5` read **EXIT**. The rest are blank. <!-- QA-GLB-121 --> |

The pads are numbered from 0 here, not from 1 as in the rest of the manual: pad 0 is the top left, pad 11 the top right.

## More tasks

### Test the buttons

Start on Input test.

1. Press each button once, and hold one. The cell lights while the key is down. <!-- QA-INP-002 -->
2. Let go. The cell shows its count, `1` after one press. A cell you have not touched stays plain.
3. Press the same button again. The count reads `2`. A bounce in the key reads as `2` for one touch.

A press that the chip repeats counts once. The count stops at `99`.

### Test the pads and the encoder

1. Touch each pad, harder and softer. The cell lights while the pad is down and shows its count after. <!-- QA-INP-003 -->
2. Turn the encoder both ways. **TOTAL** is the sum of the clicks with its sign, **LAST** the last turn (`-2` for two clicks to the left) and **EVENTS** the number of turns. <!-- QA-INP-004 -->

### Start the counts again

1. Press `F2` and `F3` together. Every count, the encoder total and the event count go back to zero. <!-- QA-INP-006 -->

A key that is still down stays lit. `F2` or `F3` alone only light their own cell. Hold `Shift` while you clear to see a key stay lit.

### Leave Input test

1. Press `F4` and `F5` together. The unit goes to Pattern, and the keys work again: `Song` opens Song. <!-- QA-INP-007 -->

`F4` or `F5` alone are counted and do nothing.

## Keys on this screen

| Key | What it does | Ref |
|---|---|---|
| `F2` and `F3` together (**CLEAR**) | Sets every count to zero. A key that is down stays lit. | QA-INP-006 |
| `F4` and `F5` together (**EXIT**) | Leaves for Pattern. | QA-INP-007 |
| Any other key, pad or turn | Is drawn and counted. Nothing else happens. | QA-INP-008 |

## Good to know

- Nothing acts while the screen is up. `Song`, `File`, `Pattern`, `Master`, `Play` and the arrows only count. `Shift` + `Master` does not change the theme, and a pad selects no instrument. `F1`, `F6`, `F7` and `F8` alone do nothing. <!-- QA-INP-008 -->
- The age of an event runs from the scan of the key to the moment the app gets it. The error, drop and latency figures are real only on the unit.
- The screen cannot be reached in the simulator.

## See also

- [The panel](panel.md): the keys held at power-up, and what each opens.
- [Getting started](getting_started.md): the first boot and the screen layout.
