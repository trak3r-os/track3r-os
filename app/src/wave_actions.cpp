// SPDX-License-Identifier: GPL-3.0-or-later
// Wave / Chop actions (OS-011a): moving the four points and the slices of the instrument in view, slicing it (ADD, DEL, EVEN, AUTO), and the pads that play and
// select slices. Every edit of the instrument is one undo unit (the actions are undoable: they edit through ActionContext::edit) and asks the sample bank for the
// handover, so the engine follows when playback is stopped (sample_bank.h). The view-only ones (zoom, ZC, the point, the EVEN count) change the ViewState.
#include "action_handlers.h"

#include "param_access.h"

#include <o3os/app/og_samples.h>
#include <o3os/app/play_mode.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/wave_screen.h>
#include <o3os/audio/voices.h>

#include <cstring>

namespace o3os::app {

namespace {

constexpr int kLaneColumns = 768;       // the lane's width: a click of the encoder is one column of the view (Shift: kCoarse)
constexpr int kCoarse = 16;
constexpr int kMaxDetents = 1 << 16;
constexpr std::uint8_t kBeatSlice = 5;  // play mode 5 of the OG instrument file (beat slice): its pads play the slices

bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return say_tag(ctx, severity, text, "wave");
}

bool chop_page(const ActionContext& ctx) noexcept { return ctx.view.page[screen_index(ScreenId::Wave)] == 1u; }

// The sample instrument in view and its audio; false (with a word to the user) when there is none to edit.
struct Target {
  unsigned slot = 0;
  const model::Instrument* in = nullptr;
  SampleBank::Pcm pcm;
};

bool target(ActionContext& ctx, Target& t) noexcept {
  const model::Project* project = ctx.edit != nullptr ? &ctx.edit->project() : ctx.model.project;
  t.slot = wave_slot(ctx.view);
  t.in = project != nullptr ? project->instrument(t.slot) : nullptr;
  t.pcm = ctx.model.samples != nullptr ? ctx.model.samples->pcm(t.slot) : SampleBank::Pcm{};
  if (t.in == nullptr || t.in->kind != model::InstrumentKind::Sample || t.pcm.frames == 0u) {
    (void)say(ctx, core::Severity::Info, "NO SAMPLE IN THIS INSTRUMENT");
    return false;
  }
  return true;
}

Points points_of(const model::Instrument& in) noexcept {
  Points p;
  p.start = in.sampler.start;
  p.loop_start = in.sampler.loop_start;
  p.loop_end = in.sampler.loop_end;
  p.end = in.sampler.end;
  return p;
}

void store_points(model::Instrument& in, const Points& p) noexcept {
  in.sampler.start = p.start;
  in.sampler.loop_start = p.loop_start;
  in.sampler.loop_end = p.loop_end;
  in.sampler.end = p.end;
}

// Writes the edited instrument back as one unit and has the engine follow. False when nothing changed (the editor records no unit for it).
bool commit(ActionContext& ctx, const Target& t, const model::Instrument& edited) noexcept {
  if (std::memcmp(&edited, t.in, sizeof edited) == 0 || ctx.edit == nullptr) {
    return false;
  }
  // OS-MOD, LIVE LOOP: a loop moved inside the same window reaches the sounding voices through the engine's ring (the render moves the descriptor's loop, voices.h); the handover below is the rest
  // (PCM, window, slices, mode) and the safety net when the ring drops one. Made BEFORE the edit: `t.in` points into the project, which the edit overwrites.
  // None while the slot waits for a handover of more than its loop (stale(): a window, mode or PCM edit, an undo): the descriptor's window is not the project's, and a move
  // counted from the project's window start would land in the wrong place (OS-MOD review).
  LoopMove moves[2];
  const unsigned n = ctx.samples != nullptr && !ctx.samples->stale(t.slot) ? live_loop_moves(*t.in, edited, t.pcm.frames, moves) : 0u;
  if (ctx.edit->set_instrument(t.slot, edited) != model::Status::Ok) {
    return say(ctx, core::Severity::Warn, "THE INSTRUMENT REFUSED THE EDIT");
  }
  if (ctx.samples != nullptr) {
    if (n != 0u) {
      ctx.samples->request_rebind_loop(t.slot);  // the render moves the loop; the handover rebuilds the same one
    } else {
      ctx.samples->request_rebind(t.slot);
    }
    for (unsigned i = 0; i < n; ++i) {
      send_param_change(ctx, core::param_id(core::ParamKind::Instrument, t.slot, moves[i].field), moves[i].value, 0);
    }
  }
  return true;
}

// The point a click of the encoder asks for (`target`), moved to a zero crossing when ZC is on and the crossing is further along the way it moves; a snap that
// would stand still or go back leaves the step as asked, so the encoder can always leave a crossing.
unsigned snapped(const ActionContext& ctx, const Target& t, const Window& w, unsigned from, int to, int sign) noexcept {
  const unsigned clamped = static_cast<unsigned>(to < 0 ? 0 : (to > static_cast<int>(kPointMax) ? static_cast<int>(kPointMax) : to));
  if (!ctx.view.wave.zc) {
    return clamped;
  }
  const unsigned s = snap_point(t.pcm.data, t.pcm.frames, clamped, snap_radius(t.pcm.frames, w.span, kLaneColumns));
  return ((sign > 0 && s <= from) || (sign < 0 && s >= from)) ? clamped : s;
}

// The step of `detents` clicks in points: a column of the view a click (at least one point), sixteen with Shift.
int step_points(const Target& t, const Window& w, int detents, bool coarse) noexcept {
  detents = detents > kMaxDetents ? kMaxDetents : (detents < -kMaxDetents ? -kMaxDetents : detents);
  std::int64_t frames_per_click = w.span / static_cast<std::uint32_t>(kLaneColumns);
  frames_per_click = frames_per_click < 1 ? 1 : frames_per_click;
  std::int64_t units = frames_per_click * kPointMax / t.pcm.frames;
  units = units < 1 ? 1 : units;
  const std::int64_t delta = units * detents * (coarse ? kCoarse : 1);
  return static_cast<int>(delta > 200000 ? 200000 : (delta < -200000 ? -200000 : delta));
}

bool nudge(ActionContext& ctx, int detents, bool coarse) noexcept {
  Target t;
  if (detents == 0 || !target(ctx, t)) {
    return false;
  }
  WaveState& ws = ctx.view.wave;
  const Window w = wave_window(ws, t.pcm.frames);
  const int delta = step_points(t, w, detents, coarse);
  const int sign = delta > 0 ? 1 : -1;
  model::Instrument edited = *t.in;
  std::uint32_t shown = 0;  // the point that has to stay in view
  if (!chop_page(ctx)) {
    Points p = points_of(edited);
    const PointId id = static_cast<PointId>(ws.point < kPointCount ? ws.point : 0u);
    const unsigned from = get_point(p, id);
    shown = set_point(p, id, static_cast<int>(snapped(ctx, t, w, from, static_cast<int>(from) + delta, sign)));
    store_points(edited, p);
  } else {
    const unsigned count = edited.sampler.slice_count > kMaxSliceCount ? kMaxSliceCount : edited.sampler.slice_count;
    if (ws.slice >= count) {
      return say(ctx, core::Severity::Info, "NO SLICE: ADD OR EVEN FIRST");
    }
    const unsigned from = edited.sampler.slices[ws.slice];
    shown = slice_move(edited.sampler.slices, edited.sampler.slice_count, ws.slice, static_cast<int>(snapped(ctx, t, w, from, static_cast<int>(from) + delta, sign)));
  }
  const bool changed = commit(ctx, t, edited);
  if (ws.span != 0u) {  // the moved point stays in view
    const Window nw = window_show(w, t.pcm.frames, point_to_frame(shown, t.pcm.frames));
    ws.first = nw.first;
    ws.span = nw.span;
  }
  return changed;
}

// Slicing puts the instrument in the mode whose pads play slices; the user is told when that changes something.
bool set_beat_slice(ActionContext& ctx, model::Instrument& in) noexcept {
  if (in.sampler.play_mode == kBeatSlice) {
    return false;
  }
  in.sampler.play_mode = kBeatSlice;
  (void)say(ctx, core::Severity::Info, "PLAY MODE: BEAT SLICE");
  return true;
}

bool pad(ActionContext& ctx, core::ActionArg arg, bool sound) noexcept {
  if (arg < 0 || arg >= 48 || ctx.view.screen != ScreenId::Wave) {
    return false;
  }
  const unsigned slot = wave_slot(ctx.view);
  const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(slot) : nullptr;
  const unsigned count = in != nullptr ? (in->sampler.slice_count > kMaxSliceCount ? kMaxSliceCount : in->sampler.slice_count) : 0u;
  bool changed = false;
  if (chop_page(ctx) && static_cast<unsigned>(arg) < count) {
    changed = ctx.view.wave.slice != arg;
    ctx.view.wave.slice = static_cast<std::uint8_t>(arg);
  }
  if (sound && ctx.preview != nullptr) {
    const bool beat = in != nullptr && in->sampler.play_mode == kBeatSlice;
    // A beat slice instrument plays slice (note - kSliceRootNote), so pad n is note 24 + n; anything else plays the pad board's note.
    ctx.preview->pad_down(static_cast<unsigned>(arg), static_cast<unsigned>(ctx.view.pattern.track()), static_cast<unsigned>(ctx.view.instrument),
                          beat ? audio::kSliceRootNote + static_cast<unsigned>(arg) : ctx.view.edit.pads.note(static_cast<unsigned>(arg)));
  }
  return changed;
}

}  // namespace

