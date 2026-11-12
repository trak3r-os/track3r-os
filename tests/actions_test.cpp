// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/model.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/view_state.h>
#include <o3os/core/action.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

using namespace o3os;
using app::ActionContext;
using app::ActionDef;
using app::ActionTable;
using app::Column;
using app::DispatchResult;
using core::ActionArg;
using core::ActionId;

namespace {

struct Rig {
  std::unique_ptr<app::Model> model = std::make_unique<app::Model>();
  app::ViewState view;
  ActionContext ctx{*model, view, app::App::kSoftKeyLabels, 1000, true};

  Rig() {
    app::fill_demo(model->pattern);
    app::fill_song_demo(model->song);
  }

  DispatchResult run(const char* name, ActionArg arg = 0) {
    return app::dispatch(app::action_registry(), core::action_id(name), arg, ctx);
  }
};

}  // namespace

TEST_CASE("actions: the registry is well formed and every action can be found by id and by name") {
  const ActionTable t = app::action_registry();
  CHECK(t.count >= 10u);
  CHECK(app::registry_valid(t.items, t.count));
  for (std::size_t i = 0; i < t.count; ++i) {
    const ActionDef& a = t.items[i];
    CHECK_MESSAGE(a.handler != nullptr, a.name);  // not checkable at compile time (see actions.h)
    CHECK(app::find_action(t, a.id) == &a);
    CHECK(app::find_action(t, a.name, std::strlen(a.name)) == &a);
    CHECK(a.id == core::action_id(a.name));
    for (std::size_t j = 0; j < i; ++j) {
      CHECK(std::strcmp(t.items[j].name, a.name) != 0);
    }
  }
}

TEST_CASE("actions: names are looked up whole and case-insensitively") {
  const ActionTable t = app::action_registry();
  const ActionDef* left = app::find_action(t, "nav.left", 8);
  CHECK(left != nullptr);
  CHECK(app::find_action(t, "NAV.Left", 8) == left);
  CHECK(app::find_action(t, "nav.lef", 7) == nullptr);       // a prefix is not the name
  CHECK(app::find_action(t, "nav.leftx", 9) == nullptr);     // nor is an extension of it
  CHECK(app::find_action(t, "nav.left", 4) == nullptr);
  CHECK(app::find_action(t, "", 0) == nullptr);
  CHECK(app::find_action(t, "none", 4) == nullptr);
  const char with_nul[] = {'n', 'a', 'v', '\0', 'x'};
  CHECK(app::find_action(t, with_nul, 5) == nullptr);        // stops at the name's end, never reads past it
  CHECK(app::find_action(t, ActionId::None) == nullptr);
}

TEST_CASE("actions: the registry checker rejects what it must") {
  const auto noop = [](ActionContext&, ActionArg) noexcept { return false; };
  const ActionDef ok[] = {app::make_action("a.b", noop), app::make_action("c.d", noop)};
  CHECK(app::registry_valid(ok, 2));
  const ActionDef dup[] = {app::make_action("a.b", noop), app::make_action("a.b", noop)};
  CHECK_FALSE(app::registry_valid(dup, 2));
  const ActionDef no_dot[] = {app::make_action("nodot", noop)};
  CHECK_FALSE(app::registry_valid(no_dot, 1));
  const ActionDef upper[] = {app::make_action("A.b", noop)};
  CHECK_FALSE(app::registry_valid(upper, 1));
  const ActionDef spaced[] = {app::make_action("a b.c", noop)};
  CHECK_FALSE(app::registry_valid(spaced, 1));
  const ActionDef empty[] = {app::make_action("", noop)};
  CHECK_FALSE(app::registry_valid(empty, 1));
  ActionDef wrong_id = app::make_action("a.b", noop);
  wrong_id.id = core::action_id("a.c");
  CHECK_FALSE(app::registry_valid(&wrong_id, 1));
  ActionDef none_id = app::make_action("a.b", noop);
  none_id.id = ActionId::None;
  CHECK_FALSE(app::registry_valid(&none_id, 1));
}

