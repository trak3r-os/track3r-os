// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/wave_math.h>

namespace o3os::app {

namespace {

constexpr std::uint32_t kMaxSnapRadius = 4096;

constexpr int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

int sample_abs(const std::int16_t* pcm, std::uint32_t i) noexcept { return pcm[i] < 0 ? -static_cast<int>(pcm[i]) : static_cast<int>(pcm[i]); }

}  // namespace

std::uint32_t point_to_frame(std::uint32_t point, std::uint32_t frames) noexcept {
  if (point >= kPointMax) {
    return frames;  // the last point is the end of the sample exactly (og_samples.cpp at_point)
  }
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(point) * frames / kPointMax);
}

std::uint32_t frame_to_point(std::uint32_t frame, std::uint32_t frames) noexcept {
  if (frames == 0u) {
    return 0u;
  }
  if (frame >= frames) {
    return kPointMax;
  }
  return static_cast<std::uint32_t>((static_cast<std::uint64_t>(frame) * kPointMax + frames / 2u) / frames);
}

// ---- Points -----------------------------------------------------------------------------------------------------------------------------------

std::uint16_t get_point(const Points& p, PointId id) noexcept {
  switch (id) {
    case PointId::Start: return p.start;
    case PointId::LoopStart: return p.loop_start;
    case PointId::LoopEnd: return p.loop_end;
    case PointId::End: break;
  }
  return p.end;
}

bool points_valid(const Points& p) noexcept {
  return p.start < p.end && p.start <= p.loop_start && p.loop_start < p.loop_end && p.loop_end <= p.end;
}

std::uint16_t set_point(Points& p, PointId id, int value) noexcept {
  if (!points_valid(p)) {  // a table that arrived broken (a file): made valid first, then edited
    p.start = p.start >= kPointMax ? static_cast<std::uint16_t>(kPointMax - 1) : p.start;
    p.end = p.end <= p.start ? static_cast<std::uint16_t>(kPointMax) : p.end;
    p.loop_start = p.loop_start < p.start ? p.start : (p.loop_start >= p.end ? static_cast<std::uint16_t>(p.end - 1) : p.loop_start);
    p.loop_end = p.loop_end > p.end ? p.end : (p.loop_end <= p.loop_start ? static_cast<std::uint16_t>(p.loop_start + 1) : p.loop_end);
  }
  switch (id) {
    case PointId::Start: {
      const int v = clampi(value, 0, static_cast<int>(p.end) - 1);
      p.start = static_cast<std::uint16_t>(v);
      if (p.loop_start < p.start) {  // the loop is pushed ahead of the start
        p.loop_start = p.start;
        if (p.loop_end <= p.loop_start) {
          p.loop_end = static_cast<std::uint16_t>(p.loop_start + 1);  // (start <= end - 1, so this is at most end)
        }
      }
      return p.start;
    }
    case PointId::LoopStart: {
      p.loop_start = static_cast<std::uint16_t>(clampi(value, p.start, static_cast<int>(p.loop_end) - 1));
      return p.loop_start;
    }
    case PointId::LoopEnd: {
      p.loop_end = static_cast<std::uint16_t>(clampi(value, static_cast<int>(p.loop_start) + 1, p.end));
      return p.loop_end;
    }
    case PointId::End: {
      const int v = clampi(value, static_cast<int>(p.start) + 1, static_cast<int>(kPointMax));
      p.end = static_cast<std::uint16_t>(v);
      if (p.loop_end > p.end) {  // the loop is pulled in behind the end
        p.loop_end = p.end;
        if (p.loop_start >= p.loop_end) {
          p.loop_start = static_cast<std::uint16_t>(p.loop_end - 1);
          if (p.loop_start < p.start) {
            p.loop_start = p.start;  // (end >= start + 1, so loop_end = end >= start + 1: loop_start = start < loop_end)
          }
        }
      }
      return p.end;
    }
  }
  return 0;
}

PointsRec crop_points(const PointsRec& in, std::uint32_t frames, std::uint32_t a, std::uint32_t b) noexcept {
  PointsRec out;
  if (frames == 0u || b <= a || b > frames) {
    return in;
  }
  const std::uint32_t m = b - a;
  const auto remap = [&](unsigned point) {
    const std::uint32_t f = point_to_frame(point, frames);
    return frame_to_point(f <= a ? 0u : (f - a > m ? m : f - a), m);
  };
  out.points.start = static_cast<std::uint16_t>(remap(in.points.start));
  out.points.loop_start = static_cast<std::uint16_t>(remap(in.points.loop_start));
  out.points.loop_end = static_cast<std::uint16_t>(remap(in.points.loop_end));
  out.points.end = static_cast<std::uint16_t>(remap(in.points.end));
  (void)set_point(out.points, PointId::Start, out.points.start);  // an order that the remap broke (points on one frame) is made valid
  const unsigned n = in.slice_count > kMaxSliceCount ? kMaxSliceCount : in.slice_count;
  for (unsigned i = 0; i < n; ++i) {
    const std::uint32_t f = point_to_frame(in.slices[i], frames);
    if (f >= a && f < b) {
      (void)slice_add(out.slices, out.slice_count, frame_to_point(f - a, m));
    }
  }
  return out;
}

