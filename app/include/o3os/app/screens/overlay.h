// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The overlay slot (ADR 0009 rev 1, OS-002b): one overlay at a time over the base screen. An overlay declares the
// rectangle it covers; the base screen is handed it and skips what lies entirely inside, and the overlay then paints its
// own pixels over the rest (the Navigator also repaints the F-key row, below). The three overlays of this build:
//
//   Learn        modal, a dialog: title, "move a control or send a MIDI CC", the target, the list of bindings (both stores; the cursor chooses
//                one, F3 deletes it, F4 clears all: OS-013a), the keys.
//                Opened and closed by the FX1 + FX2 chord (learn.toggle); F8 (ui.back) closes it too.
//   Navigator    modal, a dialog with a grid of every registered screen (name, icon, screen button; the ones not
//                implemented yet disabled): the chords Master + Song and Song + Pattern open it and it stays open (OS-APPSNAV, OS-NAV2); choose with the
//                arrows or the encoder, F8 (or F1) GO to the screen, F7 or Pattern again CLOSE it without changing the view.
//   Notify       not modal: the head of the notification queue as a strip exactly over the info line, with a severity
//                tile. Shown while no modal overlay is up; the queue waits behind one.
//
// A modal overlay's context is first in the binding chain and captures: an input it does not bind is swallowed, so it
// cannot reach the screen underneath (core/binding.h). Notify has no context: the keys stay the screen's. The three choosers of the
// Pattern screen (OS-009 P1a.1: pattern, length, step jump) are modal strips over the info line, one context, `chooser`. An Overlay is
// also a Cursor (cursor.h): the navigation actions move the overlay that is up, so the Navigator's grid is moved by the same
// actions as a screen's list; Learn and the strip have no cursor.

#include <o3os/app/bindings.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/cursor.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/verb_menu.h>
#include <o3os/app/view_state.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/renderer.h>

namespace o3os::app {

class Overlay : public Cursor {
 public:
  // The rectangle this overlay paints, in the theme's metrics. The base screen receives it as `covered`.
  virtual ui::Rect rect(const ui::Theme& theme) const noexcept = 0;
  virtual void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept = 0;
  // Modal: the overlay owns the input, context() is first in the chain and captures.
  virtual bool modal() const noexcept = 0;
  virtual Context context() const noexcept { return Context::Global; }  // meaningful for modal overlays only
  // OS-HELP: a press (a button but Shift, a pad) that the overlay's context does not bind and so captures closes it and does nothing else (App::dispatch_gestures).
  virtual bool closes_on_press() const noexcept { return false; }

 protected:
  Overlay() = default;
  ~Overlay() = default;
};

// nullptr for OverlayId::None.
const Overlay* overlay_of(OverlayId id) noexcept;

// OS-POWER P1 (docs/specs/OS-POWER.md D4): the hold of Power, drawn after the overlays. A line across the top edge of the screen, full width and at least 4 px (the theme's pad), filled to
// `show.fill` thousandths in Sel on the Chrome of the status strip (the Sel-on-Chrome pair of ui/theme.h) with a one-pixel ChromeDim rule under its whole length, and the strip text on
// the info line in the look of the notification strip. Nothing is drawn while ViewState::power.show is empty. The words are power_text(); no new colour, no new pair.
void render_power(const ViewState& view, ui::Renderer& r) noexcept;
const char* power_text(core::PowerText text, bool unsaved) noexcept;

// The Navigator's grid: columns (the rows follow from the registry's size).
inline constexpr int kNavigatorColumns = 4;

class LearnOverlay final : public Overlay {
 public:
  constexpr LearnOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::Learn; }
  // The list of bindings (app/learn_list.h): a cursor along it, a window of kLearnListRows of it on screen.
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override { return view.learn.cursor; }
  int page() const noexcept override;
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
};
extern const LearnOverlay kLearnOverlay;

// LEARN MAP (OS-LEARNMAP, docs/specs/OS-LEARNMAP.md): every Learn binding plus the encoder's target as one table, the ENC row first. Up / Down and the encoder's row keys move the row, Left / Right the
// column (step), the encoder edits the column (lmap.edit). Full-screen like the Events list; modal, context `learnmap`.
inline constexpr int kLearnMapRows = 15;  // data rows in view (the cover is the header band plus the 16 grid rows; the title and the captions take two)

class LearnMapOverlay final : public Overlay {
 public:
  constexpr LearnMapOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::LearnMap; }
  int count(const Model& model, const ViewState& view) const noexcept override;  // the ENC row plus the bindings
  int position(const ViewState& view) const noexcept override;
  int page() const noexcept override { return kLearnMapRows; }
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
};
extern const LearnMapOverlay kLearnMapOverlay;

