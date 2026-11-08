// SPDX-License-Identifier: GPL-3.0-or-later
//
// E-SONG-07: edits of the song's order list while the song plays keep the playhead on its slot. The Project journals every change of the list (Project::OrderEdit, an odd epoch while it runs);
// the sequencer keeps its own copy of the list and carries its song positions (the playing slot, the queued jump) over the edits made since the last block. Host only for the thread case.
#include <doctest/doctest.h>

#include <o3os/model/editor.h>
#include <o3os/model/undo.h>
#include <o3os/seq/sequencer.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;

namespace {

std::uint64_t exact(std::uint64_t ticks) {
  const std::uint64_t n = ticks * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t d = std::uint64_t{4u} * core::kRefCyclesPerSample.num * 24000u;  // 64-bit: 4 * 1,800,000 * 24,000 overflows 32 bits at the 44,100 Hz clock
  return (2u * n + d) / (2u * d);
}
// 240 BPM: a step is 48 ticks; every pattern is 4 steps, so slot k of the list plays steps 4k..4k+3 and the song's pattern events come every 4 steps.
std::uint64_t at_step(std::uint32_t step, std::uint32_t j = 0) { return exact(48u * step + j); }

struct Landing {
  std::uint64_t t;
  std::uint32_t pattern;
  std::uint32_t order;
};

// A sequencer over the shared project, run block by block with the pattern events kept.
class Rig {
 public:
  explicit Rig(std::uint32_t patterns = 9) {
    fresh_project(4, 4);
    model::Project& p = seq_project();
    CHECK(p.set_tempo_x100(24000) == model::Status::Ok);
    for (std::uint32_t id = 1; id <= patterns; ++id) {
      CHECK(p.create_pattern(id, 4) == model::Status::Ok);
    }
    s_ = &seq_slot().make(p);
    s_->set_song_loop(true);
  }
  seq::Sequencer& s() { return *s_; }
  model::Project& p() { return seq_project(); }
  void song(std::initializer_list<std::uint32_t> ids) {
    for (std::uint32_t id : ids) {
      CHECK(p().append_order(id) == model::Status::Ok);
    }
  }
  void to(std::uint64_t end) {
    while (t_ < end) {
      const std::uint32_t n = static_cast<std::uint32_t>(end - t_ < 64 ? end - t_ : 64);
      const std::uint32_t c = s_->step(t_, n, out_, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        if (out_[i].kind == EventKind::Pattern && n_ < kCap) {
          land_[n_++] = Landing{t_ + out_[i].offset, out_[i].a, audio::event_order_pos(out_[i])};
        }
      }
      t_ += n;
    }
  }
  // One block, whatever the time (an edit is taken at the block start).
  void block() { to(t_ + 1); }
  // The first pattern event after the mark.
  const Landing* next(std::uint32_t after) const { return after < n_ ? &land_[after] : nullptr; }
  std::uint32_t mark() const { return n_; }
  std::uint32_t order() const { return s_->play_position().read().order; }
  std::uint32_t queued() const { return s_->play_position().read().queued; }
  std::uint64_t now() const { return t_; }

 private:
  static constexpr std::uint32_t kCap = 512;
  seq::Sequencer* s_ = nullptr;
  std::uint64_t t_ = 0;
  audio::SeqEvent out_[audio::kMaxEventsPerBlock];
  Landing land_[kCap];
  std::uint32_t n_ = 0;
};

constexpr std::uint32_t kNoQueue = audio::PlayPosition::kNoOrder;

bool queue(Rig& r, std::uint32_t pos, std::uint32_t stamp) {
  return r.s().commands().push(seq::Command{seq::Command::Kind::QueueJump, 0, static_cast<std::uint16_t>(pos), stamp});
}

}  // namespace

TEST_CASE("song order sync: an insert above the playhead shifts it and the next slot is still the old next one") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.to(at_step(9, 5));  // in slot 2 (pattern 3)
  CHECK(r.order() == 2u);
  CHECK(r.p().insert_order(0, 9) == model::Status::Ok);
  const std::uint32_t m = r.mark();
  r.to(at_step(10));
  CHECK(r.order() == 3u);  // the same slot, one index down
  r.to(at_step(13));
  NEED(r.next(m) != nullptr);
  CHECK(r.next(m)->pattern == 4u);
  CHECK(r.next(m)->order == 4u);
  CHECK(r.next(m)->t == at_step(12));
}

TEST_CASE("song order sync: an insert at the playhead's own index shifts it, an insert below leaves it") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.to(at_step(9, 5));
  CHECK(r.p().insert_order(2, 9) == model::Status::Ok);  // at the playing slot's index: it moves down
  r.block();
  CHECK(r.order() == 3u);
  const std::uint32_t m = r.mark();
  r.to(at_step(13));
  NEED(r.next(m) != nullptr);
  CHECK(r.next(m)->pattern == 4u);

  Rig q;
  q.song({1, 2, 3, 4});
  q.s().play_song(0);
  q.to(at_step(9, 5));
  CHECK(q.p().insert_order(3, 9) == model::Status::Ok);  // below: the playhead stays and the new slot is next
  q.block();
  CHECK(q.order() == 2u);
  const std::uint32_t m2 = q.mark();
  q.to(at_step(13));
  NEED(q.next(m2) != nullptr);
  CHECK(q.next(m2)->pattern == 9u);
  CHECK(q.next(m2)->order == 3u);
}

TEST_CASE("song order sync: a delete above the playhead shifts it down") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.to(at_step(9, 5));
  CHECK(r.p().remove_order(0) == model::Status::Ok);
  r.block();
  CHECK(r.order() == 1u);
  const std::uint32_t m = r.mark();
  r.to(at_step(13));
  NEED(r.next(m) != nullptr);
  CHECK(r.next(m)->pattern == 4u);
  CHECK(r.next(m)->order == 2u);
}

