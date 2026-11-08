// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CECHIP C2: the Chip module's new waves and parameters (docs/specs/OS-CECHIP.md 3.1 to 3.3), driven through the ABI from the module's host shared object, as the OS drives it. What this file
// holds: the descriptor of the 30 parameters (defaults, ranges, labels, pages within the ABI's limits); a project that has only the 20 old values (or whose new ones are at their defaults, or
// set to values that only the new waves read) renders waves 0..3 bit for bit the same; hostile values for ids 20..29 are clamped; every wave sounds; Chord (every wave) and ARP CHORD (Major
// while Chord is Off); block-size independence of the new path; Zap's drop; Shape and PWM only for waves 4..11. The module against the original engine is tests/cechip_oracle_test.cpp; the
// renders of today's waves are held by chip_golden_test.cpp (G1), which this slice leaves unchanged. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../tests/o3m_test_support.h"

#include "../chip_gen.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;

namespace {

// A flat voice as chip_test.cpp's: full volume, no velocity sensitivity, instant envelope limits, sustain at the top.
struct Fixture {
  Lib lib;
  Instance inst;
  bool ok = false;
  explicit Fixture(float rate = kRate) {
    ok = lib.open(O3M_CHIP_CE_HOST_SO) && inst.start(*lib.entry, rate);
    if (ok) {
      inst.set(CHIP_PARAM_VOLUME, 0.0f);
      inst.set(CHIP_PARAM_VEL_SENS, 0.0f);
      inst.set(CHIP_PARAM_ATTACK, 0.0f);
      inst.set(CHIP_PARAM_DECAY, 0.0f);
      inst.set(CHIP_PARAM_SUSTAIN, 100.0f);
      inst.set(CHIP_PARAM_RELEASE, 0.0f);
      inst.set(CHIP_PARAM_SWEEP_TIME, 0.0f);
      inst.set(CHIP_PARAM_RAW_EDGES, 1.0f);
      inst.set(CHIP_PARAM_DUTY, 2.0f);
    }
  }
};

constexpr std::uint32_t kFrames = 22050;

bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

float peak(const std::vector<float>& v) {
  float p = 0.0f;
  for (const float s : v) {
    p = std::fmax(p, std::fabs(s));
  }
  return p;
}

bool all_finite(const std::vector<float>& v) {
  for (const float s : v) {
    if (!std::isfinite(s)) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("chip ce: the descriptor has 30 parameters, 12 waves, 8 chords, and pages within the ABI's limits") {
  Lib lib;
  NEED(lib.open(O3M_CHIP_CE_HOST_SO));
  const o3m_module& m = *lib.entry;
  ModuleSet set;
  CHECK(set.add_native("chip", m));
  CHECK(m.param_count == 30u);
  CHECK(m.param_count == CHIP_PARAM_COUNT);
  CHECK(m.state_bytes <= CHIP_STATE_BYTES);
  CHECK(CHIP_STATE_BYTES == 336);  // C4 and C5: 312 B of voice became 332 B (the models' state, less two unread floats); 336 keeps twelve instances in the 4 KiB arena of 16-byte-aligned blocks
  CHECK(m.params[CHIP_PARAM_WAVE].max == 11.0f);
  struct Row {
    std::uint32_t id;
    const char* name;
    float min, max, def;
  };
  const Row rows[] = {
      {CHIP_PARAM_SHAPE, "Shape", 0, 100, 35},         {CHIP_PARAM_SUB_OSC, "Sub Osc", 0, 100, 0},    {CHIP_PARAM_CUTOFF, "Cutoff", 80, 18000, 18000},
      {CHIP_PARAM_RESONANCE, "Resonance", 0, 100, 0},  {CHIP_PARAM_BITCRUSH, "Bitcrush", 2, 16, 16},  {CHIP_PARAM_PWM_DEPTH, "PWM Depth", 0, 100, 20},
      {CHIP_PARAM_PWM_RATE, "PWM Rate", 0.1f, 40, 2.5f}, {CHIP_PARAM_ENV_AMT, "Env Amt", -100, 100, 0}, {CHIP_PARAM_DECIMATE, "Decimate", 1, 32, 1},
      {CHIP_PARAM_CHORD, "Chord", 0, 7, 0},
  };
  CHECK(CHIP_PARAM_SHAPE == 20);
  CHECK(CHIP_PARAM_CHORD == 29);
  for (const Row& r : rows) {
    CAPTURE(r.name);
    CHECK(std::string(m.params[r.id].name) == r.name);
    CHECK(m.params[r.id].min == r.min);
    CHECK(m.params[r.id].max == r.max);
    CHECK(m.params[r.id].def == r.def);
  }
  CHECK((m.params[CHIP_PARAM_CUTOFF].flags & O3M_PARAM_LOG) != 0u);
  CHECK((m.params[CHIP_PARAM_DECIMATE].flags & O3M_PARAM_INT) != 0u);
  CHECK((m.params[CHIP_PARAM_CHORD].flags & O3M_PARAM_INT) != 0u);
  NEED(m.labels != nullptr);
  CHECK(m.labels[CHIP_PARAM_WAVE].count == 12u);
  CHECK(std::string(m.labels[CHIP_PARAM_WAVE].names[4]) == "2A03 PULSE");
  CHECK(std::string(m.labels[CHIP_PARAM_WAVE].names[9]) == "ZAP/LASER");
  CHECK(std::string(m.labels[CHIP_PARAM_WAVE].names[10]) == "ARP CHORD");
  CHECK(std::string(m.labels[CHIP_PARAM_WAVE].names[11]) == "CHIP DRUM");
  CHECK(m.labels[CHIP_PARAM_CHORD].count == 8u);
  CHECK(std::string(m.labels[CHIP_PARAM_CHORD].names[0]) == "OFF");
  CHECK(std::string(m.labels[CHIP_PARAM_CHORD].names[7]) == "MIN7");
  NEED(m.pages != nullptr);
  CHECK(m.page_count == 4u);
  for (std::uint32_t k = 0; k < m.page_count; ++k) {
    const std::uint32_t end = k + 1u < m.page_count ? m.pages[k + 1].first_param : m.param_count;
    CHECK(end - m.pages[k].first_param <= O3M_PAGE_PARAMS_MAX);
  }
}

TEST_CASE("chip ce: a project with the 20 old values, or any value only the new waves read, renders waves 0..3 bit for bit the same") {
  for (std::uint32_t w = 0; w < 4u; ++w) {
    CAPTURE(w);
    Fixture a, b, c;
    NEED(a.ok && b.ok && c.ok);
    for (Fixture* f : {&a, &b, &c}) {
      f->inst.set(CHIP_PARAM_WAVE, static_cast<float>(w));
      f->inst.set(CHIP_PARAM_ARP_1, 4.0f);
      f->inst.set(CHIP_PARAM_ARP_2, 7.0f);
      f->inst.set(CHIP_PARAM_SWEEP, -5.0f);
      f->inst.set(CHIP_PARAM_SWEEP_TIME, 200.0f);
      f->inst.set(CHIP_PARAM_VIB_DEPTH, 40.0f);
    }
    // b: the ids 20..29 written back at their defaults; c: Shape, PWM Depth and PWM Rate (waves 4..11 only) and an explicit Chord Off
    for (std::uint32_t id = CHIP_PARAM_SHAPE; id < CHIP_PARAM_COUNT; ++id) {
      b.inst.set(id, b.lib.entry->params[id].def);
    }
    c.inst.set(CHIP_PARAM_SHAPE, 90.0f);
    c.inst.set(CHIP_PARAM_PWM_DEPTH, 100.0f);
    c.inst.set(CHIP_PARAM_PWM_RATE, 33.0f);
    c.inst.set(CHIP_PARAM_CHORD, 5.0f);
    c.inst.set(CHIP_PARAM_CHORD, 0.0f);
    for (Fixture* f : {&a, &b, &c}) {
      f->inst.on(57);
    }
    const std::vector<float> ra = a.inst.render(kFrames), rb = b.inst.render(kFrames), rc = c.inst.render(kFrames);
    CHECK(peak(ra) > 0.01f);
    CHECK(same_bits(ra, rb));
    CHECK(same_bits(ra, rc));
  }
}

TEST_CASE("chip ce: hostile values for the new parameters and for Wave are clamped, and the voice stays finite") {
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  const float hostile[] = {nan, inf, -inf, 1.0e30f, -1.0e30f, 0.5f, -3.0f};
  std::vector<std::uint32_t> ids = {CHIP_PARAM_WAVE};
  for (std::uint32_t id = CHIP_PARAM_SHAPE; id < CHIP_PARAM_COUNT; ++id) {
    ids.push_back(id);
  }
  for (const std::uint32_t id : ids) {
    for (const float h : hostile) {
      CAPTURE(id);
      CAPTURE(h);
      Fixture f;
      NEED(f.ok);
      f.inst.set(CHIP_PARAM_WAVE, 4.0f);
      f.inst.set(id, h);
      f.inst.on(60);
      const std::vector<float> out = f.inst.render(4096);
      CHECK(all_finite(out));
      CHECK(peak(out) < 8.0f);
      f.inst.off(60);
      CHECK(all_finite(f.inst.render(4096)));
    }
  }
}

TEST_CASE("chip ce: every one of the twelve waves sounds, finite and bounded") {
  for (std::uint32_t w = 0; w < 12u; ++w) {
    CAPTURE(w);
    Fixture f;
    NEED(f.ok);
    f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(w));
    f.inst.on(60);
    const std::vector<float> out = f.inst.render(kFrames);
    CHECK(all_finite(out));
    CHECK(peak(out) > 0.01f);
    CHECK(peak(out) < 2.0f);
  }
}

TEST_CASE("chip ce: Shape, PWM Depth and PWM Rate belong to waves 4..11; Duty and Raw Edges to waves 0..3") {
  const auto render_with = [](std::uint32_t wave, std::uint32_t id, float value) {
    Fixture f;
    f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(wave));
    f.inst.set(id, value);
    f.inst.on(64);
    return f.inst.render(kFrames);
  };
  for (const std::uint32_t id : {CHIP_PARAM_SHAPE, CHIP_PARAM_PWM_DEPTH, CHIP_PARAM_PWM_RATE}) {
    CAPTURE(id);
    CHECK(same_bits(render_with(0, id, 10.0f), render_with(0, id, 90.0f)));   // PULSE ignores them
    CHECK(!same_bits(render_with(4, id, 10.0f), render_with(4, id, 30.0f)));  // 2A03 PULSE reads them
  }
  CHECK(same_bits(render_with(4, CHIP_PARAM_DUTY, 0.0f), render_with(4, CHIP_PARAM_DUTY, 3.0f)));  // the new waves have no Duty
  CHECK(same_bits(render_with(4, CHIP_PARAM_RAW_EDGES, 0.0f), render_with(4, CHIP_PARAM_RAW_EDGES, 1.0f)));
}

TEST_CASE("chip ce: Chord replaces the arpeggio on every wave, at Arp Speed; ARP CHORD plays Major while Chord is Off") {
  const std::uint32_t step = 8820;  // Arp Speed 5 Hz: a step every 0.2 s
  const auto semis_of_steps = [&](std::uint32_t wave, float chord, long out[4]) {
    Fixture f;
    f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(wave));
    f.inst.set(CHIP_PARAM_ARP_SPEED, 5.0f);
    f.inst.set(CHIP_PARAM_CHORD, chord);
    f.inst.on(60);
    const std::vector<float> s = f.inst.render(step * 4 + 64);
    for (std::uint32_t k = 0; k < 4u; ++k) {
      const double hz = frequency(s.data() + k * step + 1500, step - 3000);
      out[k] = hz > 0.0 ? std::lround(12.0 * std::log2(hz / hz_of_note(60.0))) : -999;
    }
  };
  const long table[8][4] = {{0, 0, 0, 0}, {0, 12, 0, 12}, {0, 7, 12, 7}, {0, 4, 7, 12}, {0, 3, 7, 12}, {0, 5, 7, 12}, {0, 4, 7, 11}, {0, 3, 7, 10}};
  for (const std::uint32_t wave : {0u, 1u, 4u}) {
    for (std::uint32_t c = 1; c < 8u; ++c) {
      long got[4];
      semis_of_steps(wave, static_cast<float>(c), got);
      for (std::uint32_t k = 0; k < 4u; ++k) {
        CAPTURE(wave);
        CAPTURE(c);
        CAPTURE(k);
        CHECK(got[k] == table[c][k]);
      }
    }
  }
  long off[4];
  semis_of_steps(10, 0.0f, off);  // ARP CHORD, Chord Off: Major
  for (std::uint32_t k = 0; k < 4u; ++k) {
    CAPTURE(k);
    CHECK(off[k] == table[3][k]);
  }
  Fixture a, b;
  for (Fixture* f : {&a, &b}) {
    f->inst.set(CHIP_PARAM_WAVE, 10.0f);
    f->inst.set(CHIP_PARAM_ARP_SPEED, 20.0f);
  }
  b.inst.set(CHIP_PARAM_CHORD, 3.0f);
  a.inst.on(55);
  b.inst.on(55);
  CHECK(same_bits(a.inst.render(kFrames), b.inst.render(kFrames)));
}

