// SPDX-License-Identifier: GPL-3.0-or-later
// LEARN MAP (OS-LEARNMAP, docs/specs/OS-LEARNMAP.md): the full-screen list of every MIDI Learn binding and the encoder's target. The row under the cursor is edited one column at a time (SOURCE, CH, PARAMETER,
// INS, MODE); an edit rewrites the binding with the same fields (no format change) through the Learn dialog's own store helpers (param_access.h), so a project binding is one undo record and a device binding is not.
// Nothing here runs in an ISR: the actions and the CC capture run on the main loop.
#include "param_access.h"

#include <o3os/app/learn_list.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/model/instrument.h>
#include <o3os/model/params.h>

namespace o3os::app {

namespace {

namespace ip = model::instrument_param;
using core::LearnBinding;

constexpr unsigned kFields = ip::kModuleFirst + ip::kModuleMax;  // the walk covers field 0 .. 0x11F
constexpr int kMaxTurn = 64;                                       // detents of one turn, clamped before they are multiplied
constexpr unsigned kInsMax = 64;

struct Row {
  LearnBinding b;
  core::LearnScope scope;
  unsigned slot;  // kLearnSlots on the encoder row
  bool enc;
};

bool refuse(ActionContext& ctx, const char* text) noexcept { return say_tag(ctx, core::Severity::Warn, text, "learn"); }

unsigned selected(const ViewState& v) noexcept { return v.instrument > 0 ? v.instrument - 1u : 0u; }

// The parameter a row acts on now: a follow binding means the selected instrument's.
core::ParamId resolved(const ViewState& v, const LearnBinding& b) noexcept {
  return (b.flags & core::kLearnFollow) != 0 ? core::param_id(core::ParamKind::Instrument, selected(v), core::param_field(b.param)) : b.param;
}

// Whether instrument `index` has field `g` with its meaning (a module field only on a module instrument; a MIDI row on a MIDI one; the rest on a sample one).
bool has_field(const Model& model, unsigned index, unsigned g) noexcept {
  const model::Instrument* in = model.project != nullptr ? model.project->instrument(index) : nullptr;
  if (in == nullptr) {
    return false;
  }
  return ip::is_module_field(g) ? in->kind == model::InstrumentKind::Module : follow_fits(in->kind, g);
}

bool row_at(const ActionContext& ctx, Row& r) noexcept {
  const unsigned c = ctx.view.learn_map.cursor;
  if (c == 0) {
    r.b = LearnBinding{ctx.view.enc_target, 0, 0, static_cast<std::uint8_t>(core::LearnMode::Relative), 0};
    r.scope = scope_of(ctx.view.enc_target);
    r.slot = core::kLearnSlots;
    r.enc = true;
    return true;
  }
  LearnEntry e{};
  if (!learn_entry_at(ctx.model, c - 1u, e)) {
    return false;
  }
  r = Row{*e.binding, e.scope, e.slot, false};
  return true;
}

void clamp_cursor(ActionContext& ctx) noexcept {
  const unsigned last = learn_entry_count(ctx.model);  // rows are the encoder's plus the bindings: the last index is the count
  if (ctx.view.learn_map.cursor > last) {
    ctx.view.learn_map.cursor = static_cast<std::uint8_t>(last);
  }
}

// Another used slot that acts on the same parameter as `nb`.
bool param_taken(const LearnBinding* slots, unsigned self, const LearnBinding& nb) noexcept {
  for (unsigned s = 0; s < core::kLearnSlots; ++s) {
    if (s != self && !core::learn_empty(slots[s]) && (core::learn_targets(slots[s], nb.param) || core::learn_targets(nb, slots[s].param))) {
      return true;
    }
  }
  return false;
}

// Says why `nb` may not take slot `self`: its controller on that channel is another binding's ("CC 74 CH* IS TAKEN"), or its parameter is already bound. True when refused.
bool clash(ActionContext& ctx, const LearnBinding* slots, unsigned self, const LearnBinding& nb) noexcept {
  core::FixedString<core::kNotifyTextChars> text;
  for (unsigned s = 0; s < core::kLearnSlots; ++s) {
    if (s != self && !core::learn_empty(slots[s]) && core::learn_overlap(slots[s], nb)) {
      text.assign("CC ");
      append_uint(text, slots[s].cc);
      text.append(" CH");
      if (slots[s].channel == 0) {
        text.append("*");
      } else {
        append_uint(text, slots[s].channel);
      }
      text.append(" IS TAKEN");
      (void)refuse(ctx, text.c_str());
      return true;
    }
  }
  if (param_taken(slots, self, nb)) {
    (void)refuse(ctx, "ALREADY BOUND");
    return true;
  }
  return false;
}

// Writes `nb` over the row: the encoder row takes the target, a binding goes into its slot (after the refusals).
bool commit(ActionContext& ctx, const Row& r, const LearnBinding& nb) noexcept {
  if (r.enc) {
    ctx.view.enc_target = nb.param;
    return true;
  }
  const LearnBinding* slots = store_slots(ctx, r.scope);
  if (slots == nullptr) {
    return say_not_written(ctx, WriteResult::NoProject);
  }
  if (clash(ctx, slots, r.slot, nb)) {
    return true;
  }
  if (store_set(ctx, r.scope, r.slot, nb) != model::Status::Ok) {
    return say_not_written(ctx, WriteResult::NoProject);
  }
  reset_runtime(ctx, r.scope, r.slot);
  return true;
}

// The next parameter `dir` (+1 / -1) away from `at` in the field order of its kind and index: Learn-able ones only, only those the instrument has (a follow row: those of its kind family),
// not bound by another slot of `slots` (null on the encoder row). A binding with no parameter starts from the selected instrument's first (last) field. None when there is no further one.
core::ParamId walk(ActionContext& ctx, const LearnBinding& at, int dir, const LearnBinding* slots, unsigned self) noexcept {
  core::ParamKind kind = core::param_kind(at.param);
  unsigned index = core::param_index(at.param);
  int f = static_cast<int>(core::param_field(at.param));
  const bool follow = (at.flags & core::kLearnFollow) != 0;
  const auto family = follow_fits(model::InstrumentKind::Midi, static_cast<unsigned>(f)) ? model::InstrumentKind::Midi : model::InstrumentKind::Sample;
  if (kind == core::ParamKind::None) {
    kind = core::ParamKind::Instrument;
    index = selected(ctx.view);
  }
  const int limit = static_cast<int>(kind == core::ParamKind::Insert ? core::kInsertIdFields : kFields);
  if (at.param == core::ParamId::None) {
    f = dir > 0 ? -1 : limit;
  }
  for (int g = f + dir; g >= 0 && g < limit; g += dir) {
    const unsigned ug = static_cast<unsigned>(g);
    LearnBinding probe = at;
    probe.param = core::param_id(kind, follow ? 0u : index, ug);
    ParamScratch scratch;
    const core::ParamDesc* d = describe_param(ctx.model, core::param_id(kind, index, ug), scratch);
    if (d == nullptr || (d->flags & core::kParamLearnable) == 0u) {
      continue;
    }
    if (kind == core::ParamKind::Instrument && !(follow ? follow_fits(family, ug) : has_field(ctx.model, index, ug))) {
      continue;
    }
    if (slots != nullptr && param_taken(slots, self, probe)) {
      continue;
    }
    return probe.param;
  }
  return core::ParamId::None;
}

bool edit_param(ActionContext& ctx, const Row& r, int detents) noexcept {
  const LearnBinding* slots = r.enc ? nullptr : store_slots(ctx, r.scope);
  if (!r.enc && slots == nullptr) {
    return say_not_written(ctx, WriteResult::NoProject);
  }
  LearnBinding nb = r.b;
  const int dir = detents > 0 ? 1 : -1;
  for (int n = detents < 0 ? -detents : detents; n > 0; --n) {
    const core::ParamId next = walk(ctx, nb, dir, slots, r.slot);
    if (next == core::ParamId::None) {
      break;
    }
    nb.param = next;
  }
  return nb.param != r.b.param && commit(ctx, r, nb);
}

// INS: 1 .. 64 pins the binding to an instrument, * (position 0) makes it follow the selected one. A plain step lands on the next number and says so when that instrument has no such parameter;
// a Shift step (coarse) skips to the next instrument that has it (documented in the spec, D6).
bool edit_ins(ActionContext& ctx, const Row& r, int detents, bool coarse) noexcept {
  const core::ParamId id = r.b.param;
  if (id == core::ParamId::None) {
    return refuse(ctx, "NO ENCODER TARGET");
  }
  if (core::param_kind(id) != core::ParamKind::Instrument) {
    return refuse(ctx, "INS: INSTRUMENT ROWS ONLY");
  }
  const unsigned f = core::param_field(id);
  if (ip::is_module_field(f)) {
    return refuse(ctx, "NOT FOR A MODULE PARAMETER");
  }
  const int lo = r.enc ? 1 : 0;
  const int cur = (r.b.flags & core::kLearnFollow) != 0 ? 0 : static_cast<int>(core::param_index(id)) + 1;
  int p = cur;
  if (coarse) {
    const int dir = detents > 0 ? 1 : -1;
    for (int n = detents < 0 ? -detents : detents; n > 0; --n) {
      int q = p;
      do {
        q += dir;
      } while (q >= lo && q <= static_cast<int>(kInsMax) && q != 0 && !has_field(ctx.model, static_cast<unsigned>(q) - 1u, f));
      if (q < lo || q > static_cast<int>(kInsMax)) {
        break;
      }
      p = q;
    }
  } else {
    p = cur + detents;
    p = p < lo ? lo : (p > static_cast<int>(kInsMax) ? static_cast<int>(kInsMax) : p);
  }
  if (p == cur) {
    return false;
  }
  if (p != 0 && !has_field(ctx.model, static_cast<unsigned>(p) - 1u, f)) {
    ParamScratch scratch;
    const core::ParamDesc* d = describe_param(ctx.model, resolved(ctx.view, r.b), scratch);
    core::FixedString<core::kNotifyTextChars> text;
    text.assign("INS ");
    append_dec(text, static_cast<unsigned>(p), 2);
    text.append(" HAS NO ");
    text.append(d != nullptr ? d->name : "THIS");
    return refuse(ctx, text.c_str());
  }
  LearnBinding nb = r.b;
  nb.param = core::param_id(core::ParamKind::Instrument, p == 0 ? 0u : static_cast<unsigned>(p) - 1u, f);
  nb.flags = p == 0 ? static_cast<std::uint8_t>(r.b.flags | core::kLearnFollow) : static_cast<std::uint8_t>(r.b.flags & ~core::kLearnFollow);
  return commit(ctx, r, nb);
}

bool edit(ActionContext& ctx, int detents, bool coarse) noexcept {
  LearnMapView& v = ctx.view.learn_map;
  v.wait = 0;
  ctx.view.learn.confirm = false;
  clamp_cursor(ctx);  // the list may have shrunk since the cursor was placed
  Row r;
  if (detents == 0 || !row_at(ctx, r)) {
    return false;
  }
  detents = detents < -kMaxTurn ? -kMaxTurn : (detents > kMaxTurn ? kMaxTurn : detents);
  const int step = coarse ? 8 : 1;
  LearnBinding nb = r.b;
  switch (v.column) {
    case 0:
    case 1: {
      if (r.enc) {
        return refuse(ctx, "ENCODER HAS NO CC");
      }
      const int cap = v.column == 0 ? 127 : 16;
      int n = static_cast<int>(v.column == 0 ? r.b.cc : r.b.channel) + detents * step;
      n = n < 0 ? 0 : (n > cap ? cap : n);
      (v.column == 0 ? nb.cc : nb.channel) = static_cast<std::uint8_t>(n);
      break;
    }
    case 2:
      return edit_param(ctx, r, detents * step);
    case 3:
      return edit_ins(ctx, r, detents, coarse);
    default: {
      if (r.enc) {
        return refuse(ctx, "ENCODER IS ALWAYS RELATIVE");
      }
      const int n = static_cast<int>(r.b.mode) + detents;
      nb.mode = static_cast<std::uint8_t>(n < 0 ? 0 : (n >= static_cast<int>(core::kLearnModeCount) ? static_cast<int>(core::kLearnModeCount) - 1 : n));
      break;
    }
  }
  return (nb.cc != r.b.cc || nb.channel != r.b.channel || nb.mode != r.b.mode) && commit(ctx, r, nb);
}

}  // namespace

// The value column: the current value of what the row acts on ("--" when there is none: no project, a free slot, an instrument without the field).
void learn_value_text(const Model& model, const ViewState& view, const core::LearnBinding& b, char* out) noexcept {
  const core::ParamId id = resolved(view, b);
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(model, id, scratch);
  float value = 0.0f;
  const bool fits = (b.flags & core::kLearnFollow) == 0 || has_field(model, selected(view), core::param_field(id));
  if (d == nullptr || !fits || !read_param(model, id, value)) {
    out[0] = '-';
    out[1] = '-';
    out[2] = '\0';
    return;
  }
  (void)core::param_format(*d, value, out, core::kParamTextChars + 1u);
}

bool lmap_open(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.learn_map = LearnMapView{};
  ctx.view.learn.confirm = false;
  ctx.view.overlay = OverlayId::LearnMap;
  return true;
}

bool lmap_edit(ActionContext& ctx, core::ActionArg arg) noexcept { return edit(ctx, arg, false); }
bool lmap_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return edit(ctx, arg, true); }

