// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012d: the built-in modules (module/builtin.h): the TB-303, chip, FM and SID .tmod files that are bytes in the image, loaded with NO card by the module loader into
// the arenas it uses for a card's file, and (where this process can call them: the Cortex-M4F, so the QEMU suite) played through audio::ModuleVoices: a note renders
// audible audio. On the host the module code is Thumb and cannot be called: the loads and the arena accounting are checked, the render is skipped. The card's side of the
// rule (a newer copy wins, an older or broken one is ignored) is module_builtin_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/null_storage.h"

#include <o3os/audio/module_voices.h>
#include <o3os/module/loader.h>
#if defined(__arm__)
#include <o3os/platform/qemu/clock.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if !defined(__arm__)
#include <fstream>
#endif
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace o3os;
using module::LoadedModule;
using module::ModuleLoader;
using module::ModuleSet;

// The module memory the image gives the loader: the slab (14 KiB at the top of SRAM_L: the voices' output blocks, then the hot arena) and SDRAM. Statics, 16-byte aligned,
// at the address the pointer says: on the target the placement is where the code runs.
constexpr std::uint32_t kSlab = 14u * 1024u - audio::ModuleVoices::kScratchBytes;  // (mem_map.h kModuleSlabSize)
alignas(64) std::uint8_t g_scratch[audio::ModuleVoices::kScratchBytes];
alignas(64) std::uint8_t g_slab[kSlab];
alignas(64) std::uint8_t g_cold[64 * 1024];

struct Rig {
  test::NullStorage card;
  ModuleSet set;
  ModuleLoader loader;
  Rig() {
    set.set_builtins(module::kBuiltinModules, module::kBuiltinModuleCount);
    set.hot().init(g_slab, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_slab)), sizeof g_slab);
    set.cold().init(g_cold, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_cold)), sizeof g_cold);
  }
  bool load(const char* stem) {
    if (!loader.begin(card, set, stem, nullptr)) {
      return false;
    }
    for (unsigned n = 0; n < 100000 && loader.state() == ModuleLoader::State::Running; ++n) {
      (void)loader.step(2048);
    }
    return loader.state() == ModuleLoader::State::Done;
  }
};

// OS-FLASHDIET P1: the notices are a deflate stream; the whole text, as the loader of a module would inflate it.
std::string whole_notices() {
  core::Inflater inf;
  inf.begin(module::kBuiltinNotices.z, module::kBuiltinNotices.z_size, module::kBuiltinNotices.size);
  std::string text(module::kBuiltinNotices.size, '\0');
  const std::size_t got = inf.read(reinterpret_cast<std::uint8_t*>(&text[0]), text.size());
  std::uint8_t extra = 0;
  CHECK(inf.read(&extra, 1) == 0u);  // the stream ends where the table says
  CHECK(inf.done());
  CHECK_FALSE(inf.failed());
  text.resize(got);
  return text;
}

struct Page {
  std::string text;
  bool pieces_clean = true;  // no piece holds a newline
};

void collect(void* ctx, const char* text, std::size_t n, bool eol) noexcept {
  Page& p = *static_cast<Page*>(ctx);
  if (eol) {
    p.text += '\n';
    return;
  }
  p.pieces_clean = p.pieces_clean && std::memchr(text, '\n', n) == nullptr;
  p.text.append(text, n);
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t e = text.find('\n', at);
    if (e == std::string::npos) {
      out.push_back(text.substr(at));
      break;
    }
    out.push_back(text.substr(at, e - at));
    at = e + 1;
  }
  return out;
}

}  // namespace

TEST_CASE("built-in modules: the table has the TB-303, chip, FM and SID, and the image carries Open303's notice") {
  NEED(module::kBuiltinModuleCount == 4u);
  CHECK(std::strcmp(module::kBuiltinModules[0].stem, "tb303") == 0);
  CHECK(std::strcmp(module::kBuiltinModules[1].stem, "chip") == 0);
  CHECK(std::strcmp(module::kBuiltinModules[2].stem, "fm") == 0);
  CHECK(std::strcmp(module::kBuiltinModules[3].stem, "sid") == 0);
  const std::string notices = whole_notices();
  CHECK(notices.find("Copyright (c) 2009 Robin Schmidt") != std::string::npos);
  CHECK(notices.find("THE SOFTWARE IS PROVIDED \"AS IS\"") != std::string::npos);
  CHECK(notices.find("track3r-os") != std::string::npos);
  // OS-CECHIP C2: Chip's waves 4..11 are a port of tracker-ce's CHIP engine: its copyright line, its permission notice and Part 2's conditions travel with the image
  CHECK(notices.find("Copyright (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors") != std::string::npos);
  CHECK(notices.find("The above copyright notice, this permission notice, and the conditions in") != std::string::npos);
  CHECK(notices.find("PART 2: WHOLESALE COMMERCIAL CLONE RESTRICTION") != std::string::npos);
  CHECK(notices.find("ChiptuneSynthVoice.cpp") != std::string::npos);
}

