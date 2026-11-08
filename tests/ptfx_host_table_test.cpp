// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S2a: the `.ptfx` host table builder (module/ptfx_host_table.h, docs/specs/OS-CEPTFX.md 3.4): the 68 bytes in
// 2.1's field order with our own offset constants, sample_rate (0 means 44,100, A8), every function slot pointing at the
// right core function, the rand LCG reseeded per build with newlib's range, and the set_extra_samples latch armed in the
// init phase and ignored afterwards (A10). Host only: on the host build a slot holds the pointer's low word, so the test
// compares the same truncation.
#include <doctest/doctest.h>

#include <o3os/core/fast_math.h>
#include <o3os/core/ptfx_math.h>
#include <o3os/module/ptfx_host_table.h>

#include <cstdint>
#include <cstring>

using namespace o3os::module;
namespace pm = o3os::core::ptfx_math;

namespace {

std::uint32_t word_at(const std::uint8_t* t, std::uint32_t off) {  // little endian, explicit
  return static_cast<std::uint32_t>(t[off]) | (static_cast<std::uint32_t>(t[off + 1]) << 8) | (static_cast<std::uint32_t>(t[off + 2]) << 16) |
         (static_cast<std::uint32_t>(t[off + 3]) << 24);
}

template <typename Fn>
std::uint32_t low_word(Fn fn) noexcept {
  return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(fn));  // the slots are 4 bytes: exact on the target
}

}  // namespace

TEST_CASE("ptfx_host_table: 68 bytes, the 2.1 field order pinned") {
  CHECK(kPtfxHostTableBytes == 68u);
  CHECK(kPtfxHostSampleRateOff == 0u);
  CHECK(kPtfxHostSinfOff == 4u);
  CHECK(kPtfxHostCosfOff == 8u);
  CHECK(kPtfxHostTanfOff == 12u);
  CHECK(kPtfxHostTanhfOff == 16u);
  CHECK(kPtfxHostPowfOff == 20u);
  CHECK(kPtfxHostExpfOff == 24u);
  CHECK(kPtfxHostSqrtfOff == 28u);
  CHECK(kPtfxHostLogfOff == 32u);
  CHECK(kPtfxHostLog10fOff == 36u);
  CHECK(kPtfxHostFminfOff == 40u);
  CHECK(kPtfxHostFmaxfOff == 44u);
  CHECK(kPtfxHostFabsfOff == 48u);
  CHECK(kPtfxHostMemcpyOff == 52u);
  CHECK(kPtfxHostMemsetOff == 56u);
  CHECK(kPtfxHostRandOff == 60u);
  CHECK(kPtfxHostSetExtraSamplesOff == 64u);
  CHECK(kPtfxHostSetExtraSamplesOff + 4u == kPtfxHostTableBytes);
}

TEST_CASE("ptfx_host_table: build fills sample_rate (0 means 44,100) and every slot with its function, in order") {
  alignas(4) std::uint8_t table[kPtfxHostTableBytes];
  std::memset(table, 0xEE, sizeof table);
  PtfxHostTable t(table);
  t.build(0u);
  CHECK(word_at(table, kPtfxHostSampleRateOff) == 44100u);  // A8
  t.build(48000u);
  CHECK(word_at(table, kPtfxHostSampleRateOff) == 48000u);
  t.build(0u);
  CHECK(word_at(table, kPtfxHostSinfOff) == low_word(&o3os::core::sin_fast));
  CHECK(word_at(table, kPtfxHostCosfOff) == low_word(&o3os::core::cos_fast));
  CHECK(word_at(table, kPtfxHostTanfOff) == low_word(&pm::tanf_));
  CHECK(word_at(table, kPtfxHostTanhfOff) == low_word(&pm::tanhf_));
  CHECK(word_at(table, kPtfxHostPowfOff) == low_word(&pm::powf_));
  CHECK(word_at(table, kPtfxHostExpfOff) == low_word(&pm::expf_));
  CHECK(word_at(table, kPtfxHostSqrtfOff) == low_word(&ptfx_host_sqrtf));
  CHECK(word_at(table, kPtfxHostLogfOff) == low_word(&pm::logf_));
  CHECK(word_at(table, kPtfxHostLog10fOff) == low_word(&pm::log10f_));
  CHECK(word_at(table, kPtfxHostFminfOff) == low_word(&pm::fminf_));
  CHECK(word_at(table, kPtfxHostFmaxfOff) == low_word(&pm::fmaxf_));
  CHECK(word_at(table, kPtfxHostFabsfOff) == low_word(&pm::fabsf_));
  CHECK(word_at(table, kPtfxHostMemcpyOff) == low_word(&ptfx_host_memcpy));
  CHECK(word_at(table, kPtfxHostMemsetOff) == low_word(&ptfx_host_memset));
  CHECK(word_at(table, kPtfxHostRandOff) == low_word(&PtfxHostTable::rand_fn));
  CHECK(word_at(table, kPtfxHostSetExtraSamplesOff) == low_word(&PtfxHostTable::set_extra_samples));
  // The slots are distinct functions, none zero.
  for (std::uint32_t off = kPtfxHostSinfOff; off < kPtfxHostTableBytes; off += 4u) {
    CHECK(word_at(table, off) != 0u);
  }
  // The service functions work: sqrtf, memcpy, memset through the OS's.
  CHECK(ptfx_host_sqrtf(2.0f) == 1.41421356f);
  std::uint8_t dst[8];
  CHECK(ptfx_host_memcpy(dst, "abcd", 5) == dst);
  CHECK(std::memcmp(dst, "abcd", 5) == 0);
  CHECK(ptfx_host_memset(dst, 'x', 8) == dst);
  CHECK(dst[0] == 'x');
  CHECK(dst[7] == 'x');
}

