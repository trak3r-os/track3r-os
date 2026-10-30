// SPDX-License-Identifier: GPL-3.0-or-later
// File actions (OS-010a): what the File screen's F-keys do. Opening hands the folder under the cursor to the device's project load (ProjectSink: the
// same `start_project_load` that `diag load` uses, so there is one loader); the result comes back through the App's pass, which tells the user.
#include "action_handlers.h"

#include <o3os/app/key_prompt.h>
#include <o3os/app/module_import.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/file_screen.h>

#include <cstring>

namespace o3os::app {

namespace {

// One line of news at a time: a new one replaces the File screen's last (upsert), so OPENING is overwritten by how the load ended (App::poll_files, same source).
// OS-USBDISK 4.3: while the computer has the card the news is asked, ahead of the persistent disk strip (save_actions.cpp).
bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return ctx.view.card_on_computer ? ctx.view.notify.ask(severity, text, "file", ViewState::kMessageMs) : ctx.view.notify.upsert(severity, text, "file", ViewState::kMessageMs);
}

// OS-DJ C1: the scratch ring is the sample arena's free tail while SCR or TAPE STOP runs and while it ends (the backspin takes 0.5 s, and leaving the DJ page ends it): a project load, an import or a new
// project empties the arena (SampleBank::clear forgets the held tail) and its loader writes the samples from the start of it, over a ring the render is still filling. Refused until the engine has let go.
bool scratch_refuses(ActionContext& ctx) noexcept {
  if (!ctx.view.dj.scratch_holds_arena()) {
    return false;
  }
  (void)say(ctx, core::Severity::Info, "END THE SCRATCH FIRST");
  return true;
}

// OS-OPENGUARD (OPEN and the IMPORT page's F1): "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM". The text of every question is made from its key table (key_prompt.h, OS-KEYROWDLG), which is also what the key row marks.
bool ask_discard(ActionContext& ctx) noexcept {
  AskText text;
  ask_text(Ask::Open, kAskHeadUnsaved, text);
  return ctx.view.notify.ask(core::Severity::Warn, text.c_str(), "file", ViewState::kMessageMs);  // (a question takes the head of the strip: it counts only there)
}

// "<folder>/<name>" into `out` (hal::storage::kPathMax + 1 bytes); false when it does not fit. An empty folder is the card's root.
bool join_path(char* out, const char* folder, const char* name) noexcept {
  const std::size_t fl = std::strlen(folder);
  const std::size_t nl = std::strlen(name);
  const std::size_t sep = fl != 0u ? 1u : 0u;
  if (nl == 0u || fl + sep + nl > hal::storage::kPathMax) {
    return false;
  }
  std::memcpy(out, folder, fl);
  if (sep != 0u) {
    out[fl] = '/';
  }
  std::memcpy(out + fl + sep, name, nl + 1u);
  return true;
}

// OS-FILES part A, F1 on the IMPORT page: a folder is entered (the App's pass restarts the scan there); a module file replaces the document with a new project made from it, after a
// second press when the project has changes no save has taken (the same question as F3 NEW; `armed` is whether it was asked).
bool import_go(ActionContext& ctx, bool armed) noexcept {
  if (ctx.view.card_on_computer) {  // OS-USBDISK 4.3: the listing is dropped while the computer has the card; the refusal is the card's, not "nothing to import"
    return say(ctx, core::Severity::Warn, SaveService::gate_text(hal::storage::WriteStatus::Host));
  }
  const DirScan* scan = ctx.model.files;
  if (scan == nullptr) {
    return say(ctx, core::Severity::Info, "NO SD CARD");
  }
  const DirIndex& index = scan->index();
  const int pos = ctx.view.file.pos();
  if (pos < 0 || static_cast<unsigned>(pos) >= index.count()) {
    return say(ctx, core::Severity::Info, "NOTHING TO IMPORT");
  }
  if (index.is_dir(static_cast<unsigned>(pos))) {
    ctx.view.import.go = FileImport::Go::Enter;
    return true;
  }
  if (ctx.project == nullptr || ctx.model.project == nullptr) {
    return say(ctx, core::Severity::Warn, "NO DOCUMENT TO REPLACE");
  }
  if (ctx.project->busy()) {
    return say(ctx, core::Severity::Info, "A LOAD IS ALREADY RUNNING");
  }
  if (scratch_refuses(ctx)) {
    return true;
  }
  if (ctx.saves != nullptr && ctx.saves->busy()) {
    return say(ctx, core::Severity::Info, "A SAVE IS RUNNING: TRY AGAIN WHEN IT ENDS");
  }
  char path[hal::storage::kPathMax + 1];
  if (!join_path(path, scan->folder(), index.name(static_cast<unsigned>(pos)))) {
    return say(ctx, core::Severity::Warn, "THE PATH IS TOO LONG");
  }
  // OS-IMPORT-ANY I3, I4: the file's first bytes decide whether a module reader takes it, before the unsaved-changes question: a file that is none never asks it and nothing changes.
  if (!ctx.project->module_file(path)) {
    ctx.view.file.ask_clear();
    return say(ctx, core::Severity::Warn, "NOT A MODULE THIS OS READS");
  }
  if (!armed && ctx.saves != nullptr && ctx.saves->dirty(*ctx.model.project)) {
    ctx.view.file.confirm_new = true;
    return ask_discard(ctx);
  }
  switch (ctx.project->import_module(path, ctx.view.import.engine)) {
    case ProjectSink::Start::Started: {
      ctx.view.file.opening = true;
      ctx.view.file.pending = FileView::Pending::Import;
      core::FixedString<kFileNameChars> shown;
      file_display_name(index.name(static_cast<unsigned>(pos)), shown);
      core::FixedString<core::kNotifyTextChars> text;
      text.assign("IMPORTING ");
      for (const char* p = shown.c_str(); *p != '\0'; ++p) {  // upper case, as the end of the import says the name (module_strip_text)
        const char c = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - ('a' - 'A')) : *p;
        text.append(&c, 1);
      }
      return say(ctx, core::Severity::Info, text.c_str());
    }
    case ProjectSink::Start::Busy: return say(ctx, core::Severity::Warn, "A LOAD IS ALREADY RUNNING");
    case ProjectSink::Start::NoCard: return say(ctx, core::Severity::Warn, ctx.view.card_off_text("THE CARD IS NOT MOUNTED"));
    case ProjectSink::Start::NoDocument: return say(ctx, core::Severity::Warn, "NO DOCUMENT TO REPLACE");
    case ProjectSink::Start::BadPath: return say(ctx, core::Severity::Warn, "NOT A MODULE FILE PATH");
  }
  return false;
}

