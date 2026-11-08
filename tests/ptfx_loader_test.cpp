// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S2a: the stepped `.ptfx` loader (module/ptfx_loader.h) over an in-memory card (tests/support/mem_card.h) and a
// plain array as the image buffer: docs/specs/OS-CEPTFX.md 3.2's four load steps — bytes copied, the tail zeroed including
// the 1-3 byte gap, the sync hook called once over the exact range, the entries base + offset | 1 inside the image, a null
// entry staying null, a short read and a card error aborting with no entries, the step count for a 32 KiB image, and the
// stack guard's paint and check. Host only.
#include <doctest/doctest.h>

#include <o3os/module/ptfx_loader.h>

#include "support/mem_card.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace o3os::module;

namespace {

namespace hal = o3os::hal;

void put16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t x) {
  v[at] = static_cast<std::uint8_t>(x);
  v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
  put16(v, at, static_cast<std::uint16_t>(x));
  put16(v, at + 2, static_cast<std::uint16_t>(x >> 16));
}

// A valid file: the 472-byte header plus `code_size` payload bytes (a pattern), truncated to `file_payload` bytes when
// that is smaller (a file that shrank between the scan and the load). Entries at the given offsets (0 is a valid entry).
std::vector<std::uint8_t> make_file(std::uint32_t code_size, std::uint32_t bss_size, std::uint32_t init_off, std::uint32_t set_param_off,
                                    std::uint32_t process_block_off, std::uint32_t file_payload = 0xFFFFFFFFu) {
  std::vector<std::uint8_t> v(kPtfxHeaderSize + code_size, 0);
  put32(v, 0, kPtfxMagic);
  put16(v, 4, kPtfxVersion);
  put16(v, 6, 0);
  std::memcpy(&v[8], "Loader Test", 11);
  put32(v, 64, kPtfxHeaderSize);  // code_offset
  put32(v, 68, code_size);
  put32(v, 72, bss_size);
  put32(v, 76, init_off);
  put32(v, 80, set_param_off);
  put32(v, 84, process_block_off);
  for (std::uint32_t i = 0; i < code_size; ++i) {
    v[kPtfxHeaderSize + i] = static_cast<std::uint8_t>(i * 7u + 1u);
  }
  if (file_payload != 0xFFFFFFFFu && file_payload < code_size) {
    v.resize(kPtfxHeaderSize + file_payload);
  }
  return v;
}

// No REQUIRE in a no-exceptions build: the status is checked at the call site.
PtfxInfo parse(const std::vector<std::uint8_t>& file, PtfxStatus& status) {
  PtfxInfo info;
  status = ptfx_parse_header(file.data(), file.size() < kPtfxHeaderSize ? file.size() : kPtfxHeaderSize, file.size(), info);
  return info;
}
PtfxInfo parse(const std::vector<std::uint8_t>& file) {
  PtfxStatus s;
  PtfxInfo info = parse(file, s);
  CHECK(s == PtfxStatus::Ok);
  return info;
}

// A header the parser would accept, built by hand (for a file whose payload ends early: the parser refuses that when it
// sees the file's size, but the file can shrink after the scan).
PtfxInfo info_of(std::uint32_t code_size, std::uint32_t bss_size, std::uint32_t init_off, std::uint32_t set_param_off, std::uint32_t pb_off) {
  PtfxInfo info{};
  info.status = PtfxStatus::Ok;
  info.code_offset = kPtfxHeaderSize;
  info.code_size = code_size;
  info.bss_size = bss_size;
  info.init_offset = init_off;
  info.set_param_offset = set_param_off;
  info.process_block_offset = pb_off;
  return info;
}

struct SyncLog {
  const void* begin = nullptr;
  std::uint32_t bytes = 0;
  unsigned calls = 0;
};
SyncLog g_sync;
void fake_sync(const void* begin, std::uint32_t bytes) noexcept {
  g_sync.begin = begin;
  g_sync.bytes = bytes;
  ++g_sync.calls;
}

struct Rig {
  o3os::test::MemCard card;
  PtfxLoader loader;
  std::vector<std::uint8_t> image;

