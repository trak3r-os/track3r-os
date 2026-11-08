// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FXREC S2 (docs/specs/OS-FXREC.md, the wiring of section 3.2 to 3.5) on the app rig with a playing sequencer stub: a MIDI Learn binding on a mapped instrument parameter writes its step
// FX at the play line of the cursor's track during a LIVE take, in the take's undo unit, with the strip messages; outside a take nothing differs from today. The pure map and column rule are
// rec_fx_test.cpp. Host only (the app, the host fakes and the play position stub).
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/rec_fx.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/core/learn.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/instrument.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/seq/fx.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::DispatchResult;
using app::PatternMode;
using core::ActionArg;
using core::ParamId;
using core::ParamKind;
namespace ip = model::instrument_param;
namespace tp = model::track_param;
namespace seq = o3os::seq;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

ParamId inst(unsigned slot, unsigned field) { return core::param_id(ParamKind::Instrument, slot, field); }

ActionArg cc(unsigned channel, unsigned controller, unsigned value) {
  return static_cast<ActionArg>((channel << 16) | (controller << 8) | value);
}

core::LearnBinding binding(unsigned slot, unsigned field, std::uint8_t num, core::LearnMode mode, std::uint8_t flags = 0) {
  return core::LearnBinding{inst(slot, field), num, 0, static_cast<std::uint8_t>(mode), flags};
}

// The app rig of rec_test.cpp with a document of our own: pattern 1 (16 steps, one position) and two sample instruments, slot 0 (Low-pass, cutoff 0.30) and slot 1 (High-pass).
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  audio::PlayPosition pos;
  core::ParamRing ring;
  // Declared after the editor, so that it is destroyed before it: ~App closes the take's undo group on the editor (OS-LIVEREC).
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  explicit Rig(unsigned length = 16, bool edit = true, std::size_t ring_bytes = sizeof g_ring) {
    model::Project& p = project();
    p.reset();
    (void)p.create_pattern(1, length);
    (void)p.append_order(1);
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::memcpy(in.name, "TEST", 4);
    (void)p.set_instrument(0, in);
    (void)p.set_instrument(1, in);
    (void)p.set_param(inst(0, ip::Cutoff), 0.30f);
    (void)p.set_param(inst(0, ip::FilterType), 0.0f);
    (void)p.set_param(inst(1, ip::Cutoff), 0.30f);
    (void)p.set_param(inst(1, ip::FilterType), 1.0f);  // instrument 2 is High-pass
    journal.begin(g_ring, ring_bytes);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_play_position(&pos);
    app->attach_param_ring(&ring);
    app->run_once();
    if (edit) {
      live();
    }
  }
  // Edit following, LIVE (OS-RECMODE2), entered as the tests always did.
  void live() { (void)app->invoke(core::action_id("view.mode"), static_cast<ActionArg>(PatternMode::Edit)); }
  model::Project& project() { return test::arena(2).project; }
  const app::ViewState& state() const { return app->state(); }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  // The sequencer is at `step` of pattern 1, `ustep` of its 48 microsteps played (or stopped).
  void play(unsigned step, unsigned ustep = 0, bool playing = true) {
    pos.publish(playing, 1, step, 12000, audio::PlayPosition::kNoOrder, ustep);
    pos.publish_count_in(0);
    pass();
  }
  void play_pattern(unsigned pattern, unsigned step, unsigned ustep = 0) {
    pos.publish(true, pattern, step, 12000, audio::PlayPosition::kNoOrder, ustep);
    pos.publish_count_in(0);
    pass();
  }
  void count_in(unsigned step, unsigned ustep) {
    pos.publish(true, 1, step, 12000, audio::PlayPosition::kNoOrder, ustep);
    pos.publish_count_in(2);
    pass();
  }
  void stop() { play(0, 0, false); }
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
  void pad_down(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 100));
    pass();
  }
  void pad_up(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 0));
    pass();
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  DispatchResult midicc(unsigned channel, unsigned num, unsigned value) { return invoke("midi.cc", cc(channel, num, value)); }
  void bind(unsigned slot, unsigned field, std::uint8_t num, core::LearnMode mode, std::uint8_t flags = 0) {
    NEED(editor->set_learn_slot(0, binding(slot, field, num, mode, flags)) == model::Status::Ok);
  }
  void bind_track(unsigned track, unsigned field, std::uint8_t num) {  // a Mixer parameter (A-7): the device's Learn store
    NEED(app->device_settings().set_learn(0, core::LearnBinding{core::param_id(ParamKind::Track, track, field), num, 0,
                                                                static_cast<std::uint8_t>(core::LearnMode::Jump), 0}) == model::Status::Ok);
  }
  const model::Step& step(int track, int row) { return project().pattern(1)->steps[track][row]; }
  unsigned depth() const { return journal.depth(); }
  int track() const { return app->view().track(); }
  void go_track(int t) { (void)invoke("cursor.track", t - track()); }
  float value(ParamId id) { return project().get_param(id); }
  // The newest notification in the queue.
  std::string last_note() const {
    const core::NotifyQueue& q = app->state().notify;
    return q.empty() ? std::string() : std::string(q.at(q.size() - 1).text.c_str());
  }
};

