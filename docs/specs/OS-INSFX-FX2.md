# OS-INSFX-FX2 — Stereo and sidechain effect pack: WIDE, STEREO CHORUS, PING-PONG, AUTOPAN, COMP with a KEY track

Status: specified 2026-11-05, decisions taken the same day (every recommended default), not implemented · **Level B** per module (a `.tmod` on the card: no OS code, no format, no ABI change; module and DSP changes take the `tools/ci.sh qemu` gate before merge) · Backlog row OS-INSFX-FX2, approved by the maintainer 2026-11-05 · Facts from `dev` at c1dc3125 and the open branch `feat/OS-INSFX-FX1-drive` (9ead14f3, the first FX1 module: the per-module process) · Order: after OS-INSFX-ABI2 (A1, A2 and A4 for every module; A3 for PING-PONG and AUTOPAN; A6 for COMP); COMP also after OS-INSFX-FX1's COMP · Every behaviour here is **new Community OS behaviour**.

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 b, Q2 a, Q3 b, Q4 b (a WIDENS insert may take 4 points of a block, the stereo adapter included; COMP 3), Q5 a (every recommended default).**

Only what the maintainer must decide (scope, a sound the user hears, a CPU budget above the rule). Parameters, ranges, sync lists, preset values and the DSP recipes are decided in this spec (section 6).

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | AUTOPAN's headphone mode (the row's question) | a) none · b) a `MODE` switch SPEAKERS / HEADPHONE in AUTOPAN, SPEAKERS by default, its own slice: the far ear delayed by up to 0.6 ms (a fractional delay) and darkened by a one-pole head-shadow low-pass that follows the pan · c) a separate binaural module later | b | About 1k cycles a block more only when on [est]; a cheap, convincing "around the head" pan on headphones; off by default because it is not mono-compatible (the delayed ear combs in a mono sum); a full HRTF convolution is far over the budget (row) |
| Q2 | COMP with a KEY track | a) extend FX1's COMP: version 1.1.0, ABI 1.11, a `Key` parameter appended (a 1.0.0 blob still loads, the key OFF), stereo-capable, factory presets · b) a new `sccomp` module beside it | a | One compressor for the user, one code base; the appended parameter keeps every saved project's blob valid |
| Q3 | PING-PONG's longest time | a) 0.5 s (88 KB of SDRAM an instance) · b) 1 s (176 KB) · c) 2 s (352 KB: over the 256 KiB a module's state may take, `kMaxStateBytes`) | b | 8 steps (a half note) at 120 BPM; a synced time above it is halved until it fits (it stays on the grid); four instances take 704 KB of the 768 KiB module region, so a fourth may be refused with other modules loaded (R2) |
| Q4 | CPU budget of a stereo FX2 insert (OS-INSFX section 6: a track insert aims at 3 points; more needs the maintainer) | a) 3 points, the stereo adapter included · b) 4 points for a WIDENS insert, the adapter included (the adapter alone is about 2 points [est], ABI2 Q7); 3 for COMP | b | With a, about 1 point is left for the DSP: the chorus and the ping-pong (SDRAM lines) would not fit; a stereo voice costs about 1 point and a widening insert does a stereo voice's work plus the effect's |
| Q5 | AUTOPAN's pan law | a) linear: S = S_in + p x M (L + R stays 2M: mono-compatible; the sides are 3 dB louder in power than the centre) · b) constant power (no level bump, the mono sum dips by up to 3 dB at the centre of a swing) | a | The pack's promise is mono-compatible width (WIDE, AUTOPAN); the same law OS-STEREO uses to add S |

## 1. Goal

