// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_fixture.h"

#include "../formats/src/og_codec.h"  // the reader's own constants, to pin them to the seq::fx ids below

#include <o3os/formats/og.h>
#include <o3os/model/project.h>
#include <o3os/seq/fx.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace o3os;
using namespace o3os::test::og;
using formats::FormatError;
using formats::Progress;
using o3os::test::MemorySource;
using o3os::test::VectorSink;

namespace {

// The OG ids the v6 micromove logic keys on are the sequencer's own ids (formats/ cannot include seq/fx.h, so the
// pin lives here, where both are visible; OS-CEPAT S1 review minor).
static_assert(formats::og::codec::kFxMicroMove == seq::fx::kMicroMove &&
                  formats::og::codec::kFxRandomValue == seq::fx::kRandomValue,
              "the OG codec's FX 2 and Random-value ids are the sequencer's");

// The expected model step for six OG bytes, written the long way round (no shared code with the reader): OG
// note i8, -1 empty, -2 fade, -3 cut, -4 off, the other negatives kept in 129..252; instrument 0 on a note-less
// step names none, 255 reads as none, anything else is slot + 1.
model::Step expected_step(const std::uint8_t* s) {
  const int n = static_cast<std::int8_t>(s[0]);
  model::Step e{};
  if (n == -1) e.note = 0;
  else if (n >= 0) e.note = static_cast<std::uint8_t>(n + 1);
  else if (n == -2) e.note = 253;
  else if (n == -3) e.note = 254;
  else if (n == -4) e.note = 255;
  else e.note = static_cast<std::uint8_t>(257 + n);
  const bool special = e.note == 0 || e.note >= 253;
  if ((s[1] == 0 && special) || s[1] == 255) e.instr = 0;
  else e.instr = static_cast<std::uint8_t>(s[1] + 1);
  e.vol = 255;
  e.fx[0] = {s[2], s[3]};
  e.fx[1] = {s[4], s[5]};
  return e;
}

bool same_step(const model::Step& a, const model::Step& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// The expected model FX for an OG type and value, through the id map of 3.1 written the long way round (no shared
// code with the reader): 0..42 stay, 43..74 become 96..127, 75..127 are cleared, 128..255 stay.
model::StepFx expected_fx(std::uint8_t t, std::uint8_t v) {
  if (t > 42 && t <= 74) return {static_cast<std::uint8_t>(t + 53), v};
  if (t > 74 && t < 128) return {0, 0};
  return {t, v};
}

// The expected model step for eight v6 OG bytes (OS-CEPAT 3.3), written the long way round: bytes 0-5 as v5 with
// the id map, then the volume column (byte 6, i8) and the micromove column (byte 7, i8).
model::Step expected_step6(const std::uint8_t* s) {
  model::Step e = expected_step(s);  // note and instrument as v5
  e.fx[0] = expected_fx(s[2], s[3]);
  e.fx[1] = expected_fx(s[4], s[5]);
  const int vol = static_cast<std::int8_t>(s[6]);
  if (vol < 0) e.vol = 255;
  else if (vol <= 100) e.vol = static_cast<std::uint8_t>(vol * 127 / 100);
  else e.vol = 127;
  const int m = static_cast<std::int8_t>(s[7]);
  if (m >= 0 && e.fx[0].type != 2 && e.fx[1].type != 2) {  // no column holds FX 2: not superseded
    const int free_slot = e.fx[0].type == 0 ? 0 : (e.fx[1].type == 0 ? 1 : -1);
    if (free_slot >= 0 && !(e.fx[1 - free_slot].type == 16 && e.fx[1 - free_slot].value == 16)) {
      e.fx[free_slot] = {2, static_cast<std::uint8_t>(m)};  // Random value 16 in the other column would drop it
    }
  }
  return e;
}

model::Project& fresh(int i = 0) {
  model::Project& p = arena(i).project;
  p.reset();
  return p;
}

formats::og::PatternReader& reader() {
  static formats::og::PatternReader r;
  return r;
}

Progress import_pattern(const Bytes& file, unsigned id, formats::og::ImportReport* rep = nullptr, int arena_index = 0,
                        std::size_t budget = 4096) {
  formats::ReadParams params;
  params.index = static_cast<std::uint16_t>(id);
  const Progress p = run_reader(reader(), file, arena(arena_index), params, budget);
  if (rep != nullptr) *rep = reader().import_report();
  return p;
}

// Every step of the pattern is the OG step the spec says, and every track has its length.
void check_pattern(const model::Project& proj, unsigned id, const PatternSpec& spec) {
  const model::Pattern* p = proj.pattern(id);
  NEED(p != nullptr);
  for (unsigned t = 0; t < spec.tracks; ++t) {
    CHECK_MESSAGE(p->track_length[t] == spec.length[t] + 1, "track " << t);
    unsigned bad = 0;
    for (unsigned s = 0; s < 128; ++s) {
      if (!same_step(p->steps[t][s], expected_step(spec.step[t][s]))) ++bad;
    }
    CHECK_MESSAGE(bad == 0, "track " << t << ": " << bad << " steps differ");
  }
  for (unsigned t = spec.tracks; t < model::kMaxTracks; ++t) {  // tracks the file does not hold: empty, first track's length
    CHECK(p->track_length[t] == spec.length[0] + 1);
    for (unsigned s = 0; s < 128; ++s) CHECK(model::is_empty(p->steps[t][s]));
  }
}

// The same for a v6 file: every step maps as OS-CEPAT 3.3 says, every track has its length, the tracks the file
// does not hold are empty with the first track's length.
void check_pattern6(const model::Project& proj, unsigned id, const PatternSpec6& spec) {
  const model::Pattern* p = proj.pattern(id);
  NEED(p != nullptr);
  for (unsigned t = 0; t < spec.tracks; ++t) {
    CHECK_MESSAGE(p->track_length[t] == spec.length[t] + 1, "track " << t);
    unsigned bad = 0;
    for (unsigned s = 0; s < 128; ++s) {
      if (!same_step(p->steps[t][s], expected_step6(spec.step[t][s]))) ++bad;
    }
    CHECK_MESSAGE(bad == 0, "track " << t << ": " << bad << " steps differ");
  }
  for (unsigned t = spec.tracks; t < model::kMaxTracks; ++t) {
    CHECK(p->track_length[t] == spec.length[0] + 1);
    for (unsigned s = 0; s < 128; ++s) CHECK(model::is_empty(p->steps[t][s]));
  }
}

}  // namespace

TEST_CASE("og pattern: a 12-track v5 file maps every step, every length") {
  const PatternSpec spec = rich_pattern(12, 5, 1);
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  NEED(import_pattern(pattern_bytes(spec), 7, &rep) == Progress::Done);
  CHECK(rep.error == FormatError::None);
  CHECK(rep.version == 5);
  CHECK(rep.tracks == 12);
  CHECK(rep.patterns == 1);
  CHECK(!rep.unverified_version);
  CHECK(!rep.crc_mismatch);
  CHECK(proj.pattern_count() == 1);
  CHECK(proj.check_invariants());
  check_pattern(proj, 7, spec);
}

TEST_CASE("og pattern: 8-track files of versions 1 to 4 (6,184 bytes)") {
  for (unsigned v = 1; v <= 4; ++v) {
    const PatternSpec spec = rich_pattern(8, v, 10 + v);
    formats::og::ImportReport rep;
    model::Project& proj = fresh();
    NEED(import_pattern(pattern_bytes(spec), 3, &rep) == Progress::Done);
    CHECK(rep.tracks == 8);
    CHECK(rep.version == v);
    CHECK(!rep.unverified_version);
    check_pattern(proj, 3, spec);
  }
}

TEST_CASE("og pattern: v6 files of 8, 12 and 16 tracks load (OS-CEPAT S1)") {
  for (unsigned tracks : {8u, 12u, 16u}) {
    const PatternSpec6 spec = rich_pattern6(tracks, 6, 40 + tracks);
    const Bytes file = pattern6_bytes(spec);
    CHECK(file.size() == 28 + 1025u * tracks + 4);
    formats::og::ImportReport rep;
    model::Project& proj = fresh();
    NEED(import_pattern(file, 7, &rep) == Progress::Done);
    CHECK(rep.error == FormatError::None);
    CHECK(rep.version == 6);
    CHECK(rep.tracks == tracks);
    CHECK(rep.patterns == 1);
    CHECK(!rep.unverified_version);
    CHECK(!rep.crc_mismatch);
    CHECK(proj.pattern_count() == 1);
    CHECK(proj.check_invariants());
    check_pattern6(proj, 7, spec);
    // any step budget and any read size gives the same pattern
    model::Project& ref = fresh(1);
    NEED(import_pattern(file, 7, nullptr, 1) == Progress::Done);
    const model::Pattern* want = ref.pattern(7);
    NEED(want != nullptr);
    for (std::size_t budget : {std::size_t{1}, std::size_t{1025}, std::size_t{4096}}) {
      fresh(0);
      formats::ReadParams params;
      params.index = 7;
      CHECK(run_reader(reader(), file, arena(0), params, budget, 7, 5) == Progress::Done);
      const model::Pattern* got = arena(0).project.pattern(7);
      NEED(got != nullptr);
      CHECK(std::memcmp(got, want, sizeof *got) == 0);
    }
  }
}

TEST_CASE("og pattern: the v6 step mapping, every row of OS-CEPAT 3.3") {
  PatternSpec6 spec = empty_pattern6(8, 6);
  auto set = [&spec](unsigned s, std::uint8_t note, std::uint8_t instr, std::uint8_t t0, std::uint8_t v0,
                     std::uint8_t t1, std::uint8_t v1, std::int8_t vol, std::int8_t m) {
    std::uint8_t* st = spec.step[0][s];
    st[0] = note; st[1] = instr;
    st[2] = t0; st[3] = v0; st[4] = t1; st[5] = v1;
    st[6] = static_cast<std::uint8_t>(vol);
    st[7] = static_cast<std::uint8_t>(m);
  };
  // bytes 0-5 as v5: note and instrument as v5, FX through the id map of 3.1, value verbatim
  set(0, 60, 5, 42, 7, 43, 8, -1, -1);     // 42 stays; OG 43 -> model 96
  set(1, 60, 5, 74, 9, 75, 10, -1, -1);    // 74 -> 127; 75 -> (0, 0)
  set(2, 60, 5, 128, 200, 255, 1, -1, -1); // engine FX verbatim
  // volume: negative -> unset; 0..100 -> floor(v * 127 / 100); 101..127 -> 127 and counted
  set(3, 60, 5, 0, 0, 0, 0, -1, -1);
  set(4, 60, 5, 0, 0, 0, 0, -2, -1);
  set(5, 60, 5, 0, 0, 0, 0, 0, -1);
  set(6, 60, 5, 0, 0, 0, 0, 1, -1);
  set(7, 60, 5, 0, 0, 0, 0, 50, -1);
  set(8, 60, 5, 0, 0, 0, 0, 100, -1);
  set(9, 60, 5, 0, 0, 0, 0, 101, -1);
  set(10, 60, 5, 0, 0, 0, 0, 127, -1);
  // volume on a step with no note: kept in vol (plays nothing here; open research question, 3.3)
  set(11, 0xFF, 0, 0, 0, 0, 0, 50, -1);
  // micromove: negative -> nothing; into the first free column as FX 2 with the same value
  set(12, 60, 5, 0, 0, 0, 0, -1, -2);
  set(13, 60, 5, 0, 0, 0, 0, -1, 0);
  set(14, 60, 5, 0, 0, 0, 0, -1, 50);
  set(15, 60, 5, 0, 0, 0, 0, -1, 127);   // 101..127 are kept as stored
  set(16, 60, 5, 4, 50, 0, 0, -1, 100);  // fx[0] taken: m goes to fx[1]
  set(17, 60, 5, 2, 33, 0, 0, -1, 100);  // fx[0] already FX 2: superseded, not stored
  set(18, 60, 5, 4, 50, 5, 60, -1, 100); // no free column: dropped, counted
  set(19, 60, 5, 16, 16, 0, 0, -1, 100); // the other column is Random value 16: dropped, counted
  set(20, 60, 5, 0, 0, 16, 16, -1, 100); // same, m would have gone to fx[0]
  set(21, 60, 5, 0, 0, 2, 33, -1, 100);  // fx[1] already FX 2: superseded, the mirror of step 17
  set(22, 60, 5, 16, 50, 0, 0, -1, 100); // Random type in fx[0] but value 50: not 16, m is kept, in fx[1]
  set(23, 60, 5, 0, 0, 16, 50, -1, 100); // the mirror: Random 50 in fx[1], m goes to fx[0]
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  NEED(import_pattern(pattern6_bytes(spec), 2, &rep) == Progress::Done);
  const model::Pattern* p = proj.pattern(2);
  NEED(p != nullptr);
  const model::Step* s = p->steps[0];
  CHECK(s[0].fx[0].type == 42);
  CHECK(s[0].fx[0].value == 7);
  CHECK(s[0].fx[1].type == 96);
  CHECK(s[0].fx[1].value == 8);
  CHECK(s[1].fx[0].type == 127);
  CHECK(s[1].fx[0].value == 9);
  CHECK(s[1].fx[1].type == 0);
  CHECK(s[1].fx[1].value == 0);
  CHECK(s[2].fx[0].type == 128);
  CHECK(s[2].fx[0].value == 200);
  CHECK(s[2].fx[1].type == 255);
  CHECK(s[2].fx[1].value == 1);
  CHECK(s[3].vol == model::kVolumeUnset);
  CHECK(s[4].vol == model::kVolumeUnset);  // -2 is none as well
  CHECK(s[5].vol == 0);
  CHECK(s[6].vol == 1);    // floor(1 * 127 / 100)
  CHECK(s[7].vol == 63);   // floor(50 * 127 / 100)
  CHECK(s[8].vol == 127);
  CHECK(s[9].vol == 127);
  CHECK(s[10].vol == 127);
  CHECK(s[11].vol == 63);  // no note: kept in vol
  CHECK(s[12].fx[0].type == 0);
  CHECK(s[12].fx[1].type == 0);
  CHECK(s[13].fx[0].type == 2);
  CHECK(s[13].fx[0].value == 0);
  CHECK(s[14].fx[0].type == 2);
  CHECK(s[14].fx[0].value == 50);
  CHECK(s[15].fx[0].type == 2);
  CHECK(s[15].fx[0].value == 127);
  CHECK(s[16].fx[0].type == 4);
  CHECK(s[16].fx[1].type == 2);
  CHECK(s[16].fx[1].value == 100);
  CHECK(s[17].fx[0].type == 2);  // superseded
  CHECK(s[17].fx[0].value == 33);
  CHECK(s[17].fx[1].type == 0);
  CHECK(s[18].fx[0].type == 4);  // dropped
  CHECK(s[18].fx[1].type == 5);
  CHECK(s[19].fx[0].type == 16);  // dropped
  CHECK(s[19].fx[0].value == 16);
  CHECK(s[19].fx[1].type == 0);
  CHECK(s[20].fx[0].type == 0);  // dropped
  CHECK(s[20].fx[1].type == 16);
  CHECK(s[20].fx[1].value == 16);
  CHECK(s[21].fx[0].type == 0);  // superseded, the mirror
  CHECK(s[21].fx[1].type == 2);
  CHECK(s[21].fx[1].value == 33);
  CHECK(s[22].fx[0].type == 16);  // Random 50 does not drop m
  CHECK(s[22].fx[0].value == 50);
  CHECK(s[22].fx[1].type == 2);
  CHECK(s[22].fx[1].value == 100);
  CHECK(s[23].fx[0].type == 2);  // the mirror
  CHECK(s[23].fx[0].value == 100);
  CHECK(s[23].fx[1].type == 16);
  CHECK(s[23].fx[1].value == 50);
  CHECK(rep.values_clamped == 2);       // steps 9 and 10
  CHECK(rep.micromoves_dropped == 3);   // steps 18, 19 and 20
  CHECK(rep.ce_fx_cleared == 1);        // step 1, OG 75 only: the boundaries 74 (-> 127) and 128 (verbatim) are not counted (S2 review minor)
  check_pattern6(proj, 2, spec);        // the same mapping, computed independently
}

TEST_CASE("og pattern: the note byte maps all 256 values one to one, special notes keep no instrument") {
  PatternSpec spec = empty_pattern(8, 4);
  for (unsigned i = 0; i < 256; ++i) {
    spec.step[0][i % 128][0] = static_cast<std::uint8_t>(i);
    spec.step[1 + i / 128][i % 128][0] = static_cast<std::uint8_t>(i);
  }
  model::Project& proj = fresh();
  NEED(import_pattern(pattern_bytes(spec), 1) == Progress::Done);
  const model::Pattern* p = proj.pattern(1);
  NEED(p != nullptr);
  bool seen[256] = {};
  for (unsigned t = 1; t <= 2; ++t) {
    for (unsigned s = 0; s < 128; ++s) {
      seen[p->steps[t][s].note] = true;
    }
  }
  unsigned distinct = 0;
  for (bool b : seen) distinct += b ? 1u : 0u;
  CHECK(distinct == 256);
  CHECK(p->steps[1][0].note == 1);       // note 0 -> 1 (C-0)
  CHECK(p->steps[1][127].note == 128);   // note 127
  CHECK(p->steps[2][0].note == 129);     // byte 128 (-128), the first kept negative
  CHECK(p->steps[2][123].note == 252);   // byte 251 (-5), the last
  CHECK(p->steps[2][124].note == 255);   // byte 252: off
  CHECK(p->steps[2][125].note == 254);   // byte 253: cut
  CHECK(p->steps[2][126].note == 253);   // byte 254: fade
  CHECK(p->steps[2][127].note == 0);     // byte 255: empty

  // instruments: a note names slot + 1; empty, off, cut and fade with byte 0 name none
  PatternSpec ins = empty_pattern(8, 4);
  const std::uint8_t cases[][2] = {{5, 0}, {5, 47}, {5, 63}, {5, 255}, {0xFF, 0}, {0xFF, 9}, {0xFC, 0},
                                   {0xFC, 3}, {0xFD, 0}, {0xFE, 0}, {0xFE, 63}};
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
    ins.step[0][i][0] = cases[i][0];
    ins.step[0][i][1] = cases[i][1];
  }
  fresh();
  NEED(import_pattern(pattern_bytes(ins), 1) == Progress::Done);
  const model::Pattern* q = proj.pattern(1);
  NEED(q != nullptr);
  const std::uint8_t want[] = {1, 48, 64, 0, 0, 10, 0, 4, 0, 0, 64};
  for (unsigned i = 0; i < sizeof want; ++i) CHECK_MESSAGE(q->steps[0][i].instr == want[i], "case " << i);
}

