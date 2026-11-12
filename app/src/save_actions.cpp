// SPDX-License-Identifier: GPL-3.0-or-later
// The save actions (OS-007n): File's SAVE and SAVE AS, the Record screen's SAVE WAV and the Sample edit screen's save of the edited sample. Each only asks the SaveService
// (save_service.h) and says how the request went; the writing is a job of the main loop (AtomicSave), whose progress and end are the notification strip's like any job's.
// Nothing here names a path: the service builds them from the card layer's own guard. Every one of them is refused, with the reason on the strip, unless the card may be
// written: no card write layer (the simulator), no card, a write-protected card or an exFAT one (any other card is written: no marker, OS-ANYCARD).
#include "action_handlers.h"

#include <o3os/app/module_ui.h>
#include <o3os/app/recorder.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/screens/wave_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/model/editor.h>

#include <cstring>

namespace o3os::app {

namespace {

// OS-USBDISK 4.3 (U2b-2): while the computer has the card the platform rewrites the persistent disk strip at the head of the strip every second, so a plain post queues behind it and shows only after the eject. A refusal is asked instead: it takes the head ahead of the strip (an Info entry), which comes back behind it.
bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  if (ctx.view.card_on_computer) {
    return ctx.view.notify.ask(severity, text, "save", ViewState::kMessageMs);
  }
  return ctx.view.notify.upsert(severity, text, "save", ViewState::kMessageMs);
}

// The refusals every project save shares, said on the strip: false when the save may not go on.
bool project_ready(ActionContext& ctx, bool& said) noexcept {
  ctx.view.file.ask_clear();  // (a save answers F3's question another way)
  said = true;
  if (ctx.saves == nullptr || !ctx.saves->attached()) {
    said = say(ctx, core::Severity::Warn, "NO CARD WRITE LAYER");
    return false;
  }
  if (ctx.model.project == nullptr || ctx.session == nullptr) {
    said = say(ctx, core::Severity::Info, "NO PROJECT TO SAVE");
    return false;
  }
  if (ctx.project != nullptr && ctx.project->busy()) {
    said = say(ctx, core::Severity::Info, "A LOAD IS RUNNING");
    return false;
  }
  return true;
}

bool save_project(ActionContext& ctx, bool as_new, const char* as_name = nullptr) noexcept {
  bool said = false;
  if (!project_ready(ctx, said)) {
    return said;
  }
  // SAVE replaces the file of the folder the project was loaded from, and only when that load worked: a load that failed leaves the project reset (empty), and saving that over the
  // good file the folder holds would put the user's project in the backup and an empty one in its place (OS-007o).
  const char* folder = ctx.project != nullptr && ctx.project->loaded() ? ctx.project->folder() : "";
  return save_report(ctx, ctx.saves->save_project(ctx.session->jobs, *ctx.model.project, folder, as_new, as_name, ctx.model.samples));
}

}  // namespace