// True when no step of the pattern holds any FX.
bool no_fx_anywhere(Rig& rig) {
  const model::Pattern* pat = rig.project().pattern(1);
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    for (unsigned r = 0; r < model::kMaxPatternLength; ++r) {
      if (pat->steps[t][r].fx[0].type != model::kFxNone || pat->steps[t][r].fx[1].type != model::kFxNone) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

TEST_CASE("fxrec app: a bound CUTOFF knob records the filter FX at the play line of the cursor's track during LIVE; a second message on the row changes the value, not the column; Nearest past half the step writes the next row") {
  Rig rig;
  rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
  rig.go_track(2);
  rig.play(3);
  CHECK(rig.state().edit.take_open);
  CHECK(rig.midicc(1, 74, 127) == DispatchResult::Changed);
  CHECK(rig.value(inst(0, ip::Cutoff)) == 1.0f);  // the parameter moved
  CHECK(rig.step(2, 3).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(2, 3).fx[0].value == 100);
  CHECK(rig.step(2, 3).fx[1].type == model::kFxNone);
  for (int t = 0; t < 12; ++t) {
    if (t != 2) {
      CHECK(rig.step(t, 3).fx[0].type == model::kFxNone);  // only the cursor's track
      CHECK(rig.step(t, 3).fx[1].type == model::kFxNone);
    }
  }
  for (int r = 0; r < 16; ++r) {
    if (r != 3) {
      CHECK(rig.step(2, r).fx[0].type == model::kFxNone);  // only the play line's row
    }
  }
  CHECK(rig.last_note().find("REC FX: CUTOFF") != std::string::npos);  // the take's first FX says the parameter
  // a second message on the same row overwrites the value in the same column: one value per step
  CHECK(rig.midicc(1, 74, 64) == DispatchResult::Changed);
  CHECK(rig.step(2, 3).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(2, 3).fx[0].value == 50);  // round(64 / 127 * 100)
  CHECK(rig.step(2, 3).fx[1].type == model::kFxNone);
  // REC QUANTIZE Nearest past half the step writes the next row
  rig.play(3, 30);
  CHECK(rig.midicc(1, 74, 0) == DispatchResult::Changed);
  CHECK(rig.step(2, 4).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(2, 4).fx[0].value == 0);
  CHECK(rig.step(2, 3).fx[0].value == 50);  // the earlier row keeps its value
  // the same value again changes nothing and records nothing (the cell is unchanged)
  const unsigned d = rig.depth();
  rig.play(4, 30);
  CHECK(rig.midicc(1, 74, 0) == DispatchResult::Unchanged);  // the parameter is already 0: nothing moves, nothing records
  CHECK(rig.step(2, 5).fx[0].type == model::kFxNone);
  CHECK(rig.depth() == d);
}

TEST_CASE("fxrec app: the FX type follows the instrument's stored FILTER TYPE (High-pass writes 28, Band-pass 29), and the value is the stored one") {
  Rig rig;
  rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
  (void)rig.project().set_param(inst(0, ip::FilterType), 1.0f);  // High-pass
  rig.play(5);
  CHECK(rig.midicc(1, 74, 127) == DispatchResult::Changed);
  CHECK(rig.step(0, 5).fx[0].type == seq::fx::kHighPass);
  CHECK(rig.step(0, 5).fx[0].value == 100);
  (void)rig.project().set_param(inst(0, ip::FilterType), 2.0f);  // Band-pass now
  rig.play(6);
  CHECK(rig.midicc(1, 74, 64) == DispatchResult::Changed);
  CHECK(rig.step(0, 6).fx[0].type == seq::fx::kBandPass);
  CHECK(rig.step(0, 6).fx[0].value == 50);
}

TEST_CASE("fxrec app: a Pickup binding that has not caught the knob writes nothing; once caught it records") {
  Rig rig;
  rig.bind(0, ip::Cutoff, 74, core::LearnMode::Pickup);  // the knob has to reach 0.30 first
  rig.play(3);
  CHECK(rig.midicc(1, 74, 100) == DispatchResult::Unchanged);  // not caught: nothing moves, nothing records
  CHECK(rig.value(inst(0, ip::Cutoff)) == 0.30f);
  CHECK(no_fx_anywhere(rig));
  CHECK(rig.midicc(1, 74, 38) == DispatchResult::Changed);  // 38 / 127 is 0.299: caught
  CHECK(rig.step(0, 3).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(0, 3).fx[0].value == 30);  // round(38 / 127 * 100)
}

TEST_CASE("fxrec app: a binding of instrument 2 with instrument 1 selected writes nothing (Q3 a); selected, it records; a follow binding follows") {
  Rig rig;
  rig.bind(1, ip::Cutoff, 75, core::LearnMode::Jump);  // a fixed binding on instrument 2's cutoff
  CHECK(rig.state().instrument == 1);
  rig.play(3);
  CHECK(rig.midicc(1, 75, 127) == DispatchResult::Changed);
  CHECK(rig.value(inst(1, ip::Cutoff)) == 1.0f);  // the parameter of instrument 2 moved...
  CHECK(no_fx_anywhere(rig));                     // ... but no FX: instrument 1 is selected
  (void)rig.invoke("instrument.select", 2);
  rig.play(4);
  CHECK(rig.midicc(1, 75, 64) == DispatchResult::Changed);
  CHECK(rig.step(0, 4).fx[0].type == seq::fx::kHighPass);  // instrument 2 is High-pass
  CHECK(rig.step(0, 4).fx[0].value == 50);
  // a follow binding acts on the selected instrument
  Rig f;
  f.bind(0, ip::Cutoff, 76, core::LearnMode::Jump, core::kLearnFollow);
  f.play(2);
  CHECK(f.midicc(1, 76, 127) == DispatchResult::Changed);
  CHECK(f.step(0, 2).fx[0].type == seq::fx::kLowPass);  // instrument 1 selected: its cutoff
  CHECK(f.value(inst(0, ip::Cutoff)) == 1.0f);
  (void)f.invoke("instrument.select", 2);
  f.play(6);
  CHECK(f.midicc(1, 76, 0) == DispatchResult::Changed);
  CHECK(f.step(0, 6).fx[0].type == seq::fx::kHighPass);  // now instrument 2's
  CHECK(f.value(inst(1, ip::Cutoff)) == 0.0f);
  CHECK(f.value(inst(0, ip::Cutoff)) == 1.0f);
}

TEST_CASE("fxrec app: no FX when Browse, stopped, STEP, counting in, or the playhead is in another pattern; the parameter still moves") {
  {
    Rig rig(16, false);  // Browse: no Edit
    rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
    rig.play(3);
    CHECK(rig.midicc(1, 74, 127) == DispatchResult::Changed);
    CHECK(rig.value(inst(0, ip::Cutoff)) == 1.0f);
    CHECK(no_fx_anywhere(rig));
  }
  {
    Rig rig;  // LIVE mode, but the transport is stopped: no take
    rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
    rig.stop();
    CHECK_FALSE(rig.state().edit.take_open);
    CHECK(rig.midicc(1, 74, 64) == DispatchResult::Changed);
    CHECK(no_fx_anywhere(rig));
  }
  {
    Rig rig(16, false);
    rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
    rig.tap(Button::Rec);  // WATCH -> STEP (Edit free): the playhead does not record
    rig.play(5);
    CHECK(rig.state().edit.rec_free);
    CHECK_FALSE(rig.state().edit.take_open);
    CHECK(rig.midicc(1, 74, 100) == DispatchResult::Changed);
    CHECK(no_fx_anywhere(rig));
  }
  {
    Rig rig;
    rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
    rig.count_in(0, 10);  // counting in: nothing is recorded yet
    CHECK_FALSE(rig.state().edit.take_open);
    CHECK(rig.midicc(1, 74, 90) == DispatchResult::Changed);
    CHECK(no_fx_anywhere(rig));
  }
  {
    Rig rig;
    rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
    rig.play(7);
    CHECK(rig.state().edit.take_open);
    rig.play_pattern(2, 7);  // the playhead is in another pattern
    CHECK(rig.midicc(1, 74, 80) == DispatchResult::Changed);
    CHECK(no_fx_anywhere(rig));
    rig.play(9);  // back in pattern 1: it records again
    CHECK(rig.state().edit.take_open);
    CHECK(rig.midicc(1, 74, 70) == DispatchResult::Changed);
    CHECK(rig.step(0, 9).fx[0].type == seq::fx::kLowPass);
    CHECK(rig.step(0, 9).fx[0].value == 55);  // round(70 / 127 * 100)
  }
}

TEST_CASE("fxrec app: a take of notes and FX is one undo unit; Undo takes the notes and the FX back and leaves the CUTOFF where the knob put it; no other unit was journalled in the take") {
  Rig rig;
  rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
  rig.go_track(1);
  rig.play(3);
  const unsigned d0 = rig.depth();
  rig.pad_down(12);  // a note at the play line
  CHECK(rig.step(1, 3).note != model::kNoteEmpty);
  CHECK(rig.midicc(1, 74, 127) == DispatchResult::Changed);  // the FX on the same row
  CHECK(rig.step(1, 3).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(1, 3).fx[0].value == 100);
  CHECK(rig.depth() == d0);  // the take's unit is still open: nothing was committed, no SOUND unit from the CC
  rig.play(5);
  rig.pad_up(12);  // the OFF: the playhead has left the note's step, so it goes to the play line's row
  CHECK(rig.step(1, 5).note == model::kNoteOff);
  rig.stop();  // the take ends: one unit
  CHECK(rig.depth() == d0 + 1u);
  CHECK(rig.value(inst(0, ip::Cutoff)) == 1.0f);
  (void)rig.invoke("edit.undo");  // one Undo removes the take: the note, the OFF and the FX
  CHECK(rig.depth() == d0);
  CHECK(rig.step(1, 3).note == model::kNoteEmpty);
  CHECK(rig.step(1, 3).fx[0].type == model::kFxNone);
  CHECK(rig.step(1, 5).note == model::kNoteEmpty);
  CHECK(rig.value(inst(0, ip::Cutoff)) == 1.0f);  // the parameter stays where the knob left it (Q1 a)
  (void)rig.invoke("edit.redo");
  CHECK(rig.step(1, 3).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(1, 3).fx[0].value == 100);
}

TEST_CASE("fxrec app: outside a take the CC is its own undo unit as before, and records no FX") {
  Rig rig;  // LIVE mode but the transport is stopped: no take
  rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
  CHECK_FALSE(rig.state().edit.take_open);
  const unsigned d0 = rig.depth();
  CHECK(rig.midicc(1, 74, 127) == DispatchResult::Changed);
  CHECK(rig.depth() == d0 + 1u);  // the parameter write is its own unit, as today
  CHECK(rig.value(inst(0, ip::Cutoff)) == 1.0f);
  CHECK(no_fx_anywhere(rig));
  (void)rig.invoke("edit.undo");
  CHECK(rig.value(inst(0, ip::Cutoff)) == 0.30f);  // and it undoes on its own
}

TEST_CASE("fxrec app: TUNE writes nothing (Q2 a), and a Mixer parameter records nothing and does not close the take (A-7)") {
  Rig rig;
  rig.bind(0, ip::Tune, 77, core::LearnMode::Jump);
  rig.bind_track(2, tp::Volume, 20);
  rig.play(3);
  CHECK(rig.state().edit.take_open);
  const unsigned d0 = rig.depth();
  CHECK(rig.midicc(1, 77, 60) == DispatchResult::Changed);
  CHECK(rig.value(inst(0, ip::Tune)) != 0.0f);  // the parameter moved
  CHECK(no_fx_anywhere(rig));
  CHECK(rig.midicc(1, 20, 100) == DispatchResult::Changed);  // the track's volume moved
  CHECK(rig.state().edit.take_open);                         // the take stayed open (kActionRecords on midi.cc)
  CHECK(rig.depth() == d0);                                  // and nothing was journalled for either move
  CHECK(no_fx_anywhere(rig));
}

TEST_CASE("fxrec app: a write with no free column says so once per take and leaves the step alone") {
  Rig rig;
  rig.bind(0, ip::Panning, 78, core::LearnMode::Jump);
  model::Step full = model::kEmptyStep;
  full.fx[0] = {seq::fx::kChance, 80};
  full.fx[1] = {seq::fx::kMicroMove, 20};
  NEED(rig.project().set_step(1, 0, 3, full) == model::Status::Ok);
  NEED(rig.project().set_step(1, 0, 5, full) == model::Status::Ok);
  rig.play(3);
  CHECK(rig.midicc(1, 78, 127) == DispatchResult::Changed);  // the parameter moved, the FX is dropped
  CHECK(std::memcmp(&rig.step(0, 3), &full, sizeof full) == 0);
  CHECK(rig.last_note().find("REC FX: NO FREE FX COLUMN") != std::string::npos);
  rig.play(5);
  CHECK(rig.midicc(1, 78, 0) == DispatchResult::Changed);
  CHECK(std::memcmp(&rig.step(0, 5), &full, sizeof full) == 0);
  CHECK(rig.last_note().find("REC FX: NO FREE FX COLUMN") != std::string::npos);  // still the one message (once per take)
  CHECK_FALSE(rig.state().edit.rec_fx_said);  // no FX was recorded, so no "REC FX: <PARAMETER>"
  // a free column on another row records, and the strip says the parameter then
  rig.play(7);
  CHECK(rig.midicc(1, 78, 127) == DispatchResult::Changed);
  CHECK(rig.step(0, 7).fx[0].type == seq::fx::kPanning);
  CHECK(rig.step(0, 7).fx[0].value == 100);  // the stored pan of the knob at 127
  CHECK(rig.last_note().find("REC FX: PANNING") != std::string::npos);
  rig.stop();  // the take ends: the flags go with it
  CHECK_FALSE(rig.state().edit.rec_fx_said);
  CHECK_FALSE(rig.state().edit.rec_fx_full);
}

TEST_CASE("fxrec app: the recorded FX leaves the step's note, instrument and volume alone") {
  Rig rig;
  rig.bind(0, ip::Panning, 78, core::LearnMode::Jump);
  model::Step with_note = model::kEmptyStep;
  with_note.note = 60;
  with_note.instr = 1;
  with_note.vol = 100;
  NEED(rig.project().set_step(1, 0, 3, with_note) == model::Status::Ok);
  rig.play(3);
  CHECK(rig.midicc(1, 78, 127) == DispatchResult::Changed);
  CHECK(rig.step(0, 3).note == 60);
  CHECK(rig.step(0, 3).instr == 1);
  CHECK(rig.step(0, 3).vol == 100);
  CHECK(rig.step(0, 3).fx[0].type == seq::fx::kPanning);
  CHECK(rig.step(0, 3).fx[0].value == 100);
  CHECK(rig.step(0, 3).fx[1].type == model::kFxNone);
}
