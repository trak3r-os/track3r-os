// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The arithmetic of the Wave / Chop screen (OS-011a): pure, no heap, no I/O, host-tested (tests/wave_math_test.cpp). Points are the OG's: a position in a
// sample is 0..65535 of its length, floor(point * frames / 65535) (og_samples.cpp at_point; the instrument's start, loop and end points and its slice table are
// stored this way, docs/formats/og.md), so a long sample has a coarser grid than its frames: a 10 s sample moves 6.7 frames a unit.

#include <cstdint>

namespace o3os::app {

inline constexpr unsigned kPointMax = 65535;
inline constexpr unsigned kMaxSliceCount = 48;  // the instrument file's slice limit = model::kSlices
inline constexpr unsigned kNoSlice = 0xFFFFFFFFu;

// The frame a point names, and the point nearest to a frame (round to nearest, at most kPointMax).
std::uint32_t point_to_frame(std::uint32_t point, std::uint32_t frames) noexcept;
std::uint32_t frame_to_point(std::uint32_t frame, std::uint32_t frames) noexcept;

// ---- The four points of an instrument ----------------------------------------------------------------------------------------------------------

struct Points {
  std::uint16_t start = 0;
  std::uint16_t loop_start = 0;
  std::uint16_t loop_end = kPointMax;
  std::uint16_t end = kPointMax;
};
enum class PointId : std::uint8_t { Start, LoopStart, LoopEnd, End };
inline constexpr unsigned kPointCount = 4;

std::uint16_t get_point(const Points& p, PointId id) noexcept;
// Sets one point to `value` (clamped); the others move only as far as they must, so that always start < end and start <= loop_start < loop_end <= end (the loop conditions of
// og_samples.cpp make_sample_desc: a loop that breaks them is a one shot). Moving START or END into the loop pushes the loop's edge ahead of
// it (the loop keeps its other edge: simple, and the invariant holds). Returns the
// value `id` ended up with.
std::uint16_t set_point(Points& p, PointId id, int value) noexcept;
// True when the four satisfy the invariant above.
bool points_valid(const Points& p) noexcept;

// The points and the slice table of an instrument as one record (what an undo of a crop has to put back; 120 bytes). The AMIGA conversion (OS-AMG1) also changes the instrument's recorded
// rate, engine and tune: its two records have `conv` set and carry those, and only a record with `conv` puts them back (every other edit leaves them as the user has them).
struct PointsRec {
  Points points;
  std::uint16_t slices[kMaxSliceCount] = {};
  std::uint8_t slice_count = 0;
  bool conv = false;
  std::uint8_t engine = 0;   // model::Instrument::engine
  std::int8_t tune = 0;      // model::SamplerParams::tune
  std::uint32_t rate = 0;    // model::Instrument::sample_rate
  // OS-CEPTFX S4c: a WAVETABLE plugin's run also sets the play mode: its two records carry the mode and the window the instrument has before and after (pm 0xFF: this step leaves them as they are).
  std::uint8_t pm = 0xFF;    // model::SamplerParams::play_mode
  std::uint16_t wsz = 0;     // wavetable_window_size
  std::uint32_t wpos = 0;    // wavetable_window
};
static_assert(sizeof(PointsRec) == 120, "PointsRec is 120 bytes (the history keeps two per step, in SDRAM): the comment above says so");
// The record of a sample cropped to frames [a, b) of `frames` (a < b <= frames): every point and slice keeps the frame it names, relative to the new start; points
// that fell outside are held to its ends, slices outside it are dropped (the first slice, if any survives, is at 0), the order and the loop conditions are made valid.
PointsRec crop_points(const PointsRec& in, std::uint32_t frames, std::uint32_t a, std::uint32_t b) noexcept;
// The record of a sample of `frames` that got `prepend` frames inserted at `a` and `append` at `b` (a < b <= frames; Add Silence, a plugin's set_extra_samples): a point or slice at or
// before `a` keeps its frame, one inside (a, b) moves by `prepend`, one at or after `b` by `prepend + append`; the rest as crop_points. Unchanged when nothing was inserted.
PointsRec insert_points(const PointsRec& in, std::uint32_t frames, std::uint32_t a, std::uint32_t b, std::uint32_t prepend, std::uint32_t append) noexcept;

// ---- The slice table (up to 48 ascending starts; slice k plays from at[k] to at[k + 1], the last to the end of the sample) -----------------------------------

// Inserts `point` in order. Returns the index it took, or kNoSlice when the table is full (`cap` is at most kMaxSliceCount) or the point is already a start.
// `count` is updated.
unsigned slice_add(std::uint16_t* at, std::uint8_t& count, unsigned point, unsigned cap = kMaxSliceCount) noexcept;
// Removes slice `index`; false when there is none.
bool slice_remove(std::uint16_t* at, std::uint8_t& count, unsigned index) noexcept;
// `n` slices of equal length (clamped to 1..kMaxSliceCount): at[i] = i * 65535 / n. Returns n.
unsigned slice_even(std::uint16_t* at, std::uint8_t& count, unsigned n) noexcept;
// Moves slice `index` to `point`, kept strictly between its neighbours (the first may go to 0, the last up to kPointMax). Returns the value it took, or
// kNoSlice when there is no such slice.
unsigned slice_move(std::uint16_t* at, std::uint8_t count, unsigned index, int point) noexcept;
// The slice a point falls in: the last start at or before it; kNoSlice before the first (or with no slices).
unsigned slice_of(const std::uint16_t* at, unsigned count, unsigned point) noexcept;

// ---- Zero crossings ---------------------------------------------------------------------------------------------------------------------------

// The point within `radius` units of `point` whose frame has the smallest |sample| (a zero crossing if there is one: a sample of 0, or one whose neighbour has the
// other sign), the nearest to `point` among equals. `point` itself for an empty sample. At most 2 * radius + 1 samples are read (radius is held to 4096).
std::uint32_t snap_point(const std::int16_t* pcm, std::uint32_t frames, std::uint32_t point, std::uint32_t radius) noexcept;
// The radius to use for a view that shows `span` frames in `columns` columns: about 4 columns, and never less than one unit.
std::uint32_t snap_radius(std::uint32_t frames, std::uint32_t span, unsigned columns) noexcept;

// ---- The level at the loop ---------------------------------------------------------------------------------------------------------------------

// What a loop's two edges read (OS-REVIEW, new Community OS behaviour): the sample at the frame of LOOP START and at the frame of LOOP END, and the jump between them
// (|end - start|, 0..65535 counts of 16 bits), so that the Wave screen can say whether the loop closes without a click. The sample bank keeps a slot as mono (a stereo
// file is mixed when it is loaded), so one channel is all there is to read. A frame at or past the end of the sample (LOOP END at 65535 is `frames` exactly) reads the last one.
struct LoopLevels {
  bool valid = false;  // false: no sample (null or empty); the rest is then 0
  std::int16_t at_start = 0;
  std::int16_t at_end = 0;
  std::uint32_t jump = 0;
};
LoopLevels loop_levels(const std::int16_t* pcm, std::uint32_t frames, std::uint32_t loop_start_frame, std::uint32_t loop_end_frame) noexcept;

// `value` as a fraction of the 16-bit full scale with two decimals, rounded to nearest: "+0.42", "-1.00" (`with_sign`: the sign always, "+0.00" for a zero) or "0.03"
// (a jump, up to "2.00"). Writes at most 6 characters and the NUL into `out` (7 bytes) and returns the length; no heap, no <cstdio>.
unsigned level_text(char (&out)[7], std::int32_t value, bool with_sign) noexcept;

// ---- The window --------------------------------------------------------------------------------------------------------------------------------

struct Window {
  std::uint32_t first = 0;
  std::uint32_t span = 0;  // frames shown
};
inline constexpr std::uint32_t kMinSpan = 32;  // the closest zoom: 32 frames across the lane

// The whole sample in view.
Window window_fit(std::uint32_t frames) noexcept;
// Half the span (dir > 0) or twice it (dir < 0), the frame `anchor` staying at the same fraction of the lane; held to [min(kMinSpan, frames), frames] and inside
// the sample.
Window window_zoom(Window w, std::uint32_t frames, int dir, std::uint32_t anchor) noexcept;
// The window moved so that `frame` is inside it (the least movement; a window that is the whole sample does not move).
Window window_show(Window w, std::uint32_t frames, std::uint32_t frame) noexcept;
// A window made valid for `frames`: span in [1, frames], first so that first + span <= frames. An empty sample gives {0, 0}.
Window window_clamp(Window w, std::uint32_t frames) noexcept;

// ---- The wavetable's window (OS-INSTUI S3 / S4) --------------------------------------------------------------------------------------------------
//
// A wavetable instrument plays one window of its sample as an oscillator: the window is `size` frames (a power of two, 32..2048: the exponents kWtMinCounter..kWtMaxCounter, 5..11) and the sample holds frames / size of them; the position is the window in use (0 .. windows - 1). A size of 0 is "not set": the engine plays 2048 (og_samples.cpp kWindowDefault).

inline constexpr unsigned kWtMinCounter = 5;
inline constexpr unsigned kWtMaxCounter = 11;
inline constexpr unsigned kWtMinSize = 1u << kWtMinCounter;
inline constexpr unsigned kWtMaxSize = 1u << kWtMaxCounter;

// A size the list has: a power of two from 32 to 2048.
bool wt_size_valid(unsigned size) noexcept;
// The size the instrument plays with (0 and the sizes that are none of the valid ones play 2048, as the engine does).
unsigned wt_size_eff(unsigned size) noexcept;
// log2 of a power of two (0 for 0 and 1).
unsigned wt_counter(unsigned size) noexcept;
// The largest counter a sample of `frames` takes (11, halved while 2^counter > frames, never below 5). A sample under kWtMinSize frames has no window: wt_fit_size gives 0.
unsigned wt_max_counter(std::uint32_t frames) noexcept;
// The size a sample of `frames` is given when it becomes a wavetable (when the play mode is changed): the size it has (2048 when unset) held to the length, rounded down to a power of two, 0 when the
// sample is shorter than the smallest window. With frames == 0 (no sample known) the size is returned as the engine would play it.
unsigned wt_fit_size(unsigned size, std::uint32_t frames) noexcept;
// OS-INSTUI S6 (the S5 review): the window size the ENGINE plays a stored `size` with (og_samples.cpp make_sample_desc, OS-007h): a stored valid size as it is, even one the sample cannot hold;
// an unset or invalid one is wt_fit_size. With frames == 0 (no sample known) it is wt_size_eff. wt_play_size is that size when the sample holds one whole window of it, else 0: the instrument plays NOTHING.
unsigned wt_engine_size(unsigned size, std::uint32_t frames) noexcept;
unsigned wt_play_size(unsigned size, std::uint32_t frames) noexcept;
// How many whole windows `size` makes of `frames` (at least 0), and a position held inside them (0 when there are none; unchanged for frames == 0, no sample known).
unsigned wt_windows(std::uint32_t frames, unsigned size) noexcept;
unsigned wt_clamp_pos(std::uint32_t frames, unsigned size, unsigned pos) noexcept;
struct WtWindow {
  unsigned size = 0;
  unsigned pos = 0;
};
// One step of the size, `dir` +1 up / -1 down in counters: the counter held to [5, wt_max_counter(frames)], the position moved with it (<< when the windows get smaller, >> when
// they get larger: it names the same part of the sample) and held to the windows there are.
WtWindow wt_step_size(std::uint32_t frames, unsigned size, unsigned pos, int dir) noexcept;

// ---- What a wavetable and a granular instrument play, as frames (the source page's picture and the Wave screen's marks) ----------------------------------------------------------

struct FrameRange {
  std::uint32_t begin = 0;
  std::uint32_t end = 0;  // exclusive
  bool valid = false;     // false: nothing to show (no sample, or not one whole window)
};
// The window a wavetable plays: `size` and `pos` are the stored fields (the size as the engine plays it, wt_engine_size; the position held to the windows: wt_clamp_pos). Not valid when the engine plays nothing (wt_play_size 0).
FrameRange wt_window_frames(std::uint32_t frames, unsigned size, unsigned pos) noexcept;
// The grain a granular instrument plays: centred on `position` (0..65535 of the sample, point_to_frame), `length` samples long (an unset length, under the smallest grain of 44, plays 441, og_samples.cpp), held inside the sample.
FrameRange grain_frames(std::uint32_t frames, unsigned position, unsigned length) noexcept;

}  // namespace o3os::app
