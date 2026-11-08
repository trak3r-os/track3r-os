// SPDX-License-Identifier: GPL-3.0-or-later
// Sample edit actions (OS-011a): the selection, the effects' requests, the commit of a finished effect and the sample's own undo / redo. The protocol is in
// screens/sample_edit_screen.h: an effect is a job into a separate buffer; sample.commit swaps it in and records the step; the engine follows when stopped.
#include "action_handlers.h"

#include <o3os/app/amiga_convert.h>
#include <o3os/app/play_mode.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/sample_edit_screen.h>
#include <o3os/app/screens/wave_screen.h>

#include <cstring>

namespace o3os::app {

namespace {

constexpr int kLaneColumns = 768;
constexpr int kCoarse = 16;
constexpr int kMaxClicks = 1 << 16;
constexpr int clamp_clicks(int clicks) noexcept { return clicks > kMaxClicks ? kMaxClicks : (clicks < -kMaxClicks ? -kMaxClicks : clicks); }
constexpr const char* kMakeMono = "STEREO: MAKE MONO FIRST";  // OS-STEREO S7: Compressor and the Amiga conversion refuse a stereo sample (the STEREO row of the source page set to MONO)

bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return say_tag(ctx, severity, text, "edit");
}

PointsRec rec_of(const model::Instrument& in) noexcept {
  PointsRec r;
  r.points.start = in.sampler.start;
  r.points.loop_start = in.sampler.loop_start;
  r.points.loop_end = in.sampler.loop_end;
  r.points.end = in.sampler.end;
  std::memcpy(r.slices, in.sampler.slices, sizeof r.slices);
  r.slice_count = in.sampler.slice_count > kMaxSliceCount ? static_cast<std::uint8_t>(kMaxSliceCount) : in.sampler.slice_count;
  return r;
}

void apply_rec(model::Instrument& in, const PointsRec& r) noexcept {
  in.sampler.start = r.points.start;
  in.sampler.loop_start = r.points.loop_start;
  in.sampler.loop_end = r.points.loop_end;
  in.sampler.end = r.points.end;
  std::memcpy(in.sampler.slices, r.slices, sizeof r.slices);
  in.sampler.slice_count = r.slice_count;
  if (r.conv) {  // the AMIGA conversion's step: what it changed beside the audio (OS-AMG1)
    in.sample_rate = r.rate;
    in.engine = r.engine;
    in.sampler.tune = r.tune;
  }
  if (r.pm != 0xFFu) {  // a WAVETABLE plugin's run (S4c): the play mode and its window, as play_mode_set leaves them
    in.sampler.play_mode = r.pm;
    in.sampler.sample_type = r.pm == kPlayModeWavetable ? 1u : 0u;
    in.sampler.wavetable_window_size = r.wsz;
    in.sampler.wavetable_window = r.wpos;
    model::wt_slot_follow_mode(in);
  }
}

bool move_edge(ActionContext& ctx, int clicks, bool coarse) noexcept {
  const SampleBank::Pcm pcm = ctx.model.samples != nullptr ? ctx.model.samples->pcm(wave_slot(ctx.view)) : SampleBank::Pcm{};
  if (pcm.frames == 0u || clicks == 0) {
    return say(ctx, core::Severity::Info, "NO SAMPLE IN THIS INSTRUMENT");
  }
  SampleEditState& st = ctx.view.sedit;
  const SampleEditState before = st;
  clicks = clamp_clicks(clicks);
  const Window w = sedit_window(st, pcm.frames);
  std::int64_t per_click = w.span / static_cast<std::uint32_t>(kLaneColumns);
  per_click = per_click < 1 ? 1 : per_click;
  if (st.b <= st.a) {  // no selection yet: start from all of it
    st.a = 0;
    st.b = pcm.frames;
  }
  std::uint32_t& edge = st.edge == 0u ? st.a : st.b;
  const std::int64_t to = static_cast<std::int64_t>(edge) + per_click * clicks * (coarse ? kCoarse : 1);
  if (st.edge == 0u) {
    st.a = static_cast<std::uint32_t>(to < 0 ? 0 : (to > static_cast<std::int64_t>(st.b) - 1 ? static_cast<std::int64_t>(st.b) - 1 : to));
  } else {
    st.b = static_cast<std::uint32_t>(to < static_cast<std::int64_t>(st.a) + 1 ? static_cast<std::int64_t>(st.a) + 1 : (to > pcm.frames ? pcm.frames : to));
  }
  const std::uint32_t shown = st.edge == 0u ? st.a : st.b;
  if (st.span != 0u) {  // the edge stays in view
    const Window nw = window_show(w, pcm.frames, shown >= pcm.frames ? pcm.frames - 1u : shown);
    st.first = nw.first;
    st.span = nw.span;
  }
  return st.a != before.a || st.b != before.b || st.first != before.first;
}

bool undo_redo(ActionContext& ctx, bool undo) noexcept {
  const unsigned slot = wave_slot(ctx.view);
  const model::Instrument* in = ctx.edit != nullptr ? ctx.edit->project().instrument(slot) : nullptr;
  if (ctx.samples == nullptr || in == nullptr) {
    return say(ctx, core::Severity::Info, undo ? "NOTHING TO UNDO" : "NOTHING TO REDO");
  }
  PointsRec restore;
  if (!(undo ? ctx.samples->edit_undo(slot, restore) : ctx.samples->edit_redo(slot, restore))) {
    return say(ctx, core::Severity::Info, undo ? "NOTHING TO UNDO" : "NOTHING TO REDO");
  }
  model::Instrument edited = *in;
  apply_rec(edited, restore);
  if (std::memcmp(&edited, in, sizeof edited) != 0) {
    (void)ctx.edit->set_instrument(slot, edited);
  }
  (void)say(ctx, core::Severity::Info, undo ? "UNDONE" : "REDONE");
  return true;  // the audio swapped (the bank asked for the handover); the selection is checked against the new length by the App's sync
}

}  // namespace

