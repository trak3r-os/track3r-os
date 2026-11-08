// SPDX-License-Identifier: GPL-3.0-or-later
// The Perform screen's actions (OS-011b): the performance mode on the slot bank (seq/perform.h has the rules). A pad is a slot's row, the encoder
// the live value of the held slots, a function key a track that joins the layer, Rec the EDIT state in which the arrows and Copy shape the slots. Nothing here is undoable: the
// slots are runtime state, not the project, and the bank tells the engine through the sequencer's command ring.
#include "action_handlers.h"

#include <o3os/app/song_source.h>

namespace o3os::app {

// (OS-DJ A1: the DJ page is a page of this screen; the slots' keys, pads and encoder are inert there, the DJ context's bindings are dj_actions.cpp's)
// The Perform screen's slot page is in view: the mapped Perform CCs act here only (OS-MIDICCMAP Q5 a).
bool on_perform(const ActionContext& ctx) noexcept { return ctx.view.screen == ScreenId::Perform && ctx.perform != nullptr && !ctx.view.dj_page(); }

namespace {

seq::PerformBank* bank_of(ActionContext& ctx) noexcept { return ctx.perform != nullptr ? &ctx.perform->bank : nullptr; }

bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept { return say_tag(ctx, severity, text, "perform"); }

// The function keys (OS-PERF2).
constexpr unsigned kKeyHalf = 3;
constexpr unsigned kKeyDouble = 4;
constexpr unsigned kKeyLatch = 5;
constexpr unsigned kKeyNext = 6;
constexpr unsigned kKeyReset = 7;

// The step rate the held keys ask for: F4 alone half time, F5 alone double time, both (or neither) the song's own.
unsigned held_rate(const PerformKeys& k) noexcept {
  const bool half = ((k.down >> kKeyHalf) & 1u) != 0u;
  const bool dbl = ((k.down >> kKeyDouble) & 1u) != 0u;
  return half && !dbl ? 1u : (dbl && !half ? 2u : 0u);
}

}  // namespace

// The song position that follows the pattern that plays (the song loops), for NEXT PATTERN and the DJ page's TRANS pads: nullptr and `row` set, or the strip text that says why there is none.
const char* next_song_row(const ActionContext& ctx, unsigned& row) noexcept {
  const SongSource src(ctx.model);
  const int count = src.count();
  if (count <= 0) {
    return "SONG IS EMPTY";
  }
  int pos = ctx.view.play.order != audio::PlayPosition::kNoOrder ? static_cast<int>(ctx.view.play.order) : -1;
  for (int i = 0; pos < 0 && i < count; ++i) {  // Pattern mode: where the pattern that plays sits in the order list (its first place)
    pos = src.pattern(i) == ctx.view.play.pattern ? i : -1;
  }
  if (pos < 0) {
    return "PATTERN IS NOT IN THE SONG";
  }
  row = static_cast<unsigned>(pos + 1 < count ? pos + 1 : 0);
  return nullptr;
}

namespace {

// NEXT PATTERN: the next position of the song order is queued with the live jump (TransportSink::queue_jump, OS-SONG1: a command on the sequencer's ring, taken at the end of the pattern
// now playing); the key again, while it waits, cancels. Only while the sequencer says it plays (what it published, not what was asked: a jump sent before the render has seen the Play would
// be dropped there), and the blink is the published `queued` too, never what was asked. The jump wins over a QueuePattern at the pattern's end (no key sends one today).
bool next_pattern(ActionContext& ctx) noexcept {
  if (ctx.transport == nullptr) {
    return false;
  }
  if (!ctx.view.play.playing) {
    return say(ctx, core::Severity::Info, "NOT PLAYING");
  }
  if (ctx.view.play.queued != audio::PlayPosition::kNoOrder) {
    if (!ctx.transport->queue_jump(0xFFFFu)) {
      return false;
    }
    return say(ctx, core::Severity::Info, "NEXT PATTERN CANCELLED");
  }
  unsigned row = 0;
  if (const char* why = next_song_row(ctx, row)) {
    return say(ctx, core::Severity::Info, why);
  }
  if (!ctx.transport->queue_jump(row)) {
    return false;
  }
  return say(ctx, core::Severity::Info, "NEXT PATTERN QUEUED");
}

}  // namespace

namespace {

// A pattern id as two digits at least ("07", "123").
void append_pat(core::FixedString<40>& text, unsigned id) noexcept {
  char d[4] = {'0', '0', '0', '\0'};
  d[0] = static_cast<char>('0' + id / 100u);
  d[1] = static_cast<char>('0' + (id / 10u) % 10u);
  d[2] = static_cast<char>('0' + id % 10u);
  text.append(id >= 100u ? d : d + 1);
}

// E-PERF-07, Shift + F4..F8: the per-track queued pattern. The tracks that are ON (the affected mask) change their source pattern at their own loop end (Sequencer::queue_track_pattern; a
// landed pick stays until TRK SONG, Stop or a restart of the transport). PICK -/+ walk the patterns the project has, TRK PAT queues the pick, TRK NEXT the pattern of the next song slot (the rule of NEXT PAT), TRK SONG
// sends the tracks back to the song's. The same queue again takes it back. Only while the sequencer plays; no track ON, a full ring: a hint, nothing sent.
bool queue_tracks(ActionContext& ctx, unsigned pattern) noexcept {
  if (!ctx.view.play.playing) {
    return say(ctx, core::Severity::Info, "NOT PLAYING");
  }
  const std::uint32_t mask = ctx.perform->bank.affected_mask() & ((std::uint32_t{1} << seq::kPerfTracks) - 1u);
  if (mask == 0u) {
    return say(ctx, core::Severity::Info, "NO TRACK IS ON: HOLD SHIFT + TOP PADS");
  }
  if (pattern != 0u && (ctx.model.project == nullptr || ctx.model.project->pattern(pattern) == nullptr)) {
    return say(ctx, core::Severity::Info, "PATTERN IS GONE: PICK ANOTHER");  // the pick or the song slot may name a pattern that was deleted since
  }
  if (!ctx.perform->sink().push(seq::Command{seq::Command::Kind::QueueTrackPattern, 0, static_cast<std::uint16_t>(pattern), mask})) {
    return say(ctx, core::Severity::Warn, "COMMANDS BUSY: TRY AGAIN");
  }
  core::FixedString<40> text;
  if (pattern == 0u) {
    text.assign("TRACKS GO BACK TO THE SONG");
  } else {
    text.assign("TRACKS QUEUED: PAT ");
    append_pat(text, pattern);
  }
  return say(ctx, core::Severity::Info, text.c_str());
}

bool pick_pattern(ActionContext& ctx, int dir) noexcept {
  if (ctx.model.project == nullptr) {
    return false;
  }
  PerformBoard& board = *ctx.perform;
  int id = board.pick != 0u ? board.pick : ctx.view.play.pattern;
  for (int i = 0; i < 255; ++i) {
    id += dir;
    id = id > 255 ? 1 : (id < 1 ? 255 : id);
    if (ctx.model.project->pattern(static_cast<unsigned>(id)) != nullptr) {
      board.pick = static_cast<std::uint8_t>(id);
      core::FixedString<40> text;
      text.assign("PICK: PAT ");
      append_pat(text, static_cast<unsigned>(id));
      return say(ctx, core::Severity::Info, text.c_str());
    }
  }
  return say(ctx, core::Severity::Info, "NO PATTERN");
}

}  // namespace

bool perform_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!on_perform(ctx) || arg < 0 || !bank_of(ctx)->pad_down(static_cast<unsigned>(arg), ctx.perform->sink())) {
    return false;
  }
  if (arg < 64) {
    ctx.perform->pads |= std::uint64_t{1} << arg;  // this pad's release is the bank's, also on Master / Song (App::pump)
  }
  return true;
}

