// SPDX-License-Identifier: GPL-3.0-or-later
// OS-009 P1a.1: the edit rules of one step (model/step_edit.h) against the reference vectors, exhaustively
// where the inputs are few and over random selections where they are not; the FX table and the scales as data.
// Expected values: output of this project's own reference model (maintainer, 2026-10-18), kept as test vectors (data/pattern_edit_vectors.h): a digest of
// the results of each group of inputs, and a table of single cases written out in full. Whether the official firmware agrees is an open research question.
#include <doctest/doctest.h>

#include <o3os/app/fx_text.h>
#include <o3os/app/pattern_data.h>
#include <o3os/core/scales.h>
#include <o3os/model/fx_table.h>
#include <o3os/model/step_edit.h>
#include <o3os/seq/fx.h>
#include <o3os/ui/assets.h>

#include <cstdint>
#include <cstring>
#include <numeric>
#include <string>

#include "data/pattern_edit_vectors.h"
#include "support/rng.h"
#include "support/step_vectors.h"

using namespace o3os;
namespace edit = o3os::model::edit;
namespace fx = o3os::model::fx;
namespace vec = o3os::test::vec;
namespace vectors = o3os::test::vectors;

namespace {

// One step, from the vectors' note (-4..127) and the model's instrument.
struct Pair {
  model::Step m;
};

Pair make(int og_note, unsigned model_instr, std::uint8_t t0, std::uint8_t v0, std::uint8_t t1, std::uint8_t v1) {
  Pair p{};
  p.m.note = vec::to_model_note(og_note);
  p.m.instr = static_cast<std::uint8_t>(model_instr);
  p.m.vol = model::kVolumeUnset;
  p.m.fx[0] = {t0, v0};
  p.m.fx[1] = {t1, v1};
  return p;
}

// A one-step selection: the rule of the OS for it, `multi` being "a range was started on it".
edit::Rule rule_of(bool multi) { return edit::Rule{true, !multi}; }

// A rectangle of steps and whether a range was started (the random episodes).
struct Sel {
  int first_step, last_step, first_track, last_track;
  bool multi;
};
bool is_single(const Sel& s) { return s.first_step == s.last_step && s.first_track == s.last_track; }
edit::Rule rule_of(const Sel& s) { return edit::Rule{is_single(s), !s.multi}; }

// The OS's Last from one set of numbers: `slot` is the vectors' last used instrument, `name` its last used FX.
struct Session {
  edit::Last last;
};
Session session(unsigned slot, unsigned name) {
  Session s{};
  s.last = edit::Last{static_cast<std::uint8_t>(slot + 1), static_cast<std::uint8_t>(name)};
  return s;
}

constexpr int kNotes[] = {-4, -3, -2, -1, 0, 1, 11, 12, 59, 60, 61, 100, 119, 120, 125, 126, 127};

}  // namespace

TEST_CASE("step edit: every note against the reference vectors, single step: the encoder, and the pad") {
  // 132 note values (-4..127), instruments (none for a step without a note, slots 0, 5, 47, 48, 63 for a note), the last used slot,
  // and the deltas -5..5 (0 is the encoder that did not move: the OS does nothing).
  unsigned checked = 0;
  for (int n = -4; n <= 127; ++n) {
    vec::Digest dg;
    for (const unsigned instr : {0u, 1u, 6u, 48u, 49u, 64u}) {
      if (n >= 0 && instr == 0) continue;  // a note always names an instrument
      for (const unsigned last_slot : {0u, 7u, 63u}) {
        for (int d = -5; d <= 5; ++d) {
          if (d == 0) continue;
          const Pair start = make(n, instr, 0, 0, 0, 0);
          Pair p = start;
          Session s = session(last_slot, 0);
          edit::note_delta(p.m, d, rule_of(false), s.last);
          dg.add(vec::out_of(p.m, s.last));
          ++checked;
        }
        // The key or pad: a note entry with every note byte the pad board and the keys can give (0..120) and the off.
        for (const int value : {-4, 0, 36, 60, 120, 127}) {
          Pair p = make(n, instr, 0, 0, 0, 0);
          Session s = session(last_slot, 0);
          edit::note_set(p.m, vec::to_model_note(value), rule_of(false), s.last);
          dg.add(vec::out_of(p.m, s.last));
          ++checked;
        }
      }
    }
    CHECK_MESSAGE(dg.value() == vectors::kNoteDigest[n + 4], "note " << n);
  }
  CHECK(checked > 30000u);
  // A zero delta does nothing at all.
  Pair p = make(-1, 0, 0, 0, 0, 0);
  const Pair untouched = p;
  Session s = session(3, 0);
  edit::note_delta(p.m, 0, rule_of(false), s.last);
  CHECK(std::memcmp(&p.m, &untouched.m, sizeof(model::Step)) == 0);
}

TEST_CASE("step edit: single cases of the reference vectors, written out in full") {
  const unsigned clear_mask[5] = {edit::kElAll & ~edit::kElVol, edit::kElNote, edit::kElInstr, edit::kElFx1, edit::kElFx2};
  unsigned index = 0;
  for (const vectors::StepCase& c : vectors::kStepCases) {
    Pair p = make(c.note, c.instr, static_cast<std::uint8_t>(c.type0), static_cast<std::uint8_t>(c.value0), static_cast<std::uint8_t>(c.type1),
                  static_cast<std::uint8_t>(c.value1));
    Session s = session(c.last_slot, c.last_fx);
    const edit::Rule rule{c.kind != 2, c.kind == 0};
    switch (c.op) {
      case 'n': edit::note_delta(p.m, c.arg, rule, s.last); break;
      case 'N': edit::note_set(p.m, vec::to_model_note(c.arg), rule, s.last); break;
      case 'i': edit::instrument_delta(p.m, c.arg, rule, s.last); break;
      case 'I': edit::instrument_set(p.m, static_cast<unsigned>(c.arg) + 1u, rule, s.last); break;
      case 'v': edit::fx_value_delta(p.m, c.fx, c.arg, rule, s.last); break;
      case 'p': edit::fx_pad(p.m, c.fx, static_cast<unsigned>(c.arg), rule, s.last); break;
      case 't': edit::fx_type_delta(p.m, c.fx, c.arg); break;
      case 'T': edit::fx_type_set(p.m, c.fx, static_cast<unsigned>(c.arg)); break;
      case 'c': edit::clear(p.m, clear_mask[c.arg]); break;
      default: FAIL("unknown op in case " << index);
    }
    std::uint8_t got[8];
    vec::bytes_of(vec::out_of(p.m, s.last), got);
    CHECK_MESSAGE(std::memcmp(got, c.want, 8) == 0, "case " << index << " op " << c.op << " arg " << c.arg);
    ++index;
  }
}

TEST_CASE("step edit: notes, the edge cases of the rank order and the clamps") {
  const edit::Rule one{true, true};
  edit::Last last;
  last.instrument = 3;
  // An empty step: + gives note 60 (model 61) with the last used instrument; - gives Fade; off is the bottom, 127 the top.
  model::Step s = model::kEmptyStep;
  edit::note_delta(s, 1, one, last);
  CHECK((s.note == 61 && s.instr == 3));
  s = model::kEmptyStep;
  edit::note_delta(s, -1, one, last);
  CHECK((s.note == model::kNoteFade && s.instr == 3));
  edit::note_delta(s, -1, one, last);
  CHECK(s.note == model::kNoteCut);
  edit::note_delta(s, -1, one, last);
  CHECK(s.note == model::kNoteOff);
  edit::note_delta(s, -50, one, last);
  CHECK(s.note == model::kNoteOff);  // clamped at the bottom
  edit::note_delta(s, 1, one, last);
  CHECK(s.note == model::kNoteCut);
  edit::note_delta(s, 3, one, last);
  CHECK(s.note == 1);  // the next after Empty is C-0: Fade, Empty, then 0
  s.note = 128;
  edit::note_delta(s, 100, one, last);
  CHECK(s.note == 128);  // G-9 (127) is the top
  edit::note_delta(s, -2000000000, one, last);
  CHECK(s.note == model::kNoteOff);  // a huge delta clamps instead of wrapping
  edit::note_delta(s, 2000000000, one, last);
  CHECK(s.note == 128);
  // The bytes outside the note range are carried: 129..252.
  for (unsigned b = 129; b <= 252; ++b) {
    s = model::kEmptyStep;
    s.note = static_cast<std::uint8_t>(b);
    s.instr = 9;
    const model::Step before = s;
    edit::note_delta(s, 5, one, last);
    edit::note_delta(s, -5, edit::Rule{false, false}, last);
    CHECK(std::memcmp(&s, &before, sizeof s) == 0);
  }
  // Multi: only steps that have a note change, clamped to 0..127 (no Fade, Cut or Off by the encoder).
  const edit::Rule many{false, false};
  s = model::kEmptyStep;
  edit::note_delta(s, 5, many, last);
  CHECK(s.note == 0);
  s.note = 3;
  edit::note_delta(s, -50, many, last);
  CHECK(s.note == 1);
  s.note = model::kNoteOff;
  edit::note_delta(s, 5, many, last);
  CHECK(s.note == model::kNoteOff);
  // A note is enterable if it is 1..128 or fade, cut, off: a pad cannot write 0 or a reserved byte.
  for (unsigned b = 0; b < 256; ++b) {
    const bool want = (b >= 1 && b <= 128) || b >= 253;
    CHECK(edit::is_enterable_note(b) == want);
  }
  s = model::kEmptyStep;
  edit::note_set(s, 0, one, last);
  edit::note_set(s, 200, one, last);
  CHECK(std::memcmp(&s, &model::kEmptyStep, sizeof s) == 0);
}

