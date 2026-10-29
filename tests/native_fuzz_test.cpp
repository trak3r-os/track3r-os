// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/iff.h"
#include "support/memory_io.h"
#include "support/project_fixture.h"
#include "support/rng.h"

#include <o3os/formats/native.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using Bytes = std::vector<std::uint8_t>;

namespace {

// O3OS_FUZZ_SCALE=<n> multiplies the random-mutation run counts (default 1, about 35 s under ASan + UBSan):
// a longer unattended run, e.g. O3OS_FUZZ_SCALE=20 build-os-host/tests/o3os_tests -tc="native fuzz*".
// Under ThreadSanitizer (this code is single-threaded, the unit test there only has to run) the loops are
// thinned out to a twentieth.
#if defined(__SANITIZE_THREAD__)
constexpr std::uint32_t kThin = 20;
#else
constexpr std::uint32_t kThin = 1;
#endif

std::uint32_t scale() {
  const char* e = std::getenv("O3OS_FUZZ_SCALE");
  const long v = e != nullptr ? std::strtol(e, nullptr, 10) : 1;
  return v < 1 ? 1u : (v > 1000 ? 1000u : static_cast<std::uint32_t>(v));
}

std::uint32_t runs(std::uint32_t base) { return base * scale() / kThin; }

struct Tally {
  std::uint32_t runs = 0;
  std::uint32_t done = 0;
  std::uint32_t failed = 0;
  std::uint32_t by_error[16] = {};
};

// The properties every input must have, whatever the bytes: the reader ends in Done or Failed (the sanitisers
// watch for any out-of-bounds access or undefined behaviour on the way), a Failed load leaves the empty
// default project, a Done load leaves a project that satisfies the model invariants and saves, and what it
// saves loads back to a file that saves to the same bytes.
void run_one(const Bytes& input, std::size_t budget, Tally& t) {
  ++t.runs;
  model::ModelArena& a = test::arena(0);
  Report rep;
  const Progress pr = test::load(a, input, &rep, budget);
  NEED((pr == Progress::Done || pr == Progress::Failed));
  if (pr == Progress::Failed) {
    ++t.failed;
    ++t.by_error[static_cast<unsigned>(rep.error) & 15u];
    NEED(rep.error != Error::None);
    NEED(test::is_default_empty(a.project));
    return;
  }
  ++t.done;
  NEED(a.project.check_invariants());
  Bytes once;
  NEED(test::save(a.project, once) == Progress::Done);
  NEED(test::load(test::arena(1), once) == Progress::Done);
  Bytes twice;
  NEED(test::save(test::arena(1).project, twice) == Progress::Done);
  NEED(once == twice);
}

}  // namespace

TEST_CASE("native fuzz: every byte of a small file set to a few values, with and without the CRC repaired") {
  test::fill_small(test::arena(2).project);
  Bytes base;
  NEED(test::save(test::arena(2).project, base) == Progress::Done);
  Tally t;
  const std::uint8_t values[] = {0x00, 0xFF, 0x80};
  for (std::size_t i = 0; i < base.size(); i += kThin) {  // every byte (every 20th under TSan)
    for (std::uint8_t v : values) {
      Bytes m = base;
      m[i] = v;
      test::fix_up(m);
      run_one(m, 4096, t);
    }
    {
      Bytes m = base;
      m[i] = static_cast<std::uint8_t>(base[i] ^ 0x10);
      run_one(m, i % 16 == 0 ? 1 : 4096, t);  // not repaired: mostly BadCrc, after everything was parsed
    }
  }
  MESSAGE("byte sweep: " << t.runs << " runs, " << t.done << " loaded, " << t.failed << " rejected");
  CHECK(t.done > 0);
  CHECK(t.failed > 0);
}

