// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SONG1: the Song screen's editing: add / delete slot, the slot's pattern, the duplicate as a new pattern, copy and paste of a range of slots, the tempo field
// and its tap, F1 PLAY PAT; each an undo unit through the Editor. The track map and the live jump have their own tests below and in seq_sequencer_test.cpp.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/mixer_view.h>
#include <o3os/app/model.h>
#include <o3os/app/song_map.h>
#include <o3os/app/song_source.h>
#include <o3os/app/screens/song_screen.h>
#include <o3os/app/view_state.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/limits.h>
#include <o3os/model/step.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/seq/sequencer.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::DispatchResult;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];

struct TransportLog final : app::TransportSink {
  unsigned plays = 0, stops = 0, songs = 0, pattern = 0, jumps = 0, jump_row = 0;
  bool accept_jumps = true;
  bool queue_jump(unsigned row) noexcept override {
    if (!accept_jumps) {
      return false;
    }
    ++jumps;
    jump_row = row;
    return true;
  }
  bool play(unsigned p) noexcept override {
    pattern = p;
    ++plays;
    return true;
  }
  bool play_song(unsigned) noexcept override {
    ++songs;
    return true;
  }
  bool stop() noexcept override {
    ++stops;
    return true;
  }
};

// An App on the Song screen with a document: patterns 1..3 (16 steps) and the song `order`.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<core::ParamRing> params = std::make_unique<core::ParamRing>();

  explicit Rig(const std::vector<std::uint8_t>& order = {1, 2, 3}) {
    model::Project& p = test::arena(0).project;
    p.reset();
    for (unsigned id = 1; id <= 3; ++id) {
      NEED(p.create_pattern(id, 16) == model::Status::Ok);
    }
    for (const std::uint8_t id : order) {
      NEED(p.append_order(id) == model::Status::Ok);
    }
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_param_ring(params.get());
    app->run_once();
    tap(Button::Song);
  }
  model::Project& project() { return test::arena(0).project; }
  const app::SongView& song() { return app->state().song; }
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
  // OS-APPSNAV: the chord Song + Pattern, both let go together: the Navigator.
  void open_navigator() {
    press(Button::Song);
    pass(1);
    press(Button::Pattern);
    pass(1);
    release(Button::Pattern);
    release(Button::Song);
    pass(1);
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass(1);
  }
  void shift_turn(int detents) {
    press(Button::Shift);
    pass(1);
    turn(detents);
    release(Button::Shift);
    pass(1);
  }
  std::vector<unsigned> order() {
    std::vector<unsigned> v;
    for (std::uint32_t i = 0; i < project().song_length(); ++i) {
      v.push_back(project().order(i));
    }
    return v;
  }
  std::string message() const { return app->message(); }
  void flush() {
    pass(1);
    pass(10000);
  }
};

using Order = std::vector<unsigned>;

// The bytes a unit of `n` order-list records takes in the ring (an insert, a remove and a set_order each carry a payload of 2 or 3 bytes, a word either way): what a move wrote, counted from the journal.
constexpr std::size_t records_bytes(unsigned n) { return model::UndoJournal::unit_bytes_for(3) + (n - 1u) * (model::UndoJournal::kRecordOverheadBytes + 4u); }

}  // namespace

TEST_CASE("song edit: F3 and Insert add a slot after the cursor holding its pattern, the cursor on it; undo and redo") {
  Rig rig;
  rig.tap(Button::Down);
  CHECK(rig.song().pos() == 1);
  rig.tap(Button::F3);
  CHECK(rig.order() == Order{1, 2, 2, 3});
  CHECK(rig.song().pos() == 2);
  rig.tap(Button::Insert);
  CHECK(rig.order() == Order{1, 2, 2, 2, 3});
  CHECK(rig.song().pos() == 3);
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 2, 3});
  rig.shift_tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 2, 2, 3});
  rig.tap(Button::F8);
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 3});
  CHECK(rig.project().check_invariants());
}

TEST_CASE("song edit: adding to an empty list makes pattern 1; a full list says SONG FULL and changes nothing") {
  Rig rig{std::vector<std::uint8_t>{}};
  CHECK(rig.order().empty());
  rig.tap(Button::F3);
  CHECK(rig.order() == Order{1});
  CHECK(rig.song().pos() == 0);
  Rig full(std::vector<std::uint8_t>(255, 2));
  CHECK(full.order().size() == 255u);
  const std::uint32_t generation = full.project().generation();
  full.tap(Button::F3);
  CHECK(full.message() == "SONG FULL");
  CHECK(full.project().generation() == generation);
  full.shift_tap(Button::Insert);
  CHECK(full.message() == "SONG FULL");
  CHECK(full.project().pattern_count() == 3u);  // a refused duplicate made no pattern
}

TEST_CASE("song edit: F4 and Delete remove the slot and put the cursor on the one above (our rule), the last slot stays") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3, 2});
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::F4);
  CHECK(rig.order() == Order{1, 2, 2});
  CHECK(rig.song().pos() == 1);
  rig.tap(Button::Up);
  rig.tap(Button::Delete);
  CHECK(rig.order() == Order{2, 2});
  CHECK(rig.song().pos() == 0);
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 2});
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 3, 2});
  Rig one(std::vector<std::uint8_t>{3});
  one.tap(Button::F4);
  CHECK(one.message() == "SONG NEEDS A SLOT");
  CHECK(one.order() == Order{3});
}

// OS-SONGMAP (Q1, to confirm): F5 and F6 are UNIQUE and TOOLS now, so song.pattern_delta (the old PAT - / PAT +) has no key; the action stays registered and the encoder does the same.
TEST_CASE("song edit: the encoder and song.pattern_delta change the slot's pattern, clamped to 1..255, each an undo unit") {
  Rig rig;
  const auto delta = [&](int arg) {
    (void)rig.app->invoke(core::action_id("song.pattern_delta"), arg);
    rig.pass(1);
  };
  delta(1);
  CHECK(rig.order() == Order{2, 2, 3});
  delta(-1);
  delta(-1);  // 1: the lowest
  CHECK(rig.order() == Order{1, 2, 3});
  rig.turn(5);
  CHECK(rig.order() == Order{6, 2, 3});
  CHECK(rig.song().pos() == 0);  // the encoder edits the slot, it does not scroll the list
  rig.shift_turn(2);
  CHECK(rig.order() == Order{8, 2, 3});
  rig.turn(-1000);
  CHECK(rig.order() == Order{1, 2, 3});
  rig.turn(30000);
  CHECK(rig.order() == Order{255, 2, 3});
  delta(1);
  CHECK(rig.order() == Order{255, 2, 3});
  rig.tap(Button::F8);  // undo takes the units back one at a time: 1 -> 255, then 8 -> 1 (the clamped one that changed nothing is none)
  CHECK(rig.order() == Order{1, 2, 3});
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{8, 2, 3});
  CHECK(rig.project().check_invariants());
}

TEST_CASE("song edit: Shift + Insert adds a slot with a COPY of the pattern at the first free number, one undo unit") {
  Rig rig;
  model::Project& p = rig.project();
  CHECK(p.set_pattern_name(2, "VERSE", 5) == model::Status::Ok);
  CHECK(p.set_track_length(2, 5, 9) == model::Status::Ok);
  model::Step a = model::kEmptyStep;
  a.note = 61;
  a.instr = 2;
  a.fx[1] = {7, 9};
  CHECK(p.set_step(2, 0, 3, a) == model::Status::Ok);
  CHECK(p.set_step(2, 5, 120, a) == model::Status::Ok);  // past the track's length: kept
  rig.tap(Button::Down);
  rig.editor->history_reset();  // the setup above went round the editor
  rig.shift_tap(Button::Insert);
  CHECK(rig.order() == Order{1, 2, 4, 3});
  CHECK(rig.song().pos() == 2);
  const model::Pattern* copy = p.pattern(4);
  const model::Pattern* orig = p.pattern(2);
  NEED(copy != nullptr);
  NEED(orig != nullptr);
  CHECK(std::strcmp(copy->name, "VERSE") == 0);
  CHECK(std::memcmp(copy->track_length, orig->track_length, sizeof orig->track_length) == 0);
  CHECK(std::memcmp(copy->steps, orig->steps, sizeof orig->steps) == 0);
  CHECK(copy->steps[0][3].note == 61);
  CHECK(p.pattern_count() == 4u);
  // It is a copy: editing it leaves the original alone.
  CHECK(p.set_step(4, 0, 3, model::kEmptyStep) == model::Status::Ok);
  CHECK(orig->steps[0][3].note == 61);
  CHECK(p.set_step(4, 0, 3, a) == model::Status::Ok);
  rig.editor->history_reset();
  rig.shift_tap(Button::Insert);
  CHECK(rig.order() == Order{1, 2, 4, 5, 3});  // the slot's pattern is now 4: its copy is 5
  rig.tap(Button::F8);                          // one unit: the slot and the pattern together
  CHECK(rig.order() == Order{1, 2, 4, 3});
  CHECK(p.pattern(5) == nullptr);
  CHECK(p.pattern_count() == 4u);
  rig.shift_tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 4, 5, 3});
  NEED(p.pattern(5) != nullptr);
  CHECK(std::memcmp(p.pattern(5)->steps, copy->steps, sizeof copy->steps) == 0);
  CHECK(std::strcmp(p.pattern(5)->name, "VERSE") == 0);
  CHECK(p.check_invariants());
}

TEST_CASE("song edit: Shift + Insert on a slot whose pattern does not exist yet makes an empty pattern; a full pool refuses and records nothing") {
  Rig rig(std::vector<std::uint8_t>{9});
  rig.shift_tap(Button::Insert);
  CHECK(rig.order() == Order{9, 4});
  NEED(rig.project().pattern(4) != nullptr);
  CHECK(rig.project().pattern_length(4) == rig.project().default_pattern_length());
  Rig full;
  model::Project& p = full.project();
  for (unsigned id = 4; id <= model::kMaxPatterns; ++id) {
    NEED(p.create_pattern(id, 8) == model::Status::Ok);
  }
  full.editor->history_reset();
  const Order before = full.order();
  full.shift_tap(Button::Insert);
  CHECK(full.message() == "NO FREE PATTERN");
  CHECK(full.order() == before);
  CHECK_FALSE(full.editor->can_undo());
  full.tap(Button::F3);  // the plain add still works with a full pool: it names an existing pattern
  CHECK(full.order() == Order{1, 1, 2, 3});
}