  // Puts the file on the card, opens it and starts the load of `info` into `image_bytes` of a 0xCC-filled image.
  PtfxLoader::Begin start_info(const std::vector<std::uint8_t>& file, const PtfxInfo& info, std::uint32_t image_bytes, PtfxLoader::SyncFn sync = &fake_sync) {
    card.put("Effects/test.ptfx", file);
    hal::storage::FileHandle f{};
    CHECK(card.open("Effects/test.ptfx", f) == hal::storage::Status::Ok);
    image.assign(image_bytes, 0xCC);
    return loader.begin(card, f, info, image.data(), image_bytes, sync);
  }
  PtfxLoader::Begin start(const std::vector<std::uint8_t>& file, std::uint32_t image_bytes) { return start_info(file, parse(file), image_bytes); }
  // Steps to the end; returns the number of step() calls (one phase action a step).
  unsigned run_to_end() {
    unsigned steps = 0;
    for (unsigned guard = 0; guard < 100000u && loader.step(2000) == o3os::core::JobStatus::Running; ++guard) {
      ++steps;
    }
    return steps + 1u;  // the step that answered Done / Failed
  }
};

}  // namespace

TEST_CASE("ptfx_loader: a full load of a 32 KiB image: bytes copied, sync once over the exact range, entries odd and inside, 9 steps") {
  Rig r;
  g_sync = SyncLog{};
  const std::uint32_t code_size = 32u * 1024u;
  const auto file = make_file(code_size, 0, 0, 8, 16);
  CHECK(r.start(file, 32u * 1024u) == PtfxLoader::Begin::Started);
  CHECK(r.loader.state() == PtfxLoader::State::Running);
  CHECK(r.loader.total_bytes() == 32u * 1024u);
  const unsigned steps = r.run_to_end();
  // One phase action a step: 1 seek + 32 KiB / 4 KiB copy steps + 1 sync/entries step (no tail: bss is 0).
  CHECK(steps == 1u + (code_size / PtfxLoader::kChunk) + 1u);
  CHECK(r.loader.state() == PtfxLoader::State::Done);
  CHECK(r.loader.status() == PtfxLoader::Status::Ok);
  CHECK(r.loader.progress() == o3os::core::kJobProgressMax);
  CHECK(std::strcmp(r.loader.name(), "PTFX LOAD") == 0);
  CHECK(std::strcmp(r.loader.detail(), "") == 0);
  // The payload landed byte for byte.
  CHECK(std::memcmp(r.image.data(), file.data() + kPtfxHeaderSize, code_size) == 0);
  // The sync hook ran once, over the exact image range.
  CHECK(g_sync.calls == 1u);
  CHECK(g_sync.begin == r.image.data());
  CHECK(g_sync.bytes == 32u * 1024u);
  // The entries: base + offset | 1, inside the image. OS-BUG-PTFXREADY: init at offset 0 (the SDK plugins' layout) is a real entry, not an absent one.
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(r.image.data());
  CHECK(r.loader.entries().init == (base | 1u));
  CHECK(r.loader.entries().set_param == ((base + 8u) | 1u));
  CHECK(r.loader.entries().process_block == ((base + 16u) | 1u));
  CHECK((r.loader.entries().set_param & 1u) != 0u);
  CHECK(r.loader.entries().set_param >= base );
  CHECK(r.loader.entries().set_param < base + code_size);
  CHECK(r.card.open_handles == 0);  // the loader closed the file it was given
}

TEST_CASE("ptfx_loader: the tail is zeroed up to align4(code_size) + bss_size, the 1-3 byte gap included") {
  Rig r;
  g_sync = SyncLog{};
  // code_size 101: align4 is 104, so the 3-byte gap [101, 104) plus bss [104, 109) is the tail.
  const auto file = make_file(101, 5, 4, 8, 16);
  CHECK(r.start(file, 4096) == PtfxLoader::Begin::Started);
  CHECK(r.loader.total_bytes() == 109u);
  const unsigned steps = r.run_to_end();
  CHECK(steps == 1u + 1u + 1u + 1u);  // seek, one copy step (101 B), one tail step (8 B), sync
  CHECK(r.loader.state() == PtfxLoader::State::Done);
  CHECK(std::memcmp(r.image.data(), file.data() + kPtfxHeaderSize, 101) == 0);  // the payload
  for (std::uint32_t i = 101; i < 109; ++i) {
    CHECK(r.image[i] == 0u);  // the gap and .bss zeroed
  }
  CHECK(r.image[109] == 0xCCu);  // past the image, untouched
  CHECK(g_sync.calls == 1u);
  CHECK(g_sync.bytes == 109u);
  CHECK(g_sync.begin == r.image.data());
  CHECK(r.loader.entries().init == ((reinterpret_cast<std::uintptr_t>(r.image.data()) + 4u) | 1u));
}

