// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SONG3 S2 (docs/specs/OS-SONG3.md; new Community OS behaviour, no format change): the editable TIMELINE. The list's model transposed: Left / Right walk the slots, Up / Down the tracks (the lanes), the block row
// (column 0) is the whole slot, a lane is a cell; Shift + arrows extend the ranges the other way round; Copy + Left / Right moves the slots; the actions are the list's (their code does not know the page). The
// cursor is shared by the two pages. The window of kLanes (12) tracks that follows the track cursor (OS-SONG3 Q8) is SongView's.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/song_screen.h>
#include <o3os/app/song_view.h>
#include <o3os/app/view_state.h>
#include <o3os/core/input_event.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/model/editor.h>
#include <o3os/model/limits.h>
#include <o3os/model/step_edit.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;

namespace {

alignas(8) std::uint8_t g_region[model::kUndoRegionBytes];

struct Sink final : app::TransportSink {
  bool queue_jump(unsigned) noexcept override { return true; }
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
};

constexpr std::uint8_t note_of(unsigned id, unsigned track) { return static_cast<std::uint8_t>(40u + 8u * id + track); }

// An App on the Song screen's TIMELINE (page 1) over a document: patterns 1..3 with a note on step 0 of every track, the order `order`.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoHistory history;
  std::unique_ptr<model::Editor> editor;
  Sink sink;

  explicit Rig(const std::vector<std::uint8_t>& order, unsigned tracks = 4) {
    model::Project& p = project();
    p.reset();
    NEED(p.set_active_tracks(tracks) == model::Status::Ok);
    for (unsigned id = 1; id <= 3; ++id) {
      NEED(p.create_pattern(id, 16) == model::Status::Ok);
      for (unsigned t = 0; t < tracks; ++t) {
        model::Step s = model::kEmptyStep;
        s.note = note_of(id, t);
        s.instr = 1;
        NEED(p.set_step(id, t, 0, s) == model::Status::Ok);
      }
    }
    for (const std::uint8_t id : order) {
      NEED(p.append_order(id) == model::Status::Ok);
    }
    history.begin(g_region, sizeof g_region);
    editor = std::make_unique<model::Editor>(p, history);
    app->attach_document(editor.get());
    app->attach_transport(&sink);
    app->run_once();
    tap(Button::Song);  // LIST
    NEED(page() == 0);
  }
  model::Project& project() { return test::arena(0).project; }
  const app::SongView& song() const { return app->state().song; }
  const app::ViewState& state() const { return app->state(); }
  unsigned page() const { return state().page[app::screen_index(app::ScreenId::Song)]; }
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
  void copy_tap(Button b, unsigned n = 1) {  // Copy held, then the key: Copy is a held modifier on Song
    press(Button::Copy);
    pass(1);
    tap(b, n);
    release(Button::Copy);
    pass(1);
  }
  std::string last() const { return state().notify.last_text(); }
  std::vector<unsigned> order() {
    std::vector<unsigned> v;
    for (std::uint32_t i = 0; i < project().song_length(); ++i) {
      v.push_back(project().order(i));
    }
    return v;
  }
  model::Step step(unsigned id, unsigned track) { return project().pattern(id)->steps[track][0]; }
};

using Order = std::vector<unsigned>;

}  // namespace

TEST_CASE("song timeline: Left / Right walk the slots and keep the lane, Down enters the lanes and walks the tracks, Up from lane 1 returns to the block row") {
  Rig rig({1, 2, 3});
  rig.timeline();
  CHECK(rig.song().pos() == 0);
  CHECK(rig.song().col() == 0);
  rig.tap(Button::Up);  // the block row: Up does nothing
  CHECK(rig.song().col() == 0);
  CHECK(rig.song().pos() == 0);
  rig.tap(Button::Right);
  CHECK(rig.song().pos() == 1);
  CHECK(rig.song().col() == 0);
  rig.tap(Button::Down);  // lane 1
  CHECK(rig.song().in_map());
  CHECK(rig.song().track() == 0);
  CHECK(rig.song().pos() == 1);
  rig.tap(Button::Down, 2);
  CHECK(rig.song().track() == 2);
  rig.tap(Button::Right);  // the slot moves, the lane stays
  CHECK(rig.song().pos() == 2);
  CHECK(rig.song().track() == 2);
  rig.tap(Button::Right, 5);  // no wrap, clamped at the last slot
  CHECK(rig.song().pos() == 2);
  rig.tap(Button::Left, 5);
  CHECK(rig.song().pos() == 0);
  CHECK(rig.song().track() == 2);
  rig.tap(Button::Down, 9);  // clamped at the last lane (4 tracks)
  CHECK(rig.song().track() == 3);
  rig.tap(Button::Up, 3);
  CHECK(rig.song().track() == 0);
  rig.tap(Button::Up);  // from lane 1 back to the block row
  CHECK(rig.song().col() == 0);
  CHECK_FALSE(rig.song().in_map());
}

