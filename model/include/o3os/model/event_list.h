// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-MIDIREC-P1 S6 (docs/specs/OS-MIDIREC-P1.md, "What EVENTS shows"): the lane rows. When Env::lanes is set the list also
// carries the take lanes of the pattern's tracks: one row per lane event (a NOTE row for a lane note, a CC row for every
// other kind), keyed by its place in the lane (Key::lane), merged by time with the step rows, step rows first at one tick,
// then lane rows in tick order (track, then lane order). A lane row edits through Plan::lane, a write the action applies
// with Editor::lane_replace; the step planners never see a lane row.
//
// OS-MIDIREC S1 + S2a (docs/specs/OS-MIDIREC.md): the EVENTS list's pure model — the scope, the projection, the
// order, the ownership, LEN and the inputs of CC resolution (S1), and the edit planners (S2a, section 3.5, A6,
// A9, A10, A19-A21): each planner is a pure function that answers one edit with a refusal (its text) or at most
// four step writes, which the action then applies through Editor::set_step inside its dispatch unit. Phase 0
// only (Level B): nothing here writes a step itself, and nothing calls the planners yet (S2b wires the actions);
// the list is projected from the steps on each redraw, with no row table and no heap (A15).
//
// The rows (spec 3.2): a NOTE row for every step whose note column holds a note, a CC row for every FX column of
// type 8..12 or 37, and an OFF / CUT / FADE row for an end that ends no note above it (A4). Rows are ordered by
// step, then microstep, then kind (a CC row before a NOTE row at the same time), then track, then FX column 2
// before column 1 (A3: the sequencer runs the second column first, so the lower of two CC rows on one step is the
// value that wins on the wire).
//
// The FX ids and the micro-move formula are repeated here because the model cannot include seq/fx.h; they are the
// sequencer's values (F2, F6), and tests/event_list_test.cpp checks them against the facts.

#include <o3os/model/instrument.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/limits.h>
#include <o3os/model/pattern.h>
#include <o3os/model/project.h>
#include <o3os/model/project_state.h>
#include <o3os/model/step.h>
#include <o3os/model/step_edit.h>

#include <cstddef>
#include <cstdint>

