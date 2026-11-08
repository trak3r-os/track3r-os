// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FLASHDIET P5/P8: the platform text pools (text.h). The generated blobs inflate to exactly their raw size, every string the pool serves equals the C literal the
// source had (the generated *_expect.h tables, spelled from the authoring files), every command / kernel help id resolves to its own text, and a damaged stream is
// refused. Host and Cortex-M4F (`qemu_mk66`): the guest has no filesystem, so the expected literals are compiled in.
#include <doctest/doctest.h>

#include <o3os/platform/mk66/bench_report.h>
#include <o3os/platform/mk66/bench_text_expect.h>
#include <o3os/platform/mk66/bench_text_ids.h>
#include <o3os/platform/mk66/console_text_expect.h>
#include <o3os/platform/mk66/console_text_ids.h>
#include <o3os/platform/mk66/text.h>
#include <o3os/platform/mk66/usb/command_table.h>

#include "mk66_text_pools.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::platform::mk66 {

// The pools of the image need a decoder from the composition root; the tests have one of their own.
core::Inflater& text_inflater() noexcept {
  static core::Inflater inf;
  return inf;
}

namespace testing {
namespace {

char g_console[txt::kRawSize];
char g_bench[btxt::kRawSize];
int g_console_state = 0;  // 0 not tried, 1 ready, -1 lost
int g_bench_state = 0;

const char* pooled(int& state, char* pool, std::size_t raw, const std::uint8_t* z, std::size_t zsize, std::uint16_t id) {
  if (state == 0) {
    state = text_unpack(text_inflater(), z, zsize, pool, raw) ? 1 : -1;
  }
  return (state > 0 && id < raw) ? pool + id : nullptr;
}

}  // namespace

const char* console_text(std::uint16_t id) {
  return pooled(g_console_state, g_console, txt::kRawSize, kConsoleTextZ, kConsoleTextZSize, id);
}

const char* bench_pool_text(std::uint16_t id) {
  return pooled(g_bench_state, g_bench, btxt::kRawSize, kBenchTextZ, kBenchTextZSize, id);
}

}  // namespace testing
}  // namespace o3os::platform::mk66

namespace mk66 = o3os::platform::mk66;

TEST_CASE("mk66 text: the console pool serves every string as the literal the source had") {
  if (mk66::testing::console_text(0) == nullptr) {
    FAIL_CHECK("the console pool does not decode");
    return;
  }
  std::size_t total = 0;
  for (const mk66::txt::Expect& e : mk66::txt::kExpect) {
    INFO(e.text);
    const char* got = mk66::testing::console_text(e.id);
    CHECK(got != nullptr);
    if (got == nullptr) {
      continue;
    }
    CHECK(std::strcmp(got, e.text) == 0);
    total += std::strlen(e.text) + 1u;
  }
  CHECK(total == mk66::txt::kRawSize);  // the table covers the whole pool: every byte belongs to a string
}

TEST_CASE("mk66 text: the bench pool serves every string as the literal the source had") {
  if (mk66::testing::bench_pool_text(0) == nullptr) {
    FAIL_CHECK("the bench pool does not decode");
    return;
  }
  std::size_t total = 0;
  for (const mk66::btxt::Expect& e : mk66::btxt::kExpect) {
    INFO(e.text);
    const char* got = mk66::testing::bench_pool_text(e.id);
    CHECK(got != nullptr);
    if (got == nullptr) {
      continue;
    }
    CHECK(std::strcmp(got, e.text) == 0);
    total += std::strlen(e.text) + 1u;
  }
  CHECK(total == mk66::btxt::kRawSize);
}