TEST_CASE("actions: dispatch passes the argument and reports whether anything visible changed") {
  Rig r;
  CHECK(r.run("nav.down") == DispatchResult::Changed);
  CHECK(r.view.pattern.row() == 1);
  CHECK(r.run("nav.up") == DispatchResult::Changed);
  r.ctx.repeat = true;
  CHECK(r.run("nav.up") == DispatchResult::Unchanged);  // at the top: an auto-repeat never wraps
  r.ctx.repeat = false;
  CHECK(r.run("nav.up") == DispatchResult::Changed);    // OS-ROWWRAP: a fresh press wraps to the last row
  CHECK(r.run("nav.down") == DispatchResult::Changed);  // and a fresh Down on the last row back to the first
  CHECK(r.view.pattern.row() == 0);
  CHECK(r.run("nav.right") == DispatchResult::Changed);
  CHECK(r.view.pattern.column() == Column::Instr);
  CHECK(r.run("nav.left") == DispatchResult::Changed);
  CHECK(r.run("nav.left") == DispatchResult::Unchanged);

  CHECK(r.run("nav.scroll", 8) == DispatchResult::Changed);
  CHECK(r.view.pattern.row() == 8);
  CHECK(r.run("nav.scroll", -3) == DispatchResult::Changed);
  CHECK(r.view.pattern.row() == 5);
  CHECK(r.run("nav.scroll", 0) == DispatchResult::Unchanged);
  CHECK(r.run("nav.scroll", 1000) == DispatchResult::Changed);
  CHECK(r.view.pattern.row() == 63);

  CHECK(r.run("pattern.column", static_cast<ActionArg>(Column::Vol)) == DispatchResult::Changed);
  CHECK(r.view.pattern.column() == Column::Vol);
  CHECK(r.run("pattern.column", static_cast<ActionArg>(Column::Vol)) == DispatchResult::Unchanged);
  CHECK(r.run("pattern.column", static_cast<ActionArg>(Column::Fx2)) == DispatchResult::Changed);
  CHECK(r.view.pattern.column() == Column::Fx2);
  CHECK(r.run("pattern.column", 5) == DispatchResult::Unchanged);  // there is no such column: refused, not wrapped
  CHECK(r.run("pattern.column", -1) == DispatchResult::Unchanged);
  CHECK(r.view.pattern.column() == Column::Fx2);

  CHECK(r.run("pattern.fx_column") == DispatchResult::Changed);  // from FX2 the cycle goes to FX1
  CHECK(r.view.pattern.column() == Column::Fx1);
  CHECK(r.run("pattern.fx_column") == DispatchResult::Changed);
  CHECK(r.view.pattern.column() == Column::Fx2);
}

TEST_CASE("actions: instrument.select takes an instrument number and refuses the rest") {
  Rig r;
  CHECK(r.view.instrument == 1);
  CHECK(r.run("instrument.select", 4) == DispatchResult::Changed);
  CHECK(r.view.instrument == 4);
  CHECK(r.run("instrument.select", 4) == DispatchResult::Unchanged);
  CHECK(r.run("instrument.select", 48) == DispatchResult::Changed);
  CHECK(r.view.instrument == 48);
  for (ActionArg bad : {0, 49, -5, 2147483647, static_cast<ActionArg>(-2147483647 - 1)}) {
    CHECK(r.run("instrument.select", bad) == DispatchResult::Unchanged);
    CHECK(r.view.instrument == 48);
  }
}

TEST_CASE("actions: ui.softkey posts the screen's label as an info notification, learn.toggle opens and closes Learn") {
  Rig r;
  CHECK(r.view.notify.empty());
  CHECK(r.run("ui.softkey", 2) == DispatchResult::Changed);
  CHECK(r.view.notify.size() == 1u);
  CHECK(std::string(r.view.notify.head().text.c_str()) == "F3 JUMP");
  CHECK(r.view.notify.head().severity == core::Severity::Info);
  CHECK(std::string(r.view.notify.head().source.c_str()) == "ui");
  CHECK(r.view.notify.head().ttl_ms == app::ViewState::kMessageMs);
  CHECK(std::string(r.view.shown_message()) == "F3 JUMP");
  CHECK(r.run("ui.softkey", 7) == DispatchResult::Changed);  // queued behind the first
  CHECK(r.view.notify.size() == 2u);
  CHECK(std::string(r.view.notify.at(1).text.c_str()) == "F8 UNDO");
  CHECK(std::string(r.view.shown_message()) == "F3 JUMP");   // the head is what is shown
  CHECK(r.run("ui.softkey", 8) == DispatchResult::Unchanged);
  CHECK(r.run("ui.softkey", -1) == DispatchResult::Unchanged);
  CHECK(r.view.notify.size() == 2u);

  // The labels are the screen in view's own: the context carries its table.
  const char* const other[8] = {"AA", "BB", "CC", "DD", "EE", "FF", "GG", "HH"};
  r.ctx.soft_keys = other;
  r.view.notify.clear();
  CHECK(r.run("ui.softkey", 1) == DispatchResult::Changed);
  CHECK(std::string(r.view.notify.head().text.c_str()) == "F2 BB");

  CHECK(r.view.overlay == app::OverlayId::None);
  CHECK(r.run("learn.toggle") == DispatchResult::Changed);
  CHECK(r.view.overlay == app::OverlayId::Learn);
  CHECK(std::string(r.view.shown_message()) == "");  // a modal overlay hides the notification strip
  CHECK(r.view.shown_overlay() == app::OverlayId::Learn);
  CHECK(r.run("learn.toggle") == DispatchResult::Changed);
  CHECK(r.view.overlay == app::OverlayId::None);
  CHECK(r.view.shown_overlay() == app::OverlayId::Notify);  // and the queue was waiting behind it
  // Learn does not open over the Navigator.
  r.view.overlay = app::OverlayId::Navigator;
  CHECK(r.run("learn.toggle") == DispatchResult::Unchanged);
  CHECK(r.view.overlay == app::OverlayId::Navigator);
}