PointsRec insert_points(const PointsRec& in, std::uint32_t frames, std::uint32_t a, std::uint32_t b, std::uint32_t prepend, std::uint32_t append) noexcept {
  if (frames == 0u || b <= a || b > frames || (prepend == 0u && append == 0u) || prepend > 0x3FFFFFFFu - frames || append > 0x3FFFFFFFu - frames - prepend) {
    return in;
  }
  const std::uint32_t n = frames + prepend + append;
  PointsRec out;
  const std::uint16_t p[kPointCount] = {in.points.start, in.points.loop_start, in.points.loop_end, in.points.end};
  std::uint16_t q[kPointCount];
  const unsigned k = in.slice_count > kMaxSliceCount ? kMaxSliceCount : in.slice_count;
  for (unsigned i = 0; i < kPointCount + k; ++i) {  // one loop and one remap site for the points, then the slices
    const std::uint32_t f = point_to_frame(i < kPointCount ? p[i] : in.slices[i - kPointCount], frames);
    const unsigned to = frame_to_point(f <= a ? f : (f < b ? f + prepend : f + prepend + append), n);
    if (i < kPointCount) {
      q[i] = static_cast<std::uint16_t>(to);
    } else {
      (void)slice_add(out.slices, out.slice_count, to);
    }
  }
  out.points = Points{q[0], q[1], q[2], q[3]};
  (void)set_point(out.points, PointId::Start, out.points.start);  // an order that the rounding broke is made valid
  return out;
}

// ---- Slices -----------------------------------------------------------------------------------------------------------------------------------

unsigned slice_add(std::uint16_t* at, std::uint8_t& count, unsigned point, unsigned cap) noexcept {
  cap = cap > kMaxSliceCount ? kMaxSliceCount : cap;
  if (point > kPointMax || count >= cap) {
    return kNoSlice;
  }
  unsigned i = 0;
  while (i < count && at[i] < point) {
    ++i;
  }
  if (i < count && at[i] == point) {
    return kNoSlice;  // already a start
  }
  for (unsigned k = count; k > i; --k) {
    at[k] = at[k - 1];
  }
  at[i] = static_cast<std::uint16_t>(point);
  ++count;
  return i;
}

bool slice_remove(std::uint16_t* at, std::uint8_t& count, unsigned index) noexcept {
  if (index >= count) {
    return false;
  }
  for (unsigned k = index; k + 1u < count; ++k) {
    at[k] = at[k + 1];
  }
  --count;
  at[count] = 0;  // the table past the count is zero
  return true;
}

unsigned slice_even(std::uint16_t* at, std::uint8_t& count, unsigned n) noexcept {
  n = n < 1u ? 1u : (n > kMaxSliceCount ? kMaxSliceCount : n);
  for (unsigned i = 0; i < kMaxSliceCount; ++i) {
    at[i] = i < n ? static_cast<std::uint16_t>(i * kPointMax / n) : std::uint16_t{0};
  }
  count = static_cast<std::uint8_t>(n);
  return n;
}

unsigned slice_move(std::uint16_t* at, std::uint8_t count, unsigned index, int point) noexcept {
  if (index >= count) {
    return kNoSlice;
  }
  const int lo = index == 0u ? 0 : static_cast<int>(at[index - 1]) + 1;
  const int hi = index + 1u >= count ? static_cast<int>(kPointMax) : static_cast<int>(at[index + 1]) - 1;
  const int v = lo > hi ? static_cast<int>(at[index]) : clampi(point, lo, hi);  // neighbours that touch leave it where it is
  at[index] = static_cast<std::uint16_t>(v);
  return static_cast<unsigned>(v);
}

unsigned slice_of(const std::uint16_t* at, unsigned count, unsigned point) noexcept {
  unsigned found = kNoSlice;
  for (unsigned i = 0; i < count; ++i) {
    if (at[i] <= point) {
      found = i;
    } else {
      break;
    }
  }
  return found;
}

// ---- Zero crossings ---------------------------------------------------------------------------------------------------------------------------

