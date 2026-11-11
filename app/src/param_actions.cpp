// SPDX-License-Identifier: GPL-3.0-or-later
// Parameters, the generated parameter page and MIDI Learn (OS-002d, ADR 0009 rev 1 mechanism 5). Every function here works on a
// ParamId and its descriptor (model/params.h): nothing names a particular parameter, so a new row of the tables is editable, learnable,
// undoable and sent to the audio side without a line in this file.
#include "param_access.h"

#include <o3os/app/cc_map.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/lane_record.h>
#include <o3os/app/learn_list.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/param_set.h>
#include <o3os/app/play_mode.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/config_screen.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/text_format.h>
#include <o3os/app/wave_math.h>
#include <o3os/audio/voices.h>
#include <o3os/core/fixed_string.h>
#include <o3os/model/instrument.h>
#include <o3os/model/param_auto.h>
#include <o3os/model/params.h>
#include <o3os/seq/sequencer.h>

#include <cstring>
#include <initializer_list>

namespace o3os::app {

// The Learn store helpers (declared in param_access.h): the Learn dialog here and LEARN MAP (learn_map_actions.cpp) write the stores through the same ones.
core::LearnScope scope_of(core::ParamId id) noexcept { return core::learn_scope(core::param_kind(id)); }
unsigned scope_index(core::LearnScope s) noexcept { return s == core::LearnScope::Device ? 1u : 0u; }

const core::LearnBinding* store_slots(const ActionContext& ctx, core::LearnScope scope) noexcept {
  if (scope == core::LearnScope::Device) {
    return ctx.model.device.learn_slots();
  }
  return ctx.model.project != nullptr ? ctx.model.project->settings().learn_slots() : nullptr;
}

// Writes one slot of a store: the device's block directly, the project's through the editor (so it is part of the unit being made).
model::Status store_set(ActionContext& ctx, core::LearnScope scope, unsigned slot, const core::LearnBinding& b) noexcept {
  if (scope == core::LearnScope::Device) {
    return ctx.model.device.set_learn(slot, b);
  }
  return ctx.edit != nullptr ? ctx.edit->set_learn_slot(slot, b) : model::Status::BadIndex;
}

void reset_runtime(ActionContext& ctx, core::LearnScope scope, unsigned slot) noexcept {
  if (ctx.session != nullptr && slot < core::kLearnSlots) {
    ctx.session->learn[scope_index(scope)][slot] = core::LearnRuntime{};
  }
}

LearnPut learn_put(ActionContext& ctx, const core::LearnBinding& b) noexcept {
  const core::LearnScope scope = scope_of(b.param);
  const core::LearnBinding* slots = store_slots(ctx, scope);
  if (slots == nullptr) {
    return LearnPut::Refused;
  }
  core::LearnBinding plan_slots[core::kLearnSlots];
  std::memcpy(plan_slots, slots, sizeof plan_slots);
  core::LearnPlan plan{0, 0};
  if (!core::learn_plan(plan_slots, core::kLearnSlots, b, plan)) {
    return LearnPut::Full;
  }
  const std::uint32_t gone = plan.clear;  // the bindings this one takes the message from
  for (unsigned s = 0; s < core::kLearnSlots; ++s) {
    if ((gone & (std::uint32_t{1} << s)) != 0 && static_cast<int>(s) != plan.write) {
      (void)store_set(ctx, scope, s, core::LearnBinding{});
      reset_runtime(ctx, scope, s);
    }
  }
  if (store_set(ctx, scope, static_cast<unsigned>(plan.write), b) != model::Status::Ok) {
    return LearnPut::Refused;
  }
  reset_runtime(ctx, scope, static_cast<unsigned>(plan.write));
  return LearnPut::Done;
}

namespace {

constexpr int kMaxDetents = 1 << 16;  // a turn larger than this is clamped before it is multiplied (an encoder fed from data)

bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return say_tag(ctx, severity, text, "params");
}

// The CARD AS DISK row's messages (OS-USBDISK U2b). While the card is out the platform keeps a persistent strip up and rewrites it every second, so a post() would queue behind it and show only after the eject:
// ask() puts the message ahead of the Info news (the strip is dropped and comes back behind it), and an answer replaces the question of the same source. Questions and refusals alike.
bool say_disk(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return ctx.view.notify.ask(severity, text, "diskrow", ViewState::kMessageMs);
}

// "CC 74 > CUTOFF" (what a binding did), "UNBOUND CUTOFF".
bool say_binding(ActionContext& ctx, const char* verb, unsigned cc, const char* name, bool with_cc) noexcept {
  core::FixedString<core::kNotifyTextChars> text;
  text.assign(verb);
  if (with_cc) {
    append_uint(text, cc);
    text.append(" > ");
  }
  text.append(name);
  return ctx.view.notify.post(core::Severity::Info, text.c_str(), "learn", ViewState::kMessageMs);
}

// The edit of a parameter: `detents` fine steps (each coarse one is the descriptor's `coarse` of them), or its default.
bool edit_param(ActionContext& ctx, core::ParamId id, int detents, bool coarse, bool reset) noexcept {
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(ctx.model, id, scratch);
  if (id == core::ParamId::None || d == nullptr) {
    return false;
  }
  float now = 0.0f;
  if (!read_param(ctx.model, id, now)) {
    return say_not_written(ctx, ctx.model.project == nullptr ? WriteResult::NoProject : WriteResult::NoInstrument);
  }
  const bool deferred = (d->flags & core::kParamApply) != 0;  // OS-USBAUDIO: press to apply: the turn moves the pending value, the setting waits for param.apply
  const float live = now;
  if (deferred) {
    const PendingParam* held = ctx.view.params.find_pending(id);
    now = held != nullptr ? held->value : now;
  }
  float next = d->def;
  if (!reset) {
    detents = detents > kMaxDetents ? kMaxDetents : (detents < -kMaxDetents ? -kMaxDetents : detents);
    next = core::param_step(*d, now, coarse ? detents * static_cast<int>(d->coarse) : detents);
  }
  if (deferred) {
    ParamView& pv = ctx.view.params;
    const bool redraw = next != now || pv.armed;  // the shown value moved, or the arming goes back
    if (next == live) {
      pv.clear_pending(id);  // back to what the unit does: nothing to apply
    } else if (!pv.set_pending(id, next)) {
      return say(ctx, core::Severity::Warn, "VALUE REFUSED");
    }
    return redraw;
  }
  if (!reset) {
    merge_click(ctx, id);  // a reset (Delete) is a unit of its own
  }
  const WriteResult r = write_param(ctx, id, next);
  return r == WriteResult::Changed || (r != WriteResult::Unchanged && say_not_written(ctx, r));
}

// OS-USBDISK U2b: the Config > USB page's rows that are not settings (config_screen.h).
ConfigRow special_row(const ActionContext& ctx) noexcept { return ctx.view.screen == ScreenId::Config ? config_row(ctx.view) : ConfigRow::Param; }

constexpr const char* kReadOnlyLater = "EJECT THE CARD TO CHANGE IT";
static_assert(sizeof("EJECT THE CARD TO CHANGE IT") - 1 <= core::kNotifyTextChars, "the READ ONLY refusal fits the strip");
constexpr const char* kChangingHands = "THE CARD IS CHANGING HANDS: WAIT";
static_assert(sizeof("THE CARD IS CHANGING HANDS: WAIT") - 1 <= core::kNotifyTextChars, "the hand-over refusal fits the strip");

// The same, for the parameter under the cursor. READ ONLY is a session choice (Q4, RAM only): right is On, left is Off, Delete puts the default (On) back. The computer's disk takes the choice at the share
// (disk_open) and keeps it until the eject: while the card is out, changing hands, or a share request waits for the platform, the edit is refused so the row never shows a policy the computer does not have.
bool edit_focus(ActionContext& ctx, int detents, bool coarse, bool reset) noexcept {
  if (special_row(ctx) == ConfigRow::ReadOnly) {
    ViewState::DiskView& d = ctx.view.disk;
    if (d.phase != ViewState::DiskView::Phase::Idle || d.request != ViewState::DiskView::Request::None) {
      // Out with nothing waiting: the card is the computer's, eject it first. Working, or a request that waits (a share, an eject): the hand-over is under way (the text of the row's own press)
      const bool out_idle = d.phase == ViewState::DiskView::Phase::Out && d.request == ViewState::DiskView::Request::None;
      return say_disk(ctx, core::Severity::Info, out_idle ? kReadOnlyLater : kChangingHands);
    }
    bool& read_only = d.read_only;
    const bool next = reset || detents > 0 ? true : (detents < 0 ? false : read_only);
    const bool changed = next != read_only;
    read_only = next;
    return changed;
  }
  return edit_param(ctx, focus_param(ctx), detents, coarse, reset);
}

}  // namespace

bool param_edit_id(ActionContext& ctx, core::ParamId id, int detents) noexcept { return edit_param(ctx, id, detents, false, false); }

void merge_click(ActionContext& ctx, core::ParamId id) noexcept {
  if (ctx.edit == nullptr || ctx.session == nullptr || core::param_kind(id) == core::ParamKind::Device) {
    return;
  }
  if (core::tick_elapsed(ctx.now, ctx.session->merge_at) > core::ticks_from_ms(kClickGroupMs)) {
    ctx.edit->merge_break();
  }
  ctx.session->merge_at = ctx.now;
  ctx.edit->merge_hint(id);
}

core::ParamId focus_param(const ActionContext& ctx) noexcept {
  const ScreenEntry* e = find_screen(screen_registry(), ctx.view.screen);
  return e != nullptr && e->screen != nullptr ? e->screen->focus_param(ctx.model, ctx.view) : core::ParamId::None;
}

