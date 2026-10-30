// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/actions.h>
#include <o3os/app/bindings.h>
#include <o3os/app/buttons.h>
#include <o3os/app/dir_scan.h>
#include <o3os/app/import_job.h>
#include <o3os/app/midi_input.h>
#include <o3os/app/model.h>
#include <o3os/app/pad_lights.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/pattern_view.h>
#include <o3os/app/recorder.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/peak_job.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/sample_edit_job.h>
#include <o3os/app/governor_watch.h>
#include <o3os/app/slice_job.h>
#include <o3os/app/system_load.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/session.h>
#include <o3os/app/view_state.h>
#include <o3os/core/binding.h>
#include <o3os/core/byte_io.h>
#include <o3os/core/gesture.h>
#include <o3os/core/input_event.h>
#include <o3os/core/keymap.h>
#include <o3os/core/tick.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/hal/clock.h>
#include <o3os/hal/display.h>
#include <o3os/hal/firmware.h>
#include <o3os/hal/input.h>
#include <o3os/hal/storage.h>
#include <o3os/midi/cc_in.h>
#include <o3os/formats/format.h>
#include <o3os/model/editor.h>
#include <o3os/ui/plot/peak_cache.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// OS-SDSWAP: what the platform tells the app about the card slot (App::card_changed).
enum class CardEvent : std::uint8_t {
  Removed,      // the card was taken out and the volume released: every card call answers NotMounted
  Reading,      // a card went in; the platform is about to initialise and mount it (it blocks for a moment: the strip says so first)
  Ready,        // it is mounted
  NotReadable,  // it is in the slot and did not mount (no FAT volume, a card that does not answer)
};

class SaveService;

// The application: one base screen (the registry in screens/screen.h: Pattern and Song today) with at most one overlay
// over it (Learn, the Navigator, or the notification strip), over static demo data. It talks to the world only through
// the HAL contracts, so the same object runs in the simulator and (later) on the device. No heap: the display-list
// buffer, the demo model, the two keymap pools and the File screen's listing are members (about 26 KB).
//
//   Input path: HAL event -> gesture (core/gesture.h) -> binding (core/binding.h, bindings.h) -> action
//   (actions.h). The app holds no key logic of its own: its keys are bindings.cpp's table, grouped by context
//   (overlay, mode, screen, global), and what they do is in the feature files. Releases are ignored except the one that
//   ends a hold (GestureKind::HoldEnd: the choosers' and the step popup's "let go to pick").
//   Keymaps loaded with load_user_keymap / load_preset_keymap change what the gestures do.
//
// Frames are drawn only when something changed. There is no wall-clock or animation: the only timed element is the
// notification at the head of the queue, whose expiry is measured with hal::Clock::uptime_ms while it is shown.
//
// Undo and jobs (OS-002c). The App edits no project of its own yet (the screens still draw static demo data, OS-004 replaces
// it): a document is attached from outside, `attach_document(editor)`, and then the undoable actions edit it and F8 / Shift + F8
// undo and redo through the Project's own mutators. Jobs (core/job.h) run from run_once() with a budget of kJobBudgetUs per
// pass; `start_import` starts a reader of the format registry (the WAV importer, the OG files, a native project) as one, and its
// progress is the "IMPORT KICK.WAV 45%" strip of the notification queue. A sample import is chained to a second job, the
// peak cache of the new sample, if the request gives storage for it. Whatever an import changed in the attached project
// ends the undo history (a notice says so): the readers do not go through the editor.
//
// The sample screens (OS-011a) share the sample memory (`attach_samples`, sample_bank.h): the PCM of each instrument slot in the arena, the load of a WAV from
// the card (Sample load: the import job of OS-002c, which the App starts when the screen's load action asks, then the peak cache of the new sample), and the
// handover of changed instruments to the voices, polled once a pass: the bank applies it when playback is stopped and the voices are quiet.
//
// The File screen (OS-010a) browses the card: `attach_storage` gives the App the card (read only, hal/storage.h) and, while the File screen is in
// view, its pass starts a DirScan of Projects/ as a job (quiet: the screen shows the scan, the notification strip does not), keeps the cursor on
// its entry while the listing grows, and tells the user how a project open ended (`attach_project_sink`, the device's load).
class App {
 public:
  static constexpr std::uint32_t kMessageMs = ViewState::kMessageMs;
  // F1..F8 of the pattern screen (docs/design/product-ux.md section 4); each screen owns its own (Screen::soft_keys).
  static constexpr const char* const* kSoftKeyLabels = kPatternSoftKeys;
  static constexpr unsigned kMaxEventsPerPass = core::kInputQueueCapacity;  // bounds one pass of the loop
  static constexpr std::uint32_t kJobBudgetUs = 2000;   // what the running jobs share in one pass of the loop (core/job.h)
  static constexpr std::uint32_t kProgressMs = 1500;    // a progress strip stays up this long without news (it is refreshed every pass)

  // `dl_capacity_words` (at most eve::kMaxDisplayListWords) is the size of the frame buffer the screen is built
  // in; the default is the whole RAM_DL. A smaller value exists so tests can make a frame overflow.
  App(hal::Input& input, const hal::Clock& clock, hal::Display& display,
      std::size_t dl_capacity_words = eve::kMaxDisplayListWords) noexcept;

  App(const App&) = delete;
  App& operator=(const App&) = delete;
  // A scan in progress holds one of the card's few directory handles: it is given back (the platform builds the App again after a display recovery). So is the live take's undo group (OS-LIVEREC): the
  // editor outlives the App, and a group left open would keep its depth at 1, so that Undo said Busy until a reboot.
  ~App() noexcept {
    end_take();
    files_.abandon();
  }

