// SPDX-License-Identifier: GPL-3.0-or-later
// The Mixer screen's actions (OS-011b). The model's parameters (track level, pan, mute and solo, the master's levels and switches) are edited as every parameter is: through
// param_actions.cpp, so they are undoable, learnable and sent to the engine as a ParamChange. What only the engine has (the routing matrix, the CUE level, OUT MODE, the effect
// returns, the INPUT level) is the MixBoard's: set here and told to the engine in the same way, not undoable (nothing in the project changes), and the stem map is edited in
// place and handed to the USB stream.
#include "param_access.h"

#include <o3os/app/mixer_view.h>
#include <o3os/app/screens/mixer_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/audio/graph_params.h>

namespace o3os::app {

namespace {

namespace tp = model::track_param;
namespace mp = model::master_param;
namespace gp = audio::graph_param;

constexpr int kCoarseDetents = 10;  // a coarse turn of an engine level is ten fine ones (the descriptors' own coarse step for a 0..100 level)

unsigned clamp100(int v) noexcept { return v < 0 ? 0u : (v > 100 ? 100u : static_cast<unsigned>(v)); }

// An engine field: the board's copy is the one the screen shows, the engine hears the same value (ramp 0: its own smoothing, so a level does not click).
void tell_strip(ActionContext& ctx, unsigned strip, unsigned field, float value) noexcept {
  send_param_change(ctx, audio::strip_param(strip, field), value);
}
void tell_master(ActionContext& ctx, unsigned field, float value) noexcept { send_param_change(ctx, audio::master_param(field), value); }

// Where the cursor is on the ROUTING page.
struct RouteRow {
  enum Kind : std::uint8_t { Source, Return, OutMode } kind;
  unsigned index;  // the strip (a track, or audio::kInputStrip) / the return 0 or 1
};
RouteRow route_row(const Model& model, unsigned row) noexcept {
  const unsigned n = mixer::tracks(model);
  if (row < n) {
    return {RouteRow::Source, row};
  }
  if (row == n) {
    return {RouteRow::Source, audio::kInputStrip};
  }
  if (row == n + 1u || row == n + 2u) {
    return {RouteRow::Return, row - n - 1u};
  }
  return {RouteRow::OutMode, 0};
}

// A send effect's control (OS-SENDFX: the MASTER FX page's rows after the CUE LEVEL, send_fx_rows.h): the board's value moves by the turn, the engine is told in the same way as the
// levels above (a ParamChange of kind Master), nothing is stored in the project.
// `index` counts the controls of the page after the CUE LEVEL (kMasterFxControls): the send effects' first, then the master limiter's three (OS-LIMIT, limiter_rows.h).
bool edit_fx(ActionContext& ctx, unsigned index, int detents, bool coarse) noexcept {
  MixBoard* mix = ctx.mix;
  if (mix == nullptr || index >= kMasterFxControls) {
    return false;
  }
  const SendFxRow& row = master_fx_row(index);
  const int now = mix->control(index);
  const int next = send_fx_step(row, now, detents, coarse);
  if (next == now) {
    return false;
  }
  mix->control(index) = static_cast<std::uint16_t>(next);
  tell_master(ctx, row.field, static_cast<float>(next));
  return true;
}
bool reset_fx(ActionContext& ctx, unsigned index) noexcept {
  MixBoard* mix = ctx.mix;
  if (mix == nullptr || index >= kMasterFxControls || mix->control(index) == master_fx_row(index).def) {
    return false;
  }
  mix->control(index) = master_fx_row(index).def;
  tell_master(ctx, master_fx_row(index).field, static_cast<float>(master_fx_row(index).def));
  return true;
}

bool edit_level(ActionContext& ctx, std::uint8_t& level, int detents, bool coarse, unsigned strip, bool master_field, unsigned field) noexcept {
  const unsigned now = level;
  const unsigned next = clamp100(static_cast<int>(now) + (coarse ? detents * kCoarseDetents : detents));
  if (next == now) {
    return false;
  }
  level = static_cast<std::uint8_t>(next);
  if (master_field) {
    tell_master(ctx, field, static_cast<float>(next));
  } else {
    tell_strip(ctx, strip, field, static_cast<float>(next));
  }
  return true;
}

// STEMS: the cursor's source added to (set) or cleared from the cursor's slot (row 1 is S1: row 0 is the OUTPUTS row), and the map handed to the stream. A bit already as asked, no map, the
// OUTPUTS row or a row past the profile's channels change nothing and hand nothing on.
bool stem_cell(ActionContext& ctx, bool set) noexcept {
  MixBoard* mix = ctx.mix;
  if (mix == nullptr || mix->stems == nullptr) {
    return false;
  }
  const mixer::Cursor c = mixer::cursor(ctx.model, ctx.view);
  if (c.row == 0u || c.row > mix->stems->channels()) {
    return false;
  }
  const unsigned slot = c.row - 1u;
  const std::uint32_t bit = std::uint32_t{1} << c.col;
  const std::uint32_t now = mix->stems->slot(slot);
  const std::uint32_t next = set ? (now | bit) : (now & ~bit);
  if (next == now || !mix->stems->set_slot(slot, next)) {
    return false;
  }
  (void)mix->commit_stems();
  return true;
}

// OS-USBOUTS: a turn on the OUTPUTS row changes the PENDING value (shown starred): from the pending value, or from the number now presented; back at the presented one it is no longer pending.
bool outputs_turn(ActionContext& ctx, int detents) noexcept {
  MixBoard* mix = ctx.mix;
  if (mix == nullptr || mix->stems == nullptr || mix->request_outputs == nullptr || detents == 0) {
    return false;
  }
  MixerView& mv = ctx.view.mixer;
  const std::uint8_t now = static_cast<std::uint8_t>(mix->stems->channels());
  const std::uint8_t base = mv.outputs_pending != 0u ? mv.outputs_pending : now;
  std::uint8_t next = base;
  for (int n = detents < 0 ? -detents : detents; n > 0; --n) {
    next = mixer::outputs_next(next, detents);  // one offered value a detent
  }
  const std::uint8_t pending = next == now ? 0u : next;
  if (pending == mv.outputs_pending) {
    return false;
  }
  mv.outputs_pending = pending;
  mv.reset_asked = 0;  // a changed value is asked again
  return true;
}

// The turn of the encoder (or its coarse form) on whatever the cursor is on; the model's parameters go to the generic edit.
bool edit_at_cursor(ActionContext& ctx, core::ActionArg detents, bool coarse) noexcept {
  if (ctx.view.screen != ScreenId::Mixer) {
    return false;
  }
  const mixer::Cursor c = mixer::cursor(ctx.model, ctx.view);
  if (focus_param(ctx) != core::ParamId::None) {
    return coarse ? param_coarse(ctx, detents) : param_edit(ctx, detents);
  }
  MixBoard* mix = ctx.mix;
  if (mix == nullptr) {
    return false;
  }
  switch (mixer::page_of(ctx.view)) {
    case MixerPage::Mixer:
      switch (static_cast<MixBus>(mixer::bus_of(ctx.model, c.col))) {
        case MixBus::In: return edit_level(ctx, mix->in_level, detents, coarse, audio::kInputStrip, false, gp::strip::Volume);
        case MixBus::Cue: return edit_level(ctx, mix->cue_level, detents, coarse, 0, true, gp::master::CueLevel);
        default: return false;
      }
    case MixerPage::MasterFx:
      if (c.row >= kMasterFirstFxRow) {
        return edit_fx(ctx, c.row - kMasterFirstFxRow, detents, coarse);
      }
      return c.row == kMasterCueRow && edit_level(ctx, mix->cue_level, detents, coarse, 0, true, gp::master::CueLevel);
    case MixerPage::Routing: {
      const RouteRow rr = route_row(ctx.model, c.row);
      if (rr.kind == RouteRow::Source) {
        const std::uint8_t next = route_turn(mix->route[rr.index], rr.index, c.col, detents);
        if (next == mix->route[rr.index]) {
          return false;
        }
        mix->route[rr.index] = next;
        tell_strip(ctx, rr.index, gp::strip::Route, static_cast<float>(next));
        return true;
      }
      if (rr.kind == RouteRow::Return) {
        const std::uint8_t next = return_turn(mix->ret_route[rr.index], c.col, detents);
        if (next == mix->ret_route[rr.index]) {
          return false;
        }
        mix->ret_route[rr.index] = next;
        tell_master(ctx, rr.index == 0u ? gp::master::RetRouteA : gp::master::RetRouteB, static_cast<float>(next));
        return true;
      }
      const std::uint8_t next = detents > 0 ? 1u : (detents < 0 ? 0u : mix->out_mode);
      if (next == mix->out_mode) {
        return false;
      }
      mix->out_mode = next;
      tell_master(ctx, gp::master::OutMode, static_cast<float>(next));
      return true;
    }
    case MixerPage::Stems:
      if (c.row == 0u) {
        return outputs_turn(ctx, detents);
      }
      return detents != 0 && stem_cell(ctx, detents > 0);
  }
  return false;
}

}  // namespace

bool mixer_edit(ActionContext& ctx, core::ActionArg arg) noexcept { return edit_at_cursor(ctx, arg, false); }
bool mixer_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return edit_at_cursor(ctx, arg, true); }

