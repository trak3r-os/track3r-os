// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The SD modules' side of the user interface (OS-012b, ADR 0002 amendment 1): what the Instrument page, the engine picker, the MODULES screen and an app's page
// need of the modules, in one object the platform (or the simulator, or a test) owns and attaches to the App (App::attach_modules, Model::modules).
//
//   VALUE CONTROLS. A module's parameters are its o3m_param descriptors; describe() turns one into a core::ParamDesc (name upper-cased and cut to the page's
//   12 characters, the unit text, the curve, the steps, learnable unless the module says otherwise), so the generated page, MIDI Learn, undo and the Perform layer
//   work on a module's parameters with no module code. A parameter of a Module instrument is ParamId {Instrument, slot, kModuleFirst + i} (model/params.h): its
//   value is a float in the instrument's blob, which the project file already stores. An app's parameters live here (runtime values, no project): the page edits
//   them with the same controls; Learn, undo and Perform for them wait for a decision on where they are saved (docs/guides/module-sdk.md).
//
//   VIEWS. draw_slot_view() / draw_app_view() run a module's view_draw() through module::ViewHost (budgets, record, replay) and replay the record to the
//   renderer. A view that goes over a budget is blanked, its module's views are blocked for the boot and the module is quarantined (Status::ViewOverrun).
//
//   ENGINE PICKER. scan_instruments() reads modules/instruments/ (and notes the modules the project names that the card does not have: greyed in the picker);
//   pick() binds a module to an instrument slot: it loads the module if need be (synchronously: a module is a few KB, a few SD reads; the project-load path
//   is the sliced one), writes the instrument and its default parameter blob through the editor (one undo unit) and binds an instance. A slot that already
//   has an instance cannot be changed at run time (the voices have no unbind: reload the project) and says so.
//
//   APPS. scan_apps() reads modules/apps/; launch_app() loads one (if need be), gives it its state block and calls init() and enter(); app_input() hands it the
//   input events; app_exit() calls exit() and shutdown(). One app at a time. An app that goes over a budget is stopped and quarantined; the exit chord (F7 + F8,
//   the OS's, bound in its page's context) works whatever the module does.
//
//   APP HOST API v2 (OS-H1, ADR 0014; ABI 1.3). launch_app() builds the app's own copy of the host table: the capabilities its file asks for (header reserved2[0]) decide which of
//   the read, edit and io tables it is lent (NULL otherwise). The tables' functions have no context argument, so they reach this object through the one running app (a file-local
//   pointer set while an app runs, main loop only). set_tempo only QUEUES a request (one value, the last wins): the App applies it as the undoable action "module.app_tempo"
//   (take_tempo_request, App::poll_modules). An app that holds edit_project is confirmed once per boot: the first F1 answers NeedsConfirm, the second runs it. MIDI in: the app's
//   midi_read drains the render's monitor ring (midi/monitor.h), which is switched on only while an app with midi_in runs.
//   EDIT GROUPS (OS-H3A, ABI 1.5): edit_begin / set_step(s) / set_tempo / edit_end. A write is CHECKED at the call (host_set_steps: pattern, track, row, count, values, mask; the pattern
//   in `project_`) and only STORED in a fixed buffer (ops_ + pool_, 2,048 steps); edit_end hands the whole group to the App through the edit sink (set_edit_sink), which runs the undoable action
//   "module.app_group" with this object as its source: apply_group() writes every op through the Editor inside the unit that dispatch() opened (so the user's UNDO takes the whole group, the
//   dirty flag, the sequencer and macros follow as for a hand edit). Nothing touches the project before that, so a group that overflows (4,096 write calls or 2,048 steps) is DROPPED, and the
//   rollback is "never applied": edit_end answers LIMIT and the project is byte-identical. A group still open when the app's call returns (end_call) is closed and applied by the OS and counted
//   (abandoned_groups()). Groups are made from input() only (the App edits on the user's key press), at most one per call into the app.
//   INSTRUMENT PARAMETERS (OS-H3B, ABI 1.6): instrument_info / get_param_desc / get_instrument_param read the project's instrument slot (a SAMPLE's 83 table rows, a MODULE's own descriptors); set_instrument_param joins the
//   open group as an op of its own (EditOp::pattern 0, the slot in `track`, the id and the value in one pool slot: the group's limits are unchanged, a parameter write is one call, one of the 2,048 slots and one of the O3M_EDIT_MAX_PARAM_WRITES = 64 a group may hold: the rings write_param pushes into take no more). It is
//   CHECKED at the call (a SAMPLE or MODULE instrument, a known id, model::instrument_field_app_writable for a sample / any module parameter, finite and inside min..max) and WRITTEN at edit_end through the
//   same write_param the Params page uses (module_app_group), so the engine hears it, UNDO takes it back with the group's other writes and the old float bits come back exactly.
//   EXPORT WRITES (OS-H4, ABI 1.7): an app that holds export_files is lent the four export_* members of the io table over the AppExport object the SaveService owns (set_export: the App gives it);
//   they never block and write only AppExport/<app>/ (app/app_export.h has the rules). The app's launch starts its file count (begin_app) and release_host() aborts a file it left open or committing
//   (end_app). The first launch per boot of an app that holds edit_project OR export_files is confirmed (the text names what it does; both bits make ONE message); TOOLS lists such apps too.
//   AUDIO TAP (OS-H2a, ABI 1.4): an app that holds audio_tap is lent the four tap_* members of the io table over the render's AppTap (audio/app_tap.h). The tap is Off until the app selects a
//   point, and Off again when the app stops. The cursors of tap_stream and tap_levels are the object's (one app runs at a time); a select starts them two blocks after "now", so that nothing the
//   render wrote for the previous point is ever returned. The render stores the SAMPLES of the tap only after the app's first tap_latest or tap_stream call (OS-BUG-TAPWRITE: a launch clears
//   AppTap::samples, that first call sets it and moves the freshness point two blocks on); an app that reads only tap_levels never costs the render the 32 SDRAM stores.
//
// Contexts: main loop only. No heap: the catalogs, the state of the running app and the view host (about 6 KB) and, since OS-H3A, an app's edit group (about 26 KB) are members; on the MK66 the object lives in the module
// region (SDRAM), not in SRAM_U.

