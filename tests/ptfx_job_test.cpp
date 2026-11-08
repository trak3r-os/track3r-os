// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S3a: the plugin run job (app/ptfx_job.h, docs/specs/OS-CEPTFX.md 2.2 and 3.5) with a host stand-in plugin through PtfxJob::Plugin::call: the call sequence, the
// block splitting at 8,192 frames and at the prepend / body / append boundaries, the extra samples (A10) and their clamp to the edit pool, a return of 0 or less (A11), one undo step,
// the stereo modes of S5 (one pass, two passes, to mono, the no-op round trip, a generator), the quarantine on a Timeout / Fault / StackOverflow with the bytes untouched, the Why::Crash catalogue rule, and long_step() against the runner's overrun count.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"

#include <o3os/app/ptfx_job.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/voices.h>
#include <o3os/core/job.h>
#include <o3os/model/instrument.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::PtfxJob;
using core::JobStatus;
using hal::code_gate::GateResult;

namespace {

constexpr std::uintptr_t kInit = 0x100;
constexpr std::uintptr_t kSet = 0x200;
constexpr std::uintptr_t kProc = 0x300;

enum class Kind { Init, Set, Proc };
struct Ev {
  Kind kind;
  std::uint32_t n;  // Init: scratch bytes; Set: the index; Proc: the frames asked for
  float v;          // Set: the value
  bool in_null;     // Proc: in[0] == NULL
  std::uintptr_t channels;
};

// What the stand-in plugin does and what it saw. One plugin call runs at a time, as on the unit.
struct Fake {
  std::vector<Ev> ev;
  int calls = 0;
  int fail_at = -1;  // the call with this index returns `fail_with` and does nothing
  GateResult fail_with = GateResult::Ok;
  std::uint32_t want_prepend = 0;
  std::uint32_t want_append = 0;
  std::uint32_t produce_limit = 0xFFFFFFFFu;  // frames it will produce in all; after that it returns 0
  bool negative_end = false;                  // it returns -1 instead of 0 when it has nothing more
  bool over_return = false;                   // it claims one frame more than asked
  bool smash_guard = false;
  bool scratch_was_zero = false;
  bool guard_was_painted = false;
  bool table_ok = false;
  std::uint32_t second_prepend = 0xFFFFFFFFu;  // the prepend asked for from the second init on (0xFFFFFFFF: as want_prepend)
  int inits = 0;                           // init calls so far (a two-pass run starts the plugin twice)
  std::uint32_t limit_second = 0xFFFFFFFFu;  // produce_limit from the second init on (the R pass of a two-pass run)
  int gain[2] = {2, 2};                      // per channel: the stand-in's output is sat(in * gain), 7000 for a NULL input
  std::vector<std::int16_t> seen_in[2];      // the first block's inputs per channel (S5: what the stereo staging gave the plugin)
  std::uint8_t* guard = nullptr;
  std::uint32_t guard_bytes = 0;
  std::uintptr_t table = 0;
};
Fake g;

GateResult fake_call(const PtfxJob::Plugin& p, std::uintptr_t entry, std::uintptr_t r0, std::uintptr_t r1, std::uintptr_t r2, std::uintptr_t r3, float s0, std::int32_t* ret) noexcept {
  const int idx = g.calls++;
  if (idx == g.fail_at) {
    return g.fail_with;
  }
  *ret = 0;
  if (entry == kInit) {
    const auto* scratch = reinterpret_cast<const std::uint8_t*>(r0);
    g.scratch_was_zero = true;
    for (std::uintptr_t i = 0; i < r1; ++i) {
      g.scratch_was_zero = g.scratch_was_zero && scratch[i] == 0u;
    }
    g.table_ok = r2 == g.table && r2 == p.host_table;
    g.guard_was_painted = g.guard != nullptr;
    for (std::uint32_t i = 0; g.guard != nullptr && i < g.guard_bytes; ++i) {
      g.guard_was_painted = g.guard_was_painted && g.guard[i] == module::kPtfxGuardByte;
    }
    if (g.smash_guard && g.guard != nullptr) {
      g.guard[10] = 0;
    }
    module::PtfxHostTable::set_extra_samples(5, 5);  // overwritten by the later call of the replay
    if (g.inits++ > 0) {
      g.produce_limit = g.limit_second;
    }
    g.ev.push_back({Kind::Init, static_cast<std::uint32_t>(r1), 0.0f, false, 0});
  } else if (entry == kSet) {
    if (r0 == 3) {
      const std::uint32_t pre = (g.inits >= 2 && g.second_prepend != 0xFFFFFFFFu) ? g.second_prepend : g.want_prepend;  // (the R pass of a two-pass run may differ)
      module::PtfxHostTable::set_extra_samples(pre, g.want_append);  // inside the replay: latched
    }
    g.ev.push_back({Kind::Set, static_cast<std::uint32_t>(r0), s0, false, 0});
  } else {
    module::PtfxHostTable::set_extra_samples(900, 900);  // after the replay: ignored (A10)
    const auto* in = reinterpret_cast<const std::int16_t* const*>(r0);
    auto* out = reinterpret_cast<std::int16_t* const*>(r1);
    const std::uint32_t n = static_cast<std::uint32_t>(r2);
    g.ev.push_back({Kind::Proc, n, 0.0f, in[0] == nullptr, r3});
    const std::uint32_t make = n < g.produce_limit ? n : g.produce_limit;
    const std::uint32_t chans = r3 >= 2u ? 2u : 1u;
    for (std::uint32_t c = 0; c < chans; ++c) {
      if (g.seen_in[c].empty() && in[c] != nullptr) {
        g.seen_in[c].assign(in[c], in[c] + n);
      }
      for (std::uint32_t i = 0; i < make; ++i) {
        const int v = in[c] != nullptr ? in[c][i] * g.gain[c] : 7000;
        out[c][i] = static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
      }
    }
    g.produce_limit -= make;
    if (make == 0u) {
      *ret = g.negative_end ? -1 : 0;
    } else {
      *ret = static_cast<std::int32_t>(make) + (g.over_return ? 1 : 0);
    }
  }
  return GateResult::Ok;
}

// The edit pool stand-in: one buffer of `cap` frames, filled with a sentinel.
struct Pool {
  std::vector<std::int16_t> buf;
  unsigned calls = 0;
  std::uint32_t last = 0;
  bool last_stereo = false;
  explicit Pool(std::uint32_t cap) : buf(cap + 8, std::int16_t{-1}) {}
};
std::int16_t* pool_alloc(void* ctx, std::uint32_t frames, bool stereo) noexcept {
  auto* p = static_cast<Pool*>(ctx);
  ++p->calls;
  p->last = frames;
  p->last_stereo = stereo;
  const std::size_t need = stereo ? std::size_t{2} * ((frames + 1u) & ~1u) : frames;  // the bank's layout: an M and an S block of even(frames)
  return need <= p->buf.size() - 8u ? p->buf.data() : nullptr;
}

void put16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t x) {
  v[at] = static_cast<std::uint8_t>(x);
  v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
  put16(v, at, static_cast<std::uint16_t>(x));
  put16(v, at + 2, static_cast<std::uint16_t>(x >> 16));
}
std::vector<std::uint8_t> make_ptfx(const char* display_name) {
  std::vector<std::uint8_t> v(module::kPtfxHeaderSize + 64, 0);
  put32(v, 0, module::kPtfxMagic);
  put16(v, 4, 1);
  std::memcpy(&v[8], display_name, std::strlen(display_name));
  put32(v, 64, module::kPtfxHeaderSize);
  put32(v, 68, 64);
  put32(v, 80, 8);
  put32(v, 84, 16);
  return v;
}

