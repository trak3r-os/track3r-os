// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-ODD11 slices 1 and 2, the voice gate (voices.h GATE): a voice on an inaudible strip skips the tremolo, the filter and the shaper and is not fed to its track; the graph says which strips are
// inaudible (Graph::quiet_tracks), from the smoothers and not the mute flag; the voice comes back through a 32-sample ramp and a cold filter; an export is never gated.
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os::audio;
namespace gps = o3os::audio::graph_param::strip;
namespace gpm = o3os::audio::graph_param::master;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr std::uint32_t kLen = 3000;

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 7u; }

std::int16_t g_wave[kLen];

void init_wave() {
  for (std::uint32_t i = 0; i < kLen; ++i) {
    g_wave[i] = static_cast<std::int16_t>(std::lround(16000.0 * std::sin(6.283185307 * 7.0 * static_cast<double>(i) / kLen) + 6000.0 * std::sin(0.37 * static_cast<double>(i))));
  }
}

// A sustained looped note: the whole sample loops forward.
SampleDesc loop_desc(bool filter = false) {
  SampleDesc d{g_wave, kLen, 0, kLen, 65536};
  d.filter.enable = filter;
  d.filter.wanted = filter;
  d.filter.type = kFilterLow;
  d.filter.cutoff = 0.3f;
  d.filter.resonance = 0.4f;
  return d;
}

// ---- the pool alone ----------------------------------------------------------------------------------------------------------

o3os::test::StaticSlot<VoicePool> slot_x;
o3os::test::StaticSlot<VoicePool> slot_y;

struct Block {
  std::int16_t s[kBlk];
  bool fed;
};

Block pool_block(VoicePool& p, std::uint32_t gate) {
  Block b{};
  p.set_gate(gate);
  p.begin_block();
  p.render(0, kBlk);
  p.end_block();
  const std::int16_t* f = p.feed(0);
  b.fed = f != nullptr;
  if (f != nullptr) {
    std::memcpy(b.s, f, sizeof b.s);
  }
  return b;
}

std::int32_t max_step(const Block& b, std::int32_t prev) {
  std::int32_t m = 0;
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    m = std::max<std::int32_t>(m, std::abs(static_cast<std::int32_t>(b.s[i]) - prev));
    prev = b.s[i];
  }
  return m;
}

}  // namespace

TEST_CASE("voice gate: a gated voice is not fed, keeps its note, and comes back on the reference trajectory after a 32-sample ramp") {
  init_wave();
  VoicePool& x = slot_x.make();
  VoicePool& y = slot_y.make();
  x.set_sample(0, loop_desc());
  y.set_sample(0, loop_desc());
  NEED(x.note_on(0, 60, 0, 127));
  NEED(y.note_on(0, 60, 0, 127));
  for (int b = 0; b < 2; ++b) {  // the attack ramp is over
    (void)pool_block(x, 0);
    (void)pool_block(y, 0);
  }
  for (int b = 0; b < 5; ++b) {
    const Block ref = pool_block(x, 0);
    const Block g = pool_block(y, 1u);
    CHECK(ref.fed);
    CHECK_FALSE(g.fed);
  }
  CHECK(y.stats().gated_blocks == 5u);
  CHECK(x.stats().gated_blocks == 0u);
  CHECK(y.voices_in_use() == 1u);  // a gated voice still counts (diag voices is unchanged)
  const Block ref = pool_block(x, 0);
  const Block back = pool_block(y, 0);
  NEED(back.fed);
  for (std::uint32_t j = 0; j < kBlk; ++j) {
    const double want = j < 32u ? static_cast<double>(ref.s[j]) * static_cast<double>(j + 1u) / 32.0 : static_cast<double>(ref.s[j]);
    CHECK_MESSAGE(std::fabs(static_cast<double>(back.s[j]) - want) <= 1.0, "sample " << j << ": " << back.s[j] << " against " << want);
  }
  CHECK(y.stats().gated_blocks == 5u);
  const Block next_ref = pool_block(x, 0);
  const Block next = pool_block(y, 0);
  CHECK(std::memcmp(next_ref.s, next.s, sizeof next.s) == 0);  // the ramp is over: bit for bit the reference
}

