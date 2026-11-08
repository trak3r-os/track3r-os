// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SONG3 S4 (docs/specs/OS-SONG3.md section 3.4 point 1; new Community OS behaviour): the wiring of PREVIEW, F1 in a lane of the Song screen's TIMELINE page. The slot's pattern
// loops with only the rectangle's tracks sounding, through the project's solo path from the main loop (the track solo flags, applied plain — no generation moves, so the save
// layer does not call it unsaved work — and restored when the audition ends); F1 again, Stop, a live jump, a loop hold, a hand mute/solo, leaving the screen and a project load
// end it, the solo set after is the one before (a track already soloed included), and it is not an undo unit. The state itself is tests/song_preview_test.cpp.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/model.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/song_screen.h>
#include <o3os/app/song_view.h>
#include <o3os/app/view_state.h>
#include <o3os/audio/graph_params.h>
#include <o3os/core/input_event.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/limits.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;

namespace {

alignas(8) std::uint8_t g_region[model::kUndoRegionBytes];

// The transport of the test: records what was asked of it.
struct Sink final : app::TransportSink {
  std::vector<unsigned> plays;
  unsigned stops = 0;
  bool queue_jump(unsigned) noexcept override {
    ++jumps;
    return true;
  }
  unsigned jumps = 0;
  bool play(unsigned pattern) noexcept override {
    plays.push_back(pattern);
    return true;
  }
  bool play_song(unsigned) noexcept override {
    plays.push_back(0xFFFFu);
    return true;
  }
  bool stop() noexcept override {
    ++stops;
    return true;
  }
};

// An App on the Song screen over a document: patterns 1..3 with a note on step 0 of every track (pattern `empty` has none), the order `order`, `soloed` the track soloed
// before anything runs (0xFFFF: none). The save layer is attached, so dirty() is the real one.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  test::MemCard card;
  app::SaveService saves;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoHistory history;
  std::unique_ptr<model::Editor> editor;
  Sink sink;
  core::ParamRing ring;

  explicit Rig(const std::vector<std::uint8_t>& order, unsigned tracks = 6, unsigned soloed = 0xFFFFu, unsigned empty = 0) {
    model::Project& p = project();
    p.reset();
    NEED(p.set_active_tracks(tracks) == model::Status::Ok);
    for (unsigned id = 1; id <= 3; ++id) {
      NEED(p.create_pattern(id, 16) == model::Status::Ok);
      if (id == empty) {
        continue;  // an empty pattern: a slot that holds no notes
      }
      for (unsigned t = 0; t < tracks; ++t) {
        model::Step s = model::kEmptyStep;
        s.note = static_cast<std::uint8_t>(40u + id + t);
        s.instr = 1;
        NEED(p.set_step(id, t, 0, s) == model::Status::Ok);
      }
    }
    for (const std::uint8_t id : order) {
      NEED(p.append_order(id) == model::Status::Ok);
    }
    if (soloed != 0xFFFFu) {  // a track soloed before the audition: the state the audition must put back
      model::TrackSlot slot = *p.track(soloed);
      slot.flags = static_cast<std::uint8_t>(slot.flags | model::kTrackSolo);
      NEED(p.set_track(soloed, slot) == model::Status::Ok);
    }
    history.begin(g_region, sizeof g_region);
    editor = std::make_unique<model::Editor>(p, history);
    saves.attach(card, card);
    app->attach_saves(&saves);
    app->attach_document(editor.get());
    app->attach_param_ring(&ring);
    app->attach_transport(&sink);
    app->run_once();
    tap(Button::Song);  // LIST
    NEED(page() == 0);
    drain();
    saves.document_replaced(p, false);  // the clean baseline: nothing unsaved
  }
  model::Project& project() const { return test::arena(0).project; }
  const app::SongView& song() const { return app->state().song; }
  const app::ViewState& state() const { return app->state(); }
  unsigned page() const { return state().page[app::screen_index(app::ScreenId::Song)]; }
  bool dirty() { return saves.dirty(project()); }  // the save layer's real unsaved-changes answer
  void timeline() {
    if (page() != 1u) {
      tap(Button::Song);
    }
    NEED(page() == 1u);
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b, unsigned n = 1) {
    for (unsigned i = 0; i < n; ++i) {
      press(b);
      pass(1);
      release(b);
      pass(1);
    }
  }
  void shift_tap(Button b, unsigned n = 1) {
    press(Button::Shift);
    pass(1);
    tap(b, n);
    release(Button::Shift);
    pass(1);
  }
  void drain() {  // the ring's startup pushes (the strips' mute / solo) are not the audition's
    core::ParamChange c{};
    while (ring.pop(c)) {
    }
  }
  // The solo set in force now, as a bit mask (bit n = track n's solo flag), read from the project.
  std::uint16_t solo_mask() const {
    std::uint16_t m = 0;
    for (unsigned t = 0; t < model::kMaxTracks; ++t) {
      const model::TrackSlot* s = project().track(t);
      if (s != nullptr && (s->flags & model::kTrackSolo) != 0u) {
        m = static_cast<std::uint16_t>(m | (1u << t));
      }
    }
    return m;
  }
  // The Solo ParamChanges the actions pushed to the ring, split by value: `on` the tracks told 1.0, `off` those told 0.0.
  void ring_solo_masks(std::uint16_t& on, std::uint16_t& off) {
    on = 0;
    off = 0;
    core::ParamChange c{};
    while (ring.pop(c)) {
      if (core::param_kind(c.id) == core::ParamKind::Track && core::param_field(c.id) == audio::graph_param::strip::Solo) {
        if (c.value != 0.0f) {
          on = static_cast<std::uint16_t>(on | (1u << core::param_index(c.id)));
        } else {
          off = static_cast<std::uint16_t>(off | (1u << core::param_index(c.id)));
        }
      }
    }
  }
};

}  // namespace