// How a request went. Started says nothing: the job's own strip ("SAVE TRACK 01 12%") takes over.
bool save_report(ActionContext& ctx, SaveService::Start start) noexcept {
  switch (start) {
    case SaveService::Start::Started: return true;
    case SaveService::Start::NoWriter: return say(ctx, core::Severity::Warn, "NO CARD WRITE LAYER");
    case SaveService::Start::NotWritable: return say(ctx, core::Severity::Warn, SaveService::gate_text(ctx.saves->gate()));
    case SaveService::Start::Busy: return say(ctx, core::Severity::Info, "A SAVE IS ALREADY RUNNING");
    case SaveService::Start::NoJobSlot: return say(ctx, core::Severity::Warn, "TOO MANY JOBS");
    case SaveService::Start::NoName: return say(ctx, core::Severity::Warn, "NO FREE NAME ON THE CARD");
    case SaveService::Start::Empty: return say(ctx, core::Severity::Info, "NOTHING TO SAVE");
    case SaveService::Start::TooBig: return say(ctx, core::Severity::Warn, "TOO BIG FOR A WAV FILE");
    case SaveService::Start::CardInUse: return say(ctx, core::Severity::Warn, "FIRMWARE UPDATE HAS THE CARD: TRY AGAIN");
    case SaveService::Start::NameTaken: return say(ctx, core::Severity::Warn, "A PROJECT OF THAT NAME EXISTS");
    case SaveService::Start::BadName: return say(ctx, core::Severity::Warn, "NOT A NAME THE CARD TAKES");
    case SaveService::Start::NoExport: return say(ctx, core::Severity::Warn, "NO EXPORT ON THIS BUILD");
    case SaveService::Start::NotSample: return say(ctx, core::Severity::Info, "SAMPLE INSTRUMENTS ONLY");  // OS-INSTUI S7
    case SaveService::Start::PtiAmiga: return say(ctx, core::Severity::Warn, "AMIGA ENGINE: ONE SHOT OR LOOP FWD ONLY");  // OS-BUG-PTIRATE, OS-FILES-C C1b
    case SaveService::Start::PtiTooLong: return say(ctx, core::Severity::Warn, "TOO LONG FOR A .PTI AT 44100 HZ");
    case SaveService::Start::PtiBits: return say(ctx, core::Severity::Warn, "12/8 BIT ENGINE: NOT SAVED AS A .PTI");
    case SaveService::Start::PtiLevel: return say(ctx, core::Severity::Warn, "LEVEL TOO LOW FOR A .PTI (1.1.14 LAW)");
    case SaveService::Start::BadValues: return say(ctx, core::Severity::Warn, "VALUES NOT SAVEABLE (A PARAMETER IS NOT A NUMBER)");  // OS-MODPRESET P3
  }
  return false;
}

// F4 SAVE: the project the user is in, into its own folder (the previous file becomes project.o3pj.bak); a project that came from outside Projects/ is saved as a new one.
bool file_save(ActionContext& ctx, core::ActionArg) noexcept { return save_project(ctx, false); }

// OS-UXF-KEYS 1a (new Community OS behaviour): Shift + File on every screen is File's F4 SAVE (the same job, the same refusals, the same strip). A project no folder holds yet (new or imported) has no name to
// save under, so the save-as keyboard opens instead of an automatic "Track NN". file_save_as makes the refusals that need no name first.
bool file_save_any(ActionContext& ctx, core::ActionArg arg) noexcept {
  const char* folder = ctx.project != nullptr && ctx.project->loaded() ? ctx.project->folder() : "";
  if (ctx.saves != nullptr && ctx.saves->attached() && !ctx.saves->has_folder(folder)) {
    if (ctx.saves->busy()) {  // the first save of a nameless project is running: no keyboard for a save that cannot start
      return say(ctx, core::Severity::Info, "A SAVE IS ALREADY RUNNING");
    }
    return file_save_as(ctx, arg);
  }
  return save_project(ctx, false);
}

// F5 SAVE AS (OS-FU2): the on-screen keyboard opens with the first free "Track NN" for a name, all of it selected; F1 saves a new
// folder of that name (text.accept), F6 puts the automatic name back, F8 cancels. Nothing is ever replaced. The refusals that need no name (no card write layer, no writable
// card, a load running) come first, so that nobody types a name for a save that cannot go.
bool file_save_as(ActionContext& ctx, core::ActionArg) noexcept {
  bool said = false;
  if (!project_ready(ctx, said)) {
    return said;
  }
  if (ctx.saves->gate() != hal::storage::WriteStatus::Ok) {
    return say(ctx, core::Severity::Warn, SaveService::gate_text(ctx.saves->gate()));
  }
  char name[hal::storage::save::kNameMax + 1] = {};
  // OS-NAMES: a project that no folder holds yet (new or imported) offers its title as the folder's name, reduced to the characters a name takes; F6 still gives the automatic one.
  const bool has_folder = ctx.project != nullptr && ctx.project->loaded() && ctx.project->folder()[0] != '\0';
  if (has_folder || text_file_name(ctx.model.project->name(), name, sizeof name) == 0u || !SaveService::project_name_ok(name)) {
    name[0] = '\0';
    if (!ctx.saves->suggest_name(name, sizeof name)) {
      name[0] = '\0';  // every "Track NN" is taken: the user types one
    }
  }
  TextEntryView& t = ctx.view.text;
  t.entry.begin(name, hal::storage::save::kNameMax, TextEntry::Filter::FileName);
  t.target = TextEntryView::Target::SaveAs;
  t.message = "";
  ctx.view.overlay = OverlayId::TextEntry;
  return true;
}

