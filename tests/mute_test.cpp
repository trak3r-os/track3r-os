// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MUTE: the sound-level mute and solo of the Pattern screen and the TRACKS pads. The actions write the Track parameters Mute and Solo (the Mixer's: undoable, told to the graph), the sequencer no
// longer skips a muted track (only a MIDI track's notes are held back, the graph does not carry MIDI), a loaded mute reaches the strip and does not mark the project modified. The per-track MUTE MODE
// (Immediate, Bar, Pattern): the deferred toggle lands on the exact boundary sample and a second press cancels it; the per-track state and the activity counters of the TRACKS pads.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_fixture.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

#include <o3os/app/app.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/save_service.h>
#include <o3os/app/track_state.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/voices.h>
#include <o3os/core/input_event.h>
#include <o3os/formats/og.h>
#include <o3os/hal/audio.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/sequencer.h>

#include "support/mem_card.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace gp = audio::graph_param;
namespace haudio = o3os::hal::audio;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 11u; }

// ---- the App, with a document and the ring of ParamChange to the audio side ----------------------------------------------------------------

struct AppRig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  test::MemCard card;
  app::SaveService saves;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  core::ParamRing ring;
  model::ModelArena& arena;

  explicit AppRig(model::ModelArena& a) : arena(a) {
    arena.project.reset();
    arena.project.start_new();  // C major, the runtime's default: the document is not unsaved at the start (OS-DEFSCALE)
    (void)arena.project.create_pattern(1, 16);
    (void)arena.project.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena.project, journal);
    app->attach_document(editor.get());
    saves.attach(card, card);
    app->attach_saves(&saves);
    app->attach_param_ring(&ring);
    app->run_once();
    (void)sent();
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  app::DispatchResult invoke(const char* name, core::ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  std::vector<core::ParamChange> sent() {
    std::vector<core::ParamChange> out;
    core::ParamChange c;
    while (ring.pop(c)) {
      out.push_back(c);
    }
    return out;
  }
  const model::TrackSlot& track(unsigned t) const { return *arena.project.track(t); }
};

core::ParamId strip(unsigned t, unsigned field) { return audio::strip_param(t, field); }

bool has(const std::vector<core::ParamChange>& v, core::ParamId id, float value) {
  for (const core::ParamChange& c : v) {
    if (c.id == id && c.value == value) {
      return true;
    }
  }
  return false;
}

// ---- the sequencer, the voices and the graph -----------------------------------------------------------------------------------------------

constexpr std::uint32_t kBlk = haudio::kBlock;

struct SoundRig {
  audio::VoicePool* voices = nullptr;
  audio::Engine* engine = nullptr;
  seq::Sequencer* seq = nullptr;
  audio::SeqEvent events[audio::kMaxEventsPerBlock];
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  std::int32_t peak = 0;

  // The project is seq_project(): `tracks` tracks, four sample instruments, one MIDI instrument in slot 10 (support/seq_fixture.h); slot 0 plays a looped DC sample, so a note rings on.
  explicit SoundRig(std::uint32_t tracks, std::uint32_t steps = 16) {
    static StaticSlot<audio::VoicePool> pool_slot;
    static StaticSlot<audio::Engine> engine_slot;
    static StaticSlot<seq::Sequencer> seq_slot;
    static audio::TapMem taps;
    static std::int16_t dc[3000];
    for (std::int16_t& v : dc) {
      v = 16384;
    }
    fresh_project(tracks, 4);
    model::Project& p = seq_project();
    (void)p.set_tempo_x100(12000);
    (void)p.create_pattern(1, steps);
    voices = &pool_slot.make();
    voices->set_sample(0, audio::SampleDesc{dc, 3000, 0, 3000, 65536});
    engine = &engine_slot.make(&fake_cycles, taps);
    seq = &seq_slot.make(p);
    engine->attach_voices(voices);
    engine->attach_events(seq::event_source(*seq, events, audio::kMaxEventsPerBlock));
  }
  void block() {
    haudio::BlockReport report;
    const haudio::BlockIo io{in, out, 0, &report};
    engine->render_block(io);
    for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
      const std::int32_t a = std::abs(static_cast<std::int32_t>(out[i]));
      peak = a > peak ? a : peak;
    }
  }
  void render(std::uint32_t blocks) {
    for (std::uint32_t b = 0; b < blocks; ++b) {
      block();
    }
  }
  bool tell(unsigned t, unsigned field, float value, std::uint32_t ramp = 1) {
    return engine->params().push(audio::ParamChange{audio::strip_param(t, field), value, ramp});
  }
};

}  // namespace

