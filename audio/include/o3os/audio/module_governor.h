// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-GOV: the CPU governor of the module instruments (new Community OS behaviour, maintainer design 2026-10-13). A .tmod instrument costs whatever its code costs (the
// TB-303 is 42.7k cycles a block, 16 % of the 261,120, for ONE sounding voice) and the user may bind as many as the instances allow, so the render measures what each one really costs and refuses
// a note that would START a voice the block cannot afford, rather than let the output underrun. A lost note beats an underrun. Nothing here trusts a value a module declares.
//
//   1. MEASURED COST. ModuleVoices::render() stamps the cycle counter around every render() call of an instance (the module's call and the Q15 conversion). At the end of the block the cycles of the
//      instance are brought to a whole block (cycles x 64 / frames rendered; a block with fewer than kMinFrames frames of the instance says too little and is skipped), divided by its active voices
//      (the host's instance is MONO: one voice, so 1). The figure that is kept needs TWO blocks in a row: min(this block, the previous one), so that one block that ran long (a cold code cache, an
//      interrupt) is not taken for the module's cost; the first block after a bind only waits for its successor. The estimate is a MOVING MAX WITH DECAY: max(that figure, the estimate aged by the
//      blocks since it was last measured), the age being a halving every kCostHalfBlocks blocks (370 ms), IDLE BLOCKS INCLUDED: an instance that does not sound is not rendered, and an estimate
//      that waited for the next render to fall would refuse its notes for ever (admit() uses the aged figure too). 0 is "never measured" (a COLD instance). The counter is the wall clock: a USB or
//      DMA interrupt that lands inside the render is in the figure, which is the conservative direction.
//
//   2. NOTE ADMISSION (a note that would START a module voice: ModuleVoices::note_on, so the sequencer's notes and the pads, the MIDI keys and the previews, and `diag mod note` too).
//        - A note for an instrument that ALREADY HAS a voice (is rendering: ModuleVoices `active`; a module whose amplitude has died away reports itself idle while its gate may still be held, and has none)
//          is a mono retrigger: the instance takes the new note over, no voice is added and no load either, so it is admitted
//          silently: no verdict, no count, no toast. (The instance's own voice is already in the block's load: judging the retrigger as a second voice refused the 303's line at the unit's 88 %
//          stress and shed nothing, because a dropped note leaves the old voice sounding.)
//        - A NEW voice is admitted when  base + (the cost of the voices that sound) + (the voices added earlier in this same block) + cost  <=  threshold.  `base` is what the render takes besides
//          the module voices: a moving max (kWorstDecayShift) of the engine's cycles of each block less the cycles the instances took in it, so a voice's own past cost is never counted twice;
//          `cost` is the instance's aged estimate, or kColdVoicePercent of the block while it has never been measured (assumed heavier than the TB-303: a module is not trusted to be cheap).
//          Otherwise the note is DROPPED: it starts nothing, and the note that was ringing on its track, if any, is released (a track whose note was replaced does not go on sounding the old one).
//        - THE THRESHOLD is kThresholdPercent of the block (one constant, tunable at run time through Governor::set_threshold_percent for the unit's tuning: `diag mod gov thr`, 50..95), 90 % so
//          that the UI (the main loop's slice), the USB interrupts and the pipeline's commit still fit.
//      ONE VOICE A SLOT: an instance is one mono voice (module_voices.h), so a note for a slot that sounds is a retrigger wherever it plays (on another track it takes the instance over: the first
//      track's block goes silent, as for any mono instrument). A module that is polyphonic inside its own render is one voice to the host and its whole cost is the per-voice figure.
//
//   3. SAFETY NET. An instance whose cost, in the block, exceeds its time budget (kBudgetPercent of the block, tunable: `diag mod gov budget`) for kMuteBlocks blocks in a row is MUTED: its note ends,
//      the block it was heard in is faded out (no click), it is not rendered and its notes are refused (silence for that instance only; the others play on) and it is flagged (Governor events,
//      InstanceGov::muted, the Params page). THE MUTE CLEARS (a) on the first note that arrives after kMuteCooldownBlocks blocks (about 1.5 s) since the mute: the instance gets another chance (its
//      estimate has aged meanwhile, so it is not refused for the cost it had in the storm) and is muted again after kMuteBlocks if it is still heavy, so a permanently heavy instance costs a few
//      milliseconds of overload per cooldown, not a lasting one; (b) on a reload: bind() starts the instance cold and unmuted.
//
// THREADS. Everything marked "render" runs in the render context (the audio interrupt, one thread): admission is note_on's, so it is the render's, called from Engine::dispatch / play_live between
// the block's events; the accounting is end_block's and block_done's. The main loop only reads the published words (relaxed atomics, each with ONE writer: the render) and writes the two tunables
// (relaxed atomics, one writer: the main loop). No lock, no allocation, no printf; every loop is over kMaxModuleInstances.
//
// PLACEMENT (SRAM_L is full): the per-block and per-note functions below are O3OS_AUDIO_COLD: they live in flash and the hot code reaches them through a long call, once per note and once per block.
// The hot code itself only stamps and adds (module_voices.cpp render()).