bool sedit_enabled(const ActionContext& ctx) noexcept { return ctx.view.screen == ScreenId::SampleEdit && ctx.model.samples != nullptr; }

bool sedit_move(ActionContext& ctx, core::ActionArg arg) noexcept { return move_edge(ctx, arg, false); }
bool sedit_move_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return move_edge(ctx, arg, true); }

bool sedit_all(ActionContext& ctx, core::ActionArg) noexcept {
  const bool changed = ctx.view.sedit.a != 0u || ctx.view.sedit.b != 0u;
  ctx.view.sedit.a = ctx.view.sedit.b = 0;
  return changed;
}

bool sedit_amp(ActionContext& ctx, core::ActionArg arg) noexcept {
  const int to = static_cast<int>(ctx.view.sedit.amp) + (arg > 0 ? 10 : (arg < 0 ? -10 : 0));
  const std::uint16_t next = static_cast<std::uint16_t>(to < SampleEditState::kAmpMin ? SampleEditState::kAmpMin : (to > SampleEditState::kAmpMax ? SampleEditState::kAmpMax : to));
  const bool changed = ctx.view.sedit.amp != next;
  ctx.view.sedit.amp = next;
  return changed;
}

// The FX page (OS-AUDIT-15): the encoder changes the row `fx_sel` by `clicks` steps (coarse: Shift), held to its range.
namespace {
bool fx_edit(ActionContext& ctx, int clicks, bool coarse) noexcept {
  SampleEditState& st = ctx.view.sedit;
  const unsigned row = st.fx_sel < kFxCount ? st.fx_sel : 0u;
  const FxParamDef& d = kFxParams[row];
  clicks = clamp_clicks(clicks);
  int v = st.fx[row];
  for (int n = clicks < 0 ? -clicks : clicks; n > 0; --n) {
    int step = coarse ? d.coarse : d.step;
    if (d.log) {
      step = (coarse ? v / 4 : v / 16);
      step = step < 1 ? 1 : step;
    }
    v += clicks < 0 ? -step : step;
    v = v < d.min ? d.min : (v > d.max ? d.max : v);
  }
  const bool changed = v != st.fx[row];
  st.fx[row] = static_cast<std::int16_t>(v);
  return changed;
}
}  // namespace

bool sedit_fx(ActionContext& ctx, core::ActionArg arg) noexcept { return fx_edit(ctx, arg, false); }
bool sedit_fx_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return fx_edit(ctx, arg, true); }