struct PRig {
  module::PtfxInfo info{};
  module::PtfxHostTableStorage store{};
  module::PtfxHostTable host{store.bytes};
  std::vector<std::uint8_t> scratch = std::vector<std::uint8_t>(200000, 0xFFu);
  alignas(4) std::uint8_t guard[4096];
  module::PtfxQuarantine quarantine;
  test::MemCard card;
  module::PtfxEntry entries[module::kPtfxCatalogMax];
  module::PtfxCatalog cat{entries};
  PtfxJob::Plugin plugin;
  float values[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  std::vector<std::int16_t> stage = std::vector<std::int16_t>(2u * app::kPtfxBlockFrames);  // the stereo staging (the ptfx region's 32 KiB on the unit)

  PRig() {
    plugin.stage = stage.data();
    g = Fake{};
    std::memset(guard, 0, sizeof guard);  // the job paints it before init
    g.guard = guard;
    g.guard_bytes = sizeof guard;
    g.table = reinterpret_cast<std::uintptr_t>(store.bytes);
    info.status = module::PtfxStatus::Ok;
    std::strcpy(info.name, "Gain");
    info.num_params = 3;
    info.scratch_size = 100000;
    info.params[0].def = 0.5f;
    info.params[1].def = 0.25f;
    info.params[2].def = 2.0f;
    card.make_dir_now("Effects");
    card.put("Effects/gain.ptfx", make_ptfx("Gain"));
    cat.begin(card);
    for (unsigned n = 0; n < 1000u && cat.step(2000) == JobStatus::Running; ++n) {
    }
    plugin.info = &info;
    plugin.entries = {kInit, kSet, kProc};
    plugin.host = &host;
    plugin.host_table = g.table;
    plugin.scratch = scratch.data();
    plugin.scratch_cap = static_cast<std::uint32_t>(scratch.size());
    plugin.guard = guard;
    plugin.guard_bytes = sizeof guard;
    plugin.id = module::ptfx_plugin_id("gain.ptfx");
    plugin.sample_rate = 44100;
    plugin.quarantine = &quarantine;
    plugin.catalog = &cat;
    plugin.call = &fake_call;
  }
};

JobStatus run(PtfxJob& j) {
  for (unsigned n = 0; n < 100000u; ++n) {
    const JobStatus s = j.step(1000);
    if (s != JobStatus::Running) {
      return s;
    }
  }
  return JobStatus::Running;
}

std::vector<std::int16_t> ramp(std::uint32_t frames) {
  std::vector<std::int16_t> v(frames);
  for (std::uint32_t i = 0; i < frames; ++i) {
    v[i] = static_cast<std::int16_t>(i % 100u + 1u);
  }
  return v;
}

PtfxJob::Source source(const std::vector<std::int16_t>& pcm, Pool& pool, std::uint32_t a = 0, std::uint32_t b = 0) {
  PtfxJob::Source s;
  s.pcm = pcm.data();
  s.frames = static_cast<std::uint32_t>(pcm.size());
  s.a = a;
  s.b = b;
  s.alloc = &pool_alloc;
  s.alloc_ctx = &pool;
  return s;
}

std::vector<std::uint32_t> proc_blocks() {
  std::vector<std::uint32_t> v;
  for (const Ev& e : g.ev) {
    if (e.kind == Kind::Proc) {
      v.push_back(e.n);
    }
  }
  return v;
}

// ---- S5: stereo (docs/specs/OS-CEPTFX.md 3.6): a sample as the WAV importer leaves it, M = (L + R + 1) >> 1 and S = clamp16(L - M), M then S in one block of 2 x even(frames) ----
std::uint32_t even_of(std::uint32_t n) { return (n + 1u) & ~1u; }
std::int32_t sat16i(std::int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }

struct MS {
  std::vector<std::int16_t> buf;
  std::uint32_t n = 0;
  std::int16_t* m() { return buf.data(); }
  std::int16_t* s() { return buf.data() + even_of(n); }
  const std::int16_t* m() const { return buf.data(); }
  const std::int16_t* s() const { return buf.data() + even_of(n); }
};

// Pseudo-random L and R with the full-scale corners mixed in (the saturation edges of L = sat(M + S) and R = sat(M - S)).
MS make_ms(std::uint32_t n) {
  MS x;
  x.n = n;
  x.buf.assign(2u * even_of(n), 0);
  std::uint32_t rng = 12345u;
  for (std::uint32_t i = 0; i < n; ++i) {
    rng = rng * 1664525u + 1013904223u;
    std::int32_t l = static_cast<std::int16_t>(rng >> 16);
    rng = rng * 1664525u + 1013904223u;
    std::int32_t r = static_cast<std::int16_t>(rng >> 16);
    if (i % 97u == 0u) {
      l = 32767;
      r = -32768;
    } else if (i % 89u == 0u) {
      l = -32768;
      r = 32767;
    } else if (i % 83u == 0u) {
      l = r = 32767;
    } else if (i % 79u == 0u) {
      l = r = -32768;
    } else if (i % 7u == 0u) {
      l /= 64;  // quiet parts too
      r /= 64;
    }
    const std::int32_t mi = (l + r + 1) >> 1;
    x.m()[i] = static_cast<std::int16_t>(mi);
    x.s()[i] = static_cast<std::int16_t>(sat16i(l - mi));
  }
  return x;
}

struct Ref {
  std::vector<std::int16_t> m;
  std::vector<std::int16_t> s;
};

// What a run must give: [0, a) as it is, `produced` frames of the body through L' = sat(gl * L) and R' = sat(gr * R) back to M and S, then [b, n) as it is.
Ref expect_ms(const MS& in, std::uint32_t a, std::uint32_t b, std::uint32_t produced, int gl, int gr) {
  Ref r;
  for (std::uint32_t i = 0; i < a; ++i) {
    r.m.push_back(in.m()[i]);
    r.s.push_back(in.s()[i]);
  }
  for (std::uint32_t i = 0; i < produced; ++i) {
    const std::int32_t l = sat16i(gl * sat16i(in.m()[a + i] + in.s()[a + i]));
    const std::int32_t rr = sat16i(gr * sat16i(in.m()[a + i] - in.s()[a + i]));
    const std::int32_t mi = (l + rr + 1) >> 1;
    r.m.push_back(static_cast<std::int16_t>(mi));
    r.s.push_back(static_cast<std::int16_t>(sat16i(l - mi)));
  }
  for (std::uint32_t i = b; i < in.n; ++i) {
    r.m.push_back(in.m()[i]);
    r.s.push_back(in.s()[i]);
  }
  return r;
}

PtfxJob::Source ms_source(const MS& in, Pool& pool, std::uint32_t a = 0, std::uint32_t b = 0) {
  PtfxJob::Source s;
  s.pcm = in.m();
  s.side = in.s();
  s.stereo = true;
  s.frames = in.n;
  s.a = a;
  s.b = b;
  s.alloc = &pool_alloc;
  s.alloc_ctx = &pool;
  return s;
}

// The finished run's buffer is `ref` in both blocks, S where the bank wants it: at result() + even(frames).
void check_ms(const PtfxJob& job, const Ref& ref) {
  const std::uint32_t n = job.result_frames();
  NEED(n == ref.m.size());
  CHECK(job.stereo_result());
  const std::int16_t* m = job.result();
  const std::int16_t* s = m + even_of(n);
  std::uint32_t bad_m = 0;
  std::uint32_t bad_s = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    bad_m += m[i] != ref.m[i] ? 1u : 0u;
    bad_s += s[i] != ref.s[i] ? 1u : 0u;
  }
  CHECK(bad_m == 0u);
  CHECK(bad_s == 0u);
}

}  // namespace

