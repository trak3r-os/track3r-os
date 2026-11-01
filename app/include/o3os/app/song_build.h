// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FORM and BRIDGE of the Song ORDER page's TOOLS dialog (OS-SONG-BUILD, docs/specs/OS-SONG-BUILD.md; NEW COMMUNITY OS BEHAVIOUR, no format change): two builders of new order slots and new patterns, written through the
// model's own API (create_pattern, set_steps, set_track_length, set_pattern_name, insert_order). Pure over the model, in the shape of song_edit.h: a planner that reads a `Project` and writes nothing, and an executor that
// writes the plan through the `Editor` inside the caller's one undo unit. The plan is the single source of truth (the numbers of the new patterns, the slots that name them, the journal bytes), so the pre-check, the
// bytes and the writes cannot disagree.
//
// FORM (song_form_plan / song_form_apply): 1 to 3 patterns become a song shape from a const table of forms, by subtraction: a slot of the form points at a source, or at a copy of it with some groups of tracks left
// empty (kick, the rest of the drums, the music). BRIDGE (OS-SONG-BRIDGE) reuses everything below the FORM part: the numbers, the pre-check, the slot insert, the texts.
//
// The pre-check (SongBuildPlan): slots (the list holds at most kMaxSongPositions), then patterns (the pool and the free numbers), then the caller's undo ceiling on `bytes`. Nothing is written when one fails.
//
// Main loop only. No heap, no globals.

#include <o3os/app/song_edit.h>
#include <o3os/core/fixed_string.h>
#include <o3os/model/limits.h>
#include <o3os/model/pattern.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

enum class SongBuildKind : std::uint8_t { Form, Bridge };

// Why a build writes nothing (the text of each is song_build_text).
enum class SongBuildRefusal : std::uint8_t { None, Select, NoPattern, SongFull, NoRoom, NoNext, Same };

// The most new slots (and so new patterns) a build makes: BRIDGE's SLOTS row is 1..16, FORM's longest form has 10 slots.
inline constexpr unsigned kSongBuildMax = 16;

// What the footer and the strip say of a plan: small enough to live in the dialog's state (SongToolsView).
struct SongBuildInfo {
  SongBuildRefusal refusal = SongBuildRefusal::None;
  std::uint8_t number = 0;        // NoPattern: the slot; SongFull: the slots needed; NoRoom: the patterns needed
  std::uint8_t have = 0;          // SongFull: the slots free; NoRoom: the patterns that can be made (the pool or the numbers, the lesser)
  std::uint8_t slots = 0;         // the new slots
  std::uint8_t copies = 0;        // the new patterns
  std::uint8_t source_count = 0;  // FORM: the distinct source patterns (1..3); BRIDGE: A and B (2)
  std::uint8_t sources[3] = {};   // their ids, in slot order
};

// One new pattern: a copy of `from` as `to`; the tracks of `cleared` (bit t = track t) stay empty. (BRIDGE builds its own bodies and leaves `cleared` 0.)
struct SongBuildCopy {
  std::uint8_t from = 0;
  std::uint8_t to = 0;
  std::uint16_t cleared = 0;
};

struct SongBuildPlan {
  SongBuildInfo info;
  unsigned at = 0;             // the list position of the first new slot
  unsigned free_numbers = 0;   // pattern numbers 1..255 that no pattern has and no slot names
  unsigned pool_left = 0;      // patterns the pool can still take
  std::size_t bytes = 0;       // the journal bytes of the whole build, the unit's header and trailer included; 0 when nothing is written
  std::uint8_t slot_pattern[kSongBuildMax] = {};  // the pattern each new slot names, in order
  SongBuildCopy copy[kSongBuildMax];              // the new patterns, `info.copies` of them
};

// ---- shared by FORM and BRIDGE ----------------------------------------------------------------------------------------------------

// The slots and patterns checks of the pre-check for `new_slots` slots and `need` new patterns: fills `free_numbers`, `pool_left` and, on a refusal, `info.refusal` / `number` / `have`. Returns the refusal (None: it fits).
SongBuildRefusal song_build_precheck(const model::Project& project, unsigned new_slots, unsigned need, SongBuildPlan& plan) noexcept;

// Gives plan.copy[0..info.copies) their numbers: the first free numbers, ascending (SONG-UX D15). False when there are not enough (the pre-check says so first; it then also sets NoRoom with its numbers in plan.info).
bool song_build_assign_numbers(const model::Project& project, SongBuildPlan& plan) noexcept;

// Inserts the plan's new slots at `plan.at` as one change for a playing song (an Editor::OrderBatch). False when the list refused one (the pre-check ruled it out).
bool song_build_insert_slots(model::Editor& editor, const SongBuildPlan& plan) noexcept;

// The strip's text for a refusal (every text is 48 characters at most; none names an F-key).
void song_build_text(SongBuildKind kind, const SongBuildInfo& info, core::FixedString<48>& out) noexcept;

// ---- FORM ---------------------------------------------------------------------------------------------------------------------------

enum class SongForm : std::uint8_t { LoopSong = 0, Pop = 1, Techno = 2 };
inline constexpr unsigned kSongFormCount = 3;
inline constexpr unsigned kSongDrumsMax = 8;      // DRUMS row: 1..8 tracks
inline constexpr unsigned kSongDrumsDefault = 4;

