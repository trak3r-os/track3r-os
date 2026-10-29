// SPDX-License-Identifier: GPL-3.0-or-later
// The project-level state's runtime side (app/project_state.h, OS-004d), and the App's part of it.
#include <o3os/app/project_state.h>

#include "action_handlers.h"

#include <o3os/app/app.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/play_mode.h>
#include <o3os/app/wave_math.h>
#include <o3os/app/track_state.h>
#include <o3os/app/save_service.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/paula.h>
#include <o3os/model/step_edit.h>
#include <o3os/seq/perform.h>

#include <cstring>
#include <initializer_list>

namespace o3os::app {

namespace gp = audio::graph_param;

static_assert(model::kMixStrips == kMixStrips && model::kMixRouteMask == audio::route::kAll && model::kMixRouteUsb == audio::route::kUsb &&
                  model::kMixDefaultTrackRoute == (audio::route::kMain | audio::route::kUsb) && model::kMixDefaultInputRoute == (audio::route::kMain | audio::route::kCue) &&
                  model::kMixDefaultCue == 100,
              "model/project_state.h mirrors app/mix_board.h");
static_assert(model::kStemSlots == usb::kMaxChannels && model::kUsbModeMax == static_cast<std::uint8_t>(usb::Mode::Audio) &&
                  model::kUsbProfileMax == static_cast<std::uint8_t>(usb::Profile::P6) && model::kUsbProfileP4S == static_cast<std::uint8_t>(usb::Profile::P4S) &&
                  model::kUsbProfileMaxChunk == static_cast<std::uint8_t>(usb::Profile::P11) &&
                  model::kUsbProfileChannels[static_cast<std::uint8_t>(usb::Profile::P4)] == 4 && model::kUsbProfileChannels[static_cast<std::uint8_t>(usb::Profile::P6)] == 6 &&
                  model::kUsbProfileChannels[static_cast<std::uint8_t>(usb::Profile::P2)] == 2 && model::kUsbProfileChannels[static_cast<std::uint8_t>(usb::Profile::P8)] == 8 &&
                  model::kUsbProfileChannels[static_cast<std::uint8_t>(usb::Profile::P10)] == 10 && model::kUsbProfileChannels[static_cast<std::uint8_t>(usb::Profile::P11)] == 11,
              "model/project_state.h mirrors usb/usb_types.h");
static_assert(model::kMaxAppParams == O3M_PARAMS_MAX && model::kAppNameMax == model::kModuleNameMax, "model/project_state.h mirrors the module ABI");

// OS-PERSIST2: the session state's ranges are the runtime's.
static_assert(model::kNoteRootMax == 11 && model::kNoteScaleMax == kNoteScales - 1 && model::kNoteOctaveMax == kEntryOctaves - 1 && model::kNoteDefaultOctave == NoteEntry{}.octave,
              "model/project_state.h mirrors app/note_entry.h (the default JUMP, PatternEdit::edit_step, is held equal by tests/persist2_test.cpp)");
static_assert(model::kMuteModeMax == kMuteModes - 1 && model::kMaxTracks == kMixTracks, "model/project_state.h mirrors app/track_state.h");
static_assert(model::kSceneCount == seq::kPerfScenes, "model/project_state.h mirrors seq/perform.h");
static_assert(model::kNoteJumpMax == model::edit::kMaxEditStep, "a saved JUMP is the JUMP the editor plays (model/step_edit.h)");

namespace {

// The rows of the MASTER FX page have the ranges and defaults of the project's SNFX chunk, row by row.
constexpr bool send_fx_rows_match() noexcept {
  for (unsigned i = 0; i < kSendFxRows; ++i) {
    if (kSendFxTable[i].min != model::kSendFxMin[i] || kSendFxTable[i].max != model::kSendFxMax[i] || kSendFxTable[i].def != model::kSendFxDefault[i]) {
      return false;
    }
  }
  return true;
}
static_assert(model::kSendFxCount == kSendFxRows && send_fx_rows_match(), "model/project_state.h mirrors app/send_fx_rows.h");

}  // namespace

// ---- the mixer ------------------------------------------------------------------------------------------------------------------------

model::MixState mix_state_of(const MixBoard& board) noexcept {
  model::MixState s;
  std::memcpy(s.route, board.route, sizeof s.route);
  s.ret_route[0] = board.ret_route[0];
  s.ret_route[1] = board.ret_route[1];
  s.out_mode = board.out_mode;
  s.cue_level = board.cue_level;
  s.in_level = board.in_level;
  return s;
}

namespace {

void tell(core::ParamRing* ring, core::ParamId id, std::uint8_t value) noexcept {
  if (ring != nullptr) {
    (void)ring->push(core::ParamChange{id, static_cast<float>(value), 0});  // a full ring drops it: the ring is 256 deep and this sends at most 22
  }
}

}  // namespace

unsigned mix_apply(MixBoard& board, const model::MixState& s, core::ParamRing* ring) noexcept {
  unsigned changed = 0;
  for (unsigned i = 0; i < kMixStrips; ++i) {
    if (board.route[i] != s.route[i]) {
      board.route[i] = s.route[i];
      tell(ring, audio::strip_param(i, gp::strip::Route), s.route[i]);
      ++changed;
    }
  }
  for (unsigned r = 0; r < 2; ++r) {
    if (board.ret_route[r] != s.ret_route[r]) {
      board.ret_route[r] = s.ret_route[r];
      tell(ring, audio::master_param(r == 0 ? gp::master::RetRouteA : gp::master::RetRouteB), s.ret_route[r]);
      ++changed;
    }
  }
  if (board.out_mode != s.out_mode) {
    board.out_mode = s.out_mode;
    tell(ring, audio::master_param(gp::master::OutMode), s.out_mode);
    ++changed;
  }
  if (board.cue_level != s.cue_level) {
    board.cue_level = s.cue_level;
    tell(ring, audio::master_param(gp::master::CueLevel), s.cue_level);
    ++changed;
  }
  if (board.in_level != s.in_level) {
    board.in_level = s.in_level;
    tell(ring, audio::strip_param(audio::kInputStrip, gp::strip::Volume), s.in_level);
    ++changed;
  }
  return changed;
}

// ---- the session state (OS-PERSIST2) --------------------------------------------------------------------------------------------------------

model::NoteState note_state_of(const PatternEdit& edit) noexcept {
  model::NoteState s;
  s.root = edit.entry.root;
  s.scale = edit.entry.scale;
  s.octave = edit.entry.octave;
  s.lock = edit.entry.lock;
  s.jump = edit.edit_step;
  (void)model::sanitise_note_state(s);  // what is stored is always a value the format holds
  return s;
}

bool note_apply(PatternEdit& edit, const model::NoteState& s) noexcept {
  const bool changed = edit.entry.root != s.root || edit.entry.scale != s.scale || edit.entry.octave != s.octave || edit.entry.lock != s.lock || edit.edit_step != s.jump;
  edit.entry.root = s.root;
  edit.entry.scale = s.scale;
  edit.entry.octave = s.octave;
  edit.entry.lock = s.lock;
  edit.edit_step = s.jump;
  edit.apply_entry();  // the NOTES board is laid out from the settings
  return changed;
}

model::MuteState mute_state_of(const MixBoard& board) noexcept {
  model::MuteState s;
  std::memcpy(s.mode, board.mute_mode, sizeof s.mode);
  (void)model::sanitise_mute_state(s);
  return s;
}

unsigned mute_apply(MixBoard& board, const model::MuteState& s) noexcept {
  unsigned changed = 0;
  for (unsigned t = 0; t < kMixTracks; ++t) {
    changed += board.mute_mode[t] != s.mode[t] ? 1u : 0u;
    board.mute_mode[t] = s.mode[t];
  }
  return changed;
}

model::SendFxState send_fx_state_of(const MixBoard& board) noexcept {
  model::SendFxState s;
  std::memcpy(s.value, board.fx, sizeof s.value);
  (void)model::sanitise_send_fx_state(s);
  return s;
}

unsigned send_fx_apply(MixBoard& board, const model::SendFxState& s, core::ParamRing* ring) noexcept {
  unsigned changed = 0;
  for (unsigned i = 0; i < kSendFxRows; ++i) {
    if (board.fx[i] != s.value[i]) {
      board.fx[i] = s.value[i];
      ++changed;
      if (ring != nullptr) {  // (the ring is 256 deep and this sends at most 12)
        (void)ring->push(core::ParamChange{audio::master_param(kSendFxTable[i].field), static_cast<float>(s.value[i]), 0});
      }
    }
  }
  return changed;
}

// ---- the tracks' mute and solo -------------------------------------------------------------------------------------------------------------

unsigned tracks_apply(const model::Project& project, core::ParamRing* ring) noexcept {
  unsigned sent = 0;
  for (unsigned t = 0; ring != nullptr && t < audio::kTracks; ++t) {
    const model::TrackSlot* slot = project.track(t);
    const std::uint8_t flags = slot != nullptr ? slot->flags : std::uint8_t{0};
    // (the ring is 256 deep and this sends 32)
    sent += ring->push(core::ParamChange{audio::strip_param(t, gp::strip::Mute), (flags & model::kTrackMute) != 0u ? 1.0f : 0.0f, 1}) ? 1u : 0u;
    sent += ring->push(core::ParamChange{audio::strip_param(t, gp::strip::Solo), (flags & model::kTrackSolo) != 0u ? 1.0f : 0.0f, 1}) ? 1u : 0u;
  }
  if (ring != nullptr) {  // OS-SENDFX: and the project's SENDS setting (pre- or post-fader, PAR-024), which the graph takes (graph_params.h graph_param::project::SendsMode)
    const model::Settings& st = project.settings();
    sent += ring->push(core::ParamChange{core::param_id(core::ParamKind::Project, 0, gp::project::SendsMode), st.get(model::project_setting::SendsMode), 1}) ? 1u : 0u;
    // OS-LIMIT: and the master limiter's two project fields, the LIMITER switch (the master section's flag) and the LIMITER MODE (the engine's base limiter is on either way: these put its controls and preset in force)
    sent += ring->push(core::ParamChange{core::param_id(core::ParamKind::Project, 0, gp::project::LimiterMode), st.get(model::project_setting::LimiterMode), 1}) ? 1u : 0u;
    sent += ring->push(core::ParamChange{audio::master_param(gp::master::Limiter), (project.master().flags & model::kMasterLimiter) != 0u ? 1.0f : 0.0f, 1}) ? 1u : 0u;
    // and the sounds of the last project are forgotten: the effects' memory (cleared a slice a block, so an old tail does not ring into the new project) and the instrument sends of the last notes
    for (const unsigned field : {gp::master::DelayClear, gp::master::ReverbClear, gp::master::VoiceSendsReset}) {
      sent += ring->push(core::ParamChange{audio::master_param(field), 1.0f, 0}) ? 1u : 0u;
    }
  }
  return sent;
}

// ---- USB --------------------------------------------------------------------------------------------------------------------------------

model::UsbState usb_state_of(const audio::StemMap& map, usb::Mode mode) noexcept {
  model::UsbState s;
  std::memset(&s, 0, sizeof s);
  s.mode = static_cast<std::uint8_t>(mode);
  s.profile = static_cast<std::uint8_t>(map.profile());
  s.slots = static_cast<std::uint8_t>(map.channels());
  for (std::uint32_t i = 0; i < s.slots && i < model::kStemSlots; ++i) {
    s.mask[i] = map.slot(i);
  }
  return s;
}

bool stem_map_apply(audio::StemMap& map, const model::UsbState& s) noexcept {
  if (s.profile != static_cast<std::uint8_t>(map.profile()) || s.slots != map.channels()) {
    return false;
  }
  for (std::uint32_t i = 0; i < s.slots; ++i) {
    (void)map.set_slot(i, s.mask[i]);
  }
  return true;
}

// ---- SD apps ----------------------------------------------------------------------------------------------------------------------------

unsigned app_params_apply(ModuleUi& ui, const model::Project& project) noexcept {
  const o3m_module* m = ui.app_module();
  const char* name = ui.app_name();
  if (m == nullptr) {
    return 0;
  }
  const model::AppParams* saved = project.find_app_params(name, std::strlen(name));
  if (saved == nullptr || saved->abi_major != m->abi_major) {
    return 0;  // nothing remembered for this module, or for the ABI it was built with
  }
  unsigned set = 0;
  for (unsigned i = 0; i < saved->count && i < m->param_count; ++i) {
    set += ui.app_set_param(i, saved->value[i]) ? 1u : 0u;
  }
  return set;
}

bool app_params_sync(const ModuleUi& ui, model::Editor& editor) noexcept {
  const o3m_module* m = ui.app_module();
  const char* name = ui.app_name();
  if (m == nullptr || m->param_count == 0 || m->param_count > model::kMaxAppParams || name[0] == '\0') {
    return false;
  }
  const unsigned n = m->param_count;
  float values[model::kMaxAppParams];
  for (unsigned i = 0; i < n; ++i) {
    values[i] = ui.app_param(i);
  }
  const std::size_t len = std::strlen(name);
  const model::AppParams* saved = editor.project().find_app_params(name, len);
  bool differs = false;
  if (saved != nullptr && saved->abi_major == m->abi_major) {
    differs = saved->count != n || std::memcmp(saved->value, values, n * sizeof(float)) != 0;
  } else {
    for (unsigned i = 0; i < n && !differs; ++i) {
      differs = values[i] != m->params[i].def;  // no entry: only a change from the module's defaults is worth keeping
    }
  }
  return differs && editor.set_app_params(name, len, m->abi_major, m->abi_minor, values, n) == model::Status::Ok;
}

// ---- the instrument's playback engine (OS-004d) ----------------------------------------------------------------------------------------

// The playback engine of a sample instrument (the stored value: 0 automatic, 1 Modern, 2 Amiga, 3 12-bit, 4 8-bit). One undo unit; the sample bank rebinds the slot (the voices take the new
// engine at the next quiet handover). Undoing it changes the instrument too: edit_undo rebinds the slots whose engine it moved.
//
// AMIGA needs a sample Paula can play at the unity note: that note is period 428 for a sample at 8,287 Hz (amiga_voices.h), so the period at the sample's rate r is 3,546,895 / r and
// must not fall below Paula's floor (113: r up to 31,388 Hz). A rate of 0 means 44,100 Hz (not recorded). Above it the choice is refused until the sample is converted (a later slice).
namespace {

bool amiga_can_play(const model::Instrument& in) noexcept {
  const std::uint64_t rate = in.sample_rate != 0u ? in.sample_rate : 44100u;
  return rate * audio::paula::kMinPeriod <= audio::paula::kPalClockHz;
}

}  // namespace

bool play_engine_set(ActionContext& ctx, unsigned slot, unsigned target, int dir) noexcept {
  if (ctx.edit == nullptr || target > model::kInstEngineMax) {
    return false;
  }
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return ctx.view.notify.post(core::Severity::Info, "NO SAMPLE INSTRUMENT HERE", "engine", ViewState::kMessageMs);
  }
  bool skipped = false;
  if (target == model::kInstEngineAmiga && !amiga_can_play(*in)) {
    (void)ctx.view.notify.post(core::Severity::Info, "CONVERT TO AMIGA FIRST", "engine", ViewState::kMessageMs);
    if (dir == 0) {
      return false;
    }
    target = dir > 0 ? model::kInstEngineAmiga + 1u : model::kInstEngineAmiga - 1u;  // a step goes on past it
    skipped = true;
  }
  if (target == in->engine) {
    return false;
  }
  model::Instrument edited = *in;
  edited.engine = static_cast<std::uint8_t>(target);
  if (ctx.edit->set_instrument(slot, edited) != model::Status::Ok) {
    return false;
  }
  if (ctx.samples != nullptr) {
    ctx.samples->request_rebind(slot);
  }
  static constexpr const char* kNames[model::kInstEngineMax + 1] = {"ENGINE AUTO", "ENGINE MODERN", "ENGINE AMIGA", "ENGINE 12-BIT", "ENGINE 8-BIT"};
  if (!skipped) {  // (a skipped step keeps the refusal on the strip: it says why the value jumped)
    (void)ctx.view.notify.post(core::Severity::Info, kNames[target], "engine", ViewState::kMessageMs);
  }
  return true;
}