bool read_param(const Model& model, core::ParamId id, float& value) noexcept {
  if (core::param_kind(id) == core::ParamKind::Device) {
    if (model::param_desc(id) == nullptr) {
      return false;
    }
    value = model.device.get(core::param_field(id));
    return true;
  }
  if (is_insert_param(id)) {  // OS-INSFX S5: an insert's parameter: the record's blob, or the module's default while `n` does not reach it
    ParamScratch scratch;
    const core::ParamDesc* d = describe_param(model, id, scratch);
    if (d == nullptr || model.project == nullptr) {
      return false;
    }
    const model::InsertRecord* rec = model.project->find_insert(core::param_index(id) / 2u, core::param_index(id) % 2u);
    if (rec == nullptr) {
      return false;
    }
    value = core::param_field(id) < rec->n ? rec->param[core::param_field(id)] : d->def;
    return true;
  }
  if (is_module_param(id)) {  // a module's parameter: the blob's value, or the module's default while the blob does not reach it (the first edit writes the whole blob)
    ParamScratch scratch;
    const core::ParamDesc* d = describe_param(model, id, scratch);
    if (d == nullptr) {
      return false;
    }
    value = model.project->has_param(id) ? model.project->get_param(id) : d->def;
    return true;
  }
  if (is_midi_channel_param(id)) {  // OS-INSTUI S8a: the MIDI page's CHANNEL is the instrument's own field (not a table row)
    const model::Instrument* in = model.project != nullptr ? model.project->instrument(core::param_index(id)) : nullptr;
    if (in == nullptr || in->kind != model::InstrumentKind::Midi) {
      return false;
    }
    value = static_cast<float>(in->midi_channel);
    return true;
  }
  if (is_play_stereo_param(id)) {  // OS-STEREO step 2: the source page's STEREO is the instrument's own byte (not a table row)
    const model::Instrument* in = model.project != nullptr ? model.project->instrument(core::param_index(id)) : nullptr;
    if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
      return false;
    }
    value = static_cast<float>(in->stereo);
    return true;
  }
  if (is_wt_param(id)) {  // OS-CEWTENV W7a: the MOD page's WT fields are the instrument's own bytes (not table rows)
    const model::Instrument* in = model.project != nullptr ? model.project->instrument(core::param_index(id)) : nullptr;
    if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
      return false;
    }
    value = model::wt_param_value(*in, core::param_field(id));
    return true;
  }
  if (is_play_thru_param(id)) {  // OS-CEPLAYTHRU P2: the source page's PLAY THRU is the instrument's own byte (not a table row)
    const model::Instrument* in = model.project != nullptr ? model.project->instrument(core::param_index(id)) : nullptr;
    if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
      return false;
    }
    value = static_cast<float>(in->slice_thru);
    return true;
  }
  if (is_module_rate_param(id)) {  // OS-MODRATE: the params page's RATE row is the Module instrument's own byte
    const model::Instrument* in = model.project != nullptr ? model.project->instrument(core::param_index(id)) : nullptr;
    if (in == nullptr || in->kind != model::InstrumentKind::Module) {
      return false;
    }
    value = static_cast<float>(in->rate);
    return true;
  }
  if (model.project == nullptr || !model.project->has_param(id)) {
    return false;
  }
  value = model.project->get_param(id);
  if (core::param_kind(id) == core::ParamKind::Instrument && core::param_field(id) == model::instrument_param::WtSize) {
    // OS-INSTUI S3: the window size as the instrument plays it (an unset 0 is 2048, held to the sample), which is where the encoder's step starts from: source_param steps it by counters
    const std::uint32_t frames = model.samples != nullptr ? model.samples->pcm(core::param_index(id)).frames : 0u;
    const unsigned fit = wt_engine_size(static_cast<unsigned>(value), frames);  // (S6: a stored valid size is shown as it is, even where the sample cannot hold it: the page says it plays nothing)
    value = static_cast<float>(fit != 0u ? fit : wt_size_eff(static_cast<unsigned>(value)));
  } else if (core::param_kind(id) == core::ParamKind::Instrument && core::param_field(id) == model::instrument_param::GrainLength && value < 44.0f) {
    value = 441.0f;  // an unset grain plays 441 (og_samples.cpp): the encoder steps from there
  } else if (core::param_kind(id) == core::ParamKind::Instrument && model::instrument_param::is_mod_field(core::param_field(id)) &&
             (core::param_field(id) - model::instrument_param::kModFirst) % model::instrument_param::kModStride == model::instrument_param::LfoShape &&
             value > static_cast<float>(model::kLfoShapeSine)) {
    value = static_cast<float>(model::kLfoShapeTriangle);  // OS-CEWTENV W7a: the byte of a slot that was a WT LFO is a contour; as an LFO it plays (and reads) as a Triangle
  }
  return true;
}

std::uint32_t param_generation_of(const Model& model, core::ParamId id) noexcept {
  if (core::param_kind(id) == core::ParamKind::Device) {
    return model.device.generation(core::param_field(id));
  }
  return model.project != nullptr ? model.project->param_generation(id) : 0u;
}

void send_param_change(const ActionContext& ctx, core::ParamId id, float value, std::uint32_t ramp) noexcept {
  if (is_insert_param(id)) {  // OS-INSFX S5: an insert's parameter goes to its instance through the host (ParamChange{Insert, where, field}, with the binding's generation)
    const unsigned where = core::param_index(id);
    if (ctx.model.modules == nullptr || !ctx.model.modules->send_param(ParamTarget::of_insert(where), core::param_field(id), value)) {
      // A drop only when no instance is bound there: a bound instance already holds the value in its host blob, flagged dirty, and flush_params retries it on a later service tick once the ring has room.
      if (ctx.session != nullptr && (ctx.model.modules == nullptr || !ctx.model.modules->insert_bound(where))) {
        ++ctx.session->param_drops;  // no instance bound: nothing holds the value
      }
    }
    return;
  }
  if (is_module_param(id)) {  // a module's parameter goes to its instance (the module's own ring, applied at the next block start), not to the engine's table
    if (ctx.model.modules == nullptr || !ctx.model.modules->send_param(core::param_index(id), core::param_field(id) - model::instrument_param::kModuleFirst, value)) {
      if (ctx.session != nullptr) {
        ++ctx.session->param_drops;  // no instance (muted) or the ring is full: the module catches up at the next change; a rebind starts from the blob
      }
    }
    return;
  }
  if (core::param_kind(id) == core::ParamKind::Instrument && model::instrument_param::is_midi_row(core::param_field(id))) {
    // OS-INSTUI S8b + S9: the MIDI instrument's fields and VEL> are read where they are used (the sequencer and the MIDI link read the project when a note is made, the MIDI keyboard's velocity is scaled by
    // the app), and the engine's table has no such field. Only PROGRAM is told to the render: the receiver is put on the new program now (seq_param::MidiProgram, the slot as the value).
    if (core::param_field(id) == model::instrument_param::MidiProgram && ctx.params != nullptr &&
        !ctx.params->push(core::ParamChange{core::param_id(core::ParamKind::Project, 0, seq::seq_param::MidiProgram), static_cast<float>(core::param_index(id)), 0u}) && ctx.session != nullptr) {
      ++ctx.session->param_drops;
    }
    return;
  }
  if (core::param_kind(id) == core::ParamKind::Instrument && model::instrument_param::is_source_field(core::param_field(id))) {
    // OS-INSTUI S3: a field of the source page is not one the engine's table takes (the descriptor is rebuilt from the project: the slot is rebound, when stopped and idle); the two positions are also
    // live edits of a sounding wavetable / granular voice (voices.h WtWindow, GranPos), sent only to an instrument that is one.
    const unsigned slot = core::param_index(id);
    const unsigned field = core::param_field(id);
    if (ctx.samples != nullptr) {
      ctx.samples->request_rebind(slot);
    }
    const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(slot) : nullptr;
    const bool wt = field == model::instrument_param::WtPos && in != nullptr && in->sampler.play_mode == kPlayModeWavetable;
    const bool gr = field == model::instrument_param::GrainPos && in != nullptr && in->sampler.play_mode == kPlayModeGranular;
    if ((wt || gr) && ctx.params != nullptr &&
        !ctx.params->push(core::ParamChange{core::param_id(core::ParamKind::Instrument, slot, wt ? audio::inst_param::WtWindow : audio::inst_param::GranPos), value, 0u}) && ctx.session != nullptr) {
      ++ctx.session->param_drops;
    }
    return;
  }
  if (core::param_kind(id) == core::ParamKind::Instrument && model::instrument_param::is_mod_field(core::param_field(id))) {
    // OS-INSTUI S6, the MOD page, and OS-LFOLIVE: the SOURCE (Off / Envelope / LFO, the enable and loop bytes) and the LFO's shape, speed and amount are LIVE: the same id goes to the engine's ring
    // (audio::inst_param::lfo, equal to model::instrument_param::mod: audio_model_ids_test), and a sounding voice has them within a block (voices.h LIVE PARAMETERS, OS-LFOLIVE). The slot is also
    // rebound when stopped and idle (the handover), which heals a dropped ring entry: the rebuild from the project is what the live edits made. The descriptor holds one amount for the slot's two
    // generators, so a SOURCE edit also sends what the new generator reads: the LFO's shape, speed and amount, or, for Off and Envelope, the ADSR's amount (the amp slot keeps its own).
    namespace ip = model::instrument_param;
    const unsigned slot = core::param_index(id);
    const unsigned rel = core::param_field(id) - ip::kModFirst;
    const unsigned e = rel / ip::kModStride;
    if (ctx.samples != nullptr) {
      ctx.samples->request_rebind(slot);
    }
    const auto push = [&](unsigned field, float v) {
      if (ctx.params != nullptr && !ctx.params->push(core::ParamChange{core::param_id(core::ParamKind::Instrument, slot, field), v, ramp}) && ctx.session != nullptr) {
        ++ctx.session->param_drops;
      }
    };
    const unsigned mf = rel % ip::kModStride;
    if (value >= 2.5f && mf == ip::ModSource) {
      return;  // OS-CEWTENV W7a: WT ENV / WT LFO are not live (the engine's ring knows Off, Envelope and LFO): the rebind above applies them at the quiet moment
    }
    push(core::param_field(id), value);
    const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(slot) : nullptr;
    if (rel % ip::kModStride == ip::ModSource && in != nullptr && e < ip::kEnvelopes) {
      if (value >= 1.5f) {
        const model::Lfo& l = in->sampler.lfo[e];
        push(ip::mod(e, ip::LfoShape), static_cast<float>(l.shape));
        push(ip::mod(e, ip::LfoSpeed), static_cast<float>(l.speed));
        push(ip::mod(e, ip::LfoAmount), l.amount);
      } else if (e != 0u) {
        push(ip::envelope(e, ip::EnvAmount), in->sampler.envelope[e].amount);
      }
    }
    return;
  }
  if (ctx.samples != nullptr && core::param_kind(id) == core::ParamKind::Instrument && sample_desc_reads(core::param_field(id))) {
    ctx.samples->request_rebind(core::param_index(id));  // OS-LIVEP: the engine's descriptor of the slot is built from these fields; the bank applies the edit when stopped and idle, whatever the ring does
  }
  if (ctx.params != nullptr && !ctx.params->push(core::ParamChange{id, value, ramp}) && ctx.session != nullptr) {
    ++ctx.session->param_drops;  // the ring is full: the engine drains 64 a block; absolute values, so the next change of it catches up
  }
}

