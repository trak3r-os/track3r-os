// SPDX-License-Identifier: GPL-3.0-or-later
// The Instrument screen's own actions (OS-INSTUI S1, S2, S5, S7): the page keys F1..F5, the neighbour keys F6 / F7 (and Shift + F6 / F7: the nearest in use), the Shift row (NAME, COPY, PASTE, DELETE,
// INIT; SAVE is in save_actions.cpp), the type chooser and the pads that play the instrument.
#include "action_handlers.h"
#include "param_access.h"

#include <o3os/app/pad_lights.h>
#include <o3os/app/pad_maps.h>
#include <o3os/app/pad_note.h>
#include <o3os/app/sample_load_view.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/text_entry.h>
#include <o3os/app/text_format.h>
#include <o3os/model/params.h>

#include <cstring>

namespace o3os::app {

namespace {

bool say(ActionContext& ctx, const char* text) noexcept { return say_tag(ctx, core::Severity::Info, text, "instrument"); }

bool on_screen(const ActionContext& ctx) noexcept { return ctx.view.screen == ScreenId::Instrument; }

// OS-INSFX S5: the keys that edit the instrument in view (NAME, COPY, PASTE, DELETE, INIT, SAVE) do not act on an insert's page: there the rows are the insert's parameters.
bool on_instrument_page(const ActionContext& ctx) noexcept { return on_screen(ctx) && !params_insert_target(ctx.view); }

// The instrument in view becomes `to` (1..64): the notes the pads of this screen hold end first, on the instrument that started them (O2).
bool select_to(ActionContext& ctx, int to) noexcept {
  if (to < 1 || to > static_cast<int>(model::kMaxInstruments) || to == ctx.view.instrument) {
    return false;
  }
  release_live_pads(ctx);
  ctx.view.instrument = to;
  ctx.view.params.insert = ParamView::kNoInsert;  // OS-INSFX S5: an instrument chosen on an insert's page takes the page over
  std::uint8_t& page = ctx.view.page[screen_index(ScreenId::Instrument)];
  if (page == ParamsScreen::kMixPage) {  // OS-MODSENDS: MIX belongs to the module instrument it was opened on: another slot opens on its first page
    page = 0;
    ctx.view.params.row = 0;
    ctx.view.params.discard_pending();
  }
  return true;
}

}  // namespace

// O2: every pad this screen started ends its note (PreviewSink::pad_up ends the note on the instrument that started it, whatever that was). The physical release later finds nothing to end.
void release_live_pads(ActionContext& ctx) noexcept {
  std::uint64_t held = ctx.view.inst_pads;
  ctx.view.inst_pads = 0;
  if (ctx.preview == nullptr) {
    return;
  }
  for (unsigned pad = 0; held != 0u && pad < kPadCount; ++pad, held >>= 1) {
    if ((held & 1u) != 0u) {
      ctx.preview->pad_up(pad);
    }
  }
}

// F1..F5, arg = the key 0..4: what the key means depends on the instrument in view (D1). A module's keys are its pages; a sample instrument's F1 is the source page (S3) and F2..F5 the groups; a MIDI
// instrument has two pages, F1 MIDI (S8a) and F2 CC (S8b), and F3..F5 say why they are dim; an empty slot's keys are the type chooser's (S5).
bool instrument_page(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!on_screen(ctx) || arg < 0 || arg > 4) {
    return false;
  }
  const unsigned key = static_cast<unsigned>(arg);
  switch (params_slot_kind(ctx.model, ctx.view)) {
    case SlotKind::Module:
      if (key == 4u) {  // OS-INSFX-ABI2 A9: a module has at most four pages (F1..F4), F5 is PRESET (an instrument module's page and an insert's)
        return preset_open(ctx);
      }
      if (key == 0u && kParamsScreen.module_page_count(ctx.model, ctx.view) == 0u) {  // OS-MODSENDS: a module with no pages: F1 leaves MIX (as the second Shift + F2 does)
        std::uint8_t& page = ctx.view.page[screen_index(ScreenId::Instrument)];
        if (page != ParamsScreen::kMixPage || params_insert_target(ctx.view)) {
          return false;
        }
        page = 0;
        ctx.view.params.row = 0;
        ctx.view.params.discard_pending();
        return true;
      }
      return key < kParamsScreen.module_page_count(ctx.model, ctx.view) && param_group(ctx, arg);
    case SlotKind::Empty:  // the type chooser has its own keys (context instrument.empty): nothing of the pages is here
      return false;
    case SlotKind::Sample:  // F1 is the source page (S3), F2..F5 the groups; the key of the page in view again steps that group's windows (OS-INSTUI-ARROWS: F4 the six envelopes)
      return (key == 0u ? param_group(ctx, model::instrument_param::GroupSource) : param_group(ctx, arg - 1)) || kParamsScreen.next_window(ctx.model, ctx.view, key);
    case SlotKind::Midi: {  // OS-INSTUI S8b: F1 MIDI (CHANNEL, VELOCITY, PROGRAM, OUT, PB RANGE, OFFSET), F2 CC (CC A..F); F3..F5 are dim and say why
      if (key <= 1u) {
        ParamView& pv = ctx.view.params;
        std::uint8_t& page = ctx.view.page[screen_index(ctx.view.screen)];
        const std::uint8_t to = static_cast<std::uint8_t>(key == 0u ? model::instrument_param::GroupMidi : model::instrument_param::GroupCc);
        const bool at_cc = page == model::instrument_param::GroupCc;
        const bool moved = pv.row != 0 || at_cc != (key == 1u);
        page = to;
        pv.row = 0;
        pv.discard_pending();
        return moved;
      }
      static constexpr const char* kNames[3] = {"FILTER", "MOD", "SENDS"};
      core::FixedString<core::kNotifyTextChars> text;
      text.assign("NO ");
      text.append(kNames[key - 2u]);
      text.append(" PAGE ON A MIDI INSTRUMENT");
      return say(ctx, text.c_str());
    }
  }
  return false;
}

