// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-ABI2 A6 (docs/specs/OS-INSFX-ABI2.md 3.5, `key:` cases): the sidechain key of the insert host (audio/insert_host.h, audio/src/insert_key.cpp, module/key_state.h), driven block by block on the real
// Graph with a native ducker module written here that reads host->track_level. Host only.
//   * which block a level is from: a key on a lower track is this block's, on a higher one the previous block's, on the master insert this block's; a silent key reads 0;
//   * who measures: the host's tap in an empty slot B, the module in slot B, the tap over a bypassed module, the tap goes back when the module leaves; the tap is cleared when no key names the track;
//   * a KEY parameter changed by ParamChange moves the key (and the tap); a module of ABI 1.11 never has the flag honoured;
//   * a muted key track still keys: its voices are not gated while it is a key (Engine::block_end), and are when it is not;
//   * an export starts with no key level of the live render, and two exports give equal bits; no allocation in the render.
#include <doctest/doctest.h>

#include "support/static_slot.h"

#include <o3os/audio/engine.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/insert_host.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/taps.h>
#include <o3os/audio/voices.h>
#include <o3os/core/param.h>
#include <o3os/hal/audio.h>
#include <o3os/module/key_state.h>

#include <cmath>
#include <cstdint>
#include <cstring>

// The counting allocator of tests/audio_insert_test.cpp (the same binary): its window and its count.
namespace insert_alloc {
extern bool window;
extern unsigned count;
}  // namespace insert_alloc

namespace {

using namespace o3os;
using audio::Graph;
using audio::InsertHost;
using audio::ModuleVoices;
namespace ha = o3os::hal::audio;
namespace gps = o3os::audio::graph_param::strip;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;
constexpr float kQ = 1.0f / 32768.0f;

// ---- the ducker: gain from the Gain parameter, reads the level of its Key track every block ------------------------------------------
struct alignas(16) KState {
  const o3m_host* host;
  float gain;
  std::uint32_t key;  // 0 none, 1..16
  float seen;         // the level the last render read (-1: it never read one)
  std::uint32_t renders;
};
int k_init(void* s, const o3m_host* h) {
  KState* d = static_cast<KState*>(s);
  d->host = h;
  d->gain = 1.0f;
  d->key = 0;
  d->seen = -1.0f;
  return 0;
}
void k_shutdown(void*) {}
void k_set(void* s, std::uint32_t id, float v) {
  KState* d = static_cast<KState*>(s);
  if (id == 0u) {
    d->gain = v;
  } else if (id == 1u) {
    d->key = static_cast<std::uint32_t>(v + 0.5f);
  }
}
std::uint32_t k_render(void* s, const float* in, float* out, std::uint32_t n) {
  KState* d = static_cast<KState*>(s);
  ++d->renders;
  if (d->key != 0u && O3M_HOST_HAS(d->host, track_level)) {
    d->seen = d->host->track_level(d->key - 1u);
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = in[i] * d->gain;
  }
  return 1;
}
const o3m_param kParams[2] = {{"Gain", 0.0f, 4.0f, 1.0f, 0}, {"Key", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY}};
#define DUCKER(name, minor) \
  o3m_module { O3M_ABI_MAJOR, minor, O3M_KIND_EFFECT, sizeof(o3m_module), name, sizeof(KState), 2, kParams, k_init, k_render, k_set, nullptr, nullptr, k_shutdown, nullptr, 0 }
const o3m_module kDuck = DUCKER("duck", O3M_ABI_MINOR);
const o3m_module kDuckOld = DUCKER("duck11", 11);  // declares 1.11: its KEY bit is not honoured

// A mini-ducker that mirrors the template modules/templates/effect_key/example_key.cpp: the gain follows the key's level (1 - Depth x level), ramped across the block, and a
// silent input block resets env and gain to 0 / 1 — the template's pattern for forgetting the key across a gap. The host test of that contract is the last `key:` case.
struct alignas(16) DState {
  const o3m_host* host;
  float depth;
  std::uint32_t key;
  float env;
  float gain;
  std::uint32_t renders;
};
int d_init(void* raw, const o3m_host* host) {
  DState* d = static_cast<DState*>(raw);
  d->host = host;
  d->depth = 0.8f;
  d->key = 0;
  d->env = 0.0f;
  d->gain = 1.0f;
  d->renders = 0;
  return 0;
}
void d_shutdown(void*) {}
void d_set(void* raw, std::uint32_t id, float v) {
  DState* d = static_cast<DState*>(raw);
  if (id == 0u) {
    d->depth = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  } else if (id == 1u) {
    d->key = static_cast<std::uint32_t>(v + 0.5f);
  }
}
std::uint32_t d_render(void* raw, const float* in, float* out, std::uint32_t n) {
  DState* d = static_cast<DState*>(raw);
  ++d->renders;
  float any = 0.0f;
  for (std::uint32_t i = 0; i < n; ++i) {
    any += in[i] != 0.0f ? 1.0f : 0.0f;
  }
  if (any == 0.0f) {  // silent input: reset once — the host skips the effect on the silent blocks after this one
    d->env = 0.0f;
    d->gain = 1.0f;
    return 0;
  }
  float level = 0.0f;
  if (d->key != 0u && O3M_HOST_HAS(d->host, track_level)) {
    level = d->host->track_level(d->key - 1u);
  }
  if (level > 1.0f) {
    level = 1.0f;
  }
  d->env = level;
  const float g1 = 1.0f - d->depth * d->env;
  float g = d->gain;
  const float dg = (g1 - g) / static_cast<float>(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    g += dg;
    out[i] = in[i] * g;
  }
  d->gain = g1;
  return 1;
}
const o3m_param kDParams[2] = {{"Depth", 0.0f, 1.0f, 0.8f, 0}, {"Key", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY}};
const o3m_module kDuck2 = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), "duck2", sizeof(DState), 2, kDParams, d_init, d_render, d_set, nullptr, nullptr, d_shutdown, nullptr, 0};

