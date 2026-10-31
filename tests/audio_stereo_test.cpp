// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-STEREO step 2, slice (c): the stereo voice and the mix (voices.h STEREO VOICES, graph.h feed_side, kernels.h gain_pan_st). Host and Cortex-M4F (`qemu_audio_stereo`: the loop's inline asm only exists there).
//   * the side kernel against its scalar reference;
//   * a stereo voice's M is the mono voice's block bit for bit (S = 0 included, where S is silence), and its S block is what a MONO voice makes of the S wave itself (same position, amplitude, envelope, residual,
//     declick): so the residual rules of a stop, a retrigger, a steal and a one shot's end are those of M, sample for sample, over a scripted run;
//   * the cap of four stereo voices; an effect that turns on mid-note drops S with a block fade and keeps it dropped;
//   * the graph: L = M + S and R = M - S through the strip's level and pan law against the mono graph's own output, a muted or closed strip and a strip with an insert in the path take no S.
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/kernels.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/need.h"
#include "support/rng.h"
#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

namespace ha = o3os::hal::audio;
using namespace o3os::audio;
using o3os::test::Rng;
namespace gps = o3os::audio::graph_param::strip;

constexpr std::uint32_t kB = ha::kBlock;
constexpr std::uint32_t kLen = 3000;  // even: S is at kLen in the test's block, as the bank's even(frames)

alignas(8) std::int16_t g_buf[2 * kLen];       // M, then S
alignas(8) std::int16_t g_buf_zero[2 * kLen];  // M, then S = 0

void init_waves() {
  for (std::uint32_t i = 0; i < kLen; ++i) {
    const double t = static_cast<double>(i);
    g_buf[i] = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(6.283185307 * 7.0 * t / kLen) + 5000.0 * std::sin(0.37 * t)));
    g_buf[kLen + i] = static_cast<std::int16_t>(std::lround(9000.0 * std::sin(6.283185307 * 11.0 * t / kLen + 1.0) + 3000.0 * std::sin(0.11 * t) - 4000.0));
    g_buf_zero[i] = g_buf[i];
    g_buf_zero[kLen + i] = 0;
  }
}

// Which pool plays what: the stereo pool (M with S at st_off), the mono pool (the same M alone), the reference pool (the S wave as a mono sample).
enum class Kind { Stereo, Mono, SideAsMono };

void make_descs(VoicePool& p, Kind kind, const std::int16_t* buf, std::uint32_t st_off = kLen) {
  for (unsigned slot = 0; slot < 3; ++slot) {
    SampleDesc d{kind == Kind::SideAsMono ? buf + st_off : buf, kLen, 0, 0, 65536};
    if (slot == 1u) {
      d.loop_start = 200;
      d.loop_end = 2900;
    } else if (slot == 2u) {
      d.env.enable = true;
      d.env.attack = 20;
      d.env.hold = 1;
      d.env.decay = 60;
      d.env.sustain = kEnvOne / 2u;
      d.env.release = 40;
    }
    if (kind == Kind::Stereo) {
      d.st_off = st_off;
    }
    p.set_sample(slot, d);
  }
}

struct Act {
  unsigned block;
  unsigned at;  // the sample the event lands on in the block
  char kind;    // 'n' note on, 'o' note off, 'c' cut
  unsigned track;
  unsigned note;
  unsigned slot;
};

constexpr unsigned kTr = 16;

struct Trace {
  std::vector<std::int16_t> m;     // [block][track][kB], zero for a track with no feed
  std::vector<std::int16_t> s;     // the same for S
  std::vector<std::uint16_t> has;  // [block]: the tracks that had S
  std::vector<std::uint8_t> held;  // [block]: stereo_voices() at the block's end
};

