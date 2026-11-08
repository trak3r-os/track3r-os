// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/song_edit.h>
#include <o3os/app/song_map.h>
#include <o3os/app/song_preview.h>
#include <o3os/core/tick.h>

#include <cstdint>

namespace o3os::app {

// The edit that waits for the answer of the question of 1.6 (a paste, a clear or a tool that would also change a pattern a slot outside the rectangle holds): what it was asked for, and what the question names. It
// does not survive another key, an undo, a NEW or a screen change (song_question_sync, song_map_actions.cpp).
struct SongAsk {
  static constexpr std::uint32_t kMs = 4000;  // how long the question's text stays on the strip (as the File screen's NEW question)
  bool live = false;
  bool confirm = false;            // OS-AUDIT-69: the question is "CLEAR n PATTERNS?" (F1 YES, any other key withdraws), not the shared-pattern one; `shared` then holds n
  bool cut = false;                // with `confirm`: the clear is a Cut's; the song clip is filled on F1 YES only, so a withdrawn cut leaves the clip as it was
  SongEdit edit;                  // the rectangle and the tool (a Paste reads the App's song clip when it is answered)
  std::uint32_t generation = 0;    // the project's generation when it was asked: anything that edits the project in between withdraws it
  std::uint8_t first = 0;          // the pattern the question names when it is one
  std::uint8_t shared = 0;         // how many patterns it names
  std::uint8_t others = 0;         // the slots outside the rectangle that hold them (at most 255)
};

// The Song screen's cursor and window: one position of the order list, and the first of the 16 rows shown. The list's
// length comes from the model each time (the order list grows and shrinks as it is edited), so every move says it.
// Each move returns true if the visible state changed.
//
// OS-SONGMAP: the track map beside the list is a grid the cursor can enter (docs/specs/SONG-UX.md section 1). The cursor has a column, PAT (the order list's own field) or a track of the map; Shift + Left / Right
// extend a range of tracks as Shift + Up / Down extend the slots, and the two make the rectangle that Copy, Delete, the TOOLS dialog and UNIQUE act on. All of it is view state.
//
// OS-SONG1 adds what editing the list needs, all of it view state (nothing here is saved or undone): a range of slots selected with Shift + the
// arrows (the anchor is where Shift was first used; the range runs from there to the cursor, as the pattern's block), whether the encoder is on the
// tempo field (F7) instead of the slot, the tap-tempo timer, and the clipboard Copy fills and Shift + Copy inserts.
class SongView {
 public:
  static constexpr int kVisibleRows = 16;
  static constexpr int kMaxStep = 1 << 16;  // larger moves are clamped to this before they are added
  static constexpr int kClipMax = 255;      // slots the clipboard holds: the longest order list (model::kMaxSongPositions; song_actions.cpp checks)
  static constexpr int kLanes = 12;         // OS-SONG3 Q8: the lanes (track cells) drawn at once on either page; a list with more active tracks scrolls them with the track cursor
  static constexpr int kPadSlots = 48;      // OS-SONGPADS: the SLOTS pads are a page of 48 slots (pad_lights.h kPadCount; song_screen.cpp checks)

  int pos() const noexcept { return pos_; }
  int top() const noexcept { return top_; }
  // OS-SONGPADS: the first slot of the page of 48 that holds the cursor in a list of `count` positions (pad n of the SLOTS mode is slot pad_base(count) + n; the cursor is read clamped into the list, as the screen draws it,
  // so the lights and a tap agree); the page moves when the cursor leaves it.
  int clamped_pos(int count) const noexcept { return count > 0 && pos_ >= count ? count - 1 : pos_; }
  int pad_base(int count) const noexcept { return clamped_pos(count) / kPadSlots * kPadSlots; }

  bool set(int pos, int count) noexcept;    // to `pos`, clamped to 0..count-1; the window follows
  bool move(int delta, int count) noexcept;
  bool fit(int count) noexcept;             // after the list changed: the cursor and the window inside it (the selection too)