TEST_CASE("chip ce: the new path renders the same bits for every block size") {
  const std::uint32_t blocks[] = {1, 7, 64, 128, 480};
  for (std::uint32_t wave = 4; wave < 12u; ++wave) {
    CAPTURE(wave);
    std::vector<float> first;
    for (const std::uint32_t block : blocks) {
      Fixture f;
      NEED(f.ok);
      f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(wave));
      f.inst.set(CHIP_PARAM_ATTACK, 5.0f);
      f.inst.set(CHIP_PARAM_DECAY, 100.0f);
      f.inst.set(CHIP_PARAM_SUSTAIN, 50.0f);
      f.inst.set(CHIP_PARAM_VOL_STEPS, 1.0f);
      f.inst.set(CHIP_PARAM_VIB_DEPTH, 50.0f);
      f.inst.set(CHIP_PARAM_SWEEP, 7.0f);
      f.inst.set(CHIP_PARAM_SWEEP_TIME, 150.0f);
      f.inst.set(CHIP_PARAM_ARP_1, 3.0f);
      f.inst.set(CHIP_PARAM_ARP_SPEED, 30.0f);
      f.inst.on(62);
      std::vector<float> out = f.inst.render(6000, block);
      f.inst.off(62);
      const std::vector<float> tail = f.inst.render(4000, block);
      out.insert(out.end(), tail.begin(), tail.end());
      if (first.empty()) {
        first = out;
      } else {
        CAPTURE(block);
        std::size_t d = 0;
        while (d < first.size() && d < out.size() && first[d] == out[d]) {
          ++d;
        }
        CAPTURE(d);
        CAPTURE(d < out.size() ? out[d] : 0.0f);
        CAPTURE(d < first.size() ? first[d] : 0.0f);
        CHECK(same_bits(first, out));
      }
    }
  }
}

