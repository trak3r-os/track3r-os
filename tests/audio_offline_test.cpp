// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine::offline_* (OS-WAVEXP): the engine's side of the offline render, with a scripted event source and the real graph and voices (no project, no sequencer: those are in
// seq_offline_test.cpp), so that it also runs on the Cortex-M4F.
//   * render_offline() makes the blocks render_block() makes, bit for bit, from a source of its own on a clock that starts at 0, and not one live note is taken;
//   * the ownership protocol: once the request stands, a live block (the engine's callback) is a block of silence that renders nothing, asks the live source nothing and moves no clock; offline_ready()
//     says when the render has stood down (or at once when the audio is not running); offline_leave() gives it all back: the live source, the clock, the stems tap, the input feed;
//   * every voice is stopped when the export takes the engine and when it gives it back.
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os::audio;

constexpr std::uint32_t kBlk = haudio::kBlock;

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 7u; }

// A source that plays a note at block 2 (sample 17) and ends it at block 5, and writes down the block starts it was asked for.
struct Script {
  std::uint32_t calls = 0;
  std::uint64_t last_start = 0;
  std::uint64_t starts[64] = {};
  std::uint32_t offset = 17;
  std::uint32_t params = 0;
  std::uint32_t resumes = 0;
  const Engine* engine = nullptr;     // set: resume() notes whether the engine was still the export's (parked) when it was called
  bool resumed_parked = false;
};

std::uint32_t script_step(void* self, std::uint64_t block_start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
  Script& s = *static_cast<Script*>(self);
  if (s.calls < 64u) {
    s.starts[s.calls] = block_start;
  }
  ++s.calls;
  s.last_start = block_start;
  const std::uint32_t block = static_cast<std::uint32_t>(block_start / n);
  std::uint32_t count = 0;
  if (block == 2u && count < cap) {
    SeqEvent e{};
    e.offset = static_cast<std::uint16_t>(s.offset);
    e.kind = EventKind::NoteOn;
    e.track = 0;
    e.a = 60;
    e.b = 0;
    e.c = 127;
    out[count++] = e;
  }
  if (block == 5u && count < cap) {
    SeqEvent e{};
    e.offset = 3;
    e.kind = EventKind::NoteOff;
    e.track = 0;
    out[count++] = e;
  }
  return count;
}

bool script_param(void* self, const o3os::core::ParamChange&) noexcept {
  ++static_cast<Script*>(self)->params;
  return false;
}

void script_resume(void* self) noexcept {
  Script& s = *static_cast<Script*>(self);
  ++s.resumes;
  s.resumed_parked = s.engine != nullptr && s.engine->offline_wanted();
}

struct Rig {
  Rig() : engine(&fake_cycles, taps) {
    static std::int16_t dc[2000];
    for (std::int16_t& s : dc) {
      s = 9000;
    }
    SampleDesc d{dc, 2000, 0, 0, 65536};
    pool.set_sample(0, d);
    engine.attach_voices(&pool);
    engine.attach_live(&live_box);
  }
  EventSource source_of(Script& s) {
    EventSource e;
    e.self = &s;
    e.step = &script_step;
    e.param = &script_param;
    e.resume = &script_resume;
    e.buffer = events;
    e.capacity = kMaxEventsPerBlock;
    return e;
  }
  void live_block(std::vector<std::int16_t>* into = nullptr) {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    const haudio::BlockCallback cb = engine.callback();  // the live render line's entry: it is where the export's request is looked at
    cb.fn(cb.user, io);
    if (into != nullptr) {
      into->insert(into->end(), out, out + haudio::kBlockSamples);
    }
  }
  void offline_block(std::vector<std::int16_t>* into = nullptr) {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine.render_offline(io);
    if (into != nullptr) {
      into->insert(into->end(), out, out + haudio::kBlockSamples);
    }
  }

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

}  // namespace