namespace {

// A module's parameter (OS-012b): clamped to the module's descriptor (rounded when it says whole numbers), the instrument's blob extended to hold it when the
// project has a short one (the module's defaults fill the gap, one undo unit with the edit), then the Project's own set_param.
WriteResult write_module_param(ActionContext& ctx, core::ParamId id, float value) noexcept {
  if (ctx.edit == nullptr) {
    return WriteResult::NoProject;
  }
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(ctx.model, id, scratch);
  const unsigned slot = core::param_index(id);
  const unsigned index = core::param_field(id) - model::instrument_param::kModuleFirst;
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (d == nullptr || in == nullptr || in->kind != model::InstrumentKind::Module) {
    return WriteResult::NoInstrument;
  }
  const float v = fit_param_value(*d, scratch.whole, value);
  if (!ctx.edit->project().has_param(id)) {
    model::Instrument full = *in;
    const unsigned have = full.param_len / 4u;
    for (unsigned i = have; i <= index && i < O3M_PARAMS_MAX; ++i) {
      ParamScratch s2;
      float def = 0.0f;
      if (ctx.model.modules != nullptr && ctx.model.modules->describe(*ctx.model.project, slot, i, s2)) {
        def = s2.desc.def;
      }
      std::memcpy(full.params + 4u * i, &def, 4);
    }
    full.param_len = static_cast<std::uint16_t>(4u * (index + 1u) > full.param_len ? 4u * (index + 1u) : full.param_len);
    if (ctx.edit->set_instrument(slot, full) != model::Status::Ok) {
      return WriteResult::Rejected;
    }
  }
  const float before = ctx.edit->project().get_param(id);
  if (ctx.edit->set_param(id, v) != model::Status::Ok) {
    return WriteResult::Rejected;
  }
  const float after = ctx.edit->project().get_param(id);
  if (after == before) {
    return WriteResult::Unchanged;
  }
  send_param_change(ctx, id, after);
  return WriteResult::Changed;
}

}  // namespace

namespace {

// OS-INSFX S5, an insert's parameter (ParamKind::Insert): clamped to the module's descriptor (rounded when it says whole numbers), written into the project's INSX blob through the
// Editor (Editor::set_insert: NOT an undo step, D9), then sent to the bound instance (ParamChange{Insert}). A record whose blob is shorter than the parameter is extended first (the
// module's defaults fill the gap), like a module instrument's.
WriteResult write_insert_param(ActionContext& ctx, core::ParamId id, float value) noexcept {
  if (ctx.edit == nullptr) {
    return WriteResult::NoProject;
  }
  const unsigned where = core::param_index(id);
  const unsigned index = core::param_field(id);
  const model::Project& p = ctx.edit->project();
  const model::InsertRecord* rec = p.find_insert(where / 2u, where % 2u);
  if (rec == nullptr) {
    return WriteResult::NoInstrument;  // (the page is open only on a bound cell)
  }
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(ctx.model, id, scratch);
  if (d == nullptr || index >= model::kInsertParamsMax) {
    return WriteResult::NoInstrument;
  }
  const float v = fit_param_value(*d, scratch.whole, value);
  model::InsertRecord next = *rec;
  if (index >= next.n) {  // the blob is extended to hold the parameter: the module's defaults fill the gap
    if (ctx.model.modules == nullptr || ctx.model.project == nullptr) {
      return WriteResult::Rejected;
    }
    const ParamTarget target = ParamTarget::of_insert(where);
    for (unsigned i = next.n; i <= index; ++i) {
      ParamScratch s2;
      float def = 0.0f;
      if (ctx.model.modules->describe(p, target, i, s2)) {
        def = s2.desc.def;
      }
      next.param[i] = def;
    }
    next.n = static_cast<std::uint8_t>(index + 1u);
  }
  if (next.param[index] == v) {
    return WriteResult::Unchanged;  // before the extended blob (its n) is stored: safe, the read path falls back to the module default past n
  }
  next.param[index] = v;
  if (ctx.edit->set_insert(next) != model::Status::Ok) {  // not journalled (D9): no undo entry, the generation moves so a save keeps it
    return WriteResult::Rejected;
  }
  send_param_change(ctx, id, v);
  return WriteResult::Changed;
}

}  // namespace

namespace {

// The PLAY ENGINE row (OS-FU5): the engine action's rules (a sample instrument, an Amiga its rate can play, the slot rebound), a step of the encoder moving past a refused Amiga.
WriteResult write_play_engine(ActionContext& ctx, core::ParamId id, float value) noexcept {
  if (ctx.edit == nullptr) {
    return WriteResult::NoProject;
  }
  const unsigned slot = core::param_index(id);
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (in == nullptr) {
    return WriteResult::NoInstrument;
  }
  if (!(value >= 0.0f && value <= static_cast<float>(model::kInstEngineMax))) {
    return WriteResult::Rejected;
  }
  const unsigned target = static_cast<unsigned>(value + 0.5f);
  const int dir = target > in->engine ? 1 : (target < in->engine ? -1 : 0);
  return dir != 0 && play_engine_set(ctx, slot, target, dir) ? WriteResult::Changed : WriteResult::Unchanged;  // (what was refused or skipped has been said)
}

}  // namespace

namespace {

// A field of the source page (OS-INSTUI S3), held to what the sample in the slot allows: the play mode and the window size go through their validating actions, SLICES is
// a readout, the positions, the grain length and the selected slice are clamped. True when the edit is done (`out` is the answer); false: carry on with `value` as it is now.
bool source_param(ActionContext& ctx, core::ParamId id, float& value, WriteResult& out) noexcept {
  namespace ip = model::instrument_param;
  if (ctx.edit == nullptr) {
    out = WriteResult::NoProject;
    return true;
  }
  const unsigned slot = core::param_index(id);
  const unsigned field = core::param_field(id);
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    out = WriteResult::NoInstrument;
    return true;
  }
  if (!(value >= 0.0f && value <= 65535.0f)) {
    out = WriteResult::Rejected;  // NaN, negative, or past the largest of the rows
    return true;
  }
  const std::uint32_t frames = ctx.model.samples != nullptr ? ctx.model.samples->pcm(slot).frames : 0u;
  const unsigned want = static_cast<unsigned>(value + 0.5f);
  const model::SamplerParams& s = in->sampler;
  switch (field) {
    case ip::PlayMode:
      out = play_mode_set(ctx, slot, want) ? WriteResult::Changed : WriteResult::Unchanged;
      return true;
    case ip::Slices:  // a count the slices must agree with: they are made (and removed) on Wave
      (void)say(ctx, core::Severity::Info, "SLICES ARE MADE ON WAVE: AUTO, EVEN, ADD, DEL");
      out = WriteResult::Unchanged;
      return true;
    case ip::WtSize: {
      const unsigned have = frames != 0u && wt_engine_size(s.wavetable_window_size, frames) != 0u ? wt_engine_size(s.wavetable_window_size, frames) : wt_size_eff(s.wavetable_window_size);
      WtWindow w{have, s.wavetable_window};
      const unsigned goal = want == 0u ? wt_fit_size(0, frames) : want;  // 0 is the default (Delete): the size an unset window plays with
      if (wt_size_valid(goal)) {
        for (unsigned i = 0; i < kWtMaxCounter && w.size != goal; ++i) {  // a size asked for: there in as many steps as it takes, the position moving with it
          const WtWindow next = wt_step_size(frames, w.size, w.pos, goal > w.size ? 1 : -1);
          if (next.size == w.size) {
            break;  // the sample holds no larger window
          }
          w = next;
        }
      } else if (want != 0u && want != have) {  // a step of the encoder (the value it asks for is none of the sizes): a size per detent up or down
        const int dir = want > have ? 1 : -1;
        const unsigned detents = dir > 0 ? want - have : have - want;
        for (unsigned i = 0; i < detents && i < kWtMaxCounter - kWtMinCounter; ++i) {
          const WtWindow next = wt_step_size(frames, w.size, w.pos, dir);
          if (next.size == w.size) {
            break;
          }
          w = next;
        }
      }
      if (w.size == have && w.pos == s.wavetable_window && s.wavetable_window_size == have) {
        out = WriteResult::Unchanged;
        return true;
      }
      model::Instrument edited = *in;
      edited.sampler.wavetable_window_size = static_cast<std::uint16_t>(w.size);
      edited.sampler.wavetable_window = w.pos;
      if (ctx.edit->set_instrument(slot, edited) != model::Status::Ok) {
        out = WriteResult::Rejected;
        return true;
      }
      if (ctx.samples != nullptr) {
        ctx.samples->request_rebind(slot);
      }
      out = WriteResult::Changed;
      return true;
    }
    case ip::WtPos:
      value = static_cast<float>(wt_clamp_pos(frames, wt_size_eff(s.wavetable_window_size), want));
      return false;
    case ip::GrainLength: {
      const float top = frames != 0u && frames < 44100u ? static_cast<float>(frames) : 44100.0f;
      value = want == 0u ? 0.0f : (value > top ? top : (value < 44.0f ? 44.0f : value));  // 0 is "not set" (Delete); otherwise 44 .. min(length, 44100)
      if (top < 44.0f && want != 0u) {
        value = top;  // a sample shorter than the floor: the grain is the sample
      }
      return false;
    }
    case ip::Slice:
      value = static_cast<float>(want >= s.slice_count ? (s.slice_count != 0u ? s.slice_count - 1u : 0u) : want);
      return false;
    default:
      return false;
  }
}

}  // namespace