TEST_CASE("og pattern: FX bytes and leftovers are kept verbatim, the volume column stays unset") {
  PatternSpec spec = empty_pattern(8, 3);
  spec.step[2][5][0] = 60;
  spec.step[2][5][2] = 18; spec.step[2][5][3] = 77;
  spec.step[2][5][4] = 200; spec.step[2][5][5] = 255;          // a type nobody defines
  spec.step[2][6][3] = 99;                                    // a value on an FX-less column
  model::Project& proj = fresh();
  NEED(import_pattern(pattern_bytes(spec), 2) == Progress::Done);
  const model::Pattern* p = proj.pattern(2);
  NEED(p != nullptr);
  CHECK(p->steps[2][5].fx[0].type == 18);
  CHECK(p->steps[2][5].fx[0].value == 77);
  CHECK(p->steps[2][5].fx[1].type == 200);
  CHECK(p->steps[2][5].fx[1].value == 255);
  CHECK(p->steps[2][5].vol == model::kVolumeUnset);
  CHECK(p->steps[2][6].fx[0].type == 0);
  CHECK(p->steps[2][6].fx[0].value == 99);
}

TEST_CASE("og pattern: the FX id map of OS-CEFXIDS (ADR 0004 amendment 3): OG 43..74 -> 96..127, 75..127 -> (0, 0), the rest verbatim") {
  PatternSpec spec = empty_pattern(8, 5);
  const std::uint8_t types[] = {42, 43, 50, 74, 75, 127, 128, 200};
  for (unsigned i = 0; i < 8; ++i) {
    spec.step[0][i][2] = types[i];
    spec.step[0][i][3] = static_cast<std::uint8_t>(10 + i);
    spec.step[0][i][4] = types[i];
    spec.step[0][i][5] = static_cast<std::uint8_t>(20 + i);
  }
  model::Project& proj = fresh();
  formats::og::ImportReport rep;
  NEED(import_pattern(pattern_bytes(spec), 2, &rep) == Progress::Done);
  const model::Pattern* p = proj.pattern(2);
  NEED(p != nullptr);
  const std::uint8_t want_type[] = {42, 96, 103, 127, 0, 0, 128, 200};
  const std::uint8_t want_value[] = {10, 11, 12, 13, 0, 0, 16, 17};       // FX 1: 10 + i, cleared to 0
  const std::uint8_t want_value2[] = {20, 21, 22, 23, 0, 0, 26, 27};     // FX 2: 20 + i, cleared to 0
  for (unsigned i = 0; i < 8; ++i) {
    CHECK_MESSAGE(p->steps[0][i].fx[0].type == want_type[i], "FX 1 type of step " << i);
    CHECK_MESSAGE(p->steps[0][i].fx[0].value == want_value[i], "FX 1 value of step " << i);
    CHECK_MESSAGE(p->steps[0][i].fx[1].type == want_type[i], "FX 2 type of step " << i);
    CHECK_MESSAGE(p->steps[0][i].fx[1].value == want_value2[i], "FX 2 value of step " << i);
  }
  CHECK(rep.ce_fx_cleared == 4);  // OG 75 and 127, in both columns; the boundaries 74 (-> 127) and 128 (verbatim) are in the fixture and not counted (OS-CEPAT S1b/S2, A10)
}

