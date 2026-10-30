// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011b: the performance layer reaching the voices. The Filter group's performance value (events.h perf_filter_value: the slot's type and a cutoff offset), the voices' PERFORMANCE
// FILTER (voices.h) against the same voice with the equivalent instrument filter, the layer's priority over the Cutoff FX and its release back to it, that the transport's FxReset
// leaves the layer alone, and the whole path through the Engine from a ModSet event. The rules are described in voices.h (PERFORMANCE FILTER). OS-PERFFX P0 adds the layer's own state in the
// voices and the strips (it survives FxReset and Stopped) and the export putting it away and giving it back. Runs on the host and, as qemu_audio_perform, on the target.
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/event_source.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

using namespace o3os;
using audio::EventKind;
using audio::SampleDesc;
using audio::SeqEvent;
using audio::VoicePool;
namespace haudio = o3os::hal::audio;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kLen = 6000;
std::int16_t g_wave[kLen];

void init_wave() {
  for (std::uint32_t i = 0; i < kLen; ++i) {
    g_wave[i] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(2.0 * kPi * 37.3 * static_cast<double>(i) / kLen) + 6000.0 * std::sin(0.9 * static_cast<double>(i))));
  }
}

SampleDesc desc_of(bool filter, std::uint8_t type, float cutoff, float res) {
  SampleDesc d{g_wave, kLen, 0, 0, 65536};
  d.filter.enable = filter;
  d.filter.type = type;
  d.filter.cutoff = cutoff;
  d.filter.resonance = res;
  return d;
}

VoicePool& pool(unsigned which) {
  static o3os::test::StaticSlot<VoicePool> a, b;
  return which == 0u ? a.make() : b.make();
}

constexpr std::uint32_t kBlocks = 40;
std::int16_t g_ref[kBlocks * kBlk], g_got[kBlocks * kBlk];

void render_n(VoicePool& p, std::int16_t* out, std::uint32_t blocks) {
  for (std::uint32_t b = 0; b < blocks; ++b) {
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
    const std::int16_t* f = p.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      out[b * kBlk + i] = f != nullptr ? f[i] : std::int16_t{0};
    }
  }
}

// The largest difference between two renders, in LSB.
std::int32_t worst(const std::int16_t* a, const std::int16_t* b, std::uint32_t n) {
  std::int32_t w = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t e = std::abs(static_cast<std::int32_t>(a[i]) - static_cast<std::int32_t>(b[i]));
    w = e > w ? e : w;
  }
  return w;
}

// The reference: a voice whose INSTRUMENT filters with `type` and `cutoff` (resonance `res`), rendered for kBlocks blocks.
void render_reference(std::uint8_t type, float cutoff, float res) {
  VoicePool& a = pool(0);
  a.set_sample(0, desc_of(true, type, cutoff, res));
  a.note_on(0, 60, 0, 127);
  render_n(a, g_ref, kBlocks);
}

}  // namespace

TEST_CASE("perform filter: the value carries the slot's type and a signed offset, and is never zero") {
  for (unsigned type = 0; type < 3; ++type) {
    for (int off = -100; off <= 100; ++off) {
      const std::int16_t v = audio::perf_filter_value(type, off);
      CHECK(v != 0);
      CHECK(audio::perf_filter_type(v) == type);
      CHECK(audio::perf_filter_offset(v) == off);
    }
  }
  CHECK(audio::perf_filter_offset(audio::perf_filter_value(0, 500)) == 100);   // held to the range
  CHECK(audio::perf_filter_offset(audio::perf_filter_value(0, -500)) == -100);
  CHECK(audio::perf_filter_type(audio::perf_filter_value(9, 0)) == 2);         // a type past the three is the last
}

TEST_CASE("perform filter: the layer filters a note whose instrument has no filter exactly as an instrument filter of that cutoff would (resonance 0)") {
  init_wave();
  for (std::uint8_t type = 0; type < 3; ++type) {
    render_reference(type, 0.37f, 0.0f);  // the instrument filters: type, cutoff 0.37
    VoicePool& b = pool(1);
    b.set_sample(0, desc_of(false, 0, 0.0f, 0.0f));
    b.set_filter_perf(0, type, 37);  // base 0 (the instrument's own cutoff: 0.0) plus 37 / 100
    b.note_on(0, 60, 0, 127);
    render_n(b, g_got, kBlocks);
    CHECK_MESSAGE(worst(g_ref, g_got, kBlocks * kBlk) <= 2, "type " << int(type));
    b.clear_filter_perf(0);
  }
}

TEST_CASE("perform filter: the offset is added to the instrument's cutoff, held to 0..1, and the type is the slot's, not the instrument's") {
  init_wave();
  render_reference(audio::kFilterHigh, 0.60f, 1.5f);  // the reference: high-pass at 0.60 with the instrument's resonance
  VoicePool& b = pool(1);
  b.set_sample(0, desc_of(true, audio::kFilterLow, 0.40f, 1.5f));  // the instrument: a low-pass at 0.40
  b.set_filter_perf(0, audio::kFilterHigh, 20);                    // the layer: high-pass, + 20
  b.note_on(0, 60, 0, 127);
  render_n(b, g_got, kBlocks);
  CHECK(worst(g_ref, g_got, kBlocks * kBlk) <= 2);
  CHECK(std::fabs(b.filter_cutoff(0) - 0.60f) < 1.0e-5f);
  // the clamps: the cutoff is held to 0..100
  b.set_sample(0, desc_of(true, audio::kFilterLow, 0.90f, 0.0f));
  b.set_filter_perf(0, audio::kFilterLow, 100);
  b.note_on(0, 60, 0, 127);
  render_n(b, g_got, 1);
  CHECK(b.filter_cutoff(0) == 1.0f);
  b.set_sample(0, desc_of(true, audio::kFilterLow, 0.10f, 0.0f));
  b.set_filter_perf(0, audio::kFilterLow, -100);
  b.note_on(0, 60, 0, 127);
  render_n(b, g_got, 1);
  CHECK(b.filter_cutoff(0) == 0.0f);
  b.clear_filter_perf(0);
}

TEST_CASE("perform filter: the layer wins over the Cutoff FX and works on its value, and clearing it returns the Cutoff FX, then the instrument's own filter") {
  init_wave();
  VoicePool& b = pool(1);
  b.set_sample(0, desc_of(true, audio::kFilterLow, 0.80f, 0.0f));
  b.set_filter_fx(0, audio::kFilterHigh, 30);   // the track's Cutoff FX: high-pass 0.30
  b.set_filter_perf(0, audio::kFilterBand, 25); // the layer: band-pass, + 25 over the FX's value
  b.note_on(0, 60, 0, 127);
  render_n(b, g_got, kBlocks);
  CHECK(std::fabs(b.filter_cutoff(0) - 0.55f) < 1.0e-5f);
  render_reference(audio::kFilterBand, 0.55f, 0.0f);
  CHECK(worst(g_ref, g_got, kBlocks * kBlk) <= 2);
  // the layer lets go: the Cutoff FX (high-pass 0.30), then the instrument's low-pass 0.80
  b.clear_filter_perf(0);
  render_n(b, g_got, 1);
  CHECK(std::fabs(b.filter_cutoff(0) - 0.30f) < 1.0e-5f);
  b.clear_filter_fx(0);
  render_n(b, g_got, 1);
  CHECK(std::fabs(b.filter_cutoff(0) - 0.80f) < 1.0e-5f);
  // an instrument with no filter ends in the bypass: bit for bit the voice without any
  VoicePool& a = pool(0);
  a.set_sample(0, desc_of(false, 0, 0.0f, 0.0f));
  b.set_sample(0, desc_of(false, 0, 0.0f, 0.0f));
  a.cut(0);  // two fresh voices: nothing of the earlier notes (a retrigger keeps a click tail and a filter's state)
  b.cut(0);
  render_n(a, g_ref, 3);
  render_n(b, g_got, 3);
  a.note_on(0, 60, 0, 127);
  b.note_on(0, 60, 0, 127);
  render_n(a, g_ref, 30);
  render_n(b, g_got, 10);
  b.set_filter_perf(0, audio::kFilterLow, 10);
  render_n(b, g_got + 10 * kBlk, 10);
  b.clear_filter_perf(0);
  render_n(b, g_got + 20 * kBlk, 10);
  CHECK(std::memcmp(g_ref, g_got, 10 * kBlk * sizeof(std::int16_t)) == 0);
  CHECK(std::memcmp(g_ref + 10 * kBlk, g_got + 10 * kBlk, 10 * kBlk * sizeof(std::int16_t)) != 0);  // under the layer
  CHECK(std::memcmp(g_ref + 20 * kBlk, g_got + 20 * kBlk, 10 * kBlk * sizeof(std::int16_t)) == 0);  // after it
}

TEST_CASE("perform filter: the layer is its own: the Cutoff FX's end and the transport start (FxReset) leave it, and a track outside the pool or a bad type is ignored") {
  init_wave();
  VoicePool& b = pool(1);
  b.set_sample(0, desc_of(false, 0, 0.0f, 0.0f));
  b.set_filter_fx(0, audio::kFilterLow, 20);
  b.set_filter_perf(0, audio::kFilterLow, 10);
  b.clear_filter_fx(0);
  b.note_on(0, 60, 0, 127);
  render_n(b, g_got, 1);
  CHECK(std::fabs(b.filter_cutoff(0) - 0.10f) < 1.0e-5f);  // the layer over the instrument's 0.0
  b.clear_filter_fxs();                                    // FxReset
  render_n(b, g_got, 1);
  CHECK(std::fabs(b.filter_cutoff(0) - 0.10f) < 1.0e-5f);
  b.set_filter_perf(0, 3, 50);  // not a type
  b.set_filter_perf(99, 0, 50);
  render_n(b, g_got, 1);
  CHECK(std::fabs(b.filter_cutoff(0) - 0.10f) < 1.0e-5f);
  b.clear_filter_perf(99);
  b.clear_filter_perf(0);
  render_n(b, g_got, 1);
  CHECK(b.filter_cutoff(0) == -1.0f);  // out of the path
}

namespace {

// A script of events for the block that is rendered next.
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

SeqEvent mod_set(unsigned group_filter, std::uint8_t track, std::uint8_t a, std::uint8_t b, std::uint8_t layer, std::int32_t value) {
  SeqEvent e{};
  e.kind = EventKind::ModSet;
  e.track = track;
  e.a = a;
  e.b = b;
  e.c = layer;
  e.flags = static_cast<std::uint8_t>(group_filter);
  e.value = value;
  return e;
}

}  // namespace

TEST_CASE("perform filter: through the Engine, a performance ModSet of the Filter group holds the filter over the sequencer's, its ModEnd releases it, FxReset does not") {
  init_wave();
  static o3os::test::StaticSlot<audio::Engine> slot;
  static audio::TapMem taps;
  static SeqEvent events[audio::kMaxEventsPerBlock];
  audio::Engine& e = slot.make(&fake_cycles, taps);
  VoicePool& p = pool(1);
  p.set_sample(0, desc_of(true, audio::kFilterLow, 0.50f, 0.0f));
  e.attach_voices(&p);
  audio::EventSource src;
  src.step = &scripted;
  src.buffer = events;
  src.capacity = audio::kMaxEventsPerBlock;
  e.attach_events(src);
  alignas(8) static std::int16_t in[haudio::kBlockSamples];
  alignas(8) static std::int16_t out[haudio::kBlockSamples];
  const auto block = [&](std::initializer_list<SeqEvent> events_in) {
    g_script_n = 0;
    for (const SeqEvent& s : events_in) {
      g_script[g_script_n++] = s;
    }
    haudio::BlockReport report{};
    const haudio::BlockIo io{in, out, 0, &report};
    e.render_block(io);
  };
  SeqEvent on{};
  on.kind = EventKind::NoteOn;
  on.a = 60;
  on.b = 0;
  on.c = 127;
  block({on});
  CHECK(std::fabs(p.filter_cutoff(0) - 0.50f) < 1.0e-5f);
  // a performance-only winner: low-pass, +20 over the instrument
  block({mod_set(audio::kModGroupFilter, 0, 0, 0, audio::kLayerPerf, audio::perf_filter_value(audio::kFilterLow, 20))});
  CHECK(std::fabs(p.filter_cutoff(0) - 0.70f) < 1.0e-5f);
  // the sequencer's Cutoff FX arrives underneath (FX 28 high-pass, value 40): the layer is still the winner and now works on 0.40
  block({mod_set(audio::kModGroupFilter, 0, 28, 40, audio::kLayerPerf, audio::perf_filter_value(audio::kFilterBand, -10))});
  CHECK(std::fabs(p.filter_cutoff(0) - 0.30f) < 1.0e-5f);
  // the layer lets go with the sequencer's FX still there (a winner of layer FX1): the FX's value
  block({mod_set(audio::kModGroupFilter, 0, 28, 40, audio::kLayerFx1, 0)});
  CHECK(std::fabs(p.filter_cutoff(0) - 0.40f) < 1.0e-5f);
  // the layer again, then the transport starts: the sequencer's FX end, the layer's does not
  SeqEvent reset{};
  reset.kind = EventKind::FxReset;
  reset.track = audio::kNoTrack;
  block({mod_set(audio::kModGroupFilter, 0, 28, 40, audio::kLayerPerf, audio::perf_filter_value(audio::kFilterLow, 5)), reset});
  CHECK(std::fabs(p.filter_cutoff(0) - 0.55f) < 1.0e-5f);  // 0.50 + 0.05 over the instrument: the FX underneath is gone
  // the end of the group: the instrument's own filter
  SeqEvent end{};
  end.kind = EventKind::ModEnd;
  end.track = 0;
  end.flags = audio::kModGroupFilter;
  block({end});
  CHECK(std::fabs(p.filter_cutoff(0) - 0.50f) < 1.0e-5f);
}