TEST_CASE("ptfx_loader: progress while running, and a null sync hook is allowed") {
  Rig r;
  g_sync = SyncLog{};
  const auto file = make_file(3u * PtfxLoader::kChunk, 0, 0, 8, 16);
  CHECK(r.start_info(file, parse(file), 16u * 1024u, nullptr) == PtfxLoader::Begin::Started);
  CHECK(r.loader.step(2000) == o3os::core::JobStatus::Running);  // the seek
  CHECK(r.loader.progress() == 0u);
  CHECK(r.loader.step(2000) == o3os::core::JobStatus::Running);  // 4 KiB copied
  const unsigned p1 = r.loader.progress();
  CHECK(p1 > 0u );
  CHECK(p1 < o3os::core::kJobProgressMax);
  while (r.loader.step(2000) == o3os::core::JobStatus::Running) {
  }
  CHECK(r.loader.state() == PtfxLoader::State::Done);
  CHECK(r.loader.progress() == o3os::core::kJobProgressMax);
  CHECK(g_sync.calls == 0u);  // no sync hook was given
  CHECK(r.loader.entries().init != 0u);  // (init at offset 0 of the image is still an entry)
  CHECK(r.loader.entries().set_param != 0u);
}

TEST_CASE("ptfx_loader: OS-BUG-PTFXREADY: a plugin with init at offset 0 gets all three entries (the job refuses a null one as PLUGIN NOT READY)") {
  Rig r;
  const auto file = make_file(64, 0, 0, 8, 16);  // the SDK fixtures' layout: init is the first function
  CHECK(r.start(file, 4096) == PtfxLoader::Begin::Started);
  r.run_to_end();
  CHECK(r.loader.state() == PtfxLoader::State::Done);
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(r.image.data());
  CHECK(r.loader.entries().init == (base | 1u));
  CHECK(r.loader.entries().set_param == ((base + 8u) | 1u));
  CHECK(r.loader.entries().process_block == ((base + 16u) | 1u));
}

TEST_CASE("ptfx_loader: a short read aborts the load with no entries") {
  Rig r;
  g_sync = SyncLog{};
  // The header promises 8 KiB of payload, the file holds 5 KiB: the read ends inside the payload.
  const auto file = make_file(8u * 1024u, 0, 0, 8, 16, 5u * 1024u);
  CHECK(r.start_info(file, info_of(8u * 1024u, 0, 0, 8, 16), 32u * 1024u) == PtfxLoader::Begin::Started);
  r.run_to_end();
  CHECK(r.loader.state() == PtfxLoader::State::Failed);
  CHECK(r.loader.status() == PtfxLoader::Status::ShortFile);
  CHECK(std::strcmp(r.loader.detail(), "SHORT FILE") == 0);
  CHECK(r.loader.entries().init == 0u);
  CHECK(r.loader.entries().set_param == 0u);
  CHECK(r.loader.entries().process_block == 0u);
  CHECK(r.card.open_handles == 0);
  // Part of the payload was copied, but no entry was yielded and no sync ran.
  CHECK(r.image[0] == file[kPtfxHeaderSize]);
  CHECK(g_sync.calls == 0u);
}

