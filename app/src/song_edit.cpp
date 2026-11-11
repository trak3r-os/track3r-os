// SPDX-License-Identifier: GPL-3.0-or-later
// The edits of the Song map (OS-SONGMAP, docs/specs/SONG-UX.md section 1; song_edit.h): the planner and the executor over one walk, copy of the cells, UNIQUE. New Community OS behaviour, no format change.
#include <o3os/app/song_edit.h>

#include "song_edit_internal.h"

#include <o3os/app/song_clip.h>
#include <o3os/model/editor.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/limits.h>
#include <o3os/model/project.h>
#include <o3os/model/step_edit.h>
#include <o3os/model/undo.h>

#include <cstring>

namespace o3os::app {

namespace {

using model::Step;

static_assert(song_record_bytes(3 + 2 * sizeof(Step)) + kSongUnitOverhead == model::UndoJournal::unit_bytes_for(3 + 2 * sizeof(Step)), "the planner counts the journal as undo.h does");
static_assert(kSongUnitOverhead == model::UndoJournal::kUnitHeaderBytes + model::UndoJournal::kUnitTrailerBytes, "a unit's header and trailer");
static_assert(song_record_bytes(0) == model::UndoJournal::kRecordOverheadBytes, "a record's header and trailer");
static_assert(SongClip::kSteps == model::kMaxPatternLength, "a clip cell is a whole track");

// The slots of the list that the rectangle covers, and its tracks, inside the list and the project's active tracks.
struct Span {
  int s0;
  int s1;
  int t0;
  int t1;
};

Span span_of(const model::Project& p, const SongRect& r) noexcept {
  const int count = static_cast<int>(p.song_length());
  const int active = static_cast<int>(p.active_tracks());
  Span s{};
  s.s0 = r.s0 < 0 ? 0 : r.s0;
  s.s1 = r.s1 < count ? r.s1 : count - 1;
  s.t0 = r.t0 < 0 ? 0 : r.t0;
  s.t1 = r.t1 < active ? r.t1 : active - 1;
  return s;
}

// The ids that a slot outside the slots s0..s1 names.
SongIdSet outside_of(const model::Project& p, int s0, int s1) noexcept {
  SongIdSet out;
  const int count = static_cast<int>(p.song_length());
  for (int s = 0; s < count; ++s) {
    if (s < s0 || s > s1) {
      out.add(p.order(static_cast<std::uint32_t>(s)));
    }
  }
  return out;
}

// Applies the tool of `e` to one step: the rules of Pattern's block edits (model/step_edit.h), the multi-step branch (a note is never made from nothing).
void apply_tool(const SongEdit& e, Step& step) noexcept {
  const model::edit::Rule rule{false, false};
  model::edit::Last last{};
  switch (e.op) {
    case SongOp::Clear:
      model::edit::clear(step, e.elements);
      break;
    case SongOp::Transpose:
      model::edit::note_delta(step, e.amount, rule, last);
      break;
    case SongOp::Instrument:
      model::edit::instrument_set(step, e.instrument, rule, last);
      break;
    case SongOp::Paste:
      break;
  }
}

// The one walk of an edit, which both the planner and the executor take (a pattern's steps are read here and nowhere else): for each distinct pattern of the rectangle's slots, in slot order, and each of its tracks,
// the steps the edit leaves there, reduced to the span from the first step that changes to the last. A track the edit leaves as it is is not offered. `sink.write(id, track, first, steps, count, exists)` is called
// for each span; `exists` is whether the pattern exists at that moment (a paste into one that does not exist yet creates it: the executor does, so the next track of it sees it).
template <class Sink>
void walk(const model::Project& p, const SongEdit& e, const SongClip* clip, Sink& sink) noexcept {
  if (e.op == SongOp::Paste && (clip == nullptr || clip->empty())) {
    return;
  }
  const Span span = span_of(p, e.rect);
  SongIdSet seen;
  for (int s = span.s0; s <= span.s1; ++s) {
    const unsigned id = p.order(static_cast<std::uint32_t>(s));
    if (id == 0u || seen.has(id)) {
      continue;  // once per distinct pattern: the top-most slot (the top-most clip row) decides (D5)
    }
    seen.add(id);
    for (int t = span.t0; t <= span.t1; ++t) {
      const model::Pattern* pat = p.pattern(id);  // (taken again for every track: the sink may have created the pattern)
      const bool exists = pat != nullptr;
      if (!exists && e.op != SongOp::Paste) {
        continue;  // a clear or a tool skips a pattern that does not exist (D10)
      }
      const unsigned target = exists ? pat->track_length[t] : p.default_pattern_length();
      Step buf[model::kMaxPatternLength];
      unsigned n = 0;
      if (e.op == SongOp::Paste) {
        const int r = (s - e.rect.s0) % clip->rows();
        const int c = (t - e.rect.t0) % clip->cols();
        const unsigned len = clip->length(r, c);
        n = len < target ? len : target;  // clipped at the target track's end (Q2)
        n = n < model::kMaxPatternLength ? n : model::kMaxPatternLength;
        std::memcpy(buf, clip->cell(r, c), n * sizeof(Step));
        sink.cell(len > target);
      } else {
        n = target < model::kMaxPatternLength ? target : model::kMaxPatternLength;
        std::memcpy(buf, pat->steps[t], n * sizeof(Step));
        for (unsigned k = 0; k < n; ++k) {
          apply_tool(e, buf[k]);
        }
      }
      int first = -1;
      int last = -1;
      for (unsigned k = 0; k < n; ++k) {
        const Step& now = exists ? pat->steps[t][k] : model::kEmptyStep;
        if (std::memcmp(&buf[k], &now, sizeof now) != 0) {
          first = first < 0 ? static_cast<int>(k) : first;
          last = static_cast<int>(k);
        }
      }
      if (first >= 0) {
        sink.write(id, static_cast<unsigned>(t), static_cast<unsigned>(first), buf + first, static_cast<unsigned>(last - first + 1), exists);
      }
    }
  }
}

// What the planner counts of a span: the journal bytes of the records the Editor will write for it, the patterns it changes, the ones it creates.
struct PlanSink {
  SongPlan& out;
  const SongIdSet& outside;
  SongIdSet changed;
  SongIdSet created;
  std::size_t body = 0;