TEST_CASE("song order sync: a delete of the playing slot lets it finish and plays the slot that slid in, once") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.to(at_step(9, 5));
  CHECK(r.p().remove_order(2) == model::Status::Ok);  // [1 2 4]
  r.to(at_step(11));
  CHECK(r.s().playing());
  const std::uint32_t m = r.mark();
  r.to(at_step(13));
  NEED(r.next(m) != nullptr);
  CHECK(r.next(m)->pattern == 4u);  // the slot that slid into the place, not the one after it
  CHECK(r.next(m)->order == 2u);
  CHECK(r.next(m)->t == at_step(12));
  r.to(at_step(17));
  NEED(r.next(m + 1u) != nullptr);
  CHECK(r.next(m + 1u)->pattern == 1u);  // then the list ends and loops: the slot is not played twice
  CHECK(r.next(m + 1u)->order == 0u);
}

TEST_CASE("song order sync: a delete of the last slot while it plays ends the song as the list ends") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.to(at_step(13, 5));  // in slot 3, the last
  CHECK(r.order() == 3u);
  CHECK(r.p().remove_order(3) == model::Status::Ok);
  const std::uint32_t m = r.mark();
  r.to(at_step(17));
  NEED(r.next(m) != nullptr);
  CHECK(r.next(m)->pattern == 1u);  // the loop to the start
  CHECK(r.next(m)->order == 0u);
  CHECK(r.s().playing());
}

TEST_CASE("song order sync: a queued jump follows an insert above it, is dropped when its slot is deleted and is stamped against the list it was authored on") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.to(at_step(1, 5));
  CHECK(queue(r, 3, r.s().jump_stamp()));  // slot 3 (pattern 4)
  r.block();
  CHECK(r.queued() == 3u);
  CHECK(r.p().insert_order(0, 9) == model::Status::Ok);
  r.block();
  CHECK(r.queued() == 4u);  // the same slot
  const std::uint32_t m = r.mark();
  r.to(at_step(5));
  NEED(r.next(m) != nullptr);
  CHECK(r.next(m)->pattern == 4u);
  CHECK(r.next(m)->order == 4u);

  // a delete of the queued slot cancels the jump: the song goes on in order
  Rig d;
  d.song({1, 2, 3, 4});
  d.s().play_song(0);
  d.to(at_step(1, 5));
  CHECK(queue(d, 3, d.s().jump_stamp()));
  d.block();
  CHECK(d.queued() == 3u);
  CHECK(d.p().remove_order(3) == model::Status::Ok);
  d.block();
  CHECK(d.queued() == kNoQueue);
  CHECK(d.s().stats().queue_dropped == 1u);
  const std::uint32_t m2 = d.mark();
  d.to(at_step(5));
  NEED(d.next(m2) != nullptr);
  CHECK(d.next(m2)->pattern == 2u);
  CHECK(d.next(m2)->order == 1u);

  // a delete above shifts it down
  Rig u;
  u.song({1, 2, 3, 4});
  u.s().play_song(0);
  u.to(at_step(1, 5));
  CHECK(queue(u, 3, u.s().jump_stamp()));
  u.block();
  CHECK(u.p().remove_order(1) == model::Status::Ok);
  u.block();
  CHECK(u.queued() == 2u);
}

TEST_CASE("song order sync: an edit and a QueueJump in the same block: the stamp says which list the position was authored on") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.to(at_step(1, 5));
  const std::uint32_t before = r.s().jump_stamp();
  CHECK(r.p().insert_order(0, 9) == model::Status::Ok);  // the edit is after the app read slot 3 ...
  CHECK(queue(r, 3, before));                            // ... and the command arrives with the older stamp
  r.block();
  CHECK(r.queued() == 4u);  // remapped through the ops after the stamp: the same slot as the app meant

  Rig n;
  n.song({1, 2, 3, 4});
  n.s().play_song(0);
  n.to(at_step(1, 5));
  CHECK(n.p().insert_order(0, 9) == model::Status::Ok);
  CHECK(queue(n, 4, n.s().jump_stamp()));  // authored on the new list (slot 4 is pattern 4): not shifted again
  n.block();
  CHECK(n.queued() == 4u);

  // an unstamped command (0) is taken as authored on the list the render has
  Rig z;
  z.song({1, 2, 3, 4});
  z.s().play_song(0);
  z.to(at_step(1, 5));
  CHECK(queue(z, 2, 0));
  z.block();
  CHECK(z.queued() == 2u);

  // the render cannot adopt (an edit is open) when the command arrives: the position, authored on the new list, waits and is not shifted when the list is taken
  Rig w;
  w.song({1, 2, 3, 4});
  w.s().play_song(0);
  w.to(at_step(1, 5));
  {
    model::Project::OrderEdit open(w.p());
    CHECK(w.p().insert_order(0, 9) == model::Status::Ok);
    CHECK(queue(w, 4, w.s().jump_stamp()));
    w.block();
    CHECK(w.order() == 0u);  // (the old list still plays)
  }
  w.block();
  CHECK(w.queued() == 4u);
  CHECK(w.order() == 1u);
}

