// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SONGMAP (docs/specs/SONG-UX.md section 1; new Community OS behaviour): the track map as an editable grid. The cursor's column (PAT and the tracks of the map), the rectangle (Shift + arrows), Copy / Shift + Copy /
// Delete on the cells, the F6 TOOLS dialog (TRANSPOSE, INSTRUMENT, CLEAR), F5 UNIQUE, the question that asks first when an edit would also change a slot outside the rectangle, the size ceiling of the undo
// unit, and the edits while the transport runs. Every edit is ONE undo unit of the Song area (the copies of UNIQUE and the edit after them too). The planner is also run against the real journal: what it counts is
// what the editor writes.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/song_screen.h>
#include <o3os/app/song_clip.h>
#include <o3os/app/song_edit.h>
#include <o3os/app/state_line.h>
#include <o3os/app/view_state.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/core/notify.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/model/limits.h>
#include <o3os/model/step_edit.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "support/key_row.h"
#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::DispatchResult;
using model::UndoArea;

namespace {

alignas(8) std::uint8_t g_region[model::kUndoRegionBytes];

struct JumpLog final : app::TransportSink {
  bool queue_jump(unsigned) noexcept override { return true; }
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
};

constexpr std::uint8_t note_of(unsigned id, unsigned track) { return static_cast<std::uint8_t>(40u + 8u * id + track); }

// An App on the Song screen with a document and the device's three rings. Patterns 1..`count` of `length` steps; pattern p holds a note on step 0 of track t (note_of) and on step 3 of track 1.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoHistory history;
  std::unique_ptr<model::Editor> editor;
  JumpLog log;
  audio::PlayPosition position;

  explicit Rig(const std::vector<std::uint8_t>& order, unsigned patterns = 3, unsigned length = 16, unsigned tracks = 4) {
    model::Project& p = project();
    p.reset();
    NEED(p.set_active_tracks(tracks) == model::Status::Ok);
    for (unsigned id = 1; id <= patterns; ++id) {
      NEED(p.create_pattern(id, length) == model::Status::Ok);
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
    app->attach_transport(&log);
    app->attach_play_position(&position);
    app->run_once();
    tap(Button::Song);
  }
  model::Project& project() { return test::arena(0).project; }
  const app::SongView& song() const { return app->state().song; }
  const app::ViewState& state() const { return app->state(); }
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
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass(1);
  }
  void pad(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 100));
    pass(1);
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass(1);
  }
  std::string last() const { return state().notify.last_text(); }
  std::string keys() const {
    char buf[128];
    (void)app::describe_keys(app->model(), state(), buf, sizeof buf);
    return buf;
  }
  std::vector<unsigned> order() {
    std::vector<unsigned> v;
    for (std::uint32_t i = 0; i < project().song_length(); ++i) {
      v.push_back(project().order(i));
    }
    return v;
  }
  model::Step step(unsigned id, unsigned track, unsigned row = 0) { return project().pattern(id)->steps[track][row]; }
  unsigned song_depth() const { return history.ring[1].depth(); }
  std::size_t song_bytes() const { return history.ring[1].used_bytes(); }
  // The map cursor: Right enters it at track 1, `track` more presses go on.
  void enter_map(unsigned track = 0) { tap(Button::Right, 1 + track); }
};

using Order = std::vector<unsigned>;

// The Song screen drawn as it is now, in the raster of the simulator: how many pixels of `c` it has at the right of x = 290 (the track map of the Order page and nothing else that wears `c`).
unsigned pixels_at_right(Rig& rig, ui::Color c) {
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
  const ui::Rgb& want = theme.rgb(c);
  unsigned n = 0;
  for (int y = 0; y < 480; ++y) {
    for (int x = 290; x < 800; ++x) {
      const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
      n += p[0] == want.r && p[1] == want.g && p[2] == want.b ? 1u : 0u;
    }
  }
  return n;
}

bool empty_step(const model::Step& s) { return model::is_empty(s); }

}  // namespace

TEST_CASE("song map: Right enters the map at track 1, Left on track 1 returns to PAT, no wrap, clamped at both ends; Up and Down keep the column") {
  Rig rig({1, 2, 3});
  const int cells = app::song_map_cells(rig.app->model(), rig.state());
  CHECK(cells == 4);
  CHECK(rig.song().col() == 0);
  CHECK_FALSE(rig.song().in_map());
  rig.tap(Button::Left);  // PAT: nothing to the left
  CHECK(rig.song().col() == 0);
  rig.tap(Button::Right);
  CHECK(rig.song().in_map());
  CHECK(rig.song().track() == 0);
  rig.tap(Button::Right, 2);
  CHECK(rig.song().track() == 2);
  rig.tap(Button::Down);  // the column stays
  CHECK(rig.song().pos() == 1);
  CHECK(rig.song().track() == 2);
  rig.tap(Button::Up, 3);
  CHECK(rig.song().pos() == 0);
  CHECK(rig.song().track() == 2);
  rig.tap(Button::Right, 9);  // clamped at the last cell, no wrap
  CHECK(rig.song().track() == cells - 1);
  rig.tap(Button::Left, 9);   // back to PAT and no further
  CHECK(rig.song().col() == 0);
  rig.tap(Button::Left);
  CHECK(rig.song().col() == 0);
}

TEST_CASE("song map: with no map drawn the cursor stays on PAT: an empty song, no project") {
  Rig rig({1, 2, 3});
  NEED(rig.editor->clear_order() == model::Status::Ok);
  rig.pass(1);
  CHECK(app::song_map_cells(rig.app->model(), rig.state()) == 0);
  rig.tap(Button::Right);
  CHECK(rig.song().col() == 0);
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  app::App bare(input, clock, display);
  bare.run_once();
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Song), true));
  bare.run_once();
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Song), false));
  bare.run_once();
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Right), true));
  bare.run_once();
  CHECK(bare.state().song.col() == 0);
  CHECK(app::song_map_cells(bare.model(), bare.state()) == 0);
}

TEST_CASE("song map: the cursor goes back inside the map when the tracks or the list shrink under it") {
  Rig rig({1, 2, 3}, 3, 16, 12);
  rig.enter_map(7);
  CHECK(rig.song().track() == 7);
  NEED(rig.editor->set_active_tracks(5) == model::Status::Ok);
  rig.pass(1);
  rig.tap(Button::Down);  // any action syncs the view
  CHECK(rig.song().track() == 4);
}

TEST_CASE("song map: every active track has a cell while the cursor is in the map, also the empty tracks at the right: a faint tick, 12 and 16 tracks (OS-BUG-SONGMAPCUR)") {
  struct Shape {
    unsigned tracks;
    unsigned used;  // the first `used` tracks hold notes in every pattern
  };
  for (const Shape shape : {Shape{12, 8}, Shape{16, 10}}) {
    Rig rig({1, 2, 1}, 3, 16, shape.tracks);
    for (unsigned id = 1; id <= 3; ++id) {
      for (unsigned t = shape.used; t < shape.tracks; ++t) {
        NEED(rig.project().set_step(id, t, 0, model::kEmptyStep) == model::Status::Ok);
      }
    }
    rig.pass(1);
    const int cells = app::song_map_cells(rig.app->model(), rig.state());
    CHECK(cells == static_cast<int>(shape.tracks));  // the cursor reaches every active track, filled or not
    // the cursor on PAT: the map as the Song screen has always drawn it, an empty cell is nothing
    const unsigned browse = pixels_at_right(rig, ui::Color::Empty);
    CHECK(browse == 0u);
    rig.enter_map(shape.tracks - 1);  // the last track
    CHECK(rig.song().track() == static_cast<int>(shape.tracks) - 1);
    // in the map each empty cell of the 3 rows is a 20 x 2 tick (OS-SONG3 Q1b: a cell is two characters wide), but the cursor's own (the last track of slot 0) is its box
    const unsigned empties = (shape.tracks - shape.used) * 3u - 1u;
    CHECK(pixels_at_right(rig, ui::Color::Empty) == empties * 20u * 2u);
    // a selected cell is on the band: the rectangle of the last two tracks over all 3 slots has no tick of its own
    rig.shift_tap(Button::Left);
    rig.shift_tap(Button::Down, 2);
    CHECK(pixels_at_right(rig, ui::Color::Empty) == ((shape.tracks - shape.used) * 3u - 6u) * 20u * 2u);
  }
}

TEST_CASE("song map: Shift + Left / Right extend the tracks in the map only, Shift + Up / Down the slots, a plain move collapses both; the rectangle is slots x tracks") {
  Rig rig({1, 2, 3, 1});
  rig.shift_tap(Button::Right);  // on PAT: nothing
  CHECK(rig.song().col() == 0);
  CHECK_FALSE(rig.song().track_selecting());
  rig.enter_map(1);  // track 2
  rig.shift_tap(Button::Right, 2);
  CHECK(rig.song().track_selecting());
  CHECK(rig.song().tfirst() == 1);
  CHECK(rig.song().tlast() == 3);
  rig.shift_tap(Button::Right, 5);  // clamped to the last cell
  CHECK(rig.song().tlast() == 3);
  rig.shift_tap(Button::Left, 3);   // back past the anchor: the range turns round it
  CHECK(rig.song().tfirst() == 0);
  CHECK(rig.song().tlast() == 1);
  rig.shift_tap(Button::Down, 2);
  CHECK(rig.song().selecting());
  CHECK(rig.song().first() == 0);
  CHECK(rig.song().last() == 2);
  rig.tap(Button::Right);  // a plain Right: the selection (slots and tracks) collapses, the column moves
  CHECK_FALSE(rig.song().selecting());
  CHECK_FALSE(rig.song().track_selecting());
  rig.shift_tap(Button::Right);
  rig.shift_tap(Button::Down);
  rig.tap(Button::Down);  // a plain Down collapses it too, the column stays
  CHECK_FALSE(rig.song().selecting());
  CHECK_FALSE(rig.song().track_selecting());
  CHECK(rig.song().in_map());
  // the encoder keeps changing the slot's pattern in the map (D2); F5 / F6 are not PAT - / + any more (Q1)
  const unsigned now = rig.project().order(static_cast<std::uint32_t>(rig.song().pos()));
  rig.turn(1);
  CHECK(rig.project().order(static_cast<std::uint32_t>(rig.song().pos())) == now + 1u);
  rig.turn(-1);
  CHECK(rig.project().order(static_cast<std::uint32_t>(rig.song().pos())) == now);
}