  // One pass of the main loop: drains at most kMaxEventsPerPass input events, lets the gesture timers run
  // (only once the queue has been seen empty, so that time never overtakes an event still waiting), lets the
  // notification at the head of the queue run down, and redraws if anything changed. If the sink is busy the redraw is
  // retried on the next pass.
  void run_once() noexcept;

  // Forgets every button the gesture layer believes is held and every pending hold / double-tap / chord wait.
  // run_once() does it by itself when the context chain changes (a new screen or mode, an overlay opening or closing),
  // sparing only the buttons whose hold has fired and that are still down, so the release that ends the hold (a chooser's
  // "let go to pick") is still seen; call this one also whenever input may have been lost.
  void reset_input() noexcept;

  // Runs an action by id as a script or menu would, without a gesture or binding: scenarios (`action <name>`)
  // and tests. The screen is redrawn if anything visible changed.
  DispatchResult invoke(core::ActionId id, core::ActionArg arg = 0, bool repeat = false) noexcept;  // `repeat`: a held button's auto-repeat (OS-DELREPEAT; dispatch runs it only for a kActionRepeatable action)

  // Replace the user / preset binding layer from keymap text (core/keymap.h; the caller reads the file, the app
  // does no I/O). A rejected keymap leaves that layer empty and the result says why; built-in bindings are
  // never touched.
  core::KeymapResult load_user_keymap(const char* text, std::size_t length) noexcept {
    return user_.load(text, length, keymap_names());
  }
  core::KeymapResult load_preset_keymap(const char* text, std::size_t length) noexcept {
    return preset_.load(text, length, keymap_names());
  }
  core::BindingSet bindings() const noexcept {
    return core::BindingSet{{user_.view(), preset_.view(), builtin_bindings()}};
  }

  // The colour theme the screen is drawn in (Amiga at start). `ui.theme.next` (Shift + Master) cycles it; the
  // simulator's `--theme` and the scenario command `theme <name>` set it. The screen is redrawn if it changed.
  void set_theme(ui::ThemeId id) noexcept;  // (also the device setting THEME, which the Config screen shows)
  ui::ThemeId theme() const noexcept { return state_.theme; }

  // Attaches the document: the Editor over the project and its undo journal, owned by the caller (the platform provides the
  // arena and the journal region); it must outlive the App (~App closes the live take's undo group on it). nullptr detaches it. Without one the undoable actions are Disabled and undo says "nothing to
  // undo".
  void attach_document(model::Editor* editor) noexcept {
    end_take();  // a take that was open on the old document ends with it (OS-LIVEREC)
    session_.attach(editor);
    model_.project = editor != nullptr ? &editor->project() : nullptr;  // the parameter screens and the Pattern screen read it (Model::project)
    state_.song.ask = SongAsk{};  // a question about the old document is over (OS-SONGMAP)
    sync_pattern();
    dirty_ = true;
    watch_document();
  }
  // The platform replaced what is in the document (a project loaded, or File F3 made a new one, OS-007o): the save layer takes it as the project that is "on the card or fresh" (the
  // baseline of "unsaved changes") and forgets which folder SAVE replaced, so that the next SAVE never writes the old project's folder.
  // `unsaved`: the document is new work that is on no card (File > IMPORT): it counts as changed (F3 NEW, OPEN and another IMPORT ask first) until a project save ends well.
  void document_replaced(bool unsaved = false) noexcept;

  // The project-level state (OS-004d, app/project_state.h). document_replaced() applies the loaded project's mixer routing and Perform slots to the runtime (the engine is told by ParamChange);
  // from then on the pass copies every change of the mixer board, the Perform bank and the running SD app's parameters into the project (not undoable: the undo history stays), except while a save
  // is writing it. The platform does the same for MIDI and USB with project_state_writable(): it may write those parts of the project only while this is true.
  bool project_state_writable() const noexcept;

  // A project was loaded: its Pattern screen shows the pattern the OG project was saved on. A pattern the project does not have leaves the view as it is. True when the view moved to it.
  bool open_pattern(unsigned id) noexcept;

  // The engine's live notes (OS-005g, PreviewSink): the pads in Browse and Chop, the preview, the MIDI keyboard. nullptr detaches it.
  void attach_preview(PreviewSink* sink) noexcept {
    preview_ = sink;
    wire_app_audio();
  }

  // The sample memory (OS-011a): what the sample screens read and the Sample load screen puts a WAV into; nullptr detaches it (the load action then says so).
  // It must outlive the App.
  void attach_samples(SampleBank* bank) noexcept {
    samples_ = bank;
    model_.samples = bank;
    dirty_ = true;
  }

  // The LEDs of the pads that the screen in view wants lit (Chop: the pads that have a slice, the selected one brighter): false when it has no opinion, then
  // `roles` is untouched and the platform's own policy stands. OS-011a.
  bool pad_lights(PadRole (&roles)[48]) const noexcept;

