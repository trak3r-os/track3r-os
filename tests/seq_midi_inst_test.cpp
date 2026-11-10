// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S8b + S9: what the sequencer makes of the stored fields of a MIDI instrument (INST 1.3, model/instrument.h): VELOCITY for a step that names none, PROGRAM before the first note (and
// again after a project load, and when another instrument changed the channel), OUT and OFFSET as the route of every event of the instrument (the note-off follows the note-on's route), the
// Micro-tune FX as a pitch bend (PB RANGE), and CC A..F as controller, Program Change, channel and polyphonic aftertouch. The events are the sequencer's (audio/events.h); the bytes the
// router makes of them are midi_router_test.cpp's and inst_midi_fields_test.cpp's. Host only (the collector is 144 KB).
#include <doctest/doctest.h>

#include <o3os/seq/midi_route.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace fx = o3os::seq::fx;

namespace {

StaticSlot<seq::Sequencer>& g_slot = seq_slot();
Collector& g_col = seq_collector();

constexpr std::uint8_t kMidi = 11;    // the step's instrument number of the MIDI instrument in slot 10 (channel 3: wire 2)
constexpr std::uint8_t kMidi2 = 12;   // slot 11: a second MIDI instrument, channel 3 as well

seq::Sequencer& make() {
  seq::Sequencer& s = g_slot.make(seq_project());
  g_col.n = 0;
  g_col.total_events = 0;
  g_col.hash = 0xCBF29CE484222325ull;
  return s;
}

// One 16-step pattern, id 1, 120 BPM; slot 10 (channel 3) and slot 11 (channel 3) are MIDI instruments.
void project16() {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  model::Instrument m;
  model::init_instrument(m, model::InstrumentKind::Midi);
  m.midi_channel = 3;
  CHECK(p.set_instrument(11, m) == model::Status::Ok);
}

void edit_midi(unsigned slot, void (*fn)(model::Instrument&)) {
  model::Instrument in = *seq_project().instrument(slot);
  fn(in);
  CHECK(seq_project().set_instrument(slot, in) == model::Status::Ok);
}

constexpr std::uint64_t kStep = 5515;  // a step at 120 BPM: 48 ticks of 1/384 s, in samples of 44,117.6 Hz (rounded: the tests look at which step an event is in)

// The records of one kind in time order.
std::uint32_t count(EventKind k) { return g_col.count(k); }

// The index of the first record of `k` at or after `from`, or g_col.n.
std::uint32_t find_from(EventKind k, std::uint32_t from = 0) {
  for (std::uint32_t i = from; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == k) {
      return i;
    }
  }
  return g_col.n;
}

void run_steps(seq::Sequencer& s, std::uint32_t steps) {
  s.play_pattern(1);
  g_col.run(s, 0, static_cast<std::uint64_t>(steps) * kStep + 300u, 64);
}

}  // namespace

