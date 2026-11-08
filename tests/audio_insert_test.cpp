// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX S1 (docs/specs/OS-INSFX.md): the insert host (audio/insert_host.h) for effect modules, driven block by block on the real Graph with native effect modules written here
// (a gain, a one-pole low-pass with state, a module that goes idle on silence, a NaN one, a loud one). Host only.
//   * the adapter: float / Q15, the in-place render, the transition ramps, the module rule of the Q15 conversion (NaN is silence, clips counted), the idle skip;
//   * the swap protocol: a ParamChange queued for a binding that has left (and whose record was reused) is dropped, a parameter set right after the bind waits for the render to hold the binding,
//     a full parameter ring and a full command ring lose nothing, a slot on a strip that never runs is retired at the boundary all the same;
//   * both nets (per instance, aggregate), the forced bypass and its clear; admission of a re-bind; the bind refusals;
//   * the state reset at un-bypass and at the start of an export (two exports give the same bits; the nets are off in an export);
//   * the Engine's side: attach_inserts needs the modules, the block boundary drives the host, ParamKind::Insert reaches it;
//   * OS-INSFX S6, the master insert (`master insert:` cases at the end): two instances (L and R) in the master chain between Bass and Limiter, the chain order, the identity through the float planes bit for bit
//     with L and R independent, a NaN or infinite effect cannot reach the limiter or the converter, an export gives equal bits, two instances count 2 of the cap, and the pair's lifecycle (ramps, bypass, forced
//     bypass, unbind, reset, parameters to both);
//   * OS-INSFX-ABI2 A7 (`master stereo:` cases at the end): a stereo effect (render_st) on the master is ONE instance, render_st(l, r, l, r, n) in place with the S6 scrub after it, counting 1 of the cap.
// OS-INSFX-ABI2 A4 (docs/specs/OS-INSFX-ABI2.md 3.2): the stereo track adapter, "stereo:" cases at the end, with native stereo effects: `st_identity` (capable), `st_widen` (WIDENS, S = M / 2), `st_flip` (capable,
// S' = -S), idle variants, an S-only tail and a stateful WIDENS low-pass.
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
  // OS-INSFX-ABI2 A4: the shared S block, and the tracks that were handed it in the block that just ran.
  static const std::int16_t* sblk(const InsertHost& h) { return h.sblk_; }
  static std::uint16_t s_used(const InsertHost& h) { return h.s_used_; }
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
  CHECK(r.bind(InsertHost::kWhere, kGain, 1) == InsertHost::Bind::BadWhere);
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
  audio::VoicePool* pool = nullptr;
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
  r.pool = &pool;
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

// ============================================================================================================================================
// OS-INSFX S6: the master insert (spec section 2 and 5): two instances of one effect module, L and R, the Graph's MasterStage::Insert between Bass and Limiter.
// ============================================================================================================================================
namespace {

constexpr unsigned kMaster = InsertHost::kMasterWhere;  // the key of the master insert (its left instance); the right one is kMaster + 1

std::uint32_t inf_render(void*, const float* in, float* out, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = (i & 1u) != 0u ? -HUGE_VALF : HUGE_VALF + in[i] * 0.0f;
  }
  return 1;
}
std::uint32_t mixed_bad_render(void*, const float* in, float* out, std::uint32_t n) {  // finite samples with a NaN and an infinity among them: only the bad ones may go
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = in[i];
  }
  out[3] = std::nanf("");
  out[17] = HUGE_VALF;
  out[40] = -HUGE_VALF;
  return 1;
}
const o3m_module kInf = FX_MODULE("inf", O3M_KIND_EFFECT, inf_render);
const o3m_module kMixedBad = FX_MODULE("mixedbad", O3M_KIND_EFFECT, mixed_bad_render);

// The order the master chain runs in, as seen from a stage and from an effect.
std::vector<int> g_seq;
void stage_note(void* tag, float*, float*, std::uint32_t) noexcept { g_seq.push_back(*static_cast<int*>(tag)); }
void stage_pass(void*, float*, float*, std::uint32_t) noexcept {}
std::uint32_t seq_render(void*, const float* in, float* out, std::uint32_t n) {
  g_seq.push_back(2);
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = in[i];
  }
  return 1;
}
const o3m_module kSeq = FX_MODULE("seq", O3M_KIND_EFFECT, seq_render);

// A master pair bound with its two state blocks (st[l], st[r]).
InsertHost::Bind bind_master(Rig& r, const o3m_module& m, unsigned l, unsigned rr, const float* params = nullptr, unsigned count = 0, bool force = false) {
  return r.h->bind(kMaster, m, 0x8001, &r.st[l], reinterpret_cast<const std::uint8_t*>(params), count * 4u, force, &r.st[rr]);
}

// A track panned hard to one side, fed a block of `v` (a constant): its content is only in that side's plane.
void side_track(Rig& r, unsigned t, float pan, std::int16_t v) {
  CHECK(r.g->params().push(core::ParamChange{audio::strip_param(t, gps::Pan), pan, 1}));
  r.dc(t, v);
}
std::int16_t rch(const Rig& r, unsigned i) { return r.out[2u * i + 1u]; }

bool all_zero(const Rig& r, bool right) {
  for (std::uint32_t i = 0; i < kB; ++i) {
    if ((right ? rch(r, i) : r.l(i)) != 0) {
      return false;
    }
  }
  return true;
}

// Two tracks of different content, hard left and hard right (so the two planes differ), a no-op stage in the chain so that both runs go through the float planes. `with_insert` binds an identity effect
// on the master. The output of `blocks` blocks is recorded whole (the ramp-in blocks too).
constexpr unsigned kIdBlocks = 12;
void identity_run(bool with_insert, std::int16_t (&rec)[kIdBlocks][kBS]) {
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  // varying content (the feed is re-filled every block): a ramp up on the left track, a down-going saw on the right one
  CHECK(r.g->params().push(core::ParamChange{audio::strip_param(0, gps::Pan), 0.0f, 1}));
  CHECK(r.g->params().push(core::ParamChange{audio::strip_param(1, gps::Pan), 100.0f, 1}));
  CHECK(r.g->params().push(core::ParamChange{audio::strip_param(0, gps::Source), 3.0f, 1}));
  CHECK(r.g->params().push(core::ParamChange{audio::strip_param(1, gps::Source), 3.0f, 1}));
  if (with_insert) {
    NEED(bind_master(r, kGain, 0, 1) == InsertHost::Bind::Ok);
  }
  for (unsigned b = 0; b < kIdBlocks; ++b) {
    for (std::uint32_t i = 0; i < kB; ++i) {
      r.feed[0][i] = static_cast<std::int16_t>(((i + b * 7u) * 37u) % 9000u);
      r.feed[1][i] = static_cast<std::int16_t>(-static_cast<int>(((i + b * 5u) * 53u) % 11000u));
    }
    r.g->feed(0, r.feed[0]);
    r.g->feed(1, r.feed[1]);
    r.block();
    std::memcpy(rec[b], r.out, sizeof rec[b]);
  }
}

}  // namespace

TEST_CASE("master insert: the chain order: Headroom, Saturation, Eq, Bass, INSERT, Limiter, Space, BitDepth, and the effect runs between Bass and the Limiter") {
  CHECK(audio::kMasterStages == 8u);
  CHECK(audio::kMasterChain[3].id == audio::MasterStage::Bass);
  CHECK(audio::kMasterChain[4].id == audio::MasterStage::Insert);
  CHECK(audio::kMasterChain[5].id == audio::MasterStage::Limiter);
  Rig r;
  int bass = 1;
  int limiter = 3;
  g_seq.clear();
  r.g->install_master_stage(audio::MasterStage::Limiter, &stage_note, &limiter);  // installed in the wrong order on purpose: the table decides
  r.g->install_master_stage(audio::MasterStage::Bass, &stage_note, &bass);
  r.g->set_master_bypass(audio::MasterStage::Limiter, false);
  r.g->set_master_bypass(audio::MasterStage::Bass, false);
  r.dc(0, 8000);
  NEED(bind_master(r, kSeq, 0, 1) == InsertHost::Bind::Ok);
  r.blocks(4);
  // one block: Bass (1), the effect for the left plane (2), the effect for the right plane (2), the Limiter (3)
  g_seq.clear();
  r.block();
  const std::vector<int> want = {1, 2, 2, 3};
  CHECK(g_seq == want);
  // bypassed: the effect leaves the chain (its stage is out), the others stay in order
  CHECK(r.h->set_bypass(kMaster, true) == InsertHost::Toggle::Done);
  r.blocks(4);
  g_seq.clear();
  r.block();
  const std::vector<int> without = {1, 3};
  CHECK(g_seq == without);
}

TEST_CASE("master insert: an identity effect through the float planes is bit for bit what the chain gives without it, L and R independent") {
  static std::int16_t ref[kIdBlocks][kBS];
  static std::int16_t got[kIdBlocks][kBS];
  identity_run(false, ref);
  identity_run(true, got);
  CHECK(std::memcmp(ref, got, sizeof ref) == 0);  // every block, the ramp-in blocks included
  bool left = false;
  bool right = false;
  for (std::uint32_t i = 0; i < kBS; i += 2) {
    left = left || got[kIdBlocks - 1][i] != 0;
    right = right || got[kIdBlocks - 1][i + 1] != 0;
  }
  CHECK(left);   // (the two planes did carry sound: the test is not an empty one)
  CHECK(right);
}

TEST_CASE("master insert: the two instances are independent: a stateful effect on a left-only mix leaves the right plane exactly silent, and each instance has its own state") {
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  side_track(r, 0, 0.0f, 8000);  // hard to one side
  NEED(bind_master(r, kLp, 0, 1) == InsertHost::Bind::Ok);
  r.blocks(12);
  const bool left_side = !all_zero(r, false);
  const bool right_side = !all_zero(r, true);
  CHECK(left_side != right_side);  // the track is in one plane only
  CHECK(r.st[0].renders == r.st[1].renders);
  CHECK(r.st[0].renders >= 10u);
  // the instance of the silent side saw nothing: its one-pole state is exactly 0, the other's is not
  const FxState& live = left_side ? r.st[0] : r.st[1];
  const FxState& quiet = left_side ? r.st[1] : r.st[0];
  CHECK(live.y > 0.0f);
  CHECK(quiet.y == 0.0f);
  CHECK(all_zero(r, left_side));  // (the other side of the output: no crosstalk, no DC)
  // and the state blocks are two (the host keeps a block per instance)
  InsertHost::Info a{};
  InsertHost::Info b{};
  unsigned seen = 0;
  const void* states[2] = {nullptr, nullptr};
  for (unsigned i = 0; i < InsertHost::kInstances; ++i) {
    if (r.h->info(i, a) && (a.where == kMaster || a.where == kMaster + 1u)) {
      states[seen++ & 1u] = a.state;
    }
  }
  CHECK(seen == 2u);
  CHECK(states[0] != states[1]);
  (void)b;
}

TEST_CASE("master insert: a gain effect fades in over one block on both planes and fades out when unbound; the parameter reaches both instances") {
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  r.dc(0, 8000);
  const int base = r.level_after_warmup();
  NEED(base > 100);
  CHECK(std::abs(rch(r, kB - 1) - base) <= 2);  // (centred: both planes)
  const float half = 0.5f;
  NEED(bind_master(r, kGain, 0, 1, &half, 1) == InsertHost::Bind::Ok);
  CHECK(g_init == 2);
  CHECK(g_params == 2);  // the stored blob, to each instance, by the main loop
  CHECK(r.h->bound(kMaster));
  CHECK_FALSE(r.h->bound(kMaster + 1u));
  r.block();  // the boundary applies the pair's Activate
  CHECK(r.phase(0) != InsertHost::Phase::Off);
  CHECK(r.h->stats().activations == 1);  // one binding: the pair
  r.block();  // the ramp block: dry at the start, wet at the end, never a step, both planes
  CHECK(r.l(0) > base - base / 8);
  CHECK(rch(r, 0) > base - base / 8);
  CHECK(std::abs(r.l(kB - 1) - base / 2) <= 3);
  CHECK(std::abs(rch(r, kB - 1) - base / 2) <= 3);
  for (std::uint32_t i = 1; i < kB; ++i) {
    CHECK(r.l(i) <= r.l(i - 1) + 1);
    CHECK(rch(r, i) <= rch(r, i - 1) + 1);
  }
  r.block();
  CHECK(r.steady_near(base / 2, 3));
  // a parameter to the master goes to both instances (one ParamChange, the key 32, applied to both at the block start)
  NEED(r.h->set_param(kMaster, 0, 0.25f));
  r.blocks(3);
  CHECK(r.steady_near(base / 4, 3));
  CHECK(r.st[0].gain == 0.25f);
  CHECK(r.st[1].gain == 0.25f);
  CHECK(r.h->stats().params == 2);
  CHECK(r.h->stats().stale == 0);
  CHECK_FALSE(r.h->set_param(kMaster + 1u, 0, 0.9f));  // the right instance is the pair's: never addressed alone
  // unbind: the pair ramps out together, the module of each instance is shut down after the render is clear of both
  CHECK(r.h->unbind(kMaster));
  CHECK_FALSE(r.h->unbind(kMaster));
  CHECK_FALSE(r.h->unbind(kMaster + 1u));
  r.block();
  r.block();
  CHECK(r.l(0) < base / 4 + base / 8);
  CHECK(std::abs(r.l(kB - 1) - base) <= 3);
  CHECK(std::abs(rch(r, kB - 1) - base) <= 3);
  CHECK(g_shut == 0);
  r.blocks(3);
  CHECK(g_shut == 2);
  CHECK_FALSE(r.h->bound(kMaster));
  CHECK(r.h->stats().retired == 2);
  CHECK(r.h->settled());
  CHECK_FALSE(r.g->master_stage_active(audio::MasterStage::Insert));  // the stage is out of the chain
  CHECK(r.steady_near(base, 3));
}

