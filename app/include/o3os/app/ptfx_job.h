// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-CEPTFX S3a (docs/specs/OS-CEPTFX.md 3.5, call sequence 2.2): a core::Job that runs ONE loaded `.ptfx` effect plugin over the selection (or the whole) of a sample
// and writes the result into a buffer of the edit pool, beside SampleEditJob. The plugin's code is foreign: every entry is called through the call gate (hal/code_gate.h),
// one call per step (A17), and never inside a critical section.
//
// The phases, one bounded step each (the plugin calls are the long ones; long_step() tells the runner):
//   ZeroScratch (64 KiB a step) -> Init (host table built, latch armed, init()) -> Params (one set_param a step, all 12 indices, 0.0 past num_params)
//   -> Size (latch disarmed and read, the result sized, the buffer taken from the edit pool through `alloc`) -> CopyBefore ([0, a) of the source)
//   -> Process (one process_block of at most 8,192 frames a step, split at the prepend / body / append boundaries; `in` is NULL outside the body)
//   -> CopyAfter ([b, frames) after what was produced) -> Done.
// A GENERATOR (flag GENERATOR, S4c) ignores the source: Size takes kPtfxGeneratorFrames, nothing is copied, `in` is NULL throughout, the result is what Process produced (prepend and
// append are not read). Its Source needs no sample (pcm nullptr and frames 0 are an empty slot).
// A STEREO sample (S5, 3.6, A12, A13; the bank's M and S blocks) goes by its plugin's kind: a STEREO plugin in ONE pass with channels = 2, a plugin without STEREO in TWO mono passes (L, then
// R, the plugin re-initialised between; both must come out the same length), a CONVERT_TO_MONO plugin over the whole sample with channels = 2 (the result is L, mono), a generator as ever
// (mono). Each Process block's input is converted from M and S to L = sat(M + S), R = sat(M - S) in the `Plugin::stage` staging; the output goes to the result's M block (L) and S block
// (R), and Convert then turns the PRODUCED range back to M and S in place (M = (L + R + 1) >> 1, S = clamp16(L - M): the WAV importer's rounding without its dither, so a plugin that
// changes nothing gives the sample back as it was). The untouched head and tail are copied as M and S as they are; Pack moves S when the plugin produced less than it was given room for.
// A return of 0 or less ends Process (the frames produced are the result, A11); one above the block asked for fails the run. Nothing is committed here: the App
// stages the buffer and runs sample.commit (one undo unit) when the job is Done; a Failed or Cancelled job leaves the sample as it was.
//
// A plugin that times out, faults or runs off its stack (the gate's answer, or the guard painted below its stack) is put in the PtfxQuarantine and the run fails with the
// strip text of 3.3; the bytes are never committed. Main loop only (the job is stepped by the runner), no heap.

#include <o3os/core/job.h>
#include <o3os/module/ptfx_catalog.h>
#include <o3os/module/ptfx_format.h>
#include <o3os/module/ptfx_host_table.h>
#include <o3os/module/ptfx_loader.h>
#include <o3os/module/ptfx_quarantine.h>
#include <o3os/hal/code_gate.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

inline constexpr std::uint32_t kPtfxCallTimeoutMs = 2000;  // 3.3: the deadline of ONE plugin call
inline constexpr std::uint32_t kPtfxBlockFrames = 8192;    // 2.2: at most this many frames a process_block call
inline constexpr unsigned kPtfxParamSlots = module::kPtfxMaxParams;
inline constexpr std::uint32_t kPtfxGeneratorFrames = 88200;  // 3.5: a generator's result is this long (verified parity), trimmed to what it produced

