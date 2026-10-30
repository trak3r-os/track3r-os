// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/iff.h"
#include "support/memory_io.h"
#include "support/project_fixture.h"

#include <o3os/formats/crc32.h>
#include <o3os/formats/native.h>
#include <o3os/model/project.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using namespace o3os::model;
using o3os::formats::Crc32;
using o3os::formats::crc32;
using o3os::test::BuildOptions;
using o3os::test::MemorySource;
using o3os::test::RawChunk;
using o3os::test::VectorSink;
using Bytes = std::vector<std::uint8_t>;

namespace {

Project& proj(int i = 0) { return test::arena(i).project; }

Bytes small_file() {
  test::fill_small(proj(2));
  Bytes out;
  CHECK(test::save(proj(2), out) == Progress::Done);
  return out;
}

RawChunk& chunk_of(std::vector<RawChunk>& cs, std::uint32_t id, int nth = 0) {
  for (RawChunk& c : cs) {
    if (c.id == id && nth-- == 0) {
      return c;
    }
  }
  CHECK_MESSAGE(false, "chunk not found");
  return cs[0];
}

Bytes build(const std::vector<RawChunk>& cs, const BuildOptions& o = {}) { return test::build_file(cs, o); }

// A hypothetical INST 1.6 (INST 1.4 is OS-LFOSINE's, with no field of its own; INST 1.5 is OS-STEREO's stereo byte) made of an INST 1.1 payload: the engine byte (automatic), the twelve bytes of INST 1.3 at their defaults (OS-INSTUI S8b)
// and the stereo byte of 1.5 (mono), then `tail` bytes of a field of a later version.
void make_inst_next_minor(RawChunk& c, std::size_t tail, std::uint8_t fill = 0xAB, std::uint8_t engine = 0) {
  c.payload[1] = 6;
  c.payload.push_back(engine);
  const std::uint8_t twelve[12] = {127, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 100};
  c.payload.insert(c.payload.end(), twelve, twelve + 12);
  c.payload.push_back(0);
  c.payload.insert(c.payload.end(), tail, fill);
}

void put32_at(Bytes& b, std::size_t at, std::uint32_t v) {
  b[at] = static_cast<std::uint8_t>(v >> 24);
  b[at + 1] = static_cast<std::uint8_t>(v >> 16);
  b[at + 2] = static_cast<std::uint8_t>(v >> 8);
  b[at + 3] = static_cast<std::uint8_t>(v);
}

// Start of the sampler block inside an INST payload.
std::size_t sampler_at(const Bytes& payload) {
  const std::size_t name = payload[6];
  const std::size_t path = payload[7 + name];
  return 6 + 1 + name + 1 + path;
}

bool contains(const char* text, const char* part) { return std::strstr(text, part) != nullptr; }

// The default project as a file, written down byte by byte (it is also the worked example of the format in
// docs/formats/o3-project.md, and Python's build() produces the same 186 bytes).
const std::uint8_t kDefaultFile[186] = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0xb2, 0x4f, 0x33, 0x50, 0x4a, 0x50, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x13, 0x01, 0x00, 0x0c, 0x20, 0x00, 0x00, 0x32, 0xc8, 0x32, 0x56, 0x56, 0x56,
    0x56, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x52, 0x41, 0x4b, 0x00, 0x00, 0x00, 0x63,
    0x01, 0x00, 0x10, 0x00, 0x00, 0x56, 0x32, 0x00, 0x00, 0x00, 0x00, 0x56, 0x32, 0x00, 0x00, 0x00,
    0x00, 0x56, 0x32, 0x00, 0x00, 0x00, 0x00, 0x56, 0x32, 0x00, 0x00, 0x00, 0x00, 0x56, 0x32, 0x00,
    0x00, 0x00, 0x00, 0x56, 0x32, 0x00, 0x00, 0x00, 0x00, 0x56, 0x32, 0x00, 0x00, 0x00, 0x00, 0x56,
    0x32, 0x00, 0x00, 0x00, 0x00, 0x56, 0x32, 0x00, 0x00, 0x00, 0x00, 0x56, 0x32, 0x00, 0x00, 0x00,
    0x00, 0x56, 0x32, 0x00, 0x00, 0x00, 0x00, 0x56, 0x32, 0x00, 0x00, 0x00, 0x00, 0x56, 0x32, 0x00,
    0x00, 0x00, 0x00, 0x56, 0x32, 0x00, 0x00, 0x00, 0x00, 0x56, 0x32, 0x00, 0x00, 0x00, 0x00, 0x56,
    0x32, 0x00, 0x00, 0x00, 0x4d, 0x4f, 0x44, 0x52, 0x00, 0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00,
    0x53, 0x4f, 0x4e, 0x47, 0x00, 0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20,
    0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0x3b, 0x09, 0xf0, 0xae,
};

}  // namespace

TEST_CASE("crc32: the standard check values and incremental use") {
  const char* check = "123456789";
  CHECK(crc32(reinterpret_cast<const std::uint8_t*>(check), 9) == 0xCBF43926u);
  CHECK(crc32(nullptr, 0) == 0u);
  const std::uint8_t zeros[4] = {0, 0, 0, 0};
  CHECK(crc32(zeros, 4) == 0x2144DF1Cu);
  Bytes data(1000);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::uint8_t>(i * 31 + 7);
  }
  Crc32 inc;
  inc.update(data.data(), 1);
  inc.update(data.data() + 1, 333);
  inc.update(data.data() + 334, 666);
  CHECK(inc.value() == crc32(data.data(), data.size()));
  inc.reset();
  CHECK(inc.value() == 0u);
}

TEST_CASE("native: the default project round-trips and the file has the documented shape") {
  Project& a = proj(0);
  a.reset();
  Bytes first;
  NEED(test::save(a, first) == Progress::Done);
  // FORM, length, O3PJ, then PHDR first and the CRC chunk last; every chunk starts on an even offset
  NEED(first.size() > 40);
  CHECK(std::memcmp(first.data(), "FORM", 4) == 0);
  CHECK(test::be32(&first[4]) == first.size() - 8);
  CHECK(std::memcmp(first.data() + 8, "O3PJ", 4) == 0);
  CHECK(std::memcmp(first.data() + 12, "PHDR", 4) == 0);
  CHECK(std::memcmp(first.data() + first.size() - 14, "CRC ", 4) == 0);
  CHECK(test::be32(&first[first.size() - 10]) == 6);
  CHECK(first.size() % 2 == 0);
  CHECK(test::be32(&first[first.size() - 4]) == crc32(first.data(), first.size() - 14));
  std::size_t pos = 12;
  while (pos < first.size()) {
    CHECK(pos % 2 == 0);
    const std::uint32_t len = test::be32(&first[pos + 4]);
    pos += 8 + len + (len & 1u);
  }
  CHECK(pos == first.size());

  // the layout, byte by byte
  CHECK(first.size() == sizeof kDefaultFile);
  CHECK(std::memcmp(first.data(), kDefaultFile, std::min(first.size(), sizeof kDefaultFile)) == 0);

  Report rep;
  NEED(test::load(test::arena(1), first, &rep) == Progress::Done);
  CHECK(rep.error == Error::None);
  CHECK(rep.bytes == first.size());
  CHECK(test::is_default_empty(proj(1)));
  Bytes second;
  NEED(test::save(proj(1), second) == Progress::Done);
  CHECK(second == first);
}

