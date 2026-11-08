// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S2a: the host table a `.ptfx` plugin is called with (docs/specs/OS-CEPTFX.md 3.4, the field order of 2.1): 68
// bytes built for each run, in the `ptfx` region's meta sub-area on the MK66. The offsets below are OUR constants, written
// from the format facts of 2.1 — never copied from the SDK header — and pinned by static_asserts. Each function slot is
// 4 bytes: on the MK66 (32-bit) a function pointer fits exactly; on the host build the builder stores the pointer's low
// word and the test compares the same truncation.
//
// What fills the slots (3.4): sinf / cosf are core::sin_fast / cos_fast, the rest of the math is core::ptfx_math (no libm),
// sqrtf is VSQRT, memcpy / memset are the OS's, rand is the 64-bit LCG of core::ptfx_math reseeded at every build (A8: a
// run repeats), and set_extra_samples latches (prepend, append) only while the run is in its init phase (A10). The table's
// functions run in the plugin's unprivileged context and touch no peripheral and no shared state but the latch. No heap.
#pragma once

#include <cstddef>
#include <cstdint>

namespace o3os::module {

inline constexpr std::uint32_t kPtfxHostTableBytes = 68;
inline constexpr std::uint32_t kPtfxHostSampleRateOff = 0;    // u32, little endian
inline constexpr std::uint32_t kPtfxHostSinfOff = 4;
inline constexpr std::uint32_t kPtfxHostCosfOff = 8;
inline constexpr std::uint32_t kPtfxHostTanfOff = 12;
inline constexpr std::uint32_t kPtfxHostTanhfOff = 16;
inline constexpr std::uint32_t kPtfxHostPowfOff = 20;
inline constexpr std::uint32_t kPtfxHostExpfOff = 24;
inline constexpr std::uint32_t kPtfxHostSqrtfOff = 28;
inline constexpr std::uint32_t kPtfxHostLogfOff = 32;
inline constexpr std::uint32_t kPtfxHostLog10fOff = 36;
inline constexpr std::uint32_t kPtfxHostFminfOff = 40;
inline constexpr std::uint32_t kPtfxHostFmaxfOff = 44;
inline constexpr std::uint32_t kPtfxHostFabsfOff = 48;
inline constexpr std::uint32_t kPtfxHostMemcpyOff = 52;
inline constexpr std::uint32_t kPtfxHostMemsetOff = 56;
inline constexpr std::uint32_t kPtfxHostRandOff = 60;
inline constexpr std::uint32_t kPtfxHostSetExtraSamplesOff = 64;
static_assert(kPtfxHostTableBytes == 68u, "2.1: the host table is 68 bytes");
static_assert(kPtfxHostSampleRateOff == 0u && kPtfxHostSinfOff == 4u && kPtfxHostCosfOff == 8u && kPtfxHostTanfOff == 12u && kPtfxHostTanhfOff == 16u &&
                  kPtfxHostPowfOff == 20u && kPtfxHostExpfOff == 24u && kPtfxHostSqrtfOff == 28u && kPtfxHostLogfOff == 32u && kPtfxHostLog10fOff == 36u &&
                  kPtfxHostFminfOff == 40u && kPtfxHostFmaxfOff == 44u && kPtfxHostFabsfOff == 48u && kPtfxHostMemcpyOff == 52u && kPtfxHostMemsetOff == 56u &&
                  kPtfxHostRandOff == 60u && kPtfxHostSetExtraSamplesOff == 64u,
              "2.1's field order: sample_rate, then sinf..set_extra_samples, 4 bytes each");
static_assert(kPtfxHostSetExtraSamplesOff + 4u == kPtfxHostTableBytes, "the last slot ends the table");

inline constexpr std::uint32_t kPtfxDefaultSampleRate = 44100u;  // A8: sample_rate 0 means 44,100

// The table's own entries that are not a core function already (their addresses go into the slots).
float ptfx_host_sqrtf(float x) noexcept;                             // VSQRT on the Cortex-M4F
void* ptfx_host_memcpy(void* dst, const void* src, std::size_t n) noexcept;  // the OS's
void* ptfx_host_memset(void* dst, int c, std::size_t n) noexcept;            // the OS's

// The table's storage, 4-byte aligned (the plugin reads it as 17 words). A caller without its own region (the host tests) uses
// this; on the MK66 the meta sub-area's offset is a multiple of 4 (mem_map.h), asserted there.
struct alignas(4) PtfxHostTableStorage {
  std::uint8_t bytes[kPtfxHostTableBytes];
};
static_assert(alignof(PtfxHostTableStorage) == 4u && sizeof(PtfxHostTableStorage) == kPtfxHostTableBytes, "68 bytes, word aligned");

class PtfxHostTable {
 public:
  // `table` is kPtfxHostTableBytes of caller-given memory (the `ptfx` region's meta sub-area on the MK66); it is not owned and
  // must be 4-byte aligned: a null or misaligned pointer makes valid() false and build() a no-op (nothing is written).
  explicit PtfxHostTable(std::uint8_t* table) noexcept;
  // The active table pointer the set_extra_samples entry reads must never dangle: the destructor clears it when it is this one.
  // The object is therefore not copyable or movable (the pointer would name the wrong object).
  ~PtfxHostTable();
  PtfxHostTable(const PtfxHostTable&) = delete;
  PtfxHostTable& operator=(const PtfxHostTable&) = delete;

  bool valid() const noexcept { return table_ != nullptr; }

  // Fills the table: sample_rate (0 means 44,100, A8), the math and service slots above, the rand seed reset (A8) and the
  // latch disarmed. Makes this the active table the set_extra_samples entry reads.
  void build(std::uint32_t sample_rate) noexcept;

  // A10: arm at the start of the init phase (set_extra_samples counts now); disarm after it (the call is ignored).
  void arm() noexcept;
  void disarm() noexcept;
  bool armed() const noexcept { return armed_; }
  std::uint32_t latched_prepend() const noexcept { return prepend_; }
  std::uint32_t latched_append() const noexcept { return append_; }

  // The table's set_extra_samples(prepend, append) entry: latches while armed, ignores the call afterwards (A10).
  static void set_extra_samples(std::uint32_t prepend, std::uint32_t append) noexcept;
  // The table's rand() entry: the 64-bit LCG, range 0..0x7FFFFFFF (newlib's).
  static std::int32_t rand_fn() noexcept;

 private:
  std::uint8_t* table_;
  bool armed_ = false;
  std::uint32_t prepend_ = 0;
  std::uint32_t append_ = 0;
};

}  // namespace o3os::module