// OS-FLASHDIET P1: the stream holds the text the earlier image held as a string literal: the notice files, each without its trailing blank lines and followed by one
// blank line, the whole ending in one newline (and one more, which the literal's last line carried): no change in what `mod notice` prints.
// Host only: the QEMU guest has no files (the paging tests below run on both).
#if !defined(__arm__)
TEST_CASE("built-in modules: the notices are the joined notice files, byte for byte") {
  std::string want;
  for (const char* file : {"builtin/NOTICES.txt", "tb303/LICENSE-Open303.txt", "chip/LICENSE-tracker-ce.txt"}) {
    std::ifstream in(std::string(O3_MODULES_DIR) + "/" + file, std::ios::binary);
    NEED(in.good());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (std::size_t at = text.find("\r\n"); at != std::string::npos; at = text.find("\r\n", at)) {
      text.erase(at, 1);  // the generator writes LF line ends (Open303's licence file has CRLF), as the string literal did
    }
    while (!text.empty() && std::strchr("\n\r \t\f\v", text.back()) != nullptr) {
      text.pop_back();
    }
    want += text + "\n\n";
  }
  while (!want.empty() && want.back() == '\n') {
    want.pop_back();
  }
  want += "\n\n";
  CHECK(whole_notices() == want);
}
#endif

TEST_CASE("built-in modules: notices_page pages the stream like a split of the text, whatever the start line and the page length") {
  const std::string text = whole_notices();
  const std::vector<std::string> lines = lines_of(text);
  NEED(lines.size() > 60u);
  core::Inflater inf;
  const unsigned count = static_cast<unsigned>(lines.size());
  const unsigned starts[] = {0u, 1u, 7u, 23u, 24u, 25u, count - 25u, count - 24u, count - 1u, count, count + 1u, 1000u};
  for (const unsigned first : starts) {
    for (const unsigned len : {1u, 5u, 24u, 100000u}) {
      CAPTURE(first);
      CAPTURE(len);
      Page page;
      const module::NoticePage r = module::notices_page(module::kBuiltinNotices, inf, first, len, &collect, &page);
      CHECK(r.ok);
      std::string want;
      for (unsigned i = first; i < count && i - first < len; ++i) {
        want += lines[i] + "\n";
      }
      CHECK(page.text == want);
      CHECK(page.pieces_clean);
      const bool more = static_cast<unsigned long long>(first) + len < count;
      CHECK(r.more == more);
      if (more) {
        CHECK(r.next == first + len);
      }
    }
  }
  // the console's walk: each call starts where the last said, 24 lines a page, and the pages join into the whole text
  std::string joined;
  unsigned at = 0;
  unsigned pages = 0;
  for (;; ++pages) {
    Page page;
    const module::NoticePage r = module::notices_page(module::kBuiltinNotices, inf, at, 24, &collect, &page);
    NEED(r.ok);
    joined += page.text;
    if (!r.more) {
      break;
    }
    at = r.next;
    NEED(pages < 200u);
  }
  CHECK(joined == text);
  CHECK(pages + 1u == (count + 23u) / 24u);
}

TEST_CASE("built-in modules: notices_page reports a damaged stream") {
  std::vector<std::uint8_t> z(module::kBuiltinNotices.z, module::kBuiltinNotices.z + module::kBuiltinNotices.z_size);
  z.resize(z.size() / 2);  // cut short
  const module::BuiltinNotices cut{z.data(), static_cast<std::uint32_t>(z.size()), module::kBuiltinNotices.size};
  core::Inflater inf;
  Page page;
  const module::NoticePage r = module::notices_page(cut, inf, 0, 100000u, &collect, &page);
  CHECK_FALSE(r.ok);
}

TEST_CASE("built-in modules: the stem matches whatever its case, as FAT and the ENGINE picker do (OS-012d review)") {
  Rig r;
  NEED(r.set.builtin("tb303") != nullptr);
  CHECK(r.set.builtin("TB303") == r.set.builtin("tb303"));
  CHECK(r.set.builtin("Fm") == r.set.builtin("fm"));
  CHECK(r.set.builtin("tb30") == nullptr);
  CHECK(r.set.builtin("tb3033") == nullptr);
}

TEST_CASE("built-in modules: each loads with no card, like a card's file, and the slab holds them all (hot budget)") {
  NEED(module::kBuiltinModuleCount == 4u);
  Rig r;
  std::uint32_t hot_total = 0;
  for (unsigned i = 0; i < module::kBuiltinModuleCount; ++i) {
    const char* stem = module::kBuiltinModules[i].stem;
    CAPTURE(stem);
    NEED(r.load(stem));
    const int idx = r.set.find(stem);
    NEED(idx >= 0);
    const LoadedModule& m = *r.set.at(static_cast<unsigned>(idx));
    CHECK(m.builtin);
    CHECK(m.card_status == module::Status::NotFound);  // no card: nothing there
    CHECK(!m.hot_in_sdram);                            // the hot part is in the slab, not SDRAM
    CHECK(m.header.kind == O3M_KIND_INSTRUMENT);
    CHECK(r.set.quarantined(stem) == false);
    hot_total += m.header.hot_size;
    std::printf("[hot budget] %-5s hot %5u B, cold %5u B, state %4u B; slab used %5u of %u B (%u B left)\n", stem, static_cast<unsigned>(m.header.hot_size),
                static_cast<unsigned>(m.header.image_size - m.header.hot_size), static_cast<unsigned>(m.header.state_bytes), static_cast<unsigned>(r.set.hot().used()),
                static_cast<unsigned>(r.set.hot().size()), static_cast<unsigned>(r.set.hot().size() - r.set.hot().used()));
  }
  CHECK(r.set.hot().used() >= hot_total);
  CHECK(r.set.count() == 4u);
}

