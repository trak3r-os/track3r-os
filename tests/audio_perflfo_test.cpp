// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PERF-LFO (docs/specs/OS-PERFFX.md section 11): the Perform rows VOL, PAN, TUNE, FLT and POS LFO act on the sampler voices. The row's value is a signed offset on the LFO speed index of the track's LFO
// for the destination. The oracle is the instrument's own LFO: a layer over an instrument whose slot is Off or an envelope is bit for bit the instrument whose slot is an LFO at index 14 + offset (the
// slot's stored shape and amount), and a layer over an LFO slot is the instrument with that LFO at the index moved by the offset. Also: the clamps, the sampler-only and the wavetable / granular-only rules,
// the layer across FxReset and Stopped, the layer over the step FX 35 and the step FX back, the layer's end, and an export with all five rows held that is byte for byte the export with Perform off.
// Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/event_source.h>

#include "support/lfo_probe.h"
#include "support/need.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define REQ LFO_REQ

namespace {

using audio::EventKind;
using audio::SeqEvent;

SeqEvent mod_set(unsigned group, std::uint8_t track, std::uint8_t a, std::uint8_t b, std::uint8_t layer, std::int32_t value) {
  SeqEvent e{};
  e.kind = EventKind::ModSet;
  e.track = track;
  e.a = a;
  e.b = b;
  e.c = layer;
  e.flags = static_cast<std::uint8_t>(group);
  e.value = value;
  return e;
}

SeqEvent mod_end(std::uint8_t track, unsigned group) {
  SeqEvent e{};
  e.kind = EventKind::ModEnd;
  e.track = track;
  e.flags = static_cast<std::uint8_t>(group);
  return e;
}

// The layer's group of a destination (VOL, PAN, TUNE, FLT, POS LFO = the groups 11, 12, 13, 14, 15) and the probe's destination of each (kAmp, kPan, kTune, kCut, kWt / kGran for POS).
constexpr unsigned kGroup[5] = {audio::kModGroupLfoVolume, audio::kModGroupLfoPanning, audio::kModGroupLfoFinetune, audio::kModGroupLfoFilter, audio::kModGroupLfoPosition};
unsigned group_of(Dest d) {
  switch (d) {
    case kAmp: return kGroup[0];
    case kPan: return kGroup[1];
    case kTune: return kGroup[2];
    case kCut: return kGroup[3];
    default: return kGroup[4];
  }
}

void hold(VoicePool& p, Dest d, int offset) { p.fx_event(0, mod_set(group_of(d), 0, 0, 0, audio::kLayerPerf, offset)); }

// The table index an offset gives over `base` (14: "1 step"), held to the table (the amp's is 5..28).
unsigned expect_index(Dest d, int base, int offset) {
  const int lo = d == kAmp ? static_cast<int>(audio::kAmpRateBase) : 0;
  const int i = base + offset;
  return static_cast<unsigned>(i < lo ? lo : (i > 28 ? 28 : i));
}

// The LFO instrument at table index `idx` (the amp's rate field is the index less 5).
audio::FilterMod lfo_at(Dest d, std::uint8_t shape, unsigned idx, float amount) {
  return lfo(shape, static_cast<std::uint8_t>(d == kAmp ? idx - audio::kAmpRateBase : idx), amount);
}

// A slot with no LFO that stores one: Off, or an ADSR; the stored shape and amount are what the layer's LFO plays, the stored rate is not used.
audio::FilterMod stored(std::uint8_t shape, float amount, bool envelope) {
  audio::FilterMod m;
  m.kind = envelope ? audio::kFmodEnv : audio::kFmodNone;
  m.shape = shape;
  m.rate = 3;
  m.amount = envelope ? 0.8f : 1.0f;
  m.lfo_amount = amount;
  m.sustain = 0.5f;
  m.attack = 40;
  m.decay = 80;
  m.release = 40;
  return m;
}

// `blocks` blocks of a note on track 0 of the descriptor, the transport stopped, the layer held before the note when `offset` is not 0.
std::vector<double> run(Dest d, const SampleDesc& desc, int offset, unsigned blocks) {
  VoicePool& p = pool(0);
  p.perf_clear();
  p.set_sample(0, desc);
  p.set_tempo_x100(kTempo);
  p.restart_clock();
  p.transport_stopped();
  if (offset != 0) {
    hold(p, d, offset);
  }
  std::vector<double> out;
  if (!p.note_on(0, 60, 0, audio::kVelocityDefault)) {
    return out;
  }
  p.mark_live();
  Probe pr{d, 0};
  for (unsigned b = 0; b < blocks; ++b) {
    out.push_back(step_block(p, pr));
  }
  p.perf_clear();
  return out;
}

// One block, the probe's value, except that the cutoff is read from the filter itself (the coefficient the post pass set last) and not from the rms of the noise table through it: that rms varies by itself from
// block to block, a threshold on it is not a test; the cutoff is a pure function of the LFO and the base.
double sb(VoicePool& p, Probe& pr) {
  const double r = step_block(p, pr);
  return pr.dest == kCut ? static_cast<double>(p.filter_cutoff(0)) : r;
}

constexpr unsigned kRun = 300;  // blocks: a bit more than a period of the 8-step LFO at 300 BPM
constexpr Dest kAll[] = {kAmp, kPan, kTune, kCut, kWt, kGran};

}  // namespace

static_assert(VoicePool::perf_slot_of(audio::kModGroupLfoVolume) == VoicePool::kPerfLfoSlot && VoicePool::perf_slot_of(audio::kModGroupLfoPosition) == VoicePool::kPerfLfoSlot + 4u &&
                  VoicePool::perf_slot_of(audio::kModGroupLfoFilter) == VoicePool::kPerfLfoSlot + 3u && VoicePool::kPerfLfoSlot + VoicePool::kLfoDests == VoicePool::kPerfSlots,
              "one slot a group, the five LFO slots last");

TEST_CASE("perform lfo: over a slot that is Off or an ADSR the layer is bit for bit the instrument's LFO at index 14 + offset, with the slot's stored shape and amount") {
  for (const Dest d : kAll) {
    for (const bool envelope : {false, true}) {
      if (d == kAmp && envelope) {
        continue;  // (the amp slot's ADSR is the player's envelope, not a modulator: the Off case covers the tremolo)
      }
      for (const int o : {-8, -3, 3, 14, 20, -28}) {
        const audio::FilterMod want = lfo_at(d, 2, expect_index(d, 14, o), 0.6f);
        const std::vector<double> ref = run(d, desc_for(d, want), 0, kRun);
        const std::vector<double> got = run(d, desc_for(d, stored(2, 0.6f, envelope)), o, kRun);
        REQ(ref.size() == kRun && got.size() == kRun);
        CHECK_MESSAGE(got == ref, "dest " << static_cast<unsigned>(d) << (envelope ? " ADSR" : " Off") << " offset " << o);
        CHECK(spread(ref, 40) > 0.0);  // the LFO moves something: a comparison of two constants proves nothing
      }
    }
  }
}