TEST_CASE("ptfx_job: the call sequence is init, 12 set_param calls (0.0 past num_params), then the process_block blocks") {
  PRig r;
  Pool pool(40000);
  const auto pcm = ramp(20000);
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
  CHECK(job.long_step());
  CHECK(run(job) == JobStatus::Done);
  CHECK(job.done());
  NEED(g.ev.size() == 1u + 12u + 3u);
  CHECK(g.ev[0].kind == Kind::Init);
  CHECK(g.ev[0].n == 100000u);  // scratch_size, within the capacity
  CHECK(g.scratch_was_zero);    // zeroed before init (2.2)
  CHECK(g.table_ok);
  CHECK(g.guard_was_painted);
  for (unsigned i = 0; i < 12u; ++i) {
    CHECK(g.ev[1 + i].kind == Kind::Set);
    CHECK(g.ev[1 + i].n == i);
  }
  CHECK(g.ev[1].v == 0.5f);
  CHECK(g.ev[2].v == 0.25f);
  CHECK(g.ev[3].v == 2.0f);
  for (unsigned i = 3; i < 12u; ++i) {
    CHECK(g.ev[1 + i].v == 0.0f);
  }
  CHECK(r.scratch[100000] == 0xFFu);  // only what init was given was zeroed
  CHECK(r.scratch[99999] == 0u);
  // 20000 frames: 8192 + 8192 + 3616, one channel, `in` given.
  const auto blocks = proc_blocks();
  NEED(blocks.size() == 3u);
  CHECK(blocks[0] == 8192u);
  CHECK(blocks[1] == 8192u);
  CHECK(blocks[2] == 3616u);
  for (unsigned i = 0; i < 3u; ++i) {
    CHECK_FALSE(g.ev[13 + i].in_null);
    CHECK(g.ev[13 + i].channels == 1u);
  }
  CHECK(job.result_frames() == 20000u);
  NEED(job.result() == pool.buf.data());
  CHECK(pool.buf[0] == pcm[0] * 2);
  CHECK(pool.buf[19999] == pcm[19999] * 2);
  CHECK(pool.last == 20000u);
  CHECK(job.progress() == core::kJobProgressMax);
}

TEST_CASE("ptfx_job: the parameter values given replace the defaults, all 12 indices are still replayed") {
  PRig r;
  r.plugin.values = r.values;
  Pool pool(100);
  const auto pcm = ramp(50);
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  NEED(g.ev.size() >= 13u);
  CHECK(g.ev[1].v == 1.0f);
  CHECK(g.ev[3].v == 3.0f);
  CHECK(g.ev[4].v == 0.0f);
  CHECK(g.ev[12].v == 0.0f);
}

TEST_CASE("ptfx_job: a selection is processed alone, the rest is copied; blocks split at the prepend, body and append boundaries and in is NULL outside the body") {
  PRig r;
  g.want_prepend = 3000;
  g.want_append = 100;
  Pool pool(40000);
  const auto pcm = ramp(30000);
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool, 10000, 20000)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  const auto blocks = proc_blocks();
  NEED(blocks.size() == 4u);
  CHECK(blocks[0] == 3000u);  // the prepend alone
  CHECK(blocks[1] == 8192u);
  CHECK(blocks[2] == 1808u);  // the end of the body
  CHECK(blocks[3] == 100u);   // the append alone
  CHECK(g.ev[13].in_null);
  CHECK_FALSE(g.ev[14].in_null);
  CHECK_FALSE(g.ev[15].in_null);
  CHECK(g.ev[16].in_null);
  CHECK(job.result_frames() == 33100u);
  const auto& o = pool.buf;
  for (std::uint32_t i = 0; i < 10000u; ++i) {
    CHECK(o[i] == pcm[i]);  // before the selection: copied
  }
  for (std::uint32_t i = 0; i < 3000u; ++i) {
    CHECK(o[10000 + i] == 7000);  // the prepend
  }
  for (std::uint32_t i = 0; i < 10000u; ++i) {
    CHECK(o[13000 + i] == pcm[10000 + i] * 2);  // the body
  }
  for (std::uint32_t i = 0; i < 100u; ++i) {
    CHECK(o[23000 + i] == 7000);  // the append
  }
  for (std::uint32_t i = 0; i < 10000u; ++i) {
    CHECK(o[23100 + i] == pcm[20000 + i]);  // after the selection: copied
  }
  CHECK(o[33100] == -1);  // nothing past the result
}

TEST_CASE("ptfx_job: set_extra_samples counts only in init and the replay") {
  PRig r;
  g.want_prepend = 12;
  g.want_append = 34;
  Pool pool(500);
  const auto pcm = ramp(100);
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK(job.result_frames() == 146u);  // not the (5, 5) of init nor the (900, 900) of process_block
  CHECK_FALSE(r.host.armed());
}

TEST_CASE("ptfx_job: a request beyond the edit pool fails the run with a message and writes nothing") {
  {
    PRig r;
    g.want_prepend = 2000;  // 4000 + 2000 > 5000
    Pool pool(5000);
    const auto pcm = ramp(4000);
    PtfxJob job;
    CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
    CHECK(run(job) == JobStatus::Failed);
    CHECK(std::strcmp(job.detail(), "NO ROOM FOR THE EDIT") == 0);
    CHECK(proc_blocks().empty());
    for (std::uint32_t i = 0; i < pool.buf.size(); ++i) {
      CHECK(pool.buf[i] == -1);
    }
    CHECK_FALSE(r.quarantine.contains(r.plugin.id));  // a request the pool cannot hold is no fault of the plugin
  }
  {
    PRig r;  // 32-bit sums would wrap: the request is refused before the pool is asked
    g.want_prepend = 0xFFFFFFFFu;
    g.want_append = 0xFFFFFFFFu;
    Pool pool(5000);
    const auto pcm = ramp(4000);
    PtfxJob job;
    CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
    CHECK(run(job) == JobStatus::Failed);
    CHECK(std::strcmp(job.detail(), "NO ROOM FOR THE EDIT") == 0);
    CHECK(pool.calls == 0u);
    CHECK(proc_blocks().empty());
  }
  {
    PRig r;  // exactly the pool: allowed
    g.want_prepend = 1000;
    Pool pool(5000);
    const auto pcm = ramp(4000);
    PtfxJob job;
    CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
    CHECK(run(job) == JobStatus::Done);
    CHECK(job.result_frames() == 5000u);
    CHECK(pool.buf[5000] == -1);
  }
}

