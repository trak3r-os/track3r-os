// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the actions that touch parameters share (private to app/src, OS-002d): finding the parameter under the cursor, reading and
// writing one wherever it lives (the project through the editor an undoable action is handed, the device in its settings block), and
// telling the audio side.

#include "action_handlers.h"

#include <o3os/core/learn.h>
#include <o3os/core/param.h>

namespace o3os::app {

// The parameter the base screen's cursor is on (core::ParamId::None when it has none).
core::ParamId focus_param(const ActionContext& ctx) noexcept;

// The value of a parameter; false when there is nothing to read: a project parameter without a project, an instrument slot that is free.
bool read_param(const Model& model, core::ParamId id, float& value) noexcept;
// Its generation (project or device block), 0 if it does not exist.
std::uint32_t param_generation_of(const Model& model, core::ParamId id) noexcept;

// OS-PARAMAUTO S2: bit L is set when CC letter L (A..F) owns a parameter of the project (model::auto_scan): the letters the sequencer publishes instead of sending a MIDI CC (Sequencer::set_auto_mask).
std::uint8_t param_auto_mask(const Model& model) noexcept;

enum class WriteResult : std::uint8_t { Changed, Unchanged, Rejected, NoProject, NoInstrument };

// Sets a parameter to `value` (it must be in range: BadValue is Rejected). A project parameter goes through ctx.edit, so it is part of the
// undo unit the dispatcher opened; without an editor it is NoProject. A change is sent to the audio side as a ParamChange (ramp 0: the
// engine's own smoothing).
// `tell` false (OS-MUTE: a deferred mute, which the sequencer tells the strip at its boundary): the project is written, nothing is sent.
WriteResult write_param(ActionContext& ctx, core::ParamId id, float value, bool tell = true) noexcept;

// OS-UNDOGRP: the click of an encoder on a parameter row. The edit that follows may join the undo unit of the last click on the same row (a sweep is one
// unit); a pause of kClickGroupMs since the last click ends the group first. Device settings have no undo and nothing to merge.
inline constexpr std::uint32_t kClickGroupMs = 1500;  // the tap timeout (D8)
void merge_click(ActionContext& ctx, core::ParamId id) noexcept;

// Pushes `{id, value, ramp}` to the ring if one is attached; a full ring counts a drop.
void send_param_change(const ActionContext& ctx, core::ParamId id, float value, std::uint32_t ramp = 0) noexcept;

// `detents` fine steps of a parameter that is not under the cursor (OS-MASTERVOL: the master volume from any screen), as the encoder moves the one that is: the same clamp, the click group
// and the message when nothing could be written. True when the parameter changed or a message was posted.
bool param_edit_id(ActionContext& ctx, core::ParamId id, int detents) noexcept;

// The Learn stores (param_actions.cpp), shared by the Learn dialog's actions and LEARN MAP's (learn_map_actions.cpp, OS-LEARNMAP).
core::LearnScope scope_of(core::ParamId id) noexcept;  // the store a parameter's bindings are in
unsigned scope_index(core::LearnScope scope) noexcept;  // 0 project, 1 device: the index of Session::learn and LearnMapView::hit
const core::LearnBinding* store_slots(const ActionContext& ctx, core::LearnScope scope) noexcept;  // nullptr: no project
model::Status store_set(ActionContext& ctx, core::LearnScope scope, unsigned slot, const core::LearnBinding& b) noexcept;  // one slot (a project's is an undo record)
void reset_runtime(ActionContext& ctx, core::LearnScope scope, unsigned slot) noexcept;  // the pickup state of a slot just written
// A follow binding acts only on an instrument that has the field with its meaning (never a module field).
bool follow_fits(model::InstrumentKind kind, unsigned field) noexcept;
// Writes binding `b` into the store of its parameter by core::learn_plan: the slot of the same parameter, else the first one it displaces, else a free one; the others that listen to the same message go.
// Nothing is written when the store is full.
enum class LearnPut : std::uint8_t { Done, Full, Refused };
LearnPut learn_put(ActionContext& ctx, const core::LearnBinding& b) noexcept;
// F5 of the Learn dialog on parameter `id` (a followed field counts as its field), shared with LEARN MAP's F5: the binding of `id` follows the selected instrument, or is pinned back to its instrument.
bool learn_follow_of(ActionContext& ctx, core::ParamId id) noexcept;

// An info notification saying why nothing could be written ("NO PROJECT OPEN"). Returns whether the screen changed.
bool say_not_written(ActionContext& ctx, WriteResult result) noexcept;

}  // namespace o3os::app
