// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The application's side of the input pipeline: its contexts, its built-in binding layer, the names a keymap text
// may use. The machinery (gestures, resolution, parsing) is in core.

#include <o3os/app/buttons.h>
#include <o3os/core/binding.h>
#include <o3os/core/keymap.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// Contexts, as the chain lists them (innermost first): overlay, mode, field, screen, global (App::context_chain). The numbers
// are what core compares; the names a keymap uses are in bindings.cpp. A screen adds its own context, and one per mode
// if it has modes, and one per field under its cursor if it has fields (OS-009 D3: Pattern's Note, Instr, Vol and FX columns); a
// modal overlay adds one that captures (screens/overlay.h). A mode row overrides, a field row specialises the screen's default.
enum class Context : std::uint8_t {
  Global = 0,
  Pattern = 1,        // the pattern screen
  PatternEdit = 2,    // its modes (screens/pattern_screen.h)
  PatternFollow = 3,
  PatternSelect = 4,
  Song = 5,           // the song screen
  Learn = 6,          // the Learn overlay (captures)
  Navigator = 7,      // the Navigator overlay (captures)
  InputTest = 8,      // the hidden INPUT TEST screen (captures: every control must reach the test, none may act)
  Instrument = 9,     // the generated parameter page (OS-002d): the encoder edits, F1..F4 pick its page
  PatternBrowse = 10,  // OS-009: the pattern's fourth mode (the default: pads preview, the encoder scrolls)
  PatternNote = 11,    // OS-009: the field under the pattern cursor (the chain's field level)
  PatternInstr = 12,
  PatternVol = 13,
  PatternFx = 14,      // both FX columns
  Chooser = 15,        // OS-009: the strip chooser (pattern, length, step jump) over the info line (captures)
  File = 16,           // OS-010a: the File screen (open, rescan)
  SampleLoad = 17,     // OS-011a: the Sample load screen
  Wave = 18,           // OS-011a: the Wave / Chop screen (both pages)
  WaveChop = 19,       // OS-011a: its CHOP page (the screen's field level: a page's keys specialise the screen's)
  SampleEdit = 20,     // OS-011a: the Sample edit screen
  Config = 21,         // OS-011a: the Config screen (the device settings pages)
  Mixer = 22,          // OS-011b: the Mixer screen (MIXER, ROUTING, MASTER FX, STEMS)
  Perform = 23,        // OS-011b: the Perform screen (the slots; its pads are the twelve slots' four rows)
  Modules = 24,        // OS-012b: the MODULES screen (launch an app, rescan)
  ModuleApp = 25,      // OS-012b: a running app's page (captures: the app gets the input, the OS keeps the exit chord and the Shift layer)
  ModulePicker = 26,   // OS-012b: the engine picker overlay (captures)
  Record = 27,         // OS-011c: the Record screen (SOURCES, RESAMPLE, RADIO)
  TextEntry = 28,      // OS-FU2: the on-screen keyboard overlay (captures)
  StepPopup = 29,      // OS-FU3: the step popup (instrument / FX / note list; captures)
  PadsTracks = 30,     // OS-PADS-A: the TRACKS layer of the pads (a fixed TRACKS mode on Pattern, a lone Shift hold on Pattern and Perform): innermost, binds the 48 pads only
  PadsMode = 31,       // OS-PADS-B: the pads of a fixed PIANO / ISO / STEP / PATS / VU mode on Pattern (innermost, binds all pads to pads.play, which acts by the mode)
  FillDialog = 32,     // OS-PTOOLS: the Fill dialog of Pattern's F4 (captures)
  WaveWt = 34,         // OS-INSTUI S4: the Wave screen of a WAVETABLE instrument (the screen's field level: its rows are the window's position and size)
  WaveGran = 35,       // OS-INSTUI S4: the Wave screen of a GRANULAR instrument (position, length, shape, loop)
  PlayModePicker = 36, // OS-INSTUI S4: the play mode list over the Wave screen (captures)
  ToolsList = 38,      // OS-H3A: the Pattern screen's F7 TOOLS list over the screen (captures)
  SongAsk = 39,        // OS-SONGMAP: the Song screen's question of 1.6 is up: the screen's field level, F1 ALL and F2 UNIQUE answer it (it captures nothing else: another key takes the question back and acts)
  SongTools = 40,      // OS-SONGMAP: the Song screen's F6 TOOLS dialog (captures)
  InstrumentEmpty = 37, // OS-INSTUI S5: the Instrument screen on an empty slot (the screen's field level: F1..F5 are the type chooser)
  FileExport = 44,     // OS-WAVEXP: the File screen's EXPORT page (the screen's field level: F1 starts, F6 IMPORT, F7 PROJECTS, the encoder picks the pattern; 44 and 45 leave the numbers between to the branches that add contexts)
  ExportProgress = 45, // OS-WAVEXP: the progress of a running export (captures: F8 CANCEL is the one key)
  FirmwareUpdate = 48, // OS-FWCONFIG: the firmware update dialog (captures: F1 and F8 answer, Up / Down / the encoder choose; any other key takes a question back)
  PerformDj = 46,      // OS-DJ A1: the Perform screen's DJ page (the screen's field level: the crossfader on the encoder and the pads' row 4, F3 CURVE, F8 RESET; the slots' keys are inert there)
  MixerStems = 49,     // OS-USBOUTS: the Mixer screen's STEMS page (the screen's field level: F6 APPLY the pending OUTPUTS value; the other Mixer keys are the screen's)
  SampleEditFx = 43,  // OS-AUDIT-15: the Sample edit screen's FX page (the screen's field level: F1 FILTER, F2 COMPRESSOR, the encoder changes the row's value)
  RecordRadio = 33,   // OS-SHIFTROW: the Record screen's RADIO page (the screen's field level: Shift + F5 / F6 tune ten channels there only)
  Help = 47,           // OS-HELP: the help overlay (captures: the arrows and the encoder scroll, F8 and every other key close it)
  None = 0xFF,         // no context: a screen without a field level
};

