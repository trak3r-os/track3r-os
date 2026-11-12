// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S2b: a pad on a MIDI instrument sends MIDI (seq/midi_link.h LIVE MIDI NOTES). The path under test is the real one, minus the main loop: a pad event on the live queue
// (audio/live_notes.h), Engine::play_live offering it to MidiLink::live_event, MidiOut and the two ports, captured on the DIN emitter and the USB ring. Note-on / note-off pairs on
// channels 1 and 16, the pad's velocity, what is not a pad or not a MIDI instrument, the hang rules (the release never looks at the instrument; an all-notes-off; a stop; a lost
// release repaired), the queue's bounds and the order against the sequencer's own MIDI in the same block. The app side (the instrument changes of O2) is inst_live_midi_test.cpp.
#include <doctest/doctest.h>

#include <o3os/audio/live_notes.h>
#include <o3os/model/instrument.h>

#include "support/live_midi_rig.h"
#include "support/need.h"
#include "support/seq_fixture.h"

#include <cstdint>
#include <utility>
#include <vector>

using namespace o3os;
using namespace o3os::test;
using audio::LiveEvent;
using audio::LiveKind;

namespace {

LiveEvent on(std::uint8_t source, std::uint8_t key, std::uint8_t note, std::uint8_t instrument, std::uint8_t velocity = audio::kLiveVelocityDefault, std::uint8_t track = 0) {
  return LiveEvent{LiveKind::On, source, key, note, instrument, velocity, track, 0};
}
LiveEvent pad_on(std::uint8_t pad, std::uint8_t note, std::uint8_t instrument, std::uint8_t velocity = audio::kLiveVelocityDefault) {
  return on(audio::kLiveSourcePad, pad, note, instrument, velocity);
}
LiveEvent pad_off(std::uint8_t pad) { return LiveEvent{LiveKind::Off, audio::kLiveSourcePad, pad, 0, 0, 0, 0, 0}; }
LiveEvent all_off(std::uint8_t source) { return LiveEvent{LiveKind::AllOff, source, 0, 0, 0, 0, 0, 0}; }

WireNote note_on(std::uint8_t ch, std::uint8_t note, std::uint8_t vel = 127) { return WireNote{true, ch, note, vel}; }
WireNote note_off(std::uint8_t ch, std::uint8_t note) { return WireNote{false, ch, note, 0}; }

// Notes only: a note-off's velocity is the router's (0), not under test.
bool same(const std::vector<WireNote>& got, const std::vector<WireNote>& want) {
  if (got.size() != want.size()) {
    return false;
  }
  for (std::size_t i = 0; i < got.size(); ++i) {
    if (got[i].on != want[i].on || got[i].ch != want[i].ch || got[i].note != want[i].note || (got[i].on && got[i].vel != want[i].vel)) {
      return false;
    }
  }
  return true;
}

void midi_slot(std::uint32_t slot, std::uint8_t channel) {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = channel;
  CHECK(seq_project().set_instrument(slot, in) == model::Status::Ok);
}

// Instruments (1-based, as a pad names them): 1..2 samples, 11 MIDI channel 3, 12 MIDI channel 1, 13 MIDI channel 16.
void setup(LiveMidiRig& rig) {
  fresh_project(2, 2);
  midi_slot(11, 1);
  midi_slot(12, 16);
  rig.init(seq_project());
}

LiveMidiRig& rig() {
  static LiveMidiRig r;
  return r;
}

void push(const LiveEvent& e) { CHECK(rig().box().queue.push(e)); }

// Both ports saw the same notes, in the same order.
void ports_agree() { CHECK(same(rig().din_notes(), rig().notes())); }

}  // namespace