TEST_CASE("native fuzz: deterministic random mutations of a small file and of a few chunks") {
  test::fill_small(test::arena(2).project);
  Bytes base;
  NEED(test::save(test::arena(2).project, base) == Progress::Done);
  test::Rng r(0xC0FFEE11u);
  Tally t;
  const std::uint32_t kRuns = runs(25000);
  const std::size_t budgets[] = {1, 3, 64, 4096};
  for (std::uint32_t n = 0; n < kRuns; ++n) {
    Bytes m = base;
    const std::uint32_t edits = 1 + r.below(4);
    for (std::uint32_t e = 0; e < edits && !m.empty(); ++e) {
      const std::size_t at = r.below(static_cast<std::uint32_t>(m.size()));
      switch (r.below(9)) {
        case 0: m[at] = static_cast<std::uint8_t>(m[at] ^ (1u << r.below(8))); break;
        case 1: m[at] = r.byte(); break;
        case 2: {
          const std::uint8_t special[] = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};
          m[at] = special[r.below(6)];
          break;
        }
        case 3: m.erase(m.begin() + static_cast<std::ptrdiff_t>(at)); break;
        case 4: m.insert(m.begin() + static_cast<std::ptrdiff_t>(at), r.byte()); break;
        case 5: m.resize(at); break;
        case 6: {  // copy a slice somewhere else
          const std::size_t len = std::min<std::size_t>(r.below(64), m.size() - at);
          const Bytes slice(m.begin() + static_cast<std::ptrdiff_t>(at), m.begin() + static_cast<std::ptrdiff_t>(at + len));
          const std::size_t to = r.below(static_cast<std::uint32_t>(m.size()));
          m.insert(m.begin() + static_cast<std::ptrdiff_t>(to), slice.begin(), slice.end());
          break;
        }
        case 7: {  // a 32-bit word with an interesting value: lengths and counts
          const std::uint32_t words[] = {0u, 1u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x00010000u, 0x0000FFFFu,
                                         static_cast<std::uint32_t>(m.size())};
          const std::uint32_t w = words[r.below(8)];
          for (std::size_t k = 0; k < 4 && at + k < m.size(); ++k) {
            m[at + k] = static_cast<std::uint8_t>(w >> (24 - 8 * k));
          }
          break;
        }
        default: {  // swap two bytes
          const std::size_t other = r.below(static_cast<std::uint32_t>(m.size()));
          std::swap(m[at], m[other]);
          break;
        }
      }
    }
    if (r.below(2) == 0) {
      test::fix_up(m);  // half of them get past the FORM length and the CRC
    }
    run_one(m, budgets[r.below(4)], t);
  }
  MESSAGE("random mutations: " << t.runs << " runs, " << t.done << " loaded, " << t.failed << " rejected");
  CHECK(t.runs == kRuns);
  CHECK(t.failed > 0);
  CHECK(t.by_error[static_cast<unsigned>(Error::Truncated)] > 0);
  CHECK(t.by_error[static_cast<unsigned>(Error::BadCrc)] > 0);
  CHECK(t.by_error[static_cast<unsigned>(Error::Malformed)] > 0);
}

