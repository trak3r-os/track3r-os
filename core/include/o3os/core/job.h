// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Cooperative jobs (ADR 0009 rev 1, OS-002c): work that takes longer than one pass of the main loop (an import, an
// export, an analysis) is a Job. The main loop gives the runner a time budget per pass; the runner hands each running
// job a slice of what is left, and a job does about that much work and returns, so the screen never stalls behind it.
// Nothing here blocks, allocates or reads a clock of its own: the time comes in as a Clock (anything with a
// `core::Tick now() const`, which hal::Clock is), so a run in the simulator is a pure function of its input.
//
//   Job       the contract: step(budget_us) -> Running | Done | Failed, progress() 0..1000, cancel(), name().
//   JobRunner four static slots; start() takes a job, run() steps the running ones within a budget, cancel() and
//             release() end and free a slot. A finished slot stays occupied until released, so the owner can read the
//             result without racing the next start().
//
// Budget behaviour of run(budget_us):
//   * the jobs that are Running are stepped in turn, starting after the one that ran last in the previous pass (a job that
//     uses its whole slice every time cannot starve the others);
//   * each gets remaining / jobs_left microseconds, so a job that finishes early (or has nothing to do: its source would
//     block) leaves its share to the ones after it;
//   * a job is never started with less than kMinSliceUs left, so with a budget of 0 nothing runs;
//   * a job is called at most once per pass, whatever time is left, and only if it is still Running when its turn comes: a
//     step may cancel or release another job (a sibling that is no longer wanted), and that one is simply skipped; A step is as long as the job makes it: the budget is a
//     request, not a pre-emption. The runner measures every step and counts the ones that took clearly longer than
//     their slice (JobStats::overruns), which the diagnostics and the tests read; the next jobs in that pass simply find
//     less left.
//
// Main loop only; not thread-safe. Jobs are owned by the caller and must outlive their slot.

#include <o3os/core/tick.h>

#include <cstddef>
#include <cstdint>

namespace o3os::core {

enum class JobStatus : std::uint8_t { Running, Done, Failed };

inline constexpr unsigned kJobProgressMax = 1000;  // progress() is in thousandths

class Job {
 public:
  // Does about `budget_us` microseconds of work (a job that cannot measure time maps the budget to an amount of work it
  // knows the cost of) and returns. After Done or Failed it is not stepped again. Must not block.
  virtual JobStatus step(std::uint32_t budget_us) noexcept = 0;
  // 0..kJobProgressMax; kJobProgressMax once Done.
  virtual unsigned progress() const noexcept = 0;
  // Abandon the work: no step() follows. Release whatever the job holds; the result is not valid.
  virtual void cancel() noexcept = 0;
  // False for a job that cannot be abandoned without harm (an import that has half changed the project): JobRunner::cancel()
  // leaves it running.
  virtual bool cancellable() const noexcept { return true; }
  // OS-CEPTFX A17: true for a job whose step is one indivisible call (a plugin's process_block) that may take longer than its slice: the runner's overrun
  // counter skips its steps (the step still counts in steps, total_us and max_step_us). Existing jobs keep false and are counted as before.
  virtual bool long_step() const noexcept { return false; }
  // A short label for the notification ("IMPORT KICK.WAV"). Static or owned by the job.
  virtual const char* name() const noexcept = 0;
  // After Failed: why, in a few upper-case words ("TRUNCATED"). "" when it has nothing to say.
  virtual const char* detail() const noexcept { return ""; }

 protected:
  Job() = default;
  ~Job() = default;  // not deleted through the interface: no heap, so no virtual destructor slot
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;
};

// done / total in thousandths, 0..kJobProgressMax, without a 64-bit division (both are scaled down together until `total` fits 22
// bits, so `done * 1000` fits 32). 0 when total is 0.
constexpr unsigned permille_of(std::uint64_t done, std::uint64_t total) noexcept {
  unsigned shift = 0;
  while ((total >> shift) > 4194303u) {
    ++shift;
  }
  const std::uint32_t t = static_cast<std::uint32_t>(total >> shift);
  std::uint32_t d = static_cast<std::uint32_t>(done >> shift);
  d = d > t ? t : d;
  return t == 0 ? 0u : static_cast<unsigned>(d * 1000u / t);
}

enum class SlotState : std::uint8_t { Free, Running, Done, Failed, Cancelled };

struct JobStats {
  std::uint32_t steps = 0;        // step() calls
  std::uint32_t total_us = 0;     // microseconds spent in them (wraps; for the diagnostics)
  std::uint32_t max_step_us = 0;  // the longest single step
  std::uint32_t overruns = 0;     // steps that took clearly longer than their slice (see overran())
};

// What one run() did.
struct PassResult {
  std::uint32_t used_us = 0;  // from the first step's start to the last one's end
  std::uint8_t stepped = 0;   // jobs stepped
  std::uint8_t skipped = 0;   // running jobs left for the next pass because the budget was gone
};

class JobRunner {
 public:
  static constexpr unsigned kSlots = 4;
  static constexpr std::uint32_t kMinSliceUs = 50;  // below this a step is not worth starting

