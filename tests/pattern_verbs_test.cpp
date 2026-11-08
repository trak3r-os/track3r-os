// SPDX-License-Identifier: GPL-3.0-or-later
// OS-PTOOLS2 S4: the four verbs the Pattern tools lacked (model/pattern_verbs.h): INTERPOLATE (the ramp's endpoints, rounding, monotonic, what it leaves alone), RANDOMISE in place (chance and amount bounds,
// seed determinism, only valued steps move), ROTATE (wrap, up then down, field masks), HUMANISE (the same steps as the HUMANIZE app), and `apply_block`: one undo unit per verb, exact undo, never past a track's end.
#include "support/need.h"

#include <doctest/doctest.h>

#include <o3os/core/action.h>
#include <o3os/model/editor.h>
#include <o3os/model/fx_table.h>
#include <o3os/model/pattern_verbs.h>
#include <o3os/model/project.h>
#include <o3os/model/undo.h>

#include <cstdint>
#include <cstring>
#include <string>

using namespace o3os;
using namespace o3os::model;
namespace vb = o3os::model::verbs;

namespace {

bool same(const Step& a, const Step& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

void empty_column(Step* col) {
  for (unsigned i = 0; i < kMaxPatternLength; ++i) {
    col[i] = kEmptyStep;
  }
}

Step noted(unsigned note, unsigned vol = kVolumeUnset, unsigned instr = 1) {
  Step s = kEmptyStep;
  s.note = static_cast<std::uint8_t>(note);
  s.instr = static_cast<std::uint8_t>(instr);
  s.vol = static_cast<std::uint8_t>(vol);
  return s;
}

std::string vols(const Step* s, unsigned n) {
  std::string r;
  for (unsigned i = 0; i < n; ++i) {
    r += (s[i].vol == kVolumeUnset ? std::string("--") : std::to_string(s[i].vol)) + " ";
  }
  return r;
}

}  // namespace

// ---- INTERPOLATE ------------------------------------------------------------------------------------------------------------

TEST_CASE("interpolate: a volume ramp from the first to the last value over the rows that hold a note, the anchors untouched, note-less rows left alone") {
  Step c[kMaxPatternLength];
  empty_column(c);
  c[0] = noted(61, 100);
  c[1] = noted(62);  // a note, no volume: takes the ramp
  c[2] = noted(63);
  c[3] = kEmptyStep;  // no note: left alone, counted
  c[4] = noted(65, 0);
  const vb::Count r = vb::interpolate(vb::Column::Vol, c, 5);
  CHECK(vols(c, 5) == "100 75 50 -- 0 ");
  CHECK(r.changed == 2);
  CHECK(r.skipped == 1);
  CHECK(c[0].note == 61);  // (only the column changes)
  CHECK(c[3].note == kNoteEmpty);
  CHECK(c[5].vol == kVolumeUnset);  // past the range: as it was
}

TEST_CASE("interpolate: rounds to the nearest, halves away from zero, up and down") {
  Step c[kMaxPatternLength];
  empty_column(c);
  for (unsigned i = 0; i < 3; ++i) {
    c[i] = noted(61);  // the VOL ramp writes rows that hold a note
  }
  c[0].vol = 0;
  c[2].vol = 3;  // 0, 1.5, 3
  (void)vb::interpolate(vb::Column::Vol, c, 3);
  CHECK(c[1].vol == 2);
  empty_column(c);
  for (unsigned i = 0; i < 3; ++i) {
    c[i] = noted(61);
  }
  c[0].vol = 3;
  c[2].vol = 0;  // 3, 1.5, 0
  (void)vb::interpolate(vb::Column::Vol, c, 3);
  CHECK(c[1].vol == 1);
  empty_column(c);
  for (unsigned i = 0; i < 4; ++i) {
    c[i] = noted(61);
  }
  c[0].vol = 0;
  c[3].vol = 10;  // 0, 3.33, 6.67, 10
  (void)vb::interpolate(vb::Column::Vol, c, 4);
  CHECK(vols(c, 4) == "0 3 7 10 ");
}

TEST_CASE("interpolate: monotonic between the anchors, whatever the anchors and the distance; the range stays inside the anchors") {
  for (unsigned a = 0; a <= 127; a += 9) {
    for (unsigned b = 0; b <= 127; b += 11) {
      for (unsigned n : {2u, 3u, 7u, 16u, 128u}) {
        Step c[kMaxPatternLength];
        empty_column(c);
        for (unsigned i = 0; i < n; ++i) {
          c[i] = noted(61);  // every row holds a note, so the VOL ramp writes it
        }
        c[0].vol = static_cast<std::uint8_t>(a);
        c[n - 1].vol = static_cast<std::uint8_t>(b);
        (void)vb::interpolate(vb::Column::Vol, c, n);
        CHECK(c[0].vol == a);
        CHECK(c[n - 1].vol == b);
        for (unsigned i = 1; i < n; ++i) {
          if (b >= a) {
            CHECK(c[i].vol >= c[i - 1].vol);
          } else {
            CHECK(c[i].vol <= c[i - 1].vol);
          }
          CHECK(c[i].vol >= (a < b ? a : b));
          CHECK(c[i].vol <= (a < b ? b : a));
        }
      }
    }
  }
}

TEST_CASE("interpolate: the anchors are the first and the last value, an unset volume is no anchor, existing values between are replaced") {
  Step c[kMaxPatternLength];
  empty_column(c);
  c[1] = noted(61, 20);
  c[2] = noted(61);  // no volume: takes the ramp
  c[3] = noted(62, 99);  // replaced
  c[4] = noted(63, 200);  // untrusted (128..254): not an anchor, and replaced by the ramp
  c[5] = noted(64);
  c[6] = noted(65, 60);
  c[7] = noted(66);  // a note with no volume: not an anchor
  (void)vb::interpolate(vb::Column::Vol, c, 8);
  CHECK(c[0].vol == kVolumeUnset);  // before the first anchor
  CHECK(c[1].vol == 20);
  CHECK(c[6].vol == 60);
  CHECK(c[7].vol == kVolumeUnset);  // after the last anchor
  CHECK(c[3].vol == 36);
  CHECK(c[2].vol == 28);
  CHECK(c[4].vol == 44);
  CHECK(c[5].vol == 52);
}

TEST_CASE("interpolate: fewer than two anchors, or both on one step, change nothing") {
  Step c[kMaxPatternLength];
  empty_column(c);
  CHECK(vb::interpolate(vb::Column::Vol, c, 16).changed == 0);
  c[3] = noted(61, 50);
  CHECK(vb::interpolate(vb::Column::Vol, c, 16).changed == 0);
  CHECK(c[4].vol == kVolumeUnset);
  c[9] = noted(62, 50);  // two anchors with the same value: the noted rows between take it
  for (unsigned i = 4; i <= 8; ++i) {
    c[i] = noted(63);  // notes, no volume
  }
  CHECK(vb::interpolate(vb::Column::Vol, c, 16).changed == 5);
  CHECK(vols(c + 3, 7) == "50 50 50 50 50 50 50 ");
  CHECK(vb::interpolate(vb::Column::Vol, c, 16).changed == 0);  // again: nothing left to change
  CHECK(vb::interpolate(vb::Column::Vol, c, 0).changed == 0);
}

TEST_CASE("interpolate: a note ramp is chromatic; a cut, a fade, an off and a reserved byte stay; an empty row takes the first anchor's instrument") {
  Step c[kMaxPatternLength];
  empty_column(c);
  c[0] = noted(61, kVolumeUnset, 5);
  c[8] = noted(69);
  c[2].note = kNoteCut;
  c[4].note = kNoteOff;
  c[5].note = kNoteFade;
  c[6].note = 200;  // reserved
  c[3] = noted(10, 77, 9);  // an existing note is replaced; its instrument and volume stay
  const vb::Count r = vb::interpolate(vb::Column::Note, c, 9);
  CHECK(c[0].note == 61);
  CHECK(c[8].note == 69);
  CHECK(c[1].note == 62);
  CHECK(c[7].note == 68);
  CHECK(c[1].instr == 5);
  CHECK(c[7].instr == 5);
  CHECK(c[3].note == 64);
  CHECK(c[3].instr == 9);
  CHECK(c[3].vol == 77);
  CHECK(c[2].note == kNoteCut);
  CHECK(c[4].note == kNoteOff);
  CHECK(c[5].note == kNoteFade);
  CHECK(c[6].note == 200);
  CHECK(r.skipped == 4);
  CHECK(r.changed == 3);
}

TEST_CASE("interpolate: an instrument ramp stays in 1..64, and writes only the rows that hold a note") {
  Step c[kMaxPatternLength];
  empty_column(c);
  c[0] = noted(61);
  c[0].instr = 1;
  c[1] = noted(62);
  c[2] = kEmptyStep;  // no note: left alone, counted
  c[3] = noted(63);
  c[4] = noted(64);
  c[4].instr = 64;
  const vb::Count r = vb::interpolate(vb::Column::Instr, c, 5);
  CHECK(c[1].instr == 17);
  CHECK(c[3].instr == 48);
  CHECK(c[2].instr == 0);  // the note-less row keeps no instrument
  CHECK(r.skipped == 1);
  CHECK(r.changed == 2);
  CHECK(c[1].note == 62);  // the notes stay
  for (unsigned i = 0; i < 5; ++i) {
    if (i != 2u) {
      CHECK((c[i].instr >= 1 && c[i].instr <= 64));
    }
  }
}

TEST_CASE("interpolate: an FX ramp keeps the first anchor's type; the last anchor is the last step of that type; another type between stays") {
  Step c[kMaxPatternLength];
  empty_column(c);
  c[0].fx[1] = {31, 0};  // Panning, 0..100
  c[2].fx[1] = {7, 33};   // another type: stays
  c[6].fx[1] = {31, 100};
  c[7].fx[1] = {18, 5};   // a later anchor of another type: not the end of the ramp
  const vb::Count r = vb::interpolate(vb::Column::Fx2, c, 8);
  CHECK(r.skipped == 1);
  CHECK((c[1].fx[1].type == 31 && c[1].fx[1].value == 17));
  CHECK((c[2].fx[1].type == 7 && c[2].fx[1].value == 33));
  CHECK((c[3].fx[1].type == 31 && c[3].fx[1].value == 50));
  CHECK((c[5].fx[1].type == 31 && c[5].fx[1].value == 83));
  CHECK((c[7].fx[1].type == 18 && c[7].fx[1].value == 5));
  CHECK(c[1].fx[0].type == kFxNone);  // the other column is not touched
  // FX1 is its own column
  empty_column(c);
  c[0].fx[0] = {18, 10};
  c[2].fx[0] = {18, 30};
  (void)vb::interpolate(vb::Column::Fx1, c, 3);
  CHECK((c[1].fx[0].type == 18 && c[1].fx[0].value == 20));
}

TEST_CASE("interpolate: an INSTR / VOL anchor also holds a note: a stale value on a note-less row never sets an endpoint") {
  Step c[kMaxPatternLength];
  empty_column(c);
  c[0].vol = 100;          // no note: not an anchor
  c[0].instr = 64;         // no note: not an anchor either
  c[2] = noted(61, 0);     // the first real anchor (vol 0, with a note)
  c[3] = noted(62);
  c[4] = noted(63);
  c[5] = noted(64);
  c[6] = noted(65, 80);    // the last anchor
  const vb::Count r = vb::interpolate(vb::Column::Vol, c, 8);
  CHECK(c[0].vol == 100);  // outside the ramp (before the first anchor): untouched
  CHECK(c[3].vol == 20);   // the ramp 0..80 over the noted rows 3..5
  CHECK(c[4].vol == 40);
  CHECK(c[5].vol == 60);
  CHECK(r.changed == 3);
  // with only the note-less row holding a value, there is no anchor pair and nothing changes
  empty_column(c);
  c[0].vol = 100;
  c[4].vol = 10;
  CHECK(vb::interpolate(vb::Column::Vol, c, 8).changed == 0);
  CHECK(c[0].vol == 100);
  CHECK(c[4].vol == 10);
  // the same for INSTR
  empty_column(c);
  c[0].instr = 64;  // no note
  c[2] = noted(61);
  c[2].instr = 1;
  c[3] = noted(62);
  c[4] = noted(63);
  c[5] = noted(64);
  c[6] = noted(65);
  c[6].instr = 41;
  const vb::Count ri = vb::interpolate(vb::Column::Instr, c, 8);
  CHECK(c[0].instr == 64);
  CHECK(c[3].instr == 11);  // the ramp 1..41 over rows 3..5
  CHECK(c[4].instr == 21);
  CHECK(c[5].instr == 31);
  CHECK(ri.changed == 3);
}

TEST_CASE("interpolate: an FX anchor is a type the engine executes (fx::is_executable), not just any non-None type") {
  Step c[kMaxPatternLength];
  empty_column(c);
  c[0].fx[1] = {100, 5};  // unknown (>= kTypeCount): holds a value but is not executable: not an anchor, and outside the ramp it stays
  c[2].fx[1] = {31, 0};   // the first executable anchor
  c[4].fx[1] = {100, 9};  // not executable: not the last anchor either
  c[6].fx[1] = {31, 100};
  const vb::Count r = vb::interpolate(vb::Column::Fx2, c, 8);
  CHECK((c[0].fx[1].type == 100 && c[0].fx[1].value == 5));  // before the first anchor: untouched
  CHECK((c[3].fx[1].type == 31 && c[3].fx[1].value == 25));
  CHECK((c[4].fx[1].type == 100 && c[4].fx[1].value == 9));  // not an anchor, and a different type: stays
  CHECK((c[5].fx[1].type == 31 && c[5].fx[1].value == 75));
  CHECK((c[2].fx[1].value == 0 && c[6].fx[1].value == 100));
  CHECK(r.skipped == 1);
  CHECK(r.changed == 2);
}

// ---- RANDOMISE --------------------------------------------------------------------------------------------------------------

TEST_CASE("randomise: only steps that hold a value move, within AMOUNT, clamped to the column; nothing is ever added") {
  for (unsigned amount : {1u, 5u, 64u}) {
    Step c[kMaxPatternLength];
    empty_column(c);
    for (unsigned i = 0; i < 128; ++i) {
      if (i % 3 != 0) {
        c[i] = noted(1 + (i * 5) % 128, i % 7 == 0 ? kVolumeUnset : (i * 3) % 128, 1 + i % 64);
      }
    }
    Step before[kMaxPatternLength];
    std::memcpy(before, c, sizeof c);
    const vb::Count r = vb::randomise(amount, 100, 42, 0, vb::Column::Vol, c, 128);
    unsigned changed = 0;
    for (unsigned i = 0; i < 128; ++i) {
      CHECK(c[i].note == before[i].note);
      CHECK(c[i].instr == before[i].instr);
      CHECK(std::memcmp(&c[i].fx, &before[i].fx, sizeof c[i].fx) == 0);
      if (before[i].vol == kVolumeUnset) {
        CHECK(c[i].vol == kVolumeUnset);  // never gains a value
        continue;
      }
      CHECK(c[i].vol <= kVolumeMax);
      const int d = static_cast<int>(c[i].vol) - static_cast<int>(before[i].vol);
      CHECK(d <= static_cast<int>(amount));
      CHECK(-d <= static_cast<int>(amount));
      changed += d != 0 ? 1u : 0u;
    }
    CHECK(r.changed == changed);
    CHECK(changed > 0);
  }
}

TEST_CASE("randomise: notes stay 1..128, instruments 1..64, an FX value inside its type's range") {
  Step c[kMaxPatternLength];
  empty_column(c);
  c[0] = noted(1, 0, 1);
  c[1] = noted(128, 127, 64);
  c[2] = noted(kNoteCut);
  c[3] = noted(kNoteOff);
  for (unsigned seed = 0; seed < 50; ++seed) {
    (void)vb::randomise(64, 100, seed, 0, vb::Column::Note, c, 4);
    (void)vb::randomise(64, 100, seed, 0, vb::Column::Instr, c, 4);
    (void)vb::randomise(64, 100, seed, 0, vb::Column::Vol, c, 4);
    CHECK((c[0].note >= 1 && c[0].note <= 128));
    CHECK((c[1].note >= 1 && c[1].note <= 128));
    CHECK((c[0].instr >= 1 && c[0].instr <= 64));
    CHECK((c[1].instr >= 1 && c[1].instr <= 64));
    CHECK(c[0].vol <= 127);
    CHECK(c[1].vol <= 127);
    CHECK(c[2].note == kNoteCut);  // a cut is no value
    CHECK(c[3].note == kNoteOff);
  }
  empty_column(c);
  c[0].fx[0] = {17, 50};   // Swing 25..75
  c[1].fx[1] = {39, 8};    // Bit Depth 1..16
  c[2].fx[0] = {fx::kChanceType, 150};  // a Chance condition: not a number to move
  c[3].fx[0] = {99, 7};    // not an executable type
  for (unsigned seed = 0; seed < 50; ++seed) {
    (void)vb::randomise(64, 100, seed, 1, vb::Column::Fx1, c, 4);
    (void)vb::randomise(64, 100, seed, 1, vb::Column::Fx2, c, 4);
    CHECK((c[0].fx[0].value >= 25 && c[0].fx[0].value <= 75));
    CHECK((c[1].fx[1].value >= 1 && c[1].fx[1].value <= 16));
    CHECK(c[0].fx[0].type == 17);
    CHECK(c[2].fx[0].value == 150);
    CHECK(c[3].fx[0].value == 7);
  }
  // Chance drawn: a probability stays in 0..100
  empty_column(c);
  c[0].fx[0] = {fx::kChanceType, 99};
  for (unsigned seed = 0; seed < 50; ++seed) {
    (void)vb::randomise(64, 100, seed, 0, vb::Column::Fx1, c, 1);
    CHECK(c[0].fx[0].value <= 100);
  }
}

TEST_CASE("randomise: CHANCE 0 and AMOUNT 0 change nothing; CHANCE 100 picks every valued step; the share follows CHANCE") {
  Step c[kMaxPatternLength];
  empty_column(c);
  for (unsigned i = 0; i < 128; ++i) {
    c[i].vol = 64;
  }
  Step before[kMaxPatternLength];
  std::memcpy(before, c, sizeof c);
  CHECK(vb::randomise(10, 0, 3, 0, vb::Column::Vol, c, 128).changed == 0);
  CHECK(vb::randomise(0, 100, 3, 0, vb::Column::Vol, c, 128).changed == 0);
  CHECK(std::memcmp(before, c, sizeof c) == 0);
  // amount 1: a step moves unless it draws 0 (1 in 3), so CHANCE 100 changes about two thirds of the 128 steps; CHANCE 50 about a third; with chance 0..100 the count grows with the chance
  const unsigned all = vb::randomise(1, 100, 3, 0, vb::Column::Vol, c, 128).changed;
  std::memcpy(c, before, sizeof c);
  const unsigned half = vb::randomise(1, 50, 3, 0, vb::Column::Vol, c, 128).changed;
  CHECK(all > 60);
  CHECK(all < 110);
  CHECK(half > 20);
  CHECK(half < all);
}

TEST_CASE("randomise: the same seed and stream give the same steps, another seed or stream do not; a step does not depend on the others") {
  Step a[kMaxPatternLength];
  Step b[kMaxPatternLength];
  empty_column(a);
  for (unsigned i = 0; i < 128; ++i) {
    a[i].vol = static_cast<std::uint8_t>(20 + i / 2);
  }
  std::memcpy(b, a, sizeof a);
  Step c[kMaxPatternLength];
  std::memcpy(c, a, sizeof a);
  (void)vb::randomise(10, 100, 7, 2, vb::Column::Vol, a, 128);
  (void)vb::randomise(10, 100, 7, 2, vb::Column::Vol, b, 128);
  CHECK(std::memcmp(a, b, sizeof a) == 0);
  (void)vb::randomise(10, 100, 8, 2, vb::Column::Vol, c, 128);
  CHECK(std::memcmp(a, c, sizeof a) != 0);
  empty_column(b);
  for (unsigned i = 0; i < 128; ++i) {
    b[i].vol = static_cast<std::uint8_t>(20 + i / 2);
  }
  (void)vb::randomise(10, 100, 7, 3, vb::Column::Vol, b, 128);
  CHECK(std::memcmp(a, b, sizeof a) != 0);  // another track of the block draws other numbers
  // drop every other value before: the steps that keep theirs get the same result
  Step d[kMaxPatternLength];
  empty_column(d);
  for (unsigned i = 0; i < 128; i += 2) {
    d[i].vol = static_cast<std::uint8_t>(20 + i / 2);
  }
  (void)vb::randomise(10, 100, 7, 2, vb::Column::Vol, d, 128);
  for (unsigned i = 0; i < 128; i += 2) {
    CHECK(d[i].vol == a[i].vol);
  }
}

// ---- ROTATE -----------------------------------------------------------------------------------------------------------------

TEST_CASE("rotate: up and down move the steps by one row and wrap inside the range; up then down is the identity") {
  Step c[kMaxPatternLength];
  empty_column(c);
  for (unsigned i = 0; i < 8; ++i) {
    c[i] = noted(61 + i, 10 + i);
    c[i].fx[0] = {18, static_cast<std::uint8_t>(i)};
  }
  c[8] = noted(99);  // past the range: stays
  Step orig[kMaxPatternLength];
  std::memcpy(orig, c, sizeof c);
  vb::Count r = vb::rotate(true, edit::kElMask, c, 8);
  CHECK(r.changed == 8);
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(same(c[i], orig[(i + 1) % 8]));
  }
  CHECK(same(c[8], orig[8]));
  r = vb::rotate(false, edit::kElMask, c, 8);
  CHECK(std::memcmp(c, orig, sizeof c) == 0);
  r = vb::rotate(false, edit::kElMask, c, 8);
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(same(c[i], orig[(i + 7) % 8]));
  }
  // a full turn is the identity
  for (unsigned k = 0; k < 7; ++k) {
    (void)vb::rotate(false, edit::kElMask, c, 8);
  }
  CHECK(std::memcmp(c, orig, sizeof c) == 0);
}

