# OS-LEARNMAP — one screen for every MIDI Learn binding

Status: spec, `feat/OS-LEARNMAP` · Level B · no format change (an edited binding keeps the same `LearnBinding` fields; the encoder target stays session only) · Evidence: **new Community OS behaviour**; there is no official-firmware counterpart. Backlog row: OS-LEARNMAP (maintainer, 2026-11-10).

## 1. Goal

The Learn dialog shows five rows and edits a binding only by delete and learn again. LEARN MAP lists all bindings of both stores plus the encoder's target on one full screen, and edits each column in place: pick the column with Left/Right, set it with the encoder.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | A binding is 8 B: `param`, `cc`, `channel` (0 = all), `mode` (Pickup, Jump, Relative), `flags` (bit 0 = follow). Two stores of 32 slots: Project (saved in the project `LRNB` chunk) and Device (track, master and device params). | `core/include/o3os/core/learn.h` (`LearnBinding`, `kLearnSlots`, `learn_scope`) | code fact |
| F2 | One binding per param and per message inside a store (`learn_plan`, `learn_overlap`); a duplicate CC across the two stores is allowed. | `core/include/o3os/core/learn.h` | code fact |
| F3 | Project edits go through `ctx.edit->set_learn_slot` (one undo record each); the Device store is not on the history. The Learn actions are `kActionUndoableIfOpen` and listed in `kAutoArea`. | `app/src/param_actions.cpp` (`store_set`), `app/src/actions.cpp:216-220, 411` | code fact |
| F4 | `learn_entry_at(model, i, out)` lists project slots first, then device slots, empty ones skipped. | `app/src/param_actions.cpp:985` | code fact |
| F5 | A follow binding is `param_id(Instrument, 0, field)` plus the flag; `apply_cc` resolves it to `ViewState::instrument`; `follow_fits` keeps Sample fields and MIDI fields apart and never follows a module field. | `app/src/param_actions.cpp:1161, 1250` | code fact |
| F6 | The encoder target is `ViewState::enc_target`, set by `enc_assign`, with no clear gesture today. | `app/include/o3os/app/view_state.h:189`, `param_actions.cpp:1305` | code fact |
| F7 | Learn-able means `describe_param(...).flags & kParamLearnable`. | `model/include/o3os/model/params.h` | code fact |
| F8 | `midi_cc` hands a CC to `learn_bind` while the Learn overlay is open. | `param_actions.cpp:1358` | code fact |
| F9 | The Events overlay is a full cover (`Color::Pane` from the strip to the info line, 15 rows, key row stays): the layout template. | `app/src/screens/events_overlay.cpp` | code fact |
| F10 | The gesture layer takes two-button chords only; `Shift + Vol + Fx` cannot be bound without a core gesture change. | `core/` gesture recogniser, `app/src/bindings.cpp:463` | code fact |
| F11 | Config's INFO page swaps F8 to UPDATE per page (`config_key_row`, `kFirmwareOpen`); F8 on the MIDI page is free. | `app/src/screens/config_screen.cpp`, `app/src/bindings.cpp:866` | code fact |
| F12 | Display-list cap 2048 words; every screen state is budgeted at 1,792 (`kBudgetWords`); the dearest cited gallery state is input test at 999 words. | `docs/architecture/os-tree.md:105, 935`, `sim/src/screens_app.cpp` | code fact |

## 3. Local decisions

