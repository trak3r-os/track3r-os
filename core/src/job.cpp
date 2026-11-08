// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/core/job.h>

namespace o3os::core {

int JobRunner::start(Job& job) noexcept {
  for (unsigned i = 0; i < kSlots; ++i) {
    if (slots_[i].state == SlotState::Free) {
      slots_[i].job = &job;
      slots_[i].state = SlotState::Running;
      slots_[i].stats = JobStats{};
      return static_cast<int>(i);
    }
  }
  return -1;
}

void JobRunner::cancel(unsigned slot) noexcept {
  if (slot < kSlots && slots_[slot].state == SlotState::Running && slots_[slot].job->cancellable()) {
    slots_[slot].state = SlotState::Cancelled;  // before the call: the job may look at its own slot
    slots_[slot].job->cancel();
  }
}

void JobRunner::cancel_all() noexcept {
  for (unsigned i = 0; i < kSlots; ++i) {
    cancel(i);
  }
}

void JobRunner::release(unsigned slot) noexcept {
  if (slot < kSlots && slots_[slot].state != SlotState::Running) {
    slots_[slot].job = nullptr;
    slots_[slot].state = SlotState::Free;
  }
}

unsigned JobRunner::running() const noexcept {
  unsigned n = 0;
  for (const Slot& s : slots_) {
    n += s.state == SlotState::Running ? 1u : 0u;
  }
  return n;
}

unsigned JobRunner::occupied() const noexcept {
  unsigned n = 0;
  for (const Slot& s : slots_) {
    n += s.state != SlotState::Free ? 1u : 0u;
  }
  return n;
}

unsigned JobRunner::collect(unsigned (&order)[kSlots]) noexcept {
  unsigned n = 0;
  for (unsigned k = 0; k < kSlots; ++k) {
    const unsigned slot = (rotor_ + k) % kSlots;
    if (slots_[slot].state == SlotState::Running) {
      order[n++] = slot;
    }
  }
  return n;
}

void JobRunner::finish(unsigned slot, JobStatus status, std::uint32_t slice_us, std::uint32_t used_us) noexcept {
  Slot& s = slots_[slot];
  ++s.stats.steps;
  s.stats.total_us += used_us;
  s.stats.max_step_us = used_us > s.stats.max_step_us ? used_us : s.stats.max_step_us;
  s.stats.overruns += (!s.job->long_step() && overran(used_us, slice_us)) ? 1u : 0u;  // OS-CEPTFX A17: an indivisible step is not an overrun
  // A job may have cancelled itself during its step: that stays, whatever it then answered.
  if (s.state == SlotState::Running) {
    if (status == JobStatus::Done) {
      s.state = SlotState::Done;
    } else if (status == JobStatus::Failed) {
      s.state = SlotState::Failed;
    }
  }
  rotor_ = (slot + 1u) % kSlots;
}

}  // namespace o3os::core
