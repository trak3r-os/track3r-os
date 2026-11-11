// SPDX-License-Identifier: GPL-3.0-or-later
// OS-MIDIREC S1 + S2a: the event list model (model/event_list.h) — the pure half of the EVENTS list (S1: the
// order (A3), the ownership and the ends (A4, A5), LEN, the CC resolution (F3), CH and IN (A8, Q2 a), the
// scope (A2), the filter counts, the index/key round trip (3.2), the full 16 x 128 pattern's cell count and
// stack bound (A15)) and the edit planners (S2a: every rule of 3.5 and A6, A9, A10, A19-A21 with each refusal
// text, the F7 micro-move round trip, and the property test of I1-I3 over random patterns and edits).
#include "support/need.h"
#include "support/rng.h"

#include <o3os/app/rec_place.h>
#include <o3os/model/editor.h>
#include <o3os/model/event_list.h>
#include <o3os/model/fx_table.h>
#include <o3os/model/undo.h>

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::model;
using namespace o3os::model::events;

namespace {

// One shared fixture (static: the pattern and the instrument table stay out of the stack), reset per test case.
struct Fixture {
  static constexpr unsigned kSlots = 8;
  Pattern pat;
  Instrument ins[kSlots];
  MidiState midi;

  void reset() noexcept {
    std::memset(&pat, 0, sizeof pat);
    pat.id = 1;
    for (unsigned t = 0; t < kMaxTracks; ++t) {
      pat.track_length[t] = 16;
    }
    std::memset(ins, 0, sizeof ins);
    std::memset(&midi, 0, sizeof midi);
    for (unsigned i = 0; i < kMidiCcSlots; ++i) {
      midi.cc_number[i] = kMidiCcUnassigned;
    }
    midi.notes_channel = 0;  // the device's notes channel 1
  }

  Step& step(unsigned t, unsigned s) noexcept { return pat.steps[t][s]; }

  void set_midi(unsigned slot, std::uint8_t channel, const std::uint8_t cc[kMidiCcSlotsPerInstrument]) noexcept {
    init_instrument(ins[slot], InstrumentKind::Midi);
    ins[slot].midi_channel = channel;
    for (unsigned i = 0; i < kMidiCcSlotsPerInstrument; ++i) {
      ins[slot].midi_cc[i] = cc[i];
    }
  }
  void set_sample(unsigned slot) noexcept { init_instrument(ins[slot], InstrumentKind::Sample); }

  Env env() noexcept {
    Env e;
    e.midi = &midi;
    e.instrument = &instrument_of;
    e.instrument_ctx = this;
    return e;
  }
  static const Instrument* instrument_of(void* ctx, std::uint32_t slot) noexcept {
    const Fixture* f = static_cast<const Fixture*>(ctx);
    return slot < kSlots && f->ins[slot].kind != InstrumentKind::None ? &f->ins[slot] : nullptr;
  }
};
Fixture g_fx;  // the tests run in one binary, one case at a time

Step note_step(std::uint8_t note, std::uint8_t instr, std::uint8_t vol = kVolumeUnset) noexcept {
  Step s = kEmptyStep;
  s.note = note;
  s.instr = instr;
  s.vol = vol;
  return s;
}

std::vector<Row> all(const Pattern& pat, const Env& env, Scope scope, Filter f = Filter::All) {
  std::vector<Row> v;
  Projector p(pat, env, scope, f);
  Row r;
  while (p.next(r)) {
    v.push_back(r);
  }
  return v;
}

const Row* find(const std::vector<Row>& rows, const Key& k) noexcept {
  for (const Row& r : rows) {
    if (r.key == k) {
      return &r;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("event list: A1, the micro-move percent (TIME reads SSS.PP on the 48 microsteps)") {
  CHECK(micromove_start(0) == 1);    // no delay
  CHECK(micromove_start(50) == 25);  // value 50 plays at microstep 24 (0-based), which reads .50
  CHECK(micromove_start(100) == 47);
  CHECK(percent_of(0) == 0);
  CHECK(percent_of(24) == 50);
  CHECK(percent_of(46) == 96);
}

TEST_CASE("event list: order (A3): step, microstep, kind (CC before NOTE), track, FX column 2 before column 1") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  p.steps[0][2] = note_step(60, 1);
  p.steps[0][2].fx[0] = {8, 10};  // CC A in FX 1
  p.steps[0][2].fx[1] = {9, 20};  // CC B in FX 2
  p.steps[1][2] = note_step(65, 1);
  p.steps[0][3] = note_step(62, 1);
  p.steps[0][3].fx[0] = {10, 30};  // CC C
  p.steps[0][3].fx[1] = {kFxMicroMove, 50};  // Micro-move 50: the note starts at .50
  p.steps[1][3] = note_step(64, 1);

  const std::vector<Row> rows = all(p, g_fx.env(), scope_block(0, 15, 0, 1));
  NEED(rows.size() == 7);
  const Key want[7] = {
      {0, 2, Kind::Cc, 1}, {0, 2, Kind::Cc, 0}, {0, 2, Kind::Note, 0}, {1, 2, Kind::Note, 0},  // step 2: FX 2's CC first, then FX 1's, then the notes by track
      {0, 3, Kind::Cc, 0}, {1, 3, Kind::Note, 0},                                              // step 3 at .00
      {0, 3, Kind::Note, 0},                                                                   // the micro-moved note last, at .50
  };
  for (unsigned i = 0; i < 7; ++i) {
    CHECK(rows[i].key == want[i]);
  }
  CHECK(rows[6].microstep == 24);
  CHECK(rows[6].percent == 50);
  CHECK(rows[4].microstep == 0);  // a CC row goes out at the step's start (F6)
}

TEST_CASE("event list: ownership and ends (A4, A5): an OFF folded into the note above, an end row when nothing is above, CUT and FADE, FX on an empty step not listed") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][4].note = kNoteOff;   // the note's end: folded, not listed, it moves with LEN
  p.steps[0][6].fx[0] = {14, 47};  // a Chord on a step with nothing in its note column: no row (A4)
  p.steps[0][8].note = kNoteOff;   // nothing above it ends here: an OFF row of its own
  p.steps[0][10].note = kNoteCut;  // a CUT row
  p.steps[0][12].note = kNoteFade; // a FADE row
  p.steps[0][14].fx[1] = {8, 5};    // a CC on an empty step: listed

  const std::vector<Row> rows = all(p, g_fx.env(), scope_track(p, 0));
  NEED(rows.size() == 5);
  CHECK(rows[0].key == Key{0, 0, Kind::Note, 0});
  CHECK(rows[1].key == Key{0, 8, Kind::Off, 0});
  CHECK(rows[2].key == Key{0, 10, Kind::Cut, 0});
  CHECK(rows[3].key == Key{0, 12, Kind::Fade, 0});
  CHECK(rows[4].key == Key{0, 14, Kind::Cc, 1});
  CHECK(find(rows, Key{0, 4, Kind::Off, 0}) == nullptr);  // the folded end is no row
  CHECK(find(rows, Key{0, 6, Kind::Note, 0}) == nullptr); // the empty step with FX holds no row
  // the note owns its end: LEN reaches the OFF at step 4
  const Row& n = rows[0];
  CHECK(n.len_steps == 4);
  CHECK(n.len_percent == 0);
  CHECK_FALSE(n.len_runs_on);
  CHECK_FALSE(n.gated);
}

TEST_CASE("event list: LEN (A5): the next note ends it, '>' past the track's end, a Gate FX, an end with a Micro-move") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  for (unsigned t = 0; t < 5; ++t) {
    p.track_length[t] = 16;
  }
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][5] = note_step(62, 1);                        // the note at 0 ends here: LEN 05.00
  p.steps[1][10] = note_step(60, 1);                       // nothing below: '>' 06.00 (16 - 10)
  p.steps[2][12] = note_step(60, 1);
  p.steps[2][12].fx[0] = {kFxGate, 50};                    // gated: 50 % of one step = .50
  p.steps[3][2] = note_step(60, 1);
  p.steps[3][6].note = kNoteOff;
  p.steps[3][6].fx[1] = {kFxMicroMove, 50};                // the end starts at .50: LEN 04.50
  p.steps[4][3] = note_step(60, 1);
  p.steps[4][3].fx[0] = {kFxGate, 100};                    // gated to a whole step: 01.00

  const std::vector<Row> rows = all(p, g_fx.env(), scope_block(0, 15, 0, 4));
  const Row* n0 = find(rows, Key{0, 0, Kind::Note, 0});
  const Row* n1 = find(rows, Key{1, 10, Kind::Note, 0});
  const Row* n2 = find(rows, Key{2, 12, Kind::Note, 0});
  const Row* n3 = find(rows, Key{3, 2, Kind::Note, 0});
  const Row* n4 = find(rows, Key{4, 3, Kind::Note, 0});
  NEED(n0 != nullptr);
  NEED(n1 != nullptr);
  NEED(n2 != nullptr);
  NEED(n3 != nullptr);
  NEED(n4 != nullptr);
  CHECK(n0->len_steps == 5);
  CHECK(n0->len_percent == 0);
  CHECK_FALSE(n0->len_runs_on);
  CHECK(n1->len_runs_on);
  CHECK(n1->len_steps == 6);
  CHECK(n1->len_percent == 0);
  CHECK(n2->gated);
  CHECK(n2->len_steps == 0);
  CHECK(n2->len_percent == 50);
  CHECK_FALSE(n2->len_runs_on);
  CHECK(n3->len_steps == 4);
  CHECK(n3->len_percent == 50);
  CHECK(n4->gated);
  CHECK(n4->len_steps == 1);
  CHECK(n4->len_percent == 0);
}

TEST_CASE("event list: CC resolution (F3): controller, PROG, CHAT, PAT, the Config number, unassigned, a non-MIDI instrument") {
  g_fx.reset();
  const std::uint8_t cc[kMidiCcSlotsPerInstrument] = {75, kMidiCcProg, kMidiCcChanAt, kMidiCcPolyAt, kMidiCcConfig, kMidiCcConfig};
  g_fx.set_midi(0, 3, cc);    // slot 0: a MIDI instrument on channel 3, CC A = controller 74
  g_fx.set_sample(1);         // slot 1: a sample instrument
  g_fx.midi.cc_number[0] = 71;
  g_fx.midi.cc_number[4] = 71;
  g_fx.midi.notes_channel = 0;  // the device's notes channel 1

  Pattern& p = g_fx.pat;
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][0].fx[0] = {8, 10};    // CC A: controller 74 on channel 3
  p.steps[0][1].fx[0] = {9, 10};    // CC B: PROG
  p.steps[0][2].fx[0] = {10, 10};   // CC C: channel aftertouch
  p.steps[0][3].fx[0] = {11, 10};   // CC D: poly aftertouch
  p.steps[0][4].fx[0] = {12, 10};   // CC E: Config -> number 71, on the notes channel 1
  p.steps[0][5].fx[0] = {37, 10};   // CC F: Config -> unassigned: sends nothing
  p.steps[1][0] = note_step(60, 2);
  p.steps[1][0].fx[0] = {8, 10};    // a CC on a sample instrument: the Config number, the notes channel, dim

  const std::vector<Row> rows = all(p, g_fx.env(), scope_block(0, 15, 0, 1));
  const Row* a = find(rows, Key{0, 0, Kind::Cc, 0});
  const Row* b = find(rows, Key{0, 1, Kind::Cc, 0});
  const Row* c = find(rows, Key{0, 2, Kind::Cc, 0});
  const Row* d = find(rows, Key{0, 3, Kind::Cc, 0});
  const Row* e = find(rows, Key{0, 4, Kind::Cc, 0});
  const Row* f = find(rows, Key{0, 5, Kind::Cc, 0});
  const Row* s = find(rows, Key{1, 0, Kind::Cc, 0});
  NEED(a != nullptr);
  NEED(b != nullptr);
  NEED(c != nullptr);
  NEED(d != nullptr);
  NEED(e != nullptr);
  NEED(f != nullptr);
  NEED(s != nullptr);
  CHECK(a->cc_letter == 0);
  CHECK(a->cc.what == CcOut::What::Controller);
  CHECK(a->cc.number == 74);
  CHECK(a->cc.channel == 3);
  CHECK(a->channel == 3);
  CHECK(b->cc.what == CcOut::What::Program);
  CHECK(b->cc.channel == 3);
  CHECK(c->cc.what == CcOut::What::ChannelAt);
  CHECK(d->cc.what == CcOut::What::PolyAt);
  CHECK(e->cc.what == CcOut::What::Controller);
  CHECK(e->cc.number == 71);
  CHECK(e->cc.channel == 1);  // the Config path goes out on the device's notes channel
  CHECK(f->cc_letter == 5);
  CHECK(f->cc.what == CcOut::What::Unassigned);
  CHECK(f->cc.channel == 1);
  CHECK(s->cc.what == CcOut::What::Controller);
  CHECK(s->cc.number == 71);
  CHECK(s->cc.channel == 1);
  CHECK_FALSE(s->midi);  // the sample row dims (Q2 a)
  const Row* note = find(rows, Key{0, 0, Kind::Note, 0});
  NEED(note != nullptr);
  CHECK(note->midi);
  CHECK(note->channel == 3);
}

