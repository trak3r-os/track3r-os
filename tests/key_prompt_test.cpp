// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-KEYROWDLG (docs/specs/OS-KEYROWDLG.md): the F-key row says what the keys do while a question or a dialog is up. One table (app/key_prompt.h) makes the key row and the strip's question text; the bindings stay the
// truth about what a key does, and these tests hold the three together:
//   - the strip text of every question is byte for byte what it was before the table (the cases that drive the questions through the App check the same strings);
//   - a question marks its answer keys, inverted, and only while its text is the head of the strip: a question queued behind another message, or one that was taken back, marks nothing;
//   - the key a question marks is bound to the action that answers it;
//   - a dialog (every modal overlay) owns the row: its keys, every other key blank, each live key bound in the overlay's own context (which captures), and no bound key left off the row.
#include <doctest/doctest.h>

#include "support/key_row.h"
#include "support/need.h"

#include <o3os/app/app.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/config_screen.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/app/screens/input_test_screen.h>
#include <o3os/app/screens/modules_screen.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/record_screen.h>
#include <o3os/app/screens/song_screen.h>
#include <o3os/app/view_state.h>
#include <o3os/core/param.h>

#include <o3os/platform/host/fakes.h>

#include <cstring>
#include <memory>
#include <string>

using namespace o3os;
using app::Ask;
using app::AskText;
using app::ScreenId;

namespace {

std::string text_of(Ask which, const char* head) {
  AskText out;
  app::ask_text(which, head, out);
  return out.c_str();
}

// A File screen with a question's text on the strip, as the action leaves it.
struct FileRig {
  app::Model model;
  app::ViewState view;
  FileRig() { view.screen = ScreenId::File; }
  void say(const std::string& text) { CHECK(view.notify.post(core::Severity::Warn, text.c_str(), "file", app::ViewState::kMessageMs)); }
  test::RowRead row(bool shifted = false) const { return test::read_row(model, view, app::kFileScreen, shifted); }
};

}  // namespace

TEST_CASE("key prompt: the strip text of every question is what it was before the table") {
  CHECK(text_of(Ask::New, app::kAskHeadNew) == "NEW PROJECT: F3 ALL, F1 KEEP INSTRUMENTS");
  CHECK(text_of(Ask::New, app::kAskHeadUnsaved) == "UNSAVED CHANGES: F3 ALL, F1 KEEP INSTRUMENTS");
  CHECK(text_of(Ask::Open, app::kAskHeadUnsaved) == "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM");
  CHECK(text_of(Ask::Import, app::kAskHeadUnsaved) == "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM");
  CHECK(text_of(Ask::Purge, "PURGE 3 UNUSED INSTRUMENTS?") == "PURGE 3 UNUSED INSTRUMENTS? SHIFT+F7 AGAIN");
  CHECK(text_of(Ask::Og, "LOSES 2 MODULES") == "LOSES 2 MODULES: F1 AGAIN");
  CHECK(text_of(Ask::Defaults, "") == "SHIFT+F7 AGAIN: RESET PAGE");
  CHECK(text_of(Ask::Launch, "EDITS THE PROJECT (UNDOABLE)") == "EDITS THE PROJECT (UNDOABLE): F1 AGAIN");
  CHECK(text_of(Ask::Apply, "PRESS") == "PRESS F7 AGAIN");
  CHECK(text_of(Ask::Shared, "PATTERN 03 ALSO IN 2 SLOTS") == "PATTERN 03 ALSO IN 2 SLOTS: F1 ALL, F2 UNIQUE");  // OS-SONGMAP
  CHECK(text_of(Ask::TakeDrop, app::kAskHeadTake) == "TAKE NOT SAVED: F4 AGAIN");
  CHECK(text_of(Ask::TakeRec, app::kAskHeadTake) == "TAKE NOT SAVED: F1 AGAIN");
  CHECK(text_of(Ask::TakeCrop, app::kAskHeadCrop) == "CROP KEEPS THE WINDOW ONLY: SHIFT+F3 AGAIN");
  AskText tail;
  app::ask_tail(Ask::Og, tail);
  CHECK(std::string(tail.c_str()) == ": F1 AGAIN");
  app::ask_tail(Ask::Purge, tail);
  CHECK(std::string(tail.c_str()) == " SHIFT+F7 AGAIN");
}

TEST_CASE("key prompt: every label of a question fits a key cap, and the key it names is bound to something") {
  for (unsigned q = static_cast<unsigned>(Ask::New); q <= static_cast<unsigned>(Ask::Shared); ++q) {
    const app::KeyPrompt& prompt = app::ask_prompt(static_cast<Ask>(q));
    CHECK(prompt.kind == app::PromptKind::Question);
    CHECK(prompt.count >= 1);
    for (unsigned i = 0; i < prompt.count; ++i) {
      const app::KeyOption& o = prompt.options[i];
      const std::string where = "question " + std::to_string(q) + " option " + std::to_string(i) + " '" + o.label + "'";
      CHECK_MESSAGE(o.key < 16, where);
      CHECK_MESSAGE(o.label[0] != '\0', where << " has no label");
      CHECK_MESSAGE(std::strlen(o.label) <= 8, where << " does not fit a key cap");
    }
  }
}