TEST_CASE("chip ce: Zap's pitch starts high and drops over Sweep Time, and stays low") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(CHIP_PARAM_WAVE, 9.0f);
  f.inst.set(CHIP_PARAM_SWEEP_TIME, 1000.0f);
  f.inst.set(CHIP_PARAM_PWM_DEPTH, 0.0f);
  f.inst.on(84);
  const std::vector<float> s = f.inst.render(88200);
  const double early = frequency(s.data() + 500, 2000), late = frequency(s.data() + 60000, 4000);
  CHECK(early > 0.0);
  CHECK(late > 0.0);
  CHECK(early > late * 2.5);  // the drop at the default Shape of 35 % is 12 + 24 x 0.35 = 20.4 st over the sweep, a factor of 3.3 (a little less in the early window)
  // after the sweep the pitch is held
  const double a = frequency(s.data() + 50000, 6000), b = frequency(s.data() + 80000, 6000);
  CHECK(a == doctest::Approx(b).epsilon(0.01));
}

TEST_CASE("chip ce: the post chain changes waves 0..3 and the new ones, and renders the same bits for every block size") {
  const std::uint32_t blocks[] = {1, 7, 64, 128, 480};
  for (const std::uint32_t wave : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 11u}) {
    CAPTURE(wave);
    std::vector<float> first, dry;
    for (const std::uint32_t block : blocks) {
      Fixture f;
      NEED(f.ok);
      f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(wave));
      f.inst.set(CHIP_PARAM_ATTACK, 5.0f);
      f.inst.set(CHIP_PARAM_DECAY, 100.0f);
      f.inst.set(CHIP_PARAM_SUSTAIN, 50.0f);
      f.inst.set(CHIP_PARAM_SUB_OSC, 40.0f);
      f.inst.set(CHIP_PARAM_BITCRUSH, 6.0f);
      f.inst.set(CHIP_PARAM_DECIMATE, 3.0f);
      f.inst.set(CHIP_PARAM_CUTOFF, 1800.0f);
      f.inst.set(CHIP_PARAM_RESONANCE, 45.0f);
      f.inst.set(CHIP_PARAM_ENV_AMT, 30.0f);
      f.inst.set(CHIP_PARAM_SWEEP, 5.0f);
      f.inst.set(CHIP_PARAM_SWEEP_TIME, 150.0f);
      f.inst.on(55);
      std::vector<float> out = f.inst.render(6000, block);
      f.inst.off(55);
      const std::vector<float> tail = f.inst.render(4000, block);
      out.insert(out.end(), tail.begin(), tail.end());
      CHECK(all_finite(out));
      CHECK(peak(out) > 0.01f);
      if (first.empty()) {
        first = out;
      } else {
        CAPTURE(block);
        CHECK(same_bits(first, out));
      }
    }
    // the same note with the chain off differs (the chain is on: Sub, Bitcrush, Decimate, Cutoff, Resonance, Env Amt)
    Fixture d;
    NEED(d.ok);
    d.inst.set(CHIP_PARAM_WAVE, static_cast<float>(wave));
    d.inst.set(CHIP_PARAM_ATTACK, 5.0f);
    d.inst.set(CHIP_PARAM_DECAY, 100.0f);
    d.inst.set(CHIP_PARAM_SUSTAIN, 50.0f);
    d.inst.set(CHIP_PARAM_SWEEP, 5.0f);
    d.inst.set(CHIP_PARAM_SWEEP_TIME, 150.0f);
    d.inst.on(55);
    dry = d.inst.render(6000, 64);
    CHECK(!same_bits(std::vector<float>(first.begin(), first.begin() + 6000), dry));
  }
}