TEST_CASE("built-in modules: a note renders audible audio (where the module code can run)") {
  NEED(module::kBuiltinModuleCount == 4u);
  Rig r;
  static audio::ModuleVoices mv;
  mv.attach_scratch(g_scratch, sizeof g_scratch);
  mv.set_sample_rate(44100.0f);
  const struct {
    const char* stem;
    unsigned note;
  } kNotes[] = {{"tb303", 36}, {"chip", 60}, {"fm", 60}, {"sid", 60}};
  unsigned slot = 0;
  for (const auto& k : kNotes) {
    CAPTURE(k.stem);
    NEED(r.load(k.stem));
    const LoadedModule& m = *r.set.at(static_cast<unsigned>(r.set.find(k.stem)));
    if (m.entry == nullptr) {
      continue;  // the host cannot call Thumb code: the target suite does
    }
    std::uint8_t* state = nullptr;
    std::uint32_t addr = 0;
    NEED(r.set.cold().take(m.entry->state_bytes, 16, state, addr));
    std::memset(state, 0, m.entry->state_bytes);
    NEED(mv.bind(slot, *m.entry, module::fault_id(k.stem), state, nullptr, 0) == audio::ModuleVoices::Bind::Ok);
    NEED(mv.note_on(slot, k.note, slot, 110));
    std::int32_t peak = 0;
    for (unsigned b = 0; b < 40; ++b) {  // 40 blocks, about 58 ms
      mv.begin_block();
      mv.render(0, hal::audio::kBlock);
      mv.end_block();
      const std::int16_t* f = mv.feed(slot);
      for (unsigned i = 0; f != nullptr && i < hal::audio::kBlock; ++i) {
        const std::int32_t v = f[i] < 0 ? -f[i] : f[i];
        peak = v > peak ? v : peak;
      }
    }
    CHECK(peak > 200);  // audible: well above the quantisation floor
    std::printf("[built-in render] %-5s note %u: peak %d of 32767\n", k.stem, k.note, static_cast<int>(peak));
    ++slot;
  }
  mv.retire_all();
  mv.release_all();
}