TEST_CASE("ptfx_job: a return of 0 or less ends the run with the frames produced so far (A11)") {
  {
    PRig r;
    g.produce_limit = 5000;
    Pool pool(30000);
    const auto pcm = ramp(20000);
    PtfxJob job;
    CHECK(job.begin(r.plugin, source(pcm, pool, 2000, 12000)) == nullptr);
    CHECK(run(job) == JobStatus::Done);
    const auto blocks = proc_blocks();
    CHECK(blocks.size() == 2u);  // 5000 of the first block, then the 0
    CHECK(job.result_frames() == 2000u + 5000u + 8000u);
    CHECK(pool.buf[2000] == pcm[2000] * 2);
    CHECK(pool.buf[6999] == pcm[6999] * 2);
    CHECK(pool.buf[7000] == pcm[12000]);  // the rest follows what was produced
    CHECK(pool.buf[14999] == pcm[19999]);
  }
  {
    PRig r;
    g.produce_limit = 100;
    g.negative_end = true;
    Pool pool(30000);
    const auto pcm = ramp(10000);
    PtfxJob job;
    CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
    CHECK(run(job) == JobStatus::Done);
    CHECK(job.result_frames() == 100u);
  }
  {
    PRig r;
    g.produce_limit = 0;  // produced nothing and nothing before or after: nothing to commit
    Pool pool(30000);
    const auto pcm = ramp(1000);
    PtfxJob job;
    CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
    CHECK(run(job) == JobStatus::Failed);
    CHECK(std::strcmp(job.detail(), "NOTHING PRODUCED") == 0);
  }
  {
    PRig r;
    g.over_return = true;  // more than asked for
    Pool pool(30000);
    const auto pcm = ramp(1000);
    PtfxJob job;
    CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
    CHECK(run(job) == JobStatus::Failed);
    CHECK(std::strcmp(job.detail(), "PLUGIN ERROR") == 0);
  }
}

// ---- OS-CEPTFX S5 (3.6, A12, A13): a stereo sample goes through every kind of plugin; the bank keeps M and S ----

namespace {
// At least one process_block ran and every one had this channel count.
bool all_procs_have(std::uintptr_t channels) {
  bool any = false;
  for (const Ev& e : g.ev) {
    if (e.kind == Kind::Proc) {
      if (e.channels != channels) {
        return false;
      }
      any = true;
    }
  }
  return any;
}
}  // namespace

TEST_CASE("ptfx_job: a mono sample is unchanged by S5: one channel, a mono buffer, no staging needed") {
  PRig r;
  r.plugin.stage = nullptr;
  Pool pool(5000);
  const auto pcm = ramp(1000);
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK_FALSE(pool.last_stereo);
  CHECK_FALSE(job.stereo_result());
  CHECK(all_procs_have(1u));
  CHECK(job.result_frames() == 1000u);
}