TEST_CASE("ptfx_loader: a card error aborts the load with its status, never half-loaded entries") {
  Rig r;
  g_sync = SyncLog{};
  const auto file = make_file(8u * 1024u, 0, 0, 8, 16);
  r.card.read_fault = "effects/test.ptfx";  // every read of the file fails with Io
  CHECK(r.start(file, 32u * 1024u) == PtfxLoader::Begin::Started);
  r.run_to_end();
  CHECK(r.loader.state() == PtfxLoader::State::Failed);
  CHECK(r.loader.status() == PtfxLoader::Status::CardError);
  CHECK(r.loader.card_status() == hal::storage::Status::Io);
  CHECK(std::strcmp(r.loader.detail(), "i/o error") == 0);
  CHECK(r.loader.entries().init == 0u);
  CHECK(r.loader.entries().set_param == 0u);
  CHECK(r.loader.entries().process_block == 0u);
  CHECK(r.card.open_handles == 0);
  CHECK(g_sync.calls == 0u);
}

TEST_CASE("ptfx_loader: begin's refusals, and cancel closes the file and leaves no entries") {
  Rig r;
  g_sync = SyncLog{};
  const auto good = make_file(1024, 0, 0, 8, 16);
  const auto big = make_file(8u * 1024u, 0, 0, 8, 16);
  auto damaged = make_file(1024, 0, 0, 8, 16);
  damaged[0] = 0;  // not the magic: the parser refuses it
  PtfxInfo bad_info;
  CHECK(ptfx_parse_header(damaged.data(), kPtfxHeaderSize, damaged.size(), bad_info) == PtfxStatus::NotPtfx);
  r.card.put("Effects/good.ptfx", good);
  r.card.put("Effects/big.ptfx", big);
  r.card.put("Effects/bad.ptfx", damaged);
  r.image.assign(4096, 0xCC);
  hal::storage::FileHandle f{};
  // BadImage: the image buffer cannot hold align4(code_size) + bss_size.
  CHECK(r.card.open("Effects/big.ptfx", f) == hal::storage::Status::Ok);
  CHECK(r.loader.begin(r.card, f, parse(big), r.image.data(), 4096, nullptr) == PtfxLoader::Begin::BadImage);
  CHECK(r.card.open_handles == 1);  // a refused begin leaves the handle to the caller
  r.card.close(f);
  // BadHeader: the parser refused the file.
  CHECK(r.card.open("Effects/bad.ptfx", f) == hal::storage::Status::Ok);
  CHECK(r.loader.begin(r.card, f, bad_info, r.image.data(), 4096, nullptr) == PtfxLoader::Begin::BadHeader);
  r.card.close(f);
  CHECK(r.card.open_handles == 0);
  // Started, then Busy while it runs.
  CHECK(r.card.open("Effects/good.ptfx", f) == hal::storage::Status::Ok);
  CHECK(r.loader.begin(r.card, f, parse(good), r.image.data(), 4096, nullptr) == PtfxLoader::Begin::Started);
  hal::storage::FileHandle f2{};
  CHECK(r.card.open("Effects/good.ptfx", f2) == hal::storage::Status::Ok);
  CHECK(r.loader.begin(r.card, f2, parse(good), r.image.data(), 4096, nullptr) == PtfxLoader::Begin::Busy);
  r.card.close(f2);
  // Cancel: the file is closed, no entries, and a new begin can start.
  r.loader.cancel();
  CHECK(r.loader.state() == PtfxLoader::State::Idle);
  CHECK(r.loader.entries().init == 0u);
  CHECK(r.loader.entries().set_param == 0u);
  CHECK(r.loader.entries().process_block == 0u);
  CHECK(r.card.open_handles == 0);
  CHECK(r.card.open("Effects/good.ptfx", f) == hal::storage::Status::Ok);
  CHECK(r.loader.begin(r.card, f, parse(good), r.image.data(), 4096, &fake_sync) == PtfxLoader::Begin::Started);
  r.run_to_end();
  CHECK(r.loader.state() == PtfxLoader::State::Done);
  CHECK(g_sync.calls == 1u);
  CHECK(r.loader.entries().set_param == ((reinterpret_cast<std::uintptr_t>(r.image.data()) + 8u) | 1u));
}