// Left / Right in the type chooser of an empty slot (D1: the previous / next page, wrapping; OS-INSTUI-ARROWS: on a used slot they edit the row, param.edit). The gestures deliver an auto-repeat as another plain tap, so a press within kTurnFreshMs of the last is
// taken for one and stops at the ends instead of going round (holding Right on ENV would cycle the pages for ever).
bool instrument_turn(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!on_screen(ctx) || arg == 0) {
    return false;
  }
  ParamView& pv = ctx.view.params;
  const bool fresh = !pv.turn_seen || core::tick_elapsed(ctx.now, pv.turn_at) > core::ticks_from_ms(ParamView::kTurnFreshMs);
  pv.turn_seen = true;
  pv.turn_at = ctx.now;
  return kParamsScreen.turn(ctx.model, ctx.view, arg < 0 ? -1 : 1, fresh);
}

// F6 / F7 (O1): one slot back / on, 1..64, no wrap; at an end nothing happens (no notification for a non-event).
bool instrument_step(ActionContext& ctx, core::ActionArg arg) noexcept {
  return on_screen(ctx) && arg != 0 && select_to(ctx, ctx.view.instrument + (arg < 0 ? -1 : 1));
}

// Shift + F6 / F7 (O1): the previous / next instrument that holds something; with none that way, nothing. OS-INSFX S5: not on an insert's page (the key row dims the Shift row there; the rows are the insert's
// parameters, and choosing an instrument stays with the pads and the plain F6 / F7).
bool instrument_used(ActionContext& ctx, core::ActionArg arg) noexcept {
  return on_instrument_page(ctx) && arg != 0 && select_to(ctx, instrument_in_use(ctx.model, ctx.view.instrument, arg));
}

namespace {

unsigned slot_in_view(const ActionContext& ctx) noexcept { return static_cast<unsigned>(ctx.view.instrument < 1 ? 0 : ctx.view.instrument - 1); }

// "INS 07 <what> - F8 UNDO" (the strip text is upper case ASCII: the font has no middle dot).
bool say_done(ActionContext& ctx, unsigned slot, const char* what) noexcept {
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("INS ");
  append_dec(text, slot + 1u, 2);
  text.append(" ");
  text.append(what);
  text.append(" - F8 UNDO");
  return say(ctx, text.c_str());
}

}  // namespace

// Shift + F4 DELETE (OS-INSTUI D5): the instrument leaves its slot, one undo unit, no dialog. The steps are not touched (they stay silent until another instrument is put there), nothing on the card
// is touched, and the bank's slot is made silent at the next quiet handover (the PCM stays in the arena until the next project load, which is what an undo needs). A module instance stays bound (there is
// no unbind): the gate of ModuleUi::sync_slots keeps it from sounding, and ModuleUi::pick refuses a new module on the slot until the project is reloaded.
bool instrument_delete(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_instrument_page(ctx) || ctx.edit == nullptr) {
    return false;
  }
  const unsigned slot = slot_in_view(ctx);
  if (ctx.edit->project().instrument(slot) == nullptr) {
    return say(ctx, "NOTHING TO DELETE");
  }
  release_live_pads(ctx);  // O2: the notes the pads hold end on the instrument that started them
  if (ctx.edit->clear_instrument(slot) != model::Status::Ok) {
    return false;
  }
  if (ctx.samples != nullptr) {
    ctx.samples->request_rebind(slot);
  }
  if (ctx.model.modules != nullptr) {
    ctx.model.modules->sync_slots(ctx.edit->project());
  }
  ctx.view.params.row = 0;
  ctx.view.params.discard_pending();
  (void)say_done(ctx, slot, "DELETED");
  return true;
}