TEST_CASE("live midi: a pad on a MIDI instrument sends note-on then note-off on the instrument's channel, channels 1 and 16, on both ports") {
  setup(rig());
  push(pad_on(5, 60, 12));  // instrument 12: channel 1
  push(pad_on(6, 72, 13));  // instrument 13: channel 16
  push(pad_on(7, 64, 11));  // instrument 11: channel 3
  rig().blocks(3);
  CHECK(same(rig().notes(), {note_on(0, 60), note_on(15, 72), note_on(2, 64)}));  // the pad's default velocity is full, as a step's
  CHECK(rig().pool().voices_in_use() == 0);  // a MIDI instrument starts no voice, and holds no track
  CHECK(rig().engine().live_held() == 0);
  push(pad_off(6));
  push(pad_off(5));
  push(pad_off(7));
  rig().settle();
  CHECK(same(rig().notes(), {note_on(0, 60), note_on(15, 72), note_on(2, 64), note_off(15, 72), note_off(0, 60), note_off(2, 64)}));
  ports_agree();
  CHECK(LiveMidiRig::balanced(rig().notes()));
  CHECK(rig().link().out().book().owed() == 0u);
}

TEST_CASE("live midi: the same pad twice is two pairs, and two pads on one note are two notes") {
  setup(rig());
  for (unsigned i = 0; i < 2; ++i) {
    push(pad_on(1, 48, 12));
    rig().blocks(2);
    push(pad_off(1));
    rig().blocks(2);
  }
  push(pad_on(2, 50, 12));
  push(pad_on(3, 50, 12));
  rig().blocks(2);
  push(pad_off(2));
  push(pad_off(3));
  rig().settle();
  CHECK(same(rig().notes(), {note_on(0, 48), note_off(0, 48), note_on(0, 48), note_off(0, 48), note_on(0, 50), note_on(0, 50), note_off(0, 50), note_off(0, 50)}));
  CHECK(LiveMidiRig::balanced(rig().notes()));
  ports_agree();
}

TEST_CASE("live midi: the velocity of the event is the note's; the pad's default is 127 and 0 is never sent as a note-on") {
  setup(rig());
  push(pad_on(0, 60, 12, 100));
  push(pad_on(1, 61, 12, 0));
  push(pad_on(2, 62, 12, 200));   // past 127
  push(pad_on(3, 200, 12));       // note past 127
  rig().settle();
  CHECK(same(rig().notes(), {note_on(0, 60, 100), note_on(0, 61, 1), note_on(0, 62, 127), note_on(0, 127, 127)}));
  push(all_off(audio::kLiveSourcePad));
  rig().settle();
  CHECK(LiveMidiRig::balanced(rig().notes()));
}

TEST_CASE("live midi: what is not a pad or not a MIDI instrument is not MIDI, and a sample instrument still plays") {
  static std::int16_t dc[4000];
  for (std::int16_t& s : dc) {
    s = 16384;
  }
  setup(rig());
  rig().pool().set_sample(0, audio::SampleDesc{dc, 4000, 0, 0, 65536});  // instrument 1
  push(on(audio::kLiveSourceMidi, 60, 60, 12, 100));   // the keyboard: not this slice
  push(on(audio::kLiveSourcePreview, 0, 60, 12));      // the preview voice
  push(pad_on(4, 60, 0));                              // instrument 0 names none
  push(pad_on(5, 60, 30));                             // a free slot
  push(pad_on(48, 60, 12));                            // past the pads
  rig().blocks(3);
  CHECK(rig().notes().empty());
  push(pad_on(9, 60, 1));                              // a sample instrument: a voice, no MIDI
  rig().blocks(2);
  CHECK(rig().pool().voices_in_use() == 1);
  CHECK(rig().engine().live_held() == 1);
  push(pad_off(9));
  rig().settle();
  CHECK(rig().notes().empty());
  ports_agree();
}