// ---- OS-303PERF: the cost of the TB-303 in instructions, on the target ----------------------------------------------------------------------------------------------------------
// QEMU runs with -icount shift=5, so the guest's milliseconds are instructions (31,250 each). That is not cycles (no pipeline, no wait states, no loads that stall): it is the figure that
// MOVES when the code does less, and it moves the same way the instruction count of the disassembly does but weighted by what the sound really executes (the saw path, the accent
// envelope, the coefficient update every kCtl samples). The unit's cycles are the bench's (`diag bench tb303`); a change to the voice quotes both.
#if defined(__arm__)
TEST_CASE("built-in tb303: instructions per block of the module's render alone and through ModuleVoices (QEMU icount)") {
  NEED(module::kBuiltinModuleCount == 4u);
  Rig r;
  static audio::ModuleVoices mv;
  mv.attach_scratch(g_scratch, sizeof g_scratch);
  mv.set_sample_rate(44117.647f);
  NEED(r.load("tb303"));
  const LoadedModule& m = *r.set.at(static_cast<unsigned>(r.set.find("tb303")));
  NEED(m.entry != nullptr);
  std::uint8_t* state = nullptr;
  std::uint32_t addr = 0;
  NEED(r.set.cold().take(m.entry->state_bytes, 16, state, addr));
  std::memset(state, 0, m.entry->state_bytes);
  NEED(mv.bind(0, *m.entry, module::fault_id("tb303"), state, nullptr, 0) == audio::ModuleVoices::Bind::Ok);
  NEED(mv.note_on(0, 45, 0, 110));  // A1, accented, held: the amplitude decays over 1.2 s, the voice sounds for all of the run
  constexpr unsigned kBlocks = 2000;  // 2.9 s of audio, 70 M instructions
  for (unsigned b = 0; b < 40; ++b) {
    mv.begin_block();
    mv.render(0, hal::audio::kBlock);
    mv.end_block();
  }
  const std::uint64_t t0 = platform::qemu::system_clock().uptime_ms();
  for (unsigned b = 0; b < kBlocks; ++b) {
    mv.begin_block();
    mv.render(0, hal::audio::kBlock);
    mv.end_block();
  }
  const std::uint32_t through = static_cast<std::uint32_t>((platform::qemu::system_clock().uptime_ms() - t0) * 31250u / kBlocks);
  // the module alone, on a second instance of the state
  alignas(16) static float buf[O3M_BLOCK_MAX];
  std::uint8_t* state2 = nullptr;
  NEED(r.set.cold().take(m.entry->state_bytes, 16, state2, addr));
  std::memset(state2, 0, m.entry->state_bytes);

  {
    const o3m_host& host = mv.host();
    NEED(m.entry->init(state2, &host) == 0);
  }
  m.entry->note_on(state2, 45, 110, 0);
  for (unsigned b = 0; b < 40; ++b) {
    (void)m.entry->render(state2, nullptr, buf, hal::audio::kBlock);
  }
  const std::uint64_t t1 = platform::qemu::system_clock().uptime_ms();
  for (unsigned b = 0; b < kBlocks; ++b) {
    (void)m.entry->render(state2, nullptr, buf, hal::audio::kBlock);
  }
  const std::uint32_t alone = static_cast<std::uint32_t>((platform::qemu::system_clock().uptime_ms() - t1) * 31250u / kBlocks);
  std::printf("[tb303 icount] instructions per block of %u frames: module render alone %u, through ModuleVoices (render + conversion + bookkeeping) %u\n",
              static_cast<unsigned>(hal::audio::kBlock), static_cast<unsigned>(alone), static_cast<unsigned>(through));
  CHECK(alone > 5000u);      // the voice did run
  CHECK(alone < 100000u);    // and does not cost a block of cycles (a sanity bound, not a budget: the budget is the unit's)
  CHECK(through >= alone);
  mv.retire_all();
  mv.release_all();
}
// ---- OS-303PERF (M1): the packed TB-303 is bit for bit the one of rc/0.4.10 (b3203e1), on the target ----------------------------------------------------------------------------------
// The restructure of voice_render (tb303_dsp.h) is held to the old sample loop on the host by tb303_refactor_test.cpp, which compares two compiled shapes of one float arithmetic and so cannot
// speak for the Cortex-M4F (VFMA exists there; the module is built -ffp-contract=off, tools/o3m_pack.py CFLAGS, so no fused multiply-add may be formed, but only the real binary shows it). This is that proof:
// the module the image embeds (the .tmod bytes, loaded as on the unit, the flags of the packer, -O3 -mlong-calls ...) plays scripts from a fixed seed, and a hash of what it makes (every output float
// bit, every return value and every byte of the voice after each call but its four service pointers, which are addresses of this binary) is compared with a CONSTANT that was taken by running this same test on the tree of b3203e1 (the old loop), on this same QEMU
// target. The constant is the old loop's output; it changes only if the sound of the module does, or core::exp2_fast / log2_fast and the sin / cos the host hands to modules do (they are in it).
namespace {

struct Fnv {
  std::uint32_t h = 2166136261u;
  void bytes(const void* p, std::size_t n) {
    const std::uint8_t* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
      h = (h ^ b[i]) * 16777619u;
    }
  }
  void word(std::uint32_t w) { bytes(&w, sizeof w); }
};

struct Lcg {
  std::uint64_t s;
  std::uint32_t next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(s >> 33);
  }
  // The goldens below were recorded when this line was compiled with GCC's default contraction, i.e. as one fused multiply-add; it is spelled so now, so that the script (the parameter values the module is given) does not
  // depend on the test target's -ffp-contract flag (every QEMU suite is built -ffp-contract=off like production).
  float range(float lo, float hi) { return __builtin_fmaf(hi - lo, static_cast<float>(next()) / 2147483648.0f, lo); }
};

constexpr std::uint32_t kServices = 16;  // Voice::sv, the first member of the voice: the four service function pointers, which are addresses in THIS binary and so not part of what the module made
// FNV-1a of the scripts below, as b3203e1's voice_render made them (the same value from the restructured loop: OS-303PERF M1, checked on 2026-10-14): 0x22675067 over the output AND the voice's bytes.
// OS-MODPITCH (ABI 1.8) added one member to the voice, `note_hz` (tb303_dsp.h: the sounding note's own frequency, which pitch() moves the frequency from): every later member moved by four bytes, so the hash of
// the bytes changed (0x22675067 -> kTb303Golden) while the output did not. Evidence, on this target (QEMU, 2026-10-16): the tb303 module of ceec397 (the repository's first commit, ABI 1.2, with only its
// descriptor size spelled O3M_MODULE_SIZE(2) to build against the 1.8 header) run through this very test gives 0x22675067 over output + bytes and 0xf910ea28 over the return values and the output floats
// alone (kTb303AudioGolden), the same as the 1.8 module's output alone. (b3203e1, the commit the old constant was first taken at, is not in this repository's history; ceec397 reproduces its value.)
// OS-SNDMOD S3 appended five parameters (ids 9..13) to the voice (Tune, Transpose, Acc Thresh, Filter Track, Acc Decay, and the note-start factor): 24 more bytes of members, so the hash of the bytes moved
// (0x8ad564f3 -> 0xce6397d7) while the output did not: kTb303AudioGolden is unchanged. Evidence, on this target (QEMU, 2026-10-19, 0ebec56): hash ce6397d7, output alone f910ea28.
// The Filter Track slide fix (ec51b91) added the slew's goal and note to the voice (8 bytes, state 416 B): the bytes moved again (0xce6397d7 -> kTb303Golden), the output
// did not. Evidence, on this target (QEMU, 2026-10-19, 1ad018e): hash d7e0cea9, output alone f910ea28.
constexpr std::uint32_t kTb303Golden = 0xd7e0cea9u;
constexpr std::uint32_t kTb303AudioGolden = 0xf910ea28u;

}  // namespace

