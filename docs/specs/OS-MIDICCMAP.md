# OS-MIDICCMAP — the official fixed MIDI CC map (mixer, levels, perform)

Status: specified 2026-11-05, decisions taken the same day (Q1 a, Q2 a, Q3 a, Q4 a, Q5 a), not implemented · Level B, except slice S1 (a device setting appended to `settings.o3st`: Level C) · Backlog row "Official MIDI CC map" (no ID yet; this spec names it OS-MIDICCMAP), approved by the maintainer 2026-11-05 (IDEA triage): "a static table, not Learn slots; verify the map against tracker-ce" · Facts from `dev` at c1dc3125 and `tracker-ce@7fd6c2c` · The map, its value laws and its screen rule are **verified parity** from Polyend's published source (section 2); how it sits beside MIDI Learn, its setting and every detail of our model are **new Community OS behaviour**.

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 a, Q3 a, Q4 a, Q5 a (every recommended default).**

Only what the maintainer must decide. Every technical detail is decided here and listed in section 5.

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | How the map is turned on | a) a device setting **CC MAP** Off / On, default Off, saved in `settings.o3st`; it listens on the ports and channel of MIDI IN and IN CHANNEL, like Learn · b) the same row, session only (RAM, every boot Off, like STATUS LED): no format change · c) always on, no setting | a | The official default is off (F3), and a controller that sends CC 71 to 82 for something else must not move the mixer by surprise; a map that has to be switched on after every boot (b) is a trap for live use. Costs one settings entry (Level C slice, F10) |
| Q2 | A message that is both in the map and bound with Learn | a) Learn wins: if any binding of either store listens to the message, the map ignores it · b) both act · c) the map wins | a | A binding is the user's explicit choice for that controller; one knob moving two things is a surprise. The test is "a binding listens", not "a binding moved something" (a Pickup binding that has not caught the knob still owns it) |
| Q3 | How the map moves a value | a) at once (jump), as the official firmware (F4, F5) · b) Pickup, like Learn's default | a | Parity, and a controller template made for the Tracker expects it; Pickup would need a runtime state per map entry |
| Q4 | Scope: also the official **instrument** CC map? tracker-ce applies about 51 more CCs (1, 3, 5, 7, 9 to 31, 83 to 88, 102 to 119) to the last used instrument (F8) | a) the 32 CCs approved here; the instrument map becomes a new IDEA row ("Official instrument CC map"), built on OS-LEARNFOLLOW's follow resolution · b) add it here as a fifth slice | a | It is outside the approved row, overlaps the most common controller numbers (CC 1 mod wheel moves the cutoff, CC 7, CC 10) and needs per-parameter value laws of its own |
| Q5 | Where the Perform CCs (41 to 48, 51 to 62) act | a) only while the Perform screen's slot page is in view, as the official firmware (F6) · b) on every screen | a | Parity; the mixer CCs act on every screen in both firmwares |

## 1. Goal

