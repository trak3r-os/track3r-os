// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PERF-CPU3 slice 1 (docs/specs/OS-PERF-CPU3.md): the tail of VoicePool::note_on (the envelope or attack-ramp set-up, `dk`, `gated_`, `age`, `playing`, the note counter) moved to the end of
// mods_start (flash), so that note_on loses those bytes of SRAM_L. The move must not change a bit of the voices' state. This test drives a VoicePool through a randomized script (notes on all 16
// tracks over 12 voices, so retriggers and steals by `age` happen; six instruments: plain, enveloped, looped with every modulator, granular, beat slice, wavetable; the tracker FX, the step FX
// and the Perform layer that change what a note starts with; a gate mask; blocks rendered between notes) and hashes the WHOLE state after every note_on: each voice's bytes (the two pointers made
// relative), `gated_`, the age counter, the last voice, the pan-dirty mask, the previous-note memory, the stats and the return value. Seeds give one hash each; the expected values were captured
// from the code BEFORE the move (dev 03a0015e). Host only: the bytes of Voice differ on the Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/events.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace o3os::audio {
struct NoteOnStateTest {  // (friend of VoicePool: no code)
  struct Cover {
    unsigned notes = 0, retriggers = 0, steals = 0, granular = 0, reversed = 0, gated = 0, enveloped = 0, ramped = 0;
  };

  static std::uint64_t mix(std::uint64_t h, const void* p, std::size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (std::size_t k = 0; k < n; ++k) {
      h = (h ^ b[k]) * 1099511628211ull;
    }
    return h;
  }

  // The whole state a note_on can touch. A Voice is copied and its two pointers replaced by an offset (the sample buffer's address moves between runs).
  static std::uint64_t state(const VoicePool& p, const std::int16_t* base, bool result) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < kMaxVoices; ++i) {
      VoicePool::Voice c;
      std::memcpy(&c, &p.v_[i], sizeof c);
      c.data = c.data == nullptr ? nullptr : reinterpret_cast<const std::int16_t*>(static_cast<std::uintptr_t>(c.data - base) + 1u);
      c.env = c.env == nullptr ? nullptr : reinterpret_cast<const AmpEnvelope*>(std::uintptr_t{1});
      h = mix(h, &c, sizeof c);
    }
    h = mix(h, &p.gated_, sizeof p.gated_);
    h = mix(h, &p.age_, sizeof p.age_);
    h = mix(h, &p.last_idx_, sizeof p.last_idx_);
    h = mix(h, &p.pan_dirty_, sizeof p.pan_dirty_);
    h = mix(h, p.last_note1_, sizeof p.last_note1_);
    h = mix(h, &p.stats_, sizeof p.stats_);
    const std::uint8_t r = result ? 1u : 0u;
    return mix(h, &r, 1);
  }

  static bool track_has_voice(const VoicePool& p, unsigned track) { return p.find(track) != kMaxVoices; }
  static void count(const VoicePool& p, unsigned track, Cover& c) {
    const unsigned i = p.find(track);
    if (i == kMaxVoices) {
      return;
    }
    const VoicePool::Voice& v = p.v_[i];
    c.granular += v.mode == VoicePool::kModeGranular ? 1u : 0u;
    c.reversed += v.mode == VoicePool::kModeReverse ? 1u : 0u;
    c.gated += ((p.gated_ >> i) & 1u) != 0u ? 1u : 0u;
    c.enveloped += v.env != nullptr ? 1u : 0u;
    c.ramped += v.ramp_left != 0u ? 1u : 0u;
  }
};
}  // namespace o3os::audio

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os::audio;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr std::uint32_t kLen = 6000;

std::int16_t g_noise[kLen];
std::uint16_t g_slices[2u * 5u];  // four slices of 1500 frames: five boundaries as low / high halves

struct Rng {
  std::uint32_t s;
  std::uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
  unsigned below(unsigned n) { return next() % n; }
};

FilterMod lfo_mod(std::uint8_t shape, std::uint8_t rate, float amount) {
  FilterMod m;
  m.kind = kFmodLfo;
  m.lfo = true;
  m.shape = shape;
  m.rate = rate;
  m.amount = amount;
  m.lfo_amount = amount;
  return m;
}

FilterMod env_mod(std::uint16_t attack, std::uint16_t decay, float sustain, std::uint16_t release, float amount) {
  FilterMod m;
  m.kind = kFmodEnv;
  m.amount = amount;
  m.sustain = sustain;
  m.attack = attack;
  m.decay = decay;
  m.release = release;
  return m;
}

