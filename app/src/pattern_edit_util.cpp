// SPDX-License-Identifier: GPL-3.0-or-later
// The one walk behind every pattern edit (OS-FLASHDIET T1): pe::edit_selection was a template, so each of the 17 edits linked its
// own copy of the walk; here it is one out-of-line function, and the edits pass a captureless lambda (an EditFn) plus a context
// pointer carrying what they captured.

#include "pattern_edit_util.h"

namespace o3os::app::pe {

Edited edit_selection(ActionContext& ctx, EditFn fn, void* fn_ctx, bool multi_cell, bool block_cols) noexcept {
  Edited out;
  ViewState& v = ctx.view;
  const PatternSource src(ctx.model, v.edit.pattern);
  const Selection& sel = v.edit.selection;
  const Selection::Block b = sel.block(v.pattern.row(), v.pattern.track());
  const model::edit::Rule rule{b.row0 == b.row1 && b.track0 == b.track1 && !multi_cell, !sel.active};
  model::edit::Last last{static_cast<std::uint8_t>(clampi(v.instrument, 1, static_cast<int>(model::kMaxInstruments))), v.edit.last_fx};
  bool exists = src.exists();
  const int tracks = src.tracks();
  for (int t = b.track0; t <= b.track1 && t < tracks; ++t) {
    const int r1 = b.row1 < src.track_length(t) - 1 ? b.row1 : src.track_length(t) - 1;
    if (b.row0 > r1) {
      ++out.refused;
      continue;
    }
    model::Step buf[model::kMaxPatternLength];
    const int n = r1 - b.row0 + 1;
    int first = -1;
    int final_changed = -1;
    for (int k = 0; k < n; ++k) {
      model::Step s = src.step(t, b.row0 + k);
      const model::Step before = s;
      fn(s, rule, last, fn_ctx);
      if (block_cols) {  // OS-SELCOL: only the columns of a column block change
        s = with_columns(before, s, b);
      }
      buf[k] = s;
      if (std::memcmp(&s, &before, sizeof s) != 0) {
        first = first < 0 ? k : first;
        final_changed = k;
      }
    }
    if (first < 0) {
      continue;
    }
    if (!create_if_missing(ctx, exists, static_cast<unsigned>(src.length()))) {
      out.failed = true;
      break;
    }
    if (ctx.edit->set_steps(v.edit.pattern, static_cast<std::uint32_t>(t), static_cast<std::uint32_t>(b.row0 + first), buf + first,
                            static_cast<std::uint32_t>(final_changed - first + 1)) == model::Status::Ok) {
      out.changed = true;
    }
  }
  v.instrument = last.instrument;
  v.edit.last_fx = last.fx;
  return out;
}

}  // namespace o3os::app::pe
