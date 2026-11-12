// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PADS-A: the pads' layers on the Pattern and Perform screens (new Community OS behaviour). The LEDs of the step popups; the Shift picker and the momentary TRACKS layer; the TRACKS
// rows (TRACK / MUTE / MUTE MODE / SOLO) and their LEDs; Perform's AFFECTED row.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/note_entry.h>
#include <o3os/app/pattern_edit.h>
#include <o3os/audio/meters.h>
#include <o3os/audio/play_position.h>
#include <o3os/audio/kernels.h>
#include <o3os/hal/audio.h>
#include <o3os/app/track_state.h>
#include <o3os/app/pad_lights.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/model/editor.h>
#include <o3os/model/fx_table.h>
#include <o3os/model/instrument.h>
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

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;

  explicit Rig(unsigned length = 16) {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, length);
    (void)p.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->run_once();
  }
  model::Project& project() { return test::arena(0).project; }
  const app::ViewState& state() const { return app->state(); }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void hold_open(Button b) {
    press(b);
    pass(core::kGestureTimings.hold_ms + 1);
  }
  void let_go(Button b) {
    release(b);
    pass(1);
  }
  void instrument(unsigned slot, model::InstrumentKind kind) {
    model::Instrument in;
    model::init_instrument(in, kind);
    if (kind == model::InstrumentKind::Sample) {
      in.sample_path[0] = 'a';
      in.sample_path[1] = '\0';
    } else if (kind == model::InstrumentKind::Midi) {
      in.midi_channel = 1;
    }
    CHECK(project().set_instrument(slot, in) == model::Status::Ok);
  }
  bool lights(PadRole (&roles)[48]) const { return app->pad_lights(roles); }
  // OS-PADS-B helpers
  void pad_down(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 100));
    pass(1);
  }
  void pad_up(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 0));
    pass(1);
  }
  void tap_pad(unsigned pad) {
    pad_down(pad);
    pad_up(pad);
  }
  void tap(Button b) {
    press(b);
    pass(1);
    release(b);
    pass(1);
  }
  // OS-STEPPADOFF: Shift + F5 no longer picks STEP (the mode is kept, with no key until OS-STEPPAD); the tests of its code pick it through the action.
  void steps_on() { (void)app->invoke(core::action_id("pads.mode"), static_cast<core::ActionArg>(app::PadMode::Steps)); }
  // OS-UXF-PAT 7: the pads' modes are the PADS picker's (Shift + F2, then F1..F6 = NOTES PIANO ISO TRACKS PATS VU); the old Shift + F2..F7 of a test still name the mode, so this opens the picker and
  // presses the key that is one lower. Shift + F1 (the octave) and the other Shift keys are what they say.
  void shift_tap(Button b) {
    const unsigned n = static_cast<unsigned>(b);
    const bool mode = n >= static_cast<unsigned>(Button::F2) && n <= static_cast<unsigned>(Button::F7);
    press(Button::Shift);
    pass(1);
    tap(mode ? Button::F2 : b);
    release(Button::Shift);
    pass(1);
    if (mode) {
      tap(static_cast<Button>(n - 1u));
    }
  }
  // The note-entry settings through the panel (held Note, then its pads): root 0..11, a scale pad of row 2 (0..11), lock 0 / 1.
  void set_entry(unsigned root, unsigned scale, unsigned lock) {
    hold_open(Button::Note);
    tap_pad(root);
    tap_pad(12 + scale);
    tap_pad(36 + 10 + lock);
    let_go(Button::Note);
  }
  void edit_mode() { (void)app->invoke(core::action_id("view.mode"), static_cast<core::ActionArg>(app::PatternMode::Edit)); }
  model::Step step(unsigned row, unsigned track = 0) { return project().pattern(1)->steps[track][row]; }
  unsigned depth() const { return journal.depth(); }
};

}  // namespace

TEST_CASE("pad lights: the Instr popup lights the instruments with content Back, the current one Front, the empty ones Off") {
  Rig rig;
  rig.instrument(0, model::InstrumentKind::Sample);
  rig.instrument(2, model::InstrumentKind::Midi);
  rig.instrument(5, model::InstrumentKind::Sample);
  PadRole roles[48];
  NEED(rig.lights(roles));  // no popup, Browse: the NOTES board's lights (OS-NOTESLED), not the popup's
  CHECK(roles[36] == PadRole::Front);
  rig.hold_open(Button::Instr);
  NEED(rig.state().overlay == app::OverlayId::StepPopup);
  CHECK(rig.state().instrument == 1);
  NEED(rig.lights(roles));
  CHECK(roles[0] == PadRole::Front);  // the current one (it also has content)
  CHECK(roles[1] == PadRole::Off);
  CHECK(roles[2] == PadRole::Back);
  CHECK(roles[3] == PadRole::Off);
  CHECK(roles[5] == PadRole::Back);
  CHECK(roles[47] == PadRole::Off);
  // a pad chooses its instrument: the lights follow, the empty current one is Front too
  rig.input.push(core::make_pad_event(rig.clock.now(), 7, 100));
  rig.pass(1);
  NEED(rig.lights(roles));
  CHECK(rig.state().instrument == 8);
  CHECK(roles[7] == PadRole::Front);
  CHECK(roles[0] == PadRole::Back);
  CHECK(roles[2] == PadRole::Back);
  rig.let_go(Button::Instr);
  NEED(rig.lights(roles));  // the NOTES board again
  CHECK(roles[36] == PadRole::Front);
}

TEST_CASE("pad lights: the Fx popup lights the current FX") {
  Rig rig;
  rig.hold_open(Button::Fx);
  NEED(rig.state().overlay == app::OverlayId::StepPopup);
  PadRole roles[48];
  NEED(rig.lights(roles));
  const int item = rig.state().popup.item;
  NEED(item >= 0 && item < 48);
  for (unsigned n = 0; n < 48; ++n) {
    CHECK(roles[n] == (n == static_cast<unsigned>(item) ? PadRole::Front : PadRole::Off));
  }
  rig.input.push(core::make_pad_event(rig.clock.now(), 9, 100));
  rig.pass(1);
  NEED(rig.lights(roles));
  CHECK(roles[9] == PadRole::Front);
  CHECK(roles[static_cast<unsigned>(item)] == (item == 9 ? PadRole::Front : PadRole::Off));
  rig.let_go(Button::Fx);
}

// ---- the pads' mode and the momentary layer (pad_layer.h) --------------------------------------------------------------------------------

TEST_CASE("pad layer: a lone Shift is momentary TRACKS, any other control makes it yield for the rest of the hold, a pad does not") {
  app::PadLayer l;
  CHECK(l.effective() == app::PadMode::Notes);
  CHECK_FALSE(l.momentary());
  CHECK(l.shift_changed(true));
  CHECK(l.momentary());
  CHECK(l.effective() == app::PadMode::Tracks);
  CHECK(l.tracks(true));
  CHECK(l.tracks(false));  // Perform: the momentary layer only
  CHECK(l.other_input());  // an arrow, Insert, an F-key, the encoder: a chord
  CHECK_FALSE(l.momentary());
  CHECK(l.effective() == app::PadMode::Notes);
  CHECK_FALSE(l.other_input());  // already yielded
  CHECK_FALSE(l.shift_changed(true));  // no change: it is still held
  CHECK_FALSE(l.momentary());          // a release of the other key does not bring it back
  CHECK_FALSE(l.shift_changed(false));  // back to the mode before: Notes, no change
  // the next hold starts lone again
  CHECK(l.shift_changed(true));
  CHECK(l.momentary());
  CHECK(l.shift_changed(false));
  CHECK(l.effective() == app::PadMode::Notes);
  // another control with no Shift is nothing
  CHECK_FALSE(l.other_input());
  CHECK_FALSE(l.momentary());
}