TEST_CASE("song preview: F1 in a TIMELINE lane loops the cursor slot's pattern with only the rectangle's tracks soloed; F1 again restores the solo set and stops") {
  Rig rig({1, 2, 3});
  rig.timeline();
  rig.tap(Button::Down);  // lane 1 (track 0)
  CHECK(rig.song().in_map());
  rig.shift_tap(Button::Down, 2);  // the rectangle: tracks 0..2
  CHECK(rig.song().tfirst() == 0);
  CHECK(rig.song().tlast() == 2);
  CHECK_FALSE(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0);
  rig.drain();
  rig.tap(Button::F1);
  CHECK(rig.song().preview.active());
  CHECK(rig.state().playing);
  NEED(rig.sink.plays.size() == 1u);
  CHECK(rig.sink.plays.back() == 1u);  // the cursor's slot (0) holds pattern 1
  CHECK(rig.solo_mask() == 0x0007);    // only the rectangle's tracks sound
  std::uint16_t on = 0, off = 0;
  rig.ring_solo_masks(on, off);
  CHECK(on == 0x0007);  // the strips were told the solo ON for the rectangle's tracks...
  CHECK(off == 0x0000);  // ...and OFF for none
  rig.drain();
  rig.tap(Button::F1);  // again: the audition ends and the transport stops
  CHECK_FALSE(rig.song().preview.active());
  CHECK_FALSE(rig.state().playing);
  CHECK(rig.sink.stops == 1u);
  CHECK(rig.solo_mask() == 0);
  rig.ring_solo_masks(on, off);
  CHECK(on == 0x0000);
  CHECK(off == 0x0007);  // the end told the same tracks OFF (value 0.0), not just nothing
}

TEST_CASE("song preview: Stop (the Play key while the audition runs) ends it and restores the solo set exactly once") {
  Rig rig({1, 2, 3});
  rig.timeline();
  rig.tap(Button::Down, 3);  // lane 3 (track 2)
  rig.tap(Button::F1);
  CHECK(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0x0004);
  rig.tap(Button::Play);  // Stop
  CHECK_FALSE(rig.song().preview.active());
  CHECK_FALSE(rig.state().playing);
  CHECK(rig.sink.stops == 1u);
  CHECK(rig.solo_mask() == 0);
  rig.tap(Button::Play);  // Play again starts the song as usual: nothing is left soloed by the ended audition
  CHECK(rig.state().playing);
  CHECK(rig.sink.stops == 1u);
  CHECK(rig.solo_mask() == 0);
}

TEST_CASE("song preview: leaving the Song screen ends the audition and restores the solo set") {
  Rig rig({1, 2, 3});
  rig.timeline();
  rig.tap(Button::Down);
  rig.shift_tap(Button::Down, 3);  // tracks 0..3
  rig.tap(Button::F1);
  CHECK(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0x000F);
  rig.tap(Button::Pattern);  // another screen comes up
  CHECK(rig.state().screen == app::ScreenId::Pattern);
  CHECK_FALSE(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0);
}

