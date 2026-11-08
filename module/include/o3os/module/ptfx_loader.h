// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S2a: the stepped loader of a `.ptfx` payload (docs/specs/OS-CEPTFX.md 3.2, the four load steps): it copies
// code_size bytes from an open file into a caller-given image buffer, 4 KiB a step on the main loop, zeroes the tail up to
// align4(code_size) + bss_size (which also zeroes the 1-3 byte gap tracker-ce's zeroing misses, 2.1), calls the platform's
// code-sync hook over the image once (the module loader's SyncFn: DSB, the LMEM line invalidate over the range, ISB), and
// yields the entry addresses base + offset | 1 (Thumb). Offset 0 is a valid entry (the SDK plugins put init first); the parser guarantees all three exist. A card error or
// a short read aborts the load with a status and leaves every entry null: nothing half-loaded is ever handed out. No plugin
// code is loaded outside an explicit begin(), and none is executed here.
//
// The buffer and the file are injected: on the MK66 the image buffer is the `ptfx` region's image sub-area (mem_map.h
// kPtfxImageOff, taken through core_view()) and the file is a card file the caller opened and whose 472-byte header it
// parsed with ptfx_parse_header; in the host test both are a plain array and an in-memory card. A begin() that returns Started
// takes ownership of the file handle and closes it when the load ends, fails or is cancelled; a refused begin() (any other
// answer) leaves the file open and with the caller. No heap, main loop only, one phase action a step.
#pragma once

#include <o3os/core/job.h>
#include <o3os/hal/storage.h>
#include <o3os/module/loader.h>  // ModuleLoader::SyncFn
#include <o3os/module/ptfx_format.h>

#include <cstdint>

namespace o3os::module {

constexpr std::uint32_t ptfx_align4(std::uint32_t n) noexcept { return (n + 3u) & ~3u; }

class PtfxLoader final : public core::Job {
 public:
  // What the platform does after bytes that will be executed were written (the module loader's type): barriers and the
  // code cache's invalidate over [begin, begin + bytes). May be null.
  using SyncFn = ModuleLoader::SyncFn;

  static constexpr std::uint32_t kChunk = 4096u;  // one step copies (or zeroes) at most this many bytes: 3.2's 4 KiB steps

  enum class State : std::uint8_t { Idle, Running, Done, Failed };
  // Why a load failed. CardError: card_status() says which; ShortFile: the file ended inside the payload.
  enum class Status : std::uint8_t { Ok, CardError, ShortFile };

  // The entry addresses of a Done load: image base + offset | 1. Offset 0 is a real entry (init first). Valid only
  // when state() is Done: a failed or cancelled load leaves them all 0.
  struct Entries {
    std::uintptr_t init = 0;
    std::uintptr_t set_param = 0;
    std::uintptr_t process_block = 0;
  };

  enum class Begin : std::uint8_t {
    Started,
    Busy,       // a load is running
    BadHeader,  // info.status is not Ok: the parser refused this file
    BadImage,   // the image buffer cannot hold align4(code_size) + bss_size, or the header's sizes / entries are out of range
  };

  // Starts loading `info` (a parsed header with status Ok) from the already open `file` into `image` (`image_bytes` bytes),
  // reading through `storage`. Takes ownership of `file` only when it returns Started; every refusal leaves it with the caller.
  // The header is re-checked here, not trusted to the parser alone: code_size > 0, every non-zero entry offset inside
  // code_size, and align4(code_size) + bss_size (64-bit) within image_bytes and kPtfxMaxCode, else BadImage.
  Begin begin(hal::storage::Storage& storage, hal::storage::FileHandle file, const PtfxInfo& info, void* image,
              std::uint32_t image_bytes, SyncFn sync) noexcept;

  core::JobStatus step(std::uint32_t budget_us) noexcept override;
  // The payload and tail bytes done against align4(code_size) + bss_size; kJobProgressMax when Done.
  unsigned progress() const noexcept override;
  void cancel() noexcept override;
  const char* name() const noexcept override { return "PTFX LOAD"; }
  const char* detail() const noexcept override;

  State state() const noexcept { return state_; }
  Status status() const noexcept { return status_; }
  hal::storage::Status card_status() const noexcept { return card_status_; }
  const Entries& entries() const noexcept { return entries_; }
  std::uint32_t total_bytes() const noexcept { return total_; }  // align4(code_size) + bss_size

 private:
  enum class Phase : std::uint8_t { Seek, Copy, Tail, Sync };
  void fail(Status s, hal::storage::Status card) noexcept;
  void finish() noexcept;

  hal::storage::Storage* storage_ = nullptr;
  hal::storage::FileHandle file_{};
  SyncFn sync_ = nullptr;
  std::uint32_t code_offset_ = 0, code_size_ = 0, bss_size_ = 0;
  std::uint32_t init_offset_ = 0, set_param_offset_ = 0, process_block_offset_ = 0;
  std::uint8_t* image_ = nullptr;
  std::uint32_t total_ = 0;  // align4(code_size) + bss_size: the image plus its zeroed tail
  std::uint32_t pos_ = 0;    // payload bytes copied so far
  std::uint32_t tail_ = 0;   // tail bytes zeroed so far (from code_size to total_)
  State state_ = State::Idle;
  Status status_ = Status::Ok;
  hal::storage::Status card_status_ = hal::storage::Status::Ok;
  Phase phase_ = Phase::Seek;
  Entries entries_{};
};

// The 4 KiB stack guard (3.2, kPtfxGuardOff of the `ptfx` region): painted before a run, checked after every call.
inline constexpr std::uint8_t kPtfxGuardByte = 0xA5u;
void ptfx_paint_guard(void* guard, std::uint32_t bytes) noexcept;
// True when every word of the guard still holds the pattern (no call ran into it). The guard is word-aligned and a whole
// number of words (4 KiB): `bytes` must be a multiple of 4 (a trailing partial word is not looked at).
bool ptfx_check_guard(const void* guard, std::uint32_t bytes) noexcept;

}  // namespace o3os::module