TEST_CASE("song order sync: a move keeps the playhead and the queue on their slot, also for a range and for a full list") {
  Rig r;
  r.song({1, 2, 3, 4, 5});
  r.s().play_song(0);
  r.to(at_step(5, 5));  // slot 1 (pattern 2)
  CHECK(queue(r, 3, r.s().jump_stamp()));  // pattern 4
  r.block();
  CHECK(r.p().move_order(1, 3) == model::Status::Ok);  // [1 3 4 2 5]
  r.block();
  CHECK(r.order() == 3u);
  CHECK(r.queued() == 2u);
  CHECK(r.p().move_order(3, 0) == model::Status::Ok);  // [2 1 3 4 5]
  r.block();
  CHECK(r.order() == 0u);
  CHECK(r.queued() == 3u);

  // a range moved by one is the slot it steps over moved the other way (the app's song_move): the playhead in the range shifts with it
  Rig g;
  g.song({1, 2, 3, 4, 5});
  g.s().play_song(0);
  g.to(at_step(5, 5));  // slot 1
  model::UndoJournal journal;
  static std::uint32_t ring[16 * 1024];
  NEED(journal.begin(ring, sizeof ring));
  model::Editor ed(g.p(), journal);
  ed.history_reset();
  CHECK(ed.move_order(3, 1) == model::Status::Ok);  // the slot 3 steps up over slots 1..2: the range 1..2 goes down
  g.block();
  CHECK(g.order() == 2u);
  CHECK(ed.undo(model::UndoArea::Song) == model::UndoResult::Ok);
  CHECK(g.p().order(1) == 2u);
  CHECK(g.p().order(2) == 3u);
  CHECK(g.p().order(3) == 4u);
  CHECK(ed.redo(model::UndoArea::Song) == model::UndoResult::Ok);
  CHECK(g.p().order(1) == 4u);
  g.block();
  CHECK(g.s().playing());
}

TEST_CASE("song order sync: a move of a list of 255 slots is one move and its undo rewrites the slots exactly") {
  Rig r;
  for (std::uint32_t i = 0; i < model::kMaxSongPositions; ++i) {
    CHECK(r.p().append_order(1 + i % 9) == model::Status::Ok);
  }
  r.s().play_song(0);
  r.to(at_step(4 * 10 + 1, 5));  // slot 10
  CHECK(r.order() == 10u);
  model::UndoJournal journal;
  static std::uint32_t ring[32 * 1024];
  NEED(journal.begin(ring, sizeof ring));
  model::Editor ed(r.p(), journal);
  ed.history_reset();
  std::uint8_t before[model::kMaxSongPositions];
  for (std::uint32_t i = 0; i < model::kMaxSongPositions; ++i) {
    before[i] = r.p().order(i);
  }
  CHECK(ed.move_order(10, 14) == model::Status::Ok);
  r.block();
  CHECK(r.order() == 14u);  // the slot was moved: the playhead went with it
  CHECK(ed.undo(model::UndoArea::Song) == model::UndoResult::Ok);
  for (std::uint32_t i = 0; i < model::kMaxSongPositions; ++i) {
    CHECK(r.p().order(i) == before[i]);
  }
  CHECK(r.p().song_length() == model::kMaxSongPositions);
  r.block();
  CHECK(r.s().playing());
}

TEST_CASE("song order sync: a range delete or a paste of more slots than the journal holds is one op and keeps the playhead") {
  Rig r;
  for (std::uint32_t i = 0; i < 60; ++i) {
    CHECK(r.p().append_order(1 + i % 9) == model::Status::Ok);
  }
  r.s().play_song(0);
  r.to(at_step(4 * 50 + 1, 5));  // slot 50
  CHECK(r.order() == 50u);
  model::UndoJournal journal;
  static std::uint32_t ring[16 * 1024];
  NEED(journal.begin(ring, sizeof ring));
  model::Editor ed(r.p(), journal);
  ed.history_reset();
  ed.begin_group(core::action_id("test.range"), 0, model::UndoArea::Song);
  {
    const model::Editor::OrderBatch batch(ed);
    for (int i = 0; i < 40; ++i) {
      CHECK(ed.remove_order(5) == model::Status::Ok);  // 40 slots from 5: all below the playhead
    }
  }
  ed.end_group();
  r.block();
  CHECK(r.order() == 10u);
  CHECK(r.s().stats().queue_dropped == 0u);
  ed.begin_group(core::action_id("test.paste"), 0, model::UndoArea::Song);
  {
    const model::Editor::OrderBatch batch(ed);
    for (int i = 0; i < 30; ++i) {
      CHECK(ed.insert_order(static_cast<std::uint32_t>(2 + i), 1 + i % 9) == model::Status::Ok);
    }
  }
  ed.end_group();
  r.block();
  CHECK(r.order() == 40u);
  CHECK(ed.undo(model::UndoArea::Song) == model::UndoResult::Ok);  // the undo of the paste is one change too
  r.block();
  CHECK(r.order() == 10u);
  CHECK(ed.undo(model::UndoArea::Song) == model::UndoResult::Ok);  // and of the range delete
  r.block();
  CHECK(r.order() == 50u);
}

