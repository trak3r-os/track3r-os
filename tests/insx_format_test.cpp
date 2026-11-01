// SPDX-License-Identifier: GPL-3.0-or-later
// OS-INSFX slice S3a (docs/specs/OS-INSFX.md section 7): the inserts as data of the native project, the INSX chunk 1.0, written only when the project has an insert. This file is the model and
// format side: the mutators set_insert / clear_insert with their validation, the exact bytes (written here from the description, not by the writer), the round trip, the files that never had
// an insert (they are the files they were, and loading one clears the inserts a document held), the reader's tolerance (a record it cannot use is skipped and counted, the project opens), the
// shape errors (Malformed), a newer minor, the chunk's place, and the pool arithmetic of an older build that keeps the chunk. The Python reference and the old-reader simulation are
// tests/o3project_roundtrip.py. The use of the inserts (InsertBinder, App::apply_project_state) is S3b.
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/memory_io.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/formats/native.h>
#include <o3os/model/project.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using namespace o3os::model;
using o3os::test::RawChunk;
using Bytes = std::vector<std::uint8_t>;

namespace {

Project& proj(int i = 0) { return test::arena(i).project; }

Bytes build(const std::vector<RawChunk>& cs) { return test::build_file(cs, {}); }

void put_f32(Bytes& b, float f) {
  std::uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  test::put_be32(b, bits);
}

// One record as the description gives it.
Bytes rec(unsigned where, unsigned slot, unsigned module, unsigned flags, const std::vector<float>& v) {
  Bytes b = {static_cast<std::uint8_t>(where), static_cast<std::uint8_t>(slot), static_cast<std::uint8_t>(module), static_cast<std::uint8_t>(flags), static_cast<std::uint8_t>(v.size())};
  for (float f : v) put_f32(b, f);
  return b;
}

// A payload: version, count, records.
Bytes payload(unsigned version, unsigned count, const std::vector<Bytes>& records) {
  Bytes b;
  test::put_be16(b, version);
  b.push_back(static_cast<std::uint8_t>(count));
  for (const Bytes& r : records) b.insert(b.end(), r.begin(), r.end());
  return b;
}

// The persist2 fixture with the three inserts of fill_inserts.
Bytes saved() {
  test::fill_inserts(proj(2));
  Bytes out;
  CHECK(test::save(proj(2), out) == Progress::Done);
  return out;
}

// The chunks of that file, INSX's payload replaced (or the chunk dropped when `drop`).
std::vector<RawChunk> with_payload(const Bytes& p, bool drop = false) {
  std::vector<RawChunk> out;
  for (const RawChunk& c : test::split_chunks(saved())) {
    if (c.id != kChunkInsx) {
      out.push_back(c);
    } else if (!drop) {
      out.push_back({kChunkInsx, p});
    }
  }
  return out;
}

struct Outcome {
  Progress progress;
  Error error;
  std::uint32_t skipped;
  bool newer;
};

// Loads into arena 1, which held the inserts of another document before (a load must not inherit them).
Outcome load1(const Bytes& file) {
  test::fill_inserts(proj(1));
  Report rep;
  const Progress p = test::load(test::arena(1), file, &rep);
  return {p, rep.error, rep.settings_skipped, rep.newer_minor};
}

// The inserts the load left, as (where, slot, module, flags, n) of each record.
std::vector<std::vector<unsigned>> inserts_of(const Project& p) {
  std::vector<std::vector<unsigned>> out;
  for (std::uint32_t i = 0; i < p.inserts().count; ++i) {
    const InsertRecord& r = p.inserts().rec[i];
    out.push_back({r.where, r.slot, r.module, r.flags, r.n});
  }
  return out;
}

const std::vector<float> kThree = {1.0f, -2.5f, 0.125f};

}  // namespace

