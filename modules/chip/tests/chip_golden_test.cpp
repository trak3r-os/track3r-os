// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CECHIP G1 (docs/specs/OS-CECHIP.md 4, the gate): the golden renders of today's Chip, on the host. The module's host shared object is driven through the ABI as the OS
// drives it (modules/tests/o3m_test_support.h) and the FNV-1a of the float output bytes of each case is compared with a constant that was recorded from dev before any change
// to the voice. The matrix: each wave, each duty, Raw Edges, Vol Steps, three envelopes, sustain 0, an arpeggio of one/two/three steps, a sweep up and down, a vibrato with delay,
// velocity 1/96/127, Volume -48/0/+6, a RAMP note with pitch(), a note off before the first sample, and the block sizes 1/7/64/128; about 60 cases, each 0.5 s. From G1 on no slice
// may change these constants. Host only. The cases run on make_host()'s tables with exp2_f overridden by a pure-arithmetic exp2 (golden_exp2 below, the logic of core::exp2_fast):
// the voice's only call out is the host's exp2, and the support header's std::exp2 is the system libm, which could move a hash with no change to the voice.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../tests/o3m_test_support.h"

#include "../chip_gen.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace o3m_test;

namespace {

// FNV-1a (32-bit), as the TB-303's golden in tests/module_builtin_run_test.cpp.
struct Fnv {
  std::uint32_t h = 2166136261u;
  void floats(const float* f, std::size_t n) {
    const std::uint8_t* b = reinterpret_cast<const std::uint8_t*>(f);
    for (std::size_t i = 0; i < n * sizeof(float); ++i) {
      h = (h ^ b[i]) * 16777619u;
    }
  }
};

constexpr std::uint32_t kFrames = 22050u;  // 0.5 s at 44.1 kHz

// 2^x in pure arithmetic, the logic of core::exp2_fast (core/src/fast_math.cpp) — the exp2 the OS hands modules on the MK66 (audio/src/module_voices.cpp), so the one the
// QEMU hash (tests/module_builtin_run_test.cpp) already hashes with. The golden test only overrides make_host()'s exp2_f (the support header's std::exp2, the system libm)
// with this, so the constants below do not depend on the build machine's libm.
float golden_exp2(float x) noexcept {
  if (!(x >= -126.0f)) {
    return 0.0f;  // underflow (2^-126 is the smallest normal float), and NaN
  }
  x = x > 127.0f ? 127.0f : x;
  int n = static_cast<int>(x);
  n = static_cast<float>(n) > x ? n - 1 : n;  // floor
  float f = x - static_cast<float>(n);         // [0, 1)
  if (f > 0.5f) {                              // [-1/2, 1/2]: the series converges fastest round zero
    f -= 1.0f;
    ++n;
  }
  // 2^f = sum (f ln 2)^k / k!, to k = 7: the first neglected term is below 6e-9 for |f| <= 1/2
  const float p = 1.0f + f * (0.6931471806f + f * (0.2402265070f + f * (0.0555041087f + f * (0.0096181291f +
                         f * (0.0013333558f + f * (0.0001540353f + f * 0.0000152527f))))));
  const std::uint32_t bits = static_cast<std::uint32_t>(n + 127) << 23;  // 2^n, n in [-126, 127]
  float scale;
  std::memcpy(&scale, &bits, sizeof(scale));
  return p * scale;
}

struct Param {
  std::uint32_t id;
  float value;
};

struct Case {
  std::string name;
  std::uint32_t note = 60, vel = 127, block = 64;
  int action = 0;  // 0: a note held; 1: a RAMP note over a held one, with pitch(); 2: a note off before the first sample
  std::vector<Param> params;
  std::uint32_t expected = 0;
};

std::vector<Case> build_cases() {
  std::vector<Case> cs;
  auto add = [&](std::string name, std::vector<Param> ps, std::uint32_t note = 60, std::uint32_t vel = 127, std::uint32_t block = 64, int action = 0) {
    cs.push_back(Case{std::move(name), note, vel, block, action, std::move(ps), 0});
  };
  const std::uint32_t W = CHIP_PARAM_WAVE, DUTY = CHIP_PARAM_DUTY, RAW = CHIP_PARAM_RAW_EDGES, STEPS = CHIP_PARAM_VOL_STEPS, VOL = CHIP_PARAM_VOLUME,
                      A = CHIP_PARAM_ATTACK, D = CHIP_PARAM_DECAY, S = CHIP_PARAM_SUSTAIN, R = CHIP_PARAM_RELEASE, SW = CHIP_PARAM_SWEEP, SWT = CHIP_PARAM_SWEEP_TIME,
                      VD = CHIP_PARAM_VIB_DEPTH, VR = CHIP_PARAM_VIB_RATE, VDEL = CHIP_PARAM_VIB_DELAY, A1 = CHIP_PARAM_ARP_1, A2 = CHIP_PARAM_ARP_2, AS = CHIP_PARAM_ARP_SPEED;
  const auto wave_name = [](std::uint32_t w, const char* what) {
    return "wave" + std::to_string(w) + "-" + what;
  };
  // each wave, at the defaults
  for (std::uint32_t w = 0; w < 4u; ++w) {
    add(wave_name(w, "default"), {{W, static_cast<float>(w)}});
  }
  // each wave, each duty (another note, so no case repeats another's script)
  for (std::uint32_t w = 0; w < 4u; ++w) {
    for (std::uint32_t d = 0; d < 4u; ++d) {
      add(wave_name(w, ("duty" + std::to_string(d)).c_str()), {{W, static_cast<float>(w)}, {DUTY, static_cast<float>(d)}}, 45);
    }
  }
  // each wave, Raw Edges on
  for (std::uint32_t w = 0; w < 4u; ++w) {
    add(wave_name(w, "raw"), {{W, static_cast<float>(w)}, {RAW, 1.0f}});
  }
  // each wave, Vol Steps on
  for (std::uint32_t w = 0; w < 4u; ++w) {
    add(wave_name(w, "steps"), {{W, static_cast<float>(w)}, {STEPS, 1.0f}});
  }
  // three envelopes, on each wave
  const std::vector<std::vector<Param>> envs = {
      {{A, 0.0f}, {D, 20.0f}, {S, 100.0f}, {R, 500.0f}},
      {{A, 200.0f}, {D, 800.0f}, {S, 30.0f}, {R, 1200.0f}},
      {{A, 10.0f}, {D, 0.0f}, {S, 80.0f}, {R, 10.0f}},
  };
  for (std::uint32_t w = 0; w < 4u; ++w) {
    for (std::size_t e = 0; e < envs.size(); ++e) {
      std::vector<Param> ps = {{W, static_cast<float>(w)}};
      ps.insert(ps.end(), envs[e].begin(), envs[e].end());
      add(wave_name(w, ("env" + std::to_string(e)).c_str()), std::move(ps));
    }
  }
  // sustain 0 (the note decays to nothing), on each wave
  for (std::uint32_t w = 0; w < 4u; ++w) {
    add(wave_name(w, "sus0"), {{W, static_cast<float>(w)}, {S, 0.0f}, {D, 300.0f}, {R, 50.0f}});
  }
  // an arpeggio of one / two / three steps
  add("arp1", {{A1, 0.0f}, {A2, 0.0f}, {AS, 120.0f}});
  add("arp2", {{A1, 7.0f}, {A2, 0.0f}, {AS, 120.0f}});
  add("arp3", {{A1, 7.0f}, {A2, -5.0f}, {AS, 120.0f}});
  // a sweep up and one down
  add("sweep-up", {{SW, 12.0f}, {SWT, 200.0f}});
  add("sweep-down", {{SW, -12.0f}, {SWT, 200.0f}});
  // a vibrato with delay
  add("vibrato", {{VD, 100.0f}, {VR, 8.0f}, {VDEL, 300.0f}});
  // velocity 1 / 96 / 127
  add("vel1", {}, 60, 1);
  add("vel96", {}, 60, 96);
  add("vel127", {}, 60, 127);
  // Volume -48 / 0 / +6 dB
  add("vol-48", {{VOL, -48.0f}});
  add("vol0", {{VOL, 0.0f}});
  add("vol+6", {{VOL, 6.0f}});
  // a RAMP note over a held one, with pitch()
  add("ramp-pitch", {}, 60, 127, 64, 1);
  // a note off before the first sample
  add("off-first", {}, 60, 127, 64, 2);
  // the block sizes 1 / 7 / 64 / 128
  for (std::uint32_t b : {1u, 7u, 64u, 128u}) {
    add("block" + std::to_string(b), {}, 72, 127, b);
  }
  return cs;
}

// The constants, in the order build_cases() lists the cases: FNV-1a of the 0.5 s of output bytes each case makes on dev (the voice of this tree, unchanged), recorded before
// any change to Chip (OS-CECHIP G1) and re-recorded once, with the libm-free golden_exp2 above in place of the support header's std::exp2 (the voice did not change; the review
// of G1). From G1 on no slice may change them.
constexpr std::uint32_t kGolden[] = {
    0xd371e03du, 0xc2f4c4a4u, 0xbb4a2f66u, 0x5e91a466u, 0xb7ac86fdu, 0xc6f155cbu, 0x045c6277u, 0x588416bau, 0xe6005210u, 0xe6005210u, 0xe6005210u, 0xe6005210u,
    0x201ed4e6u, 0x201ed4e6u, 0x201ed4e6u, 0x201ed4e6u, 0x9c4dbce6u, 0x9c4dbce6u, 0x9c4dbce6u, 0x9c4dbce6u, 0xee9a9419u, 0xc2f4c4a4u, 0xbb4a2f66u, 0x5e91a466u,
    0x2b9002a3u, 0xd323c485u, 0xbc79fc86u, 0x54aef786u, 0xbd8b0350u, 0xee393f74u, 0x60eac0ceu, 0x0136efb3u, 0xf21681c6u, 0x64fe501du, 0x3dd14dbfu, 0x81cbaa52u,
    0xb91b7b9bu, 0x6cfb8abfu, 0x74766a52u, 0xcd92c89bu, 0xacf147d4u, 0xf78d41c1u, 0xdc650759u, 0xf8b178d9u, 0xd371e03du, 0xeb88025eu, 0xcb56a551u, 0x4fceb9b4u,
    0x8bebb014u, 0x66bc5ca0u, 0xe4974882u, 0x3f0bd01eu, 0xd371e03du, 0x7f4a9efau, 0xe16d2077u, 0x98c59e7du, 0x0bc456b5u, 0xcb8d2b65u, 0x7e1b559fu, 0x7e1b559fu,
    0x7e1b559fu, 0x7e1b559fu,
};
constexpr std::size_t kGoldenCount = sizeof kGolden / sizeof kGolden[0];

}  // namespace