TEST_CASE("native: a small project round-trips byte for byte and keeps every field") {
  Project& a = proj(0);
  test::fill_small(a);
  Bytes first;
  NEED(test::save(a, first) == Progress::Done);

  Report rep;
  NEED(test::load(test::arena(1), first, &rep) == Progress::Done);
  CHECK(rep.patterns == 3);
  CHECK(rep.instruments == 3);
  CHECK_FALSE(rep.newer_minor);
  Project& b = proj(1);
  CHECK(b.check_invariants());
  CHECK(std::strcmp(b.name(), "Small Song") == 0);
  CHECK(b.tempo_x100() == 12850);
  CHECK(b.swing() == 54);
  CHECK(b.default_pattern_length() == 16);
  CHECK(b.master().volume == 90);
  CHECK(b.master().flags == (kMasterLimiter | kMasterDelayMute));
  CHECK(std::strcmp(b.track(0)->name, "Kick") == 0);
  CHECK(b.track(1)->type == TrackType::Midi);
  CHECK(b.track(2)->flags == kTrackMute);
  CHECK(b.track(3)->bus == 2);
  CHECK(b.track(8)->type == TrackType::ExternalAudio);
  CHECK(std::strcmp(b.track(8)->name, "Line in") == 0);
  CHECK(b.module_count() == 2);
  CHECK(std::strcmp(b.module(1)->name, "fmsynth") == 0);
  CHECK(b.module(1)->abi_minor == 2);
  CHECK(b.song_length() == 5);
  CHECK(b.order(3) == 7);
  CHECK(b.pattern_count() == 3);
  NEED(b.pattern(1) != nullptr);
  CHECK(std::strcmp(b.pattern(1)->name, "Intro") == 0);
  CHECK(b.pattern(1)->track_length[0] == 16);  // tracks keep their own lengths
  CHECK(b.pattern(1)->track_length[3] == 12);
  CHECK(b.pattern(1)->track_length[15] == 128);
  CHECK(b.pattern(7)->track_length[0] == 5);
  CHECK(b.pattern(7)->track_length[1] == 3);
  CHECK(b.pattern_length(1) == 16);  // longest of the 12 active tracks
  for (std::uint32_t id : {1u, 2u, 7u}) {
    for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
      CHECK(a.pattern(id)->track_length[t] == b.pattern(id)->track_length[t]);
    }
  }
  CHECK(b.pattern(1)->steps[15][127].note == kNoteFade);  // past the length, kept
  CHECK(b.pattern(1)->steps[3][15].fx[0].type == 128);
  CHECK(b.pattern(1)->steps[0][4].note == kNoteCut);
  for (std::uint32_t id : {1u, 2u, 7u}) {
    CHECK(std::memcmp(a.pattern(id)->steps, b.pattern(id)->steps, sizeof a.pattern(id)->steps) == 0);
  }
  NEED(b.instrument(0) != nullptr);
  CHECK(std::strcmp(b.instrument(0)->sample_path, "samples/kick.wav") == 0);
  CHECK(b.instrument(0)->sampler.envelope[0].amount == 1.5f);
  CHECK(b.instrument(0)->sampler.tune == -3);
  CHECK(b.instrument(0)->sampler.slices[2] == 43690);
  NEED(b.instrument(48) != nullptr);
  CHECK(b.instrument(48)->midi_channel == 3);
  NEED(b.instrument(50) != nullptr);
  CHECK(b.instrument(50)->param_len == 5);
  CHECK(b.instrument(50)->params[4] == 0xA4);
  CHECK(b.instrument(1) == nullptr);
  CHECK(b.unknown_count() == 2);

  Bytes second;
  NEED(test::save(b, second) == Progress::Done);
  CHECK(second == first);  // byte exact
}

TEST_CASE("native: a maximal project round-trips byte for byte") {
  Project& a = proj(0);
  test::fill_maximal(a);
  Bytes first;
  NEED(test::save(a, first) == Progress::Done);
  MESSAGE("maximal file: " << first.size() << " bytes, arena " << kModelArenaBytes << " bytes");
  CHECK(first.size() < kMaxFileBytes);
  CHECK(first.size() > 1800000);  // 128 full patterns alone are 1.8 MB

  Report rep;
  NEED(test::load(test::arena(1), first, &rep) == Progress::Done);
  CHECK(rep.patterns == kMaxPatterns);
  CHECK(rep.instruments == kMaxInstruments);
  CHECK(proj(1).unknown_count() == kMaxUnknownChunks);
  CHECK(proj(1).unknown_dropped_chunks() == 0);
  CHECK(proj(1).check_invariants());
  Bytes second;
  NEED(test::save(proj(1), second) == Progress::Done);
  CHECK(second.size() == first.size());
  CHECK(second == first);
  for (std::uint32_t id = 1; id <= kMaxPatternId; ++id) {
    const Pattern* pa = a.pattern(id);
    const Pattern* pb = proj(1).pattern(id);
    NEED((pa == nullptr) == (pb == nullptr));
    if (pa != nullptr) {
      NEED(std::memcmp(pa->steps, pb->steps, sizeof pa->steps) == 0);
      NEED(std::memcmp(pa->track_length, pb->track_length, sizeof pa->track_length) == 0);
    }
  }
}

TEST_CASE("native: saving does not depend on the step budget, and the size is known once measured") {
  test::fill_small(proj(0));
  Bytes reference;
  NEED(test::save(proj(0), reference) == Progress::Done);
  for (std::size_t budget : {1u, 2u, 7u, 100u, 1023u, 1024u, 1025u, 100000u}) {
    CAPTURE(budget);
    Bytes out;
    NEED(test::save(proj(0), out, budget) == Progress::Done);
    CHECK(out == reference);
  }
  VectorSink sink;
  Writer w;
  w.begin(proj(0), sink);
  CHECK_FALSE(w.sized());  // begin() does not touch the project
  CHECK(w.total_bytes() == 0);
  while (w.step() == Progress::Running) {
  }
  CHECK(w.sized());
  CHECK(w.total_bytes() == reference.size());
  CHECK(sink.bytes.size() == w.total_bytes());
  CHECK(w.report().bytes == reference.size());
  CHECK(w.progress() == Progress::Done);
  CHECK(w.step() == Progress::Done);  // stays done
  CHECK(sink.bytes.size() == reference.size());
}

TEST_CASE("native: loading does not depend on how the source hands out bytes") {
  const Bytes file = small_file();
  for (std::size_t budget : {1u, 5u, 64u, 4096u}) {
    for (std::size_t per_read : {1u, 3u, 1000u}) {
      for (std::uint32_t block : {0u, 2u, 5u}) {
        CAPTURE(budget);
        CAPTURE(per_read);
        CAPTURE(block);
        MemorySource src(file);
        src.max_per_read = per_read;
        src.block_every = block;
        Reader rd;
        rd.begin(src, test::arena(0));
        Progress pr = Progress::Running;
        std::uint32_t steps = 0;
        while (pr == Progress::Running && steps < 10000000u) {
          pr = rd.step(budget);
          ++steps;
        }
        NEED(pr == Progress::Done);
        CHECK(rd.report().bytes == file.size());
        Bytes again;
        NEED(test::save(proj(0), again) == Progress::Done);
        CHECK(again == file);
      }
    }
  }
}

