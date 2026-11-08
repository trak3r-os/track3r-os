// SPDX-License-Identifier: GPL-3.0-or-later

// OS-FLASHDIET P5: the one decode of a text pool (text.h). Pure, so the host and QEMU tests run it on the generated blobs.

#include <o3os/platform/mk66/text.h>

namespace o3os::platform::mk66 {

bool text_unpack(core::Inflater& inf, const std::uint8_t* z, std::size_t z_size, char* dst, std::size_t raw) noexcept {
  inf.begin(z, z_size, raw);
  const std::size_t got = inf.read(reinterpret_cast<std::uint8_t*>(dst), raw);
  return got == raw && inf.done() && !inf.failed();
}

}  // namespace o3os::platform::mk66