TEST_CASE("live midi: the release does not look at the instrument (O2): its channel changed, the slot freed, the kind replaced") {
  setup(rig());
  push(pad_on(1, 60, 12));  // channel 1
  push(pad_on(2, 61, 13));  // channel 16
  push(pad_on(3, 62, 11));  // channel 3
  rig().blocks(3);
  CHECK(LiveMidiRig::held(rig().notes()) == 3);
  midi_slot(11, 9);                                              // channel 1 -> 9 while held
  CHECK(seq_project().clear_instrument(12) == model::Status::Ok);  // slot freed
  model::Instrument s;
  model::init_instrument(s, model::InstrumentKind::Sample);
  CHECK(seq_project().set_instrument(10, s) == model::Status::Ok);  // a sample instrument now
  push(pad_off(1));
  push(pad_off(2));
  push(pad_off(3));
  rig().settle();
  CHECK(same(rig().notes(), {note_on(0, 60), note_on(15, 61), note_on(2, 62), note_off(0, 60), note_off(15, 61), note_off(2, 62)}));
  CHECK(LiveMidiRig::balanced(rig().notes()));
}

TEST_CASE("live midi: an all-notes-off of the pads, or of all sources, ends the MIDI notes; the keyboard's does not") {
  setup(rig());
  for (std::uint8_t k = 0; k < 4; ++k) {
    push(pad_on(k, static_cast<std::uint8_t>(60 + k), 12));
  }
  rig().blocks(3);
  push(all_off(audio::kLiveSourceMidi));
  rig().blocks(3);
  CHECK(LiveMidiRig::held(rig().notes()) == 4);  // a keyboard's all-off leaves the pads' notes
  push(all_off(audio::kLiveSourcePad));
  rig().settle();
  CHECK(LiveMidiRig::held(rig().notes()) == 0);
  CHECK(rig().notes().size() == 8u);
  for (std::uint8_t k = 0; k < 4; ++k) {
    push(pad_on(k, static_cast<std::uint8_t>(60 + k), 13));
  }
  rig().blocks(3);
  push(all_off(audio::kLiveAllSources));  // App::reset_input()
  rig().settle();
  CHECK(LiveMidiRig::balanced(rig().notes()));
  CHECK(rig().notes().size() == 16u);
  push(pad_off(0));  // the pads' own releases come later: nothing more is sent
  push(pad_off(3));
  rig().settle();
  CHECK(rig().notes().size() == 16u);
  ports_agree();
}

TEST_CASE("live midi: a pad that goes down while it holds a note ends that note first") {
  setup(rig());
  push(pad_on(7, 60, 12));
  rig().blocks(2);
  push(pad_on(7, 62, 13));  // the release was lost, and the pad is on another instrument now
  rig().blocks(2);
  push(pad_off(7));
  rig().settle();
  CHECK(same(rig().notes(), {note_on(0, 60), note_off(0, 60), note_on(15, 62), note_off(15, 62)}));
  push(pad_on(7, 64, 12));
  rig().blocks(2);
  push(pad_on(7, 65, 1));  // now a sample instrument (no sample in the pool: nothing starts), the MIDI note still ends
  rig().settle();
  CHECK(LiveMidiRig::balanced(rig().notes()));
}

TEST_CASE("live midi: the transport stopping with a pad held takes the note once, and the pad's release then adds nothing (MIDI panic)") {
  setup(rig());
  CHECK(seq_project().create_pattern(1, 16) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(seq_project().set_step(1, 0, k, make_step(static_cast<std::uint8_t>(40 + k), 11)) == model::Status::Ok);  // the sequencer's MIDI: instrument 11, channel 3
  }
  CHECK(seq_project().append_order(1) == model::Status::Ok);
  rig().sequencer().play_pattern(1);
  rig().blocks(2);
  push(pad_on(4, 90, 12));  // channel 1
  rig().blocks(4);
  rig().sequencer().stop();
  rig().settle();
  const std::size_t after_stop = rig().notes().size();
  CHECK(LiveMidiRig::held(rig().notes()) == 0);  // the sequencer's notes and the pad's are all off: the router's hang book paid both
  push(pad_off(4));
  rig().settle();
  CHECK(rig().notes().size() == after_stop);     // the off was not owed any more: dropped, not sent twice
  CHECK(rig().link().out().stats().stray_offs >= 1u);
  CHECK(LiveMidiRig::balanced(rig().notes()));
  ports_agree();
}