TEST_CASE("rotate: a range of one step, or none, or no elements, changes nothing; identical steps change nothing") {
  Step c[kMaxPatternLength];
  empty_column(c);
  c[0] = noted(61);
  CHECK(vb::rotate(true, edit::kElMask, c, 1).changed == 0);
  CHECK(vb::rotate(true, edit::kElMask, c, 0).changed == 0);
  CHECK(vb::rotate(true, 0, c, 4).changed == 0);
  CHECK(c[0].note == 61);
  empty_column(c);
  CHECK(vb::rotate(true, edit::kElMask, c, 16).changed == 0);
}

TEST_CASE("rotate: only the named fields move (a column block): the volume alone, FX1 alone, note and instrument alone") {
  Step c[kMaxPatternLength];
  empty_column(c);
  for (unsigned i = 0; i < 4; ++i) {
    c[i] = noted(61 + i, 10 + i, 1 + i);
    c[i].fx[0] = {18, static_cast<std::uint8_t>(i)};
    c[i].fx[1] = {31, static_cast<std::uint8_t>(50 + i)};
  }
  Step orig[kMaxPatternLength];
  std::memcpy(orig, c, sizeof c);
  (void)vb::rotate(true, edit::kElVol, c, 4);
  for (unsigned i = 0; i < 4; ++i) {
    CHECK(c[i].vol == orig[(i + 1) % 4].vol);
    CHECK(c[i].note == orig[i].note);
    CHECK(c[i].instr == orig[i].instr);
    CHECK(std::memcmp(c[i].fx, orig[i].fx, sizeof c[i].fx) == 0);
  }
  std::memcpy(c, orig, sizeof c);
  (void)vb::rotate(false, edit::kElFx1, c, 4);
  for (unsigned i = 0; i < 4; ++i) {
    CHECK(c[i].fx[0].value == orig[(i + 3) % 4].fx[0].value);
    CHECK(c[i].fx[1].value == orig[i].fx[1].value);
    CHECK(c[i].vol == orig[i].vol);
  }
  std::memcpy(c, orig, sizeof c);
  (void)vb::rotate(true, edit::kElNote, c, 4);
  for (unsigned i = 0; i < 4; ++i) {
    CHECK(c[i].note == orig[(i + 1) % 4].note);
    CHECK(c[i].instr == orig[(i + 1) % 4].instr);  // note and instrument travel together
    CHECK(c[i].vol == orig[i].vol);
  }
  std::memcpy(c, orig, sizeof c);
  (void)vb::rotate(true, edit::kElFx2, c, 4);
  for (unsigned i = 0; i < 4; ++i) {
    CHECK(c[i].fx[1].value == orig[(i + 1) % 4].fx[1].value);
    CHECK(c[i].fx[0].value == orig[i].fx[0].value);
  }
}