TEST_CASE("song order sync: the journal holds 16 ops, 17 between two blocks become a reset: clamped, still playing, never past the list") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.to(at_step(9, 5));  // slot 2
  for (std::uint32_t i = 0; i < 16; ++i) {
    CHECK(r.p().insert_order(0, 5) == model::Status::Ok);
  }
  r.block();
  CHECK(r.order() == 18u);  // 16 ops are all replayed

  Rig l;
  l.song({1, 2, 3, 4});
  l.s().play_song(0);
  l.to(at_step(9, 5));
  for (std::uint32_t i = 0; i < 17; ++i) {
    CHECK(l.p().insert_order(0, 5) == model::Status::Ok);
  }
  l.block();
  CHECK(l.order() < l.p().song_length());  // a reset: the position is where it was, within the list
  CHECK(l.order() == 2u);
  CHECK(l.s().playing());

  // the song shrinks by a reset (a load): the position is clamped, the queue dropped, the song plays on
  Rig c;
  c.song({1, 2, 3, 4});
  c.s().play_song(0);
  c.to(at_step(13, 5));  // slot 3
  CHECK(queue(c, 2, c.s().jump_stamp()));
  c.block();
  CHECK(c.p().clear_order() == model::Status::Ok);
  CHECK(c.p().append_order(7) == model::Status::Ok);
  CHECK(c.p().append_order(8) == model::Status::Ok);
  c.block();
  CHECK(c.order() == 1u);
  CHECK(c.queued() == kNoQueue);
  const std::uint32_t m = c.mark();
  c.to(at_step(18));
  NEED(c.next(m) != nullptr);
  CHECK(c.next(m)->pattern == 7u);  // the end of the list loops
  CHECK(c.next(m)->order == 0u);
}

TEST_CASE("song order sync: a project reset while playing is a reset of the position") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.to(at_step(9, 5));
  r.p().reset();
  CHECK(r.p().create_pattern(1, 4) == model::Status::Ok);
  CHECK(r.p().append_order(1) == model::Status::Ok);
  r.to(at_step(13));
  CHECK(r.order() == 0u);
  CHECK(r.s().playing());
}

TEST_CASE("song order sync: an edit that is still open is not read; the old list plays until it closes") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.to(at_step(9, 5));
  const std::uint32_t m = r.mark();
  {
    model::Project::OrderEdit open(r.p());
    CHECK(r.p().insert_order(0, 9) == model::Status::Ok);
    CHECK((r.p().order_epoch() & 1u) == 1u);
    r.to(at_step(13));
    NEED(r.next(m) != nullptr);
    CHECK(r.next(m)->pattern == 4u);  // the old list: the old next slot at its old index
    CHECK(r.next(m)->order == 3u);
  }
  CHECK((r.p().order_epoch() & 1u) == 0u);
  r.block();
  CHECK(r.order() == 4u);  // taken at the next block: slot 3 of the old list is slot 4 of the new one
}

namespace {
struct Hook {
  model::Project* p;
  int calls;
};
void edit_between(void* ctx) noexcept {
  Hook* h = static_cast<Hook*>(ctx);
  ++h->calls;
  (void)h->p->insert_order(0, 3);
}
}  // namespace

TEST_CASE("song order sync: a copy during which an edit closed is discarded, the next one is whole") {
  Rig r;
  r.song({1, 2, 3});
  model::OrderSnapshot snap;
  CHECK(r.p().snapshot_order(snap));
  CHECK(snap.len == 3u);
  CHECK((snap.epoch & 1u) == 0u);
  Hook h{&r.p(), 0};
  CHECK_FALSE(r.p().snapshot_order(snap, &edit_between, &h));
  CHECK(h.calls == 1);
  const std::uint32_t before = snap.ops;
  CHECK(r.p().snapshot_order(snap));
  CHECK(snap.len == 4u);
  CHECK(snap.list[0] == 3u);
  CHECK((snap.ops - before) >= 1u);
}

TEST_CASE("song order sync: undo and redo of an insert and of a delete keep the playhead by the same rules") {
  Rig r;
  r.song({1, 2, 3, 4});
  model::UndoJournal journal;
  static std::uint32_t ring[16 * 1024];
  NEED(journal.begin(ring, sizeof ring));
  model::Editor ed(r.p(), journal);
  ed.history_reset();
  r.s().play_song(0);
  r.to(at_step(9, 5));  // slot 2
  CHECK(ed.insert_order(0, 9) == model::Status::Ok);
  r.block();
  CHECK(r.order() == 3u);
  CHECK(ed.undo(model::UndoArea::Song) == model::UndoResult::Ok);
  r.block();
  CHECK(r.order() == 2u);
  CHECK(ed.redo(model::UndoArea::Song) == model::UndoResult::Ok);
  r.block();
  CHECK(r.order() == 3u);
  ed.history_reset();
  // the playing slot deleted, and brought back: it sits behind the playhead, the slot after it plays next
  CHECK(ed.remove_order(3) == model::Status::Ok);  // [9 1 2 4]
  r.block();
  CHECK(ed.undo(model::UndoArea::Song) == model::UndoResult::Ok);  // [9 1 2 3 4]
  r.block();
  const std::uint32_t m = r.mark();
  r.to(at_step(13));
  NEED(r.next(m) != nullptr);
  CHECK(r.next(m)->pattern == 4u);
  CHECK(r.next(m)->order == 4u);
}

TEST_CASE("song order sync: an edit while stopped is in the list the next Play starts from, the block the pattern ends in takes it too") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.block();  // stopped: the list is taken all the same
  CHECK(r.p().insert_order(0, 9) == model::Status::Ok);
  r.block();
  r.s().play_song(0);
  r.to(at_step(1));
  CHECK(r.order() == 0u);
  const std::uint32_t m = r.mark();
  r.to(at_step(5));
  NEED(r.next(m) != nullptr);
  CHECK(r.next(m)->pattern == 1u);
  CHECK(r.next(m)->order == 1u);

  // the edit lands in the block the pattern ends in (the end is decided when its last step runs, one step ahead of the landing): the next pattern is read from the new list
  Rig e;
  e.song({1, 2, 3, 4});
  e.s().play_song(0);
  e.to(at_step(3) - 20);
  CHECK(e.p().insert_order(1, 8) == model::Status::Ok);
  const std::uint32_t m2 = e.mark();
  e.to(at_step(5));
  NEED(e.next(m2) != nullptr);
  CHECK(e.next(m2)->pattern == 8u);
  CHECK(e.next(m2)->order == 1u);
  CHECK(e.next(m2)->t == at_step(4));
}