Five effect modules for the track inserts on ABI 1.11 (OS-INSFX-ABI2), each with factory presets, each its own S slice, made the way the FX1 modules and the `biquad` are made (`modules/effects/<id>/`, `modules/CMakeLists.txt` rows, an oracle host test, the `tmod-perf` rules, a unit measurement, QA rows, a manual entry): **WIDE** (a Lauridsen-style widener, mono-compatible), **STEREO CHORUS** (two modulated taps in opposite phase), **PING-PONG** (a tempo-synced stereo echo), **AUTOPAN** (an LFO on the side, synced to the beat, with the optional headphone mode of Q1), and **COMP** with a **KEY** track (an audio sidechain). None costs flash: they ship as `.tmod` files for `/modules/effects/` on the card and in the release zip.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | The official firmware has no insert effects; its send delay has a ping-pong switch and a sync rate; its chorus is an offline (destructive) sample-editor effect, not a live one | `tracker-ce@7fd6c2c:core/source/mtStructs.h:857`, `tracker-ce@7fd6c2c:core/AudioEngine/mtAudioEngine.cpp:1057`, `tracker-ce@7fd6c2c:core/effector/mtEffectChorus.h:8-18` | verified (tracker-ce source); no parity claim |
| F2 | This OS's send delay already has ping-pong, 19 sync rates in sequencer steps (labels `8` .. `1/32`, "4" a quarter note), a cross-fade of 1,024 frames on a time change, int16 words rounded toward zero so a tail ends at exactly 0, and a block-burst copy of the read span because a delay of 64 frames or more reads only frames written before the block | `audio/include/o3os/audio/send_delay.h:7-26`; `audio/include/o3os/audio/graph_params.h:173-176`; `app/include/o3os/app/send_fx_rows.h:41-43` | code fact |
| F3 | Performance has a PAN LFO that moves the track's pan (not a stereo image of the track's own sound) | `docs/specs/OS-PERFFX.md:381` | code fact |
| F4 | The per-module process of FX1: `modules/effects/<id>/{<id>.cpp, module.json, README.md, tests/<id>_test.cpp}`, CMake `o3m_manifest(... GENERATE)`, `o3m_add_module(... OPT -O2 VERSION 1.0.0 GEN)`, `o3m_add_test`, a HIL file `platform/mk66/tools/hil/qa_<id>.json`, three `docs/qa/catalogue/global.md` rows (load and hear, oracle, unit cost), a `docs/architecture/cpu-budget.md` section, a `THIRD_PARTY.md` own-maths line, the SDK README's module list | `git show 9ead14f3 --stat` (branch `feat/OS-INSFX-FX1-drive`); `modules/CMakeLists.txt:186-193` (biquad) | code fact |
| F5 | A module's state goes to the 4 KiB SRAM_U arena (about 640 B free), then to SDRAM; at most 256 KiB a module; the SDRAM module region is 768 KiB and a bump allocator (no free of one block; a free record's state is reused by the same module); all hot parts share a 14 KiB SRAM_L slab (about 6 KiB free), a hot part that does not fit runs from SDRAM, 10 to 30 times slower | `module/include/o3os/module/image.h:53`; `module/include/o3os/module/arena.h:4-9`; `platform/mk66/include/o3os/platform/mk66/mem_map.h:103`; `.claude/skills/tmod-perf/SKILL.md:35-37` | code fact |
| F6 | An insert's stored blob is positional (a float a parameter); a short blob sets the first parameters, the rest keep their defaults | `audio/include/o3os/audio/insert_host.h:103-104` | code fact |
| F7 | A track insert aims at 3 points of the block or less, a master insert at 6; more needs the maintainer; at most 4 instances | `docs/specs/OS-INSFX.md:106-107` | code fact (spec) |
| F8 | ABI 1.11 (OS-INSFX-ABI2): `render_st` on L / R float planes, the WIDENS and stereo-capable shapes, `get_clock` (tempo and the beat from the pattern's step 0), `track_level` (the key track's block peak, at most one block late), the KEY parameter type, factory presets in the descriptor; the stereo adapter costs about 4.5k to 5.5k cycles a block [est] | `docs/specs/OS-INSFX-ABI2.md` sections 3.1 to 3.6 | design (not built) |

## 3. Design (common to the five)

