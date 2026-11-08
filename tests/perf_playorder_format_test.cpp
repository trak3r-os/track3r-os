// SPDX-License-Identifier: GPL-3.0-or-later
// OS-PERFMODES M3: PERF and SCEN 1.2 (formats/native.h kStateVersionPlayOrder), the persistent half of the custom play orders C1..C20 (docs/specs/OS-PERFMODES.md section 3.4). The layout and the length are 1.0's
// (99 and 295 bytes): the version only says that a PLAYMODE slot (id 18) may hold 4..23. The writer writes 1.2 only when a slot (of a stored scene, for SCEN) holds such a value in any of its three rows, 1.1 when it
// holds the Perform FX 20 or 21 (OS-PERFFX P1), and 1.0, byte for byte as always, for every other bank, so the version written is the lowest that holds the data and the pinned fixtures do not move. The reader knows
// minor 2: in a 1.0 or 1.1 chunk a PLAYMODE value above 3 reads as RND (3, what every build before this one played), in 1.2 as it is, a value above 23 held to 23; a 1.3 chunk is a newer minor, read as far as 1.2.
// Nothing plays the orders yet (M2) and the screen does not offer them (M4): this file is the format only. The Python reference agrees on every case: ctest `project_python` (tests/o3project_roundtrip.py).
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/formats/native.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using namespace o3os::model;
using o3os::test::RawChunk;
using Bytes = std::vector<std::uint8_t>;