std::uint32_t snap_point(const std::int16_t* pcm, std::uint32_t frames, std::uint32_t point, std::uint32_t radius) noexcept {
  if (pcm == nullptr || frames < 2u) {
    return point;
  }
  radius = radius > kMaxSnapRadius ? kMaxSnapRadius : radius;
  const std::uint32_t lo = point > radius ? point - radius : 0u;
  const std::uint32_t hi = point + radius > kPointMax ? kPointMax : point + radius;
  std::uint32_t best = point;
  int best_score = 0x7FFFFFFF;
  std::uint32_t best_dist = 0xFFFFFFFFu;
  for (std::uint32_t u = lo; u <= hi; ++u) {
    std::uint32_t f = point_to_frame(u, frames);
    f = f >= frames ? frames - 1u : f;
    int score = sample_abs(pcm, f);
    if (f + 1u < frames && ((pcm[f] < 0) != (pcm[f + 1u] < 0) || pcm[f] == 0)) {
      score = 0;  // a crossing between this frame and the next: as good as it gets
    }
    const std::uint32_t dist = u > point ? u - point : point - u;
    if (score < best_score || (score == best_score && dist < best_dist)) {
      best = u;
      best_score = score;
      best_dist = dist;
    }
  }
  return best;
}

std::uint32_t snap_radius(std::uint32_t frames, std::uint32_t span, unsigned columns) noexcept {
  if (frames == 0u || span == 0u || columns == 0u) {
    return 1u;
  }
  // 4 columns of the view, in units: 4 * span / columns frames, a unit being frames / 65535 of them.
  const std::uint64_t frames_wide = static_cast<std::uint64_t>(4u) * span / columns;
  const std::uint64_t units = frames_wide * kPointMax / frames;
  return units < 1u ? 1u : (units > kMaxSnapRadius ? kMaxSnapRadius : static_cast<std::uint32_t>(units));
}

// ---- The level at the loop ---------------------------------------------------------------------------------------------------------------------

LoopLevels loop_levels(const std::int16_t* pcm, std::uint32_t frames, std::uint32_t loop_start_frame, std::uint32_t loop_end_frame) noexcept {
  LoopLevels l;
  if (pcm == nullptr || frames == 0u) {
    return l;
  }
  const std::uint32_t last = frames - 1u;
  l.valid = true;
  l.at_start = pcm[loop_start_frame > last ? last : loop_start_frame];
  l.at_end = pcm[loop_end_frame > last ? last : loop_end_frame];
  const std::int32_t d = static_cast<std::int32_t>(l.at_end) - static_cast<std::int32_t>(l.at_start);
  l.jump = static_cast<std::uint32_t>(d < 0 ? -d : d);
  return l;
}

unsigned level_text(char (&out)[7], std::int32_t value, bool with_sign) noexcept {
  const bool neg = value < 0;
  const std::uint32_t mag = static_cast<std::uint32_t>(neg ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value));
  const std::uint32_t hundredths = (mag * 100u + 16384u) / 32768u;  // 0..200 for |value| <= 65535: no overflow in 32 bits
  unsigned n = 0;
  if (with_sign) {
    out[n++] = neg && hundredths != 0u ? '-' : '+';
  }
  out[n++] = static_cast<char>('0' + hundredths / 100u);
  out[n++] = '.';
  out[n++] = static_cast<char>('0' + hundredths / 10u % 10u);
  out[n++] = static_cast<char>('0' + hundredths % 10u);
  out[n] = '\0';
  return n;
}

// ---- The window -------------------------------------------------------------------------------------------------------------------------------

Window window_fit(std::uint32_t frames) noexcept { return Window{0u, frames}; }

Window window_clamp(Window w, std::uint32_t frames) noexcept {
  if (frames == 0u) {
    return Window{0u, 0u};
  }
  if (w.span < 1u) {
    w.span = 1u;
  }
  if (w.span > frames) {
    w.span = frames;
  }
  if (w.first > frames - w.span) {
    w.first = frames - w.span;
  }
  return w;
}

Window window_zoom(Window w, std::uint32_t frames, int dir, std::uint32_t anchor) noexcept {
  if (frames == 0u) {
    return Window{0u, 0u};
  }
  w = window_clamp(w, frames);
  const std::uint32_t min_span = frames < kMinSpan ? frames : kMinSpan;
  std::uint64_t span = w.span;
  if (dir > 0) {
    span = span / 2u;
  } else if (dir < 0) {
    span = span * 2u;
  }
  if (span < min_span) {
    span = min_span;
  }
  if (span > frames) {
    span = frames;
  }
  // The anchor keeps its fraction of the lane: first' = anchor - (anchor - first) * span' / span.
  const std::uint32_t a = anchor < w.first ? w.first : (anchor > w.first + w.span ? w.first + w.span : anchor);
  const std::uint64_t offset = static_cast<std::uint64_t>(a - w.first) * span / w.span;
  const std::int64_t first = static_cast<std::int64_t>(a) - static_cast<std::int64_t>(offset);
  Window out{first < 0 ? 0u : static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(span)};
  return window_clamp(out, frames);
}

