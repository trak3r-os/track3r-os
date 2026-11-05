# OS-LEARNFOLLOW — MIDI synth mode: a Learn binding follows the selected instrument

Status: specified 2026-11-05, decisions taken the same day (Q1 a, Q2 a), not implemented · Level B (no format change: the LRNB chunk already defines the follow flag, F4) · Backlog row "MIDI synth mode: Learn follows the instrument" (no ID yet; this spec names it OS-LEARNFOLLOW), approved by the maintainer 2026-11-05 (IDEA triage) · Facts from `dev` at c1dc3125 and `tracker-ce@7fd6c2c` · That a controller acts on the last used instrument is **verified parity** with the official instrument CC map (F8); choosing it per binding, the key and the rules are **new Community OS behaviour**.

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 a (every recommended default).**

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | How the user makes a binding follow | a) **F5 FOLLOW** in the Learn overlay toggles the target's binding between "this instrument" and "the selected instrument"; a new binding is fixed, as today · b) a new binding of an instrument parameter follows by default; F5 pins it to its instrument · c) a device setting LEARN FOLLOW that decides for every new binding | a | No change for existing users and projects; one key, shown on the row, beside F2 MODE which works the same way (on the target's binding). b is closer to the official firmware (F8) but changes what Learn does today without being asked |
| Q2 | Should a track parameter (volume, pan, mute, solo of the Mixer) follow the cursor's track too | a) no: instrument parameters only; record "Learn follows the track" as an IDEA · b) yes, a third slice | a | b changes the LRNB chunk's meaning (follow is valid only on an Instrument binding today, F1 and F4): a minor version, a loader rule and a round trip, so Level C, for a case the row does not ask for |

The official instrument CC numbers as a ready-made set of follow bindings is OS-MIDICCMAP's Q4 (recommended there: a separate IDEA row).

## 1. Goal

