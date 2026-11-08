// SPDX-License-Identifier: GPL-3.0-or-later
// The step popups (OS-FU3). A list shows while the Instr, Vol (FX1: Kind::Vol, FX column 1) or Fx (FX2: Kind::Fx, column 2) button is held, and a pad keyboard while Note is held
// (shown after 180 ms). The arrows, the
// encoder and the pads choose; the release closes the popup and, in Edit with one step selected, enters the choice there. On the
// instrument parameters page the Instr button opens the instrument list and the choice is the instrument edited.
// OS-INSTPICK (NEW OS BEHAVIOUR, maintainer request): the same list on Wave / Chop and Sample edit, the same edited instrument (ViewState::instrument); there only the sample instruments
// can be chosen (StepPopupView::samples_only): the module, MIDI and empty slots are dimmed, the arrows and the encoder skip them and a pad on one does nothing. The choice
// is live, as on the parameters page, and resets the screens' windows and selection (wave_view_reset); an effect or a slicer job still running keeps its own slot, so no edit is lost.
//
// OS-PADS-B: the held Note button no longer shows a pad keyboard: its 48 pads are the note-entry settings (note_entry.h: ROOT, SCALE, JUMP, OCTAVE + SCALE LOCK), nothing is entered on the
// release and a short tap on Note is what it always was (the cursor goes to the Note column). OS-SCALES28 S2: the encoder and Up / Down walk the 39 scales there, as a ring in the official
// order; the pads of row 2 keep their twelve.
//
// Here the hold is the gesture layer's (as the choosers of F1..F3), the popup is the StepPopup overlay (screens/step_popup_overlay.cpp), and the entry is an undo unit.
// NEW OS BEHAVIOUR: the Note popup writes only when a pad (or an arrow) chose a note; the Fx list stops at
// the 43 FX; the Instr list is 64 entries, 48 samples then the MIDI channels (OS-INSTRMIDI: a MIDI instrument reads "MIDI CH n"); F8 closes the popup without entering anything.
#include "pattern_edit_util.h"

#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/wave_screen.h>

namespace o3os::app {

namespace {

using Kind = StepPopupView;
using model::edit::Last;
using model::edit::Rule;

constexpr int kInstrumentMax = static_cast<int>(model::kMaxInstruments);
constexpr int kFxMax = static_cast<int>(model::fx::kPickerCount) - 1;
constexpr int kPadMax = static_cast<int>(model::edit::kPadCount) - 1;

// The direction codes: Up, Right, Down, Left. The instrument and FX lists run down, a page is 12 (left and right).
constexpr int kListStep[4] = {-1, 12, 1, -12};

// The thunk context of the Vol / Fx entry (OS-FLASHDIET T1): the FX column under the cursor and the picked type.
struct FxPick {
  int column;
  unsigned type;
};

bool on_pattern(const ViewState& v) noexcept { return v.screen == ScreenId::Pattern; }
bool on_sample_screen(const ViewState& v) noexcept { return v.screen == ScreenId::Wave || v.screen == ScreenId::SampleEdit; }

// The instrument `delta` places from the one in view that the list lets be chosen: a list with dimmed entries (samples_only) skips them in the direction it moves (at most 64 steps),
// and with none left that way the choice stays where it is.
int instrument_after(const ActionContext& ctx, int delta) noexcept {
  const ViewState& v = ctx.view;
  if (delta == 0) {
    return v.instrument;
  }
  const int dir = delta < 0 ? -1 : 1;
  int to = pe::clampi(v.instrument + delta, 1, kInstrumentMax);
  while (to >= 1 && to <= kInstrumentMax && !step_popup_pickable(ctx.model, v, static_cast<unsigned>(to - 1))) {
    to += dir;
  }
  return to >= 1 && to <= kInstrumentMax ? to : v.instrument;
}

// The list chose instrument `to` (1-based). On Wave and Sample edit the new sample starts with its own window and selection.
bool choose_instrument(ActionContext& ctx, int to) noexcept {
  ViewState& v = ctx.view;
  const bool changed = to != v.instrument;
  if (changed) {
    release_live_pads(ctx);  // OS-INSTUI O2
  }
  v.instrument = to;
  if (changed && on_sample_screen(v)) {
    wave_view_reset(v);
  }
  return changed;
}

// Moves the choice by `delta` places.
bool bump(ActionContext& ctx, int delta) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::StepPopup) {
    return false;
  }
  switch (v.popup.kind) {
    case Kind::Instr: {
      return choose_instrument(ctx, instrument_after(ctx, delta));
    }
    case Kind::Vol:
    case Kind::Fx: {
      const int to = pe::clampi(v.popup.item + delta, 0, kFxMax);
      const bool changed = to != v.popup.item;
      v.popup.item = static_cast<std::int16_t>(to);
      return changed;
    }
    case Kind::Note: {  // OS-SCALES28 S2: the encoder and Up / Down walk the 39 scales as a ring in the official order (Up -1, Down +1, one a detent, wrapping)
      const int count = static_cast<int>(model::kNoteScaleMax) + 1;  // 39
      const int cur = v.edit.entry.scale <= model::kNoteScaleMax ? static_cast<int>(v.edit.entry.scale) : 0;
      int to = (cur + delta) % count;
      if (to < 0) {
        to += count;
      }
      if (to == cur) {
        return false;
      }
      v.edit.entry.scale = static_cast<std::uint8_t>(to);
      v.edit.apply_entry();  // the NOTES board follows the scale in force (with the lock on)
      return true;
    }
    default:
      return false;
  }
}

