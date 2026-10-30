// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/dj.h>
#include <o3os/app/enc_accel.h>
#include <o3os/app/export_view.h>
#include <o3os/app/file_import.h>
#include <o3os/app/file_view.h>
#include <o3os/app/fill_view.h>
#include <o3os/app/firmware_view.h>
#include <o3os/app/help_text.h>
#include <o3os/app/input_test.h>
#include <o3os/app/mixer_state.h>
#include <o3os/app/pad_layer.h>
#include <o3os/app/param_view.h>
#include <o3os/app/pattern_edit.h>
#include <o3os/app/pattern_view.h>
#include <o3os/app/record_view.h>
#include <o3os/app/sample_load_view.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/sample_edit_state.h>
#include <o3os/app/song_tools_view.h>
#include <o3os/app/song_view.h>
#include <o3os/app/text_entry.h>
#include <o3os/app/wave_state.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/notify.h>
#include <o3os/core/power_hold.h>
#include <o3os/ui/theme.h>

#include <cstdint>

namespace o3os::app {

// The step the sequencer is on in `pattern`, or -1 when it is not playing that pattern (the Pattern screen's play bar shows the position of the
// pattern that plays and no other).
inline int play_row(const audio::PlayPosition::Value& v, unsigned pattern) noexcept {
  return v.playing && v.pattern == pattern ? static_cast<int>(v.step) : -1;
}

// The song row (position in the order list) the sequencer is on, or -1 when it is not playing the song (the Song screen's play marker).
inline int play_order(const audio::PlayPosition::Value& v) noexcept {
  return v.playing && v.order != audio::PlayPosition::kNoOrder ? static_cast<int>(v.order) : -1;
}

// What the user has navigated to, as opposed to the model (Model): the base screen and its cursor, page and mode, the
// overlay, the Navigator's choice, the current instrument, the colour theme and the notifications. Actions change it and
// the screens draw from it; nothing else does.
// The Learn overlay's list of bindings (OS-013a, app/learn_list.h): the row the cursor is on (an index into the list of both stores, clamped when read) and whether Clear all
// has been pressed once (a second press clears; anything else disarms it).
struct LearnView {
  std::uint8_t cursor = 0;
  bool confirm = false;
};

// OS-012b: the module screens' cursors: the MODULES screen's chosen app, the engine picker's chosen entry and the row of the running app's parameter list.
struct ModulesView {
  std::uint8_t apps = 0;
  std::uint8_t picker = 0;
  std::uint8_t app_row = 0;
  // OS-H3A, Pattern F7 TOOLS: the list's cursor, the line it says when a launch was refused (a pointer to a literal; the notification strip waits behind a modal overlay), and whether the running app
  // was opened from the list (its exit chord then goes back to Pattern, one level of return, and not to MODULES).
  std::uint8_t tools = 0;
  bool from_tools = false;
  // OS-KEYROWDLG S5 (ABI 1.9): the running app declares its keys, so its page draws the app's Shift + F1..F7 labels while Shift is held (follows_shift). Set by App::poll_modules, cleared when the page is left.
  bool app_keys = false;
  const char* tools_msg = "";
};

// OS-KEYROWDLG S3, the boot disk (OS-USBDISK U1c): the strip of the disk mode is a persistent notice on the app's own Pattern screen, so `overlay` is None there and nothing would stop the screen from drawing its own
// F-key row (PATTERN .. UNDO) under a strip that says "press any key to restart". The platform says which row the disk mode has: every key reads RESTART (a key or pad restarts the unit), AGAIN? inverted while the
// computer may still be writing (a second press restarts), blank while the restart waits for the command in flight. Off: the app is not in the disk mode. Folded into overlay_keys(): one predicate for every row site.
enum class DiskKeys : std::uint8_t { Off, Restart, Confirm, Finishing };

// OS-FU2: the on-screen keyboard overlay: the text being edited (text_entry.h), what it is for, and the line it says under the name (the notification strip waits behind a modal overlay, so a
// refusal is said inside it: a pointer to a literal).
struct TextEntryView {
  // Instrument (OS-INSTUI S7): NAME, Shift + F1 of the Instrument screen. Track, Pattern and Project (OS-NAMES): NAME of the Mixer (Shift + F1), the Song (Shift + F6) and the File screen (Shift + F6).
  enum class Target : std::uint8_t { None, SaveAs, Instrument, Track, Pattern, Project };
  TextEntry entry;
  Target target = Target::None;
  std::uint8_t subject = 0;  // what Track and Pattern name: the track slot (0-based) or the pattern id, fixed when the keyboard opens
  const char* message = "";
};

// The step popup (OS-FU3): which list a held Instr, Fx or Note button opened and what it has chosen. The instrument
// popup chooses ViewState::instrument itself (and updates it live); the others keep their choice here.
struct StepPopupView {
  enum Kind : std::uint8_t { Instr = 0, Fx = 1, Note = 2, Vol = 3 };  // Fx: the Fx button (FX column 2); Vol: the Vol button (FX column 1)
  std::uint8_t kind = Instr;
  std::int16_t item = 0;  // Fx: the picker index 0..model::fx::kPickerCount-1; Note: the pad 0..47 chosen, -1 none yet
  bool samples_only = false;  // Instr (OS-INSTPICK, Wave / Sample edit): only the sample instruments can be chosen, the rest of the list is dimmed (step_popup_pickable)
};

// The Preview key held on a sample screen (OS-FU3; Wave / Chop, Sample edit, the Record take): the action sets it, the App's pass starts the sound and the key's release (a raw
// event) clears `button`, which ends it. Start: the instrument from its own start point (a live note); Over: the instrument's sample from `from` (a frame), on the audition's
// descriptor; Take: the Record take's trim window, on the same.
struct HeldPreview {
  static constexpr std::uint8_t kNone = 0xFF;
  enum Kind : std::uint8_t { Start = 0, Over = 1, Take = 2 };
  std::uint8_t button = kNone;  // the key held (a Button id), kNone when let go
  std::uint8_t kind = Start;
  bool pending = false;         // asked, not started yet
  std::uint32_t from = 0;       // Over: the frame the sound starts at
};

// The pads of the Record screen playing the take under review (OS-REC2; record_view.h has the layout): the action sets `pending` for a pad that went down, the App's pass
// binds the take's window to the preview descriptor when the quiet window is open and starts the notes (`pending` to `down`), and a pad's release (a raw event) clears its bit.
struct TakePads {
  std::uint64_t pending = 0;  // bit n: pad n went down and its note has not started yet
  std::uint64_t down = 0;     // bit n: pad n's note was started and the pad is still held
  bool any() const noexcept { return (pending | down) != 0u; }
  void release(unsigned pad) noexcept {
    const std::uint64_t m = ~(std::uint64_t{1} << (pad & 63u));
    pending &= m;
    down &= m;
  }
};

// OS-QAFIX-E-SREC-09: the question a take under review asks before the keys that lose audio (F4 DISCARD, a new take over it, CROP): the first press arms and puts the question on the strip, a second press of the same
// key within kWindowMs does it. RAM only. The key row marks the key while the question's text is the head of the strip (key_prompt.cpp).
struct TakeAsk {
  enum class Kind : std::uint8_t { None, Discard, Record, Crop };
  static constexpr std::uint32_t kWindowMs = 4000;  // as long as the question's text stays on the strip
  Kind kind = Kind::None;
  std::uint32_t at = 0;  // core::Tick of the press that asked
};

// OS-POWER P1: what the hold of Power shows (docs/specs/OS-POWER.md D4): a full-width line that fills, and a strip text on the info line, both drawn AFTER the overlays so that they show over a
// modal one. Told by the platform (or the simulator) once a pass through App::set_power(); the App adds only the warning that the project has unsaved work.
struct PowerView {
  core::PowerShow show;
  bool unsaved = false;  // the Hold text reads UNSAVED WORK - LET GO TO KEEP IT
};

// OS-HELP: the help overlay (help_text.h). RAM only, never saved (Q5): the page in view, and where each page was left, so Shift + Config comes back to it and Left / Right show each page as it was.
struct HelpView {
  std::uint8_t page = 0;
  std::uint16_t scroll[kHelpPages] = {};
};

// OS-PATTEMPO (NEW Community OS behaviour): the taps of the tempo tap, Pattern held + a pad. `stamp` is the pad event's time, set by the App just before the action runs; `at` are the last
// kTaps taps, the tempo is the mean of the intervals between them. A pause of more than kGapMs starts a new series.
struct TapTempo {
  static constexpr unsigned kTaps = 5;       // the last up to 4 intervals
  static constexpr unsigned kGapMs = 2000;
  core::Tick stamp = 0;
  core::Tick at[kTaps] = {};
  std::uint8_t n = 0;
};

struct ViewState {
  static constexpr std::uint32_t kMessageMs = 2000;  // how long an F-key placeholder notification is shown
  // The mode the Pattern screen starts in: Browse (PatternMode::Browse, screens/pattern_screen.h asserts the number; Screen::default_mode()).
  static constexpr std::uint8_t kPatternDefaultMode = 3;