TEST_CASE("song map: Copy in the map takes the cells, Shift + Copy with no selection lands the clip's top-left on the cursor's cell and drops what runs past the song; one undo unit") {
  Rig rig({1, 2, 3});
  rig.enter_map(1);  // slot 0, track 2 (index 1)
  rig.shift_tap(Button::Right);
  rig.shift_tap(Button::Down);  // slots 0..1 x tracks 2..3: a 2 x 2 clip
  rig.tap(Button::Copy);
  CHECK(rig.last() == "COPIED 2 SLOTS X 2 TRACKS");
  rig.tap(Button::Left, 2);  // a plain move collapses; the cursor on track 1 (index 0)
  rig.tap(Button::Down);
  CHECK(rig.song().pos() == 2);
  CHECK(rig.song().track() == 0);
  const unsigned depth = rig.song_depth();
  rig.shift_tap(Button::Copy);  // the clip's second row would be slot 3: the song has none, it is dropped
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.last() == "PASTED: 1 PATTERN");
  CHECK(rig.order() == Order{1, 2, 3});  // a paste never adds slots
  CHECK(rig.step(3, 0).note == note_of(1, 1));  // the clip's cell (0, 0) is pattern 1's track 2
  CHECK(rig.step(3, 1).note == note_of(1, 2));  // its cell (0, 1) is pattern 1's track 3
  CHECK(rig.step(3, 2).note == note_of(3, 2));  // the other tracks are as they were
  CHECK(rig.step(3, 3).note == note_of(3, 3));
  CHECK(rig.step(2, 0).note == note_of(2, 0));
  // one F8 takes the whole paste back
  rig.tap(Button::F8);
  CHECK(rig.step(3, 0).note == note_of(3, 0));
  CHECK(rig.step(3, 1).note == note_of(3, 1));
  CHECK(rig.song_depth() == depth);
}

TEST_CASE("song map: Shift + Copy over a selection tiles the clip to fill the rectangle, clipped to it") {
  Rig rig({1, 2, 3, 4, 5, 6}, 6);
  rig.enter_map(0);
  rig.tap(Button::Copy);  // one cell: slot 0, track 1 (pattern 1)
  CHECK(rig.last() == "COPIED 1 SLOT X 1 TRACK");
  rig.tap(Button::Down);
  rig.shift_tap(Button::Down, 3);  // slots 1..4
  rig.shift_tap(Button::Right);    // tracks 1..2
  rig.shift_tap(Button::Copy);
  CHECK(rig.last() == "PASTED: 4 PATTERNS");
  for (unsigned id = 2; id <= 5; ++id) {
    CHECK(rig.step(id, 0).note == note_of(1, 0));
    CHECK(rig.step(id, 1).note == note_of(1, 0));  // the one cell fills both tracks
    CHECK(rig.step(id, 2).note == note_of(id, 2));  // outside the rectangle
  }
  CHECK(rig.step(6, 0).note == note_of(6, 0));  // the slot below the rectangle
  CHECK(rig.step(1, 1).note == note_of(1, 1));
  rig.tap(Button::F8);
  CHECK(rig.step(3, 0).note == note_of(3, 0));
  CHECK(rig.step(5, 1).note == note_of(5, 1));

  // a two-slot clip tiles over four: the rows alternate
  rig.tap(Button::Up, 4);       // collapsed; slot 0, on the track the extension ended on (track 2)
  rig.shift_tap(Button::Down);  // slots 0..1 x track 2
  rig.tap(Button::Copy);
  CHECK(rig.last() == "COPIED 2 SLOTS X 1 TRACK");
  rig.tap(Button::Down);        // slot 2
  rig.shift_tap(Button::Down, 3);  // slots 2..5
  rig.shift_tap(Button::Copy);
  CHECK(rig.step(3, 1).note == note_of(1, 1));
  CHECK(rig.step(4, 1).note == note_of(2, 1));
  CHECK(rig.step(5, 1).note == note_of(1, 1));
  CHECK(rig.step(6, 1).note == note_of(2, 1));
  CHECK(rig.step(3, 0).note == note_of(3, 0));  // the other track
}

TEST_CASE("song map: a paste is clipped at the target track's length and says so; the lengths of the pattern never change") {
  Rig rig({1, 2});
  NEED(rig.editor->set_track_length(2, 0, 8) == model::Status::Ok);
  model::Step late = model::kEmptyStep;
  late.note = 99;
  NEED(rig.editor->set_step(1, 0, 12, late) == model::Status::Ok);
  rig.enter_map(0);
  rig.tap(Button::Copy);  // pattern 1, track 1, 16 steps
  rig.tap(Button::Down);
  const unsigned depth = rig.song_depth();
  rig.shift_tap(Button::Copy);
  CHECK(rig.last() == "PASTED: 1 PATTERN, CLIPPED AT TRACK END");
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.step(2, 0).note == note_of(1, 0));         // step 0 arrived
  CHECK(empty_step(rig.step(2, 0, 12)));               // step 12 is past the 8-step track: dropped
  CHECK(rig.project().pattern(2)->track_length[0] == 8);
  CHECK(rig.project().pattern(2)->track_length[1] == 16);
}

TEST_CASE("song map: a paste into a slot whose pattern does not exist yet creates it, in the same unit") {
  Rig rig({1, 9});  // pattern 9 is named by slot 1 and does not exist
  NEED(rig.project().pattern(9) == nullptr);
  rig.enter_map(0);
  rig.tap(Button::Copy);
  rig.tap(Button::Down);
  const unsigned depth = rig.song_depth();
  rig.shift_tap(Button::Copy);
  CHECK(rig.last() == "PASTED: 1 PATTERN");
  NEED(rig.project().pattern(9) != nullptr);
  CHECK(rig.project().pattern(9)->track_length[0] == rig.project().default_pattern_length());
  CHECK(rig.step(9, 0).note == note_of(1, 0));
  CHECK(rig.song_depth() == depth + 1u);
  rig.tap(Button::F8);  // the creation goes with it (the fence clears the Pattern history, which has nothing)
  CHECK(rig.project().pattern(9) == nullptr);
  CHECK(rig.song_depth() == depth);
  // a missing SOURCE copies as empty cells: pasting them clears the target
  rig.tap(Button::Up);
  rig.tap(Button::Down);
  rig.tap(Button::Copy);  // slot 1 (pattern 9, missing): an empty cell
  rig.tap(Button::Up);
  rig.shift_tap(Button::Copy);
  CHECK(empty_step(rig.step(1, 0)));
}

TEST_CASE("song map: Delete in the map clears every element of the rectangle's tracks once per distinct pattern, a held key does it once, lengths stay") {
  Rig rig({1, 2, 2, 3});
  model::Step full = model::kEmptyStep;
  full.note = 61;
  full.instr = 3;
  full.vol = 90;
  full.fx[0] = {12, 5};
  full.fx[1] = {13, 6};
  NEED(rig.editor->set_step(2, 1, 4, full) == model::Status::Ok);
  rig.enter_map(0);
  rig.shift_tap(Button::Right);    // tracks 1..2
  rig.shift_tap(Button::Down, 2);  // slots 0..2: patterns 1, 2, 2
  rig.press(Button::Delete);
  rig.pass(1);
  for (unsigned t = 0; t < 1500; ++t) {
    rig.pass(1);
  }
  rig.release(Button::Delete);
  rig.pass(1);
  CHECK(rig.last() == "CLEAR 2 PATTERNS?: F1 YES, OTHER KEY CANCELS");  // OS-AUDIT-69: two patterns ask first
  CHECK(rig.state().song.ask.live);
  rig.tap(Button::F1);
  CHECK(rig.last() == "CLEARED: 2 PATTERNS");
  CHECK(empty_step(rig.step(1, 0)));
  CHECK(empty_step(rig.step(1, 1)));
  CHECK(empty_step(rig.step(2, 0)));
  CHECK(empty_step(rig.step(2, 1, 4)));
  CHECK(rig.step(1, 2).note == note_of(1, 2));  // the track outside the rectangle
  CHECK(rig.step(3, 0).note == note_of(3, 0));  // the slot outside
  CHECK(rig.project().pattern(2)->track_length[0] == 16);
  CHECK(rig.song_depth() == 1u);  // one unit however many slots and repeats
  CHECK(rig.order() == Order{1, 2, 2, 3});  // Delete in the map is not DEL SLOT
  rig.tap(Button::F8);
  CHECK(rig.step(2, 1, 4).vol == 90);
  CHECK(rig.step(1, 1).note == note_of(1, 1));
  // on PAT the same key is still DEL SLOT; F4 is DEL SLOT in the map too
  rig.tap(Button::Left, 3);
  CHECK(rig.song().col() == 0);
  rig.tap(Button::Delete);
  CHECK(rig.order().size() == 3u);
  rig.enter_map(0);
  rig.tap(Button::F4);
  CHECK(rig.order().size() == 2u);
}