TEST_CASE("master insert: a NaN or an infinite effect cannot reach the limiter or the converter: the planes are scrubbed, finite samples are untouched") {
  for (const o3m_module* m : {&kNan, &kInf}) {
    CAPTURE(m->name);
    Rig r;
    static test::StaticSlot<audio::MasterLimiter> lslot;
    audio::MasterLimiter& lim = lslot.make();
    r.g->install_master_limiter(lim);
    r.dc(0, 8000);
    r.blocks(6);
    NEED(bind_master(r, *m, 0, 1) == InsertHost::Bind::Ok);
    for (unsigned b = 0; b < 40; ++b) {
      r.block();
      CHECK(lim.finite());  // the limiter's delay line and followers hold no NaN or infinity, at any block
    }
    // the output is silence (the effect's output is 0 once scrubbed: not the +32767 rail the float converter gives a NaN, not -32768 for an infinity)
    CHECK(all_zero(r, false));
    CHECK(all_zero(r, true));
    CHECK(r.h->stats().scrubbed >= 2u * kB * 30u);
    // and the converter's own rail rule is what the scrub protects from: the float -> Q15 kernel maps a NaN to +32767
    float bad[4] = {std::nanf(""), 0.0f, 0.0f, 0.0f};
    std::int16_t q[4];
    audio::kernel::float_to_q15(q, bad, 4);
    CHECK(q[0] == 32767);
  }
  // one NaN and two infinities in a block of finite samples: only those samples go, the rest is the input exactly
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  r.dc(0, 8000);
  r.blocks(6);
  const std::uint32_t before = r.h->stats().scrubbed;
  NEED(bind_master(r, kMixedBad, 0, 1) == InsertHost::Bind::Ok);
  r.blocks(4);
  const std::int16_t keep = r.l(10);
  CHECK(r.l(3) == 0);
  CHECK(r.l(17) == 0);
  CHECK(r.l(40) == 0);
  CHECK(r.l(11) == keep);
  CHECK(rch(r, 3) == 0);
  CHECK(rch(r, 40) == 0);
  CHECK(r.h->stats().scrubbed - before >= 2u * 3u);
}

TEST_CASE("master insert: two instances count 2 of the cap of four instances, in either order, and the free list gives a pair back") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(bind_master(r, kGain, 0, 1) == InsertHost::Bind::Ok);
  NEED(r.bind(0, kGain, 2) == InsertHost::Bind::Ok);
  NEED(r.bind(1, kGain, 3) == InsertHost::Bind::Ok);
  CHECK(r.bind(2, kGain, 4) == InsertHost::Bind::NoInstance);  // the master is two of the four
  CHECK(bind_master(r, kGain, 0, 1) == InsertHost::Bind::WhereTaken);
  r.blocks(4);
  // the master leaves: its two records are free, a track insert fits again, and a master bind again reuses the pair's state blocks
  CHECK(r.h->unbind(kMaster));
  r.blocks(6);
  CHECK(r.h->bind(0, kGain, 0x8001, &r.st[4], nullptr, 0, true) == InsertHost::Bind::WhereTaken);
  // the former pair's two records hold the module and their state blocks: two track inserts take them with no new memory (the free list)
  NEED(r.h->bind(2, kGain, 0x8001, nullptr, nullptr, 0, true) == InsertHost::Bind::Ok);
  NEED(r.h->bind(3, kGain, 0x8001, nullptr, nullptr, 0, true) == InsertHost::Bind::Ok);
  CHECK(r.h->bind(4, kGain, 0x8001, nullptr, nullptr, 0, true) == InsertHost::Bind::NoInstance);  // four tracks fill it
  // the other way round: with one record free (three track inserts) the master has no room for its second instance
  CHECK(r.h->unbind(2));
  r.blocks(6);
  CHECK(r.h->bind(kMaster, kGain, 0x8001, nullptr, nullptr, 0, true) == InsertHost::Bind::NoInstance);
  CHECK(r.h->unbind(3));
  r.blocks(6);
  // two free records that hold the module and a state block: the master finds them with no new memory
  CHECK(r.h->bind(kMaster, kGain, 0x8001, nullptr, nullptr, 0, true) == InsertHost::Bind::Ok);
  CHECK(r.h->bound(kMaster));
  CHECK(r.h->unbind(1));
  CHECK(r.h->unbind(0));
  // refusals: the right instance is no `where` of its own; a three-track project leaves no master
  CHECK(r.h->bind(kMaster + 1u, kGain, 0x8001, &r.st[0], nullptr, 0, true) == InsertHost::Bind::BadWhere);
  CHECK(r.h->bind(kMaster + 2u, kGain, 0x8001, &r.st[0], nullptr, 0, true) == InsertHost::Bind::BadWhere);
  CHECK(r.h->set_bypass(kMaster + 1u, true) == InsertHost::Toggle::NotBound);
  r.blocks(4);
  CHECK(r.h->settled() == false);  // (bound)
  r.h->retire_all();
  for (int i = 0; i < 10 && !r.h->settled(); ++i) {
    r.block();
  }
  CHECK(r.h->settled());
}

TEST_CASE("master insert: three track inserts leave the master no room: NoInstance, and nothing of it is left half bound") {
  Rig r;
  r.dc(0, 8000);
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  NEED(r.bind(1, kGain, 1) == InsertHost::Bind::Ok);
  NEED(r.bind(2, kGain, 2) == InsertHost::Bind::Ok);
  const int inits = g_init;
  CHECK(bind_master(r, kGain, 3, 4) == InsertHost::Bind::NoInstance);
  CHECK(g_init == inits);  // nothing was initialised for a bind that could not hold both
  CHECK_FALSE(r.h->bound(kMaster));
  // the fourth record is still free for a track
  NEED(r.bind(3, kGain, 3) == InsertHost::Bind::Ok);
}

TEST_CASE("master insert: a master pair whose second init fails takes the first one down again and binds nothing") {
  Rig r;
  r.dc(0, 8000);
  // the first init passes, the second refuses: fx_init refuses while g_fail_init is set, so count calls through a wrapper state: the first instance is made, then the flag is raised
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  CHECK(r.h->unbind(0));
  r.blocks(6);
  g_fail_init = 1;
  CHECK(bind_master(r, kGain, 1, 2) == InsertHost::Bind::InitFailed);
  g_fail_init = 0;
  CHECK_FALSE(r.h->bound(kMaster));
  const int shut = g_shut;
  r.blocks(2);
  CHECK(g_shut == shut);
  NEED(bind_master(r, kGain, 1, 2) == InsertHost::Bind::Ok);  // and the records are as free as before
}

TEST_CASE("master insert: bypass is a ramp to dry on both planes together, un-bypass is a reset of both instances") {
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  r.dc(0, 8000);
  const int base = r.level_after_warmup();
  const float half = 0.5f;
  NEED(bind_master(r, kLp, 0, 1, &half, 1) == InsertHost::Bind::Ok);
  r.blocks(12);
  CHECK(r.st[0].y > 0.0f);
  CHECK(r.st[1].y > 0.0f);
  CHECK(r.h->set_bypass(kMaster, true) == InsertHost::Toggle::Done);
  r.block();  // the Bypass applied at its boundary
  r.block();  // the ramp out
  CHECK(std::abs(r.l(kB - 1) - base) <= 3);
  CHECK(std::abs(rch(r, kB - 1) - base) <= 3);
  r.blocks(2);
  for (unsigned i = 0; i < InsertHost::kInstances; ++i) {
    InsertHost::Info info{};
    if (r.h->info(i, info) && (info.where == kMaster || info.where == kMaster + 1u)) {
      CHECK(info.phase == InsertHost::Phase::Bypassed);
      CHECK(info.user_bypass);
    }
  }
  CHECK_FALSE(r.g->master_stage_active(audio::MasterStage::Insert));  // out of the chain: nothing runs, nothing costs
  const int calls = g_calls;
  r.blocks(3);
  CHECK(g_calls == calls);
  // off: a reset of the pair: shutdown and init of each instance, the stored parameter again
  const int shut = g_shut;
  const int inits = g_init;
  CHECK(r.h->set_bypass(kMaster, false) == InsertHost::Toggle::Done);
  r.blocks(10);
  CHECK(g_shut == shut + 2);
  CHECK(g_init == inits + 2);
  CHECK(r.st[0].gain == 0.5f);
  CHECK(r.st[1].gain == 0.5f);
  CHECK(r.h->bound(kMaster));
  CHECK(r.g->master_stage_active(audio::MasterStage::Insert));
  for (unsigned i = 0; i < InsertHost::kInstances; ++i) {
    InsertHost::Info info{};
    if (r.h->info(i, info) && (info.where == kMaster || info.where == kMaster + 1u)) {
      CHECK((info.phase == InsertHost::Phase::On || info.phase == InsertHost::Phase::In));
      CHECK_FALSE(info.user_bypass);
    }
  }
}

TEST_CASE("master insert: the nets force the pair to dry together, never one channel; an un-bypass is admitted as a pair") {
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  r.dc(0, 8000);
  const int base = r.level_after_warmup();
  g_step = 150000;  // each instance's run is over the instance budget (50 % of the block)
  const float half = 0.5f;
  NEED(bind_master(r, kGain, 0, 1, &half, 1, true) == InsertHost::Bind::Ok);
  r.blocks(8);
  CHECK(r.h->stats().forced_instance >= 1);
  for (unsigned i = 0; i < InsertHost::kInstances; ++i) {
    InsertHost::Info info{};
    if (r.h->info(i, info) && (info.where == kMaster || info.where == kMaster + 1u)) {
      CHECK(info.forced);  // both, or neither
      CHECK(info.phase == InsertHost::Phase::Bypassed);
    }
  }
  CHECK(r.steady_near(base, 3));  // dry, not silent
  CHECK(std::abs(rch(r, kB - 1) - base) <= 3);
  CHECK(r.h->set_bypass(kMaster, false) == InsertHost::Toggle::NoCpu);  // two instances at the measured cost: the net would force them again
  CHECK(r.h->set_bypass(kMaster, false, true) == InsertHost::Toggle::Done);
  g_step = 0;
  r.blocks(14);
  CHECK(r.h->bound(kMaster));
  InsertHost::Info info{};
  unsigned live = 0;
  for (unsigned i = 0; i < InsertHost::kInstances; ++i) {
    if (r.h->info(i, info) && (info.where == kMaster || info.where == kMaster + 1u) && !info.forced) {
      ++live;
    }
  }
  CHECK(live == 2u);
}

TEST_CASE("master insert: the aggregate net takes the pair as one candidate, at twice the cost of an instance") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  // master (two instances of cost A) and a track insert of cost B: each instance fits, the three together do not. The pair is the most expensive candidate (2 * A against B) and is forced as a unit.
  g_cost_a = 70000;  // each master instance
  g_cost_b = 60000;  // the track insert
  NEED(bind_master(r, kCostA, 0, 1, nullptr, 0, true) == InsertHost::Bind::Ok);
  NEED(r.h->bind(0, kCostB, 0x8002, &r.st[2], nullptr, 0, true) == InsertHost::Bind::Ok);
  r.gblocks(40, 0);
  CHECK(r.h->stats().forced_aggregate >= 1);
  CHECK(r.h->stats().forced_instance == 0);
  unsigned forced_master = 0;
  bool track_forced = false;
  for (unsigned i = 0; i < InsertHost::kInstances; ++i) {
    InsertHost::Info info{};
    if (!r.h->info(i, info)) {
      continue;
    }
    if (info.where == kMaster || info.where == kMaster + 1u) {
      forced_master += info.forced ? 1u : 0u;
    } else if (info.where == 0u) {
      track_forced = info.forced;
    }
  }
  CHECK((forced_master == 0u || forced_master == 2u));  // never one channel of the pair
  CHECK(forced_master == 2u);
  CHECK_FALSE(track_forced);
}