TEST_CASE("event list: CH and IN (A8, Q2 a): own, inherited (dim), '?' at the top, '--' for a non-MIDI instrument") {
  g_fx.reset();
  const std::uint8_t cc[kMidiCcSlotsPerInstrument] = {0, 0, 0, 0, 0, 0};
  g_fx.set_midi(0, 3, cc);
  g_fx.set_sample(1);
  Pattern& p = g_fx.pat;
  p.steps[0][0] = note_step(60, 0);  // no instrument above: unknown '?'
  p.steps[0][2] = note_step(62, 1);  // its own: slot 0, channel 3
  p.steps[0][4] = note_step(64, 0);  // inherited from step 2
  p.steps[1][0] = note_step(60, 2);  // a sample instrument

  const std::vector<Row> rows = all(p, g_fx.env(), scope_block(0, 15, 0, 1));
  const Row* top = find(rows, Key{0, 0, Kind::Note, 0});
  const Row* own = find(rows, Key{0, 2, Kind::Note, 0});
  const Row* inh = find(rows, Key{0, 4, Kind::Note, 0});
  const Row* smp = find(rows, Key{1, 0, Kind::Note, 0});
  NEED(top != nullptr);
  NEED(own != nullptr);
  NEED(inh != nullptr);
  NEED(smp != nullptr);
  CHECK(top->instr == 0);
  CHECK_FALSE(top->inherited);
  CHECK(top->channel == 0);  // '--'
  CHECK(own->instr == 1);
  CHECK_FALSE(own->inherited);
  CHECK(own->channel == 3);
  CHECK(inh->instr == 1);
  CHECK(inh->inherited);
  CHECK(inh->channel == 3);
  CHECK(smp->instr == 2);
  CHECK(smp->channel == 0);  // '--' when not MIDI (Q2 a)
  CHECK_FALSE(smp->midi);
}

TEST_CASE("event list: scope (A2): the track scope and its length, a block, a block clamped per track, a column block widened to whole steps") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  p.track_length[0] = 8;
  p.track_length[1] = 4;
  p.track_length[2] = 16;
  p.steps[0][3] = note_step(60, 1);
  p.steps[0][6] = note_step(62, 1);  // past track 0's length of 8? no: 6 < 8
  p.steps[0][7] = note_step(64, 1);  // the last step of track 0
  p.steps[1][3] = note_step(60, 1);
  p.steps[1][3].fx[1] = {8, 1};      // a CC on the same step
  p.steps[2][3] = note_step(60, 1);
  p.steps[2][9] = note_step(62, 1);

  // the track scope: every step of its length
  const Scope st = scope_track(p, 0);
  CHECK(st.track0 == 0);
  CHECK(st.track1 == 0);
  CHECK(st.row0 == 0);
  CHECK(st.row1 == 7);
  std::vector<Row> rows = all(p, g_fx.env(), st);
  NEED(rows.size() == 3);
  for (const Row& r : rows) {
    CHECK(r.key.track == 0);
  }

  // a track past the last is the last track, and the length is that track's
  p.track_length[kMaxTracks - 1u] = 12;
  const Scope so = scope_track(p, kMaxTracks + 83u);
  CHECK(so.track0 == kMaxTracks - 1u);
  CHECK(so.track1 == kMaxTracks - 1u);
  CHECK(so.row0 == 0);
  CHECK(so.row1 == 11);

  // a block: its tracks and rows, clamped to each track's length (track 1 is 4 long: only step 3)
  const Scope sb = scope_block(2, 9, 1, 2);
  CHECK(sb.track0 == 1);
  CHECK(sb.track1 == 2);
  CHECK(sb.row0 == 2);
  CHECK(sb.row1 == 9);
  rows = all(p, g_fx.env(), sb);
  NEED(rows.size() == 4);  // track 1 step 3 (note + CC, the track is 4 long), track 2 steps 3 and 9
  CHECK(find(rows, Key{1, 3, Kind::Note, 0}) != nullptr);
  CHECK(find(rows, Key{1, 3, Kind::Cc, 1}) != nullptr);
  CHECK(find(rows, Key{2, 3, Kind::Note, 0}) != nullptr);
  CHECK(find(rows, Key{2, 9, Kind::Note, 0}) != nullptr);
  CHECK(find(rows, Key{1, 0, Kind::Note, 0}) == nullptr);  // outside the block's rows
  CHECK(find(rows, Key{0, 3, Kind::Note, 0}) == nullptr);  // outside the block's tracks

  // a column block is widened (A2): a one-track block lists the CC rows of its steps too, whole steps
  rows = all(p, g_fx.env(), scope_block(3, 3, 1, 1));
  NEED(rows.size() == 2);
  CHECK(find(rows, Key{1, 3, Kind::Note, 0}) != nullptr);
  CHECK(find(rows, Key{1, 3, Kind::Cc, 1}) != nullptr);

  // the track scopes of tracks of different lengths
  CHECK(scope_track(p, 1).row1 == 3);
  CHECK(scope_track(p, 2).row1 == 15);
}

TEST_CASE("event list: the filter counts (3.2): ALL, NOTES, CC") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][4].note = kNoteOff;      // folded
  p.steps[0][8].note = kNoteOff;      // an end row
  p.steps[0][10] = note_step(62, 1);
  p.steps[0][2].fx[0] = {8, 1};       // CC A
  p.steps[0][2].fx[1] = {37, 2};      // CC F
  const Env e = g_fx.env();
  const Scope sc = scope_track(p, 0);
  CHECK(count_rows(p, e, sc, Filter::All) == 5);    // note, 2 CC, end row, note
  CHECK(count_rows(p, e, sc, Filter::Notes) == 3);  // the notes and the end row
  CHECK(count_rows(p, e, sc, Filter::Cc) == 2);
  const std::vector<Row> notes = all(p, e, sc, Filter::Notes);
  for (const Row& r : notes) {
    CHECK((r.key.kind == Kind::Note || is_end(r.key.kind)));
  }
}

TEST_CASE("event list: the index <-> key round trip (3.2)") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  p.track_length[1] = 8;
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][0].fx[0] = {kFxMicroMove, 50};
  p.steps[0][2].fx[1] = {9, 7};       // CC B
  p.steps[0][4].note = kNoteOff;      // folded into the note at 0
  p.steps[1][1] = note_step(62, 1);
  p.steps[1][3].fx[0] = {8, 3};       // CC A
  p.steps[2][0].note = kNoteCut;      // nothing above: a CUT row of its own
  const Env e = g_fx.env();
  const Scope sc = scope_block(0, 15, 0, 2);
  const std::vector<Row> rows = all(p, e, sc);
  const std::uint32_t n = count_rows(p, e, sc, Filter::All);
  NEED(n == rows.size());
  NEED(n > 0);
  for (std::uint32_t i = 0; i < n; ++i) {
    Key k;
    NEED(key_of_index(p, e, sc, Filter::All, i, k));
    CHECK(k == rows[i].key);
    CHECK(index_of_key(p, e, sc, Filter::All, k) == static_cast<std::int32_t>(i));
  }
  Key k;
  CHECK(index_of_key(p, e, sc, Filter::All, Key{5, 0, Kind::Note, 0}) == -1);  // a key that is no row
  CHECK_FALSE(key_of_index(p, e, sc, Filter::All, n, k));                      // past the end
}

TEST_CASE("event list: the full 16 x 128 pattern: 6,144 cells, 6,144 rows, inside the stack bound (A15)") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  for (unsigned t = 0; t < kMaxTracks; ++t) {
    p.track_length[t] = kMaxPatternLength;
    for (unsigned s = 0; s < kMaxPatternLength; ++s) {
      p.steps[t][s] = note_step(static_cast<std::uint8_t>(1 + (s % 128u)), 1, 100);
      p.steps[t][s].fx[0] = {8, 1};   // CC A
      p.steps[t][s].fx[1] = {9, 2};   // CC B
    }
  }
  const Env e = g_fx.env();
  const Scope sc = scope_block(0, 127, 0, 15);
  Projector proj(p, e, sc, Filter::All);
  Row r;
  std::uint32_t n = 0;
  while (proj.next(r)) {
    ++n;
  }
  CHECK(n == kMaxTracks * kMaxPatternLength * 3);  // 6,144 rows
  CHECK(proj.cells() == kMaxTracks * kMaxPatternLength * 3);
  CHECK(count_rows(p, e, sc, Filter::All) == 6144);
  CHECK(count_rows(p, e, sc, Filter::Notes) == 2048);
  CHECK(count_rows(p, e, sc, Filter::Cc) == 4096);
  CHECK(sizeof(Projector) <= kStackBound);
  CHECK(kStackBound == 256);
  // a window of the middle
  Row win[16];
  std::uint32_t total = 0;
  const std::uint32_t got = window(p, e, sc, Filter::All, 3000, win, 16, &total);
  CHECK(got == 16);
  CHECK(total == 6144);
  Key k;
  NEED(key_of_index(p, e, sc, Filter::All, 3000, k));
  CHECK(win[0].key == k);
}

// ---- S2a: the edit planners (3.5, A6, A9, A10, A19-A21) ---------------------------------------------------------------------

namespace {

// Apply a plan to a copy of the fixture's pattern: the plan plus the pattern it names.
struct Applied {
  Plan pl;
  Pattern after;
};

Applied run_plan(const Request& req, Scope scope, const edit::Last& last = edit::Last{}) {
  Applied a;
  a.pl = plan(g_fx.pat, g_fx.env(), scope, last, req);
  a.after = g_fx.pat;
  for (unsigned i = 0; i < a.pl.n; ++i) {
    a.after.steps[a.pl.writes[i].track][a.pl.writes[i].step] = a.pl.writes[i].value;
  }
  return a;
}

std::string step_holds(unsigned step1) {  // 1-based
  char b[32];
  std::snprintf(b, sizeof b, "STEP %03u HOLDS A NOTE", step1);
  return b;
}
std::string step_no_fx(unsigned step1) {
  char b[32];
  std::snprintf(b, sizeof b, "STEP %03u: NO FREE FX COLUMN", step1);
  return b;
}
std::string no_midi_ch(unsigned ch) {
  char b[32];
  std::snprintf(b, sizeof b, "NO MIDI INSTRUMENT ON CH %02u", ch);
  return b;
}
std::string ends_note_at(unsigned step1, unsigned percent) {
  char b[32];
  std::snprintf(b, sizeof b, "ENDS THE NOTE AT %03u.%02u", step1, percent);
  return b;
}

Request field_req(Field f, Key k, int delta, bool coarse = false) {
  Request r;
  r.op = Op::Edit;
  r.field = f;
  r.key = k;
  r.delta = delta;
  r.coarse = coarse;
  return r;
}

}  // namespace

TEST_CASE("event list S2a: F7 — micromove_start(micro_value(o)) - 1 == o for o in 0..46") {
  CHECK(micro_value(0) == 0u);
  for (int o = 0; o <= 46; ++o) {
    const unsigned v = micro_value(o);
    CHECK(v <= 100u);
    CHECK(static_cast<int>(micromove_start(static_cast<std::uint8_t>(v))) - 1 == o);
  }
  // offset 47 cannot be reached: the FX's top is 46 microsteps (F7)
  CHECK(micromove_start(static_cast<std::uint8_t>(micro_value(47))) - 1 == 46);
  // the copy cannot drift from the recording rule: the two formulas agree over the whole range (F7)
  for (int o = -2; o <= 60; ++o) {
    CHECK(events::micro_value(o) == app::rec::micro_value(o));
  }
}

TEST_CASE("event list S2a: A6 — a TIME microstep edit writes the Micro-move (FX 2 first, FX 1 else, remove at 0, refusal)") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key nk{0, 2, Kind::Note, 0};

  // a new Micro-move goes to FX 2
  p.steps[0][2] = note_step(60, 1);
  Applied a = run_plan(field_req(Field::Time, nk, 1), sc);
  NEED(a.pl.n == 1);
  CHECK(a.pl.text[0] == '\0');
  CHECK(a.pl.writes[0].track == 0);
  CHECK(a.pl.writes[0].step == 2);
  CHECK(a.after.steps[0][2].fx[1].type == kFxMicroMove);
  CHECK(a.after.steps[0][2].fx[1].value == micro_value(1));
  CHECK(a.after.steps[0][2].fx[0].type == kFxNone);
  CHECK(a.after.steps[0][2].note == 60);

  // back to .00 removes it (m02 00 is never written)
  p.steps[0][2].fx[1] = {kFxMicroMove, static_cast<std::uint8_t>(micro_value(1))};
  a = run_plan(field_req(Field::Time, nk, -1), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][2].fx[1].type == kFxNone);
  CHECK(a.after.steps[0][2].fx[1].value == 0);

  // FX 2 taken by another FX: the new Micro-move goes to FX 1
  p.steps[0][2] = note_step(60, 1);
  p.steps[0][2].fx[1] = {14, 47};  // a Chord
  a = run_plan(field_req(Field::Time, nk, 1), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][2].fx[0].type == kFxMicroMove);
  CHECK(a.after.steps[0][2].fx[1].type == 14);  // the other FX stays

  // both columns taken: the refusal
  p.steps[0][2].fx[0] = {14, 47};
  a = run_plan(field_req(Field::Time, nk, 1), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "NO FREE FX COLUMN FOR THE MICRO-MOVE");

  // an existing Micro-move keeps its column
  p.steps[0][2] = note_step(60, 1);
  p.steps[0][2].fx[0] = {kFxMicroMove, static_cast<std::uint8_t>(micro_value(3))};
  p.steps[0][2].fx[1] = {8, 10};  // CC A in FX 2: stays
  a = run_plan(field_req(Field::Time, nk, 1), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][2].fx[0].type == kFxMicroMove);
  CHECK(a.after.steps[0][2].fx[0].value == micro_value(4));
  CHECK(a.after.steps[0][2].fx[1].type == 8);
}