Give a MIDI controller the official firmware's fixed control numbers, without any Learn set-up: CC 71 to 82 move the eight track levels and the master's delay, reverb, dry and input levels on any screen; on the Perform screen CC 51 to 62 set the value of the twelve slots and CC 41 to 48 queue a pattern on tracks 1 to 8. A static table in flash, gated by one device setting; Learn bindings keep priority.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | The official firmware routes every incoming control change of either port through one handler per port: the CC-in port mode and channel gate it, then the Perform handler (only while the Performance screen is active), the mixer handler (always), and the instrument and sample-playback handlers (always) | `tracker-ce@7fd6c2c:core/source/mtMidi.cpp:271-303` | verified (tracker-ce) |
| F2 | Two Config rows: "CC in" (None, USB, Jack, USB and Jack) and "CC in channel" (All, 1..16; 0 = All) | `tracker-ce@7fd6c2c:core/source/mtMidi.h:32-37`, `:56`; `core/interface/configEditor/configMenu.cpp:214-215`, `:238-243` | verified (tracker-ce) |
| F3 | Their defaults are None (off) and All | `tracker-ce@7fd6c2c:core/source/mtConfig.cpp:520-521` (sanitised at `:321-329`) | verified (tracker-ce) |
| F4 | Mixer: CC 71..78 are the track volumes 1..8, 79 the delay volume, 80 the reverb volume, 81 the dry mix, 82 the line-in volume; the value is `cc * 100 / 127` (integer), written at once on any screen; the Master screen redraws when it is in view | `tracker-ce@7fd6c2c:core/interface/masterParams/fMasterParams.cpp:3062-3098` | verified (tracker-ce) |
| F5 | Perform slots: CC 51..62 are slots 1..12. The slot goes to a CC row (row 4, beside the pad rows), its value is `min + cc * (max - min) / 127` of the slot FX's range (integer `mapClamped`), and it is applied to the tracks the layer affects, ending the other slots that hold the same FX | `tracker-ce@7fd6c2c:core/interface/performanceMode/fPerformance.cpp:880-904`, `:295-309`; `performanceEngine.cpp:607-621`, `:399-412`; `core/source/util_math.h:17-34` | verified (tracker-ce) |
| F6 | Perform tracks: CC 41..48 are tracks 1..8; the track's performance pattern becomes `cc + 1` and is loaded at the track's next switch. Both Perform ranges act only while the Performance screen is active (F1) | `tracker-ce@7fd6c2c:core/interface/performanceMode/fPerformance.cpp:311-327`, `core/source/mtMidi.cpp:279-280` | verified (tracker-ce) |
| F7 | The official manual documents the same three ranges (CC41 to 48, 51 to 62, 71 to 82): our map of it | `docs/ui/official-manual-map.md` section 6.10 ("Fixed CC map for control", status missing) | documented; now verified by F4 to F6 |
| F8 | tracker-ce also has an instrument CC map (CC 1 cutoff, 3 finetune, 5 tune, 7 volume, 9 filter type, 10 panning, 11 resonance, 12 overdrive, 13 bit depth, 14 reverb send, 15 delay send, 16..19 wavetable / granular, 20..31 and 83..88, 102..119 envelopes; 110..119 macros of a synth instrument) acting on the last used instrument, with no screen gate | `tracker-ce@7fd6c2c:core/interface/instrumentEditor/instrumentEditor.h:38-100`, `fIinstrEdit.cpp:2126-2154`, `core/interface/samplePlayback/samplePlayback.h:26-32` | verified (tracker-ce); out of scope (Q4) |
| F9 | Our MIDI input: the render parses every control change of both ports into `midi::CcInput` (32 entries, controllers 120..127 dropped); the main loop takes 16 a pass (`App::poll_midi_cc`), drops what MIDI IN / IN CHANNEL exclude (`midi_in_accepts`) and runs `midi.cc`, which binds (Learn overlay up), else applies the project store's then the device store's binding; nothing during a WAV export | `app/src/app.cpp:894-910`, `app/include/o3os/app/midi_input.h:19-21`, `app/src/param_actions.cpp:934-1007`; `docs/formats/o3-settings.md` section 6 | code fact |
| F10 | The device scope of the settings block is full (26 of 26); OS-MTC raises it to 32 and takes ids 26..28 (spec, not merged). An appended entry needs no chunk and no version bump; an older build skips and counts it | `model/include/o3os/model/settings.h:25-31`, `model/include/o3os/model/params.h:237-238`, `docs/formats/o3-settings.md:101`; `docs/specs/OS-MTC.md` Q7 | code fact; documented format rule |
| F11 | Our mixer: track VOLUME 0..100 (`track_param::Volume`, 16 tracks), master DRY, REVERB, DELAY 0..100 (`master_param`), all learnable; a MIDI CC on a track's or the master's parameter is final (no history, OS-UNDOGRP D10). The input strip's level is `MixBoard::in_level` 0..100, told to the engine as `gp::strip::Volume` of `audio::kInputStrip` (no model parameter) | `model/src/params.cpp:202-219`, `app/src/param_actions.cpp:972-980`, `app/include/o3os/app/mix_board.h:37`, `app/src/mixer_actions.cpp:79-92`, `:154` | code fact |
| F12 | Our Perform: 12 slots, rows 0 (off) and 1..3 (values), the live value, the affected tracks; `perf_info(fx).min / max`; `exclude()` ends the slots of the same FX; the slot page is `on_perform()` (not the DJ page). E-PERF-07 queues a pattern on a mask of tracks with `Command::Kind::QueueTrackPattern`; the same queue again for a track that waits takes it back; ignored while stopped | `seq/include/o3os/seq/perform.h:55-57`, `:138`, `:164-255`; `app/src/perform_actions.cpp:16`, `:95-122`; `seq/src/sequencer.cpp:590-615` | code fact |
| F13 | The Config MIDI page has 9 rows (`kConfigRows` 9, `kMidiOrder`); the pane holds 16 | `app/include/o3os/app/param_view.h:30`, `app/src/screens/config_screen.cpp:64` | code fact |

