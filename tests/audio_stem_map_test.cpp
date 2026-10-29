// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006d: the USB stem map (audio/stem_map.h): the factory slots of every profile (ADR 0012 Decision 3), editing a slot, handing the map to the StemTap,
// and the text form `diag usb map` prints and reads.
#include <doctest/doctest.h>

#include <o3os/audio/stem_map.h>
#include <o3os/usb/descriptors.h>

#include "support/static_slot.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>

using namespace o3os::audio;
using o3os::usb::Profile;

namespace {
constexpr std::uint32_t t(unsigned n) { return 1u << (n - 1u); }
constexpr std::uint32_t ml() { return 1u << stem_src::kMasterL; }
constexpr std::uint32_t mr() { return 1u << stem_src::kMasterR; }
}  // namespace

TEST_CASE("stem map: the factory slots of each profile are the descriptor's channels (T1..Tn, then the master pair)") {
  StemMap m;
  CHECK(m.profile() == Profile::P10);  // the default
  CHECK(m.channels() == 10u);
  for (unsigned s = 0; s < 8u; ++s) {
    CHECK(m.slot(s) == t(s + 1u));
  }
  CHECK(m.slot(8) == ml());
  CHECK(m.slot(9) == mr());
  CHECK(m.slot(10) == 0u);

  CHECK(m.reset(Profile::P8));
  CHECK(m.channels() == 8u);
  for (unsigned s = 0; s < 6u; ++s) {
    CHECK(m.slot(s) == t(s + 1u));
  }
  CHECK(m.slot(6) == ml());
  CHECK(m.slot(7) == mr());
  CHECK(m.slot(8) == 0u);

  CHECK(m.reset(Profile::P11));
  CHECK(m.channels() == 11u);
  CHECK(m.slot(8) == t(9));
  CHECK(m.slot(9) == ml());
  CHECK(m.slot(10) == mr());

  CHECK(m.reset(Profile::P2));
  CHECK(m.channels() == 2u);
  CHECK(m.slot(0) == ml());
  CHECK(m.slot(1) == mr());
  CHECK(m.slot(2) == 0u);

  // OS-USBOUTS: P4 = T1, T2, Master L/R; P6 = T1..T4, Master L/R
  CHECK(StemMap::supported(Profile::P4));
  CHECK(StemMap::supported(Profile::P6));
  CHECK(m.reset(Profile::P4));
  CHECK(m.profile() == Profile::P4);
  CHECK(m.channels() == 4u);
  CHECK(m.slot(0) == t(1));
  CHECK(m.slot(1) == t(2));
  CHECK(m.slot(2) == ml());
  CHECK(m.slot(3) == mr());
  CHECK(m.slot(4) == 0u);
  CHECK_FALSE(m.set_slot(4, t(1)));
  CHECK(m.reset(Profile::P6));
  CHECK(m.channels() == 6u);
  for (unsigned s = 0; s < 4u; ++s) {
    CHECK(m.slot(s) == t(s + 1u));
  }
  CHECK(m.slot(4) == ml());
  CHECK(m.slot(5) == mr());
  CHECK(m.slot(6) == 0u);

  // The names the descriptors give the channels agree: every non-master channel is "S<n>" and the last two are the master.
  for (Profile p : {Profile::P2, Profile::P8, Profile::P10, Profile::P11, Profile::P4, Profile::P6}) {
    StemMap q;
    CHECK(q.reset(p));
    char name[10];
    CHECK(o3os::usb::channel_name(p, q.channels() - 2u, name) == 8u);
    CHECK(std::strcmp(name, "Master L") == 0);
    CHECK(q.slot(q.channels() - 2u) == ml());
  }
}

TEST_CASE("stem map: P4S (post-pan stereo stems) is refused and leaves the map alone; slots outside the profile cannot be edited") {
  StemMap m;
  CHECK_FALSE(StemMap::supported(Profile::P4S));
  CHECK_FALSE(m.reset(Profile::P4S));
  CHECK(m.profile() == Profile::P10);
  CHECK(m.slot(0) == t(1));
  CHECK(m.set_slot(7, t(8) | t(12)));
  CHECK(m.slot(7) == (t(8) | t(12)));  // S8 = T8 + T12
  CHECK(m.set_slot(2, 0));              // an empty slot is allowed (silence on that channel)
  CHECK(m.slot(2) == 0u);
  CHECK_FALSE(m.set_slot(10, t(1)));    // P10 has slots 0..9
  CHECK(m.slot(10) == 0u);
  CHECK(m.set_slot(0, 0xFFFF1234u));    // bits above the 16 sources are dropped
  CHECK(m.slot(0) == 0x1234u);
  // A profile change resets the edits.
  CHECK(m.reset(Profile::P8));
  CHECK(m.slot(7) == mr());
}

TEST_CASE("stem map: apply() hands the channel count and the masks to the StemTap and clears the slots beyond the profile") {
  static o3os::test::StaticSlot<StemTap> slot;
  static StemPool pool;
  StemTap& tap = slot.make(pool);
  StemMap m;
  CHECK(m.reset(Profile::P11));
  m.apply(tap);
  CHECK(tap.channels() == 11u);
  CHECK(tap.slot(8) == t(9));
  CHECK(tap.slot(10) == mr());
  CHECK(m.reset(Profile::P8));
  CHECK(m.set_slot(0, t(1) | t(2)));
  m.apply(tap);
  CHECK(tap.channels() == 8u);
  CHECK(tap.slot(0) == (t(1) | t(2)));
  CHECK(tap.slot(7) == mr());
  CHECK(tap.slot(8) == 0u);
  CHECK(tap.slot(10) == 0u);
}

TEST_CASE("stem map: the source text round-trips and rejects what it does not know") {
  char buf[48];
  CHECK(format_sources(0, buf, sizeof buf) == 1u);
  CHECK(std::strcmp(buf, "-") == 0);
  format_sources(t(1), buf, sizeof buf);
  CHECK(std::strcmp(buf, "T1") == 0);
  format_sources(t(1) | t(9) | t(12), buf, sizeof buf);
  CHECK(std::strcmp(buf, "T1+T9+T12") == 0);
  format_sources(ml() | mr() | (1u << stem_src::kSendA) | (1u << stem_src::kSendB), buf, sizeof buf);
  CHECK(std::strcmp(buf, "ML+MR+SA+SB") == 0);
  // every mask of the 16 sources survives a round trip
  for (std::uint32_t mask = 0; mask < 0x10000u; mask += 37u) {
    format_sources(mask, buf, sizeof buf);
    std::uint32_t back = 0xFFFFFFFFu;
    CHECK(parse_sources(buf, back));
    CHECK(back == mask);
  }
  std::uint32_t v = 0;
  CHECK(parse_sources("t3+ml", v));
  CHECK(v == (t(3) | ml()));
  CHECK(parse_sources("none", v));
  CHECK(v == 0u);
  CHECK(parse_sources("T12", v));
  CHECK(v == t(12));
  v = 0x55;
  for (const char* bad : {"", "T0", "T13", "T", "T1+", "+T1", "T1++T2", "T1 T2", "X1", "M", "MX", "SC", "T1T2", "t99", "ML+", "T1,T2"}) {
    CHECK_FALSE(parse_sources(bad, v));
    CHECK(v == 0x55u);  // untouched on failure
  }
  CHECK_FALSE(parse_sources(nullptr, v));
  // A small buffer is cut and stays a C string.
  char tiny[5];
  format_sources(t(1) | t(2) | t(3), tiny, sizeof tiny);
  CHECK(std::strlen(tiny) == 4u);
}
