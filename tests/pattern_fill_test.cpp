// SPDX-License-Identifier: GPL-3.0-or-later
// OS-PTOOLS part 1: the Fill generators (model/pattern_fill.h): the Euclid distribution, every-N, density, the scales, no duplicates, what is written and what is left alone.
#include <doctest/doctest.h>

#include <o3os/core/scales.h>
#include <o3os/model/fx_table.h>
#include <o3os/model/pattern_fill.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::model;

namespace {

std::string euclid(unsigned events, unsigned len) {
  std::string s;
  for (unsigned i = 0; i < len; ++i) {
    s += fill::euclid_hit(events, len, i) ? 'x' : '.';
  }
  return s;
}

// The notes (model bytes) of `steps` that are not empty, in order.
std::string notes(const Step* steps, unsigned len) {
  std::string s;
  for (unsigned i = 0; i < len; ++i) {
    if (steps[i].note != kNoteEmpty) {
      s += std::to_string(steps[i].note - 1) + " ";
    }
  }
  return s;
}

}  // namespace

TEST_CASE("fill: Euclid distribution table (Bresenham-even; a rotation of Bjorklund's necklace, first step a hit)") {
  CHECK(euclid(0, 8) == "........");     // 0 events: none
  CHECK(euclid(1, 8) == "x.......");
  CHECK(euclid(2, 8) == "x...x...");
  CHECK(euclid(3, 8) == "x..x..x.");     // the tresillo
  CHECK(euclid(4, 8) == "x.x.x.x.");
  CHECK(euclid(5, 8) == "x.x.xx.x");
  CHECK(euclid(7, 8) == "x.xxxxxx");     // seven hits, the one gap after the first
  CHECK(euclid(8, 8) == "xxxxxxxx");     // events == steps: every step
  CHECK(euclid(9, 8) == "xxxxxxxx");     // more events than steps: clamped
  CHECK(euclid(32, 8) == "xxxxxxxx");
  CHECK(euclid(5, 16) == "x...x..x..x..x..");
  CHECK(euclid(3, 4) == "x.xx");
  CHECK(euclid(4, 4) == "xxxx");
  CHECK(euclid(1, 1) == "x");
  CHECK(euclid(3, 0).empty());
  CHECK_FALSE(fill::euclid_hit(3, 8, 8));  // past the range
}

TEST_CASE("fill: Euclid properties over every length and event count: exactly E hits, gaps differ by at most one, the first step hits") {
  for (unsigned len = 1; len <= 128; ++len) {
    for (unsigned events = 1; events <= 32; ++events) {
      const unsigned e = events > len ? len : events;
      unsigned hits = 0;
      int last = -1;
      int first = -1;
      int gap_min = 1000;
      int gap_max = 0;
      for (unsigned i = 0; i < len; ++i) {
        if (!fill::euclid_hit(events, len, i)) {
          continue;
        }
        ++hits;
        if (last >= 0) {
          const int g = static_cast<int>(i) - last;
          gap_min = g < gap_min ? g : gap_min;
          gap_max = g > gap_max ? g : gap_max;
        }
        first = first < 0 ? static_cast<int>(i) : first;
        last = static_cast<int>(i);
      }
      CHECK_MESSAGE(hits == e, "len " << len << " events " << events);
      CHECK(first == 0);
      if (hits > 1) {
        CHECK_MESSAGE(gap_max - gap_min <= 1, "len " << len << " events " << events);
        // the wrap-around gap (last hit to the next pass's first) is in the same band
        const int wrap = static_cast<int>(len) - last;
        CHECK_MESSAGE((wrap >= gap_min - 1 && wrap <= gap_max + 1), "len " << len << " events " << events);
      }
    }
  }
}

TEST_CASE("fill: every N chooses offset % N == 0 from the first step of the range; N is 1..32, 0 counts as 1") {
  fill::Rng rng;
  bool hit[128];
  fill::Plan plan;
  plan.type = fill::Type::Every;
  for (unsigned n = 0; n <= 40; ++n) {
    plan.n = static_cast<std::uint8_t>(n);
    const unsigned every = n < 1 ? 1 : (n > 32 ? 32 : n);
    const unsigned count = fill::select(plan, 64, rng, hit);
    CHECK(count == (64 + every - 1) / every);  // ceil(len / n): PAR-073
    for (unsigned i = 0; i < 64; ++i) {
      CHECK(hit[i] == (i % every == 0));
    }
  }
}