bool wave_enabled(const ActionContext& ctx) noexcept { return ctx.view.screen == ScreenId::Wave && ctx.model.samples != nullptr; }

// The encoder: the selected point (WAVE page) or slice (CHOP page).
bool wave_nudge(ActionContext& ctx, core::ActionArg arg) noexcept { return nudge(ctx, arg, false); }
bool wave_nudge_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return nudge(ctx, arg, true); }

// F1..F4 of the WAVE page: which point the encoder moves.
bool wave_point(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < 0 || arg >= static_cast<core::ActionArg>(kPointCount)) {
    return false;
  }
  const bool changed = ctx.view.wave.point != arg;
  ctx.view.wave.point = static_cast<std::uint8_t>(arg);
  return changed;
}

bool wave_zoom_action(ActionContext& ctx, core::ActionArg arg) noexcept { return wave_zoom(ctx.model, ctx.view, arg); }

bool wave_zc(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.wave.zc = !ctx.view.wave.zc;
  return true;
}

// Shift + Up / Down on the CHOP page: how many slices EVEN makes.
bool wave_even_n(ActionContext& ctx, core::ActionArg arg) noexcept {
  const int to = static_cast<int>(ctx.view.wave.even) + (arg > 0 ? 1 : (arg < 0 ? -1 : 0));
  const std::uint8_t next = static_cast<std::uint8_t>(to < 2 ? 2 : (to > static_cast<int>(kMaxSliceCount) ? static_cast<int>(kMaxSliceCount) : to));
  const bool changed = ctx.view.wave.even != next;
  ctx.view.wave.even = next;
  return changed;
}