// ---- HUMANISE ---------------------------------------------------------------------------------------------------------------

TEST_CASE("humanise: the same steps as the HUMANIZE app (its pinned values, tests of modules/apps/humanize)") {
  Step c[8];
  for (unsigned j = 0; j < 8; ++j) {
    c[j] = noted(49 + j, 100);
  }
  const vb::Count r = vb::humanise(20, 8, 0, 0, c, 8);
  std::string got;
  for (unsigned j = 0; j < 8; ++j) {
    got += std::to_string(c[j].vol) + "/" + std::to_string(c[j].fx[1].value) + " ";
  }
  CHECK(got == "105/1 98/12 95/8 92/20 102/20 105/11 99/16 100/13 ");  // (the string pinned in humanize_test.cpp, the app's draw)
  CHECK(r.changed == 8);
  CHECK(r.skipped == 0);
  for (unsigned j = 0; j < 8; ++j) {
    CHECK(c[j].fx[1].type == vb::kFxMicroMove);  // the same micro-move FX, FX2 first
    CHECK(c[j].fx[0].type == kFxNone);
  }
}

TEST_CASE("humanise: bounds, only steps with a note, only VOL and the FX slots; an unset volume stays unset") {
  for (unsigned time : {0u, 1u, 30u, 100u}) {
    for (unsigned vol : {0u, 1u, 8u, 64u}) {
      Step cur[kMaxPatternLength];
      Step out[kMaxPatternLength];
      for (unsigned j = 0; j < 128; ++j) {
        cur[j] = noted(j % 3 == 0 ? 0 : 1 + j, j % 5 == 0 ? kVolumeUnset : j % 128, 1 + j % 64);
      }
      std::memcpy(out, cur, sizeof cur);
      (void)vb::humanise(time, vol, 77, 1, out, 128);
      for (unsigned j = 0; j < 128; ++j) {
        CHECK(out[j].note == cur[j].note);
        CHECK(out[j].instr == cur[j].instr);
        if (cur[j].note == 0) {
          CHECK(same(out[j], cur[j]));
          continue;
        }
        if (cur[j].vol == kVolumeUnset || vol == 0) {
          CHECK(out[j].vol == cur[j].vol);
        } else {
          const int d = static_cast<int>(out[j].vol) - static_cast<int>(cur[j].vol);
          CHECK(d <= static_cast<int>(vol) + 0);
          CHECK(-d <= static_cast<int>(vol));
          CHECK(out[j].vol <= 127);
        }
        CHECK(out[j].fx[1].value <= time);
      }
    }
  }
}