TEST_CASE("offline engine: render_offline is render_block, bit for bit, from the export's own source on a clock that starts at 0") {
  std::vector<std::int16_t> live;
  {
    Rig& r = make_rig();
    Script script;
    r.engine.attach_events(r.source_of(script));
    for (int b = 0; b < 12; ++b) {
      r.live_block(&live);
    }
  }
  Rig& r = make_rig();
  Script live_script;
  Script export_script;
  r.engine.attach_events(r.source_of(live_script));
  // The live engine has been running: its clock is far from 0.
  for (int b = 0; b < 30; ++b) {
    r.live_block();
  }
  r.pool.cut(0);
  const std::uint64_t clock = r.engine.sample_clock();
  CHECK(clock == 30u * kBlk);
  const std::uint32_t live_calls = live_script.calls;

  r.engine.offline_request();
  CHECK(r.engine.offline_wanted());
  CHECK(r.engine.offline_ready(false));    // the audio is not running: nothing to wait for
  CHECK_FALSE(r.engine.offline_ready(true));  // ... and if it were, it has not seen the request yet
  r.engine.offline_enter(r.source_of(export_script));
  CHECK(r.engine.offline_entered());
  CHECK(r.engine.sample_clock() == 0u);
  std::vector<std::int16_t> off;
  for (int b = 0; b < 12; ++b) {
    r.offline_block(&off);
  }
  CHECK(live == off);
  CHECK(export_script.calls == 12u);
  for (std::uint32_t b = 0; b < 12u; ++b) {
    CHECK(export_script.starts[b] == static_cast<std::uint64_t>(b) * kBlk);
  }
  CHECK(live_script.calls == live_calls);  // the live source was not asked
  CHECK(r.engine.sample_clock() == 12u * kBlk);
  // The note sounded: a comparison of silence would prove nothing.
  std::int32_t peak = 0;
  for (std::int16_t s : off) {
    peak = std::max<std::int32_t>(peak, s < 0 ? -s : s);
  }
  CHECK(peak > 1000);

  r.engine.offline_leave();
  CHECK_FALSE(r.engine.offline_wanted());
  CHECK_FALSE(r.engine.offline_entered());
  CHECK(r.engine.sample_clock() == clock);
  CHECK(r.pool.voices_in_use() == 0u);
  r.live_block();
  CHECK(live_script.calls == live_calls + 1u);
  CHECK(live_script.last_start == clock);  // the live source goes on where it was
  CHECK(r.engine.sample_clock() == clock + kBlk);
}

TEST_CASE("offline engine: while the request stands a live block is silence that renders nothing; a live note queued before the export is dropped when it takes the engine (OS-EXPNOTE)") {
  Rig& r = make_rig();
  Script script;
  r.engine.attach_events(r.source_of(script));
  r.live_block();
  CHECK(script.calls == 1u);
  const LiveEvent on{LiveKind::On, kLiveSourcePad, 3, 60, 1, kLiveVelocityDefault, 0, 0};
  CHECK(r.live_box.queue.push(on));

  r.engine.offline_request();
  std::memset(r.out, 0x55, sizeof r.out);
  r.live_block();
  for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
    CHECK(r.out[i] == 0);
  }
  CHECK(script.calls == 1u);  // nothing rendered: the source was not asked
  CHECK(r.engine.sample_clock() == kBlk);
  CHECK(r.engine.offline_ready(true));
  CHECK_FALSE(r.live_box.queue.empty());  // the live note is still there while the request stands

  // The export takes the engine: the note the render never took is dropped and counted (it would start after the export, its off possibly lost), named as the pad's note.
  Script export_script;
  r.engine.offline_enter(r.source_of(export_script));
  CHECK(r.live_box.queue.empty());
  CHECK(r.engine.live_stopped_count() == 1u);
  CHECK(o3os::audio::held_source(r.engine.live_stopped_what()) == kLiveSourcePad);
  CHECK(o3os::audio::held_note(r.engine.live_stopped_what()) == 60u);
  for (int b = 0; b < 4; ++b) {
    r.offline_block();
  }
  CHECK(r.pool.voices_in_use() == 1u);  // the export's own note (block 2 of its script), not the pad's
  r.engine.offline_leave();  // (the export's note is stopped; its residual fades in the first live block)

  r.live_block();
  CHECK(r.live_box.queue.empty());
  CHECK(r.pool.voices_in_use() == 0u);  // the pad's note did not start
  CHECK(script.calls == 2u);
}

// STATUS latent (aa): a note that sounds when the export takes the engine is cut, and the cut leaves a declick residual that fades over the next block. That tail belongs to the live render,
// so it must not be in the file: the first block of an export of a project is the same whatever the pads were doing (two exports give the same bits).
TEST_CASE("offline engine: a live note that sounds when the export takes the engine leaves no declick tail in the export's first block") {
  Rig& r = make_rig();
  Script live_script;
  live_script.offset = 0;
  r.engine.attach_events(r.source_of(live_script));
  for (int b = 0; b < 4; ++b) {
    r.live_block();  // the scripted note of block 2 sounds (DC 9000 a sample)
  }
  CHECK(r.pool.voices_in_use() == 1u);
  r.engine.offline_request();
  Script export_script;  // its note starts in block 2: block 0 is silence
  r.engine.offline_enter(r.source_of(export_script));
  std::vector<std::int16_t> first;
  r.offline_block(&first);
  std::int32_t peak = 0;
  for (std::int16_t v : first) {
    peak = std::max<std::int32_t>(peak, v < 0 ? -v : v);
  }
  CHECK(peak == 0);
  CHECK(r.pool.voices_in_use() == 0u);
  r.engine.offline_leave();
}

