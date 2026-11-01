// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX S1 (docs/specs/OS-INSFX.md): the insert host (audio/insert_host.h) for effect modules, driven block by block on the real Graph with native effect modules written here
// (a gain, a one-pole low-pass with state, a module that goes idle on silence, a NaN one, a loud one). Host only.
//   * the adapter: float / Q15, the in-place render, the transition ramps, the module rule of the Q15 conversion (NaN is silence, clips counted), the idle skip;
//   * the swap protocol: a ParamChange queued for a binding that has left (and whose record was reused) is dropped, a parameter set right after the bind waits for the render to hold the binding,
//     a full parameter ring and a full command ring lose nothing, a slot on a strip that never runs is retired at the boundary all the same;
//   * both nets (per instance, aggregate), the forced bypass and its clear; admission of a re-bind; the bind refusals;
//   * the state reset at un-bypass and at the start of an export (two exports give the same bits; the nets are off in an export);
//   * the Engine's side: attach_inserts needs the modules, the block boundary drives the host, ParamKind::Insert reaches it.
#include <doctest/doctest.h>

#include "support/static_slot.h"

#include <o3os/audio/engine.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/insert_host.h>
#include <o3os/audio/kernels.h>
#include <o3os/audio/module_q15.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/taps.h>
#include <o3os/audio/voices.h>
#include <o3os/core/param.h>
#include <o3os/hal/audio.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

// A counting allocator for the whole test binary: every operator new counts while a window is open (the render's own calls open one: Rig::block, `render_block`), and the cases that prove "no
// allocation in the render path" read the count. Outside a window it is malloc.
namespace insert_alloc {
bool window = false;
unsigned count = 0;
struct Window {  // RAII: the render (the graph's pass and the host's boundary) runs inside
  bool was;
  Window() : was(window) { window = true; }
  ~Window() { window = was; }
};
}  // namespace insert_alloc
namespace insert_alloc {
// Every replaceable form is replaced (the sanitizer's own would otherwise allocate what these free): plain, array, nothrow, aligned, and the matching deletes. Not inlined: GCC would see malloc and
// free across a direct call and warn of a mismatch.
[[gnu::noinline]] void* get(std::size_t n, std::size_t align) noexcept {
  if (window) {
    ++count;
  }
  if (align > alignof(std::max_align_t)) {
    return std::aligned_alloc(align, (n + align - 1u) / align * align);
  }
  return std::malloc(n != 0u ? n : 1u);
}
[[gnu::noinline]] void* must(void* p) {
  if (p == nullptr) {
    std::abort();
  }
  return p;
}
}  // namespace insert_alloc
[[gnu::noinline]] void* operator new(std::size_t n) { return insert_alloc::must(insert_alloc::get(n, 0)); }
[[gnu::noinline]] void* operator new[](std::size_t n) { return insert_alloc::must(insert_alloc::get(n, 0)); }
[[gnu::noinline]] void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return insert_alloc::get(n, 0); }
[[gnu::noinline]] void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return insert_alloc::get(n, 0); }
[[gnu::noinline]] void* operator new(std::size_t n, std::align_val_t a) { return insert_alloc::must(insert_alloc::get(n, static_cast<std::size_t>(a))); }
[[gnu::noinline]] void* operator new[](std::size_t n, std::align_val_t a) { return insert_alloc::must(insert_alloc::get(n, static_cast<std::size_t>(a))); }
[[gnu::noinline]] void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return insert_alloc::get(n, static_cast<std::size_t>(a)); }
[[gnu::noinline]] void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return insert_alloc::get(n, static_cast<std::size_t>(a)); }
[[gnu::noinline]] void operator delete(void* p) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete[](void* p) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

namespace o3os::audio {
// The friend of InsertHost: the command ring cannot be filled through the public calls (four instances, three commands each); two states the public calls cannot reach are made here.
struct InsertHostTest {
  static void fill_commands(InsertHost& h) {
    for (;;) {
      const InsertHost::Command c{InsertHost::Cmd::Bypass, 0xFF, 0, 0, 0, 0, 0};  // an instance that does not exist: the render pops it and ignores it
      if (!h.ring_.push(c)) {
        break;
      }
    }
  }
  // A record that never finishes leaving (the Deactivate was sent and the render's retirement never comes): what prepare_export must report instead of falling through.
  static void stick_leaving(InsertHost& h, unsigned i) {
    InsertHost::Command c;
    while (h.ring_.pop(c)) {
    }
    h.in_[i].ml = InsertHost::Ml::Leaving;
    h.in_[i].then_reactivate = false;
    h.in_[i].act_pushed = true;
    h.in_[i].deact_pushed = true;
  }
  static void set_gen(InsertHost& h, unsigned i, std::uint16_t g) { h.in_[i].gen = g; }
  // The "gone" figure (what the base still holds of inserts that stopped), and an insert made to look as if it had been measured (its previous block's cost and an estimate).
  static std::uint32_t gone(const InsertHost& h) { return h.gone_.load(std::memory_order_relaxed); }
  static void measured(InsertHost& h, unsigned i, std::uint32_t cost) {
    h.in_[i].prev = cost;
    h.in_[i].est.store(cost, std::memory_order_relaxed);
  }
};
}  // namespace o3os::audio

namespace {

using namespace o3os;
using audio::Graph;
using audio::InsertHost;
using audio::InsertHostTest;
using audio::ModuleVoices;
namespace ha = o3os::hal::audio;
namespace gps = o3os::audio::graph_param::strip;
namespace gpm = o3os::audio::graph_param::master;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kBS = ha::kBlockSamples;

#define NEED(cond)                                  \
  do {                                              \
    const bool need_ok_ = static_cast<bool>(cond);  \
    CHECK(need_ok_);                                \
    if (!need_ok_) {                                \
      return;                                       \
    }                                               \
  } while (0)

// ---- native effect modules ----------------------------------------------------------------------------------------------------------
struct alignas(16) FxState {
  float gain;
  float y;
  std::uint32_t renders;
  std::uint32_t pad;
};
int g_init = 0;
int g_shut = 0;
int g_params = 0;
int g_calls = 0;
int g_fail_init = 0;
std::uint32_t g_last_id = 0;
float g_last_value = 0.0f;

int fx_init(void* s, const o3m_host*) {
  if (g_fail_init != 0) {
    return 1;
  }
  ++g_init;
  static_cast<FxState*>(s)->gain = 1.0f;
  return 0;
}
void fx_shutdown(void*) { ++g_shut; }
void fx_set_param(void* s, std::uint32_t id, float v) {
  ++g_params;
  g_last_id = id;
  g_last_value = v;
  if (id == 0u) {
    static_cast<FxState*>(s)->gain = v;
  }
}
std::uint32_t gain_render(void* s, const float* in, float* out, std::uint32_t n) {
  FxState* d = static_cast<FxState*>(s);
  ++g_calls;
  ++d->renders;
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = in[i] * d->gain;
  }
  return 1;
}
std::uint32_t lp_render(void* s, const float* in, float* out, std::uint32_t n) {  // a one-pole low-pass: it has state that a reset must clear
  FxState* d = static_cast<FxState*>(s);
  ++g_calls;
  ++d->renders;
  for (std::uint32_t i = 0; i < n; ++i) {
    d->y += (in[i] - d->y) * 0.1f;
    out[i] = d->y * d->gain;
  }
  return 1;
}
std::uint32_t idle_render(void* s, const float* in, float* out, std::uint32_t n) {  // silent and idle on a silent input
  FxState* d = static_cast<FxState*>(s);
  ++g_calls;
  float any = 0.0f;
  for (std::uint32_t i = 0; i < n; ++i) {
    any += in[i] != 0.0f ? 1.0f : 0.0f;
    out[i] = in[i] * d->gain;
  }
  return any != 0.0f ? 1u : 0u;
}
std::uint32_t nan_render(void*, const float*, float* out, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = std::nanf("");
  }
  return 1;
}
std::uint32_t loud_render(void*, const float* in, float* out, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = in[i] * 100.0f;
  }
  return 1;
}