// The PLUGINS page (OS-CEPTFX S4a): no card access here. The encoder moves the listing index (list focus; the App's pass reads the plugin's header after it) or the value of the row; a run (S4b) or a scan
// in progress holds both, so the listing and the plugin in view stay what the run was asked with.
namespace {
bool plug_busy(const ActionContext& ctx, const PtfxPage* page) noexcept {
  return ctx.view.sedit.running || (page->cat != nullptr && page->cat->state() == module::PtfxCatalog::State::Scanning);
}

bool plug_edit(ActionContext& ctx, int clicks, bool coarse) noexcept {
  PtfxPage* page = ctx.model.samples != nullptr ? ctx.model.samples->ptfx_page() : nullptr;
  if (page == nullptr || page->cat == nullptr || clicks == 0) {
    return false;
  }
  if (plug_busy(ctx, page)) {
    return say(ctx, core::Severity::Info, "BUSY");
  }
  SampleEditState& st = ctx.view.sedit;
  if (st.plug_focus == 0u) {
    const int count = static_cast<int>(page->cat->count());
    if (count == 0) {
      return false;
    }
    const int to = static_cast<int>(st.plug_sel) + clamp_clicks(clicks) * (coarse ? 10 : 1);
    const std::uint8_t next = static_cast<std::uint8_t>(to < 0 ? 0 : (to > count - 1 ? count - 1 : to));
    const bool changed = next != st.plug_sel;
    st.plug_sel = next;
    st.plug_param = 0;
    return changed;
  }
  return page->bump(st.plug_param, clicks, coarse);  // (bump holds the clicks to its own range)
}
}  // namespace

bool sedit_plug(ActionContext& ctx, core::ActionArg arg) noexcept { return plug_edit(ctx, arg, false); }
bool sedit_plug_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return plug_edit(ctx, arg, true); }

bool sedit_plug_do(ActionContext& ctx, core::ActionArg arg) noexcept {
  PtfxPage* page = ctx.model.samples != nullptr ? ctx.model.samples->ptfx_page() : nullptr;
  if (page == nullptr || page->cat == nullptr) {
    return say(ctx, core::Severity::Info, "NO PLUGINS HERE");
  }
  SampleEditState& st = ctx.view.sedit;
  if (plug_busy(ctx, page)) {
    return say(ctx, core::Severity::Info, "BUSY");
  }
  if (arg == 1) {  // F1 APPLY: the App's pass checks the plugin and the sample and runs it (the refusal is its strip text)
    st.plug_apply = true;
    return true;
  }
  if (arg == 2) {  // F2 RESCAN: the App starts the catalogue job (the listing and the plugin in view are read again)
    st.plug_scan = true;
    return true;
  }
  return arg == 3 ? page->reset_all() : page->reset(st.plug_param);
}

// F1..F6, and Shift + F4 (DC REMOVE, OS-SEDFX): the effect (arg = EditOp) is asked for; the App starts the job. On the FX page F1 / F2 are FILTER and COMPRESSOR (OS-AUDIT-15).
bool sedit_run(ActionContext& ctx, core::ActionArg arg) noexcept {
  const bool fx_op = arg == static_cast<core::ActionArg>(EditOp::Filter) || arg == static_cast<core::ActionArg>(EditOp::Compressor);
  if (arg < 0 || (arg > static_cast<core::ActionArg>(EditOp::Amplify) && arg != static_cast<core::ActionArg>(EditOp::DcRemove) && !fx_op)) {
    return false;
  }
  const unsigned slot = wave_slot(ctx.view);
  const SampleBank::Pcm pcm = ctx.model.samples != nullptr ? ctx.model.samples->pcm(slot) : SampleBank::Pcm{};
  const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(slot) : nullptr;
  if (pcm.frames == 0u || in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return say(ctx, core::Severity::Info, "NO SAMPLE IN THIS INSTRUMENT");
  }
  if (static_cast<EditOp>(arg) == EditOp::Compressor && ctx.model.samples->edit_stereo(slot)) {  // OS-STEREO S7: one signal only
    return say(ctx, core::Severity::Info, kMakeMono);
  }
  const SampleEditState& st = ctx.view.sedit;
  if (static_cast<EditOp>(arg) == EditOp::Crop && !(st.b > st.a && st.b - st.a < pcm.frames)) {
    return say(ctx, core::Severity::Info, st.b > st.a ? "THE SELECTION IS ALL OF IT" : "SELECT WHAT TO KEEP FIRST");
  }
  ctx.view.sedit.op = static_cast<EditOp>(arg);
  ctx.view.sedit.go = true;
  return true;
}

