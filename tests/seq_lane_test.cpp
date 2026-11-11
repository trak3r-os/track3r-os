// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MIDIREC-P1 S2 (docs/specs/OS-MIDIREC-P1.md A4..A7, A17): the sequencer plays the MIDI take lanes of a LaneStore beside the steps. A lane note sounds for its length counted in microsteps (a loop, a stop, a jump and a
// pattern change end or keep it as the spec says), the events of a block are capped, control changes are rate limited with the latest value winning, a jump chases the controllers, and a block that meets the store mid-write
// (an odd seq) plays nothing yet loses no note-off. Host only (the LaneStore is 76 KiB).
#include <doctest/doctest.h>

#include <o3os/model/lane_store.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>
#include <initializer_list>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
using model::LaneEvent;
using model::LaneStore;

namespace {

constexpr std::uint32_t kBlock = 64;

LaneStore& store() {
  static LaneStore s;
  return s;
}

LaneEvent note(std::uint32_t tick, std::uint32_t len, std::uint8_t n, std::uint8_t vel = 100, std::uint8_t ch = 0, std::uint8_t port = 0) {
  LaneEvent e{};
  e.tick = static_cast<std::uint16_t>(tick);
  e.len = static_cast<std::uint16_t>(len);
  e.kind_channel = static_cast<std::uint8_t>(ch);
  e.port = port;
  e.a = n;
  e.b = vel;
  return e;
}
LaneEvent cc(std::uint32_t tick, std::uint8_t ctl, std::uint8_t v, std::uint8_t ch = 0, std::uint8_t port = 0) {
  LaneEvent e{};
  e.tick = static_cast<std::uint16_t>(tick);
  e.kind_channel = static_cast<std::uint8_t>((1u << 4) | ch);
  e.port = port;
  e.a = ctl;
  e.b = v;
  return e;
}
LaneEvent poly(std::uint32_t tick, std::uint8_t n, std::uint8_t v) {
  LaneEvent e{};
  e.tick = static_cast<std::uint16_t>(tick);
  e.kind_channel = static_cast<std::uint8_t>(4u << 4);
  e.a = n;
  e.b = v;
  return e;
}

void set_lane(std::uint32_t pattern, std::uint32_t track, std::initializer_list<LaneEvent> ev) {
  LaneStore& s = store();
  const LaneStore::View v = s.lane(pattern, track);
  if (v.count != 0) {
    CHECK(s.erase(pattern, track, 0, v.count));
  }
  if (ev.size() == 0) {
    return;
  }
  LaneStore::WriteWindow w(s);
  LaneEvent* gap = s.open_gap(pattern, track, 0, static_cast<std::uint32_t>(ev.size()));
  NEED(gap != nullptr);
  std::uint32_t i = 0;
  for (const LaneEvent& e : ev) {
    gap[i++] = e;
  }
}

// A 16-step pattern 1, `tracks` tracks, an empty store that belongs to the project; steps carry nothing unless a test adds them.
void setup(std::uint32_t tracks = 2) {
  fresh_project(tracks, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  store().clear();
  store().adopt(p.load_epoch());
}

seq::Sequencer& make(bool attach = true) {
  seq::Sequencer& s = seq_slot().make(seq_project());
  s.configure(seq::Sequencer::Config{});
  Collector& c = seq_collector();
  c.n = 0;
  c.total_events = 0;
  c.hash = 0xCBF29CE484222325ull;
  if (attach) {
    static seq::LaneRun run;  // the caller owns the playback state (SDRAM on the unit)
    s.attach_lanes(&store(), &run);
  }
  return s;
}

Collector& col() { return seq_collector(); }

// Runs `steps` more steps at 120 BPM (a step is 48 ticks of 1/384 s: 5515 samples, rounded; each run ends 300 samples into its last step so that the events of a step's first block are in).
constexpr std::uint64_t kStep = 5515;
std::uint64_t g_now = 0;
void run_to(seq::Sequencer& s, std::uint64_t to) {
  col().run(s, g_now, to, kBlock);
  g_now = to;
}
void run_steps(seq::Sequencer& s, std::uint32_t steps) { run_to(s, (g_now / kStep + steps) * kStep + 300u); }

std::uint64_t step_time(std::uint32_t n) {
  const Rec* r = col().nth(EventKind::Step, n);
  return r != nullptr ? r->t : ~std::uint64_t{0};
}

}  // namespace

TEST_CASE("lane: a note starts on its tick and ends after its length, on the step grid of the track") {
  setup();
  set_lane(1, 0, {note(96, 48, 60, 90, 3, 1)});  // step 2, one step long, channel 4, DIN
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 5);
  NEED(col().count(EventKind::MidiNoteOn) == 1u);
  NEED(col().count(EventKind::MidiNoteOff) == 1u);
  const Rec* on = col().nth(EventKind::MidiNoteOn, 0);
  const Rec* off = col().nth(EventKind::MidiNoteOff, 0);
  CHECK(on->t == step_time(2));
  CHECK(off->t == step_time(3));
  CHECK(on->ev.track == 0);
  CHECK(on->ev.a == 60);
  CHECK(on->ev.b == 90);
  CHECK(on->ev.c == 3);
  CHECK(on->ev.flags == 1);
  CHECK(off->ev.a == 60);
  CHECK(off->ev.c == 3);
  CHECK(off->ev.flags == 1);
}

