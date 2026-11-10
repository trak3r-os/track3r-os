// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/pattern_screen.h>

#include <o3os/app/frame.h>
#include <o3os/app/fx_text.h>
#include <o3os/app/pattern_source.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/scales.h>
#include <o3os/model/event_list.h>
#include <o3os/model/params.h>

namespace o3os::app {

namespace {

// One row per PatternMode, in the enum's order: the name, the binding context of the mode. Browse and Edit follow the playhead while it plays, but not a free recording
// (pattern_cursor_follows, screens/pattern_screen.h: OS-009c, OS-LIVEREC). Follow (OS-004d) and Select (OS-LIVEREC) are retired: their rows stay for the numbering.
constexpr ModeDef kModes[kPatternModes] = {
    {"edit", Context::PatternEdit},
    {"follow", Context::PatternFollow, true},
    {"select", Context::PatternSelect, true},
    {"browse", Context::PatternBrowse},
};

// The mode chip of the info line, in the order of PatternMode (the retired ones, which mode_of() never returns, read BROWSE).
// OS-RECSWAP: Edit names its state, FOLLOW (the cursor rides the playhead: Shift + Rec) or FREE (the cursor and the view stay: Rec); both fit the info line with room to spare.
constexpr const char* kChips[kPatternModes] = {"LIVE", "WATCH", "WATCH", "WATCH"};  // OS-RECMODE2: Edit following is LIVE; Edit free is STEP (kChipFree)
constexpr const char* kChipFree = "STEP";

unsigned mode_of(const ViewState& view) noexcept {
  const unsigned mode = view.mode[screen_index(ScreenId::Pattern)];
  return mode < kPatternModes && !kModes[mode].retired ? mode : static_cast<unsigned>(PatternMode::Browse);
}

// A plain move collapses the selection. True if there was one to collapse.
bool collapse(ViewState& view) noexcept {
  const bool was = view.edit.selection.active;
  view.edit.selection.clear();
  return was;
}

// The Shift row for each octave of the pads (OS-PADOCT): kPatternShiftKeys with F1 saying the octave in force, "C0".."C9" (the C the lowest NOTES pad starts on; the same number as the held-Note OCTAVE row).
// Built at compile time: 10 rows of 8 pointers, no run-time buffer to keep in step with the view.
constexpr const char* kOctaveLabels[kEntryOctaves] = {"C0", "C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9"};
struct OctaveRows {
  const char* row[kEntryOctaves][8];
};
constexpr OctaveRows make_octave_rows() noexcept {
  OctaveRows t{};
  for (unsigned o = 0; o < kEntryOctaves; ++o) {
    t.row[o][0] = kOctaveLabels[o];
    for (unsigned k = 1; k < 8u; ++k) {
      t.row[o][k] = kPatternShiftKeys[k];
    }
  }
  return t;
}
constexpr OctaveRows kOctaveRows = make_octave_rows();

}  // namespace

ModeTable PatternScreen::modes() const noexcept { return ModeTable{kModes, kPatternModes}; }

const char* const* PatternScreen::shift_row(const ViewState& view) const noexcept {
  const unsigned o = view.edit.entry.octave;
  return kOctaveRows.row[o < kEntryOctaves ? o : kEntryOctaves - 1u];
}

Context PatternScreen::field_context(const Model&, const ViewState& view) const noexcept {
  switch (view.pattern.column()) {
    case Column::Note:
      return Context::PatternNote;
    case Column::Instr:
      return Context::PatternInstr;
    case Column::Vol:
      return Context::PatternVol;
    case Column::Fx1:
    case Column::Fx2:
      break;
  }
  return Context::PatternFx;
}

unsigned pattern_numbering(const Model& model) noexcept {
  if (model.device.kind() != core::ParamKind::Device) {  // a bare Model (the simulator's scenes, a test) has no settings block: the defaults
    return static_cast<unsigned>(Numbering::From1);
  }
  const float v = model.device.get(model::device_setting::Numbering);
  return v >= 2.0f ? static_cast<unsigned>(Numbering::Hex) : (v >= 1.0f ? static_cast<unsigned>(Numbering::From1) : static_cast<unsigned>(Numbering::From0));
}

// The setting is the name of MIDI note 60 (C-3..C-6): C-5 is the name that note names starting at C-0 give it, so the shift is the index - 2 (model/params.cpp kMiddleC).
int pattern_octave_shift(const Model& model) noexcept {
  if (model.device.kind() != core::ParamKind::Device) {
    return 0;
  }
  const float v = model.device.get(model::device_setting::MiddleC);
  return (v >= 3.0f ? 3 : (v >= 2.0f ? 2 : (v >= 1.0f ? 1 : 0))) - 2;
}

PatternView::Dims pattern_dims(const Model& model, const ViewState& view) noexcept {
  const PatternSource src(model, view.edit.pattern);
  return PatternView::Dims{src.length(), src.tracks()};
}

bool pattern_sync(const Model& model, ViewState& view) noexcept {
  const PatternView::Dims d = pattern_dims(model, view);
  bool changed = view.pattern.clamp_to(d);
  Selection& sel = view.edit.selection;
  if (sel.active) {  // the block must lie inside what exists: a shorter pattern or fewer tracks ends it
    const Selection::Block b = sel.block(view.pattern.row(), view.pattern.track());
    if (b.row1 >= d.steps || b.track1 >= d.tracks) {
      sel.clear();
      changed = true;
    }
  }
  return changed;
}

bool pattern_set_mode(ViewState& view, PatternMode mode) noexcept {
  std::uint8_t& current = view.mode[screen_index(ScreenId::Pattern)];
  const std::uint8_t next = static_cast<std::uint8_t>(mode);
  if (current == next) {
    return false;
  }
  if (current == static_cast<std::uint8_t>(PatternMode::Edit)) {
    view.edit.rec_free = false;  // free is a state of Edit (Rec enters it, Shift + Rec toggles it); the next Edit is entered afresh
  }
  current = next;
  return true;
}

void PatternScreen::render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept {
  const PatternSource src(model, view.edit.pattern);
  const unsigned mode = mode_of(view);
  const bool shift = view.shift && view.overlay == OverlayId::None;  // under an overlay the keys are the overlay's, not the layer's
  PatternView::Status status{view.instrument, view.overlay_keys() ? nullptr : (shift ? shift_row(view) : kPatternSoftKeys)};  // no labels: the Navigator's row is the one drawn (OS-SMALL2)
  const bool edit = mode == static_cast<unsigned>(PatternMode::Edit);
  status.chip = edit && view.edit.rec_free ? kChipFree : kChips[mode];
  status.chip_tone = !edit ? PatternView::Status::kToneNone : (view.edit.rec_free ? PatternView::Status::kToneStep : PatternView::Status::kToneLive);
  if (src.is_demo()) {
    status.instrument_name = instrument_name(view.instrument);
  } else {
    const model::Instrument* in = view.instrument >= 1 ? src.project()->instrument(static_cast<std::uint32_t>(view.instrument - 1)) : nullptr;
    status.instrument_name = in != nullptr ? in->name : "";
    status.jump = view.edit.edit_step;
  }
  status.scale = view.edit.entry.scale_name();  // OS-PADS-B: the note-entry settings (note_entry.h), not the project file's pad board scale
  status.root = static_cast<std::uint8_t>(view.edit.entry.board_root());
  status.undo = model.undo_depth + (view.edit.take_wrote ? 1u : 0u);  // the take in progress is the level Undo would take
  status.pattern = src.strip_pattern();
  status.patterns = src.strip_patterns();
  status.pattern_name = src.strip_name();
  status.selection = view.edit.selection;
  status.shift = shift;
  if (shift) {  // the Shift picker: the reserved modes dim, the mode that stays inverted
    status.soft_dim = kPatternPickerDim;
    const int key = kPadModeKey[static_cast<unsigned>(view.padmode.fixed())];  // (STEP has no key)
    status.soft_hot = key >= 0 ? 1u << static_cast<unsigned>(key) : 0u;
  }
  status.pads = view.padmode.name();  // OS-INFOLINE: every mode is named on the info line, NOTES too
  status.playing = view.playing;
  // OS-RECSIGN (new Community OS behaviour): the REC signs (the strip badge and the Rec frame) show whenever the pattern can be edited (pattern_editable, the edit actions' predicate), STEP included, with a
  // document to write into. The Rec playhead bar stays for the take itself, as rec_active() (rec_actions.cpp): LIVE (OS-RECMODE2: Edit following, not STEP) with the sequencer playing (OS-METRO: not red until the count-in is over).
  status.rec = !src.is_demo() && pattern_editable(view);
  status.recording = status.rec && !view.edit.rec_free && view.play.playing && view.play.count_in == 0u;
  status.metro = model.device.kind() == core::ParamKind::Device && model.device.get(model::device_setting::MetroMode) != 0.0f;  // the METRO chip of the info line (UI map rule 7)
  status.load = model.load;
  status.blink = view.mute_blink;
  status.numbering = static_cast<std::uint8_t>(pattern_numbering(model));
  status.octave_shift = static_cast<std::int8_t>(pattern_octave_shift(model));
  const Column column = view.pattern.column();
  // OS-PTOOLS2 S1 (spec 3.1 item 7): in Edit, while a block is active, the info line says what it is and what the keys do to it, so the encoder's transpose is no longer hidden: the encoder acts on the
  // cursor's column (TRANSPOSE on Note, INSTRUMENT, VOLUME, FX VALUE; Shift: the coarse step, OCTAVE on Note). The parts the line has no room for go from the end, so F7 comes before PAD and DEL.
  // Outside Edit (Browse / WATCH, where S1 lets F7 open with a block) the hint is not shown: ENC, PAD and DEL all need Edit, so the line would advertise keys that say TURN EDIT ON.
  core::FixedString<96> hint;
  if (!src.is_demo() && view.overlay == OverlayId::None && pattern_editable(view) && view.edit.selection.active) {
    const Selection::Block b = view.edit.selection.block(view.pattern.row(), view.pattern.track());
    hint.assign("BLOCK ");
    append_uint(hint, static_cast<unsigned>(b.row1 - b.row0 + 1));
    hint.append(b.row1 == b.row0 ? " STEP" : " STEPS");
    if (b.track1 > b.track0) {
      hint.append(", ");
      append_uint(hint, static_cast<unsigned>(b.track1 - b.track0 + 1));
      hint.append(" TRACKS");
    }
    hint.append("  ENC: ");
    switch (column) {
      case Column::Note: hint.append(shift ? "OCTAVE" : "TRANSPOSE"); break;
      case Column::Instr: hint.append(shift ? "INSTR x8" : "INSTRUMENT"); break;
      case Column::Vol: hint.append("VOLUME"); break;
      case Column::Fx1:
      case Column::Fx2: hint.append("FX VALUE"); break;
    }
    hint.append("  F7: TOOLS  PAD: SET  DEL: CLEAR");
    status.hint = hint.c_str();
  }
  // OS-PFX: the FX under the cursor in words (Chord, Arp, Tune, Slide), on the info line.
  core::FixedString<24> fx_read;
  if (column == Column::Fx1 || column == Column::Fx2) {
    const model::StepFx& f = src.step(view.pattern.track(), view.pattern.row()).fx[column == Column::Fx1 ? 0 : 1];
    if (append_fx_value(fx_read, f.type, f.value)) {
      status.fx_read = fx_read.c_str();
    }
  }
  // OS-MIDIREC S1c (QA-PAT-406): under the EVENTS list the info line names the cursor row's other FX — "ALSO: CHORD 57B": every FX
  // on its step that the row does not show by itself (not a CC, which has its own row; not the Micro-move, which is the row's
  // TIME; the Gate is the G flag and has no words).
  core::FixedString<48> events_also;
  if (view.overlay == OverlayId::Events && model.project != nullptr) {
    model::events::Row row;
    if (events_cursor_row(*model.project, view.edit.pattern, view.events, row)) {
      const model::Pattern* pat = model.project->pattern(view.edit.pattern);
      if (pat != nullptr) {
        const model::Step& st = pat->steps[row.key.track][row.key.step];
        events_also.assign("ALSO:");
        bool any = false;
        for (unsigned c = 0; c < 2u; ++c) {
          const model::StepFx& f = st.fx[c];
          if (f.type == model::kFxNone || model::events::is_cc_type(f.type) || f.type == model::events::kFxMicroMove) {
            continue;
          }
          core::FixedString<40> word;
          if (append_fx_value(word, f.type, f.value)) {
            events_also.append(any ? ", " : " ");
            events_also.append(word.c_str());
            any = true;
          }
        }
        if (any) {
          status.fx_read = events_also.c_str();
        }
      }
    }
  }
  // The sequencer's position, for the pattern that plays only (the demo has no sequencer); the BPM is the one it uses while it plays, the project's
  // otherwise (what the next Play starts with).
  if (!src.is_demo()) {
    status.play_row = play_row(view.play, src.id());
    status.tempo_x100 = view.play.playing && view.play.tempo_x100 != 0u ? view.play.tempo_x100 : src.project()->tempo_x100();
  }
  // OS-PTOOLS2 S3p: while the right-hand panel is up (the F7 verb menu, the TOOLS dialog) the grid is drawn in the 500 px left of it, so
  // three tracks stay visible at the 5-track zoom and the block and the cursor stay in view while the panel is open.
  const bool panel = view.overlay == OverlayId::VerbMenu || view.overlay == OverlayId::PatternTools || (view.screen == ScreenId::ModuleApp && view.modules.panel);  // (S6: a small app in the panel)
  view.pattern.draw(r, src, status, covered, panel ? kRightPanelX : ui::kScreenW);
}

bool pattern_follow_play(const Model& model, ViewState& view) noexcept {
  if (view.screen != ScreenId::Pattern || !pattern_cursor_follows(view) || model.project == nullptr || !view.play.playing) {
    return false;
  }
  if (view.overlay == OverlayId::Fill) {
    // OS-PTOOLS2 S3c (review): with the Fill dialog open the target is the selection as it was when the dialog opened. The playhead neither moves the cursor nor
    // switches the pattern under it — this function is the only runtime path that changes the pattern in view while the dialog is up (the chooser, the pads and the
    // screen keys are captured by the modal; App::open_pattern runs at boot only), so the fill's target cannot change behind the dialog.
    return false;
  }
  if (view.edit.queued_pattern != 0u && view.edit.chooser_held && view.overlay == OverlayId::PatternChooser) {
    return false;  // E-PAT-35: PAT SWITCH = Pattern end: the held chooser keeps the view on its pick while the queue waits
  }
  if (view.edit.switch_to != 0u) {
    return false;  // OS-PATSWITCH: a pick of the held PATTERN chooser waits for its step boundary: the view stays on the pick, it is not pulled back to the pattern that still plays
  }
  // The block is cleared while it plays with edit off; the playhead must not be able to extend one either (PAR-093). A block the user made while following (Shift + the arrows,
  // Selection::follow: its end row is the arrows', not the cursor's) stays: the playhead does not move it.
  bool changed = view.edit.selection.follow ? false : collapse(view);
  // The song moves to another pattern (or a Song Play began on one), or a queued or switched pattern started: the view goes with it. A pattern the project lacks (a song may name one, a pad of PATS or the held
  // chooser may choose an unused one) plays as empty at the default length and shows as an empty grid with the play bar running through it: the view is the pattern's, nothing is created (OS-PATSWITCH).
  const unsigned playing = view.play.pattern;
  if (playing != view.edit.pattern && playing >= 1u) {
    view.edit.pattern = static_cast<std::uint8_t>(playing);
    view.edit.selection.clear();  // (its rows belong to the pattern it was made in)
    changed = true;
  }
  const int row = play_row(view.play, view.edit.pattern);
  if (row >= 0 && view.pattern.jump_row(row, pattern_dims(model, view))) {
    changed = true;
  } else if (changed) {
    (void)pattern_sync(model, view);  // the cursor stays where it was, inside the new pattern
  }
  return changed;
}

int PatternScreen::count(const Model& model, const ViewState& view) const noexcept { return pattern_dims(model, view).steps; }

bool PatternScreen::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const bool moved = view.pattern.jump_row(position, pattern_dims(model, view));
  return collapse(view) || moved;
}

