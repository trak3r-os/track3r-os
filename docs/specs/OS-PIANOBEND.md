# OS-PIANOBEND: a pitch-bend strip in PIANO pad mode

Level C (audio path). New Community OS behaviour (backlog row OS-PIANOBEND). Not recorded into the pattern (OS-PIANOBEND-REC is separate).

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
