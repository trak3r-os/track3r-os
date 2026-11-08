# OS-HELP: Shift + Config opens a help page for the current screen

Status: spec, 2026-10-19. APPROVED by the maintainer 2026-10-19. **Level B**: new UI only; no persistent format, no audio or ISR path, no file on the card. This is **new Community OS behaviour**. Whether the official firmware binds Shift + Config is not checked and does not matter here: the gesture is new in this OS.

The Q rows were decided with the maintainer in the chat of 2026-10-19 (Q1, Q3, Q4, Q5) or by the spec author with the simplest option (Q2, Q6, Q7).

## Intent

`Shift + Config` opens a full-screen help overlay on the page of the screen you are on. Each page says what the screen is for, what each key does on it (with a short explanation, not just the action's name) and what the pads do. `Left` / `Right` change the page, `Up` / `Down` and the encoder scroll, any other key closes it. Pressing `Shift + Config` again later on the same screen comes back to where you left that page.

## What exists

- **The gesture is free.** No context binds `Shift + Config` today (`app/src/bindings.cpp`). An unbound Shift tap falls back to the plain binding (`core/src/binding.cpp` `resolve()`), so today `Shift + Config` acts as `Config`. A binding that names the modifier always wins over one that does not (`core/include/o3os/core/binding.h`), so one `kGlobal` binding is reached from every screen, Config included. Plain `Config` (open Config, next page) is unchanged.
- **Modal overlays capture** (`app/include/o3os/app/screens/overlay.h`): a modal overlay's context is first in the chain and swallows what it does not bind. So `Shift + Config` does nothing while a dialog, a chooser, the Navigator, Learn or the on-screen keyboard is up; that is the wanted behaviour (Q6).
- **Overlay frame:** `app/src/screens/dialog.h` (`make_dialog`), as used by `tools_overlay.cpp` and `play_mode_overlay.cpp`. OverlayId list: `app/include/o3os/app/screens/ids.h`. Per-screen state lives in `ViewState` (`app/include/o3os/app/view_state.h`), indexed by `screen_index()` (`kScreenCount` = 14).
- **Wildcards:** `kAnyId` matches every input of a kind (`core/include/o3os/core/input_event.h`), but a wildcard and a specific binding in one layer and context are a reported conflict (`binding.h`). So "any other key closes" cannot be a button wildcard next to the arrows in the same context; see behaviour item 4.
- **Text sources for the pages:** the manual chapters (`docs/manual/*.md`, written for owners of the official firmware), the cheat sheet (`docs/manual/cheat_sheet.md`) and the QA catalogue (`docs/qa/catalogue/*.md`). The bindings themselves are in `app/src/bindings.cpp`.

## Behaviour

1. **Open.** `Shift + Config` (a tap of Config while Shift is held), global, opens the help overlay (a new `OverlayId::Help`, modal, its own context `Context::Help`) on the page of the base screen. Not reached while another modal overlay is up (Q6).
2. **Pages.** One page for the global keys, then one page per screen, in this order: Global, Pattern, Song, Instrument, Wave, Sample edit, Sample load, Record, Mixer, Perform, File, Config, Modules, Module app (a generic page: the app's keys belong to the module; the page explains the exit chord and the shared keys). InputTest has no page (it is reached only by a boot chord and never opens help). A screen with several pages of its own (Instrument, Config, Perform...) has one help page with a section for each.
3. **Keys in help.**
   - `Left` / `Right`: the previous / next help page, with no wrap at the ends (Q2). The new page shows at the scroll where it was last left.
   - `Up` / `Down`: one line. They auto-repeat as everywhere (`kRepeatButtons`).
   - The **encoder**: scrolls like `Up` / `Down`, one line a detent (maintainer, 2026-10-19).
   - `Shift + Up` / `Shift + Down` and `Shift` + the encoder: one screenful (the OS-SHIFTENC convention).
   - The scroll stops at the top and when the last line is at the bottom; a page shorter than the body does not scroll.
4. **Any other key closes** (Q3, Q4). Any other button press, `Play` and every screen button included, any pad press and `Shift + Config` itself closes help and does nothing else (the press is swallowed). `Shift` alone neither closes nor scrolls; its release after the opening gesture does nothing. Releases are never swallowed in a way that leaves a note or a hold hanging: a pad or key held when help opened releases normally, as under the other modal overlays. Mechanism (an implementer's choice, recorded in the commit): preferred, a flag on the overlay that closes it when a press resolves as `Captured` in its context; or a parent context holding the button and pad wildcards.
5. **Where it reopens** (Q5). RAM only, never saved: one scroll offset per page (15 x `uint16_t`, 30 B in `ViewState`). Each page keeps its own position: `Shift + Config` opens the base screen's page at that page's last scroll, and `Left` / `Right` show each page at its own last scroll. After a power cycle every page starts at the top.
6. **Layout.** A full-body dialog: title `HELP: <SCREEN>` on the left, `n/15` on the right; the text in the Label face, one string per line, ASCII; a thin scroll bar on the right when the page is longer than the body. The F-key row is the dialog's (OS-KEYROWDLG): every label empty except F8 `CLOSE` (every key closes anyway). Colours are existing theme pairs only; it must read in the three built-in themes.
7. **Page content** (Q1). Each page has three parts:
   - **What the screen is for:** 2-4 lines on its purpose and the idea behind it (the order list, the track map, slots...), for an owner of the official firmware, as the manual is.
   - **KEYS:** every binding of the screen's contexts (and of its sub-contexts: the step columns on Pattern, the PAT and map columns on Song...), the key on the left, a short explanation on the right, wrapped onto further lines when needed. Shift combinations included.
   - **PADS:** what the pads do on that screen, in each pad mode it has.
   The Global page lists the `kGlobal` bindings, the screen buttons and the boot chords. The text is condensed from the manual and the catalogue and checked against `bindings.cpp`; where those disagree, the binding is the truth and the disagreement is reported in the commit.

## Decisions

| # | Question | Decided |
|---|---|---|
| Q1 | Bindings only, or explanations too | explanations too: purpose, KEYS with an explanation each, PADS (maintainer, 2026-10-19) |
| Q2 | Wrap at the first / last page | no wrap, the simplest and the safest to read (spec author) |
| Q3 | What closes help | any key or pad other than the arrows and the encoder (maintainer, 2026-10-19: "any other key goes out") |
| Q4 | Does the closing key also act | no, it only closes; `Play` too (spec author, from the maintainer's words: the earlier suggestion to let `Play` through is dropped as more complex) |
| Q5 | Where it reopens | the current screen's page at its last scroll, a scroll kept per page (maintainer, 2026-10-19); RAM only, not saved (maintainer, 2026-10-19: "no need to save that") |
| Q6 | Over dialogs and overlays | not reachable there: modal overlays capture (spec author) |
| Q7 | Where the text lives | in flash, one C++ table in `app/src/help_text.cpp`; on the card it would be missing with no card or an old card. Compression only if flash gets tight (spec author) |

## Budget

- Flash: viewer about 3 KB [est]; text about 2-3 KB a page, **about 30-40 KB** for 15 pages [est]. Hard cap 48 KB for the whole item; past it, stop and report (a word-dictionary compression, about 1 KB of decoder, roughly halves the text). FLASH was 823,268 B at int.11. Record the delta in the commit.
- SRAM: about 32 B (`ViewState`). No SDRAM. CPU: none in the audio path; the overlay draws only lines inside the body.

## Slices (one branch `feat/OS-HELP`, two commits)

- **H1 viewer** (S): `OverlayId::Help`, `Context::Help`, the actions `help.open` / `help.close` / page / scroll, the global binding, the close-on-any-other-press mechanism, the `ViewState` fields, the layout of item 6, `help_text.cpp` with the table type and the Global and Song pages filled. Host test: open on a screen lands on its page; Left/Right stop at the ends; Up/Down/encoder scroll and clamp; Shift + Up/Down page; reopen restores the scroll; any other button, a pad and Play close it and do nothing else; a held pad's release still ends its note; `Shift + Config` under the Navigator does nothing. Sim scenario `sim/scenarios/qa/qa_help.txt` with goldens.
- **H2 content** (M): every page of item 2, written to item 7. A host test that every line fits the body width in each built-in theme (`text_width` of the Label face) and is ASCII. QA rows in `docs/qa/catalogue/global.md` (from QA-GLB-148: H1 took 141-147 for the viewer); the cheat sheet's Global keys and the manual's global-keys chapter gain `Shift + Config`.

## Acceptance

- `Shift + Config` opens help from every screen of item 2, Config included; plain `Config` is unchanged.
- Arrows, encoder and Shift paging behave as item 3; everything else closes as item 4.
- Reopening on the same screen returns to the last scroll of that page; nothing is written to the card.
- Every page has the three parts of item 7; a reviewer spot-checks at least three pages against `bindings.cpp`.
- Host tests and the sim scenario pass; one full `tools/ci.sh host`; the flash delta is recorded and under the cap.
- One `firmware-reviewer` pass (Level B).
- **Still needed on the unit:** open and read on the device (legibility at arm's length, scroll feel with the real encoder, no hanging note after closing with a pad held).

## Risks

- **Text drifts from the bindings** as the keys change. v1 is hand-maintained; a later row can generate the KEYS lists and add a drift test (every binding of a context appears on its page).
- **Flash:** the largest single cost of the item; capped above.
- **Long lines** overflow in one theme: the H2 fit test covers it.

## State for implementer

**OS-FLASHDIET A7 (branch `feat/OS-FLASHDIET-A7`):** the authoring file of the text is `app/help/pages.txt` (packed at build time by `tools/help_pack.py`; the old C++ table in `app/src/help_text.cpp` is gone and `help_page()` is served from the blob, with `tests/data/help_old_pages.txt` as the golden of the old text in `tests/help_blob_test.cpp`).

H1 is on branch `feat/OS-HELP` (the viewer, the Global and Song pages, tests `tests/help_test.cpp`, scenario `qa_help`, QA-GLB-141..147). Implementer's choices: the list of item 2 has **14** pages (Global plus 13 screens), not 15, so `kHelpPages` = 14 and the scroll array is 28 B; closing is the existing `ui.back` (the overlay's `closes_on_press` flag, `App::press_closes_overlay`), the scroll and the pages are `nav.*` / `cursor.page` through the overlay's Cursor, so the one new action is `help.open`; `tools/check_keytext.py` skips `app/src/help_text.cpp` (it names keys by design); the app page binds Shift + Config itself because it captures. Lines are wrapped by hand to `kHelpKeyCols` / `kHelpTextCols` (help_text.h), 22 + 70 characters.
**H1 and H2 are done on the branch** (H1: the viewer and the Global and Song pages; H2: the other twelve pages, the fit and ASCII test, QA-GLB-148 on); the review fold (Mixer pads, File and IMPORT keys, the app-input rule for module apps, the release of the pad that closes help) is in the last commit. What is left is the unit check of the acceptance list and, as a later row, the drift test of the Risks section. (H1 brief:) Read `overlay.h`, `dialog.h`, `tools_overlay.cpp` (an overlay with a list), the Navigator's bindings in `bindings.cpp` (a modal context), `core/include/o3os/core/binding.h` (fallback, wildcards, conflicts), `view_state.h`. Text sources for H2: `docs/manual/<screen>.md`, `docs/manual/cheat_sheet.md`, `docs/qa/catalogue/<screen>.md`.

**OS-SMALLFIX11 (2026-10-20, branch `fix/SMALLFIX11`), the follow-up from int.15:** (1) the page is opaque. The overlay now declares the full width of the pane as what it covers and paints it Chrome before the dialog, so a base screen leaves out everything wholly inside it (the unit's fills do not hide what lies under them: OS-SIMKEY, as OS-BUG-OGOVERLAY); the Mixer's level cursor, the Record screen's empty state and the MODULES list and a module app's parameter column were drawn without the guard and now have it. `help_test` reads the display list of every base screen under the cover. (2) A second PERFORM page, `HELP: PERFORM DJ` (page 11 of 15), written in the three parts; Shift + Config on the DJ page opens it, and `help_page_of(screen, dj_page)` is the lookup, so File, Config, Modules and the Module app are one page later (12 to 15).