// ---- OS-PERFFX P0: an export is deaf to Perform, and gives the layer back --------------------------------------------------------------------------------------------------------------
// The export builds a sequencer of its own over the same voices (seq/offline.cpp), and Perform stays on in Master and Song, so the layer's hold in the voices must not reach the export's notes
// and the export's own events must not wipe it: Engine::offline_enter puts the layer away and offline_leave brings it back.

namespace {

constexpr std::uint32_t kNever = 0xFFFFFFFFu;

// The export's source: a note on track 0 at block `note_block` and, when asked, a Cutoff FX of its own (FX 28 value 40, the first column) from `fx_on` to `fx_off`.
struct ExportSrc {
  std::uint32_t note_block = 1;
  std::uint32_t fx_on = kNever;
  std::uint32_t fx_off = kNever;
};

std::uint32_t export_step(void* self, std::uint64_t start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
  const ExportSrc& s = *static_cast<const ExportSrc*>(self);
  const std::uint32_t block = static_cast<std::uint32_t>(start / n);
  std::uint32_t k = 0;
  if (block == s.fx_on && k < cap) {
    out[k++] = mod_set(audio::kModGroupFilter, 0, 28, 40, audio::kLayerFx1, 0);
  }
  if (block == s.fx_off && k < cap) {
    SeqEvent end{};
    end.kind = EventKind::ModEnd;
    end.track = 0;
    end.flags = audio::kModGroupFilter;
    out[k++] = end;
  }
  if (block == s.note_block && k < cap) {
    SeqEvent on{};
    on.kind = EventKind::NoteOn;
    on.a = 60;
    on.b = 0;
    on.c = 127;
    out[k++] = on;
  }
  return k;
}

// The engine, the pool and a live block that can speak: the layer is put in through the live source as the sequencer would, then the export takes the engine.
struct ExportRig {
  ExportRig() : engine(slot.make(&fake_cycles, taps)), p(pool(1)) {
    init_wave();
    p.set_sample(0, desc_of(false, 0, 0.0f, 0.0f));
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
  // `blocks` blocks of the export (from the source `enter` gave), appended to `into`.
  void export_blocks(std::uint32_t blocks, std::vector<std::int16_t>& into) {
    for (std::uint32_t b = 0; b < blocks; ++b) {
      haudio::BlockReport report{};
      const haudio::BlockIo io{in, out, 0, &report};
      engine.render_offline(io);
      into.insert(into.end(), out, out + haudio::kBlockSamples);
    }
  }
  void enter(ExportSrc& s) {
    audio::EventSource src;
    src.self = &s;
    src.step = &export_step;
    src.buffer = events;
    src.capacity = audio::kMaxEventsPerBlock;
    engine.offline_request();
    CHECK(engine.offline_ready(false));
    engine.offline_enter(src);
    CHECK(engine.offline_entered());
  }
  // A live note on track 0 afterwards: the cutoff its filter has (-1: none), as the performance layer would give it.
  float live_note_cutoff() {
    SeqEvent on{};
    on.kind = EventKind::NoteOn;
    on.a = 60;
    on.b = 0;
    on.c = 127;
    live({on});
    return p.filter_cutoff(0);
  }

  // (static: the target suites run on a small stack, and the engine and its tap memory are big)
  static inline o3os::test::StaticSlot<audio::Engine> slot;
  static inline audio::TapMem taps;
  static inline SeqEvent events[audio::kMaxEventsPerBlock];
  alignas(8) static inline std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) static inline std::int16_t out[haudio::kBlockSamples] = {};
  audio::Engine& engine;
  VoicePool& p;
};

std::vector<std::int16_t> export_with_layer(bool layer_held, ExportSrc& src, float* during_cutoff, float* after_cutoff) {
  ExportRig r;
  if (layer_held) {
    r.live({mod_set(audio::kModGroupFilter, 0, 0, 0, audio::kLayerPerf, audio::perf_filter_value(audio::kFilterLow, 40))});
  }
  r.enter(src);
  std::vector<std::int16_t> off;
  r.export_blocks(3, off);  // the note starts in block 1
  if (during_cutoff != nullptr) {
    *during_cutoff = r.p.filter_cutoff(0);
  }
  r.export_blocks(9, off);
  r.engine.offline_leave();
  if (after_cutoff != nullptr) {
    *after_cutoff = r.live_note_cutoff();
  }
  return off;
}

}  // namespace

TEST_CASE("perform layer: an export is byte for byte what it is with Perform off, and the voices have the layer back when it ends") {
  ExportSrc plain;
  float during_off = 0.0f;
  float after_off = 0.0f;
  const std::vector<std::int16_t> off = export_with_layer(false, plain, &during_off, &after_off);
  float during_held = 0.0f;
  float after_held = 0.0f;
  const std::vector<std::int16_t> held = export_with_layer(true, plain, &during_held, &after_held);
  CHECK(off.size() == held.size());
  CHECK(off == held);
  std::int32_t peak = 0;
  for (const std::int16_t s : off) {
    peak = std::max<std::int32_t>(peak, s < 0 ? -s : s);
  }
  CHECK(peak > 1000);  // the note sounded: a comparison of silence would prove nothing
  CHECK(during_off == -1.0f);
  CHECK(during_held == -1.0f);  // the export's note has no filter in the path
  CHECK(after_off == -1.0f);
  CHECK(std::fabs(after_held - 0.40f) < 1.0e-5f);  // and the live note after it does: the layer is held again
}

TEST_CASE("perform layer: the export's own Cutoff FX and its end leave the layer as it was") {
  ExportSrc own;
  own.fx_on = 1;
  own.fx_off = 6;
  float after_held = 0.0f;
  const std::vector<std::int16_t> held = export_with_layer(true, own, nullptr, &after_held);
  float after_off = 0.0f;
  const std::vector<std::int16_t> off = export_with_layer(false, own, nullptr, &after_off);
  CHECK(held == off);
  CHECK(after_off == -1.0f);
  CHECK(std::fabs(after_held - 0.40f) < 1.0e-5f);
}

// ---- OS-PERFFX P0: the layer's values in the voices and the strips are its own ----------------------------------------------------------------------------------------------------------

namespace {

SeqEvent mod_end(std::uint8_t track, unsigned group) {
  SeqEvent e{};
  e.kind = EventKind::ModEnd;
  e.track = track;
  e.flags = static_cast<std::uint8_t>(group);
  return e;
}

SeqEvent plain_event(EventKind kind) {
  SeqEvent e{};
  e.kind = kind;
  e.track = audio::kNoTrack;
  return e;
}

}  // namespace

TEST_CASE("perform layer: the voices hold the layer's value of Overdrive and Bit depth per track, across FxReset and Stopped; a step FX, a ModEnd or the layer's end lets it go") {
  using audio::kLayerFx1;
  using audio::kLayerPerf;
  using audio::kModGroupBitDepth;
  using audio::kModGroupOverdrive;
  ExportRig r;
  const auto held = [&](unsigned t, unsigned g, int v) { return r.p.perf_held(t, g) && r.p.perf_value(t, g) == v; };
  r.live({mod_set(kModGroupOverdrive, 3, 0, 0, kLayerPerf, 40), mod_set(kModGroupBitDepth, 5, 0, 0, kLayerPerf, 7)});
  CHECK(held(3, kModGroupOverdrive, 40));
  CHECK(held(5, kModGroupBitDepth, 7));
  CHECK_FALSE(r.p.perf_held(3, kModGroupBitDepth));  // one slot a group and a track
  CHECK_FALSE(r.p.perf_held(5, kModGroupOverdrive));
  CHECK_FALSE(r.p.perf_held(4, kModGroupOverdrive));
  CHECK_FALSE(r.p.perf_held(3, audio::kModGroupSlide));  // a group the voices keep no value for
  // the transport starts and stops: the step FX end, the layer does not (as the performance filter)
  r.live({plain_event(EventKind::FxReset)});
  CHECK(held(3, kModGroupOverdrive, 40));
  CHECK(held(5, kModGroupBitDepth, 7));
  r.live({plain_event(EventKind::Stopped)});
  CHECK(held(3, kModGroupOverdrive, 40));
  CHECK(held(5, kModGroupBitDepth, 7));
  r.live({plain_event(EventKind::FxReset), plain_event(EventKind::Stopped), plain_event(EventKind::FxReset)});
  CHECK(held(3, kModGroupOverdrive, 40));
  CHECK(held(5, kModGroupBitDepth, 7));
  // the other groups' events on the same track leave it
  r.live({mod_set(audio::kModGroupSlide, 3, audio::kFxSlideUp, 40, kLayerFx1, 0), mod_set(audio::kModGroupFilter, 3, 28, 40, kLayerFx1, 0), mod_set(kModGroupBitDepth, 3, audio::kFxBitDepth, 9, kLayerFx1, 0)});
  CHECK(held(3, kModGroupOverdrive, 40));
  // the layer over a step FX (the FX is in a and b underneath): the layer's value is what is kept, and the step layer is told what is underneath
  r.live({mod_set(kModGroupOverdrive, 3, audio::kFxOverdrive, 30, kLayerPerf, 55)});
  CHECK(held(3, kModGroupOverdrive, 55));
  // the layer ends while the step FX stays: the control table sends the step FX as the winner (a layer of the columns), and the slot lets go
  r.live({mod_set(kModGroupOverdrive, 3, audio::kFxOverdrive, 30, kLayerFx1, 0)});
  CHECK_FALSE(r.p.perf_held(3, kModGroupOverdrive));
  CHECK(r.p.perf_value(3, kModGroupOverdrive) == 0);
  // a negative value, and the byte it is held to
  r.live({mod_set(kModGroupOverdrive, 3, 0, 0, kLayerPerf, -20)});
  CHECK(held(3, kModGroupOverdrive, -20));
  r.live({mod_set(kModGroupOverdrive, 3, 0, 0, kLayerPerf, 1000), mod_set(kModGroupBitDepth, 3, 0, 0, kLayerPerf, -1000)});
  CHECK(held(3, kModGroupOverdrive, 127));
  CHECK(held(3, kModGroupBitDepth, -127));
  // no source is left
  r.live({mod_end(3, kModGroupOverdrive), mod_end(3, kModGroupBitDepth), mod_end(5, kModGroupBitDepth)});
  CHECK_FALSE(r.p.perf_held(3, kModGroupOverdrive));
  CHECK_FALSE(r.p.perf_held(3, kModGroupBitDepth));
  CHECK_FALSE(r.p.perf_held(5, kModGroupBitDepth));
  // a track outside the pool is ignored
  r.live({mod_set(kModGroupOverdrive, 20, 0, 0, kLayerPerf, 40), mod_set(kModGroupOverdrive, 15, 0, 0, kLayerPerf, 41)});
  CHECK_FALSE(r.p.perf_held(20, kModGroupOverdrive));
  CHECK(held(15, kModGroupOverdrive, 41));
}

