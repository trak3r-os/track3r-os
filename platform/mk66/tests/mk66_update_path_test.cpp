// SPDX-License-Identifier: GPL-3.0-or-later
//
// The firmware writer's path guard (update/path_guard.h, OS-006c): accept and refuse tables, every single-character mutation of an accepted name, and
// the last guard on the full path (the three forms the writer may touch). Pure; runs on the host and on QEMU's Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/platform/mk66/update/path_guard.h>

#include <cstring>
#include <string>

namespace up = o3os::platform::mk66::update;

TEST_CASE("path guard: the names a finished image may get") {
  const char* const good[] = {"Track3r_0.3.2.ptf", "o3os-30e4de8.ptf", "x.PTF",   "x.Ptf", "a.ptf",   "0.ptf", "PolyendTracker_1.9.0.ptf",
                              "a-b_c.d.ptf",       "9-.ptf",           "A_.ptf",  "a.ptf.ptf", "ABCdef0123456789.ptf"};
  for (const char* name : good) {
    INFO(name);
    CHECK(up::valid_image_name(name));
    CHECK(up::readable_name(name));
  }
  // The longest stem, 48 characters, and one more.
  const std::string stem48(48, 'a');
  CHECK(up::valid_image_name((stem48 + ".ptf").c_str()));
  CHECK(!up::valid_image_name((stem48 + "a.ptf").c_str()));
  CHECK((stem48 + ".ptf").size() == up::kImageNameMax);
  const char* const bad[] = {"",
                             "x",
                             ".ptf",                      // empty stem
                             "..ptf",
                             ".a.ptf",                    // a leading dot
                             "_a.ptf",                    // the first character is alphanumeric
                             "-a.ptf",
                             "a.ptf ",                    // a trailing space
                             " a.ptf",
                             "a b.ptf",                   // a space inside
                             "a/b.ptf",                   // a separator
                             "a\\b.ptf",
                             "../a.ptf",
                             "a..b.ptf",                  // ".." in the stem
                             "a...ptf",                   // the stem ends in '.': "..ptf"
                             "a.ptf.ptf.bak",
                             "a.ptf\n",
                             "a\x01.ptf",                  // a control character
                             "a\x7F.ptf",
                             "a\xC3\xA9.ptf",              // not ASCII
                             "a:b.ptf",
                             "a*.ptf",
                             "a?.ptf",
                             "a\".ptf",
                             "a<.ptf",
                             "a>.ptf",
                             "a|.ptf",
                             "a+b.ptf",
                             "a~1.ptf",
                             "a.ptf/",
                             "/a.ptf",
                             "Firmware/a.ptf",
                             "_o3up.tmp",
                             "_fwinfo",
                             "a.hex",
                             "a.ptx",
                             "a.pt",
                             "a.txt",
                             "ptf",
                             "a"};
  for (const char* name : bad) {
    INFO(name);
    CHECK(!up::valid_image_name(name));
  }
  CHECK(!up::valid_image_name(nullptr));
  // Every printable and control character in the stem position, one at a time: accepted exactly for the class [0-9A-Za-z._-].
  for (unsigned c = 1; c < 256; ++c) {
    std::string name = "a";
    name += static_cast<char>(c);
    name += "b.ptf";
    const bool in_class = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-';
    INFO(c);
    CHECK(up::valid_image_name(name.c_str()) == in_class);
  }
}

