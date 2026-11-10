// SPDX-License-Identifier: GPL-3.0-or-later
// OS-MIDIREC-P1 S1: hostile MEVT chunks. Random mutations of a good chunk (flipped bytes, bad counts, shuffled and duplicated lanes, cut
// payloads) and cut files go through the Reader with a lane store: it must end in Done or Failed, a Failed load leaves the empty default
// project, and whatever lanes a Done load keeps are valid, sorted, inside the limits, and save and load back to the same bytes. The sanitisers
// watch for out-of-bounds access. O3OS_FUZZ_SCALE multiplies the run counts like tests/native_fuzz_test.cpp.
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/rng.h"

#include <o3os/formats/native.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using namespace o3os::model;
using o3os::test::RawChunk;
using Bytes = std::vector<std::uint8_t>;

namespace {

LaneStore g_src;
LaneStore g_dst;
LaneStore g_again;

#if defined(__SANITIZE_THREAD__)
constexpr std::uint32_t kThin = 20;
#else
constexpr std::uint32_t kThin = 1;
#endif

std::uint32_t runs(std::uint32_t base) {
  const char* e = std::getenv("O3OS_FUZZ_SCALE");
  const long v = e != nullptr ? std::strtol(e, nullptr, 10) : 1;
  const std::uint32_t scale = v < 1 ? 1u : (v > 1000 ? 1000u : static_cast<std::uint32_t>(v));
  return base * scale / kThin;
}

// Lanes on the first patterns of the project, varied counts.
void fill(const Project& p) {
  g_src.clear();
  g_src.adopt(p.load_epoch());
  std::uint32_t n = 0;
  for (std::uint32_t id = 1; id <= kMaxPatternId && n < 24; ++id) {
    if (p.pattern(id) == nullptr) continue;
    for (std::uint32_t t = 0; t < 4 && n < 24; ++t, ++n) {
      const std::uint32_t count = 1 + (id * 5 + t * 11) % 40;
      LaneEvent* g = g_src.open_gap(id, t, 0, count);
      NEED(g != nullptr);
      for (std::uint32_t i = 0; i < count; ++i) {
        LaneEvent e{};
        e.tick = static_cast<std::uint16_t>(i * 100);
        e.len = (i & 1u) != 0 ? 0 : static_cast<std::uint16_t>(1 + i);
        e.kind_channel = (i & 1u) != 0 ? static_cast<std::uint8_t>(1u << 4 | t) : static_cast<std::uint8_t>(t);
        e.port = static_cast<std::uint8_t>(i & 3);
        e.a = static_cast<std::uint8_t>((id + i) & 127);
        e.b = static_cast<std::uint8_t>((i * 3) & 127);
        g[i] = e;
      }
    }
  }
}

// What a Done load promises about the store it filled.
void check_store(const LaneStore& s) {
  CHECK(s.lane_count() <= kMaxLanes);
  CHECK(s.total_events() <= kMaxLanePoolEvents);
  std::uint32_t lanes = 0;
  std::uint32_t events = 0;
  for (std::uint32_t k = s.next_key(0); k < LaneStore::kKeys; k = s.next_key(k + 1)) {
    const LaneStore::View v = s.lane_at(k);
    NEED(v.count != 0 && v.count <= kMaxLaneEvents);
    CHECK(lane_events_valid(v.events, v.count, 0, kLaneTicks));
    ++lanes;
    events += v.count;
  }
  CHECK(lanes == s.lane_count());
  CHECK(events == s.total_events());
}

void run_one(const Bytes& input) {
  g_dst.clear();
  Report rep;
  model::ModelArena& a = test::arena(1);
  const Progress pr = test::load(a, input, &rep, kDefaultStepBudget, &g_dst);
  NEED((pr == Progress::Done || pr == Progress::Failed));
  check_store(g_dst);
  if (pr == Progress::Failed) {
    CHECK(test::is_default_empty(a.project));
    return;
  }
  CHECK(a.project.check_invariants());
  // what it keeps saves, and loads back to the same lanes and bytes
  Bytes saved;
  NEED(test::save(a.project, saved, kDefaultStepBudget, &g_dst) == Progress::Done);
  g_again.clear();
  Report rep2;
  NEED(test::load(test::arena(2), saved, &rep2, kDefaultStepBudget, &g_again) == Progress::Done);
  CHECK(rep2.mevt_skipped == 0u);
  CHECK(g_again.lane_count() == g_dst.lane_count());
  CHECK(g_again.total_events() == g_dst.total_events());
  Bytes again;
  NEED(test::save(test::arena(2).project, again, kDefaultStepBudget, &g_again) == Progress::Done);
  CHECK(again == saved);
}

std::vector<RawChunk> chunks_of(const Bytes& file) { return test::split_chunks(file); }

Bytes with_payload(const std::vector<RawChunk>& cs, const Bytes& payload) {
  std::vector<RawChunk> out = cs;
  for (RawChunk& c : out) {
    if (c.id == kChunkMevt) c.payload = payload;
  }
  return test::build_file(out);
}

}  // namespace