TEST_CASE("fill: density bounds (0 none, 100 all, 30 about three in ten) and the same seed gives the same fill") {
  bool hit[128];
  fill::Plan plan;
  plan.type = fill::Type::Random;
  fill::Rng rng;
  plan.density = 0;
  CHECK(fill::select(plan, 128, rng, hit) == 0u);
  plan.density = 100;
  CHECK(fill::select(plan, 128, rng, hit) == 128u);
  plan.density = 200;  // clamped to 100
  CHECK(fill::select(plan, 128, rng, hit) == 128u);
  plan.density = fill::kDefaultDensity;
  unsigned total = 0;
  unsigned draws = 0;
  for (int pass = 0; pass < 100; ++pass) {
    total += fill::select(plan, 100, rng, hit);
    draws += 100;
  }
  CHECK(draws == 10000u);
  CHECK(total > 2800u);  // 30 % +- 2 points over 10,000 draws
  CHECK(total < 3200u);
  fill::Rng a;
  fill::Rng b;
  bool ha[128];
  bool hb[128];
  (void)fill::select(plan, 128, a, ha);
  (void)fill::select(plan, 128, b, hb);
  CHECK(std::memcmp(ha, hb, sizeof ha) == 0);
  (void)fill::select(plan, 128, a, ha);  // the second pass of one generator differs
  CHECK(std::memcmp(ha, hb, sizeof ha) != 0);
}

TEST_CASE("fill: the four corrected scales (PAR-073) have the standard intervals") {
  const auto semis = [](unsigned scale) {
    std::string out;
    for (unsigned k = 0; k < 12; ++k) {
      if (core::is_in_scale(static_cast<std::uint8_t>(k), 0, static_cast<std::uint8_t>(scale))) {
        out += std::to_string(k) + " ";
      }
    }
    return out;
  };
  CHECK(semis(11) == "0 2 3 5 7 9 11 ");  // melodic minor
  CHECK(semis(17) == "0 2 3 4 7 9 ");      // blues major
  CHECK(semis(23) == "0 1 3 6 7 8 11 ");   // Todi
  CHECK(semis(36) == "0 2 3 6 7 8 11 ");   // Gypsy minor
  // ... and they follow the root: Todi on D has D Eb F G# A Bb C# (2 3 5 8 9 10 1).
  for (unsigned k = 0; k < 12; ++k) {
    const unsigned rel = (k + 12 - 2) % 12;
    CHECK(core::is_in_scale(static_cast<std::uint8_t>(k), 2, 23) == (rel == 0 || rel == 1 || rel == 3 || rel == 6 || rel == 7 || rel == 8 || rel == 11));
  }
}

TEST_CASE("fill: scale_up finds the next in-scale note and never goes past 127") {
  CHECK(fill::scale_up(60, 0, 0) == 60u);   // no scale: the note itself
  CHECK(fill::scale_up(61, 0, 3) == 62u);   // C major: C# -> D
  CHECK(fill::scale_up(60, 0, 3) == 60u);
  CHECK(fill::scale_up(125, 0, 3) == 125u);  // 125 is F: in C major
  CHECK(fill::scale_up(126, 0, 3) == 127u);  // F# -> G (127)
  CHECK(fill::scale_up(127, 1, 3) == 128u);  // 127 (G) is not in C# major, and there is nothing above
  CHECK(fill::scale_up(200, 0, 0) == 128u);
}

namespace {

constexpr unsigned kLen = 64;

// The plan with FROM and TO as the dialog opens them from the current value (OS-FILL2 Q2): what the fills of OS-PTOOLS wrote with `Value` alone.
fill::Plan started(fill::Plan plan, const fill::Value& value) {
  fill::range_default(plan, value);
  return plan;
}

struct Rig {
  Step steps[kLen];
  fill::Rng rng;
  edit::Last last{3, 0};
  Rig() {
    for (Step& s : steps) {
      s = kEmptyStep;
    }
  }
};

}  // namespace

TEST_CASE("fill: EVERY writes the current note on every Nth step, an empty step takes the last instrument, one with an instrument keeps it, the others are untouched") {
  Rig r;
  r.steps[4] = Step{70, 9, kVolumeUnset, {{0, 0}, {0, 0}}};  // a step that already has a note and instrument
  r.steps[5] = Step{80, 2, 90, {{8, 5}, {0, 0}}};            // not chosen: byte-identical afterwards
  const Step before5 = r.steps[5];
  fill::Plan plan;
  plan.type = fill::Type::Every;
  plan.n = 4;
  fill::Value value;
  value.note = 61;
  CHECK(fill::apply(started(plan, value), r.steps, kLen, r.rng, r.last) == 16u);
  for (unsigned i = 0; i < kLen; ++i) {
    CHECK_MESSAGE((r.steps[i].note == 61) == (i % 4 == 0), "step " << i);
  }
  CHECK(r.steps[0].instr == 3);  // the last used instrument
  CHECK(r.steps[4].instr == 9);  // its own
  CHECK(std::memcmp(&r.steps[5], &before5, sizeof before5) == 0);
}

TEST_CASE("fill: EUCLID writes the hits only; E larger than the range fills it; E = 0 writes nothing") {
  Rig r;
  fill::Plan plan;
  plan.type = fill::Type::Euclid;
  plan.events = 3;
  CHECK(fill::apply(started(plan, fill::Value{}), r.steps, 8, r.rng, r.last) == 3u);
  std::string s;
  for (unsigned i = 0; i < 8; ++i) {
    s += r.steps[i].note != kNoteEmpty ? 'x' : '.';
  }
  CHECK(s == "x..x..x.");
  CHECK(r.steps[8].note == kNoteEmpty);  // past the range
  plan.events = 32;
  CHECK(fill::apply(started(plan, fill::Value{}), r.steps, 8, r.rng, r.last) == 8u);
  plan.events = 0;
  Rig q;
  CHECK(fill::apply(started(plan, fill::Value{}), q.steps, 8, q.rng, q.last) == 0u);
  CHECK(notes(q.steps, 8).empty());
}