TEST_CASE("song map: Delete over 2 or more patterns asks first (F1 YES, one undo unit); any other key withdraws and changes nothing; one pattern stays immediate (OS-AUDIT-69)") {
  Rig rig({1, 2, 3});
  rig.enter_map(0);
  const unsigned depth = rig.song_depth();
  // one pattern: no question
  rig.tap(Button::Delete);
  CHECK(rig.last() == "CLEARED: 1 PATTERN");
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.song_depth() == depth + 1u);
  rig.tap(Button::F8);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.song_depth() == depth);
  // two patterns: the question, nothing written
  rig.shift_tap(Button::Down);  // slots 0..1
  rig.tap(Button::Delete);
  CHECK(rig.last() == "CLEAR 2 PATTERNS?: F1 YES, OTHER KEY CANCELS");
  CHECK(rig.last().size() <= 48u);
  CHECK(rig.state().song.ask.live);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.step(2, 0).note == note_of(2, 0));
  CHECK(rig.song_depth() == depth);
  // F2 and every other key withdraw
  rig.tap(Button::F2);
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.step(2, 0).note == note_of(2, 0));
  CHECK(rig.song_depth() == depth);
  rig.tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  rig.tap(Button::Right);  // a key that acts as usual: the question is gone, the cells alone
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.step(2, 0).note == note_of(2, 0));
  CHECK(rig.song_depth() == depth);
  // F1 YES: both patterns clear as one unit (the cursor is on track 1 now, so the rectangle is track 1 of slots 0..1; select the slots again)
  rig.shift_tap(Button::Up);
  rig.tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  rig.tap(Button::F1);
  CHECK(rig.last() == "CLEARED: 2 PATTERNS");
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(empty_step(rig.step(1, 1)));
  CHECK(empty_step(rig.step(2, 1)));
  CHECK(rig.step(3, 1).note == note_of(3, 1));
  CHECK(rig.song_depth() == depth + 1u);
  rig.tap(Button::F8);
  CHECK(rig.step(1, 1).note == note_of(1, 1));
  CHECK(rig.step(2, 1).note == note_of(2, 1));
  CHECK(rig.song_depth() == depth);
}

TEST_CASE("song map: Delete on a pattern that does not exist, or on a cell that is empty, says NOTHING TO CHANGE and writes nothing") {
  Rig rig({1, 9});
  rig.enter_map(0);
  rig.tap(Button::Down);  // pattern 9 does not exist
  const unsigned depth = rig.song_depth();
  rig.tap(Button::Delete);
  CHECK(rig.last() == "NOTHING TO CHANGE");
  CHECK(rig.song_depth() == depth);
  CHECK(rig.project().pattern(9) == nullptr);
}

TEST_CASE("song map: TOOLS, TRANSPOSE acts once per distinct pattern with Pattern's block rules, one unit") {
  Rig rig({1, 1, 2});
  model::Step edge = model::kEmptyStep;
  edge.note = model::kNoteLast;  // the highest note
  edge.instr = 1;
  NEED(rig.editor->set_step(1, 0, 2, edge) == model::Status::Ok);
  model::Step off = model::kEmptyStep;
  off.note = model::kNoteOff;  // not a note: untouched
  NEED(rig.editor->set_step(1, 0, 3, off) == model::Status::Ok);
  rig.enter_map(0);
  rig.shift_tap(Button::Down, 2);  // slots 0..2
  rig.tap(Button::F6);
  CHECK(rig.state().overlay == app::OverlayId::SongTools);
  CHECK(rig.state().tools.rect.s0 == 0);
  CHECK(rig.state().tools.rect.s1 == 2);
  CHECK(rig.state().tools.rect.t0 == 0);
  CHECK(rig.state().tools.rect.t1 == 0);
  CHECK(rig.state().tools.patterns == 2);
  rig.turn(2);  // OP is TRANSPOSE: the encoder moves the OP row ... the row is OP: back to TRANSPOSE below
  rig.turn(-2);
  rig.tap(Button::Down);  // the AMOUNT row
  rig.turn(3);
  CHECK(rig.state().tools.amount == 3);
  const unsigned depth = rig.song_depth();
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.last() == "TRANSPOSED: 2 PATTERNS");
  CHECK(rig.step(1, 0).note == note_of(1, 0) + 3);  // once, not once per slot
  CHECK(rig.step(2, 0).note == note_of(2, 0) + 3);
  CHECK(rig.step(1, 0, 2).note == model::kNoteLast);  // clamped at the edge, as Pattern's block edit
  CHECK(rig.step(1, 0, 3).note == model::kNoteOff);
  CHECK(rig.step(1, 1).note == note_of(1, 1));        // the track outside the rectangle
  CHECK(rig.song_depth() == depth + 1u);
  rig.tap(Button::F8);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.step(2, 0).note == note_of(2, 0));
  CHECK(rig.song_depth() == depth);
}

TEST_CASE("song map: TOOLS, AMOUNT 0 says NOTHING TO DO, F8 BACK leaves, INSTRUMENT sets the instrument of every note, CLEAR takes the chosen element") {
  Rig rig({1, 2, 3});
  model::Step s = model::kEmptyStep;
  s.note = 61;
  s.instr = 7;
  s.vol = 100;
  s.fx[0] = {12, 5};
  NEED(rig.editor->set_step(1, 0, 1, s) == model::Status::Ok);
  rig.enter_map(0);
  rig.tap(Button::F6);
  CHECK(rig.state().tools.instrument == 1);  // the instrument of the cursor's first note (pattern 1, track 1: instr 1)
  rig.tap(Button::F1);                       // TRANSPOSE by 0
  CHECK(rig.last() == "NOTHING TO DO");
  CHECK(rig.state().overlay == app::OverlayId::None);
  rig.tap(Button::F6);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
  // INSTRUMENT
  rig.tap(Button::F6);
  rig.turn(1);  // OP: INSTRUMENT
  CHECK(rig.state().tools.tool == app::SongTool::Instrument);
  rig.tap(Button::Down);
  rig.turn(4);  // TO: 5
  CHECK(rig.state().tools.instrument == 5);
  rig.tap(Button::F1);
  CHECK(rig.last() == "INSTRUMENT SET: 1 PATTERN");
  CHECK(rig.step(1, 0).instr == 5);
  CHECK(rig.step(1, 0, 1).instr == 5);
  CHECK(empty_step(rig.step(1, 0, 5)));  // a step with no note stays empty
  CHECK(rig.step(2, 0).instr == 1);      // the slot outside the rectangle
  // CLEAR, WHAT = VOLUME
  rig.tap(Button::F6);
  rig.turn(1);  // OP: CLEAR
  CHECK(rig.state().tools.tool == app::SongTool::Clear);
  rig.tap(Button::Down);
  rig.turn(-3);  // WHAT: ALL -> NOTES, VOLUME... (ALL is 4: three back is VOLUME)
  CHECK(rig.state().tools.what == app::SongWhat::Volume);
  rig.tap(Button::F1);
  CHECK(rig.step(1, 0, 1).vol == model::kVolumeUnset);
  CHECK(rig.step(1, 0, 1).note == 61);
  CHECK(rig.step(1, 0, 1).fx[0].type == 12);
}

TEST_CASE("song map: TOOLS, EXPORT WAV is the fourth OP with the OP row alone; nothing selected (on PAT, or in the map) means every track, a selection means its tracks; with no card it says so and writes nothing (OS-SONGSHARE S5)") {
  Rig rig({1, 2, 3, 1});
  CHECK(app::song_tools_rows(app::SongToolsView{}) == 2u);
  const unsigned depth = rig.song_depth();
  // on PAT
  rig.tap(Button::F6);
  rig.turn(3);  // OP: TRANSPOSE, INSTRUMENT, CLEAR, EXPORT WAV
  CHECK(rig.state().tools.tool == app::SongTool::ExportWav);
  CHECK(app::song_tools_rows(rig.state().tools) == 1u);
  char text[24];
  CHECK(app::song_tools_text(rig.state().tools, app::SongToolsRow::Op, text, sizeof text) == 10u);
  CHECK(std::string(text) == "EXPORT WAV");
  CHECK(rig.state().tools.whole);
  rig.tap(Button::Down);  // there is no second row: the cursor stays on OP
  CHECK(rig.state().tools.row == 0);
  rig.turn(3);            // OP goes on to FORM and BRIDGE (OS-SONG-BUILD) and wraps to TRANSPOSE ...
  CHECK(rig.state().tools.tool == app::SongTool::Transpose);
  rig.turn(-3);           // ... and back to EXPORT WAV
  CHECK(rig.state().tools.tool == app::SongTool::ExportWav);
  rig.tap(Button::F1);    // no card write layer in this rig: nothing is exported, nothing is written, the dialog is closed
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.last() == "NO CARD WRITE LAYER");
  CHECK(rig.song_depth() == depth);
  // in the map with nothing selected: the cursor's cell is no choice of tracks
  rig.enter_map(1);
  rig.tap(Button::F6);
  CHECK(rig.state().tools.tool == app::SongTool::ExportWav);  // (the dialog keeps its OP, as the Fill dialog does)
  CHECK(rig.state().tools.whole);
  CHECK(rig.state().tools.rect.t0 == 1);
  rig.tap(Button::F8);
  // a track range, and a slot range: both are a choice
  rig.press(Button::Shift);
  rig.tap(Button::Right);
  rig.release(Button::Shift);
  rig.pass();
  rig.tap(Button::F6);
  CHECK_FALSE(rig.state().tools.whole);
  CHECK(rig.state().tools.rect.t0 == 1);
  CHECK(rig.state().tools.rect.t1 == 2);
  rig.tap(Button::F8);
  rig.tap(Button::Left);  // the track range ends
  rig.press(Button::Shift);
  rig.tap(Button::Down);
  rig.release(Button::Shift);
  rig.pass();
  rig.tap(Button::F6);
  CHECK_FALSE(rig.state().tools.whole);
  CHECK(rig.state().tools.rect.s0 == 0);
  CHECK(rig.state().tools.rect.s1 == 1);
  rig.tap(Button::F8);
}