#include <o3os/audio/hot.h>
#include <o3os/hal/audio.h>

#include <atomic>
#include <cstdint>

namespace o3os::audio::gov {

// The cycle counter (audio::CycleFn has the same type).
using Clock = std::uint32_t (*)() noexcept;

inline constexpr std::uint32_t kBlockCycles = hal::audio::kCyclesPerBlock;

// A percentage of the block, in cycles, exactly in 32 bits (with c = 100 q + r: c * p / 100 = q * p + r * p / 100 (floor), and q * p fits for any p <= 100: the form Pipeline::init uses).
constexpr std::uint32_t cycles_of_percent(std::uint32_t percent) noexcept {
  return (kBlockCycles / 100u) * percent + ((kBlockCycles % 100u) * percent) / 100u;
}
// Cycles as tenths of a percent of the block (0..1000 for a block that fits), for the readouts.
constexpr std::uint32_t tenths_of_block(std::uint32_t cycles) noexcept { return kBlockCycles == 0u ? 0u : (cycles >= kBlockCycles ? 1000u : cycles * 1000u / kBlockCycles); }

// ---- the knobs ------------------------------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kThresholdPercent = 90;     // admission: a new voice that would take the predicted block above this is dropped
inline constexpr std::uint32_t kColdVoicePercent = 20;     // the assumed cost of a voice of an instance that was never measured (the TB-303 is 16.4 %)
inline constexpr std::uint32_t kColdInsertPercent = 10;   // OS-INSFX: the assumed cost of an effect insert that was never measured (an unmeasured effect is not trusted to be cheap)
inline constexpr unsigned kMaxInserts = 4;                 // OS-INSFX D3: effect instances active at once (a master insert will count 2); the cap is retuned from the unit's figures (S8)
inline constexpr std::uint32_t kBudgetPercent = 50;        // the time one instance may take in a block before it counts as over (the TB-303 before OS-PERF3 was 50 %, now 16 %)
inline constexpr unsigned kMuteBlocks = 4;                 // N: consecutive blocks over the budget that mute an instance (5.8 ms)
inline constexpr std::uint32_t kMuteCooldownBlocks = 1024; // blocks (1.5 s) after a mute before a note may clear it
inline constexpr unsigned kWorstDecayShift = 8;            // the recent worst render and the base fall by 1/256 a block (about 370 ms for 63 %)
inline constexpr unsigned kCostHalfBlocksShift = 8;        // an estimate halves in 2^8 = 256 blocks (370 ms)
inline constexpr std::uint32_t kMinFrames = 32;            // frames of an instance in a block for the block to count (a note that starts in the last frames says nothing)
// The ranges the tunables accept (the console refuses outside them): a threshold under 50 % starves the notes, over 95 % leaves nothing for the interrupts; the budget is the forced-heavy knob of the
// unit's tests (10 % makes the 303 a heavy instance), it can only mute.
inline constexpr std::uint32_t kMinThresholdPercent = 50;
inline constexpr std::uint32_t kMaxThresholdPercent = 95;
inline constexpr std::uint32_t kMinBudgetPercent = 10;
inline constexpr std::uint32_t kMaxBudgetPercent = 100;

inline constexpr std::uint32_t kThresholdCycles = cycles_of_percent(kThresholdPercent);
inline constexpr std::uint32_t kColdCycles = cycles_of_percent(kColdVoicePercent);
inline constexpr std::uint32_t kColdInsertCycles = cycles_of_percent(kColdInsertPercent);
inline constexpr std::uint32_t kBudgetCycles = cycles_of_percent(kBudgetPercent);
static_assert(kThresholdPercent >= kMinThresholdPercent && kThresholdPercent <= kMaxThresholdPercent && kBudgetPercent >= kMinBudgetPercent && kBudgetPercent <= kMaxBudgetPercent, "the defaults are in range");
static_assert(kColdVoicePercent < kThresholdPercent, "the knobs are fractions of the block, in order");
static_assert(kMuteBlocks >= 2 && kMuteBlocks < 255, "N blocks in a row: at least two, and it counts in a byte");

// ---- the pure rules (host-tested in tests/module_governor_test.cpp) -----------------------------------------------------------------------
enum class Verdict : std::uint8_t { Admit, Drop, Muted };

// A note for an instance; `load`: the base plus the voices that sound plus the ones added in this block; `cost`: the instance's per-voice cost (or the cold assumption); `has_voice`: the instance is
// rendering (a retrigger adds nothing).
constexpr Verdict decide(std::uint32_t load, std::uint32_t cost, bool has_voice, std::uint32_t threshold) noexcept {
  // (load + cost <= threshold, written so that no sum can wrap)
  return has_voice || (cost <= threshold && load <= threshold - cost) ? Verdict::Admit : Verdict::Drop;
}

// A moving max with slow decay: the sample if it is above the decayed estimate, else the decayed estimate.
constexpr std::uint32_t track_max(std::uint32_t estimate, std::uint32_t sample, unsigned shift) noexcept {
  const std::uint32_t decayed = estimate - (estimate >> shift);
  return sample > decayed ? sample : decayed;
}

// An estimate after `elapsed` blocks without a measure: halved every 2^kCostHalfBlocksShift blocks, linearly in between.
constexpr std::uint32_t decayed_cost(std::uint32_t estimate, std::uint32_t elapsed) noexcept {
  const std::uint32_t halvings = elapsed >> kCostHalfBlocksShift;
  if (halvings >= 32u) {
    return 0u;
  }
  const std::uint32_t whole = estimate >> halvings;
  return whole - (whole >> 9) * (elapsed & ((1u << kCostHalfBlocksShift) - 1u));  // (whole / 512) x up to 255: at most half of it
}

// What an instance is predicted to cost for admission, `elapsed` blocks after its estimate was made: aged, but never below min(estimate, kColdCycles). A figure ABOVE the cold assumption (a stale
// outlier) is forgiven down to it, where the instance stood before it was ever measured; a figure under it (the TB-303's 16 %, a cheap module's 3 %) is the module's own and is kept: idleness does
// not make a 303 cheap. 0 stays 0 (never measured).
constexpr std::uint32_t aged_cost(std::uint32_t estimate, std::uint32_t elapsed) noexcept {
  const std::uint32_t floor = estimate < kColdCycles ? estimate : kColdCycles;
  const std::uint32_t decayed = decayed_cost(estimate, elapsed);
  return decayed > floor ? decayed : floor;
}

// The cost of ONE voice of an instance for a whole block, from `cycles` it took to render `frames` frames of the block with `voices` voices sounding. 0: too few frames to say, or no voice.
constexpr std::uint32_t voice_block_cost(std::uint32_t cycles, std::uint32_t frames, std::uint32_t voices) noexcept {
  constexpr std::uint32_t kBlk = hal::audio::kBlock;
  if (frames < kMinFrames || voices == 0u) {
    return 0u;
  }
  const std::uint32_t capped = cycles > 0xFFFFFFFFu / kBlk ? 0xFFFFFFFFu / kBlk : cycles;  // a module that never came back in time: a huge figure, not a wrapped one
  return capped * kBlk / (frames * voices);
}

enum class Event : std::uint8_t { None, Drop, Mute };

// What the render keeps of one instance (a member of ModuleVoices::Instance).
struct InstanceGov {
  // render context only
  std::uint32_t acc_cycles = 0;  // this block: cycles in render() and the conversion, summed over the instance's segments
  std::uint32_t acc_frames = 0;  // and the frames they covered
  std::uint32_t muted_at = 0;    // the block counter when it was muted
  std::uint32_t cost_at = 0;     // ... when the estimate was last made
  std::uint32_t prev_sample = 0; // the previous block's figure (0: none since the bind)
  std::uint32_t added_at = 0xFFFFFFFFu;  // ... when admission added this instance's voice (the voices added in a block are counted once: in `pending`)
  std::uint8_t over = 0;         // consecutive blocks over the budget
  // render writes (one word, relaxed), any context reads
  std::atomic<std::uint32_t> cost{0};  // cycles of one voice for a whole block, 0: never measured
  std::atomic<std::uint8_t> muted{0};