TEST_CASE("perform lfo: a slot that stores no audible amount (an old project, an OG file: 0, a negative amount) plays a triangle at 50 %, and the descriptor is left as it was") {
  for (const Dest d : kAll) {
    for (const float legacy : {0.0f, -0.4f, 0.01f}) {
      for (const bool envelope : {false, true}) {
        if (d == kAmp && envelope) {
          continue;
        }
        for (const int o : {-8, 3}) {
          const std::vector<double> ref = run(d, desc_for(d, lfo_at(d, 2, expect_index(d, 14, o), 0.5f)), 0, kRun);
          const SampleDesc desc = desc_for(d, stored(5, legacy, envelope));  // (the stored shape is not used either: the fallback is a triangle)
          const std::vector<double> got = run(d, desc, o, kRun);
          REQ(ref.size() == kRun && got.size() == kRun);
          CHECK_MESSAGE(got == ref, "dest " << static_cast<unsigned>(d) << " legacy amount " << legacy << (envelope ? " ADSR" : " Off") << " offset " << o);
          const audio::FilterMod& m = d == kAmp ? desc.amp_mod : (d == kPan ? desc.pan_mod : (d == kTune ? desc.tune_mod : (d == kCut ? desc.filter.mod : desc.pos_mod)));
          CHECK(m.lfo_amount == legacy);  // the shared descriptor is not patched
        }
      }
    }
  }
}

TEST_CASE("perform lfo: over an LFO slot the shape and amount stay and only the speed index moves, held to the table") {
  for (const Dest d : kAll) {
    const unsigned lo = d == kAmp ? audio::kAmpRateBase : 0u;
    for (const int o : {-4, 2, 20, -28}) {
      for (const std::uint8_t shape : {std::uint8_t{1}, std::uint8_t{3}, std::uint8_t{5}}) {
        const unsigned own = 10;  // table index of the instrument's own speed (4 steps)
        const std::vector<double> ref = run(d, desc_for(d, lfo_at(d, shape, expect_index(d, static_cast<int>(own), o), 0.7f)), 0, kRun);
        const std::vector<double> got = run(d, desc_for(d, lfo_at(d, shape, own, 0.7f)), o, kRun);
        REQ(ref.size() == kRun && got.size() == kRun);
        CHECK_MESSAGE(got == ref, "dest " << static_cast<unsigned>(d) << " shape " << static_cast<unsigned>(shape) << " offset " << o << " lo " << lo);
      }
    }
  }
}

TEST_CASE("perform lfo: the offset is held to +-28, the layer's slot is per track, and a 0, a step ModSet or a ModEnd lets it go") {
  VoicePool& p = pool(0);
  p.perf_clear();
  const auto value = [&](unsigned t, unsigned g) { return p.perf_held(t, g) ? p.perf_value(t, g) : -999; };
  p.fx_event(2, mod_set(audio::kModGroupLfoVolume, 2, 0, 0, audio::kLayerPerf, 1000));
  p.fx_event(2, mod_set(audio::kModGroupLfoPanning, 2, 0, 0, audio::kLayerPerf, -1000));
  p.fx_event(5, mod_set(audio::kModGroupLfoFinetune, 5, 0, 0, audio::kLayerPerf, 7));
  p.fx_event(5, mod_set(audio::kModGroupLfoPosition, 5, 0, 0, audio::kLayerPerf, -3));
  p.fx_event(5, mod_set(audio::kModGroupLfoFilter, 5, 0, 0, audio::kLayerPerf, 12));
  CHECK(value(2, audio::kModGroupLfoVolume) == 28);
  CHECK(value(2, audio::kModGroupLfoPanning) == -28);
  CHECK(value(5, audio::kModGroupLfoFinetune) == 7);
  CHECK(value(5, audio::kModGroupLfoPosition) == -3);
  CHECK(value(5, audio::kModGroupLfoFilter) == 12);
  CHECK(value(3, audio::kModGroupLfoVolume) == -999);  // another track: not held
  CHECK(value(2, audio::kModGroupLfoFinetune) == -999);  // another destination: not held
  CHECK(value(2, audio::kModGroupTune) == -999);         // and the other slots are not touched
  // FxReset (the transport start) and Stopped leave the layer
  p.restart_clock();
  p.clear_filter_fxs();
  p.clear_slide_fxs();
  p.transport_stopped();
  CHECK(value(2, audio::kModGroupLfoVolume) == 28);
  CHECK(value(5, audio::kModGroupLfoFilter) == 12);
  // a step ModSet of the group (the layer is not the winner) and a ModEnd let go
  p.fx_event(2, mod_set(audio::kModGroupLfoVolume, 2, 23, 5, audio::kLayerFx1, 0));
  p.fx_event(5, mod_end(5, audio::kModGroupLfoPosition));
  CHECK(value(2, audio::kModGroupLfoVolume) == -999);
  CHECK(value(5, audio::kModGroupLfoPosition) == -999);
  CHECK(value(2, audio::kModGroupLfoPanning) == -28);
  CHECK(value(5, audio::kModGroupLfoFilter) == 12);
  // the export puts the slots away and gives them back
  VoicePool::PerfSave save{};
  p.perf_save(save);
  p.perf_clear();
  CHECK(value(2, audio::kModGroupLfoPanning) == -999);
  p.perf_restore(save);
  CHECK(value(2, audio::kModGroupLfoPanning) == -28);
  CHECK(value(5, audio::kModGroupLfoFinetune) == 7);
  p.perf_clear();
}

TEST_CASE("perform lfo: POS acts on a wavetable or granular instrument only, FLT only where a filter runs; a plain sampler note is what it was") {
  // POS over a plain looping sampler: the descriptor has no position modulator, the note is bit for bit the one without the layer
  {
    const SampleDesc plain = desc_for(kAmp, audio::FilterMod{});
    const std::vector<double> ref = run(kAmp, plain, 0, 60);
    VoicePool& p = pool(0);
    p.perf_clear();
    p.set_sample(0, plain);
    p.set_tempo_x100(kTempo);
    p.restart_clock();
    p.transport_stopped();
    p.fx_event(0, mod_set(audio::kModGroupLfoPosition, 0, 0, 0, audio::kLayerPerf, 9));
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    p.mark_live();
    Probe pr{kAmp, 0};
    std::vector<double> got;
    for (unsigned b = 0; b < 60; ++b) {
      got.push_back(step_block(p, pr));
    }
    CHECK(got == ref);
    p.perf_clear();
  }
  // FLT over an instrument with no filter and none held: ignored; with a filter held by the layer (LOWPASS) it moves the cutoff
  {
    SampleDesc nofilter = desc_for(kCut, audio::FilterMod{});
    nofilter.filter.enable = nofilter.filter.wanted = false;
    const std::vector<double> ref = run(kCut, nofilter, 0, 80);
    const std::vector<double> got = run(kCut, nofilter, 3, 80);
    CHECK(got == ref);
    VoicePool& p = pool(0);
    p.perf_clear();
    p.set_sample(0, nofilter);
    p.set_tempo_x100(kTempo);
    p.restart_clock();
    p.transport_stopped();
    p.set_filter(0, false, 0, 0, 1u, -20);  // the layer's LOWPASS at -20 (type + 1, offset)
    hold(p, kCut, -2);
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    p.mark_live();
    Probe pr{kCut, 0};
    std::vector<double> with_lfo;
    for (unsigned b = 0; b < kRun; ++b) {
      with_lfo.push_back(sb(p, pr));
    }
    p.perf_clear();
    p.set_filter(0, false, 0, 0, 0u, 0);
    p.cut(0);
    p.set_filter(0, false, 0, 0, 1u, -20);
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    p.mark_live();
    std::vector<double> held_only;
    for (unsigned b = 0; b < kRun; ++b) {
      held_only.push_back(sb(p, pr));
    }
    p.set_filter(0, false, 0, 0, 0u, 0);
    CHECK(spread(with_lfo, 40) > 0.05);   // the cutoff sweeps under the LFO ...
    CHECK(spread(held_only, 40) < 1e-6);  // ... and the held filter alone keeps it where it is
  }
}