TEST_CASE("mute: track.mute and track.solo write the Track parameters Mute and Solo (the Mixer's), tell the strip, and are undoable") {
  static model::ModelArena arena;
  AppRig rig(arena);
  CHECK(rig.invoke("track.mute", 2) == app::DispatchResult::Changed);
  CHECK(rig.track(2).flags == model::kTrackMute);
  CHECK(has(rig.sent(), strip(2, gp::strip::Mute), 1.0f));  // the graph's strip, not the sequencer, is the mute
  CHECK(rig.invoke("track.solo", 4) == app::DispatchResult::Changed);
  CHECK(rig.track(4).flags == model::kTrackSolo);
  CHECK(has(rig.sent(), strip(4, gp::strip::Solo), 1.0f));
  CHECK(rig.invoke("track.mute", 4) == app::DispatchResult::Changed);  // both on one track
  CHECK(rig.track(4).flags == (model::kTrackMute | model::kTrackSolo));
  (void)rig.sent();
  // Undo is one toggle at a time and the engine hears it (the unit's parameters are told by value).
  CHECK(rig.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(rig.track(4).flags == model::kTrackSolo);
  CHECK(has(rig.sent(), strip(4, gp::strip::Mute), 0.0f));
  CHECK(rig.invoke("edit.redo") == app::DispatchResult::Changed);
  CHECK(has(rig.sent(), strip(4, gp::strip::Mute), 1.0f));
  // It is the Mixer's parameter: the same value read through the parameter path.
  CHECK(rig.arena.project.get_param(core::param_id(core::ParamKind::Track, 2, model::track_param::Mute)) == 1.0f);
  CHECK(rig.arena.project.get_param(core::param_id(core::ParamKind::Track, 4, model::track_param::Solo)) == 1.0f);
  // Not for a track that is not active, nor out of range.
  CHECK(rig.invoke("track.mute", 12) != app::DispatchResult::Changed);
  CHECK(rig.invoke("track.solo", -1) != app::DispatchResult::Changed);
}

TEST_CASE("mute: a loaded mute (the OG's project.mt, older native projects) reaches the strips and the project is not modified, nothing is an undo unit") {
  static model::ModelArena arena;
  AppRig rig(arena);
  test::og::ProjectSpec spec;
  spec.version = 16;
  for (std::uint8_t& m : spec.mute) {
    m = 0;
  }
  spec.mute[1] = 1;
  spec.mute[6] = 1;
  formats::og::ProjectReader reader;
  NEED(test::og::run_reader(reader, test::og::project_bytes(spec), arena) == formats::Progress::Done);
  CHECK(arena.project.track(1)->flags == model::kTrackMute);  // the flag the OG's mute byte becomes IS the Track parameter Mute
  (void)rig.sent();
  rig.app->document_replaced();
  rig.pass();
  const std::vector<core::ParamChange> sent = rig.sent();
  for (unsigned t = 0; t < audio::kTracks; ++t) {
    CHECK_MESSAGE(has(sent, strip(t, gp::strip::Mute), (t == 1 || t == 6) ? 1.0f : 0.0f), "track " << t);
    CHECK_MESSAGE(has(sent, strip(t, gp::strip::Solo), 0.0f), "track " << t);
  }
  CHECK(arena.project.track(1)->flags == model::kTrackMute);  // still the project's value
  CHECK_FALSE(rig.saves.dirty(arena.project));
  CHECK_FALSE(rig.editor->can_undo());
  // A project that mutes nothing clears the last one's mutes in the strips.
  arena.project.reset();
  rig.app->document_replaced();
  rig.pass();
  const std::vector<core::ParamChange> cleared = rig.sent();
  CHECK(has(cleared, strip(1, gp::strip::Mute), 0.0f));
  CHECK_FALSE(has(cleared, strip(1, gp::strip::Mute), 1.0f));
}

TEST_CASE("mute: a muted sample track keeps firing notes (the voices start, the strip is at 0) and an unmute is heard mid-note") {
  SoundRig r(2);
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 1)) == model::Status::Ok);
  NEED(r.tell(0, gp::strip::Mute, 1.0f));  // what track.mute sends
  NEED(r.seq->commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0}));
  r.render(40);
  CHECK(r.voices->stats().note_ons == 1u);  // the sequencer did not skip the muted track
  CHECK(r.voices->voices_in_use() == 1u);   // the looped note is ringing
  CHECK(r.peak == 0);                       // ... and nothing is heard
  NEED(r.tell(0, gp::strip::Mute, 0.0f, 0));
  r.render(4);
  CHECK(r.voices->stats().note_ons == 1u);  // the same note, no new one
  CHECK_MESSAGE(r.peak > 1000, "the unmute is not heard mid-note: peak " << r.peak);
  // And back: the note goes on, silent again.
  r.peak = 0;
  NEED(r.tell(0, gp::strip::Mute, 1.0f, 0));
  r.render(3);  // the ramp
  r.peak = 0;
  r.render(10);
  CHECK(r.peak == 0);
  CHECK(r.voices->voices_in_use() == 1u);
}