void run(VoicePool& p, const std::vector<Act>& script, unsigned blocks, Trace& out) {
  out.m.assign(static_cast<std::size_t>(blocks) * kTr * kB, 0);
  out.s.assign(static_cast<std::size_t>(blocks) * kTr * kB, 0);
  out.has.assign(blocks, 0);
  out.held.assign(blocks, 0);
  for (unsigned b = 0; b < blocks; ++b) {
    p.set_gate(0);
    p.begin_block();
    std::uint32_t cursor = 0;
    for (unsigned at = 0; at < kB; ++at) {
      bool any = false;
      for (const Act& a : script) {
        if (a.block == b && a.at == at) {
          any = true;
        }
      }
      if (!any) {
        continue;
      }
      if (at > cursor) {
        p.render(cursor, at);
        cursor = at;
      }
      for (const Act& a : script) {
        if (a.block != b || a.at != at) {
          continue;
        }
        if (a.kind == 'n') {
          p.note_on(a.track, a.note, a.slot, kVelocityDefault);
        } else if (a.kind == 'o') {
          p.note_off(a.track);
        } else {
          p.cut(a.track);
        }
      }
    }
    p.render(cursor, kB);
    p.end_block();
    for (unsigned t = 0; t < kTr; ++t) {
      const std::int16_t* m = p.feed(t);
      const std::int16_t* s = p.feed_side(t);
      if (m != nullptr) {
        std::memcpy(&out.m[(static_cast<std::size_t>(b) * kTr + t) * kB], m, kB * sizeof(std::int16_t));
      }
      if (s != nullptr) {
        std::memcpy(&out.s[(static_cast<std::size_t>(b) * kTr + t) * kB], s, kB * sizeof(std::int16_t));
        out.has[b] = static_cast<std::uint16_t>(out.has[b] | (1u << t));
      }
    }
    out.held[b] = static_cast<std::uint8_t>(p.stereo_voices());
  }
}

unsigned popcount(std::uint16_t v) {
  unsigned n = 0;
  for (; v != 0u; v = static_cast<std::uint16_t>(v & (v - 1u))) {
    ++n;
  }
  return n;
}

// The long script: one shots that end, loops, an enveloped note and its release, retriggers (same track), a cut and a note after it, mid-block events, more tracks than the cap and than the voices (steals).
std::vector<Act> long_script() {
  std::vector<Act> s = {
      {0, 0, 'n', 0, 72, 0}, {1, 0, 'n', 1, 60, 1}, {3, 20, 'n', 2, 64, 2}, {6, 0, 'n', 1, 67, 1},  {8, 0, 'c', 1, 0, 0},   {9, 0, 'n', 1, 60, 0},
      {12, 0, 'o', 2, 0, 0}, {14, 30, 'n', 0, 60, 1}, {18, 0, 'n', 3, 48, 0}, {19, 5, 'n', 4, 52, 1}, {20, 0, 'n', 5, 55, 1}, {25, 0, 'o', 0, 0, 0},
      {26, 0, 'c', 3, 0, 0}, {27, 9, 'n', 3, 61, 2},
  };
  for (unsigned t = 6; t < 16; ++t) {
    s.push_back({30u + (t & 1u), 0, 'n', t, 40 + t, 1});  // the voices run out (12): the oldest is stolen
  }
  s.push_back({36, 0, 'c', 7, 0, 0});
  s.push_back({37, 0, 'n', 7, 59, 0});
  s.push_back({40, 0, 'o', 3, 0, 0});
  return s;
}

o3os::test::StaticSlot<VoicePool> slot_a;
o3os::test::StaticSlot<VoicePool> slot_b;
o3os::test::StaticSlot<VoicePool> slot_c;

// M of the stereo pool against the mono pool, S against the reference pool, block by block and track by track.
void compare(const Trace& st, const Trace& mono, const Trace* side_ref, unsigned blocks, unsigned& side_seen, unsigned& side_max) {
  side_seen = 0;
  side_max = 0;
  for (unsigned b = 0; b < blocks; ++b) {
    side_max = popcount(st.has[b]) > side_max ? popcount(st.has[b]) : side_max;
    CHECK(st.held[b] <= 4u);
    CHECK(popcount(st.has[b]) <= 4u);
    for (unsigned t = 0; t < kTr; ++t) {
      const std::size_t o = (static_cast<std::size_t>(b) * kTr + t) * kB;
      INFO("block " << b << " track " << t);
      CHECK(std::memcmp(&st.m[o], &mono.m[o], kB * sizeof(std::int16_t)) == 0);
      if (((st.has[b] >> t) & 1u) != 0u) {
        ++side_seen;
        if (side_ref != nullptr) {
          CHECK(std::memcmp(&st.s[o], &side_ref->m[o], kB * sizeof(std::int16_t)) == 0);
        } else {
          for (std::uint32_t i = 0; i < kB; ++i) {
            CHECK(st.s[o + i] == 0);
          }
        }
      }
    }
  }
}