// F4 ADD (OS-BUG-ADDSLICE): the selected slice is split in two, at the middle of its start and end (the point there, snapped to a zero crossing when ZC is on); with no slice yet the
// whole sample is the one slice, so the first ADD comes with a slice at 0. The zoom window does not matter.
bool wave_add(ActionContext& ctx, core::ActionArg) noexcept {
  Target t;
  if (!target(ctx, t)) {
    return false;
  }
  WaveState& ws = ctx.view.wave;
  const Window w = wave_window(ws, t.pcm.frames);
  model::Instrument in = *t.in;
  const unsigned count = in.sampler.slice_count;
  const unsigned sel = ws.slice < count ? ws.slice : (count != 0u ? count - 1u : 0u);
  const unsigned from = count != 0u ? in.sampler.slices[sel] : 0u;
  const unsigned to = sel + 1u < count ? in.sampler.slices[sel + 1u] : kPointMax;
  unsigned at = from + (to - from) / 2u;
  if (ws.zc) {
    const unsigned snapped = snap_point(t.pcm.data, t.pcm.frames, at, snap_radius(t.pcm.frames, w.span, kLaneColumns));
    at = snapped > from && snapped < to ? snapped : at;  // (a snap that crossed a neighbour would not split the selected slice)
  }
  if (in.sampler.slice_count == 0u) {
    (void)slice_add(in.sampler.slices, in.sampler.slice_count, 0);
  }
  const unsigned index = slice_add(in.sampler.slices, in.sampler.slice_count, at);
  if (index == kNoSlice) {
    return say(ctx, core::Severity::Info, in.sampler.slice_count >= kMaxSliceCount ? "48 SLICES: THAT IS ALL" : "THERE IS A SLICE HERE");
  }
  (void)set_beat_slice(ctx, in);
  ws.slice = static_cast<std::uint8_t>(index);
  (void)commit(ctx, t, in);
  return true;
}

