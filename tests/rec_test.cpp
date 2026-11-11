// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-LIVEREC (the maintainer's design, new Community OS behaviour; it replaced the placement of OS-FU2): the live recording of the Pattern screen. The pure rules (app/rec_place.h: the
// rounding of REC QUANTIZE, the tracks REC POLY Spill walks, the OFF of a release), then the actions on the Pattern screen with a document: always the cursor's track, the note over
// what the step holds (one undo unit), the selected instrument, follow (Play + Shift + Rec) against free (Play + Rec), left / right changing the recording track, Spill against Off,
// Nearest against Playing step, the release's OFF, and what went (track arming, the Select mode).
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/rec_place.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/midi/cc_in.h>
#include <o3os/model/editor.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/lane_take.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::DispatchResult;
using app::PatternMode;
using core::ActionArg;
namespace rec = o3os::app::rec;
namespace ds = o3os::model::device_setting;

// ---- what went: track arming is not a member of anything any more ------------------------------------------------------------------------------------------------

namespace {
template <class T, class = void>
struct HasArmed : std::false_type {};
template <class T>
struct HasArmed<T, std::void_t<decltype(std::declval<T&>().armed)>> : std::true_type {};
}  // namespace
static_assert(!HasArmed<app::PatternEdit>::value, "OS-LIVEREC: no track arming (PAR-047 / OS-009 P1d dropped)");
static_assert(!HasArmed<app::PatternView::Status>::value, "OS-LIVEREC: the header has no arm mark");

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  audio::PlayPosition pos;
  // Declared after the editor, so that it is destroyed before it: ~App closes the take's undo group on the editor (OS-LIVEREC).
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  explicit Rig(unsigned length = 16, bool edit = true, std::size_t ring_bytes = sizeof g_ring) {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, length);
    (void)p.append_order(1);
    journal.begin(g_ring, ring_bytes);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_play_position(&pos);
    app->run_once();
    if (edit) {
      live();
    }
  }
  // Edit following, LIVE (OS-RECMODE2), entered as the tests always did: through view.mode, with no transport (Shift + Rec from stopped starts one).
  void live() { (void)app->invoke(core::action_id("view.mode"), static_cast<ActionArg>(PatternMode::Edit)); }
  model::Project& project() { return test::arena(0).project; }
  const app::ViewState& state() const { return app->state(); }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  // The sequencer is at `step` of pattern 1, `ustep` of its 48 microsteps played (or stopped).
  void play(unsigned step, unsigned ustep = 0, bool playing = true) {
    pos.publish(playing, 1, step, 12000, audio::PlayPosition::kNoOrder, ustep);
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
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  const model::Step& step(int track, int row) { return project().pattern(1)->steps[track][row]; }
  unsigned note_of(unsigned pad) const { return app->state().edit.pads.note(pad); }
  unsigned depth() const { return journal.depth(); }
  int track() const { return app->view().track(); }
  int row() const { return app->view().row(); }
  void set(unsigned field, unsigned value) { CHECK(app->device_settings().set(field, static_cast<float>(value)) == model::Status::Ok); }
  void quantize_playing() { set(ds::RecQuantize, 1); }  // Playing step: the step the playhead is on
  void spill() { set(ds::RecPoly, 1); }
  // The cursor to a track, in the Note column.
  void go_track(int t) { (void)invoke("cursor.track", t - track()); }
  std::string message() { return app->message(); }
  void flush_messages() {  // lets every notification run out (the first pass starts the clock of the one just posted)
    pass(1);
    pass(10000);
  }
  // The keyboard: `bytes` are MIDI messages of DIN port 0 on channel 1 (a note on is 0x90, its off 0x80), handed to the app as the platform does.
  midi::CcInput keys;
  unsigned send(std::initializer_list<std::uint8_t> bytes) {
    for (const std::uint8_t b : bytes) {
      keys.feed(midi::RtItem{0, b, 0, 0});
    }
    return app->poll_midi_notes(keys);
  }
};

model::Step note_step(unsigned note, unsigned instr, unsigned vol = model::kVolumeUnset) {
  model::Step s = model::kEmptyStep;
  s.note = static_cast<std::uint8_t>(note);
  s.instr = static_cast<std::uint8_t>(instr);
  s.vol = static_cast<std::uint8_t>(vol);
  return s;
}

}  // namespace

// ---- the rules --------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("rec: REC QUANTIZE: Nearest rounds to the next step after half of it (24 of 48 microsteps), Playing never; the step after the pattern's last is the first, or the last again in a song") {
  using Q = rec::Quantize;
  for (int ustep = 0; ustep < 48; ++ustep) {
    CHECK(rec::quantize_row(5, ustep, 16, 16, Q::Playing, true) == 5);
    CHECK(rec::quantize_row(5, ustep, 16, 16, Q::Nearest, true) == (ustep < 24 ? 5 : 6));
    CHECK(rec::quantize_row(15, ustep, 16, 16, Q::Nearest, true) == (ustep < 24 ? 15 : 0));   // the pattern loops
    CHECK(rec::quantize_row(15, ustep, 16, 16, Q::Nearest, false) == 15);                      // a song leaves it there: the note stays on the last step
    CHECK(rec::quantize_row(15, ustep, 16, 16, Q::Playing, true) == 15);
    CHECK(rec::quantize_row(0, ustep, 1, 1, Q::Nearest, true) == 0);                           // a track of one step
    CHECK(rec::quantize_row(0, ustep, 1, 1, Q::Nearest, false) == 0);
    CHECK(rec::quantize_row(14, ustep, 16, 16, Q::Nearest, false) == (ustep < 24 ? 14 : 15));  // no wrap before the last step
  }
  CHECK(rec::quantize_row(63, 47, 64, 64, Q::Nearest, true) == 0);
  CHECK(rec::quantize_row(63, 47, 64, 64, Q::Nearest, false) == 63);
  CHECK(rec::quantize_row(62, 24, 64, 64, Q::Nearest, false) == 63);
  // a track shorter than the pattern plays at the step modulo its own length
  CHECK(rec::quantize_row(9, 0, 4, 16, Q::Playing, true) == 1);
  CHECK(rec::quantize_row(9, 30, 4, 16, Q::Nearest, true) == 2);
  CHECK(rec::quantize_row(11, 30, 4, 16, Q::Nearest, false) == 0);   // 12 % 4: the short track loops inside the pattern, in a song too
  CHECK(rec::quantize_row(15, 30, 4, 16, Q::Nearest, true) == 0);    // the pattern's own wrap
  CHECK(rec::quantize_row(15, 30, 4, 16, Q::Nearest, false) == 3);   // ... which a song does not make: 15 % 4
  CHECK(rec::quantize_row(127, 0, 128, 128, Q::Playing, false) == 127);
  CHECK(static_cast<unsigned>(rec::Poly::Off) == 0u);  // the descriptors' values (model/params.cpp): the defaults are 0
  CHECK(static_cast<unsigned>(rec::Poly::Spill) == 1u);
  CHECK(static_cast<unsigned>(rec::Quantize::Nearest) == 0u);
  CHECK(static_cast<unsigned>(rec::Quantize::Playing) == 1u);
}

TEST_CASE("rec: Spill walks on from the cursor's track to the first one whose note is not held; none left is -1") {
  CHECK(rec::next_free_track(0, 8, 0x00u) == 0);
  CHECK(rec::next_free_track(3, 8, 0x00u) == 3);
  CHECK(rec::next_free_track(3, 8, 0x08u) == 4);       // 3 is held
  CHECK(rec::next_free_track(3, 8, 0x38u) == 6);       // 3, 4 and 5 are
  CHECK(rec::next_free_track(0, 8, 0x01u) == 1);
  CHECK(rec::next_free_track(0, 8, 0xFFu) == -1);
  CHECK(rec::next_free_track(7, 8, 0x80u) == -1);      // the last track is held: no wrapping round to the first
  CHECK(rec::next_free_track(8, 8, 0x00u) == -1);      // the cursor is past the tracks (cannot happen: said anyway)
  CHECK(rec::next_free_track(0, 8, 0x0Fu) == 4);       // bits below the cursor do not matter
  CHECK(rec::next_free_track(2, 8, 0x03u) == 2);
}

TEST_CASE("rec: the release's OFF goes to the playhead, or to the step after the note when it is still in the step it was played in or on the note's own step; wrapping, except at a song's end") {
  //                        now head at length pattern wrap
  CHECK(rec::off_row(8, 5, 5, 16, 16, true) == 8);     // held a while: at the playhead
  CHECK(rec::off_row(5, 5, 5, 16, 16, true) == 6);     // released in the same step: the next
  CHECK(rec::off_row(15, 15, 15, 16, 16, true) == 0);  // ... which wraps
  CHECK(rec::off_row(15, 15, 15, 16, 16, false) == -1);  // a song's last step: the pattern ends the note, no OFF
  CHECK(rec::off_row(6, 5, 6, 16, 16, true) == 7);     // Nearest: the note went to 6, the playhead was in 5; it is on 6 now: after the note
  CHECK(rec::off_row(5, 5, 6, 16, 16, true) == 7);     // ... still in 5: after the note
  CHECK(rec::off_row(9, 5, 6, 16, 16, true) == 9);
  CHECK(rec::off_row(2, 14, 14, 16, 16, true) == 2);   // the playhead wrapped past the end: the pattern loops, the note is held across
  CHECK(rec::off_row(0, 15, 0, 16, 16, true) == 1);    // Nearest at the last step: the note wrapped to 0 and the playhead is there
  CHECK(rec::off_row(0, 0, 0, 1, 1, true) == 0);       // a track of one step: no step after (rec_note_off writes nothing)
  // a track shorter than the pattern loops inside it, in a song too
  CHECK(rec::off_row(3, 3, 3, 4, 16, false) == 0);
  CHECK(rec::off_row(3, 3, 3, 4, 16, true) == 0);
  CHECK(rec::off_row(1, 0, 0, 4, 16, true) == 1);
  model::Step s = note_step(40, 3, 20);
  s.fx[0] = {5, 6};
  rec::write_off(s);
  CHECK(s.note == model::kNoteOff);
  CHECK(s.instr == 3);  // nothing else of the step is touched
  CHECK(s.vol == 20);
  CHECK(s.fx[0].type == 5);
}

TEST_CASE("rec: a recorded note is the note and the instrument over the step; the volume and the FX stay unless REC MICRO / REC VELOCITY give theirs") {
  model::Step s = note_step(40, 3, 20);
  s.fx[0] = {5, 6};
  s.fx[1] = {7, 8};
  rec::write_note(s, 60, 9, -1, 0);
  CHECK(s.note == 61u);  // the model's 1..128
  CHECK(s.instr == 9u);
  CHECK(s.vol == 20u);
  CHECK(s.fx[0].type == 5);
  CHECK(s.fx[1].value == 8);
  rec::write_note(s, 200, 99, -1, 0);  // held to 127 and to the 64 slots
  CHECK(s.note == 128u);
  CHECK(s.instr == model::edit::kInstrumentSlots);
  rec::write_note(s, 0, 0, -1, 0);
  CHECK(s.instr == 1u);
}

// ---- OS-RECMICRO: REC MICRO, REC VELOCITY (PAR-037, PAR-064; new Community OS behaviour; the scaling of the microstep onto 0..100 is our own choice; what the official firmware writes is an open research question) -----------------------------------------------

TEST_CASE("rec micro: the Micro-move value of an offset is offset * 100 / 47; an early or zero offset has none") {
  CHECK(rec::micro_value(0) == 0u);   // on the step: no Micro-move (never an unconditional m02)
  CHECK(rec::micro_value(-1) == 0u);  // early: the FX has no negative, clamped
  CHECK(rec::micro_value(-24) == 0u);
  CHECK(rec::micro_value(1) == 2u);   // 100 / 47 = 2.1
  CHECK(rec::micro_value(10) == 21u);
  CHECK(rec::micro_value(24) == 51u);
  CHECK(rec::micro_value(47) == 100u);  // the last microstep: the top of the range 0..100 (model/fx_table.h)
  CHECK(rec::micro_value(500) == 100u);
  for (int u = 0; u < 48; ++u) {
    CHECK(rec::micro_value(u) == static_cast<unsigned>(model::edit::map_int(u + 1, 1, 48, 0, 100)));  // the source's expression, for every microstep
    CHECK(rec::micro_value(u) <= 100u);
  }
  // which offset: the playhead's microstep on the step it is written to, negative when the note was rounded up to the next
  using Q = rec::Quantize;
  CHECK_FALSE(rec::goes_on(5, 23, 16, Q::Nearest, true));
  CHECK(rec::goes_on(5, 24, 16, Q::Nearest, true));
  CHECK_FALSE(rec::goes_on(5, 40, 16, Q::Playing, true));
  CHECK_FALSE(rec::goes_on(15, 40, 16, Q::Nearest, false));  // a song's last step has no next: the note stays and is late for it
  CHECK(rec::goes_on(15, 40, 16, Q::Nearest, true));
  CHECK(rec::micro_offset(10, false) == 10);
  CHECK(rec::micro_offset(30, true) == -18);
  // the truth table: [Playing | Nearest] x ustep -> the value
  for (int ustep = 0; ustep < 48; ++ustep) {
    const unsigned playing = rec::micro_value(rec::micro_offset(ustep, rec::goes_on(5, ustep, 16, Q::Playing, true)));
    const unsigned nearest = rec::micro_value(rec::micro_offset(ustep, rec::goes_on(5, ustep, 16, Q::Nearest, true)));
    CHECK(playing == rec::micro_value(ustep));
    CHECK(nearest == (ustep < 24 ? rec::micro_value(ustep) : 0u));
  }
}