TEST_CASE("humanise: the Micro-move slot, an existing one is replaced, both slots taken keeps the timing and is counted; TIME 0 and VOL 0 change nothing") {
  bool went_to_fx1 = false;  // (a step that draws a delay of 0 writes none: over the seeds both happen)
  bool counted = false;
  for (unsigned seed = 0; seed < 40; ++seed) {
    Step c[8];
    for (auto& s : c) {
      s = noted(61, 100);
    }
    c[0].fx[0] = {vb::kFxMicroMove, 99};  // replaced in FX1
    c[1].fx[1] = {vb::kFxMicroMove, 99};  // replaced in FX2
    c[2].fx[1] = {18, 5};                 // FX2 taken: the delay goes to FX1
    c[3].fx[0] = {18, 5};
    c[3].fx[1] = {31, 5};                 // both taken: kept
    const vb::Count r = vb::humanise(100, 0, seed, 0, c, 8);
    CHECK(c[0].fx[0].type == vb::kFxMicroMove);
    CHECK(c[0].fx[0].value <= 100);
    CHECK(c[0].fx[1].type == kFxNone);
    CHECK(c[1].fx[1].type == vb::kFxMicroMove);
    CHECK(c[1].fx[1].value <= 100);
    CHECK((c[2].fx[1].type == 18 && c[2].fx[1].value == 5));  // the FX that was there stays
    CHECK((c[2].fx[0].type == kFxNone || c[2].fx[0].type == vb::kFxMicroMove));
    CHECK((c[3].fx[0].type == 18 && c[3].fx[0].value == 5 && c[3].fx[1].type == 31 && c[3].fx[1].value == 5));
    CHECK(r.skipped <= 1);
    went_to_fx1 = went_to_fx1 || c[2].fx[0].type == vb::kFxMicroMove;
    counted = counted || r.skipped == 1;
  }
  CHECK(went_to_fx1);
  CHECK(counted);
  Step d[8];
  for (auto& s : d) {
    s = noted(61, 100);
  }
  CHECK(vb::humanise(0, 0, 5, 0, d, 8).changed == 0);
  CHECK(d[0].fx[1].type == kFxNone);
}

