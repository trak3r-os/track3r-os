# OS-SONG-VIEW2: STAGE leaves the Song cycle, a simpler TIMELINE with a note map

Level B (main loop and display list; no format change, no audio path). Approved by the maintainer on 2026-10-30 ("dale"), after the unit check of OS-SONG-VIEW (`build-images/v1.0.1-songview.1/REPORT.md`, not in git) and a Fable design opinion. New Community OS behaviour. Follows docs/specs/OS-SONG-VIEW.md (D1-D11).

## Why

On the unit the maintainer found TIMELINE odd ("so many black-and-white patterns") and STAGE irrelevant. The opinion, from the tester's photos:
- the twelve density lanes (0..15 in a 15 px lane) read as on / off stripes, not information;
- the four fills chosen by `(id-1)%4` carry no meaning (patterns 1 and 5 share a colour) and dominate the page;
- the energy strip is nearly flat;
- nothing shows rhythm.
STAGE fails its own brief: about 20 px type that the header already duplicates, 100 empty rows, and one more tap on every pass back to ORDER.

## Decisions (the maintainer, 2026-10-30, on the opinion's recommendation; details are defaults to confirm)

| # | Question | Answer |
|---|---|---|
| V1 | STAGE | Leaves the Song page cycle: ORDER, ARRANGEMENT, TIMELINE (n/3). Its code and QA rows go (QA-SNG-067..072 marked retired, never reused) |
| V2 | The countdown | Moves to TIMELINE's info line while playing, e.g. `NEXT IN 2 BARS` (the bar arithmetic of `song_stage` stays, host-tested) |
| V3 | Blocks | A neutral Pane fill with the pattern number always drawn; the pattern colour shrinks to a 4 px strip under the number (repeats still read; colour is never the only cue) |
| V4 | Note map | Replaces the density thumbnails: one bitmap per pattern, one row per track (12), one column per step (up to 128), a tick where a step holds a note; drawn as an FT812 bitmap scaled to the block (as the Wave view's bitmap, `ui/plot/wave_bitmap.h`); built once per `pattern_generation` (the density cache's trigger), never per frame |
| V5 | Energy | A thin row (about 10 px) at the foot; the density cache stays for it |

## Plan

- **Slice 1 (S): V1, V2.** Remove the STAGE page from the cycle and its drawing (keep the pure bar arithmetic used by V2); the info line shows the countdown on TIMELINE while playing; page labels n/3; scenarios, goldens, QA rows (retired, new), manual (docs/manual/song.md), ui-map.
- **Slice 2 (S): V3, V5.** The de-clutter; no display-list growth.
- **Slice 3 (M): V4.**
  - Step 0: read the RAM_G map (`eve/`, the fonts, the Wave bitmaps, the themes) and say where 255 x 192 B (about 48 KB, L1 at 128 x 12) fits, or a smaller cache: only the patterns in the song, or only those in view, with an LRU.
  - Then the bitmap writer (main loop, sliced if a frame budget needs it), the per-slot BITMAP_SOURCE / LAYOUT / SIZE / VERTEX2F and one BITMAP_TRANSFORM pair per page.
  - Re-measure `song/timeline_worst` (estimate about 800 words of 1792).
  - Host tests on the bitmap bits (a step with a note sets its bit; empty steps and tracks past 12 clear it; the cache invalidates on every pattern writer).

## Unit check of slices 1 and 2 (2026-10-30, `build-images/v1.0.1-songview2.1/REPORT.md`, not in git)

PASS: the cycle reads 1/3 to 3/3 with STAGE gone; the countdown agrees with the playhead and reads 1 in the last bar; LOOP and JUMP as specified; a 12-track, 34-slot song for 90 s with no display garbage and 0 underruns. Simpler than the `songview.1` build, by the tester's panel shots (Amiga, Black, White). Inputs for slice 3:
- the 12 dim lanes still fill about two thirds of the page as grey rectangles and show no rhythm: the note map replaces them;
- the numbers are about 7 px; at 48 bars the one-bar blocks run together (`7 8 1 2`);
- the strip has 4 colours only, so patterns 1 and 5 look alike: consider more strip colours, or the number alone as the key;
- the energy row is nearly flat on the test songs: rescale it or drop it;
- in the White theme the lanes read heavy.

## Done when

Each slice is merged with its scenario, goldens, QA rows and manual section; the display-list worst case is measured; slice 3's RAM_G use is recorded. Unit: how the note map reads on the panel, and that the frame stays clean at the worst case.