TEST_CASE("song edit: Shift + arrows select a range; Delete removes it, Copy and Shift + Copy insert it at the cursor") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3, 1, 2});
  rig.shift_tap(Button::Down);
  rig.shift_tap(Button::Down);
  CHECK(rig.song().selecting());
  CHECK(rig.song().first() == 0);
  CHECK(rig.song().last() == 2);
  rig.tap(Button::Copy);
  CHECK(rig.message() == "COPIED 3 SLOTS");
  rig.tap(Button::Up);  // a plain move collapses the range
  CHECK_FALSE(rig.song().selecting());
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.song().pos() == 3);
  rig.shift_tap(Button::Copy);  // inserted at the cursor: the old slot 3 and the ones after it move down
  CHECK(rig.order() == Order{1, 2, 3, 1, 2, 3, 1, 2});
  CHECK(rig.song().pos() == 3);
  CHECK(rig.song().selecting());
  CHECK(rig.song().first() == 3);
  CHECK(rig.song().last() == 5);
  rig.tap(Button::F8);  // the whole paste is one unit
  CHECK(rig.order() == Order{1, 2, 3, 1, 2});
  rig.shift_tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 3, 1, 2, 3, 1, 2});
  rig.shift_tap(Button::Down);  // (the undo took the range away: select it again)
  rig.shift_tap(Button::Down);
  CHECK(rig.song().first() == 3);
  CHECK(rig.song().last() == 5);
  rig.tap(Button::Delete);  // the range goes, the cursor to the slot above it
  CHECK(rig.order() == Order{1, 2, 3, 1, 2});
  CHECK(rig.song().pos() == 2);
  CHECK_FALSE(rig.song().selecting());
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 3, 1, 2, 3, 1, 2});
}

TEST_CASE("song edit: Copy of one slot, a paste of nothing, a range that is the whole list, a paste into a full list") {
  Rig rig;
  rig.shift_tap(Button::Copy);
  CHECK(rig.message() == "NOTHING COPIED");
  rig.flush();
  rig.tap(Button::Down);
  rig.tap(Button::Copy);
  CHECK(rig.message() == "COPIED 1 SLOT");
  rig.shift_tap(Button::Copy);
  CHECK(rig.order() == Order{1, 2, 2, 3});
  CHECK_FALSE(rig.song().selecting());  // one slot pasted: no range
  rig.tap(Button::Up);
  rig.shift_tap(Button::Down);
  rig.shift_tap(Button::Down);
  rig.shift_tap(Button::Down);
  CHECK(rig.song().first() == 0);
  CHECK(rig.song().last() == 3);
  rig.flush();
  rig.tap(Button::F4);  // everything selected: a song keeps a slot
  CHECK(rig.message() == "SONG NEEDS A SLOT");
  CHECK(rig.order() == Order{1, 2, 2, 3});
  Rig full(std::vector<std::uint8_t>(254, 3));
  full.tap(Button::Copy);
  full.shift_tap(Button::Copy);
  CHECK(full.order().size() == 255u);
  full.shift_tap(Button::Down);
  full.tap(Button::Copy);  // two slots
  full.flush();
  full.shift_tap(Button::Copy);
  CHECK(full.message() == "SONG FULL");
  CHECK(full.order().size() == 255u);
}

TEST_CASE("song edit: F7 puts the encoder on the tempo (1 BPM a click, Shift 0.1), the arrows put it back; the sequencer is told") {
  Rig rig;
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100);
  rig.tap(Button::F7);
  CHECK(rig.song().tempo_focus());
  rig.turn(2);
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100 + 200u);
  rig.shift_turn(3);
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100 + 230u);
  rig.turn(1);  // a whole step first cuts the tempo to whole BPM
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100 + 300u);
  CHECK(rig.order() == Order{1, 2, 3});  // the slot's pattern did not change
  core::ParamChange pc{};
  unsigned sent = 0;
  while (rig.params->pop(pc)) {
    ++sent;
    CHECK(pc.id == seq::tempo_param());
  }
  CHECK(sent == 3u);
  CHECK(pc.value == doctest::Approx(static_cast<float>(model::kDefaultTempoX100 + 300u) / 100.0f));
  rig.turn(30000);
  CHECK(rig.project().tempo_x100() == model::kMaxTempoX100);
  rig.turn(-30000);
  CHECK(rig.project().tempo_x100() == model::kMinTempoX100);
  rig.tap(Button::F8);  // each edit is a unit
  CHECK(rig.project().tempo_x100() == model::kMaxTempoX100);
  rig.tap(Button::Down);  // the arrows are the list's again and so is the encoder
  CHECK_FALSE(rig.song().tempo_focus());
  rig.turn(1);
  CHECK(rig.order() == Order{1, 3, 3});
  rig.tap(Button::F7);
  rig.tap(Button::F5);  // F5 UNIQUE and F6 TOOLS (OS-SONGMAP; they were the slot's pattern - / +) still take the encoder back to the slot
  CHECK_FALSE(rig.song().tempo_focus());
  CHECK(rig.order() == Order{1, 4, 3});  // (UNIQUE: slot 1 shared pattern 3 with slot 2, so it was given a copy at the first free number)
}

TEST_CASE("song edit: tap tempo: two taps less than 1.5 s apart set 60000 / the interval, out of range or late taps do not") {
  Rig rig;
  rig.tap(Button::F7);  // the first tap only starts the run
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100);
  rig.pass(498);        // (the key taps themselves take 2 ms)
  rig.tap(Button::F7);
  CHECK(rig.project().tempo_x100() == 12000u);
  rig.pass(248);
  rig.tap(Button::F7);  // 250 ms: 240 BPM
  CHECK(rig.project().tempo_x100() == 24000u);
  rig.pass(3);
  rig.tap(Button::F7);  // 5 ms: 12000 BPM, past the maximum: ignored
  CHECK(rig.project().tempo_x100() == 24000u);
  rig.pass(2000);
  rig.tap(Button::F7);  // after the pause the run starts again: no tempo yet
  CHECK(rig.project().tempo_x100() == 24000u);
  rig.pass(298);
  rig.tap(Button::F7);  // 300 ms: 200 BPM
  CHECK(rig.project().tempo_x100() == 20000u);
  rig.pass(1990);
  rig.tap(Button::F7);  // 1.992 s: past the timeout, a new run (and slower than 40 BPM anyway)
  CHECK(rig.project().tempo_x100() == 20000u);
  rig.tap(Button::F8);
  CHECK(rig.project().tempo_x100() == 24000u);
}

TEST_CASE("song edit: F1 PLAY PAT plays the cursor slot's pattern, again it stops; with no transport nothing happens") {
  Rig rig(std::vector<std::uint8_t>{1, 3, 2});
  rig.tap(Button::F1);
  CHECK_FALSE(rig.app->state().playing);
  TransportLog t;
  rig.app->attach_transport(&t);
  rig.tap(Button::Down);
  rig.tap(Button::F1);
  CHECK(t.plays == 1u);
  CHECK(t.pattern == 3u);
  CHECK(rig.app->state().playing);
  rig.tap(Button::F1);
  CHECK(t.stops == 1u);
  CHECK_FALSE(rig.app->state().playing);
  rig.tap(Button::F2);  // PLAY SNG is still the Play button's action
  CHECK(t.songs == 1u);
}

TEST_CASE("song edit: with no document the edit keys say NO PROJECT and change nothing; the keys are the Song screen's only") {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  app::App a(input, clock, display);
  a.run_once();
  CHECK(a.invoke(core::action_id("song.add_slot"), 0) == DispatchResult::Disabled);  // the Pattern screen is in view
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Song), true));
  a.run_once();
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Song), false));
  a.run_once();
  CHECK(a.state().screen == app::ScreenId::Song);
  CHECK(a.invoke(core::action_id("song.add_slot"), 0) == DispatchResult::Disabled);
  CHECK(std::string(a.message()) == "NO PROJECT");
  CHECK(a.invoke(core::action_id("song.copy"), 0) == DispatchResult::Changed);  // the demo list can be read and copied
  CHECK(a.state().song.clip_count == 1);
  CHECK(a.invoke(core::action_id("song.cut"), 0) == DispatchResult::Disabled);  // OS-SONGSHARE S2: a cut writes the list, so it needs the document
  CHECK(a.state().song.clip_count == 1);
}

TEST_CASE("song live jump: Rec while playing queues the cursor's slot, Rec on the queued slot cancels, stopped it says so; the slot blinks while it waits") {
  Rig rig;
  TransportLog t;
  audio::PlayPosition position;
  rig.app->attach_transport(&t);
  rig.app->attach_play_position(&position);
  rig.tap(Button::Rec);
  CHECK(rig.message() == "NOT PLAYING");
  CHECK(t.jumps == 0u);
  rig.flush();
  rig.tap(Button::F2);  // PLAY SNG
  position.publish(true, 1, 0, 13000, 0);
  rig.pass(1);
  CHECK(rig.app->state().play.queued == audio::PlayPosition::kNoOrder);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::Rec);
  CHECK(t.jumps == 1u);
  CHECK(t.jump_row == 2u);
  CHECK(rig.message() == "JUMP QUEUED");
  rig.flush();
  // the sequencer publishes what waits: the screen reads it, blinks the slot at ~2 Hz and redraws at each half period
  position.publish_queued(2);
  rig.pass(1);
  CHECK(rig.app->state().play.queued == 2u);
  const bool phase = rig.app->state().blink;
  const std::uint32_t frames = rig.app->frames();
  rig.pass(260);
  CHECK(rig.app->state().blink != phase);
  CHECK(rig.app->frames() > frames);
  rig.pass(260);
  CHECK(rig.app->state().blink == phase);
  rig.tap(Button::Rec);  // the same slot: cancel
  CHECK(t.jumps == 2u);
  CHECK(t.jump_row == 0xFFFFu);
  rig.flush();
  position.publish_queued(audio::PlayPosition::kNoOrder);
  rig.pass(1);
  CHECK_FALSE(rig.app->state().blink);  // nothing waits: no blink
  // a refusal (no sequencer, a full ring) changes nothing and says nothing
  t.accept_jumps = false;
  rig.tap(Button::Up);
  rig.flush();
  rig.tap(Button::Rec);
  CHECK(t.jumps == 2u);
  CHECK(rig.message() == "");
}