// ---- the kernel ---------------------------------------------------------------------------------------------------------------

TEST_CASE("stereo: gain_pan_st equals its scalar reference (steady and ramped gains, the right side subtracted)") {
  Rng rng(7);
  alignas(8) std::int16_t s[kB];
  alignas(8) std::int32_t al[kB];
  alignas(8) std::int32_t ar[kB];
  alignas(8) std::int32_t bl[kB];
  alignas(8) std::int32_t br[kB];
  for (unsigned round = 0; round < 200; ++round) {
    for (std::uint32_t i = 0; i < kB; ++i) {
      s[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(65536u)) - 32768);
      al[i] = bl[i] = static_cast<std::int32_t>(rng.below(1u << 22)) - (1 << 21);
      ar[i] = br[i] = static_cast<std::int32_t>(rng.below(1u << 22)) - (1 << 21);
    }
    const auto gain = [&rng, round]() { return static_cast<std::int32_t>(round % 7u == 0u ? 0u : rng.below(65536u * 3u)); };
    const kernel::GainRamp lv{gain(), round % 3u == 0u ? 0 : gain()};
    const kernel::GainRamp l{gain(), gain()};
    const kernel::GainRamp r{round % 5u == 0u ? 0 : gain(), gain()};
    const kernel::GainRamp steady{lv.start, lv.start};
    const kernel::GainRamp lv2 = round % 2u == 0u ? lv : steady;
    kernel::gain_pan_st(s, lv2, l, r, al, ar, kB);
    kernel::gain_pan_st_ref(s, lv2, l, r, bl, br, kB);
    CHECK(std::memcmp(al, bl, sizeof al) == 0);
    CHECK(std::memcmp(ar, br, sizeof ar) == 0);
  }
  // (f): the steady loop (all three gains constant: the common block, its own LDRD / STRD loop, taken when the right accumulator is the left one's kB words on, as MAIN's two rows are) against the reference,
  // with a row layout (the steady loop), the separate arrays above (the paired loop of the ramps), one ramp in every fourth round, and a count that ends in the scalar tail.
  alignas(8) std::int32_t rows[2][kB];
  for (unsigned round = 0; round < 300; ++round) {
    for (std::uint32_t i = 0; i < kB; ++i) {
      s[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(65536u)) - 32768);
      rows[0][i] = bl[i] = static_cast<std::int32_t>(rng.below(1u << 22)) - (1 << 21);
      rows[1][i] = br[i] = static_cast<std::int32_t>(rng.below(1u << 22)) - (1 << 21);
      al[i] = bl[i];
      ar[i] = br[i];
    }
    const auto gain = [&rng, round]() { return static_cast<std::int32_t>(round % 7u == 0u ? 0u : rng.below(65536u * 3u)); };
    const std::int32_t gv = gain();
    const std::int32_t gl = gain();
    const std::int32_t gr = gain();
    const kernel::GainRamp lv{gv, gv};
    const kernel::GainRamp l{gl, gl};
    const kernel::GainRamp r{gr, round % 4u == 3u ? gr + 1 : gr};
    const std::uint32_t n = round % 5u == 4u ? kB - 1u : kB;  // (an odd count ends in the scalar tail)
    kernel::gain_pan_st(s, lv, l, r, rows[0], rows[1], n);
    kernel::gain_pan_st(s, lv, l, r, al, ar, n);
    kernel::gain_pan_st_ref(s, lv, l, r, bl, br, n);
    CHECK(std::memcmp(rows[0], bl, sizeof bl) == 0);
    CHECK(std::memcmp(rows[1], br, sizeof br) == 0);
    CHECK(std::memcmp(al, bl, sizeof al) == 0);
    CHECK(std::memcmp(ar, br, sizeof ar) == 0);
  }
  // S = 0 adds nothing
  for (std::uint32_t i = 0; i < kB; ++i) {
    s[i] = 0;
    al[i] = ar[i] = 123;
  }
  kernel::gain_pan_st(s, kernel::GainRamp{65536, 65536}, kernel::GainRamp{65536, 65536}, kernel::GainRamp{65536, 65536}, al, ar, kB);
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(al[i] == 123);
    CHECK(ar[i] == 123);
  }
}

