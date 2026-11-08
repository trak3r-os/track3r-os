// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/pattern_data.h>
#include <o3os/app/pattern_edit.h>
#include <o3os/app/pattern_source.h>
#include <o3os/app/system_load.h>
#include <o3os/ui/renderer.h>

namespace o3os::app {

struct PatternLayout;

// The extent of the pattern in view: steps (the cursor's main axis) and tracks. The defaults are the demo's.
struct PatternDims {
  int steps = kSteps;
  int tracks = kTracks;
};

// The pattern screen (docs/design/product-ux.md section 4), restyled in OS-003b (docs/ui/style-guide.md): two
// materials, a chrome status strip with track headers on top and an info line and F-key key-caps below, the data pane
// between them with the step grid in the Data face. It draws from (model, layout, theme): pattern_layout.h derives
// every rectangle from the theme's metrics and the faces' geometry, and the theme comes with the Renderer.
//
// The cursor is (row, track, column); the view window follows it: the step window centres the cursor's row at both ends of the pattern too (kCentreRow, window_top: OS-CENTER, OS-PATCENTER), the track window scrolls at its edges. At the default zoom level 5 of the tracks and 16 of the steps are visible
// (OS-ZOOM: Note + Instr cycles the zoom, 5 tracks with every field, 8 with Note, Instr and Vol, 12 with Note; the level is RAM-only, it starts at 5 and survives a project load): 16 rows at
// the 24 px pitch are what 480 px leave under the chrome, and 5 tracks are what 800 px hold at 10 px per Data cell. A step is
// drawn from tiles (hex pairs, note names): one display-list word a field, so the worst case, every field filled, is about
// half the budget (sim/budget.h; the `budget` tests pin it).
//
// What it draws comes from a PatternSource (pattern_source.h: the attached document's pattern, or the demo) and a Status (what the
// screen worked out besides the grid: the strip's numbers, the info line's chip and scale, the selection, the Shift layer). Its size
// is not fixed any more (OS-009 P1a.1): every move takes the Dims of the pattern in view, the number of steps (the longest of the
// active tracks) and of tracks. The defaults are the demo's, which is what the OS-001b tests and the sweeps of the simulator use.
class PatternView {
 public:
  static constexpr int kVisibleRows = 16;
  static constexpr int kVisibleTracks = 5;      // the default zoom level's window, and the other screens' (the Pattern grid is 5, 8 or 12 wide: visible_tracks())
  static constexpr int kMaxVisibleTracks = 12;  // the widest zoom level (OS-ZOOM): the size of the per-track arrays
  static constexpr int kZoomCount = 3;
  static constexpr std::uint32_t kDemoTempoX100 = 12800;  // what the demo says: BPM 128.0
  // OS-CENTER (new Community OS behaviour: seven rows above it): the slot the cursor's row holds while the pattern
  // scrolls under it, counted from the top of the window (0..kVisibleRows-1).
  static constexpr int kCentreRow = 7;
  // The first row shown for a cursor on `row`: the row sits in the centre slot, always. OS-PATCENTER (maintainer, 2026-10-17; new Community OS behaviour): the window no longer stops at the first page and at the last, so at
  // row 0 the slots above it are blank and at the last row the slots below it are blank (the draw skips a row outside 0..steps-1: no number, no fields, no rule). The result is negative while the cursor is within
  // kCentreRow rows of the start. A pure function: the window is always where the cursor puts it, however it got there (arrows, encoder, jumps, the playhead of Browse, song follow); the same at every zoom level
  // (OS-ZOOM changes the tracks and fields, never the 16 rows of the window).
  static constexpr int window_top(int row) noexcept { return row - kCentreRow; }

  using Dims = PatternDims;

  static constexpr std::size_t kInfoNameChars = 14;  // OS-NAMES: the pattern name's room on the info line (a stored name has up to 30)

