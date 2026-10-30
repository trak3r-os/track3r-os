<!-- manual-sync: c3ee4c54 -->
<!--
Chapter: TB-303, FM and Chip, the new parameters (OS-SNDMOD S6). Rules: .claude/skills/manual-writing/SKILL.md. Sources: docs/qa/catalogue/modules.md (QA-MOD-140..156), modules/tb303|fm|chip/README.md and module.json, docs/specs/OS-SNDMOD.md.
Only the parameters this version adds are written here. Modules are not in the simulator yet (E2E-7), so there are no screenshots and the walk-through has no scenario.
-->
# TB-303, FM and Chip: new parameters

Status: draft

The TB-303 has a second page, **MODS**, with a tuning, an octave shift, an accent threshold, filter tracking and an accent decay. The FM has sixteen algorithms instead of eight, and a Detune that reaches a whole octave either way. The Chip's wave and duty can now follow a MIDI controller. SID is a new sound in the same family: a chip voice with its own filter on every track. All of this is new in track3r-os. A project saved earlier sounds the same, because every new value starts where the old sound is.

## What it is for

Come here when a built-in sound needs more than its first page gives. Examples are a bass line that must match the tuning of a song, accents on only the hardest notes, or an FM bell that is not a whole-number multiple of the note. You reach everything on the Instrument screen of a module instrument. To put a module in a slot, see [Modules](modules.md).

## How it works
<!-- sync: src=modules/tb303/module.json,modules/fm/module.json,modules/chip/module.json qa=QA-MOD-144 -->

### Old projects keep their sound
<!-- sync: src=modules/tb303/module.json,modules/fm/module.json qa=QA-MOD-144 -->

A project stores the values of a module in a fixed order. The new values come after the old ones. If a project holds fewer values than the module has now, the module keeps its own starting values for the rest. Those starting values leave the old sound unchanged. There is no tuning, no transposition and no tracking. An accent starts at velocity 100 and its decay is 200 ms. When you change a new value, it is saved with the project from then on. <!-- QA-MOD-144 -->

### The TB-303's MODS page
<!-- sync: src=modules/tb303/module.json,modules/tb303/tb303_dsp.h qa=QA-MOD-144,QA-MOD-145 -->

Press `F2` (**MODS**) on the Instrument screen of a TB-303. The page starts with **LEGATO SLIDE**, which was on the second page before. The page then lists five new values: **TUNE**, **TRANSPOSE**, **ACC THRESH**, **FILTER TRACK** and **ACC DECAY**. The first page, **SOUND**, now holds eight rows, from **WAVEFORM** to **VOLUME**. <!-- QA-MOD-144 -->

**TUNE** moves the pitch of the next note by up to 100 cents (a cent is a hundredth of a semitone). **TRANSPOSE** moves it by up to 24 semitones. A note that is already sounding keeps its pitch until the next note starts. With both at 0 the pitch is the old one. <!-- QA-MOD-145 -->

### Accents: ACC THRESH and ACC DECAY
<!-- sync: src=modules/tb303/module.json,modules/tb303/tb303_dsp.h qa=QA-MOD-146,QA-MOD-148 -->

On a 303 an accent is a note played harder than the rest. Here an accented note is louder and opens the filter further, by the amount of the **ACCENT** value on the **SOUND** page. The filter then falls back on its own time. A note is an accent when its velocity is **ACC THRESH** or more. The start value is 100. <!-- QA-MOD-146 -->

A step's volume sets the velocity, up to 127. A step with no volume plays at velocity 96, so at the start value it is plain. Set **ACC THRESH** to 90 and every note from 90 up is an accent. Set it to 127 and only the hardest notes are. <!-- QA-MOD-146 -->

**ACC DECAY** is the time that filter fall takes on an accented note, 30 to 2000 ms. The start value, 200 ms, is the time the TB-303 always had. A plain note keeps the time of **DECAY**. Both values act on the next note. <!-- QA-MOD-148 -->

### Filter tracking
<!-- sync: src=modules/tb303/module.json,modules/tb303/tb303_dsp.h qa=QA-MOD-147 -->

On the **SOUND** page, **CUTOFF** is one fixed frequency for every note. With **FILTER TRACK** above 0 %, the cutoff follows the pitch of the note instead: high notes open the filter, low notes close it. At 100 % it follows by one octave for every octave of the note, measured from MIDI note 48 (the C below middle C). At 50 % it follows by half as much. <!-- QA-MOD-147 -->

The factor is set when a note starts, and a note that is already sounding keeps its filter. During a slide the factor moves with the pitch, so the filter glides to the new note with it. At 0 % the cutoff is the knob, as before. <!-- QA-MOD-147 -->

