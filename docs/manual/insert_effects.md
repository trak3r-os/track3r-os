<!-- manual-sync: b2a5b4d9 -->
# Insert effects: eleven effects for one track

Status: draft

New in track3r-os. <!-- og: new --> The official firmware has no effect that sits on one track. The Community OS has **inserts**, and eleven effect modules you can put in them: **Drive** (saturation), **Crush** (bit and sample-rate reduction), **Biquad** (a filter), **Fold** (a wavefolder), **Env Filter** (an auto-wah), **Phaser**, **Comp** (a compressor that can also shape attacks and duck from another track), **Ping-Pong** (a tempo-synced stereo echo), **Wide** (a stereo widener), **Chorus** and **Autopan**. Each is a file on the card that you load once and then pick from the INSERTS page of the Mixer.

## What it is for

Use an insert when one track needs its own treatment: fold a bass into a buzz, make a pluck open and close a filter, sweep a pad, or squeeze a drum bus. The effect hears that track only, before the track goes to the mix and the sends. The Perform effects act on the whole pattern; an insert stays on its track until you remove it. The steps to load a module and fill an insert slot are in [Mixer: put an effect on a track](mixer.md); this page says what each of the eleven does.

## How it works
<!-- sync: src=modules/effects/fold/README.md,modules/effects/envfilter/README.md,modules/effects/phaser/README.md,modules/effects/comp/README.md spec=docs/specs/OS-INSFX.md,modules/effects/drive/README.md,modules/effects/crush/README.md,modules/effects/biquad/README.md,modules/effects/pingpong/README.md,modules/effects/wide/README.md,modules/effects/chorus/README.md,modules/effects/autopan/README.md qa=QA-GLB-197,QA-GLB-198,QA-GLB-199,QA-GLB-201,QA-GLB-202,QA-GLB-203,QA-GLB-205,QA-GLB-206,QA-GLB-208 -->

An insert is a small effect in the signal path of one track. A track has two slots, **INS A** and **INS B**, and the sound goes through A, then B. Almost every one has a **Mix** parameter: at 0 % you hear the track untouched, at 100 % only the effect. Pull Mix down to blend the effect in. Everything else is set on the parameter page of the insert.

Fold, Env Filter, Phaser and Comp share one rule for loudness: **at their default settings none of them is louder than the track without it.** (The echo, the widener, the chorus and the autopan add something, so they follow their own Mix or Width.) That is on purpose, so that putting one in does not push the master limiter. The price is that some of them make the track a little quieter. When that happens, raise the effect's output level (Output on Fold, Makeup on Comp). Past full scale the converter clips, so raise it in steps.

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

### Drive: saturation
Drive pushes the signal into a soft clipper. A little warms a sound; a lot gives a thick, crunchy distortion.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Shape | 0 or 1 | 0 | 0 is SOFT (even on both sides). 1 is ASYM (one side clips harder, which adds even harmonics). The row shows the number; the view names it. |
| Drive | 0 to 36 dB | 12 | How hard the signal hits the clipper. At 0 dB a quiet signal passes unchanged. |
| Tone | 500 to 20000 Hz | 8000 | A low-pass after the clipper. Lower it to soften the fizz. |
| Mix | 0 to 100 % | 100 | Dry and driven sound. |
| Output | -24 to +6 dB | 0 | Level of the result. |

Good to know: Drive does not oversample, so a high setting on a bright sound aliases. Use Tone to soften it. A **Curve** view draws the clipping curve.

### Crush: bits and sample rate
Crush makes the sound coarser, like an early sampler. **Bits** cuts the number of levels; **Rate** holds each value for several samples.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Rate | 1 to 64 | 1 | How many samples each value is held. 1 holds nothing. It can be a fraction. |
| Bits | 1 to 16 | 8 | The depth. 16 changes nothing; 8 gives 256 levels; 1 leaves only three. |
| Mix | 0 to 100 % | 100 | Dry and crushed sound. |
| Output | -24 to +6 dB | 0 | Level of the result. Low Bits lose level, so this is the make-up. |

Good to know: silence stays silence. A view shows the staircase the Bits value makes.

### Biquad: a plain filter
Biquad is a resonant filter with a Mix. It sits at a fixed cutoff and does not follow the track.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Mode | 0 to 3 | 0 | 0 low-pass, 1 high-pass, 2 band-pass, 3 notch. A change crossfades in about 25 ms. |
| Cutoff | 20 to 20000 Hz | 1000 | The filter's corner or centre. |
| Resonance | 0 to 100 % | 10 | A peak at the cutoff. At 10 % it is smooth; at 100 % it is sharp and can clip. |
| Mix | 0 to 100 % | 100 | Dry and filtered sound. |

