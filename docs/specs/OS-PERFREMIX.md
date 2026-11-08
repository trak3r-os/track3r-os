# OS-PERFREMIX — Perform per-track remix

**Removed 2026-11-08 (OS-NOREMIX).** The maintainer removed the REMIX page (R2 and R3: the page, the remix board, its actions and bindings, its tests and scenarios) with "quitala, no la quiero". What stays: R1 (the sequencer's `SetTrackPattern`), the SLOTS Shift keys TRK PAT / TRK NEXT / TRK SONG (E-PERF-07), and Q5 a (leaving Perform for a screen but Master or Song sends every track that is not on the song back at once; now `PerformBoard::back` and `App::poll_perform`). The rest of this document is history; QA-PRF-220..224, 226, 228 and 229 are dropped.

Status: specified 2026-11-05, decisions taken the same day (Q1 a, Q2 a, Q3 a, Q4 a, Q5 a), not implemented · Level C for R1 (a sequencer command, the render's state); B for the UI; A for the docs · Backlog row "Perform per-track remix", approved by the maintainer 2026-11-05 (IDEA triage; the ID OS-PERFREMIX is given here) · Facts from `dev` at c1dc3125 and from `polyend/tracker-ce` at 7fd6c2c (cited `tracker-ce@7fd6c2c:<path>:<line>`, paths under `core/`; a read-only clone, untracked) · The gestures (hold a track, turn for a change at once, an arrow for a change at the track's loop end) are **verified parity**; that the tracks are the pads' columns, the REMIX page and the rules for Song, Stop and leaving are **new Community OS behaviour** unless the table says otherwise.

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 a, Q3 a, Q4 a, Q5 a (every recommended default).**

Only what the maintainer must decide. Every technical detail is decided here and listed as an assumption (section 5).

| # | Question | Options | Recommended default | Why |
|---|---|---|---|---|
| Q1 | Where the remix is played | a) a third Perform page, REMIX (the Performance button again: SLOTS, DJ, REMIX): hold a track's pad (its column), turn the encoder for a change at once, Left / Right for one at the track's loop end · b) on the SLOTS page: a pad of the TRACKS layer (lone Shift) held + the encoder · c) on the SLOTS page with E-PERF-07's Shift keys plus a NOW key, no hold | a | The official remix is a hold of a track key (F1..F8, F1); our F-keys are scenes and our tracks are the pads' columns, so a page whose pads are the twelve tracks gives the same gesture without taking anything from the slots; b collides with the TRACKS layer, which yields to any other control (`app/include/o3os/app/pad_layer.h:61-80`); c has no per-track hold |
| Q2 | A new Song slot | a) a track on another pattern stays on it (E-PERF-07's rule today: "the pick pins it") · b) as the official firmware: each slot returns every track to the song (F9) | a | The maintainer approved E-PERF-07's pin (2026-10-23); a remix that survives the song is what the per-track hold is for; b makes the remix useless in Song mode beyond one slot |
| Q3 | Stop | a) Stop ends the remix (today's rule) and the page says NOT PLAYING while stopped · b) as the official firmware: a waiting change lands at the Stop, the remix stays for the next Play until Perform is left, and the page works while stopped (F10) | a | Our sequencer drops every per-track queue and override at a stop (E-PERF-07, tested); b would change E-PERF-07, add a stopped path to the sequencer and an extra state the screens must show |
| Q4 | E-PERF-07's Shift + F4..F8 on the SLOTS page (PICK -, PICK +, TRK PAT, TRK NEXT, TRK SONG) | a) keep them (they act on the AFFECTED tracks, the remix on the held ones) · b) remove them for the REMIX page | a | Merged, documented, in the QA catalogue; TRK NEXT and the AFFECTED mask have no REMIX equivalent; removing them costs a manual section and six QA rows for 0.3 KiB |
| Q5 | Leaving Perform (any screen but Master and Song) | a) every track on another pattern, or waiting for one, goes back to the song at once, as the FX end (F7: the official firmware ends the remix there) · b) at each track's own loop end · c) nothing (today: an E-PERF-07 override survives until Stop, H10) | a | The Pattern screen then shows what plays; the FX already end at once there; the same for both ways of remixing (E-PERF-07's included) |

## 1. Goal

