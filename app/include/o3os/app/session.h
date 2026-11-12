// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the actions of OS-002c need besides the model and the view (ActionContext::session): the document's editor, which holds
// the undo journal; the macro recorder; the job runner. The App owns one. All of it is optional: without an editor the history
// actions say "nothing to undo" and an undoable action is Disabled (there is no project to edit).
//
// The editor is private. Every edit of the project is an undo unit named by an action (ADR 0009 rev 1), and a handler that could
// reach the editor could make units named "none": so the only ways in are dispatch(), which hands it to an undoable action as
// ActionContext::edit and to nobody else, and the history actions (edit.undo, edit.redo, macro.play), which take it through
// `SessionAccess`, declared in the private header app/src/session_access.h that tools/check_layers.py lets nothing else include.

#include <o3os/core/action.h>
#include <o3os/core/job.h>
#include <o3os/core/learn.h>
#include <o3os/core/tick.h>
#include <o3os/model/editor.h>

#include <cstdint>

namespace o3os::app {

// A macro is the sequence of (ActionId, ActionArg) that were dispatched while it recorded (ADR 0009 rev 1, amendment 1: macros
// keep their own buffer, because navigation and other actions that never reach the undo journal must replay too; the edits of
// a replayed action are journaled like any others, as dispatch() runs them). The macro and the history control themselves
// (macro.record, macro.play, edit.undo, edit.redo) are not recorded, and recording is off while it plays.
inline constexpr unsigned kMacroSteps = 64;

struct MacroStep {
  core::ActionId action = core::ActionId::None;
  core::ActionArg arg = 0;
};

struct Macro {
  MacroStep steps[kMacroSteps];
  unsigned count = 0;
  bool recording = false;
  bool playing = false;
  bool full = false;  // recording stopped by itself: the macro filled up
};

struct SessionAccess;

class Session {
 public:
  Macro macro;
  core::JobRunner jobs;
  // MIDI Learn between messages (OS-002d): whether each binding has caught its parameter, by store (0 project, 1 device) and slot, for the
  // parameter and generation it last saw (core::LearnRuntime). Never persisted; a binding that is rewritten starts again.
  core::LearnRuntime learn[2][core::kLearnSlots] = {};
  // ParamChange messages that did not fit the ring to the audio side (the engine drains 64 a block of 64 frames), and undo or redo steps
  // that changed more than the applied list carries (the audio side then has to re-read the project): counts, for the diagnostics.
  std::uint32_t param_drops = 0;
  std::uint32_t param_resyncs = 0;
  // OS-UNDOGRP: when the last click on a parameter row was made (the click group ends 1500 ms after it, param_actions.cpp merge_click).
  core::Tick merge_at = 0;
  // OS-BUG-PARAMAUTO-PLAY: the parameters the played CC letters A..F (bit L of auto_live) have moved on the audio side only; auto_id[L] is the one letter L moved. A stopped transport sends their base values from the
  // project again (param_auto). Main loop only; never persisted.
  core::ParamId auto_id[6] = {};
  std::uint8_t auto_live = 0;

  // The document: the Editor over the project and its journal, owned by the caller. nullptr detaches it. The Learn bindings are another
  // set of bindings now: no knob has caught anything.
  void attach(model::Editor* editor) noexcept {
    editor_ = editor;
    reset_learn();
  }
  // Every binding has to be caught again (a document attached, an undo or redo: the slots may hold other bindings than they did).
  void reset_learn() noexcept {
    for (auto& store : learn) {
      for (core::LearnRuntime& rt : store) {
        rt = core::LearnRuntime{};
      }
    }
  }
  // Read-only: the project, the journal and can_undo() of the document, none of which can edit.
  const model::Editor* document() const noexcept { return editor_; }
  // The document's editor for ONE use: the project-level state that the runtime owns (OS-004d: Editor::set_mix_state, set_perf_state, set_midi_state, set_usb_state, set_app_params). Those
  // are not journalled and keep the undo history; an undoable edit still goes through dispatch() only. (And the brackets of the live take's undo group, App::sync_take: begin_group / end_group
  // only, OS-LIVEREC; what is recorded in it is written by dispatch().)
  model::Editor* state_editor() noexcept { return editor_; }
  // Forgets the undo history and takes the project as it is: after something changed it behind the editor's back (an import, a
  // load). Makes no unit.
  void reset_history() noexcept {
    if (editor_ != nullptr) {
      editor_->history_reset();
    }
    reset_learn();  // what changed behind the editor's back may have replaced the project's bindings and the parameters under them
  }

 private:
  friend struct SessionAccess;
  model::Editor* editor_ = nullptr;
};

}  // namespace o3os::app