// OS-MASTERVOL: Master held + the encoder moves the master VOLUME from any screen (bindings.cpp: hold_encoder). It is the Mixer's own parameter, so the edit has no history (kMixerActions), and the
// strip says the value, since the screen in view may not show it. At either end of the range the strip still says where it is.
bool mixer_master_vol(ActionContext& ctx, core::ActionArg arg) noexcept {
  const core::ParamId id = core::param_id(core::ParamKind::Master, 0, mp::Volume);
  const bool edited = param_edit_id(ctx, id, arg);
  float now = 0.0f;
  if (!read_param(ctx.model, id, now)) {
    return edited;  // no project: param_edit_id has said so
  }
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("MASTER VOLUME ");
  append_uint(text, static_cast<unsigned>(now + 0.5f));
  text.append("%");
  return ctx.view.notify.upsert(core::Severity::Info, text.c_str(), "master", ViewState::kMessageMs) || edited;
}

// Delete: the default. A model parameter goes to the generic reset; the board's values go back to the engine's defaults. On STEMS, Delete clears the cursor's source from its
// slot and Shift + Delete (arg 1) puts the whole map back to the factory slots of its profile; on the other pages Shift + Delete is the plain Delete.
bool mixer_reset(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.screen != ScreenId::Mixer) {
    return false;
  }
  if (focus_param(ctx) != core::ParamId::None) {
    return param_reset(ctx, 0);
  }
  MixBoard* mix = ctx.mix;
  if (mix == nullptr) {
    return false;
  }
  const mixer::Cursor c = mixer::cursor(ctx.model, ctx.view);
  switch (mixer::page_of(ctx.view)) {
    case MixerPage::Mixer:
      switch (static_cast<MixBus>(mixer::bus_of(ctx.model, c.col))) {
        case MixBus::In: return edit_level(ctx, mix->in_level, -100, false, audio::kInputStrip, false, gp::strip::Volume);
        case MixBus::Cue: return edit_level(ctx, mix->cue_level, 100, false, 0, true, gp::master::CueLevel);
        default: return false;
      }
    case MixerPage::MasterFx:
      if (c.row >= kMasterFirstFxRow) {
        return reset_fx(ctx, c.row - kMasterFirstFxRow);
      }
      return c.row == kMasterCueRow && edit_level(ctx, mix->cue_level, 100, false, 0, true, gp::master::CueLevel);
    case MixerPage::Routing: {
      const RouteRow rr = route_row(ctx.model, c.row);
      if (rr.kind == RouteRow::Source) {
        const std::uint8_t def = rr.index == audio::kInputStrip ? static_cast<std::uint8_t>(audio::route::kMain | audio::route::kCue)
                                                                : static_cast<std::uint8_t>(audio::route::kMain | audio::route::kUsb);
        if (mix->route[rr.index] == def) {
          return false;
        }
        mix->route[rr.index] = def;
        tell_strip(ctx, rr.index, gp::strip::Route, static_cast<float>(def));
        return true;
      }
      if (rr.kind == RouteRow::Return) {
        if (mix->ret_route[rr.index] == audio::route::kMain) {
          return false;
        }
        mix->ret_route[rr.index] = audio::route::kMain;
        tell_master(ctx, rr.index == 0u ? gp::master::RetRouteA : gp::master::RetRouteB, static_cast<float>(audio::route::kMain));
        return true;
      }
      if (mix->out_mode == 0u) {
        return false;
      }
      mix->out_mode = 0;
      tell_master(ctx, gp::master::OutMode, 0.0f);
      return true;
    }
    case MixerPage::Stems: {
      // Final, no undo (the Mixer has none, maintainer 2026-10-22): the key asks first and the same key again within the window does it.
      MixerView& mv = ctx.view.mixer;
      if (mix->stems != nullptr && arg == 0 && c.row == 0u) {
        // OS-USBOUTS: Delete on the OUTPUTS row drops the pending value (nothing is lost: no question)
        const bool had = mv.outputs_pending != 0u;
        mv.outputs_pending = 0;
        mv.reset_asked = 0;
        return had;
      }
      if (mix->stems == nullptr || (arg == 0 && (c.row == 0u || c.row > mix->stems->channels() || (mix->stems->slot(c.row - 1u) & (std::uint32_t{1} << c.col)) == 0u))) {
        return false;  // no map, or the cursor's source is not in the slot: nothing would be lost, so nothing is asked
      }
      const std::uint8_t which = arg == 0 ? 1u : 2u;
      const bool same_cell = which == 2u || (mv.reset_row == c.row && mv.reset_col == c.col);  // (the whole-map reset does not depend on the cursor)
      if (mv.reset_asked != which || !same_cell || core::tick_elapsed(ctx.now, mv.reset_at) > core::ticks_from_ms(MixerView::kResetWindowMs)) {
        mv.reset_asked = which;
        mv.reset_row = static_cast<std::uint8_t>(c.row);
        mv.reset_col = static_cast<std::uint8_t>(c.col);
        mv.reset_at = ctx.now;
        return ctx.view.notify.ask(core::Severity::Warn, which == 1u ? "DELETE AGAIN: THE SOURCE LEAVES THE SLOT" : "SHIFT+DELETE AGAIN: STEMS BACK TO FACTORY", "mixer", ViewState::kMessageMs);
      }
      mv.reset_asked = 0;
      if (arg == 0) {
        return stem_cell(ctx, false);  // Delete: the cursor's source leaves the slot
      }
      if (mix->stems == nullptr || !mix->stems->reset(mix->stems->profile())) {
        return false;  // Shift + Delete: the whole map back to the factory slots
      }
      (void)mix->commit_stems();
      return true;
    }
  }
  return false;
}

