// SPDX-License-Identifier: GPL-3.0-or-later
// OS-MASTEREQ slice 1 (docs/specs/OS-MASTEREQ.md, docs/formats/o3-project.md "The MEQ chunk"): the master output EQ as data of the native project, the MEQ chunk. This file is the format side: the exact
// bytes (written here from the description, not by the writer), the round trip, the files that never had the chunk (they keep their bytes and load flat, whatever the document held before), the chunk's
// shape rules and the verdict on each damaged form, the clamp, the values set by the Reader's last step (a load that fails before it leaves the flat EQ that reset() gave), the held value of a Reader
// that is used twice, and the bytes an older build writes back (it keeps MEQ as an unknown chunk). The use of the values (the codec) is platform/mk66/tests/mk66_sgtl5000_test.cpp.
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/memory_io.h"
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

const MasterEq kEq = {{-12, -1, 0, 6, 12}, 100};
const Bytes kEqPayload = {0x01, 0x00, 0xF4, 0xFF, 0x00, 0x06, 0x0C, 0x64};

// The persist2 fixture (every state part, unknown chunks) with the EQ set.
Bytes saved(const MasterEq& eq) {
  test::fill_persist2(proj(2));
  proj(2).set_master_eq(eq);
  Bytes out;
  CHECK(test::save(proj(2), out) == Progress::Done);
  return out;
}

// The same file written from the description: the chunks of the flat file, and after SNFX (the last known chunk of the fixture, before its unknown chunks) the MEQ chunk.
std::vector<RawChunk> meq_chunks(const Bytes& flat_file, const Bytes& payload = kEqPayload) {
  std::vector<RawChunk> out;
  for (const RawChunk& c : test::split_chunks(flat_file)) {
    out.push_back(c);
    if (c.id == kChunkSnfx) {
      out.push_back({kChunkMeq, payload});
    }
  }
  return out;
}

std::vector<RawChunk> with_payload(const Bytes& payload) { return meq_chunks(saved(MasterEq{}), payload); }

// Loads `file` into arena 1 (so that the document held another EQ before: the load must not inherit it) and answers how it ended.
Progress load1(const Bytes& file, Report* rep = nullptr) {
  proj(1).set_master_eq(MasterEq{{3, 3, 3, 3, 3}, 50});
  return test::load(test::arena(1), file, rep);
}

bool flat(const Project& p) { return master_eq_is_default(p.master_eq()); }

}  // namespace

TEST_CASE("meq: the exact bytes of a project with an EQ, from the description") {
  const Bytes flat_file = saved(MasterEq{});
  const Bytes file = saved(kEq);
  // The chunk: the id with its fourth character a space, a length of 8, the version 1.0, five signed bands, the bass. 16 bytes after SNFX, the new CRC.
  CHECK(file.size() == flat_file.size() + 16u);
  CHECK(file == build(meq_chunks(flat_file)));
  const std::vector<RawChunk> cs = test::split_chunks(file);
  std::size_t at = 0;
  while (at < cs.size() && cs[at].id != kChunkSnfx) {
    ++at;
  }
  NEED(at + 2 < cs.size());
  CHECK(cs[at + 1].id == kChunkMeq);  // right after SNFX
  CHECK(cs[at + 1].payload == kEqPayload);
  CHECK(kChunkMeq == 0x4D455120u);  // 'M' 'E' 'Q' ' '
  CHECK(cs[at + 2].id != kChunkMeq);
  // the 16 bytes in the file
  const Bytes want = {'M', 'E', 'Q', ' ', 0, 0, 0, 8, 0x01, 0x00, 0xF4, 0xFF, 0x00, 0x06, 0x0C, 0x64};
  Bytes got;
  for (std::size_t i = 0, pos = 12; i < cs.size(); ++i) {
    const std::size_t len = cs[i].payload.size();
    if (cs[i].id == kChunkMeq) {
      got.assign(file.begin() + static_cast<std::ptrdiff_t>(pos), file.begin() + static_cast<std::ptrdiff_t>(pos + 8 + len + (len & 1u)));
    }
    pos += 8 + len + (len & 1u);
  }
  CHECK(got == want);
}

