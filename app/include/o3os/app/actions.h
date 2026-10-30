// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Actions (ADR 0009 rev 1, mechanism 1): every user function is a named action in one registry. Inputs reach
// them through gestures and bindings (core/binding.h), scenarios and, later, menus and MIDI call them by name or
// id; none of those calls feature code directly. A handler changes the model (Model) and the ViewState and
// says whether anything visible changed; screens draw from those and never switch on actions.
//
// Adding an action: write its handler in a feature file (nav_actions.cpp, screen_actions.cpp, pattern_actions.cpp,
// ui_actions.cpp), declare it in action_handlers.h, add one make_action() line to the registry in actions.cpp, bind it in
// bindings.cpp. The registry is a constexpr table checked at compile time (registry_valid) and by actions_test.
//
// The flags (core/action.h): kActionRepeatable: a held button's auto-repeats (ActionContext::repeat) reach only an action that has it, the press itself always runs (OS-DELREPEAT); dispatch() honours kActionNeedsFocus and `enabled`,
// and kActionUndoable (OS-002c): an undoable action is handed the document's editor (ActionContext::edit) and nothing else is,
// its edits are one undo unit (named by the action and its argument), and without a document it is Disabled. kActionUndoableIfOpen
// (OS-002d) is the same when a document is attached and is not Disabled when there is none: `edit` is then null and the action does
// only what needs no project (a device setting, a device Learn binding). dispatch() also feeds the macro recorder (session.h).