TEST_CASE("humanise: the same seed gives the same steps; VARY (seed + 1) gives others; the tracks of a block draw apart") {
  Step a[16];
  Step b[16];
  Step c[16];
  Step d[16];
  for (unsigned j = 0; j < 16; ++j) {
    a[j] = b[j] = c[j] = d[j] = noted(61 + j, 90);
  }
  (void)vb::humanise(30, 10, 5, 0, a, 16);
  (void)vb::humanise(30, 10, 5, 0, b, 16);
  (void)vb::humanise(30, 10, 6, 0, c, 16);
  (void)vb::humanise(30, 10, 5, 1, d, 16);
  CHECK(std::memcmp(a, b, sizeof a) == 0);
  CHECK(std::memcmp(a, c, sizeof a) != 0);
  CHECK(std::memcmp(a, d, sizeof a) != 0);
}

// ---- the block: one undo unit, exact undo ----------------------------------------------------------------------------------------

namespace {

model::ModelArena g_arena;
alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

constexpr core::ActionId kAct = core::action_id("pattern.verb");

// 3 tracks of 16 steps, track 2 only 8 long; some notes with a volume on every track.
void make(Editor& ed, Project& p) {
  NEED(ed.create_pattern(1, 16) == Status::Ok);
  NEED(ed.set_track_length(1, 2, 8) == Status::Ok);
  for (unsigned t = 0; t < 3; ++t) {
    for (unsigned i = 0; i < 16; i += 2) {
      Step s = noted(61 + (i % 12), 20 + 5 * i + t + (i * i) / 4);  // not a straight line, so INTERPOLATE has something to change
      s.fx[0] = {18, static_cast<std::uint8_t>(10 + i)};
      NEED(ed.set_step(1, t, i, s) == Status::Ok);
    }
  }
  (void)p;
  ed.history_reset();
}

std::string snapshot(const Project& p) {
  const Pattern* pat = p.pattern(1);
  return pat == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(pat->steps), sizeof pat->steps);
}

}  // namespace

