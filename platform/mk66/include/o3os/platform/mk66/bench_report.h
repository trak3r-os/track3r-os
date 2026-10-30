// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// `diag bench <kernel> [blocks]` (OS-PERF3): the kernel list, the statistics of a run and the one-line text a run prints. Pure: no hardware, host-tested. The measuring is
// src/bench.cpp.
//
// THE LINE (the HIL scenarios and the notes in the perf notes grep it, so it is stable):
//   BENCH <kernel> <cycles/block> <blocks> avg <cycles> max <cycles>[ <note>]
// <cycles/block> is the MINIMUM over the run, DWT cycles of the 180 MHz core for one block of 64 frames: the figure of the kernel with the caches warm and nothing in front of it,
// which is what a change to the code or the placement moves. avg and max add the interrupts that got in (the audio DMA, USB, SysTick) and say how steady it is. The worst case of the
// whole render, the one that decides an underrun, is `diag cpu` (the stage maxima), not this.
// So the three statistics of the task protocol (docs/qa/catalogue/global.md) are on every line: the first number after the name is the MIN, then the number of blocks, `avg` the MEAN, `max`.
// `tb303` asks the module for 64 frames per call (the engine's block), `tb303_128` for 128 (O3M_BLOCK_MAX): the note carries cycles per frame, so the two read against each other.

#include <o3os/audio/diag.h>

#include <cstddef>
#include <cstdint>