// Insert: the cursor's source joins the slot (the same edit as a turn up of the encoder, without the detent).
bool mixer_stem_set(ActionContext& ctx, core::ActionArg) noexcept {
  return ctx.view.screen == ScreenId::Mixer && mixer::page_of(ctx.view) == MixerPage::Stems && stem_cell(ctx, true);
}

namespace {

// Writes a model switch (0 or 1) as a parameter: undoable, and told to the engine.
bool toggle_param(ActionContext& ctx, core::ParamId id) noexcept {
  float now = 0.0f;
  if (!read_param(ctx.model, id, now)) {
    return say_not_written(ctx, WriteResult::NoProject);
  }
  const WriteResult r = write_param(ctx, id, now != 0.0f ? 0.0f : 1.0f);
  return r == WriteResult::Changed || (r != WriteResult::Unchanged && say_not_written(ctx, r));
}

// What F7 / F8 say when the cursor is on nothing that has a mute or a solo (OS-QAFIX-9: a labelled key answers).
bool flag_refused(ActionContext& ctx, const char* text) noexcept { return ctx.view.notify.post(core::Severity::Info, text, "mixer", ViewState::kMessageMs); }

// The master parameter row (kMasterShownParams rows: VOLUME, DRY, REVERB, DELAY, then the three mute switches) -> the mute switch of its bus, or 0xFF.
unsigned master_mute_of_row(unsigned row) noexcept {
  switch (row) {
    case mp::Dry: case mp::DryMute: return mp::DryMute;
    case mp::Reverb: case mp::ReverbMute: return mp::ReverbMute;
    case mp::Delay: case mp::DelayMute: return mp::DelayMute;
    default: return 0xFFu;
  }
}

// The mute (or solo) of what the cursor is on: a track on the MIXER and ROUTING pages (a track's row), the effect's mute on SND A and SND B; on MASTER FX the mute of the bus the row belongs to (DRY, REVERB, DELAY),
// on STEMS the mute or solo of the source column's track (SA and SB: the effect's mute). Anything else says TRACKS ONLY (or NO TRACK for a column the project has no track for).
bool flag_at_cursor(ActionContext& ctx, bool solo) noexcept {
  if (ctx.view.screen != ScreenId::Mixer) {
    return false;
  }
  const mixer::Cursor c = mixer::cursor(ctx.model, ctx.view);
  const unsigned tracks = mixer::tracks(ctx.model);
  switch (mixer::page_of(ctx.view)) {
    case MixerPage::Mixer:
      if (mixer::is_track_column(ctx.model, c.col)) {
        return toggle_param(ctx, core::param_id(core::ParamKind::Track, c.col, solo ? tp::Solo : tp::Mute));
      }
      if (!solo) {
        switch (static_cast<MixBus>(mixer::bus_of(ctx.model, c.col))) {
          case MixBus::SndA: return toggle_param(ctx, core::param_id(core::ParamKind::Master, 0, mp::DelayMute));
          case MixBus::SndB: return toggle_param(ctx, core::param_id(core::ParamKind::Master, 0, mp::ReverbMute));
          default: break;
        }
      }
      return flag_refused(ctx, "TRACKS ONLY");
    case MixerPage::Routing:
      return c.row < tracks ? toggle_param(ctx, core::param_id(core::ParamKind::Track, c.row, solo ? tp::Solo : tp::Mute)) : flag_refused(ctx, "TRACKS ONLY");  // (the rows past the tracks are IN, SND A and SND B: not a missing track)
    case MixerPage::MasterFx: {
      const unsigned mute = solo ? 0xFFu : master_mute_of_row(c.row);
      return mute != 0xFFu ? toggle_param(ctx, core::param_id(core::ParamKind::Master, 0, mute)) : flag_refused(ctx, solo ? "TRACKS ONLY" : "DRY, REVERB OR DELAY ROW");
    }
    case MixerPage::Stems: {
      if (c.col < tracks) {
        return toggle_param(ctx, core::param_id(core::ParamKind::Track, c.col, solo ? tp::Solo : tp::Mute));
      }
      if (c.col < kStemSources - 4u) {  // T(n + 1)..T12 of a project with fewer tracks
        return flag_refused(ctx, "NO TRACK");
      }
      if (!solo && c.col == kStemSources - 2u) {  // SA
        return toggle_param(ctx, core::param_id(core::ParamKind::Master, 0, mp::DelayMute));
      }
      if (!solo && c.col == kStemSources - 1u) {  // SB
        return toggle_param(ctx, core::param_id(core::ParamKind::Master, 0, mp::ReverbMute));
      }
      return flag_refused(ctx, "TRACKS ONLY");  // ML, MR (and the solo of SA, SB)
    }
  }
  return false;
}

}  // namespace

