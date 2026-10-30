// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/screens/screen.h>
#include <o3os/app/song_source.h>

namespace o3os::app {

// The Song screen's pages (pressing Song again cycles them): the order list, and the same list as an arrangement with
// each pattern's start bar, length in bars and a proportional bar. OS-SONG-TIMELINE: the visible slots as blocks scaled to bars, with a thumbnail of each track's note density.
// OS-SONG-VIEW2: the STAGE page is gone; the bar countdown to the next slot is on TIMELINE's info line while it plays. The cycle is ORDER, ARRANGEMENT, TIMELINE.
enum class SongPage : std::uint8_t { Order = 0, Arrangement = 1, Timeline = 2 };
inline constexpr unsigned kSongPages = 3;

// F1..F8 of the song screen: at most 8 characters, what a key-cap of the row has room for (the F-number box and the label
// fit 100 px). OS-SONG1: the row (Play Pattern, Play Song, Undo, Add Slot, Delete Slot, Pattern -, Pattern +,
// Tempo) with the undo moved to F8 (as on Pattern) and everything between it shifted one key to the left.
// OS-SONGMAP (Q1, to confirm): F5 UNIQUE and F6 TOOLS, where the slot's pattern - / + were (they stay on the encoder).
inline constexpr const char* kSongSoftKeys[8] = {"PLAY PAT", "PLAY SNG", "ADD SLOT", "DEL SLOT", "UNIQUE", "TOOLS", "TEMPO", "UNDO"};
// While Shift is held (OS-SHIFTROW): the quarter jumps and the end (bound in Song's context), Shift + F8 redo. The duplicate-as-new-pattern slot is Shift + Insert. Shift + F6 NAME names the pattern (OS-NAMES).
// Shift + F7 PADS switches the pads between TRACKS and SLOTS (OS-SONGPADS).
inline constexpr const char* kSongShiftKeys[8] = {"0/4", "1/4", "2/4", "3/4", "END", "NAME", "PADS", "REDO"};  // (OS-NAMES: F6 NAME, the slot's pattern)

// The order list (docs/design/product-ux.md section 4: playlist slots, play pattern / song, undo): position, pattern number, pattern name and
// length, the play position marker, the cursor. It is the attached project's order list (OS-005f; a pattern that does not exist yet is dimmed) and,
// with no project, the static demo list; SongSource is what it reads. Editing the order is song_actions.cpp (OS-SONG1).
class SongScreen final : public Screen {
 public:
  constexpr SongScreen() noexcept = default;
  void enter(const Model& model, ViewState& view) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::Song; }
  Context context() const noexcept override { return Context::Song; }
  unsigned pages() const noexcept override { return kSongPages; }
  const char* const* soft_keys() const noexcept override { return kSongSoftKeys; }
  const char* const* soft_keys_shifted() const noexcept override { return kSongShiftKeys; }
  // OS-SONGPADS: in SLOTS mode the pads are the page of 48 slots that holds the cursor: Back = the slot exists, Front = the slot that plays (stopped: the cursor's), a queued jump blinks, Off = past the song's end.
  // In TRACKS mode the pads are the layer's (App::pad_lights), and this has no opinion.
  bool pad_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) const noexcept override;
  // The cursor: the order list, one axis, the 16 visible positions a page. Every plain move collapses the selected range and puts the encoder back on the
  // slot (it may be on the tempo field); Left / Right do only that.
  int count(const Model& model, const ViewState&) const noexcept override { return SongSource(model).count(); }
  int position(const ViewState& view) const noexcept override { return view.song.pos(); }
  int page() const noexcept override { return SongView::kVisibleRows; }
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override {
    const bool left = view.song.leave_edit();
    return view.song.set(position, SongSource(model).count()) || left;
  }
  // OS-SONGMAP: Left / Right walk the column, PAT and then the tracks of the map (no wrap); the selection collapses either way. Up / Down keep the column.
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
  // The question of 1.6 is the screen's field level while it is up: F1 ALL and F2 UNIQUE (Context::SongAsk).
  Context field_context(const Model& model, const ViewState& view) const noexcept override;
};
extern const SongScreen kSongScreen;

// The cursor and the window back inside the list in view, which an action, an undo or another project attached may have changed under them
// (the App calls it after every action and when a document is attached). True if the view moved.
bool song_sync(const Model& model, ViewState& view) noexcept;

// How many cells of the track map the page draws: the ones the cursor can enter (OS-SONGMAP). 0 with no project, an empty song, or a page where the map does not fit.
int song_map_cells(const Model& model, const ViewState& view) noexcept;

}  // namespace o3os::app