class PtfxJob final : public core::Job {
 public:
  struct Plugin;
  // One plugin call: `entry` is an absolute address (the loader's Entries), the arguments are full-width so that a host stand-in can take real pointers. The default (nullptr in
  // Plugin::call) is the call gate with the plugin's stack and image. Must return the gate's verdict and the plugin's return value in `ret`.
  using CallFn = hal::code_gate::GateResult (*)(const Plugin& plugin, std::uintptr_t entry, std::uintptr_t r0, std::uintptr_t r1, std::uintptr_t r2, std::uintptr_t r3,
                                                float s0, std::int32_t* ret) noexcept;
  // The buffer for the result of `frames` frames, from the edit pool (SampleBank::edit_begin), or nullptr when there is no room. `stereo`: an M block and an S block, each even(frames) long, in
  // one buffer (the bank's layout); false: mono.
  using AllocFn = std::int16_t* (*)(void* ctx, std::uint32_t frames, bool stereo) noexcept;

  // The loaded plugin and its memory; owned by the caller, copied into the job at begin().
  struct Plugin {
    const module::PtfxInfo* info = nullptr;
    module::PtfxLoader::Entries entries{};   // init, set_param, process_block (absolute)
    module::PtfxHostTable* host = nullptr;   // built by the job at the start of every run (sample rate, math, rand seed, latch)
    std::uintptr_t host_table = 0;           // the table's address, the third argument of init
    void* scratch = nullptr;                 // `scratch_cap` bytes (the `ptfx` region's scratch); init gets min(info->scratch_size, scratch_cap, kPtfxMaxScratch)
    std::uint32_t scratch_cap = 0;
    std::uintptr_t stack_top = 0;            // the plugin stack and what the gate needs to diagnose a fault
    std::uintptr_t stack_bottom = 0;
    std::uintptr_t image_base = 0;
    std::uint32_t image_bytes = 0;
    void* guard = nullptr;                   // the 4 KiB guard under the stack (painted at the start, checked after every call); nullptr: none
    std::uint32_t guard_bytes = 0;
    std::uint16_t id = 0;                    // A7: 0x8000 | hash
    std::uint32_t sample_rate = 0;           // the instrument's rate; 0 means 44,100 (A8)
    const float* values = nullptr;           // 12 parameter values (A15), nullptr: every default
    module::PtfxQuarantine* quarantine = nullptr;  // where a timeout / fault / overflow goes (nullptr: none kept)
    module::PtfxCatalog* catalog = nullptr;        // marks the entry quarantined, and tells whether a Why::Crash id belongs to a listed plugin (nullptr: every id is honoured)
    std::int16_t* stage = nullptr;           // S5: 2 x kPtfxBlockFrames samples (L, then R) the stereo input of a block is converted into (the ptfx region's 32 KiB staging); nullptr: no stereo
    CallFn call = nullptr;                   // nullptr: the call gate; tests put a host stand-in here
  };

  // What the run reads and writes. `pcm` is M (the whole sample when mono); the range [a, b) of it is the body (a == b or out of range: the whole sample).
  struct Source {
    const std::int16_t* pcm = nullptr;
    std::uint32_t frames = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    bool stereo = false;                   // the sample is stereo: `side` is its S block (even(frames) behind M in the bank); false or no `side`: mono
    const std::int16_t* side = nullptr;
    AllocFn alloc = nullptr;
    void* alloc_ctx = nullptr;
  };

  // Starts a run. nullptr when it started (hand the job to the runner); otherwise the strip text of the refusal and nothing changed.
  const char* begin(const Plugin& plugin, const Source& source) noexcept;
  // Which check made the last begin() say PLUGIN NOT READY ("plugin", "stereo", "scratch"), else nullptr: for the console's reply only (the strip keeps its one text).
  const char* not_ready_why() const noexcept { return why_; }
  void clear_not_ready_why() noexcept { why_ = nullptr; }

  core::JobStatus step(std::uint32_t budget_us) noexcept override;
  unsigned progress() const noexcept override;
  void cancel() noexcept override;
  bool long_step() const noexcept override { return true; }
  const char* name() const noexcept override { return name_; }
  const char* detail() const noexcept override { return detail_; }