// ---- the pure rules --------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi route: the instrument's channel, port and offset; the bend's conversion; a CC slot's target") {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = 16;
  in.midi_out = model::kMidiOutBoth;
  in.midi_offset = 100;
  seq::MidiRoute r = seq::midi_route(in);
  CHECK(r.ch == 15);
  CHECK(r.ports == 3);
  CHECK(r.delay_ms == 100);
  CHECK(seq::route_delay_us(r) == 100000);
  in.midi_channel = 1;
  in.midi_out = model::kMidiOutConfig;
  in.midi_offset = 0;
  r = seq::midi_route(in);
  CHECK((r.ch == 0 && r.ports == 0 && r.delay_ms == 0));
  in.midi_offset = 200;  // out of range (a damaged field): held to the bound
  CHECK(seq::midi_route(in).delay_ms == 100);
  in.midi_velocity = 0;
  CHECK(seq::midi_velocity_of(in) == 127);
  in.midi_velocity = 64;
  CHECK(seq::midi_velocity_of(in) == 64);

  // Micro-tune FX 0..198, 99 in tune: cents -99..+99
  CHECK(seq::microtune_cents(99) == 0);
  CHECK(seq::microtune_cents(0) == -99);
  CHECK(seq::microtune_cents(198) == 99);
  CHECK(seq::microtune_cents(255) == 99);
  // the bend: round(cents x 8192 / (100 x range)), 8192 the centre, held to 0..16383
  CHECK(seq::pitch_bend_value(0, 2) == 8192);
  CHECK(seq::pitch_bend_value(99, 2) == 12247);
  CHECK(seq::pitch_bend_value(-99, 2) == 4137);
  CHECK(seq::pitch_bend_value(50, 12) == 8533);
  CHECK(seq::pitch_bend_value(-50, 12) == 7851);
  CHECK(seq::pitch_bend_value(99, 1) == 16302);  // 99 cents of a one-semitone range: 8110 up
  CHECK(seq::pitch_bend_value(100, 1) == 16384 - 1);  // (cents are held to 99 by the FX; the function holds the travel)
  CHECK(seq::pitch_bend_value(-100, 1) == 0);
  CHECK(seq::pitch_bend_value(99, 24) == 8192 + 338);
  CHECK(seq::pitch_bend_value(99, 0) == 8192);  // range 0: no bend
  // the CC slot targets
  CHECK(seq::cc_target(0).kind == seq::CcKind::Config);
  CHECK((seq::cc_target(1).kind == seq::CcKind::Controller && seq::cc_target(1).number == 0));
  CHECK((seq::cc_target(75).kind == seq::CcKind::Controller && seq::cc_target(75).number == 74));
  CHECK((seq::cc_target(128).kind == seq::CcKind::Controller && seq::cc_target(128).number == 127));
  CHECK(seq::cc_target(129).kind == seq::CcKind::Program);
  CHECK(seq::cc_target(130).kind == seq::CcKind::ChannelAt);
  CHECK(seq::cc_target(131).kind == seq::CcKind::PolyAt);
  CHECK(seq::cc_target(132).kind == seq::CcKind::Config);  // damaged: the default
  CHECK(seq::cc_target(255).kind == seq::CcKind::Config);
}

// ---- VELOCITY ----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi inst: VELOCITY is the velocity of a step that names none; a step's own volume wins") {
  project16();
  edit_midi(10, [](model::Instrument& i) { i.midi_velocity = 90; });
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(62, kMidi, 0, 0, 0, 0, 50)) == model::Status::Ok);
  seq::Sequencer& s = make();
  run_steps(s, 3);
  NEED(count(EventKind::MidiNoteOn) >= 2u);
  CHECK(g_col.nth(EventKind::MidiNoteOn, 0)->ev.b == 90);
  CHECK(g_col.nth(EventKind::MidiNoteOn, 1)->ev.b == 50);
}

// ---- PROGRAM -----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi inst: PROGRAM is sent before the first note, once, with the note's route") {
  project16();
  edit_midi(10, [](model::Instrument& i) {
    i.midi_program = 6;  // program 5
    i.midi_out = model::kMidiOutUsb;
  });
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(62, kMidi)) == model::Status::Ok);
  seq::Sequencer& s = make();
  run_steps(s, 3);
  NEED(count(EventKind::MidiProgram) == 1u);
  const Rec* pc = g_col.nth(EventKind::MidiProgram, 0);
  CHECK(pc->ev.a == 5);
  CHECK(pc->ev.c == 2);
  CHECK(pc->ev.flags == 2);  // USB
  const Rec* on = g_col.nth(EventKind::MidiNoteOn, 0);
  NEED(on != nullptr);
  CHECK(pc->t == on->t);
  CHECK(find_from(EventKind::MidiProgram) < find_from(EventKind::MidiNoteOn));  // the program change comes first at the same sample
  CHECK(count(EventKind::MidiNoteOn) >= 2u);
}