TEST_CASE("insx: the mutators validate, keep the records in (where, slot) order and mark the part present") {
  Project& p = proj(0);
  p.reset();
  CHECK(p.inserts().count == 0);
  CHECK(p.add_module("fx_a", 4, 1, 0) == Status::Ok);
  CHECK(p.add_module("fx_b", 4, 1, 0) == Status::Ok);
  const std::uint32_t g0 = p.generation();
  InsertRecord r;
  std::memset(&r, 0, sizeof r);
  r.where = 5;
  r.slot = 1;
  r.module = 1;
  r.n = 2;
  r.param[0] = 0.5f;
  r.param[1] = -1.0f;
  CHECK(p.set_insert(r) == Status::Ok);
  CHECK(p.generation() != g0);
  CHECK(p.inserts().count == 1);
  const std::uint32_t g1 = p.generation();
  CHECK(p.set_insert(r) == Status::Ok);  // equal: nothing changes
  CHECK(p.generation() == g1);
  // order: the master after the tracks, slot 0 before slot 1, the table stays sorted whatever the order of the calls
  InsertRecord m = r;
  m.where = kInsertWhereMaster;
  m.slot = 0;
  CHECK(p.set_insert(m) == Status::Ok);
  InsertRecord a = r;
  a.slot = 0;
  CHECK(p.set_insert(a) == Status::Ok);
  InsertRecord z = r;
  z.where = 0;
  z.slot = 1;
  CHECK(p.set_insert(z) == Status::Ok);  // the fourth, the last the cap allows (three were taken)
  CHECK(p.inserts().count == 4);
  CHECK(inserts_of(p) == std::vector<std::vector<unsigned>>{{0, 1, 1, 0, 2}, {5, 0, 1, 0, 2}, {5, 1, 1, 0, 2}, {16, 0, 1, 0, 2}});
  InsertRecord fifth = r;
  fifth.where = 9;
  CHECK(p.set_insert(fifth) == Status::Full);  // a fifth is refused, nothing changes
  CHECK(p.inserts().count == 4);
  // replacing one record keeps the count
  InsertRecord rep = r;
  rep.flags = kInsertFlagBypass;
  rep.param[0] = 3.0f;
  CHECK(p.set_insert(rep) == Status::Ok);
  CHECK(p.inserts().count == 4);
  NEED(p.find_insert(5, 1) != nullptr);
  CHECK(p.find_insert(5, 1)->flags == kInsertFlagBypass);
  CHECK(p.find_insert(5, 1)->param[0] == 3.0f);
  CHECK(p.find_insert(6, 0) == nullptr);
  CHECK(p.check_invariants());
  CHECK(validate_insert_state(p.inserts(), p.module_count()) == nullptr);
  // the module table is not pruned and cannot be cleared under an insert
  CHECK(p.clear_modules() == Status::BadValue);

  // removing: the others keep their order and the freed record is zero (the state is compared by bytes)
  CHECK(p.clear_insert(5, 0) == Status::Ok);
  CHECK(inserts_of(p) == std::vector<std::vector<unsigned>>{{0, 1, 1, 0, 2}, {5, 1, 1, 1, 2}, {16, 0, 1, 0, 2}});
  InsertRecord zero;
  std::memset(&zero, 0, sizeof zero);
  CHECK(p.inserts().rec[3] == zero);
  CHECK(p.clear_insert(5, 0) == Status::NotFound);
  CHECK(p.clear_insert(17, 0) == Status::BadIndex);
  CHECK(p.clear_insert(3, 2) == Status::BadIndex);
  CHECK(p.clear_insert(kInsertWhereMaster, 1) == Status::BadIndex);
  CHECK(p.inserts().count == 3);
  CHECK(p.check_invariants());
  p.clear_inserts();
  CHECK(p.inserts().count == 0);
  CHECK(p.module_count() == 2);  // MODR is not pruned
  CHECK(p.clear_modules() == Status::Ok);
  // a reset leaves none
  CHECK(p.add_module("fx_a", 4, 1, 0) == Status::Ok);
  InsertRecord first = a;
  first.module = 0;
  CHECK(p.set_insert(first) == Status::Ok);
  p.reset();
  CHECK(p.inserts().count == 0);
  CHECK(p.check_invariants());
}