// instrument.engine: the instrument under the cursor, `arg` 0..4 for that engine, -1 for the next one (the table wraps; an Amiga that cannot be played is stepped over).
bool instrument_engine(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.edit == nullptr || ctx.view.instrument < 1 || arg < -1 || arg > model::kInstEngineMax) {
    return false;
  }
  const unsigned slot = static_cast<unsigned>(ctx.view.instrument - 1);
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (arg < 0) {
    const unsigned now = in != nullptr ? in->engine : 0u;
    return play_engine_set(ctx, slot, (now + 1u) % (model::kInstEngineMax + 1u), +1);
  }
  return play_engine_set(ctx, slot, static_cast<unsigned>(arg), 0);
}

// ---- the instrument's play mode (OS-INSTUI S3) ------------------------------------------------------------------------------------------------

// The play mode of a sample instrument set to `target` (play_mode.h, the OG instrument file's 0..7), the one door for the PLAY MODE row of the source page and the Wave screen's mode list. One undo unit; the slot is
// rebound. New Community OS behaviour that keeps the instrument playable when the mode changes: the loop repair gives the
// whole play window; the grain clamp and the refusal under 32 frames; a playing note keeps the old mode until the rebind. What the official firmware does on a mode change is an
// open research question (Q). The rules:
//  - WAVETABLE needs a window the sample can hold: the size is fitted to the length (2048 when unset, held to the length, rounded down to a power of two) and the position to the windows
//    there are; a sample under 32 frames has none, so the change is refused and says why. The OG file's sample type follows (1 for a wavetable, 0 otherwise).
//  - GRANULAR keeps the grain inside [44, min(length, 44100)] so that it is a grain.
//  - a loop mode with loop points that break the loop's conditions (start <= loop start < loop end <= end) gets the whole play window as its loop: the engine would play such a loop as a one shot.
//  - SLICE / BEAT SLICE with no slices is allowed, and says where to make them (AUTO / EVEN on Wave).
// True when the mode changed.
bool play_mode_set(ActionContext& ctx, unsigned slot, unsigned target) noexcept {
  if (ctx.edit == nullptr || target >= kPlayModeCount) {
    return false;
  }
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return ctx.view.notify.post(core::Severity::Info, "NO SAMPLE INSTRUMENT HERE", "playmode", ViewState::kMessageMs);
  }
  if (in->sampler.play_mode == target) {
    return false;
  }
  const std::uint32_t frames = ctx.model.samples != nullptr ? ctx.model.samples->pcm(slot).frames : 0u;
  model::Instrument edited = *in;
  model::SamplerParams& s = edited.sampler;
  s.play_mode = static_cast<std::uint8_t>(target);
  s.sample_type = target == kPlayModeWavetable ? 1u : 0u;
  if (target == kPlayModeWavetable) {
    const unsigned size = wt_fit_size(s.wavetable_window_size, frames);
    if (size == 0u) {
      return ctx.view.notify.post(core::Severity::Warn, "WAVETABLE NEEDS A SAMPLE OF 32 FRAMES", "playmode", ViewState::kMessageMs);
    }
    s.wavetable_window_size = static_cast<std::uint16_t>(size);
    s.wavetable_window = wt_clamp_pos(frames, size, s.wavetable_window);
  } else if (target == kPlayModeGranular) {
    const unsigned top = frames != 0u && frames < 44100u ? frames : 44100u;
    const unsigned low = top < 44u ? top : 44u;
    s.grain_length = static_cast<std::uint16_t>(s.grain_length < low ? (441u < top ? 441u : top) : (s.grain_length > top ? top : s.grain_length));
  } else if (target >= 1u && target <= 3u) {
    Points p;
    p.start = s.start;
    p.loop_start = s.loop_start;
    p.loop_end = s.loop_end;
    p.end = s.end;
    if (!points_valid(p) && p.start < p.end) {
      s.loop_start = p.start;
      s.loop_end = p.end;
    }
  }
  if (ctx.edit->set_instrument(slot, edited) != model::Status::Ok) {
    return false;
  }
  if (ctx.samples != nullptr) {
    ctx.samples->request_rebind(slot);
  }
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("PLAY MODE: ");
  text.append(kPlayModeNames[target]);
  if (play_mode_is_slice(target) && s.slice_count == 0u) {
    text.assign(kPlayModeNames[target]);
    text.append(": NO SLICES, AUTO OR EVEN ON WAVE");
  }
  (void)ctx.view.notify.post(core::Severity::Info, text.c_str(), "playmode", ViewState::kMessageMs);
  return true;
}