TEST_CASE("midi inst: PROGRAM is sent again for the first note after a project load, not after a stop") {
  project16();
  edit_midi(10, [](model::Instrument& i) { i.midi_program = 4; });
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi)) == model::Status::Ok);
  seq::Sequencer& s = make();
  run_steps(s, 2);
  CHECK(count(EventKind::MidiProgram) == 1u);
  // stop and play again: the receiver still has the program, nothing is sent
  s.stop();
  g_col.run(s, 20000, 20400, 64);
  const std::uint32_t before = count(EventKind::MidiProgram);
  s.play_pattern(1);
  g_col.run(s, 20400, 40000, 64);
  CHECK(count(EventKind::MidiProgram) == before);
  CHECK(count(EventKind::MidiNoteOn) >= 2u);
  // a project load (reset() and the same project again): the first note puts the receiver on the program
  s.stop();
  g_col.run(s, 40000, 40400, 64);
  project16();
  edit_midi(10, [](model::Instrument& i) { i.midi_program = 4; });
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidi)) == model::Status::Ok);
  s.play_pattern(1);
  g_col.run(s, 40400, 60000, 64);
  CHECK(count(EventKind::MidiProgram) == before + 1u);
}

TEST_CASE("midi inst: two instruments on one channel with different programs put the receiver on each program as the track moves between them; one without a program sends none") {
  project16();
  edit_midi(10, [](model::Instrument& i) { i.midi_program = 3; });
  edit_midi(11, [](model::Instrument& i) { i.midi_program = 8; });
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(61, kMidi2)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 2, make_step(62, kMidi2)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 3, make_step(63, kMidi)) == model::Status::Ok);
  seq::Sequencer& s = make();
  run_steps(s, 5);
  NEED(count(EventKind::MidiProgram) == 3u);
  CHECK(g_col.nth(EventKind::MidiProgram, 0)->ev.a == 2);
  CHECK(g_col.nth(EventKind::MidiProgram, 1)->ev.a == 7);
  CHECK(g_col.nth(EventKind::MidiProgram, 2)->ev.a == 2);
  // a MIDI instrument with PROGRAM none sends nothing and leaves the channel's program as it is
  project16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidi)) == model::Status::Ok);
  seq::Sequencer& s2 = make();
  run_steps(s2, 2);
  CHECK(count(EventKind::MidiProgram) == 0u);
}

TEST_CASE("midi inst: an edit of PROGRAM (the ParamChange seq_param::MidiProgram) sends it at the next block, on the channel and the route; none sends nothing") {
  project16();
  edit_midi(10, [](model::Instrument& i) {
    i.midi_program = 10;
    i.midi_out = model::kMidiOutDin;
    i.midi_offset = 12;
  });
  seq::Sequencer& s = make();
  g_col.run(s, 0, 640, 64);
  CHECK(count(EventKind::MidiProgram) == 0u);
  const core::ParamChange pc{core::param_id(core::ParamKind::Project, 0, seq::seq_param::MidiProgram), 10.0f, 0u};
  CHECK(s.apply(pc));
  g_col.run(s, 640, 1280, 64);
  NEED(count(EventKind::MidiProgram) == 1u);
  const Rec* r = g_col.nth(EventKind::MidiProgram, 0);
  CHECK(r->t == 640);
  CHECK(r->ev.a == 9);
  CHECK(r->ev.c == 2);
  CHECK(r->ev.flags == 1);
  CHECK(r->ev.value == 12000);
  // the same value again is still a request
  CHECK(s.apply(pc));
  g_col.run(s, 1280, 1920, 64);
  CHECK(count(EventKind::MidiProgram) == 2u);
  // none: nothing is sent; a slot that is not a MIDI instrument: nothing
  edit_midi(10, [](model::Instrument& i) { i.midi_program = 0; });
  CHECK(s.apply(pc));
  CHECK(s.apply(core::ParamChange{pc.id, 0.0f, 0u}));  // a sample instrument
  CHECK(s.apply(core::ParamChange{pc.id, 99.0f, 0u}));  // past the slots: ignored, still the sequencer's id
  g_col.run(s, 1920, 2560, 64);
  CHECK(count(EventKind::MidiProgram) == 2u);
}