Window window_show(Window w, std::uint32_t frames, std::uint32_t frame) noexcept {
  w = window_clamp(w, frames);
  if (frames == 0u) {
    return w;
  }
  if (frame < w.first) {
    w.first = frame;
  } else if (frame >= w.first + w.span) {
    w.first = frame - w.span + 1u;
  }
  return window_clamp(w, frames);
}

// ---- The wavetable's window --------------------------------------------------------------------------------------------------------------------

bool wt_size_valid(unsigned size) noexcept { return size >= kWtMinSize && size <= kWtMaxSize && (size & (size - 1u)) == 0u; }

unsigned wt_size_eff(unsigned size) noexcept { return wt_size_valid(size) ? size : kWtMaxSize; }

unsigned wt_counter(unsigned size) noexcept {
  unsigned counter = 0;
  while (size > 1u) {
    size >>= 1;
    ++counter;
  }
  return counter;
}

unsigned wt_max_counter(std::uint32_t frames) noexcept {
  if (frames == 0u) {
    return kWtMaxCounter;  // no sample known: nothing to hold the size to
  }
  unsigned counter = kWtMaxCounter;
  while ((1u << counter) > frames && counter > kWtMinCounter) {
    --counter;
  }
  return counter;
}

unsigned wt_fit_size(unsigned size, std::uint32_t frames) noexcept {
  unsigned fit = wt_size_eff(size);
  if (frames == 0u) {
    return fit;
  }
  if (frames < kWtMinSize) {
    return 0;
  }
  while (fit > frames) {
    fit >>= 1;
  }
  return fit;
}

unsigned wt_engine_size(unsigned size, std::uint32_t frames) noexcept { return wt_size_valid(size) ? size : wt_fit_size(size, frames); }

unsigned wt_play_size(unsigned size, std::uint32_t frames) noexcept {
  const unsigned n = wt_engine_size(size, frames);
  return frames != 0u && frames < n ? 0u : n;  // (a stored valid size larger than the sample is not fitted: the engine plays nothing)
}

unsigned wt_windows(std::uint32_t frames, unsigned size) noexcept { return size == 0u ? 0u : static_cast<unsigned>(frames / size); }

unsigned wt_clamp_pos(std::uint32_t frames, unsigned size, unsigned pos) noexcept {
  if (frames == 0u) {
    return pos;  // no sample known: no windows to count
  }
  const unsigned windows = wt_windows(frames, size);
  return windows == 0u ? 0u : (pos >= windows ? windows - 1u : pos);
}

WtWindow wt_step_size(std::uint32_t frames, unsigned size, unsigned pos, int dir) noexcept {
  const unsigned engine = wt_engine_size(size, frames);
  const unsigned from = wt_counter(engine != 0u ? engine : wt_size_eff(size));
  const unsigned top = wt_max_counter(frames);
  const int want = static_cast<int>(from) + (dir < 0 ? -1 : (dir > 0 ? 1 : 0));
  const unsigned to = want < static_cast<int>(kWtMinCounter) ? kWtMinCounter : (want > static_cast<int>(top) ? top : static_cast<unsigned>(want));
  WtWindow out;
  out.size = 1u << to;
  unsigned p = pos > 0xFFFFu ? 0xFFFFu : pos;
  p = to < from ? p << (from - to) : p >> (to - from);
  out.pos = wt_clamp_pos(frames, out.size, p);
  return out;
}

FrameRange wt_window_frames(std::uint32_t frames, unsigned size, unsigned pos) noexcept {
  FrameRange r;
  const unsigned fit = wt_play_size(size, frames);
  if (frames == 0u || fit == 0u || wt_windows(frames, fit) == 0u) {
    return r;
  }
  r.begin = wt_clamp_pos(frames, fit, pos) * fit;
  r.end = r.begin + fit;
  r.valid = true;
  return r;
}

FrameRange grain_frames(std::uint32_t frames, unsigned position, unsigned length) noexcept {
  FrameRange r;
  if (frames == 0u) {
    return r;
  }
  const std::uint32_t centre = point_to_frame(position, frames);
  const std::uint32_t half = (length >= 44u ? length : 441u) / 2u;
  r.begin = centre > half ? centre - half : 0u;
  r.end = centre + half < frames ? centre + half : frames;
  r.valid = r.end > r.begin;
  return r;
}

}  // namespace o3os::app