- **Manifest.** `"abi": "1.11"`, `"stereo": "widens"` (WIDE, CHORUS, PING-PONG, AUTOPAN) or `"capable"` (COMP), `"presets": [...]` (the ABI2 A2 generator), no `views` (no UI of its own: the OS's parameter page shows the rows; no hot or cold view code).
- **Render.** `render_st` is the work; the mono `render` (the loader requires it) of a WIDENS module is the same code with `in_r = in_l` and L written back as the mono output (a fallback no host of this OS calls). Control at block rate (`tmod-perf`): parameters read once a block into the state, per-sample values ramped inside the block; no libm, no double, no division in the sample loop; `O3M_HOT` only on the render.
- **SDRAM lines.** Every line's shortest delay is at least 64 frames (1.45 ms), so a block reads only what earlier blocks wrote: the read span is copied once a block into a stack scratch, the arithmetic runs in SRAM, the 64 new frames are written back in one burst (the send delay's rule, F2).
- **Mono compatibility.** WIDE and AUTOPAN (SPEAKERS) keep the mid exactly: the effect only adds to S, so the mono sum (L + R) / 2 is the input M within 1 LSB.
- **Tails.** A module returns 1 while its line holds a non-zero sample (the OS's S2 tail rule then keeps the strip alive), 0 once the line is silent; int16 lines round toward zero (F2) so a tail ends.
- **Presets.** Four or five a module (section 5), every value in range (the generator checks).
- **Budget.** Q4: a WIDENS insert at most 4 points with the adapter, COMP at most 3; the unit's `diag bench insert` decides; above the budget the slice stops for the maintainer.

## 4. The modules

### 4.1 WIDE (`wide`, WIDENS)

S' = S_in + w x HP(delay(M)): a delayed copy of the mid, high-passed, added to the side (Lauridsen's widening: the two ears get complementary combs, the mono sum cancels them). Parameters: `Width` 0..100 % (default 50; w = Width / 100), `Time` 2..30 ms (default 12), `Low cut` 20..1000 Hz log (default 200: the bass stays centred). Line: 30 ms of float M in SDRAM (1,324 floats, 5.3 KB). State: under 64 B besides the line. Cost [est]: 64 x (one-pole high-pass + the add) plus the bursts, about 1.2k cycles (0.5 %); the insert with the adapter about 6.5k (2.5 %). Presets (Width, Time, Low cut): `SUBTLE` (25, 8, 300), `WIDE` (50, 12, 200), `HUGE` (85, 18, 150), `DOUBLER` (60, 26, 120).

### 4.2 STEREO CHORUS (`chorus`, WIDENS)

