# OS-PIANOVIB: a held vibrato pad in PIANO pad mode

Level B (live path, no persistent format). New Community OS behaviour (backlog row OS-PIANOVIB, approved by the maintainer 2026-11-11).

## Goal

In PIANO the two unused pads of col 12 (0-based col 11, rows 2 and 3: pads 23 and 35; `piano_pad` returns `PadFn{}` for them today, between OctUp at row 1 and OctDown at row 4) become one momentary vibrato pad. While either is held the internal voices play the Finetune LFO (FX 33); a MIDI instrument gets CC1 127 on the press and CC1 0 on the release. Pads are on/off, no pressure.

## Facts

- The FX 33 machinery exists (OS-PERF-LFO-STEP): `VoicePool::step_lfo_[2][track]` holds the rate index + 1; `perf_index` reads it, `perf_dest` starts, moves or ends the TUNE LFO on a sounding voice (`perf_sync(v, 2)`), and a note that starts on a track with a held `step_lfo_` starts the LFO (`mods_start`). The LFO is the instrument's own finetune LFO (shape, amount), or `kPerfLfoMod` (a triangle at 50 %) when the instrument stores no audible amount. It moves the fine tune in cents (clamped to +-100), once a block, in the existing modulator pass: no new per-block cost.
- The live pad path is the PIANOBEND one: `PreviewSink` -> `LiveQueue` (SPSC, 8-byte `LiveEvent`) -> `Engine::play_live` (render context) -> `MidiLink::live_event` first.

## Design

Pad map: `PadKey::Vib` on pads 23 and 35 of the PIANO map (`value` unused). Both pads are the same control: the vibrato is on while at least one of them is held (`VibHold`, a two-bit mask in the edit state, main loop only, like `BendHold`).

Path: `play_fn` (press) and the pad release in `App::run_once` call `PreviewSink::pad_vibrato(on, instrument)` only on the edge (none held -> one held, last one let go). `reset_input` clears the hold and sends the off. The sim and MK66 sinks push `LiveEvent{Vibrato, key = on}`; the off is pushed with `release()` (a refused push is repaired by the all-notes-off, as a note off), the on with `push()` (dropped when the queue is full, the pad is simply pressed again).

Engine: `LiveKind::Vibrato` calls `VoicePool::live_vib(on, tracks)`: on sets `step_lfo_[2][t]` = `kVibRate + 1` for the tracks of the live notes (`LiveNotes::tracks()`) and remembers them in `vib_mask_`; a note that starts while it is held joins (`live_vib_track(track)`, called next to `live_bend_track`, before `note_on`). Off clears the row for every track in the mask and `perf_sync`s the sounding voices (the instrument's own modulator comes back). The live path has no allocation, no loop beyond `kVoiceTracks` (16), render context only, flash (`O3OS_AUDIO_COLD`).

- Defaults (decision, record): rate index 13 of `kLfoRate` (1.5 steps per cycle, about 5.3 Hz at 120 BPM; tempo-synced as every FX 33); depth is what FX 33 gives: the instrument's own finetune LFO amount, else +-50 cents.
- AllOff of the pad source or of all sources: vibrato off (like the bend). A stop (`Engine::stop_live_notes`, the start of an export) puts it off too: `live_vib(false)`, and `MidiLink::stop_live_pads` sends the CC1 0. `perf_save`/`perf_clear` of the export already put away `step_lfo_`; the off before it leaves nothing for the export to inherit.
- Known limit (as the bend, minimal change; the fix, `live_vib_end` called from the Off case, costs +32 B and the bench image has 16 B of slack): a live note that ends while the pad is held keeps its track in `vib_mask_` and its `step_lfo_[2]` row until the pad is let go, so a sequencer note that starts on that track meanwhile has the vibrato too. Follow-up when flash allows.
- A transport stop (FxReset) zeroes `step_lfo_` and so ends the running vibrato on the sounding voices, but leaves `vib_on_` and `vib_mask_`: with the pad still held the next live note starts with the vibrato again, and the release clears everything. Not a defect: no vibrato outlives the pad.
- Edge (documented, not coordinated): a track in `vib_mask_` gets its FX 33 row overwritten (a sequencer FX 33 on that track is replaced while the pad is held) and cleared on release, so a sequencer FX 33 that was running on that track stops at the release until its next step sets it. Recording the vibrato is out of scope (the Fx 33 recording of live pads is a follow-up if wanted).
- Amiga and module instruments do not follow the TUNE modulator: no vibrato on the voices (same limit as the bend); the MIDI CC still goes out when the instrument in view is MIDI.

MIDI: `MidiLink::live_event` takes `LiveKind::Vibrato` for the instrument in view, when it is `InstrumentKind::Midi`: `MidiCc` explicit controller 1, value 127 on the instrument's route (ports, OFFSET, channel); returns true when taken so the voices get nothing, false otherwise (the voices run the LFO). The route is kept in `vib_route_` (as `bend_route_`) so the release sends CC1 0 to the route that was pressed even if the instrument changed. AllOff of pad or all sources and `stop_live_pads` send the CC1 0 to a held route and return it to rest. `MidiOut::send` ignores a full staging queue for a `MidiCc` (out.cpp, as for the bend), so a CC1 lost to a full queue is not retried: the route is forgotten once sent, and the next press and release (or route change, or AllOff) sends one again.

## LEDs

Vib pads: `Back` (dim) at rest, `Front` while held, like the Bend pads (a pure function of `VibHold`). Both pads light together.

## Real-time safety

No allocation; one `step_lfo_` row write per mask track and one `perf_sync` per sounding voice, once per press and release; render context only (single writer of `step_lfo_`/`vib_mask_` is the render: layer_event and live_vib both run there). `LiveEvent` stays 8 bytes. No on-disk format changes.

## Verification

Host: pad map and hold tests (`tests/pad_modes_test.cpp`), engine tests (`tests/audio_live_test.cpp`: on/off, a note that starts while held, AllOff, stop_live_notes), MIDI tests (`tests/audio_live_midi_test.cpp`: CC1 127/0 on the route, AllOff, stop_live_pads). Hardware: ear and scope check of the vibrato rate and depth on a sampler instrument (hardware check pending).