TEST_CASE("rec micro: write_note puts the value in FX 2 only when it is free; REC MICRO off leaves FX 2 alone; velocity goes to the volume column") {
  model::Step s = model::kEmptyStep;
  rec::write_note(s, 60, 1, 21, 0);
  CHECK(s.fx[1].type == rec::kFxMicroMove);
  CHECK(s.fx[1].value == 21);
  CHECK(s.fx[0].type == 0);  // FX 1 is never touched
  CHECK(s.vol == model::kVolumeUnset);
  rec::write_note(s, 62, 1, 0, 0);  // a note on the step: the earlier take's Micro-move does not belong to it
  CHECK(s.fx[1].type == 0);
  CHECK(s.fx[1].value == 0);
  rec::write_note(s, 62, 1, 0, 0);  // nothing to clear, nothing written (no m02 00)
  CHECK(s.fx[1].type == 0);
  model::Step u = model::kEmptyStep;
  u.fx[1] = {7, 9};  // a user FX
  rec::write_note(u, 60, 1, 33, 0);
  CHECK(u.fx[1].type == 7);
  CHECK(u.fx[1].value == 9);
  rec::write_note(u, 60, 1, 0, 0);  // ... which a zero offset does not clear either
  CHECK(u.fx[1].type == 7);
  model::Step off = model::kEmptyStep;
  off.fx[1] = {rec::kFxMicroMove, 40};
  rec::write_note(off, 60, 1, -1, 0);  // REC MICRO off: whatever FX 2 holds stays
  CHECK(off.fx[1].value == 40);
  model::Step big = model::kEmptyStep;
  rec::write_note(big, 60, 1, 400, 0);
  CHECK(big.fx[1].value == 100);
  // REC VELOCITY: 1..127 is the volume column's own scale
  model::Step v = model::kEmptyStep;
  rec::write_note(v, 60, 1, -1, 100);
  CHECK(v.vol == 100u);
  rec::write_note(v, 60, 1, -1, 0);  // none: the volume stays
  CHECK(v.vol == 100u);
  rec::write_note(v, 60, 1, -1, 1);
  CHECK(v.vol == 1u);
  rec::write_note(v, 60, 1, -1, 127);
  CHECK(v.vol == 127u);
  rec::write_note(v, 60, 1, -1, 200);  // held to the column's top
  CHECK(v.vol == 127u);
}

TEST_CASE("rec micro: the two device settings are appended rows 20 and 21, Off by default, two choices each") {
  CHECK(ds::RecMicro == 20u);
  CHECK(ds::RecVelocity == 21u);
  CHECK(ds::kCount == 32u);  // 31 before OS-MIDIREC-P1 S3 appended REC STORE, 30 before OS-LEDBPM appended STATUS LED, 29 before OS-MTC B3 appended MTC IN, 27 before OS-MTC A3 appended MTC OUT and MTC START, 26 before OS-MIDICCMAP S1 appended CC MAP
  model::Settings d;
  d.init(core::ParamKind::Device);
  CHECK(d.get(ds::RecMicro) == 0.0f);
  CHECK(d.get(ds::RecVelocity) == 0.0f);
  CHECK(d.set(ds::RecMicro, 2.0f) == model::Status::BadValue);
  CHECK(d.set(ds::RecVelocity, 2.0f) == model::Status::BadValue);
  CHECK(d.set(ds::RecMicro, 1.0f) == model::Status::Ok);
  CHECK(d.set(ds::RecVelocity, 1.0f) == model::Status::Ok);
  CHECK(d.check_invariants());
}

TEST_CASE("rec: the two device settings are appended rows whose defaults are Off and Nearest step") {
  CHECK(ds::RecPoly == 14u);
  CHECK(ds::RecQuantize == 15u);
  CHECK(ds::kCount == 32u);  // 31 before OS-MIDIREC-P1 S3 appended REC STORE, 30 before OS-LEDBPM appended STATUS LED, 29 before OS-MTC B3 appended MTC IN, 27 before OS-MTC A3 appended MTC OUT and MTC START, 26 before OS-MIDICCMAP S1 appended CC MAP (23 until E-CFG-03 appended TRANSPORT IN and OUT; 18 until OS-METRO appended two rows, 20 until OS-RECMICRO, 22 until OS-PADOFFSET did)
  model::Settings d;
  d.init(core::ParamKind::Device);
  CHECK(d.get(ds::RecPoly) == 0.0f);
  CHECK(d.get(ds::RecQuantize) == 0.0f);
  CHECK(d.set(ds::RecPoly, 2.0f) == model::Status::BadValue);  // two choices
  CHECK(d.set(ds::RecQuantize, 2.0f) == model::Status::BadValue);
  CHECK(d.set(ds::RecPoly, 1.0f) == model::Status::Ok);
  CHECK(d.check_invariants());
}

// ---- the actions: the track and the instrument -------------------------------------------------------------------------------------------------------------------

TEST_CASE("rec: playing in Edit a pad writes its note at the playhead's step of the CURSOR's track, over what the step holds, with the selected instrument; no search for an empty step") {
  Rig rig;
  rig.go_track(3);
  (void)rig.invoke("instrument.select", 7);
  rig.project().pattern(1);  // (the pattern exists: Rig made it)
  model::Step old = note_step(40, 2, 55);
  old.fx[0] = {5, 6};
  NEED(rig.project().set_step(1, 3, 5, old) == model::Status::Ok);
  NEED(rig.project().set_step(1, 4, 5, note_step(41, 2)) == model::Status::Ok);
  rig.play(5);
  const unsigned note = rig.note_of(12);
  const unsigned d0 = rig.depth();
  rig.pad_down(12);
  CHECK(rig.step(3, 5).note == note + 1u);    // over the old note, though the step was taken, and not on another track
  CHECK(rig.step(3, 5).instr == 7u);          // the selected instrument, not the one the old step held
  CHECK(rig.step(3, 5).vol == 55u);           // the rest of the step stays
  CHECK(rig.step(3, 5).fx[0].type == 5);
  CHECK(rig.step(4, 5).note == 41u);          // the next track is not touched
  CHECK(rig.step(0, 5).note == 0u);           // nor the first, which is free: the old search is gone
  CHECK(rig.depth() == d0);                   // the take is still open: its unit is written when it ends
  rig.pad_up(12);
  CHECK(rig.step(3, 6).note == model::kNoteOff);
  (void)rig.invoke("edit.undo");              // the whole take: the note and its OFF, and the old step is back
  CHECK(std::memcmp(&rig.step(3, 5), &old, sizeof old) == 0);
  CHECK(rig.step(3, 6).note == 0u);
  CHECK(rig.depth() == d0);
}

TEST_CASE("rec: the selected instrument is the strip's: choosing another in the Instr column changes the next note, and a recorded note does not move it") {
  Rig rig;
  (void)rig.invoke("instrument.select", 12);
  rig.play(2);
  rig.pad_down(14);
  CHECK(rig.step(0, 2).instr == 12u);
  CHECK(rig.state().instrument == 12);
  rig.pad_up(14);
  (void)rig.invoke("instrument.select", 3);
  rig.play(8);
  rig.pad_down(14);
  CHECK(rig.step(0, 8).instr == 3u);
  CHECK(rig.step(0, 2).instr == 12u);
}

TEST_CASE("rec: stopped, a pad and a key enter the cursor's step as before; the notes held when the sequencer stops are let go without an OFF") {
  Rig rig;
  const unsigned note = rig.note_of(12);
  rig.pad_down(12);  // stopped (the position was never published as playing)
  rig.pad_up(12);
  CHECK(rig.step(0, 0).note == note + 1u);  // the cursor's step (step entry)
  CHECK(rig.state().edit.rec_open == 0u);
  rig.play(5);
  rig.pad_down(14);
  CHECK(rig.state().edit.rec_open != 0u);
  rig.stop();  // the transport stops while the pad is held
  CHECK(rig.state().edit.rec_open == 0u);
  rig.pad_up(14);
  for (unsigned r = 5; r < 9; ++r) {
    CHECK(rig.step(0, r).note != model::kNoteOff);  // no OFF anywhere
  }
}

TEST_CASE("rec: Browse (no Edit) plays the pads and records nothing") {
  Rig rig(16, false);
  rig.play(5);
  rig.pad_down(12);
  rig.pad_up(12);
  for (unsigned t = 0; t < 12; ++t) {
    CHECK(rig.step(t, 5).note == 0u);
  }
}

TEST_CASE("rec: the keyboard's notes are recorded at the playhead too, and its release ends them") {
  Rig rig;
  rig.play(3);
  CHECK(rig.invoke("step.midi_note", 62) == DispatchResult::Changed);
  CHECK(rig.step(0, 3).note == 63u);
  rig.play(7);
  (void)rig.invoke("rec.note_off", 99);  // a key that was never recorded: nothing
  CHECK(rig.state().edit.rec_open == 1u);
  (void)rig.invoke("rec.note_off", 62);
  CHECK(rig.step(0, 7).note == model::kNoteOff);
  CHECK(rig.state().edit.rec_open == 0u);
}

// ---- REC POLY ---------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("rec: REC POLY Off (the default) keeps a chord on the current track: the last note of a step wins and its release writes the OFF") {
  Rig rig;
  rig.go_track(2);
  rig.play(5);
  rig.pad_down(12);
  rig.pad_down(13);
  rig.pad_down(14);
  CHECK(rig.step(2, 5).note == rig.note_of(14) + 1u);
  CHECK(rig.step(3, 5).note == 0u);
  CHECK(rig.step(1, 5).note == 0u);
  CHECK(rig.state().edit.rec_open == (1u << 2));
  rig.play(7);
  rig.pad_up(12);  // the first two were written over: no track holds them
  rig.pad_up(13);
  CHECK(rig.step(2, 7).note == 0u);
  rig.pad_up(14);
  CHECK(rig.step(2, 7).note == model::kNoteOff);
  CHECK(rig.state().edit.rec_open == 0u);
}

TEST_CASE("rec: REC POLY Spill puts the notes held together on the current track, then the next ones, one a voice; a released track is the current one again") {
  Rig rig;
  rig.spill();
  rig.go_track(2);
  rig.play(5);
  rig.pad_down(12);
  rig.pad_down(13);
  rig.pad_down(14);
  CHECK(rig.step(2, 5).note == rig.note_of(12) + 1u);
  CHECK(rig.step(3, 5).note == rig.note_of(13) + 1u);
  CHECK(rig.step(4, 5).note == rig.note_of(14) + 1u);
  CHECK(rig.step(1, 5).note == 0u);  // never below the cursor's
  CHECK(rig.state().edit.rec_open == ((1u << 2) | (1u << 3) | (1u << 4)));
  rig.play(7);
  rig.pad_up(13);  // track 3 is free again, the others stay held
  CHECK(rig.step(3, 7).note == model::kNoteOff);
  CHECK(rig.state().edit.rec_open == ((1u << 2) | (1u << 4)));
  rig.play(8);
  rig.pad_down(15);  // the current track is held: the first free one from it up is track 3
  CHECK(rig.step(3, 8).note == rig.note_of(15) + 1u);
  rig.pad_up(12);
  rig.pad_up(14);
  rig.pad_up(15);
  CHECK(rig.state().edit.rec_open == 0u);
  rig.play(10);
  rig.pad_down(12);  // everything let go: the current track again
  CHECK(rig.step(2, 10).note == rig.note_of(12) + 1u);
}

TEST_CASE("rec: Spill past the last track writes nothing and says so") {
  Rig rig;
  rig.spill();
  rig.go_track(99);  // the last track
  const int last = rig.track();
  NEED(last >= 1);
  rig.play(5);
  rig.pad_down(12);
  CHECK(rig.step(last, 5).note != 0u);
  rig.pad_down(13);
  CHECK(rig.message() == "NO FREE TRACK TO SPILL TO");
  CHECK(rig.state().edit.rec_open == (1u << last));  // the second note is nowhere
  rig.pad_up(12);
  rig.pad_up(13);
}

