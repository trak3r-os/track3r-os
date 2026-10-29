// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011a: the Config screen on the App: four pages (MIDI, DISPLAY, SD, INFO), the device settings pages generated from the descriptors and edited through the
// parameter actions (no project needed), the THEME setting and the screen's theme following each other, the SD and Info pages' facts. The descriptors and the settings
// file are param_test.cpp and settings_format_test.cpp.
#include <doctest/doctest.h>

#include "support/key_row.h"
#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/param_view.h>
#include <o3os/app/pattern_edit.h>
#include <o3os/app/screens/config_screen.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fake_storage.h>
#include <o3os/platform/host/fakes.h>

#include <memory>
#include <string>

using namespace o3os;
using app::Button;
using app::ScreenId;
namespace ds = model::device_setting;

namespace {

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  platform::host::FakeStorage card;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  Rig() {
    app->attach_storage(&card);
    app->set_build_info("Track3r test", "0123456789ab");
    tap(Button::Config);
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void tap(Button b) {
    press(b);
    release(b);
    pass();
  }
  void shift_tap(Button b) {
    press(Button::Shift);
    pass(1);
    press(b);
    release(b);
    pass();
    release(Button::Shift);
    pass(1);
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass();
  }
  unsigned page() const { return app->state().page[app::screen_index(ScreenId::Config)]; }
  int row() const { return app->state().params.row; }
  float get(unsigned field) const { return app->device_settings().get(field); }
  std::uint32_t generation(unsigned field) const { return app->device_settings().generation(field); }
  const app::ParamView& params() const { return app->state().params; }
  const app::PendingParam* pending(unsigned field) const { return params().find_pending(core::param_id(core::ParamKind::Device, 0, field)); }
  std::string last_news() const { return app->state().notify.last_text(); }
};

}  // namespace

TEST_CASE("config: it is a screen of the registry, reached by its button; the button again goes through the six pages, F1..F6 pick one; F7 is APPLY") {
  Rig rig;
  const app::ScreenEntry* e = app::find_screen(app::screen_registry(), ScreenId::Config);
  NEED(e != nullptr);
  CHECK(e->screen == &app::kConfigScreen);
  CHECK(rig.app->state().screen == ScreenId::Config);
  CHECK(rig.page() == 0u);
  for (unsigned expect : {1u, 2u, 3u, 4u, 5u, 0u}) {
    rig.tap(Button::Config);
    CHECK(rig.page() == expect);
  }
  rig.tap(Button::F3);
  CHECK(rig.page() == 2u);
  rig.tap(Button::F2);
  CHECK(rig.page() == 1u);
  rig.tap(Button::F5);
  CHECK(rig.page() == 4u);
  rig.tap(Button::F6);
  CHECK(rig.page() == 5u);
  CHECK(rig.app->frames() >= 1u);
  CHECK(std::string(app::kConfigSoftKeys[6]) == "APPLY");
  rig.tap(Button::F7);  // (APPLY is not a page)
  CHECK(rig.page() == 5u);
}

// OS-LIVEREC: the PATTERN page (device group 3) holds the two settings of the live recording.
TEST_CASE("config: the PATTERN page has REC POLY and REC QUANTIZE; the encoder edits the row, Delete puts the default back, both are learnable") {
  Rig rig;
  rig.tap(Button::F6);
  CHECK(app::kConfigScreen.count(rig.app->model(), rig.app->state()) == 9);  // REC POLY, REC QUANTIZE, and since OS-RECMICRO REC MICRO, REC VELOCITY, and since OS-METRO METRO, METRO VOL, COUNT-IN, and since OS-PADOFFSET PAD OFFSET (the tests below)
  CHECK(app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()) == core::param_id(core::ParamKind::Device, 0, ds::RecPoly));
  CHECK(rig.get(ds::RecPoly) == 0.0f);  // Off
  rig.turn(1);
  CHECK(rig.get(ds::RecPoly) == 1.0f);  // Spill
  rig.turn(5);
  CHECK(rig.get(ds::RecPoly) == 1.0f);  // two choices: the last
  rig.tap(Button::Delete);
  CHECK(rig.get(ds::RecPoly) == 0.0f);
  rig.tap(Button::Down);
  CHECK(app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()) == core::param_id(core::ParamKind::Device, 0, ds::RecQuantize));
  CHECK(rig.get(ds::RecQuantize) == 0.0f);  // Nearest step
  rig.turn(1);
  CHECK(rig.get(ds::RecQuantize) == 1.0f);  // Playing step
  rig.tap(Button::Delete);
  CHECK(rig.get(ds::RecQuantize) == 0.0f);
  for (const unsigned field : {ds::RecPoly, ds::RecQuantize}) {
    const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Device, 0, field));
    NEED(d != nullptr);
    CHECK((d->flags & core::kParamLearnable) != 0);
    CHECK(model::param_group_name(core::ParamKind::Device, d->group) == std::string("PATTERN"));
  }
  rig.tap(Button::F1);  // another page starts at its first row
  CHECK(rig.row() == 0);
}

// OS-RECMICRO: REC MICRO and REC VELOCITY follow REC QUANTIZE (the PATTERN page stays within the eight rows a page may have).
TEST_CASE("config: the PATTERN page has REC MICRO and REC VELOCITY after REC QUANTIZE; the encoder edits them, Delete puts the default back, both are learnable") {
  Rig rig;
  rig.tap(Button::F6);
  const auto focus = [&] { return app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()); };
  const auto id = [](unsigned field) { return core::param_id(core::ParamKind::Device, 0, field); };
  CHECK(app::kConfigScreen.count(rig.app->model(), rig.app->state()) <= 9);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.row() == 2);
  CHECK(focus() == id(ds::RecMicro));
  CHECK(rig.get(ds::RecMicro) == 0.0f);  // Off
  rig.turn(1);
  CHECK(rig.get(ds::RecMicro) == 1.0f);  // On
  rig.turn(5);
  CHECK(rig.get(ds::RecMicro) == 1.0f);  // two choices: the last
  rig.tap(Button::Delete);
  CHECK(rig.get(ds::RecMicro) == 0.0f);
  rig.tap(Button::Down);
  CHECK(focus() == id(ds::RecVelocity));
  CHECK(rig.get(ds::RecVelocity) == 0.0f);  // Off
  rig.turn(1);
  CHECK(rig.get(ds::RecVelocity) == 1.0f);  // External
  rig.tap(Button::Delete);
  CHECK(rig.get(ds::RecVelocity) == 0.0f);
  rig.tap(Button::Down);
  CHECK(focus() == id(ds::MetroMode));  // the metronome's rows follow
}