// (f): the stereo loop's product, floor((s * amp + 2^23) / 2^24), as the SMMULR form (s * 32) x (amp * 8) + 2^31, high word: the identity (voices.cpp mul_q24) over the whole range of s and the extremes of amp.
TEST_CASE("stereo: the loop's SMMULR product is the 64-bit product it replaces (all s at the amplitude extremes, random pairs)") {
  const auto plain = [](std::int32_t s, std::int32_t amp) { return static_cast<std::int32_t>((static_cast<std::int64_t>(s) * amp + 0x800000) >> 24); };
  const auto smmulr = [](std::int32_t s, std::int32_t amp) { return static_cast<std::int32_t>((static_cast<std::int64_t>(s * 32) * (amp * 8) + 0x80000000LL) >> 32); };
  const std::int32_t amps[] = {0, 1, 255, 256, 0x7FFFFF, 0x800000, 0x1000000, 0x0FFFFFFF - 255, 0x0FFFFFFF};
  unsigned wrong = 0;
  for (const std::int32_t amp : amps) {
    for (std::int32_t s = -32768; s <= 32767; ++s) {
      wrong += smmulr(s, amp) == plain(s, amp) ? 0u : 1u;
    }
  }
  Rng rng(11);
  for (unsigned i = 0; i < 200000; ++i) {
    const std::int32_t s = static_cast<std::int32_t>(rng.below(65536u)) - 32768;
    const std::int32_t amp = static_cast<std::int32_t>(rng.below(1u << 28));
    wrong += smmulr(s, amp) == plain(s, amp) ? 0u : 1u;
  }
  CHECK(wrong == 0u);
}

// ---- the voices ----------------------------------------------------------------------------------------------------------------

TEST_CASE("stereo: M is the mono voice's block bit for bit and S is the mono render of the S wave (stops, retriggers, steals, one shot ends, envelope)") {
  init_waves();
  VoicePool& st = slot_a.make();
  VoicePool& mono = slot_b.make();
  VoicePool& ref = slot_c.make();
  make_descs(st, Kind::Stereo, g_buf);
  make_descs(mono, Kind::Mono, g_buf);
  make_descs(ref, Kind::SideAsMono, g_buf);
  const std::vector<Act> script = long_script();
  const unsigned blocks = 48;
  Trace ts, tm, tr;
  run(st, script, blocks, ts);
  run(mono, script, blocks, tm);
  run(ref, script, blocks, tr);
  unsigned seen = 0;
  unsigned most = 0;
  compare(ts, tm, &tr, blocks, seen, most);
  CHECK(seen > 60u);   // the run is mostly stereo, so the comparisons above were made on S
  CHECK(most == 4u);   // and the cap was reached, and held
  CHECK(st.stereo_voices() <= 4u);
}

// The steady stereo loop at every halfword phase of the arena, M and S, and an S at an odd distance from M.
TEST_CASE("stereo: M and S are exact at every halfword offset of the data and with an odd S distance") {
  init_waves();
  alignas(8) static std::int16_t shifted[2 * kLen + 8];
  const std::vector<Act> script = long_script();
  const unsigned blocks = 48;
  for (unsigned cfg = 0; cfg < 5; ++cfg) {
    const unsigned off = cfg < 4u ? cfg : 1u;
    const std::uint32_t st_off = cfg < 4u ? kLen : kLen + 1u;  // cfg 4: S one halfword further on
    std::memset(shifted, 0, sizeof shifted);
    for (std::uint32_t i = 0; i < kLen; ++i) {
      shifted[off + i] = g_buf[i];
      shifted[off + st_off + i] = g_buf[kLen + i];
    }
    VoicePool& st = slot_a.make();
    VoicePool& mono = slot_b.make();
    VoicePool& ref = slot_c.make();
    make_descs(st, Kind::Stereo, shifted + off, st_off);
    make_descs(mono, Kind::Mono, shifted + off, st_off);
    make_descs(ref, Kind::SideAsMono, shifted + off, st_off);
    Trace ts, tm, tr;
    run(st, script, blocks, ts);
    run(mono, script, blocks, tm);
    run(ref, script, blocks, tr);
    unsigned seen = 0;
    unsigned most = 0;
    INFO("offset " << off << " S distance " << st_off);
    compare(ts, tm, &tr, blocks, seen, most);
    CHECK(seen > 60u);
  }
}