TEST_CASE("perform lfo: the layer starts on the sounding voice, moves its speed with the phase kept, and ends back at the instrument's own modulator") {
  for (const Dest d : {kAmp, kPan, kTune, kCut, kWt}) {
    VoicePool& p = pool(0);
    p.perf_clear();
    p.set_sample(0, desc_for(d, stored(2, 1.0f, false)));
    p.set_tempo_x100(kTempo);
    p.restart_clock();
    p.transport_stopped();
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    p.mark_live();
    Probe pr{d, 0};
    std::vector<double> v;
    for (unsigned b = 0; b < 40; ++b) {
      v.push_back(sb(p, pr));
    }
    const double quiet = spread(v, 10);
    hold(p, d, 6);  // starts
    for (unsigned b = 0; b < 300; ++b) {
      v.push_back(sb(p, pr));
    }
    CHECK_MESSAGE(spread(v, 40) > 20.0 * quiet + 1e-6, "dest " << static_cast<unsigned>(d) << " the LFO starts on the sounding voice");
    p.fx_event(0, mod_end(0, group_of(d)));  // ends
    const std::size_t at = v.size();
    for (unsigned b = 0; b < 100; ++b) {
      v.push_back(step_block(p, pr));
    }
    if (d == kPan) {
      CHECK(p.pan_mod_value(0) == static_cast<unsigned>(desc_for(d, stored(2, 1.0f, false)).pan));  // the instrument's pan comes back
    } else if (d == kAmp || d == kTune || d == kWt) {
      std::vector<double> tail(v.begin() + static_cast<std::ptrdiff_t>(at) + 60, v.end());
      CHECK_MESSAGE(spread(tail, 0) <= 1e-9 + 5.0 * quiet, "dest " << static_cast<unsigned>(d) << " the instrument's own state is back");
    }
    p.perf_clear();
  }
}

TEST_CASE("perform lfo: a live edit of the slot's amount moves the sounding layer's LFO between the 50 % fallback and the stored amount, as a note with that amount does") {
  static constexpr unsigned kSlotOf[kDests] = {0, 1, 5, 2, 3, 4};  // the instrument's modulation slot of each destination (audio_lfolive_test)
  for (const Dest d : {kAmp, kPan, kTune, kCut, kWt}) {
    for (const bool up : {true, false}) {
      const float from = up ? 0.0f : 0.8f;
      const float to = up ? 0.8f : 0.0f;
      VoicePool& a = pool(0);
      VoicePool& c = pool(2);
      for (VoicePool* p : {&a, &c}) {
        p->perf_clear();
        p->set_sample(0, desc_for(d, stored(2, p == &a ? from : to, false)));
        p->set_tempo_x100(kTempo);
        p->restart_clock();
        p->transport_stopped();
        hold(*p, d, 3);
        REQ(p->note_on(0, 60, 0, audio::kVelocityDefault));
        p->mark_live();
      }
      Probe pa{d, 0}, pc{d, 0};
      unsigned equal = 0, total = 0;
      const unsigned at = d == kTune ? 0u : 70u;  // (the tune keeps its history in the sample position: only an edit before the first block gives the same series as a note that had the amount)
      for (unsigned b = 0; b < 2 * kPeriodBlocks; ++b) {
        if (b == at) {
          std::uint32_t pan = 0;
          CHECK(a.apply_param(0, ip::lfo(kSlotOf[d], ip::LfoAmount), to, pan));
        }
        const double xa = sb(a, pa);
        const double xc = sb(c, pc);
        if (b > at + 1u) {  // (the tremolo's multiplier walks to its new target over the first block)
          ++total;
          equal += xa == xc ? 1u : 0u;
        }
      }
      CHECK_MESSAGE(equal == total, "dest " << static_cast<unsigned>(d) << " amount " << from << " -> " << to << ": " << equal << " of " << total << " blocks equal");
      a.perf_clear();
      c.perf_clear();
    }
  }
}

TEST_CASE("perform lfo: the layer's end puts the cutoff back at the unmodulated base (the filter pass no longer evaluates it), whatever the instrument's modulator was") {
  for (const int kind : {0, 1}) {  // 0: Off, 1: an ADSR
    VoicePool& p = pool(0);
    p.perf_clear();
    p.set_sample(0, desc_for(kCut, stored(2, 1.0f, kind == 1)));
    p.set_tempo_x100(kTempo);
    p.restart_clock();
    p.transport_stopped();
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    p.mark_live();
    Probe pr{kCut, 0};
    hold(p, kCut, 4);
    float lo = 9.0f, hi = -9.0f;
    for (unsigned b = 0; b < 300; ++b) {
      (void)step_block(p, pr);
      lo = std::min(lo, p.filter_cutoff(0));
      hi = std::max(hi, p.filter_cutoff(0));
    }
    CHECK(hi - lo > 0.1f);  // the LFO sweeps the cutoff
    p.fx_event(0, mod_end(0, audio::kModGroupLfoFilter));
    for (unsigned b = 0; b < 3; ++b) {
      (void)step_block(p, pr);
    }
    if (kind == 0) {
      const float c0 = p.filter_cutoff(0);
      for (unsigned b = 0; b < 40; ++b) {
        (void)step_block(p, pr);
        CHECK(p.filter_cutoff(0) == c0);  // stays: no modulator is left
      }
      CHECK(std::fabs(c0 - 0.5f) < 1.0e-6f);
    }
    p.perf_clear();
  }
}

TEST_CASE("perform lfo: a retrigger and a steal with the layer held start the LFO again; a gated track and the Filter LFO FX's end (FxReset) leave it") {
  for (const Dest d : {kAmp, kPan, kTune, kCut}) {
    VoicePool& p = pool(0);
    p.perf_clear();
    p.set_sample(0, desc_for(d, stored(2, 1.0f, false)));
    p.set_sample(1, desc_for(d, stored(2, 1.0f, false)));
    p.set_tempo_x100(kTempo);
    p.restart_clock();
    p.transport_stopped();
    hold(p, d, 5);
    Probe pr{d, 0};
    std::vector<double> v;
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    p.mark_live();
    for (unsigned b = 0; b < 60; ++b) {
      v.push_back(sb(p, pr));
    }
    REQ(p.note_on(0, 62, 0, audio::kVelocityDefault));  // a retrigger of the track's voice
    p.mark_live();
    for (unsigned b = 0; b < 300; ++b) {
      v.push_back(sb(p, pr));
    }
    CHECK_MESSAGE(spread(v, 100) > 0.0, "dest " << static_cast<unsigned>(d) << " retrigger");
    REQ(p.note_on(0, 64, 1, audio::kVelocityDefault));  // another instrument on the track
    p.mark_live();
    std::vector<double> w;
    for (unsigned b = 0; b < 300; ++b) {
      w.push_back(sb(p, pr));
    }
    CHECK_MESSAGE(spread(w, 20) > 0.0, "dest " << static_cast<unsigned>(d) << " another instrument");
    // a gated track: the voice skips its post stages for a while, and the layer is still there when it is heard again
    p.set_gate(1u);
    for (unsigned b = 0; b < 30; ++b) {
      p.begin_block();
      p.render(0, kBlk);
      p.end_block();
    }
    p.set_gate(0u);
    // the end of the Filter LFO FX 35 (FxReset: lfo_set_ for every track) does not take the layer's FLT LFO
    p.clear_filter_fxs();
    std::vector<double> x;
    for (unsigned b = 0; b < 300; ++b) {
      x.push_back(sb(p, pr));
    }
    CHECK_MESSAGE(spread(x, 20) > 0.0, "dest " << static_cast<unsigned>(d) << " after the gate and FxReset");
    p.perf_clear();
  }
}