TEST_CASE("OS-CECHIP G1: the golden renders of today's Chip (FNV-1a of the output bytes, host)") {
  Lib lib;
  NEED(lib.open(O3M_CHIP_GOLDEN_HOST_SO));
  CHECK(std::string(lib.entry->name) == "Chip");
  std::vector<Case> cases = build_cases();
  NEED(cases.size() == kGoldenCount);
  for (std::size_t i = 0; i < cases.size(); ++i) {
    cases[i].expected = kGolden[i];
  }
  // The golden's host: make_host()'s tables, but exp2_f is the pure-arithmetic golden_exp2, not the system libm.
  o3m_host host = make_host();
  host.exp2_f = &golden_exp2;
  std::size_t n = 0;
  for (const Case& c : cases) {
    CAPTURE(c.name);
    CAPTURE(n);
    Instance inst;
    NEED(inst.start_with(*lib.entry, host));
    for (const Param& p : c.params) {
      inst.set(p.id, p.value);
    }
    Fnv fnv;
    if (c.action == 2) {  // a note off before the first sample: the voice goes idle at once
      inst.on(c.note, c.vel);
      inst.off(c.note);
      const std::vector<float> s = inst.render(kFrames, c.block);
      fnv.floats(s.data(), s.size());
    } else if (c.action == 1) {  // a RAMP note over the held one, and pitch() moving it
      inst.on(c.note, c.vel);
      const std::vector<float> s0 = inst.render(256, 64);
      inst.on(72, 100, O3M_NOTE_LEGATO | O3M_NOTE_RAMP);
      inst.m->pitch(inst.state, 7.0f, 0.25f);
      const std::vector<float> s1 = inst.render(kFrames - 256, c.block);
      fnv.floats(s0.data(), s0.size());
      fnv.floats(s1.data(), s1.size());
    } else {
      inst.on(c.note, c.vel);
      const std::vector<float> s = inst.render(kFrames, c.block);
      fnv.floats(s.data(), s.size());
    }
    std::printf("[chip golden] %2zu/%zu %-16s hash %08x (constant %08x)\n", n, cases.size(), c.name.c_str(), static_cast<unsigned>(fnv.h), static_cast<unsigned>(c.expected));
    CHECK_MESSAGE(fnv.h == c.expected, c.name << ": got " << std::hex << fnv.h << ", expected " << c.expected);
    ++n;
  }
}
