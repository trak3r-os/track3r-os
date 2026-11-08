# PAT-NAV: OS-ZOOM (Note+Instr zoom, Note+Left/Right track moves) and OS-QJUMP (Shift+Note/Instr/Vol/Fx quarter jumps)

BACKLOG PAT-NAV (queued 2026-10-16, level B, size M). Written 2026-10-16 from the current tree. **New Community OS behaviour; OG parity of these keys is an open research question, a separate follow-up (maintainer Q3 a). No persistent format changes.**

## Goal
Make Pattern navigable on a 12-track project without leaving the grid. Note acts as a held modifier. Note+Instr cycles three zoom levels. Note+Left/Right moves one track. Shift+Note/Instr/Vol/Fx jumps to 0/4, 1/4, 2/4, 3/4 of the pattern. One branch, one commit per ID, one review of the whole Note/Instr/Vol/Fx key map. Land it after PAT-UX (see Conflicts).

## Decisions
| Topic | Decision |
|---|---|
| Levels (maintainer Q1 a) | 5 tracks (all five fields, Data face 10x20), 8 tracks (Note, Instr, Vol; small text, Label face 8x16), 12 tracks (Note only, Micro face 6x12). Cycle 5, 8, 12, 5. No separate text-size control. |
| Gesture (maintainer Q2 a) | Note is a context-scoped held modifier, not a 40 ms Chord. Note has no Tap or Hold of its own once a second button joins it. |
| Where bound | `kPattern` and `kStepPopupCtx`, with the same Note-modified taps in both. A Note tap with no second key behaves as today. |
| Track move | Note+Left/Right calls the existing `cursor.track` (+/-1) and keeps the column. |
| Left/Right | In Pattern they step over visible fields only. Hidden fields stay editable through the hold popups. |
| QJUMP | `cursor.jump_fraction` 0..3 on Shift+Note, Shift+Instr, Shift+Vol, Shift+Fx, bound in `kPattern` (OS-SELSHIFT: now `select.jump`, the same jump plus, in Edit, the selection of the rows crossed). Shift+Insert stays. The Shift+Fx FILL binding is dropped and F4 keeps Fill. |
| Persistence | The zoom level is RAM-only (orchestrator default). It resets to 5 at boot and survives project loads. No project, settings or SD format is touched. |
| Free keys | Note+Vol and Note+Fx stay unbound. |

## Gesture design
The recognizer's modifier set is a constructor constant (`kModifierButtons`, Shift only). Add a data-driven, context-scoped path in `core/gesture.h`, `core/src/gesture.cpp`, `core/src/binding.cpp`. This is the same shape as the hold-encoder wait.
- New want bit `kWantModifier`. `gesture_wants()` sets it for a button when a reachable Tap, Hold or DoubleTap binding in the current chain names that button as its `modifier`. Chord and HoldEncoder triggers already reuse the field for their own meaning, so they do not count. Shift is excluded.
- A button with that bit is a held modifier, like Shift, while it is down or after its Hold fired. `held_modifier()` accepts Shift or any entry with the bit. Add the bit to `kWaits`, so the button's own Tap waits for the release, as the hold-encoder buttons do. The context scoping comes from the bindings: only Pattern and the Note StepPopup name Note as a modifier, so nowhere else changes.
- Consume rule (orchestrator default): a press of another button (not Shift) while a `kWantModifier` entry is in State::Down turns that entry into Decided without emitting. This runs before `settle_waiting`, which would otherwise emit Note's Tap. Its release is then silent and its 400 ms Hold never fires.
- A pad hit or an encoder turn under Note keeps today's behaviour (Note's Tap first, then the pad or turn).
- A Note tap with no second key still gives Tap at the release (Note column), or Hold at 400 ms (the popup), unchanged.
- After the popup is open (entry Held), the same Note-modified taps resolve in `kStepPopupCtx`. Release is still HoldEnd.
- Binding resolution already retries with no modifier. So in the popup, Up, Down and the encoder under a held Note still reach their plain rows. Note+Vol and Note+Fx behave as plain Vol and Fx taps.
- Arrow auto-repeat already follows the modifier, so a held Note+Right repeats track moves and stops when Note is let go.
- Shift pressed after Note does not consume Note. Shift-first Shift+Note is the supported order.
- Keymap parser: `kNames` in `app/src/bindings.cpp` passes `kModifierButtons` as the allowed modifier mask. Check that the user-keymap text can name Note as a modifier for these rows, or confirm the built-in table is enough and the parser stays unchanged.

