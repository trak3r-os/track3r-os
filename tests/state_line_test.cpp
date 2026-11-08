// SPDX-License-Identifier: GPL-3.0-or-later
//
// `diag state` (OS-HIL): the one machine-readable line the PC test host reads (app/state_line.h) and the button names the injection commands take. The line is
// read from a real App driven by the fake input, so the test fails when the app's state moves and the line does not follow.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/state_line.h>
#include <o3os/audio/play_position.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

using namespace o3os;
using app::Button;

namespace {

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    app->run_once();
  }
  std::string line() const {
    char buf[320];
    const std::size_t n = app::describe_state(*app, buf, sizeof buf);
    CHECK(n == std::strlen(buf));
    return buf;
  }
};

// key=value pairs of a line, in order.
std::string value_of(const std::string& line, const std::string& key) {
  std::size_t at = 0;
  while (at < line.size()) {
    const std::size_t end = line.find(' ', at);
    const std::string word = line.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (word.compare(0, key.size() + 1, key + "=") == 0) {
      return word.substr(key.size() + 1);
    }
    if (end == std::string::npos) {
      break;
    }
    at = end + 1;
  }
  return "<missing>";
}

}  // namespace

TEST_CASE("state line: the first frame says Pattern, the cursor, the defaults and no news") {
  Rig r;
  r.app->run_once();
  const std::string l = r.line();
  INFO(l);
  CHECK(l.compare(0, 15, "screen=PATTERN ") == 0);
  CHECK(value_of(l, "overlay") == "NONE");
  CHECK(value_of(l, "pattern") == "1");
  CHECK(value_of(l, "track") == "0");
  CHECK(value_of(l, "row") == "0");
  CHECK(value_of(l, "col") == "NOTE");
  CHECK(value_of(l, "playing") == "0");
  CHECK(value_of(l, "seq") == "0");
  CHECK(value_of(l, "instrument") == "1");
  CHECK(value_of(l, "pads") == "NOTES");
  CHECK(value_of(l, "shift") == "0");
  CHECK(value_of(l, "order") == "-");
  CHECK(value_of(l, "posts") == "0");
  CHECK(value_of(l, "msg") == "-");
  CHECK(value_of(l, "sel") == "-");
  // The keys come in one fixed order, each once, and nothing in the line but pairs separated by single spaces.
  const char* const keys[] = {"screen", "page", "mode", "overlay", "pattern", "track", "row", "col", "playing", "seq", "tempo", "instrument",
                              "pads", "shift", "order", "play_pattern", "play_step", "posts", "sel", "msg"};
  std::size_t from = 0;
  for (const char* k : keys) {
    const std::size_t at = l.find(std::string(k) + "=", from);
    CHECK_MESSAGE(at != std::string::npos, k);
    from = at == std::string::npos ? from : at + 1;
  }
  CHECK(l.find("  ") == std::string::npos);
  CHECK(l.find('\n') == std::string::npos);
}

TEST_CASE("state line: it follows the cursor, the screen, the overlay and the notification") {
  Rig r;
  r.app->run_once();
  r.tap(Button::Down);
  r.tap(Button::Down);
  r.tap(Button::Right);
  r.tap(Button::Right);
  std::string l = r.line();
  CHECK(value_of(l, "row") == "2");
  CHECK(value_of(l, "col") == "VOL");
  r.tap(Button::Song);
  l = r.line();
  CHECK(value_of(l, "screen") == "SONG");
  CHECK(value_of(l, "row") == "2");  // the Pattern cursor stays where it was left
  r.tap(Button::Master);
  CHECK(value_of(r.line(), "screen") == "MIXER");
  r.tap(Button::SampleLoad);
  CHECK(value_of(r.line(), "screen") == "SAMPLE_LOAD");  // a value has no space
  r.tap(Button::Pattern);
  CHECK(value_of(r.line(), "screen") == "PATTERN");
  // a notification: the strip is the overlay while it is up, and the text and the post counter stay after it has gone
  const std::uint32_t posts = r.app->state().notify.posts();
  r.tap(Button::F8);  // (a key that posts: undo with nothing to undo says so)
  l = r.line();
  if (r.app->state().notify.posts() != posts) {  // whichever way this build words it, the line says what the queue holds
    CHECK(value_of(l, "posts") == std::to_string(r.app->state().notify.posts()));
    std::string msg = r.app->state().notify.last_text();
    for (char& c : msg) {
      c = c == ' ' ? '_' : c;
    }
    CHECK(value_of(l, "msg") == msg);
    CHECK(value_of(l, "overlay") == "NOTIFY");
    for (int i = 0; i < 40; ++i) {
      r.clock.advance(100000);
      r.app->run_once();
    }
    l = r.line();
    CHECK(value_of(l, "overlay") == "NONE");
    CHECK(value_of(l, "msg") == msg);  // gone from the screen, still the last news
  }
}