TEST_CASE("ptfx_host_table: rand keeps newlib's range and build reseeds, so a run repeats") {
  alignas(4) std::uint8_t table[kPtfxHostTableBytes];
  PtfxHostTable t(table);
  t.build(44100u);
  std::int32_t first[256];
  for (int i = 0; i < 256; ++i) {
    first[i] = PtfxHostTable::rand_fn();
    CHECK(first[i] >= 0);
    CHECK(first[i] <= 0x7FFFFFFF);
  }
  t.build(44100u);  // the next run: the same seed again (A8)
  for (int i = 0; i < 256; ++i) {
    CHECK(PtfxHostTable::rand_fn() == first[i]);  // reseeded per run: repeatable
  }
  // Not a constant sequence.
  bool varies = false;
  for (int i = 1; i < 256; ++i) {
    varies = varies || first[i] != first[0];
  }
  CHECK(varies);
}

TEST_CASE("ptfx_host_table: the set_extra_samples latch counts only while armed (A10)") {
  alignas(4) std::uint8_t table[kPtfxHostTableBytes];
  PtfxHostTable t(table);
  t.build(44100u);
  CHECK_FALSE(t.armed());
  CHECK(t.latched_prepend() == 0u );
  CHECK(t.latched_append() == 0u);
  // Disarmed (before the init phase / during process_block): the call is ignored.
  PtfxHostTable::set_extra_samples(3u, 4u);
  CHECK(t.latched_prepend() == 0u );
  CHECK(t.latched_append() == 0u);
  // Armed (the init phase and the parameter replay that follows it): the call latches.
  t.arm();
  CHECK(t.armed());
  PtfxHostTable::set_extra_samples(5u, 7u);
  CHECK(t.latched_prepend() == 5u );
  CHECK(t.latched_append() == 7u);
  PtfxHostTable::set_extra_samples(9u, 11u);
  CHECK(t.latched_prepend() == 9u);  // the last call wins while armed
  CHECK(t.latched_append() == 11u);
  // Disarmed again: ignored.
  t.disarm();
  PtfxHostTable::set_extra_samples(1u, 2u);
  CHECK(t.latched_prepend() == 9u );
  CHECK(t.latched_append() == 11u);
  // build() starts a new run: disarmed, latch cleared.
  t.arm();
  t.build(44100u);
  CHECK_FALSE(t.armed());
  CHECK(t.latched_prepend() == 0u );
  CHECK(t.latched_append() == 0u);
  PtfxHostTable::set_extra_samples(6u, 8u);
  CHECK(t.latched_prepend() == 0u );
  CHECK(t.latched_append() == 0u);
}

TEST_CASE("ptfx_host_table: a destroyed table is no longer the active one (no dangling latch), a misaligned buffer is refused") {
  PtfxHostTableStorage store;
  {
    PtfxHostTable t(store.bytes);
    CHECK(t.valid());
    t.build(44100u);
    t.arm();
  }  // t is gone: the entry must not write through a dangling pointer (ASan/UBSan would flag it)
  PtfxHostTable::set_extra_samples(1u, 2u);
  // Another table built after the first died works on its own latch.
  PtfxHostTable t2(store.bytes);
  t2.build(0u);
  t2.arm();
  PtfxHostTable::set_extra_samples(3u, 4u);
  CHECK(t2.latched_prepend() == 3u);
  CHECK(t2.latched_append() == 4u);
  // A misaligned or null buffer: invalid, build() writes nothing.
  alignas(4) std::uint8_t raw[kPtfxHostTableBytes + 4];
  std::memset(raw, 0x5A, sizeof raw);
  PtfxHostTable bad(raw + 1);
  CHECK_FALSE(bad.valid());
  bad.build(44100u);
  CHECK(raw[1] == 0x5Au);
  CHECK(raw[kPtfxHostTableBytes] == 0x5Au);
  PtfxHostTable none(nullptr);
  CHECK_FALSE(none.valid());
  none.build(44100u);  // no crash
}
