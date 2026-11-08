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
#include <o3os/platform/mk66/bench_text_ids.h>

#include <cstddef>
#include <cstdint>

namespace o3os::platform::mk66::bench {

enum class Kernel : std::uint8_t { Voice, VoiceFilter, VoiceDrive, VoiceCrush, VoiceBoth, VoiceRev, VoicePan, VoiceSt, Tb303, Tb303x128, Delay, Reverb, ReverbFull, Limiter, LimiterQuiet, Strips, Stages, Mem, Worst, Kernels, Cpuopt, Looper, Disk, Burst, DjMaster, Insert, Wt, Count };

struct KernelInfo {
  const char* name;  // lower case, digits and '_'; the argument of `bench`
  std::uint16_t help;  // the id of the one-line help (btxt::k_<name>, text/bench_text.txt; bench_text(help) is the text)
};

inline constexpr KernelInfo kKernelInfo[] = {
    {"voice", btxt::k_voice},
    {"voice_f", btxt::k_voice_f},
    {"voice_od", btxt::k_voice_od},
    {"voice_bit", btxt::k_voice_bit},
    {"voice_odbit", btxt::k_voice_odbit},
    {"voice_rev", btxt::k_voice_rev},
    {"voice_pan", btxt::k_voice_pan},
    {"voice_st", btxt::k_voice_st},
    {"tb303", btxt::k_tb303},
    {"tb303_128", btxt::k_tb303_128},
    {"delay", btxt::k_delay},
    {"reverb", btxt::k_reverb},
    {"reverb_full", btxt::k_reverb_full},
    {"limiter", btxt::k_limiter},
    {"limiter_q", btxt::k_limiter_q},
    {"strips", btxt::k_strips},
    {"stages", btxt::k_stages},
    {"mem", btxt::k_mem},
    {"worst", btxt::k_worst},
    {"kernels", btxt::k_kernels},
    {"cpuopt", btxt::k_cpuopt},
    {"looper", btxt::k_looper},
    {"disk", btxt::k_disk},
    {"burst", btxt::k_burst},
    {"dj", btxt::k_dj},
    {"insert", btxt::k_insert},
    {"wt", btxt::k_wt},
};
static_assert(sizeof(kKernelInfo) / sizeof(kKernelInfo[0]) == static_cast<std::size_t>(Kernel::Count), "one row per Kernel");

// OS-FLASHDIET B1: the retired one-off A/B benches. They are compiled into a bench image only with -DO3OS_BENCH_ARCHIVE=ON; without it the console still lists them (their help says so) and `run`
// answers `not in this image (O3OS_BENCH_ARCHIVE)`. The kernels every CPU measurement uses are not archived.
constexpr bool archived(Kernel k) noexcept {
  return k == Kernel::Cpuopt || k == Kernel::Looper || k == Kernel::VoiceSt || k == Kernel::Disk;
}

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

// OS-FLASHDIET B1: the answer of an archived kernel in a bench image built without O3OS_BENCH_ARCHIVE (the console still lists it): `ERR <name> is not in this image (O3OS_BENCH_ARCHIVE)`.
inline std::size_t format_archived(const char* name, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("ERR ").str(name).str(" is not in this image (O3OS_BENCH_ARCHIVE)").nl();
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
