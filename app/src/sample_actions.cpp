// SPDX-License-Identifier: GPL-3.0-or-later
// Sample load actions (OS-011a): what the Sample load screen's F-keys do. They only choose and ask: the card, the jobs and the sample arena are the App's (its
// pass reads WavView::go), as the File screen's rescan is.
#include "action_handlers.h"

#include <o3os/app/key_prompt.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/text_format.h>
#include <o3os/app/screens/file_screen.h>

#include <algorithm>

namespace o3os::app {

// The first instrument slot (0-based) the project has nothing in; -1 when all are used (Sample load's target, and the Sample edit plugin page's NEW INS).
int first_free_slot(const model::Project& project) noexcept {
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    if (project.instrument(i) == nullptr) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

namespace {

// OS-USBDISK 4.3: while the computer has the card the news is asked, ahead of the persistent disk strip (save_actions.cpp).
bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return ctx.view.card_on_computer ? ctx.view.notify.ask(severity, text, "sample", ViewState::kMessageMs) : ctx.view.notify.upsert(severity, text, "sample", ViewState::kMessageMs);
}

// OS-AUDIT-A7: the listing is still being read (or not started): its rows move as entries arrive, so a mark by row is not safe yet. Cancelled / failed lists stay as they are.
bool list_unsettled(const DirScan& scan) noexcept {
  return scan.state() == DirScan::State::Scanning || scan.state() == DirScan::State::Idle;
}

}  // namespace

// F1 LOAD / F4 ADD (arg 0 / 1): the WAV under the cursor goes to the target instrument (the selected one), or to the first free one.
bool sample_load(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.card_on_computer) {  // OS-USBDISK 4.3: LOAD and ADD alike, whatever state the listing is in
    return say(ctx, core::Severity::Warn, "CARD ON COMPUTER");
  }
  if (ctx.model.samples == nullptr || !ctx.model.samples->ready()) {
    return say(ctx, core::Severity::Info, "NO SAMPLE MEMORY");
  }
  const DirScan* scan = ctx.model.files;
  if (scan == nullptr || scan->state() == DirScan::State::NoCard) {
    return say(ctx, core::Severity::Warn, ctx.view.card_off_text("THE CARD IS NOT MOUNTED"));
  }
  const DirIndex& index = scan->index();
  if (!ctx.view.wav.pti && ctx.view.wav.mark_count() != 0u) {  // OS-AUDIT-A7 (SLD-MULTI): files are marked: F1 / F4 load those (the cursor's file only when it is marked), from the target / the first free instrument on
    if (list_unsettled(*scan)) {
      return say(ctx, core::Severity::Info, "LIST STILL READING");
    }
    if (ctx.model.project == nullptr) {
      return say(ctx, core::Severity::Info, "NO PROJECT OPEN");
    }
    ctx.view.wav.slot = arg == 1 ? std::uint8_t{0} : static_cast<std::uint8_t>(std::clamp(ctx.view.instrument - 1, 0, static_cast<int>(model::kMaxInstruments) - 1));
    ctx.view.wav.go = WavView::Go::LoadMarked;
    return true;
  }
  const int pos = ctx.view.wav.list.pos();
  if (pos < 0 || static_cast<unsigned>(pos) >= index.count()) {
    return say(ctx, core::Severity::Info, "NO FILE TO LOAD");
  }
  if (index.is_dir(static_cast<unsigned>(pos))) {
    return say(ctx, core::Severity::Info, "A FOLDER: PRESS ENTER");
  }
  if (ctx.model.project == nullptr) {
    return say(ctx, core::Severity::Info, "NO PROJECT OPEN");
  }
  const bool pti = ctx.view.wav.pti;  // OS-INSTUI S7: the list is of instrument files, which go to the slot in view
  if (pti && (arg == 1 || !DirScan::ends_with_ci(index.name(static_cast<unsigned>(pos)), ".pti"))) {
    return say(ctx, core::Severity::Info, arg == 1 ? "ADD IS FOR SAMPLE FILES" : "NOT AN INSTRUMENT FILE (.PTI)");
  }
  int slot = ctx.view.instrument - 1;
  if (arg == 1) {
    slot = first_free_slot(*ctx.model.project);
    if (slot < 0) {
      return say(ctx, core::Severity::Warn, "NO FREE INSTRUMENT");
    }
  }
  if (slot < 0 || slot >= static_cast<int>(model::kMaxInstruments)) {
    return false;
  }
  const model::Instrument* cur = ctx.model.project->instrument(static_cast<std::uint32_t>(slot));
  if (cur != nullptr && cur->kind == model::InstrumentKind::Module) {  // the module rule of PASTE (D9), for a WAV too (OS-BUG-SLDOVER): a module instance cannot be replaced, delete the instrument first
    return say(ctx, core::Severity::Info, "SLOT HAS A MODULE: INSTRUMENT, SHIFT+F4 DELETE");
  }
  if (cur != nullptr && cur->kind == model::InstrumentKind::Midi) {  // OS-QAFIX-E-LOAD-25 (OS-INSTMIDI: any slot can be any type): a MIDI instrument is not replaced by a sample either; an empty or sample slot, 49..64 too, is a target
    return say(ctx, core::Severity::Info, "SLOT IS MIDI: INSTRUMENT, SHIFT+F4 DELETE");
  }
  if (!pti && cur != nullptr) {  // OS-BUG-SLDOVER: a WAV over a sample instrument that is there asks first (a .pti load is an undo unit; a free slot, and ADD, ask nothing)
    WavView& w = ctx.view.wav;
    const bool answer = w.ask_pos == pos && w.ask_slot == slot && core::tick_elapsed(ctx.now, w.asked_at) <= core::ticks_from_ms(WavView::kAskMs) && !w.ask_text.empty() && !ctx.view.notify.empty() &&
                        ctx.view.notify.head().text == w.ask_text.c_str();
    if (!answer) {
      AskText text;
      core::FixedString<20> head;
      head.assign("REPLACE ");
      append_dec(head, static_cast<unsigned>(slot) + 1u, 2);
      head.append(" ");
      append_clipped(head, cur->name, 14);
      ask_text(Ask::Replace, head.c_str(), text);
      w.ask_pos = pos;
      w.ask_slot = static_cast<std::uint8_t>(slot);
      w.asked_at = ctx.now;
      w.ask_text.assign(text.c_str());
      return ctx.view.notify.ask(core::Severity::Warn, text.c_str(), "sample", WavView::kAskMs);
    }
    ctx.view.notify.dismiss("sample", w.ask_text.c_str());
    w.ask_pos = -1;
    w.ask_text.clear();
  }
  ctx.view.wav.go = WavView::Go::Load;
  ctx.view.wav.slot = static_cast<std::uint8_t>(slot);
  return true;
}