  // The card (OS-010a): what the File screen lists. Read only; nullptr detaches it (the screen then says there is no card). It must outlive the App.
  void attach_storage(hal::storage::Storage* storage) noexcept {
    storage_ = storage;
    model_.card = storage;  // (the Config screen's SD page reads it)
    dirty_ = true;
  }
  // The audio render's cycles, for the status strip's CPU (OS-FU3, system_load.h); nullptr (the simulator) shows "--". It must outlive the App. MEM is the sample bank's own fill.
  void attach_audio_load(const AudioLoadSource* source) noexcept { audio_load_ = source; }
  // What the Config screen's Info page says the product and the build are (PAR-116); the strings must outlive the App. OS-011a.
  void set_build_info(const char* product, const char* build_id) noexcept {
    model_.product = product != nullptr ? product : "";
    model_.build_id = build_id != nullptr ? build_id : "";
    dirty_ = true;
  }
  // The mixer's runtime side (OS-011b, mix_board.h): the routing the Mixer screen edits, the meters' ballistics, the stem map. Placed by the platform (SDRAM on the device); the App reads
  // the engine's meters into it once a pass while the Mixer screen is in view. nullptr detaches it (the pages then show the defaults and no meters). It must outlive the App.
  void attach_mix(MixBoard* board) noexcept {
    mix_ = board;
    model_.mix = board;
    dirty_ = true;
  }

  // The Perform screen's slots (OS-011b, perform_board.h, seq/perform.h): the bank and the sink its commands go to (the sequencer's command ring on the device). The App applies the
  // bank to it once a pass (a command the ring refused goes out at the next one), ends every FX when the user leaves the screen for any but the Master and Song screens
  // and sends a pad's release to it. nullptr detaches it (the screen then shows the default slots and tells no engine). It must outlive the App.
  void attach_perform(PerformBoard* board) noexcept {
    perform_ = board;
    model_.perform = board;
    dirty_ = true;
  }

  // The sample recorder (OS-011c, recorder.h): the model behind the Record screen. The App polls it once a frame (it owns the engine's input path while the screen is in view
  // or a take is armed), reads the engine's meters into the mixer board while the screen is in view, and saves a take into the instrument slot the user is on. The platform
  // wires the recorder (the engine's graph and parameter ring, the sample bank, the codec and radio) before attaching it; nullptr detaches it. It must outlive the App.
  void attach_recorder(Recorder* recorder) noexcept {
    model_.rec = recorder;  // (the one pointer: the screens read through it, the App writes through recorder(); SRAM_U has no room for a second)
    dirty_ = true;
  }

  // The card's save layer (OS-007n, save_service.h): File's SAVE and SAVE AS, the WAV saves, and the device settings' own save (polled once a pass). nullptr detaches it (the actions
  // then say there is no card write layer). It must outlive the App.
  void attach_saves(SaveService* saves) noexcept {
    saves_ = saves;
    watch_document();
    wire_app_export();
  }

  // The firmware update path (OS-FWCONFIG, hal/firmware.h): what F8 UPDATE on Config's INFO page asks for. nullptr detaches it (the key then does nothing and the INFO page labels none). The App never
  // names the card's writer, the pin or the hand-over: the port is the whole contract. It must outlive the App.
  void attach_firmware(hal::firmware::FirmwareSink* sink) noexcept {
    firmware_ = sink;
    model_.firmware = sink != nullptr && sink->available();
    dirty_ = true;
  }

  // The device's project load (OS-010a): what File's open action hands a folder to. nullptr detaches it (the action is then Disabled).
  void attach_project_sink(ProjectSink* sink) noexcept { project_ = sink; }
  // OS-QAFIX-E-FILE-10: the project the unit opens at power-on (the platform names it: boot_recent_project). Starts the load as File's F1 does and tells its end on the strip. False: nothing started.
  bool boot_open(const char* folder, bool native) noexcept;

  // The SD modules' user interface (OS-012b, app/module_ui.h): the Instrument page's module controls and views, the engine picker (F1), the MODULES screen and an app's page. The App
  // draws them, routes an app its input and keeps its page redrawn (30 times a second); the platform owns the object and feeds it the loader, the voices and the clock. nullptr detaches it
  // (the screens then show no modules). It must outlive the App.
  void attach_modules(ModuleUi* modules) noexcept;  // (OS-H3A: also gives it the sink that applies an app's edit group)

  // The engine's transport (OS-005d): what the Play button asks to play or stop. nullptr detaches it (the action is then Disabled).
  void attach_transport(TransportSink* sink) noexcept { transport_ = sink; }
  // Where the sequencer is (OS-009b): the lock-free word the render publishes at every block (audio/play_position.h). The App reads it once a pass; the Pattern
  // screen shows the step of the pattern that plays as a bar, Browse takes the cursor along, the strip says the tempo the sequencer uses. nullptr detaches it.
  void attach_play_position(const audio::PlayPosition* position) noexcept {
    play_position_ = position;
    state_.play = audio::PlayPosition::Value{};
    wire_app_audio();
    dirty_ = true;
  }
  // The platform stopped the transport behind the app's back (a project load stops it, OS-007f): the badge says STOP, so the next Play starts.
  void transport_stopped() noexcept {
    state_.playing = false;
    dirty_ = true;
  }

