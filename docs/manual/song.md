<!-- manual-sync: 3ba49bb7 -->
# Song
<!-- sync: src=app/src/screens/song_screen.cpp,app/src/song_actions.cpp,app/include/o3os/app/song_map.h spec=docs/specs/SONG-UX.md,docs/specs/OS-SONGSHARE.md qa=QA-SNG-020,QA-SNG-034,QA-SNG-036,QA-SNG-040,QA-SNG-050 -->

Status: draft

Song is an editor, not only a list. Adding a slot with `F3` (**ADD SLOT**) works as before, and the new slot shares the pattern of the one you were on. New in track3r-os: a `*` marks slots that share a pattern, and `F5` (**UNIQUE**) gives a slot a copy of its own. `Right` steps from the list into a map of slots and tracks that you can copy, paste, clear and transpose, `F6` (**TOOLS**) opens the block tools, and the **TIMELINE** page is an editing grid with the same keys. The pads can pick slots, and holding `Song` shows the screen for a moment. <!-- og: new -->

## What it is for

Use Song to arrange patterns in time. Press `Song`. The list holds the slots, and each slot holds one pattern number, the bar it starts at (**BAR**) and how many bars it lasts (**BARS**). The song plays the slots from the top. Press `Song` again for the **TIMELINE** page, which draws the song as blocks along the page. Two pages in all; the header says which one you are on (**LIST 1/2**, **TIMELINE 2/2**). The next press after **TIMELINE 2/2** goes back to **LIST 1/2**. <!-- QA-SNG-087 -->

## How it works
<!-- sync: src=app/src/song_actions.cpp,app/src/screens/song_screen.cpp spec=docs/specs/SONG-UX.md,docs/specs/OS-SONGSHARE.md qa=QA-SNG-034,QA-SNG-036,QA-SNG-037,QA-SNG-040,QA-SNG-043,QA-SNG-050 -->

### Slots and patterns

A pattern holds the notes. A slot is a place in the song that points at a pattern, like a playlist entry points at a track. Slots 1, 2, 1, 3 play verse, chorus, verse, bridge: pattern 1 is in the song twice.

