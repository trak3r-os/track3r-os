// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/core/learn.h>
#include <o3os/core/param.h>
#include <o3os/model/instrument.h>
#include <o3os/model/limits.h>
#include <o3os/model/params.h>
#include <o3os/model/pattern.h>
#include <o3os/model/project_state.h>
#include <o3os/model/settings.h>
#include <o3os/model/status.h>
#include <o3os/model/step.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace o3os::model {

// The project model (OS-004a, ADR 0003/0004/0007/0009): everything a project holds, with no knowledge of any
// screen, file or device. No heap: all of it lives in one fixed-size ModelArena that the platform provides
// (SDRAM on the MK66, a static in the host tests). Reads are free (const accessors, nothing is copied).
// Writes go through the mutators below, the single choke point that checks the value, changes it and bumps
// the generation counters. Undoable edits go through the Editor (editor.h), which calls these same mutators and journals
// what they changed (OS-002c). A rejected mutation changes nothing and bumps nothing.
//
// Contexts: not thread-safe. The main loop owns the arena; the audio side gets copies or messages.

// A chunk id is four printable ASCII characters (0x20..0x7E), first one in the top byte.
constexpr bool is_printable_fourcc(std::uint32_t id) noexcept {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    const std::uint32_t c = (id >> shift) & 0xFFu;
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

enum class TrackType : std::uint8_t { Audio = 0, Midi = 1, ExternalAudio = 2 };

inline constexpr std::uint8_t kTrackMute = 1;
inline constexpr std::uint8_t kTrackSolo = 2;
inline constexpr std::uint8_t kTrackFlagsMask = kTrackMute | kTrackSolo;

struct TrackSlot {
  TrackType type;
  std::uint8_t flags;   // kTrackMute | kTrackSolo
  std::uint8_t volume;  // 0..100
  std::uint8_t pan;     // 0..100, 50 centre
  std::uint8_t bus;     // 0 master mix, 1..kMaxOutputBus stem buses
  char name[kTrackNameMax + 1];
};

inline constexpr std::uint8_t kMasterDryMute = 1;
inline constexpr std::uint8_t kMasterReverbMute = 2;
inline constexpr std::uint8_t kMasterDelayMute = 4;
inline constexpr std::uint8_t kMasterLimiter = 8;
inline constexpr std::uint8_t kMasterFlagsMask = 15;

// Master section levels. Engine FX parameters (reverb, delay, limiter settings) are not stored yet: they
// arrive with the audio engine as new chunks.
struct MasterSettings {
  std::uint8_t volume;         // 0..100
  std::uint8_t dry_volume;     // 0..100
  std::uint8_t reverb_volume;  // 0..100
  std::uint8_t delay_volume;   // 0..100
  std::uint8_t flags;          // kMaster* bits
};

// A module an instrument may name (ADR 0002): the file stem in /modules and the ABI version the project was
// saved with. Opening the project loads these; a missing one mutes its instruments.
struct ModuleRef {
  char name[kModuleNameMax + 1];
  std::uint8_t abi_major;
  std::uint8_t abi_minor;
};

// A chunk of a newer or foreign file that this version does not know, kept so a save re-emits it.
struct UnknownChunk {
  std::uint32_t id;      // four ASCII characters, first one in the top byte
  std::uint32_t length;  // payload bytes (no header, no pad)
  std::uint32_t offset;  // into the pool
};

// The generation counters of the parameters stored in the Project itself (the settings block keeps its own): one for every parameter
// of every instrument slot, track slot and the master section (OS-002d).
inline constexpr unsigned kProjectParamSlots = static_cast<unsigned>(kMaxInstruments) * instrument_param::kCount +
                                              static_cast<unsigned>(kMaxTracks) * track_param::kCount + master_param::kCount;

// E-SONG-07: what the render needs to know about edits of the song's order list, so a playing song keeps its place. The list is written by the main loop; every change is one
// bracket (Project::OrderEdit: an odd epoch while it runs) that also appends one op to a journal of the last kOrderJournal ops. The render keeps a copy of the list (OrderSnapshot) and,
// when the epoch moved, replays the new ops over its own song positions. Not saved: the journal is runtime state and no file changes.
struct alignas(4) OrderOp {  // (one word: the journal is written and copied as relaxed words)
  enum Kind : std::uint8_t { Insert = 1, Remove = 2, Move = 3, Reset = 4 };
  std::uint8_t kind;
  std::uint8_t a;  // Insert, Remove: the first position; Move: from
  std::uint8_t b;  // Move: to (the index the slot has afterwards)
  std::uint8_t n;  // Insert, Remove: how many slots
};
inline constexpr std::uint32_t kOrderJournal = 16;  // a power of two

struct OrderSnapshot {
  std::uint32_t epoch;  // even
  std::uint32_t ops;    // ops since boot at the snapshot (wraps)
  std::uint8_t len;
  std::uint8_t list[kMaxSongPositions];
  OrderOp journal[kOrderJournal];  // op number k is journal[k % kOrderJournal]; the last kOrderJournal are there
};

class Project {
  // OS-FLASHDIET I1/I2: model/import_layout.h hashes this layout into the importer's layout tag (a kind 3 .tmod's header word 44); the probe is a friend so it can read the members' offsets.
  friend constexpr std::uint32_t model_import_layout_probe(std::uint32_t h) noexcept;

 public:
  // Deliberately trivial, so a static ModelArena in zero-initialised or uninitialised SDRAM is legal: call
  // reset() before use.
  Project() = default;

  // Back to the empty default project; keeps counting generations (every counter advances).
  // `keep_instruments` (OS-NEWKEEP, File > NEW > KEEP INSTRUMENTS): the instrument table and the module table stay byte for byte; everything else is reset.
  void reset(bool keep_instruments = false) noexcept;
  // OS-DEFSCALE: what a NEW document (not a loaded one) has beyond reset(): the NOTE part present, at C major. The new-project path and the platform's boot document call it after reset().
  void start_new() noexcept;

  // ---- reads --------------------------------------------------------------------------------------------

  const char* name() const noexcept { return name_; }
  std::uint32_t tempo_x100() const noexcept { return tempo_x100_; }
  std::uint8_t swing() const noexcept { return swing_; }
  std::uint8_t default_pattern_length() const noexcept { return default_pattern_length_; }
  std::uint8_t active_tracks() const noexcept { return active_tracks_; }
  const MasterSettings& master() const noexcept { return master_; }
  // nullptr when slot >= kMaxTracks
  const TrackSlot* track(std::uint32_t slot) const noexcept { return slot < kMaxTracks ? &tracks_[slot] : nullptr; }

  std::uint32_t song_length() const noexcept { return song_length_; }
  // Pattern id at the position, 0 when pos >= song_length()
  std::uint8_t order(std::uint32_t pos) const noexcept { return pos < song_length_ ? song_[pos] : std::uint8_t{0}; }

  std::uint32_t pattern_count() const noexcept { return pattern_count_; }
  // nullptr when no pattern has that id (a song may name an id that does not exist yet: it plays as empty)
  const Pattern* pattern(std::uint32_t id) const noexcept;
  // The length of the pattern as a whole: the longest of the first active_tracks() tracks (the tracks the
  // user sees and plays); 0 when there is no such pattern. Tracks keep their own lengths (Pattern::track_length).
  std::uint32_t pattern_length(std::uint32_t id) const noexcept;

  // nullptr when the slot is free or index >= kMaxInstruments
  const Instrument* instrument(std::uint32_t index) const noexcept;
  std::uint32_t instrument_count() const noexcept { return instrument_count_; }

  std::uint32_t module_count() const noexcept { return module_count_; }
  const ModuleRef* module(std::uint32_t index) const noexcept {
    return index < module_count_ ? &modules_[index] : nullptr;
  }

  std::uint32_t unknown_count() const noexcept { return unknown_count_; }
  const UnknownChunk* unknown(std::uint32_t index) const noexcept {
    return index < unknown_count_ ? &unknown_[index] : nullptr;
  }
  // The payload of unknown chunk `index` (length bytes); nullptr when there is none.
  const std::uint8_t* unknown_data(std::uint32_t index) const noexcept {
    return index < unknown_count_ ? unknown_pool_ + unknown_[index].offset : nullptr;
  }
  // Chunks and payload bytes that did not fit the pool and were dropped since reset(): the UI tells the user.
  std::uint32_t unknown_dropped_chunks() const noexcept { return unknown_dropped_chunks_; }
  std::uint32_t unknown_dropped_bytes() const noexcept { return unknown_dropped_bytes_; }

  // ---- parameters (OS-002d; model/params.h says which values are parameters and how each is stored) ---------------
  // True when the parameter exists: its kind, index and field are valid and, for an instrument, the slot is in use. The kinds a
  // Project holds are Instrument, Track, Master and Project (the project's settings); a Device parameter belongs to the device's
  // Settings block, not to a project. OS-INSFX S7: also an Insert id (core::insert_param_valid) whose INSX record's blob reaches the field (the record is
  // index / 2, index % 2: the master's index 32 is record 16, 0); set_param does not write it (an insert is written by set_insert, not an undo unit, D9).
  bool has_param(core::ParamId id) const noexcept;
  // Its value in the descriptor's units (0 when it does not exist).
  float get_param(core::ParamId id) const noexcept;
  // The project's settings block: the values of kind Project and the project's MIDI Learn store.
  const Settings& settings() const noexcept { return settings_; }

  // ---- the project-level state that used to be runtime only (OS-004d, model/project_state.h) --------------------------
  // Which parts the project has been told about (set_* below, or a file that has the chunk): a bit each. MIDI and USB have no value until they are set; Mixer and Perform always
  // have one (the defaults), but only a part that is set is written to a file.
  static constexpr std::uint8_t kStateMidi = 1;
  static constexpr std::uint8_t kStateUsb = 2;
  static constexpr std::uint8_t kStateMix = 4;
  static constexpr std::uint8_t kStatePerf = 8;
  // OS-PERSIST2: the session state (project_state.h). Mixer, Perform and these always have a value (the defaults); only a part that was set is written.
  static constexpr std::uint8_t kStateNote = 16;
  static constexpr std::uint8_t kStateMute = 32;
  static constexpr std::uint8_t kStateScenes = 64;
  static constexpr std::uint8_t kStateSendFx = 128;
  std::uint8_t state_present() const noexcept { return state_present_; }
  // OS-BUG-PASTE24: the law the instruments' volume levels (0..100) are read through, state of the document. The native file stores it only when it is 1, as the LAW chunk (formats/native, docs/formats/o3-project.md); a file without the chunk loads as the 1.2+ law, 0, and the reader applies the stored value with set_volume_law at Done. 0 is
  // the 1.2+ instrument law (50 is 0 dB), 1 the 1.1.14 table (100 is 0 dB), the law this OS reads an OG project of a structure version below 7 through (our model, maintainer answer Q4; what the official firmware plays is an open research question). The loader sets it once a load has succeeded
  // (set_volume_law); reset() puts it back to 0 (a new or half-read document), except for a KEEP INSTRUMENTS reset, whose instruments, and the audio bound to them, stay as they were. The sample
  // bank reads it at every rebuild of a descriptor (app::project_law), so a rebuild cannot use a law other than the document's.
  std::uint8_t volume_law() const noexcept { return volume_law_; }
  void set_volume_law(std::uint8_t law) noexcept { volume_law_ = law == 0u ? std::uint8_t{0} : std::uint8_t{1}; }
  const MidiState& midi() const noexcept { return midi_; }
  const UsbState& usb() const noexcept { return usb_; }
  const MixState& mix() const noexcept { return mix_; }
  const PerfState& perf() const noexcept { return perf_; }
  const NoteState& note() const noexcept { return note_; }
  const MuteState& mute() const noexcept { return mute_; }
  const ScenesState& scenes() const noexcept { return scenes_; }
  const SendFxState& send_fx() const noexcept { return send_fx_; }
  // OS-INSFX S3a: the inserts, the INSX chunk. Present (written to a file) when inserts().count is not 0; a reset and a file without the chunk leave none.
  const InsertState& inserts() const noexcept { return inserts_; }
  // The record (where, slot), nullptr when there is none.
  const InsertRecord* find_insert(std::uint32_t where, std::uint32_t slot) const noexcept;
  // The apps whose parameters the project remembers, oldest stored first; the entry of a module name, nullptr when there is none.
  std::uint32_t app_params_count() const noexcept { return app_params_count_; }
  const AppParams* app_params(std::uint32_t index) const noexcept { return index < app_params_count_ ? &app_params_[index] : nullptr; }
  const AppParams* find_app_params(const char* name, std::size_t len) const noexcept;
  // The bytes the APPM chunk's payload takes for what is stored (version, count, the entries): the model keeps it within kAppParamsBytesMax.
  std::uint32_t app_params_bytes() const noexcept;

  // ---- generations ---------------------------------------------------------------------------------------
  // Free-running 32-bit counters (they wrap after 2^32 changes, compare for equality only). A screen redraws
  // when the one it shows differs from the value it saw last. A pattern's generation is the project counter at
  // its last change, so it is never repeated, not even by a pattern deleted and created again in another slot.
  std::uint32_t generation() const noexcept { return generation_; }  // any accepted mutation
  // Advances at every reset() (a project load, a new project): what the MIDI side compares to know that the instruments it has told the receivers about are not these ones (OS-INSTUI S8b: PROGRAM is sent before the first note after a load).
  std::uint32_t load_epoch() const noexcept { return param_epoch_; }
  // OS-FILES-C C2a (H15), not stored in any file: the pattern NAMES file of an OG import (the OGPN chunk) names patterns by their ids at the import. note_names() (the PAMD reader, once it has
  // kept the file) marks the names as matching the patterns; creating or deleting a pattern afterwards (an add, a remove, a renumber, the undo or redo of one) marks them stale, and so does
  // nothing else; reset() clears the mark. The OG writer does not give a stale file back (it would put a name on another pattern). A project the chunk came to by a native load is never noted, so
  // never stale: the mark cannot know what happened before the project was saved.
  void note_names() noexcept { names_state_ = kNamesFresh; }
  bool names_stale() const noexcept { return names_state_ == kNamesStale; }
  std::uint32_t tracks_generation() const noexcept { return tracks_generation_; }  // header, tracks, master
  std::uint32_t song_generation() const noexcept { return song_generation_; }
  // 0 for an id that does not exist
  std::uint32_t pattern_generation(std::uint32_t id) const noexcept;
  // OS-MIDIREC-P1: the lanes of a pattern live in a LaneStore outside the arena, so an Editor lane edit says so here: the generation
  // and the pattern's generation advance (the native Writer's Modified check and the Editor's undo sync both read them). False,
  // and nothing changes, for an id that does not exist.
  bool touch_lanes(std::uint32_t id) noexcept;
  std::uint32_t instrument_generation(std::uint32_t index) const noexcept;
  // A counter per parameter: advances whenever the value of THAT parameter changes, by set_param or by a whole-object mutator that
  // changes it (set_instrument, clear_instrument, set_track, set_master, set_settings, reset), and by nothing else; setting a value
  // to what it is changes nothing. Free-running, equality only, and never repeats. 0 for a parameter that does not exist. What a
  // screen polls on redraw; there are no observers. Exception (OS-INSFX S7): an Insert parameter has no counter; its value is read back from the INSX
  // record and the generation is the value's bits plus the load epoch (equal when the value is put back where it was).
  std::uint32_t param_generation(core::ParamId id) const noexcept;

  // ---- mutators ------------------------------------------------------------------------------------------

  Status set_name(const char* text) noexcept;
  Status set_name(const char* text, std::size_t len) noexcept;
  Status set_tempo_x100(std::uint32_t value) noexcept;
  Status set_swing(std::uint32_t percent) noexcept;
  Status set_default_pattern_length(std::uint32_t steps) noexcept;
  Status set_active_tracks(std::uint32_t count) noexcept;  // 1..kMaxTracks
  Status set_master(const MasterSettings& value) noexcept;
  // Whole slot; the name must be NUL-terminated within its field.
  Status set_track(std::uint32_t slot, const TrackSlot& value) noexcept;
  // OS-SONG3 S4 (PREVIEW): only the flags byte, with no generation moving — no touch(), no parameter generation (SaveService::dirty() reads generation(), and a transient
  // audition's solo set must not show as unsaved work). No format change: the same flags field, written plain. Nothing else of the slot is touched.
  void set_track_flags_plain(std::uint32_t slot, std::uint8_t flags) noexcept;
  Status set_track_name(std::uint32_t slot, const char* text, std::size_t len) noexcept;

  Status append_order(std::uint32_t pattern_id) noexcept;
  Status insert_order(std::uint32_t pos, std::uint32_t pattern_id) noexcept;  // pos <= song_length()
  Status set_order(std::uint32_t pos, std::uint32_t pattern_id) noexcept;     // pos < song_length()
  Status remove_order(std::uint32_t pos) noexcept;
  Status clear_order() noexcept;
  // The slot at `from` goes to index `to` (both < song_length()); the slots between shift by one. One change and one journal op, so a playing song follows the slot.
  Status move_order(std::uint32_t from, std::uint32_t to) noexcept;

  // E-SONG-07: the bracket of an edit of the order list. The mutators above open one each; a caller that makes several edits as one (a range delete, an undo unit) opens one around them, and
  // the render sees them together as ops of that bracket, merged where they touch (a run of single inserts or removes is one op). Nests (only the outermost one moves the epoch).
  class OrderEdit {
   public:
    explicit OrderEdit(Project& p) noexcept : p_(p) { p_.order_begin(); }
    ~OrderEdit() { p_.order_end(); }
    OrderEdit(const OrderEdit&) = delete;
    OrderEdit& operator=(const OrderEdit&) = delete;

   private:
    Project& p_;
  };
  // The ops since boot (the stamp a command carries for the list it was authored against).
  std::uint32_t order_ops() const noexcept { return order_ops_; }
  // The render's copy: false when an edit runs or ran during the copy (try again next block). `between` runs after the copy and before the check (a test seam).
  bool snapshot_order(OrderSnapshot& out, void (*between)(void*) noexcept = nullptr, void* ctx = nullptr) const noexcept;
  std::uint32_t order_epoch() const noexcept { return __atomic_load_n(&order_epoch_, __ATOMIC_ACQUIRE); }

  // New empty pattern (all steps empty, no name), every track `length` steps long.
  // Exists / Full / BadIndex (id 0 or > 255) / BadValue.
  Status create_pattern(std::uint32_t id, std::uint32_t length) noexcept;
  // Frees the pool slot. The song is not touched: positions naming it play as empty.
  Status delete_pattern(std::uint32_t id) noexcept;
  Status set_pattern_name(std::uint32_t id, const char* text, std::size_t len) noexcept;
  // One track's length (1..128; the steps past it are kept), or all 16 tracks at once (what a pattern
  // length edit does).
  Status set_track_length(std::uint32_t id, std::uint32_t track, std::uint32_t length) noexcept;
  Status set_pattern_length(std::uint32_t id, std::uint32_t length) noexcept;
  Status clear_pattern(std::uint32_t id) noexcept;  // every step empty, name and length kept
  Status set_step(std::uint32_t id, std::uint32_t track, std::uint32_t step, const Step& value) noexcept;
  // `count` consecutive steps of one track from `first`; all or nothing (count 0 is an accepted no-op).
  Status set_steps(std::uint32_t id, std::uint32_t track, std::uint32_t first, const Step* values,
                   std::uint32_t count) noexcept;

  // Replaces a slot. validate_instrument() decides; Module instruments need their module in the table.
  Status set_instrument(std::uint32_t index, const Instrument& value) noexcept;
  Status clear_instrument(std::uint32_t index) noexcept;

  // One parameter (kinds Instrument, Track, Master and Project): the value must be inside the descriptor's range (BadValue, NaN too);
  // an integer field is rounded to the nearest, a switch is 0 or 1. BadIndex: no such parameter or a kind the Project does not hold;
  // NotFound: the instrument slot is free. A value equal to the stored one is Ok and changes nothing.
  Status set_param(core::ParamId id, float value) noexcept;
  // The whole settings block of scope project (values and Learn store): all or nothing, BadValue if it is of another scope or invalid.
  Status set_settings(const Settings& value) noexcept;
  // One slot of the project's Learn store (see Settings::set_learn).
  Status set_learn_slot(unsigned slot, const core::LearnBinding& b) noexcept;

  // The project-level state (OS-004d). Each checks the value (BadValue with nothing changed), stores it, marks the part present and bumps the generation; a value equal to the stored
  // one of a part that is already present changes nothing. These are NOT undoable (the Editor's wrappers keep the history); a load goes through them too.
  Status set_midi(const MidiState& value) noexcept;
  Status set_usb(const UsbState& value) noexcept;
  Status set_mix(const MixState& value) noexcept;
  Status set_perf(const PerfState& value) noexcept;
  // OS-PERSIST2, the same rules: strict (BadValue and nothing changed for a value validate_*() refuses; a reader sanitises first), marks the part present, a value equal to the stored one of a part that is present changes nothing.
  Status set_note(const NoteState& value) noexcept;
  Status set_mute(const MuteState& value) noexcept;
  Status set_scenes(const ScenesState& value) noexcept;
  Status set_send_fx(const SendFxState& value) noexcept;
  // OS-INSFX S3a: puts one insert (validate_insert_record against module_count(): BadValue and nothing changed for a record it refuses). Replaces the record of the same (where, slot) or adds
  // one in (where, slot) order (Full when kMaxInserts are taken); a record equal to the stored one changes nothing. NOT undoable (D9). MODR is never pruned: the module stays in the table.
  Status set_insert(const InsertRecord& value) noexcept;
  // Removes the record (where, slot): BadIndex for a (where, slot) no record can have, NotFound when there is none (nothing changes). The module stays in MODR.
  Status clear_insert(std::uint32_t where, std::uint32_t slot) noexcept;
  // Removes every insert (a project without the chunk has none); reset() does it too.
  void clear_inserts() noexcept;
  // The parameters of an app: `count` values (1..kMaxAppParams, finite) for the module `name` built with ABI abi_major.abi_minor. Replaces the entry of that name and moves it to the
  // end; a new name when all kMaxAppParamSets are taken, or when the entries would no longer fit kAppParamsBytesMax, drops the oldest. BadValue for an unsafe name, a bad count or a non-finite value.
  Status set_app_params(const char* name, std::size_t len, std::uint32_t abi_major, std::uint32_t abi_minor, const float* values, std::uint32_t count) noexcept;
  // Forgets every app's parameters.
  void clear_app_params() noexcept;

  // Appends a module reference (name safe per is_safe_module_name, no duplicate name).
  Status add_module(const char* name, std::size_t len, std::uint32_t abi_major, std::uint32_t abi_minor) noexcept;
  // Empties the table; BadValue (nothing changes) while a Module instrument still names one.
  Status clear_modules() noexcept;

  // Unknown chunks. begin reserves `length` zeroed bytes in the pool and makes them the chunk being filled
  // (Full: no room, the chunk is counted as dropped and nothing is reserved; BadValue: id is not four
  // printable ASCII characters); append adds to it, up to the reserved length (BadValue when it would exceed
  // it or nothing is being filled). A chunk is complete when exactly `length` bytes were appended.
  Status unknown_begin(std::uint32_t id, std::uint32_t length) noexcept;
  Status unknown_append(const std::uint8_t* data, std::uint32_t count) noexcept;
  void clear_unknown() noexcept;

  // True when every invariant holds (ranges, terminated texts, id map, module references, unknown table).
  // Always true after any sequence of accepted mutations and after a successful load; tests and the fuzz
  // loop call it to prove that.
  bool check_invariants() const noexcept;

 private:
  static constexpr std::uint8_t kNoSlot = 0xFF;
  static constexpr std::uint8_t kNamesNone = 0;   // names_state_: no pattern names file noted
  static constexpr std::uint8_t kNamesFresh = 1;  // noted, and no pattern created or deleted since
  static constexpr std::uint8_t kNamesStale = 2;

  void order_begin() noexcept;
  void order_end() noexcept;
  void order_log(std::uint8_t kind, std::uint32_t a, std::uint32_t b, std::uint32_t n) noexcept;

  Pattern* find_pattern(std::uint32_t id) noexcept;
  void touch() noexcept { ++generation_; }
  void bump_instrument_params(std::uint32_t index, const Instrument* before, const Instrument& after) noexcept;
  void touch_pattern(const Pattern& p) noexcept;

  char name_[kProjectNameMax + 1];
  std::uint8_t swing_;
  std::uint8_t default_pattern_length_;
  std::uint8_t active_tracks_;
  std::uint8_t song_length_;
  std::uint8_t pattern_count_;
  std::uint8_t instrument_count_;
  std::uint8_t module_count_;
  std::uint8_t names_state_;  // kNamesNone, kNamesFresh, kNamesStale
  std::uint8_t unknown_filling_;  // index of the unknown chunk being filled, or kNoSlot
  std::uint32_t tempo_x100_;
  alignas(4) std::uint8_t song_[kMaxSongPositions];  // word-aligned (it follows a word): Project::snapshot_order reads it as relaxed words; placed here so the arena keeps its size
  MasterSettings master_;
  TrackSlot tracks_[kMaxTracks];
  std::uint8_t slot_of_id_[kMaxPatternId + 1];  // pool slot of a pattern id, kNoSlot if none
  ModuleRef modules_[kMaxModules];
  UnknownChunk unknown_[kMaxUnknownChunks];
  std::uint8_t state_present_;      // kState* bits
  std::uint8_t volume_law_;         // OS-BUG-PASTE24: 0 the 1.2+ instrument law, 1 the 1.1.14 table (stored as the LAW chunk when 1; no chunk = 0)
  std::uint8_t app_params_count_;
  MidiState midi_;
  UsbState usb_;
  MixState mix_;
  PerfState perf_;
  NoteState note_;
  MuteState mute_;
  ScenesState scenes_;
  SendFxState send_fx_;
  InsertState inserts_;  // OS-INSFX S3a
  AppParams app_params_[kMaxAppParamSets];
  std::uint32_t unknown_count_;
  std::uint32_t unknown_used_;    // bytes reserved in the pool
  std::uint32_t unknown_filled_;  // bytes appended to the chunk being filled
  std::uint32_t unknown_dropped_chunks_;
  std::uint32_t unknown_dropped_bytes_;
  std::uint32_t generation_;
  std::uint32_t tracks_generation_;
  std::uint32_t song_generation_;
  // E-SONG-07, runtime only (not saved, never rewound by reset()): written by the main loop inside an OrderEdit, read by the render (Sequencer::adopt_order) as a seqlock: the epoch is odd
  // while an edit runs, ops and journal belong to the epoch read; one writer (the main loop), one reader (the render, which preempts it on the MK66). The list song_, song_length_, order_ops_ and
  // order_journal_ are written and copied as relaxed atomic bytes / words (project.cpp: store_byte, store_word), the epoch carries the order.
  std::uint32_t order_epoch_;  // accessed with the __atomic builtins only (a plain word keeps Project trivially copyable: the simulator zeroes arenas)
  std::uint32_t order_ops_;
  std::uint32_t order_open_;  // order_ops_ when the outermost bracket opened: only ops after it may merge
  OrderOp order_journal_[kOrderJournal];
  std::uint8_t order_depth_;
  std::uint32_t pattern_generation_[kMaxPatterns];      // by pool slot
  std::uint32_t instrument_generation_[kMaxInstruments];
  std::uint32_t param_generation_[kProjectParamSlots];  // see param_generation(); the epoch is added to each
  std::uint32_t param_epoch_;
  Settings settings_;
  std::uint8_t unknown_pool_[kUnknownPoolBytes];
  Instrument instruments_[kMaxInstruments];
  Pattern patterns_[kMaxPatterns];
};

static_assert(std::is_standard_layout<Project>::value && std::is_trivially_default_constructible<Project>::value &&
                  std::is_trivially_destructible<Project>::value,
              "the arena may live in raw SDRAM: Project must be trivial");

// What the platform provides: one object, aligned for the largest member (4) and rounded up to 8 so the
// start is also a good address for a future DMA or SIMD touch. host: `static ModelArena arena;`; MK66: the
// same object in the SDRAM section by the platform's linker script. It is a typed object, never a cast of a
// raw buffer. Call arena.project.reset() once before use (a Reader::begin does it again).
struct alignas(8) ModelArena {
  Project project;
};

inline constexpr std::size_t kModelArenaBytes = sizeof(ModelArena);

static_assert(alignof(Project) == 4 && alignof(ModelArena) == 8 && sizeof(ModelArena) % 8 == 0,
              "the arena is 8-byte aligned and sized in whole 8-byte units (ADR 0011)");

}  // namespace o3os::model
