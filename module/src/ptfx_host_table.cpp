// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/module/ptfx_host_table.h>

#include <o3os/core/fast_math.h>
#include <o3os/core/ptfx_math.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::module {
namespace {

// The one run whose latch the table's set_extra_samples entry reads (one plugin call runs at a time; the main loop is the
// only writer before the call). A single pointer: the latch itself lives in the caller-given table object.
PtfxHostTable* g_active = nullptr;

void put32(std::uint8_t* p, std::uint32_t v) noexcept {  // little endian, explicit
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

// A function slot is 4 bytes: exact on the 32-bit target; on the host the low word is stored (the test compares the same
// truncation).
template <typename Fn>
void put_fn(std::uint8_t* table, std::uint32_t off, Fn fn) noexcept {
  put32(table + off, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(fn)));
}

}  // namespace

// VSQRT.F32 on the Cortex-M4F. This TU is built -fno-math-errno (module/CMakeLists.txt), so __builtin_sqrtf is the one
// instruction and never a call to libm's sqrtf (which would set errno, a newlib object the plugin context must not reach).
float ptfx_host_sqrtf(float x) noexcept { return __builtin_sqrtf(x); }

void* ptfx_host_memcpy(void* dst, const void* src, std::size_t n) noexcept { return std::memcpy(dst, src, n); }

void* ptfx_host_memset(void* dst, int c, std::size_t n) noexcept { return std::memset(dst, c, n); }

PtfxHostTable::PtfxHostTable(std::uint8_t* table) noexcept
    : table_((reinterpret_cast<std::uintptr_t>(table) & 3u) == 0u ? table : nullptr) {}  // null and misaligned both refused

PtfxHostTable::~PtfxHostTable() {
  if (g_active == this) {
    g_active = nullptr;
  }
}

void PtfxHostTable::build(std::uint32_t sample_rate) noexcept {
  if (table_ == nullptr) {
    return;
  }
  std::memset(table_, 0, kPtfxHostTableBytes);
  put32(table_ + kPtfxHostSampleRateOff, sample_rate == 0u ? kPtfxDefaultSampleRate : sample_rate);
  put_fn(table_, kPtfxHostSinfOff, &core::sin_fast);
  put_fn(table_, kPtfxHostCosfOff, &core::cos_fast);
  put_fn(table_, kPtfxHostTanfOff, &core::ptfx_math::tanf_);
  put_fn(table_, kPtfxHostTanhfOff, &core::ptfx_math::tanhf_);
  put_fn(table_, kPtfxHostPowfOff, &core::ptfx_math::powf_);
  put_fn(table_, kPtfxHostExpfOff, &core::ptfx_math::expf_);
  put_fn(table_, kPtfxHostSqrtfOff, &ptfx_host_sqrtf);
  put_fn(table_, kPtfxHostLogfOff, &core::ptfx_math::logf_);
  put_fn(table_, kPtfxHostLog10fOff, &core::ptfx_math::log10f_);
  put_fn(table_, kPtfxHostFminfOff, &core::ptfx_math::fminf_);
  put_fn(table_, kPtfxHostFmaxfOff, &core::ptfx_math::fmaxf_);
  put_fn(table_, kPtfxHostFabsfOff, &core::ptfx_math::fabsf_);
  put_fn(table_, kPtfxHostMemcpyOff, &ptfx_host_memcpy);
  put_fn(table_, kPtfxHostMemsetOff, &ptfx_host_memset);
  put_fn(table_, kPtfxHostRandOff, &PtfxHostTable::rand_fn);
  put_fn(table_, kPtfxHostSetExtraSamplesOff, &PtfxHostTable::set_extra_samples);
  core::ptfx_math::rand_seed();  // A8: reseeded at the start of every run, so a run is repeatable
  armed_ = false;
  prepend_ = 0;
  append_ = 0;
  g_active = this;
}

void PtfxHostTable::arm() noexcept { armed_ = true; }

void PtfxHostTable::disarm() noexcept { armed_ = false; }

void PtfxHostTable::set_extra_samples(std::uint32_t prepend, std::uint32_t append) noexcept {
  if (g_active != nullptr && g_active->armed_) {  // A10: counts only in the init phase
    // The values are latched as given: S3 clamps them to the edit pool when it reads the latch (not here, by design).
    g_active->prepend_ = prepend;
    g_active->append_ = append;
  }
}

std::int32_t PtfxHostTable::rand_fn() noexcept { return core::ptfx_math::rand_(); }

}  // namespace o3os::module
