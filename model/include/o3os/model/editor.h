// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/core/action.h>
#include <o3os/core/learn.h>
#include <o3os/core/param.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/lane_take.h>
#include <o3os/model/project.h>
#include <o3os/model/undo.h>

#include <cstddef>
#include <cstdint>

namespace o3os::model {

// The Editor is the undoable face of a Project (ADR 0009 mechanism 6, OS-002c): the same mutators, with the same checks and
// the same Status, that also write what they changed into the UndoJournal. Undoable actions edit through it and nothing else
// does (a rejected edit records nothing); undo() and redo() replay the journal through the Project's own mutators, so the
// generations bump and the screens redraw exactly as for a first edit.
//
// Units. An undo unit is what one undoable action did: begin_group() / end_group() bracket it (they nest: the outermost one
// names the unit, so a macro playing ten actions inside one group is one undo step). A mutator called outside any group is a
// unit of its own. A unit that recorded nothing (every edit rejected, or only no-ops) leaves no trace and does not end the
// redo history.
//
// Exactness. undo() brings back everything the group changed, by value: the Project reads the same through every accessor
// (the generations excepted, which only ever advance; and which pool slot a pattern lives in, which nothing can observe) as
// before the unit. The test of record is a property test against full snapshots over random edit, undo and redo sequences.
// Edits that change nothing (a step set to what it was) are applied but not recorded.
//
// What is journalled: the header, the tracks and the master, the song, the patterns (create, delete, clear, name, lengths,
// steps), the instruments, single parameters (set_param: the id with the new and the old value, 40 bytes of journal) and the
// project's MIDI Learn bindings (set_learn_slot: slot, new and old binding, 48 bytes). Modules and unknown chunks are load-time
// plumbing, not edits: nothing records them. The device's settings are not the project's and not here.
//
// A Project that changes behind the Editor's back (a file load, reset(), a raw mutator) is noticed: the journal remembers
// the Project's generation after the last thing it did, and a difference clears the history (undo() answers Stale), because
// older units can no longer be applied to what is there. After a deliberate load, call history_reset().
//
// Areas (OS-UNDOGRP). The history is three rings, one per area (undo.h UndoHistory): PATTERN (the Pattern screen), SONG (the Song
// screen) and SOUND (Instrument, Wave, the parameters and MIDI CC). A unit goes to the ring of the area it was begun in (begin_group's
// `area`); an Auto unit goes where its first record belongs (steps and patterns: PATTERN; order, tempo, swing, title: SONG;
// instruments, parameters, Learn: SOUND). undo(area) and redo(area) touch that ring only; an edit discards the redo units of its own ring
// only. A unit that creates or deletes a pattern, or fills or clears an instrument slot, is *structural*: the rings that name
// such an object (a pattern: PATTERN and SONG; an instrument slot: SOUND) must not hold records of it that the unit has made untrue. Written to one ring,
// it drops the redo units of the others (a deletion or a clear: all their units); undone or redone, it clears the others' whole history, the redo too
// (fence_cleared() says which). The Mixer has no history: set_final() makes the edits of one action apply and record nothing (the `mixer.*` actions, a
// track's name, a MIDI CC on a track's or the master's parameter), and a unit begun with UndoArea::None does the same.
//
// Click groups. merge_hint(key) before a parameter edit lets it join the unit committed last when that unit had the same key and
// nothing was written since (undo.h): the sweep of an encoder is one unit. merge_break() ends the group (a screen change, a pause).
//
// Capacity. Records are what the edit changed, not copies of the project; the sizes (journal bytes, header and trailer
// included): a step edit 48, a song edit 32, clearing a 255-position song 284, an instrument 1,472 (1,464 before OS-STEREO step 2, 1,440 before OS-INSTUI S8b), a pattern clear or
// delete 14,368 and 14,412 (UndoJournal::unit_bytes_for). The rings hold (160 / 96 / 128 KiB): PATTERN 3,413 step edits or 11 whole-pattern
// deletes, SONG 3,072 song edits or 6 full pattern copies, SOUND 2,457 parameter edits or 89 instrument edits. The largest single step edit is a paste or a pull over 16 tracks (the steps of each track, old and new: about 29 KB), so at least 5 levels of even that are kept in PATTERN.
//
// Contexts: main loop only. One Editor per Project and journal.

enum class UndoResult : std::uint8_t {
  Ok,       // one unit undone or redone
  Nothing,  // nothing to undo or redo
  Busy,     // a group is open: undo and redo are not allowed in the middle of one
  Stale,    // the project changed behind the Editor's back: the history was cleared and nothing was done
  Failed,   // the journal or the project did not permit it (a bug: the history was cleared)
};

// OS-PATTEMPO: the click-group key of the project tempo (merge_hint). Kind None: no parameter has it.
inline constexpr core::ParamId kTempoMergeKey = core::param_id(core::ParamKind::None, 0, 1);

class Editor {
 public:
  Editor(Project& project, UndoHistory& history) noexcept;
  // One ring for every area (the old shape of the journal: the tests that need no areas, a device with too little memory to split).
  Editor(Project& project, UndoJournal& journal) noexcept;
  Editor(const Editor&) = delete;
  Editor& operator=(const Editor&) = delete;