TEST_CASE("actions: unknown ids, focus and the enabled predicate") {
  Rig r;
  CHECK(app::dispatch(app::action_registry(), ActionId::None, 0, r.ctx) == DispatchResult::Unknown);
  CHECK(app::dispatch(app::action_registry(), core::action_id("no.such"), 0, r.ctx) == DispatchResult::Unknown);

  // An action that needs focus does nothing while a modal overlay has it; one that does not still runs.
  r.ctx.focus = false;
  CHECK(r.run("pattern.column", 2) == DispatchResult::Disabled);
  CHECK(r.run("view.mode_next") == DispatchResult::Disabled);
  CHECK(r.run("navigator.open") == DispatchResult::Disabled);
  CHECK(r.view.pattern.column() == Column::Note);
  CHECK(r.run("learn.toggle") == DispatchResult::Changed);
  CHECK(r.run("instrument.select", 3) == DispatchResult::Changed);
  CHECK(r.run("ui.back") == DispatchResult::Changed);
  // The cursor actions need no focus: they move whatever owns the input (here the base screen: no overlay is in the view).
  CHECK(r.run("nav.down") == DispatchResult::Changed);
  r.ctx.focus = true;
  CHECK(r.run("nav.down") == DispatchResult::Changed);

  // A registry of its own, with a predicate: the handler is not called while it says no.
  static int calls;
  static bool allowed;
  calls = 0;
  allowed = false;
  const ActionDef table[] = {
      app::make_action("t.count",
                       [](ActionContext&, ActionArg arg) noexcept {
                         calls += arg;
                         return true;
                       },
                       0, [](const ActionContext&) noexcept { return allowed; }),
      app::make_action("t.always", [](ActionContext&, ActionArg) noexcept { return false; }),
  };
  const ActionTable t{table, 2};
  CHECK(app::registry_valid(table, 2));
  CHECK(app::dispatch(t, core::action_id("t.count"), 5, r.ctx) == DispatchResult::Disabled);
  CHECK(calls == 0);
  allowed = true;
  CHECK(app::dispatch(t, core::action_id("t.count"), 5, r.ctx) == DispatchResult::Changed);
  CHECK(app::dispatch(t, core::action_id("t.count"), -2, r.ctx) == DispatchResult::Changed);
  CHECK(calls == 3);  // the argument arrived as given, including its sign
  CHECK(app::dispatch(t, core::action_id("t.always"), 0, r.ctx) == DispatchResult::Unchanged);
  CHECK(app::dispatch(t, core::action_id("nav.down"), 0, r.ctx) == DispatchResult::Unknown);  // not in this table
}