TEST_CASE("fill: SCALE runs up one octave of the scale from the current note, repeating, with no note twice in a row") {
  Rig r;
  fill::Plan plan;
  plan.type = fill::Type::Scale;
  plan.n = 1;
  plan.root = 0;
  plan.scale = 3;  // C major
  fill::Value value;
  value.note = 61;  // C-5 as a step byte: OG note 60
  CHECK(fill::apply(started(plan, value), r.steps, 16, r.rng, r.last) == 16u);
  CHECK(notes(r.steps, 8) == "60 62 64 65 67 69 71 60 ");  // wraps after the seventh; no eighth note above
  for (unsigned i = 1; i < 16; ++i) {
    CHECK(r.steps[i].note != r.steps[i - 1].note);
  }
  // Every 2nd step, from a note outside the scale: snapped up to the first in-scale note.
  Rig q;
  plan.n = 2;
  value.note = 62;  // OG 61, C#
  (void)fill::apply(started(plan, value), q.steps, 8, q.rng, q.last);
  CHECK(notes(q.steps, 8) == "62 64 65 67 ");
}

TEST_CASE("fill: RANDOM notes stay inside the scale and the octave, never repeat a neighbour, and are the same for the same seed") {
  fill::Plan plan;
  plan.type = fill::Type::Random;
  plan.density = 100;
  plan.root = 9;   // A
  plan.scale = 18; // pentatonic minor
  fill::Value value;
  value.note = 61;
  Rig a;
  Rig b;
  CHECK(fill::apply(started(plan, value), a.steps, kLen, a.rng, a.last) == kLen);
  (void)fill::apply(started(plan, value), b.steps, kLen, b.rng, b.last);
  CHECK(std::memcmp(a.steps, b.steps, sizeof a.steps) == 0);
  for (unsigned i = 0; i < kLen; ++i) {
    const unsigned n = a.steps[i].note - 1u;
    CHECK_MESSAGE((n >= 60u && n < 72u), "step " << i);
    CHECK_MESSAGE(core::is_in_scale(static_cast<std::uint8_t>(n), 9, 18), "step " << i);
    if (i > 0) {
      CHECK_MESSAGE(a.steps[i].note != a.steps[i - 1].note, "step " << i);
    }
  }
  // A one-note window (No scale is twelve notes; a root-only top of the range is one): still writes, repeats allowed only when there is no other choice.
  Rig c;
  value.note = 128;  // OG 127 = G; scale C major leaves only 127 in the window
  plan.root = 0;
  plan.scale = 3;
  CHECK(fill::apply(started(plan, value), c.steps, 8, c.rng, c.last) == 8u);
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(c.steps[i].note == 128);
  }
}

TEST_CASE("fill: no in-scale note up to 127 leaves the step as it was; nothing is ever written outside 0..127 (PAR-150)") {
  Rig r;
  r.steps[0] = Step{50, 4, kVolumeUnset, {{0, 0}, {0, 0}}};
  const Step before = r.steps[0];
  fill::Plan plan;
  plan.type = fill::Type::Every;
  plan.root = 1;  // C# major: 127 (G) is out, and so is everything between 127 and 127
  plan.scale = 3;
  fill::Value value;
  value.note = 128;
  CHECK(fill::apply(started(plan, value), r.steps, 4, r.rng, r.last) == 0u);
  CHECK(std::memcmp(&r.steps[0], &before, sizeof before) == 0);
  // A sweep: every base note, every scale, both writing types: bytes stay 1..128 and a step never loses what it had.
  for (unsigned scale = 0; scale < 40; ++scale) {
    for (unsigned base = 1; base <= 128; base += 3) {
      for (const fill::Type type : {fill::Type::Every, fill::Type::Random, fill::Type::Scale}) {
        Rig q;
        for (Step& s : q.steps) {
          s.note = 20;
          s.instr = 5;
        }
        fill::Plan p;
        p.type = type;
        p.density = 100;
        p.scale = static_cast<std::uint8_t>(scale);
        p.root = static_cast<std::uint8_t>(base % 12);
        fill::Value v;
        v.note = static_cast<std::uint8_t>(base);
        (void)fill::apply(started(p, v), q.steps, 16, q.rng, q.last);
        for (unsigned i = 0; i < 16; ++i) {
          CHECK(q.steps[i].note >= 1);
          CHECK(q.steps[i].note <= 128);
          CHECK(q.steps[i].instr == 5);
        }
      }
    }
  }
}