TEST_CASE("song map: TOOLS on PAT acts on the cursor slot's tracks, all of them; its key row is APPLY and BACK") {
  Rig rig({1, 2});
  rig.tap(Button::F6);
  CHECK(rig.state().overlay == app::OverlayId::SongTools);
  CHECK(rig.state().tools.rect.t0 == 0);
  CHECK(rig.state().tools.rect.t1 == 3);
  const test::RowRead row = test::read_owner_row(rig.app->model(), rig.state());
  CHECK(row.marked);
  CHECK(row.label[0] == "APPLY");
  CHECK(row.label[7] == "BACK");
  CHECK(row.label[1] == "");
  CHECK(rig.keys() == "F1:APPLY,F8:BACK");
  rig.tap(Button::Down);
  rig.turn(2);  // +2
  rig.tap(Button::F1);
  for (unsigned t = 0; t < 4; ++t) {
    CHECK(rig.step(1, t).note == note_of(1, t) + 2);
  }
  CHECK(rig.step(2, 0).note == note_of(2, 0));
}

TEST_CASE("song map: UNIQUE with no selection copies a shared pattern to the first free number and points the slot at it, one unit; on an unshared pattern it says so") {
  Rig rig({1, 2, 1, 3, 1});
  NEED(rig.editor->set_pattern_name(1, "INTRO", 5) == model::Status::Ok);
  NEED(rig.project().pattern(4) == nullptr);
  app::SongUnique plan;
  app::song_unique_plan(rig.project(), 0, 0, nullptr, plan);
  CHECK(plan.copies == 1u);
  CHECK(plan.first_from == 1u);
  CHECK(plan.first_to == 4u);
  const unsigned depth = rig.song_depth();
  const std::size_t before = rig.song_bytes();
  rig.tap(Button::F5);
  CHECK(rig.last() == "SLOT 00: PATTERN 04 (A COPY OF 01)");
  CHECK(rig.order() == Order{4, 2, 1, 3, 1});
  NEED(rig.project().pattern(4) != nullptr);
  CHECK(std::string(rig.project().pattern(4)->name) == "INTRO");  // the copy keeps the name, the lengths and the steps
  CHECK(rig.step(4, 0).note == note_of(1, 0));
  CHECK(rig.step(4, 1, 3).note == 0);
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.song_bytes() - before == plan.bytes);  // the planner counts what the editor writes
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 1, 3, 1});
  CHECK(rig.project().pattern(4) == nullptr);
  CHECK(rig.song_depth() == depth);
  // pattern 2 is only in slot 1
  rig.tap(Button::Down);
  rig.tap(Button::F5);
  CHECK(rig.last() == "PATTERN 02 IS ONLY IN THIS SLOT");
  CHECK(rig.order() == Order{1, 2, 1, 3, 1});
  CHECK(rig.song_depth() == depth);
}

TEST_CASE("song map: UNIQUE over a selection makes one copy for each pattern held inside and outside, slots that shared it keep sharing the copy; a full pool writes nothing") {
  Rig rig({1, 2, 1, 3, 1});
  rig.shift_tap(Button::Down, 2);  // slots 0..2: patterns 1, 2, 1; pattern 1 is also in slot 4, pattern 2 is not outside
  const std::size_t before = rig.song_bytes();
  app::SongUnique plan;
  app::song_unique_plan(rig.project(), 0, 2, nullptr, plan);
  rig.tap(Button::F5);
  CHECK(rig.last() == "UNIQUE: 1 COPY");
  CHECK(rig.order() == Order{4, 2, 4, 3, 1});
  CHECK(rig.song_bytes() - before == plan.bytes);
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 1, 3, 1});
  // a selection with nothing shared
  rig.tap(Button::Down, 2);
  rig.shift_tap(Button::Down);  // slots 2..3? pattern 1 is in slots 0, 2, 4: shared; pick slots 3 alone
  rig.tap(Button::Up);
  rig.tap(Button::Down);
  rig.shift_tap(Button::Up);
  // (slots 2..3 hold patterns 1 and 3: pattern 1 is outside too)
  const unsigned depth = rig.song_depth();
  // the pool is full: ids 4..128 exist, so no copy can be made
  for (unsigned id = 4; id <= model::kMaxPatterns; ++id) {
    NEED(rig.editor->create_pattern(id, 16) == model::Status::Ok);
  }
  NEED(rig.project().pattern_count() == model::kMaxPatterns);
  rig.tap(Button::Up, 3);
  rig.tap(Button::F5);
  CHECK(rig.last() == "NO FREE PATTERN");
  CHECK(rig.order() == Order{1, 2, 1, 3, 1});
  CHECK(rig.song_depth() == depth);
}

TEST_CASE("song map: the question asks before anything is written when an edit would also change a slot outside the rectangle; F1 ALL changes them all, F2 UNIQUE gives the rectangle copies, one unit each") {
  Rig rig({1, 2, 1});
  rig.enter_map(0);
  const unsigned depth = rig.song_depth();
  const std::size_t before = rig.song_bytes();
  rig.tap(Button::Delete);  // would clear pattern 1's track 1, which slot 2 also holds
  CHECK(rig.last() == "PATTERN 01 ALSO IN 1 SLOT: F1 ALL, F2 UNIQUE");
  CHECK(rig.state().song.ask.live);
  CHECK(rig.keys() == "F1:ALL,F2:UNIQUE");
  const test::RowRead row = test::read_row(*rig.app);
  CHECK(row.marked);
  CHECK(row.label[0] == "ALL");
  CHECK(row.label[1] == "UNIQUE");
  CHECK(row.hot == 0x03u);
  CHECK(row.label[2] == "ADD SLOT");  // the other keys keep their labels
  CHECK(rig.step(1, 0).note == note_of(1, 0));  // nothing was written
  CHECK(rig.song_depth() == depth);
  CHECK(rig.song_bytes() == before);
  // F1 ALL: the shared pattern itself, so slot 2 changes too
  rig.tap(Button::F1);
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.keys() == "");
  CHECK(rig.last() == "CLEARED: 1 PATTERN");
  CHECK(empty_step(rig.step(1, 0)));
  CHECK(rig.order() == Order{1, 2, 1});
  CHECK(rig.song_depth() == depth + 1u);
  rig.tap(Button::F8);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
}

TEST_CASE("song map: F2 UNIQUE gives the rectangle copies and edits the copies only, one F8 takes both back, the journal holds what the planners counted") {
  Rig rig({1, 2, 1});
  rig.enter_map(0);
  const unsigned depth = rig.song_depth();
  app::SongEdit edit;
  edit.op = app::SongOp::Clear;
  edit.rect = app::SongRect{0, 0, 0, 0};
  edit.elements = static_cast<std::uint8_t>(model::edit::kElAll);
  app::SongPlan plan;
  app::song_plan(rig.project(), edit, nullptr, plan);
  app::SongUnique unique;
  app::song_unique_plan(rig.project(), 0, 0, &plan.shared_ids, unique);
  const std::size_t b2 = rig.song_bytes();
  rig.tap(Button::Delete);
  rig.tap(Button::F2);
  CHECK(rig.last() == "CLEARED: 1 PATTERN + 1 COPY");
  CHECK(rig.order() == Order{4, 2, 1});
  CHECK(rig.step(1, 0).note == note_of(1, 0));  // the shared pattern is as it was
  CHECK(empty_step(rig.step(4, 0)));            // the copy is cleared
  CHECK(rig.step(4, 1).note == note_of(1, 1));  // ... on the rectangle's track only
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.song_bytes() - b2 == app::song_bytes_sum(unique.bytes, plan.bytes));
  rig.tap(Button::F8);  // ONE F8 takes copies and edit back
  CHECK(rig.order() == Order{1, 2, 1});
  CHECK(rig.project().pattern(4) == nullptr);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.song_depth() == depth);
}

TEST_CASE("song map: with Pattern history behind it, the one F8 that takes UNIQUE's copy and edit back also ends that history and says so (OS-UNDOGRP D4); without it the strip says nothing (OS-BUG-SONGUNDO)") {
  Rig rig({1, 2, 1});
  // a Pattern edit of an unrelated pattern: the PATTERN ring holds a unit
  rig.editor->begin_group(core::action_id("step.note"), 0, UndoArea::Pattern);
  model::Step edited = model::kEmptyStep;
  edited.note = 99;
  NEED(rig.editor->set_step(3, 0, 5, edited) == model::Status::Ok);
  rig.editor->end_group();
  CHECK(rig.history.ring[0].depth() == 1u);
  rig.enter_map(0);
  rig.tap(Button::Delete);
  rig.tap(Button::F2);
  CHECK(rig.last() == "CLEARED: 1 PATTERN + 1 COPY");
  CHECK(rig.order() == Order{4, 2, 1});
  CHECK(empty_step(rig.step(4, 0)));
  rig.tap(Button::F8);  // ONE F8: the copy and the edit are back together ...
  CHECK(rig.order() == Order{1, 2, 1});
  CHECK(rig.project().pattern(4) == nullptr);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.song_depth() == 0u);
  CHECK(rig.last() == "PATTERN HISTORY CLEARED");  // ... and the undone unit made a pattern, so the Pattern area's records can no longer be trusted: D4's notice
  CHECK(rig.editor->fence_cleared(UndoArea::Pattern));
  CHECK(rig.history.ring[0].depth() == 0u);
  rig.shift_tap(Button::F8);  // the redo makes the copy again and the edit over it
  CHECK(rig.order() == Order{4, 2, 1});
  CHECK(empty_step(rig.step(4, 0)));
  CHECK(rig.step(1, 0).note == note_of(1, 0));
}