// F5 DEL: the selected slice goes.
bool wave_del(ActionContext& ctx, core::ActionArg) noexcept {
  Target t;
  if (!target(ctx, t)) {
    return false;
  }
  model::Instrument in = *t.in;
  WaveState& ws = ctx.view.wave;
  if (!slice_remove(in.sampler.slices, in.sampler.slice_count, ws.slice)) {
    return say(ctx, core::Severity::Info, "NO SLICE TO DELETE");
  }
  if (ws.slice >= in.sampler.slice_count && ws.slice > 0u) {
    --ws.slice;
  }
  (void)commit(ctx, t, in);
  return true;
}

// F2 EVEN: n slices of equal length.
bool wave_even(ActionContext& ctx, core::ActionArg) noexcept {
  Target t;
  if (!target(ctx, t)) {
    return false;
  }
  model::Instrument in = *t.in;
  (void)slice_even(in.sampler.slices, in.sampler.slice_count, ctx.view.wave.even);
  (void)set_beat_slice(ctx, in);
  ctx.view.wave.slice = 0;
  (void)commit(ctx, t, in);
  return true;
}

// F1 AUTO: the slicer is a job (App::poll_wave starts it); its answer comes back as wave.auto_apply.
bool wave_auto(ActionContext& ctx, core::ActionArg) noexcept {
  Target t;
  if (!target(ctx, t)) {
    return false;
  }
  ctx.view.wave.auto_go = true;
  return true;
}

// The slicer's answer (the App invokes it when its job is done): the slices it chose replace the instrument's, in one undo unit.
bool wave_auto_apply(ActionContext& ctx, core::ActionArg) noexcept {
  WaveState& ws = ctx.view.wave;
  if (!ws.auto_ready) {
    return false;
  }
  ws.auto_ready = false;
  Target t;
  if (ctx.samples == nullptr || ws.auto_slot != wave_slot(ctx.view) || !target(ctx, t) || ws.auto_count == 0u) {
    return false;  // the instrument in view is another one now: the answer is not for it
  }
  model::Instrument in = *t.in;
  for (unsigned i = 0; i < kMaxSliceCount; ++i) {
    in.sampler.slices[i] = i < ws.auto_count ? ctx.samples->auto_slices()[i] : std::uint16_t{0};
  }
  in.sampler.slice_count = ws.auto_count;
  (void)set_beat_slice(ctx, in);
  ws.slice = 0;
  (void)commit(ctx, t, in);
  return true;
}

// Shift + F1 MODE (OS-INSTUI S4): the list of the eight play modes opens with the cursor on the instrument's own. Only a sample instrument has one: a MIDI instrument, a module or a free slot says so.
bool wave_mode_open(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen != ScreenId::Wave || ctx.view.overlay != OverlayId::None) {
    return false;
  }
  const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(wave_slot(ctx.view)) : nullptr;
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return say(ctx, core::Severity::Info, "NO SAMPLE INSTRUMENT HERE");
  }
  ctx.view.wave.mode_pick = static_cast<std::uint8_t>(in->sampler.play_mode & 7u);
  ctx.view.overlay = OverlayId::PlayMode;
  return true;
}

// The list's F1: the chosen mode through play_mode_set (the same door as the source page's PLAY MODE row: the window fitted, a refusal says why on the strip), then the view the mode has: the slice modes
// are the CHOP page, the others the WAVE page (a wavetable and a granular instrument have their own, by the mode). The list closes whatever the answer.
bool wave_mode_pick(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::PlayMode) {
    return false;
  }
  ctx.view.overlay = OverlayId::None;
  if (ctx.edit == nullptr) {
    return true;
  }
  const unsigned slot = wave_slot(ctx.view);
  (void)play_mode_set(ctx, slot, ctx.view.wave.mode_pick);
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (in != nullptr && in->kind == model::InstrumentKind::Sample) {
    ctx.view.page[screen_index(ScreenId::Wave)] = play_mode_is_slice(in->sampler.play_mode) ? 1u : 0u;
    (void)wave_sync(ctx.model, ctx.view);
  }
  return true;
}

bool wave_pad(ActionContext& ctx, core::ActionArg arg) noexcept { return pad(ctx, arg, true); }
bool wave_pad_select(ActionContext& ctx, core::ActionArg arg) noexcept { return pad(ctx, arg, false); }

}  // namespace o3os::app