TEST_CASE("perform layer: OVERDRV and BITDEPTH act on the note, and a note started after FxReset and Stopped is shaped by the layer (OS-PERFFX P1); the layer alone no longer ends a step FX") {
  using audio::kLayerFx1;
  using audio::kLayerPerf;
  init_wave();
  VoicePool& a = pool(0);
  VoicePool& b = pool(1);
  SampleDesc d = desc_of(false, 0, 0.0f, 0.0f);
  a.set_sample(0, d);
  b.set_sample(0, d);
  a.perf_clear();
  b.perf_clear();
  a.clear_slide_fxs();
  b.clear_slide_fxs();
  a.fx_event(0, audio::kModGroupOverdrive, audio::kFxOverdrive, 60);  // the step FX alone
  b.fx_event(0, audio::kModGroupOverdrive, audio::kFxOverdrive, 60);
  a.note_on(0, 60, 0, 127);
  b.note_on(0, 60, 0, 127);
  render_n(a, g_ref, 12);
  render_n(b, g_got, 12);
  CHECK(std::memcmp(g_ref, g_got, 12 * kBlk * sizeof(std::int16_t)) == 0);
  // ... and the layer over it (a note started after it): another sound, the layer's 90
  b.fx_event(0, mod_set(audio::kModGroupOverdrive, 0, audio::kFxOverdrive, 60, kLayerPerf, 90));
  CHECK(b.perf_held(0, audio::kModGroupOverdrive));
  b.cut(0);
  render_n(b, g_got, 8);
  b.note_on(0, 60, 0, 127);
  render_n(b, g_got, 12);
  CHECK(std::memcmp(g_ref, g_got, 12 * kBlk * sizeof(std::int16_t)) != 0);
  // the transport starts and stops: the step FX end, the layer stays, and a note after them is the same sound
  static std::int16_t held[12 * kBlk];
  std::memcpy(held, g_got, sizeof held);
  b.clear_slide_fxs();  // (FxReset)
  b.cut(0);
  render_n(b, g_got, 8);
  b.note_on(0, 60, 0, 127);
  render_n(b, g_got, 12);
  CHECK(b.perf_held(0, audio::kModGroupOverdrive));
  CHECK(std::memcmp(held, g_got, sizeof held) == 0);  // the step FX under the layer went; the layer's value won before and is the whole sound now: the same note
  // the layer alone has no step FX under it: its value is the whole sound, the same as the layer over a step FX of any value
  static std::int16_t alone[12 * kBlk];
  b.fx_event(0, mod_set(audio::kModGroupOverdrive, 0, 0, 0, kLayerPerf, 90));
  b.cut(0);
  render_n(b, g_got, 8);
  b.note_on(0, 60, 0, 127);
  render_n(b, alone, 12);
  b.fx_event(0, mod_set(audio::kModGroupOverdrive, 0, audio::kFxOverdrive, 10, kLayerPerf, 90));
  b.cut(0);
  render_n(b, g_got, 8);
  b.note_on(0, 60, 0, 127);
  render_n(b, g_got, 12);
  CHECK(std::memcmp(alone, g_got, sizeof alone) == 0);
  // the layer ends with a ModEnd: the instrument's own (clean) sound is back
  b.fx_event(0, mod_end(0, audio::kModGroupOverdrive));
  CHECK_FALSE(b.perf_held(0, audio::kModGroupOverdrive));
  b.cut(0);
  render_n(b, g_got, 8);
  b.note_on(0, 60, 0, 127);
  render_n(b, g_got, 12);
  a.fx_event(0, mod_end(0, audio::kModGroupOverdrive));
  a.cut(0);
  render_n(a, g_ref, 8);
  a.note_on(0, 60, 0, 127);
  render_n(a, g_ref, 12);
  CHECK(std::memcmp(g_ref, g_got, 12 * kBlk * sizeof(std::int16_t)) == 0);
  // the layer alone on the bit depth is held, and a ModEnd lets it go
  b.fx_event(0, mod_set(audio::kModGroupBitDepth, 0, 0, 0, kLayerPerf, 5));
  CHECK(b.perf_held(0, audio::kModGroupBitDepth));
  b.fx_event(0, mod_end(0, audio::kModGroupBitDepth));
  CHECK_FALSE(b.perf_held(0, audio::kModGroupBitDepth));
  a.perf_clear();
  b.perf_clear();
}

// OS-BUG-MODDRIVE: the engine's gate for the module tracks' shaping is VoicePool::shape_mask: a bit for every track that holds the Overdrive or Bit depth FX (step or layer), and only those. FxReset
// ends the step FX, not the layer; the export's put-away clears the layer's, its restore brings it back (model: the P5 test below).
TEST_CASE("perform layer (OS-BUG-MODDRIVE): shape_mask names the tracks that hold an Overdrive or Bit depth FX, FxReset leaves the layer's, the export puts it away and back") {
  using audio::kLayerFx1;
  using audio::kLayerPerf;
  using audio::kModGroupBitDepth;
  using audio::kModGroupOverdrive;
  VoicePool& p = pool(0);
  CHECK(p.shape_mask() == 0u);
  p.fx_event(2, mod_set(kModGroupOverdrive, 2, audio::kFxOverdrive, 30, kLayerFx1, 0));
  p.fx_event(5, mod_set(kModGroupBitDepth, 5, audio::kFxBitDepth, 8, kLayerFx1, 0));
  p.fx_event(9, mod_set(kModGroupBitDepth, 9, 0, 0, kLayerPerf, 4));
  p.fx_event(11, mod_set(audio::kModGroupTune, 11, 0, 0, kLayerPerf, 3));  // another group: not the shaper's
  CHECK(p.shape_mask() == ((1u << 2) | (1u << 5) | (1u << 9)));
  p.clear_slide_fxs();  // FxReset: the step FX end, the layer stays
  CHECK(p.shape_mask() == (1u << 9));
  VoicePool::PerfSave away{};
  p.perf_save(away);
  p.perf_clear();
  CHECK(p.shape_mask() == 0u);
  p.perf_restore(away);
  CHECK(p.shape_mask() == (1u << 9));
  p.fx_event(9, mod_end(9, kModGroupBitDepth));
  CHECK(p.shape_mask() == 0u);
  p.perf_clear();
}

// OS-PERFFX P5: TUNE has the third slot (kPerfTuneSlot), held to -48..48 (the others to a byte), per track, across FxReset and Stopped; the slot and the byte are the voices' alone: 54 B for the three.
TEST_CASE("perform layer (P5): the voices hold the layer's TUNE per track in a slot of its own, held to -48..48, across FxReset and Stopped; a step ModSet or a ModEnd lets it go, and the other slots are left alone") {
  using audio::kLayerFx1;
  using audio::kLayerPerf;
  using audio::kModGroupTune;
  static_assert(VoicePool::kPerfSlots == 10 && sizeof(VoicePool::PerfSlots) == 10u * audio::kVoiceTracks + 10u * 2u, "ten slots (P7 added SMP POS and SMP END, OS-PERF-LFO the five LFO rows): a byte a track and a 16-bit mask each");
  static_assert(VoicePool::perf_slot_of(kModGroupTune) == VoicePool::kPerfTuneSlot && VoicePool::perf_slot_of(audio::kModGroupOverdrive) == 0u && VoicePool::perf_slot_of(audio::kModGroupBitDepth) == 1u,
                "one slot a group");
  ExportRig r;
  const auto held = [&](unsigned t, int v) { return r.p.perf_held(t, kModGroupTune) && r.p.perf_value(t, kModGroupTune) == v; };
  r.live({mod_set(kModGroupTune, 2, 0, 0, kLayerPerf, 12), mod_set(kModGroupTune, 9, 0, 0, kLayerPerf, -30), mod_set(audio::kModGroupOverdrive, 2, 0, 0, kLayerPerf, 40)});
  CHECK(held(2, 12));
  CHECK(held(9, -30));
  CHECK_FALSE(r.p.perf_held(3, kModGroupTune));
  CHECK(r.p.perf_value(2, audio::kModGroupOverdrive) == 40);  // each group its own slot
  CHECK_FALSE(r.p.perf_held(9, audio::kModGroupOverdrive));
  r.live({plain_event(EventKind::FxReset), plain_event(EventKind::Stopped)});
  CHECK(held(2, 12));
  CHECK(held(9, -30));
  // the range: 48 in either direction, whatever the event carries
  r.live({mod_set(kModGroupTune, 2, 0, 0, kLayerPerf, 1000), mod_set(kModGroupTune, 9, 0, 0, kLayerPerf, -1000)});
  CHECK(held(2, 48));
  CHECK(held(9, -48));
  r.live({mod_set(kModGroupTune, 2, 0, 0, kLayerPerf, 49), mod_set(kModGroupTune, 9, 0, 0, kLayerPerf, -49)});
  CHECK(held(2, 48));
  CHECK(held(9, -48));
  r.live({mod_set(kModGroupTune, 2, 0, 0, kLayerPerf, 47), mod_set(kModGroupTune, 9, 0, 0, kLayerPerf, -47)});
  CHECK(held(2, 47));
  CHECK(held(9, -47));
  // another group's event on the track leaves it; a ModSet that is not the layer's, or a ModEnd, lets it go
  r.live({mod_set(audio::kModGroupSlide, 2, audio::kFxSlideUp, 40, kLayerFx1, 0), mod_set(audio::kModGroupFilter, 2, 28, 40, kLayerFx1, 0)});
  CHECK(held(2, 47));
  r.live({mod_set(kModGroupTune, 2, 0, 0, kLayerFx1, 5), mod_end(9, kModGroupTune)});
  CHECK_FALSE(r.p.perf_held(2, kModGroupTune));
  CHECK_FALSE(r.p.perf_held(9, kModGroupTune));
  CHECK(r.p.perf_value(2, audio::kModGroupOverdrive) == 40);
  // the export puts the slot away and brings it back
  r.live({mod_set(kModGroupTune, 4, 0, 0, kLayerPerf, -7)});
  VoicePool::PerfSave s{};
  r.p.perf_save(s);
  r.p.perf_clear();
  CHECK_FALSE(r.p.perf_held(4, kModGroupTune));
  CHECK_FALSE(r.p.perf_held(2, audio::kModGroupOverdrive));
  r.p.perf_restore(s);
  CHECK(held(4, -7));
  CHECK(r.p.perf_value(2, audio::kModGroupOverdrive) == 40);
  r.p.perf_clear();
}

// OS-PERFFX P2: the strips' values are written by the layer's ModSet (the Volume and Panning groups, TrackerFx) and read by recompute_strip. FxReset and Stopped reset the tracker's gain and pan
// (end_volumes) and must leave the layer's.
TEST_CASE("perform layer (P2): the strips' values are the layer's own: FxReset and Stopped reset the tracker's gain and pan and leave them") {
  using audio::kLayerPerf;
  ExportRig r;
  audio::Graph& g = r.engine.graph();
  r.live({mod_set(audio::kModGroupVolume, 2, 0, 0, kLayerPerf, -40), mod_set(audio::kModGroupPanning, 2, 0, 0, kLayerPerf, 25), mod_set(audio::kModGroupVolume, 11, 0, 0, kLayerPerf, -100),
          mod_set(audio::kModGroupPanning, 15, 0, 0, kLayerPerf, 100)});
  CHECK(g.perf_gain(2) == -40);
  CHECK(g.perf_pan(2) == 25);
  g.set_track_gain(2, 0.5f, 1u);  // what the tracker FX hold (a channel volume, a panbrello)
  g.set_track_pan_mod(2, 30.0f, 1u);
  r.live({plain_event(EventKind::FxReset)});
  CHECK(g.track_gain(2) == 1.0f);  // end_volumes(true) ...
  CHECK(g.track_pan_mod(2) == 0.0f);
  CHECK(g.perf_gain(2) == -40);    // ... never reaches the layer's
  CHECK(g.perf_pan(2) == 25);
  CHECK(g.perf_gain(11) == -100);
  CHECK(g.perf_pan(15) == 100);
  r.live({plain_event(EventKind::Stopped), plain_event(EventKind::FxReset)});
  CHECK(g.perf_gain(2) == -40);
  CHECK(g.perf_pan(2) == 25);
  CHECK(g.perf_gain(3) == 0);
  CHECK(g.perf_gain(16) == 0);  // not a track
  // the layer lets go, and each field only its own
  r.live({mod_end(2, audio::kModGroupVolume)});
  CHECK(g.perf_gain(2) == 0);
  CHECK(g.perf_pan(2) == 25);
  r.live({mod_end(2, audio::kModGroupPanning)});
  CHECK(g.perf_pan(2) == 0);
}

TEST_CASE("perform layer: an export puts the layer away - the voices' slots, the performance filter, the strips - and gives all of it back, the live render's blocks in between") {
  using audio::kLayerPerf;
  ExportRig r;
  audio::Graph& g = r.engine.graph();
  r.live({mod_set(audio::kModGroupOverdrive, 3, 0, 0, kLayerPerf, 40), mod_set(audio::kModGroupBitDepth, 5, 0, 0, kLayerPerf, 7),
          mod_set(audio::kModGroupFilter, 0, 0, 0, kLayerPerf, audio::perf_filter_value(audio::kFilterHigh, -15))});
  audio::Graph::PerfSave s{};
  s.gain[1] = -60;
  s.pan[1] = -30;
  g.perf_restore(s);
  ExportSrc src;
  r.enter(src);
  CHECK_FALSE(r.p.perf_held(3, audio::kModGroupOverdrive));
  CHECK_FALSE(r.p.perf_held(5, audio::kModGroupBitDepth));
  CHECK(g.perf_gain(1) == 0);
  CHECK(g.perf_pan(1) == 0);
  std::vector<std::int16_t> off;
  r.export_blocks(4, off);
  CHECK(r.p.filter_cutoff(0) == -1.0f);  // the export's note has no performance filter
  r.engine.offline_leave();
  CHECK(r.p.perf_held(3, audio::kModGroupOverdrive));
  CHECK(r.p.perf_value(3, audio::kModGroupOverdrive) == 40);
  CHECK(r.p.perf_held(5, audio::kModGroupBitDepth));
  CHECK(r.p.perf_value(5, audio::kModGroupBitDepth) == 7);
  CHECK(g.perf_gain(1) == -60);
  CHECK(g.perf_pan(1) == -30);
  CHECK(std::fabs(r.live_note_cutoff() - 0.0f) < 1.0e-5f);  // the high-pass at -15 on an unfiltered instrument: base 0 - 0.15, held to 0..1; on, so not -1
  // a second export in a row (the stems), with a layer change in between that the live sequencer sent after the first
  r.live({mod_end(3, audio::kModGroupOverdrive)});
  r.enter(src);
  r.engine.offline_leave();
  CHECK_FALSE(r.p.perf_held(3, audio::kModGroupOverdrive));
  CHECK(r.p.perf_held(5, audio::kModGroupBitDepth));
}