// OS-NEWKEEP: the NEW question is up (F3 asked, nothing cancelled it, it is not older than its text on the strip, and that text is the one on the strip now: another message in front of it
// would let a quick second F3 answer a question nobody has read; it then only asks again). OS-KEYROWDLG: the key row marks F3 and F1 by the same test (file_question).
bool new_question_open(const ActionContext& ctx) noexcept {
  return file_question(ctx.view) == Ask::New && core::tick_elapsed(ctx.now, ctx.view.file.new_asked_at) <= core::ticks_from_ms(FileView::kNewAskMs);
}

// OS-NEWKEEP: the question on the strip is the plain one, but the project has been changed since it was asked (inside its 4 s): it would be answered without the unsaved-changes warning.
bool new_question_stale(const ActionContext& ctx) noexcept {
  AskText plain;
  ask_text(Ask::New, kAskHeadNew, plain);
  return ctx.view.notify.head().text == plain.c_str() && ctx.saves != nullptr && ctx.model.project != nullptr && ctx.saves->dirty(*ctx.model.project);
}

// OS-NEWKEEP: why F3 NEW cannot ask or start now (the line is said), false when it can.
bool new_refused(ActionContext& ctx) noexcept {
  if (ctx.project == nullptr || ctx.model.project == nullptr) {
    (void)say(ctx, core::Severity::Warn, "NO DOCUMENT TO REPLACE");
    return true;
  }
  if (ctx.project->busy()) {
    (void)say(ctx, core::Severity::Info, "A LOAD IS ALREADY RUNNING");
    return true;
  }
  if (scratch_refuses(ctx)) {
    return true;
  }
  if (ctx.saves != nullptr && ctx.saves->busy()) {
    (void)say(ctx, core::Severity::Info, "A SAVE IS RUNNING: TRY AGAIN WHEN IT ENDS");
    return true;
  }
  return false;
}

