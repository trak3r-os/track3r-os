// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-KEYROWDLG (docs/specs/OS-KEYROWDLG.md; new Community OS behaviour): the F-key row says what the keys do while a question is up. One table per question, three readers: the key row, the strip's
// question text and the tests that resolve the keys against the bindings. The bindings stay the truth about what a key does; the table only says it again, and a test holds the two together.
//
// A QUESTION is a strip text whose answer is the next press of a key (F3 NEW asks ALL or KEEP INSTRUMENTS, OPEN with changes asks F1 again, ..). It is not modal: the other keys act, and take the question back
// (FileView::ask_clear()). So the row keeps every label of the screen and marks the answers: the key's word, inverted. A question only counts while its text is the head of the strip.
//
// A DIALOG (every modal overlay: Learn, the Navigator, the choosers, the pickers, the keyboard, the step popup, Fill, the export progress) OWNS the row: the dialog's own keys, every other key blank (it does nothing:
// the dialog's context captures). The row does not follow Shift, unless the prompt declares Shift options (the Navigator does). The dialog's hint line is made from the same table (draw_prompt_hints), and the
// scene paints the row once, after the overlay (draw_owner_keys), the screen under it drawing none (ViewState::overlay_keys). A key may have two options, one live at a time (Learn's CLR ALL and CLR ALL?).

#include <o3os/app/model.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/view_state.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/notify.h>
#include <o3os/ui/components.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// One key of a prompt. `key`: 0..7 = F1..F8, 8..15 = Shift + F1..F8. `label` is the key row's word (at most 8 characters, upper case). `words` is the phrase of the strip text: null = the label, "" = not in the text.
struct KeyOption {
  std::uint8_t key;
  const char* label;
  const char* words;
};

enum class PromptKind : std::uint8_t { Owner, Question };

struct KeyPrompt {
  PromptKind kind;
  const KeyOption* options;
  std::uint8_t count;
  bool every_key = false;  // an Owner whose options each stand for all of F1..F8 (the boot disk: any key or pad does it), so the table holds the word once and `key` is not read
};

// The eight labels a row draws, and the keys drawn dim and inverted (bit n = F(n+1)): ui::soft_keys' arguments.
struct KeyRow {
  const char* label[8];
  std::uint8_t dim;
  std::uint8_t hot;
};

// The prompt in force and, per option (bit n = option n), which are live and which are drawn inverted.
struct PromptState {
  const KeyPrompt* prompt = nullptr;
  std::uint16_t live = 0;
  std::uint16_t hot = 0;
};

// The questions: the ones the File, Config and Modules screens ask. Open and Import are the same question (F1 again, to discard the changes) on two pages of the File screen.
// Shared (OS-SONGMAP, 1.6): an edit of the Song map would also change a pattern that a slot outside the rectangle holds: F1 ALL changes it for every slot, F2 UNIQUE first gives the rectangle's slots copies.
// Replace (OS-BUG-SLDOVER): F1 again on Sample load, to load a WAV over an instrument that is there.
// Clear (OS-AUDIT-69): the Song map's Delete over 2 or more patterns: F1 YES clears, any other key withdraws.
// Share and Eject (OS-USBDISK U2b): F7 again on the CARD AS DISK row, to hand the card to the computer and to take it back (Apply's key, the same two presses).
// TakeDrop, TakeRec, TakeCrop (OS-QAFIX-E-SREC-09, OS-RECCROP): the Record screen's take under review, which F4 DISCARD, F1 / Rec (a new take over it) and Shift + F3 CROP would lose audio of: the same key again.
enum class Ask : std::uint8_t { None, New, Open, Import, Purge, Og, Apply, Defaults, Launch, Shared, Share, Eject, TakeDrop, TakeRec, TakeCrop, Replace, Clear };

using AskText = core::FixedString<core::kNotifyTextChars>;

// The heads of the strip texts that are not made by the caller (the part before the keys).
inline constexpr const char* kAskHeadTake = "TAKE NOT SAVED";
inline constexpr const char* kAskHeadCrop = "CROP KEEPS THE WINDOW ONLY";
inline constexpr const char* kAskHeadNew = "NEW PROJECT";
inline constexpr const char* kAskHeadUnsaved = "UNSAVED CHANGES";
inline constexpr const char* kAskHeadPurge = "PURGE ";

const KeyPrompt& ask_prompt(Ask which) noexcept;

// The name of a key as a text says it: "F3", "SHIFT+F7" (`key`: 0..7 = F1..F8, 8..15 = Shift + F1..F8). For a dialog whose line names the key it holds or releases (the PATTERN chooser's "release F1: close").
void append_key_name(AskText& out, std::uint8_t key) noexcept;

// The strip text of a question: `head` and then the keys of its table ("NEW PROJECT: F3 ALL, F1 KEEP INSTRUMENTS", "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM", "SHIFT+F7 AGAIN: RESET PAGE" with an empty head).
// ask_tail() is the part after the head (": F3 ALL, F1 KEEP INSTRUMENTS"), for a caller that must fit its own head in the strip's 48 characters.
void ask_tail(Ask which, AskText& out) noexcept;
void ask_text(Ask which, const char* head, AskText& out) noexcept;

