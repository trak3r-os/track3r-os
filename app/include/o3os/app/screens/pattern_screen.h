// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/rec_place.h>
#include <o3os/app/screens/screen.h>
#include <o3os/model/params.h>

namespace o3os::app {

// The pattern view's modes (docs/design/product-ux.md section 4; OS-009 D2: Edit, Follow, Select, Browse; OS-LIVEREC left Edit and Browse). One enum, one ModeDef row each
// (pattern_screen.cpp), one binding context each (bindings.h); the mode index is the enum's value. The first three keep the numbers of
// OS-002b; Browse is appended and is the default (Screen::default_mode()): the pads preview until Rec arms editing (edit mode off at start).
//
// Playing and the cursor. In Browse the cursor goes with the playhead (pattern_follow_play; OS-009c: edit off while the sequencer plays =
// the selection is cleared and the cursor row is the playhead's, every refresh). In Edit while the sequencer plays it records (OS-LIVEREC, rec_actions.cpp). OS-RECSWAP (maintainer, 2026-10-14; new Community OS
// behaviour): Rec enters Edit FREE (`PatternEdit::rec_free`: the cursor and the view stay where the user puts them; chip STEP) and Shift + Rec enters it FOLLOWING (the cursor rides
// the playhead; chip LIVE); OS-RECMODE2 (WATCH / STEP / LIVE) reworked it: in Edit Shift + Rec toggles the two and Rec goes back to Browse (WATCH). Shift + the arrows still make a block while the cursor follows (Browse playing,
// Edit following; OS-BUG-WATCHSEL): its end row is set by the arrows (Selection::follow), the playhead does not move it, and the block stays. Stopped, the cursor stays where it was.
//
// FOLLOW was Browse with selecting always off (PAR-093). OS-004d (maintainer decision 2026-10-12, "Browse already follows the playhead"): it is RETIRED. Its number and its
// binding context stay (nothing renumbers, a user keymap that names `pattern.follow` still loads, to no effect), but the mode has no bindings, no chip, is skipped by
// `view.mode_next` and refused by `view.mode`; no view is ever in it (mode_of() would read it as Browse).
// SELECT (the OctaMED block mark, OS-009) is RETIRED the same way (OS-LIVEREC, maintainer decision 2026-10-13): a block is made with Shift + the arrows in the mode the view is in (Edit, and Browse
// while stopped); Shift + Rec no longer cycles modes (it toggles free and following Edit). The number and the context `pattern.select` stay, with no bindings.
enum class PatternMode : std::uint8_t {
  Edit = 0,    // pads, encoder and Delete edit the step under the cursor (a selection makes them act on the block); while playing it records
  Follow = 1,  // RETIRED (OS-004d): never entered; the number is kept
  Select = 2,  // RETIRED (OS-LIVEREC): never entered; the number is kept
  Browse = 3,  // the default: pads preview, the encoder scrolls the steps, nothing edits; while the sequencer plays the cursor follows the playhead
};
inline constexpr unsigned kPatternModes = 4;
static_assert(static_cast<std::uint8_t>(PatternMode::Browse) == ViewState::kPatternDefaultMode,
              "ViewState starts the Pattern screen in the mode Screen::default_mode() names");

// Edit actions are enabled in Edit only (F4, F6, F7, F8, Copy, Insert and Delete are unbound outside edit mode).
constexpr bool pattern_mode_edits(unsigned mode) noexcept { return mode == static_cast<unsigned>(PatternMode::Edit); }
// OS-RECSIGN (new Community OS behaviour): THE predicate for "the pattern can be edited now": the Pattern screen in Edit (STEP and LIVE alike). The edit actions' enabled test
// (pe::edit_enabled, app/src/pattern_edit_util.h) and the REC signs (the strip badge and the Rec frame, pattern_screen.cpp) both call it, so they cannot disagree.
constexpr bool pattern_editable(const ViewState& view) noexcept {
  return view.screen == ScreenId::Pattern && pattern_mode_edits(view.mode[screen_index(ScreenId::Pattern)]);
}

// OS-RECMODE2 (maintainer, 2026-10-14; new Community OS behaviour, replaces OS-RECSWAP's mapping): three states, each a chip of the info line. WATCH = Browse (the view follows the playhead, nothing is written).
// STEP = Edit with `rec_free` (REC: notes go to the cursor, even while playing; the arrows move it). LIVE = Edit without it (Shift + REC: notes go to the playhead, the view follows; it exists only while the
// transport runs, and ends, to WATCH, when it stops). REC toggles STEP; Shift + REC toggles LIVE; the count-in is Shift + REC from stopped. OS-LIVEEXIT: inside LIVE the keys swap, REC goes to WATCH and Shift + REC to STEP.
// The cursor goes with the playhead while the sequencer plays (OS-009c, OS-LIVEREC): in Browse (and in a retired mode, which the screen reads as Browse), and in LIVE.
constexpr bool pattern_cursor_follows(const ViewState& view) noexcept {
  return view.mode[screen_index(ScreenId::Pattern)] != static_cast<unsigned>(PatternMode::Edit) || !view.edit.rec_free;
}

// Recording (OS-LIVEREC): the Pattern screen in Edit with the sequencer playing and a document to write into. As rec_active() (rec_actions.cpp), without the editor.
// OS-METRO: not during the count-in of a recording (pattern_counting_in), which counts on row 1 with the pattern silent: the take begins with row 1. The exception is the last half step of the count-in under REC QUANTIZE
// Nearest (the default): a hit there is the start step's, rounded up as at any step boundary (rec_place.h quantize_row; spec section 2), so the take, and its undo unit, begin with it. The REC badge
// (pattern_screen.cpp) waits for the start point all the same.
inline bool pattern_recording(const Model& model, const ViewState& view) noexcept {
  if (model.project == nullptr || view.screen != ScreenId::Pattern || view.mode[screen_index(ScreenId::Pattern)] != static_cast<std::uint8_t>(PatternMode::Edit) || view.edit.rec_free ||
      !view.play.playing) {  // OS-RECMODE2: only LIVE (Edit following) records; STEP (Edit free) writes at the cursor
    return false;
  }
  if (view.play.count_in == 0u) {
    return true;
  }
  const bool playing_step = model.device.kind() == core::ParamKind::Device && model.device.get(model::device_setting::RecQuantize) >= 1.0f;  // (a bare Model has the default: Nearest)
  return view.play.count_in == 1u && !playing_step && view.play.ustep >= rec::kUsteps / 2;
}
// OS-METRO: Edit is armed and the transport is counting in (COUNT-IN, Shift + Rec from stop: LIVE, OS-RECMODE2): the play line waits on row 1 and the pattern is silent, nothing is recorded yet, a pad only sounds.
inline bool pattern_counting_in(const Model& model, const ViewState& view) noexcept {
  return model.project != nullptr && view.screen == ScreenId::Pattern && view.mode[screen_index(ScreenId::Pattern)] == static_cast<std::uint8_t>(PatternMode::Edit) && !view.edit.rec_free &&
         view.play.playing && view.play.count_in != 0u && !pattern_recording(model, view);
}

// F1..F8 of the pattern screen (product-ux section 4). One table: the key row draws it and the F-key placeholder says it.
// (OS-PADS-A: FILL and EXPORT moved to Shift + Fx; OS-CFGWIRE: Shift + Fx is pattern.fill, Shift + Copy pattern.export, F6 was INVERT; OS-PTOOLS: F4 is FILL, the dialog (Shift + Fx repeated the last fill until OS-QJUMP took the key for the 3/4 jump: pattern.fill has no key now); F7 is TOOLS (OS-H3A: the list of the tool apps; OS-WAVEXP: EXPORT PATTERN is its first entry); OS-PTOOLS2 S3 (Q1) made F6 the REPEAT of the last parameterised apply (RE FILL, REPEAT after a TOOLS APPLY); OS-F6EVENTS (new Community OS behaviour): F6 is EVENTS, the list opens directly, and RE FILL / REPEAT (tools.repeat) moved to F6 of the verb menu (F7 then F6), whose key row reads the label of the last verb (key_prompt.cpp); INVERT took the menu's pad 9.)
inline constexpr const char* kPatternSoftKeys[8] = {"PATTERN", "LENGTH", "JUMP", "FILL", "PREVIEW", "EVENTS", "TOOLS", "UNDO"};
// What the key row shows while Shift is held (OS-PADS-A, OS-PADOCT): Shift + F1 the pads' octave (the label is the octave in force, "C3": PatternScreen::shift_row swaps it in; this table
// is the row's shape), Shift + F2 the PADS picker (OS-UXF-PAT 7: an overlay whose F1..F6 pick the pads' mode, pads_picker_overlay.cpp; the modes had Shift + F2..F7), and Shift + F8 redo.
// Shift + F3..F7 are free and blank. The STEP pads have no key: the mode is off until OS-STEPPAD (OS-STEPPADOFF). The mute labels and the quarter jumps this row had are gone.
inline constexpr const char* kPatternShiftKeys[8] = {"OCTAVE", "PADS", "-", "-", "-", "-", "-", "REDO"};

// The first registered screen: PatternView (pattern_view.h) behind the Screen interface.
class PatternScreen final : public Screen {
 public:
  constexpr PatternScreen() noexcept = default;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::Pattern; }
  Context context() const noexcept override { return Context::Pattern; }
  ModeTable modes() const noexcept override;
  unsigned default_mode() const noexcept override { return static_cast<unsigned>(PatternMode::Browse); }
  Context field_context(const Model& model, const ViewState& view) const noexcept override;
  const char* const* soft_keys() const noexcept override { return kPatternSoftKeys; }
  const char* const* soft_keys_shifted() const noexcept override { return kPatternShiftKeys; }
  const char* const* shift_row(const ViewState& view) const noexcept override;  // OS-PADOCT: F1 says the octave
  // The cursor: the step (the list, as long as the pattern), the columns and the tracks (the second axis), the 16 visible steps a
  // page. Every move through it collapses the selection (a plain move).
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override { return view.pattern.row(); }
  int page() const noexcept override { return PatternView::kVisibleRows; }
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
  bool step_press(const Model& model, ViewState& view, int dx, int dy) const noexcept override;  // OS-ROWWRAP: Up on the first row goes to the last, Down on the last to the first
  bool track(const Model& model, ViewState& view, int delta) const noexcept override;
  bool home(const Model& model, ViewState& view) const noexcept override;
  bool end(const Model& model, ViewState& view) const noexcept override;
};
extern const PatternScreen kPatternScreen;

