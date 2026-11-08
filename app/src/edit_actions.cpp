// SPDX-License-Identifier: GPL-3.0-or-later
// History and macros (OS-002c): undo, redo, the macro recorder, and `project.tempo`, the one undoable action of this slice.
#include "action_handlers.h"
#include "param_access.h"
#include "session_access.h"

#include <o3os/app/module_ui.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/sample_bank.h>
#include <o3os/model/params.h>

#include <cstring>

namespace o3os::app {

namespace {

bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return say_tag(ctx, severity, text, "history");
}

model::Editor* editor_of(const ActionContext& ctx) noexcept {
  return ctx.session != nullptr ? SessionAccess::editor(*ctx.session) : nullptr;
}

bool undo_or_redo(ActionContext& ctx, bool undo) noexcept {
  model::Editor* e = editor_of(ctx);
  // OS-UNDOGRP: the area of the screen F8 is pressed on; a screen with none of its own has no undo (D1, D2).
  const model::UndoArea area = screen_undo_area(ctx.view.screen);
  if (area == model::UndoArea::Auto || area == model::UndoArea::None ||
      (ctx.view.screen == ScreenId::Instrument && params_insert_target(ctx.view))) {  // OS-INSFX S5: an insert's edits are not journalled (D9): nothing of theirs to undo
    return say(ctx, core::Severity::Info, undo ? "NOTHING TO UNDO HERE" : "NOTHING TO REDO HERE");
  }
  rec_take_end(e, ctx.view.edit);  // OS-LIVEREC: a take in progress is closed first (it cannot be undone from inside its group), so that Undo takes what was recorded so far; recording goes on in a new take
  // The generation of every instrument before the unit is taken back or done again (256 bytes of stack): a unit that replaced a whole instrument (INIT, DELETE, an engine or play mode change, a
  // paste) is not in the applied list, so the slots it changed are rebound below, and a module's running instance is told its restored parameters (OS-FU5, OS-INSTUI S5).
  std::uint32_t generations[model::kMaxInstruments] = {};
  if (e != nullptr) {
    for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
      generations[i] = e->project().instrument_generation(i);
    }
  }
  // The unit about to be undone / done again, read once before the walk (undo() moves it across the cursor): OS-H3A, the unit an app's edit group made carries the app's catalog entry and the notice names
  // the app (UNDO RHYTHM GEN); OS-INSTUI S7, a unit that put an instrument and its audio in a slot (PASTE, LOAD) is followed by the bank's audio.
  core::ActionId unit_action = core::ActionId::None;
  core::ActionArg unit_arg = 0;
  const bool have_unit = e != nullptr && (undo ? e->journal(area).newest_undo(unit_action, unit_arg) : e->journal(area).next_redo(unit_action, unit_arg));
  const bool app_unit = have_unit && unit_action == core::action_id("module.app_group");
  const bool audio_unit = have_unit && (unit_action == core::action_id("instrument.paste") || unit_action == core::action_id("instrument.load"));
  const bool purge_unit = have_unit && unit_action == core::action_id("file.purge");  // OS-FILES part B: PURGE UNUSED is one unit, and the strip names it
  // OS-MIDIREC S2b: the list's edit units say the event on the strip (spec A12's `UNDO EVENT`). The unit itself is named for
  // the action (the dispatcher names every unit) and carries only its argument (a detent, a pad), not the field edited, so
  // the notice names no field: reading it from the view would say the field in the box now, not the one the unit edited.
  const bool events_unit = have_unit && (unit_action == core::action_id("events.edit") || unit_action == core::action_id("events.coarse") ||
                                         unit_action == core::action_id("events.pad") ||
                                         unit_action == core::action_id("events.insert") || unit_action == core::action_id("events.delete") ||
                                         unit_action == core::action_id("events.quant"));
  const model::UndoResult r = e == nullptr ? model::UndoResult::Nothing : (undo ? e->undo(area) : e->redo(area));
  switch (r) {
    case model::UndoResult::Ok: {
      if (audio_unit && ctx.samples != nullptr) {
        for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
          if (e->project().instrument_generation(i) != generations[i]) {
            (void)(undo ? ctx.samples->audio_undone(i) : ctx.samples->audio_redone(i));  // the slot's audio goes back (or again) with its instrument; the rebind below follows
          }
        }
      }
      // The project changed through its own mutators: the screens see the generations move, and the audio side hears the parameters the
      // unit set by value (OS-002d). A unit that replaced a whole instrument, track or master is not in that list: counted, for the engine
      // to re-read (a session with no engine attached has nothing to tell).
      for (unsigned i = 0; i < e->applied_count(); ++i) {
        send_param_change(ctx, e->applied(i).id, e->applied(i).value);
      }
      if (ctx.samples != nullptr && (ctx.view.screen == ScreenId::Wave || ctx.view.screen == ScreenId::SampleEdit) && ctx.view.instrument >= 1) {
        ctx.samples->request_rebind(static_cast<unsigned>(ctx.view.instrument - 1));  // the instrument in view may have changed: the engine follows (OS-011a)
      }
      if (e->applied_incomplete()) {
        for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
          if (e->project().instrument_generation(i) == generations[i]) {
            continue;
          }
          if (ctx.samples != nullptr) {
            ctx.samples->request_rebind(i);  // the voices follow at once, from any screen; a delete undone (or redone): the slot is the sample's again (or silent)
          }
          const model::Instrument* in = e->project().instrument(i);
          // A sample instrument: every sound parameter an app can write goes to the engine again (the applied list above holds 16 of a unit's changes; OS-H3B: a MUTATE of 61 would leave 45 sounding as they were)
          for (unsigned slot = 0; in != nullptr && in->kind == model::InstrumentKind::Sample && slot < model::instrument_param::kCount; ++slot) {
            const unsigned field = model::param_field_of_slot(core::ParamKind::Instrument, slot);
            const core::ParamId pid = core::param_id(core::ParamKind::Instrument, i, field);
            if (model::instrument_field_app_writable(field)) {
              send_param_change(ctx, pid, e->project().get_param(pid));
            }
          }
          for (unsigned j = 0; in != nullptr && in->kind == model::InstrumentKind::Module && j < in->param_len / 4u && j < model::instrument_param::kModuleMax; ++j) {
            float value = 0.0f;
            std::memcpy(&value, in->params + 4u * j, sizeof value);
            send_param_change(ctx, core::param_id(core::ParamKind::Instrument, i, model::instrument_param::kModuleFirst + j), value);  // the running instance: the blob the unit restored
          }
        }
      }
      if (ctx.model.modules != nullptr) {
        ctx.model.modules->sync_slots(e->project());  // OS-INSTUI S5: an undone delete opens the gate of its module instance again, a redone one closes it
      }
      if (ctx.session != nullptr) {
        ctx.session->reset_learn();  // the Learn store may have changed under the bindings' state
        ctx.session->param_resyncs += e->applied_incomplete() ? 1u : 0u;
      }
      char title[26];
      if (app_unit && ctx.model.modules != nullptr && unit_arg >= 0 && ctx.model.modules->app_title(static_cast<unsigned>(unit_arg), title, sizeof title)) {
        core::FixedString<core::kNotifyTextChars> text;
        text.assign(undo ? "UNDO " : "REDO ");
        text.append(title);
        (void)say(ctx, core::Severity::Info, text.c_str());
      }
      if (purge_unit) {
        (void)say(ctx, core::Severity::Info, undo ? "UNDO PURGE" : "REDO PURGE");
      }
      if (events_unit) {
        (void)say(ctx, core::Severity::Info, undo ? "UNDO EVENT" : "REDO EVENT");
      }
      // D4: the unit made or removed something the other areas' units name: their history is gone, and says so
      if (e->fence_cleared(model::UndoArea::Pattern)) {
        (void)say(ctx, core::Severity::Warn, "PATTERN HISTORY CLEARED");
      }
      if (e->fence_cleared(model::UndoArea::Song)) {
        (void)say(ctx, core::Severity::Warn, "SONG HISTORY CLEARED");
      }
      if (e->fence_cleared(model::UndoArea::Sound)) {
        (void)say(ctx, core::Severity::Warn, "SOUND HISTORY CLEARED");
        if (ctx.samples != nullptr) {
          ctx.samples->drop_undone_swaps();  // the undone units of SOUND are gone: the audio swaps held for them go too
        }
      }
      return true;
    }
    case model::UndoResult::Nothing:
      return say(ctx, core::Severity::Info, undo ? "NOTHING TO UNDO HERE" : "NOTHING TO REDO HERE");
    case model::UndoResult::Busy:
      return false;
    case model::UndoResult::Stale:
      return say(ctx, core::Severity::Warn, "UNDO HISTORY RESET");
    case model::UndoResult::Failed:
      break;
  }
  return say(ctx, core::Severity::Error, undo ? "UNDO FAILED" : "REDO FAILED");
}

}  // namespace

