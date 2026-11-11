# OS-SNDMOD: sound modules package (TB-303, Chip, FM)

Status: APPROVED as one package (maintainer, 2026-10-18: "all your recommendations, into the backlog as one complete package of changes that then go one by one"). Source: a read-only review by Fable of `dev` 777d84c (2026-10-18), kept below from section 0 on; line numbers are those of that commit. Each slice still gets a short spec of its own (intent, acceptance, the appended ids) before code.

Evidence labels: `verified` = seen in code or a screen; `open question` = not checked; `hw` = a claim about the real hardware (public knowledge, no official-firmware material).

## The package, slice by slice

One branch and one commit per slice, one `firmware-reviewer` each, merged as soon as it is green. The numbers in the second column are the rows of the triage table (section 6).

| Slice | # | What | Level | Gate |
|---|---|---|---|---|
| S1 | 17, 16 | Chip `enter_release` at env 0 (B1, with a host test). Page and descriptor polish: one algorithm-name table for FM's field and view (F1), FM Level without the `percent` unit (F4), TB-303 page split (T1: SOUND holds ids 0..7, page 2 starts at id 8 and is named for what S3 puts there), Chip Wave and Duty learnable (C3), the NOISE LONG view pre-rolled (H4). The three READMEs refreshed (section 5). | A/B | host tests; `tools/ci.sh qemu` (the module bytes change) |
| S2 | 1, 2 | FM: Algorithm 1..16 with labels (F-a); Detune +-1200 cents (F-b). Range extensions: stored values keep their meaning. | B | host tests against the double-precision model; qemu; unit `diag mod` before merge |
| S3 | 3, 4, 5 | TB-303 page 2 (`MODS`): Drive, Tune, Transpose, Accent threshold, Filter Tracking, Accent Decay, Soft Attack, appended as ids 9..15 (T-a..T-d). | B | the id order is fixed in the slice's spec before code (appended ids are persistent once released); oracle test unchanged with the new parameters at their neutral values; unit CPU measurement before merge (the voice is at 16 %, over its 12 % budget row) |
| S4 | 7 | Chip PWM Depth and Rate, ids 20-21; page 4 renamed `ARP/PWM` (H-a). | B | qemu; unit `diag mod` before merge |
| S5 | 8 | Chip SVF filter (mode, cutoff, resonance), ids 22-24; `state_bytes` 208 to 256 (H-b). | B | unit CPU measurement before merge |
| S6 | 18 | The manual chapter for the three sound modules (the rules of OS-MANUAL2: owners of the official firmware, what is new first) and the by-ear QA rows per module sound. Each feature slice adds its own catalogue rows; S6 covers the rest and writes the chapter once the pages are final. | A | `o3sim_manual_images` if it adds images |
| D1 | 13, 14 | Maintainer decision (Level C): one ABI minor with `O3M_PARAMS_MAX` 64, `O3M_PAGES_MAX` 8, page keys past F4 (W1) and `tempo` in `o3m_host` (W4). Needs a spec and a risk review. | C | open |
| D2 | 15 | Maintainer decision (Level C): the module version in `MODR` (W3), a migration hook before any module reorders. | C | open |
| L1 | 9 | Chip Noise Mode and Arp Mode (H-c). | A/B | after D1: S4 and S5 fill Chip's last five rows |
| L2 | 10 | Chip ring/sync oscillator and duty sequence (H-d, H-e). | B | after D1 |
| L3 | 11 | FM per-operator velocity, LFO, pitch envelope, key scaling, OPL waveforms (F-c..F-f), one slice each. | B | after D1 |
| L4 | 6 | TB-303 sub-oscillator (T-e). | B | after Perf M2 (PARKED; it returns on the cpu-budget trigger) |
| — | 12 | FM `voice_pitch` follows `per_frame` (F8): folded into Perf M3 (PARKED). | — | a note on the Perf M3 row |
| — | 19 | The built-ins in the simulator, so their pages and views render: the E2E-7 slice of OS-E2E (IDEA), not duplicated here. | — | — |

Rules for every slice: parameters are only appended (format-safe today, section 1: `bind()` applies min(count, blob / 4) values and keeps defaults for the rest); no reorder, removal or change of meaning of an existing id without D2; a range may only widen. Corrections made when this review was filed: Chip and FM were measured on the unit (section 1), and Perf M2 and M3 are PARKED, not queued.

## 0. What was looked at, and how

- Code: `modules/tb303/{module.json,tb303.cpp,tb303_dsp.h,tb303_gen.h,README.md}`, `modules/chip/*`, `modules/fm/*`, the module tests, `module/sdk/o3module.h`, `module/sdk/README.md`, `app/src/module_ui.cpp` (`describe_module_param`), `app/src/screens/params_screen.cpp` (module pages and view layout), `app/src/param_actions.cpp` (the edit path of a module parameter), `audio/src/module_voices.cpp` (`bind`, the pitch ramp), `model/src/project.cpp` (the blob), `docs/specs/OS-MODPITCH.md`, `docs/architecture/cpu-budget.md`, `docs/qa/catalogue/modules.md`, `docs/manual/modules.md`, `docs/manual/module-instrument-fx.md`, `docs/roadmap/BACKLOG.md` (MOD-ABI, OS-FXMODULE, ODD-11, Perf M2/M3, OS-MODMX, dropped "Drum synth module"), the `tmod-perf` skill.
- Screens: **the simulator cannot show the three built-in modules' pages.** `sim/src/screens_modules.cpp:208-219, 323-371` binds fake descriptors ("Demo Synth", "Stress", "Named Synth"); the catalogue says "modules do not run audio in the simulator" (QA-MOD-100..104) and E2E-7 "modules in the sim" is a later backlog slice. I looked at the OS page frame on all three themes with `gallery_shard{4,5,6}/modules_named_{amiga,black,white}.png` (a labelled INT value, the side card, named page keys) and `modules_page_amiga.png` / `modules_page_two_black.png` (two views under the rows). **Every module-specific finding below (pages, views, ranges) is from code**, not from a rendered screen. Rendering them would need a sim change (link the built-ins through `ModuleSet::set_builtins` as `platform/mk66/src/app_main.cpp:768` does), which is E2E-7.

## 1. Facts checked (the orchestrator's brief to the review)