  const Project& project() const noexcept { return project_; }
  // The ring of an area; the Pattern ring for Auto and None, and the one ring of the single-ring shape for every area.
  const UndoJournal& journal(UndoArea area = UndoArea::Pattern) const noexcept { return *rings_[ring_index(area)]; }

  // ---- groups ----
  // Starts an undo unit, named by `action` and `arg` (what the user did: the unit carries them so a macro can replay it).
  // Groups nest; only the outermost names the unit and chooses its area (see "Areas" above).
  void begin_group(core::ActionId action, core::ActionArg arg, UndoArea area = UndoArea::Auto) noexcept;
  void end_group() noexcept;
  unsigned group_depth() const noexcept { return depth_; }
  // The next parameter edit of the open group may join the unit committed last when that had this key (a ParamId). Cleared when the group ends. Only the
  // outermost group asks: a unit made inside a macro or a take is that macro's or take's, and no later click joins it.
  void merge_hint(core::ParamId key) noexcept { hint_ = depth_ == 1 ? static_cast<std::uint32_t>(key) : 0u; }
  // Ends every click group: the next edit starts a unit of its own.
  void merge_break() noexcept;
  // While on, every edit applies and records nothing: what the Mixer owns (an action `mixer.*`, a track's name, a MIDI CC on a track's or the master's parameter) has no history
  // (D10). Per action, not per screen: the caller turns it on around that action's handler and restores what it was.
  void set_final(bool on) noexcept { final_ = on; }
  bool final_edits() const noexcept { return final_; }

  // ---- history ----
  UndoResult undo(UndoArea area = UndoArea::Pattern) noexcept;
  UndoResult redo(UndoArea area = UndoArea::Pattern) noexcept;
  // A history that no longer matches the project (it changed behind the editor's back: a load, an import) cannot be applied:
  // can_undo() and can_redo() say false until history_reset(); the next undo() / redo() says Stale and clears every ring.
  bool in_sync() const noexcept { return project_.generation() == synced_; }
  bool can_undo(UndoArea area = UndoArea::Pattern) const noexcept { return journal(area).depth() != 0 && in_sync(); }
  bool can_redo(UndoArea area = UndoArea::Pattern) const noexcept { return journal(area).redo_depth() != 0 && in_sync(); }
  // True if any area holds a unit to undo or redo.
  bool has_history() const noexcept;
  // The area whose history the last undo() / redo() cleared because the unit it undid or redid was structural (Auto: none).
  UndoArea fence_cleared() const noexcept { return fenced_; }
  // Whether it cleared the history of this area (the unit touched a pattern, or an instrument slot, that records of several areas name).
  bool fence_cleared(UndoArea area) const noexcept { return (fence_mask_ >> ring_index(area)) & 1u; }
  // Forgets the history of every area and takes the Project as it is: call it after a load or any change made without the Editor.
  void history_reset() noexcept;
  // A sample load put an instrument into `slot` through the Project, not through the Editor (OS-BUG-SLDOVER: a load is no undo unit, and it must not end the undo of everything else).
  // The caller says that `slot` is the only thing that changed since the Editor was last in sync. The history stays, but for the SOUND ring when one of its records names the slot (a
  // set, a clear or a parameter of that instrument: undone, it would write the old instrument over the new): the ring holds parameters of every instrument, in one byte ring that cannot
  // drop a unit from its middle, so it ends whole, and only it (the PATTERN and SONG rings never name an instrument slot). True when that ring had history and lost it.
  // With a group open, or an Editor already out of sync, it is history_reset() (and true if there was history).
  bool adopt_instrument_load(std::uint32_t slot) noexcept;
  // How many times the Project was found changed behind the Editor's back (and the history cleared for it).
  std::uint32_t stale_resets() const noexcept { return stale_; }
  // Units that undo() or redo() could not finish (a bug, never input): also counted by UndoJournal::damaged().
  std::uint32_t failures() const noexcept { return failures_; }