namespace {

// OS-INSTUI S8a, the MIDI page's CHANNEL: the instrument is written whole with the new channel, one undo record (as NAME; the editor records nothing for the same instrument). Nothing is sent to the
// audio side: the notes already sounding end on the channel of their own note-on (the sequencer keeps `sent_ch`, a pad's note `live_pad_`), the next note-on reads the new one.
WriteResult write_midi_channel(ActionContext& ctx, core::ParamId id, float value) noexcept {
  if (ctx.edit == nullptr) {
    return WriteResult::NoProject;
  }
  const model::Instrument* in = ctx.edit->project().instrument(core::param_index(id));
  if (in == nullptr || in->kind != model::InstrumentKind::Midi) {
    return WriteResult::NoInstrument;
  }
  if (!(value >= 0.5f && value < 16.5f)) {
    return WriteResult::Rejected;  // (the descriptor's range is 1..16: a NaN or an out-of-range value is not written)
  }
  const std::uint8_t channel = static_cast<std::uint8_t>(value + 0.5f);  // 1..16
  if (channel == in->midi_channel) {
    return WriteResult::Unchanged;
  }
  model::Instrument next = *in;
  next.midi_channel = channel;
  return ctx.edit->set_instrument(core::param_index(id), next) == model::Status::Ok ? WriteResult::Changed : WriteResult::Rejected;
}

// OS-STEREO step 2, the source page's STEREO row: the instrument is written whole with the flag, one undo record. MONO is always allowed; STEREO only while the bank holds a side signal for the slot (decision S4
// of docs/specs/OS-STEREO.md: the flag is the instrument's wish, S the sample's), else it is refused on the strip. Nothing is sent to the audio side (no audio change before slice (c)).
constexpr const char* kStereoNeedsLoad = "STEREO NEEDS A STEREO LOAD";
static_assert(sizeof("STEREO NEEDS A STEREO LOAD") - 1 <= core::kNotifyTextChars, "the STEREO refusal fits the strip");

WriteResult write_play_stereo(ActionContext& ctx, core::ParamId id, float value) noexcept {
  if (ctx.edit == nullptr) {
    return WriteResult::NoProject;
  }
  const unsigned slot = core::param_index(id);
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return WriteResult::NoInstrument;
  }
  if (!(value >= 0.0f && value <= static_cast<float>(model::kInstStereoMax))) {
    return WriteResult::Rejected;  // (NaN, negative or past the two choices)
  }
  const std::uint8_t target = static_cast<std::uint8_t>(value + 0.5f);
  if (target == in->stereo) {
    return WriteResult::Unchanged;
  }
  if (target != 0u && !(ctx.samples != nullptr && ctx.samples->has_side(slot))) {
    (void)say(ctx, core::Severity::Info, kStereoNeedsLoad);
    return WriteResult::Unchanged;  // (the refusal has been said)
  }
  model::Instrument next = *in;
  next.stereo = target;
  if (ctx.edit->set_instrument(slot, next) != model::Status::Ok) {
    return WriteResult::Rejected;
  }
  if (ctx.samples != nullptr) {
    ctx.samples->request_rebind(slot);  // OS-STEREO (c): the descriptor's S follows the flag (the bank applies it when stopped and idle, as for every descriptor field)
  }
  return WriteResult::Changed;
}

// OS-CEPLAYTHRU P2, the source page's PLAY THRU row (the slice modes only): the instrument is written whole with the flag, one undo record, and the slot is rebound, so the
// rebuilt descriptor carries it (beat_slice_span ends a slice at the sample end). Like write_play_stereo, no refusal: OFF and ON are always allowed.
WriteResult write_play_thru(ActionContext& ctx, core::ParamId id, float value) noexcept {
  if (ctx.edit == nullptr) {
    return WriteResult::NoProject;
  }
  const unsigned slot = core::param_index(id);
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return WriteResult::NoInstrument;
  }
  if (!(value >= 0.0f && value <= static_cast<float>(model::kInstSliceThruMax))) {
    return WriteResult::Rejected;  // (NaN, negative or past the two choices)
  }
  const std::uint8_t target = static_cast<std::uint8_t>(value + 0.5f);
  if (target == in->slice_thru) {
    return WriteResult::Unchanged;
  }
  model::Instrument next = *in;
  next.slice_thru = target;
  if (ctx.edit->set_instrument(slot, next) != model::Status::Ok) {
    return WriteResult::Rejected;
  }
  if (ctx.samples != nullptr) {
    ctx.samples->request_rebind(slot);  // the descriptor's slice_thru follows the flag (the bank applies it when stopped and idle, as for every descriptor field)
  }
  return WriteResult::Changed;
}

// OS-MODRATE, the params page's RATE row (a rate-capable instrument module): the instrument is written whole with the byte, one undo record (saved with the project, INST 1.8). ModuleUi starts the
// change (shutdown, init and the blob again at idle, in the main loop: sync_slots, here, by undo and redo, and once a pass); a module that does not declare the flag refuses a RATE above FULL.
WriteResult write_module_rate(ActionContext& ctx, core::ParamId id, float value) noexcept {
  if (ctx.edit == nullptr || ctx.model.modules == nullptr) {
    return WriteResult::NoProject;
  }
  const unsigned slot = core::param_index(id);
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (in == nullptr || in->kind != model::InstrumentKind::Module) {
    return WriteResult::NoInstrument;
  }
  if (!(value >= 0.0f && value <= static_cast<float>(model::kInstRateMax))) {
    return WriteResult::Rejected;  // (NaN, negative or past 1/4)
  }
  const std::uint8_t target = static_cast<std::uint8_t>(value + 0.5f);
  if (target == in->rate) {
    return WriteResult::Unchanged;
  }
  if (target != 0u && !ctx.model.modules->rate_capable(ctx.edit->project(), slot)) {
    return WriteResult::Rejected;
  }
  model::Instrument next = *in;
  next.rate = target;
  if (ctx.edit->set_instrument(slot, next) != model::Status::Ok) {
    return WriteResult::Rejected;
  }
  ctx.model.modules->sync_slots(ctx.edit->project());  // (starts the change now; the next passes finish it)
  return WriteResult::Changed;
}

// OS-CEWTENV W7a, the MOD page's WT fields (WAVE, DURATION, WARP X, WARP Y, SUSTAIN of a slot): the instrument is written whole with the changed byte, one undo record, and the slot is rebound, so the rebuilt
// descriptor carries it (nothing is sent live: a WT slot is built from the project, as the SOURCE choice itself). A value outside the descriptor's range (or NaN) is Rejected.
WriteResult write_wt_param(ActionContext& ctx, core::ParamId id, float value) noexcept {
  namespace ip = model::instrument_param;
  if (ctx.edit == nullptr) {
    return WriteResult::NoProject;
  }
  const unsigned slot = core::param_index(id);
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return WriteResult::NoInstrument;
  }
  const unsigned field = core::param_field(id);
  const core::ParamDesc* d = model::wt_param_desc(field);
  if (d == nullptr || !core::param_in_range(*d, value)) {
    return WriteResult::Rejected;
  }
  const int whole = static_cast<int>(value + (value < 0.0f ? -0.5f : 0.5f));
  if (model::wt_param_value(*in, field) == static_cast<float>(whole)) {
    return WriteResult::Unchanged;
  }
  const unsigned e = (field - ip::kWtFirst) / ip::kWtStride;
  model::Instrument next = *in;
  model::Lfo& l = next.sampler.lfo[e];
  switch ((field - ip::kWtFirst) % ip::kWtStride) {
    case ip::WtWave: l.shape = static_cast<std::uint8_t>(whole); break;
    case ip::WtDuration: next.sampler.envelope[e].decay = static_cast<std::uint16_t>(whole); break;
    case ip::WtWarpX: l.wt_bend = static_cast<std::uint8_t>((ip::wt_nibble_of(whole) << 4) | (l.wt_bend & 15u)); break;
    case ip::WtWarpY: l.wt_bend = static_cast<std::uint8_t>((l.wt_bend & 0xF0u) | ip::wt_nibble_of(whole)); break;
    default: l.wt_sust = static_cast<std::uint8_t>(whole); break;
  }
  if (ctx.edit->set_instrument(slot, next) != model::Status::Ok) {
    return WriteResult::Rejected;
  }
  if (ctx.samples != nullptr) {
    ctx.samples->request_rebind(slot);
  }
  return WriteResult::Changed;
}

}  // namespace

