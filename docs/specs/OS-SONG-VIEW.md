# OS-SONG-VIEW: Song STAGE and TIMELINE pages

Batch of OS-SONG-STAGE and OS-SONG-TIMELINE (backlog rows, approved by the maintainer 2026-10-30). Level B, no format change (view state only, nothing saved), main loop only (no audio-path code). Both are **new Community OS behaviour**.

## 1. Facts (read from the tree, 2026-10-30)

- **Pages today.** `SongPage { Order = 0, Arrangement = 1 }`, `kSongPages = 2` (`app/include/o3os/app/screens/song_screen.h:11-12`). The header says `ORDER 1/2` / `ARRANGEMENT 2/2` (`app/src/screens/song_screen.cpp`, the page caption in `SongScreen::render`).
- **Page key.** `tap Song` is `nav.screen(Song)` (`app/src/bindings.cpp:375`). On the screen in view, `navigate_to` calls `Screen::repress`, which by default does `page = (page + 1) % pages()` (`app/include/o3os/app/screens/screen.h:96-104`). `Song` overrides nothing, so a fifth page needs only `pages()`.
- **Hold to peek.** `hold Song` is `nav.peek` and `hold_end Song` is `nav.peek_end` (`app/src/bindings.cpp:388-389`, `app/src/screen_actions.cpp:50-70`); the peeked Song comes up on the page it was left on (QA-SNG-060). The tap acts on release and a hold of 0.4 s or more turns nothing (QA-SNG-061). Neither rule changes: a page is just another value of `view.page[Song]`, which the peek keeps.
- **Faces.** The largest face is Display, 16x32 (`ui/include/o3os/ui/font.h:44`); Data is 10x20, Label 8x16, Micro 6x12. Text on a fill uses a pair of `kContrastPairs` (`ui/include/o3os/ui/theme.h:88`): SelText on Sel, PlayText on Play, AccentText on Accent, SelText on Rec, Text on Pane.
- **Display list.** The chip's list is 2048 words (`eve/include/o3os/eve/dl.h:37`); the project budget is 1792 (`tests/budget_test.cpp`, `ctest budget`). A `panel` costs 2 words in the colour already set (+1 for a colour change, `ui/src/renderer.cpp:167`). The worst screen is the zoomed 8-track Pattern grid, about 1605 of 1792 (`tests/budget_test.cpp:128`). Song's list pages were recorded at 571-616 words before the map (`docs/specs/SONG-UX.md:33`).
- **Play position.** `audio::PlayPosition::Value` has `playing`, `order`, `pattern`, `step`, `ustep` (0..47), `queued`, `tempo_x100` (`audio/include/o3os/audio/play_position.h:25`). A bar is 16 steps (`seq::kStepsPerBar`). The song loops at its end in the live engine (`Config::song_loop = true`, `seq/include/o3os/seq/sequencer.h:198`).
- **Cache pattern to follow.** `SongTrackMap` caches 2 bits a track per pattern keyed by `Project::pattern_generation` (`app/include/o3os/app/song_map.h`); `SongView` owns it. The `App` lives in SDRAM (`platform/mk66/src/app_main.cpp:164`), so a bigger cache costs flash only.

## 2. Decisions (all: default, to confirm by the maintainer)