## Deviations from the decisions (found in review; defaults to confirm by the maintainer)

- **V3, "the pattern number always drawn":** a block narrower than its number (a one-bar slot of a 48-bar window with a 3-digit pattern id: about 9 px of block for a number about 14 px wide) goes without its number. Drawing it anyway would run over the next block's number, and a second row for it would grow the display list; the colour strip and the cursor's info line still name the pattern. Default: keep the omission. Confirm, or ask for a clipped or abbreviated number.

## Slice 3, the note map (V4): what was built and measured (2026-10-31)

New Community OS behaviour, Level B, no format change (nothing is saved; the pictures live in RAM_G only).

**RAM_G (step 0).** The map is `ui/plot/include/o3os/ui/plot/ram_g_map.h`: faces and tiles to 0x0F000, the four wave slots (4 x 96 KiB) to 0x6F000, the boot logo (8 KiB) to 0x71000, and from there the spare region, 572 KiB, whose only user was the screenshot (`kRamGShot`, two bands of 240 rows RGB565 = 384,000 B, or six of 80 rows at 8888 = 256,000 B; with the note map in front of it the region is 476 KiB less 64 B, so the 8888 default takes 7 bands of 69 rows and the RGB565 shot still 2 of 240). Nothing else is placed there, so the whole table fits with room to spare: no LRU, no "only the patterns in view".