// OS-KEYROWDLG S4: ` keys=` says which keys the prompt in force marks, only while one is live, as the last pair. A pixel compare of the row proves nothing (91% of a row's pixels differed on the unit with the same
// words), so a scenario reads the keys here (platform/mk66/tools/hil/qa_keyrowdlg.json).
TEST_CASE("state line: keys= is present only while a prompt is live, and names the keys the row says") {
  Rig r;
  r.app->run_once();
  CHECK(value_of(r.line(), "keys") == "<missing>");  // nothing asked, no dialog
  // A dialog: Learn with no target and no bindings has BACK alone.
  (void)r.app->invoke(core::action_id("learn.toggle"));
  r.app->run_once();
  std::string l = r.line();
  CHECK(value_of(l, "overlay") == "LEARN");
  CHECK(value_of(l, "keys") == "F8:BACK");
  CHECK(l.compare(l.rfind(' ') + 1, 5, "keys=") == 0);  // the last pair
  (void)r.app->invoke(core::action_id("learn.toggle"));
  r.app->run_once();
  CHECK(value_of(r.line(), "keys") == "<missing>");
  // The Navigator: its keys in the table's order, a Shift key as SF7, the inert ones left out.
  (void)r.app->invoke(core::action_id("navigator.open"));  // (the chord Song + Pattern, OS-APPSNAV: the action it is bound to)
  r.app->run_once();
  l = r.line();
  NEED(value_of(l, "overlay") == "NAVIGATOR");
  CHECK(value_of(l, "keys") == "F1:GO,F7:CLOSE,F8:GO,SF7:CLOSE,SF8:GO");
  r.tap(Button::F7);  // CLOSE
  CHECK(value_of(r.line(), "keys") == "<missing>");
  // A question: the Config page's armed APPLY marks F7.
  (void)r.app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(app::ScreenId::Config));
  r.tap(Button::F5);  // the USB page
  r.input.push(core::make_encoder_event(r.clock.now(), 0, -1));
  r.app->run_once();
  r.tap(Button::F7);
  l = r.line();
  NEED(value_of(l, "screen") == "CONFIG");
  NEED(r.app->state().params.armed);
  CHECK(value_of(l, "keys") == "F7:APPLY?");
  r.tap(Button::Down);  // a move takes the first press back
  CHECK(value_of(r.line(), "keys") == "<missing>");
}

TEST_CASE("state line: the keys of any prompt fit the room keys= has, so the console's 384-character line is never cut by it") {
  // The longest line without keys= is about 310 characters (msg up to 48, sel up to 40, and sel only on File and Sample load). A dialog or a question over such a screen has at most kScreenKeys characters of keys; the boot
  // disk's eight RESTART (the one long prompt) is on Pattern, where sel is "-": its line is 39 characters shorter.
  constexpr std::size_t kScreenKeys = 68;  // (Learn with both its CLEAR ALL options and F5 FOLLOW live at once, which no state has: 68; OS-LEARNFOLLOW S1 binds the key)
  static_assert(310u + 6u + kScreenKeys <= 384u, "keys= over the longest line must fit the console's state buffer");
  static_assert(310u - 39u + 6u + app::kKeysLineChars <= 384u, "the boot disk's keys= must fit the console's state buffer next to its line");
  std::size_t longest = 0;
  char buf[256];
  for (unsigned id = 0; id < 64; ++id) {
    const app::KeyPrompt* prompt = app::owner_prompt(static_cast<app::OverlayId>(id));
    if (prompt != nullptr) {
      app::PromptState all;
      all.prompt = prompt;
      all.live = static_cast<std::uint16_t>((1u << prompt->count) - 1u);  // every option live at once: more than any state has
      longest = std::max(longest, app::describe_prompt_keys(all, buf, sizeof buf));
    }
  }
  for (unsigned q = static_cast<unsigned>(app::Ask::New); q <= static_cast<unsigned>(app::Ask::Launch); ++q) {
    const app::KeyPrompt& prompt = app::ask_prompt(static_cast<app::Ask>(q));
    app::PromptState all;
    all.prompt = &prompt;
    all.live = static_cast<std::uint16_t>((1u << prompt.count) - 1u);
    longest = std::max(longest, app::describe_prompt_keys(all, buf, sizeof buf));
  }
  CHECK(longest > 0u);
  CHECK(longest <= kScreenKeys);
  // The boot disk: eight keys with one word, the longest of all.
  app::Model model;
  app::ViewState view;
  view.disk_keys = app::DiskKeys::Restart;
  const std::size_t disk = app::describe_keys(model, view, buf, sizeof buf);
  CHECK(std::string(buf) == "F1:RESTART,F2:RESTART,F3:RESTART,F4:RESTART,F5:RESTART,F6:RESTART,F7:RESTART,F8:RESTART");
  CHECK(disk <= app::kKeysLineChars);
  view.disk_keys = app::DiskKeys::Confirm;
  CHECK(std::string(buf, app::describe_keys(model, view, buf, sizeof buf)).compare(0, 9, "F1:AGAIN?") == 0);
  view.disk_keys = app::DiskKeys::Finishing;
  CHECK(app::describe_keys(model, view, buf, sizeof buf) == 0u);  // no key acts: no keys= in the line
  // A small buffer cuts the text, never writes past it.
  app::PromptState learn;
  learn.prompt = app::owner_prompt(app::OverlayId::Learn);
  learn.live = 0x3Fu;
  char small[10];
  std::memset(small, 'x', sizeof small);
  const std::size_t n = app::describe_prompt_keys(learn, small, sizeof small);
  CHECK(n == sizeof small - 1u);
  CHECK(small[n] == '\0');
  CHECK(app::describe_prompt_keys(learn, nullptr, 0) == 0u);
}