TEST_CASE("step edit: instruments against the reference vectors: the walk, the pad, the encoder") {
  // The walk: every instrument 0..65 (0 and 65 are not in the order: position 0), every delta -8..8.
  for (unsigned instr = 0; instr <= 65; ++instr) {
    vec::Digest dg;
    for (int d = -8; d <= 8; ++d) {
      dg.add_u32(edit::instrument_walk(instr, d));
    }
    CHECK_MESSAGE(dg.value() == vectors::kWalkDigest[instr], "instrument " << instr);
  }
  // The order itself: slots 63..48, then 0..47 (so the encoder goes from the MIDI channels into the samples).
  CHECK(edit::instrument_walk(1, -1) == 49u);   // slot 0 down: slot 48 (MIDI channel 1)
  CHECK(edit::instrument_walk(49, -1) == 50u);  // 48 -> 49
  CHECK(edit::instrument_walk(64, -1) == 64u);  // slot 63 is the first: it stays
  CHECK(edit::instrument_walk(48, 1) == 48u);   // slot 47 is the last
  CHECK(edit::instrument_walk(1, 5) == 6u);
  CHECK(edit::instrument_walk(1, 99) == 6u);    // a delta is clamped to +-5 places

  unsigned checked = 0;
  unsigned group = 0;
  for (int n = -4; n <= 127; n += (n < 4 ? 1 : 9)) {
    vec::Digest dg;
    for (const unsigned instr : {0u, 1u, 6u, 48u, 49u, 64u}) {
      if (n >= 0 && instr == 0) continue;
      for (const unsigned last_slot : {0u, 9u, 63u}) {
        for (const int d : {-7, -5, -1, 1, 3, 5, 7}) {
          for (const bool multi : {false, true}) {
            Pair p = make(n, instr, 0, 0, 0, 0);
            Session s = session(last_slot, 0);
            edit::instrument_delta(p.m, d, rule_of(multi), s.last);
            dg.add(vec::out_of(p.m, s.last));
            ++checked;
          }
        }
        for (const unsigned value : {1u, 2u, 48u, 49u, 64u}) {
          for (const bool multi : {false, true}) {
            Pair p = make(n, instr, 0, 0, 0, 0);
            Session s = session(last_slot, 0);
            edit::instrument_set(p.m, value, rule_of(multi), s.last);
            dg.add(vec::out_of(p.m, s.last));
            ++checked;
          }
        }
      }
    }
    CHECK_MESSAGE(dg.value() == vectors::kInstrDigest[group], "note " << n);
    ++group;
  }
  CHECK(group == sizeof vectors::kInstrDigest / sizeof vectors::kInstrDigest[0]);
  CHECK(checked > 5000u);
  // Out of range arguments change nothing.
  const edit::Rule one{true, true};
  edit::Last last;
  model::Step s = model::kEmptyStep;
  edit::instrument_set(s, 0, one, last);
  edit::instrument_set(s, 65, one, last);
  CHECK(std::memcmp(&s, &model::kEmptyStep, sizeof s) == 0);
}

namespace {

// Every FX type the table knows, and some it does not (43..127 reserved, 128.. engine), and the values that matter at the edges.
constexpr std::uint8_t kTypes[] = {0, 1, 2, 3, 13, 14, 15, 16, 17, 18, 20, 21, 25, 26, 34, 37, 38, 39, 41, 42, 43, 100, 127, 128, 200, 255};
constexpr std::uint8_t kValues[] = {0, 1, 5, 24, 25, 50, 53, 60, 99, 100, 127, 198, 199, 200, 254, 255};

}  // namespace

TEST_CASE("step edit: FX value, pad and type against the reference vectors, every type, single and range") {
  unsigned checked = 0;
  unsigned group = 0;
  for (const std::uint8_t type : kTypes) {
    vec::Digest dg;
    for (const std::uint8_t value : kValues) {
      for (const unsigned fxi : {0u, 1u}) {
        for (const unsigned last_name : {0u, 1u, 9u, 33u}) {
          for (const bool multi : {false, true}) {
            std::uint8_t t[2] = {0, 0}, v[2] = {0, 0};
            t[fxi] = type;
            v[fxi] = value;
            for (const int d : {-300, -7, -1, 1, 7, 300}) {  // an FX value delta
              Pair p = make(60, 1, t[0], v[0], t[1], v[1]);
              Session s = session(0, last_name);
              edit::fx_value_delta(p.m, fxi, d, rule_of(multi), s.last);
              dg.add(vec::out_of(p.m, s.last));
              ++checked;
            }
            for (const unsigned pad : {0u, 1u, 23u, 24u, 46u, 47u}) {  // an FX value from a pad
              Pair p = make(60, 1, t[0], v[0], t[1], v[1]);
              Session s = session(0, last_name);
              edit::fx_pad(p.m, fxi, pad, rule_of(multi), s.last);
              dg.add(vec::out_of(p.m, s.last));
              ++checked;
            }
          }
        }
      }
      for (const unsigned fxi : {0u, 1u}) {
        std::uint8_t t[2] = {0, 0}, v[2] = {0, 0};
        t[fxi] = type;
        v[fxi] = value;
        for (const int d : {-1, 1}) {  // an FX type delta
          Pair p = make(60, 1, t[0], v[0], t[1], v[1]);
          edit::fx_type_delta(p.m, fxi, d);
          dg.add(vec::out_of(p.m));
          ++checked;
        }
        for (unsigned set = 0; set < 43; ++set) {  // an FX type set: every id 0..42 (the table refuses what it does not run; the OS also takes the tracker FX 46..50, below)
          Pair p = make(60, 1, t[0], v[0], t[1], v[1]);
          edit::fx_type_set(p.m, fxi, set);
          if (type == 0 && set != 0 && fx::is_executable(static_cast<std::uint8_t>(set))) {
            // OS-PFXFIX (new behaviour, not parity): an empty cell takes the type's table default; the other column is untouched
            CHECK_MESSAGE((p.m.fx[fxi].type == set && p.m.fx[fxi].value == fx::row(static_cast<std::uint8_t>(set)).start && p.m.fx[1u - fxi].type == t[1u - fxi]), "set type " << set << " over None fx " << fxi);
          } else {
            dg.add(vec::out_of(p.m));
          }
          ++checked;
        }
      }
    }
    CHECK_MESSAGE(dg.value() == vectors::kFxDigest[group], "type " << int(type));
    ++group;
  }
  CHECK(checked > 10000u);
}