TEST_CASE("offline engine: the stems tap and the input feed are put away and come back; enter without a request, a second enter and a leave without one do nothing") {
  Rig& r = make_rig();
  Script script;
  r.engine.attach_events(r.source_of(script));
  r.engine.graph().stems().set_enabled(true);
  r.engine.graph().set_input_feed(InputFeed::ReturnOnly);

  Script export_script;
  r.engine.offline_enter(r.source_of(export_script));  // no request
  CHECK_FALSE(r.engine.offline_entered());
  CHECK(r.engine.graph().stems().enabled());
  r.engine.offline_leave();  // no enter: only clears the (absent) request
  CHECK_FALSE(r.engine.offline_wanted());

  r.engine.offline_request();
  r.engine.offline_enter(r.source_of(export_script));
  CHECK(r.engine.offline_entered());
  CHECK_FALSE(r.engine.graph().stems().enabled());
  CHECK(r.engine.graph().input_feed() == InputFeed::CodecOnly);
  Script other;
  r.engine.offline_enter(r.source_of(other));  // already entered: the first source stays
  r.offline_block();
  CHECK(export_script.calls == 1u);
  CHECK(other.calls == 0u);
  r.engine.offline_leave();
  CHECK(r.engine.graph().stems().enabled());
  CHECK(r.engine.graph().input_feed() == InputFeed::ReturnOnly);
  r.engine.offline_leave();  // twice
  CHECK_FALSE(r.engine.offline_wanted());
  // render_offline outside an export renders nothing and moves nothing
  const std::uint64_t clock = r.engine.sample_clock();
  r.offline_block();
  CHECK(r.engine.sample_clock() == clock);
}

TEST_CASE("offline engine: every voice is stopped when the export takes the engine and when it gives it back") {
  Rig& r = make_rig();
  Script script;
  script.offset = 0;
  r.engine.attach_events(r.source_of(script));
  for (int b = 0; b < 4; ++b) {
    r.live_block();
  }
  CHECK(r.pool.voices_in_use() == 1u);  // the note of block 2 sounds
  r.engine.offline_request();
  Script export_script;
  r.engine.offline_enter(r.source_of(export_script));
  r.offline_block();
  r.offline_block();
  CHECK(r.pool.voices_in_use() == 0u);  // (the cut's residual faded within the first block)
  // a note the export starts is stopped at the leave
  Script again;
  again.offset = 0;
  r.engine.offline_leave();
  r.engine.offline_request();
  r.engine.offline_enter(r.source_of(again));
  for (int b = 0; b < 4; ++b) {
    r.offline_block();
  }
  CHECK(r.pool.voices_in_use() == 1u);
  r.engine.offline_leave();
  r.live_block();
  CHECK(r.pool.voices_in_use() == 0u);
}

TEST_CASE("offline engine: the apps' tap, the scope and the spectrum are put away for the export and come back as they were") {
  Rig& r = make_rig();
  Script script;
  r.engine.attach_events(r.source_of(script));
  static AppTap tap;
  tap.reset();
  r.engine.graph().attach_app_tap(&tap);
  tap.set_source(AppTap::Source::Master);
  r.engine.graph().set_scope_enabled(true);
  r.engine.graph().set_spectrum_enabled(false);
  r.live_block();
  const std::uint32_t head = tap.head();
  CHECK(head != 0u);  // the tap is fed by a live block

  r.engine.offline_request();
  Script export_script;
  r.engine.offline_enter(r.source_of(export_script));
  CHECK(tap.source() == AppTap::Source::Off);
  CHECK_FALSE(r.engine.graph().scope_enabled());
  CHECK_FALSE(r.engine.graph().spectrum_enabled());
  for (int b = 0; b < 8; ++b) {
    r.offline_block();
  }
  CHECK(tap.head() == head);  // an app's tuner or scope did not hear the export
  r.engine.offline_leave();
  CHECK(tap.source() == AppTap::Source::Master);
  CHECK(r.engine.graph().scope_enabled());
  CHECK_FALSE(r.engine.graph().spectrum_enabled());
  r.live_block();
  CHECK(tap.head() != head);  // and hears the live render again
  r.engine.graph().attach_app_tap(nullptr);
  r.engine.graph().set_scope_enabled(false);
}