TEST_CASE("chip ce: setting every post-chain parameter back to its default gives waves 0..3 today's bits again") {
  for (std::uint32_t w = 0; w < 4u; ++w) {
    CAPTURE(w);
    Fixture a, b;
    NEED(a.ok && b.ok);
    for (Fixture* f : {&a, &b}) {
      f->inst.set(CHIP_PARAM_WAVE, static_cast<float>(w));
      f->inst.set(CHIP_PARAM_ARP_1, 3.0f);
      f->inst.set(CHIP_PARAM_VIB_DEPTH, 30.0f);
    }
    const std::uint32_t post[] = {CHIP_PARAM_SUB_OSC, CHIP_PARAM_CUTOFF, CHIP_PARAM_RESONANCE, CHIP_PARAM_BITCRUSH, CHIP_PARAM_ENV_AMT, CHIP_PARAM_DECIMATE};
    const float hostile[] = {70.0f, 500.0f, 80.0f, 5.0f, 60.0f, 9.0f};
    for (std::uint32_t k = 0; k < 6u; ++k) {
      b.inst.set(post[k], hostile[k]);
    }
    for (std::uint32_t k = 0; k < 6u; ++k) {
      b.inst.set(post[k], b.lib.entry->params[post[k]].def);
    }
    a.inst.on(60);
    b.inst.on(60);
    CHECK(same_bits(a.inst.render(kFrames), b.inst.render(kFrames)));
  }
}