TEST_CASE("live midi: a note staged and not yet handed over when the transport stops never sounds, and its release adds nothing") {
  setup(rig());
  CHECK(seq_project().create_pattern(1, 16) == model::Status::Ok);
  CHECK(seq_project().set_step(1, 0, 0, make_step(40, 1)) == model::Status::Ok);
  CHECK(seq_project().append_order(1) == model::Status::Ok);
  rig().sequencer().play_pattern(1);
  rig().blocks(2);
  push(pad_on(4, 90, 12));
  rig().block();              // popped: staged for the next block's start
  rig().sequencer().stop();
  rig().settle();             // the stop's drop of staged note-ons took it
  push(pad_off(4));
  rig().settle();
  CHECK(LiveMidiRig::balanced(rig().notes()));
  CHECK(rig().link().out().book().owed() == 0u);
}

TEST_CASE("live midi: the queue is bounded: kLivePerBlock events a block, in order, the rest waits; a full queue refuses the producer, and the repair all-notes-off ends every note") {
  setup(rig());
  unsigned pushed = 0;
  for (std::uint8_t k = 0; k < 48; ++k) {  // 48 note-ons fit in the 64-event queue
    if (!rig().box().queue.push(pad_on(k, static_cast<std::uint8_t>(30 + k % 48), 12))) {
      break;
    }
    ++pushed;
  }
  CHECK(pushed == 48u);
  rig().block();
  CHECK(rig().box().queue.size() == pushed - audio::kLivePerBlock);  // one block took 16
  rig().blocks(2);  // staged at the end of the block they were taken in, handed over a block later
  CHECK(rig().box().queue.empty());
  rig().settle(400);
  NEED(rig().notes().size() == 48u);
  for (std::uint8_t k = 0; k < 48; ++k) {
    CHECK(rig().notes()[k].note == 30 + k);  // in the order they were pushed
  }
  // The producer's side of an overflow: the ring refuses, nothing is lost silently (the platform counts it) and a refused Off is repaired by an all-notes-off (LivePreview::tick).
  unsigned refused = 0;
  for (unsigned i = 0; i < 2u * audio::kLiveQueueSize; ++i) {
    refused += rig().box().queue.push(pad_off(static_cast<std::uint8_t>(i % 48))) ? 0u : 1u;
  }
  CHECK(refused > 0u);
  rig().blocks(8);  // drains what was accepted
  push(all_off(audio::kLiveAllSources));
  rig().settle(400);
  CHECK(LiveMidiRig::held(rig().notes()) == 0);
  CHECK(LiveMidiRig::balanced(rig().notes()));
  ports_agree();
}

TEST_CASE("live midi: against the sequencer's MIDI of the same block: after the events of the block it was taken in, ahead of the next block's, each port in due order") {
  setup(rig());
  const auto seq_event = [](audio::EventKind kind, std::uint8_t note, std::uint8_t ch, std::uint16_t offset) {
    audio::SeqEvent e{};
    e.offset = offset;
    e.kind = kind;
    e.track = 0;
    e.a = note;
    e.b = 100;
    e.c = ch;
    return e;
  };
  rig().blocks(2);
  const std::uint64_t start = rig().sample();  // the block about to render starts here
  // the sequencer's note late in the block that the pad's is taken in (the router stages it; the block's step flushes it) ...
  CHECK(rig().link().out().send(seq_event(audio::EventKind::MidiNoteOn, 40, 2, 63), start));
  push(pad_on(1, 60, 12));
  rig().block();
  // ... and the next block's very first sample (the live note's own due time): the live note was staged first, a stable sort keeps it ahead
  CHECK(rig().link().out().send(seq_event(audio::EventKind::MidiNoteOn, 41, 2, 0), start + 64));
  CHECK(rig().link().out().send(seq_event(audio::EventKind::MidiNoteOn, 42, 2, 5), start + 64));
  rig().settle();
  CHECK(same(rig().notes(), {note_on(2, 40, 100), note_on(0, 60), note_on(2, 41, 100), note_on(2, 42, 100)}));
  ports_agree();
}