  // Main loop, from bind() before the instance is published.
  void reset() noexcept {
    acc_cycles = 0;
    acc_frames = 0;
    muted_at = 0;
    cost_at = 0;
    prev_sample = 0;
    added_at = 0xFFFFFFFFu;
    over = 0;
    cost.store(0, std::memory_order_relaxed);
    muted.store(0, std::memory_order_relaxed);
  }
};

// What `diag mod gov`, the strip's toast and the tests read.
struct Snapshot {
  std::uint32_t worst;      // the recent worst render (the engine's cycles of a block), cycles
  std::uint32_t base;       // the recent worst of what the render takes besides the module voices, cycles
  std::uint32_t threshold;  // cycles
  std::uint32_t budget;     // cycles
  std::uint32_t admitted;   // notes that started a new voice (a retrigger of a sounding instrument is not counted)
  std::uint32_t drops;      // notes refused for lack of room (cold or measured)
  std::uint32_t mutes;      // instances muted by the safety net
  std::uint32_t refused;    // notes refused because their instance was muted
  Event last;               // the latest drop or mute, and the instrument slot (0..63) it was about
  std::uint8_t last_slot;
};

class Governor {
 public:
  // ---- main loop ------------------------------------------------------------------------------------------------------------------
  // The tunables, as percent of the block (clamped to their ranges): `diag mod gov thr|budget`. Relaxed single words; the render reads them at the next note / block.
  void set_threshold_percent(std::uint32_t percent) noexcept {
    threshold_.store(cycles_of_percent(clamp(percent, kMinThresholdPercent, kMaxThresholdPercent)), std::memory_order_relaxed);
  }
  void set_budget_percent(std::uint32_t percent) noexcept { budget_.store(cycles_of_percent(clamp(percent, kMinBudgetPercent, kMaxBudgetPercent)), std::memory_order_relaxed); }
  std::uint32_t threshold() const noexcept { return threshold_.load(std::memory_order_relaxed); }
  std::uint32_t budget() const noexcept { return budget_.load(std::memory_order_relaxed); }
  // OS-INSFX: the recent worst of what the render takes besides the module voices (it holds the effect inserts' cycles: InsertHost does not call account()), any context.
  std::uint32_t base() const noexcept { return base_pub_.load(std::memory_order_relaxed); }
  // Render context only (OS-INSFX: InsertHost's aggregate net, in the boundary after observe_block): the last block's cycles as a whole, the module voices' and the inserts' share included (the net's
  // "others" is this less the inserts' own cycles: every render work but the inserts).
  std::uint32_t last_total() const noexcept { return last_total_; }
  Snapshot snapshot() const noexcept;