TEST_CASE("song order thread: the render never sees a list that is not one of the written lists") {
  model::ModelArena* arena = new model::ModelArena;
  std::memset(arena, 0, sizeof *arena);
  model::Project& p = arena->project;
  p.reset();
  CHECK(p.create_pattern(1, 4) == model::Status::Ok);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::uint32_t k = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const std::uint32_t id = 1 + (k++ % 2);                  // list A: 10 slots of 1; list B: 20 slots of 2
      const std::uint32_t len = id == 1 ? 10u : 20u;
      {
        model::Project::OrderEdit edit(p);
        (void)p.clear_order();
        for (std::uint32_t i = 0; i < len; ++i) {
          (void)p.append_order(id);
        }
      }
      std::this_thread::yield();  // (under ThreadSanitizer the writer would otherwise hold the epoch odd nearly always: the reader needs a gap to take a whole copy)
    }
  });
  std::uint32_t whole = 0;
  std::uint32_t bad = 0;
  model::OrderSnapshot snap;
  for (std::uint32_t i = 0; i < 300000u || (whole < 1000u && i < 3000000u); ++i) {  // (and on until enough copies were whole: how many the writer leaves depends on the scheduler)
    if (!p.snapshot_order(snap)) {
      continue;
    }
    ++whole;
    const std::uint32_t len = snap.len;
    bool ok = len == 0u || len == 10u || len == 20u;
    const std::uint8_t id = len == 10u ? 1 : 2;
    for (std::uint32_t k = 0; ok && k < len; ++k) {
      ok = snap.list[k] == id;
    }
    bad += ok ? 0u : 1u;
  }
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  CHECK(whole > 0u);
  CHECK(bad == 0u);
  delete arena;
}

namespace {

// A move of the playing slot (pattern 3 of 5, slot 2) through the Editor, then its undo (and its redo): the journal's insert and remove are applied as the one move, so the playhead stays on
// pattern 3 and the slot after it is the right one (not a deleted slot: the song would end or loop and skip a slot).
void move_playing_then_undo(std::uint32_t from, std::uint32_t to, bool redo, std::uint32_t order, std::uint32_t next_pattern) {
  Rig r;
  r.song({1, 2, 3, 4, 5});
  model::UndoJournal journal;
  static std::uint32_t ring[16 * 1024];
  NEED(journal.begin(ring, sizeof ring));
  model::Editor ed(r.p(), journal);
  ed.history_reset();
  r.s().play_song(0);
  r.to(at_step(9, 5));  // slot 2 (pattern 3)
  CHECK(r.order() == 2u);
  CHECK(ed.move_order(from, to) == model::Status::Ok);
  r.block();
  CHECK(r.order() == to);
  CHECK(ed.undo(model::UndoArea::Song) == model::UndoResult::Ok);
  r.block();
  if (redo) {
    CHECK(r.order() == 2u);
    CHECK(ed.redo(model::UndoArea::Song) == model::UndoResult::Ok);
    r.block();
  }
  CHECK(r.order() == order);
  CHECK(r.p().order(order) == 3u);
  const std::uint32_t m = r.mark();
  r.to(at_step(13));
  NEED(r.next(m) != nullptr);
  CHECK(r.next(m)->pattern == next_pattern);
  CHECK(r.next(m)->order == order + 1u);
  CHECK(r.s().playing());
}

}  // namespace

TEST_CASE("song order sync: undo and redo of a move of the playing slot keep the playhead on it, down and up") {
  move_playing_then_undo(2, 3, false, 2u, 4u);  // [1 2 4 3 5] undone: pattern 3 at 2, then 4
  move_playing_then_undo(2, 3, true, 3u, 5u);   // redone: pattern 3 at 3, then 5
  move_playing_then_undo(2, 1, false, 2u, 4u);  // [1 3 2 4 5] undone
  move_playing_then_undo(2, 1, true, 1u, 2u);   // redone: pattern 3 at 1, then 2
}

TEST_CASE("song order sync: the undo of a move of a range that holds the playhead, and a unit of an insert and a remove of two ids, apply right") {
  Rig r;
  r.song({1, 2, 3, 4, 5});
  model::UndoJournal journal;
  static std::uint32_t ring[16 * 1024];
  NEED(journal.begin(ring, sizeof ring));
  model::Editor ed(r.p(), journal);
  ed.history_reset();
  r.s().play_song(0);
  r.to(at_step(9, 5));  // slot 2
  CHECK(ed.move_order(0, 4) == model::Status::Ok);  // [2 3 4 5 1]: the playhead shifts up with the range
  r.block();
  CHECK(r.order() == 1u);
  CHECK(ed.undo(model::UndoArea::Song) == model::UndoResult::Ok);
  r.block();
  CHECK(r.order() == 2u);
  for (std::uint32_t i = 0; i < 5; ++i) {
    CHECK(r.p().order(i) == i + 1u);
  }
  // an insert and a remove of different ids are two edits, not a move
  ed.history_reset();
  ed.begin_group(core::action_id("test.pair"), 0, model::UndoArea::Song);
  CHECK(ed.insert_order(0, 9) == model::Status::Ok);  // [9 1 2 3 4 5]
  CHECK(ed.remove_order(5) == model::Status::Ok);     // [9 1 2 3 4]: the slot of pattern 5
  ed.end_group();
  CHECK(ed.undo(model::UndoArea::Song) == model::UndoResult::Ok);
  for (std::uint32_t i = 0; i < 5; ++i) {
    CHECK(r.p().order(i) == i + 1u);
  }
  CHECK(ed.redo(model::UndoArea::Song) == model::UndoResult::Ok);
  CHECK(r.p().song_length() == 5u);
  CHECK(r.p().order(0) == 9u);
  CHECK(r.p().order(4) == 4u);
}

