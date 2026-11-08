// SPDX-License-Identifier: GPL-3.0-or-later
//
// The register values the boot code writes for the clock tree and the SDRAM controller (clock_config.h, sdramc_config.h):
// pinned to the values the boot code programs, with the arithmetic that leads to them, and the pin table against an
// independent copy with the signal names. The values are facts about the board; a change to one of them is a change to
// hardware behaviour and must change a test here on purpose.
#include <doctest/doctest.h>

#include <o3os/platform/mk66/board.h>
#include <o3os/platform/mk66/clock_config.h>
#include <o3os/platform/mk66/mem_map.h>
#include <o3os/platform/mk66/sdramc_config.h>

#include <cstdint>

namespace mk66 = o3os::platform::mk66;
namespace clock_cfg = o3os::platform::mk66::clock;
namespace sdram = o3os::platform::mk66::sdram;

TEST_CASE("mk66 clock: 16 MHz crystal to 180 MHz") {
  CHECK(clock_cfg::kCrystalHz == 16'000'000u);
  CHECK(clock_cfg::kPrdiv == 1);   // MCG_C5[PRDIV]: / 2
  CHECK(clock_cfg::kVdiv == 29);   // MCG_C6[VDIV]: x 45
  CHECK(clock_cfg::kFrdiv == 4);   // 16 MHz / 512 = 31.25 kHz for the FLL reference
  CHECK(clock_cfg::pll_reference_hz() == 8'000'000u);
  CHECK(clock_cfg::vco_hz() == 360'000'000u);
  CHECK(clock_cfg::kCoreHz == 180'000'000u);
  CHECK(clock_cfg::kBusHz == 90'000'000u);
  CHECK(clock_cfg::kFlexBusHz == 90'000'000u);
  CHECK(clock_cfg::kFlashHz == 25'714'285u);
  // 16 MHz / 512 is within the FLL reference window of 31.25 .. 39.0625 kHz.
  CHECK(clock_cfg::kCrystalHz / 512u == 31'250u);
  // SIM_CLKDIV1: 0x01060000 from the startup, with OUTDIV3(1) added by the SDRAM bring-up.
  CHECK(clock_cfg::kClkdiv1 == 0x01160000u);
  CHECK((clock_cfg::kClkdiv1 & 0x01060000u) == 0x01060000u);
}

TEST_CASE("mk66 clock: SysTick is a whole 1 kHz") {
  CHECK(clock_cfg::kSysTickReload == 179'999u);
  CHECK((clock_cfg::kSysTickReload + 1u) * clock_cfg::kSysTickHz == clock_cfg::kCoreHz);
  CHECK(clock_cfg::kSysTickReload <= 0x00FFFFFFu);
}

TEST_CASE("mk66 sdram: refresh arithmetic") {
  // 4096 rows in 64 ms is 15.625 us per row; at 90 MHz the controller refreshes every (RC + 1) * 16 clocks.
  CHECK(sdram::kRefreshRowNs == 15'625u);
  CHECK(sdram::kRc == 86u);
  const std::uint32_t interval_clocks = (sdram::kRc + 1u) * 16u;
  CHECK(interval_clocks == 1392u);
  CHECK(interval_clocks * 1'000'000u / (clock_cfg::kFlexBusHz / 1000u) <= sdram::kRefreshRowNs);  // not longer than the row time (ns)
  CHECK(sdram::refresh_count(90'000'000u, 15'625u) == 86u);
  CHECK(sdram::refresh_count(60'000'000u, 15'625u) == 57u);  // the formula tracks the clock
  CHECK(sdram::refresh_count(90'000'000u, 7'812u) == 42u);   // and the row time
}

TEST_CASE("mk66 sdram: the controller registers, word by word") {
  CHECK(sdram::kCtrlAddress == 0x4000F042u);
  CHECK(sdram::kAc0Address == 0x4000F048u);
  CHECK(sdram::kCm0Address == 0x4000F04Cu);
  CHECK(sdram::kCtrl == 0x0256u);        // RC 86, RTIM six clocks
  CHECK(sdram::kAc0 == 0x70003320u);     // base 0x70000000, CASL 3, CBM 3 (bit 20), PS 16-bit
  CHECK(sdram::kCm0 == 0x00FC0001u);     // 16 MiB, valid
  CHECK(sdram::kAcIp == 0x0008u);
  CHECK(sdram::kAcImrs == 0x0040u);
  CHECK(sdram::kAcRe == 0x8000u);
  // Field by field, so a wrong shift is not hidden in a sum.
  CHECK(((sdram::kAc0 >> 4) & 3u) == 2u);
  CHECK(((sdram::kAc0 >> 8) & 7u) == 3u);
  CHECK(((sdram::kAc0 >> 12) & 3u) == 3u);
  CHECK((sdram::kAc0 & 0xFFFC0000u) == mk66::mem::kSdramBase);
  CHECK((sdram::kCtrl & 0x1FFu) == 86u);
  CHECK(((sdram::kCtrl >> 9) & 3u) == 1u);
  CHECK((sdram::kCtrl & 0x800u) == 0u);  // not in self-refresh
}

TEST_CASE("mk66 sdram: the block mask covers 16 MiB and nothing else") {
  // CM[BAM] bits 23:18 set: those six address bits are not compared, so the block spans 2^24 bytes.
  CHECK((sdram::kBlockMask >> 18) == 0x3Fu);
  CHECK((1u << 24) == mk66::mem::kSdramSize);
  CHECK((sdram::kCm0 & 1u) == 1u);       // valid
  CHECK((sdram::kCm0 & 0x100u) == 0u);   // not write-protected
}

TEST_CASE("mk66 sdram: mode register word and where it is presented") {
  // Wiring of the SDRAM's A0..A13 to the controller's address bits (the PCB's).
  const std::uint8_t wiring[14] = {16, 15, 14, 13, 12, 11, 10, 9, 18, 19, 20, 21, 22, 23};
  for (unsigned i = 0; i < 14; ++i) {
    CHECK(sdram::kPinToAddressBit[i] == wiring[i]);
  }
  CHECK(sdram::kCommandBit == 20u);      // the SDRAM's A10 (precharge-all select) is controller address bit 20
  CHECK(sdram::kCbm == 3u);
  // Mode register: burst 1, sequential, CAS 3, standard, programmed burst: only A4 and A5 are set (CAS latency 3 = 011).
  CHECK(sdram::kModeRegister == 0x30u);
  CHECK(sdram::mode_register_offset(0x30u) == ((1u << 12) | (1u << 11)));
  CHECK(sdram::kMrsAddress == 0x70001800u);
  CHECK(sdram::mode_register_offset(0u) == 0u);
  CHECK(sdram::mode_register_offset(1u << 10) == 0u);   // the mode word is A0..A9: A10 (controller bit 20, the command bit) is never part of it
  CHECK(sdram::mode_register_offset(1u << 9) == (1u << 19));  // A9 (write burst mode) is wired to controller bit 19
  // The mode word never touches the command bit (A10): a precharge-all must not be mistaken for a mode set.
  CHECK((sdram::kMrsAddress & (1u << sdram::kCommandBit)) == 0u);
  CHECK(sdram::kCommandAccessValue == 0xAAu);
}

TEST_CASE("mk66 sdram: waits are long enough, and the refresh settle covers eight intervals") {
  CHECK(sdram::kPowerUpDelayMs == 10u);
  CHECK(sdram::kClockStableDelayUs >= 100u);
  const std::uint32_t eight_refreshes_us = 8u * (sdram::kRc + 1u) * 16u / (clock_cfg::kFlexBusHz / 1'000'000u);
  CHECK(eight_refreshes_us == 123u);
  CHECK(sdram::kRefreshSettleDelayUs > eight_refreshes_us);
}

TEST_CASE("mk66 sdram: FlexBus system settings") {
  CHECK(sdram::kSimSopt2Fbsl == 0x300u);
  CHECK(sdram::kSimSopt2ClkoutFlexBus == 0u);
  CHECK(sdram::kCspmcrMask == 0x0FFFF000u);
  CHECK(sdram::kCspmcrValue == 0x02222000u);
  CHECK((sdram::kCspmcrValue & ~sdram::kCspmcrMask) == 0u);
}

TEST_CASE("mk66 sdram: the 39 pins, against an independent list with their signal names") {
  struct Named {
    mk66::Port port;
    std::uint8_t bit;
    const char* signal;
  };
  using P = mk66::Port;
  const Named expected[] = {
      {P::A, 6, "CLKOUT"},    {P::B, 0, "CAS_b"},    {P::B, 1, "RAS_b"},    {P::B, 2, "WE"},      {P::B, 3, "CS0_b"},
      {P::C, 16, "DQM2"},     {P::C, 17, "DQM3"},    {P::D, 7, "CKE"},
      {P::B, 18, "A23"},      {P::C, 0, "A22"},      {P::C, 1, "A21"},      {P::C, 2, "A20"},     {P::C, 4, "A19"},
      {P::C, 5, "A18"},       {P::C, 6, "A17"},      {P::C, 7, "A16"},      {P::C, 8, "A15"},     {P::C, 9, "A14"},
      {P::C, 10, "A13"},      {P::D, 2, "A12"},      {P::D, 3, "A11"},      {P::D, 4, "A10"},     {P::D, 5, "A9"},
      {P::B, 20, "D31"},      {P::B, 21, "D30"},     {P::B, 22, "D29"},     {P::B, 23, "D28"},    {P::C, 12, "D27"},
      {P::C, 13, "D26"},      {P::C, 14, "D25"},     {P::C, 15, "D24"},     {P::B, 6, "D23"},     {P::B, 7, "D22"},
      {P::B, 8, "D21"},       {P::B, 9, "D20"},      {P::B, 10, "D19"},     {P::B, 11, "D18"},    {P::B, 16, "D17"},
      {P::B, 17, "D16"},
  };
  CHECK(sdram::kPinCount == sizeof(expected) / sizeof(expected[0]));
  CHECK(sdram::kPinMux == 5u);
  if (sdram::kPinCount != sizeof(expected) / sizeof(expected[0])) {
    return;
  }
  for (unsigned i = 0; i < sdram::kPinCount; ++i) {
    INFO("signal " << expected[i].signal);
    CHECK(sdram::kPins[i].port == expected[i].port);
    CHECK(sdram::kPins[i].bit == expected[i].bit);
    CHECK(sdram::kPins[i].bit < 32);
  }
  // No pin twice.
  for (unsigned i = 0; i < sdram::kPinCount; ++i) {
    for (unsigned j = i + 1; j < sdram::kPinCount; ++j) {
      CHECK_FALSE(sdram::kPins[i] == sdram::kPins[j]);
    }
  }
}

TEST_CASE("mk66 board: the pins that are not part of the SDRAM interface") {
  // None of them is one of the 39 SDRAM pins (a GPIO claim must never collide with the FlexBus), and none is claimed twice.
  const mk66::Pin gpio[] = {mk66::board::kStatusLed,        mk66::board::kAudioMute,      mk66::board::kSdramPower,
                            mk66::board::kBootloaderRequest, mk66::board::kFt812ChipSelect, mk66::board::kFt812PowerDown,
                            mk66::board::kInputSourceSelect, mk66::board::kRadioReset,      mk66::board::kMidiTx};
  const unsigned count = sizeof(gpio) / sizeof(gpio[0]);
  for (unsigned i = 0; i < count; ++i) {
    for (unsigned j = 0; j < sdram::kPinCount; ++j) {
      CHECK_FALSE(gpio[i] == sdram::kPins[j]);
    }
    for (unsigned j = i + 1; j < count; ++j) {
      CHECK_FALSE(gpio[i] == gpio[j]);
    }
    CHECK(gpio[i].bit < 32);
  }
  const mk66::Pin led{mk66::Port::C, 19};
  const mk66::Pin mute{mk66::Port::D, 6};
  const mk66::Pin power{mk66::Port::A, 24};
  const mk66::Pin boot{mk66::Port::A, 1};
  CHECK(mk66::board::kStatusLed == led);
  CHECK(mk66::board::kAudioMute == mute);
  CHECK(mk66::board::kSdramPower == power);
  CHECK(mk66::board::kBootloaderRequest == boot);
  CHECK(mk66::board::kStatusLedActiveLow);
}

TEST_CASE("mk66 board: the power key is driven first, high, with high drive and a limited slew (OS-BUG-PADS step 3)") {
  // boot::early_pins() = rail_on() (this pin, this PCR), then kQuietPins in order: the mute key is the first of them. The order itself is code
  // (boot.cpp), reviewed there; this pins the data it uses. Untested on both units.
  const std::uint32_t pcr = mk66::board::kSdramPowerPcr;
  // The K66 port control register's fields, written out again here: PS 0, PE 1, SRE 2, PFE 4, ODE 5, DSE 6, MUX 10:8, LK 15, IRQC 19:16.
  CHECK(((pcr >> 8) & 7u) == 1u);   // GPIO
  CHECK(((pcr >> 6) & 1u) == 1u);   // DSE: high drive
  CHECK(((pcr >> 2) & 1u) == 1u);   // SRE: slew-rate limited
  CHECK((pcr & ~((7u << 8) | (1u << 6) | (1u << 2))) == 0u);  // no pull, no filter, no open drain, not locked, no interrupt
  CHECK(pcr == 0x144u);
  // The key is not one of the quiet pins (those are re-driven by the power-off and the audio tests as a table; the key has its own owners),
  // and the quiet table still starts with the mute key, LOW: the second pin action of the reset.
  for (unsigned i = 0; i < mk66::board::kQuietPinCount; ++i) {
    CHECK_FALSE(mk66::board::kQuietPins[i].pin == mk66::board::kSdramPower);
  }
  CHECK(mk66::board::kQuietPins[0].pin == mk66::board::kAudioMute);
  CHECK_FALSE(mk66::board::kQuietPins[0].high);
}

TEST_CASE("mk66 board: only the LOW mute key goes before the rail's settle wait, every HIGH quiet pin after it (OS-BUG-PADS)") {
  // boot::early_pins(): rail_on(), the first kQuietPinsAtOnce entries, the wait, then the rest in table order (the loops are code, boot.cpp;
  // this pins the data they split on). A HIGH pin driven before the wait would go into a rail that may still be ramping.
  using mk66::board::kQuietPins;
  CHECK(mk66::board::kQuietPinsAtOnce == 1u);
  CHECK(mk66::board::kQuietPinsAtOnce < mk66::board::kQuietPinCount);
  for (unsigned i = 0; i < mk66::board::kQuietPinsAtOnce && i < mk66::board::kQuietPinCount; ++i) {
    CHECK_FALSE(kQuietPins[i].high);
  }
  // The wait: 2 ms (the review's figure; the rail's real ramp is not measured), counted on the reset clock (20.97 MHz) as boot.cpp
  // computes it: 41943 cycles, a constant that fits the 32-bit cycle counter's wrap-safe compare.
  CHECK(mk66::board::kRailSettleUs == 2000u);
  const std::uint64_t cycles = (std::uint64_t{clock_cfg::kFeiHz} * mk66::board::kRailSettleUs) / 1'000'000u;
  CHECK(cycles == 41943u);
  CHECK(cycles < 0x80000000ull);
}

TEST_CASE("mk66 board: the quiet state the reset sequence drives after the power key") {
  // Teensy pin numbers as port and bit: 54 = PTD15, 55 = PTD11, 24 = PTE26, 67 = PTE27,
  // 33 = PTE24. Written out again here as the independent copy.
  const mk66::Pin cs{mk66::Port::D, 15};
  const mk66::Pin pd{mk66::Port::D, 11};
  const mk66::Pin source{mk66::Port::E, 26};
  const mk66::Pin radio{mk66::Port::E, 27};
  const mk66::Pin midi{mk66::Port::E, 24};
  CHECK(mk66::board::kFt812ChipSelect == cs);
  CHECK(mk66::board::kFt812PowerDown == pd);
  CHECK(mk66::board::kInputSourceSelect == source);
  CHECK(mk66::board::kRadioReset == radio);
  CHECK(mk66::board::kMidiTx == midi);

  using mk66::board::kQuietPins;
  CHECK(mk66::board::kQuietPinCount == 7u);
  if (mk66::board::kQuietPinCount != 7u) {
    return;
  }
  // The amplifier mute is first of the quiet pins (right after the power key), and is LOW (muted).
  CHECK(kQuietPins[0].pin == mk66::board::kAudioMute);
  CHECK_FALSE(kQuietPins[0].high);
  // Level of each, as driven first.
  for (unsigned i = 0; i < mk66::board::kQuietPinCount; ++i) {
    const mk66::Pin p = kQuietPins[i].pin;
    const bool high = kQuietPins[i].high;
    if (p == mk66::board::kAudioMute) CHECK_FALSE(high);
    if (p == mk66::board::kStatusLed) CHECK(high == mk66::board::kStatusLedActiveLow);  // off
    if (p == mk66::board::kInputSourceSelect) CHECK_FALSE(high);                        // the receiver's position, the receiver in reset (OS-011c)
    if (p == mk66::board::kRadioReset) CHECK_FALSE(high);                               // held in reset
    if (p == mk66::board::kFt812ChipSelect) CHECK(high);                                // deselected
    if (p == mk66::board::kFt812PowerDown) CHECK(high);                                 // standby
    if (p == mk66::board::kMidiTx) CHECK(high);                                         // UART idle
    // Not the SDRAM power key, the bootloader request line or an SDRAM pin: those have their own owners.
    CHECK_FALSE(p == mk66::board::kSdramPower);
    CHECK_FALSE(p == mk66::board::kBootloaderRequest);
    for (unsigned j = 0; j < sdram::kPinCount; ++j) {
      CHECK_FALSE(p == sdram::kPins[j]);
    }
    for (unsigned j = i + 1; j < mk66::board::kQuietPinCount; ++j) {
      CHECK_FALSE(p == kQuietPins[j].pin);
    }
  }
}

// OS-PERF3: the ICode alias. The controller compares the unmasked bits of AC0[BA]; the alias adds don't-care bits 30:27 so that 0x0800_0000 reaches the same SDRAM as 0x7000_0000.
TEST_CASE("mk66 sdram: the ICode alias mask makes 0x0800_0000 and 0x7000_0000 the same bytes and moves nothing else") {
  namespace mem = o3os::platform::mk66::mem;
  const auto hits = [](std::uint32_t address) {
    const std::uint32_t ba = sdram::kAc0 & sdram::kBaMask;
    const std::uint32_t bam = sdram::kCm0WithAlias & sdram::kBaMask;
    return ((address ^ ba) & ~bam & sdram::kBaMask) == 0u;
  };
  CHECK(sdram::kCm0WithAlias == 0x78FC0001u);
  CHECK((sdram::kCm0WithAlias & 1u) == 1u);  // still valid, not write protected
  CHECK((sdram::kCm0WithAlias & 0x100u) == 0u);
  CHECK(mem::kSdramAliasBase == 0x08000000u);
  // Where it answers: all 16 MiB at the system-bus address, all 16 MiB at the alias.
  CHECK(hits(mem::kSdramBase));
  CHECK(hits(mem::kSdramBase + mem::kSdramSize - 1u));
  CHECK(hits(mem::kSdramAliasBase));
  CHECK(hits(mem::kSdramAliasBase + mem::kSdramSize - 1u));
  CHECK(mem::sdram_alias(mem::kSampleArenaBase) == 0x08000000u);
  CHECK(mem::sdram_alias(mem::kUiCacheBase) == 0x08F50000u);  // OS-CEPTFX S2a: ui_cache keeps its last 448 KiB from 0x70F50000
  CHECK(mem::sdram_alias(mem::kPtfxBase) == 0x08E90000u);
  CHECK(mem::sdram_alias(mem::kSdramBase + mem::kSdramSize - 4u) == 0x08FFFFFCu);
  // Where it does not: the end of the block (the next 16 MiB) at either address, the neighbouring regions.
  CHECK_FALSE(hits(mem::kSdramBase + mem::kSdramSize));
  CHECK_FALSE(hits(mem::kSdramAliasBase + mem::kSdramSize));
  CHECK_FALSE(hits(0x88000000u));  // bit 31 is still compared: the RM's other alias address is not this block's
  CHECK_FALSE(hits(0xE0000000u));
  // The controller only sees what the crossbar sends it (RM Table 5-1): the SDRAM alias window, the FlexBus alias window and 0x6000_0000-0xDFFF_FFFF. Walk those in 16 MiB blocks: the
  // blocks that hit are the 0x7000_0000 one, the ICode alias, and four more addresses of the same bytes, none of which the OS uses (the FlexBus chip selects are not programmed).
  struct Window {
    std::uint32_t first;
    std::uint32_t last;
  };
  const Window windows[] = {{0x08000000u, 0x0FFFFFFFu}, {0x18000000u, 0x1BFFFFFFu}, {0x60000000u, 0xDFFFFFFFu}};
  std::uint32_t hit_blocks[8] = {};
  unsigned n = 0;
  for (const Window& w : windows) {
    for (std::uint64_t a = w.first; a <= w.last; a += mem::kSdramSize) {
      if (hits(static_cast<std::uint32_t>(a))) {
        if (n < 8u) {
          hit_blocks[n] = static_cast<std::uint32_t>(a);
        }
        ++n;
      }
    }
  }
  CHECK(n == 6u);
  CHECK(hit_blocks[0] == 0x08000000u);  // the ICode alias
  CHECK(hit_blocks[1] == 0x18000000u);  // the FlexBus alias window
  CHECK(hit_blocks[2] == 0x60000000u);
  CHECK(hit_blocks[3] == 0x68000000u);
  CHECK(hit_blocks[4] == 0x70000000u);
  CHECK(hit_blocks[5] == 0x78000000u);
  // The alias is in the code-bus region R1 of the LMEM table, where the cache can hold it.
  CHECK(mem::kSdramAliasBase + mem::kSdramSize <= 0x10000000u);
}