One line of the mono input M (in = (L + R) / 2), two taps at `Delay` +- `Depth` x LFO, the two LFOs a half cycle apart (one sine, tap 2 = 2 x Delay - tap 1, so the taps' sum stays constant and the mono sum is a mild, even chorus); L = dry + Mix x tap 1, R = dry + Mix x tap 2 (dry: the input's L and R, so a stereo voice keeps its S). Linear interpolation, a fractional delay. Parameters: `Rate` 0.05..5 Hz log (0.6), `Depth` 0..100 % of 8 ms (40), `Delay` 5..25 ms (12), `Mix` 0..100 % (50). Line: 40 ms of float in SDRAM (7 KB). Cost [est]: the read span of each tap (64 frames plus the LFO's move in a block, under 70) is copied by burst; about 2.5k cycles (1.0 %); the insert about 7.5k (2.9 %). Presets (Rate, Depth, Delay, Mix): `CLASSIC` (0.6, 40, 12, 50), `SLOW WIDE` (0.15, 70, 18, 60), `VIBRATO` (4.5, 25, 6, 100), `ENSEMBLE` (2.2, 30, 20, 45).

### 4.3 PING-PONG (`pingpong`, WIDENS)

The send delay's ping-pong on a track (F2), stereo out: `Lw = x + g f(Rr)`, `Rw = g f(Lr)`, both lines delayed by D (x = the input's mid), so the first echo is on the left, the next on the right; the output L = dry L + Mix x (a Lr + b Rr), R = dry R + Mix x (b Lr + a Rr) with a = (1 + Width) / 2, b = (1 - Width) / 2 (Width 0: the echoes centred). f: a one-pole low-pass (`High cut`) and a one-pole high-pass (`Low cut`) in the loop; g capped below unity loop gain at every frequency. Parameters: `Sync` INT with 16 labels: `FREE` and the send delay's 15 longest step lengths (`8`, `6`, `4`, `3`, `2`, `3/2`, `1`, `3/4`, `1/2`, `3/8`, `1/3`, `1/4`, `3/16`, `1/6`, `1/8`; default `3`, a dotted eighth), `Time` 10..1000 ms (used when FREE; 375), `Feedback` 0..95 % (45), `Low cut` 20..2000 Hz log (120), `High cut` 1000..20000 Hz log (6000), `Mix` 0..100 % (35), `Width` 0..100 % (100). Synced: D = round(steps x fs x 15 / bpm) from `get_clock`'s tempo (120 BPM while there is no clock), halved while it is over the line (Q3), held to 64 frames at least. A change of D cross-fades the old and new taps over 1,024 frames (the send delay's rule). Lines: interleaved int16 pairs, one 32-bit word a frame, 1 s = 44,117 frames (176 KB in SDRAM), rounded toward zero. Cost [est]: one read and one write burst of 64 words a block plus about 20 cycles a frame, about 2.0k cycles (0.8 %); the insert about 7k (2.7 %). Presets (Sync, Feedback, Low cut, High cut, Mix, Width; Time 375 ms unless FREE): `1/8 PING` (2, 45, 120, 6000, 35, 100), `DOTTED 8` (3, 40, 150, 5000, 35, 100), `SLAP` (FREE 90 ms, 10, 200, 8000, 30, 60), `DUB` (4, 75, 300, 2500, 40, 100), `WIDE 16TH` (1, 30, 200, 9000, 25, 100).

### 4.4 AUTOPAN (`autopan`, WIDENS)

p = Depth x LFO(phase), S' = S_in + p x M (Q5 a, the linear law; L = (1 + p) M + S_in, R = (1 - p) M - S_in, the mid unchanged). The LFO is computed once a block and ramped linearly inside it. Synced: the phase is `beat x 4 / steps + Phase` (mod 1) from `get_clock` (so a bar-aligned pattern swings on the beat and a jump re-aligns at once); FREE: a free-running phase at `Rate`. Shapes: SINE (a parabolic approximation, no libm), TRIANGLE, SQUARE (smoothed by a one-pole of about 5 ms so it does not click). Parameters: `Sync` INT 16 labels (`FREE`, then `64`, `32`, `16`, `12`, `8`, `6`, `4`, `3`, `2`, `3/2`, `1`, `3/4`, `1/2`, `1/3`, `1/4` steps a cycle; default `16`, one bar), `Rate` 0.05..10 Hz log (when FREE; 1), `Depth` 0..100 % (70), `Shape` INT SINE / TRI / SQUARE (0), `Phase` 0..360 degrees (0), and with Q1 b `Mode` INT SPEAKERS / HEADPHONE (0). State: under 64 B. Cost [est]: about 3 cycles a sample, 0.4k cycles (0.15 %); the insert about 5.5k (2.1 %). Presets (Sync, Rate, Depth, Shape, Phase): `SLOW SINE` (16, 1, 70, SINE, 0), `TRI 8TH` (2, 1, 80, TRI, 0), `SQUARE 16TH` (1, 1, 60, SQUARE, 0), `GENTLE` (32, 1, 35, SINE, 90).

**HEADPHONE mode (Q1 b).** The pan position p drives two cues a block: the interaural time difference (the far ear, the one p points away from, delayed by 0.6 ms x |p|: 0 to 26.5 frames, a fractional delay with linear interpolation on a 32-frame float ring an ear, 256 B in the state) and the head shadow (the far ear through a one-pole low-pass whose corner falls from 20 kHz at the centre to about 1.8 kHz at full side, coefficient a block, ramped). The near ear gets the level law of SPEAKERS. Not mono-compatible (the delayed ear combs in the mono sum): said on the parameter's help line and in the manual. Cost [est]: about +15 cycles a sample, +1.0k cycles (0.4 %) while on. Own maths from the textbook spherical-head model (a time delay and a level-and-tone cue), no code or data taken.

### 4.5 COMP with KEY (`comp` 1.1.0, stereo-capable, Q2 a)

FX1's COMP as merged (its parameters, its TRANSIENT mode, its oracle test) moves to ABI 1.11 and version 1.1.0 and gains: (1) a `Key` parameter appended after FX1's last one, type key (`OFF`, `T1`..`T16`, default OFF; a 1.0.0 blob loads with the key OFF, F6); (2) a stereo-capable `render_st` with a linked gain (the same gain on L and R, the detector on max(|L|, |R|)); (3) the keyed detector: when Key is not OFF, the detector input is `track_level(key)` (a block peak, ABI2 3.5) instead of the module's own input, at block rate; the gain curve (threshold, ratio, knee) and the attack and release act on it once a block, the gain is ramped linearly over the block; TRANSIENT mode ignores the key (it detects its own transients; the help line says so); (4) presets. Unkeyed and mono, the output is bit for bit the 1.0.0 output. Cost [est]: keyed, the per-sample detector is skipped: at most FX1's figure. Presets, in FX1's parameter names (values adapted to FX1's ranges at implementation): `DUCK T1` (key 1, deep ratio, fast attack, 180 ms release), `PUMP T1` (key 1, 8:1, 1 ms attack, a release of about a quarter note at 120 BPM), `GLUE` (no key, 2:1, 30 ms attack, 300 ms release), `PUNCH` (no key, 4:1, 25 ms attack to let the transient through).