## 3. Design

### 3.1 The table (`app/include/o3os/app/cc_map.h`, pure)

`constexpr CcTarget cc_map_target(unsigned cc)` gives `{kind, index}`: `Track` (71..78, index 0..7), `MasterDelay` (79), `MasterReverb` (80), `MasterDry` (81), `Input` (82), `PerfSlot` (51..62, index 0..11), `PerfTrack` (41..48, index 0..7), `None` otherwise. Value laws, integer, as F4 and F5: `cc_map_level(v) = v * 100 / 127`; `cc_map_perf(v, min, max) = min + v * (max - min) / 127`; `cc_map_pattern(v) = v + 1`. The table is 128 bytes of flash or a switch; no RAM.

### 3.2 Where it hooks

In `midi_cc` (`app/src/param_actions.cpp:988`), after the Learn overlay and export checks and after both `apply_cc` calls: if the device setting CC MAP is On and **no binding of either store listens to the message** (`core::learn_find_cc` on both stores, channel and controller; Q2 a), run `cc_map_apply(ctx, cc, value)`. The port and channel filter is the one `poll_midi_cc` already applies (MIDI IN, IN CHANNEL; Q1 a). Main loop only, an action like the rest of `midi.cc`; nothing in the render or an interrupt changes.

### 3.3 What each target does

- **Track 1..8, master DELAY, REVERB, DRY:** `write_param` of the parameter with `cc_map_level(v)`, inside `set_final(true)` as `apply_cc` does for a mixer parameter (no history). On every screen.
- **Input (82):** `MixBoard::in_level = cc_map_level(v)` and the strip told, through the same code as the Mixer's encoder (`edit_level` made callable with an absolute value). On every screen; no history (the board has none).
- **Perform slot 1..12 (51..62):** only while `on_perform(ctx)` (Q5 a). `PerformBank::set_cc(slot, v, sink)` (new, 3.4). A slot with no FX ignores it.
- **Perform track 1..8 (41..48):** only while `on_perform(ctx)` and the sequencer plays (F12: a queue is ignored while stopped; no strip message, a fader would flood it). Pattern `v + 1`; a pattern the project does not have is ignored (strip `CC 4x: NO PAT nnn`). The command is `QueueTrackPattern` with the one track's bit, sent only when the value differs from the last one this map sent for that track (`PerformBoard::cc_pattern[8]`, runtime, cleared when the run stops), so a fader resending the same value never takes the queue back (F12). The dedupe knows only the map's own last value for the track: a TRK PAT or Shift + F6 queue on that track in the same run is not seen by it, so a resent CC equal to the map's last value is swallowed until the run stops (accepted F12 trade-off, new Community OS behaviour).

### 3.4 The CC row of a Perform slot (`seq/perform.h`, pure, host-tested)