namespace o3os::model::events {

// ---- the FX ids the projection reads (F2) ----
inline constexpr std::uint8_t kFxMicroMove = 2;  // seq::fx::kMicroMove: delays the note only (F6)
inline constexpr std::uint8_t kFxGate = 20;       // seq::fx::kGate: the gated length of a note (A5)
inline constexpr std::uint8_t kCcFirst = 8;       // CC A..E: seq::fx::kCcA..kCcE
inline constexpr std::uint8_t kCcLast = 12;
inline constexpr std::uint8_t kCcF = 37;          // CC F: seq::fx::kSlot37 (kSlot37IsProgramChange == false)

constexpr bool is_cc_type(std::uint8_t type) noexcept {
  return (type >= kCcFirst && type <= kCcLast) || type == kCcF;
}
// The CC slot of a type: 0..5, A..F.
constexpr std::uint8_t cc_letter_of(std::uint8_t type) noexcept {
  return type == kCcF ? 5u : static_cast<std::uint8_t>(type - kCcFirst);
}

// ---- the note column, as the sequencer reads it (F8) ----
// 1..128 a note, 253..255 an end; 0 and the reserved 129..252 hold nothing (inert where used).
constexpr bool note_is_note(std::uint8_t note) noexcept { return note >= kNoteFirst && note <= kNoteLast; }
constexpr bool note_is_end(std::uint8_t note) noexcept {
  return note == kNoteOff || note == kNoteCut || note == kNoteFade;
}
constexpr bool note_occupies(std::uint8_t note) noexcept { return note_is_note(note) || note_is_end(note); }

// seq/fx.h micromove_start (F6): start = (v + 1) * 48 / 101 + 1, held to 1..47; the note's offset into its step
// is start - 1, so 0..46 microsteps.
constexpr std::uint8_t micromove_start(std::uint8_t value) noexcept {
  const std::uint32_t v = value > 100u ? 100u : value;
  const std::uint32_t start = (v + 1u) * 48u / 101u + 1u;
  return static_cast<std::uint8_t>(start > 47u ? 47u : start);
}
// A1: the percent of a step an offset reads as, offset x 100 / 48 rounded (0..46 -> 0..96).
constexpr std::uint8_t percent_of(std::uint8_t offset) noexcept {
  const std::uint32_t p = (static_cast<std::uint32_t>(offset) * 100u + 24u) / 48u;
  return static_cast<std::uint8_t>(p > 96u ? 96u : p);
}

// A6's inverse, the recording rule: rec::micro_value (app/include/o3os/app/rec_place.h:76 — the model cannot
// include app) is repeated here over the same model::edit::map_int, so the recording writes and the list's TIME
// writes agree byte for byte. F7: micromove_start(micro_value(o)) - 1 == o for o in 0..46 (offset 47 cannot be
// reached); tests/event_list_test.cpp checks the round trip.
constexpr unsigned micro_value(int offset) noexcept {
  if (offset <= 0) {
    return 0;
  }
  const int v = edit::map_int(offset > 47 ? 47 : offset, 0, 47, 0, 100);
  return v > 100 ? 100u : static_cast<unsigned>(v);
}

// ---- kinds and the row key (3.2) ----
// The enum's order is the kind order of A3 at one time: a CC row first, then a NOTE row, then the ends.
enum class Kind : std::uint8_t { Cc = 0, Note = 1, Off = 2, Cut = 3, Fade = 4 };
constexpr bool is_end(Kind k) noexcept { return k >= Kind::Off; }

constexpr Kind end_kind_of(std::uint8_t note) noexcept {
  return note == kNoteOff ? Kind::Off : (note == kNoteCut ? Kind::Cut : Kind::Fade);
}

// The row's identity (3.2), kept by the cursor across edits, undo and re-projection.
struct Key {
  std::uint8_t track = 0;   // 0..15
  std::uint8_t step = 0;    // 0-based, 0..127
  Kind kind = Kind::Note;
  std::uint8_t column = 0;  // the FX column of a CC row: 0 = fx[0] ("FX 1"), 1 = fx[1] ("FX 2"); 0 for the other kinds
  // S6: 0 for a step row; a lane row: its place in the track's lane + 1 (the lane's order is stable between edits; a plan that
  // moves or inserts an event names the row's new key). A lane row's step is its tick / 48, its kind NOTE or CC.
  std::uint16_t lane = 0;
};
constexpr bool operator==(const Key& a, const Key& b) noexcept {
  return a.track == b.track && a.step == b.step && a.kind == b.kind && a.column == b.column && a.lane == b.lane;
}
constexpr bool operator!=(const Key& a, const Key& b) noexcept { return !(a == b); }

// ---- the scope (A2) ----
struct Scope {
  std::uint8_t track0 = 0, track1 = 0;  // inclusive
  std::uint8_t row0 = 0, row1 = 0;      // 0-based steps, inclusive; row1 is clamped to each track's length while projecting
  bool multi_track() const noexcept { return track0 != track1; }
};

// No block: the cursor's track, every step of its length.
Scope scope_track(const Pattern& pat, std::uint32_t track) noexcept;
// A block: its tracks and rows as whole steps. A column block is widened by the caller (the app's
// Selection::block()); the scope has no columns, the projection always reads whole steps.
Scope scope_block(std::int16_t row0, std::int16_t row1, std::int8_t track0, std::int8_t track1) noexcept;

// ---- the environment: the inputs of CC resolution (F3) ----
struct Env {
  const MidiState* midi = nullptr;  // the project's MIDI state; nullptr: every CC number unassigned, notes channel 0
  // The instrument table by slot (0..63), nullptr for a free slot. A function, because Project::instrument()
  // hides its array.
  const Instrument* (*instrument)(void* ctx, std::uint32_t slot) noexcept = nullptr;
  void* instrument_ctx = nullptr;
  const LaneStore* lanes = nullptr;  // S6: the take lanes of the project (the app's store); nullptr: no lane rows