// OS-NEWKEEP: asks which new project (with the unsaved-changes warning when no save has taken the changes), or says why it cannot.
bool new_ask(ActionContext& ctx) noexcept {
  if (new_refused(ctx)) {
    return true;
  }
  ctx.view.file.new_choice = true;
  ctx.view.file.new_asked_at = ctx.now;
  const bool unsaved = ctx.saves != nullptr && ctx.saves->dirty(*ctx.model.project);  // (UNSAVED CHANGES is the guard of OS-OPENGUARD, now part of the one question)
  AskText text;
  ask_text(Ask::New, unsaved ? kAskHeadUnsaved : kAskHeadNew, text);
  return ctx.view.notify.ask(core::Severity::Warn, text.c_str(), "file", FileView::kNewAskMs);  // (OS-BUG-PROMPTQ: ahead of a job's DONE, which would hide it from the keys)
}

// OS-NEWKEEP: the answer to the NEW question: the platform replaces the document, keeping the instruments or not.
bool new_go(ActionContext& ctx, bool keep_instruments) noexcept {
  if (new_refused(ctx)) {
    return true;
  }
  switch (keep_instruments ? ctx.project->start_new_keeping_instruments() : ctx.project->start_new()) {
    case ProjectSink::Start::Started:
      ctx.view.file.opening = true;
      ctx.view.file.pending = FileView::Pending::New;
      return say(ctx, core::Severity::Info, keep_instruments ? "NEW PROJECT, KEEPING INSTRUMENTS..." : "NEW PROJECT...");
    case ProjectSink::Start::Busy: return say(ctx, core::Severity::Warn, "A LOAD IS ALREADY RUNNING");
    case ProjectSink::Start::NoCard:
    case ProjectSink::Start::BadPath:
    case ProjectSink::Start::NoDocument: return say(ctx, core::Severity::Warn, "NO DOCUMENT TO REPLACE");
  }
  return false;
}

}  // namespace

// F1 OPEN: with changes no save has taken the first press only asks (OS-OPENGUARD, as F3 NEW); the project folder under the cursor (an OG project, a native one or a native one to recover: the listing's tag says which). The load is the platform's (slices of the main loop, the app's frames held, so the strip says
// OPENING before the screen stops); how it ended is told when the App sees it end (App::poll_files).
bool file_open(ActionContext& ctx, core::ActionArg) noexcept {
  const bool armed = ctx.view.file.confirm_new;
  const bool keep = new_question_open(ctx);
  const bool stale = keep && new_question_stale(ctx);
  ctx.view.file.ask_clear();
  if (keep) {  // OS-NEWKEEP: F3 NEW asked which new project, and F1 is KEEP INSTRUMENTS (never an open); changed since it was asked: it asks again, with the warning
    return stale ? new_ask(ctx) : new_go(ctx, true);
  }
  if (ctx.view.import.page) {  // OS-FILES: on the IMPORT page F1 is ENTER / IMPORT
    return import_go(ctx, armed);
  }
  if (ctx.view.card_on_computer) {  // OS-USBDISK 4.3: ahead of "a save is running" and "nothing to open" (the listing is dropped while the computer has the card)
    return say(ctx, core::Severity::Warn, SaveService::gate_text(hal::storage::WriteStatus::Host));
  }
  const DirScan* scan = ctx.model.files;
  if (ctx.project == nullptr || scan == nullptr) {
    return false;
  }
  if (scratch_refuses(ctx)) {
    return true;
  }
  if (ctx.saves != nullptr && ctx.saves->busy()) {  // OS-SAVEABORT: a load replaces the document and the sample memory under the save, which would stop it half done: it is refused, not the save
    return say(ctx, core::Severity::Info, "A SAVE IS RUNNING: TRY AGAIN WHEN IT ENDS");
  }
  const DirIndex& index = scan->index();
  const int pos = ctx.view.file.entry(index);  // (OS-FILES part B: the cursor may be on a row of the RECENT group, which is the same folder as its row in the list)
  if (pos < 0 || static_cast<unsigned>(pos) >= index.count() || !index.is_dir(static_cast<unsigned>(pos))) {
    return say(ctx, core::Severity::Info, "NOTHING TO OPEN");
  }
  if (!armed && ctx.saves != nullptr && ctx.model.project != nullptr && ctx.saves->dirty(*ctx.model.project)) {  // OS-OPENGUARD: as NEW and IMPORT, the open throws the changes away: it asks first
    ctx.view.file.confirm_new = true;
    return ask_discard(ctx);
  }
  char path[hal::storage::kPathMax + 1];
  if (!join_path(path, scan->folder(), index.name(static_cast<unsigned>(pos)))) {
    return say(ctx, core::Severity::Warn, "THE PATH IS TOO LONG");
  }
  if (ctx.saves != nullptr && ctx.saves->project_deleted(path)) {  // OS-FILES part B: deleted since the list was read (a stale row), or a RECENT row of one: never loaded
    return say(ctx, core::Severity::Warn, "THIS PROJECT WAS DELETED");
  }
  const EntryTag tag = index.tag(static_cast<unsigned>(pos));
  const ProjectSink::Format format = tag == EntryTag::Native ? ProjectSink::Format::Native : (tag == EntryTag::Recover ? ProjectSink::Format::NativeBackup : ProjectSink::Format::Og);
  switch (ctx.project->open(path, format)) {
    case ProjectSink::Start::Started: {
      ctx.view.file.opening = true;
      ctx.view.file.pending = format == ProjectSink::Format::NativeBackup ? FileView::Pending::Recover : FileView::Pending::Open;
      core::FixedString<kFileNameChars> shown;
      file_display_name(index.name(static_cast<unsigned>(pos)), shown);
      ctx.view.file.opening_name = shown;
      core::FixedString<core::kNotifyTextChars> text;
      text.assign(format == ProjectSink::Format::NativeBackup ? "RECOVERING " : "OPENING ");
      text.append(shown.c_str());
      return say(ctx, core::Severity::Info, text.c_str());
    }
    case ProjectSink::Start::Busy: return say(ctx, core::Severity::Warn, "A LOAD IS ALREADY RUNNING");
    case ProjectSink::Start::NoCard: return say(ctx, core::Severity::Warn, ctx.view.card_off_text("THE CARD IS NOT MOUNTED"));
    case ProjectSink::Start::NoDocument: return say(ctx, core::Severity::Warn, "NO DOCUMENT TO LOAD INTO");
    case ProjectSink::Start::BadPath: return say(ctx, core::Severity::Warn, "NOT A PROJECT FOLDER PATH");
  }
  return false;
}