#include <o3os/app/mix_board.h>
#include <o3os/app/model.h>
#include <o3os/app/pattern_clip.h>
#include <o3os/app/song_clip.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/session.h>
#include <o3os/app/view_state.h>
#include <o3os/audio/voices.h>
#include <o3os/core/action.h>
#include <o3os/core/param.h>
#include <o3os/core/tick.h>
#include <o3os/hal/firmware.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// The engine's live notes (OS-005g, audio/live_notes.h): what a pad, a MIDI keyboard and a preview ask of the voices, from the main loop. The platform attaches one that puts
// events on the audio engine's live queue; the simulator and most tests attach none, or a fake that logs what was asked. Without one the pads change only what the screen shows
// (the last used instrument). preview() is the one a fake must have; the others do nothing by default, or (pad_down) fall back to it, so a fake that only knows preview() sees
// every pad press as before.
class PreviewSink {
 public:
  virtual void preview(unsigned instrument, unsigned note) noexcept = 0;  // instrument 1..64, note 0..127 (as the OG files number them): a one-shot with no release, the preview voice
  // A pad went down (the Pattern screen's pads in Browse, a Chop page's pads): it holds its note until pad_up(). `pad` 0..47 is the key of the release, `track` 0..15 the
  // selected track the voice search starts from. A beat slice instrument plays slice (note - 24): the Chop page's pads and, since OS-SLICEFIX, the Pattern pads ask for note 24 + n (app/pad_note.h).
  virtual void pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note) noexcept {
    (void)pad;
    (void)track;
    preview(instrument, note);
  }
  virtual void pad_up(unsigned pad) noexcept { (void)pad; }  // the pad's release (a raw input event: every pad release, whether or not it started a note)
  // A note from the MIDI keyboard, after the device settings MIDI IN and IN CHANNEL let it through: the selected track and the last used instrument play it. `velocity` 1..127, `channel` 1..16 (the one it came on:
  // named by the export's refusal while the note is held, nothing plays differently).
  virtual void midi_note(bool on, unsigned track, unsigned instrument, unsigned note, unsigned velocity, unsigned channel) noexcept {
    (void)channel;
    (void)on;
    (void)track;
    (void)instrument;
    (void)note;
    (void)velocity;
  }
  // Every live note ends (its release): the keyboard's only (`midi_only`: an all-notes-off or a lost message) or all of them (input was lost: App::reset_input()).
  virtual void notes_off(bool midi_only) noexcept { (void)midi_only; }
  // The Sample load audition (OS-FU2): the preview buffer (SampleBank::preview_bind, descriptor audio::kPreviewSlot) starts at its own pitch on a track found from `track` (on = true) or
  // is released (on = false). One at a time (the preview source's key 0). Nothing by default (a fake with no engine).
  virtual void audition(bool on, unsigned track) noexcept {
    (void)on;
    (void)track;
  }
  // The Preview key of the sample screens (OS-FU3: note 60 on at the press, off at the release): `note` of `instrument` plays from the
  // instrument's own start point until hold_note(false) (the platform's source kLiveSourcePreview, key 1). A fake with no engine plays a one shot.
  virtual void hold_note(bool on, unsigned track, unsigned instrument, unsigned note) noexcept {
    (void)track;
    if (on) {
      preview(instrument, note);
    }
  }
  // A pad of the Record screen plays the take under review (OS-REC2): a pad note (source kLiveSourcePad, key `pad`, ended by pad_up) of the preview descriptor, instrument kPreviewSlot + 1, which
  // the App has bound over the take's window (SampleBank::preview_bind_over) while the quiet window was open. Falls back to preview() on that instrument in a fake.
  virtual void take_pad_down(unsigned pad, unsigned track, unsigned note) noexcept {
    (void)pad;
    (void)track;
    preview(audio::kPreviewSlot + 1u, note);
  }
  // OS-APPUX S3 (ABI 1.10): a note an app plays (source kLiveSourceApp, key = the app's note slot 0..3), through the project's instruments like a pad; `on` false ends the slot's note. True when the
  // request went; a push that did not go (a full ring, no audio running) answers false and a sink with no engine behind it keeps the default, false: the app is told NO_AUDIO either way and holds nothing.
  virtual bool app_note(bool on, unsigned slot, unsigned instrument, unsigned note, unsigned velocity) noexcept {
    (void)on;
    (void)slot;
    (void)instrument;
    (void)note;
    (void)velocity;
    return false;
  }
  // Every note of the app ends (its page closed, or it was stopped): an AllOff of source kLiveSourceApp. Nothing by default.
  virtual void app_notes_off() noexcept {}
  // Where the voice of the last live note (pad, MIDI keyboard, Preview, audition) is: audio::VoicePool::live_position() (one lock-free word the render publishes). 0: none sounds.
  virtual std::uint32_t live_position() const noexcept { return 0; }
  // True when no live note is queued, sounding or held on behalf of this sink: the sample bank's handover (sample_bank.h) rewrites a slot's descriptor only then, because a
  // live note reads it in the render. The platform answers from the engine's queue and the render's published table; a fake with no notes keeps the default.
  virtual bool live_idle() const noexcept { return true; }

 protected:
  PreviewSink() = default;
  ~PreviewSink() = default;
};

