// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The path guard of the firmware writer (OS-006c): the only names the writer may create, rename to, read or remove, as pure functions the host tests
// walk exhaustively. The FatFs-backed store (fat_firmware_store.cpp) applies `classify_write_path` to the full path right before every call that
// writes, so a bug above it cannot reach another file, and the transfer applies `valid_image_name` before it creates anything.
//
//   Firmware/_o3up.tmp                  the upload while it is being written (never a .ptf: the bootloader looks for .ptf files)
//   Firmware/_fwinfo                    the hand-over file: exactly the bare name of the image
//   Firmware/<stem>.ptf                 a finished image: <stem> is [0-9A-Za-z][0-9A-Za-z._-]{0,47}, the whole name without "..", the suffix in any case
//
// Any such name is allowed (maintainer decision: a candidate is better NOT named like an official file, PolyendTracker_<version>.ptf). Removal is narrower: the temporary file and any valid image
// name EXCEPT one that looks official: starting (any case) with "Polyend" unless it starts with the old dev namespace "PolyendTracker_0.". The
// writer never deletes a name that could be an official image. FAT is case-insensitive, so a name that differs from an existing one only in case
// is the same file (the store's existence checks go through FatFs); the guard's prefix test for removal is case-insensitive for the same reason.

#include <o3os/hal/firmware.h>

#include <cstddef>

namespace o3os::platform::mk66::update {

inline constexpr char kFirmwareDir[] = "Firmware";
inline constexpr char kTempName[] = "_o3up.tmp";
inline constexpr char kFwInfoName[] = "_fwinfo";
inline constexpr char kOfficialPrefix[] = "Polyend";         // names starting like this (any case) are never removed ...
inline constexpr char kDevPrefix[] = "PolyendTracker_0.";     // ... except the dev namespace (0.x versions, assumed never published)
inline constexpr char kImageSuffix[] = ".ptf";
inline constexpr std::size_t kSuffixLen = sizeof(kImageSuffix) - 1;  // 4
inline constexpr std::size_t kStemMax = 48;
inline constexpr std::size_t kImageNameMax = kStemMax + kSuffixLen;  // 52
inline constexpr std::size_t kPathMax = sizeof(kFirmwareDir) + 1 + kImageNameMax;  // "Firmware/" + the longest name + NUL
// OS-BUG-FWNAMELEN: the longest name, ".ptf" included, that an update may be armed for (one constant, hal/firmware.h: 30, OBSERVED on the unit; the dialog uses it too). A valid image name may be longer
// (it is stored, listed and read); `update_name_ok` is what write_fwinfo, `fw arm` and the screen's check ask before anything is armed.
inline constexpr std::size_t kUpdateNameMax = hal::firmware::kUpdateNameMax;
static_assert(kUpdateNameMax < kImageNameMax, "the update limit is inside the stored names");

enum class WriteKind : unsigned char { None, Temp, FwInfo, Image };

// A bare image name a finished upload may get: <stem>.ptf (see the top of this file).
bool valid_image_name(const char* name) noexcept;
// A valid image name that an update may be armed for: at most kUpdateNameMax characters in all (a longer one makes the SD bootloader flash the first .ptf of Firmware/, OS-BUG-FWNAMELEN).
bool update_name_ok(const char* name) noexcept;
// A bare name `remove` may delete: the temporary file, or a valid image name that does not look official (see the top of this file).
bool removable_name(const char* name) noexcept;
// A bare name the store may read: a valid image name or the temporary file.
bool readable_name(const char* name) noexcept;
// True for a name that ends in ".ptf" in any case (what the bootloader's reset button may pick up; listing only, not a write permission).
bool is_ptf_name(const char* name) noexcept;
// "Firmware/<name>" into `out`; false when it does not fit (the name is not checked here).
bool build_path(char* out, std::size_t cap, const char* name) noexcept;
// The last guard: a full path ("Firmware/<name>", no leading slash) that the writer may create, rename or write: exactly the three forms above.
WriteKind classify_write_path(const char* path) noexcept;

}  // namespace o3os::platform::mk66::update