TEST_CASE("step edit: the picker walk ends in None at both ends, and a placeholder restarts at the next type's default") {
  const edit::Rule one{true, true};
  (void)one;
  // From the last picker entry (type 39, Bit Depth: the OS appended Swing, Gate, Chord, Arp, Tune, the two Slides, Overdrive and Bit Depth to the first 34, whose last is type 13, Break Pattern) one place up is past the picker: None.
  // One place down from the first real type (Off, picker index 1) is index 0: None too. (the picker index is a uint8_t.)
  model::Step s = model::kEmptyStep;
  s.fx[0] = {39, 1};
  edit::fx_type_delta(s, 0, 1);
  CHECK((s.fx[0].type == 46 && s.fx[0].value == 1));  // OS-COREFX appended the tracker FX: Bit Depth is no longer the last, one place up is the tracker Arpeggio (0..255)
  s.fx[0] = {62, 9};
  edit::fx_type_delta(s, 0, 1);
  CHECK(s.fx[0].type == 96);  // OS-CEWTENV W6b appended w, y, z after Panbrello (CF3): one place up is w
  s.fx[0] = {98, 9};
  edit::fx_type_delta(s, 0, 1);
  CHECK((s.fx[0].type == 0 && s.fx[0].value == 0));  // the last entry is z (Grain Length)
  s.fx[0] = {96, 9};
  edit::fx_type_delta(s, 0, -1);
  CHECK(s.fx[0].type == 62);  // Panbrello precedes w
  s.fx[0] = {62, 9};
  edit::fx_type_delta(s, 0, -1);
  CHECK((s.fx[0].type == 61 && s.fx[0].value == 9));  // Global volume slide precedes it
  s.fx[0] = {57, 9};
  edit::fx_type_delta(s, 0, -1);
  CHECK((s.fx[0].type == 56 && s.fx[0].value == 9));  // Tremor precedes Channel pan
  s.fx[0] = {57, 9};
  edit::fx_type_delta(s, 0, 1);
  CHECK((s.fx[0].type == 58 && s.fx[0].value == 9));  // Channel volume follows it (0..64: kept)
  s.fx[0] = {57, 90};
  edit::fx_type_delta(s, 0, 1);
  CHECK((s.fx[0].type == 58 && s.fx[0].value == 64));  // ... and holds the value to its range
  s.fx[0] = {51, 9};
  edit::fx_type_delta(s, 0, -1);
  CHECK((s.fx[0].type == 50 && s.fx[0].value == 9));  // Pan slide follows Fine porta down
  s.fx[0] = {46, 9};
  edit::fx_type_delta(s, 0, -1);
  CHECK((s.fx[0].type == 39 && s.fx[0].value == 9));
  edit::fx_type_set(s, 1, 49);  // set by id: the tracker FX are executable
  CHECK((s.fx[1].type == 49));
  edit::fx_type_set(s, 1, 45);  // Order jump is not offered; 63 and 300 are not types at all
  CHECK((s.fx[1].type == 49));
  edit::fx_type_set(s, 1, 56);  // (the CF2 FX are offered too)
  CHECK((s.fx[1].type == 56));
  edit::fx_type_set(s, 1, 57);  // (and Channel pan)
  CHECK((s.fx[1].type == 57));
  edit::fx_type_set(s, 1, 62);  // (and the five of CF3)
  CHECK((s.fx[1].type == 62));
  edit::fx_type_set(s, 1, 49);
  edit::fx_type_set(s, 1, 63);
  CHECK((s.fx[1].type == 49));
  edit::fx_type_set(s, 1, 300);
  CHECK((s.fx[1].type == 49));
  s.fx[0] = {42, 30};  // Slide Down, 30: one place up is Overdrive (38, 0..100), kept; one more is Bit Depth (39, 1..16), clamped
  edit::fx_type_delta(s, 0, 1);
  CHECK((s.fx[0].type == 38 && s.fx[0].value == 30));
  edit::fx_type_delta(s, 0, 1);
  CHECK((s.fx[0].type == 39 && s.fx[0].value == 16));
  s.fx[0] = {20, 1};  // Gate Length is no longer the last: one place up is Chord (14, 0..15), the value kept
  edit::fx_type_delta(s, 0, 1);
  CHECK((s.fx[0].type == 14 && s.fx[0].value == 1));
  s.fx[0] = {40, 30};  // Tune, 30: one place up is Slide Up (0..255), kept
  edit::fx_type_delta(s, 0, 1);
  CHECK((s.fx[0].type == 41 && s.fx[0].value == 30));
  s.fx[0] = {13, 1};  // the last of the first 34: one place up is now Swing (17, 25..75), the value kept and clamped into its range
  edit::fx_type_delta(s, 0, 1);
  CHECK((s.fx[0].type == 17 && s.fx[0].value == 25));
  s.fx[0] = {1, 0};
  edit::fx_type_delta(s, 0, -1);
  CHECK(s.fx[0].type == 0);
  s.fx[0] = {0, 0};  // a None slot is not stepped: it has no type to leave
  edit::fx_type_delta(s, 0, 1);
  CHECK(s.fx[0].type == 0);
  // From Off (index 1) up: Volume/Velocity (18), whose range is 0..100: the old value is kept, clamped.
  s.fx[0] = {1, 0};
  edit::fx_type_delta(s, 0, 1);
  CHECK((s.fx[0].type == 18 && s.fx[0].value == 0));
  s.fx[0] = {2, 100};  // Micro-move, 100: up two places to Chance (4, 0..100): kept
  edit::fx_type_delta(s, 0, 1);
  CHECK(s.fx[0].type == 4);
  // A type outside the picker (43, undefined): its picker index reads 0, one place up is index 1, Off, and its value restarts at the default.
  s.fx[1] = {43, 30};
  edit::fx_type_delta(s, 1, 1);
  CHECK((s.fx[1].type == 1 && s.fx[1].value == 0));
  s.fx[1] = {44, 77};  // an undefined type: down is index 255, which reads as None
  edit::fx_type_delta(s, 1, -1);
  CHECK(s.fx[1].type == 0);
  // Edits do not touch an unknown type's data (no placeholder is left among the 43).
  edit::Last last;
  for (const std::uint8_t type : {std::uint8_t{43}, std::uint8_t{44}, std::uint8_t{128}, std::uint8_t{255}}) {
    s = model::kEmptyStep;
    s.fx[0] = {type, 77};
    const model::Step before = s;
    edit::fx_value_delta(s, 0, 5, edit::Rule{true, true}, last);
    edit::fx_pad(s, 0, 10, edit::Rule{true, true}, last);
    edit::fx_value_set(s, 0, 5, edit::Rule{true, true}, last);
    CHECK_MESSAGE(std::memcmp(&s, &before, sizeof s) == 0, int(type));
  }
  // An FX type may be set only to an executable one.
  s = model::kEmptyStep;
  edit::fx_type_set(s, 0, 43);
  edit::fx_type_set(s, 0, 4000);
  CHECK(s.fx[0].type == 0);
  edit::fx_type_set(s, 0, 37);
  CHECK(s.fx[0].type == 37);
  edit::fx_type_set(s, 2, 4);  // there is no third FX column
  CHECK(s.fx[1].type == 0);
}

TEST_CASE("step edit: clear against the reference vectors, and the volume column") {
  // Notes and Instruments are treated alike; "all" is note, instrument and both FX types (the OS's volume is its own).
  const unsigned maps[] = {edit::kElAll & ~edit::kElVol, edit::kElNote, edit::kElInstr, edit::kElFx1, edit::kElFx2};
  unsigned group = 0;
  for (const unsigned elements : maps) {
    vec::Digest dg;
    for (const int n : kNotes) {
      Pair p = make(n, n >= 0 ? 7 : 0, 3, 30, 27, 40);
      edit::clear(p.m, elements);
      dg.add(vec::out_of(p.m));
    }
    CHECK_MESSAGE(dg.value() == vectors::kClearDigest[group], "elements " << elements);
    ++group;
  }
  model::Step s = model::kEmptyStep;
  s.vol = 100;
  s.note = 40;
  s.instr = 2;
  edit::clear(s, edit::kElVol);
  CHECK((s.vol == model::kVolumeUnset && s.note == 40 && s.instr == 2));
  s.vol = 100;
  edit::clear(s, edit::kElAll);
  CHECK(model::is_empty(s));  // a whole-step clear leaves nothing, the volume and the leftover FX values included
  s = model::kEmptyStep;
  s.fx[0] = {5, 9};
  edit::clear(s, edit::kElFx1);
  CHECK(model::is_empty(s));
  edit::clear(s, 0);  // no element: nothing
  CHECK(model::is_empty(s));
}

TEST_CASE("step edit: the volume column follows the FX rule: the first entry takes 127, a range changes only what has one") {
  edit::Last last;
  const edit::Rule one{true, true};
  const edit::Rule many{false, false};
  model::Step s = model::kEmptyStep;
  edit::volume_delta(s, -1, one);
  CHECK(s.vol == model::edit::kFirstVolume);  // the first entry takes the default, whatever the direction (Q-P1-10)
  edit::volume_delta(s, -5, one);
  CHECK(s.vol == 122);
  edit::volume_delta(s, -1000, one);
  CHECK(s.vol == 0);
  edit::volume_delta(s, 5000, one);
  CHECK(s.vol == model::kVolumeMax);
  edit::volume_delta(s, 0, one);
  CHECK(s.vol == model::kVolumeMax);
  s = model::kEmptyStep;
  edit::volume_delta(s, 3, many);
  CHECK(s.vol == model::kVolumeUnset);  // a range does not give an unset step a volume
  edit::volume_set(s, 90, many);
  CHECK(s.vol == model::kVolumeUnset);
  edit::volume_set(s, 90, one);
  CHECK(s.vol == 90);
  edit::volume_set(s, 5, many);
  CHECK(s.vol == 5);
  edit::volume_set(s, 4000, one);
  CHECK(s.vol == model::kVolumeMax);
  s.vol = 200;  // not a volume: carried by a delta, replaced by an entry on the single step
  edit::volume_delta(s, 3, one);
  CHECK(s.vol == 200);
  edit::volume_delta(s, 3, many);
  CHECK(s.vol == 200);
  edit::volume_set(s, 10, many);
  CHECK(s.vol == 200);
  edit::volume_set(s, 10, one);
  CHECK(s.vol == 10);
  (void)last;
}