TEST_CASE("master insert: an idle effect on a silent plane is not called; the effect on one plane does not wake the other") {
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  NEED(bind_master(r, kIdle, 0, 1) == InsertHost::Bind::Ok);
  r.blocks(6);  // silent planes
  const int calls = g_calls;
  r.blocks(6);
  CHECK(g_calls <= calls + 2);  // (the first render said idle on silence: not called again)
  side_track(r, 0, 0.0f, 8000);
  r.blocks(4);
  CHECK(g_calls > calls + 2);  // the sound wakes the effect of the plane that has it
  const int busy = g_calls;
  r.block();
  CHECK(g_calls - busy == 1);  // one plane has sound: one instance is called (the other plane is silent and its effect said idle)
}

TEST_CASE("master insert: project load: retire_all, settled, release_all clear the pair; a release with the render still holding it clears the stage") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(bind_master(r, kGain, 0, 1) == InsertHost::Bind::Ok);
  r.blocks(4);
  CHECK(r.g->master_stage_active(audio::MasterStage::Insert));
  r.h->retire_all();
  CHECK_FALSE(r.h->settled());
  for (int i = 0; i < 10 && !r.h->settled(); ++i) {
    r.block();
  }
  CHECK(r.h->settled());
  r.blocks(3);
  r.h->release_all();
  CHECK(g_shut == 2);
  CHECK_FALSE(r.h->bound(kMaster));
  CHECK_FALSE(r.g->master_stage_active(audio::MasterStage::Insert));
  // the render still holds a pair, released at once (the timeout of a project load): the stage must not outlive the modules
  NEED(bind_master(r, kGain, 0, 1) == InsertHost::Bind::Ok);
  r.blocks(4);
  CHECK(r.g->master_stage_active(audio::MasterStage::Insert));
  r.h->release_all();
  CHECK(g_shut == 4);
  CHECK_FALSE(r.g->master_stage_active(audio::MasterStage::Insert));
  const int calls = g_calls;
  r.blocks(2);
  CHECK(g_calls == calls);  // nothing calls the modules that are gone
}

TEST_CASE("master insert: the state is reset at the start of an export (two exports give the same bits), both instances") {
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  side_track(r, 0, 0.0f, 8000);
  side_track(r, 1, 100.0f, 6000);
  r.blocks(4);
  NEED(bind_master(r, kLp, 0, 1) == InsertHost::Bind::Ok);
  CHECK(r.h->set_param(kMaster, 0, 0.5f));
  r.blocks(10);
  CHECK(r.st[0].y > 0.0f);
  CHECK(r.st[1].y > 0.0f);
  CHECK(g_init == 2);
  static std::int16_t a[10][kBS];
  static std::int16_t b[10][kBS];
  CHECK(r.h->prepare_export());
  CHECK(g_shut == 2);
  CHECK(g_init == 4);
  CHECK(r.st[0].y == 0.0f);
  CHECK(r.st[1].y == 0.0f);
  CHECK(r.st[0].gain == 0.5f);  // the stored parameter again, on both
  CHECK(r.st[1].gain == 0.5f);
  for (unsigned i = 0; i < 10; ++i) {
    r.block();
    std::memcpy(a[i], r.out, sizeof a[i]);
  }
  r.h->export_end();
  r.blocks(5);  // live again: the states move on
  CHECK(r.st[0].y > 0.0f);
  CHECK(r.st[1].y > 0.0f);
  CHECK(r.h->prepare_export());
  CHECK(r.st[0].y == 0.0f);
  CHECK(r.st[1].y == 0.0f);
  for (unsigned i = 0; i < 10; ++i) {
    r.block();
    std::memcpy(b[i], r.out, sizeof b[i]);
  }
  CHECK(std::memcmp(a, b, sizeof a) == 0);  // equal bits
  bool any = false;
  for (std::uint32_t i = 0; i < kBS; ++i) {
    any = any || a[9][i] != 0;
  }
  CHECK(any);
  r.h->export_end();
}

TEST_CASE("master insert: an export whose restart fails refuses the export and the pair leaves together") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(bind_master(r, kLp, 0, 1) == InsertHost::Bind::Ok);
  r.blocks(10);
  g_fail_init = 1;  // the first instance's init() refuses on the restart
  CHECK_FALSE(r.h->prepare_export());
  g_fail_init = 0;
  CHECK(r.h->stats().export_unsettled == 1);
  r.blocks(12);  // the live render goes on; the pair left together
  CHECK_FALSE(r.h->bound(kMaster));
  CHECK_FALSE(r.g->master_stage_active(audio::MasterStage::Insert));
  CHECK(r.h->settled());
}

TEST_CASE("master insert: the render path allocates nothing with the pair running, ramping, bypassed, forced and retiring") {
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  side_track(r, 0, 0.0f, 8000);
  side_track(r, 1, 100.0f, 6000);
  insert_alloc::count = 0;
  NEED(bind_master(r, kLp, 0, 1) == InsertHost::Bind::Ok);
  r.blocks(8);
  CHECK(r.h->set_bypass(kMaster, true) == InsertHost::Toggle::Done);
  r.blocks(6);
  CHECK(r.h->set_bypass(kMaster, false) == InsertHost::Toggle::Done);
  r.blocks(12);
  CHECK(r.h->unbind(kMaster));
  r.blocks(8);
  CHECK(insert_alloc::count == 0u);
}

TEST_CASE("master insert: with no master insert the engine's chain is bit for bit what it was (the stage slot is empty and out of the chain)") {
  Rig r;
  CHECK_FALSE(r.g->master_stage_active(audio::MasterStage::Insert));
  side_track(r, 0, 0.0f, 8000);
  r.blocks(4);
  CHECK_FALSE(r.g->master_stage_active(audio::MasterStage::Insert));
  CHECK(r.h->stats().scrubbed == 0u);
}

TEST_CASE("master insert: a ParamChange{Insert, key 32} from outside the host (a MIDI Learn binding) reaches both instances; the internal key 33 and a stale generation are dropped") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(bind_master(r, kGain, 0, 1) == InsertHost::Bind::Ok);
  r.blocks(4);
  InsertHost::Info left{};
  bool found = false;
  for (unsigned i = 0; i < InsertHost::kInstances && !found; ++i) {
    found = r.h->info(i, left) && left.where == kMaster;
  }
  NEED(found);
  const std::uint32_t params = r.h->stats().params;
  CHECK(r.g->params().push(core::ParamChange{core::param_id(core::ParamKind::Insert, kMaster, 0), 0.75f, left.gen}));
  r.blocks(2);
  CHECK(r.st[0].gain == 0.75f);
  CHECK(r.st[1].gain == 0.75f);
  CHECK(r.h->stats().params == params + 2u);
  const std::uint32_t stale = r.h->stats().stale;
  CHECK(r.g->params().push(core::ParamChange{core::param_id(core::ParamKind::Insert, kMaster + 1u, 0), 0.1f, left.gen}));  // the right instance is never addressed alone
  CHECK(r.g->params().push(core::ParamChange{core::param_id(core::ParamKind::Insert, kMaster, 0), 0.2f, static_cast<std::uint16_t>(left.gen + 7u)}));  // another generation
  r.blocks(2);
  CHECK(r.st[0].gain == 0.75f);
  CHECK(r.st[1].gain == 0.75f);
  CHECK(r.h->stats().stale == stale + 2u);
}

// ---- OS-INSFX-ABI2 A4: the stereo track adapter -------------------------------------------------------------------------------------------
namespace {
int g_st_calls = 0;  // render_st() calls (g_calls counts the mono render() of the effects above, which a stereo effect keeps for its mono blocks)

std::uint32_t st_id_render(void*, const float* il, const float* ir, float* ol, float* orr, std::uint32_t n) {  // identity, in == out or not (the master's transition renders out of place)
  ++g_st_calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    ol[i] = il[i];
    orr[i] = ir[i];
  }
  return 1;
}
std::uint32_t st_widen_render(void*, const float* il, const float* ir, float* ol, float* orr, std::uint32_t n) {  // M kept, S' = M / 2: L' = 1.5 M, R' = 0.5 M
  ++g_st_calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    const float m = (il[i] + ir[i]) * 0.5f;
    const float sd = m * 0.5f;
    ol[i] = m + sd;
    orr[i] = m - sd;
  }
  return 1;
}
std::uint32_t st_flip_render(void*, const float* il, const float* ir, float* ol, float* orr, std::uint32_t n) {  // S' = -S
  ++g_st_calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    const float l = il[i];
    const float r = ir[i];
    ol[i] = r;
    orr[i] = l;
  }
  return 1;
}
std::uint32_t st_idle_render(void*, const float* il, const float* ir, float* ol, float* orr, std::uint32_t n) {  // identity; 0 when both inputs are silent
  ++g_st_calls;
  float any = 0.0f;
  for (std::uint32_t i = 0; i < n; ++i) {
    any += (il[i] != 0.0f || ir[i] != 0.0f) ? 1.0f : 0.0f;
    ol[i] = il[i];
    orr[i] = ir[i];
  }
  return any != 0.0f ? 1u : 0u;
}
std::uint32_t st_stail_render(void*, const float* il, const float* ir, float* ol, float* orr, std::uint32_t n) {  // M kept, a constant S' of 0.001 (32 LSB) whatever the input: only S rings, M' is silent on a silent source
  ++g_st_calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    const float m = (il[i] + ir[i]) * 0.5f;
    ol[i] = m + 0.001f;
    orr[i] = m - 0.001f;
  }
  return 1;
}
std::uint32_t st_wlp_render(void* s, const float* il, const float* ir, float* ol, float* orr, std::uint32_t n) {  // WIDENS with state: a one-pole on M, S' = M / 2
  FxState* d = static_cast<FxState*>(s);
  ++g_st_calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    const float m = (il[i] + ir[i]) * 0.5f;
    d->y += (m - d->y) * 0.1f;
    ol[i] = d->y * 1.5f;
    orr[i] = d->y * 0.5f;
  }
  return 1;
}
// A 1.12 descriptor: the mono render (valid, as the ABI asks), render_st and fx_flags after `importer`.
#define FX_ST_MODULE(name, render_st, flags)                                                                                                                                                           \
  o3m_module { O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), name, sizeof(FxState), 1, kFxParams, fx_init, gain_render, fx_set_param, nullptr, nullptr, fx_shutdown, nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr, render_st, flags }
const o3m_module kStId = FX_ST_MODULE("st_identity", st_id_render, 0u);
const o3m_module kStWiden = FX_ST_MODULE("st_widen", st_widen_render, O3M_FX_WIDENS);
const o3m_module kStFlip = FX_ST_MODULE("st_flip", st_flip_render, 0u);
const o3m_module kStIdleC = FX_ST_MODULE("st_idle_c", st_idle_render, 0u);
const o3m_module kStIdleW = FX_ST_MODULE("st_idle_w", st_idle_render, O3M_FX_WIDENS);
const o3m_module kStTail = FX_ST_MODULE("st_tail", st_stail_render, O3M_FX_WIDENS);
const o3m_module kStWlp = FX_ST_MODULE("st_wlp", st_wlp_render, O3M_FX_WIDENS);