// Is a notice of the File screen on the strip that starts like one of the questions that wait for a second press (NEW's, the unsaved-changes guard's)? A cheap look at the few entries of the queue: the texts themselves
// are only made when one is (file_question_sync runs every pass).
namespace {
bool question_text_queued(const ViewState& view) noexcept {
  for (unsigned i = 0; i < view.notify.size(); ++i) {
    const core::Notification& n = view.notify.at(i);
    if (n.source == "file" && (std::strncmp(n.text.c_str(), kAskHeadNew, std::strlen(kAskHeadNew)) == 0 || std::strncmp(n.text.c_str(), kAskHeadUnsaved, std::strlen(kAskHeadUnsaved)) == 0)) {
      return true;
    }
  }
  return false;
}
}  // namespace

// OS-NEWKEEP2: a question that was asked (F3 NEW, the unsaved-changes guard of OPEN and IMPORT) and taken back (another key, a move, a page change, leaving the screen) leaves the strip at once,
// instead of showing its text for the rest of its time with nothing left to answer. The App calls it once a pass; an answer has already replaced the text, so nothing matches then.
bool file_question_sync(ViewState& view) noexcept {
  if (view.screen != ScreenId::File) {
    view.file.ask_clear();  // leaving the screen takes the question back (the screen's enter() would, but only on the way back in)
  }
  if (view.notify.empty()) {
    return false;
  }
  const unsigned before = view.notify.size();
  if (!view.file.og_ask && !view.file.og_ask_text.empty()) {
    view.notify.dismiss("file", view.file.og_ask_text.c_str());  // OS-FILES-C C2c: the OG PROJECT row's question, taken back
    view.file.og_ask_text.clear();
  }
  if (!view.file.new_choice && !view.file.confirm_new && question_text_queued(view)) {
    AskText text;  // (made on the stack and only while one of these questions is on the strip: no static, SRAM is strict)
    ask_text(Ask::New, kAskHeadNew, text);
    view.notify.dismiss("file", text.c_str());
    ask_text(Ask::New, kAskHeadUnsaved, text);
    view.notify.dismiss("file", text.c_str());
    ask_text(Ask::Open, kAskHeadUnsaved, text);
    view.notify.dismiss("file", text.c_str());
  }
  return view.notify.size() != before;  // the strip changed: a redraw
}

bool file_open_available(const ActionContext& ctx) noexcept { return ctx.project != nullptr; }