TEST_CASE("live midi: a real sequencer step and a pad in one block: the step's note, then the pad's, each with its own off") {
  setup(rig());
  CHECK(seq_project().create_pattern(1, 16) == model::Status::Ok);
  CHECK(seq_project().set_step(1, 0, 0, make_step(40, 11)) == model::Status::Ok);  // instrument 11, channel 3, at the first step
  CHECK(seq_project().append_order(1) == model::Status::Ok);
  rig().sequencer().play_pattern(1);
  push(pad_on(2, 70, 12));
  rig().blocks(3);
  NEED(rig().notes().size() >= 2u);
  CHECK(same({rig().notes()[0], rig().notes()[1]}, {note_on(2, 40, 127), note_on(0, 70)}));
  push(pad_off(2));
  rig().sequencer().stop();
  rig().settle();
  CHECK(LiveMidiRig::balanced(rig().notes()));
  ports_agree();
}

TEST_CASE("live midi: an offline export (MIDI out suppressed) sends nothing and a pad's release leaves nothing owed") {
  setup(rig());
  rig().link().out().set_suppress(true);
  push(pad_on(1, 60, 12));
  rig().blocks(3);
  push(pad_off(1));
  rig().settle();
  CHECK(rig().notes().empty());
  CHECK(rig().link().out().book().owed() == 0u);
}

TEST_CASE("live midi: an export stops a pad held on a MIDI instrument: its note-off goes out when the export takes the engine, it is named, and the late release sends nothing (OS-EXPNOTE)") {
  setup(rig());
  push(pad_on(5, 60, 13));  // instrument 13: channel 16
  push(pad_on(6, 62, 11));  // instrument 11: channel 3
  rig().blocks(3);
  CHECK(same(rig().notes(), {note_on(15, 60), note_on(2, 62)}));
  CHECK(rig().engine().live_held() == 0);  // the link took them: not in the engine's table
  rig().engine().offline_request();
  rig().engine().offline_enter(audio::EventSource{});
  CHECK(rig().engine().live_stopped_count() == 2u);
  const std::uint32_t w = rig().engine().live_stopped_what();
  CHECK(audio::held_source(w) == audio::kLiveSourcePad);
  CHECK(audio::held_key(w) == 5u);
  CHECK(audio::held_note(w) == 60u);
  CHECK(audio::held_channel(w) == 16u);
  rig().settle();  // the ports' interrupts run over the export
  CHECK(same(rig().notes(), {note_on(15, 60), note_on(2, 62), note_off(15, 60), note_off(2, 62)}));  // before the engine is given back
  rig().engine().offline_leave();
  push(pad_off(5));  // the late releases
  push(pad_off(6));
  rig().settle();
  CHECK(same(rig().notes(), {note_on(15, 60), note_on(2, 62), note_off(15, 60), note_off(2, 62)}));  // nothing more: no stray off
  ports_agree();
  CHECK(LiveMidiRig::balanced(rig().notes()));
  CHECK(rig().link().out().book().owed() == 0u);
}

// OS-PIANOBEND: the pitch-bend strip of PIANO mode on a MIDI instrument (a Bend live event, seq/midi_link.h): one 14-bit pitch bend a press, the centre on release.
namespace {

LiveEvent bend(int semitones, std::uint8_t instrument) {
  return LiveEvent{LiveKind::Bend, audio::kLiveSourcePad, 0, static_cast<std::uint8_t>(static_cast<std::int8_t>(semitones)), instrument, 0, 0, 0};
}

using Bends = std::vector<std::pair<int, int>>;

// The pitch bends of one port as (channel, 14-bit value).
Bends bends_of(std::uint8_t port) {
  Bends r;
  for (const WireMsg& m : rig().msgs_of(port)) {
    if ((m.b[0] & 0xF0u) == 0xE0u) {
      r.emplace_back(m.b[0] & 0x0F, m.b[1] | (m.b[2] << 7));
    }
  }
  return r;
}

void midi_slot_range(std::uint32_t slot, std::uint8_t channel, std::uint8_t range) {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = channel;
  in.midi_pb_range = range;
  CHECK(seq_project().set_instrument(slot, in) == model::Status::Ok);
}

}  // namespace

