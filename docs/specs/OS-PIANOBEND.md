# OS-PIANOBEND: a pitch-bend strip in PIANO pad mode

Level C (audio path). New Community OS behaviour (backlog row OS-PIANOBEND). Recording into the pattern and MIDI-in pitch bend: section OS-PIANOBEND-REC below.

## Goal

In PIANO the pads of cols 10-11 (0-based 9, 10; `app/include/o3os/app/pad_maps.h`, today `PadKey::None`) become a momentary bend strip: up 2, up 1, down 1, down 2 semitones, back to 0 on release. Pads are on/off, no pressure.

## Facts

- `piano_pad(pad)` returns `PadFn{}` for cols 9 and 10 (pad_maps.h); `tests/pad_modes_test.cpp:685` asserts it.
- Pads reach sound only through `PreviewSink` (`app/include/o3os/app/actions.h`) into `LiveQueue` (SPSC, 64 x 8 bytes, `audio/include/o3os/audio/live_notes.h`), drained by `Engine::play_live` (`audio/src/engine.cpp`, up to `kLivePerBlock` events at block start). The main loop is the only producer, the render the only consumer.
- A sampler voice has a per-voice pitch offset (`slide_q16`, `slide_tgt`, `slide_left`) added in `mod_block`/`repitch` once per block; its zero is `VoicePool::tune_base_q16(track)` (performance TUNE). `layer_event` already moves a sounding voice's offset and target by a TUNE change (`audio/src/voices.cpp`).
- Amiga voices and module instruments do not follow TUNE, so they do not bend (limitation, below).
- MIDI: `EventKind::MidiPitchBend` (a = low 7, b = high 7, c = channel, flags = ports); `seq::pitch_bend_value(cents, range)` is 14-bit centred on 8192 (`seq/include/o3os/seq/midi_route.h`); `midi_pb_range` defaults to 2.

## Design

Pad map (same value on both columns of a pair, so each strip is two pads wide): row 1 `+2`, row 2 `+1`, row 3 `-1`, row 4 `-2`. `PadKey::Bend`, `value = semitones + 2` (0..4, never 2).

Rule (`BendHold`, a pure struct in `app/include/o3os/app/pad_maps.h`): the pads held are a press order (at most 8); the active bend is that of the last pressed pad still held; releasing the active pad falls back to the previous still-held pad, and to 0 when none is held. Each change of the active value (including to 0) is sent once.

Path: `pads_play` (`pad_mode_actions.cpp`) press and the pad release in `app.cpp` call `PreviewSink::pad_bend(semitones, instrument)` (default no-op). The sim and the MK66 sinks push `LiveEvent{Bend, note = int8 semitones, instrument}`. A bend is dropped when the queue is full except 0, which is retried via the existing flush repair (`flush_`), and `AllOff` of every source also sets the bend to 0.

Engine: `LiveKind::Bend` calls `VoicePool::set_live_bend(q16)`: one `std::int32_t` plus a track mask of tracks sounding live notes, both written by the render only (single writer, no lock). It adds to `tune_base_q16` for tracks in the mask, so a note started during the hold also bends. Returning to 0 clears the mask. The existing per-block offset path applies it (`mod_block` re-pitches once a block): no per-sample work, no allocation. The step lands at the block edge and is not glided: the delta is added to the voice's `slide_q16` and `slide_tgt` at once, so a sounding note jumps by the interval at the next block (the 4 to 6 ms of a block, no ramp); a gliding bend is a follow-up if the unit shows a zipper.

A track stays in `bend_mask_` for the whole hold, so the sequencer's notes on that track are bent too while the strip is held (the offset's zero is the bend, see `tune_base_q16`); releasing the strip clears the mask. The sequencer's own pitch bends on a MIDI channel (the stop-centre at `seq/src/sequencer.cpp:324` and the micro-tune bend at `:2302`) can override a held strip bend on the same channel and ports until the strip is pressed again; they are not coordinated with the strip. `Engine::stop_live_notes` (the start of an export) puts the bend back: `VoicePool::live_bend(0, 0)` for the voices and `MidiLink::stop_live_pads` for the MIDI route, so an export renders unbent; a centre the output queue refuses keeps its route and is sent again by the next release, route change or all-notes-off.

MIDI: `MidiLink::live_event` handles `Bend` for a MIDI instrument: sends `MidiPitchBend` of `pitch_bend_value(semitones * 100, range)` on the channel and ports of the instrument, remembers channel and ports so a 0 re-centres the channel that was bent even if the instrument changed during the hold. A bend 0 always zeroes both sides.

## Which voices bend

Sampler/synth voices on the tracks of live notes, and MIDI instruments. Not Amiga voices and module instruments (they ignore TUNE): follow-up OS-PIANOBEND-AMIGA, noted in the backlog.

## LEDs