TEST_CASE("stereo: with S = 0 the stereo voice is the mono voice bit for bit and S is silent") {
  init_waves();
  VoicePool& st = slot_a.make();
  VoicePool& mono = slot_b.make();
  make_descs(st, Kind::Stereo, g_buf_zero);
  make_descs(mono, Kind::Mono, g_buf_zero);
  const std::vector<Act> script = long_script();
  const unsigned blocks = 48;
  Trace ts, tm;
  run(st, script, blocks, ts);
  run(mono, script, blocks, tm);
  unsigned seen = 0;
  unsigned most = 0;
  compare(ts, tm, nullptr, blocks, seen, most);
  CHECK(seen > 60u);
}

TEST_CASE("stereo: a note that cannot make S is mono (reversed, backward loop, wavetable, Amiga engine, filter, gate) and the cap is four") {
  init_waves();
  VoicePool& st = slot_a.make();
  make_descs(st, Kind::Stereo, g_buf);
  // five loops on five tracks: four have S
  for (unsigned t = 0; t < 5; ++t) {
    CHECK(st.note_on(t, 60, 1, kVelocityDefault));
  }
  st.set_gate(0);
  st.begin_block();
  st.render(0, kB);
  st.end_block();
  CHECK(popcount(st.side_mask()) == 4u);
  CHECK(st.stereo_voices() == 4u);
  CHECK(st.feed_side(4) == nullptr);
  CHECK(st.feed(4) != nullptr);
  // the filter on a track: its next note is M alone
  st.cut(0);
  st.cut(1);
  st.cut(2);
  st.cut(3);
  for (unsigned b = 0; b < 3; ++b) {  // the tails run out and give their blocks back
    st.begin_block();
    st.render(0, kB);
    st.end_block();
  }
  CHECK(st.stereo_voices() == 0u);
  st.set_filter_fx(0, 0, 80);
  CHECK(st.note_on(0, 60, 1, kVelocityDefault));
  st.begin_block();
  st.render(0, kB);
  st.end_block();
  CHECK(st.feed_side(0) == nullptr);
  // a descriptor that plays backward has no S (set_sample drops it)
  SampleDesc bwd{g_buf, kLen, 200, 2900, 65536};
  bwd.st_off = kLen;
  bwd.loop_mode = 1;
  st.set_sample(5, bwd);
  SampleDesc amiga{g_buf, kLen, 0, 0, 65536};
  amiga.st_off = kLen;
  amiga.engine = kEngineAmiga;
  st.set_sample(6, amiga);
  SampleDesc gran{g_buf, kLen, 0, 0, 65536};
  gran.st_off = kLen;
  gran.granular = true;
  st.set_sample(7, gran);
  CHECK(st.note_on(8, 60, 5, kVelocityDefault));
  CHECK(st.note_on(9, 60, 7, kVelocityDefault));
  st.begin_block();
  st.render(0, kB);
  st.end_block();
  CHECK(st.feed_side(8) == nullptr);
  CHECK(st.feed_side(9) == nullptr);
  // the gate drops S and keeps it dropped for the note
  st.cut(0);
  st.cut(8);
  st.cut(9);
  for (unsigned b = 0; b < 3; ++b) {
    st.begin_block();
    st.render(0, kB);
    st.end_block();
  }
  CHECK(st.note_on(10, 60, 1, kVelocityDefault));
  st.begin_block();
  st.render(0, kB);
  st.end_block();
  CHECK(st.feed_side(10) != nullptr);
  st.set_gate(1u << 10);
  st.begin_block();
  st.render(0, kB);
  st.end_block();
  CHECK(st.stereo_voices() == 0u);
  st.set_gate(0);
  for (unsigned b = 0; b < 4; ++b) {
    st.begin_block();
    st.render(0, kB);
    st.end_block();
    CHECK(st.feed_side(10) == nullptr);
  }
}

