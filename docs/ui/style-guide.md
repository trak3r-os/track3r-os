# UI style guide v1

Status: v1, OS-003b (2026-10-11); OS-002b (2026-10-11) adds the Song screen, dialogs, the notification strip and how a screen
adds modes and bindings (sections 5a, 5b, 9); OS-003d adds the plots (section 11). Source of truth for the look of every Community OS screen. The code is in `ui`
(tokens, themes, faces, renderer, components, `ui/plot`) and `app` (the pattern view and its layout); the decisions behind it are
ADR 0001 (visual direction), ADR 0010 (rendering toolkit, amendment 1: themes) and ADR 0009 (actions and bindings).

**The north-star test (ADR 0001):** *if OctaMED could have looked like this on an impossibly powerful 1990s Amiga, we are
on the right track.* Every screen review asks it first. Screens are judged in the gallery, in all three themes
(`o3sim --gallery <dir>`; see "How to judge a change").

## 1. Principles

- Flat filled panels, one-pixel rules, rectangular controls, dense monospace data, one strong highlight, a small fixed
  palette. No gradients, glows, shadows, rounded corners or antialiased text.
- **Two materials.** *Chrome*: light, desaturated grey-blue strips with dark text (status strip, track headers, info line,
  key row). *Data pane*: dark, where the grid lives. Chrome frames the work, the pane is the work.
- **One type size per role, four roles** (section 3). A screen never draws everything in one size.
- **Colour has jobs.** Blue is the selection, orange is the cursor, record-arm and (OS-003d) a clip and nothing else, green is "playing" and
  level, the other hues tell the columns apart. Everything else is grey.
- Hardware first: 800 x 480 at about 135 ppi, read at arm's length, driven by pads, buttons and an encoder. Nothing
  actionable below a 9 px cap height; no interaction that needs a pointer.

## 2. Grid and metrics

Pixels, on a 4 px base. These are the `Metrics` of every built-in theme (`ui/theme.h`); a layout derives its rectangles
from them and from the faces' geometry and holds no literals of its own (`app/pattern_layout.h`).

| Metric | px | Use |
|---|---|---|
| `border` | 1 | a rule, a border |
| `pad` | 4 | inside a chrome strip or a badge |
| `gap` | 4 | between fields of a row; a group gap is 4 gaps (16) |
| `row_pitch` | 24 | a grid row; the Data face's 20 px cell sits inside it (capitals centred) |
| `strip_h` | 32 | the top strip, so the 16x32 Display cell fits with its capitals (20 px) centred |
| `header_h` | 26 | track header: badge row, the meter (a 4 px bar in a 1 px frame: 6 px), 2 px selection underline |
| `info_h` | 16 | the info line |
| `keys_h` | 22 | the key row: an 18 px key-cap in 22 |
| `index_w` | 30 | the row-number column |
| `meter_h` | 4 | the track meter |

The pattern screen, top to bottom: strip 32 + header 26 + grid 16 x 24 = 384 + info 16 + keys 22 = **480**. Across: a 30 px
row-number column and 5 track columns of 154 px. A track column starts with a 1 px separator; its fields are centred in
it: note tile 30 px, instrument 20, volume 20, FX1 30, FX2 30, four 4 px gaps: 146 px. All of it is whole Data cells plus
gaps, which is what lets 5 tracks fit 800 px at 10 px a cell (a text string with a space between fields needs 170 px a
track and fits 4).

- **Beats and bars.** There is no banding. A beat (every 4th step) is a bright row number (`Text`; the others are
  `TextDim`). A bar (every 16th step, four beats) is a clear one-pixel rule across the pane in `TextDim`, the colour of the
  dim row numbers: well above the quiet `Empty` of the track separators and empty fields.
- **Empty fields** are dashes in `Empty`, about 2:1 against the pane: there, not shouting.
- **The selected row** is a full-width `Sel` bar; the row's text is `SelText`; the cursor's field is an `Accent` block
  (the field grown by 2 px, 3 px of air above and below the capitals) with `AccentText` text and a one-pixel
  `AccentText` outline all round (section 4, Contrast: the outline is what sets orange off the blue).
- **The selected track:** its header name is `ChromeText` (the others `ChromeDim`) and it has a 2 px `Sel` underline.

## 3. Type scale

Four roles, all L1 pixel faces drawn at native size (ADR 0010 section 3). A theme says which compiled face plays each role.
L4 coverage is for MCU-rendered curves and waveforms, never for text.

| Role | Face | Cell | Caps | Source | Use |
|---|---|---|---|---|---|
| Display | `f16x32` | 16 x 32 | 20 px | Terminus Bold (OFL 1.1) | BPM, pattern number, the value being edited |
| Data | `f10x20` | 10 x 20 | 13 px | Terminus Bold (OFL 1.1) | the grid, lists, parameters |
| Label | `f8x16` | 8 x 16 | 10 px | Spleen (BSD-2-Clause) | labels, the info line, key-caps, track names |
| Micro | `f6x12` | 6 x 12 | 8 px | Spleen (BSD-2-Clause) | axis ticks only |