TEST_CASE("rec: a track shorter than the pattern plays at the step modulo its length, and the note, Spill and the OFF follow it") {
  {
    Rig rig;
    NEED(rig.project().set_track_length(1, 1, 4) == model::Status::Ok);
    rig.quantize_playing();
    rig.go_track(1);
    rig.play(9);  // 9 % 4 = 1
    rig.pad_down(14);
    CHECK(rig.step(1, 1).note == rig.note_of(14) + 1u);
    rig.play(11);  // 11 % 4 = 3
    rig.pad_up(14);
    CHECK(rig.step(1, 3).note == model::kNoteOff);
    CHECK(rig.state().edit.rec_open == 0u);
    for (unsigned r = 4; r < 16; ++r) {
      CHECK(rig.step(1, r).note == 0u);  // nothing past the track's end
    }
  }
  {
    Rig rig;  // released within the step it started in: the next row of the short track, wrapping inside it
    NEED(rig.project().set_track_length(1, 1, 4) == model::Status::Ok);
    rig.quantize_playing();
    rig.go_track(1);
    rig.play(11, 3);
    rig.pad_down(14);
    CHECK(rig.step(1, 3).note != 0u);
    rig.play(11, 40);
    rig.pad_up(14);
    CHECK(rig.step(1, 0).note == model::kNoteOff);
  }
  {
    Rig rig;  // Nearest: 11 late is 12, which is row 0 of the short track
    NEED(rig.project().set_track_length(1, 1, 4) == model::Status::Ok);
    rig.go_track(1);
    rig.play(11, 30);
    rig.pad_down(14);
    CHECK(rig.step(1, 0).note != 0u);
  }
  {
    Rig rig;  // Spill: no track is passed over for being short
    NEED(rig.project().set_track_length(1, 1, 4) == model::Status::Ok);
    rig.quantize_playing();
    rig.spill();
    rig.go_track(1);
    rig.play(9);
    rig.pad_down(14);
    rig.pad_down(15);
    CHECK(rig.step(1, 1).note == rig.note_of(14) + 1u);
    CHECK(rig.step(2, 9).note == rig.note_of(15) + 1u);
    CHECK(rig.state().edit.rec_open == ((1u << 1) | (1u << 2)));
  }
}

// ---- LIVE and STEP (OS-RECMODE2; the table of every transition is recmode_test.cpp) ---------------------------------------------------------------------------------

TEST_CASE("rec: Shift + Rec while playing is LIVE (OS-RECMODE2): the cursor rides the playhead and the view keeps the playing row centred; the note is written where the cursor is") {
  Rig rig(64, false);
  rig.play(19);
  rig.shift_tap(Button::Rec);  // WATCH -> LIVE
  CHECK(rig.state().mode[app::screen_index(app::ScreenId::Pattern)] == static_cast<unsigned>(PatternMode::Edit));
  rig.play(20);
  CHECK(rig.row() == 20);
  CHECK(rig.app->view().top_row() == app::PatternView::window_top(20));  // OS-CENTER
  rig.play(21);
  CHECK(rig.row() == 21);
  CHECK(rig.app->view().top_row() == app::PatternView::window_top(21));
  rig.quantize_playing();
  rig.pad_down(12);
  CHECK(rig.step(0, rig.row()).note == rig.note_of(12) + 1u);  // where the cursor is
  CHECK(rig.step(0, 21).note == rig.note_of(12) + 1u);
  CHECK_FALSE(rig.state().edit.rec_free);
}

TEST_CASE("rec: while it records Left / Right change the recording track, a track a press and the column stays; stopped they walk the columns as before") {
  Rig rig(16, false);
  rig.tap(Button::Rec);  // STEP
  rig.tap(Button::Right);  // stopped: the next column
  CHECK(rig.track() == 0);
  CHECK(rig.app->view().column() == app::Column::Instr);
  rig.tap(Button::Left);
  rig.play(4);
  rig.shift_tap(Button::Rec);  // STEP -> LIVE
  rig.tap(Button::Right);
  CHECK(rig.track() == 1);
  CHECK(rig.app->view().column() == app::Column::Note);
  rig.tap(Button::Right);
  CHECK(rig.track() == 2);
  rig.pad_down(12);
  CHECK(rig.step(2, 4).note != 0u);  // the new recording track
  rig.pad_up(12);
  rig.tap(Button::Left);
  CHECK(rig.track() == 1);
  rig.pad_down(13);
  CHECK(rig.step(1, 4).note != 0u);
  rig.pad_up(13);
  rig.tap(Button::Left);
  rig.tap(Button::Left);  // the first track is the end
  CHECK(rig.track() == 0);
}

TEST_CASE("rec: STEP while playing (OS-RECMODE2): the playhead does not take the cursor or the view, a pad enters its note at the cursor and the cursor moves on by JUMP; no take is open") {
  Rig rig(64, false);
  rig.play(10);
  rig.tap(Button::Rec);  // WATCH -> STEP: the cursor stops on the playhead's row
  CHECK(rig.state().edit.rec_free);
  CHECK(rig.row() == 10);
  const int top = rig.app->view().top_row();
  rig.play(30);
  CHECK(rig.row() == 10);  // it stays
  CHECK(rig.app->view().top_row() == top);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.row() == 12);  // the user moves freely
  rig.tap(Button::Right);  // Left / Right walk the columns
  CHECK(rig.track() == 0);
  rig.tap(Button::Left);
  rig.play(31);
  const unsigned jump = rig.state().edit.edit_step;
  rig.pad_down(12);
  CHECK(rig.step(0, 12).note == rig.note_of(12) + 1u);  // at the cursor, not at the playhead
  CHECK(rig.step(0, 31).note == 0u);
  CHECK(rig.row() == 12 + static_cast<int>(jump));
  CHECK_FALSE(rig.state().edit.take_open);
  rig.pad_up(12);
  CHECK(rig.state().edit.rec_open == 0u);
  // the keyboard: the cursor step as well (and the cursor stays)
  const int row = rig.row();
  CHECK(rig.invoke("step.midi_note", 62) == DispatchResult::Changed);
  CHECK(rig.step(0, row).note == 63u);
  CHECK(rig.step(0, 31).note == 0u);
}

TEST_CASE("rec: Browse still follows the playhead and does not record") {
  Rig rig(64, false);
  rig.play(25);
  CHECK(rig.row() == 25);
  rig.pad_down(12);
  rig.pad_up(12);
  CHECK(rig.step(0, 25).note == 0u);
}

// ---- REC QUANTIZE ---------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("rec: Nearest step (the default) writes the next step once more than half of this one has gone; Playing step writes the step the playhead is on") {
  Rig rig;
  rig.play(5, 10);
  rig.pad_down(12);
  CHECK(rig.step(0, 5).note != 0u);
  rig.pad_up(12);
  rig.play(8, 23);
  rig.pad_down(12);
  CHECK(rig.step(0, 8).note != 0u);
  rig.pad_up(12);
  rig.play(10, 24);
  rig.pad_down(12);
  CHECK(rig.step(0, 10).note == 0u);
  CHECK(rig.step(0, 11).note != 0u);
  rig.pad_up(12);
  rig.play(15, 40);  // the step after the last is the first
  rig.pad_down(12);
  CHECK(rig.step(0, 0).note != 0u);
  rig.pad_up(12);
  rig.quantize_playing();
  rig.play(3, 47);
  rig.pad_down(12);
  CHECK(rig.step(0, 3).note != 0u);
  CHECK(rig.step(0, 4).note == 0u);
}

TEST_CASE("rec: the release's OFF is at the playhead on the same track, the next step if it is released within the step it started; Nearest counts the step the note went to") {
  {
    Rig rig;
    rig.quantize_playing();
    rig.play(5, 3);
    rig.pad_down(12);
    rig.play(5, 40);
    rig.pad_up(12);  // within the step it started
    CHECK(rig.step(0, 6).note == model::kNoteOff);
    CHECK(rig.step(0, 5).note != model::kNoteOff);
  }
  {
    Rig rig;
    rig.quantize_playing();
    rig.play(5, 3);
    rig.pad_down(12);
    rig.play(9, 0);
    rig.pad_up(12);  // held a while
    CHECK(rig.step(0, 9).note == model::kNoteOff);
    CHECK(rig.step(0, 6).note == 0u);
  }
  {
    Rig rig;  // Nearest: pressed late in step 5, the note is on step 6
    rig.play(5, 30);
    rig.pad_down(12);
    CHECK(rig.step(0, 6).note != 0u);
    rig.play(5, 45);  // still in the step it started: after the note
    rig.pad_up(12);
    CHECK(rig.step(0, 7).note == model::kNoteOff);
  }
  {
    Rig rig;
    rig.play(5, 30);
    rig.pad_down(12);
    rig.play(6, 2);  // the playhead is on the note's own step: after it
    rig.pad_up(12);
    CHECK(rig.step(0, 7).note == model::kNoteOff);
    CHECK(rig.step(0, 6).note != model::kNoteOff);
  }
  {
    Rig rig;
    rig.play(5, 30);
    rig.pad_down(12);
    rig.play(11, 0);
    rig.pad_up(12);
    CHECK(rig.step(0, 11).note == model::kNoteOff);
  }
  {
    Rig rig;  // the last step: the OFF wraps to the first
    rig.quantize_playing();
    rig.play(15, 3);
    rig.pad_down(12);
    rig.pad_up(12);
    CHECK(rig.step(0, 0).note == model::kNoteOff);
  }
  {
    Rig rig;  // the OFF goes to the track that holds the note, not to the cursor's by then
    rig.quantize_playing();
    rig.go_track(1);
    rig.play(4, 0);
    rig.pad_down(12);
    rig.go_track(5);
    rig.play(8, 0);
    rig.pad_up(12);
    CHECK(rig.step(1, 8).note == model::kNoteOff);
    CHECK(rig.step(5, 8).note == 0u);
  }
}

TEST_CASE("rec: an OFF never overwrites a note or an OFF: that one ends the held note itself; one step tracks write no OFF") {
  Rig rig;
  rig.quantize_playing();
  NEED(rig.project().set_step(1, 0, 9, note_step(50, 4)) == model::Status::Ok);
  rig.play(5);
  rig.pad_down(12);
  rig.play(9);
  rig.pad_up(12);
  CHECK(rig.step(0, 9).note == 50u);  // not turned into an OFF
  CHECK(rig.state().edit.rec_open == 0u);
  for (unsigned r = 0; r < 16; ++r) {
    CHECK(rig.step(0, r).note != model::kNoteOff);
  }
  rig.play(2);
  rig.pad_down(13);
  rig.play(2, 40);
  rig.pad_up(13);                    // released within its step: the OFF goes to the next
  CHECK(rig.step(0, 3).note == model::kNoteOff);
  rig.pad_down(14);                  // another note on step 2 ...
  rig.pad_up(14);                    // ... whose next step holds an OFF already: it stays one
  CHECK(rig.step(0, 2).note == rig.note_of(14) + 1u);
  CHECK(rig.step(0, 3).note == model::kNoteOff);
  rig.stop();
  // a track of one step: the OFF would be the note's own step
  Rig one;
  NEED(one.project().set_track_length(1, 4, 1) == model::Status::Ok);
  one.quantize_playing();
  one.go_track(4);
  one.play(0);
  one.pad_down(14);
  one.play(3);
  one.pad_up(14);
  CHECK(one.step(4, 0).note == one.note_of(14) + 1u);
}

// ---- the take: one undo unit ------------------------------------------------------------------------------------------------------------------------------------

namespace {
// The pattern as it is: a copy of the model's own bytes, to compare a whole pattern before and after.
struct Snapshot {
  model::Pattern p;
  explicit Snapshot(const model::Project& proj) { std::memcpy(&p, proj.pattern(1), sizeof p); }
  bool same_as(const model::Project& proj) const { return std::memcmp(&p, proj.pattern(1), sizeof p) == 0; }
};
}  // namespace

TEST_CASE("rec: the take is ONE undo unit: every note and OFF from Rec to the stop, one Undo takes them all, one Redo gives them back") {
  Rig rig;
  rig.quantize_playing();
  NEED(rig.project().set_step(1, 0, 3, note_step(70, 2, 33)) == model::Status::Ok);  // (before the take: a change behind the editor's back would end its history)
  rig.editor->history_reset();                                                       // the project changed behind the editor: take it as it is
  const Snapshot start(rig.project());
  const unsigned d0 = rig.depth();
  rig.play(1);
  // a take with notes, chords and OFFs on several tracks, over a step that held something
  rig.play(2);
  rig.pad_down(12);
  rig.pad_down(13);
  rig.play(3);
  rig.pad_up(12);
  rig.pad_up(13);
  rig.play(5);
  rig.pad_down(14);
  rig.play(8);
  rig.pad_up(14);
  rig.go_track(2);
  rig.play(9);
  (void)rig.send({0x90, 62, 100});
  rig.play(12);
  (void)rig.send({0x80, 62, 0});
  CHECK(rig.depth() == d0);  // the take is still open: its unit is written when it ends
  const Snapshot recorded(rig.project());
  CHECK_FALSE(start.same_as(rig.project()));
  rig.stop();                // the take ends
  CHECK(rig.depth() == d0 + 1u);
  CHECK(rig.editor->can_undo());
  (void)rig.invoke("edit.undo");  // everything recorded, in one
  CHECK(rig.depth() == d0);
  CHECK(start.same_as(rig.project()));
  CHECK(rig.editor->can_redo());
  (void)rig.invoke("edit.redo");  // and all of it again
  CHECK(rig.depth() == d0 + 1u);
  CHECK(recorded.same_as(rig.project()));
  (void)rig.invoke("edit.undo");
  CHECK(start.same_as(rig.project()));
}