TEST_CASE("built-in tb303: the packed module's output is bit for bit the old loop's (golden hash of the ceec397 module, on the Cortex-M4F); the voice's bytes are pinned since ABI 1.8") {
  NEED(module::kBuiltinModuleCount == 4u);
  Rig r;
  static audio::ModuleVoices mv;
  NEED(r.load("tb303"));
  const LoadedModule& m = *r.set.at(static_cast<unsigned>(r.set.find("tb303")));
  NEED(m.entry != nullptr);
  const std::uint32_t state_bytes = m.entry->state_bytes;
  std::uint8_t* state = nullptr;
  std::uint32_t addr = 0;
  NEED(r.set.cold().take(state_bytes, 16, state, addr));
  alignas(16) static float buf[O3M_BLOCK_MAX];
  Fnv fnv;
  Fnv audio;  // the return values and the output alone
  std::uint32_t calls = 0, went_idle = 0, samples = 0, accents = 0;
  float peak = 0.0f;  // (the scripts make a sound: a hash of silence would prove nothing)
  bool idle = false;
  for (unsigned seed = 1; seed <= 8; ++seed) {
    Lcg g{static_cast<std::uint64_t>(seed) * 2654435761ull + 7ull};
    mv.set_sample_rate((seed & 1u) ? 44100.0f : 44117.647f);
    std::memset(state, 0, state_bytes);
    NEED(m.entry->init(state, &mv.host()) == 0);
    const float waves[] = {0.0f, 1.0f, 0.5f, 0.25f};
    const float lo[] = {0, 314, 0, 0, 200, 0, 0, -24, 0}, hi[] = {1, 2394, 100, 100, 2000, 100, 300, 6, 1};  // the module's parameter ranges, by id
    m.entry->set_param(state, 0, waves[g.next() & 3u]);
    for (std::uint32_t id = 1; id < 9; ++id) {
      m.entry->set_param(state, id, g.range(lo[id], hi[id]));
    }
    bool held = false;
    for (unsigned phrase = 0; phrase < 12; ++phrase) {  // a phrase: notes with parameter changes in between, then the release and a tail long enough to go idle
      const unsigned notes = 1u + g.next() % 3u;
      for (unsigned n = 0; n < notes; ++n) {
        const std::uint32_t note = 24u + g.next() % 48u;
        const std::uint32_t vel = (g.next() & 1u) ? 100u + g.next() % 28u : 1u + g.next() % 99u;
        accents += vel >= 100u ? 1u : 0u;
        m.entry->note_on(state, note, vel, held && (g.next() & 1u) ? O3M_NOTE_LEGATO : 0u);
        held = true;
        idle = false;
        const std::uint32_t id = g.next() % 9u;
        m.entry->set_param(state, id, g.range(lo[id], hi[id]));
        std::uint32_t todo = 200u + g.next() % 700u;
        while (todo != 0u) {
          const std::uint32_t cuts[] = {1, 2, 3, 4, 5, 7, 16, 33, 63, 64, 65, 127, 128, 0, 4, 8};
          std::uint32_t c = g.next() % 5u == 0u ? 64u : cuts[g.next() % 16u];
          c = c > todo ? todo : c;
          for (std::uint32_t k = 0; k < c; ++k) {
            buf[k] = 777.0f;  // (a sample the render does not write would show in the hash)
          }
          const std::uint32_t ret = m.entry->render(state, nullptr, buf, c);
          fnv.word(ret);
          fnv.bytes(buf, c * sizeof(float));
          audio.word(ret);
          audio.bytes(buf, c * sizeof(float));
          fnv.bytes(state + kServices, state_bytes - kServices);
          for (std::uint32_t k = 0; k < c; ++k) {
            peak = buf[k] > peak ? buf[k] : (-buf[k] > peak ? -buf[k] : peak);
          }
          went_idle += ret == 0u && !idle ? 1u : 0u;
          idle = ret == 0u;
          ++calls;
          samples += c;
          todo -= c != 0u ? c : (todo > 0u ? 1u : 0u);  // (a call of no frames moves nothing: the loop must still end)
        }
      }
      m.entry->note_off(state, 0);
      held = false;
      std::uint32_t tail = 300u + g.next() % 3500u;
      while (tail != 0u) {
        const std::uint32_t c = tail < 61u ? tail : 1u + g.next() % 61u;
        const std::uint32_t ret = m.entry->render(state, nullptr, buf, c);
        fnv.word(ret);
        fnv.bytes(buf, c * sizeof(float));
        audio.word(ret);
        audio.bytes(buf, c * sizeof(float));
        fnv.bytes(state + kServices, state_bytes - kServices);
        went_idle += ret == 0u && !idle ? 1u : 0u;
        idle = ret == 0u;
        ++calls;
        samples += c;
        tail -= c;
      }
    }
  }
  std::printf("[tb303 golden] %u calls, %u samples, %u voices went idle, %u accents; hash %08x (golden %08x), output alone %08x (golden %08x)\n", static_cast<unsigned>(calls),
              static_cast<unsigned>(samples), static_cast<unsigned>(went_idle), static_cast<unsigned>(accents), static_cast<unsigned>(fnv.h), static_cast<unsigned>(kTb303Golden),
              static_cast<unsigned>(audio.h), static_cast<unsigned>(kTb303AudioGolden));
  CHECK(samples > 150000u);
  CHECK(went_idle > 30u);  // the idle test ended voices, in blocks of every size, at all sorts of phases of the coefficient grid
  CHECK(accents > 20u);
  CHECK(peak > 0.1f);
  CHECK(audio.h == kTb303AudioGolden);  // the sound
  CHECK(fnv.h == kTb303Golden);         // the sound and the voice's bytes
}