TEST_CASE("native fuzz: chunk-level mutations reach the parsers behind the CRC") {
  test::fill_small(test::arena(2).project);
  Bytes base;
  NEED(test::save(test::arena(2).project, base) == Progress::Done);
  const std::vector<test::RawChunk> chunks = test::split_chunks(base);
  test::Rng r(0xFEED5EEDu);
  Tally t;
  const std::uint32_t kRuns = runs(8000);
  for (std::uint32_t n = 0; n < kRuns; ++n) {
    std::vector<test::RawChunk> cs = chunks;
    const std::uint32_t edits = 1 + r.below(3);
    for (std::uint32_t e = 0; e < edits; ++e) {
      test::RawChunk& c = cs[r.below(static_cast<std::uint32_t>(cs.size()))];
      switch (r.below(5)) {
        case 0:
          if (!c.payload.empty()) c.payload[r.below(static_cast<std::uint32_t>(c.payload.size()))] = r.byte();
          break;
        case 1:
          if (!c.payload.empty()) c.payload.resize(r.below(static_cast<std::uint32_t>(c.payload.size())));
          break;
        case 2:
          for (std::uint32_t k = r.below(8); k > 0; --k) c.payload.push_back(r.byte());
          break;
        case 3:
          std::swap(c, cs[r.below(static_cast<std::uint32_t>(cs.size()))]);
          break;
        default:
          cs.insert(cs.begin() + r.below(static_cast<std::uint32_t>(cs.size())), c);
          break;
      }
    }
    test::BuildOptions o;
    o.pad_byte = r.below(16) == 0 ? 1 : 0;
    run_one(test::build_file(cs, o), r.below(2) == 0 ? 4096 : 7, t);
  }
  MESSAGE("chunk mutations: " << t.runs << " runs, " << t.done << " loaded, " << t.failed << " rejected");
  CHECK(t.done > 0);
  CHECK(t.failed > 0);
}

// OS-004d: the same properties over a file that has the project-level state (MIDI, USBS, MIXR, PERF, APPM and INST 1.2, and the LAW chunk of OS-BUG-PASTE24), so that their decoders get byte-level and chunk-level fuzzing.
TEST_CASE("native fuzz: a file with the project-level state chunks: every byte, random mutations and chunk-level mutations") {
  test::fill_state(test::arena(2).project);
  test::arena(2).project.set_volume_law(1);  // OS-BUG-PASTE24: the file also holds the LAW chunk
  Bytes base;
  NEED(test::save(test::arena(2).project, base) == Progress::Done);
  Tally t;
  const std::uint8_t values[] = {0x00, 0xFF, 0x80, 0x01, 0x14};
  for (std::size_t i = 0; i < base.size(); i += kThin) {
    for (std::uint8_t v : values) {
      Bytes m = base;
      m[i] = v;
      test::fix_up(m);
      run_one(m, 4096, t);
    }
  }
  MESSAGE("state byte sweep: " << t.runs << " runs, " << t.done << " loaded, " << t.failed << " rejected");
  CHECK(t.done > 0);
  CHECK(t.failed > 0);
  const std::vector<test::RawChunk> chunks = test::split_chunks(base);
  test::Rng r(0x5747E004u);
  Tally c;
  for (std::uint32_t n = 0; n < runs(6000); ++n) {
    std::vector<test::RawChunk> cs = chunks;
    for (std::uint32_t e = 1 + r.below(3); e > 0; --e) {
      test::RawChunk& ch = cs[r.below(static_cast<std::uint32_t>(cs.size()))];
      switch (r.below(5)) {
        case 0:
          if (!ch.payload.empty()) ch.payload[r.below(static_cast<std::uint32_t>(ch.payload.size()))] = r.byte();
          break;
        case 1:
          if (!ch.payload.empty()) ch.payload.resize(r.below(static_cast<std::uint32_t>(ch.payload.size())));
          break;
        case 2:
          for (std::uint32_t k = r.below(8); k > 0; --k) ch.payload.push_back(r.byte());
          break;
        case 3:
          std::swap(ch, cs[r.below(static_cast<std::uint32_t>(cs.size()))]);
          break;
        default:
          cs.insert(cs.begin() + r.below(static_cast<std::uint32_t>(cs.size())), ch);
          break;
      }
    }
    run_one(test::build_file(cs, {}), r.below(2) == 0 ? 4096 : 5, c);
  }
  MESSAGE("state chunk mutations: " << c.runs << " runs, " << c.done << " loaded, " << c.failed << " rejected");
  CHECK(c.done > 0);
  CHECK(c.failed > 0);
}