| # | Question | Default |
|---|---|---|
| D1 | Does TIMELINE replace ARRANGEMENT? | No. TIMELINE is a page of its own and ARRANGEMENT stays: the smallest change, nothing the list pages show is removed. |
| D2 | Tap cycle order | ORDER, ARRANGEMENT, TIMELINE, STAGE, back to ORDER (`ORDER 1/4` ... `STAGE 4/4`). The hold-to-peek rule is unchanged. |
| D3 | STAGE with the transport stopped | NOW is the cursor's slot (caption `CURSOR`), NEXT the one after it, countdown line `STOPPED`, empty progress bar. |
| D4 | Countdown unit | Bars to the next slot, counted up: `ceil(steps left / 16)`, so the last bar says `1 BAR`. A queued live jump (Rec) replaces NEXT and is captioned `JUMP`; the last slot's next is slot 0 captioned `LOOP`. Pattern mode (no song row) shows the playing pattern, caption `PATTERN`, counting to its loop. |
| D5 | Section label | Not shown: OS-SONG-SLOTDATA does not exist yet. The slot shows its pattern name, or `PATTERN 03` when it has none. |
| D6 | Cursor on TIMELINE | Up, Down, Left and Right move the cursor slot; the window starts 4 slots before it (cached nothing: a pure function of the cursor). F-keys are the list pages' keys. |
| D7 | TIMELINE window | At most 40 slots and at most 48 bars; the display list is bounded structurally: slots x (active tracks + 6) is at most 620 (a slot costs its tracks' rectangles plus about 6 rectangles of block, colour, number and energy column), so 16 active tracks show 28 slots, 12 show 34 and 9 or fewer 40 (review fold, `timeline_slot_cap`). |
| D8 | Energy curve | Over the visible window, aligned with the blocks (not a whole-song strip). |
| D9 | TIMELINE details added by the engineer (defaults) | Blocks start at the cumulative bar of the window and are as wide as `ceil(steps / 16)` bars; the page is `max(window bars, 16)` bars wide. Colours: fills Sel, Play, Accent, Rec by `(pattern - 1) % 4` with the text of the contrast pair each has (SelText, PlayText, AccentText, SelText); a slot whose pattern does not exist is an outline. The cursor is a two-pixel white frame, the playing slot a green bar under its block, the playhead a white line at the step. Density of a track in a pattern: `min(15, ceil(30 * notes / length))` over the track's own length, 4 bits a track, cached by `Project::pattern_generation` in `ViewState::density` (not in `SongView`, which `SongScreen::render` copies each frame). Left and Right walk the slots like Up and Down. |
| D10 | Energy curve details added by the engineer (defaults) | Per visible slot: sum over the active tracks of density (0..15) x loudness (0..15, the mean volume of the pattern's notes, an unset volume is 127) x the track's volume (0..100), 0 for a muted track and for every unsoloed track while one is soloed. Only note bytes 1..128 count (a fade, cut or off is not a note: it no longer raises the density either). The columns are in proportion to the biggest slot in view (the curve always fills its strip), one rectangle a slot, drawn in the Vol colour in a strip of two rows at the foot of the pane under an `ENERGY` caption; the thumbnail lanes shrink to make room. The loudness nibbles share the scan and the cache of the densities (`SongDensity::loudness`). QA-SNG-081 and 082 (the ID range 073..080 of the row was for the page). |

## 3. Done when

**OS-SONG-STAGE:** the Song button's fourth page shows the playing slot and the next one in Display-face type, a bar countdown and a progress bar that follows the playhead to a microstep, with the words NOW / NEXT / JUMP / LOOP / STOPPED so colour is never the only cue; the countdown arithmetic is host-tested; a sim scenario and goldens exist; the worst display list is under 1792 in every theme; QA-SNG-067..072, the ui-map line and the manual section are in. Unit check: legibility at a distance and the countdown against the audio (HIL).

**OS-SONG-TIMELINE:** the third page draws the visible slots as blocks scaled to bars, a colour per pattern (four fills, `(pattern - 1) % 4`) with the pattern number on the block when it fits, and one rectangle per track per slot at most, as thumbnail height; densities are computed once per pattern generation (host test counts the scans); the window and scaling are host-tested; a sim scenario and goldens; display list under 1792 in every theme; QA-SNG-073..080, the ui-map line and the manual section are in. Second commit: the energy curve (density x volume x track level, a muted track counts nothing), host-tested, QA-SNG-081..082.

## 4. Left for later

A whole-song minimap of the energy curve; the section label once OS-SONG-SLOTDATA exists; repeats in the bar counts (SLOTDATA).

## 5. Measured (OS-SONG-STAGE)

Display list of the STAGE page, the dearest of the 15-variant sweep (`song/stage_worst`: stopped, next, jump, loop and pattern mode, three playhead places, 16 active tracks): 521 words in the VTX2F mode (438 translated) of 1792, the same in every theme. Typical (`song/stage`): 418 words in the scenario's count.

## 6. Measured (OS-SONG-TIMELINE)

Display list of the TIMELINE page, the dearest of the 64-variant sweep (`song/timeline_worst`: every count of active tracks 1 to 16, 40 one-bar patterns with single-digit ids cycling 1 to 8 and notes in every track, the cursor at the start and in the middle, the playhead off and on): 1650 words in the VTX2F mode (14 tracks, 28 slots 1650; 12 tracks 1629; 16 tracks 1648; 1 track and 40 slots 939), 1579 translated, of 1792, the same in every theme: the headroom is 142 words. Before the fold the sweep covered 16 tracks only and 12 tracks x 40 slots reached 1841 (over); the slot budget (620 rectangle-equivalents, 6 of them per slot besides its tracks) makes the bound structural, and `song_timeline_test` checks the arithmetic for every track count. The other dearest screen is the zoomed 8-track Pattern grid at about 1605. Typical (`song/timeline`, 12 tracks): 863 (783 translated), unchanged.