TEST_CASE("native: step with no budget does nothing and a budget bounds the bytes read per step") {
  const Bytes file = small_file();
  MemorySource src(file);
  Reader rd;
  rd.begin(src, test::arena(0));
  CHECK(rd.step(0) == Progress::Running);
  CHECK(src.position() == 0);
  CHECK(rd.step(10) == Progress::Running);
  CHECK(src.position() <= 10);
  std::size_t last = src.position();
  Progress pr = Progress::Running;
  while (pr == Progress::Running) {
    pr = rd.step(100);
    CHECK(src.position() - last <= 100);
    last = src.position();
  }
  CHECK(pr == Progress::Done);
}

TEST_CASE("native: unknown chunks are kept and written back, even from odd places") {
  test::fill_small(proj(0));
  Bytes first;
  NEED(test::save(proj(0), first) == Progress::Done);
  NEED(test::load(test::arena(1), first) == Progress::Done);
  NEED(proj(1).unknown_count() == 2);
  CHECK(proj(1).unknown(0)->id == fourcc('X', 'T', 'R', 'A'));
  CHECK(proj(1).unknown(0)->length == 5);
  CHECK(std::memcmp(proj(1).unknown_data(0), "hello", 5) == 0);
  CHECK(proj(1).unknown(1)->id == fourcc('E', 'M', 'P', 'T'));
  CHECK(proj(1).unknown(1)->length == 0);

  // A foreign chunk between known ones (and one before the song) is kept and comes back after the known
  // chunks, in the order it was met.
  std::vector<RawChunk> cs = test::split_chunks(first);
  const Bytes blob = {9, 8, 7, 6, 5, 4, 3};
  cs.insert(cs.begin() + 4, RawChunk{fourcc('Z', 'Z', 'Z', 'Z'), blob});
  cs.insert(cs.begin() + 6, RawChunk{fourcc('a', 'b', 'c', 'd'), Bytes{1}});
  NEED(test::load(test::arena(1), build(cs)) == Progress::Done);
  CHECK(proj(1).unknown_count() == 4);
  CHECK(proj(1).unknown(0)->id == fourcc('Z', 'Z', 'Z', 'Z'));
  CHECK(proj(1).unknown(1)->id == fourcc('a', 'b', 'c', 'd'));
  CHECK(proj(1).unknown(2)->id == fourcc('X', 'T', 'R', 'A'));
  Bytes out;
  NEED(test::save(proj(1), out) == Progress::Done);
  std::vector<RawChunk> back = test::split_chunks(out);
  NEED(back.size() == cs.size());
  CHECK(back[back.size() - 4].id == fourcc('Z', 'Z', 'Z', 'Z'));
  CHECK(back[back.size() - 4].payload == blob);
  CHECK(back[back.size() - 3].id == fourcc('a', 'b', 'c', 'd'));
  CHECK(back[back.size() - 3].payload == Bytes{1});
  NEED(test::load(test::arena(2), out) == Progress::Done);
  Bytes out2;
  NEED(test::save(proj(2), out2) == Progress::Done);
  CHECK(out2 == out);  // stable after the first save
}

TEST_CASE("native: unknown chunks beyond the pool are dropped and counted, not fatal") {
  std::vector<RawChunk> cs = test::split_chunks(small_file());
  cs.resize(cs.size() - 2);  // without the fixture's two unknown chunks
  // one chunk bigger than the whole pool, then three small ones that still fit
  cs.push_back(RawChunk{fourcc('B', 'I', 'G', '1'), Bytes(kUnknownPoolBytes + 1, 0xEE)});
  cs.push_back(RawChunk{fourcc('S', 'M', 'L', '1'), Bytes(100, 1)});
  cs.push_back(RawChunk{fourcc('S', 'M', 'L', '2'), Bytes(101, 2)});
  cs.push_back(RawChunk{fourcc('B', 'I', 'G', '2'), Bytes(kUnknownPoolBytes, 0xDD)});  // does not fit any more
  Report rep;
  NEED(test::load(test::arena(0), build(cs), &rep) == Progress::Done);
  CHECK(rep.error == Error::None);
  CHECK(rep.unknown_kept == 2);
  CHECK(rep.unknown_dropped == 2);
  CHECK(proj(0).unknown_count() == 2);
  CHECK(proj(0).unknown_dropped_chunks() == 2);
  CHECK(proj(0).unknown_dropped_bytes() == kUnknownPoolBytes + 1 + kUnknownPoolBytes);
  CHECK(proj(0).unknown(0)->id == fourcc('S', 'M', 'L', '1'));
  CHECK(proj(0).unknown_data(1)[100] == 2);
  Bytes out;
  NEED(test::save(proj(0), out) == Progress::Done);
  std::vector<RawChunk> back = test::split_chunks(out);
  CHECK(back.size() == cs.size() - 2);

  // more chunks than the table holds
  cs.resize(cs.size() - 4);
  for (std::uint32_t i = 0; i < kMaxUnknownChunks + 3; ++i) {
    cs.push_back(RawChunk{fourcc('U', 'N', 'K', static_cast<char>('A' + i)), Bytes(i % 3, 7)});
  }
  NEED(test::load(test::arena(0), build(cs), &rep) == Progress::Done);
  CHECK(proj(0).unknown_count() == kMaxUnknownChunks);
  CHECK(proj(0).unknown_dropped_chunks() == 3);
}