// The groups of tracks a form clears (bits of an entry's low nibble) and the role of its source (bits 4-5: 0 = A, 1 = B, 2 = C).
inline constexpr std::uint8_t kFormKick = 1;
inline constexpr std::uint8_t kFormPerc = 2;
inline constexpr std::uint8_t kFormMusic = 4;

// The table of one form: `count` entries (role | cleared groups), one per slot.
const std::uint8_t* song_form_table(SongForm form, unsigned& count) noexcept;
const char* song_form_name(SongForm form) noexcept;
// The tracks of the groups in `groups` for DRUMS = `drums`: kick = track 1, perc = tracks 2..drums, music = tracks drums+1..16. A bit mask of tracks.
std::uint16_t song_form_tracks(std::uint8_t groups, unsigned drums) noexcept;

// Plans FORM over the slots `rect.s0..s1` (the tracks of the rectangle are ignored). Writes nothing. A refusal is in plan.info.refusal; `plan.bytes` is set when none.
void song_form_plan(const model::Project& project, const SongRect& rect, SongForm form, unsigned drums, SongBuildPlan& plan) noexcept;

// Writes the plan: each copy (song_copy_pattern_masked), then the slots. The caller's undo unit holds it. False when the model refused a step (the caller checked the pool and the list first: a bug).
bool song_form_apply(model::Editor& editor, const SongBuildPlan& plan) noexcept;

// The footer of FORM's dialog: `FROM 02 05 07  8 SLOTS  3 NEW PATTERNS` (at most 43 characters), or the refusal's text.
void song_form_footer(const SongBuildInfo& info, core::FixedString<48>& out) noexcept;

// ---- BRIDGE (OS-SONG-BRIDGE) -----------------------------------------------------------------------------------------------------

inline constexpr unsigned kSongBridgeMax = kSongBuildMax;  // SLOTS row: 1..16
inline constexpr unsigned kSongBridgeDefault = 4;
inline constexpr std::uint32_t kBridgeSeed = 0x0B91D6E5u;  // D15: part of the contract (the tests pin the hash); no clock, no rand

// The 32-bit integer mix of BRIDGE (D13, D15): a xorshift-multiply over (seed, track), then over (.., step). `step` = kBridgeNoStep is the track's own hash (the phase). No state.
inline constexpr unsigned kBridgeNoStep = 0xFFFFu;
std::uint32_t song_bridge_hash(std::uint32_t seed, unsigned track, unsigned step = kBridgeNoStep) noexcept;

// phase(tr) = 2048 x the rank of hash(seed, tr) among the 16 tracks (ties by track number): 0..30720.
void song_bridge_phases(std::uint32_t seed, std::uint16_t out[model::kMaxTracks]) noexcept;

// t16 of bridge slot k (1..n) of n: k * 65536 / (n + 1), the share of the way from A to B in 1/65536.
unsigned song_bridge_t16(unsigned k, unsigned n) noexcept;

// thr(tr, i) = phase(tr) + (hash(seed, tr, i) >> 17): the cell takes B's step when thr < t16.
unsigned song_bridge_thr(std::uint32_t seed, const std::uint16_t phase[model::kMaxTracks], unsigned track, unsigned step) noexcept;

// Track `track` of the bridge slot at `t16`: returns its length (A's while 2 x t16 <= 65536, B's after) and puts the steps 0..length-1 in `out` (kMaxPatternLength entries). Each cell is A's or B's step whole:
// A.steps[track][i % lenA] or B.steps[track][i % lenB] (a short track loops, as the sequencer plays it).
unsigned song_bridge_track(const model::Pattern& a, const model::Pattern& b, const std::uint16_t phase[model::kMaxTracks], unsigned track, unsigned t16, model::Step* out) noexcept;

// Plans BRIDGE over the slots `rect.s0..s1` (the rectangle's tracks are ignored, D18): A is the first slot and B the slot after it, the rectangle being the cursor's slot or exactly two slots; `slots` new slots (1..16)
// go between them. Writes nothing. A refusal is in plan.info.refusal; `plan.bytes` is set when none, unless `with_bytes` is false: then the per-cell walk that counts the journal bytes (up to ~33k cells) is skipped
// and `bytes` stays 0 (the dialog's footer needs only the counts; F1 plans with the bytes for the undo cap). plan.info.sources = {A, B}.
void song_bridge_plan(const model::Project& project, const SongRect& rect, unsigned slots, SongBuildPlan& plan, bool with_bytes = true) noexcept;

// Writes the plan: each bridge pattern (create, track lengths, steps, the name `BRIDGE k/N`), then the slots. The caller's undo unit holds it. False when the model refused a step (the pre-check ran first: a bug).
bool song_bridge_apply(model::Editor& editor, const SongBuildPlan& plan) noexcept;

// The footer of BRIDGE's dialog: `PATTERN 02 TO 05  4 SLOTS  4 NEW PATTERNS`, or the refusal's text.
void song_bridge_footer(const SongBuildInfo& info, core::FixedString<48>& out) noexcept;

}  // namespace o3os::app