TEST_CASE("rec: the take ends with Rec off, with the stop and with another screen; the next recording is a take of its own") {
  Rig rig;
  rig.quantize_playing();
  const unsigned d0 = rig.depth();
  rig.play(2);
  rig.pad_down(12);
  rig.pad_up(12);
  rig.tap(Button::Rec);  // LIVE -> WATCH: recording stops (OS-LIVEEXIT)
  CHECK(rig.depth() == d0 + 1u);
  CHECK_FALSE(rig.state().edit.take_open);
  rig.shift_tap(Button::Rec);  // WATCH -> LIVE, still playing: a new take
  CHECK(rig.state().edit.take_open);
  rig.pad_down(13);
  rig.pad_up(13);
  (void)rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Song));  // another screen: recording stops
  CHECK(rig.depth() == d0 + 2u);
  CHECK_FALSE(rig.state().edit.take_open);
  (void)rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Pattern));
  CHECK(rig.state().edit.take_open);  // back in Edit, playing: recording again
  rig.play(6);
  rig.pad_down(14);
  rig.pad_up(14);
  rig.stop();
  CHECK(rig.depth() == d0 + 3u);
  // three takes, three Undos, an empty pattern again
  (void)rig.invoke("edit.undo");
  (void)rig.invoke("edit.undo");
  (void)rig.invoke("edit.undo");
  CHECK(rig.depth() == d0);
  for (unsigned t = 0; t < 12; ++t) {
    for (unsigned r = 0; r < 16; ++r) {
      CHECK(rig.step(t, r).note == 0u);
    }
  }
}

namespace {
// A transport that takes every request and says nothing: the PATS pads queue a pattern behind the one that plays through it.
struct QueueSink final : app::TransportSink {
  unsigned queued = 0;
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
  bool queue_pattern(unsigned id) noexcept override {
    queued = id;
    return true;
  }
};
}  // namespace

TEST_CASE("rec: the display recovery destroys the App and builds another on the same editor: a take open at that moment does not leave Undo busy") {
  Rig rig;
  rig.quantize_playing();
  rig.play(4);
  rig.pad_down(12);
  rig.pad_up(12);
  NEED(rig.state().edit.take_open);
  CHECK(rig.editor->group_depth() == 1u);  // the take's group is open on the editor
  rig.app.reset();                         // app_main.cpp: g_app->~App(), then a new one on the surviving g_editor
  CHECK(rig.editor->group_depth() == 0u);  // the destructor closed it: the take is a unit
  CHECK(rig.depth() == 1u);
  rig.app = std::make_unique<app::App>(rig.input, rig.clock, rig.display);
  rig.app->attach_document(rig.editor.get());
  rig.app->attach_play_position(&rig.pos);
  rig.app->run_once();
  CHECK(rig.editor->undo() == model::UndoResult::Ok);  // not Busy
  CHECK(rig.depth() == 0u);
  CHECK(rig.step(0, 4).note == 0u);
  // and through the action of the new App, the sequencer still playing
  rig.pass();
  CHECK_FALSE(rig.state().edit.take_open);  // (the new App starts in Browse: not recording)
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // "NOTHING TO UNDO" is a message, not Busy; redo is there
  CHECK(rig.editor->can_redo());
}

TEST_CASE("rec: a take that grows to half the journal is closed and a new one begun, with a notice; the history is not lost") {
  Rig rig(16, true, 4096);  // a small ring: half of it is about 70 step records
  rig.quantize_playing();
  NEED(rig.invoke("step.note", 61) == DispatchResult::Changed);  // a unit before the take
  rig.play(3);
  for (unsigned i = 0; i < 400; ++i) {
    rig.pos.publish(true, 1, i % 16, 12000, audio::PlayPosition::kNoOrder, 0);
    rig.pass();
    CHECK(rig.invoke("step.midi_note", static_cast<ActionArg>(30 + i % 90)) == DispatchResult::Changed);
  }
  CHECK(rig.message() == "UNDO: LONG TAKE IS SPLIT");
  CHECK(rig.journal.lost_units() == 0u);  // never too long for the ring: nothing was lost whole
  CHECK(rig.depth() >= 1u);               // the parts that were closed are units
  CHECK(rig.journal.open_bytes() <= rig.journal.capacity_bytes() / 2 + 64);
  rig.stop();
  const unsigned parts = rig.depth();
  CHECK(parts >= 2u);
  // every part can be undone, down to the history that is left
  unsigned undone = 0;
  while (rig.invoke("edit.undo") == DispatchResult::Changed && rig.editor->can_redo() && undone < 1000) {
    ++undone;
    if (rig.depth() == 0u) {
      break;
    }
  }
  CHECK(rig.depth() == 0u);
  CHECK(rig.journal.lost_units() == 0u);
}

TEST_CASE("rec: the notice for a long take comes once for a recording, and again for the next one") {
  Rig rig(16, true, 4096);
  rig.quantize_playing();
  const auto burst = [&] {
    for (unsigned i = 0; i < 300; ++i) {
      rig.pos.publish(true, 1, i % 16, 12000, audio::PlayPosition::kNoOrder, 0);
      rig.pass();
      (void)rig.invoke("step.midi_note", static_cast<ActionArg>(30 + i % 90));
    }
  };
  rig.play(0);
  burst();
  CHECK(rig.message() == "UNDO: LONG TAKE IS SPLIT");
  CHECK(rig.state().edit.take_noticed);
  rig.flush_messages();
  CHECK(rig.message() != "UNDO: LONG TAKE IS SPLIT");
  rig.pos.publish(true, 1, 0, 12000, audio::PlayPosition::kNoOrder, 0);  // (splits go on without another notice while it records)
  rig.pass();
  burst();
  CHECK(rig.message() != "UNDO: LONG TAKE IS SPLIT");
  rig.stop();
  CHECK_FALSE(rig.state().edit.take_noticed);
  rig.live();  // (the stop ended LIVE: OS-RECMODE2)
  rig.play(0);
  burst();
  CHECK(rig.message() == "UNDO: LONG TAKE IS SPLIT");
}

TEST_CASE("rec: the take counts as an undo level only for a write that changed something, and not inside a macro") {
  Rig rig;
  rig.quantize_playing();
  rig.play(5);
  rig.pad_down(12);
  rig.pad_up(12);
  CHECK(rig.state().edit.take_wrote);
  rig.stop();
  CHECK(rig.depth() == 1u);
  // the same note again over the same step: nothing is written, the take is empty, there is no entry
  rig.play(5);
  CHECK_FALSE(rig.state().edit.take_wrote);
  rig.pad_down(12);
  CHECK_FALSE(rig.state().edit.take_wrote);  // (the step already holds this note and instrument)
  rig.pad_up(12);
  CHECK_FALSE(rig.state().edit.take_wrote);  // (and the step after it holds the OFF)
  rig.stop();
  CHECK(rig.depth() == 1u);
  // inside a macro the take is closed: what it writes is the macro's unit, not a level of a take
  rig.shift_tap(Button::Rec);
  NEED(rig.invoke("macro.record") != DispatchResult::Disabled);
  (void)rig.invoke("track.mute", 4);
  (void)rig.invoke("macro.record");
  (void)rig.invoke("edit.undo");
  const unsigned d0 = rig.depth();
  rig.play(9);
  CHECK(rig.invoke("macro.play") == DispatchResult::Changed);
  CHECK_FALSE(rig.state().edit.take_wrote);
  rig.stop();
  CHECK(rig.depth() == d0 + 1u);  // the macro; the take after it was empty
}

TEST_CASE("rec: with a pattern queued behind this one the pattern's last step does not wrap to row 0 either, as in a song") {
  Rig rig;
  QueueSink sink;
  rig.app->attach_transport(&sink);
  rig.play(15, 30);
  (void)rig.invoke("pads.mode", 5);            // PATS
  CHECK(rig.invoke("pads.play", 1) == DispatchResult::Changed);  // queue pattern 2
  NEED(rig.state().edit.queued_pattern == 2u);
  (void)rig.invoke("pads.mode", 0);            // NOTES again
  rig.pad_down(12);
  CHECK(rig.step(0, 15).note == rig.note_of(12) + 1u);  // the last step, not row 0 of the pattern that is being left
  CHECK(rig.step(0, 0).note == 0u);
  rig.pad_up(12);                                          // released in the same step: the OFF would be in the next pattern
  CHECK(rig.step(0, 0).note == 0u);
  CHECK(rig.state().edit.rec_open == 0u);
  // the sequencer started it: the queue is empty, and the pattern loops again
  rig.pos.publish(true, 2, 0, 12000, audio::PlayPosition::kNoOrder, 0);
  rig.pass();
  CHECK(rig.state().edit.queued_pattern == 0u);
  rig.pos.publish(true, 1, 15, 12000, audio::PlayPosition::kNoOrder, 30);
  rig.pass();
  rig.pad_down(13);
  CHECK(rig.step(0, 0).note == rig.note_of(13) + 1u);
  // a song's queued jump is the same
  Rig song;
  song.pos.publish(true, 1, 15, 12000, audio::PlayPosition::kNoOrder, 30);
  song.pass();
  song.pad_down(12);
  CHECK(song.step(0, 0).note == song.note_of(12) + 1u);  // (no queue: it loops)
}

TEST_CASE("rec: the info line counts the take in progress as an undo level as soon as it holds a note") {
  Rig rig;
  rig.quantize_playing();
  rig.play(2);
  CHECK_FALSE(rig.state().edit.take_wrote);  // an empty take is no level
  rig.pad_down(12);
  CHECK(rig.state().edit.take_wrote);
  CHECK(rig.depth() == 0u);                  // (the journal itself writes the unit when the take ends)
  rig.pad_up(12);
  rig.stop();
  CHECK_FALSE(rig.state().edit.take_wrote);
  CHECK(rig.depth() == 1u);
}

TEST_CASE("rec: a take with nothing recorded leaves no undo entry and does not end the redo history") {
  Rig rig;
  const unsigned d0 = rig.depth();
  // something to redo
  NEED(rig.invoke("step.note", 61) == DispatchResult::Changed);  // stopped, Edit: a manual edit, a unit
  NEED(rig.depth() == d0 + 1u);
  (void)rig.invoke("edit.undo");
  NEED(rig.editor->can_redo());
  rig.play(2);
  rig.play(5);
  rig.shift_tap(Button::Rec);  // free, then follow again: nothing played in either
  rig.shift_tap(Button::Rec);
  rig.stop();
  CHECK(rig.depth() == d0);
  CHECK(rig.editor->can_redo());  // the empty take wrote nothing, and kept the history that redo needs
  (void)rig.invoke("edit.redo");
  CHECK(rig.depth() == d0 + 1u);
  // and a take whose notes were all refused is as empty
  rig.play(2);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // (undo during a take: closes it, which was empty, and undoes the manual edit)
  rig.stop();
  CHECK(rig.depth() == d0);
}

TEST_CASE("rec: an edit of another kind during the take closes it and is a unit of its own; the next note opens a new take") {
  Rig rig;
  rig.quantize_playing();
  const unsigned d0 = rig.depth();
  rig.play(4);                 // (LIVE: the cursor is on the playhead's step, 4, which is where the manual edit goes)
  rig.pad_down(12);            // a note: the first take
  rig.pad_up(12);
  CHECK(rig.depth() == d0);
  CHECK(rig.invoke("step.note", 70) == DispatchResult::Changed);  // a manual edit of the cursor's step (step 4, track 0)
  CHECK(rig.depth() == d0 + 2u);  // the take so far and the edit, two units
  CHECK(rig.step(0, 4).note == 70u);
  CHECK(rig.state().edit.take_open);  // recording goes on: a new take, empty so far
  rig.play(8);
  rig.pad_down(13);
  rig.pad_up(13);
  rig.stop();
  CHECK(rig.depth() == d0 + 3u);  // and the second take
  (void)rig.invoke("edit.undo");  // the second take
  CHECK(rig.step(0, 8).note == 0u);
  CHECK(rig.step(0, 4).note == 70u);
  (void)rig.invoke("edit.undo");  // the manual edit alone
  CHECK(rig.step(0, 4).note != 70u);
  CHECK(rig.step(0, 4).note != 0u);
  (void)rig.invoke("edit.undo");  // the first take
  CHECK(rig.step(0, 4).note == 0u);
  CHECK(rig.depth() == d0);
  // a track mute (an undoable parameter) and a controller during a take split it the same way
  rig.live();  // (the stop ended LIVE: OS-RECMODE2)
  rig.play(2);
  rig.pad_down(14);
  rig.pad_up(14);
  CHECK(rig.invoke("track.mute", 3) == DispatchResult::Changed);
  CHECK(rig.depth() == d0 + 2u);
  rig.stop();
  CHECK(rig.depth() == d0 + 2u);  // (the take after the mute is empty: no entry)
}

TEST_CASE("rec: Undo during the take undoes what was recorded so far and recording goes on in a new take") {
  Rig rig;
  rig.quantize_playing();
  const unsigned d0 = rig.depth();
  rig.play(3);
  rig.pad_down(12);
  rig.pad_up(12);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // F8 mid-take
  CHECK(rig.step(0, 3).note == 0u);
  CHECK(rig.depth() == d0);
  CHECK(rig.state().edit.take_open);                           // still recording
  rig.play(7);
  rig.pad_down(13);
  rig.pad_up(13);
  rig.stop();
  CHECK(rig.depth() == d0 + 1u);
  CHECK(rig.step(0, 7).note != 0u);
  CHECK(rig.step(0, 3).note == 0u);
}

TEST_CASE("rec: a macro played during a take is a unit of its own, and the take before it is another") {
  Rig rig;
  rig.quantize_playing();
  const unsigned d0 = rig.depth();
  NEED(rig.invoke("macro.record") != DispatchResult::Disabled);
  (void)rig.invoke("track.mute", 2);  // what the macro does: a mute (a unit when played)
  (void)rig.invoke("macro.record");
  (void)rig.invoke("edit.undo");      // the macro's own recording run is undone (it was a real mute)
  NEED(rig.depth() == d0);
  rig.play(4);
  rig.pad_down(12);
  rig.pad_up(12);
  (void)rig.invoke("macro.play");
  CHECK(rig.depth() == d0 + 2u);      // the take, and the macro
  rig.stop();
  CHECK(rig.depth() == d0 + 2u);
  (void)rig.invoke("edit.undo");      // the macro
  CHECK(rig.step(0, 4).note != 0u);
  (void)rig.invoke("edit.undo");      // the take
  CHECK(rig.step(0, 4).note == 0u);
}

