// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The note map of the Song screen's TIMELINE page (OS-SONG-VIEW2 V4, docs/specs/OS-SONG-VIEW2.md; new Community OS behaviour): one L1 picture per pattern in RAM_G (ui/plot/ram_g_map.h, kRamGNoteMap), 128
// columns (one a step, up to the pattern's longest track) by 24 rows (two a track, 12 tracks: row 2t holds track t's ticks, row 2t+1 is blank, which is the gap between tracks and costs no display-list word),
// a bit set where the step holds a note (a note byte 1..128: not a fade, cut or off, as the density and the energy count). The page draws it as a bitmap scaled to the block (draw_song_timeline).
//
// The picture of a pattern is rendered into a 384-byte stack buffer and uploaded (Display::upload, one call, hal/display.h) the first time the page needs it and whenever the pattern's generation
// (model::Project::pattern_generation, which every pattern writer moves) has moved since: a frame of the page that finds the picture current does a compare per slot and nothing else. The index is one
// generation per pattern id; it is dropped when the project or the display is another one (a display that was set up again starts the whole app over, so its caches are new). Main loop only, never the audio
// path or an ISR; nothing is allocated and none of it is saved. A pattern edited while the page is on screen is overwritten in RAM_G in place: the one frame that the chip scans out while the upload lands may
// show the old and new rows mixed (the page is drawn from the main loop, an edit comes from a key or the sequencer's live recording, both rare against the page's frames).

#include <o3os/hal/display.h>
#include <o3os/model/project.h>
#include <o3os/ui/plot/ram_g_map.h>

#include <cstdint>

namespace o3os::app {

class SongNoteMap {
 public:
  static constexpr unsigned kTracks = 12;  // the tracks Pattern can show (PatternView::kMaxVisibleTracks, asserted in the .cpp); a pattern's tracks past these are not in the picture
  static constexpr unsigned kSteps = ui::plot::kNoteMapCols;
  static constexpr unsigned kRows = ui::plot::kNoteMapRows;  // two a track
  static constexpr unsigned kStride = ui::plot::kNoteMapStride;
  static constexpr unsigned kBytes = ui::plot::kNoteMapBytes;
  static_assert(kRows == 2u * kTracks && kStride * kRows == kBytes, "two rows a track, 16 bytes a row");
  static_assert(kSteps == model::kMaxPatternLength, "a column a step of the longest pattern");
  static_assert(ui::plot::kNoteMapPictures == model::kMaxPatternId, "a picture a pattern id");

  // Pure: the picture of `pattern` in `out`: bit (7 - step % 8) of byte row * kStride + step / 8, MSB leftmost (L1), set for each step of a track's first track_length steps that holds a note, in row 2 * track.
  static void render(const model::Pattern& pattern, std::uint8_t (&out)[kBytes]) noexcept;
  // The bit of track `track`, step `step` of a rendered picture; false past the tracks or the steps.
  static bool tick(const std::uint8_t (&bits)[kBytes], unsigned track, unsigned step) noexcept {
    return track < kTracks && step < kSteps && ((bits[2u * track * kStride + step / 8u] >> (7u - step % 8u)) & 1u) != 0u;
  }
  // The RAM_G address of the picture of pattern `id` (1..255).
  static constexpr std::uint32_t addr(unsigned id) noexcept { return ui::plot::kRamGNoteMap + (id - 1u) * kBytes; }

  // True when RAM_G holds the current picture of pattern `id` of `project` on `display`: it is rendered and uploaded now if the pattern's generation moved (or it was never drawn). False for a pattern that does not exist, an id
  // out of range, or a display that refused the upload (Busy: nothing is recorded, the next call tries again).
  bool ensure(hal::Display& display, const model::Project& project, unsigned id) const noexcept;

