# Comp (module `comp`)

A compressor with a TRANSIENT mode, with a dry/wet mix and a make-up gain, for a **track insert** of the Community OS (OS-INSFX-FX1, module 3 of the first effect pack; `docs/roadmap/BACKLOG.md`), loaded from the SD card as
`/modules/effects/comp.tmod` and bound with `diag ins bind <track> <slot> comp` after `mod load` (`docs/specs/OS-INSFX.md`; the Mixer's INSERTS page and the parameter page come with S4 and S5).
Since 1.1.0 (OS-INSFX-FX2 X6, ABI 1.12) it is **stereo-capable** (one linked gain on the peak of L and R) and has a **sidechain Key** (OFF, T1..T16): in COMP mode the detector reads the key track's block peak instead of its own input, and the file ships four factory presets.
It is **not** built into the image. Status: new Community OS behaviour; tested on the host through the ABI against a double-precision reference (`tests/comp_test.cpp`, ctest `o3m_comp_test`); **its cost and its sound are not
verified on the unit** (`platform/mk66/tools/hil/qa_comp.json`, QA-GLB-193..196).

Licence: GPL-3.0-or-later like the OS (the source includes the MIT SDK header `o3module.h`). **Source of the maths:** none: the peak followers, the soft-knee curve, the two-follower transient law and the polynomials for log2 and
exp2 are this module's own, written for it; no code of any note or library is used (`THIRD_PARTY.md`).

## Parameters (in this order for ever: a stored parameter blob is one little-endian float32 per parameter; a 1.0.0 blob of nine floats leaves the Key at its default OFF)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Threshold | -60..0 dB | -18 | modulation target; COMP only. The soft knee (6 dB wide) is centred on it: nothing happens 3 dB below it. Listed first so that the unit bench's moving-parameter row (the first MOD target) sweeps it |
| 1 | Ratio | 1..20, log | 4 | modulation target; COMP only. 1 is no compression (the input bit for bit) |
| 2 | Attack | 0.1..100 ms, log | 10 | modulation target. COMP: the detector's attack. TRANSIENT: the slow follower's attack (how long an onset lasts); the fast one is Attack / 8, at least 0.1 ms |
| 3 | Release | 10..1000 ms, log | 120 | modulation target. COMP: the detector's release. TRANSIENT: the slow follower's release; the fast one is Release / 3 |
| 4 | Makeup | -24..+24 dB | 0 | modulation target; the output level 10^(dB/20), folded into the gain. 0 at the defaults, so the effect never raises the level |
| 5 | Mix | 0..100 % | 100 | modulation target; parallel compression: out = (1 - m) x + m wet; 0 is the input bit for bit (at Makeup 0 dB) |
| 6 | Mode | 0 or 1 | 0 | 0 COMP, 1 TRANSIENT (an integer switch, not a modulation target; the descriptor has no value labels for an effect yet, so a page shows 0 and 1) |
| 7 | Punch | -100..+100 % | 0 | modulation target; TRANSIENT only: the onsets' gain, 2^(Punch d) for d over 0 (d is log2 of fast over slow) |
| 8 | Sustain | -100..+100 % | 0 | modulation target; TRANSIENT only: the decays' gain, 2^(-Sustain d) for d under 0 |
| 9 | Key | 0..16 | 0 (OFF) | the sidechain key (ABI 1.12): OFF, or T1..T16 — in COMP mode the detector follows that track's block peak (`host->track_level`) instead of this track's input; TRANSIENT ignores it. An enumeration the page shows as OFF, T1..T16; never a Learn target, not a modulation target |

All of them but the Key will be learnable with MIDI Learn (S7): the Key carries `O3M_PARAM_KEY`, which the OS honours from ABI 1.12 — it is shown as OFF, T1..T16 and no controller is ever bound to it. `set_param` does one division (Ratio) or two (Attack, Release: the fast and the slow follower's coefficients) and a few multiplies, nothing else, so a modulation source may call it every block. A change of Mode re-seeds the followers from the next input sample.

## How it works

Both modes are gain-only: the sample loop follows the input's envelope and multiplies the input by a gain that ramps in a straight line across each control step of 16 samples (counted across blocks, so any block size gives the same bits for
static parameters). The mix is parallel compression for free: with no latency in the wet path, `(1 - m) x + m g x = x (1 - m + m g)`, so the step's target is `G = Makeup (1 - m + m g)`.

- **COMP:** a feed-forward compressor with a peak follower (a one-pole attack and release on `|x|`). At each step, in log2 units of the follower: `over = L - T`, gain reduction `gr = 0` under -1/2, `s over` above +1/2 and `s (over + 1/2)^2 / 2` between
  (a 1 log2 = 6 dB soft knee, continuous in value and slope), `s = 1 - 1/Ratio`, `g = 2^-gr`. No look-ahead: the gain follows the follower with a step's delay (16 samples, 0.36 ms), so the fastest attacks overshoot for that long (ramped, never a click).
- **COMP keyed (Key set, OFF is the 1.0.0 arithmetic bit for bit):** the detector input is not this track's input but the key track's level, one number a block — the linear peak of the key track's block after its inserts and before its fader (`host->track_level`, 0 for a silent or unnamed track, at most one block = 1.45 ms late): the follower is stepped once a block through the Attack (the level rose) or the Release (it fell), the reduction comes from it as usual, and the gain ramps in a straight line across the block. A silent key reads 0, so the reduction is 0, the gain is 1 and the output is the input bit for bit. TRANSIENT never reads the key.
- **TRANSIENT:** two followers of `|x|`, a fast one and a slow one; `d = log2(fast + f) - log2(slow + f)` (f = 3e-4, about -70 dBFS, so the noise floor is left alone) is positive on an onset and negative on a decay. The gain is `2^(Punch d)` for `d >= 0` and
  `2^(-Sustain d)` for `d < 0`, with Punch and Sustain as factors -1..+1, limited to +-12 dB. It is level independent (a ratio of followers). At Punch 0 and Sustain 0 (the defaults) the gain is exactly 1: TRANSIENT is the input bit for bit until you move one.
  Measured on the host: on a decaying noise burst Punch +100 % lifts the first 8 ms by about 11 dB and leaves the decay alone, Sustain +100 % lifts the decay by about 7 dB and leaves the onset alone; a steady tone at Punch and Sustain +100 % moves by under 1 dB
  (0.4 to 0.5 dB measured at 60 to 3000 Hz: the followers' ripple counts on both sides). Threshold and Ratio are not used in this mode.
- **Level law (the lesson of DRIVE, which came out +12 to +16 dB over bypass at its defaults):** the gain of COMP is never above 1 and the default Makeup is 0 dB, so at the defaults (Threshold -18, Ratio 4, Makeup 0, Mix 100) the output is never above the input sample by sample,
  and its rms never above the input's: the test scans sines (100, 440, 1234.5 and 3000 Hz) and noise from -60 to -6 dBFS, in both modes. A signal whose peak is under -21 dBFS (under the knee) passes bit for bit. It is a compressor, so a loud signal comes out quieter
  (a -6 dBFS peak sine by about 8 dB at the defaults); **Makeup is the make-up gain**: raise it to bring the level back, and from there the module can be louder than bypass, by your setting (Makeup +24 dB or TRANSIENT's +12 dB can exceed full scale: the host's Q15 conversion clips, as for any insert).
- **Mix and Makeup:** once a block (1..128 frames) the smoothed mix and make-up (dB) move k = frames / 256 of the way to their targets (about 6 ms at 64 frames) and the gain ramps across each step: no zipper. The make-up level is the module's own `exp2` polynomial.
- **Own log and exp:** `log2` and `exp2` are short polynomials (absolute error under 3e-6 and relative under 1e-7), the one-pole coefficients a 5-term series: no host call and no division in `render()`. They run once per 16 samples. A gain step that moves the gain by less than 3e-5 does not ramp (it holds and lands on the target): a ramp of rounding-sized slope could overshoot a target of 1 by an ulp, and the defaults promise no sample above its input.
- Tail (the OS-INSFX S2 contract): `render` returns 1 while the input is live (its last sample is not 0) **or a follower is still above -80 dBFS**, and 0 when both are silent; at 0 the followers are flushed to exactly 0 and the gain to neutral, so the first note after an
  idle stretch is treated as by a fresh instance (a stale follower would duck it). The tail runs while a follower falls to -80 dBFS: about 90 ms at Release 10 ms, about 9 s at Release 1000 ms from full scale (the host then keeps running the insert, at its idle-less cost of about 0.6 %).
  A NaN, an infinity or an absurd input (over 1e18) is not confined to its sample: it poisons the follower for the rest of the block, so in COMP the gain falls to -96 dB (ramped) until the end-of-block flush (in TRANSIENT the gain stays 1); the next block is clean, and the sample itself comes out as NaN or infinity.
- One view, **Curve**: COMP draws the transfer law (input level -60..0 dB against output level, with the 1:1 line); TRANSIENT draws the gain law (gain +-12 dB against d, Sustain on the left of the centre and Punch on the right). The mode's name is in the corner.
- Known limits: **no look-ahead** (so no latency to report), a **peak** detector (not RMS: a sustained low note rides the gain a little), a **fixed knee** (6 dB), the unit's console has no insert parameter verb, so TRANSIENT can be heard only once the parameter page (S5) exists.
- **Stereo-capable** (ABI 1.12, `render_st`): on a block whose input has a side signal one linked gain is applied to both planes, computed from the peak of L and R, so the image does not shift under compression; on L = R the stereo render is the mono render bit for bit. The master insert is dual mono: its L and R still get two independent instances.
- A **Key naming the effect's own track reads OFF in the host**: the host never measures a track for its own insert (no tap, no mask bit), so `track_level` answers 0 and COMP does no compression. No code fix is possible — the module cannot see its own track.
- **e0 and G freeze while the track has no voice**: the OS never calls an insert on a track whose strip is not running at all, so no silent block reaches the module and the idle reset (the one a rendered silent block gets) cannot run; the first note after such a gap starts at the old, ducked gain and recovers over about the Release time.

## Cost

Static (packed file, from the mk66 build, the o3m_pack line of the CI build): comp.tmod 5,712 B, hot 2,000 B, cold 3,488 B, state 120 B, core flash unchanged (measured on the CI's mk66-targeted pack; the cap of the host test is 2,048 B, the SRAM_L slab of 14 KiB being
shared by every loaded module: DRIVE's hot part is 1,328 B, CRUSH's 720 B). `render_st` is the only hot body — `render` is a cold thunk that runs it on L = R, bit for bit the mono render (the tests pin it) — and `log2` and `exp2` are two hot out-of-line helpers (once or twice a gain step: 8 calls a block in COMP, 12 in TRANSIENT, none inside a sample loop). The keyed block is one out-of-line cold function both shapes call.
Estimates [est, not measured on the unit]: COMP about 1.6k cycles a block (a sample loop of 13 instructions with a dependent chain of about 12 cycles, plus four gain steps of about 110 cycles with the two helper calls), TRANSIENT about 2.1k (two interleaved followers, two logs a step):
0.6 % and 0.8 % of a 64-frame block of 261,225 cycles, the biquad's measured 1,666 for comparison, and well under the 3 points (7.8k cycles) of a track insert. Measure with `bench insert 64` on a bench image (rows `insert_effect` and `insert_effect_mod`), see `docs/architecture/cpu-budget.md`.

## Build and test

    cmake --build build-os-host --target o3m_comp_test o3m_comp_tmod
    ctest --test-dir build-os-host -R comp --output-on-failure
