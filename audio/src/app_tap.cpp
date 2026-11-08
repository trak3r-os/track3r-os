// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/app_tap.h>

#include <o3os/audio/meters.h>

#include <cmath>

namespace o3os::audio {

void AppTap::reset() noexcept {
  source_.store(0, std::memory_order_relaxed);
  samples_.store(1, std::memory_order_relaxed);
  ring_.reset();
  for (std::uint32_t i = 0; i < kBlocks; ++i) {
    peak_[i].store(0, std::memory_order_relaxed);
    sq_lo_[i].store(0, std::memory_order_relaxed);
    sq_hi_[i].store(0, std::memory_order_relaxed);
  }
  priv_ = Cost{};
  cost_reset_seen_ = 0;
  cost_reset_.store(0, std::memory_order_relaxed);
  cost_blocks_.store(0, std::memory_order_relaxed);
  cost_last_.store(0, std::memory_order_relaxed);
  cost_max_.store(0, std::memory_order_relaxed);
  cost_sum_.store(0, std::memory_order_relaxed);
}

// Render. The stats slot is written BEFORE the samples, so that the release store of `head` inside write() (or skip()) publishes both.
void AppTap::push(const std::int16_t* mono) noexcept {
  kernel::MeterAcc acc{0u, 0u};
  kernel::meter(mono, kBlock, acc);
  const std::uint32_t slot = (ring_.head() / kBlock) & (kBlocks - 1u);
  peak_[slot].store(acc.peak, std::memory_order_relaxed);
  sq_lo_[slot].store(static_cast<std::uint32_t>(acc.sumsq), std::memory_order_relaxed);
  sq_hi_[slot].store(static_cast<std::uint32_t>(acc.sumsq >> 32), std::memory_order_relaxed);
  if (samples_.load(std::memory_order_relaxed) != 0u) {
    ring_.write(mono, kBlock);
  } else {
    ring_.skip(kBlock);  // the statistics only (OS-BUG-TAPWRITE): no reader has asked for samples, so the 32 stores to SDRAM are not paid
  }
}

float AppTap::linear_rms(const Levels& l) noexcept {
  if (l.frames == 0u) {
    return 0.0f;
  }
  // the 64-bit sum as its two 32-bit halves (each half a single FPU conversion; a uint64 -> float one would call __aeabi_ul2f)
  const float sq = static_cast<float>(static_cast<std::uint32_t>(l.sumsq >> 32)) * 4294967296.0f + static_cast<float>(static_cast<std::uint32_t>(l.sumsq));
  return std::sqrt(sq / static_cast<float>(l.frames)) * (1.0f / 32768.0f);  // (a float sum of squares: 24 bits of mantissa are plenty for a level meter)
}

bool AppTap::levels_since(std::uint32_t& cursor, Levels& out) const noexcept {
  constexpr unsigned kTries = 3;
  bool whole = false;
  std::uint32_t h1 = cursor;
  for (unsigned attempt = 0; attempt < kTries && !whole; ++attempt) {
    h1 = ring_.head();
    std::uint32_t first = cursor;
    Levels l;
    if (static_cast<std::int32_t>(h1 - cursor) < 0) {  // a cursor ahead of the head (another run's): nothing to fold, nothing lost; it follows the head
      cursor = h1;
      out = l;
      return true;
    }
    const std::uint32_t pending = h1 - cursor;
    if (pending > kMaxFrames) {  // lapped: keep the newest kMaxFrames, count the rest
      l.lost = pending - kMaxFrames;
      first = h1 - kMaxFrames;
    }
    std::uint32_t p = first;
    for (std::uint32_t n = (h1 - first) / kBlock; n != 0u; --n, p += kBlock) {  // (at most kBlocks - 1 slots)
      const std::uint32_t slot = (p / kBlock) & (kBlocks - 1u);
      const std::uint32_t peak = peak_[slot].load(std::memory_order_relaxed);
      const std::uint64_t sq = static_cast<std::uint64_t>(sq_lo_[slot].load(std::memory_order_relaxed)) | (static_cast<std::uint64_t>(sq_hi_[slot].load(std::memory_order_relaxed)) << 32);
      l.peak = peak > l.peak ? peak : l.peak;
      l.sumsq += sq;
      l.clip_blocks += peak >= kMeterClipLevel ? 1u : 0u;
      l.frames += kBlock;
    }
#if !defined(__SANITIZE_THREAD__)
    std::atomic_thread_fence(std::memory_order_acquire);  // the slot reads are ordered before the re-check
#endif
    const std::uint32_t h2 = ring_.head();
    whole = (h2 - first) <= kMaxFrames;  // the writer has not come round to the oldest block we read (the block it is writing now is the slack)
    out = l;
  }
  cursor = h1;
  return whole;
}

}  // namespace o3os::audio