  // ---- the track column (OS-SONGMAP) -----------------------------------------------------------------------------------------------------
  // `cells` is how many cells of the map are drawn (song_map_cells, song_screen.h; 0: no map). col 0 is PAT, 1..cells the track col - 1. Left / Right walk it without wrapping; Up / Down keep it.
  int col() const noexcept { return col_; }
  bool in_map() const noexcept { return col_ > 0; }  // the cursor is in a track cell (on TIMELINE: a lane; col 0 is then the block row, the whole slot)
  int track() const noexcept { return col_ - 1; }  // the cursor's track, -1 on PAT
  bool step_col(int delta, int cells) noexcept;    // Left / Right: the selection collapses, the column moves one (clamped), true if either changed
  bool set_track(int track, int cells) noexcept;   // the pads' row 1: the cursor to that track of the map; false with no such cell
  bool extend_tracks(int delta, int cells) noexcept;  // Shift + Left / Right, in the map only: the tracks of the rectangle grow from where Shift was first used
  bool fit_cols(int cells) noexcept;               // after the map changed (a page, a project, an undo): the column and the track range inside it
  int tbase() const noexcept { return tbase_; }    // OS-SONG3 Q8: the first track drawn: the window of kLanes tracks follows the cursor's, and is 0 while the cursor is on PAT or the block row
  bool track_selecting() const noexcept { return tanchor_ >= 0; }
  int tfirst() const noexcept { return tanchor_ >= 0 && tanchor_ < col_ - 1 ? tanchor_ : col_ - 1; }  // the tracks of the rectangle (0-based), the cursor's alone with no range
  int tlast() const noexcept { return tanchor_ >= 0 && tanchor_ > col_ - 1 ? tanchor_ : col_ - 1; }

  // ---- the selected range of slots -----------------------------------------------------------------------------------
  bool selecting() const noexcept { return anchor_ >= 0; }
  int first() const noexcept { return anchor_ >= 0 && anchor_ < pos_ ? anchor_ : pos_; }  // the range, or just the cursor's slot when there is none
  int last() const noexcept { return anchor_ >= 0 && anchor_ > pos_ ? anchor_ : pos_; }
  bool in_range(int pos) const noexcept { return pos >= first() && pos <= last(); }
  bool extend(int delta, int count) noexcept;  // Shift + Up / Down: the anchor is set where the cursor is, the cursor moves
  bool shift_range(int delta, int count) noexcept;  // OS-SONGSHARE S3: the slots moved one position (delta = +-1): the cursor and the far end of the range follow, the column and the tracks of the rectangle stay
  void select(int anchor) noexcept { anchor_ = anchor; }  // the range from `anchor` (a paste selects what it inserted)

  // ---- the tempo field (the encoder edits it instead of the slot's pattern) ------------------------------------------------
  bool tempo_focus() const noexcept { return tempo_focus_; }
  void set_tempo_focus(bool on) noexcept { tempo_focus_ = on; }
  // A plain cursor move or Left / Right: the range collapses and the encoder is back on the slot. True if either was set.
  // OS-SONGMAP: the range of tracks collapses too; the column stays.
  bool leave_edit() noexcept {
    const bool had = anchor_ >= 0 || tanchor_ >= 0 || tempo_focus_;
    anchor_ = -1;
    tanchor_ = -1;
    tempo_focus_ = false;
    return had;
  }

  // ---- tap tempo (F8): the time of the last tap and how many taps this run has had ------------
  core::Tick tap_at = 0;
  std::uint8_t taps = 0;

  // ---- the track map of each row (song_map.h): a cache, filled while drawing -------------------------------------------------------
  SongTrackMap map;

  // ---- how many slots hold each pattern (song_map.h, OS-SONGSHARE S1): a cache of 256 bytes, filled while drawing, rebuilt when the order list changes ---------------------------------------
  SongUseCount use;

  // ---- the clipboard: pattern numbers of the copied slots ----------------------------------------------------------------------
  std::uint8_t clip[kClipMax] = {};
  int clip_count = 0;

  // ---- the question (OS-SONGMAP 1.6) --------------------------------------------------------------------------------------------------------
  SongAsk ask;

  // ---- the PREVIEW audition (OS-SONG3 S4, song_preview.h) -------------------------------------------------------------------------------------
  // F1 in a lane of the TIMELINE page: the audition's state (the saved solo set and the rectangle's mask). View state: not saved, not an undo unit; F1 again, Stop and leaving
  // the screen end it (song_preview_end), a project load drops it (App::attach_document), so no solo is ever left stuck.
  SongPreview preview;
  bool preview_resync = false;  // a Solo push found the ring full: the next main-loop pass re-sends every strip's mute / solo (song_solo_resync, App::run_once)

 private:
  int pos_ = 0;
  int top_ = 0;
  int anchor_ = -1;  // -1: no range
  int col_ = 0;      // OS-SONGMAP: 0 = PAT, n = track n - 1 of the map
  int tanchor_ = -1; // the far end of the range of tracks (0-based), -1: none
  int tbase_ = 0;    // the first track of the kLanes window (follow_track)
  void follow_track(int cells) noexcept;  // the window on the cursor's track after the column moved, inside 0..cells - kLanes
  bool tempo_focus_ = false;
};

}  // namespace o3os::app