// instrument.play_mode: the instrument in view, `arg` 0..7 for that mode, -1 / -2 for the previous / next one (stopping at the ends).
bool instrument_play_mode(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.edit == nullptr || ctx.view.instrument < 1 || arg < -2 || arg >= static_cast<core::ActionArg>(kPlayModeCount)) {
    return false;
  }
  const unsigned slot = static_cast<unsigned>(ctx.view.instrument - 1);
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (arg >= 0) {
    return play_mode_set(ctx, slot, static_cast<unsigned>(arg));
  }
  const int now = in != nullptr ? in->sampler.play_mode : 0;
  const int to = now + (arg == -1 ? -1 : 1);
  return to >= 0 && to < static_cast<int>(kPlayModeCount) && play_mode_set(ctx, slot, static_cast<unsigned>(to));
}

// ---- the App's part ------------------------------------------------------------------------------------------------------------------------

bool App::project_state_writable() const noexcept {
  return model_.project != nullptr && (saves_ == nullptr || !saves_->busy());  // a project that is being written must not change under the writer
}

void App::sync_project_state() noexcept {
  model::Editor* editor = session_.state_editor();
  if (editor == nullptr || !project_state_writable()) {
    return;
  }
  const model::Project& project = editor->project();
  if (mix_ != nullptr) {
    const model::MixState now = mix_state_of(*mix_);
    if (now != project.mix()) {
      (void)editor->set_mix_state(now);
    }
    const model::MuteState mute_now = mute_state_of(*mix_);  // OS-PERSIST2: the session state, the same way
    if (mute_now != project.mute()) {
      (void)editor->set_mute_state(mute_now);
    }
    const model::SendFxState fx_now = send_fx_state_of(*mix_);
    if (fx_now != project.send_fx()) {
      (void)editor->set_send_fx_state(fx_now);
    }
  }
  const model::NoteState note_now = note_state_of(state_.edit);
  if (note_now != project.note()) {
    (void)editor->set_note_state(note_now);
  }
  if (perform_ != nullptr) {
    const model::PerfState now = perform_->bank.state();
    if (now != project.perf()) {
      (void)editor->set_perf_state(now);
    }
    const model::ScenesState scenes_now = perform_->bank.scenes_state();
    if (scenes_now != project.scenes()) {
      (void)editor->set_scenes_state(scenes_now);
    }
  }
  if (model_.modules != nullptr && model_.modules->app_running()) {
    (void)app_params_sync(*model_.modules, *editor);
  }
}