TEST_CASE("song timeline: song_map_cells is the lanes the cursor can reach: 12 at most on TIMELINE, every track up to 16 on the list") {
  Rig rig({1, 2}, 16);
  CHECK(app::song_map_cells(rig.app->model(), rig.state()) == 16);
  rig.timeline();
  CHECK(app::song_map_cells(rig.app->model(), rig.state()) == 12);
  rig.tap(Button::Down, 40);
  CHECK(rig.song().track() == 11);
  Rig few({1}, 5);
  few.timeline();
  CHECK(app::song_map_cells(few.app->model(), few.state()) == 5);
  Rig none({}, 4);
  none.timeline();
  CHECK(app::song_map_cells(none.app->model(), none.state()) == 0);  // an empty song has no cells
  none.tap(Button::Down);
  CHECK_FALSE(none.song().in_map());
}

TEST_CASE("song timeline: the cursor is shared by the two pages, and a track the lanes do not reach is clamped to the last lane when the page turns") {
  Rig rig({1, 2, 3}, 16);
  rig.tap(Button::Down);  // the list: slot 1
  rig.tap(Button::Right, 3);  // track 3 of the map
  CHECK(rig.song().track() == 2);
  rig.timeline();  // same slot, same track
  CHECK(rig.song().pos() == 1);
  CHECK(rig.song().track() == 2);
  rig.tap(Button::Down, 20);  // the last lane
  CHECK(rig.song().track() == 11);
  rig.tap(Button::Song);  // back to the list: the cursor is where it was
  CHECK(rig.page() == 0u);
  CHECK(rig.song().pos() == 1);
  CHECK(rig.song().track() == 11);
  rig.tap(Button::Right, 4);  // the list reaches tracks 13..16
  CHECK(rig.song().track() == 15);
  rig.tap(Button::Song);  // TIMELINE has 12 lanes: the cursor goes to the last of them
  CHECK(rig.page() == 1u);
  CHECK(rig.song().track() == 11);
  CHECK(rig.song().tbase() == 0);
}

TEST_CASE("song view: the 12-lane window follows the track cursor, scrolls one track at a time and is back at 0 on PAT") {
  app::SongView v;
  const int cells = 16;
  CHECK(v.tbase() == 0);
  for (int i = 0; i < 12; ++i) {
    (void)v.step_col(1, cells);
  }
  CHECK(v.track() == 11);
  CHECK(v.tbase() == 0);  // track 12 is the last of the first window
  (void)v.step_col(1, cells);
  CHECK(v.track() == 12);
  CHECK(v.tbase() == 1);
  for (int i = 0; i < 3; ++i) {
    (void)v.step_col(1, cells);
  }
  CHECK(v.track() == 15);
  CHECK(v.tbase() == 4);
  (void)v.step_col(1, cells);  // clamped at the last cell
  CHECK(v.tbase() == 4);
  (void)v.step_col(-1, cells);  // going back, the window stays until the cursor reaches its left edge
  CHECK(v.track() == 14);
  CHECK(v.tbase() == 4);
  for (int i = 0; i < 10; ++i) {
    (void)v.step_col(-1, cells);
  }
  CHECK(v.track() == 4);
  CHECK(v.tbase() == 4);
  (void)v.step_col(-1, cells);
  CHECK(v.tbase() == 3);
  for (int i = 0; i < 20; ++i) {
    (void)v.step_col(-1, cells);
  }
  CHECK(v.col() == 0);
  CHECK(v.tbase() == 0);
  CHECK(v.set_track(14, cells));  // the pads
  CHECK(v.tbase() == 3);
  CHECK(v.fit_cols(12));  // a page with fewer cells: the cursor and the window inside them
  CHECK(v.track() == 11);
  CHECK(v.tbase() == 0);
  app::SongView w;
  (void)w.step_col(1, 8);  // never a window past the cells
  (void)w.step_col(1, 8);
  CHECK(w.tbase() == 0);
}