  // What the app shows besides the grid.
  struct Status {
    int instrument;                       // the last used instrument 1..64 (status strip)
    const char* const* soft_keys;         // 8 labels, F1..F8, owned by the caller (the screen's tables)
    const char* instrument_name = "";     // its name; "" shows "--"
    const char* chip = "WATCH";           // the mode chip of the info line (WATCH, STEP, LIVE: OS-RECMODE2)
    static constexpr std::uint8_t kToneNone = 0, kToneStep = 1, kToneLive = 2;
    std::uint8_t chip_tone = kToneNone;   // its colour: the plain chip, STEP in the Accent token (amber), LIVE in the Rec token (red, with a dot)
    const char* scale = "No scale";       // the pad board's scale (the names of core/scales.h)
    std::uint8_t root = 36;               // and its root note (36 = C-3)
    unsigned undo = 0;                    // the journal's depth: "undo n"
    int pattern = PatternSource::kDemoPattern;    // strip: PAT nn/nn
    int patterns = PatternSource::kDemoPatterns;
    const char* pattern_name = "";        // OS-NAMES: the pattern's name; "" says nothing, a name is "P05 VERSE" on the info line (cut to kInfoNameChars). Owned by the caller for the call of draw()
    int jump = PatternSource::kDemoJump;          // strip: JUMP nn (the edit step)
    Selection selection{};                // the range to paint; with none the cursor step is the selection
    bool shift = false;                   // Shift is held: the header marks are drawn bold
    std::uint32_t soft_dim = 0;           // key row: bit n = F(n+1)'s label is drawn dim (a reserved key of the Shift picker)
    std::uint32_t soft_hot = 0;           // key row: bit n = F(n+1) is the key in force (the Shift picker's mode): an inverted label
    const char* pads = "";                // the pads' mode (info line: "pads TRACKS", "pads NOTES" too since OS-INFOLINE); "" says nothing
    bool playing = false;                 // the transport runs: the strip's badge says PLAY (OS-005d)
    bool metro = false;                   // OS-METRO: the metronome is on (the device setting METRO is not Off): a METRO chip beside the mode chip in the info line
    bool rec = false;                     // the pattern can be edited (OS-RECSIGN: Edit, STEP and LIVE): the Rec frame round the grid and the REC badge in the strip
    bool recording = false;               // a take is running (LIVE, playing, past the count-in; OS-REVIEW): the playhead bar is Rec red
    int play_row = -1;                    // the step the sequencer is on in this pattern, -1 none: a bar in the Play colour (OS-009b)
    SystemLoad load{};                    // the strip's CPU and MEM (OS-FU3)
    std::uint32_t tempo_x100 = kDemoTempoX100;  // the strip's BPM, x 100: the sequencer's while it plays, else the project's (OS-009b)
    bool blink = false;                   // the phase of the MUTE tag of a track whose quantised mute is pending: lit or not (OS-MUTEVIS; ViewState::mute_blink)
    std::uint8_t numbering = static_cast<std::uint8_t>(Numbering::From1);  // the device setting NUMBERING: how the rows are numbered (OS-CFGWIRE; this OS's default counts from 1)
    std::int8_t octave_shift = 0;         // the device setting MIDDLE C as an octave shift of the note names (OS-CFGWIRE; 0 = the default names)
    const char* hint = "";                // OS-PTOOLS2 S1: while a block is active, what its keys do ("BLOCK 16 STEPS  ENC: TRANSPOSE  F7: TOOLS  PAD: SET  DEL: CLEAR"): parts separated by two spaces; draw_info drops the last parts until it fits, and it replaces fx_read. "" says nothing. Owned by the caller for the call of draw()
    const char* fx_read = "";             // the FX under the cursor in words (OS-PFX, app/fx_text.h: Chord, Arp, Tune, Slide); "" says nothing. Owned by the caller for the call of draw()
  };