// ---- OUT and OFFSET -----------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi inst: OUT and OFFSET label every event of the instrument; the note-off keeps the route of its note-on") {
  project16();
  edit_midi(10, [](model::Instrument& i) {
    i.midi_out = model::kMidiOutUsb;
    i.midi_offset = 40;
  });
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 2, special_step(model::kNoteOff, kMidi)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, kStep + 1000, 64);  // inside step 1: the note is on
  NEED(count(EventKind::MidiNoteOn) == 1u);
  const Rec* on = g_col.nth(EventKind::MidiNoteOn, 0);
  CHECK(on->ev.flags == 2);
  CHECK(on->ev.value == 40000);
  // the instrument is edited under the sounding note: another port, another channel, no offset
  edit_midi(10, [](model::Instrument& i) {
    i.midi_out = model::kMidiOutDin;
    i.midi_offset = 0;
    i.midi_channel = 9;
  });
  g_col.run(s, kStep + 1000, 3 * kStep + 400, 64);
  NEED(count(EventKind::MidiNoteOff) >= 1u);
  const Rec* off = g_col.nth(EventKind::MidiNoteOff, 0);
  CHECK(off->ev.c == 2);       // the channel of the note-on
  CHECK(off->ev.flags == 2);   // its port
  CHECK(off->ev.value == 40000);  // and its delay
  CHECK(off->ev.a == 60);
}

// ---- the Micro-tune FX as a pitch bend ---------------------------------------------------------------------------------------------------

TEST_CASE("midi inst: the Micro-tune FX is a pitch bend before the note (PB RANGE), the centre again for a note without it, and a stop centres a bent channel") {
  project16();
  edit_midi(10, [](model::Instrument& i) { i.midi_pb_range = 2; });
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi, fx::kMicroTune, 198)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(62, kMidi, fx::kMicroTune, 99)) == model::Status::Ok);  // in tune: the centre, already there? no: the first bent it
  CHECK(p.set_step(1, 0, 2, make_step(64, kMidi)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 3, make_step(65, kMidi, fx::kMicroTune, 0)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, 4 * kStep + 200, 64);
  NEED(count(EventKind::MidiPitchBend) >= 3u);
  const auto bend = [](std::uint32_t n) {
    const audio::SeqEvent& e = g_col.nth(EventKind::MidiPitchBend, n)->ev;
    return static_cast<unsigned>(e.a) | (static_cast<unsigned>(e.b) << 7);
  };
  CHECK(bend(0) == 12247);  // +99 cents
  CHECK(bend(1) == 8192);   // the next note (value 99, in tune) brings the centre back
  CHECK(bend(2) == 4137);   // step 3: -99 cents
  CHECK(g_col.nth(EventKind::MidiPitchBend, 0)->ev.c == 2);
  // the order at a bent note: bend, then note
  const std::uint32_t b0 = find_from(EventKind::MidiPitchBend);
  const std::uint32_t n0 = find_from(EventKind::MidiNoteOn);
  CHECK(b0 < n0);
  CHECK(g_col.rec[b0].t == g_col.rec[n0].t);
  // step 2 (no FX) sent no bend: the channel was at the centre already: exactly three bends in four notes
  CHECK(count(EventKind::MidiPitchBend) == 3u);
  // a stop with the channel bent: the centre goes out with the note-off
  const std::uint32_t before = count(EventKind::MidiPitchBend);
  s.stop();
  g_col.run(s, 4 * kStep + 200, 4 * kStep + 600, 64);
  CHECK(count(EventKind::MidiPitchBend) == before + 1u);
  CHECK(bend(before) == 8192);
  CHECK(find_from(EventKind::MidiNoteOff, 0) < g_col.n);
}

TEST_CASE("midi inst: a MIDI instrument that was never edited bends by a range of 2 semitones: the Micro-tune FX at +-99 cents is +-0.99 of a 100-cent travel of 2 semitones") {
  project16();
  CHECK(seq_project().instrument(11)->midi_pb_range == 2);  // the default (model::kMidiPbRangeDefault)
  CHECK(model::kMidiPbRangeDefault == 2);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi2, fx::kMicroTune, 198)) == model::Status::Ok);  // +99 cents
  CHECK(p.set_step(1, 0, 1, make_step(62, kMidi2, fx::kMicroTune, 0)) == model::Status::Ok);    // -99 cents
  seq::Sequencer& s = make();
  run_steps(s, 3);
  NEED(count(EventKind::MidiPitchBend) >= 2u);
  const auto bend = [](std::uint32_t n) {
    const audio::SeqEvent& e = g_col.nth(EventKind::MidiPitchBend, n)->ev;
    return static_cast<unsigned>(e.a) | (static_cast<unsigned>(e.b) << 7);
  };
  CHECK(bend(0) == 8192u + 4055u);  // round(99 x 8192 / 200) = 4,055 up: 12,247
  CHECK(bend(1) == 8192u - 4055u);  // 4,137
}