// The engine's transport (OS-005d lite): the Play button asks it to play the pattern being edited, or on the Song screen the song, or to stop. The platform
// attaches one that puts a command on the sequencer's ring (the main loop is the ring's one producer); the simulator has none and the action is then Disabled.
// Each returns false when the request did not go (no audio path running, ring full): the screen then does not change.
class TransportSink {
 public:
  virtual bool play(unsigned pattern) noexcept = 0;  // pattern 1..255, from its first step; takes effect at the next audio block
  // OS-METRO: like play(), counting in `bars` (1 or 2) first: the transport starts that many bars before the first step and the pattern, and the click if the metronome is on, play them; the position
  // reports the count-in meanwhile (audio::PlayPosition::Value::count_in). A sink without a sequencer to count (a fake) just plays.
  virtual bool play_count_in(unsigned pattern, unsigned bars) noexcept {
    (void)bars;
    return play(pattern);
  }
  // OS-PLAYCUR (Shift + Play): like play(), starting the pattern at `row` (0-based) instead of its first step, never counting in. A sink with no sequencer behind it plays from the start.
  virtual bool play_from(unsigned pattern, unsigned row) noexcept {
    (void)row;
    return play(pattern);
  }
  // OS-PLAYCUR: while playing, the play line goes to `row` of the pattern now playing at the next step boundary and carries on from there. Refuses (false) by default and while the transport is not running.
  virtual bool locate_row(unsigned row) noexcept {
    (void)row;
    return false;
  }
  virtual bool play_song(unsigned row) noexcept = 0; // the project's order list from position `row` (0-based; past its end: from the first), from the pattern's first step
  virtual bool stop() noexcept = 0;
  // The live jump (OS-SONG1; seq::Sequencer::queue_jump): while playing, the song goes to order position `row` at the end of the pattern now playing; row 0xFFFF cancels the one
  // waiting. What waits is read back from the sequencer's published position (view.play.queued). Refuses (false) by default: a sink with no sequencer behind it. OS-PERF2's NEXT PATTERN
  // is this call with the next position of the song.
  virtual bool queue_jump(unsigned row) noexcept {
    (void)row;
    return false;
  }
  // The sequencer's own queue (seq::Command::QueuePattern, the sequential switch; OS-PADS-B's PATS pads): while playing, pattern `id` (1..255) plays next, at the end of the pattern now
  // playing, and the transport then loops it (Pattern mode); id 0 cancels the one waiting. Refuses (false) by default: a sink with no sequencer behind it. What waits is not published:
  // the caller remembers it (PatternEdit::queued_pattern) until the sequencer's position shows that pattern.
  virtual bool queue_pattern(unsigned id) noexcept {
    (void)id;
    return false;
  }
  // OS-PATSWITCH (seq::Command::SwitchPattern): while playing, the playing pattern becomes `id` (1..255) at the next step boundary and the line goes on at the same row; in Song mode the song is left. The pick of the pattern that
  // already plays takes back a switch that waits. Refuses (false) by default and when the command did not go (no sequencer behind the sink, ring full): the caller then changes nothing. A pattern the project lacks plays as empty.
  virtual bool switch_pattern(unsigned id) noexcept {
    (void)id;
    return false;
  }
  // OS-BUG-PASTEHANG (seq::Command::ReleasePattern): pattern `id` (1..255) was rewritten under the run (the whole-pattern paste), so its OFF rows are gone: at the next step boundary the tracks that play it end
  // what they still sound, as a switch does. Refuses (false) by default and when the command did not go (no sequencer behind the sink, ring full); the paste has been made either way.
  virtual bool release_pattern(unsigned id) noexcept {
    (void)id;
    return false;
  }
  // What a transport that starts without the Play key plays (OS-MIDISLV; seq::Command::Select): an external MIDI Start or Continue (the clock slave) runs the sequencer with no command of ours, so
  // the App says beforehand, while the transport is stopped and whenever the choice changes, what the Play key would play now: the song from its first position (`song`, on the Song screen) or the
  // pattern being edited (`pattern`, 1..255). Refuses (false) by default and while the sequencer cannot take it; the App then asks again on a later pass.
  virtual bool select(bool song, unsigned pattern) noexcept {
    (void)song;
    (void)pattern;
    return false;
  }

 protected:
  TransportSink() = default;
  ~TransportSink() = default;
};