TEST_CASE("song order sync: a delete of the playing slot and then a locate to another slot plays the located slot once") {
  Rig r;
  r.song({1, 2, 3, 4, 5});
  r.s().play_song(0);
  r.to(at_step(9, 5));  // slot 2
  CHECK(r.p().remove_order(2) == model::Status::Ok);  // [1 2 4 5]
  r.block();
  r.s().locate(4);  // slot 1, row 0 (pattern 2), at the next step boundary
  r.to(at_step(11));
  const std::uint32_t m = r.mark();
  r.to(at_step(15));
  NEED(r.next(m) != nullptr);
  CHECK(r.next(m)->pattern == 4u);  // the slot after the located one: pattern 2 was not repeated
  CHECK(r.next(m)->order == 2u);

  // a locate_row of the pattern that plays keeps the deleted slot's mark: the slot that slid in follows it
  Rig q;
  q.song({1, 2, 3, 4, 5});
  q.s().play_song(0);
  q.to(at_step(9, 5));
  CHECK(q.p().remove_order(2) == model::Status::Ok);
  q.block();
  q.s().locate_row(0);
  q.to(at_step(11));
  const std::uint32_t m2 = q.mark();
  q.to(at_step(15));
  NEED(q.next(m2) != nullptr);
  CHECK(q.next(m2)->pattern == 4u);
  CHECK(q.next(m2)->order == 2u);
}

TEST_CASE("song order sync: a delete of the slot a stopped song was in: Continue finishes that pattern and plays the slot that slid in") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.to(at_step(9, 5));  // slot 2
  r.s().stop();
  r.block();
  CHECK_FALSE(r.s().playing());
  CHECK(r.p().remove_order(2) == model::Status::Ok);  // [1 2 4]
  r.block();
  r.s().resume();
  r.block();
  CHECK(r.s().playing());
  const std::uint32_t m = r.mark();
  r.to(r.now() + at_step(5));  // the rest of the pattern (three steps at most), then the slot that slid in (four steps): the song loops to slot 0 only after both
  bool four = false;
  for (std::uint32_t i = m; r.next(i) != nullptr; ++i) {
    CHECK(r.next(i)->order != 0u);  // the song did not end and loop before the slot that slid in
    four = four || (r.next(i)->pattern == 4u && r.next(i)->order == 2u);
  }
  CHECK(four);
}

TEST_CASE("song order sync: a locate to a slot after a delete plays it once even when the song repeats the pattern of the deleted slot") {
  Rig r;
  r.song({3, 3, 4, 5});
  r.s().play_song(0);
  r.to(at_step(1));  // slot 0 (pattern 3)
  CHECK(r.p().remove_order(0) == model::Status::Ok);  // [3 4 5]: slot 0 is now the other 3, the deleted slot's pattern keeps playing
  r.block();
  const std::uint32_t m = r.mark();
  r.s().locate(0);  // slot 0 again: the same index and pattern as the one that slid in
  r.to(at_step(12));
  std::uint32_t threes = 0;
  bool four = false;
  for (std::uint32_t i = m; r.next(i) != nullptr; ++i) {
    threes += r.next(i)->pattern == 3u ? 1u : 0u;
    four = four || (r.next(i)->pattern == 4u && r.next(i)->order == 1u);
  }
  CHECK(threes == 1u);  // the located slot, once
  CHECK(four);
}

// ---- OS-SONG3 S7: loop a range of slots --------------------------------------------------------------------------------------------------------------------------
// The patterns of these songs are all different (1..N), so the pattern of a landing names its slot as well as the position does.

namespace {

bool loop_cmd(Rig& r, std::uint32_t first, std::uint32_t last, std::uint32_t stamp = 0) {
  return r.s().commands().push(seq::Command{seq::Command::Kind::SetLoop, static_cast<std::uint8_t>(first), static_cast<std::uint16_t>(last), stamp});
}

// The positions of the landings after `from`, as a string of digits ("123423"). The run's own start (slot 0) is not among them: a Pattern event is made at a pattern's end, and a run begins without one.
std::string slots_from(const Rig& r, std::uint32_t from) {
  std::string s;
  for (std::uint32_t i = from; r.next(i) != nullptr; ++i) {
    s += static_cast<char>('0' + r.next(i)->order);
  }
  return s;
}

std::uint32_t loop_first(Rig& r) { return r.s().play_position().read().loop_first; }
std::uint32_t loop_last(Rig& r) { return r.s().play_position().read().loop_last; }

}  // namespace

TEST_CASE("song loop: the song returns to the first slot of the range after its last one, and never ends") {
  Rig r;
  r.song({1, 2, 3, 4, 5, 6});
  r.s().set_song_loop(false);  // the loop is not the song's own: with the song's loop off it still goes round, and no SongEnd stops it
  r.s().play_song(0);
  r.block();
  r.s().set_loop(2, 4);
  CHECK(r.s().looping());
  r.to(at_step(4 * 10));
  CHECK(r.s().playing());
  CHECK(slots_from(r, 0) == "123423423");
  CHECK(loop_first(r) == 2u);
  CHECK(loop_last(r) == 4u);
}

TEST_CASE("song loop: a range that ends the list loops without the SongEnd, one slot is a range") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.block();
  r.s().set_loop(2, 3);
  r.to(at_step(4 * 9));
  CHECK(slots_from(r, 0) == "12323232");
  Rig q;
  q.song({1, 2, 3, 4});
  q.s().play_song(0);
  q.block();
  q.s().set_loop(1, 1);
  q.to(at_step(4 * 6));
  CHECK(slots_from(q, 0) == "11111");
}

