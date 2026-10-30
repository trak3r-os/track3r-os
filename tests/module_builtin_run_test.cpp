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

}  // namespace

TEST_CASE("built-in modules: the table has the TB-303, chip, FM and SID, and the image carries Open303's notice") {
  NEED(module::kBuiltinModuleCount == 4u);
  CHECK(std::strcmp(module::kBuiltinModules[0].stem, "tb303") == 0);
  CHECK(std::strcmp(module::kBuiltinModules[1].stem, "chip") == 0);
  CHECK(std::strcmp(module::kBuiltinModules[2].stem, "fm") == 0);
  CHECK(std::strcmp(module::kBuiltinModules[3].stem, "sid") == 0);
  CHECK(std::strstr(module::kBuiltinNotices, "Copyright (c) 2009 Robin Schmidt") != nullptr);
  CHECK(std::strstr(module::kBuiltinNotices, "THE SOFTWARE IS PROVIDED \"AS IS\"") != nullptr);
  CHECK(std::strstr(module::kBuiltinNotices, "track3r-os") != nullptr);
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
#endif