TEST_CASE("fill: INSTR and VOLUME write the chosen steps only; an instrument on an empty step gives it the default note") {
  Rig r;
  fill::Plan plan;
  plan.what = fill::What::Instr;
  plan.type = fill::Type::Every;
  plan.n = 8;
  fill::Value value;
  value.instr = 12;
  CHECK(fill::apply(started(plan, value), r.steps, kLen, r.rng, r.last) == 8u);
  for (unsigned i = 0; i < kLen; ++i) {
    CHECK((r.steps[i].instr == 12) == (i % 8 == 0));
    CHECK((r.steps[i].note == edit::kDefaultNote + 1) == (i % 8 == 0));
  }
  Rig q;
  plan.what = fill::What::Vol;
  plan.n = 16;
  value.vol = 64;
  CHECK(fill::apply(started(plan, value), q.steps, kLen, q.rng, q.last) == 4u);
  for (unsigned i = 0; i < kLen; ++i) {
    CHECK(q.steps[i].vol == (i % 16 == 0 ? 64 : kVolumeUnset));
    CHECK(q.steps[i].note == kNoteEmpty);
  }
}

// ---- OS-FILL2: value modes, FROM and TO, FX -------------------------------------------------------------------------------------------------

namespace {

// The value (VOLUME) of every step of a rig, -1 for an unset one.
std::vector<int> vols(const Step* steps, unsigned len) {
  std::vector<int> v;
  for (unsigned i = 0; i < len; ++i) {
    v.push_back(steps[i].vol == kVolumeUnset ? -1 : steps[i].vol);
  }
  return v;
}

fill::Plan vol_plan(fill::Mode mode, unsigned from, unsigned to, unsigned n = 1) {
  fill::Plan p;
  p.what = fill::What::Vol;
  p.type = fill::Type::Every;
  p.n = static_cast<std::uint8_t>(n);
  p.mode = mode;
  p.from = static_cast<std::uint8_t>(from);
  p.to = static_cast<std::uint8_t>(to);
  return p;
}

}  // namespace

TEST_CASE("fill FILL2: CONST writes FROM on every hit whatever TO says") {
  Rig r;
  CHECK(fill::apply(vol_plan(fill::Mode::Const, 90, 10, 16), r.steps, kLen, r.rng, r.last) == 4u);
  for (unsigned i = 0; i < kLen; ++i) {
    CHECK(r.steps[i].vol == (i % 16 == 0 ? 90 : kVolumeUnset));
  }
}

TEST_CASE("fill FILL2: RAMP up and down has exact endpoints and integer rounding, per track") {
  Rig up;
  CHECK(fill::apply(vol_plan(fill::Mode::Ramp, 0, 100), up.steps, 5, up.rng, up.last) == 5u);
  CHECK(vols(up.steps, 5) == std::vector<int>{0, 25, 50, 75, 100});
  Rig round;  // (10k + 1) / 3: the rounding is "+ (K-1)/2", then an integer division
  (void)fill::apply(vol_plan(fill::Mode::Ramp, 0, 10), round.steps, 4, round.rng, round.last);
  CHECK(vols(round.steps, 4) == std::vector<int>{0, 3, 7, 10});
  Rig down;  // FROM above TO: a fade-out
  (void)fill::apply(vol_plan(fill::Mode::Ramp, 127, 0), down.steps, 8, down.rng, down.last);
  CHECK(vols(down.steps, 8) == std::vector<int>{127, 109, 91, 73, 54, 36, 18, 0});
  // The steps that are not hits keep what they had; the ramp counts the hits only (every 2nd of 9: 5 hits).
  Rig sparse;
  (void)fill::apply(vol_plan(fill::Mode::Ramp, 20, 100, 2), sparse.steps, 9, sparse.rng, sparse.last);
  CHECK(vols(sparse.steps, 9) == std::vector<int>{20, -1, 40, -1, 60, -1, 80, -1, 100});
}

TEST_CASE("fill FILL2: RAMP over every FROM, TO and hit count: first FROM, last TO, monotone, inside the bounds") {
  for (unsigned hits = 1; hits <= 64; hits += (hits < 8 ? 1 : 9)) {
    for (unsigned from = 0; from <= 127; from += 13) {
      for (unsigned to = 0; to <= 127; to += 17) {
        Rig r;
        CHECK(fill::apply(vol_plan(fill::Mode::Ramp, from, to), r.steps, hits, r.rng, r.last) == hits);
        CHECK_MESSAGE(r.steps[0].vol == from, "K " << hits << " " << from << "->" << to);
        if (hits > 1) {
          CHECK_MESSAGE(r.steps[hits - 1].vol == to, "K " << hits << " " << from << "->" << to);
        }
        for (unsigned i = 1; i < hits; ++i) {
          const int d = static_cast<int>(r.steps[i].vol) - static_cast<int>(r.steps[i - 1].vol);
          CHECK_MESSAGE((to >= from ? d >= 0 : d <= 0), "K " << hits << " " << from << "->" << to << " step " << i);
        }
      }
    }
  }
}