// OS-METRO: the three metronome rows follow the two of the live recording, in the order of the spec (METRO VOL is the table's row 2, but the page shows it after METRO); the encoder edits them,
// Delete puts the default back, all three are learnable, and METRO VOL is gone from the MIDI page.
TEST_CASE("config: the PATTERN page has METRO, METRO VOL and COUNT-IN after the live recording's rows; the encoder edits them, Delete puts the default back, with no project open") {
  Rig rig;
  rig.tap(Button::F6);
  const auto focus = [&] { return app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()); };
  const auto id = [](unsigned field) { return core::param_id(core::ParamKind::Device, 0, field); };
  for (int i = 0; i < 4; ++i) {  // past REC POLY, REC QUANTIZE, REC MICRO and REC VELOCITY
    rig.tap(Button::Down);
  }
  CHECK(rig.row() == 4);
  CHECK(focus() == id(ds::MetroMode));
  CHECK(rig.get(ds::MetroMode) == 1.0f);  // Cue, the default
  rig.turn(1);
  CHECK(rig.get(ds::MetroMode) == 2.0f);  // Main
  rig.turn(5);
  CHECK(rig.get(ds::MetroMode) == 2.0f);  // three choices: the last
  rig.turn(-9);
  CHECK(rig.get(ds::MetroMode) == 0.0f);  // Off: the first
  rig.tap(Button::Delete);
  CHECK(rig.get(ds::MetroMode) == 1.0f);
  rig.tap(Button::Down);
  CHECK(focus() == id(ds::MetronomeVolume));
  CHECK(rig.get(ds::MetronomeVolume) == 50.0f);
  rig.turn(5);
  CHECK(rig.get(ds::MetronomeVolume) == 55.0f);
  rig.turn(500);
  CHECK(rig.get(ds::MetronomeVolume) == 100.0f);
  rig.tap(Button::Delete);
  CHECK(rig.get(ds::MetronomeVolume) == 50.0f);
  rig.tap(Button::Down);
  CHECK(focus() == id(ds::CountIn));
  CHECK(rig.get(ds::CountIn) == 0.0f);  // Off, the default
  rig.turn(1);
  CHECK(rig.get(ds::CountIn) == 1.0f);  // 1 bar
  rig.turn(5);
  CHECK(rig.get(ds::CountIn) == 2.0f);  // 2 bars: the last
  rig.tap(Button::Delete);
  CHECK(rig.get(ds::CountIn) == 0.0f);
  rig.tap(Button::Down);
  CHECK(rig.row() == 7);  // PAD OFFSET follows (the next test)
  for (const unsigned field : {ds::MetroMode, ds::MetronomeVolume, ds::CountIn}) {
    const core::ParamDesc* d = model::param_desc(id(field));
    NEED(d != nullptr);
    CHECK((d->flags & core::kParamLearnable) != 0);
    CHECK(model::param_group_name(core::ParamKind::Device, d->group) == std::string("PATTERN"));
  }
  // The MIDI page lost the row.
  rig.tap(Button::F1);
  CHECK(app::kConfigScreen.count(rig.app->model(), rig.app->state()) == 9);  // 7 before E-CFG-03 added TRANSPORT IN and OUT
  std::uint16_t fields[12];
  const unsigned n = model::param_group_fields(core::ParamKind::Device, 0, 0, fields, 12);
  for (unsigned i = 0; i < n; ++i) {
    CHECK(fields[i] != ds::MetronomeVolume);
  }
}

// OS-PADOFFSET: PAD OFFSET is the PATTERN page's last row, a device setting (0..12, default 12 = the layout before the row). The NOTES board takes it at once, keeping its scale and root.
TEST_CASE("config: the PATTERN page ends with PAD OFFSET; the encoder moves it 0..12, Delete puts 12 back, the NOTES pads are laid out again at once, a loaded setting does the same") {
  Rig rig;
  rig.tap(Button::F6);
  const auto focus = [&] { return app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()); };
  const app::PadBoard& pads = rig.app->state().edit.pads;
  CHECK(pads.offset() == 12u);  // the board before the row, and the setting's default
  CHECK(pads.note(24) == 48u);  // row 2 of the pads continues right after row 1 (36..47)
  for (int i = 0; i < 7; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.row() == 7);
  CHECK(focus() == core::param_id(core::ParamKind::Device, 0, ds::PadOffset));
  CHECK(rig.get(ds::PadOffset) == 12.0f);
  rig.turn(1);
  CHECK(rig.get(ds::PadOffset) == 12.0f);  // 12 is the largest
  rig.turn(-1);
  CHECK(rig.get(ds::PadOffset) == 11.0f);
  CHECK(pads.offset() == 11u);
  CHECK(pads.note(24) == 47u);  // row 2 starts on row 1's last note
  CHECK(pads.note(36) == 36u);
  CHECK((pads.scale() == 0u && pads.root() == 36u));  // the scale and the root are the entry settings'
  rig.turn(-50);
  CHECK(rig.get(ds::PadOffset) == 0.0f);
  CHECK(pads.offset() == 0u);
  CHECK((pads.note(24) == 36u && pads.note(12) == 36u && pads.note(0) == 36u));  // every row starts where the one below does
  rig.tap(Button::Delete);
  CHECK(rig.get(ds::PadOffset) == 12.0f);
  CHECK(pads.offset() == 12u);
  CHECK(pads.note(24) == 48u);
  rig.turn(-3);
  CHECK(rig.get(ds::PadOffset) == 9.0f);
  CHECK(pads.offset() == 9u);
  // A settings file loaded by the platform does the same (assign advances the generation), and the note entry's apply_entry keeps the offset.
  model::Settings loaded = rig.app->device_settings();
  CHECK(loaded.set(ds::PadOffset, 5.0f) == model::Status::Ok);
  CHECK(rig.app->device_settings().assign(loaded) == model::Status::Ok);
  rig.pass();
  CHECK(pads.offset() == 5u);
  app::PatternEdit edit;
  edit.pads.set(0, 5, 36);
  edit.entry.octave = 4;
  edit.apply_entry();
  CHECK(edit.pads.offset() == 5u);  // (a project load, an octave or a root change lay the board out again with the offset in force)
  const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::PadOffset));
  NEED(d != nullptr);
  CHECK(std::string(d->name) == "PAD OFFSET");
  CHECK((d->flags & core::kParamLearnable) != 0);
  CHECK((d->min == 0.0f && d->max == 12.0f && d->def == 12.0f));
  CHECK(model::param_group_name(core::ParamKind::Device, d->group) == std::string("PATTERN"));
}

// OS-QAFIX-7 (finding 7): SD and INFO have no row to apply and render() draws no F7 label there, so the key does nothing and says nothing (it used to answer NOTHING TO APPLY to an unlabelled key).
TEST_CASE("config: F7 on the SD and INFO pages says nothing") {
  Rig rig;
  for (const Button page : {Button::F3, Button::F4}) {
    rig.tap(page);
    const std::string before = rig.last_news();
    rig.tap(Button::F7);
    CHECK(rig.last_news() == before);
  }
}

// OS-BUG-APPLYSILENT: the rule of OS-QAFIX-7 over every state of F7 (the random walk reported it five times on CONFIG): on every page and every row that draws an F7 label, a press posts news (first press, second
// press, with or without a pending value, and after a turn); the key is silent on exactly the pages that draw no label (SD and INFO).
TEST_CASE("config: F7 answers on every labelled page and row, in every state") {
  Rig rig;
  const Button pages[] = {Button::F1, Button::F2, Button::F3, Button::F4, Button::F5, Button::F6};
  for (unsigned p = 0; p < 6; ++p) {
    rig.tap(pages[p]);
    const bool labelled = p != 2 && p != 3;  // SD and INFO: no row, no label (OS-QAFIX-7)
    int rows = 0;
    int last = -1;
    for (int guard = 0; guard < 24; ++guard) {
      if (rig.row() == last && guard != 0) {
        break;
      }
      last = rig.row();
      ++rows;
      for (int state = 0; state < 3; ++state) {
        if (state == 1) {
          rig.turn(1);  // a turn: a pending value on a USB row, a live write elsewhere
        } else if (state == 2) {
          rig.tap(Button::F7);  // the second press follows an armed first one
        }
        for (int press = 0; press < 2; ++press) {
          const unsigned posts = rig.app->state().notify.posts();
          rig.tap(Button::F7);
          INFO("page " << p << " row " << rig.row() << " state " << state << " press " << press);
          CHECK((rig.app->state().notify.posts() != posts) == labelled);
        }
      }
      rig.tap(Button::Down);
    }
    CHECK(rows > 0);
  }
}