| Claim | Finding | Evidence |
|---|---|---|
| TB-303 41.5-43.5k cycles per block (~16 %) | Confirmed in the backlog (ODD-11 row, tester on v0.6.4). An earlier unit figure is 21.6 % on 0.4.9 before OS-303PERF. | `docs/roadmap/BACKLOG.md:54`, `docs/architecture/cpu-budget.md:9` verified |
| Chip and FM "only estimates (1.3-1.8 % and ~6 %)" | **Wrong: both were measured on the unit on 0.4.9: FM 11.7 % (~30.5k cycles), Chip 2.8 % (~7.3k).** The READMEs still carry the low estimates (FM 16k / 6 %, Chip 4.8k / 1.8 %). FM costs about twice its README figure. | `cpu-budget.md:9`, `modules/fm/README.md` "Sizes and cost", `modules/chip/README.md` "Sizes and cost" verified |
| Budget rows | The heavy-song budget gives TB-303 12 %, FM 8 %, Chip 2 %; all three are **over** their rows today (16 / 11.7 / 2.8). Any added hot-loop work must be near zero or come after Perf M2 (TB-303 decimator) and M3 (FM per-block envelopes). | `cpu-budget.md:58`, `tmod-perf` §1 verified |
| 14 KiB shared slab | Slab is 13,056 B; hot parts TB-303 3,424 B, Chip 2,096 B, FM 1,952 B = 7,472 B, about 5.5 KB free. | `cpu-budget.md:99` verified |
| Flash about 70 % | 787,280 B after OS-POWER P2 = **75.1 %** of 1 MiB. | `docs/STATUS.md:8` verified |
| Parameter counts and pages | TB-303 9 on SOUND(0-5)/SLIDE(6-8); Chip 20 on TONE(0-7)/ENV(8-11)/PITCH(12-16)/ARP(17-19) (audit time; now 30 on TONE(0-7)/ENV(8-13)/PITCH(14-21)/OSC-FX(22-29), OS-CECHIP C7); FM 32 on OP1..OP4. | `*_gen.h` verified |
| Preset = float32 list in parameter order, saved in the project | Yes: `Instrument::params`, `param_len` (INST chunk, "opaque, kept verbatim", at most 256 B = 64 floats). | `docs/formats/o3-project.md:141-142`, `model/include/o3os/model/limits.h:36` verified |
| What the loader does with fewer or more values | **Fewer:** `bind()` applies `min(param_count, param_len/4)` values; the rest keep the module's `init()` defaults; the page reads the default for a parameter the blob does not reach; the first edit of such a parameter extends the blob with the module's defaults up to that index. **More:** the extra values are ignored, kept verbatim, and written back. So **appending parameters at the end is format-safe today without a version bump**; reordering, removing or changing the meaning of an existing index is a silent break (the blob is opaque and the project records the module name and ABI version only, `MODR`, not the module version, so there is no migration hook). | `audio/src/module_voices.cpp:191-199`, `app/src/module_ui.cpp:1542`, `app/src/param_actions.cpp:268-283`, `model/src/project.cpp:805-874`, `docs/formats/o3-project.md:94` verified |
| TB-303 README stale | Yes, several statements (section 4). | verified |
| Default velocity vs accent | A pattern note that names no velocity reaches a module as velocity **96**, under the TB-303 accent threshold of 100: plain steps are not accented, a volume FX of 100+ accents. Good design; it is not in the manual. | `audio/include/o3os/audio/module_voices.h:74`, `tb303_dsp.h` `voice_note_on` verified |
| Mod targets | The modulation matrix (OS-MODMX) is **PARKED** by the maintainer (2026-10-17): `O3M_PARAM_MOD` has no consumer except the "MOD" word on the side card. | `BACKLOG.md:111`, `params_screen.cpp:1159` verified |
| Host tempo | `o3m_host` gives an instrument `sample_rate`, exp2/log2/sin/cos and the draw table only; **no tempo** (apps get `get_tempo`). A tempo-synced arpeggio or LFO in a module needs an ABI minor. | `o3module.h:625-642` verified |

## 2. Page review

### 2.1 Cross-module consistency

| | TB-303 | Chip | FM |
|---|---|---|---|
| Volume | id 7, on **SLIDE** | id 4, TONE | id 23, on **OP3** |
| Vel Sens | none (velocity >= 100 = accent) | id 5, TONE | id 31, on **OP4** |
| Transpose / Fine | none | ids 6, 7 | none (per-operator Detune only) |
| MOD flags | none | Volume, Sweep, Vib Depth | Op1..4 Level, Volume |
| NO_LEARN | none | Wave, Duty | Algorithm |
| Pages | 2 | 4 | 4 (full) |

Findings:
- **C1 (inconsistency, verified)** Volume sits on a different page in each module: TB-303 `SLIDE` (`tb303/module.json:16-19, 24-27`), Chip `TONE`, FM `OP3` (`fm/module.json` id 23). FM's four globals are scattered one per page (Algorithm on OP1, Feedback on OP2, Volume on OP3, Vel Sens on OP4): a user looking for Volume has to know it is on the third operator's page. Forced by the 32-parameter ceiling. A page name cannot say it (7 characters). Only a `(G)` suffix or a docs note can help; or an ABI change (section 3).
- **C2 (inconsistency, verified)** No Transpose/Fine on TB-303 or FM; Chip has both. A 303 has a Tune knob (`hw`); an FM piano a semitone off has no fix but per-operator Detune (+-100 cents on each of four operators). Both are append-only additions (section 3).
- **C3 (polish, verified)** Learn flags: Chip's Wave and Duty are `learnable: false` (`chip/module.json:10-11`); TB-303's Waveform is learnable. Switching duty from a CC is a classic chip trick; the flag seems a cautious default, not a decision. FM Algorithm `NO_LEARN` is reasonable (a routing swap clicks).
- **C4 (verified)** Every name fits the 12-character caption (`core::kParamNameChars`): the longest is "Legato Slide" (12, exactly the limit), labels <= 11 ("NOISE SHORT" = 11), page names <= 7. `o3m_gen.py` enforces the label and page limits (`tools/o3m_gen.py:81, 192-195, 252`).
- **C5 (verified, gallery)** The OS page frame is readable on amiga, black and white: labelled value field, side card with "RANGE PULSE .. NOISE SHORT", named F-keys, `TONE 1/2` heading. The module views themselves were not rendered (section 0).

### 2.2 TB-303 (`modules/tb303`)

Pages: SOUND = Waveform, Cutoff, Resonance, Env Mod, Decay, Accent; SLIDE = Slide, Volume, Legato Slide.