// Shift + Right / Shift + Left (OS-AUDIT-A7, SLD-MULTI; arg 0 / 1): mark the file under the cursor, or take its mark back, and step down one row (so a run of files is marked by repeating the key); clear
// every mark. The official firmware marks with Shift + the arrows (E-LOAD-14); here Shift + Up / Down are the page keys (OS-NAV3), so the Left / Right pair, which are free with Shift, carry it (new Community OS behaviour).
bool sample_mark(ActionContext& ctx, core::ActionArg arg) noexcept {
  WavView& w = ctx.view.wav;
  if (arg == 1) {
    if (w.mark_count() == 0u) {
      return say(ctx, core::Severity::Info, "NO FILE MARKED");
    }
    w.clear_marks();
    return say(ctx, core::Severity::Info, "MARKS CLEARED");
  }
  if (w.pti) {
    return say(ctx, core::Severity::Info, "MARKS ARE FOR SAMPLE FILES");
  }
  if (w.back_to_params) {  // the type chooser sent us here for one instrument: the load goes back to it after the first file
    return say(ctx, core::Severity::Info, "ONE FILE FOR THIS INSTRUMENT");
  }
  const DirScan* scan = ctx.model.files;
  if (scan != nullptr && list_unsettled(*scan)) {  // marks are bits by list row and the listing is sorted as entries arrive: a mark set now could end up on another file
    return say(ctx, core::Severity::Info, "LIST STILL READING");
  }
  const int pos = w.list.pos();
  if (scan == nullptr || scan->state() == DirScan::State::NoCard || pos < 0 || static_cast<unsigned>(pos) >= scan->index().count()) {
    return say(ctx, core::Severity::Info, "NO FILE TO MARK");
  }
  if (scan->index().is_dir(static_cast<unsigned>(pos))) {
    return say(ctx, core::Severity::Info, "A FOLDER: PRESS ENTER");
  }
  w.set_mark(static_cast<unsigned>(pos), !w.marked(static_cast<unsigned>(pos)));
  (void)w.list.move(1, scan->index());
  return true;
}

// F3 ENTER: the folder under the cursor becomes the listing.
bool sample_enter(ActionContext& ctx, core::ActionArg) noexcept {
  const DirScan* scan = ctx.model.files;
  const int pos = ctx.view.wav.list.pos();
  if (scan == nullptr || pos < 0 || static_cast<unsigned>(pos) >= scan->index().count() || !scan->index().is_dir(static_cast<unsigned>(pos))) {
    return say(ctx, core::Severity::Info, "NOT A FOLDER");
  }
  ctx.view.wav.go = WavView::Go::Enter;
  return true;
}