namespace {

struct Mutation {
  const char* name;
  Error expect;
  const char* detail;  // a fragment of Report::detail, "" = do not check
  void (*apply)(std::vector<RawChunk>&, BuildOptions&);
};

Bytes& payload(std::vector<RawChunk>& cs, std::uint32_t id, int nth = 0) { return chunk_of(cs, id, nth).payload; }

const Mutation kMutations[] = {
    // ---- PHDR
    {"tempo 0", Error::Malformed, "tempo", [](auto& cs, auto&) { put32_at(payload(cs, kChunkPhdr), 4, 0); }},
    {"tempo too high", Error::Malformed, "tempo",
     [](auto& cs, auto&) { put32_at(payload(cs, kChunkPhdr), 4, kMaxTempoX100 + 1); }},
    {"swing 10", Error::Malformed, "swing", [](auto& cs, auto&) { payload(cs, kChunkPhdr)[8] = 10; }},
    {"no active tracks", Error::Malformed, "active", [](auto& cs, auto&) { payload(cs, kChunkPhdr)[2] = 0; }},
    {"17 active tracks", Error::Malformed, "active", [](auto& cs, auto&) { payload(cs, kChunkPhdr)[2] = 17; }},
    {"default length 0", Error::Malformed, "default pattern length",
     [](auto& cs, auto&) { payload(cs, kChunkPhdr)[3] = 0; }},
    {"master volume 200", Error::Malformed, "master", [](auto& cs, auto&) { payload(cs, kChunkPhdr)[9] = 200; }},
    {"master flag bit 4", Error::Malformed, "master", [](auto& cs, auto&) { payload(cs, kChunkPhdr)[13] = 0x10; }},
    {"name 33 characters", Error::Malformed, "do not fit", [](auto& cs, auto&) { payload(cs, kChunkPhdr)[18] = 33; }},
    {"NUL in the name", Error::Malformed, "do not fit", [](auto& cs, auto&) { payload(cs, kChunkPhdr)[20] = 0; }},
    {"129 patterns announced", Error::LimitExceeded, "more patterns",
     [](auto& cs, auto&) { payload(cs, kChunkPhdr)[15] = 129; }},
    {"65 instruments announced", Error::LimitExceeded, "more patterns",
     [](auto& cs, auto&) { payload(cs, kChunkPhdr)[16] = 65; }},
    {"17 modules announced", Error::LimitExceeded, "more patterns",
     [](auto& cs, auto&) { payload(cs, kChunkPhdr)[17] = 17; }},
    {"pattern count differs", Error::Malformed, "counts", [](auto& cs, auto&) { payload(cs, kChunkPhdr)[15] = 2; }},
    {"instrument count differs", Error::Malformed, "counts",
     [](auto& cs, auto&) { payload(cs, kChunkPhdr)[16] = 4; }},
    {"module count differs", Error::Malformed, "counts", [](auto& cs, auto&) { payload(cs, kChunkPhdr)[17] = 1; }},
    {"PHDR version 0", Error::Malformed, "version 0",
     [](auto& cs, auto&) { payload(cs, kChunkPhdr)[0] = 0; payload(cs, kChunkPhdr)[1] = 0; }},
    {"PHDR newer major", Error::NewerMajor, "major", [](auto& cs, auto&) { payload(cs, kChunkPhdr)[0] = 2; }},
    {"PHDR shorter than its fields", Error::Malformed, "do not fit",
     [](auto& cs, auto&) { payload(cs, kChunkPhdr).pop_back(); }},
    {"PHDR trailing byte", Error::Malformed, "do not fit",
     [](auto& cs, auto&) { payload(cs, kChunkPhdr).push_back(0); }},
    {"PHDR length below minimum", Error::Malformed, "length out of range",
     [](auto& cs, auto&) { payload(cs, kChunkPhdr).resize(10); }},
    // ---- chunk order
    {"no PHDR first", Error::Malformed, "first chunk", [](auto& cs, auto&) { cs.erase(cs.begin()); }},
    {"unknown chunk before PHDR", Error::Malformed, "first chunk",
     [](auto& cs, auto&) { cs.insert(cs.begin(), RawChunk{fourcc('X', 'X', 'X', 'X'), {}}); }},
    {"PHDR twice", Error::Malformed, "out of order",
     [](auto& cs, auto&) { cs.insert(cs.begin() + 1, cs[0]); }},
    {"TRAK and MODR swapped", Error::Malformed, "out of order", [](auto& cs, auto&) { std::swap(cs[1], cs[2]); }},
    {"SONG twice", Error::Malformed, "out of order",
     [](auto& cs, auto&) { cs.insert(cs.begin() + 4, cs[3]); }},
    {"PATT after INST", Error::Malformed, "out of order", [](auto& cs, auto&) {
       RawChunk p = chunk_of(cs, kChunkPatt);
       cs.insert(cs.begin() + 8, p);
     }},
    {"INST before PATT", Error::Malformed, "out of order", [](auto& cs, auto&) { std::swap(cs[6], cs[7]); }},
    // ---- TRAK
    {"0 track slots", Error::Malformed, "slot count", [](auto& cs, auto&) { payload(cs, kChunkTrak)[2] = 0; }},
    {"17 track slots", Error::Malformed, "slot count", [](auto& cs, auto&) { payload(cs, kChunkTrak)[2] = 17; }},
    {"track type 3", Error::Malformed, "track slot", [](auto& cs, auto&) { payload(cs, kChunkTrak)[3] = 3; }},
    {"track flags 4", Error::Malformed, "track slot", [](auto& cs, auto&) { payload(cs, kChunkTrak)[4] = 4; }},
    {"track volume 101", Error::Malformed, "track slot", [](auto& cs, auto&) { payload(cs, kChunkTrak)[5] = 101; }},
    {"track bus 17", Error::Malformed, "track slot", [](auto& cs, auto&) { payload(cs, kChunkTrak)[7] = 17; }},
    {"track name 21", Error::Malformed, "do not fit", [](auto& cs, auto&) { payload(cs, kChunkTrak)[8] = 21; }},
    {"NUL in a track name", Error::Malformed, "do not fit", [](auto& cs, auto&) { payload(cs, kChunkTrak)[9] = 0; }},
    {"TRAK newer major", Error::NewerMajor, "major", [](auto& cs, auto&) { payload(cs, kChunkTrak)[0] = 3; }},
    // ---- MODR
    {"17 modules", Error::LimitExceeded, "MODR", [](auto& cs, auto&) { payload(cs, kChunkModr)[2] = 17; }},
    {"duplicate module", Error::Malformed, "module", [](auto& cs, auto&) {
       payload(cs, kChunkModr) = Bytes{1, 0, 2, 1, 0, 5, 't', 'b', '3', '0', '3', 1, 2, 5, 't', 'b', '3', '0', '3'};
     }},
    {"module name with a slash", Error::Malformed, "module", [](auto& cs, auto&) { payload(cs, kChunkModr)[6] = '/'; }},
    {"module name 32 characters", Error::Malformed, "do not fit",
     [](auto& cs, auto&) { payload(cs, kChunkModr)[5] = 32; }},
    // ---- SONG
    {"pattern id 0 in the song", Error::Malformed, "SONG", [](auto& cs, auto&) { payload(cs, kChunkSong)[3] = 0; }},
    {"song longer than its chunk", Error::Malformed, "do not fit",
     [](auto& cs, auto&) { payload(cs, kChunkSong)[2] = 200; }},
    {"song trailing bytes", Error::Malformed, "do not fit",
     [](auto& cs, auto&) { payload(cs, kChunkSong).push_back(1); }},
    // ---- PATT (pattern 1: ver 0-1, id [2], tracks [3], name length [4], name "Intro" [5..9], track 0: length [10],
    // stored steps [11]; pattern 7: name "Fill" [5..8], track 0: length [9], stored steps [10])
    {"pattern id 0", Error::Malformed, "PATT", [](auto& cs, auto&) { payload(cs, kChunkPatt)[2] = 0; }},
    {"track length 0", Error::Malformed, "track length", [](auto& cs, auto&) { payload(cs, kChunkPatt)[10] = 0; }},
    {"track length 129", Error::Malformed, "track length", [](auto& cs, auto&) { payload(cs, kChunkPatt)[10] = 129; }},
    {"last track length 0", Error::Malformed, "track length",
     [](auto& cs, auto&) { payload(cs, kChunkPatt, 2)[9 + 15 * 2] = 0; }},
    {"pattern with 0 tracks", Error::Malformed, "PATT", [](auto& cs, auto&) { payload(cs, kChunkPatt)[3] = 0; }},
    {"pattern with 17 tracks", Error::Malformed, "PATT", [](auto& cs, auto&) { payload(cs, kChunkPatt)[3] = 17; }},
    {"pattern name 31", Error::Malformed, "PATT", [](auto& cs, auto&) { payload(cs, kChunkPatt)[4] = 31; }},
    {"NUL in a pattern name", Error::Malformed, "NUL", [](auto& cs, auto&) { payload(cs, kChunkPatt)[5] = 0; }},
    {"duplicate pattern id", Error::Malformed, "duplicate id",
     [](auto& cs, auto&) { payload(cs, kChunkPatt, 1)[2] = 1; }},
    {"129 steps in a track", Error::Malformed, "more than 128",
     [](auto& cs, auto&) { payload(cs, kChunkPatt)[11] = 129; }},
    {"track longer than the chunk", Error::Malformed, "inside a track",
     [](auto& cs, auto&) { payload(cs, kChunkPatt, 2)[10] = 128; }},  // pattern 7: 41 bytes in all
    {"PATT cut inside a track", Error::Malformed, "",
     [](auto& cs, auto&) { payload(cs, kChunkPatt).resize(payload(cs, kChunkPatt).size() - 3); }},
    {"PATT with too few tracks", Error::Malformed, "track list",
     [](auto& cs, auto&) { payload(cs, kChunkPatt, 2).resize(5 + 4 + 3 * 2); }},
    {"PATT trailing bytes", Error::Malformed, "unused bytes",
     [](auto& cs, auto&) { payload(cs, kChunkPatt).push_back(0); }},
    {"PATT length below minimum", Error::Malformed, "length out of range",
     [](auto& cs, auto&) { payload(cs, kChunkPatt).resize(6); }},
    {"PATT newer major", Error::NewerMajor, "major", [](auto& cs, auto&) { payload(cs, kChunkPatt)[0] = 9; }},
    // ---- INST
    {"instrument index 64", Error::Malformed, "INST", [](auto& cs, auto&) { payload(cs, kChunkInst)[2] = 64; }},
    {"instrument kind 0", Error::Malformed, "INST", [](auto& cs, auto&) { payload(cs, kChunkInst)[3] = 0; }},
    {"instrument kind 4", Error::Malformed, "INST", [](auto& cs, auto&) { payload(cs, kChunkInst)[3] = 4; }},
    {"duplicate instrument slot", Error::Malformed, "duplicate slot",
     [](auto& cs, auto&) { payload(cs, kChunkInst, 1)[2] = 0; }},
    {"MIDI channel 0", Error::Malformed, "MIDI", [](auto& cs, auto&) { payload(cs, kChunkInst, 1)[4] = 0; }},
    {"module index outside the table", Error::Malformed, "module", [](auto& cs, auto&) { payload(cs, kChunkInst, 2)[5] = 5; }},
    {"sample instrument naming a module", Error::Malformed, "module",
     [](auto& cs, auto&) { payload(cs, kChunkInst, 0)[5] = 0; }},
    {"NaN in a filter value", Error::Malformed, "non-finite", [](auto& cs, auto&) {
       Bytes& p = payload(cs, kChunkInst, 0);
       put32_at(p, sampler_at(p) + 36, 0x7FC00000u);
     }},
    {"infinite envelope", Error::Malformed, "non-finite", [](auto& cs, auto&) {
       Bytes& p = payload(cs, kChunkInst, 0);
       put32_at(p, sampler_at(p) + 44, 0xFF800000u);  // envelope 0 amount
     }},
    {"path escaping the folder", Error::Malformed, "unsafe", [](auto& cs, auto&) {
       Bytes& p = payload(cs, kChunkInst, 0);
       p[12] = '.';
       p[13] = '.';
       p[14] = '/';  // "../xles/kick.wav": a '..' component
       p[15] = 'x';
     }},
    {"absolute path", Error::Malformed, "unsafe", [](auto& cs, auto&) { payload(cs, kChunkInst, 0)[12] = '/'; }},
    {"blob longer than 256", Error::Malformed, "blob", [](auto& cs, auto&) {
       Bytes& p = payload(cs, kChunkInst, 2);
       const std::size_t at = sampler_at(p) + 296;
       p[at] = 1;
       p[at + 1] = 1;  // 257
     }},
    {"play mode 8", Error::Malformed, "play mode", [](auto& cs, auto&) {
       Bytes& p = payload(cs, kChunkInst, 0);
       p[sampler_at(p) + 1] = 8;
     }},
    {"volume 101", Error::Malformed, "level", [](auto& cs, auto&) {
       Bytes& p = payload(cs, kChunkInst, 0);
       p[sampler_at(p) + 6] = 101;
     }},
    {"INST cut short", Error::Malformed, "", [](auto& cs, auto&) { payload(cs, kChunkInst, 0).resize(100); }},
    {"INST length below minimum", Error::Malformed, "length out of range",
     [](auto& cs, auto&) { payload(cs, kChunkInst, 0).resize(50); }},
    {"INST newer major", Error::NewerMajor, "major", [](auto& cs, auto&) { payload(cs, kChunkInst)[0] = 2; }},
    // ---- unknown chunks and the container
    {"chunk id not printable", Error::Malformed, "printable",
     [](auto& cs, auto&) { cs.push_back(RawChunk{0x01020304u, Bytes{1}}); }},
    {"pad byte not zero", Error::Malformed, "pad", [](auto&, auto& o) { o.pad_byte = 1; }},
    {"FORM without a CRC chunk", Error::Malformed, "without a CRC", [](auto&, auto& o) { o.crc_chunk = false; }},
    {"CRC off by one bit", Error::BadCrc, "CRC", [](auto&, auto& o) { o.fix_crc = false; }},
    {"CRC chunk newer major", Error::NewerMajor, "major", [](auto&, auto& o) { o.crc_version = 0x0200; }},
    {"CRC chunk version 0", Error::Malformed, "version 0", [](auto&, auto& o) { o.crc_version = 0; }},
};

}  // namespace