TEST_CASE("insx: the mutators refuse what the format cannot hold, and change nothing") {
  Project& p = proj(0);
  p.reset();
  CHECK(p.add_module("fx_a", 4, 1, 0) == Status::Ok);
  InsertRecord good;
  std::memset(&good, 0, sizeof good);
  good.module = 0;
  good.n = 1;
  good.param[0] = 1.0f;
  CHECK(p.set_insert(good) == Status::Ok);
  const std::uint32_t gen = p.generation();
  const auto bad = [&](const char* what, auto edit) {
    InsertRecord r = good;
    r.where = 2;
    edit(r);
    CAPTURE(what);
    CHECK(p.set_insert(r) == Status::BadValue);
    CHECK(p.inserts().count == 1);
    CHECK(p.generation() == gen);
  };
  bad("where 17", [](InsertRecord& r) { r.where = 17; });
  bad("track slot 2", [](InsertRecord& r) { r.slot = 2; });
  bad("master slot 1", [](InsertRecord& r) { r.where = kInsertWhereMaster; r.slot = 1; });
  bad("module past MODR", [](InsertRecord& r) { r.module = 1; });
  bad("a flag bit", [](InsertRecord& r) { r.flags = 2; });
  bad("n 33", [](InsertRecord& r) { r.n = 33; });
  bad("NaN", [](InsertRecord& r) { r.param[0] = std::numeric_limits<float>::quiet_NaN(); });
  bad("infinity", [](InsertRecord& r) { r.param[0] = std::numeric_limits<float>::infinity(); });
  bad("negative infinity", [](InsertRecord& r) { r.param[0] = -std::numeric_limits<float>::infinity(); });
  bad("a value past n", [](InsertRecord& r) { r.param[5] = 1.0f; });
  bad("reserved byte", [](InsertRecord& r) { r.reserved[1] = 1; });
  // the whole-state check agrees
  InsertState s = p.inserts();
  CHECK(validate_insert_state(s, 1) == nullptr);
  CHECK(validate_insert_state(s, 0) != nullptr);  // module 0 is past an empty MODR
  s.rec[1] = s.rec[0];
  s.count = 2;
  CHECK(validate_insert_state(s, 1) != nullptr);  // not ascending (a duplicate)
  s.count = 5;
  CHECK(validate_insert_state(s, 1) != nullptr);
}

TEST_CASE("insx: the exact bytes of a project with inserts, from the description") {
  const Bytes file = saved();
  const std::vector<RawChunk> cs = test::split_chunks(file);
  std::size_t at = 0;
  while (at < cs.size() && cs[at].id != kChunkSnfx) {
    ++at;
  }
  NEED(at + 2 < cs.size());
  CHECK(cs[at + 1].id == kChunkInsx);  // after SNFX (there is no LAW here), before the unknown chunks
  CHECK(kChunkInsx == 0x494E5358u);    // 'I' 'N' 'S' 'X'
  CHECK(cs[at + 2].id != kChunkInsx);
  std::vector<float> blob;
  for (int k = 0; k < 32; ++k) blob.push_back(0.5f * static_cast<float>(k) - 3.25f);
  const Bytes want = payload(0x0100, 3, {rec(0, 0, 0, 0, {}), rec(3, 1, 1, 1, blob), rec(16, 0, 1, 0, {1.0f, -0.0f, 123456.789f})});
  CHECK(cs[at + 1].payload == want);
  CHECK(want.size() == 3u + 5u + (5u + 128u) + (5u + 12u));
  // the same file written from the description
  std::vector<RawChunk> described;
  for (const RawChunk& c : cs) described.push_back(c);
  CHECK(file == build(described));
  // and the project's file without the chunk is the persist2 file it came from
  test::fill_persist2(proj(0));
  Bytes persist2;
  NEED(test::save(proj(0), persist2) == Progress::Done);
  CHECK(build(with_payload({}, true)) == persist2);
  CHECK(persist2.size() == 3032u);  // pinned by tests/o3project_roundtrip.py (PRE_LAW) as well
}