Play the Tracker as a synth from a MIDI keyboard with knobs: the knobs act on whichever instrument is selected, so the same controller shapes instrument 3, then instrument 7, without binding again. The data path exists (F1, F2, F4); what is missing is the way to make such a binding (F3) and two guard rules.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | `kLearnFollow` is flag bit 0 of a `LearnBinding`; `learn_valid` accepts it only on an Instrument binding with index 0; `learn_targets`, `learn_plan` and `learn_store_valid` already treat a follow binding as "this field of any instrument" (a fixed and a follow binding of the same field may coexist) | `core/include/o3os/core/learn.h:35-45`, `core/src/learn.cpp:18-40`, `:66-106`, `:109-122` | code fact |
| F2 | `apply_cc` resolves a follow binding to `ViewState::instrument` and writes that instrument's field; a free slot finds no parameter. A host test drives it with a binding written by hand | `app/src/param_actions.cpp:944-950`; `tests/params_app_test.cpp:735-748` | code fact |
| F3 | `learn_bind` never sets `flags`; no action or key does. Only tests and the `o3pj_tool` generator make follow bindings | `app/src/param_actions.cpp:892-931`; `tests/tools/o3pj_tool.cpp:738` | code fact |
| F4 | The LRNB chunk (project and settings files) already carries `flags` bit 0 follow ("the id's index is 0 and only its kind and field count"); the codec reads and writes it and a test round-trips it | `docs/formats/o3-settings.md:103`; `tests/param_model_test.cpp:785`, `:890` | code fact; documented format |
| F5 | The Learn overlay: F1 UNBIND and F2 MODE act on the target's binding (`learn_find_param` on the focus parameter), F3 DELETE, F4 CLEAR ALL, F8 BACK; F5 to F7 are free. The list already names a follow binding's owner `INS *` | `app/src/bindings.cpp:943-954`, `app/src/key_prompt.cpp:76-78`, `app/src/param_actions.cpp:818-839`, `app/src/screens/overlays.cpp:110-125` | code fact |
| F6 | `ViewState::instrument` is the last used instrument (1..64, the strip's INS): what a MIDI keyboard note plays and what LIVE recording records | `app/include/o3os/app/view_state.h:244`; `app/src/app.cpp:912-950`; `app/include/o3os/app/rec_place.h:11` | code fact |
| F7 | Instrument field ids: the sampler, source, envelope and MOD rows (0x00..0x77) and VEL> (0x8B) belong to a Sample instrument, the MIDI rows (0x80..0x8A) to a MIDI one, 0x100 + i is parameter i of whatever module the slot holds (another module, another meaning) | `model/include/o3os/model/params.h:30-153` | code fact |
| F8 | The official firmware's instrument CC map acts on the last used instrument (`mtProject.values.lastUsedInstrument`), on any screen once CC in is on; its page is "Instrument MIDI Mapping", whose text says "To use MIDI Mapping, the sequence must be stopped" although the handler has no such gate | `tracker-ce@7fd6c2c:core/interface/instrumentEditor/fIinstrEdit.cpp:2126-2154`, `core/interface/samplePlayback/fSamplePlay.cpp:3273-3296`, `gInstrEdit.cpp:101`, `:367`; `core/source/mtMidi.cpp:271-303` | verified (tracker-ce) |
| F9 | Our map of the official manual lists "MIDI synthesizer mode (Instrument MIDI Mapping)" as missing, Learn standing in for it | `docs/ui/official-manual-map.md:262` | documented |
| F10 | A follow binding's Pickup state is per resolved parameter: after a selection change the knob must catch the new instrument's value (`LearnRuntime::key`) | `core/include/o3os/core/learn.h:85-120` | code fact |
| F11 | The learn actions are in the `kAutoArea` list (their unit goes to the ring of its first record: SOUND for the project store) | `app/src/actions.cpp:165-168`, `:349-350` | code fact |

## 3. Design

### 3.1 F5 FOLLOW (`learn.follow`, Learn overlay)

On the target (the parameter under the cursor of the screen below, `focus_param`), like F2:

1. Not an Instrument parameter: `FOLLOW: INSTRUMENT PARAMETERS ONLY`. A module parameter (field 0x100 or more): `FOLLOW: NOT FOR A MODULE PARAMETER`. Nothing bound to it: `NOTHING BOUND TO THIS`.
2. The binding `b = slots[learn_find_param(project store, target)]`.
   - `b` is fixed: the new binding is `{param_id(Instrument, 0, field), b.cc, b.channel, b.mode, kLearnFollow}`; strip `CC 74 FOLLOWS: CUTOFF`.
   - `b` follows: the new binding is `{target, b.cc, b.channel, b.mode, 0}` (pinned to the target's instrument); strip `CC 74 PINNED: CUTOFF INS 03`.
3. The store is changed with `learn_plan`, as `learn_bind` does: one follow binding per field (a second one of the same field gives its slot to the new one), one binding per message (the toggled slot is cleared or rewritten). The project store only (instrument parameters are the project's), through the editor: one undo unit in SOUND (`learn.follow` added to `kAutoArea`). The runtime state of every slot touched is reset.

### 3.2 Rebinding keeps follow

`learn_bind`: when the target's current binding (`learn_find_param`) is a follow binding, the new binding is a follow binding of the same field (as the mode is kept today). Binding a field that has no binding makes a fixed binding (Q1 a).

### 3.3 The fit rule

A follow binding acts only when the selected instrument has the field with its meaning (F7): a sampler, source, envelope, MOD or VEL> field needs a Sample instrument; a MIDI row needs a MIDI instrument; nothing else. Otherwise the message does nothing (no write, no journal, no strip message: a knob would flood it), exactly as for a free slot today (F2). One pure helper `follow_fits(InstrumentKind, field)` in `app/src/param_actions.cpp` (or `learn.h` if the kind is passed), used by `apply_cc`.

### 3.4 What the overlay shows

The target line: when the binding that acts on the target follows, the instrument beside the name reads `INS *` (not `INS 03`) and the binding text ends `FOLLOW` (`CC 74 PICKUP FOLLOW`). The key row: `FOLLOW` on F5 when the target is an instrument parameter with a binding; blank otherwise (the A1 rule of the dialogs, QA-GLB-118). The list keeps `INS *` (F5).

### 3.5 Stored

Yes: a follow binding is a project-store binding with bit 0 set, which the LRNB chunk already defines and the codec already round-trips (F4). No layout, size or meaning change, so no version bump; an older build of this OS reads it the same way (the flag predates this item).

## 4. Slices

| # | Slice | Size | Level | Flash [est] | Depends on |
|---|---|---|---|---|---|
| S1 | `learn.follow`, rebind keeps follow, the fit rule | S | B | +0.5 KiB | — |
| S2 | The overlay's text and key, scenario, QA rows, manual | S | B | +0.2 KiB | S1 |

**S1** (S, Level B). Files: `app/src/param_actions.cpp` (`learn_follow`, `learn_bind`, `apply_cc`'s fit test), `app/src/action_handlers.h`, `app/src/actions.cpp` (`make_action("learn.follow", learn_follow, core::kActionUndoableIfOpen)` and `kAutoArea`), `app/src/bindings.cpp` (Learn context, tap F5). Acceptance: 3.1 to 3.3. Host tests, `tests/learn_follow_test.cpp` (new) on the app rig: bind CC 74 to instrument 3's CUTOFF, F5: the store holds `{Instrument 0 CUTOFF, follow}` and nothing else on CC 74; `midicc` moves the selected instrument's cutoff, then instrument 5's after `instrument.select 5`, instrument 3 left alone; F5 on instrument 5's CUTOFF pins it to 5; rebind on a followed field stays follow and keeps the mode; F5 when another follow binding of CUTOFF exists on CC 20: one follow binding of CUTOFF remains, on CC 74, and `learn_store_valid` holds; a module parameter, a track volume, an unbound target: the messages and an unchanged store; the fit rule: a MIDI instrument selected, a follow CUTOFF does nothing and writes no journal; a follow MIDI VELOCITY with a Sample instrument selected does nothing; Pickup: after a selection change the knob must catch the new value first (F10); undo: F5 is one SOUND unit and Undo gives the fixed binding back; the project store with a follow binding made by F5 survives the LRNB write and read (the codec, `formats/settings_chunks.h`). Scoped: `ctest -R "learn|params_app"`. QA rows: none in S1 (the overlay text is S2's). Unit check: none (no hardware path changes; S2 carries the unit row).

**S2** (S, Level B). Files: `app/src/screens/overlays.cpp` (target line, binding text), `app/src/key_prompt.cpp` (`kLearnOptions`: F5 FOLLOW), `sim/scenarios/params.txt` (a scene `learn_follow`: bind, F5, the overlay), goldens; `docs/qa/catalogue/global.md` QA-GLB-174 (F5 FOLLOW toggles; the overlay shows `INS *` and `FOLLOW`), QA-GLB-175 (a follow binding moves the selected instrument, not another; nothing on an instrument of another kind), QA-GLB-176 (the follow binding is saved with the project; unit: reload and move the knob); `docs/manual/panel.md` (the Learn paragraph grows into a short "Learn a MIDI control" subsection: open, move a control, F2 MODE, F5 FOLLOW; "use your keyboard as a synth: bind the knobs on one instrument, press F5 on each, then select any instrument"); `docs/ui/official-manual-map.md:262` (the row: different, Learn with FOLLOW stands in, evidence QA-GLB-175). Acceptance: the goldens; QA-GLB-118's row text gains `FOLLOW` on F5 (update that row). Unit check: a USB MIDI controller (`amidi` CC 74 sweeps) moves the cutoff of instrument 1, then of instrument 2 after selecting it on the Pattern screen, heard on a playing track.

Totals [est]: flash about +0.7 KiB, SRAM 0, CPU: main loop only, one kind test a message.

## 5. Assumptions (decided here; local, testable, reversible)

- A-1 "Selected instrument" is `ViewState::instrument`, the last used instrument (F6), as `apply_cc` already does and as the official map does (F8). Not the instrument of the step under the cursor.
- A-2 One follow binding per field per store (as one binding per parameter today); a fixed binding of the same field on some instrument may coexist with it (F1).
- A-3 When the target has both a fixed and a follow binding, F5 acts on the one `learn_find_param` finds first (the lower slot).
- A-4 Module parameters never follow (F7: parameter i means something else in another module). A follow by module id is a possible follow-up, not built.
- A-5 A follow binding on an instrument of the wrong kind, or a free slot, is silent (3.3).
- A-6 The follow binding's journal unit is that of the resolved instrument parameter (today's `merge_click` by resolved id): a sweep on instrument 3 then on 5 is two units.
- A-7 No new setting, no format change (3.5).

## 6. Risks

- R1 **A surprise write** on the wrong instrument after a selection change: Pickup (the default) waits for the knob to catch the new value (F10); Jump bindings jump, as documented for Jump.
- R2 **The fit rule changes** what a hand-made follow binding did on a MIDI or module instrument (it wrote unused bytes): no UI made such bindings (F3); the test pins the new rule.
- R3 **The key row** of the Learn dialog grows to six keys: QA-GLB-118 and its goldens are updated in S2.

## 7. Evidence labels

- A controller acting on the last used instrument: **verified parity** in concept (F8); the official firmware does it with a fixed CC list, ours with Learn bindings.
- F5 FOLLOW, pinning, rebind keeping follow, the fit rule, the overlay text: **new Community OS behaviour**.
- Whether the official firmware refuses its instrument map while the sequencer plays (the page's text, F8): **open research question**; ours has no such gate.

## 8. State for implementer

- **S1.** Start with: `learn_mode` `app/src/param_actions.cpp:818` (the model for an action on the target's binding); `learn_bind` `:892`; `apply_cc`'s follow branch `:944-950`; `core::learn_plan` `core/src/learn.cpp:66`. Rules: the project store only, through `store_set` (the editor: undo); `learn_store_valid` holds after every action; `learn_valid` is unchanged (follow stays Instrument index 0); no change to `LearnBinding`, the LRNB codec or the settings file. Unit check: none.
- **S2.** Start with: `owner_text` `app/src/screens/overlays.cpp:110`, the target line `:160-185` (in `LearnOverlay::render`, `:145`); `kLearnOptions` `app/src/key_prompt.cpp:77`; the scene `learn_target` in `sim/scenarios/params.txt`; QA-GLB-118 `docs/qa/catalogue/global.md:69`. Rules: the row text and dialog line name the same keys (QA-GLB-118); goldens regenerated with the 44.1 kHz build. Unit check: as S2 above.
- Report per slice: flash, SRAM deltas; `ctest -R "learn|params_app|overlay|key_prompt"`; the unit check left.
- Follow-ups to record, not build: Learn follows the cursor's track (Q2 b, Level C); a module parameter that follows by module id (A-4).
