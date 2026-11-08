// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/og_pcm.h>

#include <o3os/app/save_service.h>
#include <o3os/app/wave_math.h>
#include <o3os/audio/paula.h>
#include <o3os/audio/voices.h>

#include <cstring>

namespace o3os::app {

static_assert(kOgPtiStemMax == SaveService::kAppNameMax, "the stem limit is the card guard's name limit");

namespace {

constexpr float kTwoPow32 = 4294967296.0f;
constexpr std::uint8_t kLoopBackward = 2;
constexpr std::uint8_t kLoopPingPong = 3;

// x rounded to the nearest integer, halves away from zero.
std::int32_t round_to_int(float x) noexcept { return static_cast<std::int32_t>(x + (x < 0.0f ? -0.5f : 0.5f)); }

// The sample at source frame `i`, where the frame may lie outside the sample or outside the span the output frame may read directly: the continuation the export wants (header of og_pcm.h).
// `wrap_start`: the output frame being made is inside the loop (its centre is at or past the loop's start and before its end): its taps before the loop's start are what the voice would have
// played BEFORE the wrap, the loop's own tail, not the pre-loop data; an output frame before the loop's start keeps the real pre-loop data (the first pass). `wrap_end`: the centre is before the
// loop's end: its taps past the end are the loop's start onward. An output frame centred at or past the loop's end is in the real tail between the loop's end and the window's end, which a loop
// never plays and the file keeps as it is: it reads the real data (edge-extended at the ends of the sample), never the wrapped loop.
float source_at(const std::int16_t* src, std::int64_t count, const OgLoop& loop, bool wrap_start, bool wrap_end, std::int64_t i) noexcept {
  if (loop.active) {
    const std::int64_t ls = static_cast<std::int64_t>(loop.start);
    const std::int64_t le = static_cast<std::int64_t>(loop.end);
    const std::int64_t len = le - ls;  // at least 2
    if (i >= le && wrap_end) {
      const std::int64_t k = i - le;
      if (loop.mode == kLoopPingPong) {
        const std::int64_t r = k % (2 * len);
        i = r < len ? le - 1 - r : ls + (r - len);
      } else {
        i = ls + k % len;
      }
    } else if (i < ls && wrap_start) {
      const std::int64_t d = ls - i;  // 1 for the frame just before the loop
      if (loop.mode == kLoopPingPong) {
        const std::int64_t r = (d - 1) % (2 * len);
        i = r < len ? ls + r : le - 1 - (r - len);
      } else {
        i = le - 1 - (d - 1) % len;
      }
    }
  }
  i = i < 0 ? 0 : (i >= count ? count - 1 : i);
  return static_cast<float>(src[i]);
}

// floor(point x (frames / 65535.0f)) in single precision, exactly the arithmetic of the voices' binding (og_samples.cpp at_point): the loop's frames must be the ones the voice loops on, so that a
// two-frame loop is a loop here when it is one there.
std::uint32_t voice_point_frame(std::uint32_t point, std::uint32_t frames) noexcept {
  if (point >= 65535u) {
    return frames;
  }
  return static_cast<std::uint32_t>(static_cast<float>(point) * (static_cast<float>(frames) / 65535.0f));
}

OgPcmPlan finish_plan(std::uint64_t frames_out, std::uint64_t step_q32, bool identity) noexcept {
  OgPcmPlan p;
  p.frames_out = frames_out > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(frames_out);
  p.step_q32 = step_q32;
  p.identity = identity;
  p.status = frames_out > kOgMaxFrames ? OgPcmStatus::TooLong : OgPcmStatus::Ok;
  return p;
}

}  // namespace

// ---- 1. the name ---------------------------------------------------------------------------------------------------------------------------------

std::size_t og_pti_stem(unsigned slot, const char* name, char* out, std::size_t cap) noexcept {
  if (out == nullptr || cap < kOgPtiStemMax + 1u || slot >= formats::og::kSampleSlots) {
    return 0;
  }
  char stem[kOgPtiStemMax - 3u + 1u];  // 28 bytes and the NUL
  SaveService::instrument_file_stem(name, slot, stem, sizeof stem);
  const unsigned number = slot + 1u;
  out[0] = static_cast<char>('0' + number / 10u);
  out[1] = static_cast<char>('0' + number % 10u);
  out[2] = ' ';
  std::size_t n = 3;
  for (std::size_t i = 0; stem[i] != '\0' && n < kOgPtiStemMax; ++i) {
    out[n++] = stem[i];
  }
  while (n > 3u && (out[n - 1u] == ' ' || out[n - 1u] == '.')) {
    --n;
  }
  out[n] = '\0';
  return SaveService::project_name_ok(out) ? n : 0u;
}

// ---- 2. the plan --------------------------------------------------------------------------------------------------------------------------------

OgPcmPlan og_pcm_plan(std::uint32_t frames_in, std::uint32_t rate_hz) noexcept {
  if (frames_in == 0u) {
    return OgPcmPlan{};
  }
  if (rate_hz == 0u || rate_hz == kOgRateHz) {
    return finish_plan(frames_in, std::uint64_t{1} << 32, true);
  }
  const std::uint64_t rate = rate_hz < audio::kRateMinHz ? audio::kRateMinHz : (rate_hz > audio::kRateMaxHz ? audio::kRateMaxHz : rate_hz);
  const std::uint64_t out = (static_cast<std::uint64_t>(frames_in) * kOgRateHz + rate - 1u) / rate;
  return finish_plan(out, (rate << 32) / kOgRateHz, false);
}

OgPcmPlan og_pcm_plan_amiga(std::uint32_t frames_in) noexcept {
  if (frames_in == 0u) {
    return OgPcmPlan{};
  }
  // The data rate is PAL clock / 428 = 3,546,895 / 428 Hz; the source frames per output frame are that over 44,100.
  const std::uint64_t num = audio::paula::kPalClockHz;
  const std::uint64_t den = static_cast<std::uint64_t>(kAmigaPeriod8) * kOgRateHz;
  const std::uint64_t out = (static_cast<std::uint64_t>(frames_in) * den + num - 1u) / num;
  return finish_plan(out, (num << 32) / den, false);
}

std::int8_t og_amiga_fine(int fine) noexcept {
  int steps = (2 * fine + (fine < 0 ? -12 : 12)) / 25;  // as the Amiga voice (amiga_voices.cpp): a MED step is 12.5 hundredths of a semitone
  steps = steps < -8 ? -8 : (steps > 7 ? 7 : steps);
  const int hundredths = steps == 0 ? 0 : (steps * 25 + (steps < 0 ? -1 : 1)) / 2;
  return static_cast<std::int8_t>(hundredths);
}

std::uint32_t og_point_frame(std::uint32_t point, std::uint32_t frames) noexcept { return point_to_frame(point, frames); }

OgLoop og_loop_of(const model::Instrument& in, std::uint32_t frames) noexcept {
  OgLoop loop;
  const model::SamplerParams& p = in.sampler;
  if (in.kind != model::InstrumentKind::Sample || frames == 0u || p.play_mode < 1u || p.play_mode > 3u) {
    return loop;
  }
  const std::uint32_t start = voice_point_frame(p.start, frames);
  const std::uint32_t end = voice_point_frame(p.end, frames);
  const std::uint32_t l0 = voice_point_frame(p.loop_start, frames);
  const std::uint32_t l1 = voice_point_frame(p.loop_end, frames);
  if (start >= end || l0 < start || l0 >= l1 || l1 > end || l1 - l0 < 2u) {
    return loop;
  }
  loop.active = true;
  loop.mode = p.play_mode;
  loop.start = l0;
  loop.end = l1;
  return loop;
}

// ---- 3. the resampler ---------------------------------------------------------------------------------------------------------------------------

std::uint32_t og_resample(const AmigaKernel& kernel, const std::int16_t* src, std::uint32_t frames, std::uint64_t step_q32, std::uint32_t total, const OgLoop& loop, std::int16_t* dst,
                          std::uint32_t from, std::uint32_t n, std::uint32_t* clipped) noexcept {
  if (src == nullptr || dst == nullptr || frames == 0u || step_q32 == 0u || from >= total || !kernel.ready()) {
    return 0;
  }
  n = n > total - from ? total - from : n;
  const float* const tab = kernel.table();
  // source frames per output frame, the Q32.32 step as its two 32-bit halves (a uint64 -> float conversion would call __aeabi_ul2f)
  const float rho = static_cast<float>(static_cast<std::uint32_t>(step_q32 >> 32)) + static_cast<float>(static_cast<std::uint32_t>(step_q32)) * (1.0f / kTwoPow32);
  const float scale = static_cast<float>(AmigaKernel::kRes) / (rho > 1.0f ? rho : 1.0f);  // table steps per source frame
  const float limit = static_cast<float>(AmigaKernel::kLimit);
  const std::int64_t count = static_cast<std::int64_t>(frames);
  const std::int64_t ls = static_cast<std::int64_t>(loop.start);
  const std::int64_t le = static_cast<std::int64_t>(loop.end);
  for (std::uint32_t k = 0; k < n; ++k) {
    const std::uint64_t pos = static_cast<std::uint64_t>(from + k) * step_q32;  // Q32.32: no drift
    const std::int64_t base = static_cast<std::int64_t>(pos >> 32);
    const float frac = static_cast<float>(static_cast<std::uint32_t>(pos)) * (1.0f / kTwoPow32);
    const bool wrap_end = loop.active && base < le;                  // the centre is before the loop's end
    const bool wrap_start = wrap_end && base >= ls;                  // ... and at or past its start: in the loop
    // The taps in [lo_ok, hi_ok) are the real frames, read straight (most of them); the others take the continuation (source_at).
    const std::int64_t lo_ok = wrap_start ? ls : 0;
    const std::int64_t hi_ok = wrap_end ? (le < count ? le : count) : count;
    float acc = 0.0f;
    float wsum = 0.0f;
    float u = frac * scale;  // the left taps (the source frames at and before `base`), then the right ones
    for (std::int64_t i = base; u < limit; --i, u += scale) {
      const unsigned j = static_cast<unsigned>(u);
      const float f = u - static_cast<float>(j);
      const float w = tab[j] + f * (tab[j + 1] - tab[j]);
      wsum += w;
      acc += w * (i >= lo_ok && i < hi_ok ? static_cast<float>(src[i]) : source_at(src, count, loop, wrap_start, wrap_end, i));
    }
    u = (1.0f - frac) * scale;
    for (std::int64_t i = base + 1; u < limit; ++i, u += scale) {
      const unsigned j = static_cast<unsigned>(u);
      const float f = u - static_cast<float>(j);
      const float w = tab[j] + f * (tab[j + 1] - tab[j]);
      wsum += w;
      acc += w * (i >= lo_ok && i < hi_ok ? static_cast<float>(src[i]) : source_at(src, count, loop, wrap_start, wrap_end, i));
    }
    const float y = wsum > 1e-6f ? acc / wsum : 0.0f;
    std::int32_t v = round_to_int(y);
    if (v > 32767 || v < -32768) {
      v = v > 0 ? 32767 : -32768;
      if (clipped != nullptr) {
        ++*clipped;
      }
    }
    dst[k] = static_cast<std::int16_t>(v);
  }
  return n;
}

std::uint32_t og_frames_per_step(std::uint64_t step_q32) noexcept {
  // taps a frame: 2 x kHalf output frames each side, stretched by the ratio when it is above 1 (rho = step / 2^32)
  const std::uint64_t ratio_q8 = step_q32 >> 24;  // rho x 256
  const std::uint64_t taps_q8 = static_cast<std::uint64_t>(2u * AmigaKernel::kHalf) * (ratio_q8 > 256u ? ratio_q8 : 256u);
  const std::uint64_t n = (static_cast<std::uint64_t>(kOgTapsPerStep) << 8) / taps_q8;
  return n < 8u ? 8u : (n > kOgMaxFramesPerStep ? kOgMaxFramesPerStep : static_cast<std::uint32_t>(n));
}

void OgResampleSource::set(AmigaKernel& kernel, const std::int16_t* src, std::uint32_t frames, std::uint64_t step_q32, std::uint32_t total, const OgLoop& loop) noexcept {
  kernel_ = &kernel;
  src_ = src;
  frames_ = frames;
  step_q32_ = step_q32;
  total_ = total;
  loop_ = loop;
  pos_ = 0;
  left_ = 0;
  clipped_ = 0;
}

core::IoResult OgResampleSource::read(void* dst, std::size_t max) noexcept {
  if (kernel_ == nullptr || src_ == nullptr || dst == nullptr || step_q32_ == 0u) {
    return {0, core::IoStatus::Failed};
  }
  if (pos_ >= total_) {
    return {0, core::IoStatus::End};
  }
  if (!kernel_->ready()) {
    (void)kernel_->build(kKernelPerStep);
    return {0, core::IoStatus::WouldBlock};
  }
  std::int16_t chunk[kOgMaxFramesPerStep];
  std::uint32_t n = static_cast<std::uint32_t>(max / 2u);
  n = n > total_ - pos_ ? total_ - pos_ : n;
  n = n > left_ ? left_ : n;
  n = n > kOgMaxFramesPerStep ? kOgMaxFramesPerStep : n;
  if (n == 0u) {
    return {0, max < 2u ? core::IoStatus::Failed : core::IoStatus::WouldBlock};
  }
  const std::uint32_t made = og_resample(*kernel_, src_, frames_, step_q32_, total_, loop_, chunk, pos_, n, &clipped_);
  if (made != n) {
    return {0, core::IoStatus::Failed};
  }
  std::memcpy(dst, chunk, 2u * static_cast<std::size_t>(n));  // (little-endian: the file's byte order, asserted in pti_load.cpp)
  pos_ += n;
  left_ -= n;
  return {2u * static_cast<std::size_t>(n), core::IoStatus::Ok};
}

}  // namespace o3os::app