TEST_CASE("lane: a microstep tick inside a step starts at that microstep") {
  setup();
  set_lane(1, 0, {note(48 + 24, 12, 62)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 3);
  NEED(col().count(EventKind::MidiNoteOn) == 1u);
  NEED(col().count(EventKind::MidiNoteOff) == 1u);
  const std::uint64_t on = col().nth(EventKind::MidiNoteOn, 0)->t;
  const std::uint64_t off = col().nth(EventKind::MidiNoteOff, 0)->t;
  CHECK(on > step_time(1));
  CHECK(on < step_time(2));
  CHECK(off > on);
  CHECK(off < step_time(2));
}

TEST_CASE("lane: a project without lanes sounds byte for byte the same with a store attached") {
  setup();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 10)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 4, make_step(64, 10)) == model::Status::Ok);
  g_now = 0;
  seq::Sequencer& a = make(false);
  a.play_pattern(1);
  run_steps(a, 20);
  const std::uint64_t plain = col().hash;
  const std::uint32_t n = col().total_events;
  NEED(n > 20u);
  g_now = 0;
  seq::Sequencer& b = make(true);
  b.play_pattern(1);
  run_steps(b, 20);
  CHECK(col().hash == plain);
  CHECK(col().total_events == n);
  CHECK(b.stats().lane_events == 0u);
}

TEST_CASE("lane: a note that runs over the end of the pattern is not cut by the loop") {
  setup();
  set_lane(1, 0, {note(15 * 48, 96, 61)});  // step 16, two steps long: its off falls in the second pass
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 20);
  NEED(col().count(EventKind::MidiNoteOn) == 1u);
  NEED(col().count(EventKind::MidiNoteOff) == 1u);
  CHECK(col().nth(EventKind::MidiNoteOn, 0)->t == step_time(15));
  CHECK(col().nth(EventKind::MidiNoteOff, 0)->t == step_time(17));
}

