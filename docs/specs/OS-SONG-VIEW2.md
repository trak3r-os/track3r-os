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