  // A step overran when it took more than its slice plus a quarter of it plus 20 us (clock granularity and the cost of
  // the call itself are not the job's fault).
  static constexpr bool overran(std::uint32_t used_us, std::uint32_t slice_us) noexcept {
    return used_us > slice_us + slice_us / 4u + 20u;
  }

  // The slot the job runs in, or -1 when all four are occupied (a finished job occupies its slot until release()).
  int start(Job& job) noexcept;
  // A Running slot becomes Cancelled and its job's cancel() is called; any other state, and a job that is not cancellable(),
  // is left alone.
  void cancel(unsigned slot) noexcept;
  void cancel_all() noexcept;
  // Done, Failed or Cancelled -> Free. A Running slot is not released (cancel it first).
  void release(unsigned slot) noexcept;

  SlotState state(unsigned slot) const noexcept { return slot < kSlots ? slots_[slot].state : SlotState::Free; }
  Job* job(unsigned slot) noexcept { return slot < kSlots ? slots_[slot].job : nullptr; }
  const Job* job(unsigned slot) const noexcept { return slot < kSlots ? slots_[slot].job : nullptr; }
  const JobStats& stats(unsigned slot) const noexcept { return slots_[slot < kSlots ? slot : 0u].stats; }
  unsigned running() const noexcept;
  // Slots in use, finished ones included.
  unsigned occupied() const noexcept;

  // One pass of the main loop: steps the running jobs within `budget_us`, see above. `clock` is anything with a
  // `core::Tick now() const noexcept` (hal::Clock).
  template <typename Clock>
  PassResult run(const Clock& clock, std::uint32_t budget_us) noexcept {
    PassResult result;
    unsigned order[kSlots];
    unsigned left = collect(order);
    const Tick begin = clock.now();
    unsigned i = 0;
    for (; i < left; ++i) {
      if (slots_[order[i]].state != SlotState::Running) {
        continue;  // cancelled (or released) by the step of a job before it in this pass
      }
      const std::uint32_t spent = tick_elapsed(clock.now(), begin);
      if (spent >= budget_us || budget_us - spent < kMinSliceUs) {
        break;
      }
      const std::uint32_t slice = (budget_us - spent) / running_from(order, i, left);
      const Tick before = clock.now();
      const JobStatus status = slots_[order[i]].job->step(slice);
      finish(order[i], status, slice, tick_elapsed(clock.now(), before));
      ++result.stepped;
    }
    result.skipped = static_cast<std::uint8_t>(running_from(order, i, left));  // the budget ran out before their turn
    result.used_us = tick_elapsed(clock.now(), begin);
    return result;
  }

 private:
  struct Slot {
    Job* job = nullptr;
    SlotState state = SlotState::Free;
    JobStats stats;
  };

  // Fills `order` with the Running slots, starting after the one stepped last; returns how many.
  unsigned collect(unsigned (&order)[kSlots]) noexcept;
  void finish(unsigned slot, JobStatus status, std::uint32_t slice_us, std::uint32_t used_us) noexcept;
  // How many of order[from .. count) are still Running (a step may have cancelled some).
  unsigned running_from(const unsigned (&order)[kSlots], unsigned from, unsigned count) const noexcept {
    unsigned n = 0;
    for (unsigned k = from; k < count; ++k) {
      n += slots_[order[k]].state == SlotState::Running ? 1u : 0u;
    }
    return n;
  }

  Slot slots_[kSlots];
  unsigned rotor_ = 0;  // the slot to start the next pass with
};

}  // namespace o3os::core