// F1 .. F5 and F7 of the map (arg = the key's number).
bool lmap_key(ActionContext& ctx, core::ActionArg arg) noexcept {
  LearnMapView& v = ctx.view.learn_map;
  v.wait = 0;
  if (arg != 4) {
    ctx.view.learn.confirm = false;
  }
  clamp_cursor(ctx);
  Row r;
  if (arg == 1) {
    if (ctx.model.project == nullptr) {
      return say_not_written(ctx, WriteResult::NoProject);
    }
    v.wait = LearnMapView::kWaitNew;
    return true;
  }
  if (arg == 4) {
    const bool changed = learn_clear_all(ctx, 0);
    clamp_cursor(ctx);
    return changed;
  }
  if (!row_at(ctx, r)) {
    return false;
  }
  switch (arg) {
    case 7:
      if (r.enc) {
        return refuse(ctx, "ENCODER HAS NO CC");
      }
      v.wait = LearnMapView::kWaitRelearn;
      return true;
    case 2: {
      if (r.enc) {
        return refuse(ctx, "ENCODER IS ALWAYS RELATIVE");
      }
      LearnBinding nb = r.b;
      nb.mode = static_cast<std::uint8_t>((r.b.mode + 1u) % core::kLearnModeCount);
      return commit(ctx, r, nb);
    }
    case 3: {
      if (r.enc) {
        if (r.b.param == core::ParamId::None) {
          return refuse(ctx, "NO ENCODER TARGET");
        }
        ctx.view.enc_target = core::ParamId::None;
        return say_tag(ctx, core::Severity::Info, "ENCODER TARGET CLEARED", "learn");
      }
      const std::uint8_t dialog_cursor = ctx.view.learn.cursor;  // learn_delete works on the dialog's cursor: lend it ours, give it back
      ctx.view.learn.cursor = static_cast<std::uint8_t>(v.cursor - 1u);
      const bool done = learn_delete(ctx, 0);
      ctx.view.learn.cursor = dialog_cursor;
      clamp_cursor(ctx);
      return done;
    }
    case 5: {
      if (r.enc) {
        return refuse(ctx, "FOLLOW: NOT FOR THE ENCODER");
      }
      const core::ParamId id = resolved(ctx.view, r.b);
      const unsigned f = core::param_field(id);
      // Either way the binding then acts on the selected instrument: refuse when that one has no such parameter (it would be dead).
      if (core::param_kind(id) == core::ParamKind::Instrument && !ip::is_module_field(f) && !has_field(ctx.model, selected(ctx.view), f)) {
        ParamScratch scratch;
        const core::ParamDesc* d = describe_param(ctx.model, id, scratch);
        core::FixedString<core::kNotifyTextChars> text;
        text.assign("INS ");
        append_dec(text, selected(ctx.view) + 1u, 2);
        text.append(" HAS NO ");
        text.append(d != nullptr ? d->name : "THIS");
        return refuse(ctx, text.c_str());
      }
      return learn_follow_of(ctx, id);
    }
    default:
      return false;
  }
}