TEST_CASE("pad layer: Shift + F1..F7 leave a mode fixed, a key past them is nothing, the release of a lone Shift goes back to the previous mode") {
  app::PadLayer l;
  CHECK(l.pick(2));  // TRACKS
  CHECK(l.fixed() == app::PadMode::Tracks);
  CHECK(l.effective() == app::PadMode::Tracks);
  CHECK(l.tracks(true));
  CHECK_FALSE(l.tracks(false));  // Perform has no fixed TRACKS
  CHECK_FALSE(l.pick(2));        // no change
  CHECK(l.pick(1));
  CHECK(l.fixed() == app::PadMode::Piano);
  for (unsigned key = app::kPadModes; key < app::kPadPickerKeys + 2; ++key) {
    CHECK_FALSE(l.pick(key));  // a key with no mode (F8 is redo)
    CHECK(l.fixed() == app::PadMode::Piano);
  }
  // a lone Shift over PIANO is TRACKS, and the release is PIANO again
  CHECK(l.shift_changed(true));
  CHECK(l.effective() == app::PadMode::Tracks);
  CHECK(l.shift_changed(true) == false);
  CHECK(l.shift_changed(false));
  CHECK(l.effective() == app::PadMode::Piano);
  // Shift + F3 while Shift is down: the chord yields and the fixed mode is what shows
  CHECK(l.shift_changed(true));
  CHECK(l.other_input());
  CHECK(l.pick(0));
  CHECK(l.effective() == app::PadMode::Notes);
  CHECK(l.shift_changed(false) == false);
  CHECK(l.effective() == app::PadMode::Notes);
}

TEST_CASE("pad layer: the release of a pad the layer took is the layer's") {
  app::PadLayer l;
  l.pad_down(5, true);
  l.pad_down(6, false);
  CHECK(l.pad_up(5));
  CHECK_FALSE(l.pad_up(5));  // once
  CHECK_FALSE(l.pad_up(6));
  CHECK_FALSE(l.pad_up(63));
  CHECK_FALSE(l.pad_up(96));  // the preview key is not a pad of the layer
  l.pad_down(7, true);
  l.pad_down(7, false);  // pressed again by another layer (the release was lost): not the layer's any more
  CHECK_FALSE(l.pad_up(7));
}

TEST_CASE("pad modes in the App: Shift + F1..F3 choose, the picker is on the key row, the mode that stays is in the info line; pattern.fill says it needs Edit") {
  Rig rig;
  const auto tap = [&](Button b) {
    rig.press(b);
    rig.pass(1);
    rig.release(b);
    rig.pass(1);
  };
  const auto shift_tap = [&](Button b) { rig.shift_tap(b); };  // (the mode keys go through the PADS picker: Rig::shift_tap)
  CHECK(rig.state().padmode.fixed() == app::PadMode::Notes);
  shift_tap(Button::F5);
  CHECK(rig.state().padmode.fixed() == app::PadMode::Tracks);
  shift_tap(Button::F3);
  CHECK(rig.state().padmode.fixed() == app::PadMode::Piano);
  shift_tap(Button::F3);  // the same again: no change
  CHECK(rig.state().padmode.fixed() == app::PadMode::Piano);
  CHECK(rig.state().notify.empty());
  // Shift + F8 is still redo, and Shift + the arrows still extend the block (a chord: the momentary layer yielded)
  rig.press(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().padmode.momentary());
  tap(Button::Down);
  CHECK_FALSE(rig.state().padmode.momentary());
  CHECK(rig.state().padmode.effective() == app::PadMode::Piano);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().padmode.effective() == app::PadMode::Piano);
  // pattern.fill (OS-CFGWIRE; no key since OS-QJUMP, Shift + Fx is the 3/4 jump) is an edit: outside Edit it says so (and where EXPORT, the key's other half, will come from)
  (void)rig.app->invoke(core::action_id("pattern.fill"));
  CHECK(std::string(rig.app->message()) == "FILL: TURN EDIT ON (EXPORT: F7 APPS >)");
  // the plain F7 is the verb menu (OS-PTOOLS2 S3), whose APPS > row is the TOOLS list (OS-H3A): with a document it lists EXPORT PATTERN even with no module system (OS-WAVEXP); F8 closes it
  rig.pass(3000);
  tap(Button::F7);
  rig.tap_pad(15);  // APPS > (pad 16)
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
  // the picker is drawn: a frame with Shift held differs from one without
  const std::uint32_t frames = rig.app->frames();
  rig.press(Button::Shift);
  rig.pass(1);
  CHECK(rig.app->frames() > frames);
  CHECK(rig.app->failed_frames() == 0u);
  rig.release(Button::Shift);
  rig.pass(1);
}

// ---- the TRACKS layer ----------------------------------------------------------------------------------------------------------------------

namespace {

// The sequencer's side as the TRACKS pads see it (the same fake as mute_test.cpp's).
struct FakeSink final : app::TrackSink {
  bool run = true;
  std::uint16_t pending = 0;
  std::uint8_t counts[16] = {};
  bool playing() const noexcept override { return run; }
  bool mute_defer(unsigned t, app::MuteMode, bool) noexcept override {
    pending = static_cast<std::uint16_t>(pending | (1u << t));
    return true;
  }
  bool mute_cancel(unsigned t) noexcept override {
    pending = static_cast<std::uint16_t>(pending & ~(1u << t));
    return true;
  }
  std::uint16_t mute_pending() const noexcept override { return pending; }
  std::uint8_t note_count(unsigned t) const noexcept override { return counts[t]; }
};

struct TracksRig : Rig {
  FakeSink sink;
  std::unique_ptr<app::MixBoard> board = std::make_unique<app::MixBoard>();
  TracksRig() {
    board->tracks = &sink;
    app->attach_mix(board.get());
    pass(1);
  }
  ~TracksRig() { app->attach_mix(nullptr); }
  void pad(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 100));
    pass(1);
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass(1);
  }
  void tap(Button b) {
    press(b);
    pass(1);
    release(b);
    pass(1);
  }
  std::uint8_t flags(unsigned t) { return project().track(t)->flags; }
};

constexpr unsigned kRowTrack = 0, kRowMute = 12, kRowMode = 24, kRowSolo = 36;

}  // namespace

TEST_CASE("TRACKS pads: a lone Shift hold takes the pads: row 1 the cursor, row 2 mute, row 3 the mute mode, row 4 solo; the release gives them back") {
  TracksRig rig;
  rig.press(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().padmode.momentary());
  rig.pad(kRowTrack + 3);
  CHECK(rig.app->view().track() == 3);
  rig.pad(kRowMute + 2);
  CHECK(rig.flags(2) == model::kTrackMute);
  CHECK(rig.flags(3) == 0);
  rig.pad(kRowMode + 2);
  CHECK(rig.board->mute_mode[2] == static_cast<std::uint8_t>(app::MuteMode::Bar));
  rig.pad(kRowMode + 2);
  CHECK(rig.board->mute_mode[2] == static_cast<std::uint8_t>(app::MuteMode::Pattern));
  rig.pad(kRowMode + 2);
  CHECK(rig.board->mute_mode[2] == static_cast<std::uint8_t>(app::MuteMode::Immediate));
  rig.pad(kRowSolo + 5);
  rig.pad(kRowSolo + 6);  // solo is multi-track
  CHECK(rig.flags(5) == model::kTrackSolo);
  CHECK(rig.flags(6) == model::kTrackSolo);
  rig.pad(kRowSolo + 5);
  CHECK(rig.flags(5) == 0);
  rig.pad(kRowMute + 2);  // the second press un-mutes
  CHECK(rig.flags(2) == 0);
  rig.pad(kRowTrack + 11);  // a track that exists in the model (12 are active): the cursor goes there
  CHECK(rig.app->view().track() == 11);
  CHECK(rig.state().notify.empty());
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK_FALSE(rig.state().padmode.momentary());
  // back to NOTES: a pad is a preview again, it does not mute
  rig.pad(kRowMute + 4);
  CHECK(rig.flags(4) == 0);
  CHECK(rig.app->view().track() == 11);
}