TEST_CASE("key prompt: the keys a question marks answer it (the labelled key does what its label says)") {
  app::Model model;
  struct Row {
    Ask ask;
    ScreenId screen;
    const app::Screen* screen_object;
    bool export_page;
    std::uint8_t key;
    const char* action;
  };
  const Row rows[] = {
      {Ask::New, ScreenId::File, &app::kFileScreen, false, 2, "file.new"},           // F3 ALL
      {Ask::New, ScreenId::File, &app::kFileScreen, false, 0, "file.open"},          // F1 KEEP INS
      {Ask::Open, ScreenId::File, &app::kFileScreen, false, 0, "file.open"},         // F1 OPEN?
      {Ask::Import, ScreenId::File, &app::kFileScreen, false, 0, "file.open"},       // F1 IMPORT? (the IMPORT page's F1 is the same action)
      {Ask::Purge, ScreenId::File, &app::kFileScreen, false, 14, "file.purge"},      // Shift + F7 PURGE?
      {Ask::Og, ScreenId::File, &app::kFileScreen, true, 0, "file.export"},          // F1 EXPORT? on the EXPORT page
      {Ask::Apply, ScreenId::Config, &app::kConfigScreen, false, 6, "param.apply"},  // F7 APPLY?
      {Ask::Share, ScreenId::Config, &app::kConfigScreen, false, 6, "param.apply"},  // F7 SHARE? (the CARD AS DISK row's)
      {Ask::Eject, ScreenId::Config, &app::kConfigScreen, false, 6, "param.apply"},  // F7 EJECT?
      {Ask::Defaults, ScreenId::Config, &app::kConfigScreen, false, 14, "config.page_default"},
      {Ask::Launch, ScreenId::Modules, &app::kModulesScreen, false, 0, "module.launch"},
      {Ask::TakeDrop, ScreenId::Record, &app::kRecordScreen, false, 3, "record.discard"},  // F4 DISCARD? (OS-QAFIX-E-SREC-09)
      {Ask::TakeRec, ScreenId::Record, &app::kRecordScreen, false, 0, "record.toggle"},    // F1 REC?
      {Ask::TakeCrop, ScreenId::Record, &app::kRecordScreen, false, 10, "record.crop"},    // Shift + F3 CROP? (OS-RECCROP)
  };
  for (const Row& row : rows) {
    app::ViewState view;
    view.screen = row.screen;
    view.xport.page = row.export_page;
    const app::KeyPrompt& prompt = app::ask_prompt(row.ask);
    bool named = false;
    for (unsigned i = 0; i < prompt.count; ++i) {
      named = named || prompt.options[i].key == row.key;
    }
    CHECK_MESSAGE(named, "question " << static_cast<unsigned>(row.ask) << " does not mark key " << static_cast<unsigned>(row.key));
    const test::KeyBinding b = test::resolve_key(model, view, *row.screen_object, row.key);
    CHECK_MESSAGE(b.bound, "key " << static_cast<unsigned>(row.key) << " of question " << static_cast<unsigned>(row.ask) << " is not bound");
    CHECK_MESSAGE(b.action == core::action_id(row.action), "key " << static_cast<unsigned>(row.key) << " of question " << static_cast<unsigned>(row.ask) << " is not " << row.action);
  }
}

TEST_CASE("key prompt: NEW marks F3 ALL and F1 KEEP INS, inverted, while its text is the head of the strip") {
  FileRig rig;
  CHECK_FALSE(rig.row().marked);  // nothing asked
  rig.view.file.new_choice = true;
  rig.say(text_of(Ask::New, app::kAskHeadNew));
  const test::RowRead row = rig.row();
  CHECK(row.marked);
  CHECK(row.label[0] == "KEEP INS");
  CHECK(row.label[2] == "ALL");
  CHECK(row.hot == 0x05u);  // F1 and F3, no other
  CHECK(row.label[1] == "RESCAN");  // the other keys keep their labels: they still work, and take the question back
  CHECK(row.label[3] == "SAVE");
  CHECK(row.label[4] == "SAVE AS");
  CHECK(row.label[5] == "IMPORT");
  CHECK(row.label[6] == "EXPORT");
  // The Shift layer is not the question's: under Shift the row is the screen's Shift row.
  CHECK_FALSE(rig.row(true).marked);
  // The unsaved-changes text is the same question.
  rig.view.notify.clear();
  rig.say(text_of(Ask::New, app::kAskHeadUnsaved));
  CHECK(rig.row().marked);
  // Taken back (the flag): the plain row is back in the same pass.
  rig.view.file.new_choice = false;
  CHECK_FALSE(rig.row().marked);
  CHECK(rig.row().label[0] == "OPEN");
  CHECK(rig.row().label[2] == "NEW");
}