TEST_CASE("ptfx_job: a STEREO plugin takes a stereo sample in one pass with channels = 2; one that changes nothing gives the sample back bit for bit") {
  PRig r;
  r.info.flags = module::kPtfxFlagStereo;
  g.gain[0] = g.gain[1] = 1;
  Pool pool(40000);
  const MS in = make_ms(9001);  // two blocks (8,192 + 809), an odd length: S starts at 9,002
  PtfxJob job;
  CHECK(job.begin(r.plugin, ms_source(in, pool)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK(pool.last_stereo);
  CHECK(pool.last == 9001u);
  CHECK(g.inits == 1);
  CHECK(proc_blocks().size() == 2u);
  CHECK(all_procs_have(2u));
  check_ms(job, expect_ms(in, 0, in.n, in.n, 1, 1));
  Ref same;  // the identity: M and S as they were
  same.m.assign(in.m(), in.m() + in.n);
  same.s.assign(in.s(), in.s() + in.n);
  check_ms(job, same);
  // what the plugin was given: L = sat(M + S) and R = sat(M - S)
  NEED(g.seen_in[0].size() == 8192u && g.seen_in[1].size() == 8192u);
  bool staged = true;
  for (std::uint32_t i = 0; i < 8192u; ++i) {
    staged = staged && g.seen_in[0][i] == sat16i(in.m()[i] + in.s()[i]) && g.seen_in[1][i] == sat16i(in.m()[i] - in.s()[i]);
  }
  CHECK(staged);
}

TEST_CASE("ptfx_job: the two channels of a STEREO plugin are routed apart (L is channel 0, R channel 1)") {
  PRig r;
  r.info.flags = module::kPtfxFlagStereo;
  g.gain[0] = 2;
  g.gain[1] = 1;
  Pool pool(40000);
  const MS in = make_ms(9001);
  PtfxJob job;
  CHECK(job.begin(r.plugin, ms_source(in, pool)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  check_ms(job, expect_ms(in, 0, in.n, in.n, 2, 1));
}

TEST_CASE("ptfx_job: a plugin without STEREO runs twice, L then R with a re-init between; a no-op gives the sample back bit for bit") {
  {
    PRig r;
    g.gain[0] = g.gain[1] = 1;
    Pool pool(40000);
    const MS in = make_ms(9001);
    PtfxJob job;
    CHECK(job.begin(r.plugin, ms_source(in, pool)) == nullptr);
    CHECK(run(job) == JobStatus::Done);
    CHECK(pool.last_stereo);
    CHECK(g.inits == 2);
    CHECK(proc_blocks().size() == 4u);
    CHECK(all_procs_have(1u));
    Ref same;
    same.m.assign(in.m(), in.m() + in.n);
    same.s.assign(in.s(), in.s() + in.n);
    check_ms(job, same);
  }
  {
    PRig r;  // both channels through the same gain
    g.gain[0] = g.gain[1] = 2;
    Pool pool(40000);
    const MS in = make_ms(9001);
    PtfxJob job;
    CHECK(job.begin(r.plugin, ms_source(in, pool)) == nullptr);
    CHECK(run(job) == JobStatus::Done);
    check_ms(job, expect_ms(in, 0, in.n, in.n, 2, 2));
    CHECK(job.progress() == core::kJobProgressMax);
  }
}

TEST_CASE("ptfx_job: a selection on a stereo sample: the head and the tail stay M and S as they were, in both modes") {
  const std::uint16_t modes[2] = {module::kPtfxFlagStereo, 0};
  for (const std::uint16_t flags : modes) {
    PRig r;
    r.info.flags = flags;
    Pool pool(70000);
    const MS in = make_ms(20001);
    PtfxJob job;
    CHECK(job.begin(r.plugin, ms_source(in, pool, 300, 18001)) == nullptr);
    CHECK(run(job) == JobStatus::Done);
    check_ms(job, expect_ms(in, 300, 18001, 17701, 2, 2));
    CHECK(job.body_a() == 300u);
    CHECK(job.body_b() == 18001u);
  }
}

TEST_CASE("ptfx_job: a stereo run that ends early (A11) moves S down to where the bank wants it") {
  const std::uint16_t modes[2] = {module::kPtfxFlagStereo, 0};
  const std::uint32_t limits[2] = {3000, 2999};  // an odd result and an even one
  for (const std::uint16_t flags : modes) {
    for (const std::uint32_t limit : limits) {
      PRig r;
      r.info.flags = flags;
      g.produce_limit = limit;
      g.limit_second = limit;
      Pool pool(40000);
      const MS in = make_ms(9001);
      PtfxJob job;
      CHECK(job.begin(r.plugin, ms_source(in, pool, 1000, 6000)) == nullptr);
      CHECK(run(job) == JobStatus::Done);
      CHECK(job.result_frames() == 1000u + limit + 3001u);
      check_ms(job, expect_ms(in, 1000, 6000, limit, 2, 2));
    }
  }
}

TEST_CASE("ptfx_job: a plugin without STEREO that comes out a different length on R fails cleanly, A13") {
  PRig r;
  g.limit_second = 1000;  // L makes all 9,001 frames, R only 1,000
  Pool pool(40000);
  const MS in = make_ms(9001);
  PtfxJob job;
  CHECK(job.begin(r.plugin, ms_source(in, pool)) == nullptr);
  CHECK(run(job) == JobStatus::Failed);
  CHECK(std::strcmp(job.detail(), "STEREO: CHANNELS DIFFER") == 0);
  CHECK_FALSE(job.done());
  CHECK_FALSE(r.quarantine.contains(r.plugin.id));
  // the source was only read
  CHECK(std::memcmp(in.buf.data(), make_ms(9001).buf.data(), in.buf.size() * sizeof(std::int16_t)) == 0);
}

TEST_CASE("ptfx_job: a plugin without STEREO that asks for different extra samples on R fails the same way") {
  {
    PRig r;  // the same on both passes: fine, the result is 10 frames longer
    g.want_prepend = 10;
    const MS in = make_ms(2000);
    Pool pool(40000);
    PtfxJob job;
    CHECK(job.begin(r.plugin, ms_source(in, pool)) == nullptr);
    CHECK(run(job) == JobStatus::Done);
    CHECK(job.result_frames() == 2010u);
    CHECK(job.prepend() == 10u);
  }
  {
    PRig r;
    g.want_prepend = 10;
    g.second_prepend = 20;
    const MS in = make_ms(2000);
    Pool pool(40000);
    PtfxJob job;
    CHECK(job.begin(r.plugin, ms_source(in, pool)) == nullptr);
    CHECK(run(job) == JobStatus::Failed);
    CHECK(std::strcmp(job.detail(), "STEREO: CHANNELS DIFFER") == 0);
  }
}

TEST_CASE("ptfx_job: a CONVERT_TO_MONO plugin gets L and R over the whole sample whatever the selection; the result is L, mono") {
  PRig r;
  r.info.flags = module::kPtfxFlagConvertToMono;
  g.gain[0] = g.gain[1] = 1;
  Pool pool(40000);
  const MS in = make_ms(9001);
  PtfxJob job;
  CHECK(job.begin(r.plugin, ms_source(in, pool, 100, 200)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK(job.body_a() == 0u);
  CHECK(job.body_b() == 9001u);
  CHECK(all_procs_have(2u));
  CHECK(proc_blocks().size() == 2u);
  CHECK_FALSE(job.stereo_result());
  NEED(job.result_frames() == 9001u);
  bool is_l = true;
  for (std::uint32_t i = 0; i < 9001u; ++i) {
    is_l = is_l && job.result()[i] == sat16i(in.m()[i] + in.s()[i]);
  }
  CHECK(is_l);
}

TEST_CASE("ptfx_job: a generator on a stereo sample makes a mono result (A12)") {
  PRig r;
  r.info.flags = module::kPtfxFlagGenerator;
  Pool pool(100000);
  const MS in = make_ms(3000);
  PtfxJob job;
  CHECK(job.begin(r.plugin, ms_source(in, pool, 100, 200)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK_FALSE(pool.last_stereo);
  CHECK(pool.last == app::kPtfxGeneratorFrames);
  CHECK_FALSE(job.stereo_result());
  CHECK(job.result_frames() == app::kPtfxGeneratorFrames);
  CHECK(all_procs_have(1u));
  CHECK(job.result()[0] == 7000);
}

TEST_CASE("ptfx_job: a stereo run needs the S block and the staging, and a pool with room for both blocks") {
  {
    PRig r;
    Pool pool(40000);
    const MS in = make_ms(1000);
    PtfxJob job;
    PtfxJob::Source s = ms_source(in, pool);
    s.side = nullptr;
    CHECK(std::strcmp(job.begin(r.plugin, s), "PLUGIN NOT READY") == 0);
    CHECK(std::strcmp(job.not_ready_why(), "stereo") == 0);  // the console's reply names the check
    PtfxJob::Plugin no_stage = r.plugin;
    no_stage.stage = nullptr;
    CHECK(std::strcmp(job.begin(no_stage, ms_source(in, pool)), "PLUGIN NOT READY") == 0);
    CHECK(g.calls == 0);
    CHECK(pool.calls == 0u);
  }
  {
    PRig r;
    Pool pool(15000);  // 9,001 frames mono fit, M and S (18,004) do not
    const MS in = make_ms(9001);
    PtfxJob job;
    CHECK(job.begin(r.plugin, ms_source(in, pool)) == nullptr);
    CHECK(run(job) == JobStatus::Failed);
    CHECK(std::strcmp(job.detail(), "NO ROOM FOR THE EDIT") == 0);
  }
}

TEST_CASE("ptfx_job: the refusals before a start leave everything alone") {
  PRig r;
  Pool pool(5000);
  const auto pcm = ramp(1000);
  PtfxJob job;
  PtfxJob::Source empty = source(pcm, pool);
  empty.frames = 0;
  CHECK(std::strcmp(job.begin(r.plugin, empty), "NOTHING TO PROCESS") == 0);
  PtfxJob::Plugin bad = r.plugin;
  bad.entries.process_block = 0;
  CHECK(std::strcmp(job.begin(bad, source(pcm, pool)), "PLUGIN NOT READY") == 0);
  CHECK(std::strcmp(job.not_ready_why(), "plugin") == 0);
  job.clear_not_ready_why();
  CHECK(job.not_ready_why() == nullptr);
  PtfxJob::Source no_alloc = source(pcm, pool);
  no_alloc.alloc = nullptr;
  CHECK(std::strcmp(job.begin(r.plugin, no_alloc), "NOTHING TO PROCESS") == 0);
  CHECK(g.calls == 0);
}

// OS-CEPTFX S4c: a generator (flag 0x01) needs no source: it makes up to 88,200 frames from NULL inputs, the selection and the latch of extra samples are not read, and the result is trimmed
// to what it made. Add Silence is the same job with set_extra_samples: the body is copied around the inserted frames.
TEST_CASE("ptfx_job: a generator with no sample makes 88,200 frames from NULL inputs and the result is what it made") {
  PRig r;
  r.info.flags = module::kPtfxFlagGenerator;
  Pool pool(100000);
  PtfxJob job;
  PtfxJob::Source s;
  s.alloc = &pool_alloc;
  s.alloc_ctx = &pool;
  g.want_prepend = 50;  // a generator's latch is not read
  g.want_append = 60;
  CHECK(job.begin(r.plugin, s) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK(pool.calls == 1u);
  CHECK(pool.last == app::kPtfxGeneratorFrames);
  CHECK(job.result_frames() == app::kPtfxGeneratorFrames);
  CHECK(job.generator());
  CHECK(job.prepend() == 0u);
  CHECK(job.append() == 0u);
  const std::vector<std::uint32_t> blocks = proc_blocks();
  NEED(blocks.size() == 11u);  // 10 x 8,192 and 7,880
  CHECK(blocks.back() == 88200u - 10u * 8192u);
  bool nulls = true;
  for (const Ev& e : g.ev) {
    nulls = nulls && (e.kind != Kind::Proc || e.in_null);
  }
  CHECK(nulls);
  CHECK(job.result()[0] == 7000);
  CHECK(job.result()[88199] == 7000);
}

TEST_CASE("ptfx_job: a generator that stops early is trimmed; one that makes nothing fails") {
  PRig r;
  r.info.flags = module::kPtfxFlagGenerator;
  Pool pool(100000);
  PtfxJob job;
  PtfxJob::Source s;
  s.alloc = &pool_alloc;
  s.alloc_ctx = &pool;
  g.produce_limit = 12345;
  CHECK(job.begin(r.plugin, s) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK(job.result_frames() == 12345u);
  PRig r2;
  r2.info.flags = module::kPtfxFlagGenerator;
  PtfxJob job2;
  g.produce_limit = 0;
  CHECK(job2.begin(r2.plugin, s) == nullptr);
  CHECK(run(job2) == JobStatus::Failed);
  CHECK(job2.result_frames() == 0u);
}

TEST_CASE("ptfx_job: a generator over a sample ignores its audio and its selection, and the pool refusing the room fails the run") {
  PRig r;
  r.info.flags = module::kPtfxFlagGenerator;
  Pool pool(100000);
  const auto pcm = ramp(3000);
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool, 100, 200)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK(job.result_frames() == app::kPtfxGeneratorFrames);
  CHECK(job.result()[0] == 7000);  // not the source's doubled ramp
  PRig r2;
  r2.info.flags = module::kPtfxFlagGenerator;
  Pool small(50000);
  PtfxJob job2;
  CHECK(job2.begin(r2.plugin, source(pcm, small)) == nullptr);
  CHECK(run(job2) == JobStatus::Failed);
  CHECK(std::strcmp(job2.detail(), "NO ROOM FOR THE EDIT") == 0);
}

TEST_CASE("ptfx_job: set_extra_samples lengthens the result: the prepend and the append are the job's, read by the commit for the points") {
  PRig r;
  Pool pool(40000);
  const auto pcm = ramp(1000);
  g.want_prepend = 300;
  g.want_append = 200;
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool, 100, 600)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK(job.result_frames() == 1500u);
  CHECK(job.prepend() == 300u);
  CHECK(job.append() == 200u);
  CHECK(job.body_a() == 100u);
  CHECK(job.body_b() == 600u);
  CHECK(!job.generator());
}

TEST_CASE("ptfx_job: a Timeout, Fault or StackOverflow quarantines the plugin, ends the run and leaves the bytes") {
  struct Case {
    GateResult gate;
    module::PtfxQuarantine::Why why;
    const char* text;
  };
  const Case cases[] = {
      {GateResult::Timeout, module::PtfxQuarantine::Why::Timeout, "PLUGIN TIMED OUT: QUARANTINED"},
      {GateResult::Fault, module::PtfxQuarantine::Why::Fault, "PLUGIN CRASHED: SAVE, THEN RESTART"},
      {GateResult::StackOverflow, module::PtfxQuarantine::Why::StackOverflow, "PLUGIN STACK OVERFLOW: SAVE, THEN RESTART"},
  };
  const int at[] = {0, 5, 14};  // init, a set_param, the second process_block
  for (const Case& c : cases) {
    for (int when : at) {
      PRig r;
      Pool pool(30000);
      const auto pcm = ramp(20000);
      const auto before = pcm;
      g.fail_at = when;
      g.fail_with = c.gate;
      PtfxJob job;
      CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
      CHECK(run(job) == JobStatus::Failed);
      CHECK(std::strcmp(job.detail(), c.text) == 0);
      CHECK(r.quarantine.contains(r.plugin.id));
      CHECK(r.quarantine.why_of(r.plugin.id) == c.why);
      CHECK(r.cat.entry(0).quarantined);
      CHECK_FALSE(r.host.armed());
      CHECK(pcm == before);
      CHECK(g.calls == when + 1);  // nothing is called after it
      CHECK(job.step(1000) == JobStatus::Failed);
      CHECK(g.calls == when + 1);
      CHECK(std::strcmp(job.begin(r.plugin, source(pcm, pool)), "PLUGIN QUARANTINED") == 0);
    }
  }
}

TEST_CASE("ptfx_job: a plugin that ran into the stack guard and came back is a StackOverflow") {
  PRig r;
  g.smash_guard = true;
  Pool pool(30000);
  const auto pcm = ramp(1000);
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
  CHECK(run(job) == JobStatus::Failed);
  CHECK(std::strcmp(job.detail(), "PLUGIN STACK OVERFLOW: SAVE, THEN RESTART") == 0);
  CHECK(r.quarantine.why_of(r.plugin.id) == module::PtfxQuarantine::Why::StackOverflow);
}

TEST_CASE("ptfx_job: a gate that could not run the call fails the run without a quarantine") {
  PRig r;
  g.fail_at = 0;
  g.fail_with = GateResult::Unavailable;
  Pool pool(30000);
  const auto pcm = ramp(1000);
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
  CHECK(run(job) == JobStatus::Failed);
  CHECK(std::strcmp(job.detail(), "PLUGIN COULD NOT RUN") == 0);
  CHECK_FALSE(r.quarantine.contains(r.plugin.id));
}

TEST_CASE("ptfx_job: a Why::Crash id counts only when a listed plugin has it; the other reasons always count") {
  PRig r;
  Pool pool(30000);
  const auto pcm = ramp(1000);
  PtfxJob job;
  // A crash record naming something the catalogue does not list (a .tmod's id): inert.
  r.plugin.id = 0x8123;
  (void)r.quarantine.quarantine(0x8123, module::PtfxQuarantine::Why::Crash);
  CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
  // The listed plugin's id as a crash: refused.
  r.plugin.id = module::ptfx_plugin_id("gain.ptfx");
  (void)r.quarantine.quarantine(r.plugin.id, module::PtfxQuarantine::Why::Crash);
  CHECK(std::strcmp(job.begin(r.plugin, source(pcm, pool)), "PLUGIN QUARANTINED") == 0);
  // Without a catalogue every id is honoured.
  r.plugin.id = 0x8123;
  r.plugin.catalog = nullptr;
  CHECK(std::strcmp(job.begin(r.plugin, source(pcm, pool)), "PLUGIN QUARANTINED") == 0);
  // A timeout is the gate's own word and counts without the catalogue's say.
  r.plugin.id = 0x8456;
  (void)r.quarantine.quarantine(0x8456, module::PtfxQuarantine::Why::Timeout);
  r.plugin.catalog = &r.cat;
  CHECK(std::strcmp(job.begin(r.plugin, source(pcm, pool)), "PLUGIN QUARANTINED") == 0);
}

TEST_CASE("ptfx_job: a cancelled run leaves the plugin alone and commits nothing") {
  PRig r;
  Pool pool(30000);
  const auto pcm = ramp(1000);
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
  for (int i = 0; i < 4; ++i) {
    CHECK(job.step(1000) == JobStatus::Running);
  }
  job.cancel();
  CHECK_FALSE(job.done());
  CHECK(job.step(1000) == JobStatus::Failed);
  CHECK_FALSE(r.host.armed());
  CHECK_FALSE(r.quarantine.contains(r.plugin.id));
}

TEST_CASE("ptfx_job: a scratch smaller than the plugin asked for is refused, init never runs on less") {
  PRig r;
  Pool pool(5000);
  const auto pcm = ramp(1000);
  PtfxJob job;
  r.plugin.scratch_cap = r.info.scratch_size - 1u;
  CHECK(std::strcmp(job.begin(r.plugin, source(pcm, pool)), "PLUGIN NOT READY") == 0);
  CHECK(std::strcmp(job.not_ready_why(), "scratch") == 0);
  CHECK(job.step(1000) == JobStatus::Failed);  // never started
  CHECK(g.calls == 0);
  r.plugin.scratch_cap = r.info.scratch_size;  // exactly enough is fine
  CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
  r.plugin.scratch_cap = 0xFFFFFFFFu;
  r.info.scratch_size = module::kPtfxMaxScratch + 1u;  // more than the region can ever hold
  CHECK(std::strcmp(job.begin(r.plugin, source(pcm, pool)), "PLUGIN NOT READY") == 0);
}

TEST_CASE("ptfx_job: a cancel after Size leaves the pool buffer unclaimed by a commit and the plugin alone") {
  PRig r;
  Pool pool(30000);
  const auto pcm = ramp(1000);
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
  for (unsigned n = 0; n < 1000u && pool.calls == 0u; ++n) {  // up to the Size step that took the buffer
    CHECK(job.step(1000) == JobStatus::Running);
  }
  CHECK(pool.calls == 1u);
  CHECK(proc_blocks().empty());
  job.cancel();
  CHECK_FALSE(job.done());
  CHECK(job.step(1000) == JobStatus::Failed);
  CHECK(proc_blocks().empty());  // no process_block after the cancel
  CHECK_FALSE(r.host.armed());
  CHECK_FALSE(r.quarantine.contains(r.plugin.id));
}

TEST_CASE("ptfx_job: a cancel in the middle of Process stops at once, no further plugin call, nothing quarantined") {
  PRig r;
  Pool pool(30000);
  const auto pcm = ramp(20000);
  PtfxJob job;
  CHECK(job.begin(r.plugin, source(pcm, pool)) == nullptr);
  for (unsigned n = 0; n < 1000u && proc_blocks().size() < 2u; ++n) {
    CHECK(job.step(1000) == JobStatus::Running);
  }
  CHECK(proc_blocks().size() == 2u);  // 20000 frames are 3 blocks: one is still to come
  const int calls = g.calls;
  job.cancel();
  CHECK_FALSE(job.done());
  CHECK(job.step(1000) == JobStatus::Failed);
  CHECK(g.calls == calls);
  CHECK_FALSE(r.host.armed());
  CHECK_FALSE(r.quarantine.contains(r.plugin.id));
}

namespace {

struct BankRig {
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;

  MS ms;  // stereo: the 1,000 frames the slot was loaded with

  // Mono: 1,000 frames of 3. Stereo (S5): 1,000 frames of make_ms with S, and the instrument's STEREO row on, so that the editor treats the slot as stereo.
  explicit BankRig(bool stereo = false, std::uint32_t pool = 40000) : memory(pool + 8) {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), pool);
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    in.stereo = stereo ? 1u : 0u;
    CHECK(arena->project.set_instrument(0, in) == model::Status::Ok);
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED(p != nullptr);
    if (stereo) {
      ms = make_ms(1000);
      std::memcpy(p, ms.buf.data(), ms.buf.size() * sizeof(std::int16_t));
      CHECK(bank.commit_load(0, p, 1000, p + even_of(1000)));
    } else {
      for (std::uint32_t i = 0; i < 1000u; ++i) {
        p[i] = 3;
      }
      CHECK(bank.commit_load(0, p, 1000));
    }
    bank.rebind_now(0);
  }
};

std::int16_t* bank_alloc(void* ctx, std::uint32_t frames, bool stereo) noexcept { return static_cast<app::SampleBank*>(ctx)->edit_begin(0, frames, stereo); }

}  // namespace

TEST_CASE("ptfx_job: one run is one undo step and undo gives the original bytes back") {
  PRig r;
  BankRig b;
  g.want_prepend = 10;
  g.want_append = 20;
  const std::int16_t* original = b.bank.pcm(0).data;
  const std::vector<std::int16_t> bytes(original, original + 1000);
  PtfxJob::Source s;
  s.pcm = original;
  s.frames = 1000;
  s.alloc = &bank_alloc;
  s.alloc_ctx = &b.bank;
  PtfxJob job;
  CHECK(job.begin(r.plugin, s) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK(job.result_frames() == 1030u);
  CHECK(b.bank.undo_depth(0) == 0u);  // nothing yet: the job only wrote a free pool buffer
  app::PointsRec before;
  app::PointsRec after;
  CHECK(b.bank.edit_commit(0, job.result(), job.result_frames(), before, after));
  CHECK(b.bank.undo_depth(0) == 1u);
  CHECK(b.bank.pcm(0).frames == 1030u);
  CHECK(b.bank.pcm(0).data[0] == 7000);
  CHECK(b.bank.pcm(0).data[10] == 6);
  CHECK(b.bank.pcm(0).data[1029] == 7000);
  app::PointsRec restore;
  CHECK(b.bank.edit_undo(0, restore));
  CHECK(b.bank.undo_depth(0) == 0u);
  CHECK(b.bank.pcm(0).data == original);
  CHECK(b.bank.pcm(0).frames == 1000u);
  CHECK(std::memcmp(b.bank.pcm(0).data, bytes.data(), bytes.size() * sizeof(std::int16_t)) == 0);
  CHECK_FALSE(b.bank.edit_undo(0, restore));  // one step, not two
}

TEST_CASE("ptfx_job: a failed run on the bank leaves the slot and its history as they were") {
  PRig r;
  BankRig b;
  const std::int16_t* original = b.bank.pcm(0).data;
  g.fail_at = 13;  // the only process_block of 1000 frames
  g.fail_with = GateResult::Fault;
  PtfxJob::Source s;
  s.pcm = original;
  s.frames = 1000;
  s.alloc = &bank_alloc;
  s.alloc_ctx = &b.bank;
  PtfxJob job;
  CHECK(job.begin(r.plugin, s) == nullptr);
  CHECK(run(job) == JobStatus::Failed);
  CHECK(b.bank.pcm(0).data == original);
  CHECK(b.bank.pcm(0).frames == 1000u);
  CHECK(b.bank.undo_depth(0) == 0u);
  CHECK(original[0] == 3);
  CHECK(original[999] == 3);
}

namespace {
PtfxJob::Source bank_source(BankRig& b) {
  const app::SampleBank::Pcm pcm = b.bank.pcm(0);
  PtfxJob::Source s;
  s.pcm = pcm.data;
  s.frames = pcm.frames;
  s.side = pcm.side;
  s.stereo = b.bank.edit_stereo(0);
  s.alloc = &bank_alloc;
  s.alloc_ctx = &b.bank;
  return s;
}

bool bank_is(BankRig& b, const MS& want) {
  const app::SampleBank::Pcm pcm = b.bank.pcm(0);
  if (pcm.frames != want.n || pcm.side == nullptr) {
    return false;
  }
  return std::memcmp(pcm.data, want.m(), want.n * sizeof(std::int16_t)) == 0 && std::memcmp(pcm.side, want.s(), want.n * sizeof(std::int16_t)) == 0;
}
}  // namespace

TEST_CASE("ptfx_job: a stereo slot through a no-op plugin is committed with S as one undo step and comes back bit for bit, then undo restores it") {
  const std::uint16_t modes[2] = {module::kPtfxFlagStereo, 0};
  for (const std::uint16_t flags : modes) {
    PRig r;
    r.info.flags = flags;
    g.gain[0] = g.gain[1] = 1;
    BankRig b(true);
    NEED(b.bank.edit_stereo(0));
    const MS original = b.ms;
    PtfxJob job;
    CHECK(job.begin(r.plugin, bank_source(b)) == nullptr);
    CHECK(run(job) == JobStatus::Done);
    CHECK(job.stereo_result());
    CHECK(job.result_frames() == 1000u);
    app::PointsRec before;
    app::PointsRec after;
    CHECK(b.bank.edit_commit(0, job.result(), job.result_frames(), before, after, job.result() + even_of(job.result_frames())));
    CHECK(b.bank.undo_depth(0) == 1u);
    CHECK(b.bank.edit_stereo(0));
    CHECK(bank_is(b, original));  // what the importer made, bit for bit
    app::PointsRec restore;
    CHECK(b.bank.edit_undo(0, restore));
    CHECK(b.bank.undo_depth(0) == 0u);
    CHECK(bank_is(b, original));
  }
}

TEST_CASE("ptfx_job: a CONVERT_TO_MONO plugin on a stereo slot commits mono (L), and undo brings the stereo back") {
  PRig r;
  r.info.flags = module::kPtfxFlagConvertToMono;
  g.gain[0] = g.gain[1] = 1;
  BankRig b(true);
  NEED(b.bank.edit_stereo(0));
  const MS original = b.ms;
  PtfxJob job;
  CHECK(job.begin(r.plugin, bank_source(b)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK_FALSE(job.stereo_result());
  app::PointsRec before;
  app::PointsRec after;
  CHECK(b.bank.edit_commit(0, job.result(), job.result_frames(), before, after));  // no side: a mono result
  CHECK(b.bank.undo_depth(0) == 1u);
  CHECK(b.bank.pcm(0).side == nullptr);
  CHECK(b.bank.pcm(0).frames == 1000u);
  bool is_l = true;
  for (std::uint32_t i = 0; i < 1000u; ++i) {
    is_l = is_l && b.bank.pcm(0).data[i] == sat16i(original.m()[i] + original.s()[i]);
  }
  CHECK(is_l);
  app::PointsRec restore;
  CHECK(b.bank.edit_undo(0, restore));
  CHECK(bank_is(b, original));
}

TEST_CASE("ptfx_job: a generator on a stereo slot replaces it with a mono sample, undo brings the stereo back (A12)") {
  PRig r;
  r.info.flags = module::kPtfxFlagGenerator;
  BankRig b(true, 200000);  // the generator's 88,200 frames need room beside the stereo slot
  NEED(b.bank.edit_stereo(0));
  const MS original = b.ms;
  PtfxJob job;
  CHECK(job.begin(r.plugin, bank_source(b)) == nullptr);
  CHECK(run(job) == JobStatus::Done);
  CHECK_FALSE(job.stereo_result());
  app::PointsRec before;
  app::PointsRec after;
  CHECK(b.bank.edit_commit(0, job.result(), job.result_frames(), before, after));
  CHECK(b.bank.pcm(0).side == nullptr);
  CHECK(b.bank.pcm(0).frames == job.result_frames());
  app::PointsRec restore;
  CHECK(b.bank.edit_undo(0, restore));
  CHECK(bank_is(b, original));
}

TEST_CASE("ptfx_job: a two-pass run that fails on the length leaves the stereo slot and its history as they were") {
  PRig r;
  g.limit_second = 400;
  BankRig b(true);
  NEED(b.bank.edit_stereo(0));
  const MS original = b.ms;
  PtfxJob job;
  CHECK(job.begin(r.plugin, bank_source(b)) == nullptr);
  CHECK(run(job) == JobStatus::Failed);
  CHECK(std::strcmp(job.detail(), "STEREO: CHANNELS DIFFER") == 0);
  CHECK(b.bank.undo_depth(0) == 0u);
  CHECK(bank_is(b, original));
}

namespace {

// A job that spends `spend_us` of the clock a step whatever its budget; `is_long` is its long_step().
struct FakeClock {
  mutable core::Tick t = 0;
  core::Tick now() const noexcept { return t; }
};
class Hog final : public core::Job {
 public:
  Hog(FakeClock& c, bool is_long) : clock_(c), long_(is_long) {}
  JobStatus step(std::uint32_t) noexcept override {
    clock_.t += 100000;
    return JobStatus::Running;
  }
  unsigned progress() const noexcept override { return 0; }
  void cancel() noexcept override {}
  bool long_step() const noexcept override { return long_; }
  const char* name() const noexcept override { return "HOG"; }
  const char* detail() const noexcept override { return ""; }

 private:
  FakeClock& clock_;
  bool long_;
};

}  // namespace

TEST_CASE("ptfx_job: long_step() keeps a plugin call out of the overrun count, and a job without it is counted as before") {
  {
    FakeClock clock;
    core::JobRunner runner;
    Hog plain(clock, false);
    CHECK_FALSE(plain.long_step());  // the default
    runner.start(plain);
    runner.run(clock, 2000);
    CHECK(runner.stats(0).overruns == 1u);
    CHECK(runner.stats(0).max_step_us == 100000u);
  }
  {
    FakeClock clock;
    core::JobRunner runner;
    Hog lng(clock, true);
    runner.start(lng);
    runner.run(clock, 2000);
    CHECK(runner.stats(0).overruns == 0u);
    CHECK(runner.stats(0).max_step_us == 100000u);  // the time is still measured
  }
}
