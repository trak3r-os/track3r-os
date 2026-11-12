// SPDX-License-Identifier: GPL-3.0-or-later
// The pads' modes beyond NOTES and TRACKS (OS-PADS-B; new Community OS behaviour, pad_layer.h): the one action pads.play that a pad of a fixed PIANO / ISO / ... mode runs, by the mode
// (the context PadsMode binds all 48 pads to it, so a mode costs no bindings). The maps are pad_maps.h's, the LEDs pad_lights.cpp's.
#include "param_access.h"
#include "pattern_edit_util.h"

#include <o3os/app/pad_lights.h>
#include <o3os/app/pad_maps.h>
#include <o3os/app/pad_note.h>

#include <cstring>

namespace o3os::app {

namespace {

// A note pad of PIANO / ISO: a Browse pad sounds, Edit enters the note in the cursor step as the NOTES pads do, Rec (Edit while it plays) writes it at the playhead (rec_note, OS-LIVEREC). The pad's own
// sound is the live note's, held until its release (App::run_once ends it, and the Rec note with the note remembered here: the octave may change under a held pad).
bool play_note(ActionContext& ctx, unsigned pad, unsigned note) noexcept {
  ViewState& v = ctx.view;
  // A beat slice instrument has no layout (OS-SLICEFIX): the pad is the slice, note 24 + pad, whatever the octave or the scale lock say.
  note = beat_slice_instrument(ctx.model.project, v.instrument) ? audio::kSliceRootNote + pad : v.edit.entry.locked(note);
  if (note > kEntryMaxNote) {
    return false;
  }
  v.edit.last_note = static_cast<std::uint8_t>(note);
  v.edit.pad_held[pad] = static_cast<std::uint8_t>(note + 1u);
  bool changed = false;
  if (rec_active(ctx)) {
    changed = rec_note(ctx, note, 0u);
  } else if (rec_counting_in(ctx)) {
    // the count-in (OS-METRO): the pad sounds, nothing is written yet
  } else if (pe::edit_enabled(ctx)) {
    changed = step_note(ctx, static_cast<core::ActionArg>(note + 1u));
  }
  if (ctx.preview != nullptr) {
    ctx.preview->pad_down(pad, static_cast<unsigned>(v.pattern.track()), static_cast<unsigned>(v.instrument), note);
  }
  return changed;
}

bool octave_by(ViewState& v, int by) noexcept {
  const int to = pe::clampi(static_cast<int>(v.edit.entry.octave) + by, 0, static_cast<int>(kEntryOctaves) - 1);
  if (to == v.edit.entry.octave) {
    return false;
  }
  v.edit.entry.octave = static_cast<std::uint8_t>(to);
  v.edit.apply_entry();
  return true;
}

bool play_fn(ActionContext& ctx, unsigned pad, PadFn fn) noexcept {
  switch (fn.kind) {
    case PadKey::Note:
      return (beat_slice_instrument(ctx.model.project, ctx.view.instrument) || pad_fn_note(ctx.view.edit.entry, fn) != kNoNote) && play_note(ctx, pad, pad_fn_note(ctx.view.edit.entry, fn));
    case PadKey::Nav:
      return cursor_jump_fraction(ctx, fn.value);
    case PadKey::OctUp:
      return octave_by(ctx.view, 1);
    case PadKey::OctDown:
      return octave_by(ctx.view, -1);
    case PadKey::Bend: {  // OS-PIANOBEND: momentary, last pressed wins; the release is App::run_once's. Nothing is written to the pattern
      const int bend = ctx.view.edit.bend.press(pad, bend_semitones(fn));
      if (ctx.preview != nullptr) {
        ctx.preview->pad_bend(bend, static_cast<unsigned>(ctx.view.instrument));
      }
      return false;
    }
    default:
      return false;
  }
}

// ---- STEP -------------------------------------------------------------------------------------------------------------------------------------
// The pads are the cursor track's steps (pad_maps.h: 48 to a page, the page of the cursor's row). A tap on an empty step writes the last note played (a pad, the MIDI keyboard; the default
// before any) with the current instrument; a tap on a lit one clears it, at the release, unless the encoder changed it while the pad was held; a held step plus the encoder changes its note
// (by semitones, by the notes of the scale with SCALE LOCK on). Every write is one undo unit through the Editor (the action's own); none of it sounds.

// Rewrites one step of the cursor's track (an absolute row of `track`) with `fn(step, last)`. True when it changed.
template <class Fn>
bool edit_one(ActionContext& ctx, int track, int row, Fn fn) noexcept {
  ViewState& v = ctx.view;
  const PatternSource src(ctx.model, v.edit.pattern);
  if (ctx.edit == nullptr || row < 0 || row >= src.track_length(track)) {
    return false;
  }
  model::Step s = src.step(track, row);
  const model::Step before = s;
  model::edit::Last last{static_cast<std::uint8_t>(pe::clampi(v.instrument, 1, static_cast<int>(model::kMaxInstruments))), v.edit.last_fx};
  fn(s, last);
  if (std::memcmp(&s, &before, sizeof s) == 0) {
    return false;
  }
  bool exists = src.exists();
  if (!pe::create_if_missing(ctx, exists, static_cast<unsigned>(src.length()))) {
    return false;
  }
  v.instrument = last.instrument;
  return ctx.edit->set_steps(v.edit.pattern, static_cast<std::uint32_t>(track), static_cast<std::uint32_t>(row), &s, 1) == model::Status::Ok;
}

bool step_pad_down(ActionContext& ctx, unsigned pad) noexcept {
  ViewState& v = ctx.view;
  const int track = v.pattern.track();
  const int row = step_row(v.pattern.row(), pad);
  const PatternSource src(ctx.model, v.edit.pattern);
  if (ctx.edit == nullptr || row >= src.track_length(track)) {
    return false;
  }
  StepHold& hold = v.edit.step_hold;
  hold = StepHold{static_cast<std::int16_t>(row), static_cast<std::int8_t>(track), static_cast<std::uint8_t>(pad), false};
  if (src.step(track, row).note != model::kNoteEmpty) {
    return false;  // lit: the release clears it unless the encoder changes it
  }
  const unsigned note = v.edit.last_note != kNoNote ? v.edit.last_note : model::edit::kDefaultNote;
  hold.edited = true;  // a fresh step is not cleared by its own release; held and turned it is adjusted
  return edit_one(ctx, track, row, [&](model::Step& s, model::edit::Last& l) { model::edit::note_set(s, note + 1u, model::edit::Rule{true, true}, l); });
}

bool step_scale_delta(model::Step& s, const NoteEntry& e, int delta) noexcept {
  if (e.lock == 0u || s.note < 1u || s.note > 128u) {
    return false;
  }
  unsigned note = s.note - 1u;
  for (int k = 0, n = delta < 0 ? -delta : delta; k < n && k < 12; ++k) {
    note = e.next(note, delta);
  }
  s.note = static_cast<std::uint8_t>(note + 1u);
  return true;
}

}  // namespace

// ---- PATS -------------------------------------------------------------------------------------------------------------------------------------
// The pads are patterns 1..48. Playing, a tap queues that pattern at the end of the one now playing (TransportSink::queue_pattern: the sequencer's QueuePattern, after which the transport
// loops it) and a second tap on the queued one cancels it; stopped there is nothing to queue behind, so the tap selects the pattern for editing (the F1 chooser's choice).
// The tap on pattern `id` (1..255): also the DJ page's pattern row (OS-DJ A4).
bool pattern_queue_tap(ActionContext& ctx, unsigned id) noexcept {
  ViewState& v = ctx.view;
  if (id < 1u || id > 255u) {
    return false;
  }
  if (!v.playing && !v.play.playing) {
    if (ctx.model.project == nullptr || v.edit.pattern == id) {
      return false;
    }
    v.edit.pattern = static_cast<std::uint8_t>(id);
    v.edit.selection.clear();
    return true;
  }
  if (ctx.transport == nullptr) {
    return false;
  }
  const bool cancel = v.edit.queued_pattern == id;
  if (!ctx.transport->queue_pattern(cancel ? 0u : id)) {
    return false;  // no sequencer behind the sink, or its ring is full: nothing changed
  }
  v.edit.queued_pattern = cancel ? std::uint8_t{0} : static_cast<std::uint8_t>(id);
  return true;
}

namespace {

bool patterns_pad(ActionContext& ctx, unsigned pad) noexcept { return pattern_queue_tap(ctx, pad + 1u); }

// ---- FADERS -----------------------------------------------------------------------------------------------------------------------------------
// The pads of VU's second page: column c is track c's volume, four rows are four levels, the top row 100 %, then 75, 50 and 25; a tap on the bottom pad when the track is already at 25 sets 0
// (there would be no other way to the bottom of the fader). The write is the generic parameter edit (write_param: the project through the Editor, one undo unit, and the same ParamChange
// to the engine as the Mixer page's). The pad lights show the level (pad_lights.cpp fader_roles).
bool fader_pad(ActionContext& ctx, unsigned pad) noexcept {
  const unsigned track = pad % 12u;
  const unsigned row = pad / 12u;
  if (ctx.model.project == nullptr || track >= ctx.model.project->active_tracks()) {
    return false;
  }
  const core::ParamId id = core::param_id(core::ParamKind::Track, track, model::track_param::Volume);
  float now = 0.0f;
  (void)read_param(ctx.model, id, now);
  const float level = static_cast<float>((4u - row) * 25u);
  return write_param(ctx, id, row == 3u && now == level ? 0.0f : level) == WriteResult::Changed;
}

}  // namespace

// Shift + F1 on Pattern (OS-PADOCT, new Community OS behaviour, maintainer 2026-10-22): the octave of the pads' base C goes up one, from the last (9) round to 0. It is the note-entry octave
// (NoteEntry::octave) that the NOTES, PIANO and ISO pads, the held-Note OCTAVE row and the PIANO octave keys share, so the board is laid out again; the key's label shows it ("C3").
bool pads_octave(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen != ScreenId::Pattern) {
    return false;
  }
  ViewState& e = ctx.view;
  e.edit.entry.octave = static_cast<std::uint8_t>((e.edit.entry.octave + 1u) % kEntryOctaves);
  e.edit.apply_entry();
  return true;
}