bool mixer_mute(ActionContext& ctx, core::ActionArg) noexcept { return flag_at_cursor(ctx, false); }
bool mixer_solo(ActionContext& ctx, core::ActionArg) noexcept { return flag_at_cursor(ctx, true); }

namespace {

// OS-SHIFTROW: every strip's mute (or solo) off, written as parameters inside the one undo unit the dispatcher opened for the action.
bool clear_flags(ActionContext& ctx, bool solo) noexcept {
  if (ctx.view.screen != ScreenId::Mixer) {
    return false;
  }
  bool changed = false;
  for (unsigned strip = 0; strip < kMixTracks; ++strip) {
    const WriteResult r = write_param(ctx, core::param_id(core::ParamKind::Track, strip, solo ? tp::Solo : tp::Mute), 0.0f);
    if (r == WriteResult::Changed) {
      changed = true;
    } else if (r == WriteResult::NoProject) {
      return say_not_written(ctx, r);
    }  // (a strip the project has no track for is refused and left as it is)
  }
  return changed;
}

}  // namespace

// Shift + F1 NAME (OS-NAMES): the keyboard on the name of the track the cursor is on: a track's column (MIXER, STEMS) or row (ROUTING). A bus, the master or a page with no track under the cursor says
// TRACKS ONLY and opens nothing; a STEMS column of a track the project does not have says NO TRACK.
bool mixer_name(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen != ScreenId::Mixer) {
    return false;
  }
  const mixer::Cursor c = mixer::cursor(ctx.model, ctx.view);
  const unsigned tracks = mixer::tracks(ctx.model);
  unsigned track = tracks;  // (none)
  switch (mixer::page_of(ctx.view)) {
    case MixerPage::Mixer:
    case MixerPage::Stems: track = c.col; break;
    case MixerPage::Routing: track = c.row; break;
    case MixerPage::MasterFx: break;
  }
  const model::TrackSlot* slot = track < tracks && ctx.model.project != nullptr ? ctx.model.project->track(track) : nullptr;
  if (track >= tracks || (ctx.model.project != nullptr && slot == nullptr)) {
    if (mixer::page_of(ctx.view) == MixerPage::Stems && track >= tracks && track < kStemSources - 4u) {  // (the last four columns are ML, MR, SA, SB)
      return ctx.view.notify.post(core::Severity::Info, "NO TRACK", "mixer", ViewState::kMessageMs);  // a STEMS column T(n + 1)..T12 of a project with fewer tracks (no refusal convention on this page: the shortest neutral word)
    }
    return ctx.view.notify.post(core::Severity::Info, "TRACKS ONLY", "mixer", ViewState::kMessageMs);
  }
  return name_open(ctx, TextEntryView::Target::Track, track, slot != nullptr ? slot->name : "", model::kTrackNameMax);
}