## Behaviour
1. Note+Instr: level 5 to 8 to 12 to 5. A note under the cursor never changes.
2. On every level change, clamp the view: left = clamp(left, 0, max(0, tracks - N)), then scroll so the cursor track is inside the window.
3. A project with fewer than N tracks shows what exists. The track width stays fixed, so the right side is empty pane. No stretching.
4. Hidden-column rule: if the new level hides the cursor's column, snap to the nearest visible column on the left (Fx2 to Vol at level 8, anything to Note at level 12). Tapping Instr, Vol or Fx on a hidden column is refused with an info-line hint naming the zoom key (orchestrator default). Hold popups still open and enter into the cursor step.
5. Left/Right (`move_left`/`move_right` in `pattern_view.cpp`) skip hidden fields and cross to the neighbouring track at the visible edge. `home` goes to the first track's Note, and `end` to the last track's last visible field.
6. QJUMP: Shift+Note goes to step 0, Shift+Instr to n/4, Shift+Vol to 2/4, Shift+Fx to 3/4. A 64-step pattern lands on 0, 16, 32, 48. The block-collapse behaviour is whatever `cursor.jump_fraction` does for Shift+Insert today.
7. Drop the `kPattern` Shift+Fx `kPatternFill` row (`bindings.cpp` near line 379). Keep the `pattern.fill` action registered and unbound, to avoid churn in the action tests and keymap files. Fix the comments that mention the repeat-fill.

## Rendering per level
- Layout: `make_pattern_layout(theme, rows, tracks)` in `app/include/o3os/app/pattern_layout.h` derives `track_w = (800 - index_w) / tracks`. With index_w 30 that is 154 at 5, 96 at 8 and 64 at 12 (not the 66 in the backlog; orchestrator default is to keep index_w). Add a zoom parameter (face, field mask, cell widths). Level 5 must produce the identical layout to today.
- Level 8 fits: Note 24 + Instr 16 + Vol 16 px plus two gaps is 64 px of 96. A Fx1 field would make 92 px of 96, which is too tight, so it is out unless the maintainer says otherwise.
- `PatternView::kVisibleTracks` (a constant used in `pattern_view.cpp` follow/draw, in `app.cpp` near line 1649 and in `pattern_bars_[5][2]` in `app.h`) becomes `kMaxVisibleTracks = 12` plus a per-view `visible_tracks()`. The other screens (Mixer, Song, File, Config and the rest) keep 5.
- `PatternView::draw` needs the face, field mask and track count from the level. Headers, MUTE/SOLO tags, selection block and Rec frame already derive from `l.track_x` and `l.track_w`. Header name and badge text need a per-level character limit.
- Tiles: the hex-pair and note tiles are composed at boot from the Data face (10x20 only: `ui/assets.h`, `compose_tile`). Small text needs 8x16 and 6x12 tile sets. Handles 5, 6, 7 and 12 are taken, and only a few of 9..14 are free. So compose the two extra sets at boot into new RAM_G regions (about 14.6 KB for 8x16, about 10.9 KB for 6x12). `Renderer::begin()` rebinds the same four handles to the level's regions: about 16 extra words, only on a small level, so level-5 frames are unchanged. Check `ui/plot/include/o3os/ui/plot/ram_g_map.h` still clears the wave slots.
- Display list: a note tile is still one word, so the 12-track frame is at most 12 x 16 note tiles. The `budget` tests pin the worst case at each level.