// OS-NAMES: the keyboard over a track's, a pattern's or the project's name, which the Mixer, the Song and the File screen's Shift keys open. The stored field decides what can be typed (its limit, every key);
// there is nothing to write a name into without a document.
bool name_open(ActionContext& ctx, TextEntryView::Target target, unsigned subject, const char* current, unsigned cap) noexcept {
  if (ctx.model.project == nullptr || ctx.session == nullptr || ctx.session->document() == nullptr) {
    return say(ctx, core::Severity::Info, "NO PROJECT TO NAME");
  }
  TextEntryView& t = ctx.view.text;
  t.entry.begin(current, cap, TextEntry::Filter::Any);
  t.target = target;
  t.subject = static_cast<std::uint8_t>(subject);
  t.message = "";
  ctx.view.overlay = OverlayId::TextEntry;
  return true;
}

// F8 in the keyboard for those targets: the typed name replaces the stored one through its Editor setter, one undo record (the same name records nothing; an empty one clears the name).
bool name_accept(ActionContext& ctx) noexcept {
  TextEntryView& t = ctx.view.text;
  const TextEntryView::Target target = t.target;
  const unsigned subject = t.subject;
  ctx.view.overlay = OverlayId::None;
  t.target = TextEntryView::Target::None;
  if (ctx.edit == nullptr) {
    return true;
  }
  const char* name = t.entry.text();
  const std::size_t len = t.entry.length();
  core::FixedString<core::kNotifyTextChars> text;
  model::Status st = model::Status::BadValue;
  switch (target) {
    case TextEntryView::Target::Track:
      ctx.edit->set_final(true);  // a track's name is the Mixer's: final, no undo unit (OS-UNDOGRP D10)
      st = ctx.edit->set_track_name(subject, name, len);
      ctx.edit->set_final(false);
      text.assign("TRACK ");
      append_dec(text, subject + 1u, 2);
      break;
    case TextEntryView::Target::Pattern:
      st = ctx.edit->set_pattern_name(subject, name, len);
      text.assign("PATTERN ");
      append_dec(text, subject, 2);
      break;
    case TextEntryView::Target::Project:
      st = ctx.edit->set_name(name, len);
      text.assign("PROJECT");
      break;
    default: break;
  }
  if (st != model::Status::Ok) {
    return say(ctx, core::Severity::Warn, "NAME REFUSED");
  }
  text.append(len != 0u ? " NAMED" : " NAME CLEARED");
  return say(ctx, core::Severity::Info, text.c_str());
}

