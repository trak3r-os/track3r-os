// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/sample_edit_job.h>

namespace o3os::app {

bool SampleEditJob::begin(const EditSpec& spec, const std::int16_t* src, std::uint32_t frames, std::int16_t* dst, const char* label, const std::int16_t* src_side, std::int16_t* dst_side) noexcept {
  if (src == nullptr || dst == nullptr || frames == 0u || spec.b <= spec.a || spec.b > frames || (spec.op == EditOp::Amiga && spec.step_q32 == 0u)) {
    return false;
  }
  if ((src_side == nullptr) != (dst_side == nullptr) || (src_side != nullptr && (spec.op == EditOp::Amiga || spec.op == EditOp::Compressor))) {
    return false;  // both blocks or neither; the two effects that act on one signal are refused for S (OS-STEREO S7)
  }
  spec_ = spec;
  src_ = src;
  dst_ = dst;
  src_s_ = src_side;
  dst_s_ = dst_side;
  dc_m_ = dc_s_ = 0;
  carry_ = 0;
  side_pass_ = false;
  frames_ = frames;
  total_ = edit_result_frames(spec, frames);
  done_ = 0;
  warmed_ = 0;
  scanned_ = 0;
  peak_ = 0;
  dc_ = DcScan{};
  run_ = EditRun{};
  held_ = false;
  scanning_ = spec.op == EditOp::Normalise || spec.op == EditOp::DcRemove;
  cancelled_ = false;
  name_.assign("EDIT ");
  name_.append(label);
  return true;
}

core::JobStatus SampleEditJob::step(std::uint32_t budget_us) noexcept {
  if (cancelled_) {
    return core::JobStatus::Failed;
  }
  std::uint64_t budget = static_cast<std::uint64_t>(budget_us) * kFramesPerUs;
  budget = budget < 256u ? 256u : budget;  // a step always moves something
  if (scanning_) {
    const std::uint32_t range = spec_.b - spec_.a;
    budget = src_s_ != nullptr && spec_.op == EditOp::Normalise ? budget / 2u : budget;  // (the joint scan reads both blocks)
    const std::uint32_t n = range - scanned_ > budget ? static_cast<std::uint32_t>(budget) : range - scanned_;
    if (spec_.op == EditOp::DcRemove) {
      scan_dc(side_pass_ ? src_s_ : src_, frames_, spec_.a + scanned_, n, dc_);
    } else {
      const std::uint32_t p = src_s_ != nullptr ? scan_peak_ms(src_, src_s_, frames_, spec_.a + scanned_, n) : scan_peak(src_, frames_, spec_.a + scanned_, n);
      peak_ = p > peak_ ? p : peak_;
    }
    scanned_ += n;
    if (scanned_ < range) {
      return core::JobStatus::Running;
    }
    if (spec_.op == EditOp::DcRemove) {
      bool held = false;
      const std::int32_t d = dc_offset(dc_, range, &held);
      held_ = held_ || held;
      if (!side_pass_) {
        dc_m_ = spec_.dc = d;
        if (src_s_ != nullptr) {  // S is scanned next, with its own offset
          carry_ += scanned_;
          scanned_ = 0;
          dc_ = DcScan{};
          side_pass_ = true;
          return core::JobStatus::Running;
        }
      } else {
        dc_s_ = d;
        side_pass_ = false;
      }
    } else {
      spec_.gain_q16 = normalise_gain_q16(peak_);
    }
    scanning_ = false;
    return core::JobStatus::Running;  // the writing starts next step
  }
  if (spec_.op == EditOp::Amiga) {
    if (!kernel_.ready()) {
      (void)kernel_.build(kKernelPerStep);
      return core::JobStatus::Running;
    }
    budget = amiga_frames_per_budget(spec_.step_q32, budget_us);
    const std::uint32_t m = total_ - done_ > budget ? static_cast<std::uint32_t>(budget) : total_ - done_;
    amiga_resample(kernel_, src_, frames_, spec_.step_q32, spec_.q8, dst_ + done_, done_, m);
    done_ += m;
    return done_ >= total_ ? core::JobStatus::Done : core::JobStatus::Running;
  }
  budget /= edit_cost(spec_.op);  // Filter and Compressor cost several gains a frame (OS-AUDIT-15)
  budget = budget < 64u ? 64u : budget;
  if (warmed_ < spec_.warm_n) {  // OS-A9-SEAM: the state first runs over the loop's tail, the output discarded
    const std::uint32_t w = spec_.warm_n - warmed_ > budget ? static_cast<std::uint32_t>(budget) : spec_.warm_n - warmed_;
    edit_warm(spec_, side_pass_ ? src_s_ : src_, frames_, warmed_, w, &run_);
    warmed_ += w;
    return core::JobStatus::Running;
  }
  const std::uint32_t n = total_ - done_ > budget ? static_cast<std::uint32_t>(budget) : total_ - done_;
  edit_chunk(spec_, side_pass_ ? src_s_ : src_, frames_, (side_pass_ ? dst_s_ : dst_) + done_, done_, n, &run_);
  done_ += n;
  if (done_ < total_) {
    return core::JobStatus::Running;
  }
  if (src_s_ == nullptr || side_pass_) {
    return core::JobStatus::Done;
  }
  carry_ += warmed_ + done_;  // M is written: the same edit over S, from rest, with S's own DC offset
  warmed_ = 0;
  done_ = 0;
  run_ = EditRun{};
  spec_.dc = dc_s_;
  side_pass_ = true;
  return core::JobStatus::Running;
}

unsigned SampleEditJob::progress() const noexcept {
  const std::uint64_t range = spec_.b - spec_.a;
  const bool scans = spec_.op == EditOp::Normalise || spec_.op == EditOp::DcRemove;
  const std::uint64_t blocks = src_s_ != nullptr ? 2u : 1u;
  const std::uint64_t all = (scans ? range * (spec_.op == EditOp::DcRemove ? blocks : 1u) : 0u) + blocks * (spec_.warm_n + static_cast<std::uint64_t>(total_));
  return core::permille_of(carry_ + (scans ? scanned_ : 0u) + warmed_ + static_cast<std::uint64_t>(done_), all);
}

}  // namespace o3os::app