std::uint32_t g_gclock = 0;
std::uint32_t graph_clock() noexcept { return ++g_gclock; }
std::uint32_t g_ins_clock = 0;
std::uint32_t ins_clock() noexcept { return g_ins_clock += 1u; }

bool route_insert(void* self, const core::ParamChange& c) noexcept {
  return core::param_kind(c.id) == core::ParamKind::Insert && static_cast<InsertHost*>(self)->apply_param(c);
}

struct Rig {
  Graph* g = nullptr;
  ModuleVoices* mv = nullptr;
  InsertHost* h = nullptr;
  alignas(16) KState st[InsertHost::kInstances + 1];
  InsertHost::Blobs blobs;
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  alignas(8) std::int16_t feed[audio::kTracks][kB] = {};
  ha::BlockReport report{};

  Rig() {
    static test::StaticSlot<Graph> gslot;
    static test::StaticSlot<ModuleVoices> mslot;
    static test::StaticSlot<InsertHost> hslot;
    static audio::TapMem taps;
    g_gclock = 0;
    g_ins_clock = 0;
    module::g_keys.clear();
    g = &gslot.make(&graph_clock, taps);
    mv = &mslot.make();
    h = &hslot.make();
    std::memset(st, 0, sizeof st);
    std::memset(blobs, 0, sizeof blobs);
    h->attach(*g, *mv, &blobs);
    h->set_cycles(&ins_clock);
    g->set_param_hook(&route_insert, h);
  }
  ~Rig() { module::g_keys.clear(); }
  void dc(unsigned t, std::int16_t v) {  // a track fed with a constant block
    fill(t, v);
    CHECK(g->params().push(core::ParamChange{audio::strip_param(t, gps::Source), 3.0f, 1}));
    g->feed(t, feed[t]);
  }
  void fill(unsigned t, std::int16_t v) {
    for (std::int16_t& s : feed[t]) {
      s = v;
    }
  }
  void block() {  // as the Engine does it: the graph, the boundary, the main loop's tick
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0, &report};
    g->render(io);
    h->begin_block();
    h->service();
  }
  void blocks(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      block();
    }
  }
  // Binds the ducker (or `m`) at `where` with Gain and Key.
  InsertHost::Bind bind(unsigned where, unsigned state_index, float gain, float key, const o3m_module& m = kDuck) {
    const float blob[2] = {gain, key};
    return h->bind(where, m, 0x8001, &st[state_index], reinterpret_cast<const std::uint8_t*>(blob), sizeof blob, false);
  }
  static float level(std::int16_t v) { return static_cast<float>(v < 0 ? -v : v) * kQ; }
};

}  // namespace