// Two gain effects with a cost of their own: each render moves the insert host's clock by g_cost_a / g_cost_b (g_block_cycles sums what the inserts took in the block, for the governor's base).
std::uint32_t g_ins_clock = 0;
std::uint32_t g_cost_a = 0;
std::uint32_t g_cost_b = 0;
std::uint32_t g_block_cycles = 0;
std::uint32_t cost_a_render(void* s, const float* in, float* out, std::uint32_t n) {
  g_ins_clock += g_cost_a;
  g_block_cycles += g_cost_a;
  return gain_render(s, in, out, n);
}
std::uint32_t cost_b_render(void* s, const float* in, float* out, std::uint32_t n) {
  g_ins_clock += g_cost_b;
  g_block_cycles += g_cost_b;
  return gain_render(s, in, out, n);
}
const o3m_param kFxParams[1] = {{"Gain", 0.0f, 4.0f, 1.0f, 0}};
#define FX_MODULE(name, kind, render) \
  o3m_module { O3M_ABI_MAJOR, O3M_ABI_MINOR, kind, sizeof(o3m_module), name, sizeof(FxState), 1, kFxParams, fx_init, render, fx_set_param, nullptr, nullptr, fx_shutdown, nullptr, 0 }
const o3m_module kGain = FX_MODULE("gain", O3M_KIND_EFFECT, gain_render);
const o3m_module kLp = FX_MODULE("lp", O3M_KIND_EFFECT, lp_render);
const o3m_module kIdle = FX_MODULE("idle", O3M_KIND_EFFECT, idle_render);
const o3m_module kNan = FX_MODULE("nan", O3M_KIND_EFFECT, nan_render);
const o3m_module kLoud = FX_MODULE("loud", O3M_KIND_EFFECT, loud_render);
const o3m_module kInstrument = FX_MODULE("inst", O3M_KIND_INSTRUMENT, gain_render);
const o3m_module kCostA = FX_MODULE("cost_a", O3M_KIND_EFFECT, cost_a_render);
const o3m_module kCostB = FX_MODULE("cost_b", O3M_KIND_EFFECT, cost_b_render);

// The insert host's own clock: every call moves it by g_step, so one run() measures exactly g_step cycles.
std::uint32_t g_step = 0;
std::uint32_t ins_clock() noexcept { return g_ins_clock += g_step; }
std::uint32_t g_gclock = 0;
std::uint32_t graph_clock() noexcept { return ++g_gclock; }

void reset_counters() {
  g_init = g_shut = g_params = g_calls = g_fail_init = 0;
  g_last_id = 0;
  g_last_value = 0.0f;
  g_step = 0;
  g_ins_clock = 0;
  g_cost_a = g_cost_b = g_block_cycles = 0;
  g_gclock = 0;
}

// ---- the graph rig ------------------------------------------------------------------------------------------------------------------
bool route_insert(void* self, const core::ParamChange& c) noexcept {
  return core::param_kind(c.id) == core::ParamKind::Insert && static_cast<InsertHost*>(self)->apply_param(c);
}

struct Rig {
  Graph* g = nullptr;
  ModuleVoices* mv = nullptr;
  InsertHost* h = nullptr;
  alignas(16) FxState st[InsertHost::kInstances + 1];
  InsertHost::Blobs blobs;
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  alignas(8) std::int16_t feed[audio::kTracks][kB] = {};
  alignas(8) std::int16_t side[kB] = {};  // the stereo S of track 0 (side_track)
  ha::BlockReport report{};

  Rig() {
    static test::StaticSlot<Graph> gslot;
    static test::StaticSlot<ModuleVoices> mslot;
    static test::StaticSlot<InsertHost> hslot;
    static audio::TapMem taps;
    reset_counters();
    g = &gslot.make(&graph_clock, taps);
    mv = &mslot.make();
    h = &hslot.make();
    std::memset(st, 0, sizeof st);
    std::memset(blobs, 0, sizeof blobs);
    h->attach(*g, *mv, &blobs);
    h->set_cycles(&ins_clock);
    g->set_param_hook(&route_insert, h);
  }
  // A track fed with a constant block.
  void dc(unsigned t, std::int16_t v) {
    for (std::int16_t& s : feed[t]) {
      s = v;
    }
    CHECK(g->params().push(core::ParamChange{audio::strip_param(t, gps::Source), 3.0f, 1}));
    g->feed(t, feed[t]);
  }
  // The stereo S of track `t` (a constant block), next to the M that dc() feeds: L = M + S, R = M - S at the centre.
  void dc_side(unsigned t, std::int16_t v) {
    for (std::int16_t& s : side) {
      s = v;
    }
    g->feed_side(t, side);
  }
  void fill(unsigned t, std::int16_t v) {
    for (std::int16_t& s : feed[t]) {
      s = v;
    }
  }
  // One block as the Engine does it: the graph, then the boundary, then the main loop's tick.
  void block() {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0, &report};
    {
      insert_alloc::Window w;  // the render context: the graph's pass and the boundary (the main loop's tick below is outside)
      g->render(io);
      h->begin_block();
    }
    h->service();
  }
  void blocks(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      block();
    }
  }
  // A block as the Engine closes it with the render's other work in the governor's base: `others` cycles besides the inserts (the sampler voices), then what the inserts took.
  void gblock(std::uint32_t others) {
    g_block_cycles = 0;
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0, &report};
    {
      insert_alloc::Window w;
      g->render(io);
      mv->governor().observe_block(others + g_block_cycles);
      h->begin_block();
    }
    h->service();
  }
  void gblocks(unsigned n, std::uint32_t others) {
    for (unsigned i = 0; i < n; ++i) {
      gblock(others);
    }
  }
  std::int16_t l(unsigned i) const { return out[2u * i]; }
  InsertHost::Phase phase(unsigned i) const {
    InsertHost::Info info{};
    return h->info(i, info) ? info.phase : InsertHost::Phase::Off;
  }
  InsertHost::Bind bind(unsigned where, const o3m_module& m, unsigned state_index, const float* params = nullptr, unsigned count = 0, bool force = false) {
    return h->bind(where, m, 0x8001, &st[state_index], reinterpret_cast<const std::uint8_t*>(params), count * 4u, force);
  }
  // Until the output of the last frame settles (the strips' own smoothing): the reference level a case compares with.
  int level_after_warmup() {
    blocks(10);
    return l(kB - 1);
  }
  bool steady_near(int expect, int tol) const {
    for (std::uint32_t i = 0; i < kB; ++i) {
      if (std::abs(static_cast<int>(l(i)) - expect) > tol) {
        return false;
      }
    }
    return true;
  }
};

}  // namespace