#include <o3os/audio/app_tap.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/param.h>
#include <o3os/hal/clock.h>
#include <o3os/hal/storage.h>
#include <o3os/midi/monitor.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>
#include <o3os/module/loader.h>
#include <o3os/module/view.h>
#include <o3os/ui/renderer.h>

#include <cstdint>
#include <cstring>

namespace o3os::app {

struct Model;
class AppExport;
class PreviewSink;

// Working memory of one descriptor made by describe(): the ParamDesc points into it (the name).
struct ParamScratch {
  core::ParamDesc desc;
  char name[core::kParamNameChars + 1];
  bool whole;  // the module says whole numbers (INT or BOOL): the app rounds before it writes
};

// The descriptor of a module parameter as the standard controls take it. False for a descriptor no control can show (an empty range). Pure. `labels` (OS-INSTUI S10, ABI 1.8: the module's entry for this
// parameter, or null): an INT parameter whose values have names is an enumeration, its value text the label (names[round(value - min)]); an entry that does not fit the range is ignored.
bool describe_module_param(const o3m_param& p, ParamScratch& out, const o3m_labels* labels = nullptr) noexcept;

// The descriptor of any parameter id: the model's table for the built-in ones, the module's for a module parameter (Model::modules). nullptr when there is none.
// `scratch` must outlive the use of the result.
const core::ParamDesc* describe_param(const Model& model, core::ParamId id, ParamScratch& scratch) noexcept;
// Whether `id` is a module parameter (Instrument kind, field kModuleFirst + i).
bool is_module_param(core::ParamId id) noexcept;
// OS-INSTUI S8a: whether `id` is the MIDI page's CHANNEL (the virtual field instrument_param::MidiChannel of an instrument slot; describe_param answers it, the tables do not).
bool is_midi_channel_param(core::ParamId id) noexcept;
// OS-STEREO step 2: whether `id` is the source page's STEREO choice (the virtual field instrument_param::PlayStereo of an instrument slot; describe_param answers it, the tables do not).
bool is_play_stereo_param(core::ParamId id) noexcept;

inline constexpr unsigned kModuleCatalogMax = 16;

struct CatalogEntry {
  enum class State : std::uint8_t {
    Ready,        // on the card, not loaded yet
    Loaded,       // in the set
    Quarantined,  // refused or faulted this boot
    Missing,      // the project names it and the card does not have it
  };
  char stem[module::kStemMax + 1];
  State state;
};

class ModuleUi {
 public:
  enum class PickResult : std::uint8_t {
    Ok,
    OnComputer,     // OS-USBDISK 4.3: the computer has the card: a module already bound keeps playing, a new one is not loaded
    NoEntry,        // no such entry in the picker
    Missing,        // not on the card
    Quarantined,
    Bound,          // the slot already has an instance
    NoProject,
    NoCard,
    LoadFailed,     // the file would not load (module::Status in last_status())
    NotInstrument,  // an effect or an app
    NotRunnable,    // loaded, but this build cannot call it
    NoRoom,
    TableFull,      // the project's module table is full
    BindFailed,     // init() refused or the instances are all taken
    BadInstrument,  // the editor refused the instrument
  };
  enum class LaunchResult : std::uint8_t { Ok, OnComputer, NoEntry, NoCard, Quarantined, LoadFailed, NotApp, NoRoom, InitFailed, Busy, NeedsConfirm, NeedsAudioTap, NeedsExportFiles, NeedsMidiOut, NeedsNewerOs };  // the last three: the app asks for a published capability this OS does not grant (C4)