// ---- OS-CECHIP G1: today's Chip on the Cortex-M4F: the golden render and the cost of a 64-frame block a wave ---------------------------------------------------------------
// docs/specs/OS-CECHIP.md 4, G1: the constants below were recorded from dev (this branch, before any change to the voice; the .tmod of this tree) on this same QEMU target, and
// from G1 on no slice may change them. kChipGolden is the FNV-1a of the script below (every render call's return value and every output float's bits, as kTb303AudioGolden is);
// kChipIcount[wave] is the instructions a 64-frame block of the module's render alone costs on wave `wave` (QEMU -icount shift=5: 31,250 instructions a millisecond), held within 1 % (C6's bound).
namespace {

constexpr std::uint32_t kChipGolden = 0xa648d76au;
constexpr std::uint32_t kChipIcount[4] = {3273u, 1765u, 1402u, 1464u};

}  // namespace

TEST_CASE("built-in chip: the packed module's output is bit for bit today's (golden hash, on the Cortex-M4F)") {
  NEED(module::kBuiltinModuleCount == 4u);
  Rig r;
  static audio::ModuleVoices mv;
  mv.attach_scratch(g_scratch, sizeof g_scratch);
  mv.set_sample_rate(44100.0f);
  NEED(r.load("chip"));
  const LoadedModule& m = *r.set.at(static_cast<unsigned>(r.set.find("chip")));
  NEED(m.entry != nullptr);
  const std::uint32_t state_bytes = m.entry->state_bytes;
  std::uint8_t* state = nullptr;
  std::uint32_t addr = 0;
  NEED(r.set.cold().take(state_bytes, 16, state, addr));
  alignas(16) static float buf[O3M_BLOCK_MAX];
  Fnv fnv;
  std::uint32_t calls = 0, samples = 0;
  float peak = 0.0f;  // (the script makes a sound: a hash of silence would prove nothing)
  bool idle = false, went_idle = false;
  // The script: every wave with its own duty, Raw Edges, Vol Steps and sweep, three notes a wave (0.5 s each, in blocks of every size; the middle note of wave 0 a RAMP with pitch()),
  // then the release and a tail. The parameter ids are the manifest's (modules/chip/module.json): 0 Wave, 1 Duty, 2 Raw Edges, 3 Vol Steps, 4 Volume, 12 Sweep, 13 Sweep Time.
  for (std::uint32_t wave = 0; wave < 4u; ++wave) {
    CAPTURE(wave);
    std::memset(state, 0, state_bytes);
    NEED(m.entry->init(state, &mv.host()) == 0);
    m.entry->set_param(state, 0, static_cast<float>(wave));
    m.entry->set_param(state, 1, static_cast<float>((wave + 1u) & 3u));
    m.entry->set_param(state, 2, static_cast<float>(wave & 1u));
    m.entry->set_param(state, 3, static_cast<float>((wave >> 1) & 1u));
    m.entry->set_param(state, 4, -6.0f);
    m.entry->set_param(state, 12, wave == 0u ? 12.0f : (wave == 1u ? -12.0f : 0.0f));
    m.entry->set_param(state, 13, 200.0f);
    const std::uint32_t notes[3] = {36u, 60u, 96u};
    for (unsigned ni = 0; ni < 3u; ++ni) {
      const bool ramp = ni == 1u && wave == 0u;
      m.entry->note_on(state, notes[ni], 100u + wave * 8u + ni, ramp ? (O3M_NOTE_LEGATO | O3M_NOTE_RAMP) : 0u);
      if (ramp) {
        m.entry->pitch(state, 7.0f, 0.25f);
      }
      std::uint32_t todo = 22050u;  // 0.5 s a note
      while (todo != 0u) {
        const std::uint32_t cuts[] = {64u, 7u, 128u, 1u, 33u, 64u, 97u, 5u, 128u, 64u};
        std::uint32_t c = cuts[(calls + ni) % 10u];
        c = c > todo ? todo : c;
        for (std::uint32_t k = 0; k < c; ++k) {
          buf[k] = 777.0f;  // (a sample the render does not write would show in the hash)
        }
        const std::uint32_t ret = m.entry->render(state, nullptr, buf, c);
        fnv.word(ret);
        fnv.bytes(buf, c * sizeof(float));
        for (std::uint32_t k = 0; k < c; ++k) {
          peak = buf[k] > peak ? buf[k] : (-buf[k] > peak ? -buf[k] : peak);
        }
        went_idle = went_idle || (ret == 0u && !idle);
        idle = ret == 0u;
        ++calls;
        samples += c;
        todo -= c;
      }
      m.entry->note_off(state, notes[ni]);
      for (unsigned t = 0; t < 2048u; t += 64u) {  // the release and the tail, in blocks of 64
        const std::uint32_t ret = m.entry->render(state, nullptr, buf, 64u);
        fnv.word(ret);
        fnv.bytes(buf, 64u * sizeof(float));
        went_idle = went_idle || (ret == 0u && !idle);
        idle = ret == 0u;
        ++calls;
        samples += 64u;
      }
    }
  }
  std::printf("[chip golden] %u calls, %u samples, went idle %d, peak %f; hash %08x (constant %08x)\n", static_cast<unsigned>(calls), static_cast<unsigned>(samples),
              static_cast<int>(went_idle), static_cast<double>(peak), static_cast<unsigned>(fnv.h), static_cast<unsigned>(kChipGolden));
  CHECK(samples > 200000u);
  CHECK(peak > 0.05f);
  CHECK(went_idle);  // the releases ended voices, in blocks of every size
  CHECK(fnv.h == kChipGolden);
}