// The strip texts of Config's first F7 on pending values (param_apply posts them). The arm (`params.armed`) has no clock of its own: it counts only while one of these texts is the head of the strip
// (OS-BUG-CFGARM). The key row reads APPLY? by the same test, and a press of F7 with the arm set but no text up (the two seconds are over, another message came) asks again instead of applying.
inline constexpr const char* kApplyAskStrip = "PRESS AGAIN TO APPLY";
inline constexpr const char* kApplyAskStripReconnect = "USB WILL RECONNECT: PRESS AGAIN";
bool apply_ask_live(const ViewState& view) noexcept;
// Shift + F7's question on Config (RESET PAGE): is its text the head of the strip (the same rule for its arm, `params.defaults_armed`)?
bool defaults_ask_live(const ViewState& view) noexcept;

// Sample load's question (OS-BUG-SLDOVER): up while its text is the head of the strip.
bool sample_question_live(const ViewState& view) noexcept;
// Is the question `which` (with `head`) the head of the strip now? A question that was answered, pushed down by another message or timed out is not up, whatever flag armed it.
bool ask_is_up(const ViewState& view, Ask which, const char* head) noexcept;

// The Song screen's question (OS-SONGMAP): its head ("PATTERN 03 ALSO IN 2 SLOTS", "3 PATTERNS ALSO IN 9 SLOTS": D11) that ask_text puts the keys after, and whether it is up (its edit waits, and its text is the head of the strip).
void song_ask_head(const SongAsk& ask, AskText& out) noexcept;
bool song_question_live(const ViewState& view) noexcept;
// Which of the two Song questions `ask` is (OS-AUDIT-69).
inline Ask song_ask_kind(const SongAsk& ask) noexcept { return ask.confirm ? Ask::Clear : Ask::Shared; }

// The question the File screen has up, Ask::None if none: its flag is set and its text is the head of the strip (a question queued behind another message answers nothing, so it marks nothing).
Ask file_question(const ViewState& view) noexcept;

// The dialog prompt of a modal overlay, nullptr for None and Notify. A test walks every OverlayId and fails on a modal overlay that has none.
const KeyPrompt* owner_prompt(OverlayId id) noexcept;

// The prompt in force: the dialog that owns the row if a modal overlay is up, else the question of the screen in view. Pure: reads the model and the view.
PromptState prompt_in_force(const Model& model, const ViewState& view) noexcept;

// A dialog's row: every key blank except the live options, the Shift layer only if the prompt declares one (`shifted` and a Shift option: that layer, else the plain; in that layer a key with no option reads "-":
// it is bound to nothing, the Navigator's Shift + F1..F6). True if the prompt owns a row.
bool owner_row(const PromptState& state, bool shifted, KeyRow& row) noexcept;

// A question's answers over a screen's own row: the answer keys of the layer in view (`shifted`: the Shift layer) take their word and are inverted, every other key keeps its label. True if anything changed.
bool patch_row(const PromptState& state, bool shifted, KeyRow& row) noexcept;

// The dialog line: a key-cap and its words for every live option that has words, left to right from `x`, `gaps` of the theme's gap between them (none after the last). Returns the width drawn.
// `strip`: the caps for a 16 px strip (the choosers' band over the info line): as high as the strip's own title chip, so they touch nothing above or below.
int draw_prompt_hints(ui::Renderer& r, int x, int y, int h, const PromptState& state, int gaps, bool strip = false) noexcept;

// The width draw_prompt_hints would draw, for a dialog that right-aligns its chips.
int prompt_hints_width(ui::Renderer& r, const PromptState& state, int gaps, bool strip = false) noexcept;

// The F-key row of the dialog in force, on the frame band: render_scene calls it after the overlay, once. Nothing is drawn when no dialog owns the row.
void draw_owner_keys(ui::Renderer& r, const Model& model, const ViewState& view) noexcept;

// The prompt in force as the state line (`diag state`, ` keys=`) says it: the live keys in the order of the table, "F3:ALL,F1:KEEP_INS", a Shift key as "SF7:PURGE?", a space of a label as `_`, the inert keys ("-") left out.
// "" (and nothing in the line) when no prompt is up or none of its keys acts (the boot disk while its restart waits): a test host reads which keys the app says, not pixels. At most kKeysLineChars characters.
inline constexpr std::size_t kKeysLineChars = 90;
std::size_t describe_prompt_keys(const PromptState& state, char* out, std::size_t cap) noexcept;
std::size_t describe_keys(const Model& model, const ViewState& view, char* out, std::size_t cap) noexcept;

// The key row of a screen that can ask a question (File, Config, Modules): the row key_row() chooses, with the answers of the question in force marked.
void draw_question_keys(ui::Renderer& r, const ui::Rect& area, const Screen& screen, const Model& model, const ViewState& view, const char* const* plain) noexcept;

}  // namespace o3os::app