TEST_CASE("offline engine: a ParamChange the graph does not own reaches the export's source and the live one, which is parked and would never see it") {
  Rig& r = make_rig();
  Script live_script;
  r.engine.attach_events(r.source_of(live_script));
  r.engine.offline_request();
  Script export_script;
  r.engine.offline_enter(r.source_of(export_script));
  const o3os::core::ParamChange change{o3os::core::param_id(o3os::core::ParamKind::Project, 0, 0), 100.0f, 0u};
  CHECK(r.engine.params().push(change));
  r.offline_block();  // the first export block drains the ring
  CHECK(export_script.params == 1u);
  CHECK(live_script.params == 1u);
  r.engine.offline_leave();
  CHECK(r.engine.params().push(change));
  r.live_block();  // back with the live source alone
  CHECK(live_script.params == 2u);
  CHECK(export_script.params == 1u);
}

TEST_CASE("offline engine: giving the engine back tells the live source to resume - once, while the render is still parked - and not the export's") {
  Rig& r = make_rig();
  Script live_script;
  live_script.engine = &r.engine;
  r.engine.attach_events(r.source_of(live_script));
  r.engine.offline_request();
  Script export_script;
  export_script.engine = &r.engine;
  r.engine.offline_enter(r.source_of(export_script));
  r.offline_block();
  CHECK(live_script.resumes == 0u);
  r.engine.offline_leave();
  CHECK(live_script.resumes == 1u);
  CHECK(live_script.resumed_parked);  // before `want_` was cleared: the render line could not have been in the source meanwhile
  CHECK(export_script.resumes == 0u);
  r.engine.offline_leave();  // twice: nothing again
  CHECK(live_script.resumes == 1u);
}

TEST_CASE("offline engine: many exports in a row, as a set of stems makes them - the tap, the scope and the spectrum come back each time, the live source resumes each time, and every pass is the same blocks") {
  Rig& r = make_rig();
  Script live_script;
  live_script.engine = &r.engine;
  EventSource live = r.source_of(live_script);
  live.step = [](void*, std::uint64_t, std::uint32_t, SeqEvent*, std::uint32_t) noexcept -> std::uint32_t { return 0; };  // (the transport is stopped between two files: no live note)
  r.engine.attach_events(live);
  static AppTap tap;
  tap.reset();
  r.engine.graph().attach_app_tap(&tap);
  tap.set_source(AppTap::Source::Master);
  r.engine.graph().set_scope_enabled(true);
  r.engine.graph().set_spectrum_enabled(true);
  r.live_block();
  const std::uint32_t first_head = tap.head();
  std::vector<std::int16_t> first;
  constexpr unsigned kCycles = 8;
  for (unsigned cycle = 0; cycle < kCycles; ++cycle) {
    CAPTURE(cycle);
    const std::uint32_t head = tap.head();  // (the live block between two exports moved them)
    const std::uint64_t clock = r.engine.sample_clock();
    r.engine.offline_request();
    Script export_script;
    r.engine.offline_enter(r.source_of(export_script));
    CHECK(tap.source() == AppTap::Source::Off);
    CHECK_FALSE(r.engine.graph().scope_enabled());
    CHECK_FALSE(r.engine.graph().spectrum_enabled());
    std::vector<std::int16_t> pcm;
    for (int b = 0; b < 40; ++b) {
      r.offline_block(&pcm);
    }
    CHECK(tap.head() == head);  // never an app's tuner or scope: not one of the exports
    r.engine.offline_leave();
    CHECK(tap.source() == AppTap::Source::Master);
    CHECK(r.engine.graph().scope_enabled());
    CHECK(r.engine.graph().spectrum_enabled());
    CHECK(live_script.resumes == cycle + 1u);  // the MIDI link starts its clock loop over each time
    CHECK(r.engine.sample_clock() == clock);
    CHECK_FALSE(r.engine.offline_wanted());
    if (cycle == 0u) {
      first = pcm;
    } else {
      std::size_t at = 0;
      while (at < pcm.size() && at < first.size() && pcm[at] == first[at]) {
        ++at;
      }
      CHECK_MESSAGE(pcm == first, "first difference at " << at << " of " << pcm.size() << ": " << (at < pcm.size() ? pcm[at] : 0) << " against " << (at < first.size() ? first[at] : 0));  // the limiter and the effects start every pass alike
    }
    r.live_block();  // the live render between two files
  }
  CHECK(tap.head() != first_head);  // and it hears the live render between them
  r.engine.graph().attach_app_tap(nullptr);
  r.engine.graph().set_scope_enabled(false);
  r.engine.graph().set_spectrum_enabled(false);
}