  // The run as the commit needs it (valid from begin() on): the body's range in the source, what the plugin asked for before and after it (0 for a generator), the kind.
  std::uint32_t body_a() const noexcept { return src_.a; }
  std::uint32_t body_b() const noexcept { return src_.b; }
  std::uint32_t prepend() const noexcept { return prepend_; }
  std::uint32_t append() const noexcept { return append_; }
  bool generator() const noexcept { return gen_; }
  bool stereo_result() const noexcept { return mode_ == Mode::Stereo || mode_ == Mode::TwoPass; }  // the buffer is M then S (S at result() + even(result_frames())); else mono
  bool wavetable() const noexcept { return plugin_.info != nullptr && (plugin_.info->flags & module::kPtfxFlagWavetable) != 0u; }

  // After Done: the buffer written (the frames of the result), what the App stages for sample.commit.
  std::int16_t* result() const noexcept { return dst_; }
  std::uint32_t result_frames() const noexcept { return result_; }
  bool done() const noexcept { return phase_ == Phase::Done; }

 private:
  enum class Phase : std::uint8_t { Idle, ZeroScratch, Init, Params, Size, CopyBefore, Process, Convert, CopyAfter, Pack, Done, Ended };
  // How the sample goes through the plugin: Mono (also a generator), Stereo (one pass, channels = 2), TwoPass (L, then R, channels = 1), ToMono (channels = 2, the result is L only).
  enum class Mode : std::uint8_t { Mono, Stereo, TwoPass, ToMono };

  bool call(std::uintptr_t entry, std::uintptr_t r0, std::uintptr_t r1, std::uintptr_t r2, std::uintptr_t r3, float s0, std::int32_t* ret) noexcept;
  core::JobStatus fail(const char* why) noexcept;
  bool copy_step(const std::int16_t* from, std::int16_t* to, std::uint32_t count) noexcept;
  bool copy_lanes(std::uint32_t from, std::uint32_t to, std::uint32_t count) noexcept;  // copy_step of M, then of S when the result is stereo
  void stage_block(const std::int16_t* m, const std::int16_t* s, std::uint32_t n, bool left, bool right) noexcept;

  const char* why_ = nullptr;  // not_ready_why (main loop only)
  Plugin plugin_{};
  Source src_{};
  const char* detail_ = "";
  std::int16_t* dst_ = nullptr;
  const std::int16_t* in_arr_[2] = {nullptr, nullptr};  // process_block's `in` (the channel pointers) and `out`; the plugin reads them while it runs
  std::int16_t* out_arr_[2] = {nullptr, nullptr};
  std::uint32_t even_ = 0;   // even(frames of dst_): where the result's S block starts (stereo modes; ToMono uses it as the discarded R's block)
  std::uint32_t pass0_ = 0;  // TwoPass: the frames the L pass produced (the R pass must match)
  std::uint32_t conv_ = 0;   // frames converted back to M and S so far
  std::uint32_t scratch_bytes_ = 0;
  std::uint32_t zeroed_ = 0;
  std::uint32_t prepend_ = 0;
  std::uint32_t append_ = 0;
  std::uint32_t pos_ = 0;     // frames produced by Process so far
  std::uint32_t total_ = 0;   // prepend + body + append
  std::uint32_t copied_ = 0;  // frames copied by the current copy phase
  std::uint32_t result_ = 0;
  std::uint32_t target_ = 0;  // the frames the run writes at most (the progress denominator)
  std::uint8_t param_ = 0;
  bool gen_ = false;  // the plugin is a generator (set by begin())
  Mode mode_ = Mode::Mono;
  std::uint8_t pass_ = 0;  // TwoPass: 0 is L, 1 is R
  std::uint8_t lane_ = 0;  // copy_lanes: 0 is M, 1 is S
  Phase phase_ = Phase::Idle;
  char name_[24] = "PLUGIN";
};

}  // namespace o3os::app