// Shift + F1 / F2: the AMIGA conversion of the whole sample (OS-AMG1, amiga_convert.h), arg 0: 8,287 Hz (period 428), 1: 16,574 Hz (period 214, one octave up: the tune gets +12 so that the pitch stays).
// Refused for a wavetable (its windows are the sample's own length), when the tune has no room for +12, and when the sample already is what it asks for. Shift + F3: the 8-bit option.
bool sedit_amiga(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < 0 || arg > 1) {
    return false;
  }
  const unsigned slot = wave_slot(ctx.view);
  const SampleBank::Pcm pcm = ctx.model.samples != nullptr ? ctx.model.samples->pcm(slot) : SampleBank::Pcm{};
  const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(slot) : nullptr;
  if (pcm.frames == 0u || in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return say(ctx, core::Severity::Info, "NO SAMPLE IN THIS INSTRUMENT");
  }
  if (ctx.model.samples->edit_stereo(slot)) {  // OS-STEREO S7: the conversion makes one signal
    return say(ctx, core::Severity::Info, kMakeMono);
  }
  const unsigned period = arg == 0 ? kAmigaPeriod8 : kAmigaPeriod16;
  if (in->sampler.sample_type == 1u || in->sampler.play_mode == 6u) {
    return say(ctx, core::Severity::Info, "A WAVETABLE CANNOT BE CONVERTED");
  }
  if (period == kAmigaPeriod16 && in->sampler.tune > 24 - kAmigaTune16) {
    return say(ctx, core::Severity::Info, "TUNE TOO HIGH FOR 16 KHZ");
  }
  const bool src_amiga = ctx.model.samples->engine_for(slot, *in) == audio::kEngineAmiga;
  if (src_amiga && period == kAmigaPeriod8) {
    return say(ctx, core::Severity::Info, "ALREADY AMIGA 8287 HZ");
  }
  ctx.view.sedit.op = EditOp::Amiga;
  ctx.view.sedit.period = static_cast<std::uint16_t>(period);
  ctx.view.sedit.go = true;
  return true;
}

bool sedit_q8(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.sedit.q8 = !ctx.view.sedit.q8;
  (void)say(ctx, core::Severity::Info, ctx.view.sedit.q8 ? "AMIGA: 8-BIT ON" : "AMIGA: 8-BIT OFF");
  return true;
}

// Shift + F5 (OS-QAFIX-27, new Community OS behaviour): the next instrument to edit, 1..64 and round to 1 (OS-SLD-RANGE-ALL: any slot can be any type, as Sample load's target); a pad picks one of 1..48
// directly (instrument_select, which also resets the window and the selection). A slot that is not a sample instrument is stepped onto like any other: the effects say NO SAMPLE IN THIS INSTRUMENT.
bool sedit_instrument(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg == 0) {
    return false;
  }
  constexpr int kSlots = static_cast<int>(model::kMaxInstruments);
  const int next = ctx.view.instrument + (arg < 0 ? -1 : 1);
  const int to = next < 1 ? kSlots : (next > kSlots ? 1 : next);
  if (to <= static_cast<int>(kInstruments)) {
    return instrument_select(ctx, to);
  }
  release_live_pads(ctx);  // as instrument_select (which refuses what a pad cannot give): the same step for 49..64
  ctx.view.instrument = to;
  wave_view_reset(ctx.view);
  return true;
}