TEST_CASE("event list S2a: A21 — crossing .96 / .00 moves the note to the next / previous step at .00 / .96") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key nk{0, 2, Kind::Note, 0};

  // at .96 (offset 46), +1 microstep: step 3 at .00
  p.steps[0][2] = note_step(60, 1);
  p.steps[0][2].fx[1] = {kFxMicroMove, static_cast<std::uint8_t>(micro_value(46))};
  p.steps[0][4].note = kNoteOff;  // the note's end at step 4
  Applied a = run_plan(field_req(Field::Time, nk, 1), sc);
  NEED(a.pl.n == 4);
  CHECK(a.after.steps[0][2].note == kNoteEmpty);   // the source loses its owned bytes
  CHECK(a.after.steps[0][2].instr == 0);
  CHECK(a.after.steps[0][3].note == 60);            // the target takes the note
  CHECK(a.after.steps[0][3].fx[1].type == kFxNone); // offset 0: the Micro-move is removed
  CHECK(a.after.steps[0][4].note == kNoteEmpty);    // the old end leaves
  CHECK(a.after.steps[0][5].note == kNoteOff);      // the end moved by the same whole step

  // at .00, -1 microstep: step 1 at .96
  g_fx.reset();
  p.steps[0][2] = note_step(60, 1);
  a = run_plan(field_req(Field::Time, nk, -1), sc);
  NEED(a.pl.n == 2);
  CHECK(a.after.steps[0][2].note == kNoteEmpty);
  CHECK(a.after.steps[0][1].note == 60);
  CHECK(a.after.steps[0][1].fx[1].type == kFxMicroMove);
  CHECK(a.after.steps[0][1].fx[1].value == micro_value(46));
}

TEST_CASE("event list S2a: A21 — a whole-step move carries the note, its FX and its LEN; the CC rows stay; the refusals") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key nk{0, 0, Kind::Note, 0};
  p.steps[0][0] = note_step(60, 1, 100);
  p.steps[0][0].fx[0] = {8, 10};   // CC A: stays on step 0
  p.steps[0][0].fx[1] = {kFxMicroMove, static_cast<std::uint8_t>(micro_value(24))};  // carried, keeps FX 2
  p.steps[0][4].note = kNoteOff;   // the end
  p.steps[0][1].fx[0] = {14, 47};  // the target's own FX: the carried Micro-move takes the other column

  Applied a = run_plan(field_req(Field::Time, nk, 1, true), sc);  // Shift + encoder: a whole step
  NEED(a.pl.n == 4);
  CHECK(a.pl.text[0] == '\0');
  const Step& src = a.after.steps[0][0];
  CHECK(src.note == kNoteEmpty);
  CHECK(src.instr == 0);
  CHECK(src.vol == kVolumeUnset);
  CHECK(src.fx[0].type == 8);
  CHECK(src.fx[0].value == 10);  // the CC row stays on its step
  CHECK(src.fx[1].type == kFxNone);
  const Step& dst = a.after.steps[0][1];
  CHECK(dst.note == 60);
  CHECK(dst.instr == 1);
  CHECK(dst.vol == 100);
  CHECK(dst.fx[0].type == 14);
  CHECK(dst.fx[0].value == 47);  // the target's FX is untouched
  CHECK(dst.fx[1].type == kFxMicroMove);
  CHECK(dst.fx[1].value == micro_value(24));
  CHECK(a.after.steps[0][4].note == kNoteEmpty);
  CHECK(a.after.steps[0][5].note == kNoteOff);

  // the target holds a note: the refusal
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][2] = note_step(62, 1);
  a = run_plan(field_req(Field::Time, nk, 2, true), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == step_holds(3));

  // no free FX column for the FX it carries: the column message
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][0].fx[0] = {14, 47};  // a Chord travels with the note
  p.steps[0][2].fx[0] = {14, 1};
  p.steps[0][2].fx[1] = {15, 2};
  a = run_plan(field_req(Field::Time, nk, 2, true), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == step_no_fx(3));

  // out of the scope's rows: the scope refusal (A2)
  g_fx.reset();
  p.steps[0][3] = note_step(60, 1);
  a = run_plan(field_req(Field::Time, Key{0, 3, Kind::Note, 0}, 1, true), scope_block(0, 3, 0, 0));
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "OUTSIDE THE SCOPE");
}

TEST_CASE("event list S2a: A21 — a backward move with an adjacent end: the end lands on the vacated step, no step is written twice") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key nk{0, 2, Kind::Note, 0};
  p.steps[0][2] = note_step(60, 2, 100);
  p.steps[0][2].fx[0] = {14, 47};  // a Chord travels with the note
  p.steps[0][3].note = kNoteOff;   // the end, adjacent to the note
  p.steps[0][3].fx[1] = {kFxMicroMove, static_cast<std::uint8_t>(micro_value(24))};

  // one step back: the end moves onto the vacated source step (e2 == s), built from the cleared source
  Applied a = run_plan(field_req(Field::Time, nk, -1, true), sc);
  NEED(a.pl.n == 3);
  CHECK(a.pl.text[0] == '\0');
  for (unsigned i = 0; i < a.pl.n; ++i) {  // no step is written twice
    for (unsigned j = i + 1u; j < a.pl.n; ++j) {
      CHECK(!(a.pl.writes[i].track == a.pl.writes[j].track && a.pl.writes[i].step == a.pl.writes[j].step));
    }
  }
  CHECK(a.after.steps[0][1].note == 60);  // the note moved with its owned bytes
  CHECK(a.after.steps[0][1].instr == 2);
  CHECK(a.after.steps[0][1].vol == 100);
  CHECK(a.after.steps[0][1].fx[0].type == 14);
  CHECK(a.after.steps[0][1].fx[0].value == 47);
  CHECK(a.after.steps[0][2].note == kNoteOff);  // the end landed on the vacated step
  CHECK(a.after.steps[0][2].instr == 0);        // A4: the owned bytes left with the note
  CHECK(a.after.steps[0][2].vol == kVolumeUnset);
  CHECK(a.after.steps[0][2].fx[0].type == kFxNone);
  CHECK(a.after.steps[0][2].fx[1].type == kFxMicroMove);  // the end's Micro-move went with it (A20)
  CHECK(a.after.steps[0][2].fx[1].value == micro_value(24));
  CHECK(a.after.steps[0][3].note == kNoteEmpty);  // the old end left
  CHECK(a.after.steps[0][3].fx[1].type == kFxNone);

  // the end may not land on an orphan end row: the refusal
  g_fx.reset();
  p.steps[0][2] = note_step(60, 1);
  p.steps[0][4].note = kNoteOff;  // the note's end
  p.steps[0][5].note = kNoteCut;  // an orphan CUT row
  a = run_plan(field_req(Field::Time, nk, 1, true), sc);  // +1 step: the end would land on step 5, the CUT
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == step_holds(6));
}

TEST_CASE("event list S2a: A21 — a CC row moves by whole steps, keeps its column, else the other, else the refusal") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key ck{0, 2, Kind::Cc, 0};
  p.steps[0][2].fx[0] = {8, 10};  // CC A value 10 in FX 1

  // +1 step: keeps FX 1
  Applied a = run_plan(field_req(Field::Time, ck, 1), sc);
  NEED(a.pl.n == 2);
  CHECK(a.after.steps[0][2].fx[0].type == kFxNone);
  CHECK(a.after.steps[0][3].fx[0].type == 8);
  CHECK(a.after.steps[0][3].fx[0].value == 10);

  // the target's FX 1 is taken: it takes FX 2
  g_fx.reset();
  p.steps[0][2].fx[0] = {8, 10};
  p.steps[0][3].fx[0] = {9, 20};
  a = run_plan(field_req(Field::Time, ck, 1), sc);
  NEED(a.pl.n == 2);
  CHECK(a.after.steps[0][3].fx[0].type == 9);
  CHECK(a.after.steps[0][3].fx[1].type == 8);
  CHECK(a.after.steps[0][3].fx[1].value == 10);

  // both taken: the refusal
  g_fx.reset();
  p.steps[0][2].fx[0] = {8, 10};
  p.steps[0][3].fx[0] = {9, 20};
  p.steps[0][3].fx[1] = {10, 30};
  a = run_plan(field_req(Field::Time, ck, 1), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == step_no_fx(4));

  // out of the scope
  g_fx.reset();
  p.steps[0][2].fx[0] = {8, 10};
  a = run_plan(field_req(Field::Time, ck, 1), scope_block(0, 2, 0, 0));
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "OUTSIDE THE SCOPE");
}

TEST_CASE("event list S2a: an end row's TIME moves its note column and its Micro-move, without LEN") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key ok{0, 8, Kind::Off, 0};
  p.steps[0][8].note = kNoteOff;
  p.steps[0][8].fx[1] = {kFxMicroMove, static_cast<std::uint8_t>(micro_value(24))};

  // a microstep: only the Micro-move changes
  Applied a = run_plan(field_req(Field::Time, ok, 1), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][8].note == kNoteOff);
  CHECK(a.after.steps[0][8].fx[1].value == micro_value(25));

  // a whole step: the note column and the Micro-move move
  a = run_plan(field_req(Field::Time, ok, 1, true), sc);
  NEED(a.pl.n == 2);
  CHECK(a.after.steps[0][8].note == kNoteEmpty);
  CHECK(a.after.steps[0][8].fx[1].type == kFxNone);
  CHECK(a.after.steps[0][9].note == kNoteOff);
  CHECK(a.after.steps[0][9].fx[1].type == kFxMicroMove);
  CHECK(a.after.steps[0][9].fx[1].value == micro_value(24));

  // the target holds a note: the refusal
  g_fx.reset();
  p.steps[0][8].note = kNoteOff;
  p.steps[0][9] = note_step(60, 1);
  a = run_plan(field_req(Field::Time, ok, 1, true), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == step_holds(10));
}

TEST_CASE("event list S2a: TYPE cycles CC A..F (types 8..12 and 37), the value stays; CH is read-only on a CC row") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key ck{0, 2, Kind::Cc, 0};
  p.steps[0][2].fx[0] = {8, 42};
  Applied a = run_plan(field_req(Field::Type, ck, 1), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][2].fx[0].type == 9);
  CHECK(a.after.steps[0][2].fx[0].value == 42);
  a = run_plan(field_req(Field::Type, ck, 5), sc);  // A -> F (37)
  CHECK(a.after.steps[0][2].fx[0].type == 37);
  a = run_plan(field_req(Field::Type, ck, -1), sc);  // A -> F the other way
  CHECK(a.after.steps[0][2].fx[0].type == 37);
  p.steps[0][2].fx[0] = {37, 42};
  a = run_plan(field_req(Field::Type, ck, 1), sc);  // F -> A
  CHECK(a.after.steps[0][2].fx[0].type == 8);
  // TYPE on a NOTE row: '-' (3.5)
  a = run_plan(field_req(Field::Type, Key{0, 0, Kind::Note, 0}, 1), sc);
  p.steps[0][0] = note_step(60, 1);
  a = run_plan(field_req(Field::Type, Key{0, 0, Kind::Note, 0}, 1), sc);
  CHECK(a.pl.n == 0);
  CHECK(a.pl.text[0] == '\0');
  // CH on a CC row: read-only
  p.steps[0][2].fx[0] = {8, 42};
  a = run_plan(field_req(Field::Ch, ck, 1), sc);
  CHECK(a.pl.n == 0);
  CHECK(a.pl.text[0] == '\0');
}

TEST_CASE("event list S2a: CH (Q1 b) — the step's instrument becomes the MIDI instrument on that channel") {
  g_fx.reset();
  const std::uint8_t cc0[kMidiCcSlotsPerInstrument] = {0, 0, 0, 0, 0, 0};
  const std::uint8_t cc1[kMidiCcSlotsPerInstrument] = {0, 0, 0, 0, 0, 0};
  g_fx.set_midi(0, 3, cc0);   // slot 0: MIDI on channel 3
  g_fx.set_midi(2, 5, cc1);   // slot 2: MIDI on channel 5
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key nk{0, 0, Kind::Note, 0};
  p.steps[0][0] = note_step(60, 1);  // slot 0, channel 3

  // +2 channels: channel 5 -> the lowest slot that has it (slot 2 -> instr 3)
  Applied a = run_plan(field_req(Field::Ch, nk, 2), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][0].instr == 3);

  // channel 4 has no MIDI instrument: the refusal
  a = run_plan(field_req(Field::Ch, nk, 1), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == no_midi_ch(4));

  // the step's own instrument when it has the channel: channel 3 again is a no-op
  p.steps[0][0] = note_step(60, 3);  // slot 2, channel 5
  a = run_plan(field_req(Field::Ch, nk, -2), sc);  // back to channel 3 -> slot 0
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][0].instr == 1);
  a = run_plan(field_req(Field::Ch, nk, 16), sc);  // wraps to channel 3: the step's own (slot 0) keeps it
  CHECK(a.pl.n == 0);
  CHECK(a.pl.text[0] == '\0');
}