| # | Finding | Severity | Where | Evidence |
|---|---|---|---|---|
| T1 | Page 2 is called SLIDE but holds Volume. The page table is not persistent, so the boundary can move freely: SOUND = 0..7 (adds Slide and Volume: eight rows, the page's maximum) and a second page starting at 8 ("MORE") for Legato Slide and whatever is appended later. | polish | `module.json:24-27` | verified |
| T2 | Waveform is a 0..1 float with no unit, shown as a 0..100 % row ("WAVEFORM 0 %"). The blend is a feature (Open303's), but the caption says nothing about saw/square. A float cannot take labels. Renaming to "Saw>Square" (10 chars) is a descriptor-only change (name is not stored). | polish | `module.json:9`, `module_ui.cpp:259-267` | verified |
| T3 | Ranges and defaults reach the classic sound: Cutoff 314..2394 Hz log (the Open303 fit's measured knob range), Resonance 50, Env Mod 25, Decay 200..2000 ms log (default 1000), Accent 50, Slide 0..300 ms (60). The accent's filter decay is fixed at 200 ms (`tb303_dsp.h` `main_d_acc`), as on the unit (`hw`). A starting squelch needs Res 70-90 and Env Mod 60+; the default is a neutral mid tone. Acceptable. | ok | `tb303_gen.h:29-37` | verified |
| T4 | Views: FILTER draws the ladder response at the knob cutoff and at the envelope's peak (40 points each, log-frequency grid crowded round the cutoff); ENVELOPE the cutoff against time over 2 x Decay. Both draw from `canvas->params` (ABI rule). Useful. Accent is not shown (the peak line could use the accent decay when Accent > 0; cosmetic). | ok | `tb303.cpp:52-151` | verified |
| T5 | The two views together are **688 of the page's 700 estimated words** (the comment at `tb303.cpp:71`; `tests/tb303_view_test.cpp:112-121` pins it). 12 words of slack: any change to the OS's cost model or to the views blanks the second view and quarantines the module for the boot (`view.h` Words). Watch. | hazard (latent) | `tb303.cpp:71` | verified |
| T6 | No MOD flags. Right as long as OS-MODMX is parked: `voice_set_param` runs the full `derive()` (4 exp2/log2 and divisions) on every change (`tb303_dsp.h:405-420`), which cpu-budget M5 names. Flag Cutoff / Env Mod / Resonance only together with M5's cheap path. | ok (recorded) | `tb303_dsp.h:405-420` | verified |
| T7 | All parameters learnable, Legato Slide included; fine. | ok | | verified |

### 2.3 Chip (`modules/chip`)

Pages: TONE = Wave, Duty, Raw Edges, Vol Steps, Volume, Vel Sens, Transpose, Fine; ENV = A D S R; PITCH = Sweep, Sweep Time, Vib Depth, Vib Rate, Vib Delay; ARP = Arp 1, Arp 2, Arp Speed. (As audited. Since OS-CECHIP C7 the Chip has 30 parameters on four pages: TONE 0-7, ENV 8-13 (A D S R, Sweep, Sweep Time), PITCH 14-21 (Vib x3, Arp 1, Arp 2, Arp Speed, Shape, Sub Osc), OSC/FX 22-29; no ARP page.)

| # | Finding | Severity | Where | Evidence |
|---|---|---|---|---|
| H1 | Wave and Duty **have** labels since ABI 1.8 (`PULSE`.. and `12.5%`..): the README's "shown as a number; the Wave view names it" is stale. | docs | `chip/module.json:10-11`, `README.md` Parameters | verified |
| H2 | Page grouping is good and the order sensible; TONE is full (8). ARP has 3 of 8 rows: **appended parameters (ids 20..24) land on the ARP page** (pages must be contiguous and there is no fifth page), so any addition means renaming page 4 (for example `ARP/PWM`, 7 chars) or putting arpeggio-adjacent things there. | constraint | `chip_gen.h:71-76` | verified |
| H3 | Ranges: Attack 0..500, Decay 0..2000, Release 0..2000 ms linear (1 ms floor in code); Sweep +-48 st; Vib Depth 0..200 cents; Vib Rate 1..16 Hz log; Arp Speed 1..120 Hz log (free-running, not tempo-synced: no host tempo). Defaults (pulse 25 %, A0 D250 S60 R80, Volume -12) give a usable lead at once. Vol Steps off by default: a chip purist would want it on, but off is the safer default. | ok | `chip_gen.h:30-49` | verified |
| H4 | WAVE view, noise: it draws the LFSR's first 48 steps **from seed 1**. In long mode the first 14 outputs after the seed are all the same bit (the single 1 travels from bit 14 down to bit 0 before the feedback tap sees it), so the left third of the NOISE LONG picture is a flat line. Pre-roll the register a few hundred steps (or seed with a mid-sequence value). | polish | `chip.cpp:96-105`, `chip_dsp.h:32-35` | verified (by the LFSR arithmetic) |
| H5 | ENVELOPE view quantises to 4-bit steps when Vol Steps is on: a nice, correct touch. | ok | `chip.cpp:109-140` | verified |
| H6 | MOD flags Volume, Sweep, Vib Depth: all cheap in `set_param` (one exp2 or a store), as `tmod-perf` §3 asks. Vib Rate and Arp Speed could be flagged too (a store each). | ok | `chip_dsp.h:198-231` | verified |
| H7 | Noise is clocked at 16 x the note frequency, so it tracks the note; there is no fixed-period mode (a drum track with different notes changes the noise colour). A labelled `Noise Mode` (PITCHED / FIXED) is cheap. | gap | `chip_dsp.h:165-166` | verified |

### 2.4 FM (`modules/fm`)

Pages: OP1..OP4, each Ratio, Detune, Level, Attack, Decay, Sustain, Release + one global (Algorithm, Feedback, Volume, Vel Sens).

| # | Finding | Severity | Where | Evidence |
|---|---|---|---|---|
| F1 | **Two spellings of the same algorithm on one screen.** The value field uses `module.json` labels (`(4+3)>2>1`, `(4>3)+2>1`, `3>2>1 + 4`, `2>1 + 4>3`, `4>3>2 + 1`, `4>1,2,3`); the Algorithm view's title uses `fm.cpp:93` `kNames` (`(3+4)>2>1`, `(4>3 + 2)>1`, `(3>2>1) + 4`, `(2>1) + (4>3)`, `(4>3>2) + 1`, `4>(1+2+3)`). One table should feed both (`fm_labels_algorithm` from `fm_gen.h` is visible to `fm.cpp`). | inconsistency | `fm.cpp:93`, `fm/module.json` id 7 | verified |
| F2 | `Ratio` 0 means 0.5 (`fm_dsp.h:215`): the row shows **0**, the view shows **x0.5**. Labels cannot fix it (32 values > `O3M_LABELS_MAX` 16). Options: accept and document; or a descriptor name "Ratio(0=.5)" (11 chars). | polish | `fm_gen.h:52`, `fm_dsp.h:214-216` | verified |
| F3 | Only integer ratios with +-100 cents of detune: the inharmonic ratios of bells and metallic FM (1.41, 2.76, 3.53 ...) are unreachable. Widening Detune to +-1200 cents keeps every stored value's meaning (a range extension) and costs nothing. | gap | `fm/module.json` Detune | verified |
| F4 | `Level` is 0.75 dB a step (0 = silent) but carries the `percent` unit, so the row says "55 %". DX-style users expect a bare 0..99; a unit of none is one word in the manifest. | polish | `fm/module.json` Level, `fm_dsp.h:225` | verified |
| F5 | Velocity scales the carriers' sum only (`derive_gain`): **velocity never changes timbre** (no per-operator velocity sensitivity), which is the main expressive feature of FM keyboards. Needs 4 parameters: blocked by the 32 limit (section 3). | gap | `fm_dsp.h:160, 296-297` | verified |
| F6 | Views: ALGORITHM places operators by depth, lines from modulator to target, stubs to a bus, the feedback loop lit in ACCENT when Feedback > 0; ENVELOPES draws four polylines scaled by Level with the release marked. Good and generic (a new algorithm draws itself). The unlit loop and the box fill use `O3M_COLOR_EMPTY` / `PANE`: contrast on the white theme not seen. | ok / open question | `fm.cpp:88-253` | verified / open question |
| F7 | Defaults: the electric piano (alg 5, op2 ratio 14 at level 55, 4 cents chorus) is a good first sound; eight algorithms match the usual 4-op set. | ok | `fm/module.json` | verified |
| F8 | FM `voice_pitch` ignores `per_frame` (a staircase of one block, documented at `fm_dsp.h:313`). An octave Glide over 200 ms steps in 0.44-semitone jumps, 1.45 ms apart; the spec lists "FM octave-glide zipper" as an open unit check. Following `per_frame` needs a control-rate split, which Perf M3 adds anyway: fold it in. | polish (unit open) | `fm_dsp.h:313-320`, `OS-MODPITCH.md` | verified |

## 3. Feature ideas, ranked by musical value against cost

Common constraints (verified): append-only is format-safe (section 1); CPU budgets are already exceeded, so hot-loop additions must be measured on the unit before merge and should wait for M2/M3 where they compete; the slab has about 5.5 KB free; flash is at 75 %; `O3M_PARAMS_MAX` is 32 and 4 pages x 8; there is no host tempo; no table or blob parameters exist (so no macro sequences, no wavetables, no DPCM data); the "Drum synth module" IDEA was dropped by the maintainer (not proposed again); Perf M2 and M3 are PARKED, brought back by the cpu-budget trigger (not proposed again); Slide via the pattern FX exists since MOD-ABI (not proposed again).

Cycle figures are `[est]` at 1.3 cycles per instruction, 64 frames; "hot" = inside the per-sample loop.

### 3.1 TB-303

| # | Idea | Musically | CPU / block [est] | Bytes | Params / format / host | Risk | Size |
|---|---|---|---|---|---|---|---|
| T-a | **Drive** (soft clip after the notch, a tanh rational like `tanh_f` already in the file, with gain compensation) | The acid sound is a distorted 303; today only the ladder's internal soft clip | ~15 instr hot = **~1.3k cycles (0.5 %)**; skippable at Drive 0 | hot +~120 B, state +8 B | 1 appended param (id 9), on a new page (see T1); no format bump; no host change | B (hot loop, oracle test unchanged at Drive 0) | S |
| T-b | **Tune** (+-100 cents) and **Transpose** (+-24 st) | Tune the 303 to a song (the unit has a Tune knob, `hw`); octave shift without re-entering notes | 0 (in `note_frequency`) | hot 0, state +8 B | 2 appended params; no format bump | B | S |
| T-c | **Devil Fish-style set**: Filter Tracking (cutoff follows the note, 0..100 %), Accent Decay (today fixed 200 ms), Normal Attack / "Soft Attack" (amp envelope rise, today a 1-sample step) | The classic mod set; tracking makes high notes open up, separate accent decay shapes the "wow" | tracking: one add per kCtl update (0); accent decay: 0; soft attack: ~3 instr hot (~0.25k) | hot +~60 B, state +16 B | 3 appended params; no format bump | B | S/M |
| T-d | **Accent threshold** (velocity 1..127, default 100) | MIDI keyboards with soft velocity curves rarely reach 100; also lets a project accent at 90 | 0 | state +4 B | 1 appended param | A/B | S |
| T-e | **Sub-oscillator** (square one octave down, PolyBLEP, mixed before the filter) | Weight under the bass line; not a 303 feature (new Community OS behaviour) | ~25 instr hot x 2 passes = **~2k cycles (0.8 %)** | hot +~150 B, state +8 B | 1 appended param (Sub Level) | B | M |
| T-f | Square shape, accent sweep, pattern Slide | **Already present**: Waveform 1.0 is Open303's pulse; the accent's rc2 integrator is Open303's sweep model; Slide/Glide FX reach the module since MOD-ABI | | | | | |

T-a + T-b + T-c + T-d together: about 7 appended parameters (ids 9..15), fitting one new page of 8 (`MODS`, 4 chars) with no reorder; total hot cost ~1.6k cycles (0.6 %) on a voice that is already at 16 %: measure on the unit before merge, or land after Perf M2.

### 3.2 Chip

| # | Idea | Musically | CPU / block [est] | Bytes | Params / format / host | Risk | Size |
|---|---|---|---|---|---|---|---|
| H-a | **PWM**: PWM Depth (0..45 %), PWM Rate (0.1..20 Hz log), sweeping the duty around the Duty setting (the DC removal already follows any duty) | The C64 / Game Boy lead sound; the single most-missed chip feature | recompute `duty`/`dc` at control rate: ~10 instr per 32 samples = **~30 cycles** (0) | hot +~60 B, state +12 B | 2 appended params (ids 20, 21), on page 4 renamed `ARP/PWM` | B | S |
| H-b | **Filter** (12 dB SVF, LP/BP/HP labelled mode, Cutoff, Resonance; SID-flavoured) | Turns the three waves into bass and pads; a SID-like voice | ~12 instr hot = **~1k cycles (0.4 %)**; coefficients at control rate (host sin/cos: 4 calls a block, or a polynomial tan); bypass at mode OFF | hot +~200 B, state +32 B (`state_bytes` 208 -> 256, a manifest field, not persistent) | 3 appended params; with H-a that fills page 4 exactly (ids 17..24) | B | M |
| H-c | **Noise Mode** (PITCHED / FIXED, labelled) and **Arp Mode** (UP / DOWN / UP-DOWN / RANDOM, labelled) | Drums whose noise colour does not follow the note; the usual arp orders | 0 (control rate) | state +8 B | 2 appended params; competes with H-b for the last rows of page 4 | A/B | S |
| H-d | **Duty sequence** (NES-style: the pulse steps 12.5 -> 25 -> 50 at the arp rate) | Mega Man leads | 0 | | 1 param; same page pressure | B | S |
| H-e | Ring/sync second oscillator | Metallic SID timbres | ~3-5k cycles (second pulse at the same cost as the first) | hot +~300 B | 3+ params: no room left on page 4 after H-a/H-b | B | M |
| H-f | Wave sequences / macros, 4-bit wavetable, DPCM | What chiptune trackers do | | | **Not possible in this ABI**: no table or blob parameter, no sample data path to an instrument module | C (ABI) | L |
| H-g | Tempo-synced arp / PWM | Arps that land on the grid | 0 | | needs `tempo` in `o3m_host` (ABI minor, **Level C**); today the user converts BPM to Hz | C | M |

Page pressure: Chip can append at most **5** parameters (ids 20..24). H-a (2) + H-b (3) fills it; H-c/H-d then need a decision (or the ABI change in 3.4).

### 3.3 FM (all 32 parameters used)

| # | Idea | Musically | CPU / block [est] | Bytes | Params / format / host | Risk | Size |
|---|---|---|---|---|---|---|---|
| F-a | **More algorithms** (8 -> up to 16: the label list may hold 16; e.g. `4>(3+2)>1`, `4>3>(2+1)`, `(4>3)+(4>2)>1`, `4>(3>1)+2`, `(4+3)>(2+1)`) | Doubles the timbral map at no cost; the view already draws any routing | 0 (the weight matrix is per-sample already) | cold +~200 B (labels, table) | 0 new params: Algorithm `max` 8 -> 16, labels 16. Stored values 1..8 keep their meaning: **no format bump** (a range extension); `module_image` validation accepts 16 labels | B | S |
| F-b | **Detune +-1200 cents** (from +-100) | Inharmonic bells, metallic hits (ratio 1.41 = 1 + 600 cents) | 0 | 0 | 0 new params; range extension; a Learn binding on Detune now sweeps a wider range (note it) | B | S |
| F-c | **Per-operator velocity sensitivity** (4 params) | Velocity opens the sound: the core of expressive FM | ~0 (gain at note on) | state +16 B | **blocked by the 32 limit** | C (ABI) or new module | M |
| F-d | **LFO** (rate, pitch depth, amp depth, delay) | Vibrato and tremolo, the DX staple | ~100-300 cycles (control rate + one multiply per sample) | | 4 params: **blocked** | C or new module | M |
| F-e | **Pitch envelope** (depth, time) and **key scaling** (level by note, 1 global) | Drums, brass; even tone across the keyboard | ~0 | | 3 params: **blocked** | C or new module | S/M |
| F-f | **Operator waveforms** (OPL: sine, half, abs, quarter; labelled per op) | OPL/OPLL colours, cheap brightness | branchless select ~3 instr per op hot = **~1k cycles** | hot +~80 B | 4 params: **blocked** | C or new module | M |
| F-g | Fixed-frequency operators | Noise-like and percussive FM | 0 | | 4 bools: **blocked**; also redefines Ratio's meaning per op | C | M |
| F-h | Perf M3 (per-block envelopes, skip silent operators) and following `per_frame` in `voice_pitch` (F8) | -2..4 % and no glide zipper | | | PARKED (Perf M3); fold F8 into it | B | M |

### 3.4 The shared wall: 32 parameters, 4 pages, no host tempo

FM is at the ceiling; Chip has five rows left; TB-303 has room. The options, for the maintainer:

| Option | What | Cost / risk |
|---|---|---|
| W1 | **ABI minor: `O3M_PARAMS_MAX` 32 -> 64, `O3M_PAGES_MAX` 4 -> 8.** Left/Right already turn pages with wrap (`params_screen.cpp:749-780`); F1..F4 would show pages 1-4 and a Shift layer or a second press pages 5-8 (a UI decision). The blob limit is 256 B = 64 floats already (`kModuleParamBytes`), `O3M_EDIT_MAX_PARAM_WRITES` says "a module at most 32" (`o3module.h:502`), `APPM` says `params 1..32`. | Level C (published ABI, loader validation, image test, apps' `params` byte); S/M of code; unlocks F-c..F-g and H-c..H-e |
| W2 | **A second FM module** ("FM-X", new id, 48 params would still not fit: so this only helps with W1) | Flash +~9 KB per module at 75 % used; not useful alone |
| W3 | **Record the module version in `MODR`** (today name + ABI only) so a future module may migrate a blob | Level C format change, small (one u32 per module reference, INST unchanged); insurance before any module grows |
| W4 | **`tempo` in `o3m_host`** (BPM x 100, read at block start) | ABI minor, Level C, S; enables tempo-synced arp/PWM/LFO |

Recommendation: decide W1 (and W4 with it, one ABI minor) before any FM feature; TB-303 and Chip can take their append-only items now.

## 4. Bugs and hazards

| # | Where | What | Severity |
|---|---|---|---|
| B1 | `modules/chip/chip_dsp.h:109-114` with `:330-347` | `enter_release` from `env == 0` sets `target 0`, `slope > 0`; the loop's `if (e != t)` never runs, `advance` is never called, the stage stays `kRelease` and `voice_render` returns busy (1) until the next note: the governor counts a sounding voice and the loop renders zeros (~3k cycles a block). Reachable only by a `note_off` before the first rendered sample of the note (same dispatch pass). One line: `if (v.env <= 0) { v.stage = kOff; return; }`. | low (latent), A/B |
| B2 | `modules/fm/fm.cpp:93` vs `fm/module.json` labels | Two spellings of the algorithms (F1). | inconsistency |
| B3 | `modules/tb303/tb303.cpp:71`, `tests/tb303_view_test.cpp:112-121` | Views at 688/700 words: 12 words from a blank-and-quarantine. | latent |
| B4 | `modules/tb303/tb303_dsp.h:405-420` | Every `set_param` runs the full `derive()` (4 host calls, divisions) in the render context; a CC sweep is one a block, so bounded, but it is why no TB-303 parameter may carry `MOD` (cpu-budget M5). | recorded |
| B5 | `modules/tb303/tb303_dsp.h:38, 195, 206` | `kTiny` added per sample in every recursive filter: 6+ operations a sample for the host oracle; the `tmod-perf` skill says the FPv4-SP needs none. Guard to host builds in M2. | perf (known) |
| B6 | `modules/fm/fm_dsp.h:286-295` | A Glide/Slide `note_on` on a sounding voice keeps the old velocity (spec OS-MODPITCH hazard 1, recorded). | known |
| B7 | `modules/chip/chip.cpp:96-105` | NOISE LONG view starts flat (H4). | polish |
| B8 | READMEs | Stale statements, listed in section 5. | docs |
| B9 | `modules/fm/fm_dsp.h:313-320`, `tb303_dsp.h:447-455` | `per_frame` ignored: pitch staircase of one block (documented; FM zipper is an open unit check). | polish |

No NaN path found: TB-303 drops a NaN parameter (`tb303_dsp.h:402`), Chip and FM clamp it to `min` (`clampf` is NaN-to-lo), `bind()` filters NaN and +-1e30 (`module_voices.cpp:196`); the hot loops are bounded by `frames`; no division, no host call, no allocation inside the sample loops except the TB-303's one exp2 per kCtl (by design). Denormals: TB-303 biases; Chip's envelope is linear; FM's exponential envelopes stop at `kEnvFloor` 1e-4 (no denormal tail).

## 5. Stale or wrong documentation (all verified against code or the status docs)

`modules/tb303/README.md`
- "not yet run on a Tracker": wrong; measured 21.6 % (0.4.9) and 41.5-43.5k cycles (v0.6.4).
- "ABI 1.1 (it declares views)": the manifest is 1.8 (pitch, pages).
- "hot part 2.3 KB, cold 4.9 KB, 25 relocations, state 380 B (reserves 400)": cpu-budget 2026-10-16 says the hot part is 3,424 B; the rest to re-measure with `o3m_pack`.
- "a slide step FX is not wired to modules yet" and "The host passes it [legato] for a live key ... not for a pattern's notes": wrong since MOD-ABI; the code handles `O3M_NOTE_RAMP` (`tb303_dsp.h` `voice_note_on`, `voice_pitch`).
- "Cost (estimate, not a measurement): 32-38k cycles, 12-15 %": replace with the unit figures.
- The parameter table does not mention the two pages or `pitch: true`.

`modules/chip/README.md`
- "not yet run on a Tracker" and "Hardware check (not done)": the unit measured chip 2.8 % (0.4.9); `platform/mk66/tools/hil/modules.json` binds and sounds it; the tuner bug row used a chip triangle on the unit. By-ear checks of each wave remain open.
- "a legato note retriggers like any other": a host-ramped legato (Glide/Slide FX) keeps the envelope (`chip.cpp:27-31`).
- "instance 184 B (the manifest reserves 192)": the manifest says 208 since MOD-ABI; "hot part 2480 B": cpu-budget says 2,096 B after MOD-ABI.
- "Wave ... (shown as a number; the Wave view names it)": labels exist (`module.json:10`).
- Cost "about 4,800 cycles (1.8 %)": the unit says 2.8 %.

`modules/fm/README.md`
- "not yet run on a Tracker", "Hardware check (not done)": measured 11.7 % (0.4.9); HIL `modules.json` binds and sounds it.
- "about 16,000 cycles per block, 6 %": the unit says about twice that.
- "hot part 1904 B": cpu-budget says 1,952 B.
- Parameter table lacks the pages and the Algorithm labels; the view's algorithm names differ from the labels (F1).

`docs/manual/modules.md`: no chapter describes what the three sound modules' parameters do, the accent rule (velocity >= 100; a plain step is 96), the default sounds or the pages' contents; the manual is for owners who know the official firmware, and these engines are all new. `docs/qa/catalogue/modules.md` has rows for pages and labels (QA-MOD-105..107) and the pitch FX (099..104) but none for each module's sound by ear per wave / algorithm / accent (the Chip and FM READMEs' "Hardware check" lists are not catalogue rows).

## 6. Triage table (for the maintainer to copy and answer)

Value: H/M/L musical value. Cost: CPU per voice [est] / flash or hot bytes / risk level. "Agent can do" = no format change, Level A/B.

| # | Module | Proposal | Value | Cost (CPU / flash / risk) | Size | Recommendation | Maintainer: yes / no / later |
|---|---|---|---|---|---|---|---|
| 1 | FM | More algorithms: Algorithm max 8 -> up to 16, labels; the view draws any routing | H | 0 / +200 B cold / B | S | **IDEA, do first**: zero CPU, no format bump | |
| 2 | FM | Detune +-1200 cents (inharmonic ratios) | H | 0 / 0 / B | S | **IDEA, with #1** | |
| 3 | TB-303 | Drive (post-filter soft clip) | H | ~1.3k cycles (0.5 %) / +120 B hot / B | S | IDEA; measure on the unit (voice at 16 % already) | |
| 4 | TB-303 | Tune + Transpose + Accent threshold | M | 0 / +20 B / B | S | IDEA, append-only; bundle with #3 and #5 as one `MODS` page | |
| 5 | TB-303 | Devil Fish set: Filter Tracking, Accent Decay, Soft Attack | M/H | ~0.25k / +60 B / B | S/M | IDEA, same page as #3-#4 | |
| 6 | TB-303 | Sub-oscillator | M | ~2k cycles (0.8 %) / +150 B hot / B | M | later, after Perf M2 | |
| 7 | Chip | PWM Depth + Rate | H | ~0 / +60 B / B | S | **IDEA, do first for Chip**; page 4 renamed `ARP/PWM` | |
| 8 | Chip | SVF filter (mode, cutoff, res) | H | ~1k cycles (0.4 %) / +200 B hot, state 208 -> 256 / B | M | IDEA; fills Chip's last rows with #7 | |
| 9 | Chip | Noise Mode + Arp Mode (labelled) | M | 0 / +40 B / A-B | S | IDEA; competes with #8 for page-4 rows: decide which | |
| 10 | Chip | Ring/sync second oscillator, duty sequence | M | 3-5k / +300 B / B | M | later; no rows left without #13 | |
| 11 | FM | Per-op velocity, LFO, pitch envelope, key scaling, OPL waveforms | H | ~1-2k total / +300 B / C | M each | **blocked by 32 params**: only after #13 | |
| 12 | FM | Fold "follow `per_frame`" (no glide zipper) into Perf M3 | M | part of M3 / 0 / B | S | note on the M3 row | |
| 13 | ABI | `O3M_PARAMS_MAX` 64, `O3M_PAGES_MAX` 8, page keys past F4 (Left/Right exist) | H (enabler) | 0 / +1 KB / **C** | M | maintainer decision; prerequisite for #11 | |
| 14 | ABI | `tempo` in `o3m_host` (tempo-synced arp / PWM / LFO) | M | 0 / +100 B / **C** | S | later, same ABI minor as #13 | |
| 15 | Format | Module version in `MODR` (migration hook for future reorders) | M (insurance) | 0 / +50 B / **C** | S | maintainer decision; cheap now, costly later | |
| 16 | All | README refresh (section 5), FM label spelling (F1), TB-303 page split (T1), Chip Wave/Duty learnable (C3), Chip noise-view pre-roll (H4), FM Level unit (F4) | M | 0 / 0 / A-B | S | **agent can do (Level A/B)** | |
| 17 | Chip | `enter_release` at env 0 leaves the voice busy (B1) | L | 0 / 0 / A-B | S | **agent can do**, with a host test | |
| 18 | Docs | Manual chapter for the three sound modules + QA rows per module sound (by ear on the unit) | M | 0 / 0 / A | S/M | **agent can do (Level A)** | |
| 19 | Sim | Link the built-ins in the simulator so their pages and views render (E2E-7) | M (review and manual screenshots) | 0 / 0 / B | M | later; it is the E2E-7 slice already | |

## 7. Slice notes (fast mode, maintainer 2026-10-19: S1, then S3 reduced, then S2, then S6; one branch `feat/OS-SNDMOD`, one review at the end)

### S1 note (no id appended)

- B1: `chip::enter_release` with `env <= 0` sets `kOff` and returns (a note off before the first sample of the note has nothing to release). Host test: on + off with no render between, then the first render returns idle.
- F1: the Algorithm view's title reads the label table the descriptor already carries (`fm_labels_algorithm`, generated from `module.json`); the second spelling table `kNames` in `fm.cpp` is removed. Bytes go down.
- F4: FM's four `Level` rows lose the `percent` unit (a bare 0..100; the step is 0.75 dB, not a percentage). A descriptor change only; the stored values keep their meaning.
- T1: TB-303 pages are `SOUND` (ids 0..7: Waveform, Cutoff, Resonance, Env Mod, Decay, Accent, Slide, Volume) and `MODS` (from id 8: Legato Slide, then what S3 appends). The page table is not persistent, so no stored value moves.
- C3: Chip `Wave` and `Duty` become learnable (the `learnable: false` flag is dropped; a CC may now switch them). H4: the NOISE view runs the LFSR 200 steps (`chip::kNoisePreroll`) before it draws, so the long picture no longer starts with a flat run.
- READMEs refreshed from section 5 (stale statements only; unit figures from `cpu-budget.md`, nothing invented).
- Host cost: none of S1 touches a render path except B1 (a branch in `enter_release`, not per sample). Unit CPU arm: none needed.

### S3 note (reduced to S by the maintainer, 2026-10-19): TB-303 page MODS, ids 9..13 appended

| id | name | type, range, default | what it does | where it costs |
|---|---|---|---|---|
| 9 | Tune | int, -100..100 cents, 0 | the pitch of every following note, `note + Tune/100` semitones | note on: nothing (one add in the exp2 argument it already computes) |
| 10 | Transpose | int, -24..24 semitones, 0 | the same, whole semitones; the sum is held to notes 0..127 | note on: nothing |
| 11 | Acc Thresh | int, 1..127, 100 | the velocity from which a note is an accent (`velocity >= Acc Thresh`; today fixed at 100) | note on: one compare it already makes |
| 12 | Filter Track | float, 0..100 %, 0 | the cutoff follows the note: `cutoff x 2^((note - 48) / 12 x Track)`, 48 the reference note; the goal factor is computed at note start and by `pitch()` (one `exp2`, only when Track > 0); the render slews the factor toward it at each coefficient update with the pitch's own rate (`min(1, 4 x slide_d)`), so it follows the pitch during a slide (unit finding 2026-10-19: fixed at the note start the cutoff stepped the whole interval at a glide start), and multiplies the knob cutoff by it | note on / pitch: one exp2 if Track > 0; render: one fma per 4 samples (one per coefficient update), none at Track 0 beyond that |
| 13 | Acc Decay | float, 30..2000 ms (log), 200 | the filter decay of an accented note (today fixed at 200 ms, `main_d_acc`) | set_param: one `decay_rate` (no host call); render: none |

- In: the five above. Out, with the reason: **Drive** (maintainer: per-sample DSP); **Soft Attack** (Open303 as ported has no attack stage on the amplitude envelope: it steps to 1 at the trigger and the 200 Hz de-clicker rounds it, so a soft attack is a new per-sample ramp, which the reduced package excludes; it can come back with Perf M2). Accent Decay is in because the port already holds that time (`main_d_acc`, 200 ms) as a coefficient. Filter Track is in because the factor costs one `exp2` a note and one fma per coefficient update (it follows the pitch during a slide, unit finding 2026-10-19).
- Neutral values (Tune 0, Transpose 0, Acc Thresh 100, Filter Track 0, Acc Decay 200) give the same bits as before: `x + 0 + 0`, `knob x 1.0f`, `decay_rate(200)` are the old expressions. The oracle test runs unchanged on them; a new test pins that the voice with the five parameters at neutral equals a voice that never saw them, and each new parameter gets its own test.
- Page MODS = ids 8..13 (six rows). Existing projects: a blob of nine values leaves ids 9..13 at their defaults (`bind()`), so no stored sound changes. Range extensions later may only widen; Drive and Soft Attack, if they return, are appended after id 13.
- Not MOD targets (T6/B4: `set_param` stays off the modulation matrix). The five apply at the next note (a held note keeps the pitch, cutoff factor and accent it started with; Acc Decay applies to the next accented note).
- Bytes: the voice grows by 24 B (six words); `state_bytes` 400 to 432 (a manifest field, not persistent; the host build's voice is 16 B larger than the target's because of the four service pointers, and it must fit: 424 B). Hot part: one multiply a block. The QEMU test `module_builtin_run_test.cpp` hashes the voice's bytes (`kTb303Golden`): the layout change moves that constant, the audio-only hash does not move.
- Arena (SRAM_U, 4,096 B for the instruments' state blocks, `app_main.cpp` `kModuleStateBytes`): the state block grows 400 to 432 B, so the most the engine can hold, eight instances (`kMaxModuleInstances`), takes 3,456 B instead of 3,200 B. It still fits and nothing spills to SDRAM; the room left for other modules' states falls from 896 B to 640 B.
- Batch size: mk66 image of the whole batch (S1 to S6, `o3os_image` at 4 jobs) against origin/dev 19281d4, from the linker usage table: FLASH 819,460 -> 820,404 B (+944 B, 78.15 -> 78.24 %), SRAM_L 49,880 B, SRAM_U 138,120 B and SDRAM_STATIC 146,528 B unchanged (+0 B each: the module state arena is a fixed 4,096 B).
- Unit CPU arm (tester, later): a TB-303 on each of eight tracks (eight instruments: `kMaxModuleInstances` is 8) with Filter Track 100, Acc Thresh 1, Acc Decay 30, Tune +100, Transpose +24 against the same song at the defaults (`diag cpu`, `diag mod`): expected equal within noise, since nothing was added to the per-sample loop.
- Result (host, `tmod-perf` static check on `tb303.elf`): hot 3,424 -> 3,440 B, 909 -> 910 instructions in `.o3m_hot` (30 % FP load/store, 12 vdiv, 1 blx as before; the added multiply and its load), image 8,784 -> 9,584 B (cold: five table rows, five `set_param` cases, the note-start code), voice 384 -> 408 B on the target. Host replica of the QEMU script: the audio-only hash is the same with the old and the new voice. **QEMU follow-up for the machine that runs it:** `kTb303Golden` (voice bytes, tests/module_builtin_run_test.cpp) moves because the voice has 24 B more members (368 -> 392); take the new value from the `[tb303 golden]` log line. `kTb303AudioGolden` (0xf910ea28) must NOT move: if it does, the neutral sound changed.

### S2 note (no id appended; two ranges widen)

- **Algorithm** `max` 8 -> 16, `labels` 8 -> 16 (`O3M_LABELS_MAX` is 16). Stored values 1..8 keep their table rows (rows 1..8 are not touched), the default stays 5, still not learnable. The render loop is unchanged (it always evaluates 4, 3, 2, 1 with the full 6-link matrix), so an algorithm costs nothing; the Algorithm view draws any routing by depth.
- The eight new rows (a modulator has a higher number than its target; `src > dst`; carriers are heard): 9 `4>(3+2)>1` (4 -> 3, 4 -> 2, 3 -> 1, 2 -> 1; 1), 10 `4>3>1,2` (4 -> 3, 3 -> 1, 3 -> 2; 1, 2), 11 `3>1 + 4>2` (3 -> 1, 4 -> 2; 1, 2), 12 `4+3+2>1` (4, 3, 2 -> 1; 1), 13 `4>3>2, 4>1` (4 -> 3, 3 -> 2, 4 -> 1; 1, 2), 14 `(4+3)>(2+1)` (4, 3 -> 2; 4, 3 -> 1; 1, 2), 15 `(3>2)+4>1` (3 -> 2, 2 -> 1, 4 -> 1; 1), 16 `4>2>1 + 3` (4 -> 2, 2 -> 1; 1, 3). None equals one of 1..8.
- **Detune** (Op1..Op4, ids 1, 9, 17, 25) -100..100 -> -1200..1200 cents, whole numbers, default unchanged (0, 0, 4, 0). The code already took the descriptor's range and made `2^(cents/1200)`; a value stored today means the same. A MIDI Learn binding on Detune now sweeps 24 times the range, and the row's coarse step stays 10 cents (`describe_module_param`: span >= 40), so a 2400-step row is slow to turn by encoder: a follow-up for the UI core, not this slice. A MIDI Learn binding saved on Detune sweeps the wider range too (24 times the old one), and Relative mode counts more detents; the stored values themselves sound the same.
- Cost: render 0; the voice, the state (448 B in the manifest, as before) and the hot part do not change; cold +160 B measured (`tmod-perf` static check: hot 1,952 B and 530 instructions as before; image 8,848 -> 9,008 B).
- Tests: the table against the independent transcription of the diagrams for all 16, the double-precision model for all 16 with and without feedback and for Detune up to +-1200, the view's geometry for all 16, the descriptor (16 labels, the ranges).
- Batch size (S1 to S6 together, not S2 alone): see the S3 note: FLASH +944 B, SRAM_L, SRAM_U and SDRAM_STATIC +0 B against origin/dev 19281d4.
- Unit CPU arm: none needed beyond the FM figure that exists (the loop is the same); the tester listens to algorithms 9..16 and to a +-1200 Detune (a ratio 1.41 bell via +600 cents).

### S6 note (docs only)

- New manual chapter `docs/manual/module-sounds.md` (the new parameters only: TB-303 MODS page, FM 16 algorithms and wide Detune, Chip learnable Wave and Duty), registered in the index, `tools/manual_check.py` (CATALOGUE) and `tools/manual_llms.py` (ORDER); glossary entries Accent, Cent, Operator, Modulator, Algorithm, Detune; `modules.md` links to it. No screenshots (modules are not in the simulator, E2E-7) and no walk-through scenario.
- By-ear rows QA-MOD-151..156 (TB-303 filter, TB-303 accent and slide, Chip waves, Chip envelope/arp/sweep/vibrato, FM default patch and feedback, FM algorithms 1..8) with HIL steps in `qa_modules.json`; QA-MOD-140..150 came with S1..S3.
- Unit CPU arms the tester needs (none was run): S3 a TB-303 on each of eight tracks at the maxima against the defaults; S2 nothing beyond the existing FM figure.
