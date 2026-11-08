// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-MIDIREC S1b (docs/specs/OS-MIDIREC.md): the EVENTS overlay's view state, in the ViewState (view_state.h). The list is
// an editor over the steps, projected on every redraw (A15: no row table, no heap), so the state keeps positions, not keys:
// `cursor` and `top` are indices into the projected list under the filter, and the render walks the window from `top`, so no
// key<->index scan runs on a full scope while the list is up (the S1a review's rule). The scope is fixed when the list opens
// (A2); the anchor is the pattern generation the window was last read at, with the first visible row's step (A15), so a
// re-projection after an edit or an undo is recognised without a second pass.
//
// Main loop only. No allocation.

#include <o3os/app/pattern_edit.h>
#include <o3os/app/pattern_view.h>
#include <o3os/model/event_list.h>

#include <cstdint>

namespace o3os::app {

struct EventsView {
  // The fields Left / Right walk through on the cursor's row (spec 3.4). A field the row's kind does not show is skipped.
  enum class Field : std::uint8_t { Time = 0, Type, Ch, Data, Val, Len, In, Fx1, Fx2, kCount };

  model::events::Scope scope{};                        // fixed when the list opens (A2)
  std::uint32_t cursor = 0;                            // the row under the cursor, as a position in the projected list
  std::uint32_t top = 0;                               // the first visible row, the same kind of position
  std::uint8_t field = static_cast<std::uint8_t>(Field::Time);
  model::events::Filter filter = model::events::Filter::All;  // S1c cycles it with F1
  std::uint32_t anchor_gen = 0;                        // A15: the pattern's generation the anchor was read at
  std::uint8_t anchor_step = 0;                        // ... and the first visible row's step then
  bool anchor_valid = false;
};

// The fields a row of `kind` has (the columns spec 3.3 gives it): a NOTE row has no TYPE field (its type is fixed), a CC row
// has no LEN, an end row has only TIME, TYPE, CH and IN; FX1 and FX2 (OS-EVFX) are the NOTE row's alone.
constexpr bool events_field_ok(model::events::Kind kind, EventsView::Field f) noexcept {
  const auto fi = static_cast<unsigned>(f);
  if (kind == model::events::Kind::Note) {
    return f != EventsView::Field::Type;
  }
  if (kind == model::events::Kind::Cc) {
    return f != EventsView::Field::Len && fi <= static_cast<unsigned>(EventsView::Field::In);  // FX1 and FX2 are NOTE-only
  }
  return fi <= static_cast<unsigned>(EventsView::Field::In);  // an end row: Time, Type, Ch, In (Data, Val, Len are '-')
}

// The scope the list opens with (A2): the block's tracks and rows as whole steps when a range is active (a column block is
// widened — the scope has no columns), else the cursor's track, every step of its length. Shared by events_open and the
// simulator's budget sweep, so the two cannot disagree.
inline model::events::Scope events_scope_of(const model::Pattern& pat, const PatternEdit& edit, int cursor_row, int cursor_track) noexcept {
  const Selection& sel = edit.selection;
  const Selection::Block b = sel.block(cursor_row, cursor_track);
  return sel.active
             ? model::events::scope_block(static_cast<std::int16_t>(b.row0), static_cast<std::int16_t>(b.row1), static_cast<std::int8_t>(b.track0), static_cast<std::int8_t>(b.track1))
             : model::events::scope_track(pat, static_cast<std::uint32_t>(cursor_track));
}

// The cursor's start position (3.1): the first row at or after the grid cursor's step, else the last row. One projection.
inline std::uint32_t events_start_cursor(const model::Pattern& pat, model::events::Env env, model::events::Scope scope,
                                         model::events::Filter filter, int grid_row) noexcept {
  model::events::Projector pr(pat, env, scope, filter);
  model::events::Row row;
  std::uint32_t index = 0;
  while (pr.next(row)) {
    if (row.key.step >= static_cast<std::uint8_t>(grid_row)) {
      return index;
    }
    ++index;
  }
  return index > 0u ? index - 1u : 0u;  // none at or after: the last row
}

// The row under the cursor, read from the window the view draws (one projection from `top`, the same the render walks): false
// when the list is empty or the pattern is gone. Shared by the actions (events_actions.cpp), the info line of the base screen
// and the tests, so the three cannot disagree about what the cursor is on.
bool events_cursor_row(const model::Project& project, unsigned pattern, const EventsView& ev, model::events::Row& out) noexcept;

}  // namespace o3os::app