TEST_CASE("mute: a MIDI track holds its notes back while muted, or while another track is soloed, and a sounding note gets its off") {
  SoundRig r(3);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 11)) == model::Status::Ok);   // track 0: a MIDI note (slot 10) at step 0 ...
  CHECK(p.set_step(1, 0, 4, make_step(62, 11)) == model::Status::Ok);   // ... and at step 4
  CHECK(p.set_step(1, 0, 8, make_step(64, 11)) == model::Status::Ok);   // ... and at step 8
  CHECK(p.set_step(1, 0, 12, make_step(65, 11)) == model::Status::Ok);  // ... and at step 12
  std::uint32_t ons = 0;
  std::uint32_t offs = 0;
  std::uint64_t at = 0;
  const auto run_blocks = [&](std::uint32_t blocks) {
    for (std::uint32_t b = 0; b < blocks; ++b) {
      audio::SeqEvent out[audio::kMaxEventsPerBlock];
      const std::uint32_t n = r.seq->step(at, kBlk, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < n; ++i) {
        ons += out[i].kind == EventKind::MidiNoteOn ? 1u : 0u;
        offs += out[i].kind == EventKind::MidiNoteOff ? 1u : 0u;
      }
      at += kBlk;
    }
  };
  NEED(r.seq->commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0}));
  run_blocks(10);  // the first note (a step is 0.125 s at 120 BPM: 86 blocks, 4 steps a beat)
  CHECK(ons == 1u);
  CHECK(offs == 0u);
  // Mute track 0 (TrackSlot flags, what track.mute writes): the sounding note ends, the next ones are not sent.
  model::TrackSlot t0 = *p.track(0);
  t0.flags = model::kTrackMute;
  CHECK(p.set_track(0, t0) == model::Status::Ok);
  run_blocks(2);
  CHECK(offs == 1u);
  run_blocks(340);  // up to just past the step 4 note (block 345)
  CHECK(ons == 1u);
  // Unmuted but track 1 soloed: track 0 is silenced by the other's solo.
  t0.flags = 0;
  CHECK(p.set_track(0, t0) == model::Status::Ok);
  model::TrackSlot t1 = *p.track(1);
  t1.flags = model::kTrackSolo;
  CHECK(p.set_track(1, t1) == model::Status::Ok);
  run_blocks(344);  // the step 8 note (block 689)
  CHECK(ons == 1u);
  // Solo on track 0 itself: it plays.
  t1.flags = 0;
  CHECK(p.set_track(1, t1) == model::Status::Ok);
  t0.flags = model::kTrackSolo;
  CHECK(p.set_track(0, t0) == model::Status::Ok);
  run_blocks(344);  // the step 12 note (block 1034)
  CHECK(ons == 2u);
}

// ---- MUTE MODE: the deferred toggle ----------------------------------------------------------------------------------------------------------