TEST_CASE("song timeline: Shift + Left / Right extend the range of slots, Shift + Up / Down the range of tracks (in a lane only), and a plain move collapses both") {
  Rig rig({1, 2, 3}, 4);
  rig.timeline();
  rig.shift_tap(Button::Down);  // on the block row: no lane to extend
  CHECK_FALSE(rig.song().track_selecting());
  CHECK_FALSE(rig.song().in_map());
  rig.shift_tap(Button::Right, 2);  // slots 0..2, the whole slots
  CHECK(rig.song().selecting());
  CHECK(rig.song().first() == 0);
  CHECK(rig.song().last() == 2);
  rig.tap(Button::Left);  // a plain move collapses
  CHECK_FALSE(rig.song().selecting());
  CHECK(rig.song().pos() == 1);
  rig.tap(Button::Down);  // lane 1
  rig.shift_tap(Button::Down, 2);
  CHECK(rig.song().track_selecting());
  CHECK(rig.song().tfirst() == 0);
  CHECK(rig.song().tlast() == 2);
  CHECK(rig.song().pos() == 1);
  rig.shift_tap(Button::Right);
  CHECK(rig.song().first() == 1);
  CHECK(rig.song().last() == 2);
  CHECK(rig.song().tlast() == 2);  // the rectangle: slots x tracks
  rig.tap(Button::Up);
  CHECK_FALSE(rig.song().selecting());
  CHECK_FALSE(rig.song().track_selecting());
}

TEST_CASE("song timeline: Copy, Shift + Copy, Delete and Shift + Delete act on the cells in a lane and on the slots on the block row, as the list's map and PAT do") {
  Rig rig({1, 2, 3}, 4);
  rig.timeline();
  rig.tap(Button::Down);  // slot 0, lane 1
  rig.shift_tap(Button::Right, 1);
  rig.shift_tap(Button::Down, 1);  // slots 0..1 x lanes 1..2
  rig.tap(Button::Copy);
  CHECK(rig.last() == "COPIED 2 SLOTS X 2 TRACKS");
  rig.tap(Button::Delete);  // 2 patterns: asks first
  rig.tap(Button::F1);
  CHECK(rig.last() == "CLEARED: 2 PATTERNS");
  CHECK(model::is_empty(rig.step(1, 0)));
  CHECK(model::is_empty(rig.step(1, 1)));
  CHECK(model::is_empty(rig.step(2, 1)));
  CHECK_FALSE(model::is_empty(rig.step(1, 2)));  // the other lanes stay
  CHECK_FALSE(model::is_empty(rig.step(3, 0)));  // and the other slots
  CHECK(rig.order() == Order{1, 2, 3});  // Delete in a lane clears cells, it never deletes a slot
  rig.tap(Button::Right, 2);  // slot 2
  rig.tap(Button::Up, 3);     // the block row
  CHECK(rig.song().col() == 0);
  rig.tap(Button::Delete);  // the block row: DEL SLOT
  CHECK(rig.order() == Order{1, 2});
  rig.tap(Button::Left);
  rig.shift_tap(Button::Delete);  // CUT on the block row: the slot goes into the slot clipboard
  CHECK(rig.last() == "CUT 1 SLOT");
  CHECK(rig.order() == Order{2});
}

TEST_CASE("song timeline: Copy + Left / Right moves the slot along the song, Copy + Up / Down does nothing there and moves it on the list, Copy + Left / Right does nothing on the list") {
  Rig rig({1, 2, 3});
  rig.timeline();
  rig.copy_tap(Button::Right);
  CHECK(rig.order() == Order{2, 1, 3});
  CHECK(rig.song().pos() == 1);  // the cursor follows
  rig.copy_tap(Button::Down);  // Down is the tracks on TIMELINE: no move
  CHECK(rig.order() == Order{2, 1, 3});
  rig.copy_tap(Button::Left);
  CHECK(rig.order() == Order{1, 2, 3});
  CHECK(rig.song().pos() == 0);
  rig.copy_tap(Button::Left);  // the first slot: nothing moves
  CHECK(rig.order() == Order{1, 2, 3});
  rig.shift_tap(Button::Right);  // a range of two slots moves as a block
  rig.copy_tap(Button::Right);
  CHECK(rig.order() == Order{3, 1, 2});
  CHECK(rig.song().first() == 1);
  CHECK(rig.song().last() == 2);
  rig.tap(Button::Song);  // the list
  CHECK(rig.page() == 0u);
  rig.tap(Button::Up);  // collapse the range, cursor on slot 1
  const Order before = rig.order();
  rig.copy_tap(Button::Right);  // the list's Left / Right are the columns: no move
  CHECK(rig.order() == before);
  rig.copy_tap(Button::Down);
  CHECK(rig.order() != before);
}

