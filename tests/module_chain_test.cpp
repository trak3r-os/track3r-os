// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MODSILENT: the built-in modules through the WHOLE host path, the way the image wires them (platform/mk66/src/app_main.cpp make_modules(), module_bind(), module_audition()):
// the module voices are a static object (SRAM_U on the unit), the instance state comes from a small state arena (ModuleSet::take_state), the hot part of the module is in its own
// slab and the cold part in another region, `mod note` is ModuleVoices::audition() on the audition track (16), and the sound has to come out of the Engine's graph (the track strip,
// MAIN, the interleaved TX block): the voice -> output block -> track strip -> MAIN chain that module_builtin_run_test.cpp (ModuleVoices alone) and module_voices_test.cpp (a host DC
// module) each cover only half of. Where the module code can run (the Cortex-M4F: the QEMU suite) every built-in module must be audible at the end of it, with a peak that is neither
// silence nor a clip; on the host the Thumb code cannot be called and only the loads are checked.
//
// WHAT IT CAUGHT. The unit's `mod note` was silent: the voices rendered (the voice block held the note, the render stage rose by the module's cost) but the strip of track 16 was never
// run, because the graph's mask of active strips is the twelve tracks of a new project (kDefaultActiveMask) and nothing switched the spare one on. The first test below failed on the
// tree of that report (voice block peak 21,240 for the TB-303, engine output 0) and passes with Engine::attach_modules() switching the audition strip on. The second proves the same chain
// on an ordinary track, where it never failed: the OS-PERF3 placement of the module state and voices (a static, a state arena) does not change what the render makes.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/null_storage.h"
#include "support/static_slot.h"

#include <o3os/audio/engine.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/taps.h>
#include <o3os/audio/voices.h>
#include <o3os/model/project.h>
#include <o3os/module/loader.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using namespace o3os;
using module::LoadedModule;
using module::ModuleLoader;
using module::ModuleSet;
namespace haudio = o3os::hal::audio;

// The module memory, laid out like the unit's (mem_map.h): the slab (the hot arena behind the voices' output blocks), the cold region, and the state arena (SRAM_U on the unit).
constexpr std::uint32_t kSlabBytes = 14u * 1024u - audio::ModuleVoices::kScratchBytes;
constexpr std::uint32_t kStateBytes = 4096;
alignas(64) std::uint8_t g_scratch[audio::ModuleVoices::kScratchBytes];
alignas(64) std::uint8_t g_slab[kSlabBytes];
alignas(64) std::uint8_t g_cold[64 * 1024];
alignas(64) std::uint8_t g_state[kStateBytes];
audio::ModuleVoices g_voices;  // a static with a constructor, as g_module_voices is in app_main.cpp

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return g_cycles += 7u; }

struct Chain {
  test::NullStorage card;
  ModuleSet set;
  ModuleLoader loader;
  audio::Engine* engine = nullptr;
  audio::VoicePool* pool = nullptr;
  haudio::BlockReport report{};
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};

  Chain() {
    static test::StaticSlot<audio::Engine> engine_slot;
    static test::StaticSlot<audio::VoicePool> pool_slot;
    static audio::TapMem taps;
    set.set_builtins(module::kBuiltinModules, module::kBuiltinModuleCount);
    set.hot().init(g_slab, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_slab)), sizeof g_slab);
    set.cold().init(g_cold, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_cold)), sizeof g_cold);
    set.state().init(g_state, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_state)), sizeof g_state);
    engine = &engine_slot.make(&fake_cycles, taps);
    pool = &pool_slot.make();
    std::memset(g_scratch, 0, sizeof g_scratch);
    g_voices.retire_all();
    g_voices.release_all();
    g_voices.attach_scratch(g_scratch, sizeof g_scratch);
    g_voices.set_sample_rate(static_cast<float>(haudio::sample_rate_mhz()) * 0.001f);
    engine->attach_voices(pool);
    engine->attach_modules(&g_voices);  // as make_sound() does, before the audio starts
  }
  ~Chain() {
    g_voices.retire_all();
    g_voices.release_all();
  }
  bool load(const char* stem) {
    if (!loader.begin(card, set, stem, nullptr)) {
      return false;
    }
    for (unsigned n = 0; n < 100000 && loader.state() == ModuleLoader::State::Running; ++n) {
      (void)loader.step(2048);
    }
    return loader.state() == ModuleLoader::State::Done;
  }
  // One block through the engine and its graph; the TX block's peak (both channels).
  std::int32_t block() {
    report = haudio::BlockReport{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine->render_block(io);
    std::int32_t peak = 0;
    for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
      const std::int32_t v = out[i] < 0 ? -static_cast<std::int32_t>(out[i]) : static_cast<std::int32_t>(out[i]);
      peak = v > peak ? v : peak;
    }
    return peak;
  }
};