// ---- the track map ---------------------------------------------------------------------------------------------------------

TEST_CASE("track map: a note makes a track filled, other data dim, nothing empty; only the steps that play count") {
  model::Project& p = test::arena(1).project;
  p.reset();
  NEED(p.create_pattern(1, 16) == model::Status::Ok);
  model::Step note = model::kEmptyStep;
  note.note = 61;
  model::Step fx = model::kEmptyStep;
  fx.fx[0] = {12, 5};
  model::Step vol = model::kEmptyStep;
  vol.vol = 100;
  CHECK(p.set_step(1, 0, 3, note) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 0, fx) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 9, vol) == model::Status::Ok);
  CHECK(p.set_step(1, 2, 5, fx) == model::Status::Ok);
  CHECK(p.set_step(1, 2, 7, note) == model::Status::Ok);  // a note after an FX step: the track is a Note
  CHECK(p.set_step(1, 3, 12, note) == model::Status::Ok);
  CHECK(p.set_track_length(1, 3, 8) == model::Status::Ok);  // ... past this track's length: it does not play
  note.note = model::kNoteCut;
  CHECK(p.set_step(1, 4, 0, note) == model::Status::Ok);  // a cut is a note-column event
  const std::uint32_t map = app::SongTrackMap::compute(*p.pattern(1));
  using Cell = app::SongTrackMap::Cell;
  CHECK(app::SongTrackMap::cell(map, 0) == Cell::Note);
  CHECK(app::SongTrackMap::cell(map, 1) == Cell::Fx);
  CHECK(app::SongTrackMap::cell(map, 2) == Cell::Note);
  CHECK(app::SongTrackMap::cell(map, 3) == Cell::Empty);
  CHECK(app::SongTrackMap::cell(map, 4) == Cell::Note);
  CHECK(app::SongTrackMap::cell(map, 5) == Cell::Empty);
  CHECK(app::SongTrackMap::cell(map, 99) == Cell::Empty);
}

TEST_CASE("track map: the cache is keyed by the pattern's generation: drawn frames do not scan, an edit of that pattern alone does") {
  Rig rig;  // song 1 2 3, on the Song screen
  const app::SongTrackMap& cache = rig.app->state().song.map;
  CHECK(cache.computed() == 3u);  // the first frame: the three rows
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::Up);
  CHECK(cache.computed() == 3u);  // redrawn three times: nothing scanned again
  model::Step note = model::kEmptyStep;
  note.note = 64;
  CHECK(rig.project().set_step(2, 7, 1, note) == model::Status::Ok);
  rig.tap(Button::Down);
  CHECK(cache.computed() == 4u);  // pattern 2 changed: that one pattern is scanned again
  CHECK(app::SongTrackMap::cell(cache.get(rig.project(), 2), 7) == app::SongTrackMap::Note);
  CHECK(app::SongTrackMap::cell(cache.get(rig.project(), 1), 7) == app::SongTrackMap::Empty);
  CHECK(app::SongTrackMap::cell(cache.get(rig.project(), 9), 7) == app::SongTrackMap::Empty);  // a pattern that does not exist: empty
  CHECK(cache.computed() == 4u);
  rig.tap(Button::Up);  // slot 1, pattern 2
  rig.shift_tap(Button::Insert);  // a duplicate: a new pattern (4), one scan more when it is drawn
  CHECK(app::SongTrackMap::cell(cache.get(rig.project(), 4), 7) == app::SongTrackMap::Note);
  CHECK(cache.computed() == 5u);
  // another project is never answered from this one's cache
  model::Project& other = test::arena(2).project;
  other.reset();
  NEED(other.create_pattern(2, 16) == model::Status::Ok);
  CHECK(app::SongTrackMap::cell(cache.get(other, 2), 7) == app::SongTrackMap::Empty);
}

// OS-DELREPEAT: Delete repeats in Pattern only as far as the repeatable actions go; held on a Song slot it removes one slot (the press), not a slot per repeat. The arrows still repeat here.
TEST_CASE("song edit: a held Delete removes one slot, a held Down still walks the list") {
  Rig walk(std::vector<std::uint8_t>{1, 2, 3, 1, 2, 3, 1, 2, 3, 1});
  walk.press(Button::Down);
  walk.pass(1);
  for (unsigned t = 0; t < 1500; ++t) {
    walk.pass(1);
  }
  walk.release(Button::Down);
  walk.pass(1);
  CHECK(walk.song().pos() >= 8);  // the press and the repeats of the schedule

  Rig rig(std::vector<std::uint8_t>{1, 2, 3, 1, 2, 3, 1, 2, 3, 1});
  rig.press(Button::Delete);
  rig.pass(1);
  for (unsigned t = 0; t < 1500; ++t) {
    rig.pass(1);
  }
  rig.release(Button::Delete);
  rig.pass(1);
  CHECK(rig.order().size() == 9u);  // exactly one slot
}

// ---- OS-SONGSHARE S1: how many slots hold a pattern (docs/specs/OS-SONGSHARE.md) ---------------------------------------------------------------------

TEST_CASE("song uses: the table counts the slots that hold each pattern, whether or not it exists yet") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 1, 3, 1, 9, 9});  // pattern 9 is named by two slots and does not exist
  const model::Project& p = rig.project();
  app::SongUseCount u;
  CHECK(u.get(p, 1) == 3u);
  CHECK(u.get(p, 2) == 1u);
  CHECK(u.get(p, 3) == 1u);
  CHECK(u.get(p, 9) == 2u);
  CHECK(u.get(p, 4) == 0u);    // no slot names it
  CHECK(u.get(p, 0) == 0u);    // ids are 1..255
  CHECK(u.get(p, 256) == 0u);
  CHECK(u.get(p, 70000) == 0u);
  CHECK(u.built() == 1u);      // one pass served all of it
}

TEST_CASE("song uses: the table is rebuilt when the order list changes and not when a pattern's steps do; another project is never answered from it") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 1});
  model::Project& p = rig.project();
  app::SongUseCount u;
  CHECK(u.get(p, 1) == 2u);
  CHECK(u.built() == 1u);
  model::Step note = model::kEmptyStep;
  note.note = 61;
  CHECK(p.set_step(1, 0, 0, note) == model::Status::Ok);  // a step is not the order list
  CHECK(p.create_pattern(5, 16) == model::Status::Ok);
  CHECK(u.get(p, 1) == 2u);
  CHECK(u.built() == 1u);
  CHECK(p.append_order(2) == model::Status::Ok);
  CHECK(u.get(p, 2) == 2u);
  CHECK(u.built() == 2u);
  CHECK(p.set_order(0, 2) == model::Status::Ok);
  CHECK(u.get(p, 1) == 1u);
  CHECK(u.get(p, 2) == 3u);
  CHECK(u.built() == 3u);
  CHECK(p.remove_order(0) == model::Status::Ok);
  CHECK(u.get(p, 2) == 2u);
  CHECK(u.built() == 4u);
  model::Project& other = test::arena(2).project;
  other.reset();
  CHECK(other.append_order(1) == model::Status::Ok);
  CHECK(u.get(other, 1) == 1u);  // not this project's 1
  CHECK(u.get(other, 2) == 0u);
  CHECK(u.built() == 5u);
}

TEST_CASE("song uses: the longest list counts in a byte, and the screen's cache is the view's and follows the editor") {
  Rig rig(std::vector<std::uint8_t>(250, 3));  // 250 slots of pattern 3 (the list holds 255)
  const app::SongUseCount& cache = rig.app->state().song.use;
  CHECK(cache.get(rig.project(), 3) == 250u);  // (the table is a cache the drawing fills: it counts the same whoever asks)
  for (unsigned i = 0; i < 5; ++i) {
    CHECK(rig.project().append_order(3) == model::Status::Ok);
  }
  CHECK(rig.project().song_length() == 255u);
  CHECK(cache.get(rig.project(), 3) == 255u);
  CHECK(cache.get(rig.project(), 2) == 0u);
  const std::uint32_t built = cache.built();
  rig.tap(Button::Down);  // a redraw: nothing is rebuilt
  CHECK(cache.built() == built);
  rig.tap(Button::Insert);  // the list is full: nothing changes
  CHECK(cache.built() == built);
  rig.tap(Button::Delete);  // F4's Delete removes one slot of pattern 3
  CHECK(rig.project().song_length() == 254u);
  CHECK(cache.get(rig.project(), 3) == 254u);
  CHECK(cache.built() > built);
}

// ---- OS-SONGSHARE S2: CUT on Shift + Delete ------------------------------------------------------------------------------------------------------

TEST_CASE("song cut: on PAT the slot goes into the slot clipboard and out of the list as one undo unit, the cursor on the slot above; Shift + Copy puts it back anywhere") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3});
  rig.shift_tap(Button::Delete);  // the cursor's slot, slot 0
  CHECK(rig.order() == Order{2, 3});
  CHECK(rig.song().clip_count == 1);
  CHECK(rig.song().clip[0] == 1);
  CHECK(rig.song().pos() == 0);
  CHECK(rig.message() == "CUT 1 SLOT");
  rig.tap(Button::Down);
  rig.shift_tap(Button::Copy);  // before the cursor's slot: the cut slot moved
  CHECK(rig.order() == Order{2, 1, 3});
  rig.tap(Button::F8);  // the paste
  CHECK(rig.order() == Order{2, 3});
  rig.tap(Button::F8);  // the cut, in one unit
  CHECK(rig.order() == Order{1, 2, 3});
  CHECK(rig.song().clip_count == 1);  // the clipboard is not undone (as Copy's is not)
  CHECK(rig.song().clip[0] == 1);
  rig.shift_tap(Button::F8);  // redo of the cut
  CHECK(rig.order() == Order{2, 3});
  CHECK(rig.project().check_invariants());
}

