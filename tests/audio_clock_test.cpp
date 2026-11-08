// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-ABI2 A3 (docs/specs/OS-INSFX-ABI2.md section 3.4), the engine's side of get_clock, with a scripted event source (the sequencer's clock_at has its own tests: seq_clock_test.cpp) and the
// real engine, voices and module host table. Host and Cortex-M4F.
//   * the engine asks the source's `clock` once a block, at the block's end, with the first sample of the NEXT block, and writes the answer to the clock store behind the module host's get_clock: a
//     module that calls it in block b (the script's step() does, where the voices and modules render) reads what the source said for block b's first sample; block 0 has none: the stub's answer, 1;
//   * a source without a `clock` leaves the stub; attach_events puts the stub back;
//   * the values reach the module whole: flags, the tempo, the Q16.16 beat as a float, the size rule (a caller that knows less gets the members that fit);
//   * an export asks at the start of each of its blocks on its own clock (sample 0 up), so a module in its block b reads that block's values (the first block too), two exports give the same sequence,
//     and the live source is neither asked nor written meanwhile.
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <cstdint>
#include <cstring>

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os::audio;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr std::uint32_t kBlocks = 24;

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 7u; }

// A source whose clock is a pure function of the sample: flags PLAYING (+ WRAPPED on every 5th block), the beat is the sample in beats of 256 samples (Q16.16), the tempo moves with the block so that a
// stale value cannot pass for the right one. Its step() is where a module would run: it calls get_clock and writes down what it got, by block.
struct Script {
  ModuleVoices* modules = nullptr;
  std::uint32_t asked = 0;  // the clock function's calls
  std::uint64_t samples[kBlocks] = {};
  std::int32_t rc[kBlocks];
  std::uint32_t flags[kBlocks];
  std::uint32_t tempo[kBlocks];
  float beat[kBlocks];
  std::uint32_t stepped = 0;
  Script() {
    for (std::uint32_t i = 0; i < kBlocks; ++i) {
      rc[i] = -1;
      flags[i] = tempo[i] = 0;
      beat[i] = -1.0f;
    }
  }
};

std::uint32_t script_step(void* self, std::uint64_t block_start, std::uint32_t, SeqEvent*, std::uint32_t) noexcept {
  Script& s = *static_cast<Script*>(self);
  const std::uint32_t b = static_cast<std::uint32_t>(block_start / kBlk);
  ++s.stepped;
  if (b < kBlocks) {
    o3m_clock c{};
    c.size = sizeof c;
    s.rc[b] = s.modules->host().get_clock(&c);
    s.flags[b] = c.flags;
    s.tempo[b] = c.tempo_x100;
    s.beat[b] = c.beat;
  }
  return 0;
}

std::uint32_t beat_q16(std::uint64_t sample) { return static_cast<std::uint32_t>(sample * 65536u / 256u); }

ClockPos script_clock(void* self, std::uint64_t sample) noexcept {
  Script& s = *static_cast<Script*>(self);
  if (s.asked < kBlocks) {
    s.samples[s.asked] = sample;
  }
  ++s.asked;
  const std::uint32_t block = static_cast<std::uint32_t>(sample / kBlk);
  return ClockPos{1u | ((block % 5u) == 0u ? 4u : 0u), beat_q16(sample), 12000u + block};
}

float beat_float(std::uint64_t sample) { return static_cast<float>(beat_q16(sample)) * (1.0f / 65536.0f); }

struct Rig {
  Rig() : engine(&fake_cycles, taps) {
    engine.attach_voices(&pool);
    engine.attach_modules(&modules);
    engine.attach_live(&live_box);
  }
  EventSource source_of(Script& s, bool with_clock = true) {
    s.modules = &modules;
    EventSource e;
    e.self = &s;
    e.step = &script_step;
    e.clock = with_clock ? &script_clock : nullptr;
    e.buffer = events;
    e.capacity = kMaxEventsPerBlock;
    return e;
  }
  void live_block() {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    const haudio::BlockCallback cb = engine.callback();
    cb.fn(cb.user, io);
  }
  void offline_block() {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine.render_offline(io);
  }
  std::int32_t ask(o3m_clock& c) {
    c = o3m_clock{};
    c.size = sizeof c;
    return modules.host().get_clock(&c);
  }

  TapMem taps;
  Engine engine;
  VoicePool pool;
  ModuleVoices modules;
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

}  // namespace