std::int32_t peak_of(const std::int16_t* p, std::uint32_t n) {
  std::int32_t peak = 0;
  for (std::uint32_t i = 0; p != nullptr && i < n; ++i) {
    const std::int32_t v = p[i] < 0 ? -static_cast<std::int32_t>(p[i]) : static_cast<std::int32_t>(p[i]);
    peak = v > peak ? v : peak;
  }
  return peak;
}

// What a note of the module is heard as: the peak of the module voices' block for the track, and of the engine's output block, over 80 blocks (about 116 ms).
struct Heard {
  bool bound = false;  // false: the module could not be loaded and bound here (the host cannot call Thumb code: the target suite does)
  bool fed = false;
  std::int32_t feed_peak = 0;
  std::int32_t out_peak = 0;
};

// `mod load <stem>`, `mod bind 61 <stem>` (module_bind(): the state block from the state arena first, zeroed), `mod note 61 <note> 127` on `track` (ModuleVoices::audition()).
Heard hear(const char* stem, unsigned note, unsigned track) {
  Heard h;
  Chain c;
  if (!c.load(stem)) {
    CHECK_MESSAGE(false, "the built-in module does not load");
    return h;
  }
  const LoadedModule& m = *c.set.at(static_cast<unsigned>(c.set.find(stem)));
  if (m.entry == nullptr) {
    return h;
  }
  std::uint8_t* state = nullptr;
  std::uint32_t addr = 0;
  if (!c.set.take_state(m.entry->state_bytes, state, addr)) {
    CHECK_MESSAGE(false, "no state block");
    return h;
  }
  CHECK(c.set.state().used() >= m.entry->state_bytes);  // it came from the state arena, not the cold one
  std::memset(state, 0, m.entry->state_bytes);
  constexpr unsigned kSlot = 60;
  const bool bound = g_voices.bind(kSlot, *m.entry, module::fault_id(stem), state, nullptr, 0) == audio::ModuleVoices::Bind::Ok;
  CHECK(bound);
  if (!bound || !g_voices.audition(kSlot, track, note, 127u, true)) {
    CHECK_MESSAGE(false, "bind or audition refused");
    return h;
  }
  h.bound = true;
  for (unsigned b = 0; b < 80; ++b) {
    const std::int32_t p = c.block();
    h.out_peak = p > h.out_peak ? p : h.out_peak;
    const std::int16_t* f = g_voices.feed(track);
    h.fed = h.fed || f != nullptr;
    const std::int32_t fp = peak_of(f, haudio::kBlock);
    h.feed_peak = fp > h.feed_peak ? fp : h.feed_peak;
  }
  return h;
}

constexpr struct {
  const char* stem;
  unsigned note;
} kModules[] = {{"tb303", 36}, {"chip", 60}, {"fm", 60}, {"sid", 60}};

}  // namespace