// The device settings the Pattern view draws by (OS-CFGWIRE): NUMBERING as a `Numbering` value, and MIDDLE C as the octave shift of the note names (0 = the default names).
unsigned pattern_numbering(const Model& model) noexcept;
int pattern_octave_shift(const Model& model) noexcept;
// The extent of the pattern in view: its length and the active tracks.
PatternView::Dims pattern_dims(const Model& model, const ViewState& view) noexcept;
// Puts the cursor, the window and the selection back inside the pattern in view (it may have changed under them: another pattern, a
// new length, an undo). True if anything moved.
bool pattern_sync(const Model& model, ViewState& view) noexcept;
// Changes the Pattern screen's mode. Leaving Edit drops the free state (the next Edit is entered afresh). True if the mode changed.
bool pattern_set_mode(ViewState& view, PatternMode mode) noexcept;

// The cursor follows the playhead (OS-009b, in Browse since OS-009c, in Edit too since OS-LIVEREC): it goes to the step the sequencer is on, and the window with it,
// and a block is dropped. When the song moves to another pattern (OS-005e) the view goes to that pattern first, if the project
// has it. Nothing in free Edit, on another screen, or while stopped. True if the view, the cursor or the block changed. The App calls it when
// the position changes, so a key pressed in between moves the cursor until the next step.
bool pattern_follow_play(const Model& model, ViewState& view) noexcept;

}  // namespace o3os::app