// ---- OS-CECHIP C4 and C5: SID SAW/TRI, NOISE/LFSR, SUPER SQR, FM BELL, CHIP DRUM, through the module --------------------------------------------------------------------------------

TEST_CASE("chip ce: each of the five models has a voice of its own (they differ from each other and from the 2A03 pulse), finite and bounded") {
  std::vector<std::vector<float>> outs;
  for (const std::uint32_t wave : {4u, 5u, 6u, 7u, 8u, 11u}) {
    CAPTURE(wave);
    Fixture f;
    NEED(f.ok);
    f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(wave));
    f.inst.on(60);
    std::vector<float> out = f.inst.render(kFrames);
    CHECK(all_finite(out));
    CHECK(peak(out) > 0.05f);
    CHECK(peak(out) < 1.5f);
    outs.push_back(out);
  }
  for (std::size_t a = 0; a < outs.size(); ++a) {
    for (std::size_t b = a + 1; b < outs.size(); ++b) {
      CAPTURE(a);
      CAPTURE(b);
      CHECK(!same_bits(outs[a], outs[b]));
    }
  }
}

TEST_CASE("chip ce: Shape, PWM Depth and PWM Rate act where the original reads them, on each of the five models") {
  const auto render_with = [](std::uint32_t wave, std::uint32_t id, float value) {
    Fixture f;
    f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(wave));
    f.inst.set(id, value);
    f.inst.on(64);
    return f.inst.render(kFrames);
  };
  struct Row {
    std::uint32_t wave;
    bool shape, depth, rate;
  };
  // SID: the duty blends saw and triangle (Shape, Depth moving it at Rate); Noise: Shape picks the sequence, Depth the clock, no PWM LFO; Super Square: duty as SID; FM: Shape the ratio, Depth the index and the
  // LFO's phase modulation at Rate; Drum: Shape only
  const Row rows[] = {{5, true, true, true}, {6, true, true, false}, {8, true, true, true}, {7, true, true, true}, {11, true, false, false}};
  for (const Row& r : rows) {
    CAPTURE(r.wave);
    CHECK(same_bits(render_with(r.wave, CHIP_PARAM_SHAPE, 10.0f), render_with(r.wave, CHIP_PARAM_SHAPE, 90.0f)) != r.shape);
    CHECK(same_bits(render_with(r.wave, CHIP_PARAM_PWM_DEPTH, 10.0f), render_with(r.wave, CHIP_PARAM_PWM_DEPTH, 90.0f)) != r.depth);
    CHECK(same_bits(render_with(r.wave, CHIP_PARAM_PWM_RATE, 3.0f), render_with(r.wave, CHIP_PARAM_PWM_RATE, 30.0f)) != r.rate);
    CHECK(same_bits(render_with(r.wave, CHIP_PARAM_DUTY, 0.0f), render_with(r.wave, CHIP_PARAM_DUTY, 3.0f)));  // no Duty
    CHECK(same_bits(render_with(r.wave, CHIP_PARAM_RAW_EDGES, 0.0f), render_with(r.wave, CHIP_PARAM_RAW_EDGES, 1.0f)));
  }
}