Bend pads: `Back` (dim) at rest, `Front` while held (the pad's own held state; the sink-side LED role is a pure function of the hold mask).

## Real-time safety

Main loop: mask arithmetic only. Render: one switch case, a few integer stores, inside the existing per-block drain (bounded by `kLivePerBlock`). No heap, no blocking, no float in the control path beyond the existing q16 cents conversion. `LiveEvent` stays 8 bytes (static_assert kept). No persistent format changes.

## Tests

Host (`pad_bend_test`, label `app`): pad-to-bend map (8 pads, four values, no zero), `BendHold` overlap (press A, press B, release B returns A, release A returns 0; release of a non-active pad sends nothing), MIDI 14-bit scaling (centre 8192, +-2 at range 2 is full travel, clamped), engine: a held live note bends by +2 semitones at a block boundary and returns on 0; the AllOff reset. `pad_modes_test` updated for cols 9-10. Goldens: none expected (no screen change).

## Risks

A lost release leaves a bent note: covered by the AllOff reset and by the mask being cleared on 0. A full queue could drop a 0: the sink keeps a pending flag and resends it (same repair as a lost Note Off). Unit checks owed: audible bend of a sampler note and a MIDI synth, release returns to pitch, two pads held.

## OS-PIANOBEND-REC: recording the strip, and MIDI-in pitch bend

Level C (audio path, ISR-fed input). New Community OS behaviour, approved by the maintainer. No persistent-format change.

### Decision (a): recording

The strip writes the existing slide FX, no new FX id: press of a +n / -n pad writes `kSlideUp` / `kSlideDown` (41 / 42) with value `n * 16` (sixteenths of a semitone, `seq::slide_sixteenths`, `seq/include/o3os/seq/fx.h:347`) into an FX column of the step the press lands on; release writes `kSlideUp` value 0 (back to the note's pitch) on the step the release lands on. Slide holds until the note or FX ends, so playback bends at the press step and springs back at the release step. A press and a release on one step overwrite the column (the release wins, as `rec_fx_write` does for the same type). Targets: the `rec_open` tracks of the take, else the cursor track; only Sample instruments (Amiga, module and MIDI instruments do not slide, `rec_fx_place` is not called for them). Chords of pads: only the active value of `BendHold` is written (one FX per change, same rule as the live send). Full FX columns raise the existing once-per-take notice (`rec_fx_full`);.

Path: `play_fn` (`app/src/pad_mode_actions.cpp:62`, `PadKey::Bend`) calls `rec::rec_bend` on press; the release in `app/src/app.cpp` (next to `bend.release`) invokes action `rec.bend` (arg = bend + 2) only while `edit.take_open`. The write reuses the take machinery of `app/src/rec_actions.cpp` (`rec_fx_write`, one undo group per take).

### Decision (b): MIDI-in pitch bend

`CcParser::next` (`midi/include/o3os/midi/cc_in.h`) today drops `0xE0`. It now returns `ChannelMsg::Kind::Bend` (14 bit). `CcInput::feed` (render) stores the latest value in one atomic word (a mailbox, single writer render, single reader main loop; a bend flood cannot fill the 32-entry note ring or drop notes). `App::poll_midi_notes` takes it after the note loop, applies `midi_in_accepts`, drops an unchanged value, and calls `PreviewSink::midi_bend(value14, instrument)`. Sinks push `LiveKind::BendFine` (`audio/include/o3os/audio/live_notes.h`; key = low 7, note = high 7; `LiveEvent` stays 8 B); the centre 8192 goes through `release()` (flush repair, as a pad bend 0).

Range and smoothing: fixed +-2 semitones, full travel = 8192 counts (the standard bend range, and the strip's own range; the sampler offset is q16 `(v14 - 8192) << 4`: no division, no float). MIDI instruments get the raw 14-bit value (`MidiLink::live_event`, `seq/src/midi_link.cpp:174`). No extra glide: the value is applied once per block by the existing `mod_block` re-pitch; the wheel's own 14-bit resolution is finer than a block step; the main loop coalesces to the last value per pass.

### Facts

- Live bend today: `VoicePool::live_bend(int, mask)` (`audio/src/voices.cpp:2437`) writes `bend_q16_` / `bend_mask_` (render only) and moves a sounding voice's `slide_q16` / `slide_tgt` by the difference; refactored to a q16 core that both the semitone and the fine event call.
- Note path of MIDI in: render -> `notes_` ring -> main loop (`cc_in.h` `pop_note`); the bend does not use it.

### Real-time safety

Render: parser branch plus one atomic store; one extra switch case in the existing bounded drain; integer maths only. Main loop: one atomic exchange per pass. No heap, no blocking, no `double`, no printf. Atomic `std::uint32_t` (lock-free on the M4).

### Tests

`pad_bend_test` / `pad_modes_test`: press and release on expected steps with the expected FX (slide 41/42 values 16/32, release 0), full-column notice, non-sample instrument writes nothing, playback bends a sampler voice and springs back at the release step. `midi_cc_in_test`: the parser routes 0xE0 (14-bit, channel), the mailbox keeps the last value. Engine: `BendFine` scales (+8191 = just under +2 semitones, 0 centre, -8192 = -2) and the voice offset follows; MIDI route sends the raw value. Goldens: none (no screen change).

### Fold (review minors)

- The wheel's mailbox is filtered by MIDI IN / IN CHANNEL in the render before the store (`CcInput::set_bend_filter`, set by the main loop each pass), so a bend of another port or channel cannot overwrite one that is listened to.
- The App forgets the last wheel value sent (`midi_bend_value_` = 0xFFFF) when the strip moved the shared offset (`BendHold::gen`), on `reset_input` (all-sources all-notes-off) and while an export runs, so the next wheel value is always applied.
- The platform refuses `BendFine` while the engine is the export's (`offline_wanted`), like a new note; the simulator has no such refusal.
- A strip still held when the take ends (stop, long-take split) writes no spring-back: nothing records at that row any more; the Slide holds to the next note of the track. Known limit, no data hazard.