  // ---- mutators: the Project's, same checks, same Status ----
  Status set_name(const char* text) noexcept;
  Status set_name(const char* text, std::size_t len) noexcept;
  Status set_tempo_x100(std::uint32_t value) noexcept;
  Status set_swing(std::uint32_t percent) noexcept;
  Status set_default_pattern_length(std::uint32_t steps) noexcept;
  Status set_active_tracks(std::uint32_t count) noexcept;
  // The count as project STATE (OS-FU5): not an undo unit, and an in-sync history stays in sync. For the step after a load that normalises what the file said (App::normalise_loaded_state).
  Status set_active_tracks_state(std::uint32_t count) noexcept;
  Status set_master(const MasterSettings& value) noexcept;
  Status set_track(std::uint32_t slot, const TrackSlot& value) noexcept;
  // OS-SONG3 S4 (PREVIEW): the track's flags written plain (Project::set_track_flags_plain): no undo record, and the generation does not move, so the Editor never reads the change
  // as one behind its back (check_sync would reset the history) and the save layer does not call it unsaved work.
  void set_track_flags_plain(std::uint32_t slot, std::uint8_t flags) noexcept { project_.set_track_flags_plain(slot, flags); }
  Status set_track_name(std::uint32_t slot, const char* text, std::size_t len) noexcept;

  Status append_order(std::uint32_t pattern_id) noexcept;
  Status insert_order(std::uint32_t pos, std::uint32_t pattern_id) noexcept;
  Status set_order(std::uint32_t pos, std::uint32_t pattern_id) noexcept;
  Status remove_order(std::uint32_t pos) noexcept;
  Status clear_order() noexcept;
  // The slot at `from` goes to index `to` (E-SONG-07: one change of the list, so a playing song follows the slot). The undo records are the ones an insert and a remove make (a list of 255 slots, which
  // cannot take the insert: one set_order each for the slots that change); undo and redo apply the insert/remove pair as one move again (Editor::replay_move).
  Status move_order(std::uint32_t from, std::uint32_t to) noexcept;
  // E-SONG-07: edits of the order list made while one of these lives are one change for a playing song (a range delete or paste): see Project::OrderEdit.
  class OrderBatch {
   public:
    explicit OrderBatch(Editor& e) noexcept : edit_(e.project_) {}

   private:
    Project::OrderEdit edit_;
  };

  Status create_pattern(std::uint32_t id, std::uint32_t length) noexcept;
  Status delete_pattern(std::uint32_t id) noexcept;
  Status set_pattern_name(std::uint32_t id, const char* text, std::size_t len) noexcept;
  Status set_track_length(std::uint32_t id, std::uint32_t track, std::uint32_t length) noexcept;
  Status set_pattern_length(std::uint32_t id, std::uint32_t length) noexcept;
  // OS-MIDIREC-P1 S2 fold: a dry run for a caller that changes the steps AND several track lengths in one unit (pattern ops, the whole-pattern paste): false when
  // the lane events its shortenings cut (`lengths[t]`, 16 entries) plus the step writes of the whole pattern would not fit the undo ring, so the caller writes
  // nothing. Conservative (it counts a full steps image); true when no lane is cut (a pattern without lanes keeps the old rule). Changes nothing.
  bool lengths_fit_undo(std::uint32_t id, const std::uint8_t* lengths) noexcept;
  Status clear_pattern(std::uint32_t id) noexcept;
  Status set_step(std::uint32_t id, std::uint32_t track, std::uint32_t step, const Step& value) noexcept;
  Status set_steps(std::uint32_t id, std::uint32_t track, std::uint32_t first, const Step* values,
                   std::uint32_t count) noexcept;