This is new in track3r-os: the reference note is a choice of this OS, not a measurement of any other 303.

### FM: sixteen algorithms and a wide Detune
<!-- sync: src=modules/fm/module.json,modules/fm/fm_dsp.h qa=QA-MOD-143,QA-MOD-149,QA-MOD-150 -->

The FM has four operators, which are four sine waves. A modulator is an operator that bends the pitch of another one. A carrier is an operator you hear. The algorithm says which operators modulate which. `4>3>2>1` means 4 modulates 3, 3 modulates 2 and 2 modulates 1, and only 1 is heard. See the [glossary](glossary.md).

**ALGORITHM** on the **OP1** page now runs from 1 to 16. The first eight are unchanged, so a saved project plays the same algorithm. Algorithms 9 to 16 add new shapes. One has three modulators feeding one carrier (`4+3+2>1`). One has two modulators feeding two carriers (`(4+3)>(2+1)`). One mixes a chain with a free carrier (`4>2>1 + 3`). The Algorithm view draws each one. The value and the view's title spell it the same way, for example **ALG 14  (4+3)>(2+1)**. <!-- QA-MOD-143 QA-MOD-149 -->

**DETUNE** on each operator page now reaches 1200 cents up or down, one octave each way. It was 100 cents. A detune of 600 cents puts a modulator at about 1.41 times its note. Whole-number ratios cannot reach that, and it is the colour of a bell. Hold `Shift` and turn the encoder to move in steps of 10 cents. A MIDI Learn binding you saved on a Detune row now sweeps the wider range (24 times the old one), and Relative mode counts more detents. The values you stored sound the same. <!-- QA-MOD-150 -->

### Chip: wave and duty from a controller
<!-- sync: src=modules/chip/module.json qa=QA-MOD-141 -->

**WAVE** and **DUTY** can now be bound to a MIDI controller with Learn (`Vol` and `Fx` together, see [The panel](panel.md)). Turning the controller steps through the four waves or the four duties. Before, these two rows refused Learn. <!-- QA-MOD-141 -->

### SID: a chip voice with four pages
<!-- sync: src=modules/sid/module.json,modules/sid/sid_dsp.h qa=QA-MOD-210,QA-MOD-216,QA-MOD-217,QA-MOD-218 -->

SID is a voice in the style of the Commodore 64's sound chip. It is an imitation, not a copy of the chip. Pick it with `F1` (**ENGINE**) on the Instrument screen. It plays one note at a time, so play chords on several tracks. <!-- QA-MOD-210 -->

It has four pages, one per `F` key under the rows:

| Page | What is on it |
|---|---|
| **OSC** | the wave (saw, triangle, pulse, noise and four combined waves), the pulse width and its slow movement (**PWM DEPTH**, **PWM RATE**), **SYNC**, **RING**, **MOD PITCH**, **MOD FINE** |
| **ENV** | **ATTACK**, **DECAY**, **SUSTAIN**, **RELEASE** (sixteen steps each, the label shows the time), volume, velocity, transpose and fine tune |
| **FILTER** | **MODE**, **CUTOFF**, **RESONANCE**, **KEY TRACK**, **ENV AMOUNT**, **MODEL**, **DRIVE** |
| **PITCH** | **SWEEP**, vibrato and a three-note arpeggio |

<!-- QA-MOD-218 -->
The three views under the rows show the wave, the envelope and the filter's response.

### SID: sweep the sync, the pitch stays

**SYNC** restarts the main oscillator every time a second oscillator completes a cycle. The second oscillator always plays the key you press, so the pitch you hear is the key's. **MOD PITCH** sets how far above or below the key the main oscillator runs. Hold a note and turn **MOD PITCH** from 0 to +24: the pitch does not move and the tone sweeps from mellow to sharp and buzzy. That is the classic sync lead. A modulation source on **MOD PITCH** does the same sweep for you. <!-- QA-MOD-216 -->

With **SYNC** off, **MOD PITCH** sets the pitch of the second oscillator for **RING**. Ring makes a triangle sound like a bell, and it only affects the triangle. <!-- QA-MOD-217 -->

### SID: the filter, one on every track

The filter mixes a low pass, a band pass and a high pass. **MODE** picks the mix: **OFF**, **LP**, **BP**, **HP**, **NOTCH**, **LP+BP**, **BP+HP** or **ALL**. With **OFF** the filter is skipped and costs nothing. <!-- QA-MOD-226 -->