TEST_CASE("clock: the engine asks the source once a block with the next block's first sample; a module in block b reads block b's values; block 0 has the stub's") {
  Rig& r = make_rig();
  Script script;
  r.engine.attach_events(r.source_of(script));
  o3m_clock c;
  CHECK(r.ask(c) == 1);  // nothing written yet
  CHECK((c.size == sizeof c && c.flags == 0u && c.tempo_x100 == 12000u && c.beat == 0.0f));
  for (std::uint32_t b = 0; b < kBlocks; ++b) {
    r.live_block();
    CHECK(script.asked == b + 1u);  // once a block
    CHECK(script.samples[b] == static_cast<std::uint64_t>(b + 1u) * kBlk);  // the sample the next block starts at
  }
  CHECK(script.stepped == kBlocks);
  CHECK((script.rc[0] == 1 && script.flags[0] == 0u && script.tempo[0] == 12000u && script.beat[0] == 0.0f));  // no block ended before it
  for (std::uint32_t b = 1; b < kBlocks; ++b) {
    INFO("block " << b);
    const std::uint64_t first = static_cast<std::uint64_t>(b) * kBlk;
    CHECK(script.rc[b] == 0);
    CHECK(script.flags[b] == (1u | ((b % 5u) == 0u ? 4u : 0u)));
    CHECK(script.tempo[b] == 12000u + b);
    CHECK(script.beat[b] == beat_float(first));
  }
  // what a module sees after the run is the last block's next one; and a caller that knows less gets the members that fit, the answer still 0
  CHECK(r.ask(c) == 0);
  CHECK((c.size == sizeof c && c.tempo_x100 == 12000u + kBlocks));
  o3m_clock small{};
  small.size = 8;
  small.tempo_x100 = 77u;
  small.beat = 9.0f;
  CHECK(r.modules.host().get_clock(&small) == 0);
  CHECK((small.size == 8u && small.flags != 0u && small.tempo_x100 == 77u && small.beat == 9.0f));
}

TEST_CASE("clock: a source without a clock leaves the stub; attaching a source puts the stub back until its first block ends") {
  Rig& r = make_rig();
  Script script;
  r.engine.attach_events(r.source_of(script, false));
  for (int b = 0; b < 6; ++b) {
    r.live_block();
  }
  o3m_clock c;
  CHECK(r.ask(c) == 1);
  CHECK((c.flags == 0u && c.tempo_x100 == 12000u && c.beat == 0.0f));
  CHECK(script.asked == 0u);
  // with a clock: written after a block (the engine's clock is at block 6)
  r.engine.attach_events(r.source_of(script));
  CHECK(r.ask(c) == 1);
  r.live_block();
  CHECK(r.ask(c) == 0);
  CHECK(c.tempo_x100 == 12000u + 7u);
  // a source with no clock after one with: back to the stub, not the old source's last answer
  r.engine.attach_events(r.source_of(script, false));
  CHECK(r.ask(c) == 1);
  r.live_block();
  CHECK(r.ask(c) == 1);
}

TEST_CASE("clock: an export asks at the start of each block on its own clock, so its first block has a value; two exports are the same; the live source is not asked meanwhile") {
  Rig& r = make_rig();
  Script live;
  r.engine.attach_events(r.source_of(live));
  for (int b = 0; b < 10; ++b) {
    r.live_block();
  }
  const std::uint32_t live_asked = live.asked;
  o3m_clock live_before;
  CHECK(r.ask(live_before) == 0);

  Script pass[2];
  for (int p = 0; p < 2; ++p) {
    r.engine.offline_request();
    CHECK(r.engine.offline_ready(false));
    r.engine.offline_enter(r.source_of(pass[p]));
    CHECK(r.engine.offline_entered());
    for (std::uint32_t b = 0; b < 12u; ++b) {
      r.offline_block();
    }
    r.engine.offline_leave();
    CHECK(pass[p].asked == 12u);  // once a block, at its start
    for (std::uint32_t b = 0; b < 12u; ++b) {
      INFO("pass " << p << " block " << b);
      const std::uint64_t first = static_cast<std::uint64_t>(b) * kBlk;
      CHECK(pass[p].samples[b] == first);  // the block's own first sample (the live engine asks for the next block's, at the end)
      CHECK(pass[p].rc[b] == 0);           // the first block too
      CHECK(pass[p].flags[b] == (1u | ((b % 5u) == 0u ? 4u : 0u)));
      CHECK(pass[p].tempo[b] == 12000u + b);
      CHECK(pass[p].beat[b] == beat_float(first));
    }
  }
  CHECK(std::memcmp(pass[0].flags, pass[1].flags, sizeof pass[0].flags) == 0);
  CHECK(std::memcmp(pass[0].tempo, pass[1].tempo, sizeof pass[0].tempo) == 0);
  CHECK(std::memcmp(pass[0].beat, pass[1].beat, sizeof pass[0].beat) == 0);
  CHECK(live.asked == live_asked);  // the live source was not asked
  // the live engine goes on where it was: its next clock is the live one's, for the block after the one it renders
  r.live_block();
  CHECK(live.asked == live_asked + 1u);
  CHECK(live.samples[live_asked] == 11u * kBlk);
}