TEST_CASE("live midi: a bend pad sends a 14-bit pitch bend on the instrument's channel, +-2 at the default range is the full travel, release returns to the centre (OS-PIANOBEND)") {
  setup(rig());
  push(bend(2, 12));  // channel 1, range 2: +2 semitones is the top
  rig().blocks(2);
  CHECK(bends_of(1) == Bends{{0, 16383}});
  CHECK(bends_of(0) == Bends{{0, 16383}});  // both ports
  push(bend(-2, 12));
  rig().blocks(2);
  CHECK(bends_of(1) == Bends{{0, 16383}, {0, 0}});
  push(bend(1, 12));
  rig().blocks(2);
  const Bends one = bends_of(1);
  NEED(one.size() == 3u);
  CHECK(one[2].second > 8192 + 4000);  // half of the travel
  CHECK(one[2].second < 8192 + 4200);
  push(bend(0, 12));  // the release
  rig().settle();
  CHECK(bends_of(1).size() == 4u);
  CHECK(bends_of(1).back() == std::make_pair(0, 8192));
  CHECK(rig().notes().empty());  // no note: the bend is not a note
  CHECK(rig().pool().voices_in_use() == 0);
  CHECK(rig().link().out().book().owed() == 0u);
}

TEST_CASE("live midi: the MIDI-in wheel reaches a MIDI instrument as the raw 14-bit value whatever its range, and the centre returns to 8192 (OS-PIANOBEND-REC)") {
  setup(rig());
  const auto fine = [](unsigned v14, std::uint8_t instrument) {
    return LiveEvent{LiveKind::BendFine, audio::kLiveSourceMidi, static_cast<std::uint8_t>(v14 & 0x7Fu), static_cast<std::uint8_t>((v14 >> 7) & 0x7Fu), instrument, 0, 0, 0};
  };
  push(fine(12345, 12));
  rig().settle();
  CHECK(bends_of(1) == Bends{{0, 12345}});
  CHECK(bends_of(0) == Bends{{0, 12345}});
  midi_slot_range(11, 1, 12);  // a range of 12 does not scale it: the keyboard's wheel is already in the instrument's own units
  push(fine(1, 12));
  rig().settle();
  CHECK(bends_of(1).back() == std::make_pair(0, 1));
  push(fine(8192, 12));  // the centre
  rig().settle();
  CHECK(bends_of(1).back() == std::make_pair(0, 8192));
  CHECK(rig().notes().empty());
  CHECK(rig().pool().voices_in_use() == 0);
  CHECK(rig().link().out().book().owed() == 0u);
}

TEST_CASE("live midi: a bend scales by the instrument's range, range 0 stays at the centre, another route is centred first (OS-PIANOBEND)") {
  setup(rig());
  midi_slot_range(11, 1, 12);  // instrument 12: channel 1, range 12 semitones: +2 is a sixth of the travel
  midi_slot_range(12, 16, 0);  // instrument 13: channel 16, range 0
  push(bend(2, 12));
  rig().settle();  // (a centre of the sequencer's kind drops the bends of its channel that are still staged, so each event gets its own settle)
  push(bend(2, 13));  // another route: the first goes back to the centre before this one is bent
  rig().settle();
  const Bends b = bends_of(1);
  NEED(b.size() == 3u);
  CHECK(b[0].first == 0);
  CHECK(b[0].second > 8192 + 1300);  // 8192 * 2 / 12 = 1365
  CHECK(b[0].second < 8192 + 1400);
  CHECK(b[1] == std::make_pair(0, 8192));   // the old route centred
  CHECK(b[2] == std::make_pair(15, 8192));  // range 0: the centre (no bend)
  push(bend(-1, 12));
  rig().settle();
  const Bends c = bends_of(1);
  NEED(c.size() >= 5u);
  CHECK(c[c.size() - 2] == std::make_pair(15, 8192));  // channel 16 centred when channel 1 is bent again
  CHECK(c.back().first == 0);
  CHECK(c.back().second < 8192 - 600);
}

