// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FirmwareStore over FatFs (OS-006c). THE ONLY translation unit of the tree that names a FatFs call that writes (f_write, f_sync, f_unlink,
// f_rename, ...): tools/check_layers.py rule 6 fails the build if another source mentions one. Every such call is preceded by the path guard on the full
// path it is about to touch, and every mutating public method by `writable()`: a mounted FAT12/16/32 volume, not write protected, with a Firmware/ folder
// (any such card: no marker file, OS-ANYCARD).
#include <o3os/platform/mk66/update/fat_firmware_store.h>

#include <cstring>

namespace o3os::platform::mk66::update {
namespace {

constexpr std::size_t kFwInfoMax = kImageNameMax + 1;

std::uint32_t saturate(FSIZE_t v) noexcept { return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v); }

void hex4(char* out, std::uint32_t v) noexcept {
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned nibble = (v >> (12u - 4u * i)) & 0xFu;
    out[i] = static_cast<char>(nibble < 10u ? '0' + nibble : 'A' + (nibble - 10u));
  }
}

}  // namespace

Fault FatFirmwareStore::from_fresult(FRESULT r) noexcept {
  if (r != FR_OK) {
    last_fresult_ = static_cast<int>(r);
  }
  switch (r) {
    case FR_OK: return Fault::Ok;
    case FR_NO_FILE:
    case FR_NO_PATH: return Fault::NotFound;
    case FR_EXIST: return Fault::Exists;
    case FR_DENIED: return Fault::NoSpace;  // a full volume or a full directory
    case FR_WRITE_PROTECTED: return Fault::WriteProtected;
    case FR_INVALID_NAME: return Fault::BadName;
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM: return Fault::NoCard;
    case FR_LOCKED:
    case FR_TOO_MANY_OPEN_FILES: return Fault::Busy;
    default: return Fault::Io;  // FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_INVALID_OBJECT, FR_TIMEOUT, ...
  }
}

bool FatFirmwareStore::exists(const char* path, bool* is_dir) noexcept {
  FILINFO fi;
  if (f_stat(path, &fi) != FR_OK) {
    return false;
  }
  if (is_dir != nullptr) {
    *is_dir = (fi.fattrib & AM_DIR) != 0u;
  }
  return true;
}

WriteState FatFirmwareStore::state() noexcept {
  WriteState s;
  s.mounted = fat_.mounted();
  if (!s.mounted) {
    s.why = Fault::NoCard;
    return s;
  }
  const hal::storage::VolumeInfo volume = fat_.volume();
  const char* fs = hal::storage::fs_kind_name(volume.kind);
  for (std::size_t i = 0; i + 1 < sizeof s.fs && fs[i] != '\0'; ++i) {
    s.fs[i] = fs[i];
  }
  const std::uint32_t serial = fat_.volume_serial();
  hex4(s.serial, serial >> 16);
  s.serial[4] = '-';
  hex4(s.serial + 5, serial);
  s.serial[9] = '\0';
  s.exfat = volume.kind == hal::storage::FsKind::ExFat;
  s.write_protected = device_.write_protected();
  bool is_dir = false;
  s.dir = exists(kFirmwareDir, &is_dir) && is_dir;
  const bool fat_kind = volume.kind == hal::storage::FsKind::Fat12 || volume.kind == hal::storage::FsKind::Fat16 || volume.kind == hal::storage::FsKind::Fat32;
  if (!fat_kind) {
    s.why = Fault::Unsupported;
  } else if (s.write_protected) {
    s.why = Fault::WriteProtected;
  } else if (!s.dir) {
    s.why = Fault::NoDir;
  } else {
    s.why = Fault::Ok;
  }
  s.enabled = s.why == Fault::Ok;
  return s;
}

Fault FatFirmwareStore::writable() noexcept { return state().why; }

void FatFirmwareStore::drop_temp() noexcept {
  char path[kPathMax];
  if (build_path(path, sizeof path, kTempName) && classify_write_path(path) == WriteKind::Temp) {
    (void)f_unlink(path);  // absent, or the card is gone: nothing more to do
  }
}