  // ---- composition (once, the main loop) -----------------------------------------------------------------------------------------------
  void attach(module::ModuleSet& set, module::ModuleLoader& loader, audio::ModuleVoices& voices) noexcept {
    set_ = &set;
    loader_ = &loader;
    voices_ = &voices;
  }
  void set_storage(hal::storage::Storage* card) noexcept { card_ = card; }
  void set_sync(module::ModuleLoader::SyncFn sync) noexcept { sync_ = sync; }
  // OS-H1: the render's MIDI-in ring for the apps that hold midi_in (nullptr: none in this build, those apps are lent no midi_read).
  void set_midi_monitor(midi::MidiMonitor* monitor) noexcept { midi_ = monitor; }
  // OS-H3A: where a closed edit group goes to be applied. `apply(ctx, arg)` must run the undoable action "module.app_group" (arg: the app's catalog entry, the unit's name) and answer an
  // O3M_EDIT_* code; the App sets it (App::attach_modules). Without a sink (a bare ModuleUi) a group cannot be applied: edit_end answers NO_PROJECT and drops it.
  using EditSink = std::int32_t (*)(void* ctx, std::int32_t arg) noexcept;
  void set_edit_sink(EditSink sink, void* ctx) noexcept {
    sink_ = sink;
    sink_ctx_ = ctx;
  }
  // OS-H4: where the export calls of the apps that hold export_files go (the SaveService's AppExport; nullptr: none, those calls answer NO_CARD). Set by App::attach_saves / attach_modules.
  void set_export(AppExport* export_service) noexcept { export_ = export_service; }
  // OS-H2a: the render's audio tap for the apps that hold audio_tap (nullptr: none in this build: such an app is refused at launch, "NEEDS AUDIO TAP").
  void set_audio_tap(audio::AppTap* tap) noexcept { tap_ = tap; }
  // OS-APPUX S3 (ABI 1.10): the sequencer's play position for get_transport (nullptr: none, the call answers 1 with zeros), and the live-note path for play_note / stop_note (the App's PreviewSink: the
  // engine's live queue on the unit; nullptr: none, play_note answers O3M_PLAY_NO_AUDIO). Set by App::attach_modules / attach_play_position / attach_preview.
  void set_play_position(const audio::PlayPosition* position) noexcept { position_ = position; }
  void set_note_sink(PreviewSink* sink) noexcept { notes_ = sink; }
  // The clock the views' time budget runs on (microseconds; null: no time budget), and the crash-context stamp of audio::ModuleVoices::Hooks::enter.
  void set_clock(const hal::Clock* clock, std::uint16_t (*stamp)(std::uint16_t) = nullptr) noexcept;
  module::ViewHost& views() noexcept { return views_; }
  const module::ModuleSet* set() const noexcept { return set_; }
  // The project's modules were cleared (module::ModuleSet::clear): nothing here may point into them any more. Stops the running app without calling it.
  void reset() noexcept;

  // ---- the slot's module (Instrument page) ---------------------------------------------------------------------------------------------
  // The module of a Module instrument that is loaded and bound to a voice instance: its descriptor, and its index in the set. nullptr otherwise (the instrument is muted
  // or not a module's).
  const o3m_module* bound_module(const model::Project& project, unsigned slot, int* set_index = nullptr) const noexcept;
  // Parameters of that module (0 when it is none), the descriptor of one, its default.
  unsigned param_count(const model::Project& project, unsigned slot) const noexcept;
  // OS-INSTUI S10 (ABI 1.8): the params pages the slot's module declares (its o3m_page table; only a module of ABI 1.8 has one): how many (0: it declares none, the page makes pages of eight called
  // "1-8" ...), and page k's name, first parameter and parameter count (held to 1..8 and to the module's parameters).
  unsigned page_table_count(const model::Project& project, unsigned slot) const noexcept;
  bool page_at(const model::Project& project, unsigned slot, unsigned k, const char*& name, unsigned& first, unsigned& count) const noexcept;
  bool describe(const model::Project& project, unsigned slot, unsigned index, ParamScratch& out) const noexcept;
  // Views the page may draw for the slot: 0 for a module without views, one whose views are blocked, or one that is not bound.
  unsigned view_count(const model::Project& project, unsigned slot) const noexcept;
  // A new page is being drawn: the words of the module views drawn so far in the frame are forgotten. The views of one page share module::kViewMaxWords (the frame buffer is one
  // and the page's own words are in it too); the page calls this once before its first view.
  void begin_view_frame() noexcept { view_words_ = 0; }
  // Draws view `view` of the slot's module in `area` of the frame in progress (the canvas is `area`, clamped to the legal sizes). False: nothing drawn (blanked, blocked, no
  // module, area too small). A budget overrun blocks and quarantines the module.
  bool draw_slot_view(ui::Renderer& r, const model::Project& project, unsigned slot, unsigned view, const ui::Rect& area) noexcept;
  // A parameter value to the module's instance (the render applies it at its next block start). False when the slot has no instance or the ring is full.
  bool send_param(unsigned slot, unsigned index, float value) noexcept;
  // OS-GOV (audio/module_governor.h): what the render measured of the slot's instance. The cycles of ONE voice for a whole block (0: never measured, or not bound; governor_watch.h
  // append_voice_cost says it as a percent), and whether the safety net muted the instance (it stays muted until a note after the cool-down, or a reload).
  std::uint32_t voice_cost(unsigned slot) const noexcept { return voices_ != nullptr ? voices_->voice_cost(slot) : 0u; }
  bool slot_muted(unsigned slot) const noexcept { return voices_ != nullptr && voices_->muted(slot); }
  // OS-INSTUI S5: the gate of every bound instance follows the project: open while the slot's instrument is a Module, closed otherwise (an instrument deleted from the slot, a sample or MIDI instrument
  // put there; an undo opens it again). The instance itself stays bound: there is no unbind before the next project load. Main loop; called by the actions that change an instrument, by undo and
  // redo, and once a pass.
  void sync_slots(const model::Project& project) noexcept;
  // OS-BUGS4: the console's `mod bind` puts an instance on a slot whose project instrument is free (a bench aid: a free slot makes no sound from a pattern, but `mod note` and a pad play it). The
  // once-a-pass sync_slots would close that gate at the next pass; a slot marked here is left open instead while the project's slot is free (no instrument); a slot that gets an instrument follows the project from then on, for good (the mark is dropped); the marks go when the project is replaced (diag_clear). Main loop only.
  void diag_keep_open(unsigned slot) noexcept { diag_slots_ |= slot < 64u ? (std::uint64_t{1} << slot) : 0u; }
  void diag_clear() noexcept { diag_slots_ = 0; }
  // The same for a module of the picker, by its stem: the largest measured cost among the bound instances of that module (0: none measured, or the module is not loaded).
  std::uint32_t voice_cost_of(const char* stem) const noexcept;
  // The module name of the slot's instrument for the page ("tb303"), or "" when it is not a Module instrument.
  static const char* module_name_of(const model::Project& project, unsigned slot) noexcept;