## Edge cases
- Playback follow: the row follows the playhead as today. The horizontal window does not follow playback. The play bar and cursor row bar stay full width.
- Selection and block ops (Shift+arrows, copy, paste, fill) work in field and track units and ignore zoom. A block can extend into hidden fields, and the block rectangle still covers whole tracks.
- Record mode: live-record and step-record write at the cursor. At level 12, pad entry targets the Note column, which is always visible. The Rec frame is drawn at every level.
- Note+Instr with a Note StepPopup open changes the grid underneath it. The popup stays open.
- A zoom key pressed in Fill, chooser or other capturing overlays does nothing: those contexts do not bind it.

## Done when
- Host tests (`tests/gesture_test.cpp`, `tests/binding_test.cpp` or equivalent). Cover: kWantModifier from bindings; a Note tap alone gives Tap and a Note held 400 ms gives Hold; Note then Instr gives one Tap(Instr, mod Note) with no Note Tap or Hold; the same after Hold has fired; repeat of Note+Right; Shift-after-Note leaves Note intact; reset(keep_held) with Note held.
- Further host tests: layout `fits` and field positions for the three levels in all three themes; window clamp on level change; hidden-column snap; Left/Right skip; Note+Left/Right at the first and last track; QJUMP lands on 0/16/32/48 and the Shift+Fx fill row is gone; a project with 3 tracks at levels 8 and 12.
- Sim scenarios: add `sim/scenarios/pattern_zoom.txt` (three levels, themes, 12-track and 3-track projects, Note popup with the hint line, Note+Right track move) and extend `pattern_keys.txt` for QJUMP. Level-5 goldens must not move. Updated: the Note popup (hint line) and gallery shards that show it. New: the zoom snaps. Regenerate goldens with `update_goldens` and look at the PNGs before committing.
- Docs: `docs/ui/ui-map.md` (Pattern row, StepPopup hint, Shift row), `docs/qa/catalogue/pattern.md` (QA-PAT-090 removed or rewritten, new QA rows), this spec, BACKLOG and STATUS.
- `fw-build` is clean with no new warnings.

## Hardware-only checks
- A fast roll (Note down, Instr down, then both released) feels right and does not drop single taps.
- The 12-track text is legible on the panel at arm's length.
- Boot time and RAM_G upload with the extra tile sets.
- Auto-repeat of Note+Right.
- Learn chord (Vol+Fx) and Shift+Vol/Fx still behave.

## Budget
- Flash: roughly 1 to 2 KB of code. The tiles are composed at boot, so no flash for them.
- RAM_G: about 25.5 KB added (about 64 KB used in total).
- SRAM: tens of bytes (zoom level, per-level bars array grows to 12 x 2).
- Measure with `fw-build`.

## Conflicts
PAT-UX is editing `app/src/bindings.cpp` (Pattern rows around lines 345-380, the Delete rows), `pattern_view.cpp` (row wrap in `follow` and the move functions, REC signs in `draw`), the pattern layout and the sim goldens. Likely conflicts: the same binding block, `move_*`/`follow`, and `sim/goldens.txt` hash lines. Start PAT-NAV from `dev` after PAT-UX merges (no rebase), and regenerate goldens instead of hand-merging hashes.

## Maintainer answers (2026-10-16)
| Question | Answer |
|---|---|
| Q1 Level 8 fields | (a) Note, Instr, Vol. |
| Q2 Note consumed by any other button | (a) Yes: any other button cancels Note's Tap and Hold; Note+Vol and Note+Fx act as plain Vol and Fx. |
| Q3 OG parity first? | (a) Ship now; checking whether the OG uses these keys is a separate follow-up. |

## Files
- `core/gesture.h`, `core/src/gesture.cpp`, `core/src/binding.cpp`
- `app/src/bindings.cpp`, `app/src/pattern_view.cpp`, `app/include/o3os/app/pattern_layout.h`
- `ui/assets.h` (tile composition), `ui/plot/include/o3os/ui/plot/ram_g_map.h`

