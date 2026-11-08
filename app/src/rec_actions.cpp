// SPDX-License-Identifier: GPL-3.0-or-later
// The live recording of the Pattern screen (OS-FU2, redone by OS-LIVEREC: the maintainer's design, new Community OS behaviour; rec_place.h has the rules and what they replace). While the pattern
// plays with Edit on, the notes of the pads (Note column) and of the MIDI keyboard are written at the PLAYHEAD's step (rounded by REC QUANTIZE) of the CURSOR's track, over what the step holds
// (REC POLY Spill puts the notes held together on to the next tracks); stopped, the notes enter the cursor's step as before (step.note_pad, step.midi_note). The notes and their OFFs of one take
// are ONE undo unit (the maintainer's whole-take rule: rec_take_sync below keeps one undo group open from the moment recording begins to the moment it ends).
//
// Main loop only (an action): the playhead is read from audio::PlayPosition by App::poll_play_position, once at the top of every pass, and nothing here runs in the audio context.
#include "param_access.h"
#include "pattern_edit_util.h"

#include <o3os/app/pad_note.h>
#include <o3os/app/rec_fx.h>
#include <o3os/app/rec_place.h>
#include <o3os/core/fixed_string.h>
#include <o3os/model/instrument.h>
#include <o3os/model/params.h>

