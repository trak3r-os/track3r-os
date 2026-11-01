// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/module_governor.h>

namespace o3os::audio::gov {

Snapshot Governor::snapshot() const noexcept {
  Snapshot s{};
  s.worst = worst_pub_.load(std::memory_order_relaxed);
  s.base = base_pub_.load(std::memory_order_relaxed);
  s.threshold = threshold_.load(std::memory_order_relaxed);
  s.budget = budget_.load(std::memory_order_relaxed);
  s.admitted = admitted_.load(std::memory_order_relaxed);
  s.drops = drops_.load(std::memory_order_relaxed);
  s.mutes = mutes_.load(std::memory_order_relaxed);
  s.refused = refused_.load(std::memory_order_relaxed);
  const std::uint16_t last = last_.load(std::memory_order_relaxed);
  s.last = static_cast<Event>(last >> 8);
  s.last_slot = static_cast<std::uint8_t>(last & 0xFFu);
  return s;
}

void Governor::record(Event e, unsigned slot) noexcept {
  last_.store(static_cast<std::uint16_t>((static_cast<unsigned>(e) << 8) | (slot & 0xFFu)), std::memory_order_relaxed);
}

Verdict Governor::admit(InstanceGov& g, unsigned slot, bool has_voice, std::uint32_t sounding, std::uint32_t now) noexcept {
  if (g.muted.load(std::memory_order_relaxed) != 0u) {
    if (static_cast<std::uint32_t>(now - g.muted_at) < kMuteCooldownBlocks) {
      bump(refused_);  // the mute already told the user: no event for each refused note
      return Verdict::Muted;
    }
    g.muted.store(0, std::memory_order_relaxed);  // the cool-down is over: this note is the retrigger that gives the instance another chance (and it is judged like any other below)
    g.over = 0;
  }
  if (has_voice) {
    return Verdict::Admit;  // a mono retrigger: the instance takes the note over, no voice is added, the voice is already in the block's load
  }
  const std::uint32_t measured = g.cost.load(std::memory_order_relaxed);
  const std::uint32_t cost = measured != 0u ? aged_cost(measured, now - g.cost_at) : kColdCycles;
  const Verdict v = decide(base_ + sounding + pending_, cost, false, threshold_.load(std::memory_order_relaxed));
  if (v == Verdict::Admit) {
    pending_ += cost;  // a voice is added: the next note of this block must see it (and the sum of the sounding ones must not count it twice)
    g.added_at = now;
    bump(admitted_);
  } else {
    bump(drops_);
    record(Event::Drop, slot);
  }
  return v;
}

Governor::Account Governor::account(InstanceGov& g, unsigned slot, std::uint32_t now) noexcept {
  const std::uint32_t cycles = g.acc_cycles;
  const std::uint32_t frames = g.acc_frames;
  g.acc_cycles = 0;
  g.acc_frames = 0;
  module_cycles_ += cycles;  // observe_block() takes the instances' share off the block's total
  const std::uint32_t sample = voice_block_cost(cycles, frames, 1u);  // the host's instance is mono: one voice
  if (sample == 0u) {
    return Account::Skipped;
  }
  // The estimate takes min(this block, the previous one): a single long block (a cold code cache, an interrupt) is not the module's cost. The first block after a bind waits for its successor.
  const std::uint32_t prev = g.prev_sample;
  g.prev_sample = sample;
  const std::uint32_t confirmed = prev == 0u ? 0u : (prev < sample ? prev : sample);
  if (confirmed != 0u) {
    const std::uint32_t decayed = decayed_cost(g.cost.load(std::memory_order_relaxed), now - g.cost_at);
    g.cost.store(confirmed > decayed ? confirmed : decayed, std::memory_order_relaxed);
    g.cost_at = now;
  }
  if (sample <= budget_.load(std::memory_order_relaxed)) {
    g.over = 0;
    return Account::Measured;
  }
  if (g.over < 255u) {
    ++g.over;
  }
  if (g.over < kMuteBlocks) {
    return Account::Measured;
  }
  g.over = 0;
  g.muted_at = now;
  g.muted.store(1, std::memory_order_relaxed);
  bump(mutes_);
  record(Event::Mute, slot);
  return Account::Muted;
}

void Governor::observe_block(std::uint32_t total) noexcept {
  worst_ = track_max(worst_, total, kWorstDecayShift);
  worst_pub_.store(worst_, std::memory_order_relaxed);
  const std::uint32_t others = total > module_cycles_ ? total - module_cycles_ : 0u;  // what the render took besides the module voices
  module_cycles_ = 0;
  last_total_ = total;
  base_ = track_max(base_, others, kWorstDecayShift);
  base_pub_.store(base_, std::memory_order_relaxed);
}

}  // namespace o3os::audio::gov