TEST_CASE("song loop: SetLoop is a command, and a range past the list, upside down or kNoJump is no loop") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.block();
  CHECK(loop_cmd(r, 1, 2));
  r.block();
  CHECK(loop_first(r) == 1u);
  CHECK(loop_last(r) == 2u);
  CHECK(loop_cmd(r, 1, 4));  // past the list (4 slots)
  r.block();
  CHECK_FALSE(r.s().looping());
  CHECK(loop_first(r) == audio::PlayPosition::kNoOrder);
  CHECK(loop_cmd(r, 1, 2));
  r.block();
  CHECK(r.s().looping());
  CHECK(loop_cmd(r, 0, seq::kNoJump));  // the cancel
  r.block();
  CHECK_FALSE(r.s().looping());
  CHECK(loop_cmd(r, 3, 1));  // upside down
  r.block();
  CHECK_FALSE(r.s().looping());
  CHECK(loop_first(r) == audio::PlayPosition::kNoOrder);
  CHECK(loop_last(r) == audio::PlayPosition::kNoOrder);
}

TEST_CASE("song loop: set while the song plays before the range, the song enters it by itself and loops from its last slot") {
  Rig r;
  r.song({1, 2, 3, 4, 5, 6});
  r.s().play_song(0);
  r.to(at_step(1));
  CHECK(loop_cmd(r, 3, 4));
  r.to(at_step(4 * 8));
  CHECK(slots_from(r, 0) == "1234343");
  // and set while it plays past the range, it waits: the song goes on to its end and round to the range
  Rig q;
  q.song({1, 2, 3, 4, 5, 6});
  q.s().play_song(0);
  q.to(at_step(4 * 5 + 1));  // slot 5
  CHECK(loop_cmd(q, 1, 2));
  q.to(at_step(4 * 12));
  CHECK(slots_from(q, 0) == "12345012121");
}

TEST_CASE("song loop: a live jump out of the range leaves the loop, a live jump inside it keeps it") {
  Rig r;
  r.song({1, 2, 3, 4, 5, 6});
  r.s().play_song(0);
  r.block();
  r.s().set_loop(2, 4);
  r.to(at_step(4 * 3 + 1));  // slot 3
  CHECK(queue(r, 0, r.s().jump_stamp()));  // the Rec jump to slot 0
  r.to(at_step(4 * 12));
  CHECK(slots_from(r, 0) == "12301234501");  // slot 3 finishes, the song jumps to 0 and walks the whole list: no wrap at 4 any more
  CHECK_FALSE(r.s().looping());
  CHECK(loop_first(r) == audio::PlayPosition::kNoOrder);

  Rig q;
  q.song({1, 2, 3, 4, 5, 6});
  q.s().play_song(0);
  q.block();
  q.s().set_loop(2, 4);
  q.to(at_step(4 * 3 + 1));  // slot 3
  CHECK(queue(q, 2, q.s().jump_stamp()));  // a jump inside it
  q.to(at_step(4 * 9));
  CHECK(slots_from(q, 0) == "12323423");  // slot 3 finishes, the jump to 2 stays in the range: 3, 4, then round to 2
  CHECK(q.s().looping());
}

TEST_CASE("song loop: an Order jump or a queued pattern that leaves the range ends the loop") {
  Rig r;
  r.song({1, 2, 3, 4, 5, 6});
  r.s().play_song(0);
  r.block();
  r.s().set_loop(2, 4);
  r.to(at_step(4 * 2 + 1));
  r.s().queue_pattern(8);  // the sequential switch: Pattern mode, the song is left
  r.to(at_step(4 * 5));
  CHECK_FALSE(r.s().looping());
  CHECK(loop_first(r) == audio::PlayPosition::kNoOrder);
}

TEST_CASE("song loop: a stop, a new start and a pattern start end the loop") {
  Rig r;
  r.song({1, 2, 3, 4, 5, 6});
  r.s().play_song(0);
  r.block();
  r.s().set_loop(1, 2);
  CHECK(r.s().looping());
  r.s().stop();
  r.block();
  CHECK_FALSE(r.s().playing());
  CHECK_FALSE(r.s().looping());
  r.s().play_song(0);
  r.block();
  CHECK_FALSE(r.s().looping());
  r.s().set_loop(1, 2);
  r.s().play_song(0);  // a Play while it loops: the loop is not kept
  CHECK_FALSE(r.s().looping());
  r.s().set_loop(1, 2);
  r.s().play_pattern(3);
  CHECK_FALSE(r.s().looping());
}

TEST_CASE("song loop: a slot deleted inside the loop shrinks it") {
  Rig r;
  r.song({1, 2, 3, 4, 5, 6});
  r.s().play_song(0);
  r.block();
  r.s().set_loop(2, 4);
  r.to(at_step(4 * 2 + 1));  // slot 2
  CHECK(r.p().remove_order(3) == model::Status::Ok);  // [1 2 3 5 6]: the loop is slots 2..3
  r.block();
  CHECK(loop_first(r) == 2u);
  CHECK(loop_last(r) == 3u);
  r.to(at_step(4 * 8));
  CHECK(slots_from(r, 0) == "1232323");
  // the first slot of the range deleted: the one that slid in is the first; the last one: the one before the hole is the last
  CHECK(r.p().remove_order(2) == model::Status::Ok);  // [1 2 5 6]
  r.block();
  CHECK(loop_first(r) == 2u);
  CHECK(loop_last(r) == 2u);
  CHECK(r.p().remove_order(2) == model::Status::Ok);  // [1 2 6]: the range's last slot, the only one left
  r.block();
  CHECK_FALSE(r.s().looping());
  CHECK(loop_first(r) == audio::PlayPosition::kNoOrder);
}