namespace {

StaticSlot<seq::Sequencer>& g_slot = seq_slot();
Collector& g_col = seq_collector();

// The exact sample of `ticks` 48ths of a step at a tempo (core/qtime.h), as in seq_sequencer_test.cpp.
std::uint64_t exact_sample(std::uint64_t ticks, std::uint32_t bpm_x100) {
  const std::uint64_t n = ticks * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t d = 4u * core::kRefCyclesPerSample.num * static_cast<std::uint64_t>(bpm_x100);
  return (2u * n + d) / (2u * d);
}
std::uint64_t step_sample(std::uint32_t step) { return exact_sample(48u * step, 12000); }

// 120 BPM, a pattern of `steps` steps, `tracks` tracks; a sequencer playing it.
seq::Sequencer& play32(std::uint32_t steps = 32, std::uint32_t tracks = 4) {
  fresh_project(tracks, 4);
  model::Project& p = seq_project();
  (void)p.set_tempo_x100(12000);
  (void)p.create_pattern(1, steps);
  seq::Sequencer& s = g_slot.make(p);
  g_col.n = 0;
  g_col.total_events = 0;
  s.play_pattern(1);
  return s;
}

void set_mute(unsigned t, bool on) {
  model::TrackSlot ts = *seq_project().track(t);
  ts.flags = static_cast<std::uint8_t>(on ? ts.flags | model::kTrackMute : ts.flags & ~model::kTrackMute);
  (void)seq_project().set_track(t, ts);
}

bool defer(seq::Sequencer& s, unsigned t, seq::MuteQuant q, bool held) {
  return s.commands().push(seq::Command{seq::Command::Kind::MuteQuant, static_cast<std::uint8_t>(t), static_cast<std::uint16_t>(q), held ? 1u : 0u});
}

}  // namespace

TEST_CASE("mute mode: a Bar toggle lands on the exact sample of the next bar (16 steps), a Pattern toggle on the pattern's end, with the project's value") {
  for (const bool pattern_mode : {false, true}) {
    seq::Sequencer& s = play32(32);
    g_col.run(s, 0, step_sample(3) + 100, 64);  // a few steps in
    CHECK(s.mute_pending() == 0u);
    set_mute(1, true);  // what track.mute wrote: the flag changes at once ...
    NEED(defer(s, 1, pattern_mode ? seq::MuteQuant::Pattern : seq::MuteQuant::Bar, false));  // ... the strip waits
    g_col.run(s, step_sample(3) + 100, step_sample(3) + 164, 64);
    CHECK(s.mute_pending() == 2u);
    g_col.run(s, step_sample(3) + 164, step_sample(40), 64);
    NEED(g_col.count(EventKind::StripMute) == 1u);
    const Rec* r = g_col.nth(EventKind::StripMute, 0);
    CHECK(r->ev.track == 1);
    CHECK(r->ev.a == 1);  // muted
    CHECK_MESSAGE(r->t == step_sample(pattern_mode ? 32 : 16), "the boundary is the exact sample of the step that starts the bar / pattern");
    CHECK(s.mute_pending() == 0u);
  }
}

TEST_CASE("mute mode: an unmute is deferred the same way, and a second press cancels the toggle (the strip is never told)") {
  seq::Sequencer& s = play32(32);
  set_mute(2, true);  // muted before: the strip has it
  g_col.run(s, 0, step_sample(2), 64);
  set_mute(2, false);
  NEED(defer(s, 2, seq::MuteQuant::Bar, true));
  g_col.run(s, step_sample(2), step_sample(20), 64);
  NEED(g_col.count(EventKind::StripMute) == 1u);
  CHECK(g_col.nth(EventKind::StripMute, 0)->ev.a == 0);
  CHECK(g_col.nth(EventKind::StripMute, 0)->t == step_sample(16));
  // cancel: press, put the flag back, MuteCancel
  seq::Sequencer& c = play32(32);
  g_col.run(c, 0, step_sample(2), 64);
  set_mute(0, true);
  NEED(defer(c, 0, seq::MuteQuant::Pattern, false));
  g_col.run(c, step_sample(2), step_sample(4), 64);
  CHECK(c.mute_pending() == 1u);
  set_mute(0, false);
  NEED(c.commands().push(seq::Command{seq::Command::Kind::MuteCancel, 0, 0, 0}));
  g_col.run(c, step_sample(4), step_sample(40), 64);
  CHECK(c.mute_pending() == 0u);
  CHECK(g_col.count(EventKind::StripMute) == 0u);
}