TEST_CASE("song cut: a range goes in one unit, the clipboard holds it in order, the range collapses, a held key cuts once") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3, 1, 2});
  rig.tap(Button::Down);
  rig.shift_tap(Button::Down);
  rig.shift_tap(Button::Down);
  CHECK(rig.song().first() == 1);
  CHECK(rig.song().last() == 3);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.press(Button::Delete);
  rig.pass(1);
  for (unsigned t = 0; t < 1500; ++t) {
    rig.pass(1);  // the repeats of a held Delete: a cut does not repeat
  }
  rig.release(Button::Delete);
  rig.pass(1);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.order() == Order{1, 2});
  CHECK(rig.song().clip_count == 3);
  CHECK(rig.song().clip[0] == 2);
  CHECK(rig.song().clip[1] == 3);
  CHECK(rig.song().clip[2] == 1);
  CHECK(rig.song().pos() == 0);
  CHECK_FALSE(rig.song().selecting());
  CHECK(rig.message() == "CUT 3 SLOTS");
  rig.tap(Button::F8);  // one unit
  CHECK(rig.order() == Order{1, 2, 3, 1, 2});
}

TEST_CASE("song cut: cutting every slot, or an empty song, is refused and the clipboard keeps what it held; a refusal opens no undo unit") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3});
  rig.tap(Button::Copy);  // the clipboard holds pattern 1
  CHECK(rig.song().clip_count == 1);
  rig.tap(Button::F3);    // an edit, so that a wrong empty unit would show in the undo below
  CHECK(rig.order() == Order{1, 1, 2, 3});
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.shift_tap(Button::Down);
  rig.shift_tap(Button::Down);
  rig.shift_tap(Button::Down);
  CHECK(rig.song().first() == 0);
  CHECK(rig.song().last() == 3);
  rig.flush();  // (the strip shows the notices in turn: the refusal's must not queue behind the Copy's)
  rig.shift_tap(Button::Delete);
  CHECK(rig.message() == "SONG NEEDS A SLOT");
  CHECK(rig.order() == Order{1, 1, 2, 3});
  CHECK(rig.song().clip_count == 1);
  CHECK(rig.song().clip[0] == 1);
  CHECK(rig.song().selecting());  // nothing happened: the range is still there
  rig.tap(Button::F8);            // takes the F3 back, not an empty unit
  CHECK(rig.order() == Order{1, 2, 3});

  Rig one(std::vector<std::uint8_t>{2});
  one.shift_tap(Button::Delete);
  CHECK(one.message() == "SONG NEEDS A SLOT");
  CHECK(one.order() == Order{2});
  CHECK(one.song().clip_count == 0);

  Rig empty{std::vector<std::uint8_t>{}};
  empty.shift_tap(Button::Delete);
  CHECK(empty.message() == "SONG IS EMPTY");
  CHECK(empty.order().empty());
}

TEST_CASE("song cut: the plain Delete is still DEL SLOT and leaves the clipboard alone") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3});
  rig.tap(Button::Copy);  // the clipboard holds pattern 1
  rig.tap(Button::Down);
  rig.tap(Button::Delete);
  CHECK(rig.order() == Order{1, 3});
  CHECK(rig.song().clip_count == 1);
  CHECK(rig.song().clip[0] == 1);  // not the slot Delete removed
  CHECK(rig.song().pos() == 0);
  rig.tap(Button::Down);
  rig.shift_tap(Button::Delete);  // the same key with Shift cuts: the clipboard takes pattern 3
  CHECK(rig.order() == Order{1});
  CHECK(rig.song().clip_count == 1);
  CHECK(rig.song().clip[0] == 3);
}

// ---- OS-SONGSHARE S3: MOVE, Copy held + Up / Down ---------------------------------------------------------------------------------------------------

namespace {

// Copy held while `b` is tapped once: the gesture of the move.
void copy_tap(Rig& rig, Button b) {
  rig.press(Button::Copy);
  rig.pass(1);
  rig.tap(b);
  rig.release(Button::Copy);
  rig.pass(1);
}

}  // namespace

TEST_CASE("song move: Copy held + Down moves the cursor's slot one position and the cursor follows, Up moves it back; each step is one undo unit and Copy itself does not copy") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3, 1, 2});
  rig.tap(Button::Down);  // slot 1, pattern 2
  const unsigned depth = rig.journal.depth();
  const std::size_t bytes = rig.journal.used_bytes();
  copy_tap(rig, Button::Down);
  CHECK(rig.order() == Order{1, 3, 2, 1, 2});
  CHECK(rig.journal.used_bytes() - bytes == records_bytes(2));  // an insert and a remove (their order is song_map_edit_test's)
  CHECK(rig.song().pos() == 2);
  CHECK(rig.journal.depth() == depth + 1u);
  CHECK(rig.song().clip_count == 0);  // Copy's own tap was consumed by the arrow: no COPIED
  CHECK(rig.message() != "COPIED 1 SLOT");
  copy_tap(rig, Button::Down);
  CHECK(rig.order() == Order{1, 3, 1, 2, 2});
  CHECK(rig.song().pos() == 3);
  CHECK(rig.journal.depth() == depth + 2u);
  copy_tap(rig, Button::Up);
  copy_tap(rig, Button::Up);
  CHECK(rig.order() == Order{1, 2, 3, 1, 2});
  CHECK(rig.song().pos() == 1);
  rig.tap(Button::F8);  // the last step only
  CHECK(rig.order() == Order{1, 3, 2, 1, 2});
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 3, 1, 2, 2});
  rig.shift_tap(Button::F8);  // redo
  CHECK(rig.order() == Order{1, 3, 2, 1, 2});
  CHECK(rig.project().check_invariants());
}

TEST_CASE("song move: a selected range moves as a block, the cursor and the far end of the range follow, and the list's multiset stays") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3, 1, 2});
  rig.tap(Button::Down);
  rig.shift_tap(Button::Down);  // slots 1..2 (patterns 2, 3), the cursor on 2
  CHECK(rig.song().first() == 1);
  CHECK(rig.song().last() == 2);
  const std::size_t bytes = rig.journal.used_bytes();
  copy_tap(rig, Button::Down);  // slot 3 (pattern 1) steps over the block
  CHECK(rig.order() == Order{1, 1, 2, 3, 2});
  CHECK(rig.journal.used_bytes() - bytes == records_bytes(2));  // a block moves by the one slot that steps over it: two writes, however long the block
  CHECK(rig.song().selecting());
  CHECK(rig.song().first() == 2);
  CHECK(rig.song().last() == 3);
  CHECK(rig.song().pos() == 3);
  copy_tap(rig, Button::Down);  // slot 4 (pattern 2) steps over it: the block is at the end
  CHECK(rig.order() == Order{1, 1, 2, 2, 3});
  CHECK(rig.song().first() == 3);
  CHECK(rig.song().last() == 4);
  copy_tap(rig, Button::Down);  // the bottom: nothing
  CHECK(rig.order() == Order{1, 1, 2, 2, 3});
  copy_tap(rig, Button::Up);
  copy_tap(rig, Button::Up);
  copy_tap(rig, Button::Up);
  copy_tap(rig, Button::Up);
  CHECK(rig.order() == Order{2, 3, 1, 1, 2});  // the block is at the top
  CHECK(rig.song().first() == 0);
  CHECK(rig.song().last() == 1);
  CHECK(rig.song().selecting());
  CHECK(rig.project().check_invariants());
}

TEST_CASE("song move: at the top or the bottom nothing moves and no undo unit is opened; an empty song says so") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3});
  const unsigned depth = rig.journal.depth();
  const std::uint32_t generation = rig.project().song_generation();
  copy_tap(rig, Button::Up);  // slot 0 is at the top
  CHECK(rig.order() == Order{1, 2, 3});
  CHECK(rig.song().pos() == 0);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  copy_tap(rig, Button::Down);  // slot 2 is at the bottom
  CHECK(rig.order() == Order{1, 2, 3});
  CHECK(rig.song().pos() == 2);
  CHECK(rig.journal.depth() == depth);
  CHECK(rig.project().song_generation() == generation);
  Rig one(std::vector<std::uint8_t>{2});
  copy_tap(one, Button::Down);
  copy_tap(one, Button::Up);
  CHECK(one.order() == Order{2});
  Rig empty{std::vector<std::uint8_t>{}};
  copy_tap(empty, Button::Down);
  CHECK(empty.message() == "SONG IS EMPTY");
}