  // ---- the engine picker -----------------------------------------------------------------------------------------------------------------
  // Reads modules/instruments/, adds the built-in instrument modules (module/builtin.h: there with no card) and the project's missing modules (`project` may be null). Entries are sorted by name.
  void scan_instruments(const model::Project* project) noexcept;
  unsigned instrument_count() const noexcept { return inst_count_; }
  const CatalogEntry* instrument_at(unsigned i) const noexcept { return i < inst_count_ ? &inst_[i] : nullptr; }
  // OS-USBDISK 4.3: while the computer has the card, pick() and launch_app() answer OnComputer before they touch the loader. Main loop only.
  void set_card_on_computer(bool on) noexcept { card_on_computer_ = on; }
  PickResult pick(model::Editor& editor, unsigned slot, unsigned entry) noexcept;
  static const char* pick_text(PickResult r) noexcept;  // upper case, for the notification strip

  // ---- apps ---------------------------------------------------------------------------------------------------------------------------
  void scan_apps() noexcept;
  unsigned app_entry_count() const noexcept { return app_count_; }
  const CatalogEntry* app_entry_at(unsigned i) const noexcept { return i < app_count_ ? &apps_[i] : nullptr; }
  // OS-H3A, Pattern F7 TOOLS: the apps that hold edit_project (OS-H4: or export_files), in the catalog's order (sorted by name). scan_tools() reads the card again (scan_apps) and each file's header (64 bytes; a loaded app is
  // not read again), so it is for when the list opens, not for every frame. tool_entry(i) is the index launch_app takes.
  void scan_tools() noexcept;
  unsigned tool_count() const noexcept { return tool_count_; }
  unsigned tool_entry(unsigned i) const noexcept { return i < tool_count_ ? tools_[i] : 0u; }
  // The capability word of an app of the catalog (header reserved2[0]): from the loaded module, else the file's header; 0 when it cannot be read or is not an app.
  std::uint32_t app_caps(unsigned entry) const noexcept;
  // The name an app's undo unit carries for the user: the module's own name (upper case) when it is loaded, else the file stem. `entry` is the unit's argument.
  bool app_title(unsigned entry, char* out, unsigned cap) const noexcept;
  // `project` is the document the app's read calls see (nullptr: none open). `ctx` is the launch context (OS-H1: nullptr = opened from the app list, all zeros; Pattern F7 TOOLS passes the
  // Pattern's block, OS-H3A). An app that holds edit_project is not started the first time per boot: NeedsConfirm (the caller says so), and the next launch of the same app goes through.
  LaunchResult launch_app(unsigned entry, const model::Project* project = nullptr, const o3m_launch_ctx* ctx = nullptr) noexcept;
  static const char* launch_text(LaunchResult r) noexcept;
  // The text to show for a launch that answered `r`: launch_text(r), except that NeedsConfirm says what the app does ("EDITS THE PROJECT (UNDOABLE): F1 AGAIN", "WRITES FILES TO APPEXPORT/<APP>: F1 AGAIN", or
  // one message for both: "EDITS (UNDOABLE) + WRITES FILES: F1 AGAIN"), at most kConfirmChars (the notification strip's 48). Valid until the next launch.
  static constexpr unsigned kConfirmChars = 48;
  const char* launch_message(LaunchResult r) const noexcept { return r == LaunchResult::NeedsConfirm && confirm_text_[0] != '\0' ? confirm_text_ : launch_text(r); }
  // OS-KEYROWDLG: the confirm of the last launch is waiting for its second F1, and it is catalog entry `entry` it asked about: the key row marks F1 by it (the text is what the strip said).
  bool confirm_pending_for(unsigned entry) const noexcept {
    const CatalogEntry* e = app_entry_at(entry);
    return e != nullptr && confirm_stem_[0] != '\0' && std::strcmp(confirm_stem_, e->stem) == 0;
  }
  const char* confirm_text() const noexcept { return confirm_text_; }
  bool app_running() const noexcept { return app_running_; }
  bool app_stopped() const noexcept { return app_stopped_; }  // went over a budget: its page says so, only the exit chord is left
  module::ViewStatus app_verdict() const noexcept { return app_verdict_; }
  const char* app_name() const noexcept { return app_stem_; }  // the file stem of the running (or last) app
  const void* app_state() const noexcept { return app_state_; }  // the running app's state block (tests and `diag`)
  const o3m_module* app_module() const noexcept;               // the running app's descriptor, nullptr when none
  unsigned app_param_count() const noexcept;
  // OS-APPUX S3: the OS parameter column (the page's list of the declared rows, edited with Shift + the arrows and Shift + the encoder) belongs to an app that declares rows and was built below ABI 1.10. An
  // app of 1.10 draws its own rows in the grammar of modules/apps/common/o3m_edit.h and declares them as storage (set_own_param): it gets no column, and every Shift key reaches its input().
  bool app_os_column() const noexcept { return app_param_count() != 0u && app_minor_ < 10u; }
  bool app_describe(unsigned index, ParamScratch& out) const noexcept;
  float app_param(unsigned index) const noexcept { return index < o3m_params_max ? app_values_[index] : 0.0f; }
  // Sets an app parameter (clamped to its descriptor) and tells the app (its set_param, under the time budget). True when the value changed.
  bool app_set_param(unsigned index, float value) noexcept;
  // One input event (the raw Tracker control, see o3m_ui_event); `shift` is the Shift layer. False when no app is running (or it was stopped).
  bool app_input(std::uint32_t kind, std::uint32_t id, std::int32_t value, bool shift) noexcept;
  // ABI 1.9 (OS-KEYROWDLG S5): the key row of the running app. True while an app that declares ABI 1.9 and a keys() runs and is not stopped; the page then draws its labels (else the legacy row, BACK on F8).
  // app_key_label(i, shifted) is F(i+1)'s (Shift + F(i+1)'s) label as the OS copied it (at most 7 printable ASCII characters), "" for a blank key; F8 is always "": the OS draws BACK there.
  bool app_keys_declared() const noexcept { return keys_declared_ && app_running_ && !app_stopped_; }
  const char* app_key_label(unsigned i, bool shifted) const noexcept { return i < 8u ? keys_[(shifted ? 8u : 0u) + i] : ""; }
  // The App's bookkeeping of what the app was given, here (SDRAM) so that the App itself, which lives in the SRAM that has no room, does not carry it: the buttons and pads whose press
  // the app has seen (only their release is passed on), and the uptime of the last redraw the page asked for.
  std::uint64_t buttons_down = 0;
  std::uint64_t pads_down = 0;
  std::uint64_t frame_ms = 0;
  // The page body: view 0 of the app in `area`. False: nothing drawn (stopped, no area).
  bool draw_app_view(ui::Renderer& r, const ui::Rect& area) noexcept;
  void app_exit() noexcept;