// One scene: track 0 fed noise (M) and, with `voice_s`, noise S (the voice's, re-fed every block as the Engine does: the host nulls the pointers that hold its own block); `a` in slot 0 from block 3 and `b` in slot 1.
// Returns the output of every block (L, R interleaved); `calls` the running count of render_st calls after each block.
std::vector<std::int16_t> st_scene(const o3m_module* a, const o3m_module* b, bool voice_s, unsigned blocks, std::vector<int>* calls = nullptr) {
  Rig r;
  g_st_calls = 0;
  r.dc(0, 0);
  Lcg rm{11u};
  Lcg rs{77u};
  std::vector<std::int16_t> rec;
  for (unsigned i = 0; i < blocks; ++i) {
    if (i == 3u) {
      if (a != nullptr) {
        CHECK(r.bind(0, *a, 0) == InsertHost::Bind::Ok);
      }
      if (b != nullptr) {
        CHECK(r.bind(1, *b, 1) == InsertHost::Bind::Ok);
      }
    }
    noise(rm, r.feed[0], kB, 6000);
    if (voice_s) {
      noise(rs, r.side, kB, 4000);
      r.g->feed_side(0, r.side);
    }
    r.block();
    rec.insert(rec.end(), r.out, r.out + kBS);
    if (calls != nullptr) {
      calls->push_back(g_st_calls);
    }
  }
  return rec;
}
// The last block of a scene's record.
const std::int16_t* last_block(const std::vector<std::int16_t>& v) { return v.data() + v.size() - kBS; }
int max_lr(const std::int16_t* blk) {  // the largest |L - R|
  int worst = 0;
  for (std::uint32_t i = 0; i < kB; ++i) {
    worst = std::max(worst, std::abs(static_cast<int>(blk[2u * i]) - static_cast<int>(blk[2u * i + 1u])));
  }
  return worst;
}
int s_of_out(const Rig& r, std::uint32_t i) { return (static_cast<int>(r.out[2u * i]) - static_cast<int>(r.out[2u * i + 1u])) / 2; }
int m_of_out(const Rig& r, std::uint32_t i) { return (static_cast<int>(r.out[2u * i]) + static_cast<int>(r.out[2u * i + 1u])) / 2; }
// A one-block ramp from `from` to `to`: monotone, no step, about 58 of its samples strictly between 5 % and 95 % of the span.
bool ramp_ok(const std::vector<int>& seq, int from, int to) {
  const int span = std::abs(to - from);
  if (span < 100 || seq.size() < 2u) {
    return false;
  }
  const int lo = std::min(from, to);
  const int hi = std::max(from, to);
  const int max_step = span / 40 + 2;
  int steps_over = 0;
  int mid = 0;
  int direction = 0;
  int turns = 0;
  for (std::size_t i = 1; i < seq.size(); ++i) {
    const int d = seq[i] - seq[i - 1];
    steps_over += std::abs(d) > max_step ? 1 : 0;
    mid += (seq[i] > lo + span / 20 && seq[i] < hi - span / 20) ? 1 : 0;
    if (d != 0) {
      const int dir = d > 0 ? 1 : -1;
      turns += (direction != 0 && dir != direction) ? 1 : 0;
      direction = dir;
    }
  }
  return steps_over == 0 && mid >= 56 && mid <= 64 && turns == 0 && std::abs(seq.front() - from) <= 2 && std::abs(seq.back() - to) <= 2;
}
}  // namespace

TEST_CASE("insert host: stereo: the shape of a module: mono, stereo-capable or WIDENS; a module of 1.11 or below, or an instrument, is mono whatever its tail says") {
  CHECK(InsertHost::shape_of(kGain) == InsertHost::Shape::Mono);
  CHECK(InsertHost::shape_of(kStId) == InsertHost::Shape::Capable);
  CHECK(InsertHost::shape_of(kStWiden) == InsertHost::Shape::Widens);
  o3m_module old = kStWiden;
  old.abi_minor = 11;  // the members past 84 are not read for a module that declares 1.11
  CHECK(InsertHost::shape_of(old) == InsertHost::Shape::Mono);
  o3m_module inst = kStWiden;
  inst.kind = O3M_KIND_INSTRUMENT;
  CHECK(InsertHost::shape_of(inst) == InsertHost::Shape::Mono);
}

TEST_CASE("insert host: stereo: an identity stereo effect on a stereo voice keeps M and S bit for bit, and does run stereo") {
  const std::vector<std::int16_t> ref = st_scene(nullptr, nullptr, true, 24);
  const std::vector<std::int16_t> got = st_scene(&kStId, nullptr, true, 24);
  CHECK(g_st_calls > 10);
  CHECK(ref.size() == got.size());
  CHECK(ref == got);
  CHECK(max_lr(last_block(got)) > 500);  // the S really is there
}

TEST_CASE("insert host: stereo: an identity stereo-capable effect on a mono track adds no S, runs its mono render and is bit for bit the track without it") {
  const std::vector<std::int16_t> ref = st_scene(nullptr, nullptr, false, 24);
  const std::vector<std::int16_t> got = st_scene(&kStId, nullptr, false, 24);
  CHECK(g_st_calls == 0);
  CHECK(g_calls > 10);
  CHECK(ref == got);
  CHECK(max_lr(last_block(got)) <= 1);
}

TEST_CASE("insert host: stereo: a WIDENS effect on a mono track gives L = M + S and R = M - S at MAIN (here S = M / 2: 1.5 and 0.5 of the dry track)") {
  const std::vector<std::int16_t> ref = st_scene(nullptr, nullptr, false, 24);
  const std::vector<std::int16_t> got = st_scene(&kStWiden, nullptr, false, 24);
  CHECK(g_st_calls > 10);
  const std::int16_t* a = last_block(ref);
  const std::int16_t* b = last_block(got);
  int worst = 0;
  int big = 0;
  for (std::uint32_t i = 0; i < kB; ++i) {
    const int m = a[2u * i];
    worst = std::max(worst, std::abs(static_cast<int>(b[2u * i]) - (m * 3) / 2));
    worst = std::max(worst, std::abs(static_cast<int>(b[2u * i + 1u]) - m / 2));
    big = std::max(big, std::abs(m));
  }
  CHECK(big > 1000);
  CHECK(worst <= 4);
}

TEST_CASE("insert host: stereo: a mono insert before or after a stereo one: the mono one drops the S that reaches it (D6), both ways; a WIDENS after a mono one makes its own S") {
  const std::vector<std::int16_t> dry = st_scene(nullptr, nullptr, false, 24);
  const auto m_ok = [&](const std::int16_t* blk) {  // M is the track's, whatever happens to S
    int worst = 0;
    for (std::uint32_t i = 0; i < kB; ++i) {
      worst = std::max(worst, std::abs((blk[2u * i] + blk[2u * i + 1u]) / 2 - (last_block(dry)[2u * i] + last_block(dry)[2u * i + 1u]) / 2));
    }
    return worst <= 2;
  };
  {  // stereo first, mono (a gain of 1) second: S is gone after it
    const std::vector<std::int16_t> v = st_scene(&kStId, &kGain, true, 24);
    CHECK(max_lr(last_block(v)) <= 1);
    CHECK(m_ok(last_block(v)));
  }
  {  // mono first, a stereo-capable one second: S never reaches it, so it runs its mono render again once the S ramp of the first is over
    std::vector<int> calls;
    const std::vector<std::int16_t> v = st_scene(&kGain, &kStId, true, 24, &calls);
    CHECK(max_lr(last_block(v)) <= 1);
    CHECK(m_ok(last_block(v)));
    CHECK(calls.back() == calls[calls.size() - 8]);  // no render_st in the last seven blocks
  }
  {  // mono first, WIDENS second: the voice's S is dropped, the effect's own S is made from M: the same as on a track with no voice S, bit for bit
    const std::vector<std::int16_t> with_s = st_scene(&kGain, &kStWiden, true, 24);
    const std::vector<std::int16_t> no_s = st_scene(&kGain, &kStWiden, false, 24);
    CHECK(std::memcmp(last_block(with_s), last_block(no_s), kBS * 2u) == 0);
    CHECK(max_lr(last_block(with_s)) > 500);
  }
  {  // WIDENS first, mono second: the S the widener made is dropped by the gain
    const std::vector<std::int16_t> v = st_scene(&kStWiden, &kGain, false, 24);
    CHECK(max_lr(last_block(v)) <= 1);
    CHECK(m_ok(last_block(v)));
  }
}

TEST_CASE("insert host: stereo: a mono insert bound in the path of a stereo one takes S away over one block, never as a step (after it and before it)") {
  for (unsigned order = 0; order < 2u; ++order) {  // 0: the mono one is bound after the stereo one (slot 1), 1: before it (slot 0, the stereo one in slot 1)
    Rig r;
    g_st_calls = 0;
    r.dc(0, 8000);
    float one = 1.0f;
    NEED(r.bind(order == 0u ? 0u : 1u, kStId, 0) == InsertHost::Bind::Ok);
    for (unsigned i = 0; i < 10; ++i) {
      r.dc_side(0, 6000);
      r.block();
    }
    const int s_full = s_of_out(r, kB - 1);
    NEED(s_full > 1000);
    NEED(r.bind(order == 0u ? 1u : 0u, kGain, 1, &one, 1) == InsertHost::Bind::Ok);
    std::vector<int> seq;
    int worst_m = 0;
    const int m_full = m_of_out(r, kB - 1);
    for (unsigned i = 0; i < 4; ++i) {
      r.dc_side(0, 6000);
      r.block();
      for (std::uint32_t k = 0; k < kB; ++k) {
        seq.push_back(s_of_out(r, k));
        worst_m = std::max(worst_m, std::abs(m_of_out(r, k) - m_full));
      }
    }
    INFO("order " << order);
    CHECK(ramp_ok(seq, s_full, 0));
    CHECK(worst_m <= 2);
  }
}

TEST_CASE("insert host: stereo: bind, bypass on and off and unbind of a WIDENS effect on a mono track: S comes and goes over one block each, monotone, and M never moves; no side pointer outlives its block") {
  Rig r;
  g_st_calls = 0;
  r.dc(0, 8000);
  r.blocks(10);
  const int m_full = m_of_out(r, kB - 1);
  NEED(std::abs(s_of_out(r, kB - 1)) <= 1);
  int worst_m = 0;
  const auto take = [&](unsigned blocks, std::vector<int>& seq) {
    seq.clear();
    for (unsigned b = 0; b < blocks; ++b) {
      r.block();
      CHECK(r.g->side(0) == nullptr);  // the boundary took the host's block off the strip
      CHECK(InsertHostTest::s_used(*r.h) == 0u);
      for (std::uint32_t i = 0; i < kB; ++i) {
        seq.push_back(s_of_out(r, i));
        worst_m = std::max(worst_m, std::abs(m_of_out(r, i) - m_full));
      }
    }
  };
  std::vector<int> bind_seq, byp_seq, unbyp_seq, unbind_seq;
  NEED(r.bind(0, kStWiden, 0) == InsertHost::Bind::Ok);
  take(4, bind_seq);
  const int s_full = bind_seq.back();
  NEED(s_full > 1000);
  CHECK(ramp_ok(bind_seq, 0, s_full));
  CHECK(r.h->set_bypass(0, true) == InsertHost::Toggle::Done);
  take(4, byp_seq);
  CHECK(ramp_ok(byp_seq, s_full, 0));
  CHECK(r.h->set_bypass(0, false) == InsertHost::Toggle::Done);  // a reset, then the activation again
  take(8, unbyp_seq);
  CHECK(ramp_ok(unbyp_seq, 0, s_full));
  CHECK(r.h->unbind(0));
  take(8, unbind_seq);
  CHECK(ramp_ok(unbind_seq, s_full, 0));
  CHECK(!r.h->bound(0));
  CHECK(r.g->insert_stereo_mask() == 0u);  // the slot's mark went with it
  CHECK(worst_m <= 3);
}

TEST_CASE("insert host: stereo: two stereo inserts on one track: S goes from the first to the second through the shared block; a flip in the second flips the image") {
  const std::vector<std::int16_t> dry = st_scene(nullptr, nullptr, false, 24);
  const std::vector<std::int16_t> one = st_scene(&kStWiden, nullptr, false, 24);
  {  // the second is an identity: the same output as the first alone, bit for bit (the voice's S is replaced by the widener's, so it makes no difference)
    const std::vector<std::int16_t> two = st_scene(&kStWiden, &kStId, false, 24);
    CHECK(std::memcmp(last_block(one), last_block(two), kBS * 2u) == 0);
    const std::vector<std::int16_t> two_s = st_scene(&kStWiden, &kStId, true, 24);
    CHECK(std::memcmp(last_block(one), last_block(two_s), kBS * 2u) == 0);
  }
  {  // WIDENS then flip: L = 0.5 M, R = 1.5 M
    const std::vector<std::int16_t> v = st_scene(&kStWiden, &kStFlip, false, 24);
    int worst = 0;
    for (std::uint32_t i = 0; i < kB; ++i) {
      const int m = last_block(dry)[2u * i];
      worst = std::max(worst, std::abs(static_cast<int>(last_block(v)[2u * i]) - m / 2));
      worst = std::max(worst, std::abs(static_cast<int>(last_block(v)[2u * i + 1u]) - (m * 3) / 2));
    }
    CHECK(worst <= 4);
  }
  {  // a flip alone on a stereo voice swaps the sides of S and nothing else: L and R of a pan-centred strip trade places by S -> -S
    const std::vector<std::int16_t> ref = st_scene(nullptr, nullptr, true, 24);
    const std::vector<std::int16_t> v = st_scene(&kStFlip, nullptr, true, 24);
    int worst = 0;
    for (std::uint32_t i = 0; i < kB; ++i) {
      const int m_ref = (last_block(ref)[2u * i] + last_block(ref)[2u * i + 1u]) / 2;
      const int m_got = (last_block(v)[2u * i] + last_block(v)[2u * i + 1u]) / 2;
      const int s_ref = (last_block(ref)[2u * i] - last_block(ref)[2u * i + 1u]) / 2;
      const int s_got = (last_block(v)[2u * i] - last_block(v)[2u * i + 1u]) / 2;
      worst = std::max(worst, std::max(std::abs(m_ref - m_got), std::abs(s_ref + s_got)));
    }
    CHECK(worst <= 3);
  }
}