// OS-USBAUDIO: the USB page has USB AUDIO above USB RETURN above RETURN LEVEL. The first two re-enumerate the USB port, so they are press to apply: the encoder (and Delete) only change a
// pending value, shown starred; F7 says "USB WILL RECONNECT: PRESS AGAIN" and the second press writes the settings; a move of the cursor takes the first press back; leaving the page throws the
// pending values away. RETURN LEVEL stays live.
TEST_CASE("config: the USB page has USB AUDIO first; a turn only changes a pending value, the settings are written by the second press of F7, once") {
  Rig rig;
  rig.tap(Button::F5);
  NEED(rig.page() == 4u);
  CHECK(app::kConfigScreen.count(rig.app->model(), rig.app->state()) == 5);  // + CARD AS DISK and READ ONLY (OS-USBDISK U2b)
  const auto focus = [&] { return app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()); };
  const auto id = [](unsigned f) { return core::param_id(core::ParamKind::Device, 0, f); };
  CHECK(focus() == id(ds::UsbAudio));  // the first row, above USB RETURN
  CHECK(rig.get(ds::UsbAudio) == 1.0f);  // On, the default
  const std::uint32_t g_audio = rig.generation(ds::UsbAudio);
  const std::uint32_t g_return = rig.generation(ds::UsbReturn);

  // A turn alone changes nothing the unit does.
  rig.turn(-1);
  CHECK(rig.get(ds::UsbAudio) == 1.0f);
  CHECK(rig.generation(ds::UsbAudio) == g_audio);
  NEED(rig.pending(ds::UsbAudio) != nullptr);
  CHECK(rig.pending(ds::UsbAudio)->value == 0.0f);  // Off, waiting
  CHECK(rig.params().pending_n == 1u);
  CHECK_FALSE(rig.params().armed);
  rig.turn(-3);  // (the end of two choices)
  CHECK(rig.pending(ds::UsbAudio)->value == 0.0f);
  rig.turn(-1);
  rig.turn(-1);
  CHECK(rig.generation(ds::UsbAudio) == g_audio);
  CHECK(rig.params().pending_n == 1u);

  // The first press says what will happen and arms; nothing is written.
  rig.tap(Button::F7);
  CHECK(rig.params().armed);
  CHECK(rig.last_news() == "USB WILL RECONNECT: PRESS AGAIN");
  CHECK(test::read_row(*rig.app).label[6] == "APPLY?");  // OS-KEYROWDLG: the second F7 is marked, inverted
  CHECK(test::read_row(*rig.app).hot == 0x40u);
  CHECK(rig.get(ds::UsbAudio) == 1.0f);
  CHECK(rig.generation(ds::UsbAudio) == g_audio);
  // The second writes it: once, and the pending value and the arming are gone.
  rig.tap(Button::F7);
  CHECK(rig.get(ds::UsbAudio) == 0.0f);
  CHECK(rig.generation(ds::UsbAudio) == g_audio + 1u);
  CHECK(rig.generation(ds::UsbReturn) == g_return);  // (the other row was not touched)
  CHECK(rig.params().pending_n == 0u);
  CHECK_FALSE(rig.params().armed);
  // Nothing is left to apply: another press writes nothing, and says so.
  rig.tap(Button::F7);
  rig.tap(Button::F7);
  CHECK(rig.generation(ds::UsbAudio) == g_audio + 1u);
  CHECK(rig.last_news() == "NOTHING TO APPLY");
  CHECK(rig.get(ds::UsbAudio) == 0.0f);

  // Back to the value in force: nothing pending (the star goes away), and APPLY has nothing to do.
  rig.turn(1);
  CHECK(rig.pending(ds::UsbAudio) != nullptr);
  rig.turn(-1);
  CHECK(rig.pending(ds::UsbAudio) == nullptr);
  CHECK(rig.params().pending_n == 0u);
}

// OS-USBAUDIO: the messages say a reconnection only when the apply will cause one. USB AUDIO always does (the mode changes); USB RETURN only while AUDIO is presented (STANDARD's descriptors do not
// depend on it); a write that comes back Unchanged does not.
TEST_CASE("config: APPLY says the USB port reconnects only when it will: a lone USB RETURN change in STANDARD does not, in AUDIO it does, a write that changes nothing does not") {
  Rig rig;
  rig.tap(Button::F5);
  // A lone USB RETURN change with USB AUDIO On (AUDIO is presented): a reconnection.
  rig.tap(Button::Down);
  rig.turn(-1);
  NEED(rig.pending(ds::UsbReturn) != nullptr);
  rig.tap(Button::F7);
  CHECK(rig.last_news() == "USB WILL RECONNECT: PRESS AGAIN");
  rig.tap(Button::F7);
  CHECK(rig.get(ds::UsbReturn) == 0.0f);
  CHECK(rig.last_news() == "USB RECONNECTING");
  // USB AUDIO to Off: a reconnection (the mode changes), whatever USB RETURN does.
  rig.tap(Button::Up);
  rig.turn(-1);
  rig.tap(Button::F7);
  CHECK(rig.last_news() == "USB WILL RECONNECT: PRESS AGAIN");
  rig.tap(Button::F7);
  CHECK(rig.get(ds::UsbAudio) == 0.0f);
  CHECK(rig.last_news() == "USB RECONNECTING");
  // A lone USB RETURN change in STANDARD: nothing re-enumerates, and nothing says so.
  rig.tap(Button::Down);
  rig.turn(1);
  NEED(rig.pending(ds::UsbReturn) != nullptr);
  rig.tap(Button::F7);
  CHECK(rig.last_news() == "PRESS AGAIN TO APPLY");
  rig.tap(Button::F7);
  CHECK(rig.get(ds::UsbReturn) == 1.0f);
  CHECK(rig.last_news() == "APPLIED");
  // Both together, with USB AUDIO going On: the mode changes, so a reconnection.
  rig.tap(Button::Up);
  rig.turn(1);
  rig.tap(Button::Down);
  rig.turn(-1);
  rig.tap(Button::F7);
  CHECK(rig.last_news() == "USB WILL RECONNECT: PRESS AGAIN");
  rig.tap(Button::F7);
  CHECK(rig.get(ds::UsbAudio) == 1.0f);
  CHECK(rig.get(ds::UsbReturn) == 0.0f);
  CHECK(rig.last_news() == "USB RECONNECTING");
  // The setting reached the chosen value by another way before the second press: the write changes nothing, and no reconnection is announced.
  rig.tap(Button::Up);
  rig.turn(-1);
  NEED(rig.pending(ds::UsbAudio) != nullptr);
  rig.tap(Button::F7);
  CHECK(rig.app->device_settings().set(ds::UsbAudio, 0.0f) == model::Status::Ok);
  rig.tap(Button::F7);
  CHECK(rig.get(ds::UsbAudio) == 0.0f);
  CHECK(rig.last_news() == "APPLIED");
}