TEST_CASE("insert host: a gain effect halves the track, fades in over one block, and fades out and is shut down when unbound") {
  Rig r;
  r.dc(0, 8000);
  const int base = r.level_after_warmup();
  NEED(base > 100);
  const float half = 0.5f;
  NEED(r.bind(0, kGain, 0, &half, 1) == InsertHost::Bind::Ok);
  CHECK(r.h->bound(0));
  CHECK(g_init == 1);
  CHECK(g_params == 1);  // the stored blob, applied by the main loop before the render holds the binding
  r.block();             // the render has not heard of it yet: the block is dry; the boundary applies the Activate
  CHECK(std::abs(r.l(kB - 1) - base) <= 2);
  CHECK(r.phase(0) == InsertHost::Phase::In);
  r.block();  // the ramp block: dry at the start, wet at the end, never a step
  CHECK(r.l(0) > base - base / 8);
  CHECK(std::abs(r.l(kB - 1) - base / 2) <= 3);
  for (std::uint32_t i = 1; i < kB; ++i) {
    CHECK(r.l(i) <= r.l(i - 1) + 1);  // monotonic (rounding)
  }
  CHECK(r.phase(0) == InsertHost::Phase::On);
  r.block();
  CHECK(r.steady_near(base / 2, 3));
  CHECK(r.h->stats().saturated == 0);
  // a parameter through the graph's ring and the route
  NEED(r.h->set_param(0, 0, 0.25f));
  r.blocks(3);
  CHECK(r.steady_near(base / 4, 3));
  CHECK(r.h->stats().params == 1);
  CHECK(r.h->stats().stale == 0);
  CHECK(g_last_value == 0.25f);
  // unbind: a ramp to dry, the boundary retires the slot, the main loop shuts the module down two boundaries later
  CHECK(r.h->unbind(0));
  CHECK(!r.h->unbind(0));
  r.block();  // the Deactivate is applied at its boundary
  r.block();  // the ramp out
  CHECK(r.l(0) < base / 4 + base / 8);
  CHECK(std::abs(r.l(kB - 1) - base) <= 3);
  CHECK(g_shut == 0);
  r.blocks(3);
  CHECK(g_shut == 1);
  CHECK(!r.h->bound(0));
  CHECK(r.h->stats().retired == 1);
  CHECK(r.steady_near(base, 3));
  CHECK(r.h->settled());
}

// A bind and an unbind (mode 0) or a bypass on (mode 1) before the Activate reaches the render: both are applied at one boundary, so the slot never ramps in. The first blocks after are recorded;
// `fed_first` false: the strip has no source until after that boundary (an idle strip that starts in the block).
namespace {
void ramp_scenario(bool with_insert, unsigned mode, bool fed_first, std::int16_t (&rec)[4][kB]) {
  Rig r;
  if (fed_first) {
    r.dc(0, 8000);
    r.blocks(10);
  }
  if (with_insert) {
    const float half = 0.5f;
    NEED(r.bind(0, kGain, 0, &half, 1) == InsertHost::Bind::Ok);
    if (mode == 0u) {
      NEED(r.h->unbind(0));
    } else {
      NEED(r.h->set_bypass(0, true) == InsertHost::Toggle::Done);
    }
  }
  r.block();  // the boundary applies the Activate and the Deactivate / Bypass
  if (!fed_first) {
    r.dc(0, 8000);
  }
  for (unsigned b = 0; b < 4; ++b) {
    r.block();
    for (std::uint32_t i = 0; i < kB; ++i) {
      rec[b][i] = r.l(i);
    }
  }
  if (with_insert) {
    CHECK(g_calls == 0);  // a slot that leaves before it ran never calls the module
  }
}
}  // namespace

TEST_CASE("insert host: a bind then an unbind or a bypass inside one boundary stays dry from the first sample: no wet block between two dry ones") {
  for (unsigned mode = 0; mode < 2; ++mode) {
    for (unsigned fed = 0; fed < 2; ++fed) {
      CAPTURE(mode);
      CAPTURE(fed);
      static std::int16_t ref[4][kB];
      static std::int16_t got[4][kB];
      ramp_scenario(false, mode, fed != 0u, ref);
      ramp_scenario(true, mode, fed != 0u, got);
      int worst = 0;
      for (unsigned b = 0; b < 4; ++b) {
        for (std::uint32_t i = 0; i < kB; ++i) {
          worst = std::max(worst, std::abs(static_cast<int>(ref[b][i]) - static_cast<int>(got[b][i])));
        }
      }
      CHECK(worst <= 2);
    }
  }
}

TEST_CASE("insert host: the adapter's identity: every Q15 value goes to float and back unchanged, and nothing clips") {
  alignas(8) std::int16_t src[kB];
  alignas(8) std::int16_t dst[kB];
  alignas(16) float fl[kB];
  std::uint32_t clipped = 0;
  for (std::int32_t base = -32768; base < 32768; base += static_cast<std::int32_t>(kB)) {
    for (std::uint32_t i = 0; i < kB; ++i) {
      src[i] = static_cast<std::int16_t>(base + static_cast<std::int32_t>(i));
    }
    audio::kernel::q15_to_float(fl, src, kB);
    clipped += audio::to_q15(dst, fl, kB);
    for (std::uint32_t i = 0; i < kB; ++i) {
      if (dst[i] != src[i]) {
        FAIL("value " << src[i] << " came back as " << dst[i]);
      }
    }
  }
  CHECK(clipped == 0u);
}

TEST_CASE("insert host: the module rule of the Q15 conversion: NaN is silence and not a clip, a loud effect clips and is counted") {
  Rig r;
  r.dc(0, 8000);
  const int base = r.level_after_warmup();
  NEED(r.bind(0, kNan, 0) == InsertHost::Bind::Ok);
  r.blocks(4);
  CHECK(r.steady_near(0, 0));
  CHECK(r.h->stats().saturated == 0);
  CHECK(r.h->unbind(0));
  r.blocks(6);
  CHECK(!r.h->bound(0));
  NEED(r.bind(0, kLoud, 1) == InsertHost::Bind::Ok);
  r.blocks(4);
  CHECK(r.h->stats().saturated >= kB);
  CHECK(r.l(kB - 1) > base);  // 100 times the signal, clipped at full scale and then the strip's pan gain
}

TEST_CASE("insert host: a module that said idle is not called again on a silent input, and is called when the signal returns") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(r.bind(0, kIdle, 0) == InsertHost::Bind::Ok);
  r.blocks(4);
  CHECK(g_calls >= 2);
  r.fill(0, 0);
  r.block();  // the module sees silence and says idle
  const int calls = g_calls;
  r.blocks(4);
  CHECK(g_calls == calls);  // skipped
  r.fill(0, 8000);
  r.block();
  CHECK(g_calls == calls + 1);
  r.blocks(2);
  CHECK(g_calls == calls + 3);
}

TEST_CASE("insert host: swap protocol: a ParamChange for a binding that left (record reused) is dropped, the new binding's is applied") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  r.blocks(3);
  InsertHost::Info info{};
  NEED(r.h->info(0, info));
  const std::uint16_t gen1 = info.gen;
  CHECK(r.h->unbind(0));
  r.blocks(6);
  CHECK(!r.h->bound(0));
  NEED(r.h->bind(0, kGain, 0x8001, nullptr, nullptr, 0, false) == InsertHost::Bind::Ok);  // the same record, state and module again
  NEED(r.h->info(0, info));
  const std::uint16_t gen2 = info.gen;
  CHECK(gen2 != gen1);
  CHECK(gen2 != 0);
  CHECK(g_init == 2);
  r.block();  // activated
  const int params = g_params;
  CHECK(r.g->params().push(core::ParamChange{core::param_id(core::ParamKind::Insert, 0, 0), 0.125f, gen1}));
  r.block();
  CHECK(r.h->stats().stale == 1);
  CHECK(g_params == params);  // the module never saw it
  CHECK(r.g->params().push(core::ParamChange{core::param_id(core::ParamKind::Insert, 0, 0), 0.125f, gen2}));
  r.block();
  CHECK(r.h->stats().stale == 1);
  CHECK(g_params == params + 1);
  CHECK(g_last_value == 0.125f);
  // nothing bound at that index: stale as well
  CHECK(r.g->params().push(core::ParamChange{core::param_id(core::ParamKind::Insert, 5, 0), 1.0f, gen2}));
  r.block();
  CHECK(r.h->stats().stale == 2);
}