TEST_CASE("key prompt: a question that is not the head of the strip marks nothing (queued behind another message, or gone)") {
  FileRig rig;
  rig.view.file.new_choice = true;
  rig.say("PROJECT NAMED");  // another message in front of the question
  rig.say(text_of(Ask::New, app::kAskHeadNew));
  CHECK_FALSE(rig.row().marked);
  CHECK(app::file_question(rig.view) == Ask::None);
  rig.view.notify.clear();
  CHECK_FALSE(rig.row().marked);  // the strip is empty: the flag alone marks nothing
  rig.say(text_of(Ask::New, app::kAskHeadNew));
  CHECK(rig.row().marked);
}

TEST_CASE("key prompt: OPEN? and IMPORT? mark F1 on their page; PURGE? marks the Shift layer; EXPORT? marks F1 of the OG row") {
  FileRig rig;
  rig.view.file.confirm_new = true;
  rig.say(text_of(Ask::Open, app::kAskHeadUnsaved));
  CHECK(rig.row().label[0] == "OPEN?");
  CHECK(rig.row().hot == 0x01u);
  CHECK(rig.row().label[1] == "RESCAN");
  rig.view.import.page = true;
  CHECK(rig.row().label[0] == "IMPORT?");
  rig.view.import.page = false;
  rig.view.file.confirm_new = false;
  rig.view.notify.clear();

  // PURGE: the answer is Shift + F7, so the plain row marks nothing, and the Shift row marks F7 (the other Shift labels stay).
  rig.view.file.confirm_purge = true;
  rig.say("PURGE 2 UNUSED INSTRUMENTS? SHIFT+F7 AGAIN");
  CHECK_FALSE(rig.row().marked);
  const test::RowRead shifted = rig.row(true);
  CHECK(shifted.marked);
  CHECK(shifted.label[6] == "PURGE?");
  CHECK(shifted.hot == 0x40u);
  CHECK(shifted.label[0] == "0/4");
  rig.view.file.confirm_purge = false;
  CHECK_FALSE(rig.row(true).marked);
  rig.view.notify.clear();

  // The OG PROJECT row's question: its own text is the one on the strip.
  rig.view.xport.page = true;
  rig.view.file.og_ask = true;
  rig.view.file.og_ask_text.assign("LOSES 2 MODULES: F1 AGAIN");
  rig.say("LOSES 2 MODULES: F1 AGAIN");
  CHECK(rig.row().label[0] == "EXPORT?");
  CHECK(rig.row().hot == 0x01u);
  rig.view.file.og_ask = false;
  CHECK_FALSE(rig.row().marked);
}

TEST_CASE("key prompt: Config marks F7 APPLY? while armed, and Shift + F7 RESET? while its text is on the strip") {
  app::Model model;
  app::ViewState view;
  view.screen = ScreenId::Config;
  CHECK_FALSE(test::read_row(model, view, app::kConfigScreen).marked);
  view.params.armed = true;  // a press of F7 writes the pending values now (the arm has no clock), but the row follows the strip (OS-BUG-CFGARM)
  CHECK_FALSE(test::read_row(model, view, app::kConfigScreen).marked);  // armed, but the text is not on the strip (its two seconds are over): the plain row
  CHECK(view.notify.post(core::Severity::Info, app::kApplyAskStrip, "params", app::ViewState::kMessageMs));
  const test::RowRead apply = test::read_row(model, view, app::kConfigScreen);
  CHECK(apply.marked);
  CHECK(apply.label[6] == "APPLY?");
  CHECK(apply.hot == 0x40u);
  CHECK(apply.label[4] == "USB");  // the page keys keep their labels
  CHECK_FALSE(test::read_row(model, view, app::kConfigScreen, true).marked);
  view.notify.clear();
  CHECK_FALSE(test::read_row(model, view, app::kConfigScreen).marked);
  CHECK(view.notify.post(core::Severity::Info, app::kApplyAskStripReconnect, "params", app::ViewState::kMessageMs));  // the USB AUDIO case: the apply re-enumerates
  CHECK(test::read_row(model, view, app::kConfigScreen).marked);
  view.notify.clear();
  view.params.armed = false;
  CHECK_FALSE(test::read_row(model, view, app::kConfigScreen).marked);

  view.params.defaults_armed = true;
  CHECK_FALSE(test::read_row(model, view, app::kConfigScreen, true).marked);  // armed, but the text is not on the strip (its two seconds are over)
  CHECK(view.notify.post(core::Severity::Info, "SHIFT+F7 AGAIN: RESET PAGE", "defaults", app::ViewState::kMessageMs));
  const test::RowRead reset = test::read_row(model, view, app::kConfigScreen, true);
  CHECK(reset.marked);
  CHECK(reset.label[6] == "RESET?");
  CHECK(reset.hot == 0x40u);
  CHECK_FALSE(test::read_row(model, view, app::kConfigScreen).marked);  // the plain row is APPLY's
  view.params.defaults_armed = false;
  CHECK_FALSE(test::read_row(model, view, app::kConfigScreen, true).marked);
}

