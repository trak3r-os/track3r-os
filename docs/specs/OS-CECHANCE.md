# OS-CECHANCE — Chance conditions beyond 0..100 (Group with Last, Not Last, Play/Skip cycles)

Status: specified 2026-11-05, decisions taken the same day (Q1 a, Q2 a, Q3 a, Q4 a, Q5 a), not implemented · Level C for C1 (the sequencer's row evaluation and the meaning of stored values), Level B for C2 (editor) · Backlog row OS-CECHANCE, approved by the maintainer 2026-11-05 · Facts from `dev` at c1dc3125 and from `tracker-ce@7fd6c2c` (Polyend's published firmware source, cited as facts; no code taken) · Order: C1 can merge on synthetic tests at any time; real tracker-ce projects reach it only after OS-CEPAT S1 (the v6 pattern reader), because today a v6 pattern fails to load (F11).

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 a, Q3 a, Q4 a, Q5 a (every recommended default).**

Only what the maintainer must decide. Every technical detail is decided in this spec and listed under Assumptions (section 6).

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | The probability rule for 0..100 | a) tracker-ce's: a draw in 0..99 below the value plays (0 never, 50 half, 100 always) · b) keep ours: a draw above the value cancels (0 plays 1 %, 99 and 100 always) | a | The same value means the same thing in both firmwares; 0 = never is what a user expects. Existing projects with Chance 1..99 play about 1 % less often; one host test that pins today's rule changes |
| Q2 | What a cancelled row still runs | a) tracker-ce's: nothing of the row (no note, no FX of either column) · b) ours: the note is dropped but the row's other FX still run | a | A step that "does not play" should not still change the tempo, send a CC or start a slide; parity for OG files |
| Q3 | Two Chance FX on one row, and Random value next to Chance | a) tracker-ce's: only the first Chance (column 2, then column 1) is evaluated; Random value never changes a Chance value · b) ours: both are evaluated (both must pass) and Random value randomises Chance too | a | Parity; with the conditions, a randomised Chance could become a cycle or a "with last" by accident |
| Q4 | What counts as one pass for Play N / Skip M | a) each track's own loop (its own length, play mode), plus a pattern restart that cuts it · b) the pattern's loop only, the same count for every track | a | Identical for every OG file (tracker-ce plays all tracks at track 1's length, F7); with our per-track lengths a cycle follows what the track itself repeats |
| Q5 | `.o3pj` and Chance 101..174 | a) no PATT version change: the values were stored verbatim and clamped at the point of use, so they get their meaning as an ADR 0004 amendment (the precedent of amendments 2 and 3); an older build plays them as 100 · b) a PATT minor bump | a | Nothing in the layout changes; an older build loads and plays the file (the step always plays); a bump would make older builds refuse projects for no data reason |

## 1. Goal