TEST_CASE("insx: a project with inserts round trips; one without has no chunk and loading it clears the inserts") {
  const Bytes file = saved();
  const Outcome o = load1(file);
  CHECK(o.progress == Progress::Done);
  CHECK(o.skipped == 0u);
  CHECK_FALSE(o.newer);
  CHECK(proj(1).check_invariants());
  CHECK(proj(1).inserts().count == 3);
  CHECK(std::memcmp(&proj(1).inserts(), &proj(2).inserts(), sizeof(InsertState)) == 0);  // bit for bit, -0.0 included
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == file);

  // a file that never had one: no chunk, and the load puts back none whatever the document held
  const Bytes none = build(with_payload({}, true));
  const Outcome n = load1(none);
  CHECK(n.progress == Progress::Done);
  CHECK(proj(1).inserts().count == 0);
  CHECK(n.skipped == 0u);

  // an insert removed again: the chunk is gone, MODR is not pruned (the file differs from the one that never had an insert only by its module table: the fixture's two modules were there already)
  Project& q = proj(0);
  test::fill_inserts(q);
  CHECK(q.clear_insert(0, 0) == Status::Ok);
  CHECK(q.clear_insert(3, 1) == Status::Ok);
  CHECK(q.clear_insert(kInsertWhereMaster, 0) == Status::Ok);
  Bytes empty;
  NEED(test::save(q, empty) == Progress::Done);
  CHECK(empty == none);
  for (const RawChunk& c : test::split_chunks(empty)) CHECK(c.id != kChunkInsx);

  // a chunk of count 0 is read as none (no writer produces it)
  const Outcome z = load1(build(with_payload(payload(0x0100, 0, {}))));
  CHECK(z.progress == Progress::Done);
  CHECK(proj(1).inserts().count == 0);
}

TEST_CASE("insx: a record the reader cannot use is skipped and counted, the project opens") {
  const std::vector<Bytes> keep = {rec(2, 0, 0, 0, kThree)};
  const auto one = [&](const char* what, const Bytes& bad, unsigned skipped, unsigned flags_out = 0) {
    CAPTURE(what);
    const Outcome o = load1(build(with_payload(payload(0x0100, 2, {keep[0], bad}))));
    CHECK(o.progress == Progress::Done);
    CHECK(o.skipped == skipped);
    CHECK(proj(1).check_invariants());
    CHECK(inserts_of(proj(1)).size() == (skipped == 0 || flags_out != 0 ? 2u : 1u));
    CHECK(proj(1).inserts().rec[0].where == 2);
  };
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  one("module past MODR (2 entries: index 2)", rec(4, 0, 2, 0, kThree), 1);
  one("module 255", rec(4, 0, 255, 0, kThree), 1);
  one("where 17", rec(17, 0, 0, 0, kThree), 1);
  one("where 255", rec(255, 0, 0, 0, kThree), 1);
  one("track slot 2", rec(4, 2, 0, 0, kThree), 1);
  one("master slot 1", rec(kInsertWhereMaster, 1, 0, 0, kThree), 1);
  one("NaN first", rec(4, 0, 0, 0, {nan, 1.0f}), 1);
  one("NaN last", rec(4, 0, 0, 0, {1.0f, nan}), 1);
  one("+infinity", rec(4, 0, 0, 0, {inf}), 1);
  one("-infinity", rec(4, 0, 0, 0, {-inf}), 1);
  one("a duplicate of an earlier record", rec(2, 0, 1, 0, kThree), 1);
  one("flag bits other than bypass are cleared and counted", rec(4, 0, 0, 0x03, kThree), 1, 1);
  CHECK(proj(1).inserts().rec[1].flags == kInsertFlagBypass);
  one("flag bits and a module past MODR: the one record counts once", rec(4, 0, 255, 0x03, kThree), 1);
  // n above 32: the record's bytes are still its own and it is skipped (the chunk stays within its 535 bytes)
  std::vector<float> many(33, 0.25f);
  one("n 33", rec(4, 0, 0, 0, many), 1);
  // the record after a skipped one is read from the right place
  {
    const Outcome o = load1(build(with_payload(payload(0x0100, 3, {rec(1, 0, 9, 0, many), rec(1, 1, 0, 0, kThree), rec(3, 0, 0, 0, {})}))));
    CHECK(o.progress == Progress::Done);
    CHECK(o.skipped == 1u);
    CHECK(inserts_of(proj(1)) == std::vector<std::vector<unsigned>>{{1, 1, 0, 0, 3}, {3, 0, 0, 0, 0}});
    CHECK(proj(1).inserts().rec[0].param[1] == -2.5f);
  }
  // records out of order are stored in order; every one of four skipped leaves none and the project opens
  {
    const Outcome o = load1(build(with_payload(payload(0x0100, 3, {rec(16, 0, 0, 0, {}), rec(0, 1, 1, 0, {}), rec(0, 0, 0, 0, {})}))));
    CHECK(o.progress == Progress::Done);
    CHECK(o.skipped == 0u);
    CHECK(inserts_of(proj(1)) == std::vector<std::vector<unsigned>>{{0, 0, 0, 0, 0}, {0, 1, 1, 0, 0}, {16, 0, 0, 0, 0}});
    const Outcome all = load1(build(with_payload(payload(0x0100, 4, {rec(17, 0, 0, 0, {}), rec(0, 0, 7, 0, {}), rec(0, 2, 0, 0, {}), rec(1, 0, 0, 0, {nan})}))));
    CHECK(all.progress == Progress::Done);
    CHECK(all.skipped == 4u);
    CHECK(proj(1).inserts().count == 0);
  }
}