TEST_CASE("step edit: random selections of a pattern against the reference vectors, every rule") {
  test::Rng rng(0x0509A1u);
  const unsigned episodes = 3000;
  vec::Digest dg;
  unsigned group = 0;
  for (unsigned e = 0; e < episodes; ++e) {
    if (e % 100 == 0 && e != 0) {  // 100 episodes to a digest
      CHECK_MESSAGE(dg.value() == vectors::kRandomDigest[group], "episodes before " << e);
      ++group;
      dg.reset();
    }
    model::Step ms[4][5];
    for (int t = 0; t < 4; ++t) {
      for (int s = 0; s < 5; ++s) {
        const int n = rng.below(4) == 0 ? -1 : (rng.below(8) == 0 ? -2 - static_cast<int>(rng.below(3)) : static_cast<int>(rng.below(128)));
        const unsigned instr = n >= 0 ? 1 + rng.below(64) : (rng.below(3) == 0 ? 1 + rng.below(64) : 0);
        const std::uint8_t t0 = rng.below(3) == 0 ? 0 : kTypes[rng.below(sizeof kTypes)];
        const std::uint8_t t1 = rng.below(3) == 0 ? 0 : kTypes[rng.below(sizeof kTypes)];
        // (the order of the two calls is pinned: the arguments of a call are not sequenced, and the recorded digests follow this one)
        const std::uint8_t v1 = rng.byte();
        const std::uint8_t v0 = rng.byte();
        const Pair p = make(n, instr, t0, v0, t1, v1);
        ms[t][s] = p.m;
      }
    }
    // A rectangle (it may be one step) and whether a range was started (it may be one step with the range flag set).
    Sel sel{};
    sel.first_step = static_cast<int>(rng.below(5));
    sel.last_step = sel.first_step + static_cast<int>(rng.below(static_cast<unsigned>(5 - sel.first_step)));
    sel.first_track = static_cast<int>(rng.below(4));
    sel.last_track = sel.first_track + static_cast<int>(rng.below(static_cast<unsigned>(4 - sel.first_track)));
    sel.multi = !is_single(sel) || rng.below(4) == 0;
    const unsigned last_name = rng.below(36);  // (called before the slot: the recorded digests follow this order)
    const unsigned last_slot = rng.below(64);
    Session s = session(last_slot, last_name);
    const edit::Rule rule = rule_of(sel);
    const unsigned fxi = rng.below(2);
    const int op = static_cast<int>(rng.below(10));
    const int d = static_cast<int>(rng.below(13)) - 6;
    const int pad = static_cast<int>(rng.below(48));
    const int note_value = static_cast<int>(rng.below(121));
    const int slot_value = static_cast<int>(rng.below(64));
    const int type_value = static_cast<int>(rng.below(45));
    bool comparable = true;
    const auto os_each = [&](auto fn) {
      for (int t = sel.first_track; t <= sel.last_track; ++t) {
        for (int st = sel.first_step; st <= sel.last_step; ++st) {
          fn(ms[t][st]);
          if (is_single(sel) && (op == 0 || op == 1 || op == 2 || op == 3)) {
            return;  // the reference model's note and instrument routines return after the first step of a single selection
          }
        }
      }
    };
    switch (op) {
      case 0:
        if (d == 0) comparable = false;
        os_each([&](model::Step& m) { edit::note_delta(m, d, rule, s.last); });
        break;
      case 1:
        os_each([&](model::Step& m) { edit::note_set(m, vec::to_model_note(note_value), rule, s.last); });
        break;
      case 2:
        if (d == 0) comparable = false;
        os_each([&](model::Step& m) { edit::instrument_delta(m, d, rule, s.last); });
        break;
      case 3:
        os_each([&](model::Step& m) { edit::instrument_set(m, static_cast<unsigned>(slot_value) + 1u, rule, s.last); });
        break;
      case 4:
        if (d == 0) comparable = false;
        os_each([&](model::Step& m) { edit::fx_value_delta(m, fxi, d, rule, s.last); });
        break;
      case 5:
        os_each([&](model::Step& m) { edit::fx_pad(m, fxi, static_cast<unsigned>(pad), rule, s.last); });
        break;
      case 6:
        if (d == 0) comparable = false;  // (the reference model's zero turns a placeholder into None; the encoder never says zero)
        os_each([&](model::Step& m) { edit::fx_type_delta(m, fxi, d); });
        break;
      case 7:
        if (type_value != 0 && fx::is_executable(static_cast<std::uint8_t>(type_value))) {  // OS-PFXFIX: an empty cell takes the table default, the reference value 0 differs (pinned apart)
          for (int t = sel.first_track; t <= sel.last_track; ++t) {
            for (int st = sel.first_step; st <= sel.last_step; ++st) {
              if (ms[t][st].fx[fxi].type == 0) comparable = false;
            }
          }
        }
        os_each([&](model::Step& m) { edit::fx_type_set(m, fxi, static_cast<unsigned>(type_value)); });
        break;
      case 8: {
        const int el = static_cast<int>(rng.below(5));
        const unsigned mask[5] = {edit::kElAll & ~edit::kElVol, edit::kElNote, edit::kElInstr, edit::kElFx1, edit::kElFx2};
        os_each([&](model::Step& m) { edit::clear(m, mask[el]); });
        break;
      }
      default:
        break;
    }
    if (!comparable) continue;
    for (int t = 0; t < 4; ++t) {
      for (int st = 0; st < 5; ++st) {
        dg.add(vec::out_of(ms[t][st]));
      }
    }
    dg.add(vec::session_out(s.last.instrument - 1u, s.last.fx));
  }
  CHECK_MESSAGE(dg.value() == vectors::kRandomDigest[group], "the last episodes");
  CHECK(group + 1 == sizeof vectors::kRandomDigest / sizeof vectors::kRandomDigest[0]);
}

TEST_CASE("step edit: the cursor step after an entry wraps inside the pattern") {
  for (int len = 1; len <= 128; ++len) {
    vec::Digest dg;
    for (int es = 0; es <= 32; ++es) {
      for (int row = 0; row < len; row += (len > 40 ? 7 : 1)) {
        const int step = es;
        const int os_row = step <= 0 ? row : (row + step <= len - 1 ? row + step : (row + step) % len);
        if (es < len) {
          dg.add_u32(static_cast<std::uint32_t>(os_row));
        }
        CHECK((os_row >= 0 && os_row < len));  // the row stays inside the pattern
      }
    }
    CHECK_MESSAGE(dg.value() == vectors::kCursorDigest[len - 1], "len " << len);
  }
}

// ---- the tables ----------------------------------------------------------------------------------------------------------------

TEST_CASE("fx table: the model's rows and the sequencer's table state the same range for every type 1..42") {
  // Two tables of this tree (model/fx_table.h for the editor, seq/fx.h for the sequencer) must never drift; type 0 carries no value, so its range is not compared.
  for (unsigned t = 1; t < fx::kTypeCount; ++t) {
    CHECK_MESSAGE(fx::kRows[t].lo == seq::kFxInfo[t].min, "type " << t);
    if (t == static_cast<unsigned>(seq::fx::kChance)) {
      // OS-CECHANCE C2: the editor's row reaches 174 like the sequencer's (0..100 a probability, 101..174 the conditions of ADR 0004 amendment 4).
      CHECK(fx::kRows[t].hi == seq::fx::kChanceMax);
      CHECK(seq::kFxInfo[t].max == seq::fx::kChanceMax);
    } else {
      CHECK_MESSAGE(fx::kRows[t].hi == seq::kFxInfo[t].max, "type " << t);
    }
  }
}

