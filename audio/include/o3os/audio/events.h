// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The event the sequencer hands to the voices, the MIDI out stage and the UI (ADR 0013 D5; OS-005c). The sequencer is a pure function
// `step(block_start, n) -> SeqEvent[]` over the sample timeline (seq/sequencer.h); this header is its output contract and lives in the
// audio layer because the voices of OS-005d, which consume it, are audio and may not include the sequencer (which reads the project model).
//
// STABLE LAYOUT. 12 bytes, alignment 4, trivially copyable (the static_asserts below hold it). A field's meaning is fixed by `kind`; a new
// kind may be added at the end of its group, an existing kind's fields never change meaning (a voice built against this header keeps working).
//
// TIME. `offset` is the sample at which the event takes effect, 0..n-1 within the block that step() was asked for, nearest sample to the
// exact time (halves up). The voice kernels take (begin, end) and split the block at the offsets (ADR 0013 D5). Events come out sorted by
// offset; events with the same offset keep the order the sequencer made them in, which is a contract: within one track a NoteOff or an FX
// change comes before the NoteOn of the same instant (the voice is configured before it triggers), and tracks go in index order.
// The FRACTION. Tick and MidiClock carry the exact time to a fraction of a sample: their sample is the nearest one, as for every event, and `value` is the exact time minus that
// sample in units of 2^-32 sample, a signed number within half a sample either way (core::cycles_from_samples_sfrac turns the pair into CPU cycles).
// A time that has already passed when the event is made (a transport command that lands mid-block, a backlog) comes out at offset 0.
//
// Per-kind fields (`track` is 0..15, kNoTrack for an event that is not about a track):
//   NoteOn      a note 0..127 | b instrument slot 0..63 | c velocity 0..127, kVelocityDefault = the instrument's own | flags kEvRetrigger
//               when it is a roll repeat of a step that is already sounding
//   NoteOff     a note | b instrument slot: the track's voice enters its release
//   NoteCut     the track's voice stops at once (step note CUT)
//   NoteFade    the track's voice fades out fast (step note FADE)
//   ModSet      a persistent per-track modulation or override starts or changes. The winner of an arbitration (seq/control.h): the
//               step FX of the sequencer layer (FX column 1 or 2) or the performance layer.
//               a FX type (the sequencer layer's FX when there is one, else 0) | b its value | c layer (kLayerFx1 / kLayerFx2 /
//               kLayerPerf) | flags the group (seq::FxGroup) | value the performance layer's value (int16 range), 0 for the others
//   ModEnd      the group on that track has no source left: the voice returns to the instrument's own value | flags group
//   FxReset     every FX of every track ends (at play): track kNoTrack
//   Tick        Speed ticks (core FX 43, off unless asked for): a tick index 0..b-1 within the step | b ticks per step |
//               value the signed fraction of a sample (see below) of the exact time; the Amiga engine's tick processor runs on it
//   TrkFx       (OS-COREFX) a tracker FX of the row, row-scoped (it lasts the row it came with: core FX 46..62, kFxTrkFirst..kFxTrkLast): a FX type | b its value | flags the FX column (0 or 1). It comes before
//               the row's NoteOn and its Tick 0 on the same sample; a row that has none carries none over (audio/tracker_fx.h)
//   StripMute   (OS-MUTE) the track's strip mute takes the value `a` (0 or 1) at this sample: the quantized mute reaching its boundary. The engine applies it to the graph (Graph::set_strip_mute: the
//               level ramps over the block the sample is in, no click); the sequencer reads the project's flag for the value when it emits the event
//   Metro       (OS-METRO) a beat of the metronome, on the first microstep of every step whose index is a multiple of 4 (a bar is 16 steps): a 1 on the first beat of a bar, else 0 | value the step index in the
//               pattern | flags kEvCountIn while the transport is counting in (the count-in of a recording, below). Emitted only while the sequencer is told the metronome is on (seq_param::Metro)
//   MidiNoteOn  a note | b velocity 1..127 | c channel 0..15
//   MidiNoteOff a note | c channel
//   MidiCc      a CC slot 0..5 (CC A..F: the device's table gives the numbers), or kMidiCcExplicit | controller 0..127 (OS-INSTUI S8b: the instrument names the controller) | b value | c channel, kChannelDefault: the device's
//               notes-out channel (set in the config)
//   MidiProgram a program | c channel (kChannelDefault as above)
//   MidiPitchBend / MidiChanAt / MidiPolyAt   (OS-INSTUI S8b) a pitch bend 14 bits, a = the low 7 | b the high 7 | c channel; channel aftertouch: a value | c channel; polyphonic aftertouch: a note | b value | c channel
//   The route of a channel message (NoteOn, NoteOff, Cc, Program, PitchBend, ChanAt, PolyAt), OS-INSTUI S8b: `flags` the ports (kRoutePorts: 0 = the device's MIDI OUT, 1 DIN, 2 USB, 3 both) and `value` a delay in
//   microseconds (0 .. kRouteDelayMaxUs, the instrument's OFFSET; MidiOut adds it to the due time). Both 0 is what these events were before.
//   MidiClock   one 24 PPQN timing clock on the NOMINAL grid (swing does not move it) | value the signed fraction of a sample, so that
//               the MIDI out stage can place it to a fraction of a sample (the clock is the one event that is timed finer than a sample)
//   MidiStart, MidiContinue, MidiStop   transport bytes; value 0
//   MidiSpp     a Song Position Pointer in MIDI beats (one beat is a step) | value the beat count (0..16383)
//   Step        the song cursor entered a step (UI step change, ADR 0013 D6 events ring) | a pattern id | value step index | order
//               position in `b`/`c` as a 16-bit number (b low, c high), 0xFFFF in pattern mode | flags kEvCountIn for a step of the count-in (OS-METRO): the engine does not offer it to a take that waits
//               for a step, so a take armed before a count-in begins at the step after it
//   Pattern     the song cursor changed pattern | a pattern id | b/c order position as above | value the pattern's length in steps
//   SongEnd     the order list ran out (it then loops or stops: see Sequencer::set_song_loop)
//   Stopped     the transport stopped (a stop, or the end of the song with looping off)