TEST_CASE("insert host: a parameter set right after the bind waits until the render holds the binding, and is neither lost nor stale") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  CHECK(r.h->set_param(0, 0, 0.25f));
  CHECK(g_params == 0);  // not sent: the render would drop it as stale
  r.blocks(3);
  CHECK(r.h->stats().stale == 0);
  CHECK(r.h->stats().params == 1);
  CHECK(g_last_value == 0.25f);
  // out of range, NaN, not bound
  CHECK(!r.h->set_param(0, 7, 1.0f));
  CHECK(!r.h->set_param(0, 0, std::nanf("")));
  CHECK(!r.h->set_param(9, 0, 1.0f));
}

TEST_CASE("insert host: a full parameter ring loses nothing: the change stays dirty and the next tick sends it") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  r.blocks(3);
  unsigned filled = 0;
  while (r.g->params().push(core::ParamChange{audio::master_param(gpm::Volume), 1.0f, 1})) {
    ++filled;
  }
  CHECK(filled > 0);
  CHECK(!r.h->set_param(0, 0, 0.5f));  // not sent: reported as not sent
  for (int i = 0; i < 12 && g_last_value != 0.5f; ++i) {
    r.block();
  }
  CHECK(g_last_value == 0.5f);
  CHECK(r.h->stats().stale == 0);
}

TEST_CASE("insert host: a full command ring never drops an Activate or a Deactivate: the main loop keeps the intent and the next tick pushes it") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  InsertHostTest::fill_commands(*r.h);
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  CHECK(r.h->stats().ring_full == 1);
  CHECK(r.phase(0) == InsertHost::Phase::Off);  // the render has not seen it
  r.blocks(4);                                   // the render pops the filler, the tick pushes the Activate, the next boundary applies it
  CHECK(r.phase(0) == InsertHost::Phase::On);
  InsertHostTest::fill_commands(*r.h);
  CHECK(r.h->unbind(0));
  r.blocks(10);
  CHECK(!r.h->bound(0));
  CHECK(g_shut == 1);
  CHECK(r.h->settled());
}

TEST_CASE("insert host: a strip that never runs the insert is retired at the boundary all the same, and a bypass reaches the slot") {
  Rig r;  // track 0 has no source: the graph never calls run()
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  r.blocks(3);
  CHECK(r.phase(0) == InsertHost::Phase::In);  // the ramp never ran
  CHECK(g_calls == 0);
  CHECK(r.h->set_bypass(0, true) == InsertHost::Toggle::Done);
  r.blocks(4);
  CHECK(r.phase(0) == InsertHost::Phase::Bypassed);
  CHECK(r.h->unbind(0));
  r.blocks(6);
  CHECK(!r.h->bound(0));
  CHECK(g_shut == 1);
  CHECK(g_calls == 0);
  CHECK(r.h->stats().retired == 1);
  NEED(r.bind(0, kGain, 0, nullptr, 0) == InsertHost::Bind::Ok);  // the record is free again: its state is reused
  r.blocks(3);
  CHECK(r.h->unbind(0));
  r.blocks(6);
  CHECK(!r.h->bound(0));
  CHECK(g_calls == 0);
  CHECK(g_shut == 2);
}

TEST_CASE("insert host: per-instance net: an insert over its budget for four blocks is forced to dry, flagged, never silence; un-bypass resets it") {
  Rig r;
  r.dc(0, 8000);
  const int base = r.level_after_warmup();
  g_step = 150000;  // over the instance budget (50 % of the block)
  const float half = 0.5f;
  NEED(r.bind(0, kGain, 0, &half, 1) == InsertHost::Bind::Ok);
  r.blocks(3);
  CHECK(r.h->stats().forced_instance == 0);
  CHECK(r.phase(0) == InsertHost::Phase::On);
  r.blocks(4);
  CHECK(r.h->stats().forced_instance == 1);
  CHECK(r.h->stats().forced_aggregate == 0);
  InsertHost::Info info{};
  NEED(r.h->info(0, info));
  CHECK(info.forced);
  CHECK(info.phase == InsertHost::Phase::Bypassed);
  CHECK(r.steady_near(base, 3));  // dry, not silent
  const int calls = g_calls;
  r.blocks(3);
  CHECK(g_calls == calls);  // a bypassed insert is not run
  // the user toggles bypass off: a reset (shutdown, init, the stored parameter), a new generation, and the net judges it again
  const std::uint16_t gen = info.gen;
  CHECK(r.h->set_bypass(0, false) == InsertHost::Toggle::Done);
  r.blocks(4);
  NEED(r.h->info(0, info));
  CHECK(!info.forced);
  CHECK(info.gen != gen);
  CHECK(g_shut == 1);
  CHECK(g_init == 2);
  CHECK(g_last_value == 0.5f);  // the blob again
  r.blocks(14);
  CHECK(r.h->stats().forced_instance == 2);
}

TEST_CASE("insert host: aggregate net: two inserts that each fit but not together: the most expensive is forced to dry, the other stays") {
  Rig r;
  r.dc(0, 8000);
  r.dc(1, 8000);
  r.blocks(4);
  g_step = 100000;
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  NEED(r.bind(2, kGain, 1) == InsertHost::Bind::Ok);  // track 1, slot A
  r.blocks(16);
  CHECK(r.h->stats().forced_instance == 0);
  CHECK(r.h->stats().forced_aggregate == 1);
  unsigned bypassed = 0;
  unsigned on = 0;
  for (unsigned i = 0; i < InsertHost::kInstances; ++i) {
    InsertHost::Info info{};
    if (r.h->info(i, info)) {
      bypassed += info.phase == InsertHost::Phase::Bypassed ? 1u : 0u;
      on += info.phase == InsertHost::Phase::On ? 1u : 0u;
    }
  }
  CHECK(bypassed == 1);
  CHECK(on == 1);
  r.blocks(12);
  CHECK(r.h->stats().forced_aggregate == 1);  // what is left fits
  CHECK(r.h->total_cycles() == 100000u);
}

// The governor's base falls by 1/256 a block only: with other work of 50 %, an insert of 30 % (A) and one of 20 % (B), the base is 100 % when A is forced, and stays near it.
TEST_CASE("insert host: aggregate net with a high base: the insert that was forced is taken off the base, the other stays (voices 50, A 30, B 20: only A)") {
  namespace gov = o3os::audio::gov;
  Rig r;
  r.dc(0, 8000);
  r.dc(1, 8000);
  const std::uint32_t voices = gov::cycles_of_percent(50);
  r.gblocks(4, voices);
  g_cost_a = gov::cycles_of_percent(30);
  g_cost_b = gov::cycles_of_percent(20);
  NEED(r.bind(0, kCostA, 0) == InsertHost::Bind::Ok);
  NEED(r.bind(2, kCostB, 1) == InsertHost::Bind::Ok);
  r.gblocks(24, voices);
  CHECK(r.h->stats().forced_instance == 0);
  CHECK(r.h->stats().forced_aggregate == 1);
  InsertHost::Phase a = InsertHost::Phase::Off;
  InsertHost::Phase b = InsertHost::Phase::Off;
  for (unsigned i = 0; i < InsertHost::kInstances; ++i) {
    InsertHost::Info info{};
    if (r.h->info(i, info)) {
      (info.where == 0u ? a : b) = info.phase;
    }
  }
  CHECK(a == InsertHost::Phase::Bypassed);  // the most expensive
  CHECK(b == InsertHost::Phase::On);
  r.gblocks(40, voices);
  CHECK(r.h->stats().forced_aggregate == 1);
  CHECK(r.h->total_cycles() == gov::cycles_of_percent(20));
}