TEST_CASE("event list S2a: DATA (note_delta / note_set) and VAL (volume / fx value, the pad ramp)") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key nk{0, 0, Kind::Note, 0};
  const Key ck{0, 2, Kind::Cc, 0};
  p.steps[0][0] = note_step(60, 1);          // note byte 61, vol unset
  p.steps[0][2].fx[0] = {8, 10};             // CC A 10

  // DATA: a semitone, an octave (Shift), a pad's note (note byte 60 = og 59)
  Applied a = run_plan(field_req(Field::Data, nk, 1), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][0].note == 61);
  a = run_plan(field_req(Field::Data, nk, 1, true), sc);
  CHECK(a.after.steps[0][0].note == 60 + 12);
  Request pr;
  pr.op = Op::Edit;
  pr.field = Field::Data;
  pr.key = nk;
  pr.absolute = true;
  pr.value = 40;  // the model note byte the app resolved for the pad
  a = run_plan(pr, sc);
  CHECK(a.after.steps[0][0].note == 40);

  // VAL of a NOTE row: unset -> the first detent is 127 (A7), then down, 10 coarse, the pad ramp
  a = run_plan(field_req(Field::Val, nk, 1), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][0].vol == 127);
  p.steps[0][0].vol = 127;
  a = run_plan(field_req(Field::Val, nk, -1), sc);
  CHECK(a.after.steps[0][0].vol == 126);
  p.steps[0][0].vol = 100;
  a = run_plan(field_req(Field::Val, nk, 1, true), sc);
  CHECK(a.after.steps[0][0].vol == 110);
  Request pv = field_req(Field::Val, nk, 0);
  pv.pad = true;
  pv.value = 47;
  a = run_plan(pv, sc);
  CHECK(a.after.steps[0][0].vol == 127);
  pv.value = 0;
  a = run_plan(pv, sc);
  CHECK(a.after.steps[0][0].vol == 0);

  // VAL of a CC row: the FX value 0..127, delta / set / pad
  a = run_plan(field_req(Field::Val, ck, 1), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][2].fx[0].value == 11);
  Request pc = field_req(Field::Val, ck, 0);
  pc.absolute = true;
  pc.value = 100;
  a = run_plan(pc, sc);
  CHECK(a.after.steps[0][2].fx[0].value == 100);
  Request pp = field_req(Field::Val, ck, 0);
  pp.pad = true;
  pp.value = 47;
  a = run_plan(pp, sc);
  CHECK(a.after.steps[0][2].fx[0].value == 127);
  // VAL on an end row: '-'
  p.steps[0][5].note = kNoteOff;
  a = run_plan(field_req(Field::Val, Key{0, 5, Kind::Off, 0}, 1), sc);
  CHECK(a.pl.n == 0);
  CHECK(a.pl.text[0] == '\0');
}

TEST_CASE("event list S2a: LEN (A20) — the end moves by whole steps, keeps its kind, its Micro-move goes with it; A19's gate refusal") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key nk{0, 0, Kind::Note, 0};
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][4].note = kNoteCut;                          // the end: a CUT
  p.steps[0][4].fx[1] = {kFxMicroMove, static_cast<std::uint8_t>(micro_value(24))};

  // +1 step: the CUT and its Micro-move move to step 5
  Applied a = run_plan(field_req(Field::Len, nk, 1), sc);
  NEED(a.pl.n == 2);
  CHECK(a.after.steps[0][4].note == kNoteEmpty);
  CHECK(a.after.steps[0][4].fx[1].type == kFxNone);
  CHECK(a.after.steps[0][5].note == kNoteCut);
  CHECK(a.after.steps[0][5].fx[1].type == kFxMicroMove);
  CHECK(a.after.steps[0][5].fx[1].value == micro_value(24));

  // -1 step: back to step 3
  a = run_plan(field_req(Field::Len, nk, -1), sc);
  NEED(a.pl.n == 2);
  CHECK(a.after.steps[0][4].note == kNoteEmpty);
  CHECK(a.after.steps[0][3].note == kNoteCut);

  // past the next note: the end is removed, the note runs to the next note
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][2].note = kNoteOff;
  p.steps[0][3] = note_step(62, 1);
  a = run_plan(field_req(Field::Len, nk, 2), sc);  // e = 2, e1 = 4 >= the next note 3
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][2].note == kNoteEmpty);
  CHECK(a.after.steps[0][3].note == 62);

  // a new end (the note ran into a note) is an OFF with no Micro-move
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][3] = note_step(62, 1);
  a = run_plan(field_req(Field::Len, nk, -1), sc);  // e = 3 (a note), e1 = 2
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][2].note == kNoteOff);
  CHECK(a.after.steps[0][2].fx[0].type == kFxNone);
  CHECK(a.after.steps[0][2].fx[1].type == kFxNone);

  // the shortest LEN is the note's next step: clamped, no-op here
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][1].note = kNoteOff;
  a = run_plan(field_req(Field::Len, nk, -1), sc);
  CHECK(a.pl.n == 0);

  // the end may not land on an orphan end row: the refusal
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][2].note = kNoteOff;  // the note's end
  p.steps[0][4].note = kNoteCut;  // an orphan CUT row
  a = run_plan(field_req(Field::Len, nk, 2), sc);  // e1 = 4 holds the CUT
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == step_holds(5));
  // +1 step lands on a free step: the end moves there
  a = run_plan(field_req(Field::Len, nk, 1), sc);  // e1 = 3
  NEED(a.pl.n == 2);
  CHECK(a.after.steps[0][2].note == kNoteEmpty);
  CHECK(a.after.steps[0][3].note == kNoteOff);

  // A19: a gated note refuses
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][0].fx[0] = {kFxGate, 50};
  p.steps[0][4].note = kNoteOff;
  a = run_plan(field_req(Field::Len, nk, 1), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "THE GATE FX SETS THIS LENGTH");
}

TEST_CASE("event list S2a: IN — the grid's instrument walk (fine 5 places, Shift 8 places over the whole order)") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key nk{0, 0, Kind::Note, 0};
  p.steps[0][0] = note_step(60, 1);  // slot 0
  Applied a = run_plan(field_req(Field::In, nk, 1), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][0].instr == 2);  // the next slot in the walk
  a = run_plan(field_req(Field::In, nk, 1, true), sc);
  CHECK(a.after.steps[0][0].instr == 9);  // 8 places on
  // IN on a CC row: read-only
  p.steps[0][2].fx[0] = {8, 1};
  a = run_plan(field_req(Field::In, Key{0, 2, Kind::Cc, 0}, 1), sc);
  CHECK(a.pl.n == 0);
}

TEST_CASE("event list OS-EVFX: FX1 and FX2 — the grid's FX value walk on that slot, an empty slot takes the last used type, CC and end rows read-only") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  const Key nk{0, 0, Kind::Note, 0};
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][0].fx[0] = {kFxGate, 50};
  p.steps[0][0].fx[1] = {kFxMicroMove, static_cast<std::uint8_t>(micro_value(3))};
  p.steps[0][4].note = kNoteOff;

  // The reference is the grid's own routine, so the planner is one thin wrapper over it.
  const edit::Rule rule{true, true};
  for (unsigned slot = 0; slot < 2u; ++slot) {
    const Field f = slot == 0u ? Field::Fx1 : Field::Fx2;
    for (const int delta : {1, -1}) {
      for (const bool coarse : {false, true}) {
        Step want = p.steps[0][0];
        edit::Last l{};
        edit::fx_value_delta(want, slot, delta * (coarse ? 16 : 1), rule, l);
        const Applied a = run_plan(field_req(f, nk, delta, coarse), sc);
        NEED(a.pl.n == 1);
        CHECK(std::memcmp(&a.after.steps[0][0], &want, sizeof(Step)) == 0);
        CHECK(a.after.steps[0][0].fx[slot].type == p.steps[0][0].fx[slot].type);
        CHECK(a.after.steps[0][0].fx[1u - slot].value == p.steps[0][0].fx[1u - slot].value);  // the other slot is not touched
        CHECK(a.after.steps[0][0].note == 60);
      }
    }
  }
  // A plain detent moves the value by one.
  Applied a = run_plan(field_req(Field::Fx1, nk, 1), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][0].fx[0].value == 51);

  // An empty slot takes the last used type at its start value; with no last type it stays empty and plans nothing.
  p.steps[0][0].fx[0] = {kFxNone, 0};
  a = run_plan(field_req(Field::Fx1, nk, 1), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "NO FX TO REPEAT");
  a = run_plan(field_req(Field::Fx1, nk, 1), sc, edit::Last{1, fx::picker_index(kFxGate)});
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][0].fx[0].type == kFxGate);
  // a CC type as the last used one is refused: a NOTE row's FX slot does not carry a CC (A..E and F)
  for (const std::uint8_t cc : {std::uint8_t{kCcFirst}, std::uint8_t{kCcLast}, std::uint8_t{kCcF}}) {
    a = run_plan(field_req(Field::Fx1, nk, 1), sc, edit::Last{1, fx::picker_index(cc)});
    CHECK(a.pl.n == 0);
    CHECK(std::string(a.pl.text) == "A CC FX NEEDS A CC ROW");
    CHECK(std::memcmp(&a.after.steps[0][0], &p.steps[0][0], sizeof(Step)) == 0);
  }

  // The value clamps at the type's range ends.
  const fx::Row& gr = fx::row(kFxGate);
  p.steps[0][0].fx[0] = {kFxGate, gr.hi};
  a = run_plan(field_req(Field::Fx1, nk, 1), sc);
  CHECK(a.after.steps[0][0].fx[0].value == gr.hi);
  p.steps[0][0].fx[0] = {kFxGate, gr.lo};
  a = run_plan(field_req(Field::Fx1, nk, -1), sc);
  CHECK(a.after.steps[0][0].fx[0].value == gr.lo);

  // Chance takes its coarse walk (OS-CECHANCE C2), not a plain +16: 100 % goes on to the "with last" value.
  p.steps[0][0].fx[1] = {fx::kChanceType, fx::kChanceProbMax};
  a = run_plan(field_req(Field::Fx2, nk, 1, true), sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][0].fx[1].value == fx::kChanceWithLast);

  // The CC and end rows have no FX fields: nothing planned.
  p.steps[0][2].fx[0] = {8, 1};
  a = run_plan(field_req(Field::Fx1, Key{0, 2, Kind::Cc, 0}, 1), sc);
  CHECK(a.pl.n == 0);
  a = run_plan(field_req(Field::Fx2, Key{0, 4, Kind::Off, 0}, 1), sc);
  CHECK(a.pl.n == 0);
}

TEST_CASE("event list S2a: A9 Insert — on a NOTE row, on a CC row, Shift + Insert, the empty list, the refusals") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  p.steps[0][0] = note_step(60, 2, 100);
  p.steps[0][0].fx[1] = {kFxMicroMove, static_cast<std::uint8_t>(micro_value(24))};  // not copied
  p.steps[0][4].note = kNoteOff;  // the row's own end: the copy replaces it
  p.steps[0][4].fx[0] = {9, 20};  // a CC on the end step: it stays (it owns its column)

  Request ins;
  ins.op = Op::Insert;
  ins.key = Key{0, 0, Kind::Note, 0};
  Applied a = run_plan(ins, sc);
  NEED(a.pl.n == 1);
  CHECK(a.pl.writes[0].step == 4);
  CHECK(a.after.steps[0][4].note == 60);
  CHECK(a.after.steps[0][4].instr == 2);
  CHECK(a.after.steps[0][4].vol == 100);
  CHECK(a.after.steps[0][4].fx[0].type == 9);
  CHECK(a.after.steps[0][4].fx[0].value == 20);  // the CC stays
  CHECK(a.after.steps[0][4].fx[1].type == kFxNone);  // no Micro-move, no other FX
  CHECK(a.pl.has_new_key);
  CHECK(a.pl.new_key == Key{0, 4, Kind::Note, 0});

  // the row runs into the next note: the refusal
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][3] = note_step(62, 1);
  a = run_plan(ins, sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "NO FREE STEP AFTER THIS NOTE");

  // the note runs on past the track's end: the end lies outside: the refusal
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  a = run_plan(ins, sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "NO FREE STEP AFTER THIS NOTE");

  // the end below the block's rows (outside the scope): the refusal (A2)
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][4].note = kNoteOff;
  a = run_plan(ins, scope_block(0, 2, 0, 0));
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "NO FREE STEP AFTER THIS NOTE");

  // on a CC row: the same letter and value on the next step of the scope with a free FX column
  g_fx.reset();
  p.steps[0][2].fx[0] = {9, 33};
  p.steps[0][3].fx[0] = {8, 1};
  p.steps[0][3].fx[1] = {10, 2};  // step 3 is full: step 4 takes it
  Request ci;
  ci.op = Op::Insert;
  ci.key = Key{0, 2, Kind::Cc, 0};
  a = run_plan(ci, sc);
  NEED(a.pl.n == 1);
  CHECK(a.pl.writes[0].step == 4);
  CHECK(a.after.steps[0][4].fx[0].type == 9);
  CHECK(a.after.steps[0][4].fx[0].value == 33);
  CHECK(a.pl.new_key == Key{0, 4, Kind::Cc, 0});
  // no free column anywhere in the scope: the refusal
  g_fx.reset();
  p.track_length[0] = 4;
  for (unsigned s = 0; s < 4; ++s) {
    p.steps[0][s].fx[0] = {8, 1};
    p.steps[0][s].fx[1] = {9, 2};
  }
  a = run_plan(ci, sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "NO FREE STEP IN THE SCOPE");

  // Shift + Insert on a NOTE row: a CC A 000 in the first free FX column
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][0].fx[1] = {14, 47};
  Request si = ci;
  si.key = Key{0, 0, Kind::Note, 0};
  si.shift = true;
  a = run_plan(si, sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][0].fx[0].type == 8);
  CHECK(a.after.steps[0][0].fx[0].value == 0);
  CHECK(a.after.steps[0][0].fx[1].type == 14);
  CHECK(a.pl.new_key == Key{0, 0, Kind::Cc, 0});
  // both columns taken: the refusal
  p.steps[0][0].fx[0] = {9, 1};
  a = run_plan(si, sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == step_no_fx(1));

  // Shift + Insert on a CC row: a NOTE on the row's step when its note column is empty
  g_fx.reset();
  p.steps[0][2].fx[0] = {8, 1};
  Request sn = ci;
  sn.key = Key{0, 2, Kind::Cc, 0};
  sn.shift = true;
  a = run_plan(sn, sc, edit::Last{7, 0});
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][2].note == edit::kDefaultNote + 1);
  CHECK(a.after.steps[0][2].instr == 7);  // the last used instrument, as the pads enter them
  CHECK(a.pl.new_key == Key{0, 2, Kind::Note, 0});
  // the note column occupied: the refusal
  g_fx.reset();
  p.steps[0][2] = note_step(60, 1);
  p.steps[0][2].fx[0] = {8, 1};
  a = run_plan(sn, sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == step_holds(3));

  // an empty list: a NOTE (Shift: a CC A) at the scope's first step on the grid cursor's track
  g_fx.reset();
  Request ei;
  ei.op = Op::Insert;
  ei.has_key = false;
  ei.cursor_track = 2;
  a = run_plan(ei, scope_block(0, 15, 0, 3), edit::Last{5, 0});
  NEED(a.pl.n == 1);
  CHECK(a.pl.writes[0].track == 2);
  CHECK(a.pl.writes[0].step == 0);
  CHECK(a.after.steps[2][0].note == edit::kDefaultNote + 1);
  CHECK(a.after.steps[2][0].instr == 5);
  CHECK(a.pl.new_key == Key{2, 0, Kind::Note, 0});
  ei.shift = true;
  a = run_plan(ei, scope_block(0, 15, 0, 3));
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[2][0].fx[0].type == 8);
  CHECK(a.after.steps[2][0].fx[0].value == 0);
  CHECK(a.pl.new_key == Key{2, 0, Kind::Cc, 0});
}

