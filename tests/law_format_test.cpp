// SPDX-License-Identifier: GPL-3.0-or-later
// OS-BUG-PASTE24 slice S1 (docs/specs/OS-BUG-PASTE24.md section 5 "D3", section 11): the instrument volume law as data of the native project, the LAW chunk. This file is the format side: the exact
// bytes (written here from the description, not by the writer), the round trip of both laws, the files that never had the chunk (they keep their bytes and load as the 1.2+ law, whatever the
// document held before), the chunk's shape rules and the verdict on each damaged form, the law set by the Reader's last step, after the CRC and the model checks (a load that fails before it leaves the 0 that reset() gave), the held value of a Reader that is
// used twice, the unknown pool of an older build that keeps the chunk, and the bytes an older build writes back. The use of the law (the sites that create an instrument, the OG v6 round trip) is
// og_volume_law_test.cpp.
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

// The persist2 fixture (every state part, unknown chunks) with the law set.
Bytes saved(std::uint8_t law) {
  test::fill_persist2(proj(2));
  proj(2).set_volume_law(law);
  Bytes out;
  CHECK(test::save(proj(2), out) == Progress::Done);
  return out;
}

// The same file written from the description: the chunks of the law-0 file, and after SNFX (before the unknown chunks) the LAW chunk.
std::vector<RawChunk> law_chunks(const Bytes& law0_file, const Bytes& law_payload = {0x01, 0x00, 0x01}) {
  std::vector<RawChunk> out;
  for (const RawChunk& c : test::split_chunks(law0_file)) {
    out.push_back(c);
    if (c.id == kChunkSnfx) {
      out.push_back({kChunkLaw, law_payload});
    }
  }
  return out;
}

// Replaces the LAW chunk's payload (the chunk sits after SNFX).
std::vector<RawChunk> with_payload(const Bytes& payload) { return law_chunks(saved(0), payload); }

// Loads `file` into arena 1 (so that the document held another law before: the load must not inherit it) and answers how it ended.
Progress load1(const Bytes& file, Report* rep = nullptr) {
  proj(1).set_volume_law(1);
  return test::load(test::arena(1), file, rep);
}

}  // namespace

TEST_CASE("law: the exact bytes of a project whose law is 1, from the description") {
  const Bytes law0 = saved(0);
  const Bytes file = saved(1);
  // The chunk: the id with its fourth character a space, a length of 3, the version 1.0, the law, and the pad byte. 12 bytes after SNFX, the new CRC.
  CHECK(file.size() == law0.size() + 12u);
  CHECK(file == build(law_chunks(law0)));
  const std::vector<RawChunk> cs = test::split_chunks(file);
  std::size_t at = 0;
  while (at < cs.size() && cs[at].id != kChunkSnfx) {
    ++at;
  }
  NEED(at + 1 < cs.size());
  CHECK(cs[at + 1].id == kChunkLaw);  // right after SNFX
  CHECK(cs[at + 1].payload == Bytes({0x01, 0x00, 0x01}));
  CHECK(kChunkLaw == 0x4C415720u);  // 'L' 'A' 'W' ' '
  CHECK(cs[at + 2].id != kChunkLaw);
  // the 12 bytes in the file
  Bytes want = {'L', 'A', 'W', ' ', 0, 0, 0, 3, 0x01, 0x00, 0x01, 0x00};
  Bytes got;
  for (std::size_t i = 0, pos = 12; i < cs.size(); ++i) {
    const std::size_t len = cs[i].payload.size();
    if (cs[i].id == kChunkLaw) {
      got.assign(file.begin() + static_cast<std::ptrdiff_t>(pos), file.begin() + static_cast<std::ptrdiff_t>(pos + 8 + len + (len & 1u)));
    }
    pos += 8 + len + (len & 1u);
  }
  CHECK(got == want);
  // pinned (taken from this writer once the bytes above had been checked): the law-1 persist2 fixture
  CHECK(file.size() == 3044u);
  CHECK(formats::crc32(file.data(), file.size()) == 0x2ce18274u);
}