TEST_CASE("key prompt: Record marks F4 DISCARD?, F1 REC? and Shift + F3 CROP? while their text is the head of the strip") {
  app::Model model;
  app::ViewState view;
  view.screen = ScreenId::Record;
  CHECK_FALSE(test::read_row(model, view, app::kRecordScreen).marked);
  view.take_ask.kind = app::TakeAsk::Kind::Discard;
  CHECK_FALSE(test::read_row(model, view, app::kRecordScreen).marked);  // armed, but the text is not on the strip
  CHECK(view.notify.post(core::Severity::Info, "TAKE NOT SAVED: F4 AGAIN", "record", app::ViewState::kMessageMs));
  const test::RowRead drop = test::read_row(model, view, app::kRecordScreen);
  CHECK(drop.marked);
  CHECK(drop.label[3] == "DISCARD?");
  CHECK(drop.hot == 0x08u);
  CHECK(drop.label[2] == "SAVE");  // the other keys keep their labels
  view.notify.clear();
  view.take_ask.kind = app::TakeAsk::Kind::Record;
  CHECK(view.notify.post(core::Severity::Info, "TAKE NOT SAVED: F1 AGAIN", "record", app::ViewState::kMessageMs));
  const test::RowRead rec = test::read_row(model, view, app::kRecordScreen);
  CHECK(rec.label[0] == "REC?");
  CHECK(rec.hot == 0x01u);
  view.notify.clear();
  view.take_ask.kind = app::TakeAsk::Kind::Crop;
  CHECK(view.notify.post(core::Severity::Info, "CROP KEEPS THE WINDOW ONLY: SHIFT+F3 AGAIN", "record", app::ViewState::kMessageMs));
  const test::RowRead crop = test::read_row(model, view, app::kRecordScreen, true);
  CHECK(crop.marked);
  CHECK(crop.label[2] == "CROP?");
  CHECK(crop.hot == 0x04u);
}

TEST_CASE("key prompt: no question on a screen that cannot ask one") {
  app::Model model;
  app::ViewState view;
  view.file.new_choice = true;
  view.params.armed = true;
  CHECK(view.notify.post(core::Severity::Warn, "NEW PROJECT: F3 ALL, F1 KEEP INSTRUMENTS", "file", app::ViewState::kMessageMs));
  view.screen = ScreenId::Pattern;
  CHECK(app::prompt_in_force(model, view).prompt == nullptr);
  view.screen = ScreenId::Song;
  CHECK(app::prompt_in_force(model, view).prompt == nullptr);
}

// ---- The dialogs -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

// What `key` (0..7 = F1..F8, 8..15 = Shift + F1..F8) resolves to in the context chain a modal overlay has: its own context first, which captures, then Global.
test::KeyBinding resolve_in_overlay(app::OverlayId id, std::uint8_t key) {
  const app::Overlay* overlay = app::overlay_of(id);
  const core::BindingSet set{{core::BindingView{}, core::BindingView{}, app::builtin_bindings()}};
  core::ContextChain chain;
  (void)chain.push(app::context_id(overlay->context()), overlay->modal());
  (void)chain.push(app::context_id(app::Context::Global));
  const core::InputRef shift = key >= 8u ? app::button_ref(app::Button::Shift) : core::kNoInput;
  const core::Gesture tap{0, core::GestureKind::Tap, app::button_ref(test::kFKeys[key & 7u]), shift, 0};
  const core::Resolution r = core::resolve(set, chain, tap);
  test::KeyBinding out;
  out.bound = r.status == core::Resolution::Status::Bound && r.action != core::action_id("ui.none");
  out.action = r.action;
  return out;
}

constexpr app::OverlayId kModal[] = {app::OverlayId::Learn,       app::OverlayId::Navigator, app::OverlayId::PatternChooser, app::OverlayId::LengthChooser, app::OverlayId::JumpChooser, app::OverlayId::ModulePicker,
                                     app::OverlayId::TextEntry,   app::OverlayId::StepPopup, app::OverlayId::Fill,           app::OverlayId::PlayMode,      app::OverlayId::Tools,       app::OverlayId::ExportProgress, app::OverlayId::SongTools, app::OverlayId::FirmwareUpdate, app::OverlayId::Help};

}  // namespace