// OS-PERFFX P1: the layer's OVERDRV and BITDEPTH, held in Master or Song while an export runs: the export's notes are the plain ones (byte for byte what Perform off makes), and the live note after the
// export is shaped again, as it is when no export came between.
namespace {

struct ShapeRun {
  std::vector<std::int16_t> exported;  // the export's blocks
  std::vector<std::int16_t> live_note;  // the voice's block of track 0, six blocks after a live note
};

ShapeRun shape_run(bool layer_held, bool export_between, int kind = 0) {  // kind 1: the layer is TUNE +7 (OS-PERFFX P5), 2: SMP POS +30 and SMP END -20 (P7), in place of OVERDRV and BITDEPTH
  ShapeRun run;
  ExportRig r;
  r.p.perf_clear();
  r.p.clear_slide_fxs();
  r.p.cut(0);
  r.live({});
  r.live({});
  r.live({});
  if (layer_held && kind == 1) {
    r.live({mod_set(audio::kModGroupTune, 0, 0, 0, audio::kLayerPerf, 7)});
  } else if (layer_held && kind == 3) {
    r.live({mod_set(audio::kModGroupReverse, 0, 0, 0, audio::kLayerPerf, 1)});  // OS-PERF-REVERSE (P8): PLAYBACK 1
  } else if (layer_held && kind == 2) {
    r.live({mod_set(audio::kModGroupPosition, 0, 0, 0, audio::kLayerPerf, 30), mod_set(audio::kModGroupSampleEnd, 0, 0, 0, audio::kLayerPerf, -20)});
  } else if (layer_held) {
    r.live({mod_set(audio::kModGroupOverdrive, 0, 0, 0, audio::kLayerPerf, 90), mod_set(audio::kModGroupBitDepth, 0, 0, 0, audio::kLayerPerf, 9)});
  }
  if (export_between) {
    ExportSrc src;
    r.enter(src);
    r.export_blocks(12, run.exported);
    r.engine.offline_leave();
    r.live({});
    r.live({});
    r.live({});
  }
  SeqEvent on{};
  on.kind = EventKind::NoteOn;
  on.a = 60;
  on.b = 0;
  on.c = 127;
  r.live({on});
  for (unsigned b = 0; b < 6; ++b) {
    const std::int16_t* const f = r.p.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      run.live_note.push_back(f != nullptr ? f[i] : std::int16_t{0});
    }
    r.live({});
  }
  r.live({mod_end(0, audio::kModGroupOverdrive), mod_end(0, audio::kModGroupBitDepth), mod_end(0, audio::kModGroupTune), mod_end(0, audio::kModGroupPosition), mod_end(0, audio::kModGroupSampleEnd),
          mod_end(0, audio::kModGroupReverse)});
  r.p.perf_clear();
  return run;
}

}  // namespace

TEST_CASE("perform layer P1: a held OVERDRV and BITDEPTH do not shape an export, and shape the live note after it as they do without an export") {
  const ShapeRun plain = shape_run(false, true);                 // Perform off, an export between
  const ShapeRun held = shape_run(true, true);                   // the layer held, an export between
  const ShapeRun no_export = shape_run(true, false);             // the layer held, no export
  CHECK(plain.exported.size() == held.exported.size());
  CHECK(plain.exported == held.exported);                        // the export is what Perform off makes, byte for byte
  std::int32_t peak = 0;
  for (const std::int16_t v : plain.exported) {
    peak = std::max<std::int32_t>(peak, v < 0 ? -v : v);
  }
  CHECK(peak > 1000);                                            // the note sounded: a comparison of silence would prove nothing
  NEED(held.live_note.size() == no_export.live_note.size());
  CHECK(held.live_note == no_export.live_note);                  // the layer is back after the export
  CHECK(held.live_note != plain.live_note);                      // ... and it shapes: the plain note is another sound
}

// OS-PERFFX P5: the same for TUNE. A held TUNE does not move the export's notes (the voices put the slot away: the export's note starts at its own pitch) and moves the live note after it as it does
// without an export; the live note's pitch is the slot's, so the plain note and the held one are other sounds.
TEST_CASE("perform layer (P5): a held TUNE does not move an export's notes, and moves the live note after it as it does without an export") {
  const ShapeRun plain = shape_run(false, true);
  const ShapeRun held = shape_run(true, true, 1);
  const ShapeRun no_export = shape_run(true, false, 1);
  CHECK(plain.exported.size() == held.exported.size());
  CHECK(plain.exported == held.exported);
  std::int32_t peak = 0;
  for (const std::int16_t v : plain.exported) {
    peak = std::max<std::int32_t>(peak, v < 0 ? -v : v);
  }
  CHECK(peak > 1000);
  NEED(held.live_note.size() == no_export.live_note.size());
  CHECK(held.live_note == no_export.live_note);
  CHECK(held.live_note != plain.live_note);
}

// OS-PERFFX P7: and for SMP POS and SMP END. The slots are put away with the others, so the export's notes start at frame 0 and play to the end; the live note after it starts at the layer's frame.
TEST_CASE("perform layer (P7): a held SMP POS and SMP END do not move an export's notes, and move the live note after it as they do without an export") {
  const ShapeRun plain = shape_run(false, true);
  const ShapeRun held = shape_run(true, true, 2);
  const ShapeRun no_export = shape_run(true, false, 2);
  CHECK(plain.exported.size() == held.exported.size());
  CHECK(plain.exported == held.exported);
  std::int32_t peak = 0;
  for (const std::int16_t v : plain.exported) {
    peak = std::max<std::int32_t>(peak, v < 0 ? -v : v);
  }
  CHECK(peak > 1000);
  NEED(held.live_note.size() == no_export.live_note.size());
  CHECK(held.live_note == no_export.live_note);
  CHECK(held.live_note != plain.live_note);
  const ShapeRun again = shape_run(true, true, 2);  // the same twice: the export with the layer held is deterministic
  CHECK(again.exported == held.exported);
  CHECK(again.live_note == held.live_note);
}

// P7: the Position and Sample end groups have the fourth and fifth slots, held to -100..100, per track, across FxReset and Stopped; a step ModSet or a ModEnd lets them go, and the other slots are left alone.
TEST_CASE("perform layer (P7): the voices hold the layer's SMP POS and SMP END per track in slots of their own, held to -100..100, across FxReset and Stopped") {
  using audio::kLayerFx1;
  using audio::kLayerPerf;
  using audio::kModGroupPosition;
  using audio::kModGroupSampleEnd;
  static_assert(VoicePool::perf_slot_of(kModGroupPosition) == VoicePool::kPerfPosSlot && VoicePool::perf_slot_of(kModGroupSampleEnd) == VoicePool::kPerfEndSlot && VoicePool::kPerfEndSlot < VoicePool::kPerfSlots,
                "one slot a group");
  ExportRig r;
  const auto held = [&](unsigned t, unsigned g, int v) { return r.p.perf_held(t, g) && r.p.perf_value(t, g) == v; };
  r.live({mod_set(kModGroupPosition, 2, 0, 0, kLayerPerf, 40), mod_set(kModGroupSampleEnd, 2, 0, 0, kLayerPerf, -60), mod_set(kModGroupPosition, 9, 0, 0, kLayerPerf, -5),
          mod_set(audio::kModGroupTune, 2, 0, 0, kLayerPerf, 12)});
  CHECK(held(2, kModGroupPosition, 40));
  CHECK(held(2, kModGroupSampleEnd, -60));
  CHECK(held(9, kModGroupPosition, -5));
  CHECK_FALSE(r.p.perf_held(9, kModGroupSampleEnd));
  CHECK_FALSE(r.p.perf_held(3, kModGroupPosition));
  CHECK(held(2, audio::kModGroupTune, 12));
  r.live({plain_event(EventKind::FxReset), plain_event(EventKind::Stopped)});
  CHECK(held(2, kModGroupPosition, 40));
  CHECK(held(2, kModGroupSampleEnd, -60));
  r.live({mod_set(kModGroupPosition, 2, 0, 0, kLayerPerf, 1000), mod_set(kModGroupSampleEnd, 2, 0, 0, kLayerPerf, -1000)});
  CHECK(held(2, kModGroupPosition, 100));
  CHECK(held(2, kModGroupSampleEnd, -100));
  // the step FX 22 shares the Position group: its ModSet is not the layer's, and lets the slot go
  r.live({mod_set(kModGroupPosition, 2, 22, 50, kLayerFx1, 0), mod_end(2, kModGroupSampleEnd)});
  CHECK_FALSE(r.p.perf_held(2, kModGroupPosition));
  CHECK_FALSE(r.p.perf_held(2, kModGroupSampleEnd));
  CHECK(held(2, audio::kModGroupTune, 12));
  r.live({mod_set(kModGroupSampleEnd, 4, 0, 0, kLayerPerf, 33)});
  VoicePool::PerfSave sv{};
  r.p.perf_save(sv);
  r.p.perf_clear();
  CHECK_FALSE(r.p.perf_held(4, kModGroupSampleEnd));
  r.p.perf_restore(sv);
  CHECK(held(4, kModGroupSampleEnd, 33));
  r.p.perf_clear();
}

// OS-PERF-REVERSE (P8): and for PLAYBACK. The layer's mask is put away with the slots, so the export's notes play forward (byte for byte what Perform off makes); the live note after it plays backward.
TEST_CASE("perform layer (P8): a held PLAYBACK does not reverse an export's notes, and reverses the live note after it as it does without an export") {
  const ShapeRun plain = shape_run(false, true);
  const ShapeRun held = shape_run(true, true, 3);
  const ShapeRun no_export = shape_run(true, false, 3);
  CHECK(plain.exported.size() == held.exported.size());
  CHECK(plain.exported == held.exported);
  std::int32_t peak = 0;
  for (const std::int16_t v : plain.exported) {
    peak = std::max<std::int32_t>(peak, v < 0 ? -v : v);
  }
  CHECK(peak > 1000);
  NEED(held.live_note.size() == no_export.live_note.size());
  CHECK(held.live_note == no_export.live_note);
  CHECK(held.live_note != plain.live_note);
  const ShapeRun again = shape_run(true, true, 3);
  CHECK(again.exported == held.exported);
  CHECK(again.live_note == held.live_note);
}

// P8: the Reverse group's two sources are told by every event of it. The layer's PLAYBACK (value != 0) survives FxReset and Stopped, the step FX 26 (a == 26, b != 0) ends with FxReset and not with
// Stopped; either lets go at its ModEnd; the layer's end brings the step FX back (a and b of the same event name it); the export puts the layer away and clears the step FX.
TEST_CASE("perform layer (P8): PLAYBACK and the step FX 26 are held per track, FxReset ends the step FX and not the layer, the export clears both") {
  using audio::kFxReverse;
  using audio::kLayerFx1;
  using audio::kLayerPerf;
  using audio::kModGroupReverse;
  ExportRig r;
  r.live({mod_set(kModGroupReverse, 2, 0, 0, kLayerPerf, 1), mod_set(kModGroupReverse, 3, kFxReverse, 1, kLayerFx1, 0), mod_set(kModGroupReverse, 4, kFxReverse, 1, kLayerPerf, 1),
          mod_set(kModGroupReverse, 5, kFxReverse, 0, kLayerFx1, 0), mod_set(kModGroupReverse, 6, 18, 1, kLayerFx1, 0)});
  CHECK(r.p.perf_held(2, kModGroupReverse));
  CHECK_FALSE(r.p.step_reverse(2));
  CHECK_FALSE(r.p.perf_held(3, kModGroupReverse));
  CHECK(r.p.step_reverse(3));
  CHECK(r.p.perf_held(4, kModGroupReverse));  // the layer over the FX: both are told
  CHECK(r.p.step_reverse(4));
  CHECK_FALSE(r.p.step_reverse(5));  // the FX 26 with 0 is forward
  CHECK_FALSE(r.p.step_reverse(6));  // another FX of the group's number is not the Reverse FX
  CHECK_FALSE(r.p.perf_held(1, kModGroupReverse));
  CHECK(r.p.perf_value(2, kModGroupReverse) == 1);
  // the layer ends over the FX: the control table sends the FX again, the layer lets go
  r.live({mod_set(kModGroupReverse, 4, kFxReverse, 1, kLayerFx1, 0)});
  CHECK_FALSE(r.p.perf_held(4, kModGroupReverse));
  CHECK(r.p.step_reverse(4));
  r.live({plain_event(EventKind::Stopped)});
  CHECK(r.p.perf_held(2, kModGroupReverse));
  CHECK(r.p.step_reverse(3));
  r.live({plain_event(EventKind::FxReset)});
  CHECK(r.p.perf_held(2, kModGroupReverse));
  CHECK_FALSE(r.p.step_reverse(3));
  CHECK_FALSE(r.p.step_reverse(4));
  r.live({mod_set(kModGroupReverse, 3, kFxReverse, 1, kLayerFx1, 0), mod_set(kModGroupReverse, 7, 0, 0, kLayerPerf, 0)});
  CHECK(r.p.step_reverse(3));
  CHECK_FALSE(r.p.perf_held(7, kModGroupReverse));  // a layer value of 0 is the end
  r.live({mod_end(3, kModGroupReverse), mod_end(2, kModGroupReverse)});
  CHECK_FALSE(r.p.step_reverse(3));
  CHECK_FALSE(r.p.perf_held(2, kModGroupReverse));
  // the export
  r.live({mod_set(kModGroupReverse, 2, 0, 0, kLayerPerf, 1), mod_set(kModGroupReverse, 3, kFxReverse, 1, kLayerFx1, 0)});
  ExportSrc src;
  r.enter(src);
  CHECK_FALSE(r.p.perf_held(2, kModGroupReverse));
  CHECK_FALSE(r.p.step_reverse(3));
  r.engine.offline_leave();
  CHECK(r.p.perf_held(2, kModGroupReverse));
  CHECK_FALSE(r.p.step_reverse(3));
  r.p.perf_clear();
}