TEST_CASE("law: both laws round trip; law 0 writes no chunk and is the file of a project that never set the law") {
  const Bytes law0 = saved(0);
  proj(2).reset();
  test::fill_persist2(proj(2));
  Bytes never;
  NEED(test::save(proj(2), never) == Progress::Done);
  CHECK(never == law0);  // never set
  CHECK(never.size() == 3032u);

  // law 1: the chunk is written and read back
  const Bytes law1 = saved(1);
  Report rep;
  NEED(load1(law1, &rep) == Progress::Done);
  CHECK(proj(1).volume_law() == 1);
  CHECK(proj(1).check_invariants());
  CHECK_FALSE(rep.newer_minor);
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == law1);

  // law 0: no chunk, and the reader restores 0 whatever the document held
  for (const RawChunk& c : test::split_chunks(law0)) {
    CHECK(c.id != kChunkLaw);
  }
  NEED(load1(law0, &rep) == Progress::Done);
  CHECK(proj(1).volume_law() == 0);
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == law0);

  // a project that was law 1 and is set back to 0 is the same file again
  proj(2).set_volume_law(1);
  proj(2).set_volume_law(0);
  NEED(test::save(proj(2), again) == Progress::Done);
  CHECK(again == law0);

  // the law survives a re-save through a document that is law 1 (the unknown chunks and the state do not move it)
  test::fill_small(proj(0));
  proj(0).set_volume_law(1);
  Bytes small1;
  NEED(test::save(proj(0), small1) == Progress::Done);
  NEED(test::load(test::arena(1), small1) == Progress::Done);
  CHECK(proj(1).volume_law() == 1);
  CHECK(small1.size() == 2648u + 12u);
}

TEST_CASE("law: the files that never had the chunk keep their bytes and load as law 0, including after a law-1 document") {
  // Size and CRC-32 of the fixtures written before this slice (taken from the writer of the commit before S1; persist2_format_test.cpp pins the same three, tools/o3project_roundtrip.py the others).
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
    NEED(load1(file) == Progress::Done);  // arena 1 held law 1 a moment ago
    CHECK(proj(1).volume_law() == 0);
    Bytes back;
    NEED(test::save(proj(1), back) == Progress::Done);
    CHECK(back == file);
  }
  proj(0).reset();
  Bytes empty;
  NEED(test::save(proj(0), empty) == Progress::Done);
  CHECK(empty.size() == 186u);
  NEED(load1(empty) == Progress::Done);
  CHECK(proj(1).volume_law() == 0);
}