  ViewState() noexcept { mode[screen_index(ScreenId::Pattern)] = kPatternDefaultMode; }

  PatternView pattern;  // the Pattern screen's cursor and window
  PatternEdit edit;     // the pattern being edited, the edit step, the pad board, the selection (OS-009)
  SongView song;        // the Song screen's cursor and window
  SongDensity density;  // the TIMELINE page's thumbnails (song_map.h): a cache kept here, not in SongView, which SongScreen::render copies each frame
  FileView file;        // the File screen's cursor and window over the folder listing (OS-010a)
  FileImport import;    // the File screen's IMPORT page (OS-FILES): the page, its folder, the report flag, the engine choice
  ExportView xport;     // the File screen's EXPORT page and the progress of a running WAV export (OS-WAVEXP)
  WavView wav;          // the Sample load screen's cursor, folder and requests (OS-011a)
  WaveState wave;       // the Wave / Chop screen's window, selection and options (OS-011a)
  SampleEditState sedit;  // the Sample edit screen's window, selection and hand-offs (OS-011a)
  ParamView params;     // the generated parameter page's row and envelope (OS-002d)
  LearnView learn;      // the Learn overlay's list cursor (OS-013a)
  MixerView mixer;      // the Mixer screen's cursor (OS-011b)
  ModulesView modules;  // the MODULES screen, the engine picker and the app page (OS-012b)
  TextEntryView text;   // the on-screen keyboard overlay (OS-FU2)
  StepPopupView popup;  // the step popup (OS-FU3)
  FillView fill;        // the Fill dialog's rows and the last fill (OS-PTOOLS)
  SongToolsView tools;  // the Song screen's TOOLS dialog: its rows and where it acts (OS-SONGMAP)
  HelpView help;        // the help overlay: its page and the scroll of each page (OS-HELP)
  HeldPreview hold;     // the Preview key of the sample screens (OS-FU3)
  bool step_preview = false;  // F5 PREVIEW on the Pattern screen is down and its note sounds (OS-CFGWIRE): the key's release (a raw event) or leaving the screen ends it (App::stop_step_preview)
  TakePads take_pads;   // the pads playing the Record take (OS-REC2)
  TakeAsk take_ask;     // the question of a take under review (OS-QAFIX-E-SREC-09)
  PadLayer padmode;     // the pads' mode and the momentary TRACKS layer (OS-PADS-A; runtime only)
  RecordView record;    // the Record screen's cursor on each page (OS-011c)
  TapTempo tap_tempo;   // OS-PATTEMPO: the pad taps under a held Pattern (runtime only, main loop only)
  DjState dj;           // OS-DJ A1: the DJ page's groups (kept until power off), crossfader and curve (end on leaving the page); runtime only, main loop only