bool edit_undo(ActionContext& ctx, core::ActionArg) noexcept { return undo_or_redo(ctx, true); }
bool edit_redo(ActionContext& ctx, core::ActionArg) noexcept { return undo_or_redo(ctx, false); }

// Tempo up or down by `arg` BPM, clamped to the model's range. The proof of the undoable path: it edits through
// ActionContext::edit, which only an undoable action is given, and the dispatcher makes its edits one undo unit.
bool project_tempo(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.edit == nullptr) {
    return false;
  }
  const std::int64_t now = ctx.edit->project().tempo_x100();
  std::int64_t next = now + static_cast<std::int64_t>(arg) * 100;
  next = next < static_cast<std::int64_t>(model::kMinTempoX100) ? model::kMinTempoX100
                                                                : (next > model::kMaxTempoX100 ? model::kMaxTempoX100 : next);
  return next != now && ctx.edit->set_tempo_x100(static_cast<std::uint32_t>(next)) == model::Status::Ok;
}

// Start recording (the macro is emptied), or stop. While it records every action that ran is appended by dispatch().
bool macro_record(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.session == nullptr || ctx.session->macro.playing) {
    return false;
  }
  Macro& m = ctx.session->macro;
  if (!m.recording) {
    m = Macro{};
    m.recording = true;
    return say(ctx, core::Severity::Info, "MACRO RECORDING");
  }
  m.recording = false;
  core::FixedString<core::kNotifyTextChars> text;
  text.assign(m.full ? "MACRO FULL: " : "MACRO: ");
  char digits[4] = {static_cast<char>('0' + (m.count / 10) % 10), static_cast<char>('0' + m.count % 10), ' ', '\0'};
  text.append(m.count >= 10 ? digits : digits + 1);
  text.append(m.count == 1 ? "STEP" : "STEPS");
  return say(ctx, core::Severity::Info, text.c_str());
}

// Plays the macro: every recorded (action, argument) dispatched again, in order. The whole run is one undo unit (the unit
// that undo takes is the macro, not its last step). Playing while recording, or inside a macro, does nothing.
bool macro_play(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.session == nullptr) {
    return false;
  }
  Macro& m = ctx.session->macro;
  if (m.recording || m.playing) {
    return false;
  }
  if (m.count == 0) {
    return say(ctx, core::Severity::Info, "NO MACRO");
  }
  model::Editor* e = editor_of(ctx);
  rec_take_end(e, ctx.view.edit);  // OS-LIVEREC: a macro is a unit of its own, not part of a take
  m.playing = true;
  if (e != nullptr) {
    e->begin_group(core::action_id("macro.play"), 0, screen_undo_area(ctx.view.screen));  // one unit, in the area of the screen it was played from
  }
  bool changed = false;
  for (unsigned i = 0; i < m.count; ++i) {
    const MacroStep step = m.steps[i];
    changed = dispatch(action_registry(), step.action, step.arg, ctx) == DispatchResult::Changed || changed;
  }
  if (e != nullptr) {
    e->end_group();
  }
  m.playing = false;
  return changed;
}

}  // namespace o3os::app