// F3 NEW (OS-NEWKEEP): a fresh project (the model's defaults: pattern 1, one song position, 12 tracks as ADR 0003) in place of the one in the document. The first press only asks which one
// (the question says so, with the unsaved-changes warning when no save has taken the changes): F3 again is ALL (the instruments, sample and module, and the sample memory go with the project), F1 is
// KEEP INSTRUMENTS (the instrument table and the audio of the sample instruments stay); another key, a move or leaving the screen takes the question back. The project is thrown away, and the card is
// not touched. The platform does the replacing as it does a load (stops the transport and the live notes, lets the voices die away, then resets the document and, for ALL, the sample memory).
bool file_new(ActionContext& ctx, core::ActionArg) noexcept {
  const bool asked = new_question_open(ctx);
  const bool stale = asked && new_question_stale(ctx);
  ctx.view.file.ask_clear();
  if (ctx.view.import.page) {  // OS-FILES: on the IMPORT page F3 is UP (one folder; the root has none)
    if (ctx.view.import.folder[0] == '\0') {
      return say(ctx, core::Severity::Info, "ALREADY AT THE ROOT");
    }
    ctx.view.import.go = FileImport::Go::Up;
    return true;
  }
  if (asked && !stale) {
    return new_go(ctx, false);
  }
  return new_ask(ctx);  // the first press, or the project changed since the question was asked: once more, with the warning
}

// F6 IMPORT / PROJECTS (OS-FILES): the File screen's pages. The App's pass sees the page change and starts the other listing. On the EXPORT page (OS-WAVEXP) F6 is IMPORT: the way to the module files.
bool file_page(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.file.ask_clear();
  if (ctx.view.xport.page) {
    ctx.view.xport.page = false;
    ctx.view.import.page = true;
    ctx.view.import.report = false;
    ctx.view.import.all = false;  // OS-IMPORT-ANY I1: MODULES at every visit
    ctx.view.import.go = FileImport::Go::None;
    return true;
  }
  ctx.view.import.page = !ctx.view.import.page;
  ctx.view.import.report = false;
  ctx.view.import.all = false;
  ctx.view.import.go = FileImport::Go::None;
  return true;
}

// F7 (OS-FILES, OS-WAVEXP): on the IMPORT page FX, where the next import's commands go (AUTO, AMIGA, MODERN, round); on the PROJECTS page EXPORT, the EXPORT page, which starts at the Pattern
// screen's pattern; on the EXPORT page PROJECTS, back.
bool file_engine(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.file.ask_clear();
  ExportView& x = ctx.view.xport;
  if (x.page) {
    x.page = false;
    return true;
  }
  if (!ctx.view.import.page) {
    if (ctx.view.card_on_computer) {  // OS-USBDISK 4.3: the page exists to export to the card, which the computer has: refused here, not on a later F1 behind the persistent strip
      return say(ctx, core::Severity::Warn, SaveService::gate_text(hal::storage::WriteStatus::Host));
    }
    x.page = true;
    x.fresh = false;
    x.pattern = ctx.view.edit.pattern < 1 ? std::uint8_t{1} : static_cast<std::uint8_t>(ctx.view.edit.pattern);
    return true;
  }
  ctx.view.import.engine = next_engine(ctx.view.import.engine);
  return say(ctx, core::Severity::Info, engine_notice(ctx.view.import.engine));
}

// F8 REPORT / LIST (IMPORT page): the last import's report instead of the list, and back.
bool file_report(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.file.ask_clear();
  if (!ctx.view.import.page) {
    return false;
  }
  if (!ctx.view.import.report && (ctx.model.import_report == nullptr || !ctx.model.import_report->valid)) {
    return say(ctx, core::Severity::Info, "NO IMPORT YET");
  }
  ctx.view.import.report = !ctx.view.import.report;
  return true;
}

// Shift + F6 NAME (OS-NAMES): the keyboard on the open project's title, the name stored in the project. It never renames the folder on the card (SAVE AS makes folders), and a folder's name never
// changes the title.
bool file_name(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.file.ask_clear();  // another key takes back every pending question (NEW, DELETE, PURGE)
  return name_open(ctx, TextEntryView::Target::Project, 0, ctx.model.project != nullptr ? ctx.model.project->name() : "", model::kProjectNameMax);
}

// F2 RESCAN: read the folder again. The request is the view's; the App's pass owns the card and the job, and starts it. On the IMPORT page (OS-IMPORT-ANY I1) F2 switches the list between MODULES and
// ALL FILES (the key says the mode a press goes to) and reads the folder again as it does so.
bool file_rescan(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.file.ask_clear();
  if (ctx.view.import.page && !ctx.view.import.report) {
    ctx.view.import.all = !ctx.view.import.all;  // (the key row and the info line say the mode; the scan's end says the count)
  }
  ctx.view.file.rescan = true;
  return true;
}

}  // namespace o3os::app