TEST_CASE("ptfx_loader: the stack guard is painted and checked") {
  std::uint8_t guard[4096];
  std::memset(guard, 0, sizeof guard);
  CHECK_FALSE(ptfx_check_guard(guard, sizeof guard));
  ptfx_paint_guard(guard, sizeof guard);
  CHECK(ptfx_check_guard(guard, sizeof guard));
  CHECK(guard[0] == kPtfxGuardByte );
  CHECK(guard[sizeof guard - 1] == kPtfxGuardByte);
  guard[2048] ^= 0xFFu;  // a call ran into the guard
  CHECK_FALSE(ptfx_check_guard(guard, sizeof guard));
  ptfx_paint_guard(guard, sizeof guard);
  CHECK(ptfx_check_guard(guard, sizeof guard));
  CHECK(ptfx_check_guard(guard, 0u));  // an empty guard is intact
  // Word compare: a single wrong byte in any position of a word is seen, also in the last word.
  for (const std::uint32_t at : {0u, 1u, 2u, 3u, 4092u, 4095u}) {
    ptfx_paint_guard(guard, sizeof guard);
    guard[at] = 0xA4u;
    CHECK_FALSE(ptfx_check_guard(guard, sizeof guard));
  }
}

TEST_CASE("ptfx_loader: a tail longer than one 4 KiB step is zeroed over several steps, the odd code_size gap included") {
  Rig r;
  g_sync = SyncLog{};
  // code_size 101 (align4 104) + bss 9000: the tail [101, 9104) is 9003 bytes, 3 steps of 4096 + 4096 + 811.
  const auto file = make_file(101, 9000, 4, 8, 16);
  CHECK(r.start(file, 16u * 1024u) == PtfxLoader::Begin::Started);
  CHECK(r.loader.total_bytes() == 9104u);
  CHECK(r.run_to_end() == 1u + 1u + 3u + 1u);  // seek, one copy, three tail steps, sync
  CHECK(r.loader.state() == PtfxLoader::State::Done);
  CHECK(std::memcmp(r.image.data(), file.data() + kPtfxHeaderSize, 101) == 0);
  bool all_zero = true;
  for (std::uint32_t i = 101; i < 9104; ++i) {
    all_zero = all_zero && r.image[i] == 0u;
  }
  CHECK(all_zero);
  CHECK(r.image[9104] == 0xCCu);  // one past the tail: untouched
  CHECK(g_sync.calls == 1u);
  CHECK(g_sync.bytes == 9104u);
}

TEST_CASE("ptfx_loader: a gap of 1-3 bytes with bss 0 is zeroed and nothing past it") {
  Rig r;
  g_sync = SyncLog{};
  const auto file = make_file(102, 0, 4, 8, 16);  // align4(102) = 104: a 2-byte gap
  CHECK(r.start(file, 4096) == PtfxLoader::Begin::Started);
  CHECK(r.loader.total_bytes() == 104u);
  CHECK(r.run_to_end() == 1u + 1u + 1u + 1u);  // seek, copy, tail (the gap), sync
  CHECK(r.loader.state() == PtfxLoader::State::Done);
  CHECK(r.image[102] == 0u);
  CHECK(r.image[103] == 0u);
  CHECK(r.image[104] == 0xCCu);
  CHECK(g_sync.bytes == 104u);
}

TEST_CASE("ptfx_loader: a partial read (got < want, not zero) continues from where it stopped") {
  Rig r;
  g_sync = SyncLog{};
  const std::uint32_t code_size = 10000;
  const auto file = make_file(code_size, 0, 0, 8, 16);
  r.card.read_cap = 1000;  // every read gives at most 1000 bytes
  CHECK(r.start(file, 16u * 1024u) == PtfxLoader::Begin::Started);
  CHECK(r.run_to_end() == 1u + 10u + 1u);  // seek, ten 1000-byte reads, sync
  CHECK(r.loader.state() == PtfxLoader::State::Done);
  CHECK(std::memcmp(r.image.data(), file.data() + kPtfxHeaderSize, code_size) == 0);
  CHECK(g_sync.calls == 1u);
}