TEST_CASE("insert host: stereo: two tracks with a stereo insert in one block: each track's S is consumed before the other's insert overwrites the shared block (the sum of the two alone)") {
  const auto scene = [](bool t0, bool t1) {
    Rig r;
    g_st_calls = 0;
    r.dc(0, 0);
    r.dc(1, 0);
    Lcg a{5u};
    Lcg b{9u};
    CHECK(r.bind(0, kStWiden, 0) == InsertHost::Bind::Ok);
    CHECK(r.bind(2, kStWiden, 1) == InsertHost::Bind::Ok);
    std::vector<std::int16_t> rec;
    for (unsigned i = 0; i < 16; ++i) {
      std::memset(r.feed[0], 0, sizeof r.feed[0]);
      std::memset(r.feed[1], 0, sizeof r.feed[1]);
      if (t0) {
        noise(a, r.feed[0], kB, 5000);
      }
      if (t1) {
        noise(b, r.feed[1], kB, 2500);
      }
      r.block();
      rec.insert(rec.end(), r.out, r.out + kBS);
    }
    return rec;
  };
  const std::vector<std::int16_t> only0 = scene(true, false);
  const std::vector<std::int16_t> only1 = scene(false, true);
  const std::vector<std::int16_t> both = scene(true, true);
  int worst = 0;
  int big = 0;
  for (std::size_t i = both.size() - kBS; i < both.size(); ++i) {
    worst = std::max(worst, std::abs(static_cast<int>(both[i]) - (static_cast<int>(only0[i]) + static_cast<int>(only1[i]))));
    big = std::max(big, std::abs(static_cast<int>(both[i])));
  }
  CHECK(big > 1000);
  CHECK(worst <= 3);
}

TEST_CASE("insert host: stereo: no stale S: after an unbind and on an idle strip the strip's side pointer is never left at the host's block, and the output has no S once the insert is gone") {
  Rig r;
  g_st_calls = 0;
  r.dc(0, 8000);
  NEED(r.bind(0, kStIdleW, 0) == InsertHost::Bind::Ok);
  const auto step = [&]() {
    r.block();
    CHECK(r.g->side(0) != InsertHostTest::sblk(*r.h));
    CHECK(r.g->side(0) == nullptr);
  };
  for (int i = 0; i < 8; ++i) {
    step();
  }
  CHECK(g_st_calls > 0);
  r.fill(0, 0);  // the source goes silent: the effect says idle, then is not called
  for (int i = 0; i < 8; ++i) {
    step();
  }
  const int calls = g_st_calls;
  for (int i = 0; i < 8; ++i) {
    step();
  }
  CHECK(g_st_calls == calls);  // idle: not called
  CHECK(max_lr(r.out) == 0);
  r.fill(0, 8000);
  for (int i = 0; i < 6; ++i) {
    step();
  }
  CHECK(g_st_calls > calls);  // the signal returned: called again
  CHECK(r.h->unbind(0));
  for (int i = 0; i < 8; ++i) {
    step();
  }
  CHECK(!r.h->bound(0));
  CHECK(max_lr(r.out) <= 1);  // no S left on MAIN
  CHECK(r.g->insert_stereo_mask() == 0u);
}

TEST_CASE("insert host: stereo: no stale S on a voice's track: the voice's S is fed every block, and the host's block is never what is left at a block's end") {
  Rig v;
  g_st_calls = 0;
  v.dc(0, 0);
  NEED(v.bind(0, kStId, 0) == InsertHost::Bind::Ok);
  Lcg rng{3u};
  for (int i = 0; i < 12; ++i) {
    noise(rng, v.feed[0], kB, 6000);
    noise(rng, v.side, kB, 4000);
    v.g->feed_side(0, v.side);
    v.block();
    CHECK(v.g->side(0) != InsertHostTest::sblk(*v.h));
    if (i >= 4) {  // the insert is in the path from the block after the bind's boundary; before it the voice's own pointer is what is there
      CHECK(v.g->side(0) == nullptr);
    }
  }
  CHECK(g_st_calls > 0);
}

TEST_CASE("insert host: stereo: the idle skip needs M and S both silent: S alone wakes a stereo effect, silence puts it to sleep again") {
  for (unsigned shape = 0; shape < 2u; ++shape) {  // 0: stereo-capable, 1: WIDENS
    Rig r;
    g_st_calls = 0;
    r.dc(0, 0);  // M silent throughout
    NEED(r.bind(0, shape == 0u ? kStIdleC : kStIdleW, 0) == InsertHost::Bind::Ok);
    const auto go = [&](std::int16_t s, unsigned n) {
      for (unsigned i = 0; i < n; ++i) {
        r.dc_side(0, s);
        r.block();
      }
    };
    INFO("shape " << shape);
    go(3000, 6);
    const int awake = g_st_calls;
    CHECK(awake >= 4);  // S is not silence, so the effect runs although M is
    CHECK(max_lr(r.out) > 100);
    go(0, 4);  // both silent: the effect says 0 once, then is skipped
    const int asleep = g_st_calls;
    go(0, 6);
    CHECK(g_st_calls == asleep);
    go(3000, 3);
    CHECK(g_st_calls > asleep);  // S came back
  }
}

TEST_CASE("insert host: stereo: the tail of a stereo effect counts M' and S' both: an S-only ring outlives the 2 s of zero output") {
  InsertHost::Info info{};
  Rig r;
  g_st_calls = 0;
  r.dc(0, 8000);
  NEED(r.bind(0, kStTail, 0) == InsertHost::Bind::Ok);
  r.blocks(8);
  r.fill(0, 0);
  constexpr unsigned kPast = InsertHost::kTailZeroBlocks + 140u;
  for (unsigned i = 0; i < kPast; ++i) {
    r.block();
  }
  NEED(r.h->info(0, info));
  CHECK(info.tail);  // M' has been exact zero for over 2 s, S' has not
  const int calls = g_st_calls;
  r.block();
  CHECK(g_st_calls == calls + 1);
  CHECK(max_lr(r.out) > 10);  // and the S' of the ring reaches MAIN
  CHECK(r.g->side(0) == nullptr);
  CHECK(r.h->unbind(0));
  r.blocks(8);
  CHECK(!r.h->bound(0));
}

TEST_CASE("insert host: stereo: a stereo effect that goes quiet with its source: the tail ends after 2 s of exact zero M' and S'") {
  InsertHost::Info info{};
  Rig q;
  g_st_calls = 0;
  q.dc(0, 8000);
  NEED(q.bind(0, kStWiden, 0) == InsertHost::Bind::Ok);
  q.blocks(8);
  q.fill(0, 0);
  for (unsigned i = 0; i < 100; ++i) {
    q.block();
  }
  NEED(q.h->info(0, info));
  CHECK(info.tail);  // under 2 s: still ringing as far as it can tell
  for (unsigned i = 0; i < InsertHost::kTailZeroBlocks; ++i) {
    q.block();
  }
  NEED(q.h->info(0, info));
  CHECK(!info.tail);
  const int c2 = g_st_calls;
  q.blocks(4);
  CHECK(g_st_calls == c2);
}

TEST_CASE("insert host: stereo: the render path allocates nothing with stereo inserts bound, running, ramping, bypassed and retiring") {
  Rig r;
  g_st_calls = 0;
  r.dc(0, 8000);
  r.dc(1, 6000);
  insert_alloc::count = 0;
  NEED(r.bind(0, kStWiden, 0) == InsertHost::Bind::Ok);
  NEED(r.bind(1, kStFlip, 1) == InsertHost::Bind::Ok);
  NEED(r.bind(2, kStId, 2) == InsertHost::Bind::Ok);
  r.blocks(6);
  CHECK(r.h->set_bypass(0, true) == InsertHost::Toggle::Done);
  r.blocks(4);
  CHECK(r.h->set_bypass(0, false) == InsertHost::Toggle::Done);
  r.blocks(8);
  CHECK(r.h->unbind(1));
  CHECK(r.h->unbind(2));
  r.blocks(8);
  CHECK(r.h->unbind(0));
  r.blocks(8);
  CHECK(g_st_calls > 10);
  CHECK(r.h->settled());
  CHECK(insert_alloc::count == 0u);
}

TEST_CASE("insert host: stereo: two exports of a song with a stateful WIDENS effect give the same bits, S included") {
  Rig r;
  g_st_calls = 0;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(r.bind(0, kStWlp, 0) == InsertHost::Bind::Ok);
  r.blocks(10);
  CHECK(r.st[0].y > 0.0f);
  std::int16_t a[10][kBS];
  std::int16_t b[10][kBS];
  CHECK(r.h->prepare_export());
  CHECK(r.st[0].y == 0.0f);
  for (unsigned i = 0; i < 10; ++i) {
    r.block();
    std::memcpy(a[i], r.out, sizeof a[i]);
  }
  r.h->export_end();
  r.blocks(5);
  CHECK(r.st[0].y > 0.0f);
  CHECK(r.h->prepare_export());
  for (unsigned i = 0; i < 10; ++i) {
    r.block();
    std::memcpy(b[i], r.out, sizeof b[i]);
  }
  CHECK(std::memcmp(a, b, sizeof a) == 0);
  CHECK(max_lr(a[9]) > 500);  // the S is in the bits
  r.h->export_end();
}

const o3m_module kStTailC = FX_ST_MODULE("st_tail_c", st_stail_render, 0u);

TEST_CASE("insert host: stereo: a stereo-capable effect whose tail rings in S stays stereo after the voice's S ends: the S part of the tail is not cut to the mono render") {
  Rig r;
  g_st_calls = 0;
  r.dc(0, 8000);
  NEED(r.bind(0, kStTailC, 0) == InsertHost::Bind::Ok);
  for (int i = 0; i < 10; ++i) {  // a stereo voice: S is fed every block
    r.dc_side(0, 3000);
    r.block();
  }
  const int calls = g_st_calls;
  CHECK(calls >= 6);
  r.fill(0, 0);  // the sample ends: M silent and no S any more
  for (int i = 0; i < 100; ++i) {
    r.block();
    CHECK(r.g->side(0) == nullptr);
  }
  CHECK(g_st_calls == calls + 100);  // render_st every block, never the mono render
  CHECK(max_lr(r.out) > 10);         // and the ring's S reaches MAIN
}

TEST_CASE("insert host: stereo: a bypass or an unbind in the middle of an S tail with S live: the effect's S hands over to the dry S over one block, M never moves, and nothing is left ringing or pointed at") {
  Rig r;
  g_st_calls = 0;
  r.dc(0, 8000);
  int s_dry = 0;
  int m_dry = 0;
  for (int i = 0; i < 10; ++i) {  // the dry reference: no insert bound, the voice's S alone at MAIN
    r.dc_side(0, 3000);
    r.block();
  }
  s_dry = s_of_out(r, kB - 1);
  m_dry = m_of_out(r, kB - 1);
  NEED(s_dry > 1000);
  NEED(r.bind(0, kStTail, 0) == InsertHost::Bind::Ok);
  const auto s_live = [&](unsigned blocks, std::vector<int>& seq) {  // the voice's S stays live throughout
    seq.clear();
    for (unsigned b = 0; b < blocks; ++b) {
      r.dc_side(0, 3000);
      r.block();
      CHECK(r.g->side(0) != InsertHostTest::sblk(*r.h));  // never the host's block at a block's end
      for (std::uint32_t i = 0; i < kB; ++i) {
        seq.push_back(s_of_out(r, i));
        CHECK(std::abs(m_of_out(r, i) - m_dry) <= 3);  // M never moves
      }
    }
  };
  std::vector<int> seq;
  s_live(8, seq);
  InsertHost::Info info{};
  NEED(r.h->info(0, info));
  CHECK(info.tail);  // the S tail is ringing (kStTail keeps S' at 0.001 whatever the input)
  const int s_full = seq.back();
  NEED(std::abs(s_full) > 10);
  CHECK(r.g->insert_stereo_mask() != 0u);
  CHECK(r.h->set_bypass(0, true) == InsertHost::Toggle::Done);  // a bypass in the middle of the tail
  s_live(4, seq);
  CHECK(ramp_ok(seq, s_full, s_dry));  // one block, monotone, no step: the effect's S hands over to the dry S
  const int calls = g_st_calls;
  s_live(4, seq);
  CHECK(g_st_calls == calls);  // bypassed: not called
  CHECK(r.g->insert_stereo_mask() == 0u);
  CHECK(r.h->set_bypass(0, false) == InsertHost::Toggle::Done);
  s_live(8, seq);
  CHECK(ramp_ok(seq, s_dry, s_full));  // the un-bypass ramps back in over one block
  CHECK(r.h->unbind(0));  // an unbind in the middle of the tail
  s_live(4, seq);
  CHECK(ramp_ok(seq, s_full, s_dry));
  CHECK(!r.h->bound(0));
  CHECK(r.g->insert_stereo_mask() == 0u);
  const int calls2 = g_st_calls;
  s_live(4, seq);
  CHECK(g_st_calls == calls2);  // gone: not called
}