  // OS-MIDIREC-P1 S1: the MIDI take lanes (lane_store.h). The store is the app's, outside the Project; without one attached (the default) the lane
  // calls answer NotFound and delete / clear / length changes touch no lane. A lane edit is journalled in the Pattern ring as kLaneDelete and kLaneInsert
  // records (the events moved, so one record never exceeds one lane's 32 KiB) and advances the pattern's generation (Project::touch_lanes), so undo and
  // redo and the native Writer's Modified check see it. A17: shortening a track (set_track_length, set_pattern_length) deletes the lane events at or
  // past the new length in the same unit; lengthening does not; delete_pattern and clear_pattern free the pattern's lanes (their records come before the
  // pattern's own, so undo brings the pattern back first). A lane record holds the moved events once (1x the lane data, maintainer 2026-11-10: a replace
  // keeps only the events that changed, the equal head and tail stay in place), so before any
  // erase, cut, free or replace the Editor checks that the unit's lane records (plus the pattern's own record) fit the ring: when they do not the call
  // answers Full and changes nothing (state and history intact). That is delete_pattern, clear_pattern, set_track_length, set_pattern_length and
  // lane_replace / lane_clear, and only when lanes are touched (a pattern without lanes keeps the old rule: an oversize unit clears the ring and the edit
  // stays done); only a unit begun with UndoArea::None, or a final or replayed one, is not checked (nothing is recorded).
  void attach_lanes(LaneStore* lanes) noexcept { lanes_ = lanes; }
  const LaneStore* lanes() const noexcept { return lanes_; }
  // Replaces the events of the lane of (pattern, track) whose tick is in [t0, t1) with `n` events (sorted, valid, ticks inside [t0, t1)). A take commit is
  // one call: t0 and t1 are its range. NotFound: no pattern or no store; BadIndex: track; BadValue: t0 >= t1, t1 above 6144, n above 4096 or events
  // that are invalid, unsorted or outside the range, or t1 above the track's length x 48 ticks (A17: no event past the end of its track); Full: the lane
  // would pass 4096 events, the pool 7,680, there would be a 2049th lane, or the unit does not fit the undo ring (nothing changes). The same events as
  // there are: applied, not recorded. lane_clear empties the whole lane whatever the track's length.
  Status lane_replace(std::uint32_t pattern, std::uint32_t track, std::uint32_t t0, std::uint32_t t1, const LaneEvent* events,
                      std::uint32_t n) noexcept;
  Status lane_clear(std::uint32_t pattern, std::uint32_t track) noexcept;
  // OS-MIDIREC-P1 S7a, A16: a pattern copy carries its lanes. lane_copy puts the events of the lane of (from, track) below the track's end into the EMPTY lane of
  // (to, track) as one insert record (Ok with nothing to copy; NotFound: no store or pattern; BadIndex: track; BadValue: from == to or the target lane has events;
  // Full: the pool, the lane count or the undo ring would not take it, nothing changes). lanes_copy_fit is its dry run for a caller that copies the lanes of the
  // tracks in the mask `tracks` after other edits of the same unit that take `other_bytes` of ring: true when no lane is involved (the old rule), else whether
  // the pool, the lane count and the ring (the open unit's bytes included) take them all. Changes nothing.
  Status lane_copy(std::uint32_t from, std::uint32_t to, std::uint32_t track) noexcept;
  bool lanes_copy_fit(std::uint32_t from, std::uint16_t tracks, std::size_t other_bytes) noexcept;
  // OS-MIDIREC-P1 S3, A10 / A11 / A15: merges a finished take (lane_take.h) into the lane of its (pattern, track) in one pass and one pair of undo records
  // (the changed span before, after). Overdub: the lane's events stay and the take's are merged by tick (a take event goes after the lane's events at its
  // tick); a CC controller the take moved REPLACES the lane's old points of that controller (the take's channel and port) inside the take's intervals.
  // The merge is done in place in the pool (no scratch). Ok; NotFound: no store or pattern; BadIndex: track; BadValue: the take has an event that is
  // invalid, unsorted or past the end of the track; Full: the lane would pass 4096 events, the pool 7,680, there would be a 2049th lane, or the unit does
  // not fit the undo ring (nothing changes: the take stays valid for one more try).
  Status lane_merge_take(const LaneTake& take) noexcept;