TEST_CASE("TRACKS pads: the momentary layer yields to any chord (an arrow, Delete, an F-key, the encoder), and a pad is not one") {
  TracksRig rig;
  rig.press(Button::Shift);
  rig.pass(1);
  rig.pad(kRowMute + 1);  // a pad: still the layer
  CHECK(rig.flags(1) == model::kTrackMute);
  CHECK(rig.state().padmode.momentary());
  rig.tap(Button::Down);  // Shift + Down: the block, not a track
  CHECK_FALSE(rig.state().padmode.momentary());
  rig.pad(kRowMute + 7);  // the layer yielded: a plain pad (Browse preview)
  CHECK(rig.flags(7) == 0);
  rig.release(Button::Shift);
  rig.pass(1);
  rig.press(Button::Shift);  // a new hold is lone again
  rig.pass(1);
  CHECK(rig.state().padmode.momentary());
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, 1));
  rig.pass(1);
  CHECK_FALSE(rig.state().padmode.momentary());
  rig.release(Button::Shift);
  rig.pass(1);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F8);  // Shift + F8 is redo
  CHECK_FALSE(rig.state().padmode.momentary());
  rig.release(Button::Shift);
  rig.pass(1);
}

TEST_CASE("TRACKS pads: Shift + F5 leaves TRACKS fixed (no Shift needed), Shift + F2 puts NOTES back; PIANO behaves as NOTES until part B") {
  TracksRig rig;
  rig.shift_tap(Button::F5);  // the PADS picker's TRACKS
  CHECK(rig.state().padmode.fixed() == app::PadMode::Tracks);
  rig.pad(kRowSolo + 8);
  CHECK(rig.flags(8) == model::kTrackSolo);
  rig.pad(kRowTrack + 4);
  CHECK(rig.app->view().track() == 4);
  // other screens are not touched: the File screen's pads have no meaning here (Song has a TRACKS layer of its own since OS-SONGPADS), and Pattern comes back as it was
  rig.tap(Button::File);
  rig.pad(kRowSolo + 9);
  CHECK(rig.flags(9) == 0);
  rig.tap(Button::Pattern);
  rig.pad(kRowSolo + 9);
  CHECK(rig.flags(9) == model::kTrackSolo);
  // a chooser is an overlay: the pads are the chooser's
  rig.shift_tap(Button::F3);  // the PADS picker's PIANO
  CHECK(rig.state().padmode.fixed() == app::PadMode::Piano);
  rig.pad(kRowSolo + 10);
  CHECK(rig.flags(10) == 0);
  rig.shift_tap(Button::F2);  // NOTES
  CHECK(rig.state().padmode.fixed() == app::PadMode::Notes);
  rig.pad(kRowSolo + 10);
  CHECK(rig.flags(10) == 0);
}

TEST_CASE("TRACKS pads: the LEDs of each row (Front = yours, Back = silenced by a solo / the next bar, blink = pending, Off = sounding)") {
  TracksRig rig;
  rig.press(Button::Shift);
  rig.pass(1);
  rig.pad(kRowMute + 1);  // track 2 muted
  rig.pad(kRowSolo + 3);  // track 4 soloed: every other track is silenced by it
  rig.pad(kRowMode + 5);  // track 6: at the next bar
  rig.pad(kRowMode + 6);
  rig.pad(kRowMode + 6);  // track 7: at the end of the pattern
  rig.pad(kRowTrack + 2);  // the cursor on track 3
  rig.pass(1);
  PadRole roles[48];
  NEED(rig.lights(roles));
  // row 1: the cursor's track Front, the others Off (no note fired)
  for (unsigned t = 0; t < 12; ++t) {
    CHECK(roles[kRowTrack + t] == (t == 2 ? PadRole::Front : PadRole::Off));
  }
  // row 2: muted by you Front; silenced by another's solo Back; the soloed one itself sounds (Off)
  for (unsigned t = 0; t < 12; ++t) {
    CHECK_MESSAGE(roles[kRowMute + t] == (t == 1 ? PadRole::Front : (t == 3 ? PadRole::Off : PadRole::Back)), "track " << t + 1);
  }
  // row 3: Immediate Off, Bar Back, Pattern Front
  for (unsigned t = 0; t < 12; ++t) {
    CHECK(roles[kRowMode + t] == (t == 5 ? PadRole::Back : (t == 6 ? PadRole::Front : PadRole::Off)));
  }
  // row 4: soloed Front
  for (unsigned t = 0; t < 12; ++t) {
    CHECK(roles[kRowSolo + t] == (t == 3 ? PadRole::Front : PadRole::Off));
  }
  // a track that is muted AND silenced by the solo is yours: Front
  rig.pad(kRowMute + 3);  // the soloed track muted too
  rig.pass(1);
  NEED(rig.lights(roles));
  CHECK(roles[kRowMute + 3] == PadRole::Front);
  rig.pad(kRowMute + 3);
  // a pending toggle blinks: Front and Off alternate over time
  rig.sink.pending = 1u << 8;
  bool seen_front = false, seen_off = false;
  for (int i = 0; i < 40; ++i) {
    rig.pass(30);
    NEED(rig.lights(roles));
    seen_front = seen_front || roles[kRowMute + 8] == PadRole::Front;
    seen_off = seen_off || roles[kRowMute + 8] == PadRole::Off;
    CHECK(roles[kRowMute + 8] != PadRole::Back);
  }
  CHECK(seen_front);
  CHECK(seen_off);
  rig.sink.pending = 0;
  // the activity flash: a track that fires a note flashes Back for a moment, the cursor's track stays Front
  rig.pass(30);
  rig.sink.counts[5] = 1;
  rig.sink.counts[2] = 1;
  rig.pass(1);
  NEED(rig.lights(roles));
  CHECK(roles[kRowTrack + 5] == PadRole::Back);
  CHECK(roles[kRowTrack + 2] == PadRole::Front);
  rig.pass(app::kFlashMs + 20);
  NEED(rig.lights(roles));
  CHECK(roles[kRowTrack + 5] == PadRole::Off);
  // tracks the pattern does not have stay dark
  CHECK(rig.project().set_active_tracks(8) == model::Status::Ok);
  rig.pass(1);
  NEED(rig.lights(roles));
  for (unsigned row = 0; row < 4; ++row) {
    for (unsigned t = 8; t < 12; ++t) {
      CHECK(roles[row * 12 + t] == PadRole::Off);
    }
  }
  rig.release(Button::Shift);
  rig.pass(1);
  NEED(rig.lights(roles));  // NOTES again: its own lights (OS-NOTESLED2: a note in the scale Front; no scale, every note)
  CHECK(roles[36] == PadRole::Front);
}

TEST_CASE("TRACKS pads: the track flash lights for a moment per note counter change, nothing at start-up, nothing without a sink") {
  FakeSink sink;
  sink.counts[3] = 9;
  app::TrackFlash f;
  CHECK_FALSE(f.poll(&sink, 1000));  // the first poll only takes the counters
  CHECK(f.lit() == 0u);
  sink.counts[3] = 10;
  CHECK(f.poll(&sink, 1010));
  CHECK(f.lit() == (1u << 3));
  CHECK_FALSE(f.poll(&sink, 1010 + app::kFlashMs - 1));
  CHECK(f.lit() == (1u << 3));
  sink.counts[3] = 11;  // another note extends it
  CHECK_FALSE(f.poll(&sink, 1010 + app::kFlashMs + 50));
  CHECK(f.lit() == (1u << 3));
  CHECK(f.poll(&sink, 1010 + 2 * app::kFlashMs + 60));
  CHECK(f.lit() == 0u);
  app::TrackFlash none;
  CHECK_FALSE(none.poll(nullptr, 5));
  CHECK(none.lit() == 0u);
}

// ---- OS-PADS-B: the note-entry settings (note_entry.h) ----------------------------------------------------------------------------------------

