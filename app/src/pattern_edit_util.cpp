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

// ---- MIDI lanes (OS-MIDIREC-P1 S7c, A16) --------------------------------------------------------------------------------------------------------------------------------------------------

namespace {
constexpr std::uint32_t kTps = model::kLaneTicksPerStep;
}  // namespace

LaneCopy lanes_to_clip(PatternClip& clip, const model::LaneStore* store, unsigned pattern, int track0, int row0) noexcept {
  if (store == nullptr || !clip.whole_steps()) {
    return LaneCopy::None;
  }
  const std::uint32_t t0 = static_cast<std::uint32_t>(row0) * kTps;
  const std::uint32_t t1 = static_cast<std::uint32_t>(row0 + clip.rows()) * kTps;
  for (int i = 0; i < clip.tracks(); ++i) {
    const model::LaneStore::View v = store->lane(pattern, static_cast<std::uint32_t>(track0 + i));
    const std::uint32_t lo = model::LaneStore::lower_bound(v, t0);
    const std::uint32_t hi = model::LaneStore::lower_bound(v, t1);
    if (hi > lo && !clip.lanes_add(i, v.events + lo, hi - lo, t0)) {
      clip.lanes_drop();
      return LaneCopy::TooMany;
    }
  }
  clip.lanes_commit();
  return LaneCopy::Carried;
}

LanePaste::LanePaste(ActionContext& ctx, unsigned pattern, int track0, int row0, const int* steps, bool whole) noexcept
    : clip_(ctx.clip), edit_(ctx.edit), pattern_(pattern), shift_(row0 * static_cast<int>(kTps)) {
  store_ = edit_ != nullptr ? edit_->lanes() : nullptr;
  if (clip_ == nullptr || store_ == nullptr || !clip_->has_lanes() || !clip_->whole_steps()) {
    return;
  }
  active_ = true;
  clip_->lanes_shift(shift_);  // (undone by the destructor)
  for (int i = 0; i < clip_->tracks() && i < static_cast<int>(model::kMaxTracks); ++i) {
    if (steps[i] <= 0 || track0 + i >= static_cast<int>(model::kMaxTracks)) {
      continue;
    }
    const std::uint32_t t0 = static_cast<std::uint32_t>(row0) * kTps;
    const std::uint32_t t1 = static_cast<std::uint32_t>(row0 + steps[i]) * kTps;
    const model::LaneStore::View mine{clip_->lane_events(i), clip_->lane_count(i)};
    const std::uint32_t m = model::LaneStore::lower_bound(mine, t1);  // (ascending, none below t0)
    const model::LaneStore::View cur = store_->lane(pattern, static_cast<std::uint32_t>(track0 + i));
    const std::uint32_t lo = model::LaneStore::lower_bound(cur, t0);
    const std::uint32_t hi = model::LaneStore::lower_bound(cur, t1);
    if (hi == lo && m == 0) {
      continue;
    }
    differs_ = differs_ || hi - lo != m || std::memcmp(cur.events + lo, mine.events, m * sizeof(model::LaneEvent)) != 0;
    events_ += m;
    slot_[count_++] = Slot{static_cast<std::uint8_t>(track0 + i), static_cast<std::uint8_t>(i), t0, t1, whole ? model::kLaneTicks : t1, m};
  }
}

LanePaste::~LanePaste() {
  if (active_) {
    clip_->lanes_shift(-shift_);
  }
}

bool LanePaste::fits(std::size_t other_bytes) noexcept {
  if (!active_ || !differs_) {
    return true;
  }
  model::Editor::LaneSpan spans[model::kMaxTracks];
  for (std::uint32_t k = 0; k < count_; ++k) {
    spans[k] = model::Editor::LaneSpan{slot_[k].track, slot_[k].t0, slot_[k].dry_t1, slot_[k].n};
  }
  return edit_->lanes_replace_fit(pattern_, spans, count_, other_bytes);
}

bool LanePaste::apply() noexcept {
  if (!active_ || !differs_) {
    return false;
  }
  const model::Pattern* pat = edit_->project().pattern(pattern_);
  bool changed = false;
  for (std::uint32_t k = 0; pat != nullptr && k < count_; ++k) {
    const Slot& s = slot_[k];
    const std::uint32_t end = pat->track_length[s.track] * kTps;  // (a whole paste has set the lengths by now)
    const std::uint32_t t1 = s.t1 < end ? s.t1 : end;
    const model::LaneStore::View mine{clip_->lane_events(s.clip_track), clip_->lane_count(s.clip_track)};
    if (t1 > s.t0 && edit_->lane_replace(pattern_, s.track, s.t0, t1, mine.events, model::LaneStore::lower_bound(mine, t1)) == model::Status::Ok) {
      changed = true;
    }
  }
  return changed;
}