TEST_CASE("stereo: an effect that turns on mid-note drops S with a block fade, and S stays dropped") {
  init_waves();
  VoicePool& st = slot_a.make();
  make_descs(st, Kind::Stereo, g_buf);
  CHECK(st.note_on(0, 60, 1, kVelocityDefault));
  std::int16_t last = 0;
  for (unsigned b = 0; b < 6; ++b) {
    st.set_gate(0);
    st.begin_block();
    st.render(0, kB);
    st.end_block();
    NEED(st.feed_side(0) != nullptr);
    last = st.feed_side(0)[kB - 1];
  }
  CHECK(last != 0);
  st.set_filter_fx(0, 0, 90);  // the filter comes on under the sounding note
  st.begin_block();
  st.render(0, kB);
  st.end_block();
  NEED(st.feed_side(0) != nullptr);
  const std::int16_t* f = st.feed_side(0);
  const std::int32_t dec = (std::abs(static_cast<std::int32_t>(last)) + 63) / 64;
  std::int32_t want = last;
  for (std::uint32_t i = 0; i < kB; ++i) {  // S's last value, walking to zero over one block (kDeclickSamples): no step, then nothing
    CHECK(f[i] == want);
    want = want > 0 ? (want > dec ? want - dec : 0) : (want < -dec ? want + dec : 0);
  }
  CHECK(want == 0);
  CHECK(std::abs(static_cast<std::int32_t>(f[kB - 1])) <= dec);  // the last sample of the fade is one step from zero; the residual is zero after it
  for (unsigned b = 0; b < 3; ++b) {  // the block is free again, and the note goes on as M alone
    st.begin_block();
    st.render(0, kB);
    st.end_block();
    CHECK(st.feed_side(0) == nullptr);
    CHECK(st.feed(0) != nullptr);
    CHECK(st.track_playing(0));
  }
  CHECK(st.stereo_voices() == 0u);
}

// Review fold: a side block written in a block and dropped in the same block (a non-stereo retrigger over a stereo note whose S is zero at that moment) must be zeroed at its next first use, though
// end_block skipped end_block_st (nothing stereo was held at the block's end): the "touched" bit goes with the drop.
TEST_CASE("stereo: a side block dropped in the block it was written in is zeroed at its next use") {
  init_waves();
  alignas(8) static std::int16_t spike[2 * kLen];  // M as g_buf's, S = 10000 for twelve samples and zero after them: a note's S is nonzero at its start and zero by sample 40
  for (std::uint32_t i = 0; i < kLen; ++i) {
    spike[i] = g_buf[i];
    spike[kLen + i] = i < 12u ? std::int16_t{10000} : std::int16_t{0};
  }
  VoicePool& st = slot_a.make();
  make_descs(st, Kind::Stereo, g_buf);
  SampleDesc a{spike, kLen, 0, 0, 65536};
  a.st_off = kLen;
  st.set_sample(3, a);                       // stereo, S spiked
  st.set_sample(4, SampleDesc{g_buf, kLen, 0, 0, 65536});  // mono (st_off 0)
  // Block 1: the stereo note starts at 8 and writes S; at 40 a mono note retriggers the track (S's last is 0 and its residual is 0: the block is dropped); nothing stereo is held at the block's end.
  st.set_gate(0);
  st.begin_block();
  st.render(0, 8);
  CHECK(st.note_on(0, 60, 3, kVelocityDefault));
  st.render(8, 40);
  CHECK(st.note_on(0, 60, 4, kVelocityDefault));
  st.render(40, kB);
  st.end_block();
  CHECK(st.stereo_voices() == 0u);
  CHECK(st.feed_side(0) == nullptr);
  // Block 2: a stereo note retriggers at 32; S before it is silence, whatever block 1 left in the memory.
  st.set_gate(0);
  st.begin_block();
  st.render(0, 32);
  CHECK(st.note_on(0, 60, 0, kVelocityDefault));
  st.render(32, kB);
  st.end_block();
  NEED(st.feed_side(0) != nullptr);
  const std::int16_t* f = st.feed_side(0);
  for (std::uint32_t i = 0; i < 32u; ++i) {
    INFO("sample " << i);
    CHECK(f[i] == 0);
  }
  bool any = false;
  for (std::uint32_t i = 32; i < kB; ++i) {
    any = any || f[i] != 0;
  }
  CHECK(any);
}

// ---- the graph ----------------------------------------------------------------------------------------------------------------

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

struct GraphRig {
  Graph& g;
  alignas(8) std::int16_t in[ha::kBlockSamples] = {};
  alignas(8) std::int16_t out[ha::kBlockSamples] = {};
  alignas(8) std::int16_t m[kB] = {};
  alignas(8) std::int16_t s[kB] = {};
  ha::BlockReport report = {};
  explicit GraphRig(Graph& graph) : g(graph) {}
  void set(unsigned field, float v) { CHECK(g.params().push(ParamChange{strip_param(0, field), v, 1})); }
  void render() {
    report = ha::BlockReport{};
    const ha::BlockIo io{in, out, 0, &report};
    g.render(io);
  }
};