TEST_CASE("fill FILL2: one hit writes FROM in every mode") {
  for (const fill::Mode mode : {fill::Mode::Const, fill::Mode::Ramp}) {
    Rig r;
    CHECK(fill::apply(vol_plan(mode, 33, 99, 32), r.steps, 1, r.rng, r.last) == 1u);
    CHECK(r.steps[0].vol == 33);
  }
  Rig e;  // EUCLID with one event over any range: one hit, the first step
  fill::Plan p = vol_plan(fill::Mode::Ramp, 7, 90);
  p.type = fill::Type::Euclid;
  p.events = 1;
  CHECK(fill::apply(p, e.steps, kLen, e.rng, e.last) == 1u);
  CHECK(e.steps[0].vol == 7);
}

TEST_CASE("fill FILL2: RANDOM stays inside FROM..TO (either order), reaches both ends, and is the same for the same seed") {
  for (const bool reversed : {false, true}) {
    Rig a;
    Rig b;
    a.rng.s = b.rng.s = 0xBEEF1234u;
    const fill::Plan p = vol_plan(fill::Mode::Random, reversed ? 13 : 10, reversed ? 10 : 13);
    CHECK(fill::apply(p, a.steps, kLen, a.rng, a.last) == kLen);
    (void)fill::apply(p, b.steps, kLen, b.rng, b.last);
    CHECK(std::memcmp(a.steps, b.steps, sizeof a.steps) == 0);
    bool seen[4] = {false, false, false, false};
    for (unsigned i = 0; i < kLen; ++i) {
      CHECK(a.steps[i].vol >= 10);
      CHECK(a.steps[i].vol <= 13);
      if (a.steps[i].vol >= 10 && a.steps[i].vol <= 13) {
        seen[a.steps[i].vol - 10] = true;
      }
    }
    CHECK((seen[0] && seen[1] && seen[2] && seen[3]));  // 64 draws of four values
  }
  Rig c;  // FROM = TO is CONST, and a draw costs the generator nothing then
  CHECK(fill::apply(vol_plan(fill::Mode::Random, 50, 50), c.steps, 8, c.rng, c.last) == 8u);
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(c.steps[i].vol == 50);
  }
  Rig d;  // the full range of a byte-sized domain
  d.rng.s = 99;
  (void)fill::apply(vol_plan(fill::Mode::Random, 0, 127), d.steps, kLen, d.rng, d.last);
  for (unsigned i = 0; i < kLen; ++i) {
    CHECK(d.steps[i].vol <= 127);
  }
}

TEST_CASE("fill FILL2: INSTR ramps and draws inside 1..64 and writes the instrument") {
  fill::Plan p;
  p.what = fill::What::Instr;
  p.type = fill::Type::Every;
  p.mode = fill::Mode::Ramp;
  p.from = 1;
  p.to = 64;
  Rig r;
  CHECK(fill::apply(p, r.steps, 4, r.rng, r.last) == 4u);
  CHECK((r.steps[0].instr == 1 && r.steps[1].instr == 22 && r.steps[2].instr == 43 && r.steps[3].instr == 64));
  p.mode = fill::Mode::Random;
  p.from = 5;
  p.to = 8;
  Rig q;
  (void)fill::apply(p, q.steps, kLen, q.rng, q.last);
  for (unsigned i = 0; i < kLen; ++i) {
    CHECK((q.steps[i].instr >= 5 && q.steps[i].instr <= 8));
    CHECK(q.steps[i].note == edit::kDefaultNote + 1);  // an empty step takes the default note (as an instrument entry does)
  }
}

TEST_CASE("fill FILL2: a NOTE ramp is moved up into the scale; a note that cannot be placed at 127 leaves its step as it was (PAR-150)") {
  fill::Plan p;
  p.type = fill::Type::Every;
  p.mode = fill::Mode::Ramp;
  p.root = 0;
  p.scale = 3;  // C major
  p.from = 60;
  p.to = 72;
  Rig r;
  CHECK(fill::apply(p, r.steps, 13, r.rng, r.last) == 13u);
  for (unsigned i = 0; i < 13; ++i) {
    const unsigned n = r.steps[i].note - 1u;
    CHECK_MESSAGE(core::is_in_scale(static_cast<std::uint8_t>(n), 0, 3), "step " << i);
    CHECK_MESSAGE((n >= 60u + i && n <= 72u), "step " << i);  // up, never down
  }
  CHECK(r.steps[0].note == 61);
  CHECK(r.steps[12].note == 73);
  // Descending.
  p.from = 72;
  p.to = 60;
  Rig d;
  (void)fill::apply(p, d.steps, 13, d.rng, d.last);
  CHECK(d.steps[0].note == 73);
  CHECK(d.steps[12].note == 61);
  // Past the top: C# major has no note above 126 (127 is G).
  p.root = 1;
  p.from = 120;
  p.to = 127;
  Rig t;
  t.steps[7] = Step{50, 4, kVolumeUnset, {{0, 0}, {0, 0}}};
  const Step before = t.steps[7];
  const unsigned written = fill::apply(p, t.steps, 8, t.rng, t.last);
  CHECK(written == 7u);
  CHECK(std::memcmp(&t.steps[7], &before, sizeof before) == 0);
  for (unsigned i = 0; i < 7; ++i) {
    CHECK(t.steps[i].note >= 121);
    CHECK(t.steps[i].note <= 127);
  }
}