TEST_CASE("insert host: stereo: a stereo-capable effect whose S tail rings drops to the mono render when M is live again with S still null: today the S tail is cut in one block (known limitation, OS-INSFX-ABI2 3.2)") {
  Rig r;
  g_st_calls = 0;
  r.dc(0, 8000);
  NEED(r.bind(0, kStTailC, 0) == InsertHost::Bind::Ok);
  for (int i = 0; i < 10; ++i) {  // a stereo voice: S fed every block, the effect runs stereo
    r.dc_side(0, 3000);
    r.block();
  }
  const int m_live = m_of_out(r, kB - 1);
  r.fill(0, 0);  // the sample ends: M silent and no S any more: the S tail rings and the effect stays stereo (the case above)
  for (int i = 0; i < 20; ++i) {
    r.block();
    CHECK(r.g->side(0) == nullptr);
  }
  InsertHost::Info info{};
  NEED(r.h->info(0, info));
  CHECK(info.tail);                        // the S tail is ringing
  CHECK(r.g->insert_stereo_mask() != 0u);  // still stereo through the tail
  const int calls = g_st_calls;
  CHECK(calls >= 25);       // render_st every block, never the mono render
  CHECK(max_lr(r.out) > 10);  // the ring's S is on MAIN
  NEED(std::abs(m_of_out(r, kB - 1)) <= 1);
  const int mono_calls = g_calls;
  r.fill(0, 8000);  // M live again, S still null
  r.block();
  CHECK(r.g->insert_stereo_mask() == 0u);  // the drop to the mono render happens in this one block
  CHECK(max_lr(r.out) == 0);               // the S tail is cut in one block (today's behaviour, recorded)
  CHECK(g_st_calls == calls);              // no render_st any more
  for (int i = 0; i < 6; ++i) {
    r.block();
    CHECK(r.g->side(0) == nullptr);
    CHECK(max_lr(r.out) == 0);
  }
  CHECK(g_st_calls == calls);    // still the mono render
  CHECK(g_calls > mono_calls);   // render() runs instead
  CHECK(std::abs(m_of_out(r, kB - 1) - m_live) <= 3);  // and M passes through unchanged
}

namespace {
alignas(8) std::int16_t g_stwave[2u * 3000u];  // M, then S at 3000
// A stereo sample note on track 0 through the Engine, an identity stereo-capable effect bound or not; the output of every block.
std::vector<std::int16_t> engine_voice_scene(bool with_insert, bool& side_clean) {
  for (std::uint32_t i = 0; i < 3000u; ++i) {
    g_stwave[i] = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(6.283185307 * 7.0 * static_cast<double>(i) / 3000.0)));
    g_stwave[3000u + i] = static_cast<std::int16_t>(std::lround(9000.0 * std::sin(6.283185307 * 11.0 * static_cast<double>(i) / 3000.0 + 1.0)));
  }
  reset_counters();
  EngineRig er;
  make_engine<4>(er, with_insert);
  g_st_calls = 0;
  audio::SampleDesc d{g_stwave, 3000, 0, 0, 65536};
  d.st_off = 3000;
  er.pool->set_sample(0, d);
  alignas(16) static FxState state;
  std::memset(&state, 0, sizeof state);
  if (with_insert) {
    CHECK(er.h->bind(0, kStId, 0x8001, &state, nullptr, 0, false) == InsertHost::Bind::Ok);
  }
  CHECK(er.pool->note_on(0, 60, 0, 127));
  std::vector<std::int16_t> rec;
  side_clean = true;
  for (unsigned b = 0; b < 20; ++b) {
    if (b == 12u) {
      er.pool->cut(0);  // the note ends
    }
    er.block();
    rec.insert(rec.end(), er.out, er.out + kBS);
    if (with_insert) {
      const std::int16_t* sd = er.e->graph().side(0);
      side_clean = side_clean && sd != InsertHostTest::sblk(*er.h) && (b < 14u || sd == nullptr);
    }
  }
  return rec;
}
}  // namespace

TEST_CASE("insert host: stereo: through the Engine: an identity stereo-capable effect on a stereo voice is the voice without it, bit for bit, and no side pointer is left at the host's block after the note ends") {
  bool clean_a = true;
  bool clean_b = true;
  const std::vector<std::int16_t> ref = engine_voice_scene(false, clean_a);
  const std::vector<std::int16_t> got = engine_voice_scene(true, clean_b);
  CHECK(g_st_calls > 5);  // the voice made S, so the effect ran stereo
  int worst = 0;
  for (std::size_t i = 0; i < kBS * 12u; ++i) {
    worst = std::max(worst, std::abs(static_cast<int>(ref[i])));
  }
  CHECK(worst > 500);  // the note sounds
  CHECK(max_lr(ref.data() + kBS * 8u) > 100);  // with its S
  CHECK(ref == got);
  CHECK(clean_b);
}

// ---- OS-INSFX-ABI2 A5: the cap (the stereo reservation, spec 3.2) --------------------------------------------------------------------
namespace {
alignas(8) std::int16_t g_capwave[2u * 3000u];  // M, then S at 3000

// One rig with the VoicePool attached to the host (as Engine::attach_inserts does) and one stereo sample on slot 0.
struct CapRig {
  Graph* g = nullptr;
  ModuleVoices* mv = nullptr;
  InsertHost* h = nullptr;
  audio::VoicePool* pool = nullptr;
  alignas(16) FxState st[InsertHost::kInstances + 2];
  InsertHost::Blobs blobs;
  alignas(8) std::int16_t in[kBS] = {};
  alignas(8) std::int16_t out[kBS] = {};
  ha::BlockReport report{};
  CapRig() {
    static test::StaticSlot<Graph> gslot;
    static test::StaticSlot<ModuleVoices> mslot;
    static test::StaticSlot<InsertHost> hslot;
    static test::StaticSlot<audio::VoicePool> pslot;
    static audio::TapMem taps;
    reset_counters();
    for (std::uint32_t i = 0; i < 3000u; ++i) {
      g_capwave[i] = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(6.283185307 * 7.0 * static_cast<double>(i) / 3000.0)));
      g_capwave[3000u + i] = static_cast<std::int16_t>(std::lround(9000.0 * std::sin(6.283185307 * 11.0 * static_cast<double>(i) / 3000.0 + 1.0)));
    }
    audio::SampleDesc d{g_capwave, 3000, 0, 0, 65536};
    d.st_off = 3000;
    g = &gslot.make(&graph_clock, taps);
    mv = &mslot.make();
    h = &hslot.make();
    pool = &pslot.make();
    pool->set_sample(0, d);
    std::memset(st, 0, sizeof st);
    std::memset(blobs, 0, sizeof blobs);
    h->attach(*g, *mv, &blobs, pool);  // A5: the pool, as the engine attaches it
    h->set_cycles(&ins_clock);
    g->set_param_hook(&route_insert, h);
  }
  // One block as the Engine does: the graph, then the pool's block (a note event when `note_track` >= 0), then the host's boundary, then the tick.
  void block(int note_track = -1) {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0, &report};
    g->render(io);
    pool->set_gate(0);
    pool->begin_block();
    if (note_track >= 0) {
      CHECK(pool->note_on(static_cast<unsigned>(note_track), 60, 0, audio::kVelocityDefault));
    }
    pool->render(0, kB);
    pool->end_block();
    h->begin_block();
    h->service();
  }
  void blocks(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      block();
    }
  }
  unsigned st_next = 0;
  InsertHost::Bind bind_track(unsigned where, const o3m_module& m) {
    const unsigned i = st_next++;
    return h->bind(where, m, 0x8001, &st[i], nullptr, 0, false);
  }
};
}  // namespace

TEST_CASE("insert host: cap: two WIDENS inserts reserve two side blocks; with four stereo notes only two voices play S, the third and fourth play M") {
  CapRig r;
  r.blocks(2);
  NEED(r.bind_track(0, kStWiden) == InsertHost::Bind::Ok);  // track 0, slot 0
  NEED(r.bind_track(2, kStWiden) == InsertHost::Bind::Ok);  // track 1, slot 0
  r.blocks(2);                                              // the activates apply, then the count runs and lands in the pool
  CHECK(r.pool->st_reserved() == 2u);
  for (unsigned t = 0; t < 4u; ++t) {
    r.block(static_cast<int>(t));  // a stereo note on each of tracks 0..3
  }
  CHECK(r.pool->stereo_voices() == 2u);  // kStMax 4 less the 2 reserved
  r.block();
  CHECK(r.pool->feed_side(0) != nullptr);
  CHECK(r.pool->feed_side(1) != nullptr);
  CHECK(r.pool->feed_side(2) == nullptr);  // the third note plays M
  CHECK(r.pool->feed_side(3) == nullptr);  // so does the fourth
}

TEST_CASE("insert host: cap: two WIDENS inserts on one track cost two side blocks (the count is of inserts, not tracks)") {
  CapRig r;
  r.blocks(2);
  NEED(r.bind_track(0, kStWiden) == InsertHost::Bind::Ok);  // track 0, slot 0
  NEED(r.bind_track(1, kStWiden) == InsertHost::Bind::Ok);  // track 0, slot 1
  r.blocks(2);
  CHECK(r.pool->st_reserved() == 2u);
  for (unsigned t = 0; t < 4u; ++t) {
    r.block(static_cast<int>(t));
  }
  CHECK(r.pool->stereo_voices() == 2u);
  r.block();
  CHECK(r.pool->feed_side(2) == nullptr);
  CHECK(r.pool->feed_side(3) == nullptr);
}

TEST_CASE("insert host: cap: a note that holds a block keeps it when the reservation grows (nothing is taken from a sounding voice)") {
  CapRig r;
  r.blocks(2);
  NEED(r.bind_track(0, kStWiden) == InsertHost::Bind::Ok);
  NEED(r.bind_track(2, kStWiden) == InsertHost::Bind::Ok);
  r.blocks(2);
  CHECK(r.pool->st_reserved() == 2u);
  r.block(0);  // the two notes that take the free blocks
  r.block(1);
  CHECK(r.pool->stereo_voices() == 2u);
  NEED(r.bind_track(4, kStWiden) == InsertHost::Bind::Ok);  // a third WIDENS, on track 2
  r.blocks(2);
  CHECK(r.pool->st_reserved() == 3u);
  CHECK(r.pool->stereo_voices() == 2u);  // the held blocks survived the bigger reservation
  r.block();
  CHECK(r.pool->feed_side(0) != nullptr);
  CHECK(r.pool->feed_side(1) != nullptr);
  r.block(3);  // a new note now: 2 held + 3 reserved is past the cap, it plays M
  CHECK(r.pool->stereo_voices() == 2u);
  CHECK(r.pool->feed_side(3) == nullptr);
}

TEST_CASE("insert host: cap: the one-block lag: a note that starts in the block after an Activate can still take the last free block and keeps it to the end of the note") {
  CapRig r;
  r.blocks(2);
  for (unsigned t = 0; t < 3u; ++t) {
    r.block(static_cast<int>(t));  // three notes hold three of the four blocks
  }
  CHECK(r.pool->stereo_voices() == 3u);
  CHECK(r.pool->st_reserved() == 0u);
  NEED(r.bind_track(0, kStWiden) == InsertHost::Bind::Ok);
  r.block();       // the Activate applies at this boundary, after the count ran: the reservation is still 0 for the next block's notes
  r.block(3);      // the note that starts in the block after the Activate takes the last free block
  CHECK(r.pool->stereo_voices() == 4u);
  r.blocks(2);     // now the count has landed
  CHECK(r.pool->st_reserved() == 1u);
  CHECK(r.pool->stereo_voices() == 4u);  // nothing was taken from the sounding voices
  r.block();
  CHECK(r.pool->feed_side(3) != nullptr);  // the lagging note keeps its block
  r.block(4);                              // a fifth note is refused
  CHECK(r.pool->stereo_voices() == 4u);
  CHECK(r.pool->feed_side(4) == nullptr);
}