TEST_CASE("song move: the first write of a move is the insert, so the order list the render reads without a check is never one slot short; on a full list a move needs no room and still succeeds (OS-SONGSHARE review, E-SONG-07)") {
  // 254 slots, one short of full. The move's unit holds an insert and a remove; redo writes them in order, so with the list full again the insert is refused at once. (Written the other way round, the remove
  // would go first and the list would be one slot short between the two writes.)
  std::vector<std::uint8_t> many(model::kMaxSongPositions - 1u, 1);
  Rig rig(many);
  rig.press(Button::Copy);
  rig.pass(1);
  rig.tap(Button::Down);  // Copy held + Down: slot 0 steps down
  rig.release(Button::Copy);
  rig.pass(1);
  CHECK(rig.song().pos() == 1);
  CHECK(rig.project().song_length() == many.size());
  rig.tap(Button::F8);
  CHECK(rig.project().song_length() == many.size());
  CHECK(rig.history.ring[1].redo_depth() == 1u);
  rig.editor->begin_group(core::action_id("step.note"), 0, UndoArea::Pattern);  // a unit of the Pattern ring (the Song ring's redo stays) that makes the list full: 255
  NEED(rig.editor->append_order(1) == model::Status::Ok);
  rig.editor->end_group();
  CHECK(rig.project().song_length() == model::kMaxSongPositions);
  // E-SONG-07: the insert and the remove of one slot are replayed as the one Project::move_order (a playing song follows its slot), which needs no room: the redo is taken and the list is never short.
  CHECK(rig.editor->redo(UndoArea::Song) == model::UndoResult::Ok);
  CHECK(rig.project().song_length() == model::kMaxSongPositions);
}

TEST_CASE("song map: several shared patterns are one question that counts them; any other key, an encoder turn, a move, a screen change or time takes the question back and acts as usual") {
  Rig rig({1, 2, 3, 1, 2, 3});
  rig.enter_map(0);
  rig.shift_tap(Button::Down, 2);  // slots 0..2, all three patterns are also held by slots 3..5
  rig.tap(Button::Delete);
  CHECK(rig.last() == "3 PATTERNS ALSO IN 3 SLOTS: F1 ALL, F2 UNIQUE");
  CHECK(rig.state().song.ask.live);
  CHECK(rig.song_depth() == 0u);
  rig.tap(Button::F2);
  CHECK(rig.order() == Order{4, 5, 6, 1, 2, 3});
  CHECK(rig.last() == "CLEARED: 3 PATTERNS + 3 COPIES");
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 3, 1, 2, 3});
  // another key: the question goes and the key acts (Down moves the cursor)
  rig.tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  rig.tap(Button::Down);
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.keys() == "");
  CHECK(rig.song().pos() == 3);  // the key acted: the cursor went on from slot 2
  CHECK(rig.song_depth() == 0u);
  // an encoder turn
  rig.tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  rig.turn(1);
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.project().order(3) == 2u);  // the encoder acted (slot 3's pattern 1 -> 2)
  rig.turn(-1);
  // Shift + Copy again: the old question is dropped and a new one asked
  rig.tap(Button::Delete);
  rig.shift_tap(Button::Copy);  // nothing copied yet: the key says so
  CHECK_FALSE(rig.state().song.ask.live);
  // time: the text leaves the strip after its time
  rig.tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  rig.pass(4500);
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.keys() == "");
  // a screen change
  rig.tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  rig.tap(Button::Pattern);
  CHECK_FALSE(rig.state().song.ask.live);
  rig.tap(Button::Song);
  CHECK(rig.keys() == "");
  // the project changed behind the question
  rig.tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  NEED(rig.editor->append_order(1) == model::Status::Ok);
  rig.pass(1);
  CHECK_FALSE(rig.state().song.ask.live);
  // with the question gone F1 is PLAY PAT again (it is the question's field level only while it is up)
  CHECK(rig.log.queue_jump(0));
}

TEST_CASE("song map: an edit over half the Song ring writes nothing and says so; below it the journal holds exactly what the planner counted") {
  Rig rig({1, 2, 3}, 3, 128, 12);
  model::Step last = model::kEmptyStep;
  last.note = 70;
  last.instr = 1;
  for (unsigned id = 1; id <= 3; ++id) {
    for (unsigned t = 0; t < 12; ++t) {
      NEED(rig.editor->set_step(id, t, 127, last) == model::Status::Ok);
    }
  }
  NEED(rig.history.ring[1].capacity_bytes() == 96u * 1024u);
  // slots 0..1 x 12 tracks: 24 spans of 128 steps, 1,804 bytes each: 43,316 bytes with the unit's header and trailer
  app::SongEdit edit;
  edit.op = app::SongOp::Transpose;
  edit.amount = 2;
  edit.rect = app::SongRect{0, 1, 0, 11};
  app::SongPlan plan;
  app::song_plan(rig.project(), edit, nullptr, plan);
  CHECK(plan.writes == 24u);
  CHECK(plan.bytes == 20u + 24u * 1804u);
  CHECK(plan.bytes <= app::song_undo_limit(96u * 1024u));
  // slots 0..2: 36 spans, 65 KB: over the ceiling of 48 KiB
  app::SongEdit big = edit;
  big.rect = app::SongRect{0, 2, 0, 11};
  app::SongPlan bigger;
  app::song_plan(rig.project(), big, nullptr, bigger);
  CHECK(bigger.bytes > app::song_undo_limit(96u * 1024u));
  rig.enter_map(0);
  rig.shift_tap(Button::Right, 11);
  rig.shift_tap(Button::Down, 2);
  rig.tap(Button::F6);
  rig.tap(Button::Down);
  rig.turn(2);
  const unsigned depth = rig.song_depth();
  const std::size_t before = rig.song_bytes();
  rig.tap(Button::F1);
  CHECK(rig.last() == "TOO BIG TO UNDO: SELECT LESS");
  CHECK(rig.step(1, 0).note == note_of(1, 0));  // nothing was written
  CHECK(rig.step(3, 11, 127).note == 70);
  CHECK(rig.song_depth() == depth);
  CHECK(rig.song_bytes() == before);
  // two slots are written, and the journal grows by exactly the plan
  rig.tap(Button::Up, 2);  // collapsed, slot 0, on track 12
  rig.tap(Button::Left, 11);
  CHECK(rig.song().track() == 0);
  rig.shift_tap(Button::Right, 11);
  rig.shift_tap(Button::Down);
  rig.tap(Button::F6);
  CHECK(rig.state().tools.amount == 2);  // the dialog's settings live for the session
  rig.tap(Button::F1);
  CHECK(rig.last() == "TRANSPOSED: 2 PATTERNS");
  CHECK(rig.step(1, 0).note == note_of(1, 0) + 2);
  CHECK(rig.step(2, 11, 127).note == 72);
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.song_bytes() - before == plan.bytes);
  CHECK(rig.history.ring[1].lost_units() == 0u);  // no unit ever outgrew the ring: the history was not reset
  rig.tap(Button::F8);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
}

TEST_CASE("song map: a Song step edit followed by a Pattern edit of the same steps: Song's F8 takes its own unit back by value and clears no other history (OS-UNDOGRP's cross-area rule)") {
  Rig rig({1, 2});
  rig.enter_map(0);
  rig.tap(Button::Delete);  // clears pattern 1 track 1 (the cells' step 0)
  CHECK(empty_step(rig.step(1, 0)));
  rig.editor->begin_group(core::action_id("step.note"), 0, UndoArea::Pattern);
  model::Step edited = model::kEmptyStep;
  edited.note = 99;
  NEED(rig.editor->set_step(1, 0, 0, edited) == model::Status::Ok);
  rig.editor->end_group();
  const unsigned pattern_depth = rig.history.ring[0].depth();
  CHECK(pattern_depth >= 1u);
  rig.tap(Button::F8);  // Song's undo: the steps go back to what they were before the Song unit
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.editor->fence_cleared() == UndoArea::Auto);  // the step edit is not structural: nothing was cleared
  CHECK(rig.history.ring[0].depth() == pattern_depth);
}

TEST_CASE("song map: the edits work while the transport runs: the model is written on the main loop, the sequencer is not told") {
  Rig rig({1, 2, 1});
  rig.position.publish(true, 1, 0, 12000, 0);
  rig.pass(1);
  CHECK(rig.state().play.playing);
  rig.enter_map(0);
  rig.tap(Button::Copy);
  rig.tap(Button::Down);
  rig.shift_tap(Button::Copy);
  CHECK(rig.last() == "PASTED: 1 PATTERN");
  CHECK(rig.step(2, 0).note == note_of(1, 0));
  rig.tap(Button::Down);
  rig.tap(Button::F5);  // UNIQUE on the slot the song may be playing: the slot is pointed at an identical copy
  CHECK(rig.order() == Order{1, 2, 4});
  CHECK(rig.step(4, 0).note == note_of(1, 0));
  rig.tap(Button::F6);
  rig.tap(Button::Down);
  rig.turn(1);
  rig.tap(Button::F1);
  CHECK(rig.last() == "TRANSPOSED: 1 PATTERN");
  CHECK(rig.state().play.playing);
}