  // Navigation. Each returns true if the visible state changed.
  bool move_left(const Dims& d = Dims{}) noexcept;   // previous column; from Note to the last visible column of the previous track
  bool move_right(const Dims& d = Dims{}) noexcept;  // next column; from the last visible column to the Note column of the next track (the hidden fields of a zoom level are skipped)
  bool move_up(const Dims& d = Dims{}) noexcept;
  bool move_down(const Dims& d = Dims{}) noexcept;
  bool scroll_rows(int delta, const Dims& d = Dims{}) noexcept;  // encoder: moves the cursor `delta` steps, clamped to the pattern
  bool select_column(Column column) noexcept;
  bool cycle_fx_column() noexcept;       // FX button: FX1, then FX2, then FX1 again
  // OS-ZOOM. The zoom level 0..2 (5, 8, 12 tracks), the tracks and fields it shows, and whether a column is one of them.
  int zoom() const noexcept { return zoom_; }
  int visible_tracks() const noexcept;
  int visible_fields() const noexcept;
  bool column_visible(Column column) const noexcept { return static_cast<int>(column) < visible_fields(); }
  bool cycle_zoom(const Dims& d = Dims{}) noexcept;  // Note + Instr: 5 -> 8 -> 12 -> 5; the window is clamped to the tracks and holds the cursor, the column snaps left to a visible one
  bool set_zoom(int level, const Dims& d = Dims{}) noexcept;  // the same for a level (clamped to 0..2); true if anything changed
  bool snap_column() noexcept;           // a column the level hides (a hold popup chose it) goes to the nearest visible one on its left
  bool jump_row(int row, const Dims& d = Dims{}) noexcept;       // to a step, clamped to the pattern (the cursor.jump_* actions)
  bool move_track(int delta, const Dims& d = Dims{}) noexcept;   // `delta` tracks, clamped, the same column (cursor.track)
  bool home(const Dims& d = Dims{}) noexcept;                    // the first track's Note column; the step stays (cursor.home)
  bool end(const Dims& d = Dims{}) noexcept;                     // the last track's last visible column; the step stays (cursor.end)
  // Puts the cursor and the window back inside a pattern that changed under them (a shorter pattern, fewer tracks: a length edit,
  // another pattern, an undo). True if anything moved.
  bool clamp_to(const Dims& d) noexcept;

  // OS-MUTEVIS (new Community OS behaviour): a muted track says MUTE, a soloed one SOLO, inside its own column, in the top slot of the grid, an inverted tag (a bright chip with the letters knocked out; the shape of the header's M / S chip) over the
  // dimmed rows. It is fixed to the slot, not to a row, so it stays while the pattern scrolls under it; the header keeps the track name and the small chip. A track whose quantised mute is pending blinks it.
  // It must never hide the cursor's row or the play bar: while either is in the top slot the tag takes the bottom one (or, if both are taken, the next free of 1, then the second from the bottom).
  static constexpr int tag_slot(int cursor_slot, int play_slot) noexcept {
    const int candidates[4] = {0, kVisibleRows - 1, 1, kVisibleRows - 2};
    for (const int c : candidates) {
      if (c != cursor_slot && c != play_slot) {
        return c;
      }
    }
    return 0;
  }

  int row() const noexcept { return row_; }
  int track() const noexcept { return track_; }
  Column column() const noexcept { return column_; }
  int top_row() const noexcept { return top_; }  // the row in the window's first slot: negative while the cursor is within kCentreRow rows of the start (OS-PATCENTER)
  int left_track() const noexcept { return left_; }

  // Appends the whole screen to the frame in progress. `covered` is the overlay's rectangle (OS-002b): a row, a field or
  // a component that lies entirely inside it is not drawn, the overlay paints over it (an empty rectangle skips nothing).
  // `pane_w` is the width the grid is drawn in: the full screen by default, kRightPanelX while the right-hand panel is up
  // (OS-PTOOLS2 S3p): the grid narrows to the left of the panel and shows the tracks that fit (pattern_tracks_fit), with the
  // window scrolled to keep the cursor in view.
  void draw(ui::Renderer& r, const PatternSource& src, const Status& status,
            ui::Rect covered = ui::Rect{0, 0, 0, 0}, int pane_w = ui::kScreenW) const noexcept;
  // The demo alone: the OS-001b entry the tests and the simulator's sweeps use.
  void draw(ui::Renderer& r, const PatternData& data, const Status& status,
            ui::Rect covered = ui::Rect{0, 0, 0, 0}, int pane_w = ui::kScreenW) const noexcept {
    draw(r, PatternSource(data), status, covered, pane_w);
  }

 private:
  void follow(const Dims& d) noexcept;  // moves the window to where the cursor puts it (window_top) and the track window so the cursor's track is inside it

  void draw_headers(ui::Renderer& r, const PatternLayout& l, const PatternSource& src, const Status& status, int shown, int left) const noexcept;
  void draw_strip(ui::Renderer& r, const PatternSource& src, const Status& status) const noexcept;
  void draw_info(ui::Renderer& r, const Status& status) const noexcept;

  int row_ = 0;
  int track_ = 0;
  Column column_ = Column::Note;
  int top_ = window_top(0);  // (the cursor starts on row 0: OS-PATCENTER puts it on the centre slot)
  int left_ = 0;
  int zoom_ = 0;  // OS-ZOOM: RAM-only (no file holds it), 0 at boot
};

}  // namespace o3os::app