TEST_CASE("config: leaving the USB page or the screen discards the pending values; a turn or a move of the cursor takes the first press back") {
  Rig rig;
  rig.tap(Button::F5);
  rig.turn(-1);  // USB AUDIO pending Off
  NEED(rig.pending(ds::UsbAudio) != nullptr);
  rig.tap(Button::Down);  // USB RETURN
  rig.turn(-1);
  NEED(rig.pending(ds::UsbReturn) != nullptr);
  CHECK(rig.params().pending_n == 2u);  // a move of the cursor within the page keeps them
  const std::uint32_t g_audio = rig.generation(ds::UsbAudio);
  const std::uint32_t g_return = rig.generation(ds::UsbReturn);

  // Another page (F1), and back: both are gone, and nothing was applied.
  rig.tap(Button::F1);
  CHECK(rig.params().pending_n == 0u);
  rig.tap(Button::F5);
  CHECK(rig.pending(ds::UsbAudio) == nullptr);
  CHECK(rig.pending(ds::UsbReturn) == nullptr);
  CHECK(rig.get(ds::UsbAudio) == 1.0f);
  CHECK(rig.get(ds::UsbReturn) == 1.0f);
  // The same page again (F5 on F5) is not leaving it.
  rig.turn(-1);
  NEED(rig.pending(ds::UsbAudio) != nullptr);
  rig.tap(Button::F5);
  CHECK(rig.pending(ds::UsbAudio) != nullptr);
  // The Config button cycles the pages: leaving.
  rig.tap(Button::Config);
  CHECK(rig.params().pending_n == 0u);
  rig.tap(Button::F5);
  // Another screen (Pattern) and back.
  rig.turn(-1);
  NEED(rig.pending(ds::UsbAudio) != nullptr);
  rig.tap(Button::Pattern);
  CHECK(rig.params().pending_n == 0u);
  rig.tap(Button::Config);
  CHECK(rig.page() == 4u);
  CHECK(rig.pending(ds::UsbAudio) == nullptr);
  // An F7 after leaving applies nothing.
  rig.tap(Button::F7);
  rig.tap(Button::F7);
  CHECK(rig.generation(ds::UsbAudio) == g_audio);
  CHECK(rig.generation(ds::UsbReturn) == g_return);

  // The first press, then a turn, a move or a reset: the arming is taken back, so the next press arms again and the one after applies.
  rig.turn(-1);
  rig.tap(Button::F7);
  NEED(rig.params().armed);
  rig.turn(1);  // back to On: no pending
  CHECK_FALSE(rig.params().armed);
  rig.turn(-1);
  rig.tap(Button::F7);
  CHECK(rig.params().armed);
  rig.tap(Button::Down);  // a move of the cursor
  CHECK_FALSE(rig.params().armed);
  rig.tap(Button::Up);
  rig.tap(Button::F7);  // arms again (the first press of this round) ...
  CHECK(rig.params().armed);
  CHECK(rig.generation(ds::UsbAudio) == g_audio);  // ... and has written nothing
  rig.tap(Button::F7);
  CHECK(rig.get(ds::UsbAudio) == 0.0f);
  CHECK(rig.generation(ds::UsbAudio) == g_audio + 1u);
}

TEST_CASE("config: USB AUDIO and USB RETURN are applied together by one confirmed press; Delete is the default as a pending value; RETURN LEVEL is live") {
  Rig rig;
  rig.tap(Button::F5);
  rig.turn(-1);           // USB AUDIO -> Off
  rig.tap(Button::Down);
  rig.turn(-1);           // USB RETURN -> Off
  rig.tap(Button::Down);  // RETURN LEVEL
  CHECK(app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()) == core::param_id(core::ParamKind::Device, 0, ds::UsbReturnLevel));
  const std::uint32_t g_level = rig.generation(ds::UsbReturnLevel);
  rig.turn(-10);  // live: written at once, no pending, no APPLY
  CHECK(rig.get(ds::UsbReturnLevel) == 90.0f);
  CHECK(rig.generation(ds::UsbReturnLevel) == g_level + 1u);
  CHECK(rig.pending(ds::UsbReturnLevel) == nullptr);
  CHECK(rig.params().pending_n == 2u);  // (it did not disturb the others)
  CHECK(rig.get(ds::UsbAudio) == 1.0f);
  CHECK(rig.get(ds::UsbReturn) == 1.0f);
  rig.tap(Button::Delete);  // RETURN LEVEL back to unity, live
  CHECK(rig.get(ds::UsbReturnLevel) == 100.0f);

  const std::uint32_t g_audio = rig.generation(ds::UsbAudio);
  const std::uint32_t g_return = rig.generation(ds::UsbReturn);
  rig.tap(Button::F7);
  CHECK(rig.params().armed);
  rig.tap(Button::F7);
  CHECK(rig.get(ds::UsbAudio) == 0.0f);
  CHECK(rig.get(ds::UsbReturn) == 0.0f);
  CHECK(rig.generation(ds::UsbAudio) == g_audio + 1u);
  CHECK(rig.generation(ds::UsbReturn) == g_return + 1u);
  CHECK(rig.params().pending_n == 0u);

  // Delete on a press-to-apply row is the default as a pending value: USB AUDIO is Off, so On waits; the second press applies it.
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  CHECK(app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()) == core::param_id(core::ParamKind::Device, 0, ds::UsbAudio));
  rig.tap(Button::Delete);
  NEED(rig.pending(ds::UsbAudio) != nullptr);
  CHECK(rig.pending(ds::UsbAudio)->value == 1.0f);
  CHECK(rig.get(ds::UsbAudio) == 0.0f);
  rig.tap(Button::F7);
  rig.tap(Button::F7);
  CHECK(rig.get(ds::UsbAudio) == 1.0f);
  CHECK(rig.get(ds::UsbReturn) == 0.0f);  // (the other stays as it was)
  CHECK(rig.generation(ds::UsbAudio) == g_audio + 2u);
  CHECK(rig.generation(ds::UsbReturn) == g_return + 1u);
  // Delete at the default is no change and no pending value.
  rig.tap(Button::Delete);
  CHECK(rig.params().pending_n == 0u);
}

TEST_CASE("config: the USB page draws the pending value starred, the armed prompt and the card of each row, in every theme") {
  for (const ui::ThemeId theme : {ui::ThemeId::Amiga, ui::ThemeId::Black, ui::ThemeId::White}) {
    Rig rig;
    rig.app->set_theme(theme);
    rig.tap(Button::F5);
    const std::uint32_t before = rig.app->frames();
    rig.turn(-1);
    CHECK(rig.app->frames() > before);  // a pending value is a change to draw
    rig.tap(Button::F7);
    CHECK(rig.app->frames() > before);
    rig.tap(Button::Down);
    rig.tap(Button::Down);
    rig.tap(Button::Up);
    CHECK(rig.app->failed_frames() == 0u);
  }
}