// ---- OS-PERFFX P2: the layer's VOLUME and PANNING on the strips -------------------------------------------------------------------------------------------------------------------------

namespace {

struct Peaks {
  std::int32_t left = 0;
  std::int32_t right = 0;
};

// The largest sample of each channel of the block the rig rendered last (the output is L, R interleaved).
Peaks peaks_of(const std::int16_t* b) {
  Peaks p;
  for (std::uint32_t i = 0; i + 1u < haudio::kBlockSamples; i += 2u) {
    p.left = std::max<std::int32_t>(p.left, std::abs(static_cast<std::int32_t>(b[i])));
    p.right = std::max<std::int32_t>(p.right, std::abs(static_cast<std::int32_t>(b[i + 1u])));
  }
  return p;
}

SeqEvent note_on_0() {
  SeqEvent on{};
  on.kind = EventKind::NoteOn;
  on.a = 60;
  on.b = 0;
  on.c = 127;
  return on;
}

// The loudest of each channel over `blocks` live blocks of a note on track 0 whose layer events came a block before it; the strips have reached their targets by then (the smoothing is a few blocks).
// `track_pan` >= 0: the pan the tracker's FX would hold on track 0 (Graph::set_track_pan, after the note's start has set the instrument's), 0..100.
Peaks live_peaks(std::initializer_list<SeqEvent> layer, unsigned blocks = 40u, float track_pan = -1.0f) {
  ExportRig r;
  r.live(layer);
  r.live({note_on_0()});
  if (track_pan >= 0.0f) {
    r.engine.graph().set_track_pan(0, track_pan, 1u);
  }
  Peaks all;
  for (unsigned b = 0; b < blocks; ++b) {
    r.live({});
    if (b >= 16u) {  // (the last blocks only: the strips' ramps are over)
      const Peaks p = peaks_of(r.out);
      all.left = std::max(all.left, p.left);
      all.right = std::max(all.right, p.right);
    }
  }
  return all;
}

}  // namespace

TEST_CASE("perform layer (P2): VOLUME only cuts - (1 + v/100)^2 below 0, nothing from 0 up - and PANNING adds to the pan, held to the ends") {
  using audio::kLayerPerf;
  const Peaks ref = live_peaks({});
  NEED(ref.left > 3000);  // the note sounds, on both channels (the centre pan is unity on both)
  NEED(ref.right > 3000);
  const auto vol = [&](int v) { return live_peaks({mod_set(audio::kModGroupVolume, 0, 0, 0, kLayerPerf, v)}); };
  const auto pan = [&](int v) { return live_peaks({mod_set(audio::kModGroupPanning, 0, 0, 0, kLayerPerf, v)}); };
  // VOLUME: the curve, to 1 % of the reference's peak
  struct Case {
    int v;
    double gain;
  };
  for (const Case c : {Case{-100, 0.0}, Case{-90, 0.01}, Case{-50, 0.25}, Case{-20, 0.64}, Case{-1, 0.9801}}) {
    const Peaks p = vol(c.v);
    CHECK_MESSAGE(std::fabs(static_cast<double>(p.left) - c.gain * ref.left) <= 0.01 * ref.left + 1.0, "v " << c.v << " left " << p.left << " of " << ref.left);
    CHECK_MESSAGE(std::fabs(static_cast<double>(p.right) - c.gain * ref.right) <= 0.01 * ref.right + 1.0, "v " << c.v << " right " << p.right << " of " << ref.right);
  }
  for (const int v : {0, 1, 50, 100}) {  // 0 dB: the same samples as with no layer
    const Peaks p = vol(v);
    CHECK_MESSAGE((p.left == ref.left && p.right == ref.right), "v " << v);
  }
  // PANNING: the instrument's pan is 50; +25 is 75 (the left gain halves), +-50 and past are the hard sides
  const Peaks p25 = pan(25);
  CHECK(std::fabs(static_cast<double>(p25.left) - 0.5 * ref.left) <= 0.01 * ref.left + 1.0);
  CHECK(p25.right == ref.right);
  const Peaks m25 = pan(-25);
  CHECK(m25.left == ref.left);
  CHECK(std::fabs(static_cast<double>(m25.right) - 0.5 * ref.right) <= 0.01 * ref.right + 1.0);
  for (const int v : {50, 100}) {
    const Peaks p = pan(v);
    CHECK_MESSAGE((p.left <= 1 && p.right == ref.right), "v " << v);
  }
  for (const int v : {-50, -100}) {
    const Peaks p = pan(v);
    CHECK_MESSAGE((p.right <= 1 && p.left == ref.left), "v " << v);
  }
  // an off-centre track: the layer adds to the tracker's pan (a Panning FX of 80 here), it does not replace it
  const auto pan_at_80 = [&](std::initializer_list<SeqEvent> layer) { return live_peaks(layer, 40u, 80.0f); };
  const Peaks t80 = pan_at_80({});
  CHECK(std::fabs(static_cast<double>(t80.left) - 0.4 * ref.left) <= 0.01 * ref.left + 1.0);  // pan 80: the left gain is 20 / 50
  CHECK(t80.right == ref.right);
  const Peaks t80p = pan_at_80({mod_set(audio::kModGroupPanning, 0, 0, 0, kLayerPerf, 20)});  // 80 + 20 reaches the right side
  CHECK_MESSAGE((t80p.left <= 1 && t80p.right == ref.right), "left " << t80p.left << " right " << t80p.right);
  const Peaks t80m = pan_at_80({mod_set(audio::kModGroupPanning, 0, 0, 0, kLayerPerf, -30)});  // 80 - 30 is the centre again
  CHECK_MESSAGE((t80m.left == ref.left && t80m.right == ref.right), "left " << t80m.left << " right " << t80m.right);
  // both together: the cut is on both channels, the pan on the sides
  const Peaks both = live_peaks({mod_set(audio::kModGroupVolume, 0, 0, 0, kLayerPerf, -50), mod_set(audio::kModGroupPanning, 0, 0, 0, kLayerPerf, 100)});
  CHECK(both.left <= 1);
  CHECK(std::fabs(static_cast<double>(both.right) - 0.25 * ref.right) <= 0.01 * ref.right + 1.0);
}

TEST_CASE("perform layer (P2): the tracker's gain and pan stay their own - they multiply and add with the layer's, and the layer outlives their reset") {
  using audio::kLayerPerf;
  const Peaks ref = live_peaks({});
  ExportRig r;
  audio::Graph& g = r.engine.graph();
  r.live({mod_set(audio::kModGroupVolume, 0, 0, 0, kLayerPerf, -50)});
  g.set_track_gain(0, 0.5f, 0u);  // a channel volume of 32
  r.live({note_on_0()});
  Peaks p;
  for (unsigned b = 0; b < 40u; ++b) {
    r.live({});
  }
  p = peaks_of(r.out);
  CHECK(std::fabs(static_cast<double>(p.left) - 0.25 * 0.5 * ref.left) <= 0.015 * ref.left + 1.0);  // 0.25 of the layer, 0.5 of the tracker's
  r.live({plain_event(EventKind::FxReset)});  // the tracker's gain goes, the layer's stays
  for (unsigned b = 0; b < 40u; ++b) {
    r.live({});
  }
  p = peaks_of(r.out);
  CHECK(std::fabs(static_cast<double>(p.left) - 0.25 * ref.left) <= 0.01 * ref.left + 1.0);
}

TEST_CASE("perform layer (P2): VOLUME -100 gates the track's voice, and the voice is heard again at once when the layer ends") {
  using audio::kLayerPerf;
  ExportRig r;
  r.live({mod_set(audio::kModGroupVolume, 0, 0, 0, kLayerPerf, -100)});
  r.live({note_on_0()});
  for (unsigned b = 0; b < 24u; ++b) {
    r.live({});
  }
  const Peaks silent = peaks_of(r.out);
  CHECK(silent.left == 0);
  CHECK(silent.right == 0);
  CHECK(r.p.stats().gated_blocks > 0u);  // the strip is inaudible in its smoothers: the voice skips its post stages
  r.live({mod_end(0, audio::kModGroupVolume)});
  Peaks back;
  Peaks early;  // the first blocks after the ModEnd: the voice is audible again within the gate's ramp, not after the strips' whole smoothing
  for (unsigned b = 0; b < 24u; ++b) {
    r.live({});
    if (b < 3u) {
      early.left = std::max(early.left, peaks_of(r.out).left);
    }
    if (b >= 12u) {
      const Peaks p = peaks_of(r.out);
      back.left = std::max(back.left, p.left);
    }
  }
  CHECK_MESSAGE(early.left > 0, "silent in the first 3 blocks after the ModEnd");
  CHECK(back.left > 3000);
  const Peaks ref = live_peaks({});
  CHECK(back.left <= ref.left + 1);
}

namespace {

// An export with the layer's VOLUME and PANNING held on track 0 (the strips have reached them before the export starts), against one without; the result is the export's samples. `after`: the loudest of
// each channel of the live note that follows the export (the layer must be back on the strips at once, not smoothed up from the export's levels).
std::vector<std::int16_t> export_with_strips(bool layer_held, int volume, int pan, Peaks* after, int* gain_back, int* pan_back) {
  using audio::kLayerPerf;
  ExportRig r;
  if (layer_held) {
    r.live({mod_set(audio::kModGroupVolume, 0, 0, 0, kLayerPerf, volume), mod_set(audio::kModGroupPanning, 0, 0, 0, kLayerPerf, pan)});
    for (unsigned b = 0; b < 16u; ++b) {
      r.live({});  // the strips are at the layer's levels
    }
  }
  ExportSrc src;
  r.enter(src);
  std::vector<std::int16_t> samples;
  r.export_blocks(12, samples);
  r.engine.offline_leave();
  *gain_back = r.engine.graph().perf_gain(0);
  *pan_back = r.engine.graph().perf_pan(0);
  r.live({note_on_0()});
  for (unsigned b = 0; b < 24u; ++b) {
    r.live({});
    if (b >= 12u) {
      const Peaks p = peaks_of(r.out);
      after->left = std::max(after->left, p.left);
      after->right = std::max(after->right, p.right);
    }
  }
  return samples;
}

}  // namespace

TEST_CASE("perform layer (P2): an export is byte for byte what it is with Perform off, whatever VOLUME and PANNING are held, and the layer is back for the next live note") {
  const Peaks ref = live_peaks({});
  Peaks off_after;
  int g0 = 0;
  int p0 = 0;
  const std::vector<std::int16_t> off = export_with_strips(false, 0, 0, &off_after, &g0, &p0);
  std::int32_t peak = 0;
  for (const std::int16_t s : off) {
    peak = std::max<std::int32_t>(peak, s < 0 ? -s : s);
  }
  CHECK(peak > 1000);  // the note sounded: a comparison of silence would prove nothing
  CHECK(g0 == 0);
  CHECK(p0 == 0);
  CHECK(std::abs(off_after.left - ref.left) <= ref.left / 200);  // (the note after an export is the same note to within the master's own state)
  struct Case {
    int volume;
    int pan;
  };
  for (const Case c : {Case{-100, 0}, Case{-100, 100}, Case{-50, 25}, Case{0, -100}, Case{-1, 0}}) {
    Peaks after;
    int g = 0;
    int pn = 0;
    const std::vector<std::int16_t> held = export_with_strips(true, c.volume, c.pan, &after, &g, &pn);
    CHECK_MESSAGE(held == off, "volume " << c.volume << " pan " << c.pan);
    CHECK(g == c.volume);  // the layer is back in the strips ...
    CHECK(pn == c.pan);
    // ... and heard on the next note: the export's levels did not linger
    const double gain = c.volume < 0 ? (1.0 + c.volume / 100.0) * (1.0 + c.volume / 100.0) : 1.0;
    const double lg = c.pan > 0 ? std::max(0.0, 1.0 - c.pan / 50.0) : 1.0;
    const double rg = c.pan < 0 ? std::max(0.0, 1.0 + c.pan / 50.0) : 1.0;
    CHECK_MESSAGE(std::fabs(static_cast<double>(after.left) - gain * lg * ref.left) <= 0.015 * ref.left + 1.0, "volume " << c.volume << " pan " << c.pan << " left " << after.left);
    CHECK_MESSAGE(std::fabs(static_cast<double>(after.right) - gain * rg * ref.right) <= 0.015 * ref.right + 1.0, "volume " << c.volume << " pan " << c.pan << " right " << after.right);
  }
}

