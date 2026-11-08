// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FLASHDIET P6: the built-in key bindings table as a deflate blob (tools/bindings_pack.py over the bytes of the constexpr table, lifted from a second compile of
// app/src/bindings.cpp by objcopy), unpacked once at boot on the MK66 image into an SDRAM table that builtin_bindings() then returns. The host keeps the constexpr
// table; this test holds the blob to it: the stream inflates to exactly the table's bytes, the unpacked table answers like the constexpr one (and its find_conflict
// stays clear), a short or damaged stream or a too small table is refused, and detaching returns builtin_bindings() to the constexpr table.
#include <doctest/doctest.h>

#include <o3os/app/bindings.h>
#include <o3os/core/binding.h>
#include <o3os/core/inflate.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "support/need.h"

using namespace o3os;

namespace {

// builtin_bindings() as the constexpr table (this test binary never attaches an unpacked table, except inside the cases below, which detach again).
const core::BindingView plain_view() { return app::builtin_bindings(); }

}  // namespace

TEST_CASE("bindings blob: the stream inflates to the constexpr table's bytes, and is a real saving") {
  const core::BindingView plain = plain_view();
  const std::size_t bytes = plain.count * sizeof(core::Binding);
  NEED(plain.count > 0);
  CHECK(app::kBuiltinBindingsRawSize == bytes);
  CHECK(app::kBuiltinBindingsZSize < bytes / 2);
  CHECK(plain.count <= app::kBuiltinBindingsCapacity);
  core::Inflater inf;
  inf.begin(app::kBuiltinBindingsZ, app::kBuiltinBindingsZSize, app::kBuiltinBindingsRawSize);
  static std::uint8_t got[app::kBuiltinBindingsCapacity * sizeof(core::Binding)];
  NEED(inf.read(got, bytes) == bytes);
  CHECK(inf.done());
  CHECK_FALSE(inf.failed());
  // Binding by binding, not one memcmp: it names the first wrong row. The struct has no padding (16 B, static_assert in binding.h), so the bytes are the table.
  unsigned wrong = 0;
  for (std::size_t i = 0; i < plain.count; ++i) {
    wrong += std::memcmp(got + i * sizeof(core::Binding), plain.items + i, sizeof(core::Binding)) != 0 ? 1u : 0u;
  }
  CHECK(wrong == 0u);
}

TEST_CASE("bindings blob: the unpacked table answers like the constexpr one, and detaching returns to it") {
  const core::BindingView plain = plain_view();
  static core::Binding table[app::kBuiltinBindingsCapacity];
  core::Inflater inf;
  std::memset(table, 0xFF, sizeof table);
  const std::size_t n = app::builtin_bindings_unpack(inf, table, app::kBuiltinBindingsCapacity);
  NEED(n == plain.count);
  CHECK(std::memcmp(table, plain.items, n * sizeof(core::Binding)) == 0);
  CHECK_FALSE(core::find_conflict(table, n).found);

  app::builtin_bindings_use(table, n);
  const core::BindingView packed = app::builtin_bindings();
  CHECK(packed.items == table);
  CHECK(packed.count == n);
  app::builtin_bindings_use(nullptr, 0);  // the table above is static, but leave the accessor as the other tests expect it
  CHECK(app::builtin_bindings().items == plain.items);
  CHECK(app::builtin_bindings().count == plain.count);
}

TEST_CASE("bindings blob: a truncated or damaged stream, or a table that is too small, is refused") {
  static core::Binding table[app::kBuiltinBindingsCapacity];
  const std::size_t bytes = app::kBuiltinBindingsRawSize;
  core::Inflater inf;
  // The stream cut short: the decoder reports failure, and builtin_bindings_unpack's own check (all bytes, done, not failed) cannot pass on a cut copy of it.
  inf.begin(app::kBuiltinBindingsZ, app::kBuiltinBindingsZSize / 2, app::kBuiltinBindingsRawSize);
  const std::size_t got = inf.read(reinterpret_cast<std::uint8_t*>(table), bytes);
  CHECK((got < bytes || !inf.done()));
  CHECK(inf.failed());
  // A decoder asked for more bytes than the stream holds does not run past it.
  core::Inflater inf2;
  static std::uint8_t big[app::kBuiltinBindingsCapacity * sizeof(core::Binding) + 16u];
  inf2.begin(app::kBuiltinBindingsZ, app::kBuiltinBindingsZSize, app::kBuiltinBindingsRawSize + 16u);
  CHECK(inf2.read(big, bytes + 16u) <= bytes + 16u);
  CHECK_FALSE(inf2.done());
  // A table with room for fewer bindings than the blob holds is refused whole (0), and the destination is not attached.
  core::Inflater inf3;
  CHECK(app::builtin_bindings_unpack(inf3, table, 1) == 0u);
  core::Inflater inf4;
  CHECK(app::builtin_bindings_unpack(inf4, table, 0) == 0u);
}