TEST_CASE("voice gate: a filtered voice re-enters cold with no step larger than the filtered reference makes") {
  init_wave();
  VoicePool& x = slot_x.make();
  VoicePool& y = slot_y.make();
  x.set_sample(0, loop_desc(true));
  y.set_sample(0, loop_desc(true));
  NEED(x.note_on(0, 60, 0, 127));
  NEED(y.note_on(0, 60, 0, 127));
  Block last_x{};
  Block last_y{};
  for (int b = 0; b < 8; ++b) {
    last_x = pool_block(x, 0);
    last_y = pool_block(y, 0);
  }
  CHECK(std::memcmp(last_x.s, last_y.s, sizeof last_x.s) == 0);
  for (int b = 0; b < 9; ++b) {
    last_x = pool_block(x, 0);
    last_y = pool_block(y, 1u);
  }
  CHECK_FALSE(last_y.fed);
  const Block ref = pool_block(x, 0);
  const Block back = pool_block(y, 0);
  NEED(back.fed);
  // The cold filter starts from rest under the ramp: the largest step stays within what the reference makes in steady state (a bound with a margin of a quarter).
  const std::int32_t ref_step = max_step(ref, last_x.s[kBlk - 1]);
  const std::int32_t back_step = max_step(back, 0);
  CHECK_MESSAGE(back_step <= ref_step + ref_step / 4, "re-entry step " << back_step << " against the reference's " << ref_step);
}

// OS-PERFFX P0: the Perform layer arrives while the track is inaudible (a muted track is still under Perform). The filter is on the voice when the gate lifts, as it would be on a voice that was heard
// all along, and the layer's values in the pool are untouched by the gate.
TEST_CASE("voice gate: a performance filter set while the voice is gated is on it when the gate lifts, and re-enters cold under the reference's steps; the layer's slots stand") {
  init_wave();
  VoicePool& x = slot_x.make();
  VoicePool& y = slot_y.make();
  x.set_sample(0, loop_desc());  // an instrument with no filter of its own
  y.set_sample(0, loop_desc());
  NEED(x.note_on(0, 60, 0, 127));
  NEED(y.note_on(0, 60, 0, 127));
  Block last_x{};
  Block last_y{};
  for (int b = 0; b < 4; ++b) {
    last_x = pool_block(x, 0);
    last_y = pool_block(y, 0);
  }
  CHECK(std::memcmp(last_x.s, last_y.s, sizeof last_x.s) == 0);
  SeqEvent layer{};
  layer.kind = EventKind::ModSet;
  layer.track = 0;
  layer.c = kLayerPerf;
  layer.flags = kModGroupOverdrive;
  layer.value = 55;
  for (int b = 0; b < 9; ++b) {
    if (b == 2) {  // the layer arrives in the third gated block, on the gated voice and on the one that is heard
      x.set_filter_perf(0, kFilterLow, 30);
      y.set_filter_perf(0, kFilterLow, 30);
      x.fx_event(0, layer);
      y.fx_event(0, layer);
    }
    last_x = pool_block(x, 0);
    last_y = pool_block(y, 1u);
  }
  CHECK_FALSE(last_y.fed);
  CHECK(y.stats().gated_blocks == 9u);
  CHECK(y.perf_held(0, kModGroupOverdrive));
  CHECK(y.perf_value(0, kModGroupOverdrive) == 55);
  const Block ref = pool_block(x, 0);
  const Block back = pool_block(y, 0);
  NEED(back.fed);
  CHECK(std::fabs(x.filter_cutoff(0) - 0.60f) < 1.0e-5f);  // the instrument's cutoff 0.3 (its filter is off, the descriptor keeps the number) plus the layer's 30 / 100
  CHECK(std::fabs(y.filter_cutoff(0) - 0.60f) < 1.0e-5f);  // ... on the voice that was gated when the layer came, as on the one that was heard
  const std::int32_t ref_step = max_step(ref, last_x.s[kBlk - 1]);
  const std::int32_t back_step = max_step(back, 0);
  CHECK_MESSAGE(back_step <= ref_step + ref_step / 4, "re-entry step " << back_step << " against the reference's " << ref_step);
  CHECK(y.perf_held(0, kModGroupOverdrive));
  y.clear_filter_perf(0);  // the layer lets go: the voice's own (no) filter
  (void)pool_block(y, 0);
  CHECK(y.filter_cutoff(0) == -1.0f);
}