TEST_CASE("module chain: each built-in module, played by audition() on the audition track (16) as `mod note` does, is audible at the engine's output") {
  NEED(module::kBuiltinModuleCount == 4u);
  CHECK(audio::kAuditionTrack == 15u);  // track 16: the spare strip, outside the twelve of a new project
  for (const auto& k : kModules) {
    CAPTURE(k.stem);
    const Heard h = hear(k.stem, k.note, audio::kAuditionTrack);
    if (!h.bound) {
      continue;
    }
    std::printf("[module chain] %-5s note %u on track 16: voice block peak %d, engine output peak %d of 32767\n", k.stem, k.note, static_cast<int>(h.feed_peak),
                static_cast<int>(h.out_peak));
    CHECK(h.fed);               // the module's note reached the track
    CHECK(h.feed_peak > 200);   // the module rendered audible audio into its output block (not silence, not NaN)
    CHECK(h.out_peak > 100);    // and the track strip carried it to MAIN and the TX block (the unit's `mod note` was silent here: the strip was off)
    CHECK(h.out_peak < 32000);  // sane: not a clip
  }
}

TEST_CASE("module chain: the same through an ordinary track (4): the OS-PERF3 placement of the module state and voices changes nothing the render makes") {
  NEED(module::kBuiltinModuleCount == 4u);
  for (const auto& k : kModules) {
    CAPTURE(k.stem);
    const Heard h = hear(k.stem, k.note, 3u);
    if (!h.bound) {
      continue;
    }
    std::printf("[module chain] %-5s note %u on track 4: voice block peak %d, engine output peak %d of 32767\n", k.stem, k.note, static_cast<int>(h.feed_peak),
                static_cast<int>(h.out_peak));
    CHECK(h.fed);
    CHECK(h.feed_peak > 200);
    CHECK(h.out_peak > 100);
    CHECK(h.out_peak < 32000);
  }
}

// Only the audition strip was switched on: the other tracks beyond the twelve stay as they were (a sampler note there is still skipped by the strip pass).
TEST_CASE("module chain: attaching the modules switches on the audition strip and no other beyond the default twelve") {
  Chain c;
  static std::int16_t pcm[4000];
  for (std::int16_t& s : pcm) {
    s = 8000;
  }
  c.pool->set_sample(0, audio::SampleDesc{pcm, 4000, 0, 0, 65536});
  const struct {
    unsigned track;
    bool heard;
  } kTracks[] = {{3, true}, {11, true}, {12, false}, {13, false}, {14, false}, {audio::kAuditionTrack, true}};
  for (const auto& t : kTracks) {
    CAPTURE(t.track);
    c.pool->note_on(t.track, 60, 0, 127);
    std::int32_t peak = 0;
    for (unsigned b = 0; b < 20; ++b) {
      const std::int32_t p = c.block();
      peak = p > peak ? p : peak;
    }
    CHECK((peak > 100) == t.heard);
    c.pool->cut(t.track);
    for (unsigned b = 0; b < 8; ++b) {
      (void)c.block();  // the declick residual runs out
    }
  }
}

// ---- the sequencer and the pads --------------------------------------------------------------------------------------------------------

namespace {

constexpr unsigned kInstrument = 20;  // 1-based: slot 19, in the sampler's 1..48 as the unit test used it

// A project with one pattern: a note of instrument 20 on `track` at step 0. `module_kind`: the project has the instrument as a Module instrument (what the engine picker makes), else the slot
// is free (what a console `mod bind` alone leaves).
model::Project& make_project(const char* stem, unsigned note, unsigned track, bool module_kind, const o3m_module& mod, unsigned notes = 1, unsigned gap = 2) {
  static model::ModelArena arena;
  model::Project& project = arena.project;
  project.reset();
  CHECK(project.create_pattern(1, 16) == model::Status::Ok);
  CHECK(project.append_order(1) == model::Status::Ok);
  if (module_kind) {
    CHECK(project.add_module(stem, std::strlen(stem), mod.abi_major, mod.abi_minor) == model::Status::Ok);
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Module);
    in.module_index = static_cast<std::uint8_t>(project.module_count() - 1u);
    std::strncpy(in.name, mod.name, sizeof in.name - 1u);
    in.param_len = static_cast<std::uint16_t>(4u * mod.param_count);
    for (unsigned i = 0; i < mod.param_count; ++i) {
      std::memcpy(in.params + 4u * i, &mod.params[i].def, 4);
    }
    CHECK(project.set_instrument(kInstrument - 1u, in) == model::Status::Ok);
  }
  model::Step st = model::kEmptyStep;
  st.note = static_cast<std::uint8_t>(note + 1u);
  st.instr = static_cast<std::uint8_t>(kInstrument);
  for (unsigned n = 0; n < notes; ++n) {  // `gap` rows apart, no note-off anywhere
    CHECK(project.set_step(1, track, n * gap, st) == model::Status::Ok);
  }
  return project;
}