TEST_CASE("law: the chunk's shape rules and the verdict on each damaged form") {
  const auto verdict = [](const char* what, const std::vector<RawChunk>& cs, Progress want, Error err, int law) {
    INFO(what);
    Report rep;
    const Progress got = load1(build(cs), &rep);
    CHECK(got == want);
    CHECK(rep.error == err);
    if (want == Progress::Done) {
      CHECK(proj(1).volume_law() == law);
      CHECK(proj(1).check_invariants());
    } else {
      CHECK(proj(1).volume_law() == 0);  // nothing committed: the project is the empty one the reader leaves behind
      CHECK(test::is_default_empty(proj(1)));
    }
  };
  const auto payload = [](const Bytes& p) { return with_payload(p); };

  verdict("law 1 (the chunk as written)", payload({0x01, 0x00, 0x01}), Progress::Done, Error::None, 1);
  verdict("law 0 present is accepted", payload({0x01, 0x00, 0x00}), Progress::Done, Error::None, 0);
  verdict("law 2", payload({0x01, 0x00, 0x02}), Progress::Failed, Error::Malformed, 0);
  verdict("law 255", payload({0x01, 0x00, 0xFF}), Progress::Failed, Error::Malformed, 0);
  verdict("length 2 (no law byte)", payload({0x01, 0x00}), Progress::Failed, Error::Malformed, 0);
  verdict("length 4 for version 1.0", payload({0x01, 0x00, 0x01, 0x00}), Progress::Failed, Error::Malformed, 0);
  verdict("version 0", payload({0x00, 0x00, 0x01}), Progress::Failed, Error::Malformed, 0);
  verdict("a newer major", payload({0x02, 0x00, 0x01}), Progress::Failed, Error::NewerMajor, 0);

  // A newer minor only appends: read as far as 1.0 goes, the rest skipped, newer_minor set.
  Report rep;
  NEED(load1(build(payload({0x01, 0x01, 0x01, 0xAA, 0xBB})), &rep) == Progress::Done);
  CHECK(rep.newer_minor);
  CHECK(proj(1).volume_law() == 1);
  // ... and the byte's meaning is fixed by 1.0: an unknown law is Malformed in a newer minor as well
  verdict("law 2 in a newer minor with extra bytes", payload({0x01, 0x01, 0x02, 0xAA, 0xBB}), Progress::Failed, Error::Malformed, 0);
  verdict("a newer minor shorter than 1.0", payload({0x01, 0x01}), Progress::Failed, Error::Malformed, 0);

  // Order: the chunk is the last known one; a second LAW, or a LAW before a chunk of a lower rank, is refused.
  const Bytes law0 = saved(0);
  std::vector<RawChunk> twice;
  std::vector<RawChunk> early;
  std::vector<RawChunk> after_unknown;
  for (const RawChunk& c : test::split_chunks(law0)) {
    if (c.id == kChunkNote) {
      early.push_back({kChunkLaw, {0x01, 0x00, 0x01}});  // before NOTE
    }
    early.push_back(c);
    twice.push_back(c);
    if (c.id == kChunkSnfx) {
      twice.push_back({kChunkLaw, {0x01, 0x00, 0x01}});
      twice.push_back({kChunkLaw, {0x01, 0x00, 0x01}});
    }
  }
  verdict("a second LAW", twice, Progress::Failed, Error::Malformed, 0);
  verdict("LAW before NOTE", early, Progress::Failed, Error::Malformed, 0);
  // After an unknown chunk it is still a known chunk of a higher rank than SNFX: legal, as a build older than this one re-saves the file (it writes the unknown chunks, LAW among them, after the known ones).
  for (const RawChunk& c : test::split_chunks(law0)) {
    after_unknown.push_back(c);
  }
  after_unknown.push_back({kChunkLaw, {0x01, 0x00, 0x01}});  // behind the unknown chunks of fill_persist2
  verdict("LAW after the unknown chunks", after_unknown, Progress::Done, Error::None, 1);
}