TEST_CASE("perform lfo: the group 14 keeps the Filter LFO FX's rules - a ModSet starts its LFO only when a is 35 (b the rate), the layer alone does not, a ModEnd ends it") {
  const auto run14 = [&](unsigned how) {
    VoicePool& p = pool(0);
    p.perf_clear();
    p.set_filter_lfo(0, false, 0);
    p.set_sample(0, desc_for(kCut, audio::FilterMod{}));
    p.set_tempo_x100(kTempo);
    p.restart_clock();
    p.transport_stopped();
    if (how == 1u) {
      p.fx_event(0, mod_set(audio::kModGroupLfoFilter, 0, audio::kFxFilterLfo, 8, audio::kLayerFx1, 0));  // the FX
    } else if (how == 2u) {
      p.fx_event(0, mod_set(audio::kModGroupLfoFilter, 0, 0, 0, audio::kLayerFx1, 0));  // a ModSet with no FX 35 in it: no LFO
    } else if (how == 3u) {
      p.fx_event(0, mod_set(audio::kModGroupLfoFilter, 0, audio::kFxFilterLfo, 8, audio::kLayerFx1, 0));
      p.fx_event(0, mod_end(0, audio::kModGroupLfoFilter));  // started, then ended
    } else if (how == 4u) {
      p.fx_event(0, mod_set(audio::kModGroupLfoFilter, 0, audio::kFxFilterLfo, 8, audio::kLayerFx1, 0));
      p.fx_event(0, mod_set(audio::kModGroupLfoFilter, 0, 0, 0, audio::kLayerPerf, 0));  // the layer's ModSet with no FX underneath (a == 0) ends the FX's LFO
    }
    (void)p.note_on(0, 60, 0, audio::kVelocityDefault);
    p.mark_live();
    Probe pr{kCut, 0};
    std::vector<double> v;
    for (unsigned b = 0; b < kRun; ++b) {
      v.push_back(sb(p, pr));
    }
    p.set_filter_lfo(0, false, 0);
    return v;
  };
  const std::vector<double> plain = run14(0);
  CHECK(spread(run14(1), 40) > 10.0 * spread(plain, 40) + 1e-6);
  CHECK(run14(2) == plain);
  CHECK(run14(3) == plain);
  CHECK(run14(4) == plain);
}

TEST_CASE("perform lfo: FLT over the step FX 35 - the layer's speed wins, the FX's comes back with the layer's end, and the FX alone is as it was") {
  // a filter with no modulator; the step FX 35 at index 8 on track 0, then the layer's FLT LFO +3 over it (the FX's type and rate stay in a and b), then the layer ends (the step ModSet again)
  const auto cut_run = [&](bool step, int layer_offset, bool layer_end) {
    VoicePool& p = pool(0);
    p.perf_clear();
    p.set_filter_lfo(0, false, 0);
    p.set_sample(0, desc_for(kCut, audio::FilterMod{}));
    p.set_tempo_x100(kTempo);
    p.restart_clock();
    p.transport_stopped();
    if (step) {
      p.fx_event(0, mod_set(audio::kModGroupLfoFilter, 0, audio::kFxFilterLfo, 8, audio::kLayerFx1, 0));
    }
    if (layer_offset != 0) {
      p.fx_event(0, mod_set(audio::kModGroupLfoFilter, 0, step ? audio::kFxFilterLfo : 0, step ? 8 : 0, audio::kLayerPerf, layer_offset));
    }
    p.note_on(0, 60, 0, audio::kVelocityDefault);
    p.mark_live();
    Probe pr{kCut, 0};
    std::vector<double> v;
    for (unsigned b = 0; b < kRun; ++b) {
      v.push_back(sb(p, pr));
    }
    if (layer_end) {
      p.fx_event(0, mod_set(audio::kModGroupLfoFilter, 0, audio::kFxFilterLfo, 8, audio::kLayerFx1, 0));  // the control table's winner is the step FX again
      for (unsigned b = 0; b < kRun; ++b) {
        v.push_back(sb(p, pr));
      }
    }
    p.set_filter_lfo(0, false, 0);
    p.perf_clear();
    return v;
  };
  const std::vector<double> fx_only = cut_run(true, 0, false);
  const std::vector<double> layer_only = cut_run(false, 3, false);
  const std::vector<double> both = cut_run(true, 3, false);
  const std::vector<double> ended = cut_run(true, 3, true);
  REQ(fx_only.size() == kRun && both.size() == kRun && ended.size() == 2u * kRun);
  CHECK(both == layer_only);  // the layer wins whatever the FX holds underneath: index 14 + 3, not 8
  CHECK(both != fx_only);
  const std::vector<double> fx_alone_long = cut_run(true, 0, false);
  // after the layer's end the cutoff LFO runs at the FX's rate again: the tail has the FX-only series' spread, within the filter's own memory
  std::vector<double> tail(ended.begin() + static_cast<std::ptrdiff_t>(kRun) + 40, ended.end());
  std::vector<double> ref(fx_alone_long.begin() + 40, fx_alone_long.end());
  CHECK(std::fabs(spread(tail, 0) - spread(ref, 0)) < 0.25 * spread(ref, 0));
  // and the step FX without the layer is untouched by the change of the group's route (the old Engine branch)
  CHECK(spread(fx_only, 40) > 0.0);
}

// ---- the export ---------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

SeqEvent g_script[8];
std::uint32_t g_script_n = 0;
std::uint32_t scripted(void*, std::uint64_t, std::uint32_t, SeqEvent* out, std::uint32_t cap) noexcept {
  std::uint32_t n = 0;
  for (; n < g_script_n && n < cap; ++n) {
    out[n] = g_script[n];
  }
  g_script_n = 0;
  return n;
}
std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return g_cycles += 5u; }

SeqEvent note_on_ev() {
  SeqEvent on{};
  on.kind = EventKind::NoteOn;
  on.a = 60;
  on.b = 0;
  on.c = 127;
  return on;
}

std::uint32_t export_step(void*, std::uint64_t start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
  const std::uint32_t block = static_cast<std::uint32_t>(start / n);
  std::uint32_t k = 0;
  if (block == 0u && cap >= 2u) {
    // the sequencer's start, as an export's source gives it: the FX reset (it restarts the LFOs' tick count: without it the export's LFO phase would be the live clock's) and the first step
    SeqEvent reset{};
    reset.kind = EventKind::FxReset;
    reset.track = audio::kNoTrack;
    SeqEvent step{};
    step.kind = EventKind::Step;
    step.track = audio::kNoTrack;
    out[k++] = reset;
    out[k++] = step;
  }
  if (block == 1u && k < cap) {
    out[k++] = note_on_ev();
  }
  return k;
}