// Binds the (loaded) module at slot 19 as the console and the picker do: the state block from the state arena, zeroed, bind() with the instrument's stored parameters (null: none).
const o3m_module* bind_module(Chain& c, const char* stem, const std::uint8_t* params, std::uint32_t param_len) {
  const LoadedModule& m = *c.set.at(static_cast<unsigned>(c.set.find(stem)));
  if (m.entry == nullptr) {
    return nullptr;  // the host cannot call Thumb code
  }
  std::uint8_t* state = nullptr;
  std::uint32_t addr = 0;
  if (!c.set.take_state(m.entry->state_bytes, state, addr)) {
    CHECK_MESSAGE(false, "no state block");
    return nullptr;
  }
  std::memset(state, 0, m.entry->state_bytes);
  const bool ok = g_voices.bind(kInstrument - 1u, *m.entry, module::fault_id(stem), state, params, param_len) == audio::ModuleVoices::Bind::Ok;
  CHECK(ok);
  return ok ? m.entry : nullptr;
}

// Plays pattern 1 for 80 blocks through the sequencer as the engine's event source, as app_main.cpp wires it.
Heard play_pattern(Chain& c, model::Project& project, unsigned track, unsigned blocks = 80, std::int32_t* per_block = nullptr, unsigned* start_block = nullptr, unsigned max_starts = 0) {
  static test::StaticSlot<seq::Sequencer> seq_slot;
  static audio::SeqEvent events[audio::kMaxEventsPerBlock];
  seq::Sequencer& sequencer = seq_slot.make(project);
  c.engine->attach_events(seq::event_source(sequencer, events, audio::kMaxEventsPerBlock));
  CHECK(sequencer.commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0}));
  Heard h;
  h.bound = true;
  unsigned starts = 0;
  std::uint32_t seen = g_voices.stats().note_ons;
  for (unsigned b = 0; b < blocks; ++b) {
    const std::int32_t p = c.block();
    h.out_peak = p > h.out_peak ? p : h.out_peak;
    const std::int16_t* f = g_voices.feed(track);
    h.fed = h.fed || f != nullptr;
    const std::int32_t fp = peak_of(f, haudio::kBlock);
    h.feed_peak = fp > h.feed_peak ? fp : h.feed_peak;
    if (per_block != nullptr) {
      per_block[b] = fp;
    }
    if (g_voices.stats().note_ons != seen) {  // a note started in this block
      if (start_block != nullptr && starts < max_starts) {
        start_block[starts] = b;
      }
      ++starts;
      seen = g_voices.stats().note_ons;
    }
  }
  return h;
}

}  // namespace

