// SPDX-License-Identifier: GPL-3.0-or-later
// OS-SCALES28 S1 (docs/specs/OS-SCALES28.md sections 3.1, 3.3 and 3.5): NOTE 1.1, the official scale numbering. The model and the runtime hold the official index 0..38
// (model::kNoteScaleMax); the NOTE 1.0 byte keeps its 1.0 meaning (the panel's twelve, model::kNotePanelScales) and a 1.1 chunk appends the official scale after `jump`, with the 1.0 byte 0
// (Chromatic, so an older build snaps no note outside the user's scale). This file is the format side: every scale round-trips, the exact bytes, the 1.0 files as the build before wrote them
// (mapped, and counted when out of range), a 1.1 chunk that holds one of the twelve written back as 1.0, the fallback and the counts of a damaged 1.1 chunk, a newer minor, the shape errors, and
// the fixture bytes that do not move. The runtime side is persist2_app_test.cpp, the Python mirror is ctest `project_python` (the `scales` fixture).
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/formats/crc32.h>
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

Project& proj(int i = 0) { return test::arena(i).project; }

Bytes build(const std::vector<RawChunk>& cs) { return test::build_file(cs, {}); }

// The persist2 fixture's file (the NOTE chunk is 1.0 there: Blues, scale10 9).
Bytes session_file() {
  test::fill_persist2(proj(2));
  Bytes out;
  CHECK(test::save(proj(2), out) == Progress::Done);
  return out;
}

// The file with the NOTE chunk's payload replaced.
std::vector<RawChunk> with_note(const Bytes& payload) {
  std::vector<RawChunk> out;
  for (const RawChunk& c : test::split_chunks(session_file())) {
    out.push_back(c.id == kChunkNote ? RawChunk{kChunkNote, payload} : c);
  }
  return out;
}

const RawChunk* find(const std::vector<RawChunk>& cs, std::uint32_t id) {
  for (const RawChunk& c : cs) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

// A project whose NOTE part is `n`, saved: the NOTE chunk's payload.
Bytes note_payload_of(const NoteState& n) {
  Project& p = proj(0);
  p.reset();
  CHECK(p.set_note(n) == Status::Ok);
  Bytes out;
  CHECK(test::save(p, out) == Progress::Done);
  const std::vector<RawChunk> cs = test::split_chunks(out);
  const RawChunk* c = find(cs, kChunkNote);
  CHECK(c != nullptr);
  return c != nullptr ? c->payload : Bytes{};
}

}  // namespace

TEST_CASE("note11: every one of the 39 scales round-trips; the twelve write NOTE 1.0 byte for byte, the 27 write 1.1") {
  for (unsigned scale = 0; scale <= kNoteScaleMax; ++scale) {
    NoteState n;
    init_note_state(n);
    n.root = 7;
    n.scale = static_cast<std::uint8_t>(scale);
    n.octave = 5;
    n.lock = 1;
    n.jump = 4;
    const Bytes payload = note_payload_of(n);
    bool panel = false;
    for (std::uint8_t s : kNotePanelScales) {
      if (s == scale) panel = true;
    }
    if (panel) {
      // one of the twelve: 1.0, 7 bytes, the byte is its pad - 1 (the bytes of before, whatever the scale's official index is)
      std::uint8_t byte = 0xFF;
      for (std::uint32_t i = 0; i < sizeof kNotePanelScales; ++i) {
        if (kNotePanelScales[i] == scale) byte = static_cast<std::uint8_t>(i);
      }
      CHECK(payload == Bytes({0x01, 0x00, 7, byte, 5, 1, 4}));
    } else {
      // one of the 27: 1.1, 8 bytes, the 1.0 byte 0 and the official scale appended
      CHECK(payload == Bytes({0x01, 0x01, 7, 0, 5, 1, 4, static_cast<std::uint8_t>(scale)}));
    }
    // model -> write -> read -> the same model
    Project& p = proj(0);
    p.reset();
    CHECK(p.set_note(n) == Status::Ok);
    Bytes file;
    NEED(test::save(p, file) == Progress::Done);
    Report rep;
    NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
    CHECK_FALSE(rep.newer_minor);
    CHECK(rep.settings_skipped == 0u);
    CHECK(proj(1).note() == n);
    Bytes again;
    NEED(test::save(proj(1), again) == Progress::Done);
    CHECK(again == file);
    CHECK(find(test::split_chunks(again), kChunkNote)->payload == payload);
  }
}