TEST_CASE("rec: stopped, in Edit, the notes are manual edits as before: one unit each, no take") {
  Rig rig;
  const unsigned d0 = rig.depth();
  rig.pad_down(12);
  rig.pad_up(12);
  CHECK(rig.depth() == d0 + 1u);
  CHECK_FALSE(rig.state().edit.take_open);
  CHECK(rig.invoke("step.midi_note", 62) == DispatchResult::Changed);
  CHECK(rig.depth() == d0 + 2u);
}

namespace {
// A transport that takes every request: the held F1 chooser switches the playing pattern through it (OS-PATSWITCH) and remembers the last pattern asked for.
struct SwitchSink final : app::TransportSink {
  unsigned switched = 0;
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
  bool switch_pattern(unsigned id) noexcept override {
    switched = id;
    return true;
  }
};
}  // namespace

TEST_CASE("rec: the playhead in another pattern writes nothing and says so (the step between a held-F1 pick and its landing); a song that goes on to another pattern lets go of the held notes") {
  Rig rig;
  (void)rig.project().create_pattern(2, 16);
  rig.pos.publish(true, 2, 5, 12000, audio::PlayPosition::kNoOrder);
  rig.pass();
  CHECK(rig.state().edit.pattern == 2);  // LIVE follows it
  // A pattern the project does not have (a song may name one, a pad or the held chooser may pick an unused one) plays as empty: LIVE follows it, nothing is created by that.
  NEED(rig.project().pattern(9) == nullptr);
  rig.pos.publish(true, 9, 5, 12000, audio::PlayPosition::kNoOrder);
  rig.pass();
  CHECK(rig.state().edit.pattern == 9);
  CHECK(rig.project().pattern(9) == nullptr);
  rig.pos.publish(true, 2, 5, 12000, audio::PlayPosition::kNoOrder);
  rig.pass();
  CHECK(rig.state().edit.pattern == 2);
  // F1 held, pad 9: the pick waits for its step boundary. The view is on 9, the playhead still in 2: a note is refused and nothing is written to the wrong pattern.
  SwitchSink sink;
  rig.app->attach_transport(&sink);
  NEED(rig.invoke("chooser.hold", 0) == DispatchResult::Changed);
  (void)rig.invoke("chooser.pad", 8);
  NEED(sink.switched == 9);
  (void)rig.invoke("chooser.release", 0);
  CHECK(rig.state().edit.pattern == 9);
  rig.pad_down(12);
  CHECK(rig.message() == "THE PLAYHEAD IS IN ANOTHER PATTERN");
  CHECK(rig.step(0, 5).note == 0u);
  CHECK(rig.project().pattern(9) == nullptr);
  // a held note and a change of pattern: no OFF in the wrong pattern
  rig.pad_up(12);
  rig.pos.publish(true, 1, 5, 12000, audio::PlayPosition::kNoOrder);  // the playhead is back in a pattern the project has: the view goes to pattern 1
  rig.pass();
  CHECK(rig.state().edit.pattern == 1);
  rig.quantize_playing();
  rig.pad_down(13);
  CHECK(rig.state().edit.rec_open != 0u);
  rig.pos.publish(true, 2, 7, 12000, audio::PlayPosition::kNoOrder);
  rig.pass();
  CHECK(rig.state().edit.rec_open == 0u);
  rig.pad_up(13);
  NEED(rig.project().pattern(2) != nullptr);
  CHECK(rig.project().pattern(2)->steps[0][7].note == 0u);
}

// ---- a song, the keyboard, and Shift + arrows while it follows -----------------------------------------------------------------------------------------------------

TEST_CASE("rec: in a song the step after the pattern's last is not wrapped to: the note stays on the last step, and no OFF is written there") {
  Rig rig;
  const auto song = [&](unsigned step, unsigned ustep, bool playing = true) {
    rig.pos.publish(playing, 1, step, 12000, 0, ustep);  // song row 0
    rig.pass();
  };
  song(15, 30);  // late in the last step: Nearest would round to the next, which is another pattern
  rig.pad_down(12);
  CHECK(rig.step(0, 15).note == rig.note_of(12) + 1u);
  CHECK(rig.step(0, 0).note == 0u);  // not row 0 of the pattern being left
  rig.pad_up(12);                    // released in the same step: the OFF would be past the pattern's end
  CHECK(rig.step(0, 0).note == 0u);
  CHECK(rig.state().edit.rec_open == 0u);
  rig.stop();
  // In Pattern mode (no song row) it loops: row 0.
  Rig loop;
  loop.play(15, 30);
  loop.pad_down(12);
  CHECK(loop.step(0, 0).note == loop.note_of(12) + 1u);
  CHECK(loop.step(0, 15).note == 0u);
  loop.pad_up(12);
  CHECK(loop.step(0, 1).note == model::kNoteOff);  // released within the step it started in: the step after the note
  // A song and the earlier steps are as in a pattern: late in step 5 is step 6, the OFF at the playhead.
  Rig mid;
  mid.pos.publish(true, 1, 5, 12000, 0, 30);
  mid.pass();
  mid.pad_down(12);
  CHECK(mid.step(0, 6).note == mid.note_of(12) + 1u);
  mid.pos.publish(true, 1, 9, 12000, 0, 0);
  mid.pass();
  mid.pad_up(12);
  CHECK(mid.step(0, 9).note == model::kNoteOff);
}

TEST_CASE("rec: a keyboard note is placed by the playhead of the moment it is polled, not by the last pass") {
  Rig rig;
  rig.quantize_playing();
  rig.play(3);
  rig.pos.publish(true, 1, 7, 12000, audio::PlayPosition::kNoOrder, 0);  // the sequencer moved on; no pass of the app has run since
  CHECK(rig.send({0x90, 62, 100}) == 0u);                                 // (no preview sink: nothing sounds, the note is recorded)
  CHECK(rig.step(0, 7).note == 63u);
  CHECK(rig.step(0, 3).note == 0u);
  rig.pos.publish(true, 1, 9, 12000, audio::PlayPosition::kNoOrder, 0);
  (void)rig.send({0x80, 62, 0});
  CHECK(rig.step(0, 9).note == model::kNoteOff);  // the release too
  // Nearest uses the microstep of that moment as well
  rig.pos.publish(true, 1, 11, 12000, audio::PlayPosition::kNoOrder, 40);
  rig.set(ds::RecQuantize, 0);
  (void)rig.send({0x90, 64, 100});
  CHECK(rig.step(0, 12).note == 65u);
}

TEST_CASE("select: Shift + the arrows make a block while the cursor rides the playhead (LIVE, WATCH playing) as in STEP; its end is the arrows', the playhead leaves it (OS-BUG-WATCHSEL)") {
  Rig rig(32, false);
  const auto say_nothing = [&] {
    CHECK(rig.message().empty());
    CHECK(rig.state().notify.size() == 0u);
  };
  rig.play(8);  // WATCH, playing
  CHECK(rig.invoke("select.extend", 1) == DispatchResult::Changed);
  say_nothing();
  CHECK(rig.state().edit.selection.active);
  CHECK(rig.state().edit.selection.end_row == rig.state().edit.selection.start_row + 1);
  CHECK(rig.invoke("select.column_all", 0) == DispatchResult::Changed);
  CHECK(rig.state().edit.selection.column);
  rig.press(Button::Shift);  // ... by the keys as well
  rig.tap(Button::Right);
  rig.release(Button::Shift);
  CHECK(rig.state().edit.selection.end_track == 1);
  rig.shift_tap(Button::Rec);  // WATCH -> LIVE
  CHECK(rig.invoke("select.extend", 10) == DispatchResult::Changed);
  CHECK(rig.invoke("select.extend", -1) == DispatchResult::Changed);
  CHECK(rig.invoke("select.extend", -10) == DispatchResult::Changed);
  say_nothing();
  rig.play(11);  // the playhead moves on: the block stays where the arrows put it
  CHECK(rig.state().edit.selection.active);
  CHECK(rig.state().edit.selection.follow);  // (the playhead did not move or drop it)
  rig.shift_tap(Button::Rec);  // LIVE -> STEP (OS-LIVEEXIT): it selects, and says nothing
  CHECK(rig.invoke("select.extend", 1) == DispatchResult::Changed);
  say_nothing();
  CHECK(rig.state().edit.selection.active);
}

// ---- what went: SELECT -------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("select mode: it is retired; view.mode refuses it, mode_next is Edit and Browse, select.mark is gone, Shift + Rec is not a cycle") {
  Rig rig(16, false);
  const unsigned pattern = app::screen_index(app::ScreenId::Pattern);
  const app::ModeTable modes = app::kPatternScreen.modes();
  CHECK(modes.items[static_cast<unsigned>(PatternMode::Select)].retired);
  CHECK(modes.items[static_cast<unsigned>(PatternMode::Follow)].retired);
  CHECK_FALSE(modes.items[static_cast<unsigned>(PatternMode::Edit)].retired);
  CHECK_FALSE(modes.items[static_cast<unsigned>(PatternMode::Browse)].retired);
  CHECK(rig.invoke("view.mode", static_cast<ActionArg>(PatternMode::Select)) == DispatchResult::Unchanged);
  CHECK(rig.state().mode[pattern] == static_cast<unsigned>(PatternMode::Browse));
  for (int i = 0; i < 6; ++i) {  // the cycle: Edit, Browse, Edit ... never Select or Follow
    (void)rig.invoke("view.mode_next");
    const unsigned m = rig.state().mode[pattern];
    CHECK((m == static_cast<unsigned>(PatternMode::Edit) || m == static_cast<unsigned>(PatternMode::Browse)));
  }
  CHECK(app::find_action(app::action_registry(), "select.mark", 11) == nullptr);
  CHECK(rig.invoke("select.mark") == DispatchResult::Unknown);
  // Shift + Rec (playing): LIVE and WATCH, never a mode other than Edit or Browse
  rig.play(0);
  for (int i = 0; i < 5; ++i) {
    rig.shift_tap(Button::Rec);
    const unsigned m = rig.state().mode[pattern];
    CHECK((m == static_cast<unsigned>(PatternMode::Edit) || m == static_cast<unsigned>(PatternMode::Browse)));
  }
  // the info line says which
  CHECK(app::pattern_mode_edits(static_cast<unsigned>(PatternMode::Edit)));
  CHECK_FALSE(app::pattern_mode_edits(static_cast<unsigned>(PatternMode::Select)));
  CHECK_FALSE(app::pattern_mode_edits(static_cast<unsigned>(PatternMode::Browse)));
}

TEST_CASE("select: a block is Shift + the arrows in STEP and in WATCH (stopped); and while the cursor rides the playhead (LIVE)") {
  Rig rig(32, false);
  // Browse, stopped
  rig.press(Button::Shift);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.release(Button::Shift);
  CHECK(rig.state().edit.selection.active);
  CHECK(rig.state().edit.selection.end_row == 2);
  rig.tap(Button::Down);  // a plain move collapses it
  CHECK_FALSE(rig.state().edit.selection.active);
  // Edit, stopped
  rig.tap(Button::Rec);
  rig.press(Button::Shift);
  for (int i = 0; i < 8 && rig.state().pattern.track() < 1; ++i) {
    rig.tap(Button::Right);  // (OS-AUDIT-56: a column at a time inside track 0, then the next track whole)
  }
  rig.tap(Button::Down);
  rig.release(Button::Shift);
  CHECK(rig.state().edit.selection.active);
  CHECK(rig.state().edit.selection.end_track == 1);
  CHECK_FALSE(rig.state().edit.selection.extending);
  rig.tap(Button::Down);
  // playing in LIVE: the playhead owns the cursor
  rig.play(8);
  rig.shift_tap(Button::Rec);  // (Rec entered STEP; playing, Shift + Rec makes it LIVE)
  rig.press(Button::Shift);
  rig.tap(Button::Down);
  rig.release(Button::Shift);
  CHECK(rig.state().edit.selection.active);  // OS-BUG-WATCHSEL: it selects, by its own end row
  CHECK(rig.state().edit.selection.end_row == rig.state().edit.selection.start_row + 1);
  // STEP: the user's
  rig.shift_tap(Button::Rec);  // LIVE -> STEP (OS-LIVEEXIT)
  rig.press(Button::Shift);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.release(Button::Shift);
  CHECK(rig.state().edit.selection.active);
  CHECK(rig.state().edit.selection.end_row == rig.row());
  rig.play(9);
  CHECK(rig.state().edit.selection.active);  // the playhead does not touch it
}