TEST_CASE("built-in chip: instructions per 64-frame block of the module's render alone, per wave (QEMU icount)") {
  NEED(module::kBuiltinModuleCount == 4u);
  Rig r;
  static audio::ModuleVoices mv;
  mv.attach_scratch(g_scratch, sizeof g_scratch);
  mv.set_sample_rate(44100.0f);
  NEED(r.load("chip"));
  const LoadedModule& m = *r.set.at(static_cast<unsigned>(r.set.find("chip")));
  NEED(m.entry != nullptr);
  const std::uint32_t state_bytes = m.entry->state_bytes;
  std::uint8_t* state = nullptr;
  std::uint32_t addr = 0;
  NEED(r.set.cold().take(state_bytes, 16, state, addr));
  alignas(16) static float buf[O3M_BLOCK_MAX];
  // 8000 blocks a wave: uptime_ms() has a millisecond's resolution, and a millisecond is 31,250 instructions, so at 8000 blocks the rounding of the two clock reads is
  // under 4 instructions a block (0.3 % at the cheapest wave) - well inside the 1 % band below whatever run recorded the constants.
  constexpr unsigned kBlocks = 8000;  // 512,000 frames a wave, about 11.6 s: the envelope is at the sustain level for the whole run
  for (std::uint32_t wave = 0; wave < 4u; ++wave) {
    CAPTURE(wave);
    std::memset(state, 0, state_bytes);
    NEED(m.entry->init(state, &mv.host()) == 0);
    m.entry->set_param(state, 0, static_cast<float>(wave));  // Wave
    m.entry->note_on(state, 60u, 110u, 0u);                  // held: the default envelope decays in 250 ms, long past the warm-up
    for (unsigned b = 0; b < 200u; ++b) {
      (void)m.entry->render(state, nullptr, buf, hal::audio::kBlock);
    }
    const std::uint64_t t0 = platform::qemu::system_clock().uptime_ms();
    for (unsigned b = 0; b < kBlocks; ++b) {
      (void)m.entry->render(state, nullptr, buf, hal::audio::kBlock);
    }
    const std::uint32_t per = static_cast<std::uint32_t>((platform::qemu::system_clock().uptime_ms() - t0) * 31250u / kBlocks);
    std::printf("[chip icount] wave %u: %u instructions per %u-frame block (constant %u)\n", static_cast<unsigned>(wave), static_cast<unsigned>(per),
                static_cast<unsigned>(hal::audio::kBlock), static_cast<unsigned>(kChipIcount[wave]));
    CHECK(per > 500u);       // the voice did run
    CHECK(per < 100000u);    // and does not cost a block of cycles (a sanity bound, not a budget: the budget is the unit's)
    CHECK(per >= kChipIcount[wave] * 99u / 100u);  // within 1 % of G1's figure (C6's bound)
    CHECK(per <= kChipIcount[wave] * 101u / 100u);
  }
}

// OS-CECHIP C2: a note on each of the new waves (4..11) is finite and audible on the Cortex-M4F, through the packed module (its cold path runs from where the loader put it); the hash
// of waves 0..3 above is unchanged. Parameter ids as the manifest's: 0 Wave, 4 Volume.
TEST_CASE("built-in chip: a note on each of the waves 4..11 renders finite, audible audio (on the Cortex-M4F)") {
  NEED(module::kBuiltinModuleCount == 4u);
  Rig r;
  static audio::ModuleVoices mv;
  mv.attach_scratch(g_scratch, sizeof g_scratch);
  mv.set_sample_rate(44100.0f);
  NEED(r.load("chip"));
  const LoadedModule& m = *r.set.at(static_cast<unsigned>(r.set.find("chip")));
  NEED(m.entry != nullptr);
  const std::uint32_t state_bytes = m.entry->state_bytes;
  std::uint8_t* state = nullptr;
  std::uint32_t addr = 0;
  NEED(r.set.cold().take(state_bytes, 16, state, addr));
  alignas(16) static float buf[O3M_BLOCK_MAX];
  for (std::uint32_t wave = 4; wave < 12u; ++wave) {
    CAPTURE(wave);
    std::memset(state, 0, state_bytes);
    NEED(m.entry->init(state, &mv.host()) == 0);
    m.entry->set_param(state, 0, static_cast<float>(wave));
    m.entry->set_param(state, 4, 0.0f);  // Volume 0 dB
    m.entry->note_on(state, 60u, 110u, 0u);
    float peak = 0.0f;
    bool finite = true;
    for (unsigned b = 0; b < 60u; ++b) {
      (void)m.entry->render(state, nullptr, buf, hal::audio::kBlock);
      for (unsigned i = 0; i < hal::audio::kBlock; ++i) {
        const float a = buf[i] < 0.0f ? -buf[i] : buf[i];
        finite = finite && a < 100.0f;  // (a NaN fails the comparison)
        peak = a > peak ? a : peak;
      }
    }
    CHECK(finite);
    CHECK(peak > 0.01f);
    std::printf("[chip new wave] wave %u: peak %f\n", static_cast<unsigned>(wave), static_cast<double>(peak));
  }
}