WriteResult write_param(ActionContext& ctx, core::ParamId id, float value, bool tell) noexcept {
  if (is_wt_param(id)) {
    return write_wt_param(ctx, id, value);
  }
  if (is_midi_channel_param(id)) {
    return write_midi_channel(ctx, id, value);
  }
  if (is_play_stereo_param(id)) {
    return write_play_stereo(ctx, id, value);
  }
  if (is_play_thru_param(id)) {
    return write_play_thru(ctx, id, value);
  }
  if (is_module_rate_param(id)) {
    return write_module_rate(ctx, id, value);
  }
  if (is_insert_param(id)) {  // OS-INSFX S5: an insert's parameter writes the INSX blob and goes to the instance
    return write_insert_param(ctx, id, value);
  }
  if (is_module_param(id)) {
    return write_module_param(ctx, id, value);
  }
  if (core::param_kind(id) == core::ParamKind::Instrument && model::instrument_param::is_source_field(core::param_field(id))) {
    WriteResult done = WriteResult::Unchanged;
    if (source_param(ctx, id, value, done)) {
      return done;
    }
  }
  if (core::param_kind(id) == core::ParamKind::Instrument && core::param_field(id) == model::instrument_param::PlayEngine) {
    return write_play_engine(ctx, id, value);
  }
  float before = 0.0f;
  float after = 0.0f;
  if (core::param_kind(id) == core::ParamKind::Device) {
    const unsigned field = core::param_field(id);
    before = ctx.model.device.get(field);
    if (ctx.model.device.set(field, value) != model::Status::Ok) {
      return WriteResult::Rejected;
    }
    after = ctx.model.device.get(field);
  } else {
    if (ctx.edit == nullptr) {
      return WriteResult::NoProject;
    }
    before = ctx.edit->project().get_param(id);
    const model::Status st = ctx.edit->set_param(id, value);
    if (st == model::Status::NotFound) {
      return WriteResult::NoInstrument;
    }
    if (st != model::Status::Ok) {
      return WriteResult::Rejected;
    }
    after = ctx.edit->project().get_param(id);
  }
  if (after == before) {
    return WriteResult::Unchanged;
  }
  if (tell) {
    send_param_change(ctx, id, after);
  }
  return WriteResult::Changed;
}

bool say_not_written(ActionContext& ctx, WriteResult result) noexcept {
  switch (result) {
    case WriteResult::NoProject: return say(ctx, core::Severity::Info, "NO PROJECT OPEN");
    case WriteResult::NoInstrument: return say(ctx, core::Severity::Info, "NO INSTRUMENT IN THIS SLOT");
    case WriteResult::Rejected: return say(ctx, core::Severity::Warn, "VALUE REFUSED");
    case WriteResult::Changed:
    case WriteResult::Unchanged:
      break;
  }
  return false;
}

bool param_edit(ActionContext& ctx, core::ActionArg arg) noexcept { return edit_focus(ctx, arg, false, false); }
bool param_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return edit_focus(ctx, arg, true, false); }
bool param_reset(ActionContext& ctx, core::ActionArg) noexcept { return edit_focus(ctx, 0, false, true); }

bool param_group(ActionContext& ctx, core::ActionArg arg) noexcept {
  const ScreenEntry* e = find_screen(screen_registry(), ctx.view.screen);
  if (e == nullptr || e->screen == nullptr || arg < 0 || static_cast<unsigned>(arg) >= e->screen->pages()) {
    return false;
  }
  std::uint8_t& page = ctx.view.page[screen_index(ctx.view.screen)];
  if (page == arg) {
    return false;
  }
  page = static_cast<std::uint8_t>(arg);
  ctx.view.params.row = 0;
  ctx.view.params.discard_pending();  // OS-USBAUDIO: leaving a page without applying throws the pending values away
  return true;
}

// Shift + F7 on Config (OS-SHIFTROW): every row of the page back to its default. Device settings have no undo, so the first press only asks ("SHIFT+F7 AGAIN: RESET PAGE") and a second within
// two seconds does it. Each row goes through the Delete path (param.reset): a row that re-enumerates USB only changes its pending value, APPLY writes it. The pages with no rows say so.
// The strip's one line of DEFAULTS: the second press's answer replaces the first's question (upsert by source) instead of waiting behind it for its two seconds.
namespace {
bool say_defaults(ActionContext& ctx, const char* text) noexcept {
  return ctx.view.notify.upsert(core::Severity::Info, text, "defaults", ViewState::kMessageMs);
}
}  // namespace

bool config_page_default(ActionContext& ctx, core::ActionArg) noexcept {
  const ScreenEntry* e = find_screen(screen_registry(), ctx.view.screen);
  if (e == nullptr || e->screen == nullptr || ctx.view.screen != ScreenId::Config) {
    return false;
  }
  const Screen& screen = *e->screen;
  ParamView& pv = ctx.view.params;
  const int rows = screen.count(ctx.model, ctx.view);
  if (rows <= 0) {
    pv.defaults_armed = false;
    return say_defaults(ctx, "NO ROWS ON THIS PAGE");
  }
  if (pv.defaults_armed && !defaults_ask_live(ctx.view)) {
    pv.defaults_armed = false;  // OS-BUG-CFGARM: its question is gone from the strip (another message took the head): this press asks again
  }
  if (!pv.defaults_armed || core::tick_elapsed(ctx.now, pv.defaults_at) > core::ticks_from_ms(ParamView::kDefaultsWindowMs)) {
    pv.defaults_armed = true;
    pv.defaults_at = ctx.now;
    AskText text;  // "SHIFT+F7 AGAIN: RESET PAGE", from the question's key table (OS-KEYROWDLG); the key row marks the key by the same text
    ask_text(Ask::Defaults, "", text);
    return ctx.view.notify.ask(core::Severity::Info, text.c_str(), "defaults", ViewState::kMessageMs);
  }
  pv.defaults_armed = false;
  const int keep = screen.position(ctx.view);
  bool changed = false;
  for (int row = 0; row < rows; ++row) {
    (void)screen.set_position(ctx.model, ctx.view, row);
    changed = edit_focus(ctx, 0, false, true) || changed;
  }
  (void)screen.set_position(ctx.model, ctx.view, keep);
  // A row that re-enumerates USB (kParamApply) only moved its pending value: nothing is applied until F7 says so.
  (void)say_defaults(ctx, pv.pending_n != 0 ? "PAGE RESET: F7 TO APPLY" : (changed ? "PAGE RESET TO DEFAULTS" : "PAGE ALREADY AT DEFAULTS"));
  return true;
}

// F7 on Config (OS-USBAUDIO): the pending values of the page become the settings, on a second press. The first says what will happen (the USB port re-enumerates, when it does) and arms; a turn, a move of
// the cursor or another page puts it back. The settings are written together, so the unit re-enumerates once (the main loop's USB syncs ask the driver for the whole layout).
// F7 on the CARD AS DISK row (OS-USBDISK U2b, docs/specs/OS-USBDISK.md 9): the same two presses as APPLY. In: the refusals first (they are checked on the first press, so the warning is not shown for nothing, and
// again by the platform on the second), then THE COMPUTER GETS THE CARD; the second press leaves the request (App::take_disk_request). Out: THE COMPUTER MAY STILL BE WRITING, then the eject. The platform's
// state machine does the rest; a refusal it makes on the second press comes back as its own strip text. The texts fit one strip line (core::kNotifyTextChars).
namespace {

constexpr const char* kShareAsk = "THE COMPUTER GETS THE CARD: SAVES STOP. F7 AGAIN";
constexpr const char* kShareAskAudio = "USB AUDIO PAUSES. F7 AGAIN TO SHARE THE CARD";
constexpr const char* kEjectAsk = "THE COMPUTER MAY STILL BE WRITING: F7 AGAIN";
static_assert(sizeof("THE COMPUTER GETS THE CARD: SAVES STOP. F7 AGAIN") - 1 <= core::kNotifyTextChars, "the share question fits the strip");

bool disk_row_press(ActionContext& ctx) noexcept {
  using Phase = ViewState::DiskView::Phase;
  ViewState::DiskView& d = ctx.view.disk;
  ParamView& pv = ctx.view.params;
  if (d.phase == Phase::Working) {
    pv.armed = false;
    return say_disk(ctx, core::Severity::Info, kChangingHands);
  }
  if (d.phase == Phase::Out) {
    if (!pv.armed) {
      pv.armed = true;
      return say_disk(ctx, core::Severity::Warn, kEjectAsk);
    }
    pv.armed = false;
    d.request = ViewState::DiskView::Request::Eject;
    return say_disk(ctx, core::Severity::Info, "EJECTING THE CARD");
  }
  if (ctx.model.card == nullptr || !ctx.model.card->mounted()) {
    pv.armed = false;
    return say_disk(ctx, core::Severity::Warn, "NO CARD");
  }
  if (d.claims_busy || ctx.view.xport.running) {
    pv.armed = false;
    return say_disk(ctx, core::Severity::Warn, "STOP THE EXPORT FIRST");
  }
  if (!pv.armed) {
    pv.armed = true;
    return say_disk(ctx, core::Severity::Info, d.audio ? kShareAskAudio : kShareAsk);
  }
  pv.armed = false;
  d.request = d.read_only ? ViewState::DiskView::Request::ShareReadOnly : ViewState::DiskView::Request::ShareWritable;  // the policy is taken here, not when the platform gets to it
  return say_disk(ctx, core::Severity::Info, "SHARING THE CARD");
}

}  // namespace