TEST_CASE("song map: a copy of more than 32 cells says TOO BIG TO COPY and keeps the old clip; the clip is not the slot clipboard") {
  Rig rig({1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3}, 3, 16, 4);
  rig.enter_map(0);
  rig.tap(Button::Copy);
  CHECK(rig.last() == "COPIED 1 SLOT X 1 TRACK");
  rig.shift_tap(Button::Right, 3);
  rig.shift_tap(Button::Down, 10);  // 11 slots x 4 tracks = 44 cells
  rig.tap(Button::Copy);
  CHECK(rig.last() == "TOO BIG TO COPY");
  rig.tap(Button::Down);  // collapse
  rig.tap(Button::Left, 5);
  CHECK(rig.song().col() == 0);
  rig.tap(Button::Copy);  // on PAT: the slots' clipboard
  CHECK(rig.last() == "COPIED 1 SLOT");
  CHECK(rig.song().clip_count == 1);
  // the old cell clip is still there
  rig.enter_map(0);
  rig.tap(Button::Down);
  rig.shift_tap(Button::Copy);  // every pattern of this song is held by several slots: the question first
  CHECK(rig.state().song.ask.live);
  rig.tap(Button::F1);
  CHECK(rig.last() == "PASTED: 1 PATTERN");
  CHECK(rig.step(rig.project().order(static_cast<std::uint32_t>(rig.song().pos())), 0).note == note_of(1, 0));
}

TEST_CASE("song map: a paste with nothing copied, or into an empty song, says so") {
  Rig rig({1, 2});
  rig.enter_map(0);
  rig.shift_tap(Button::Copy);
  CHECK(rig.last() == "NOTHING COPIED");
  CHECK(rig.song_depth() == 0u);
}

TEST_CASE("song map: the pads' row 1 moves the map's track cursor and lights it; with no map it does nothing") {
  Rig rig({1, 2, 3}, 3, 16, 6);
  rig.pad(3);
  CHECK(rig.song().in_map());
  CHECK(rig.song().track() == 3);
  app::PadRole roles[48];
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[3] == app::PadRole::Front);
  rig.pad(5);
  CHECK(rig.song().track() == 5);
  rig.pad(8);  // track 9: the project has 6
  CHECK(rig.song().track() == 5);
  rig.tap(Button::Left, 9);
  NEED(rig.app->pad_lights(roles));
  for (unsigned t = 0; t < 12; ++t) {
    CHECK(roles[t] != app::PadRole::Front);  // on PAT no track is the cursor's
  }
  NEED(rig.editor->clear_order() == model::Status::Ok);
  rig.pass(1);
  rig.pad(2);
  CHECK(rig.song().col() == 0);
}

TEST_CASE("song map: F5 and F6 are UNIQUE and TOOLS in the key row, and Shift + Left / Right are bound in Song (they no longer fall back to the plain arrows)") {
  CHECK(std::string(app::kSongSoftKeys[4]) == "UNIQUE");
  CHECK(std::string(app::kSongSoftKeys[5]) == "TOOLS");
  CHECK(std::string(app::kSongSoftKeys[7]) == "UNDO");
  Rig rig({1, 2, 3});
  const unsigned now = rig.project().order(0);
  rig.tap(Button::F6);  // TOOLS, not PAT +
  CHECK(rig.state().overlay == app::OverlayId::SongTools);
  CHECK(rig.project().order(0) == now);
  rig.tap(Button::F8);
  rig.tap(Button::F5);  // UNIQUE: nothing shared
  CHECK(rig.project().order(0) == now);
  rig.shift_tap(Button::Left);  // on PAT: nothing
  CHECK(rig.song().col() == 0);
}

TEST_CASE("song map: Shift + Copy again while a question is up drops the old question and asks a new one; the question names the pattern it is about") {
  Rig rig({1, 2, 1, 3, 3});
  rig.enter_map(0);
  rig.tap(Button::Copy);  // slot 0, track 1: the clip
  rig.tap(Button::Down, 3);  // slot 3: pattern 3 is also in slot 4
  rig.shift_tap(Button::Copy);
  CHECK(rig.state().song.ask.live);
  CHECK(rig.last() == "PATTERN 03 ALSO IN 1 SLOT: F1 ALL, F2 UNIQUE");
  CHECK(rig.state().song.ask.first == 3);
  const unsigned depth = rig.song_depth();
  rig.shift_tap(Button::Copy);  // the key acts: the old question goes, a new one is asked
  CHECK(rig.state().song.ask.live);
  CHECK(rig.last() == "PATTERN 03 ALSO IN 1 SLOT: F1 ALL, F2 UNIQUE");
  CHECK(rig.song_depth() == depth);
  CHECK(rig.step(3, 0).note == note_of(3, 0));  // nothing was written
  rig.tap(Button::F1);                          // ALL: the shared pattern is written, both slots hold it
  CHECK(rig.step(3, 0).note == note_of(1, 0));
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.order() == Order{1, 2, 1, 3, 3});
}

// ---- the review fold of OS-SONGMAP (rev-songmap): redo, refusals, the limits of the question and the footer ------------------------------------------------------------------------

TEST_CASE("song map: Shift + F8 redoes an edit, also one that made UNIQUE's copies; a new edit drops the redo") {
  Rig rig({1, 2, 3});
  rig.enter_map(0);
  const unsigned depth = rig.song_depth();
  rig.tap(Button::Delete);  // pattern 1, track 1
  CHECK(empty_step(rig.step(1, 0)));
  rig.tap(Button::F8);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.history.ring[1].redo_depth() == 1u);
  rig.shift_tap(Button::F8);
  CHECK(empty_step(rig.step(1, 0)));
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.history.ring[1].redo_depth() == 0u);
  rig.tap(Button::F8);  // undone again, then a new edit instead of the redo: the redo is gone
  CHECK(rig.history.ring[1].redo_depth() == 1u);
  rig.tap(Button::Down);
  rig.tap(Button::Delete);  // pattern 2
  CHECK(rig.history.ring[1].redo_depth() == 0u);
  rig.shift_tap(Button::F8);  // nothing to redo: pattern 1 stays as it was undone
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(empty_step(rig.step(2, 0)));
  // an edit that made a copy (F2 UNIQUE): the redo makes the copy again, and the edit over it
  Rig two({1, 2, 1});
  two.enter_map(0);
  const unsigned d2 = two.song_depth();
  two.tap(Button::Delete);
  two.tap(Button::F2);
  CHECK(two.order() == Order{4, 2, 1});
  two.tap(Button::F8);
  CHECK(two.order() == Order{1, 2, 1});
  CHECK(two.project().pattern(4) == nullptr);
  two.shift_tap(Button::F8);
  CHECK(two.order() == Order{4, 2, 1});
  NEED(two.project().pattern(4) != nullptr);
  CHECK(empty_step(two.step(4, 0)));
  CHECK(two.step(4, 1).note == note_of(1, 1));
  CHECK(two.step(1, 0).note == note_of(1, 0));
  CHECK(two.song_depth() == d2 + 1u);
}

TEST_CASE("song map: a refused edit writes nothing and opens no unit: undo takes the edit before it, and a refusal between an undo and its redo keeps the redo") {
  Rig rig({1, 9});  // pattern 9 does not exist: Delete on it is refused (D10)
  rig.enter_map(0);
  const unsigned depth = rig.song_depth();
  rig.tap(Button::Delete);
  CHECK(empty_step(rig.step(1, 0)));
  rig.tap(Button::Down);
  rig.tap(Button::Delete);
  CHECK(rig.last() == "NOTHING TO CHANGE");
  CHECK(rig.song_depth() == depth + 1u);  // the refusal made no unit
  rig.tap(Button::Up);
  rig.tap(Button::F8);  // the unit before the refusal, not an empty one
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.song_depth() == depth);
  CHECK(rig.history.ring[1].redo_depth() == 1u);
  rig.tap(Button::Down);
  rig.tap(Button::Delete);  // refused again, between the undo and the redo
  CHECK(rig.last() == "NOTHING TO CHANGE");
  rig.shift_tap(Button::Copy);  // and a paste with nothing copied
  CHECK(rig.last() == "NOTHING COPIED");
  CHECK(rig.history.ring[1].redo_depth() == 1u);
  rig.shift_tap(Button::F8);
  CHECK(empty_step(rig.step(1, 0)));
  CHECK(rig.song_depth() == depth + 1u);
}

TEST_CASE("song map: F2 UNIQUE is refused at answer time when the copies and the edit together outgrow the unit's ceiling, although the edit alone fits and F1 ALL goes through") {
  Rig rig({1, 2, 1, 2}, 2, 128, 12);
  model::Step last = model::kEmptyStep;
  last.note = 70;
  last.instr = 1;
  for (unsigned id = 1; id <= 2; ++id) {
    for (unsigned t = 0; t < 12; ++t) {
      NEED(rig.editor->set_step(id, t, 127, last) == model::Status::Ok);
    }
  }
  const std::size_t limit = app::song_undo_limit(rig.history.ring[1].capacity_bytes());
  app::SongEdit edit;
  edit.op = app::SongOp::Clear;
  edit.rect = app::SongRect{0, 1, 0, 11};
  edit.elements = static_cast<std::uint8_t>(model::edit::kElAll);
  app::SongPlan plan;
  app::song_plan(rig.project(), edit, nullptr, plan);
  app::SongUnique unique;
  app::song_unique_plan(rig.project(), 0, 1, &plan.shared_ids, unique);
  CHECK(plan.shared == 2u);
  CHECK(plan.bytes <= limit);                                    // the edit alone fits: the question is asked
  CHECK(app::song_bytes_sum(unique.bytes, plan.bytes) > limit);  // ... the copies and the edit do not
  rig.enter_map(0);
  rig.shift_tap(Button::Right, 11);
  rig.shift_tap(Button::Down);
  const unsigned depth = rig.song_depth();
  const std::size_t before = rig.song_bytes();
  rig.tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  rig.tap(Button::F2);
  CHECK(rig.last() == "TOO BIG TO UNDO: SELECT LESS");
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.order() == Order{1, 2, 1, 2});
  CHECK(rig.project().pattern(3) == nullptr);  // no copy was made
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.song_depth() == depth);
  CHECK(rig.song_bytes() == before);
  rig.tap(Button::Delete);  // F1 ALL writes the shared patterns themselves: that fits
  rig.tap(Button::F1);
  CHECK(rig.last() == "CLEARED: 2 PATTERNS");
  CHECK(empty_step(rig.step(1, 0)));
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.song_bytes() - before == plan.bytes);
}