TEST_CASE("ptfx_loader: a backend that over-reports its read is clamped to what was asked") {
  Rig r;
  g_sync = SyncLog{};
  const std::uint32_t code_size = 10000;
  const auto file = make_file(code_size, 0, 0, 8, 16);
  r.card.read_over = 100000;  // every read says it gave 100000 bytes more than it did
  CHECK(r.start(file, 16u * 1024u) == PtfxLoader::Begin::Started);
  CHECK(r.run_to_end() == 1u + 3u + 1u);  // seek, 4096 + 4096 + 1808, sync: pos_ never passed code_size
  CHECK(r.loader.state() == PtfxLoader::State::Done);
  CHECK(std::memcmp(r.image.data(), file.data() + kPtfxHeaderSize, code_size) == 0);
  CHECK(r.image[code_size] == 0xCCu);  // nothing written past the payload (bss 0, code_size a multiple of 4)
  CHECK(g_sync.bytes == code_size);
}

TEST_CASE("ptfx_loader: a seek failure aborts with the card's status and no entries") {
  Rig r;
  g_sync = SyncLog{};
  const auto file = make_file(2048, 0, 0, 8, 16);
  r.card.seek_fault = true;
  CHECK(r.start(file, 4096) == PtfxLoader::Begin::Started);
  r.run_to_end();
  CHECK(r.loader.state() == PtfxLoader::State::Failed);
  CHECK(r.loader.status() == PtfxLoader::Status::CardError);
  CHECK(r.loader.card_status() == hal::storage::Status::Io);
  CHECK(r.loader.entries().init == 0u);
  CHECK(r.loader.entries().set_param == 0u);
  CHECK(r.loader.entries().process_block == 0u);
  CHECK(r.card.open_handles == 0);
  CHECK(r.image[0] == 0xCCu);  // nothing was copied
  CHECK(g_sync.calls == 0u);
}

TEST_CASE("ptfx_loader: begin() re-checks a hand-built PtfxInfo: empty payload, entries outside it, sizes that wrap or exceed 32 KiB") {
  Rig r;
  const auto file = make_file(1024, 0, 0, 8, 16);
  r.card.put("Effects/test.ptfx", file);
  const std::uint32_t big = 64u * 1024u;
  r.image.assign(big, 0xCC);
  struct Case {
    PtfxInfo info;
    const char* what;
  };
  const Case cases[] = {
      {info_of(0, 0, 0, 0, 0), "code_size 0"},
      {info_of(1024, 0, 1024, 8, 16), "init at code_size"},
      {info_of(1024, 0, 0, 1025, 16), "set_param past code_size (bit 0 masked)"},
      {info_of(1024, 0, 0, 8, 0xFFFFFFFFu), "process_block far outside"},
      {info_of(0xFFFFFFFDu, 0, 0, 8, 16), "align4 would wrap (code_size 0xFFFFFFFD)"},
      {info_of(0xFFFFFFFFu, 0, 0, 8, 16), "align4 would wrap to 0 (code_size 0xFFFFFFFF)"},
      {info_of(1024, 0xFFFFFFFFu, 0, 8, 16), "bss 0xFFFFFFFF"},
      {info_of(32u * 1024u + 4u, 0, 0, 8, 16), "code over 32 KiB"},
      {info_of(32u * 1024u, 4, 0, 8, 16), "code + bss over 32 KiB"},
      {info_of(1023, 32u * 1024u - 1023u, 0, 8, 16), "align4(1023) + bss over 32 KiB by one word"},
  };
  for (const Case& c : cases) {
    INFO(c.what);
    hal::storage::FileHandle f{};
    CHECK(r.card.open("Effects/test.ptfx", f) == hal::storage::Status::Ok);
    CHECK(r.loader.begin(r.card, f, c.info, r.image.data(), big, nullptr) == PtfxLoader::Begin::BadImage);
    CHECK(r.loader.state() == PtfxLoader::State::Idle);
    CHECK(r.card.open_handles == 1);  // the refusal left the file with the caller
    r.card.close(f);
  }
  // The exact limit is accepted: 32 KiB - 4 of code plus 4 of bss.
  hal::storage::FileHandle f{};
  CHECK(r.card.open("Effects/test.ptfx", f) == hal::storage::Status::Ok);
  CHECK(r.loader.begin(r.card, f, info_of(32u * 1024u - 4u, 4, 0, 8, 16), r.image.data(), big, nullptr) == PtfxLoader::Begin::Started);
  r.loader.cancel();
  CHECK(r.card.open_handles == 0);
}