// ---- a live pass ---------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("rec: a live pass: follow over a loop with a chord, a slip into free, a track change, and the whole pass undone in one") {
  Rig rig(16);
  rig.spill();
  const unsigned d0 = rig.depth();
  rig.go_track(1);
  (void)rig.invoke("instrument.select", 5);
  // step 0..3 straight, nothing played
  for (unsigned s = 0; s < 4; ++s) {
    rig.play(s, 0);
  }
  // a chord just before step 4 (Nearest: lands on 4), held through 6
  rig.play(3, 30);
  rig.pad_down(12);
  rig.pad_down(16);
  rig.pad_down(19);
  rig.play(6, 0);
  rig.pad_up(12);
  rig.pad_up(16);
  rig.pad_up(19);
  // the user moves to track 0 on step 8 and goes on playing on the playhead
  rig.play(8, 5);
  rig.go_track(0);
  rig.play(10, 10);
  rig.pad_down(13);
  rig.play(12, 0);
  rig.pad_up(13);
  // the notes
  CHECK(rig.step(1, 4).note == rig.note_of(12) + 1u);
  CHECK(rig.step(2, 4).note == rig.note_of(16) + 1u);
  CHECK(rig.step(3, 4).note == rig.note_of(19) + 1u);
  CHECK(rig.step(1, 4).instr == 5u);
  CHECK(rig.step(1, 6).note == model::kNoteOff);
  CHECK(rig.step(2, 6).note == model::kNoteOff);
  CHECK(rig.step(3, 6).note == model::kNoteOff);
  CHECK(rig.step(0, 10).note == rig.note_of(13) + 1u);
  CHECK(rig.step(0, 12).note == model::kNoteOff);
  CHECK(rig.row() == 12);  // LIVE: the cursor rides the playhead
  // 4 notes and 4 OFFs are one take: one unit once the recording stops, and one Undo gives the empty pattern back
  CHECK(rig.depth() == d0);
  rig.stop();
  CHECK(rig.depth() == d0 + 1u);
  (void)rig.invoke("edit.undo");
  CHECK(rig.depth() == d0);
  for (unsigned t = 0; t < 4; ++t) {
    for (unsigned r = 0; r < 16; ++r) {
      CHECK(rig.step(t, r).note == 0u);
    }
  }
}

// ---- OS-RECMICRO: the actions ---------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("rec micro: Off by default, a recorded note has no FX and no volume however late in its step") {
  Rig rig;
  rig.play(3, 10);
  CHECK(rig.invoke("step.midi_note", 62) == DispatchResult::Changed);
  CHECK(rig.step(0, 3).note == 63u);
  CHECK(rig.step(0, 3).fx[0].type == 0);
  CHECK(rig.step(0, 3).fx[1].type == 0);
  CHECK(rig.step(0, 3).vol == model::kVolumeUnset);
}

TEST_CASE("rec micro: On, a note late in its step writes the offset to FX 2; on the step's start it writes nothing; Nearest step rounds a late one up and clamps its offset to none") {
  Rig rig;
  rig.set(ds::RecMicro, 1);
  rig.play(3, 10);  // Nearest step, the first half: the playing step, 10 microsteps late
  (void)rig.invoke("step.midi_note", 62);
  CHECK(rig.step(0, 3).fx[1].type == rec::kFxMicroMove);
  CHECK(rig.step(0, 3).fx[1].value == 21);
  CHECK(rig.step(0, 3).fx[0].type == 0);  // FX 1 is never touched
  rig.play(5, 0);  // exactly on the step: no m02 00
  (void)rig.invoke("step.midi_note", 63);
  CHECK(rig.step(0, 5).note == 64u);
  CHECK(rig.step(0, 5).fx[1].type == 0);
  rig.play(8, 30);  // Nearest step, the second half: the next step, the note is early for it, no negative Micro-move
  (void)rig.invoke("step.midi_note", 64);
  CHECK(rig.step(0, 8).note == 0u);
  CHECK(rig.step(0, 9).note == 65u);
  CHECK(rig.step(0, 9).fx[1].type == 0);
  rig.quantize_playing();  // Playing step: the step the playhead is on, whatever the phase: 30 microsteps late
  rig.play(12, 30);
  (void)rig.invoke("step.midi_note", 65);
  CHECK(rig.step(0, 12).note == 66u);
  CHECK(rig.step(0, 12).fx[1].type == rec::kFxMicroMove);
  CHECK(rig.step(0, 12).fx[1].value == 63);
  rig.play(13, 47);  // the last microstep: the top of the range
  (void)rig.invoke("step.midi_note", 66);
  CHECK(rig.step(0, 13).fx[1].value == 100);
}

TEST_CASE("rec micro: a user FX in FX 2 stays, a Micro-move there is replaced, a recorded note over a step with an old m02 and no offset clears it") {
  Rig rig;
  rig.set(ds::RecMicro, 1);
  rig.quantize_playing();
  model::Step u = rig.step(0, 4);
  u.note = 50;
  u.instr = 1;
  u.fx[1] = {7, 9};
  (void)rig.editor->set_steps(1, 0, 4, &u, 1);
  model::Step m = u;
  m.fx[1] = {rec::kFxMicroMove, 80};
  (void)rig.editor->set_steps(1, 0, 6, &m, 1);
  rig.play(4, 20);
  (void)rig.invoke("step.midi_note", 62);
  CHECK(rig.step(0, 4).note == 63u);
  CHECK(rig.step(0, 4).fx[1].type == 7);  // the user's FX stays: this note has no Micro-move
  CHECK(rig.step(0, 4).fx[1].value == 9);
  rig.play(6, 20);
  (void)rig.invoke("step.midi_note", 62);
  CHECK(rig.step(0, 6).fx[1].type == rec::kFxMicroMove);
  CHECK(rig.step(0, 6).fx[1].value == 42);  // 20 * 100 / 47: the earlier take's 80 is replaced
  rig.play(6, 0);
  (void)rig.invoke("rec.note_off", 62);
  (void)rig.invoke("step.midi_note", 62);
  CHECK(rig.step(0, 6).fx[1].type == 0);  // on the step's start: the old Micro-move does not belong to the new note
}

TEST_CASE("rec micro: REC VELOCITY External writes the keyboard's velocity to the volume column, the pads' notes leave it") {
  Rig rig;
  rig.play(3);
  (void)rig.send({0x90, 62, 100});
  CHECK(rig.step(0, 3).note == 63u);
  CHECK(rig.step(0, 3).vol == model::kVolumeUnset);  // Off by default
  (void)rig.send({0x80, 62, 0});
  rig.set(ds::RecVelocity, 1);
  rig.play(5);
  (void)rig.send({0x90, 64, 100});
  CHECK(rig.step(0, 5).vol == 100u);
  (void)rig.send({0x80, 64, 0});
  rig.play(7);
  (void)rig.send({0x90, 65, 1});
  CHECK(rig.step(0, 7).vol == 1u);
  (void)rig.send({0x80, 65, 0});
  rig.play(9);
  rig.pad_down(12);  // a pad has no velocity to record
  CHECK(rig.step(0, 9).note != 0u);
  CHECK(rig.step(0, 9).vol == model::kVolumeUnset);
  rig.pad_up(12);
}

// ---- OS-MIDIREC-P1 S3: REC STORE = Lane (the stamped take, the merge, the notices) ---------------------------------------------------------------------------------

namespace {

model::LaneStore g_lane_store;  // static: the store and the take are large
model::LaneTake g_lane_take;

// A recording rig for the Lane: pattern 1 of `length` steps, MIDI instrument 7 on channel 1 selected, the store and the take attached, the transport playing in LIVE.
struct LaneRig : Rig {
  explicit LaneRig(unsigned length = 128, bool lane = true, std::size_t ring_bytes = sizeof g_ring) : Rig(length, true, ring_bytes) {
    g_lane_store.clear();
    g_lane_store.adopt(project().load_epoch());
    g_lane_take.reset();
    editor->attach_lanes(&g_lane_store);
    app->attach_lane_take(&g_lane_take);
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Midi);
    in.midi_channel = 1;
    (void)project().set_instrument(6, in);
    (void)invoke("instrument.select", 7);
    if (lane) {
      set(ds::RecStore, 1);
    }
    play(0, 0);
    (void)send({});  // the first poll arms the keyboard's ring
  }
  // The render's side of one message: the stamp of the block, then the bytes.
  void stamped(unsigned tick, std::initializer_list<std::uint8_t> bytes) {
    keys.set_stamp(1, tick);
    (void)send(bytes);
    keys.clear_stamp();
  }
  model::LaneStore::View lane() const { return g_lane_store.lane(1, 0); }
};

}  // namespace

TEST_CASE("rec lane: REC STORE = Lane records the keyboard into the lane with its tick and length, the grid stays empty, and the take is one undo unit") {
  LaneRig rig;
  CHECK(rig.keys.take_track() == 0u);  // armed for the cursor's track
  const unsigned before = rig.depth();
  rig.stamped(100, {0x90, 60, 90});
  rig.stamped(130, {0xB0, 74, 10});
  rig.stamped(160, {0x80, 60, 0});
  rig.stamped(175, {0xB0, 74, 40});
  rig.play(4, 0);  // the playhead is at tick 192 when the recording ends
  rig.stop();
  (void)rig.send({});
  CHECK(rig.keys.take_track() == midi::CcInput::kNoTrack);  // disarmed
  CHECK(rig.step(0, 2).note == 0u);                          // nothing in the grid
  const model::LaneStore::View v = rig.lane();
  CHECK(v.count == 3u);  // the note and two control changes
  CHECK(rig.depth() == before + 1u);  // one unit
  unsigned notes = 0;
  unsigned ccs = 0;
  for (unsigned i = 0; i < v.count; ++i) {
    if ((v.events[i].kind_channel >> 4) == 0u) {
      ++notes;
      CHECK(v.events[i].tick == 100);
      CHECK(v.events[i].len == 60);
      CHECK(v.events[i].a == 60);
      CHECK(v.events[i].b == 90);
    } else {
      ++ccs;
      CHECK(v.events[i].a == 74);
    }
  }
  CHECK(notes == 1u);
  CHECK(ccs == 2u);
  CHECK(rig.editor->undo(model::UndoArea::Pattern) == model::UndoResult::Ok);
  CHECK(rig.lane().count == 0u);
}

TEST_CASE("rec lane: with REC STORE = Grid (the default) the keyboard writes the grid as always and nothing is armed or stamped") {
  LaneRig rig(128, false);
  CHECK(rig.keys.take_track() == midi::CcInput::kNoTrack);
  rig.play(3, 0);
  (void)rig.send({0x90, 62, 100});
  CHECK(rig.step(0, 3).note == 63u);
  (void)rig.send({0x80, 62, 0});
  CHECK(rig.lane().count == 0u);
  midi::StampMsg m;
  CHECK_FALSE(rig.keys.pop_stamped(m));
}

TEST_CASE("rec lane: a take that fills the buffer commits what it has, says TAKE FULL, and goes on in a new take") {
  LaneRig rig;
  // A note is one event (its length is filled in by the off), so 2048 notes fill the take; the one that does not fit and the 52 after it go into a new one.
  constexpr unsigned kNotes = 2101;
  bool full = false;
  for (unsigned i = 0; i < kNotes; ++i) {
    const std::uint8_t key = static_cast<std::uint8_t>(30 + i % 60);
    rig.stamped(i * 2, {0x90, key, 90});
    rig.stamped(i * 2 + 1, {0x80, key, 0});
    if (!full && rig.lane().count != 0u) {  // the take was committed by the one note that filled it
      full = true;
      CHECK(rig.message().find("TAKE FULL") != std::string::npos);
      CHECK(rig.lane().count == model::LaneTake::kCapacity);
    }
  }
  CHECK(full);
  rig.play(88, 0);  // the playhead (tick 4224) is past the last note when the recording ends
  rig.play(88, 0, false);  // stop: the new take's 53 notes are merged after the first
  (void)rig.send({});
  CHECK(rig.lane().count == kNotes);
}

TEST_CASE("rec lane: a take the undo ring cannot hold is refused whole, kept for one more try, then dropped; the next take is merged normally") {
  LaneRig rig(128, true, 3 * 1024);  // a ring smaller than the 600 events' records (8 bytes each)
  const unsigned depth0 = rig.depth();
  for (unsigned i = 0; i < 600; ++i) {
    rig.stamped(i * 2, {0x90, static_cast<std::uint8_t>(30 + i % 60), 90});
    rig.stamped(i * 2 + 1, {0x80, static_cast<std::uint8_t>(30 + i % 60), 0});
  }
  rig.play(25, 0);
  rig.stop();
  (void)rig.send({});
  CHECK(rig.lane().count == 0u);  // nothing partial
  CHECK(rig.message().find("NO ROOM") != std::string::npos);
  CHECK(rig.message().find("KEPT") != std::string::npos);
  rig.live();  // (a stop leaves the Pattern screen in BROWSE)
  rig.play(0, 0);
  (void)rig.send({});  // recording begins again: the held take gets its one more try, still too big for this ring, and is dropped
  CHECK(rig.lane().count == 0u);
  CHECK(rig.message().find("DROPPED") != std::string::npos);
  CHECK(rig.depth() == depth0);  // no unit was written
  rig.stamped(10, {0x90, 60, 90});  // a new take is begun and merged normally
  rig.stamped(20, {0x80, 60, 0});
  rig.play(2, 0);
  rig.stop();
  (void)rig.send({});
  CHECK(rig.lane().count == 1u);
}

// ---- OS-MIDIREC-P1 S4 (A13): a knob's move is the CC lane's value ------------------------------------------------------------------------------------------------