TEST_CASE("voice gate: a note that starts after a gated one owes no ramp, and a gate with no voice counts nothing") {
  init_wave();
  VoicePool& x = slot_x.make();
  VoicePool& y = slot_y.make();
  x.set_sample(0, loop_desc());
  y.set_sample(0, loop_desc());
  (void)pool_block(x, 1u);
  CHECK(x.stats().gated_blocks == 0u);  // nothing sounds: nothing is counted
  NEED(x.note_on(0, 60, 0, 127));
  (void)pool_block(x, 1u);
  (void)pool_block(x, 1u);
  CHECK(x.stats().gated_blocks == 2u);
  x.cut(0);
  for (int b = 0; b < 3; ++b) {  // the residual of the cut fades, rendered whole
    const Block f = pool_block(x, 1u);
    (void)f;
  }
  CHECK(x.stats().gated_blocks == 2u);
  NEED(x.note_on(0, 62, 0, 127));
  NEED(y.note_on(0, 62, 0, 127));
  const Block a = pool_block(x, 0);
  const Block b = pool_block(y, 0);
  NEED(a.fed);
  CHECK(std::memcmp(a.s, b.s, sizeof a.s) == 0);  // the plain attack of a note, as a pool that never gated plays it
}

namespace {

// A block with a note of track `track` taken at `at` (the voice reused in the middle of the block, as an event does).
Block pool_block_note(VoicePool& p, std::uint32_t gate, std::uint32_t at, unsigned track, unsigned slot) {
  Block b{};
  p.set_gate(gate);
  p.begin_block();
  p.render(0, at);
  (void)p.note_on(track, 60, slot, 127);
  p.render(at, kBlk);
  p.end_block();
  const std::int16_t* f = p.feed(track);
  b.fed = f != nullptr;
  if (f != nullptr) {
    std::memcpy(b.s, f, sizeof b.s);
  }
  return b;
}

double rms_of(const Block& b) {
  double e = 0.0;
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    e += static_cast<double>(b.s[i]) * static_cast<double>(b.s[i]);
  }
  return std::sqrt(e / kBlk);
}

}  // namespace

TEST_CASE("voice gate: a cutoff ADSR gated for longer than 16 bits of samples stands where the ungated voice's does") {
  init_wave();
  VoicePool& x = slot_x.make();
  VoicePool& y = slot_y.make();
  SampleDesc d = loop_desc(true);
  d.filter.cutoff = 0.0f;
  d.filter.resonance = 0.0f;
  d.filter.mod.kind = kFmodEnv;
  d.filter.mod.amount = 1.0f;
  d.filter.mod.sustain = 1.0f;
  d.filter.mod.attack = 2000;  // ms: the cutoff opens over 88,000 samples
  x.set_sample(0, d);
  y.set_sample(0, d);
  NEED(x.note_on(0, 60, 0, 127));
  NEED(y.note_on(0, 60, 0, 127));
  constexpr int kGated = 1300;  // 83,200 samples: past 65,536 (a 16-bit clock wraps to a fifth of the attack) and still inside the attack
  for (int b = 0; b < kGated; ++b) {
    (void)pool_block(x, 0);
    (void)pool_block(y, 1u);
  }
  Block ref{};
  Block back{};
  for (int b = 0; b < 12; ++b) {  // the cold filter has forgotten its start
    ref = pool_block(x, 0);
    back = pool_block(y, 0);
  }
  NEED(back.fed);
  const double want = rms_of(ref);
  CHECK(want > 100.0);
  CHECK_MESSAGE(std::fabs(rms_of(back) - want) <= 0.03 * want, "the unmuted voice's level " << rms_of(back) << " against the reference's " << want);
}