// F2 UP: the folder above (the card's root has none).
bool sample_up(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.wav.folder[0] == '\0') {
    return say(ctx, core::Severity::Info, "ALREADY AT THE TOP");
  }
  ctx.view.wav.go = WavView::Go::Up;
  return true;
}

// F6 RESCAN.
bool sample_rescan(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.wav.list.rescan = true;
  return true;
}

// F7 WT (OS-GRAN): the way a WAV is read as a wavetable, round: AUTO, LEN, 256, 512, 1024, 2048, OFF (sample_load_view.h). Applies to the next LOAD / ADD.
bool sample_wt(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.wav.wavetable = next_wavetable_mode(ctx.view.wav.wavetable);
  return say(ctx, core::Severity::Info, wavetable_notice(ctx.view.wav.wavetable));
}

// F8 CH (OS-LOADCH): which channel of a stereo file is loaded, round: MIX, LEFT, RIGHT. The key only acts on a stereo file (the one under the cursor); the choice stays for the session.
bool sample_channel(ActionContext& ctx, core::ActionArg) noexcept {
  const DirScan* scan = ctx.model.files;
  const int pos = ctx.view.wav.list.pos();
  if (scan == nullptr || pos < 0 || !scan->index().stereo(static_cast<unsigned>(pos))) {
    return say(ctx, core::Severity::Info, "ONLY FOR A STEREO FILE");
  }
  ctx.view.wav.channel = next_channel(ctx.view.wav.channel);
  return say(ctx, core::Severity::Info, channel_notice(ctx.view.wav.channel));
}

// Shift + F7 / F8 (OS-SLDTARGET, new Community OS behaviour): the target instrument one back / on, 1..64 round (the Instrument screen's range: any slot can be any type, OS-QAFIX-E-LOAD-25),
// the header and the card follow the view's instrument.
bool sample_target(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg == 0) {
    return false;
  }
  constexpr int kSlots = static_cast<int>(model::kMaxInstruments);
  int next = ctx.view.instrument + (arg < 0 ? -1 : 1);
  next = next < 1 ? kSlots : (next > kSlots ? 1 : next);
  WavView& w = ctx.view.wav;
  if (w.ask_pos >= 0 && !w.ask_text.empty()) {  // a REPLACE question was about the old target: it lapses with it
    ctx.view.notify.dismiss("sample", w.ask_text.c_str());
    w.ask_pos = -1;
    w.ask_text.clear();
  }
  release_live_pads(ctx);  // as the Instrument screen's step: instrument_select (the pads' action) only reaches 1..48
  ctx.view.instrument = next;
  return true;
}

// F5 PLAY (OS-FU2): the audition of the WAV under the cursor, for as long as the key is held. The App starts the read and plays
// it through the engine's live notes (App::start_audition); the key's release, a move of the cursor or leaving the screen ends it.
bool sample_play(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.card_on_computer) {  // OS-USBDISK 4.3: ahead of the preview memory and the listing, as LOAD and ADD
    return say(ctx, core::Severity::Warn, "CARD ON COMPUTER");
  }
  if (ctx.model.samples == nullptr || !ctx.model.samples->preview_ready()) {
    return say(ctx, core::Severity::Info, "NO PREVIEW MEMORY");
  }
  const DirScan* scan = ctx.model.files;
  if (scan == nullptr || scan->state() == DirScan::State::NoCard) {
    return say(ctx, core::Severity::Warn, ctx.view.card_off_text("THE CARD IS NOT MOUNTED"));
  }
  const int pos = ctx.view.wav.list.pos();
  if (ctx.view.wav.pti) {
    return say(ctx, core::Severity::Info, "NO PREVIEW OF AN INSTRUMENT FILE");
  }
  if (pos < 0 || static_cast<unsigned>(pos) >= scan->index().count()) {
    return say(ctx, core::Severity::Info, "NO FILE TO PLAY");
  }
  if (scan->index().is_dir(static_cast<unsigned>(pos))) {
    return say(ctx, core::Severity::Info, "A FOLDER: PRESS ENTER");
  }
  if (DirScan::ends_with_ci(scan->index().name(static_cast<unsigned>(pos)), ".wt")) {
    return say(ctx, core::Severity::Info, "NO PREVIEW OF A .WT: LOAD IT");  // (OS-GRAN: the audition reads a WAV)
  }
  if (!DirScan::ends_with_ci(scan->index().name(static_cast<unsigned>(pos)), ".wav")) {
    return say(ctx, core::Severity::Info, "NO PREVIEW OF THIS TYPE: LOAD IT");  // (OS-SAMPFMT: AIFF and 8SVX are read by the load only)
  }
  ctx.view.wav.audition_held = true;
  ctx.view.wav.go = WavView::Go::Preview;
  return true;
}

}  // namespace o3os::app
