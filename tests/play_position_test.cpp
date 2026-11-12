// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-009b: the play position from the audio side to the Pattern screen. The one-word hand-off (audio/play_position.h) under a real writer thread, the
// sequencer publishing it at the end of every block, the App reading it once a pass (the play bar for the pattern that plays, Browse following it, the
// redraws), and the strip's BPM text. The picture itself is not pinned (no golden: the gallery holds the screens).
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/app/view_state.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/core/fixed_string.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/seq/sequencer.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/need.h"
#include "support/project_fixture.h"
#include "support/seq_fixture.h"

using namespace o3os;
using audio::PlayPosition;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

// An App on a document (pattern 1 and 2, 64 steps) with a position source attached.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  PlayPosition position;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);  // (after the editor: ~App closes a take's undo group on it, OS-LIVEREC)

  Rig() {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, 64);
    (void)p.create_pattern(2, 64);
    (void)p.append_order(1);
    (void)p.set_tempo_x100(12000);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_play_position(&position);
    app->run_once();
  }
  void pass() { app->run_once(); }
  void mode(app::PatternMode m) { (void)app->invoke(core::action_id("view.mode"), static_cast<core::ActionArg>(m)); }
  std::size_t frames() const { return display.frames(); }
};

std::string bpm(unsigned x100) {
  core::FixedString<24> s;
  app::append_bpm(s, x100);
  return s.c_str();
}

}  // namespace

TEST_CASE("play position: one word carries playing, pattern, song row and step, the tempo its own") {
  PlayPosition p;
  PlayPosition::Value v = p.read();
  CHECK_FALSE(v.playing);
  CHECK(v.tempo_x100 == 0u);
  p.publish(true, 255, 127, 80000);
  v = p.read();
  CHECK(v.playing);
  CHECK(v.pattern == 255);
  CHECK(v.step == 127);
  CHECK(v.tempo_x100 == 80000u);
  CHECK(v.order == PlayPosition::kNoOrder);  // Pattern mode: no row
  p.publish(true, 7, 31, 12000, 254);        // the last row an order list of 255 positions has
  v = p.read();
  CHECK(v.order == 254);
  CHECK(v.pattern == 7);
  CHECK(v.step == 31);
  p.publish(true, 7, 31, 12000, 255);  // out of the list's range: none
  CHECK(p.read().order == PlayPosition::kNoOrder);
  p.publish(true, 7, 31, 12000, 0);    // row 0 is a row
  CHECK(p.read().order == 0);
  p.publish(false, 3, 0, 4000);
  v = p.read();
  CHECK_FALSE(v.playing);
  CHECK(v.pattern == 3);
  CHECK(v.tempo_x100 == 4000u);
  // OS-LIVEREC: the microstep the playhead is in (0..47) shares the word and disturbs none of the others; it is 0 when not given, and held at 47.
  CHECK(v.ustep == 0);
  for (std::uint32_t u = 0; u < 48; ++u) {
    p.publish(true, 255, 127, 80000, 254, u);
    v = p.read();
    CHECK(v.ustep == u);
    CHECK(v.playing);
    CHECK(v.pattern == 255);
    CHECK(v.order == 254);
    CHECK(v.step == 127);
  }
  p.publish(true, 1, 2, 12000, PlayPosition::kNoOrder, 48);
  CHECK(p.read().ustep == 47);
  p.publish(true, 1, 2, 12000, PlayPosition::kNoOrder, 5000);
  CHECK(p.read().ustep == 47);
  p.publish(true, 1, 2, 12000);
  CHECK(p.read().ustep == 0);
}