- **Stored:** the order list is saved with the project.
- **Live:** `Play` plays the song from the selected slot. While it plays, a thin line under the **PAT** tile of the row that plays runs through the pattern with the playhead. <!-- QA-SNG-210 --> `F1` (**PLAY PAT**) loops only that slot's pattern. `F2` (**DUP PAT**) adds a slot after the cursor with a copy of its pattern, as `Shift` + `Insert` does; it replaces the old `F2` PLAY SNG, which `Play` already did (new Community OS behaviour). <!-- QA-SNG-212 --> At the end of the list the song goes round to the first slot. In a lane of the TIMELINE page `F1` (**PREVIEW**) instead loops that slot's pattern with only the selected tracks (the rectangle) sounding; `F1` again, `Stop` or leaving the screen restores the solo set as it was. New in the Community OS. <!-- QA-SNG-202, QA-SNG-203, QA-SNG-204, QA-SNG-205, QA-SNG-206 --> <!-- seq/sequencer.h --> Not checked against the original firmware. <!-- og: open -->
- **Jump:** while the song plays, `Rec` queues the cursor's slot. It starts after the pattern now playing, and the slot blinks until then. `Rec` on it again cancels. <!-- QA-SNG-043 -->
- **Loop:** select the slots with `Shift` + `Up` / `Down` (the cursor's slot alone is enough) and hold `F2`. The song starts at the first of them, and after the last it goes back to the first. The info line says **LOOP 03-06**. Edit the patterns and the list meanwhile: a slot you delete inside the loop shortens it, a slot you add inside it lengthens it. `Rec` on a slot outside the loop leaves it, so does `Stop`; hold `F2` on the same slots again to leave it and keep playing. A tap of `F2` still plays the song, now at the release of the key. New Community OS behaviour. <!-- QA-SNG-200, QA-SNG-201 -->

### Sharing a pattern

`F3` (**ADD SLOT**) adds a slot with the same pattern number. Both slots now play one pattern, so a change made in Pattern is heard in both. Each sharing slot has a `*`, and the info line says, for example, **pattern 03 in 2 slots**. The other slots of the cursor's pattern also show their PAT number in a second ink. <!-- QA-SNG-050, QA-SNG-051, QA-SNG-052 -->

Hold `Copy` and press `F3` (**ADD EMPTY**) instead, and the new slot goes on the first free pattern number, one that does not exist yet: an empty slot, so a new section starts from silence, not from a copy. If the pattern pool is full the strip says **NO FREE PATTERN** and nothing is written. `F8` (**UNDO**) takes the slot back. New in track3r-os. <!-- QA-SNG-211 -->

On the TIMELINE page the blocks of sharing slots wear the same `*` at the right of the number. <!-- QA-SNG-208 -->

To split them, put the cursor on the slot and press `F5` (**UNIQUE**). It gets a copy at the first free pattern number, with the same name, lengths and steps, and the strip says, for example, **SLOT 04: PATTERN 05 (A COPY OF 04)**. `Shift` + `Insert` (**DUP PAT**) adds a new slot with a copy straight away. `Note` jumps to the next slot that holds the cursor's pattern, and `Shift` + `Note` to the previous. <!-- QA-SNG-034, QA-SNG-003, QA-SNG-058 -->

Example: the second verse should have other drums. Put the cursor on it, press `F5`, then edit pattern 5 in Pattern. The first verse is unchanged. `F8` (**UNDO**) takes the copy back.

### The track map

Press `Right` on the PAT column. The cursor moves into a grid: slots down, tracks across. A cell is one track of that slot's pattern. `Left` on track 1 returns to PAT. <!-- QA-SNG-020, QA-SNG-021 -->

The list draws 12 tracks at a time, each cell 20 pixels wide. A project with more tracks scrolls them with the cursor: the **TRACKS** caption then says which ones you see, for example **TRACKS 5-16**. <!-- QA-SNG-047 -->

A muted track's cells are dim, so you see which tracks sound where you look. A solo does not dim them: only the mute does. New in track3r-os. <!-- QA-SNG-209 -->

- `Shift` + arrows select a block of slots and tracks. <!-- QA-SNG-022 -->
- `Copy` copies it (up to 32 cells). `Shift` + `Copy` pastes at the cursor, or tiles the clip over a selection. A paste never adds slots, and it never changes a track's length: steps past the end of a shorter track are dropped (**PASTED: 1 PATTERN, CLIPPED AT TRACK END**). A slot whose pattern does not exist yet gets a new one; if the pool is full the strip says **NO FREE PATTERN** and nothing is written. <!-- QA-SNG-023, QA-SNG-024, QA-SNG-025, QA-SNG-026, QA-SNG-027 -->
- `Delete` clears the cells. `Shift` + `Delete` cuts them. <!-- QA-SNG-028, QA-SNG-054 -->
- If the block covers two or more patterns, `Delete` (and the cut) first asks on the strip: **CLEAR 3 PATTERNS?: F1 YES, OTHER KEY CANCELS**. `F1` clears them as one step that `F8` takes back; any other key leaves the cells as they were (a cut then leaves the clipboard as it was too; it fills on `F1`). The TOOLS dialog's CLEAR does not ask again after its own `F1`. One pattern is cleared at once. New in track3r-os. <!-- QA-SNG-066 -->
- `F6` (**TOOLS**) opens a dialog over the block: **TRANSPOSE**, **INSTRUMENT**, **CLEAR**, **EXPORT WAV**, **FORM** and **BRIDGE**. `F1` applies, `F8` leaves. <!-- QA-SNG-029, QA-SNG-030, QA-SNG-031, QA-SNG-032, QA-SNG-059 -->
- **FORM** builds the shape of a song from one to three patterns in one go, by taking things away: the first slot is the pattern with some tracks cleared (LOOP>SONG: the music, so drums only; POP: the kick and the rest of the drums, so music only; TECHNO: everything but the kick), a break is the pattern without the kick. Select the slots whose patterns you want (up to three different ones: A, B, C in the order of the slots; with fewer, the missing ones are A), open TOOLS, turn OP to **FORM**, choose **LOOP>SONG** (8 slots), **POP** (9) or **TECHNO** (10) and set **DRUMS**, how many tracks from track 1 are drums (the kick is track 1, the music is the tracks after the drums). The footer says the plan, for example `FROM 02 05 07  8 SLOTS  3 NEW PATTERNS`. `F1` puts the new slots after your selection (nothing you built moves or changes) and the cursor on the first of them; the changed versions are new patterns, so your originals are never touched. If the song, the pattern pool or the undo memory cannot take it, the strip says why (`SONG FULL: NEEDS 8 SLOTS, 7 FREE`, `NEEDS 3 FREE PATTERNS, 1 FREE`, `TOO BIG TO UNDO: SELECT LESS`) and nothing is written. One `F8` takes the slots and the new patterns back. It works while the song plays. New in the Community OS. <!-- QA-SNG-097, QA-SNG-098, QA-SNG-099, QA-SNG-100, QA-SNG-101, QA-SNG-102, QA-SNG-103, QA-SNG-104, QA-SNG-105, QA-SNG-106 -->
- **BRIDGE** puts a run of new slots between two neighbouring slots, so the music moves from the first pattern (A) to the second (B) instead of jumping. Put the cursor on A (B is the slot after it) or select exactly two slots, open TOOLS, turn OP to **BRIDGE** and set **SLOTS**, how many new slots (1 to 16, `Shift` turns it by 4). The footer says the plan, for example `PATTERN 02 TO 05  4 SLOTS  4 NEW PATTERNS`. `F1` makes each new slot a new pattern, `BRIDGE 1/4`, `BRIDGE 2/4` and so on: in the first one most steps are still A's, in the last most are B's, and in between the steps change from A's to B's a few at a time (always a whole step, never a note of A with the instrument of B; once a step has become B's it stays B's in the later slots; empty steps move like notes, so a note of A that B lacks thins out and a note of B that A lacks fills in). The tracks do not all start together and the steps of a track change one by one, which makes a gradual change. Every track is bridged, whatever the tracks you had selected. A short track repeats as it plays; when A and B have different lengths the tracks keep A's length up to the middle of the bridge and B's after it. The same A and B always give the same result. The cursor goes to the first new slot, A and B are never touched, and the refusals are FORM's, plus `SELECT 2 SLOTS`, `NO NEXT SLOT` (the cursor is on the last slot) and `SAME PATTERN IN BOTH SLOTS`. One `F8` takes the slots and the new patterns back. It works while the song plays. New in the Community OS. <!-- QA-SNG-107, QA-SNG-108, QA-SNG-109, QA-SNG-110, QA-SNG-111, QA-SNG-112, QA-SNG-113, QA-SNG-114, QA-SNG-115 -->

An edit writes into the patterns, not into the slots. If a pattern is also held by a slot outside the block, nothing is written. The strip asks, for example, **PATTERN 06 ALSO IN 2 SLOTS: F1 ALL, F2 UNIQUE**. `F1` changes the pattern for every slot. `F2` copies it first and edits only the copy. Any other key, or 4 s, takes the question back with nothing written. <!-- QA-SNG-036, QA-SNG-037, QA-SNG-038 -->

Each edit is one undo step with `F8`. An edit too big for the undo memory is refused: **TOO BIG TO UNDO: SELECT LESS**. <!-- QA-SNG-039 -->

### The pads

`Shift` + `F7` (**PADS**) switches the 48 pads. The strip says **PADS: SLOTS** or **PADS: TRACKS**. TRACKS is the default at power-on. Rows 2 to 4 mute a track, cycle its mute mode and solo it, as on Pattern. <!-- QA-SNG-040, QA-SNG-041 -->

In SLOTS mode pad n is slot n of the 48-slot page that holds the cursor. When stopped, a pad moves the cursor. While the song plays, it queues that slot as a jump. A tap on the queued slot cancels it. <!-- QA-SNG-042, QA-SNG-043, QA-SNG-046 -->

> **Careful**: In TRACKS mode a stray pad tap on Song mutes a track, and the mute is an undo step of Song. <!-- QA-SNG-041 -->

![Song, map: slots 1 to 3 and tracks 1 to 3 are selected, and the strip counts what was copied.](img/song_qa_sng_023.png)

## Try it
<!-- sync: src=app/src/song_actions.cpp,app/src/screens/song_screen.cpp qa=QA-SNG-003,QA-SNG-034,QA-SNG-050,QA-SNG-051 -->

1. Press `Song`. The list shows the slots, with the cursor on the first.

   ![Song, LIST page: the slots with their pattern numbers; the NAME column is empty until a pattern is named.](img/song_walk_1.png)

2. Press `Shift` + `Insert`. A new slot appears with a copy of the pattern. The info line says **SHIFT+INS DUP**. <!-- QA-SNG-003 -->

   ![Song: a second slot holds the copy.](img/song_walk_2.png)

3. Press `F3` (**ADD SLOT**). The new slot holds the same pattern number. A `*` marks the two slots, and the info line says **pattern nn in 2 slots**. <!-- QA-SNG-050 -->

   ![Song: two slots share a pattern, marked with a star.](img/song_walk_3.png)

4. Press `F5` (**UNIQUE**). The slot gets its own copy and the strip says **A COPY OF**. <!-- QA-SNG-034 -->

   ![Song: the strip reports the copy made for the slot.](img/song_walk_4.png)

5. Press `Song` again. The **TIMELINE** page draws the same slots as blocks as wide as they have bars. Press `Song` once more to come back to the list, where **BAR** and **BARS** give each slot's start and length.

   ![Song, TIMELINE page: each slot as a block as wide as it has bars.](img/song_walk_5.png)

6. Press `F8` (**UNDO**). The slot points at the shared pattern again.

## The screen at a glance

![Song: the Shift row while Shift is held, with the quarter jumps, NAME, PADS and REDO.](img/song_qa_sng_001.png)

| Area | What it shows |
|---|---|
| Main area | The list: **POS**, **PAT**, **NAME** (12 characters), **BAR** (the bar the slot starts at), **BARS** (its length in bars) and the `*` mark. The track map sits to the right. The info line gives the slot, the bars in all and, for a shared pattern, **pattern nn in n slots**. |
| Timeline | The slots as blocks as wide as they have bars, with the pattern's number and a strip in its colour, and a lane per track under them that shows how many notes the track plays, and a thin **ENERGY** row at the foot. The info line gives the cursor's slot, its start bar, its pattern and its length; while the song plays it gives the bars left before the next slot instead (see "Read the song on the Timeline page"). |
| Info line | A note on the last key, or on the cursor's slot or track. |
| Strip | Messages, for example **PATTERN 03 NAMED**. |
| Key row | `F1` to `F8`. See the table under "Keys on this screen". |
| Shift row | **0/4** to **3/4** and **END** jump the cursor; `F6` **NAME**, `F7` **PADS**, `F8` **REDO**. |

## More tasks

### Move a slot

Start on Song, with the cursor on the slot.

1. Hold `Copy`, and press `Up` or `Down`. <!-- QA-SNG-056 -->
2. Hold the arrow to keep moving. Each step is one undo step. `Shift` + arrows first select a range, and the range moves as a block. <!-- QA-SNG-057 -->

You should see the slot change place, with the cursor following it. It works while the song plays.

### Cut and paste slots

Start on Song, with the cursor on PAT.

1. Press `Shift` + `Delete`. The strip says **CUT 1 SLOT**. <!-- QA-SNG-053 -->
2. Move the cursor. Press `Shift` + `Copy`. The slots come back at the cursor.

Cutting every slot is refused: **SONG NEEDS A SLOT**. <!-- QA-SNG-055 -->

### Name a pattern

Start on Song, with the cursor on the slot.

1. Press `Shift` + `F6` (**NAME**). The keyboard opens, titled **PATTERN NAME**. <!-- QA-SNG-010 -->
2. Type with the pads, up to 30 characters, and press `F1` (**OK**). `F8` (**CANCEL**) keeps the old name.

You should see **PATTERN nn NAMED** in the strip. The NAME column shows it for every slot that holds the pattern. <!-- QA-SNG-011 -->

### Delete a slot

Start on Song, with the cursor on the slot.

1. Press `Delete` or `F4` (**DEL SLOT**). One slot goes, even if you hold the key. <!-- QA-SNG-012 -->

### Read the song on the Timeline page

New in track3r-os. The Timeline page shows the song as you would draw it on paper: one block for each slot, as wide as the slot is long. It helps you see where the long parts and the short parts are, and which tracks are busy where.

Start on Song.

1. Press `Song` until the header says **TIMELINE 2/2**. The blocks run from four slots before the cursor. <!-- QA-SNG-073 -->
   A slot is as wide as it has bars (a bar is 16 steps). The page is always at least 16 bars wide, so a short song stays small and does not stretch.
2. Look at the blocks. Each one carries its pattern's number, and under the number a short strip in the pattern's colour: there are four colours, so patterns 1 and 5 share one. The strip tells you when a pattern comes back; the number tells you which it is. A very narrow block (one bar in a long view) may be too small for a three-digit number. A slot whose pattern does not exist yet has a dim number and no strip. A block whose pattern another slot also holds wears a `*` at the right of its number, as the list's rows do. <!-- QA-SNG-074, QA-SNG-208 -->
3. Look under the blocks. This is the note map: each track has a lane, and each slot shows a small tick for every step that holds a note, sixteen steps to a bar. You see the rhythm of each track, not only how busy it is. A step with only a volume, an effect or a note-off has no tick. A track that is shorter than the pattern stops where it stops. Patterns of different lengths use the same step width, so the ticks of a short and a long pattern line up with the bars. The ticks follow your edits in Pattern: edit one pattern and only its map changes. A slot whose pattern does not exist yet has no map. <!-- QA-SNG-088, QA-SNG-089, QA-SNG-090, QA-SNG-091, QA-SNG-092 -->
4. Press `Left` or `Right`. The cursor moves one slot and the white frame follows it. The info line says, for example, **timeline slot 02 bar 5 pattern 02 4 bars**. <!-- QA-SNG-076, QA-SNG-080 -->
5. Press `Play`. A green bar under a block and a white line mark the slot and the step that play. <!-- QA-SNG-077 -->
   The info line now counts down: **timeline NEXT IN 2 BARS cursor slot 02**. It counts the bars left in the pattern that plays, and the last bar says **1 BAR**. <!-- QA-SNG-083 -->
   When you queue a jump (put the cursor on another slot with `Left` or `Right`, then press `Rec`) it says **JUMP IN**. On the last slot, where the song starts again at slot 0, it says **LOOP IN**; a single pattern playing says **LOOP IN** too. Stop, and the line goes back to the cursor's slot. <!-- QA-SNG-084, QA-SNG-085, QA-SNG-086 -->
6. Look at the thin row **ENERGY** at the foot of the page. It has one column for each slot, as high as the slot is busy and loud: for each track, how many notes it plays, times how loud they are, times the track's volume. A muted track adds nothing, and while a track is soloed only the soloed tracks add. The tallest slot in view fills the row, so the curve shows the shape of the song around the cursor, not a level you can compare between two views. <!-- QA-SNG-081, QA-SNG-082 -->

The page draws the tracks Pattern can show, at most 12, and at most 40 slots and 48 bars: with 12 tracks it shows 34 slots. The cursor's slot is always in view. <!-- QA-SNG-078 -->

### Edit on the Timeline page

New in track3r-os. The Timeline page is an editing grid. It uses the list's map with the arrows turned: `Left` and `Right` walk the slots, `Up` and `Down` the tracks. The row of blocks is the whole slot; a lane is one track of one slot, a cell.

Start on the Timeline page.

1. Press `Down`. The cursor drops into lane 1 of the slot: the cell has a box and the lane's number is bright. `Up` on lane 1 goes back to the blocks. <!-- QA-SNG-116, QA-SNG-117 -->
2. `Left` and `Right` keep the lane and move the slot. `Shift` + `Left` or `Right` select slots, `Shift` + `Up` or `Down` tracks; the block of cells you chose is on a band. <!-- QA-SNG-118 -->
3. `Copy`, `Shift` + `Copy`, `Delete`, `Shift` + `Delete`, `F5` (**UNIQUE**) and `F6` (**TOOLS**) do what they do in the list's map, including the questions on the strip. On the blocks they act on whole slots, as on PAT. <!-- QA-SNG-119, QA-SNG-120, QA-SNG-121, QA-SNG-122, QA-SNG-123, QA-SNG-124, QA-SNG-125, QA-SNG-126, QA-SNG-127, QA-SNG-128, QA-SNG-129, QA-SNG-130, QA-SNG-131, QA-SNG-132 -->
4. Hold `Copy` and press `Left` or `Right` to move the slot along the song. <!-- QA-SNG-133, QA-SNG-134 -->

The cursor is shared with the list: turn the page and it stays on the same slot and track. The Timeline has 12 lanes, so track 13 to 16 are edited on the list. <!-- QA-SNG-135 -->

### Look at Song and come back

Start on any other screen.

1. Hold `Song`. After 0.4 s the Song screen comes up. It opens on the page you left it on, for example **TIMELINE 2/2**. <!-- QA-SNG-060, QA-SNG-087 -->
2. Let go. You return to the screen you came from, as you left it. <!-- QA-SNG-060 -->

## Keys on this screen

<!-- manual-tables: keys screen=song -->
The key row, and the row drawn while `Shift` is held. `-` means the key does nothing.

| Key | Label | With `Shift` |
|---|---|---|
| `F1` | **PLAY PAT** | **0/4** |
| `F2` | **DUP PAT** | **1/4** |
| `F3` | **ADD SLOT** | **2/4** |
| `F4` | **DEL SLOT** | **3/4** |
| `F5` | **UNIQUE** | **END** |
| `F6` | **TOOLS** | **NAME** |
| `F7` | **TEMPO** | **PADS** |
| `F8` | **UNDO** | **REDO** |

<!-- /manual-tables -->

| Key | What it does | Ref |
|---|---|---|
| `F1` (**PLAY PAT**) | play the slot's pattern in a loop; `Play` plays the song from the selected slot | QA-SNG-043 |
| `F2` (**DUP PAT**) | a slot after the cursor with a copy of the pattern, the same as `Shift` + `Insert` | QA-SNG-212 |
| `F1` (**PREVIEW**) in a lane of TIMELINE | loop the slot's pattern with only the rectangle's tracks sounding; `F1` again, `Stop` or leaving the screen ends the audition and restores the solo set; not an undo unit | QA-SNG-203, QA-SNG-204, QA-SNG-205, QA-SNG-206 |
| hold `F2` | loop the selected slots; the same slots again take the loop back | QA-SNG-200, QA-SNG-201 |
| `F3` (**ADD SLOT**), `Insert` | add a slot with the same pattern | QA-SNG-050 |
| hold `Copy` + `F3` (**ADD EMPTY**) | add a slot on the first free pattern (an empty one); **NO FREE PATTERN** when the pool is full | QA-SNG-211 |
| `F4` (**DEL SLOT**) | removes one slot, in the list and in the map too | QA-SNG-012, QA-SNG-028 |
| `Delete` | removes one slot in the list. In the map it clears the cells of the selection instead | QA-SNG-012, QA-SNG-028 |
| `F5` (**UNIQUE**) | give a shared slot its own copy | QA-SNG-034 |
| `F6` (**TOOLS**) | open the block tools | QA-SNG-029 |
| `F7` (**TEMPO**) | put the encoder on the tempo (1 BPM a detent, `Shift` for 0.1); `F7` again taps the tempo, two taps under 1.5 s apart set it (40 to 800 BPM); `F8` takes the edit back | QA-SNG-062 |
| `F8` (**UNDO**) | undo the last Song edit | QA-SNG-013 |
| the encoder | change the slot's pattern number, or the tempo after `F7` | QA-SNG-062 |
| `Right`, `Left` | enter and leave the map, walk the tracks (on Timeline: `Down` and `Up`; `Left` and `Right` walk the slots) | QA-SNG-020, QA-SNG-021, QA-SNG-116 |
| `Shift` + arrows | select slots and tracks | QA-SNG-022 |
| `Copy`, `Shift` + `Copy` | copy, paste | QA-SNG-023, QA-SNG-024 |
| `Shift` + `Delete` | cut | QA-SNG-053 |
| hold `Copy` + `Up` or `Down` | move the slot or range (on Timeline: `Left` or `Right`) | QA-SNG-056, QA-SNG-133 |
| `Note`, `Shift` + `Note` | next and previous use of the pattern | QA-SNG-058 |
| `Rec` while playing | queue a jump to the cursor's slot | QA-SNG-043 |
| `Shift` + `F1` to `F5` | jump the cursor to a quarter and to the end | QA-SNG-002 |
| `Shift` + `F6` (**NAME**), `F7` (**PADS**), `F8` (**REDO**) | name the pattern, switch the pads, redo | QA-SNG-010, QA-SNG-040 |
| hold `Song` | peek at Song until you let go | QA-SNG-060 |

## Good to know
<!-- sync: src=app/src/song_actions.cpp qa=QA-SNG-010,QA-SNG-013,QA-SNG-061 -->

- `Song` acts when you let go of it, because it waits to see whether you hold it. A press of 0.4 s or more on Song does not turn the page. <!-- QA-SNG-061 -->
- Coming back from a peek enters the screen again: a question open on File is closed. <!-- QA-SNG-060 -->
- `Shift` + `F3` jumps to 2/4. It does not add a slot. <!-- QA-SNG-003 -->
- **NO SUCH PATTERN YET** and **SONG IS EMPTY** are the strip's replies to a name on a slot with no pattern, or on an empty song. <!-- QA-SNG-010 -->
- `Pattern` on Song opens the pattern of the slot under the cursor. <!-- QA-SNG-063 -->
- Adding, deleting or moving slots while the song plays does not move the playhead: the marker follows its slot and the song goes on to the slot that came after it. Delete the slot that plays and its pattern finishes, then the slot that took its place plays. Delete the queued slot and the strip says **QUEUED SLOT DELETED**. New in track3r-os. <!-- QA-SNG-064,QA-SNG-065 -->
- `F8` undoes Song edits only. An empty Song undo says **NOTHING TO UNDO HERE**. <!-- QA-SNG-013 -->
- New in track3r-os: the map, TOOLS, UNIQUE, the marks, cut, move, next use, the pads and the peek. <!-- og: new -->
- Not checked against the original firmware: the keys and pages not named above. <!-- og: open -->

## See also

- [Pattern](pattern.md): the notes of each pattern. A slot holds one.
- [Pattern tools](pattern_tools.md): the same block tools on a single pattern.
- [Perform](perform.md): play the pattern live, without writing it.

<!-- Rows not written: QA-SNG-067 to QA-SNG-072 (retired 2026-10-30 with the STAGE page, OS-SONG-VIEW2); QA-SNG-079 (host only: no key sequence reaches an empty TIMELINE). -->