// A pad (arg = 0..47) in the fixed mode that is in force on the Pattern screen.
bool pads_play(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.screen != ScreenId::Pattern || arg < 0 || arg >= static_cast<core::ActionArg>(kPadCount)) {
    return false;
  }
  const unsigned pad = static_cast<unsigned>(arg);
  switch (ctx.view.padmode.fixed()) {
    case PadMode::Piano:
      return play_fn(ctx, pad, piano_pad(pad));
    case PadMode::Iso:
      return play_fn(ctx, pad, iso_pad(pad));
    case PadMode::Steps:
      return step_pad_down(ctx, pad);
    case PadMode::Patterns:
      return patterns_pad(ctx, pad);
    case PadMode::Vu:
      return ctx.view.padmode.faders() && fader_pad(ctx, pad);  // VU itself only shows
    default:
      return false;
  }
}

}  // namespace o3os::app

namespace o3os::app {

// The release of a STEP pad (arg = the pad): ends the hold, and a lit step that nothing changed is cleared (the note and the instrument, as Delete does).
bool pads_step_up(ActionContext& ctx, core::ActionArg arg) noexcept {
  StepHold& hold = ctx.view.edit.step_hold;
  if (arg < 0 || hold.row < 0 || hold.pad != static_cast<std::uint8_t>(arg)) {
    return false;
  }
  const StepHold was = hold;
  hold.row = -1;
  if (was.edited || ctx.view.padmode.fixed() != PadMode::Steps) {
    return false;
  }
  return edit_one(ctx, was.track, was.row, [](model::Step& s, model::edit::Last&) { model::edit::clear(s, model::edit::kElNote); });
}

// The encoder while a STEP pad is held (App::run_once hands it here, ahead of the bindings): the held step's note by `arg` detents.
bool pads_step_delta(ActionContext& ctx, core::ActionArg arg) noexcept {
  StepHold& hold = ctx.view.edit.step_hold;
  if (hold.row < 0) {
    return false;
  }
  const NoteEntry& e = ctx.view.edit.entry;
  const int delta = pe::clampi(arg, -1000, 1000);
  const bool changed = edit_one(ctx, hold.track, hold.row, [&](model::Step& s, model::edit::Last& l) {
    if (s.note == model::kNoteEmpty) {
      return;
    }
    if (!step_scale_delta(s, e, delta)) {
      model::edit::note_delta(s, delta, model::edit::Rule{true, true}, l);
    }
  });
  hold.edited = true;  // turned: the release leaves it
  return changed;
}

}  // namespace o3os::app