  Status set_instrument(std::uint32_t index, const Instrument& value) noexcept;
  Status clear_instrument(std::uint32_t index) noexcept;
  // OS-INSTUI S7, NAME: the instrument's name (the stored field: up to kInstrumentNameMax characters, any but NUL) as one undo record (the whole instrument, as set_instrument: the unit
  // restores the old name). NotFound for an empty slot, BadValue for a name that is too long or holds a NUL; the same name records nothing.
  Status set_instrument_name(std::uint32_t index, const char* text, std::size_t len) noexcept;
  // OS-PARAMAUTO: one byte of Instrument::midi_cc (the letter map of a Sample or Module instrument, param_auto.h) as a 4-byte record (32 bytes of journal): undo restores that byte only, so a
  // take that gave a parameter its letter takes the letter back without touching the instrument's other parameters. BadIndex (slot or letter), NotFound (empty slot); the same code records nothing.
  Status set_instrument_cc(std::uint32_t index, unsigned letter, std::uint8_t code) noexcept;
  // OS-012b: a module reference joins the project's table (the engine picker of the Instrument page names a module the project did not yet use). Load-time plumbing
  // like the rest of the table: not journalled, and it does not end the history (OS-012b decision: the table only ever grows, so every older unit still applies).
  // A history that was already stale stays so.
  Status add_module(const char* name, std::size_t len, std::uint32_t abi_major, std::uint32_t abi_minor) noexcept;

  // OS-004d: the project-level state that the runtime owns (project_state.h): the app, the platform and the sync that copies the runtime into the project call these. Like add_module
  // they are load-time plumbing, not journalled, and they do not end the history (nothing a record holds changes); a history that was already stale stays so. Same checks and Status
  // as the Project's.
  Status set_midi_state(const MidiState& value) noexcept;
  Status set_usb_state(const UsbState& value) noexcept;
  Status set_mix_state(const MixState& value) noexcept;
  Status set_perf_state(const PerfState& value) noexcept;
  Status set_note_state(const NoteState& value) noexcept;      // OS-PERSIST2: the session state, the same way
  Status set_mute_state(const MuteState& value) noexcept;
  Status set_scenes_state(const ScenesState& value) noexcept;
  Status set_send_fx_state(const SendFxState& value) noexcept;
  Status set_app_params(const char* name, std::size_t len, std::uint32_t abi_major, std::uint32_t abi_minor, const float* values, std::uint32_t count) noexcept;
  // OS-INSFX S3b: the insert records (Project::set_insert, clear_insert: same checks and Status), written the same way. NOT undoable (D9: an insert is the engine's state, not an edit): no record,
  // and an in-sync history stays in sync when the project's generation moves.
  Status set_insert(const InsertRecord& value) noexcept;
  Status clear_insert(std::uint32_t where, std::uint32_t slot) noexcept;

  // Parameters (OS-002d): one value, or one slot of the project's Learn store; same checks and Status as the Project's.
  Status set_param(core::ParamId id, float value) noexcept;
  Status set_learn_slot(unsigned slot, const core::LearnBinding& b) noexcept;

  // What the last undo() or redo() set by value, in order: the parameters (with the value they now have) that the app sends to the
  // audio side as ParamChange. The records that replace a whole instrument, track or master are not listed, and neither are more than
  // kAppliedMax: applied_incomplete() says the list is short of what changed (the app then has the audio side re-read the
  // project). Cleared by every undo() and redo().
  struct AppliedParam {
    core::ParamId id;
    float value;
  };
  static constexpr unsigned kAppliedMax = 16;
  unsigned applied_count() const noexcept { return applied_n_; }
  const AppliedParam& applied(unsigned i) const noexcept { return applied_[i < kAppliedMax ? i : 0u]; }
  bool applied_incomplete() const noexcept { return applied_incomplete_; }

 private:
  struct Piece {
    const void* data;
    std::size_t size;
  };
  class Scope;
  friend class Scope;

