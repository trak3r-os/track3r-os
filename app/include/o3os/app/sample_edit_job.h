// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// An offline edit as a core::Job (OS-011a): sample_dsp.h over one sample, a slice of the main loop's pass at a time, into a buffer of its own. Never in the audio
// path, and never over the source: the destination is another buffer (a spare of the bank's edit pool), so a voice that reads the source meanwhile reads what it always
// read. Normalise first scans the range for its peak, and DC removal for its mean (a second pass over it), then writes. The App starts it from the edit actions and, when it is Done, hands the
// result to the internal action sample.commit. The AMIGA conversion (amiga_convert.h) is the same job with a different length: it builds its kernel table first, once, a few hundred entries a step.

#include <o3os/app/amiga_convert.h>
#include <o3os/app/sample_dsp.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/job.h>

#include <cstdint>

namespace o3os::app {

class SampleEditJob final : public core::Job {
 public:
  // Frames processed per microsecond of budget: a load, a multiply, a store: about 20 cycles at 180 MHz, SDRAM on both sides. PROVISIONAL (the runner counts overruns).
  static constexpr std::uint32_t kFramesPerUs = 4;

  // False when there is nothing to do (no source, a range that is empty). `dst` has room for edit_result_frames(spec, frames). Everything must stay valid and
  // unchanged until the job has ended. `label` names it in the strip ("EDIT REVERSE").
  // OS-STEREO (d): with `src_side` and `dst_side` (S of the source, `frames` long, and room for S of the result, edit_result_frames long) the job edits both blocks: Crop, Reverse, Fades and Amplify
  // run the same edit over M and over S; Normalise takes its factor from the larger of |L| and |R| (scan_peak_ms) and applies it to both; DC remove and Filter run per block (their offset and their
  // state are each block's own: L = M + S and R = M - S are linear in the two). False for Compressor and Amiga (the caller refuses them: a stereo sample has to be made mono first).
  bool begin(const EditSpec& spec, const std::int16_t* src, std::uint32_t frames, std::int16_t* dst, const char* label, const std::int16_t* src_side = nullptr, std::int16_t* dst_side = nullptr) noexcept;

  core::JobStatus step(std::uint32_t budget_us) noexcept override;
  unsigned progress() const noexcept override;
  void cancel() noexcept override { cancelled_ = true; }
  const char* name() const noexcept override { return name_.c_str(); }

  std::uint32_t result_frames() const noexcept { return total_; }
  bool stereo() const noexcept { return src_s_ != nullptr; }
  const EditSpec& spec() const noexcept { return spec_; }
  // Normalise found nothing to do (silence, or already full scale), or DC removal found no offset: the result equals the source.
  bool unchanged() const noexcept { return (spec_.op == EditOp::Normalise && spec_.gain_q16 == 65536u) || (spec_.op == EditOp::DcRemove && dc_m_ == 0 && dc_s_ == 0); }
  // DC removal could not take the whole mean (full-scale frames held it back): partial, or nothing when unchanged() too.
  bool held() const noexcept { return held_; }
  // The resampler's kernel table (8 KB, SDRAM): the OG export's PCM stage borrows it (OS-FILES-C C1b, SampleBank::resample_kernel). Building is idempotent, a step continues where the last stopped, so
  // two users on the one main loop only ever add entries; a table that is ready() is only read.
  AmigaKernel& kernel() noexcept { return kernel_; }
  static constexpr unsigned kKernelPerStep = 256;  // kernel entries built a step (a sinf and a Bessel series each: about 0.3 ms)

 private:
  EditSpec spec_;
  const std::int16_t* src_ = nullptr;
  std::int16_t* dst_ = nullptr;
  const std::int16_t* src_s_ = nullptr;  // OS-STEREO (d): S of the source and of the result (nullptr: a mono sample); the pass over S follows the pass over M
  std::int16_t* dst_s_ = nullptr;
  std::int32_t dc_m_ = 0;                // DC remove: each block's offset (the spec's `dc` is the pass's own)
  std::int32_t dc_s_ = 0;
  std::uint32_t carry_ = 0;              // progress units of the passes that ended (the counters restart for S)
  bool side_pass_ = false;               // the scan or the write now runs over S
  std::uint32_t frames_ = 0;
  std::uint32_t total_ = 0;
  std::uint32_t done_ = 0;       // frames written
  std::uint32_t warmed_ = 0;     // Filter and Compressor on a loop (OS-A9-SEAM): warm-up frames run
  std::uint32_t scanned_ = 0;    // Normalise: frames of the range scanned
  std::uint32_t peak_ = 0;
  DcScan dc_;
  EditRun run_;  // Filter and Compressor: the state carried from one step to the next
  bool held_ = false;
  bool scanning_ = false;
  bool cancelled_ = false;
  core::FixedString<40> name_;
  AmigaKernel kernel_;  // 8 KB, built the first time an Amiga conversion runs and kept
};

}  // namespace o3os::app