// OS-MODPRESET P4: the keyboard for a preset's name (TextEntryView::Target::Preset): Shift + F5 on a bound module's parameter page (instrument_save) or in the F5 PRESET list (preset_save).
// The suggested name is the instrument's name, or the effect's display name, sanitised as a file stem; the filter is the FileName one and the cap kPresetNameMax (the guard's preset rule).
bool preset_name_open(ActionContext& ctx) noexcept {
  const ParamTarget target = params_target(ctx.view);
  if (ctx.model.modules == nullptr || ctx.model.project == nullptr) {
    return say(ctx, core::Severity::Info, "NO MODULE SYSTEM IN THIS BUILD");
  }
  const o3m_module* m = ctx.model.modules->bound_module(*ctx.model.project, target);
  if (m == nullptr || ctx.model.modules->param_count(*ctx.model.project, target) == 0u) {
    return say(ctx, core::Severity::Info, "NO MODULE HERE");
  }
  if (ctx.saves == nullptr || !ctx.saves->attached() || ctx.session == nullptr) {
    return say(ctx, core::Severity::Warn, "NO CARD WRITE LAYER");
  }
  if (ctx.saves->gate() != hal::storage::WriteStatus::Ok) {  // as Save As: nobody types a name for a save that cannot go
    return say(ctx, core::Severity::Warn, SaveService::gate_text(ctx.saves->gate()));
  }
  char name[kPresetNameMax + 1] = {};
  if (target.insert) {
    const char* display = m->name != nullptr && m->name[0] != '\0' ? m->name : ModuleUi::module_name_of(*ctx.model.project, target);
    SaveService::instrument_file_stem(display, 0, name, sizeof name);
  } else {
    const model::Instrument* in = target.slot < model::kMaxInstruments ? ctx.model.project->instrument(target.slot) : nullptr;
    SaveService::instrument_file_stem(in != nullptr ? in->name : "", target.slot, name, sizeof name);
  }
  TextEntryView& t = ctx.view.text;
  t.entry.begin(name, kPresetNameMax, TextEntry::Filter::FileName);
  t.target = TextEntryView::Target::Preset;
  t.message = "";
  ctx.view.overlay = OverlayId::TextEntry;
  return true;
}

// F8 in the keyboard for a preset (Target::Preset, OS-MODPRESET P4): the typed name goes to SaveService::save_preset with the values in force (ModuleUi::capture_preset), as a NewOnly .o3mp in
// modules/presets/<stem>/; a taken name got the next free one and the strip says the name the file got. A name the card cannot take stays in the keyboard with the reason.
bool preset_accept(ActionContext& ctx) noexcept {
  TextEntryView& t = ctx.view.text;
  if (t.entry.length() == 0u) {
    t.message = "TYPE A NAME";
    return true;
  }
  if (!SaveService::project_name_ok(t.entry.text())) {  // the guard's rule; the entry's cap is kPresetNameMax
    t.message = "NOT A NAME: LETTERS, DIGITS, SPACE, -";
    return true;
  }
  if (ctx.saves == nullptr || !ctx.saves->attached() || ctx.session == nullptr || ctx.model.modules == nullptr || ctx.model.project == nullptr) {
    ctx.view.overlay = OverlayId::None;
    t.target = TextEntryView::Target::None;
    return say(ctx, core::Severity::Warn, "NO CARD WRITE LAYER");
  }
  formats::preset::Preset p;
  const bool captured = ctx.model.modules->capture_preset(*ctx.model.project, params_target(ctx.view), p);
  char final_name[kPresetNameMax + 8] = {};
  const SaveService::Start start = captured ? ctx.saves->save_preset(ctx.session->jobs, p, t.entry.text(), final_name, sizeof final_name) : SaveService::Start::Empty;
  if (start == SaveService::Start::BadName) {
    t.message = "NOT A NAME THE CARD TAKES";  // the keyboard stays open
    return true;
  }
  ctx.view.overlay = OverlayId::None;
  t.target = TextEntryView::Target::None;
  if (start == SaveService::Start::Started) {
    core::FixedString<core::kNotifyTextChars> text;  // "PRESET SAVED: <NAME>": the name the file got, upper case (a taken name became "<name> 2")
    text.assign("PRESET SAVED: ");
    for (unsigned i = 0; final_name[i] != '\0' && final_name[i] != '.' && text.size() + 1u < core::kNotifyTextChars; ++i) {
      const char c = final_name[i];
      const char up[2] = {static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c), '\0'};
      text.append(up);
    }
    (void)say(ctx, core::Severity::Info, text.c_str());
    return true;
  }
  if (!captured) {
    return say(ctx, core::Severity::Info, "NO MODULE HERE");
  }
  return save_report(ctx, start);
}