FX 4 Chance takes tracker-ce's conditions: 101 Group with Last (plays when the last condition of the track fired), 102 Not Last (when it did not), 103..138 Play N / Skip M and 139..174 Skip N / Play M (N, M 1..6, counted in passes of the pattern), so a project made in tracker-ce plays the same here, and the 0..100 rule matches tracker-ce's.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | Values: 0..100 probability; 101 Group with Last; 102 Not Last; 103..138 Play N / Skip M; 139..174 Skip N / Play M; max 174. For a cycle value v: i = v − 103 (or v − 139), N = 1 + i / 6, M = 1 + i % 6 | `tracker-ce@7fd6c2c:core/source/mtSequencer.h:260-269`; evaluation `core/source/mtSequencer.cpp:2185-2222` | verified (tracker-ce source) |
| F2 | Per-track state: a pass counter (starts 0) and the last result (starts true) | `mtSequencer.h:471-472`, reset `mtSequencer.cpp:2175-2183` | verified (tracker-ce source) |
| F3 | Evaluation: v ≤ 100 plays when a draw in 0..99 is below v (`core/source/util_math.cpp:45-50`), and stores the result as last; 101 returns last, 102 returns not last, neither stores; Play N / Skip M plays when pass % (N + M) < N, Skip N / Play M when pass % (N + M) ≥ N, both store last; no draw for 101..174 | `mtSequencer.cpp:2185-2222` | verified (tracker-ce source) |
| F4 | One Chance a row: the columns are scanned (the second column first, then the first); only the first Chance found is evaluated; a failure cancels the row; the column of a Chance FX gets its running FX killed | `mtSequencer.cpp:434-456` | verified (tracker-ce source) |
| F5 | A cancelled row runs none of its FX (the FX loop stops), arms no micro-move and starts no note; the Random value FX's draw is applied only in that FX loop, so it never changes a Chance | `mtSequencer.cpp:404-416`, `:494-501`, `:784`, `:797` | verified (tracker-ce source) |
| F6 | Resets: every start (play, play selection, record with pre-roll) and stop set pass 0 and last true; a switch to a different pattern index sets pass 0 and keeps last; the same index again counts on; a Perform track switching its own pattern sets that track's pass 0 | `mtSequencer.cpp:1321`, `:1364`, `:1591`, `:1657`, `:3313-3328`, `:3390` | verified (tracker-ce source) |
| F7 | A pass ends at each track's wrap: Forward at the end (also through Break pattern), Backward at the start, Random every pattern length of steps, the other modes at their turn; all tracks use track 1's length except Perform lengths | `mtSequencer.cpp:1701`, `:1768`, `:1801`, `:1822`, `:1846` | verified (tracker-ce source) |
| F8 | A muted track returns before the Chance evaluation (its last is not updated; its pass still counts) | `mtSequencer.cpp:389-390` | verified (tracker-ce source) |
| F9 | Ours today: Chance is `kChance = 4`, range 0..100 in the sequencer and the editor; `pre_fx` evaluates every Chance column (`rng_.range(0,100) > value` cancels: 0 plays 1 %, 99 and 100 always); Random value first randomises both columns, a Chance included; the row's other FX run even when cancelled | `seq/include/o3os/seq/fx.h:42`, `:154`; `model/include/o3os/model/fx_table.h:53`; `seq/src/sequencer.cpp:1820-1834`, `:1878-1884`; `seq/include/o3os/seq/rng.h` (range is [min, max)) | code fact |
| F10 | A stored value above 100 is clamped to 100 at the point of use (`fx_sanitize` in `read_row`), so it always plays; an encoder edit clamps it into 0..100; a save (`.o3pj` or OG Export) keeps it verbatim | `seq/include/o3os/seq/fx.h:235-247`; `seq/src/sequencer.cpp:1621-1622`; `model/src/step_edit.cpp:187` | code fact |
| F11 | tracker-ce saves patterns as v6; our reader fails a v6 file today (counted in `patterns_failed`), so a tracker-ce project's Chance conditions do not reach the model until OS-CEPAT S1. A v1..v5 file holding 101..174 loads verbatim (F10) | `docs/specs/OS-CEPAT.md` section 2.4; `formats/src/og_codec.h` (FX bytes copied) | code fact |
| F12 | tracker-ce's editor: a coarse turn walks 0..100, then 101, 102, 103, then +6 (and back); the random fill uses 0..100; the grid shows `GL`, `!L`, `n:m`, `Snm` | `core/source/mtSequencer_patternEdit.cpp:731-757`, `:1752`, `:1982-2020`; `core/interface/patternEditor/gPatternEdit.cpp:1748-1780` | verified (tracker-ce source) |

## 3. Design

### 3.1 State (per track, `TrackState`, `seq/include/o3os/seq/sequencer.h:355-406`)

`uint16_t ch_pass` (kept modulo 27,720, the least common multiple of 2..12, so every N + M cycle stays continuous forever), `bool ch_last` (true at start), `bool ch_pass_new` (a pass ended, applied at the next row), `uint8_t ch_pat` (the pattern id the count belongs to), `uint8_t ch_rnd` (rows since the last pass in Random mode). About 6 B a track, +96 B in the sequencer object.

### 3.2 Counting passes (Q4 a)

- `advance_tracks` sets `ch_pass_new` where it already raises `loop_end` (Forward, Backward, PingPong, `sequencer.cpp:1363-1404`). Random has no loop end: `ch_rnd` counts rows and sets `ch_pass_new` every `len` rows (F7, parity).
- A pattern restart (`load_tracks`, `sequencer.cpp:810`) also sets `ch_pass_new` and zeroes `ch_rnd`: a natural end, a Break pattern or a song step to the same pattern. When the track's own loop end falls on the same boundary the flag is set twice and counts once.
- `read_row` (`sequencer.cpp:1607`) applies it: if the track's pattern id differs from `ch_pat`, then `ch_pass = 0`, `ch_pass_new = false`, `ch_pat = id`, `ch_last` kept (F6: song advance to another pattern, live switch, order jump, a Perform track's own pattern landing or set); else if `ch_pass_new`, `ch_pass = (ch_pass + 1) % 27720`. One place covers every way a pattern changes.