// The instrument a page (`delta` places, signed, a page being the list's visible entries) from the one in view: the target clamped to the list, and when it is dimmed (samples_only) the nearest
// one the list lets be chosen on the way back to the one in view, so a page near the end of a list that ends in dimmed entries still goes as far as it can. Stays put with none.
int instrument_page_after(const ActionContext& ctx, int delta) noexcept {
  const ViewState& v = ctx.view;
  const int dir = delta < 0 ? -1 : 1;
  const int to = pe::clampi(v.instrument + delta, 1, kInstrumentMax);
  for (int n = to; n != v.instrument && n >= 1 && n <= kInstrumentMax; n -= dir) {
    if (step_popup_pickable(ctx.model, v, static_cast<unsigned>(n - 1))) {
      return n;
    }
  }
  return v.instrument;
}

// The release enters the choice in the cursor step: Edit only, one step selected.
void enter(ActionContext& ctx) noexcept {
  ViewState& v = ctx.view;
  if (ctx.edit == nullptr || !on_pattern(v) || !pattern_mode_edits(v.mode[screen_index(ScreenId::Pattern)])) {
    return;
  }
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());  // (OS-AUDIT-56: only the rows and tracks count; the popup writes its own field of the one step)
  if (b.row0 != b.row1 || b.track0 != b.track1) {
    return;
  }
  pe::Edited e;
  switch (v.popup.kind) {
    case Kind::Instr: {
      unsigned instrument = static_cast<unsigned>(pe::clampi(v.instrument, 1, kInstrumentMax)); // the thunk context (non-const)
      e = pe::edit_selection(
          ctx, [](model::Step& s, const Rule& r, Last& l, void* p) { model::edit::instrument_set(s, *static_cast<const unsigned*>(p), r, l); }, &instrument);
      break;
    }
    case Kind::Vol:
    case Kind::Fx: {
      const int fx = pe::fx_column(v);
      if (fx < 0) {
        return;
      }
      const unsigned type = static_cast<unsigned>(model::fx::type_of_picker(v.popup.item));
      FxPick pick{fx, type}; // the thunk context (non-const)
      e = pe::edit_selection(
          ctx,
          [](model::Step& s, const Rule&, Last&, void* p) {
            const FxPick& c = *static_cast<const FxPick*>(p);
            model::edit::fx_type_set(s, static_cast<unsigned>(c.column), c.type);
          },
          &pick);
      break;
    }
    default:  // Note: the settings were changed live, there is nothing to enter
      return;
  }
  (void)pe::finish_entry(ctx, e, false);  // the cursor stays
}

}  // namespace

