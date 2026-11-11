// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-METRO, the app's side (docs/specs/OS-METRO.md sections 2 and 4), on the host:
//   * Shift + REC from stop (OS-RECMODE2: LIVE) asks the transport to count in when COUNT-IN is on; Play never does (Browse, STEP, the Song screen, COUNT-IN Off just play or stop);
//   * during the count-in nothing is recorded or entered: a pad sounds and writes no note, no take is opened, the REC frame is not red; at the start point the recording begins;
//   * the notification strip counts the beats left ("RECORDING IN 4.. 3.. 2.. 1.."), once a beat, in place, and the pads flash each beat, the first of a bar brighter;
//   * the Record screen's F1 from stop with an aligned take and COUNT-IN on arms the take to wait for the start point and starts the transport counting in.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/pad_palette.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/core/gesture.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::PadRole;
using app::PatternMode;
using core::ActionArg;
namespace ds = o3os::model::device_setting;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

struct Sink final : app::TransportSink {
  unsigned plays = 0;
  unsigned counted = 0;
  unsigned songs = 0;
  unsigned stops = 0;
  unsigned pattern = 0;
  unsigned bars = 0;
  bool play(unsigned p) noexcept override {
    ++plays;
    pattern = p;
    return true;
  }
  bool play_count_in(unsigned p, unsigned b) noexcept override {
    ++counted;
    pattern = p;
    bars = b;
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

// What the engine would be asked: a pad's note and its release (OS-BUG-STEPSND).
struct PadCount final : app::PreviewSink {
  unsigned downs = 0, ups = 0;
  void preview(unsigned, unsigned) noexcept override {}
  void pad_down(unsigned, unsigned, unsigned, unsigned) noexcept override { ++downs; }
  void pad_up(unsigned) noexcept override { ++ups; }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  audio::PlayPosition pos;
  Sink sink;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  explicit Rig(bool edit = true) {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, 64);
    (void)p.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_play_position(&pos);
    app->attach_transport(&sink);
    app->run_once();
    if (edit) {
      (void)app->invoke(core::action_id("view.mode"), static_cast<ActionArg>(PatternMode::Edit));
    }
    use_c_major();
  }
  model::Project& project() { return test::arena(0).project; }
  const app::ViewState& state() const { return app->state(); }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  // The sequencer's published position: `step` of pattern 1 at 120 BPM, `left` steps of the count-in still to run.
  void play(unsigned step, unsigned left = 0, bool playing = true, unsigned ustep = 0) {
    pos.publish(playing, 1, step, 12000, audio::PlayPosition::kNoOrder, ustep);
    pos.publish_count_in(left);
    pass();
  }
  void press(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    pass();
  }
  void release(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void tap(Button b) {
    press(b);
    release(b);
  }
  void shift_tap(Button b) {
    press(Button::Shift);
    tap(b);
    release(Button::Shift);
  }
  void pad_down(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 100));
    pass();
  }
  void pad_up(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 0));
    pass();
  }
  const model::Step& step(int track, int row) { return project().pattern(1)->steps[track][row]; }
  void set(unsigned field, unsigned value) { CHECK(app->device_settings().set(field, static_cast<float>(value)) == model::Status::Ok); }
  std::string message() const { return app->message(); }
  // OS-NOTESLED2: C major through the note-entry panel (held Note, then its pads), so the NOTES board is lit in two levels: C Front (in the scale), C# Back (outside it).
  void use_c_major() {
    press(Button::Note);
    pass(core::kGestureTimings.hold_ms + 1);
    pad_down(0);
    pad_up(0);
    pad_down(12 + 1);
    pad_up(12 + 1);
    release(Button::Note);
    pass();
  }
  // With no count-in the pads are the NOTES board's lights (C major: C Front, C# Back), neither a count-in's flash (all Front or all Back) nor nothing.
  bool notes_lit() const {
    PadRole roles[48];
    return app->pad_lights(roles) && roles[36] == PadRole::Front && roles[37] == PadRole::Back;
  }
};

}  // namespace