- **CUTOFF** is where the filter works, from dull to open. **KEY TRACK** moves it with the key (100 % keeps the same tone on every note). **ENV AMOUNT** opens it with the envelope, or closes it when it is negative. <!-- QA-MOD-227 QA-MOD-229 QA-MOD-230 -->
- **RESONANCE** (0 to 15) adds a peak at the cutoff. At 15 it whistles, and the level stays under control. <!-- QA-MOD-228 -->
- **MODEL** picks 6581 or 8580. The 6581 has a rounder curve, saturates earlier and sounds grittier. The 8580 is cleaner and brighter. **DRIVE** pushes the sound into the saturation before the filter. <!-- QA-MOD-231 QA-MOD-232 -->

The real chip has one filter shared by its three voices. Here every track has its own filter. After you release a key, a resonant filter rings on for a moment. <!-- QA-MOD-234 -->

Not yet checked on the unit: everything in this chapter.

## Try it
<!-- sync: src=modules/tb303/module.json,modules/tb303/tb303_dsp.h qa=QA-MOD-145,QA-MOD-146,QA-MOD-147,QA-MOD-148 -->

This walk-through uses a TB-303 in an instrument slot and a pattern with a short bass line over two octaves. A few steps have a volume of 95.

1. Press `Play` to hear the line as it is. Press `Params`, then `F2` (**MODS**). Press `Down` until the cursor is on **TRANSPOSE** and turn the encoder to 12. The next notes sound an octave higher.
2. Put **TRANSPOSE** back to 0. Move the cursor to **ACC THRESH** and turn the encoder to 90. The steps with volume 95 now sound as accents.
3. Move the cursor to **ACC DECAY**. Set 30 ms for a short blip, then 800 ms for a long sweep.
4. Press `F1` (**SOUND**), move the cursor to **RESONANCE** and set 70.
5. Press `F2`, move the cursor to **FILTER TRACK** and set 100 %.

You should hear the high notes open up and the low notes close in.

## Parameters at a glance

| Module | Value | Range | Starts at | Ref |
|---|---|---|---|---|
| TB-303 | **TUNE** | -100 to 100 cents | 0 | QA-MOD-145 |
| TB-303 | **TRANSPOSE** | -24 to 24 semitones | 0 | QA-MOD-145 |
| TB-303 | **ACC THRESH** | 1 to 127 | 100 | QA-MOD-146 |
| TB-303 | **FILTER TRACK** | 0 to 100 % | 0 | QA-MOD-147 |
| TB-303 | **ACC DECAY** | 30 to 2000 ms | 200 | QA-MOD-148 |
| FM | **ALGORITHM** | 1 to 16 (was 1 to 8) | 5 | QA-MOD-149 |
| FM | **DETUNE** (each operator) | -1200 to 1200 cents (was -100 to 100) | 0, 0, 4, 0 | QA-MOD-150 |
| Chip | **WAVE**, **DUTY** | can be learned | as before | QA-MOD-141 |
| SID | **MODE** | OFF, LP, BP, HP, NOTCH, LP+BP, BP+HP, ALL | OFF | QA-MOD-226 |
| SID | **CUTOFF** | 0 to 100 % | 50 | QA-MOD-227 |
| SID | **RESONANCE** | 0 to 15 | 0 | QA-MOD-228 |
| SID | **KEY TRACK** | 0 to 100 % | 0 | QA-MOD-229 |
| SID | **ENV AMOUNT** | -100 to 100 % | 0 | QA-MOD-230 |
| SID | **MODEL**, **DRIVE** | 6581 or 8580, 0 to 100 % | 6581, 0 | QA-MOD-231 |

## Good to know

- The five TB-303 values act on the next note, not on a note that is sounding. **ACC DECAY** acts on the next accented note. <!-- QA-MOD-145 QA-MOD-148 -->
- Transposing a note past the top or the bottom of the MIDI range holds it at the end of the range. <!-- QA-MOD-145 -->
- None of the new TB-303 values can be driven by the modulation matrix. <!-- QA-MOD-144 -->
- SID's **CUTOFF** can be driven by the modulation matrix, so a controller or an LFO can sweep the filter. The other filter values cannot. <!-- QA-MOD-227 -->
- **MOD PITCH** never changes the pitch you hear. With **SYNC** on it changes the tone, with **SYNC** off it matters only for **RING**. <!-- QA-MOD-216 QA-MOD-217 -->
- A MIDI controller bound to **DETUNE** sweeps the whole 2400 cents. Hold `Shift` while turning the encoder for steps of 10 cents. <!-- QA-MOD-150 -->

## See also

- [Modules](modules.md): putting a module in an instrument slot, and finding its pages.
- [Step FX on module instruments](module-instrument-fx.md): which effects a module instrument obeys.
- [The panel](panel.md): where `Vol`, `Fx` and `Shift` are.