TEST_CASE("note entry: the scales are the masks of core/scales.h, snap goes to the nearest in-scale note and the lower one on a tie") {
  app::NoteEntry e;
  e.scale = 2;  // Major, the official index (OS-SCALES28)
  CHECK(std::string(e.scale_name()) == "Major");
  CHECK(e.snap(60) == 60);
  CHECK(e.snap(61) == 60);  // C# is as far from C as from D: the lower
  CHECK(e.snap(63) == 62);
  CHECK(e.snap(66) == 65);
  CHECK(e.snap(70) == 69);
  e.root = 9;  // A major: C# is in, C is not
  CHECK(e.in_scale(61));
  CHECK_FALSE(e.in_scale(60));
  CHECK(e.snap(60) == 59);  // B is in (one below), C# one above: the lower
  e.scale = 15;             // Blues, the official index: Blues on A: A C D Eb E G
  CHECK(std::string(e.scale_name()) == "Blues");
  CHECK(e.snap(70) == 69);  // A# -> A
  // For every scale, root and note: the result is in the scale, within 2 semitones and a valid MIDI note; an in-scale note is itself.
  for (unsigned scale = 0; scale <= model::kNoteScaleMax; ++scale) {  // all 39 official scales (OS-SCALES28)
    for (unsigned root = 0; root < 12; ++root) {
      e.scale = static_cast<std::uint8_t>(scale);
      e.root = static_cast<std::uint8_t>(root);
      for (unsigned note = 0; note < 128; ++note) {
        const unsigned s = e.snap(note);
        NEED(s < 128);
        NEED(e.in_scale(s));
        const unsigned dist = s > note ? s - note : note - s;
        NEED(dist <= 4u);                                              // at the range's edges the snap may go the long way round
        if (note >= 6u && note <= 121u) NEED(dist <= 2u);               // inside, no gap of the 39 is wider than 4 semitones
        NEED((!e.in_scale(note) || s == note));
      }
    }
  }
  e.lock = 0;
  CHECK(e.locked(61) == 61);
  e.lock = 1;
  e.scale = 2;  // Major
  e.root = 0;
  CHECK(e.locked(61) == 60);
}

TEST_CASE("note entry: the panel's pads set root, scale, jump, octave and lock, and the NOTES board follows") {
  app::PatternEdit p;
  CHECK(p.pads.note(36) == 36);  // the default: C, octave 3, no lock: the board as it always was
  CHECK(p.pads.note(0) == 72);   // the top left pad carries the highest notes
  CHECK(p.entry_pad(4));         // row 1: ROOT E
  CHECK(p.entry.root == 4);
  CHECK(p.pads.note(36) == 40);
  CHECK_FALSE(p.entry_pad(4));   // no change
  CHECK(p.entry.scale == 2);     // a new project starts in Major (OS-DEFSCALE)
  CHECK_FALSE(p.entry_pad(12 + 1));  // row 2: SCALE Major is already the scale: no change
  CHECK(p.entry_pad(12 + 0));    // pad 1: Chromatic, the official 0
  CHECK(p.entry.scale == 0);
  CHECK(p.entry_pad(12 + 11));   // pad 12: Whole tone, the official 23 (OS-SCALES28: row 2 goes through model::kNotePanelScales)
  CHECK(p.entry.scale == 23);
  CHECK(p.entry_pad(12 + 1));    // back to Major
  CHECK(p.entry.scale == 2);
  CHECK(p.pads.note(37) == 41);  // still chromatic
  CHECK(p.entry_pad(24 + 5));    // row 3: JUMP 5: the F3 chooser's own setting
  CHECK(p.edit_step == 5);
  CHECK(p.entry_pad(36 + 6));    // row 4: OCTAVE 6
  CHECK(p.pads.note(36) == 6 * 12 + 4);
  CHECK(p.entry_pad(36 + 11));   // LOCK on
  CHECK(p.entry.lock == 1);
  CHECK(p.pads.note(36) == 76);  // E major from E6: E F# G# A B ...
  CHECK(p.pads.note(37) == 78);
  CHECK(p.pads.note(38) == 80);
  CHECK(p.entry_pad(36 + 10));   // LOCK off
  CHECK(p.entry.lock == 0);
  CHECK(p.pads.note(37) == 77);
  CHECK(p.entry_pad(36 + 9));    // the top octave: the board never goes past 120
  CHECK(p.entry.octave == 9);
  CHECK(p.pads.note(0) <= 120);
}

TEST_CASE("note entry: the held Note button's pads set the entry, light each row's choice and write nothing") {
  Rig rig;
  PadRole roles[48];
  rig.hold_open(Button::Note);
  NEED(rig.state().overlay == app::OverlayId::StepPopup);
  NEED(rig.lights(roles));
  CHECK(roles[0] == PadRole::Front);      // ROOT C
  CHECK(roles[1] == PadRole::Back);
  CHECK(roles[12] == PadRole::Back);      // SCALE Chromatic is not the default any more (OS-DEFSCALE)
  CHECK(roles[12 + 1] == PadRole::Front); // SCALE Major, the default of a new project
  CHECK(roles[24 + 1] == PadRole::Front); // JUMP 1 (the edit step's default)
  CHECK(roles[36 + 3] == PadRole::Front); // OCTAVE 3
  CHECK(roles[36 + 10] == PadRole::Front);  // LOCK off
  CHECK(roles[36 + 11] == PadRole::Back);
  rig.input.push(core::make_pad_event(rig.clock.now(), 7, 100));  // ROOT G
  rig.input.push(core::make_pad_event(rig.clock.now(), 12 + 2, 100));  // SCALE Minor (pad 3 = the official 1)
  rig.input.push(core::make_pad_event(rig.clock.now(), 36 + 11, 100));  // LOCK on
  rig.pass(1);
  CHECK(rig.state().edit.entry.root == 7);
  CHECK(rig.state().edit.entry.scale == 1);
  CHECK(rig.state().edit.entry.lock == 1);
  NEED(rig.lights(roles));
  CHECK(roles[7] == PadRole::Front);
  CHECK(roles[0] == PadRole::Back);
  CHECK(roles[12 + 2] == PadRole::Front);
  CHECK(roles[36 + 11] == PadRole::Front);
  CHECK(roles[36 + 10] == PadRole::Back);
  rig.let_go(Button::Note);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.app->failed_frames() == 0u);  // the panel is drawn
}

TEST_CASE("note entry: a scale from the 27 of a file lights no pad of row 2 (OS-SCALES28)") {
  Rig rig;
  model::NoteState n;
  model::init_note_state(n);
  n.scale = 26;  // Hirajoshi, the official index: outside the panel's twelve
  n.lock = 1;
  CHECK(rig.project().set_note(n) == model::Status::Ok);
  rig.app->document_replaced();
  CHECK(rig.state().edit.entry.scale == 26);
  CHECK(rig.state().edit.pads.scale() == 27);  // the core index, laid out with the lock on
  PadRole roles[48];
  rig.hold_open(Button::Note);
  NEED(rig.lights(roles));
  for (unsigned c = 0; c < 12; ++c) {
    CHECK(roles[12 + c] == PadRole::Back);  // no pad of row 2 is Front, as JUMP above 11 leaves row 3 dark
  }
  rig.let_go(Button::Note);
}

// ---- OS-PADS-B: PIANO ---------------------------------------------------------------------------------------------------------------------------

