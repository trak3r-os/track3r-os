// SPDX-License-Identifier: GPL-3.0-or-later
// The firmware update dialog (OS-FWCONFIG slice 2a, firmware_view.h): the keys that open it, answer its two questions and leave it, and the App's pass that follows the check and the hand-over.
// The update path is hal::firmware::FirmwareSink and nothing else: no card access, no pin, no flash here. The check, the second check and the hand-over are the console's own code behind the
// port; this file only asks for them in order, within the rules of the spec (two questions, each answered by F1 alone, nothing written before INSTALL, cancel() on every way out).
#include "action_handlers.h"

#include <o3os/app/firmware_view.h>
#include <o3os/app/recorder.h>
#include <o3os/app/screens/config_screen.h>
#include <o3os/app/text_format.h>

#include <cstdint>

namespace o3os::app {

namespace {

using hal::firmware::CheckStatus;
using hal::firmware::FirmwareSink;
using hal::firmware::Refusal;

// Every text is a refusal or a failure of one step and fits the dialog's line (FirmwareView::kMsgChars) and the strip (48 characters). NoPath is never asked: open() returns before it when available() is false.
// The texts that two cases share are one string.
constexpr const char* kCheckAgain = "CHECK AGAIN";
constexpr const char* kCheckExpired = "THE CHECK EXPIRED: CHECK AGAIN";

const char* refusal_text(Refusal r) noexcept {
  switch (r) {
    case Refusal::None:
    case Refusal::NoPath: break;
    case Refusal::OnComputer: return "THE CARD IS ON THE COMPUTER";
    case Refusal::StorageBusy: return "THE CARD IS BUSY: TRY AGAIN";
    case Refusal::Busy: return "ANOTHER UPDATE STEP IS RUNNING";
    case Refusal::NoCard: return "NO CARD";
    case Refusal::NotWritable: return "THE CARD CANNOT BE USED FOR AN UPDATE";
    case Refusal::NoFolder: return "NO FIRMWARE FOLDER ON THE CARD";
    case Refusal::NotFound: return "THE FILE IS NOT ON THE CARD ANY MORE";
    case Refusal::BadName: return "THE UPDATE CANNOT TAKE THIS FILE NAME";
    case Refusal::NotChecked: return kCheckAgain;
    case Refusal::Expired: return kCheckExpired;
    case Refusal::Io: return "CARD READ ERROR";
  }
  return "";
}

// The list again, with `text` on the dialog's line ("" for none). The sink's job is over or was never started: whoever calls this has already told it to cancel() when it must.
void to_list(FirmwareView& v, const char* text) noexcept {
  v.phase = FirmwareView::Phase::List;
  v.bad = text[0] != '\0';
  v.msg.assign(text);
  v.done = v.total = 0;
}

// A step of the walk (a question, the check, the hand-over): nothing is left on the line or the bar.
void step(FirmwareView& v, FirmwareView::Phase phase) noexcept {
  to_list(v, "");
  v.phase = phase;
}

// The list again with a failure of the sink on the line: "<head><FAULT>", and " AT <offset>" for a fault found in the file. The fault is the sink's static text, upper-cased with the export's own loop.
void to_failure(FirmwareView& v, const char* head, const char* fault, const std::uint32_t* offset) noexcept {
  char upper[FirmwareView::kMsgChars + 1];
  upper_copy(upper, sizeof upper, fault != nullptr ? fault : "");
  to_list(v, head);
  v.bad = true;
  v.msg.append(upper);
  if (offset != nullptr) {
    v.msg.append(" AT ");
    append_uint(v.msg, *offset);
  }
}

// What stops an update right now, as the strip says it, or nullptr: an export or a recording is running, or the project has unsaved changes. Asked when the dialog opens and again at INSTALL.
const char* blocked(ActionContext& ctx) noexcept {
  if (ctx.view.xport.running || (ctx.saves != nullptr && ctx.saves->og_state() == SaveService::OgState::Running) || (ctx.rec != nullptr && ctx.rec->state() != Recorder::State::Idle)) {
    return "AN EXPORT OR A RECORDING IS RUNNING";
  }
  if (ctx.saves != nullptr && ctx.model.project != nullptr && ctx.saves->dirty(*ctx.model.project)) {
    return "SAVE THE PROJECT FIRST";
  }
  return nullptr;
}

// One pass over the folder, after a list_begin() that said None: the window of kMax entries that starts at the folder's index `first` is copied (the entries before it are read and dropped: the
// card has one directory handle and no seek), the entries are counted, and list_end() closes the pass. Bounded: at most kScanMax list_next() calls, kMax copies.
void scan(FirmwareView& v, FirmwareSink& sink, unsigned first) noexcept {
  v.n = 0;
  unsigned i = 0;
  hal::firmware::File f;
  while (i < FirmwareView::kScanMax && sink.list_next(f)) {
    if (i >= first && v.n < FirmwareView::kMax) {
      v.files[v.n++] = f;
    }
    ++i;
  }
  sink.list_end();
  v.first = static_cast<std::uint16_t>(first);
  v.entries = static_cast<std::uint16_t>(i);
}

// The window the cursor was sent to (v.want, outside the one held) is loaded: the folder is scanned again. The windows are multiples of kMax, so moving down past the last entry of a full window
// lands on the first of the next, and moving up past the first lands on the last of the one before. A folder that lost files since the last pass ends the cursor on its last entry; a card that cannot
// give the list now keeps the window as it was and says why on the line.
void load_want(FirmwareView& v, FirmwareSink& sink) noexcept {
  unsigned target = v.want;
  v.want = FirmwareView::kNoWant;
  to_list(v, "");
  for (unsigned attempt = 0; attempt < 2; ++attempt) {  // (the second only when the folder shrank under the wanted window)
    const Refusal why = sink.list_begin();
    if (why != Refusal::None) {
      to_list(v, refusal_text(why));
      v.bad = true;
      if (attempt != 0) {  // (the first pass had already replaced the window with an empty one)
        v.n = 0;
        v.cursor = 0;
        v.first = 0;
        v.entries = 0;
      }
      return;
    }
    const unsigned first = target / FirmwareView::kMax * FirmwareView::kMax;
    scan(v, sink, first);
    if (v.n != 0 || v.entries == 0) {
      break;
    }
    target = v.entries - 1u;
  }
  v.cursor = v.n == 0 ? 0 : static_cast<std::uint8_t>(target - v.first < v.n ? target - v.first : v.n - 1u);
}

void leave(ActionContext& ctx) noexcept {
  if (ctx.firmware != nullptr) {
    ctx.firmware->cancel();  // (nothing may be left running or waiting behind a closed dialog)
  }
  ctx.view.firmware.close();
  if (ctx.view.overlay == OverlayId::FirmwareUpdate) {
    ctx.view.overlay = OverlayId::None;
  }
}

}  // namespace

// F8 UPDATE on the INFO page of Config. The refusals say why on the strip (the dialog is not up yet): an export or a recording is running, the project has unsaved changes, and whatever the update path
// itself refuses (the card is on a computer, storage is busy, a console `fw` operation runs, no card, no Firmware/ folder). The list is copied in this one pass: the card has one directory handle.
bool firmware_open(ActionContext& ctx, core::ActionArg) noexcept {
  FirmwareSink* sink = ctx.firmware;
  if (sink == nullptr || !sink->available() || ctx.view.screen != ScreenId::Config || ctx.view.page[screen_index(ScreenId::Config)] != kConfigPageInfo || ctx.view.overlay != OverlayId::None) {
    return false;
  }
  const auto say = [&ctx](const char* text) noexcept { return ctx.view.notify.upsert(core::Severity::Warn, text, "firmware", ViewState::kMessageMs); };
  if (const char* why = blocked(ctx)) {
    return say(why);
  }
  const Refusal why = sink->list_begin();
  if (why != Refusal::None) {
    return say(refusal_text(why));
  }
  FirmwareView& v = ctx.view.firmware;
  v.close();
  scan(v, *sink, 0);
  v.phase = FirmwareView::Phase::List;
  ctx.view.overlay = OverlayId::FirmwareUpdate;
  return true;
}

// F1 in the dialog: UPDATE on the list asks question 1, YES starts the check, INSTALL starts the hand-over. Nothing is written before the last.
bool firmware_go(ActionContext& ctx, core::ActionArg) noexcept {
  FirmwareSink* sink = ctx.firmware;
  FirmwareView& v = ctx.view.firmware;
  if (sink == nullptr) {
    return false;
  }
  switch (v.phase) {
    case FirmwareView::Phase::List:
      if (v.want != FirmwareView::kNoWant) {  // (the cursor moved out of the window in this very pass: the list shows the entry before it is asked about)
        load_want(v, *sink);
        return true;
      }
      if (v.n == 0) {
        return false;
      }
      step(v, FirmwareView::Phase::AskUpdate);
      return true;
    case FirmwareView::Phase::AskUpdate: {
      const Refusal why = sink->begin_check(v.chosen());
      if (why != Refusal::None) {
        to_list(v, refusal_text(why));  // (a refused check started nothing)
        return true;
      }
      step(v, FirmwareView::Phase::Checking);
      return true;
    }
    case FirmwareView::Phase::AskInstall: {
      if (const char* stop = blocked(ctx)) {  // (the dialog is modal, but a recording or an edit can still come from outside, and 30 s is long: the rules of the opening hold at INSTALL too)
        sink->cancel();
        to_list(v, stop);
        return true;
      }
      const Refusal why = sink->install();
      if (why != Refusal::None) {
        sink->cancel();  // (nothing was touched; the good check, if it is still there, is let go)
        to_list(v, refusal_text(why));
        return true;
      }
      step(v, FirmwareView::Phase::Installing);  // (the hand-over stops the transport and mutes the outputs itself: FwHost::quiesce)
      return true;
    }
    case FirmwareView::Phase::Closed:
    case FirmwareView::Phase::Checking:
    case FirmwareView::Phase::Installing: break;
  }
  return false;
}

// F8 in the dialog: BACK closes the list, CANCEL takes a question or a running check back to the list. Once INSTALL has been taken nothing cancels it.
bool firmware_back(ActionContext& ctx, core::ActionArg) noexcept {
  FirmwareView& v = ctx.view.firmware;
  switch (v.phase) {
    case FirmwareView::Phase::List:
      leave(ctx);
      return true;
    case FirmwareView::Phase::AskUpdate:
      to_list(v, "");
      return true;
    case FirmwareView::Phase::Checking:
    case FirmwareView::Phase::AskInstall:
      if (ctx.firmware != nullptr) {
        ctx.firmware->cancel();
      }
      to_list(v, "");
      return true;
    case FirmwareView::Phase::Closed:
    case FirmwareView::Phase::Installing: break;
  }
  return false;
}

// Does this input take the open question back? Any control but F1 alone (and F8, whose binding does it): another button, Shift + F1 (bound to nothing in the dialog), a pad hit or a turn of the
// encoder. The input then acts as usual in the list (OS-KEYROWDLG: a question is answered by its key and taken back by any other). Shift + F8 is F8: it also steps back.
bool firmware_question_taken_back(const ViewState& view, const core::InputEvent& event) noexcept {
  if (!view.firmware.asking() || view.overlay != OverlayId::FirmwareUpdate) {
    return false;
  }
  switch (event.kind) {
    case core::InputKind::Encoder: return true;
    case core::InputKind::Pad: return event.value > 0;
    case core::InputKind::Button: {
      if (event.value == 0 || event.id == static_cast<std::uint8_t>(Button::Shift) || event.id == static_cast<std::uint8_t>(Button::F8)) {
        return false;
      }
      return event.id != static_cast<std::uint8_t>(Button::F1) || view.shift;
    }
    default: return false;
  }
}

void firmware_question_drop(ViewState& view, FirmwareSink* sink) noexcept {
  if (view.firmware.phase == FirmwareView::Phase::AskInstall && sink != nullptr) {
    sink->cancel();
  }
  to_list(view.firmware, "");
}

// The App's pass, once, while the dialog is open: the check and the install are sliced by poll_check(), one slice a call (2 KiB of the file). Returns true when the picture changed.
bool firmware_poll(ViewState& view, FirmwareSink& sink, core::Tick now) noexcept {
  FirmwareView& v = view.firmware;
  if (!v.open()) {
    return false;
  }
  if (view.overlay != OverlayId::FirmwareUpdate) {  // the overlay was cleared by a path that is not the dialog's own keys (the dialog is modal, so none does today)
    if (v.phase == FirmwareView::Phase::Installing) {  // past INSTALL nothing cancels or closes it, and "DO NOT POWER OFF" stays on the screen
      view.overlay = OverlayId::FirmwareUpdate;
      return true;
    }
    sink.cancel();  // before INSTALL nothing may be left running or waiting behind a closed dialog (a power switch-off is App::power_begin's, which calls firmware_abandon)
    v.close();
    return true;
  }
  if (v.phase == FirmwareView::Phase::List && v.want != FirmwareView::kNoWant) {  // the cursor left the window (FirmwareOverlay::set_position has no sink): the window it went to is read now, one pass
    load_want(v, sink);
    return true;
  }
  if (v.phase != FirmwareView::Phase::Checking && v.phase != FirmwareView::Phase::AskInstall && v.phase != FirmwareView::Phase::Installing) {
    return false;
  }
  const CheckStatus s = sink.poll_check();
  using State = CheckStatus::State;
  if (v.phase == FirmwareView::Phase::AskInstall) {
    if (s.state == State::Checked && core::tick_elapsed(now, v.asked_at) < core::ticks_from_ms(hal::firmware::kCheckLifetimeMs)) {
      return false;
    }
    sink.cancel();  // the 30 s are over, or the service let the check go (it lapsed, a console command took the card, the card went to a computer): the good check is no longer good
    to_list(v, s.state == State::Checked ? kCheckExpired : kCheckAgain);
    return true;
  }
  const bool installing = v.phase == FirmwareView::Phase::Installing;
  switch (s.state) {
    case State::Running: {
      const bool changed = s.done != v.done || s.total != v.total;
      v.done = s.done;
      v.total = s.total;
      return changed;
    }
    case State::Checked:
      if (!installing) {
        v.image = s.image;
        v.crc = s.crc;
        v.asked_at = now;
        v.phase = FirmwareView::Phase::AskInstall;
        return true;
      }
      break;  // (never in an install: it does not report Checked again)
    case State::Failed:
      if (installing) {
        to_failure(v, "UPDATE FAILED: ", s.fault, nullptr);
      } else {
        sink.cancel();
        to_failure(v, "NOT A VALID IMAGE: ", s.fault, &s.offset);
      }
      return true;
    case State::DidNotStart:
      if (installing) {
        to_list(v, "UPDATE DID NOT START");
        return true;
      }
      break;
    case State::Idle: break;
  }
  to_list(v, "THE STEP WAS DROPPED");  // (the service has no job: nothing to follow)
  return true;
}

// The card was taken out or put in, or the unit is switching off (App::power_begin): a dialog that is not past the point of no return closes, and the sink is cancelled (a check read a card that is gone, or
// holds the card the shutdown's save needs). An install in the hand-over is not touched.
bool firmware_abandon(ViewState& view, FirmwareSink* sink) noexcept {
  FirmwareView& v = view.firmware;
  if (!v.open() || v.phase == FirmwareView::Phase::Installing) {
    return false;
  }
  if (sink != nullptr) {
    sink->cancel();
  }
  v.close();
  if (view.overlay == OverlayId::FirmwareUpdate) {
    view.overlay = OverlayId::None;
  }
  return true;
}

}  // namespace o3os::app