TEST_CASE("live midi: an all-notes-off of the pads or of all sources centres a bent route, the keyboard's does not (OS-PIANOBEND)") {
  setup(rig());
  push(bend(2, 13));  // channel 16
  rig().blocks(2);
  push(all_off(audio::kLiveSourceMidi));
  rig().blocks(2);
  CHECK(bends_of(1) == Bends{{15, 16383}});
  push(all_off(audio::kLiveSourcePad));
  rig().settle();
  CHECK(bends_of(1) == Bends{{15, 16383}, {15, 8192}});
  push(all_off(audio::kLiveAllSources));  // nothing is bent now: nothing more is sent
  rig().settle();
  CHECK(bends_of(1).size() == 2u);
  push(bend(-2, 12));
  rig().settle();
  push(all_off(audio::kLiveAllSources));  // App::reset_input()
  rig().settle();
  CHECK(bends_of(1).size() == 4u);
  CHECK(bends_of(1).back() == std::make_pair(0, 8192));
  ports_agree();
}

TEST_CASE("live midi: a bend on a sample instrument or on none is not consumed, a release with nothing bent sends nothing (OS-PIANOBEND)") {
  setup(rig());
  push(bend(2, 1));    // a sample instrument
  push(bend(1, 0));    // none
  push(bend(-1, 30));  // a free slot
  push(bend(0, 12));   // a release with nothing bent
  rig().settle();
  CHECK(bends_of(1).empty());
  CHECK(bends_of(0).empty());
  // A bend released on a sample instrument (the instrument in view changed during the hold) still puts the MIDI route back.
  push(bend(2, 12));
  rig().settle();
  push(bend(0, 1));
  rig().settle();
  CHECK(bends_of(1) == Bends{{0, 16383}, {0, 8192}});
}

TEST_CASE("live midi: an export starts with the strip's bend gone, on the voices and on the MIDI route; the late release adds nothing (OS-PIANOBEND)") {
  setup(rig());
  push(bend(2, 13));  // a MIDI route bent (channel 16)
  rig().settle();
  CHECK(bends_of(1) == Bends{{15, 16383}});
  rig().pool().live_bend(-2, 1u);  // and the voices' own bend, as a strip on a sampler track holds it
  CHECK(rig().pool().live_bend_q16() == -2 * 65536);
  rig().engine().offline_request();
  rig().engine().offline_enter(audio::EventSource{});
  CHECK(rig().pool().live_bend_q16() == 0);  // the export renders unbent
  rig().settle();
  CHECK(bends_of(1) == Bends{{15, 16383}, {15, 8192}});  // the receiver is back in tune before the export's own events
  CHECK(bends_of(0) == Bends{{15, 16383}, {15, 8192}});
  rig().engine().offline_leave();
  push(bend(0, 13));  // the late release
  rig().settle();
  CHECK(bends_of(1).size() == 2u);
  CHECK(rig().pool().live_bend_q16() == 0);
  ports_agree();
}

// OS-PIANOVIB: the vibrato pad of PIANO mode on a MIDI instrument (a Vibrato live event, seq/midi_link.h): CC1 127 on the press, CC1 0 on the release, both on the instrument's route.
namespace {

LiveEvent vib(bool on, std::uint8_t instrument) {
  return LiveEvent{LiveKind::Vibrato, audio::kLiveSourcePad, static_cast<std::uint8_t>(on ? 1 : 0), 0, instrument, 0, 0, 0};
}

// The CC1 messages of one port as (channel, value).
Bends ccs_of(std::uint8_t port) {
  Bends r;
  for (const WireMsg& m : rig().msgs_of(port)) {
    if ((m.b[0] & 0xF0u) == 0xB0u && m.b[1] == 1u) {
      r.emplace_back(m.b[0] & 0x0F, m.b[2]);
    }
  }
  return r;
}

}  // namespace

