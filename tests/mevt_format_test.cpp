// SPDX-License-Identifier: GPL-3.0-or-later
// OS-MIDIREC-P1 S1 (docs/formats/o3-project.md, "The MEVT chunk"): the MIDI take lanes in the native project. A project with no lane is
// the file it was (byte pin); the chunk round trips (empty store, one lane, 2048 lanes, a full pool); the exact bytes of the chunk are
// written here from the description, not by the writer; a lane the reader cannot use is skipped and counted and the project opens; the shape
// errors are Malformed; a build without a store (the old reader) keeps a chunk of 16 KiB or less as unknown and drops a bigger one. The Python
// reference and the same old-reader simulation are tests/o3project_roundtrip.py; the random mutations are tests/mevt_fuzz_test.cpp.
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/memory_io.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/formats/native.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using namespace o3os::model;
using o3os::test::RawChunk;
using Bytes = std::vector<std::uint8_t>;

namespace {

LaneStore g_a;  // the store that is saved
LaneStore g_b;  // the store that is loaded into

Project& proj(int i = 0) { return test::arena(i).project; }

// A deterministic valid lane of `n` events (n <= 6144): notes and controllers with rising ticks.
void make_events(std::uint32_t seed, std::uint32_t n, std::vector<LaneEvent>& out) {
  out.clear();
  for (std::uint32_t i = 0; i < n; ++i) {
    LaneEvent e{};
    e.tick = static_cast<std::uint16_t>(i * 6144u / (n != 0 ? n : 1u));
    if (i % 3 == 0) {
      e.len = static_cast<std::uint16_t>(1 + (seed + i) % 48);
      e.kind_channel = static_cast<std::uint8_t>(seed % 16);
    } else {
      e.len = 0;
      e.kind_channel = static_cast<std::uint8_t>((1 + i % 5) << 4 | (seed % 16));
    }
    e.port = static_cast<std::uint8_t>(i % 4);
    e.a = static_cast<std::uint8_t>((seed * 7 + i) % 128);
    e.b = static_cast<std::uint8_t>((seed + i * 3) % 128);
    out.push_back(e);
  }
}

// Puts `per` events on every track of the first `max_lanes` lanes of the project's patterns (ids ascending), into an empty store.
std::uint32_t fill_lanes(const Project& p, LaneStore& s, std::uint32_t max_lanes, std::uint32_t per) {
  s.clear();
  s.adopt(p.load_epoch());
  std::vector<LaneEvent> ev;
  std::uint32_t made = 0;
  for (std::uint32_t id = 1; id <= kMaxPatternId && made < max_lanes && per != 0; ++id) {
    if (p.pattern(id) == nullptr) continue;
    for (std::uint32_t t = 0; t < kMaxTracks && made < max_lanes; ++t) {
      make_events(id * 16 + t, per, ev);
      LaneEvent* g = s.open_gap(id, t, 0, per);
      if (g == nullptr) return made;
      std::memcpy(g, ev.data(), per * sizeof(LaneEvent));
      ++made;
    }
  }
  return made;
}

bool same_stores(const LaneStore& x, const LaneStore& y) {
  if (x.lane_count() != y.lane_count() || x.total_events() != y.total_events()) return false;
  for (std::uint32_t k = x.next_key(0); k < LaneStore::kKeys; k = x.next_key(k + 1)) {
    const LaneStore::View a = x.lane_at(k);
    const LaneStore::View b = y.lane_at(k);
    if (a.count != b.count || std::memcmp(a.events, b.events, a.count * sizeof(LaneEvent)) != 0) return false;
  }
  return true;
}

const RawChunk* find_chunk(const std::vector<RawChunk>& cs, std::uint32_t id) {
  for (const RawChunk& c : cs) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

// save, load into another arena and store, compare, save again: the same bytes.
void roundtrip(const Project& p, const LaneStore& s, std::uint32_t want_lanes, std::uint32_t want_events) {
  Bytes bytes;
  NEED(test::save(p, bytes, kDefaultStepBudget, &s) == Progress::Done);
  const std::vector<RawChunk> cs = test::split_chunks(bytes);
  const RawChunk* m = find_chunk(cs, kChunkMevt);
  if (want_lanes == 0) {
    CHECK(m == nullptr);
  } else {
    NEED(m != nullptr);
    CHECK(m->payload.size() == 4u + want_lanes * 6u + want_events * 8u);
  }
  g_b.clear();
  Report rep;
  NEED(test::load(test::arena(1), bytes, &rep, kDefaultStepBudget, &g_b) == Progress::Done);
  CHECK(rep.mevt_skipped == 0u);
  CHECK(g_b.in_sync(test::arena(1).project.load_epoch()));
  CHECK(g_b.lane_count() == want_lanes);
  CHECK(g_b.total_events() == want_events);
  CHECK(same_stores(s, g_b));
  Bytes again;
  NEED(test::save(test::arena(1).project, again, kDefaultStepBudget, &g_b) == Progress::Done);
  CHECK(again == bytes);
  // a Reader without a store reads the same file and keeps nothing of the lanes but the unknown chunk it may keep
  Report old;
  CHECK(test::load(test::arena(2), bytes, &old) == Progress::Done);
}

// ---- the chunk as the description gives it -----------------------------------------------------------------------------------

void be16(Bytes& b, std::uint32_t x) { test::put_be16(b, x); }

Bytes event_bytes(const LaneEvent& e) {
  Bytes b;
  be16(b, e.tick);
  be16(b, e.len);
  b.push_back(e.kind_channel);
  b.push_back(e.port);
  b.push_back(e.a);
  b.push_back(e.b);
  return b;
}

struct Lane {
  unsigned pattern;
  unsigned track;
  std::vector<LaneEvent> ev;
  unsigned size = 8;    // the event size byte
  unsigned count = ~0u;  // event_count when it is not ev.size()
};

Bytes mevt(const std::vector<Lane>& lanes, unsigned version = 0x0100, int lane_count = -1) {
  Bytes b;
  be16(b, version);
  be16(b, lane_count < 0 ? static_cast<std::uint32_t>(lanes.size()) : static_cast<std::uint32_t>(lane_count));
  for (const Lane& l : lanes) {
    b.push_back(static_cast<std::uint8_t>(l.pattern));
    b.push_back(static_cast<std::uint8_t>(l.track));
    be16(b, l.count != ~0u ? l.count : static_cast<std::uint32_t>(l.ev.size()));
    b.push_back(static_cast<std::uint8_t>(l.size));
    b.push_back(0);
    for (const LaneEvent& e : l.ev) {
      const Bytes eb = event_bytes(e);
      b.insert(b.end(), eb.begin(), eb.begin() + (l.size < 8 ? l.size : 8));
      for (unsigned i = 8; i < l.size; ++i) b.push_back(0xEE);
    }
  }
  return b;
}

// The chunks of the small project saved with one lane, MEVT's payload replaced (or the chunk dropped when `drop`).
std::vector<RawChunk> small_with(const Bytes& payload, bool drop = false) {
  Bytes bytes;
  fill_lanes(proj(0), g_a, 1, 1);
  CHECK(test::save(proj(0), bytes, kDefaultStepBudget, &g_a) == Progress::Done);
  std::vector<RawChunk> out;
  for (const RawChunk& c : test::split_chunks(bytes)) {
    if (c.id != kChunkMevt) {
      out.push_back(c);
    } else if (!drop) {
      out.push_back({kChunkMevt, payload});
    }
  }
  return out;
}

std::uint32_t first_pattern() {
  for (std::uint32_t id = 1; id <= kMaxPatternId; ++id) {
    if (proj(0).pattern(id) != nullptr) return id;
  }
  return 0;
}

// Loads `payload` as the MEVT chunk of the small project; returns the progress, the report and the store content.
Progress load_payload(const Bytes& payload, Report& rep) {
  g_b.clear();
  return test::load(test::arena(1), test::build_file(small_with(payload)), &rep, kDefaultStepBudget, &g_b);
}

}  // namespace

TEST_CASE("mevt: a project with no lane is the file it was") {
  test::fill_small(proj(0));
  Bytes plain;
  Bytes with_null;
  Bytes with_empty;
  CHECK(test::save(proj(0), plain) == Progress::Done);
  CHECK(test::save(proj(0), with_null, kDefaultStepBudget, nullptr) == Progress::Done);
  g_a.clear();
  g_a.adopt(proj(0).load_epoch());
  CHECK(test::save(proj(0), with_empty, kDefaultStepBudget, &g_a) == Progress::Done);
  CHECK(plain == with_null);
  CHECK(plain == with_empty);
  CHECK(find_chunk(test::split_chunks(plain), kChunkMevt) == nullptr);
  // a store of another epoch is empty to the writer, even when it holds lanes
  fill_lanes(proj(0), g_a, 3, 5);
  g_a.adopt(proj(0).load_epoch() + 1);
  Bytes stale;
  CHECK(test::save(proj(0), stale, kDefaultStepBudget, &g_a) == Progress::Done);
  CHECK(plain == stale);
}

TEST_CASE("mevt: round trips, empty, one lane, 2048 lanes and a full pool") {
  test::fill_small(proj(0));
  g_a.clear();
  g_a.adopt(proj(0).load_epoch());
  roundtrip(proj(0), g_a, 0, 0);

  NEED(fill_lanes(proj(0), g_a, 1, 4096) == 1u);
  roundtrip(proj(0), g_a, 1, 4096);
  CHECK(g_b.lane(first_pattern(), 0).count == 4096u);

  fill_lanes(proj(0), g_a, 5, 7);
  roundtrip(proj(0), g_a, 5, 35);

  test::fill_maximal(proj(0));
  NEED(fill_lanes(proj(0), g_a, 2048, 1) == 2048u);
  roundtrip(proj(0), g_a, 2048, 2048);

  NEED(fill_lanes(proj(0), g_a, 1920, 4) == 1920u);  // the pool exactly full (OS-MIDIREC-P1 decision 2026-11-10: 7,680 events)
  CHECK(g_a.total_events() == 7680u);
  roundtrip(proj(0), g_a, 1920, 7680);
}

TEST_CASE("mevt: the chunk is written as described, after INSX and before the unknown chunks and the CRC") {
  test::fill_small(proj(0));
  const std::uint32_t id = first_pattern();
  NEED(id != 0);
  g_a.clear();
  g_a.adopt(proj(0).load_epoch());
  std::vector<LaneEvent> ev;
  make_events(3, 2, ev);
  LaneEvent* g = g_a.open_gap(id, 2, 0, 2);
  NEED(g != nullptr);
  g[0] = ev[0];
  g[1] = ev[1];
  Bytes bytes;
  NEED(test::save(proj(0), bytes, kDefaultStepBudget, &g_a) == Progress::Done);
  const std::vector<RawChunk> cs = test::split_chunks(bytes);
  const RawChunk* m = find_chunk(cs, kChunkMevt);
  NEED(m != nullptr);
  const Bytes want = mevt({Lane{id, 2, ev}});
  CHECK(m->payload == want);
  CHECK(want.size() == 4u + 6u + 16u);
  // order: everything the file had before is still before it, and what followed is unknown chunks only
  std::size_t at = 0;
  while (cs[at].id != kChunkMevt) ++at;
  CHECK(at > 0);
  CHECK(cs[at - 1].id != kChunkMevt);
  // the file without the lane is the file with the chunk taken out
  Bytes plain;
  fill_lanes(proj(0), g_a, 0, 0);
  CHECK(test::save(proj(0), plain, kDefaultStepBudget, &g_a) == Progress::Done);
  std::vector<RawChunk> without;
  for (const RawChunk& c : cs) {
    if (c.id != kChunkMevt) without.push_back(c);
  }
  CHECK(test::build_file(without) == plain);
}

TEST_CASE("mevt: a lane the reader cannot use is skipped and counted, the project opens") {
  test::fill_small(proj(0));
  const unsigned id = first_pattern();
  NEED(id != 0);
  std::vector<LaneEvent> two;
  make_events(1, 2, two);
  std::vector<LaneEvent> ok;
  make_events(2, 3, ok);
  Report rep;

  // a good lane after a bad one is still taken
  auto skipped_one = [&](const std::vector<Lane>& lanes, std::uint32_t lanes_left) {
    rep = Report{};
    NEED(load_payload(mevt(lanes), rep) == Progress::Done);
    CHECK(rep.mevt_skipped == 1u);
    CHECK(g_b.lane_count() == lanes_left);
  };

  skipped_one({Lane{id, 1, two}, Lane{id, 0, ok}}, 1);       // unsorted keys: the second is below the expected key
  skipped_one({Lane{id, 1, two}, Lane{id, 1, ok}}, 1);       // a duplicate key
  unsigned absent = kMaxPatternId;  // the last ID is taken only by fill_maximal
  CHECK(proj(0).pattern(absent) == nullptr);
  skipped_one({Lane{id, 0, ok}, Lane{absent, 1, ok}}, 1);  // a pattern the project does not have
  skipped_one({Lane{0, 0, ok}}, 0);                          // pattern 0
  skipped_one({Lane{id, 16, ok}}, 0);                        // track 16
  skipped_one({Lane{id, 0, {}}}, 0);                         // no events
  Lane tiny{id, 0, two};
  tiny.size = 7;
  skipped_one({tiny}, 0);  // an event size below 8
  skipped_one({Lane{id, 0, std::vector<LaneEvent>(4097, two[0])}}, 0);

  LaneEvent bad = two[0];
  bad.a = 128;
  skipped_one({Lane{id, 0, {bad, two[1]}}, Lane{id, 1, ok}}, 1);  // an invalid event drops its whole lane
  skipped_one({Lane{id, 0, {two[1], two[0]}}, Lane{id, 1, ok}}, 1);  // events out of tick order

  // the lane that was skipped is not in the store
  rep = Report{};
  NEED(load_payload(mevt({Lane{id, 0, {bad}}, Lane{id, 1, ok}}), rep) == Progress::Done);
  CHECK(g_b.lane(id, 0).count == 0u);
  CHECK(g_b.lane(id, 1).count == 3u);

  // a pool that has no room for the next lane
  std::vector<LaneEvent> full;
  make_events(5, 4096, full);
  std::vector<Lane> three;  // the pool holds 7,680: one lane of 4,096 fits, the second and third do not
  for (unsigned t = 0; t < 3; ++t) three.push_back(Lane{id, t, full});
  rep = Report{};
  NEED(load_payload(mevt(three), rep) == Progress::Done);  // never a failed load
  CHECK(rep.mevt_skipped == 2u);
  CHECK(g_b.lane_count() == 1u);
  CHECK(g_b.total_events() == 4096u);
}

TEST_CASE("mevt: the reader's tolerances") {
  test::fill_small(proj(0));
  const unsigned id = first_pattern();
  NEED(id != 0);
  std::vector<LaneEvent> ev;
  make_events(4, 3, ev);
  Report rep;

  // a wider event: the first eight bytes of each
  Lane wide{id, 0, ev};
  wide.size = 10;
  NEED(load_payload(mevt({wide}), rep) == Progress::Done);
  CHECK(rep.mevt_skipped == 0u);
  CHECK(g_b.lane(id, 0).count == 3u);
  CHECK(std::memcmp(g_b.lane(id, 0).events, ev.data(), 3 * sizeof(LaneEvent)) == 0);

  // trailing bytes after the last lane
  Bytes tail = mevt({Lane{id, 0, ev}});
  tail.push_back(1);
  tail.push_back(2);
  rep = Report{};
  NEED(load_payload(tail, rep) == Progress::Done);
  CHECK(g_b.lane(id, 0).count == 3u);

  // a newer minor with appended bytes: read as far as 1.0 knows it, written back as 1.0
  Bytes newer = mevt({Lane{id, 0, ev}}, 0x0101);
  newer.push_back(9);
  newer.push_back(9);
  rep = Report{};
  NEED(load_payload(newer, rep) == Progress::Done);
  CHECK(g_b.lane(id, 0).count == 3u);
  Bytes resaved;
  NEED(test::save(proj(1), resaved, kDefaultStepBudget, &g_b) == Progress::Done);
  const std::vector<RawChunk> resaved_chunks = test::split_chunks(resaved);
  const RawChunk* m = find_chunk(resaved_chunks, kChunkMevt);
  NEED(m != nullptr);
  CHECK(m->payload == mevt({Lane{id, 0, ev}}));

  // an empty lane list is no chunk on the next save
  rep = Report{};
  NEED(load_payload(mevt({}), rep) == Progress::Done);
  CHECK(g_b.lane_count() == 0u);
  Bytes none;
  NEED(test::save(proj(1), none, kDefaultStepBudget, &g_b) == Progress::Done);
  CHECK(find_chunk(test::split_chunks(none), kChunkMevt) == nullptr);
}

TEST_CASE("mevt: shape errors are Malformed, a newer major is refused, a chunk out of place is refused") {
  test::fill_small(proj(0));
  const unsigned id = first_pattern();
  NEED(id != 0);
  std::vector<LaneEvent> ev;
  make_events(4, 3, ev);
  Report rep;
  const Bytes good = mevt({Lane{id, 0, ev}});

  auto fails = [&](const Bytes& payload) {
    rep = Report{};
    const Progress pr = load_payload(payload, rep);
    CHECK(pr == Progress::Failed);
    CHECK(rep.error != Error::None);
    CHECK(test::is_default_empty(test::arena(1).project));
  };

  fails(mevt({Lane{id, 0, ev}}, 0x0200));                      // a newer major
  fails(mevt({Lane{id, 0, ev}}, 0x0000));                      // version 0
  fails(Bytes{0x01});                                          // shorter than the head
  fails(Bytes{0x01, 0x00, 0x00});                              // the lane count is cut
  fails(mevt({}, 0x0100, 2049));                               // more lanes than a project holds
  fails(mevt({Lane{id, 0, ev}}, 0x0100, 2));                   // more lanes than there are
  Bytes cut = good;
  cut.resize(cut.size() - 1);
  fails(cut);                                                  // the last event is cut
  cut = good;
  cut.resize(4 + 3);
  fails(cut);                                                  // a lane header is cut

  // twice
  std::vector<RawChunk> cs = small_with(good);
  std::vector<RawChunk> twice;
  for (const RawChunk& c : cs) {
    twice.push_back(c);
    if (c.id == kChunkMevt) twice.push_back(c);
  }
  rep = Report{};
  g_b.clear();
  CHECK(test::load(test::arena(1), test::build_file(twice), &rep, kDefaultStepBudget, &g_b) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);

  // before a chunk it must follow
  std::vector<RawChunk> early;
  early.push_back(cs.front());
  for (const RawChunk& c : cs) {
    if (c.id == kChunkMevt) early.push_back(c);
  }
  for (std::size_t i = 1; i < cs.size(); ++i) {
    if (cs[i].id != kChunkMevt) early.push_back(cs[i]);
  }
  rep = Report{};
  g_b.clear();
  CHECK(test::load(test::arena(1), test::build_file(early), &rep, kDefaultStepBudget, &g_b) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
}

TEST_CASE("mevt: an older build keeps a chunk of 16 KiB or less as unknown and writes it back, and drops a bigger one") {
  test::fill_small(proj(0));
  const unsigned id = first_pattern();
  NEED(id != 0);
  Bytes plain;
  NEED(test::save(proj(0), plain) == Progress::Done);
  Report base;
  NEED(test::load(test::arena(1), plain, &base) == Progress::Done);

  // 200 events: 4 + 6 + 1600 bytes
  fill_lanes(proj(0), g_a, 1, 200);
  Bytes small_file;
  NEED(test::save(proj(0), small_file, kDefaultStepBudget, &g_a) == Progress::Done);
  Report rep;
  NEED(test::load(test::arena(1), small_file, &rep) == Progress::Done);  // no store: the old reader
  CHECK(rep.mevt_skipped == 0u);
  CHECK(rep.unknown_kept == base.unknown_kept + 1u);
  CHECK(rep.unknown_dropped == base.unknown_dropped);
  Bytes back;
  NEED(test::save(test::arena(1).project, back) == Progress::Done);
  CHECK(back == small_file);  // byte for byte: MEVT kept its place in front of the older unknown chunks

  // 4096 events: 32,778 bytes, more than the 16 KiB pool
  fill_lanes(proj(0), g_a, 1, 4096);
  Bytes big_file;
  NEED(test::save(proj(0), big_file, kDefaultStepBudget, &g_a) == Progress::Done);
  rep = Report{};
  NEED(test::load(test::arena(1), big_file, &rep) == Progress::Done);
  CHECK(rep.unknown_kept == base.unknown_kept);
  CHECK(rep.unknown_dropped == base.unknown_dropped + 1u);
  back.clear();
  NEED(test::save(test::arena(1).project, back) == Progress::Done);
  CHECK(back == plain);  // the lane is gone, the rest is the file it was

  // the same chunk read by a build with a store is lanes, not an unknown chunk
  g_b.clear();
  rep = Report{};
  NEED(test::load(test::arena(1), big_file, &rep, kDefaultStepBudget, &g_b) == Progress::Done);
  CHECK(rep.unknown_kept == base.unknown_kept);
  CHECK(rep.unknown_dropped == base.unknown_dropped);
  CHECK(g_b.total_events() == 4096u);
}

TEST_CASE("mevt: the model's limits are the file's") {
  // 2 + 2 + 2048 lanes x (6 + 4096 x 8) would not fit the pool: the pool caps the chunk
  CHECK(4u + kMaxLanes * 6u + kMaxLanePoolEvents * 8u == 4u + 12288u + 61440u);
  CHECK(kMaxLanePoolEvents * sizeof(LaneEvent) == 60u * 1024u);
  CHECK(LaneStore::kKeys == 4080u);
  // the biggest file (every arena slot full, every lane, the whole pool) is saved and fits the reader's size limit
  test::fill_maximal(proj(0));
  NEED(fill_lanes(proj(0), g_a, 1920, 4) == 1920u);
  CHECK(g_a.total_events() == 7680u);
  Bytes biggest;
  NEED(test::save(proj(0), biggest, kDefaultStepBudget, &g_a) == Progress::Done);
  CHECK(biggest.size() < kMaxFileBytes);
  g_b.clear();
  Report rep;
  NEED(test::load(test::arena(1), biggest, &rep, kDefaultStepBudget, &g_b) == Progress::Done);
  CHECK(g_b.total_events() == 7680u);
}