bool param_apply(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen == ScreenId::Config && !config_page_has_rows(ctx.view)) {
    ctx.view.params.armed = false;
    return false;  // OS-QAFIX-7: SD and INFO have no row to apply and their F7 shows no label: no silent answer for an unlabelled key
  }
  if (special_row(ctx) == ConfigRow::CardAsDisk) {
    return disk_row_press(ctx);
  }
  if (special_row(ctx) == ConfigRow::ReadOnly) {
    ctx.view.params.armed = false;
    return say_disk(ctx, core::Severity::Info, "TURN THE ENCODER TO CHOOSE");
  }
  ParamView& pv = ctx.view.params;
  if (pv.pending_n == 0) {
    pv.armed = false;
    return say(ctx, core::Severity::Info, "NOTHING TO APPLY");
  }
  if (pv.armed && !apply_ask_live(ctx.view)) {
    pv.armed = false;  // OS-BUG-CFGARM: the question is gone from the strip (its time, or another message): this press asks again, it does not apply
  }
  if (!pv.armed) {
    pv.armed = true;
    // A question: ahead of the Info news (a post() would queue behind it, and the platform's persistent disk strip keeps the head while the card is on the computer, so F7 would ask for ever). A Warn in front keeps its place for its own time only.
    (void)ctx.view.notify.ask(core::Severity::Info, pv.will_reenumerate(ctx.model.device) ? kApplyAskStripReconnect : kApplyAskStrip, "params", ViewState::kMessageMs);
    return true;
  }
  PendingParam held[ParamView::kMaxPending];
  const unsigned n = pv.pending_n;
  for (unsigned i = 0; i < n; ++i) {
    held[i] = pv.pending[i];
  }
  const bool audio = pv.audio_after(ctx.model.device);
  pv.discard_pending();
  bool reconnect = false;  // a write that took and re-enumerates (an Unchanged one does nothing)
  bool refused = false;
  for (unsigned i = 0; i < n; ++i) {
    const WriteResult r = write_param(ctx, held[i].id, held[i].value);
    reconnect = reconnect || (r == WriteResult::Changed && ParamView::reenumerates(held[i].id, audio));
    if (r != WriteResult::Changed && r != WriteResult::Unchanged) {
      refused = true;
      (void)say_not_written(ctx, r);
    }
  }
  if (reconnect || !refused) {  // (a refusal's own message stays when there is nothing to add)
    (void)say(ctx, core::Severity::Info, reconnect ? "USB RECONNECTING" : "APPLIED");
  }
  return true;
}

// ---- MIDI Learn -----------------------------------------------------------------------------------------------------------------

// ---- The list of bindings (learn_list.h) ----------------------------------------------------------------------------------------

namespace {

const core::LearnBinding* model_store(const Model& model, core::LearnScope scope) noexcept {
  if (scope == core::LearnScope::Device) {
    return model.device.learn_slots();
  }
  return model.project != nullptr ? model.project->settings().learn_slots() : nullptr;
}

}  // namespace

unsigned learn_entry_count(const Model& model) noexcept {
  unsigned n = 0;
  for (const core::LearnScope scope : {core::LearnScope::Project, core::LearnScope::Device}) {
    const core::LearnBinding* slots = model_store(model, scope);
    for (unsigned s = 0; slots != nullptr && s < core::kLearnSlots; ++s) {
      n += core::learn_empty(slots[s]) ? 0u : 1u;
    }
  }
  return n;
}

bool learn_entry_at(const Model& model, unsigned index, LearnEntry& out) noexcept {
  for (const core::LearnScope scope : {core::LearnScope::Project, core::LearnScope::Device}) {
    const core::LearnBinding* slots = model_store(model, scope);
    for (unsigned s = 0; slots != nullptr && s < core::kLearnSlots; ++s) {
      if (core::learn_empty(slots[s])) {
        continue;
      }
      if (index == 0) {
        out = LearnEntry{&slots[s], scope, s};
        return true;
      }
      --index;
    }
  }
  return false;
}

bool learn_clear(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.learn.confirm = false;
  const core::ParamId id = focus_param(ctx);
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(ctx.model, id, scratch);
  if (id == core::ParamId::None || d == nullptr) {
    return false;
  }
  const core::LearnScope scope = scope_of(id);
  const core::LearnBinding* slots = store_slots(ctx, scope);
  const int slot = slots != nullptr ? core::learn_find_param(slots, core::kLearnSlots, id) : -1;
  if (slot < 0) {
    return say(ctx, core::Severity::Info, "NOTHING BOUND TO THIS");
  }
  if (store_set(ctx, scope, static_cast<unsigned>(slot), core::LearnBinding{}) != model::Status::Ok) {
    return say_not_written(ctx, WriteResult::NoProject);
  }
  reset_runtime(ctx, scope, static_cast<unsigned>(slot));
  (void)say_binding(ctx, "UNBOUND ", 0, d->name, false);
  return true;
}

bool learn_mode(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.learn.confirm = false;
  const core::ParamId id = focus_param(ctx);
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(ctx.model, id, scratch);
  if (id == core::ParamId::None || d == nullptr) {
    return false;
  }
  const core::LearnScope scope = scope_of(id);
  const core::LearnBinding* slots = store_slots(ctx, scope);
  const int slot = slots != nullptr ? core::learn_find_param(slots, core::kLearnSlots, id) : -1;
  if (slot < 0) {
    return say(ctx, core::Severity::Info, "NOTHING BOUND TO THIS");
  }
  core::LearnBinding b = slots[slot];
  b.mode = static_cast<std::uint8_t>((b.mode + 1u) % core::kLearnModeCount);
  if (store_set(ctx, scope, static_cast<unsigned>(slot), b) != model::Status::Ok) {
    return say_not_written(ctx, WriteResult::NoProject);
  }
  reset_runtime(ctx, scope, static_cast<unsigned>(slot));
  return true;
}

// OS-LEARNFOLLOW 3.1, F5 in the Learn overlay: toggle the target's binding between "this instrument" and "the selected instrument".
bool learn_follow_of(ActionContext& ctx, core::ParamId id) noexcept {
  if (id == core::ParamId::None) {
    return false;
  }
  if (core::param_kind(id) != core::ParamKind::Instrument) {
    return say(ctx, core::Severity::Info, "FOLLOW: INSTRUMENT PARAMETERS ONLY");
  }
  if (model::instrument_param::is_module_field(core::param_field(id))) {
    return say(ctx, core::Severity::Info, "FOLLOW: NOT FOR A MODULE PARAMETER");  // A-4: parameter i means something else in every module
  }
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(ctx.model, id, scratch);
  if (d == nullptr) {
    return false;
  }
  const core::LearnScope scope = scope_of(id);  // an Instrument parameter is the project's
  const core::LearnBinding* slots = store_slots(ctx, scope);
  const int slot = slots != nullptr ? core::learn_find_param(slots, core::kLearnSlots, id) : -1;
  if (slot < 0) {
    return say(ctx, core::Severity::Info, "NOTHING BOUND TO THIS");
  }
  const core::LearnBinding old = slots[slot];
  core::LearnBinding b = old;
  const bool to_follow = (old.flags & core::kLearnFollow) == 0;
  if (to_follow) {  // the field of whichever instrument is selected (index 0: only its kind and field count)
    b.param = core::param_id(core::ParamKind::Instrument, 0, core::param_field(id));
    b.flags = core::kLearnFollow;
  } else {  // pinned back to the target's instrument
    b.param = id;
    b.flags = 0;
  }
  const LearnPut put = learn_put(ctx, b);
  if (put != LearnPut::Done) {
    return say(ctx, core::Severity::Warn, put == LearnPut::Full ? "LEARN STORE FULL" : "BINDING REFUSED");
  }
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("CC ");
  append_uint(text, b.cc);
  text.append(to_follow ? " FOLLOWS: " : " PINNED: ");
  text.append(d->name);
  if (!to_follow) {
    text.append(" INS ");
    append_dec(text, core::param_index(id) + 1u, 2);
  }
  (void)ctx.view.notify.post(core::Severity::Info, text.c_str(), "learn", ViewState::kMessageMs);
  return true;
}

bool learn_follow(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.learn.confirm = false;
  return learn_follow_of(ctx, focus_param(ctx));
}

// F3 in the overlay: deletes the binding the list cursor is on (either store; a project binding is an undo unit).
bool learn_delete(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.learn.confirm = false;
  LearnEntry e{};
  if (!learn_entry_at(ctx.model, ctx.view.learn.cursor, e)) {
    return false;
  }
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(ctx.model, e.binding->param, scratch);
  const core::ParamId id = e.binding->param;
  if (store_set(ctx, e.scope, e.slot, core::LearnBinding{}) != model::Status::Ok) {
    return say_not_written(ctx, WriteResult::NoProject);
  }
  reset_runtime(ctx, e.scope, e.slot);
  const unsigned left = learn_entry_count(ctx.model);
  if (ctx.view.learn.cursor != 0 && ctx.view.learn.cursor >= left) {
    ctx.view.learn.cursor = static_cast<std::uint8_t>(left != 0 ? left - 1u : 0u);
  }
  if (d != nullptr && id != core::ParamId::None) {
    (void)say_binding(ctx, "UNBOUND ", 0, d->name, false);
  }
  return true;
}