TEST_CASE("live midi: the vibrato pad sends CC1 127 on the instrument's channel and CC1 0 on release, on both ports, no note and no voice (OS-PIANOVIB)") {
  setup(rig());
  push(vib(true, 12));  // channel 1
  rig().blocks(2);
  CHECK(ccs_of(1) == Bends{{0, 127}});
  CHECK(ccs_of(0) == Bends{{0, 127}});
  CHECK(!rig().pool().live_vib_on());  // taken by the MIDI link: the voices get no LFO
  push(vib(false, 12));
  rig().settle();
  CHECK(ccs_of(1) == Bends{{0, 127}, {0, 0}});
  CHECK(rig().notes().empty());
  CHECK(rig().pool().voices_in_use() == 0);
  CHECK(rig().link().out().book().owed() == 0u);
  push(vib(false, 12));  // a release with nothing on sends nothing
  rig().settle();
  CHECK(ccs_of(1).size() == 2u);
  ports_agree();
}

TEST_CASE("live midi: an all-notes-off of the pads or of all sources ends a vibrato, the keyboard's does not; another route is ended first (OS-PIANOVIB)") {
  setup(rig());
  push(vib(true, 13));  // channel 16
  rig().blocks(2);
  push(all_off(audio::kLiveSourceMidi));
  rig().blocks(2);
  CHECK(ccs_of(1) == Bends{{15, 127}});
  push(all_off(audio::kLiveSourcePad));
  rig().settle();
  CHECK(ccs_of(1) == Bends{{15, 127}, {15, 0}});
  push(all_off(audio::kLiveAllSources));  // nothing is on now: nothing more is sent
  rig().settle();
  CHECK(ccs_of(1).size() == 2u);
  push(vib(true, 13));
  push(vib(true, 12));  // the instrument in view changed while the pad was held: the first route goes back to 0 before the second starts
  rig().settle();
  CHECK(ccs_of(1) == Bends{{15, 127}, {15, 0}, {15, 127}, {15, 0}, {0, 127}});
  push(all_off(audio::kLiveAllSources));  // App::reset_input()
  rig().settle();
  CHECK(ccs_of(1).back() == std::make_pair(0, 0));
  ports_agree();
}

TEST_CASE("live midi: a vibrato on a sample instrument or on none is left to the voices and sends no CC; a release on a sample instrument still ends the MIDI route (OS-PIANOVIB)") {
  setup(rig());
  push(vib(true, 1));   // a sample instrument
  push(vib(true, 0));   // none
  push(vib(true, 30));  // a free slot
  rig().settle();
  CHECK(ccs_of(1).empty());
  CHECK(ccs_of(0).empty());
  push(vib(false, 1));
  push(vib(true, 12));
  rig().settle();
  push(vib(false, 1));
  rig().settle();
  CHECK(ccs_of(1) == Bends{{0, 127}, {0, 0}});
}

TEST_CASE("live midi: an export starts with the vibrato off, on the voices and on the MIDI route; the late release adds nothing (OS-PIANOVIB)") {
  setup(rig());
  push(vib(true, 13));
  rig().settle();
  CHECK(ccs_of(1) == Bends{{15, 127}});
  rig().pool().live_vib(true, 1u);  // and the voices' own LFO, as a pad on a sampler track holds it
  CHECK(rig().pool().live_vib_on());
  rig().engine().offline_request();
  rig().engine().offline_enter(audio::EventSource{});
  CHECK(!rig().pool().live_vib_on());
  rig().settle();
  CHECK(ccs_of(1) == Bends{{15, 127}, {15, 0}});
  CHECK(ccs_of(0) == Bends{{15, 127}, {15, 0}});
  rig().engine().offline_leave();
  push(vib(false, 13));  // the late release
  rig().settle();
  CHECK(ccs_of(1).size() == 2u);
  CHECK(!rig().pool().live_vib_on());
  ports_agree();
}