int lanes_clear_block(ActionContext& ctx, unsigned pattern, const Selection::Block& b, const PatternSource& src, std::size_t other_bytes) noexcept {
  const model::LaneStore* store = ctx.edit != nullptr ? ctx.edit->lanes() : nullptr;
  if (store == nullptr || !b.whole_steps()) {
    return 0;
  }
  model::Editor::LaneSpan spans[model::kMaxTracks];
  std::uint32_t count = 0;
  for (int t = b.track0; t <= b.track1 && t < src.tracks() && t < static_cast<int>(model::kMaxTracks); ++t) {
    const int r1 = b.row1 < src.track_length(t) - 1 ? b.row1 : src.track_length(t) - 1;
    if (b.row0 > r1) {
      continue;
    }
    const model::LaneStore::View v = store->lane(pattern, static_cast<std::uint32_t>(t));
    const std::uint32_t t0 = static_cast<std::uint32_t>(b.row0) * kTps;
    const std::uint32_t t1 = static_cast<std::uint32_t>(r1 + 1) * kTps;
    if (model::LaneStore::lower_bound(v, t1) > model::LaneStore::lower_bound(v, t0)) {
      spans[count++] = model::Editor::LaneSpan{static_cast<std::uint8_t>(t), t0, t1, 0};
    }
  }
  if (count == 0) {
    return 0;
  }
  if (!ctx.edit->lanes_replace_fit(pattern, spans, count, other_bytes)) {
    (void)say(ctx, core::Severity::Warn, "TOO BIG TO UNDO: NOTHING CHANGED");
    return -1;
  }
  int changed = 0;
  for (std::uint32_t k = 0; k < count; ++k) {
    if (ctx.edit->lane_replace(pattern, spans[k].track, spans[k].t0, spans[k].t1, nullptr, 0) == model::Status::Ok) {
      changed = 1;
    }
  }
  return changed;
}

int lanes_transpose_block(ActionContext& ctx, unsigned pattern, const Selection::Block& b, const PatternSource& src, int delta, std::size_t other_bytes) noexcept {
  const model::LaneStore* store = ctx.edit != nullptr ? ctx.edit->lanes() : nullptr;
  if (store == nullptr || !b.whole_steps() || delta == 0) {
    return 0;
  }
  std::uint16_t mask = 0;
  std::uint32_t lo[model::kMaxTracks] = {};
  std::uint32_t hi[model::kMaxTracks] = {};
  for (int t = b.track0; t <= b.track1 && t < src.tracks() && t < static_cast<int>(model::kMaxTracks); ++t) {
    const int r1 = b.row1 < src.track_length(t) - 1 ? b.row1 : src.track_length(t) - 1;
    if (b.row0 > r1) {
      continue;
    }
    lo[t] = static_cast<std::uint32_t>(b.row0) * kTps;
    hi[t] = static_cast<std::uint32_t>(r1 + 1) * kTps;
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    if (ctx.edit->lane_transpose_span(pattern, static_cast<std::uint32_t>(t), lo[t], hi[t], delta, first, last) != 0u) {
      mask = static_cast<std::uint16_t>(mask | (1u << t));
    }
  }
  if (mask == 0) {
    return 0;
  }
  // The tracks of a block may end at different rows, so the dry run takes the widest range; the records are per track and only cover the changed notes' span.
  std::uint32_t wide_lo = 0xFFFFFFFFu;
  std::uint32_t wide_hi = 0;
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    if (((mask >> t) & 1u) != 0u) {
      wide_lo = lo[t] < wide_lo ? lo[t] : wide_lo;
      wide_hi = hi[t] > wide_hi ? hi[t] : wide_hi;
    }
  }
  if (!ctx.edit->lanes_transpose_fit(pattern, mask, wide_lo, wide_hi, delta, other_bytes)) {
    (void)say(ctx, core::Severity::Warn, "TOO BIG TO UNDO: NOTHING CHANGED");
    return -1;
  }
  int changed = 0;
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    if (((mask >> t) & 1u) != 0u && ctx.edit->lane_transpose(pattern, t, lo[t], hi[t], delta) == model::Status::Ok) {
      changed = 1;
    }
  }
  return changed;
}

int lanes_rows(ActionContext& ctx, unsigned pattern, const Selection::Block& b, const PatternSource& src, bool insert) noexcept {
  const model::LaneStore* store = ctx.edit != nullptr ? ctx.edit->lanes() : nullptr;
  if (store == nullptr || !b.whole_steps()) {
    return 0;
  }
  std::uint16_t mask = 0;
  for (int t = b.track0; t <= b.track1 && t < src.tracks() && t < static_cast<int>(model::kMaxTracks); ++t) {
    if (b.row0 >= src.track_length(t)) {
      continue;
    }
    const model::LaneStore::View v = store->lane(pattern, static_cast<std::uint32_t>(t));
    if (model::LaneStore::lower_bound(v, static_cast<std::uint32_t>(b.row0) * kTps) < v.count) {  // an event at or after the row
      mask = static_cast<std::uint16_t>(mask | (1u << t));
    }
  }
  if (mask == 0) {
    return 0;
  }
  // The step writes that follow cover every track of the block, not only the tracks with lane events (review of S7c).
  if (!ctx.edit->lanes_rows_fit(pattern, mask, static_cast<std::uint32_t>(b.row0), insert, step_bytes(b.track1 - b.track0 + 1,static_cast<int>(model::kMaxPatternLength) - b.row0))) {
    (void)say(ctx, core::Severity::Warn, "TOO BIG TO UNDO: NOTHING CHANGED");
    return -1;
  }
  int changed = 0;
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    if (((mask >> t) & 1u) != 0u && ctx.edit->lane_rows(pattern, t, static_cast<std::uint32_t>(b.row0), insert) == model::Status::Ok) {
      changed = 1;
    }
  }
  return changed;
}

}  // namespace o3os::app::pe