TEST_CASE("mute mode: with the transport stopped, and at a stop, a deferred toggle applies at once") {
  seq::Sequencer& s = play32(32);
  g_col.n = 0;
  s.stop();  // not playing: no boundary is coming
  g_col.run(s, 0, 64, 64);
  set_mute(0, true);
  NEED(defer(s, 0, seq::MuteQuant::Bar, false));
  g_col.run(s, 64, 128, 64);
  NEED(g_col.count(EventKind::StripMute) == 1u);
  CHECK(g_col.nth(EventKind::StripMute, 0)->t < 128u);
  CHECK(s.mute_pending() == 0u);
  // while playing, a stop flushes it
  seq::Sequencer& p = play32(32);
  g_col.run(p, 0, step_sample(2), 64);
  set_mute(3, true);
  NEED(defer(p, 3, seq::MuteQuant::Pattern, false));
  g_col.run(p, step_sample(2), step_sample(3), 64);
  CHECK(p.mute_pending() == 8u);
  p.stop();
  g_col.run(p, step_sample(3), step_sample(3) + 128, 64);
  CHECK(g_col.count(EventKind::StripMute, 3) == 1u);
  CHECK(p.mute_pending() == 0u);
}

TEST_CASE("mute mode: a MIDI track follows the old mute until the boundary, then the new one") {
  seq::Sequencer& s = play32(32);
  model::Project& p = seq_project();
  for (const std::uint32_t st : {4u, 8u, 12u, 16u, 20u}) {
    CHECK(p.set_step(1, 0, st, make_step(60, 11)) == model::Status::Ok);
  }
  g_col.run(s, 0, step_sample(2), 64);
  set_mute(0, true);
  NEED(defer(s, 0, seq::MuteQuant::Bar, false));
  g_col.run(s, step_sample(2), step_sample(24), 64);
  // the notes of steps 4, 8 and 12 sound (the mute has not landed), those of 16 and 20 do not
  CHECK(g_col.count(EventKind::MidiNoteOn, 0) == 3u);
  CHECK(g_col.count(EventKind::StripMute, 0) == 1u);
}

TEST_CASE("mute mode: the strip goes down on the boundary block and not before, the note rings on") {
  SoundRig r(2, 32);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1)) == model::Status::Ok);
  NEED(r.seq->commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0}));
  r.render(20);
  r.peak = 0;
  model::TrackSlot ts = *p.track(0);
  ts.flags = model::kTrackMute;
  CHECK(p.set_track(0, ts) == model::Status::Ok);
  NEED(r.seq->commands().push(seq::Command{seq::Command::Kind::MuteQuant, 0, static_cast<std::uint16_t>(seq::MuteQuant::Bar), 0}));
  const std::uint64_t boundary_block = (step_sample(16) / kBlk);  // the block the bar's first sample is in
  r.render(static_cast<std::uint32_t>(boundary_block - 20 - 1));  // up to the block before it
  CHECK_MESSAGE(r.peak > 1000, "the strip was muted before the boundary: peak " << r.peak);
  CHECK(r.voices->voices_in_use() == 1u);
  r.render(3);  // the boundary block and the next: the level ramps down over one block
  r.peak = 0;
  r.render(10);
  CHECK_MESSAGE(r.peak == 0, "still heard after the boundary: peak " << r.peak);
  CHECK(r.voices->voices_in_use() == 1u);  // the voice goes on
}

// ---- the state of a track and its activity ---------------------------------------------------------------------------------------------------

namespace {

struct FakeSink final : app::TrackSink {
  bool run = true;
  bool accept = true;
  std::uint16_t pending = 0;
  std::uint8_t counts[16] = {};
  int defers = 0;
  int cancels = 0;
  unsigned last_track = 99;
  app::MuteMode last_mode = app::MuteMode::Immediate;
  bool last_was = false;
  bool playing() const noexcept override { return run; }
  bool mute_defer(unsigned t, app::MuteMode m, bool was) noexcept override {
    ++defers;
    last_track = t;
    last_mode = m;
    last_was = was;
    if (accept) {
      pending = static_cast<std::uint16_t>(pending | (1u << t));
    }
    return accept;
  }
  bool mute_cancel(unsigned t) noexcept override {
    ++cancels;
    pending = static_cast<std::uint16_t>(pending & ~(1u << t));
    return true;
  }
  std::uint16_t mute_pending() const noexcept override { return pending; }
  std::uint8_t note_count(unsigned t) const noexcept override { return counts[t]; }
};

}  // namespace