struct Rig {
  Rig() : engine(slot.make(&fake_cycles, taps)), p(pool(1)) {
    fill();
    SampleDesc d = desc_for(kCut, lfo(2, 6, 0.5f));
    d.filter.mod = audio::FilterMod{};
    d.pan_mod = lfo(2, 3, 0.5f);
    d.tune_mod = lfo(2, 5, 0.3f);
    p.set_sample(0, d);
    engine.attach_voices(&p);
    audio::EventSource src;
    src.step = &scripted;
    src.buffer = events;
    src.capacity = audio::kMaxEventsPerBlock;
    engine.attach_events(src);
  }
  void live(std::initializer_list<SeqEvent> list) {
    g_script_n = 0;
    for (const SeqEvent& s : list) {
      g_script[g_script_n++] = s;
    }
    haudio::BlockReport report{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine.render_block(io);
  }
  static inline o3os::test::StaticSlot<audio::Engine> slot;
  static inline audio::TapMem taps;
  static inline SeqEvent events[audio::kMaxEventsPerBlock];
  alignas(8) static inline std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) static inline std::int16_t out[haudio::kBlockSamples] = {};
  audio::Engine& engine;
  VoicePool& p;
};

struct ExportRun {
  std::vector<std::int16_t> exported;
  std::vector<std::int16_t> live_note;
};

ExportRun export_run(bool held, bool export_between) {
  ExportRun run;
  Rig r;
  r.p.perf_clear();
  r.p.cut(0);
  r.live({});
  r.live({});
  if (held) {
    r.live({mod_set(audio::kModGroupLfoVolume, 0, 0, 0, audio::kLayerPerf, 4), mod_set(audio::kModGroupLfoPanning, 0, 0, 0, audio::kLayerPerf, -5),
            mod_set(audio::kModGroupLfoFinetune, 0, 0, 0, audio::kLayerPerf, 6), mod_set(audio::kModGroupLfoFilter, 0, 0, 0, audio::kLayerPerf, 2),
            mod_set(audio::kModGroupLfoPosition, 0, 0, 0, audio::kLayerPerf, 3)});
    r.live({});
  }
  r.live({note_on_ev()});  // a note sounding under the layer when the export is asked: its layer-driven pan / tune / LFO state must not reach the export (offline_enter) nor outlive it
  for (unsigned b = 0; b < 8; ++b) {
    r.live({});
  }
  if (export_between) {
    audio::EventSource src;
    src.step = &export_step;
    src.buffer = r.events;
    src.capacity = audio::kMaxEventsPerBlock;
    r.engine.offline_request();
    CHECK(r.engine.offline_ready(false));
    r.engine.offline_enter(src);
    CHECK(r.engine.offline_entered());
    for (std::uint32_t b = 0; b < 40; ++b) {
      haudio::BlockReport report{};
      const haudio::BlockIo io{Rig::in, Rig::out, 0, &report};
      r.engine.render_offline(io);
      run.exported.insert(run.exported.end(), Rig::out, Rig::out + haudio::kBlockSamples);
    }
    r.engine.offline_leave();
    r.live({});
    r.live({});
  } else {  // (the same without the export: the note ends here, as offline_enter ends it, and its declick fade is out before the next note)
    r.p.cut(0);
    r.live({});
    r.live({});
  }
  SeqEvent reset{};  // (a transport start in both flows: the LFOs' tick count is the same at the note, the export's own start having restarted it)
  reset.kind = EventKind::FxReset;
  reset.track = audio::kNoTrack;
  r.live({reset, note_on_ev()});
  for (unsigned b = 0; b < 60; ++b) {
    run.live_note.insert(run.live_note.end(), Rig::out, Rig::out + haudio::kBlockSamples);
    r.live({});
  }
  r.live({mod_end(0, audio::kModGroupLfoVolume), mod_end(0, audio::kModGroupLfoPanning), mod_end(0, audio::kModGroupLfoFinetune), mod_end(0, audio::kModGroupLfoFilter),
          mod_end(0, audio::kModGroupLfoPosition)});
  r.p.perf_clear();
  return run;
}

}  // namespace

TEST_CASE("perform lfo: an export with the five rows held is byte for byte the export with Perform off, and the live note after it is as it is without an export") {
  const ExportRun plain = export_run(false, true);
  const ExportRun held = export_run(true, true);
  const ExportRun no_export = export_run(true, false);
  CHECK(plain.exported.size() == held.exported.size());
  NEED(plain.exported.size() == held.exported.size() && plain.exported.size() > haudio::kBlockSamples);
  // (the first export block carries the declick fade of the note that sounded when the export was asked, which is live audio and differs with the layer: the export proper starts at block 1)
  CHECK(std::equal(plain.exported.begin() + haudio::kBlockSamples, plain.exported.end(), held.exported.begin() + haudio::kBlockSamples));
  std::int32_t peak = 0;
  for (const std::int16_t v : plain.exported) {
    peak = std::max<std::int32_t>(peak, v < 0 ? -v : v);
  }
  CHECK(peak > 1000);  // the export sounded: a comparison of silence proves nothing
  NEED(held.live_note.size() == no_export.live_note.size());
  CHECK(held.live_note == no_export.live_note);  // the layer is back after the export
  CHECK(held.live_note != plain.live_note);      // ... and it acts
  const ExportRun again = export_run(true, true);
  CHECK(again.exported == held.exported);  // deterministic
  CHECK(again.live_note == held.live_note);
}

// ---- OS-PERF-CPU2 P1: the pan is sent to the strip only when it changed -----------------------------------------------------------------------------------------------
//
// The engine output of a held PAN LFO (the instrument's own, and the Perform row), of all five rows, of a Panning FX over a held LFO and the FX's end, and of a live instrument-pan edit under an LFO that is
// clamped at 0 / 100, as a hash over every output sample. The expected values were captured on dev (599c4b7c) before Engine::apply_pan_mods compared the pan with the strip's own: the change is
// bit-identical, so they must not move. The other writers of the strip's pan (Panning FX, its end, the instrument edit) are the reason the comparison is with the strip's value and not a cached one.

namespace {

std::uint64_t fnv_mix(std::uint64_t h, std::uint16_t x) {
  h ^= x;
  return h * 1099511628211ull;
}

SeqEvent pan_fx(std::uint8_t value) { return mod_set(audio::kModGroupPanning, 0, audio::kFxPanning, value, audio::kLayerFx1, 0); }

std::uint64_t pan_scene(unsigned scene) {
  Rig r;
  r.p.perf_clear();
  r.p.cut(0);
  if (scene >= 5u) {  // an instrument at the edge of the range and an LFO deep enough to be held at 0 / 100 for part of its period
    SampleDesc d = desc_for(kCut, lfo(2, 6, 0.5f));
    d.filter.mod = audio::FilterMod{};
    d.pan = static_cast<std::uint8_t>(scene == 5u ? 100u : (scene == 6u ? 0u : 70u));
    d.pan_mod = lfo(2, 3, scene >= 7u ? 0.004f : 1.0f);  // 7 / 8: an LFO too shallow to move the pan (the value is the same every block, whole pan units): the FX alone moves it, or would, if the LFO did not put its value over it
    r.p.set_sample(0, d);
  }
  r.live({});
  const SeqEvent row = mod_set(audio::kModGroupLfoPanning, 0, 0, 0, audio::kLayerPerf, 3);
  if (scene == 1u || scene == 4u) {
    r.live({row});
  } else if (scene == 2u) {
    r.live({row, mod_set(audio::kModGroupLfoVolume, 0, 0, 0, audio::kLayerPerf, 4), mod_set(audio::kModGroupLfoFinetune, 0, 0, 0, audio::kLayerPerf, 6),
            mod_set(audio::kModGroupLfoFilter, 0, 0, 0, audio::kLayerPerf, 2), mod_set(audio::kModGroupLfoPosition, 0, 0, 0, audio::kLayerPerf, 3)});
  }
  r.live({note_on_ev()});
  std::uint64_t h = 1469598103934665603ull;
  for (unsigned b = 0; b < 56; ++b) {
    const bool fx = scene == 3u || scene == 4u || scene == 7u;
    if (fx && b == 12u) {
      r.live({pan_fx(20)});
    } else if (fx && b == 30u) {
      r.live({mod_end(0, audio::kModGroupPanning)});
    } else if ((scene == 5u || scene == 6u) && (b == 10u || b == 22u || b == 34u)) {
      const float v = b == 10u ? 30.0f : (b == 22u ? 100.0f : 0.0f);
      CHECK(r.engine.params().push(o3os::core::ParamChange{o3os::core::param_id(o3os::core::ParamKind::Instrument, 0, audio::inst_param::Panning), v, 0u}));
      r.live({});
    } else {
      r.live({});
    }
    for (unsigned i = 0; i < haudio::kBlockSamples; ++i) {
      h = fnv_mix(h, static_cast<std::uint16_t>(Rig::out[i]));
    }
  }
  r.live({mod_end(0, audio::kModGroupLfoPanning), mod_end(0, audio::kModGroupLfoVolume), mod_end(0, audio::kModGroupLfoFinetune), mod_end(0, audio::kModGroupLfoFilter),
          mod_end(0, audio::kModGroupLfoPosition)});
  r.p.perf_clear();
  return h;
}

}  // namespace