namespace o3os::app {

namespace {

constexpr int kRecTracks = 16;  // the bits of PatternEdit::rec_open

// A choice of the device settings, as its index; a bare Model (the simulator's scenes, a test) has no settings block: the defaults.
unsigned device_choice(const Model& model, unsigned field) noexcept {
  return model.device.kind() == core::ParamKind::Device && model.device.get(field) >= 1.0f ? 1u : 0u;
}

// The step after the pattern's last is the pattern's first only when the pattern loops: in Pattern mode with nothing queued behind it. A song leaves the pattern there, and so does a queued
// pattern (a PATS pad) or a queued song jump: the next step is another pattern's.
bool pattern_loops(const ViewState& v) noexcept {
  return v.play.order == audio::PlayPosition::kNoOrder && v.edit.queued_pattern == 0u && v.play.queued == audio::PlayPosition::kNoOrder;
}

// A take is closed and a new one begun when it holds this much of the journal: a take longer than the ring would be lost whole, and the history before it with it.
bool take_is_long(const model::Editor& editor) noexcept {
  const model::UndoJournal& j = editor.journal();
  return j.open_bytes() > j.capacity_bytes() / 2;
}

// A write was written into the open take (the journal grew): the info line counts the take as an undo level. A write that changed nothing, and one made while no take is open (inside a macro),
// do not.
void note_take_write(ViewState& v, std::size_t journal_bytes_before, const model::Editor& editor) noexcept {
  if (v.edit.take_open && editor.journal().open_bytes() > journal_bytes_before) {
    v.edit.take_wrote = true;
  }
}

// The row of `track` the play line is on at this moment (REC QUANTIZE, the loop, the track's own length, the count-in rule): the row a recorded note and a recorded FX alike go to (OS-FXREC 3.3:
// one shared helper with rec_note).
int rec_row_now(const ActionContext& ctx, const PatternSource& src, int track) noexcept {
  namespace ds = model::device_setting;
  const ViewState& v = ctx.view;
  const rec::Quantize q = device_choice(ctx.model, ds::RecQuantize) == 0u ? rec::Quantize::Nearest : rec::Quantize::Playing;
  const bool loops = pattern_loops(v);
  const int head = v.play.step;
  // OS-COUNTIN: in the last half step of the count-in (pattern_recording lets it through under Nearest) the play line waits on the start point, so the hit IS the start step's: no round-up from it.
  if (v.play.count_in != 0u) {
    return src.track_length(track) > 0 ? head % src.track_length(track) : 0;
  }
  return rec::quantize_row(head, v.play.ustep, src.track_length(track), src.length(), q, loops);
}

// OS-ENCLEARN: STEP, the Pattern screen in Edit with the cursor free (OS-RECMODE2: REC), over a document.
bool rec_step_mode(const Model& model, const ViewState& v) noexcept {
  return model.project != nullptr && v.screen == ScreenId::Pattern && v.mode[screen_index(ScreenId::Pattern)] == static_cast<std::uint8_t>(PatternMode::Edit) && v.edit.rec_free;
}
}  // namespace

// Rec is on: the Pattern screen in Edit, the sequencer playing (and past its count-in, OS-METRO), a document to write into.
bool rec_active(const ActionContext& ctx) noexcept { return ctx.edit != nullptr && pattern_recording(ctx.model, ctx.view); }
// Edit is armed and the transport is counting in (OS-METRO): the notes of the pads and the keyboard sound and are not recorded, nor entered in the cursor's step.
bool rec_counting_in(const ActionContext& ctx) noexcept { return ctx.edit != nullptr && pattern_counting_in(ctx.model, ctx.view); }

// One note at the playhead (rec_place.h has the placement). `note` 0..127, `velocity` 1..127 for a note of the MIDI keyboard, 0 for one that has none (the pads): REC VELOCITY (External) writes it to the
// step's volume column, REC MICRO the note's offset from its step to FX 2 (OS-RECMICRO).
bool rec_note(ActionContext& ctx, unsigned note, unsigned velocity) noexcept {
  ViewState& v = ctx.view;
  if (!rec_active(ctx) || note > 127u) {
    return false;
  }
  if (v.play.pattern != v.edit.pattern) {  // the playhead is in another pattern (free recording, or a song that left the view): the step it is at is not this pattern's
    return pe::say(ctx, core::Severity::Info, "THE PLAYHEAD IS IN ANOTHER PATTERN");
  }
  namespace ds = model::device_setting;
  const PatternSource src(ctx.model, v.edit.pattern);
  const int tracks = src.tracks() < kRecTracks ? src.tracks() : kRecTracks;
  const int cursor = v.pattern.track();
  const int head = v.play.step;
  const bool spill = device_choice(ctx.model, ds::RecPoly) == static_cast<unsigned>(rec::Poly::Spill);
  if (cursor >= tracks) {
    return false;
  }
  // Off: the cursor's track. Spill: the first track from it whose recorded note is not still held.
  const int track = spill ? rec::next_free_track(static_cast<unsigned>(cursor), static_cast<unsigned>(tracks), v.edit.rec_open) : cursor;
  if (track < 0) {
    return pe::say(ctx, core::Severity::Info, "NO FREE TRACK TO SPILL TO");
  }
  const rec::Quantize q = device_choice(ctx.model, ds::RecQuantize) == 0u ? rec::Quantize::Nearest : rec::Quantize::Playing;
  const bool loops = pattern_loops(v);
  // OS-COUNTIN: in the last half step of the count-in (pattern_recording lets it through under Nearest) the play line waits on the start point, so the hit IS the start step's: no round-up from it.
  const bool in_count = v.play.count_in != 0u;
  const int row = rec_row_now(ctx, src, track);
  // REC MICRO: the offset from the step the note is written to (an early note, rounded up, has none: rec_place.h); -1 leaves FX 2 alone. REC VELOCITY: the keyboard's velocity, or 0 for none.
  const int micro = device_choice(ctx.model, ds::RecMicro) != 0u
                        ? static_cast<int>(rec::micro_value(rec::micro_offset(v.play.ustep, in_count || rec::goes_on(head, v.play.ustep, src.length(), q, loops))))
                        : -1;
  const unsigned vol = device_choice(ctx.model, ds::RecVelocity) != 0u && velocity <= 127u ? velocity : 0u;
  model::Step s = src.step(track, row);
  rec::write_note(s, note, static_cast<unsigned>(pe::clampi(v.instrument, 1, static_cast<int>(model::kMaxInstruments))), micro, vol);
  bool exists = src.exists();
  if (!pe::create_if_missing(ctx, exists, static_cast<unsigned>(src.length()))) {
    return false;
  }
  const std::size_t journal_before = ctx.edit->journal().open_bytes();
  if (ctx.edit->set_steps(v.edit.pattern, static_cast<std::uint32_t>(track), static_cast<std::uint32_t>(row), &s, 1) != model::Status::Ok) {
    return false;
  }
  note_take_write(v, journal_before, *ctx.edit);
  v.edit.rec_open = static_cast<std::uint16_t>(v.edit.rec_open | (1u << track));
  v.edit.rec_note[track] = static_cast<std::uint8_t>(note);
  v.edit.rec_row[track] = static_cast<std::uint8_t>(row);
  v.edit.rec_head[track] = static_cast<std::uint8_t>(head);
  return true;
}

// A pad of the Note column in Rec: the pad board's note, at the pad's own level (the instrument's velocity; not used with the options off), and the sound (the live note,
// held until the pad's release, App::run_once).
bool rec_pad(ActionContext& ctx, unsigned pad) noexcept {
  const unsigned note = pad_note(ctx.model.project, ctx.view.instrument, ctx.view.edit.pads, pad);  // a beat slice instrument: 24 + pad (OS-SLICEFIX)
  ctx.view.edit.last_note = static_cast<std::uint8_t>(note);  // OS-PADS-B
  ctx.view.edit.pad_held[pad] = static_cast<std::uint8_t>(note + 1u);  // the release ends this note, whatever the instrument is by then (app.cpp)
  const bool done = rec_note(ctx, note, 0u);  // a pad has no velocity of its own to record
  if (ctx.preview != nullptr) {
    ctx.preview->pad_down(pad, static_cast<unsigned>(ctx.view.pattern.track()), static_cast<unsigned>(ctx.view.instrument), note);
  }
  return done;
}

// The release of a recorded note (arg = the note 0..127; a pad's or the keyboard's): the track that holds it is closed and an OFF written at the playhead (rec_place.h, RELEASE).
// Whether or not Rec is still on, the hold ends; only a playing pattern in Edit gets the OFF.
bool rec_note_off(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (arg < 0 || arg > 127 || v.edit.rec_open == 0u) {
    return false;
  }
  int track = -1;
  for (int t = 0; t < kRecTracks; ++t) {
    if (((v.edit.rec_open >> t) & 1u) != 0u && v.edit.rec_note[t] == static_cast<std::uint8_t>(arg)) {
      track = t;
      break;
    }
  }
  if (track < 0) {
    return false;
  }
  v.edit.rec_open = static_cast<std::uint16_t>(v.edit.rec_open & ~(1u << track));
  if (!rec_active(ctx) || v.play.pattern != v.edit.pattern) {
    return false;  // stopped or left: the hold is over, nothing is written
  }
  const PatternSource src(ctx.model, v.edit.pattern);
  const int length = src.track_length(track);
  if (length <= 0 || !src.exists()) {
    return false;
  }
  const int placed = v.edit.rec_row[track];
  // The rows of a track are the pattern's steps modulo its length (a shorter track loops inside the pattern).
  const int at = rec::off_row(v.play.step % length, v.edit.rec_head[track] % length, placed, length, src.length(), pattern_loops(v));
  if (at < 0 || at == placed) {
    return false;  // the pattern ends the note (a song's last step), or a track of one step: the OFF would erase the note
  }
  model::Step s = src.step(track, at);
  if (s.note != model::kNoteEmpty) {
    return false;  // a note or an OFF is already there: it ends the held note itself (the track is monophonic), and a take is not erased
  }
  rec::write_off(s);
  const std::size_t journal_before = ctx.edit->journal().open_bytes();
  const bool done = ctx.edit->set_steps(v.edit.pattern, static_cast<std::uint32_t>(track), static_cast<std::uint32_t>(at), &s, 1) == model::Status::Ok;
  note_take_write(v, journal_before, *ctx.edit);
  return done;
}

// ---- the take ------------------------------------------------------------------------------------------------------------------------------------------------

// OS-FXREC S2 (docs/specs/OS-FXREC.md 3.2, 3.3, 3.5): a mapped parameter move of a LIVE take is written as the step FX at the play line of the cursor's track. Called from apply_cc after
// write_param returned Changed, with the parameter the message resolved to. All of 3.2 holds or nothing records: recording is active, the playhead is in the edited pattern, the parameter is of
// the selected instrument (Q3 a) and that instrument is a Sample one (A-5), and rec_fx_map gives an FX (the filter type read from the instrument's stored FILTER TYPE, the value read back from the
// parameter). The column is rec_fx_place's; the write goes back with Editor::set_steps inside the open take, and is skipped when the cell is unchanged (F11).
//
// OS-ENCLEARN (`enc`): the same function takes the move of Instr + FX1 and the encoder. LIVE is as above. STEP (Edit free, REC: the take that App::run_once opened for the gesture, enc_take) writes the
// same FX at the CURSOR's step of the cursor's track, whether or not the pattern plays. A parameter that records nothing (not an instrument one, not of the selected Sample instrument, no FX map) says
// "REC FX: <PARAMETER> HAS NO FX" once per gesture; WATCH and the other screens say nothing and record nothing.
bool rec_fx_write(ActionContext& ctx, core::ParamId id, const char* name, bool enc) noexcept {
  ViewState& v = ctx.view;
  const bool live = rec_active(ctx);
  const bool step = enc && !live && ctx.edit != nullptr && v.edit.enc_take && rec_step_mode(ctx.model, v);
  if (!live && !step) {
    return false;  // not recording
  }
  if (live && v.play.pattern != v.edit.pattern) {
    return false;  // the playhead is in another pattern
  }
  const auto no_fx = [&]() noexcept {
    if (enc && !v.edit.enc_said) {
      v.edit.enc_said = true;
      core::FixedString<core::kNotifyTextChars> text;
      text.append("REC FX: ");
      text.append(name != nullptr ? name : "");
      text.append(" HAS NO FX");
      (void)v.notify.post(core::Severity::Info, text.c_str(), "rec_fx", ViewState::kMessageMs);
    }
    return false;
  };
  if (core::param_kind(id) != core::ParamKind::Instrument) {
    return no_fx();  // a Mixer, Perform or device move records nothing (A-7)
  }
  const unsigned slot = core::param_index(id);
  if (static_cast<int>(slot) + 1 != v.instrument) {
    return no_fx();  // Q3 a: only a binding of the selected instrument
  }
  const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(slot) : nullptr;
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return no_fx();  // A-5: only a Sample instrument records (the mapped fields are sampler fields)
  }
  float value = 0.0f;
  if (!read_param(ctx.model, id, value)) {
    return false;  // the value as it is stored, so as it is heard
  }
  float filter_type = 0.0f;
  (void)read_param(ctx.model, core::param_id(core::ParamKind::Instrument, slot, model::instrument_param::FilterType), filter_type);  // the stored FILTER TYPE (A-1)
  model::StepFx fx{};
  if (!rec::rec_fx_map(core::param_field(id), static_cast<unsigned>(filter_type), value, fx)) {
    return no_fx();  // Tune and every other parameter write nothing (Q2 a)
  }
  const PatternSource src(ctx.model, v.edit.pattern);
  const int tracks = src.tracks() < kRecTracks ? src.tracks() : kRecTracks;
  const int track = v.pattern.track();  // REC POLY Spill does not spill FX (A-3): the cursor's track
  if (track >= tracks) {
    return false;
  }
  const int row = step ? v.pattern.row() : rec_row_now(ctx, src, track);  // STEP: the cursor's step
  if (row < 0 || row >= src.track_length(track)) {
    return false;
  }
  model::Step s = src.step(track, row);
  const model::Step before = s;
  if (rec::rec_fx_place(s, fx.type, fx.value) < 0) {
    if (!v.edit.rec_fx_full) {  // once per take
      v.edit.rec_fx_full = true;
      (void)v.notify.post(core::Severity::Info, "REC FX: NO FREE FX COLUMN", "rec_fx", ViewState::kMessageMs);
    }
    return false;
  }
  if (std::memcmp(&s, &before, sizeof s) == 0) {
    return false;  // the cell is unchanged: nothing to write, nothing recorded
  }
  bool exists = src.exists();
  if (!pe::create_if_missing(ctx, exists, static_cast<unsigned>(src.length()))) {
    return false;
  }
  const std::size_t journal_before = ctx.edit->journal().open_bytes();
  if (ctx.edit->set_steps(v.edit.pattern, static_cast<std::uint32_t>(track), static_cast<std::uint32_t>(row), &s, 1) != model::Status::Ok) {
    return false;
  }
  note_take_write(v, journal_before, *ctx.edit);
  if (!v.edit.rec_fx_said) {  // the take's first recorded FX says which parameter (3.5)
    v.edit.rec_fx_said = true;
    core::FixedString<core::kNotifyTextChars> text;
    text.append("REC FX: ");
    text.append(name != nullptr ? name : "");
    (void)v.notify.post(core::Severity::Info, text.c_str(), "rec_fx", ViewState::kMessageMs);
  }
  return true;
}