// ---- OS-PERFFX P3: the layer's DELAY and REVERB, and the step FX 30 and 32, on the strips' sends -------------------------------------------------------------------------------------------
// The sends are measured where the send effects are fed: two stand-in effects keep the peak of their input block and return it as it is (so an export carries what the sends would add). The note is a
// looping DC, so a level is the same in every block once the smoothers have settled, and "the same level" is "the same peak, to the sample".

namespace {

std::int16_t g_dc[256];

SampleDesc dc_desc(unsigned delay_send, unsigned reverb_send) {
  for (std::int16_t& v : g_dc) {
    v = 16384;
  }
  SampleDesc d{g_dc, 256, 0, 256, 65536};
  d.delay_send = static_cast<std::uint8_t>(delay_send);
  d.reverb_send = static_cast<std::uint8_t>(reverb_send);
  return d;
}

struct SendTap {
  std::int32_t peak = 0;  // the largest |sample| of the last block the effect was given (0 when it was not called)
};
SendTap g_tap[2];

void send_tap_fx(void* state, const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept {
  std::int32_t m = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t v = in[i] < 0 ? -static_cast<std::int32_t>(in[i]) : in[i];
    m = v > m ? v : m;
    out_l[i] = in[i];
    out_r[i] = in[i];
  }
  static_cast<SendTap*>(state)->peak = m;
}

struct SendPeaks {
  std::int32_t delay = 0;
  std::int32_t reverb = 0;
};

// The rig: both send effects on, instrument 0 with the sends given, a note on track 0 whenever the test asks.
struct SendRig {
  explicit SendRig(unsigned delay_send = 0, unsigned reverb_send = 0, bool tapped = true) {
    inst(0, delay_send, reverb_send);
    g_tap[0] = SendTap{};
    g_tap[1] = SendTap{};
    if (tapped) {
      audio::Graph& g = r.engine.graph();
      g.install_send_fx(0, &send_tap_fx, &g_tap[0]);
      g.set_send_fx_bypass(0, false);
      g.install_send_fx(1, &send_tap_fx, &g_tap[1]);
      g.set_send_fx_bypass(1, false);
    }
  }
  void inst(unsigned slot, unsigned delay_send, unsigned reverb_send) { r.p.set_sample(slot, dc_desc(delay_send, reverb_send)); }
  void note(unsigned slot = 0) {
    SeqEvent on = note_on_0();
    on.b = static_cast<std::uint8_t>(slot);
    r.live({on});
  }
  // `first` in a block, a dozen blocks for the smoothers, then the peaks of one more block.
  SendPeaks level(std::initializer_list<SeqEvent> first = {}) {
    r.live(first);
    for (unsigned b = 0; b < 12u; ++b) {
      r.live({});
    }
    g_tap[0].peak = 0;
    g_tap[1].peak = 0;
    r.live({});
    return SendPeaks{g_tap[0].peak, g_tap[1].peak};
  }
  ExportRig r;
};

// The peaks of a note on track 0 whose instrument has the sends given and whose layer events (and step events) came a block before it.
SendPeaks send_level(unsigned delay_send, unsigned reverb_send, std::initializer_list<SeqEvent> layer = {}) {
  SendRig s(delay_send, reverb_send);
  s.r.live(layer);
  s.note();
  return s.level();
}

SeqEvent send_set(bool reverb, std::uint8_t layer, int perf_value, std::uint8_t step_value = 0, bool step = false) {
  return mod_set(reverb ? audio::kModGroupReverbSend : audio::kModGroupDelaySend, 0, step ? (reverb ? audio::kFxReverbSend : audio::kFxDelaySend) : 0u, step_value, layer, perf_value);
}

}  // namespace

TEST_CASE("perform layer (P3): DELAY and REVERB add to the instrument's send as an offset of a full send, held to 0..100 - -100 closes it, +100 opens it whole") {
  using audio::kLayerPerf;
  const SendPeaks none = send_level(0, 0);
  const SendPeaks full = send_level(100, 100);
  NEED(none.delay == 0);
  NEED(none.reverb == 0);
  NEED(full.delay > 3000);  // the note reaches both effects at full send
  NEED(full.reverb > 3000);
  const SendPeaks mid = send_level(50, 50);
  NEED(mid.delay > 0);
  NEED(mid.delay < full.delay);
  CHECK(mid.delay == mid.reverb);
  struct Case {
    unsigned inst_d, inst_r;
    int layer_d, layer_r;
    unsigned want_d, want_r;
  };
  for (const Case c : {Case{40, 80, 30, -30, 70, 50}, Case{60, 60, -100, -100, 0, 0}, Case{20, 20, 100, 100, 100, 100}, Case{100, 100, 50, 50, 100, 100}, Case{0, 0, -50, -50, 0, 0},
                       Case{30, 30, 1, -1, 31, 29}, Case{70, 0, -20, 40, 50, 40}}) {
    const SendPeaks got = send_level(c.inst_d, c.inst_r, {send_set(false, kLayerPerf, c.layer_d), send_set(true, kLayerPerf, c.layer_r)});
    const SendPeaks want = send_level(c.want_d, c.want_r);
    CHECK_MESSAGE(got.delay == want.delay, "delay " << c.inst_d << " + " << c.layer_d << " = " << c.want_d << ": " << got.delay << " against " << want.delay);
    CHECK_MESSAGE(got.reverb == want.reverb, "reverb " << c.inst_r << " + " << c.layer_r << " = " << c.want_r << ": " << got.reverb << " against " << want.reverb);
  }
  // each send is its own
  const SendPeaks only_d = send_level(0, 30, {send_set(false, kLayerPerf, 100)});
  CHECK(only_d.delay == full.delay);
  CHECK(only_d.reverb == send_level(0, 30).reverb);
}

TEST_CASE("perform layer (P3): the offset is added after the strip's own send and the instrument's are held to 100, and it is not gated by the routing bit") {
  using audio::kLayerPerf;
  namespace gps = audio::graph_param::strip;
  const auto run = [&](int layer_d) {
    SendRig s(70, 0);
    // SND A open, the strip's own send at 80: with the instrument's 70 it is 150, held to 100; -100 then closes the send whatever the sum was
    CHECK(s.r.engine.params().push(core::ParamChange{audio::strip_param(0, gps::Route), static_cast<float>(audio::route::kMain | audio::route::kSndA), 1u}));
    CHECK(s.r.engine.params().push(core::ParamChange{audio::strip_param(0, gps::SendA), 80.0f, 1u}));
    s.r.live(layer_d != 0 ? std::initializer_list<SeqEvent>{send_set(false, kLayerPerf, layer_d)} : std::initializer_list<SeqEvent>{});
    s.note();
    return s.level().delay;
  };
  const std::int32_t full = send_level(100, 0).delay;
  CHECK(run(0) == full);
  CHECK(run(-100) == 0);
  CHECK(run(-30) == send_level(70, 0).delay);
  // the instrument's send is open on the default route (no SND bit), and so is the layer's
  CHECK(send_level(0, 0, {send_set(false, kLayerPerf, 100)}).delay == full);
}

TEST_CASE("perform layer (P3): the offset outlives the next note and a live edit of the instrument's send, and the instrument's send is the next note's base") {
  using audio::kLayerPerf;
  // (the references first: every rig remakes the one engine of the test binary)
  const std::int32_t at40 = send_level(40, 0).delay;
  const std::int32_t at70 = send_level(70, 0).delay;
  const std::int32_t at80 = send_level(80, 0).delay;
  NEED(at40 > 0);
  NEED(at40 < at70);
  NEED(at70 < at80);
  SendRig s(40, 0);
  s.inst(1, 10, 0);
  s.r.live({send_set(false, kLayerPerf, 30)});
  s.note(0);
  CHECK(s.level().delay == at70);
  s.note(1);  // the next note's start gives the strip the instrument 1's 10: the layer is still on top of it
  CHECK(s.level().delay == at40);
  // a live edit of instrument 1's DELAY SEND reaches the track (Engine::instrument_hook) and the layer is on top of it as well
  CHECK(s.r.engine.params().push(core::ParamChange{core::param_id(core::ParamKind::Instrument, 1, audio::inst_param::DelaySend), 50.0f, 0u}));
  CHECK(s.level().delay == at80);
  CHECK(s.r.engine.graph().perf_send(0, 0) == 30);
}

TEST_CASE("perform layer (P3): the step FX 30 and 32 hold the track's send in place of the instrument's, the layer is added to it, and each ending gives the one underneath back") {
  using audio::kLayerFx1;
  using audio::kLayerPerf;
  for (const bool reverb : {false, true}) {
    CAPTURE(reverb);
    const auto at = [&](unsigned level) { return reverb ? send_level(0, level).reverb : send_level(level, 0).delay; };  // (a rig remakes the engine: the references come first)
    const std::int32_t l5 = at(5), l20 = at(20), l45 = at(45), l60 = at(60), l80 = at(80), l100 = at(100);
    NEED(l5 > 0);
    NEED(l5 < l20);
    NEED(l60 < l80);
    NEED(l80 < l100);
    const auto got = [&](const SendPeaks& p) { return reverb ? p.reverb : p.delay; };
    const unsigned g = reverb ? audio::kModGroupReverbSend : audio::kModGroupDelaySend;
    const unsigned which = reverb ? 1u : 0u;
    SendRig s(reverb ? 0 : 20, reverb ? 20 : 0);
    s.note();
    CHECK(got(s.level()) == l20);
    CHECK(got(s.level({send_set(reverb, kLayerFx1, 0, 60, true)})) == l60);  // the FX replaces the instrument's 20
    CHECK(s.r.engine.graph().step_send(0, which) == 60);
    CHECK(got(s.level({send_set(reverb, kLayerPerf, 20, 60, true)})) == l80);  // the layer over the FX: 60 + 20
    CHECK(got(s.level({send_set(reverb, kLayerFx1, 0, 60, true)})) == l60);  // the layer lets go with the FX still there: the FX's value
    CHECK(s.r.engine.graph().perf_send(0, which) == 0);
    CHECK(got(s.level({send_set(reverb, kLayerFx1, 0, 255, true)})) == l100);  // held to 100
    s.note();  // a note's start does not give the instrument's send back while the FX is held
    CHECK(got(s.level()) == l100);
    CHECK(got(s.level({mod_end(0, g)})) == l20);  // the FX's end: the instrument's send
    CHECK(s.r.engine.graph().step_send(0, which) == -1);
    // the layer alone, then the FX under it, then the transport starts (FxReset): the FX ends, the layer does not
    CHECK(got(s.level({send_set(reverb, kLayerPerf, -15)})) == l5);
    CHECK(s.r.engine.graph().perf_send(0, which) == -15);
    CHECK(got(s.level({send_set(reverb, kLayerPerf, -15, 60, true)})) == l45);
    CHECK(got(s.level({plain_event(EventKind::FxReset)})) == l5);
    CHECK(s.r.engine.graph().step_send(0, which) == -1);
    CHECK(s.r.engine.graph().perf_send(0, which) == -15);
    // a stop leaves a step FX held, as it leaves the Cutoff FX (new Community OS behaviour, no change): the next live note still takes its send, and the next run's FxReset ends it
    CHECK(got(s.level({send_set(reverb, kLayerPerf, -15, 60, true)})) == l45);
    s.r.live({plain_event(EventKind::Stopped)});
    CHECK(s.r.engine.graph().step_send(0, which) == 60);
    CHECK(s.r.engine.graph().perf_send(0, which) == -15);
    s.note();
    CHECK(got(s.level()) == l45);
    CHECK(got(s.level({plain_event(EventKind::FxReset)})) == l5);
    CHECK(s.r.engine.graph().step_send(0, which) == -1);
    s.r.live({plain_event(EventKind::Stopped)});  // a stop ends the voices and leaves the layer (the Perform screen owns it)
    s.note();
    CHECK(got(s.level()) == l5);
    CHECK(got(s.level({mod_end(0, g)})) == l20);
    CHECK(s.r.engine.graph().perf_send(0, which) == 0);
  }
}