TEST_CASE("song move: a full list (255 slots) cannot take the insert: the slots rotate with set_order writes, the length never changes, a slot or a block, down and up, one undo unit; one slot short of full it inserts") {
  const unsigned full = model::kMaxSongPositions;
  std::vector<std::uint8_t> order(full);
  for (unsigned i = 0; i < full; ++i) {
    order[i] = static_cast<std::uint8_t>(1 + i % 3);
  }
  order[100] = 9;  // marks: the slots that move are told apart (and every write of a rotation changes a slot, so each is a record)
  order[101] = 8;
  order[102] = 7;
  Rig rig(order);
  CHECK(rig.project().song_length() == full);
  const std::vector<unsigned> start = rig.order();
  rig.tap(Button::Down, 100);  // the cursor on slot 100 (pattern 9)
  CHECK(rig.song().pos() == 100);
  const unsigned depth = rig.journal.depth();
  std::size_t bytes = rig.journal.used_bytes();
  copy_tap(rig, Button::Down);  // one slot down: 9 and 8 trade places
  std::vector<unsigned> want = start;
  std::swap(want[100], want[101]);
  CHECK(rig.order() == want);
  CHECK(rig.song().pos() == 101);
  CHECK(rig.project().song_length() == full);
  CHECK(rig.journal.depth() == depth + 1u);
  CHECK(rig.journal.used_bytes() - bytes == records_bytes(2));  // slot 101 takes slot 100's pattern, slot 100 the one that stepped over
  copy_tap(rig, Button::Up);
  CHECK(rig.order() == start);
  CHECK(rig.song().pos() == 100);
  // a block of three: the slot below steps over it (down), or the slot above does (up): four writes, the three shifts and the one that lands (an insert and a remove would be two)
  rig.shift_tap(Button::Down, 2);  // slots 100..102 (9, 8, 7)
  bytes = rig.journal.used_bytes();
  copy_tap(rig, Button::Down);
  want = start;
  std::rotate(want.begin() + 100, want.begin() + 103, want.begin() + 104);  // slot 103 goes to the front of the block
  CHECK(rig.order() == want);
  CHECK(rig.song().first() == 101);
  CHECK(rig.song().last() == 103);
  CHECK(rig.journal.used_bytes() - bytes == records_bytes(4));
  copy_tap(rig, Button::Up);
  CHECK(rig.order() == start);
  bytes = rig.journal.used_bytes();
  copy_tap(rig, Button::Up);
  want = start;
  std::rotate(want.begin() + 99, want.begin() + 100, want.begin() + 103);  // slot 99 goes behind the block
  CHECK(rig.order() == want);
  CHECK(rig.journal.used_bytes() - bytes == records_bytes(4));
  CHECK(rig.project().song_length() == full);
  CHECK(rig.project().check_invariants());
  // every move is one undo unit (five: a slot down and up, a block down, up and up) and takes its writes back exactly
  for (int i = 0; i < 5; ++i) {
    rig.tap(Button::F8);
  }
  CHECK(rig.order() == start);
  CHECK(rig.journal.depth() == depth);
  // one slot short of full the insert fits, and a block of three moves with two writes
  rig.shift_tap(Button::F1);  // the cursor to slot 0
  rig.tap(Button::F4);        // DEL SLOT: 254 slots
  CHECK(rig.project().song_length() == full - 1u);
  const std::vector<unsigned> short_start = rig.order();
  rig.shift_tap(Button::Down, 2);
  bytes = rig.journal.used_bytes();
  copy_tap(rig, Button::Down);
  want = short_start;
  std::rotate(want.begin(), want.begin() + 3, want.begin() + 4);
  CHECK(rig.order() == want);
  CHECK(rig.journal.used_bytes() - bytes == records_bytes(2));
  CHECK(rig.project().song_length() == full - 1u);
  CHECK(rig.project().check_invariants());
}

TEST_CASE("song move: Copy held, the arrow held: the move repeats, one undo unit a step, and the slot ends at the bottom") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3, 1, 2, 3, 1, 2, 3, 1});
  const Order before = rig.order();
  const unsigned depth = rig.journal.depth();
  rig.press(Button::Copy);
  rig.pass(1);
  rig.press(Button::Down);
  rig.pass(1);
  for (unsigned t = 0; t < 4000; ++t) {
    rig.pass(1);  // the press and the repeats of the schedule
  }
  rig.release(Button::Down);
  rig.pass(1);
  rig.release(Button::Copy);
  rig.pass(1);
  const int steps = rig.song().pos();  // slot 0 walked down: one position a step
  CHECK(steps >= 8);
  CHECK(rig.journal.depth() == depth + static_cast<unsigned>(steps));
  CHECK(rig.song().clip_count == 0);
  for (int i = 0; i < steps; ++i) {
    rig.tap(Button::F8);
  }
  CHECK(rig.order() == before);
  CHECK(rig.journal.depth() == depth);
}

TEST_CASE("song move: Copy and Shift + Copy still copy and paste, on the release; Copy then another key is consumed, Shift + arrows still select") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3});
  rig.press(Button::Copy);
  rig.pass(1);
  CHECK(rig.song().clip_count == 0);  // nothing at the press: Copy waits for the release
  rig.release(Button::Copy);
  rig.pass(1);
  CHECK(rig.song().clip_count == 1);
  CHECK(rig.message() == "COPIED 1 SLOT");
  rig.tap(Button::Down);
  rig.press(Button::Shift);  // Shift, then Copy: the paste, on the release
  rig.pass(1);
  rig.press(Button::Copy);
  rig.pass(1);
  CHECK(rig.order() == Order{1, 2, 3});
  rig.release(Button::Copy);
  rig.pass(1);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.order() == Order{1, 1, 2, 3});
  Rig other(std::vector<std::uint8_t>{1, 2, 3});
  other.press(Button::Copy);  // Copy, then Up: consumed (the slot moves, the clipboard stays empty)
  other.pass(1);
  other.tap(Button::Down);
  other.release(Button::Copy);
  other.pass(1);
  CHECK(other.song().clip_count == 0);
  CHECK(other.order() == Order{2, 1, 3});
  other.shift_tap(Button::Down);  // Shift + Down is the range, as before
  CHECK(other.song().selecting());
  CHECK(other.order() == Order{2, 1, 3});
}

TEST_CASE("song move: it works while the song plays, the play position and the queued jump stay positions of the list, the sequencer is not told") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3, 1});
  TransportLog t;
  audio::PlayPosition position;
  rig.app->attach_transport(&t);
  rig.app->attach_play_position(&position);
  rig.tap(Button::F2);  // PLAY SNG
  CHECK(rig.app->state().playing);
  position.publish(true, 2, 0, 12000, 1);  // the song plays slot 1 (pattern 2) ...
  position.publish_queued(3);              // ... and a live jump waits for slot 3
  rig.pass(1);
  CHECK(rig.app->state().play.order == 1u);
  CHECK(rig.app->state().play.queued == 3u);
  const unsigned jumps = t.jumps, songs = t.songs;
  copy_tap(rig, Button::Down);  // slot 0 steps over slot 1
  CHECK(rig.order() == Order{2, 1, 3, 1});
  CHECK(rig.song().pos() == 1);
  CHECK(t.jumps == jumps);  // nothing is sent to the sequencer: it goes on with the positions it has
  CHECK(t.songs == songs);
  CHECK(t.plays == 0u);
  CHECK(t.stops == 0u);
  rig.pass(1);
  CHECK(rig.app->state().playing);
  CHECK(rig.app->state().play.order == 1u);  // the playing slot is still the position that the sequencer published ...
  CHECK(rig.app->state().play.queued == 3u);  // ... and so is the queued jump: both are slots of the new list
  CHECK(app::SongSource(rig.app->model()).marker(rig.app->state().play) == 1);  // the marker of the screen sits on the position
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 3, 1});
  CHECK(rig.app->state().play.order == 1u);
  CHECK(rig.app->state().play.queued == 3u);
}

// ---- OS-SONGSHARE S4: NEXT USE, Note / Shift + Note --------------------------------------------------------------------------------------------------

TEST_CASE("song next use: Note goes to the next slot that holds the cursor's pattern, Shift + Note to the previous one, both wrap, the strip says where") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 1, 3, 1});
  const unsigned depth = rig.journal.depth();
  rig.tap(Button::Note);
  CHECK(rig.song().pos() == 2);
  CHECK(rig.message() == "SLOT 02 (2 OF 3)");
  rig.tap(Button::Note);
  CHECK(rig.song().pos() == 4);
  CHECK(rig.message() == "SLOT 04 (3 OF 3)");
  rig.tap(Button::Note);  // wraps to the first
  CHECK(rig.song().pos() == 0);
  CHECK(rig.message() == "SLOT 00 (1 OF 3)");
  rig.shift_tap(Button::Note);  // wraps back to the last
  CHECK(rig.song().pos() == 4);
  CHECK(rig.message() == "SLOT 04 (3 OF 3)");
  rig.shift_tap(Button::Note);
  CHECK(rig.song().pos() == 2);
  CHECK(rig.message() == "SLOT 02 (2 OF 3)");
  rig.shift_tap(Button::Note);
  CHECK(rig.song().pos() == 0);
  CHECK(rig.order() == Order{1, 2, 1, 3, 1});  // the list is untouched
  CHECK(rig.journal.depth() == depth);         // and no undo unit is opened
}

TEST_CASE("song next use: a pattern held by one slot only says so and the cursor stays; two slots go to each other both ways") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3, 2});
  rig.tap(Button::Down);  // slot 1, pattern 2, held by slot 3 too
  rig.tap(Button::Note);
  CHECK(rig.song().pos() == 3);
  CHECK(rig.message() == "SLOT 03 (2 OF 2)");
  rig.shift_tap(Button::Note);
  CHECK(rig.song().pos() == 1);
  CHECK(rig.message() == "SLOT 01 (1 OF 2)");
  rig.shift_tap(Button::Note);  // previous of the first is the last
  CHECK(rig.song().pos() == 3);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  CHECK(rig.song().pos() == 0);
  rig.flush();
  rig.tap(Button::Note);  // pattern 1 is held by slot 0 alone
  CHECK(rig.message() == "ONLY IN THIS SLOT");
  CHECK(rig.song().pos() == 0);
  rig.flush();
  rig.shift_tap(Button::Note);
  CHECK(rig.message() == "ONLY IN THIS SLOT");
  CHECK(rig.song().pos() == 0);
}