TEST_CASE("key: a key on a lower track is this block's level, on a higher track the previous block's") {
  {
    Rig r;  // the ducker on track 2 (where 4), its key is track 1 (T1): the key's strip runs first
    r.dc(0, 1000);
    r.dc(1, 1000);
    CHECK(r.bind(2, 0, 1.0f, 1.0f) == InsertHost::Bind::Ok);
    r.blocks(4);
    CHECK(r.st[0].seen == Rig::level(1000));
    r.fill(0, 5000);
    r.block();
    CHECK(r.st[0].seen == Rig::level(5000));  // this block's: the key track ran before the ducker's
  }
  {
    Rig r;  // the ducker on track 1 (where 0), its key is track 2 (T2): the key's strip runs after, so its word is the block before's
    r.dc(0, 1000);
    r.dc(1, 1000);
    CHECK(r.bind(0, 0, 1.0f, 2.0f) == InsertHost::Bind::Ok);
    r.blocks(4);
    CHECK(r.st[0].seen == Rig::level(1000));
    r.fill(1, 5000);
    r.block();
    CHECK(r.st[0].seen == Rig::level(1000));  // the previous block's
    r.block();
    CHECK(r.st[0].seen == Rig::level(5000));  // one block late
  }
}

TEST_CASE("key: the master insert reads this block's level of every track, both instances alike") {
  Rig r;
  r.dc(0, 2000);
  r.dc(3, 2000);
  const float blob[2] = {1.0f, 4.0f};  // Key T4: track 3, a higher track than the first strip
  CHECK(r.h->bind(InsertHost::kMasterWhere, kDuck, 0x8001, &r.st[0], reinterpret_cast<const std::uint8_t*>(blob), sizeof blob, false, &r.st[1]) == InsertHost::Bind::Ok);
  r.blocks(4);
  CHECK(r.st[0].seen == Rig::level(2000));
  CHECK(r.st[1].seen == Rig::level(2000));
  r.fill(3, 9000);
  r.block();
  CHECK(r.st[0].seen == Rig::level(9000));  // the master's stage runs after every strip: this block's, whatever the track
  CHECK(r.st[1].seen == Rig::level(9000));
  CHECK(r.h->key_mask() == (1u << 3));
  CHECK(r.h->taps() == (1u << 3));
}

TEST_CASE("key: a silent key track reads 0, and a track that stops running is 0 one block later") {
  Rig r;
  r.dc(0, 0);
  r.dc(1, 1000);
  CHECK(r.bind(2, 0, 1.0f, 1.0f) == InsertHost::Bind::Ok);  // the ducker on track 2, key T1 (a silent track)
  r.blocks(5);
  CHECK(r.st[0].seen == 0.0f);
  r.fill(0, 6000);
  r.blocks(2);
  CHECK(r.st[0].seen == Rig::level(6000));
  r.g->feed(0, nullptr);  // the key strip has no source: it does not run
  r.block();
  CHECK(r.st[0].seen == Rig::level(6000));  // the block before's, once
  r.block();
  CHECK(r.st[0].seen == 0.0f);
  CHECK(r.mv->host().track_level(0) == 0.0f);
  CHECK(r.mv->host().track_level(16) == 0.0f);  // a bad index
  CHECK(r.mv->host().track_level(9) == 0.0f);   // a track nobody names
}