TEST_CASE("voice gate: a voice reused in the middle of a block takes the new track's gate bit (its first block is fed and ramped)") {
  init_wave();
  VoicePool& x = slot_x.make();
  VoicePool& y = slot_y.make();
  const SampleDesc shot{g_wave, 10, 0, 0, 65536};  // track 0's one shot ends in the block's first span
  for (VoicePool* p : {&x, &y}) {
    p->set_sample(0, shot);
    p->set_sample(1, loop_desc());
    for (unsigned t = 1; t < kMaxVoices; ++t) {
      NEED(p->note_on(t, 60, 1, 127));  // the other tracks fill the pool (kMaxVoices = 12): the next note takes the voice that ended
    }
  }
  for (int b = 0; b < 3; ++b) {
    (void)pool_block(x, 0);
    (void)pool_block(y, 0);
  }
  NEED(x.note_on(0, 60, 0, 127));
  NEED(y.note_on(0, 60, 0, 127));
  const unsigned fresh = 13;  // a track with no voice
  const Block got = pool_block_note(x, 1u, 17, fresh, 1);  // track 0 gated: its voice ends in span 1 and track 13's note takes it at sample 17
  const Block ref = pool_block_note(y, 0, 17, fresh, 1);   // the same with no gate
  NEED(got.fed);
  NEED(ref.fed);
  CHECK(x.stats().steals == 0u);
  for (std::uint32_t j = 17; j < kBlk; ++j) {
    CHECK_MESSAGE(got.s[j] == ref.s[j], "sample " << j << ": " << got.s[j] << " against " << ref.s[j]);
  }
  CHECK(std::abs(static_cast<int>(got.s[kBlk - 1])) > 0);
}

// ---- the engine ---------------------------------------------------------------------------------------------------------------

namespace {

// A source with one note on track 0 at block `note_block` (a block counted on the source's own clock), sustained.
struct Src {
  std::uint32_t note_block = 1;
  std::uint32_t calls = 0;
};

std::uint32_t src_step(void* self, std::uint64_t block_start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
  Src& s = *static_cast<Src*>(self);
  ++s.calls;
  if (cap == 0u || static_cast<std::uint32_t>(block_start / n) != s.note_block) {
    return 0;
  }
  SeqEvent e{};
  e.offset = 17;
  e.kind = EventKind::NoteOn;
  e.track = 0;
  e.a = 60;
  e.b = 0;
  e.c = 127;
  out[0] = e;
  return 1;
}

struct Rig {
  Rig() : engine(&fake_cycles, taps) {
    init_wave();
    pool.set_sample(0, loop_desc());
    engine.attach_voices(&pool);
    engine.attach_live(&live_box);
  }
  EventSource source_of(Src& s) {
    EventSource e;
    e.self = &s;
    e.step = &src_step;
    e.buffer = events;
    e.capacity = kMaxEventsPerBlock;
    return e;
  }
  void param(unsigned slot, unsigned field, float v, std::uint32_t ramp = 1) {
    CHECK(engine.graph().params().push(o3os::core::ParamChange{strip_param(slot, field), v, ramp}));
  }
  void master(unsigned field, float v) { CHECK(engine.graph().params().push(o3os::core::ParamChange{master_param(field), v, 1u})); }
  void live_block(std::vector<std::int16_t>* into = nullptr) {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    const haudio::BlockCallback cb = engine.callback();
    cb.fn(cb.user, io);
    if (into != nullptr) {
      into->insert(into->end(), out, out + haudio::kBlockSamples);
    }
  }
  void offline_block(std::vector<std::int16_t>* into) {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine.render_offline(io);
    into->insert(into->end(), out, out + haudio::kBlockSamples);
  }
  bool quiet(unsigned t) const { return ((engine.graph().quiet_tracks() >> t) & 1u) != 0u; }

  TapMem taps;
  Engine engine;
  VoicePool pool;
  LiveBox live_box;
  SeqEvent events[kMaxEventsPerBlock];
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  haudio::BlockReport report;
};

Rig& make_rig() {
  static o3os::test::StaticSlot<Rig> slot;
  return slot.make();
}

std::int32_t peak_of(const std::vector<std::int16_t>& v, std::size_t from = 0) {
  std::int32_t p = 0;
  for (std::size_t i = from; i < v.size(); ++i) {
    p = std::max<std::int32_t>(p, std::abs(static_cast<std::int32_t>(v[i])));
  }
  return p;
}

}  // namespace