TEST_CASE("fx table: 43 types and 17 tracker FX, a picker of 63 executable ones (the first 34, then Swing, Gate, Chord, Arp, Tune, the two Slides, Overdrive, Bit Depth, the tracker FX and w, y, z), placeholders carried and never picked") {
  CHECK(fx::kTypeCount == 43u);
  CHECK(fx::kPickerCount == 63u);
  CHECK(sizeof(fx::kRows) / sizeof(fx::kRows[0]) == 43u);
  CHECK(sizeof(fx::kCoreRows) / sizeof(fx::kCoreRows[0]) == fx::kCoreCount);
  CHECK(fx::kCoreFirst == seq::fx::kTrkArp);  // OS-COREFX: the model's table and the sequencer's ids are one range
  CHECK(fx::kCoreFirst + fx::kCoreCount - 1u == seq::fx::kCoreLast);
  bool seen[256] = {};
  for (unsigned i = 0; i < fx::kPickerCount; ++i) {
    const std::uint8_t type = fx::kPickerOrder[i];
    CHECK(fx::is_known(type));
    CHECK_MESSAGE(fx::is_executable(type), "picker " << i);
    CHECK_FALSE(seen[type]);  // each type once
    seen[type] = true;
    CHECK(fx::picker_index(type) == i);
  }
  unsigned placeholders = 0;
  for (unsigned t = 0; t < fx::kTypeCount; ++t) {
    const bool placeholder = fx::kRows[t].support == fx::Support::Reserved;
    placeholders += placeholder ? 1u : 0u;
    CHECK_MESSAGE(fx::is_executable(static_cast<std::uint8_t>(t)) == !placeholder, "type " << t);
    CHECK_MESSAGE(seen[t] == !placeholder, "type " << t << ": the picker lists exactly the executable ones");
    CHECK(fx::kRows[t].lo <= fx::kRows[t].start);
    CHECK(fx::kRows[t].start <= fx::kRows[t].hi);
  }
  CHECK(placeholders == 0u);  // 17 Swing and 20 Gate Length run since OS-CFGWIRE; 14, 21, 40, 41, 42 since OS-PFX; 38 Overdrive and 39 Bit depth since OS-IFX1
  for (unsigned t = 0; t < 256; ++t) {  // OS-COREFX: the tracker FX are known, executable and in the picker; 43..45 and 63..255 are none of the three, except 96 and 97 (OS-CEWTENV W6a: known and executable, not in the picker until W6b)
    const bool core = t >= 46u && t <= 62u;
    const bool ce = t >= fx::kCeFirst && t < fx::kCeFirst + fx::kCeCount;
    CHECK_MESSAGE(fx::is_core(static_cast<std::uint8_t>(t)) == core, "type " << t);
    CHECK_MESSAGE(fx::is_known(static_cast<std::uint8_t>(t)) == (t < fx::kTypeCount || core || ce), "type " << t);
    if (t >= fx::kTypeCount) {
      CHECK_MESSAGE(fx::is_executable(static_cast<std::uint8_t>(t)) == (core || ce), "type " << t);
      CHECK_MESSAGE(seen[t] == (core || ce), "type " << t);  // OS-CEWTENV W6b: 96, 97 and 98 are in the picker
    }
  }
  for (unsigned i = 0; i < fx::kCoreCount; ++i) {  // the same range as the sequencer's table, and a default inside it
    const std::uint8_t type = static_cast<std::uint8_t>(fx::kCoreFirst + i);
    CHECK(fx::row(type).lo == seq::kFxInfo[type].min);
    CHECK(fx::row(type).hi == seq::kFxInfo[type].max);
    CHECK(fx::row(type).lo <= fx::row(type).start);
    CHECK(fx::row(type).start <= fx::row(type).hi);
    CHECK(fx::picker_index(type) == 43u + i);
    CHECK(seq::fx_kind(type) == seq::FxKind::Row);
    CHECK_FALSE(fx::is_random_eligible(type));  // a Fill's random FX stay the first 34
  }
  CHECK_FALSE(fx::is_executable(43));
  CHECK_FALSE(fx::is_executable(45));
  CHECK_FALSE(fx::is_executable(63));
  CHECK_FALSE(fx::is_executable(255));
  CHECK(fx::is_executable(0));  // None is a valid, do-nothing slot
  CHECK(fx::row(200).hi == 100);  // an unknown type reads as None
  CHECK(fx::type_of_picker(34) == 17);  // Swing and Gate Length are appended: the first 34 indices keep their places
  CHECK(fx::type_of_picker(35) == 20);
  CHECK(fx::type_of_picker(36) == 14);  // OS-PFX appends Chord, Arp, Tune, Slide up and Slide down after them
  CHECK(fx::type_of_picker(37) == 21);
  CHECK(fx::type_of_picker(38) == 40);
  CHECK(fx::type_of_picker(39) == 41);
  CHECK(fx::type_of_picker(40) == 42);
  CHECK(fx::type_of_picker(41) == 38);  // OS-IFX1 appends Overdrive and Bit Depth
  CHECK(fx::type_of_picker(42) == 39);
  CHECK(fx::type_of_picker(43) == 46);  // OS-COREFX appends the five tracker FX
  CHECK(fx::type_of_picker(44) == 47);
  CHECK(fx::type_of_picker(45) == 48);
  CHECK(fx::type_of_picker(46) == 49);
  CHECK(fx::type_of_picker(47) == 50);
  CHECK(fx::type_of_picker(48) == 51);  // CF2 appends Pan slide, Vibrato waveform, Tremolo waveform, Glissando, Retrigger, Tremor
  CHECK(fx::type_of_picker(49) == 52);
  CHECK(fx::type_of_picker(50) == 53);
  CHECK(fx::type_of_picker(51) == 54);
  CHECK(fx::type_of_picker(52) == 55);
  CHECK(fx::type_of_picker(53) == 56);
  CHECK(fx::type_of_picker(54) == 57);  // and Channel pan
  CHECK(fx::type_of_picker(55) == 58);  // CF3 appends Channel volume, Channel volume slide, Global volume, Global volume slide, Panbrello
  CHECK(fx::type_of_picker(56) == 59);
  CHECK(fx::type_of_picker(57) == 60);
  CHECK(fx::type_of_picker(58) == 61);
  CHECK(fx::type_of_picker(59) == 62);
  CHECK(fx::type_of_picker(60) == 96);  // OS-CEWTENV W6b appends w, y and z
  CHECK(fx::type_of_picker(61) == 97);
  CHECK(fx::type_of_picker(62) == 98);
  CHECK(fx::type_of_picker(63) == 0);
  CHECK(fx::picker_index(96) == 60);
  CHECK(fx::picker_index(98) == 62);
  CHECK(fx::row(96).hi == 255);  // w and y: two nibbles; z: 0..100, 50 (100 ms) to start
  CHECK(fx::row(97).hi == 255);
  CHECK(fx::row(98).lo == 0);
  CHECK(fx::row(98).hi == 100);
  CHECK(fx::row(98).start == 50);
  CHECK(seq::fx_kind(98) == seq::FxKind::Voice);
  CHECK(seq::fx_group(98, 0) == seq::FxGroup::GranLenFx);
  CHECK(seq::fx_group(98, 1) == seq::FxGroup::GranLenFx);
  CHECK(fx::type_of_picker(255) == 0);
  CHECK(fx::type_of_picker(-1) == -1);
  CHECK(fx::picker_index(38) == 41);
  CHECK(fx::picker_index(39) == 42);
  CHECK(fx::picker_index(46) == 43);
  CHECK(fx::picker_index(50) == 47);
  CHECK(fx::picker_index(56) == 53);
  CHECK(fx::picker_index(57) == 54);
  CHECK(fx::picker_index(58) == 55);
  CHECK(fx::picker_index(62) == 59);
  CHECK(fx::row(58).start == 64);   // a Channel volume picked on an empty cell starts at full
  CHECK(fx::row(60).hi == 64);
  CHECK(fx::picker_index(43) == 0);  // an undefined type is not in the picker
  // sanitize: idempotent, None carries no value, a placeholder reads as None, a value is clamped.
  for (unsigned t = 0; t < 256; ++t) {
    for (const unsigned v : {0u, 1u, 99u, 100u, 101u, 255u}) {
      std::uint8_t type = static_cast<std::uint8_t>(t), value = static_cast<std::uint8_t>(v);
      fx::sanitize(type, value);
      const std::uint8_t t1 = type, v1 = value;
      fx::sanitize(type, value);
      CHECK((type == t1 && value == v1));
      if (type == 0) CHECK(value == 0);
      if (type != 0) CHECK((value >= fx::row(type).lo && value <= fx::row(type).hi));
    }
  }
  CHECK(fx::is_random_eligible(2));
  CHECK_FALSE(fx::is_random_eligible(1));
  CHECK_FALSE(fx::is_random_eligible(13));
  CHECK_FALSE(fx::is_random_eligible(15));
  CHECK(fx::is_random_eligible(14));  // OS-PFX: Chord runs
  CHECK(fx::is_random_eligible(38));  // Overdrive runs (OS-IFX1)
  CHECK_FALSE(fx::is_random_eligible(43));
  CHECK_FALSE(fx::is_random_eligible(46));
}

TEST_CASE("fx table: Swing 17 and Gate Length 20 are executable, in the picker, and the sequencer runs both (OS-CFGWIRE)") {
  for (const std::uint8_t type : {std::uint8_t{17}, std::uint8_t{20}}) {
    CHECK_MESSAGE(fx::kRows[type].support == fx::Support::Executes, "type " << int(type));
    CHECK_MESSAGE(fx::is_executable(type), "type " << int(type));
    CHECK_MESSAGE(fx::picker_index(type) != 0, "type " << int(type) << " is in the picker");
    CHECK_MESSAGE(seq::fx_kind(type) == seq::FxKind::Row, "type " << int(type) << ": the sequencer consumes it at the start of the row");
    // The table and the sequencer say the same range (the sequencer's range of the same slot).
    CHECK(seq::kFxInfo[type].min == fx::kRows[type].lo);
    CHECK(seq::kFxInfo[type].max == fx::kRows[type].hi);
    std::uint8_t t = type;
    std::uint8_t v = 255;
    fx::sanitize(t, v);  // not a placeholder any more: the type stays and the value is clamped to its range
    CHECK(t == type);
    CHECK(v == fx::kRows[type].hi);
  }
  CHECK(fx::picker_index(17) == 34);
  CHECK(fx::picker_index(20) == 35);
}