TEST_CASE("mute mode: track.mute in Bar mode writes the flag, tells the sink and not the strip; a second press cancels; track.mute_mode cycles") {
  static model::ModelArena arena;
  AppRig rig(arena);
  FakeSink sink;
  auto board = std::make_unique<app::MixBoard>();
  board->tracks = &sink;
  rig.app->attach_mix(board.get());
  rig.pass();
  (void)rig.sent();
  // the mode cycles Immediate -> Bar -> Pattern -> Immediate (runtime only: the project is untouched)
  CHECK(board->mute_mode[2] == 0);
  CHECK(rig.invoke("track.mute_mode", 2) == app::DispatchResult::Changed);
  CHECK(board->mute_mode[2] == static_cast<std::uint8_t>(app::MuteMode::Bar));
  CHECK_FALSE(rig.saves.dirty(arena.project));
  // Bar: the flag at once, the strip told nothing, the sequencer told the old value
  CHECK(rig.invoke("track.mute", 2) == app::DispatchResult::Changed);
  CHECK(rig.track(2).flags == model::kTrackMute);
  CHECK(sink.defers == 1);
  CHECK(sink.last_track == 2);
  CHECK(sink.last_mode == app::MuteMode::Bar);
  CHECK_FALSE(sink.last_was);
  CHECK(rig.sent().empty());
  CHECK(sink.pending == 4u);
  // a second press: the flag back, the toggle withdrawn, the strip never told
  CHECK(rig.invoke("track.mute", 2) == app::DispatchResult::Changed);
  CHECK(rig.track(2).flags == 0);
  CHECK(sink.cancels == 1);
  CHECK(sink.pending == 0u);
  CHECK(rig.sent().empty());
  // the sink refuses (no sequencer / ring full): the strip is told at once
  sink.accept = false;
  CHECK(rig.invoke("track.mute", 2) == app::DispatchResult::Changed);
  CHECK(has(rig.sent(), strip(2, gp::strip::Mute), 1.0f));
  sink.accept = true;
  // a stopped transport has no boundary: at once, and nothing is sent to the sink
  const int defers = sink.defers;
  sink.run = false;
  CHECK(rig.invoke("track.mute", 2) == app::DispatchResult::Changed);
  CHECK(has(rig.sent(), strip(2, gp::strip::Mute), 0.0f));
  CHECK(sink.defers == defers);
  // Solo is never deferred
  sink.run = true;
  CHECK(rig.invoke("track.solo", 3) == app::DispatchResult::Changed);
  CHECK(has(rig.sent(), strip(3, gp::strip::Solo), 1.0f));
  // Pattern, then back to Immediate
  CHECK(rig.invoke("track.mute_mode", 2) == app::DispatchResult::Changed);
  CHECK(board->mute_mode[2] == static_cast<std::uint8_t>(app::MuteMode::Pattern));
  CHECK(rig.invoke("track.mute_mode", 2) == app::DispatchResult::Changed);
  CHECK(board->mute_mode[2] == 0);
  CHECK(rig.invoke("track.mute_mode", 12) != app::DispatchResult::Changed);  // not an active track
  rig.app->attach_mix(nullptr);
}

TEST_CASE("track state: muted, soloed, silenced by another's solo, pending, cue, mode, and the activity watch") {
  static model::ModelArena arena;
  AppRig rig(arena);
  model::Project& p = arena.project;
  model::TrackSlot m = *p.track(1);
  m.flags = model::kTrackMute;
  (void)p.set_track(1, m);
  model::TrackSlot s = *p.track(3);
  s.flags = model::kTrackSolo;
  (void)p.set_track(3, s);
  FakeSink sink;
  sink.pending = 1u << 5;
  auto board = std::make_unique<app::MixBoard>();
  board->tracks = &sink;
  board->route[5] = audio::route::kMain | audio::route::kCue;
  board->mute_mode[5] = static_cast<std::uint8_t>(app::MuteMode::Pattern);
  auto model = std::make_unique<app::Model>();
  model->project = &p;
  model->mix = board.get();
  const app::TrackLamp l1 = app::track_lamp(*model, 1);
  CHECK(l1.muted);
  CHECK_FALSE(l1.soloed);
  CHECK(l1.solo_silenced);  // track 3 is soloed
  const app::TrackLamp l3 = app::track_lamp(*model, 3);
  CHECK(l3.soloed);
  CHECK_FALSE(l3.solo_silenced);
  const app::TrackLamp l5 = app::track_lamp(*model, 5);
  CHECK(l5.mute_pending);
  CHECK(l5.cue);
  CHECK(l5.mode == app::MuteMode::Pattern);
  CHECK_FALSE(l5.muted);
  CHECK_FALSE(app::track_lamp(*model, 0).cue);
  CHECK_FALSE(app::track_lamp(*model, 0).mute_pending);
  CHECK_FALSE(app::track_lamp(*model, 15).solo_silenced);  // not an active track
  // the activity watch: the first poll only takes the counters, then a change per track (and the byte wraps)
  app::ActivityWatch watch;
  sink.counts[2] = 7;
  CHECK(watch.poll(&sink) == 0u);
  CHECK(watch.poll(&sink) == 0u);
  sink.counts[2] = 8;
  sink.counts[9] = 1;
  CHECK(watch.poll(&sink) == ((1u << 2) | (1u << 9)));
  CHECK(watch.poll(&sink) == 0u);
  sink.counts[9] = 0;  // wrapped to the old value elsewhere: still a change
  CHECK(watch.poll(&sink) == (1u << 9));
  CHECK(watch.poll(nullptr) == 0u);
}