TEST_CASE("midi inst: two instruments on one channel bend different ports: the stop centres every port a note bent, a note without the FX centres only its own") {
  project16();
  edit_midi(10, [](model::Instrument& i) { i.midi_pb_range = 2; i.midi_out = 1; });  // DIN
  edit_midi(11, [](model::Instrument& i) { i.midi_pb_range = 2; i.midi_out = 2; });  // USB, the same channel
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi, fx::kMicroTune, 198)) == model::Status::Ok);   // +99 cents on DIN
  CHECK(p.set_step(1, 0, 1, make_step(62, kMidi2, fx::kMicroTune, 198)) == model::Status::Ok);  // the same value on USB: its receiver is not bent yet, so it is sent
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, 2 * kStep + 200, 64);
  const auto flags_of = [](std::uint32_t n) { return static_cast<unsigned>(g_col.nth(EventKind::MidiPitchBend, n)->ev.flags); };
  NEED(count(EventKind::MidiPitchBend) == 2u);
  CHECK(flags_of(0) == 1u);
  CHECK(flags_of(1) == 2u);
  s.stop();
  g_col.run(s, 2 * kStep + 200, 2 * kStep + 600, 64);
  // the stop centres DIN and USB, one each (the last note's route alone would leave DIN bent)
  NEED(count(EventKind::MidiPitchBend) == 4u);
  unsigned seen = 0;
  for (std::uint32_t n = 2; n < 4u; ++n) {
    const audio::SeqEvent& e = g_col.nth(EventKind::MidiPitchBend, n)->ev;
    CHECK((static_cast<unsigned>(e.a) | (static_cast<unsigned>(e.b) << 7)) == 8192u);
    CHECK(e.c == 2);  // the channel (wire 2)
    seen |= 1u << e.flags;
  }
  CHECK(seen == ((1u << 1) | (1u << 2)));
}

TEST_CASE("midi inst: a note without the FX centres the port it bent and no other; the stop then centres only the port still bent") {
  project16();
  edit_midi(10, [](model::Instrument& i) { i.midi_pb_range = 2; i.midi_out = 1; });  // DIN
  edit_midi(11, [](model::Instrument& i) { i.midi_pb_range = 2; i.midi_out = 2; });  // USB
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi, fx::kMicroTune, 198)) == model::Status::Ok);   // DIN bent
  CHECK(p.set_step(1, 0, 1, make_step(62, kMidi2, fx::kMicroTune, 0)) == model::Status::Ok);    // USB bent the other way
  CHECK(p.set_step(1, 0, 2, make_step(64, kMidi)) == model::Status::Ok);                        // DIN without the FX: the centre on DIN only
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, 3 * kStep + 200, 64);
  NEED(count(EventKind::MidiPitchBend) == 3u);
  CHECK(g_col.nth(EventKind::MidiPitchBend, 2)->ev.flags == 1u);
  CHECK((static_cast<unsigned>(g_col.nth(EventKind::MidiPitchBend, 2)->ev.a) | (static_cast<unsigned>(g_col.nth(EventKind::MidiPitchBend, 2)->ev.b) << 7)) == 8192u);
  s.stop();
  g_col.run(s, 3 * kStep + 200, 3 * kStep + 600, 64);
  NEED(count(EventKind::MidiPitchBend) == 4u);
  CHECK(g_col.nth(EventKind::MidiPitchBend, 3)->ev.flags == 2u);  // USB is still bent: it alone is centred
}

// ---- a full queue records nothing as sent ----------------------------------------------------------------------------------------------------