TEST_CASE("meq: round trip; a flat EQ writes no chunk and is the file of a project that never set it") {
  const Bytes flat_file = saved(MasterEq{});
  proj(2).reset();
  test::fill_persist2(proj(2));
  Bytes never;
  NEED(test::save(proj(2), never) == Progress::Done);
  CHECK(never == flat_file);
  CHECK(never.size() == 3032u);
  for (const RawChunk& c : test::split_chunks(flat_file)) {
    CHECK(c.id != kChunkMeq);
  }

  const Bytes file = saved(kEq);
  Report rep;
  NEED(load1(file, &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  const MasterEq got = proj(1).master_eq();
  for (int i = 0; i < 5; ++i) {
    CHECK(got.band_db[i] == kEq.band_db[i]);
  }
  CHECK(got.bass == 100);
  CHECK(proj(1).check_invariants());
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == file);

  // a file without the chunk restores the flat EQ whatever the document held
  NEED(load1(flat_file, &rep) == Progress::Done);
  CHECK(flat(proj(1)));
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == flat_file);

  // an EQ set and set back is the flat file again; each value alone is written
  proj(2).set_master_eq(kEq);
  proj(2).set_master_eq(MasterEq{});
  NEED(test::save(proj(2), again) == Progress::Done);
  CHECK(again == flat_file);
  for (int i = 0; i < 6; ++i) {
    MasterEq one{};
    if (i < 5) {
      one.band_db[i] = -5;
    } else {
      one.bass = 1;
    }
    proj(2).set_master_eq(one);
    NEED(test::save(proj(2), again) == Progress::Done);
    CHECK(again.size() == flat_file.size() + 16u);
    NEED(load1(again) == Progress::Done);
    const MasterEq r = proj(1).master_eq();
    for (int b = 0; b < 5; ++b) {
      CHECK(r.band_db[b] == one.band_db[b]);
    }
    CHECK(r.bass == one.bass);
  }

  // the EQ survives a re-save through a small document as well
  test::fill_small(proj(0));
  proj(0).set_master_eq(kEq);
  Bytes small_file;
  NEED(test::save(proj(0), small_file) == Progress::Done);
  CHECK(small_file.size() == 2648u + 16u);
  NEED(test::load(test::arena(1), small_file) == Progress::Done);
  CHECK(proj(1).master_eq().bass == 100);
}

TEST_CASE("meq: the files that never had the chunk keep their bytes and load flat, including after a document with an EQ") {
  // Size and CRC-32 of the fixtures written before this slice (law_format_test.cpp and persist2_format_test.cpp pin the same three).
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
    NEED(load1(file) == Progress::Done);  // arena 1 held an EQ a moment ago
    CHECK(flat(proj(1)));
    Bytes back;
    NEED(test::save(proj(1), back) == Progress::Done);
    CHECK(back == file);
  }
  proj(0).reset();
  Bytes empty;
  NEED(test::save(proj(0), empty) == Progress::Done);
  CHECK(empty.size() == 186u);
  NEED(load1(empty) == Progress::Done);
  CHECK(flat(proj(1)));
}