TEST_CASE("key prompt: every modal overlay has a dialog prompt, and nothing else has one (a new overlay without a prompt fails here)") {
  for (unsigned v = 0; v < 64; ++v) {
    const app::OverlayId id = static_cast<app::OverlayId>(v);
    const app::Overlay* overlay = app::overlay_of(id);
    const app::KeyPrompt* prompt = app::owner_prompt(id);
    const bool modal = overlay != nullptr && overlay->modal();
    CHECK_MESSAGE(modal == (prompt != nullptr), "overlay " << v << (modal ? " is modal and has no prompt" : " has a prompt and is not a modal overlay"));
    if (prompt != nullptr) {
      CHECK(prompt->kind == app::PromptKind::Owner);
    }
  }
  for (const app::OverlayId id : kModal) {
    CHECK_MESSAGE(app::owner_prompt(id) != nullptr, "overlay " << static_cast<unsigned>(id));
  }
}

TEST_CASE("key prompt: every key a dialog labels is bound in its own context, and every key the context binds is on the row (the row cannot disagree with the keys)") {
  for (const app::OverlayId id : kModal) {
    const app::KeyPrompt* prompt = app::owner_prompt(id);
    NEED(prompt != nullptr);
    const std::string name = "overlay " + std::to_string(static_cast<unsigned>(id));
    unsigned plain_keys = 0;  // the F-keys the prompt names on the plain layer
    for (unsigned i = 0; i < prompt->count; ++i) {
      const app::KeyOption& o = prompt->options[i];
      const std::string where = name + " option " + std::to_string(i) + " '" + o.label + "'";
      CHECK_MESSAGE(o.label[0] != '\0', where << " is live with a blank label");
      CHECK_MESSAGE(std::strlen(o.label) <= 8, where << " does not fit a key cap");
      const test::KeyBinding b = resolve_in_overlay(id, o.key);
      CHECK_MESSAGE(std::strcmp(o.label, "-") != 0, where << " is a '-' in the table (owner_row makes those of a Shift key with no option)");
      CHECK_MESSAGE(b.bound, where << " is not bound to an action");
      if (o.key < 8) {
        plain_keys |= 1u << o.key;
      }
    }
    if (id == app::OverlayId::Navigator) {  // the Shift keys it declares no option for read "-" (owner_row): they must be bound to nothing
      for (std::uint8_t key = 8; key < 14; ++key) {
        CHECK_MESSAGE(!resolve_in_overlay(id, key).bound, name << " Shift F" << key - 7 << " is bound to something");
      }
    }
    // The other way: an F-key the context binds to a real action has a label (a bound key left blank would do something the row does not say).
    for (std::uint8_t key = 0; key < 8; ++key) {
      const test::KeyBinding b = resolve_in_overlay(id, key);
      const bool on_row = ((plain_keys >> key) & 1u) != 0u;
      CHECK_MESSAGE((on_row || !b.bound), name << " F" << key + 1 << " is bound and not on the row");
    }
  }
}