TEST_CASE("perform layer (P3): the group's other track and a bad track are left alone, each send keeps its own value, and the strips' fields are the layer's own across FxReset and Stopped") {
  using audio::kLayerPerf;
  ExportRig r;
  audio::Graph& g = r.engine.graph();
  r.live({mod_set(audio::kModGroupDelaySend, 2, 0, 0, kLayerPerf, -40), mod_set(audio::kModGroupReverbSend, 2, 0, 0, kLayerPerf, 25), mod_set(audio::kModGroupDelaySend, 15, 0, 0, kLayerPerf, 100),
          mod_set(audio::kModGroupDelaySend, 16, 0, 0, kLayerPerf, 100), mod_set(audio::kModGroupReverbSend, 3, audio::kFxReverbSend, 77, audio::kLayerFx2, 999)});
  CHECK(g.perf_send(2, 0) == -40);
  CHECK(g.perf_send(2, 1) == 25);
  CHECK(g.perf_send(15, 0) == 100);
  CHECK(g.perf_send(16, 0) == 0);  // not a track
  CHECK(g.perf_send(3, 1) == 0);   // a step layer's ModSet carries no layer value (value is the layer's only)
  CHECK(g.step_send(3, 1) == 77);
  CHECK(g.perf_send(2, 2) == 0);   // not a send
  r.live({plain_event(EventKind::FxReset), plain_event(EventKind::Stopped)});
  CHECK(g.perf_send(2, 0) == -40);
  CHECK(g.perf_send(2, 1) == 25);
  CHECK(g.perf_send(15, 0) == 100);
  CHECK(g.step_send(3, 1) == -1);  // FxReset: the sequencer's FX end
  // the range of the value: held to -100..100
  r.live({mod_set(audio::kModGroupDelaySend, 4, 0, 0, kLayerPerf, 5000), mod_set(audio::kModGroupReverbSend, 4, 0, 0, kLayerPerf, -5000)});
  CHECK(g.perf_send(4, 0) == 100);
  CHECK(g.perf_send(4, 1) == -100);
  r.live({mod_end(2, audio::kModGroupDelaySend)});
  CHECK(g.perf_send(2, 0) == 0);
  CHECK(g.perf_send(2, 1) == 25);  // only its own
  // a project load forgets the step FX's held send and the instrument's, not the layer's
  r.live({mod_set(audio::kModGroupDelaySend, 2, audio::kFxDelaySend, 30, audio::kLayerFx1, 0)});
  CHECK(g.step_send(2, 0) == 30);
  CHECK(r.engine.params().push(core::ParamChange{audio::master_param(audio::graph_param::master::VoiceSendsReset), 1.0f, 1u}));
  r.live({});
  CHECK(g.step_send(2, 0) == -1);
  CHECK(g.perf_send(2, 1) == 25);
}

TEST_CASE("perform layer (P3): a track the gate had shut is heard through the send the layer opens, and shut again when the layer closes it") {
  using audio::kLayerPerf;
  namespace gps = audio::graph_param::strip;
  const std::int32_t full = send_level(100, 0).delay;
  NEED(full > 3000);
  SendRig s(0, 0);
  // the track's fader is down: with no send, nothing of the strip is open, so its voice is gated (OS-ODD11)
  CHECK(s.r.engine.params().push(core::ParamChange{audio::strip_param(0, gps::Volume), 0.0f, 1u}));
  s.note();
  const SendPeaks shut = s.level();
  CHECK(shut.delay == 0);
  CHECK(s.r.p.stats().gated_blocks > 0u);
  CHECK(s.level({send_set(false, kLayerPerf, 100)}).delay == full);  // pre-fader sends: the fader at 0 does not close them
  const std::uint32_t gated = s.r.p.stats().gated_blocks;
  CHECK(s.level({mod_end(0, audio::kModGroupDelaySend)}).delay == 0);
  CHECK(s.level().delay == 0);
  CHECK(s.r.p.stats().gated_blocks > gated);  // and the voice is gated again
}

namespace {

// An export of a note on track 0 (instrument 0 with the sends given) with the layer's DELAY and REVERB held live before it, or not; and what the live render makes of the next note.
struct SendExport {
  std::vector<std::int16_t> samples;
  int delay_back = 0;
  int reverb_back = 0;
  int step_back = 0;
  SendPeaks after;
};

SendExport export_with_sends(bool layer_held, int delay, int reverb, bool step_held, unsigned inst_d, unsigned inst_r, bool sounding = false) {
  SendExport out;
  SendRig s(inst_d, inst_r);
  if (layer_held) {
    s.r.live({send_set(false, audio::kLayerPerf, delay), send_set(true, audio::kLayerPerf, reverb)});
  }
  if (step_held) {
    s.r.live({send_set(false, audio::kLayerFx1, 0, 90, true)});
  }
  if (sounding) {
    s.note();  // a note on track 0 rings (its strip carries signal) when the export takes the engine: the strip's send gains must not climb from the live levels through the export's first block
  }
  for (unsigned b = 0; b < 16u; ++b) {
    s.r.live({});  // the strips are at the layer's levels
  }
  ExportSrc src;
  s.r.enter(src);
  s.r.export_blocks(12, out.samples);
  s.r.engine.offline_leave();
  out.delay_back = s.r.engine.graph().perf_send(0, 0);
  out.reverb_back = s.r.engine.graph().perf_send(0, 1);
  out.step_back = s.r.engine.graph().step_send(0, 0);
  s.note();
  out.after = s.level();
  return out;
}

}  // namespace

TEST_CASE("perform layer (P3): an export is byte for byte what it is with Perform off, whatever DELAY and REVERB are held, and the layer is back for the next live note") {
  struct Case {
    unsigned inst_d, inst_r;
    int delay, reverb;
  };
  for (const Case c : {Case{0, 0, 100, 100}, Case{60, 20, -100, -100}, Case{30, 30, 40, -25}, Case{100, 100, -1, 0}}) {
    const SendExport off = export_with_sends(false, 0, 0, false, c.inst_d, c.inst_r);
    std::int32_t peak = 0;
    for (const std::int16_t v : off.samples) {
      peak = std::max<std::int32_t>(peak, v < 0 ? -v : v);
    }
    CHECK(peak > 1000);  // the note sounded
    const SendExport held = export_with_sends(true, c.delay, c.reverb, false, c.inst_d, c.inst_r);
    CHECK_MESSAGE(held.samples == off.samples, "instrument " << c.inst_d << " / " << c.inst_r << ", layer " << c.delay << " / " << c.reverb);
    CHECK(held.delay_back == c.delay);  // the layer is back in the strips ...
    CHECK(held.reverb_back == c.reverb);
    // ... and heard on the next note: the export's levels did not linger
    const auto clamp = [](int v) { return static_cast<unsigned>(std::max(0, std::min(100, v))); };
    const SendPeaks want = send_level(clamp(static_cast<int>(c.inst_d) + c.delay), clamp(static_cast<int>(c.inst_r) + c.reverb));
    CHECK_MESSAGE(held.after.delay == want.delay, "after: delay " << held.after.delay << " against " << want.delay);
    CHECK_MESSAGE(held.after.reverb == want.reverb, "after: reverb " << held.after.reverb << " against " << want.reverb);
  }
  // a step FX's held send does not colour the export either (the export is a fresh run), and the live sequencer's table does not send it again: it is gone after
  const SendExport off = export_with_sends(false, 0, 0, false, 10, 10);
  const SendExport step = export_with_sends(false, 0, 0, true, 10, 10);
  CHECK(step.samples == off.samples);
  CHECK(step.step_back == -1);
  // ... also while a note rings on the track whose send the step FX holds (a Stop leaves the FX on a live note; the export ends it with ramp 1, as the layer's clear does: spec 8.15, so the first block of
  // the export has the instrument's send at once, not a climb down from the FX's), and the live note after the export has the instrument's send
  const SendExport ring_off = export_with_sends(false, 0, 0, false, 10, 10, true);
  const SendExport ring_step = export_with_sends(false, 0, 0, true, 10, 10, true);
  CHECK(ring_step.samples == ring_off.samples);
  CHECK(ring_step.step_back == -1);
  CHECK(ring_step.after.delay == ring_off.after.delay);
  const SendExport ring_both = export_with_sends(true, 40, -25, true, 10, 10, true);  // the layer and the step FX together
  CHECK(ring_both.samples == ring_off.samples);
}

// ---- OS-DJ A1: the DJ crossfader's cut on a strip (graph_param::strip::XfGain) ---------------------------------------------------------------------------------------------------------------
// A cut only (0..255, 255 is the strip's own level), composed where the Perform layer's VOLUME is: the dry level and the sends. The export puts it away like the layer.

namespace {

void set_xf(ExportRig& r, unsigned track, float value, std::uint32_t ramp = 1u) {
  CHECK(r.engine.params().push(audio::ParamChange{audio::strip_param(track, audio::graph_param::strip::XfGain), value, ramp}));
}

// The loudest of each channel of a note on track 0 with the cut at `xf` (told a block before it); the strips have reached their targets by then.
Peaks xf_peaks(float xf, unsigned blocks = 40u) {
  ExportRig r;
  set_xf(r, 0, xf);
  r.live({});
  r.live({note_on_0()});
  Peaks all;
  for (unsigned b = 0; b < blocks; ++b) {
    r.live({});
    if (b >= 16u) {
      const Peaks p = peaks_of(r.out);
      all.left = std::max(all.left, p.left);
      all.right = std::max(all.right, p.right);
    }
  }
  return all;
}

// An export with the cut held on track 0 (the strip has reached it before the export starts), against one without; `after`: the loudest of the live note that follows the export.
std::vector<std::int16_t> export_with_xf(bool held, float xf, Peaks* after, int* xf_back) {
  ExportRig r;
  if (held) {
    set_xf(r, 0, xf);
    for (unsigned b = 0; b < 16u; ++b) {
      r.live({});
    }
  }
  ExportSrc src;
  r.enter(src);
  std::vector<std::int16_t> samples;
  r.export_blocks(12, samples);
  r.engine.offline_leave();
  *xf_back = r.engine.graph().xf_gain(0);
  r.live({note_on_0()});
  for (unsigned b = 0; b < 24u; ++b) {
    r.live({});
    if (b >= 12u) {
      const Peaks p = peaks_of(r.out);
      after->left = std::max(after->left, p.left);
      after->right = std::max(after->right, p.right);
    }
  }
  return samples;
}

}  // namespace

TEST_CASE("dj crossfader (A1): the strip's cut is xf / 255 of its level - 255 is the level itself, 0 gates the track - and it only cuts") {
  const Peaks ref = live_peaks({});
  NEED(ref.left > 3000);
  CHECK(xf_peaks(255.0f).left == ref.left);  // no cut: the same samples as a strip that never heard of it
  CHECK(xf_peaks(255.0f).right == ref.right);
  struct Case {
    float xf;
    double gain;
  };
  for (const Case c : {Case{0.0f, 0.0}, Case{1.0f, 1.0 / 255.0}, Case{64.0f, 64.0 / 255.0}, Case{180.0f, 180.0 / 255.0}, Case{254.0f, 254.0 / 255.0}}) {
    const Peaks p = xf_peaks(c.xf);
    CHECK_MESSAGE(std::fabs(static_cast<double>(p.left) - c.gain * ref.left) <= 0.01 * ref.left + 1.0, "xf " << c.xf << " left " << p.left << " of " << ref.left);
    CHECK_MESSAGE(std::fabs(static_cast<double>(p.right) - c.gain * ref.right) <= 0.01 * ref.right + 1.0, "xf " << c.xf << " right " << p.right << " of " << ref.right);
    CHECK_MESSAGE(p.left <= ref.left, "xf " << c.xf << " never boosts");
  }
  // out of range or not a number: refused (the rejected count rises), the strip keeps what it had
  ExportRig r;
  const std::uint32_t before = r.engine.graph().params_rejected();
  set_xf(r, 0, 256.0f);
  set_xf(r, 0, -1.0f);
  set_xf(r, 16, 100.0f);  // the input strip has none
  r.live({});
  CHECK(r.engine.graph().params_rejected() == before + 3u);
  CHECK(r.engine.graph().xf_gain(0) == 255);
}

TEST_CASE("dj crossfader (A1): the cut scales the sends with the dry level - the tracker's channel gain does too - and a THRU track beside it is untouched") {
  const SendPeaks ref = send_level(40, 40);
  NEED(ref.delay > 200);
  NEED(ref.reverb > 200);
  SendRig s(40, 40);
  set_xf(s.r, 0, 128.0f);
  s.note();
  const SendPeaks cut = s.level();
  CHECK_MESSAGE(std::fabs(static_cast<double>(cut.delay) - ref.delay * 128.0 / 255.0) <= 0.02 * ref.delay + 1.0, "delay " << cut.delay << " of " << ref.delay);
  CHECK_MESSAGE(std::fabs(static_cast<double>(cut.reverb) - ref.reverb * 128.0 / 255.0) <= 0.02 * ref.reverb + 1.0, "reverb " << cut.reverb << " of " << ref.reverb);
  SendRig z(40, 40);
  set_xf(z.r, 0, 0.0f);
  z.note();
  const SendPeaks shut = z.level();
  CHECK(shut.delay == 0);
  CHECK(shut.reverb == 0);
  // another track's cut leaves track 0 alone
  SendRig o(40, 40);
  set_xf(o.r, 1, 0.0f);
  o.note();
  const SendPeaks other = o.level();
  CHECK(other.delay == ref.delay);
  CHECK(other.reverb == ref.reverb);
  // it multiplies with the Perform layer's VOLUME rather than replacing it
  const Peaks vol = live_peaks({mod_set(audio::kModGroupVolume, 0, 0, 0, audio::kLayerPerf, -50)});
  ExportRig both;
  set_xf(both, 0, 128.0f);
  both.live({mod_set(audio::kModGroupVolume, 0, 0, 0, audio::kLayerPerf, -50)});
  both.live({note_on_0()});
  Peaks got;
  for (unsigned b = 0; b < 40u; ++b) {
    both.live({});
    if (b >= 16u) {
      const Peaks p = peaks_of(both.out);
      got.left = std::max(got.left, p.left);
    }
  }
  CHECK(std::fabs(static_cast<double>(got.left) - vol.left * 128.0 / 255.0) <= 0.015 * vol.left + 1.0);
}