TEST_CASE("insert host: a re-bind or an un-bypass right after the insert stopped is admitted: the base still holds its cycles") {
  namespace gov = o3os::audio::gov;
  Rig r;
  r.dc(0, 8000);
  const std::uint32_t voices = gov::cycles_of_percent(50);
  r.gblocks(4, voices);
  g_cost_a = gov::cycles_of_percent(30);
  NEED(r.bind(0, kCostA, 0) == InsertHost::Bind::Ok);
  r.gblocks(10, voices);
  CHECK(r.h->cost(0) == gov::cycles_of_percent(30));
  // 50 + 30 = 80 % in the base; 80 + the insert's 30 would be over the threshold of 90 %, but 30 of it is the insert's own
  NEED(r.h->set_bypass(0, true) == InsertHost::Toggle::Done);
  r.gblocks(4, voices);
  CHECK(r.phase(0) == InsertHost::Phase::Bypassed);
  CHECK(r.h->set_bypass(0, false) == InsertHost::Toggle::Done);
  r.gblocks(10, voices);
  CHECK(r.phase(0) == InsertHost::Phase::On);
  CHECK(r.h->unbind(0));
  r.gblocks(5, voices);
  CHECK(!r.h->bound(0));
  CHECK(r.h->bind(0, kCostA, 0x8001, nullptr, nullptr, 0, false) == InsertHost::Bind::Ok);  // the record's state is reused
}

TEST_CASE("insert host: a re-bind of a module that measured over the threshold is refused unless forced; the bind refusals") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  g_step = 250000;
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  r.blocks(10);
  CHECK(r.h->stats().forced_instance == 1);
  CHECK(r.h->cost(0) >= 250000u);
  CHECK(r.h->unbind(0));
  r.blocks(6);
  CHECK(!r.h->bound(0));
  CHECK(r.h->bind(0, kGain, 1, nullptr, nullptr, 0, false) == InsertHost::Bind::NoCpu);
  CHECK(!r.h->bound(0));
  CHECK(r.h->bind(0, kGain, 1, nullptr, nullptr, 0, true) == InsertHost::Bind::Ok);
  CHECK(r.h->unbind(0));
  g_step = 0;
  r.blocks(8);
  // refusals
  CHECK(r.bind(32, kGain, 1) == InsertHost::Bind::BadWhere);
  CHECK(r.bind(0, kInstrument, 1) == InsertHost::Bind::BadModule);
  CHECK(r.h->bind(0, kLp, 1, nullptr, nullptr, 0, false) == InsertHost::Bind::NeedState);  // no free record that held the lp
  CHECK(r.h->bind(0, kLp, 1, reinterpret_cast<std::uint8_t*>(r.st) + 4, nullptr, 0, false) == InsertHost::Bind::BadState);
  g_fail_init = 1;
  CHECK(r.bind(0, kLp, 1) == InsertHost::Bind::InitFailed);
  g_fail_init = 0;
  CHECK(!r.h->bound(0));
  NEED(r.bind(0, kLp, 1) == InsertHost::Bind::Ok);
  CHECK(r.bind(0, kLp, 2) == InsertHost::Bind::WhereTaken);
  NEED(r.bind(1, kLp, 2) == InsertHost::Bind::Ok);
  NEED(r.bind(2, kLp, 3) == InsertHost::Bind::Ok);
  NEED(r.bind(3, kLp, 4) == InsertHost::Bind::Ok);
  CHECK(r.bind(4, kLp, 0) == InsertHost::Bind::NoInstance);  // four at most
  // the stored blob: short blobs set the first parameters, a NaN is skipped
  CHECK(r.h->unbind(3));
  r.blocks(8);
  const float nan_blob[1] = {std::nanf("")};
  g_params = 0;
  NEED(r.bind(3, kLp, 4, nan_blob, 1) == InsertHost::Bind::Ok);
  CHECK(g_params == 0);
}

TEST_CASE("insert host: the state is reset at the start of an export (two exports give the same bits) and the nets are off in it") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(r.bind(0, kLp, 0) == InsertHost::Bind::Ok);
  CHECK(r.h->set_param(0, 0, 0.5f));
  r.blocks(10);
  const FxState* s = &r.st[0];
  CHECK(s->y > 0.0f);
  CHECK(s->renders > 4u);
  CHECK(g_init == 1);
  std::int16_t a[10][kB];
  std::int16_t b[10][kB];
  r.h->prepare_export();
  CHECK(g_shut == 1);
  CHECK(g_init == 2);
  CHECK(s->y == 0.0f);
  CHECK(s->gain == 0.5f);  // the stored parameter again
  for (unsigned i = 0; i < 10; ++i) {
    r.block();
    for (std::uint32_t k = 0; k < kB; ++k) {
      a[i][k] = r.l(k);
    }
  }
  r.h->export_end();
  r.blocks(5);  // live again: the state moves on
  CHECK(s->y > 0.0f);
  r.h->prepare_export();
  CHECK(s->y == 0.0f);
  for (unsigned i = 0; i < 10; ++i) {
    r.block();
    for (std::uint32_t k = 0; k < kB; ++k) {
      b[i][k] = r.l(k);
    }
  }
  CHECK(std::memcmp(a, b, sizeof a) == 0);
  CHECK(a[9][kB - 1] != 0);
  // the nets are off in the export, on again after it
  g_step = 300000;
  r.blocks(12);
  CHECK(r.h->stats().forced_instance == 0);
  r.h->export_end();
  r.blocks(12);
  CHECK(r.h->stats().forced_instance == 1);
}

TEST_CASE("insert host: an effect whose init fails on the export's restart refuses the export, leaves, and the live render goes on") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(r.bind(0, kLp, 0) == InsertHost::Bind::Ok);
  r.blocks(10);
  CHECK(g_init == 1);
  g_fail_init = 1;
  CHECK(!r.h->prepare_export());
  g_fail_init = 0;
  CHECK(r.h->stats().export_unsettled == 1);
  // the nets are on again (an insert over its budget is forced), and the insert whose state is zeroed leaves instead of running on
  r.blocks(8);
  CHECK(!r.h->bound(0));
  const int calls = g_calls;
  r.blocks(3);
  CHECK(g_calls == calls);
  // a good restart still passes
  NEED(r.bind(0, kLp, 0) == InsertHost::Bind::Ok);
  r.blocks(10);
  CHECK(r.h->prepare_export());
  CHECK(r.h->stats().export_unsettled == 1);
  r.h->export_end();
}

TEST_CASE("insert host: a leave during an export leaves no gone figure after export_end") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  r.blocks(10);
  NEED(r.h->prepare_export());
  r.blocks(2);
  InsertHostTest::measured(*r.h, 0, 40000);  // (it ran in the export with the nets off, which measure nothing: this gives it a cost so that leaving adds to the figure)
  CHECK(r.h->unbind(0));
  r.blocks(8);
  CHECK(!r.h->bound(0));
  CHECK(InsertHostTest::gone(*r.h) != 0u);  // not decayed while the nets are off
  r.h->export_end();
  CHECK(InsertHostTest::gone(*r.h) == 0u);
}

TEST_CASE("insert host: project load: retire_all, settled, release_all; a release without the render clears the slot and shuts down") {
  Rig r;
  r.dc(0, 8000);
  const int base = r.level_after_warmup();
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  NEED(r.bind(3, kLp, 1) == InsertHost::Bind::Ok);
  r.blocks(3);
  r.h->retire_all();
  CHECK(!r.h->settled());
  for (int i = 0; i < 10 && !r.h->settled(); ++i) {
    r.block();
  }
  CHECK(r.h->settled());
  r.blocks(3);
  r.h->release_all();
  CHECK(g_shut == 2);
  CHECK(!r.h->bound(0));
  CHECK(!r.h->bound(3));
  // an insert the render still holds, released at once (the timeout of a project load)
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  r.blocks(4);
  r.h->release_all();
  CHECK(g_shut == 3);
  const int calls = g_calls;
  r.blocks(2);
  CHECK(g_calls == calls);  // the slot was cleared: nothing calls the module that is gone
  CHECK(r.steady_near(base, 3));
}