TEST_CASE("og pattern: the magic PM is accepted, the type field and header bytes are not judged") {
  PatternSpec spec = rich_pattern(8, 4, 2);
  spec.magic[0] = 'P';
  spec.magic[1] = 'M';
  Bytes f = pattern_bytes(spec);
  f[2] = 9;                    // a type that is not 2
  f[14] = f[15] = 0xA5;        // junk in the padding
  f[4] = 77;                   // another firmware
  for (std::size_t i = 16; i < 28; ++i) f[i] = 0x66;
  seal(f);
  model::Project& proj = fresh();
  CHECK(import_pattern(f, 4) == Progress::Done);
  check_pattern(proj, 4, spec);
}

TEST_CASE("og pattern: version 0 is refused, v6 reads verified, 7 and up with a v6 size read as unverified") {
  formats::og::ImportReport rep;
  PatternSpec spec = rich_pattern(12, 5, 3);
  Bytes f = pattern_bytes(spec);
  for (unsigned i = 8; i < 12; ++i) f[i] = 0;
  seal(f);
  model::Project& proj = fresh();
  CHECK(import_pattern(f, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::UnsupportedVersion);
  CHECK(proj.pattern_count() == 0);

  // v6 has its own layout (rows of 1,025 B, the declared size at offset 12) and is verified
  const PatternSpec6 s6 = rich_pattern6(16, 6, 31);
  fresh();
  CHECK(import_pattern(pattern6_bytes(s6), 1, &rep) == Progress::Done);
  CHECK(rep.error == FormatError::None);
  CHECK(rep.version == 6);
  CHECK(rep.tracks == 16);
  CHECK(!rep.unverified_version);
  check_pattern6(proj, 1, s6);

  // v7 and up with a v6 size: read with the v6 layout and flagged unverified (OS-CEPAT A1)
  for (unsigned v : {7u, 100u, 255u}) {
    const PatternSpec6 sv = rich_pattern6(12, v, 30 + v);
    fresh();
    CHECK(import_pattern(pattern6_bytes(sv), 1, &rep) == Progress::Done);
    CHECK_MESSAGE(rep.unverified_version, "version " << v);
    CHECK(rep.version == v);
    CHECK(rep.tracks == 12);
    check_pattern6(proj, 1, sv);
  }
  // a v6 version byte at a v5 size is refused: see og_hostile_test
}

TEST_CASE("og pattern: not a pattern file") {
  formats::og::ImportReport rep;
  Bytes f = pattern_bytes(rich_pattern(8, 4, 4));
  f[0] = 'X';
  model::Project& proj = fresh();
  CHECK(import_pattern(f, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::BadMagic);
  CHECK(proj.pattern_count() == 0);
  CHECK(import_pattern(Bytes{}, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Truncated);
}

TEST_CASE("og pattern: a bad CRC refuses the file and leaves the project as it was") {
  for (unsigned tracks : {8u, 12u, 16u}) {
    const Bytes good = pattern_bytes(rich_pattern(tracks, 5, 5));
    model::Project& proj = fresh();
    CHECK(proj.create_pattern(9, 16) == model::Status::Ok);  // something already there
    const std::uint32_t generation_before = proj.pattern_generation(9);
    for (std::size_t at : {std::size_t{16}, std::size_t{28}, std::size_t{1000}, good.size() - 5, good.size() - 4,
                           good.size() - 1}) {
      Bytes bad = good;
      bad[at] ^= 0x10;
      formats::og::ImportReport rep;
      CHECK_MESSAGE(import_pattern(bad, 2, &rep) == Progress::Failed, "tracks " << tracks << " at " << at);
      CHECK(rep.error == FormatError::BadCrc);
      CHECK(proj.pattern(2) == nullptr);
      CHECK(proj.pattern_count() == 1);
      CHECK(proj.pattern_generation(9) == generation_before);
    }
  }
}

TEST_CASE("og pattern: a track length byte above 127 is refused") {
  PatternSpec spec = rich_pattern(8, 4, 6);
  spec.length[3] = 128;
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  CHECK(import_pattern(pattern_bytes(spec), 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Malformed);
  CHECK(proj.pattern_count() == 0);
  spec.length[3] = 127;
  CHECK(import_pattern(pattern_bytes(spec), 1) == Progress::Done);
  CHECK(proj.pattern(1)->track_length[3] == 128);
}

TEST_CASE("og pattern: truncated at every offset") {
  for (unsigned tracks : {8u, 12u, 16u}) {
    const Bytes good = pattern_bytes(rich_pattern(tracks, 5, 7));
    model::Project& proj = fresh();
    const std::size_t stride =
#if defined(__arm__)
        37;
#else
        1;
#endif
    for (std::size_t n = 0; n < good.size(); n += (n < 64 || n + 64 > good.size()) ? 1 : stride) {
      const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
      formats::og::ImportReport rep;
      const Progress p = import_pattern(cut, 1, &rep);
      const bool failed = p == Progress::Failed;
      CHECK_MESSAGE(failed, "tracks " << tracks << " cut at " << n);
      // the two fork points (after row 8 and after row 12) read as the shorter file whose CRC the four bytes then
      // are: BadCrc there, Truncated everywhere else (OS-CEPAT S1b)
      if (n != 28 + 769 * 8 + 4 && n != 28 + 769 * 12 + 4) {
        CHECK_MESSAGE(rep.error == FormatError::Truncated, "tracks " << tracks << " cut at " << n);
      } else {
        CHECK_MESSAGE(rep.error == FormatError::BadCrc, "tracks " << tracks << " cut at " << n);
      }
      CHECK(proj.pattern_count() == 0);
    }
  }
}

TEST_CASE("og pattern: bytes after the end are refused") {
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  Bytes f = pattern_bytes(rich_pattern(12, 5, 8));
  f.push_back(0);  // five bytes after the twelfth row look like a 13th row: the file is then cut short inside it
  CHECK(import_pattern(f, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Truncated);
  CHECK(proj.pattern_count() == 0);
  f = pattern_bytes(rich_pattern(8, 4, 8));
  f.push_back(0);  // five bytes after the eighth row look like a ninth row: it is then cut short
  CHECK(import_pattern(f, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Truncated);
  f = pattern_bytes(rich_pattern(12, 5, 8));
  f.resize(f.size() + 4000, 0);  // rows 13-16 of zeros follow, then a CRC that does not match
  f[9256] = f[9257] = f[9258] = f[9259] = 0;  // the old CRC: as the 13th row's length byte it must be <= 127 to read on
  CHECK(import_pattern(f, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::BadCrc);
  CHECK(proj.pattern_count() == 0);
  Bytes g = pattern_bytes(rich_pattern(16, 5, 8));  // a 16-track v5 file with one byte after its CRC
  g.push_back(0);
  CHECK(import_pattern(g, 1, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Malformed);
  CHECK(proj.pattern_count() == 0);
}

TEST_CASE("og pattern: any step budget and any source behaviour gives the same pattern") {
  const PatternSpec spec = rich_pattern(12, 5, 9);
  const Bytes file = pattern_bytes(spec);
  model::Project& ref = fresh(1);
  NEED(import_pattern(file, 5, nullptr, 1) == Progress::Done);
  const model::Pattern* want = ref.pattern(5);
  NEED(want != nullptr);
  for (std::size_t budget : {std::size_t{1}, std::size_t{7}, std::size_t{769}, std::size_t{4096}}) {
    fresh(0);
    formats::ReadParams params;
    params.index = 5;
    CHECK(run_reader(reader(), file, arena(0), params, budget, 3, 5) == Progress::Done);  // 3-byte reads, a WouldBlock every 5th
    const model::Pattern* got = arena(0).project.pattern(5);
    NEED(got != nullptr);
    CHECK(std::memcmp(got, want, sizeof *got) == 0);
  }
}

TEST_CASE("og pattern: id out of range, duplicate id, full pool") {
  formats::og::ImportReport rep;
  const Bytes f = pattern_bytes(rich_pattern(8, 4, 10));
  model::Project& proj = fresh();
  CHECK(import_pattern(f, 0, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Malformed);
  CHECK(import_pattern(f, 256, &rep) == Progress::Failed);
  CHECK(import_pattern(f, 3) == Progress::Done);
  const model::Pattern* first = proj.pattern(3);
  NEED(first != nullptr);
  const model::Pattern copy = *first;
  CHECK(import_pattern(pattern_bytes(rich_pattern(8, 4, 11)), 3, &rep) == Progress::Failed);  // not replaced
  CHECK(rep.error == FormatError::Malformed);
  CHECK(std::memcmp(proj.pattern(3), &copy, sizeof copy) == 0);
  for (unsigned id = 1; proj.pattern_count() < model::kMaxPatterns; ++id) {
    if (id != 3) NEED(proj.create_pattern(id, 8) == model::Status::Ok);
  }
  CHECK(import_pattern(f, 200, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::LimitExceeded);
  CHECK(proj.pattern_count() == model::kMaxPatterns);
}

TEST_CASE("og pattern: a failing or lying source") {
  const Bytes f = pattern_bytes(rich_pattern(12, 5, 12));
  formats::og::PatternReader& r = reader();
  model::Project& proj = fresh();
  formats::ReadParams params;
  params.index = 1;
  {
    MemorySource src(f);
    src.fail_at = 3000;
    r.begin(src, arena(0), params);
    while (r.step(4096) == Progress::Running) {}
    CHECK(r.report().error == FormatError::SourceFailed);
    CHECK(proj.pattern_count() == 0);
  }
  for (MemorySource::Lie lie : {MemorySource::Lie::TooMany, MemorySource::Lie::OkZero}) {
    MemorySource src(f);
    src.lie = lie;
    r.begin(src, arena(0), params);
    while (r.step(4096) == Progress::Running) {}
    CHECK(r.report().error == FormatError::SourceFailed);
    CHECK(proj.pattern_count() == 0);
  }
}

// ---- export -------------------------------------------------------------------------------------------------

namespace {
formats::og::PatternWriter& writer() {
  static formats::og::PatternWriter w;
  return w;
}
Progress export_pattern(const model::Project& p, unsigned id, Bytes& out, std::size_t budget = 4096) {
  formats::WriteParams params;
  params.index = static_cast<std::uint16_t>(id);
  return run_writer(writer(), p, out, params, budget);
}
}  // namespace

TEST_CASE("og pattern: OG -> model -> OG is byte-identical for a 12-track v5 file") {
  for (std::uint32_t seed = 1; seed <= 4; ++seed) {
    const Bytes in = pattern_bytes(rich_pattern(12, 5, seed));
    model::Project& proj = fresh();
    NEED(import_pattern(in, 9) == Progress::Done);
    Bytes out;
    NEED(export_pattern(proj, 9, out) == Progress::Done);
    CHECK_MESSAGE(out == in, "seed " << seed);
    CHECK(writer().export_report().drops.tracks_beyond_12 == 0);
    CHECK(!writer().export_report().drops.any_lost());
    Bytes slow;  // one byte per step gives the same file
    NEED(export_pattern(proj, 9, slow, 1) == Progress::Done);
    CHECK(slow == in);
  }
}

TEST_CASE("og pattern: the instrument bytes 48 and 63 (MIDI channels 1 and 16 in this OS's design) import as instruments 49 and 64 and export the same bytes (OS-INSTRMIDI)") {
  PatternSpec spec = empty_pattern(12, 5);
  spec.step[0][0][0] = 60;
  spec.step[0][0][1] = 48;
  spec.step[0][1][0] = 60;
  spec.step[0][1][1] = 63;
  const Bytes in = pattern_bytes(spec);
  model::Project& proj = fresh();
  NEED(import_pattern(in, 3) == Progress::Done);
  const model::Pattern* p = proj.pattern(3);
  NEED(p != nullptr);
  CHECK(p->steps[0][0].instr == 49);
  CHECK(p->steps[0][1].instr == 64);
  Bytes out;
  NEED(export_pattern(proj, 3, out) == Progress::Done);
  CHECK(out == in);
}

TEST_CASE("og pattern: an 8-track file is exported as 12 tracks, the four new ones empty") {
  const PatternSpec spec = rich_pattern(8, 4, 5);
  model::Project& proj = fresh();
  NEED(import_pattern(pattern_bytes(spec), 1) == Progress::Done);
  Bytes out;
  NEED(export_pattern(proj, 1, out) == Progress::Done);
  PatternSpec want = spec;  // the same eight tracks, then four empty ones of the first track's length
  want.tracks = 12;
  for (unsigned t = 8; t < 12; ++t) {
    want.length[t] = spec.length[0];
    for (unsigned s = 0; s < 128; ++s) {
      std::memset(want.step[t][s], 0, 6);
      want.step[t][s][0] = 0xFF;
    }
  }
  want.version = 5;
  CHECK(out == pattern_bytes(want));
}

TEST_CASE("og pattern: a 16-track v5 file (12,336 bytes, tracker-ce) loads 16 tracks, Export writes 12 (OS-CEPAT S1b)") {
  const PatternSpec spec = rich_pattern(16, 5, 21);
  const Bytes file = pattern_bytes(spec);
  CHECK(file.size() == 12336);
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  NEED(import_pattern(file, 7, &rep) == Progress::Done);
  CHECK(rep.error == FormatError::None);
  CHECK(rep.version == 5);
  CHECK(rep.tracks == 16);
  CHECK(rep.patterns == 1);
  CHECK(!rep.unverified_version);
  CHECK(!rep.crc_mismatch);
  CHECK(proj.pattern_count() == 1);
  CHECK(proj.check_invariants());
  check_pattern(proj, 7, spec);  // tracks 13-16 land in the model's tracks 13-16
  // any step budget and any read size gives the same pattern
  model::Project& ref = fresh(1);
  NEED(import_pattern(file, 7, nullptr, 1) == Progress::Done);
  const model::Pattern* want = ref.pattern(7);
  NEED(want != nullptr);
  for (std::size_t budget : {std::size_t{1}, std::size_t{769}, std::size_t{4096}}) {
    fresh(0);
    formats::ReadParams params;
    params.index = 7;
    CHECK(run_reader(reader(), file, arena(0), params, budget, 7, 5) == Progress::Done);
    const model::Pattern* got = arena(0).project.pattern(7);
    NEED(got != nullptr);
    CHECK(std::memcmp(got, want, sizeof *got) == 0);
  }
  // Export still writes 12 tracks: what tracks 13-16 hold is counted
  Bytes out;
  NEED(export_pattern(proj, 7, out) == Progress::Done);
  CHECK(out.size() == 9260);
  CHECK(writer().export_report().drops.tracks_beyond_12 > 0);
  // and the 8- and 12-track v5 files still parse byte-for-byte as before (12-track: identical round trip; 8-track:
  // exported as 12 tracks with empty ones, as the test above pins)
  {
    const Bytes in12 = pattern_bytes(rich_pattern(12, 5, 34));
    fresh(0);
    NEED(import_pattern(in12, 3) == Progress::Done);
    Bytes back;
    NEED(export_pattern(proj, 3, back) == Progress::Done);
    CHECK(back == in12);
    const Bytes in8 = pattern_bytes(rich_pattern(8, 5, 42));
    fresh(0);
    formats::og::ImportReport rep8;
    NEED(import_pattern(in8, 3, &rep8) == Progress::Done);
    CHECK(rep8.tracks == 8);
    NEED(export_pattern(proj, 3, back) == Progress::Done);
    CHECK(back.size() == 9260);
  }
}

TEST_CASE("og pattern: the header Export writes") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(1, 32) == model::Status::Ok);
  Bytes out;
  NEED(export_pattern(proj, 1, out) == Progress::Done);
  NEED(out.size() == 9260);
  const std::uint8_t head[28] = {'K', 'S', 2, 0, 1, 9, 0, 1, 5, 5, 5, 5, 0x2C, 0x24, 0, 0,
                                 0, 0, 0xC8, 0x42, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(std::memcmp(out.data(), head, 28) == 0);
  CHECK(out[28] == 31);                                  // 32 steps: the index of the last
  CHECK(std::memcmp(&out[29], "\xFF\x00\x00\x00\x00\x00", 6) == 0);  // an empty step
  CHECK(r32(out, 9256) == formats::crc32(out.data(), 9256));
}

TEST_CASE("og pattern: the volume column goes to a free FX slot as FX 18, and what cannot is counted") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(1, 32) == model::Status::Ok);
  auto step = [](std::uint8_t note, std::uint8_t vol, std::uint8_t t0, std::uint8_t v0, std::uint8_t t1,
                 std::uint8_t v1) {
    model::Step s{};
    s.note = note;
    s.instr = 3;
    s.vol = vol;
    s.fx[0] = {t0, v0};
    s.fx[1] = {t1, v1};
    return s;
  };
  NEED(proj.set_step(1, 0, 0, step(10, 127, 0, 0, 0, 0)) == model::Status::Ok);    // free slot 1 -> FX 18 = 100
  NEED(proj.set_step(1, 0, 1, step(10, 0, 0, 55, 0, 0)) == model::Status::Ok);     // a leftover value is overwritten
  NEED(proj.set_step(1, 0, 2, step(10, 64, 4, 50, 0, 0)) == model::Status::Ok);    // slot 2 free
  NEED(proj.set_step(1, 0, 3, step(10, 100, 4, 50, 5, 60)) == model::Status::Ok);  // both taken: dropped
  NEED(proj.set_step(1, 0, 4, step(10, 50, 18, 33, 0, 0)) == model::Status::Ok);   // FX 18 already there: dropped
  NEED(proj.set_step(1, 0, 5, step(10, 1, 200, 7, 0, 0)) == model::Status::Ok);    // engine FX cleared, slot freed
  NEED(proj.set_step(1, 0, 6, step(10, model::kVolumeUnset, 4, 9, 0, 0)) == model::Status::Ok);  // no volume
  NEED(proj.set_step(1, 0, 7, step(10, 200, 0, 0, 0, 0)) == model::Status::Ok);    // out of range: full
  NEED(proj.set_step(1, 0, 8, step(10, 127, 130, 1, 140, 2)) == model::Status::Ok);  // both engine: cleared, then FX 18
  Bytes out;
  NEED(export_pattern(proj, 1, out) == Progress::Done);
  auto at = [&out](unsigned s) { return &out[29 + 6 * s]; };
  auto eq = [](const std::uint8_t* p, std::initializer_list<std::uint8_t> v) {
    return std::equal(v.begin(), v.end(), p);
  };
  CHECK(eq(at(0), {9, 2, 18, 100, 0, 0}));
  CHECK(eq(at(1), {9, 2, 18, 0, 0, 0}));
  CHECK(eq(at(2), {9, 2, 4, 50, 18, 50}));
  CHECK(eq(at(3), {9, 2, 4, 50, 5, 60}));
  CHECK(eq(at(4), {9, 2, 18, 33, 0, 0}));
  CHECK(eq(at(5), {9, 2, 18, 1, 0, 0}));
  CHECK(eq(at(6), {9, 2, 4, 9, 0, 0}));
  CHECK(eq(at(7), {9, 2, 18, 100, 0, 0}));
  CHECK(eq(at(8), {9, 2, 18, 100, 0, 0}));
  const formats::og::ExportDrops& d = writer().export_report().drops;
  CHECK(d.volume_as_fx18 == 6);
  CHECK(d.volume_dropped == 2);
  CHECK(d.engine_fx == 3);
  CHECK(d.any_lost());
}

TEST_CASE("og pattern: core FX 43..95 and tracker-ce FX 96..127 are written as none and counted separately, every other byte is as it was (ADR 0004 amendments 1-3)") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(1, 16) == model::Status::Ok);
  auto step = [](std::uint8_t t0, std::uint8_t v0, std::uint8_t t1, std::uint8_t v1) {
    model::Step s{};
    s.note = 10;
    s.instr = 3;
    s.vol = model::kVolumeUnset;
    s.fx[0] = {t0, v0};
    s.fx[1] = {t1, v1};
    return s;
  };
  // the last OG type and the first core one; the tracker FX 46..50, 51, Channel pan 57 and CF3's Channel volume 58 and Panbrello 62; the first tracker-ce type and the last one, and the first engine one
  NEED(proj.set_step(1, 0, 0, step(42, 7, 43, 8)) == model::Status::Ok);      // 1 core dropped
  NEED(proj.set_step(1, 0, 1, step(46, 0x37, 50, 0x12)) == model::Status::Ok);  // 2 core
  NEED(proj.set_step(1, 0, 2, step(96, 255, 127, 9)) == model::Status::Ok);    // 2 tracker-ce dropped
  NEED(proj.set_step(1, 0, 3, step(4, 50, 5, 60)) == model::Status::Ok);       // OG types: kept
  NEED(proj.set_step(1, 0, 4, step(45, 1, 0, 77)) == model::Status::Ok);       // the leftover value of the free column stays
  NEED(proj.set_step(1, 0, 5, step(51, 0x44, 57, 75)) == model::Status::Ok);   // 2 core: Pan slide and Channel pan (57) are dropped, not turned into the Panning FX (31)
  NEED(proj.set_step(1, 0, 6, step(58, 32, 62, 0x3A)) == model::Status::Ok);   // 2 core: CF3's Channel volume and Panbrello are dropped likewise
  NEED(proj.set_step(1, 0, 7, step(128, 9, 200, 3)) == model::Status::Ok);    // 2 engine
  Bytes out;
  NEED(export_pattern(proj, 1, out) == Progress::Done);
  auto at = [&out](unsigned s) { return &out[29 + 6 * s]; };
  auto eq = [](const std::uint8_t* p, std::initializer_list<std::uint8_t> v) { return std::equal(v.begin(), v.end(), p); };
  CHECK(eq(at(0), {9, 2, 42, 7, 0, 0}));
  CHECK(eq(at(1), {9, 2, 0, 0, 0, 0}));
  CHECK(eq(at(2), {9, 2, 0, 0, 0, 0}));
  CHECK(eq(at(3), {9, 2, 4, 50, 5, 60}));
  CHECK(eq(at(4), {9, 2, 0, 0, 0, 77}));
  CHECK(eq(at(5), {9, 2, 0, 0, 0, 0}));
  CHECK(eq(at(6), {9, 2, 0, 0, 0, 0}));
  CHECK(eq(at(7), {9, 2, 0, 0, 0, 0}));
  const formats::og::ExportDrops& d = writer().export_report().drops;
  CHECK(d.core_fx == 8);  // 43, 46, 50, 45, 51, 57, 58, 62
  CHECK(d.ce_fx == 2);    // 96 and 127
  CHECK(d.engine_fx == 2);
  CHECK(d.volume_as_fx18 == 0);
  CHECK(d.any_lost());
  // every other byte of the file is what a file of the same pattern with those cells emptied would hold
  Bytes expect;
  {
    model::Project& p2 = fresh(1);
    NEED(p2.create_pattern(1, 16) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 0, step(42, 7, 0, 0)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 1, step(0, 0, 0, 0)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 2, step(0, 0, 0, 0)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 3, step(4, 50, 5, 60)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 4, step(0, 0, 0, 77)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 5, step(0, 0, 0, 0)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 6, step(0, 0, 0, 0)) == model::Status::Ok);
    NEED(p2.set_step(1, 0, 7, step(0, 0, 0, 0)) == model::Status::Ok);
    NEED(export_pattern(p2, 1, expect) == Progress::Done);
  }
  CHECK(out == expect);
  CHECK(writer().export_report().drops.core_fx == 0);  // the clean pattern drops none
  CHECK(writer().export_report().drops.ce_fx == 0);
  // a native copy of the pattern still holds the FX
  CHECK(proj.pattern(1)->steps[0][2].fx[0].type == 96);
}

TEST_CASE("og pattern: a pattern read through the id map exports with the mapped ids counted as ce_fx and the cleared ones as nothing (OS-CEFXIDS)") {
  PatternSpec spec = empty_pattern(8, 5);
  const std::uint8_t types[] = {42, 43, 50, 74, 75, 127, 128, 200};
  for (unsigned i = 0; i < 8; ++i) {
    spec.step[0][i][2] = types[i];
    spec.step[0][i][3] = static_cast<std::uint8_t>(10 + i);
    spec.step[0][i][4] = types[i];
    spec.step[0][i][5] = static_cast<std::uint8_t>(20 + i);
  }
  model::Project& proj = fresh();
  NEED(import_pattern(pattern_bytes(spec), 2) == Progress::Done);
  Bytes out;
  NEED(export_pattern(proj, 2, out) == Progress::Done);
  auto at = [&out](unsigned s) { return &out[29 + 6 * s]; };
  CHECK(at(0)[2] == 42);  // the OG type 42 is written as it is, value and all
  CHECK(at(0)[3] == 10);
  CHECK(at(0)[4] == 42);
  CHECK(at(0)[5] == 20);
  for (unsigned i = 1; i < 8; ++i) {
    CHECK(at(i)[2] == 0);
    CHECK(at(i)[3] == 0);
    CHECK(at(i)[4] == 0);
    CHECK(at(i)[5] == 0);
  }
  const formats::og::ExportDrops& d = writer().export_report().drops;
  CHECK(d.ce_fx == 6);      // 43, 50 and 74, in both columns
  CHECK(d.engine_fx == 4);  // 128 and 200, in both columns
  CHECK(d.core_fx == 0);
  CHECK(d.any_lost());
}

TEST_CASE("og pattern: tracks 13-16 are left out and counted, reserved notes survive") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(2, 16) == model::Status::Ok);
  model::Step s{};
  s.note = 200;  // a reserved model note: an i8 of -57 in the file
  s.instr = 1;
  s.vol = model::kVolumeUnset;
  NEED(proj.set_step(2, 3, 0, s) == model::Status::Ok);
  s.note = 5;
  NEED(proj.set_step(2, 12, 0, s) == model::Status::Ok);
  NEED(proj.set_step(2, 15, 127, s) == model::Status::Ok);
  NEED(proj.set_step(2, 15, 126, s) == model::Status::Ok);
  NEED(proj.set_track_length(2, 5, 128) == model::Status::Ok);
  Bytes out;
  NEED(export_pattern(proj, 2, out) == Progress::Done);
  CHECK(out[28 + 769 * 3 + 1] == 199);                      // 200 - 1 = 0xC7 = -57
  CHECK(out[28 + 769 * 5] == 127);                          // 128 steps: the index of the last is 127
  CHECK(out[28 + 769 * 0] == 15);
  CHECK(writer().export_report().drops.tracks_beyond_12 == 3);
  // and it reads back as the same pattern
  NEED(import_pattern(out, 4) == Progress::Done);
  const model::Pattern* a = proj.pattern(2);
  const model::Pattern* b = proj.pattern(4);
  NEED(a != nullptr && b != nullptr);
  for (unsigned t = 0; t < 12; ++t) {
    CHECK(a->track_length[t] == b->track_length[t]);
    CHECK(std::memcmp(a->steps[t], b->steps[t], sizeof a->steps[t]) == 0);
  }
}

TEST_CASE("og pattern: writer refusals and failures") {
  model::Project& proj = fresh();
  NEED(proj.create_pattern(1, 8) == model::Status::Ok);
  Bytes out;
  CHECK(export_pattern(proj, 2, out) == Progress::Failed);  // no such pattern
  CHECK(writer().report().error == FormatError::Malformed);
  CHECK(export_pattern(proj, 0, out) == Progress::Failed);
  {  // the sink fails
    VectorSink sink;
    sink.fail_after = 5000;
    formats::WriteParams params;
    params.index = 1;
    writer().begin(proj, sink, params);
    while (writer().step(4096) == Progress::Running) {}
    CHECK(writer().report().error == FormatError::SinkFailed);
  }
  for (VectorSink::Lie lie : {VectorSink::Lie::TooMany, VectorSink::Lie::OkZero, VectorSink::Lie::End}) {
    VectorSink sink;
    sink.lie = lie;
    formats::WriteParams params;
    params.index = 1;
    writer().begin(proj, sink, params);
    while (writer().step(4096) == Progress::Running) {}
    CHECK(writer().report().error == FormatError::SinkFailed);
  }
  {  // the project changes between two steps
    VectorSink sink;
    formats::WriteParams params;
    params.index = 1;
    writer().begin(proj, sink, params);
    CHECK(writer().step(1000) == Progress::Running);
    NEED(proj.set_pattern_name(1, "x", 1) == model::Status::Ok);
    CHECK(writer().step(1000) == Progress::Failed);
    CHECK(writer().report().error == FormatError::Modified);
  }
  {  // a sink that blocks now and then
    VectorSink sink;
    sink.block_every = 3;
    sink.max_per_write = 100;
    formats::WriteParams params;
    params.index = 1;
    writer().begin(proj, sink, params);
    unsigned steps = 0;
    while (writer().step(4096) == Progress::Running && ++steps < 100000) {}
    CHECK(writer().progress() == Progress::Done);
    CHECK(sink.bytes.size() == 9260);
  }
}