TEST_CASE("voice gate: the quiet mask is the smoothers' (level and both sends closed), not the mute flag") {
  Rig& r = make_rig();
  Src src;
  r.engine.attach_events(r.source_of(src));
  r.live_block();
  r.live_block();
  CHECK_FALSE(r.quiet(0));
  CHECK_FALSE(r.quiet(11));
  CHECK_FALSE(r.quiet(12));  // an inactive strip is not named
  CHECK_FALSE(r.quiet(15));  // nor is the audition strip, whatever it carries

  // A mute: closed once the block that ramped it has gone by.
  r.param(0, gps::Mute, 1.0f);
  r.live_block();
  CHECK(r.quiet(0));
  CHECK_FALSE(r.quiet(1));
  r.param(0, gps::Mute, 0.0f);
  r.live_block();
  CHECK_FALSE(r.quiet(0));

  // A fader that ramps shut over four blocks is not quiet until it has arrived (the strip carries the sustained note: a strip with no feed snaps to its target).
  r.param(0, gps::Volume, 0.0f, 4u * kBlk);
  r.live_block();
  CHECK_FALSE(r.quiet(0));
  r.live_block();
  r.live_block();
  CHECK_FALSE(r.quiet(0));
  r.live_block();
  r.live_block();
  CHECK(r.quiet(0));
  r.param(0, gps::Volume, 100.0f);
  r.live_block();
  r.live_block();
  CHECK_FALSE(r.quiet(0));
  // Track 1 has no feed: its faders snap, and a fader at 0 is quiet.
  r.param(1, gps::Volume, 0.0f);
  r.live_block();
  CHECK(r.quiet(1));
  // ... but a pre-fader send that is open keeps it audible, closed again it is quiet.
  r.param(1, gps::Route, static_cast<float>(route::kMain | route::kSndA));
  r.param(1, gps::SendA, 50.0f);
  r.live_block();
  r.live_block();
  CHECK_FALSE(r.quiet(1));
  r.param(1, gps::SendA, 0.0f);
  r.live_block();
  r.live_block();
  CHECK(r.quiet(1));
  r.param(1, gps::Volume, 100.0f);
  r.param(1, gps::Route, static_cast<float>(route::kMain));
  r.live_block();
  r.live_block();
  CHECK_FALSE(r.quiet(1));

  // A solo elsewhere: every other track is quiet; the soloed one is not. With the sends left open by the export's half it still is not quiet when a send is open.
  r.param(2, gps::Solo, 1.0f);
  r.live_block();
  r.live_block();
  CHECK_FALSE(r.quiet(2));
  CHECK(r.quiet(0));
  CHECK(r.quiet(1));
  CHECK(r.quiet(11));
  r.param(2, gps::Solo, 0.0f);
  r.live_block();
  r.live_block();
  CHECK_FALSE(r.quiet(0));

  // The master DRY MUTE closes every fader; the sends decide.
  r.param(3, gps::Route, static_cast<float>(route::kMain | route::kSndB));
  r.param(3, gps::SendB, 40.0f);
  r.master(gpm::DryMute, 1.0f);
  r.live_block();
  r.live_block();
  CHECK(r.quiet(0));
  CHECK_FALSE(r.quiet(3));
  r.master(gpm::DryMute, 0.0f);
  r.live_block();
  r.live_block();
  CHECK_FALSE(r.quiet(0));
}