TEST_CASE("perform lfo: the engine output with the pan sent only when it changed is what it was (the instrument's and the Perform PAN LFO, all five rows, a Panning FX over the LFO and its end, a live pan edit under a clamped LFO)") {
  // captured on dev before the change (OS-PERF-CPU2 P1); 0: the instrument's LFO, 1: the PAN row, 2: all five rows, 3: a Panning FX over the LFO, then its end, 4: the same over the row, 5 / 6: a live pan edit
  // under an LFO held at 100 / 0
  // (3 and 4 equal 0 and 1: while the LFO holds the pan, it puts its value over the FX's at every block, before the graph renders; a cached "last sent" pan would let the FX show through)
  constexpr std::uint64_t kExpect[7] = {11680965860270691895ull, 2424177774905447162ull, 15266661993918184244ull, 11680965860270691895ull,
                                        2424177774905447162ull,  16334462511641850661ull, 11915491318002011819ull};
  std::uint64_t got[9];
  for (unsigned s = 0; s < 9u; ++s) {
    got[s] = pan_scene(s);
  }
  for (unsigned s = 0; s < 7u; ++s) {
    CAPTURE(s);
    CHECK(got[s] == kExpect[s]);
  }
  CHECK(got[7] == got[8]);  // a Panning FX under a held LFO whose value never changes is not heard, and not heard after its end either: the LFO's pan wins every block (rule 1: no cached "last sent" pan)
  CHECK(got[0] != got[1]);  // the scenes act: a hash of silence, or of a pan that does not move, would prove nothing
  CHECK(got[1] != got[2]);
  CHECK(got[5] != got[6]);
}

// ---- OS-PERF-LFO-STEP: the step FX Volume LFO 23, Panning LFO 24, Finetune LFO 33 and Position LFO 36 act -------------------------------------------------------------------
//
// New Community OS behaviour (docs/specs/OS-PERF-LFO-STEP.md), modelled on the step FX 35: the FX's value is an absolute rate index of the LFO table (VOL: 0..23 -> index 5..28, the amp table's last 24), and the LFO
// is the instrument's own at that index, as the Perform row's is (same shape and amount, the 50 % triangle fallback over a slot that stores none). The oracle is the instrument whose slot is that LFO.

namespace o3os::audio {
struct LfoStepTest {  // (friend of VoicePool: no code) the voice's modulator mask and the step state, which no public function says
  static int mmask(VoicePool& p, unsigned track) {
    const unsigned i = p.find(track);
    return i < kMaxVoices ? static_cast<int>(p.v_[i].mmask) : -1;
  }
  static unsigned step(const VoicePool& p, unsigned row, unsigned track) { return p.step_lfo_[row][track]; }
};
}  // namespace o3os::audio

namespace {

using audio::LfoStepTest;

struct StepRow {
  Dest d;
  unsigned group;
  std::uint8_t fx;
  unsigned max;    // the FX's range, 0..max
  unsigned rate0;  // the table index of value 0 (the tremolo's table is the last 24 of the 29)
  unsigned slot;   // the row's index in the voices' step state
};
constexpr StepRow kStepRows[] = {{kAmp, audio::kModGroupLfoVolume, audio::kFxLfoVolume, 23, 5, 0},
                                 {kPan, audio::kModGroupLfoPanning, audio::kFxLfoPanning, 28, 0, 1},
                                 {kTune, audio::kModGroupLfoFinetune, audio::kFxLfoFinetune, 28, 0, 2},
                                 {kWt, audio::kModGroupLfoPosition, audio::kFxLfoPosition, 28, 0, 3},
                                 {kGran, audio::kModGroupLfoPosition, audio::kFxLfoPosition, 28, 0, 3}};

SeqEvent step_ev(const StepRow& r, unsigned b) { return mod_set(r.group, 0, r.fx, static_cast<std::uint8_t>(b), audio::kLayerFx1, 0); }

void restart(VoicePool& p, const SampleDesc& desc) {
  p.perf_clear();
  p.set_sample(0, desc);
  p.set_tempo_x100(kTempo);
  p.restart_clock();
  p.transport_stopped();
}

// `blocks` blocks of a note on track 0 with the events told before the note (the step FX held when it starts), the transport stopped; the pool is left clean.
std::vector<double> run_step(Dest d, const SampleDesc& desc, std::initializer_list<SeqEvent> before, unsigned blocks) {
  VoicePool& p = pool(0);
  restart(p, desc);
  for (const SeqEvent& e : before) {
    p.fx_event(0, e);
  }
  std::vector<double> out;
  if (!p.note_on(0, 60, 0, audio::kVelocityDefault)) {
    return out;
  }
  p.mark_live();
  Probe pr{d, 0};
  for (unsigned b = 0; b < blocks; ++b) {
    out.push_back(step_block(p, pr));
  }
  p.perf_clear();
  return out;
}

}  // namespace

TEST_CASE("step lfo: each of the FX 23, 24, 33 and 36 is bit for bit the instrument's LFO at that table index (VOL: value + 5), with the slot's stored shape and amount") {
  for (const StepRow& r : kStepRows) {
    for (const unsigned b : {0u, 3u, 8u, r.max}) {
      const audio::FilterMod want = lfo_at(r.d, 2, r.rate0 + b, 0.6f);
      const std::vector<double> ref = run(r.d, desc_for(r.d, want), 0, kRun);
      const std::vector<double> got = run_step(r.d, desc_for(r.d, stored(2, 0.6f, false)), {step_ev(r, b)}, kRun);
      REQ(ref.size() == kRun && got.size() == kRun);
      CHECK_MESSAGE(got == ref, "fx " << static_cast<unsigned>(r.fx) << " value " << b);
      CHECK(spread(ref, 40) > 0.0);  // the LFO moves something: a comparison of two constants proves nothing
    }
  }
}