  const Instrument* instrument_of(std::uint32_t slot) const noexcept {
    return instrument != nullptr ? instrument(instrument_ctx, slot) : nullptr;
  }
};
const Instrument* project_instrument(void* ctx, std::uint32_t slot) noexcept;
inline Env env_of(const Project& p) noexcept {
  Env e;
  e.midi = &p.midi();
  e.instrument = &project_instrument;
  e.instrument_ctx = const_cast<void*>(static_cast<const void*>(&p));
  return e;
}

// F3: what a CC row sends, and on which channel (1..16). `in` is the step's resolved instrument (nullptr: unknown,
// the top of a track). A MIDI instrument's slot names the target and its channel; the Config value (0, and any
// non-MIDI instrument) uses the project's number for the slot on the device's notes channel, and an unassigned
// number sends nothing.
struct CcOut {
  enum class What : std::uint8_t { Controller, Program, ChannelAt, PolyAt, Unassigned };  // Unassigned reads '--'
  What what = What::Unassigned;
  std::uint8_t number = 0;   // Controller: the controller 0..127
  std::uint8_t channel = 1;  // 1..16
};
CcOut cc_resolve(const Env& env, const Instrument* in, std::uint8_t letter) noexcept;

// ---- the lane event kinds (LaneEvent::kind_channel >> 4) ----
inline constexpr std::uint8_t kLaneNote = 0, kLaneCc = 1, kLaneProgram = 2, kLaneChanAt = 3, kLanePolyAt = 4, kLaneBend = 5;
inline constexpr std::uint16_t kLaneTicksPerStep = 48;
inline constexpr std::uint16_t kLaneBendMax = 16383;  // 14 bit: b << 7 | a

// ---- the row ----
struct Row {
  Key key;
  std::uint8_t microstep = 0;       // the row's offset into its step, 0..46 (a CC row: 0, F6)
  std::uint8_t percent = 0;         // percent_of(microstep), 0..96 (A1)
  std::uint8_t instr = 0;           // the resolved instrument, slot + 1; 0: unknown at the top of the track (A8, '?')
  bool inherited = false;           // the instrument comes from a step above (drawn dim, A8)
  std::uint8_t channel = 0;         // NOTE and end rows: the instrument's channel 1..16, 0: '--' (not MIDI, Q2 a); CC rows: the channel it goes out on
  bool midi = false;                // the resolved instrument is a MIDI one (a row of another kind dims, Q2 a)
  std::uint8_t vol = kVolumeUnset;  // NOTE rows: the volume column (A7); kVolumeUnset reads '---'
  std::uint8_t cc_letter = 0;       // CC rows: 0..5, A..F
  CcOut cc;                         // CC rows: what goes out and on which channel
  std::uint16_t len_steps = 0;      // NOTE rows (A5)
  std::uint8_t len_percent = 0;
  bool len_runs_on = false;         // '>': the note plays on past the track's end
  bool gated = false;               // 'G': a Gate FX on the note's step sets the length
  // S6, a lane row (key.lane != 0): the stored event. microstep is the tick's offset into its step, 0..47; percent is exact to a
  // tick (offset x 100 / 48); channel the stored one 1..16; len_* the true length (NOTE rows); vol the velocity.
  std::uint8_t lane_kind = 0;       // kLaneNote .. kLaneBend
  std::uint8_t lane_port = 0;
  std::uint8_t lane_a = 0;
  std::uint8_t lane_b = 0;
  std::uint16_t lane_len = 0;       // ticks (notes)
};

// ---- the filter (3.2) ----
enum class Filter : std::uint8_t { All = 0, Notes = 1, Cc = 2 };
constexpr bool filter_keeps(Filter f, Kind k) noexcept {
  if (f == Filter::All) {
    return true;
  }
  return f == Filter::Notes ? (k == Kind::Note || is_end(k)) : k == Kind::Cc;
}

// ---- the projector (A15) ----
inline constexpr std::size_t kMaxStepKeys = kMaxTracks * 3;  // 48: one note or end row and two CC rows per track and step
// One pass over the scope's steps in step order; the rows of one step (at most kMaxStepKeys) are gathered, sorted
// on the stack and emitted. No row table, no heap.
class Projector {
 public:
  Projector(const Pattern& pat, Env env, Scope scope, Filter filter) noexcept;

  bool next(Row& out) noexcept;               // the next row in A3 order; false at the end
  std::uint32_t index() const noexcept { return index_; }  // the rows returned so far
  // The cells read by the main pass: 3 per step of every scope track, each read once (at most 16 x 128 x 3 =
  // 6,144, A15). The end, fold and instrument scans peek at note and instrument columns of steps of the same
  // tracks.
  std::uint32_t cells() const noexcept { return cells_; }
  void reset() noexcept;