TEST_CASE("module chain: a pattern step of a bound Module instrument (1..48, bound at run time) plays the module through the strip") {
  NEED(module::kBuiltinModuleCount == 4u);
  for (const auto& k : kModules) {
    CAPTURE(k.stem);
    Chain c;
    const unsigned track = 2;  // track 3
    // The module is needed first for the project (its parameter table), the project for the bind (the instrument's stored parameters): load, then both.
    if (!c.load(k.stem)) {
      CHECK_MESSAGE(false, "the built-in module does not load");
      continue;
    }
    const LoadedModule& lm = *c.set.at(static_cast<unsigned>(c.set.find(k.stem)));
    if (lm.entry == nullptr) {
      continue;
    }
    model::Project& project = make_project(k.stem, k.note, track, true, *lm.entry);
    const model::Instrument* stored = project.instrument(kInstrument - 1u);
    NEED(stored != nullptr);
    NEED(bind_module(c, k.stem, stored->params, stored->param_len) != nullptr);
    const std::uint32_t before = g_voices.stats().note_ons;  // (the statistics of the static voices run on from one case to the next)
    const Heard h = play_pattern(c, project, track);
    const std::uint32_t note_ons = g_voices.stats().note_ons - before;
    std::printf("[module chain] %-5s pattern step on track 3, instrument 20: voice block peak %d, engine output peak %d, note_ons %u\n", k.stem, static_cast<int>(h.feed_peak),
                static_cast<int>(h.out_peak), static_cast<unsigned>(note_ons));
    CHECK(note_ons == 1u);  // the sequencer's one NoteOn reached the module voice
    CHECK(h.fed);
    CHECK(h.feed_peak > 200);
    CHECK(h.out_peak > 100);
    CHECK(h.out_peak < 32000);
  }
}

TEST_CASE("module chain: a console bind alone (the project's slot is free) is not played by a pattern (a free slot makes no sound); a pad and `mod note` play it") {
  Chain c;
  if (!c.load("tb303")) {
    CHECK_MESSAGE(false, "the built-in module does not load");
    return;
  }
  const LoadedModule& lm = *c.set.at(static_cast<unsigned>(c.set.find("tb303")));
  if (lm.entry == nullptr) {
    return;
  }
  model::Project& project = make_project("tb303", 36, 2, false, *lm.entry);
  NEED(project.instrument(kInstrument - 1u)->kind == model::InstrumentKind::None);
  NEED(bind_module(c, "tb303", nullptr, 0) != nullptr);
  const std::uint32_t before = g_voices.stats().note_ons;
  const Heard seq_heard = play_pattern(c, project, 2);
  CHECK(g_voices.stats().note_ons == before);  // the sequencer never started the voice: the instrument is idle
  CHECK(!seq_heard.fed);
  CHECK(seq_heard.out_peak == 0);
  // A pad on the same instrument: the live note path asks the module voices, not the project.
  static audio::LiveBox box;
  box.notes = audio::LiveNotes{};
  while (box.queue.size() != 0u) {
    audio::LiveEvent drop;
    (void)box.queue.pop(drop);
  }
  c.engine->attach_live(&box);
  CHECK(box.queue.push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, 1, 36, static_cast<std::uint8_t>(kInstrument), audio::kLiveVelocityDefault, 2, 0}));
  std::int32_t out_peak = 0;
  for (unsigned b = 0; b < 80; ++b) {
    const std::int32_t p = c.block();
    out_peak = p > out_peak ? p : out_peak;
  }
  std::printf("[module chain] tb303 pad on track 3, instrument 20: engine output peak %d, note_ons %u\n", static_cast<int>(out_peak),
              static_cast<unsigned>(g_voices.stats().note_ons - before));
  CHECK(g_voices.stats().note_ons == before + 1u);
  CHECK(out_peak > 100);
  CHECK(out_peak < 32000);
  CHECK(box.queue.push(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePad, 1, 0, 0, 0, 0, 0}));
  (void)c.block();
  bool found = false;
  for (unsigned i = 0; i < audio::kMaxModuleInstances; ++i) {
    audio::ModuleVoices::Info info{};
    if (g_voices.info(i, info) && info.slot == kInstrument - 1u) {
      found = true;
      CHECK(!info.held);  // the pad's release ended the note (the module's own tail may still sound)
    }
  }
  CHECK(found);
}

// ---- a pattern's notes are new notes (no legato from the sequencer) --------------------------------------------------------------------