TEST_CASE("key prompt: a dialog's row is its own keys, every other key blank, the row of a dialog does not follow Shift unless it declares a Shift layer") {
  app::Model model;
  app::ViewState view;
  view.screen = ScreenId::Pattern;
  // Fill: F1 FILL, F8 BACK.
  view.overlay = app::OverlayId::Fill;
  {
    const test::RowRead row = test::read_owner_row(model, view);
    CHECK(row.marked);
    CHECK(row.label[0] == "FILL");
    CHECK(row.label[7] == "BACK");
    for (unsigned i = 1; i < 7; ++i) {
      CHECK(row.label[i].empty());
    }
    CHECK(row.hot == 0u);
    const test::RowRead shifted = test::read_owner_row(model, view, true);  // no Shift options: the plain row is the truth (Shift + Fn acts as Fn)
    for (unsigned i = 0; i < 8; ++i) {
      CHECK(shifted.label[i] == row.label[i]);
    }
  }
  // The Navigator declares the layer: Shift + F1..F6 read "-", F7 CLOSE and F8 GO stay.
  view.overlay = app::OverlayId::Navigator;
  view.navigator = 0;
  {
    const test::RowRead row = test::read_owner_row(model, view);
    const char* const want[8] = {"GO", "", "", "", "", "", "CLOSE", "GO"};
    for (unsigned i = 0; i < 8; ++i) {
      CHECK_MESSAGE(row.label[i] == want[i], "Navigator F" << i + 1);
    }
    const test::RowRead shifted = test::read_owner_row(model, view, true);
    for (unsigned i = 0; i < 8; ++i) {
      CHECK_MESSAGE(shifted.label[i] == (i < 6 ? std::string("-") : std::string(want[i])), "Navigator Shift F" << i + 1);
    }
  }
  // The choosers: PATTERN LENGTH JUMP, the open one inverted, BACK on F8.
  const struct {
    app::OverlayId id;
    unsigned hot;
  } choosers[] = {{app::OverlayId::PatternChooser, 0x01u}, {app::OverlayId::LengthChooser, 0x02u}, {app::OverlayId::JumpChooser, 0x04u}};
  for (const auto& c : choosers) {
    view.overlay = c.id;
    const test::RowRead row = test::read_owner_row(model, view);
    // F1 is the PATTERN chooser's held key (OS-PATSWITCH: a tap of F1 opens nothing, in any chooser): live there, blank in the other two
    CHECK_MESSAGE(row.label[0] == (c.id == app::OverlayId::PatternChooser ? "PATTERN" : ""), "chooser " << static_cast<unsigned>(c.id));
    CHECK(row.label[1] == "LENGTH");
    CHECK(row.label[2] == "JUMP");
    CHECK(row.label[7] == "BACK");
    CHECK(row.hot == c.hot);
    for (unsigned i = 3; i < 7; ++i) {
      CHECK(row.label[i].empty());
    }
  }
  // The keyboard: Save As has AUTO and SAVE, a name has OK and no AUTO.
  view.overlay = app::OverlayId::TextEntry;
  view.text.target = app::TextEntryView::Target::SaveAs;
  CHECK(test::read_owner_row(model, view).label[5] == "AUTO");
  CHECK(test::read_owner_row(model, view).label[7] == "SAVE");
  view.text.target = app::TextEntryView::Target::Instrument;
  CHECK(test::read_owner_row(model, view).label[5].empty());
  CHECK(test::read_owner_row(model, view).label[7] == "OK");
  CHECK(test::read_owner_row(model, view).label[0] == "TYPE");
  CHECK(test::read_owner_row(model, view).label[1] == "CAPS");
  CHECK(test::read_owner_row(model, view).label[6] == "CANCEL");
  // The step popup and the export progress: F8 CANCEL alone.
  for (const app::OverlayId id : {app::OverlayId::StepPopup, app::OverlayId::ExportProgress}) {
    view.overlay = id;
    const test::RowRead row = test::read_owner_row(model, view);
    CHECK(row.label[7] == "CANCEL");
    for (unsigned i = 0; i < 7; ++i) {
      CHECK(row.label[i].empty());
    }
  }
  // The picker and the play mode list: SELECT and BACK.
  for (const app::OverlayId id : {app::OverlayId::ModulePicker, app::OverlayId::PlayMode}) {
    view.overlay = id;
    const test::RowRead row = test::read_owner_row(model, view);
    CHECK(row.label[0] == "SELECT");
    CHECK(row.label[7] == "BACK");
  }
  // Learn without a target and without bindings: BACK alone; its CLEAR ALL asks and then reads CLR ALL? inverted.
  model.device.init(core::ParamKind::Device);  // (a bare Model's settings block holds whatever the stack held until init(): its Learn store must start empty)
  view.overlay = app::OverlayId::Learn;
  {
    const test::RowRead row = test::read_owner_row(model, view);
    CHECK(row.label[7] == "BACK");
    for (unsigned i = 0; i < 7; ++i) {
      CHECK_MESSAGE(row.label[i].empty(), "Learn F" << i + 1 << " with nothing to act on");
    }
  }
}

TEST_CASE("key prompt: the question of the screen under a dialog marks nothing (the dialog owns the row)") {
  FileRig rig;
  rig.view.file.new_choice = true;
  rig.say(text_of(Ask::New, app::kAskHeadNew));
  CHECK(rig.row().marked);
  rig.view.overlay = app::OverlayId::Fill;
  const app::PromptState state = app::prompt_in_force(rig.model, rig.view);
  CHECK(state.prompt == app::owner_prompt(app::OverlayId::Fill));
  CHECK(rig.view.overlay_keys());
  CHECK_FALSE(app::follows_shift(app::kFileScreen, rig.view));
}

TEST_CASE("key prompt: overlay_keys is true for every overlay a screen can be under except None and Notify, and follows_shift agrees with it") {
  app::ViewState view;
  for (unsigned v = 0; v < 64; ++v) {
    view.overlay = static_cast<app::OverlayId>(v);
    const bool owns = view.overlay != app::OverlayId::None && view.overlay != app::OverlayId::Notify;
    CHECK_MESSAGE(view.overlay_keys() == owns, "overlay " << v);
    const app::ScreenTable table = app::screen_registry();
    for (std::size_t i = 0; i < table.count; ++i) {
      if (table.items[i].screen != nullptr) {
        const bool expected = !owns && !table.items[i].screen->captures();
        CHECK_MESSAGE(app::follows_shift(*table.items[i].screen, view) == expected, table.items[i].name << " under overlay " << v);
      }
    }
  }
  CHECK_FALSE(app::overlay_of(app::OverlayId::Notify)->modal());  // (Notify is never state, and the predicate does not treat a notice as a dialog: the loop above says so)
}