TEST_CASE("insert host: cap: bypass returns the slot: a bypassed WIDENS insert stops reserving and the next note takes the block") {
  CapRig r;
  r.blocks(2);
  NEED(r.bind_track(0, kStWiden) == InsertHost::Bind::Ok);
  NEED(r.bind_track(2, kStWiden) == InsertHost::Bind::Ok);
  r.blocks(2);
  CHECK(r.pool->st_reserved() == 2u);
  CHECK(r.h->set_bypass(0, true) == InsertHost::Toggle::Done);
  r.blocks(3);  // the ramp out still counts, then Bypassed does not
  CHECK(r.pool->st_reserved() == 1u);
  for (unsigned t = 0; t < 3u; ++t) {
    r.block(static_cast<int>(t));
  }
  CHECK(r.pool->stereo_voices() == 3u);  // the freed block went to a new note
  r.block();
  CHECK(r.pool->feed_side(0) != nullptr);
  CHECK(r.pool->feed_side(1) != nullptr);
  CHECK(r.pool->feed_side(2) != nullptr);
  CHECK(r.pool->feed_side(3) == nullptr);
}

TEST_CASE("insert host: cap: a stereo-capable insert reserves none: four notes play S with it bound") {
  CapRig r;
  r.blocks(2);
  NEED(r.bind_track(0, kStId) == InsertHost::Bind::Ok);
  r.blocks(2);
  CHECK(r.pool->st_reserved() == 0u);
  for (unsigned t = 0; t < 4u; ++t) {
    r.block(static_cast<int>(t));
  }
  CHECK(r.pool->stereo_voices() == 4u);
  r.block();
  for (unsigned t = 0; t < 4u; ++t) {
    CHECK(r.pool->feed_side(t) != nullptr);
  }
}

TEST_CASE("insert host: cap: the master insert reserves none") {
  CapRig r;
  r.blocks(2);
  NEED(r.h->bind(InsertHost::kMasterWhere, kStWiden, 0x8001, &r.st[0], nullptr, 0, false, &r.st[1]) == InsertHost::Bind::Ok);
  r.blocks(2);
  CHECK(r.h->bound(InsertHost::kMasterWhere));
  CHECK(r.pool->st_reserved() == 0u);  // a WIDENS on the master still keeps all four side blocks for the voices
  for (unsigned t = 0; t < 4u; ++t) {
    r.block(static_cast<int>(t));
  }
  CHECK(r.pool->stereo_voices() == 4u);
}

TEST_CASE("insert host: cap: attach re-syncs the reservation to the pool (st_res_ = 0xFF), also to a second pool") {
  CapRig r;
  r.blocks(2);
  r.pool->set_st_reserved(2u);  // a pool that comes in already reserved (another engine's host had it)
  r.h->attach(*r.g, *r.mv, &r.blobs, r.pool);  // st_res_ = 0xFF: the next boundary pushes the count even when it would match
  r.block();
  CHECK(r.pool->st_reserved() == 0u);  // no WIDENS bound: the stale reservation was cleared at the boundary
  // a second pool: the count lands on the new one, not the old
  static test::StaticSlot<audio::VoicePool> p2slot;
  audio::VoicePool& p2 = p2slot.make();
  NEED(r.bind_track(0, kStWiden) == InsertHost::Bind::Ok);
  r.blocks(2);
  CHECK(r.pool->st_reserved() == 1u);
  r.h->attach_pool(&p2);  // st_res_ = 0xFF again
  r.blocks(2);
  CHECK(p2.st_reserved() == 1u);  // the WIDENS count was pushed to the new pool
}

TEST_CASE("insert host: cap: unbind returns the slot (the ramp out still counts, the retired instance does not)") {
  CapRig r;
  r.blocks(2);
  NEED(r.bind_track(0, kStWiden) == InsertHost::Bind::Ok);
  r.blocks(2);
  CHECK(r.pool->st_reserved() == 1u);
  r.block(0);  // a note takes a block while the insert runs
  CHECK(r.pool->stereo_voices() == 1u);
  CHECK(r.h->unbind(0));
  r.blocks(2);  // OutLeave is still counted, then the instance retires and the count drops
  CHECK(r.pool->st_reserved() == 0u);
  for (unsigned t = 1; t < 4u; ++t) {
    r.block(static_cast<int>(t));  // the slot is back: three more notes play S (1 held + 3 = the cap 4)
  }
  CHECK(r.pool->stereo_voices() == 4u);
  r.block();
  CHECK(r.pool->feed_side(3) != nullptr);
}

TEST_CASE("insert host: cap: release_all returns the slot at the next boundary") {
  CapRig r;
  r.blocks(2);
  NEED(r.bind_track(0, kStWiden) == InsertHost::Bind::Ok);
  r.blocks(2);
  CHECK(r.pool->st_reserved() == 1u);
  r.h->release_all();  // main loop, no render: the instances go Off at once
  CHECK_FALSE(r.h->bound(0));
  r.block();
  CHECK(r.pool->st_reserved() == 0u);
}

TEST_CASE("insert host: cap: a forced bypass (OVER BUDGET) returns the slot") {
  CapRig r;
  alignas(8) static std::int16_t blk[kB];
  for (std::int16_t& s : blk) {
    s = 8000;
  }
  CHECK(r.g->params().push(core::ParamChange{audio::strip_param(0, gps::Source), 3.0f, 1}));
  r.g->feed(0, blk);  // a sounding strip, so the insert runs and is measured
  g_step = 150000;    // over the instance budget (50 % of the block): the per-instance net forces it dry
  r.blocks(2);
  NEED(r.bind_track(0, kStWiden) == InsertHost::Bind::Ok);
  r.blocks(3);
  CHECK(r.pool->st_reserved() == 1u);
  CHECK(r.h->stats().forced_instance == 0u);
  r.blocks(5);
  CHECK(r.h->stats().forced_instance == 1u);
  r.blocks(2);  // the ramp to bypassed is counted, then Bypassed is not
  CHECK(r.pool->st_reserved() == 0u);
}

// ---- OS-INSFX-ABI2 A7: the master stereo insert, ONE instance (docs/specs/OS-INSFX-ABI2.md 3.3) --------------------------------------------------
namespace {

std::uint32_t st_nan_render(void*, const float*, const float*, float* ol, float* orr, std::uint32_t n) {  // NaN on both planes
  ++g_st_calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    ol[i] = std::nanf("");
    orr[i] = std::nanf("");
  }
  return 1;
}
std::uint32_t st_mixedbad_render(void*, const float* il, const float* ir, float* ol, float* orr, std::uint32_t n) {  // finite samples with a NaN and two infinities among them
  ++g_st_calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    ol[i] = il[i];
    orr[i] = ir[i];
  }
  ol[3] = orr[3] = std::nanf("");
  ol[17] = orr[17] = HUGE_VALF;
  ol[40] = orr[40] = -HUGE_VALF;
  return 1;
}
const o3m_module kStNan = FX_ST_MODULE("st_nan", st_nan_render, 0u);
const o3m_module kStMixedBad = FX_ST_MODULE("st_mixedbad", st_mixedbad_render, 0u);

// A master bind of a stereo module: ONE state block is taken (A7); `state_r` is passed too, so the S6 pair would bind with it before the fix.
InsertHost::Bind bind_master_st(Rig& r, const o3m_module& m, unsigned s, const float* params = nullptr, unsigned count = 0, bool force = false) {
  return r.h->bind(kMaster, m, 0x8001, &r.st[s], reinterpret_cast<const std::uint8_t*>(params), count * 4u, force, &r.st[s + 1u]);
}
// How many records the host holds on the master's keys (kMaster, kMaster + 1).
unsigned master_records(const Rig& r) {
  unsigned n = 0;
  InsertHost::Info in{};
  for (unsigned i = 0; i < InsertHost::kInstances; ++i) {
    if (r.h->info(i, in) && (in.where == kMaster || in.where == kMaster + 1u) && !in.leaving) {
      ++n;
    }
  }
  return n;
}

}  // namespace

TEST_CASE("master stereo: an identity stereo effect on the master is bit for bit L and R, one instance, render_st in place [OS-INSFX-ABI2 A7]") {
  static std::int16_t ref[kIdBlocks][kBS];
  static std::int16_t got[kIdBlocks][kBS];
  identity_run(false, ref);
  {
    Rig r;
    r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
    r.g->set_master_bypass(audio::MasterStage::Headroom, false);
    CHECK(r.g->params().push(core::ParamChange{audio::strip_param(0, gps::Pan), 0.0f, 1}));
    CHECK(r.g->params().push(core::ParamChange{audio::strip_param(1, gps::Pan), 100.0f, 1}));
    CHECK(r.g->params().push(core::ParamChange{audio::strip_param(0, gps::Source), 3.0f, 1}));
    CHECK(r.g->params().push(core::ParamChange{audio::strip_param(1, gps::Source), 3.0f, 1}));
    // the gain parameter 0.5: the S6 dual-mono pair would run the mono render and halve both planes; the stereo instance ignores it and passes L and R through
    const float half = 0.5f;
    NEED(bind_master_st(r, kStId, 0, &half, 1) == InsertHost::Bind::Ok);
    CHECK(r.h->bound(kMaster));
    CHECK_FALSE(r.h->bound(kMaster + 1u));
    CHECK(master_records(r) == 1u);  // ONE record of the four
    g_st_calls = 0;
    for (unsigned b = 0; b < kIdBlocks; ++b) {
      for (std::uint32_t i = 0; i < kB; ++i) {
        r.feed[0][i] = static_cast<std::int16_t>(((i + b * 7u) * 37u) % 9000u);
        r.feed[1][i] = static_cast<std::int16_t>(-static_cast<int>(((i + b * 5u) * 53u) % 11000u));
      }
      r.g->feed(0, r.feed[0]);
      r.g->feed(1, r.feed[1]);
      r.block();
      std::memcpy(got[b], r.out, sizeof got[b]);
    }
    CHECK(g_st_calls >= kIdBlocks - 2u);  // render_st ran (the pair would have called the mono render instead)
  }
  CHECK(std::memcmp(ref, got, sizeof ref) == 0);  // every block, the ramp-in blocks included, L and R bit for bit
  bool left = false;
  bool right = false;
  for (std::uint32_t i = 0; i < kBS; i += 2) {
    left = left || got[kIdBlocks - 1][i] != 0;
    right = right || got[kIdBlocks - 1][i + 1] != 0;
  }
  CHECK(left);   // (the two planes did carry different sound: L and R independent)
  CHECK(right);
}

TEST_CASE("master stereo: a NaN or an infinity from render_st never reaches the limiter or the converter [OS-INSFX-ABI2 A7]") {
  {
    Rig r;
    static test::StaticSlot<audio::MasterLimiter> lslot;
    audio::MasterLimiter& lim = lslot.make();
    r.g->install_master_limiter(lim);
    r.dc(0, 8000);
    r.blocks(6);
    NEED(bind_master_st(r, kStNan, 0) == InsertHost::Bind::Ok);
    CHECK(master_records(r) == 1u);
    for (unsigned b = 0; b < 40; ++b) {
      r.block();
      CHECK(lim.finite());  // the limiter's delay line and followers hold no NaN or infinity, at any block
    }
    CHECK(all_zero(r, false));  // the scrubbed output is silence, not the +32767 rail the converter gives a NaN
    CHECK(all_zero(r, true));
    CHECK(r.h->stats().scrubbed >= 2u * kB * 30u);
  }
  {  // one NaN and two infinities in a block of finite samples: only those samples go, the rest is the input exactly
    Rig r;
    r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
    r.g->set_master_bypass(audio::MasterStage::Headroom, false);
    r.dc(0, 8000);
    r.blocks(6);
    const std::uint32_t before = r.h->stats().scrubbed;
    NEED(bind_master_st(r, kStMixedBad, 1) == InsertHost::Bind::Ok);
    CHECK(master_records(r) == 1u);
    r.blocks(4);
    const std::int16_t keep = r.l(10);
    CHECK(r.l(3) == 0);
    CHECK(r.l(17) == 0);
    CHECK(r.l(40) == 0);
    CHECK(r.l(11) == keep);
    CHECK(rch(r, 3) == 0);
    CHECK(rch(r, 17) == 0);
    CHECK(rch(r, 40) == 0);
    CHECK(rch(r, 11) == keep);
    CHECK(r.h->stats().scrubbed - before >= 2u * 3u);  // both planes, three bad samples each
  }
}