namespace {

// "INS 07 <what>" (no undo hint): COPIED and the refusals that name the slot.
bool say_slot(ActionContext& ctx, unsigned slot, const char* what) noexcept {
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("INS ");
  append_dec(text, slot + 1u, 2);
  text.append(" ");
  text.append(what);
  return say(ctx, text.c_str());
}

// An instrument pasted into a slot gets that slot's sample file name, as the importers give it ("samples/instrNN.wav"): two instruments must never name one file in the project folder, and the
// audio they share in memory is written once for each slot.
void own_sample_path(model::Instrument& in, unsigned slot) noexcept {
  static constexpr char kStem[] = "samples/instr";
  std::memset(in.sample_path, 0, sizeof in.sample_path);
  std::memcpy(in.sample_path, kStem, sizeof kStem - 1);
  std::size_t at = sizeof kStem - 1;
  in.sample_path[at++] = static_cast<char>('0' + (slot + 1u) / 10u);
  in.sample_path[at++] = static_cast<char>('0' + (slot + 1u) % 10u);
  std::memcpy(in.sample_path + at, ".wav", 4);
}

}  // namespace

// Shift + F1 NAME (OS-INSTUI S7): the on-screen keyboard (text_entry.h) over the instrument's name, the whole of it selected; F8 keeps the typed name as one undo unit (text.accept ->
// instrument_rename_accept), F7 leaves it as it was. The stored field decides what can be typed: up to 32 characters (kInstrumentNameMax), every key of the keyboard.
bool instrument_name(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_instrument_page(ctx)) {
    return false;
  }
  const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(slot_in_view(ctx)) : nullptr;
  if (in == nullptr || ctx.session == nullptr || ctx.session->document() == nullptr) {
    return say(ctx, "NO INSTRUMENT TO NAME");
  }
  static_assert(model::kInstrumentNameMax == kTextCapacity, "the keyboard's name limit is the instrument name field");
  TextEntryView& t = ctx.view.text;
  t.entry.begin(in->name, model::kInstrumentNameMax, TextEntry::Filter::Any);
  t.target = TextEntryView::Target::Instrument;
  t.message = "";
  ctx.view.overlay = OverlayId::TextEntry;
  return true;
}

// F8 in the keyboard, for the Instrument target: the typed name replaces the instrument's (Editor::set_instrument_name: one undo record). The same name changes nothing.
bool instrument_rename_accept(ActionContext& ctx) noexcept {
  TextEntryView& t = ctx.view.text;
  ctx.view.overlay = OverlayId::None;
  t.target = TextEntryView::Target::None;
  if (ctx.edit == nullptr) {
    return true;
  }
  const unsigned slot = slot_in_view(ctx);
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (in == nullptr || std::strcmp(in->name, t.entry.text()) == 0) {
    return true;
  }
  if (ctx.edit->set_instrument_name(slot, t.entry.text(), t.entry.length()) != model::Status::Ok) {
    (void)say(ctx, "NAME REFUSED");
    return true;
  }
  (void)say_done(ctx, slot, "RENAMED");
  return true;
}

// Shift + F2 COPY: the clipboard takes the instrument (its parameters by value, its audio by reference: sample_bank.h). A module instrument has a running instance that cannot be duplicated: its key opens MIX.
bool instrument_copy(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_instrument_page(ctx)) {
    return false;
  }
  const unsigned slot = slot_in_view(ctx);
  const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(slot) : nullptr;
  if (in == nullptr) {
    return say(ctx, "NOTHING TO COPY");
  }
  if (in->kind == model::InstrumentKind::Module) {  // OS-MODSENDS: a module instrument cannot be copied (its running instance); its Shift + F2 key is MIX, the page of PANNING and the sends
    (void)kParamsScreen.open_mix(ctx.model, ctx.view);
    return true;
  }
  if (ctx.samples == nullptr || !ctx.samples->ready()) {
    return say(ctx, "NO CLIPBOARD (NO SAMPLE MEMORY)");
  }
  ctx.samples->clip_set(*in, slot);
  return say_slot(ctx, slot, "COPIED");
}

