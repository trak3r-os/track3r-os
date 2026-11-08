// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-FLASHDIET A6/A7: the help pages packed into one blob (docs/specs/OS-FLASHDIET.md 3.4). The authoring
// file is app/help/pages.txt; app/CMakeLists.txt runs tools/help_pack.py over it and generates help_blob.cpp
// into the build tree, which defines the externs below: the raw-deflate stream of the pages' lines (A2's
// tools/deflate_pack.py, never zlib), the page index (each page's offset in the inflated text) and the page
// titles. src/help_blob.cpp inflates a page out of the stream, and src/help_text.cpp's help_page() serves it
// from the page buffer below (A7: the old C++ table is gone).

#include <o3os/app/help_text.h>  // HelpLine, kHelpPages

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// Defined by the generated help_blob.cpp (app/CMakeLists.txt, tools/help_pack.py):
extern const std::uint8_t* const kHelpBlobZ;  // the raw-deflate stream (word aligned, deflate_pack's emit_cpp)
extern const std::size_t kHelpBlobZSize;      // its size in bytes
extern const std::size_t kHelpBlobRawSize;    // the inflated text: every page's lines, concatenated
extern const std::uint32_t kHelpBlobPageOffsets[];  // kHelpBlobPages entries: a page's offset in the inflated text
extern const char* const kHelpBlobTitles[];        // kHelpBlobPages entries: the page titles (in flash)
extern const unsigned kHelpBlobPages;

// The page buffer: one 4 KiB object holding a page's line table and the strings its keys and texts point
// into. The title comes from kHelpBlobTitles (flash), so the largest page's strings fit. A7 keeps one in
// SDRAM_STATIC; its content is valid until the next help_blob_page call with another index.
struct HelpPageBuf {
  static constexpr unsigned kMaxLines = 64;  // the largest page today has 57 lines
  static constexpr std::size_t kSize = 4096;
  // The text area: what is left of the 4 KiB after the title pointer, the table and the count. tools/help_pack.py
  // validates every page against this with the build host's sizes (8, 16, 2); the 32-bit target's are smaller.
  static constexpr std::size_t kTextBytes =
      kSize - sizeof(const char*) - sizeof(HelpLine) * kMaxLines - sizeof(std::uint16_t);

  const char* title;         // kHelpBlobTitles[index]
  HelpLine lines[kMaxLines];  // key == nullptr stays nullptr; key "" / text "" point at text[0], the empty string
  std::uint16_t count;
  char text[kTextBytes];
};

static_assert(sizeof(HelpPageBuf) == HelpPageBuf::kSize, "the page buffer is one 4 KiB object");

// Inflate the page `index` (clamped to the last, like help_page()) into `out`: the decode skips to the
// page's offset in the stream and reads just that page. False when the stream is malformed or the page
// does not fit the buffer (help_pack.py refuses such a pages.txt at build time, so this is a guard).
// Main loop only, synchronous; not thread-safe. It decodes through the one shared core::Inflater of the
// UI assets (ui::asset_inflater(): both decode synchronously in one call, never across passes).
bool help_blob_page(unsigned index, HelpPageBuf& out) noexcept;

// The 4 KiB page buffer help_page() serves from; its content is valid until the next help_page() call with
// another index. On the MK66 image it lives in SDRAM_STATIC and is defined by the platform
// (platform/mk66/src/app_main.cpp; app/CMakeLists.txt sets O3OS_HELP_PAGE_BUF_EXTERNAL, which drops app's own
// copy in help_text.cpp); on host, the simulator and QEMU help_text.cpp defines it.
HelpPageBuf& help_page_buf() noexcept;

}  // namespace o3os::app