// F8 in the keyboard: the typed name goes to the save. A name the card cannot take, or that is taken, is said inside the keyboard and it stays open; any other answer closes it
// and the strip says how the save went.
bool text_accept(ActionContext& ctx, core::ActionArg) noexcept {
  TextEntryView& t = ctx.view.text;
  if (ctx.view.overlay == OverlayId::TextEntry && t.target == TextEntryView::Target::Instrument) {
    return instrument_rename_accept(ctx);  // OS-INSTUI S7: the instrument's name (an undo unit)
  }
  if (ctx.view.overlay == OverlayId::TextEntry && t.target == TextEntryView::Target::Preset) {
    return preset_accept(ctx);  // OS-MODPRESET P4: the preset's file name
  }
  if (ctx.view.overlay == OverlayId::TextEntry && (t.target == TextEntryView::Target::Track || t.target == TextEntryView::Target::Pattern || t.target == TextEntryView::Target::Project)) {
    return name_accept(ctx);  // OS-NAMES: a track's, a pattern's or the project's name (an undo unit)
  }
  if (ctx.view.overlay != OverlayId::TextEntry || t.target != TextEntryView::Target::SaveAs) {
    return false;
  }
  if (t.entry.length() == 0u) {
    t.message = "TYPE A NAME";  // OS-SILENTMSG: an empty name is not a name for a project (the Mixer's, Song's and File's names take an empty one: it clears them)
    return true;
  }
  if (!SaveService::project_name_ok(t.entry.text())) {
    t.message = "NOT A NAME: LETTERS, DIGITS, SPACE, - _ .";
    return true;
  }
  bool said = false;
  if (!project_ready(ctx, said)) {
    ctx.view.overlay = OverlayId::None;
    return true;
  }
  const char* folder = ctx.project != nullptr && ctx.project->loaded() ? ctx.project->folder() : "";
  const SaveService::Start start = ctx.saves->save_project(ctx.session->jobs, *ctx.model.project, folder, true, t.entry.text(), ctx.model.samples);
  if (start == SaveService::Start::NameTaken) {
    t.message = "A PROJECT OF THAT NAME EXISTS: TYPE ANOTHER";
    return true;
  }
  if (start == SaveService::Start::BadName) {
    t.message = "NOT A NAME THE CARD TAKES";
    return true;
  }
  ctx.view.overlay = OverlayId::None;
  t.target = TextEntryView::Target::None;
  (void)save_report(ctx, start);
  return true;
}

// Shift + F5 on the Instrument screen (OS-INSTUI S7): the sample instrument in view and its audio as instruments/<name>.pti (the OG's instrument format, SaveService::save_instrument), never over
// an existing file. OS-MODPRESET P4: on a bound module's parameter page — a module instrument's or an insert's — the same key opens the keyboard for a preset's name instead, and the save is a
// .o3mp (preset_name_open / preset_accept). A MIDI instrument says so; an insert whose module is not bound has a blank SAVE; the card's gate (no card, write protected, exFAT) and the other refusals
// say why on the strip like every save.
bool instrument_save(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen != ScreenId::Instrument) {
    return false;
  }
  const ParamTarget target = params_target(ctx.view);
  if (ctx.model.modules != nullptr && ctx.model.project != nullptr && ctx.model.modules->bound_module(*ctx.model.project, target) != nullptr &&
      ctx.model.modules->param_count(*ctx.model.project, target) != 0u) {
    return preset_name_open(ctx);  // OS-MODPRESET P4: a bound module's page saves a preset
  }
  if (params_insert_target(ctx.view)) {  // OS-INSFX S5: an insert whose module is not bound
    return false;
  }
  const unsigned slot = static_cast<unsigned>(ctx.view.instrument < 1 ? 0 : ctx.view.instrument - 1);
  const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(slot) : nullptr;
  if (in == nullptr) {
    return say(ctx, core::Severity::Info, "NOTHING TO SAVE");
  }
  if (in->kind == model::InstrumentKind::Module) {
    return say(ctx, core::Severity::Info, "NO MODULE HERE");  // a module instrument whose module is not bound
  }
  if (in->kind != model::InstrumentKind::Sample) {
    return say(ctx, core::Severity::Info, "SAMPLE INSTRUMENTS ONLY");
  }
  if (ctx.saves == nullptr || !ctx.saves->attached() || ctx.session == nullptr || ctx.model.samples == nullptr) {
    return say(ctx, core::Severity::Warn, "NO CARD WRITE LAYER");
  }
  const SampleBank::Pcm pcm = ctx.model.samples->pcm(slot);
  if (pcm.data == nullptr || pcm.frames == 0u) {
    return say(ctx, core::Severity::Info, "NO AUDIO IN THIS INSTRUMENT");
  }
  const SaveService::Start start = ctx.saves->save_instrument(ctx.session->jobs, *ctx.model.project, *ctx.model.samples, slot);
  const bool reported = save_report(ctx, start);
  const std::uint32_t from = ctx.saves->pti_resampled_hz();  // (0 on every refusal: save_instrument clears it first)
  const bool mid = start == SaveService::Start::Started && ctx.model.samples->edit_stereo(slot);  // OS-STEREO (e): a stereo instrument's file holds its mid M (a mono .pti); S is not in it
  if (start == SaveService::Start::Started && from != 0u) {  // OS-FILES-C C1b: the file is at 44,100 Hz, the instrument was not
    core::FixedString<core::kNotifyTextChars> text;
    text.assign("RESAMPLED ");
    append_uint(text, from);
    text.append(" -> 44100 HZ");
    text.append(mid ? ", MID ONLY" : "");
    return say(ctx, core::Severity::Info, text.c_str());
  }
  if (mid) {
    return say(ctx, core::Severity::Info, "STEREO: MID WRITTEN, SIDE NOT KEPT");
  }
  return reported;
}