TEST_CASE("fill FILL2: NOTE with MODE RANDOM draws from the in-scale notes of FROM..TO and never repeats a neighbour") {
  fill::Plan p;
  p.type = fill::Type::Every;
  p.mode = fill::Mode::Random;
  p.root = 9;
  p.scale = 18;  // A pentatonic minor
  p.from = 72;
  p.to = 48;     // either order
  Rig a;
  Rig b;
  a.rng.s = b.rng.s = 2024u;
  CHECK(fill::apply(p, a.steps, kLen, a.rng, a.last) == kLen);
  (void)fill::apply(p, b.steps, kLen, b.rng, b.last);
  CHECK(std::memcmp(a.steps, b.steps, sizeof a.steps) == 0);
  unsigned lo = 200;
  unsigned hi = 0;
  for (unsigned i = 0; i < kLen; ++i) {
    const unsigned n = a.steps[i].note - 1u;
    CHECK_MESSAGE((n >= 48u && n <= 72u), "step " << i);
    CHECK_MESSAGE(core::is_in_scale(static_cast<std::uint8_t>(n), 9, 18), "step " << i);
    if (i > 0) {
      CHECK_MESSAGE(a.steps[i].note != a.steps[i - 1].note, "step " << i);
    }
    lo = n < lo ? n : lo;
    hi = n > hi ? n : hi;
  }
  CHECK(hi - lo > 12);  // two octaves of window: the draw is not stuck in one
}

TEST_CASE("fill FILL2: TYPE RANDOM and SCALE take FROM..TO as their window, larger than an octave, whatever MODE says") {
  fill::Plan p;
  p.type = fill::Type::Random;
  p.density = 100;
  p.scale = 0;
  p.mode = fill::Mode::Ramp;  // plays no part
  p.from = 40;
  p.to = 100;
  Rig r;
  r.rng.s = 31337;
  CHECK(fill::apply(p, r.steps, kLen, r.rng, r.last) == kLen);
  unsigned lo = 200;
  unsigned hi = 0;
  for (unsigned i = 0; i < kLen; ++i) {
    const unsigned n = r.steps[i].note - 1u;
    CHECK((n >= 40u && n <= 100u));
    lo = n < lo ? n : lo;
    hi = n > hi ? n : hi;
  }
  CHECK(hi - lo > 24);
  // SCALE walks up the whole window and wraps: 13 notes with no scale.
  fill::Plan s;
  s.type = fill::Type::Scale;
  s.n = 1;
  s.from = 60;
  s.to = 72;
  Rig q;
  CHECK(fill::apply(s, q.steps, 15, q.rng, q.last) == 15u);
  CHECK(notes(q.steps, 15) == "60 61 62 63 64 65 66 67 68 69 70 71 72 60 61 ");
  // The window is cut at 127.
  s.from = 125;
  s.to = 127;
  Rig w;
  (void)fill::apply(s, w.steps, 6, w.rng, w.last);
  CHECK(notes(w.steps, 6) == "125 126 127 125 126 127 ");
}

TEST_CASE("fill FILL2: legacy equivalence, FROM = the cursor's note and TO = FROM + 11 give the OS-PTOOLS fills for the same seed") {
  // The outputs below were taken from the generators of OS-PTOOLS (dev before OS-FILL2) for the same plans and seeds.
  const auto run = [](fill::Plan plan, unsigned note, unsigned len, std::uint32_t seed) {
    Rig r;
    r.rng.s = seed;
    fill::Value v;
    v.note = static_cast<std::uint8_t>(note);
    fill::range_default(plan, v);
    CHECK(plan.from == note - 1u);
    CHECK(plan.to == (note + 10u > 127u ? 127u : note + 10u));
    (void)fill::apply(plan, r.steps, len, r.rng, r.last);
    std::string s;
    for (unsigned i = 0; i < len; ++i) {
      s += (r.steps[i].note == kNoteEmpty ? std::string("-1") : std::to_string(r.steps[i].note - 1)) + " ";
    }
    return s;
  };
  fill::Plan a;
  a.type = fill::Type::Random;
  a.density = 100;
  a.scale = 3;
  CHECK(run(a, 61, 16, 0x1234567u) == "69 60 64 69 62 69 64 60 69 64 65 62 64 67 62 65 ");
  a.root = 9;
  a.scale = 18;
  a.density = 60;
  CHECK(run(a, 66, 16, 0xCAFEF00Du) == "69 -1 -1 72 74 67 -1 69 74 76 -1 -1 69 -1 72 67 ");
  fill::Plan b;
  b.type = fill::Type::Scale;
  b.n = 2;
  b.root = 2;
  b.scale = 8;
  CHECK(run(b, 63, 16, 1u) == "62 -1 63 -1 65 -1 67 -1 69 -1 70 -1 72 -1 62 -1 ");
  fill::Plan c;
  c.type = fill::Type::Random;
  c.density = 100;
  c.scale = 0;
  CHECK(run(c, 126, 8, 77u) == "127 125 127 126 127 126 127 125 ");  // the window is cut at 127
}