bool popup_open(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::None || arg < Kind::Instr || arg > Kind::Vol) {
    return false;
  }
  const bool pattern = on_pattern(v);
  if (!pattern && !((v.screen == ScreenId::Instrument || on_sample_screen(v)) && arg == Kind::Instr)) {
    return false;
  }
  v.popup.kind = static_cast<std::uint8_t>(arg);
  v.popup.item = -1;
  v.popup.samples_only = on_sample_screen(v);
  if (pattern) {
    // A button press moves the edit to that column before the popup shows.
    const Column column = arg == Kind::Instr ? Column::Instr : (arg == Kind::Note ? Column::Note : (arg == Kind::Vol ? Column::Fx1 : Column::Fx2));
    (void)v.pattern.select_column(column);
    if (arg == Kind::Fx || arg == Kind::Vol) {  // opens on the step's own FX
      const PatternSource src(ctx.model, v.edit.pattern);
      v.popup.item = static_cast<std::int16_t>(model::fx::picker_index(src.step(v.pattern.track(), v.pattern.row()).fx[pe::fx_column(v)].type));
    }
  } else {
    v.popup.kind = Kind::Instr;
  }
  v.overlay = OverlayId::StepPopup;
  return true;
}

// The release of the button whose hold opened the popup.
bool popup_release(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::StepPopup || v.popup.kind != arg) {
    return false;
  }
  v.overlay = OverlayId::None;
  enter(ctx);
  (void)v.pattern.snap_column();  // OS-ZOOM: a hold popup of a field the zoom level hides chose its column for the entry; the cursor comes back to one in sight
  return true;
}

bool popup_move(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < 1 || arg > 4) {
    return false;
  }
  return bump(ctx, kListStep[arg - 1]);
}

// OS-NAV3 (new OS behaviour; Left / Right are 12 places): Shift + Up / Down, arg = pages (signed), a page being the entries of the list in view (step_popup_list_rows). Clamps, never wraps.
bool popup_page(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::StepPopup || arg == 0) {
    return false;
  }
  const int rows = step_popup_list_rows(ui::theme(v.theme), v.popup.kind);
  if (rows < 1) {
    return false;  // the Note panel has no list
  }
  const int delta = pe::clampi(arg, -kInstrumentMax, kInstrumentMax) * rows;
  return v.popup.kind == Kind::Instr ? choose_instrument(ctx, instrument_page_after(ctx, delta)) : bump(ctx, delta);
}

bool popup_delta(ActionContext& ctx, core::ActionArg arg) noexcept { return bump(ctx, pe::clampi(arg, -1000, 1000)); }

// A pad: the instrument list takes pad + 1 as the Pattern's Browse pads and the parameter page's pads do, and sounds it; the FX list the pad's index; the Note panel the setting
// of its pad (PatternEdit::entry_pad), which makes no sound.
bool popup_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::StepPopup || arg < 0 || arg > kPadMax) {
    return false;
  }
  bool changed = false;
  unsigned note = v.edit.pads.note(static_cast<unsigned>(arg));
  switch (v.popup.kind) {
    case Kind::Instr: {
      if (!step_popup_pickable(ctx.model, v, static_cast<unsigned>(arg))) {
        return false;  // dimmed (samples_only): the pad does nothing
      }
      changed = choose_instrument(ctx, arg + 1);
      const PatternSource src(ctx.model, v.edit.pattern);
      const unsigned at = src.step(v.pattern.track(), v.pattern.row()).note;
      note = at >= 1u && at <= 128u ? at - 1u : model::edit::kDefaultNote;
      break;
    }
    case Kind::Vol:
    case Kind::Fx: {
      const int to = pe::clampi(arg, 0, kFxMax);
      changed = to != v.popup.item;
      v.popup.item = static_cast<std::int16_t>(to);
      return changed;  // no sound
    }
    case Kind::Note:
      return v.edit.entry_pad(static_cast<unsigned>(arg));
    default:
      return false;
  }
  if (ctx.preview != nullptr && on_pattern(v)) {
    ctx.preview->pad_down(static_cast<unsigned>(arg), static_cast<unsigned>(v.pattern.track()), static_cast<unsigned>(v.instrument), note);
  }
  return changed;
}

}  // namespace o3os::app