TEST_CASE("config: the MIDI page has the clock, the ports and the channels; the encoder edits the row, Delete puts the default back, with no project open") {
  Rig rig;
  CHECK(app::kConfigScreen.count(rig.app->model(), rig.app->state()) == 9);  // (eight until OS-METRO moved METRO VOL to the PATTERN page, seven until E-CFG-03 added TRANSPORT IN and OUT)
  CHECK(rig.app->model().project == nullptr);
  // The rows: CLOCK IN, CLOCK SYNC, CLOCK MODE, TRANSPORT IN, TRANSPORT OUT, MIDI IN, MIDI OUT, IN CHANNEL, OUT CHANNEL.
  for (int i = 0; i < 2; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.row() == 2);
  CHECK(rig.get(ds::ClockMode) == 0.0f);
  rig.turn(1);
  CHECK(rig.get(ds::ClockMode) == 1.0f);  // Master
  rig.turn(5);
  CHECK(rig.get(ds::ClockMode) == 2.0f);  // Slave: the last
  rig.tap(Button::Delete);
  CHECK(rig.get(ds::ClockMode) == 0.0f);
  rig.tap(Button::Down);                  // TRANSPORT IN
  rig.tap(Button::Down);                  // TRANSPORT OUT
  rig.tap(Button::Down);                  // MIDI IN
  CHECK(rig.row() == 5);
  rig.turn(-1);
  CHECK(rig.get(ds::MidiInPort) == 1.0f);  // USB
  rig.tap(Button::Down);
  rig.tap(Button::Down);                  // IN CHANNEL
  rig.turn(3);
  CHECK(rig.get(ds::MidiInChannel) == 3.0f);
  rig.shift_tap(Button::Down);            // (Shift + Down is no binding here: the Cursor's Down runs)
  CHECK(rig.row() == 8);
  rig.turn(40);
  CHECK(rig.get(ds::MidiOutChannel) == 16.0f);  // 1..16, clamped
  rig.turn(-100);
  CHECK(rig.get(ds::MidiOutChannel) == 1.0f);
  // The arrows clamp at both ends.
  for (int i = 0; i < 20; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.row() == 8);
  for (int i = 0; i < 20; ++i) {
    rig.tap(Button::Up);
  }
  CHECK(rig.row() == 0);
}

TEST_CASE("config: TRANSPORT rows are on the MIDI page after CLOCK MODE; the encoder changes the device value and its generation, Delete puts On back, both are learnable") {
  Rig rig;
  const auto focus = [&] { return app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()); };
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // CLOCK MODE
  CHECK(focus() == core::param_id(core::ParamKind::Device, 0, ds::ClockMode));
  rig.tap(Button::Down);  // TRANSPORT IN
  CHECK(rig.row() == 3);
  CHECK(focus() == core::param_id(core::ParamKind::Device, 0, ds::TransportIn));
  CHECK(rig.get(ds::TransportIn) == 1.0f);  // On, the default
  std::uint32_t gen = rig.generation(ds::TransportIn);
  rig.turn(-1);
  CHECK(rig.get(ds::TransportIn) == 0.0f);
  CHECK(rig.generation(ds::TransportIn) != gen);
  CHECK(rig.get(ds::TransportOut) == 1.0f);  // the other one did not move
  rig.turn(-5);
  CHECK(rig.get(ds::TransportIn) == 0.0f);  // Off is the first
  rig.tap(Button::Delete);
  CHECK(rig.get(ds::TransportIn) == 1.0f);
  rig.tap(Button::Down);  // TRANSPORT OUT
  CHECK(rig.row() == 4);
  CHECK(focus() == core::param_id(core::ParamKind::Device, 0, ds::TransportOut));
  gen = rig.generation(ds::TransportOut);
  const std::uint32_t gen_in = rig.generation(ds::TransportIn);
  rig.turn(-1);
  CHECK(rig.get(ds::TransportOut) == 0.0f);
  CHECK(rig.generation(ds::TransportOut) != gen);
  CHECK(rig.generation(ds::TransportIn) == gen_in);
  CHECK(rig.get(ds::TransportIn) == 1.0f);
  rig.turn(3);
  CHECK(rig.get(ds::TransportOut) == 1.0f);  // On is the last
  // the rows after them are the ports
  rig.tap(Button::Down);
  CHECK(focus() == core::param_id(core::ParamKind::Device, 0, ds::MidiInPort));
  for (const unsigned id : {static_cast<unsigned>(ds::TransportIn), static_cast<unsigned>(ds::TransportOut)}) {
    const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Device, 0, id));
    NEED(d != nullptr);
    CHECK((d->flags & core::kParamLearnable) != 0);
    CHECK(model::param_group_name(core::ParamKind::Device, d->group) == std::string("MIDI"));
  }
  CHECK(std::string(model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::TransportIn))->name) == "TRANSP IN");  // the caption holds 12 characters
  CHECK(std::string(model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::TransportOut))->name) == "TRANSP OUT");
  // The page has nine rows (ParamView::kConfigRows; the data pane holds sixteen): the cursor reaches the last.
  rig.tap(Button::F1);
  for (int i = 0; i < 8; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.row() == 8);
  CHECK(focus() == core::param_id(core::ParamKind::Device, 0, ds::MidiOutChannel));
}

TEST_CASE("config: a page change starts at its first row; SD and INFO have no rows and no parameter to edit") {
  Rig rig;
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.row() == 2);
  rig.tap(Button::F2);
  CHECK(rig.row() == 0);
  CHECK(app::kConfigScreen.count(rig.app->model(), rig.app->state()) == 5);  // DISPLAY, PAD LIGHTS, NUMBERING, MIDDLE C, THEME
  rig.tap(Button::F3);
  CHECK(app::kConfigScreen.count(rig.app->model(), rig.app->state()) == 0);
  CHECK(app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()) == core::ParamId::None);
  const float before = rig.get(ds::MetronomeVolume);
  rig.turn(3);
  rig.tap(Button::Delete);
  CHECK(rig.get(ds::MetronomeVolume) == before);
  rig.tap(Button::F4);
  CHECK(app::kConfigScreen.count(rig.app->model(), rig.app->state()) == 0);
}

TEST_CASE("config: the THEME row and the screen's theme follow each other: the row, Shift + Master, the App's own set_theme and a settings file loaded into the block") {
  Rig rig;
  rig.tap(Button::F2);
  for (int i = 0; i < 4; ++i) {
    rig.tap(Button::Down);  // THEME is the fifth row of the DISPLAY page
  }
  NEED(app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()) == core::param_id(core::ParamKind::Device, 0, ds::Theme));
  CHECK(rig.app->theme() == ui::ThemeId::Amiga);
  rig.turn(1);
  CHECK(rig.get(ds::Theme) == 1.0f);
  CHECK(rig.app->theme() == ui::ThemeId::Black);
  rig.shift_tap(Button::Master);  // Shift + Master: the next theme, and the setting says so
  CHECK(rig.app->theme() == ui::ThemeId::White);
  CHECK(rig.get(ds::Theme) == 2.0f);
  rig.app->set_theme(ui::ThemeId::Amiga);
  CHECK(rig.get(ds::Theme) == 0.0f);
  rig.pass();
  CHECK(rig.app->theme() == ui::ThemeId::Amiga);
  // The platform loads the settings file into the block: the colours follow on the next pass.
  CHECK(rig.app->device_settings().set(ds::Theme, 2.0f) == model::Status::Ok);
  rig.pass();
  CHECK(rig.app->theme() == ui::ThemeId::White);
  const std::uint32_t frames = rig.app->frames();
  rig.pass();
  rig.pass();
  CHECK(rig.app->frames() == frames);  // and nothing is redrawn without a reason
}