TEST_CASE("meq: the chunk's shape rules, the clamp and the verdict on each damaged form") {
  const auto verdict = [](const char* what, const std::vector<RawChunk>& cs, Progress want, Error err) {
    INFO(what);
    Report rep;
    const Progress got = load1(build(cs), &rep);
    CHECK(got == want);
    CHECK(rep.error == err);
    if (want == Progress::Done) {
      CHECK(proj(1).check_invariants());
    } else {
      CHECK(flat(proj(1)));  // nothing committed: the project is the empty one the reader leaves behind
      CHECK(test::is_default_empty(proj(1)));
    }
  };

  verdict("the chunk as written", with_payload(kEqPayload), Progress::Done, Error::None);
  verdict("an all-zero chunk is accepted", with_payload({0x01, 0x00, 0, 0, 0, 0, 0, 0}), Progress::Done, Error::None);
  verdict("a newer major", with_payload({0x02, 0x00, 0, 0, 0, 0, 0, 0}), Progress::Failed, Error::NewerMajor);
  verdict("version 0", with_payload({0x00, 0x00, 0, 0, 0, 0, 0, 0}), Progress::Failed, Error::Malformed);
  verdict("length 7", with_payload({0x01, 0x00, 0, 0, 0, 0, 0}), Progress::Failed, Error::Malformed);
  verdict("length 9 for version 1.0", with_payload({0x01, 0x00, 0, 0, 0, 0, 0, 0, 0}), Progress::Failed, Error::Malformed);
  verdict("a newer minor shorter than 1.0", with_payload({0x01, 0x01, 0, 0, 0, 0, 0}), Progress::Failed, Error::Malformed);

  // The clamp: -13 and 13 become -12 and 12, -128 and 127 likewise, the bass 255 becomes 100; the file loads.
  Report rep;
  NEED(load1(build(with_payload({0x01, 0x00, 0x7F, 0x80, 0x0D, 0xF3, 0x01, 0xFF})), &rep) == Progress::Done);
  MasterEq g = proj(1).master_eq();
  CHECK(g.band_db[0] == 12);
  CHECK(g.band_db[1] == -12);
  CHECK(g.band_db[2] == 12);
  CHECK(g.band_db[3] == -12);
  CHECK(g.band_db[4] == 1);
  CHECK(g.bass == 100);
  CHECK(proj(1).check_invariants());

  // A newer minor only appends: read as far as 1.0 goes, the rest skipped, newer_minor set.
  NEED(load1(build(with_payload({0x01, 0x01, 0xF4, 0xFF, 0x00, 0x06, 0x0C, 0x64, 0xAA, 0xBB})), &rep) == Progress::Done);
  CHECK(rep.newer_minor);
  g = proj(1).master_eq();
  CHECK(g.band_db[0] == -12);
  CHECK(g.band_db[4] == 12);
  CHECK(g.bass == 100);

  // Order: the chunk sits after the chunks of a lower rank; a second MEQ, or an MEQ before one of them, is refused.
  const Bytes flat_file = saved(MasterEq{});
  std::vector<RawChunk> twice;
  std::vector<RawChunk> early;
  std::vector<RawChunk> after_unknown;
  for (const RawChunk& c : test::split_chunks(flat_file)) {
    if (c.id == kChunkNote) {
      early.push_back({kChunkMeq, kEqPayload});  // before NOTE
    }
    early.push_back(c);
    twice.push_back(c);
    after_unknown.push_back(c);
    if (c.id == kChunkSnfx) {
      twice.push_back({kChunkMeq, kEqPayload});
      twice.push_back({kChunkMeq, kEqPayload});
    }
  }
  verdict("a second MEQ", twice, Progress::Failed, Error::Malformed);
  verdict("MEQ before NOTE", early, Progress::Failed, Error::Malformed);
  // After the unknown chunks it is still a known chunk of a higher rank than SNFX: legal, as a build older than this one re-saves the file (it writes the unknown chunks, MEQ among them, after the known ones).
  after_unknown.push_back({kChunkMeq, kEqPayload});
  verdict("MEQ after the unknown chunks", after_unknown, Progress::Done, Error::None);
  CHECK(proj(1).master_eq().bass == 100);
}