TEST_CASE("insx: a length that does not fit the count, a count above the cap or a bad version is Malformed") {
  const auto refused = [&](const char* what, const Bytes& p, Error want = Error::Malformed) {
    CAPTURE(what);
    const Outcome o = load1(build(with_payload(p)));
    CHECK(o.progress == Progress::Failed);
    CHECK(o.error == want);
  };
  refused("count 5", payload(0x0100, 5, {rec(0, 0, 0, 0, {}), rec(1, 0, 0, 0, {}), rec(2, 0, 0, 0, {}), rec(3, 0, 0, 0, {}), rec(4, 0, 0, 0, {})}));
  refused("count 255", payload(0x0100, 255, {}));
  refused("count 2, one record", payload(0x0100, 2, {rec(0, 0, 0, 0, kThree)}));
  refused("count 1, two records (a byte too long at 1.0)", payload(0x0100, 1, {rec(0, 0, 0, 0, kThree), rec(1, 0, 0, 0, {})}));
  {
    Bytes p = payload(0x0100, 1, {rec(0, 0, 0, 0, kThree)});
    p.push_back(0);
    refused("one trailing byte at 1.0", p);
    p.resize(p.size() - 3);
    refused("a parameter cut short", p);
  }
  refused("a record header cut short", payload(0x0100, 1, {Bytes{0, 0, 0}}));
  refused("n 33 with its bytes missing", payload(0x0100, 1, {Bytes{0, 0, 0, 0, 33, 0, 0, 0, 0}}));
  refused("version 0", payload(0x0000, 0, {}));
  refused("a payload shorter than the header", Bytes{0x01, 0x00});
  refused("INSX 2.0 is a newer major", payload(0x0200, 0, {}), Error::NewerMajor);
  // a chunk over its 535 bytes is refused at its header, in every case
  Bytes huge = payload(0x0100, 4, {});
  huge.resize(536, 0);
  refused("over 535 bytes at 1.0", huge);
}

TEST_CASE("insx: a newer minor is read as far as 1.0 knows it, with its appended bytes, and written back as 1.0") {
  const Bytes plain = saved();
  const Bytes appended = {0xAB, 0xCD, 0xEF, 0x01, 0x23};
  Bytes p = payload(0x0101, 3, {});
  for (const RawChunk& c : test::split_chunks(plain)) {
    if (c.id == kChunkInsx) {
      p.assign(c.payload.begin(), c.payload.end());
      p[1] = 0x01;  // 1.1
    }
  }
  p.insert(p.end(), appended.begin(), appended.end());
  const Outcome o = load1(build(with_payload(p)));
  CHECK(o.progress == Progress::Done);
  CHECK(o.newer);
  CHECK(proj(1).inserts().count == 3);
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == plain);  // the appended data is lost on the next save, as for every chunk
  // a newer minor shorter than the 1.0 minimum is refused; with count 0 and appended bytes it is fine
  CHECK(load1(build(with_payload(Bytes{0x01, 0x01}))).progress == Progress::Failed);
  Bytes z = payload(0x0107, 0, {});
  z.insert(z.end(), 100, 0x55);
  const Outcome oz = load1(build(with_payload(z)));
  CHECK(oz.progress == Progress::Done);
  CHECK(oz.newer);
  CHECK(proj(1).inserts().count == 0);
  // a newer minor still counts its 1.0 records strictly: count above the cap is Malformed
  CHECK(load1(build(with_payload(payload(0x0101, 5, {})))).progress == Progress::Failed);
}