`set_cc(slot, v, out)`: the slot enters **CC mode** (a runtime bit `cc_mask_`, never stored), its live value becomes `cc_map_perf(v, min, max)` of its FX, the slots holding the same FX end (`exclude`, as F5), and `apply()` sends it to the affected tracks. `active(slot)` is true for a slot in CC mode whose live value is not 0. The slot's stored row and values are untouched, so the PERF chunk and the scenes never see CC mode (no format change). CC mode ends on: a pad of that slot, on its press or its release (the pad's row as today: a CC that arrives while the pad is held is dropped when the pad comes up, new Community OS behaviour), RESET (`all_off`), leaving the screen (`turn_off`), a scene recall, `change_fx`, `paste_slot`, `swap_slots` of that slot, `load_slots`. The encoder on a held slot in CC mode changes the live value as for a row. The screen lights no pad of a slot in CC mode and tags its value cell `CC`.

### 3.5 The setting

Device setting **CC MAP** (Off 0 / On 1, default Off), appended to the device scope by the documented rule (no chunk, no version bump, default-fill, an older build skips and counts the entry; F10). Its id is the next free device id at the slice's merge: 29 if OS-MTC's A3 and B3 have landed (ids 26..28), 26 if this slice lands first. Whichever of OS-MTC A3 and this S1 merges first makes the one raise 26 -> 32 (`kMaxFields`, `kValueBytes`, `kModelArenaBytes`, the arena asserts), exactly as `docs/specs/OS-MTC.md` A3 describes; the other only appends. Config MIDI page row `CC MAP`, after IN CHANNEL (10 rows; 12 to 13 once MTC lands). Not learnable (a device MIDI routing switch, like CLOCK MODE).

## 4. Slices

| # | Slice | Size | Level | Flash [est] | Depends on |
|---|---|---|---|---|---|
| S1 | CC MAP device setting and Config row | S | C | +0.3 KiB (+the raise if first) | — |
| S2 | The table, the Learn priority, the mixer CCs 71..82 | S | B | +0.9 KiB | S1 |
| S3 | Perform slots 51..62: the CC row | S | B | +0.7 KiB | S2 |
| S4 | Perform tracks 41..48, docs, the manual map row | S | B | +0.4 KiB | S3 |

