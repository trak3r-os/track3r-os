// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The edits of the Song map (OS-SONGMAP, docs/specs/SONG-UX.md section 1; new Community OS behaviour, no format change): a rectangle of slots x tracks of the order list, and what copy, paste, clear, the TOOLS
// dialog and UNIQUE do to the patterns those slots name. Pure over the model: the planner reads a `Project` and writes nothing, the executor writes through the `Editor` (so every edit is in the caller's undo unit).
//
// Distinct patterns. An edit acts once per (pattern, track), never once per slot: pattern 02 in two selected slots is changed once, and when two rows of the clip would write different steps to the same pattern
// the top-most row wins (D5). The planner and the executor walk the same list with the same arithmetic (`walk` in song_edit.cpp), so the question's pattern set (1.6), the undo size (1.7) and the writes cannot
// disagree.
//
// Undo size (1.7). `SongPlan::bytes` is what the edit puts in the journal, record by record (the changed span of each track as Editor::set_steps records it, the pattern creations, the unit's header and trailer): an
// exact count, because a unit that outgrows its ring silently resets the whole Song history (undo.h). The caller refuses an edit over half the ring.
//
// Main loop only. No heap, no globals.

#include <o3os/model/status.h>

#include <cstddef>
#include <cstdint>

namespace o3os::model {
class Project;
class Editor;
}  // namespace o3os::model