TEST_CASE("play position: a reader never sees a pattern and a step from two different publishes") {
  PlayPosition p;
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::uint32_t k = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      p.publish((k & 1u) != 0u, k & 0xFFu, k & 0xFFu, 12000, k & 0x7Fu, (k & 0xFFu) % 48u);  // pattern and step always equal, the row is their low 7 bits, the playing flag follows their parity, the microstep their remainder
      ++k;
    }
  });
  std::uint32_t torn = 0;
  for (std::uint32_t i = 0; i < 200000; ++i) {
    const PlayPosition::Value v = p.read();
    if (v.pattern != (v.step & 0xFFu)) {
      ++torn;
    }
    if (v.order != (v.pattern & 0x7Fu)) {
      ++torn;
    }
    if (v.playing != ((v.pattern & 1u) != 0u)) {  // the low bit of k & 0xFF is the low bit of k
      ++torn;
    }
    if (v.ustep != (v.step % 48u)) {
      ++torn;
    }
  }
  stop.store(true);
  writer.join();
  CHECK(torn == 0u);
}

TEST_CASE("play position: the sequencer publishes at the end of every block, and a stop says so") {
  test::fresh_project(4, 4);
  model::Project& proj = test::seq_project();
  CHECK(proj.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(proj.create_pattern(1, 16) == model::Status::Ok);
  seq::Sequencer& s = test::seq_slot().make(proj);
  static audio::SeqEvent events[64];
  std::uint64_t at = 0;
  const auto block = [&] {
    (void)s.step(at, 64, events, 64);
    at += 64;
  };
  block();
  CHECK_FALSE(s.play_position().read().playing);
  s.play_pattern(1);
  block();
  PlayPosition::Value v = s.play_position().read();
  CHECK(v.playing);
  CHECK(v.pattern == 1);
  CHECK(v.step == 0);
  CHECK(v.tempo_x100 == 12000u);
  for (int i = 0; i < 100; ++i) {  // a step is 0.125 s at 120 BPM: about 86 blocks of 64 samples
    block();
  }
  v = s.play_position().read();
  CHECK(v.playing);
  CHECK(v.step == s.position().step);
  CHECK(v.step >= 1);
  s.stop();
  block();
  CHECK_FALSE(s.play_position().read().playing);
}

TEST_CASE("play position: the bar is for the pattern that plays and no other") {
  PlayPosition::Value v;
  v.playing = true;
  v.pattern = 2;
  v.step = 9;
  CHECK(app::play_row(v, 2) == 9);
  CHECK(app::play_row(v, 1) == -1);
  v.playing = false;
  CHECK(app::play_row(v, 2) == -1);
}

TEST_CASE("play position: the sequencer publishes the microstep the playhead is in: 0..47, rising through a step and starting over with the next (OS-LIVEREC)") {
  test::fresh_project(4, 4);
  model::Project& proj = test::seq_project();
  CHECK(proj.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(proj.create_pattern(1, 16) == model::Status::Ok);
  seq::Sequencer& s = test::seq_slot().make(proj);
  static audio::SeqEvent events[64];
  std::uint64_t at = 0;
  s.play_pattern(1);
  bool first = true;
  unsigned prev_step = 0;
  unsigned prev_ustep = 0;
  unsigned changes = 0;
  bool seen[48] = {};
  for (int i = 0; i < 400; ++i) {  // about 4.6 steps at 120 BPM (a step is 5,512 samples: 86 blocks of 64)
    (void)s.step(at, 64, events, 64);
    at += 64;
    const PlayPosition::Value v = s.play_position().read();
    NEED(v.ustep < 48);
    CHECK(v.ustep == s.position().ustep);
    seen[v.ustep] = true;
    if (!first && v.step == prev_step) {
      CHECK(v.ustep >= prev_ustep);
    } else if (!first) {
      CHECK(v.step == (prev_step + 1u) % 16u);
      CHECK(prev_ustep >= 45);  // the last block of a step is in its last microsteps ...
      CHECK(v.ustep <= 1);      // ... and the next one begins the new one
      ++changes;
    }
    first = false;
    prev_step = v.step;
    prev_ustep = v.ustep;
  }
  CHECK(changes >= 3u);
  unsigned distinct = 0;
  for (const bool b : seen) {
    distinct += b ? 1u : 0u;
  }
  CHECK(distinct >= 44u);  // a block is half a microstep: the word follows the grid
  s.stop();
  (void)s.step(at, 64, events, 64);
  CHECK_FALSE(s.play_position().read().playing);
}

TEST_CASE("play position: the screen redraws when the bar moves, and not when nothing it shows changed") {
  Rig rig;
  rig.mode(app::PatternMode::Edit);  // Browse would follow the song to the pattern that plays (OS-009c), and so does Edit (OS-LIVEREC) unless the recording is free: the free one shows the bar and stays
  rig.mode(app::PatternMode::Browse);
  (void)rig.app->invoke(core::action_id("pattern.edit_toggle"));  // REC: STEP (OS-RECMODE2; Shift + REC in a LIVE that view.mode made goes to WATCH, OS-UXF-PAT item 8)
  rig.pass();
  rig.position.publish(true, 1, 3, 12000);
  const std::size_t f0 = rig.frames();
  rig.pass();
  CHECK(rig.frames() == f0 + 1);  // the bar appeared
  CHECK(rig.app->state().play.step == 3);
  const std::vector<std::uint32_t> bar3 = rig.display.last_frame();
  rig.pass();
  CHECK(rig.frames() == f0 + 1);  // the same step: nothing to draw
  rig.position.publish(true, 1, 4, 12000);
  rig.pass();
  CHECK(rig.frames() == f0 + 2);
  CHECK(rig.display.last_frame() != bar3);
  rig.position.publish(true, 2, 5, 12000);  // another pattern plays: the one in view has no bar, and the bar went away
  rig.pass();
  CHECK(rig.frames() == f0 + 3);
  rig.position.publish(true, 2, 6, 12000);  // and its steps are not this screen's news
  rig.pass();
  CHECK(rig.frames() == f0 + 3);
  rig.position.publish(false, 2, 6, 12000);
  rig.pass();
  CHECK(rig.frames() == f0 + 4);
}

TEST_CASE("play position: the strip says the tempo the sequencer uses while it plays, the project's otherwise") {
  Rig rig;
  rig.mode(app::PatternMode::Edit);  // stays on pattern 1 while pattern 2 plays (the free recording: Edit in follow would go to pattern 2)
  (void)rig.app->invoke(core::action_id("pattern.rec_free"));
  rig.pass();
  rig.position.publish(true, 2, 0, 12000);  // playing at the project's own 120.0 (Edit and playing is Rec mode: its badge and frame are in both pictures)
  rig.pass();
  const std::vector<std::uint32_t> project_tempo = rig.display.last_frame();
  rig.position.publish(true, 2, 0, 13250);  // a Tempo FX moved the sequencer's tempo (another pattern plays: no bar, only the BPM differs)
  rig.pass();
  CHECK(rig.display.last_frame() != project_tempo);
  rig.position.publish(true, 2, 0, 12000);
  rig.pass();
  CHECK(rig.display.last_frame() == project_tempo);
}

TEST_CASE("play position: Browse (the default) and Edit take the cursor to the step that plays; the free recording leaves it (OS-009c, OS-LIVEREC; Follow and Select are retired)") {
  Rig rig;
  rig.position.publish(true, 1, 20, 12000);
  rig.pass();
  CHECK(rig.app->view().row() == 20);  // Browse, the default: the cursor goes with the playhead (edit off)
  rig.mode(app::PatternMode::Edit);
  rig.position.publish(true, 1, 24, 12000);
  rig.pass();
  CHECK(rig.app->view().row() == 24);  // Edit through view.mode follows (from the keys: Shift + Rec): the cursor rides the playhead
  rig.mode(app::PatternMode::Browse);
  (void)rig.app->invoke(core::action_id("pattern.edit_toggle"));  // REC: STEP, the cursor stays (OS-RECMODE2, OS-LIVEEXIT)
  rig.position.publish(true, 1, 25, 12000);
  rig.pass();
  CHECK(rig.app->view().row() == 24);  // the cursor stays and only the play bar moves
  rig.mode(app::PatternMode::Select);  // retired (OS-LIVEREC): refused, the view stays in Edit
  CHECK(rig.app->state().mode[app::screen_index(app::ScreenId::Pattern)] == static_cast<unsigned>(app::PatternMode::Edit));
  rig.mode(app::PatternMode::Browse);
  rig.position.publish(true, 1, 26, 12000);
  rig.pass();
  CHECK(rig.app->view().row() == 26);
  rig.position.publish(false, 1, 7, 12000);  // stopped: the cursor stays where it was last moved, no jump back
  rig.pass();
  CHECK(rig.app->view().row() == 26);
  rig.mode(app::PatternMode::Follow);  // retired (OS-004d): refused, the view stays in Browse and follows
  CHECK(rig.app->state().mode[app::screen_index(app::ScreenId::Pattern)] == static_cast<unsigned>(app::PatternMode::Browse));
  rig.position.publish(true, 1, 21, 12000);
  rig.pass();
  CHECK(rig.app->view().row() == 21);
  CHECK(rig.app->view().top_row() == app::PatternView::window_top(21));  // the playing row is on the centre slot (OS-CENTER)
  rig.position.publish(true, 1, 60, 12000);
  rig.pass();
  CHECK(rig.app->view().row() == 60);
  CHECK(rig.app->view().top_row() == app::PatternView::window_top(60));
  // OS-PATCENTER: at both ends too. The first step is on the centre slot with seven blank slots above it, the last step with eight blank below (the pattern is 64 steps); the pattern loops from one to the other.
  rig.position.publish(true, 1, 63, 12000);
  rig.pass();
  CHECK((rig.app->view().row() == 63 && rig.app->view().top_row() == 63 - app::PatternView::kCentreRow));
  rig.position.publish(true, 1, 0, 12000);
  rig.pass();
  CHECK((rig.app->view().row() == 0 && rig.app->view().top_row() == -app::PatternView::kCentreRow));
  rig.position.publish(true, 1, 60, 12000);
  rig.pass();
  rig.position.publish(false, 1, 7, 12000);  // stopped: the cursor is left alone
  rig.pass();
  CHECK(rig.app->view().row() == 60);
}

TEST_CASE("play position: a block made stopped is dropped when the cursor follows the playhead (Browse, Edit); one made while following stays (OS-BUG-WATCHSEL; PAR-093); free Edit keeps its own") {
  Rig rig;
  CHECK(rig.app->invoke(core::action_id("select.extend"), 1) == app::DispatchResult::Changed);  // stopped, in Browse: Shift + Down starts a block
  CHECK(rig.app->state().edit.selection.active);
  rig.position.publish(true, 1, 5, 12000);
  rig.pass();
  CHECK_FALSE(rig.app->state().edit.selection.active);  // made stopped: the playhead drops it (PAR-093, OS-009c)
  CHECK(rig.app->view().row() == 5);
  // Browse playing (WATCH): the arrows set the block's end; the cursor stays on the playhead's row, and the block stays
  CHECK(rig.app->invoke(core::action_id("select.extend"), 1) == app::DispatchResult::Changed);
  CHECK(std::string(rig.app->message()).empty());
  CHECK(rig.app->state().edit.selection.active);
  CHECK(rig.app->state().edit.selection.start_row == 5);
  CHECK(rig.app->state().edit.selection.end_row == 6);
  CHECK(rig.app->view().row() == 5);
  rig.position.publish(true, 1, 9, 12000);
  rig.pass();
  CHECK(rig.app->view().row() == 9);
  CHECK(rig.app->state().edit.selection.active);  // the playhead moved the cursor, not the block
  CHECK(rig.app->state().edit.selection.start_row == 5);
  CHECK(rig.app->state().edit.selection.end_row == 6);
  CHECK(rig.app->invoke(core::action_id("select.extend"), 1) == app::DispatchResult::Changed);
  CHECK(rig.app->state().edit.selection.end_row == 7);  // (a held Shift keeps extending it from its own end)
  // stopped again: a block made while following does not carry to the cursor: it starts afresh
  rig.position.publish(false, 1, 9, 12000);
  rig.pass();
  CHECK(rig.app->invoke(core::action_id("select.extend"), 1) == app::DispatchResult::Changed);
  CHECK(rig.app->state().edit.selection.start_row == 9);
  CHECK(rig.app->state().edit.selection.end_row == 10);
  // Edit following (Shift + Rec) rides the playhead too (OS-LIVEREC): the same, until the Edit is made free
  (void)rig.app->invoke(core::action_id("select.clear"));
  rig.mode(app::PatternMode::Edit);
  rig.position.publish(true, 1, 5, 12000);
  rig.pass();
  CHECK_FALSE(rig.app->state().edit.selection.active);
  CHECK(rig.app->invoke(core::action_id("select.extend"), 1) == app::DispatchResult::Changed);
  CHECK(rig.app->state().edit.selection.active);
  CHECK(rig.app->state().edit.selection.end_row == 6);
  CHECK(rig.app->state().notify.size() == 0u);
  (void)rig.app->invoke(core::action_id("pattern.rec_free"));
  CHECK(rig.app->invoke(core::action_id("select.extend"), 1) == app::DispatchResult::Changed);
  rig.position.publish(true, 1, 6, 12000);
  rig.pass();
  CHECK(rig.app->state().edit.selection.active);  // the playhead leaves it alone
}

TEST_CASE("play position: Browse goes with the song to the next pattern, and to the pattern a Song Play starts on (OS-005e)") {
  Rig rig;
  CHECK(rig.app->state().edit.pattern == 1);
  rig.position.publish(true, 2, 0, 12000, 1);  // the song moves on to pattern 2: the view goes there and the cursor to the step
  rig.pass();
  CHECK(rig.app->state().edit.pattern == 2);
  CHECK(rig.app->view().row() == 0);
  rig.position.publish(true, 2, 9, 12000, 1);
  rig.pass();
  CHECK(rig.app->state().edit.pattern == 2);
  CHECK(rig.app->view().row() == 9);
  NEED(rig.app->model().project->pattern(9) == nullptr);
  rig.position.publish(true, 9, 0, 12000, 2);  // a song position names a pattern the project does not have: it plays as empty, and the view goes there (OS-PATSWITCH): an empty grid with the play bar on it, nothing created
  rig.pass();
  CHECK(rig.app->state().edit.pattern == 9);
  CHECK(rig.app->view().row() == 0);
  rig.position.publish(true, 9, 7, 12000, 2);
  rig.pass();
  CHECK(rig.app->view().row() == 7);
  CHECK(rig.app->model().project->pattern(9) == nullptr);
  rig.position.publish(true, 2, 9, 12000, 1);
  rig.pass();
  CHECK(rig.app->state().edit.pattern == 2);
  CHECK(rig.app->view().row() == 9);
  rig.position.publish(true, 1, 4, 12000, 0);  // the loop: back to the first position
  rig.pass();
  CHECK(rig.app->state().edit.pattern == 1);
  CHECK(rig.app->view().row() == 4);
  rig.position.publish(false, 2, 0, 12000, 1);  // stopped: nothing follows
  rig.pass();
  CHECK(rig.app->state().edit.pattern == 1);
  // Edit follows the song too (it records into the pattern that plays, OS-LIVEREC), the free recording does not
  rig.mode(app::PatternMode::Browse);
  (void)rig.app->invoke(core::action_id("pattern.edit_toggle"));  // REC: STEP
  rig.position.publish(true, 2, 0, 12000, 1);
  rig.pass();
  CHECK(rig.app->state().edit.pattern == 1);
  (void)rig.app->invoke(core::action_id("pattern.rec_free"));  // follow: on the pattern that plays at once
  CHECK(rig.app->state().edit.pattern == 2);
}

TEST_CASE("play position: the strip's BPM has one decimal and five cells") {
  CHECK(bpm(12800) == "128.0");
  CHECK(bpm(12000) == "120.0");
  CHECK(bpm(12044) == "120.4");
  CHECK(bpm(12045) == "120.5");  // rounded half up
  CHECK(bpm(12055) == "120.6");
  CHECK(bpm(4000) == " 40.0");
  CHECK(bpm(9999) == "100.0");
  CHECK(bpm(9949) == " 99.5");
  CHECK(bpm(80000) == "800.0");
  CHECK(bpm(0) == "  0.0");
  CHECK(bpm(4294967295u) == "999.0");  // clamped, never wider than five cells
}