**S1 — the setting** (S, Level C: the settings file). Files: `model/include/o3os/model/params.h` (`device_setting::CcMap`), `model/src/params.cpp` (the row: choice Off / On, group MIDI, not learnable), `model/include/o3os/model/settings.h` and `model/include/o3os/model/project.h`, `platform/mk66/include/o3os/platform/mk66/mem_map.h` only if this slice makes the raise; `app/include/o3os/app/param_view.h` (`kConfigRows`), `app/src/screens/config_screen.cpp` (`kMidiOrder`: after `MidiInChannel`); `docs/formats/o3-settings.md` (the field paragraph, default file size +8 bytes, the raise paragraph if made here). Acceptance: the previous default file (246 bytes, or MTC's) reads with no entry skipped and CC MAP Off; Off and On round trip byte for byte; 2 is refused by the block and skipped by the reader; an older build skips and counts the entry. Host tests: `tests/settings_format_test.cpp` (the literals), `tests/config_screen_test.cpp` (the row, its place, its values, Delete gives Off). Sim: `sim/scenarios/qa/qa_config.txt` › `qa_cfg_275` (the MIDI page with the row; golden). QA rows: `docs/qa/catalogue/config.md` QA-CFG-275 (the row, Off by default, survives a reboot: unit). Manual: `docs/manual/config.md`, "Choose where the unit listens and sends MIDI" (the row; the page count). SRAM 0; SDRAM arena +0 (the raise, made here to 32 fields: +32 B measured values and generations, two blocks).

**S2 — the table and the mixer** (S, Level B). Files: `app/include/o3os/app/cc_map.h` (3.1), `app/src/cc_map.cpp` (`cc_map_apply`), `app/src/param_actions.cpp` (`midi_cc`: the priority test and the call), `app/src/mixer_actions.cpp` and its header (an absolute setter of a bus level reused by the encoder path), the app library's CMake list. Acceptance: 3.1 to 3.3 for 71..82; CC MAP Off changes nothing that `midi.cc` did before (the existing Learn tests unchanged). Host tests `tests/cc_map_test.cpp`: the table over 0..127 (every target, `None` elsewhere, 120..127 never reach it); the laws (0 -> 0, 64 -> 50, 127 -> 100; perf law at both ends of a signed range); app rig: CC 71 = 127 sets track 1 to 100 on the Pattern screen, CC 78 track 8, CC 79 / 80 / 81 the master levels, CC 82 the input strip (the board and the ParamChange); no journal unit (final); Off: nothing; a Pickup binding on CC 71 that has not caught the knob: the map still does nothing (Q2 a); a binding on CC 71 channel 5 and the message on channel 6: the map acts; the Learn overlay up: the CC binds; a WAV export running: nothing. Sim: `sim/scenarios/qa/qa_mixer.txt` › `qa_mix_073` (`midicc 1 71 64` then the Mixer page; golden). QA rows: `docs/qa/catalogue/mixer.md` QA-MIX-073 (CC 71..78 track levels), QA-MIX-074 (79..82), QA-MIX-075 (a Learn binding wins). Unit check: `amidi` sends `B0 47 7F` and `B0 52 40` to the USB MIDI node with CC MAP On; the Mixer shows track 1 at 100 and INPUT at 50; with Off, nothing.

**S3 — Perform slots** (S, Level B). Files: `seq/include/o3os/seq/perform.h`, `seq/src/perform.cpp` (3.4), `app/src/cc_map.cpp` (the slot branch), `app/src/screens/perform_screen.cpp` (no pad lit, the `CC` tag). Acceptance: 3.4. Host tests: `tests/seq_perform_test.cpp` (set_cc: the law over the FX ranges of `kPerfFxInfo`, a labelled FX, value 0 is off, exclude, apply sends to the affected tracks only, every exit of 3.4, `state()` and the scenes unchanged by CC mode); `tests/cc_map_test.cpp` (CC 51 on the Perform slot page acts, on the DJ page and on Pattern it does not). Sim: `sim/scenarios/qa/qa_perform.txt` › `qa_prf_206` (`midicc 1 51 127`, the slot's value and tag; golden). QA rows: `docs/qa/catalogue/perform.md` QA-PRF-206 (CC 51..62 set the slot value), QA-PRF-207 (a pad, RESET or leaving ends CC mode). Unit check: CC 51 sweeps slot 1's FX on an affected track (heard), the screen follows. SRAM +2 B (`cc_mask_`).

**S4 — Perform tracks and the docs** (S, Level B). Files: `app/src/cc_map.cpp` (the track branch), `app/include/o3os/app/perform_board.h` (`PerformBoard::cc_pattern[8]`, cleared when `view.play.playing` falls), `docs/manual/config.md` (a section "Control the unit with the official CC numbers": the 32-row table, Q2's rule, the Perform screen rule), `docs/ui/official-manual-map.md` section 6.10 (the row becomes `same`, evidence the QA rows), `docs/manual/perform.md` (one paragraph). Acceptance: 3.3's last item. Host tests `tests/cc_map_test.cpp`: playing, CC 41 = 4 queues pattern 5 on track 1 only (one `QueueTrackPattern`, mask 1); the same value again sends nothing (the queue is not taken back); 5 then 4 sends twice; stopped: nothing sent; a missing pattern: nothing sent and the strip message; after the real Stop and Play keys the same value is sent again (the clear follows the sequencer's own stop edge in `App::poll_play_position`, not the badge, which the Stop key sets at once). Sim: none (the queue needs a running sequencer: host only). QA rows: `docs/qa/catalogue/perform.md` QA-PRF-208. Unit check: playing on Perform, `B0 29 01` queues pattern 2 on track 1 at its loop end. SRAM +8 B.

Totals [est]: flash about +2.3 KiB (+ the settings raise if S1 is first); SRAM about +10 B; CPU: main loop only, a table lookup and two store scans a message; render 0.

## 5. Assumptions (decided here; local, testable, reversible)

- A-1 The map is our own static table in flash with the official numbers and laws (F4 to F6); no code is taken from tracker-ce.
- A-2 Tracks 9..12 and the MIDI tracks have no map entry (the official map stops at track 8); Learn reaches them.
- A-3 CC 82 is our INPUT strip's level (the official line-in volume); CC 81 our master DRY.
- A-4 Perform: the slot CCs act on the slot page only (not the DJ page); CC mode is runtime only and never written to the PERF or SCEN chunks.
- A-5 Perform tracks: the official firmware stores the per-track pattern in the project (F6); ours keeps E-PERF-07's runtime queue (no project change). The track CCs ignore the affected mask (the official handler addresses the track by number).
- A-6 Values arriving while a project loads are dropped as today (F9); the mixer writes need a project, the input level does not.
- A-7 The map follows MIDI IN and IN CHANNEL (Q1 a); the official firmware's separate CC port and channel are not copied.
- A-8 Strip messages only for a refused pattern; the mixer and slot moves are silent (the screens show the values).

## 6. Risks

- R1 **The settings raise** collides with OS-MTC: one raise only, the second slice appends (3.5); `qemu` before S1 merges (the arena size).
- R2 **A controller that sends 71..82 for its own use** moves the mixer once CC MAP is On: the manual says so; Q2 a lets a Learn binding claim any of them.
- R3 **CC mode in the Perform bank** touches every exit of a slot: S3's tests walk each one; a missed exit leaves an FX on (the RESET key still ends it).
- R4 **Message rate**: a fader sends up to about 100 messages a second; each is one table lookup and two scans of 32 slots in the main loop (well under 0.1 % of a pass); the 16-per-pass budget of `poll_midi_cc` is unchanged.

## 7. Evidence labels

- The CC numbers, the targets, the value laws, the Perform screen rule, the official default off: **verified parity** (F1 to F6, tracker-ce).
- The Learn priority, the setting on our MIDI IN / IN CHANNEL, the CC tag and exits of CC mode, the dedupe of the track queue (and its blindness to a TRK PAT / Shift + F6 queue on the same track in the same run), the missing-pattern message: **new Community OS behaviour**.
- Whether the official firmware loads an empty pattern for a missing one (F6): **open research question** (ours ignores it).
- Unit behaviour (the sound of a swept slot, the real controller rate): unit checks of S2 to S4.

## 8. State for implementer

- **S1.** Start with: `device_setting` ids `model/include/o3os/model/params.h:187-238`; the settings rows `model/src/params.cpp` (search `PatSwitch`); `Settings::kMaxFields` `model/include/o3os/model/settings.h:25-31`; `kMidiOrder` `app/src/screens/config_screen.cpp:64`. Rules: append only, ids never reused; if the scope is still 26, raise to 32 as `docs/specs/OS-MTC.md` A3 says and update its literals; the older default files stay test literals. Unit check: the row survives a reboot.
- **S2.** Start with: `midi_cc` `app/src/param_actions.cpp:988`, `apply_cc` `:934`; `core::learn_find_cc` `core/src/learn.cpp:55`; `edit_level` `app/src/mixer_actions.cpp:79`, its In case `:154`. Rules: main loop only; no allocation; Off is byte-for-byte today's behaviour; mixer writes final (`set_final`), never journalled. Unit check: amidi CC 71 / 82.
- **S3.** Start with: `PerformBank` `seq/include/o3os/seq/perform.h:164-255`; `exclude` `seq/src/perform.cpp:189`, `pad_down` `:236`, `turn_off` `:310`, `all_off` `:408`, `apply` `:438`; `perf_info` `perform.h:138`. Rules: CC mode never reaches `state()`, the scenes or a file; every exit of 3.4 tested; the bank stays pure. Unit check: the swept FX is heard on an affected track.
- **S4.** Start with: `queue_tracks` `app/src/perform_actions.cpp:98`; `Sequencer::queue_track_pattern` `seq/src/sequencer.cpp:590`; `on_perform` `app/src/perform_actions.cpp:16`. Rules: no sequencer change (the dedupe is app-side); one command per changed value. Unit check: the queued pattern lands at the track's loop end.
- Report per slice: flash, SRAM_L, SRAM_U, SDRAM deltas; scoped `ctest -R "cc_map|settings_format|config_screen|seq_perform"`; the remaining unit check.
- Follow-up to record, not build: the official instrument CC map (Q4 b) as an IDEA row.