// OS-CECHIP C3: what waves 0..3 cost once a post-chain parameter is off its default (they leave today's loops for the cold path: raw<W> into out[], then a pass with the sub phase, the hold and
// the lowpass a sample, exp2_f and sin_f every 32 samples). Measured like the default case above and only printed (modules/chip/README.md quotes it): the bound is a sanity bound.
// Parameter ids as the manifest's: 0 Wave, 21 Sub Osc, 22 Cutoff, 23 Resonance, 24 Bitcrush, 27 Env Amt, 28 Decimate.
TEST_CASE("built-in chip: instructions per 64-frame block of waves 0..3 with the post chain on and of waves 4..11 at their defaults (QEMU icount, measured, not a golden)") {
  NEED(module::kBuiltinModuleCount == 4u);
  Rig r;
  static audio::ModuleVoices mv;
  mv.attach_scratch(g_scratch, sizeof g_scratch);
  mv.set_sample_rate(44100.0f);
  NEED(r.load("chip"));
  const LoadedModule& m = *r.set.at(static_cast<unsigned>(r.set.find("chip")));
  NEED(m.entry != nullptr);
  const std::uint32_t state_bytes = m.entry->state_bytes;
  std::uint8_t* state = nullptr;
  std::uint32_t addr = 0;
  NEED(r.set.cold().take(state_bytes, 16, state, addr));
  alignas(16) static float buf[O3M_BLOCK_MAX];
  constexpr unsigned kBlocks = 8000;
  struct Config {
    const char* name;
    bool whole;  // false: Sub Osc only
    std::uint32_t wave;
    float sub;  // Sub Osc in percent (0 for the new waves at their defaults: OS-CECHIP C6 records their cost here, on the cold path without the post chain's passes)
  };
  const Config configs[] = {{"sub only", false, 0, 40.0f}, {"whole chain", true, 0, 40.0f}, {"whole chain", true, 1, 40.0f}, {"whole chain", true, 2, 40.0f}, {"whole chain", true, 3, 40.0f},
                            {"defaults", false, 4, 0.0f},  {"defaults", false, 5, 0.0f},      {"defaults", false, 6, 0.0f},      {"defaults", false, 7, 0.0f},      {"defaults", false, 8, 0.0f},
                            {"defaults", false, 9, 0.0f},  {"defaults", false, 10, 0.0f},     {"defaults", false, 11, 0.0f}};
  for (const Config& c : configs) {
    CAPTURE(c.name);
    CAPTURE(c.wave);
    std::memset(state, 0, state_bytes);
    NEED(m.entry->init(state, &mv.host()) == 0);
    m.entry->set_param(state, 0, static_cast<float>(c.wave));
    m.entry->set_param(state, 21, c.sub);  // Sub Osc
    if (c.whole) {
      m.entry->set_param(state, 22, 1800.0f);  // Cutoff
      m.entry->set_param(state, 23, 45.0f);    // Resonance
      m.entry->set_param(state, 24, 6.0f);     // Bitcrush
      m.entry->set_param(state, 27, 30.0f);    // Env Amt
      m.entry->set_param(state, 28, 3.0f);     // Decimate
    }
    m.entry->note_on(state, 60u, 110u, 0u);
    for (unsigned b = 0; b < 200u; ++b) {
      (void)m.entry->render(state, nullptr, buf, hal::audio::kBlock);
    }
    const std::uint64_t t0 = platform::qemu::system_clock().uptime_ms();
    for (unsigned b = 0; b < kBlocks; ++b) {
      (void)m.entry->render(state, nullptr, buf, hal::audio::kBlock);
    }
    const std::uint32_t per = static_cast<std::uint32_t>((platform::qemu::system_clock().uptime_ms() - t0) * 31250u / kBlocks);
    std::printf("[chip post icount] %s, wave %u: %u instructions per %u-frame block\n", c.name, static_cast<unsigned>(c.wave), static_cast<unsigned>(per), static_cast<unsigned>(hal::audio::kBlock));
    CHECK(per > 500u);
    CHECK(per < 100000u);
  }
}

#endif