namespace {
// Sixteen tracks: track 0 plays the MIDI instrument (slot 10, channel 3) at `row` with `fx0` / `v0`; tracks 1..15 each start a tracker effect on every one of the first 20 rows (the 256-entry queue
// fills up in the first 16 and nothing takes the events away while step() is given no room: the technique of seq_corefx_test.cpp).
void flood_project(std::uint32_t row, std::uint8_t fx0, std::uint8_t v0, std::uint8_t program) {
  fresh_project(16, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 32) == model::Status::Ok);
  edit_midi(10, [](model::Instrument&) {});
  model::Instrument m = *p.instrument(10);
  m.midi_program = program;
  CHECK(p.set_instrument(10, m) == model::Status::Ok);
  for (std::uint32_t t = 1; t < 16; ++t) {
    for (std::uint32_t r = 0; r < 20; ++r) {
      CHECK(p.set_step(1, t, r, fx_step(fx::kTrkArp, 0x37)) == model::Status::Ok);
    }
  }
  CHECK(p.set_step(1, 0, row, make_step(60, kMidi, fx0, v0)) == model::Status::Ok);
}
std::uint64_t row_at(std::uint32_t n) { return static_cast<std::uint64_t>(n) * kStep; }
}  // namespace

TEST_CASE("midi inst: a program that found no room in the full queue is not recorded as sent: the next note sends it") {
  flood_project(18, 0, 0, 6);  // the note is on row 18, when the queue is full
  seq::Sequencer& s = make();
  s.play_pattern(1);
  audio::SeqEvent none[1];
  (void)s.step(0, static_cast<std::uint32_t>(row_at(20)), none, 0);
  CHECK(s.stats().queue_overflow > 0u);
  s.stop();
  g_col.run(s, row_at(20), row_at(20) + 600, 64);  // the events are taken away
  NEED(count(EventKind::MidiProgram) == 0u);       // the program of row 18 never got into the queue ...
  NEED(count(EventKind::MidiNoteOn) == 0u);
  // ... so the receiver was not told: the same note on an idle queue sends it
  for (std::uint32_t t = 1; t < 16; ++t) {
    for (std::uint32_t r = 0; r < 20; ++r) {
      CHECK(seq_project().set_step(1, t, r, fx_step(0, 0)) == model::Status::Ok);
    }
  }
  CHECK(seq_project().set_step(1, 0, 18, fx_step(0, 0)) == model::Status::Ok);
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidi)) == model::Status::Ok);
  g_col.n = 0;
  g_col.total_events = 0;
  s.play_pattern(1);
  g_col.run(s, row_at(21), row_at(21) + row_at(2), 64);
  NEED(count(EventKind::MidiProgram) == 1u);
  CHECK(g_col.nth(EventKind::MidiProgram, 0)->ev.a == 5);  // program 6 is the wire's 5
  CHECK(find_from(EventKind::MidiProgram) < find_from(EventKind::MidiNoteOn));
}

TEST_CASE("midi inst: a centre that found no room in the full queue leaves the channel recorded as bent: the next stop sends it") {
  flood_project(0, fx::kMicroTune, 198, 0);  // +99 cents on row 0, while the queue still has room
  seq::Sequencer& s = make();
  s.play_pattern(1);
  audio::SeqEvent none[1];
  (void)s.step(0, static_cast<std::uint32_t>(row_at(20)), none, 0);
  const std::uint64_t overflow = s.stats().queue_overflow;
  CHECK(overflow > 0u);
  s.stop();  // the queue is full: the centre cannot be queued
  (void)s.step(row_at(20), 64, none, 0);
  CHECK(s.stats().queue_overflow > overflow);
  g_col.run(s, row_at(20) + 64, row_at(20) + 600, 64);
  const auto centres = [] {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < count(EventKind::MidiPitchBend); ++i) {
      const audio::SeqEvent& e = g_col.nth(EventKind::MidiPitchBend, i)->ev;
      n += (e.a == 0 && e.b == 0x40) ? 1u : 0u;
    }
    return n;
  };
  NEED(count(EventKind::MidiPitchBend) == 1u);  // the +99 of row 0 only
  CHECK(centres() == 0u);
  // a run with no note at all (so nothing of it sends a centre by itself): its stop still owes the centre of the first run
  for (std::uint32_t t = 1; t < 16; ++t) {
    for (std::uint32_t r = 0; r < 20; ++r) {
      CHECK(seq_project().set_step(1, t, r, fx_step(0, 0)) == model::Status::Ok);
    }
  }
  CHECK(seq_project().set_step(1, 0, 0, fx_step(0, 0)) == model::Status::Ok);
  g_col.n = 0;
  g_col.total_events = 0;
  s.play_pattern(1);
  g_col.run(s, row_at(21), row_at(22), 64);
  CHECK(count(EventKind::MidiPitchBend) == 0u);  // (nothing was bent or centred by the run itself)
  s.stop();
  g_col.run(s, row_at(22), row_at(22) + 600, 64);
  NEED(count(EventKind::MidiPitchBend) == 1u);
  CHECK(centres() == 1u);
}