  // One base screen at a time, and at most one overlay over it (ADR 0009 rev 1). Page and mode are kept per screen, so
  // a screen is as it was left: the page is cycled by pressing the screen's own button again, the mode is chosen with
  // view.mode (both indexed by screen_index()).
  ScreenId screen = ScreenId::Pattern;
  // OS-SONGPEEK: a held Song button shows Song until it is let go (nav.peek), then the screen it came from is back, one level (nav.peek_end). `peek_from` is that screen; `peeking` is
  // true from the Hold to the release, and any other change of the base screen in between (switch_screen) ends it: the release then leaves the screen where the user went. Runtime only.
  ScreenId peek_from = ScreenId::Pattern;
  bool peeking = false;
  std::uint8_t page[kScreenCount] = {};
  std::uint8_t mode[kScreenCount] = {};
  // OS-DJ A1: the Perform screen is in view on its DJ page (the second; the Performance button again turns to it).
  bool dj_page() const noexcept { return screen == ScreenId::Perform && page[screen_index(ScreenId::Perform)] == kDjPageIndex; }
  // OS-DJ A4: the pattern that plays (stopped: the one in view) and the bank of 12 the DJ page's row 1 shows.
  unsigned dj_now() const noexcept { return play.playing ? play.pattern : edit.pattern; }
  unsigned dj_bank() const noexcept { return dj.bank_now(dj_now()); }
  OverlayId overlay = OverlayId::None;  // a modal overlay (Learn, Navigator) or None; see shown_overlay()
  DiskKeys disk_keys = DiskKeys::Off;   // the boot disk's key row (App::set_disk_keys); anything but Off owns the row, as a modal overlay does
  // OS-USBDISK 4.3: the computer has the card (App::set_card_on_computer; the platform tells it every pass). The listings say CARD ON COMPUTER instead of NO SD CARD and the refusals of a read say it too.
  bool card_on_computer = false;
  const char* card_off_text(const char* otherwise) const noexcept { return card_on_computer ? "CARD ON COMPUTER" : otherwise; }
  // OS-USBDISK U2b, the Config > USB page's CARD AS DISK row (docs/specs/OS-USBDISK.md 9). View state: nothing here is saved (Q4: READ ONLY is a choice of this session; the first press is `params.armed`).
  // The platform tells `phase`, `claims_busy`, `audio` and `writable` once a pass (App::set_disk_state); the row's second press leaves `request` for the platform (App::take_disk_request). Main loop only.
  struct DiskView {
    enum class Phase : std::uint8_t { Idle, Working, Out };  // Working: the hand-over or the eject is running (the keys wait)
    enum class Request : std::uint8_t { None, ShareReadOnly, ShareWritable, Eject };  // the share carries READ ONLY as it was at the press
    Phase phase = Phase::Idle;
    bool claims_busy = false;  // a load, an export or an upload holds the card (the platform's card_busy())
    bool audio = false;        // the unit presents USB AUDIO now: the share pauses it
    bool writable = false;     // the disk the computer has is writable now (usb0::DiskState::writable, told with the phase): the READ ONLY row shows it while the card is out
    bool read_only = true;     // READ ONLY row; On by default (Q4: read-only first); a session's choice, not a setting. Changes only while Idle with no request left (the policy is fixed at the share)
    Request request = Request::None;
  };
  DiskView disk;
  int navigator = 0;                    // the Navigator's chosen cell: an index into the screen registry