TEST_CASE("song next use: the range collapses, the window follows to a slot far down, a slot that names a pattern that does not exist yet counts, an empty song says so") {
  Rig rig(std::vector<std::uint8_t>{1, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 1});
  rig.tap(Button::Down);
  rig.shift_tap(Button::Down);
  rig.shift_tap(Button::Down);
  CHECK(rig.song().selecting());
  rig.tap(Button::Note);  // the cursor is on slot 3 (pattern 2): the next slot with pattern 2
  CHECK_FALSE(rig.song().selecting());
  CHECK(rig.song().pos() == 5);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  CHECK(rig.song().pos() == 0);
  rig.tap(Button::Note);  // pattern 1: slot 0 and the last slot
  CHECK(rig.song().pos() == 20);
  CHECK(rig.song().top() + app::SongView::kVisibleRows > 20);  // the window shows it
  CHECK(rig.message() == "SLOT 20 (2 OF 2)");

  Rig ghost(std::vector<std::uint8_t>{9, 1, 9});  // pattern 9 does not exist: two slots still hold it
  ghost.tap(Button::Note);
  CHECK(ghost.song().pos() == 2);
  CHECK(ghost.message() == "SLOT 02 (2 OF 2)");

  Rig empty{std::vector<std::uint8_t>{}};
  empty.tap(Button::Note);
  CHECK(empty.message() == "SONG IS EMPTY");
  empty.flush();
  empty.shift_tap(Button::Note);
  CHECK(empty.message() == "SONG IS EMPTY");
}

TEST_CASE("song next use: it needs no document (the demo list answers) and a question that is up is taken back by the key") {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  app::App a(input, clock, display);
  a.run_once();
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Song), true));
  a.run_once();
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Song), false));
  a.run_once();
  CHECK(a.state().screen == app::ScreenId::Song);
  CHECK(a.invoke(core::action_id("song.next_use"), 1) == DispatchResult::Changed);  // the demo's slots 0 and 1 both hold pattern 1
  CHECK(a.state().song.pos() == 1);
  CHECK(std::string(a.message()) == "SLOT 01 (2 OF 2)");
  CHECK(a.invoke(core::action_id("song.next_use"), -1) == DispatchResult::Changed);
  CHECK(a.state().song.pos() == 0);
  CHECK(a.invoke(core::action_id("song.next_use"), 0) != DispatchResult::Changed);  // not a direction

  // a question that is up (Delete over cells of a pattern that another slot holds) is taken back by Note and by Shift + Note: the key acts as usual
  Rig rig(std::vector<std::uint8_t>{1, 2, 1});
  model::Step note = model::kEmptyStep;
  note.note = 60;
  note.instr = 1;
  NEED(rig.project().set_step(1, 0, 0, note) == model::Status::Ok);
  rig.tap(Button::Right);  // into the map, track 1
  for (const bool shift : {false, true}) {
    rig.flush();
    rig.tap(Button::Delete);  // asks: pattern 1 is also in slot 2
    CHECK(rig.app->state().song.ask.live);
    CHECK(rig.message() == "PATTERN 01 ALSO IN 1 SLOT: F1 ALL, F2 UNIQUE");
    const unsigned depth = rig.journal.depth();
    if (shift) {
      rig.shift_tap(Button::Note);
    } else {
      rig.tap(Button::Note);
    }
    CHECK_FALSE(rig.app->state().song.ask.live);  // the question is gone
    CHECK(rig.journal.depth() == depth);          // and nothing was written
    CHECK(rig.project().pattern(1)->steps[0][0].note == 60);
    CHECK(rig.song().pos() == 2);  // the key acted: slot 0 and slot 2 hold pattern 1 and the cursor went to the other one (both ways round a list of two)
    CHECK(rig.message() == "SLOT 02 (2 OF 2)");
    rig.tap(Button::Note);  // back to slot 0 for the next round
    CHECK(rig.song().pos() == 0);
  }
}

// ---- OS-PATTEMPO (NEW Community OS behaviour): Pattern held + the encoder is the project tempo, on any screen ---------------------------------------------

namespace {

// The strip's text for a tempo of `x100` (BPM x 100): `TEMPO 130.0 BPM`.
std::string tempo_text(std::uint32_t x100) { return "TEMPO " + std::to_string(x100 / 100u) + "." + std::to_string((x100 % 100u) / 10u) + " BPM"; }

// The tempo changes the sequencer was told of since the last call (and that nothing else was sent).
std::vector<float> tempo_sent(Rig& rig) {
  std::vector<float> v;
  core::ParamChange pc{};
  while (rig.params->pop(pc)) {
    CHECK(pc.id == seq::tempo_param());
    v.push_back(pc.value);
  }
  return v;
}

}  // namespace

TEST_CASE("pattern tempo: Pattern held + the encoder moves the tempo 1 BPM a detent, from any screen, tells the sequencer, says it, and the release does not navigate") {
  for (const Button screen : {Button::Song, Button::Pattern, Button::Params, Button::Master}) {
    Rig rig;
    rig.tap(screen);
    (void)tempo_sent(rig);
    const app::ScreenId where = rig.app->state().screen;
    const std::uint32_t t0 = rig.project().tempo_x100();
    CHECK(t0 == model::kDefaultTempoX100);
    rig.press(Button::Pattern);
    rig.pass(1);
    CHECK(rig.app->state().screen == where);  // the press waits: it may be a turn
    rig.turn(2);
    CHECK(rig.project().tempo_x100() == t0 + 200u);
    CHECK(rig.message() == tempo_text(t0 + 200u));
    CHECK(rig.app->state().screen == where);  // the turn consumed the Pattern tap
    rig.turn(-3);  // and again, the key still down
    CHECK(rig.project().tempo_x100() == t0 - 100u);
    CHECK(rig.message() == tempo_text(t0 - 100u));
    const std::vector<float> sent = tempo_sent(rig);
    NEED(sent.size() == 2u);
    CHECK(sent.back() == doctest::Approx(static_cast<float>(t0 - 100u) / 100.0f));
    rig.release(Button::Pattern);
    rig.pass(1);
    CHECK(rig.app->state().screen == where);  // the release after a turn is silent
    CHECK(rig.project().tempo_x100() == t0 - 100u);
  }
}

TEST_CASE("pattern tempo: Shift held first makes a detent 0.1 BPM, a whole step first cuts the tempo to whole BPM") {
  Rig rig;
  rig.tap(Button::Pattern);
  const std::uint32_t t0 = rig.project().tempo_x100();
  rig.press(Button::Shift);
  rig.pass(1);
  rig.press(Button::Pattern);
  rig.pass(1);
  rig.turn(3);
  CHECK(rig.project().tempo_x100() == t0 + 30u);
  CHECK(rig.message() == tempo_text(t0 + 30u));
  rig.release(Button::Pattern);
  rig.pass(1);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.app->state().overlay == app::OverlayId::None);  // the turn consumed the Shift + Pattern tap: no apps list
  rig.press(Button::Pattern);
  rig.pass(1);
  rig.turn(1);
  CHECK(rig.project().tempo_x100() == t0 + 100u);
  rig.release(Button::Pattern);
  rig.pass(1);
}

TEST_CASE("pattern tempo: both ends of the range hold and the strip still says where it is; one undo unit a hold, as the Song screen's F7") {
  Rig rig;
  rig.tap(Button::Song);
  const std::uint32_t t0 = rig.project().tempo_x100();
  rig.press(Button::Pattern);
  rig.pass(1);
  rig.turn(30000);
  CHECK(rig.project().tempo_x100() == model::kMaxTempoX100);
  CHECK(rig.message() == "TEMPO 800.0 BPM");
  rig.turn(1);  // already at the top
  CHECK(rig.project().tempo_x100() == model::kMaxTempoX100);
  CHECK(rig.message() == "TEMPO 800.0 BPM");
  rig.turn(-30000);
  CHECK(rig.project().tempo_x100() == model::kMinTempoX100);
  CHECK(rig.message() == "TEMPO 40.0 BPM");
  rig.turn(-1);
  CHECK(rig.project().tempo_x100() == model::kMinTempoX100);
  rig.release(Button::Pattern);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Song);
  rig.tap(Button::F8);  // every turn of the hold is one unit on the Song history: back to the tempo from before the hold
  CHECK(rig.project().tempo_x100() == t0);
  rig.press(Button::Shift);  // the fine step clamps too
  rig.pass(1);
  rig.press(Button::Pattern);
  rig.pass(1);
  rig.turn(30000);
  CHECK(rig.project().tempo_x100() == model::kMaxTempoX100);
  rig.turn(5);
  CHECK(rig.project().tempo_x100() == model::kMaxTempoX100);
  rig.turn(-30000);
  CHECK(rig.project().tempo_x100() == model::kMinTempoX100);
  rig.release(Button::Pattern);
  rig.release(Button::Shift);
  rig.pass(1);
}

TEST_CASE("pattern tempo: a plain tap of Pattern still opens Pattern, at the release; Shift + Pattern opens the apps list and the chord Song + Pattern the Navigator (OS-APPSNAV)") {
  Rig rig;
  CHECK(rig.app->state().screen == app::ScreenId::Song);
  rig.press(Button::Pattern);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Song);  // not at the press
  rig.release(Button::Pattern);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100);
  rig.tap(Button::Song);
  rig.shift_tap(Button::Pattern);  // OS-APPSNAV: the list of the apps, no longer the Navigator
  CHECK(rig.app->state().overlay == app::OverlayId::ModulePicker);
  CHECK(rig.app->state().screen == app::ScreenId::Song);
  rig.tap(Button::Pattern);
  NEED(rig.app->state().overlay == app::OverlayId::None);
  rig.open_navigator();  // the chord Song + Pattern is the Navigator
  CHECK(rig.app->state().overlay == app::OverlayId::Navigator);
  CHECK(rig.app->state().screen == app::ScreenId::Song);
}