// F4 in the overlay: clears every binding of both stores. A destructive key asks twice: the first press arms it (the footer says so), the second clears; any other
// key of the overlay disarms it.
bool learn_clear_all(ActionContext& ctx, core::ActionArg) noexcept {
  if (learn_entry_count(ctx.model) == 0) {
    ctx.view.learn.confirm = false;
    return false;
  }
  if (!ctx.view.learn.confirm) {
    ctx.view.learn.confirm = true;
    return true;
  }
  ctx.view.learn.confirm = false;
  ctx.view.learn.cursor = 0;
  for (const core::LearnScope scope : {core::LearnScope::Project, core::LearnScope::Device}) {
    const core::LearnBinding* slots = store_slots(ctx, scope);
    for (unsigned s = 0; slots != nullptr && s < core::kLearnSlots; ++s) {
      if (!core::learn_empty(slots[s]) && store_set(ctx, scope, s, core::LearnBinding{}) == model::Status::Ok) {
        reset_runtime(ctx, scope, s);
      }
    }
  }
  return true;
}

// OS-LEARNFOLLOW 3.3, the fit rule: a follow binding acts only when the instrument has the field with its meaning (F7): the sampler,
// source, envelope, MOD and VEL> rows (0x00..0x77, 0x8B) belong to a Sample instrument, a MIDI row (0x80..0x8A) to a MIDI one; a
// module parameter (0x100..) means something else in every module, so it never follows (A-4).
bool follow_fits(model::InstrumentKind kind, unsigned field) noexcept {
  if (model::instrument_param::is_module_field(field)) {
    return false;
  }
  if (field >= model::instrument_param::kMidiFirst && field < model::instrument_param::VelSens) {
    return kind == model::InstrumentKind::Midi;
  }
  return kind == model::InstrumentKind::Sample;
}

namespace {

// Binds controller `cc` (channel `channel`, 0 = every) to the Learn target, in the store its kind belongs to.
bool learn_bind(ActionContext& ctx, unsigned channel, unsigned cc) noexcept {
  ctx.view.learn.confirm = false;
  const core::ParamId id = focus_param(ctx);
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(ctx.model, id, scratch);
  if (id == core::ParamId::None || d == nullptr || (d->flags & core::kParamLearnable) == 0) {
    return say(ctx, core::Severity::Info, "NO TARGET TO LEARN");
  }
  const core::LearnScope scope = scope_of(id);
  const core::LearnBinding* slots = store_slots(ctx, scope);
  if (slots == nullptr || (scope == core::LearnScope::Project && ctx.edit == nullptr)) {
    return say_not_written(ctx, WriteResult::NoProject);
  }
  core::LearnBinding b{};
  b.param = id;
  b.cc = static_cast<std::uint8_t>(cc);
  b.channel = static_cast<std::uint8_t>(channel);
  b.mode = static_cast<std::uint8_t>(core::LearnMode::Pickup);  // the maintainer's default: no jumps
  // Rebinding the same parameter keeps the mode the user chose for it.
  const int old = core::learn_find_param(slots, core::kLearnSlots, id);
  if (old >= 0) {
    b.mode = slots[old].mode;
    // OS-LEARNFOLLOW 3.2: rebinding a followed field stays a follow binding of that field (binding a field that has no binding makes a fixed one).
    if ((slots[old].flags & core::kLearnFollow) != 0) {
      b.param = core::param_id(core::ParamKind::Instrument, 0, core::param_field(id));
      b.flags = core::kLearnFollow;
    }
  }
  const LearnPut put = learn_put(ctx, b);
  if (put != LearnPut::Done) {
    return say(ctx, core::Severity::Warn, put == LearnPut::Full ? "LEARN STORE FULL" : "BINDING REFUSED");
  }
  (void)say_binding(ctx, "CC ", cc, d->name, true);
  return true;
}

// OS-MIDICCMAP S2, Q2 a: does any binding of either store listen to this message (channel and controller)? "A binding listens", not "a binding moved something": a Pickup
// binding that has not caught the knob still owns the message.
bool learn_listens(const ActionContext& ctx, unsigned channel, unsigned cc) noexcept {
  for (const core::LearnScope scope : {core::LearnScope::Project, core::LearnScope::Device}) {
    const core::LearnBinding* slots = store_slots(ctx, scope);
    if (slots != nullptr && core::learn_find_cc(slots, core::kLearnSlots, channel, cc) >= 0) {
      return true;
    }
  }
  return false;
}

// The write of a knob's move, shared by a Learn binding (apply_cc) and the encoder of Instr + FX1 (enc_turn, OS-ENCLEARN): the same click group, the same final rules, the same write.
WriteResult write_moved(ActionContext& ctx, core::ParamId id, float next) noexcept {
  merge_click(ctx, id);  // a knob's sweep is one unit too, by the parameter it resolved to
  // D10: a track's or the master's parameter is the Mixer's, and the Mixer has no history, whichever screen the controller moved it on.
  const bool mixer_owned = ctx.edit != nullptr && (core::param_kind(id) == core::ParamKind::Track || core::param_kind(id) == core::ParamKind::Master);
  // OS-FXREC 3.4: while a take is open, every parameter write of midi.cc is final as well: the knob's move is applied, not journalled, so one Undo removes the take's notes and FX and leaves the
  // parameter where the knob put it; only the recorded FX steps are journalled into the take (rec_fx_write).
  const bool in_take = ctx.edit != nullptr && ctx.view.edit.take_open;
  const bool was_final = ctx.edit != nullptr ? ctx.edit->final_edits() : false;
  if (mixer_owned || in_take) {
    ctx.edit->set_final(true);
  }
  const WriteResult r = write_param(ctx, id, next);
  if (mixer_owned || in_take) {
    ctx.edit->set_final(was_final);
  }
  return r;
}

// OS-MIDIREC-P1 S4 (A13, "S4 as built, part b"): while a Lane take is recording, the move of a CC A..F parameter of the cursor's MIDI instrument is the controller's value, not an edit of the letter's selector.
// `abs_norm` >= 0 is the knob's absolute position 0..1 (a Learn binding in Jump or Pickup mode: the pickup is bypassed, the knob IS the value); else `detents` steps the letter's running value (the encoder, a
// relative binding), which starts at the last value of that letter in this take, else at CC 64 (never at the stored selector value, which is a controller NUMBER). The value recorded is the one rec_fx_write
// writes (round(norm * 127)), the controller the letter names now (read before this move; nothing writes the selector). `now` is not used. A move that cannot be recorded (no take, the count-in) is swallowed
// all the same, with one notice per take: the selector write must not leak in half way through a take.
struct LaneCcTake {
  bool taken = false;    // the move was handled (the caller writes nothing); false: not a lane CC move
  unsigned letter = 0;   // 0..5
  unsigned value = 0;    // 0..127, what the take got
};
LaneCcTake lane_cc_take(ActionContext& ctx, core::ParamId id, const core::ParamDesc& d, float abs_norm, int detents) noexcept {
  LaneCcTake t;
  if (ctx.lane == nullptr) {
    return t;
  }
  const int letter = lane_cc_letter(*ctx.lane, ctx.model, ctx.view, id);
  if (letter < 0) {
    return t;
  }
  LaneRec& rec = *ctx.lane;
  const unsigned bit = 1u << static_cast<unsigned>(letter);
  float next;
  if (abs_norm >= 0.0f) {
    next = core::param_denorm(d, abs_norm);
  } else {
    const float from = (rec.cc_have & bit) != 0u ? rec.cc_now[letter] : core::param_denorm(d, 64.0f / 127.0f);
    next = core::param_step(d, from, detents);
  }
  rec.cc_now[letter] = next;
  rec.cc_have = static_cast<std::uint8_t>(rec.cc_have | bit);
  const float n = core::param_norm(d, next);
  const float c = n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
  t.taken = true;
  t.letter = static_cast<unsigned>(letter);
  t.value = static_cast<unsigned>(c * 127.0f + 0.5f);
  if (!lane_cc_move(rec, ctx.edit, ctx.model, ctx.view, t.letter, t.value) && !rec.said_ci) {
    rec.said_ci = true;
    (void)say(ctx, core::Severity::Info, "LANE REC: NOT RECORDED: COUNT-IN");
  }
  return t;
}

// A message for the bindings of one store.
bool apply_cc(ActionContext& ctx, core::LearnScope scope, unsigned channel, unsigned cc, unsigned value) noexcept {
  const core::LearnBinding* slots = store_slots(ctx, scope);
  if (slots == nullptr || ctx.session == nullptr) {
    return false;
  }
  const int slot = core::learn_find_cc(slots, core::kLearnSlots, channel, cc);
  if (slot < 0) {
    return false;
  }
  const core::LearnBinding& b = slots[slot];
  core::LearnRuntime& rt = ctx.session->learn[scope_index(scope)][slot];
  if (ctx.view.overlay == OverlayId::LearnMap) {  // OS-LEARNMAP: the row's activity dot (the app's poll clears it)
    ctx.view.learn_map.hit[scope_index(scope)] |= std::uint32_t{1} << slot;
    ctx.view.learn_map.hit_at = ctx.now;
  }
  // A follow binding acts on the instrument the UI has selected (the Instruments are 1..48, slots 0..47).
  core::ParamId id = b.param;
  if ((b.flags & core::kLearnFollow) != 0) {
    const unsigned inst_slot = ctx.view.instrument > 0 ? static_cast<unsigned>(ctx.view.instrument - 1) : 0u;
    const unsigned field = core::param_field(b.param);
    id = core::param_id(core::ParamKind::Instrument, inst_slot, field);
    // OS-LEARNFOLLOW 3.3: the selected instrument must have the field with its meaning, else the message does nothing (silent, as a free slot).
    const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(inst_slot) : nullptr;
    if (in == nullptr || !follow_fits(in->kind, field)) {
      return false;
    }
  }
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(ctx.model, id, scratch);
  float now = 0.0f;
  if (d == nullptr || (d->flags & core::kParamLearnable) == 0u || !read_param(ctx.model, id, now)) {
    return false;  // no such parameter now (a free instrument slot, or no project), or one that cannot be learned (a binding edited by hand onto it: the OS-INSTUI S8a CHANNEL)
  }
  float next = now;
  if (static_cast<core::LearnMode>(b.mode) == core::LearnMode::Relative) {
    const int delta = core::learn_relative_delta(value);
    if (delta == 0) {
      return false;
    }
    if (lane_cc_take(ctx, id, *d, -1.0f, delta).taken) {
      return true;
    }
    next = core::param_step(*d, now, delta);
  } else {
    if (lane_cc_take(ctx, id, *d, static_cast<float>(value > 127u ? 127u : value) / 127.0f, 0).taken) {
      return true;
    }
    float norm = 0.0f;
    if (!core::learn_absolute(rt, id, static_cast<core::LearnMode>(b.mode), value, core::param_norm(*d, now),
                              param_generation_of(ctx.model, id), norm)) {
      return false;  // pickup: the knob has not reached the parameter yet
    }
    next = core::param_denorm(*d, norm);
  }
  const WriteResult r = write_moved(ctx, id, next);
  core::learn_applied(rt, id, param_generation_of(ctx.model, id));
  if (r == WriteResult::Changed) {
    (void)rec_fx_write(ctx, id, d->name);  // OS-FXREC S2, 3.2: the move records as the step FX of the open take (nothing when no take is open)
  }
  return r == WriteResult::Changed;
}

}  // namespace