namespace o3os::app {

class SongClip;

// The rectangle: slots s0..s1 x tracks t0..t1, 0-based and inclusive.
struct SongRect {
  int s0 = 0;
  int s1 = -1;
  int t0 = 0;
  int t1 = -1;
  constexpr int slots() const noexcept { return s1 >= s0 ? s1 - s0 + 1 : 0; }
  constexpr int tracks() const noexcept { return t1 >= t0 ? t1 - t0 + 1 : 0; }
  constexpr bool single() const noexcept { return slots() == 1 && tracks() == 1; }
};

enum class SongOp : std::uint8_t { Paste, Clear, Transpose, Instrument };

// One edit of the rectangle. `Paste` tiles the clip from the rectangle's top-left (the clip's cell of slot s, track t is row (s - s0) % rows, column (t - t0) % cols), so the caller makes the rectangle the clip's own
// size from the cursor (no selection) or the selection itself (the clip tiled to fill it, Q3).
struct SongEdit {
  SongOp op = SongOp::Clear;
  SongRect rect;
  int amount = 0;                // Transpose: semitones
  std::uint8_t instrument = 1;   // Instrument: 1..64
  std::uint8_t elements = 0;     // Clear: model::edit::kEl* bits
};

// A bitset of pattern ids 0..255.
struct SongIdSet {
  std::uint8_t bits[32] = {};
  bool has(unsigned id) const noexcept { return id < 256u && ((bits[id >> 3] >> (id & 7u)) & 1u) != 0u; }
  void add(unsigned id) noexcept {
    if (id < 256u) {
      bits[id >> 3] = static_cast<std::uint8_t>(bits[id >> 3] | (1u << (id & 7u)));
    }
  }
};

// What an edit would do, from the planner.
struct SongPlan {
  unsigned patterns = 0;       // distinct patterns the edit changes
  unsigned writes = 0;         // (pattern, track) spans it writes
  unsigned creates = 0;        // missing patterns a paste creates
  unsigned shared = 0;         // of `patterns`, the ones a slot outside the rectangle also holds (the question, 1.6)
  unsigned shared_slots = 0;   // those outside slots
  unsigned first_shared = 0;   // the first of the shared patterns in slot order (the question names it when it is one)
  SongIdSet shared_ids;        // ... and all of them
  bool clipped = false;        // paste: a clip cell was longer than its target track
  std::size_t bytes = 0;       // the journal bytes of the edit alone, the unit's header and trailer included; 0 when nothing is written
};

// The distinct patterns the slots of the rectangle name that exist (a clear or a tool skips one that does not exist yet, D10: this is the count its result will give, at most).
unsigned song_distinct(const model::Project& project, const SongRect& rect) noexcept;

// Plans `edit` over `project`. `clip` is read for a Paste only (null: nothing to write). Writes nothing.
void song_plan(const model::Project& project, const SongEdit& edit, const SongClip* clip, SongPlan& out) noexcept;

// What the executor did.
struct SongDone {
  unsigned patterns = 0;
  unsigned writes = 0;
  unsigned creates = 0;
  bool failed = false;  // the pattern pool refused a creation part-way (the caller checked the pool first: a bug)
};
// Writes `edit` through `editor`: the same walk as song_plan. The caller's undo unit holds it.
void song_apply(model::Editor& editor, const SongEdit& edit, const SongClip* clip, SongDone& out) noexcept;

// Copy: the cells of the rectangle into `clip` (rows = slots, columns = tracks). False (the clip is left as it was) when the rectangle holds more cells than the clip, or none.
bool song_copy_cells(const model::Project& project, const SongRect& rect, SongClip& clip) noexcept;

// UNIQUE (1.5). The patterns of the slots s0..s1 that a slot outside also holds each get one copy, at the first free number that no slot names, and every slot of the range that held the pattern points at the
// copy (slots that shared it keep sharing the copy, D4). `only` (null: every shared pattern) limits it to those ids. `copies` and `bytes` are the copies' journal bytes (the unit's header and trailer included
// when something is written), `no_room` is set when the pattern pool cannot take them.
struct SongUnique {
  unsigned copies = 0;
  unsigned slots = 0;         // slots pointed at a copy
  unsigned first_from = 0;    // the first pattern copied and its copy
  unsigned first_to = 0;
  bool no_room = false;
  std::size_t bytes = 0;
};
void song_unique_plan(const model::Project& project, int s0, int s1, const SongIdSet* only, SongUnique& out) noexcept;
void song_unique_apply(model::Editor& editor, int s0, int s1, const SongIdSet* only, SongUnique& out) noexcept;

// The pattern `from` copied to the new number `to` through the editor: its name, every track's length and every track's steps (the ones past a track's length too: a copy is exact). A pattern that does not exist
// yet copies as an empty pattern of the default length. Status::Ok, or what the model said (Full: the pattern pool). DUP PAT (Shift + Insert) and UNIQUE use it.
model::Status song_copy_pattern(model::Editor& editor, unsigned from, unsigned to) noexcept;

// FORM's copy (OS-SONG-BUILD D11): the pattern `from` (which must exist) as the new pattern `to`, with its name and every track's length; the tracks of `cleared_tracks` (bit t = track t) stay empty and the others hold
// the source's steps up to the track's length (the ones past it never sound and are not copied), written as the span from the first to the last step that holds something. Status::Ok, or what the model said.
model::Status song_copy_pattern_masked(model::Editor& editor, unsigned from, unsigned to, std::uint16_t cleared_tracks) noexcept;

// The bytes one journal record of `payload` bytes takes, and the fixed part of a unit (header and trailer): the arithmetic of undo.h, for the planners.
constexpr std::size_t song_record_bytes(std::size_t payload) noexcept { return 8u + ((payload + 3u) & ~std::size_t{3}); }
inline constexpr std::size_t kSongUnitOverhead = 16u + 4u;

// The journal bytes of two edits in one unit: both carry the unit's header and trailer, the unit has them once.
constexpr std::size_t song_bytes_sum(std::size_t a, std::size_t b) noexcept { return a + b - (a != 0u && b != 0u ? kSongUnitOverhead : 0u); }

// Half of a ring's bytes: the largest unit a Song edit may make (1.7). 48 KiB on the device.
constexpr std::size_t song_undo_limit(std::size_t ring_bytes) noexcept { return ring_bytes / 2u; }

}  // namespace o3os::app
