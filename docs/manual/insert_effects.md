<!-- manual-sync: b2a5b4d9 -->
# Insert effects: Fold, Env Filter, Phaser and Comp

Status: draft

New in track3r-os. <!-- og: new --> The official firmware has no effect that sits on one track. The Community OS has **inserts**, and four effect modules you can put in them: **Fold** (a wavefolder), **Env Filter** (an auto-wah), **Phaser** and **Comp** (a compressor that can also shape attacks). Each is a file on the card that you load once and then pick from the INSERTS page of the Mixer. All four were checked on the host by tests; none is checked on the unit yet, so how they sound on the hardware is still open.

## What it is for

Use an insert when one track needs its own treatment: fold a bass into a buzz, make a pluck open and close a filter, sweep a pad, or squeeze a drum bus. The effect hears that track only, before the track goes to the mix and the sends. The Perform effects act on the whole pattern; an insert stays on its track until you remove it. The steps to load a module and fill an insert slot are in [Mixer: put an effect on a track](mixer.md); this page says what each of the four does.

## How it works
<!-- sync: src=modules/effects/fold/README.md,modules/effects/envfilter/README.md,modules/effects/phaser/README.md,modules/effects/comp/README.md spec=docs/specs/OS-INSFX.md qa=QA-GLB-197,QA-GLB-198,QA-GLB-199,QA-GLB-201,QA-GLB-202,QA-GLB-203,QA-GLB-205,QA-GLB-206,QA-GLB-208 -->

An insert is a small effect in the signal path of one track. A track has two slots, **INS A** and **INS B**, and the sound goes through A, then B. Every one of these four has a **Mix** parameter: at 0 % you hear the track untouched, at 100 % only the effect. Pull Mix down to blend the effect in. Everything else is set on the parameter page of the insert.

All four share one rule for loudness: **at their default settings none of them is louder than the track without it.** That is on purpose, so that putting one in does not push the master limiter. The price is that some of them make the track a little quieter. When that happens, raise the effect's output level (Output on Fold, Makeup on Comp). Past full scale the converter clips, so raise it in steps.

A parameter you move does not click or zipper: the effect glides to the new value in a few milliseconds. That means you can turn a knob while the track plays, or bind the parameter to a MIDI controller with Learn (see the Mixer chapter).

### Fold: a wavefolder
Fold turns up the signal and, when it passes a ceiling, bends it back instead of cutting it off. A sine becomes a richer, buzzier tone; a loud sample gets a hollow, metallic edge. A quiet signal passes unchanged, so the effect only speaks when the track is loud enough.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Fold | 0 to 24 dB | 9 | How hard the signal is pushed into the fold. At 9 dB, peaks above about 0.35 of full scale fold back; at 24 dB, above about 0.06. |
| Symmetry | -100 to +100 % | 0 | At 0 the fold is even on both sides and sounds hollow (odd harmonics). Moving it folds one side first, which adds brighter, more nasal harmonics. |
| Tone | 500 to 20000 Hz | 12000 | A low-pass after the fold. Lower it to take off the fizz of a hard fold. At the bottom it dulls the whole effect, not only the top. |
| Mix | 0 to 100 % | 100 | Dry and folded sound. |
| Output | -24 to +6 dB | 0 | Level of the result. A hard fold is quieter than the source, so this is the make-up. |

Example: put Fold on a sine bass, set Fold to 15 dB and Tone to 4000 Hz. The bass keeps its note but gains a growl. Then slowly turn Symmetry up and hear the growl get more nasal.

Good to know: with Fold at 9 dB and a track whose peaks are below -9 dBFS, you hear no change. Fold does not oversample, so at 24 dB on a bright sound some aliasing remains; use Tone to soften it. A view called **Curve** on the parameter page draws how the effect bends the signal.

### Env Filter: a filter that follows the track
Env Filter is an auto-wah. It measures how loud the track is and moves a filter with it: a loud hit opens the filter, and the sound dulls again as the hit fades. It reacts to the track's own level, so it plays along with the pattern.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Mode | LP, BP, HP | LP | Low-pass, band-pass or high-pass. Switching glides (about 4 ms), with no click. |
| Cutoff | 20 to 20000 Hz | 400 | Where the filter rests when the track is quiet. |
| Range | -6 to +6 octaves | +4 | How far a loud hit moves the cutoff from Cutoff. Positive opens up, negative closes down. |
| Resonance | 0 to 100 % | 0 | A peak at the cutoff. At 0 the filter is smooth. |
| Attack | 0.5 to 200 ms | 5 | How fast the filter follows a rising level. |
| Release | 5 to 2000 ms | 150 | How fast it falls back. |
| Sens | 0 to +36 dB | +12 | How loud the track must be to open the filter fully. At +12 dB, a signal at -12 dBFS opens it all the way; a quiet passage stays dull. |
| Mix | 0 to 100 % | 100 | Dry and filtered sound. |

Example: on a drum track, leave the defaults and listen. Each hit opens the filter from 400 Hz toward 6 kHz and it closes in about a quarter of a second. Set Mode to BP and Resonance to 50 % for a vowel-like wah. Set Range to a negative number to make each hit close the filter instead.

Good to know: at the defaults it acts as a low-pass, so a quiet track sounds duller, and that is the effect, not a fault. A high Resonance at a low cutoff rings for seconds after the sound stops, and a Resonance above 0 can be louder than the track. Env Filter hears only the track it sits on. It has no stereo. A **Response** view shows the filter curve at the resting cutoff and at its fully open one.

