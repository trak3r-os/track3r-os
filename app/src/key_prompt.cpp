// SPDX-License-Identifier: GPL-3.0-or-later
// OS-KEYROWDLG: the key prompts (key_prompt.h). This is the one place a question names an F-key: the strip text, the key row and the test all read the tables below (tools/check_keytext.py keeps it so).
#include <o3os/app/key_prompt.h>

#include <o3os/app/frame.h>
#include <o3os/app/learn_list.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/screens/config_screen.h>
#include <o3os/app/tools_list.h>

#include "screens/dialog.h"

#include <cstring>

namespace o3os::app {

namespace {

// Keys: 0..7 = F1..F8, 8..15 = Shift + F1..F8.
constexpr std::uint8_t kF1 = 0;
constexpr std::uint8_t kF2 = 1;
constexpr std::uint8_t kF3 = 2;
constexpr std::uint8_t kF4 = 3;
constexpr std::uint8_t kF5 = 4;
constexpr std::uint8_t kF6 = 5;
constexpr std::uint8_t kF7 = 6;
constexpr std::uint8_t kF8 = 7;
constexpr std::uint8_t kShiftF3 = 8 + 2;
constexpr std::uint8_t kShiftF7 = 8 + 6;
constexpr std::uint8_t kShiftF8 = 8 + 7;

// New (OS-NEWKEEP): F3 again is ALL, F1 is KEEP INSTRUMENTS; the strip names the keys in that order. "KEEP INS" is the 8 characters the key has; the strip carries the long words.
constexpr KeyOption kNewOptions[] = {{kF3, "ALL", "ALL"}, {kF1, "KEEP INS", "KEEP INSTRUMENTS"}};
// Open / Import (OS-OPENGUARD): the changes no save has taken are thrown away by the second press of F1.
constexpr KeyOption kOpenOptions[] = {{kF1, "OPEN?", "AGAIN TO DISCARD THEM"}};
constexpr KeyOption kImportOptions[] = {{kF1, "IMPORT?", "AGAIN TO DISCARD THEM"}};
// Purge (OS-FILES part B): Shift + F7 again.
constexpr KeyOption kPurgeOptions[] = {{kShiftF7, "PURGE?", "AGAIN"}};
// Og (OS-FILES-C C2c): F1 again on the OG PROJECT row.
constexpr KeyOption kOgOptions[] = {{kF1, "EXPORT?", "AGAIN"}};
// Apply (OS-USBAUDIO): F7 again writes the pending USB values. The strip says "PRESS AGAIN" without a key; the info line of the page names F7.
constexpr KeyOption kApplyOptions[] = {{kF7, "APPLY?", "AGAIN"}};
// Defaults (OS-SHIFTROW): Shift + F7 again, within two seconds, puts the page's rows back to their defaults.
constexpr KeyOption kDefaultsOptions[] = {{kShiftF7, "RESET?", "AGAIN: RESET PAGE"}};
// Launch (OS-H1, OS-H4): F1 again starts an app that edits the project or writes files.
constexpr KeyOption kLaunchOptions[] = {{kF1, "LAUNCH?", "AGAIN"}};
// Shared (OS-SONGMAP): F1 ALL, F2 UNIQUE, in the order the strip names them.
constexpr KeyOption kSharedOptions[] = {{kF1, "ALL", "ALL"}, {kF2, "UNIQUE", "UNIQUE"}};

// Share / Eject (OS-USBDISK U2b): F7 again on the CARD AS DISK row. The strip's texts are the row's own (param_actions.cpp), so no words here beyond AGAIN.
constexpr KeyOption kShareOptions[] = {{kF7, "SHARE?", "AGAIN"}};
constexpr KeyOption kEjectOptions[] = {{kF7, "EJECT?", "AGAIN"}};
// The take under review (OS-QAFIX-E-SREC-09, OS-RECCROP): F4 DISCARD, F1 / Rec (a new take over it) and Shift + F3 CROP, each asks once and is done by the same key again.
constexpr KeyOption kTakeDropOptions[] = {{kF4, "DISCARD?", "AGAIN"}};
constexpr KeyOption kTakeRecOptions[] = {{kF1, "REC?", "AGAIN"}};
constexpr KeyOption kTakeCropOptions[] = {{kShiftF3, "CROP?", "AGAIN"}};
// Replace (OS-BUG-SLDOVER): F1 again on Sample load loads the WAV over the instrument that is there.
constexpr KeyOption kReplaceOptions[] = {{kF1, "REPLACE?", "AGAIN"}};
// Clear (OS-AUDIT-69): the Song map's Delete over 2 or more patterns: F1 YES; any other key withdraws (the words keep "CLEAR 128 PATTERNS?: F1 YES, OTHER KEY CANCELS" inside the strip's 48 characters).
constexpr KeyOption kClearOptions[] = {{kF1, "YES", "YES, OTHER KEY CANCELS"}};

template <std::size_t N>
constexpr KeyPrompt question(const KeyOption (&options)[N]) noexcept {
  return KeyPrompt{PromptKind::Question, options, static_cast<std::uint8_t>(N)};
}
template <std::size_t N>
constexpr KeyPrompt owner(const KeyOption (&options)[N]) noexcept {
  return KeyPrompt{PromptKind::Owner, options, static_cast<std::uint8_t>(N)};
}
template <std::size_t N>
constexpr KeyPrompt every_key(const KeyOption (&options)[N]) noexcept {
  return KeyPrompt{PromptKind::Owner, options, static_cast<std::uint8_t>(N), true};
}

// ---- The dialogs (every modal overlay owns the row). The option order is the order of the dialog line. A key with two options has one live at a time. -------------------------------------------------
// Learn (A1): F1 UNBIND and F2 MODE with a target, F3 DELETE and F4 CLEAR ALL with bindings (F4 asks, then reads CLR ALL? inverted), F8 BACK.
constexpr KeyOption kLearnOptions[] = {{kF1, "UNBIND", "UNBIND"}, {kF2, "MODE", "MODE"},           {kF3, "DELETE", "DELETE"},
                                       {kF4, "CLR ALL", "CLEAR ALL"}, {kF4, "CLR ALL?", "CLEAR ALL?"}, {kF8, "BACK", "BACK"}};
// The Navigator (A2): F1 and F8 GO (F1 has no words: a second GO), F7 CLOSE; its Shift layer is declared: Shift+F7 CLOSE and Shift+F8 GO are bound. Shift+F1..F6 are bound to ui.none and read "-": owner_row makes
// that of a Shift key with no option, so the table does not hold them.
constexpr KeyOption kNavigatorOptions[] = {{kF1, "GO", ""}, {kF7, "CLOSE", "CLOSE"}, {kF8, "GO", "GO"}, {kShiftF7, "CLOSE", ""}, {kShiftF8, "GO", ""}};
// The pattern's choosers (A3): F2 and F3 switch between LENGTH and JUMP (a tap), F1 is the PATTERN chooser's held key (OS-PATSWITCH: its tap opens nothing), the open one is inverted, F8 closes. The strip has room for BACK only: the names are on the row.
constexpr KeyOption kChooserOptions[] = {{kF1, "PATTERN", ""}, {kF2, "LENGTH", ""}, {kF3, "JUMP", ""}, {kF8, "BACK", "BACK"}};
// The engine picker (A4) and the play mode list (A6): F1 chooses, F8 closes.
constexpr KeyOption kSelectOptions[] = {{kF1, "SELECT", "SELECT"}, {kF8, "BACK", "BACK"}};
// The TOOLS list (A5): F1 runs the item; after a launch that wants a second press it reads RUN? inverted.
constexpr KeyOption kToolsOptions[] = {{kF1, "RUN", "RUN"}, {kF1, "RUN?", "RUN?"}, {kF8, "BACK", "BACK"}};
// The on-screen keyboard (A7): F1 types the key in focus, F2 is CAPS, F6 AUTO is the Save As's automatic name, F7 cancels, F8 saves (SAVE for Save As, OK for the names).
constexpr KeyOption kTextOptions[] = {{kF1, "TYPE", "TYPE"}, {kF2, "CAPS", "CAPS"}, {kF6, "AUTO", "AUTO"}, {kF7, "CANCEL", "CANCEL"}, {kF8, "SAVE", "SAVE"}, {kF8, "OK", "OK"}};
// The step popup of a held Instr / Fx / Note / Vol (A8): F8 cancels.
constexpr KeyOption kPopupOptions[] = {{kF8, "CANCEL", "CANCEL"}};
// The Fill dialog (A9).
constexpr KeyOption kFillOptions[] = {{kF1, "FILL", "FILL"}, {kF8, "BACK", "BACK"}};
// The Song screen's TOOLS dialog (OS-SONGMAP): F1 applies the tool over the rectangle, F8 leaves.
constexpr KeyOption kSongToolsOptions[] = {{kF1, "APPLY", "APPLY"}, {kF8, "BACK", "BACK"}};
// The help page (OS-HELP): every key closes it, F8 is the one named.
constexpr KeyOption kHelpOptions[] = {{kF8, "CLOSE", "CLOSE"}};
// The progress of a WAV export (A10): F8 cancels; the dialog's own text says it (no words here).
constexpr KeyOption kExportOptions[] = {{kF8, "CANCEL", ""}};
// The firmware update dialog (OS-FWCONFIG): F1 UPDATE on the list, YES to question 1, INSTALL to question 2 (the answers are drawn inverted); F8 BACK on the list, CANCEL before INSTALL. No key acts once INSTALL
// was taken ("DO NOT POWER OFF"): the row is blank. Any other control takes a question back (firmware_question_taken_back).
constexpr KeyOption kFirmwareOptions[] = {{kF1, "UPDATE", "UPDATE"}, {kF1, "YES", "YES"}, {kF1, "INSTALL", "INSTALL"}, {kF8, "BACK", "BACK"}, {kF8, "CANCEL", "CANCEL"}};

// The boot disk (D1): a key or pad restarts the unit, whichever it is, so all eight keys say so (one option stands for the eight: KeyPrompt::every_key); after the first press while a computer may still be writing
// every key asks again (inverted); while the restart waits for the command in flight no key acts any more, and the row is blank (an owned row with no option).
constexpr KeyOption kDiskRestartOptions[] = {{kF1, "RESTART", ""}};
constexpr KeyOption kDiskConfirmOptions[] = {{kF1, "AGAIN?", ""}};
constexpr KeyOption kDiskFinishingOptions[] = {{kF1, "", ""}};  // (never live: the row is owned and blank)

constexpr KeyPrompt kDiskRestartPrompt = every_key(kDiskRestartOptions);
constexpr KeyPrompt kDiskConfirmPrompt = every_key(kDiskConfirmOptions);
constexpr KeyPrompt kDiskFinishingPrompt = owner(kDiskFinishingOptions);
constexpr KeyPrompt kLearnPrompt = owner(kLearnOptions);
constexpr KeyPrompt kNavigatorPrompt = owner(kNavigatorOptions);
constexpr KeyPrompt kChooserPrompt = owner(kChooserOptions);
constexpr KeyPrompt kSelectPrompt = owner(kSelectOptions);
constexpr KeyPrompt kToolsPrompt = owner(kToolsOptions);
constexpr KeyPrompt kTextPrompt = owner(kTextOptions);
constexpr KeyPrompt kPopupPrompt = owner(kPopupOptions);
constexpr KeyPrompt kFillPrompt = owner(kFillOptions);
constexpr KeyPrompt kSongToolsPrompt = owner(kSongToolsOptions);
constexpr KeyPrompt kExportPrompt = owner(kExportOptions);
constexpr KeyPrompt kFirmwarePrompt = owner(kFirmwareOptions);
constexpr KeyPrompt kHelpPrompt = owner(kHelpOptions);

// One row per question, in the order of Ask (after None). `sep` is what stands between the head and the keys.
struct AskDef {
  KeyPrompt prompt;
  const char* sep;
};
constexpr AskDef kAsks[] = {
    {question(kNewOptions), ": "},       // New
    {question(kOpenOptions), ": "},      // Open
    {question(kImportOptions), ": "},    // Import
    {question(kPurgeOptions), " "},      // Purge: "PURGE 3 UNUSED INSTRUMENTS? SHIFT+F7 AGAIN"
    {question(kOgOptions), ": "},        // Og
    {question(kApplyOptions), " "},      // Apply: "PRESS F7 AGAIN"
    {question(kDefaultsOptions), ""},    // Defaults: "SHIFT+F7 AGAIN: RESET PAGE" (no head)
    {question(kLaunchOptions), ": "},    // Launch
    {question(kSharedOptions), ": "},    // Shared: "PATTERN 03 ALSO IN 2 SLOTS: F1 ALL, F2 UNIQUE"
    {question(kShareOptions), " "},      // Share (the row's strip text is its own)
    {question(kEjectOptions), " "},      // Eject
    {question(kTakeDropOptions), ": "},  // TakeDrop: "TAKE NOT SAVED: F4 AGAIN"
    {question(kTakeRecOptions), ": "},   // TakeRec: "TAKE NOT SAVED: F1 AGAIN"
    {question(kTakeCropOptions), ": "},  // TakeCrop: "CROP KEEPS THE WINDOW ONLY: SHIFT+F3 AGAIN"
    {question(kReplaceOptions), ": "},   // Replace: "REPLACE 03 KICK: F1 AGAIN"
    {question(kClearOptions), ": "},     // Clear: "CLEAR 3 PATTERNS?: F1 YES, OTHER KEY CANCELS"
};
static_assert(sizeof(kAsks) / sizeof(kAsks[0]) == static_cast<std::size_t>(Ask::Clear), "one row per question");

const AskDef& def(Ask which) noexcept { return kAsks[static_cast<unsigned>(which) - 1u]; }

// Is `text` the head of the strip?
bool strip_head_is(const ViewState& view, const char* text) noexcept { return !view.notify.empty() && view.notify.head().text == text; }

bool strip_head_is_ask(const ViewState& view, Ask which, const char* head) noexcept {
  if (view.notify.empty()) {
    return false;
  }
  AskText text;
  ask_text(which, head, text);
  return view.notify.head().text == text.c_str();
}

// The purge question's text carries a count: "PURGE <n> UNUSED INSTRUMENT(S)? SHIFT+F7 AGAIN". It is found again by its two ends.
bool strip_head_is_purge(const ViewState& view) noexcept {
  if (view.notify.empty()) {
    return false;
  }
  AskText tail;
  ask_tail(Ask::Purge, tail);
  const char* text = view.notify.head().text.c_str();
  const std::size_t len = std::strlen(text);
  const std::size_t head = std::strlen(kAskHeadPurge);
  return len > head + tail.size() && std::strncmp(text, kAskHeadPurge, head) == 0 && std::strcmp(text + len - tail.size(), tail.c_str()) == 0;
}

// The state of a dialog: `live` and `hot` are option masks (bit n = option n).
PromptState owned(const KeyPrompt& prompt, std::uint16_t live, std::uint16_t hot = 0) noexcept {
  PromptState s;
  s.prompt = &prompt;
  s.live = live;
  s.hot = hot;
  return s;
}

constexpr std::uint16_t all_of(const KeyPrompt& prompt) noexcept { return static_cast<std::uint16_t>((1u << prompt.count) - 1u); }

PromptState owner_state(const Model& model, const ViewState& view) noexcept {
  switch (view.overlay) {
    case OverlayId::Learn: {
      const ScreenEntry* entry = find_screen(screen_registry(), view.screen);
      const core::ParamId target = entry != nullptr && entry->screen != nullptr ? entry->screen->focus_param(model, view) : core::ParamId::None;
      ParamScratch scratch;
      const bool has_target = describe_param(model, target, scratch) != nullptr;
      const bool has_list = learn_entry_count(model) != 0;
      std::uint16_t live = 1u << 5;  // BACK
      live = static_cast<std::uint16_t>(live | (has_target ? 0x03u : 0u) | (has_list ? (view.learn.confirm ? 0x14u : 0x0Cu) : 0u));
      return owned(kLearnPrompt, live, view.learn.confirm ? 1u << 4 : 0u);
    }
    case OverlayId::Navigator: {
      const ScreenEntry* chosen = screen_at(navigator_screens(), view.navigator);
      const bool built = chosen != nullptr && chosen->screen != nullptr;  // GO does nothing on a cell that is not built: its keys read blank
      std::uint16_t live = (1u << 1) | (1u << 3);  // F7 CLOSE, Shift+F7 CLOSE
      live = static_cast<std::uint16_t>(live | (built ? ((1u << 0) | (1u << 2) | (1u << 4)) : 0u));  // F1 GO, F8 GO, Shift+F8 GO
      return owned(kNavigatorPrompt, live);
    }
    // OS-PATSWITCH: the PATTERN chooser is the hold of F1 only (a tap of F1 does nothing), so F1 is live in it, as the held key, and blank in the LENGTH and JUMP choosers, where a tap of it does nothing; F2 and F3 switch.
    case OverlayId::PatternChooser: return owned(kChooserPrompt, all_of(kChooserPrompt), 1u << 0);
    case OverlayId::LengthChooser: return owned(kChooserPrompt, (1u << 1) | (1u << 2) | (1u << 3), 1u << 1);
    case OverlayId::JumpChooser: return owned(kChooserPrompt, (1u << 1) | (1u << 2) | (1u << 3), 1u << 2);
    case OverlayId::ModulePicker:
    case OverlayId::PlayMode: return owned(kSelectPrompt, all_of(kSelectPrompt));
    case OverlayId::Tools: {
      ToolItem item;
      const bool app_item = model.modules != nullptr && tools_item(model, view.modules.tools, item, view.edit.selection.active) && item.kind == ToolItem::Kind::App;
      const bool again = app_item && view.modules.tools_msg[0] != '\0' && model.modules->confirm_pending_for(item.app_entry) && std::strcmp(view.modules.tools_msg, model.modules->confirm_text()) == 0;
      return again ? owned(kToolsPrompt, (1u << 1) | (1u << 2), 1u << 1) : owned(kToolsPrompt, (1u << 0) | (1u << 2));
    }
    case OverlayId::TextEntry: {
      const bool save_as = view.text.target == TextEntryView::Target::SaveAs;
      std::uint16_t live = (1u << 0) | (1u << 1) | (1u << 3) | (save_as ? (1u << 2) | (1u << 4) : (1u << 5));  // TYPE, CAPS, CANCEL; AUTO and SAVE for Save As, OK for the names
      return owned(kTextPrompt, live);
    }
    case OverlayId::StepPopup: return owned(kPopupPrompt, all_of(kPopupPrompt));
    case OverlayId::Fill: return owned(kFillPrompt, all_of(kFillPrompt));
    case OverlayId::SongTools: return owned(kSongToolsPrompt, all_of(kSongToolsPrompt));
    case OverlayId::ExportProgress: return owned(kExportPrompt, all_of(kExportPrompt));
    case OverlayId::FirmwareUpdate:
      switch (view.firmware.phase) {
        case FirmwareView::Phase::List: return owned(kFirmwarePrompt, static_cast<std::uint16_t>((view.firmware.n != 0u ? 1u << 0 : 0u) | (1u << 3)));  // UPDATE (with a file to choose), BACK
        case FirmwareView::Phase::AskUpdate: return owned(kFirmwarePrompt, (1u << 1) | (1u << 4), 1u << 1);                                           // YES (inverted), CANCEL
        case FirmwareView::Phase::Checking: return owned(kFirmwarePrompt, 1u << 4);                                                                    // CANCEL
        case FirmwareView::Phase::AskInstall: return owned(kFirmwarePrompt, (1u << 2) | (1u << 4), 1u << 2);                                          // INSTALL (inverted), CANCEL
        case FirmwareView::Phase::Installing:
        case FirmwareView::Phase::Closed: break;
      }
      return owned(kFirmwarePrompt, 0u);  // the hand-over runs: the dialog owns the row and no key acts
    case OverlayId::Help: return owned(kHelpPrompt, all_of(kHelpPrompt));
    case OverlayId::None:
    case OverlayId::Notify: break;
  }
  return PromptState{};
}

PromptState state_of(Ask which) noexcept {
  PromptState s;
  if (which == Ask::None) {
    return s;
  }
  s.prompt = &def(which).prompt;
  s.live = s.hot = static_cast<std::uint16_t>((1u << s.prompt->count) - 1u);  // a question marks every answer, inverted
  return s;
}

}  // namespace

const KeyPrompt& ask_prompt(Ask which) noexcept { return def(which).prompt; }

bool ask_is_up(const ViewState& view, Ask which, const char* head) noexcept { return strip_head_is_ask(view, which, head); }

void append_key_name(AskText& out, std::uint8_t key) noexcept {
  if (key >= 8u) {
    out.append("SHIFT+");
  }
  const char name[3] = {'F', static_cast<char>('1' + (key & 7u)), '\0'};
  out.append(name);
}

const KeyPrompt* owner_prompt(OverlayId id) noexcept {
  switch (id) {
    case OverlayId::Learn: return &kLearnPrompt;
    case OverlayId::Navigator: return &kNavigatorPrompt;
    case OverlayId::PatternChooser:
    case OverlayId::LengthChooser:
    case OverlayId::JumpChooser: return &kChooserPrompt;
    case OverlayId::ModulePicker:
    case OverlayId::PlayMode: return &kSelectPrompt;
    case OverlayId::Tools: return &kToolsPrompt;
    case OverlayId::TextEntry: return &kTextPrompt;
    case OverlayId::StepPopup: return &kPopupPrompt;
    case OverlayId::Fill: return &kFillPrompt;
    case OverlayId::SongTools: return &kSongToolsPrompt;
    case OverlayId::ExportProgress: return &kExportPrompt;
    case OverlayId::FirmwareUpdate: return &kFirmwarePrompt;
    case OverlayId::Help: return &kHelpPrompt;
    case OverlayId::None:
    case OverlayId::Notify: break;
  }
  return nullptr;
}

void ask_tail(Ask which, AskText& out) noexcept {
  const AskDef& d = def(which);
  out.assign(d.sep);
  for (unsigned i = 0; i < d.prompt.count; ++i) {
    const KeyOption& o = d.prompt.options[i];
    if (o.words != nullptr && o.words[0] == '\0') {
      continue;
    }
    if (i != 0) {
      out.append(", ");
    }
    append_key_name(out, o.key);
    out.append(" ");
    out.append(o.words != nullptr ? o.words : o.label);
  }
}

void ask_text(Ask which, const char* head, AskText& out) noexcept {
  AskText tail;
  ask_tail(which, tail);
  out.assign(head);
  out.append(head[0] != '\0' ? tail.c_str() : tail.c_str() + std::strlen(def(which).sep));  // (no head: no separator)
}

Ask file_question(const ViewState& view) noexcept {
  const FileView& f = view.file;
  if (f.new_choice && (strip_head_is_ask(view, Ask::New, kAskHeadNew) || strip_head_is_ask(view, Ask::New, kAskHeadUnsaved))) {
    return Ask::New;
  }
  if (f.confirm_new && strip_head_is_ask(view, Ask::Open, kAskHeadUnsaved)) {
    return view.import.page ? Ask::Import : Ask::Open;
  }
  if (f.confirm_purge && strip_head_is_purge(view)) {
    return Ask::Purge;
  }
  if (f.og_ask && !f.og_ask_text.empty() && !view.notify.empty() && view.notify.head().source == "file" && strip_head_is(view, f.og_ask_text.c_str())) {  // (posted from the File screen's source)
    return Ask::Og;
  }
  return Ask::None;
}

// The Record screen's question (OS-QAFIX-E-SREC-09): armed by the first press, live while its text is the head of the strip.
Ask take_question(const ViewState& view) noexcept {
  switch (view.take_ask.kind) {
    case TakeAsk::Kind::Discard: return strip_head_is_ask(view, Ask::TakeDrop, kAskHeadTake) ? Ask::TakeDrop : Ask::None;
    case TakeAsk::Kind::Record: return strip_head_is_ask(view, Ask::TakeRec, kAskHeadTake) ? Ask::TakeRec : Ask::None;
    case TakeAsk::Kind::Crop: return strip_head_is_ask(view, Ask::TakeCrop, kAskHeadCrop) ? Ask::TakeCrop : Ask::None;
    case TakeAsk::Kind::None: break;
  }
  return Ask::None;
}

bool sample_question_live(const ViewState& view) noexcept {
  return view.screen == ScreenId::SampleLoad && view.wav.ask_pos >= 0 && view.instrument - 1 == view.wav.ask_slot && !view.wav.ask_text.empty() && !view.notify.empty() && view.notify.head().source == "sample" && strip_head_is(view, view.wav.ask_text.c_str());
}

bool song_question_live(const ViewState& view) noexcept {
  if (!view.song.ask.live || view.screen != ScreenId::Song) {
    return false;
  }
  AskText head;
  song_ask_head(view.song.ask, head);
  return strip_head_is_ask(view, song_ask_kind(view.song.ask), head.c_str());
}

bool apply_ask_live(const ViewState& view) noexcept { return strip_head_is(view, kApplyAskStrip) || strip_head_is(view, kApplyAskStripReconnect); }

bool defaults_ask_live(const ViewState& view) noexcept { return strip_head_is_ask(view, Ask::Defaults, ""); }

PromptState prompt_in_force(const Model& model, const ViewState& view) noexcept {
  switch (view.disk_keys) {  // the boot disk's row (the app is the disk mode's screen: no overlay, no question)
    case DiskKeys::Restart: return owned(kDiskRestartPrompt, all_of(kDiskRestartPrompt), 0u);
    case DiskKeys::Confirm: return owned(kDiskConfirmPrompt, all_of(kDiskConfirmPrompt), all_of(kDiskConfirmPrompt));
    case DiskKeys::Finishing: return owned(kDiskFinishingPrompt, 0u, 0u);
    case DiskKeys::Off: break;
  }
  if (view.overlay != OverlayId::None) {  // a dialog owns the row: no question of the screen under it counts
    return owner_state(model, view);
  }
  switch (view.screen) {
    case ScreenId::File: return state_of(file_question(view));
    case ScreenId::Config:
      if (view.params.armed) {  // a press of F7 now writes the pending values (or hands the card over or takes it back: the CARD AS DISK row), whatever the strip says
        if (config_row(view) == ConfigRow::CardAsDisk) {  // (its strip is the platform's persistent one while the card is out: the row keeps following the arm)
          return state_of(view.disk.phase == ViewState::DiskView::Phase::Out ? Ask::Eject : Ask::Share);
        }
        // The pending values: the row follows the strip (OS-BUG-CFGARM, QA-CFG-034: the plain row is back when the text leaves, and param_apply drops the arm by the same test).
        if (apply_ask_live(view)) {
          return state_of(Ask::Apply);
        }
      }
      return view.params.defaults_armed && defaults_ask_live(view) ? state_of(Ask::Defaults) : PromptState{};
    case ScreenId::Modules:
      return model.modules != nullptr && model.modules->confirm_pending_for(static_cast<unsigned>(view.modules.apps)) && strip_head_is(view, model.modules->confirm_text()) ? state_of(Ask::Launch) : PromptState{};
    case ScreenId::Song: return song_question_live(view) ? state_of(song_ask_kind(view.song.ask)) : PromptState{};  // OS-SONGMAP
    case ScreenId::Record: return state_of(take_question(view));
    case ScreenId::SampleLoad: return sample_question_live(view) ? state_of(Ask::Replace) : PromptState{};  // OS-BUG-SLDOVER
    default: return PromptState{};
  }
}

bool patch_row(const PromptState& state, bool shifted, KeyRow& row) noexcept {
  if (state.prompt == nullptr) {
    return false;
  }
  bool changed = false;
  for (unsigned i = 0; i < state.prompt->count; ++i) {
    const KeyOption& o = state.prompt->options[i];
    if (((state.live >> i) & 1u) == 0u || (o.key >= 8u) != shifted) {
      continue;
    }
    const unsigned n = o.key & 7u;
    row.label[n] = o.label;
    if (((state.hot >> i) & 1u) != 0u) {
      row.hot = static_cast<std::uint8_t>(row.hot | (1u << n));
    }
    changed = true;
  }
  return changed;
}

bool owner_row(const PromptState& state, bool shifted, KeyRow& row) noexcept {
  const bool owned = state.prompt != nullptr && state.prompt->kind == PromptKind::Owner;
  unsigned shift_keys = 0;  // bit n = Shift + F(n+1) has an option
  for (unsigned i = 0; owned && i < state.prompt->count; ++i) {
    const KeyOption& o = state.prompt->options[i];
    shift_keys |= o.key >= 8u ? 1u << (o.key & 7u) : 0u;
  }
  const bool layer = shifted && shift_keys != 0u;  // a dialog without Shift options does not follow Shift: Shift + Fn acts as Fn there
  for (unsigned i = 0; i < 8; ++i) {
    row.label[i] = layer && ((shift_keys >> i) & 1u) == 0u ? "-" : "";  // (in a declared Shift layer a key with no option is bound to nothing)
  }
  row.dim = 0;
  row.hot = 0;
  if (!owned) {
    return false;
  }
  const bool every = state.prompt->every_key;
  for (unsigned i = 0; i < state.prompt->count; ++i) {
    const KeyOption& o = state.prompt->options[i];
    if (((state.live >> i) & 1u) == 0u || (o.key >= 8u) != layer) {
      continue;
    }
    const unsigned first = every ? 0u : (o.key & 7u);
    for (unsigned k = first; k <= (every ? 7u : first); ++k) {
      row.label[k] = o.label;
      if (((state.hot >> i) & 1u) != 0u) {
        row.hot = static_cast<std::uint8_t>(row.hot | (1u << k));
      }
    }
  }
  return true;
}

namespace {

// One key-cap of a 16 px strip: the box is as high as the strip's title chip (cap height and a border each side), the label after it in ChromeText. Returns the width, drawing or not.
int strip_hint(ui::Renderer& r, int x, int y, int h, const char* key, const char* label, bool draw) noexcept {
  const ui::Theme& t = r.theme();
  const int pad = t.metrics.pad;
  const int box_w = r.text_width(key, ui::Face::Label) + 2 * pad;
  if (draw) {
    const int box_h = t.geometry(ui::Face::Label).cap_h + 2 * t.metrics.border;
    const int text_y = ui::caps_y(t, ui::Face::Label, y, h);
    r.panel({x, y + (h - box_h) / 2, box_w, box_h}, ui::Color::ChromeText);
    r.text(x + pad, text_y, key, ui::Color::Chrome, ui::Face::Label);
    r.text(x + box_w + pad, text_y, label, ui::Color::ChromeText, ui::Face::Label);
  }
  return box_w + pad + r.text_width(label, ui::Face::Label);
}

// The hint chips of the live options that have words; `draw` false: only the width.
int hint_chips(ui::Renderer& r, int x, int y, int h, const PromptState& state, int gaps, bool draw, bool strip) noexcept {
  if (state.prompt == nullptr) {
    return 0;
  }
  const int gap = gaps * r.theme().metrics.gap;
  int used = 0;
  bool first = true;
  for (unsigned i = 0; i < state.prompt->count; ++i) {
    const KeyOption& o = state.prompt->options[i];
    const char* words = o.words != nullptr ? o.words : o.label;
    if (((state.live >> i) & 1u) == 0u || words[0] == '\0') {
      continue;
    }
    AskText key;
    append_key_name(key, o.key);
    used += first ? 0 : gap;
    first = false;
    if (strip) {
      used += strip_hint(r, x + used, y, h, key.c_str(), words, draw);
    } else if (draw) {
      used += draw_key_hint(r, x + used, y, h, key.c_str(), words);
    } else {
      used += r.text_width(key.c_str(), ui::Face::Label) + 2 * r.theme().metrics.pad + r.theme().metrics.pad + r.text_width(words, ui::Face::Label);  // as draw_key_hint
    }
  }
  return used;
}

}  // namespace

int draw_prompt_hints(ui::Renderer& r, int x, int y, int h, const PromptState& state, int gaps, bool strip) noexcept { return hint_chips(r, x, y, h, state, gaps, true, strip); }

int prompt_hints_width(ui::Renderer& r, const PromptState& state, int gaps, bool strip) noexcept { return hint_chips(r, 0, 0, 0, state, gaps, false, strip); }

void draw_owner_keys(ui::Renderer& r, const Model& model, const ViewState& view) noexcept {
  if (!view.overlay_keys()) {
    return;
  }
  KeyRow row{};
  if (!owner_row(prompt_in_force(model, view), view.shift, row)) {
    return;
  }
  const Frame f = make_frame(r.theme(), kFrameRows);
  r.panel(f.keys, ui::Color::Chrome);
  ui::soft_keys(r, f.keys, row.label, 8, row.dim, row.hot);
}

std::size_t describe_prompt_keys(const PromptState& state, char* out, std::size_t cap) noexcept {
  if (out == nullptr || cap == 0u) {
    return 0;
  }
  out[0] = '\0';
  if (state.prompt == nullptr) {
    return 0;
  }
  std::size_t n = 0;
  const auto put = [&](char c) noexcept {
    if (n + 1u < cap) {
      out[n++] = c;
      out[n] = '\0';
    }
  };
  for (unsigned i = 0; i < state.prompt->count; ++i) {
    const KeyOption& o = state.prompt->options[i];
    if (((state.live >> i) & 1u) == 0u || o.label[0] == '\0' || std::strcmp(o.label, "-") == 0) {
      continue;
    }
    const unsigned first = state.prompt->every_key ? 0u : (o.key & 7u);
    for (unsigned k = first; k <= (state.prompt->every_key ? 7u : first); ++k) {
      if (n != 0u) {
        put(',');
      }
      if (o.key >= 8u) {
        put('S');
      }
      put('F');
      put(static_cast<char>('1' + k));
      put(':');
      for (const char* c = o.label; *c != '\0'; ++c) {
        put(*c == ' ' ? '_' : *c);
      }
    }
  }
  return n;
}

std::size_t describe_keys(const Model& model, const ViewState& view, char* out, std::size_t cap) noexcept { return describe_prompt_keys(prompt_in_force(model, view), out, cap); }

void draw_question_keys(ui::Renderer& r, const ui::Rect& area, const Screen& screen, const Model& model, const ViewState& view, const char* const* plain) noexcept {
  const char* const* base = key_row(screen, view, plain);
  KeyRow row{};
  for (unsigned i = 0; i < 8; ++i) {
    row.label[i] = base[i];
  }
  (void)patch_row(prompt_in_force(model, view), view.shift && follows_shift(screen, view), row);
  ui::soft_keys(r, area, row.label, 8, row.dim, row.hot);
}

}  // namespace o3os::app