TEST_CASE("track state: the sequencer counts the notes a track starts, muted or not, and nothing else") {
  seq::Sequencer& s = play32(16);
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 4, make_step(62, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 2, 2, make_step(40, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 3, 3, make_step(40, 6)) == model::Status::Ok);  // a free slot: no sound, no note
  set_mute(2, true);
  CHECK(s.note_count(0) == 0u);
  g_col.run(s, 0, step_sample(8), 64);
  CHECK(s.note_count(0) == 2u);
  CHECK(s.note_count(1) == 0u);
  CHECK(s.note_count(2) == 1u);  // muted: it fires, the strip is the mute
  CHECK(s.note_count(3) == 0u);
  CHECK(s.note_count(99) == 0u);
}

// OS-MUTEVIS: the Pattern screen's MUTE tag of a track whose quantised mute waits for its boundary blinks; the screen is redrawn at each half period while one waits, when the set that waits changes, and
// never while none does (a still screen draws nothing).
TEST_CASE("mute tag: the Pattern screen redraws at every half period while a quantised mute waits, and only then") {
  static model::ModelArena arena;
  AppRig rig(arena);
  FakeSink sink;
  auto board = std::make_unique<app::MixBoard>();
  board->tracks = &sink;
  rig.app->attach_mix(board.get());
  rig.pass(16);
  rig.pass(16);
  const std::uint32_t idle = rig.app->frames();
  for (int i = 0; i < 40; ++i) {
    rig.pass(100);
  }
  CHECK(rig.app->frames() == idle);  // nothing waits: nothing is drawn
  CHECK_FALSE(rig.app->state().mute_blink);

  sink.pending = 1u << 2;  // a toggle of track 3 waits for its bar
  rig.pass(16);
  const std::uint32_t start = rig.app->frames();
  CHECK(start > idle);  // the set that waits changed: redrawn
  bool lit = rig.app->state().mute_blink;
  unsigned flips = 0;
  for (int i = 0; i < 40; ++i) {  // 4 s: 16 half periods of 250 ms, a flip each
    rig.pass(100);
    if (rig.app->state().mute_blink != lit) {
      lit = rig.app->state().mute_blink;
      ++flips;
    }
  }
  CHECK(flips >= 15);
  CHECK(flips <= 17);
  CHECK(rig.app->frames() >= start + flips);  // every flip was drawn

  sink.pending = 0;  // the boundary came
  rig.pass(16);
  CHECK_FALSE(rig.app->state().mute_blink);
  const std::uint32_t done = rig.app->frames();
  CHECK(done > start);  // the last tag state is drawn
  for (int i = 0; i < 40; ++i) {
    rig.pass(100);
  }
  CHECK(rig.app->frames() == done);  // and the screen is still again

  // On another screen nothing blinks and nothing is redrawn for it.
  (void)rig.invoke("nav.screen", static_cast<core::ActionArg>(app::ScreenId::Song));
  rig.pass(16);
  sink.pending = 1u << 2;
  rig.pass(16);
  const std::uint32_t song = rig.app->frames();
  for (int i = 0; i < 20; ++i) {
    rig.pass(100);
  }
  CHECK(rig.app->frames() == song);
  CHECK_FALSE(rig.app->state().mute_blink);
  rig.app->attach_mix(nullptr);
}