TEST_CASE("key: slot B measures: the tap in an empty slot B, the module in slot B, the tap over a bypassed one, and back; a module in slot A is measured through") {
  Rig r;
  r.dc(0, 8000);
  r.dc(1, 100);
  CHECK(r.bind(2, 0, 1.0f, 1.0f) == InsertHost::Bind::Ok);  // the ducker on track 2, key T1
  r.blocks(4);
  CHECK(r.h->taps() == 1u);
  CHECK(r.st[0].seen == Rig::level(8000));  // the tap
  CHECK(r.bind(1, 1, 0.5f, 0.0f) == InsertHost::Bind::Ok);  // track 1 slot B: a gain of one half takes the slot over the tap
  r.blocks(5);
  CHECK(r.h->taps() == 0u);
  CHECK(r.st[0].seen == Rig::level(4000));  // the module's own output
  CHECK(r.h->set_bypass(1, true) == InsertHost::Toggle::Done);
  r.blocks(4);
  CHECK(r.h->taps() == 1u);  // the tap over the bypassed module: the key is the dry signal
  CHECK(r.st[0].seen == Rig::level(8000));
  CHECK(r.h->set_bypass(1, false) == InsertHost::Toggle::Done);  // a reset: the module leaves, comes back, takes the slot from the tap
  r.blocks(10);
  CHECK(r.h->taps() == 0u);
  CHECK(r.st[0].seen == Rig::level(4000));
  CHECK(r.h->unbind(1));
  r.blocks(8);
  CHECK(r.h->taps() == 1u);  // the module left: the tap is back
  CHECK(r.st[0].seen == Rig::level(8000));
  CHECK(r.bind(0, 2, 0.25f, 0.0f) == InsertHost::Bind::Ok);  // track 1 slot A: the tap in slot B measures what A leaves
  r.blocks(6);
  CHECK(r.h->taps() == 1u);
  CHECK(r.st[0].seen == Rig::level(2000));
  CHECK(r.h->unbind(2));  // the ducker leaves: no key names the track, so no tap
  r.blocks(8);
  CHECK(r.h->key_mask() == 0u);
  CHECK(r.h->taps() == 0u);
  CHECK(r.mv->host().track_level(0) == 0.0f);
}

TEST_CASE("key: a KEY parameter changed by ParamChange moves the key and the tap; a module of ABI 1.11 never has the flag honoured") {
  Rig r;
  r.dc(0, 3000);
  r.dc(1, 100);
  r.dc(2, 7000);
  CHECK(r.bind(2, 0, 1.0f, 1.0f) == InsertHost::Bind::Ok);  // the ducker on track 2, key T1
  r.blocks(4);
  CHECK(r.h->key_mask() == 1u);
  CHECK(r.st[0].seen == Rig::level(3000));
  CHECK(r.h->set_param(2, 1, 3.0f));  // T3: track index 2
  r.blocks(3);
  CHECK(r.h->key_mask() == (1u << 2));
  CHECK(r.h->taps() == (1u << 2));
  CHECK(r.st[0].seen == Rig::level(7000));
  CHECK(r.h->set_param(2, 1, 0.0f));  // OFF
  r.blocks(3);
  CHECK(r.h->key_mask() == 0u);
  CHECK(r.h->taps() == 0u);
  CHECK(r.h->unbind(2));
  r.blocks(8);
  // a module that declares 1.11 has the bit in its flags and no key
  CHECK(r.bind(2, 1, 1.0f, 1.0f, kDuckOld) == InsertHost::Bind::Ok);
  r.blocks(5);
  CHECK(r.h->key_mask() == 0u);
  CHECK(r.h->taps() == 0u);
  CHECK(r.mv->host().track_level(0) == 0.0f);
}

TEST_CASE("key: an export starts with no level of the live render, two exports give equal bits, and nothing allocates in the render") {
  Rig r;
  r.dc(0, 4000);
  r.dc(1, 100);
  CHECK(r.bind(0, 0, 0.5f, 2.0f) == InsertHost::Bind::Ok);  // the ducker on track 1, key T2: the key is a block late, so a stale word shows
  r.blocks(6);
  CHECK(r.st[0].seen == Rig::level(100));
  const unsigned allocs = insert_alloc::count;
  insert_alloc::window = true;
  r.blocks(50);
  insert_alloc::window = false;
  CHECK(insert_alloc::count == allocs);
  // two exports of one scene: the key words are cleared, the effect restarts, the first block reads 0 and the whole run is the same
  float first[2] = {-1.0f, -1.0f};
  std::int16_t bits[2][8 * kBS];
  for (unsigned run = 0; run < 2u; ++run) {
    CHECK(r.h->prepare_export());
    CHECK(r.mv->host().track_level(1) == 0.0f);
    for (unsigned b = 0; b < 8u; ++b) {
      r.fill(1, static_cast<std::int16_t>(500 + 700 * b));  // the key track changes block by block
      r.block();
      if (b == 0u) {
        first[run] = r.st[0].seen;
      }
      std::memcpy(bits[run] + b * kBS, r.out, sizeof r.out);
    }
    r.h->export_end();
  }
  CHECK(first[0] == 0.0f);  // the live render's level (100) is not read by the export
  CHECK(first[1] == 0.0f);
  CHECK(std::memcmp(bits[0], bits[1], sizeof bits[0]) == 0);
}