namespace {

namespace ip = model::instrument_param;

// CC A of instrument 7 sends controller 74 (the selector holds 75), a Learn binding of external CC 20 (channel 1) moves it in `mode`, and the encoder targets it.
void lane_cc_setup(LaneRig& rig, core::LearnMode mode, std::uint8_t selector = 75) {
  model::Instrument in = *rig.project().instrument(6);
  in.midi_cc[0] = selector;
  (void)rig.project().set_instrument(6, in);
  const_cast<app::ViewState&>(rig.app->state()).enc_target =core::param_id(core::ParamKind::Instrument, 6, ip::midi_cc(0));
  const core::LearnBinding b{core::param_id(core::ParamKind::Instrument, 6, ip::midi_cc(0)), 20, 1, static_cast<std::uint8_t>(mode), 0};
  CHECK(rig.editor->set_learn_slot(0, b) == model::Status::Ok);
}
DispatchResult knob(LaneRig& rig, unsigned value) { return rig.invoke("midi.cc", static_cast<ActionArg>((1u << 16) | (20u << 8) | value)); }
unsigned selector(LaneRig& rig) { return rig.project().instrument(6)->midi_cc[0]; }
// The CC events of the lane (kind > 0), in tick order.
unsigned lane_ccs(LaneRig& rig, model::LaneEvent* out, unsigned max) {
  const model::LaneStore::View v = rig.lane();
  unsigned n = 0;
  for (unsigned i = 0; i < v.count && n < max; ++i) {
    if ((v.events[i].kind_channel >> 4) != 0u) {
      out[n++] = v.events[i];
    }
  }
  return n;
}
bool near(unsigned got, unsigned want) { return got + 1u >= want && got <= want + 1u; }

}  // namespace

TEST_CASE("rec lane cc: a Learn knob (Jump) on a CC letter records its position as the controller the letter names, and the selector is not rewritten") {
  LaneRig rig;
  lane_cc_setup(rig, core::LearnMode::Jump);
  const unsigned depth = rig.depth();
  rig.play(2, 0);
  (void)rig.send({});  // the poll takes the playhead
  (void)knob(rig, 127);
  rig.play(2, 24);
  (void)rig.send({});  // the poll takes the playhead
  (void)knob(rig, 40);
  rig.play(4, 0);
  CHECK(selector(rig) == 75u);  // the letter still sends controller 74
  rig.stop();
  (void)rig.send({});
  model::LaneEvent ev[4];
  const unsigned n = lane_ccs(rig, ev, 4);
  CHECK(n == 2u);
  if (n == 2u) {
    CHECK(ev[0].a == 74u);
    CHECK(ev[0].tick == 96u);
    CHECK(ev[0].b == 127u);
    CHECK(ev[1].a == 74u);
    CHECK(ev[1].tick == 120u);
    CHECK(near(ev[1].b, 40u));
    CHECK((ev[1].kind_channel & 0x0Fu) == 0u);  // the instrument's channel (1)
  }
  CHECK(rig.depth() <= depth + 2u);  // the set-up edits and the take, two units at most: the knob's moves are no unit of their own
  CHECK(selector(rig) == 75u);
  CHECK(rig.editor->undo(model::UndoArea::Pattern) == model::UndoResult::Ok);  // ONE undo takes the whole take: no unit of the knob's own is left behind
  CHECK(rig.lane().count == 0u);
  CHECK(selector(rig) == 75u);
}

TEST_CASE("rec lane cc: a relative knob and the encoder step the letter's running value from the stored one; nothing is written to the selector") {
  LaneRig rig;
  lane_cc_setup(rig, core::LearnMode::Relative);
  rig.play(2, 0);
  (void)rig.send({});  // the poll takes the playhead
  (void)knob(rig, 10);  // +10 from CC 64 (the first move of a letter starts there, not at the selector's 75)
  rig.play(2, 24);
  (void)rig.send({});  // the poll takes the playhead
  (void)knob(rig, 10);  // +10 from the running value
  rig.play(3, 0);
  (void)rig.send({});  // the poll takes the playhead
  (void)rig.invoke("enc.turn", -20);  // the encoder goes on from the same running value
  rig.play(4, 0);
  CHECK(selector(rig) == 75u);
  rig.stop();
  (void)rig.send({});
  model::LaneEvent ev[8];
  const unsigned n = lane_ccs(rig, ev, 8);
  CHECK(n == 3u);
  if (n == 3u) {
    CHECK(near(ev[0].b, 74u));  // option 66 of 131 (CC 64), +10: 76 of 131
    CHECK(near(ev[1].b, 83u));  // 86 of 131
    CHECK(near(ev[2].b, 64u));  // 66 of 131
    CHECK(ev[2].tick == 144u);
  }
  CHECK(rig.message().find("CC A") != std::string::npos);  // the encoder's strip names the letter and the value recorded
  CHECK(rig.message().find("64") != std::string::npos);
}

TEST_CASE("rec lane cc: a Pickup binding records the knob's position too (no pickup in a take), and the selector is untouched") {
  LaneRig rig;
  lane_cc_setup(rig, core::LearnMode::Pickup);
  rig.play(2, 0);
  (void)rig.send({});  // the poll takes the playhead
  (void)knob(rig, 127);  // far from the selector's 75 on the row's scale: a pickup would swallow it
  rig.play(4, 0);
  CHECK(selector(rig) == 75u);
  rig.stop();
  (void)rig.send({});
  model::LaneEvent ev[4];
  const unsigned n = lane_ccs(rig, ev, 4);
  CHECK(n == 1u);
  if (n == 1u) {
    CHECK(ev[0].a == 74u);
    CHECK(ev[0].b == 127u);
  }
}

TEST_CASE("rec lane cc: with REC STORE = Lane but no recording (stopped) a knob on the letter edits the selector as always") {
  LaneRig rig;
  lane_cc_setup(rig, core::LearnMode::Jump);
  rig.stop();
  (void)rig.send({});
  (void)knob(rig, 127);
  CHECK(selector(rig) == model::kMidiCcMax);
  CHECK(rig.lane().count == 0u);
}

TEST_CASE("rec lane cc: the running value of a letter starts again at CC 64 when the next take is armed") {
  LaneRig rig;
  lane_cc_setup(rig, core::LearnMode::Relative);
  rig.play(2, 0);
  (void)rig.send({});  // the poll takes the playhead
  (void)knob(rig, 10);
  rig.play(2, 24);
  (void)rig.send({});
  (void)knob(rig, 10);  // the second step of the first take
  rig.play(4, 0);
  rig.stop();
  (void)rig.send({});
  rig.live();  // (a stop leaves the Pattern screen in BROWSE)
  rig.play(0, 0);
  (void)rig.send({});  // the next take is armed
  rig.play(2, 0);
  (void)rig.send({});  // the poll takes the playhead
  (void)knob(rig, 10);  // not 20 steps from 64: the first step of a new take
  rig.play(4, 0);
  rig.stop();
  (void)rig.send({});
  model::LaneEvent ev[8];
  const unsigned n = lane_ccs(rig, ev, 8);
  CHECK(n >= 1u);
  bool found = false;
  for (unsigned i = 0; i < n; ++i) {
    if (ev[i].tick == 96u) {
      found = true;
      CHECK(near(ev[i].b, 74u));
    }
  }
  CHECK(found);
}

TEST_CASE("rec lane cc: PROG, channel and poly aftertouch and an unassigned letter record nothing and say so once; the selector is still not touched") {
  LaneRig rig;
  lane_cc_setup(rig, core::LearnMode::Jump, model::kMidiCcProg);
  rig.play(2, 0);
  (void)rig.send({});  // the poll takes the playhead
  (void)knob(rig, 90);
  CHECK(rig.message().find("NOT RECORDED") != std::string::npos);
  rig.flush_messages();
  rig.play(2, 24);
  (void)rig.send({});  // the poll takes the playhead
  (void)knob(rig, 20);
  CHECK(rig.message().find("NOT RECORDED") == std::string::npos);  // once per take
  rig.play(4, 0);
  rig.stop();
  (void)rig.send({});
  CHECK(rig.lane().count == 0u);
  CHECK(selector(rig) == model::kMidiCcProg);
}

TEST_CASE("rec lane cc: armed in the count-in, a knob and the encoder do not write the selector, the move is swallowed and the notice comes once, through to the take") {
  LaneRig rig;
  lane_cc_setup(rig, core::LearnMode::Jump);
  rig.live();
  rig.play(0, 0);
  (void)rig.send({});
  rig.pos.publish_count_in(2);
  rig.pass();
  (void)rig.send({});
  rig.flush_messages();  // (the strip shows the oldest word: let the transport's own run out)
  (void)knob(rig, 127);
  CHECK(selector(rig) == 75u);
  CHECK(rig.message().find("NOT RECORDED: COUNT-IN") != std::string::npos);
  rig.flush_messages();
  (void)knob(rig, 20);
  (void)rig.invoke("enc.turn", 3);
  CHECK(selector(rig) == 75u);
  CHECK(rig.message().find("NOT RECORDED") == std::string::npos);  // once
  // The count-in ends: the take begins, the notice is not said again, a move is the controller's value.
  rig.pos.publish_count_in(0);
  rig.play(2, 0);
  (void)rig.send({});
  (void)knob(rig, 127);
  CHECK(rig.message().find("NOT RECORDED") == std::string::npos);
  CHECK(selector(rig) == 75u);
  rig.play(4, 0);
  rig.stop();
  (void)rig.send({});
  model::LaneEvent ev[4];
  CHECK(lane_ccs(rig, ev, 4) == 1u);
  CHECK(selector(rig) == 75u);
}

TEST_CASE("rec lane cc: a count-in on a non-lane instrument leaves the knob editing the selector as it always did") {
  LaneRig rig(128, false);  // REC STORE = Grid
  lane_cc_setup(rig, core::LearnMode::Jump);
  rig.live();
  rig.play(0, 0);
  (void)rig.send({});
  rig.pos.publish_count_in(2);
  rig.pass();
  (void)knob(rig, 127);
  CHECK(selector(rig) == model::kMidiCcMax);
  CHECK(rig.message().find("COUNT-IN") == std::string::npos);
}

TEST_CASE("rec lane cc: a letter with the Config page's number left unassigned says so and records nothing") {
  LaneRig rig;
  lane_cc_setup(rig, core::LearnMode::Jump, model::kMidiCcConfig);
  model::MidiState midi = rig.project().midi();
  midi.cc_number[0] = model::kMidiCcUnassigned;
  (void)rig.project().set_midi(midi);
  rig.play(2, 0);
  (void)rig.send({});  // the poll takes the playhead
  (void)knob(rig, 90);
  CHECK(rig.message().find("UNASSIGNED") != std::string::npos);
  rig.play(4, 0);
  rig.stop();
  (void)rig.send({});
  CHECK(rig.lane().count == 0u);
}

TEST_CASE("rec lane cc: with REC STORE = Grid a knob on the letter edits the selector as it always did, and nothing is recorded to the lane") {
  LaneRig rig(128, false);
  lane_cc_setup(rig, core::LearnMode::Jump);
  rig.play(2, 0);
  (void)rig.send({});  // the poll takes the playhead
  (void)knob(rig, 127);
  CHECK(selector(rig) == model::kMidiCcMax);  // the knob at its top: the last option
  CHECK(rig.lane().count == 0u);
}

TEST_CASE("rec lane cc: an external controller on the track's channel goes through the take, IN CHANNEL filters it as for the notes") {
  LaneRig rig;
  rig.set(ds::MidiInChannel, 1);  // the keyboard's own filter (IN CHANNEL), as for the notes
  rig.play(2, 0);
  rig.stamped(96, {0xB0, 1, 30});
  rig.stamped(120, {0xB0, 1, 90});
  rig.stamped(130, {0xB1, 1, 99});  // channel 2: IN CHANNEL is 1
  rig.play(4, 0);
  rig.stop();
  (void)rig.send({});
  model::LaneEvent ev[8];
  const unsigned n = lane_ccs(rig, ev, 8);
  CHECK(n == 2u);
  for (unsigned i = 0; i < n; ++i) {
    CHECK(ev[i].a == 1u);
    CHECK(ev[i].b != 99u);
  }
}

// ---- OS-MIDIREC-P1 S5: chords, STEP mode, sub-step timing ------------------------------------------------------------------------------------------------------

namespace {

// STEP mode on a LaneRig: Shift + REC takes LIVE to STEP (Edit with the cursor free; the transport goes on).
void to_step(LaneRig& rig) {
  rig.shift_tap(Button::Rec);
  CHECK(rig.state().edit.rec_free);
}

}  // namespace

TEST_CASE("rec lane: a chord in LIVE is N note events at the tick it was played, each with its own length; the grid stays empty") {
  LaneRig rig;
  rig.stamped(100, {0x90, 60, 90});
  rig.stamped(100, {0x90, 64, 80});
  rig.stamped(101, {0x90, 67, 70});
  rig.stamped(148, {0x80, 64, 0});
  rig.stamped(160, {0x80, 60, 0});
  rig.play(4, 0);  // the playhead is at tick 192: the third key is still down and ends there
  (void)rig.send({});
  rig.stop();
  (void)rig.send({});
  const model::LaneStore::View v = rig.lane();
  CHECK(v.count == 3u);
  if (v.count != 3u) {
    return;
  }
  CHECK(v.events[0].tick == 100);
  CHECK(v.events[0].a == 60);
  CHECK(v.events[0].len == 60);
  CHECK(v.events[1].tick == 100);
  CHECK(v.events[1].a == 64);
  CHECK(v.events[1].len == 48);
  CHECK(v.events[2].tick == 101);
  CHECK(v.events[2].a == 67);
  CHECK(v.events[2].len == 91);  // up to the end of the take, 192
  for (unsigned s = 0; s < 6; ++s) {
    CHECK(rig.step(0, s).note == 0u);
  }
}