TEST_CASE("config: every page draws, in every theme, with and without a card, a build id and sample memory") {
  for (const ui::ThemeId theme : {ui::ThemeId::Amiga, ui::ThemeId::Black, ui::ThemeId::White}) {
    Rig rig;
    rig.app->set_theme(theme);
    for (unsigned p = 1; p <= 6; ++p) {  // F2, F3, F4, F5, F6, then F1: each is another page
      const std::uint32_t frames = rig.app->frames();
      rig.tap(static_cast<Button>(static_cast<unsigned>(Button::F1) + p % 6u));
      CHECK(rig.app->frames() > frames);
    }
    CHECK(rig.app->failed_frames() == 0u);
  }
  Rig bare;  // no card present, no build info
  bare.card.set_present(false);
  bare.app->set_build_info(nullptr, nullptr);
  for (unsigned p = 0; p < 6; ++p) {
    bare.tap(static_cast<Button>(static_cast<unsigned>(Button::F1) + p));
  }
  CHECK(bare.app->failed_frames() == 0u);
}

TEST_CASE("config: a random walk over the keys keeps the page and the row inside their bounds and every setting in range") {
  Rig rig;
  std::uint32_t s = 5u;
  const auto next = [&](unsigned n) {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) % n;
  };
  const Button keys[] = {Button::F1, Button::F2, Button::F3, Button::F4, Button::F5, Button::F6, Button::F7, Button::F7, Button::Up, Button::Down, Button::Left, Button::Right, Button::Delete, Button::Config};
  for (int i = 0; i < 1500; ++i) {
    if (next(3) == 0) {
      rig.turn(static_cast<int>(next(61)) - 30);
    } else {
      rig.tap(keys[next(14)]);
    }
    CHECK(rig.page() < 6u);
    CHECK((rig.row() >= 0 && rig.row() < 8));
    CHECK(rig.app->device_settings().check_invariants());
    // OS-USBAUDIO: a pending value exists only on the USB page, for a press-to-apply row, with a value of its range.
    CHECK(rig.params().pending_n <= app::ParamView::kMaxPending);
    CHECK((rig.params().pending_n == 0u || rig.page() == 4u));
    for (unsigned k = 0; k < rig.params().pending_n; ++k) {
      const core::ParamDesc* d = model::param_desc(rig.params().pending[k].id);
      NEED(d != nullptr);
      CHECK((d->flags & core::kParamApply) != 0);
      CHECK(core::param_in_range(*d, rig.params().pending[k].value));
    }
    CHECK((!rig.params().armed || rig.params().pending_n != 0u));
  }
}

// OS-SHIFTROW: Shift + F7 DEFAULTS puts every row of the page back, on a second press within two seconds; Left / Right turn the page, wrapping.
TEST_CASE("config: Shift + F7 DEFAULTS asks first and resets the page on a second press within 2 s; a late second press only asks again; other pages are not touched") {
  Rig rig;
  const auto def = [](unsigned field) { return model::param_desc(core::param_id(core::ParamKind::Device, 0, field))->def; };
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // CLOCK MODE (row 2)
  rig.turn(1);
  for (int i = 0; i < 5; ++i) {
    rig.tap(Button::Down);  // IN CHANNEL (row 7: TRANSPORT IN and OUT, E-CFG-03, and MIDI IN and OUT are between)
  }
  rig.turn(3);
  CHECK(rig.get(ds::ClockMode) == 1.0f);
  CHECK(rig.get(ds::MidiInChannel) == 3.0f);
  rig.tap(Button::F6);  // another page keeps its own value
  rig.turn(1);
  const float rec_poly = rig.get(ds::RecPoly);
  CHECK(rec_poly == 1.0f);
  rig.tap(Button::F1);
  for (int i = 0; i < 7; ++i) {
    rig.tap(Button::Down);  // IN CHANNEL
  }
  const int row = rig.row();

  rig.shift_tap(Button::F7);  // the first press only asks
  CHECK(rig.last_news() == "SHIFT+F7 AGAIN: RESET PAGE");
  CHECK(test::read_row(*rig.app, true).label[6] == "RESET?");  // OS-KEYROWDLG: the Shift layer marks the key while the question is on the strip
  CHECK(test::read_row(*rig.app, true).hot == 0x40u);
  CHECK_FALSE(test::read_row(*rig.app).marked);
  CHECK(rig.get(ds::ClockMode) == 1.0f);
  rig.shift_tap(Button::F7);  // the second resets the whole page
  CHECK(rig.get(ds::ClockMode) == def(ds::ClockMode));
  CHECK(rig.get(ds::MidiInChannel) == def(ds::MidiInChannel));
  CHECK(rig.last_news() == "PAGE RESET TO DEFAULTS");
  CHECK(rig.row() == row);  // the cursor stays where it was
  CHECK(rig.get(ds::RecPoly) == rec_poly);

  rig.shift_tap(Button::F7);  // armed again; the window of two seconds passes
  rig.turn(1);
  CHECK(rig.get(ds::MidiInChannel) != def(ds::MidiInChannel));
  rig.pass(2500);
  CHECK_FALSE(test::read_row(*rig.app, true).marked);  // its two seconds are over: the key reads DEFAULTS again
  rig.shift_tap(Button::F7);
  CHECK(rig.last_news() == "SHIFT+F7 AGAIN: RESET PAGE");  // too late: it asked again
  CHECK(rig.get(ds::MidiInChannel) != def(ds::MidiInChannel));
  rig.shift_tap(Button::F7);
  CHECK(rig.get(ds::MidiInChannel) == def(ds::MidiInChannel));

  rig.tap(Button::F3);  // SD has no rows: nothing to reset
  rig.shift_tap(Button::F7);
  CHECK(rig.last_news() == "NO ROWS ON THIS PAGE");
}

TEST_CASE("config: Shift + F7 on the USB page only moves the pending values and says F7 applies") {
  Rig rig;
  rig.tap(Button::F5);  // USB
  NEED(rig.page() == 4u);
  rig.turn(-1);
  rig.tap(Button::F7);
  rig.tap(Button::F7);  // USB AUDIO written Off
  CHECK(rig.get(ds::UsbAudio) == 0.0f);
  CHECK(rig.params().pending_n == 0u);
  rig.shift_tap(Button::F7);
  rig.shift_tap(Button::F7);
  CHECK(rig.get(ds::UsbAudio) == 0.0f);  // nothing applied
  CHECK(rig.params().pending_n != 0u);
  CHECK(rig.last_news() == "PAGE RESET: F7 TO APPLY");
}

TEST_CASE("config: a move of the cursor between the two presses of Shift + F7 takes the first back: the second asks again and resets nothing") {
  Rig rig;
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // CLOCK MODE
  rig.turn(1);
  CHECK(rig.get(ds::ClockMode) == 1.0f);
  rig.shift_tap(Button::F7);
  CHECK(rig.last_news() == "SHIFT+F7 AGAIN: RESET PAGE");
  rig.tap(Button::Down);
  rig.shift_tap(Button::F7);  // within 2 s, but the cursor moved
  CHECK(rig.last_news() == "SHIFT+F7 AGAIN: RESET PAGE");
  CHECK(rig.get(ds::ClockMode) == 1.0f);
  rig.shift_tap(Button::F7);  // now a pair without a move
  CHECK(rig.last_news() == "PAGE RESET TO DEFAULTS");
  CHECK(rig.get(ds::ClockMode) != 1.0f);
}