TEST_CASE("voice gate: a muted track keeps its note and its position, is unmuted mid-note one block late, with no click") {
  Rig& r = make_rig();
  Src src;
  src.note_block = 3;
  r.engine.attach_events(r.source_of(src));
  r.param(0, gps::Mute, 1.0f);
  std::vector<std::int16_t> out;
  for (int b = 0; b < 12; ++b) {
    r.live_block(&out);
  }
  CHECK(r.pool.stats().note_ons == 1u);
  CHECK(r.pool.track_playing(0));  // the muted track made its note
  CHECK(r.pool.stats().gated_blocks >= 6u);
  CHECK(r.pool.stats().advanced_spans >= 6u);  // OS-ODD11 slice 2: its plain looped voice was advanced in closed form, not rendered (tests/audio_advance_test.cpp holds the state to the rendered one's)
  CHECK(peak_of(out) == 0);  // ... and nobody heard it
  const std::uint32_t before = r.pool.stats().gated_blocks;
  out.clear();
  r.param(0, gps::Mute, 0.0f);
  for (int b = 0; b < 6; ++b) {
    r.live_block(&out);
  }
  CHECK(r.pool.stats().gated_blocks == before + 1u);  // the block of the unmute drain still ran gated: one block late (Q11)
  const std::vector<std::int16_t> first_block(out.begin(), out.begin() + haudio::kBlockSamples);
  CHECK(peak_of(first_block) == 0);
  const std::int32_t peak = peak_of(out);
  CHECK_MESSAGE(peak > 500, "the unmuted track is silent: " << peak);
  // No click: no step across the whole run is near the signal's own size (the 32-sample ramp's slope is a thirty-second of it).
  std::int32_t step = 0;
  for (std::size_t i = 2; i < out.size(); ++i) {
    step = std::max<std::int32_t>(step, std::abs(static_cast<std::int32_t>(out[i]) - static_cast<std::int32_t>(out[i - 2])));
  }
  CHECK_MESSAGE(step < peak / 3, "click: step " << step << " of peak " << peak);
}

// The export: the stems pass hears the project's muted track (it unmutes it itself), whatever the live gate said.
namespace {

std::vector<std::int16_t> export_run(bool gate_off, std::uint32_t& gated_live, std::uint32_t& gated_after) {
  Rig& r = make_rig();
  Src live_src;
  live_src.note_block = 2;
  r.engine.attach_events(r.source_of(live_src));
  if (gate_off) {
    r.engine.set_voice_gate_mask(0);
  }
  r.param(0, gps::Mute, 1.0f);  // the project's muted track
  for (int b = 0; b < 10; ++b) {
    r.live_block();
  }
  gated_live = r.pool.stats().gated_blocks;
  r.pool.cut(0);  // the transport stops before the export: no voice is left, and the strip stays closed
  for (int b = 0; b < 4; ++b) {
    r.live_block();
  }
  CHECK_FALSE(r.pool.track_playing(0));
  CHECK((r.engine.graph().quiet_tracks() & 1u) == 1u);
  r.engine.offline_request();
  CHECK(r.engine.offline_ready(false));
  Src export_src;
  export_src.note_block = 0;  // the first block of the stem has the note: the stale live gate would take it
  r.engine.offline_enter(r.source_of(export_src));
  r.param(0, gps::Mute, 0.0f);  // the stem pass unmutes the masked-in track (push_mixer)
  std::vector<std::int16_t> off;
  for (int b = 0; b < 14; ++b) {
    r.offline_block(&off);
  }
  // The export ends with the strip closed (the stems pass mutes the others, and this one at its end), the voice gone and the project's value pushed back for the live render to take.
  r.param(0, gps::Mute, 1.0f);
  std::vector<std::int16_t> tail;
  r.pool.cut(0);
  for (int b = 0; b < 3; ++b) {
    r.offline_block(&tail);
  }
  CHECK(r.quiet(0));  // the graph says track 0 is quiet when the export ends
  r.param(0, gps::Mute, 0.0f);
  r.engine.offline_leave();
  // The first live block after the export: a note started on the muted track must not be gated by the export's last state.
  Src after;
  after.note_block = static_cast<std::uint32_t>(r.engine.sample_clock() / kBlk);
  r.engine.attach_events(r.source_of(after));
  const std::uint32_t g0 = r.pool.stats().gated_blocks;
  std::vector<std::int16_t> first;
  r.live_block(&first);
  gated_after = r.pool.stats().gated_blocks - g0;
  CHECK(peak_of(first) > 0);  // the note of the first live block is heard (the unmute drains in this block, after the voices)
  return off;
}

}  // namespace