// The project load of the device (OS-010a): the File screen asks it to open a project folder of the card ("Projects/My Song"; OS-007o: an OG one or a native one), which the
// platform runs as it runs `diag load` (OS-007e: in slices of the main loop, the app's frames held until it ends), and asks it afterwards how it
// went. The simulator attaches none and File's open action is then Disabled.
class ProjectSink {
 public:
  enum class Start : std::uint8_t { Started, NoCard, NoDocument, Busy, BadPath };
  // What the folder holds (the listing's tag): an OG project (project.mt, patterns/, instruments/), a native one (project.o3pj, OS-004a) or the backup of a native one
  // (project.o3pj.bak, what a save cut between the renames of its steps 8 and 9 leaves, app/atomic_save.h): read like the native file, never renamed by the open.
  enum class Format : std::uint8_t { Og, Native, NativeBackup };
  virtual Start open(const char* folder, Format format) noexcept = 0;
  // Replaces the document with a fresh project (OS-007o, File F3): the same hand-over as an open (the transport and live notes stopped, the voices let to die away, the sample
  // memory emptied (start_new: ALL, the instruments with it; no demo kit goes in), the app's frames held), reported the same way (busy() while it runs, loaded() after, folder() "" as no folder holds it). NoCard is never answered.
  virtual Start start_new() noexcept = 0;
  // OS-NEWKEEP (File F3 NEW > F1 KEEP INSTRUMENTS): as start_new(), but the instrument table (sample and module instruments alike) and the audio of the sample instruments stay; the patterns, the song,
  // the names and everything else of the project are emptied. A sink that cannot answers NoDocument.
  virtual Start start_new_keeping_instruments() noexcept { return Start::NoDocument; }
  // A load that was started has not ended yet.
  virtual bool busy() const noexcept = 0;
  // The folder of the load that is running or ran last ("Projects/My Song"; "" before any).
  virtual const char* folder() const noexcept = 0;
  // After a load ended: the project was read. When it was not, `detail()` says why in upper-case words ("NO PROJECT FILE"; "" if it has none).
  virtual bool loaded() const noexcept = 0;
  virtual const char* detail() const noexcept = 0;
  // OS-QAFIX-4: the last open was refused in its verify phase, before anything was replaced (a file that cannot load: truncated, a bad checksum, a project of a newer format): the document, the history and
  // the transport are as they were, loaded() and folder() are those of the project that was open before, and detail() says why the file was refused. False after every other end.
  virtual bool refused() const noexcept { return false; }
  // OS-FILES part A: replaces the document with a new project made from the module file `file` ("MOD/song.mod"; .mod .s3m .med .mmd0..3), with `engine` choosing where its commands go. The same
  // hand-over as open() and start_new(), reported the same way (busy(), then loaded() and detail()); import_summary() has what it made. A sink that cannot import answers NoDocument.
  virtual Start import_module(const char* file, ImportEngine engine) noexcept {
    (void)file;
    (void)engine;
    return Start::NoDocument;
  }
  // OS-IMPORT-ANY I4: false only when the sink READ the first bytes of `file` and no module reader takes it (ModuleImport::probe); true when it is one, when the file would not read (the import then says
  // why) and for a sink that cannot tell. Asked before the NEW question, so that a file that is no module never asks it. Nothing is changed.
  virtual bool module_file(const char* file) noexcept {
    (void)file;
    return true;
  }
  // The report of the last module import (valid() false before any), nullptr when the sink has none.
  virtual const ModuleSummary* import_summary() const noexcept { return nullptr; }

 protected:
  ProjectSink() = default;
  ~ProjectSink() = default;
};

class SampleBank;
class Recorder;
class SaveService;