  // The most pictures one frame uploads (ADR 0010: uploads of at most 4 KB per pass; 8 x 384 B = 3 KB): the first draw of a long song fills in over several frames, as the pad lights' do. After the
  // frame's first refusal (Busy) `ensure` uploads nothing more that frame. A picture that waits (cap, Busy) makes `ensure` return false, so the page draws no picture for it, never a stale one.
  static constexpr unsigned kMaxPerFrame = 8;
  static_assert(kMaxPerFrame * kBytes <= 4096u, "a frame's uploads stay within ADR 0010's 4 KB a pass");
  // Called once at the start of every frame (render_scene): the frame's upload count and the stop after a Busy start over, and `pending()` is cleared.
  void begin_frame() const noexcept {
    uploads_ = 0;
    stopped_ = false;
    pending_ = false;
  }
  // True when the frame that began last left a picture not uploaded (the cap, or Busy): the app draws another frame so that the rest fill in.
  bool pending() const noexcept { return pending_; }

  // Pictures rendered and uploaded since this object was made: what a test counts to prove the cache.
  std::uint32_t built() const noexcept { return built_; }

 private:
  mutable std::uint32_t generation_[model::kMaxPatternId + 1] = {};  // 0: not uploaded (an existing pattern's generation is never 0)
  mutable const model::Project* project_ = nullptr;
  mutable const hal::Display* display_ = nullptr;
  mutable std::uint32_t built_ = 0;
  mutable unsigned uploads_ = 0;  // this frame's uploads (begin_frame); main loop only
  mutable bool stopped_ = false;  // this frame's display refused an upload: no more this frame
  mutable bool pending_ = false;  // this frame left a picture for a later one
};

// ---- the scaling (BITMAP_TRANSFORM_A and E, 8.8 fixed point, as the boot logo's) ----------------------------------------------------------------------------------------------------------------
// The page draws a slot's picture over its block with the transform matrix mapping a pixel of the block to the picture's pixel (x' = A x, y' = E y; the chip samples at the pixel's centre, NEAREST).

// A for a block `width` pixels wide that holds `bars` bars: 16 steps a bar, so a step is the same width in every block of the page whatever the pattern's length: round(256 * 16 * bars / width), at least 1.
constexpr std::int32_t notemap_scale_x(unsigned bars, int width) noexcept {
  if (width <= 0) {
    return 256;
  }
  const std::uint32_t w = static_cast<std::uint32_t>(width);
  const std::uint32_t a = (4096u * bars + w / 2u) / w;
  return a < 1u ? 1 : static_cast<std::int32_t>(a);
}
// E for a track `lane` pixels tall (two rows a track, so a row is lane / 2 pixels): ceil(512 / lane), the smallest E whose rows are no taller than the lane allows (lane at least 2).
constexpr std::int32_t notemap_scale_y(int lane) noexcept { return lane < 2 ? 256 : static_cast<std::int32_t>((512u + static_cast<std::uint32_t>(lane) - 1u) / static_cast<std::uint32_t>(lane)); }
// The first pixel row (from the picture's top) that shows picture row `row` under E: the smallest y with floor((2y + 1) * E / 512) >= row. The row spans notemap_row_top(row) up to notemap_row_top(row + 1);
// row_top(0) is 0, and row_top(rows) is the height of the drawn picture. E is not a power of two in general, so a lane is not a whole number of pixels: the screen derives each lane's top from this, not from a pitch.
constexpr int notemap_row_top(unsigned row, std::int32_t e) noexcept {
  const std::uint32_t need = 512u * row;
  const std::uint32_t ee = static_cast<std::uint32_t>(e);
  return need <= ee ? 0 : static_cast<int>((need - ee + 2u * ee - 1u) / (2u * ee));
}
// The widest drawn width (BITMAP_SIZE) <= `width` whose last pixel still samples a column of the picture: floor((2w - 1) * A / 512) < kSteps. A rounds up, so the last pixel of a block of a 128-step pattern could
// otherwise land on column 128, past the picture. (Rows never pass: notemap_row_top(2 * tracks) ends the height at the last row of the tracks drawn, and the page draws at most kTracks.)
constexpr int notemap_fit_width(std::int32_t a, int width) noexcept {
  if (width <= 0 || a <= 0) {
    return 0;
  }
  const std::uint32_t most = ((SongNoteMap::kSteps * 512u - 1u) / static_cast<std::uint32_t>(a) + 1u) / 2u;
  return static_cast<std::uint32_t>(width) < most ? width : static_cast<int>(most);
}

}  // namespace o3os::app