bool rec_keeps_take(const ActionDef& action, const ViewState& view) noexcept {
  if ((action.flags & core::kActionRecords) == 0u) {
    return false;
  }
  if (action.id == core::action_id("pads.play")) {  // the PIANO / ISO pads record; the STEP pads (and the rest) edit
    const PadMode m = view.padmode.fixed();
    return m == PadMode::Piano || m == PadMode::Iso;
  }
  return true;
}

void rec_take_end(model::Editor* editor, PatternEdit& edit) noexcept {
  if (edit.take_open) {
    edit.take_open = false;
    edit.take_wrote = false;
    edit.rec_fx_said = false;  // OS-FXREC: the take's strip flags go with it
    edit.rec_fx_full = false;
    if (editor != nullptr) {
      editor->end_group();  // commits the unit if it has records; an empty take leaves no entry
    }
  }
}

void rec_take_sync(model::Editor* editor, const Model& model, ViewState& view) noexcept {
  // OS-ENCLEARN: STEP's gesture of Instr + FX1 and the encoder holds a take-like undo group too (enc_take, cleared when a key is let go); it ends with STEP.
  const bool want = editor != nullptr && (pattern_recording(model, view) || (view.edit.enc_take && rec_step_mode(model, view)));
  if (want && view.edit.take_open && take_is_long(*editor)) {
    rec_take_end(editor, view.edit);  // closed at half the journal, and a new take begun below: both can be undone, and the history before them is not all lost
    if (!view.edit.take_noticed) {
      view.edit.take_noticed = true;
      (void)view.notify.post(core::Severity::Info, "UNDO: LONG TAKE IS SPLIT", "take", ViewState::kMessageMs);
    }
  }
  if (want == view.edit.take_open) {
    return;
  }
  if (want) {
    editor->begin_group(core::ActionId::None, 0, model::UndoArea::Pattern);  // the unit of the whole take: written to the journal with its first record
    view.edit.take_open = true;
  } else {
    rec_take_end(editor, view.edit);
    view.edit.take_noticed = false;  // the recording stopped: the next one tells again
  }
}

}  // namespace o3os::app