Fault FatFirmwareStore::begin_upload(const char* name, std::uint32_t bytes) noexcept {
  if (uploading_) {
    return Fault::Busy;
  }
  if (!valid_image_name(name)) {
    return Fault::BadName;
  }
  if (bytes == 0u || bytes > kMaxImageFileBytes) {
    return Fault::Unsupported;
  }
  Fault f = writable();
  if (f != Fault::Ok) {
    return f;
  }
  char target[kPathMax];
  char temp[kPathMax];
  if (!build_path(target, sizeof target, name) || !build_path(temp, sizeof temp, kTempName) || classify_write_path(target) != WriteKind::Image ||
      classify_write_path(temp) != WriteKind::Temp) {
    return Fault::BadName;
  }
  if (exists(target, nullptr)) {
    return Fault::Exists;
  }
  drop_temp();  // the leftover of an upload that was cut short
  if (rd_open_) {
    close_read();
  }
  f = from_fresult(f_open(&wr_, temp, FA_CREATE_ALWAYS | FA_WRITE));
  if (f != Fault::Ok) {
    return f;
  }
  wr_open_ = true;
  // Reserve the room now: a seek past the end of a file open for writing allocates the clusters, so a volume that cannot hold the file says so here and
  // not half way through. (f_getfree would scan the whole FAT, which R0.15a does for every call: seconds on a 14 GB card.)
  if (f_lseek(&wr_, bytes) != FR_OK || f_tell(&wr_) != bytes) {
    (void)f_close(&wr_);
    wr_open_ = false;
    drop_temp();
    return Fault::NoSpace;
  }
  if (f_lseek(&wr_, 0) != FR_OK) {
    (void)f_close(&wr_);
    wr_open_ = false;
    drop_temp();
    return Fault::Io;
  }
  std::size_t n = 0;
  while (name[n] != '\0' && n < kImageNameMax) {
    target_[n] = name[n];
    ++n;
  }
  target_[n] = '\0';
  declared_ = bytes;
  upload_generation_ = fat_.generation();
  uploading_ = true;
  return Fault::Ok;
}

Fault FatFirmwareStore::append(const std::uint8_t* data, std::size_t n) noexcept {
  if (!uploading_ || !wr_open_) {
    return Fault::Io;
  }
  if (!same_mount()) {
    return Fault::NoCard;  // OS-SDSWAP: the card was taken out (and maybe another put in): the upload ends here, nothing more is written
  }
  UINT written = 0;
  const FRESULT r = f_write(&wr_, data, static_cast<UINT>(n), &written);
  if (r != FR_OK) {
    return from_fresult(r);
  }
  return written == n ? Fault::Ok : Fault::NoSpace;
}

Fault FatFirmwareStore::close_upload() noexcept {
  if (!uploading_ || !wr_open_) {
    return Fault::Io;
  }
  if (!same_mount()) {
    return Fault::NoCard;
  }
  const FRESULT synced = f_sync(&wr_);
  const FRESULT closed = f_close(&wr_);
  wr_open_ = false;
  return from_fresult(synced != FR_OK ? synced : closed);
}

Fault FatFirmwareStore::commit_upload() noexcept {
  if (!uploading_ || wr_open_) {
    return Fault::Io;
  }
  if (!same_mount()) {
    return Fault::NoCard;
  }
  char target[kPathMax];
  char temp[kPathMax];
  if (!build_path(target, sizeof target, target_) || !build_path(temp, sizeof temp, kTempName) || classify_write_path(target) != WriteKind::Image ||
      classify_write_path(temp) != WriteKind::Temp) {
    return Fault::BadName;
  }
  const Fault f = from_fresult(f_rename(temp, target));  // FR_EXIST when the target appeared: nothing is replaced
  if (f == Fault::Ok) {
    uploading_ = false;
    target_[0] = '\0';
  }
  return f;
}

void FatFirmwareStore::abort_upload() noexcept {
  if (wr_open_) {
    (void)f_close(&wr_);
    wr_open_ = false;
  }
  if (uploading_ && same_mount()) {  // (on another mount the temporary file is the old card's, or gone: the new card is not touched)
    drop_temp();
  }
  uploading_ = false;
  target_[0] = '\0';
}

Fault FatFirmwareStore::open_read(const char* name, std::uint32_t& size) noexcept {
  if (!readable_name(name)) {
    return Fault::BadName;
  }
  if (!fat_.mounted()) {
    return Fault::NoCard;
  }
  if (rd_open_) {
    close_read();
  }
  char path[kPathMax];
  if (!build_path(path, sizeof path, name)) {
    return Fault::BadName;
  }
  const Fault f = from_fresult(f_open(&rd_, path, FA_READ));
  if (f != Fault::Ok) {
    return f;
  }
  rd_open_ = true;
  size = saturate(f_size(&rd_));
  return Fault::Ok;
}

Fault FatFirmwareStore::read(std::uint8_t* dst, std::size_t max, std::size_t& got) noexcept {
  if (!rd_open_) {
    return Fault::Io;
  }
  UINT n = 0;
  const FRESULT r = f_read(&rd_, dst, static_cast<UINT>(max), &n);
  if (r != FR_OK) {
    return from_fresult(r);
  }
  got = n;
  return Fault::Ok;
}

void FatFirmwareStore::close_read() noexcept {
  if (rd_open_) {
    (void)f_close(&rd_);  // opened FA_READ: nothing to flush
    rd_open_ = false;
  }
}

unsigned FatFirmwareStore::count_ptf() noexcept {
  unsigned n = 0;
  FwEntry e;
  if (list_begin() == Fault::Ok) {
    while (list_next(e)) {
      n += is_ptf_name(e.name) ? 1u : 0u;
    }
  }
  list_end();
  return n;
}