TEST_CASE("fill FILL2: range_default gives each WHAT its starting FROM and TO from the current value") {
  fill::Value v;
  v.note = 62;
  v.instr = 9;
  v.vol = 40;
  v.fx_type[0] = 8;
  fill::Plan p;
  p.what = fill::What::Note;
  fill::range_default(p, v);
  CHECK((p.from == 61 && p.to == 72));
  v.note = 128;  // OG 127: the window stops at 127
  fill::range_default(p, v);
  CHECK((p.from == 127 && p.to == 127));
  v.note = 0;  // not a note: C-5
  fill::range_default(p, v);
  CHECK((p.from == 60 && p.to == 71));
  p.what = fill::What::Instr;
  fill::range_default(p, v);
  CHECK((p.from == 9 && p.to == 9));
  p.what = fill::What::Vol;
  fill::range_default(p, v);
  CHECK((p.from == 40 && p.to == 0));
  v.vol = kVolumeUnset;
  fill::range_default(p, v);
  CHECK(p.from == 127);
  p.what = fill::What::Fx1;  // the type of the cursor step in that column, with its default value for both ends
  fill::range_default(p, v);
  CHECK((p.fx_type == 8 && p.from == 0 && p.to == 0));
  p.what = fill::What::Fx2;  // no type there (None): the first picker type, Off
  fill::range_default(p, v);
  CHECK(p.fx_type == 1);
  v.fx_type[1] = 17;  // Swing: default 50
  fill::range_default(p, v);
  CHECK((p.fx_type == 17 && p.from == 50 && p.to == 50));
  v.fx_type[1] = 100;  // a type the table does not know
  fill::range_default(p, v);
  CHECK(p.fx_type == 1);
  const fill::Range d = fill::domain(p);
  CHECK((d.lo == 0 && d.hi == 0));  // Off has one value
}

TEST_CASE("fill FILL2: an FX fill writes type and value on every hit, replaces another type, clamps, and leaves the rest of the step") {
  fill::Plan p;
  p.what = fill::What::Fx1;
  p.type = fill::Type::Every;
  p.n = 2;
  p.fx_type = 3;  // Roll: 0..53
  p.mode = fill::Mode::Ramp;
  p.from = 0;
  p.to = 53;
  Rig r;
  r.steps[0] = Step{70, 9, 90, {{8, 5}, {4, 77}}};  // another type in FX 1 (MIDI CC A), FX 2 busy
  r.steps[2] = Step{kNoteEmpty, 0, kVolumeUnset, {{3, 20}, {0, 0}}};  // the same type already: its value is replaced
  r.steps[1] = Step{80, 2, 90, {{8, 5}, {0, 0}}};
  const Step untouched = r.steps[1];
  CHECK(fill::apply(p, r.steps, 5, r.rng, r.last) == 3u);
  CHECK((r.steps[0].fx[0].type == 3 && r.steps[0].fx[0].value == 0));
  CHECK((r.steps[2].fx[0].type == 3 && r.steps[2].fx[0].value == 27));  // (53 + 1) / 2
  CHECK((r.steps[4].fx[0].type == 3 && r.steps[4].fx[0].value == 53));
  CHECK((r.steps[0].note == 70 && r.steps[0].instr == 9 && r.steps[0].vol == 90));  // only the column
  CHECK((r.steps[0].fx[1].type == 4 && r.steps[0].fx[1].value == 77));
  CHECK((r.steps[2].note == kNoteEmpty && r.steps[2].instr == 0 && r.steps[2].vol == kVolumeUnset));  // an empty step stays without a note
  CHECK(std::memcmp(&r.steps[1], &untouched, sizeof untouched) == 0);
  // FX 2 writes the second column; a FROM above the table's maximum is clamped.
  fill::Plan q = p;
  q.what = fill::What::Fx2;
  q.mode = fill::Mode::Const;
  q.from = 200;
  Rig c;
  c.steps[0] = Step{70, 9, 90, {{8, 5}, {0, 0}}};
  CHECK(fill::apply(q, c.steps, 2, c.rng, c.last) == 1u);
  CHECK((c.steps[0].fx[1].type == 3 && c.steps[0].fx[1].value == 53));
  CHECK((c.steps[0].fx[0].type == 8 && c.steps[0].fx[0].value == 5));
}