TEST_CASE("song loop: deleting the playing last slot of the loop sends the song back to the first") {
  Rig r;
  r.song({1, 2, 3, 4, 5});
  r.s().play_song(0);
  r.block();
  r.s().set_loop(1, 3);
  r.to(at_step(4 * 3 + 1));  // slot 3, the loop's last
  CHECK(r.p().remove_order(3) == model::Status::Ok);  // [1 2 3 5]: the loop is slots 1..2, the playing slot is gone
  r.block();
  CHECK(loop_last(r) == 2u);
  const std::uint32_t m = r.mark();
  r.to(at_step(4 * 8));
  const std::string s = slots_from(r, m);
  CHECK(s.substr(0, 3) == "121");  // the rest of the deleted slot's pattern, then the loop from its first slot
}

TEST_CASE("song loop: an insert inside the range grows it, below it shifts it, past its end leaves it") {
  Rig r;
  r.song({1, 2, 3, 4, 5});
  r.s().play_song(0);
  r.block();
  r.s().set_loop(2, 3);
  CHECK(r.p().insert_order(3, 9) == model::Status::Ok);  // inside (at the last slot's index): [1 2 3 9 4 5], 2..4
  r.block();
  CHECK(loop_first(r) == 2u);
  CHECK(loop_last(r) == 4u);
  CHECK(r.p().insert_order(0, 9) == model::Status::Ok);  // below: shifts, 3..5
  r.block();
  CHECK(loop_first(r) == 3u);
  CHECK(loop_last(r) == 5u);
  CHECK(r.p().insert_order(6, 9) == model::Status::Ok);  // past its end: no change
  r.block();
  CHECK(loop_first(r) == 3u);
  CHECK(loop_last(r) == 5u);
}

TEST_CASE("song loop: a move that touches the range ends the loop, one outside it leaves the range alone") {
  Rig r;
  r.song({1, 2, 3, 4, 5, 6, 7});
  r.s().play_song(0);
  r.block();
  r.s().set_loop(2, 4);
  CHECK(r.p().move_order(0, 1) == model::Status::Ok);  // below the range: the moved-over span [0,1] is outside 2..4
  r.block();
  CHECK(r.s().looping());
  CHECK(loop_first(r) == 2u);
  CHECK(loop_last(r) == 4u);
  CHECK(r.p().move_order(5, 6) == model::Status::Ok);  // above it: the span [5,6] is outside too
  r.block();
  CHECK(r.s().looping());
  CHECK(loop_first(r) == 2u);
  CHECK(loop_last(r) == 4u);
  // a move inside the range that remapping only the ends used to keep (first <= last after it, 2..4 -> 2..3) changes which slots lie between the ends: the loop is over
  CHECK(r.p().move_order(3, 5) == model::Status::Ok);
  r.block();
  CHECK_FALSE(r.s().looping());
  CHECK(loop_first(r) == audio::PlayPosition::kNoOrder);
  CHECK(loop_last(r) == audio::PlayPosition::kNoOrder);
  // any other move that touches the range ends it too: a slot from outside into the range's first place, one from below into it
  r.s().set_loop(2, 4);
  CHECK(r.p().move_order(6, 2) == model::Status::Ok);
  r.block();
  CHECK_FALSE(r.s().looping());
  r.s().set_loop(2, 4);
  CHECK(r.p().move_order(0, 3) == model::Status::Ok);
  r.block();
  CHECK_FALSE(r.s().looping());
}

TEST_CASE("song loop: a clear or a load of the list ends the loop") {
  Rig r;
  r.song({1, 2, 3, 4});
  r.s().play_song(0);
  r.block();
  r.s().set_loop(1, 2);
  CHECK(r.p().clear_order() == model::Status::Ok);
  r.block();
  CHECK_FALSE(r.s().looping());
}

TEST_CASE("song loop: a range authored against the list before an edit follows the edit, one authored against a list the render lacks is refused") {
  Rig r;
  r.song({1, 2, 3, 4, 5});
  r.s().play_song(0);
  r.block();
  const std::uint32_t stamp = r.s().jump_stamp();  // the app's list as it was when the range was chosen
  CHECK(r.p().insert_order(0, 9) == model::Status::Ok);  // an edit lands before the command is read: it carries the range along
  CHECK(loop_cmd(r, 1, 2, stamp));
  r.block();
  CHECK(loop_first(r) == 2u);
  CHECK(loop_last(r) == 3u);
  // a stamp from the future: the list that range was authored against is not the render's yet
  CHECK(loop_cmd(r, 1, 2, r.s().jump_stamp() + 1u));
  r.block();
  CHECK_FALSE(r.s().looping());
}

TEST_CASE("song loop: deleting the playing slot past the range does not send the song into the loop") {
  Rig r;
  r.song({1, 2, 3, 4, 5, 6});
  r.s().play_song(0);
  r.to(at_step(4 * 3 + 1));  // slot 3, past the range that is set now
  CHECK(loop_cmd(r, 1, 2));
  r.block();
  CHECK(r.p().remove_order(3) == model::Status::Ok);  // [1 2 3 5 6]: the slot that slid into place plays after the rest of this pattern
  r.block();
  const std::uint32_t m = r.mark();
  r.to(at_step(4 * 11));
  CHECK(slots_from(r, m) == "3401212");  // 3 and 4, the song's own end round to 0, then the loop from its first slot
}