Rules: the data capital is at least 12 px and a label at least 9 px; **nothing the user must read or act on is set in
Micro**, and no label is set in Data or Display to "fill space". Mixed faces share a baseline (`FieldRow`, `ui::baseline_y`)
and a box centres by capital height (`ui::caps_y`), not by cell. Faces are named by cell size, never by font: the Terminus
bitmaps are a converted (Modified) version in the sense of the OFL and may not carry its Reserved Font Name.

## 4. Palettes

Sixteen colour tokens, `Rec` and `SelDim` (`ui/style.h`; `Rec`, the 17th, is the one red of Rec mode, added at the maintainer's request of 2026-10-13 past the sixteen of ADR 0001; `SelDim`, the 18th, is the duller selection blue of a multi-track block, OS-SELTINT: they are the last ids and the module SDK keeps its 16), valued by each theme (the tables are in `ui/src/themes.cpp`, the only place colour
values are written; `tools/check_layers.py` enforces it). **Amiga** is the default: Workbench grey-blue chrome with dark
text, a near-black data pane with a faint blue cast, one strong blue, one orange. **Black** is high-contrast for the stage.
**White** is for daylight (hues and orange darkened until they read on white). `Shift + Master` (`ui.theme.next`,
provisional until Config exists) cycles them without a reboot; `theme <name>` in a scenario and `o3sim --theme <name>` set one.

| Token | Amiga | Black | White |
|---|---|---|---|
| `Chrome` | #B2BACA | #262932 | #D4D9E4 |
| `ChromeText` | #0E121C | #ECEEF4 | #10141E |
| `ChromeDim` | #3C4660 | #9298A8 | #545C70 |
| `Pane` | #0A0D14 | #000000 | #FAFAF8 |
| `Text` | #E6EAF2 | #FFFFFF | #10141E |
| `TextDim` | #80889C | #969CAC | #60687C |
| `Empty` | #3E4454 | #464A56 | #B0B4BE |
| `Instr` | #7EC4F0 | #6EBEFF | #0A5CAA |
| `Vol` | #96D684 | #82EC78 | #147828 |
| `Fx` | #D4A6EE | #E696FF | #8232AA |
| `Sel` | #1C5CD0 | #246CEE | #245CC8 |
| `SelText` | #FFFFFF | #FFFFFF | #FFFFFF |
| `Accent` | #FF8A00 | #FF9614 | #BE5000 |
| `AccentText` | #0E0E12 | #000000 | #FFFFFF |
| `Play` | #54C85C | #46DC5A | #1C8034 |
| `PlayText` | #0A180C | #001400 | #FFFFFF |
| `Rec` | #BE1E1E | #DC3434 | #C42020 |
| `SelDim` | #2C5CB8 | #2C62C8 | #3864BC |

Tokens and what they are for: `Chrome` fill of strips, headers and the key row; `ChromeText` primary text on chrome and the
fill of inverted key-caps and type badges; `ChromeDim` secondary text and rules on chrome, the fill of the STOP badge;
`Pane` the data pane and the slot behind a meter; `Text` notes, beat row numbers, meter peak ticks; `TextDim` other row
numbers, bar rules; `Empty` empty fields and the track separators; `Instr`, `Vol`, `Fx` the column hues (none is orange);
`Sel` the selected row and underline; `SelText` text on it; `Accent` the cursor block and record-arm; `AccentText` text on it
and the cursor's one-pixel outline; `Play` the PLAY badge and meter fill; `PlayText` text on it; `Rec` Edit mode of the Pattern screen (NEW Community OS behaviour; OS-RECSIGN: the signs show whenever the pattern can be edited, STEP and LIVE): a three-pixel frame round the grid and the REC badge's fill (record icon and "REC" in `SelText`); in LIVE, while it records, also the
playhead's bar (in place of `Play`, its text `SelText`). `SelDim` (OS-SELTINT) the tracks of a block over several tracks other than the cursor's track, which keeps `Sel`; a desaturated blue, not a darker one, so that it holds 3:1 against the pane in every theme; its text is `SelText`.

### Contrast

Every pair of tokens that a component or screen draws over each other is listed in `ui/theme.h` (`kContrastPairs`) and
checked for every theme by the `contrast` ctest (`tests/theme_test.cpp`): at least **4.5:1** for data and labels (WCAG
relative-luminance ratio), at least **3:1** for Display text and non-text indicators (the selection, the cursor, a meter), and
about **2:1** (1.6 to 3.0) for the decoration that must stay quiet. Another test holds orange to the accent. An SD theme
pack must pass the same check on load or it is rejected with a notification.