Graph& make_graph() {
  static o3os::test::StaticSlot<Graph> slot;
  static TapMem taps;
  g_cycles = 0;
  return slot.make(&fake_cycles, taps);
}

struct Capture {
  std::int16_t l[kB];
  std::int16_t r[kB];
};

// A block of one strip: M fed (and S, when `with_s`), pan and level set, a second block rendered after a settle block (the ramps are instant: ramp 1).
Capture play(const std::int16_t* m, const std::int16_t* s, float pan, float level, bool mute = false, bool insert = false) {
  GraphRig rig(make_graph());
  struct Pass {
    static void fn(void*, std::int16_t*, std::uint32_t) noexcept {}
  };
  rig.set(gps::Source, 3.0f);
  rig.set(gps::Pan, pan);
  rig.set(gps::Volume, level);
  rig.set(gps::Mute, mute ? 1.0f : 0.0f);
  if (insert) {
    rig.g.install_insert(0, 0, &Pass::fn, nullptr);
    rig.g.set_insert_bypass(0, 0, false);
  }
  std::memcpy(rig.m, m, sizeof rig.m);
  rig.g.feed(0, rig.m);
  if (s != nullptr) {
    std::memcpy(rig.s, s, sizeof rig.s);
    rig.g.feed_side(0, rig.s);
  }
  rig.render();
  rig.render();
  Capture c{};
  for (std::uint32_t i = 0; i < kB; ++i) {
    c.l[i] = rig.out[2u * i];
    c.r[i] = rig.out[2u * i + 1u];
  }
  return c;
}

TEST_CASE("stereo: the graph makes L = M + S and R = M - S through the strip's level and pan law, exactly at the centre and as the mono graph elsewhere") {
  Rng rng(11);
  alignas(8) std::int16_t m[kB];
  alignas(8) std::int16_t s[kB];
  for (std::uint32_t i = 0; i < kB; ++i) {
    m[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(16001u)) - 8000);
    s[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(12001u)) - 6000);
  }
  // unity level, centre: L = M + S and R = M - S, every sample
  {
    const Capture c = play(m, s, 50.0f, 100.0f);
    for (std::uint32_t i = 0; i < kB; ++i) {
      CHECK(c.l[i] == m[i] + s[i]);
      CHECK(c.r[i] == m[i] - s[i]);
    }
  }
  // any pan and level: the sum of what the mono graph makes of M and of S alone (the level ramp and the pan law act on S as on M), within the roundings of two roads to the same bus
  const float pans[] = {0, 10, 25, 50, 64, 90, 100};
  const float levels[] = {100, 80, 57};
  for (const float pan : pans) {
    for (const float level : levels) {
      const Capture both = play(m, s, pan, level);
      const Capture only_m = play(m, nullptr, pan, level);
      const Capture only_s = play(s, nullptr, pan, level);  // S through the strip as a mono track: gl S and gr S
      for (std::uint32_t i = 0; i < kB; ++i) {
        INFO("pan " << pan << " level " << level << " i " << i);
        CHECK(std::abs(both.l[i] - (only_m.l[i] + only_s.l[i])) <= 1);
        CHECK(std::abs(both.r[i] - (only_m.r[i] - only_s.r[i])) <= 1);
      }
    }
  }
}

TEST_CASE("stereo: a muted or closed strip and a strip with an insert in the path take no S") {
  Rng rng(12);
  alignas(8) std::int16_t m[kB];
  alignas(8) std::int16_t s[kB];
  for (std::uint32_t i = 0; i < kB; ++i) {
    m[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(16001u)) - 8000);
    s[i] = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(12001u)) - 6000);
  }
  const Capture muted = play(m, s, 50.0f, 100.0f, true);
  const Capture closed = play(m, s, 50.0f, 0.0f);
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(muted.l[i] == 0);
    CHECK(muted.r[i] == 0);
    CHECK(closed.l[i] == 0);
    CHECK(closed.r[i] == 0);
  }
  const Capture inserted = play(m, s, 50.0f, 100.0f, false, true);  // the insert (here a pass) processes M only: the mono track
  for (std::uint32_t i = 0; i < kB; ++i) {
    CHECK(inserted.l[i] == m[i]);
    CHECK(inserted.r[i] == m[i]);
  }
}

}  // namespace