  void cell(bool clipped) noexcept { out.clipped = out.clipped || clipped; }
  void write(unsigned id, unsigned, unsigned, const Step*, unsigned count, bool exists) noexcept {
    ++out.writes;
    body += song_record_bytes(4u + 2u * count * sizeof(Step));  // Editor::set_steps: [id, track, first, count], the new steps, the old
    if (!exists && !created.has(id)) {
      created.add(id);
      ++out.creates;
      body += song_record_bytes(2);  // Editor::create_pattern: [id, length]
    }
    if (!changed.has(id)) {
      changed.add(id);
      ++out.patterns;
      if (outside.has(id)) {
        ++out.shared;
        out.shared_ids.add(id);
        out.first_shared = out.first_shared == 0u ? id : out.first_shared;
      }
    }
  }
};

struct WriteSink {
  model::Editor& editor;
  SongDone& out;
  SongIdSet changed;

  void cell(bool) noexcept {}
  void write(unsigned id, unsigned track, unsigned first, const Step* values, unsigned count, bool) noexcept {
    if (out.failed) {
      return;
    }
    if (editor.project().pattern(id) == nullptr) {
      if (editor.create_pattern(id, editor.project().default_pattern_length()) != model::Status::Ok) {
        out.failed = true;
        return;
      }
      ++out.creates;
    }
    if (editor.set_steps(id, track, first, values, count) == model::Status::Ok) {
      ++out.writes;
      if (!changed.has(id)) {
        changed.add(id);
        ++out.patterns;
      }
    }
  }
};

// ---- UNIQUE ------------------------------------------------------------------------------------------------------------------

// The journal bytes of the lane copies of the tracks in `mask` (OS-MIDIREC-P1 S7a: one insert record a lane that has events below the track's end, as Editor::lane_copy writes it); 0 without a store.
std::size_t lane_copy_body(const model::Project& p, const model::LaneStore* lanes, unsigned from, std::uint16_t mask) noexcept {
  const model::Pattern* src = p.pattern(from);
  if (lanes == nullptr || src == nullptr || !lanes->in_sync(p.load_epoch())) {
    return 0;
  }
  std::size_t body = 0;
  for (std::uint32_t t = 0; t < model::kMaxTracks; ++t) {
    if (((mask >> t) & 1u) != 0u) {
      const std::uint32_t n = model::LaneStore::lower_bound(lanes->lane(from, t), src->track_length[t] * model::kLaneTicksPerStep);
      body += n != 0u ? song_record_bytes(6u + n * sizeof(model::LaneEvent)) : 0u;  // [id, track, index, count], the events
    }
  }
  return body;
}

// The journal bytes song_copy_pattern writes for `from` (the same records, in the same conditions: keep the two together), the lanes' (`lanes` given) last.
std::size_t copy_pattern_body(const model::Project& p, unsigned from, const model::LaneStore* lanes = nullptr) noexcept {
  const model::Pattern* src = p.pattern(from);
  std::size_t body = song_record_bytes(2);  // create_pattern
  if (src == nullptr) {
    return body;  // a pattern that does not exist yet copies as an empty one
  }
  const std::uint32_t whole = p.pattern_length(from);
  const std::uint32_t made = whole != 0u ? whole : p.default_pattern_length();
  for (std::uint32_t t = 0; t < model::kMaxTracks; ++t) {
    if (src->track_length[t] != made) {
      body += song_record_bytes(4);  // set_track_length: [id, track, new, old]
    }
    bool any = false;
    for (std::uint32_t k = 0; k < model::kMaxPatternLength && !any; ++k) {
      any = !model::is_empty(src->steps[t][k]);
    }
    if (any) {
      body += song_record_bytes(4u + 2u * model::kMaxPatternLength * sizeof(Step));  // set_steps of the whole track
    }
  }
  const std::size_t name = std::strlen(src->name);
  if (name != 0u) {
    body += song_record_bytes(3u + name);  // set_pattern_name: [id, len], the text, [old len], no old text
  }
  return body + lane_copy_body(p, lanes, from, 0xFFFFu);
}

// The span of track `t` that a masked copy writes: from the first step that holds something to the last, inside the track's length; none (count 0) for a cleared track or one with nothing in it.
void masked_span(const model::Pattern& src, unsigned t, std::uint16_t cleared, unsigned& first, unsigned& count) noexcept {
  first = 0;
  count = 0;
  if (((cleared >> t) & 1u) != 0u) {
    return;
  }
  int lo = -1;
  int hi = -1;
  for (unsigned k = 0; k < src.track_length[t] && k < model::kMaxPatternLength; ++k) {
    if (!model::is_empty(src.steps[t][k])) {
      lo = lo < 0 ? static_cast<int>(k) : lo;
      hi = static_cast<int>(k);
    }
  }
  if (lo >= 0) {
    first = static_cast<unsigned>(lo);
    count = static_cast<unsigned>(hi - lo + 1);
  }
}

}  // namespace

// The first pattern number that no pattern has and no slot names (a slot that names a pattern that does not exist yet would start to play the copy), that is not in `taken`; 0 when there is none.
unsigned free_number(const model::Project& p, const SongIdSet& named, const SongIdSet& taken) noexcept {
  for (unsigned id = 1; id <= model::kMaxPatternId; ++id) {
    if (p.pattern(id) == nullptr && !named.has(id) && !taken.has(id)) {
      return id;
    }
  }
  return 0;
}

void lane_copy_counts(const model::Project& p, const model::LaneStore* lanes, unsigned from, std::uint16_t mask, std::uint32_t& events, std::uint32_t& lane_count) noexcept {
  const model::Pattern* src = p.pattern(from);
  if (lanes == nullptr || src == nullptr || !lanes->in_sync(p.load_epoch())) {
    return;
  }
  for (std::uint32_t t = 0; t < model::kMaxTracks; ++t) {
    if (((mask >> t) & 1u) != 0u) {
      const std::uint32_t n = model::LaneStore::lower_bound(lanes->lane(from, t), src->track_length[t] * model::kLaneTicksPerStep);
      events += n;
      lane_count += n != 0u ? 1u : 0u;
    }
  }
}

bool lane_pool_fits(const model::LaneStore* lanes, std::uint32_t events, std::uint32_t lane_count) noexcept {
  return lanes == nullptr || events == 0u || (lanes->total_events() + events <= model::kMaxLanePoolEvents && lanes->lane_count() + lane_count <= model::kMaxLanes);
}

namespace {

// The shared patterns of the slots s0..s1 in slot order, each with the number its copy takes: `sink.copy(from, to, slots)` once per pattern, `slots` being the slots of the range that hold it.
template <class Sink>
void unique_walk(const model::Project& p, int s0, int s1, const SongIdSet* only, SongUnique& out, Sink& sink) noexcept {
  const int count = static_cast<int>(p.song_length());
  s0 = s0 < 0 ? 0 : s0;
  s1 = s1 < count ? s1 : count - 1;
  const SongIdSet outside = outside_of(p, s0, s1);
  SongIdSet named;
  for (int s = 0; s < count; ++s) {
    named.add(p.order(static_cast<std::uint32_t>(s)));
  }
  SongIdSet seen;
  SongIdSet taken;
  unsigned pool = static_cast<unsigned>(p.pattern_count());
  for (int s = s0; s <= s1; ++s) {
    const unsigned id = p.order(static_cast<std::uint32_t>(s));
    if (id == 0u || seen.has(id) || !outside.has(id) || (only != nullptr && !only->has(id))) {
      continue;
    }
    seen.add(id);
    const unsigned to = free_number(p, named, taken);
    if (to == 0u || pool >= model::kMaxPatterns) {
      out.no_room = true;
      return;
    }
    taken.add(to);
    ++pool;
    unsigned slots = 0;
    for (int k = s; k <= s1; ++k) {
      slots += p.order(static_cast<std::uint32_t>(k)) == id ? 1u : 0u;
    }
    if (out.copies == 0u) {
      out.first_from = id;
      out.first_to = to;
    }
    ++out.copies;
    out.slots += slots;
    sink.copy(id, to, s, s1);
  }
}

struct UniquePlanSink {
  const model::Project& project;
  SongUnique& out;
  std::size_t body = 0;
  const model::LaneStore* lanes = nullptr;
  std::uint32_t lane_events = 0;  // the lane pool all the copies take together (OS-MIDIREC-P1 S7a)
  std::uint32_t lane_count = 0;
  void copy(unsigned from, unsigned, int s, int s1) noexcept {
    body += copy_pattern_body(project, from, lanes);
    lane_copy_counts(project, lanes, from, 0xFFFFu, lane_events, lane_count);
    for (int k = s; k <= s1; ++k) {
      body += project.order(static_cast<std::uint32_t>(k)) == from ? song_record_bytes(3) : 0u;  // set_order: [pos, new, old]
    }
  }
};

struct UniqueWriteSink {
  model::Editor& editor;
  void copy(unsigned from, unsigned to, int s, int s1) noexcept {
    if (song_copy_pattern(editor, from, to) != model::Status::Ok) {
      return;  // (the caller checked the pattern pool and the lane pool, song_unique_plan: a refusal here is a bug, and the slots stay where they are)
    }
    for (int k = s; k <= s1; ++k) {
      if (editor.project().order(static_cast<std::uint32_t>(k)) == from) {
        (void)editor.set_order(static_cast<std::uint32_t>(k), to);
      }
    }
  }
};

}  // namespace

unsigned song_distinct(const model::Project& project, const SongRect& rect) noexcept {
  const Span span = span_of(project, rect);
  SongIdSet seen;
  unsigned n = 0;
  for (int s = span.s0; s <= span.s1; ++s) {
    const unsigned id = project.order(static_cast<std::uint32_t>(s));
    if (id != 0u && !seen.has(id)) {
      seen.add(id);
      n += project.pattern(id) != nullptr ? 1u : 0u;
    }
  }
  return n;
}

void song_plan(const model::Project& project, const SongEdit& edit, const SongClip* clip, SongPlan& out) noexcept {
  out = SongPlan{};
  const Span span = span_of(project, edit.rect);
  const SongIdSet outside = outside_of(project, span.s0, span.s1);
  PlanSink sink{out, outside, {}, {}, 0};
  walk(project, edit, clip, sink);
  out.bytes = sink.body != 0u ? kSongUnitOverhead + sink.body : 0u;
  const int count = static_cast<int>(project.song_length());
  for (int s = 0; s < count; ++s) {
    if ((s < span.s0 || s > span.s1) && out.shared_ids.has(project.order(static_cast<std::uint32_t>(s)))) {
      ++out.shared_slots;
    }
  }
}

void song_apply(model::Editor& editor, const SongEdit& edit, const SongClip* clip, SongDone& out) noexcept {
  out = SongDone{};
  WriteSink sink{editor, out, {}};
  walk(editor.project(), edit, clip, sink);
}

bool song_copy_cells(const model::Project& project, const SongRect& rect, SongClip& clip) noexcept {
  const Span span = span_of(project, rect);
  if (span.s1 < span.s0 || span.t1 < span.t0 || !clip.begin(span.s1 - span.s0 + 1, span.t1 - span.t0 + 1, project.default_pattern_length())) {
    return false;
  }
  for (int r = 0; r <= span.s1 - span.s0; ++r) {
    const model::Pattern* pat = project.pattern(project.order(static_cast<std::uint32_t>(span.s0 + r)));
    for (int c = 0; c <= span.t1 - span.t0; ++c) {
      Step* cell = clip.cell(r, c);
      if (pat == nullptr) {
        for (unsigned k = 0; k < clip.length(r, c); ++k) {
          cell[k] = model::kEmptyStep;  // a slot that names a pattern that does not exist copies as empty cells of the default length (D10)
        }
        continue;
      }
      const unsigned len = pat->track_length[span.t0 + c];
      std::memcpy(cell, pat->steps[span.t0 + c], len * sizeof(Step));
      clip.set_length(r, c, len);
    }
  }
  return true;
}

model::Status song_copy_pattern(model::Editor& e, unsigned from, unsigned to) noexcept {
  const model::Pattern* src = e.project().pattern(from);
  const std::uint32_t whole = src != nullptr ? e.project().pattern_length(from) : 0u;
  if (src != nullptr && !e.lanes_copy_fit(from, 0xFFFFu, copy_pattern_body(e.project(), from))) {
    return model::Status::Full;  // the lanes would not fit the pool or the undo ring: nothing is created (OS-MIDIREC-P1 S7a)
  }
  const model::Status st = e.create_pattern(to, whole != 0u ? whole : e.project().default_pattern_length());
  if (st != model::Status::Ok || src == nullptr) {
    return st;
  }
  src = e.project().pattern(from);  // (the pool may have moved nothing, but the pointer is cheap to take again)
  for (std::uint32_t t = 0; t < model::kMaxTracks; ++t) {
    if (src->track_length[t] != e.project().pattern(to)->track_length[t]) {
      (void)e.set_track_length(to, t, src->track_length[t]);
    }
    bool any = false;
    for (std::uint32_t s = 0; s < model::kMaxPatternLength && !any; ++s) {
      any = !model::is_empty(src->steps[t][s]);
    }
    if (any) {
      (void)e.set_steps(to, t, 0, src->steps[t], model::kMaxPatternLength);
    }
    (void)e.lane_copy(from, to, t);  // checked above: it fits
  }
  (void)e.set_pattern_name(to, src->name, std::strlen(src->name));
  return model::Status::Ok;
}

std::size_t copy_pattern_body_masked(const model::Project& p, unsigned from, std::uint16_t cleared_tracks, const model::LaneStore* lanes) noexcept {
  const model::Pattern* src = p.pattern(from);
  std::size_t body = song_record_bytes(2);  // create_pattern
  if (src == nullptr) {
    return body;
  }
  const std::uint32_t whole = p.pattern_length(from);
  const std::uint32_t made = whole != 0u ? whole : p.default_pattern_length();
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    if (src->track_length[t] != made) {
      body += song_record_bytes(4);  // set_track_length: [id, track, new, old]
    }
    unsigned first = 0;
    unsigned count = 0;
    masked_span(*src, t, cleared_tracks, first, count);
    if (count != 0u) {
      body += song_record_bytes(4u + 2u * count * sizeof(Step));  // set_steps of the span: [id, track, first, count], the new steps, the old
    }
  }
  const std::size_t name = std::strlen(src->name);
  if (name != 0u) {
    body += song_record_bytes(3u + name);  // set_pattern_name: [id, len], the text, [old len], no old text
  }
  return body + lane_copy_body(p, lanes, from, static_cast<std::uint16_t>(~cleared_tracks));  // the lanes of the tracks that are not cleared
}