TEST_CASE("native: every rejection is typed, explained, and leaves a valid empty project") {
  const Bytes base = small_file();
  const std::vector<RawChunk> chunks = test::split_chunks(base);
  NEED(chunks.size() == 12);
  for (const Mutation& m : kMutations) {
    INFO("mutation: " << std::string(m.name));
    std::vector<RawChunk> cs = chunks;
    BuildOptions opt;
    m.apply(cs, opt);
    Report rep;
    test::fill_small(proj(1));  // something to be replaced
    const Progress pr = test::load(test::arena(1), build(cs, opt), &rep);
    CHECK(pr == Progress::Failed);
    CHECK(rep.error == m.expect);
    CHECK(contains(rep.detail, m.detail));
    INFO("error " << error_name(rep.error) << ": " << rep.detail);
    CHECK(test::is_default_empty(proj(1)));
  }
}

TEST_CASE("native: the pattern pool limit is enforced while loading, not only announced") {
  // PHDR announcing 128 patterns and no instruments or modules, then that many PATT chunks of 37 bytes each
  std::vector<RawChunk> cs = test::split_chunks(small_file());
  RawChunk phdr = cs[0];
  phdr.payload[14] = 0;
  phdr.payload[15] = 128;
  phdr.payload[16] = 0;
  phdr.payload[17] = 0;
  std::vector<RawChunk> file = {phdr};
  for (std::uint32_t id = 1; id <= kMaxPatterns + 1; ++id) {
    Bytes patt = {0x01, 0x00, static_cast<std::uint8_t>(id), 16, 0};  // version, id, 16 tracks, no name
    for (int t = 0; t < 16; ++t) {
      patt.push_back(8);  // track length 8
      patt.push_back(0);  // no stored steps
    }
    file.push_back(RawChunk{kChunkPatt, patt});
  }
  Report rep;
  CHECK(test::load(test::arena(1), build(file), &rep) == Progress::Failed);  // 129 chunks
  CHECK(rep.error == Error::LimitExceeded);
  CHECK(contains(rep.detail, "pool full"));
  file.pop_back();
  CHECK(test::load(test::arena(1), build(file), &rep) == Progress::Done);  // exactly 128
  CHECK(rep.patterns == kMaxPatterns);
  CHECK(proj(1).pattern_count() == kMaxPatterns);
}