Fault FatFirmwareStore::remove(const char* name) noexcept {
  if (!removable_name(name)) {
    return Fault::BadName;
  }
  Fault f = writable();
  if (f != Fault::Ok) {
    return f;
  }
  char path[kPathMax];
  if (!build_path(path, sizeof path, name)) {
    return Fault::BadName;
  }
  bool is_dir = false;
  if (!exists(path, &is_dir) || is_dir) {
    return Fault::NotFound;
  }
  if (uploading_ && std::strcmp(name, kTempName) == 0) {
    return Fault::Busy;
  }
  if (is_ptf_name(name) && count_ptf() < 2u) {
    return Fault::LastPtf;
  }
  const WriteKind kind = classify_write_path(path);
  if (kind != WriteKind::Temp && kind != WriteKind::Image) {
    return Fault::BadName;
  }
  return from_fresult(f_unlink(path));
}

Fault FatFirmwareStore::write_fwinfo(const char* name) noexcept {
  if (uploading_ || wr_open_) {
    return Fault::Busy;
  }
  if (!valid_image_name(name)) {
    return Fault::BadName;
  }
  if (!update_name_ok(name)) {
    return Fault::NameLong;  // the last guard before the hand-over: the bootloader would flash another file for this name (OS-BUG-FWNAMELEN)
  }
  Fault f = writable();
  if (f != Fault::Ok) {
    return f;
  }
  char image[kPathMax];
  char info[kPathMax];
  if (!build_path(image, sizeof image, name) || !build_path(info, sizeof info, kFwInfoName) || classify_write_path(info) != WriteKind::FwInfo) {
    return Fault::BadName;
  }
  bool is_dir = false;
  if (!exists(image, &is_dir) || is_dir) {
    return Fault::NotFound;
  }
  const UINT length = static_cast<UINT>(std::strlen(name));
  f = from_fresult(f_open(&wr_, info, FA_CREATE_NEW | FA_WRITE));  // Exists if a stale one is there: the caller deletes it first
  if (f != Fault::Ok) {
    return f;
  }
  UINT written = 0;
  FRESULT r = f_write(&wr_, name, length, &written);  // exactly the bare name: no folder, no newline, no NUL
  if (r == FR_OK) {
    r = f_sync(&wr_);
  }
  const FRESULT closed = f_close(&wr_);
  if (r == FR_OK) {
    r = closed;
  }
  f = from_fresult(r);
  if (f == Fault::Ok && written != length) {
    f = Fault::NoSpace;
  }
  if (f == Fault::Ok) {  // read it back from the card and compare
    std::uint32_t size = 0;
    close_read();
    char path_name[kFwInfoMax + 1];
    std::size_t got = 0;
    r = f_open(&rd_, info, FA_READ);
    if (r == FR_OK) {
      rd_open_ = true;
      size = saturate(f_size(&rd_));
      UINT n = 0;
      r = f_read(&rd_, path_name, sizeof path_name, &n);
      got = n;
      close_read();
    }
    f = from_fresult(r);
    if (f == Fault::Ok && (size != length || got != length || std::memcmp(path_name, name, length) != 0)) {
      f = Fault::Readback;
    }
  }
  if (f != Fault::Ok) {
    (void)f_unlink(info);  // a hand-over file that is not exactly right must not stay
  }
  return f;
}

Fault FatFirmwareStore::remove_fwinfo() noexcept {
  char info[kPathMax];
  if (!build_path(info, sizeof info, kFwInfoName) || classify_write_path(info) != WriteKind::FwInfo) {
    return Fault::BadName;
  }
  if (!fat_.mounted()) {
    return Fault::NoCard;
  }
  const FRESULT r = f_unlink(info);
  return (r == FR_OK || r == FR_NO_FILE) ? Fault::Ok : from_fresult(r);
}

Fault FatFirmwareStore::list_begin() noexcept {
  if (dir_open_) {
    list_end();
  }
  if (!fat_.mounted()) {
    return Fault::NoCard;
  }
  const Fault f = from_fresult(f_opendir(&dir_, kFirmwareDir));
  if (f == Fault::Ok) {
    dir_open_ = true;
    entries_ = 0;
  }
  return f;
}

bool FatFirmwareStore::list_next(FwEntry& out) noexcept {
  constexpr std::uint32_t kMaxEntries = 4096;  // a bound on a listing of a folder that never ends
  while (dir_open_ && entries_ < kMaxEntries) {
    ++entries_;
    if (f_readdir(&dir_, &info_) != FR_OK || info_.fname[0] == '\0') {
      return false;
    }
    if ((info_.fattrib & AM_DIR) != 0u || std::strcmp(info_.fname, ".") == 0 || std::strcmp(info_.fname, "..") == 0) {
      continue;
    }
    std::size_t n = 0;
    while (info_.fname[n] != '\0' && n < kImageNameMax) {
      out.name[n] = info_.fname[n];
      ++n;
    }
    out.name[n] = '\0';
    out.size = saturate(info_.fsize);
    return true;
  }
  return false;
}

void FatFirmwareStore::list_end() noexcept {
  if (dir_open_) {
    (void)f_closedir(&dir_);
    dir_open_ = false;
  }
}

}  // namespace o3os::platform::mk66::update
