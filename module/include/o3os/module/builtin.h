// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The modules that travel inside the firmware image (OS-012d, new Community OS behaviour): the TB-303, chip, FM and SID .tmod files, as bytes in flash. A built-in module is the very
// file the card would hold, loaded by the same ModuleLoader through the same checks (header, relocation, CRC, descriptor), into the same arenas (hot part in the SRAM_L slab,
// the rest in SDRAM); the only difference is where the bytes are read from. The image's composition root hands the table to ModuleSet::set_builtins(); this layer does not link it.
//
// OS-FLASHDIET A3: the file is stored compressed -- the 64-byte header raw in `data` (so builtin_header() and prefers_card() still read it in place) and the rest as a
// raw-deflate stream in `z` / `z_size`, made at build time by tools/deflate_pack.py (our own deterministic encoder, never Python's zlib). `size` stays the whole file's size.
// The loader reads the header from `data` and inflates the rest through its own core::Inflater (loader.h), so the CRC and every check run on the inflated bytes exactly as on a
// card's file.
//
// The rule between a built-in module and a file of the same stem on the card (loader.h): the card's copy is used when it is valid and NEWER (see prefers_card()), otherwise
// the built-in one is the fallback. "Newer" is the module version in the .tmod header (ABI 1.2, OS-MODVER, O3M_VERSION(major, minor, patch)): a card copy replaces the built-in
// module only when its version is above the built-in one's, so after a firmware update that carries a fixed built-in module an older copy left on the card no longer shadows it.

#include <o3module.h>

#include <o3os/core/inflate.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::module {

struct BuiltinModule {
  const char* stem;            // the file stem (model::ModuleRef::name)
  const std::uint8_t* data;    // the .tmod header, 64 bytes, raw (flash)
  const std::uint8_t* z;       // OS-FLASHDIET A3: the raw-deflate stream of the rest of the file (tools/deflate_pack.py, a 4 KiB window)
  std::uint32_t z_size;        // its size in bytes
  std::uint32_t size;          // the whole .tmod file's size (header + the stream's raw size)
};

// The .tmod header of a built-in module (64 bytes copied; the table is generated from valid files, the loader validates it all the same).
inline o3m_file_header builtin_header(const BuiltinModule& b) noexcept {
  o3m_file_header h{};
  if (b.size >= sizeof h) {
    std::memcpy(&h, b.data, sizeof h);
  }
  return h;
}

// The whole .tmod file of a built-in module: the header from `data`, the rest inflated from `z` (the tests use this to compare with the file the build made; the
// loader reads the same two pieces through its own Inflater). dst needs room for b.size bytes. False when the stream fails or does not end exactly at the file's size.
inline bool builtin_file(const BuiltinModule& b, std::uint8_t* dst, std::size_t cap) noexcept {
  if (b.size < sizeof(o3m_file_header) || cap < b.size) {
    return false;
  }
  std::memcpy(dst, b.data, sizeof(o3m_file_header));
  const std::size_t rest = b.size - sizeof(o3m_file_header);
  core::Inflater inf;
  inf.begin(b.z, b.z_size, rest);
  std::size_t got = 0;
  while (got < rest) {
    const std::size_t n = inf.read(dst + sizeof(o3m_file_header) + got, rest - got);
    if (n == 0u) {
      break;
    }
    got += n;
  }
  return !inf.failed() && inf.done() && got == rest;
}

// Whether a card copy with header `card` (already through check_header(), which holds the version word to zero in a file older than ABI 1.2) replaces the built-in module with
// header `builtin`: the same kind, the same ABI major and a module version strictly above the built-in one's. Equal versions keep the built-in module (a tie is "the same module"),
// and so does an unversioned card copy (0): to override a built-in module, ship a higher version.
inline bool prefers_card(const o3m_file_header& card, const o3m_file_header& builtin) noexcept {
  return card.kind == builtin.kind && card.abi_major == builtin.abi_major && card.module_version > builtin.module_version;
}

// OS-FLASHDIET P1: the licence notices of the code in the built-in modules, as text the build joined (modules/CMakeLists.txt: o3m_embed.py --notice) and deflated with
// tools/deflate_pack.py (the same encoder and stream form as a module's body). `size` is the text's length in bytes; the text is lines ending in '\n'. Nothing reads it whole:
// notices_page() streams it.
struct BuiltinNotices {
  const std::uint8_t* z;  // the raw-deflate stream (word aligned)
  std::uint32_t z_size;   // its size in bytes
  std::uint32_t size;     // the text's size in bytes
};

// The sink of notices_page(): `n` bytes of the current line (never containing '\n'; a line may come in several pieces), or `eol` true (text null, n 0) when the line ends.
using NoticeSink = void (*)(void* ctx, const char* text, std::size_t n, bool eol) noexcept;

struct NoticePage {
  bool ok;        // false: the stream failed or ended short (nothing more is written then)
  bool more;      // text follows the page
  unsigned next;  // the line a following page starts at (the line count when it began past the end)
};

// Streams lines [first, first + max_lines) of the notices `n` through `sink`, decoding with `inf` from the start (the console's `mod notice [line]`). Main loop only: `inf` is the
// caller's shared decoder (about 5 KB), restarted here. No heap, a 64-byte buffer on the stack.
NoticePage notices_page(const BuiltinNotices& n, core::Inflater& inf, unsigned first, unsigned max_lines, NoticeSink sink, void* ctx) noexcept;

// The generated table (modules/builtin: o3m_embed.py) and the notices of the code in it. Defined by the o3m_builtin library, which the images link.
extern const BuiltinModule kBuiltinModules[];
extern const unsigned kBuiltinModuleCount;
extern const BuiltinNotices kBuiltinNotices;  // the licence notices the built-in modules require (OS-FLASHDIET P1: a deflate stream, read through notices_page())

}  // namespace o3os::module