TEST_CASE("fill FILL2: single-value FX types take their one value, RANDOM draws inside the type's range, a bad type writes nothing") {
  fill::Plan p;
  p.what = fill::What::Fx1;
  p.type = fill::Type::Every;
  p.mode = fill::Mode::Random;
  p.fx_type = 1;  // Off: 0..0
  p.from = 40;
  p.to = 90;
  Rig r;
  CHECK(fill::apply(p, r.steps, 4, r.rng, r.last) == 4u);
  CHECK((r.steps[3].fx[0].type == 1 && r.steps[3].fx[0].value == 0));
  p.fx_type = 13;  // Break pattern: 1..1
  p.mode = fill::Mode::Const;
  p.from = 0;
  Rig b;
  (void)fill::apply(p, b.steps, 2, b.rng, b.last);
  CHECK((b.steps[0].fx[0].type == 13 && b.steps[0].fx[0].value == 1));
  p.fx_type = 17;  // Swing 25..75
  p.mode = fill::Mode::Random;
  p.from = 0;      // outside the type: pulled into 25..75
  p.to = 255;
  Rig s;
  s.rng.s = 5;
  (void)fill::apply(p, s.steps, kLen, s.rng, s.last);
  for (unsigned i = 0; i < kLen; ++i) {
    CHECK((s.steps[i].fx[0].value >= 25 && s.steps[i].fx[0].value <= 75));
  }
  for (const unsigned bad : {0u, 43u, 200u}) {  // None would clear the column; 43 and 200 are not executable
    p.fx_type = static_cast<std::uint8_t>(bad);
    Rig n;
    n.steps[0] = Step{70, 9, 90, {{8, 5}, {0, 0}}};
    const Step before = n.steps[0];
    CHECK(fill::apply(p, n.steps, 4, n.rng, n.last) == 0u);
    CHECK(std::memcmp(&n.steps[0], &before, sizeof before) == 0);
  }
}

TEST_CASE("fill FILL2: every picker type, every mode: the written value is inside the type's table range") {
  for (unsigned pick = 1; pick < fx::kPickerCount; ++pick) {
    const std::uint8_t type = fx::kPickerOrder[pick];
    for (const fill::Mode mode : {fill::Mode::Const, fill::Mode::Ramp, fill::Mode::Random}) {
      fill::Plan p;
      p.what = pick % 2 != 0 ? fill::What::Fx1 : fill::What::Fx2;
      p.type = fill::Type::Every;
      p.mode = mode;
      p.fx_type = type;
      p.from = 0;
      p.to = 255;
      Rig r;
      r.rng.s = 100 + pick;
      CHECK(fill::apply(p, r.steps, 16, r.rng, r.last) == 16u);
      const unsigned col = pick % 2 != 0 ? 0u : 1u;
      for (unsigned i = 0; i < 16; ++i) {
        CHECK_MESSAGE(r.steps[i].fx[col].type == type, "type " << unsigned(type));
        CHECK_MESSAGE(r.steps[i].fx[col].value >= fx::row(type).lo, "type " << unsigned(type));
        CHECK_MESSAGE(r.steps[i].fx[col].value <= fx::row(type).hi, "type " << unsigned(type));
      }
    }
  }
}

TEST_CASE("fill FILL2: Chance's RANDOM draws in 0..100, CONSTANT and FROM..TO reach 174 (OS-CECHANCE C2)") {
  fill::Plan p;
  p.what = fill::What::Fx1;
  p.type = fill::Type::Every;
  p.mode = fill::Mode::Random;
  p.fx_type = 4;  // Chance: the row reaches 174, the draw does not
  p.from = 0;
  p.to = 174;
  Rig r;
  r.rng.s = 7;
  CHECK(fill::apply(p, r.steps, kLen, r.rng, r.last) == kLen);
  for (unsigned i = 0; i < kLen; ++i) {
    CHECK(r.steps[i].fx[0].type == 4);
    CHECK_MESSAGE(r.steps[i].fx[0].value <= 100u, "step " << i);
  }
  const fill::Range d = fill::domain(p);
  CHECK((d.lo == 0 && d.hi == 174));  // CONSTANT / FROM..TO accept the whole range
  // CONSTANT 140 is kept.
  p.mode = fill::Mode::Const;
  p.from = 140;
  Rig c;
  CHECK(fill::apply(p, c.steps, 4, c.rng, c.last) == 4u);
  CHECK((c.steps[0].fx[0].type == 4 && c.steps[0].fx[0].value == 140));
  // RAMP from 100 to 174 walks into the conditions: the first hit 100, the last 174.
  p.mode = fill::Mode::Ramp;
  p.from = 100;
  p.to = 174;
  Rig q;
  q.rng.s = 3;
  CHECK(fill::apply(p, q.steps, 3, q.rng, q.last) == 3u);
  CHECK(q.steps[0].fx[0].value == 100);
  CHECK(q.steps[2].fx[0].value == 174);
  // A draw range above 100 still draws at its cap, not past it.
  p.mode = fill::Mode::Random;
  p.from = 150;
  p.to = 174;
  Rig w;
  w.rng.s = 11;
  (void)fill::apply(p, w.steps, 4, w.rng, w.last);
  for (unsigned i = 0; i < 4; ++i) {
    CHECK(w.steps[i].fx[0].value <= 100u);
  }
}