TEST_CASE("path guard: removal is for the temporary file and names that do not look official") {
  CHECK(up::removable_name("_o3up.tmp"));
  CHECK(up::removable_name("PolyendTracker_0.1.ptf"));         // our old dev namespace
  CHECK(up::removable_name("PolyendTracker_0.3.1.ptf"));
  CHECK(up::removable_name("PolyendTracker_0.9.9-dev.ptf"));
  CHECK(up::removable_name("polyendtracker_0.1.ptf"));         // FAT does not tell the case apart
  CHECK(up::removable_name("Track3r_0.3.2.ptf"));
  CHECK(up::removable_name("o3os-30e4de8.ptf"));
  CHECK(up::removable_name("x.PTF"));
  CHECK(!up::removable_name("PolyendTracker_1.9.0.ptf"));      // a version Polyend published (or may)
  CHECK(!up::removable_name("polyendtracker_1.9.0.PTF"));
  CHECK(!up::removable_name("PolyendTracker_10.1.ptf"));       // 10.x is not 0.x
  CHECK(!up::removable_name("PolyendTracker_00.ptf"));
  CHECK(!up::removable_name("PolyendTracker_0.ptf"));          // "PolyendTracker_0." needs the dot of the dev namespace
  CHECK(!up::removable_name("PolyendTracker_0..ptf"));
  CHECK(!up::removable_name("PolyendTracker_a.ptf"));
  CHECK(!up::removable_name("Polyend.ptf"));                   // anything that starts "Polyend" outside the dev namespace
  CHECK(!up::removable_name("POLYEND-beta.ptf"));
  CHECK(!up::removable_name("Polyend_Tracker_0.1.ptf"));
  CHECK(!up::removable_name("_fwinfo"));
  CHECK(!up::removable_name("_o3up.tmp "));
  CHECK(!up::removable_name("_O3UP.TMP"));
  CHECK(!up::removable_name("Firmware"));
  CHECK(!up::removable_name(".o3-test-card"));
  CHECK(!up::removable_name(".ptf"));
  CHECK(!up::removable_name("a b.ptf"));
  CHECK(!up::removable_name("a/b.ptf"));
  CHECK(!up::removable_name(""));
  CHECK(!up::removable_name(nullptr));
  CHECK(!up::removable_name("../PolyendTracker_0.1.ptf"));
}

TEST_CASE("path guard: what may be read, built paths, and the .ptf test of a listing") {
  const std::string stem48(48, 'a');
  CHECK(up::readable_name("_o3up.tmp"));
  CHECK(up::readable_name("PolyendTracker_1.9.0.ptf"));
  CHECK(!up::readable_name("_fwinfo"));
  CHECK(!up::readable_name("project.mt"));
  CHECK(!up::readable_name(nullptr));
  char path[up::kPathMax];
  CHECK(up::build_path(path, sizeof path, "_o3up.tmp"));
  CHECK(std::strcmp(path, "Firmware/_o3up.tmp") == 0);
  CHECK(up::build_path(path, sizeof path, (stem48 + ".ptf").c_str()));
  char small[10];
  CHECK(!up::build_path(small, sizeof small, "_o3up.tmp"));
  CHECK(!up::build_path(nullptr, 0, "x"));
  CHECK(up::is_ptf_name("a.ptf"));
  CHECK(up::is_ptf_name("A.PTF"));
  CHECK(up::is_ptf_name("PolyendTracker_1.9.0.Ptf"));
  CHECK(!up::is_ptf_name("a.ptf.bak"));
  CHECK(!up::is_ptf_name("ptf"));
  CHECK(!up::is_ptf_name(""));
  CHECK(!up::is_ptf_name("_fwinfo"));
}