| Ink on background | Role | Needs | Amiga | Black | White |
|---|---|---|---|---|---|
| `ChromeText` on `Chrome` | Text | 4.5 | 9.60 | 12.52 | 13.01 |
| `ChromeDim` on `Chrome` | Text | 4.5 | 4.81 | 5.04 | 4.72 |
| `Chrome` on `ChromeText` | Text | 4.5 | 9.60 | 12.52 | 13.01 |
| `Chrome` on `ChromeDim` | Text | 4.5 | 4.81 | 5.04 | 4.72 |
| `Sel` on `Chrome` | Large | 3.0 | 3.08 | 3.08 | 4.32 |
| `Text` on `Pane` | Text | 4.5 | 16.12 | 21.00 | 17.61 |
| `TextDim` on `Pane` | Text | 4.5 | 5.48 | 7.65 | 5.33 |
| `Instr` on `Pane` | Text | 4.5 | 10.22 | 10.47 | 6.43 |
| `Vol` on `Pane` | Text | 4.5 | 11.34 | 14.22 | 5.36 |
| `Fx` on `Pane` | Text | 4.5 | 9.71 | 10.17 | 6.73 |
| `Pane` on `Text` | Text | 4.5 | 16.12 | 21.00 | 17.61 |
| `Empty` on `Pane` | Dim | 1.6-3.0 | 2.00 | 2.37 | 1.99 |
| `Play` on `Pane` | Large | 3.0 | 9.06 | 11.65 | 4.80 |
| `Accent` on `Pane` | Large | 3.0 | 8.22 | 9.62 | 4.63 |
| `Sel` on `Pane` | Large | 3.0 | 3.23 | 4.45 | 5.84 |
| `SelText` on `Sel` | Text | 4.5 | 6.02 | 4.71 | 6.10 |
| `Sel` on `SelText` | Text | 4.5 | 6.02 | 4.71 | 6.10 |
| `AccentText` on `Accent` | Text | 4.5 | 8.15 | 9.62 | 4.84 |
| `AccentText` on `Sel` | Large | 3.0 | 3.20 | 4.45 | 6.10 |
| `PlayText` on `Play` | Text | 4.5 | 8.52 | 10.59 | 5.02 |
| `Rec` on `Pane` | Large | 3.0 | 3.14 | 4.58 | 5.62 |
| `Rec` on `Chrome` | Large | 3.0 | 3.17 | 3.17 | 4.15 |
| `SelText` on `Rec` | Text | 4.5 | 6.19 | 4.58 | 5.87 |
| `SelDim` on `Pane` | Large | 3.0 | 3.08 | 3.69 | 5.41 |
| `SelText` on `SelDim` | Text | 4.5 | 6.31 | 5.69 | 5.65 |

The table lists exactly the pairs the code draws over, or directly against, each other, with the place for each in a
comment beside it in `ui/theme.h`: the chrome text and the inverted badges, key-caps and chip; the PLAY badge and the
meter slots, which carry a one-pixel `ChromeDim` frame (a green lamp and a dark slot would otherwise melt into the chrome,
1.10:1 and 1.35:1); the pane's columns; the selected row; the cursor; the meter fill and its peak tick. **The cursor is the
one place the colours alone do not do it:** the orange `Accent` block lies on the blue `Sel` row at 2.55:1 (Amiga),
2.16:1 (Black) and 1.26:1 (White), under the 3:1 of an indicator in every theme, so the block has a one-pixel
`AccentText` outline, and it is the outline (3.20, 4.45 and 6.10:1 against the row) that makes it an indicator. A test keeps the table honest by deriving it
from pictures: every two tokens that touch in a rendered frame (the pattern view in several windows and cursors;
the style specimen) must be in the table or be a named decoration (a one-pixel rule or frame running beside something), and
every pair in the table must really touch somewhere (`tests/pattern_view_test.cpp`). Minimum of
the table, the dim decoration apart: **Amiga 3.08 (`Sel` on `Chrome`), Black 3.08 (the same), White 4.32 (the same)**.

## 5. Components

`ui/components.h`; each draws from a rectangle or a position the screen's layout gives it, in tokens, and keeps no state.

| Component | What it is |
|---|---|
| **Strip** | a chrome band with a one-pixel `ChromeDim` rule on the edge that meets the pane; the top band is strip + header |
| **Badge** | a filled rectangle with its text centred: the transport badge (PLAY green with `PlayText`, STOP the grey of `ChromeDim`, with its icon, both in a one-pixel `ChromeDim` frame) and the track-type badge (`A` audio, `M` MIDI, `E` external audio, inverted: `ChromeText` fill, `Chrome` letter) |
| **SoftKeys** | F-keys as key-caps: an inverted "Fn" box and the label beside it, eight equal caps; three colour passes, three `COLOR_RGB` words. The labels belong to the caller |
| **FieldRow** | captions and values on one baseline: a dim Label caption, the theme's gap, the value in any face, a group gap. `chip` / `field` with a fill (OS-BUG-LEGIB): the value on a chip, a gap each side and two borders over and under the capitals; the strip's mode word (EDIT of Perform, REC of Record, STOPPED of a module app) is an `Accent` chip with `AccentText`, since the orange is 1.2:1 on `Chrome` in Amiga (the pair is `AccentText` on `Accent`, above) |
| **Bar** | a track colour and a fill colour, the fill by value / max |
| **Meter** | a one-pixel `ChromeDim` frame round a `Pane` slot, the level in `Play` and a two-pixel peak tick in `Text`; the tick never lies on the fill (it sits at the peak, or just past the fill when the peak is at or below the level; none for a silent track or a full bar). A 4 px bar in the header (6 px with the frame), fed by fake demo levels until the audio ring exists |
| *Status chip* | an inverted `ChromeText` chip, 12 px high with a pixel of air under the band's edge rule, with a `Chrome` label: the notification strip's text (section 5a; a general component arrives with OS-003c). The pattern view's info line carries one again since OS-009 (section 5c): the mode, WATCH, STEP (Rec) or LIVE (Shift + Rec) since OS-RECMODE2 (earlier BROWSE, EDIT FOLLOW and EDIT FREE, OS-RECSWAP, which renamed EDIT ON; FOLLOW was retired in OS-004d and SELECT in OS-LIVEREC); its messages are still notifications |
| *Chrome helpers* (`app/chrome.h`) | the two chrome bands, the status strip's skeleton (a `FieldRow` on the Display baseline, group dividers, the transport badge, CPU at the right edge) and the info line (left text, right text), shared by the pattern view and the Song screen; a third screen calls them too |