## Implementation notes (OS-ZOOM and OS-QJUMP, 2026-10-16)
What was built, and where it differs from the text above. Evidence: `tests/pattern_zoom_test.cpp`, `gesture_test.cpp`, `gesture_repeat_test.cpp`, `binding_resolver_test.cpp`, `builtin_bindings_test.cpp`, the `budget` test and the scenarios `pattern_zoom`, `pattern_keys`, `qa/qa_pattern` (QA-PAT-236..245, QA-PAT-090 rewritten).
- **Gesture layer**: as designed. `kWantModifier` (`core/gesture.h`), set by `gesture_wants()` for the button a Tap, Hold or DoubleTap binding names as `modifier`; the recogniser masks it off for the constructor's modifiers (Shift). A declared modifier (Shift) wins over a bound one when both are down. `reset(keep_held)` keeps a bound modifier that is still down, as it keeps Shift.
- **Keymap files**: the loader's modifier set is now Shift and Note (`kKeymapModifiers`, `app/src/bindings.cpp`), so `pattern tap note+instr none` switches the built-in row off and a user may add `note+...` rows; this was needed for the round-trip test of the built-in table (`binding_line` prints `note+instr`), and it is coherent because the gesture layer derives the modifier from whatever the bindings name. Note is the only extra name.
- **Tiles**: the smaller sets hold 128 cells each, not the whole range: Label hex pairs `00..7F` (the volume's range is 0..127 and the instrument's BCD is at most `0x64`, so one handle serves), Label note tiles `0..127`, Micro note tiles `0..127` (Note is the only field at 12 tracks, so no Micro hex pairs). A value or cell outside them (a hex pair from `0x80`, a note from C10) is drawn as glyphs of the face (2 or 3 words). Handles: 13 (hex) and 14 (notes, shared by the Label and Micro sets, one bound at a time), bound in the first frame that draws from them (4 words a set); no change to `Renderer::begin()` and no change to a level-5 frame. RAM_G: 14,848 bytes (4,096 + 6,144 + 4,608) instead of the 25.5 KB estimated; the wave slots, the boot logo and the spare region move up by 16 KiB (`ram_g_map.h`: `kRamGWave` 0xF000, `kRamGShotBytes` 585,664).
- **Layout**: `make_pattern_layout(theme, rows, tracks, zoom = 0)`; `PatternLayout` gains `zoom`, `fields`, `face` and `index_dy` (the row numbers stay Data tiles and keep their own vertical offset). The cursor block is the Data face's height at every level, centred on the capitals of the level's face. Level 0 is bit-identical to the old layout (the level-5 goldens did not move).
- **View**: `PatternView::kVisibleTracks` stays 5 (the default level, which the other screens use) and `kMaxVisibleTracks = 12` is new, with `visible_tracks()`, `visible_fields()`, `zoom()`, `cycle_zoom()`, `set_zoom()`, `column_visible()` and `snap_column()`. A hold popup of a hidden field still opens (its release, or Back, puts the cursor back on the nearest visible column on its left). Headers: at 8 and 12 tracks two mark slots (M, S) instead of three, and the name gets what is left (none at 12 tracks: the number only).
- **Keys**: `pattern.zoom` (new action, no focus needed, so it also works in the step popup), `cursor.track` for Note + Left / Right (`StepPopupOverlay::track` forwards to the grid underneath), `cursor.jump_fraction` 0..3 for Shift + Note / Instr / Vol / Fx. `pattern.fill` is registered with no key; its callers in the tests use the action by name. The hidden-column hint is `FIELD HIDDEN: NOTE + INSTR ZOOMS`. The Note popup also says `NOTE + INSTR: ZOOM 5 / 8 / 12 TRACKS` and `NOTE + LEFT / RIGHT: MOVE A TRACK` (two lines above its footer).
- **Stack**: the draw's per-track arrays are sized for 12 tracks (about +1.4 KB of stack while the Pattern frame is built); the display list is unchanged at level 5.
- **Display list (worst case of the budget sweep, translate / VERTEX2F)**: 5 tracks 1,267 / 1,599; 8 tracks 1,403 / 1,605; 12 tracks 1,158 / 1,417 words, of the 1,792 allowed (`pattern` states `worst`, `zoom8`, `zoom12`).
- **Simulator**: the scenario command `document tracks <n>` gives the project n active tracks (a three-track project for the zoom levels).