## 5. Slices

| # | Slice | Size | Level | Depends on | Flash | Card file [est] | State [est] |
|---|---|---|---|---|---|---|---|
| X1 | WIDE | S | B | ABI2 A1, A2, A4 | 0 | 2.5 KB | 5.4 KB SDRAM |
| X2 | STEREO CHORUS | S | B | ABI2 A1, A2, A4 | 0 | 3 KB | 7.1 KB SDRAM |
| X3 | PING-PONG | S | B | ABI2 A1 to A4 | 0 | 3.5 KB | 176 KB SDRAM |
| X4 | AUTOPAN | S | B | ABI2 A1 to A4 | 0 | 2.5 KB | 64 B |
| X5 | AUTOPAN HEADPHONE mode (Q1 b) | S | B | X4 | 0 | +0.8 KB | +256 B |
| X6 | COMP with KEY (Q2 a) | S | B | ABI2 A6, FX1 COMP | 0 | +0.8 KB | as FX1 |

Every slice: no OS code (flash 0, SRAM_L and SRAM_U 0 of the image); the module's hot part at most 1.5 KB (the shared slab, R3); `tools/ci.sh qemu` before merge (the module's target goldens); the HIL file and the unit bench before the row is closed.

Each slice holds, for module `<id>`: `modules/effects/<id>/<id>.cpp`, `module.json`, `README.md` (parameters, presets, cost, the unit's figures when measured), `tests/<id>_test.cpp`; `modules/CMakeLists.txt` (the three rows and the test's generated-header include, as the biquad); `platform/mk66/tools/hil/qa_<id>.json` (load, `ins bind`, hear, bypass, tail, `bench insert 64`); `docs/qa/catalogue/global.md` three rows (the load and hear row, the oracle row, the unit cost row; ids the next free at merge); `docs/architecture/cpu-budget.md` a section with the rows `insert_effect`, `insert_effect_mod`, `insert` (stereo), `ins` on Dense8 ([est] until measured); `THIRD_PARTY.md` a line: own maths, no third-party code (X5 names the spherical-head model as the source of the idea); `module/sdk/README.md` the module list; the manual's effect page (`docs/manual/effects.md`, a section a module; created by the first FX1 or FX2 slice that needs it, linked from `docs/manual/mixer.md`). Host test: ctest `o3m_<id>_test` (a fake `o3m_host` with scripted `get_clock` and `track_level`; a double-precision oracle of the same maths written in the test). No UI of the module's own: no sim golden.