// The release of a pad: also when something else took the screen in between (the pad is let go of whatever is in view).
bool perform_pad_up(ActionContext& ctx, core::ActionArg arg) noexcept {
  return ctx.perform != nullptr && arg >= 0 && bank_of(ctx)->pad_up(static_cast<unsigned>(arg), ctx.perform->sink());
}

bool perform_turn(ActionContext& ctx, core::ActionArg arg) noexcept { return on_perform(ctx) && bank_of(ctx)->turn(static_cast<int>(arg), ctx.perform->sink()); }

bool perform_track(ActionContext& ctx, core::ActionArg arg) noexcept {
  return on_perform(ctx) && arg >= 0 && bank_of(ctx)->toggle_track(static_cast<unsigned>(arg), ctx.perform->sink());
}

bool perform_edit(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_perform(ctx)) {
    return false;
  }
  seq::PerformBank& bank = *bank_of(ctx);
  bank.set_edit(!bank.edit());
  return true;
}

// Left / Right in EDIT: the edit place.
bool perform_move(ActionContext& ctx, core::ActionArg arg) noexcept { return on_perform(ctx) && bank_of(ctx)->edit() && bank_of(ctx)->move_edit(static_cast<int>(arg)); }

// Shift + Left / Right in EDIT: the slot under the edit place swaps with its neighbour and the place goes with it.
bool perform_swap(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!on_perform(ctx) || !bank_of(ctx)->edit() || arg == 0) {
    return false;
  }
  seq::PerformBank& bank = *bank_of(ctx);
  const unsigned from = bank.edit_slot();
  const int to = static_cast<int>(from) + (arg > 0 ? 1 : -1);
  if (to < 0 || to >= static_cast<int>(seq::kPerfSlots)) {
    return false;
  }
  const bool swapped = bank.swap_slots(from, static_cast<unsigned>(to), ctx.perform->sink());
  (void)bank.move_edit(arg > 0 ? 1 : -1);
  return swapped;
}