TEST_CASE("song timeline: the page draws with the cursor in a lane and a rectangle selected, and the frame fits the display list") {
  Rig rig({1, 2, 3}, 12);
  rig.timeline();
  rig.tap(Button::Down, 2);
  rig.shift_tap(Button::Right, 2);
  rig.shift_tap(Button::Down, 3);
  const ui::Theme& theme = ui::theme(rig.state().theme);
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, theme);
  CHECK(r.begin(ui::Color::Pane));
  app::kSongScreen.render(rig.app->model(), rig.state(), r, ui::Rect{0, 0, 0, 0});
  CHECK(r.end());
  // The band is Accent: pixels of it are on the page below the blocks (the lanes), none above them.
  const ui::Rgb& accent = theme.rgb(ui::Color::Accent);
  unsigned in_lanes = 0;
  for (int y = 200; y < 440; ++y) {
    for (int x = 0; x < 800; ++x) {
      const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
      in_lanes += p[0] == accent.r && p[1] == accent.g && p[2] == accent.b ? 1u : 0u;
    }
  }
  CHECK(in_lanes > 200u);
}

TEST_CASE("song timeline: a muted lane's ticks are dim, an unmuted lane's bright (OS-SONG3 S3)") {
  Rig rig({1, 2, 3}, 4);
  rig.timeline();
  const ui::Theme& theme = ui::theme(rig.state().theme);
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, theme);
  auto render = [&] {
    for (int frame = 0; frame < 2; ++frame) {  // the first frame takes the note-map pictures, the second draws them
      CHECK(r.begin(ui::Color::Pane));
      app::kSongScreen.render(rig.app->model(), rig.state(), r, ui::Rect{0, 0, 0, 0});
      CHECK(r.end());
    }
  };
  render();  // unmuted: every lane's ticks are Text
  std::vector<std::uint8_t> unmuted(raster.rgba(), raster.rgba() + sim::EveRaster::kFramebufferBytes);
  model::TrackSlot lane2 = *rig.project().track(1);
  lane2.flags |= model::kTrackMute;  // lane 2 of 4 is muted
  (void)rig.project().set_track(1, lane2);
  render();
  // A tick of the muted lane went Text -> TextDim; a tick of an unmuted lane stayed Text.
  const ui::Rgb& dim = theme.rgb(ui::Color::TextDim);
  const ui::Rgb& text = theme.rgb(ui::Color::Text);
  unsigned dimmed = 0, kept = 0;
  for (std::size_t i = 0; i + 3u < unmuted.size(); i += 4u) {
    const bool was_text = unmuted[i] == text.r && unmuted[i + 1u] == text.g && unmuted[i + 2u] == text.b;
    const std::uint8_t* p = raster.rgba() + i;
    const bool now_dim = p[0] == dim.r && p[1] == dim.g && p[2] == dim.b;
    const bool now_text = p[0] == text.r && p[1] == text.g && p[2] == text.b;
    dimmed += was_text && now_dim ? 1u : 0u;
    kept += was_text && now_text ? 1u : 0u;
  }
  CHECK(dimmed > 0u);  // the muted lane's pixels really differ
  CHECK(kept > 0u);    // the unmuted lanes' did not
}

// OS-SONG3 S3: the shared mark on the blocks is really drawn — F3 makes a second slot hold the cursor's pattern, and the blocks of both slots wear the `*` in the Fx ink (nothing else on the page wears Fx).
TEST_CASE("song timeline: a shared pattern's blocks wear the `*` in the Fx ink, an unshared one's do not (OS-SONG3 S3)") {
  Rig rig({1, 2, 3}, 4);
  rig.timeline();
  const ui::Theme& theme = ui::theme(rig.state().theme);
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, theme);
  const auto fx_pixels = [&] {
    for (int frame = 0; frame < 2; ++frame) {  // the first frame takes the note-map pictures, the second draws them
      CHECK(r.begin(ui::Color::Pane));
      app::kSongScreen.render(rig.app->model(), rig.state(), r, ui::Rect{0, 0, 0, 0});
      CHECK(r.end());
    }
    const ui::Rgb& fx = theme.rgb(ui::Color::Fx);
    unsigned n = 0;
    for (std::size_t i = 0; i + 3u < sim::EveRaster::kFramebufferBytes; i += 4u) {
      const std::uint8_t* p = raster.rgba() + i;
      n += p[0] == fx.r && p[1] == fx.g && p[2] == fx.b ? 1u : 0u;
    }
    return n;
  };
  CHECK(fx_pixels() == 0u);  // no pattern shared yet: no Fx anywhere on the page
  rig.tap(Button::F3);      // a slot after the cursor's with its pattern: slots 0 and 1 both hold pattern 1
  CHECK(rig.order() == std::vector<unsigned>{1, 1, 2, 3});
  CHECK(fx_pixels() > 0u);   // the two blocks wear the `*` in the Fx ink (the cursor's pattern)
}