TEST_CASE("PIANO: the pad map is a piano drawn on the pads, two octaves, the navigation column and the octave keys") {
  using app::PadKey;
  const auto note = [](unsigned pad, unsigned octave = 3) {
    app::NoteEntry e;
    e.octave = static_cast<std::uint8_t>(octave);
    return app::pad_fn_note(e, app::piano_pad(pad));
  };
  CHECK(app::piano_pad(0).kind == PadKey::Nav);  // column 1: start, 1/4, 1/2, 3/4
  CHECK(app::piano_pad(12).value == 1);
  CHECK(app::piano_pad(24).value == 2);
  CHECK(app::piano_pad(36).kind == PadKey::Nav);
  CHECK(app::piano_pad(36).value == 3);
  CHECK(app::piano_pad(11).kind == PadKey::OctUp);  // column 12: row 1 up, row 4 down
  CHECK(app::piano_pad(47).kind == PadKey::OctDown);
  CHECK(app::piano_pad(23).kind == PadKey::None);
  CHECK(app::piano_pad(35).kind == PadKey::None);
  for (unsigned row = 0; row < 4; ++row) {  // columns 10 and 11: the bend strip (OS-PIANOBEND): rows 1, 2 up by 2, 1; rows 3, 4 down by 1, 2
    const int semis[4] = {2, 1, -1, -2};
    CHECK(app::piano_pad(12 * row + 9).kind == PadKey::Bend);
    CHECK(app::piano_pad(12 * row + 10).kind == PadKey::Bend);
    CHECK(app::bend_semitones(app::piano_pad(12 * row + 9)) == semis[row]);
    CHECK(app::bend_semitones(app::piano_pad(12 * row + 10)) == semis[row]);
  }
  // The white keys, C..C, on rows 2 (the upper octave) and 4 (the lower)
  const unsigned white[8] = {0, 2, 4, 5, 7, 9, 11, 12};
  for (unsigned k = 0; k < 8; ++k) {
    CHECK(note(36 + 1 + k) == 36 + white[k]);
    CHECK(note(12 + 1 + k) == 48 + white[k]);
  }
  // The black keys on columns 3, 4, 6, 7, 8 of rows 1 and 3, and nothing under the gaps (E-F and B-C)
  const unsigned black_col[5] = {2, 3, 5, 6, 7};
  const unsigned black_semi[5] = {1, 3, 6, 8, 10};
  for (unsigned k = 0; k < 5; ++k) {
    CHECK(note(24 + black_col[k]) == 36 + black_semi[k]);
    CHECK(note(black_col[k]) == 48 + black_semi[k]);
  }
  for (unsigned row = 0; row < 4; row += 2) {
    CHECK(app::piano_pad(12 * row + 1).kind == PadKey::None);
    CHECK(app::piano_pad(12 * row + 4).kind == PadKey::None);
    CHECK(app::piano_pad(12 * row + 8).kind == PadKey::None);
  }
  // A key above the pad board's last note does not exist
  CHECK(note(13, 9) == 120);                // the upper C of octave 9 is the last note of the board
  CHECK(note(14, 9) == app::kNoNote);       // the D above it does not exist
  CHECK(note(37, 9) == 108);
  CHECK(note(12 + 8, 8) == 120);            // the top C of the upper octave of 8: the last that exists
}

TEST_CASE("PIANO: Shift + F3 keeps it, a pad enters its note in Edit, the octave keys move the settings' octave, SCALE LOCK snaps") {
  Rig rig;
  rig.edit_mode();
  rig.shift_tap(Button::F3);
  NEED(rig.state().padmode.fixed() == app::PadMode::Piano);
  rig.tap_pad(37);  // C of the lower octave, octave 3 -> C-3 (note 36, byte 37)
  CHECK(rig.step(0).note == 37);
  CHECK(rig.depth() == 1);  // one undo unit
  rig.tap_pad(11);  // octave up
  CHECK(rig.state().edit.entry.octave == 4);
  CHECK(rig.depth() == 1);
  rig.tap_pad(37);
  CHECK(rig.step(1).note == 49);
  CHECK(rig.state().edit.last_note == 48);
  rig.tap_pad(47);  // octave down, twice
  rig.tap_pad(47);
  CHECK(rig.state().edit.entry.octave == 2);
  // SCALE LOCK on, C major: the black key above C snaps down to C
  rig.set_entry(0, 1, 1);
  rig.tap_pad(26);  // C# of the lower octave
  CHECK(rig.step(2).note == 25);  // C-2 (byte 25): snapped
  // the navigation column jumps the cursor: 1/2 of 16 steps
  rig.tap_pad(24);
  CHECK(rig.state().pattern.row() == 8);
}

namespace {
struct BendLog final : app::PreviewSink {
  struct Call {
    int semis;
    unsigned instrument;
  };
  std::vector<Call> calls;
  void preview(unsigned, unsigned) noexcept override {}
  void pad_bend(int semitones, unsigned instrument) noexcept override { calls.push_back({semitones, instrument}); }
};
}  // namespace

TEST_CASE("PIANO: the bend strip bends while held, the last pad pressed wins and the release gives the previous pad's bend or 0, nothing is written (OS-PIANOBEND)") {
  Rig rig;
  BendLog sink;
  rig.app->attach_preview(&sink);
  rig.edit_mode();
  rig.shift_tap(Button::F3);
  NEED(rig.state().padmode.fixed() == app::PadMode::Piano);
  const unsigned depth = rig.depth();
  rig.pad_down(9);  // row 1 col 10: +2
  NEED(sink.calls.size() == 1u);
  CHECK(sink.calls[0].semis == 2);
  CHECK(rig.state().edit.bend.holds(9));
  CHECK(rig.state().edit.bend.active() == 2);
  PadRole roles[48];
  NEED(rig.lights(roles));
  CHECK(roles[9] == PadRole::Front);   // held
  CHECK(roles[10] == PadRole::Back);   // the other pad of the row, at rest
  rig.pad_down(46);  // row 4 col 11: -2, on top of +2
  NEED(sink.calls.size() == 2u);
  CHECK(sink.calls[1].semis == -2);
  rig.pad_up(9);  // the older pad lets go: the newer one stays in force, nothing is sent that changes the pitch... the sink hears the same bend
  CHECK(rig.state().edit.bend.active() == -2);
  CHECK(sink.calls.back().semis == -2);
  rig.pad_up(46);  // the last one: back in tune
  CHECK(sink.calls.back().semis == 0);
  CHECK(rig.state().edit.bend.n == 0u);
  CHECK_FALSE(rig.state().edit.bend.holds(46));
  // A release with the PADS mode changed in between still lets go (the release is the app's, not the mode's).
  rig.pad_down(22);  // row 2 col 11: +1
  CHECK(sink.calls.back().semis == 1);
  rig.shift_tap(Button::F4);  // ISO: pad 22 is a note there, but its release is still the bend's
  rig.pad_up(22);
  CHECK(sink.calls.back().semis == 0);
  CHECK(rig.state().edit.bend.n == 0u);
  CHECK(rig.depth() == depth);  // no undo unit: nothing written to the pattern
  CHECK(rig.step(0).note == 0);
  // The bend carries the instrument in view; a pad outside the strip does not touch the hold.
  rig.shift_tap(Button::F3);
  const std::size_t n = sink.calls.size();
  rig.tap_pad(37);  // a key
  CHECK(sink.calls.size() == n);
  // A pad of the strip pressed twice without a release (a lost one) is one hold; reset_input() clears it.
  rig.pad_down(21);
  rig.pad_down(21);
  CHECK(rig.state().edit.bend.n == 1u);
  rig.app->reset_input();
  CHECK(rig.state().edit.bend.n == 0u);
}

TEST_CASE("BendHold: the order of the holds decides the bend, a pad pressed again moves to the end, a release of a pad not held changes nothing, eight at most (OS-PIANOBEND)") {
  app::BendHold h;
  CHECK(h.active() == 0);
  CHECK(h.press(1, 2) == 2);
  CHECK(h.press(2, -1) == -1);
  CHECK(h.press(1, 2) == 2);   // pressed again: now the newest
  CHECK(h.n == 2u);
  CHECK(h.release(1) == -1);
  CHECK(h.release(7) == -1);   // not held
  CHECK(h.release(2) == 0);
  CHECK(h.release(2) == 0);
  for (unsigned i = 0; i < 12; ++i) {
    (void)h.press(i, 1);
  }
  CHECK(h.n == 8u);  // a ninth is not remembered (its own release finds nothing)
  h.clear();
  CHECK(h.n == 0u);
  CHECK(h.active() == 0);
  CHECK(app::bend_semitones(app::PadFn{}) == 0);
  CHECK(app::bend_semitones(app::PadFn{app::PadKey::Note, 4}) == 0);
}