void init_samples(VoicePool& p) {
  Rng r{4242u};
  for (std::uint32_t i = 0; i < kLen; ++i) {
    g_noise[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(r.next() & 0x7FFFu) - 16384 + static_cast<std::int32_t>((i * 7u) % 9000u) - 4500);
  }
  for (unsigned k = 0; k < 5u; ++k) {
    const std::uint32_t b = k * 1500u;
    g_slices[2u * k] = static_cast<std::uint16_t>(b & 0xFFFFu);
    g_slices[2u * k + 1u] = static_cast<std::uint16_t>(b >> 16);
  }
  SampleDesc d{g_noise, kLen, 0, 0, 65536};
  p.set_sample(0, d);  // plain one shot

  SampleDesc e = d;  // amplitude envelope
  e.env.enable = true;
  e.env.delay = 3;
  e.env.attack = 20;
  e.env.hold = 5;
  e.env.decay = 40;
  e.env.release = 30;
  e.env.sustain = kEnvOne / 2u;
  p.set_sample(1, e);

  SampleDesc l = d;  // forward loop, filter with a cutoff ADSR, pan / finetune / tremolo LFOs, glide, overdrive
  l.loop_start = 500;
  l.loop_end = 5000;
  l.filter.enable = true;
  l.filter.wanted = true;
  l.filter.type = kFilterLow;
  l.filter.cutoff = 0.4f;
  l.filter.resonance = 1.0f;
  l.filter.mod = env_mod(10, 30, 0.5f, 20, 0.7f);
  l.pan_mod = lfo_mod(2, 12, 0.6f);
  l.tune_mod = lfo_mod(5, 10, 0.3f);
  l.amp_mod = lfo_mod(3, 6, 0.5f);
  l.glide = 120;
  l.overdrive = 30;
  l.bit_depth = 12;
  p.set_sample(2, l);

  SampleDesc g = d;  // granular
  g.granular = true;
  g.gr_len = 600;
  g.gr_pos = 20000;
  g.gr_shape = 2;
  g.loop_mode = 2;
  g.pos_mod = lfo_mod(2, 9, 0.5f);
  p.set_sample(3, g);

  SampleDesc b = d;  // beat slice
  b.beat_slice = true;
  b.slice_count = 4;
  b.slices = g_slices;
  p.set_sample(4, b);

  SampleDesc w = d;  // wavetable, three windows of 2000 frames
  w.wt_size = 2000;
  w.wt_windows = 3;
  w.wt_pos = 0;
  w.length = 2000;
  w.loop_start = 0;
  w.loop_end = 2000;
  w.pos_mod = lfo_mod(2, 8, 0.8f);
  p.set_sample(5, w);
}

SeqEvent ev(EventKind kind, unsigned track, unsigned group, unsigned a, unsigned b, unsigned layer, std::int32_t value) {
  SeqEvent e{};
  e.kind = kind;
  e.track = static_cast<std::uint8_t>(track);
  e.a = static_cast<std::uint8_t>(a);
  e.b = static_cast<std::uint8_t>(b);
  e.c = static_cast<std::uint8_t>(layer);
  e.flags = static_cast<std::uint8_t>(group);
  e.value = value;
  return e;
}

// One random FX event: a step FX (layer 0) or the Perform layer's row (layer 2), a set or an end.
void random_fx(VoicePool& p, Rng& r) {
  const unsigned track = r.below(kVoiceTracks);
  struct Row {
    std::uint8_t group, fx;
    int bmax;     // step FX value 0..bmax
    int lmin, lmax;  // the layer's value range
  };
  static const Row rows[] = {
      {kModGroupSlide, kFxSlideUp, 255, 0, 0},      {kModGroupSlide, kFxSlideDown, 255, 0, 0}, {kModGroupGlide, kFxGlide, 100, 0, 0},
      {kModGroupOverdrive, kFxOverdrive, 100, 0, 100}, {kModGroupBitDepth, kFxBitDepth, 16, 0, 15},  {kModGroupReverse, kFxReverse, 1, 0, 1},
      {kModGroupLfoVolume, kFxLfoVolume, 23, -28, 28}, {kModGroupLfoPanning, kFxLfoPanning, 28, -28, 28}, {kModGroupLfoFinetune, kFxLfoFinetune, 28, -28, 28},
      {kModGroupLfoPosition, kFxLfoPosition, 28, -28, 28}, {kModGroupLfoFilter, kFxFilterLfo, 28, -28, 28}, {kModGroupTune, 0, 0, -48, 48},
      {kModGroupPosition, 0, 0, -100, 100},          {kModGroupSampleEnd, 0, 0, -100, 100},
  };
  const Row& row = rows[r.below(sizeof rows / sizeof rows[0])];
  const bool layer = row.lmax != 0 || row.lmin != 0 ? (row.fx == 0u || r.below(2) == 0u) : false;
  const unsigned lay = layer ? kLayerPerf : kLayerFx1;
  if (r.below(5) == 0u) {
    p.fx_event(track, ev(EventKind::ModEnd, track, row.group, 0, 0, lay, 0));
    return;
  }
  const std::int32_t v = layer ? row.lmin + static_cast<std::int32_t>(r.below(static_cast<unsigned>(row.lmax - row.lmin + 1))) : 0;
  const unsigned b = layer ? 0u : r.below(static_cast<unsigned>(row.bmax) + 1u);
  p.fx_event(track, ev(EventKind::ModSet, track, row.group, row.fx, b, lay, v));
}