// The App invokes it when the effect's job is done: the staged buffer becomes the slot's audio (an undo unit; the bank keeps the step), a crop's points follow the new length.
bool sample_commit(ActionContext& ctx, core::ActionArg) noexcept {
  SampleEditState& st = ctx.view.sedit;
  if (!st.ready) {
    return false;
  }
  st.ready = false;
  const unsigned slot = st.ready_slot;
  const model::Instrument* in = ctx.edit != nullptr ? ctx.edit->project().instrument(slot) : nullptr;
  if (ctx.samples == nullptr || in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return false;
  }
  PointsRec before = rec_of(*in);
  const std::uint32_t old_frames = ctx.samples->pcm(slot).frames;
  PointsRec after = st.ready_op == EditOp::Crop ? crop_points(before, old_frames, st.ready_a, st.ready_b) : before;
  const PtfxJob* const run = st.ready_plug ? &ctx.samples->ptfx_job() : nullptr;  // OS-CEPTFX S4c: the run that made the staged buffer (it stays in the bank until the next one)
  if (run != nullptr) {
    // A generator made a new sample (the old points name frames of the old one): the whole range, no slices. An effect that added frames (Add Silence) moves the points as a crop would.
    after = run->generator() ? PointsRec{} : insert_points(before, old_frames, run->body_a(), run->body_b(), run->prepend(), run->append());
  }
  const bool wavetable = run != nullptr && run->wavetable();
  const unsigned wsize = wavetable ? wt_fit_size(in->sampler.wavetable_window_size, st.staged_frames) : 0u;
  if (wsize != 0u) {  // the plugin makes single-cycle frames: the instrument plays them as a wavetable, in the same undo unit as the audio (F8 puts the mode back)
    before.pm = in->sampler.play_mode;
    before.wsz = in->sampler.wavetable_window_size;
    before.wpos = in->sampler.wavetable_window;
    after.pm = kPlayModeWavetable;
    after.wsz = static_cast<std::uint16_t>(wsize);
    after.wpos = wt_clamp_pos(st.staged_frames, wsize, in->sampler.wavetable_window);
  }
  if (st.ready_op == EditOp::Amiga) {  // one undo unit: the audio, and the rate, engine and tune it goes with (the points are fractions of the length: they follow it)
    const int tune = in->sampler.tune + (st.ready_period == kAmigaPeriod16 ? kAmigaTune16 : 0);
    if (tune > 24) {  // the tune was raised while the job ran
      return say(ctx, core::Severity::Warn, "THE EDIT COULD NOT BE KEPT");
    }
    before.conv = after.conv = true;
    before.engine = in->engine;
    before.tune = in->sampler.tune;
    before.rate = in->sample_rate;
    after.engine = model::kInstEngineAmiga;
    after.tune = static_cast<std::int8_t>(tune);
    after.rate = amiga_rate_hz(st.ready_period);
  }
  std::int16_t* const side = st.ready_stereo && st.staged != nullptr ? st.staged + ((st.staged_frames + 1u) & ~1u) : nullptr;  // OS-STEREO (d): S behind M in the same buffer
  if (!ctx.samples->edit_commit(slot, st.staged, st.staged_frames, before, after, side)) {
    return say(ctx, core::Severity::Warn, "THE EDIT COULD NOT BE KEPT");
  }
  model::Instrument edited = *in;
  apply_rec(edited, after);
  if (std::memcmp(&edited, in, sizeof edited) != 0) {
    (void)ctx.edit->set_instrument(slot, edited);
  }
  st.staged = nullptr;
  st.ready_plug = false;
  if (wavetable && wsize == 0u) {  // under 32 frames the sample has no window: the mode stays
    (void)say(ctx, core::Severity::Warn, "WAVETABLE NEEDS A SAMPLE OF 32 FRAMES");
  }
  if (run != nullptr && run->generator()) {  // the length changed: a selection of the old one means nothing
    st.a = st.b = 0;
    st.first = st.span = 0;
  }
  if (st.ready_op == EditOp::Amiga) {
    (void)say(ctx, core::Severity::Info, st.ready_period == kAmigaPeriod16 ? "AMIGA 16574 HZ, ENGINE AMIGA, TUNE +12" : "AMIGA 8287 HZ, ENGINE AMIGA");
    st.a = st.b = 0;  // the length changed: a selection of the old one means nothing
    st.first = st.span = 0;
  }
  if (st.ready_op == EditOp::Crop) {
    st.a = st.b = 0;
    st.first = st.span = 0;
  }
  return true;
}

// F8 UNDO, and (OS-SEDIT-CANCEL, new Community OS behaviour) F8 CANCEL while an effect's job runs: the App stops the job at its next pass (the sample is untouched, no history step, the strip says
// "<EFFECT> CANCELLED"); the key row reads CANCEL meanwhile. Nothing changes in the project, so no undo unit is left behind.
bool sedit_undo(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.sedit.running) {
    ctx.view.sedit.cancel = true;
    return true;
  }
  return undo_redo(ctx, true);
}
bool sedit_redo(ActionContext& ctx, core::ActionArg) noexcept { return undo_redo(ctx, false); }

}  // namespace o3os::app