TEST_CASE("song map: UNIQUE says NO FREE PATTERN when every number is a pattern or a slot's although the pool is not full; nothing is written") {
  // patterns 1..56 exist; the slots name 57..255 (so no number is free) and pattern 1 twice (so it is shared)
  std::vector<std::uint8_t> order{1};
  for (unsigned id = 57; id <= model::kMaxPatternId; ++id) {
    order.push_back(static_cast<std::uint8_t>(id));
  }
  order.push_back(1);
  Rig rig(order, 56);
  NEED(rig.project().pattern_count() == 56u);
  NEED(rig.project().pattern_count() < model::kMaxPatterns);
  app::SongUnique plan;
  app::song_unique_plan(rig.project(), 0, 0, nullptr, plan);
  CHECK(plan.no_room);
  CHECK(plan.copies == 0u);
  const unsigned depth = rig.song_depth();
  const std::vector<unsigned> slots = rig.order();
  rig.tap(Button::F5);
  CHECK(rig.last() == "NO FREE PATTERN");
  CHECK(rig.order() == slots);
  CHECK(rig.project().pattern_count() == 56u);
  CHECK(rig.song_depth() == depth);
  // the same answer through the question: Delete on that cell asks, F2 UNIQUE cannot copy
  rig.enter_map(0);
  rig.tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  rig.tap(Button::F2);
  CHECK(rig.last() == "NO FREE PATTERN");
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.song_depth() == depth);
}

TEST_CASE("song map: the longest question fits the strip with its keys (48 characters), also at 100 or more patterns") {
  // the representable extremes (the counts saturate at 255)
  app::SongAsk a;
  a.shared = 255;
  a.others = 255;
  app::AskText head;
  app::AskText tail;
  app::song_ask_head(a, head);
  app::ask_tail(app::Ask::Shared, tail);
  CHECK(std::string(head.c_str()) == "255 PATS ALSO IN 255 SLOTS");
  CHECK(head.size() + tail.size() <= core::kNotifyTextChars);
  a.shared = 99;
  app::song_ask_head(a, head);
  CHECK(std::string(head.c_str()) == "99 PATTERNS ALSO IN 255 SLOTS");
  CHECK(head.size() + tail.size() <= core::kNotifyTextChars);
  // the most the model can hold: 127 patterns in the rectangle, the 128 other slots of the 255 also holding them
  std::vector<std::uint8_t> order;
  for (unsigned i = 0; i < 127; ++i) {
    order.push_back(static_cast<std::uint8_t>(1 + i));
  }
  for (unsigned i = 0; i < 128; ++i) {
    order.push_back(static_cast<std::uint8_t>(1 + i % 127));
  }
  NEED(order.size() == model::kMaxSongPositions);
  Rig rig(order, 127);
  rig.enter_map(0);
  rig.shift_tap(Button::Down, 126);
  const unsigned depth = rig.song_depth();
  rig.tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  CHECK(rig.last() == "127 PATS ALSO IN 128 SLOTS: F1 ALL, F2 UNIQUE");
  CHECK(rig.last().size() <= core::kNotifyTextChars);
  CHECK(rig.keys() == "F1:ALL,F2:UNIQUE");
  CHECK(rig.song_depth() == depth);
  rig.tap(Button::F1);
  CHECK(rig.last() == "CLEARED: 127 PATTERNS");
}

TEST_CASE("song map: a clipped paste that also made copies says both; the TOOLS footer counts the patterns that exist, not the ones a slot only names") {
  Rig rig({1, 3, 3});
  NEED(rig.editor->set_track_length(3, 0, 8) == model::Status::Ok);
  model::Step late = model::kEmptyStep;
  late.note = 99;
  NEED(rig.editor->set_step(1, 0, 12, late) == model::Status::Ok);
  rig.enter_map(0);
  rig.tap(Button::Copy);  // pattern 1, track 1, 16 steps
  rig.tap(Button::Down);  // pattern 3 (also in slot 2), whose track 1 has 8 steps
  rig.shift_tap(Button::Copy);
  CHECK(rig.state().song.ask.live);
  rig.tap(Button::F2);
  CHECK(rig.last() == "PASTED: 1 PATTERN + 1 COPY, CLIPPED");
  CHECK(rig.last().size() <= core::kNotifyTextChars);
  CHECK(rig.order() == Order{1, 4, 3});
  CHECK(rig.step(4, 0).note == note_of(1, 0));
  CHECK(empty_step(rig.step(4, 0, 12)));
  // the footer: patterns 1 and 2 exist, 9 is only named
  Rig tools({1, 9, 9, 2});
  NEED(tools.project().pattern(9) == nullptr);
  tools.enter_map(0);
  tools.shift_tap(Button::Down, 3);
  tools.tap(Button::F6);
  CHECK(tools.state().tools.patterns == 2u);
}

// ---- OS-SONGSHARE S2: CUT in the map ------------------------------------------------------------------------------------------------------------

TEST_CASE("song cut: in the map Shift + Delete fills the song clip at the press and clears the rectangle by Delete's rules, one unit; Shift + Copy puts the cells back elsewhere") {
  Rig rig({1, 2, 3});
  rig.enter_map(0);
  rig.tap(Button::Down, 2);
  rig.tap(Button::Copy);  // an earlier copy (slot 2, track 1): the cut below replaces it on F1 YES
  rig.tap(Button::Up, 2);
  rig.shift_tap(Button::Right);    // tracks 1..2
  rig.shift_tap(Button::Down);     // slots 0..1: patterns 1 and 2
  const unsigned depth = rig.song_depth();
  rig.shift_tap(Button::Delete);
  CHECK(rig.state().song.ask.live);  // OS-AUDIT-69: the cut clears two patterns, so it asks (the clip is filled by the answer)
  rig.tap(Button::F1);
  CHECK(rig.last() == "CLEARED: 2 PATTERNS");
  CHECK(empty_step(rig.step(1, 0)));
  CHECK(empty_step(rig.step(1, 1)));
  CHECK(empty_step(rig.step(2, 0)));
  CHECK(empty_step(rig.step(2, 1)));
  CHECK(rig.step(1, 2).note == note_of(1, 2));  // outside the rectangle
  CHECK(rig.step(3, 0).note == note_of(3, 0));
  CHECK(rig.order() == Order{1, 2, 3});         // the slots stay: it is the cells' cut
  CHECK(rig.song_depth() == depth + 1u);        // one unit
  // the clip holds the cells (2 slots x 2 tracks): paste them over slot 2's tracks 1..2
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  rig.tap(Button::Right);  // the cursor is in the map on slot 2, track 1
  CHECK(rig.song().in_map());
  CHECK(rig.song().pos() == 2);
  rig.shift_tap(Button::Copy);  // (the clip is 2 slots high: it is clipped at the last slot, one row left)
  CHECK(rig.step(3, 0).note == note_of(1, 0));
  CHECK(rig.step(3, 1).note == note_of(1, 1));
  rig.tap(Button::F8);
  CHECK(rig.step(3, 0).note == note_of(3, 0));
  rig.tap(Button::F8);  // the cut, in one unit
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.step(2, 1).note == note_of(2, 1));
}

TEST_CASE("song cut: a cut over 2 or more patterns leaves the clip as it was until F1 YES; a withdrawn cut keeps the earlier copy (OS-AUDIT-69)") {
  Rig rig({1, 2, 3});
  rig.enter_map(0);
  rig.tap(Button::Down);
  rig.tap(Button::Copy);  // the earlier copy: slot 1, track 1 (pattern 2)
  rig.tap(Button::Up);
  rig.shift_tap(Button::Down);  // slots 0..1: patterns 1 and 2
  rig.shift_tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  CHECK(rig.step(1, 0).note == note_of(1, 0));  // nothing written before the answer
  rig.tap(Button::Down);                        // any other key withdraws and acts as usual: the cursor goes to slot 2
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
  CHECK(rig.step(2, 0).note == note_of(2, 0));
  CHECK(rig.song().pos() == 2);
  rig.shift_tap(Button::Copy);  // the clip is still the earlier copy: pattern 2's track 1 lands on slot 2's
  CHECK(rig.step(3, 0).note == note_of(2, 0));
  rig.tap(Button::F8);
  CHECK(rig.step(3, 0).note == note_of(3, 0));
  // F1 YES: the clip then holds the cut cells
  rig.tap(Button::Up, 2);
  rig.shift_tap(Button::Down);
  rig.shift_tap(Button::Delete);
  CHECK(rig.state().song.ask.live);
  rig.tap(Button::F1);
  CHECK(rig.last() == "CLEARED: 2 PATTERNS");
  CHECK(empty_step(rig.step(1, 0)));
  rig.tap(Button::Down, 2);
  rig.shift_tap(Button::Copy);
  CHECK(rig.step(3, 0).note == note_of(1, 0));
}