namespace {

// A native module that records what the host tells it, in order: 'N' a note_on with no flags, 'L' one with O3M_NOTE_LEGATO, 'F' a note_off. It sounds a constant while a note is held.
struct alignas(16) RecState {
  std::uint32_t gate = 0;
  std::uint32_t n = 0;
  char log[48] = {};
};
int rec_init(void*, const o3m_host*) { return 0; }
std::uint32_t rec_render(void* raw, const float*, float* out, std::uint32_t frames) {
  const RecState* r = static_cast<RecState*>(raw);
  for (std::uint32_t i = 0; i < frames; ++i) {
    out[i] = r->gate != 0u ? 0.25f : 0.0f;
  }
  return r->gate != 0u ? 1u : 0u;
}
void rec_set_param(void*, std::uint32_t, float) {}
void rec_put(RecState* r, char c) {
  if (r->n + 1u < sizeof r->log) {
    r->log[r->n++] = c;
    r->log[r->n] = '\0';
  }
}
void rec_note_on(void* raw, std::uint32_t, std::uint32_t, std::uint32_t flags) {
  RecState* r = static_cast<RecState*>(raw);
  r->gate = 1;
  rec_put(r, (flags & O3M_NOTE_LEGATO) != 0u ? 'L' : 'N');
}
void rec_note_off(void* raw, std::uint32_t) {
  RecState* r = static_cast<RecState*>(raw);
  r->gate = 0;
  rec_put(r, 'F');
}
void rec_shutdown(void*) {}
const o3m_module kRec = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "rec", sizeof(RecState), 0, nullptr,
                         rec_init, rec_render, rec_set_param, rec_note_on, rec_note_off, rec_shutdown, nullptr, 0};

}  // namespace

TEST_CASE("module chain: a pattern with a note every 2 rows and no note-offs starts every note anew: the module is released before each, never told legato") {
  Chain c;
  alignas(16) RecState state;
  model::Project& project = make_project("rec", 60, 2, true, kRec, 4, 2);
  NEED(g_voices.bind(kInstrument - 1u, kRec, 0x8123, &state, nullptr, 0) == audio::ModuleVoices::Bind::Ok);
  const Heard h = play_pattern(c, project, 2, 600);  // 4 notes at rows 0, 2, 4, 6 of 125 ms
  std::printf("[module chain] recording module, 4 pattern notes 2 rows apart: '%s'\n", state.log);
  CHECK(std::strcmp(state.log, "NFNFNFN") == 0);  // N F N F N F N: each note ends the previous one first, and none is a slide
  CHECK(h.fed);
  CHECK(h.out_peak > 100);
}

TEST_CASE("module chain: a live key over a held one is legato (a played overlap), and not after a release; the pattern's notes stay new notes") {
  Chain c;
  alignas(16) RecState state;
  NEED(g_voices.bind(kInstrument - 1u, kRec, 0x8123, &state, nullptr, 0) == audio::ModuleVoices::Bind::Ok);
  static audio::LiveBox box;
  box.notes = audio::LiveNotes{};
  while (box.queue.size() != 0u) {
    audio::LiveEvent drop;
    (void)box.queue.pop(drop);
  }
  c.engine->attach_live(&box);
  const auto live = [&](audio::LiveKind kind, std::uint8_t key) {
    CHECK(box.queue.push(audio::LiveEvent{kind, audio::kLiveSourcePad, key, 60, static_cast<std::uint8_t>(kInstrument), audio::kLiveVelocityDefault, 2, 0}));
    (void)c.block();
  };
  live(audio::LiveKind::On, 1);   // key 1 down: a note
  live(audio::LiveKind::On, 2);   // key 2 down while key 1 is held: the live notes pick another track, the instance is mono and takes the new note as legato
  live(audio::LiveKind::Off, 1);  // key 1's track no longer has the instance: nothing to release
  live(audio::LiveKind::Off, 2);  // key 2's note ends
  live(audio::LiveKind::On, 3);   // after the release: a fresh note
  std::printf("[module chain] recording module, live keys: '%s'\n", state.log);
  CHECK(std::strcmp(state.log, "NLFN") == 0);
}