std::uint64_t run_seed(std::uint32_t seed, NoteOnStateTest::Cover& cov) {
  static o3os::test::StaticSlot<VoicePool> slot;
  VoicePool& p = slot.make();
  init_samples(p);
  Rng r{seed * 2654435761u + 7u};
  std::uint64_t acc = 1469598103934665603ull;
  for (unsigned step = 0; step < 900u; ++step) {
    const unsigned what = r.below(100);
    if (what < 55u) {
      const unsigned track = r.below(kVoiceTracks);
      const unsigned note = 24u + r.below(72);
      const unsigned slot_i = r.below(6);
      const unsigned vel = r.below(4) == 0u ? static_cast<unsigned>(kVelocityDefault) : 1u + r.below(127);
      const bool had = NoteOnStateTest::track_has_voice(p, track);
      const std::uint32_t steals = p.stats().steals;
      const bool ok = p.note_on(track, note, slot_i, vel);
      const std::uint64_t h = NoteOnStateTest::state(p, g_noise, ok);
      acc = NoteOnStateTest::mix(acc, &h, sizeof h);
      cov.notes += ok ? 1u : 0u;
      cov.retriggers += (ok && had) ? 1u : 0u;
      cov.steals += p.stats().steals - steals;
      if (ok) {
        NoteOnStateTest::count(p, track, cov);
      }
    } else if (what < 65u) {
      p.begin_block();
      p.render(0, kBlk);
      p.end_block();
    } else if (what < 72u) {
      const unsigned t = r.below(kVoiceTracks);
      switch (r.below(3)) {
        case 0: p.note_off(t); break;
        case 1: p.fade(t); break;
        default: p.cut(t); break;
      }
    } else if (what < 80u) {
      p.set_gate(r.next() & 0xFFFFu);
    } else {
      random_fx(p, r);
    }
  }
  return acc;
}

// Captured from the code before the move (dev 03a0015e, `note_on` with its own tail).
constexpr std::uint64_t kExpected[6] = {0xcf76b70cc10c8ef1ull, 0x14b021e6cc731b96ull, 0xb126568be39ca244ull, 0xb8df307f89522abeull, 0x6ef8a8be7a856b2bull, 0x5359da560c95ae82ull};

}  // namespace

TEST_CASE("note_on: the voices' whole state after each note matches the code before the tail moved to mods_start [OS-PERF-CPU3]") {
  NoteOnStateTest::Cover total;
  std::uint64_t got[6];
  for (unsigned k = 0; k < 6u; ++k) {
    NoteOnStateTest::Cover c;
    got[k] = run_seed(k + 1u, c);
    // the same script again in the same storage gives the same hash (the hash does not see an address or leftover padding)
    NoteOnStateTest::Cover again;
    CHECK(run_seed(k + 1u, again) == got[k]);
    total.notes += c.notes;
    total.retriggers += c.retriggers;
    total.steals += c.steals;
    total.granular += c.granular;
    total.reversed += c.reversed;
    total.gated += c.gated;
    total.enveloped += c.enveloped;
    total.ramped += c.ramped;
  }
  if (std::getenv("O3_PRINT_HASHES") != nullptr) {
    for (unsigned k = 0; k < 6u; ++k) {
      std::printf("kExpected[%u] = 0x%016llxull\n", k, static_cast<unsigned long long>(got[k]));
    }
    std::printf("cover notes=%u retriggers=%u steals=%u granular=%u reversed=%u gated=%u enveloped=%u ramped=%u\n", total.notes, total.retriggers, total.steals, total.granular, total.reversed,
                total.gated, total.enveloped, total.ramped);
  }
  // the script reaches every path the tail sits on
  CHECK(total.notes > 1000u);
  CHECK(total.retriggers > 50u);
  CHECK(total.steals > 20u);
  CHECK(total.granular > 20u);
  CHECK(total.reversed > 5u);
  CHECK(total.gated > 20u);
  CHECK(total.enveloped > 50u);
  CHECK(total.ramped > 50u);
  for (unsigned k = 0; k < 6u; ++k) {
    CHECK(got[k] == kExpected[k]);
  }
}