TEST_CASE("verbs block: every verb over several tracks is ONE undo unit, undone and redone exactly") {
  const vb::Plan plans[] = {
      {vb::Verb::Interpolate, vb::Column::Vol, edit::kElMask, 0, 0, 0, 0, 0},
      {vb::Verb::Randomise, vb::Column::Vol, edit::kElMask, 12, 100, 0, 0, 3},
      {vb::Verb::RotateUp, vb::Column::Vol, edit::kElMask, 0, 0, 0, 0, 0},
      {vb::Verb::RotateDown, vb::Column::Vol, edit::kElMask, 0, 0, 0, 0, 0},
      {vb::Verb::Humanise, vb::Column::Vol, edit::kElMask, 0, 0, 25, 9, 4},
  };
  for (const vb::Plan& plan : plans) {
    Project& p = g_arena.project;
    p.reset();
    UndoJournal journal;
    NEED(journal.begin(g_ring, sizeof g_ring));
    Editor ed(p, journal);
    make(ed, p);
    const std::string before = snapshot(p);
    const vb::BlockResult r = vb::apply_block(ed, kAct, 0, 1, vb::Block{0, 3, 1, 12}, plan);
    INFO(static_cast<int>(plan.verb));
    CHECK(r.status == Status::Ok);
    CHECK(r.count.changed > 0);
    CHECK(r.tracks_written == 3);
    const std::string after = snapshot(p);
    CHECK(after != before);
    CHECK(ed.group_depth() == 0);
    CHECK(journal.depth() == 1u);  // one unit, whatever the number of tracks
    CHECK(ed.undo() == UndoResult::Ok);
    CHECK(snapshot(p) == before);
    CHECK(ed.undo() == UndoResult::Nothing);
    CHECK(ed.redo() == UndoResult::Ok);
    CHECK(snapshot(p) == after);
    CHECK(p.check_invariants());
  }
}