TEST_CASE("chip ce: a second note after the voice went idle renders the first note's bits again (the register, the phases and the drum's decay restart)") {
  for (std::uint32_t wave = 4; wave < 12u; ++wave) {
    CAPTURE(wave);
    Fixture f;
    NEED(f.ok);
    f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(wave));
    f.inst.set(CHIP_PARAM_SUB_OSC, 30.0f);  // the post chain's state restarts too
    f.inst.set(CHIP_PARAM_CUTOFF, 3000.0f);
    f.inst.set(CHIP_PARAM_ARP_1, 5.0f);
    f.inst.on(55);
    const std::vector<float> a = f.inst.render(5000);
    f.inst.off(55);
    (void)f.inst.render(4000);  // the release is at least 1 ms and the voice goes idle
    f.inst.on(55);
    const std::vector<float> b = f.inst.render(5000);
    CHECK(same_bits(a, b));
  }
}

// ---- OS-MODRATE S5: Chip declares O3M_FX_RATE, so it is called at 22050 and 11025 Hz ---------------------------------------------------------------------

TEST_CASE("chip ce: OS-MODRATE: the descriptor declares O3M_FX_RATE at ABI 1.13") {
  Lib lib;
  NEED(lib.open(O3M_CHIP_CE_HOST_SO));
  CHECK(lib.entry->abi_minor == 13u);
  CHECK(lib.entry->fx_flags == static_cast<std::uint32_t>(O3M_FX_RATE));
}