TEST_CASE("PIANO: the LEDs light the existing keys by scale (in the scale Front, outside Back, the root like the rest), no key Off, the jump and octave pads as before (OS-NOTESLED2)") {
  Rig rig;
  rig.shift_tap(Button::F3);
  PadRole roles[48];
  NEED(rig.lights(roles));
  CHECK(roles[37] == PadRole::Front);  // C
  CHECK(roles[38] == PadRole::Front);  // D: Chromatic has every note in the scale
  CHECK(roles[0] == PadRole::Back);    // the navigation column
  CHECK(roles[4] == PadRole::Off);     // no key there
  CHECK(roles[21] == PadRole::Back);   // (row 2 col 10: a bend pad, OS-PIANOBEND)
  CHECK(roles[35] == PadRole::Off);    // (row 3 col 12: no key)
  CHECK(roles[11] == PadRole::Back);   // octave up
  rig.set_entry(2, 1, 0);  // D major
  NEED(rig.lights(roles));
  CHECK(roles[38] == PadRole::Front);  // D (the root: no marker of its own)
  CHECK(roles[39] == PadRole::Front);  // E
  CHECK(roles[40] == PadRole::Back);   // F is not in D major
  CHECK(roles[26] == PadRole::Front);  // C#, the 7th of D major
  CHECK(roles[0] == PadRole::Back);    // the jump and octave pads do not follow the scale
  CHECK(roles[11] == PadRole::Back);
  CHECK(roles[4] == PadRole::Off);
  rig.set_entry(2, 1, 1);  // SCALE LOCK on: a key plays the locked note, so every key is lit Front (F plays E or F#)
  NEED(rig.lights(roles));
  CHECK(roles[38] == PadRole::Front);
  CHECK(roles[40] == PadRole::Front);
  CHECK(roles[26] == PadRole::Front);
  CHECK(roles[0] == PadRole::Back);   // the jump pad is unchanged
  CHECK(roles[4] == PadRole::Off);    // no key there
}

TEST_CASE("PIANO: the octave pads light HIGH in the direction the octave is shifted from the default 3, Off at the end of the range (OS-NOTESLED2)") {
  Rig rig;
  rig.shift_tap(Button::F3);
  PadRole roles[48];
  struct Want {
    unsigned octave;
    PadRole up, down;
  };
  // Shift + F1 steps the octave up by one, 9 wraps to 0: 3 -> 5 -> 9 -> 0 -> 2 -> 3
  const Want wants[] = {
      {3, PadRole::Back, PadRole::Back},  {5, PadRole::Front, PadRole::Back}, {9, PadRole::Off, PadRole::Back},
      {0, PadRole::Back, PadRole::Off},   {2, PadRole::Back, PadRole::Front}, {3, PadRole::Back, PadRole::Back},
  };
  NEED(rig.state().edit.entry.octave == app::kEntryDefaultOctave);
  const unsigned taps[] = {0, 2, 4, 1, 2, 1};
  for (unsigned i = 0; i < 6; ++i) {
    for (unsigned t = 0; t < taps[i]; ++t) {
      rig.shift_tap(Button::F1);
    }
    NEED(rig.state().edit.entry.octave == wants[i].octave);
    NEED(rig.lights(roles));
    CHECK_MESSAGE(roles[11] == wants[i].up, "octave " << wants[i].octave << " up pad");
    CHECK_MESSAGE(roles[47] == wants[i].down, "octave " << wants[i].octave << " down pad");
  }
}

// OS-NOTESLED / OS-NOTESLED2: the NOTES board lit as PIANO and ISO light theirs; what a pad plays is not touched.
TEST_CASE("NOTES: the LEDs light by scale, a note in the scale Front (the root too), one outside Back, and with SCALE LOCK or no scale every pad Front (OS-NOTESLED2)") {
  Rig rig;
  PadRole roles[48];
  NEED(rig.state().padmode.fixed() == app::PadMode::Notes);
  rig.set_entry(0, 0, 0);   // Chromatic (a new project starts in C major, OS-DEFSCALE): every note is in the scale
  NEED(rig.lights(roles));
  for (const PadRole r : roles) {
    CHECK(r == PadRole::Front);
  }
  rig.set_entry(2, 1, 0);  // D major, lock off: the board keeps its chromatic notes
  NEED(rig.lights(roles));
  unsigned back = 0;
  for (unsigned pad = 0; pad < 48u; ++pad) {
    const unsigned n = rig.state().edit.pads.note(pad);
    const unsigned pc = n % 12u;
    const bool in_scale = pc == 2u || pc == 4u || pc == 6u || pc == 7u || pc == 9u || pc == 11u || pc == 1u;  // D E F# G A B C#
    CHECK(roles[pad] == (in_scale ? PadRole::Front : PadRole::Back));
    back += roles[pad] == PadRole::Back ? 1u : 0u;
  }
  CHECK(back > 0u);  // F, G#, A#, D#, C are on the chromatic board and not in the scale
  CHECK(roles[36 + 2] == PadRole::Front);  // D, the root: lit like the other scale notes
  rig.set_entry(2, 1, 1);  // SCALE LOCK on: the board holds the scale's notes only
  NEED(rig.lights(roles));
  for (unsigned pad = 0; pad < 48u; ++pad) {
    CHECK(roles[pad] == PadRole::Front);  // every note of the board is in the scale
  }
  // PIANO keeps its own lights and leaving it for NOTES brings these back.
  rig.shift_tap(Button::F3);
  NEED(rig.lights(roles));
  CHECK(roles[0] == PadRole::Back);  // PIANO's navigation column
  rig.shift_tap(Button::F2);
  NEED(rig.lights(roles));
  CHECK(roles[0] == PadRole::Front);
}

// ---- OS-PADS-B: ISOMORPHIC ----------------------------------------------------------------------------------------------------------------------

TEST_CASE("ISO: a column is a semitone and a row up is a fourth; Shift + F4 keeps it, the pads enter their notes, the LEDs show root and scale") {
  app::NoteEntry e;  // octave 3
  CHECK(app::pad_fn_note(e, app::iso_pad(36)) == 36);       // the bottom left
  CHECK(app::pad_fn_note(e, app::iso_pad(37)) == 37);       // a column right: a semitone
  CHECK(app::pad_fn_note(e, app::iso_pad(24)) == 41);       // a row up: +5
  CHECK(app::pad_fn_note(e, app::iso_pad(12)) == 46);
  CHECK(app::pad_fn_note(e, app::iso_pad(0)) == 51);
  CHECK(app::pad_fn_note(e, app::iso_pad(11)) == 62);       // the top right: 11 + 15 above
  e.octave = 9;
  CHECK(app::pad_fn_note(e, app::iso_pad(36 + 12 - 1)) == 119);
  CHECK(app::pad_fn_note(e, app::iso_pad(0)) == app::kNoNote);  // 108 + 15 does not exist
  Rig rig;
  rig.edit_mode();
  rig.shift_tap(Button::F4);
  NEED(rig.state().padmode.fixed() == app::PadMode::Iso);
  rig.tap_pad(24);  // F-3 (note 41, byte 42)
  CHECK(rig.step(0).note == 42);
  PadRole roles[48];
  rig.set_entry(0, 0, 0);              // Chromatic (a new project starts in C major, OS-DEFSCALE)
  NEED(rig.lights(roles));
  CHECK(roles[36] == PadRole::Front);  // C-3
  CHECK(roles[31] == PadRole::Front);  // C-4: 12 above, 7 columns right and a row up (7 + 5)
  CHECK(roles[37] == PadRole::Front);  // Chromatic: every note is in the scale (OS-NOTESLED2: Front by scale, no root marker)
  rig.set_entry(0, 1, 0);              // C major
  NEED(rig.lights(roles));
  CHECK(roles[36] == PadRole::Front);  // C, the root: like the other scale notes
  CHECK(roles[37] == PadRole::Back);   // C# is outside the scale
  CHECK(roles[38] == PadRole::Front);  // D
  rig.set_entry(2, 1, 1);              // D major with SCALE LOCK: the pads play the locked notes, so every pad is Front
  NEED(rig.lights(roles));
  for (unsigned pad = 0; pad < 48u; ++pad) {
    CHECK_MESSAGE(roles[pad] == PadRole::Front, "pad " << pad);
  }
}