TEST_CASE("fx table: Chord 14, Arp 21, Tune 40 and Slide up / down 41, 42 are executable, in the picker, appended in that order, and the sequencer says the same range (OS-PFX)") {
  const std::uint8_t types[5] = {14, 21, 40, 41, 42};
  for (unsigned i = 0; i < 5u; ++i) {
    const std::uint8_t type = types[i];
    CHECK_MESSAGE(fx::kRows[type].support == fx::Support::Executes, "type " << int(type));
    CHECK_MESSAGE(fx::is_executable(type), "type " << int(type));
    CHECK_MESSAGE(fx::picker_index(type) == 36u + i, "type " << int(type) << " is appended after Swing and Gate Length");
    CHECK(seq::kFxInfo[type].min == fx::kRows[type].lo);
    CHECK(seq::kFxInfo[type].max == fx::kRows[type].hi);
    // The kind the sequencer gives it: Chord, Arp and Tune are read when a note starts (Row), the Slides go to the voice.
    CHECK_MESSAGE(seq::fx_kind(type) == (type >= 41 ? seq::FxKind::Voice : seq::FxKind::Row), "type " << int(type));
    std::uint8_t t = type;
    std::uint8_t v = 255;
    fx::sanitize(t, v);
    CHECK(t == type);
    CHECK(v == fx::kRows[type].hi);
  }
  CHECK(fx::kRows[40].start == 24);  // Tune: 24 is no change
  CHECK(fx::kRows[14].start == 0);
  CHECK(fx::kRows[21].start == 0);
}

TEST_CASE("fx table: Chance 4 reaches 174 in the editor, the pads and Fill RANDOM stay in 0..100, the coarse walk follows F12 (OS-CECHANCE C2)") {
  const std::uint8_t ch = seq::fx::kChance;
  CHECK(fx::kRows[ch].hi == seq::fx::kChanceMax);
  CHECK(fx::kRows[ch].hi == 174u);
  CHECK(fx::random_hi(ch) == 100u);    // Chance draws in 0..100 only
  CHECK(fx::random_hi(2) == 100u);     // Micro-move: its whole range
  CHECK(fx::random_hi(15) == 200u);    // Tempo
  CHECK(fx::random_hi(41) == 255u);    // Slide up
  CHECK(fx::random_hi(46) == 255u);    // a tracker FX
  const edit::Rule one{true, true};
  edit::Last last;
  model::Step s = model::kEmptyStep;
  s.fx[0] = {ch, 100};
  for (int v = 101; v <= 174; ++v) {  // the plain turn walks every value up to 174
    edit::fx_value_delta(s, 0, 1, one, last);
    CHECK(s.fx[0].value == v);
  }
  edit::fx_value_delta(s, 0, 1, one, last);
  CHECK(s.fx[0].value == 174u);  // and stops there
  for (int v = 173; v >= 0; --v) {  // and back down to 0
    edit::fx_value_delta(s, 0, -1, one, last);
    CHECK(s.fx[0].value == v);
  }
  edit::fx_value_delta(s, 0, -1, one, last);
  CHECK(s.fx[0].value == 0u);
  // The coarse walk (a delta of more than one detent, F12 parity with tracker-ce): 0..100 by the delta clamped at 100, then 101, 102, 103, then +6 up to 174, and back.
  const auto coarse = [&](int from, int d) {
    s.fx[0] = {ch, static_cast<std::uint8_t>(from)};
    edit::fx_value_delta(s, 0, d, one, last);
    return static_cast<int>(s.fx[0].value);
  };
  CHECK(coarse(99, 16) == 100);
  CHECK(coarse(5, 300) == 100);
  CHECK(coarse(100, 16) == 101);
  CHECK(coarse(101, 16) == 102);
  CHECK(coarse(102, 16) == 103);
  CHECK(coarse(103, 16) == 109);
  CHECK(coarse(169, 16) == 174);
  CHECK(coarse(174, 16) == 174);  // stopped at the top
  CHECK(coarse(105, 16) == 111);  // a value a fine turn put between the coarse stops
  CHECK(coarse(174, -16) == 168);
  CHECK(coarse(108, -16) == 102);
  CHECK(coarse(103, -16) == 102);
  CHECK(coarse(102, -16) == 101);
  CHECK(coarse(101, -16) == 100);
  CHECK(coarse(100, -16) == 84);
  CHECK(coarse(5, -300) == 0);
  CHECK(coarse(105, -16) == 102);
  // The pads map 0..47 over 0..100 for Chance.
  s.fx[0] = {ch, 55};
  edit::fx_pad(s, 0, 0, one, last);
  CHECK(s.fx[0].value == 0u);
  edit::fx_pad(s, 0, 47, one, last);
  CHECK(s.fx[0].value == 100u);
  edit::fx_pad(s, 0, 24, one, last);
  CHECK(s.fx[0].value == (24 * 100) / 47);
  // An absolute value (the Fill's CONSTANT / FROM..TO, a key) reaches 174 and is kept.
  edit::fx_value_set(s, 0, 140, one, last);
  CHECK(s.fx[0].value == 140u);
  edit::fx_value_set(s, 0, 255, one, last);
  CHECK(s.fx[0].value == 174u);
  // A value carried into Chance by a type change is held to 174 too.
  s.fx[0] = {2, 200};
  edit::fx_type_delta(s, 0, 1);
  CHECK(s.fx[0].type == ch);
  CHECK(s.fx[0].value == 174u);
}

TEST_CASE("fx text: the words of Chance say what the sequencer reads, for every stored value 0..255 (OS-CECHANCE C2)") {
  // The sequencer reads a value after fx_sanitize (read_row), so a value past 174 reads as 174, and the info line has to say the same. The word forms are this OS's (the spec's
  // CHANCE 50 % / CHANCE WITH LAST / CHANCE NOT LAST / CHANCE PLAY 2 SKIP 3 / CHANCE SKIP 2 PLAY 3), not tracker-ce's grid labels.
  const auto words = [](std::uint8_t type, unsigned value, bool& said) {
    core::FixedString<32> s;
    said = app::append_fx_value(s, type, static_cast<std::uint8_t>(value));
    return std::string(s.data(), s.size());
  };
  for (unsigned v = 0; v < 256; ++v) {
    CAPTURE(v);
    bool said = false;
    std::uint8_t t = seq::fx::kChance;
    std::uint8_t r = static_cast<std::uint8_t>(v);
    seq::fx_sanitize(t, r);
    CHECK(t == seq::fx::kChance);
    const unsigned rv = r;
    std::string want;
    if (rv <= 100u) {
      want = "CHANCE " + std::to_string(rv) + " %";
    } else if (rv == seq::fx::kChanceWithLast) {
      want = "CHANCE WITH LAST";
    } else if (rv == seq::fx::kChanceNotLast) {
      want = "CHANCE NOT LAST";
    } else {
      std::uint32_t n = 0, m = 0;
      seq::fx::chance_cycle(static_cast<std::uint8_t>(rv), n, m);
      const bool skip_first = rv >= 139u;
      want = skip_first ? "CHANCE SKIP " : "CHANCE PLAY ";
      want += std::to_string(n);
      want += skip_first ? " PLAY " : " SKIP ";
      want += std::to_string(m);
    }
    CHECK(words(seq::fx::kChance, v, said) == want);
    CHECK(said);
  }
  // The words fit the info line's width (the 24 characters Chord's longest word uses).
  core::FixedString<24> line;
  CHECK(app::append_fx_value(line, seq::fx::kChance, 174));
  CHECK(line.size() <= 24u);
  CHECK(std::string(line.c_str()) == "CHANCE SKIP 6 PLAY 6");
  // The cycle words pinned as literals, not rebuilt from the same branches as the formatter (the loop above does that): the first value of each cycle
  // range, and one inside Play N / Skip M.
  bool said = false;
  CHECK(words(seq::fx::kChance, 103, said) == "CHANCE PLAY 1 SKIP 1");
  CHECK(words(seq::fx::kChance, 111, said) == "CHANCE PLAY 2 SKIP 3");
  CHECK(words(seq::fx::kChance, 139, said) == "CHANCE SKIP 1 PLAY 1");
  CHECK(said);
}