TEST_CASE("mk66 text: spot checks against the literals of the old source") {
  using mk66::testing::bench_pool_text;
  using mk66::testing::console_text;
  CHECK(std::strcmp(console_text(mk66::txt::help_help), "list the commands") == 0);
  CHECK(std::strcmp(console_text(mk66::txt::help_reset), "mute the outputs, then restart the unit") == 0);
  CHECK(std::strcmp(console_text(mk66::txt::cpu_2), "cpu: module perf off (cpu reset starts it again)\n") == 0);
  CHECK(std::strcmp(console_text(mk66::txt::usb_request_5),
                    "usb: DISK mode is held by the boot disk until a restart, nothing changed: ") == 0);
  CHECK(std::strcmp(console_text(mk66::txt::power_9), "usage: power [press|release|pin|off [dry]]\n") == 0);
  CHECK(std::strcmp(console_text(mk66::txt::mod_5), "mod: notices unreadable\n") == 0);
  CHECK(std::strcmp(bench_pool_text(mk66::btxt::k_voice), "one sampler voice, a block (the longest plain sample; takes track 16)") == 0);
  CHECK(std::strcmp(bench_pool_text(mk66::btxt::b_bench_usage_1),
                    "usage: bench <kernel|all> [blocks 1..4096, default 64] [alias|direct|sram]   |   bench disk reset|clean\n") == 0);
  CHECK(std::strcmp(bench_pool_text(mk66::btxt::b_bench_1), "BENCH disk window opened (bench disk reads it; within 60 s)\n") == 0);
}

TEST_CASE("mk66 text: every command and kernel help id resolves to a one-line text of its own") {
  namespace console = mk66::console;
  namespace bench = mk66::bench;
  for (const console::CommandInfo& c : console::kCommandInfo) {
    INFO(c.name);
    const char* h = mk66::testing::console_text(c.help);
    CHECK(h != nullptr);
    if (h == nullptr) {
      continue;
    }
    CHECK(std::strlen(h) > 0u);
    CHECK(std::strlen(h) <= 70u);
    CHECK(std::strchr(h, '\n') == nullptr);
    CHECK(c.help < mk66::txt::kRawSize);
  }
  for (const bench::KernelInfo& k : bench::kKernelInfo) {
    INFO(k.name);
    const char* h = mk66::testing::bench_pool_text(k.help);
    CHECK(h != nullptr);
    if (h == nullptr) {
      continue;
    }
    CHECK(std::strlen(h) > 0u);
    CHECK(std::strlen(h) <= 80u);
    CHECK(std::strchr(h, '\n') == nullptr);
  }
  CHECK(std::strcmp(mk66::testing::console_text(console::info(console::Cmd::Reset).help), "mute the outputs, then restart the unit") == 0);
}

TEST_CASE("mk66 text: a truncated, a corrupt or a wrongly sized stream is refused") {
  o3os::core::Inflater inf;
  static char out[mk66::txt::kRawSize + 8u];
  // the whole stream decodes
  CHECK(mk66::text_unpack(inf, mk66::kConsoleTextZ, mk66::kConsoleTextZSize, out, mk66::txt::kRawSize));
  // a truncated one does not
  CHECK_FALSE(mk66::text_unpack(inf, mk66::kConsoleTextZ, mk66::kConsoleTextZSize / 2u, out, mk66::txt::kRawSize));
  // a raw size that is too small or too large does not
  CHECK_FALSE(mk66::text_unpack(inf, mk66::kConsoleTextZ, mk66::kConsoleTextZSize, out, mk66::txt::kRawSize - 1u));
  CHECK_FALSE(mk66::text_unpack(inf, mk66::kConsoleTextZ, mk66::kConsoleTextZSize, out, mk66::txt::kRawSize + 1u));
  // garbage (a stored-block header with an impossible length complement) does not
  static const std::uint8_t kBad[] = {0x01, 0x05, 0x00, 0x00, 0x00, 'a', 'b', 'c', 'd', 'e'};
  CHECK_FALSE(mk66::text_unpack(inf, kBad, sizeof kBad, out, 5u));
  // and the decoder is reusable afterwards
  CHECK(mk66::text_unpack(inf, mk66::kBenchTextZ, mk66::kBenchTextZSize, out, mk66::btxt::kRawSize));
}