// ---- the engine: a muted key track still renders its voices -------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kLen = 3000;
std::int16_t g_wave[kLen];
std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 7u; }

struct Src {
  std::uint32_t note_block = 1;
};
std::uint32_t src_step(void* self, std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept {
  const Src& s = *static_cast<Src*>(self);
  if (cap < 2u || static_cast<std::uint32_t>(block_start / n) != s.note_block) {
    return 0;
  }
  // Two notes: track 0 is the muted key track, track 1 carries the ducker — the engine feeds a track only while a voice sounds on it (play_block
  // overwrites Graph::feed with voices_->feed), so the ducker's strip runs only if its own track sounds.
  for (unsigned k = 0; k < 2u; ++k) {
    audio::SeqEvent e{};
    e.offset = 17;
    e.kind = audio::EventKind::NoteOn;
    e.track = static_cast<std::uint8_t>(k);
    e.a = 60;
    e.b = 0;
    e.c = 127;
    out[k] = e;
  }
  return 2;
}

struct GateRun {
  std::uint32_t gated = 0;
  float seen = -1.0f;
  std::int32_t peak_out = 0;
};

// A sustained looped note on track 0, the track muted; the ducker on track 2 (Key = `key`, 0: none); `blocks` blocks.
GateRun gate_run(float key, unsigned blocks) {
  static test::StaticSlot<audio::Engine> eslot;
  static test::StaticSlot<audio::VoicePool> pslot;
  static test::StaticSlot<ModuleVoices> mslot;
  static test::StaticSlot<InsertHost> hslot;
  static audio::TapMem taps;
  static audio::SeqEvent events[audio::kMaxEventsPerBlock];
  static InsertHost::Blobs blobs;
  alignas(16) static KState st;
  alignas(8) static std::int16_t feed1[kB];
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  for (std::uint32_t i = 0; i < kLen; ++i) {
    g_wave[i] = static_cast<std::int16_t>(std::lround(16000.0 * std::sin(6.283185307 * 7.0 * static_cast<double>(i) / kLen)));
  }
  module::g_keys.clear();
  audio::Engine& e = eslot.make(&fake_cycles, taps);
  audio::VoicePool& pool = pslot.make();
  ModuleVoices& mv = mslot.make();
  InsertHost& h = hslot.make();
  std::memset(blobs, 0, sizeof blobs);
  std::memset(&st, 0, sizeof st);
  audio::SampleDesc d{g_wave, kLen, 0, kLen, 65536};
  pool.set_sample(0, d);
  e.attach_voices(&pool);
  e.attach_modules(&mv);
  Src src;
  src.note_block = 1;
  audio::EventSource es;
  es.self = &src;
  es.step = &src_step;
  es.buffer = events;
  es.capacity = audio::kMaxEventsPerBlock;
  e.attach_events(es);
  GateRun r;
  if (!e.attach_inserts(&h, &blobs)) {
    FAIL("attach_inserts");
    return r;
  }
  for (std::int16_t& s : feed1) {
    s = 500;
  }
  CHECK(e.graph().params().push(core::ParamChange{audio::strip_param(1, gps::Source), 3.0f, 1}));
  e.graph().feed(1, feed1);
  CHECK(e.graph().params().push(core::ParamChange{audio::strip_param(0, gps::Mute), 1.0f, 1}));
  const float blob[2] = {1.0f, key};
  CHECK(h.bind(2, kDuck, 0x8001, &st, reinterpret_cast<const std::uint8_t*>(blob), sizeof blob, false) == InsertHost::Bind::Ok);
  ha::BlockReport report{};
  for (unsigned b = 0; b < blocks; ++b) {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0, &report};
    const ha::BlockCallback cb = e.callback();
    cb.fn(cb.user, io);
    h.service();
    for (std::uint32_t i = 0; i < kBS; ++i) {
      const std::int32_t v = out[i] < 0 ? -out[i] : out[i];
      r.peak_out = v > r.peak_out ? v : r.peak_out;
    }
  }
  r.gated = pool.stats().gated_blocks;
  r.seen = st.seen;
  module::g_keys.clear();
  return r;
}

}  // namespace