  bool playing = false;  // the transport is running (the Play button's last accepted request): the status strip's PLAY / STOP badge
  audio::PlayPosition::Value play;  // where the sequencer is, as the App last read it (once a pass; OS-009b): the Pattern screen's play bar and BPM
  // OS-METRO: the count-in's visuals. `beat` is the beat of the count-in last seen (App::poll_play_position: beats left including the running one, 8..1, 0 when not counting in), `at_ms` when it began and
  // `bar` whether it was the first of a bar: the pads flash from it (App::pad_lights) and the notification strip counts it. A still screen holds nothing: all zero.
  struct CountIn {
    std::uint8_t beat = 0;
    bool bar = false;
    std::uint32_t at_ms = 0;
  };
  CountIn count_in;
  std::uint32_t track_pat[audio::PlayPosition::kTrackWords] = {};  // E-PERF-07: each track's source pattern word as the App last read it (PlayPosition::track_pattern; 0 while stopped): the Perform boxes
  bool blink = false;  // the phase of what blinks (the Song screen's slot waiting for a live jump; ~2 Hz): the App sets it from its clock while there is something to blink
  bool mute_blink = false;  // the same phase for the Pattern screen's MUTE tag of a track whose quantised mute is pending (OS-MUTEVIS); set only while one is, so a still screen redraws nothing
  int instrument = 1;  // the last used instrument, 1..64 (the status strip's INS; a new note takes it)
  std::uint64_t inst_pads = 0;  // OS-INSTUI S2: bit n = pad n went down on the Instrument screen and has not been released (instrument.pad sets it, App::run_once clears it at the release; a change of instrument releases these, O2)
  bool shift = false;  // Shift is physically held: the key row shows the Shift layer (the App sets it from the button events)
  EncAccel enc_accel;  // OS-AUDIT-28: the encoder's acceleration in lists: the curve's numbers (the console's `enc accel`) and the run so far; nav.scroll's, main loop only
  ui::ThemeId theme = ui::ThemeId::Amiga;  // ui.theme.next cycles it; the screen draws in it (ui/theme.h)
  core::NotifyQueue notify;                // timed messages; the head is drawn when no modal overlay is up
  InputTestState input_test;               // the hidden INPUT TEST screen's picture of the raw input (input_test.h)
  PowerView power;                         // OS-POWER P1: the line and the strip of a hold of Power, over everything (App::set_power)
  // Last on purpose: 900 bytes in the middle would move every member after it past the short offsets of a load or a store (flash in every function that reads them).
  FirmwareView firmware;                   // the firmware update dialog of Config's INFO page: the list, the questions, the check and the hand-over (OS-FWCONFIG)

  // The overlay actually drawn: the modal one if there is one, else the notification strip while the queue has something.
  OverlayId shown_overlay() const noexcept {
    return overlay != OverlayId::None ? overlay : (notify.empty() ? OverlayId::None : OverlayId::Notify);
  }
  // OS-SMALL2, OS-KEYROWDLG: a dialog owns the F-key row. Every modal overlay does (the Navigator, Fill and the export progress since OS-SMALL2; Learn, the choosers, the pickers, the keyboard and the step popup since
  // OS-KEYROWDLG): the row is its own prompt's (key_prompt.h), painted once by render_scene, and the screen under it draws none. On the unit the screen's labels showed through the Navigator's ("F1 GOTTERN") although
  // the simulator's fill hides them (why the chip's did not is open: OS-SIMKEY), so nothing under an owning row may depend on a fill, and one predicate says it for every row site. (Notify is never `overlay`: a notice is not a dialog.)
  bool overlay_keys() const noexcept { return (overlay != OverlayId::None && overlay != OverlayId::Notify) || disk_keys != DiskKeys::Off; }
  // The text of the notification on screen, or "".
  const char* shown_message() const noexcept {
    return shown_overlay() == OverlayId::Notify ? notify.head().text.c_str() : "";
  }
};

}  // namespace o3os::app