// OS-DEFSCALE: a new project is C major and saves and reloads as it; the user's Chromatic saves and reloads as Chromatic; a file with no NOTE chunk reads Chromatic, whatever the document held before.
TEST_CASE("defscale: a new project is C major and round-trips; Chromatic round-trips; a file with no NOTE chunk loads Chromatic") {
  Project& p = proj(0);
  p.reset();
  CHECK(p.note().scale == 0u);  // the reader's default
  CHECK((p.state_present() & Project::kStateNote) == 0);
  p.start_new();
  CHECK(p.note().scale == 2u);  // Major, the official index
  CHECK(p.note().scale == kNotePanelScales[1]);
  CHECK(p.note().root == 0u);
  CHECK((p.state_present() & Project::kStateNote) != 0);
  Bytes major;
  NEED(test::save(p, major) == Progress::Done);
  CHECK(find(test::split_chunks(major), kChunkNote)->payload == Bytes({0x01, 0x00, 0, 1, 3, 0, 1}));  // 1.0: C, pad 2 - 1 = Major, octave 3, lock off, jump 1
  Report rep;
  NEED(test::load(test::arena(1), major, &rep) == Progress::Done);
  CHECK(proj(1).note().scale == 2u);
  CHECK(proj(1).note() == p.note());

  // the user sets Chromatic on the new project: written, and read back as Chromatic
  NoteState n = p.note();
  n.scale = 0;
  CHECK(p.set_note(n) == Status::Ok);
  Bytes chromatic;
  NEED(test::save(p, chromatic) == Progress::Done);
  CHECK(find(test::split_chunks(chromatic), kChunkNote)->payload == Bytes({0x01, 0x00, 0, 0, 3, 0, 1}));
  NEED(test::load(test::arena(1), chromatic, &rep) == Progress::Done);
  CHECK(proj(1).note().scale == 0u);
  CHECK((proj(1).state_present() & Project::kStateNote) != 0);

  // an old file: no NOTE chunk. Loaded over a document that was a new project, it is Chromatic and the part is absent
  Project& old = proj(2);
  old.reset();
  Bytes none;
  NEED(test::save(old, none) == Progress::Done);
  CHECK(find(test::split_chunks(none), kChunkNote) == nullptr);
  proj(1).reset();
  proj(1).start_new();
  NEED(test::load(test::arena(1), none, &rep) == Progress::Done);
  CHECK(proj(1).note().scale == 0u);
  CHECK((proj(1).state_present() & Project::kStateNote) == 0);
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == none);  // the same bytes: an old file saves as it always did
}

TEST_CASE("note11: the exact bytes of the two shapes (Blues 1.0, Hirajoshi 1.1)") {
  NoteState n;
  init_note_state(n);
  n.root = 7;
  n.octave = 5;
  n.lock = 1;
  n.jump = 4;
  n.scale = 15;  // Blues, the official index
  CHECK(note_payload_of(n) == Bytes({0x01, 0x00, 7, 9, 5, 1, 4}));  // as today: the pad 10 - 1
  n.scale = 26;                                                        // Hirajoshi
  CHECK(note_payload_of(n) == Bytes({0x01, 0x01, 7, 0, 5, 1, 4, 26}));
  n.scale = 0;  // Chromatic, the default, is one of the twelve
  CHECK(note_payload_of(n) == Bytes({0x01, 0x00, 7, 0, 5, 1, 4}));
  n.scale = 23;  // Whole tone, the twelfth pad
  CHECK(note_payload_of(n) == Bytes({0x01, 0x00, 7, 11, 5, 1, 4}));
}