TEST_CASE("step lfo: the value is held to the FX's range (a byte from a file is untrusted), and a ModEnd, a layer-only ModSet or another FX's ModSet of the group ends it") {
  for (const StepRow& r : kStepRows) {
    const SampleDesc desc = desc_for(r.d, stored(2, 0.6f, false));
    const std::vector<double> top = run_step(r.d, desc, {step_ev(r, r.max)}, kRun);
    CHECK_MESSAGE(run_step(r.d, desc, {step_ev(r, 255)}, kRun) == top, "fx " << static_cast<unsigned>(r.fx) << " 255");
    CHECK_MESSAGE(run_step(r.d, desc, {step_ev(r, r.max + 1)}, kRun) == top, "fx " << static_cast<unsigned>(r.fx) << " max + 1");
    const std::vector<double> none = run_step(r.d, desc, {}, kRun);
    const SeqEvent ends[3] = {mod_end(0, r.group), mod_set(r.group, 0, 0, 0, audio::kLayerPerf, 0), mod_set(r.group, 0, audio::kFxFilterLfo, 5, audio::kLayerFx1, 0)};
    for (const SeqEvent& end : ends) {
      CHECK_MESSAGE(run_step(r.d, desc, {step_ev(r, 4), end}, kRun) == none, "fx " << static_cast<unsigned>(r.fx));
      VoicePool& p = pool(0);
      p.fx_event(0, step_ev(r, 4));
      CHECK(LfoStepTest::step(p, r.slot, 0) == 5u);  // rate + 1
      p.fx_event(0, end);
      CHECK(LfoStepTest::step(p, r.slot, 0) == 0u);
      p.perf_clear();
    }
  }
}

TEST_CASE("step lfo: the FX 36 on a plain sample (no wavetable, no granular) starts nothing: no modulator, no mask bit, the note is what it was; the same FX 23 starts the tremolo") {
  const SampleDesc plain = desc_for(kAmp, audio::FilterMod{});
  const std::vector<double> ref = run(kAmp, plain, 0, 60);
  VoicePool& p = pool(0);
  restart(p, plain);
  p.fx_event(0, step_ev(kStepRows[3], 9));  // held before the note ...
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  p.mark_live();
  CHECK(LfoStepTest::mmask(p, 0) == 0);
  Probe pr{kAmp, 0};
  std::vector<double> got;
  for (unsigned b = 0; b < 60; ++b) {
    if (b == 20u) {
      p.fx_event(0, step_ev(kStepRows[3], 3));  // ... and told again, and for another rate, while the note sounds
      CHECK(LfoStepTest::mmask(p, 0) == 0);
    }
    got.push_back(step_block(p, pr));
  }
  CHECK(LfoStepTest::mmask(p, 0) == 0);
  CHECK(got == ref);
  REQ(p.note_on(0, 62, 0, audio::kVelocityDefault));  // a new note under the held FX
  CHECK(LfoStepTest::mmask(p, 0) == 0);
  p.perf_clear();
  restart(p, plain);
  p.fx_event(0, step_ev(kStepRows[0], 9));
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  CHECK(LfoStepTest::mmask(p, 0) != 0);  // the probe reads the mask: it is not always empty
  p.perf_clear();
  // the wavetable and the granular instrument take it
  for (const Dest d : {kWt, kGran}) {
    restart(p, desc_for(d, audio::FilterMod{}));
    p.fx_event(0, step_ev(kStepRows[3], 9));
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    CHECK_MESSAGE(LfoStepTest::mmask(p, 0) > 0, "dest " << static_cast<unsigned>(d));
    p.perf_clear();
  }
}

TEST_CASE("step lfo: an FX that starts under a sounding note acts at once, a new note starts it, and FxReset ends it while Stopped keeps it") {
  for (const StepRow& r : kStepRows) {
    VoicePool& p = pool(0);
    restart(p, desc_for(r.d, audio::FilterMod{}));
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    p.mark_live();
    CHECK(LfoStepTest::mmask(p, 0) == 0);
    p.fx_event(0, step_ev(r, 6));
    CHECK_MESSAGE(LfoStepTest::mmask(p, 0) > 0, "fx " << static_cast<unsigned>(r.fx) << " starts on the sounding voice");
    Probe pr{r.d, 0};
    std::vector<double> v;
    for (unsigned b = 0; b < kRun; ++b) {
      v.push_back(step_block(p, pr));
    }
    CHECK(spread(v, 20) > 0.0);
    p.transport_stopped();  // Stopped leaves it
    CHECK(LfoStepTest::step(p, r.slot, 0) == 7u);
    CHECK(LfoStepTest::mmask(p, 0) > 0);
    REQ(p.note_on(0, 62, 0, audio::kVelocityDefault));  // a live pad note after the stop gets the LFO
    CHECK(LfoStepTest::mmask(p, 0) > 0);
    p.restart_clock();  // the transport starts: FxReset
    p.clear_filter_fxs();
    CHECK(LfoStepTest::step(p, r.slot, 0) == 0u);
    if (r.d == kAmp) {
      // The tremolo's end is a pass: the multiplier ramps back to unity in the render and the bit goes then (remod_k: "ramps back to unity by itself and then ends"), so the mask holds only the tremolo's bits (kMmTrem 4, kMmTremOff 8) until
      // a block has run; the others' bits go at once.
      CHECK_MESSAGE((LfoStepTest::mmask(p, 0) & ~(4 | 8)) == 0, "fx " << static_cast<unsigned>(r.fx) << " ended on the sounding voice by FxReset");
      for (unsigned b = 0; b < 8u && LfoStepTest::mmask(p, 0) != 0; ++b) {
        (void)step_block(p, pr);
      }
    }
    CHECK_MESSAGE(LfoStepTest::mmask(p, 0) == 0, "fx " << static_cast<unsigned>(r.fx) << " ended on the sounding voice by FxReset");
    REQ(p.note_on(0, 64, 0, audio::kVelocityDefault));
    CHECK(LfoStepTest::mmask(p, 0) == 0);
    p.perf_clear();
  }
  // a track keeps its own: FxReset ends every track's
  VoicePool& p = pool(0);
  p.perf_clear();
  p.fx_event(2, step_ev(kStepRows[1], 3));
  p.fx_event(5, step_ev(kStepRows[2], 4));
  CHECK(LfoStepTest::step(p, 1, 2) == 4u);
  CHECK(LfoStepTest::step(p, 1, 5) == 0u);
  CHECK(LfoStepTest::step(p, 2, 5) == 5u);
  p.clear_filter_fxs();
  CHECK(LfoStepTest::step(p, 1, 2) == 0u);
  CHECK(LfoStepTest::step(p, 2, 5) == 0u);
}

