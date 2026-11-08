// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S2c: the boot's set of quarantined .ptfx plugins (module/ptfx_quarantine.h, docs/specs/OS-CEPTFX.md A7): ids from ptfx_plugin_id, the cap of
// 16 with the full flag, a duplicate, the reasons, the boot load from a crash record's module word, and the set being apart from ModuleSet's .tmod
// quarantine. Host only (the console's lines are tests/mk66_usb_text_test.cpp).
#include <doctest/doctest.h>

#include <o3os/module/loader.h>
#include <o3os/module/ptfx_catalog.h>
#include <o3os/module/ptfx_quarantine.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace o3os::module;

TEST_CASE("ptfx_quarantine: an id is held once, with the reason it came in with") {
  PtfxQuarantine q;
  const std::uint16_t a = ptfx_plugin_id("gain.ptfx");
  const std::uint16_t b = ptfx_plugin_id("echo.ptfx");
  CHECK((a & 0x8000u) != 0u);
  CHECK(q.count() == 0u);
  CHECK_FALSE(q.contains(a));
  CHECK(q.quarantine(a, PtfxQuarantine::Why::Timeout) == PtfxQuarantine::Add::Added);
  CHECK(q.contains(a));
  CHECK_FALSE(q.contains(b));
  CHECK(q.quarantine(a, PtfxQuarantine::Why::Fault) == PtfxQuarantine::Add::Already);  // the first reason stays
  CHECK(q.count() == 1u);
  CHECK(q.why_of(a) == PtfxQuarantine::Why::Timeout);
  CHECK(q.quarantine(b, PtfxQuarantine::Why::StackOverflow) == PtfxQuarantine::Add::Added);
  CHECK(q.count() == 2u);
  CHECK(q.id(0) == a);
  CHECK(q.id(1) == b);
  CHECK(q.why(1) == PtfxQuarantine::Why::StackOverflow);
  CHECK(q.id(2) == 0u);  // out of range: 0, Crash, no overread
  CHECK(q.why(2) == PtfxQuarantine::Why::Crash);
  CHECK(std::strcmp(PtfxQuarantine::why_name(PtfxQuarantine::Why::Crash), "crash") == 0);
  CHECK(std::strcmp(PtfxQuarantine::why_name(PtfxQuarantine::Why::Timeout), "timeout") == 0);
  CHECK(std::strcmp(PtfxQuarantine::why_name(PtfxQuarantine::Why::Fault), "fault") == 0);
  CHECK(std::strcmp(PtfxQuarantine::why_name(PtfxQuarantine::Why::StackOverflow), "stack") == 0);
}

TEST_CASE("ptfx_quarantine: an id without bit 15 is no plugin id and is not kept") {
  PtfxQuarantine q;
  CHECK(q.quarantine(0x0000u, PtfxQuarantine::Why::Crash) == PtfxQuarantine::Add::BadId);
  CHECK(q.quarantine(0x7FFFu, PtfxQuarantine::Why::Crash) == PtfxQuarantine::Add::BadId);
  CHECK(q.count() == 0u);
  CHECK(q.refused() == 0u);
}

TEST_CASE("ptfx_quarantine: the first 16 stay, the 17th is refused and counted (the full flag)") {
  PtfxQuarantine q;
  std::uint16_t ids[PtfxQuarantine::kMax + 3];
  for (unsigned i = 0; i < PtfxQuarantine::kMax + 3u; ++i) {
    ids[i] = static_cast<std::uint16_t>(0x8000u | (i + 1u));  // distinct ids by construction (the hash is the catalogue's business)
  }
  for (unsigned i = 0; i < PtfxQuarantine::kMax; ++i) {
    CHECK(q.quarantine(ids[i], PtfxQuarantine::Why::Fault) == PtfxQuarantine::Add::Added);
  }
  CHECK(q.count() == 16u);
  CHECK(q.refused() == 0u);
  CHECK(q.quarantine(ids[16], PtfxQuarantine::Why::Fault) == PtfxQuarantine::Add::Full);
  CHECK(q.quarantine(ids[17], PtfxQuarantine::Why::Fault) == PtfxQuarantine::Add::Full);
  CHECK(q.count() == 16u);
  CHECK(q.refused() == 2u);
  CHECK_FALSE(q.contains(ids[16]));
  CHECK(q.contains(ids[0]));
  CHECK(q.contains(ids[15]));
  CHECK(q.quarantine(ids[3], PtfxQuarantine::Why::Crash) == PtfxQuarantine::Add::Already);  // a held id is no refusal when full
  CHECK(q.refused() == 2u);
}

TEST_CASE("ptfx_quarantine: the refused count saturates") {
  PtfxQuarantine q;
  for (unsigned i = 0; i < PtfxQuarantine::kMax; ++i) {
    (void)q.quarantine(static_cast<std::uint16_t>(0x8000u | (i + 1u)), PtfxQuarantine::Why::Crash);
  }
  for (unsigned i = 0; i < 400u; ++i) {
    (void)q.quarantine(static_cast<std::uint16_t>(0x9000u + i), PtfxQuarantine::Why::Crash);
  }
  CHECK(q.refused() == 255u);
  CHECK(q.count() == 16u);
}

TEST_CASE("ptfx_quarantine: the boot load puts a crash record's id (bit 15 set) in as Crash, a plain stage id stays out") {
  // boot_load is what make_modules calls with the record's module word: a word with bit 15 goes in; the word is 0 or a small number while no module ran.
  PtfxQuarantine q;
  const std::uint16_t crashed = ptfx_plugin_id("gain.ptfx");
  CHECK(q.boot_load(crashed));
  CHECK(q.contains(crashed));
  CHECK(q.why_of(crashed) == PtfxQuarantine::Why::Crash);
  CHECK(q.count() == 1u);
  CHECK_FALSE(q.boot_load(0u));
  CHECK_FALSE(q.boot_load(0x7FFFu));
  CHECK(q.boot_load(0x8000u));  // the lowest module word
  CHECK(q.boot_load(crashed));  // the same crash twice: still a module word, one entry
  CHECK(q.count() == 2u);
}

TEST_CASE("ptfx_quarantine: apart from ModuleSet's quarantine of .tmod stems, neither fills the other") {
  auto set = std::make_unique<ModuleSet>();
  PtfxQuarantine q;
  CHECK(set->quarantine("gain", Status::Quarantined));
  CHECK(set->quarantined("gain"));
  CHECK_FALSE(q.contains(ptfx_plugin_id("gain.ptfx")));  // a plugin of the same name is not quarantined by the module
  CHECK(q.quarantine(ptfx_plugin_id("echo.ptfx"), PtfxQuarantine::Why::Fault) == PtfxQuarantine::Add::Added);
  CHECK_FALSE(set->quarantined("echo"));  // nor the other way round
  CHECK(set->quarantine_count() == 1u);
  CHECK(q.count() == 1u);
}