constexpr core::ContextId context_id(Context c) noexcept { return static_cast<core::ContextId>(c); }

constexpr core::InputRef button_ref(Button b) noexcept {
  return core::InputRef{core::InputKind::Button, static_cast<std::uint8_t>(b)};
}
constexpr core::InputRef pad_ref(std::uint8_t index) noexcept {
  return core::InputRef{core::InputKind::Pad, index};
}
constexpr core::InputRef encoder_ref(std::uint8_t index) noexcept {
  return core::InputRef{core::InputKind::Encoder, index};
}

// Buttons that modify the gesture of the next input while held (Shift+pad, Shift+Config): bit n = button n.
inline constexpr std::uint64_t kModifierButtons = std::uint64_t{1} << static_cast<unsigned>(Button::Shift);

// Buttons that auto-repeat while held (OS-009b, new OS behaviour; schedule in core/gesture.h kRepeatGapMs): the four arrows, and Delete (OS-DELREPEAT: the pattern's step.clear / step.pull).
// Not every key, and not every action of these keys: a repeat is a flagged Tap that dispatch() gives only to a kActionRepeatable action (the press always runs), so file.delete, song.delete_slot, ... do not repeat.
inline constexpr std::uint64_t kRepeatButtons = (std::uint64_t{1} << static_cast<unsigned>(Button::Up)) | (std::uint64_t{1} << static_cast<unsigned>(Button::Down)) |
                                                (std::uint64_t{1} << static_cast<unsigned>(Button::Left)) | (std::uint64_t{1} << static_cast<unsigned>(Button::Right)) |
                                                (std::uint64_t{1} << static_cast<unsigned>(Button::Delete));

// Pool sizes of the loadable layers, 16 bytes a binding. PROVISIONAL: a full OctaMED keymap may need more.
inline constexpr std::size_t kUserBindings = 32;
inline constexpr std::size_t kPresetBindings = 64;

// The built-in layer: a constexpr table, free of conflicts (static_assert in bindings.cpp).
core::BindingView builtin_bindings() noexcept;

// Context, button and action names for core::parse_keymap.
const core::KeymapNames& keymap_names() noexcept;

}  // namespace o3os::app