TEST_CASE("rec lane: sub-step timing: a note is stored at the tick of its stamp (1/48 step), not rounded to a step, and carries no Micro-move") {
  LaneRig rig;
  rig.set(ds::RecMicro, 1);
  rig.stamped(5, {0x90, 60, 90});
  rig.stamped(47, {0x80, 60, 0});
  rig.stamped(48, {0x90, 62, 90});
  rig.stamped(95, {0x80, 62, 0});
  rig.stamped(133, {0x90, 64, 90});
  rig.stamped(140, {0x80, 64, 0});
  rig.play(3, 0);
  rig.stop();
  (void)rig.send({});
  const model::LaneStore::View v = rig.lane();
  CHECK(v.count == 3u);
  if (v.count != 3u) {
    return;
  }
  CHECK(v.events[0].tick == 5);
  CHECK(v.events[0].len == 42);
  CHECK(v.events[1].tick == 48);
  CHECK(v.events[1].len == 47);
  CHECK(v.events[2].tick == 133);
  CHECK(v.events[2].len == 7);
  for (unsigned s = 0; s < 4; ++s) {
    CHECK(rig.step(0, s).note == 0u);
    CHECK(rig.step(0, s).fx[1].type == 0);
  }
}

TEST_CASE("rec lane: STEP mode: a held chord enters the cursor step's first tick with LEN one step, the cursor moves on when the last key is up, one undo unit") {
  LaneRig rig;
  to_step(rig);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.row() == 3);
  const unsigned d0 = rig.depth();
  (void)rig.send({0x90, 60, 90});
  (void)rig.send({0x90, 64, 80});
  CHECK(rig.row() == 3);  // held: the cursor waits
  CHECK(rig.lane().count == 0u);
  (void)rig.send({0x90, 67, 70});
  (void)rig.send({0x80, 60, 0});
  (void)rig.send({0x80, 64, 0});
  CHECK(rig.lane().count == 0u);  // one key is still down
  (void)rig.send({0x80, 67, 0});
  CHECK(rig.row() == 4);
  const model::LaneStore::View v = rig.lane();
  CHECK(v.count == 3u);
  if (v.count != 3u) {
    return;
  }
  for (unsigned i = 0; i < 3; ++i) {
    CHECK(v.events[i].tick == 3 * 48);
    CHECK(v.events[i].len == 48);
    CHECK((v.events[i].kind_channel >> 4) == 0u);
  }
  CHECK(v.events[0].a == 60);
  CHECK(v.events[1].a == 64);
  CHECK(v.events[2].a == 67);
  CHECK(v.events[2].b == 70);
  CHECK(rig.step(0, 3).note == 0u);  // the grid has nothing
  CHECK(rig.depth() == d0 + 1u);
  // the next chord goes to the next step
  (void)rig.send({0x90, 62, 90});
  (void)rig.send({0x80, 62, 0});
  CHECK(rig.row() == 5);
  CHECK(rig.lane().count == 4u);
  CHECK(rig.depth() == d0 + 2u);
  CHECK(rig.editor->undo(model::UndoArea::Pattern) == model::UndoResult::Ok);
  CHECK(rig.lane().count == 3u);
}

TEST_CASE("rec lane: STEP mode with REC STORE = Grid writes the grid as it always did, and the lane is untouched") {
  LaneRig rig(128, false);
  to_step(rig);
  (void)rig.send({0x90, 62, 100});
  (void)rig.send({0x80, 62, 0});
  CHECK(rig.step(0, 0).note == 63u);
  CHECK(rig.lane().count == 0u);
}

TEST_CASE("rec lane: REC STORE = Grid stays byte-identical whether the setting was ever Lane: Spill and Micro, the pattern and the lane") {
  const auto run = [](bool was_lane, std::vector<unsigned char>& bytes) {
    LaneRig rig(128, was_lane);
    if (was_lane) {
      rig.set(ds::RecStore, 0);
      (void)rig.send({});
    }
    rig.spill();
    rig.set(ds::RecMicro, 1);
    rig.play(2, 20);
    (void)rig.send({0x90, 60, 90});
    (void)rig.send({0x90, 64, 80});
    (void)rig.send({0x90, 67, 70});
    rig.play(3, 10);
    (void)rig.send({0x80, 60, 0});
    (void)rig.send({0x80, 64, 0});
    (void)rig.send({0x80, 67, 0});
    rig.play(6, 0);
    const model::Pattern* p = rig.project().pattern(1);
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&p->steps);
    bytes.assign(b, b + sizeof p->steps);
    CHECK(rig.lane().count == 0u);
    midi::StampMsg m;
    CHECK_FALSE(rig.keys.pop_stamped(m));
  };
  std::vector<unsigned char> a;
  std::vector<unsigned char> b;
  run(false, a);
  run(true, b);
  CHECK(a == b);
  bool any = false;
  for (const unsigned char c : a) {
    any = any || c != 0u;
  }
  CHECK(any);  // the chord did land in the grid (Spill)
}

TEST_CASE("rec lane: a song that goes on to another pattern ends the take at the end of ITS track: a held note runs to there, and only the moved controller's own range is replaced") {
  LaneRig rig;
  (void)rig.project().create_pattern(2, 128);
  // a first take leaves a control change far from where the second one moves
  rig.stamped(3000, {0xB0, 74, 20});
  rig.play(63, 0);  // tick 3024
  (void)rig.send({});
  rig.stop();
  (void)rig.send({});
  CHECK(rig.lane().count == 1u);
  rig.live();  // (a stop leaves the Pattern screen in BROWSE)
  rig.play(0, 0);
  (void)rig.send({});
  rig.stamped(100, {0x90, 60, 90});
  rig.stamped(100, {0xB0, 74, 10});
  rig.pos.publish(true, 2, 0, 12000, audio::PlayPosition::kNoOrder, 0);  // the song moved on to pattern 2
  rig.pass();
  (void)rig.send({});
  rig.stop();
  (void)rig.send({});
  const model::LaneStore::View v = rig.lane();
  CHECK(v.count == 3u);  // the old point at 3000 stays, the new one at 100 and the note
  if (v.count != 3u) {
    return;
  }
  bool old_kept = false;
  bool note_ok = false;
  for (unsigned i = 0; i < v.count; ++i) {
    const model::LaneEvent& e = v.events[i];
    if ((e.kind_channel >> 4) == 1u && e.tick == 3000) old_kept = e.b == 20;
    if ((e.kind_channel >> 4) == 0u) note_ok = e.tick == 100 && e.len == 6143 - 100;  // up to the last tick of pattern 1's track
  }
  CHECK(old_kept);
  CHECK(note_ok);
}

TEST_CASE("rec lane: a track with a Perform override keeps its take open across polls (the take's pattern is the track's own, not the song's)") {
  LaneRig rig;
  (void)rig.project().create_pattern(2, 128);
  rig.pos.publish_track_pattern(0, 2);  // track 0 plays pattern 2 while the song is in pattern 1
  rig.play(0, 0);
  rig.keys.set_stamp(2, 100);
  (void)rig.send({0x90, 60, 90});
  rig.keys.clear_stamp();
  CHECK(g_lane_take.open());
  rig.play(10, 0);
  CHECK(g_lane_take.open());  // not ended because the song's pattern differs
  rig.play(20, 0);
  CHECK(g_lane_take.open());
  rig.keys.set_stamp(2, 1000);
  (void)rig.send({0x80, 60, 0});
  rig.keys.clear_stamp();
  rig.stop();
  (void)rig.send({});
  const model::LaneStore::View v = g_lane_store.lane(2, 0);
  CHECK(v.count == 1u);
  if (v.count != 1u) {
    return;
  }
  CHECK(v.events[0].tick == 100);
  CHECK(v.events[0].len == 900);
}

TEST_CASE("rec lane: a stop during the count-in ends the take at its first stamp, not at tick 0 (no false wrap that replaces the lane)") {
  LaneRig rig;
  rig.stamped(4000, {0x90, 62, 80});
  rig.stamped(4010, {0x80, 62, 0});
  rig.play(84, 0);
  (void)rig.send({});
  rig.stop();
  (void)rig.send({});
  CHECK(rig.lane().count == 1u);
  rig.live();
  rig.play(0, 0);
  (void)rig.send({});
  rig.pos.publish_count_in(2);
  rig.pass();
  rig.stamped(3000, {0x90, 60, 90});
  rig.stop();  // the count-in is still published: the playhead is never read
  (void)rig.send({});
  const model::LaneStore::View v = rig.lane();
  CHECK(v.count == 2u);  // the old note at 4000 stays
}

// ---- OS-MIDIREC-P1 S7b: a lane records in the forward order only -------------------------------------------------------------------------------------------------

namespace {

app::PerformBoard g_mode_board;  // static, like the store: the Perform bank is not small

// The LaneRig with a Perform bank attached: slot 1 holds PLAYMODE and track 1 is affected, so `mode(m)` is what a mapped CC on that slot does (the bank wants play mode m for the track).
struct ModeRig : LaneRig {
  ModeRig() {
    g_mode_board.bank.reset();
    app->attach_perform(&g_mode_board);
    pass();  // the first pass sees the screen change and turns the Perform tracks off: toggle them after it, as a mapped CC leaves them
    while (g_mode_board.bank.fx(0) != seq::PerfFx::PatternPlayMode) {
      (void)g_mode_board.bank.change_fx(0, 1, g_mode_board.sink());
    }
    (void)g_mode_board.bank.toggle_track(0, g_mode_board.sink());
    pass();
  }
  void mode(int m) {
    for (unsigned v = 0; v < 128; ++v) {
      (void)g_mode_board.bank.set_cc(0, v, g_mode_board.sink());
      if (g_mode_board.bank.live(0) == m) {
        break;
      }
    }
    CHECK(g_mode_board.bank.live(0) == m);
    pass();
    (void)send({});  // the platform's keyboard poll of the frame: it reads the order and ends a take that is now in a non-forward one
  }
};

}  // namespace

TEST_CASE("rec lane order: an order change under a take (Backward, PingPong, Random, a custom order) drops the take, commits nothing, says FORWARD ONLY once, and the grid takes the keyboard") {
  for (const int m : {1, 2, 3, 4, 23}) {
    CAPTURE(m);
    ModeRig rig;
    CHECK(rig.keys.take_track() == 0u);  // armed in the forward order
    const unsigned before = rig.depth();
    rig.stamped(100, {0x90, 60, 90});
    rig.mode(m);
    CHECK(rig.state().track_mode[0] == m);
    CHECK(rig.keys.take_track() == midi::CcInput::kNoTrack);
    CHECK_FALSE(g_lane_take.open());
    CHECK(std::string(rig.message()).find("LANE REC: FORWARD ONLY") != std::string::npos);
    rig.flush_messages();
    rig.play(3, 0);
    (void)rig.send({0x90, 62, 100});
    CHECK(std::string(rig.message()).empty());  // said once
    CHECK(rig.step(0, 3).note == 63u);          // the grid has it
    (void)rig.send({0x80, 62, 0});
    rig.stop();
    (void)rig.send({});
    CHECK(rig.lane().count == 0u);  // nothing was written to the lane
    CHECK(rig.depth() == before + 1u);  // only the grid's note
  }
}

TEST_CASE("rec lane order: arming in a non-forward order is refused with the notice and no take; back to forward it records into the lane") {
  ModeRig rig;
  rig.stop();
  (void)rig.send({});
  rig.live();  // a stop leaves LIVE (the cursor is free): the way back to it, as the Rig does at the start
  rig.mode(3);  // Random
  rig.play(0, 0);
  (void)rig.send({});
  CHECK(rig.keys.take_track() == midi::CcInput::kNoTrack);
  CHECK(std::string(rig.message()).find("FORWARD ONLY") != std::string::npos);
  rig.mode(0);  // Forward again
  rig.play(0, 0);
  (void)rig.send({});
  CHECK(rig.keys.take_track() == 0u);
  rig.stamped(100, {0x90, 60, 90});
  rig.stamped(160, {0x80, 60, 0});
  rig.play(4, 0);
  rig.stop();
  (void)rig.send({});
  CHECK(rig.lane().count == 1u);
  CHECK(rig.step(0, 2).note == 0u);
}

TEST_CASE("rec lane order: a mode on another track leaves this track's take alone") {
  ModeRig rig;
  (void)g_mode_board.bank.toggle_track(0, g_mode_board.sink());  // track 1 is not affected any more
  (void)g_mode_board.bank.toggle_track(1, g_mode_board.sink());  // track 2 is
  rig.mode(2);
  CHECK(rig.state().track_mode[0] == 0u);
  CHECK(rig.state().track_mode[1] == 2u);
  CHECK(rig.keys.take_track() == 0u);
  rig.stamped(100, {0x90, 60, 90});
  rig.stamped(160, {0x80, 60, 0});
  rig.play(4, 0);
  rig.stop();
  (void)rig.send({});
  CHECK(rig.lane().count == 1u);
}