TEST_CASE("note11: a 1.0 chunk as the build before wrote it reads the mapped scale (all twelve, and 12..255 as Chromatic, counted)") {
  Report rep;
  for (unsigned b = 0; b < 256; ++b) {
    std::vector<RawChunk> cs = with_note(Bytes({0x01, 0x00, 7, static_cast<std::uint8_t>(b), 5, 1, 4}));
    NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
    CHECK_FALSE(rep.newer_minor);
    if (b < 12) {
      CHECK(proj(1).note().scale == kNotePanelScales[b]);
      CHECK(rep.settings_skipped == 0u);
    } else {
      CHECK(proj(1).note().scale == 0);  // Chromatic
      CHECK(rep.settings_skipped == 1u);
    }
    // written back as 1.0, with the byte of the panel scale (Chromatic's pad 1 for the ones above 11)
    Bytes again;
    NEED(test::save(proj(1), again) == Progress::Done);
    const std::uint8_t byte = static_cast<std::uint8_t>(b < 12 ? b : 0);
    CHECK(find(test::split_chunks(again), kChunkNote)->payload == Bytes({0x01, 0x00, 7, byte, 5, 1, 4}));
  }
}

TEST_CASE("note11: a 1.1 chunk that holds one of the twelve loads with it and is written back as 1.0") {
  for (unsigned b = 0; b < 12; ++b) {
    const std::uint8_t scale = kNotePanelScales[b];
    std::vector<RawChunk> cs = with_note(Bytes({0x01, 0x01, 7, static_cast<std::uint8_t>(b), 5, 1, 4, scale}));
    Report rep;
    NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
    CHECK_FALSE(rep.newer_minor);
    CHECK(rep.settings_skipped == 0u);
    CHECK(proj(1).note().scale == scale);
    Bytes again;
    NEED(test::save(proj(1), again) == Progress::Done);
    CHECK(find(test::split_chunks(again), kChunkNote)->payload == Bytes({0x01, 0x00, 7, static_cast<std::uint8_t>(b), 5, 1, 4}));
  }
}

TEST_CASE("note11: a 1.1 scale 39..255 falls back to the 1.0 byte's scale and is counted; every combination of a bad 1.0 byte and a bad 1.1 byte") {
  Report rep;
  for (unsigned s10 = 0; s10 < 256; s10 += (s10 < 13 ? 1u : 61u)) {  // 0..12 and a few above
    for (const unsigned s11 : {0u, 15u, 38u, 39u, 100u, 255u}) {
      std::vector<RawChunk> cs = with_note(Bytes({0x01, 0x01, 7, static_cast<std::uint8_t>(s10), 5, 1, 4, static_cast<std::uint8_t>(s11)}));
      NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
      CHECK_FALSE(rep.newer_minor);
      unsigned want_scale = 0;
      unsigned want_skipped = 0;
      if (s11 <= 38) {
        want_scale = s11;  // the appended byte wins
      } else {
        want_scale = s10 < 12 ? kNotePanelScales[s10] : 0;
        ++want_skipped;  // the appended byte above 38
      }
      if (s10 >= 12) ++want_skipped;  // as every 1.0 field out of range
      CHECK(proj(1).note().scale == want_scale);
      CHECK(rep.settings_skipped == want_skipped);
      // written back: 1.0 when the scale is one of the twelve (with its pad byte), else 1.1
      Bytes again;
      NEED(test::save(proj(1), again) == Progress::Done);
      bool panel = false;
      std::uint8_t byte = 0;
      for (std::uint32_t i = 0; i < sizeof kNotePanelScales; ++i) {
        if (kNotePanelScales[i] == want_scale) {
          panel = true;
          byte = static_cast<std::uint8_t>(i);
        }
      }
      const Bytes want = panel ? Bytes({0x01, 0x00, 7, byte, 5, 1, 4})
                               : Bytes({0x01, 0x01, 7, 0, 5, 1, 4, static_cast<std::uint8_t>(want_scale)});
      CHECK(find(test::split_chunks(again), kChunkNote)->payload == want);
    }
  }
}

