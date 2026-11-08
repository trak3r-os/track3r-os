// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-FLASHDIET P5/P8: the platform's long texts (the console's help and replies, the bench's usage and kernel helps) are one deflate blob per pool in flash
// instead of a literal at every use (text/console_text.txt and text/bench_text.txt, packed by tools/text_pack.py at build time). A text is named by an id,
// the byte offset of its first character in the inflated pool (the generated console_text_ids.h / bench_text_ids.h: `txt::<key>`, `btxt::<key>`).
//
// The image inflates a pool once, on the first use, into a buffer in the SDRAM slack window (text/src/text.cpp); `text(id)` is then one addition. MAIN LOOP ONLY:
// the first call decodes through the shared inflater (text_inflater(), the UI assets' one decoder, synchronously in one call), so never from an ISR, the render,
// a fault or crash handler or before the SDRAM is up. Texts those paths print stay literals. A pool that fails to decode answers every id with a short fixed text.

#include <o3os/core/inflate.h>

#include <cstddef>
#include <cstdint>

namespace o3os::platform::mk66 {

// The deflate streams (generated, text blobs in flash): the console pool and the bench pool.
extern const std::uint8_t* const kConsoleTextZ;
extern const std::size_t kConsoleTextZSize;
extern const std::uint8_t* const kBenchTextZ;
extern const std::size_t kBenchTextZSize;

// Inflate one pool: dst receives exactly `raw` bytes. True when the stream held exactly that many and was sound.
bool text_unpack(core::Inflater& inf, const std::uint8_t* z, std::size_t z_size, char* dst, std::size_t raw) noexcept;

// The decoder the pools share: the composition root (platform/mk66/src/app_main.cpp) answers with the UI assets' one core::Inflater in SDRAM.
core::Inflater& text_inflater() noexcept;

// The text of an id (txt::<key> in console_text_ids.h): a NUL-terminated string in the SDRAM pool, valid for ever. Image only (text/src/text.cpp).
const char* text(std::uint16_t id) noexcept;
// The same for the bench pool (btxt::<key> in bench_text_ids.h); only a bench image (O3OS_BENCH) has it.
const char* bench_text(std::uint16_t id) noexcept;

}  // namespace o3os::platform::mk66