### Phaser: moving notches
Phaser passes the sound through a chain of all-pass stages and blends it with the original. Where they cancel, you hear moving notches: a slow swirl. A low-frequency oscillator (LFO) moves the notches up and down.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Stages | 4 to 6 | 4 | The number of stages: 4 gives two notches, 6 gives three. A change jumps; it is not smooth. |
| Period | 100 to 20000 ms | 2000 | How long one sweep takes. A period, not a rate: a high number is a slower sweep. |
| Depth | 0 to 100 % | 60 | How wide the sweep is (plus or minus two octaves at 100 %). At 0 the notches stay still at Centre. |
| Centre | 100 to 4000 Hz | 800 | The middle frequency of the sweep. |
| Feedback | -90 to +90 % | 35 | Sends the output back to the input. Positive sharpens and rings the notches; negative sounds hollower. It is limited to 90 %, so it never runs away. |
| Mix | 0 to 100 % | 50 | At 50 % the notches are deepest. At 100 % you hear a pure phase swirl with no notches (unless Feedback is up). |

Example: put Phaser on a pad. The defaults give a gentle swirl through the mids. Set Period to 6000 ms for a slow sweep, raise Feedback to 70 % to make the notches ring, then try Stages 6 for a deeper, more complex swirl.

Good to know: Phaser is mono. At the highest feedback and fastest sweep it stays stable and finite; a loud sound can ring for a second or two.

### Comp: a compressor and a transient shaper
Comp has two modes, set by **Mode**. In **COMP** (0) it turns down a signal that passes a threshold, so loud and quiet parts get closer. In **TRANSIENT** (1) it does not use a threshold: it boosts or cuts the attacks and the tails of a sound, whatever the level. The Mode row shows 0 or 1 for now, not the words. Since version 1.1.0 Comp is stereo-capable (one linked gain on L and R) and has a **Key** row: set it to another track and COMP ducks this track by that track's level.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Threshold | -60 to 0 dB | -18 | COMP only. Where it starts to act, with a soft 6 dB knee centred on it. |
| Ratio | 1 to 20 | 4 | COMP only. How hard it squeezes above the threshold. 1 does nothing. |
| Attack | 0.1 to 100 ms | 10 | COMP: how fast it clamps down. TRANSIENT: how long an onset counts as an attack. |
| Release | 10 to 1000 ms | 120 | COMP: how fast it lets go. TRANSIENT: how long a tail counts as a decay. |
| Makeup | -24 to +24 dB | 0 | Level of the result. Compression makes the track quieter; raise this to bring it back. |
| Mix | 0 to 100 % | 100 | Dry and processed sound. Around 50 % is parallel compression: the punch stays and the compressed body is added. |
| Mode | 0 or 1 | 0 | 0 is COMP, 1 is TRANSIENT. |
| Punch | -100 to +100 % | 0 | TRANSIENT only. Positive makes attacks louder, negative softer. |
| Sustain | -100 to +100 % | 0 | TRANSIENT only. Positive makes tails louder, negative shortens them. |
| Key | OFF, T1..T16 | OFF | The sidechain key: in COMP mode the detector follows that track's level (its block peak, after its own inserts and before its fader) instead of this track's, so a kick on T1 ducks this track while it plays. TRANSIENT ignores it. |

Example, compress: on a snare, leave the defaults and lower Threshold to -30 dB; the loud hits come down and Makeup brings the level back. Example, shape: set Mode to 1 and Punch to +60 %; a soft drum gets a sharper click, at any volume. Set Sustain to -60 % to dry out a ringing sound. Example, duck: put Comp on a pad track, set Key to the kick's track; the pad dips every time the kick hits and comes back through Release.

Good to know: at the defaults COMP never raises the level, so a track at Makeup 0 gets quieter, not louder. TRANSIENT with Punch and Sustain at 0 changes nothing, bit for bit. Its gain is limited to about 12 dB, and Makeup +24 dB can push a track over full scale. A Key naming the insert's own track does nothing (the unit does not measure a track for its own insert). While a track has no voice at all its insert is not called, so the first note after a gap can start at the old, ducked gain and recovers over about the Release time.

## Try it
<!-- sync: src=modules/effects/envfilter/README.md,docs/qa/catalogue/global.md qa=QA-GLB-205,QA-GLB-208 -->

Start with the module file `envfilter.tmod` in the `modules/effects` folder of the card, a project with a drum track on track 1, and the transport playing.

1. Open **Master**, press `F6` (**INSERTS**), move the cursor to track 1, **INS A**, and press `Insert`.
2. Pick **envfilter** in the list and press `F1`. The strip says **INSERT SET**.
3. Listen. Each drum hit opens a dull filter and it closes after it. Press `F7` (**BYPASS**) to hear the track without it, and again to turn it on.
4. Open the insert's parameter page with `Params`. Raise Resonance to 40 % and Range to +6.

You should hear a sharper, more vocal sweep on every hit. When you stop the transport, the effect goes quiet after a moment (a high Resonance at a low cutoff takes longer).

## More tasks

### Swap an effect for another
Press `Delete` on its cell, then `Insert` to pick the other one. The new insert starts from its default settings.

### Tame a hot result
Lower Fold, Resonance or Feedback first, then raise Output or Makeup in small steps. If the master limiter lights, lower the track's level in the MIXER.

## Good to know

- Put at most the effects you need: each insert uses part of the unit's processing time, and the amounts are not measured on the unit yet.
- Each effect hears only the track it sits on.
- Not checked on the unit: how any of the four sounds on the hardware, and how much processing each takes.

## See also

- [Mixer](mixer.md): the INSERTS page, bypass and Learn.
- [Modules](modules.md): loading modules from the card.
- [Perform](perform.md): effects for the whole pattern, played live.