TEST_CASE("state line: a notification text has no space in it and is cut at the buffer, never past it") {
  Rig r;
  r.app->run_once();
  char small[40];
  std::memset(small, 'x', sizeof small);
  const std::size_t n = app::describe_state(*r.app, small, sizeof small);
  CHECK(n == sizeof small - 1u);
  CHECK(small[n] == '\0');
  char one[1] = {'x'};
  CHECK(app::describe_state(*r.app, one, 1) == 0u);
  CHECK(one[0] == '\0');
  CHECK(app::describe_state(*r.app, nullptr, 0) == 0u);
}

TEST_CASE("button names: the simulator's names, any case, and the decimal codes") {
  CHECK(app::button_code("f1") == 0);
  CHECK(app::button_code("F8") == 7);
  CHECK(app::button_code("Play") == 23);
  CHECK(app::button_code("SHIFT") == 29);
  CHECK(app::button_code("right") == 32);
  CHECK(app::button_code("sampleload") == 13);
  CHECK(app::button_code("0") == 0);
  CHECK(app::button_code("32") == 32);
  CHECK(app::button_code("33") == -1);
  CHECK(app::button_code("100") == -1);
  CHECK(app::button_code("1x") == -1);
  CHECK(app::button_code("") == -1);
  CHECK(app::button_code("pla") == -1);
  CHECK(app::button_code("playy") == -1);
  CHECK(app::button_code(nullptr) == -1);
  for (const app::ButtonName& b : app::kButtonNames) {
    CHECK(app::button_code(b.name) == static_cast<int>(b.button));
  }
}

// FQA-10a: the tempo field is the strip's: the sequencer's tempo while it plays, the project's otherwise. A Pattern tempo hold or tap while stopped sets the project's tempo; the position word keeps the
// last tempo that was played, so the field lagged behind the strip and the screen.
TEST_CASE("state line: tempo is the project's while stopped (not the last played one) and the sequencer's while it plays") {
  alignas(8) static std::uint8_t ring[64 * 1024];
  Rig r;
  auto arena = std::make_unique<model::ModelArena>();
  arena->project.reset();
  model::UndoJournal journal;
  journal.begin(ring, sizeof ring);
  model::Editor editor(arena->project, journal);
  r.app->attach_document(&editor);
  audio::PlayPosition position;
  r.app->attach_play_position(&position);
  position.publish(false, 1, 0, 9000);  // the last tempo the sequencer played
  NEED(arena->project.set_tempo_x100(13000) == model::Status::Ok);  // what the tempo hold or tap left behind
  r.app->run_once();
  CHECK(value_of(r.line(), "tempo") == "13000");
  position.publish(true, 1, 0, 9000);
  r.app->run_once();
  CHECK(value_of(r.line(), "tempo") == "9000");
  position.publish(false, 1, 0, 9000);
  r.app->run_once();
  CHECK(value_of(r.line(), "tempo") == "13000");
}