TEST_CASE("song preview: the solo set after the audition is the one before it, a track already soloed included") {
  Rig rig({1, 2, 3}, 6, 3);  // track 3 is soloed before anything runs
  CHECK(rig.solo_mask() == 0x0008);
  rig.timeline();
  rig.tap(Button::Down);  // track 0
  rig.shift_tap(Button::Down, 1);  // the rectangle: tracks 0..1
  rig.tap(Button::F1);
  CHECK(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0x0003);  // the audition's own solo set: the rectangle's tracks only (the player's solo is saved, not kept)
  rig.tap(Button::F1);
  CHECK_FALSE(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0x0008);  // the one from before, track 3 still soloed
}

TEST_CASE("song preview: while the song plays, F1 in a lane becomes PLAY PAT of that slot (the audition rides on the running transport)") {
  Rig rig({1, 2, 3});
  rig.timeline();
  rig.tap(Button::F2);  // PLAY SNG
  CHECK(rig.state().playing);
  NEED(rig.sink.plays.size() == 1u);
  CHECK(rig.sink.plays.back() == 0xFFFFu);  // the song
  rig.tap(Button::Down);
  rig.tap(Button::Right);  // slot 1, lane 1
  rig.tap(Button::F1);
  CHECK(rig.song().preview.active());
  CHECK(rig.state().playing);  // still playing: now the slot's pattern
  NEED(rig.sink.plays.size() == 2u);
  CHECK(rig.sink.plays.back() == 2u);  // slot 1 holds pattern 2
  CHECK(rig.sink.stops == 0u);
  CHECK(rig.solo_mask() == 0x0001);
  rig.tap(Button::Play);  // Stop ends both
  CHECK_FALSE(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0);
}

TEST_CASE("song preview: F1 on the block row stays PLAY PAT (no solo is touched), and F1 on the LIST page too") {
  Rig rig({1, 2, 3});
  rig.timeline();
  CHECK_FALSE(rig.song().in_map());
  rig.tap(Button::F1);
  CHECK_FALSE(rig.song().preview.active());
  CHECK(rig.state().playing);
  NEED(rig.sink.plays.size() == 1u);
  CHECK(rig.sink.plays.back() == 1u);
  CHECK(rig.solo_mask() == 0);
  rig.tap(Button::F1);  // stops, as PLAY PAT does
  CHECK_FALSE(rig.state().playing);
  CHECK(rig.sink.stops == 1u);
  rig.tap(Button::Song);  // the LIST page: F1 there is PLAY PAT as well
  rig.tap(Button::Right);
  rig.tap(Button::F1);
  CHECK_FALSE(rig.song().preview.active());
  CHECK(rig.state().playing);
  CHECK(rig.solo_mask() == 0);
}

TEST_CASE("song preview: the audition is not an undo unit, resets no history and leaves the save layer's dirty() as it found it") {
  Rig rig({1, 2, 3});
  CHECK_FALSE(rig.dirty());  // clean at the baseline
  rig.timeline();
  rig.tap(Button::F3);  // ADD SLOT: an undoable edit of the song
  NEED(rig.editor->can_undo(model::UndoArea::Song));
  const std::uint32_t stale = rig.editor->stale_resets();
  const std::uint32_t gen = rig.project().generation();
  rig.tap(Button::Down);
  rig.shift_tap(Button::Down, 2);
  rig.tap(Button::F1);
  CHECK(rig.song().preview.active());
  CHECK(rig.dirty());  // the ADD SLOT before it is unsaved work
  rig.tap(Button::F1);
  CHECK_FALSE(rig.song().preview.active());
  CHECK(rig.editor->can_undo(model::UndoArea::Song));  // the history survived the audition's solo writes
  CHECK(rig.editor->stale_resets() == stale);
  CHECK(rig.editor->undo(model::UndoArea::Song) == model::UndoResult::Ok);  // and the undo still works
  CHECK(rig.solo_mask() == 0);
  CHECK(rig.project().generation() == gen + 1u);  // only the ADD SLOT moved the generation, not the audition

  Rig clean({1, 2, 3});
  CHECK_FALSE(clean.dirty());
  clean.timeline();
  clean.tap(Button::Down);
  clean.tap(Button::F1);
  CHECK(clean.song().preview.active());
  CHECK_FALSE(clean.dirty());  // the audition alone is not unsaved work
  clean.tap(Button::F1);
  CHECK_FALSE(clean.song().preview.active());
  CHECK_FALSE(clean.dirty());  // a clean project stays clean after F1, F1
}