// ---- OS-PADS-B: STEP ----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("STEP: a tap on an empty step writes the last note played, a tap on a lit one clears it, each an undo unit") {
  Rig rig;
  rig.steps_on();
  NEED(rig.state().padmode.fixed() == app::PadMode::Steps);
  rig.pad_down(3);  // nothing played yet: our default note (60, byte 61), the current instrument
  CHECK(rig.step(3).note == 61);
  CHECK(rig.step(3).instr == 1);
  CHECK(rig.depth() == 1);
  rig.pad_up(3);
  CHECK(rig.step(3).note == 61);  // the release of a fresh step leaves it
  CHECK(rig.depth() == 1);
  rig.tap_pad(3);  // lit: the tap clears it (at the release)
  CHECK(rig.step(3).note == model::kNoteEmpty);
  CHECK(rig.depth() == 2);
  // the last note played: a NOTES pad (Browse preview) first, then STEP
  rig.shift_tap(Button::F2);
  rig.tap_pad(36);  // the lowest pad of the board: C-3, note 36
  CHECK(rig.state().edit.last_note == 36);
  rig.steps_on();
  const unsigned before = rig.depth();
  rig.tap_pad(47);  // step 47 is past the 16 steps of this pattern: a dead pad
  CHECK(rig.depth() == before);
  rig.tap_pad(5);
  CHECK(rig.step(5).note == 37);
  CHECK(rig.depth() == before + 1);
  CHECK(rig.app->invoke(core::action_id("edit.undo"), 0) == app::DispatchResult::Changed);
  CHECK(rig.step(5).note == model::kNoteEmpty);  // one undo unit
}

TEST_CASE("STEP: a held lit step and the encoder change its note, by semitones or by the scale with SCALE LOCK; the release then leaves it") {
  Rig rig;
  rig.steps_on();
  rig.tap_pad(2);  // C-5 (default 60)
  NEED(rig.step(2).note == 61);
  rig.pad_down(2);
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, 2));
  rig.pass(1);
  CHECK(rig.step(2).note == 63);  // +2 semitones
  rig.pad_up(2);
  CHECK(rig.step(2).note == 63);  // turned: not cleared
  rig.set_entry(0, 1, 1);         // C major, SCALE LOCK on
  rig.pad_down(2);
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, 1));  // D (62) -> E (64), byte 65... from 62: the next in C major is E
  rig.pass(1);
  CHECK(rig.step(2).note == 65);
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, -2));
  rig.pass(1);
  CHECK(rig.step(2).note == 61);  // E -> D -> C
  rig.pad_up(2);
  CHECK(rig.step(2).note == 61);
}

TEST_CASE("STEP: 48 steps to a page, the page of the cursor's row; steps past the track's end have no pad; the LEDs show notes and the playhead") {
  Rig rig(64);
  audio::PlayPosition position;
  rig.app->attach_play_position(&position);
  rig.steps_on();
  rig.tap_pad(2);
  rig.tap_pad(30);
  NEED(rig.step(2).note == 61);
  CHECK(rig.step(30).note == 61);
  PadRole roles[48];
  NEED(rig.lights(roles));
  CHECK(roles[2] == PadRole::Back);
  CHECK(roles[30] == PadRole::Back);
  CHECK(roles[3] == PadRole::Off);
  position.publish(true, 1, 3, 12000);  // the playhead on step 3
  rig.pass(1);
  NEED(rig.lights(roles));
  CHECK(roles[3] == PadRole::Front);
  CHECK(roles[2] == PadRole::Back);
  position.publish(false, 1, 0, 12000);
  rig.pass(1);
  // page 2: the cursor on row 50: pad 2 is step 50, and pads from 16 on (step 64) do not exist
  (void)rig.app->invoke(core::action_id("nav.scroll"), 50 - rig.state().pattern.row());
  CHECK(rig.state().pattern.row() == 50);
  rig.tap_pad(2);
  CHECK(rig.step(50).note == 61);
  CHECK(rig.step(2).note == 61);  // page 1 untouched
  NEED(rig.lights(roles));
  CHECK(roles[2] == PadRole::Back);
  CHECK(roles[16] == PadRole::Off);
  const unsigned depth = rig.depth();
  rig.tap_pad(16);  // step 64: past the track
  CHECK(rig.depth() == depth);
}

// ---- OS-PADS-B: PATS ----------------------------------------------------------------------------------------------------------------------------

namespace {
struct QueueLog final : app::TransportSink {
  unsigned queued = 0, calls = 0;
  bool accept = true;
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
  bool queue_pattern(unsigned id) noexcept override {
    if (!accept) {
      return false;
    }
    queued = id;
    ++calls;
    return true;
  }
};
}  // namespace

TEST_CASE("PATS: lit = has content, Front = playing, a tap queues at the end of the pattern (blinking), again cancels; stopped it selects") {
  Rig rig;
  QueueLog log;
  audio::PlayPosition position;
  rig.app->attach_transport(&log);
  rig.app->attach_play_position(&position);
  model::Step note = model::kEmptyStep;
  note.note = 61;
  NEED(rig.project().create_pattern(2, 16) == model::Status::Ok);
  NEED(rig.editor->set_steps(2, 0, 4, &note, 1) == model::Status::Ok);
  rig.shift_tap(Button::F6);
  NEED(rig.state().padmode.fixed() == app::PadMode::Patterns);
  rig.pass(1);
  PadRole roles[48];
  NEED(rig.lights(roles));
  CHECK(roles[0] == PadRole::Front);  // stopped: the pattern being edited (1)
  CHECK(roles[1] == PadRole::Back);   // pattern 2 has a note
  CHECK(roles[2] == PadRole::Off);    // pattern 3 does not exist
  rig.tap_pad(2);                     // stopped: the tap selects pattern 3
  CHECK(rig.state().edit.pattern == 3);
  CHECK(log.calls == 0);
  position.publish(true, 1, 0, 12000);
  rig.pass(1);
  rig.tap_pad(5);  // playing: pattern 6 is queued
  CHECK(log.queued == 6);
  CHECK(rig.state().edit.queued_pattern == 6);
  NEED(rig.lights(roles));
  CHECK(roles[0] == PadRole::Front);  // pattern 1 plays
  const PadRole first = roles[5];
  CHECK((first == PadRole::Front || first == PadRole::Off));
  rig.pass(app::kBlinkMs);
  NEED(rig.lights(roles));
  CHECK(roles[5] != first);  // it blinks
  rig.tap_pad(5);            // again: cancelled
  CHECK(log.queued == 0);
  CHECK(rig.state().edit.queued_pattern == 0);
  rig.tap_pad(1);
  CHECK(log.queued == 2);
  position.publish(true, 2, 0, 12000);  // the sequencer has started pattern 2
  rig.pass(1);
  CHECK(rig.state().edit.queued_pattern == 0);
  NEED(rig.lights(roles));
  CHECK(roles[1] == PadRole::Front);
  CHECK(roles[0] == PadRole::Off);   // pattern 1 exists but holds nothing
}

// ---- OS-PADS-B: VU and FADERS -------------------------------------------------------------------------------------------------------------------

TEST_CASE("pad layer: Shift + F7 is VU, again FADERS, again VU; another mode ends it") {
  app::PadLayer l;
  CHECK(l.pick(6));
  CHECK(l.fixed() == app::PadMode::Vu);
  CHECK_FALSE(l.faders());
  CHECK(std::string(l.name()) == "VU");
  CHECK(l.pick(6));  // the same key again: the next page
  CHECK(l.faders());
  CHECK(std::string(l.name()) == "FADERS");
  CHECK(l.pick(6));
  CHECK_FALSE(l.faders());
  CHECK(l.pick(6));
  CHECK(l.pick(0));  // NOTES: the pages are forgotten
  CHECK_FALSE(l.faders());
  CHECK_FALSE(l.pick(0));
  CHECK_FALSE(l.pick(7));  // F8 is redo, not a mode
}