  // ---- the host calls of the running app (OS-H1; the table functions in module_ui.cpp call these) ----------------------------------------------------------------------------------
  // The app's host table (what it was lent), for tests; nullptr when no app runs.
  const o3m_host* app_host() const noexcept { return app_running_ ? &app_host_ : nullptr; }
  // The project's tempo in BPM x 100, or the queued request; 0 without a project.
  std::uint32_t host_tempo() const noexcept;
  // Queues a tempo (BPM x 100): an O3M_EDIT_* code. One request is held, the last wins.
  int host_set_tempo(std::uint32_t bpm_x100) noexcept;
  // The queued request, once (false: none). The App applies it as an undoable action.
  bool take_tempo_request(std::uint32_t& bpm_x100) noexcept;
  // OS-H3A: the read and edit entries of ABI 1.5 (the table functions call these). Same O3M_EDIT_* codes as the edit table.
  std::int32_t host_pattern_info(std::uint16_t pattern, o3m_pattern_info* out) const noexcept;
  std::int32_t host_get_steps(std::uint16_t pattern, std::uint8_t track, std::uint8_t first_row, o3m_step* out, std::uint8_t count) const noexcept;
  std::int32_t host_edit_begin() noexcept;
  std::int32_t host_edit_end() noexcept;
  std::int32_t host_set_step(std::uint16_t pattern, std::uint8_t track, std::uint8_t row, const o3m_step* s, std::uint8_t fields) noexcept;
  std::int32_t host_set_steps(std::uint16_t pattern, std::uint8_t track, std::uint8_t first_row, const o3m_step* s, std::uint8_t count, std::uint8_t fields) noexcept;
  // OS-H3B: the instrument-parameter entries of ABI 1.6 (instrument = 1..64, the step byte). Same O3M_EDIT_* codes.
  std::int32_t host_instrument_info(std::uint8_t instrument, o3m_instrument_info* out) const noexcept;
  std::int32_t host_get_param_desc(std::uint8_t instrument, std::uint16_t index, o3m_param_desc* out) const noexcept;
  std::int32_t host_get_instrument_param(std::uint8_t instrument, std::uint16_t id, float* out) const noexcept;
  std::int32_t host_set_instrument_param(std::uint8_t instrument, std::uint16_t id, float value) noexcept;
  // The parameter writes of the closed group, in call order (the action's side, module.app_group): start with op = at = 0 and call until false; each true answer is the next write (id, value) and advances the
  // cursor. They are written by the action through write_param, after apply_group() has written the steps and the tempo (apply_group checks them too, so the group is applied whole or not at all).
  bool next_param_op(unsigned& op, unsigned& at, core::ParamId& id, float& value) const noexcept;
  // The action's side (module.app_group): writes the closed group through `editor`, in the unit its caller opened. False when it changed nothing. apply_result() is the O3M_EDIT_* answer
  // (OK; RANGE when a write no longer fits the project, which cannot happen between the call and the apply and then writes NOTHING; the group is validated whole before the first write).
  bool apply_group(model::Editor& editor) noexcept;
  std::int32_t apply_result() const noexcept { return apply_result_; }
  // OS-APPUX S5: where the last applied group wrote steps: the pattern, the tracks and the rows (the union of the ops that changed a step; `valid` false when it changed none). The action
  // (module.app_group) puts the Pattern's cursor on the first written step and selects the block, so that the Pattern opens on what the tool wrote.
  struct GroupExtent {
    bool valid = false;
    std::uint8_t pattern = 0;
    std::uint8_t track0 = 0, track1 = 0;
    std::uint16_t row0 = 0, row1 = 0;
  };
  const GroupExtent& group_extent() const noexcept { return extent_; }
  // Whether a closed group waits to be applied (only between edit_end and the action, for the tests).
  bool group_closed() const noexcept { return closed_; }
  bool group_open() const noexcept { return g_open_; }
  // The main loop's share of this pass spent in the running app's input calls and in applying their groups (microseconds of the clock). The App stops draining the app's input events once it passes
  // kPassBudgetUs and leaves the rest of the queue for the next pass: one pass can no longer cost 64 events x (4 ms + an apply of up to 2,048 steps). begin_pass() starts a pass.
  static constexpr std::uint32_t kPassBudgetUs = 8000;
  void begin_pass() noexcept {
    pass_us_ = 0;
    triggers_ = 0;  // OS-APPUX S3: the app's play_note / stop_note calls are counted per pass (O3M_PLAY_MAX_PER_FRAME)
  }
  bool pass_full() const noexcept { return pass_us_ >= kPassBudgetUs; }
  // Counters since the object was made: groups applied (with at least one write), groups dropped whole (LIMIT), groups the app left open (closed and applied by end_call), and the writes refused for a
  // bad argument.
  std::uint32_t groups_applied() const noexcept { return n_applied_; }
  std::uint32_t groups_dropped() const noexcept { return n_dropped_; }
  std::uint32_t groups_abandoned() const noexcept { return n_abandoned_; }
  std::uint32_t writes_refused() const noexcept { return n_refused_; }
  // The clock of the io table: milliseconds of uptime, wrapping at 2^32 (0 without a clock).
  std::uint32_t host_now_ms() const noexcept;
  std::uint32_t host_midi_read(o3m_midi_event* out, std::uint32_t max) noexcept;
  void host_midi_lost(std::uint32_t* overflow, std::uint32_t* sysex) const noexcept;
  std::uint32_t host_launch_ctx(o3m_launch_ctx* out) const noexcept;
  int host_tap_select(std::uint32_t source) noexcept;
  std::uint32_t host_tap_latest(std::int16_t* out, std::uint32_t frames) noexcept;
  std::uint32_t host_tap_stream(std::int16_t* out, std::uint32_t max_frames, std::uint32_t* lost_frames) noexcept;
  void host_tap_levels(o3m_tap_levels* out) noexcept;
  // OS-APPUX S3 (ABI 1.10). get_transport: the play position as the sequencer published it (0, or 1 without one). set_own_param: the app stores its own declared row (clamped, rounded for an INT row, kept with
  // the project by app_params_sync, NO echo to its set_param). play_note / stop_note: an O3M_PLAY_* code; at most O3M_PLAY_MAX_HELD notes held and O3M_PLAY_MAX_PER_FRAME calls that act per pass, every
  // note ended when the app's host is released (every exit, a stop, a project change).
  std::int32_t host_get_transport(o3m_transport* out) const noexcept;
  std::int32_t host_set_own_param(std::uint32_t index, float value) noexcept;
  std::int32_t host_play_note(std::uint32_t instrument, std::uint32_t note, std::uint32_t velocity) noexcept;
  std::int32_t host_stop_note(std::uint32_t instrument) noexcept;
  // The notes the running app holds, for the tests (instrument 1..64 and note, or 0 for a free slot).
  unsigned held_notes() const noexcept {
    unsigned n = 0;
    for (const HeldNote& h : held_) {
      n += h.instrument != 0u ? 1u : 0u;
    }
    return n;
  }
  // OS-H4: the export entries of ABI 1.7 (O3M_EXPORT_* codes; without an export service open answers NO_CARD).
  std::int32_t host_export_open(const char* name, const char* ext) noexcept;
  std::int32_t host_export_write(const void* buf, std::uint32_t n) noexcept;
  std::int32_t host_export_close() noexcept;
  std::uint32_t host_export_status(char* final_name, std::uint32_t cap) noexcept;