  // OS-SDSWAP, a card taken out or put in while the unit runs: the strip says so (CARD REMOVED, READING CARD, CARD READY, CARD NOT READABLE) and the open browser (File, Sample
  // load) forgets its listing, which was read from a card that is gone, and reads the folder again (a card that is not mounted lists as NO SD CARD). Main loop only.
  void card_changed(CardEvent event) noexcept;
  // OS-USBDISK U1c, the boot disk: a message for the strip that the platform keeps alive (rewritten in place, source "disk"); the unit has no other screen for it. `warn`: the warning
  // severity (the writable disk's "the computer may still be writing"). Main loop only.
  void disk_notice(const char* text, std::uint32_t ttl_ms, bool warn = false) noexcept;
  // OS-SCREENSHOT: the strip's word for Shift + Power (NO SCREENSHOT: <why>, SCREENSHOT SAVED: <name>, SCREENSHOT FAILED), source "shot", rewritten in place. Main loop only.
  void shot_notice(const char* text, std::uint32_t ttl_ms, bool warn = false) noexcept;
  // OS-KEYROWDLG S3: the boot disk's key row (every key RESTART, AGAIN? while the computer may still be writing, blank while the restart waits); Off in every other mode. The platform tells it with the
  // strip, once a pass (a change is a redraw, no change costs a compare). Main loop only.
  void set_disk_keys(DiskKeys keys) noexcept;
  // OS-USBDISK 4.3: whether the computer has the card (CardOwner::Host), told by the platform once a pass. A change throws the listings away as a card change does (they were read from a card the unit no longer has); the way back is card_changed(Ready). Main loop only.
  void set_card_on_computer(bool on) noexcept;
  // The persistent strip while the card is out (the platform rewrites it each second, source "disk"; the simulator's `disk out` posts it once). 39 characters: one strip line.
  static constexpr const char* kCardOnComputerStrip = "CARD ON COMPUTER - CONFIG USB: F7 EJECT";
  // OS-USBDISK U2b, the Config > USB page's CARD AS DISK row: the platform tells the hand-over's phase, whether a claim holds the card, whether USB AUDIO is presented and whether the disk the computer has is writable once a
  // pass; the row's second F7 leaves a request (ShareReadOnly or ShareWritable, READ ONLY as it was at that press, or Eject) that the platform takes once and acts on. READ ONLY is the row's session
  // choice (Q4, RAM only) and does not change while the card is out (EJECT THE CARD TO CHANGE IT), changing hands or a request waits (THE CARD IS CHANGING HANDS: WAIT). Main loop only.
  void set_disk_state(ViewState::DiskView::Phase phase, bool claims_busy, bool audio, bool writable) noexcept;
  ViewState::DiskView::Request take_disk_request() noexcept;
  bool disk_read_only() const noexcept { return state_.disk.read_only; }
  // OS-USBDISK F3: a job of the app is running (the File / Sample load listing, the audition's reader, an import, an export): it may read the card on its next pass, so the card must
  // not be handed to a computer meanwhile. Conservative: any running job of the runner counts.
  bool card_jobs_busy() const noexcept { return session_.jobs.running() != 0u; }

  // ---- OS-POWER P1 (docs/specs/OS-POWER.md): the hold of the Power switch and the shutdown ----
  // The line and the strip text, told once a pass by the platform (or the simulator) from core::PowerHold and the shutdown (platform/mk66/power). Drawn after the overlays (screens/overlay.h render_power).
  // The Hold text becomes UNSAVED WORK - LET GO TO KEEP IT while the project has changes no save took or a recorded take is not saved. The shutdown never saves a project (D3). Main loop only.
  void set_power(const core::PowerShow& show) noexcept;
  // Whether the off would lose work: the project differs from the last save (SaveService::dirty), or a take (Recording, Review) exists. A stage-0 question and the Hold text's choice.
  bool unsaved_work() noexcept;
  // Stage 0: a WAV export, a set of stems, a Pattern selection or an OG project export runs (or is armed and waits for the transport to stop). export_stop() is the second hold's answer: the export's
  // normal cancel (finished files stay, no project.mt for the OG set, no file under a final name for a WAV).
  bool export_running() noexcept;
  void export_stop() noexcept;
  // Stage A: the shutdown has begun. The app's jobs that read the card (a listing, an audition, an import, a peak build) are cancelled; from here run_once() does only what finishes a save and draws the
  // strip (no input is read, no screen polls, nothing new starts), and the save layer takes no new job but the device settings' flush (SaveService::power_begin).
  void power_begin() noexcept;
  bool powering_off() const noexcept { return power_down_; }

  // OS-FILES-C C2a, the console's `ogexp`: the project in the document is written in the OG format into Export/<project> OG[ n] on the card (save_service.h "OG EXPORT"; the patterns, their names file and
  // project.mt; no instrument yet). nullptr: it started (one job a pass; SaveService::og_state() says how it goes). Otherwise why it did not, in a few upper-case words: no write layer, a recording or a
  // take, the transport running, a job running (a sample edit, a peak build, an import, a listing), a load, the card's own refusals. Main loop only.
  const char* start_og_export() noexcept;
  void cancel_og_export() noexcept;  // no file after the one in progress

  // The ring of ParamChange to the audio side (OS-002d, ADR 0009 rev 1): every parameter an action changes (an edit, a MIDI controller
  // moving a Learn binding, an undo) is pushed to it as {ParamId, value, ramp}. The caller owns the ring: the engine's (audio::Engine::params()),
  // a test's fake consumer. nullptr detaches it. The main loop is its one producer; a full ring drops the push and counts it.
  void attach_param_ring(core::ParamRing* ring) noexcept { params_out_ = ring; }
  void attach_scratch(audio::MasterScratch* stage) noexcept { state_.dj.scratch = stage; }  // OS-DJ C1: the engine's scratch stage (null: the page says so)