 private:
  bool gather() noexcept;  // the next step that has rows: fill keys_ and sort them
  void build(std::uint16_t packed, Row& out) const noexcept;
  // S6: the lane merge. lane_head: the track whose next listed lane event is the earliest (ties: the lower track), false when
  // none is left; events the filter drops and events past a track's length or the scope are passed for good. lane_build builds
  // the row of that head and passes it. step_head: the time of the next step row the filter keeps (false at the end).
  void lane_init() noexcept;
  bool lane_head(std::uint8_t& track, std::uint32_t& tick) noexcept;
  void lane_build(std::uint8_t track, Row& out) noexcept;
  bool step_head(std::uint32_t& time) noexcept;

  const Pattern* pat_;
  Env env_;
  Scope scope_;
  Filter filter_;
  std::uint8_t step_ = 0;  // the step keys_ holds
  std::uint8_t nkeys_ = 0;
  std::uint8_t emit_ = 0;
  std::uint16_t keys_[kMaxStepKeys];
  std::uint16_t lane_pos_[kMaxTracks];  // S6: the next event of each scope track's lane (main loop only, one reader)
  std::uint32_t cells_ = 0;
  std::uint32_t index_ = 0;
  bool started_ = false;
  bool done_ = false;
};

inline constexpr std::size_t kStackBound = 256;  // A15: the projection's main-loop stack, keys_ included
static_assert(sizeof(Projector) <= kStackBound, "the projector stays inside the 256 B stack bound (A15)");

// The rows under the filter, in A3 order.
std::uint32_t count_rows(const Pattern& pat, Env env, Scope scope, Filter filter) noexcept;
// The window the view draws: out[0..cap) gets the rows from index `from` on; returns how many were written and
// sets *total to the count under the filter.
std::uint32_t window(const Pattern& pat, Env env, Scope scope, Filter filter, std::uint32_t from, Row* out, std::uint32_t cap,
                     std::uint32_t* total) noexcept;
// The key of the row at `index`, and the index of a key (-1: none): the round trip of 3.2.
bool key_of_index(const Pattern& pat, Env env, Scope scope, Filter filter, std::uint32_t index, Key& out) noexcept;
std::int32_t index_of_key(const Pattern& pat, Env env, Scope scope, Filter filter, const Key& key) noexcept;

// ---- S2a: the edit planners (3.5, A6, A9, A10, A19-A21) ----
// One planner per field of a row, plus Insert and Delete, all through the one entry point plan() below: each
// answers one Request with a Plan — a refusal (text, no writes) or at most kMaxPlanWrites step writes. The
// action applies the writes through Editor::set_step inside its dispatch unit (3.5); the planner itself never
// writes. I1-I3 (A11) hold by construction — a plan only ever names the steps its rule writes — and
// tests/event_list_test.cpp property-tests them over random patterns and edits.

// The editable fields of a row (3.3, 3.4), in the Left / Right order. Fx1 and Fx2 (OS-EVFX, new Community OS behaviour):
// a NOTE row's two FX slots, the encoder edits the value.
enum class Field : std::uint8_t { Time = 0, Type, Ch, Data, Val, Len, In, Fx1, Fx2 };

enum class Op : std::uint8_t { Edit = 0, Insert, Delete, Quant };  // Quant (S3): one row's QUANT, the rule of quant_all below

// One edit, as the action asks it. `key` is the row (3.2); has_key false: the list is empty (Insert only, A9),
// and cursor_track is the grid cursor's track. `delta` is the encoder's detents (one per detent, signed); each
// planner scales it by its field's rule, coarse being Shift + encoder (3.4). absolute: a pad or an absolute
// entry, `value` holds it — DATA: the model note byte 1..128 (the app resolves the pad's note through the pad
// board, as the grid's entry does), VAL: 0..127. pad: a pad on VAL, `value` is the pad 0..47 and the planner
// ramps it 0..127 as the Vol pads do (step_vol_pad). shift: Shift + Insert (A9).
struct Request {
  Op op = Op::Edit;
  Field field = Field::Time;
  Key key{};
  bool has_key = true;
  std::uint8_t cursor_track = 0;
  int delta = 0;
  bool coarse = false;
  bool absolute = false;
  bool pad = false;
  bool shift = false;
  unsigned value = 0;
};

// One lane write of a plan (S6): the events of the ticks [t0, t1) of one track's lane become events[0..n), sorted and inside
// the range — Editor::lane_replace's arguments, which the action applies with the pattern's ID. A range holds at most
// kMaxLaneWriteEvents events; a longer one is refused (TOO MANY EVENTS HERE).
inline constexpr unsigned kMaxLaneWriteEvents = 32;
struct LaneWrite {
  bool on = false;
  std::uint8_t track = 0;
  std::uint8_t n = 0;
  std::uint16_t t0 = 0, t1 = 0;
  LaneEvent events[kMaxLaneWriteEvents]{};
};

// One step write of a plan: the action applies it through Editor::set_step (3.5).
struct StepWrite {
  std::uint8_t track = 0;
  std::uint8_t step = 0;
  Step value{};
};

inline constexpr unsigned kMaxPlanWrites = 4;  // 3.5: at most four step writes

// The plan of one edit. text[0] != 0 with n == 0: the refusal, nothing is written. text[0] != 0 with n > 0: the
// one-voice notice (A11, F8) — the writes happen and the strip says where the note above ends. new_key:
// Insert's new row, the one the cursor goes to (A9). last: the session's last-used after the edit.
struct Plan {
  static constexpr unsigned kTextMax = 40;
  char text[kTextMax] = {};
  bool ends_note = false;  // A11's one-voice notice is in text (the first one, when several apply)
  std::uint8_t n = 0;
  Key new_key{};
  bool has_new_key = false;
  edit::Last last{};
  StepWrite writes[kMaxPlanWrites]{};
  LaneWrite lane{};  // S6: a lane row's edit (then n == 0: no step is written); new_key names the row when it moved or was inserted
};

// Plan one edit against the pattern as it is. Pure: no globals, no heap, no step written. The scope is the
// list's (A2): TIME edits and Insert stay inside it.
Plan plan(const Pattern& pat, Env env, Scope scope, const edit::Last& last, const Request& req) noexcept;

// ---- S3: QUANT (spec S3, QA-PAT-426..428) ----
// The rule on a NOTE or an end row: an offset under 24 microsteps snaps back to .00 (the Micro-move goes); 24 or more moves
// the start to the next step at .00, by the TIME move rule (plan_time: A21, so the note takes its LEN and owned bytes with it,
// and its refusals are the move's). A CC row says CC IS ON THE STEP and a row already on the step says ALREADY ON THE STEP,
// both refusals with nothing written (Op::Quant through plan()). Starts only: an owned end is not a row, it moves with its note.
inline constexpr std::uint8_t kQuantHalf = 24;  // the snap boundary: offsets 0..23 go back, 24..46 go forward

// Why a quantise left rows alone (the first refusal's reason): the target step holds a note, the move needs an FX column the
// target has not got, it leaves the scope, or the planner refused the row for another reason.
enum class QuantWhy : std::uint8_t { None = 0, StepTaken, NoFxColumn, Outside, Other };

struct QuantResult {
  std::uint32_t done = 0;  // rows quantised (an offset snapped back or moved on)
  std::uint32_t left = 0;  // rows the rule refused
  QuantWhy why = QuantWhy::None;
};

// What a quantise of every NOTE and end row the filter shows does, walking the steps from the last back, so a row that moves
// on frees its step for the row before it in the same pass. `apply` writes a plan's steps into the pattern `pat` reads (it must
// write them: each row is planned against the pattern as the rows before it left it); it returns false when it could not, and
// the walk stops there. The CC rows and the rows already on a step are not counted. The caller wraps the call in one undo unit
// (the dispatch), which is I3.
using QuantApply = bool (*)(void* ctx, const Plan& plan) noexcept;
QuantResult quant_all(const Pattern& pat, Env env, Scope scope, Filter filter, const edit::Last& last, QuantApply apply, void* ctx) noexcept;

// The strip text: QUANTISED 12, 2 LEFT (STEP TAKEN) when rows were left, QUANTISED 12 when not, NOTHING TO QUANTISE when
// no row had an offset. `out` holds at least 48 chars (core::kNotifyTextChars).
void quant_message(const QuantResult& r, char* out, std::size_t cap) noexcept;

}  // namespace o3os::model::events