TEST_CASE("song map: TOOLS CLEAR over 3 patterns applies at its F1 APPLY with no second question (OS-AUDIT-69)") {
  Rig rig({1, 2, 3});
  rig.enter_map(0);
  rig.shift_tap(Button::Down, 2);  // slots 0..2: three patterns
  rig.tap(Button::F6);
  rig.turn(2);  // OP: TRANSPOSE -> INSTRUMENT -> CLEAR
  CHECK(rig.state().tools.tool == app::SongTool::Clear);
  CHECK(rig.state().tools.patterns == 3u);  // (WHAT stays ALL)
  rig.tap(Button::F1);
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.last() == "CLEARED: 3 PATTERNS");
  CHECK(empty_step(rig.step(1, 0)));
  CHECK(empty_step(rig.step(3, 0)));
}

TEST_CASE("song cut: in the map a rectangle that a slot outside also holds fills the clip, then asks; another key leaves the clip filled and the cells alone, F1 ALL clears") {
  Rig rig({1, 2, 1});
  rig.enter_map(0);
  const unsigned depth = rig.song_depth();
  rig.shift_tap(Button::Delete);
  CHECK(rig.last() == "PATTERN 01 ALSO IN 1 SLOT: F1 ALL, F2 UNIQUE");
  CHECK(rig.state().song.ask.live);
  CHECK(rig.step(1, 0).note == note_of(1, 0));  // nothing written before the answer
  CHECK(rig.song_depth() == depth);
  rig.tap(Button::Down);  // any other key takes the question back and acts as usual: the cursor is on slot 1 (pattern 2)
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.step(1, 0).note == note_of(1, 0));  // the cells are alone
  CHECK(rig.song().pos() == 1);
  rig.shift_tap(Button::Copy);  // the clip was filled at the press: slot 0's track 1 lands on slot 1's
  CHECK(rig.last() == "PASTED: 1 PATTERN");
  CHECK(rig.step(2, 0).note == note_of(1, 0));
  rig.tap(Button::F8);
  CHECK(rig.step(2, 0).note == note_of(2, 0));
  rig.tap(Button::Up);
  rig.shift_tap(Button::Delete);  // asks again
  CHECK(rig.state().song.ask.live);
  rig.tap(Button::F1);  // ALL: the shared pattern itself, so slot 2 changes too
  CHECK(rig.last() == "CLEARED: 1 PATTERN");
  CHECK(empty_step(rig.step(1, 0)));
  CHECK(rig.order() == Order{1, 2, 1});
  CHECK(rig.song_depth() == depth + 1u);
  rig.tap(Button::F8);
  CHECK(rig.step(1, 0).note == note_of(1, 0));
}

TEST_CASE("song cut: in the map a rectangle too big for the clip is refused whole: nothing is cleared and the old clip stays") {
  Rig rig({1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3}, 3, 16, 4);
  rig.enter_map(0);
  rig.tap(Button::Copy);  // the clip holds slot 0, track 1
  CHECK(rig.last() == "COPIED 1 SLOT X 1 TRACK");
  rig.shift_tap(Button::Right, 3);
  rig.shift_tap(Button::Down, 10);  // 11 slots x 4 tracks = 44 cells
  const unsigned depth = rig.song_depth();
  rig.shift_tap(Button::Delete);
  CHECK(rig.last() == "TOO BIG TO COPY");
  CHECK_FALSE(rig.state().song.ask.live);
  for (unsigned id = 1; id <= 3; ++id) {
    CHECK(rig.step(id, 0).note == note_of(id, 0));  // nothing was cleared
  }
  CHECK(rig.song_depth() == depth);
}

TEST_CASE("song cut: in the map with nothing to cut says so and writes nothing; Shift + Delete is not DEL SLOT there") {
  Rig rig({1, 2, 3});
  rig.enter_map(0);
  const unsigned depth = rig.song_depth();
  rig.shift_tap(Button::Delete);
  CHECK(rig.last() == "CLEARED: 1 PATTERN");  // a cell that holds a note
  CHECK(rig.song_depth() == depth + 1u);
  rig.shift_tap(Button::Delete);              // ... and again: the cell is empty now, the copy takes the empty cell, the clear finds nothing
  CHECK(rig.last() == "NOTHING TO CHANGE");
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.order() == Order{1, 2, 3});
}

// ---- OS-SONGSHARE S3: MOVE in the map ---------------------------------------------------------------------------------------------------------------

TEST_CASE("song move: in the map the rectangle's slots move as a block, the column and the tracks of the rectangle stay, the steps of the patterns are untouched") {
  Rig rig({1, 2, 3, 1});
  rig.enter_map(1);                // track 2
  rig.shift_tap(Button::Right);    // tracks 2..3
  rig.shift_tap(Button::Down);     // slots 0..1
  CHECK(rig.song().tfirst() == 1);
  CHECK(rig.song().tlast() == 2);
  const unsigned depth = rig.song_depth();
  rig.press(Button::Copy);
  rig.pass(1);
  rig.tap(Button::Down);
  rig.release(Button::Copy);
  rig.pass(1);
  CHECK(rig.order() == Order{3, 1, 2, 1});
  CHECK(rig.song().in_map());
  CHECK(rig.song().track() == 2);  // the cursor's column
  CHECK(rig.song().tfirst() == 1);
  CHECK(rig.song().tlast() == 2);
  CHECK(rig.song().selecting());
  CHECK(rig.song().first() == 1);
  CHECK(rig.song().last() == 2);
  CHECK(rig.song().pos() == 2);
  CHECK(rig.song_depth() == depth + 1u);
  for (unsigned id = 1; id <= 3; ++id) {
    CHECK(rig.step(id, 0).note == note_of(id, 0));  // a move writes the order list only
  }
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 3, 1});
}

TEST_CASE("song move: a move takes a question that is up back, and the move is made") {
  Rig rig({1, 2, 1});
  rig.enter_map(0);
  rig.tap(Button::Delete);  // asks: pattern 1 is also in slot 2
  CHECK(rig.state().song.ask.live);
  rig.press(Button::Copy);
  rig.pass(1);
  rig.tap(Button::Down);
  rig.release(Button::Copy);
  rig.pass(1);
  CHECK_FALSE(rig.state().song.ask.live);
  CHECK(rig.order() == Order{2, 1, 1});
  CHECK(rig.step(1, 0).note == note_of(1, 0));  // nothing was cleared
}

// OS-SONG3 S3: a muted track's map cell is dim, the same rule as TIMELINE's lanes: the track's own mute flag (kTrackMute) only, never a solo silences anything here.
TEST_CASE("song map: a muted track's cells are TextDim, the unmuted tracks' stay Instr, a soloed track's stay Instr (OS-SONG3 S3)") {
  Rig rig({1, 2, 3});
  const unsigned instr = pixels_at_right(rig, ui::Color::Instr);
  const unsigned dim = pixels_at_right(rig, ui::Color::TextDim);
  CHECK(instr > 0u);  // every track holds a note on step 0: every cell is a Note cell in Instr
  model::TrackSlot ts = *rig.project().track(1);
  ts.flags |= model::kTrackMute;  // track 2 of 4 is muted
  (void)rig.project().set_track(1, ts);
  CHECK(pixels_at_right(rig, ui::Color::Instr) < instr);  // the muted track's cells went dim
  CHECK(pixels_at_right(rig, ui::Color::TextDim) > dim);
  ts.flags = model::kTrackSolo;  // a solo, no mute: the rule reads kTrackMute only
  (void)rig.project().set_track(1, ts);
  CHECK(pixels_at_right(rig, ui::Color::Instr) == instr);  // nothing dimmed
  CHECK(pixels_at_right(rig, ui::Color::TextDim) == dim);
}

// OS-SONG3 S3 (spec line 179): while the song plays, a thin Play line under the PAT tile of the playing row runs with the playhead through the slot's pattern, exactly as wide as the step it is on; stopped, there is none.
TEST_CASE("song list: the playing row's progress line under the PAT tile is exactly as wide as the step it is on, only while playing (OS-SONG3 S3)") {
  Rig rig({1, 2, 3});
  const ui::Theme& theme = ui::theme(rig.state().theme);
  const int hex_w = 2 * theme.geometry(ui::Face::Data).cell_w;  // the PAT tile's width
  const auto play_pixels = [&] {
    sim::EveRaster raster;
    sim::RasterDisplay display{raster};
    CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, theme);
    CHECK(r.begin(ui::Color::Pane));
    app::kSongScreen.render(rig.app->model(), rig.state(), r, ui::Rect{0, 0, 0, 0});
    CHECK(r.end());
    const ui::Rgb& want = theme.rgb(ui::Color::Play);
    unsigned n = 0;
    for (int y = 0; y < 480; ++y) {
      for (int x = 40; x < 200; ++x) {  // the PAT column (the play marker's icon and the transport badge are outside or baseline)
        const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
        n += p[0] == want.r && p[1] == want.g && p[2] == want.b ? 1u : 0u;
      }
    }
    return n;
  };
  CHECK(play_pixels() == 0u);  // stopped: no line
  rig.position.publish(true, 2, 0, 12000, 1, 0);  // slot 1 plays pattern 2, step 0 of 16: the line is empty, the rest of the frame is the baseline
  rig.pass();
  const unsigned at0 = play_pixels();
  rig.position.publish(true, 2, 8, 12000, 1, 0);  // step 8 of 16: halfway
  rig.pass();
  const unsigned half = play_pixels();
  CHECK(half - at0 == static_cast<unsigned>(2 * (hex_w * 8 / 16)));  // exactly half the tile wide, two pixels high
  rig.position.publish(true, 2, 15, 12000, 1, 0);  // step 15 of 16: nearly the whole tile
  rig.pass();
  CHECK(play_pixels() - at0 == static_cast<unsigned>(2 * (hex_w * 15 / 16)));
  rig.position.publish(false, 2, 8, 12000, 1, 0);
  rig.pass();
  CHECK(play_pixels() == 0u);  // stopped: the line is gone
}
