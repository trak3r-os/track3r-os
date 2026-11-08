// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/module/ptfx_loader.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::module {
namespace {

using SStatus = hal::storage::Status;  // the card's answer (PtfxLoader::Status is the load's own verdict)

core::JobStatus job_status_of(PtfxLoader::State s) noexcept {
  switch (s) {
    case PtfxLoader::State::Done:
      return core::JobStatus::Done;
    case PtfxLoader::State::Running:
      return core::JobStatus::Running;
    default:
      return core::JobStatus::Failed;
  }
}

}  // namespace

PtfxLoader::Begin PtfxLoader::begin(hal::storage::Storage& storage, hal::storage::FileHandle file, const PtfxInfo& info, void* image,
                                    std::uint32_t image_bytes, SyncFn sync) noexcept {
  if (state_ == State::Running) {
    return Begin::Busy;
  }
  if (info.status != PtfxStatus::Ok) {
    return Begin::BadHeader;  // the parser refused this file: never loaded
  }
  // Not trusting the parser alone (a hand-built or stale PtfxInfo must not reach the copy): the sizes in 64 bits, so that
  // ptfx_align4 cannot wrap, and every entry inside the payload (the offsets have bit 0 masked already; the mask is applied
  // again here). Offset 0 is a valid entry (the payload's first function), never "absent": the parser demands all three.
  const std::uint64_t total64 = ((static_cast<std::uint64_t>(info.code_size) + 3u) & ~std::uint64_t{3}) + info.bss_size;
  if (image == nullptr || info.code_size == 0u || total64 > image_bytes || total64 > kPtfxMaxCode) {
    return Begin::BadImage;
  }
  const std::uint32_t entry_offsets[3] = {info.init_offset, info.set_param_offset, info.process_block_offset};
  for (const std::uint32_t off : entry_offsets) {
    const std::uint32_t o = off & ~1u;
    if (o >= info.code_size) {
      return Begin::BadImage;
    }
  }
  const std::uint32_t total = static_cast<std::uint32_t>(total64);
  storage_ = &storage;
  file_ = file;  // ownership taken: closed at the end, on a failure and on cancel
  sync_ = sync;
  code_offset_ = info.code_offset;
  code_size_ = info.code_size;
  bss_size_ = info.bss_size;
  init_offset_ = info.init_offset;
  set_param_offset_ = info.set_param_offset;
  process_block_offset_ = info.process_block_offset;
  image_ = static_cast<std::uint8_t*>(image);
  total_ = total;
  pos_ = 0;
  tail_ = 0;
  status_ = Status::Ok;
  card_status_ = hal::storage::Status::Ok;
  entries_ = Entries{};
  phase_ = Phase::Seek;
  state_ = State::Running;
  return Begin::Started;
}

void PtfxLoader::fail(Status s, hal::storage::Status card) noexcept {
  storage_->close(file_);
  status_ = s;
  card_status_ = card;
  entries_ = Entries{};  // never half-loaded entries
  phase_ = Phase::Seek;
  state_ = State::Failed;
}

void PtfxLoader::finish() noexcept {
  storage_->close(file_);
  status_ = Status::Ok;
  card_status_ = hal::storage::Status::Ok;
  state_ = State::Done;
}

void PtfxLoader::cancel() noexcept {
  if (state_ == State::Running) {
    storage_->close(file_);
    entries_ = Entries{};
    phase_ = Phase::Seek;
    state_ = State::Idle;
  }
}

core::JobStatus PtfxLoader::step(std::uint32_t /*budget_us*/) noexcept {
  if (state_ != State::Running) {
    return job_status_of(state_);
  }
  switch (phase_) {
    case Phase::Seek: {
      const SStatus s = storage_->seek(file_, code_offset_);
      if (s != SStatus::Ok) {
        fail(Status::CardError, s);
        break;
      }
      phase_ = Phase::Copy;
      break;
    }
    case Phase::Copy: {
      const std::uint32_t left = code_size_ - pos_;
      const std::size_t want = left < kChunk ? left : kChunk;
      std::size_t got = 0;
      const SStatus s = storage_->read(file_, image_ + pos_, want, got);
      if (s != SStatus::Ok) {
        fail(Status::CardError, s);  // a card error aborts the load with its status
        break;
      }
      if (got == 0u) {
        fail(Status::ShortFile, SStatus::Ok);  // the end of the file inside the payload: the file is damaged
        break;
      }
      if (got > want) {
        got = want;  // a backend that over-reports must not push pos_ past code_size_
      }
      pos_ += static_cast<std::uint32_t>(got);
      if (pos_ == code_size_) {
        phase_ = total_ > code_size_ ? Phase::Tail : Phase::Sync;
      }
      break;
    }
    case Phase::Tail: {
      const std::uint32_t left = total_ - code_size_ - tail_;
      const std::uint32_t want = left < kChunk ? left : kChunk;
      std::memset(image_ + code_size_ + tail_, 0, want);  // the tail: align4's 1-3 byte gap and .bss
      tail_ += want;
      if (code_size_ + tail_ == total_) {
        phase_ = Phase::Sync;
      }
      break;
    }
    case Phase::Sync: {
      if (sync_ != nullptr) {
        sync_(image_, total_);  // 3.2 step 3: over the image, the zeroed tail included
      }
      const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(image_);
      entries_.init = (base + init_offset_) | 1u;  // (offset 0 is the first function of the payload: the SDK plugins put init there)
      entries_.set_param = (base + set_param_offset_) | 1u;
      entries_.process_block = (base + process_block_offset_) | 1u;
      finish();
      break;
    }
  }
  return job_status_of(state_);
}

unsigned PtfxLoader::progress() const noexcept {
  return state_ == State::Done ? core::kJobProgressMax : core::permille_of(pos_ + tail_, total_);
}

const char* PtfxLoader::detail() const noexcept {
  if (state_ != State::Failed) {
    return "";
  }
  return status_ == Status::CardError ? hal::storage::status_name(card_status_) : "SHORT FILE";
}

void ptfx_paint_guard(void* guard, std::uint32_t bytes) noexcept { std::memset(guard, kPtfxGuardByte, bytes); }

bool ptfx_check_guard(const void* guard, std::uint32_t bytes) noexcept {
  // The guard is word-aligned (kPtfxGuardOff of the ptfx region): compare 32-bit words, memcpy keeps it alias-clean (one LDR).
  constexpr std::uint32_t kWord = 0x01010101u * kPtfxGuardByte;
  const std::uint8_t* p = static_cast<const std::uint8_t*>(guard);
  for (std::uint32_t i = 0; i + 4u <= bytes; i += 4u) {  // `bytes` is a whole number of words (4 KiB)
    std::uint32_t w;
    std::memcpy(&w, p + i, sizeof w);
    if (w != kWord) {
      return false;
    }
  }
  return true;
}

}  // namespace o3os::module
