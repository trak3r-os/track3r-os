// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-FLASHDIET P5/P8: the text pools of text.h for the host and QEMU tests. The image serves them from text/src/text.cpp (SDRAM, an image-only unit); the tests inflate the same
// generated blobs into buffers of their own, in mk66_text_test.cpp, which also defines the text_inflater() the pools need.

#include <cstdint>

namespace o3os::platform::mk66::testing {

// The text of an id of the console pool (txt::<key>) / the bench pool (btxt::<key>); nullptr when the blob does not decode or the id is out of range.
const char* console_text(std::uint16_t id);
const char* bench_pool_text(std::uint16_t id);

}  // namespace o3os::platform::mk66::testing