// The step between "the runtime took the loaded project" and "the project is the baseline of unsaved changes" (OS-FU5). Anything a load makes the runtime hold differently from what the
// file said (a Perform value clamped to its FX's range) is written back here, as project state (never an undo unit), or the pass would copy it in later and the project would show
// unsaved right after the load. It does not look at project_state_writable(): the document is the new one, and a save still running was started on the old one (its writer fails
// it by the generation). The active tracks are raised to the 12 a new project has (maintainer decision: always 12): a file that says fewer (an OG 8-track project, a MED song of 4) is
// played and shown on 12; the readers keep returning the file's value (their round trips and the OG export's drop report do not move) and a later save writes 12.
void App::normalise_loaded_state() noexcept {
  model::Editor* editor = session_.state_editor();
  if (editor == nullptr || model_.project == nullptr) {
    return;
  }
  if (perform_ != nullptr) {
    const model::PerfState now = perform_->bank.state();
    if (now != model_.project->perf()) {
      (void)editor->set_perf_state(now);
    }
  }
  if (model_.project->active_tracks() < model::kDefaultActiveTracks) {
    (void)editor->set_active_tracks_state(model::kDefaultActiveTracks);
  }
}

void App::apply_project_state() noexcept {
  if (model_.project == nullptr) {
    return;
  }
  if (mix_ != nullptr) {
    (void)mix_apply(*mix_, model_.project->mix(), params_out_);
    (void)mute_apply(*mix_, model_.project->mute());  // OS-PERSIST2: a project that says nothing sets the defaults, as for the mixer
    (void)send_fx_apply(*mix_, model_.project->send_fx(), params_out_);
  }
  (void)tracks_apply(*model_.project, params_out_);  // OS-MUTE: the strips take the project's mute and solo (the sequencer does not skip a muted track any more)
  (void)note_apply(state_.edit, model_.project->note());
  if (perform_ != nullptr) {
    perform_->bank.load_slots(model_.project->perf());  // every FX ends; poll_perform() tells the engine
    perform_->bank.load_scenes(model_.project->scenes());  // (load_slots cleared the scenes)
  }
  dirty_ = true;
}

}  // namespace o3os::app