TEST_CASE("path guard: the last guard accepts exactly three paths") {
  CHECK(up::classify_write_path("Firmware/_o3up.tmp") == up::WriteKind::Temp);
  CHECK(up::classify_write_path("Firmware/_fwinfo") == up::WriteKind::FwInfo);
  CHECK(up::classify_write_path("Firmware/PolyendTracker_0.1.ptf") == up::WriteKind::Image);
  const char* const refused[] = {"",
                                 "Firmware",
                                 "Firmware/",
                                 "/Firmware/_o3up.tmp",
                                 "",
                                 "FIRMWARE/_o3up.tmp",
                                 "Firmware//_o3up.tmp",
                                 "Firmware/./_o3up.tmp",
                                 "Firmware/../_o3up.tmp",
                                 "Firmware/sub/_o3up.tmp",
                                 "Firmware/_o3up.tmp/",
                                 "Firmware/_o3up.tmp.",
                                 "Firmware/_o3up",
                                 "Firmware/_fwinfo2",
                                 "Firmware/_FWINFO",
                                 "Firmware/.ptf",
                                 "Firmware/a b.ptf",
                                 "Firmware/a/b.ptf",
                                 "Firmware/../x.ptf",
                                 "Firmware/readme.txt",
                                 "Firmware/.o3-test-card",
                                 "/.o3-test-card",
                                 ".o3-test-card",
                                 "Workspace/project.mt",
                                 "Projects/x/project.mt",
                                 "PolyendTracker_0.1.ptf",
                                 "Firmware"};
  for (const char* path : refused) {
    INFO(path);
    CHECK(up::classify_write_path(path) == up::WriteKind::None);
  }
  CHECK(up::classify_write_path(nullptr) == up::WriteKind::None);
  // Mutating one character of an accepted full path (to anything) keeps it accepted only when the result is itself one of the three forms.
  const std::string base = "Firmware/PolyendTracker_0.1.ptf";
  unsigned still_accepted = 0;
  for (std::size_t at = 0; at < base.size(); ++at) {
    for (unsigned c = 1; c < 256; ++c) {
      std::string m = base;
      m[at] = static_cast<char>(c);
      if (up::classify_write_path(m.c_str()) != up::WriteKind::None) {
        ++still_accepted;
        CHECK(m.compare(0, 9, "Firmware/") == 0);  // the directory part never changes
        CHECK(up::valid_image_name(m.c_str() + 9));
      }
    }
  }
  CHECK(still_accepted > 0u);  // the body characters can change within the class
  // Truncations of every accepted form are refused (they are prefixes, not names).
  for (const char* full : {"Firmware/_o3up.tmp", "Firmware/_fwinfo", "Firmware/PolyendTracker_0.1.ptf"}) {
    const std::string s = full;
    for (std::size_t n = 0; n < s.size(); ++n) {
      CHECK(up::classify_write_path(s.substr(0, n).c_str()) == up::WriteKind::None);
    }
  }
}

// OS-BUG-FWNAMELEN: the SD bootloader flashed the FIRST .ptf of Firmware/ for a 33-character name (observed 2026-10-31; 21, 29 and 30 installed). An update is never armed for a name over
// kUpdateNameMax (30, ".ptf" included); such a file is still a valid image name (it may be stored, listed and read), only update_name_ok says no.
TEST_CASE("path guard: an update is armed for names of at most 30 characters, 31 is refused, longer names stay storable") {
  static_assert(up::kUpdateNameMax == 30, "the longest name proven on the unit");
  const auto name_of = [](std::size_t total) { return std::string(total - up::kSuffixLen, 'a') + ".ptf"; };
  for (const std::size_t n : {5u, 21u, 29u, 30u}) {
    INFO(n);
    CHECK(name_of(n).size() == n);
    CHECK(up::update_name_ok(name_of(n).c_str()));
  }
  for (const std::size_t n : {31u, 33u, 40u, 52u}) {
    INFO(n);
    CHECK(name_of(n).size() == n);
    CHECK(up::valid_image_name(name_of(n).c_str()));  // still storable and readable ...
    CHECK(up::readable_name(name_of(n).c_str()));
    CHECK(!up::update_name_ok(name_of(n).c_str()));   // ... never armed
  }
  // The names of the unit's observation.
  CHECK(up::update_name_ok("o3os_1.0.1-stbase.1-bench.ptf"));          // 29: installed
  CHECK(up::update_name_ok("o3os_1.0.1-stereo2.1-bench.ptf"));         // 30: installed
  CHECK(!up::update_name_ok("o3os_1.0.1-stereobase.1-bench.ptf"));     // 33: flashed the first .ptf instead
  // A name the guard refuses stays refused whatever its length.
  CHECK(!up::update_name_ok("a b.ptf"));
  CHECK(!up::update_name_ok("a..b.ptf"));
  CHECK(!up::update_name_ok("_o3up.tmp"));
  CHECK(!up::update_name_ok(""));
  CHECK(!up::update_name_ok(nullptr));
}