TEST_CASE("midi inst: PB RANGE 0 sends no bend, 24 spans the range, and a sample instrument's Micro-tune is not a bend") {
  project16();
  edit_midi(10, [](model::Instrument& i) { i.midi_pb_range = 0; });
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi, fx::kMicroTune, 198)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(61, 1, fx::kMicroTune, 198)) == model::Status::Ok);  // a sample instrument
  seq::Sequencer& s = make();
  run_steps(s, 3);
  CHECK(count(EventKind::MidiPitchBend) == 0u);
  project16();
  edit_midi(10, [](model::Instrument& i) { i.midi_pb_range = 24; });
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidi, fx::kMicroTune, 198)) == model::Status::Ok);
  seq::Sequencer& s2 = make();
  run_steps(s2, 2);
  NEED(count(EventKind::MidiPitchBend) >= 1u);
  const audio::SeqEvent& e = g_col.nth(EventKind::MidiPitchBend, 0)->ev;
  CHECK((static_cast<unsigned>(e.a) | (static_cast<unsigned>(e.b) << 7)) == 8192u + 338u);
}

// ---- CC A..F ---------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi inst: CC A..F go to the controller set on the instrument, as Program Change, channel or polyphonic aftertouch; the default is the Config page's number") {
  project16();
  edit_midi(10, [](model::Instrument& i) {
    i.midi_out = model::kMidiOutBoth;
    i.midi_offset = 5;
    i.midi_cc[0] = 75;                     // CC A: controller 74
    i.midi_cc[1] = model::kMidiCcProg;     // CC B: Program Change
    i.midi_cc[2] = model::kMidiCcChanAt;   // CC C: channel aftertouch
    i.midi_cc[3] = model::kMidiCcPolyAt;   // CC D: polyphonic aftertouch
    i.midi_cc[4] = model::kMidiCcConfig;   // CC E: the Config page's
    i.midi_cc[5] = 11;                     // CC F (FX 37): controller 10
  });
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi, fx::kCcA, 100, fx::kCcA + 1, 7)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, fx_step(fx::kCcA + 2, 33, fx::kCcA + 3, 44)) == model::Status::Ok);  // no note: the sounding one
  CHECK(p.set_step(1, 0, 2, fx_step(fx::kCcE, 55, fx::kSlot37, 66)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 3, special_step(model::kNoteOff, kMidi)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 4, fx_step(fx::kCcA + 3, 77)) == model::Status::Ok);  // polyphonic aftertouch with no note sounding: ignored
  seq::Sequencer& s = make();
  run_steps(s, 6);
  // CC A
  std::uint32_t found = 0;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    const audio::SeqEvent& e = g_col.rec[i].ev;
    if (e.kind == EventKind::MidiCc && e.b == 100) {
      CHECK(e.a == (audio::kMidiCcExplicit | 74));
      CHECK(e.c == 2);
      CHECK(e.flags == 3);
      CHECK(e.value == 5000);
      ++found;
    }
    if (e.kind == EventKind::MidiProgram && e.a == 7) {  // CC B: PROG, the FX value is the program
      CHECK(e.c == 2);
      CHECK(e.flags == 3);
      ++found;
    }
    if (e.kind == EventKind::MidiChanAt) {
      CHECK(e.a == 33);
      CHECK(e.c == 2);
      ++found;
    }
    if (e.kind == EventKind::MidiPolyAt) {
      CHECK(e.a == 60);  // the note the track sounds
      CHECK(e.b == 44);
      CHECK(e.c == 2);
      ++found;
    }
    if (e.kind == EventKind::MidiCc && e.b == 55) {  // CC E: the device's table, on the device's channel
      CHECK(e.a == 4);
      CHECK(e.c == audio::kChannelDefault);
      CHECK(e.flags == 3);  // (the instrument's port and delay still apply)
      ++found;
    }
    if (e.kind == EventKind::MidiCc && e.b == 66) {  // CC F, FX 37: controller 10
      CHECK(e.a == (audio::kMidiCcExplicit | 10));
      ++found;
    }
  }
  CHECK(found == 6u);
  CHECK(count(EventKind::MidiPolyAt) == 1u);  // the one with no note sounding was dropped
  CHECK(s.stats().fx_ignored >= 1u);
}