bool PatternScreen::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  const PatternView::Dims d = pattern_dims(model, view);
  bool moved = false;
  if (dx != 0) {
    // OS-LIVEREC: while it records, Left / Right change the recording track (the cursor's), a track at a time, the column staying; the columns are the buttons' (Note, Instr, Vol, Fx).
    if (pattern_recording(model, view)) {
      moved = view.pattern.move_track(dx, d);
    } else {
      moved = dx < 0 ? view.pattern.move_left(d) : view.pattern.move_right(d);
    }
  } else if (dy != 0) {
    moved = dy < 0 ? view.pattern.move_up(d) : view.pattern.move_down(d);
  }
  return collapse(view) || moved;
}

// OS-ROWWRAP (new Community OS behaviour; the official firmware's behaviour is an open research question): a fresh Up on the first row goes to the last row of the same pattern, a fresh Down on the last row to the first (the pattern's own
// length, not 64). Only the plain arrows and only on a press: App passes an auto-repeat to step(), so a held arrow stops at the edge. Not while the cursor follows the playhead (Browse
// and LIVE while playing: the cursor is the playhead's, a wrap would be undone at once), and the block extension (Shift + the arrows, select.extend) clamps as before.
bool PatternScreen::step_press(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  if (dx == 0 && dy != 0 && !(view.play.playing && pattern_cursor_follows(view))) {
    const PatternView::Dims d = pattern_dims(model, view);
    const int row = view.pattern.row();
    if (d.steps > 1 && ((dy < 0 && row == 0) || (dy > 0 && row == d.steps - 1))) {
      const bool moved = view.pattern.jump_row(dy < 0 ? d.steps - 1 : 0, d);
      return collapse(view) || moved;
    }
  }
  return step(model, view, dx, dy);
}

bool PatternScreen::track(const Model& model, ViewState& view, int delta) const noexcept {
  const bool moved = view.pattern.move_track(delta, pattern_dims(model, view));
  return collapse(view) || moved;
}

bool PatternScreen::home(const Model& model, ViewState& view) const noexcept {
  const bool moved = view.pattern.home(pattern_dims(model, view));
  return collapse(view) || moved;
}

bool PatternScreen::end(const Model& model, ViewState& view) const noexcept {
  const bool moved = view.pattern.end(pattern_dims(model, view));
  return collapse(view) || moved;
}

constexpr PatternScreen kPatternScreen{};

}  // namespace o3os::app