  // What the last pick() / launch_app() loader run said (module::Status::Ok when it did not fail in the file).
  module::Status last_status() const noexcept { return last_status_; }
  // The apps' time: the number of views drawn (for the tests and `diag`).
  std::uint32_t views_drawn() const noexcept { return drawn_; }

 private:
  static constexpr unsigned o3m_params_max = O3M_PARAMS_MAX;

  void scan(const char* dir, unsigned kind, CatalogEntry* out, unsigned& count, const model::Project* project) noexcept;
  void classify(CatalogEntry& e) const noexcept;
  bool load(const char* stem) noexcept;  // loads `stem` into the set unless it is there; false: not available (last_status_ says why)
  void fault(int set_index, module::ViewStatus verdict) noexcept;
  // OS-H3A: every call into the app's code is bracketed (begin_call / end_call): end_call closes a group the app left open and lets the next call make a new one. Edits are allowed in an Input call only.
  enum class Call : std::uint8_t { None, Init, Enter, Input, Draw, SetParam, Exit, Keys };
  void refresh_keys() noexcept;  // OS-KEYROWDLG S5: asks the app's keys(); ViewHost::call_keys copies the 16 labels into keys_ inside the call (validated: printable ASCII, at most 7 characters)
  void begin_call(Call kind) noexcept;
  void end_call(bool faulted = false) noexcept;
  std::int32_t close_group() noexcept;  // edit_end's work (the app's call or the OS's): applies the group through the sink
  std::int32_t read_range_ok(std::uint16_t pattern, std::uint8_t track, std::uint8_t first, std::uint8_t count, const model::Pattern*& pat) const noexcept;
  void release_host() noexcept;  // the running app will not be called again (exit, stop, reset): its host calls end and the MIDI ring is switched off
  bool edit_acked(const char* stem, std::uint32_t need) const noexcept;  // the user confirmed this stem for every bit of `need` (a file that gained a bit since asks again)
  void ack_caps(const char* stem, std::uint32_t need) noexcept;
  bool quarantined_for_newer_abi(const char* stem) const noexcept;  // OS-H3A: the loader refused the file because its ABI minor is above this OS's
  void replay(ui::Renderer& r, const ui::Rect& area) noexcept;
  void arm_tap_samples() noexcept;  // the first sample read of the app: the render starts storing; nothing older than two blocks after this call is returned