TEST_CASE("song preview: a hand solo during the audition ends it first, then applies; the Undo after writes the player's set back, not the audition's") {
  Rig rig({1, 2, 3});
  rig.timeline();
  rig.tap(Button::Down);
  rig.shift_tap(Button::Down, 2);  // the rectangle: tracks 0..2
  rig.tap(Button::F1);
  CHECK(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0x0007);
  // the TRACKS pad layer's solo of track 5 (track.solo, one undo unit):
  NEED(rig.app->invoke(core::action_id("track.solo"), 5) == app::DispatchResult::Changed);
  CHECK_FALSE(rig.song().preview.active());  // the hand solo ended the audition first
  CHECK(rig.solo_mask() == 0x0020);          // the saved set (none) came back, then track 5 was soloed — not the audition's 0x0007
  CHECK(rig.editor->undo(model::UndoArea::Song) == model::UndoResult::Ok);  // the unit is the Song screen's (screen_undo_area)
  CHECK(rig.solo_mask() == 0x0000);  // the undo restores the state at the toggle: the saved set, not the audition's mask
}

TEST_CASE("song preview: the rectangle's tracks are clamped to the active tracks (a stale track anchor past them)") {
  Rig rig({1, 2, 3}, 6);
  rig.timeline();
  rig.tap(Button::Down);
  rig.shift_tap(Button::Down, 5);  // the rectangle: tracks 0..5
  CHECK(rig.song().tlast() == 5);
  NEED(rig.editor->set_active_tracks(4) == model::Status::Ok);  // the project shrinks under the view; no pass runs, so the anchor is stale
  (void)rig.app->invoke(core::action_id("song.play_pattern"), 0);  // invoked directly: the view is not re-fitted first
  CHECK(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0x000F);  // clamped to the 4 active tracks, not 0x003F
  rig.tap(Button::F1);
  CHECK(rig.solo_mask() == 0);
}

TEST_CASE("song preview: a live jump (Rec) and a loop hold (F2) end the audition") {
  Rig rig({1, 2, 3});
  rig.timeline();
  rig.tap(Button::Down);
  rig.shift_tap(Button::Down, 1);  // tracks 0..1
  rig.tap(Button::F1);
  CHECK(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0x0003);
  NEED(rig.app->invoke(core::action_id("song.queue_jump"), 0) == app::DispatchResult::Changed);  // Rec while playing
  CHECK_FALSE(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0);
  CHECK(rig.sink.jumps == 1u);
  rig.tap(Button::F1);  // begin again
  CHECK(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0x0003);
  NEED(rig.app->invoke(core::action_id("song.loop"), 0) == app::DispatchResult::Changed);  // F2 held
  CHECK_FALSE(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0);
}

TEST_CASE("song preview: a slot whose pattern is empty still auditions (the pattern id goes to the transport, the rectangle solos)") {
  Rig rig({1, 2, 3}, 6, 0xFFFFu, 3);  // pattern 3 has no steps; the order's last slot holds it
  rig.timeline();
  rig.tap(Button::Right, 2);  // slot 2
  rig.tap(Button::Down);       // lane 1
  rig.shift_tap(Button::Down, 1);  // tracks 0..1
  rig.tap(Button::F1);
  CHECK(rig.song().preview.active());
  CHECK(rig.state().playing);
  NEED(rig.sink.plays.size() == 1u);
  CHECK(rig.sink.plays.back() == 3u);  // the empty pattern of the cursor's slot
  CHECK(rig.solo_mask() == 0x0003);
  rig.tap(Button::F1);
  CHECK(rig.solo_mask() == 0);
}

TEST_CASE("song preview: a project load drops the audition, nothing is left soloed") {
  Rig rig({1, 2, 3});
  rig.timeline();
  rig.tap(Button::Down);
  rig.tap(Button::F1);
  CHECK(rig.song().preview.active());
  CHECK(rig.solo_mask() == 0x0001);
  // the load replaces the document: its tracks come with their own flags (none soloed here), then the document is attached
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    const model::TrackSlot* s = rig.project().track(t);
    if (s != nullptr && (s->flags & model::kTrackSolo) != 0u) {
      rig.editor->set_track_flags_plain(t, static_cast<std::uint8_t>(s->flags & ~model::kTrackSolo));
    }
  }
  rig.app->attach_document(rig.editor.get());  // the platform's load: a document is attached
  CHECK_FALSE(rig.song().preview.active());    // the audition is over, without restoring its mask into the new document
  CHECK(rig.solo_mask() == 0);
}