Example: Mode 1 (high-pass) at 200 Hz takes the mud out of a pad. A **Response** view shows the curve.

### Ping-Pong: a tempo-synced echo
Ping-Pong repeats the sound and bounces the echoes between left and right. It follows the song's tempo. It is a stereo effect that **widens**.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Sync | FREE, or a length from 8 steps down to 1/8 step | 3 steps | The echo time as a length in sequencer steps (a beat is 4 steps), so the echo stays on the grid at any BPM. FREE uses Time instead. |
| Time | 10 to 1000 ms | 375 | The echo time while Sync is FREE. |
| Feedback | 0 to 95 % | 45 | How much of the echo feeds back. It cannot run away. |
| Low cut | 20 to 2000 Hz | 120 | Keeps bass out of the repeats. |
| High cut | 1000 to 20000 Hz | 6000 | Darkens each repeat. |
| Mix | 0 to 100 % | 35 | How much echo you hear. |
| Width | 0 to 100 % | 100 | 100 sends echoes fully to alternate ears; 0 puts them in the middle. |

Presets: 1/8 PING, DOTTED 8, SLAP, DUB, WIDE 16TH. Good to know: a time over one second is halved until it fits. Each instance holds a long delay line in module memory, so a fourth may be refused when other modules are loaded.

### Wide: a stereo widener
Wide gives a mono sound a stereo image. It adds a short, high-passed copy to the side only, so the mono sum stays the original. It **widens**.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Width | 0 to 100 % | 50 | How much side is added. 0 changes nothing. |
| Time | 2 to 30 ms | 12 | The delay of the copy. Longer sounds wider and more spaced. |
| Low cut | 20 to 1000 Hz | 200 | Keeps the bass in the centre. |

Presets: Subtle, Wide, Huge, Doubler.

### Chorus: a stereo chorus
Chorus mixes the sound with a slowly moving short delay, one for each ear. A mono sound becomes wide and shimmering. It **widens**.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Period | 200 to 20000 ms | 1667 | How long one sweep takes. A higher number is slower. |
| Depth | 0 to 100 % | 40 | How far the delay moves. |
| Delay | 5 to 25 ms | 12 | The middle delay. |
| Mix | 0 to 100 % | 50 | At 0 the sound is unchanged. At 100 you hear only the moving copy (a vibrato). |

Presets: Classic, Slow Wide, Vibrato, Ensemble.

### Autopan: a pan that swings
Autopan swings the sound between left and right. It follows the tempo while the song plays. It **widens**.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Sync | 0 to 15 | 3 | The cycle as a length in steps: 3 is one bar (16 steps), 7 is a beat, 11 is one step. 0 is FREE, and Period rules. |
| Period | 100 to 20000 ms | 1000 | The cycle length while Sync is 0. |
| Depth | 0 to 100 % | 70 | How far it swings. |
| Shape | 0 to 2 | 0 | 0 sine, 1 triangle, 2 square (smoothed, so it does not click). |
| Phase | 0 to 360 | 0 | Shifts where the swing starts. |
| Headphone | 0 or 1 | 0 | 0 is for speakers and keeps the mono sum. 1 adds a delay cue for headphones and is not mono-compatible. |

Presets: Slow Sine, Tri 8th, Square 16th, Gentle.

### Presets and the CPU guard
Comp (DUCK T1, PUMP T1, GLUE, PUNCH), Ping-Pong, Wide, Chorus and Autopan come with factory presets. Open the insert's page, press `F5` (**PRESET**), pick one and press `F1` (**LOAD**). The steps, and how to save your own to the card, are in [Mixer: load a preset into an insert](mixer.md).

A project holds up to four inserts at once, and a master insert counts as two. Each running effect that **widens** (Ping-Pong, Wide, Chorus, Autopan) keeps one of the four stereo voices; Comp is stereo-capable and keeps none. If an insert would take too much processing time, the unit forces it to dry and the cell says **OVER BUDGET**. See [Mixer](mixer.md).

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

- Put at most the effects you need: each insert uses part of the unit's processing time.
- Each effect hears only the track it sits on.

## See also

- [Mixer](mixer.md): the INSERTS page, bypass and Learn.
- [Modules](modules.md): loading modules from the card.
- [Perform](perform.md): effects for the whole pattern, played live.