  module::ModuleSet* set_ = nullptr;
  module::ModuleLoader* loader_ = nullptr;
  audio::ModuleVoices* voices_ = nullptr;
  std::uint64_t diag_slots_ = 0;  // OS-BUGS4: the slots a console bind opened (main loop only; model::kMaxInstruments is 64)
  hal::storage::Storage* card_ = nullptr;
  module::ModuleLoader::SyncFn sync_ = nullptr;
  const hal::Clock* clock_ = nullptr;
  module::ViewHost views_;
  CatalogEntry inst_[kModuleCatalogMax] = {};
  unsigned inst_count_ = 0;
  CatalogEntry apps_[kModuleCatalogMax] = {};
  unsigned app_count_ = 0;
  module::Status last_status_ = module::Status::Ok;
  std::uint32_t refused_caps_ = 0;  // with last_status_ CapUnsupported: the capability word of the refused file
  std::uint32_t drawn_ = 0;
  std::uint32_t view_words_ = 0;  // the estimated words of the views drawn since begin_view_frame()
  // the app
  bool app_running_ = false;
  bool card_on_computer_ = false;  // OS-USBDISK 4.3 (App::set_card_on_computer)
  bool app_stopped_ = false;
  module::ViewStatus app_verdict_ = module::ViewStatus::Ok;
  int app_index_ = -1;  // in the set
  void* app_state_ = nullptr;
  std::uint32_t app_state_bytes_ = 0;  // the size of the block app_state_ points to (OS-FU1: one block for every app, regrown only when an app needs more)
  char app_stem_[module::kStemMax + 1] = {};
  std::uint8_t app_minor_ = 0;  // the ABI minor of the running app's file
  float app_values_[O3M_PARAMS_MAX] = {};
  // OS-KEYROWDLG S5 (ABI 1.9): the running app's 16 labels (F1..F8, then Shift + F1..F8), 7 characters and a NUL each: 128 bytes in this object (SDRAM). Main loop only.
  module::ViewHost::KeyLabels keys_ = {};  // the 16 labels as the OS copied them (ViewHost::call_keys)
  bool keys_declared_ = false;
  // OS-H1: the running app's host table and what its calls need.
  o3m_host app_host_{};
  o3m_io_api io_{};
  const model::Project* project_ = nullptr;
  o3m_launch_ctx launch_ctx_{};
  std::uint32_t tempo_req_ = 0;  // a queued set_tempo (BPM x 100), 0 = none
  // OS-H3A: the edit group. ops_ and pool_ are not cleared (only what g_ops_ / g_steps_ count is read). One op per write call, its steps are the next `count` of the pool. The group is applied from
  // the buffer at edit_end and then forgotten; closed_ is true only while the sink runs the action.
  // pattern 0 marks a PARAMETER op (OS-H3B): `track` is the instrument slot 0..63 and its one pool slot holds a ParamOp.
  struct EditOp {
    std::uint8_t pattern, track, first, count, fields;
  };
  static_assert(sizeof(EditOp) == 5, "no padding");
  struct ParamOp {
    float value;
    std::uint16_t field;
    std::uint8_t pad[2];
  };
  static_assert(sizeof(ParamOp) == sizeof(o3m_step), "a parameter op takes one slot of the step pool");
  EditOp ops_[O3M_EDIT_MAX_STEPS];
  o3m_step pool_[O3M_EDIT_MAX_STEPS];
  std::uint16_t g_ops_ = 0, g_steps_ = 0, g_calls_ = 0, g_params_ = 0;
  bool g_open_ = false, g_over_ = false, g_tempo_set_ = false, closed_ = false;
  std::uint32_t g_tempo_ = 0;
  Call call_ = Call::None;
  bool abandon_logged_ = false;  // the first abandoned group of a launch is logged, the rest only counted
  bool group_in_call_ = false;  // a group was already made in the running call (the rate: one per call)
  int app_entry_ = -1;          // the running app's catalog entry (the unit's argument)
  EditSink sink_ = nullptr;
  void* sink_ctx_ = nullptr;
  std::int32_t apply_result_ = O3M_EDIT_OK;
  std::uint32_t pass_us_ = 0;
  std::uint32_t n_applied_ = 0, n_dropped_ = 0, n_abandoned_ = 0, n_refused_ = 0;
  unsigned tool_count_ = 0;
  std::uint8_t tools_[kModuleCatalogMax] = {};
  midi::MidiMonitor* midi_ = nullptr;
  bool midi_on_ = false;               // the running app holds midi_in and the monitor was switched on for it
  std::uint32_t midi_overflow0_ = 0;   // the monitor's counters when the app started: midi_lost reports the difference
  std::uint32_t midi_sysex0_ = 0;
  // OS-H2a: the audio tap. tap_on_: the running app holds audio_tap. A select sets tap_fresh_ to two blocks after the ring's head; the stream and levels cursors never read before it.
  audio::AppTap* tap_ = nullptr;
  bool tap_on_ = false;
  // OS-H4: the export service for the apps that hold export_files; export_on_: the running app does (its file count began, release_host ends it).
  AppExport* export_ = nullptr;
  // OS-APPUX S3: the play position, the live-note path and what the running app holds. A held note is a slot 0..3 (the key of its live note, source kLiveSourceApp); triggers_ counts the calls that acted this pass.
  GroupExtent extent_;
  const audio::PlayPosition* position_ = nullptr;
  PreviewSink* notes_ = nullptr;
  struct HeldNote {
    std::uint8_t instrument, note;  // instrument 1..64; 0 = the slot is free
  };
  HeldNote held_[O3M_PLAY_MAX_HELD] = {};
  std::uint8_t triggers_ = 0;
  bool play_on_ = false;  // the running app holds play_notes (its file asked and this OS grants it)
  void stop_app_notes() noexcept;
  void forget_ended_notes() noexcept;  // the sink holds no live note any more (a flush, the export, reset_input ended them): held_ is emptied
  bool set_param_value(const o3m_module* m, unsigned index, float value, bool echo) noexcept;
  bool export_on_ = false;
  std::uint32_t tap_fresh_ = 0;
  std::uint32_t tap_stream_cur_ = 0;
  std::uint32_t tap_levels_cur_ = 0;
  // the once-per-boot confirm of an app that holds edit_project: the stem asked about (a second launch of it goes through) and the stems already confirmed (a ring of kEditAckMax)
  static constexpr unsigned kEditAckMax = 4;
  char confirm_stem_[module::kStemMax + 1] = {};
  std::uint32_t confirm_caps_ = 0;  // the bits the last NeedsConfirm asked about (a second launch confirms only the same stem AND the same bits)
  char confirm_text_[kConfirmChars + 1] = {};  // what the last NeedsConfirm said (launch_message)
  char acked_[kEditAckMax][module::kStemMax + 1] = {};
  std::uint32_t acked_caps_[kEditAckMax] = {};  // per acknowledged stem: the confirm bits (edit_project, export_files) the user has been asked about
  unsigned acked_next_ = 0;
};

}  // namespace o3os::app