| # | Decision | Reason |
|---|---|---|
| D1 | A new modal overlay `OverlayId::LearnMap` with the Events cover. Row 0: title and position (`LEARN MAP  n/N`); row 1: captions `SOURCE CH PARAMETER INS MODE VALUE` and a dot column; then 15 list rows. | Reuses a proven layout and the overlay plumbing; no new base screen. |
| D2 | Rows: ENC first, then project bindings, then device bindings (`learn_entry_at` order). At most 1 + 32 + 32 = 65 rows; the window of 15 scrolls to keep the cursor row in view; no paging keys beyond Up/Down. | A cap above the real maximum, so no binding is hidden; the same order the dialog uses. |
| D3 | Opening: `F6 MAP` in the Learn dialog, and a `LEARN MAP` key on Config > MIDI (`F8`, per-page key row, as INFO's F8 UPDATE). The brief's `Shift + Vol + Fx` is not built: F10 makes it a Level C gesture change, and the brief names the Config > MIDI row as the fallback. | Smallest change that opens the screen from a place the user finds. |
| D4 | Columns: Left/Right pick SOURCE, CH, PARAMETER, INS, MODE (VALUE is read-only); the encoder sets one detent per step, `Shift` coarse (SOURCE and CH 8 steps; others the same as one). | The brief's editing model; one rule for all columns. |
| D5 | PARAMETER walks only the Learn-able params of the row's kind (Sample, MIDI or Module fields for an instrument row; its own kind for track, master, device and insert rows), clamps (no wrap), and skips params another binding of the store already holds. | One binding per param (F2) makes a skipped step the only conflict-free move. |
| D6 | INS column: `*` (FOLLOW) first, then 1..64; instrument rows only. A change to a slot whose kind lacks the param is refused `INS nn HAS NO <PARAM>`; a module param refuses `NOT FOR A MODULE PARAMETER`; a non-instrument row refuses `INS: INSTRUMENT ROWS ONLY`. | Same follow rules as `learn_follow` (F5). Shift (coarse) on INS skips the instruments whose kind lacks the param instead of refusing; a plain step onto one is refused. |
| D7 | Refusals (strip text): `CC n CHc IS TAKEN` (c = 1..16, `*` for all), `LEARN STORE FULL`, `NO PROJECT`, `ENCODER HAS NO CC`, `ENCODER IS ALWAYS RELATIVE`, `NO ENCODER TARGET`, `NOTHING LEFT TO BIND`, `ALREADY BOUND` (the parameter has a binding in that store already; one parameter, one source). A refused edit changes nothing and keeps the cursor. F5 FOLLOW also refuses `INS nn HAS NO <PARAM>` when the selected instrument lacks the field. Implementation: `commit` is the one refusal point for an existing row; NEW adds a slot, so it calls the same overlap check itself. | Texts in the Learn dialog's style, caps. |
| D8 | VALUE is the live `param_format` of the bound param's current value; a follow row resolves to the selected instrument; no project, a missing instrument or a non-fitting field shows `--`. Re-read on every draw: no cache. | Rendering has no clock; reading is cheap and exact. |
| D9 | Activity dot: `apply_cc` records a hit (a bit in a 2 x u32 mask in `view.learn_map`, plus the tick) only while the map is open; the dot shows for 250 ms and the main loop clears the mask from its tick and asks for a redraw. | 250 ms is visible and short; the hook costs nothing when the map is closed. |
| D10 | F1 NEW arms `MOVE A KNOB`; the next CC creates a binding (first free Learn-able param of the selected instrument's kind, INS = selected instrument, PICKUP) and the cursor goes to it on PARAMETER. F7 RELEARN arms the same; the next CC replaces the row's source and channel. Any navigation key or action disarms. While armed the CC is consumed; otherwise a CC applies as usual. | The brief; consuming only while armed keeps the screen a live monitor. |
| D11 | F2 MODE cycles with wrap; F3 DELETE removes the row (on ENC clears the target, `ENCODER TARGET CLEARED`); F4 CLEAR ALL needs a second press (both stores, ENC target kept); F5 FOLLOW toggles; F8 BACK closes. F2, F5 and F7 on the ENC row are refused. The bodies of follow and clear-all are shared with the Learn dialog, not copied. | One rule per behaviour. |
| D12 | Undo: as the dialog. Each action is one undo unit (an encoder event with N detents is one action); project edits go through `set_learn_slot`; device edits are not on the history. No click-group merge. | The brief: behave as the dialog. |
| D13 | The ENC row edits `enc_target` only: PARAMETER (an empty target starts at the first Learn-able param of the selected instrument) and INS (the instrument the target follows). Not saved. | F6. |

## 4. Acceptance

- The screen lists ENC and every binding; the cursor row, column and the window follow Up/Down/Left/Right.
- Each column edits as D4 to D6; each refusal of D7 shows its text and changes nothing.
- F1/F7 take the next CC; F2..F5 and F8 behave as D11; the dot lights on a moving CC and clears.
- An edited project binding saves and loads with the same bytes layout (`LRNB` unchanged).
- Undo of a project edit restores the previous binding.
- Image ends at or below `0xFF000`.

## 5. Files

`app/include/o3os/app/view_state.h`, `app/include/o3os/app/screens/ids.h` and `overlay.h`, `app/src/screens/overlays.cpp`, `app/src/screens/learn_map_overlay.cpp` (new), `app/src/learn_map_actions.cpp` (new), `app/src/param_actions.cpp` (shared helpers, CC capture and hit), `app/src/actions.cpp`, `app/include/o3os/app/action_handlers.h`, `app/src/bindings.cpp`, `app/src/key_prompt.cpp`, `app/src/state_line.cpp`, `app/src/screens/config_screen.cpp`, `sim/src/screens_app.cpp`, `sim/scenarios/` (new scenario), `tests/learn_map_test.cpp` (new) and the overlay/binding enumerations, docs (QA rows, manual Learn chapter, ui-map, ENTRYPOINTS).

## 6. Tests

`tests/learn_map_test.cpp` (label `app`): TAKEN and INS refusals, the param walk per kind, FOLLOW, RELEARN, CLEAR ALL twice, ENC delete, undo, and an `LRNB` save/load round trip. Key-prompt, binding and overlay enumeration tests updated. A sim scenario with goldens.

## 7. Budget

Estimate: flash 6 KiB at most (the coordinator's cap, image end 0xFAFFF at dev 6260b25d, about 16 KiB below 0xFF000), SRAM tens of bytes (`LearnMapView`: cursor, column, await, 2 x u32 mask, tick). Display list: 15 rows x about 8 items, at most about 400 words, against the 1,792 cap and the dearest cited state (999 words); measured through the gallery entry `learn_map` (as `events`). Real numbers are in the commit message and the final report; the image end is checked against `0xFF000`.

## 8. Open for the maintainer

1. `Shift + Vol + Fx` is not built (needs a Level C gesture change); the opener is Config > MIDI F8 and F6 in the dialog.
2. SOURCE steps one CC at a time, so a fine step cannot pass a CC taken by another binding (Shift hops 8).
3. One undo unit per encoder event, not per click group.
4. The dot lasts 250 ms.
5. CLEAR ALL leaves the encoder target.
6. A module param cannot change INS or follow.
7. PARAMETER silently skips params already bound.
8. F1 "go to the parameter's screen" stays a later row.