**X1 WIDE.** Acceptance: over 60 settings and white noise, sine and impulse inputs: the mono sum equals M within 1 LSB; Width 0 is the input bit for bit; a stereo input keeps S_in (S' - S_in = the oracle's added side to 1e-4); the max error against the oracle under 1e-4 and SNR over 90 dB; the L / R correlation of white noise falls monotonically with Width; render returns 0 once 30 ms of silence has passed; every preset in range; no allocation (counting allocator). QA: load and hear, oracle, unit cost. Unit check: `bench insert 64`; on headphones a mono sample on the track is wide, a mono sum of a USB recording of MAIN keeps the dry level.

**X2 STEREO CHORUS.** Acceptance: oracle (interpolated taps) under 1e-4, SNR over 90 dB, at 40 settings; the two taps' delays sum to 2 x Delay at every sample; Mix 0 is the input bit for bit; a Rate or Depth change makes no step over the oracle's ramp; silent input: 0 returned after 40 ms; presets in range. Unit check: bench; by ear, CLASSIC on a pad track.

**X3 PING-PONG.** Acceptance: an impulse gives echoes at D on L, 2D on R, 3D on L, each scaled by the oracle's loop (to 1 LSB, int16 lines); Width 0 centres them; synced D at 90, 120 and 174 BPM equals round(steps x fs x 15 / bpm) for every label; 8 steps at 60 BPM is halved to 4; a tempo or time change cross-fades over 1,024 frames with no sample step over the oracle's; feedback 95 % with silence in reaches exactly 0 (no limit cycle) and render returns 0 then; the tail flag is 1 while the lines hold energy; no clock (stub returns 1): 120 BPM; presets in range. Unit check: bench; DOTTED 8 on a snare track at the song's tempo stays on the grid after a tempo change.

**X4 AUTOPAN.** Acceptance: the mono sum equals M within 1 LSB at every setting (SPEAKERS); with a scripted clock at 120 BPM and Sync `4`, p crosses zero at the start and the middle of every beat, to one block; Phase shifts it; a WRAPPED clock re-aligns the phase in that block; FREE runs at Rate within 0.1 %; SQUARE's largest step in a sample is under the 5 ms smoothing's bound; a stereo input keeps S_in; presets in range; no allocation. Unit check: bench; SLOW SINE on a pad track pans one cycle a bar, in time with the pattern.

**X5 HEADPHONE.** Acceptance: SPEAKERS output bit for bit X4's; at p = +1 an impulse reaches the far ear 26.5 frames later (+-0.5) and its level at 5 kHz is lower than the near ear's by the oracle's figure (+-0.5 dB); at p = 0 L = R; a sweep of p makes no click (largest sample step bounded); the ring needs no allocation. Unit check: on headphones the HEADPHONE pan sounds outside the head; bench the extra cycles.

**X6 COMP with KEY.** Acceptance: FX1 COMP's own test vectors pass unchanged (unkeyed, mono: bit for bit 1.0.0); a 1.0.0 blob (one float short) binds with Key OFF; keyed, with a scripted `track_level` (a kick envelope), the gain follows the oracle's block-rate detector to 0.1 dB; TRANSIENT ignores the key; stereo-capable: L and R get the same gain (S scaled with M); a key level of 0 (a silent or unnamed track) gives unity gain; presets in range. QA: also a `docs/qa/catalogue/mixer.md` row (the KEY row on the insert parameter page shows `T1`). Unit check: DUCK T1 on a pad track keyed by a muted kick on track 1 ducks in time; bench keyed and unkeyed.

## 6. Assumptions (decided here)