## 5a. Overlays, dialogs and the notification strip (OS-002b)

One overlay at a time lies over the base screen, and it declares the rectangle it covers (`app/screens/overlay.h`). The base
screen skips every row, field or component entirely inside that rectangle and draws the rest, which the overlay then paints
over: so an overlay costs only what it adds, and a dialog that is a whole number of rows tall cuts the grid at its sides only,
never through a row's glyphs (`tests/overlay_test.cpp` counts the words and holds the pixels equal with and without skipping).

- **Dialog** (Learn, the Navigator): chrome material in a one-pixel `ChromeDim` frame, a ring of `Chrome` inside it, an
  inverted title bar (`ChromeText` fill, `Chrome` title in the Data face, one row high), the body in the Label face
  (`ChromeText`, hints in `ChromeDim`), key hints as inverted key-caps (`F8 BACK`: the `ChromeText` box with its F-number in
  `Chrome`, as in the key row). Centred across, on whole rows of the pane vertically; every size comes from the theme's
  metrics. The frame lies beside whatever it cuts through, which makes it a named decoration (one-pixel `ChromeDim` beside the
  grid's ink; the rendered-pair test lists exactly which).
- **Navigator cell** (a grid of the registry, 4 columns): a dark `Pane` cell in a one-pixel `ChromeDim` frame, the icon and the
  name in the Data face, and the screen button's name as an inverted key-cap under it. The chosen cell is `Sel` with `SelText`
  ink; a screen that does not exist yet is `TextDim` (the footer says so). The key-cap is the Text-on-Pane pair read the other
  way round (`Text` fill, `Pane` letters; `TextDim` and `Pane` for a disabled cell; `SelText` and `Sel` for the chosen one), so
  the contrast table has nothing new.
- **Notification strip**: exactly the info line's rectangle (the info line is not drawn under it), the band's top rule, a
  12 px severity tile (`i` on `ChromeText`, `!` on `ChromeDim`, `X` on `Sel`: the palette has no red and keeps its sixteen
  tokens, so severity is the tile's fill and its letter), the text as an inverted chip (like the status chip) and the source in
  `ChromeDim` at the right edge. 48 characters of text is the longest it carries; the same notification posted again is one
  strip with a count after the text ("F3 JUMP x3").
- **Contrast.** No new token pair: dialogs and the strip use the chrome pairs, the Navigator the Text / TextDim / Sel pairs on
  `Pane`; `tests/pattern_view_test.cpp` renders the Song screen and every overlay over the pattern and the song in every
  state it has and holds them to the table.

## 5b. The Song screen (OS-002b)

The order list from static demo data (editing it is OS-004), laid out on the same bands as the pattern view (`app/frame.h`: the
strip with the SONG title in the Display face, the transport badge, LEN / POS / PLAY / PAT fields and CPU; the header with the
columns' names, the cursor's column bright with the 2 px `Sel` underline and the page at the right; the pane of 16 rows at
the 24 px pitch; the info line; the key row). A row: the play marker (the `Play` icon in `Play`, in `SelText` on the selected
row), the position (a tile, bright every fourth row like the pattern's step numbers), the pattern number (a tile in `Instr`,
the cursor's `Accent` block with the `AccentText` outline on the selected row, like the pattern grid's cursor), the pattern
name in `Text` and its length in `Fx`, both Data glyphs. Since OS-SONG3 S1 the page also has the start bar (BAR) and the length in bars (BARS) of each slot, in `Text` and `Fx` (the old ARRANGEMENT page's two numbers; its `Bar` graphic is gone); page 2 (Song pressed again) is TIMELINE. The F-key labels are the
screen's own (at most 8 characters: that is what a key-cap has room for).

## 5c. Pattern editing: the block, the marks, the info line, the Shift layer, the choosers (OS-009 P1a.1)

No new token and no new pair of the contrast table: everything below is the pairs the table already has, and
`tests/pattern_view_test.cpp` (the rendered-pairs test) draws these pictures too and fails on one that is not in it.

- **The selected block**: one `Sel` rectangle a visible row across the selected tracks (the cursor's row has its own bar), the
  fields inside it in `SelText`, as on the selected row. Over several tracks (OS-SELTINT) the cursor's track keeps `Sel` and the other tracks are `SelDim`, a duller blue: at most three rectangles a row (the tracks before the cursor's, the cursor's, the tracks after), the dimmed ones also over the cursor row's bar; a block in one track, or a column block, is `Sel` throughout. Two display-list words a row for one run, no colour word between them and the bar. A
  step past its track's own length (the model keeps them; D7 of the spec) is drawn in `Empty`, whatever it holds.
- **Header marks**, in the badge row at the right end of each track column, each in a place of its own so that one mark toggling
  moves none of the others: `M` muted and `S` soloed in `ChromeText` (the `R` of an armed track, in `Accent` on a `Pane` slot, went with the track arming: OS-LIVEREC; its place stays empty).
  With Shift held the marks that are set are drawn twice, a pixel apart: bold. The track's name is clipped to what the marks leave.
- **The info line**: `scale <name> <root>   track n` in `ChromeText`, the mode as an inverted chip (`ChromeText` fill, `Chrome` text, the
  notification's own chip), and `undo n` at the right edge (the journal's depth: no denominator, the OS-001b `12/64` was a mock).
  A notification still replaces the line while it is shown; so does a chooser.
- **The Shift layer**: while Shift is held and no overlay is up, the key row says what Shift + F1..F8 do (`0/4 1/4 2/4 3/4 END - - REDO`).
- **The key row under a dialog or a question** (OS-KEYROWDLG, `app/key_prompt.{h,cpp}`): a modal overlay owns the row. It shows the dialog's own keys, every key it does not accept blank (not dim: dim means a key that answers and refuses with a reason on the strip), and does not follow Shift unless the dialog declares a Shift layer (only the Navigator does). `render_scene` paints it once, after the overlay, on the frame band; the screen under the dialog draws none (`ViewState::overlay_keys`). A dialog's own key-cap line is made from the same table (`draw_prompt_hints`). A question on the strip (NEW, OPEN, PURGE, OG PROJECT, Config APPLY and DEFAULTS, the launch confirm) leaves the screen's row and marks only its answer keys: the key's word, inverted (`hot`, Chrome on ChromeText as in Perform), at most 8 characters, while the question's text is the head of the strip. A new dialog or question adds one table there, and `tools/check_keytext.py` (ctest `keytext`) fails a function key named in any other string of `app/` or the image's main, but the pointers of its reviewed allow-list (an info line that names a key of its own plain row).
- **The choosers** (F1 pattern, F2 length, F3 step jump): a strip exactly over the info line, like the notification strip: the top rule, a chip
  with the title (PATTERN, LENGTH, STEP JUMP), the value in `ChromeText` and what the keys do in `ChromeDim` at the right edge.
- **Budget** (`o3sim --gallery`, every theme, translate / VERTEX2F fallback): the pattern's worst case is now a sweep of 909 variants (the 801 of the
  demo, three contents each: filled, sparse, and sparse with the dearest note tiles, alternating between the two note handles; and 108 over a
  document with every mark, the Shift layer and a block: **1,281 / 1,616**, it was 1,186 / 1,496), the block in
  the worst case 1,245 / 1,591, the dearest pattern state a chooser over the document's worst case, **1,338 / 1,703** (the fallback's highest of any screen; 89 words spare); the
  overlay sweeps of Learn, the Navigator and the notification strip are drawn over documents too. The field passes draw track by track and column by
  column, which is what keeps the fallback from paying a bitmap handle at every tile of the block's colour.

## 6. Tiles and icons

- **Tiles** (ADR 0010 section 5). A step is drawn from bitmap tiles, one display-list word a field: 256 hex-pair tiles
  (20 x 20; the instrument as a packed-decimal pair, the volume and FX value as hex, the row number) and 133 note tiles
  (30 x 20: cell 0 `---`, 1..120 C-0..B-9, 121 `OFF`, 122 `???`, 123 `CUT`, 124 `FAD`, 125..132 the notes 121..128, `C10 c10 D10 d10 E10
  F10 f10 G10`: this OS's spelling of the tenth octave where it fits three characters, a lowercase letter for the sharps, whose names
  would have four; the FX empty field is `---` too). They are composed from the Data face at boot (`ui::compose_tile`), so they are pixel for
  pixel the text they stand for (a test checks all 389), and uploaded in chunks of 1.9 KB through `hal::Display::upload`. VERTEX2II's cell
  field is 7 bits, so the hex pairs use two bitmap handles, and so do the note tiles: cells 0..127 on handle 7, 128..132 on handle 12, which
  `Renderer::note_tile` binds in the frame that first draws one of them (four words; a frame without one pays nothing). Past x = 511 the renderer moves a horizontal origin (`VERTEX_TRANSLATE_X`) a few times a pass; callers
  always pass screen coordinates. **[A] That the chip applies VERTEX_TRANSLATE_X to VERTEX2II is not in the guide, and a
  `hw-session` must confirm it before this reaches a device.** Plan B is `Renderer::use_translate(false)`: BITMAP_HANDLE +
  CELL + VERTEX2F, which needs nothing of the translation. It is kept honest by tests: the whole pattern view, in every theme and
  several windows, draws the same pixels in both modes (`ctest translate_fallback`), and the budget harness holds both to the
  1792 words (`ctest budget`; section 8 has the numbers). Translation stays the default (about 20 % fewer words).
- **Icons.** An in-house 16 x 16 L1 face of eleven icons, drawn as data in `tools/icon_convert.py`: play, stop, record,
  loop, mute, solo (headphones), lock, folder, module (a chip), MIDI (a DIN connector), warning. Recoloured with
  `COLOR_RGB`; used at native size, never scaled. No third-party icon set.
- **RAM_G map** (`ui/assets.h`): faces from 0x0000 (12,540 bytes), hex pairs at 0x4000 (two handles), note tiles after
  them, icons last; 42,736 bytes end to end when this line was written, +2,268 bytes since for the 63 three-letter FX name tiles of the 5-track Pattern zoom (18x12, after the Micro note tiles, handle 14: OS-AUDIT E-PAT-03; `kRamGEnd` is 59,852 now), every region word aligned. Handles 1..8 and 12 (the second note handle; 9..11 are the wave
  lanes and the boot logo); 0 and 15 are left alone.
- Every generated bitmap is `alignas(4)` with a static_assert and goes through `Display::upload` in chunks of at most
  4 KB (ADR 0011).

## 7. Motion

- None for navigation: a cursor move is a redraw, not an animation.
- Playhead and meters move at 30 Hz with peak hold (the peak tick falls slowly); the scope at 20 Hz; nothing live redraws
  while idle (zero SPI traffic).
- Record-arm blinks at 2 Hz (the `Accent` record icon); the cursor itself does not blink.
- A step written from a pad flashes for one frame.
- A notification shows for its time (2 s for an F-key label) in the strip over the info line and goes; the next of the four
  queued ones takes its place. Its time runs only while it is shown.
- Themes switch at once, with no transition.

## 8. The display-list budget

The chip's RAM_DL holds 2048 words; **every worst-case state of every screen in every theme, and in both ways of reaching
x >= 512, stays at or below 1792** (256 of margin for a longer label or another field). The pattern view is measured by a
534-variant sweep (it was 2,136 while the info line could carry a message chip): every instrument name in the strip, every
window position, the cursor in several rows, on two contents: every field filled, and the dearest mix. Note: a note is one tile word whatever it says and an FX is a type glyph and a tile (two words),
but an instrument or a volume is one tile word with a value and **two glyph words (`--`, `??`) without one**, so the
worst grid is a note and both FX filled and the instrument and volume empty.

| Words (DISPLAY included) | translate (default) | VERTEX2F fallback |
|---|---|---|
| pattern view, worst case (OS-009: 909 variants, with the marks, the Shift layer, a block and the dearest note tiles) | **1,281** (was 1,186) | **1,616** (was 1,496) |
| pattern view, typical (the info line has its chip: +6) | 973 | 1,075 (was 1,125: the passes draw column by column) |
| pattern view, cursor on the last step | 1,005 | 1,081 |
| style specimen (the tile row has CUT, FAD, C10 and G10 since OS-009: +10 / +12) | 934 | 1,040 |

OS-002b adds the Song screen and the overlays, every state of them budgeted (limit 1,792) in every theme and both modes; the
numbers are in `sim/README.md` (Budget harness). The dearest is a notification of 48 characters over the pattern's worst
grid: **1,234** words (translate) and **1,541** (fallback); the Navigator over the same grids, with each of its 11 cells
chosen, is 1,199 / 1,452, Learn 1,146 / 1,434, the Song screen's worst sweep 571 / 616. (With a document under them and OS-009's marks, layer,
block and note tiles the four are 1,324 / 1,661, 1,304 / 1,574, 1,251 / 1,576 and unchanged; the choosers, 1,338 / 1,703, are the dearest.)

The same in Amiga, Black and White. The tile faces are what make it fit: the glyph-by-glyph grid needed 1,856 (and passed
only on an interim 1920). `ctest budget` and the `o3sim --gallery` table report it.

## 9. Adding a theme, a layout, a screen

- **A theme** (compiled in): add a `Theme` to `ui/src/themes.cpp` (16 colours, the four faces, the metrics), list it in
  `kThemes`, `ThemeId` and `kThemeCount`. Run `ctest -R contrast`: every pair must pass; then look at the gallery. SD
  theme packs (later) fill the same struct, limited to the declared knobs (palette, optional face and spacing overrides) and
  pass the same check; the built-in themes always exist.
- **A layout** (a screen's rectangles): write a `constexpr` function from the theme's metrics and the faces' geometry to a
  struct of named rectangles and offsets (`app/pattern_layout.h` is the model); have a test check that it `fits` in every
  theme. A screen takes (model, layout, theme); actions never see a rectangle.
- **A screen** (OS-002b; `app/screens/screen.h` says the same in code):
  1. a class derived from `Screen`: `id()`, `render(model, view, renderer, covered)` (skip what lies inside `covered`), `context()`,
     `soft_keys()`, and `enter` / `leave` / `modes()` / `pages()` when it has them, and the `Cursor` interface (`screens/cursor.h`:
     `count`, `position`, `page`, `set_position`, `step`, and `track`, `home`, `end` if it has them) if it has a list or a grid
     to move: the navigation actions and the jump keys then work on it with nothing added to the action files; draw the
     chrome through `app/chrome.h`; its static instance (`extern const X kX;` in the header,
     `constexpr X kX{};` in the source);
  2. a `ScreenId` if it has none, and **one row** in the registry (`app/src/screens/registry.cpp`: id, name, `&kX`, screen
     button, the button's name, icon): `ctest` checks that ids, names and buttons are unique and every row's button has its
     `nav.screen` binding; the Navigator lists it by itself;
  3. its **default bindings** in `app/src/bindings.cpp` (a `Context` for the screen, one per mode; and the screen button's
     `nav.screen`), and the context's name in `kContextNames`, so a keymap can name it;
  4. its states in `sim/src/screens.cpp` (or `screens_app.cpp`, which draws through `app::render_scene`): the budget test and
     the gallery pick it up in every theme; give each state the dearest content as a sweep.
- **A mode:** an enum of the screen's modes (`PatternMode`), one `ModeDef` row each (a lower-case name for the info line and a
  `Context`), `modes()` returning the table, one `Context` value and one `kContextNames` word (`pattern.browse`) each. The mode
  is the second level of the binding chain, so the keys of that mode are `bind(kMode, ...)` lines; what it does not bind falls
  through to the screen's own. `view.mode` and `view.mode_next` choose it.
- **A field** (OS-009): `Screen::field_context(model, view)` names the context of the field under the cursor (Pattern: `pattern.note`,
  `.instr`, `.vol`, `.fx`); it is the level between the mode and the screen, so a mode's row overrides and a field's row specialises
  the screen's default. The gesture layer does not see it change (a held button survives a move from one field to another).
- **A binding:** one `bind(context, trigger, action, ...)` line in the built-in table (a conflict in one context is a compile
  error). A navigation key is an action with an argument (`cursor.jump_fraction 2`) bound to whatever gesture suits: a tap, a
  hold, a double tap, a chord, hold + encoder, Shift + a key. Shift is the one declared modifier: add a Shift binding to the
  list at the top of the table and to `tests/builtin_bindings_test.cpp`, which fails until it is a decision.
- SD override knobs (palette, spacing, a layout's declared parameters) come with the Config work; compiled defaults always ship.

## 10. How to judge a change

1. `o3sim --gallery <dir>` renders every screen x state x theme to `<screen>_<state>_<theme>.png` and the contact sheets
   `contact_sheet.png` (a row per state, a column per theme) and `contact_sheet_<theme>.png`, and prints the word budget.
2. Look at the PNGs, not only the contact sheets (they are quarter size). Ask the north-star question. The checklist: one
   type size everywhere (no); orange anywhere but the cursor (no); a dark-IDE palette instead of Workbench (no); banding
   that fights the selection (none); text below a 9 px cap height that someone must read (none).
3. `ctest`: `contrast`, `budget`, `gallery` (goldens) must pass. (The gallery also shows the Song screen and every overlay.) A golden changes only on purpose
   (`cmake --build <dir> --target update_goldens`), and then the new PNGs are looked at.

## 11. Plots (OS-003d)

`ui/plot` is part of `ui`: a peak-cache mipmap and a wave view, curves, an XY scope, a spectrum analyser and level meters
(ADR 0010 amendment 2). They add **no token**: every colour below is one of the sixteen, and every pair they put side by side
is in the contrast table or a named decoration (`ctest contrast` derives it from the rendered plot gallery, as for the pattern
view). All of them draw on the data pane, as rectangles, strips and points, and each declares its most display-list words, which
holds whatever the data.

| Job | Token | Where |
|---|---|---|
| waveform, curve line and fill, spectrum bars | `Instr` | the plot's data |
| playhead, meter body and peak, scope trace | `Play` | "playing" and "level" are green |
| selection (translucent tint and two edge rules) | `Sel` | wave |
| loop rules and flags, loop tint, LFO shape | `Fx` | wave, curve |
| slice rules, hold tick, spectrum cap, handles, dots | `Text` | bright marks |
| the handle being dragged; a clip (the wave's tinted band, a meter's lamp) | `Accent` | the plot's cursor, and the one warning: the one orange |
| grid, lane and meter frames, graticule | `Empty` | about 2:1, quiet |
| tick labels, rulers, the 0 dB line | `TextDim` | Micro face, 5:1 on the pane |

- **Micro is for tick labels only** (`1k`, `-12`): grid and ruler numbers. A plot's title and its values are Label or Data.
- **Clip is the orange.** The palette has no red and ADR 0001 caps it at sixteen tokens, so by the maintainer's decision the one
  orange also says "clipping": a translucent `Accent` band over the clipped columns of a wave, an `Accent` lamp at the top of a meter.
  (Orange is still never decoration: the cursor, record-arm, the dragged handle and clip.)
- **Crisp 1 px lines.** Grid, frame, rulers, marks and the vector wave are whole pixels (rectangles, and strips with their
  vertices on pixel centres); only curves, the zoomed-in wave line, the scope's trace and handles are antialiased, in a line
  1.5 px wide (scope and wave line 1 px).
- **Two ways to draw a wave, one picture.** Vector (two `EDGE_STRIP_B` fills, 2 words a column) and the L1 bitmap (about 14 words
  whatever the width) draw the same pixels where both apply (`tests/plot_wave_test.cpp`); L4 adds the fraction of the first and
  last pixel of each column. The lane is a flat `Pane` fill; marks go over the wave.
- **A meter is told apart by shape**: the body is the RMS at full width, the peak a narrower bar above it in the same green
  (the palette has no second green that contrasts with `Play`), a two-pixel hold tick, an orange lamp for a clip.
- **Draw order of a lane**: the pane, the wave, then the marks (zero hairline, clip bands, selection, loop, slices, playhead).

Display-list words (DISPLAY not counted; `translate` is the default, `vtx2f` the fallback of section 6). The declared maxima are
**hard**: `tests/plot_bounds_test.cpp` measures each against its worst case, in both modes, with the renderer's origin moved and a
primitive of its own open when the plot starts, and with every colour of the style a different token; where the worst case can be
built the bound is met exactly (a raw section costs at most `Renderer::kRawOverheadWords` = 4 words, 5 when it draws rectangles).

| Piece | Declared maximum | Notes |
|---|---|---|
| wave, bitmap L1 / L4 | 16 (13 on a fresh frame, 14 past 511 columns) | + the marks, at most 123 (32 slices, 8 clip runs, six different tokens: exact) |
| wave, vector envelope | `2 * max_vertices + 12` = 396 | 192 vertices a strip by default; wider lanes group columns |
| wave, zoomed-in line | `2 * max_vertices + dots + 14` | a vertex a frame, a dot on each once they are 6 px apart |
| curve | series `2 * max_vertices + 16` (272 at 128, 400 at 192), handles 50 | work memory 4.5 KB |
| curve axes | 655 | a label is at most 6 characters (longer ones are not drawn) and a glyph can cost 3 words in the fallback mode: about 250 for the usual labels in the default mode |
| spectrum | `4 * bars + 17` (273 at 64 bars) | 2 words a bar and 2 for its cap |
| meters | `7 + 12 * n` | six colour passes however many meters; the scale of marks `3 + 17 * marks` |
| scope | 380 | 208 points, the glow of the newest block, the ring |

The plot gallery (`o3sim --gallery`, screen `plot`) draws every plot in the standard chrome, in every theme, from deterministic
integer-built signals (a drum loop, a chirp, noise, a sine, a Lissajous). Its states in words (translate / vtx2f), the same in
every theme: `wave_l1` 398 / 411, `wave_l4` 338 / 353, `wave_list` 1432 / 1446 (four vector thumbnails and a zoomed line),
`curve_env` 489 / 557, `curve_filter` 419 / 442, `scope` 955 / 975, `spectrum` 627 / 649, `meters` 455 / 499, `page` (wave,
response, spectrum, scope and meters on one screen) 989 / 1024, and `worst` (a sweep of seven variants, each plot at its most) 759 / 782.
All are held to the 1792 (`ctest plot_budget`).

## 12. The boot screen (OS-003f)

The first frame the display shows is the maintainer's logo (a pixel-art floppy disk with the Tracker on it, greyscale with a
checkerboard dither), centred on black, with one Micro-face line under it: the device name and the build id, `Track3r  o3os <id>`.
It is the same in every theme: it is drawn with `ui::boot_theme()` (the Black table: a black pane, `Text` white for the logo and the
name, `TextDim` for the rest), because the device has not read its settings when it appears. It is a registry state (`boot` `logo`)
and is shown in the gallery in all three theme slots as one picture; the line is 76 words.

| What | Value |
|---|---|
| Source | `ui/assets/boot_logo.png`, the maintainer's `logo.png` byte for byte (1254 x 1254 RGB); made by the maintainer for track3r-os, GPL-3.0-or-later like the repository |
| Converter | `tools/logo_convert.py`: PNG decode, crop to the picture, grid detection (none found: the "pixels" of this picture are not a regular grid, 8 px in one place and 11 in another), box filter over the central half of each 7.85 px cell, the smallest of L1 / L2 / L4 whose quantisation error is at most 6 grey levels (L1 55.1, L2 19.7, **L4 4.5**) |
| Bitmap | 124 x 124 L4, 7,688 bytes of flash, uncompressed; `src/boot_logo_data.cpp` is committed, the ctest `logo_convert_fresh` regenerates it and fails when it differs |
| Shown | x3 through `BITMAP_TRANSFORM_A` and `_E` = 85 (8.8 fixed point, nearest): 372 x 372 at (214, 54), exactly centred; the line at y = 440 |
| RAM_G | `kRamGBootLogo` 0x6F000, two pages, handle 11 (`ui/plot/ram_g_map.h`) |
| Stay | 800 ms from the first light, then the app's first screen replaces it, and not a millisecond longer than the boot work (`core/boot_hold.h`) |

The scale by 3 is the nearest the 8.8 coefficient gets to a third (85.33): one block of the 124 is 4 px wide and the last is 2 px;
scales by a power of two would be exact but would not match the 7.9 px dither of the picture (4 x gives 100 cells of 9.7 px and the
checkerboard averages away). To change the picture: replace the PNG, run the converter with `--out`, update the constants in
`ui/boot_logo.h` (the generated file's static_asserts say which), regenerate the goldens and look at the gallery.