### 3.3 Resets

`begin_run` (every start, a resume included) and `end_run`: `ch_pass = 0`, `ch_last = true`, flags cleared, `ch_pat` = the starting pattern (F6, parity; the resume is new Community OS behaviour). `locate` / `locate_row` (SPP, playhead jump): `ch_pass = 0`, `ch_last` kept (new Community OS behaviour: tracker-ce has no comparable locate).

### 3.4 Evaluation in `pre_fx` (`sequencer.cpp:1813`)

Before Random value and before any other FX of the row: find the first Chance (column 2, then column 1, Q3 a). Evaluate it on the raw stored value (fx_sanitize's clamp for type 4 moves to 174):

- v ≤ 100: one draw `d = rng_.range(0, 100)`; plays when `d < v` (Q1 a); `ch_last` = result.
- 101: plays when `ch_last`; 102: when `!ch_last`; `ch_last` unchanged; no draw.
- 103..138, 139..174: N, M from F1; plays by the pass rule of F3; `ch_last` = result; no draw.

The Chance column keeps `kill_roll` and `consumed`, as today. A failure sets `k.cancel`; with Q2 a, `pre_fx` then returns before the row's other FX, and Random value never touches a Chance column (Q3 a). Note handling of a cancelled row (no start, the previous note untouched) stays as today. The draw happens only for 0..100, so a project without Chance keeps its RNG sequence and its determinism hashes.

### 3.5 Editor (C2)

- Range: `kFxInfo[4]` max 174, `kRows[4]` hi 174 (`fx.h:154`, `fx_table.h:53`). Encoder ±1 walks every value; a coarse delta (|delta| > 1) follows F12 (parity).
- Pads map 0..47 to 0..100 and Fill RANDOM draws in 0..100 (F12 parity); Fill CONSTANT and FROM-TO accept 0..174. One helper `fx::random_hi(type)` (100 for Chance, else `hi`) serves the pads and Fill RANDOM. `fx_range` (`seq/src/sequencer.cpp:72-80`, Random value's range) is not changed: Random value skips a Chance column (C1).
- Info line (`append_fx_value`, `app/include/o3os/app/fx_text.h:56`): `CHANCE 50 %`, `CHANCE WITH LAST`, `CHANCE NOT LAST`, `CHANCE PLAY 2 SKIP 3`, `CHANCE SKIP 2 PLAY 3` (new Community OS behaviour: our own words). The grid keeps the type glyph and two hex digits (A6).

### 3.6 Formats (Q5 a)

No layout change: OG import and Export copy the value byte verbatim, as today; `.o3pj` keeps PATT 1.0. The meaning of 101..174 is recorded as the next ADR 0004 amendment (4 if OS-CEPAT's 3 has landed first). What official 1.9.x does with 101..174 in an exported file is an open research question (it is not in the 1.9 editor's range); the Export counts nothing for it.

## 4. Slices

| # | Slice | Size | Level | Flash [est] | Depends on |
|---|---|---|---|---|---|
| C1 | Sequencer: state, pass counting, resets, evaluation, Q1 to Q3 rules | S | C | +0.4 KiB | — |
| C2 | Editor range, coarse walk, pads, Fill, info text, QA rows, manual | S | B | +0.4 KiB | C1 |

**C1** (S, Level C). Files: `seq/include/o3os/seq/sequencer.h` (TrackState fields), `seq/src/sequencer.cpp` (`advance_tracks`, `load_tracks`, `read_row`, `pre_fx`, `begin_run`, `end_run`, `locate`), `seq/include/o3os/seq/fx.h` (`kFxInfo[4]` max 174), `docs/adr/0004-step-format.md` (amendment), `tests/seq_sequencer_test.cpp:396-418` (rule a: 0 never, 100 always, 50 within bounds), `tests/seq_chance_test.cpp` (new). Tests: every value 101..174 decodes to the right N, M; Play 1/Skip 1 alternates passes; Play 2/Skip 3 over 15 passes; Skip 3/Play 2; 101 and 102 after a 50 % step follow it and do not update last; last starts true (101 plays on the first row); a cancelled row runs none of its FX (a Tempo FX and a CC beside a failing Chance: no effect); two Chance columns: only column 2 counts; Random value beside a Chance leaves it; pass counting per track length (polymeter 8 against 16), Backward, PingPong, Random (every len rows); a repeated song slot counts on, a different pattern resets pass and keeps last, Perform track queue resets that track only; Stop and Play reset; locate resets; determinism hash unchanged for a project without Chance (`seq_determinism_test`, `seq_export_determinism_test` unchanged); a project with Chance 0..100 changes its hash once (re-pinned). SRAM +96 B; render CPU about 0 (a few comparisons on rows that hold Chance).

**C2** (S, Level B). Files: `model/include/o3os/model/fx_table.h` (hi 174, `random_hi`), `model/src/step_edit.cpp` (coarse walk, pad map), `model/src/pattern_fill.cpp:50-62` (RANDOM uses `random_hi`), `app/include/o3os/app/fx_text.h`, `docs/qa/catalogue/pattern.md`, `docs/manual/fx.md` (Chance section: the table of values), sim scenario `sim/scenarios/qa/qa_pattern.txt` › `qa_pat_330`..`qa_pat_334` (set 101, 102, 103, 174 and check the info line). Tests: `pattern_edit_rules_test.cpp` (±1 walk 100→101→…→174 and back; coarse walk F12; pad 47 = 100; Fill RANDOM never above 100; CONSTANT 140 kept), `fx_text` cases for each word form. QA rows QA-PAT-330..334: values 101/102 entry and text, cycle text, coarse walk, pads 0..100, playback of Play 1 / Skip 1 (sim run with a note count).

## 5. Unit check

None for C1/C2 beyond the batch's image smoke: a project with Play 1 / Skip 1 on a step of a 16-step pattern, played 4 passes, sounds on passes 1 and 3 (hands, any image after C2).

## 6. Assumptions (decided here; local, testable, reversible)

- A1. The pass counter is a `uint16_t` modulo 27,720 instead of tracker-ce's `uint32_t`; identical for the first 27,720 passes and continuous after (tracker-ce's wraps at 2^32, never reached).
- A2. A resume (MIDI Continue, the unit's Continue) resets like a start; a locate resets the pass and keeps last.
- A3. Muted tracks keep evaluating Chance (ours runs `pre_fx` on muted tracks and holds back notes later); tracker-ce skips them (F8). Only last differs after an unmute; recorded as a difference, new Community OS behaviour.
- A4. The Chance is evaluated on every row that holds it, with or without a note (F4).
- A5. `fx_sanitize` clamps Chance to 174; 175..255 play as 174 (Skip 6 / Play 6). tracker-ce's own range ends at 174 (F1).
- A6. The pattern grid keeps hex values (`AE` for 174); the info line gives the words. tracker-ce's grid labels are not copied.
- A7. Perform stutter loops count passes only where `advance_tracks` already raises `loop_end`; nothing new is added for them.

## 7. Risks

- R1 Changing the 0..100 rule changes how often existing Chance steps play (1 point a value) and re-pins one test; the release note says so.
- R2 Double counting at a boundary where the track loop and the pattern restart coincide: the flag design and a test at equal and unequal lengths.
- R3 A Chance moved to the front of `pre_fx` must not change the order of the other FX on rows without a failing Chance: the existing pfx tests stay green.

## 8. Evidence labels

- Values, decoding, evaluation, one Chance a row, cancel stops all FX, resets at start/stop/pattern change/Perform track switch: **verified parity** (F1 to F7, tracker-ce source).
- Pass by the track's own length (Q4 a) for tracks of different lengths, resume reset, locate reset, muted tracks evaluating, the info line words: **new Community OS behaviour**.
- What official 1.9.x plays for an exported 101..174: **open research question**.

## 9. State for implementer

**C1.** Start with: `Sequencer::pre_fx` `seq/src/sequencer.cpp:1813` (Random value at `:1820-1834`, the Chance case at `:1878-1884`); `Sequencer::advance_tracks` `:1359` (`loop_end` at `:1363-1404`); `Sequencer::read_row` `:1607` and `load_tracks` `:810`; `TrackState` `seq/include/o3os/seq/sequencer.h:355`. Rules: no allocation, no double; only values 0..100 draw from the RNG; a cancel returns from `pre_fx` before any other FX of the row but after `kill_roll`; `reset_tracks` (`:837`) must zero the new fields; Level C: risk review plus one independent review.

**C2.** Start with: `fx_value_delta` `model/src/step_edit.cpp:174`, `fx_pad` `:216`; `kRows` `model/include/o3os/model/fx_table.h:48`; `domain` `model/src/pattern_fill.cpp:50`; `append_fx_value` `app/include/o3os/app/fx_text.h:56`. Rules: the pad, Fill RANDOM and Random value domain stays 0..100; CONSTANT/FROM-TO and the encoder reach 174; texts fit the info line width used by Chord.