// The review's case: a pattern's note rings (nothing releases it) and the user strikes a pad of the same instrument. The pad is a new note on a voice that still sounds, not a slide into a note
// nobody played: N F N (a slide, N L, has no new envelope and the pad would be quiet).
TEST_CASE("module chain: a pad struck over a sequencer's ringing note is a new note (N F N), not a slide") {
  Chain c;
  alignas(16) RecState state;
  model::Project& project = make_project("rec", 60, 2, true, kRec, 1, 2);
  NEED(g_voices.bind(kInstrument - 1u, kRec, 0x8123, &state, nullptr, 0) == audio::ModuleVoices::Bind::Ok);
  static audio::LiveBox box;
  box.notes = audio::LiveNotes{};
  while (box.queue.size() != 0u) {
    audio::LiveEvent drop;
    (void)box.queue.pop(drop);
  }
  c.engine->attach_live(&box);
  (void)play_pattern(c, project, 2, 20);  // the pattern's one note starts and rings: no note-off is ever sent
  CHECK(std::strcmp(state.log, "N") == 0);
  CHECK(box.queue.push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, 1, 62, static_cast<std::uint8_t>(kInstrument), audio::kLiveVelocityDefault, 5, 0}));
  for (unsigned b = 0; b < 4; ++b) {
    (void)c.block();
  }
  std::printf("[module chain] recording module, a pattern note then a pad: '%s'\n", state.log);
  CHECK(std::strcmp(state.log, "NFN") == 0);
  // and a second pad over the first IS a played overlap: legato
  CHECK(box.queue.push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, 2, 64, static_cast<std::uint8_t>(kInstrument), audio::kLiveVelocityDefault, 5, 0}));
  for (unsigned b = 0; b < 4; ++b) {
    (void)c.block();
  }
  CHECK(std::strcmp(state.log, "NFNL") == 0);
}

// The module the unit was heard with (OS-MODSILENT): track 1 with the TB-303 engine and a note every 2 rows, no note-offs, was one long note decaying over 6 s: the 303 took each held
// note as a slide, and a slide has no new envelope. Every note must have its own attack.
TEST_CASE("module chain: a TB-303 pattern with a note every 2 rows has an attack on every note (the unit's one long decaying note)") {
  Chain c;
  if (!c.load("tb303")) {
    CHECK_MESSAGE(false, "the built-in module does not load");
    return;
  }
  const LoadedModule& lm = *c.set.at(static_cast<unsigned>(c.set.find("tb303")));
  if (lm.entry == nullptr) {
    return;  // the host cannot call Thumb code
  }
  model::Project& project = make_project("tb303", 36, 0, true, *lm.entry, 4, 2);
  const model::Instrument* stored = project.instrument(kInstrument - 1u);
  NEED(stored != nullptr);
  NEED(bind_module(c, "tb303", stored->params, stored->param_len) != nullptr);
  static std::int32_t peaks[600];
  unsigned start[8] = {};
  const std::uint32_t before_notes = g_voices.stats().note_ons;
  (void)play_pattern(c, project, 0, 600, peaks, start, 8);
  NEED(g_voices.stats().note_ons - before_notes == 4u);
  unsigned rises = 0;
  std::int32_t first = 0;
  for (unsigned k = 0; k < 4; ++k) {
    std::int32_t attack = 0;
    std::int32_t before = 0;
    for (unsigned b = start[k]; b < start[k] + 8u; ++b) {
      attack = peaks[b] > attack ? peaks[b] : attack;
    }
    for (unsigned b = start[k] >= 8u ? start[k] - 8u : 0u; b < start[k]; ++b) {
      before = peaks[b] > before ? peaks[b] : before;
    }
    std::printf("[module chain] tb303 note %u at block %u: attack peak %d, level just before %d\n", k + 1u, start[k], static_cast<int>(attack), static_cast<int>(before));
    CHECK(attack > 3000);
    if (k > 0u) {
      rises += attack > before + before / 10 ? 1u : 0u;  // 10 % over what the previous note had decayed to
      CHECK(attack >= first / 10 * 9);                  // and every note is as loud as the first (the slide's decay went 9059, 7418, 5988, 4828)
    } else {
      first = attack;
    }
  }
  CHECK(rises == 3u);
}