namespace o3os::platform::mk66::bench {

enum class Kernel : std::uint8_t { Voice, VoiceFilter, VoiceDrive, VoiceCrush, VoiceBoth, VoiceRev, VoicePan, Tb303, Tb303x128, Delay, Reverb, ReverbFull, Limiter, LimiterQuiet, Strips, Stages, Mem, Worst, Kernels, Cpuopt, Looper, Disk, Burst, DjMaster, Count };

struct KernelInfo {
  const char* name;  // lower case, digits and '_'; the argument of `bench`
  const char* help;  // one line
};

inline constexpr KernelInfo kKernelInfo[] = {
    {"voice", "one sampler voice, a block (the longest plain sample; takes track 16)"},
    {"voice_f", "one sampler voice through the instrument filter (low-pass, resonance 2), a block"},
    {"voice_od", "one sampler voice with the overdrive at 100 (OS-IFX1), a block"},
    {"voice_bit", "one sampler voice at a bit depth of 8 (OS-IFX1), a block"},
    {"voice_odbit", "one sampler voice with overdrive 100 and 8 bits (OS-IFX1), a block"},
    {"voice_rev", "one sampler voice played backward (PLAYBACK, OS-PERF-CPU2), a block"},
    {"voice_pan", "one sampler voice with a pan LFO (OS-PERF-CPU2), a block"},
    {"tb303", "one TB-303 module instance, a block (mod load tb303; mod bind 1 tb303)"},
    {"tb303_128", "the same TB-303 instance asked for 128 frames in one call (OS-303PERF, E2)"},
    {"delay", "SND A delay, stereo taps, feedback 70, a block"},
    {"reverb", "SND B reverb, HALF quality, a block"},
    {"reverb_full", "SND B reverb, FULL quality, a block"},
    {"limiter", "master limiter, loud noise (limiting on every sample), slow stage on"},
    {"limiter_q", "master limiter, quiet input (the identity path), slow stage on"},
    {"strips", "in situ: the strips stage, N renders of whatever is playing"},
    {"stages", "in situ: every stage of the render, N renders of whatever is playing"},
    {"mem", "cycles a 32-bit load takes from SRAM_U, SRAM_L, flash, SDRAM (no block count)"},
    {"worst", "the slowest render since cpu reset, split by stage (no block count)"},
    {"kernels", "the graph's SIMD kernels on 64 samples each (gain, mac, strips, meters ...)"},
    {"cpuopt", "OS-CPUOPT1 A/B: loop player block and SVF, old against new, with equality lines"},
    {"looper", "OS-LOOPER L2b: overdub RMW, copy-on-write copy, restore step, equality lines"},
    {"disk", "OS-USBDISK U1d: card reads, scratch-file writes, USB window (reset|clean)"},
    {"burst", "OS-ODD11 UTAIL: 12 filtered voices: note_ons, first block, steady, 4 spans"},
    {"dj", "OS-DJ: kill EQ held (LOW MID HIGH all), DJ filter, filter + kill, a block each"},
};
static_assert(sizeof(kKernelInfo) / sizeof(kKernelInfo[0]) == static_cast<std::size_t>(Kernel::Count), "one row per Kernel");

// Where a kernel's data is, for the A/B of the placement work. Normal is the production placement (the live instance's own state; the SDRAM the core works in through the address
// sdram_view.h gives it); Alias and Direct force the SDRAM data of the delay and reverb memory, the sampler voice's sample and a COPY of the TB-303's state to the cached ICode alias
// (0x0800_0000) or to the uncached system-bus address (0x7000_0000); Sram runs a copy of the TB-303's state from the stack (SRAM_U). A kernel a variant does not apply to says so.
enum class Variant : std::uint8_t { Normal, Alias, Sram, Direct, Count };
inline constexpr const char* kVariantName[] = {"", "alias", "sram", "direct"};
static_assert(sizeof(kVariantName) / sizeof(kVariantName[0]) == static_cast<std::size_t>(Variant::Count), "one name per Variant");

inline constexpr std::uint32_t kDefaultBlocks = 64;
inline constexpr std::uint32_t kMaxBlocks = 4096;  // the sum of a run fits 32 bits at kMaxCycles a sample
inline constexpr std::uint32_t kMaxCycles = 1000000u;

constexpr bool same_name(const char* a, const char* b) noexcept {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// The kernel called `name`, or Kernel::Count.
constexpr Kernel find_kernel(const char* name) noexcept {
  for (std::size_t i = 0; i < static_cast<std::size_t>(Kernel::Count); ++i) {
    if (same_name(kKernelInfo[i].name, name)) {
      return static_cast<Kernel>(i);
    }
  }
  return Kernel::Count;
}

// The variant called `name` ("alias", "sram", "direct"), or Variant::Count.
constexpr Variant find_variant(const char* name) noexcept {
  for (std::size_t i = 1; i < static_cast<std::size_t>(Variant::Count); ++i) {
    if (same_name(kVariantName[i], name)) {
      return static_cast<Variant>(i);
    }
  }
  return Variant::Count;
}

// The statistics of a run: the minimum, the maximum and the mean of the cycle counts added.
class Stat {
 public:
  void add(std::uint32_t cycles) noexcept {
    cycles = cycles > kMaxCycles ? kMaxCycles : cycles;
    min_ = cycles < min_ ? cycles : min_;
    max_ = cycles > max_ ? cycles : max_;
    sum_ += cycles;
    ++n_;
  }
  std::uint32_t count() const noexcept { return n_; }
  std::uint32_t min() const noexcept { return n_ != 0u ? min_ : 0u; }
  std::uint32_t max() const noexcept { return max_; }
  std::uint32_t avg() const noexcept { return n_ != 0u ? (sum_ + n_ / 2u) / n_ : 0u; }

 private:
  std::uint32_t min_ = 0xFFFFFFFFu;
  std::uint32_t max_ = 0;
  std::uint32_t sum_ = 0;
  std::uint32_t n_ = 0;
};

// THE OUTPUT CHECK (OS-MODSILENT). A cycle count says that a kernel ran, not that it produced anything: the TB-303 bench of the OS-PERF3 images timed a render that nobody listened to. The kernels
// that make a signal of their own (the TB-303 instance, the sampler voice) accumulate the peak of their last blocks here, outside the timed part, and the line says it ("peak N", of 32768
// full scale); an output that is all zero, or has a NaN in it, ends the run with a `BENCH <name> FAIL: ...` line, which a HIL scenario (and a person) greps. Not for the kernels whose
// output is legitimately zero for a while (the delay before its first repeat, the reverb's pre-delay).
class Peak {
 public:
  static constexpr std::uint32_t kFullScale = 32768u;
  // `n` Q15 samples.
  void add_q15(const std::int16_t* p, std::size_t n) noexcept {
    for (std::size_t i = 0; p != nullptr && i < n; ++i) {
      const std::int32_t v = p[i];
      const std::uint32_t a = static_cast<std::uint32_t>(v < 0 ? -v : v);
      peak_ = a > peak_ ? a : peak_;
    }
    seen_ += p != nullptr ? n : 0u;
  }
  // `n` samples of a float block (+-1.0 is full scale): the module's own output, before the conversion.
  void add_float(const float* p, std::size_t n) noexcept {
    for (std::size_t i = 0; p != nullptr && i < n; ++i) {
      const float v = p[i];
      if (!(v == v)) {
        ++nan_;
        continue;
      }
      const float a = (v < 0.0f ? -v : v) * static_cast<float>(kFullScale);
      const std::uint32_t q = a >= 1.0e6f ? 1000000u : static_cast<std::uint32_t>(a);
      peak_ = q > peak_ ? q : peak_;
    }
    seen_ += p != nullptr ? n : 0u;
  }
  std::uint32_t peak() const noexcept { return peak_; }       // 0..32768 and a little over for a float block that clips (capped at 1,000,000)
  std::uint32_t nan_count() const noexcept { return nan_; }
  std::size_t samples() const noexcept { return seen_; }
  // Nothing came out: no sample looked at, all of them zero (a peak below one Q15 step), or a NaN among them.
  bool silent() const noexcept { return seen_ == 0u || peak_ == 0u || nan_ != 0u; }

 private:
  std::uint32_t peak_ = 0;
  std::uint32_t nan_ = 0;
  std::size_t seen_ = 0;
};

// The note of a result line for a kernel that has an output check: `<label> peak <N>`.
inline std::size_t format_peak_note(const char* label, const Peak& p, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str(label).str(" peak ").dec(p.peak());
  if (p.nan_count() != 0u) {
    t.chr(' ').dec(p.nan_count()).str(" NaN");
  }
  return t.size();
}

// The failed check: `BENCH <name> FAIL: the output is silent ...` (one line, trailing newline); `hint` (may be null) is appended in parentheses.
inline std::size_t format_fail(const char* name, const Peak& p, const char* hint, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("BENCH ").str(name).str(" FAIL: ");
  if (p.nan_count() != 0u) {
    t.str("the output has ").dec(p.nan_count()).str(" NaN (peak ").dec(p.peak()).str(")");
  } else if (p.samples() == 0u) {
    t.str("no output was produced");
  } else {
    t.str("the output is silent (peak 0 of 32768)");
  }
  if (hint != nullptr && *hint != '\0') {
    t.str(" (").str(hint).chr(')');
  }
  t.nl();
  return t.size();
}

// The result line of a run, with a trailing newline. `note` (may be null) is appended in parentheses.
inline std::size_t format_line(const char* name, const Stat& s, const char* note, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("BENCH ").str(name).chr(' ').dec(s.min()).chr(' ').dec(s.count()).str(" avg ").dec(s.avg()).str(" max ").dec(s.max());
  if (note != nullptr && *note != '\0') {
    t.str(" (").str(note).chr(')');
  }
  t.nl();
  return t.size();
}

// A kernel that cannot run: `BENCH <name> n/a: <why>`.
inline std::size_t format_na(const char* name, const char* why, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("BENCH ").str(name).str(" n/a: ").str(why).nl();
  return t.size();
}

// One row of the memory characterisation: the cost of a 32-bit load, in hundredths of a cycle, walking the region by word (`seq`) and by a stride that opens a new SDRAM row (`far`),
// loop overhead included (the SRAM_U row is the floor). `MEM <region> seq <x.xx> far <x.xx> cycles/load`.
inline std::size_t format_mem_row(const char* region, std::uint32_t seq_x100, std::uint32_t far_x100, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("MEM ").str(region).str(" seq ").dec(seq_x100 / 100u).chr('.').chr(static_cast<char>('0' + (seq_x100 / 10u) % 10u)).chr(static_cast<char>('0' + seq_x100 % 10u));
  t.str(" far ").dec(far_x100 / 100u).chr('.').chr(static_cast<char>('0' + (far_x100 / 10u) % 10u)).chr(static_cast<char>('0' + far_x100 % 10u)).str(" cycles/load\n");
  return t.size();
}

// `WORST <total> input <n> drain <n> ... commit <n>`: one render, the slowest since the last `cpu reset`, as its stages (names as in `diag cpu`).
inline std::size_t format_worst(std::uint32_t total, const std::uint32_t* stage, unsigned stages, const char* const* names, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("WORST ").dec(total);
  for (unsigned i = 0; i < stages; ++i) {
    t.chr(' ').str(names[i]).chr(' ').dec(stage[i]);
  }
  t.nl();
  return t.size();
}

}  // namespace o3os::platform::mk66::bench