TEST_CASE("dj crossfader (A1): an export is byte for byte what it is with the page at rest, whatever the cut is, and the cut is back for the next live note") {
  const Peaks ref = live_peaks({});
  Peaks off_after;
  int x0 = 0;
  const std::vector<std::int16_t> off = export_with_xf(false, 255.0f, &off_after, &x0);
  std::int32_t peak = 0;
  for (const std::int16_t s : off) {
    peak = std::max<std::int32_t>(peak, s < 0 ? -s : s);
  }
  CHECK(peak > 1000);  // the note sounded: a comparison of silence would prove nothing
  CHECK(x0 == 255);
  for (const float xf : {0.0f, 1.0f, 100.0f, 180.0f}) {
    Peaks after;
    int back = 0;
    const std::vector<std::int16_t> held = export_with_xf(true, xf, &after, &back);
    CHECK_MESSAGE(held == off, "xf " << xf);
    CHECK(back == static_cast<int>(xf));  // the cut is back in the strip ...
    const double gain = static_cast<double>(xf) / 255.0;  // ... and heard on the next note: the export's levels did not linger
    CHECK_MESSAGE(std::fabs(static_cast<double>(after.left) - gain * ref.left) <= 0.015 * ref.left + 1.0, "xf " << xf << " left " << after.left);
  }
}

// ---- OS-DJ A2: the master DJ filter in the bass slot of the master chain (graph_param::master::DjFilter) -------------------------------------------------------------------------------------
// The filter's own maths are audio_dj_filter_test.cpp's; here: the ParamChange switches the stage on, a 0 runs it down and off with the bus as at rest, and the export puts it away like the layer.

namespace {

void set_djf(ExportRig& r, float value, std::uint32_t ramp = 1u) {
  CHECK(r.engine.params().push(audio::ParamChange{audio::master_param(audio::graph_param::master::DjFilter), value, ramp}));
}

constexpr unsigned kDjfBlocks = 60;

}  // namespace

TEST_CASE("dj filter (A2): the DjFilter ParamChange switches the bass stage on, a 0 runs it down and off, and the bus is then bit for bit what it was at rest") {
  std::vector<std::int16_t> rest;
  {
    ExportRig r;
    r.live({});
    r.live({note_on_0()});
    for (unsigned b = 0; b < kDjfBlocks; ++b) {
      r.live({});
      rest.insert(rest.end(), r.out, r.out + haudio::kBlockSamples);
    }
  }
  std::vector<std::int16_t> got;
  ExportRig r;
  CHECK_FALSE(r.engine.graph().master_stage_active(audio::MasterStage::Bass));  // installed, bypassed: no cost at rest
  r.live({});
  r.live({note_on_0()});
  bool on_while_held = false;
  for (unsigned b = 0; b < kDjfBlocks; ++b) {
    if (b == 4u) {
      set_djf(r, 100.0f, 64);  // wide high-pass: the note's low body goes
    }
    if (b == 20u) {
      set_djf(r, 0.0f, 4 * 64);  // back, over four blocks
    }
    r.live({});
    got.insert(got.end(), r.out, r.out + haudio::kBlockSamples);
    if (b == 12u) {
      on_while_held = r.engine.graph().master_stage_active(audio::MasterStage::Bass);
    }
  }
  CHECK(on_while_held);
  CHECK_FALSE(r.engine.graph().master_stage_active(audio::MasterStage::Bass));
  CHECK(r.engine.graph().dj_filter() == 0.0f);
  const std::size_t blk = haudio::kBlockSamples;
  NEED(got.size() == rest.size());
  CHECK(std::equal(got.begin(), got.begin() + 4 * blk, rest.begin()));  // not told yet
  CHECK_FALSE(std::equal(got.begin() + 8 * blk, got.begin() + 18 * blk, rest.begin() + 8 * blk));  // heard
  CHECK(std::equal(got.begin() + 30 * blk, got.end(), rest.begin() + 30 * blk));  // run down and off: not one bit differs
  // out of range or not a number: refused, the filter keeps what it had
  const std::uint32_t before = r.engine.graph().params_rejected();
  set_djf(r, 100.5f);
  set_djf(r, -101.0f);
  set_djf(r, std::nanf(""));
  r.live({});
  CHECK(r.engine.graph().params_rejected() == before + 3u);
  CHECK(r.engine.graph().dj_filter() == 0.0f);
}

TEST_CASE("dj filter (A2): an export is byte for byte what it is with the page at rest, whatever the filter is, and the filter is back for the live render") {
  struct Done {
    std::vector<std::int16_t> samples;
    float back = 0.0f;
    bool on_back = false;
    bool on_during = true;
  };
  const auto run = [](bool held, float v) {
    ExportRig r;
    if (held) {
      set_djf(r, v);
      for (unsigned b = 0; b < 16u; ++b) {
        r.live({});
      }
    }
    Done d;
    ExportSrc src;
    r.enter(src);
    d.on_during = r.engine.graph().master_stage_active(audio::MasterStage::Bass);
    r.export_blocks(12, d.samples);
    r.engine.offline_leave();
    d.back = r.engine.graph().dj_filter();
    d.on_back = r.engine.graph().master_stage_active(audio::MasterStage::Bass);
    return d;
  };
  const Done off = run(false, 0.0f);
  std::int32_t peak = 0;
  for (const std::int16_t s : off.samples) {
    peak = std::max<std::int32_t>(peak, s < 0 ? -s : s);
  }
  CHECK(peak > 1000);  // the note sounded: a comparison of silence would prove nothing
  CHECK_FALSE(off.on_back);
  for (const float v : {-100.0f, -40.0f, 30.0f, 100.0f}) {
    const Done held = run(true, v);
    CHECK_MESSAGE(held.samples == off.samples, "filter " << v);
    CHECK_FALSE(held.on_during);
    CHECK(held.back == v);  // back in the graph ...
    CHECK(held.on_back);    // ... and its stage on again for the live render
  }
}

// ---- OS-DJ A3: the master kill EQ in the eq slot of the master chain (graph_param::master::KillEq) ----------------------------------------------------------------------------------------------
// The stage's own maths are audio_kill_eq_test.cpp's; here: the ParamChange switches the stage on, a 0 runs it down and off with the bus as at rest, and the export puts it away like the layer.

namespace {

void set_kill(ExportRig& r, float value) {
  CHECK(r.engine.params().push(audio::ParamChange{audio::master_param(audio::graph_param::master::KillEq), value, 1u}));
}

}  // namespace

TEST_CASE("kill eq (A3): the KillEq ParamChange switches the eq stage on, a 0 runs it down and off, and the bus is then bit for bit what it was at rest") {
  std::vector<std::int16_t> rest;
  {
    ExportRig r;
    r.live({});
    r.live({note_on_0()});
    for (unsigned b = 0; b < kDjfBlocks; ++b) {
      r.live({});
      rest.insert(rest.end(), r.out, r.out + haudio::kBlockSamples);
    }
  }
  std::vector<std::int16_t> got;
  ExportRig r;
  CHECK_FALSE(r.engine.graph().master_stage_active(audio::MasterStage::Eq));  // installed, bypassed: no cost at rest
  r.live({});
  r.live({note_on_0()});
  bool on_while_held = false;
  for (unsigned b = 0; b < kDjfBlocks; ++b) {
    if (b == 4u) {
      set_kill(r, 7.0f);  // everything killed: the note goes
    }
    if (b == 20u) {
      set_kill(r, 0.0f);
    }
    r.live({});
    got.insert(got.end(), r.out, r.out + haudio::kBlockSamples);
    if (b == 12u) {
      on_while_held = r.engine.graph().master_stage_active(audio::MasterStage::Eq);
    }
  }
  CHECK(on_while_held);
  CHECK_FALSE(r.engine.graph().master_stage_active(audio::MasterStage::Eq));
  CHECK(r.engine.graph().kill_eq() == 0u);
  const std::size_t blk = haudio::kBlockSamples;
  NEED(got.size() == rest.size());
  CHECK(std::equal(got.begin(), got.begin() + 4 * blk, rest.begin()));  // not told yet
  CHECK_FALSE(std::equal(got.begin() + 8 * blk, got.begin() + 18 * blk, rest.begin() + 8 * blk));  // heard
  CHECK(std::all_of(got.begin() + 10 * blk, got.begin() + 18 * blk, [](std::int16_t s) { return s == 0; }));  // (all three bands killed: silence)
  CHECK(std::equal(got.begin() + 30 * blk, got.end(), rest.begin() + 30 * blk));  // run down and off: not one bit differs
  // not a mask: refused, the stage keeps what it had
  const std::uint32_t before = r.engine.graph().params_rejected();
  set_kill(r, 8.0f);
  set_kill(r, -1.0f);
  set_kill(r, 1.5f);
  set_kill(r, std::nanf(""));
  r.live({});
  CHECK(r.engine.graph().params_rejected() == before + 4u);
  CHECK(r.engine.graph().kill_eq() == 0u);
}

TEST_CASE("kill eq (A3): an export is byte for byte what it is with the page at rest, whatever is killed, and the kill is back for the live render") {
  struct Done {
    std::vector<std::int16_t> samples;
    unsigned back = 0;
    bool on_back = false;
    bool on_during = true;
  };
  const auto run = [](bool held, unsigned mask) {
    ExportRig r;
    if (held) {
      set_kill(r, static_cast<float>(mask));
      for (unsigned b = 0; b < 16u; ++b) {
        r.live({});
      }
    }
    Done d;
    ExportSrc src;
    r.enter(src);
    d.on_during = r.engine.graph().master_stage_active(audio::MasterStage::Eq);
    r.export_blocks(12, d.samples);
    r.engine.offline_leave();
    d.back = r.engine.graph().kill_eq();
    d.on_back = r.engine.graph().master_stage_active(audio::MasterStage::Eq);
    return d;
  };
  const Done off = run(false, 0u);
  CHECK_FALSE(off.on_back);
  for (const unsigned mask : {1u, 2u, 4u, 5u, 7u}) {
    const Done held = run(true, mask);
    CHECK_MESSAGE(held.samples == off.samples, "mask " << mask);
    CHECK_FALSE(held.on_during);
    CHECK(held.back == mask);  // back in the graph ...
    CHECK(held.on_back);       // ... and its stage on again for the live render
  }
}

// OS-DJ C1: the ScratchMode ParamChange engages the `headroom` slot's scratch stage when a ring is staged; the export puts it away at once (it carries no scratch) and lets go of the ring.
TEST_CASE("scratch export (C1): ScratchMode engages the stage, the export is byte for byte what it is without a scratch, and the ring is let go") {
  static std::int16_t ring[2 * 8820];
  struct Done {
    std::vector<std::int16_t> samples;
    bool on_live = false, on_during = false, busy_after = true, idle_after = false;
  };
  auto run = [&](bool scratch) {
    Done d;
    ExportRig r;
    r.live({});
    r.live({note_on_0()});
    if (scratch) {
      CHECK(r.engine.graph().scratch().stage(ring, 8820));
      CHECK(r.engine.params().push(audio::ParamChange{audio::master_param(audio::graph_param::master::ScratchMode), 1.0f, 0u}));
    }
    for (unsigned b = 0; b < 8u; ++b) {
      r.live({});
    }
    d.on_live = r.engine.graph().master_stage_active(audio::MasterStage::Headroom);
    ExportSrc src;
    r.enter(src);
    d.on_during = r.engine.graph().master_stage_active(audio::MasterStage::Headroom);
    r.export_blocks(12, d.samples);
    r.engine.offline_leave();
    d.busy_after = r.engine.graph().scratch().busy();
    d.idle_after = r.engine.graph().scratch().idle();
    return d;
  };
  const Done off = run(false);
  const Done on = run(true);
  CHECK_FALSE(off.on_live);
  CHECK(on.on_live);
  CHECK_FALSE(on.on_during);
  CHECK(on.samples == off.samples);
  CHECK_FALSE(on.busy_after);  // the app may give the arena's tail back
  CHECK(on.idle_after);
}