**The choice, default, to confirm by the maintainer.**
- One picture per pattern id (255 of them), at `kRamGNoteMap + (id - 1) x 384` B, 0x71000..0x89000 (96 KiB, on the 4 KB upload grid), taken from the front of the old spare region; the screenshot's region (`kRamGShot`) now starts at 0x89000 and is 476 KiB (487,360 B) less the self-test's 64 B, which still holds both band layouts. A picture is 128 columns (steps) x 24 rows at L1: two rows a track (the ticks, then a blank row), 12 tracks. The brief said 128 x 12 (192 B, 48 KiB); the blank row between tracks makes the tracks read as separate lanes with no display-list word (full-lane ticks touch each other), at the price of the other 48 KiB, which RAM_G has. Reversible: the picture's rows are one constant (`kNoteMapRows`) and the page's `notemap_row_top` arithmetic.
- No LRU: a table by pattern id needs no eviction and the draw needs no lookup; `app::SongNoteMap` keeps one generation a pattern id (1 KB of SRAM in `ViewState`, no other static buffer; the 384 B render buffer is on the stack).
- A tick is a note byte 1..128 (as the density and the energy count): a fade, cut, off, a reserved byte or a step with only an instrument, a volume or an FX has none. Steps past a track's own length and tracks 13..16 are not in the picture.
- **Tracks past 12 are not shown in the map** (default to confirm by the maintainer: the map and the lanes follow Pattern's 12 visible tracks, `PatternView::kMaxVisibleTracks`). The page draws at most 12 tracks and 128 steps and never samples past the picture: the drawn height ends at the last lane (`notemap_row_top(2 x tracks)`), the drawn width is clipped by `notemap_fit_width` so that the last pixel samples column 127 at most (A rounds up). Host-tested for every lane and block width in `song_notemap_test`, with a 16-track pattern.
- Written on the main loop, inside the page's draw, when the pattern's generation (`Project::pattern_generation`, which every pattern writer moves; the test walks them all) differs from the one recorded: one 384 B `Display::upload` per pattern, once per generation. Sliced across frames (review fold, ADR 0010: uploads of at most 4 KB per pass): at most `SongNoteMap::kMaxPerFrame` = 8 pictures (3 KB) a frame, none more in that frame after the first Busy, and the app redraws (`SongNoteMap::pending()`, read by `App::render`) until all are in, as the pad lights do (2 a pass). A build is the same scan of the pattern the density cache already does per generation (12 x 128 note-byte reads, about 0.15 ms from SDRAM, estimated, not measured); a song of 40 distinct patterns fills in over 5 frames (about 1 ms of builds a frame, to measure on the unit); the repeated patterns of a real song cost nothing. A slot whose picture is not uploaded yet (waiting for its frame, or a Busy display) draws as an empty map, never a stale picture: `ensure` says false and the page draws no bitmap for it, also when the pattern's old picture is still in RAM_G. The index is dropped for another project or another display (a display that re-initialises restarts the whole app, so its caches are new).
- An edit of a pattern while the page is on screen overwrites its picture in place: the one frame the chip scans out during the upload may show old and new rows mixed (the contract of `hal::Display::upload`). Not worth a double buffer; to look at on the unit.
- Drawn as one raw section for the page (SAVE_CONTEXT, BITMAP_HANDLE, LAYOUT, TRANSFORM_E, colour, BEGIN, CELL: once) and, per slot, BITMAP_SOURCE, BITMAP_SIZE, BITMAP_TRANSFORM_A and VERTEX2F (4 words). The brief said "one BITMAP_TRANSFORM pair per page"; the horizontal scale A depends on the block (16 steps a bar over its width, so a step is the same width in every block), so A is per slot and E per page. 8.8 precision, as the boot logo's (the 1.15 form of the FT81x is not modelled by the simulator and not verified here): a tick may sit up to half a step off at the far end of a wide block; the lane is `ceil(512 / lane)` rows tall, so it is not always a whole number of pixels and the page takes each lane's top from `notemap_row_top` (host-tested against the chip's sampling). The handle is the second wave lane's (`kHandleNoteMap`): the Song page draws no wave.

**Display list.** `song/timeline_worst` (64 variants: every track count 1..16, 40 one-bar slots, cursor and playhead at two places, every theme): **891 words** in the VTX2F mode (817 translated) of 1792, variant 34, was 1500 before (the estimate was about 800). `song/timeline` (12 tracks): 683 (604 translated), was 863. The slot cap (620 rectangle-equivalents, 12 tracks show 34 slots) is unchanged: it keeps the window as it was; the room can buy more slots later.

**Size (mk66 image, against the last build of dev code, 957,844 B).** FLASH 958,916 B (+1,072 B, under the ~3 KB planned), SRAM_U 135,520 B (+0), SRAM_L 49,672 B (+0), SDRAM_STATIC 160,288 B (+1,024 B: the generation index of `SongNoteMap`, inside `ViewState`). RAM_G: +96 KiB used (0x71000..0x89000), the spare region 572 KiB -> 476 KiB.

**Tests.** `tests/song_notemap_test.cpp`: a step with a note sets its bit (the byte and the bit, MSB leftmost), empty steps, data-only steps, fades, cuts, offs and reserved bytes clear it, the odd rows stay blank, steps past a track's length and tracks past 12 are clear, the address and the bytes of an upload, one upload a generation, a rebuild after each of `set_step`, `set_steps`, `set_pattern_name`, `set_track_length`, `set_pattern_length`, `clear_pattern`, `create_pattern` (after a delete) and `reset`, another project or display starts over, a refused upload is retried, the scaling arithmetic (and, by brute force, that every picture row owns whole pixels the chip's sampling lands in). `plot_ram_g_test` holds the new region. Scenario `qa_song_notemap` (QA-SNG-088, 090) and the gallery's `song_timeline` pictures.

**Follow-ups, not in this slice** (the tester's other notes on `v1.0.1-songview2.1`):
- the numbers are about 7 px and at 48 bars the one-bar blocks run together (`7 8 1 2`): a bigger or abbreviated number;
- the strip has 4 colours only, so patterns 1 and 5 look alike: more strip colours, or the number alone as the key;
- the energy row is nearly flat on the test songs: rescale it or drop it;
- the White theme reads heavy (the ticks are Text on Pane; to look at with the note map on the panel);
- consecutive steps with notes (a 16th-note track) run together into a solid bar at 1 to 3 px a step (track 4 of the demo, `qa_sng_088`); a doubled-column picture (256 columns, 768 B, ticks in the left half of each step) would leave gaps from 2 px a step up, at 192 KiB of RAM_G and nothing on the display list; left for the maintainer to ask for once he has seen the panel;
- raise the slot cap now that the page costs 891 words, if the maintainer wants a wider window;
- the 1.15 transform (P bit) would remove the half-step drift; needs a unit check and the simulator's model.

**Unit checks left.** How the note map reads on the panel (1 px steps at 16 px a bar, ticks half a lane tall, Text colour in the three themes); that the frame stays clean at the worst case and while the first draw of a 40-pattern song uploads its pictures; an edit while the page is on screen (no more than one mixed frame); a `diag shot` with the page on screen (the screenshot's region moved to 0x89000); the cost of the first draw.

## Latent (slice 3 review, 2026-10-31)

- A display that stays Busy (not failed) keeps `SongNoteMap::pending()` true, so the Song page redraws every pass with one upload attempt a frame (at most one 70 ms `wait_quiescent`). Bounded per frame, not rate-limited; a display stuck that way would already stall the main loop. A later fix: stop re-arming `dirty_` after N consecutive all-Busy frames.