TEST_CASE("key: a muted key track still keys: its voices are not gated while it is a key, and are when it is not") {
  const GateRun keyed = gate_run(1.0f, 24);
  CHECK(keyed.gated == 0u);          // the key track's note kept rendering
  CHECK(keyed.seen > 0.1f);          // and the ducker saw it (the tap measures before the fader)
  const GateRun plain = gate_run(0.0f, 24);
  CHECK(plain.gated >= 6u);          // no key: the muted track's voice is gated as it always was (OS-ODD11)
  CHECK(plain.seen < 0.0f);          // the ducker read nothing
  CHECK(keyed.peak_out == plain.peak_out);  // the key track is muted either way: nobody hears it, the ducker's track 2 is the only sound (the same block, not ducked: gain 1)
}

// The review fold of A6: a ducker whose track goes silent for a while and then plays again must open the new note at unity, not at the frozen gain. The reset lives in the
// module (the template's pattern): a silent input block sets env 0, gain 1, and the host skips the effect on the silent blocks after it. A track with no source at all is never
// called (the strip settles), which no module-side code can see — pinned here and stated in module/sdk/README.md.
TEST_CASE("key: an idle-silent ducker track that then gets a note opens at unity; an unfed strip never calls the effect") {
  Rig r;
  r.dc(0, 10000);  // the key track, loud
  r.dc(2, 4000);   // the ducker's track
  alignas(16) DState ds;
  const float blob[2] = {0.8f, 1.0f};  // Depth 0.8, Key T1 (track 0)
  CHECK(r.h->bind(4, kDuck2, 0x8001, &ds, reinterpret_cast<const std::uint8_t*>(blob), sizeof blob, false) == InsertHost::Bind::Ok);  // track 2 slot A
  r.blocks(4);
  CHECK(ds.gain < 0.9f);  // ducked by the key's level
  // the ducker's track is fed silence (a zero block, still a source): the first silent block resets, the host then skips the effect
  r.fill(0, 0);
  r.fill(2, 0);
  r.blocks(4);
  CHECK(ds.env == 0.0f);
  CHECK(ds.gain == 1.0f);
  const std::uint32_t renders_idle = ds.renders;
  r.blocks(3);
  CHECK(ds.renders == renders_idle);  // idle and silent: the OS does not call it
  // a new note with the key silent: it opens at unity
  r.fill(2, 4000);
  r.block();
  CHECK(ds.gain == 1.0f);
  for (std::uint32_t i = 0; i < kB; ++i) {
    const int m = (static_cast<int>(r.out[2u * i]) + static_cast<int>(r.out[2u * i + 1u])) / 2;
    CHECK(m == 4000);  // the strip's block unchanged: gain exactly 1
  }
  // a track with no source at all: the strip settles and the insert fn is not called — except the one tail block (the effect returned 1 while it sounded, so the strip stays live on the zero block once), which is also a silent block the effect sees
  r.g->feed(2, nullptr);
  r.blocks(3);  // the first of these may run the insert once through the tail (a zero block: the reset again); then the strip settles
  const std::uint32_t renders_unfed = ds.renders;
  r.blocks(3);
  CHECK(ds.renders == renders_unfed);  // settled: the OS does not call it (documented in module/sdk/README.md)
  r.g->feed(2, r.feed[2]);
  r.block();
  CHECK(ds.renders == renders_unfed + 1u);
  CHECK(ds.gain == 1.0f);  // still unity here: the silent block already reset it
}

TEST_CASE("key: a KEY naming the effect's own track is OFF (no self-feedback pump)") {
  Rig r;
  r.dc(1, 5000);
  r.dc(2, 7000);
  const float blob[2] = {1.0f, 3.0f};  // Key T3: track 2, the ducker's own track (bound at where 4)
  CHECK(r.h->bind(4, kDuck, 0x8001, &r.st[0], reinterpret_cast<const std::uint8_t*>(blob), sizeof blob, false) == InsertHost::Bind::Ok);
  r.blocks(5);
  CHECK(r.h->key_mask() == 0u);  // its own track is not a key: no tap, no mask
  CHECK(r.h->taps() == 0u);
  CHECK(r.st[0].seen == 0.0f);  // the module still asks for track 2, but nobody measures it: 0, no pump
  CHECK(r.h->set_param(4, 1, 2.0f));  // T2: track 1, a real key
  r.blocks(3);
  CHECK(r.h->key_mask() == (1u << 1));
  CHECK(r.h->taps() == (1u << 1));
  CHECK(r.st[0].seen == Rig::level(5000));
}
