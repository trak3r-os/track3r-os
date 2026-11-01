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
  // What the nets and admit() judge as the render's other work, and an insert made to look as if it had been measured (its previous block's cost and an estimate).
  static std::uint32_t others(const InsertHost& h) { return h.others_pub_.load(std::memory_order_relaxed); }
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
  // A block as the Engine closes it with the render's other work in the governor's base: `others` cycles besides the inserts (the sampler voices), `modules` of instrument-module voices, then what the inserts
  // took. (The block's total is the governor's whole figure: the net's others count the module voices.)
  void gblock(std::uint32_t others, std::uint32_t modules = 0) {
    g_block_cycles = 0;
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0, &report};
    {
      insert_alloc::Window w;
      g->render(io);
      mv->governor().observe_block(others + modules + g_block_cycles);
      h->begin_block();
    }
    h->service();
  }
  void gblocks(unsigned n, std::uint32_t others, std::uint32_t modules = 0) {
    for (unsigned i = 0; i < n; ++i) {
      gblock(others, modules);
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
  CHECK(r.h->set_bypass(0, false) == InsertHost::Toggle::NoCpu);  // its measured cost is over the instance budget: the net would force it again, so admit() says so
  CHECK(r.h->set_bypass(0, false, true) == InsertHost::Toggle::Done);
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

// OS-INSFX S2 unit regression (Dense8: render mean about 86 %, peaks past 100 %): the governor's base is a moving MAX, so on a busy project that plays without underruns it sits at the peak (over the
// 90 % threshold), the aggregate allowance was 0 and a 1.3 % insert was forced to dry. The net judges the mean of the render's other work (spec 6).
TEST_CASE("insert host: aggregate net: one cheap insert on a busy project that plays (mean 86 %, peaks 117 %) is not forced; inserts that push the mean over the threshold are") {
  namespace gov = o3os::audio::gov;
  Rig r;
  r.dc(0, 8000);
  // Dense8's figures: the render's other work is 77 % in three blocks of four and 117 % in the fourth (mean 87 %, peaks over the block, as the unit's `cpu` read)
  const std::uint32_t lo2 = gov::cycles_of_percent(77);
  const std::uint32_t hi = gov::cycles_of_percent(117);
  r.gblocks(8, lo2);
  g_cost_a = gov::kBlockCycles * 13u / 1000u;  // 1.3 %
  NEED(r.bind(0, kCostA, 0, nullptr, 0, true) == InsertHost::Bind::Ok);
  for (unsigned i = 0; i < 600; ++i) {  // 2 s of the pattern
    r.gblock(i % 4u == 3u ? hi : lo2);
  }
  CHECK(r.phase(0) == InsertHost::Phase::On);
  CHECK(r.h->stats().forced_aggregate == 0);
  CHECK(r.h->stats().forced_instance == 0);
  // the same insert on a render whose other work is itself over the threshold (mean 95 %): the inserts push the total over, and the net forces it
  Rig q;
  q.dc(0, 8000);
  const std::uint32_t busy = gov::cycles_of_percent(95);
  q.gblocks(8, busy);
  g_cost_a = gov::kBlockCycles * 13u / 1000u;
  NEED(q.bind(0, kCostA, 0, nullptr, 0, true) == InsertHost::Bind::Ok);
  q.gblocks(40, busy);
  CHECK(q.h->stats().forced_aggregate == 1);
}

// The rising edge: inserts loaded unchecked on a light render (30 %), which then turns heavy (70 %): the mean of the other work follows in tens of blocks, so the net acts within about 40 and not after 110.
TEST_CASE("insert host: aggregate net: a light render that turns heavy with four inserts of 40 % in all: the net forces within 40 blocks") {
  namespace gov = o3os::audio::gov;
  Rig r;
  for (unsigned t = 0; t < 4; ++t) {
    r.dc(t, 8000);
  }
  const std::uint32_t light = gov::cycles_of_percent(30);
  const std::uint32_t heavy = gov::cycles_of_percent(70);
  r.gblocks(400, light);
  g_cost_a = gov::cycles_of_percent(10);
  for (unsigned t = 0; t < 4; ++t) {
    NEED(r.bind(2u * t, kCostA, t, nullptr, 0, true) == InsertHost::Bind::Ok);
  }
  r.gblocks(40, light);
  CHECK(r.h->stats().forced_aggregate == 0);  // 70 % of the block in all: it fits
  unsigned n = 0;
  while (n < 40u && r.h->stats().forced_aggregate == 0u) {
    r.gblock(heavy);
    ++n;
  }
  CHECK(r.h->stats().forced_aggregate >= 1);
  CHECK(n <= 40u);
  r.gblocks(300, heavy);  // what is left fits under the threshold, and nothing is forced for ever
  CHECK(gov::cycles_of_percent(70) + r.h->total_cycles() <= r.mv->governor().threshold());
  CHECK(r.h->total_cycles() > 0u);
}

// Dense8's figures (other work 77 % in three blocks of four and 117 % in the fourth, mean 87 %): admit() takes the same rule as the net, so a 1.3 % bind is admitted and a 15 % bind is refused, though the
// governor's base (a moving max) is over the threshold.
TEST_CASE("insert host: admit() judges the mean of the other work, as the net: on a busy project (mean 87 %, peaks 117 %) a 1.3 % bind is admitted and a 15 % bind is refused") {
  namespace gov = o3os::audio::gov;
  Rig r;
  r.dc(0, 8000);
  r.dc(1, 8000);
  constexpr std::uint32_t lo = gov::cycles_of_percent(77);
  constexpr std::uint32_t hi = gov::cycles_of_percent(117);
  auto pattern = [&r](unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      r.gblock(i % 4u == 3u ? hi : lo);
    }
  };
  pattern(600);
  CHECK(r.mv->governor().base() > r.mv->governor().threshold());  // the peak: what admit() used to read
  // The estimate admit() takes for a record that held the module is its measured figure: measure A (1.3 %) and B (15 %) by running them unchecked, then free both records.
  g_cost_a = gov::kBlockCycles * 13u / 1000u;
  g_cost_b = gov::cycles_of_percent(15);
  NEED(r.bind(0, kCostA, 0, nullptr, 0, true) == InsertHost::Bind::Ok);
  NEED(r.bind(2, kCostB, 1, nullptr, 0, true) == InsertHost::Bind::Ok);
  pattern(16);
  CHECK(r.h->cost(0) == g_cost_a);
  CHECK(r.h->cost(2) == g_cost_b);
  CHECK(r.h->unbind(0));
  CHECK(r.h->unbind(2));
  pattern(8);
  CHECK(!r.h->bound(0));
  CHECK(!r.h->bound(2));
  CHECK(r.h->bind(2, kCostB, 0x8001, nullptr, nullptr, 0, false) == InsertHost::Bind::NoCpu);  // 87 + 15 is over the threshold
  CHECK(!r.h->bound(2));
  const unsigned forced = r.h->stats().forced_aggregate;
  CHECK(r.h->bind(0, kCostA, 0x8001, nullptr, nullptr, 0, false) == InsertHost::Bind::Ok);  // 87 + 1.3 is not
  pattern(600);
  CHECK(r.h->stats().forced_aggregate == forced);  // admitted exactly when the net does not force it
  CHECK(r.phase(0) == InsertHost::Phase::On);
}

// OS-INSFX first bind (spec 6): a module that was never measured is counted as 0 at its first bind, then measured, and the nets force it if it overloads. On Dense8's trace (77 / 77 / 77 / 117 %, mean 87 %)
// it is admitted; on a render whose other work is itself over the threshold it is refused.
TEST_CASE("insert host: a first bind of a module never measured is admitted when the mean of the other work leaves room (Dense8), refused when it is over the threshold; the nets judge it after") {
  namespace gov = o3os::audio::gov;
  constexpr std::uint32_t lo = gov::cycles_of_percent(77);
  constexpr std::uint32_t hi = gov::cycles_of_percent(117);
  Rig r;
  r.dc(0, 8000);
  for (unsigned i = 0; i < 600; ++i) {
    r.gblock(i % 4u == 3u ? hi : lo);
  }
  g_cost_a = gov::cycles_of_percent(30);  // heavy: 87 + 30 is over the threshold, the cold figure (10 %) would refuse it too
  CHECK(r.h->bind(0, kCostA, 0x8001, &r.st[0], nullptr, 0, false) == InsertHost::Bind::Ok);
  for (unsigned i = 0; i < 80; ++i) {
    r.gblock(i % 4u == 3u ? hi : lo);
  }
  CHECK(r.h->stats().forced_aggregate >= 1);  // measured, then forced to dry by the aggregate net
  CHECK(r.phase(0) == InsertHost::Phase::Bypassed);
  // an un-bypass keeps the rule: 87 + its measured 30 % is over the threshold, so it needs force
  CHECK(r.h->set_bypass(0, false) == InsertHost::Toggle::NoCpu);
  // the other work over the threshold (mean 95 %): refused
  Rig q;
  q.dc(0, 8000);
  q.gblocks(600, gov::cycles_of_percent(95));
  g_cost_a = gov::kBlockCycles * 13u / 1000u;
  CHECK(q.h->bind(0, kCostA, 0x8001, &q.st[0], nullptr, 0, false) == InsertHost::Bind::NoCpu);
  CHECK(!q.h->bound(0));
}

// OS-INSFX module voices (spec 6): the net's "others" is all the render work but the inserts, the instrument-module voices' cycles included.
TEST_CASE("insert host: the aggregate net counts the module voices: voices 30 % + others 55 % force a 10 % insert, voices 0 % leave it on") {
  namespace gov = o3os::audio::gov;
  constexpr std::uint32_t others = gov::cycles_of_percent(55);
  {
    Rig r;
    g_cost_a = gov::cycles_of_percent(10);  // after the Rig: its constructor resets the counters
    r.dc(0, 8000);
    r.gblocks(8, others, gov::cycles_of_percent(30));
    NEED(r.bind(0, kCostA, 0, nullptr, 0, true) == InsertHost::Bind::Ok);
    r.gblocks(120, others, gov::cycles_of_percent(30));  // 55 + 30 + 10 = 95 % of the block: over the threshold
    CHECK(r.h->stats().forced_aggregate >= 1);
    CHECK(r.phase(0) == InsertHost::Phase::Bypassed);
  }
  {
    Rig r;
    g_cost_a = gov::cycles_of_percent(10);
    r.dc(0, 8000);
    r.gblocks(8, others, 0);
    NEED(r.bind(0, kCostA, 0, nullptr, 0, true) == InsertHost::Bind::Ok);
    r.gblocks(120, others, 0);  // 65 %
    CHECK(r.h->stats().forced_aggregate == 0);
    CHECK(r.phase(0) == InsertHost::Phase::On);
  }
}

TEST_CASE("insert host: a re-bind or an un-bypass right after the insert stopped is admitted: the stopped insert is not counted") {
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

TEST_CASE("insert host: an export keeps the mean of the render's other work: a cold block after it does not move what the net judges") {
  namespace gov = o3os::audio::gov;
  Rig r;
  r.dc(0, 8000);
  r.gblocks(400, gov::cycles_of_percent(50));
  const std::uint32_t before = InsertHostTest::others(*r.h);
  CHECK(before >= gov::cycles_of_percent(49));
  CHECK(before <= gov::cycles_of_percent(50));
  NEED(r.h->prepare_export());
  r.blocks(4);  // (the nets are off: the means are not followed)
  r.h->export_end();
  CHECK(InsertHostTest::others(*r.h) == before);  // not reseeded
  r.gblock(gov::cycles_of_percent(150));  // one cold block
  CHECK(InsertHostTest::others(*r.h) < gov::cycles_of_percent(52));
}

// A first block that is cold (boot, 150 %) seeds the means at the threshold at most, and the next blocks average in at once: a 5 % insert that was On is not forced for a render that is really 60 %.
TEST_CASE("insert host: the first block seeds the mean at the threshold at most, and is forgotten as the next blocks come in") {
  namespace gov = o3os::audio::gov;
  Rig r;
  r.dc(0, 8000);
  g_cost_a = gov::cycles_of_percent(5);
  NEED(r.bind(0, kCostA, 0, nullptr, 0, true) == InsertHost::Bind::Ok);
  r.gblock(gov::cycles_of_percent(150));
  CHECK(InsertHostTest::others(*r.h) <= r.mv->governor().threshold());
  CHECK(InsertHostTest::others(*r.h) >= gov::cycles_of_percent(85));
  r.gblocks(60, gov::cycles_of_percent(60));
  CHECK(InsertHostTest::others(*r.h) < gov::cycles_of_percent(65));
  CHECK(r.h->stats().forced_aggregate == 0);
  CHECK(r.phase(0) == InsertHost::Phase::On);
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

// ---- OS-INSFX S2: the effect's tail -------------------------------------------------------------------------------------------------
// A feedback-like effect: the input plus a ring (`y`, set when the input is not silent) that decays 10 % a block. `ring_render` says idle once the ring is under 0.001 (a real effect's rule);
// `ring1_render` always says 1 (an effect that never says it is done, the zero-output rule's case); `stuck_render` is a constant 0.1 and always 1 (a faulty module, the cap's case).
namespace {
int g_ring_audible = 0;  // renders whose output block had a sample the Q15 conversion keeps (|y| * 32768 >= 1)
std::uint32_t ring_core(void* s, const float* in, float* out, std::uint32_t n) {
  FxState* d = static_cast<FxState*>(s);
  ++g_calls;
  ++d->renders;
  bool any = false;
  for (std::uint32_t i = 0; i < n; ++i) {
    any = any || in[i] != 0.0f;
  }
  if (any) {
    d->y = 0.3f;
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = in[i] + d->y;
  }
  g_ring_audible += (d->y * 32768.0f >= 1.0f) ? 1 : 0;
  d->y *= 0.9f;
  return 1;
}
std::uint32_t ring_render(void* s, const float* in, float* out, std::uint32_t n) {
  const std::uint32_t r = ring_core(s, in, out, n);
  return static_cast<FxState*>(s)->y > 0.001f ? r : 0u;
}
std::uint32_t ring1_render(void* s, const float* in, float* out, std::uint32_t n) { return ring_core(s, in, out, n); }
std::uint32_t stuck_render(void*, const float*, float* out, std::uint32_t n) {
  ++g_calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = 0.1f;
  }
  return 1;
}
// A delay's echo: the input arms a countdown of kEchoBlocks (about 300 ms) that only runs while the input is silent; at zero the effect adds one 0.3 block (the echo) and says idle. Between the stop
// and the echo the output is exactly 0 for kEchoBlocks - 1 blocks, with the effect saying 1.
constexpr std::uint32_t kEchoBlocks = 207;
int g_echoes = 0;
std::uint32_t echo_render(void* s, const float* in, float* out, std::uint32_t n) {
  FxState* d = static_cast<FxState*>(s);
  ++g_calls;
  ++d->renders;
  bool any = false;
  for (std::uint32_t i = 0; i < n; ++i) {
    any = any || in[i] != 0.0f;
  }
  if (any) {
    d->pad = kEchoBlocks;
  }
  float e = 0.0f;
  if (d->pad > 0u && --d->pad == 0u) {
    e = 0.3f;
    ++g_echoes;
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = in[i] + e;
  }
  return d->pad > 0u ? 1u : 0u;
}
// A 100 % wet effect with a pre-delay of kPreBlocks blocks (a ring of the blocks' first sample): its output is exactly 0 for kPreBlocks blocks after the source starts, with the source live.
constexpr unsigned kPreBlocks = 60;
float g_pre_line[128];
unsigned g_pre_head = 0;
std::uint32_t predelay_render(void*, const float* in, float* out, std::uint32_t n) {
  ++g_calls;
  g_pre_line[g_pre_head % 128u] = in[0];
  const float v = g_pre_line[(g_pre_head + 128u - kPreBlocks) % 128u];
  ++g_pre_head;
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = v;
  }
  for (const float x : g_pre_line) {
    if (x != 0.0f) {
      return 1u;
    }
  }
  return 0u;
}
const o3m_module kEcho = FX_MODULE("echo", O3M_KIND_EFFECT, echo_render);
const o3m_module kPre = FX_MODULE("pre", O3M_KIND_EFFECT, predelay_render);
const o3m_module kRing = FX_MODULE("ring", O3M_KIND_EFFECT, ring_render);
const o3m_module kRing1 = FX_MODULE("ring1", O3M_KIND_EFFECT, ring1_render);
const o3m_module kStuck = FX_MODULE("stuck", O3M_KIND_EFFECT, stuck_render);

int peak_of(const Rig& r) {
  int p = 0;
  for (std::uint32_t i = 0; i < kBS; ++i) {
    p = std::max(p, std::abs(static_cast<int>(r.out[i])));
  }
  return p;
}
// Blocks until `cap` have run; the number of them in which the module was called.
unsigned run_counting(Rig& r, unsigned cap) {
  unsigned called = 0;
  for (unsigned i = 0; i < cap; ++i) {
    const int before = g_calls;
    r.block();
    called += g_calls != before ? 1u : 0u;
  }
  return called;
}
}  // namespace

TEST_CASE("insert host: tail: a decaying effect rings out after the source stops, then the strip goes idle and its cost returns to the idle figure") {
  Rig r;
  r.dc(0, 8000);
  r.g->feed(0, nullptr);  // the source is gone: the strip is idle
  r.blocks(4);
  const std::uint32_t idle_cost = r.report.stage_cycles[ha::kStageStrips];
  CHECK(peak_of(r) == 0);
  r.g->feed(0, r.feed[0]);
  NEED(r.bind(0, kRing, 0) == InsertHost::Bind::Ok);
  r.blocks(8);  // the effect hears the source
  CHECK(g_calls > 0);
  CHECK(peak_of(r) > 1000);
  r.g->feed(0, nullptr);  // the notes stop
  const int calls0 = g_calls;
  r.block();
  CHECK(g_calls == calls0 + 1);  // the strip stayed live for the tail
  CHECK(peak_of(r) > 1000);
  const std::uint32_t tail_cost = r.report.stage_cycles[ha::kStageStrips];
  CHECK(tail_cost > idle_cost);
  r.blocks(9);
  CHECK(peak_of(r) > 100);  // 0.3 * 0.9^10 is about 0.1 of full scale: still ringing
  const unsigned called = run_counting(r, 200) + 10u;
  CHECK(called >= 50u);  // the ring is under 0.001 after about 54 blocks
  CHECK(called <= 58u);
  const int calls1 = g_calls;
  r.blocks(20);
  CHECK(g_calls == calls1);  // idle: not called
  CHECK(peak_of(r) == 0);
  CHECK(r.report.stage_cycles[ha::kStageStrips] == idle_cost);  // the strip's cost is the idle figure again
  // the signal returns: the effect is called again
  r.g->feed(0, r.feed[0]);
  r.block();
  CHECK(g_calls == calls1 + 1);
  CHECK(peak_of(r) > 1000);
}

TEST_CASE("insert host: tail: an effect whose output is exact zero for 2 s of silent source stops being run, though it still says 1") {
  Rig r;
  r.dc(0, 8000);
  NEED(r.bind(0, kRing1, 0) == InsertHost::Bind::Ok);
  r.blocks(8);
  r.g->feed(0, nullptr);
  g_ring_audible = 0;
  const int calls0 = g_calls;
  const float seconds = static_cast<float>(InsertHost::kTailZeroBlocks) * static_cast<float>(kB) / o3os::core::kRefRateHz;
  CHECK(seconds > 1.99f);
  CHECK(seconds < 2.01f);
  r.blocks(InsertHost::kTailZeroBlocks + 150u);
  CHECK(g_ring_audible > 50);  // 0.3 * 0.9^n >= 1 / 32768 until about n = 87
  CHECK(g_calls - calls0 == g_ring_audible + static_cast<int>(InsertHost::kTailZeroBlocks));
  CHECK(peak_of(r) == 0);
}

TEST_CASE("insert host: tail: a delay's echo after a gap of 300 ms of exact zero is rendered, and again after the next note") {
  Rig r;
  r.dc(0, 8000);
  g_echoes = 0;
  NEED(r.bind(0, kEcho, 0) == InsertHost::Bind::Ok);
  r.blocks(8);
  CHECK(g_echoes == 0);
  for (unsigned round = 1; round <= 2; ++round) {
    r.g->feed(0, nullptr);  // the note stops: more than 32 blocks of exact zero follow before the echo
    unsigned loud = 0;
    unsigned zero_run = 0;
    unsigned longest_zero = 0;
    for (unsigned i = 0; i < 300; ++i) {
      r.block();
      const bool is_loud = peak_of(r) > 1000;
      loud += is_loud ? 1u : 0u;
      zero_run = peak_of(r) == 0 ? zero_run + 1u : 0u;
      longest_zero = std::max(longest_zero, zero_run);
    }
    CHECK(longest_zero > 32u);
    CHECK(g_echoes == static_cast<int>(round));
    CHECK(loud == 1u);  // the echo block
    const int calls = g_calls;
    r.blocks(20);
    CHECK(g_calls == calls);  // the effect said idle after the echo: the strip is idle
    r.g->feed(0, r.feed[0]);
    r.blocks(8);
  }
}

TEST_CASE("insert host: tail: a 100 % wet effect with a pre-delay keeps its tail when the source stops before the delayed copy arrives") {
  Rig r;
  r.dc(0, 8000);
  std::memset(g_pre_line, 0, sizeof g_pre_line);
  g_pre_head = 0;
  NEED(r.bind(0, kPre, 0) == InsertHost::Bind::Ok);
  r.blocks(2);
  const int calls0 = g_calls;
  while (g_calls - calls0 < 40) {  // 40 blocks live: the output is exactly 0 for all of them (more than 32), the source is live
    r.block();
    CHECK(peak_of(r) == 0);
  }
  r.g->feed(0, nullptr);
  unsigned loud = 0;
  for (unsigned i = 0; i < 200; ++i) {  // (the line holds 128 blocks: after 200 it is empty again)
    r.block();
    loud += peak_of(r) > 1000 ? 1u : 0u;
  }
  CHECK(loud >= 36u);  // the delayed copy of the 40 blocks (the first ramps in)
  CHECK(loud <= 42u);
  const int calls1 = g_calls;
  r.blocks(20);
  CHECK(g_calls == calls1);  // the line is empty: the effect said 0, the strip is idle
}

TEST_CASE("insert host: tail: a source that restarts while the ring is still audible re-arms the tail in full") {
  Rig r;
  r.dc(0, 8000);
  NEED(r.bind(0, kRing, 0) == InsertHost::Bind::Ok);
  r.blocks(8);
  r.g->feed(0, nullptr);
  r.blocks(10);
  CHECK(peak_of(r) > 100);  // still ringing
  r.g->feed(0, r.feed[0]);
  const int calls0 = g_calls;
  r.blocks(6);
  CHECK(g_calls == calls0 + 6);
  CHECK(peak_of(r) > 1000);
  r.g->feed(0, nullptr);
  const unsigned called = run_counting(r, 200);
  CHECK(called >= 50u);  // the ring starts again at 0.3: the whole decay is rendered, as after the first stop
  CHECK(called <= 58u);
  const int calls1 = g_calls;
  r.blocks(20);
  CHECK(g_calls == calls1);
  CHECK(peak_of(r) == 0);
}

TEST_CASE("insert host: tail: two slots, one in a tail: the other runs on its live source and neither affects the other") {
  Rig r;
  r.dc(0, 8000);
  r.dc(1, 8000);
  NEED(r.bind(0, kRing, 0) == InsertHost::Bind::Ok);  // track 0, slot A
  NEED(r.bind(2, kRing, 1) == InsertHost::Bind::Ok);  // track 1, slot A
  r.blocks(8);
  r.g->feed(0, nullptr);
  const std::uint32_t a0 = r.st[0].renders;
  const std::uint32_t b0 = r.st[1].renders;
  r.blocks(100);
  CHECK(r.st[1].renders - b0 == 100u);  // the live one is called every block
  CHECK(r.st[0].renders - a0 >= 50u);  // the one in the tail rings out and then stops
  CHECK(r.st[0].renders - a0 <= 58u);
  CHECK(peak_of(r) > 1000);  // (track 1)
  r.g->feed(1, nullptr);
  const std::uint32_t b1 = r.st[1].renders;
  r.blocks(100);
  CHECK(r.st[1].renders - b1 >= 50u);
  CHECK(r.st[1].renders - b1 <= 58u);
  const int calls = g_calls;
  r.blocks(20);
  CHECK(g_calls == calls);
  CHECK(peak_of(r) == 0);
}

TEST_CASE("insert host: tail: slot A's tail feeds slot B, which keeps the strip live for it and then ends its own after 2 s of zero") {
  Rig r;
  r.dc(0, 8000);
  NEED(r.bind(0, kRing, 0) == InsertHost::Bind::Ok);  // track 0, slot A: the ring
  NEED(r.bind(1, kGain, 1) == InsertHost::Bind::Ok);  // track 0, slot B: a gain of 1 (always says 1)
  r.blocks(10);
  r.g->feed(0, nullptr);
  const std::uint32_t b0 = r.st[1].renders;
  r.blocks(10);
  CHECK(peak_of(r) > 100);  // A's ring comes out of B
  CHECK(r.st[1].renders - b0 == 10u);
  r.blocks(InsertHost::kTailZeroBlocks + 100u);
  const std::uint32_t used = r.st[1].renders - b0;
  CHECK(used >= 50u + InsertHost::kTailZeroBlocks);  // B ran for the ring (about 54 blocks), then for 2 s of its own zero output
  CHECK(used <= 58u + InsertHost::kTailZeroBlocks);
  const std::uint32_t b1 = r.st[1].renders;
  r.blocks(20);
  CHECK(r.st[1].renders == b1);
  CHECK(peak_of(r) == 0);
}

TEST_CASE("insert host: tail: a tail byte written to a slot with no module (a render racing a forced release) never keeps the strip live") {
  Rig r;
  r.dc(0, 8000);
  r.g->feed(0, nullptr);
  r.blocks(4);
  const std::uint32_t idle_cost = r.report.stage_cycles[ha::kStageStrips];
  r.g->set_insert_tail(0, 0, true);
  r.g->set_insert_tail(0, 1, true);
  r.blocks(4);
  CHECK(r.report.stage_cycles[ha::kStageStrips] == idle_cost);
  CHECK(peak_of(r) == 0);
  CHECK(g_calls == 0);
}

TEST_CASE("insert host: tail: a module stuck returning 1 ends at the cap of 10 s of silent input, and runs again when the input does") {
  Rig r;
  r.dc(0, 8000);
  NEED(r.bind(0, kStuck, 0) == InsertHost::Bind::Ok);
  r.blocks(8);
  const float seconds = static_cast<float>(InsertHost::kTailMaxBlocks) * static_cast<float>(kB) / o3os::core::kRefRateHz;
  CHECK(seconds > 9.99f);
  CHECK(seconds < 10.01f);
  r.g->feed(0, nullptr);
  const int calls0 = g_calls;
  r.blocks(InsertHost::kTailMaxBlocks + 40u);
  CHECK(g_calls - calls0 == static_cast<int>(InsertHost::kTailMaxBlocks));
  const int calls1 = g_calls;
  r.blocks(20);
  CHECK(g_calls == calls1);
  CHECK(peak_of(r) == 0);
  r.g->feed(0, r.feed[0]);
  r.blocks(3);
  CHECK(g_calls == calls1 + 3);  // the source is back: the effect runs, and the cap starts again
  r.g->feed(0, nullptr);
  r.blocks(5);
  CHECK(g_calls == calls1 + 8);
}

TEST_CASE("insert host: tail: a bypass or an unbind in the middle of a tail ends it (the strip goes idle, nothing runs)") {
  Rig r;
  r.dc(0, 8000);
  NEED(r.bind(0, kStuck, 0) == InsertHost::Bind::Ok);
  r.blocks(8);
  r.g->feed(0, nullptr);
  r.blocks(5);
  const int calls0 = g_calls;
  CHECK(r.h->set_bypass(0, true) == InsertHost::Toggle::Done);
  r.blocks(4);
  const int calls1 = g_calls;
  CHECK(calls1 <= calls0 + 3);  // the Bypass is applied at a boundary, one block of ramp, then retired
  r.blocks(10);
  CHECK(g_calls == calls1);
  CHECK(peak_of(r) == 0);
  CHECK(r.h->set_bypass(0, false) == InsertHost::Toggle::Done);  // a reset: a new binding with a clear tail
  r.blocks(10);
  CHECK(g_calls == calls1);  // nothing has run since the reset: no tail yet, the strip stays idle
  r.g->feed(0, r.feed[0]);
  r.blocks(6);
  r.g->feed(0, nullptr);
  const int calls_live = g_calls;
  CHECK(calls_live > calls1);
  r.blocks(10);
  CHECK(g_calls == calls_live + 10);  // (a stuck module rings again with the source silent, until the cap)
  CHECK(r.h->unbind(0));
  r.blocks(6);
  const int calls2 = g_calls;
  r.blocks(10);
  CHECK(g_calls == calls2);
  CHECK(!r.h->bound(0));
  CHECK(peak_of(r) == 0);
}