TEST_CASE("FADERS: a tap on row n sets the track's volume in four steps through the parameter path, the pads show the level; VU lights from the published peaks") {
  Rig rig;
  audio::MeterStore store;
  audio::MeterBank bank(store);
  app::MixBoard mix;
  mix.meters = &bank;
  rig.app->attach_mix(&mix);
  const core::ParamId vol2 = core::param_id(core::ParamKind::Track, 2, model::track_param::Volume);
  rig.shift_tap(Button::F7);  // VU
  NEED(rig.state().padmode.fixed() == app::PadMode::Vu);
  PadRole roles[48];
  NEED(rig.lights(roles));
  for (unsigned n = 0; n < 48; ++n) {
    CHECK(roles[n] == PadRole::Off);  // nothing is sounding
  }
  rig.tap_pad(2);  // VU only shows: nothing is written
  CHECK(rig.depth() == 0);
  // a track that sounds: the bars follow its published peak (the ballistics are fed once a meter window); a half-scale peak is -6 dB, the top of the scale
  const std::uint64_t frames = audio::kMeterWindowBlocks * hal::audio::kBlock;
  const auto window = [&](bool loud) {
    for (unsigned b = 0; b < audio::kMeterWindowBlocks; ++b) {
      if (loud && b + 1u == audio::kMeterWindowBlocks) {
        bank.acc(2) = audio::kernel::MeterAcc{16384u, frames * 8192ull * 8192ull};
      }
      bank.end_block();
    }
    rig.pass(1);
  };
  window(true);
  NEED(rig.lights(roles));
  CHECK(roles[2] == PadRole::Front);  // column 3, all four pads lit, the top one Front
  CHECK(roles[12 + 2] == PadRole::Back);
  CHECK(roles[36 + 2] == PadRole::Back);
  CHECK(roles[36 + 3] == PadRole::Off);  // column 4 is silent
  for (unsigned w = 0; w < 60; ++w) {
    window(false);  // it falls
  }
  NEED(rig.lights(roles));
  CHECK(roles[2] == PadRole::Off);
  rig.shift_tap(Button::F7);  // FADERS
  NEED(rig.state().padmode.faders());
  const auto level = [&] { return rig.project().get_param(vol2); };
  rig.tap_pad(2);  // row 1: 100
  CHECK(level() == 100.0f);
  CHECK(rig.depth() == 1);
  rig.tap_pad(24 + 2);  // row 3: 50
  CHECK(level() == 50.0f);
  NEED(rig.lights(roles));
  CHECK(roles[12 + 2] == PadRole::Off);
  CHECK(roles[24 + 2] == PadRole::Front);  // the top lit pad
  CHECK(roles[36 + 2] == PadRole::Back);
  rig.tap_pad(36 + 2);  // row 4: 25
  CHECK(level() == 25.0f);
  rig.tap_pad(36 + 2);  // again: 0
  CHECK(level() == 0.0f);
  NEED(rig.lights(roles));
  CHECK(roles[36 + 2] == PadRole::Off);
  CHECK(rig.app->invoke(core::action_id("edit.undo"), 0) == app::DispatchResult::Changed);
  CHECK(level() == 25.0f);  // one undo unit a tap
}

TEST_CASE("STEP has no key: the PADS picker's F1..F6 pick the other modes, Shift + F1 is the octave (OS-STEPPADOFF, OS-PADOCT, OS-UXF-PAT 7)") {
  Rig rig;
  rig.shift_tap(Button::F1);  // the octave leaves the mode as it is: Notes
  CHECK(rig.state().padmode.fixed() == app::PadMode::Notes);
  const app::PadMode expect[6] = {app::PadMode::Notes, app::PadMode::Piano, app::PadMode::Iso, app::PadMode::Tracks, app::PadMode::Patterns, app::PadMode::Vu};
  for (unsigned n = 0; n < 6; ++n) {
    rig.shift_tap(static_cast<Button>(static_cast<unsigned>(Button::F2) + n));  // Shift + F2, then the picker's F(n+1)
    CHECK_MESSAGE(rig.state().padmode.fixed() == expect[n], "picker F" << n + 1);
    CHECK(rig.state().padmode.fixed() != app::PadMode::Steps);
    CHECK(rig.state().overlay == app::OverlayId::None);  // a pick closes the picker
    CHECK(rig.state().notify.empty());
  }
}

TEST_CASE("PADS picker: Shift + F2 opens it over Pattern and changes nothing; F8 closes it with the mode kept; F7 is no key of it; a pick closes it (OS-UXF-PAT 7)") {
  Rig rig;
  rig.shift_tap(Button::F4);  // ISO (through the picker)
  NEED(rig.state().padmode.fixed() == app::PadMode::Iso);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F2);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().overlay == app::OverlayId::PadsPicker);
  CHECK(rig.state().padmode.fixed() == app::PadMode::Iso);
  rig.tap(Button::F8);  // BACK
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.state().padmode.fixed() == app::PadMode::Iso);
  // F7 is not a key of the picker: it stays open and the mode is unchanged
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F2);
  rig.release(Button::Shift);
  rig.pass(1);
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::PadsPicker);
  CHECK(rig.state().padmode.fixed() == app::PadMode::Iso);
  rig.tap(Button::F6);  // VU
  CHECK(rig.state().padmode.fixed() == app::PadMode::Vu);
  CHECK(rig.state().overlay == app::OverlayId::None);
  // the key row of the picker names the keys; the Shift row of Pattern says PADS on F2 and is blank on F3..F7
  CHECK(std::strcmp(app::kPatternShiftKeys[0], "OCTAVE") == 0);
  CHECK(std::strcmp(app::kPatternShiftKeys[1], "PADS") == 0);
  for (unsigned n = 2; n < 7; ++n) {
    CHECK(std::strcmp(app::kPatternShiftKeys[n], "-") == 0);
  }
  CHECK(std::strcmp(app::kPatternShiftKeys[7], "REDO") == 0);
}

TEST_CASE("OCTAVE: Shift + F1 cycles the pads' base C through the octaves and wraps, the label says it, the mode stays (OS-PADOCT)") {
  Rig rig;
  CHECK(rig.state().edit.entry.octave == 3);
  CHECK(std::strcmp(app::kPatternScreen.shift_row(rig.state())[0], "C3") == 0);
  rig.shift_tap(Button::F1);
  CHECK(rig.state().edit.entry.octave == 4);
  CHECK(std::strcmp(app::kPatternScreen.shift_row(rig.state())[0], "C4") == 0);
  CHECK(rig.state().padmode.fixed() == app::PadMode::Notes);
  CHECK(rig.state().edit.pads.note(36) == 48);  // the lowest NOTES pad is the C of the octave
  for (unsigned n = 0; n < 5; ++n) {
    rig.shift_tap(Button::F1);
  }
  CHECK(rig.state().edit.entry.octave == 9);
  CHECK(std::strcmp(app::kPatternScreen.shift_row(rig.state())[0], "C9") == 0);
  rig.shift_tap(Button::F1);  // wraps at the end
  CHECK(rig.state().edit.entry.octave == 0);
  CHECK(std::strcmp(app::kPatternScreen.shift_row(rig.state())[0], "C0") == 0);
  CHECK(rig.state().edit.pads.note(36) == 0);
  CHECK(rig.state().notify.empty());
  // the other rows of the Shift picker are unchanged by the octave
  CHECK(std::strcmp(app::kPatternScreen.shift_row(rig.state())[7], "REDO") == 0);
  CHECK(std::strcmp(app::kPatternScreen.shift_row(rig.state())[1], "PADS") == 0);
}

TEST_CASE("OCTAVE: it moves the PIANO and ISO pads too, and the octave keys of PIANO and the held-Note panel are the same number (OS-PADOCT)") {
  Rig rig;
  rig.shift_tap(Button::F3);  // PIANO
  NEED(rig.state().padmode.fixed() == app::PadMode::Piano);
  rig.tap_pad(37);  // C of the lower octave
  CHECK(rig.state().edit.last_note == 36);
  rig.shift_tap(Button::F1);
  CHECK(rig.state().padmode.fixed() == app::PadMode::Piano);
  rig.tap_pad(37);
  CHECK(rig.state().edit.last_note == 48);
  rig.tap_pad(11);  // PIANO's octave-up pad: octave 5
  CHECK(rig.state().edit.entry.octave == 5);
  rig.shift_tap(Button::F4);  // ISO: the bottom left pad is the C of the octave
  NEED(rig.state().padmode.fixed() == app::PadMode::Iso);
  rig.tap_pad(36);
  CHECK(rig.state().edit.last_note == 60);
  rig.shift_tap(Button::F1);
  rig.tap_pad(36);
  CHECK(rig.state().edit.last_note == 72);
  // the held-Note panel's OCTAVE row (pads 36..45 = octaves 0..9) lands on the same octave
  rig.hold_open(Button::Note);
  rig.tap_pad(36 + 2);
  rig.let_go(Button::Note);
  CHECK(rig.state().edit.entry.octave == 2);
  CHECK(std::strcmp(app::kPatternScreen.shift_row(rig.state())[0], "C2") == 0);
}