  // MIDI Learn's input (OS-013a): takes at most kMidiCcPerPass control changes from `in` (the render's ring, midi/cc_in.h; this is its one consumer), drops those the device
  // settings MIDI IN and IN CHANNEL exclude and runs `midi.cc` for the rest: with the Learn overlay up the first one binds the target, otherwise they move what is bound
  // (main loop only: the action writes through the editor and the ParamChange ring). Returns how many changed something.
  unsigned poll_midi_cc(midi::CcInput& in) noexcept;
  // MIDI notes in (OS-005g): takes at most kMidiNotesPerPass note messages from `in`'s notes ring (the same producer and consumer as the control changes), drops those the settings
  // MIDI IN and IN CHANNEL exclude and hands the rest to the PreviewSink (midi_note / notes_off) with the selected track and the last used instrument: the keyboard plays
  // the selected track's instrument. A note message that was dropped on the way (a full ring) silences the keyboard's notes. Main loop only.
  // Returns how many notes were handed on. In Edit on the Pattern screen a note on also goes into the cursor step (`step.midi_note`, OS-FU1, no cursor advance); while the pattern plays in Edit it is recorded instead (a note at the playhead's step on the cursor's track, `rec_note`, OS-LIVEREC).
  unsigned poll_midi_notes(midi::CcInput& in) noexcept;

  // The device's settings and Learn store (Model::device): the platform loads them from the settings file at start-up (formats/settings.h)
  // and saves them after a change (compare generations).
  model::Settings& device_settings() noexcept { return model_.device; }
  const model::Settings& device_settings() const noexcept { return model_.device; }

  // What an import needs: the format (from the registry; it must have a reader), the bytes, where the project is, and, for a
  // sample format, where the audio and its peak cache go. Everything must stay valid until the strip says DONE or FAILED.
  struct ImportRequest {
    const formats::FormatDesc* format = nullptr;
    core::ByteSource* source = nullptr;
    model::ModelArena* arena = nullptr;  // the project the reader writes into: the attached document's, normally
    const char* label = "";              // the file's name, for the strip
    formats::ReadParams params;          // the slot or pattern id, the size if known, and for a sample: name, pcm, keep_stereo
    // A sample import: storage for the peak cache of the imported audio (PeakCache::elements_required(frames, channels)
    // int16 elements, word aligned); the peak job is chained after the import. nullptr: no peaks.
    std::int16_t* peak_storage = nullptr;
    std::size_t peak_elems = 0;
  };
  // Starts the import as a job. False, with a notification, when the request is incomplete, an import (or the peak job of the
  // last one) is still running, or no job slot is free.
  bool start_import(const ImportRequest& request) noexcept;

  // Observation (tests, simulator).
  Session& session() noexcept { return session_; }
  const Session& session() const noexcept { return session_; }
  const ImportJob& import_job() const noexcept { return import_job_; }
  const PeakJob& peak_job() const noexcept { return peak_job_; }
  // The peak cache of the last sample imported with peak storage (built when the strip says PEAKS ... DONE).
  const ui::plot::PeakCache& peak_cache() const noexcept { return peak_cache_; }
  const PatternView& view() const noexcept { return state_.pattern; }  // the pattern screen's cursor
  const ViewState& state() const noexcept { return state_; }
  EncAccel& enc_accel() noexcept { return state_.enc_accel; }  // OS-AUDIT-28: the console's `enc accel` reads and sets the curve's numbers
  const Model& model() const noexcept { return model_; }
  const PatternData& data() const noexcept { return model_.pattern; }
  PatternData& data() noexcept { return model_.pattern; }
  int instrument() const noexcept { return state_.instrument; }
  bool learn() const noexcept { return state_.overlay == OverlayId::Learn; }
  // The overlay on screen (the Notify strip when no modal overlay is up and the queue has something).
  OverlayId overlay() const noexcept { return state_.shown_overlay(); }
  const char* message() const noexcept { return state_.shown_message(); }  // the notification on screen, else ""
  std::uint32_t frames() const noexcept { return frames_; }              // frames committed to the display
  // Frames that did not fit (the display list overflowed or the sink refused it): dropped, never shown
  // truncated, the previous picture stays. The screen is redrawn on the next change, not retried in a loop.
  std::uint32_t failed_frames() const noexcept { return failed_frames_; }
  bool dirty() const noexcept { return dirty_; }