TEST_CASE("actions: the flags say what the machinery around an action may do with it") {
  const ActionTable t = app::action_registry();
  const auto flags = [&](const char* name) {
    const ActionDef* a = app::find_action(t, name, std::strlen(name));
    return a != nullptr ? a->flags : core::ActionFlags{0};
  };
  // The cursor actions work on whatever owns the input (the grid, the list, the Navigator while it is up): repeatable,
  // and they do not need the base screen's focus.
  for (const char* name : {"nav.left", "nav.right", "nav.up", "nav.down", "nav.scroll", "cursor.page", "cursor.track"}) {
    CHECK_MESSAGE(flags(name) == core::kActionRepeatable, name);
  }
  for (const char* name : {"cursor.jump_fraction", "cursor.jump_end", "cursor.home", "cursor.end", "nav.screen",
                           "navigator.accept", "ui.back", "learn.toggle"}) {
    CHECK_MESSAGE(flags(name) == 0, name);
  }
  // What only makes sense with the base screen in charge needs its focus.
  for (const char* name : {"pattern.column", "pattern.fx_column", "view.mode", "view.mode_next", "navigator.open", "apps.open", "edit.undo",
                           "edit.redo", "macro.play"}) {
    CHECK_MESSAGE(flags(name) == core::kActionNeedsFocus, name);
  }
  // OS-APPSNAV: the apps list's own actions run in a modal overlay (no focus); the pick is an undo unit only when a document is open (the apps list's F1 launches and needs none).
  for (const char* name : {"apps.close"}) {
    CHECK_MESSAGE(flags(name) == 0, name);
  }
  CHECK(flags("module.pick") == core::kActionUndoableIfOpen);
  // The history and the macro recorder are not edits themselves; the only action that edits the project is project.tempo
  // (OS-002c), and it is the one undoable action (the dispatcher hands the editor to it and to nothing else).
  for (const char* name : {"macro.record", "job.cancel"}) {
    CHECK_MESSAGE(flags(name) == 0, name);
  }
  // OS-009 P1a.1: the pattern's edit actions (step.*, track.mute and solo) are undoable too, and say NO PROJECT without a document.
  for (std::size_t i = 0; i < t.count; ++i) {
    bool pattern_edit = std::strncmp(t.items[i].name, "step.", 5) == 0 || (std::strncmp(t.items[i].name, "track.", 6) == 0 && std::strcmp(t.items[i].name, "track.mute_mode") != 0);  // (OS-MUTE: the mute mode is runtime state, not an edit)
    // OS-011a: the Wave / Chop screen's edits of the instrument are undo units too (the rest of wave.* is view state).
    for (const char* wave_edit : {"wave.nudge", "wave.nudge_coarse", "wave.add", "wave.del", "wave.even", "wave.to_pat", "wave.auto_apply", "sample.commit", "sedit.undo", "sedit.redo",

                                   "preset.load", "rec.note_off", "instrument.engine", "instrument.play_mode", "wave.mode_pick", "instrument.delete", "instrument.init", "instrument.new_midi", "instrument.paste",
                                   "song.add_slot", "song.add_empty", "song.delete_slot", "song.pattern_delta", "song.turn", "song.turn_fine", "song.tempo_hold", "song.paste", "song.clear", "song.cut", "song.move", "song.move_along", "song.unique", "song.answer", "songtools.apply", "block.paste", "pattern.op", "pattern.clip_paste", "pattern.fill", "tools.repeat", "verb.transpose", "verb.instrument", "verb.interpolate", "verb.randomise", "verb.humanise", "verb.rotate_up", "verb.rotate_down", "ptool.apply", "fill.apply", "pattern.invert", "file.purge", "events.edit", "events.coarse", "events.pad", "events.insert", "events.delete", "events.quant"}) {  // OS-MIDIREC S2b/S2c/S3: the EVENTS list's edits (the encoder, Shift + encoder, the pads, Insert, Delete, QUANT) are one undo unit each  // OS-PTOOLS2 S3: TOOLS REPEAT is one undo unit  // OS-FILES part B: PURGE UNUSED is one undo unit  // PAT-OPS: DUPLICATE / EXPAND / SHRINK and the whole-pattern paste are one unit each  // OS-CFGWIRE: FILL and INVERT are block edits; OS-PTOOLS2 S3b: the five S4 verbs of the menu are block edits too; OS-PTOOLS2 S2: the Pattern TOOLS dialog's APPLY  // OS-SONG1: the Song screen's slot edits (ADD EMPTY too); OS-SONGMAP: Delete, UNIQUE, the question's answer and the TOOLS dialog's APPLY write the cells' patterns; OS-SONGSHARE: CUT and MOVE  // OS-012b: the engine pick writes the instrument and its default parameters; OS-FU2: a recorded note's release writes its OFF; OS-004d: the playback engine


      pattern_edit = pattern_edit || std::strcmp(t.items[i].name, wave_edit) == 0;
    }
    const bool undoable = std::strcmp(t.items[i].name, "project.tempo") == 0 || std::strcmp(t.items[i].name, "module.app_tempo") == 0 || std::strcmp(t.items[i].name, "module.app_group") == 0 || pattern_edit;  // OS-H1: an app's queued set_tempo, one undo unit like project.tempo; OS-H3A: an app's closed edit group, one unit
    CHECK_MESSAGE(((t.items[i].flags & core::kActionUndoable) != 0) == undoable, t.items[i].name);
    CHECK_MESSAGE(((t.items[i].flags & core::kActionSaysNoProject) != 0) == pattern_edit, t.items[i].name);
  }
}