TEST_CASE("midi inst: a sample instrument's CC FX, and a MIDI instrument's default slots, are what they were: the Config page's number, the device's channel") {
  project16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kCcA, 20)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(61, kMidi, fx::kCcA + 1, 21)) == model::Status::Ok);
  seq::Sequencer& s = make();
  run_steps(s, 3);
  NEED(count(EventKind::MidiCc) == 2u);
  const audio::SeqEvent& a = g_col.nth(EventKind::MidiCc, 0)->ev;
  CHECK((a.a == 0 && a.b == 20 && a.c == audio::kChannelDefault && a.flags == 0 && a.value == 0));
  const audio::SeqEvent& b = g_col.nth(EventKind::MidiCc, 1)->ev;
  CHECK((b.a == 1 && b.b == 21 && b.c == audio::kChannelDefault && b.flags == 0 && b.value == 0));
}

TEST_CASE("param auto: a CC letter in the mask of a sample instrument is handed to the main loop (last value wins) instead of sent; a letter outside the mask, and a MIDI instrument's, still send") {
  project16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kCcA, 20, fx::kSlot37, 9)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(0, 1, fx::kCcA, 33, fx::kCcA + 1, 21)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 2, make_step(61, kMidi, fx::kCcA, 44)) == model::Status::Ok);
  seq::Sequencer& s = make();
  std::uint8_t val[6] = {};
  CHECK(s.take_auto(val) == 0u);
  s.set_auto_mask(0x21u);  // CC A and CC F
  run_steps(s, 4);
  // CC B (not mapped) and the MIDI instrument's CC A (a controller) are the events that remain.
  NEED(count(EventKind::MidiCc) == 2u);
  CHECK(g_col.nth(EventKind::MidiCc, 0)->ev.b == 21);
  CHECK(g_col.nth(EventKind::MidiCc, 1)->ev.b == 44);
  std::uint8_t taken = s.take_auto(val);
  CHECK(taken == 0x21u);
  CHECK(val[0] == 33);  // step 0 said 20, step 1 said 33: the main loop applies the latest
  CHECK(val[5] == 9);
  CHECK(s.take_auto(val) == 0u);  // taken once
}

// ---- the contract of the new kinds ----------------------------------------------------------------------------------------------------------

TEST_CASE("midi inst: the default instrument makes the events it made before INST 1.3 (flags 0, value 0, no program, no bend)") {
  project16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidi)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(62, kMidi, fx::kMicroTune, 99)) == model::Status::Ok);
  seq::Sequencer& s = make();
  run_steps(s, 3);
  CHECK(count(EventKind::MidiProgram) == 0u);
  CHECK(count(EventKind::MidiPitchBend) == 0u);
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    const audio::SeqEvent& e = g_col.rec[i].ev;
    if (e.kind == EventKind::MidiNoteOn) {
      CHECK(e.b == 127);
      CHECK(e.c == 2);
      CHECK(e.flags == 0);
      CHECK(e.value == 0);
    }
    if (e.kind == EventKind::MidiNoteOff) {
      CHECK(e.flags == 0);
      CHECK(e.value == 0);
    }
  }
}