 private:
  void dispatch_gestures(const core::GestureList& gestures) noexcept;
  bool press_closes_overlay(const core::Gesture& gesture) const noexcept;  // OS-HELP: a captured press that closes the overlay in view (Overlay::closes_on_press)
  core::ContextChain context_chain(bool with_field = true) const noexcept;
  core::ContextChain sync_context() noexcept;
  const Screen& active_screen() const noexcept;
  void sync_pattern() noexcept;
  void poll_play_position() noexcept;
  void poll_count_in(const audio::PlayPosition::Value& v, unsigned old_count_in) noexcept;  // OS-METRO: the beat of the count-in (OS-METROALIGN: or of a take's wait for the bar), for the strip's countdown and the pads' flash
  // The live recording's take (OS-LIVEREC, rec_actions.cpp): one undo group from the moment the Pattern screen records until it stops. sync_take opens and closes it as the state says (after the
  // position is read and after every action); end_take closes it at once (a new document).
  void sync_take() noexcept;
  void end_take() noexcept;
  void poll_transport_select() noexcept;  // OS-MIDISLV: what an external Start plays is told to the sequencer while it is stopped (TransportSink::select)
  void poll_mute_blink() noexcept;  // a quantised mute waits for its boundary: the Pattern screen's MUTE tag blinks and is redrawn each half period (OS-MUTEVIS)
  void watch_document() noexcept;  // the save layer's baseline of "unsaved changes": the first document it sees
  void poll_files() noexcept;    // before the jobs: a rescan asked for, the scan to start, the result of an open
  void poll_export() noexcept;   // OS-WAVEXP: an armed export starts when the sequencer has stopped, the running one's progress goes to the overlay, the EXPORT page's file name is looked up
  void finish_export(unsigned slot, core::SlotState st) noexcept;  // OS-WAVEXP: the export's job ended: the overlay closes and the strip says how
  void poll_og() noexcept;       // OS-FILES-C C2c: a whole-project OG export (the EXPORT page's OG PROJECT row, or the console's) is followed: the overlay's files, the strip at its end
  void finish_export_idle() noexcept;  // OS-WAVEXP slice B: a set of stems that ended between two files (F8): the overlay closes and the strip says how many files are saved
  void poll_firmware() noexcept; // OS-FWCONFIG: the firmware update dialog's check and install are followed a slice a pass; a dialog that lost its overlay is cancelled
  void poll_wav() noexcept;      // Sample load's requests: a folder entered, left, a file loaded
  void poll_wave() noexcept;     // the Wave / Chop screen's requests: the AUTO slicer
  static std::int32_t apply_app_group(void* self, std::int32_t entry) noexcept;  // OS-H3A: ModuleUi's edit sink; runs module.app_group
  void wire_app_audio() noexcept;   // OS-APPUX S3: get_transport reads the play position and play_note goes to the live notes (once attached, in either order)
  void wire_app_export() noexcept;  // OS-H4: the module host's export calls go to the save service's AppExport (once both are attached, in either order)
  void poll_modules() noexcept;  // an app's page: its redraw rate, and the way out when the app is gone (the project changed under it)
  bool feed_module_app(const core::InputEvent& event) noexcept;  // a raw event to the running app (not the OS's Shift layer); true when the page changed
  void poll_theme() noexcept;    // the device setting THEME moved (Config, a Learn binding, the settings file): the screen's theme follows
  void poll_pad_offset() noexcept;  // the device setting PAD OFFSET moved (OS-PADOFFSET: Config, a Learn binding, the settings file): the NOTES board is laid out again
  void poll_peaks() noexcept;    // the waveform of the sample in view, built when it is not the cache's
  void finish_slice(core::SlotState state) noexcept;
  void poll_sedit() noexcept;  // the Sample edit screen's request: an effect, as a job into a spare buffer
  bool finish_edit(core::SlotState state) noexcept;  // true: it told the user there was nothing to do, which replaces the job's DONE
  void poll_samples() noexcept;  // the bank's handover to the voices, once a pass
  void sync_project_state() noexcept;   // runtime -> project: the mixer, the Perform slots, the running app's parameters (OS-004d), the session state (OS-PERSIST2)
  void apply_project_state() noexcept;  // project -> runtime, after a load: the mixer and the Perform slots, the session state (note entry, mute modes, scenes, send effects)
  void normalise_loaded_state() noexcept;  // a load's last step before the unsaved baseline: what the runtime held to its ranges goes back into the project (not an undo unit)
  void poll_dj() noexcept;       // the DJ page's crossfader to the engine, and the end of every DJ effect on leaving the page (OS-DJ A1)
  void poll_perform() noexcept;  // the Perform bank to the engine, and what leaving or entering the screen does (OS-011b)
  void poll_meters() noexcept;   // the engine's meters into the mixer board, while the Mixer, the Record or the Pattern screen is in view (OS-011b, OS-011c, OS-PVU)
  void poll_pattern_meters() noexcept;  // the Pattern screen's track headers: a frame only when a visible track's bar or tick moved (OS-PVU)
  void poll_recorder() noexcept; // the recorder, once a frame (OS-011c)
  Recorder* recorder() noexcept { return const_cast<Recorder*>(model_.rec); }  // attach_recorder() was given a non-const object
  bool start_wav_load(int pos) noexcept;  // the file at list row `pos` into WavView::slot; false when it did not start (it has said why)
  // OS-AUDIT-A7 (SLD-MULTI): the marked files of Sample load as a sequence of the same loads, one at a time (the next is started from poll_wav() once the import and its peak build have ended).
  void begin_wav_batch() noexcept;
  void poll_wav_batch() noexcept;
  void end_wav_batch(const char* why) noexcept;  // the summary on the strip ("LOADED 3 OF 5: why"), then the batch is over
  // OS-INSTUI S7, LOAD of an instrument file: F1 of Sample load in its .pti mode (WavView::pti). The bank holds the arena's tail and the file (open_wav) for the PtiLoadJob; when it is Done the
  // instrument goes into the slot through the Editor (one undo unit, "instrument.load") and the audio through SampleBank::commit_load_undoable.
  void start_pti_load() noexcept;
  void finish_pti_load(core::SlotState state) noexcept;
  // The Sample load audition (OS-FU2, audition.cpp). Wait: the quiet window (SampleBank::preview_quiet) is awaited; Load: the PreviewJob reads the file's first seconds; Bind: the
  // descriptor waits for the window again; Play: the live note is on (when the read gets enough ahead of the head the note starts while it still runs: Load -> Play, OS-BUG-PREVLAT). Every stop sends the key's release (PreviewSink::audition(false)) and cancels a read.
  enum class Aud : std::uint8_t { Idle, Wait, Load, Bind, Play };
  void start_audition() noexcept;
  void stop_audition() noexcept;
  void poll_audition() noexcept;
  void finish_audition(core::SlotState state) noexcept;
  bool audition_quiet() const noexcept;
  bool audition_target(int pos) const noexcept;
  void zero_audition_tail(std::uint32_t chunk) noexcept;
  // The held Preview key of the sample screens (OS-FU3, audition.cpp): WaitStart / PlayStart a live note of the instrument from its start point; WaitOver / PlayOver the audition's descriptor
  // over the sample from a frame (or the Record take's window), once the quiet window opens. The key's release (a raw event) or leaving the screen ends it.
  enum class Hold : std::uint8_t { Idle, WaitStart, PlayStart, WaitOver, PlayOver };
  void poll_hold() noexcept;
  void stop_hold() noexcept;
  void stop_step_preview() noexcept;  // F5 PREVIEW's note ends (OS-CFGWIRE)
  // The pads of the Record screen playing the take under review (OS-REC2, audition.cpp): binds the preview descriptor over the take's window when the quiet window opens and starts the notes of
  // the pads that wait; ends them when the screen is left or the take goes.
  void poll_take_pads() noexcept;
  void poll_head() noexcept;
  void follow_files() noexcept;  // after them: the cursor stays on its entry while the listing grows
  void start_files_scan() noexcept;
  void sync_recent() noexcept;  // OS-FILES part B: the PROJECTS page's RECENT group follows the list of recent projects and the listing; a renamed folder gets the cursor
  void stop_files_scan() noexcept;
  void shift_changed(bool held) noexcept;
  PatternContent content_;                    // the PATS pads' lit patterns (OS-PADS-B), polled once a pass while that mode is in force
  bool pads_mode_in_force() const noexcept;    // a fixed PIANO / ISO / ... mode owns the pads (OS-PADS-B): Pattern, no overlay, no lone Shift
  bool pads_tracks_in_force() const noexcept;  // the TRACKS layer owns the pads now (pad_layer.h): the screen is Pattern or Perform and no overlay is up
  void render() noexcept;
  void report_jobs() noexcept;
  void finish_import(core::SlotState state, bool& chained) noexcept;