TEST_CASE("insert host: prepare_export that cannot settle the inserts in kExportPasses boundaries says so, counts it and leaves the nets on") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  InsertHostTest::stick_leaving(*r.h, 0);
  const std::uint32_t before = r.h->blocks();
  CHECK(!r.h->prepare_export());
  CHECK(r.h->blocks() - before == InsertHost::kExportPasses);
  CHECK(InsertHost::kExportPasses == 16u);
  CHECK(r.h->stats().export_unsettled == 1);
  // the nets are on again: an insert over its budget is still forced
  g_step = 150000;
  NEED(r.bind(2, kGain, 1) == InsertHost::Bind::Ok);
  r.dc(1, 8000);
  r.blocks(16);
  CHECK(r.h->stats().forced_instance == 1);
}

TEST_CASE("insert host: a command queued for a binding that was released and whose record was bound again is dropped, the new binding is heard") {
  Rig r;
  r.dc(0, 8000);
  const int base = r.level_after_warmup();
  NEED(base > 100);
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);  // its Activate (generation 1) waits in the ring: no render is running
  r.h->release_all();
  CHECK(g_shut == 1);
  NEED(r.bind(0, kGain, 1) == InsertHost::Bind::Ok);  // the same record, generation 2
  r.blocks(4);
  InsertHost::Info info{};
  NEED(r.h->info(0, info));
  CHECK(info.gen == 2);
  CHECK(r.phase(0) == InsertHost::Phase::On);
  NEED(r.h->set_param(0, 0, 0.25f));  // sent only to a binding the render holds under its own generation
  r.blocks(3);
  CHECK(r.steady_near(base / 4, 3));
  CHECK(r.h->stats().stale == 0);
  CHECK(r.h->unbind(0));
  r.blocks(6);
  CHECK(!r.h->bound(0));
  CHECK(r.h->settled());
  CHECK(r.steady_near(base, 3));
}

TEST_CASE("insert host: a generation that wrapped round does not match the retirement of the binding it shares its number with") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  r.blocks(3);
  CHECK(r.h->unbind(0));
  r.blocks(6);
  CHECK(!r.h->bound(0));  // generation 1 retired
  InsertHostTest::set_gen(*r.h, 0, 0xFFFFu);  // 65535 bindings later: the next one is generation 1 again
  NEED(r.h->bind(0, kGain, 0x8001, nullptr, nullptr, 0, false) == InsertHost::Bind::Ok);
  r.blocks(3);
  InsertHost::Info info{};
  NEED(r.h->info(0, info));
  CHECK(info.gen == 1);
  CHECK(r.phase(0) == InsertHost::Phase::On);
  const int shut = g_shut;
  CHECK(r.h->unbind(0));
  CHECK(g_shut == shut);  // the render still holds it: nothing shuts the module down before its retirement
  CHECK(r.h->bound(0));
  r.blocks(6);
  CHECK(!r.h->bound(0));
  CHECK(g_shut == shut + 1);
}

// ---- the engine ---------------------------------------------------------------------------------------------------------------------
namespace {
std::uint32_t quiet_step(void*, std::uint64_t, std::uint32_t, audio::SeqEvent*, std::uint32_t) noexcept { return 0; }
}  // namespace

TEST_CASE("insert host: engine: attach_inserts needs the modules, the block's end drives the host, ParamKind::Insert reaches it") {
  reset_counters();
  static test::StaticSlot<audio::Engine> eslot;
  static test::StaticSlot<audio::VoicePool> pslot;
  static test::StaticSlot<ModuleVoices> mslot;
  static test::StaticSlot<InsertHost> hslot;
  static audio::TapMem taps;
  static audio::SeqEvent events[audio::kMaxEventsPerBlock];
  static InsertHost::Blobs blobs;
  alignas(16) static FxState state;
  std::memset(&state, 0, sizeof state);
  audio::Engine& e = eslot.make(&graph_clock, taps);
  audio::VoicePool& pool = pslot.make();
  ModuleVoices& mv = mslot.make();
  InsertHost& h = hslot.make();
  CHECK(!e.attach_inserts(&h, &blobs));  // no modules yet
  e.attach_voices(&pool);
  e.attach_modules(&mv);
  audio::EventSource src;
  src.step = &quiet_step;
  src.buffer = events;
  src.capacity = audio::kMaxEventsPerBlock;
  e.attach_events(src);
  NEED(e.attach_inserts(&h, &blobs));
  CHECK(e.inserts() == &h);
  NEED(h.bind(0, kGain, 0x8001, &state, nullptr, 0, false) == InsertHost::Bind::Ok);
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  ha::BlockReport report{};
  const auto block = [&]() {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0, &report};
    e.render_block(io);
    h.service();
  };
  for (int i = 0; i < 3; ++i) {
    block();
  }
  CHECK(h.blocks() == 3u);  // the boundary ran after each block
  CHECK(h.stats().activations == 1);
  NEED(h.set_param(0, 0, 0.75f));
  block();
  block();
  CHECK(h.stats().params == 1);  // through Engine::param_route
  CHECK(g_last_value == 0.75f);
  CHECK(h.stats().stale == 0);
  // unbind through the engine's boundary
  CHECK(h.unbind(0));
  for (int i = 0; i < 6; ++i) {
    block();
  }
  CHECK(!h.bound(0));
  CHECK(g_shut == 1);
  // an export whose inserts cannot settle is refused with nothing swapped; offline_leave() gives the engine back
  NEED(h.bind(0, kGain, 0x8001, nullptr, nullptr, 0, false) == InsertHost::Bind::Ok);  // record 0 again (it holds the module and its state block)
  InsertHostTest::stick_leaving(h, 0);
  e.offline_request();
  NEED(e.offline_ready(false));
  CHECK(!e.offline_enter(src));
  CHECK(!e.offline_entered());
  CHECK(h.stats().export_unsettled == 1);
  e.offline_leave();
  CHECK(!e.offline_wanted());
}

// ---- S1 review 2: no insert bound is the engine without the host; no allocation in the render; commands at every phase; the S ramp -------------------------------
namespace {

// A noise block, the same sequence every run.
struct Lcg {
  std::uint32_t s;
  std::uint32_t next() noexcept {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
  std::uint32_t below(std::uint32_t n) noexcept { return next() % n; }
};
void noise(Lcg& rng, std::int16_t* p, std::uint32_t n, int amp) {
  for (std::uint32_t i = 0; i < n; ++i) {
    p[i] = static_cast<std::int16_t>(static_cast<int>(rng.below(static_cast<std::uint32_t>(2 * amp + 1))) - amp);
  }
}

// One Engine, with or without the insert host (attach_inserts), built in its own static place (N): the engines of a case are alive at once.
struct EngineRig {
  audio::Engine* e = nullptr;
  InsertHost* h = nullptr;
  audio::EventSource src{};
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  alignas(8) std::int16_t feed[6][kB] = {};
  ha::BlockReport report{};
  void block() {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0, &report};
    e->render_block(io);
    if (h != nullptr) {
      h->service();
    }
  }
};
std::uint32_t quiet_events(void*, std::uint64_t, std::uint32_t, audio::SeqEvent*, std::uint32_t) noexcept { return 0; }

template <int N>
void make_engine(EngineRig& r, bool with_host) {
  static test::StaticSlot<audio::Engine> eslot;
  static test::StaticSlot<audio::VoicePool> pslot;
  static test::StaticSlot<ModuleVoices> mslot;
  static test::StaticSlot<InsertHost> hslot;
  static audio::TapMem taps;
  static audio::SeqEvent events[audio::kMaxEventsPerBlock];
  static InsertHost::Blobs blobs;
  r.e = &eslot.make(&graph_clock, taps);
  audio::VoicePool& pool = pslot.make();
  ModuleVoices& mv = mslot.make();
  r.e->attach_voices(&pool);
  r.e->attach_modules(&mv);
  r.src.step = &quiet_events;
  r.src.buffer = events;
  r.src.capacity = audio::kMaxEventsPerBlock;
  r.e->attach_events(r.src);
  if (with_host) {
    r.h = &hslot.make();
    std::memset(blobs, 0, sizeof blobs);
    CHECK(r.e->attach_inserts(r.h, &blobs));
  }
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.e->graph().params().push(core::ParamChange{audio::strip_param(t, gps::Source), 3.0f, 1}));
    r.e->graph().feed(t, r.feed[t]);
  }
}
}  // namespace