TEST_CASE("metro app: Shift + REC from stop counts in (OS-RECMODE2) and Play never does; COUNT-IN Off just plays") {
  Rig rig(false);
  rig.set(ds::CountIn, 2);
  // Play from stop, in WATCH or in STEP, never counts in.
  rig.tap(Button::Play);
  CHECK(rig.sink.plays == 1u);
  CHECK(rig.sink.counted == 0u);
  rig.play(3, 0);
  rig.tap(Button::Play);
  CHECK(rig.sink.stops == 1u);
  rig.play(0, 0, false);
  rig.tap(Button::Rec);  // STEP
  rig.tap(Button::Play);
  CHECK(rig.sink.plays == 2u);
  CHECK(rig.sink.counted == 0u);
  rig.play(3, 0);
  rig.tap(Button::Play);
  rig.play(0, 0, false);
  // Shift + REC from stop, two bars.
  rig.shift_tap(Button::Rec);
  CHECK(rig.sink.counted == 1u);
  CHECK(rig.sink.bars == 2u);
  CHECK(rig.sink.pattern == 1u);
  CHECK(rig.sink.plays == 2u);
  CHECK(rig.state().playing);
  CHECK(rig.state().mode[app::screen_index(app::ScreenId::Pattern)] == static_cast<unsigned>(PatternMode::Edit));
  CHECK_FALSE(rig.state().edit.rec_free);  // LIVE
  rig.play(0, 16);
  CHECK(rig.state().pattern.row() == 0);  // OS-COUNTIN: the play line (the view follows it) is on row 1 while it counts
  rig.play(0, 9);
  CHECK(rig.state().pattern.row() == 0);
  rig.play(0, 0, false);  // the end of the run: LIVE ends
  // One bar.
  rig.set(ds::CountIn, 1);
  rig.shift_tap(Button::Rec);
  CHECK(rig.sink.counted == 2u);
  CHECK(rig.sink.bars == 1u);
  rig.play(0, 16);
  rig.play(0, 0, false);
  // COUNT-IN Off: LIVE at once, a plain play.
  rig.set(ds::CountIn, 0);
  rig.shift_tap(Button::Rec);
  CHECK(rig.sink.plays == 3u);
  CHECK(rig.sink.counted == 2u);
  CHECK(rig.state().playing);
}

TEST_CASE("metro app: Shift + Play (the Focus Loop's key) never counts in") {
  Rig rig;
  rig.set(ds::CountIn, 2);
  rig.press(Button::Shift);
  rig.tap(Button::Play);
  rig.release(Button::Shift);
  CHECK(rig.sink.counted == 0u);
  CHECK(rig.sink.plays == 1u);
}

TEST_CASE("metro app: Browse, another screen and the song just play") {
  Rig browse(false);  // the Pattern screen starts in Browse
  browse.set(ds::CountIn, 2);
  browse.tap(Button::Play);
  CHECK(browse.sink.counted == 0u);
  CHECK(browse.sink.plays == 1u);
  Rig song;
  song.set(ds::CountIn, 2);
  song.tap(Button::Song);
  song.tap(Button::Play);
  CHECK(song.sink.counted == 0u);
  CHECK(song.sink.songs == 1u);
}

TEST_CASE("metro app: during the count-in a pad sounds and writes nothing, no take is open, and the REC mark waits; at the start point the recording begins") {
  Rig rig;
  PadCount pads;
  rig.app->attach_preview(&pads);
  rig.set(ds::RecQuantize, 1);  // Playing step: the step the playhead is on
  rig.play(0, 8);              // the play line waits on row 1 (OS-COUNTIN), two beats of the count-in left
  CHECK(app::pattern_counting_in(rig.app->model(), rig.state()));
  CHECK_FALSE(app::pattern_recording(rig.app->model(), rig.state()));
  rig.pad_down(12);
  rig.pad_up(12);
  CHECK(pads.downs == 1u);  // OS-BUG-STEPSND: it sounds (the engine's live note), it did not
  CHECK(pads.ups == 1u);
  CHECK(rig.step(0, 56).note == 0u);
  CHECK(rig.step(0, 0).note == 0u);
  CHECK_FALSE(rig.state().edit.take_open);
  CHECK_FALSE(rig.state().edit.take_wrote);
  CHECK(rig.journal.depth() == 0u);
  // The start point: the count-in is over and the same pad records at the playhead's step.
  rig.play(0, 0);
  CHECK_FALSE(app::pattern_counting_in(rig.app->model(), rig.state()));
  CHECK(app::pattern_recording(rig.app->model(), rig.state()));
  rig.pad_down(12);
  CHECK(rig.step(0, 0).note != 0u);
  CHECK(rig.state().edit.take_open);
  rig.pad_up(12);
}