// OS-USBOUTS: the USBS 1.1 chunk (4 or 6 outputs): every payload byte at its interesting values, every cut and a few appended bytes; whatever loads must round-trip.
TEST_CASE("native fuzz: a USBS 1.1 chunk (4 and 6 outputs): every payload byte, every cut, appended bytes") {
  for (const std::uint8_t profile : {std::uint8_t{5}, std::uint8_t{6}}) {
    test::fill_state(test::arena(2).project);
    model::UsbState u;
    model::init_usb_state(u);
    u.profile = profile;
    u.slots = model::kUsbProfileChannels[profile];
    for (std::uint32_t i = 0; i < model::kStemSlots; ++i) {
      u.mask[i] = static_cast<std::uint16_t>(i >= u.slots ? 0u : (i == u.slots - 2u ? 0x1000u : (i == u.slots - 1u ? 0x2000u : 1u << i)));
    }
    NEED(test::arena(2).project.set_usb(u) == model::Status::Ok);
    Bytes base;
    NEED(test::save(test::arena(2).project, base) == Progress::Done);
    const std::vector<test::RawChunk> chunks = test::split_chunks(base);
    std::size_t at = chunks.size();
    for (std::size_t i = 0; i < chunks.size(); ++i) {
      if (chunks[i].id == formats::native::kChunkUsbs) at = i;
    }
    NEED(at < chunks.size());
    NEED(chunks[at].payload.size() == 22u);
    Tally t;
    const std::uint8_t values[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x7F, 0x80, 0xFF};
    for (std::size_t i = 0; i < chunks[at].payload.size(); i += kThin) {
      for (const std::uint8_t v : values) {
        std::vector<test::RawChunk> cs = chunks;
        cs[at].payload[i] = v;
        run_one(test::build_file(cs, {}), 4096, t);
      }
    }
    for (std::size_t n = 0; n <= chunks[at].payload.size(); n += kThin) {
      std::vector<test::RawChunk> cs = chunks;
      cs[at].payload.resize(n);
      run_one(test::build_file(cs, {}), 4096, t);
    }
    for (std::uint8_t extra = 1; extra <= 4; ++extra) {
      std::vector<test::RawChunk> cs = chunks;
      for (std::uint8_t k = 0; k < extra; ++k) cs[at].payload.push_back(static_cast<std::uint8_t>(0x30 + k));
      run_one(test::build_file(cs, {}), 4096, t);
    }
    MESSAGE("USBS 1.1 profile " << int(profile) << ": " << t.runs << " runs, " << t.done << " loaded, " << t.failed << " rejected");
    CHECK(t.done > 0);
    CHECK(t.failed > 0);
  }
}

TEST_CASE("native fuzz: arbitrary bytes and prefixes of a maximal file") {
  test::fill_maximal(test::arena(2).project);
  Bytes big;
  NEED(test::save(test::arena(2).project, big) == Progress::Done);
  test::Rng r(0xBADC0DEu);
  Tally t;
  for (std::uint32_t n = 0; n < 40; ++n) {  // a few cuts of the big one
    const Bytes prefix(big.begin(), big.begin() + static_cast<std::ptrdiff_t>(r.below(static_cast<std::uint32_t>(big.size()))));
    run_one(prefix, 4096, t);
  }
  for (std::uint32_t n = 0; n < runs(2000); ++n) {  // pure noise, some of it starting like a file
    Bytes noise(r.below(300));
    for (std::uint8_t& b : noise) {
      b = r.byte();
    }
    if (noise.size() >= 12 && r.below(2) == 0) {
      std::memcpy(noise.data(), "FORM", 4);
      std::memcpy(noise.data() + 8, "O3PJ", 4);
      noise[4] = 0;
      noise[5] = 0;
      noise[6] = static_cast<std::uint8_t>(r.below(2));
      noise[7] = static_cast<std::uint8_t>(r.byte() & 0xFE);
    }
    run_one(noise, 64, t);
  }
  MESSAGE("noise and cuts: " << t.runs << " runs, " << t.done << " loaded, " << t.failed << " rejected");
  CHECK(t.failed == t.runs);
}