TEST_CASE("voice gate: an export is byte for byte what it is without the gate (the project's muted track keeps its stem)") {
  std::uint32_t live_on = 0;
  std::uint32_t after_on = 0;
  std::uint32_t live_off = 0;
  std::uint32_t after_off = 0;
  const std::vector<std::int16_t> with_gate = export_run(false, live_on, after_on);
  const std::vector<std::int16_t> without = export_run(true, live_off, after_off);
  CHECK(live_on >= 4u);      // the gate was engaged on the live side before the export ...
  CHECK(live_off == 0u);     // ... and held off in the reference
  CHECK(after_on == 0u);     // the first live block after the export renders whole
  CHECK(after_off == 0u);
  CHECK(with_gate.size() == without.size());
  CHECK(std::memcmp(with_gate.data(), without.data(), with_gate.size() * sizeof(std::int16_t)) == 0);
  // The stem is not silence, and its first note block (block 0 of the export, the strip just unmuted) is in it.
  CHECK(peak_of(with_gate) > 1000);
  CHECK(peak_of(with_gate, 2u * haudio::kBlockSamples) > 1000);
}

// OS-ODD11 slice 2 (QA-MIX-024): an export that starts while a MUTED track's plain voice sounds. Engine::offline_quiet() cuts the voice; a rendered voice leaves its last sample as a residual, the voice
// that was advanced in closed form put nothing out and leaves none. STATUS latent (aa): Engine::offline_enter drops that tail (VoicePool::drop_tails), so a live voice's tail is in no stem and the two
// stems are the same from the first sample. (tests/audio_advance_test.cpp pins the pool's own cut: the residual is there until the engine drops it.)
namespace {

std::vector<std::int16_t> export_over_sounding_voice(bool gate_off, bool& was_sounding, std::uint32_t& advanced) {
  Rig& r = make_rig();
  Src live_src;
  live_src.note_block = 2;
  r.engine.attach_events(r.source_of(live_src));
  if (gate_off) {
    r.engine.set_voice_gate_mask(0);
  }
  r.param(0, gps::Mute, 1.0f);  // the project's muted track, its note sounding
  for (int b = 0; b < 10; ++b) {
    r.live_block();
  }
  was_sounding = r.pool.track_playing(0);
  advanced = r.pool.stats().advanced_spans;
  r.engine.offline_request();
  CHECK(r.engine.offline_ready(false));
  Src export_src;
  export_src.note_block = 1000;  // no note in the stem's first blocks: what is heard is what offline_quiet leaves
  r.engine.offline_enter(r.source_of(export_src));
  r.param(0, gps::Mute, 0.0f);  // the stem pass unmutes the track
  std::vector<std::int16_t> off;
  for (int b = 0; b < 6; ++b) {
    r.offline_block(&off);
  }
  r.engine.offline_leave();
  return off;
}

}  // namespace

TEST_CASE("voice gate: an export that starts over a sounding muted voice has no fade-out of it in the stem's first block, gated or not (OS-ODD11 slice 2, latent aa)") {
  bool sounding_on = false;
  bool sounding_off = false;
  std::uint32_t advanced_on = 0;
  std::uint32_t advanced_off = 0;
  const std::vector<std::int16_t> with_gate = export_over_sounding_voice(false, sounding_on, advanced_on);
  const std::vector<std::int16_t> baseline = export_over_sounding_voice(true, sounding_off, advanced_off);
  CHECK(sounding_on);
  CHECK(sounding_off);
  CHECK(advanced_on > 0u);  // the muted voice was advanced in closed form on the live side ...
  CHECK(advanced_off == 0u);  // ... and never in the baseline
  NEED(with_gate.size() == baseline.size());
  CHECK_MESSAGE(peak_of(baseline) == 0, "the baseline's stem holds no fade-out of the cut voice");
  CHECK_MESSAGE(peak_of(with_gate) == 0, "the advanced voice leaves none either");
  CHECK(std::memcmp(with_gate.data(), baseline.data(), with_gate.size() * sizeof(std::int16_t)) == 0);
}
