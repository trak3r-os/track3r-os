// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012a: the module instruments in the audio engine (audio/module_voices.h, the Engine's hooks) and the binder that opens a project with them
// (app/module_binder.h). Native modules written here (a constant "dc" voice that records what it is told, a hostile one) and the host builds of the
// sine and the TB-303 (dlopen). Host only.
#include <doctest/doctest.h>

#define NEED(cond)                                  \
  do {                                              \
    const bool need_ok_ = static_cast<bool>(cond);  \
    CHECK(need_ok_);                                \
    if (!need_ok_) {                                \
      return;                                       \
    }                                               \
  } while (0)

#include "support/static_slot.h"

#include <o3os/app/module_binder.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/taps.h>
#include <o3os/audio/voices.h>
#include <o3os/model/project.h>
#include <o3os/module/loader.h>
#include <o3os/module/view.h>
#include <o3os/platform/host/dir_storage.h>

#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace o3os;
using audio::Engine;
using audio::EventKind;
using audio::ModuleVoices;
using audio::SeqEvent;
using audio::VoicePool;
namespace haudio = o3os::hal::audio;
namespace fs = std::filesystem;

constexpr std::uint32_t kBlk = haudio::kBlock;

// ---- native test modules ------------------------------------------------------------------------------------------------------------
struct alignas(16) DcState {
  float level;
  std::uint32_t gate, last_flags, notes, last_note, last_velocity, params_set, offs;
  float last_param_value;
  std::uint32_t last_param_id;
};
std::uint32_t g_shutdowns = 0;
int g_fail_init = 0;
int g_draw_seen_null = -1;

int dc_init(void* s, const o3m_host* host) {
  if (g_fail_init != 0 || host->size < sizeof(o3m_host)) {
    return 1;
  }
  g_draw_seen_null = host->draw == nullptr ? 1 : (host->draw == o3os::module::draw_api() && host->draw->version == O3M_DRAW_API_VERSION ? 0 : 2);  // ABI 1.1: the OS lends the draw API
  static_cast<DcState*>(s)->level = 0.5f;
  return 0;
}
std::uint32_t dc_render(void* s, const float*, float* out, std::uint32_t n) {
  DcState* d = static_cast<DcState*>(s);
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = d->gate != 0u ? d->level : 0.0f;
  }
  return d->gate != 0u ? 1u : 0u;
}
void dc_set_param(void* s, std::uint32_t id, float v) {
  DcState* d = static_cast<DcState*>(s);
  ++d->params_set;
  d->last_param_id = id;
  d->last_param_value = v;
  if (id == 0u) {
    d->level = v;
  }
}
void dc_note_on(void* s, std::uint32_t note, std::uint32_t vel, std::uint32_t flags) {
  DcState* d = static_cast<DcState*>(s);
  d->gate = 1;
  d->last_note = note;
  d->last_velocity = vel;
  d->last_flags = flags;
  ++d->notes;
}
void dc_note_off(void* s, std::uint32_t) {
  DcState* d = static_cast<DcState*>(s);
  d->gate = 0;
  ++d->offs;
}
void dc_shutdown(void*) { ++g_shutdowns; }

const o3m_param kDcParams[2] = {{"Level", -4.0f, 4.0f, 0.5f, 0}, {"Other", 0.0f, 1.0f, 0.0f, 0}};
const o3m_module kDc = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "dc", sizeof(DcState), 2, kDcParams,
                        dc_init, dc_render, dc_set_param, dc_note_on, dc_note_off, dc_shutdown, nullptr, 0};

// A module that renders what it is told to: NaN, or a huge level.
float g_hostile_value = 0.0f;
std::uint32_t hostile_render(void*, const float*, float* out, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = g_hostile_value;
  }
  return 1;
}
const o3m_module kHostile = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "hostile", sizeof(DcState), 0, nullptr,
                             dc_init, hostile_render, dc_set_param, dc_note_on, dc_note_off, dc_shutdown, nullptr, 0};

// ---- the engine rig -----------------------------------------------------------------------------------------------------------------
struct Cue {
  std::uint32_t block;
  SeqEvent ev;
};
constexpr std::uint32_t kMaxCues = 16;
Cue g_cues[kMaxCues];
std::uint32_t g_cue_count = 0;

SeqEvent event(std::uint16_t offset, EventKind kind, std::uint8_t track, std::uint8_t note = 0, std::uint8_t slot = 0, std::uint8_t vel = 0) {
  SeqEvent e{};
  e.offset = offset;
  e.kind = kind;
  e.track = track;
  e.a = note;
  e.b = slot;
  e.c = vel;
  return e;
}
void cue(std::uint32_t block, const SeqEvent& e) { g_cues[g_cue_count++] = Cue{block, e}; }

std::uint32_t scripted_step(void*, std::uint64_t block_start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
  const std::uint32_t block = static_cast<std::uint32_t>(block_start / n);
  std::uint32_t count = 0;
  for (std::uint32_t i = 0; i < g_cue_count && count < cap; ++i) {
    if (g_cues[i].block == block) {
      out[count++] = g_cues[i].ev;
    }
  }
  return count;
}
std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 7u; }

struct Rig {
  alignas(16) std::uint8_t scratch[ModuleVoices::kScratchBytes];
  alignas(16) DcState dc[4];
  alignas(16) unsigned char tb_state[1024];
  Engine* engine = nullptr;
  VoicePool* pool = nullptr;
  ModuleVoices* mv = nullptr;
  haudio::BlockReport report{};
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};

  Rig() {
    static o3os::test::StaticSlot<Engine> engine_slot;
    static o3os::test::StaticSlot<VoicePool> pool_slot;
    static o3os::test::StaticSlot<ModuleVoices> mv_slot;
    static audio::TapMem taps;
    static SeqEvent events[audio::kMaxEventsPerBlock];
    engine = &engine_slot.make(&fake_cycles, taps);
    pool = &pool_slot.make();
    mv = &mv_slot.make();
    std::memset(scratch, 0, sizeof scratch);
    std::memset(dc, 0, sizeof dc);
    mv->attach_scratch(scratch, sizeof scratch);
    engine->attach_voices(pool);
    engine->attach_modules(mv);
    audio::EventSource src;
    src.step = &scripted_step;
    src.buffer = events;
    src.capacity = audio::kMaxEventsPerBlock;
    engine->attach_events(src);
    g_cue_count = 0;
  }
  void block() {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine->render_block(io);
  }
  std::vector<std::int16_t> feed(unsigned track) const {
    const std::int16_t* f = mv->feed(track);
    return f != nullptr ? std::vector<std::int16_t>(f, f + kBlk) : std::vector<std::int16_t>();
  }
};

}  // namespace