TEST_CASE("verbs block: never past a track's own length, rows outside the block untouched, and nothing changed leaves no unit") {
  Project& p = g_arena.project;
  p.reset();
  UndoJournal journal;
  NEED(journal.begin(g_ring, sizeof g_ring));
  Editor ed(p, journal);
  make(ed, p);
  NEED(p.pattern(1) != nullptr);
  const Step track2_tail = p.pattern(1)->steps[2][10];
  const Step track0_head = p.pattern(1)->steps[0][0];
  // block rows 4..15 over tracks 0..2: track 2 is 8 steps long, so it takes rows 4..7 only
  vb::Plan rot;
  rot.verb = vb::Verb::RotateUp;
  vb::BlockResult r = vb::apply_block(ed, kAct, 0, 1, vb::Block{0, 3, 4, 12}, rot);
  CHECK(r.status == Status::Ok);
  CHECK(r.tracks_written == 3);
  CHECK(same(p.pattern(1)->steps[2][10], track2_tail));  // past the track's end: not written (not even rotated into)
  CHECK(same(p.pattern(1)->steps[0][0], track0_head));   // before the block
  CHECK(!same(p.pattern(1)->steps[2][7], kEmptyStep));
  // a block that starts past a track's end: that track is refused, the others still run
  r = vb::apply_block(ed, kAct, 0, 1, vb::Block{1, 2, 10, 4}, rot);
  CHECK(r.refused == 1);
  CHECK(r.tracks_written == 1);
  // nothing to change: a cursor step is a block of one, a one-step rotate does nothing, and no unit is recorded
  const auto depth = journal.depth();
  r = vb::apply_block(ed, kAct, 0, 1, vb::Block{0, 1, 3, 1}, rot);
  CHECK(r.status == Status::Ok);
  CHECK(r.count.changed == 0);
  CHECK(journal.depth() == depth);
}

TEST_CASE("verbs block: a missing pattern and a block outside its range are refused, nothing written") {
  Project& p = g_arena.project;
  p.reset();
  UndoJournal journal;
  NEED(journal.begin(g_ring, sizeof g_ring));
  Editor ed(p, journal);
  make(ed, p);
  vb::Plan plan;
  plan.verb = vb::Verb::RotateUp;
  CHECK(vb::apply_block(ed, kAct, 0, 9, vb::Block{0, 1, 0, 4}, plan).status == Status::NotFound);
  CHECK(vb::apply_block(ed, kAct, 0, 1, vb::Block{16, 1, 0, 4}, plan).status == Status::BadIndex);
  CHECK(vb::apply_block(ed, kAct, 0, 1, vb::Block{15, 2, 0, 4}, plan).status == Status::BadIndex);
  CHECK(vb::apply_block(ed, kAct, 0, 1, vb::Block{0, 1, 120, 9}, plan).status == Status::BadIndex);
  CHECK(vb::apply_block(ed, kAct, 0, 1, vb::Block{0, 0, 0, 4}, plan).status == Status::BadIndex);
  CHECK(journal.depth() == 0u);
  CHECK(ed.group_depth() == 0);
}