// The Navigator's own F-key row (OS-NAV2, OS-KEYROWDLG). While the grid is up its F8 is GO, not the screen's own F8 (UNDO, ...; Back in the other overlays), so the row is the dialog's prompt
// (key_prompt.cpp, painted by render_scene; the screen under it draws none: ViewState::overlay_keys): F1 GO, F7 CLOSE, F8 GO, the rest blank. While Shift is held the row shows its layer, the one Shift layer a
// dialog declares (OS-SHIFTROW: no quarter jumps; Shift + F7 and F8 are CLOSE and GO, bound, as Shift may still be down from the chord that opened it; Shift + F1..F6 are bound to nothing and read "-").

class NavigatorOverlay final : public Overlay {
 public:
  constexpr NavigatorOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::Navigator; }
  // The grid of the registry: the cells are the list, read row by row; left / right a cell, up / down a row.
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override { return view.navigator; }
  int page() const noexcept override;
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
};
extern const NavigatorOverlay kNavigatorOverlay;

// The strip chooser of the Pattern screen's F1, F2 and F3: a chip with its title, the value, and what the keys do, over the info line (the
// covered rectangle is exactly that line, so the pattern beneath loses nothing else). Modal, context `chooser`.
class ChooserOverlay final : public Overlay {
 public:
  enum class Kind : std::uint8_t { Pattern, Length, Jump };
  constexpr explicit ChooserOverlay(Kind kind) noexcept : kind_(kind) {}
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::Chooser; }

 private:
  Kind kind_;
};
extern const ChooserOverlay kPatternChooser;
extern const ChooserOverlay kLengthChooser;
extern const ChooserOverlay kJumpChooser;

// The engine picker of the Instrument page's F1 (OS-012b): a dialog listing the instrument modules on the card (modules/instruments/*.tmod) and the ones the project names that
// the card does not have, greyed. Up / Down and the encoder choose, F1 binds the chosen one to the instrument in view, F8 closes. Modal, context `modulepicker`.
inline constexpr int kModulePickerRows = 10;  // entries on screen at a time; the cursor's row stays in the window (also the page of cursor.page, OS-NAV3)

class ModulePickerOverlay final : public Overlay {
 public:
  constexpr ModulePickerOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::ModulePicker; }
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override { return view.modules.picker; }
  int page() const noexcept override { return kModulePickerRows; }
  bool accelerates() const noexcept override { return true; }  // OS-AUDIT-28
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
};
extern const ModulePickerOverlay kModulePickerOverlay;

// The F5 PRESET list of a module's parameter page (OS-INSFX-ABI2 A9): the presets of the instrument module or the insert in view (app/preset_list.h says where they come from: the module's factory table now). Up / Down and
// the encoder choose, F1 LOAD writes the chosen one into every parameter of the module (preset.load), F8 closes. Modal, context `preset`.
inline constexpr int kPresetRows = 10;  // entries on screen at a time; a module has at most O3M_PRESETS_MAX (32) factory presets (also the page of cursor.page, OS-NAV3)

class PresetOverlay final : public Overlay {
 public:
  constexpr PresetOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::Preset; }
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override { return view.modules.preset; }
  int page() const noexcept override { return kPresetRows; }
  bool accelerates() const noexcept override { return true; }
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
};
extern const PresetOverlay kPresetOverlay;

// The APPS page of the verb menu (OS-H3A's TOOLS list, since OS-PTOOLS2 S6b a page of the menu below, ViewState::verb_menu.apps): a list with the engine picker's grammar. Up / Down and the encoder choose, F1 runs the entry
// (an app is launched with the Pattern's block as its context), F8 closes the menu. The items come from app/tools_list.h (the fixed entries first, a divider, then the tool apps).
inline constexpr int kToolsRows = 8;  // entries on screen at a time in the panel (the page of cursor.page, OS-NAV3: the same as the grid's 8 rows, verb_menu.h kVerbRows)