On Perform, any of the twelve tracks plays another pattern's track: hold the track's pad on the REMIX page and turn the encoder, the track switches at once and keeps its place; or press Left / Right and let go, the track switches at its own loop end and blinks until then. The page shows, for each track, the pattern it plays (the song's, dim; another, bright) and the one it waits for. Live only, not saved, as E-PERF-07.

## 2. Facts

The official firmware (tracker-ce; paths under `core/`):

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | The keys F1..F8 of the Perform screen are tracks (two pages of eight; Shift + a key mutes the track); a tap toggles whether the layer affects the track | `tracker-ce@7fd6c2c:core/interface/performanceMode/fPerformance.cpp:687-741`, `:659-680` | verified (tracker-ce) |
| F2 | The encoder with track keys held: each held track's pattern number moves by the turn, clamped to 1..255, and the track switches **at once**; Up / Down with a key held: the same by 1 | `fPerformance.cpp:335-359`, `:361-388`, `:512-567` | verified |
| F3 | Left / Right with track keys held change the number by 1 without switching; the key's release then queues the shown number for the track's **own loop end**, and the number blinks until the switch. After a turn of the same hold the arrows do not act on the track; a release after a turn sends nothing | `fPerformance.cpp:390-420`, `:422-510`, `:687-741`, `:745-792` | verified |
| F4 | A switch at once takes the other pattern's track and its length, cancels no FX, keeps the track's place, and a track past its new length rejoins at the global position modulo that length | `core/source/mtSequencer.cpp:3196-3208`, `:3379-3398` | verified |
| F5 | A queued switch lands at the track's own loop end (its wrap in any play mode) and cancels the track's FX | `mtSequencer.cpp:1738-1873` | verified |
| F6 | The screen shows "Patt: N" under each track key's label | `core/interface/performanceMode/gPerformance.cpp:266-283` | verified |
| F7 | Entering Perform sets every track's number to the pattern that plays; leaving it ends the FX and, when one was on, reloads the pattern: the remix ends. Perform stays on across the Performance, Master and Params buttons | `fPerformance.cpp:79-91`, `:155-171`, `:261-265` | verified |
| F8 | Perform turned off at the pattern's end (Pattern mode) forces a reload of the pattern to clear the changed tracks: the remix ends with the mode | `fPerformance.cpp:246-259` | verified |
| F9 | Song mode: the next slot loads the whole next pattern, every track, and resets the tracks' lengths: the remix ends at each slot | `mtSequencer.cpp:1896-1910` | verified |
| F10 | Stop with Perform on: a queued switch lands at once; the remixed tracks stay in the playing pattern | `mtSequencer.cpp:1605-1625` | verified |
| F11 | Shift + Play while stopped plays the song and sets every track's number to the pattern | `fPerformance.cpp:569-598` | verified |
| F12 | A MIDI CC per track queues pattern CC + 1 for that track | `fPerformance.cpp:309-325` | verified |

Ours (`dev` c1dc3125):

| # | Fact | Where |
|---|---|---|
| H1 | `Command::QueueTrackPattern` (b pattern 0..255, 0 = the song's; c track mask) → `queue_track_pattern`: lands at each track's own loop end, cancels its FX there (`land_track_queue`); the same queue again takes it back; the pattern a track already follows pins it at once; ignored while stopped; Stop, a restart and a resume end every queue and override; a track that cannot reach its loop end (Random, or longer than the pattern) lands at the pattern's end | `seq/include/o3os/seq/sequencer.h:148-176`, `:303-309`; `seq/src/sequencer.cpp:493-498`, `:590-615`, `:842-851`, `:1569-1574` |
| H2 | `perf_q` makes a track's pattern fields QueueTrackPattern's: `set_perf_track` (the PLAYMODE FX) leaves them alone | `seq/src/sequencer.cpp:567-588` |
| H3 | `load_tracks` and `read_row` take the source pattern `perf_pattern` (0: the song's) and the track's length from it every step; a track past its length goes to step 0 | `seq/src/sequencer.cpp:823-841`, `:1662-1668` |
| H4 | The word published per track: `perf_pattern | perf_pending << 8 | 0x10000` while it waits; the App reads it once a pass into `ViewState::track_pat` (0 while stopped) and blinks while a track waits | `seq/src/sequencer.cpp:1140-1144`; `app/src/app.cpp:641-649`; `app/include/o3os/app/view_state.h:243` |
| H5 | E-PERF-07's keys on the SLOTS page: Shift + F4 / F5 PICK, F6 TRK PAT, F7 TRK NEXT, F8 TRK SONG on the AFFECTED tracks; hints NOT PLAYING, NO TRACK IS ON, PATTERN IS GONE, COMMANDS BUSY | `app/include/o3os/app/screens/perform_screen.h:12-14`; `app/src/perform_actions.cpp:99-144`, `:215-272`; `app/src/bindings.cpp:831-835` |
| H6 | The twelve track boxes under the slots show "07" or ">07" (blinking) for a track that is ON and not muted | `app/src/screens/perform_screen.cpp:442-476` |
| H7 | Perform has two pages (SLOTS 0, DJ 1); its button again cycles them; the DJ page is a field context (`PerformDj` = 46); the context ids in use run to 49 | `perform_screen.h:42-45`; `app/include/o3os/app/screens/screen.h:93-106`; `app/src/screens/perform_screen.cpp:291`; `app/include/o3os/app/dj.h:70`; `app/include/o3os/app/bindings.h:63-70` |
| H8 | A lone Shift hold turns the pads into the TRACKS layer (AFFECTED, MUTE, MUTE MODE, SOLO); its pads' releases are kept from the screen | `app/include/o3os/app/pad_layer.h:17-20`, `:61-80`; `app/src/app.cpp:174-183` |
| H9 | Every pad release on the Perform screen goes to `perform.pad_up` → `PerformBank::pad_up`, which reloads the slot of the pad's column and, LATCH off, may end it — also on the DJ page, where the press never reached the bank (**latent issue**, fixed by R2) | `app/src/app.cpp:226-232`; `seq/src/perform.cpp:254-271` |
| H10 | Leaving Perform for any screen but Master and Song turns the bank off (every FX ends); it touches no E-PERF-07 override (H2), which survives until Stop | `app/src/app.cpp:2459-2488` |

## 3. Design

### 3.1 The sequencer: a change at once (R1)

- `Command::Kind::SetTrackPattern` (appended at the end of `Command::Kind`, after `MtcAnchor`: the kinds' numbers stay): b pattern 0..255 (0: the song's), c track mask. `Sequencer::set_track_pattern(mask, pattern)`, drained with the other commands at the block's start, so it applies at the next step boundary: the track's next row is the new pattern's.
- For each track of the mask, while playing: `perf_pattern = pattern`, `perf_pending_set = false` (a change at once replaces a wait), `perf_q = pattern != 0` (H2: PLAYMODE keeps off its pattern). No FX cancel (F4). Ignored while stopped (as H1). Stop, a restart and a resume end it like a landed override (H1, `end_run`).
- The place (F4): the track keeps `pos`; when the new source's track is shorter than `pos + 1`, the track's next step is the song's next step modulo the new length (not step 0 as H3's edit rule). A custom play order (OS-PERFMODES, `ord`) is kept as it is. A pattern that does not exist reads as today's missing pattern (the App never sends one: 3.2).
- Cost: a command, a loop over 16 tracks; nothing a step.

### 3.2 The remix board (R2)

A pure state machine, `app/include/o3os/app/remix.h` (`struct RemixBoard`, in `PerformBoard`), main loop only, live, never saved:

- `held` (12 bits: the tracks whose pad is down, any pad of the column), per track `cand` (the number shown while held) and `how` (None, Now, Next).
- `pad_down(t, now)`: the track is held; `cand` = the pattern it waits for, else the one it plays (`now`: H4's word, or the song's pattern); `how` None.
- `turn(delta, patterns)` (encoder; Up / Down = +1 / -1): for each held track, `how` = Now and `cand` walks `delta` existing patterns from where it is, clamped at the first and last (A-3); the track is marked to be sent at once. False when nothing is held: the hint "HOLD A TRACK'S PAD".
- `arrow(dir, patterns)` (Left / Right): for each held track whose `how` is not Now, `how` = Next and `cand` walks one existing pattern; nothing is sent (F3).
- `pad_up(t)`: Next and `cand` differs from what the track plays and from what it waits for → one `QueueTrackPattern(cand, 1 << t)` (the toggle rule of H1 would take a same-pattern wait back, so the same value sends nothing). Now or None: nothing (a tap does nothing: A-4).
- `flush(sink)`, once a pass from `App::poll_perform`: the marked tracks go out as `SetTrackPattern`, one command per distinct pattern with the tracks' mask; a refused push stays marked for the next pass (a turn of many detents sends the last value only).
- Keys while no pad is held: F1 SONG queues 0 for every track on another pattern or waiting (`QueueTrackPattern(0, mask)`, but not for a track that already waits for 0: the toggle rule); F2 SONG NOW sends `SetTrackPattern(0, mask)`. With pads held, F1 / F2 act on the held tracks only.
- Stopped: every action says NOT PLAYING and sends nothing (Q3 a). A page change or a screen change forgets the holds and sends nothing.
- Leaving Perform (Q5 a): where `poll_perform` turns the bank off, the board marks every track whose word is not 0 for `SetTrackPattern(0)` (flushed the same pass, retried when refused). This also ends E-PERF-07's overrides there.
- App routing: a pad release on Perform goes to `perform.pad_up` only when `PerformBoard::pads` has its bit (the press went to the bank): this fixes H9 for the DJ and REMIX pages; a pad release goes to `remix.pad_up` when the board holds its column (any page or screen: the press was the board's), never for a pad the TRACKS layer took (H8).

### 3.3 The REMIX page (R3)

- `PerformScreen::pages()` 3, `kRemixPageIndex = 2`, `ViewState::remix_page()`, `Context::PerformRemix` (the next free id, 50 at c1dc3125), the screen's field context on that page (as DJ). Bindings: the 48 pads → `remix.pad`; the encoder and Up / Down → `remix.turn`; Left / Right → `remix.arrow`; F1 SONG, F2 SONG NOW; F3..F8 "-" (and the slots' keys must do nothing on this page, as on DJ). The lone Shift stays the TRACKS layer (mute, solo while remixing).
- Soft keys: `{"SONG", "SONG NOW", "-", "-", "-", "-", "-", "-"}`; Shift row: all "-".
- Render: twelve columns (the slots' grid); each shows T1..T12, the pattern the track plays (two digits; bright when it is not the song's, dim when it follows the song), under it the pattern it waits for (">09", ">SG" for the song's) blinking with `view.blink` (H4), while held the candidate in Accent, MUTED in Accent (as H6). The title strip says REMIX; the info line: "HOLD A PAD: TURN = NOW, LEFT / RIGHT = AT ITS LOOP END". Stopped: every track shows the pattern in view, dim, and the info line "PLAY TO REMIX".
- Pad lights: row 1 of a column Front when the track plays another pattern, Back when it follows the song; the held columns all Front; rows 2-4 Off otherwise.
- Help (`app/src/help_text.cpp`), `tools/check_keytext.py` for the key names.

### 3.4 Song, Pattern, Stop (the rules in one place)

- Pattern mode: the remix holds while the pattern loops. Song mode: a track that follows the song follows each slot; a track on another pattern stays on it (Q2 a); NEXT PAT and TRK NEXT keep their meaning.
- Stop: every queue and override ends (H1); the page is inert (Q3 a). Play starts from the song.
- Leaving Perform for Master or Song keeps everything (as the FX); for any other screen every track goes back to the song at once (Q5 a). SLOTS, DJ and REMIX are pages of one screen: moving between them keeps the remix.
- E-PERF-07's Shift keys keep acting on the AFFECTED tracks (Q4 a); both ways write the same per-track state, so the REMIX page shows what either did.

### 3.5 Real time and budgets

R1 runs in the render context: one more command kind, a bounded loop, no allocation; the step path gains nothing. Flash about +4.6 KiB in all [est] (R1 0.3, R2 1.4, R3 2.5, R4 0.4 help text); SRAM about +40 B (`RemixBoard` in the main loop's `PerformBoard`) [est]; `.ramfunc_l` 0 B.

## 4. Slices

| # | Slice | Size | Level | Flash [est] | Depends on |
|---|---|---|---|---|---|
| R1 | `SetTrackPattern`: a track's pattern at once | S | C | +0.3 KiB | — |
| R2 | The remix board, its actions, the pad routing (H9 fixed), leaving Perform | S | B | +1.4 KiB, SRAM +40 B | R1 |
| R3 | The REMIX page: context, bindings, render, pad lights, sim, QA rows | S | B | +2.5 KiB | R2 |
| R4 | The manual, the help page, the maps | S | A | +0.4 KiB | R3 |

**R1 — a track's pattern at once** (S, Level C: the render's state). Files: `seq/include/o3os/seq/sequencer.h` (the kind, `set_track_pattern`, its comment), `seq/src/sequencer.cpp` (drain, `set_track_pattern`, the rejoin rule in `read_row`), `tests/seq_sequencer_test.cpp` (beside the E-PERF-07 block). Acceptance: 3.1. Host tests: track 2 of a 16-step pattern switched to pattern 3 on step 5 plays pattern 3's row 6 next (the place kept) and its FX go on (no cancel); a switch to an 8-step track on step 12 plays step `(12 + 1) % 8` next; a waiting queue is replaced; pattern 0 sends the track back to the song at once; a PLAYMODE `PerfTrack` afterwards keeps the override (H2); ignored while stopped; Stop ends it; the published word shows it with no wait bit; two offline renders identical. QA rows: none (no key yet). Unit check: none of its own (R3's).

**R2 — the remix board** (S, Level B). Files: `app/include/o3os/app/remix.h`, `app/src/remix.cpp` (new), `app/include/o3os/app/perform_board.h` (the member), `app/src/perform_actions.cpp` (`remix.pad`, `remix.pad_up`, `remix.turn`, `remix.arrow`, `remix.key`, registered, not yet bound), `app/src/app.cpp` (the two release routes of 3.2, the leave in `poll_perform`, `flush` once a pass), `tests/app_remix_test.cpp` (new), `tests/perform_screen_test.cpp` (H9). Acceptance: 3.2. Host tests: the board alone (hold, turn 3 then -1 sends the last value once; arrow then release queues; arrow to the waited pattern sends nothing; turn then arrow then release sends nothing more; a tap sends nothing; two held tracks on different patterns each walk from their own; the walk skips missing patterns and stops at the ends; a refused push is sent the next pass; F1 skips a track that waits for 0); through the App with a recording sink (the pad release on the DJ page no longer reaches the bank: a LATCH-off slot stays; a TRACKS layer pad never reaches the board; leaving Perform for Pattern sends `SetTrackPattern(0)` for the tracks on another pattern, for Master sends nothing). QA rows: none (R3). Manual: none.

**R3 — the REMIX page** (S, Level B). Files: `app/include/o3os/app/screens/perform_screen.h` (pages, keys), `app/include/o3os/app/view_state.h` (`remix_page()`), `app/include/o3os/app/bindings.h` (`PerformRemix`), `app/src/bindings.cpp`, `app/src/screens/perform_screen.cpp` (field context, shift row, pad roles, render), `app/src/perform_actions.cpp` (the slots' keys inert on the page), `sim/scenarios/qa/qa_perform_remix.txt` (new) and its goldens, `docs/qa/catalogue/perform.md`, `docs/ui/` map row of Perform's pages. Acceptance: 3.3; the Performance button cycles SLOTS, DJ, REMIX; Rec on the page does nothing. Goldens: the stopped page (PLAY TO REMIX) and the Shift row. The simulator has no sequencer for Perform, so the playing states (a held candidate, a wait, an override, MUTED) are host-tested in `tests/perform_screen_test.cpp` with a `ViewState` whose `play.playing` and `track_pat` are set, as the E-PERF-07 boxes are. QA rows QA-PRF-220..227: 220 the page is reached and shows twelve tracks; 221 hold + turn switches at once, the place kept; 222 hold + Right + release switches at the loop end, blinking until then; 223 Right to the pattern already waited for sends nothing; 224 SONG and SONG NOW; 225 a Song slot keeps the remix; 226 Stop ends it, the page says PLAY TO REMIX; 227 leaving for Pattern ends it, Master keeps it. Unit check: QA-PRF-221 / 222 by ear on a two-pattern project (a drum track switched to a fill at once and at the loop end), plus 225-227.

**R4 — the docs** (S, Level A). Files: `docs/manual/perform.md` ("Give single tracks another pattern": the REMIX page first, E-PERF-07's keys after; the screen-at-a-glance and keys tables), `app/src/help_text.cpp` (a REMIX paragraph), `docs/ui/official-manual-map.md` (rows 390 and 558: "same" for the gestures, "different" for the pads and Song), the manual's LLM files (`tools/manual_llms.py`). QA rows: QA-PRF-228 (the help page's REMIX paragraph), 229 (the manual's task followed on the unit). If R3 stays under 400 lines, R4 may fold into it.

## 5. Assumptions (decided here; each local, testable and reversible)

- A-1 The tracks are the pads' twelve columns (any of the four pads holds the column's track); several may be held; tracks 13-16 are not reachable (as DJ, `kDjTracks`). The official keys are F1..F8 over two pages (F1).
- A-2 A change at once applies at the next step boundary (the commands' rule), not inside a step.
- A-3 The walk visits only patterns that exist and stops at the first and last (the official clamps to 1..255, F2): no silent empty pattern by accident.
- A-4 A tap of a pad does nothing (the official tap toggles AFFECTED, F1: ours is the TRACKS layer's).
- A-5 Nothing is saved, nothing is restored on entering (the official resets the numbers on entering, F7; ours start from what plays, which is the same while nothing is overridden).
- A-6 The MIDI CC per track (F12) belongs to the backlog row of the official MIDI CC map, not here.
- A-7 E-PERF-07's PICK value and the board's candidates are separate.
- A-8 After a turn the arrows do nothing for that hold (F3); after an arrow a turn becomes a change at once (Now wins; nothing queued at the release).

## 6. Risks

- R-1 **The latent pad release** (H9): R2 gates it; a SLOTS pad whose press the bank refused no longer reaches `pad_up` either; R2 checks that `PerformBank::pad_down` leaves no held state on a refusal (`seq/src/perform.cpp`) and pins it with a test.
- R-2 **Ring pressure**: a fast turn on twelve held tracks; `flush` sends one command per distinct pattern per pass, after the bank's own (32-slot ring).
- R-3 **A note of the old pattern rings on** after a change at once, until its own end (as the official, F4).
- R-4 **Q5 changes E-PERF-07**: an override no longer survives leaving Perform; its QA rows QA-PRF-147..149 get a line.

## 7. Evidence labels

- Hold + turn at once with the place kept, hold + arrow at the loop end with a blink, no FX cancel at once and FX cancel at the loop end, the number per track on screen: **verified parity** (F2-F6).
- The pads as tracks, the REMIX page, SONG / SONG NOW, the walk over existing patterns, Song slots keeping the remix (Q2 a), Stop ending it (Q3 a), leaving at once (Q5 a, close to F7): **new Community OS behaviour**.
- Whether the official firmware plays an empty pattern for a number that has no file (F2 clamps to 1..255): **open research question**, not needed.

## 8. State for implementer

- **R1.** Start with: `Sequencer::queue_track_pattern` `seq/src/sequencer.cpp:590-615` (the model to follow); the drain `:493-498`; `read_row` `:1662-1668` (the place rule); the E-PERF-07 tests `tests/seq_sequencer_test.cpp:1583-1607` (`trackpat_project`, `note_at`, `queue_tracks`). Rules: render context (no allocation, bounded, no double); append the kind at the end of `Command::Kind`; `sizeof(Command)` stays 8; FWD / BACK / PING / RND and E-PERF-07 unchanged (their tests green); Level C: risk review plus one independent review, `ctest -R unit_seq`, `tools/ci.sh host` before merge.
- **R2.** Start with: `PerformBoard` `app/include/o3os/app/perform_board.h:25-50`; `queue_tracks` and `pick_pattern` `app/src/perform_actions.cpp:99-144`; the release routes `app/src/app.cpp:174-183`, `:226-235`; `App::poll_perform` `:2459-2488`. Rules: main loop only; the board pure and host-tested without the App; a hint for every refusal; no new binding yet.
- **R3.** Start with: the DJ page as the pattern (`PerformScreen::field_context` `app/src/screens/perform_screen.cpp:291`, `ViewState::dj_page()` `app/include/o3os/app/view_state.h:205`, the `kPerformDjCtx` bindings `app/src/bindings.cpp:847-861`); the track boxes `perform_screen.cpp:442-476`; `sim/scenarios/qa/qa_perform_dj.txt`. Rules: the context id is the next free one at the time; the slots' keys inert on the page; goldens regenerated with the reconfigured `build-os-host` (`-DO3OS_RATE_44100=ON`); evidence labels in the QA rows. Unit check: QA-PRF-221, 222, 225-227 on an image.
- **R4.** Start with: `docs/manual/perform.md:158-170`; the Perform help page `app/help/pages.txt:303-326` (packed into the help blob, `app/src/help_text.cpp` serves it); `docs/ui/official-manual-map.md:390`, `:558`. Rules: the `manual-writing` skill (lead with what is new); `tools/check_keytext.py` clean.
- Report per slice: flash, SRAM_L, SRAM_U deltas and the scoped `ctest`.