TEST_CASE("navigator chords: Master + Song and Song + Pattern, in either press order, open the Navigator and fire nothing else (OS-APPSNAV)") {
  const Button pairs[4][2] = {{Button::Master, Button::Song}, {Button::Song, Button::Master}, {Button::Song, Button::Pattern}, {Button::Pattern, Button::Song}};
  for (const auto& pair : pairs) {
    Rig rig;
    rig.tap(Button::Params);  // a screen that is none of the chord's
    const app::ScreenId before = rig.app->state().screen;
    NEED(before != app::ScreenId::Song);
    NEED(before != app::ScreenId::Pattern);
    NEED(before != app::ScreenId::Mixer);
    const auto depth = rig.journal.depth();
    const std::uint32_t tempo = rig.project().tempo_x100();
    rig.press(pair[0]);
    rig.pass(1);
    rig.press(pair[1]);
    rig.pass(1);
    CHECK(rig.app->state().overlay == app::OverlayId::Navigator);
    CHECK(rig.app->state().screen == before);  // neither screen button's tap
    CHECK_FALSE(rig.app->state().peeking);
    rig.pass(core::kGestureTimings.hold_ms + 600);  // far past the hold time of Song: no peek
    CHECK_FALSE(rig.app->state().peeking);
    CHECK(rig.app->state().screen == before);
    CHECK(rig.app->state().overlay == app::OverlayId::Navigator);
    const int cell = rig.app->state().navigator;
    rig.turn(1);  // both keys still down: the Navigator's cursor, not the master volume (Master held) and not the tempo (Pattern held)
    CHECK(rig.app->state().navigator != cell);
    CHECK(rig.message().rfind("MASTER VOLUME ", 0) != 0u);
    CHECK(rig.project().tempo_x100() == tempo);
    rig.release(pair[1]);
    rig.release(pair[0]);
    rig.pass(1);
    CHECK(rig.app->state().overlay == app::OverlayId::Navigator);  // the releases accept nothing
    CHECK(rig.app->state().screen == before);
    CHECK_FALSE(rig.app->state().peeking);
    CHECK(rig.project().tempo_x100() == tempo);
    CHECK(rig.journal.depth() == depth);
    rig.tap(Button::F7);  // CLOSE
    CHECK(rig.app->state().overlay == app::OverlayId::None);
    CHECK(rig.app->state().screen == before);
  }
}

TEST_CASE("navigator chords: Master + Song and Song + Pattern work on every screen, and the plain taps and holds of the three buttons still do their own things (OS-APPSNAV)") {
  for (const Button from : {Button::Pattern, Button::Song, Button::Params, Button::Master, Button::File}) {
    Rig rig;
    rig.tap(from);
    const app::ScreenId before = rig.app->state().screen;
    rig.press(Button::Master);
    rig.pass(1);
    rig.press(Button::Song);
    rig.pass(1);
    rig.release(Button::Song);
    rig.release(Button::Master);
    rig.pass(1);
    CHECK(rig.app->state().overlay == app::OverlayId::Navigator);
    CHECK(rig.app->state().screen == before);
  }
  Rig rig;
  rig.tap(Button::Master);  // the taps
  CHECK(rig.app->state().screen == app::ScreenId::Mixer);
  rig.tap(Button::Song);
  CHECK(rig.app->state().screen == app::ScreenId::Song);
  rig.tap(Button::Pattern);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  rig.press(Button::Song);  // a held Song is still the peek, alone
  rig.pass(core::kGestureTimings.hold_ms + 1);
  CHECK(rig.app->state().peeking);
  CHECK(rig.app->state().screen == app::ScreenId::Song);
  rig.release(Button::Song);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  rig.press(Button::Master);  // and Master held + the encoder is the volume, alone
  rig.pass(1);
  rig.turn(-2);
  CHECK(rig.message().rfind("MASTER VOLUME ", 0) == 0u);
  rig.release(Button::Master);
  rig.pass(1);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
}

TEST_CASE("mixer inserts chord: Master + Insert, in either order, opens the Mixer on INSERTS from any screen and fires neither tap (OS-MIXINSKEY)") {
  const std::size_t mixer = app::screen_index(app::ScreenId::Mixer);
  const std::uint8_t inserts = static_cast<std::uint8_t>(app::MixerPage::Inserts);
  for (const Button from : {Button::Pattern, Button::Song, Button::Params, Button::File, Button::Master}) {
    for (int order = 0; order < 2; ++order) {
      Rig rig;
      rig.tap(from);
      if (from == Button::Master) {
        rig.tap(Button::Master);  // a second tap: the Mixer's next page, so the chord must move it to INSERTS, not cycle on
      }
      NEED(rig.app->state().page[mixer] != inserts);
      const auto depth = rig.journal.depth();
      rig.press(order == 0 ? Button::Master : Button::Insert);
      rig.pass(1);
      rig.press(order == 0 ? Button::Insert : Button::Master);
      rig.pass(1);
      CHECK(rig.app->state().screen == app::ScreenId::Mixer);
      CHECK(rig.app->state().page[mixer] == inserts);
      rig.pass(core::kGestureTimings.hold_ms + 600);  // far past the hold time: still nothing else
      rig.release(Button::Insert);
      rig.release(Button::Master);
      rig.pass(1);
      CHECK(rig.app->state().screen == app::ScreenId::Mixer);  // no Master tap at the release (no screen switch, no page cycle)
      CHECK(rig.app->state().page[mixer] == inserts);
      CHECK(rig.app->state().overlay == app::OverlayId::None);
      CHECK(rig.journal.depth() == depth);  // Insert's own tap (step.insert, song.add_slot, mixer.stem_set) did not run
    }
  }
  Rig rig;  // on the INSERTS page already: the chord stays there
  rig.tap(Button::Master);
  rig.press(Button::Master);
  rig.pass(1);
  rig.press(Button::Insert);
  rig.pass(1);
  rig.release(Button::Insert);
  rig.release(Button::Master);
  rig.pass(1);
  rig.press(Button::Master);
  rig.pass(1);
  rig.press(Button::Insert);
  rig.pass(1);
  rig.release(Button::Insert);
  rig.release(Button::Master);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Mixer);
  CHECK(rig.app->state().page[mixer] == inserts);
  rig.tap(Button::Pattern);  // the plain keys are unchanged: Master is the Mixer again, Insert on Pattern is a step insert (no screen change)
  const auto depth = rig.journal.depth();
  rig.tap(Button::Insert);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);
  CHECK(rig.journal.depth() == depth);  // an insert on the empty pattern changes nothing; that plain Insert resolves to step.insert is builtin_bindings_test's
  rig.press(Button::Master);  // Master held + the encoder is still the volume
  rig.pass(1);
  rig.turn(-2);
  CHECK(rig.message().rfind("MASTER VOLUME ", 0) == 0u);
  rig.release(Button::Master);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);
}

TEST_CASE("pattern tempo: under a modal overlay the gesture does nothing, and neither does it in an app's page") {
  Rig rig;
  rig.press(Button::Vol);  // the Vol + Fx chord is Learn
  rig.pass(1);
  rig.press(Button::Fx);
  rig.pass(1);
  rig.release(Button::Fx);
  rig.release(Button::Vol);
  rig.pass(1);
  NEED(rig.app->state().overlay == app::OverlayId::Learn);
  rig.press(Button::Pattern);
  rig.pass(1);
  rig.turn(4);
  rig.release(Button::Pattern);
  rig.pass(1);
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100);
  CHECK(rig.app->state().overlay == app::OverlayId::Learn);

  Rig nav;
  nav.open_navigator();
  NEED(nav.app->state().overlay == app::OverlayId::Navigator);
  nav.press(Button::Pattern);
  nav.pass(1);
  nav.turn(4);
  nav.release(Button::Pattern);
  nav.pass(1);
  CHECK(nav.project().tempo_x100() == model::kDefaultTempoX100);
}

TEST_CASE("pattern tempo: a clock that is somebody else's (MIDI Slave) refuses with the DJ page's line, and nothing is written") {
  Rig rig;
  rig.tap(Button::Params);
  (void)tempo_sent(rig);
  CHECK(rig.app->device_settings().set(model::device_setting::ClockMode, 2.0f) == model::Status::Ok);
  const std::uint32_t t0 = rig.project().tempo_x100();
  const unsigned depth = rig.journal.depth();
  rig.press(Button::Pattern);
  rig.pass(1);
  rig.turn(3);
  CHECK(rig.project().tempo_x100() == t0);
  CHECK(rig.message() == "MIDI SLAVE: THE CLOCK SETS THE TEMPO");
  CHECK(tempo_sent(rig).empty());
  CHECK(rig.journal.depth() == depth);
  rig.release(Button::Pattern);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Instrument);  // still the turn's tap
  CHECK(rig.app->device_settings().set(model::device_setting::ClockMode, 0.0f) == model::Status::Ok);
  rig.press(Button::Pattern);
  rig.pass(1);
  rig.turn(3);
  CHECK(rig.project().tempo_x100() == t0 + 300u);  // the clock ours again
  rig.release(Button::Pattern);
  rig.pass(1);
}

TEST_CASE("pattern tempo: Master + the encoder and Pattern F1 + the encoder are unchanged, and a Shift that joins after the Pattern press ends the gesture") {
  Rig rig;
  rig.tap(Button::Pattern);
  rig.press(Button::Master);
  rig.pass(1);
  rig.turn(-2);
  CHECK(rig.message().rfind("MASTER VOLUME ", 0) == 0u);
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100);
  rig.release(Button::Master);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);

  rig.press(Button::F1);  // the PATTERN chooser (OS-PATSWITCH) is the hold of F1; the encoder is the chooser's
  rig.pass(600);
  NEED(rig.app->state().overlay == app::OverlayId::PatternChooser);
  rig.turn(1);
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100);
  rig.release(Button::F1);
  rig.pass(1);

  rig.press(Button::Pattern);  // accepted limit, as with Master: Shift joins after the key, the gesture ends and the turn is the screen's own
  rig.pass(1);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.turn(4);
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100);
  rig.release(Button::Pattern);
  rig.release(Button::Shift);
  rig.pass(1);
}

// ---- OS-PATTEMPO: Pattern held + a pad is a tempo tap -------------------------------------------------------------------------------------------------------