#include <cstdint>
#include <type_traits>

namespace o3os::audio {

enum class EventKind : std::uint8_t {
  None = 0,
  // voices
  NoteOn = 1,
  NoteOff = 2,
  NoteCut = 3,
  NoteFade = 4,
  ModSet = 5,
  ModEnd = 6,
  FxReset = 7,
  Tick = 8,
  StripMute = 9,
  Metro = 10,
  TrkFx = 11,
  // MIDI out
  MidiNoteOn = 16,
  MidiNoteOff = 17,
  MidiCc = 18,
  MidiProgram = 19,
  MidiClock = 20,
  MidiStart = 21,
  MidiContinue = 22,
  MidiStop = 23,
  MidiSpp = 24,
  MidiPitchBend = 25,  // OS-INSTUI S8b
  MidiChanAt = 26,
  MidiPolyAt = 27,
  // engine to UI and app
  Step = 32,
  Pattern = 33,
  SongEnd = 34,
  Stopped = 35,
};

inline constexpr std::uint8_t kNoTrack = 0xFF;
inline constexpr std::uint8_t kVelocityDefault = 0xFF;   // NoteOn: the instrument's own velocity
inline constexpr std::uint8_t kChannelDefault = 0xFF;    // MidiCc, MidiProgram: the device's notes-out channel
inline constexpr std::uint8_t kMidiCcExplicit = 0x80;    // MidiCc.a: bit 7 set: the low 7 bits are the controller number (an instrument's own CC slot); otherwise the slot 0..5 of the device's table
inline constexpr std::uint8_t kRoutePorts = 0x03;        // MidiNoteOn .. MidiPolyAt `flags`: where it goes (0 the device's MIDI OUT port, bit 0 DIN, bit 1 USB)
inline constexpr std::int32_t kRouteDelayMaxUs = 100000; // `value` of those: the delay in microseconds (the instrument's OFFSET, at most 100 ms)

inline constexpr std::uint8_t kEvRetrigger = 1;  // NoteOn.flags: a roll repeat, not the first note of the step
inline constexpr std::uint8_t kEvCountIn = 1;    // Step.flags, Metro.flags (OS-METRO): the step is one of the count-in's (OS-COUNTIN: the play line waits on the start point, its value; nothing plays), before a recording starts

inline constexpr std::uint8_t kLayerFx1 = 0;   // ModSet.c: the first FX column of a step (the more significant layer)
inline constexpr std::uint8_t kLayerFx2 = 1;   // the second FX column ("sequencer 2")
inline constexpr std::uint8_t kLayerPerf = 2;  // the performance layer

// ModSet / ModEnd `flags` (the group) the voices act on: the step FX Slice (OS-007k). It is seq::FxGroup::Slice, which the audio layer may not include;
// audio_model_ids_test holds the two equal.
inline constexpr std::uint8_t kModGroupSlice = 10;
// The engine FX of the Amiga engine (OS-008c): seq::FxGroup::Engine0 / Engine1, the first and second FX column's FX type 128 + a MED command (ModSet.a is the type, .b the data
// byte). audio_model_ids_test holds the groups equal to the sequencer's.
inline constexpr std::uint8_t kModGroupEngine0 = 17;
inline constexpr std::uint8_t kModGroupEngine1 = 18;
inline constexpr std::uint8_t kFxEngineFirst = 128;
// The Cutoff FX (OS-007l): seq::FxGroup::Filter, fed by the step FX 27 Low-pass, 28 High-pass and 29 Band-pass (ModSet.a is the FX type, .b its value 0..100); the voices
// read the first and the number of types. audio_model_ids_test holds the group and the FX numbers equal to the sequencer's and the model's.
inline constexpr std::uint8_t kModGroupFilter = 5;
// The Filter LFO FX (OS-FU2): seq::FxGroup::LfoFilter, fed by the step FX 35 (ModSet.a is the type, .b the rate index 0..28); audio_model_ids_test holds the group equal to the sequencer's.
// The Slide FX (OS-PFX): seq::FxGroup::Slide, fed by the step FX 41 Slide up and 42 Slide down (ModSet.a is the type, .b the distance in sixteenths of a semitone, 0..255); the sampler voices
// slide their pitch (voices.h SLIDE FX). audio_model_ids_test holds the group and the FX numbers equal to the sequencer's.
inline constexpr std::uint8_t kModGroupSlide = 16;
inline constexpr std::uint8_t kFxSlideUp = 41;
inline constexpr std::uint8_t kFxSlideDown = 42;
// The Glide, Overdrive and Bit depth FX (OS-IFX1): seq::FxGroup::Glide / Overdrive / BitDepth, fed by the step FX 19, 38 and 39 (ModSet.a is the type, .b the value: 0..100, 0..100, 1..16);
// the sampler voices act on them (voices.h GLIDE, OVERDRIVE, BIT DEPTH). audio_model_ids_test holds the groups and the FX numbers equal to the sequencer's.
inline constexpr std::uint8_t kModGroupGlide = 4;
inline constexpr std::uint8_t kModGroupOverdrive = 19;
inline constexpr std::uint8_t kModGroupBitDepth = 20;
inline constexpr std::uint8_t kFxGlide = 19;
inline constexpr std::uint8_t kFxOverdrive = 38;
inline constexpr std::uint8_t kFxBitDepth = 39;
// The Glide FX's value 0..100 as the milliseconds a glide takes (value <= 50 over 0..1000 ms in steps of 20 ms, above it over 1000..15000 ms in steps of 280 ms).
constexpr std::uint32_t glide_fx_ms(unsigned value) noexcept {
  const unsigned v = value > 100u ? 100u : value;
  return v <= 50u ? v * 20u : 1000u + (v - 50u) * 280u;
}
// The tracker FX (OS-COREFX, ADR 0004 amendment 2): core FX 46 Arpeggio (xy: the note, then +x and +y semitones on the next two ticks, repeating), 47 Volume slide (xy: x up or else y down, 0..64 steps
// each tick from tick 1), 48 Fine volume slide (xy: once on tick 0), 49 / 50 Fine porta up / down (value 1/4 Paula period units, once on tick 0). They are Row FX of the sequencer, which sends a TrkFx event
// with each (a ModSet would hold it past its row); audio/tracker_fx.h runs them on the Tick events. audio_model_ids_test holds the numbers equal to the sequencer's.
inline constexpr std::uint8_t kFxTrkFirst = 46;
inline constexpr std::uint8_t kFxTrkArp = 46;
inline constexpr std::uint8_t kFxTrkVolSlide = 47;
inline constexpr std::uint8_t kFxTrkFineVol = 48;
inline constexpr std::uint8_t kFxTrkFinePortaUp = 49;
inline constexpr std::uint8_t kFxTrkFinePortaDown = 50;
// CF2: 51 Pan slide (xy: y right else x left, 4/256 of the range a tick from tick 1; xF / Fy fine, once on tick 0), 52 / 53 Vibrato / Tremolo waveform (0..7, the tracker's: set at the row and kept),
// 54 Glissando (0 off), 55 Retrigger (xy: x the volume change of Scream Tracker's Q, y the interval in ticks), 56 Tremor (xy: x + 1 ticks on, y + 1 off).
inline constexpr std::uint8_t kFxTrkPanSlide = 51;
inline constexpr std::uint8_t kFxTrkVibWave = 52;
inline constexpr std::uint8_t kFxTrkTremWave = 53;
inline constexpr std::uint8_t kFxTrkGliss = 54;
inline constexpr std::uint8_t kFxTrkRetrig = 55;
inline constexpr std::uint8_t kFxTrkTremor = 56;
// 57 Channel pan (0..100): the track's absolute pan, held until another one or the transport start (not row-scoped: a TrkFx of type 0 after it ends nothing).
inline constexpr std::uint8_t kFxTrkChanPan = 57;
// CF3: 58 Channel volume (0..64) and 60 Global volume (0..64) are held like 57 (a TrkFx of type 0 after them ends nothing); 59 and 61 are the slides, 62 Panbrello, row-scoped.
inline constexpr std::uint8_t kFxTrkChanVol = 58;
inline constexpr std::uint8_t kFxTrkChanVolSlide = 59;
inline constexpr std::uint8_t kFxTrkGlobalVol = 60;
inline constexpr std::uint8_t kFxTrkGlobalVolSlide = 61;
inline constexpr std::uint8_t kFxTrkPanbrello = 62;
inline constexpr std::uint8_t kFxTrkLast = 62;
// The Panning FX (31) as a ModSet / ModEnd group: seq::FxGroup::Panning. The tracker processor takes both (audio/tracker_fx.h): the value 0..100 is the track's pan while the FX is held
// (at once and at each note) and the ModEnd gives the instrument's pan back.
inline constexpr std::uint8_t kModGroupPanning = 2;
inline constexpr std::uint8_t kFxPanning = 31;
// The Volume group (OS-PERFFX P2): seq::FxGroup::Volume. Only the performance layer's VOLUME acts on it (the tracker processor hands the value to the strip: Graph::set_perf_gain); the step FX of the
// group (Velocity, Random velocity) are the sequencer's, at the note. audio_model_ids_test holds the group equal to the sequencer's.
inline constexpr std::uint8_t kModGroupVolume = 1;
// The Delay send and Reverb send groups (OS-PERFFX P3): seq::FxGroup::DelaySend / ReverbSend, fed by the step FX 30 and 32 (ModSet.a is the type, .b the send 0..100) and by the performance layer's
// DELAY and REVERB (ModSet.value, -100..100). The tracker processor hands both to the strip (Graph::set_step_send, set_perf_send). audio_model_ids_test holds the groups and the FX numbers equal to the sequencer's.
inline constexpr std::uint8_t kModGroupDelaySend = 6;
inline constexpr std::uint8_t kModGroupReverbSend = 7;
inline constexpr std::uint8_t kFxDelaySend = 30;
inline constexpr std::uint8_t kFxReverbSend = 32;
// The Tune group (OS-PERFFX P5): seq::FxGroup::Tune. No step FX has it; the performance layer's TUNE (ModSet.value, -48..48 semitones) is the only source, and the sampler voices add it to every
// pitch offset they make (voices.h PERFORMANCE LAYER, TUNE). audio_model_ids_test holds the group equal to the sequencer's.
inline constexpr std::uint8_t kModGroupTune = 21;
// The Position and Sample end groups (OS-PERFFX P7): seq::FxGroup::Position / SampleEnd. Only the performance layer's SMP POS and SMP END (ModSet.value, -100..100 percent) act on them: the sampler voices read
// them at a note's start (voices.h PERFORMANCE LAYER, SMP POS AND SMP END); the step FX 22 of the Position group has no consumer. audio_model_ids_test holds the groups equal to the sequencer's.
inline constexpr std::uint8_t kModGroupPosition = 9;
inline constexpr std::uint8_t kModGroupSampleEnd = 22;
// The Reverse group (OS-PERF-REVERSE, P8): seq::FxGroup::Reverse, the step FX 26 (ModSet.a == kFxReverse, b 0 forward / 1 backward) and the performance layer's PLAYBACK (ModSet.value 0 / 1). The sampler
// voices read both at a note's start (voices.h PERFORMANCE LAYER, PLAYBACK). audio_model_ids_test holds the group and the FX number equal to the sequencer's.
inline constexpr std::uint8_t kModGroupReverse = 8;
inline constexpr std::uint8_t kFxReverse = 26;
inline constexpr std::uint8_t kModGroupLfoFilter = 14;
// The LFO groups (OS-PERF-LFO, docs/specs/OS-PERFFX.md section 11): seq::FxGroup::LfoVolume / LfoPanning / LfoFinetune / LfoFilter / LfoPosition, 11..15 in the order of the Perform rows VOL, PAN, TUNE, FLT and POS
// LFO. Only the performance layer's value (ModSet.value, a signed offset on the LFO speed index, -28..28) acts on 11, 12, 13 and 15; the step FX 23, 24, 33 and 36 (kFxLfo*, below) act since OS-PERF-LFO-STEP. The group 14 also
// has the Filter LFO FX 35 (above). audio_model_ids_test holds the groups equal to the sequencer's.
inline constexpr std::uint8_t kModGroupLfoVolume = 11;
inline constexpr std::uint8_t kModGroupLfoPanning = 12;
inline constexpr std::uint8_t kModGroupLfoFinetune = 13;
inline constexpr std::uint8_t kModGroupLfoPosition = 15;
inline constexpr std::uint8_t kFxFilterLfo = 35;  // ModSet.a of that group when the sequencer's FX holds it (0 when only the performance layer's slot does)
// OS-PERF-LFO-STEP: the step FX of the other four LFO groups (ModSet.a, ModSet.b the rate index): Volume LFO (0..23), Panning LFO, Finetune LFO and Position LFO (0..28). They act since this item; audio_model_ids_test holds the numbers.
inline constexpr std::uint8_t kFxLfoVolume = 23;
inline constexpr std::uint8_t kFxLfoPanning = 24;
inline constexpr std::uint8_t kFxLfoFinetune = 33;
inline constexpr std::uint8_t kFxLfoPosition = 36;
inline constexpr std::uint8_t kFxFilterFirst = 27;
inline constexpr std::uint8_t kFxFilterCount = 3;

// The performance layer's value for the Filter group (OS-011b). The control table carries one int16 per group; the performance filter is a TYPE (the slot's FX: low-pass 1,
// high-pass 2, band-pass 3: the same three FX as the Cutoff FX, in the performance layer's own slots) and a cutoff OFFSET -100..100.
// The type goes in the high byte (1..3), the offset in the low byte as two's complement, so a value is never 0 and an offset of 0 is a held filter, not "no filter".
// `type` is kFilterLow / kFilterHigh / kFilterBand (voices.h: 0, 1, 2).
constexpr std::int16_t perf_filter_value(unsigned type, int offset) noexcept {
  const int o = offset < -100 ? -100 : (offset > 100 ? 100 : offset);
  return static_cast<std::int16_t>(((type > 2u ? 3u : type + 1u) << 8) | (static_cast<unsigned>(o) & 0xFFu));
}
constexpr unsigned perf_filter_type(std::int32_t value) noexcept {
  const std::int32_t t = (value >> 8) & 3;
  return t >= 1 ? static_cast<unsigned>(t - 1) : 0u;
}
constexpr int perf_filter_offset(std::int32_t value) noexcept {
  const int o = static_cast<std::int8_t>(value & 0xFF);
  return o < -100 ? -100 : (o > 100 ? 100 : o);
}

struct SeqEvent {
  std::uint16_t offset;
  EventKind kind;
  std::uint8_t track;
  std::uint8_t a;
  std::uint8_t b;
  std::uint8_t c;
  std::uint8_t flags;
  std::int32_t value;
};
static_assert(sizeof(SeqEvent) == 12 && alignof(SeqEvent) == 4, "SeqEvent is three words");
static_assert(std::is_trivially_copyable<SeqEvent>::value, "SeqEvent is copied bytewise");

// The most events one step() can return: the sequencer's own bound, from which a caller sizes its array. A block that would make more
// carries the rest over to the next block (at offset 0, counted as late), it never drops one.
inline constexpr std::uint32_t kMaxEventsPerBlock = 192;

// The order position of a Step or Pattern event, from its b and c fields.
constexpr std::uint16_t event_order_pos(const SeqEvent& e) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint32_t>(e.b) | (static_cast<std::uint32_t>(e.c) << 8));
}

}  // namespace o3os::audio