// Everything a handler may touch, for the duration of one dispatch.
struct ActionContext {
  Model& model;                    // the project as the UI sees it
  ViewState& view;                 // screen, cursor, overlay, notifications...
  const char* const* soft_keys;    // the F1..F8 labels of the screen in view (its one table)
  core::Tick now;                  // for actions that need the time
  bool focus;                      // the base screen owns the input (false while a modal overlay is up)
  Session* session = nullptr;      // the document's editor (history), the macro recorder, the job runner; null in a bare test
  model::Editor* edit = nullptr;   // set by dispatch() for an undoable action, null otherwise: the only way to edit the project
  core::ParamRing* params = nullptr;  // the ring of ParamChange to the audio side (OS-002d): the actions that change a parameter push to it
  PreviewSink* preview = nullptr;     // the engine's preview voice, when there is one (OS-009: pad.preview)
  TransportSink* transport = nullptr; // the engine's transport, when there is one (OS-005d: transport.play)
  ProjectSink* project = nullptr;     // the device's project load, when there is one (OS-010a: file.open)
  SampleBank* samples = nullptr;      // the sample memory, when there is one (OS-011a): the sample screens' actions mark what the engine must take up
  PerformBoard* perform = nullptr;    // the Perform screen's slots, when there are some (OS-011b)
  MixBoard* mix = nullptr;            // the mixer's runtime side, when there is one (OS-011b): routing, cue level, the stem map; the Mixer screen's actions write it
  Recorder* rec = nullptr;            // the sample recorder, when there is one (OS-011c): the Record screen's actions drive it
  SaveService* saves = nullptr;       // the card's save layer, when there is one (OS-007n): File's SAVE / SAVE AS, the WAV saves of Record and Sample edit; set by the App after construction
  PatternClip* clip = nullptr;        // the pattern clipboard (OS-PATCOPY), the App's; set by the App after construction
  SongClip* song_clip = nullptr;      // the song clip (OS-SONGMAP): the Song map's cells, the App's; set by the App after construction
  hal::firmware::FirmwareSink* firmware = nullptr;  // the firmware update port (OS-FWCONFIG), when the platform has an update path; set by the App after construction
  bool repeat = false;                // the call is a key auto-repeat (a held button's later Taps, OS-DELREPEAT): dispatch() runs it only when the action is kActionRepeatable
};

// Returns true if something visible changed (the screen must be redrawn).
using ActionHandler = bool (*)(ActionContext& ctx, core::ActionArg arg) noexcept;
// Optional: false = the action is not available right now. Null = always.
using ActionEnabled = bool (*)(const ActionContext& ctx) noexcept;

struct ActionDef {
  core::ActionId id;
  const char* name;  // lower case, [a-z0-9._], "area.verb"
  ActionHandler handler;
  core::ActionFlags flags;
  ActionEnabled enabled;
};

// The id is the hash of the name, so a definition cannot disagree with itself.
constexpr ActionDef make_action(const char* name, ActionHandler handler, core::ActionFlags flags = 0,
                                ActionEnabled enabled = nullptr) noexcept {
  return ActionDef{core::action_id(name), name, handler, flags, enabled};
}

struct ActionTable {
  const ActionDef* items;
  std::size_t count;
};

// The registry: every action of this build.
ActionTable action_registry() noexcept;

// nullptr when there is no such action. By name: case-insensitive.
const ActionDef* find_action(ActionTable table, core::ActionId id) noexcept;
const ActionDef* find_action(ActionTable table, const char* name, std::size_t length) noexcept;

enum class DispatchResult : std::uint8_t {
  Unknown,    // no action with that id
  Disabled,   // `enabled` said no, or the action needs focus and the context has none
  Unchanged,  // ran, nothing visible changed
  Changed,    // ran, the screen must be redrawn
};

DispatchResult dispatch(ActionTable table, core::ActionId id, core::ActionArg arg, ActionContext& ctx) noexcept;

// Compile-time sanity of a registry: ids non-zero and unique, each the hash of its name, names well formed
// ("area.verb": [a-z0-9._] with a dot, which also keeps clear of the keymap word `none`). That every handler is
// present is checked by actions_test: GCC does not allow comparing a function address with null in a constant.
constexpr bool registry_valid(const ActionDef* items, std::size_t count) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    const ActionDef& a = items[i];
    if (a.id == core::ActionId::None || a.name == nullptr || a.name[0] == '\0') {
      return false;
    }
    std::size_t n = 0;
    bool dot = false;
    for (; a.name[n] != '\0'; ++n) {
      const char c = a.name[n];
      dot = dot || c == '.';
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_')) {
        return false;
      }
    }
    if (!dot || a.id != core::action_id(a.name, n)) {
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (items[j].id == a.id) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace o3os::app