bool mixer_mute_none(ActionContext& ctx, core::ActionArg) noexcept { return clear_flags(ctx, false); }
bool mixer_solo_none(ActionContext& ctx, core::ActionArg) noexcept { return clear_flags(ctx, true); }

// F1..F4: a page (arg = MixerPage); the cursor goes to the first row of it (what a row is depends on the page).
bool mixer_page(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.screen != ScreenId::Mixer || arg < 0 || static_cast<unsigned>(arg) >= kMixerScreen.pages()) {
    return false;
  }
  std::uint8_t& page = ctx.view.page[screen_index(ScreenId::Mixer)];
  if (page == arg) {
    return false;
  }
  page = static_cast<std::uint8_t>(arg);
  ctx.view.mixer.row = 0;
  return true;
}

// F6 APPLY on the STEMS page (OS-USBOUTS): the pending OUTPUTS value goes to the USB driver. The port re-enumerates (the computer drops the device for a moment), so the key asks first and the same
// key again within kResetWindowMs does it (the pattern of Config's F7 APPLY). A refusal (the driver is busy, DISK) keeps the pending value and says so. Elsewhere, or with nothing pending, nothing happens.
bool mixer_apply(ActionContext& ctx, core::ActionArg) noexcept {
  MixBoard* mix = ctx.mix;
  MixerView& mv = ctx.view.mixer;
  if (ctx.view.screen != ScreenId::Mixer || mixer::page_of(ctx.view) != MixerPage::Stems) {
    return false;  // (F6 is unlabelled on the other pages)
  }
  if (mix == nullptr || mix->request_outputs == nullptr || mv.outputs_pending == 0u) {
    return ctx.view.notify.post(core::Severity::Info, "NOTHING TO APPLY", "mixer", ViewState::kMessageMs);  // labelled APPLY here, so it says why nothing happens (the qanet key rule)
  }
  if (mv.reset_asked != 3u || core::tick_elapsed(ctx.now, mv.reset_at) > core::ticks_from_ms(MixerView::kResetWindowMs)) {
    mv.reset_asked = 3;
    mv.reset_at = ctx.now;
    return ctx.view.notify.ask(core::Severity::Warn, "USB WILL RECONNECT: PRESS AGAIN", "mixer", ViewState::kMessageMs);
  }
  mv.reset_asked = 0;
  if (!mix->request_outputs(mixer::outputs_profile(mv.outputs_pending))) {
    return ctx.view.notify.post(core::Severity::Warn, "USB BUSY: TRY AGAIN", "mixer", ViewState::kMessageMs);
  }
  mv.outputs_pending = 0;
  return true;
}

// F5 CUE LVL: the CUE level, on the MASTER FX page.
bool mixer_cue(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen != ScreenId::Mixer || kMixerScreen.pages() <= static_cast<unsigned>(MixerPage::MasterFx)) {
    return false;
  }
  std::uint8_t& page = ctx.view.page[screen_index(ScreenId::Mixer)];
  if (page == static_cast<std::uint8_t>(MixerPage::MasterFx) && ctx.view.mixer.row == kMasterCueRow) {
    return false;
  }
  page = static_cast<std::uint8_t>(MixerPage::MasterFx);
  ctx.view.mixer.row = static_cast<std::uint8_t>(kMasterCueRow);
  ctx.view.mixer.col = 0;
  return true;
}

}  // namespace o3os::app