// Shift + F3 PASTE, and the chooser's F3 (D9): the clipboard's instrument goes into this slot, an empty one or a used one it REPLACES, as one undo unit. Refused only where a module is in the way (a
// module instance cannot be unbound: delete it first, S5). A sample instrument shares the copied audio (no copy, no arena: SampleBank::paste_audio) and gets this slot's sample file name; a MIDI
// instrument comes with its channel. The notes the pads hold end first (O2); the audio side follows at the next quiet handover.
bool instrument_paste(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_instrument_page(ctx) || ctx.edit == nullptr) {
    return false;
  }
  SampleBank* bank = ctx.samples;
  const unsigned slot = slot_in_view(ctx);
  if (bank == nullptr || !bank->clip_valid()) {
    return say(ctx, "NOTHING TO PASTE: SHIFT+F2 COPIES");
  }
  const model::Instrument* cur = ctx.edit->project().instrument(slot);
  if (cur != nullptr && cur->kind == model::InstrumentKind::Module) {
    return say_slot(ctx, slot, "IS A MODULE: DELETE IT FIRST");
  }
  model::Instrument next = bank->clip_instrument();
  if (next.kind == model::InstrumentKind::Sample) {
    own_sample_path(next, slot);
  }
  if (cur != nullptr && std::memcmp(cur, &next, sizeof next) == 0) {
    return say_slot(ctx, slot, "ALREADY HOLDS THIS");
  }
  const bool had = cur != nullptr;
  const bool same_kind = had && cur->kind == next.kind;
  release_live_pads(ctx);
  if (ctx.edit->set_instrument(slot, next) != model::Status::Ok) {
    return say_slot(ctx, slot, "PASTE REFUSED");
  }
  (void)bank->paste_audio(slot);  // the audio half: shared, one swap on record for the undo of this unit
  bank->request_rebind(slot);
  if (ctx.model.modules != nullptr) {
    ctx.model.modules->sync_slots(ctx.edit->project());
  }
  if (!same_kind) {
    (void)kParamsScreen.repress(ctx.model, ctx.view);  // a page the new kind does not have: its first
  }
  ctx.view.params.row = 0;
  ctx.view.params.discard_pending();
  (void)say_done(ctx, slot, had ? "REPLACED" : "PASTED");
  return true;
}

// The chooser's F5 LOAD (S7): the Sample load screen, listing the .pti files of instruments/ (WavView::pti); its F1 loads the one under the cursor into this slot (App::start_pti_load).
bool instrument_load(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_screen(ctx)) {
    return false;
  }
  if (ctx.view.card_on_computer) {  // OS-USBDISK 4.3: the instrument files are on the card the computer has; asked ahead of the persistent strip
    return ctx.view.notify.ask(core::Severity::Warn, "CARD ON COMPUTER", "instrument", ViewState::kMessageMs);
  }
  if (ctx.model.project == nullptr || ctx.session == nullptr || ctx.session->document() == nullptr) {
    return say(ctx, "NO PROJECT OPEN");
  }
  if (ctx.model.samples == nullptr || !ctx.model.samples->ready()) {
    return say(ctx, "NO SAMPLE MEMORY");
  }
  WavView& w = ctx.view.wav;
  w.pti = true;
  std::strcpy(w.folder, kInstrumentsFolder);
  if (!switch_screen(screen_registry(), ctx.model, ctx.view, ScreenId::SampleLoad)) {
    w.pti = false;
    w.folder[0] = '\0';
    return false;
  }
  w.back_to_params = true;
  return true;
}

// Shift + Delete INIT (D8): every parameter back to its descriptor's default, one undo unit, no dialog. The kind, the sample, the module and the name stay. A module's blob goes back to the module's
// own defaults (and the running instance is told); a sample or MIDI instrument is rebound at the next quiet handover.
bool instrument_init(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_instrument_page(ctx) || ctx.edit == nullptr) {
    return false;
  }
  const unsigned slot = slot_in_view(ctx);
  const model::Instrument* in = ctx.edit->project().instrument(slot);
  if (in == nullptr) {
    return say(ctx, "NOTHING TO INIT");
  }
  model::Instrument edited = *in;
  float module_defaults[model::instrument_param::kModuleMax] = {};
  unsigned module_count = 0;
  if (in->kind == model::InstrumentKind::Module) {
    const ModuleUi* mu = ctx.model.modules;
    module_count = mu != nullptr ? mu->param_count(ctx.edit->project(), slot) : 0u;
    if (module_count == 0u) {
      return say(ctx, "MODULE NOT LOADED");
    }
    for (unsigned i = 0; i < module_count && i < model::instrument_param::kModuleMax; ++i) {
      ParamScratch s;
      module_defaults[i] = mu->describe(ctx.edit->project(), slot, i, s) ? s.desc.def : 0.0f;
      std::memcpy(edited.params + 4u * i, &module_defaults[i], 4);
    }
    edited.param_len = static_cast<std::uint16_t>(4u * (module_count < model::instrument_param::kModuleMax ? module_count : model::instrument_param::kModuleMax));
  } else {
    model::reset_instrument_params(edited);
  }
  if (std::memcmp(&edited, in, sizeof edited) == 0) {
    return say(ctx, "ALREADY AT THE DEFAULTS");
  }
  if (ctx.edit->set_instrument(slot, edited) != model::Status::Ok) {
    return false;
  }
  for (unsigned i = 0; i < module_count && i < model::instrument_param::kModuleMax; ++i) {
    send_param_change(ctx, core::param_id(core::ParamKind::Instrument, slot, model::instrument_param::kModuleFirst + i), module_defaults[i]);
  }
  if (ctx.samples != nullptr && in->kind != model::InstrumentKind::Module) {
    ctx.samples->request_rebind(slot);
  }
  ctx.view.params.row = 0;
  ctx.view.params.discard_pending();
  (void)say_done(ctx, slot, "INIT");
  return true;
}