// The verb menu of the Pattern screen's F7 (OS-PTOOLS2 S3, docs/specs/OS-PTOOLS2.md section 3.2 mock-up A; S3p: the right-hand panel, x 500..800,
// beside the narrowed grid): two columns of eight rows in the panel, pads 1-16 pick a verb directly (pad n = entry n-1), the arrows and the
// encoder move a cursor in the 2x8 grid, F1 runs the verb under the cursor, F8 BACK closes. The rows come from
// app/verb_menu.h (a verb not built yet is drawn dim and runs nothing). The menu remembers its last verb (view.verb_menu). Modal, context `verbmenu`.
// The menu's APPS > row (pad 16) turns it into the APPS page (view.verb_menu.apps, kToolsRows entries): the same overlay and context, the cursor methods below follow the page.
class VerbMenuOverlay final : public Overlay {
 public:
  constexpr VerbMenuOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::VerbMenu; }
  // The 2x8 grid of verbs: the cells are read row by row (pad 1..8 the left column, 9..16 the right one); left / right a column, up / down a row.
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override { return view.verb_menu.apps ? view.modules.tools : view.verb_menu.cursor; }
  int page() const noexcept override { return static_cast<int>(kVerbRows); }  // (kToolsRows == kVerbRows: the APPS page pages like the grid)
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
};
extern const VerbMenuOverlay kVerbMenuOverlay;

// The EVENTS list of the Pattern screen (OS-MIDIREC S1b, docs/specs/OS-MIDIREC.md section 3): the TOOLS entry EVENTS opens it
// over the grid pane (the strip, the info line and the key row stay). The rows are projected from the steps on every redraw
// (model/event_list.h), the cursor and the window are positions in the projected list (events_view.h). Up / Down and the encoder
// move the row, Shift + Up / Down a page, Left / Right the field, F8 BACK lands the grid cursor on the row's step, track and
// column. Modal, context `events`.
inline constexpr int kEventsRows = 15;  // event rows in view: the cover is the header band plus the 16 grid rows, two of which the list's own header and captions take

class EventsOverlay final : public Overlay {
 public:
  constexpr EventsOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::Events; }
};
extern const EventsOverlay kEventsOverlay;

// The play mode list of the Wave screen (OS-INSTUI S4, Shift + F1 MODE): the eight play modes (play_mode.h) with the instrument's own marked. Up / Down and the encoder choose, F1 sets the chosen one through
// the validating action (project_state.cpp play_mode_set: a refusal says why on the strip), F8 closes. Modal, context `playmode`.
class PlayModeOverlay final : public Overlay {
 public:
  constexpr PlayModeOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::PlayModePicker; }
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override { return view.wave.mode_pick; }
  int page() const noexcept override { return 8; }
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
};
extern const PlayModeOverlay kPlayModeOverlay;

// The on-screen keyboard (OS-FU2): the name being typed, the four rows of keys with the focus on one, the keys the actions bind. Modal, context `textentry`.
class TextEntryOverlay final : public Overlay {
 public:
  constexpr TextEntryOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::TextEntry; }
};
extern const TextEntryOverlay kTextEntryOverlay;

// The step popup (OS-FU3, the instrument / FX / note lists of a held Instr, Fx or Note button): a panel over the right of the pane (x 600..800) that lists the 64 instruments, or the 36 FX with the description of the one chosen, or
// the chosen note. Modal, context `steppopup`; it lasts as long as the button is down (step_popup_actions.cpp).
class StepPopupOverlay final : public Overlay {
 public:
  constexpr StepPopupOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::StepPopup; }
  // OS-ZOOM: cursor.track (Note + Left / Right under the held Note) moves the Pattern cursor underneath; the popup stays open.
  bool track(const Model& model, ViewState& view, int delta) const noexcept override;
};
extern const StepPopupOverlay kStepPopupOverlay;
// Whether the instrument list of the popup lets `slot` be chosen: all of them, but with StepPopupView::samples_only (Wave, Sample edit: OS-INSTPICK) only the sample instruments; the
// rest are drawn dimmed, skipped by the arrows and the encoder and refused from a pad, and a pad that would choose one is dark (step_popup_actions.cpp, pad_lights.cpp).
bool step_popup_pickable(const Model& model, const ViewState& view, unsigned slot) noexcept;
// How many entries of the list the popup shows (OS-NAV3: a page of popup.page): the instrument list's rows, the FX list's minus the lines of its description; 0 for the Note panel, which has no list.
int step_popup_list_rows(const ui::Theme& theme, std::uint8_t kind) noexcept;
// The words after the number of an entry of the instrument list (OS-INSTRMIDI): a MIDI instrument reads "MIDI CH n" on its channel (short enough for the popup's width, with room for the number), any other instrument its name; `in` null (no such slot) gives "".
void step_popup_instrument_text(const model::Instrument* in, core::FixedString<40>& out) noexcept;

// The Fill dialog of the Pattern screen's F4 (OS-PTOOLS, OS-FILL2, fill_view.h): eight descriptor rows, the steps the rule chooses over the range, the keys. Modal, context `fill`. Like the Navigator
// it owns the F-key row (F1 FILL, F8 BACK: its prompt in key_prompt.cpp), the screen under it drawing none (ViewState::overlay_keys).