  hal::Input& input_;
  const hal::Clock& clock_;
  eve::DisplayListBuffer dl_words_;  // the display list's words, explicitly 8-byte aligned (ADR 0011)
  eve::DisplayListBuilder builder_;  // over dl_words_.words, the first dl_capacity_words of it
  Model model_;
  ViewState state_;
  core::GestureRecognizer gestures_{kModifierButtons};
  std::uint64_t tap_pads_ = 0;  // OS-PATTEMPO: the pads taken as tempo taps and still down (bit n = pad n): their release is taken too
  TrackFlash flash_;  // the TRACKS pads' note flash (OS-PADS-A), polled once a pass
  core::ContextChain last_chain_;  // the chain the gesture layer last saw: a change resets it
  core::BindingPool<kUserBindings> user_;
  core::BindingPool<kPresetBindings> preset_;
  Session session_;
  core::ParamRing* params_out_ = nullptr;
  PreviewSink* preview_ = nullptr;
  std::uint32_t midi_note_drops_ = 0;  // midi::CcInput::note_dropped() at the last poll_midi_notes()
  TransportSink* transport_ = nullptr;
  ProjectSink* project_ = nullptr;
  SaveService* saves_ = nullptr;
  hal::firmware::FirmwareSink* firmware_ = nullptr;  // OS-FWCONFIG
  PatternClip clip_;  // OS-PATCOPY: the pattern clipboard (14 KB: the App is built in SDRAM on the device)
  SongClip song_clip_;  // OS-SONGMAP: the song clip, the Song map's cells (28 KB, SDRAM as the pattern clipboard)
  hal::storage::Storage* storage_ = nullptr;
  const audio::PlayPosition* play_position_ = nullptr;
  std::uint8_t select_pattern_ = 0;  // OS-MIDISLV: what the sequencer was last told an external Start plays (poll_transport_select)
  bool select_song_ = false;
  bool select_told_ = false;
  ImportJob import_job_;
  PeakJob peak_job_;
  ui::plot::PeakCache peak_cache_;
  model::ModelArena* import_arena_ = nullptr;  // what the import in progress writes into
  formats::FormatKind import_kind_ = formats::FormatKind::Project;
  const std::int16_t* import_pcm_ = nullptr;   // a sample import: the audio, and where the peak cache goes
  std::int16_t* peak_storage_ = nullptr;
  std::size_t peak_elems_ = 0;
  int import_slot_ = -1;  // the job slot of the import in progress, -1 when none
  int peak_slot_ = -1;    // the job slot of the peak build chained after it
  DirScan files_;         // the one listing of the browser screens (File: Projects/; Sample load: a folder of .wav files) and the job that fills it (about 9.5 KB)
  // Whose listing `files_` holds (the two screens are never both scanning: entering the other one starts its scan over).
  enum class FilesFor : std::uint8_t { None, Projects, Wavs, Modules };  // Modules: the File screen's IMPORT page (OS-FILES)
  FilesFor files_for_ = FilesFor::None;
  SampleBank* samples_ = nullptr;
  const AudioLoadSource* audio_load_ = nullptr;  // OS-FU3
  CpuMeter cpu_meter_;
  VoiceMeter voice_meter_;  // OS-VOX
  GovernorWatch gov_watch_;  // OS-GOV: the toast when the module governor steals, refuses or mutes (governor_watch.h)
  MixBoard* mix_ = nullptr;
  std::uint8_t pattern_bars_[PatternView::kMaxVisibleTracks][2] = {};  // the header meters (level, tick) of the visible tracks at the last frame asked for (OS-PVU)
  PerformBoard* perform_ = nullptr;
  ScreenId perform_screen_ = ScreenId::Pattern;  // the screen at the last poll_perform(): leaving Perform is seen as a change from it
  int pti_slot_ = -1;                // the job slot of the instrument-file load in progress, -1 when none
  unsigned pti_target_ = 0;          // the instrument slot it is for
  std::int16_t* pti_pcm_ = nullptr;  // where it reads the audio to (the arena's tail)
  bool files_pti_ = false;           // the listing in files_ was made with the .pti filter (WavView::pti)
  bool wav_synced_ = false;          // the document's undo history was in step when the sample import began (finish_import keeps it, but for the sound ring if it names the slot)
  bool rescan_say_ = false;          // OS-QAFIX-RESCAN: a RESCAN was asked; the strip tells the count when the scan has ended (main loop only)
  int wav_slot_ = -1;                // the instrument slot the sample import in progress is for, -1 when none
  // The multi-load in progress (main loop only): `next_slot` is where the search for a free instrument goes on from; `total` the files asked for, `loaded` those that came in whole, `failed` those that did not.
  struct WavBatch {
    bool active = false;
    std::uint16_t total = 0;
    std::uint16_t loaded = 0;
    std::uint16_t failed = 0;
    std::uint8_t next_slot = 0;
  };
  WavBatch wav_batch_;
  std::int16_t* wav_pcm_ = nullptr;  // where that import reads the audio to (the arena's tail)
  bool wav_side_ = false;            // OS-STEREO (b): the import was asked for the side signal as well (F8 CH STEREO): finish_import sets the instrument's flag when it came
  std::uint16_t mute_pending_seen_ = 0;  // the sequencer's pending-mute bits at the last poll_mute_blink()
  std::uint32_t theme_generation_ = 0;  // the generation of the device setting THEME at the last poll_theme()
  std::uint32_t pad_offset_generation_ = 0;  // ... of PAD OFFSET at the last poll_pad_offset()
  bool peaks_quiet_ = false;         // the peak build in progress was started on Params (its source visual): no strip text, a failure still says so (main loop only)
  bool peaks_after_batch_ = false;   // OS-AUDIT-A7: a multi-load just ended; the build poll_peaks starts for the sample in view in that pass is quiet, so that "LOADED n OF n" stays the last word (main loop only)
  bool peaks_superseded_ = false;   // the peak build in progress was cancelled by poll_peaks (the sample in view changed): its end is not news
  bool rebind_seen_ = false;         // the bank had a rebind pending at the last pass (the card says so)
  // The peak cache is the picture of one slot's PCM at one version of it (the bank's): what it was last built, or is being built, for.
  unsigned peaks_slot_ = SampleBank::kSlots;
  std::uint32_t peaks_version_ = 0;
  int edit_slot_ = -1;               // its job slot, -1 when it is not running
  unsigned edit_target_ = 0;         // the instrument slot it reads
  std::uint32_t edit_version_ = 0;   // the version of that sample it started on: a result for another one is thrown away
  std::int16_t* edit_dst_ = nullptr; // the buffer it writes
  EditOp edit_op_ = EditOp::Amplify;
  int slice_slot_ = -1;              // its job slot, -1 when it is not running
  unsigned slice_target_ = 0;        // the instrument slot it reads
  std::uint32_t slice_version_ = 0;  // the version of that sample it started on: an answer for another one is thrown away
  Aud aud_ = Aud::Idle;
  Hold hold_ = Hold::Idle;           // OS-FU3
  std::uint8_t hold_instrument_ = 1;  // the instrument a PlayStart note was started for (its release names it)
  std::uint64_t head_ms_ = 0;        // the playhead was last read
  int preview_slot_ = -1;            // the audition's read: its job slot
  int aud_pos_ = -1;                 // the listing entry it is for
  std::uint32_t aud_frames_ = 0;     // Bind: what was read, and the file's rate
  std::uint32_t aud_rate_ = 0;
  // OS-BUG-PREVLAT, the streamed start (main loop only, the job is the buffer's one writer): while the note plays and the read still runs, the buffer from the zeroed mark on up to
  // the frames planned holds zeros (a tail that was not read yet is silence, never the last preview's audio); the start waits for enough frames (poll_audition).
  std::uint32_t aud_zeroed_ = 0;     // samples of the buffer, from the start, that are read or zeroed
  std::uint64_t aud_t0_ms_ = 0;      // when the read began: its speed is frames written over this time
  std::uint64_t aud_zero_ms_ = 0;    // when the tail was last zeroed (main loop only): the next chunk grows with the time since, so a slow pass cannot let the head overtake the zeros
  bool og_watch_ = false;  // an OG export was started and its end has not been worded yet (poll_og)
  int files_slot_ = -1;   // its job slot, -1 when it is not in the runner
  std::uint32_t files_seen_ = 0;  // the listing's generation and state at the last follow_files(): a change is a redraw and a cursor to fit
  DirScan::State files_state_seen_ = DirScan::State::Idle;
  int shown_percent_[core::JobRunner::kSlots] = {-1, -1, -1, -1};
  bool power_down_ = false;  // OS-POWER P1: power_begin() was called; never cleared
  bool dirty_ = true;
  std::uint32_t frames_ = 0;
  std::uint32_t failed_frames_ = 0;
};

}  // namespace o3os::app