// The chooser's F2 MIDI (OS-INSTUI S5): a MIDI instrument on an empty slot, its channel the slot's (1 for slots 1..48, slot - 48 for 49..64: OS-INSTMIDI Q-IM-3), and its page (S8a: the MIDI page,
// whatever page the view holds).
bool instrument_new_midi(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_screen(ctx) || ctx.edit == nullptr) {
    return false;
  }
  const unsigned slot = slot_in_view(ctx);
  if (ctx.edit->project().instrument(slot) != nullptr) {
    return false;  // O7: the chooser only fills an empty slot
  }
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = static_cast<std::uint8_t>(midi_channel_for_slot(slot));
  if (ctx.edit->set_instrument(slot, in) != model::Status::Ok) {
    return false;
  }
  ctx.view.page[screen_index(ScreenId::Instrument)] = 0;
  ctx.view.params.row = 0;
  ctx.view.params.discard_pending();
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("MIDI INSTRUMENT ON CHANNEL ");
  append_dec(text, in.midi_channel, 1);
  return say(ctx, text.c_str());
}

// The chooser's F1 SAMPLE: the Sample load screen with this slot as the target (it loads into the selected instrument); the load brings the Params page back (App::finish_import).
bool instrument_sample(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_screen(ctx)) {
    return false;
  }
  if (ctx.model.project == nullptr || ctx.session == nullptr || ctx.session->document() == nullptr) {
    return say(ctx, "NO PROJECT OPEN");
  }
  if (!switch_screen(screen_registry(), ctx.model, ctx.view, ScreenId::SampleLoad)) {
    return false;
  }
  ctx.view.wav.back_to_params = true;
  return true;
}

// A pad (S2, D2: the instrument editor plays the instrument on the pads): the live note of the instrument in view, held until the pad's release (the App hands it to
// PreviewSink::pad_up and clears the bit). The note is the layout's (O4): the NOTES board, or PIANO / ISO as on Pattern (the other layers' pads are not notes, so they play the board); a beat slice
// instrument plays slice n on pad n. A MIDI instrument takes the same path: what its notes do is the sink's (the platform has no live MIDI output yet: S8a).
bool instrument_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!on_screen(ctx) || arg < 0 || arg >= static_cast<core::ActionArg>(kPadCount)) {
    return false;
  }
  const ViewState& v = ctx.view;
  const unsigned pad = static_cast<unsigned>(arg);
  unsigned note = 0;
  if (beat_slice_instrument(ctx.model.project, v.instrument)) {
    note = audio::kSliceRootNote + pad;
  } else if (v.padmode.fixed() == PadMode::Piano || v.padmode.fixed() == PadMode::Iso) {
    note = pad_fn_note(v.edit.entry, v.padmode.fixed() == PadMode::Piano ? piano_pad(pad) : iso_pad(pad));
    note = note == kNoNote ? note : v.edit.entry.locked(note);
    if (note > kEntryMaxNote) {
      return false;  // a pad that is not a note (the jumps and the octave keys of PIANO) or above the board's last
    }
  } else {
    note = v.edit.pads.note(pad);
  }
  if (ctx.preview != nullptr) {
    ctx.preview->pad_down(pad, static_cast<unsigned>(v.pattern.track()), static_cast<unsigned>(v.instrument), note);
    ctx.view.inst_pads |= std::uint64_t{1} << pad;
  }
  return false;  // nothing on the screen changes: the sound is the engine's
}

}  // namespace o3os::app