TEST_CASE("fx text: the words of Chord, Arp, Tune and Slide say what the sequencer reads, for every stored value 0..255 (OS-PFX)") {
  // The sequencer reads a value after fx_sanitize (read_row), so a value past the range reads as the range's end, and the info line has to say the same. The expectations are written from
  // docs/specs/OS-PFX.md (names with their offsets, 11 x direction + rate, microsteps per rate, value - 24, sixteenths), not taken from the formatter's tables.
  // The words after "CHORD ": the name where this OS gives one, then the label (a hex digit per offset above the root); the tables of docs/specs/OS-PFX.md (whether they match the official firmware is an open research question).
  static const char* const kChordWords[30] = {
      "OFF", "SUS2 27", "28", "36", "MIN 37", "MAJ 47",
      "48", "SUS4 57", "5A", "OPEN4 5C", "67", "OPEN5 7C",
      "7E", "279", "27A", "27B", "369", "36A",
      "378", "MIN6 379", "MIN7 37A", "37B", "MAJ6 479", "MAJ7 47A",
      "MAJMAJ7 47B", "489", "48A", "48B", "57A", "57B",
  };
  static const std::uint16_t kPeriods[11] = {384, 288, 192, 144, 96, 48, 24, 16, 12, 8, 6};  // microsteps between two arp notes, 48 to a step (docs/specs/OS-PFX.md)
  static const char* const kRates[11] = {"8 STEPS", "6 STEPS", "4 STEPS", "3 STEPS", "2 STEPS", "1 STEP", "1/2 STEP", "1/3 STEP", "1/4 STEP", "1/6 STEP", "1/8 STEP"};
  static const char* const kDirs[3] = {"UP", "DOWN", "RND"};
  const auto words = [](std::uint8_t type, unsigned value, bool& said) {
    core::FixedString<32> s;
    said = app::append_fx_value(s, type, static_cast<std::uint8_t>(value));
    return std::string(s.data(), s.size());
  };
  for (unsigned v = 0; v < 256; ++v) {
    CAPTURE(v);
    bool said = false;
    // Chord 14
    std::uint8_t t = seq::fx::kChord;
    std::uint8_t r = static_cast<std::uint8_t>(v);
    seq::fx_sanitize(t, r);
    CHECK(t == seq::fx::kChord);
    CHECK(r == (v > 29u ? 29u : v));
    CHECK(words(seq::fx::kChord, v, said) == std::string("CHORD ") + kChordWords[r]);
    CHECK(said);
    // Arp 21
    t = seq::fx::kArp;
    r = static_cast<std::uint8_t>(v);
    seq::fx_sanitize(t, r);
    CHECK(t == seq::fx::kArp);
    CHECK(r == (v > 33u ? 33u : v));
    std::string want = "ARP OFF";
    if (r != 0u) {
      const unsigned idx = r - 1u;
      const unsigned period = kPeriods[idx % 11u];
      CHECK(static_cast<unsigned>(seq::arp_dir(r)) == idx / 11u);
      CHECK(seq::arp_microsteps(r) == period);
      want = std::string("ARP ") + kDirs[idx / 11u] + " " + kRates[idx % 11u];
    }
    CHECK(words(seq::fx::kArp, v, said) == want);
    CHECK(said);
    // Tune 40
    t = seq::fx::kTune;
    r = static_cast<std::uint8_t>(v);
    seq::fx_sanitize(t, r);
    CHECK(t == seq::fx::kTune);
    CHECK(r == (v > 48u ? 48u : v));
    const int st = static_cast<int>(r) - 24;
    CHECK(seq::tune_semitones(r) == st);
    CHECK(words(seq::fx::kTune, v, said) == std::string("TUNE ") + (st > 0 ? "+" : "") + std::to_string(st) + " ST");
    CHECK(said);
    // Slide up 41 / down 42: sixteenths of a semitone, shown in hundredths rounded half up
    for (std::uint8_t type : {seq::fx::kSlideUp, seq::fx::kSlideDown}) {
      t = type;
      r = static_cast<std::uint8_t>(v);
      seq::fx_sanitize(t, r);
      CHECK(t == type);
      CHECK(r == v);
      CHECK(seq::slide_sixteenths(type, r) == (type == seq::fx::kSlideUp ? static_cast<int>(v) : -static_cast<int>(v)));
      const unsigned h = (v * 100u + 8u) / 16u;
      std::string frac = std::to_string(h % 100u);
      if (frac.size() < 2u) {
        frac = "0" + frac;
      }
      CHECK(words(type, v, said) == std::string(type == seq::fx::kSlideUp ? "SLIDE UP " : "SLIDE DOWN ") + std::to_string(h / 100u) + "." + frac + " ST");
      CHECK(said);
    }
  }
  // Any other FX has no words: nothing is appended and the answer is false.
  bool said = true;
  CHECK(words(seq::fx::kRoll, 5, said).empty());
  CHECK_FALSE(said);
}

TEST_CASE("fx text: the words of the tracker FX say what the engine reads, for every stored value 0..255 (OS-COREFX)") {
  // Written from docs/specs/OS-COREFX.md and ADR 0004 amendment 2: x is the high nibble, y the low one; the slides take x first (MOD Axy: x up, else y down); the fine portamento counts quarters of a Paula period unit.
  const auto words = [](std::uint8_t type, unsigned value, bool& said) {
    core::FixedString<32> s;
    said = app::append_fx_value(s, type, static_cast<std::uint8_t>(value));
    return std::string(s.data(), s.size());
  };
  for (unsigned v = 0; v < 256; ++v) {
    CAPTURE(v);
    bool said = false;
    const unsigned x = v >> 4, y = v & 15u;
    CHECK(words(seq::fx::kTrkChanPan, v, said) == "CHPAN " + std::to_string(v > 100u ? 100u : v));  // Channel pan: the stored value held to 0..100
    CHECK(said);
    // CF3: the volumes are held to 0..64; the slides read Fy / xF as the fine forms, then the channel's goes down first (y) and the global one up first (x); the panbrello is speed x, depth y (a zero depth keeps the last)
    CHECK(words(seq::fx::kTrkChanVol, v, said) == "CHVOL " + std::to_string(v > 64u ? 64u : v));
    CHECK(said);
    CHECK(words(seq::fx::kTrkGlobalVol, v, said) == "GVOL " + std::to_string(v > 64u ? 64u : v));
    CHECK(said);
    for (const std::uint8_t type : {seq::fx::kTrkChanVolSlide, seq::fx::kTrkGlobalVolSlide}) {
      const bool chan = type == seq::fx::kTrkChanVolSlide;
      std::string want = chan ? "CHVSL " : "GVSL ";
      if (y == 15u && x != 0u) {
        want += "FINE +" + std::to_string(x);
      } else if (x == 15u && y != 0u) {
        want += "FINE -" + std::to_string(y);
      } else if (chan ? y != 0u : x == 0u && y != 0u) {
        want += "-" + std::to_string(y);
      } else if (x != 0u) {
        want += "+" + std::to_string(x);
      } else {
        want += "0";
      }
      CHECK(words(type, v, said) == want);
      CHECK(said);
    }
    CHECK(words(seq::fx::kTrkPanbrello, v, said) == "PANBR SPD " + (x != 0u ? std::to_string(x) : std::string("KEEP")) + " DEP " + (y != 0u ? std::to_string(y) : std::string("KEEP")));
    CHECK(said);
    CHECK(words(seq::fx::kTrkArp, v, said) == (v == 0u ? std::string("ARPEG OFF") : "ARPEG +" + std::to_string(x) + " +" + std::to_string(y)));
    CHECK(said);
    for (const std::uint8_t type : {seq::fx::kTrkVolSlide, seq::fx::kTrkFineVol}) {
      std::string want = type == seq::fx::kTrkVolSlide ? "VSLIDE " : "FVOL ";
      want += x != 0u ? "+" + std::to_string(x) : (y != 0u ? "-" + std::to_string(y) : std::string("0"));
      CHECK(words(type, v, said) == want);
      CHECK(said);
    }
    for (const std::uint8_t type : {seq::fx::kTrkFinePortaUp, seq::fx::kTrkFinePortaDown}) {
      const unsigned h = v * 25u;  // hundredths of a period unit
      std::string frac = std::to_string(h % 100u);
      if (frac.size() < 2u) {
        frac = "0" + frac;
      }
      CHECK(words(type, v, said) == std::string(type == seq::fx::kTrkFinePortaUp ? "FINE UP " : "FINE DOWN ") + std::to_string(h / 100u) + "." + frac + " PER");
      CHECK(said);
    }
  }
}

TEST_CASE("fx text: the words of w, y and z fit the 24-character info line for every stored value, and the glyphs (OS-CEWTENV W6b)") {
  const auto words = [](std::uint8_t type, unsigned value, bool& said) {
    core::FixedString<24> s;  // the Pattern screen's info line
    said = app::append_fx_value(s, type, static_cast<std::uint8_t>(value));
    return std::string(s.data(), s.size());
  };
  CHECK(app::fx_type_char(seq::fx::kAmpEnvFx) == 'w');
  CHECK(app::fx_type_char(seq::fx::kFilterEnvFx) == 'y');
  CHECK(app::fx_type_char(seq::fx::kGranLenFx) == 'z');
  CHECK(app::fx_type_char(99) == '?');
  for (unsigned v = 0; v < 256; ++v) {
    CAPTURE(v);
    bool said = false;
    const unsigned x = v >> 4, y = v & 15u;
    const int warp = static_cast<int>(y) - 8;
    const std::string w = words(seq::fx::kAmpEnvFx, v, said);
    CHECK(said);
    CHECK(w == "AMP ENV x" + std::to_string(x) + " WARP " + (warp < 0 ? "-" : "+") + std::to_string(std::min(warp < 0 ? -warp : warp, 7)));
    CHECK(w.size() <= 24u);
    const std::string f = words(seq::fx::kFilterEnvFx, v, said);
    CHECK(said);
    CHECK(f == "FLT ENV " + std::to_string(x == 0u ? 1u : x * 8u) + "/" + std::to_string(y * 8u) + " MS");
    CHECK(f.size() <= 24u);
    const std::string z = words(seq::fx::kGranLenFx, v, said);
    CHECK(said);
    CHECK(z.rfind("GRAIN ", 0) == 0u);
    CHECK(z.size() <= 24u);
  }
  bool said = false;
  CHECK(words(seq::fx::kAmpEnvFx, 0x88, said) == "AMP ENV x8 WARP +0");
  CHECK(words(seq::fx::kGranLenFx, 0, said) == "GRAIN 1 MS");  // 44 samples
  CHECK(words(seq::fx::kGranLenFx, 50, said) == "GRAIN 100 MS");  // 4,410 samples
  CHECK(words(seq::fx::kGranLenFx, 100, said) == "GRAIN 1000 MS");  // 44,100 samples
  CHECK(words(seq::fx::kGranLenFx, 200, said) == "GRAIN 1000 MS");  // held to 100
}