model::Status song_copy_pattern_masked(model::Editor& e, unsigned from, unsigned to, std::uint16_t cleared_tracks) noexcept {
  const model::Pattern* src = e.project().pattern(from);
  if (src == nullptr) {
    return model::Status::NotFound;
  }
  const std::uint16_t kept = static_cast<std::uint16_t>(~cleared_tracks);
  if (!e.lanes_copy_fit(from, kept, copy_pattern_body_masked(e.project(), from, cleared_tracks, nullptr))) {
    return model::Status::Full;  // the lanes would not fit the pool or the undo ring: nothing is created (OS-MIDIREC-P1 S7a)
  }
  const std::uint32_t whole = e.project().pattern_length(from);
  const model::Status st = e.create_pattern(to, whole != 0u ? whole : e.project().default_pattern_length());
  if (st != model::Status::Ok) {
    return st;
  }
  src = e.project().pattern(from);  // (taken again after the creation, as song_copy_pattern does)
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    if (src->track_length[t] != e.project().pattern(to)->track_length[t]) {
      (void)e.set_track_length(to, t, src->track_length[t]);
    }
    unsigned first = 0;
    unsigned count = 0;
    masked_span(*src, t, cleared_tracks, first, count);
    if (count != 0u) {
      (void)e.set_steps(to, t, first, &src->steps[t][first], count);
    }
    if (((kept >> t) & 1u) != 0u) {
      (void)e.lane_copy(from, to, t);  // checked above: it fits
    }
  }
  (void)e.set_pattern_name(to, src->name, std::strlen(src->name));
  return model::Status::Ok;
}

void song_unique_plan(const model::Project& project, int s0, int s1, const SongIdSet* only, SongUnique& out, const model::LaneStore* lanes) noexcept {
  out = SongUnique{};
  UniquePlanSink sink{project, out, 0, lanes};
  unique_walk(project, s0, s1, only, out, sink);
  if (!lane_pool_fits(lanes, sink.lane_events, sink.lane_count)) {
    out.no_room = true;  // the copies' lanes together do not fit the lane pool: no copy is made (a skipped copy would leave its slots shared)
  }
  out.bytes = sink.body != 0u ? kSongUnitOverhead + sink.body : 0u;
}

void song_unique_apply(model::Editor& editor, int s0, int s1, const SongIdSet* only, SongUnique& out) noexcept {
  out = SongUnique{};
  UniqueWriteSink sink{editor};
  unique_walk(editor.project(), s0, s1, only, out, sink);
}

}  // namespace o3os::app
