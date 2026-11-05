# The editing tools: why they feel poor and hard, and what to do

## Maintainer decisions (2026-11-05)

**Scope cut (maintainer, 2026-11-05, after the whole-backlog estimate):** S5, the live audible preview (OS-PTOOLS2-E, Q3), is dropped: the tools apply without a preview and undo stays the way back. The S5 text below is kept as the record of the design.

Approved: every recommendation of this note (Q1 F6 = REPEAT, INVERT into the menu; Q2 drop Fill SCOPE; Q3 preview as a real edit, after S3 is on the unit; Q4 the four new verbs native; Q5 TOOLS while playing except LIVE; Q6 tool settings for the session only; Q7 exports last; Q8 apps in the panel by a host rule first; Q9 no "F1 AGAIN" for project-editing apps, kept for file writers). Addition: the F7 menu also offers COPY PATTERN and PASTE PATTERN (today hidden behind F1 held, PAT-OPS-B), as tracker-ce's Pattern page 2 does.


Product/UX note for the maintainer, 2026-11-05. Read-only study of `dev` (7ba9c8d9) and of tracker-ce (CE, Polyend's MIT firmware). Facts about CE and the official firmware are described, not copied.

**Verdict in one line:** we have more tool *logic* than the official firmware (Euclid, scale fills, ramps, FX fills, length ops, five tool apps, a Song TOOLS dialog with TRANSPOSE/INSTRUMENT/CLEAR/FORM/BRIDGE), but on the Pattern screen it is scattered over four unrelated grammars, the most-used verbs (transpose, interpolate, nudge, randomise-in-place, humanise) are missing or hidden, nothing previews on the grid, and the two overlays reset themselves every time. The fix is one grammar, **select → F7 → verb**, with the Song TOOLS dialog's shape as the model, pads as shortcuts, a REPEAT key and a real (audible) preview.

---

## 1. Inventory: ours vs CE / official

"Where" is the key path on our Pattern screen. CE facts are from `core/interface/patternEditor/gPatternEdit.cpp` (the three F-key pages, lines 1436-1600; the fill lists, lines 37-67) and `fPatternEdit.cpp` (`changeSelectedStepValue`, line 3187; the fill pad handler, line 4650).

| Tool | Ours (Pattern) | CE / official | Gap |
|---|---|---|---|
| Select a block | Shift + arrows; column blocks; Shift+Up at row 1 = track column, again = all tracks (`bindings.cpp:484-489`, QA-PAT-007/319) | Shift + arrows; a column select mode | Parity. Ours is richer (column blocks). |
| Copy / paste block | Copy, Shift+Copy (`step_actions.cpp:386,416`) | Copy / Paste on page 2 (F6/F7 "Copy Pattern"/"Paste Pattern") and the hardware keys | Parity. No CUT (manual says "copy, then delete"). Song has CUT (Shift+Delete). |
| Clear / insert / pull | Delete, Insert, Shift+Delete (`step_actions.cpp:263,301,343`) | Delete | Ours richer. |
| **Transpose a block** | **Encoder on a block** (Shift = octave) via `pe::edit_selection` (`step_actions.cpp:118-129`, OS-BUG-R3SEL). No label, no dialog, not in the help text as a block verb (`help_text.cpp:150-153` says "change what the column holds"). Song has a TRANSPOSE dialog (QA-SNG-030); Pattern has none. | Jog on a multi-selection changes every selected note (Shift = octave), instrument, FX value, volume; Fx held + jog = FX type on all (`fPatternEdit.cpp:3187-3225`). Same hidden grammar. | **Exists but invisible.** The one verb every tracker user wants is unlabelled on both; CE at least makes the pad on a selection set all notes (ours does too, R3SEL). |
| Set instrument on a block | Encoder on the Instr column of a block; Song has INSTRUMENT (QA-SNG-031) | Jog on selection | Hidden, as above. No dialog on Pattern. |
| Fill | F4, 8-row dialog (SCOPE, WHAT, TYPE, N/EVENTS/DENSITY, MODE, KEY/FX TYPE, FROM/VALUE, TO), x-line preview, F1 FILL (`fill_actions.cpp:149`, `fill_view.h:16`) | F4 "Fill" popup: title = the cursor's column (Fill Notes / Instruments / Fx 1 / Fx 2 / Micro-move / Volume); "Where?" = Note / No Note / Fx / No Fx / Random / Each / Euclidean; Fill Type = Constant / From-To / Random; From, To, Density, scale filter Yes/No, Fx Type; **pads set the values**; settings kept per column kind (`strFill fillData[6]`) | Ours has more rules (SCALE, RAMP) but: no pads, settings reset on every open (QA-PAT-309, `fill_actions.cpp:167`), no "only where a note exists / no note" targets, SCOPE row duplicates the selection, repeat-last (`pattern.fill`) has no key since OS-QJUMP. |
| Invert (reverse) | F6 INVERT, a whole F-key (`step_actions.cpp:473`) | Page 2, F2 "Invert" | Parity. Ours spends a top-level key on a rare op. |
| Duplicate / expand / shrink | F7 TOOLS list rows 2-4 (`tools_list.h:41`) | Page 2, F3/F4/F5 "Duplicate / Expand / Shrink Pattern" | Parity in function. Ours is two levels deep, refused while playing (QA-PAT-161). |
| Whole-pattern copy | F1 held + Copy / Shift+Copy (QA-PAT-285) | Page 2 Copy / Paste Pattern | Parity. |
| Export selection / pattern | TOOLS rows 1 and 5 (`tools_list.h:41-42`) | Page 2, F1 "Render Selection" | Parity. Ours puts an export **first** in a list called TOOLS. |
| Interpolate (ramp between the block's end values) | — (FILL RAMP needs FROM/TO typed by hand) | — | **Missing on both.** Every PC tracker has it (Renoise/OpenMPT "interpolate", Ctrl+I). |
| Nudge / rotate block ±1 step | — (Insert / Shift+Delete at one track only) | — | Missing on both. |
| Randomise in place (±n on existing values, p % of steps) | — (FILL RANDOM overwrites every hit) | — | Missing on both. |
| Humanise | HUMANIZE **app**: leaves Pattern for the APP screen (`qa_mod_132_humanize.png`), 8 rows of which 5 are the target (PATTERN/TRACK/ROW/ROWS/TRACKS), "F1 AGAIN" confirm, F7+F8 exit | — (micro-move column, no tool) | Ours has it but 2 screens away. |
| Generators: Rhythm, Chords, Mutate, Morph | apps in TOOLS (`modules/apps/`), same path as above | — | Ours only. Good logic, poor path. |
| Pattern / track name | Song Shift+F6 NAME; track name: Mixer | Page 3, F1/F2 | n/a |
| Undo of a tool | one unit everywhere (QA-PAT-089/166/280/287) | Undo / Redo | **Ours is better.** Keep. |
| Repeat the last tool | `pattern.fill` is registered, **no key** (QA-PAT-090) | — | Missing on both. |
| Preview on the grid | x-line of hits in the Fill dialog only; the dialog covers 5 of 9 visible rows (`qa_pat_086.png`) | none | Missing on both. |

Other things the maintainer may mean by "tools": the Song F6 TOOLS dialog (`song_tools_overlay.cpp`: OP row + value rows + a scope footer + F1 APPLY / F8 BACK) is actually the **best-designed tool UI in the OS**; the app modules under `modules/apps/` (chords, humanize, morph, mutate, rhythmgen, tapcalc + the read-only ones); the Sample edit FX page. This note is about the Pattern and Song editing tools.

---

## 2. Diagnosis: why they feel poor and hard

**D1. Four grammars for one job.** On Pattern a block verb can be (a) a bare key (Delete, Insert, Copy, F6 INVERT), (b) the encoder on the block (transpose, instrument, volume, FX value: `step_actions.cpp:118-250`), (c) the F4 form, (d) the F7 list that opens a list, which opens an app on another screen. Song has a fifth: F6 TOOLS = OP + rows + scope footer. The user cannot form the model "select, then act": the same thing (transpose) is an unlabelled encoder turn on Pattern and a labelled dialog on Song (`song_map_actions.cpp:542-546`, QA-SNG-030).

**D2. The most-wanted verbs are missing or hidden.** Transpose / instrument on a block exist only as encoder turns that nothing on screen names (the info line shows `scale Chromatic C-3  track 1  STEP`, no block hint; `help_text.cpp:150-153` does not say the encoder acts on the block). Interpolate, nudge/rotate, randomise-in-place and humanise-without-leaving are absent. The ui-map's own verdict: "Pattern is the fullest screen... every key is taken. The next Pattern feature goes into a chooser or a dialog" (`docs/ui/ui-map.md:47`); but the dialog we have (TOOLS) holds one export and three length ops.

**D3. The Fill dialog is a form, not a tool.** Eight rows (the budget's maximum), three of which change meaning by TYPE or WHAT (N/EVENTS/DENSITY, KEY/FX TYPE, VALUE/TO; QA-PAT-308), MODE dim in two cases (QA-PAT-305). Walk cost of a volume fade on one track: F4, Down, Right, Right, Down, Down, Down, Right, F1 = 9 presses (QA-PAT-300 steps). On the next track: **all nine again**, because `fill_open` rebuilds FROM/TO from the cursor and the scope every open (`fill_actions.cpp:159-168`, QA-PAT-309) and the repeat action has no key. No pad sets a value (CE's fill does, `changneFillDataByPad`). The preview is a row of `x` under the rows, while the dialog hides the rows it would write (`qa_pat_086.png`).

**D4. The TOOLS list is an empty box with the wrong first item.** `qa_pat_180.png`: a 520×330 dialog with four rows, EXPORT PATTERN at the top (a spec contract, OS-WAVEXP §3.2, not a musician's priority), the cursor forced to row 0 on every open (`tools_actions.cpp:33`, `v.modules.tools = 0`), a "NO MODULE SYSTEM" / app list below a rule, no footer saying what the selected tool will do to what. It is refused while playing in WATCH (`tools_actions.cpp:24-26`, QA-PAT-161) although with a block selected the target is unambiguous; CE's Duplicate/Expand/Shrink are plain F-keys at any time.

**D5. Apps are two screens away and ask for their target twice.** TOOLS → app row → F1 → "EDITS THE PROJECT (UNDOABLE): F1 AGAIN" → the APP screen (the grid is gone, `qa_mod_132_humanize.png`) → five target rows the block already answered (PATTERN, TRACK, ROW, ROWS, TRACKS) → F1 APPLY → F7+F8 chord to come back. Humanise is a two-parameter operation (TIME, VOL) that should be a 2-row tool over the grid. The parked rows OS-RHYTHMUX / OS-BUG-APPEXIT recorded the same pain ("no preview or help text", "which app, physical buttons, chord timing"; BACKLOG lines 189-190).

**D6. No feedback before the write.** Every tool is apply-then-look-then-undo. Undo is excellent (one unit, named: QA-PAT-166) which is why it is survivable, but a transpose you cannot hear before committing, a fill you cannot see on the rows it will hit, and a randomise you cannot re-roll without a round trip (MUTATE has F2 VARY; Fill RANDOM does not) all feel "hard".

**D7. Discoverability on the key row.** F4 FILL, F6 INVERT, F7 TOOLS tell the user nothing about transpose, interpolate or humanise; the Shift row is 8/8 pad modes. The one place that could teach ("the info line while a block is active") shows nothing block-specific.

---

## 3. Proposal: one grammar, select → F7 → verb

### 3.1 The interaction model

1. **Select, then act.** The selection (block, column block, cursor step; Shift+Up at row 1 = the track, again = the pattern) is the target of *every* tool. No tool has a SCOPE row; the dialog footer states the target (`BLOCK  TRACK 3  ROWS 1-16`, as Song does).
2. **F7 TOOLS is the single verb menu.** Two columns × 8 rows (inside the 8-row budget), pads 1-16 pick a verb directly; **F7 held + pad runs the verb without a dialog** when it has no parameters (INVERT, INTERPOLATE, ROTATE, DUPLICATE, EXPAND, SHRINK, CLEAR) – the same held-key + pad grammar as F1 PATTERN. Tap F7: the menu stays, Up/Down/Left/Right/encoder move, F1 opens the verb. The menu remembers its last verb.
3. **One dialog shape for every verb with parameters**, the Song TOOLS shape (`song_tools_overlay.cpp`): OP row (Left/Right flips between verbs without leaving the dialog), ≤ 6 value rows, a footer with the target and the plan (`16 STEPS  +12 ST  C-2..B-3`), F1 APPLY, F2 VARY (random verbs: next seed, re-applied), F8 BACK. The dialog is a **right-hand panel** (x 500-800) so the block stays visible; the grid scrolls so the block's first track is leftmost.
4. **Live preview = a real edit inside an open undo unit.** Every value change re-applies the tool (undo the previous preview, apply the new one); APPLY closes the unit, BACK rolls it back. The sequencer already plays edits live, so a transpose or a Euclid fill is **heard while playing**. Previewed cells draw in the selection ink. Over the undo memory limit the dialog says `TOO BIG TO PREVIEW: F1 APPLIES BLIND` (reuses Song FORM's `TOO BIG TO UNDO` check).
5. **F6 = REPEAT**: the last verb with its last settings on the current target (a random verb draws again). The label reads the verb (`AGAIN FILL`). INVERT moves into the menu. (Maintainer decision Q1.)
6. **Pads inside a dialog = the row's value**: note pads on a note row (FROM/TO/VALUE), the 0..127 ramp on a value row (as Vol pads already do), the instrument board on an instrument row. Shift + encoder = the coarse step as today.
7. **The block hint**: while a block is active the info line reads `BLOCK 16 STEPS  ENC: TRANSPOSE  PAD: SET  DEL: CLEAR  F7: TOOLS` (Shift held: `ENC: OCTAVE`). This alone makes the hidden transpose visible; it costs one text item.
8. **Tools open while playing** in STEP and WATCH (the target is the block or the cursor, which are the user's); refused only in LIVE (`THE PLAYHEAD HOLDS THE CURSOR IN LIVE`).
9. **Settings persist for the session**, per verb (not per open). CONST fill still takes its VALUE from the cursor (that is "fill with this note"); everything else keeps the last setting.

### 3.2 The verb set (v1), in menu order = frequency while writing

| Verb | Rows (≤6) | Notes |
|---|---|---|
| TRANSPOSE | AMOUNT −24..+24 st (Shift: octave) | reuses `model::edit::note_delta` and the Song rules (`song_edit.cpp:66`); cut/fade/off untouched |
| INSTRUMENT | TO 1..64 | reuses Song INSTRUMENT (QA-SNG-031) |
| FILL | WHAT, TYPE, N/EVENTS/DENSITY, MODE, KEY/FX TYPE, FROM, TO (7) → **OP row + 7 = 8. Drop SCOPE.** | WHAT defaults to the cursor's column (CE does the same; "Fill Notes") |
| INTERPOLATE | none | the column of the block ramps from its first to its last non-empty value (VOL, FX value, INSTR; NOTE: chromatic ramp); whole-step block: VOL |
| RANDOMISE | AMOUNT ±0..64, CHANCE 0..100 %, SEED | in place: only steps that hold a value move; F2 VARY |
| HUMANISE | TIME 0..50 (micro-move FX), VOL ±0..32, SEED | the humanize app's logic as a native op (`modules/apps/humanize`); F2 VARY |
| ROTATE ▲ / ROTATE ▼ | none | the block's steps move one step up/down, wrapping inside the block (CE has none; every hardware sequencer has it) |
| INVERT | none | as today |
| DUPLICATE ×2, EXPAND ×2, SHRINK /2 | none | as today (`pattern_ops.h`) |
| CLEAR | WHAT: NOTES / VOL / FX 1 / FX 2 / ALL | parity with Song CLEAR; Delete stays the fast path |
| EXPORT SELECTION / PATTERN | none | moved to the end of the second column |
| APPS: RHYTHM GEN, CHORDS, MUTATE, MORPH, ... | the app's rows minus the target rows | slice S6: an app with ≤ 6 rows renders inside the panel, target = the block; bigger apps keep the APP screen |

### 3.3 Mockups (800×480, 5-track zoom)

**A. The verb menu (tap F7; pads 1-16 = the verbs; F7 held + pad = run)**

```
+------------------------------------------------------------------------------------------------+
| PAT 01/1 | BPM 130.0 | STOP | REC | LEN 064 JUMP 01 | INS 01 KICK                 CPU -- MEM --  |
| A 01 KICK      A 02 SNARE      A 03 HAT        | TOOLS           BLOCK  TRACK 1  ROWS 01-16 |
|------------------------------------------------|---------------------------------------------|
| 01 C-2 01 7F SLD2  ---       ---               |  1 TRANSPOSE     9 INVERT                   |
| 02 ---             ---       ---               |  2 INSTRUMENT   10 DUPLICATE x2             |
| 03 ---             ---       ---               |  3 FILL         11 EXPAND x2                |
| 04 ---             ---       ---               |  4 INTERPOLATE  12 SHRINK /2                |
| 05 C-2 01 73       ---       ---               | >5 RANDOMISE    13 CLEAR ...                |
| 06 ---             ---       ---               |  6 HUMANISE     14 EXPORT SELECTION         |
| 07 ---             ---       ---               |  7 ROTATE UP    15 EXPORT PATTERN           |
| 08 ---             ---       ---               |  8 ROTATE DOWN  16 APPS >  (5)              |
| 09 C-2 01 67       F#3 03 44 ---               |                                             |
|  ...  (the block is highlighted on the grid)   | PAD 1-16: PICK   F7+PAD: RUN                |
|                                                | F1 OPEN   F8 BACK                           |
|------------------------------------------------------------------------------------------------|
| BLOCK 16 STEPS  ENC: TRANSPOSE  PAD: SET  DEL: CLEAR     track 1  STEP           undo 11       |
| F1 OPEN  F2 -   F3 -   F4 FILL   F5 -   F6 AGAIN FILL   F7 TOOLS   F8 BACK                      |
+------------------------------------------------------------------------------------------------+
```

**B. A verb dialog (TRANSPOSE), live preview on the grid, audible while playing**

```
+------------------------------------------------------------------------------------------------+
| PAT 01/1 | BPM 130.0 | PLAY | REC | LEN 064 JUMP 01 | INS 01 KICK                 CPU -- MEM --  |
| A 01 KICK      A 02 SNARE      A 03 HAT        | TOOLS  <  TRANSPOSE  >                      |
|------------------------------------------------|---------------------------------------------|
| 01 [C-3]01 7F SLD2 ---       ---               |  AMOUNT          < +12 ST >                 |
| 02 ---             ---       ---               |                                             |
| 03 ---             ---       ---               |                                             |
| 04 ---             ---       ---               |                                             |
| 05 [C-3]01 73      ---       ---               |                                             |
| 06 ---             ---       ---               |  BLOCK  TRACK 1  ROWS 01-16                 |
| 07 ---             ---       ---               |  16 STEPS  5 NOTES  C-2..G-2 -> C-3..G-3    |
| 08 ---             ---       ---               |  PREVIEW: PLAYING THE CHANGE                |
| 09 [C-3]01 67      F#3 03 44 ---               |                                             |
|  ... [..] = previewed cells in the selection ink                                              |
|                                                | PADS: AMOUNT   SHIFT+ENC: OCTAVE            |
|                                                | F1 APPLY   F8 BACK (UNDOES THE PREVIEW)     |
|------------------------------------------------------------------------------------------------|
| TRANSPOSE: 16 STEPS +12 ST                                track 1  STEP           undo 12       |
| F1 APPLY  F2 -   F3 -   F4 -   F5 -   F6 -   F7 -   F8 BACK                                      |
+------------------------------------------------------------------------------------------------+
```

**C. FILL in the same shape (OP row + 7 rows = 8; the x-line becomes the grid itself)**

```
| TOOLS  <  FILL  >                           |
|  WHAT            < VOLUME >                 |
|  TYPE              EVERY N                  |
|  N                 1                        |
|  MODE              RAMP                     |
|  KEY               -                        |
|  FROM              127                      |
|  TO                0                        |
|  BLOCK  TRACK 1  ROWS 01-16   16 OF 16 HITS |
|  PADS: VALUE   F2 VARY (RANDOM)             |
|  F1 APPLY   F8 BACK                         |
```

Display-list cost: panel ≈ 12 text items + 1 rect + the F-row, inside the Fill dialog's current budget; the previewed cells replace, not add, grid items. RAM: no preview buffer (the undo journal is the buffer); the menu is a constexpr table.

### 3.4 What this removes

- The F4 FILL key stays (it opens the FILL verb directly: muscle memory, official parity). F6 INVERT becomes REPEAT (Q1). `pattern.fill` is retired for `tools.repeat`.
- The SCOPE row of Fill. The ToolsList overlay (replaced by the menu). The "F1 AGAIN" confirm for apps opened from TOOLS (the undo unit is the safety; keep the confirm for apps that write files).
- The app target rows (PATTERN, TRACK, ROW, ROWS, TRACKS) for apps launched from TOOLS: the block is the target (`O3M_LAUNCH_FROM_TOOLS` already carries it, `tools_actions.cpp:40-55`).

---

## 4. Slice plan

Format impact of every slice: **none** (session state; no `.o3pj`/`.o3st` change; Q6 keeps it so). All are Level B unless noted. Existing rows to reuse: PAT-OPS (done, the length ops), OS-H3A / OS-APPUX (done, the app path), OS-RHYTHMUX and OS-BUG-APPEXIT (PARKED, covered by S6), OS-SONGMAP (done, the dialog shape to reuse). New rows proposed: OS-PTOOLS2-A..F below.

| # | Size | Slice | Acceptance | Risk |
|---|---|---|---|---|
| S1 OS-PTOOLS2-A | S | **Quick wins, no new UI**: block hint on the info line; TOOLS remembers its row (`tools_actions.cpp:33`); TOOLS opens in STEP/WATCH while playing, refused only in LIVE; EXPORT rows last; Fill keeps its settings for the session (CONST VALUE still from the cursor); F2 VARY in Fill RANDOM re-draws. | Sim snaps: the hint with a block, the list reopens on the last row, `F7` while playing in WATCH with a block opens; `pattern_edit_app_test`: Fill settings survive BACK/open; QA-PAT rows updated (086, 160, 161, 309). | Low. OS-WAVEXP §3.2 said "EXPORT first": a documented reversal. |
| S2 OS-PTOOLS2-B | S/M | **Pattern TOOLS dialog = the Song TOOLS shape**: TRANSPOSE, INSTRUMENT, CLEAR as OPs on Pattern, reusing `SongOp`/`song_edit.cpp` rules and `song_tools_view.h`'s row model; target = the block or the cursor step; footer text. Still a centred dialog; INVERT/DUPLICATE/EXPAND/SHRINK/EXPORT stay in the list for now. | `TRANSPOSED: 16 STEPS +12 ST`, one undo unit, the Song clamp rules (QA-SNG-030) hold on Pattern (host test over the shared rule); sim snaps; manual `pattern_tools.md` section. | Low. Pure reuse. |
| S3 OS-PTOOLS2-C | M | **The verb menu and REPEAT**: F7 = 2×8 menu with pad picks and F7-held + pad run; all verbs of 3.2 that exist (TRANSPOSE, INSTRUMENT, FILL as an OP without SCOPE, INVERT, length ops, CLEAR, exports, APPS >); F6 = REPEAT (`tools.repeat`); the panel moves to the right side; the ToolsList overlay retired. | Every verb reachable in ≤ 3 presses from a block (F7, pad, F1) and in 2 with F7-held + pad; `shift_labels_test` and the bindings tests green; QA-PAT-160..167 rewritten; F6 label `AGAIN <VERB>`; `bindings.cpp` ToolsList context replaced. | Medium: touches the Pattern key row (Q1), the Fill dialog rows, OS-KEYROWDLG labels. Needs an ui-map amendment (§3 item 14, §5). |
| S4 OS-PTOOLS2-D | M | **The missing verbs**: INTERPOLATE, RANDOMISE (in place), ROTATE ▲/▼, HUMANISE (native port of `modules/apps/humanize` TIME/VOL logic; the .tmod stays on the card for the MODULES screen). Pure logic in `model/` with host tests (ramp endpoints, chance bounds, seed determinism, wrap). | Host tests per verb (as `pattern_fill_test`); each is one undo unit; flash delta recorded (estimate +2-3 KB for the four). | Low-medium: flash. HUMANISE must write the same micro-move FX the app does (no new FX type). |
| S5 OS-PTOOLS2-E | M/L, **Level C review** | **Live preview as an open undo unit**: value changes re-apply; APPLY commits, BACK rolls back; audible while playing; previewed cells in the selection ink; the size refusal. | Sim: a preview then BACK leaves the project byte-identical (round trip over `Editor` journal); playing during a preview never underruns (unit HIL: `o3hil` CPU line); undo depth after 50 value changes is 1 unit. | Highest: the undo path and the sequencer reading edits under the dialog (it already does for the encoder, so the mechanism is not new, the volume of edits is). Memory bound by the Song FORM check. Hardware check needed: no click when a previewed note changes under the playhead. |
| S6 OS-PTOOLS2-F | M | **Apps in the panel**: a tool app whose rows (minus the target rows) fit in 6 renders inside the TOOLS panel over the grid, target = the block, F1 APPLY / F2 VARY, no "F1 AGAIN"; larger apps keep the APP screen. ABI: a launch flag `O3M_LAUNCH_IN_PANEL` (minor bump, SDK doc) — or none if the host simply renders the first 6 descriptor rows. | RHYTHM GEN / HUMANIZE / CHORDS run from a block without leaving Pattern; QA-MOD-132 target rows gone for TOOLS launches; F7+F8 chord no longer needed from TOOLS. | Medium: module SDK is a published ABI (Level C if the flag is added). Covers the parked OS-RHYTHMUX / OS-BUG-APPEXIT. |

Order: S1 → S2 → S3 → S4 → S5 → S6. S1+S2 are one S batch (one lane); S3 is the M item; S4 follows as a second S batch; S5 and S6 are decided after the maintainer tries S3 on the unit. The manual chapter `docs/manual/pattern_tools.md` and the catalogue `docs/qa/catalogue/pattern.md` (QA-PAT-086..095, 160..167, 180, 210..213, 280..289, 300..309, 312) are rewritten at S3.

---

## 5. Open questions for the maintainer

| # | Question | Options | Recommendation |
|---|---|---|---|
| Q1 | F6 on Pattern: keep INVERT or become REPEAT (last tool, same settings, new random draw)? | a) keep INVERT, REPEAT only inside the menu (row 0 `AGAIN: FILL`); b) F6 = REPEAT, INVERT in the menu (pad 9 / F7-held + pad 9) | **b.** A repeat key is used ten times for every invert; the official muscle memory for F6 is weak (CE has Invert on its second page). |
| Q2 | Fill's SCOPE row: drop it (the selection is the scope; Shift+Up at row 1 = the track, again = the pattern) or keep it? | a) drop; b) keep and give Fill its own 8-row dialog outside the OP shape | **a.** One grammar; it is the only way FILL fits the OP row. |
| Q3 | Preview: a real edit inside an open undo unit (audible while playing, cells highlighted) or a visual-only hint (x-line / highlight without writing)? | a) real edit; b) highlight only (no sound, needs a scratch copy of the block); c) none (today) | **a**, as S5 after S3 is on the unit; the undo journal is the buffer, and hearing a transpose before APPLY is the single biggest "powerful" step. |
| Q4 | HUMANISE / RANDOMISE / INTERPOLATE / ROTATE native in flash (+2-3 KB est.) or as card apps only? | a) native; b) apps only (S6 makes them bearable) | **a.** They are one-line verbs; a card dependency for "nudge the block" is wrong. Generators (RHYTHM GEN, CHORDS, MUTATE, MORPH) stay apps. |
| Q5 | TOOLS while playing: allow in STEP and WATCH (block or cursor), refuse only in LIVE? | a) allow; b) keep "STOP PLAYING TO OPEN TOOLS" | **a.** Writing while the loop plays is the tracker workflow; the block is the user's in both modes (OS-BUG-WATCHSEL made it so). |
| Q6 | Tool settings: session only, or saved in `.o3st` (format bump, Level C)? | a) session; b) settings file | **a.** No format change; revisit if users ask. |
| Q7 | EXPORT PATTERN / SELECTION: stay in TOOLS at the end, or leave TOOLS for the F1-held PATTERN chooser (F1 held + F7?) | a) end of TOOLS; b) chooser | **a.** No free key; a musician writing does not export. |
| Q8 | Apps in the panel (S6): add an SDK launch flag (ABI minor, Level C) or render the first six descriptor rows with no ABI change? | a) flag; b) no ABI change, host rule | **b** first; a flag only if an app needs to know. |
| Q9 | The confirm "EDITS THE PROJECT (UNDOABLE): F1 AGAIN" for apps from TOOLS | a) keep; b) drop for project edits, keep for file writers | **b.** The undo unit is the safety; the confirm is friction on the hot path. |

Assumptions recorded: the display-list and the 8-row rule hold for a 2×8 menu (8 rows); the right-hand panel at 300 px leaves three tracks visible at the 5-track zoom; `pe::edit_selection` is the single write path every verb goes through (one undo unit each, as today).