  // ---- render context ------------------------------------------------------------------------------------------------------------
  // A block starts: the voices added so far are in the render's figures from here on.
  O3OS_AUDIO_INLINE void begin_block() noexcept { pending_ = 0; }

  // What an instance that sounds adds to the prediction of a note for ANOTHER instance: its estimate (the cold assumption if it was never measured), unless it was added in this block (then it is in
  // `pending`).
  static std::uint32_t sounding_cost(const InstanceGov& g, std::uint32_t now) noexcept {
    const std::uint32_t c = g.cost.load(std::memory_order_relaxed);
    return g.added_at == now ? 0u : (c != 0u ? c : kColdCycles);
  }

  // A note wants to start on an instance (`has_voice`: it is rendering, ModuleVoices `active`; `sounding`: the sum of sounding_cost over the OTHER instances that sound; `now`: the block counter). Counts, records the
  // event, and returns the verdict; the caller starts the note unless it is Drop or Muted. Per note, in flash.
  O3OS_AUDIO_COLD Verdict admit(InstanceGov& g, unsigned slot, bool has_voice, std::uint32_t sounding, std::uint32_t now) noexcept;

  enum class Account : std::uint8_t { Skipped, Measured, Muted };
  // The instance rendered in this block: folds its cycles into its estimate and counts the blocks over the budget. `Muted`: it just went over for kMuteBlocks in a row, the caller silences it. Per block
  // per sounding instance, in flash.
  O3OS_AUDIO_COLD Account account(InstanceGov& g, unsigned slot, std::uint32_t now) noexcept;

  // The engine's cycles for the whole block, after everything (and after account() of every instance): the recent worst render, and the base. Once per block, in flash.
  O3OS_AUDIO_COLD void observe_block(std::uint32_t total) noexcept;

 private:
  static constexpr std::uint32_t clamp(std::uint32_t p, std::uint32_t lo, std::uint32_t hi) noexcept { return p < lo ? lo : (p > hi ? hi : p); }
  void record(Event e, unsigned slot) noexcept;
  static void bump(std::atomic<std::uint32_t>& a) noexcept { a.store(a.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed); }

  // render only
  std::uint32_t worst_ = 0;
  std::uint32_t base_ = 0;
  std::uint32_t last_total_ = 0;
  std::uint32_t pending_ = 0;
  std::uint32_t module_cycles_ = 0;  // this block: what the instances took (summed by account(), used and cleared by observe_block())
  // main loop writes, render reads
  std::atomic<std::uint32_t> threshold_{kThresholdCycles};
  std::atomic<std::uint32_t> budget_{kBudgetCycles};
  // render writes, any context reads
  std::atomic<std::uint32_t> worst_pub_{0};
  std::atomic<std::uint32_t> base_pub_{0};
  std::atomic<std::uint32_t> admitted_{0};
  std::atomic<std::uint32_t> drops_{0};
  std::atomic<std::uint32_t> mutes_{0};
  std::atomic<std::uint32_t> refused_{0};
  std::atomic<std::uint16_t> last_{0};  // Event << 8 | slot
};

}  // namespace o3os::audio::gov