TEST_CASE("event list S2a: A10 Delete — the owned bytes cleared (end included), the OFF for the note above, the CC column, the end row") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  Request del;
  del.op = Op::Delete;

  // a NOTE row: its owned bytes and its end are cleared; its CC stays
  p.steps[0][0] = note_step(60, 2, 100);
  p.steps[0][0].fx[0] = {8, 10};
  p.steps[0][0].fx[1] = {kFxMicroMove, static_cast<std::uint8_t>(micro_value(24))};
  p.steps[0][4].note = kNoteOff;
  p.steps[0][4].fx[1] = {kFxMicroMove, 50};
  del.key = Key{0, 0, Kind::Note, 0};
  Applied a = run_plan(del, sc);
  NEED(a.pl.n == 2);
  const Step& d = a.after.steps[0][0];
  CHECK(d.note == kNoteEmpty);
  CHECK(d.instr == 0);
  CHECK(d.vol == kVolumeUnset);
  CHECK(d.fx[0].type == 8);
  CHECK(d.fx[0].value == 10);  // the CC column stays
  CHECK(d.fx[1].type == kFxNone);
  CHECK(a.after.steps[0][4].note == kNoteEmpty);
  CHECK(a.after.steps[0][4].fx[1].type == kFxNone);

  // the deleted note was the end of the note above: an OFF is written at its time (I2)
  g_fx.reset();
  p.steps[0][2] = note_step(60, 1);
  p.steps[0][6] = note_step(62, 1);
  del.key = Key{0, 6, Kind::Note, 0};
  a = run_plan(del, sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][6].note == kNoteOff);
  CHECK(a.after.steps[0][6].instr == 0);
  CHECK(a.after.steps[0][2].note == 60);  // the note above keeps its LEN

  // a CC row: its FX column is cleared
  g_fx.reset();
  p.steps[0][2].fx[0] = {8, 10};
  p.steps[0][2].fx[1] = {9, 20};
  del.key = Key{0, 2, Kind::Cc, 0};
  a = run_plan(del, sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][2].fx[0].type == kFxNone);
  CHECK(a.after.steps[0][2].fx[1].type == 9);

  // an end row: its note column and Micro-move are cleared
  g_fx.reset();
  p.steps[0][8].note = kNoteCut;
  p.steps[0][8].fx[1] = {kFxMicroMove, 50};
  del.key = Key{0, 8, Kind::Cut, 0};
  a = run_plan(del, sc);
  NEED(a.pl.n == 1);
  CHECK(a.after.steps[0][8].note == kNoteEmpty);
  CHECK(a.after.steps[0][8].fx[1].type == kFxNone);
}

TEST_CASE("event list S2a: A11's one-voice notice — a note placed inside another note's span ends it there") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  const Scope sc = scope_track(p, 0);
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][10].note = kNoteOff;  // the note at 0 ends there
  p.steps[0][12] = note_step(64, 1);
  // move the note at 12 up to step 5: the note at 0 sounds there, so it ends at 006.00
  Applied a = run_plan(field_req(Field::Time, Key{0, 12, Kind::Note, 0}, -7, true), sc);
  NEED(a.pl.n > 0);
  CHECK(std::string(a.pl.text) == ends_note_at(6, 0));
  CHECK(a.after.steps[0][5].note == 64);
  // Shift + Insert's note inside a span says so too
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][8].note = kNoteOff;
  p.steps[0][4].fx[0] = {8, 1};  // a CC row at step 4, inside the note's span
  Request sn;
  sn.op = Op::Insert;
  sn.key = Key{0, 4, Kind::Cc, 0};
  sn.shift = true;
  a = run_plan(sn, sc);
  NEED(a.pl.n == 1);
  CHECK(std::string(a.pl.text) == ends_note_at(5, 0));

  // a moved note that was the end of the note above leaves an OFF there: that note keeps its LEN (I2, as A10)
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][4] = note_step(62, 1);  // the note at 0 ends here (one voice)
  p.steps[0][8].note = kNoteOff;    // the note at 4's own end
  a = run_plan(field_req(Field::Time, Key{0, 4, Kind::Note, 0}, 2, true), sc);  // move 4 -> 6
  NEED(a.pl.n == 4);
  CHECK(a.after.steps[0][4].note == kNoteOff);  // the OFF, not empty: the note at 0 keeps its LEN
  CHECK(a.after.steps[0][6].note == 62);
  CHECK(a.after.steps[0][8].note == kNoteEmpty);
  CHECK(a.after.steps[0][10].note == kNoteOff);  // the end moved by the same two steps
  CHECK(a.pl.ends_note == false);

  // the moved end landing inside another note's span ends that note there: the notice names the end's step
  g_fx.reset();
  p.steps[0][0] = note_step(60, 1);
  p.steps[0][2].note = kNoteOff;   // the note at 0's end
  p.steps[0][5] = note_step(64, 1);
  p.steps[0][10].note = kNoteOff;  // the note at 5's end
  a = run_plan(field_req(Field::Time, Key{0, 0, Kind::Note, 0}, 4, true), sc);  // move 0 -> 4
  NEED(a.pl.n == 4);
  CHECK(a.after.steps[0][4].note == 60);
  CHECK(a.after.steps[0][6].note == kNoteOff);  // the end moved to 6, inside the note at 5's span
  CHECK(std::string(a.pl.text) == ends_note_at(7, 0));
  CHECK(a.pl.ends_note);
}

// ---- S2a: the property test — random patterns and edits through a real Editor, I1, I2, I3 ------------------------

namespace {

model::ModelArena g_arena;
alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

struct NoteRow {
  std::uint8_t track, step;
  std::uint16_t len_steps;
  std::uint8_t len_percent;
  bool runs_on, gated;
};

std::vector<NoteRow> note_rows_of(const Pattern& pat, const Env& env, Scope scope) {
  std::vector<NoteRow> out;
  Projector p(pat, env, scope, Filter::All);
  Row r;
  while (p.next(r)) {
    if (r.key.kind == Kind::Note) {
      out.push_back(NoteRow{r.key.track, r.key.step, r.len_steps, r.len_percent, r.len_runs_on, r.gated});
    }
  }
  return out;
}

bool is_known_refusal(const char* t) {
  static const char* kFixed[] = {"NO FREE FX COLUMN FOR THE MICRO-MOVE", "NO FREE STEP AFTER THIS NOTE",
                                 "THE GATE FX SETS THIS LENGTH", "OUTSIDE THE SCOPE", "NO FREE STEP IN THE SCOPE", "NO FX TO REPEAT",
                                 "A CC FX NEEDS A CC ROW"};
  for (const char* f : kFixed) {
    if (std::strcmp(t, f) == 0) {
      return true;
    }
  }
  unsigned step1 = 0, pct = 0;
  if (std::sscanf(t, "STEP %03u HOLDS A NOTE", &step1) == 1 && step1 >= 1 && step1 <= 128) {
    return true;
  }
  if (std::sscanf(t, "STEP %03u: NO FREE FX COLUMN", &step1) == 1 && step1 >= 1 && step1 <= 128) {
    return true;
  }
  unsigned ch = 0;
  if (std::sscanf(t, "NO MIDI INSTRUMENT ON CH %02u", &ch) == 1 && ch >= 1 && ch <= 16) {
    return true;
  }
  if (std::sscanf(t, "ENDS THE NOTE AT %03u.%02u", &step1, &pct) == 2 && step1 >= 1 && step1 <= 128 && pct <= 96) {
    return true;
  }
  return false;
}

Step random_step(test::Rng& rng) {
  Step s = kEmptyStep;
  const unsigned what = rng.below(10);
  if (what < 4) {
    s.note = static_cast<std::uint8_t>(1 + rng.below(128));  // a note
  } else if (what < 5) {
    s.note = static_cast<std::uint8_t>(253 + rng.below(3));  // an end
  }
  s.instr = static_cast<std::uint8_t>(rng.below(3));  // 0 (inherits), 1, 2
  s.vol = rng.below(2) == 0 ? kVolumeUnset : static_cast<std::uint8_t>(rng.below(128));
  for (unsigned c = 0; c < 2u; ++c) {
    const unsigned fx = rng.below(10);
    if (fx == 0) {
      s.fx[c] = {kFxNone, 0};
    } else if (fx < 4) {
      s.fx[c] = {static_cast<std::uint8_t>(rng.below(5) == 0 ? 37 : 8 + rng.below(5)), rng.byte()};  // a CC
    } else if (fx == 4) {
      s.fx[c] = {kFxMicroMove, rng.byte()};
    } else if (fx == 5) {
      s.fx[c] = {kFxGate, rng.byte()};
    } else {
      s.fx[c] = {14, rng.byte()};  // another FX (a Chord)
    }
  }
  return s;
}

}  // namespace