TEST_CASE("insert host: with no insert bound the engine's output is bit for bit the engine without the host") {
  reset_counters();
  EngineRig plain;
  EngineRig hosted;
  make_engine<0>(plain, false);
  make_engine<1>(hosted, true);
  Lcg rng{777u};
  for (unsigned b = 0; b < 160; ++b) {
    for (unsigned t = 0; t < 6; ++t) {
      noise(rng, plain.feed[t], kB, 6000);
      std::memcpy(hosted.feed[t], plain.feed[t], sizeof plain.feed[t]);
    }
    noise(rng, plain.in, kBS, 3000);
    std::memcpy(hosted.in, plain.in, sizeof plain.in);
    if (b == 40u) {  // a change of the strips' parameters in the middle, the same on both
      for (EngineRig* r : {&plain, &hosted}) {
        CHECK(r->e->graph().params().push(core::ParamChange{audio::strip_param(2, gps::Pan), 20.0f, 8}));
        CHECK(r->e->graph().params().push(core::ParamChange{audio::strip_param(3, gps::Volume), 60.0f, 8}));
      }
    }
    plain.block();
    hosted.block();
    INFO("block " << b);
    CHECK(std::memcmp(plain.out, hosted.out, sizeof plain.out) == 0);
  }
  CHECK(hosted.h->blocks() == 160u);  // the boundary did run
  CHECK(hosted.h->stats().activations == 0);
  CHECK(g_calls == 0);
}

TEST_CASE("insert host: the render path allocates nothing with inserts bound, running, ramping, bypassed, forced and retiring") {
  // the counter works: a direct call of the replaceable operator new inside a window is seen
  insert_alloc::count = 0;
  {
    insert_alloc::Window w;
    void* p = ::operator new(16);
    ::operator delete(p);
  }
  NEED(insert_alloc::count == 1u);
  insert_alloc::count = 0;
  Rig r;
  r.dc(0, 8000);
  r.dc(1, 6000);
  r.dc(2, 5000);
  r.blocks(6);
  const float half = 0.5f;
  NEED(r.bind(0, kGain, 0, &half, 1) == InsertHost::Bind::Ok);
  NEED(r.bind(2, kLp, 1) == InsertHost::Bind::Ok);
  NEED(r.bind(4, kLoud, 2) == InsertHost::Bind::Ok);  // clips: the saturation count in the adapter
  r.blocks(4);                                        // ramping in, then running
  CHECK(r.h->stats().saturated > 0u);
  NEED(r.h->set_param(0, 0, 0.25f));
  NEED(r.h->set_param(2, 0, 0.75f));
  r.blocks(4);
  CHECK(r.h->set_bypass(0, true) == InsertHost::Toggle::Done);  // a ramp to dry
  r.blocks(4);
  CHECK(r.h->set_bypass(0, false) == InsertHost::Toggle::Done);  // a reset and a ramp in
  r.blocks(8);
  CHECK(r.h->unbind(2));  // a ramp out and the retirement
  r.blocks(8);
  r.fill(1, 0);  // silence on two tracks
  r.fill(2, 0);
  r.blocks(4);
  g_step = 150000;  // over the instance budget: the net forces the remaining inserts to dry
  r.fill(0, 8000);
  r.blocks(12);
  CHECK(r.h->stats().forced_instance + r.h->stats().forced_aggregate > 0u);
  g_step = 0;
  CHECK(r.h->unbind(0));
  CHECK(r.h->unbind(4));
  r.blocks(10);
  CHECK(r.h->settled());
  CHECK(r.h->stats().retired >= 3u);
  CHECK(insert_alloc::count == 0u);
  // a block of the Engine too: the boundary call and the param route
  EngineRig er;
  make_engine<2>(er, true);
  alignas(16) static FxState state;
  std::memset(&state, 0, sizeof state);
  NEED(er.h->bind(0, kGain, 0x8001, &state, nullptr, 0, false) == InsertHost::Bind::Ok);
  Lcg rng{5u};
  insert_alloc::count = 0;
  for (unsigned b = 0; b < 12; ++b) {
    noise(rng, er.feed[0], kB, 6000);
    if (b == 5u) {
      NEED(er.h->set_param(0, 0, 0.5f));
    }
    {
      insert_alloc::Window w;
      er.report = ha::BlockReport{};
      const ha::BlockIo io{er.in, er.out, 0, &er.report};
      er.e->render_block(io);
    }
    er.h->service();
  }
  CHECK(er.h->stats().params == 1u);
  CHECK(insert_alloc::count == 0u);
}

// ---- main-loop commands at every phase of a block ----------------------------------------------------------------------------------
namespace {
unsigned g_viol = 0;        // the module saw itself used outside its life: render or set_param of a shut-down state, init of a live one, shutdown of a dead one
void (*g_mid)() = nullptr;  // called by the module's render: the main loop's preemption BETWEEN strips
int chaos_init(void* s, const o3m_host*) {
  FxState* d = static_cast<FxState*>(s);
  if (d->pad == 1u) {
    ++g_viol;
  }
  d->pad = 1u;
  d->gain = 1.0f;
  ++g_init;
  return 0;
}
void chaos_shutdown(void* s) {
  FxState* d = static_cast<FxState*>(s);
  if (d->pad != 1u) {
    ++g_viol;
  }
  d->pad = 0u;
  ++g_shut;
}
void chaos_set_param(void* s, std::uint32_t, float) {
  if (static_cast<FxState*>(s)->pad != 1u) {
    ++g_viol;
  }
  ++g_params;
}
std::uint32_t chaos_render(void* s, const float* in, float* out, std::uint32_t n) {  // the identity: any wet / dry mix of it is the dry signal
  if (static_cast<FxState*>(s)->pad != 1u) {
    ++g_viol;
  }
  if (g_mid != nullptr) {
    g_mid();
  }
  ++g_calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = in[i];
  }
  return 1;
}
const o3m_module kChaos = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), "chaos", sizeof(FxState), 1, kFxParams, chaos_init, chaos_render, chaos_set_param, nullptr, nullptr, chaos_shutdown, nullptr, 0};

struct Chaos {
  Rig* r = nullptr;
  Lcg rng{1u};
  unsigned states = 0;     // state blocks handed out (a record that held the module keeps its own)
  unsigned ops = 0;
  unsigned in_render = 0;  // commands that ran inside the strip loop
  void bind(unsigned where) {
    InsertHost::Bind b = r->h->bind(where, kChaos, 0x8001, nullptr, nullptr, 0, false);
    if (b == InsertHost::Bind::NeedState && states < InsertHost::kInstances + 1u) {
      b = r->h->bind(where, kChaos, 0x8001, &r->st[states++], nullptr, 0, false);
    }
    (void)b;  // refused (taken, no instance, no room): a legal answer, nothing changes
  }
  void op() {
    ++ops;
    const unsigned where = rng.below(2u) == 0u ? 0u : 30u;  // track 1 slot A, track 16 slot A: the first and the last strip of the pass
    switch (rng.below(7u)) {
      case 0:
      case 1: bind(where); break;
      case 2: (void)r->h->unbind(where); break;
      case 3: (void)r->h->set_bypass(where, true); break;
      case 4: (void)r->h->set_bypass(where, false); break;
      default: (void)r->h->set_param(where, 0u, static_cast<float>(rng.below(100u))); break;
    }
  }
  void some() {
    const unsigned n = 1u + rng.below(3u);
    for (unsigned i = 0; i < n; ++i) {
      op();
    }
  }
};
Chaos* g_chaos = nullptr;
unsigned g_mid_calls = 0;
void chaos_mid() {
  ++g_mid_calls;
  if (g_mid_calls <= 2u && g_chaos != nullptr && g_chaos->rng.below(2u) == 0u) {  // the first and the second insert call of the block: strips remain after the first
    ++g_chaos->in_render;
    g_chaos->some();
  }
}