class FillOverlay final : public Overlay {
 public:
  constexpr FillOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::FillDialog; }
};
extern const FillOverlay kFillOverlay;

// The Song screen's TOOLS dialog (OS-SONGMAP, song_tools_view.h): the OP row, the row of the OP's value, the scope of the edit, the keys. Modal, context `song.tools`. It owns the F-key row (F1 APPLY, F8 BACK: its prompt in
// key_prompt.cpp), the screen under it drawing none (ViewState::overlay_keys).
class SongToolsOverlay final : public Overlay {
 public:
  constexpr SongToolsOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::SongTools; }
};
extern const SongToolsOverlay kSongToolsOverlay;

// The Pattern screen's TOOLS dialog (OS-PTOOLS2 S2, pattern_tools_view.h; S3p: the right-hand panel, x 500..800, beside the narrowed grid so the
// block stays visible while the dialog is open): the OP row (TRANSPOSE, INSTRUMENT, CLEAR), the OP's value row (AMOUNT / TO / WHAT), the footer naming
// the target and counting its steps and notes, the keys. Modal, context `patterntools`. It owns the F-key row (F1 APPLY, F8 BACK: its prompt in key_prompt.cpp), the screen under it
// drawing none (ViewState::overlay_keys).
class PatternToolsOverlay final : public Overlay {
 public:
  constexpr PatternToolsOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::PatternTools; }
};
extern const PatternToolsOverlay kPatternToolsOverlay;
// OS-PTOOLS2 S6: a small app opened from TOOLS (ViewState::modules.panel) in the same panel: its declared rows (the host draws them from the descriptors, the app keeps the input), the footer naming
// the target block and the count, and a line of hints. Drawn by ModuleAppScreen::render over the Pattern screen's narrowed grid; the F-key row is the app's own (that screen paints it).
void draw_tools_app_panel(const Model& model, const ViewState& view, ui::Renderer& r) noexcept;

// The progress of a running WAV export (OS-WAVEXP, export_view.h): the file being made, the pass, a bar of the export source's own account and F8 CANCEL. Modal, context `exportprogress`: every
// other key, the pads and the transport are swallowed (the export renders the live model, so nothing may change under it). Its F-key row is the dialog's own (F8 CANCEL, its prompt in key_prompt.cpp).

class ExportOverlay final : public Overlay {
 public:
  constexpr ExportOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::ExportProgress; }
};
extern const ExportOverlay kExportOverlay;

// The firmware update dialog of Config's INFO page (OS-FWCONFIG slice 2a, firmware_view.h): the Firmware/*.ptf list, question 1, the check's bar, question 2 and the hand-over's "do not power off". Modal, context
// `firmware`. It owns the F-key row (its prompt in key_prompt.cpp: F1 UPDATE / YES / INSTALL, F8 BACK / CANCEL, none while the hand-over runs), the screen under it drawing none (ViewState::overlay_keys).
class FirmwareOverlay final : public Overlay {
 public:
  constexpr FirmwareOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::FirmwareUpdate; }
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override;
  int page() const noexcept override;
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
};
extern const FirmwareOverlay kFirmwareOverlay;
// The help page of the screen in view (OS-HELP, help_text.h; Shift + Config): the lines of a page in a full-height dialog, a scroll bar on the right. Modal, context `help`. It is a Cursor over the lines of the page in
// view: Up / Down and the encoder scroll through nav.up / nav.down / nav.scroll, Shift + Up / Down and Shift + the encoder a screenful through cursor.page; Left / Right (step) turn the pages, with no wrap. Every
// other key and every pad closes it (closes_on_press). Its F-key row is the dialog's (F8 CLOSE).
inline constexpr int kHelpRows = 19;  // lines of text in the body (the dialog is the height of the pane)

class HelpOverlay final : public Overlay {
 public:
  constexpr HelpOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return true; }
  Context context() const noexcept override { return Context::Help; }
  bool closes_on_press() const noexcept override { return true; }
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override;
  int page() const noexcept override { return kHelpRows; }
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
};
extern const HelpOverlay kHelpOverlay;

class NotifyOverlay final : public Overlay {
 public:
  constexpr NotifyOverlay() noexcept = default;
  ui::Rect rect(const ui::Theme& theme) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept override;
  bool modal() const noexcept override { return false; }
};
extern const NotifyOverlay kNotifyOverlay;

}  // namespace o3os::app