TEST_CASE("native: tracks a PATT chunk does not store are empty and take the project's default length") {
  std::vector<RawChunk> cs = test::split_chunks(small_file());  // default pattern length 16
  Bytes& p7 = payload(cs, kChunkPatt, 2);
  p7 = Bytes{0x01, 0x00, 7, 2, 0, /* track 0 */ 5, 0, /* track 1 */ 9, 1, 60, 3, 0xFF, 0, 0, 0, 0};
  Report rep;
  NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
  const Pattern* pat = proj(1).pattern(7);
  NEED(pat != nullptr);
  CHECK(pat->track_length[0] == 5);
  CHECK(pat->track_length[1] == 9);
  CHECK(pat->track_length[2] == 16);
  CHECK(pat->track_length[15] == 16);
  CHECK(pat->steps[1][0].note == 60);
  CHECK(is_empty(pat->steps[2][0]));
  Bytes out;
  NEED(test::save(proj(1), out) == Progress::Done);  // saved with all 16 tracks
  std::vector<RawChunk> back = test::split_chunks(out);
  CHECK(chunk_of(back, kChunkPatt, 2).payload.size() == 5 + 2 * 16 + 7);
  CHECK(chunk_of(back, kChunkPatt, 2).payload[3] == 16);
}

TEST_CASE("native: errors that need bytes the builder would repair") {
  const Bytes base = small_file();
  Report rep;

  Bytes b = base;  // not a FORM
  std::memcpy(b.data(), "LIST", 4);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::BadMagic);
  b = base;  // not our type
  std::memcpy(b.data() + 8, "O3PX", 4);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::BadMagic);
  b = Bytes(100, 0x55);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::BadMagic);

  CHECK(test::load(test::arena(1), Bytes{}, &rep) == Progress::Failed);  // nothing at all
  CHECK(rep.error == Error::Truncated);
  CHECK(rep.offset == 0);

  b = base;  // odd FORM length
  put32_at(b, 4, test::be32(&b[4]) + 1);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  b = base;  // FORM too short for anything
  put32_at(b, 4, 4);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  b = base;  // FORM longer than the limit
  put32_at(b, 4, kMaxFileBytes);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::LimitExceeded);
  // FORM longer than the file. The CRC covers the FORM header, so unrepaired this is BadCrc; repaired, the
  // CRC chunk is not where the FORM ends: the reader stops there and says so (it never needs the bytes).
  b = base;
  put32_at(b, 4, test::be32(&b[4]) + 100);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::BadCrc);
  test::set_crc(b, b.size() - 14);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  CHECK(contains(rep.detail, "after the CRC"));
  b = base;  // FORM shorter than the file (two bytes less): the CRC chunk overruns it
  put32_at(b, 4, test::be32(&b[4]) - 2);
  test::set_crc(b, b.size() - 14);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);

  b = base;  // a chunk claiming more than the FORM has left
  put32_at(b, 16, 0x00FFFFFF);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  CHECK(contains(rep.detail, "overruns"));
  b = base;  // a chunk claiming 4 GiB: no overflow in the arithmetic
  put32_at(b, 16, 0xFFFFFFFFu);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  b = base;
  put32_at(b, 16, 0xFFFFFFFEu);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);

  // a chunk after the CRC chunk, FORM length covering it
  b = base;
  const Bytes extra = {'X', 'T', 'R', 'B', 0, 0, 0, 2, 1, 2};
  b.insert(b.end(), extra.begin(), extra.end());
  put32_at(b, 4, static_cast<std::uint32_t>(b.size() - 8));
  test::set_crc(b, base.size() - 14);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  CHECK(contains(rep.detail, "after the CRC"));

  // a flipped payload byte is caught by the CRC (the parsers accept it)
  b = base;
  b[b.size() / 2] ^= 0x01;
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK((rep.error == Error::BadCrc || rep.error == Error::Malformed));
  b = base;
  std::vector<RawChunk> cs = test::split_chunks(base);
  payload(cs, kChunkPatt, 1)[9] ^= 0x40;  // a step of pattern 2 (track 1, step 0): valid either way
  b = build(cs, BuildOptions{true, false, 0, kChunkVersion, 0});
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Failed);
  CHECK(rep.error == Error::BadCrc);
  CHECK(rep.chunk == kChunkCrc);
  CHECK(test::is_default_empty(proj(1)));

  // bytes after the FORM are not looked at
  b = base;
  b.insert(b.end(), 50, 0xAA);
  CHECK(test::load(test::arena(1), b, &rep) == Progress::Done);
  CHECK(rep.bytes == base.size());
}

TEST_CASE("native: truncated at every offset of a small file is Truncated, never a half-loaded project") {
  const Bytes file = small_file();
  NEED(file.size() > 1000);
  for (std::size_t cut = 0; cut < file.size(); ++cut) {
    CAPTURE(cut);
    const Bytes prefix(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(cut));
    test::fill_small(proj(1));
    Report rep;
    NEED(test::load(test::arena(1), prefix, &rep) == Progress::Failed);
    NEED(rep.error == Error::Truncated);
    NEED(rep.offset == cut);
    NEED(test::is_default_empty(proj(1)));
  }
  Report rep;
  CHECK(test::load(test::arena(1), file, &rep) == Progress::Done);
  CHECK(proj(1).pattern_count() == 3);
}

TEST_CASE("native: a source that fails or breaks its contract ends in SourceFailed") {
  const Bytes file = small_file();
  for (std::size_t at : {std::size_t{0}, std::size_t{5}, std::size_t{12}, std::size_t{20}, file.size() / 2,
                         file.size() - 1}) {
    CAPTURE(at);
    MemorySource src(file);
    src.fail_at = at;
    Reader rd;
    rd.begin(src, test::arena(1));
    Progress pr = Progress::Running;
    while (pr == Progress::Running) {
      pr = rd.step();
    }
    CHECK(pr == Progress::Failed);
    CHECK(rd.report().error == Error::SourceFailed);
    CHECK(test::is_default_empty(proj(1)));
  }
  for (MemorySource::Lie lie : {MemorySource::Lie::TooMany, MemorySource::Lie::OkZero}) {
    MemorySource src(file);
    src.lie = lie;
    Reader rd;
    rd.begin(src, test::arena(1));
    CHECK(rd.step() == Progress::Failed);
    CHECK(rd.report().error == Error::SourceFailed);
    CHECK(contains(rd.report().detail, "contract"));
    CHECK(rd.step() == Progress::Failed);  // stays failed
  }
}

TEST_CASE("native: a newer minor version is read, noted, and its extra fields are lost on the next save") {
  const Bytes base = small_file();
  std::vector<RawChunk> cs = test::split_chunks(base);
  // a hypothetical next minor version of every chunk kind (1.1; INST is at 1.3 since OS-INSTUI S8b, so its next is 1.4): version bumped,
  // two unknown bytes appended to the fields
  for (RawChunk& c : cs) {
    if (c.id == kChunkInst) {
      make_inst_next_minor(c, 2, 0xDE);
    } else if (c.id == kChunkPhdr || c.id == kChunkTrak || c.id == kChunkModr || c.id == kChunkSong || c.id == kChunkPatt) {
      c.payload[1] = 1;
      c.payload.push_back(0xDE);
      c.payload.push_back(0xAD);
    }
  }
  Report rep;
  NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
  CHECK(rep.newer_minor);
  CHECK(rep.error == Error::None);
  CHECK(proj(1).pattern_count() == 3);
  CHECK(proj(1).instrument_count() == 3);
  Bytes out;
  NEED(test::save(proj(1), out) == Progress::Done);
  CHECK(out == base);  // the unknown tail is gone, the rest is what was read

  // the CRC chunk itself may be a newer minor
  BuildOptions o;
  o.crc_version = 0x0105;
  NEED(test::load(test::arena(1), build(test::split_chunks(base), o), &rep) == Progress::Done);
  CHECK(rep.newer_minor);
}

