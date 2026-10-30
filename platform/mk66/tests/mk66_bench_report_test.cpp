// SPDX-License-Identifier: GPL-3.0-or-later
//
// `diag bench` (bench_report.h; OS-PERF3): the kernel list, the statistics of a run and the lines it prints. Host and Cortex-M4F (`qemu_mk66`). The measuring itself (bench.cpp) needs
// the hardware's cycle counter and the live audio path and is the unit's HIL item.
#include <doctest/doctest.h>

#include <o3os/platform/mk66/bench_report.h>

#include <cstring>

namespace bench = o3os::platform::mk66::bench;

TEST_CASE("mk66 bench: every kernel has a name that find_kernel returns, in the table's order") {
  constexpr std::size_t kCount = static_cast<std::size_t>(bench::Kernel::Count);
  CHECK(kCount == 24u);  // (OS-303PERF: tb303_128 joined; OS-LOOPER L2b: looper; OS-USBDISK U1d: disk; OS-ODD11 UTAIL: burst; OS-DJ: dj; OS-PERF-CPU2: voice_rev, voice_pan)
  for (std::size_t i = 0; i < kCount; ++i) {
    const bench::KernelInfo& k = bench::kKernelInfo[i];
    INFO(k.name);
    CHECK(bench::find_kernel(k.name) == static_cast<bench::Kernel>(i));
    CHECK(std::strlen(k.help) <= 80u);
    CHECK(std::strchr(k.help, '\n') == nullptr);
    for (const char* c = k.name; *c != '\0'; ++c) {
      CHECK(((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'));
    }
    for (std::size_t j = i + 1; j < kCount; ++j) {
      CHECK(std::strcmp(k.name, bench::kKernelInfo[j].name) != 0);
    }
  }
  CHECK(bench::find_kernel("looper") == bench::Kernel::Looper);
  CHECK(bench::find_kernel("disk") == bench::Kernel::Disk);
  CHECK(bench::find_kernel("burst") == bench::Kernel::Burst);
  CHECK(bench::find_kernel("dj") == bench::Kernel::DjMaster);
  CHECK(bench::find_kernel("voice") == bench::Kernel::Voice);
  CHECK(bench::find_kernel("tb303") == bench::Kernel::Tb303);
  CHECK(bench::find_kernel("tb303_128") == bench::Kernel::Tb303x128);  // OS-303PERF: the same instance asked for 128 frames a call
  CHECK(bench::find_kernel("voice_f") == bench::Kernel::VoiceFilter);
  CHECK(bench::find_kernel("voice_od") == bench::Kernel::VoiceDrive);  // OS-IFX1: the sampler voice with the overdrive, the bit depth, both
  CHECK(bench::find_kernel("voice_bit") == bench::Kernel::VoiceCrush);
  CHECK(bench::find_kernel("voice_odbit") == bench::Kernel::VoiceBoth);
  CHECK(bench::find_kernel("voice_rev") == bench::Kernel::VoiceRev);  // OS-PERF-CPU2: the reversed voice
  CHECK(bench::find_kernel("voice_pan") == bench::Kernel::VoicePan);  // OS-PERF-CPU2: the voice with a pan LFO
  CHECK(bench::find_kernel("limiter") == bench::Kernel::Limiter);
  CHECK(bench::find_kernel("limiter_q") == bench::Kernel::LimiterQuiet);
  CHECK(bench::find_kernel("reverb_full") == bench::Kernel::ReverbFull);
  CHECK(bench::find_kernel("") == bench::Kernel::Count);
  CHECK(bench::find_kernel("all") == bench::Kernel::Count);  // `all` is the console's word, not a kernel
  CHECK(bench::find_kernel("delay2") == bench::Kernel::Count);
  CHECK(bench::find_kernel("dela") == bench::Kernel::Count);
}

TEST_CASE("mk66 bench: the statistics are the minimum, the maximum and the rounded mean; an empty run reads zero") {
  bench::Stat s;
  CHECK(s.count() == 0u);
  CHECK(s.min() == 0u);
  CHECK(s.max() == 0u);
  CHECK(s.avg() == 0u);
  s.add(1000);
  s.add(1200);
  s.add(1100);
  s.add(1101);
  CHECK(s.count() == 4u);
  CHECK(s.min() == 1000u);
  CHECK(s.max() == 1200u);
  CHECK(s.avg() == 1100u);  // 4401 / 4 = 1100.25
  bench::Stat t;
  t.add(1);
  t.add(2);
  CHECK(t.avg() == 2u);  // 1.5 rounds up
}

TEST_CASE("mk66 bench: a sample is clamped so that the longest run cannot overflow the sum") {
  bench::Stat s;
  for (std::uint32_t i = 0; i < bench::kMaxBlocks; ++i) {
    s.add(0xFFFFFFFFu);  // a counter that wrapped
  }
  CHECK(s.count() == bench::kMaxBlocks);
  CHECK(s.max() == bench::kMaxCycles);
  CHECK(s.min() == bench::kMaxCycles);
  CHECK(s.avg() == bench::kMaxCycles);
  CHECK(static_cast<std::uint64_t>(bench::kMaxBlocks) * bench::kMaxCycles <= 0xFFFFFFFFull);
}

TEST_CASE("mk66 bench: the BENCH line is the documented one") {
  bench::Stat s;
  s.add(2500);
  s.add(2468);
  s.add(2987);
  char buf[128];
  const std::size_t n = bench::format_line("voice", s, nullptr, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  CHECK(std::strcmp(buf, "BENCH voice 2468 3 avg 2652 max 2987\n") == 0);
  bench::format_line("tb303", s, "TB-303", buf, sizeof buf);
  CHECK(std::strcmp(buf, "BENCH tb303 2468 3 avg 2652 max 2987 (TB-303)\n") == 0);
  bench::format_line("tb303", s, "", buf, sizeof buf);
  CHECK(std::strcmp(buf, "BENCH tb303 2468 3 avg 2652 max 2987\n") == 0);
  char small[16];
  bench::format_line("voice", s, nullptr, small, sizeof small);
  CHECK(std::strlen(small) == sizeof small - 1u);  // bounded, terminated
}

TEST_CASE("mk66 bench: a kernel that cannot run says why on the same prefix") {
  char buf[128];
  const std::size_t n = bench::format_na("tb303", "no instance", buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  CHECK(std::strcmp(buf, "BENCH tb303 n/a: no instance\n") == 0);
}

TEST_CASE("mk66 bench: the memory rows print hundredths of a cycle") {
  char buf[128];
  bench::format_mem_row("sram_u", 130, 105, buf, sizeof buf);
  CHECK(std::strcmp(buf, "MEM sram_u seq 1.30 far 1.05 cycles/load\n") == 0);
  bench::format_mem_row("sdram", 2307, 4012, buf, sizeof buf);
  CHECK(std::strcmp(buf, "MEM sdram seq 23.07 far 40.12 cycles/load\n") == 0);
  bench::format_mem_row("flash", 5, 0, buf, sizeof buf);
  CHECK(std::strcmp(buf, "MEM flash seq 0.05 far 0.00 cycles/load\n") == 0);
}

TEST_CASE("mk66 bench: the variants are named, the empty name is not one") {
  CHECK(bench::find_variant("alias") == bench::Variant::Alias);
  CHECK(bench::find_variant("sram") == bench::Variant::Sram);
  CHECK(bench::find_variant("direct") == bench::Variant::Direct);
  CHECK(bench::find_variant("") == bench::Variant::Count);  // Normal has no word
  CHECK(bench::find_variant("64") == bench::Variant::Count);
  CHECK(bench::find_variant("voice") == bench::Variant::Count);
  CHECK(std::strcmp(bench::kVariantName[static_cast<unsigned>(bench::Variant::Normal)], "") == 0);
}

TEST_CASE("mk66 bench: the WORST line splits one render by stage") {
  const std::uint32_t stage[3] = {2568, 2672, 25330};
  const char* const names[3] = {"input", "drain", "render"};
  char buf[128];
  const std::size_t n = bench::format_worst(207243, stage, 3, names, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  CHECK(std::strcmp(buf, "WORST 207243 input 2568 drain 2672 render 25330\n") == 0);
}

// OS-MODSILENT: the output check. A kernel that makes a signal reports its peak, and an output of zeros (or NaN) is a FAIL line.
TEST_CASE("mk66 bench: the peak of a Q15 block, of a float block, and when a block counts as silent") {
  bench::Peak none;
  CHECK(none.silent());  // nothing looked at is not an output
  CHECK(none.peak() == 0u);
  const std::int16_t zeros[8] = {};
  bench::Peak z;
  z.add_q15(zeros, 8);
  CHECK(z.samples() == 8u);
  CHECK(z.peak() == 0u);
  CHECK(z.silent());
  const std::int16_t sig[4] = {100, -2300, 40, -32768};
  bench::Peak s;
  s.add_q15(sig, 3);
  CHECK(s.peak() == 2300u);
  CHECK(!s.silent());
  s.add_q15(sig + 3, 1);
  CHECK(s.peak() == 32768u);  // -32768 is a full-scale sample, not a negative number
  s.add_q15(nullptr, 4);      // a track with no block this time: nothing added, nothing counted
  CHECK(s.samples() == 4u);
  const float f[5] = {0.0f, -0.25f, 0.125f, 0.0f, 1.5f};
  bench::Peak fp;
  fp.add_float(f, 3);
  CHECK(fp.peak() == 8192u);  // 0.25 of full scale
  CHECK(!fp.silent());
  fp.add_float(f + 4, 1);
  CHECK(fp.peak() == 49152u);  // a float block may exceed full scale: reported as it is
  bench::Peak tiny;
  const float quiet[2] = {1.0e-6f, -1.0e-6f};  // under one Q15 step
  tiny.add_float(quiet, 2);
  CHECK(tiny.silent());
  bench::Peak huge;
  const float big[1] = {1.0e30f};
  huge.add_float(big, 1);
  CHECK(huge.peak() == 1000000u);  // capped: no overflow of the conversion
}

TEST_CASE("mk66 bench: a NaN in the output is a failure however loud the rest is") {
  const float v[4] = {0.5f, __builtin_nanf(""), -0.5f, 0.25f};
  bench::Peak p;
  p.add_float(v, 4);
  CHECK(p.nan_count() == 1u);
  CHECK(p.peak() == 16384u);
  CHECK(p.silent());
  char note[64];
  bench::format_peak_note("TB-303", p, note, sizeof note);
  CHECK(std::strcmp(note, "TB-303 peak 16384 1 NaN") == 0);
  char buf[160];
  const std::size_t n = bench::format_fail("tb303", p, nullptr, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  CHECK(std::strcmp(buf, "BENCH tb303 FAIL: the output has 1 NaN (peak 16384)\n") == 0);
}

TEST_CASE("mk66 bench: the note and the FAIL line of the output check are the documented ones") {
  const float v[2] = {0.5f, -0.125f};
  bench::Peak ok;
  ok.add_float(v, 2);
  char note[64];
  const std::size_t n = bench::format_peak_note("TB-303", ok, note, sizeof note);
  CHECK(n == std::strlen(note));
  CHECK(std::strcmp(note, "TB-303 peak 16384") == 0);
  char line[160];
  bench::Stat st;
  st.add(46000);
  bench::format_line("tb303", st, note, line, sizeof line);
  CHECK(std::strcmp(line, "BENCH tb303 46000 1 avg 46000 max 46000 (TB-303 peak 16384)\n") == 0);

  const float zeros[2] = {0.0f, 0.0f};
  bench::Peak silent;
  silent.add_float(zeros, 2);
  char buf[160];
  bench::format_fail("tb303", silent, nullptr, buf, sizeof buf);
  CHECK(std::strcmp(buf, "BENCH tb303 FAIL: the output is silent (peak 0 of 32768)\n") == 0);
  bench::format_fail("voice+alias", silent, "a silent sample, or a dead voice path", buf, sizeof buf);
  CHECK(std::strcmp(buf, "BENCH voice+alias FAIL: the output is silent (peak 0 of 32768) (a silent sample, or a dead voice path)\n") == 0);
  bench::Peak nothing;
  bench::format_fail("tb303", nothing, "", buf, sizeof buf);
  CHECK(std::strcmp(buf, "BENCH tb303 FAIL: no output was produced\n") == 0);
  char small[24];
  bench::format_fail("tb303", silent, nullptr, small, sizeof small);
  CHECK(std::strlen(small) == sizeof small - 1u);  // bounded, terminated
}
