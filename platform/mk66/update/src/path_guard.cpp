// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/platform/mk66/update/path_guard.h>

namespace o3os::platform::mk66::update {
namespace {

std::size_t length(const char* s) noexcept {
  std::size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

bool starts_with(const char* s, const char* prefix) noexcept {
  while (*prefix != '\0') {
    if (*s++ != *prefix++) {
      return false;
    }
  }
  return true;
}

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool starts_with_nocase(const char* s, const char* prefix) noexcept {
  while (*prefix != '\0') {
    if (lower(*s++) != lower(*prefix++)) {
      return false;
    }
  }
  return true;
}

bool equals(const char* a, const char* b) noexcept { return length(a) == length(b) && starts_with(a, b); }

bool alnum_char(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

bool body_char(char c) noexcept {
  return alnum_char(c) || c == '.' || c == '_' || c == '-';
}

}  // namespace

bool valid_image_name(const char* name) noexcept {
  if (name == nullptr || !is_ptf_name(name)) {
    return false;
  }
  const std::size_t n = length(name);
  const std::size_t stem_len = n - kSuffixLen;
  if (stem_len < 1 || stem_len > kStemMax) {
    return false;
  }
  if (!alnum_char(name[0])) {
    return false;  // also refuses a leading dot
  }
  for (std::size_t i = 0; i < stem_len; ++i) {
    if (!body_char(name[i]) || (name[i] == '.' && name[i + 1] == '.')) {
      return false;  // a character outside the class, or ".." (the last stem character meets the '.' of the suffix)
    }
  }
  return true;
}

bool update_name_ok(const char* name) noexcept { return valid_image_name(name) && length(name) <= kUpdateNameMax; }

bool removable_name(const char* name) noexcept {
  if (name == nullptr) {
    return false;
  }
  return equals(name, kTempName) ||
         (valid_image_name(name) &&
          (!starts_with_nocase(name, kOfficialPrefix) ||
           (starts_with_nocase(name, kDevPrefix) && length(name) >= sizeof(kDevPrefix) - 1 + 1 + kSuffixLen)));  // "PolyendTracker_0." and a character more
}

bool readable_name(const char* name) noexcept { return name != nullptr && (equals(name, kTempName) || valid_image_name(name)); }

bool is_ptf_name(const char* name) noexcept {
  const std::size_t n = length(name);
  if (n < kSuffixLen) {
    return false;
  }
  for (std::size_t i = 0; i < kSuffixLen; ++i) {
    if (lower(name[n - kSuffixLen + i]) != kImageSuffix[i]) {
      return false;
    }
  }
  return true;
}

bool build_path(char* out, std::size_t cap, const char* name) noexcept {
  const std::size_t dir = sizeof(kFirmwareDir) - 1;
  const std::size_t n = length(name);
  if (out == nullptr || cap < dir + 1 + n + 1) {
    return false;
  }
  for (std::size_t i = 0; i < dir; ++i) {
    out[i] = kFirmwareDir[i];
  }
  out[dir] = '/';
  for (std::size_t i = 0; i <= n; ++i) {  // with the NUL
    out[dir + 1 + i] = name[i];
  }
  return true;
}

WriteKind classify_write_path(const char* path) noexcept {
  if (path == nullptr || !starts_with(path, kFirmwareDir) || path[sizeof(kFirmwareDir) - 1] != '/') {
    return WriteKind::None;
  }
  const char* name = path + sizeof(kFirmwareDir);
  if (equals(name, kTempName)) {
    return WriteKind::Temp;
  }
  if (equals(name, kFwInfoName)) {
    return WriteKind::FwInfo;
  }
  return valid_image_name(name) ? WriteKind::Image : WriteKind::None;
}

}  // namespace o3os::platform::mk66::update