// F7 on the Record screen: the take's window (the trim) as a 16-bit mono WAV in Samples/, at 44.1 kHz, the rate of an OG folder's files.
bool record_wav(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen != ScreenId::Record) {
    return false;
  }
  if (ctx.rec == nullptr || ctx.saves == nullptr || !ctx.saves->attached()) {
    return say(ctx, core::Severity::Warn, "NO CARD WRITE LAYER");
  }
  const Recorder& rec = *ctx.rec;
  if (rec.state() != Recorder::State::Review) {
    return say(ctx, core::Severity::Info, "NO TAKE TO SAVE: F1 RECORDS ONE");
  }
  WavRequest rq;
  rq.pcm = rec.take_data() + rec.trim_start();
  rq.frames = rec.trim_end() - rec.trim_start();
  rq.channels = 1;
  rq.rate = 44100;
  rq.prefix = "REC";
  rq.take = &rec;
  return save_report(ctx, ctx.saves->save_wav(ctx.session->jobs, rq));  // (E-SREC-09: the take counts as saved when the job ends Ok: App::run_once, SaveService::take_written)
}

// Shift + F7 on the Sample edit screen: the sample of the instrument being edited, as it is now (the edits are in it), as a 16-bit mono WAV in Samples/ at the instrument's rate.
bool sedit_save_wav(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen != ScreenId::SampleEdit) {
    return false;
  }
  if (ctx.saves == nullptr || !ctx.saves->attached() || ctx.session == nullptr) {
    return say(ctx, core::Severity::Warn, "NO CARD WRITE LAYER");
  }
  const unsigned slot = wave_slot(ctx.view);
  const SampleBank::Pcm pcm = ctx.model.samples != nullptr ? ctx.model.samples->pcm(slot) : SampleBank::Pcm{};
  const model::Instrument* in = ctx.model.project != nullptr ? ctx.model.project->instrument(slot) : nullptr;
  if (pcm.frames == 0u || in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return say(ctx, core::Severity::Info, "NO SAMPLE IN THIS INSTRUMENT");
  }
  WavRequest rq;
  rq.pcm = pcm.data;
  rq.frames = pcm.frames;
  rq.channels = 1;
  rq.rate = in->sample_rate != 0u ? in->sample_rate : 44100u;  // 0: 44,100 Hz, not recorded (native INST 1.1)
  rq.prefix = "SMP";
  rq.bank = ctx.model.samples;
  rq.slot = slot;
  return save_report(ctx, ctx.saves->save_wav(ctx.session->jobs, rq));
}

}  // namespace o3os::app