// OS-CFG-ARROWS (new Community OS behaviour, replaces OS-SHIFTROW rule 5 on Config): Left / Right are the encoder's detent on the row, Shift + Left / Right its coarse step; the page never turns.
TEST_CASE("config: Left / Right change the value under the cursor (Shift: the coarse step) and keep the page and the row; the F-keys and the screen button still go to a page") {
  Rig rig;
  CHECK(rig.page() == 0);
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // CLOCK MODE
  CHECK(rig.get(ds::ClockMode) == 0.0f);
  rig.tap(Button::Right);
  CHECK(rig.get(ds::ClockMode) == 1.0f);
  rig.tap(Button::Left);
  CHECK(rig.get(ds::ClockMode) == 0.0f);
  rig.tap(Button::Left);
  CHECK(rig.get(ds::ClockMode) == 0.0f);  // the first choice: no wrap
  CHECK(rig.page() == 0);
  CHECK(rig.row() == 2);
  // A number: one step a press, ten with Shift (METRO VOL on PATTERN).
  rig.tap(Button::F6);
  CHECK(rig.page() == 5);
  for (int i = 0; i < 5; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.get(ds::MetronomeVolume) == 50.0f);
  rig.tap(Button::Right);
  CHECK(rig.get(ds::MetronomeVolume) == 51.0f);
  rig.shift_tap(Button::Right);
  CHECK(rig.get(ds::MetronomeVolume) == 61.0f);
  rig.shift_tap(Button::Left);
  CHECK(rig.get(ds::MetronomeVolume) == 51.0f);
  rig.tap(Button::Left);
  CHECK(rig.get(ds::MetronomeVolume) == 50.0f);
  CHECK(rig.page() == 5);
  CHECK(rig.row() == 5);
  // The pages: F-keys, and the screen button again for the next one.
  rig.tap(Button::F5);
  CHECK(rig.page() == 4);
  rig.tap(Button::Config);
  CHECK(rig.page() == 5);
  rig.tap(Button::Pattern);
  rig.tap(Button::Config);
  CHECK(rig.page() == 5);  // kept per screen
}

TEST_CASE("config: Left / Right on the USB page change the pending value like the encoder, and drop it only when the page changes") {
  Rig rig;
  rig.tap(Button::F5);  // USB
  CHECK(rig.params().pending_n == 0u);
  rig.tap(Button::Left);
  CHECK(rig.params().pending_n != 0u);
  CHECK(rig.page() == 4);
  rig.tap(Button::F1);
  CHECK(rig.params().pending_n == 0u);
}

// ---- OS-USBDISK U2b: the USB page's CARD AS DISK and READ ONLY rows (docs/specs/OS-USBDISK.md 9) -----------------------------------------------------------------------------------------------------

namespace {

using Phase = app::ViewState::DiskView::Phase;
using Request = app::ViewState::DiskView::Request;

void to_disk_row(Rig& rig) {
  rig.tap(Button::F5);
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // USB AUDIO, USB RETURN, RETURN LEVEL, then the two rows that are not settings
  rig.tap(Button::Down);
  NEED(app::config_row(rig.app->state()) == app::ConfigRow::CardAsDisk);
}

}  // namespace

TEST_CASE("config: the USB page ends with CARD AS DISK and READ ONLY; neither is a setting, READ ONLY is a session choice that starts On, the encoder and Delete move it") {
  Rig rig;
  to_disk_row(rig);
  CHECK(app::kConfigScreen.focus_param(rig.app->model(), rig.app->state()) == core::ParamId::None);  // no descriptor: not learnable, nothing to write
  CHECK(rig.app->disk_read_only());  // Q4: read-only first
  rig.turn(1);  // on the action row the encoder does nothing
  CHECK(rig.app->disk_read_only());
  rig.tap(Button::Down);
  NEED(app::config_row(rig.app->state()) == app::ConfigRow::ReadOnly);
  rig.turn(-1);
  CHECK_FALSE(rig.app->disk_read_only());  // Off: the computer may write
  rig.turn(1);
  CHECK(rig.app->disk_read_only());
  rig.turn(-1);
  rig.tap(Button::Delete);
  CHECK(rig.app->disk_read_only());  // Delete puts the default back
  rig.tap(Button::F7);  // F7 on READ ONLY applies nothing and writes nothing
  CHECK(rig.app->take_disk_request() == Request::None);
  CHECK(rig.params().pending_n == 0u);
  rig.tap(Button::Down);  // the last row: Down stays
  CHECK(app::config_row(rig.app->state()) == app::ConfigRow::ReadOnly);
  rig.tap(Button::F1);  // another page: the rows are gone
  CHECK(app::config_row(rig.app->state()) == app::ConfigRow::Param);
}

// OS-CFG-ARROWS: on the rows that are not settings the arrows do what the encoder does: READ ONLY toggles, CARD AS DISK and the SD and INFO pages do nothing, and the page stays.
TEST_CASE("config: Left / Right toggle READ ONLY like the encoder, do nothing on CARD AS DISK and on the SD and INFO pages, and never turn the page") {
  Rig rig;
  to_disk_row(rig);
  rig.tap(Button::Right);  // the action row: nothing
  rig.tap(Button::Left);
  CHECK(rig.app->disk_read_only());
  CHECK(rig.params().pending_n == 0u);
  rig.tap(Button::Down);
  NEED(app::config_row(rig.app->state()) == app::ConfigRow::ReadOnly);
  rig.tap(Button::Left);
  CHECK_FALSE(rig.app->disk_read_only());
  rig.tap(Button::Right);
  CHECK(rig.app->disk_read_only());
  rig.shift_tap(Button::Left);
  CHECK_FALSE(rig.app->disk_read_only());  // a toggle has no coarse step: the same
  CHECK(rig.page() == 4);
  for (const Button page : {Button::F3, Button::F4}) {  // SD and INFO: no rows
    rig.tap(page);
    const unsigned shown = rig.page();
    rig.tap(Button::Left);
    rig.tap(Button::Right);
    rig.shift_tap(Button::Right);
    CHECK(rig.page() == shown);
    CHECK(rig.params().pending_n == 0u);
  }
}

// OS-USBDISK U2b, 9: F7 reads SHARE or EJECT on the CARD AS DISK row (WAIT while the card changes hands), SHARE? / EJECT? once the first press has asked; every other row keeps APPLY.
TEST_CASE("config: F7's label on CARD AS DISK is SHARE or EJECT, SHARE? or EJECT? when armed, APPLY on the other rows") {
  Rig rig;
  using Phase = app::ViewState::DiskView::Phase;
  CHECK(std::string(app::config_f7_label(rig.app->state())) == "APPLY");
  to_disk_row(rig);
  CHECK(std::string(app::config_f7_label(rig.app->state())) == "SHARE");  // (the plain row: what the screen draws)
  rig.tap(Button::F7);
  CHECK(rig.last_news() == "THE COMPUTER GETS THE CARD: SAVES STOP. F7 AGAIN");
  const test::RowRead armed = test::read_row(*rig.app);
  CHECK(armed.label[6] == "SHARE?");
  CHECK(armed.hot == 0x40u);
  rig.app->set_disk_state(Phase::Working, false, false, false);
  CHECK(std::string(app::config_f7_label(rig.app->state())) == "WAIT");  // (the plain row: what the screen draws)
  rig.app->set_disk_state(Phase::Out, false, false, false);
  CHECK(std::string(app::config_f7_label(rig.app->state())) == "EJECT");  // (the plain row: what the screen draws)
  rig.tap(Button::F7);
  CHECK(test::read_row(*rig.app).label[6] == "EJECT?");
  rig.app->set_disk_state(Phase::Idle, false, false, false);
  rig.tap(Button::Down);  // READ ONLY: F7 applies nothing, the label is the old one
  CHECK(test::read_row(*rig.app).label[6] == "APPLY");
}