TEST_CASE("step lfo: the Perform layer wins while it holds a value, the step FX comes back when it ends or holds 0, and a live SPEED edit of the instrument's LFO does not move it") {
  for (const StepRow& r : {kStepRows[0], kStepRows[1], kStepRows[2]}) {
    const SampleDesc desc = desc_for(r.d, stored(2, 0.6f, false));
    const unsigned fx_rate = 8;
    const SeqEvent under_layer = mod_set(r.group, 0, r.fx, fx_rate, audio::kLayerPerf, 3);  // the layer's +3 over the step FX: a and b still say the FX
    const SeqEvent layer_zero = mod_set(r.group, 0, r.fx, fx_rate, audio::kLayerPerf, 0);
    const std::vector<double> layer_only = run(r.d, desc, 3, kRun);
    const std::vector<double> step_only = run_step(r.d, desc, {step_ev(r, fx_rate)}, kRun);
    const std::vector<double> both = run_step(r.d, desc, {step_ev(r, fx_rate), under_layer}, kRun);
    const std::vector<double> zero = run_step(r.d, desc, {layer_zero}, kRun);
    REQ(layer_only.size() == kRun && step_only.size() == kRun && both.size() == kRun && zero.size() == kRun);
    CHECK_MESSAGE(both == layer_only, "fx " << static_cast<unsigned>(r.fx) << ": the layer's speed wins");
    CHECK(both != step_only);
    CHECK_MESSAGE(zero == step_only, "fx " << static_cast<unsigned>(r.fx) << ": the layer at 0 lets the step FX through (a == FX)");
    // the layer ends under a sounding note: the step FX's LFO is back (its spread, within the memory of the phase)
    VoicePool& p = pool(0);
    restart(p, desc);
    p.fx_event(0, step_ev(r, fx_rate));
    p.fx_event(0, under_layer);
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    p.mark_live();
    Probe pr{r.d, 0};
    for (unsigned b = 0; b < kRun; ++b) {
      (void)step_block(p, pr);
    }
    p.fx_event(0, step_ev(r, fx_rate));  // the control table's winner is the step FX again
    std::vector<double> tail;
    for (unsigned b = 0; b < kRun; ++b) {
      tail.push_back(step_block(p, pr));
    }
    CHECK_MESSAGE(std::fabs(spread(tail, 40) - spread(step_only, 40)) < 0.25 * spread(step_only, 40), "fx " << static_cast<unsigned>(r.fx) << ": the step FX's LFO is back");
    p.perf_clear();
  }
  // a live SPEED edit of the instrument's own LFO: the sounding voice runs the step FX's rate before and after, bit for bit
  static constexpr unsigned kSlotOf[kDests] = {0, 1, 5, 2, 3, 4};
  for (const StepRow& r : {kStepRows[0], kStepRows[1], kStepRows[2]}) {
    VoicePool& a = pool(0);
    VoicePool& c = pool(2);
    for (VoicePool* q : {&a, &c}) {
      restart(*q, desc_for(r.d, lfo_at(r.d, 2, r.rate0 + 10u, 0.6f)));
      q->fx_event(0, step_ev(r, 4));
      REQ(q->note_on(0, 60, 0, audio::kVelocityDefault));
      q->mark_live();
    }
    Probe pa{r.d, 0}, pc{r.d, 0};
    unsigned equal = 0;
    for (unsigned b = 0; b < 2u * kPeriodBlocks; ++b) {
      if (b == 70u) {
        std::uint32_t pan = 0;
        CHECK(a.apply_param(0, ip::lfo(kSlotOf[r.d], ip::LfoSpeed), 3.0f, pan));
      }
      const double xa = step_block(a, pa);
      const double xc = step_block(c, pc);
      equal += xa == xc ? 1u : 0u;
    }
    CHECK_MESSAGE(equal == 2u * kPeriodBlocks, "fx " << static_cast<unsigned>(r.fx) << ": " << equal << " of " << 2u * kPeriodBlocks << " blocks equal");
    a.perf_clear();
    c.perf_clear();
  }
}

TEST_CASE("step lfo: the export puts the step FX away and gives them back, and the exported audio has none of them") {
  VoicePool& p = pool(0);
  p.perf_clear();
  p.fx_event(3, step_ev(kStepRows[0], 4));
  p.fx_event(3, step_ev(kStepRows[3], 9));
  p.fx_event(7, step_ev(kStepRows[2], 20));
  VoicePool::PerfSave save{};
  p.perf_save(save);
  p.perf_clear();
  for (unsigned t = 0; t < audio::kVoiceTracks; ++t) {
    for (unsigned row = 0; row < 4u; ++row) {
      CHECK(LfoStepTest::step(p, row, t) == 0u);
    }
  }
  p.perf_restore(save);
  CHECK(LfoStepTest::step(p, 0, 3) == 5u);
  CHECK(LfoStepTest::step(p, 3, 3) == 10u);
  CHECK(LfoStepTest::step(p, 2, 7) == 21u);
  CHECK(LfoStepTest::step(p, 1, 3) == 0u);
  p.perf_clear();
  // the engine's export with all four held (and a note sounding when it is asked) is the export with none
  const auto run_export = [&](bool held) {
    std::vector<std::int16_t> exported;
    Rig r;
    r.p.perf_clear();
    r.p.cut(0);
    r.live({});
    if (held) {
      r.live({step_ev(kStepRows[0], 4), step_ev(kStepRows[1], 9), step_ev(kStepRows[2], 6), step_ev(kStepRows[3], 3)});
    }
    r.live({note_on_ev()});
    for (unsigned b = 0; b < 8; ++b) {
      r.live({});
    }
    audio::EventSource src;
    src.step = &export_step;
    src.buffer = r.events;
    src.capacity = audio::kMaxEventsPerBlock;
    r.engine.offline_request();
    CHECK(r.engine.offline_ready(false));
    r.engine.offline_enter(src);
    CHECK(r.engine.offline_entered());
    for (std::uint32_t b = 0; b < 40; ++b) {
      haudio::BlockReport report{};
      const haudio::BlockIo io{Rig::in, Rig::out, 0, &report};
      r.engine.render_offline(io);
      exported.insert(exported.end(), Rig::out, Rig::out + haudio::kBlockSamples);
    }
    r.engine.offline_leave();
    r.p.perf_clear();
    return exported;
  };
  const std::vector<std::int16_t> plain = run_export(false);
  const std::vector<std::int16_t> held = run_export(true);
  NEED(plain.size() == held.size() && plain.size() > haudio::kBlockSamples);
  CHECK(std::equal(plain.begin() + haudio::kBlockSamples, plain.end(), held.begin() + haudio::kBlockSamples));  // (block 0 carries the declick of the live note)
}

TEST_CASE("step lfo: the FX 24 over a held Panning FX 31 sets the pan every block (last writer wins), and its end brings the instrument's pan back until the FX 31's next ModSet") {
  const auto balance = [] {
    double l = 0, r = 0;
    for (unsigned i = 0; i + 1u < haudio::kBlockSamples; i += 2u) {
      l += std::abs(static_cast<int>(Rig::out[i]));
      r += std::abs(static_cast<int>(Rig::out[i + 1u]));
    }
    return l + r > 0.0 ? (l - r) / (l + r) : 0.0;
  };
  Rig r;
  r.p.perf_clear();
  r.p.cut(0);
  r.p.set_sample(0, desc_for(kPan, stored(2, 0.6f, false)));  // a constant sample, no pan modulator, the pan at the instrument's own
  r.live({});
  r.live({note_on_ev()});
  r.live({pan_fx(10)});
  for (unsigned b = 0; b < 20; ++b) {
    r.live({});
  }
  const double held = balance();
  CHECK(std::fabs(held) > 0.2);  // the FX 31 pans the voice
  r.live({step_ev(kStepRows[1], 8)});
  double lo = 1.0, hi = -1.0;
  for (unsigned b = 0; b < 300; ++b) {
    r.live({});
    lo = std::min(lo, balance());
    hi = std::max(hi, balance());
  }
  CHECK(hi - lo > 0.2);  // the LFO moves the pan around the instrument's (the FX 31's value is over-written every block)
  r.live({mod_end(0, audio::kModGroupLfoPanning)});
  for (unsigned b = 0; b < 40; ++b) {
    r.live({});
  }
  CHECK(std::fabs(balance()) < 0.25 * std::fabs(held));  // the instrument's pan: the FX 31 is not re-applied by the LFO's end
  r.live({pan_fx(10)});
  for (unsigned b = 0; b < 20; ++b) {
    r.live({});
  }
  CHECK(std::fabs(balance() - held) < 0.05);  // its next ModSet acts again
  r.live({mod_end(0, audio::kModGroupPanning)});
  r.p.perf_clear();
}
