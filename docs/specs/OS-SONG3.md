# SONG, rethought: one list, one editable timeline

## Maintainer decisions (2026-11-05)

Approved: every recommendation of this note, with these answers.

**Scope cut (maintainer, 2026-11-05, after the whole-backlog estimate):** S6 (OS-SONG-SLOTDATA), S8 (OS-SONG-JAM) and S9 (OS-SONG-FLOW) are dropped; Q7 falls with S6. The slices below that mention them are kept as the record of the design.

| Q | Decision |
|---|---|
| Q1, Q1b | Fold ORDER and ARRANGEMENT into LIST; 20 px map cells. |
| Q2 | TIMELINE: Left/Right slots, Up/Down tracks. |
| Q3 | **No.** Keep our Song key row (F3 ADD, F4 DELETE, F5 UNIQUE, F6 TOOLS, F7 TEMPO, F8 UNDO, Shift + F8 REDO); no realignment to the official one. |
| Q4 | PREVIEW through the solo path. |
| Q5 | Loop a range while editing: back in (un-dropped). |
| Q6 | ADD EMPTY: yes. Shift + F3 and Shift + F4 are the 1/2 and 3/4 jumps (OS-QJUMP) in our layout, so the slice's spec picks a free gesture. |
| Q7 | The section label replaces the number on the blocks after SLOTDATA. |
| Q8 | 12 visible lanes, **also on LIST** (maintainer). Tracks 13-16 stay reachable: the 12-lane window scrolls when the cursor goes past track 12 (orchestrator's proposal, to confirm in the spec). |

Copying a whole pattern already exists (PAT-OPS-B: F1 held on Pattern, then Copy / Shift + Copy; Song: Copy / Shift + Copy of slots; Shift + F3 / Shift + Insert adds a slot with a copy as a new pattern). It is hidden; OS-PTOOLS2 S3 makes COPY PATTERN / PASTE PATTERN visible verbs of the F7 menu.


Fable design opinion, 2026-11-05. Read only; nothing in the tree was changed. Facts come from the tree at dev 7ba9c8d9 (`docs/manual/song.md`, `docs/qa/catalogue/song.md`, `docs/specs/SONG-UX.md`, `OS-SONG-VIEW.md`, `OS-SONG-VIEW2.md`, `OS-SONG-BUILD.md`, `OS-SONGSHARE.md`, `app/src/screens/song_screen.cpp`, `song_pages.cpp`, `app/include/o3os/app/song_view.h`, `build-map/keys.md`) and from tracker-ce (`core/interface/songEditor/fSongEdit.cpp`, `gSongEdit.cpp`, `core/libs/NewDisplay/songPlayer.cpp`, `songPlayerControl.h`, `core/source/mtStructs.h`), described as facts, no code taken.

**The recommendation in one line:** drop ARRANGEMENT (fold its two numbers into ORDER), make TIMELINE the editing grid with the same cursor, rectangle and actions the list's map already has, and spend the saved effort on the three things that make a song fun to build: hearing a cell (PREVIEW), labelling sections (SLOTDATA, approved) and looping a range while you edit (dropped on 2026-10-18; I ask the maintainer to look again).

## 1. What the official firmware (tracker-ce) has and what we have

The official Song screen (verified from tracker-ce; `cSongPlayer`, `fSongEdit.cpp`) is **one page**: a grid of slots (12 rows visible, 23 px blocks) by tracks (16 columns: 8 sample, 8 MIDI) at the left, and a list (slot, pattern number, name; 210 px) at the right. A block is filled when that track of that slot's pattern holds data, drawn in a second colour when the track is muted, the track names run above the columns. The cursor is either in the grid, in the list, or on the tempo field (`selectedPlace` 0/1/2). Its song data is a playlist of pattern numbers (`strSong::playlist[255]`), nothing per slot.

| Area | Official firmware (tracker-ce, fact) | track3r-os today | Verdict |
|---|---|---|---|
| Main view | Grid (slots x tracks) and list on one page; no page cycling | Three pages: ORDER (list + small map), ARRANGEMENT (list + bars + small map), TIMELINE (view only) | They have one strong editing view; we have two lists and a picture |
| Grid cell content | Filled = track has data; mute colour; nothing about rhythm | Map cell: note / other data / empty (2 bits); TIMELINE: a real note map (ticks per step) but not editable | Our picture is richer; theirs is editable where it is big |
| Selection | Rectangle of slots x tracks with Shift + arrows (`song_selection_t`), both axes | Same, in the list's map only | Parity, but only on the small map |
| Copy / paste / delete tracks | Copy (pattern range x track range), paste at the cursor (clipped to the song's end), delete tracks of the selected patterns; a progress popup while the SD workspace is rewritten; one-level undo | In RAM, instant, tiled paste, cut, multi-level undo/redo with a size cap, the ALL/UNIQUE question for shared patterns | Ours is better in every way except visibility |
| Preview one track | **Shift + F1 on a single cell loops only that track of that pattern (`Preview Track`), the progress line runs inside that cell** | Nothing (IDEA row "Song clip preview") | Missing, and it is the fun one |
| Add slot | F4 `Add Slot` (same pattern); **Shift + F4 `Add Empty Slot`: a slot on the first pattern with no data** | F3 ADD SLOT; Shift + Insert DUP PAT (a copy) | Missing: add a slot on a fresh pattern |
| Edit mode | Rec toggles a red/green edit mode; outside it the cursor is locked while playing and F3..F6 are dimmed | Rec queues a live jump; every edit works while playing | Ours is better; nothing to take |
| Playhead | A line that descends through the playing slot's blocks (per step) | ORDER: a play icon; TIMELINE: a playhead line and a green bar | Parity on TIMELINE only |
| Mutes on the grid | Muted track columns in a mute colour | Not shown in the map or the lanes (the energy row counts them) | Missing, cheap |
| Encoder | Scrolls the list; Shift + encoder changes the slot's pattern; on the tempo field the tempo | Changes the slot's pattern; F7 puts it on the tempo | Different; ours is one turn fewer |
| Keys | F1 Play Pattern (Shift: Preview Track) · F2 Play Song · F3 Undo · F4 Add Slot (Shift: Add Empty) · F5 Delete Slot · F6 Pattern- · F7 Pattern+ · F8 Tempo | F1 PLAY PAT · F2 PLAY SNG · F3 ADD SLOT · F4 DEL SLOT · F5 UNIQUE · F6 TOOLS · F7 TEMPO · F8 UNDO | **Settles SONG-UX Q1:** the official F6/F7 are PAT-/PAT+ and F5 is Delete Slot, so our F5/F6 do collide with official keys; by the maintainer's own rule (Q2, 2026-10-18) the row goes back to him (Q3 below) |
| Song button | Tap: the cursor goes to the tempo field while held; hold: exit on release; Song + Play plays and exits on release | Tap: next page; hold: peek | Different, both fine |
| Shared patterns | Nothing | The `*` mark, "in n slots", UNIQUE, next use, the ALL/UNIQUE question | Ours only |
| Slot ops | Add, delete, change pattern, home (Shift + Insert) | Add, dup, delete, cut, paste, move (Copy held + Up/Down), quarter jumps, name | Ours only |
| Tools | None | TRANSPOSE, INSTRUMENT, CLEAR, EXPORT WAV, FORM, BRIDGE | Ours only |
| Live | None beyond play | Rec jump queue, 48 SLOTS pads, countdown on the info line, pads TRACKS mute | Ours only |
| Per-slot data | None | None yet; OS-SONG-SLOTDATA approved (repeats, mute mask, tempo, transpose, scene, label, colour) | Both lack it; ours is planned |

Reading of the table: we are ahead on operations and behind on **where the operations live**. The official firmware edits where you look (the big grid); we edit on a 10 x 16 px map at the side of a list, and the page you would want to edit on (TIMELINE) is read-only.

## 2. Diagnosis: ORDER and ARRANGEMENT

They are the same list with two columns swapped. Measured from `song_screen.cpp` (`make_song_layout`, the row passes):

| Column | ORDER | ARRANGEMENT |
|---|---|---|
| POS, PAT, play marker, shared mark, the cursor block, the track map, the Rec blink | yes | yes |
| NAME (12 chars) | yes | no |
| LEN (steps) | yes | no |
| START (bar), BARS | no | yes |
| LENGTH bar graphic (360 px, steps/128) | no | yes (redundant with BARS) |
| Info line extra | "in n slots" | "n bars in all" |

Everything else (keys, cursor, map editing, TOOLS, pads) is identical. The ARRANGEMENT page carries one new fact per row (the start bar) and one per page (the total bars); the 360 px bar graphic says what BARS says. That is not a page, it is two columns.

**Decision: fold them.** One list page with POS, PAT, NAME, BAR (start), BARS, the shared mark and the map, the total bars on the info line. Width check at the theme's 10 px data cell (ORDER today: POS 40, PAT 74, NAME 110, LEN 248, map 300..524): BAR (4 digits) at 248, BARS (3) at 300, the map from 352, 16 cells at a 14 px pitch end at 576; even a 20 px cell (24 px pitch, easier to read and to aim at) ends at 736. Fits. LEN in steps goes (bars are what you count on Song; steps belong to Pattern). Name it **LIST** (the word ORDER means the whole list in the manual; LIST 1/2 reads as "the list" against "the timeline").

**Keep TIMELINE and make it the grid.** It is the one page that shows the song as music (bar-scaled blocks, the rhythm of every track, the energy), it already costs little (891 words worst case of 1792 after the note map), and it is the maintainer's own pick. Making it editable is cheaper than it looks: `SongView` already has the column (`col_`, `tanchor_`), every map action (`song_copy`, `song_paste`, `song_clear`, `song_cut`, `song_tools_open`, `song_unique`, the question) reads the view, not the page; what keeps TIMELINE read-only is `song_map_cells()` returning 0 for it and `SongScreen::step()` sending all four arrows to the slot axis.

**Rejected: a CE-style grid (slots down, tracks across, big equal blocks) as a fourth or replacement page.** It would be a third rendering of the same data, equal blocks hide the time scale (a 1-bar fill and an 8-bar verse look the same), and the per-pattern note-map pictures in RAM_G are rows-per-track (tracks down), which TIMELINE's orientation uses directly. If the maintainer wants the list's map bigger, widen its cells (one constant, `map_pitch`), do not add a page.

**Two pages, one tap apart.** The cycle becomes LIST 1/2, TIMELINE 2/2: a tap toggles. This also closes the HIL gap noted in STATUS (the `qa_song.json` page guard knows 2 pages; it was 3).

## 3. The proposed SONG

### 3.1 The journey and what each page is for

| Stage | What you do | Where | Already there | To build |
|---|---|---|---|---|
| **Sketch** | Lay out the skeleton from one to three loops | LIST: F3 ADD SLOT, Shift + Insert DUP PAT, TOOLS FORM (8-10 slots in one go), the encoder on PAT | FORM, BRIDGE, DUP, ADD | ADD EMPTY (a slot on a fresh pattern) |
| **Arrange** | Move, copy, repeat, split sections; take tracks out of sections | TIMELINE: the cell cursor, rectangles, Copy / Shift + Copy / Delete / Shift + Delete, TOOLS CLEAR and TRANSPOSE, UNIQUE, Copy held + Left/Right moves a slot | the actions, on the list's map | the actions on TIMELINE |
| **Refine** | Listen to one part in place, fix it, name the sections | TIMELINE: PREVIEW (F1 on a cell), Pattern button opens the cursor's pattern, Shift + F6 NAME; later section labels and colours | NAME, Pattern on Song | PREVIEW, loop a range (Q5), SLOTDATA labels |
| **Perform** | Play through, jump, mute, count in | either page: Rec jump, the SLOTS pads, the countdown on the info line, TRACKS pads; mutes drawn on the lanes | all but the drawn mutes | mutes on the grid; JAM capture (approved, after SLOTDATA) |

### 3.2 The LIST page (ORDER + ARRANGEMENT folded)

```
+--------------------------------------------------------------------------------------------------+
| SONG | # STOP | LEN 12  POS 03  PLAY --  PAT 03 chorus   BPM 130.0                  CPU --  MEM -- |
|  POS PAT NAME          BAR  BARS   TRACKS 1  2  3  4  5  6  7  8  9 10 11 12          LIST 1/2    |
|  00  01  intro        001   4      [#][#][ ][ ][=][ ][ ][ ][ ][ ][ ][ ]                            |
|  01  02 *verse        005   4      [#][#][#][#][=][#][ ][ ][ ][ ][ ][ ]                            |
|  02  02 *verse        009   4      [#][#][#][#][=][#][ ][ ][ ][ ][ ][ ]                            |
| >03 [03] chorus       013   4      [#][#][#][#][#][#][#][#][ ][ ][ ][ ]  <- cursor on PAT          |
|  04  02 *verse        017   4      [#][#][#][#][=][#][ ][ ][ ][ ][ ][ ]                            |
|  05  04  bridge 1/4   021   4      [#][#][#][ ][=][#][#][ ][ ][ ][ ][ ]                            |
|  06  05  bridge 2/4   025   4      ...                                                             |
|  07  06  bridge 3/4   029   4                                                                      |
|  08  07  bridge 4/4   033   4                                                                      |
|  09  03  chorus       037   4                                                                      |
|  10  03  chorus       041   4                                                                      |
|  11  08  outro        045   2                                                                      |
|                                                                                                    |
| i  list   slot 03 of 12   46 bars in all   pattern 03 in 3 slots      press SONG: TIMELINE        |
| F1 PLAY PAT  F2 PLAY SNG  F3 ADD SLOT  F4 DEL SLOT  F5 UNIQUE  F6 TOOLS  F7 TEMPO  F8 UNDO         |
+--------------------------------------------------------------------------------------------------+
   [#] note   [=] other data   [ ] empty   (map cells 20 px wide instead of 10: Q1b)
```

Changes from today: START and BARS come in (BAR, BARS), LEN (steps) and the 360 px LENGTH graphic go, the map cells may grow, the page caption is LIST 1/2. Every key, gesture and QA row of the list and its map stays as it is (QA-SNG-001..066, 097..115).

### 3.3 The TIMELINE page, editable

```
+--------------------------------------------------------------------------------------------------+
| SONG | > PLAY | LEN 12  POS 02  PLAY 01  PAT 02 verse   BPM 130.0                  CPU 41  MEM 12 |
|  bar 1       5       9       13      17      21      25      29      33      37   TIMELINE 2/2    |
|     +-------+=======+-------+-------+-------+-------+---+---+---+---+-------+-------+----+          |
|     |1      |2 *    |2 *    |3      |2 *    |4      |5  |6  |7  |8  |3      |3      |8   |  blocks  |
|     |=======|=======|=======|-------|=======|.......|   |   |   |   |-------|-------|....|  + strip |
|     +-------+=======+-------+-------+-------+-------+---+---+---+---+-------+-------+----+          |
|       (the cursor's slot: a bright rule round its block; the number top-left, the colour strip at the foot, as today)|
|  1  |x.x.x.x|x.x.x.x|x.x.x.x|x.x.x.x|x.x.x.x|x.x.x.x|x.x|x.x|x.x|x.x|x.x.x.x|x.x.x.x|x.x.|  lanes   |
|  2  |.......|..x...x|..x...x|..x...x|..x...x|..x...x|..x|..x|..x|...|..x...x|..x...x|....|          |
|  3  |.......|xxxxxxx|#######|xxxxxxx|xxxxxxx|xxxxxxx|xxx|xxx|xxx|xxx|xxxxxxx|xxxxxxx|....|  <- cursor|
|  4  |.......|x..x..x|#..#..#|x..x..x|x..x..x|.......|   |   |   |   |x..x..x|x..x..x|....|  <- band |
|  5  |.......|.......|.......|x.xx.xx|.......|.......|   |   |   |   |x.xx.xx|x.xx.xx|....|          |
|  6  |.......|......x|......x|......x|......x|......x|...|...|...|...|......x|......x|....|  (muted  |
|  7  |.......|.......|.......|x...x..|.......|.......|   |   |   |   |x...x..|x...x..|....|   lane   |
|  8  |.......|.......|.......|.......|.......|.......|   |   |   |   |.......|.......|....|   dimmed)|
|  9..12 (empty lanes)                                                                                |
|     ENERGY  _  ====  ====  #####  ====  ===  ==  ==  ==  ==  #####  #####  _                       |
| i  slot 02  track 3  bar 5  pattern 02 in 3 slots   slots 02-02 tracks 3-4  Copy, Delete, F1 PREVIEW|
| F1 PREVIEW   F2 PLAY SNG  F3 ADD SLOT  F4 DEL SLOT  F5 UNIQUE  F6 TOOLS  F7 TEMPO  F8 UNDO         |
+--------------------------------------------------------------------------------------------------+
   # = cursor cell / selected cells (Accent band, ticks in AccentText), x = a note tick, . = a step
   The block row is the "PAT" column of this page: the cursor on a block is the whole slot.
```

Seen in the simulator (a scratchpad build of dev, `qa_song_notemap` and `qa_song_timeline` rendered to a scratch folder (not kept)): the block row is about 40 px (number top-left, 4 px colour strip at its foot), the twelve lanes about 24 px each with their numbers in the left margin, ENERGY at the foot; the lanes run across the page with **no vertical rule at a slot boundary**, only the blocks above say where a slot ends. For editing, S2 adds a faint rule at each slot boundary through the lanes (one 1 px panel a slot, 2 words each) so a cell reads as a cell; the cursor frame and the selection band then sit on visible cells. The sim's display list for these pages is 415-469 words (the 891 worst case is the 40-slot sweep).

**Cursor and selection (the list's model, transposed):**

| Gesture | On the block row (whole slot) | In a lane (a cell = one track of one slot) |
|---|---|---|
| Left / Right | previous / next slot (the window follows) | the same slot step, the track stays |
| Down / Up | Down enters lane 1 of the slot; Up on the block row does nothing | Up / Down walk the tracks; Up from lane 1 returns to the block row |
| Shift + Left / Right | extend the range of slots | extend the range of slots |
| Shift + Up / Down | nothing (as Shift + Left/Right on PAT today) | extend the range of tracks |
| Copy / Shift + Copy / Delete / Shift + Delete | slots (as on PAT: copy, paste-insert, delete, cut) | cells (as in the map: copy, paste tiled, clear, cut) |
| F6 TOOLS, F5 UNIQUE, the ALL / UNIQUE question | as on PAT | as in the map |
| Copy held + Left / Right | moves the slot or the range along the song (Up/Down on the list) | the same |
| the encoder | the slot's pattern | the slot's pattern (unchanged, D2 of SONG-UX) |
| F1 | PLAY PAT (loop this slot's pattern) | **PREVIEW**: this slot's pattern with the rectangle's tracks only (3.4) |
| Rec, the pads, F7, Shift row | unchanged | unchanged |

Implementation shape (for the engineer, not for the maintainer): `SongView` keeps one cursor for both pages (`pos_`, `col_`, `anchor_`, `tanchor_`): leaving LIST on slot 5 track 3 lands on TIMELINE's slot 5 track 3. `song_map_cells()` returns the lanes drawn (`min(active_tracks, 12)`) on TIMELINE; `SongScreen::step()` maps dx to the slot axis and dy to the column axis on TIMELINE (today both go to the slot); `song_select` / `song_select_tracks` get the same swap. The draw adds a cell frame (the list's `cursor_outline`, 2 panels), one Accent band per selected lane row (at most 12 rects) and a brighter rule under the block of the cursor's slot: under 60 display-list words; the 891-word worst case has 900 words of headroom. No new action, no new clipboard, no new undo kind: the QA rows 020..039 and 053..057 get a TIMELINE twin each in one scenario.

**What the actions do to shared patterns on TIMELINE:** exactly what they do in the map today, because the code is the same: an edit writes into the pattern, so a slot outside the rectangle that holds the same pattern would change too; the strip asks `PATTERN 02 ALSO IN 2 SLOTS: F1 ALL, F2 UNIQUE` (QA-SNG-036..038). The `*` on the blocks and the second ink on the other holders (D6 of OS-SONGSHARE) tell you before you press. Duplicate = Shift + Insert (a copy, a new pattern number); make unique = F5 (this slot, or every selected slot that shares with the outside); clear = Delete (keeps the pattern, empties the tracks); cut = Shift + Delete (clear + clipboard). No new semantics, which is the point: one model, two views.

**Limits to state:** TIMELINE draws 12 lanes (D11, "we have twelve"); tracks 13-16 of a 16-track project are edited on LIST, whose map has 16 cells. The window stays bounded as today (34 slots at 12 tracks); the cursor's slot is always in view.

### 3.4 The fun parts (in order of value per byte)

1. **PREVIEW (F1 in a lane).** Loop the cursor slot's pattern with only the rectangle's tracks sounding; the playhead runs through the cell as the official firmware's does; F1 again, or Stop, restores. This is the official "Preview Track" made bigger (any rectangle, not one cell). Mechanism: the existing solo path (the project's solo state is already what the pads and the Mixer use, D18): PREVIEW sets a solo on the rectangle's tracks for the audition and restores the previous mute/solo state when it ends; it must **not** be an undo unit (the mute of D18 is), so it goes through a view-side audition mask or a non-journaled call (Q4). CPU: none beyond a pattern loop.
2. **Sections on the blocks** (OS-SONG-SLOTDATA, approved, Level C, format bump): the block shows the section label and its colour instead of the pattern number (`VERSE`, `DROP`; the number stays on the info line). This fixes the tester's "7 8 1 2 run together" and "patterns 1 and 5 share a colour" notes in one move, and a song reads as a song. Repeats (x2..x16) make a block wider without copying slots.
3. **Loop a range while you edit.** Shift + Left/Right over four slots, hold F2 (or F1 with a range): the sequencer loops those slots while you fix the chorus on Pattern and come back. Every DAW arranger has it; the maintainer dropped it on 2026-10-18 (OS-SONGLOOP, with the F1 key). I ask him to look at it again now that the grid is where you arrange (Q5). Cost: the sequencer already loops the song (`song_loop`); a loop range is two bounds checked at the slot boundary.
4. **ADD EMPTY** (official parity: a slot on the first pattern with no data, so a new section starts from silence, not from a copy). One action, a slot op; where it sits is Q6.
5. **Mutes on the grid**: a muted track's lane is dimmed (and the map cell on LIST); the TRACKS pads then show their state where you look. Two colours in the draw, no new state.
6. **Copy held + Left / Right** moves the slot along the timeline (the list's Up/Down gesture, same action with the axis swapped): nudging a section is what arranging is.
7. **The playing cell**: on TIMELINE the playhead already runs through the lanes; on LIST the cursor's row could get a thin progress line under the PAT tile (the official firmware's descending line). Nice, cheap, last.

Not proposed: a fourth page, STAGE back, LANES (per-track patterns; OS-SONG-IDEAS, OG export hard), chance and flow rows (OS-SONG-FLOW, the maintainer's move), tempo ramps.

## 4. Slice plan

Each slice is one branch, one review; no format change until S6. Sizes in the backlog's letters.

| # | Slice | Size | Level | Acceptance | Format | Risk | Backlog row |
|---|---|---|---|---|---|---|---|
| S1 | **Fold ARRANGEMENT into LIST.** BAR and BARS columns on the list page, LEN (steps) and the LENGTH graphic go, the page cycle is LIST 1/2, TIMELINE 2/2; the info line keeps "n bars in all" | S | A/B | Captions LIST 1/2 and TIMELINE 2/2 (`qa_song` snaps, goldens, the manual's Song chapter and key diagrams, ui-map); QA-SNG-087 rewritten (2 pages), the HIL `qa_song.json` page guard set to 2; every list/map QA row still passes; DL unchanged or lower | none | low: columns and one page enum | new row OS-SONG-LIST (or fold into OS-SONG-VIEW2 as slice 4) |
| S2 | **Editable TIMELINE.** The cell cursor and rectangle on TIMELINE, the axis swap, the same copy/paste/clear/cut/TOOLS/UNIQUE/question, the cursor shared with LIST | M | B | A scenario `qa_song_timeline_edit` repeats QA-SNG-020..039, 053..057, 066 on TIMELINE (each gets a row 116+); `song/timeline_worst` re-measured under 1792 in every theme; the cursor's slot always in view; Rec, pads and the countdown unchanged; host tests on the axis mapping and `song_map_cells` per page | none | medium: `song_sync` / `fit_cols` on a page change, the window follow with a track cursor, the 12-lane cap vs 16 tracks | new row OS-SONG-GRID |
| S3 | **Grid polish.** Muted lanes and map cells dimmed; `*` on shared blocks; Copy held + Left/Right moves slots on TIMELINE; the LIST row progress line | S | B | Snaps for each; the move's QA-SNG-056/057 twin; no new state | none | low | fold into OS-SONG-GRID or SMALLFIX |
| S4 | **PREVIEW.** F1 in a lane loops the slot's pattern with the rectangle's tracks only; the key label reads PREVIEW in a lane; F1 again / Stop restores the mute/solo state; not an undo unit | S-M | B if it rides the existing solo path from the main loop; C if a new sequencer mask | Audio scenario (render with and without PREVIEW: the un-soloed tracks are silent); mute/solo state identical after the audition (host test); works while the song plays (it becomes PLAY PAT of that slot); QA rows | none | medium: the audition must never leave a stuck solo (a screen change ends it) | the IDEA row "Song clip preview", reshaped |
| S5 | **ADD EMPTY + key row** (Q3, Q6): ADD EMPTY as a slot op; if the maintainer realigns the row to the official, the labels, bindings, manual diagrams and QA rows move in the same commit | S | A/B | `builtin_bindings_test`, `shift_labels`, `manual_diagrams` regenerated; `NO FREE PATTERN` when the pool is full | none | low, but it touches 18 manual sections' sync lock (bindings.cpp) | SMALLFIX or OS-SONG-KEYS |
| S6 | **Sections on the blocks** (OS-SONG-SLOTDATA, approved): the chunk, loader, round trip; then labels + colours + repeats drawn on TIMELINE blocks and the LIST's NAME column; the mute mask at the slot boundary | L | C | The row's own acceptance (spec, risk review, qemu, unit); TIMELINE shows the label when the block is wide enough, the colour strip takes the section colour; repeats widen the block; OG export expands repeats | **version bump**, new optional sparse chunk | the sequencer at slot boundaries; undo journaling of slot data | OS-SONG-SLOTDATA (approved, order 5) |
| S7 | **Loop a range** (if Q5 is yes): hold F2 / F1 with a range of slots loops them; the info line says LOOP 03-06 | S | C (sequencer slot boundary) | Audio scenario: the loop returns to the first slot; Rec jump leaves the loop; a slot deleted inside the loop shrinks it | none | the sequencer's boundary logic (the E-SONG-07 journal already handles edits) | OS-SONGLOOP (dropped; the maintainer's move) |
| S8 | **JAM capture** (approved, after S6) | M | B | the row's own | none (uses S6's chunk) | — | OS-SONG-JAM |

Order: S1 → S2 (→ S3 inside it) → S4 → S5 → S6 → S8; S7 wherever the maintainer puts it. S1 and S2 can be one batch branch (two commits); S4 is the first "fun" the tester will hear, so it goes before SLOTDATA.

Budget notes: S1-S5 are main-loop and display-list only; flash a few KiB in all (S2 reuses every action; the draw grows by a cursor and a band), SRAM 0 (the view already holds the column). S4's audition state is a few bytes in the view. S6's costs are in its own row.

## 5. Open questions for the maintainer

| # | Question | Options | Recommendation |
|---|---|---|---|
| Q1 | Fold ARRANGEMENT into ORDER, two pages | a) fold; page names LIST / TIMELINE · b) fold; keep the name ORDER · c) keep three pages | a |
| Q1b | Bigger map cells on LIST (20 px instead of 10, pitch 24) now that the columns leave room | a) yes · b) as today | a (one constant; easier to aim at; the map is the list's editing surface) |
| Q2 | TIMELINE arrows | a) Left/Right = slots, Up/Down = tracks, the block row = whole slot (the list's model transposed) · b) keep all four on slots, Shift + Up/Down for tracks only | a |
| Q3 | The key row, now that tracker-ce settles SONG-UX Q1 (official: F3 Undo, F4 Add, F5 Delete, F6/F7 Pattern -/+, F8 Tempo) | a) realign: F1 PLAY PAT/PREVIEW · F2 PLAY SNG · F3 UNDO · F4 ADD SLOT · F5 DEL SLOT · F6 UNIQUE · F7 TOOLS · F8 TEMPO (6 of 8 as the official; UNIQUE and TOOLS sit where PAT-/+ were, which the encoder does); REDO to Shift + F3 · b) keep today's row | a, in S5 while the manual moves anyway; the rule of 2026-10-18 makes this his call |
| Q4 | PREVIEW mechanism | a) the existing solo path from the main loop, state saved and restored, not journaled · b) a sequencer-side audition mask (Level C) · c) skip PREVIEW | a |
| Q5 | Loop a range of slots while editing (dropped 2026-10-18 as OS-SONGLOOP) | a) stays dropped · b) back as S7: hold F2 with a range (or F1 with a range) loops it | b; it is the arranger gesture the grid now invites |
| Q6 | Where ADD EMPTY lives | a) Shift + F3, replacing the 2/4 jump (0/4 and END stay; 1/4 and 3/4 go too, the 48 SLOTS pads jump better) · b) a TOOLS OP · c) Shift + F4 as the official (then the 3/4 jump goes) | c if Q3 is a (the official place), else a |
| Q7 | After SLOTDATA, the block's text | a) the section label when set, else the pattern number · b) always the number, label on the strip | a |
| Q8 | Tracks 13-16 on TIMELINE | a) accept 12 lanes, edit them on LIST · b) a lane scroll | a (D11) |
| Q9 | Name of the fourth value: none. The STAGE countdown stays on TIMELINE's info line | — | keep |

## 6. Evidence labels

- Official firmware facts above: **verified** from tracker-ce source (file:line on request); no code taken.
- Everything proposed: **new Community OS behaviour**, except the key row of Q3a, PREVIEW and ADD EMPTY, which restore official behaviour (parity by design, to verify on a unit with the official firmware only if the maintainer wants it).
- Display-list and size numbers: measured figures are quoted from the specs (891 / 1650 / 1792 words); the slice estimates are [est].