TEST_CASE("lane: a stop ends the notes that sound, and nothing plays after it") {
  setup();
  set_lane(1, 0, {note(0, 6000, 60), note(48, 6000, 64)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 3);
  CHECK(col().count(EventKind::MidiNoteOn) == 2u);
  CHECK(col().count(EventKind::MidiNoteOff) == 0u);
  s.stop();
  run_steps(s, 1);
  CHECK(col().count(EventKind::MidiNoteOff) == 2u);
  run_steps(s, 3);
  CHECK(col().count(EventKind::MidiNoteOn) == 2u);
}

TEST_CASE("lane: a jump (locate) ends the notes and plays the lane from the new step") {
  setup();
  set_lane(1, 0, {note(0, 6000, 60), note(8 * 48, 48, 70)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 3);
  CHECK(col().count(EventKind::MidiNoteOff) == 0u);
  s.locate(8);
  run_steps(s, 3);
  CHECK(col().nth(EventKind::MidiNoteOff, 0) != nullptr);  // the first note ended by the jump
  CHECK(col().nth(EventKind::MidiNoteOff, 0)->ev.a == 60);
  bool seventy = false;
  for (std::uint32_t i = 0; i < col().count(EventKind::MidiNoteOn); ++i) {
    seventy = seventy || col().nth(EventKind::MidiNoteOn, i)->ev.a == 70;
  }
  CHECK(seventy);
}

TEST_CASE("lane: a backward track reads its lane at the step it plays") {
  setup();
  set_lane(1, 0, {note(15 * 48, 24, 66)});  // the last step
  g_now = 0;
  seq::Sequencer& s = make();
  s.set_perf_track(0, 0, 0, seq::TrackPlayMode::Backward);
  s.play_pattern(1);
  run_steps(s, 3);
  NEED(col().count(EventKind::MidiNoteOn) >= 1u);
  CHECK(col().nth(EventKind::MidiNoteOn, 0)->t == step_time(0));  // backward: the last step plays first
}

TEST_CASE("lane: a muted track is silent and its notes end") {
  setup();
  set_lane(1, 0, {note(0, 6000, 60), note(4 * 48, 48, 62)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 2);
  CHECK(col().count(EventKind::MidiNoteOn) == 1u);
  seq_project().set_track_flags_plain(0, model::kTrackMute);
  run_steps(s, 6);
  CHECK(col().count(EventKind::MidiNoteOff) == 1u);  // the sounding note ended by the mute
  CHECK(col().count(EventKind::MidiNoteOn) == 1u);   // the one at step 5 did not play
}

TEST_CASE("lane: a repeat pitch ends the first, a ninth note ends the oldest") {
  setup();
  set_lane(1, 0, {note(0, 6000, 60), note(2, 6000, 60)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 1);
  NEED(col().count(EventKind::MidiNoteOn) == 2u);
  NEED(col().count(EventKind::MidiNoteOff) == 1u);
  CHECK(col().nth(EventKind::MidiNoteOff, 0)->t <= col().nth(EventKind::MidiNoteOn, 1)->t);

  setup();
  set_lane(1, 0, {note(0, 6000, 40), note(1, 6000, 41), note(2, 6000, 42), note(3, 6000, 43), note(4, 6000, 44), note(5, 6000, 45), note(6, 6000, 46), note(7, 6000, 47), note(8, 6000, 48)});
  g_now = 0;
  seq::Sequencer& t = make();
  t.play_pattern(1);
  run_steps(t, 1);
  NEED(col().count(EventKind::MidiNoteOn) == 9u);
  NEED(col().count(EventKind::MidiNoteOff) == 1u);
  CHECK(col().nth(EventKind::MidiNoteOff, 0)->ev.a == 40);
}

TEST_CASE("lane: a block handles 16 lane events at most, the rest waits for the next block") {
  setup();
  set_lane(1, 0, {poly(0, 1, 1), poly(0, 2, 1), poly(0, 3, 1), poly(0, 4, 1), poly(0, 5, 1), poly(0, 6, 1), poly(0, 7, 1), poly(0, 8, 1), poly(0, 9, 1), poly(0, 10, 1),
                  poly(0, 11, 1), poly(0, 12, 1), poly(0, 13, 1), poly(0, 14, 1), poly(0, 15, 1), poly(0, 16, 1), poly(0, 17, 1), poly(0, 18, 1), poly(0, 19, 1), poly(0, 20, 1)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_to(s, kBlock);  // the first block
  CHECK(col().count(EventKind::MidiPolyAt) == 16u);
  CHECK(s.stats().lane_deferred >= 1u);
  run_to(s, 4 * kBlock);
  CHECK(col().count(EventKind::MidiPolyAt) == 20u);
  for (std::uint32_t i = 0; i < 20; ++i) {  // in lane order
    CHECK(col().nth(EventKind::MidiPolyAt, i)->ev.a == i + 1);
  }
}

TEST_CASE("lane: control changes go out at 300 a second a port, the latest value of a controller wins") {
  setup();
  // ten changes of one controller in ten consecutive microsteps (about 4,600 samples)
  set_lane(1, 0, {cc(0, 74, 1), cc(1, 74, 2), cc(2, 74, 3), cc(3, 74, 4), cc(4, 74, 5), cc(5, 74, 6), cc(6, 74, 7), cc(7, 74, 8), cc(8, 74, 9), cc(9, 74, 10)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 1);
  const std::uint32_t n = col().count(EventKind::MidiCc);
  CHECK(n >= 2u);
  CHECK(n < 10u);
  CHECK(col().nth(EventKind::MidiCc, n - 1)->ev.b == 10);  // the last value is the one that stands
  CHECK(s.stats().lane_cc_merged >= 1u);
  for (std::uint32_t i = 1; i < n; ++i) {  // never closer than the gap on one port (147 samples)
    CHECK(col().nth(EventKind::MidiCc, i)->t - col().nth(EventKind::MidiCc, i - 1)->t >= 147u);
  }
}

TEST_CASE("lane: notes are not rate limited") {
  setup();
  set_lane(1, 0, {note(0, 1, 60), note(1, 1, 61), note(2, 1, 62), note(3, 1, 63)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 1);
  CHECK(col().count(EventKind::MidiNoteOn) == 4u);
  CHECK(col().count(EventKind::MidiNoteOff) == 4u);
}

TEST_CASE("lane: a start part way in chases the controller to the value of that position, once, and a plain loop does not") {
  setup();
  set_lane(1, 0, {cc(0, 7, 100), cc(2 * 48, 7, 50), cc(12 * 48, 7, 20)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1, 5);
  run_steps(s, 2);
  NEED(col().count(EventKind::MidiCc) == 1u);
  CHECK(col().nth(EventKind::MidiCc, 0)->ev.b == 50);  // the latest before step 5, not the first one
  CHECK(s.stats().lane_chase_sent == 1u);
  const std::uint32_t sent = s.stats().lane_chase_sent;
  run_steps(s, 20);  // over the loop end: the lane's own events play, no chase
  CHECK(s.stats().lane_chase_sent == sent);
}

TEST_CASE("lane: a chase skips a value the track has already sent") {
  setup();
  set_lane(1, 0, {cc(0, 7, 100), cc(8 * 48, 7, 100)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 2);
  CHECK(col().count(EventKind::MidiCc) == 1u);  // the one at tick 0
  s.locate(10);
  run_steps(s, 2);
  CHECK(col().count(EventKind::MidiCc) == 1u);  // the chase finds 100 again: equal to the last sent, not sent
}

TEST_CASE("lane: a chase puts no note out") {
  setup();
  set_lane(1, 0, {note(0, 6000, 60), cc(1, 7, 30)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1, 6);
  run_steps(s, 2);
  CHECK(col().count(EventKind::MidiNoteOn) == 0u);
  CHECK(col().count(EventKind::MidiCc) == 1u);
}

TEST_CASE("lane: a store mid-write (an odd seq) plays nothing, loses no note-off and plays the owed events late") {
  setup();
  set_lane(1, 0, {note(0, 96, 60), note(3 * 48, 48, 62)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 1);  // the first note sounds
  CHECK(col().count(EventKind::MidiNoteOn) == 1u);
  store().begin_write();
  run_steps(s, 4);  // the window covers the first note's end and the start of the second
  CHECK(col().count(EventKind::MidiNoteOff) == 1u);  // the note ended on time: its length is counted in the sequencer
  CHECK(col().nth(EventKind::MidiNoteOff, 0)->t == step_time(2));
  CHECK(col().count(EventKind::MidiNoteOn) == 1u);
  CHECK(s.stats().lane_odd > 0u);
  store().end_write();
  run_steps(s, 1);
  CHECK(col().count(EventKind::MidiNoteOn) == 2u);  // the owed event is played, late
  CHECK(col().nth(EventKind::MidiNoteOn, 1)->ev.a == 62);
}

TEST_CASE("lane: an edit of the store (even seq change) does not replay what already passed") {
  setup();
  set_lane(1, 0, {note(0, 6000, 60)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 4);
  CHECK(col().count(EventKind::MidiNoteOn) == 1u);
  set_lane(1, 0, {note(0, 6000, 60), note(2 * 48, 10, 65)});  // a take committed behind the playhead
  run_steps(s, 2);
  CHECK(col().count(EventKind::MidiNoteOn) == 1u);  // the new events are in the past: not played now
  run_steps(s, 14);  // the next pass plays them
  CHECK(col().count(EventKind::MidiNoteOn) >= 3u);
}

TEST_CASE("lane: a store of another project (the epoch moved) plays nothing") {
  setup();
  set_lane(1, 0, {note(0, 48, 60)});
  g_now = 0;
  seq::Sequencer& s = make();
  store().adopt(seq_project().load_epoch() + 1u);
  s.play_pattern(1);
  run_steps(s, 3);
  CHECK(col().count(EventKind::MidiNoteOn) == 0u);
}

TEST_CASE("lane: a pattern change ends the notes and reads the lane of the new pattern") {
  setup();
  CHECK(seq_project().create_pattern(2, 16) == model::Status::Ok);
  set_lane(1, 0, {note(0, 6000, 60)});
  set_lane(2, 0, {note(0, 48, 72)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 2);
  CHECK(col().count(EventKind::MidiNoteOn) == 1u);
  s.play_pattern(2);  // a switch
  run_steps(s, 4);
  bool off60 = false;
  bool on72 = false;
  for (std::uint32_t i = 0; i < col().count(EventKind::MidiNoteOff); ++i) {
    off60 = off60 || col().nth(EventKind::MidiNoteOff, i)->ev.a == 60;
  }
  for (std::uint32_t i = 0; i < col().count(EventKind::MidiNoteOn); ++i) {
    on72 = on72 || col().nth(EventKind::MidiNoteOn, i)->ev.a == 72;
  }
  CHECK(off60);
  CHECK(on72);
}

TEST_CASE("lane: a step CC and a lane CC of one tick: the lane's is last") {
  setup();
  set_lane(1, 0, {cc(0, 74, 33)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 1);
  NEED(col().count(EventKind::MidiCc) == 1u);
  CHECK(col().nth(EventKind::MidiCc, 0)->ev.a == (audio::kMidiCcExplicit | 74));
  CHECK(col().nth(EventKind::MidiCc, 0)->ev.b == 33);
}

TEST_CASE("lane: a chase after a pattern change is not blocked by the eight controllers of the pattern before") {
  setup();
  CHECK(seq_project().create_pattern(2, 16) == model::Status::Ok);
  set_lane(1, 0, {cc(0, 1, 11), cc(1, 2, 12), cc(2, 3, 13), cc(3, 4, 14), cc(4, 5, 15), cc(5, 6, 16), cc(6, 7, 17), cc(7, 8, 18)});  // fills the eight slots of the track
  set_lane(2, 0, {cc(0, 20, 60), cc(1, 21, 61)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 2);
  CHECK(col().count(EventKind::MidiCc) == 8u);
  s.play_pattern(2, 5);  // starts part way in: the chase finds CC20 and CC21 of the new pattern
  run_steps(s, 3);
  bool c20 = false;
  bool c21 = false;
  for (std::uint32_t i = 0; i < col().count(EventKind::MidiCc); ++i) {
    const Rec* r = col().nth(EventKind::MidiCc, i);
    c20 = c20 || (r->ev.a == (audio::kMidiCcExplicit | 20) && r->ev.b == 60);
    c21 = c21 || (r->ev.a == (audio::kMidiCcExplicit | 21) && r->ev.b == 61);
  }
  CHECK(c20);
  CHECK(c21);
}

TEST_CASE("lane: a chase keeps eight controllers a track, the ninth and tenth are left out") {
  setup();
  set_lane(1, 0, {cc(0, 1, 11), cc(1, 2, 12), cc(2, 3, 13), cc(3, 4, 14), cc(4, 5, 15), cc(5, 6, 16), cc(6, 7, 17), cc(7, 8, 18), cc(8, 9, 19), cc(9, 10, 20)});
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1, 5);
  run_steps(s, 3);
  CHECK(s.stats().lane_chase_sent == 8u);
  CHECK(col().count(EventKind::MidiCc) == 8u);
}

TEST_CASE("lane: a waiting table that is full drops the oldest control change and counts it") {
  setup();
  {  // 40 controllers on one tick: the port's gap lets one out per 147 samples, the table keeps 16
    LaneStore::WriteWindow w(store());
    LaneEvent* gap = store().open_gap(1, 0, 0, 40);
    NEED(gap != nullptr);
    for (std::uint8_t k = 0; k < 40; ++k) {
      gap[k] = cc(0, static_cast<std::uint8_t>(20 + k), static_cast<std::uint8_t>(k + 1));
    }
  }
  g_now = 0;
  seq::Sequencer& s = make();
  s.play_pattern(1);
  run_steps(s, 2);
  CHECK(s.stats().lane_cc_dropped > 0u);
  CHECK(col().count(EventKind::MidiCc) + s.stats().lane_cc_dropped == 40u);  // every change went out or was counted lost (no merge: all controllers differ)
}