// OS-ENCLEARN: Instr + FX2 (the chord): the Learn row under the cursor (focus_param, kParamLearnable: Learn's own resolution) becomes the encoder's one target, replacing the old one.
bool enc_assign(ActionContext& ctx, core::ActionArg) noexcept {
  const core::ParamId id = focus_param(ctx);
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(ctx.model, id, scratch);
  if (id == core::ParamId::None || d == nullptr || (d->flags & core::kParamLearnable) == 0) {
    return say_tag(ctx, core::Severity::Info, "NO TARGET FOR ENCODER", "enc");
  }
  ctx.view.enc_target = id;  // no clear gesture (maintainer, 2026-11-08): the same row again keeps it and says it again; another row replaces it
  core::FixedString<core::kNotifyTextChars> text;
  text.append("ENCODER: ");
  text.append(d->name);
  if (core::param_kind(id) == core::ParamKind::Instrument) {
    text.append(" INS ");
    append_uint(text, core::param_index(id) + 1u);
  }
  return say_tag(ctx, core::Severity::Info, text.c_str(), "enc");
}

std::uint8_t param_auto_mask(const Model& model) noexcept {
  if (model.project == nullptr) {
    return 0;
  }
  model::AutoMap map;
  model::auto_scan(*model.project, map);
  std::uint8_t mask = 0;
  for (unsigned l = 0; l < model::kAutoLetters; ++l) {
    if (map.id[l] != core::ParamId::None) {  // (learnable or not is param_auto's to say: a module's descriptor exists only while its instance does, which no project generation tells)
      mask = static_cast<std::uint8_t>(mask | (1u << l));
    }
  }
  return mask;
}

// OS-PARAMAUTO S2 (new Community OS behaviour): the value the sequencer played on CC letter `arg >> 8` (A..F = 0..5, low byte 0..127) moves the parameter that owns the letter. The map is read from the project at
// each call (64 instruments x 6 bytes, a few values a pass at most, never per step), so a project edit needs no rebuild here. The write is final as in a take: the playback is not an edit of the document (no undo
// record, the project keeps the last value played, so a save has it), and send_param_change tells the audio side.
bool param_auto(ActionContext& ctx, core::ActionArg arg) noexcept {
  const unsigned letter = (arg >> 8) & 0xFFu;
  if (ctx.edit == nullptr || letter >= model::kAutoLetters) {
    return false;
  }
  model::AutoMap map;
  model::auto_scan(ctx.edit->project(), map);
  const core::ParamId id = map.id[letter];
  ParamScratch scratch;
  const core::ParamDesc* d = id != core::ParamId::None ? describe_param(ctx.model, id, scratch) : nullptr;
  if (d == nullptr || (d->flags & core::kParamLearnable) == 0u) {
    return false;  // a letter that names nothing now (a changed kind, a code a newer build knew), or a parameter that cannot be learned
  }
  const unsigned v = arg & 0xFFu;
  const bool was_final = ctx.edit->final_edits();
  ctx.edit->set_final(true);
  const WriteResult r = write_param(ctx, id, core::param_denorm(*d, static_cast<float>(v > 127u ? 127u : v) / 127.0f));
  ctx.edit->set_final(was_final);
  return r == WriteResult::Changed;
}

// OS-ENCLEARN: the encoder turned while Instr + FX1 are held (App::run_once takes it ahead of the bindings): the target moves by `arg` fine steps, a coarse step each with Shift, through the write
// of a relative Learn binding (write_moved) and the same recorder (rec_fx_write: LIVE, STEP). The strip shows "<PARAMETER> <value>" while it moves.
bool enc_turn(ActionContext& ctx, core::ActionArg arg) noexcept {
  const core::ParamId id = ctx.view.enc_target;
  if (id == core::ParamId::None) {
    return say_tag(ctx, core::Severity::Info, "NO ENCODER TARGET (INSTR + FX2)", "enc");
  }
  ParamScratch scratch;
  const core::ParamDesc* d = describe_param(ctx.model, id, scratch);
  float now = 0.0f;
  if (d == nullptr || (d->flags & core::kParamLearnable) == 0u || !read_param(ctx.model, id, now)) {
    return say_tag(ctx, core::Severity::Info, "ENCODER TARGET IS GONE", "enc");  // no project or a free instrument slot now
  }
  int detents = arg > kMaxDetents ? kMaxDetents : (arg < -kMaxDetents ? -kMaxDetents : static_cast<int>(arg));
  if (ctx.view.shift) {
    detents *= static_cast<int>(d->coarse);
  }
  const LaneCcTake lane = lane_cc_take(ctx, id, *d, -1.0f, detents);  // a Lane take records the letter's value, the selector stays
  if (lane.taken) {
    core::FixedString<core::kNotifyTextChars> text;  // the strip shows the value the take got
    text.append("CC ");
    const char l[2] = {static_cast<char>('A' + lane.letter), '\0'};
    text.append(l);
    text.append(" ");
    const unsigned v = lane.value > 127u ? 127u : lane.value;  // 0..127: up to three digits
    char digits[4] = {};
    unsigned n = 0;
    if (v >= 100u) {
      digits[n++] = static_cast<char>('0' + v / 100u);
    }
    if (v >= 10u) {
      digits[n++] = static_cast<char>('0' + (v / 10u) % 10u);
    }
    digits[n] = static_cast<char>('0' + v % 10u);
    text.append(digits);
    (void)ctx.view.notify.upsert(core::Severity::Info, text.c_str(), "enc", ViewState::kMessageMs);
    return true;
  }
  const float next = core::param_step(*d, now, detents);
  const WriteResult r = write_moved(ctx, id, next);
  float shown = now;
  (void)read_param(ctx.model, id, shown);
  core::FixedString<core::kNotifyTextChars> text;
  text.append(d->name);
  text.append(" ");
  char value[core::kParamTextChars + 1];
  (void)core::param_format(*d, shown, value, sizeof value);
  text.append(value);
  (void)ctx.view.notify.upsert(core::Severity::Info, text.c_str(), "enc", ViewState::kMessageMs);
  if (r == WriteResult::Changed) {
    (void)rec_fx_write(ctx, id, d->name, true);  // after the value line, so that "REC FX: ..." (the take's first FX, HAS NO FX) is the newest message
  }
  return r == WriteResult::Changed;
}


bool midi_cc(ActionContext& ctx, core::ActionArg arg) noexcept {
  const std::uint32_t bits = static_cast<std::uint32_t>(arg);
  const unsigned channel = (bits >> 16) & 0xFFu;
  const unsigned cc = (bits >> 8) & 0xFFu;
  const unsigned value = bits & 0xFFu;
  if (channel > 16 || cc > 127 || value > 127 || (bits >> 24) != 0) {
    return false;
  }
  if (ctx.view.xport.running) {
    return false;  // a WAV export is modal and renders the project: a learned CC on a mute, a solo or DRY MUTE must not change it, or the ring, between two files (OS-WAVEXP)
  }
  if (ctx.view.overlay == OverlayId::Learn) {
    return learn_bind(ctx, channel, cc);
  }
  if (ctx.view.overlay == OverlayId::LearnMap && ctx.view.learn_map.wait != 0) {
    return learn_map_capture(ctx, channel, cc);  // OS-LEARNMAP: F1 NEW / F7 RELEARN is waiting for a knob
  }
  // The project's bindings first, then the device's: one message may move a parameter of each.
  const bool a = apply_cc(ctx, core::LearnScope::Project, channel, cc, value);
  const bool b = apply_cc(ctx, core::LearnScope::Device, channel, cc, value);
  // OS-MIDICCMAP S2 (3.2): the fixed CC map acts after both stores, only when the device setting CC MAP is On and no binding of either store listens to the message (Q2 a).
  // The port and channel filter is the one poll_midi_cc already applied (MIDI IN, IN CHANNEL); with CC MAP Off this is byte for byte today's behaviour.
  bool c = false;
  if (ctx.model.device.get(model::device_setting::CcMap) != 0.0f && !learn_listens(ctx, channel, cc)) {
    c = cc_map_apply(ctx, cc, value);
  }
  return a || b || c;
}

}  // namespace o3os::app