TEST_CASE("event list S2a: the property test — random patterns and edits, I1 (only the planned steps differ), I2 (no other note's start or LEN, unless the one-voice notice), I3 (undo is byte-identical)") {
  test::Rng rng(0x5EED1234u);
  UndoJournal journal;
  NEED(journal.begin(g_ring, model::kUndoRegionBytes));
  constexpr unsigned kEpisodes = 250;
  for (unsigned episode = 0; episode < kEpisodes; ++episode) {
    Project& proj = g_arena.project;
    proj.reset();
    Editor ed(proj, journal);
    NEED(ed.create_pattern(1, 16) == Status::Ok);
    Instrument in;
    init_instrument(in, InstrumentKind::Sample);
    NEED(ed.set_instrument(0, in) == Status::Ok);
    init_instrument(in, InstrumentKind::Midi);
    in.midi_channel = 3;
    NEED(ed.set_instrument(1, in) == Status::Ok);
    // a random pattern on tracks 0..2, short lengths
    static Pattern pat;  // static, as the four below: five patterns in this frame overflow the qemu stack
    std::memset(&pat, 0, sizeof pat);
    pat.id = 1;
    std::uint8_t len[3];
    for (unsigned t = 0; t < 3; ++t) {
      len[t] = static_cast<std::uint8_t>(4 + rng.below(13));
      pat.track_length[t] = len[t];
      for (unsigned s = 0; s < kMaxPatternLength; ++s) {
        pat.steps[t][s] = s < len[t] ? random_step(rng) : kEmptyStep;
      }
    }
    for (unsigned t = 0; t < 3; ++t) {
      NEED(ed.set_track_length(1, t, len[t]) == Status::Ok);
      NEED(ed.set_steps(1, t, 0, pat.steps[t], kMaxPatternLength) == Status::Ok);
    }
    ed.history_reset();  // the setup is not under test
    static Pattern before;
    before = *proj.pattern(1);
    const Env env = env_of(proj);
    const Scope scope = rng.below(2) == 0 ? scope_track(before, rng.below(3))
                                           : scope_block(0, static_cast<std::int16_t>(rng.below(16)), 0, 2);
    const std::vector<Row> rows = all(before, env, scope);
    edit::Last last{static_cast<std::uint8_t>(1 + rng.below(2)), 0};

    Request req;
    if (rows.empty()) {
      // an empty list: Insert adds one (A9)
      req.op = Op::Insert;
      req.has_key = false;
      req.cursor_track = static_cast<std::uint8_t>(rng.below(3));
      req.shift = rng.below(2) == 0;
    } else {
      const unsigned opick = rng.below(10);
      req.op = opick < 2 ? Op::Insert : (opick < 4 ? Op::Delete : Op::Edit);
      req.key = rows[rng.below(static_cast<std::uint32_t>(rows.size()))].key;
      req.has_key = true;
      if (req.op == Op::Insert) {
        req.shift = rng.below(2) == 0;
        if (rng.below(6) == 0) {
          req.has_key = false;  // an empty list
          req.cursor_track = static_cast<std::uint8_t>(rng.below(3));
        }
      }
    }
    if (req.op == Op::Edit) {
      const Kind kind = req.key.kind;
      const Field fields[] = {Field::Time, Field::Ch, Field::Data, Field::Val, Field::Len, Field::In, Field::Fx1, Field::Fx2};
      const Field cfields[] = {Field::Time, Field::Type, Field::Val};
      if (kind == Kind::Note) {
        req.field = fields[rng.below(8)];
      } else if (kind == Kind::Cc) {
        req.field = cfields[rng.below(3)];
      } else {
        req.field = Field::Time;
      }
      req.delta = static_cast<int>(rng.below(5)) - 2;
      req.coarse = rng.below(4) == 0;
      const unsigned mode = rng.below(8);
      if (mode == 0) {
        req.absolute = true;
        req.value = 1 + rng.below(128);
      } else if (mode == 1) {
        req.pad = true;
        req.value = rng.below(48);
      }
    }

    const Plan pl = plan(before, env, scope, last, req);
    CHECK(pl.n <= kMaxPlanWrites);
    if (pl.text[0] != '\0') {
      CHECK(is_known_refusal(pl.text));  // every refusal is one of the spec's texts
    }
    if (pl.n == 0 && pl.text[0] == '\0') {
      continue;  // a no-op edit
    }

    // I1: applied to a copy, only the planned steps differ
    static Pattern after;
    after = before;
    bool written[kMaxTracks][kMaxPatternLength];
    std::memset(written, 0, sizeof written);
    for (unsigned i = 0; i < pl.n; ++i) {
      const StepWrite& w = pl.writes[i];
      CHECK(w.track < kMaxTracks);
      CHECK(w.step < kMaxPatternLength);
      CHECK(!written[w.track][w.step]);  // no step is written twice
      written[w.track][w.step] = true;
      after.steps[w.track][w.step] = w.value;
    }
    for (unsigned t = 0; t < kMaxTracks; ++t) {
      for (unsigned s = 0; s < kMaxPatternLength; ++s) {
        if (!written[t][s]) {
          CHECK(std::memcmp(&after.steps[t][s], &before.steps[t][s], sizeof(Step)) == 0);
        }
      }
    }

    // I2: no other note's start or LEN changes, unless the one-voice notice says so (A11)
    const std::vector<NoteRow> nb = note_rows_of(before, env, scope);
    const std::vector<NoteRow> na = note_rows_of(after, env, scope);
    const std::uint8_t edited_track = req.has_key ? req.key.track : req.cursor_track;
    for (const NoteRow& b : nb) {
      bool found = false;
      for (const NoteRow& a2 : na) {
        if (a2.track == b.track && a2.step == b.step) {
          found = true;
          // I2 protects every note but the edited row: its own LEN may change (a TIME microstep or a LEN edit)
          const bool edited_row = req.has_key && b.track == req.key.track && b.step == req.key.step;
          if (!edited_row && (!pl.ends_note || b.track != edited_track)) {
            CHECK(a2.len_steps == b.len_steps);
            CHECK(a2.len_percent == b.len_percent);
            CHECK(a2.runs_on == b.runs_on);
            CHECK(a2.gated == b.gated);
          }
          break;
        }
      }
      if (!found) {
        // the start went away: it must be a planned step (the edited row's own move, a delete, an insert's target)
        CHECK(written[b.track][b.step]);
      }
    }
    for (const NoteRow& a2 : na) {
      bool found = false;
      for (const NoteRow& b : nb) {
        if (b.track == a2.track && b.step == a2.step) {
          found = true;
          break;
        }
      }
      if (!found) {
        CHECK(written[a2.track][a2.step]);  // a new start only on a planned step
      }
    }

    // I3: apply through the Editor as one unit, undo: the pattern is byte-identical
    if (pl.n > 0) {
      ed.begin_group(core::ActionId::None, 0, UndoArea::Pattern);
      for (unsigned i = 0; i < pl.n; ++i) {
        CHECK(ed.set_step(1, pl.writes[i].track, pl.writes[i].step, pl.writes[i].value) == Status::Ok);
      }
      ed.end_group();
      static Pattern mid;
      mid = *proj.pattern(1);
      for (unsigned t = 0; t < kMaxTracks; ++t) {
        for (unsigned s = 0; s < kMaxPatternLength; ++s) {
          if (!written[t][s]) {
            CHECK(std::memcmp(&mid.steps[t][s], &before.steps[t][s], sizeof(Step)) == 0);
          }
        }
      }
      CHECK(ed.undo(UndoArea::Pattern) == UndoResult::Ok);
      static Pattern undone;
      undone = *proj.pattern(1);
      CHECK(std::memcmp(&undone, &before, sizeof(Pattern)) == 0);
    }
  }
}

// ---- S3: QUANT (the row, every row; QA-PAT-426..428) --------------------------------------------------------------------------------------

namespace {

Request quant_req(Key k) {
  Request r;
  r.op = Op::Quant;
  r.key = k;
  return r;
}

StepFx micro_fx(unsigned offset) { return StepFx{kFxMicroMove, static_cast<std::uint8_t>(micro_value(static_cast<int>(offset)))}; }

// quant_all's apply into the fixture's pattern.
bool apply_to_fixture(void*, const Plan& pl) noexcept {
  for (unsigned i = 0; i < pl.n; ++i) {
    g_fx.pat.steps[pl.writes[i].track][pl.writes[i].step] = pl.writes[i].value;
  }
  return true;
}

std::string quant_text(const QuantResult& r) {
  char b[64];
  quant_message(r, b, sizeof b);
  return b;
}

}  // namespace

TEST_CASE("event list S3: QUANT of a row — under 24 microsteps snaps back to .00 (the Micro-move goes), 24 or more moves to the next step at .00 (the boundary)") {
  g_fx.reset();
  const Scope sc = scope_track(g_fx.pat, 0);
  const Key nk{0, 2, Kind::Note, 0};
  for (const unsigned off : {1u, 12u, 23u}) {
    g_fx.reset();
    g_fx.pat.steps[0][2] = note_step(60, 1, 100);
    g_fx.pat.steps[0][2].fx[1] = micro_fx(off);
    const Applied a = run_plan(quant_req(nk), sc);
    NEED(a.pl.n == 1);
    CHECK(a.pl.text[0] == '\0');
    CHECK(a.after.steps[0][2].note == 60);
    CHECK(a.after.steps[0][2].vol == 100);
    CHECK(a.after.steps[0][2].fx[1].type == kFxNone);  // the Micro-move is removed, never m02 00
  }
  for (const unsigned off : {24u, 30u, 46u}) {
    g_fx.reset();
    g_fx.pat.steps[0][2] = note_step(60, 1, 100);
    g_fx.pat.steps[0][2].fx[1] = micro_fx(off);
    const Applied a = run_plan(quant_req(nk), sc);
    NEED(a.pl.n == 2);
    CHECK(a.after.steps[0][2].note == kNoteEmpty);
    CHECK(a.after.steps[0][3].note == 60);
    CHECK(a.after.steps[0][3].vol == 100);
    CHECK(a.after.steps[0][3].fx[1].type == kFxNone);
    CHECK(a.after.steps[0][3].fx[0].type == kFxNone);
  }
  // the boundary reads the offset the Micro-move plays at, so the value the list wrote for 23 and for 24 are on each side of it
  CHECK(micromove_start(micro_value(23)) - 1u == 23u);
  CHECK(micromove_start(micro_value(24)) - 1u == 24u);
}

TEST_CASE("event list S3: QUANT of a row — the note takes its end whole steps with it; an end row of its own is quantised as a start") {
  g_fx.reset();
  const Scope sc = scope_track(g_fx.pat, 0);
  g_fx.pat.steps[0][0] = note_step(60, 1);
  g_fx.pat.steps[0][0].fx[1] = micro_fx(30);
  g_fx.pat.steps[0][4].note = kNoteOff;
  g_fx.pat.steps[0][4].fx[0] = micro_fx(10);  // the end keeps its Micro-move: starts only are quantised
  Applied a = run_plan(quant_req(Key{0, 0, Kind::Note, 0}), sc);
  NEED(a.pl.text[0] == '\0');
  CHECK(a.after.steps[0][0].note == kNoteEmpty);
  CHECK(a.after.steps[0][1].note == 60);
  CHECK(a.after.steps[0][1].fx[1].type == kFxNone);
  CHECK(a.after.steps[0][4].note == kNoteEmpty);
  CHECK(a.after.steps[0][5].note == kNoteOff);
  CHECK(a.after.steps[0][5].fx[1].type == kFxMicroMove);  // a new Micro-move goes to FX 2 (A6)
  CHECK(a.after.steps[0][5].fx[1].value == micro_value(10));
  // an OFF that ends no note above it is a row of its own: it goes to the next step at .00
  g_fx.reset();
  g_fx.pat.steps[0][3].note = kNoteOff;
  g_fx.pat.steps[0][3].fx[1] = micro_fx(30);
  a = run_plan(quant_req(Key{0, 3, Kind::Off, 0}), sc);
  NEED(a.pl.text[0] == '\0');
  CHECK(a.after.steps[0][3].note == kNoteEmpty);
  CHECK(a.after.steps[0][4].note == kNoteOff);
  CHECK(a.after.steps[0][4].fx[1].type == kFxNone);
}

TEST_CASE("event list S3: QUANT of a row — a refused move leaves the row as it is; a CC row and a row on its step say so") {
  g_fx.reset();
  const Scope sc = scope_track(g_fx.pat, 0);
  g_fx.pat.steps[0][2] = note_step(60, 1);
  g_fx.pat.steps[0][2].fx[1] = micro_fx(30);
  g_fx.pat.steps[0][3] = note_step(62, 1);  // the next step is taken
  Applied a = run_plan(quant_req(Key{0, 2, Kind::Note, 0}), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == step_holds(4));
  // out of the scope's rows
  a = run_plan(quant_req(Key{0, 2, Kind::Note, 0}), scope_block(0, 2, 0, 0));
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "OUTSIDE THE SCOPE");
  // a Chord that travels with the note finds no free FX column on the next step
  g_fx.reset();
  g_fx.pat.steps[0][2] = note_step(60, 1);
  g_fx.pat.steps[0][2].fx[0] = {14, 47};
  g_fx.pat.steps[0][2].fx[1] = micro_fx(30);
  g_fx.pat.steps[0][3].fx[0] = {14, 1};
  g_fx.pat.steps[0][3].fx[1] = {15, 2};
  a = run_plan(quant_req(Key{0, 2, Kind::Note, 0}), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == step_no_fx(4));
  // a CC row is on the step; a row on its step has nothing to do
  g_fx.reset();
  g_fx.pat.steps[0][1].fx[0] = {8, 10};
  g_fx.pat.steps[0][5] = note_step(60, 1);
  a = run_plan(quant_req(Key{0, 1, Kind::Cc, 0}), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "CC IS ON THE STEP");
  a = run_plan(quant_req(Key{0, 5, Kind::Note, 0}), sc);
  CHECK(a.pl.n == 0);
  CHECK(std::string(a.pl.text) == "ALREADY ON THE STEP");
  // a stale key plans nothing
  a = run_plan(quant_req(Key{0, 9, Kind::Note, 0}), sc);
  CHECK(a.pl.n == 0);
  CHECK(a.pl.text[0] == '\0');
}