  void check_sync() noexcept;
  // Lane edits that record themselves (the caller checked everything): erase [index, index + count) of the lane / put `count` events in at `index`.
  void lane_erase(std::uint32_t pattern, std::uint32_t track, std::uint32_t index, std::uint32_t count) noexcept;
  bool lane_insert(std::uint32_t pattern, std::uint32_t track, std::uint32_t index, const LaneEvent* events, std::uint32_t count) noexcept;
  void lane_cut(std::uint32_t pattern, std::uint32_t track, std::uint32_t from_tick) noexcept;  // A17: the events at or past the tick
  std::uint32_t lane_copy_count(std::uint32_t from, std::uint32_t track) noexcept;                // the events lane_copy takes from the source lane
  void lanes_free(std::uint32_t pattern) noexcept;                                               // every lane of the pattern
  Status lane_replace_in(std::uint32_t pattern, std::uint32_t track, std::uint32_t t0, std::uint32_t t1, const LaneEvent* events, std::uint32_t n,
                         bool inside_track) noexcept;
  // The undo-ring bytes the records of such an edit take (0: no lane event is involved), and whether `record_bytes` more fit the ring that a unit starting with `op` writes to.
  std::size_t lane_cut_bytes(std::uint32_t pattern, std::uint32_t track, std::uint32_t from_tick) noexcept;
  std::size_t lanes_free_bytes(std::uint32_t pattern) noexcept;
  bool undo_fits(std::uint8_t op, std::size_t record_bytes) const noexcept;
  static std::size_t lane_record_bytes(std::uint32_t n) noexcept;  // 8n + 16 (0 for n = 0)
  void synced() noexcept { synced_ = project_.generation(); }
  static unsigned ring_index(UndoArea area) noexcept { return UndoHistory::index(area); }
  // The ring the open unit writes to; the first record of an Auto unit chooses it by `op`. nullptr: not journalled (UndoArea::None).
  UndoJournal* writer(std::uint8_t op) noexcept;
  void close_unit() noexcept;
  // Writes one record of the pieces in order (after the edit succeeded). False: not recorded (see UndoJournal). `structural`: the record creates or
  // deletes a pattern, or fills or clears an instrument slot (the owner area's redo units go if it is written to another ring).
  bool record(std::uint8_t op, const Piece* pieces, std::size_t count, bool structural = false) noexcept;
  bool merge_param(UndoJournal& ring, core::ParamId id, float now) noexcept;
  bool merge_tempo(UndoJournal& ring, std::uint32_t now) noexcept;
  void fence_write(UndoJournal& ring, std::uint8_t op) noexcept;
  std::uint8_t fence_others(const UndoJournal& keep, bool pattern) noexcept;
  Status text_edit(std::uint8_t op, std::uint8_t id, bool has_id, const char* old_text, std::size_t old_len,
                   const char* text, std::size_t len, Status status, bool changed) noexcept;
  bool replay(std::uint8_t op, std::size_t payload, std::size_t len, bool undo) noexcept;
  // E-SONG-07: the insert and the remove of one slot that move_order recorded, applied as the one Project::move_order they came from (so a playing song follows the slot). `first` is the
  // record that is applied first (the insert; the remove's record when undoing). False: they are not such a pair, nothing was applied.
  bool replay_move(const UndoJournal::RecRef& first, const UndoJournal::RecRef& second) noexcept;
  UndoResult walk(bool undo, UndoArea area) noexcept;
  void note_applied(core::ParamId id, float value) noexcept;

  Project& project_;
  LaneStore* lanes_ = nullptr;  // OS-MIDIREC-P1: not owned
  UndoJournal* rings_[kUndoAreas];  // by UndoHistory::index; the same ring three times in the single-ring shape
  UndoJournal* walking_ = nullptr;  // the ring undo() / redo() reads from
  UndoJournal* open_ = nullptr;     // the ring the open unit writes to (chosen at its first record when the area is Auto)
  bool muted_ = false;              // the open unit is not journalled (UndoArea::None)
  bool final_ = false;              // nothing is recorded (set_final)
  UndoArea area_ = UndoArea::Auto;
  core::ActionId pending_action_ = core::ActionId::None;
  core::ActionArg pending_arg_ = 0;
  std::uint32_t hint_ = 0;
  UndoArea fenced_ = UndoArea::Auto;
  std::uint8_t fence_mask_ = 0;
  bool fence_pattern_ = false;
  bool fence_sound_ = false;
  unsigned depth_ = 0;
  std::uint32_t synced_ = 0;  // the Project's generation after the last thing the Editor did
  std::uint32_t stale_ = 0;
  std::uint32_t failures_ = 0;
  AppliedParam applied_[kAppliedMax] = {};
  unsigned applied_n_ = 0;
  bool applied_incomplete_ = false;
};

}  // namespace o3os::model