TEST_CASE("module voices: a note starts at its exact sample, ends at its release, and the block is silent before it") {
  Rig r;
  NEED(r.mv->bind(3, kDc, 0x8001, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  CHECK(g_draw_seen_null == 0);  // the draw API (OS-012b) was in the host table at init()
  CHECK(r.mv->bound(3));
  CHECK(!r.mv->bound(4));
  cue(2, event(17, EventKind::NoteOn, 0, 60, 3, 127));  // block 2, sample 17
  cue(3, event(40, EventKind::NoteOff, 0));
  for (int b = 0; b < 2; ++b) {
    r.block();
    CHECK(r.feed(0).empty());  // no note yet: no input on the track
  }
  r.block();  // block 2
  std::vector<std::int16_t> f = r.feed(0);
  NEED(f.size() == kBlk);
  for (std::uint32_t i = 0; i < 17; ++i) {
    CHECK(f[i] == 0);
  }
  for (std::uint32_t i = 17; i < kBlk; ++i) {
    CHECK(f[i] == 16384);  // 0.5 * 32768
  }
  CHECK(r.dc[0].last_note == 60);
  CHECK(r.dc[0].last_velocity == 127);
  CHECK(r.dc[0].last_flags == 0);
  r.block();  // block 3: released at sample 40
  f = r.feed(0);
  NEED(f.size() == kBlk);
  for (std::uint32_t i = 0; i < 40; ++i) {
    CHECK(f[i] == 16384);
  }
  for (std::uint32_t i = 40; i < kBlk; ++i) {
    CHECK(f[i] == 0);
  }
  CHECK(r.dc[0].offs == 1);
  r.block();  // block 4: the module said it is idle: not called, no feed
  CHECK(r.feed(0).empty());
  // the graph made it audible from the same sample
  CHECK(r.out[2 * 63] == 0);
}

TEST_CASE("module voices: a module note cuts the sampler voice of its track, and a sample note on the track ends the module's") {
  Rig r;
  static std::int16_t dcpcm[2000];
  for (std::int16_t& s : dcpcm) {
    s = 8000;
  }
  r.pool->set_sample(0, audio::SampleDesc{dcpcm, 2000, 0, 0, 65536});
  NEED(r.mv->bind(5, kDc, 1, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  cue(0, event(0, EventKind::NoteOn, 0, 60, 0, 127));   // a sample on track 0
  cue(2, event(10, EventKind::NoteOn, 0, 62, 5, 100));  // the module takes the track
  cue(5, event(10, EventKind::NoteOn, 0, 60, 0, 127));  // a sample again
  r.block();
  CHECK(r.pool->track_playing(0));
  CHECK(r.feed(0).empty());
  r.block();
  r.block();  // block 2: the module's note; the sample voice is cut (it fades over the block: no click) and the module's block is the track's
  CHECK(r.mv->feed(0) != nullptr);
  CHECK(r.dc[0].gate == 1);
  r.block();
  r.block();
  CHECK(!r.pool->track_playing(0));  // the cut voice is gone
  r.block();  // block 5: a sample note again
  CHECK(r.dc[0].gate == 0);          // the module's note ended
  CHECK(r.mv->feed(0) == nullptr);
  CHECK(r.pool->track_playing(0));
}

// OS-005g review: the live notes (pads, MIDI keyboard) go through the same decision as the sequencer's note events (Engine::dispatch).
TEST_CASE("module voices: a closed gate (OS-INSTUI S5: the slot's instrument was deleted or replaced) keeps the bound instance from ever sounding; opening it brings the module back") {
  Rig r;
  static std::int16_t pcm[2000];
  for (std::int16_t& smp : pcm) {
    smp = 8000;
  }
  r.pool->set_sample(3, audio::SampleDesc{pcm, 2000, 0, 0, 65536});  // the sample instrument that now lives in slot 3
  NEED(r.mv->bind(3, kDc, 0x8001, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  CHECK(r.mv->routed(3));
  r.mv->set_gate(3, false);  // what ModuleUi::sync_slots does while the slot's instrument is not a Module
  CHECK(r.mv->bound(3));      // (there is no unbind: the instance stays)
  CHECK(!r.mv->routed(3));
  CHECK(!r.mv->note_on(0, 60, 3, 100));    // the render's entry
  CHECK(!r.mv->audition(3, 1, 60, 100, true));  // the pads' and the preview's
  cue(0, event(0, EventKind::NoteOn, 0, 60, 3, 127));  // the sequencer plays slot 3: the sample, not the module
  cue(3, event(0, EventKind::NoteOn, 1, 62, 3, 127));
  for (int b = 0; b < 3; ++b) {
    r.block();
  }
  CHECK(r.pool->track_playing(0));
  CHECK(r.dc[0].notes == 0);
  CHECK(r.dc[0].gate == 0);
  CHECK(r.feed(0).empty());
  r.mv->set_gate(3, true);  // an undo of the delete
  CHECK(r.mv->routed(3));
  r.block();  // block 3: the note of track 1
  CHECK(r.dc[0].notes == 1);
  CHECK(r.dc[0].last_note == 62);
  r.mv->set_gate(3, false);  // closed again (a redo): a note that sounds is not cut, but no new one starts
  g_cue_count = 0;
  cue(5, event(0, EventKind::NoteOn, 2, 64, 3, 127));
  r.block();
  r.block();
  CHECK(r.dc[0].notes == 1);
  // a fresh bind opens the gate
  r.mv->retire_all();
  r.mv->release_all();
  NEED(r.mv->bind(3, kDc, 0x8001, &r.dc[1], nullptr, 0) == ModuleVoices::Bind::Ok);
  CHECK(r.mv->routed(3));
}

TEST_CASE("module voices: closing the gate ends the notes the instance is playing and drops a queued audition") {
  Rig r;
  NEED(r.mv->bind(3, kDc, 0x8001, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  cue(0, event(0, EventKind::NoteOn, 0, 60, 3, 127));  // a held note on track 0 (no NoteOff is ever sent)
  r.block();
  r.block();
  CHECK(r.dc[0].gate == 1);
  CHECK(!r.feed(0).empty());
  r.mv->set_gate(3, false);  // the slot's instrument was deleted
  CHECK(r.dc[0].gate == 1);   // the render serves the release at its next block start
  r.block();
  CHECK(r.dc[0].gate == 0);   // the note ended
  CHECK(r.dc[0].offs == 1);
  CHECK(r.feed(0).empty());   // and the track is free: no block of the module is its input
  r.block();
  CHECK(r.dc[0].gate == 0);
  // an audition queued before the gate closed is dropped by it (audition_note is behind the gate too)
  r.mv->set_gate(3, true);
  const std::uint32_t notes = r.dc[0].notes;
  CHECK(r.mv->audition(3, 1, 64, 100, true));
  r.mv->set_gate(3, false);
  r.block();
  CHECK(r.dc[0].notes == notes);
  CHECK(r.dc[0].gate == 0);
  // closing a closed gate and opening an open one queue nothing
  r.mv->set_gate(3, false);
  r.mv->set_gate(3, true);
  r.mv->set_gate(3, true);
  r.block();
  CHECK(r.dc[0].offs == 1);
}

TEST_CASE("module voices: a live note on a module's instrument plays the module, its release ends it, and a sample note on a module track takes the track back") {
  Rig r;
  static std::int16_t dcpcm[4000];
  for (std::int16_t& s : dcpcm) {
    s = 8000;
  }
  static audio::LiveBox box;
  box.notes = audio::LiveNotes{};
  while (box.queue.size() != 0u) {  // (a previous test may have left events)
    audio::LiveEvent drop;
    (void)box.queue.pop(drop);
  }
  r.engine->attach_live(&box);
  r.pool->set_sample(0, audio::SampleDesc{dcpcm, 4000, 0, 0, 65536});  // instrument 1: a sample
  NEED(r.mv->bind(5, kDc, 1, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);  // instrument 6: the module
  const auto push_on = [&](std::uint8_t key, std::uint8_t instrument, std::uint8_t track) {
    return box.queue.push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, key, 60, instrument, audio::kLiveVelocityDefault, track, 0});
  };
  // A pad on the module's instrument: the module sounds on the selected track (the note is not dropped), the sampler's voice is not started.
  CHECK(push_on(1, 6, 3));
  r.block();
  CHECK(r.dc[0].gate == 1);
  CHECK(r.dc[0].notes == 1);
  CHECK(r.mv->feed(3) != nullptr);
  CHECK(!r.pool->track_playing(3));
  CHECK(r.engine->live_held() == 1);
  // A sample pad that asks for the same track gets the next one: a module's note holds its track like a voice, and the sampler's block there would not be heard.
  CHECK(push_on(2, 1, 3));
  r.block();
  CHECK(r.pool->track_playing(4));
  CHECK(!r.pool->track_playing(3));
  CHECK(r.mv->feed(4) == nullptr);
  CHECK(r.mv->feed(3) != nullptr);
  // The release of the module's pad ends the module's note.
  CHECK(box.queue.push(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePad, 1, 0, 0, 0, 0, 0}));
  r.block();
  CHECK(r.dc[0].gate == 0);
  CHECK(r.dc[0].offs == 1);
  // An all-notes-off ends it too (the module again, then the AllOff).
  CHECK(push_on(1, 6, 3));
  r.block();
  CHECK(r.dc[0].gate == 1);
  CHECK(box.queue.push(audio::LiveEvent{audio::LiveKind::AllOff, audio::kLiveAllSources, 0, 0, 0, 0, 0, 0}));
  r.block();
  CHECK(r.dc[0].gate == 0);
  CHECK(r.dc[0].offs == 2);
}

TEST_CASE("module voices: a held live key that is struck again with a sample takes its track back from a module the sequencer put there") {
  Rig r;
  static std::int16_t dcpcm[4000];
  for (std::int16_t& s : dcpcm) {
    s = 8000;
  }
  static audio::LiveBox box;
  box.notes = audio::LiveNotes{};
  while (box.queue.size() != 0u) {
    audio::LiveEvent drop;
    (void)box.queue.pop(drop);
  }
  r.engine->attach_live(&box);
  r.pool->set_sample(0, audio::SampleDesc{dcpcm, 4000, 0, 0, 65536});
  NEED(r.mv->bind(5, kDc, 1, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  const audio::LiveEvent strike{audio::LiveKind::On, audio::kLiveSourcePad, 7, 60, 1, audio::kLiveVelocityDefault, 2, 0};
  CHECK(box.queue.push(strike));
  r.block();  // block 0: the live key holds track 2 with a sample
  CHECK(r.pool->track_playing(2));
  cue(1, event(0, EventKind::NoteOn, 2, 62, 5, 100));  // the sequencer puts the module there
  r.block();
  CHECK(r.dc[0].gate == 1);
  CHECK(r.mv->feed(2) != nullptr);
  CHECK(box.queue.push(strike));  // the same key again: its own track, a sample note: the module's note ends
  r.block();
  CHECK(r.dc[0].gate == 0);
  CHECK(r.mv->feed(2) == nullptr);
  CHECK(r.pool->track_playing(2));
}

TEST_CASE("module voices: a sequencer note releases the held one first (never legato), another track taking the instrument, and two instruments on two tracks") {
  Rig r;
  NEED(r.mv->bind(1, kDc, 1, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  NEED(r.mv->bind(2, kDc, 2, &r.dc[1], nullptr, 0) == ModuleVoices::Bind::Ok);
  cue(0, event(0, EventKind::NoteOn, 0, 60, 1, 64));
  cue(1, event(0, EventKind::NoteOn, 0, 62, 1, 64));   // while the first is held (no note-off came): a new note, the host releases the first one before it (OS-MODSILENT)
  cue(2, event(0, EventKind::NoteOff, 0));
  cue(3, event(0, EventKind::NoteOn, 0, 64, 1, 64));   // after a release: not legato
  cue(4, event(0, EventKind::NoteOn, 1, 66, 2, 90));   // the other instrument, track 1: both sound
  cue(5, event(0, EventKind::NoteOn, 2, 67, 1, 90));   // track 2 plays the first instrument again: track 0 loses it
  r.block();
  CHECK(r.dc[0].last_flags == 0);
  CHECK(r.dc[0].offs == 0);
  r.block();
  CHECK(r.dc[0].last_flags == 0);  // not legato: a pattern's note retriggers
  CHECK(r.dc[0].offs == 1);        // the first note was released by the host
  CHECK(r.dc[0].last_note == 62);
  r.block();
  CHECK(r.dc[0].gate == 0);
  CHECK(r.dc[0].offs == 2);        // the pattern's own note-off released the second note
  r.block();
  CHECK(r.dc[0].last_flags == 0);
  r.block();
  CHECK(r.mv->feed(0) != nullptr);
  CHECK(r.mv->feed(1) != nullptr);
  CHECK(r.dc[1].last_note == 66);
  r.block();
  CHECK(r.mv->feed(0) == nullptr);  // taken over
  CHECK(r.mv->feed(2) != nullptr);
  CHECK(r.dc[0].last_flags == 0);  // track 0's note was still held: released first, the new one is not legato
  CHECK(r.dc[0].offs == 3);
}

// OS-MODSILENT review: legato is a live key (or the audition) over a held LIVE key, by hand on both sides. A sequencer's note never has a note-off of its own, so a key struck over it is a new note.
TEST_CASE("module voices: a live key over a held live key is legato; over a sequencer's note, after a release, or as the first key it is not") {
  Rig r;
  NEED(r.mv->bind(1, kDc, 1, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  r.mv->begin_block();
  CHECK(r.mv->note_on(0, 60, 1, 64, true));  // the first live key: a note
  CHECK(r.dc[0].last_flags == 0);
  CHECK(r.dc[0].offs == 0);
  CHECK(r.mv->note_on(1, 62, 1, 64, true));  // a second live key, the first still down: legato, no note-off between
  CHECK(r.dc[0].last_flags == O3M_NOTE_LEGATO);
  CHECK(r.dc[0].offs == 0);
  r.mv->note_off(1);                          // released
  CHECK(r.dc[0].offs == 1);
  CHECK(r.mv->note_on(2, 64, 1, 64, true));  // after a release: a note
  CHECK(r.dc[0].last_flags == 0);
  CHECK(r.mv->note_on(3, 65, 1, 64, false));  // a sequencer's note over the live key: released first, a new note
  CHECK(r.dc[0].last_flags == 0);
  CHECK(r.dc[0].offs == 2);
  CHECK(r.mv->note_on(4, 67, 1, 64, true));  // a live key over the sequencer's note: NOT legato (nobody played it), released first
  CHECK(r.dc[0].last_flags == 0);
  CHECK(r.dc[0].offs == 3);
  CHECK(r.mv->note_on(5, 69, 1, 64, true));  // and a live key over THAT live key: legato again
  CHECK(r.dc[0].last_flags == O3M_NOTE_LEGATO);
  CHECK(r.dc[0].offs == 3);
  r.mv->end_block();
}

TEST_CASE("module voices: the audition twice, no release between, is legato (a played key)") {
  Rig r;
  NEED(r.mv->bind(1, kDc, 1, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  CHECK(r.mv->audition(1, 15, 60, 100, true));
  r.block();
  CHECK(r.dc[0].last_flags == 0);
  CHECK(r.mv->audition(1, 15, 62, 100, true));
  r.block();
  CHECK(r.dc[0].last_flags == O3M_NOTE_LEGATO);
  CHECK(r.dc[0].offs == 0);
  CHECK(r.mv->audition(1, 15, 0, 0, false));  // off
  r.block();
  CHECK(r.dc[0].offs == 1);
  CHECK(r.mv->audition(1, 15, 64, 100, true));
  r.block();
  CHECK(r.dc[0].last_flags == 0);  // after the release: a note
}

TEST_CASE("module voices: bind, retire and release, the grace period, and every refusal") {
  Rig r;
  g_shutdowns = 0;
  alignas(16) static DcState extra[audio::kMaxModuleInstances];
  CHECK(r.mv->bind(64, kDc, 1, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::BadSlot);
  CHECK(r.mv->bind(0, kDc, 1, nullptr, nullptr, 0) == ModuleVoices::Bind::BadState);
  CHECK(r.mv->bind(0, kDc, 1, reinterpret_cast<char*>(&r.dc[0]) + 4, nullptr, 0) == ModuleVoices::Bind::BadState);
  g_fail_init = 1;
  CHECK(r.mv->bind(0, kDc, 1, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::InitFailed);
  g_fail_init = 0;
  CHECK(!r.mv->bound(0));
  CHECK(r.mv->instances() == 0);
  NEED(r.mv->bind(0, kDc, 1, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  CHECK(r.mv->bind(0, kDc, 1, &r.dc[1], nullptr, 0) == ModuleVoices::Bind::SlotTaken);
  for (unsigned i = 1; i < audio::kMaxModuleInstances; ++i) {
    CHECK(r.mv->bind(i, kDc, 1, &extra[i], nullptr, 0) == ModuleVoices::Bind::Ok);
  }
  CHECK(r.mv->instances() == audio::kMaxModuleInstances);
  CHECK(r.mv->bind(20, kDc, 1, &r.dc[2], nullptr, 0) == ModuleVoices::Bind::NoInstance);
  ModuleVoices fresh;
  CHECK(fresh.bind(0, kDc, 1, &r.dc[3], nullptr, 0) == ModuleVoices::Bind::NoScratch);
  // retire: unbound at once, the render clear after two block starts, then shutdown and the room is free again
  r.mv->retire_all();
  CHECK(!r.mv->bound(0));
  CHECK(!r.mv->quiescent());
  r.block();
  CHECK(!r.mv->quiescent());
  r.block();
  CHECK(r.mv->quiescent());
  CHECK(g_shutdowns == 0);
  r.mv->release_all();
  CHECK(g_shutdowns == audio::kMaxModuleInstances);
  CHECK(r.mv->instances() == 0);
  std::memset(&r.dc[0], 0, sizeof r.dc[0]);
  CHECK(r.mv->bind(0, kDc, 1, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  // nothing bound: quiescent at once
  ModuleVoices& mv = *r.mv;
  mv.retire_all();
  mv.release_all();
  CHECK(mv.quiescent());
  mv.retire_all();  // twice is harmless
  CHECK(mv.quiescent());
}

TEST_CASE("module voices: the parameter blob is applied at bind, commands at the next block start, auditions are notes") {
  Rig r;
  float blob[3] = {2.0f, std::nanf(""), 7.0f};  // the third is past the module's two parameters; the NaN is skipped
  NEED(r.mv->bind(0, kDc, 1, &r.dc[0], reinterpret_cast<const std::uint8_t*>(blob), sizeof blob) == ModuleVoices::Bind::Ok);
  CHECK(r.dc[0].params_set == 1);
  CHECK(r.dc[0].level == 2.0f);
  NEED(r.mv->bind(1, kDc, 1, &r.dc[1], reinterpret_cast<const std::uint8_t*>(blob), 6) == ModuleVoices::Bind::Ok);  // 6 bytes: one whole float
  CHECK(r.dc[1].level == 2.0f);
  CHECK(r.dc[1].params_set == 1);
  CHECK(r.mv->set_param(0, 0, -1.0f));
  CHECK(r.dc[0].level == 2.0f);  // not yet: the render applies it
  r.block();
  CHECK(r.dc[0].level == -1.0f);
  CHECK(r.dc[0].params_set == 2);
  CHECK(!r.mv->set_param(9, 0, 1.0f));  // not bound
  // the ring is bounded
  unsigned accepted = 0;
  for (unsigned i = 0; i < 200; ++i) {
    accepted += r.mv->set_param(0, 1, static_cast<float>(i)) ? 1u : 0u;
  }
  CHECK(accepted == ModuleVoices::kCommandRing);
  CHECK(r.mv->stats().dropped == 200 - ModuleVoices::kCommandRing);
  r.block();  // at most kMaxCommandsPerBlock a block
  CHECK(r.dc[0].last_param_value == 31.0f);
  r.block();
  r.block();
  CHECK(r.dc[0].last_param_value == 63.0f);
  // audition
  CHECK(r.mv->audition(0, 4, 48, 100, true));
  CHECK(!r.mv->audition(0, 16, 48, 100, true));
  CHECK(!r.mv->audition(0, 4, 128, 100, true));
  r.block();
  CHECK(r.dc[0].gate == 1);
  CHECK(r.dc[0].last_note == 48);
  CHECK(r.mv->feed(4) != nullptr);
  CHECK(r.mv->audition(0, 4, 48, 0, false));
  r.block();
  CHECK(r.dc[0].gate == 0);
}

namespace {
std::uint16_t g_enter_log[64];
unsigned g_enter_count = 0;
std::uint16_t g_stamped = 0x0042;  // what the platform's crash context held before: the main loop's own driver
std::uint16_t enter_hook(std::uint16_t id) {
  if (g_enter_count < 64) {
    g_enter_log[g_enter_count++] = id;
  }
  const std::uint16_t before = g_stamped;
  g_stamped = id;
  return before;
}
}  // namespace

TEST_CASE("module voices: every call into a module is bracketed by the crash-context hook with its id") {
  Rig r;
  g_enter_count = 0;
  ModuleVoices::Hooks h;
  h.enter = &enter_hook;
  r.mv->set_hooks(h);
  NEED(r.mv->bind(0, kDc, 0x8123, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  CHECK(g_enter_count == 2);  // init: enter(id), leave
  CHECK(g_enter_log[0] == 0x8123);
  CHECK(g_enter_log[1] == 0x0042);  // the previous stamp is put back, not zero
  CHECK(g_stamped == 0x0042);
  g_enter_count = 0;
  cue(0, event(0, EventKind::NoteOn, 0, 60, 0, 100));
  r.block();  // note_on, render
  CHECK(g_enter_count == 4);
  for (unsigned i = 0; i < g_enter_count; ++i) {
    CHECK(g_enter_log[i] == ((i % 2) == 0 ? 0x8123 : 0x0042));
  }
  CHECK(g_stamped == 0x0042);
}

TEST_CASE("module voices: NaN is silence, a huge level saturates and is counted") {
  Rig r;
  NEED(r.mv->bind(0, kHostile, 1, &r.dc[0], nullptr, 0) == ModuleVoices::Bind::Ok);
  cue(0, event(0, EventKind::NoteOn, 0, 60, 0, 100));
  g_hostile_value = std::nanf("");
  r.block();
  std::vector<std::int16_t> f = r.feed(0);
  NEED(f.size() == kBlk);
  for (std::int16_t s : f) {
    CHECK(s == 0);
  }
  g_hostile_value = 1.0e9f;
  r.block();
  f = r.feed(0);
  for (std::int16_t s : f) {
    CHECK(s == 32767);
  }
  g_hostile_value = -1.0e9f;
  r.block();
  f = r.feed(0);
  for (std::int16_t s : f) {
    CHECK(s == -32768);
  }
  CHECK(r.mv->stats().saturated == 2 * kBlk);
  g_hostile_value = 0.0f;
}

namespace {

struct So {
  void* handle = nullptr;
  const o3m_module* entry = nullptr;
  explicit So(const char* path) {
    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    entry = handle != nullptr ? static_cast<const o3m_module*>(dlsym(handle, "o3m_entry")) : nullptr;
  }
  ~So() {
    if (handle != nullptr) {
      dlclose(handle);
    }
  }
};

}  // namespace

TEST_CASE("module voices: the TB-303 and the sine, as host shared objects, through the engine") {
  Rig r;
  So tb(O3M_TB303_HOST_SO), sine(O3M_SINE_HOST_SO);
  NEED(tb.entry != nullptr);
  NEED(sine.entry != nullptr);
  NEED(sizeof(r.tb_state) >= tb.entry->state_bytes);
  alignas(16) static unsigned char sine_state[256];
  std::memset(r.tb_state, 0, sizeof r.tb_state);
  std::memset(sine_state, 0, sizeof sine_state);
  r.mv->set_sample_rate(44117.647f);
  float blob[9] = {0.0f, 600.0f, 70.0f, 60.0f, 400.0f, 80.0f, 60.0f, -6.0f, 1.0f};
  NEED(r.mv->bind(10, *tb.entry, 7, r.tb_state, reinterpret_cast<const std::uint8_t*>(blob), sizeof blob) == ModuleVoices::Bind::Ok);
  NEED(r.mv->bind(11, *sine.entry, 8, sine_state, nullptr, 0) == ModuleVoices::Bind::Ok);
  cue(0, event(0, EventKind::NoteOn, 0, 36, 10, 127));
  cue(0, event(0, EventKind::NoteOn, 1, 69, 11, 100));
  cue(60, event(0, EventKind::NoteOff, 0));
  cue(60, event(0, EventKind::NoteOff, 1));
  double peak0 = 0, peak1 = 0;
  std::uint32_t late_zero_blocks = 0;
  for (int b = 0; b < 900; ++b) {
    r.block();
    const std::vector<std::int16_t> a = r.feed(0), s = r.feed(1);
    for (std::int16_t v : a) {
      peak0 = std::fmax(peak0, std::fabs(static_cast<double>(v)));
    }
    for (std::int16_t v : s) {
      peak1 = std::fmax(peak1, std::fabs(static_cast<double>(v)));
    }
    late_zero_blocks += (b > 800 && a.empty() && s.empty()) ? 1u : 0u;
  }
  CHECK(peak0 > 1000.0);   // the 303 sounds
  CHECK(peak1 > 8000.0);   // the sine at its default level
  CHECK(late_zero_blocks > 90);  // both ended after their notes (the accented 303 rings out in about 0.7 s): the instances went idle and stopped costing anything
  r.mv->retire_all();
  r.block();
  r.block();
  CHECK(r.mv->quiescent());
  r.mv->release_all();
}

namespace {

// A held note on the FM module for `blocks` blocks of the track's feed; with `change`, Op1's Ratio (id 0) and Level (id 2) are queued after the first 40 blocks (applied at the next block start) and
// two blocks of settling are dropped. Returns the samples of the window that follows, the same blocks of the same note with and without the change.
std::vector<double> fm_window(bool change, unsigned blocks) {
  Rig r;
  So fm(O3M_FM_HOST_SO);
  std::vector<double> win;
  if (fm.entry == nullptr || sizeof(r.tb_state) < fm.entry->state_bytes) {
    FAIL("the FM host module did not load or its state does not fit");
    return win;
  }
  std::memset(r.tb_state, 0, sizeof r.tb_state);
  r.mv->set_sample_rate(44100.0f);
  if (r.mv->bind(10, *fm.entry, 9, r.tb_state, nullptr, 0) != ModuleVoices::Bind::Ok) {
    FAIL("the FM module did not bind");
    return win;
  }
  cue(0, event(0, EventKind::NoteOn, 0, 57, 10, 100));
  for (unsigned b = 0; b < 40u + 2u + blocks; ++b) {
    if (b == 40u && change) {
      CHECK(r.mv->set_param(10, 0, 3.0f));   // Op1 Ratio 1 -> 3
      CHECK(r.mv->set_param(10, 2, 20.0f));  // Op1 Level 100 -> 20
    }
    r.block();
    if (b >= 42u) {
      for (std::int16_t v : r.feed(0)) {
        win.push_back(static_cast<double>(v));
      }
    }
  }
  r.mv->retire_all();
  r.block();
  r.block();
  r.mv->release_all();
  return win;
}

}  // namespace

// Review gap: nothing ran a parameter of the FM module through the engine. A held note, Op1's Ratio and Level changed under it: the sound must change (the same note without the change is the control, so the
// envelope's own decay is not mistaken for it).
TEST_CASE("module voices: the FM module's Op1 Ratio and Level, set under a held note through the engine, change its level and its spectrum") {
  constexpr unsigned kWindowBlocks = 16;
  const std::vector<double> ctl = fm_window(false, kWindowBlocks);
  const std::vector<double> chg = fm_window(true, kWindowBlocks);
  NEED(ctl.size() >= 512u);
  NEED(chg.size() == ctl.size());
  const auto rms = [](const std::vector<double>& x) {
    double e = 0.0;
    for (double v : x) {
      e += v * v;
    }
    return std::sqrt(e / static_cast<double>(x.size()));
  };
  // the magnitude spectrum of the first 1024 samples (bins 0..255, a naive DFT: the window is short), as shares of the total, and its centroid in bins
  const auto spectrum = [](const std::vector<double>& x, double& centroid) {
    constexpr std::size_t kN = 1024, kBins = 256;
    std::vector<double> mag(kBins, 0.0);
    double total = 0.0, weighted = 0.0;
    for (std::size_t k = 1; k < kBins; ++k) {
      double re = 0.0, im = 0.0;
      for (std::size_t n = 0; n < kN && n < x.size(); ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(n) / static_cast<double>(kN));  // Hann
        const double a = 2.0 * M_PI * static_cast<double>(k * n % kN) / static_cast<double>(kN);
        re += w * x[n] * std::cos(a);
        im -= w * x[n] * std::sin(a);
      }
      mag[k] = std::sqrt(re * re + im * im);
      total += mag[k];
      weighted += mag[k] * static_cast<double>(k);
    }
    for (double& m : mag) {
      m = total > 0.0 ? m / total : 0.0;
    }
    centroid = total > 0.0 ? weighted / total : 0.0;
    return mag;
  };
  const double r_ctl = rms(ctl), r_chg = rms(chg);
  double c_ctl = 0.0, c_chg = 0.0;
  const std::vector<double> s_ctl = spectrum(ctl, c_ctl), s_chg = spectrum(chg, c_chg);
  double l1 = 0.0;
  for (std::size_t k = 0; k < s_ctl.size(); ++k) {
    l1 += std::fabs(s_ctl[k] - s_chg[k]);
  }
  std::printf("[fm params] rms control %.1f changed %.1f; centroid control %.2f changed %.2f bins; spectrum L1 distance %.3f\n", r_ctl, r_chg, c_ctl, c_chg, l1);
  CHECK(r_ctl > 500.0);                       // the control sounds
  CHECK(r_chg > 100.0);                       // and so does the changed voice (Level 20 is quiet, not silent)
  CHECK(std::fabs(r_chg - r_ctl) > 0.1 * r_ctl);  // the level changed by more than a tenth
  CHECK(l1 > 0.03);                           // and the shape of the spectrum moved (measured 0.073; bins of 43 Hz: the note is at bin 5): Ratio 3 puts Op1's partial at three times the note
  CHECK(c_chg != doctest::Approx(c_ctl).epsilon(0.02));
}

// OS-SNDMOD S3 (QA-MOD-144): a project saved before the MODS parameters stores nine values (ids 0..8, 36 bytes). bind() applies the short blob to the first
// nine and leaves ids 9..13 as init() set them, which are the descriptor's defaults: the voice and its audio equal those of a voice given the full defaults.
TEST_CASE("module voices: a pre-S3 nine-value TB-303 blob binds like the same values with the five new parameters at their defaults") {
  Rig r;
  So tb(O3M_TB303_HOST_SO);
  NEED(tb.entry != nullptr);
  const o3m_module& mod = *tb.entry;
  constexpr unsigned kOld = 9;  // ids 0..8 (through Legato Slide): the blob of a project saved before S3
  NEED(mod.param_count == 14);
  NEED(sizeof(r.tb_state) >= mod.state_bytes);
  CHECK(mod.params[9].def == 0.0f);     // Tune, Transpose, Acc Thresh, Filter Track, Acc Decay: the values QA-MOD-144 reads on the page
  CHECK(mod.params[10].def == 0.0f);
  CHECK(mod.params[11].def == 100.0f);
  CHECK(mod.params[12].def == 0.0f);
  CHECK(mod.params[13].def == 200.0f);
  float defaults[14];
  for (unsigned i = 0; i < 14; ++i) {
    defaults[i] = mod.params[i].def;
  }
  const float sound[kOld] = {1.0f, 600.0f, 70.0f, 60.0f, 400.0f, 80.0f, 60.0f, -6.0f, 0.0f};  // not the defaults: every one of the nine moves
  float full[14];
  std::memcpy(full, sound, sizeof sound);
  std::memcpy(full + kOld, defaults + kOld, (14 - kOld) * sizeof(float));
  static_assert(sizeof(sound) == 36, "the pre-S3 blob is nine floats");

  alignas(16) static unsigned char state[4][1024];
  std::memset(state, 0, sizeof state);
  r.mv->set_sample_rate(44117.647f);
  const auto bytes = [](const float* f) { return reinterpret_cast<const std::uint8_t*>(f); };
  // 0: nine defaults vs 1: no blob at all; 2: nine moved values vs 3: the same nine and the five defaults.
  NEED(r.mv->bind(10, mod, 7, state[0], bytes(defaults), kOld * sizeof(float)) == ModuleVoices::Bind::Ok);
  NEED(r.mv->bind(11, mod, 8, state[1], nullptr, 0) == ModuleVoices::Bind::Ok);
  NEED(r.mv->bind(12, mod, 9, state[2], bytes(sound), sizeof sound) == ModuleVoices::Bind::Ok);
  NEED(r.mv->bind(13, mod, 10, state[3], bytes(full), sizeof full) == ModuleVoices::Bind::Ok);
  CHECK(std::memcmp(state[0], state[1], mod.state_bytes) == 0);
  CHECK(std::memcmp(state[2], state[3], mod.state_bytes) == 0);
  CHECK(std::memcmp(state[0], state[2], mod.state_bytes) != 0);  // the comparison can fail: the moved nine are in the voice

  for (unsigned t = 0; t < 4; ++t) {
    cue(0, event(0, EventKind::NoteOn, static_cast<std::uint8_t>(t), 36, static_cast<std::uint8_t>(10 + t), 127));  // an accent
    cue(20, event(0, EventKind::NoteOn, static_cast<std::uint8_t>(t), 48, static_cast<std::uint8_t>(10 + t), 100));  // then a plain note
    cue(50, event(0, EventKind::NoteOff, static_cast<std::uint8_t>(t)));
  }
  double peak[4] = {0, 0, 0, 0};
  bool same01 = true, same23 = true, differ02 = false;
  for (int b = 0; b < 120; ++b) {
    r.block();
    const std::vector<std::int16_t> f0 = r.feed(0), f1 = r.feed(1), f2 = r.feed(2), f3 = r.feed(3);
    same01 = same01 && f0 == f1;
    same23 = same23 && f2 == f3;
    differ02 = differ02 || f0 != f2;
    const std::vector<std::int16_t>* f[4] = {&f0, &f1, &f2, &f3};
    for (unsigned t = 0; t < 4; ++t) {
      for (std::int16_t v : *f[t]) {
        peak[t] = std::fmax(peak[t], std::fabs(static_cast<double>(v)));
      }
    }
  }
  CHECK(peak[0] > 1000.0);  // the voices sound, so the equalities below are not two silences
  CHECK(peak[2] > 1000.0);
  CHECK(same01);
  CHECK(same23);
  CHECK(differ02);
  CHECK(std::memcmp(state[0], state[1], mod.state_bytes) == 0);  // and after rendering, the whole voice is still bit-equal
  CHECK(std::memcmp(state[2], state[3], mod.state_bytes) == 0);
  r.mv->retire_all();
  r.block();
  r.block();
  CHECK(r.mv->quiescent());
  r.mv->release_all();
}

// ---- the binder ---------------------------------------------------------------------------------------------------------------------

namespace {

model::Instrument module_instrument(unsigned module_index, const float* params, unsigned count) {
  model::Instrument in;
  std::memset(&in, 0, sizeof in);
  model::init_instrument(in, model::InstrumentKind::Module);
  in.module_index = static_cast<std::uint8_t>(module_index);
  if (count != 0) {
    std::memcpy(in.params, params, count * sizeof(float));
  }
  in.param_len = static_cast<std::uint16_t>(count * sizeof(float));
  return in;
}

}  // namespace

TEST_CASE("module binder: a project's module instruments are bound, the ones whose module cannot be had are muted and say why") {
  static model::Project project;
  project.reset();
  CHECK(project.add_module("dc", 2, 1, 0) == model::Status::Ok);
  CHECK(project.add_module("ghost", 5, 1, 0) == model::Status::Ok);
  CHECK(project.add_module("bad", 3, 1, 0) == model::Status::Ok);
  CHECK(project.add_module("crashy", 6, 1, 0) == model::Status::Ok);
  CHECK(project.add_module("unused", 6, 2, 0) == model::Status::Ok);  // named by no instrument: never loaded
  const float p0[2] = {1.5f, 0.25f};
  CHECK(project.set_instrument(0, module_instrument(0, p0, 2)) == model::Status::Ok);
  CHECK(project.set_instrument(1, module_instrument(0, nullptr, 0)) == model::Status::Ok);
  CHECK(project.set_instrument(2, module_instrument(1, nullptr, 0)) == model::Status::Ok);
  CHECK(project.set_instrument(3, module_instrument(2, nullptr, 0)) == model::Status::Ok);
  CHECK(project.set_instrument(4, module_instrument(3, nullptr, 0)) == model::Status::Ok);
  model::Instrument sample;
  model::init_instrument(sample, model::InstrumentKind::Sample);
  CHECK(project.set_instrument(5, sample) == model::Status::Ok);

  const fs::path root = fs::temp_directory_path() / ("o3m_binder_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(&project)));
  fs::remove_all(root);
  fs::create_directories(root / "modules" / "instruments");
  {  // a file that is not a module
    FILE* f = std::fopen((root / "modules" / "instruments" / "bad.tmod").string().c_str(), "wb");
    NEED(f != nullptr);
    std::fputs("this is not a module file, it is long enough to hold a header of sixty-four bytes....", f);
    std::fclose(f);
  }
  platform::host::DirStorage card(root.string());
  NEED(card.mount() == hal::storage::Status::Ok);
  alignas(64) static std::uint8_t slab[512];
  alignas(64) static std::uint8_t sdram[8192];
  static module::ModuleSet set;
  set.clear();
  set.hot().init(slab, 0x1FFFC000u, sizeof slab);
  set.cold().init(sdram, 0x70000000u, sizeof sdram);
  CHECK(set.add_native("dc", kDc));
  CHECK(set.quarantine("crashy", module::Status::Quarantined));  // what the crash record of the last run puts there
  static module::ModuleLoader loader;
  Rig r;
  std::memset(sdram, 0, sizeof sdram);
  app::ModuleBinder binder;
  NEED(binder.begin(card, project, set, loader, *r.mv, nullptr));
  CHECK(!binder.begin(card, project, set, loader, *r.mv, nullptr));  // one run at a time
  unsigned steps = 0;
  while (binder.state() == app::ModuleBinder::State::Running && steps < 1000) {
    binder.step(256);
    ++steps;
  }
  CHECK(binder.state() == app::ModuleBinder::State::Done);
  const app::ModuleReport& rep = binder.report();
  CHECK(rep.instruments == 5);
  CHECK(rep.modules == 4);
  CHECK(rep.already == 1);
  CHECK(rep.loaded == 0);
  CHECK(rep.bound == 2);
  CHECK(rep.muted == 3);
  CHECK(rep.problem_count == 3);
  CHECK(r.mv->bound(0));
  CHECK(r.mv->bound(1));
  CHECK(!r.mv->bound(2));
  CHECK(!r.mv->bound(3));
  CHECK(!r.mv->bound(4));
  CHECK(!r.mv->bound(5));  // a sample instrument is not the binder's
  CHECK(set.quarantined("bad"));  // the file that failed validation
  // the stored parameters reached the module: instrument 0 had {1.5, 0.25}
  alignas(16) static DcState* none = nullptr;
  (void)none;
  bool saw_missing = false, saw_refused = false, saw_quarantined = false;
  for (unsigned i = 0; i < rep.problem_count; ++i) {
    const app::ModuleProblem& p = rep.problems[i];
    char line[160];
    app::describe_module_problem(p, line, sizeof line);
    std::printf("%s\n", line);
    if (std::string(p.name) == "ghost") {
      saw_missing = p.kind == app::ModuleProblem::Kind::NotFound && p.slot == 0xFF;
    } else if (std::string(p.name) == "bad") {
      saw_refused = p.kind == app::ModuleProblem::Kind::Refused && p.status == module::Status::BadMagic;
    } else if (std::string(p.name) == "crashy") {
      saw_quarantined = p.kind == app::ModuleProblem::Kind::Quarantined;
    }
  }
  CHECK(saw_missing);
  CHECK(saw_refused);
  CHECK(saw_quarantined);
  char summary[160];
  app::describe_modules(rep, summary, sizeof summary);
  CHECK(std::string(summary) == "modules: 5 instruments, 0 loaded, 1 already there, 2 bound, 3 muted");
  // a note on a muted instrument sounds nothing and is not an error; on a bound one it sounds
  cue(0, event(0, EventKind::NoteOn, 0, 60, 2, 127));
  cue(0, event(0, EventKind::NoteOn, 1, 60, 0, 127));
  r.block();
  CHECK(r.mv->feed(0) == nullptr);
  CHECK(r.mv->feed(1) != nullptr);
  r.mv->retire_all();
  r.block();
  r.block();
  r.mv->release_all();
  fs::remove_all(root);
}

// ---- OS-BUG-MODDRIVE: Overdrive and Bit depth (step FX 38 / 39, Perform OVERDRV / BITDEPTH) shape a module instrument's track ---------------------------------
namespace {

using audio::kFxBitDepth;
using audio::kFxOverdrive;
using audio::kLayerFx1;
using audio::kLayerPerf;
using audio::kModGroupBitDepth;
using audio::kModGroupOverdrive;

// A held voice that renders the same 64-sample figure every block (a scrambled ramp over most of the range), so that one block of a run without FX is the oracle's input for every other run.
std::uint32_t ramp_render(void* s, const float*, float* out, std::uint32_t n) {
  const DcState* d = static_cast<const DcState*>(s);
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = d->gate != 0u ? static_cast<float>((i * 37u) % 64u) / 64.0f * 1.8f - 0.9f : 0.0f;
  }
  return d->gate != 0u ? 1u : 0u;
}
const o3m_module kRamp = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "ramp", sizeof(DcState), 0, nullptr,
                          dc_init, ramp_render, dc_set_param, dc_note_on, dc_note_off, dc_shutdown, nullptr, 0};

SeqEvent drive_set(unsigned group, std::uint8_t track, std::uint8_t a, std::uint8_t b, std::uint8_t layer, std::int32_t value) {
  SeqEvent e{};
  e.kind = EventKind::ModSet;
  e.track = track;
  e.a = a;
  e.b = b;
  e.c = layer;
  e.flags = static_cast<std::uint8_t>(group);
  e.value = value;
  return e;
}
SeqEvent step_od(std::uint8_t track, unsigned v) { return drive_set(kModGroupOverdrive, track, kFxOverdrive, static_cast<std::uint8_t>(v), kLayerFx1, 0); }
SeqEvent step_bits(std::uint8_t track, unsigned v) { return drive_set(kModGroupBitDepth, track, kFxBitDepth, static_cast<std::uint8_t>(v), kLayerFx1, 0); }
SeqEvent layer_od(std::uint8_t track, int v) { return drive_set(kModGroupOverdrive, track, 0, 0, kLayerPerf, v); }
SeqEvent layer_bits(std::uint8_t track, int shift) { return drive_set(kModGroupBitDepth, track, 0, 0, kLayerPerf, shift); }
SeqEvent drive_end(std::uint8_t track, unsigned group) {
  SeqEvent e{};
  e.kind = EventKind::ModEnd;
  e.track = track;
  e.flags = static_cast<std::uint8_t>(group);
  return e;
}

struct DriveCue {
  std::uint32_t block;
  SeqEvent ev;
};

// The feeds of `track` over `blocks` blocks of a ramp module bound to slot 3: a note at block 0 (after the block-0 events in `cues`; the others as cued) that stays held. `hook`, if set, runs
// before each block with its index (the export's put-away and restore).
std::vector<std::vector<std::int16_t>> ramp_run(unsigned track, const std::vector<DriveCue>& cues, unsigned blocks, void (*hook)(VoicePool&, unsigned) = nullptr) {
  Rig r;
  std::vector<std::vector<std::int16_t>> feeds;
  if (r.mv->bind(3, kRamp, 0x8003, &r.dc[0], nullptr, 0) != ModuleVoices::Bind::Ok) {
    FAIL("the ramp module did not bind");
    return feeds;
  }
  for (const DriveCue& c : cues) {
    cue(c.block, c.ev);
  }
  cue(0, event(0, EventKind::NoteOn, static_cast<std::uint8_t>(track), 60, 3, 127));
  for (unsigned b = 0; b < blocks; ++b) {
    if (hook != nullptr) {
      hook(*r.pool, b);
    }
    r.block();
    feeds.push_back(r.feed(track));
  }
  r.mv->retire_all();
  r.block();
  r.block();
  r.mv->release_all();
  return feeds;
}

// The oracle: the pure curves of voices.h over the unshaped block, as the sampler's tests hold them (the drive 0: none; the shift 0: none).
std::vector<std::int16_t> shaped(const std::vector<std::int16_t>& in, unsigned od, unsigned shift) {
  std::vector<std::int16_t> out(in.size());
  const std::int32_t g = audio::overdrive_gain_q11(od);
  for (std::size_t i = 0; i < in.size(); ++i) {
    std::int32_t x = in[i];
    x = g != 0 ? audio::overdrive_sample(x, g) : x;
    x = shift != 0u ? audio::bit_depth_sample(x, shift) : x;
    out[i] = static_cast<std::int16_t>(x);
  }
  return out;
}

const std::vector<std::int16_t>& unshaped_block() {
  static const std::vector<std::int16_t> ref = [] {
    const auto f = ramp_run(0, {}, 2);
    return f.size() == 2 ? f[1] : std::vector<std::int16_t>();
  }();
  return ref;
}

}  // namespace

TEST_CASE("module drive: with no FX the block is the module's own, and shape_pcm leaves a drive and a depth of none alone (OS-BUG-MODDRIVE)") {
  const std::vector<std::int16_t>& ref = unshaped_block();
  NEED(ref.size() == kBlk);
  CHECK(ref[1] != ref[2]);  // the figure is not flat: a shaper would show
  // a layer BITDEPTH of 0 (16 bits), a step FX 39 of 16 bits and a FX 38 of 0 are neutral: the block is byte-equal (the guard of shape_pcm: a gain of 0 would otherwise zero it)
  const auto f = ramp_run(0, {{0, layer_bits(0, 0)}, {0, step_bits(0, 16)}, {0, step_od(0, 0)}}, 2);
  NEED(f.size() == 2);
  CHECK(f[1] == ref);
  std::int16_t buf[8] = {5, -5, 32767, -32768, 100, 0, 1, -1};
  std::int16_t keep[8];
  std::memcpy(keep, buf, sizeof buf);
  VoicePool::shape_pcm(buf, 8, 0u, 0u, false);
  CHECK(std::memcmp(buf, keep, sizeof buf) == 0);
  VoicePool::shape_pcm(buf, 8, 0u, 0u, true);
  CHECK(std::memcmp(buf, keep, sizeof buf) == 0);
}

TEST_CASE("module drive: the step FX 38 and 39 on a module track give the sampler's curve over the module's block (OS-BUG-MODDRIVE)") {
  const std::vector<std::int16_t>& ref = unshaped_block();
  NEED(ref.size() == kBlk);
  for (unsigned od : {1u, 50u, 100u}) {
    const auto f = ramp_run(0, {{0, step_od(0, od)}}, 2);
    NEED(f.size() == 2);
    CHECK_MESSAGE(f[1] == shaped(ref, od, 0u), "FX 38 at " << od);
    CHECK(f[1] != ref);
  }
  for (unsigned bits : {12u, 1u}) {
    const auto f = ramp_run(0, {{0, step_bits(0, bits)}}, 2);
    NEED(f.size() == 2);
    CHECK_MESSAGE(f[1] == shaped(ref, 0u, 16u - bits), "FX 39 at " << bits);
    CHECK(f[1] != ref);
  }
  const auto both = ramp_run(0, {{0, step_od(0, 50)}, {0, step_bits(0, 12)}}, 2);
  NEED(both.size() == 2);
  CHECK(both[1] == shaped(ref, 50u, 4u));
  // the same through shape_pcm on the unshaped block: one curve
  std::vector<std::int16_t> direct = ref;
  VoicePool::shape_pcm(direct.data(), kBlk, audio::overdrive_gain_q11(50), 4u, false);
  CHECK(direct == both[1]);
  // the FX of another track shapes nothing of this one
  const auto other = ramp_run(0, {{0, step_od(5, 100)}}, 2);
  NEED(other.size() == 2);
  CHECK(other[1] == ref);
}

TEST_CASE("module drive: the Perform layer's OVERDRV and BITDEPTH win over the step FX, the BITDEPTH value is the shift, and the audition track is shaped too (OS-BUG-MODDRIVE)") {
  const std::vector<std::int16_t>& ref = unshaped_block();
  NEED(ref.size() == kBlk);
  const auto f = ramp_run(0, {{0, step_od(0, 100)}, {0, step_bits(0, 12)}, {0, layer_od(0, 10)}, {0, layer_bits(0, 7)}}, 2);
  NEED(f.size() == 2);
  CHECK(f[1] == shaped(ref, 10u, 7u));
  const auto g = ramp_run(0, {{0, layer_od(0, 40)}}, 2);  // the layer's overdrive alone, nothing under it
  NEED(g.size() == 2);
  CHECK(g[1] == shaped(ref, 40u, 0u));
  const auto a = ramp_run(audio::kAuditionTrack, {{0, step_od(audio::kAuditionTrack, 70)}, {0, step_bits(audio::kAuditionTrack, 8)}}, 2);  // the spare strip
  NEED(a.size() == 2);
  CHECK(a[1] == shaped(ref, 70u, 8u));
}

TEST_CASE("module drive: FxReset ends the step FX and leaves the layer, Stopped ends neither, a ModEnd lets go (OS-BUG-MODDRIVE)") {
  const std::vector<std::int16_t>& ref = unshaped_block();
  NEED(ref.size() == kBlk);
  SeqEvent reset{};
  reset.kind = EventKind::FxReset;
  SeqEvent stopped{};
  stopped.kind = EventKind::Stopped;
  const SeqEvent note = event(0, EventKind::NoteOn, 0, 60, 3, 127);
  const auto f = ramp_run(0,
                          {{0, step_od(0, 100)}, {0, layer_bits(0, 4)},          // blocks 0, 1: the FX 38 at 100 and the layer's 12 bits
                           {2, reset}, {2, note},                                // blocks 2, 3: the FX is gone, the layer is not
                           {4, stopped}, {5, note},                              // block 5: Stopped ended neither
                           {6, drive_end(0, kModGroupBitDepth)}, {6, note}},     // blocks 6, 7: nothing is left
                          8);
  NEED(f.size() == 8);
  CHECK(f[1] == shaped(ref, 100u, 4u));
  CHECK(f[3] == shaped(ref, 0u, 4u));
  CHECK(f[5] == shaped(ref, 0u, 4u));
  CHECK(f[7] == ref);
}

TEST_CASE("module drive: the export puts the layer away and brings it back (OS-BUG-MODDRIVE)") {
  const std::vector<std::int16_t>& ref = unshaped_block();
  NEED(ref.size() == kBlk);
  static VoicePool::PerfSave away;
  const auto f = ramp_run(0, {{0, layer_od(0, 60)}}, 6, [](VoicePool& p, unsigned b) {
    if (b == 2u) {
      p.perf_save(away);
      p.perf_clear();  // offline_enter: the export is heard without the layer
    } else if (b == 4u) {
      p.perf_restore(away);  // offline_leave
    }
  });
  NEED(f.size() == 6);
  CHECK(f[1] == shaped(ref, 60u, 0u));
  CHECK(f[2] == ref);
  CHECK(f[3] == ref);
  CHECK(f[4] == shaped(ref, 60u, 0u));
  CHECK(f[5] == shaped(ref, 60u, 0u));
}