TEST_CASE("insx: its place is after SNFX and LAW and before CRC; a second one, or one before NOTE, is refused") {
  const Bytes plain = saved();
  const std::vector<RawChunk> cs = test::split_chunks(plain);
  Bytes ins;
  for (const RawChunk& c : cs) {
    if (c.id == kChunkInsx) ins = c.payload;
  }
  std::vector<RawChunk> twice;
  std::vector<RawChunk> early;
  std::vector<RawChunk> after_law;
  std::vector<RawChunk> before_law;
  std::vector<RawChunk> after_unknown;
  for (const RawChunk& c : cs) {
    if (c.id == kChunkNote) early.push_back({kChunkInsx, ins});
    if (c.id == kChunkInsx) {
      before_law.push_back(c);
      before_law.push_back({kChunkLaw, {0x01, 0x00, 0x01}});  // INSX before LAW: a lower rank after a higher one
      after_law.push_back({kChunkLaw, {0x01, 0x00, 0x01}});
      after_law.push_back(c);
      twice.push_back(c);
      twice.push_back(c);
      continue;
    }
    early.push_back(c);
    twice.push_back(c);
    before_law.push_back(c);
    after_law.push_back(c);
    after_unknown.push_back(c);
  }
  after_unknown.push_back({kChunkInsx, ins});  // behind the unknown chunks: legal, as a build older than this one writes them
  CHECK(load1(build(twice)).progress == Progress::Failed);
  CHECK(load1(build(early)).progress == Progress::Failed);
  CHECK(load1(build(before_law)).progress == Progress::Failed);
  const Outcome al = load1(build(after_law));
  CHECK(al.progress == Progress::Done);
  CHECK(proj(1).volume_law() == 1);
  CHECK(proj(1).inserts().count == 3);
  const Outcome au = load1(build(after_unknown));
  CHECK(au.progress == Progress::Done);
  CHECK(proj(1).inserts().count == 3);
  // the writer puts LAW first and INSX after it
  test::fill_inserts(proj(0));
  proj(0).set_volume_law(1);
  Bytes both;
  NEED(test::save(proj(0), both) == Progress::Done);
  std::size_t law_at = 0, insx_at = 0, i = 0;
  for (const RawChunk& c : test::split_chunks(both)) {
    if (c.id == kChunkLaw) law_at = i;
    if (c.id == kChunkInsx) insx_at = i;
    ++i;
  }
  CHECK(insx_at == law_at + 1);
}

TEST_CASE("insx: the maximal chunk, four records of 32 parameters, is exactly 535 bytes and round trips") {
  Project& q = proj(0);
  test::fill_inserts(q);
  q.clear_inserts();
  for (unsigned i = 0; i < kMaxInserts; ++i) {
    InsertRecord r;
    std::memset(&r, 0, sizeof r);
    r.where = static_cast<std::uint8_t>(i * 5);  // 0, 5, 10, 15
    r.slot = static_cast<std::uint8_t>(i & 1u);
    r.module = static_cast<std::uint8_t>(i & 1u);
    r.flags = (i & 1u) != 0u ? kInsertFlagBypass : 0u;
    r.n = kInsertParamsMax;
    for (unsigned k = 0; k < kInsertParamsMax; ++k) r.param[k] = 0.125f * static_cast<float>(k) - static_cast<float>(i);
    NEED(q.set_insert(r) == Status::Ok);
  }
  CHECK(q.inserts().count == kMaxInserts);
  Bytes file;
  NEED(test::save(q, file) == Progress::Done);
  std::size_t size = 0;
  for (const RawChunk& c : test::split_chunks(file)) {
    if (c.id == kChunkInsx) size = c.payload.size();
  }
  CHECK(size == 535u);
  const Outcome o = load1(file);
  CHECK(o.progress == Progress::Done);
  CHECK(o.skipped == 0u);
  CHECK(std::memcmp(&proj(1).inserts(), &q.inserts(), sizeof(InsertState)) == 0);
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == file);
}

TEST_CASE("insx: the unknown pool of an older build that keeps the chunk has room for it") {
  // docs/specs/OS-INSFX.md section 7: an older build keeps INSX as an unknown chunk: one of 16, at most 535 bytes of the 16 KiB pool.
  CHECK(kUnknownPoolBytes >= 535u + 8u);
  CHECK(kMaxUnknownChunks >= 2u);
  // the model's maximum: four records of 32 parameters is the 535 bytes the format document quotes
  CHECK(3u + kMaxInserts * (5u + 4u * kInsertParamsMax) == 535u);
  CHECK(sizeof(InsertState) == 548u);
}