// Up / Down in EDIT: the FX of the slot under the edit place, one place.
bool perform_fx(ActionContext& ctx, core::ActionArg arg) noexcept {
  return on_perform(ctx) && bank_of(ctx)->edit() && bank_of(ctx)->change_fx(bank_of(ctx)->edit_slot(), static_cast<int>(arg), ctx.perform->sink());
}

bool perform_copy(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_perform(ctx) || !bank_of(ctx)->edit()) {
    return false;
  }
  bank_of(ctx)->copy_slot(bank_of(ctx)->edit_slot());
  return true;
}

bool perform_paste(ActionContext& ctx, core::ActionArg) noexcept {
  return on_perform(ctx) && bank_of(ctx)->edit() && bank_of(ctx)->paste_slot(bank_of(ctx)->edit_slot(), ctx.perform->sink());
}

// F1..F8 pressed on the Perform screen (arg 0..7; OS-PERF2). F1..F3 note the time (a scene key acts at its release, or at the hold: App::poll_perform stores it), F4 / F5 ask for half /
// double time for as long as they are held, F6 toggles LATCH, F7 queues the next pattern, F8 puts every FX off.
//
// OS-SHIFTROW: arg 0x100 + n (Shift + F1..F3, STORE n) stores scene n at once, with a visible key: no hold, nothing to release.
bool perform_key(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (on_perform(ctx) && (arg & 0x100) != 0) {
    const unsigned scene = static_cast<unsigned>(arg & 0xFF);
    switch (arg) {  // E-PERF-07: Shift + F4..F8 (0x103..0x107), the per-track queued pattern
      case 0x103:
      case 0x104:
        return pick_pattern(ctx, arg == 0x103 ? -1 : 1);
      case 0x105:
        return ctx.perform->pick != 0u ? queue_tracks(ctx, ctx.perform->pick) : say(ctx, core::Severity::Info, "PICK A PATTERN FIRST: PICK - / PICK +");
      case 0x106: {
        unsigned row = 0;
        if (const char* why = next_song_row(ctx, row)) {
          return say(ctx, core::Severity::Info, why);
        }
        return queue_tracks(ctx, SongSource(ctx.model).pattern(static_cast<int>(row)));
      }
      case 0x107:
        return queue_tracks(ctx, 0u);
      default:
        break;
    }
    static constexpr const char* kStored[seq::kPerfScenes] = {"SCENE 1 STORED", "SCENE 2 STORED", "SCENE 3 STORED"};
    if (scene >= seq::kPerfScenes) {
      return false;
    }
    (void)ctx.perform->bank.store_scene(scene);
    return say(ctx, core::Severity::Info, kStored[scene]);
  }
  if (!on_perform(ctx) || arg < 0 || arg > 7) {
    return false;
  }
  PerformBoard& board = *ctx.perform;
  const unsigned key = static_cast<unsigned>(arg);
  const std::uint8_t bit = static_cast<std::uint8_t>(1u << key);
  switch (key) {
    case kKeyHalf:
    case kKeyDouble:
      board.keys.down = static_cast<std::uint8_t>(board.keys.down | bit);
      board.bank.set_rate(held_rate(board.keys));
      (void)board.bank.apply(board.sink());
      return true;
    case kKeyLatch:
      board.bank.set_latch(!board.bank.latch());
      return true;
    case kKeyNext:
      return next_pattern(ctx);
    case kKeyReset:
      return board.bank.all_off(board.sink());
    default:  // a scene key
      board.keys.down = static_cast<std::uint8_t>(board.keys.down | bit);
      board.keys.fired = static_cast<std::uint8_t>(board.keys.fired & ~bit);
      board.keys.at[key] = ctx.now;
      return false;
  }
}

// The release of a key the board took (the App sends it as a raw event, whatever is in view: a key let go after the screen changed still ends its job). F4 / F5: the rate goes back; a scene
// key whose hold did not store: a tap, the scene is recalled.
bool perform_key_up(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.perform == nullptr || arg < 0 || arg > 7) {
    return false;
  }
  PerformBoard& board = *ctx.perform;
  const unsigned key = static_cast<unsigned>(arg);
  const std::uint8_t bit = static_cast<std::uint8_t>(1u << key);
  if ((board.keys.down & bit) == 0u) {
    return false;
  }
  board.keys.down = static_cast<std::uint8_t>(board.keys.down & ~bit);
  if (key == kKeyHalf || key == kKeyDouble) {
    board.bank.set_rate(held_rate(board.keys));
    (void)board.bank.apply(board.sink());
    return true;
  }
  if (key < seq::kPerfScenes) {
    if ((board.keys.fired & bit) != 0u) {
      board.keys.fired = static_cast<std::uint8_t>(board.keys.fired & ~bit);  // the hold stored it
      return false;
    }
    if (!board.bank.recall_scene(key, board.sink())) {
      return say(ctx, core::Severity::Info, "SCENE IS EMPTY: HOLD THE KEY TO STORE");
    }
    return true;
  }
  return false;
}

}  // namespace o3os::app
