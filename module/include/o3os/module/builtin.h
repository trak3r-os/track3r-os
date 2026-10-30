// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The modules that travel inside the firmware image (OS-012d, new Community OS behaviour): the TB-303, chip, FM and SID .tmod files, as bytes in flash. A built-in module is the very
// file the card would hold, loaded by the same ModuleLoader through the same checks (header, relocation, CRC, descriptor), into the same arenas (hot part in the SRAM_L slab,
// the rest in SDRAM); the only difference is where the bytes are read from. The image's composition root hands the table to ModuleSet::set_builtins(); this layer does not link it.
//
// The rule between a built-in module and a file of the same stem on the card (loader.h): the card's copy is used when it is valid and NEWER (see prefers_card()), otherwise
// the built-in one is the fallback. "Newer" is the module version in the .tmod header (ABI 1.2, OS-MODVER, O3M_VERSION(major, minor, patch)): a card copy replaces the built-in
// module only when its version is above the built-in one's, so after a firmware update that carries a fixed built-in module an older copy left on the card no longer shadows it.

#include <o3module.h>

#include <cstdint>
#include <cstring>

namespace o3os::module {

struct BuiltinModule {
  const char* stem;            // the file stem (model::ModuleRef::name)
  const std::uint8_t* data;    // the .tmod file, byte for byte (flash)
  std::uint32_t size;
};

// The .tmod header of a built-in module (64 bytes copied; the table is generated from valid files, the loader validates it all the same).
inline o3m_file_header builtin_header(const BuiltinModule& b) noexcept {
  o3m_file_header h{};
  if (b.size >= sizeof h) {
    std::memcpy(&h, b.data, sizeof h);
  }
  return h;
}

// Whether a card copy with header `card` (already through check_header(), which holds the version word to zero in a file older than ABI 1.2) replaces the built-in module with
// header `builtin`: the same kind, the same ABI major and a module version strictly above the built-in one's. Equal versions keep the built-in module (a tie is "the same module"),
// and so does an unversioned card copy (0): to override a built-in module, ship a higher version.
inline bool prefers_card(const o3m_file_header& card, const o3m_file_header& builtin) noexcept {
  return card.kind == builtin.kind && card.abi_major == builtin.abi_major && card.module_version > builtin.module_version;
}

// The generated table (modules/builtin: o3m_embed.py) and the notices of the code in it. Defined by the o3m_builtin library, which the images link.
extern const BuiltinModule kBuiltinModules[];
extern const unsigned kBuiltinModuleCount;
extern const char kBuiltinNotices[];  // the licence notices the built-in modules require: NUL terminated text

}  // namespace o3os::module