TEST_CASE("meq: the values are set by the Reader's last step: a load that fails after the chunk leaves the flat EQ") {
  const Bytes file = saved(kEq);
  std::vector<RawChunk> cs = test::split_chunks(file);
  test::BuildOptions bad;
  bad.fix_crc = false;
  const Bytes broken = test::build_file(cs, bad);
  Report rep;
  CHECK(load1(broken, &rep) == Progress::Failed);
  CHECK(rep.error == Error::BadCrc);
  CHECK(flat(proj(1)));
  // a failure a chunk after MEQ as well: a pattern count that disagrees with the header
  std::vector<RawChunk> short_file = cs;
  for (std::size_t i = 0; i < short_file.size(); ++i) {
    if (short_file[i].id == kChunkPatt) {
      short_file.erase(short_file.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
  }
  CHECK(load1(build(short_file), &rep) == Progress::Failed);
  CHECK(flat(proj(1)));
  // a file that is cut inside the CRC chunk
  Bytes cut = file;
  cut.resize(cut.size() - 3);
  CHECK(load1(cut, &rep) == Progress::Failed);
  CHECK(flat(proj(1)));
}

TEST_CASE("meq: one Reader used for two loads in a row cannot carry the first file's EQ into the second") {
  const Bytes with = saved(kEq);
  const Bytes without = saved(MasterEq{});
  const Bytes broken = test::build_file(test::split_chunks(with), [] {
    test::BuildOptions b;
    b.fix_crc = false;
    return b;
  }());

  static Reader rd;  // 3 KB: not on the stack
  const auto run = [&](const Bytes& file, int arena_index) {
    test::MemorySource src(file);
    rd.begin(src, test::arena(arena_index));
    Progress pr = Progress::Running;
    for (std::uint32_t guard = 0; pr == Progress::Running && guard < 100000u; ++guard) {
      pr = rd.step(4096);
    }
    return pr;
  };

  NEED(run(with, 1) == Progress::Done);
  CHECK(proj(1).master_eq().bass == 100);
  NEED(run(without, 1) == Progress::Done);
  CHECK(flat(proj(1)));
  CHECK(run(broken, 1) == Progress::Failed);
  CHECK(flat(proj(1)));
  NEED(run(without, 1) == Progress::Done);
  CHECK(flat(proj(1)));
  CHECK(run(broken, 1) == Progress::Failed);
  NEED(run(with, 1) == Progress::Done);
  CHECK(proj(1).master_eq().band_db[3] == 6);
  // and into a different arena: the first one's EQ stays with its document
  NEED(run(without, 2) == Progress::Done);
  CHECK(flat(proj(2)));
  CHECK(proj(1).master_eq().bass == 100);
}

TEST_CASE("meq: the unknown pool of an older build holds the chunk next to every other chunk and a full OG import") {
  // docs/formats/o3-project.md: a build older than this one keeps MEQ as an unknown chunk (8 payload bytes) with the LAW chunk (3), the state chunks (1,152 in five chunks), the session chunks (348 in four)
  // and the OG chunks OGPM (at most 970) and OGPN (at most 12,822): 15,304 of 16,384 bytes in 13 of 16 chunks (the MEVT take lanes are not counted: an older build that keeps them is a separate bound).
  constexpr std::size_t meq_max = 8;
  constexpr std::size_t law_max = 3;
  constexpr std::size_t session_max = 8 + 19 + 295 + 27;
  constexpr std::size_t state_max = 20 + 27 + 25 + 99 + 981;
  CHECK(meq_max + law_max + session_max + state_max + 970u + 12822u == 15304u);
  CHECK(meq_max + law_max + session_max + state_max + 970u + 12822u <= kUnknownPoolBytes);
  CHECK(1u + 1u + 4u + 5u + 2u == 13u);
  CHECK(1u + 1u + 4u + 5u + 2u <= kMaxUnknownChunks);
  for (const RawChunk& c : test::split_chunks(saved(kEq))) {
    if (c.id == kChunkMeq) {
      CHECK(c.payload.size() == meq_max);
    }
  }
}

TEST_CASE("meq: what a build older than this one writes back of a file with an EQ (it keeps MEQ as an unknown chunk)") {
  // The build before this slice knows no MEQ: it reads the chunk as an unknown one, in the order of the file, and writes the unknown chunks after the known ones in that order. The same rule is held
  // here without the old build: in the file this writer produces, MEQ is the last known chunk, so it is the first unknown chunk of the older reader, and the older writer puts it back in the same place,
  // ahead of the chunks that were unknown already, in their order. tools/o3project_roundtrip.py runs the emulation of that build byte for byte.
  const Bytes file = saved(kEq);
  const std::vector<RawChunk> cs = test::split_chunks(file);
  std::size_t meq_at = cs.size();
  for (std::size_t i = 0; i < cs.size(); ++i) {
    if (cs[i].id == kChunkMeq) {
      meq_at = i;
    }
  }
  NEED(meq_at < cs.size());
  NEED(meq_at > 0);
  CHECK(cs[meq_at - 1].id == kChunkSnfx);
  // nothing after MEQ is a chunk the older build knows, except the CRC chunk that ends the file
  const std::uint32_t known_ids[] = {kChunkPhdr, kChunkTrak, kChunkModr, kChunkSong, kChunkPatt, kChunkInst, kChunkSets, kChunkLrnb, kChunkMidi, kChunkUsbs,
                                     kChunkMixr, kChunkPerf, kChunkAppm, kChunkNote, kChunkMute, kChunkScen, kChunkSnfx, kChunkLaw, kChunkMevt};
  for (std::size_t i = meq_at + 1; i < cs.size(); ++i) {
    if (cs[i].id == kChunkCrc) {
      continue;
    }
    for (const std::uint32_t id : known_ids) {
      CHECK(cs[i].id != id);
    }
  }
  CHECK(meq_at + 1 < cs.size());
}