TEST_CASE("chip ce: OS-MODRATE: at 22050 and 11025 Hz every wave stays finite and bounded, with the post chain on, from the lowest note to the highest") {
  for (const float rate : {22050.0f, 11025.0f}) {
    for (std::uint32_t w = 0; w < 12u; ++w) {
      for (const std::uint32_t note : {0u, 36u, 84u, 127u}) {
        CAPTURE(rate);
        CAPTURE(w);
        CAPTURE(note);
        Fixture f(rate);
        NEED(f.ok);
        f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(w));
        f.inst.set(CHIP_PARAM_SUB_OSC, 40.0f);  // the post chain: its lowpass at the highest Cutoff and at a low one, with a resonance
        f.inst.set(CHIP_PARAM_RESONANCE, 0.5f);
        f.inst.set(CHIP_PARAM_SWEEP, 24.0f);
        f.inst.set(CHIP_PARAM_SWEEP_TIME, 100.0f);
        f.inst.set(CHIP_PARAM_PWM_DEPTH, 0.0f);  // NOISE/LFSR: the fastest clock, 16 steps of the register a period
        for (const float cutoff : {18000.0f, 300.0f}) {
          f.inst.set(CHIP_PARAM_CUTOFF, cutoff);
          f.inst.on(note);
          const std::vector<float> out = f.inst.render(4096);  // the kernel's own assertion (the LFSR bound) traps in this host build
          CHECK(all_finite(out));
          CHECK(peak(out) < 8.0f);
          f.inst.off(note);
          CHECK(all_finite(f.inst.render(512)));
        }
      }
    }
  }
}

TEST_CASE("chip ce: OS-MODRATE: the pitch in Hz does not depend on the rate (the period in samples halves and quarters)") {
  // (SUPER SQR, wave 8, is left out: its detuned voices have no single zero-crossing pitch to count.)
  for (const std::uint32_t w : {0u, 1u, 4u, 5u}) {
    const auto measure = [w](float rate) {
      Fixture f(rate);
      f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(w));
      f.inst.on(48);  // 130.8 Hz
      f.inst.render(512);
      const std::vector<float> s = f.inst.render(16384);
      return frequency(s.data(), s.size(), static_cast<double>(rate));
    };
    const double full = measure(kRate);
    CAPTURE(w);
    CAPTURE(full);
    if (w <= 1u) {
      CHECK(full == doctest::Approx(hz_of_note(48)).epsilon(5e-3));  // the crossing counter is sure of these two
    }
    if (full > 0.0) {
      CHECK(measure(22050.0f) == doctest::Approx(full).epsilon(5e-3));
      CHECK(measure(11025.0f) == doctest::Approx(full).epsilon(5e-3));
    }
  }
}