TEST_CASE("event list S3: quant_all — every NOTE and end row, the steps walked from the last back, the CC rows and the rows on a step not counted, the strip text") {
  g_fx.reset();
  const Scope sc = scope_track(g_fx.pat, 0);
  Pattern& p = g_fx.pat;
  p.steps[0][1] = note_step(60, 1);
  p.steps[0][1].fx[1] = micro_fx(10);  // snaps back
  p.steps[0][3] = note_step(62, 1);
  p.steps[0][3].fx[1] = micro_fx(30);  // moves on to 004? No: 004 holds a note, and once that note moves on, the OFF its move
                                       // leaves there is still not an empty note column (A21): refused, counted left
  p.steps[0][4] = note_step(64, 1);
  p.steps[0][4].fx[1] = micro_fx(40);  // moves on to 005
  p.steps[0][7] = note_step(65, 1);
  p.steps[0][7].fx[1] = micro_fx(30);  // 008 holds a note that is on its step: left
  p.steps[0][8] = note_step(67, 1);
  p.steps[0][10].fx[0] = {8, 5};       // a CC row: not counted
  p.steps[0][11] = note_step(68, 1);   // on its step: not counted
  static Pattern before;  // static: a pattern is 14 KB, and the qemu stack is small
  before = p;
  const QuantResult r = quant_all(p, g_fx.env(), sc, Filter::All, edit::Last{}, &apply_to_fixture, nullptr);
  CHECK(r.done == 2);
  CHECK(r.left == 2);
  CHECK(r.why == QuantWhy::StepTaken);
  CHECK(quant_text(r) == "QUANTISED 2, 2 LEFT (STEP TAKEN)");
  CHECK(p.steps[0][1].note == 60);
  CHECK(p.steps[0][1].fx[1].type == kFxNone);
  CHECK(p.steps[0][3].note == 62);  // refused: as it was
  CHECK(p.steps[0][3].fx[1].type == kFxMicroMove);
  CHECK(p.steps[0][4].note == kNoteOff);  // 004's note moved on; the OFF of the note above (003) stays (I2)
  CHECK(p.steps[0][5].note == 64);
  CHECK(p.steps[0][5].fx[1].type == kFxNone);
  CHECK(std::memcmp(&p.steps[0][7], &before.steps[0][7], sizeof(Step)) == 0);  // refused: as it was
  CHECK(std::memcmp(&p.steps[0][8], &before.steps[0][8], sizeof(Step)) == 0);
  CHECK(std::memcmp(&p.steps[0][10], &before.steps[0][10], sizeof(Step)) == 0);
  CHECK(std::memcmp(&p.steps[0][11], &before.steps[0][11], sizeof(Step)) == 0);
  // a second run has only the refused rows to try, and counts them again
  const QuantResult r2 = quant_all(p, g_fx.env(), sc, Filter::All, edit::Last{}, &apply_to_fixture, nullptr);
  CHECK(r2.done == 0);
  CHECK(r2.left == 2);
  // the CC filter shows no NOTE or end row
  const QuantResult r3 = quant_all(p, g_fx.env(), sc, Filter::Cc, edit::Last{}, &apply_to_fixture, nullptr);
  CHECK(r3.done == 0);
  CHECK(r3.left == 0);
  CHECK(quant_text(r3) == "NOTHING TO QUANTISE");
  // the other texts
  CHECK(quant_text(QuantResult{12, 0, QuantWhy::None}) == "QUANTISED 12");
  CHECK(quant_text(QuantResult{12, 2, QuantWhy::Outside}) == "QUANTISED 12, 2 LEFT (OUTSIDE SCOPE)");
  CHECK(quant_text(QuantResult{12, 2, QuantWhy::NoFxColumn}) == "QUANTISED 12, 2 LEFT (NO FX COLUMN)");
  CHECK(quant_text(QuantResult{12, 2, QuantWhy::Other}) == "QUANTISED 12, 2 LEFT (REFUSED)");
  char tiny[8];
  quant_message(QuantResult{12, 2, QuantWhy::StepTaken}, tiny, sizeof tiny);
  CHECK(std::strlen(tiny) == 7);  // clipped to the buffer, terminated
}

TEST_CASE("event list S3: quant_all over a block scope quantises the block's tracks and rows only") {
  g_fx.reset();
  Pattern& p = g_fx.pat;
  for (unsigned t = 0; t < 3; ++t) {
    p.steps[t][2] = note_step(60, 1);
    p.steps[t][2].fx[1] = micro_fx(10);
    p.steps[t][9] = note_step(61, 1);
    p.steps[t][9].fx[1] = micro_fx(10);
  }
  static Pattern before;  // static: a pattern is 14 KB, and the qemu stack is small
  before = p;
  const QuantResult r = quant_all(p, g_fx.env(), scope_block(0, 4, 1, 2), Filter::All, edit::Last{}, &apply_to_fixture, nullptr);
  CHECK(r.done == 2);  // tracks 2 and 3, step 3: the block's rows only
  CHECK(std::memcmp(&p.steps[0][2], &before.steps[0][2], sizeof(Step)) == 0);
  CHECK(p.steps[1][2].fx[1].type == kFxNone);
  CHECK(p.steps[2][2].fx[1].type == kFxNone);
  CHECK(std::memcmp(&p.steps[1][9], &before.steps[1][9], sizeof(Step)) == 0);
}

// ---- S3: the property test of QUANT ALL — I1 (only the planned steps differ), I2 (no other note's start or LEN), I3 (one unit, undo is
// byte-identical) over random patterns with Micro-moves on most notes -------------------------------------------------------------------------

namespace {

struct QApply {
  Editor* ed = nullptr;
  bool written[kMaxTracks][kMaxPatternLength];
  unsigned writes = 0;
};
QApply g_qa;

bool quant_apply_editor(void* c, const Plan& pl) noexcept {
  QApply* a = static_cast<QApply*>(c);
  bool ok = true;
  for (unsigned i = 0; i < pl.n; ++i) {
    ok = a->ed->set_step(1, pl.writes[i].track, pl.writes[i].step, pl.writes[i].value) == Status::Ok && ok;
    a->written[pl.writes[i].track][pl.writes[i].step] = true;
    ++a->writes;
  }
  return ok;
}

struct NoteAt {
  std::uint8_t track, step, micro, note, instr, vol;
  std::uint16_t len_steps;
  std::uint8_t len_percent;
  bool runs_on;
};

std::vector<NoteAt> notes_in(const Pattern& pat, const Env& env, Scope scope) {
  std::vector<NoteAt> out;
  Projector p(pat, env, scope, Filter::All);
  Row r;
  while (p.next(r)) {
    if (r.key.kind == Kind::Note) {
      const Step& st = pat.steps[r.key.track][r.key.step];
      out.push_back(NoteAt{r.key.track, r.key.step, r.microstep, st.note, st.instr, st.vol, r.len_steps, r.len_percent, r.len_runs_on});
    }
  }
  return out;
}

bool same_identity(const NoteAt& a, const NoteAt& b) {
  return a.track == b.track && a.step == b.step && a.micro == b.micro && a.note == b.note && a.instr == b.instr && a.vol == b.vol;
}

}  // namespace

TEST_CASE("event list S3: the property test of QUANT ALL — I1 (only the planned steps differ), I2 (notes keep their count, identity and, unless a row below moved, their LEN), I3 (one unit, undo is byte-identical)") {
  test::Rng rng(0x0A17C0DEu);
  UndoJournal journal;
  NEED(journal.begin(g_ring, model::kUndoRegionBytes));
  constexpr unsigned kEpisodes = 200;
  unsigned moved_episodes = 0;
  for (unsigned episode = 0; episode < kEpisodes; ++episode) {
    Project& proj = g_arena.project;
    proj.reset();
    Editor ed(proj, journal);
    NEED(ed.create_pattern(1, 16) == Status::Ok);
    Instrument in;
    init_instrument(in, InstrumentKind::Sample);
    NEED(ed.set_instrument(0, in) == Status::Ok);
    init_instrument(in, InstrumentKind::Midi);
    in.midi_channel = 3;
    NEED(ed.set_instrument(1, in) == Status::Ok);
    static Pattern pat;
    std::memset(&pat, 0, sizeof pat);
    pat.id = 1;
    std::uint8_t len[3];
    for (unsigned t = 0; t < 3; ++t) {
      len[t] = static_cast<std::uint8_t>(4 + rng.below(13));
      pat.track_length[t] = len[t];
      for (unsigned s = 0; s < kMaxPatternLength; ++s) {
        Step st = s < len[t] ? random_step(rng) : kEmptyStep;
        if (note_is_note(st.note) && rng.below(10) < 7) {
          st.fx[1] = StepFx{kFxMicroMove, static_cast<std::uint8_t>(micro_value(static_cast<int>(1 + rng.below(46))))};  // most notes are off the step
        }
        pat.steps[t][s] = st;
      }
    }
    for (unsigned t = 0; t < 3; ++t) {
      NEED(ed.set_track_length(1, t, len[t]) == Status::Ok);
      NEED(ed.set_steps(1, t, 0, pat.steps[t], kMaxPatternLength) == Status::Ok);
    }
    ed.history_reset();
    static Pattern before;
    before = *proj.pattern(1);
    const Env env = env_of(proj);
    const Scope scope = rng.below(2) == 0 ? scope_track(before, rng.below(3)) : scope_block(0, static_cast<std::int16_t>(rng.below(16)), 0, 2);
    const Filter filter = rng.below(4) == 0 ? Filter::Notes : Filter::All;
    const edit::Last last{static_cast<std::uint8_t>(1 + rng.below(2)), 0};

    g_qa = QApply{};
    g_qa.ed = &ed;
    ed.begin_group(core::ActionId::None, 0, UndoArea::Pattern);
    const QuantResult r = quant_all(*proj.pattern(1), env, scope, filter, last, &quant_apply_editor, &g_qa);
    ed.end_group();
    CHECK((r.done > 0u) == (g_qa.writes > 0u));  // every quantised row wrote its plan, nothing else did
    static Pattern mid;
    mid = *proj.pattern(1);

    // I1: only the steps a plan wrote differ
    for (unsigned t = 0; t < kMaxTracks; ++t) {
      for (unsigned s = 0; s < kMaxPatternLength; ++s) {
        if (!g_qa.written[t][s]) {
          CHECK(std::memcmp(&mid.steps[t][s], &before.steps[t][s], sizeof(Step)) == 0);
        }
      }
    }
    // I2: the notes are the same notes, the ones that did not move keep their start, and keep their LEN unless a step below them on
    // their track was written (their end may be the row that moved)
    const std::vector<NoteAt> nb = notes_in(before, env, scope);
    const std::vector<NoteAt> na = notes_in(mid, env, scope);
    CHECK(na.size() == nb.size());
    for (const NoteAt& a2 : na) {
      bool found = false;
      for (const NoteAt& b : nb) {
        if (same_identity(a2, b)) {
          found = true;
          bool below_written = false;
          for (unsigned u = static_cast<unsigned>(a2.step) + 1u; u < kMaxPatternLength; ++u) {
            below_written = below_written || g_qa.written[a2.track][u];
          }
          if (!below_written) {
            CHECK(a2.len_steps == b.len_steps);
            CHECK(a2.len_percent == b.len_percent);
            CHECK(a2.runs_on == b.runs_on);
          }
          break;
        }
      }
      if (!found) {
        CHECK(a2.micro == 0);  // a note that moved is on its step now
        CHECK(g_qa.written[a2.track][a2.step]);
      }
    }
    // I3: one unit; undo gives the pattern back byte for byte
    if (g_qa.writes > 0) {
      ++moved_episodes;
      CHECK(ed.undo(UndoArea::Pattern) == UndoResult::Ok);
      static Pattern undone;
      undone = *proj.pattern(1);
      CHECK(std::memcmp(&undone, &before, sizeof(Pattern)) == 0);
      CHECK(ed.undo(UndoArea::Pattern) == UndoResult::Nothing);  // it was the only unit
    }
  }
  CHECK(moved_episodes > kEpisodes / 2);  // the generator quantises something most of the time
}

// ---- OS-MIDIREC-P1 S6: the lane rows --------------------------------------------------------------------------------------------

namespace {

LaneStore g_lanes;  // static: the store is large

LaneEvent lane_ev(std::uint8_t kind, std::uint8_t ch, std::uint16_t tick, std::uint8_t a, std::uint8_t b, std::uint16_t len = 0) noexcept {
  LaneEvent e{};
  e.tick = tick;
  e.len = len;
  e.kind_channel = static_cast<std::uint8_t>(kind << 4 | ch);
  e.a = a;
  e.b = b;
  return e;
}

// Pattern 1 (the fixture's) with the given events in the lane of `track`; the store is emptied first.
void lane_fill(std::uint8_t track, const LaneEvent* ev, std::uint32_t n) {
  g_lanes.clear();
  g_lanes.sync(1);
  LaneStore::WriteWindow window(g_lanes);  // S2 contract: the gap is filled inside the window (closes at the return)
  LaneEvent* d = g_lanes.open_gap(1, track, 0, n);
  NEED(d != nullptr);
  std::memcpy(d, ev, n * sizeof(LaneEvent));
}

Env lane_env() noexcept {
  Env e = g_fx.env();
  e.lanes = &g_lanes;
  return e;
}

// The lane of `track` after a plan's lane write.
std::vector<LaneEvent> lane_after(const Plan& pl, std::uint8_t track) {
  const LaneStore::View v = g_lanes.lane(1, track);
  std::vector<LaneEvent> out;
  if (!pl.lane.on) {
    out.assign(v.events, v.events + v.count);
    return out;
  }
  const std::uint32_t a = LaneStore::lower_bound(v, pl.lane.t0);
  const std::uint32_t b = LaneStore::lower_bound(v, pl.lane.t1);
  out.assign(v.events, v.events + a);
  out.insert(out.end(), pl.lane.events, pl.lane.events + pl.lane.n);
  out.insert(out.end(), v.events + b, v.events + v.count);
  return out;
}

Plan lane_plan(const Request& r, const Scope& sc) { return plan(g_fx.pat, lane_env(), sc, edit::Last{}, r); }

Key lkey(std::uint8_t track, std::uint16_t lane, Kind kind, std::uint8_t step) noexcept { return Key{track, step, kind, 0, lane}; }

}  // namespace