namespace {

constexpr std::uint8_t kFxPlayMode = 18;  // seq::PerfFx::PatternPlayMode (formats/src/native_state.h kPerfFxPlayMode)
constexpr int kC7 = 10;                    // the custom play order C7: PLAYMODE value 4 + 6

Project& proj(int i = 0) { return test::arena(i).project; }

const RawChunk* find(const std::vector<RawChunk>& cs, std::uint32_t id) {
  for (const RawChunk& c : cs) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

Bytes build(const std::vector<RawChunk>& cs) { return test::build_file(cs, {}); }

void put16(Bytes& b, std::size_t at, int v) {
  b[at] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
  b[at + 1] = static_cast<std::uint8_t>(v & 0xFF);
}

// Sets a PERF payload's version and slot `slot` (the payload: u16 version, u8 count, then 12 slots of u8 fx, u8 row, i16 value[3]).
void set_perf_slot(Bytes& p, std::uint8_t minor, int slot, std::uint8_t fx, std::uint8_t row, int v0, int v1, int v2) {
  p[0] = 0x01;
  p[1] = minor;
  const std::size_t o = 3u + 8u * static_cast<std::size_t>(slot);
  p[o] = fx;
  p[o + 1] = row;
  put16(p, o + 2, v0);
  put16(p, o + 4, v1);
  put16(p, o + 6, v2);
}

// Sets a SCEN payload's version and scene 0's slot `slot` (the payload: u16 version, u8 latch, u8 count, then per scene u8 stored and 12 slots).
void set_scen_slot(Bytes& p, std::uint8_t minor, int slot, std::uint8_t fx, std::uint8_t row, int v0, int v1, int v2) {
  p[0] = 0x01;
  p[1] = minor;
  const std::size_t o = 5u + 8u * static_cast<std::size_t>(slot);
  p[o] = fx;
  p[o + 1] = row;
  put16(p, o + 2, v0);
  put16(p, o + 4, v1);
  put16(p, o + 6, v2);
}

// A project with a PERF part (fill_state) and, for the SCEN cases, stored scenes.
void fill_perf_project() {
  test::fill_state(proj(2));
  ScenesState sc;
  init_scenes_state(sc);
  sc.scene[0].stored = 1;
  init_perf_state(sc.scene[0].slots);
  for (std::uint32_t i = 0; i < kPerfSlots; ++i) {
    for (std::int16_t& v : sc.scene[0].slots.value[i]) v = 0;
  }
  CHECK(proj(2).set_scenes(sc) == Status::Ok);
}

// Saves the project of arena 2 and returns the file; `perf` (when given) is set before saving.
Bytes save_perf_project(const PerfState* perf = nullptr) {
  if (perf != nullptr) {
    CHECK(proj(2).set_perf(*perf) == Status::Ok);
  }
  Bytes out;
  CHECK(test::save(proj(2), out) == Progress::Done);
  return out;
}

// Loads `file` into arena 1; the file must load (Done).
Project& load_file(const Bytes& file, Report* rep = nullptr) {
  CHECK(test::load(test::arena(1), file, rep) == Progress::Done);
  CHECK(proj(1).check_invariants());
  return proj(1);
}

// The payload of chunk `id` of `file`, edited by `fn`, in a new file.
template <typename Fn>
Bytes edit_chunk(const Bytes& file, std::uint32_t id, Fn fn) {
  std::vector<RawChunk> cs = test::split_chunks(file);
  for (RawChunk& c : cs) {
    if (c.id == id) {
      fn(c.payload);
    }
  }
  return build(cs);
}

}  // namespace

TEST_CASE("OS-PERFMODES M3: PERF is 1.0 byte for byte without a C order, 1.1 with the FX 20 or 21, 1.2 with a C order, and the version written is the lowest that holds the data") {
  fill_perf_project();
  // a bank without C orders (the state fixture's: no PLAYMODE slot holds 4..23) is the 1.0 chunk it always was
  {
    const Bytes file = save_perf_project();
    const std::vector<RawChunk> cs = test::split_chunks(file);
    const RawChunk* const perf = find(cs, kChunkPerf);
    NEED(perf != nullptr);
    CHECK(perf->payload.size() == 99u);
    CHECK(perf->payload[0] == 0x01);
    CHECK(perf->payload[1] == 0x00);
  }
  // C7 (value 10) in slot 3 row 2: PERF 1.2, and the file round trips byte for byte
  {
    PerfState s = proj(2).perf();
    s.fx[3] = kFxPlayMode;
    s.row[3] = 2;
    s.value[3][0] = 0;
    s.value[3][1] = kC7;
    s.value[3][2] = 0;
    CHECK(validate_perf_state(s) == nullptr);
    const Bytes file = save_perf_project(&s);
    const std::vector<RawChunk> cs = test::split_chunks(file);
    const RawChunk* const perf = find(cs, kChunkPerf);
    NEED(perf != nullptr);
    CHECK(perf->payload.size() == 99u);
    CHECK(perf->payload[0] == 0x01);
    CHECK(perf->payload[1] == 0x02);
    CHECK(perf->payload[3 + 8 * 3] == kFxPlayMode);
    CHECK(perf->payload[3 + 8 * 3 + 1] == 2);
    Report rep;
    Project& q = load_file(file, &rep);
    CHECK_FALSE(rep.newer_minor);
    CHECK(q.perf() == s);
    Bytes again;
    NEED(test::save(q, again) == Progress::Done);
    CHECK(again == file);
  }
  // the FX 20 or 21 alone: 1.1 (OS-PERFFX P1); a PLAYMODE value of 3 alone: 1.0; a C order together with the FX 20: 1.2 (the lowest that holds both)
  {
    PerfState s = proj(2).perf();
    s.fx[3] = 20;
    s.row[3] = 2;
    s.value[3][1] = 15;
    const Bytes f11 = save_perf_project(&s);
    CHECK(find(test::split_chunks(f11), kChunkPerf)->payload[1] == 0x01);
    s.fx[3] = kFxPlayMode;
    s.value[3][1] = 3;  // RND: no C order
    const Bytes f10 = save_perf_project(&s);
    CHECK(find(test::split_chunks(f10), kChunkPerf)->payload[1] == 0x00);
    s.fx[4] = 20;
    s.value[3][1] = kC7;
    const Bytes f12 = save_perf_project(&s);
    CHECK(find(test::split_chunks(f12), kChunkPerf)->payload[1] == 0x02);
    Project& q = load_file(f12);
    CHECK(q.perf() == s);
  }
}

TEST_CASE("OS-PERFMODES M3: the reader: in a 1.0 or 1.1 PERF a PLAYMODE value above 3 reads as RND (3), in 1.2 as it is, a value above 23 is held to 23; nothing refused, the next save writes the lowest version") {
  fill_perf_project();
  PerfState s = proj(2).perf();
  s.fx[3] = kFxPlayMode;
  s.row[3] = 2;
  s.value[3][1] = kC7;
  const Bytes file = save_perf_project(&s);  // a 1.2 file to edit
  struct Case {
    std::uint8_t minor;
    int value;
    int read;
    std::uint8_t written_minor;
  };
  const Case cases[] = {
      {0x00, 8, 3, 0x00},   // 1.0: above 3 as RND
      {0x01, 8, 3, 0x00},   // 1.1: likewise
      {0x00, 24, 3, 0x00},  // 1.0: 24 as RND too
      {0x02, 8, 8, 0x02},   // 1.2: as it is
      {0x02, 24, 23, 0x02}, // 1.2: held to 23
      {0x02, 100, 23, 0x02},
      {0x02, 3, 3, 0x00},   // 1.2 holding only RND: written back as 1.0
  };
  for (const Case& c : cases) {
    const Bytes edited = edit_chunk(file, kChunkPerf, [&c](Bytes& p) { set_perf_slot(p, c.minor, 3, kFxPlayMode, 2, 0, c.value, 0); });
    Report rep;
    Project& q = load_file(edited, &rep);
    CHECK_FALSE(rep.newer_minor);
    CHECK(q.perf().fx[3] == kFxPlayMode);
    CHECK(q.perf().row[3] == 2);
    CHECK(q.perf().value[3][1] == c.read);
    CHECK(q.perf().value[3][0] == 0);
    CHECK(q.perf().value[3][2] == 0);
    Bytes again;
    NEED(test::save(q, again) == Progress::Done);
    CHECK(find(test::split_chunks(again), kChunkPerf)->payload[1] == c.written_minor);
    CHECK(find(test::split_chunks(again), kChunkPerf)->payload[3 + 8 * 3 + 4] == static_cast<std::uint8_t>(c.read >> 8));
    CHECK(find(test::split_chunks(again), kChunkPerf)->payload[3 + 8 * 3 + 5] == static_cast<std::uint8_t>(c.read & 0xFF));
  }
  // a 1.2 chunk a byte long is Malformed (1.2 is known and exact), a 1.3 chunk is a newer minor: read as far as 1.2, the tail lost on the next save
  {
    const Bytes long12 = edit_chunk(file, kChunkPerf, [](Bytes& p) { p.push_back(0); });
    Report rep;
    CHECK(test::load(test::arena(1), long12, &rep) == Progress::Failed);
    CHECK(rep.error == Error::Malformed);
    const Bytes v13 = edit_chunk(file, kChunkPerf, [](Bytes& p) {
      p[1] = 0x03;
      for (int i = 0; i < 40; ++i) p.push_back(0xAB);
    });
    Project& q = load_file(v13, &rep);
    CHECK(rep.newer_minor);
    CHECK(q.perf().value[3][1] == kC7);
    Bytes again;
    NEED(test::save(q, again) == Progress::Done);
    const std::vector<RawChunk> cs = test::split_chunks(again);
    const RawChunk* const perf = find(cs, kChunkPerf);
    CHECK(perf->payload[1] == 0x02);
    CHECK(perf->payload.size() == 99u);
    CHECK(again == file);
  }
}

TEST_CASE("OS-PERFMODES M3: SCEN 1.2: a C order only in a stored scene makes SCEN 1.2 and leaves PERF 1.0; both versions load and round trip") {
  fill_perf_project();
  // C7 in scene 0 slot 3 row 2, the PERF slots without a C order: SCEN 1.2, PERF 1.0
  {
    ScenesState sc = proj(2).scenes();
    sc.scene[0].slots.fx[3] = kFxPlayMode;
    sc.scene[0].slots.row[3] = 2;
    sc.scene[0].slots.value[3][1] = kC7;
    CHECK(validate_scenes_state(sc) == nullptr);
    CHECK(proj(2).set_scenes(sc) == Status::Ok);
    const Bytes file = save_perf_project();
    const std::vector<RawChunk> cs = test::split_chunks(file);
    const RawChunk* const scen = find(cs, kChunkScen);
    const RawChunk* const perf = find(cs, kChunkPerf);
    NEED(scen != nullptr);
    NEED(perf != nullptr);
    CHECK(scen->payload.size() == 295u);
    CHECK(scen->payload[0] == 0x01);
    CHECK(scen->payload[1] == 0x02);
    CHECK(perf->payload[0] == 0x01);
    CHECK(perf->payload[1] == 0x00);
    Report rep;
    Project& q = load_file(file, &rep);
    CHECK_FALSE(rep.newer_minor);
    CHECK(q.scenes() == sc);
    CHECK(q.perf() == proj(2).perf());
    Bytes again;
    NEED(test::save(q, again) == Progress::Done);
    CHECK(again == file);
  }
  // the FX 20 or 21 in one stored scene and a C order in another: 1.2 outranks 1.1 whatever scene holds what
  {
    fill_perf_project();
    ScenesState sc = proj(2).scenes();
    sc.scene[0].stored = 1;
    sc.scene[0].slots.fx[0] = 20;
    sc.scene[0].slots.row[0] = 2;
    sc.scene[0].slots.value[0][1] = 15;
    sc.scene[1].stored = 1;
    sc.scene[1].slots.fx[3] = kFxPlayMode;
    sc.scene[1].slots.row[3] = 2;
    sc.scene[1].slots.value[3][1] = kC7;
    CHECK(validate_scenes_state(sc) == nullptr);
    CHECK(proj(2).set_scenes(sc) == Status::Ok);
    const Bytes file = save_perf_project();
    const std::vector<RawChunk> cs = test::split_chunks(file);
    const RawChunk* const scen = find(cs, kChunkScen);
    NEED(scen != nullptr);
    CHECK(scen->payload[1] == 0x02);
    Project& q = load_file(file);
    CHECK(q.scenes() == sc);
  }
  // a stored scene without a C order (the persist2 fixture's) is the 1.0 SCEN it always was
  {
    test::fill_persist2(proj(2));
    Bytes out;
    NEED(test::save(proj(2), out) == Progress::Done);
    const std::vector<RawChunk> cs = test::split_chunks(out);
    const RawChunk* const scen = find(cs, kChunkScen);
    NEED(scen != nullptr);
    CHECK(scen->payload.size() == 295u);
    CHECK(scen->payload[1] == 0x00);
  }
  // the reader on SCEN: 1.0 / 1.1 above 3 as RND, 1.2 as it is, above 23 held to 23
  {
    fill_perf_project();
    ScenesState sc = proj(2).scenes();
    sc.scene[0].slots.fx[3] = kFxPlayMode;
    sc.scene[0].slots.row[3] = 2;
    sc.scene[0].slots.value[3][1] = kC7;
    CHECK(proj(2).set_scenes(sc) == Status::Ok);
    const Bytes file = save_perf_project();
    struct Case {
      std::uint8_t minor;
      int value;
      int read;
    };
    const Case cases[] = {
        {0x00, 8, 3},
        {0x01, 8, 3},
        {0x02, 8, 8},
        {0x02, 24, 23},
    };
    for (const Case& c : cases) {
      const Bytes edited = edit_chunk(file, kChunkScen, [&c](Bytes& p) { set_scen_slot(p, c.minor, 3, kFxPlayMode, 2, 0, c.value, 0); });
      Report rep;
      Project& q = load_file(edited, &rep);
      CHECK_FALSE(rep.newer_minor);
      CHECK(q.scenes().scene[0].slots.fx[3] == kFxPlayMode);
      CHECK(q.scenes().scene[0].slots.value[3][1] == c.read);
      Bytes again;
      NEED(test::save(q, again) == Progress::Done);
      const std::vector<RawChunk> cs = test::split_chunks(again);
      const RawChunk* const scen = find(cs, kChunkScen);
      CHECK(scen->payload[1] == (c.read > 3 ? 0x02 : 0x00));
      CHECK(scen->payload[5 + 8 * 3 + 5] == static_cast<std::uint8_t>(c.read & 0xFF));
    }
    // a scene that is not stored holds no order: its PLAYMODE bytes mean nothing (sanitise zeroes the scene)
    const Bytes notstored = edit_chunk(file, kChunkScen, [](Bytes& p) {
      p[4] = 0;  // scene 0 not stored
      set_scen_slot(p, 0x02, 3, kFxPlayMode, 2, 0, kC7, 0);
    });
    Project& q = load_file(notstored);
    CHECK(q.scenes().scene[0].stored == 0);
    Bytes again;
    NEED(test::save(q, again) == Progress::Done);
    CHECK(find(test::split_chunks(again), kChunkScen)->payload[1] == 0x00);  // and SCEN is written back as 1.0
  }
}