TEST_CASE("config: F7 on CARD AS DISK asks first and hands the card over on the second press; USB AUDIO says it pauses; a move of the cursor takes the first press back") {
  Rig rig;
  to_disk_row(rig);
  rig.tap(Button::F7);
  CHECK(rig.last_news() == "THE COMPUTER GETS THE CARD: SAVES STOP. F7 AGAIN");
  CHECK(rig.app->take_disk_request() == Request::None);  // nothing yet
  rig.tap(Button::Down);  // READ ONLY
  rig.tap(Button::Up);
  rig.tap(Button::F7);  // the first press again: the move took the earlier one back
  CHECK(rig.app->take_disk_request() == Request::None);
  rig.tap(Button::F7);
  CHECK(rig.app->take_disk_request() == Request::ShareReadOnly);  // READ ONLY is On at the start
  CHECK(rig.app->take_disk_request() == Request::None);  // once

  rig.app->set_disk_state(Phase::Idle, false, true, false);  // USB AUDIO is presented
  rig.tap(Button::F7);
  CHECK(rig.last_news() == "USB AUDIO PAUSES. F7 AGAIN TO SHARE THE CARD");
  rig.app->set_disk_state(Phase::Working, false, true, false);  // the hand-over runs: the keys wait, and the first press was for the state it was made in
  rig.tap(Button::F7);
  CHECK(rig.last_news() == "THE CARD IS CHANGING HANDS: WAIT");
  CHECK(rig.app->take_disk_request() == Request::None);
}

TEST_CASE("config: CARD AS DISK refuses on the first press: no card, a claim on the card; and while the card is out F7 twice ejects") {
  Rig rig;
  to_disk_row(rig);
  rig.card.set_present(false);
  rig.tap(Button::F7);
  CHECK(rig.last_news() == "NO CARD");
  rig.tap(Button::F7);
  CHECK(rig.app->take_disk_request() == Request::None);
  rig.card.set_present(true);
  rig.app->set_disk_state(Phase::Idle, true, false, false);  // an export, a load or an upload holds it
  rig.tap(Button::F7);
  CHECK(rig.last_news() == "STOP THE EXPORT FIRST");
  rig.tap(Button::F7);
  CHECK(rig.app->take_disk_request() == Request::None);

  rig.app->set_disk_state(Phase::Out, false, false, false);
  rig.app->set_card_on_computer(true);
  rig.tap(Button::F7);
  CHECK(rig.last_news() == "THE COMPUTER MAY STILL BE WRITING: F7 AGAIN");
  CHECK(rig.app->take_disk_request() == Request::None);
  rig.tap(Button::F7);
  CHECK(rig.app->take_disk_request() == Request::Eject);
  for (const ui::ThemeId theme : {ui::ThemeId::Amiga, ui::ThemeId::Black, ui::ThemeId::White}) {  // the rows draw in each phase
    rig.app->set_theme(theme);
    for (const Phase p : {Phase::Idle, Phase::Working, Phase::Out}) {
      rig.app->set_disk_state(p, false, p == Phase::Idle, p == Phase::Out);
      rig.tap(Button::Up);
      rig.tap(Button::Down);
    }
  }
  CHECK(rig.app->failed_frames() == 0u);
}

TEST_CASE("config: READ ONLY is refused while the card is out (EJECT THE CARD TO CHANGE IT), changing hands or a share request waits (THE CARD IS CHANGING HANDS: WAIT); the row shows the disk the computer has while out") {
  Rig rig;
  to_disk_row(rig);
  rig.tap(Button::Down);
  NEED(app::config_row(rig.app->state()) == app::ConfigRow::ReadOnly);
  rig.turn(-1);
  CHECK_FALSE(rig.app->disk_read_only());  // Off, chosen while Idle: the next share is writable
  CHECK(static_cast<std::string>("EJECT THE CARD TO CHANGE IT").size() <= core::kNotifyTextChars);

  rig.app->set_disk_state(Phase::Working, false, false, true);  // the hand-over runs: not "eject it" (it is not out yet), the row's own press says wait
  rig.turn(1);
  CHECK(rig.last_news() == "THE CARD IS CHANGING HANDS: WAIT");
  CHECK_FALSE(rig.app->disk_read_only());  // ignored
  rig.tap(Button::Delete);
  CHECK_FALSE(rig.app->disk_read_only());

  rig.app->set_disk_state(Phase::Out, false, false, true);  // the computer has the writable disk
  rig.app->set_card_on_computer(true);
  CHECK_FALSE(app::config_read_only_shown(rig.app->state()));  // Off: it can write
  rig.turn(1);  // the encoder is ignored: the row would otherwise say On while the computer still writes
  CHECK(rig.last_news() == "EJECT THE CARD TO CHANGE IT");
  CHECK_FALSE(rig.app->disk_read_only());
  CHECK_FALSE(app::config_read_only_shown(rig.app->state()));
  rig.tap(Button::Delete);
  CHECK_FALSE(rig.app->disk_read_only());
  CHECK_FALSE(app::config_read_only_shown(rig.app->state()));

  rig.tap(Button::Up);  // the eject is asked and not yet taken by the platform: the row says wait (the hand-over is under way), as for a share request
  rig.tap(Button::F7);
  rig.tap(Button::F7);
  rig.tap(Button::Down);
  rig.turn(1);
  CHECK(rig.last_news() == "THE CARD IS CHANGING HANDS: WAIT");
  CHECK_FALSE(rig.app->disk_read_only());
  CHECK(rig.app->take_disk_request() == Request::Eject);

  rig.app->set_disk_state(Phase::Out, false, false, false);  // a card that protects itself: the computer has it read only whatever the choice was
  CHECK(app::config_read_only_shown(rig.app->state()));  // the row says what the computer has
  CHECK_FALSE(rig.app->disk_read_only());                // (the choice is untouched)

  rig.app->set_disk_state(Phase::Idle, false, false, false);  // ejected: the choice is the row again, and it can be changed
  rig.app->set_card_on_computer(false);
  CHECK_FALSE(app::config_read_only_shown(rig.app->state()));
  rig.turn(1);
  CHECK(rig.app->disk_read_only());
  CHECK(app::config_read_only_shown(rig.app->state()));
}

TEST_CASE("config: the share request carries READ ONLY as it was at the second F7, and a waiting request keeps the row from changing it") {
  Rig rig;
  to_disk_row(rig);
  rig.tap(Button::F7);
  rig.tap(Button::F7);
  CHECK(rig.app->take_disk_request() == Request::ShareReadOnly);  // On (the start value)
  rig.tap(Button::Down);
  rig.turn(-1);  // Off
  rig.tap(Button::Up);
  rig.tap(Button::F7);
  rig.tap(Button::F7);
  rig.tap(Button::Down);  // the request has not been taken yet (the platform's pass comes later)
  rig.turn(1);            // READ ONLY back to On in between: refused, the request and the row agree (the hand-over is about to start: wait, not eject)
  CHECK(rig.last_news() == "THE CARD IS CHANGING HANDS: WAIT");
  CHECK_FALSE(rig.app->disk_read_only());
  CHECK(rig.app->take_disk_request() == Request::ShareWritable);  // the policy of the press
  rig.turn(1);  // taken: the platform now tells Working, but even before that the row may change again
  CHECK(rig.app->disk_read_only());
}
