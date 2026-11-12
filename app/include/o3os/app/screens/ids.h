// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Stable identities of the app's screens and overlays (ADR 0009 rev 1, OS-002b). They are numbers a binding or a keymap
// can carry as an action argument (`nav.screen 1`), so a value is never reused or renumbered: a screen that is dropped
// leaves a hole.

#include <cstdint>

namespace o3os::app {

// One per screen button of the Tracker (docs/design/product-ux.md sections 3 and 4). Only some have a Screen yet; the
// registry (screens/screen.h) lists all of them and marks the others as not implemented.
enum class ScreenId : std::uint8_t {
  Pattern = 0,     // Pattern button
  Song = 1,        // Song
  Instrument = 2,  // Params
  Wave = 3,        // SamplePlay
  SampleEdit = 4,  // SampleEdit
  SampleLoad = 5,  // SampleLoad
  Record = 6,      // SampleRec
  Mixer = 7,       // Master
  Perform = 8,     // Performance
  File = 9,        // File
  Config = 10,     // Config
  InputTest = 11,  // no button: the hidden diagnostic of the hardware session (OS-007d), reached by a boot chord, not by the Navigator
  Modules = 12,    // OS-012b: the SD modules' screen (the apps on the card), reached from the Navigator: it has no button of its own
  ModuleApp = 13,  // OS-012b: the page of a running app module (its body is the module's view); hidden, opened by launching an app, left by the exit chord
};
inline constexpr unsigned kScreenCount = 14;  // ids are 0..kScreenCount-1: an index into per-screen state

constexpr unsigned screen_index(ScreenId id) noexcept { return static_cast<unsigned>(id); }
constexpr bool screen_id_valid(std::int32_t value) noexcept {
  return value >= 0 && value < static_cast<std::int32_t>(kScreenCount);
}

// The overlay slot: at most one overlay is drawn over the base screen. Learn, the Navigator and the three choosers of the Pattern
// screen (OS-009: pattern, length, step jump: a strip over the info line) are modal: they own the input (their context captures).
// The notification overlay is not state: it is shown when no modal overlay is up and the notification queue is not empty
// (view_state.h, `shown_overlay`).
enum class OverlayId : std::uint8_t {
  None = 0,
  Learn = 1,
  Navigator = 2,
  Notify = 3,
  PatternChooser = 4,
  LengthChooser = 5,
  JumpChooser = 6,
  ModulePicker = 7,  // OS-012b: the engine picker of the Instrument page (the SD modules to bind to the slot)
  TextEntry = 8,     // OS-FU2: the on-screen keyboard (a name for Save As): app/text_entry.h
  StepPopup = 9,     // OS-FU3: the instrument / FX / note list a held Instr, Fx or Note button opens (the step popups): view_state.h StepPopupView
  Fill = 10,         // OS-PTOOLS: the Pattern screen's F4 FILL dialog (fill_view.h)
  PlayMode = 11,     // OS-INSTUI S4: the Wave screen's list of the eight play modes (Shift + F1)
  SongTools = 13,    // OS-SONGMAP: the Song screen's F6 TOOLS dialog over the map's rectangle (song_tools_view.h)
  FirmwareUpdate = 14,  // OS-FWCONFIG: the firmware update dialog of Config's INFO page (F8 UPDATE): the list, two questions, the check, the hand-over (firmware_view.h)
  Events = 15,          // OS-MIDIREC S1b: the EVENTS list over the Pattern screen's grid pane (events_view.h), opened from the TOOLS list
  VerbMenu = 16,        // OS-PTOOLS2 S3: the Pattern screen's F7 verb menu (verb_menu.h), two columns of eight rows; the TOOLS list (12) stays reachable from its APPS > row
  Preset = 17,          // OS-INSFX-ABI2 A9: the F5 PRESET list of a module's parameter page, an instrument's or an insert's (preset_list.h)
  Help = 21,            // OS-HELP: the help page of the screen in view, Shift + Config (help_text.h); modal, any key but the arrows, the encoder and Shift closes it
  ExportProgress = 20,  // OS-WAVEXP: the modal progress of a running WAV export, F8 CANCEL (export_view.h); 20 leaves the numbers between to the branches that add overlays
  PatternTools = 22,    // OS-PTOOLS2 S2: the Pattern screen's TOOLS dialog over the block (pattern_tools_view.h), opened from the verb menu's TRANSPOSE / INSTRUMENT rows
  PadsPicker = 24,      // OS-UXF-PAT 7: the pads' mode list of Pattern's Shift + F2 PADS (F1..F6 pick, F8 BACK; pads_picker_overlay.cpp)
  LearnMap = 23,        // OS-LEARNMAP: every MIDI Learn binding and the encoder's target on one cover (learn_map.h), opened by F6 MAP in the Learn dialog and F8 on Config's MIDI page
};

constexpr bool is_chooser(OverlayId id) noexcept {
  return id == OverlayId::PatternChooser || id == OverlayId::LengthChooser || id == OverlayId::JumpChooser;
}

}  // namespace o3os::app