TEST_CASE("note11: a 1.2 chunk of 9 bytes reads as 1.1 and sets newer_minor; a 1.1 chunk of 7 bytes is Malformed") {
  Report rep;
  // 1.2, 9 bytes: the 1.1 part read (the appended scale 26 wins), the ninth byte skipped, newer_minor set, and the next save is 1.1
  std::vector<RawChunk> cs = with_note(Bytes({0x01, 0x02, 7, 0, 5, 1, 4, 26, 0xAB}));
  NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
  CHECK(rep.newer_minor);
  CHECK(rep.settings_skipped == 0u);
  CHECK(proj(1).note().scale == 26);
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(find(test::split_chunks(again), kChunkNote)->payload == Bytes({0x01, 0x01, 7, 0, 5, 1, 4, 26}));
  // 1.1 of 7 bytes: short, as every chunk of a known version that is too short
  cs = with_note(Bytes({0x01, 0x01, 7, 0, 5, 1, 4}));
  CHECK(test::load(test::arena(1), build(cs), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  // 1.1 of 9 bytes: an unused byte in a chunk of this minor
  cs = with_note(Bytes({0x01, 0x01, 7, 0, 5, 1, 4, 26, 0xAB}));
  CHECK(test::load(test::arena(1), build(cs), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  // 1.0 of 8 bytes: likewise
  cs = with_note(Bytes({0x01, 0x00, 7, 9, 5, 1, 4, 0}));
  CHECK(test::load(test::arena(1), build(cs), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  // a newer major is refused whatever the length
  cs = with_note(Bytes({0x02, 0x00, 7, 9, 5, 1, 4}));
  CHECK(test::load(test::arena(1), build(cs), &rep) == Progress::Failed);
  CHECK(rep.error == Error::NewerMajor);
}

TEST_CASE("note11: the existing fixtures are byte-identical (the sizes and CRC-32s pinned before this slice)") {
  // The persist2 fixture's scale is Blues (the official 15 now): the NOTE 1.0 byte is still 9, so the file does not move.
  struct Old {
    void (*fill)(Project&);
    std::uint32_t size;
    std::uint32_t crc;
  };
  const Old olds[] = {{test::fill_small, 2648, 0x3f04f4e7u}, {test::fill_state, 3372, 0xa8d283e1u}, {test::fill_persist2, 3032, 0x40ad3678u}};
  for (const Old& o : olds) {
    o.fill(proj(0));
    Bytes file;
    NEED(test::save(proj(0), file) == Progress::Done);
    CHECK(file.size() == o.size);
    CHECK(formats::crc32(file.data(), file.size()) == o.crc);
    Report rep;
    NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
    CHECK(rep.settings_skipped == 0u);
    Bytes back;
    NEED(test::save(proj(1), back) == Progress::Done);
    CHECK(back == file);
  }
  // the persist2 fixture's NOTE chunk is the 1.0 one, byte for byte
  const std::vector<RawChunk> cs2 = test::split_chunks(session_file());
  const RawChunk* note = find(cs2, kChunkNote);
  NEED(note != nullptr);
  CHECK(note->payload == Bytes({0x01, 0x00, 7, 9, 5, 1, 4}));
  // the scales fixture (Hirajoshi) is 1.1: the same size as the persist2 one (the 8-byte payload loses its pad byte), and it round-trips
  test::fill_scales(proj(0));
  Bytes scales;
  NEED(test::save(proj(0), scales) == Progress::Done);
  CHECK(scales.size() == 3032u);
  CHECK(find(test::split_chunks(scales), kChunkNote)->payload == Bytes({0x01, 0x01, 7, 0, 5, 1, 4, 26}));
  Report rep;
  NEED(test::load(test::arena(1), scales, &rep) == Progress::Done);
  CHECK(proj(1).note().scale == 26);
  Bytes back;
  NEED(test::save(proj(1), back) == Progress::Done);
  CHECK(back == scales);
}