// ---- The boot disk (S3) -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("key prompt: the boot disk owns the row through the one predicate: every DiskKeys but Off, on every screen, with or without Shift") {
  const app::ScreenTable table = app::screen_registry();
  for (const app::DiskKeys keys : {app::DiskKeys::Restart, app::DiskKeys::Confirm, app::DiskKeys::Finishing}) {
    app::ViewState view;  // (overlay None: the disk strip is a notice on the app's own screen)
    view.disk_keys = keys;
    CHECK(view.overlay_keys());
    for (std::size_t i = 0; i < table.count; ++i) {
      if (table.items[i].screen != nullptr) {
        CHECK_MESSAGE(!app::follows_shift(*table.items[i].screen, view), table.items[i].name << " with disk keys " << static_cast<unsigned>(keys));
      }
    }
  }
  app::ViewState off;
  CHECK_FALSE(off.overlay_keys());
  off.disk_keys = app::DiskKeys::Off;
  CHECK_FALSE(off.overlay_keys());
}

TEST_CASE("key prompt: the boot disk's row is eight RESTART, eight AGAIN? inverted while the computer may be writing, and blank while the restart waits") {
  app::Model model;
  app::ViewState view;
  view.screen = ScreenId::Pattern;
  view.disk_keys = app::DiskKeys::Restart;
  for (const bool shifted : {false, true}) {  // Shift does not change it: a key or pad is a key or pad
    const test::RowRead row = test::read_owner_row(model, view, shifted);
    CHECK(row.marked);
    for (unsigned i = 0; i < 8; ++i) {
      CHECK_MESSAGE(row.label[i] == "RESTART", "F" << i + 1);
    }
    CHECK(row.hot == 0u);
  }
  view.disk_keys = app::DiskKeys::Confirm;
  {
    const test::RowRead row = test::read_owner_row(model, view);
    for (unsigned i = 0; i < 8; ++i) {
      CHECK_MESSAGE(row.label[i] == "AGAIN?", "F" << i + 1);
    }
    CHECK(row.hot == 0xFFu);
  }
  view.disk_keys = app::DiskKeys::Finishing;
  {
    const test::RowRead row = test::read_owner_row(model, view);
    CHECK(row.marked);  // owned, and blank: no key acts any more
    for (unsigned i = 0; i < 8; ++i) {
      CHECK_MESSAGE(row.label[i].empty(), "F" << i + 1);
    }
    CHECK(row.hot == 0u);
  }
  view.disk_keys = app::DiskKeys::Off;
  CHECK_FALSE(test::read_owner_row(model, view).marked);
  // The disk row wins over everything under it: a question of the screen, or an overlay left open.
  view.disk_keys = app::DiskKeys::Restart;
  view.overlay = app::OverlayId::Fill;
  CHECK(test::read_owner_row(model, view).label[0] == "RESTART");
}

TEST_CASE("key prompt: App::set_disk_keys changes the view and asks for a redraw only when the row changes") {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  const std::unique_ptr<app::App> application = std::make_unique<app::App>(input, clock, display);
  application->run_once();
  CHECK(application->state().disk_keys == app::DiskKeys::Off);
  application->set_disk_keys(app::DiskKeys::Restart);
  CHECK(application->state().disk_keys == app::DiskKeys::Restart);
  CHECK(application->state().overlay_keys());
  CHECK(application->dirty());
  application->run_once();  // the frame is drawn
  CHECK_FALSE(application->dirty());
  application->set_disk_keys(app::DiskKeys::Restart);  // the platform tells it every pass: no change, no frame
  CHECK_FALSE(application->dirty());
  application->set_disk_keys(app::DiskKeys::Confirm);
  CHECK(application->dirty());
}

TEST_CASE("key prompt: INPUT TEST labels its two chords: F2 and F3 CLEAR, F4 and F5 EXIT, the rest blank, and the chords do what the labels say") {
  const char* const* row = app::kInputTestScreen.soft_keys();
  const char* const want[8] = {"", "CLEAR", "CLEAR", "EXIT", "EXIT", "", "", ""};
  for (unsigned i = 0; i < 8; ++i) {
    CHECK_MESSAGE(std::string(row[i]) == want[i], "F" << i + 1);
  }
  CHECK(app::kInputTestScreen.captures());
  const core::BindingSet set{{core::BindingView{}, core::BindingView{}, app::builtin_bindings()}};
  core::ContextChain chain;
  (void)chain.push(app::context_id(app::Context::InputTest), true);
  (void)chain.push(app::context_id(app::Context::Global));
  const auto chord_of = [&](app::Button a, app::Button b) {
    const core::Trigger t = core::chord(app::button_ref(a), app::button_ref(b));
    return core::resolve(set, chain, core::Gesture{0, core::GestureKind::Chord, t.input, t.modifier, 0});
  };
  const core::Resolution leave = chord_of(app::Button::F4, app::Button::F5);
  CHECK(leave.status == core::Resolution::Status::Bound);
  CHECK(leave.action == core::action_id("nav.screen"));
  CHECK(leave.arg == static_cast<core::ActionArg>(ScreenId::Pattern));
  const core::Resolution clear = chord_of(app::Button::F2, app::Button::F3);
  CHECK(clear.status == core::Resolution::Status::Bound);
  CHECK(clear.action == core::action_id("input_test.reset"));
  // Every other F-key alone is swallowed: nothing acts, so nothing is labelled.
  for (const app::Button b : {app::Button::F1, app::Button::F6, app::Button::F7, app::Button::F8}) {
    const core::Gesture g{0, core::GestureKind::Tap, app::button_ref(b), core::kNoInput, 0};
    CHECK(core::resolve(set, chain, g).status == core::Resolution::Status::Captured);
  }
}