TEST_CASE("scales: 40 masks and names, four rows corrected, is_in_scale exhaustive against the reference vectors") {
  CHECK(core::kScaleCount == 40u);
  CHECK(core::kScaleNameChars == 11u);  // OS-SCALES28 Q4c: the display names, at most 11 characters
  for (unsigned i = 0; i < core::kScaleCount; ++i) {
    CHECK_MESSAGE((core::kScaleDef[i] == vectors::kScaleMask[i]) == !core::scale_corrected(i), "scale " << i);  // the reference masks, but the four rows PAR-073 names (OS-PTOOLS)
    CHECK((core::kScaleDef[i] & 1u) != 0u);  // the root is always in
    CHECK(core::kScaleDef[i] < 4096u);
    CHECK(std::strlen(core::kScaleNames[i]) <= core::kScaleNameChars);
    CHECK(core::kScaleNames[i][0] != '\0');
    CHECK(core::kScaleNames[i][std::strlen(core::kScaleNames[i]) - 1] != ' ');  // no trailing space (the old "Phrygian " had one)
    CHECK(core::scale_corrected(i) == (i == 11 || i == 17 || i == 23 || i == 36));
  }
  // OS-SCALES28 Q4c: one name table, the display names of the official order (spec section 3.1), shared by the Note panel and the Fill dialog.
  CHECK(std::string(core::kScaleNames[0]) == "No scale");
  CHECK(std::string(core::kScaleNames[5]) == "Lydian");
  CHECK(std::string(core::kScaleNames[8]) == "Phrygian");
  CHECK(std::string(core::kScaleNames[11]) == "Mel. minor");
  CHECK(std::string(core::kScaleNames[12]) == "Harm. minor");
  CHECK(std::string(core::kScaleNames[16]) == "Blues");
  CHECK(std::string(core::kScaleNames[17]) == "Blues major");
  CHECK(std::string(core::kScaleNames[20]) == "Hung. minor");
  CHECK(std::string(core::kScaleNames[23]) == "Todi");
  CHECK(std::string(core::kScaleNames[24]) == "Whole tone");
  CHECK(std::string(core::kScaleNames[36]) == "Gypsy minor");
  CHECK(std::string(core::kScaleNames[39]) == "Enigmatic");
  CHECK(core::kScaleDef[4] != core::kScaleDef[11]);        // Dorian and Melo Min no longer share a mask
  // OS-PTOOLS, PAR-073: the four corrected rows are the standard scales (semitones above the root).
  const auto semis = [](unsigned scale) {
    std::string out;
    for (unsigned k = 0; k < 12; ++k) {
      if ((core::kScaleDef[scale] & (1u << k)) != 0u) {
        out += std::to_string(k) + " ";
      }
    }
    return out;
  };
  CHECK(semis(11) == "0 2 3 5 7 9 11 ");  // melodic minor
  CHECK(semis(17) == "0 2 3 4 7 9 ");      // blues major
  CHECK(semis(23) == "0 1 3 6 7 8 11 ");   // Todi
  CHECK(semis(36) == "0 2 3 6 7 8 11 ");   // Gypsy minor (= Hungarian minor, row 20)
  CHECK(core::kScaleDef[36] == core::kScaleDef[20]);
  CHECK_FALSE(core::scale_corrected(40));
  unsigned group = 0;
  for (unsigned scale = 0; scale < 45; ++scale) {
    if (core::scale_corrected(scale)) {
      continue;  // differs from the reference mask on purpose (OS-PTOOLS)
    }
    vec::Digest dg;
    for (unsigned note = 0; note < 256; ++note) {
      for (unsigned root = 0; root < 256; root += (root < 64 ? 1 : 17)) {
        const bool in = core::is_in_scale(static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(root), static_cast<std::uint8_t>(scale));
        dg.add_bytes(reinterpret_cast<const std::uint8_t*>(&in), 1);
      }
    }
    CHECK_MESSAGE(dg.value() == vectors::kScaleDigest[group], "scale " << scale);
    ++group;
  }
  // A few by hand: C major (scale 3, root 0) has C D E F G A B.
  const bool c_major[12] = {true, false, true, false, true, true, false, true, false, true, false, true};
  for (unsigned n = 0; n < 12; ++n) {
    CHECK(core::is_in_scale(static_cast<std::uint8_t>(n), 0, 3) == c_major[n]);
    CHECK(core::is_in_scale(static_cast<std::uint8_t>(n + 12 * 5), 0, 3) == c_major[n]);  // any octave
  }
}

// OS-SCALES28: the core table is "No scale" plus the official firmware's 39 scales in the official order (core index = official index + 1), so the 39 masks are pinned here
// against the official rows of docs/specs/OS-SCALES28.md section 3.1 (facts from tracker-ce@7fd6c2c:core/source/scales.h:11-51, no code taken); Todi (core 23, official 22) is
// the one row that differs on purpose (kept standard, OS-PTOOLS).
TEST_CASE("scales: the 39 masks are the official rows in the official order (Todi excepted)") {
  const std::uint16_t official[39] = {
      0x0FFF, 0x05AD, 0x0AB5, 0x06AD, 0x0AD5, 0x05D5, 0x056B, 0x05AB,
      0x05B3, 0x06B5, 0x0AAD, 0x09AD, 0x0BB5, 0x06BD, 0x0EB5, 0x04E9,
      0x029D, 0x04A9, 0x0295, 0x09CD, 0x06CD, 0x0AD3, 0x05AB, 0x0555,
      0x0B6D, 0x055B, 0x018D, 0x04A3, 0x02A5, 0x0463, 0x0B6D, 0x028D,
      0x06D5, 0x09B3, 0x05B1, 0x09CD, 0x0AAB, 0x09AB, 0x0D53,
  };
  static_assert(sizeof official / sizeof official[0] == 39, "the 39 official scales");
  for (unsigned i = 0; i < 39; ++i) {
    if (i == 22) {  // Todi: the official row is a copy of its Phrygian; ours keeps the standard definition
      CHECK(core::kScaleDef[i + 1] == 0x09CBu);  // 0 1 3 6 7 8 11
      CHECK(core::kScaleDef[i + 1] != official[i]);
    } else {
      CHECK_MESSAGE(core::kScaleDef[i + 1] == official[i], "official scale " << i);
    }
  }
}

// The note tiles against the reference table's names (vectors::kNoteNames, the reference model's table): one transform of its spelling into the three characters of a
// tile, written here from the table and not from the code under test. A natural note gets a dash before its octave digit (the table's "C2" is
// "C-2": a fixed width), a sharp keeps its '#' ("C#2"); the tenth octave has two digits, so a natural there keeps the table's spelling ("C10")
// and a sharp, four characters in the table, takes a lowercase letter ("C#10" is "c10").
TEST_CASE("note tiles: every one of the 128 notes is named as the reference table names it, and fade, cut and off say FAD, CUT, OFF") {
  for (unsigned note = 1; note <= 128; ++note) {
    const std::string og_name = vectors::kNoteNames[note - 1];
    const bool sharp = og_name[1] == '#';
    const std::string octave = og_name.substr(sharp ? 2 : 1);
    std::string want;
    if (octave.size() == 1) {
      want = std::string{og_name[0], sharp ? '#' : '-', octave[0]};
    } else {
      want = std::string{sharp ? static_cast<char>(og_name[0] - 'A' + 'a') : og_name[0]} + octave;
    }
    char text[4];
    ui::note_tile_text(app::note_tile_cell(static_cast<std::uint8_t>(note)), text);
    CHECK_MESSAGE(std::string(text) == want, "note " << note << " (" << og_name << ")");
    model::Step s = model::kEmptyStep;
    s.note = static_cast<std::uint8_t>(note);
    char field[4];
    CHECK_FALSE(app::format_field(s, app::Column::Note, field));
    CHECK_MESSAGE(std::string(field) == want, "note " << note);
  }
  const struct {
    std::uint8_t note;
    const char* name;
  } specials[] = {{model::kNoteFade, "FAD"}, {model::kNoteCut, "CUT"}, {model::kNoteOff, "OFF"}};
  for (const auto& sp : specials) {
    char text[4];
    ui::note_tile_text(app::note_tile_cell(sp.note), text);
    CHECK(std::string(text) == sp.name);
  }
  // Nothing else is a note: 129..252 are not named.
  for (unsigned note = 129; note < 253; ++note) {
    char text[4];
    ui::note_tile_text(app::note_tile_cell(static_cast<std::uint8_t>(note)), text);
    CHECK(std::string(text) == "???");
  }
  // Every cell of every note is inside the tile set (a cell past the end would be drawn as "???").
  for (unsigned note = 0; note < 256; ++note) {
    CHECK(app::note_tile_cell(static_cast<std::uint8_t>(note)) < ui::kNoteTiles);
  }
}