TEST_CASE("mevt fuzz: mutated chunks end in Done or Failed and leave a consistent store") {
  test::fill_small(test::arena(0).project);
  fill(test::arena(0).project);
  Bytes good;
  NEED(test::save(test::arena(0).project, good, kDefaultStepBudget, &g_src) == Progress::Done);
  const std::vector<RawChunk> cs = chunks_of(good);
  const RawChunk* m = nullptr;
  for (const RawChunk& c : cs) {
    if (c.id == kChunkMevt) m = &c;
  }
  NEED(m != nullptr);
  const Bytes base = m->payload;
  run_one(good);

  test::Rng rng(0x4D455654u);
  const std::uint32_t n = runs(1500);
  for (std::uint32_t i = 0; i < n; ++i) {
    Bytes p = base;
    switch (rng.below(6)) {
      case 0:  // flip a few bytes
        for (std::uint32_t k = 1 + rng.below(4); k != 0; --k) p[rng.below(static_cast<std::uint32_t>(p.size()))] ^= static_cast<std::uint8_t>(1u << rng.below(8));
        break;
      case 1:  // overwrite a few bytes with anything
        for (std::uint32_t k = 1 + rng.below(4); k != 0; --k) p[rng.below(static_cast<std::uint32_t>(p.size()))] = rng.byte();
        break;
      case 2:  // cut the payload
        p.resize(rng.below(static_cast<std::uint32_t>(p.size())));
        break;
      case 3: {  // a bad lane count
        static const std::uint16_t counts[] = {0, 1, 2, 24, 25, 2047, 2048, 2049, 4096, 0xFFFF};
        const std::uint16_t c = counts[rng.below(10)];
        p[2] = static_cast<std::uint8_t>(c >> 8);
        p[3] = static_cast<std::uint8_t>(c);
        break;
      }
      case 4: {  // append garbage
        for (std::uint32_t k = rng.below(40); k != 0; --k) p.push_back(rng.byte());
        break;
      }
      default: {  // swap two bytes inside the first lanes: unsorted or duplicate keys and events
        const std::uint32_t lim = static_cast<std::uint32_t>(p.size() < 200 ? p.size() : 200);
        const std::uint32_t x = 4 + rng.below(lim - 4);
        const std::uint32_t y = 4 + rng.below(lim - 4);
        const std::uint8_t t = p[x];
        p[x] = p[y];
        p[y] = t;
        break;
      }
    }
    run_one(with_payload(cs, p));
  }
}

TEST_CASE("mevt fuzz: bad counts, unsorted and duplicate lanes, deterministic") {
  test::fill_small(test::arena(0).project);
  fill(test::arena(0).project);
  Bytes good;
  NEED(test::save(test::arena(0).project, good, kDefaultStepBudget, &g_src) == Progress::Done);
  const std::vector<RawChunk> cs = chunks_of(good);
  Bytes base;
  for (const RawChunk& c : cs) {
    if (c.id == kChunkMevt) base = c.payload;
  }
  NEED(base.size() > 20);

  // every cut of the payload, from the head to the whole
  for (std::size_t len = 0; len <= base.size(); len += (len < 64 ? 1 : 37)) {
    Bytes p(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(len));
    run_one(with_payload(cs, p));
  }
  // the first two lanes' keys exchanged: the second is below the expected key
  Bytes swapped = base;
  std::swap(swapped[4], swapped[4 + 6 + 8 * (base[6] << 8 | base[7])]);
  run_one(with_payload(cs, swapped));
  // the first lane's key repeated for the second
  Bytes dup = base;
  const std::size_t second = 4 + 6 + 8u * static_cast<std::size_t>(base[6] << 8 | base[7]);
  NEED(second + 2 < dup.size());
  dup[second] = dup[4];
  dup[second + 1] = dup[5];
  run_one(with_payload(cs, dup));
  // every event count in the first lane, with the data as it is
  for (std::uint32_t c : {0u, 1u, 2u, 4095u, 4096u, 4097u, 65535u}) {
    Bytes p = base;
    p[6] = static_cast<std::uint8_t>(c >> 8);
    p[7] = static_cast<std::uint8_t>(c);
    run_one(with_payload(cs, p));
  }
  // every event size
  for (std::uint32_t s = 0; s < 16; ++s) {
    Bytes p = base;
    p[8] = static_cast<std::uint8_t>(s);
    run_one(with_payload(cs, p));
  }
}

TEST_CASE("mevt fuzz: a cut or flipped file ends in Done or Failed") {
  test::fill_small(test::arena(0).project);
  fill(test::arena(0).project);
  Bytes good;
  NEED(test::save(test::arena(0).project, good, kDefaultStepBudget, &g_src) == Progress::Done);
  test::Rng rng(0x6C616E65u);
  const std::uint32_t n = runs(300);
  for (std::uint32_t i = 0; i < n; ++i) {
    Bytes b = good;
    if (rng.below(2) == 0) {
      b.resize(rng.below(static_cast<std::uint32_t>(b.size())));
    } else {
      b[rng.below(static_cast<std::uint32_t>(b.size()))] ^= static_cast<std::uint8_t>(1u << rng.below(8));
    }
    run_one(b);
  }
}