TEST_CASE("event list S6: lane rows — projected with the stored fields, merged by time, step rows first at a tick, then by track") {
  g_fx.reset();
  g_fx.pat.steps[0][1] = note_step(61, 1);  // a step note at tick 48
  const LaneEvent ev[] = {lane_ev(kLaneNote, 1, 0, 60, 100, 96), lane_ev(kLaneCc, 1, 48, 74, 64), lane_ev(kLaneBend, 2, 70, 0, 96)};
  lane_fill(1, ev, 3);
  {
    LaneStore::WriteWindow window(g_lanes);  // S2 contract: the gap is filled inside the window
    LaneEvent* d = g_lanes.open_gap(1, 0, 0, 1);
    NEED(d != nullptr);
    d[0] = lane_ev(kLaneProgram, 0, 48, 5, 0);
  }

  const Scope sc = scope_block(0, 15, 0, 1);
  const std::vector<Row> rows = all(g_fx.pat, lane_env(), sc);
  NEED(rows.size() == 5);
  CHECK(rows[0].key == lkey(1, 1, Kind::Note, 0));  // tick 0
  CHECK(rows[0].lane_kind == kLaneNote);
  CHECK(rows[0].channel == 2);  // the stored channel 1 is shown 02
  CHECK(rows[0].lane_len == 96);
  CHECK(rows[0].len_steps == 2);
  CHECK(rows[0].vol == 100);
  CHECK(rows[1].key == Key{0, 1, Kind::Note, 0, 0});  // tick 48: the step row first
  CHECK(rows[2].key == lkey(0, 1, Kind::Cc, 1));      // then the lane rows of the tick, the lower track first
  CHECK(rows[2].lane_kind == kLaneProgram);
  CHECK(rows[3].key == lkey(1, 2, Kind::Cc, 1));
  CHECK(rows[3].lane_a == 74);
  CHECK(rows[4].key == lkey(1, 3, Kind::Cc, 1));
  CHECK(rows[4].microstep == 22);
  CHECK(rows[4].lane_kind == kLaneBend);

  // the key <-> index round trip over the merged list
  for (std::uint32_t i = 0; i < rows.size(); ++i) {
    Key k;
    NEED(key_of_index(g_fx.pat, lane_env(), sc, Filter::All, i, k));
    CHECK(k == rows[i].key);
  }
}

TEST_CASE("event list S6: lane rows — the filter, the scope and a track's length bound the list; without a store there are none") {
  g_fx.reset();
  const LaneEvent ev[] = {lane_ev(kLaneNote, 0, 0, 60, 100, 48), lane_ev(kLaneCc, 0, 96, 1, 2), lane_ev(kLaneNote, 0, 20 * 48, 60, 100, 48)};
  lane_fill(0, ev, 3);
  CHECK(all(g_fx.pat, lane_env(), scope_track(g_fx.pat, 0)).size() == 2);  // the third is past the track's 16 steps
  CHECK(all(g_fx.pat, lane_env(), scope_track(g_fx.pat, 0), Filter::Notes).size() == 1);
  CHECK(all(g_fx.pat, lane_env(), scope_track(g_fx.pat, 0), Filter::Cc).size() == 1);
  CHECK(all(g_fx.pat, lane_env(), scope_block(1, 15, 0, 0)).size() == 1);  // a block from step 2 on: only the CC
  CHECK(all(g_fx.pat, lane_env(), scope_block(0, 1, 1, 1)).empty());       // another track
  CHECK(all(g_fx.pat, g_fx.env(), scope_track(g_fx.pat, 0)).empty());      // no store: no lane rows
  CHECK(sizeof(Projector) <= kStackBound);
}

TEST_CASE("event list S6: planners — CH, LEN, DATA and VAL write one lane range; TYPE, IN and FX do nothing") {
  g_fx.reset();
  const LaneEvent ev[] = {lane_ev(kLaneNote, 1, 0, 60, 100, 96), lane_ev(kLaneNote, 1, 0, 64, 90, 96), lane_ev(kLaneCc, 1, 100, 74, 64)};
  lane_fill(0, ev, 3);
  const Scope sc = scope_track(g_fx.pat, 0);
  const Key k1 = lkey(0, 2, Kind::Note, 0);

  Plan p = lane_plan(field_req(Field::Ch, k1, 1), sc);
  CHECK(p.text[0] == '\0');
  CHECK(p.n == 0);
  NEED(p.lane.on);
  CHECK(p.lane.n == 2);  // the two events of the tick only
  std::vector<LaneEvent> a = lane_after(p, 0);
  NEED(a.size() == 3);
  CHECK((a[1].kind_channel & 15) == 2);
  CHECK((a[0].kind_channel & 15) == 1);
  p = lane_plan(field_req(Field::Ch, k1, -2), sc);  // wraps
  a = lane_after(p, 0);
  CHECK((a[1].kind_channel & 15) == 15);

  p = lane_plan(field_req(Field::Len, k1, 5), sc);
  a = lane_after(p, 0);
  CHECK(a[1].len == 101);
  p = lane_plan(field_req(Field::Len, k1, -500), sc);  // clamps at 1 tick
  a = lane_after(p, 0);
  CHECK(a[1].len == 1);
  p = lane_plan(field_req(Field::Len, k1, 1, true), sc);  // coarse: a step
  a = lane_after(p, 0);
  CHECK(a[1].len == 144);

  p = lane_plan(field_req(Field::Data, k1, 3), sc);
  a = lane_after(p, 0);
  CHECK(a[1].a == 67);
  p = lane_plan(field_req(Field::Val, k1, -200), sc);  // a velocity never reaches 0
  a = lane_after(p, 0);
  CHECK(a[1].b == 1);

  const Key kc = lkey(0, 3, Kind::Cc, 2);
  p = lane_plan(field_req(Field::Val, kc, 100), sc);
  a = lane_after(p, 0);
  CHECK(a[2].b == 127);
  p = lane_plan(field_req(Field::Len, kc, 1), sc);  // a CC has no LEN
  CHECK(!p.lane.on);
  CHECK(p.text[0] == '\0');
  for (const Field f : {Field::Type, Field::In, Field::Fx1, Field::Fx2}) {
    p = lane_plan(field_req(f, k1, 1), sc);
    CHECK(!p.lane.on);
    CHECK(p.n == 0);
  }
  p = lane_plan(field_req(Field::Ch, lkey(0, 9, Kind::Note, 0), 1), sc);  // a stale key plans nothing
  CHECK(!p.lane.on);
  CHECK(p.n == 0);
}

TEST_CASE("event list S6: planners — a bend's VAL is 14 bits, a channel pressure's VAL is its byte, a program has no VAL") {
  g_fx.reset();
  const LaneEvent ev[] = {lane_ev(kLaneProgram, 0, 0, 5, 0), lane_ev(kLaneChanAt, 0, 48, 90, 0), lane_ev(kLaneBend, 0, 96, 127, 127)};
  lane_fill(0, ev, 3);
  const Scope sc = scope_track(g_fx.pat, 0);
  Plan p = lane_plan(field_req(Field::Val, lkey(0, 1, Kind::Cc, 0), 1), sc);
  CHECK(!p.lane.on);
  p = lane_plan(field_req(Field::Val, lkey(0, 2, Kind::Cc, 1), 4), sc);
  std::vector<LaneEvent> a = lane_after(p, 0);
  CHECK(a[1].a == 94);
  p = lane_plan(field_req(Field::Data, lkey(0, 2, Kind::Cc, 1), 4), sc);  // a pressure has no DATA
  CHECK(!p.lane.on);
  p = lane_plan(field_req(Field::Val, lkey(0, 3, Kind::Cc, 2), 1), sc);  // at the top of the 14 bits: no change
  CHECK(!p.lane.on);
  p = lane_plan(field_req(Field::Val, lkey(0, 3, Kind::Cc, 2), -1), sc);
  a = lane_after(p, 0);
  CHECK(a[2].a == 126);
  CHECK(a[2].b == 127);
  p = lane_plan(field_req(Field::Val, lkey(0, 3, Kind::Cc, 2), -1, true), sc);  // coarse: 128
  a = lane_after(p, 0);
  CHECK(a[2].a == 127);
  CHECK(a[2].b == 126);
}

TEST_CASE("event list S6: planners — TIME moves a tick, re-sorts and names the new key; outside the scope is refused") {
  g_fx.reset();
  const LaneEvent ev[] = {lane_ev(kLaneNote, 0, 10, 60, 100, 48), lane_ev(kLaneCc, 0, 12, 1, 2), lane_ev(kLaneCc, 0, 30, 1, 3)};
  lane_fill(0, ev, 3);
  const Scope sc = scope_track(g_fx.pat, 0);
  Plan p = lane_plan(field_req(Field::Time, lkey(0, 1, Kind::Note, 0), 3), sc);
  CHECK(p.text[0] == '\0');
  NEED(p.lane.on);
  CHECK(p.has_new_key);
  std::vector<LaneEvent> a = lane_after(p, 0);
  NEED(a.size() == 3);
  CHECK(a[1].tick == 13);  // behind the CC at 12
  CHECK(a[1].a == 60);
  CHECK(p.new_key == lkey(0, 2, Kind::Note, 0));
  CHECK(lane_events_valid(a.data(), static_cast<std::uint32_t>(a.size()), 0, 16u * 48u));

  p = lane_plan(field_req(Field::Time, lkey(0, 1, Kind::Note, 0), 1, true), sc);  // coarse: a step
  a = lane_after(p, 0);
  CHECK(a[2].tick == 58);
  CHECK(p.new_key == lkey(0, 3, Kind::Note, 1));

  p = lane_plan(field_req(Field::Time, lkey(0, 1, Kind::Note, 0), -11), sc);  // before the start
  CHECK(!p.lane.on);
  CHECK(p.text[0] != '\0');
  p = lane_plan(field_req(Field::Time, lkey(0, 3, Kind::Cc, 0), 16 * 48), sc);  // past the track's end
  CHECK(!p.lane.on);
  CHECK(p.text[0] != '\0');
}

TEST_CASE("event list S6: planners — Delete, Insert (the end of a note, the next step, Shift: the same tick), the room refusals") {
  g_fx.reset();
  const LaneEvent ev[] = {lane_ev(kLaneNote, 0, 0, 60, 100, 96), lane_ev(kLaneCc, 0, 100, 74, 64), lane_ev(kLaneCc, 0, 15 * 48 + 10, 74, 65)};
  lane_fill(0, ev, 3);
  const Scope sc = scope_track(g_fx.pat, 0);
  Request r;
  r.key = lkey(0, 2, Kind::Cc, 2);
  r.op = Op::Delete;
  Plan p = lane_plan(r, sc);
  NEED(p.lane.on);
  std::vector<LaneEvent> a = lane_after(p, 0);
  CHECK(a.size() == 2);
  CHECK(a[1].tick == 15 * 48 + 10);

  r.op = Op::Insert;
  r.key = lkey(0, 1, Kind::Note, 0);
  p = lane_plan(r, sc);  // a note: a copy at its end
  a = lane_after(p, 0);
  NEED(a.size() == 4);
  CHECK(a[1].tick == 96);
  CHECK(a[1].a == 60);
  CHECK(p.has_new_key);
  CHECK(p.new_key == lkey(0, 2, Kind::Note, 2));
  r.shift = true;  // the same tick, after the row
  p = lane_plan(r, sc);
  a = lane_after(p, 0);
  CHECK(a[1].tick == 0);
  CHECK(p.new_key == lkey(0, 2, Kind::Note, 0));
  r.shift = false;
  r.key = lkey(0, 2, Kind::Cc, 2);  // a CC: the next step
  p = lane_plan(r, sc);
  a = lane_after(p, 0);
  CHECK(a[2].tick == 148);
  r.key = lkey(0, 3, Kind::Cc, 15);  // the last step: no next one
  p = lane_plan(r, sc);
  CHECK(!p.lane.on);
  CHECK(std::strcmp(p.text, "OUTSIDE THE SCOPE") == 0);

  const LaneEvent last[] = {lane_ev(kLaneNote, 0, 15 * 48, 60, 100, 96)};  // a note whose end is past the track
  lane_fill(0, last, 1);
  r.key = lkey(0, 1, Kind::Note, 15);
  p = lane_plan(r, sc);
  CHECK(!p.lane.on);
  CHECK(std::strcmp(p.text, "NO ROOM AFTER THIS NOTE") == 0);
}

TEST_CASE("event list S6: planners — a range of more than 32 events is refused, nothing is written") {
  g_fx.reset();
  LaneEvent ev[kMaxLaneWriteEvents + 1];
  for (auto& e : ev) {
    e = lane_ev(kLaneCc, 0, 5, 1, 2);
  }
  lane_fill(0, ev, kMaxLaneWriteEvents + 1);
  const Scope sc = scope_track(g_fx.pat, 0);
  Plan p = lane_plan(field_req(Field::Val, lkey(0, 1, Kind::Cc, 0), 1), sc);
  CHECK(!p.lane.on);
  CHECK(std::strcmp(p.text, "TOO MANY EVENTS HERE") == 0);
  lane_fill(0, ev, kMaxLaneWriteEvents);  // 32 at one tick edit fine, but an insert (one more) does not
  p = lane_plan(field_req(Field::Val, lkey(0, 1, Kind::Cc, 0), 1), sc);
  CHECK(p.lane.on);
  CHECK(p.lane.n == 32);
  Request r;
  r.op = Op::Insert;
  r.shift = true;
  r.key = lkey(0, 1, Kind::Cc, 0);
  p = lane_plan(r, sc);
  CHECK(!p.lane.on);
  CHECK(std::strcmp(p.text, "TOO MANY EVENTS HERE") == 0);
}

TEST_CASE("event list S6: QUANT of a lane row is refused, QUANT ALL leaves the lane rows alone") {
  g_fx.reset();
  const LaneEvent ev[] = {lane_ev(kLaneNote, 0, 13, 60, 100, 48)};
  lane_fill(0, ev, 1);
  const Scope sc = scope_track(g_fx.pat, 0);
  Request r;
  r.op = Op::Quant;
  r.key = lkey(0, 1, Kind::Note, 0);
  const Plan p = lane_plan(r, sc);
  CHECK(!p.lane.on);
  CHECK(std::strcmp(p.text, "NO QUANT ON A LANE ROW") == 0);
  const QuantResult q = quant_all(g_fx.pat, lane_env(), sc, Filter::All, edit::Last{}, [](void*, const Plan&) noexcept { return true; }, nullptr);
  CHECK(q.done == 0);
  CHECK(q.left == 0);
}