// ---- OS-SONGMAP: the Song screen's question (1.6) and its TOOLS dialog ------------------------------------------------------------------------------------------------------

TEST_CASE("key prompt: the Song question marks F1 ALL and F2 UNIQUE, inverted, only while its text is the head of the strip; they are bound to song.answer in the screen's field context") {
  app::Model model;
  app::ViewState view;
  view.screen = ScreenId::Song;
  const auto row = [&](bool shifted = false) { return test::read_row(model, view, app::kSongScreen, shifted); };
  CHECK_FALSE(row().marked);
  // The text as the action leaves it (the head from the same function the question uses).
  view.song.ask.live = true;
  view.song.ask.shared = 1;
  view.song.ask.first = 3;
  view.song.ask.others = 2;
  AskText head;
  app::song_ask_head(view.song.ask, head);
  CHECK(std::string(head.c_str()) == "PATTERN 03 ALSO IN 2 SLOTS");
  CHECK_FALSE(app::song_question_live(view));  // nothing on the strip yet: the question is not up
  CHECK_FALSE(row().marked);
  AskText text;
  app::ask_text(Ask::Shared, head.c_str(), text);
  CHECK(view.notify.post(core::Severity::Warn, text.c_str(), "song", app::ViewState::kMessageMs));
  CHECK(app::song_question_live(view));
  const test::RowRead marked = row();
  CHECK(marked.marked);
  CHECK(marked.label[0] == "ALL");
  CHECK(marked.label[1] == "UNIQUE");
  CHECK(marked.label[2] == "ADD SLOT");  // the others keep their labels: they still work and take the question back
  CHECK(marked.label[7] == "UNDO");
  CHECK(marked.hot == 0x03u);
  CHECK_FALSE(row(true).marked);  // the Shift layer is the screen's own
  // F1 and F2 resolve to the answer, F3 to the screen's own key
  const test::KeyBinding f1 = test::resolve_key(model, view, app::kSongScreen, 0);
  const test::KeyBinding f2 = test::resolve_key(model, view, app::kSongScreen, 1);
  const test::KeyBinding f3 = test::resolve_key(model, view, app::kSongScreen, 2);
  CHECK(f1.action == core::action_id("song.answer"));
  CHECK(f2.action == core::action_id("song.answer"));
  CHECK(f3.action == core::action_id("song.add_slot"));
  // the question is a Song screen's: on another screen nothing is marked and F1 is not the answer
  view.screen = ScreenId::Pattern;
  CHECK_FALSE(app::song_question_live(view));
  // a head several patterns long
  view.song.ask.shared = 3;
  view.song.ask.others = 9;
  app::song_ask_head(view.song.ask, head);
  CHECK(std::string(head.c_str()) == "3 PATTERNS ALSO IN 9 SLOTS");
  view.song.ask.shared = 1;
  view.song.ask.others = 1;
  app::song_ask_head(view.song.ask, head);
  CHECK(std::string(head.c_str()) == "PATTERN 03 ALSO IN 1 SLOT");
  // the longest head still fits the strip with its keys
  view.song.ask.shared = 128;
  view.song.ask.others = 255;
  app::song_ask_head(view.song.ask, head);
  app::ask_text(Ask::Shared, head.c_str(), text);
  CHECK(text.size() <= core::kNotifyTextChars);
}

TEST_CASE("key prompt: the Song TOOLS dialog owns the row: F1 APPLY, F8 BACK, every other key blank, and no Shift layer") {
  app::Model model;
  app::ViewState view;
  view.screen = ScreenId::Song;
  view.overlay = app::OverlayId::SongTools;
  const test::RowRead row = test::read_owner_row(model, view);
  CHECK(row.marked);
  CHECK(row.label[0] == "APPLY");
  CHECK(row.label[7] == "BACK");
  for (unsigned i = 1; i < 7; ++i) {
    CHECK(row.label[i].empty());
  }
  CHECK(row.hot == 0u);
  const test::RowRead shifted = test::read_owner_row(model, view, true);
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(shifted.label[i] == row.label[i]);
  }
  char keys[app::kKeysLineChars + 1];
  CHECK(app::describe_keys(model, view, keys, sizeof keys) > 0u);
  CHECK(std::string(keys) == "F1:APPLY,F8:BACK");
}