TEST_CASE("native: INST is version 1.1 with the sample rate appended; 1.0 reads as rate 0, a 1.4 tail is skipped (OS-002c, OS-004d, OS-INSTUI S8b)") {
  test::fill_small(proj(0));
  CHECK(proj(0).instrument(0)->sample_rate == 22050u);
  Bytes base;
  NEED(test::save(proj(0), base) == Progress::Done);

  // The writer: version 1.1, and the rate is the last four bytes of the fields (after the module blob), big-endian.
  std::vector<RawChunk> cs = test::split_chunks(base);
  std::size_t insts = 0;
  for (const RawChunk& c : cs) {
    if (c.id == kChunkInst) {
      ++insts;
      CHECK((c.payload[0] == 1 && c.payload[1] == 1));
    }
  }
  CHECK(insts == 3u);
  for (RawChunk& c : cs) {
    if (c.id == kChunkInst && c.payload[2] == 0) {  // slot 0, the sample with a rate of 22,050 = 0x5622
      const std::size_t n = c.payload.size();
      CHECK((c.payload[n - 4] == 0 && c.payload[n - 3] == 0 && c.payload[n - 2] == 0x56 && c.payload[n - 1] == 0x22));
    }
  }
  Report rep;
  NEED(test::load(test::arena(1), base, &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  CHECK(proj(1).instrument(0)->sample_rate == 22050u);

  // A chunk written by a reader that knew only 1.0: no rate, read as 0 (not recorded), not a newer minor.
  std::vector<RawChunk> old = cs;
  for (RawChunk& c : old) {
    if (c.id == kChunkInst) {
      c.payload[1] = 0;
      c.payload.resize(c.payload.size() - 4);
    }
  }
  NEED(test::load(test::arena(1), build(old), &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  CHECK(proj(1).instrument(0)->sample_rate == 0u);
  CHECK(proj(1).instrument_count() == 3);
  Bytes resaved;
  NEED(test::save(proj(1), resaved) == Progress::Done);
  CHECK(resaved != base);  // written as 1.1 now, with rate 0 where there was none

  // Version 1.0 has no room for the four bytes, and version 1.1 needs them.
  std::vector<RawChunk> bad = cs;
  for (RawChunk& c : bad) {
    if (c.id == kChunkInst) {
      c.payload[1] = 0;  // 1.0 with the rate left on: unused bytes
    }
  }
  CHECK(test::load(test::arena(1), build(bad), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  bad = cs;
  for (RawChunk& c : bad) {
    if (c.id == kChunkInst) {
      c.payload.resize(c.payload.size() - 2);  // 1.1 cut inside the rate
    }
  }
  CHECK(test::load(test::arena(1), build(bad), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);

  // A 1.4, which a later firmware may write with a field of its own after the twelve of 1.3: read as far as 1.3 (here the engine is 0x11, which is not one, so it reads as automatic, and the
  // twelve are the defaults), the tail skipped, the report says so, and the next save writes 1.1 (an automatic engine and the defaults, what this reader knows to leave out).
  std::vector<RawChunk> next = cs;
  for (RawChunk& c : next) {
    if (c.id == kChunkInst) {
      make_inst_next_minor(c, 7, 0x33, 0x11);
    }
  }
  NEED(test::load(test::arena(1), build(next), &rep) == Progress::Done);
  CHECK(rep.newer_minor);
  CHECK(proj(1).instrument(0)->sample_rate == 22050u);
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == base);

  // The same file as seen by a reader that only knows 1.0 would be the "newer minor" case above: the tail (here the rate) is
  // skipped. That is what the 1.0 limits of every other chunk have always done, and what the Python reference does too
  // (project_python).
}

TEST_CASE("native: a Writer refuses a sink that fails or lies, and a project that changes under it") {
  test::fill_small(proj(0));
  Bytes reference;
  NEED(test::save(proj(0), reference) == Progress::Done);

  for (std::size_t after : {std::size_t{0}, std::size_t{11}, std::size_t{12}, reference.size() / 2,
                            reference.size() - 1}) {
    CAPTURE(after);
    VectorSink sink;
    sink.fail_after = after;
    Writer w;
    w.begin(proj(0), sink);
    Progress pr = Progress::Running;
    while (pr == Progress::Running) {
      pr = w.step();
    }
    CHECK(pr == Progress::Failed);
    CHECK(w.report().error == Error::SinkFailed);
    CHECK(sink.bytes.size() == after);
    CHECK(w.step() == Progress::Failed);
  }
  for (VectorSink::Lie lie : {VectorSink::Lie::TooMany, VectorSink::Lie::OkZero, VectorSink::Lie::End}) {
    VectorSink sink;
    sink.lie = lie;
    Writer w;
    w.begin(proj(0), sink);
    Progress pr = Progress::Running;
    while (pr == Progress::Running) {
      pr = w.step();
    }
    CHECK(pr == Progress::Failed);
    CHECK(w.report().error == Error::SinkFailed);
  }

  VectorSink slow;  // blocks half the time and takes three bytes at a time: still the same file
  slow.max_per_write = 3;
  slow.block_every = 2;
  Writer w;
  w.begin(proj(0), slow);
  std::uint32_t guard = 0;
  while (w.step(50) == Progress::Running && ++guard < 10000000u) {
  }
  CHECK(w.progress() == Progress::Done);
  CHECK(slow.bytes == reference);

  VectorSink sink;
  Writer m;
  m.begin(proj(0), sink);
  NEED(m.step(100) == Progress::Running);
  NEED(proj(0).set_swing(70) == Status::Ok);  // a change between two steps
  Progress pr = Progress::Running;
  while (pr == Progress::Running) {
    pr = m.step(100);
  }
  CHECK(pr == Progress::Failed);  // at the next piece boundary
  CHECK(m.report().error == Error::Modified);
  CHECK(sink.bytes.size() < reference.size());
}

TEST_CASE("native: the sizing pre-pass is budgeted, writes nothing, and a project change during it is caught") {
  test::fill_maximal(proj(0));
  Bytes reference;
  NEED(test::save(proj(0), reference) == Progress::Done);

  VectorSink sink;
  Writer w;
  w.begin(proj(0), sink);
  std::uint32_t calls = 0;
  while (!w.sized() && calls < 1000000u) {
    NEED(w.step(1) == Progress::Running);  // budget 1: one item per call, never the whole project
    if (!w.sized()) {
      CHECK(sink.bytes.empty());  // nothing is written before the size is known
    }
    ++calls;
  }
  CHECK(calls > 2048);  // 128 patterns x 16 tracks are separate items
  CHECK(w.total_bytes() == reference.size());
  CHECK(sink.bytes.size() <= 1);  // the call that finished the sizing had a byte of budget left
  while (w.step() == Progress::Running) {
  }
  CHECK(w.progress() == Progress::Done);
  CHECK(sink.bytes == reference);

  VectorSink sink2;  // changed under the pre-pass
  Writer m;
  m.begin(proj(0), sink2);
  NEED(m.step(1) == Progress::Running);
  NEED(proj(0).set_swing(30) == Status::Ok);
  CHECK(m.step(1) == Progress::Failed);
  CHECK(m.report().error == Error::Modified);
  CHECK(sink2.bytes.empty());
  CHECK(m.total_bytes() == 0);
}

TEST_CASE("native: a chunk of a newer major version is refused as such, whatever its length") {
  const std::vector<RawChunk> chunks = test::split_chunks(small_file());
  struct Case {
    std::uint32_t id;
    int nth;
    std::size_t length;
  };
  const Case cases[] = {
      {kChunkPhdr, 0, 100}, {kChunkPhdr, 0, 10}, {kChunkPhdr, 0, 2},   {kChunkTrak, 0, 1000}, {kChunkTrak, 0, 5},
      {kChunkModr, 0, 2000}, {kChunkSong, 0, 300}, {kChunkSong, 0, 2}, {kChunkPatt, 0, 20000}, {kChunkPatt, 0, 3},
      {kChunkPatt, 2, 2},   {kChunkInst, 0, 900}, {kChunkInst, 0, 2},  {kChunkInst, 1, 100000},
  };
  for (const Case& c : cases) {
    INFO("chunk " << c.id << " length " << c.length);
    std::vector<RawChunk> cs = chunks;
    Bytes& pl = chunk_of(cs, c.id, c.nth).payload;
    pl.resize(c.length, 0x5A);
    pl[0] = 2;  // version 2.0
    pl[1] = 0;
    Report rep;
    CHECK(test::load(test::arena(1), build(cs), &rep) == Progress::Failed);
    CHECK(rep.error == Error::NewerMajor);
    CHECK(test::is_default_empty(proj(1)));
  }
  for (std::size_t tail : {std::size_t{0}, std::size_t{1}, std::size_t{100}, std::size_t{70000}}) {
    BuildOptions o;
    o.crc_version = 0x0200;
    o.crc_tail = tail;
    Report rep;
    CHECK(test::load(test::arena(1), build(chunks, o), &rep) == Progress::Failed);
    CHECK(rep.error == Error::NewerMajor);  // the CRC chunk too
  }
  // a known chunk with no room for even a version has no layout to speak of
  std::vector<RawChunk> cs = chunks;
  cs[0].payload.resize(1);
  Report rep;
  CHECK(test::load(test::arena(1), build(cs), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  cs = chunks;
  cs[0].payload.clear();
  CHECK(test::load(test::arena(1), build(cs), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
}

TEST_CASE("native: a chunk of a newer minor version with appended fields is read at any length") {
  const Bytes base = small_file();
  const std::vector<RawChunk> chunks = test::split_chunks(base);
  const std::uint32_t kinds[] = {kChunkPhdr, kChunkTrak, kChunkModr, kChunkSong, kChunkPatt, kChunkInst};
  // 20,000 bytes of tail is above the version 1.0 maximum of every chunk kind
  for (std::uint32_t kind : kinds) {
    for (std::size_t tail : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{100}, std::size_t{20000}}) {
      INFO("kind " << kind << " tail " << tail);
      std::vector<RawChunk> cs = chunks;
      for (RawChunk& c : cs) {
        if (c.id == kind && kind == kChunkInst) {
          make_inst_next_minor(c, tail);  // the next minor version (INST is at 1.3)
        } else if (c.id == kind) {
          c.payload[1] = 1;
          c.payload.insert(c.payload.end(), tail, 0xAB);
        }
      }
      Report rep;
      NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
      CHECK(rep.newer_minor);
      CHECK(proj(1).pattern_count() == 3);
      CHECK(proj(1).instrument_count() == 3);
      CHECK(proj(1).module_count() == 2);
      Bytes out;
      NEED(test::save(proj(1), out) == Progress::Done);
      CHECK(out == base);  // the tail is gone, everything else is what was read
    }
  }
  for (std::size_t tail : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{100}, std::size_t{20000}}) {
    BuildOptions o;
    o.crc_version = 0x0101;
    o.crc_tail = tail;
    Report rep;
    NEED(test::load(test::arena(1), build(chunks, o), &rep) == Progress::Done);  // the CRC chunk has a newer minor too
    CHECK(rep.newer_minor);
    CHECK(rep.bytes == build(chunks, o).size());
    o.crc_version = 0x0100;  // the same length with version 1.0 is not a CRC chunk of this version
    CHECK(test::load(test::arena(1), build(chunks, o), &rep) == Progress::Failed);
    CHECK(rep.error == Error::Malformed);
    CHECK(contains(rep.detail, "length out of range"));
  }
  // a newer minor still has to hold everything version 1.0 holds
  std::vector<RawChunk> cs = chunks;
  cs[0].payload.resize(10);
  cs[0].payload[1] = 1;
  Report rep;
  CHECK(test::load(test::arena(1), build(cs), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  CHECK(contains(rep.detail, "length out of range"));
  // and its version 1.0 chunks keep their upper bound
  cs = chunks;
  cs[0].payload.resize(60, 0);
  CHECK(test::load(test::arena(1), build(cs), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  CHECK(contains(rep.detail, "length out of range"));
}

TEST_CASE("native: a Reader can be reused and replaces what the arena held") {
  const Bytes small = small_file();
  Bytes empty;
  proj(0).reset();
  NEED(test::save(proj(0), empty) == Progress::Done);
  test::fill_maximal(proj(1));
  NEED(test::load(test::arena(1), small) == Progress::Done);
  CHECK(proj(1).pattern_count() == 3);  // not 128
  CHECK(proj(1).instrument_count() == 3);
  NEED(test::load(test::arena(1), empty) == Progress::Done);
  CHECK(test::is_default_empty(proj(1)));
  CHECK(proj(1).song_length() == 0);
  CHECK(proj(1).track(0)->name[0] == '\0');
}

TEST_CASE("native: a note on instrument 64 (the MIDI instrument of channel 16, the last slot) round-trips byte for byte, with the MIDI instrument it names (OS-INSTRMIDI)") {
  Project& a = proj(0);
  a.reset();
  NEED(a.create_pattern(1, 16) == Status::Ok);
  Instrument in;
  init_instrument(in, InstrumentKind::Midi);
  in.midi_channel = 16;
  std::strcpy(in.name, "MIDI 16");
  NEED(a.set_instrument(63, in) == Status::Ok);
  Step s = kEmptyStep;
  s.note = kNoteFirst + 24;
  for (const std::uint8_t instr : {std::uint8_t{49}, std::uint8_t{63}, std::uint8_t{64}}) {
    s.instr = instr;
    NEED(a.set_step(1, 0, instr - 49u, s) == Status::Ok);
  }
  Bytes first;
  NEED(test::save(a, first) == Progress::Done);
  NEED(test::load(test::arena(1), first) == Progress::Done);
  Project& b = proj(1);
  NEED(b.pattern(1) != nullptr);
  CHECK(b.pattern(1)->steps[0][0].instr == 49);
  CHECK(b.pattern(1)->steps[0][14].instr == 63);
  CHECK(b.pattern(1)->steps[0][15].instr == 64);
  NEED(b.instrument(63) != nullptr);
  CHECK(b.instrument(63)->kind == InstrumentKind::Midi);
  CHECK(b.instrument(63)->midi_channel == 16);
  Bytes second;
  NEED(test::save(b, second) == Progress::Done);
  CHECK(second == first);
}