// `blocks` blocks of noise on three tracks, recorded; `chaos` (or null) sends commands before the render, between the strips (from the module), after the strips and before the boundary, and after it.
std::vector<std::int16_t> run_noise(Rig& r, Chaos* chaos, unsigned blocks) {
  std::vector<std::int16_t> rec;
  rec.reserve(static_cast<std::size_t>(blocks) * kBS);
  r.dc(0, 0);
  r.dc(15, 0);
  r.dc(1, 0);
  Lcg rng{4242u};
  for (unsigned b = 0; b < blocks; ++b) {
    noise(rng, r.feed[0], kB, 7000);
    noise(rng, r.feed[15], kB, 7000);
    noise(rng, r.feed[1], kB, 3000);
    r.report = ha::BlockReport{};
    const ha::BlockIo io{r.in, r.out, 0, &r.report};
    if (chaos != nullptr && chaos->rng.below(2u) == 0u) {  // before the block: after the previous boundary and tick
      chaos->some();
    }
    g_mid_calls = 0;
    {
      insert_alloc::Window w;
      r.g->render(io);
    }
    if (chaos != nullptr && chaos->rng.below(2u) == 0u) {  // after the strips, before the block's end
      chaos->some();
    }
    {
      insert_alloc::Window w;
      r.h->begin_block();
    }
    if (chaos != nullptr && chaos->rng.below(4u) == 0u) {  // after the boundary, before the main loop's tick
      chaos->some();
    }
    r.h->service();
    rec.insert(rec.end(), r.out, r.out + kBS);
  }
  return rec;
}
}  // namespace

TEST_CASE("insert host: bind, unbind, bypass and parameter commands before the render, between the strips and before the block's end: nothing torn, an identity effect never changes the sound, shutdown only when clear") {
  std::vector<std::int16_t> reference;
  {
    Rig r;  // no insert is ever bound
    reference = run_noise(r, nullptr, 400);
  }
  Rig r;
  Chaos c;
  c.r = &r;
  c.rng = Lcg{99u};
  g_chaos = &c;
  g_mid = &chaos_mid;
  g_viol = 0;
  insert_alloc::count = 0;
  const std::vector<std::int16_t> rec = run_noise(r, &c, 400);
  g_mid = nullptr;
  g_chaos = nullptr;
  NEED(rec.size() == reference.size());
  std::size_t first_diff = rec.size();
  for (std::size_t i = 0; i < rec.size(); ++i) {
    if (rec[i] != reference[i]) {
      first_diff = i;
      break;
    }
  }
  CHECK(first_diff == rec.size());  // identity through every transition: the ramps mix a signal with itself
  CHECK(g_viol == 0u);
  CHECK(insert_alloc::count == 0u);
  CHECK(c.ops > 200u);
  CHECK(c.in_render > 20u);  // the commands did land between strips
  CHECK(r.h->stats().activations >= 10u);
  CHECK(r.h->stats().params + r.h->stats().stale > 0u);
  // everything leaves, and the host is quiescent with every init matched by a shutdown
  (void)r.h->unbind(0);
  (void)r.h->unbind(30);
  r.blocks(16);
  CHECK(r.h->settled());
  CHECK(!r.h->bound(0));
  CHECK(!r.h->bound(30));
  CHECK(g_init == g_shut);
  CHECK(g_viol == 0u);
}

// ---- the stereo S over one block ---------------------------------------------------------------------------------------------------
TEST_CASE("insert host: the stereo S leaves and returns over one block, never as a step: bind, bypass on and off, unbind (slot clear)") {
  Rig r;
  r.dc(0, 8000);
  r.dc_side(0, 6000);
  r.blocks(10);
  // L = M + S, R = M - S at the centre: the S the strip carries is (L - R) / 2, the M (L + R) / 2
  const auto s_of = [&](unsigned i) { return (static_cast<int>(r.out[2u * i]) - static_cast<int>(r.out[2u * i + 1u])) / 2; };
  const auto m_of = [&](unsigned i) { return (static_cast<int>(r.out[2u * i]) + static_cast<int>(r.out[2u * i + 1u])) / 2; };
  const int s_full = s_of(kB - 1);
  const int m_full = m_of(kB - 1);
  NEED(s_full > 1000);
  NEED(m_full > 1000);
  std::vector<int> s_seq;
  std::vector<int> m_seq;
  const auto take = [&]() {
    r.block();
    for (std::uint32_t i = 0; i < kB; ++i) {
      s_seq.push_back(s_of(i));
      m_seq.push_back(m_of(i));
    }
  };
  float one = 1.0f;
  NEED(r.bind(0, kGain, 0, &one, 1) == InsertHost::Bind::Ok);  // a gain of 1: the identity on M
  for (int i = 0; i < 4; ++i) {
    take();
  }
  CHECK(std::abs(s_seq.back()) <= 1);  // the insert is in the path: S is gone
  CHECK(r.phase(0) == InsertHost::Phase::On);
  CHECK(r.h->set_bypass(0, true) == InsertHost::Toggle::Done);
  for (int i = 0; i < 4; ++i) {
    take();
  }
  CHECK(std::abs(s_seq.back() - s_full) <= 2);  // bypassed: S is back
  CHECK(r.h->set_bypass(0, false) == InsertHost::Toggle::Done);  // a reset: shutdown, init, the activation again
  for (int i = 0; i < 8; ++i) {
    take();
  }
  CHECK(std::abs(s_seq.back()) <= 1);
  CHECK(r.h->unbind(0));  // the ramp out, then the slot is cleared
  for (int i = 0; i < 8; ++i) {
    take();
  }
  CHECK(!r.h->bound(0));
  CHECK(std::abs(s_seq.back() - s_full) <= 2);
  // no step anywhere, the four ramps are four blocks, in the order: out of S, back, out of S, back
  const int max_step = s_full / 40 + 2;
  int steps_over = 0;
  int mid = 0;  // samples strictly between 5 % and 95 % of the full S: a one-block ramp has about 58 of them
  int direction = 0;
  int turns = 0;
  for (std::size_t i = 1; i < s_seq.size(); ++i) {
    const int d = s_seq[i] - s_seq[i - 1];
    steps_over += std::abs(d) > max_step ? 1 : 0;
    mid += (s_seq[i] > s_full / 20 && s_seq[i] < s_full - s_full / 20) ? 1 : 0;
    if (d != 0) {
      const int dir = d > 0 ? 1 : -1;
      turns += (direction != 0 && dir != direction) ? 1 : 0;
      direction = dir;
    }
  }
  CHECK(steps_over == 0);
  CHECK(mid >= 4 * 56);
  CHECK(mid <= 4 * 64);
  CHECK(turns == 3);  // down, up, down, up: monotone inside each ramp
  // the M the identity effect carries is untouched throughout (the S ramp and the dry / wet ramp are two separate things)
  int worst = 0;
  for (const int m : m_seq) {
    worst = std::max(worst, std::abs(m - m_full));
  }
  CHECK(worst <= 2);
}