TEST_CASE("master stereo: one instance counts 1 of the cap of four, a mono effect on the master still 2 [OS-INSFX-ABI2 A7]") {
  Rig r;
  r.dc(0, 8000);
  r.blocks(4);
  NEED(r.bind(0, kGain, 0) == InsertHost::Bind::Ok);
  NEED(r.bind(1, kGain, 1) == InsertHost::Bind::Ok);
  NEED(r.bind(2, kGain, 2) == InsertHost::Bind::Ok);
  // three track inserts leave ONE record free: the stereo master fits (A7), a mono one does not (S6)
  NEED(bind_master_st(r, kStWiden, 3) == InsertHost::Bind::Ok);
  CHECK(r.h->bound(kMaster));
  CHECK_FALSE(r.h->bound(kMaster + 1u));
  CHECK(master_records(r) == 1u);
  CHECK(r.bind(3, kGain, 3) == InsertHost::Bind::NoInstance);  // the four records are taken
  g_st_calls = 0;
  r.blocks(4);
  CHECK(g_st_calls >= 3u);  // the stage ran render_st on the pair
  CHECK(r.g->master_stage_active(audio::MasterStage::Insert));
  // the master leaves: its one record is free, a track insert fits again
  CHECK(r.h->unbind(kMaster));
  r.blocks(6);
  CHECK(master_records(r) == 0u);
  NEED(r.bind(3, kGain, 3) == InsertHost::Bind::Ok);
  // with one record free (three tracks in) a MONO master still needs two and is refused, nothing half bound
  CHECK(r.h->unbind(3));
  r.blocks(6);
  CHECK(bind_master_st(r, kGain, 0) == InsertHost::Bind::NoInstance);
  CHECK(master_records(r) == 0u);
  CHECK(r.h->bind(kMaster + 1u, kGain, 0x8001, &r.st[0], nullptr, 0, true) == InsertHost::Bind::BadWhere);
}

// ---- OS-INSFX-ABI2 A7 fold: the stereo master's transitions ramp like the pair's, and its lifecycle as ONE instance ----------------
namespace {
std::uint32_t st_gain_render(void* s, const float* il, const float* ir, float* ol, float* orr, std::uint32_t n) {  // a stereo gain: both planes by d->gain
  FxState* d = static_cast<FxState*>(s);
  ++g_st_calls;
  for (std::uint32_t i = 0; i < n; ++i) {
    ol[i] = il[i] * d->gain;
    orr[i] = ir[i] * d->gain;
  }
  return 1;
}
const o3m_module kStGain = FX_ST_MODULE("st_gain", st_gain_render, 0u);
}  // namespace

TEST_CASE("master stereo: a block straddling bind and one straddling bypass are ramped, never a step [OS-INSFX-ABI2 A7]") {
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  r.dc(0, 8000);
  const int base = r.level_after_warmup();
  NEED(base > 100);
  const float half = 0.5f;
  NEED(bind_master_st(r, kStGain, 0, &half, 1) == InsertHost::Bind::Ok);
  CHECK(master_records(r) == 1u);
  const int step = base / static_cast<int>(kB) + 2;  // the ramp's largest per-sample move against dry
  r.block();                                        // the boundary applies the Activate (the render of this block was dry)
  CHECK(r.phase(0) == InsertHost::Phase::In);
  r.block();  // the ramp-in block: dry at the first sample, wet (half) at the last, both planes
  CHECK(r.l(0) > base - base / 8);
  CHECK(rch(r, 0) > base - base / 8);
  CHECK(std::abs(r.l(kB - 1) - base / 2) <= 3);
  CHECK(std::abs(rch(r, kB - 1) - base / 2) <= 3);
  for (std::uint32_t i = 1; i < kB; ++i) {
    CHECK(std::abs(static_cast<int>(r.l(i)) - static_cast<int>(r.l(i - 1))) <= step);
    CHECK(std::abs(static_cast<int>(rch(r, i)) - static_cast<int>(rch(r, i - 1))) <= step);
  }
  r.block();
  CHECK(r.steady_near(base / 2, 3));  // steady: full wet, no copy, both planes
  CHECK(r.phase(0) == InsertHost::Phase::On);
  // bypass on: the OutBypass block ramps from wet back to dry over one block, no step
  CHECK(r.h->set_bypass(kMaster, true) == InsertHost::Toggle::Done);
  r.block();  // the Bypass applied at its boundary (this block's render was steady wet)
  r.block();  // the ramp-out block
  CHECK(std::abs(r.l(0) - base / 2) <= base / 8);  // starts wet
  CHECK(std::abs(r.l(kB - 1) - base) <= 3);        // ends dry
  CHECK(std::abs(rch(r, kB - 1) - base) <= 3);
  for (std::uint32_t i = 1; i < kB; ++i) {
    CHECK(std::abs(static_cast<int>(r.l(i)) - static_cast<int>(r.l(i - 1))) <= step);
    CHECK(std::abs(static_cast<int>(rch(r, i)) - static_cast<int>(rch(r, i - 1))) <= step);
  }
  r.blocks(2);
  CHECK(r.phase(0) == InsertHost::Phase::Bypassed);
  CHECK(r.steady_near(base, 3));
  CHECK_FALSE(r.g->master_stage_active(audio::MasterStage::Insert));
}

TEST_CASE("master stereo: the one instance's lifecycle: set_param and a ParamChange reach it, bypass, forced bypass, reset, export, unbind [OS-INSFX-ABI2 A7]") {
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  r.dc(0, 8000);
  r.blocks(6);
  NEED(bind_master_st(r, kStGain, 0) == InsertHost::Bind::Ok);
  r.blocks(6);
  CHECK(master_records(r) == 1u);
  CHECK(r.h->bound(kMaster));
  CHECK_FALSE(r.h->bound(kMaster + 1u));
  // set_param (key 32) reaches the ONE record and returns true only with the wh_[where + 1] == kNone guard (without it, set_param_at(33) fails)
  NEED(r.h->set_param(kMaster, 0, 0.25f));
  r.blocks(3);
  CHECK(r.st[0].gain == 0.25f);
  CHECK(r.h->stats().params == 1u);  // one ParamChange applied, one instance
  CHECK(r.h->stats().stale == 0);
  CHECK_FALSE(r.h->set_param(kMaster + 1u, 0, 0.9f));
  // a ParamChange through the graph's ring (the key 32) reaches the one record too
  InsertHost::Info inf{};
  NEED(r.h->info(0, inf));
  CHECK(inf.where == kMaster);
  CHECK(r.g->params().push(core::ParamChange{core::param_id(core::ParamKind::Insert, kMaster, 0), 0.5f, inf.gen}));
  r.block();
  CHECK(r.st[0].gain == 0.5f);
  NEED(r.h->set_param(kMaster, 0, 0.5f));  // and the stored copy (the blob) takes it too, so a reset applies 0.5
  r.blocks(3);
  CHECK(r.st[0].gain == 0.5f);
  // bypass on: dry, the stage out; off: a reset of the ONE instance (shutdown + init once), the stored parameter again
  CHECK(r.h->set_bypass(kMaster, true) == InsertHost::Toggle::Done);
  r.blocks(4);
  CHECK(r.phase(0) == InsertHost::Phase::Bypassed);
  CHECK_FALSE(r.g->master_stage_active(audio::MasterStage::Insert));
  const int shut = g_shut;
  const int inits = g_init;
  CHECK(r.h->set_bypass(kMaster, false, true) == InsertHost::Toggle::Done);
  r.blocks(10);
  CHECK(g_shut == shut + 1);
  CHECK(g_init == inits + 1);
  CHECK(r.st[0].gain == 0.5f);
  CHECK(master_records(r) == 1u);
  CHECK(r.g->master_stage_active(audio::MasterStage::Insert));
  // a net's forced bypass takes the one instance to dry together
  g_step = 150000;  // each render is over the instance budget (50 % of the block)
  r.blocks(8);
  CHECK(r.h->stats().forced_instance >= 1u);
  NEED(r.h->info(0, inf));
  CHECK(inf.where == kMaster);
  CHECK(inf.forced);
  CHECK(inf.phase == InsertHost::Phase::Bypassed);
  CHECK(r.h->set_bypass(kMaster, false) == InsertHost::Toggle::NoCpu);  // one instance at the measured cost: the net would force it again
  CHECK(r.h->set_bypass(kMaster, false, true) == InsertHost::Toggle::Done);
  g_step = 0;
  r.blocks(14);
  CHECK(r.h->bound(kMaster));
  // export: the one instance's state is reset (one shutdown + init), two exports give the same bits
  static std::int16_t a[8][kBS];
  static std::int16_t b[8][kBS];
  const int shut2 = g_shut;
  const int inits2 = g_init;
  CHECK(r.h->prepare_export());
  CHECK(g_shut == shut2 + 1);
  CHECK(g_init == inits2 + 1);
  CHECK(r.st[0].gain == 0.5f);
  for (unsigned i = 0; i < 8; ++i) {
    r.block();
    std::memcpy(a[i], r.out, sizeof a[i]);
  }
  r.h->export_end();
  r.blocks(5);
  CHECK(r.h->prepare_export());
  for (unsigned i = 0; i < 8; ++i) {
    r.block();
    std::memcpy(b[i], r.out, sizeof b[i]);
  }
  CHECK(std::memcmp(a, b, sizeof a) == 0);
  bool any = false;
  for (std::uint32_t i = 0; i < kBS; ++i) {
    any = any || a[7][i] != 0;
  }
  CHECK(any);
  r.h->export_end();
  // unbind: the stage leaves, the one record is freed
  CHECK(r.h->unbind(kMaster));
  r.blocks(8);
  CHECK_FALSE(r.h->bound(kMaster));
  CHECK(master_records(r) == 0u);
  CHECK_FALSE(r.g->master_stage_active(audio::MasterStage::Insert));
  CHECK(r.h->settled());
}

// ---- OS-INSFX-ABI2 A7 fold 2: a transition block must leave the chain-wet planes (the CUE accumulator) zero ----------------
namespace {
// OUT MODE MainLCueR (the right jack carries CUE), track 0 to MAIN, track 1 to CUE when `cue` is set (else to MAIN, CUE silent); the stereo master binds at block 6
// when `with_insert` (block 7 is then the transition block). Returns the right jack (CUE's mono sum) of every block.
std::vector<std::int16_t> cue_scene(bool with_insert, bool cue) {
  Rig r;
  r.g->install_master_stage(audio::MasterStage::Headroom, &stage_pass, nullptr);
  r.g->set_master_bypass(audio::MasterStage::Headroom, false);
  CHECK(r.g->params().push(core::ParamChange{audio::master_param(gpm::OutMode), 1.0f, 1}));  // MainLCueR
  CHECK(r.g->params().push(core::ParamChange{audio::strip_param(0, gps::Source), 3.0f, 1}));
  CHECK(r.g->params().push(core::ParamChange{audio::strip_param(0, gps::Route), static_cast<float>(audio::route::kMain), 1}));
  CHECK(r.g->params().push(core::ParamChange{audio::strip_param(1, gps::Source), 3.0f, 1}));
  CHECK(r.g->params().push(core::ParamChange{audio::strip_param(1, gps::Route),
                                               static_cast<float>(cue ? audio::route::kCue : audio::route::kMain), 1}));
  CHECK(r.g->params().push(core::ParamChange{audio::strip_param(1, gps::Pan), 100.0f, 1}));
  std::vector<std::int16_t> out_cue;
  for (unsigned b = 0; b < 16; ++b) {
    if (with_insert && b == 6u) {
      const float one = 1.0f;
      CHECK(bind_master_st(r, kStGain, 0, &one, 1) == InsertHost::Bind::Ok);
    }
    for (std::uint32_t i = 0; i < kB; ++i) {
      r.feed[0][i] = static_cast<std::int16_t>(((i * 37u + b * 11u) % 9000u) - 4000);
      r.feed[1][i] = static_cast<std::int16_t>(-static_cast<int>(((i * 53u + b * 7u) % 11000u) - 5000));
    }
    r.g->feed(0, r.feed[0]);
    r.g->feed(1, r.feed[1]);
    r.block();
    for (std::uint32_t i = 0; i < kB; ++i) {
      out_cue.push_back(rch(r, i));
    }
  }
  return out_cue;
}
}  // namespace

TEST_CASE("master stereo: a transition block leaves the CUE accumulator zero: the next blocks' CUE output is the run without the insert [OS-INSFX-ABI2 A7]") {
  const std::vector<std::int16_t> ref = cue_scene(false, true);  // CUE carries track 1
  const std::vector<std::int16_t> got = cue_scene(true, true);   // the stereo master binds at block 6, block 7 is the transition
  NEED(ref.size() == got.size());
  CHECK(ref == got);  // every block, the transition block's successors included: the chain-wet planes are zeroed again after the blend
  bool any = false;
  for (std::int16_t s : ref) {
    any = any || s != 0;
  }
  CHECK(any);  // (CUE really carried signal)
  // CUE silent: after the transition block the accumulator is all zero, so the CUE jack stays silent
  const std::vector<std::int16_t> silent = cue_scene(true, false);
  for (unsigned b = 8u; b < 16u; ++b) {  // the blocks after the transition
    for (std::uint32_t i = 0; i < kB; ++i) {
      CHECK(silent[b * kB + i] == 0);
    }
  }
}