TEST_CASE("law: the law is set by the Reader's last step: a load that fails after the chunk leaves the reset value") {
  const Bytes law1 = saved(1);
  // the CRC is off by one bit: the load fails at the last chunk, after LAW
  std::vector<RawChunk> cs = test::split_chunks(law1);
  test::BuildOptions bad;
  bad.fix_crc = false;
  const Bytes broken = test::build_file(cs, bad);
  Report rep;
  CHECK(load1(broken, &rep) == Progress::Failed);
  CHECK(rep.error == Error::BadCrc);
  CHECK(proj(1).volume_law() == 0);
  // a failure a chunk after LAW as well: a pattern count that disagrees with the header
  proj(1).set_volume_law(1);
  std::vector<RawChunk> short_file = cs;
  for (std::size_t i = 0; i < short_file.size(); ++i) {
    if (short_file[i].id == kChunkPatt) {
      short_file.erase(short_file.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
  }
  CHECK(load1(build(short_file), &rep) == Progress::Failed);
  CHECK(proj(1).volume_law() == 0);
  // a file that is cut inside the CRC chunk
  Bytes cut = law1;
  cut.resize(cut.size() - 3);
  CHECK(load1(cut, &rep) == Progress::Failed);
  CHECK(proj(1).volume_law() == 0);
}

TEST_CASE("law: one Reader used for two loads in a row cannot carry the first file's law into the second") {
  const Bytes law1 = saved(1);
  const Bytes law0 = saved(0);
  std::vector<RawChunk> cs = test::split_chunks(law1);
  test::BuildOptions bad;
  bad.fix_crc = false;
  const Bytes broken = test::build_file(cs, bad);

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

  // a law-1 file, then a file without the chunk, through the same Reader into the same arena
  NEED(run(law1, 1) == Progress::Done);
  CHECK(proj(1).volume_law() == 1);
  NEED(run(law0, 1) == Progress::Done);
  CHECK(proj(1).volume_law() == 0);
  // a law-1 file that fails at the CRC, then a clean file without the chunk
  CHECK(run(broken, 1) == Progress::Failed);
  CHECK(proj(1).volume_law() == 0);
  NEED(run(law0, 1) == Progress::Done);
  CHECK(proj(1).volume_law() == 0);
  // a law-1 file that fails at the CRC, then a law-1 file: the second one's law is its own
  CHECK(run(broken, 1) == Progress::Failed);
  NEED(run(law1, 1) == Progress::Done);
  CHECK(proj(1).volume_law() == 1);
  // and into a different arena: the first one's law stays with its document
  NEED(run(law0, 2) == Progress::Done);
  CHECK(proj(2).volume_law() == 0);
  CHECK(proj(1).volume_law() == 1);
}

TEST_CASE("law: the unknown pool of an older build holds the chunk next to every other chunk and a full OG import") {
  // docs/formats/o3-project.md section 10: a build older than this one keeps LAW as an unknown chunk (3 payload bytes) with the state chunks (1,152 in five chunks), the session chunks (348 in four)
  // and the OG chunks OGPM (at most 970) and OGPN (at most 12,822): 15,296 of 16,384 bytes in 12 of 16 chunks (OS-SCALES28: the NOTE 1.1 chunk is 8 bytes).
  constexpr std::size_t law_max = 3;
  constexpr std::size_t session_max = 8 + 19 + 295 + 27;
  constexpr std::size_t state_max = 20 + 27 + 25 + 99 + 981;
  CHECK(law_max + session_max + state_max + 970u + 12822u == 15296u);
  CHECK(law_max + session_max + state_max + 970u + 12822u <= kUnknownPoolBytes);
  CHECK(1u + 4u + 5u + 2u == 12u);
  CHECK(1u + 4u + 5u + 2u <= kMaxUnknownChunks);
  // the writer's chunk is exactly that size
  for (const RawChunk& c : test::split_chunks(saved(1))) {
    if (c.id == kChunkLaw) {
      CHECK(c.payload.size() == law_max);
    }
  }
}

TEST_CASE("law: what a build older than this one writes back of a law-1 file (it keeps LAW as an unknown chunk)") {
  // The build before S1 knows no LAW: it reads the chunk as an unknown one, in the order of the file, and writes the unknown chunks after the known ones in that order. The check against that build itself
  // was run once (the commit body of the S1 commit records the commit and the result: `resave` and `rewrite` of the law-1 `law` fixture of o3pj_tool and of o3project.py were byte for byte the input).
  // Here the same rule is held without the old build: in the file this writer produces, LAW is the last known chunk, so it is the first unknown chunk of the older reader, and the older writer puts it
  // back in the same place, ahead of the chunks that were unknown already, in their order. So the older build's output equals the input iff LAW sits right after the last known chunk.
  const Bytes file = saved(1);
  const std::vector<RawChunk> cs = test::split_chunks(file);
  NEED(cs.size() >= 3);
  std::size_t law_at = cs.size();
  for (std::size_t i = 0; i < cs.size(); ++i) {
    if (cs[i].id == kChunkLaw) {
      law_at = i;
    }
  }
  NEED(law_at < cs.size());
  // everything before LAW is a chunk the older build knows, and nothing after it is one: LAW is the last known chunk, the first of the chunks unknown to the older build
  CHECK(cs[law_at - 1].id == kChunkSnfx);
  const std::uint32_t known_ids[] = {kChunkPhdr, kChunkTrak, kChunkModr, kChunkSong, kChunkPatt, kChunkInst, kChunkSets, kChunkLrnb, kChunkMidi, kChunkUsbs,
                                     kChunkMixr, kChunkPerf, kChunkAppm, kChunkNote, kChunkMute, kChunkScen, kChunkSnfx, kChunkCrc};
  for (std::size_t i = law_at + 1; i < cs.size(); ++i) {
    for (const std::uint32_t id : known_ids) {
      CHECK(cs[i].id != id);
    }
  }
  CHECK(law_at + 1 < cs.size());  // the fixture has unknown chunks of its own behind it
}