- A-1 Module ids `wide`, `chorus`, `pingpong`, `autopan`; COMP keeps FX1's id `comp`. Display names `WIDE`, `STEREO CHORUS`, `PING-PONG`, `AUTOPAN`, `COMP`.
- A-2 Sync lists are step lengths, as the send delay's page (F2): the user meets one vocabulary. PING-PONG uses its 15 longest (the label table holds 16 with FREE); AUTOPAN uses cycle lengths from 64 steps (four bars) to 1/4 step.
- A-3 Lines in SDRAM with the block-burst rule; float lines for the short ones (WIDE, CHORUS: simpler oracles), int16 pairs for PING-PONG (memory; rounding toward zero ends the tail).
- A-4 No `views`: the parameter page is enough and the cold code stays small.
- A-5 PING-PONG with no clock (an OS of 1.11 always has one; the stub returns 1 on a host without a sequencer) runs at 120 BPM.
- A-6 COMP's keyed detector is block-rate on the key's block peak; finer keying is out of scope (ABI2 R5).
- A-7 Preset values above are this spec's; X-slices may retune them by ear and say so in the README.
- A-8 The headphone ITD maximum is 0.6 ms (an average head); the head-shadow corner at full side about 1.8 kHz; both fixed, not parameters.

## 7. Risks

- R1 **CPU** (Q4): the stereo adapter takes most of a WIDENS insert's cost; measured per module; a module over its budget stops for the maintainer.
- R2 **SDRAM**: four PING-PONGs take 704 KB of the 768 KiB module region (bump allocator, F5); a bind that finds no room is refused (`NeedState`) and the picker says so; the README and the manual say it.
- R3 **The hot slab**: five FX2 modules loaded with the built-ins may not all fit the slab (about 6 KiB free); a module that does not fit runs from SDRAM, much slower; each hot part at most 1.5 KB, `diag mod` checks the placement in each HIL file.
- R4 **Mono compatibility** of CHORUS and the HEADPHONE mode is not exact: documented, the mode is off by default.
- R5 **FX1 COMP's shape** is unknown until it merges: X6 is written against it then; its presets map to its parameter names.
- R6 **Feedback stability** (PING-PONG): the loop gain cap and the oracle's impulse test at 95 %.

## 8. Evidence labels

- WIDE, STEREO CHORUS, PING-PONG, AUTOPAN, the HEADPHONE mode, COMP's KEY and every preset: **new Community OS behaviour** (F1: the official firmware has no live inserts).
- Costs and sizes marked [est] are estimates until each module's `bench insert` on the unit.
- F2 to F7 are code facts at c1dc3125 and 9ead14f3; F8 is the ABI2 design, not built.

## 9. State for implementer

One slice a run, Level B; scoped CI on the remote (`tools/remote_ci.sh`, `O3OS_CI_SCOPE=changed`), `tools/ci.sh qemu` before merge; report the card file size, the hot part, the state, and the `bench` figures when the tester has them.

- **X1 to X5.** Start with: `modules/effects/drive/` on `feat/OS-INSFX-FX1-drive` (`git show 9ead14f3`: the files, the oracle test's layout, the HIL file) or `modules/effects/biquad/` on dev; `modules/CMakeLists.txt:186-193`; the stereo template `modules/templates/effect_stereo/` (ABI2 A2); `SendDelay` (`audio/include/o3os/audio/send_delay.h:7-26`) for the burst rule, the cross-fade and the rounding toward zero (facts only; the module has its own code). Rules: `tmod-perf` (control at block rate, no per-sample host maths or division, `O3M_HOT` on the render only, hot part at most 1.5 KB); float only, no libm, no double; state in the module's block; mono sum exact for WIDE and AUTOPAN SPEAKERS; every preset in range.
- **X6.** Start with: FX1's `modules/effects/comp/` as merged (its `module.json`, its test vectors), `O3M_PARAM_KEY` and `track_level` (`module/sdk/o3module.h`, ABI2 A1 and A6), the key template `modules/templates/effect_key/` (ABI2 A6). Rules: the Key parameter is appended last; the unkeyed mono output stays bit for bit; the version is 1.1.0.
- Unit checks (tester, a gated image with ABI 1.11 and the `.tmod` files on the card): each module's HIL file and `bench insert 64`; the by-ear checks in each slice.
