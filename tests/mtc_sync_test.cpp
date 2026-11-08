// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MTC A3: app::MtcSync, the main loop's answer to the time code generator's question (app/mtc_sync.h): the request word decoded, the song time by the A1 map, the MtcAnchor command, a stamp
// answered once, a position the song never plays left unanswered, a full command ring retried; and the loop end to end with a Sequencer (a start in the middle of a song).
#include <doctest/doctest.h>

#include <o3os/app/mtc_sync.h>
#include <o3os/midi/mtc.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace mtc = o3os::midi::mtc;

namespace {

constexpr bool kRate44100 = core::kRefCyclesPerSample.num == 1800000u && core::kRefCyclesPerSample.den == 441u;

constexpr std::uint32_t word(std::uint32_t stamp, std::uint32_t order, std::uint32_t pattern, std::uint32_t step) { return (stamp << 24) | (order << 16) | (pattern << 8) | step; }

void project() {
  fresh_project(4, 4);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 16) == model::Status::Ok);
  CHECK(p.append_order(1) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
}

}  // namespace

TEST_CASE("mtc sync: no request, no answer; a request is answered once with the song time of its position") {
  if (!kRate44100) {
    return;
  }
  project();
  seq::CommandRing ring;
  app::MtcSync sync;
  CHECK_FALSE(sync.step(seq_project(), 0u, ring));  // stamp 0: nothing asked yet
  CHECK(ring.size() == 0u);
  // Song mode: order 1, step 4: slot 1's 16 steps (88,200 samples) plus 4 steps (22,050)
  CHECK(sync.step(seq_project(), word(7, 1, 2, 4), ring));
  seq::Command c{};
  NEED(ring.pop(c));
  CHECK(c.kind == seq::Command::Kind::MtcAnchor);
  CHECK(c.a == 7u);
  CHECK(c.c == 88200u + 22050u);
  CHECK(sync.stats().answered == 1u);
  CHECK(sync.last_samples() == 110250u);
  CHECK_FALSE(sync.step(seq_project(), word(7, 1, 2, 4), ring));  // the same stamp: answered, not again
  CHECK(ring.size() == 0u);
  // Pattern mode (order 0xFF): the time counts from the pattern's step 0
  CHECK(sync.step(seq_project(), word(8, 0xFF, 2, 8), ring));
  NEED(ring.pop(c));
  CHECK(c.a == 8u);
  CHECK(c.c == 44100u);
  // the stamp wraps over 0: 255 then 1
  CHECK(sync.step(seq_project(), word(255, 0, 1, 0), ring));
  NEED(ring.pop(c));
  CHECK(c.a == 255u);
  CHECK(c.c == 0u);
  CHECK(sync.step(seq_project(), word(1, 1, 2, 0), ring));
  NEED(ring.pop(c));
  CHECK(c.a == 1u);
  CHECK(c.c == 88200u);
}

TEST_CASE("mtc sync: a position the song never plays is not answered; a full ring is retried on the next pass") {
  if (!kRate44100) {
    return;
  }
  project();
  seq::CommandRing ring;
  app::MtcSync sync;
  CHECK_FALSE(sync.step(seq_project(), word(3, 9, 1, 0), ring));  // order 9 of a 2-slot song
  CHECK(sync.stats().capped == 1u);
  CHECK(ring.size() == 0u);
  CHECK_FALSE(sync.step(seq_project(), word(3, 9, 1, 0), ring));  // said once
  CHECK(sync.stats().capped == 1u);
  CHECK_FALSE(sync.step(seq_project(), word(4, 0xFF, 77, 0), ring));  // a pattern that does not exist
  CHECK(sync.stats().capped == 2u);
  // fill the ring (32 commands), the answer finds no room
  while (ring.push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0})) {
  }
  CHECK_FALSE(sync.step(seq_project(), word(5, 1, 2, 0), ring));
  CHECK(sync.stats().retried == 1u);
  CHECK(sync.stats().answered == 0u);
  seq::Command c{};
  NEED(ring.pop(c));  // one slot free
  CHECK(sync.step(seq_project(), word(5, 1, 2, 0), ring));
  CHECK(sync.stats().answered == 1u);
}

TEST_CASE("mtc sync: end to end, a Play from the middle of a song gets its time code from the main loop's answer") {
  if (!kRate44100) {
    return;
  }
  project();
  seq::Sequencer::Config cfg;
  cfg.emit_mtc = true;
  cfg.mtc_fps = 25;
  cfg.mtc_hour = 1;
  cfg.start_lead = 64;
  seq::Sequencer& s = seq_slot().make(seq_project());
  s.configure(cfg);
  Collector& col = seq_collector();
  col.n = 0;
  col.total_events = 0;
  app::MtcSync sync;
  s.play_song(1, 4);  // slot 2, step 5: song time 110,250 samples
  std::uint64_t t = 0;
  for (unsigned block = 0; block < 800; ++block) {
    col.run(s, t, t + 64, 64);
    t += 64;
    (void)sync.step(seq_project(), s.mtc_request(), s.commands());  // the main loop, once a pass
  }
  const Rec* full = col.nth(EventKind::MidiMtcFull, 0);
  NEED(full != nullptr);
  CHECK(full->t <= 192u);  // the answer reaches the render one block after it asked
  // the time code at that sample: 1 h + (110,250 + (t - 64)) samples
  const std::uint64_t song = 110250u + (full->t - 64u);
  const std::uint64_t frame = 90000u + song * 25u / 44100u;
  CHECK(mtc::unpack_time(static_cast<std::uint32_t>(full->ev.value)) == mtc::time_of_frame(frame, 25));
  CHECK(col.count(EventKind::MidiMtcQf) > 100u);
  // every quarter frame is on the song's grid: 441 samples apart, from the step's own sample
  for (std::uint32_t k = 0; k < col.count(EventKind::MidiMtcQf); ++k) {
    const Rec* q = col.nth(EventKind::MidiMtcQf, k);
    NEED(q != nullptr);
    CHECK((q->t - 64u + 110250u) % 441u == 0u);
  }
}