// Spec section 2: "the nearest-step rule of REC QUANTIZE still applies at the start boundary". A hit in the last half step of the pre-roll rounds up into the start step, as at any step; it is a take of
// its own undo unit from that hit on. The rule is Nearest's: under Playing step the step the playhead is on is the count-in's, and the hit is dropped.
TEST_CASE("metro app: the last half step of the count-in rounds a hit into the start step under REC QUANTIZE Nearest, and under Playing step it does not") {
  {
    Rig rig;  // REC QUANTIZE Nearest is the default
    rig.play(0, 1, true, 10);  // the count-in's last step (the line waits on the start step): its first half
    CHECK(app::pattern_counting_in(rig.app->model(), rig.state()));
    CHECK_FALSE(app::pattern_recording(rig.app->model(), rig.state()));
    rig.pad_down(12);
    rig.pad_up(12);
    CHECK(rig.step(0, 0).note == 0u);
    CHECK(rig.step(0, 63).note == 0u);
    CHECK_FALSE(rig.state().edit.take_open);
    rig.play(0, 2, true, 40);  // the step before the last one: its second half is not the start boundary
    CHECK_FALSE(app::pattern_recording(rig.app->model(), rig.state()));
    rig.pad_down(12);
    rig.pad_up(12);
    CHECK(rig.step(0, 63).note == 0u);
    CHECK(rig.step(0, 0).note == 0u);
    rig.play(0, 1, true, 24);  // the second half of the count-in's last step: it belongs to the start step
    CHECK(app::pattern_recording(rig.app->model(), rig.state()));
    CHECK_FALSE(app::pattern_counting_in(rig.app->model(), rig.state()));
    rig.pad_down(12);
    CHECK(rig.step(0, 0).note != 0u);
    CHECK(rig.step(0, 63).note == 0u);
    CHECK(rig.state().edit.take_open);
    rig.pad_up(12);
    CHECK(rig.step(0, 1).note == model::kNoteOff);  // released within the start step: it ends in the next, as any note does
    rig.play(0, 0, true, 0);  // the start point: the take goes on, in the same unit
    CHECK(app::pattern_recording(rig.app->model(), rig.state()));
    CHECK(rig.state().edit.take_open);
  }
  {
    Rig rig;
    rig.set(ds::RecQuantize, 1);  // Playing step
    rig.play(0, 1, true, 40);
    CHECK_FALSE(app::pattern_recording(rig.app->model(), rig.state()));
    CHECK(app::pattern_counting_in(rig.app->model(), rig.state()));
    rig.pad_down(12);
    rig.pad_up(12);
    CHECK(rig.step(0, 0).note == 0u);
    CHECK(rig.step(0, 63).note == 0u);
    CHECK_FALSE(rig.state().edit.take_open);
  }
}

TEST_CASE("metro app: the strip counts the beats left, a beat at a time, and the pads flash on each beat, brighter on the first of a bar") {
  Rig rig;
  rig.set(ds::CountIn, 2);
  PadRole roles[48];
  rig.play(0, 32);  // two bars: 8 beats left, the first of the first bar
  CHECK(rig.message() == "RECORDING IN 4..");
  CHECK(rig.app->pad_lights(roles));
  for (const PadRole role : roles) {
    CHECK(role == PadRole::Front);  // the bar line
  }
  rig.play(0, 31);  // inside the beat: the text stays, the flash goes on its own time
  CHECK(rig.message() == "RECORDING IN 4..");
  rig.pass(150);  // past the flash
  CHECK(rig.notes_lit());
  rig.play(0, 28);  // the second beat
  CHECK(rig.message() == "RECORDING IN 4.. 3..");
  CHECK(rig.app->pad_lights(roles));
  for (const PadRole role : roles) {
    CHECK(role == PadRole::Back);  // a beat inside the bar
  }
  rig.play(0, 24);
  CHECK(rig.message() == "RECORDING IN 4.. 3.. 2..");
  rig.play(0, 20);
  CHECK(rig.message() == "RECORDING IN 4.. 3.. 2.. 1..");
  rig.play(0, 16);  // the second bar: the countdown starts again, and the flash is the bar's
  CHECK(rig.message() == "RECORDING IN 4..");
  CHECK(rig.app->pad_lights(roles));
  for (const PadRole role : roles) {
    CHECK(role == PadRole::Front);
  }
  // A pass that missed beats shows the beat it finds: 2 beats left is a "2".
  rig.play(0, 8);
  CHECK(rig.message() == "RECORDING IN 4.. 3.. 2..");
  rig.play(0, 4);
  CHECK(rig.message() == "RECORDING IN 4.. 3.. 2.. 1..");
  // The count-in is over: the last digit goes with its beat (a beat is 500 ms at 120 BPM), and the pads are the screen's again.
  rig.play(0, 0);
  rig.pass(1000);
  CHECK(rig.message().empty());
  CHECK(rig.notes_lit());
}

TEST_CASE("metro app: a stopped transport shows no count-in, and a stop in the middle puts the pads back") {
  Rig rig;
  PadRole roles[48];
  rig.play(0, 0, false);
  CHECK(rig.message().empty());
  CHECK(rig.notes_lit());
  rig.play(0, 14);
  CHECK(rig.app->pad_lights(roles));
  CHECK(rig.message() == "RECORDING IN 4..");
  rig.play(0, 0, false);  // stopped (the published count-in is 0 with it)
  CHECK(rig.message().empty());  // the countdown goes with the stop, not a beat (500 ms at 120 BPM) and 150 ms later
  rig.pass(200);
  CHECK(rig.notes_lit());
  CHECK(rig.state().count_in.beat == 0u);
  // A stale count-in word of a stopped transport is ignored.
  rig.play(0, 9, false);
  CHECK(rig.state().count_in.beat == 0u);
}