// The next CC while F1 NEW or F7 RELEARN waits (midi_cc hands it over; channel 0 is "unknown" and binds every channel).
bool learn_map_capture(ActionContext& ctx, unsigned channel, unsigned cc) noexcept {
  LearnMapView& v = ctx.view.learn_map;
  const std::uint8_t wait = v.wait;
  v.wait = 0;
  if (wait == LearnMapView::kWaitRelearn) {
    Row r;
    if (!row_at(ctx, r)) {
      return true;
    }
    LearnBinding nb = r.b;
    nb.cc = static_cast<std::uint8_t>(cc);
    nb.channel = static_cast<std::uint8_t>(channel);
    return commit(ctx, r, nb);
  }
  const LearnBinding* slots = store_slots(ctx, core::LearnScope::Project);
  if (slots == nullptr) {
    return say_not_written(ctx, WriteResult::NoProject);
  }
  // NEW: the selected instrument's first parameter that nothing is bound to; the columns are set afterwards.
  LearnBinding nb{core::ParamId::None, static_cast<std::uint8_t>(cc), static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(core::LearnMode::Pickup), 0};
  nb.param = walk(ctx, nb, 1, slots, core::kLearnSlots);
  if (nb.param == core::ParamId::None) {
    return refuse(ctx, "NOTHING LEFT TO BIND");
  }
  if (clash(ctx, slots, core::kLearnSlots, nb)) {
    return true;
  }
  const LearnPut put = learn_put(ctx, nb);
  if (put != LearnPut::Done) {
    return refuse(ctx, put == LearnPut::Full ? "LEARN STORE FULL" : "NO PROJECT OPEN");
  }
  for (unsigned i = 0, n = learn_entry_count(ctx.model); i < n; ++i) {  // the cursor goes to the new row, on its PARAMETER
    LearnEntry e{};
    if (learn_entry_at(ctx.model, i, e) && e.scope == core::LearnScope::Project && e.binding->param == nb.param) {
      v.cursor = static_cast<std::uint8_t>(i + 1u);
      v.column = 2;
      break;
    }
  }
  return true;
}

// F8 of Config: LEARN MAP on the MIDI page (page 0), UPDATE on the INFO page (the firmware dialog, which does nothing elsewhere).
bool config_f8(ActionContext& ctx, core::ActionArg arg) noexcept {
  constexpr unsigned kPageMidi = 0;
  if (ctx.view.screen == ScreenId::Config && ctx.view.page[screen_index(ScreenId::Config)] == kPageMidi && ctx.view.overlay == OverlayId::None) {
    return lmap_open(ctx, arg);
  }
  return firmware_open(ctx, arg);
}

}  // namespace o3os::app