namespace {

struct NoteLog final : app::PreviewSink {
  unsigned heard = 0;
  unsigned ups = 0;  // pad releases that reached the pad logic
  void pad_up(unsigned) noexcept override { ++ups; }
  void preview(unsigned, unsigned) noexcept override { ++heard; }
  void pad_down(unsigned, unsigned, unsigned, unsigned) noexcept override { ++heard; }
};

void pad_tap(Rig& rig, unsigned pad, unsigned after_ms) {
  rig.pass(after_ms);
  rig.input.push(core::make_pad_event(rig.clock.now(), static_cast<std::uint8_t>(pad), 100));
  rig.pass(0);
  rig.input.push(core::make_pad_event(rig.clock.now(), static_cast<std::uint8_t>(pad), 0));
  rig.pass(0);
}

}  // namespace

TEST_CASE("pattern tap tempo: Pattern held + pad taps 500 ms apart make 120.0, the strip says TAP then TAP 120.0 BPM, the release does not navigate, the pad plays no note") {
  for (const Button screen : {Button::Song, Button::Pattern}) {
    Rig rig;
    NoteLog notes;
    rig.app->attach_preview(&notes);
    rig.tap(screen);
    const app::ScreenId where = rig.app->state().screen;
    (void)tempo_sent(rig);
    rig.press(Button::Pattern);
    rig.pass(1);
    pad_tap(rig, 5, 1);
    CHECK(rig.message() == "TAP");
    CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100);  // one tap sets nothing
    pad_tap(rig, 9, 500);
    CHECK(rig.project().tempo_x100() == 12000u);
    CHECK(rig.message() == "TAP 120.0 BPM");
    pad_tap(rig, 5, 500);
    pad_tap(rig, 7, 500);
    CHECK(rig.project().tempo_x100() == 12000u);
    const std::vector<float> sent = tempo_sent(rig);
    NEED(!sent.empty());
    CHECK(sent.back() == doctest::Approx(120.0f));
    CHECK(notes.heard == 0u);  // no note, whatever pad
    rig.release(Button::Pattern);
    rig.pass(1);
    CHECK(rig.app->state().screen == where);  // the release after a tap is silent
    if (screen == Button::Pattern) {
      pad_tap(rig, 3, 1);  // Pattern let go: a pad is a pad again
      CHECK(notes.heard == 1u);
      CHECK(rig.project().tempo_x100() == 12000u);
    }
    rig.tap(Button::F8);  // the edit is an undo unit on the history of the screen in view
    CHECK(rig.project().tempo_x100() != 12000u);
  }
}

TEST_CASE("pattern tap tempo: a pause of more than 2 s starts a new series, the mean is of the last 4 intervals, and the range is 40..300") {
  Rig rig;
  rig.press(Button::Pattern);
  rig.pass(1);
  pad_tap(rig, 0, 1);
  pad_tap(rig, 0, 500);  // 120
  CHECK(rig.project().tempo_x100() == 12000u);
  pad_tap(rig, 0, 2100);  // a new series: this is its first tap
  CHECK(rig.message() == "TAP");
  CHECK(rig.project().tempo_x100() == 12000u);
  pad_tap(rig, 0, 600);  // 100
  CHECK(rig.project().tempo_x100() == 10000u);
  pad_tap(rig, 0, 600);
  pad_tap(rig, 0, 600);
  pad_tap(rig, 0, 600);
  pad_tap(rig, 0, 300);  // five intervals in: the last four are 600, 600, 600, 300: the mean 525 ms is 114.3
  CHECK(rig.project().tempo_x100() == 11430u);
  CHECK(rig.message() == "TAP 114.3 BPM");
  rig.release(Button::Pattern);
  rig.pass(1);

  Rig fast;  // 100 ms apart would be 600 BPM: the taps stop at 300
  fast.press(Button::Pattern);
  fast.pass(1);
  pad_tap(fast, 0, 1);
  pad_tap(fast, 0, 100);
  CHECK(fast.project().tempo_x100() == 30000u);
  CHECK(fast.message() == "TAP 300.0 BPM");
  pad_tap(fast, 0, 2100);  // a new series, whose 1.99 s interval is 30.1 BPM: the project's floor, 40
  pad_tap(fast, 0, 1990);
  CHECK(fast.project().tempo_x100() == model::kMinTempoX100);
  fast.release(Button::Pattern);
  fast.pass(1);
}

TEST_CASE("pattern tap tempo: nothing under a modal overlay or MIDI Slave, and a pad without Pattern is not a tap") {
  Rig rig;
  rig.press(Button::Vol);
  rig.pass(1);
  rig.press(Button::Fx);
  rig.pass(1);
  rig.release(Button::Fx);
  rig.release(Button::Vol);
  rig.pass(1);
  NEED(rig.app->state().overlay == app::OverlayId::Learn);
  rig.press(Button::Pattern);
  rig.pass(1);
  pad_tap(rig, 0, 1);
  pad_tap(rig, 0, 500);
  CHECK(rig.project().tempo_x100() == model::kDefaultTempoX100);
  rig.release(Button::Pattern);
  rig.pass(1);

  Rig slave;
  CHECK(slave.app->device_settings().set(model::device_setting::ClockMode, 2.0f) == model::Status::Ok);
  slave.press(Button::Pattern);
  slave.pass(1);
  pad_tap(slave, 0, 1);
  pad_tap(slave, 0, 500);
  CHECK(slave.project().tempo_x100() == model::kDefaultTempoX100);
  CHECK(slave.message() == "MIDI SLAVE: THE CLOCK SETS THE TEMPO");
  slave.release(Button::Pattern);
  slave.pass(1);
  CHECK(slave.app->state().screen == app::ScreenId::Song);

  Rig plain;  // no Pattern: the taps of two pads 500 ms apart change nothing
  pad_tap(plain, 0, 1);
  pad_tap(plain, 0, 500);
  CHECK(plain.project().tempo_x100() == model::kDefaultTempoX100);
}

TEST_CASE("pattern tap tempo: a tap whose release was lost, then reset_input or a normal press, leaves no pad hung") {
  Rig rig;
  NoteLog notes;
  rig.app->attach_preview(&notes);
  rig.tap(Button::Pattern);
  rig.press(Button::Pattern);
  rig.pass(1);
  rig.input.push(core::make_pad_event(rig.clock.now(), 5, 100));  // a tap, and its release is lost with the input
  rig.pass(0);
  CHECK(notes.heard == 0u);
  rig.app->reset_input();
  rig.pass(1);
  pad_tap(rig, 5, 1);  // a normal press and release: the release is the pad logic's (it ends the note), not eaten as the stale tap's
  CHECK(notes.heard == 1u);
  CHECK(notes.ups == 1u);

  rig.press(Button::Pattern);  // the same without a reset: the release is lost, Pattern is let go, the pad is pressed again
  rig.pass(1);
  rig.input.push(core::make_pad_event(rig.clock.now(), 6, 100));
  rig.pass(0);
  rig.release(Button::Pattern);
  rig.pass(1);
  pad_tap(rig, 6, 1);
  CHECK(notes.heard == 2u);
  CHECK(notes.ups == 2u);
}

TEST_CASE("pattern tempo: the taps and detents of one Pattern hold are one undo unit: one Undo restores the tempo from before the hold") {
  Rig rig;
  rig.tap(Button::Song);
  const std::uint32_t t0 = rig.project().tempo_x100();
  rig.press(Button::Pattern);
  rig.pass(1);
  for (unsigned i = 0; i < 8u; ++i) {
    pad_tap(rig, i, i == 0 ? 1u : 400u);
  }
  CHECK(rig.project().tempo_x100() == 15000u);
  rig.turn(2);
  CHECK(rig.project().tempo_x100() == 15200u);
  pad_tap(rig, 3, 400);
  rig.turn(-1);
  rig.release(Button::Pattern);
  rig.pass(1);
  CHECK(rig.project().tempo_x100() != t0);
  rig.tap(Button::F8);  // Undo, once
  CHECK(rig.project().tempo_x100() == t0);
  rig.tap(Button::F8);  // nothing else of the hold is left on the history
  CHECK(rig.project().tempo_x100() == t0);
}

TEST_CASE("song edit: Copy held + F3 (ADD EMPTY, OS-SONG3 S5) adds a slot on the first free pattern, the cursor on it, one undo unit; a full pool says NO FREE PATTERN and records nothing") {
  Rig rig;
  rig.tap(Button::Down);
  CHECK(rig.song().pos() == 1);
  rig.press(Button::Copy);
  rig.pass(1);
  rig.tap(Button::F3);  // Copy held + F3: ADD EMPTY, not ADD SLOT
  rig.release(Button::Copy);
  rig.pass(1);
  CHECK(rig.order() == Order{1, 2, 4, 3});  // pattern 4 does not exist: the new slot is empty
  CHECK(rig.project().pattern(4) == nullptr);
  CHECK(rig.song().pos() == 2);
  CHECK(rig.editor->can_undo());
  // A second press in a row goes to the NEXT free number (5), not to 4 again: the order list counts as used too.
  rig.press(Button::Copy);
  rig.pass(1);
  rig.tap(Button::F3);
  rig.release(Button::Copy);
  rig.pass(1);
  CHECK(rig.order() == Order{1, 2, 4, 5, 3});
  CHECK(rig.project().pattern(5) == nullptr);
  CHECK(rig.song().pos() == 3);
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 4, 3});  // one unit a press
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 3});
  CHECK_FALSE(rig.editor->can_undo());
  rig.tap(Button::F3);  // the plain F3 still adds the same pattern and does not wait for the release (the cursor stayed on slot 2, pattern 3: the new slot lands after it)
  CHECK(rig.order() == Order{1, 2, 3, 3});
  // A full pool refuses and records nothing.
  Rig full;
  model::Project& p = full.project();
  for (unsigned id = 4; id <= model::kMaxPatterns; ++id) {
    NEED(p.create_pattern(id, 8) == model::Status::Ok);
  }
  full.editor->history_reset();
  const Order before = full.order();
  full.press(Button::Copy);
  full.pass(1);
  full.tap(Button::F3);
  full.release(Button::Copy);
  full.pass(1);
  CHECK(full.message() == "NO FREE PATTERN");
  CHECK(full.order() == before);
  CHECK_FALSE(full.editor->can_undo());
  CHECK(full.project().check_invariants());
}
