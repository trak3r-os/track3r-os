// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011a: the sample bank (app/sample_bank.h): the arena's tail, what a slot owns, and the handover to the voices: a changed instrument reaches the voices'
// table only when playback is stopped and the voice pool has been silent for kSettleMs, never before, and the engine keeps its old descriptor until then.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/sample_bank.h>
#include <o3os/audio/voices.h>
#include <o3os/model/instrument.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <memory>
#include <vector>

using namespace o3os;
using app::SampleBank;

namespace {

struct Rig {
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  SampleBank bank;

  explicit Rig(std::uint32_t samples = 20000) : memory(samples + 8) {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), samples);
  }
  // A sample instrument in slot `slot` and `frames` of audio for it.
  void make(unsigned slot, std::uint32_t frames, std::uint8_t play_mode = 0, unsigned seed = 0) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    in.sampler.play_mode = play_mode;
    CHECK(arena->project.set_instrument(slot, in) == model::Status::Ok);
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < frames; ++i) {
      p[i] = static_cast<std::int16_t>(1000 + static_cast<int>((i + seed) % 50));  // (seed: a pattern of its own per load, so a test can tell which load's audio a slot holds)
    }
    CHECK(bank.commit_load(slot, p, frames));
  }
  // What a project load does for a slot (OgSampleLoader::bind): the audio at the arena's top, adopted as the slot's own block, the voices bound, the arena advanced.
  void loaded(unsigned slot, std::uint32_t frames, unsigned seed = 0) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    CHECK(arena->project.set_instrument(slot, in) == model::Status::Ok);
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < frames; ++i) {
      p[i] = static_cast<std::int16_t>(1000 + static_cast<int>((i + seed) % 50));
    }
    const std::uint32_t even = (frames + 1u) & ~1u;
    bank.adopt(slot, p, frames, even);
    bank.set_used(static_cast<std::uint32_t>(p - memory.data()) + even);
    bank.rebind_now(slot);
  }
  void set_slices(unsigned slot, unsigned n) {
    model::Instrument in = *arena->project.instrument(slot);
    in.sampler.slice_count = static_cast<std::uint8_t>(n);
    for (unsigned i = 0; i < n; ++i) {
      in.sampler.slices[i] = static_cast<std::uint16_t>(i * 65535u / n);
    }
    CHECK(arena->project.set_instrument(slot, in) == model::Status::Ok);
  }
  // Renders `blocks` blocks of the pool (what the audio interrupt does).
  void render(unsigned blocks) {
    for (unsigned i = 0; i < blocks; ++i) {
      voices->begin_block();
      voices->render(0, audio::VoicePool::kBlock);
      voices->end_block();
    }
  }
  void play(unsigned track, unsigned slot) {
    voices->begin_block();
    CHECK(voices->note_on(track, 60, slot, audio::kVelocityDefault));
    voices->render(0, audio::VoicePool::kBlock);
    voices->end_block();
  }
  void silence() {
    voices->release_all();
    render(200);
  }
};

}  // namespace

TEST_CASE("sample bank: the tail is reserved, then committed: the slot owns the audio, the arena grows by whole even runs, the version moves") {
  Rig rig;
  CHECK(rig.bank.ready());
  CHECK(rig.bank.free_samples() == 20000u);
  const std::uint32_t v0 = rig.bank.version(3);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  CHECK(p == rig.memory.data());
  CHECK(cap == 20000u);
  CHECK(rig.bank.free_samples() == 20000u);  // nothing is taken until the commit
  CHECK(rig.bank.commit_load(3, p, 999));
  CHECK(rig.bank.pcm(3).data == p);
  CHECK(rig.bank.pcm(3).frames == 999u);
  CHECK(rig.bank.free_samples() == 20000u - 1000u);  // 999 rounds up to 1000: the next sample starts on an even index
  CHECK(rig.bank.version(3) != v0);
  CHECK(rig.bank.rebind_pending());
  CHECK(rig.bank.pending(3));
  // The next reservation starts right after.
  std::int16_t* q = rig.bank.reserve(cap);
  CHECK(q == p + 1000);
  CHECK(cap == 19000u);
}

TEST_CASE("sample bank: a commit that is not the reserved tail, empty or too long is refused and changes nothing") {
  Rig rig(1000);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  CHECK_FALSE(rig.bank.commit_load(0, p + 2, 10));     // not where the reservation was
  CHECK_FALSE(rig.bank.commit_load(0, p, 0));          // no audio
  CHECK_FALSE(rig.bank.commit_load(0, p, 1001));       // more than is left
  CHECK_FALSE(rig.bank.commit_load(64, p, 10));        // no such slot
  CHECK(rig.bank.pcm(0).frames == 0u);
  CHECK(rig.bank.free_samples() == 1000u);
  CHECK_FALSE(rig.bank.rebind_pending());
  CHECK(rig.bank.commit_load(0, p, 1000));
  CHECK(rig.bank.reserve(cap) == nullptr);             // the arena is full
  CHECK(cap == 0u);
  CHECK(rig.bank.alloc(2) == nullptr);
}

TEST_CASE("sample bank: alloc gives even runs from the tail and refuses what does not fit") {
  Rig rig(100);
  std::int16_t* a = rig.bank.alloc(5);
  NEED(a != nullptr);
  std::int16_t* b = rig.bank.alloc(10);
  CHECK(b == a + 6);  // 5 rounds up to 6
  CHECK(rig.bank.free_samples() == 100u - 16u);
  CHECK(rig.bank.alloc(85) == nullptr);
  CHECK(rig.bank.alloc(0) == nullptr);
  CHECK(rig.bank.alloc(84) != nullptr);
  CHECK(rig.bank.free_samples() == 0u);
}

TEST_CASE("sample bank: the handover waits for stopped playback and a quiet pool, then writes the voices' table from the project's instrument") {
  Rig rig;
  rig.make(2, 4000);
  CHECK(rig.voices->sample(2).length == 0u);  // nothing is bound at the commit: only marked
  // Playing: nothing happens, however long.
  for (unsigned i = 0; i < 20; ++i) {
    CHECK_FALSE(rig.bank.poll(1000 + i * 10, false));
  }
  CHECK(rig.voices->sample(2).length == 0u);
  // Stopped: the first quiet poll only starts the settle, the writes come after kSettleMs.
  CHECK_FALSE(rig.bank.poll(2000, true));
  CHECK_FALSE(rig.bank.poll(2000 + SampleBank::kSettleMs - 1, true));
  CHECK(rig.voices->sample(2).length == 0u);
  CHECK(rig.bank.poll(2000 + SampleBank::kSettleMs, true));
  CHECK_FALSE(rig.bank.rebind_pending());
  CHECK(rig.voices->sample(2).length == 4000u);  // the instrument spans the whole sample (start 0, end 65535)
  CHECK(rig.voices->sample(2).data == rig.bank.pcm(2).data);
  CHECK(rig.bank.rebinds() == 1u);
  CHECK_FALSE(rig.bank.poll(3000, true));  // nothing left to do
}

TEST_CASE("sample bank: a voice still sounding keeps the engine's descriptor as it was; the settle restarts when it goes quiet") {
  Rig rig;
  rig.make(0, 3000);
  rig.bank.rebind_now(0);
  const audio::SampleDesc before = rig.voices->sample(0);
  NEED(before.length == 3000u);
  // The instrument changes under a playing voice: the slot is marked, the voice's data stays valid and unchanged.
  rig.play(0, 0);
  CHECK(rig.voices->voices_in_use() == 1u);
  model::Instrument in = *rig.arena->project.instrument(0);
  in.sampler.end = 32768;  // half
  CHECK(rig.arena->project.set_instrument(0, in) == model::Status::Ok);
  rig.bank.request_rebind(0);
  for (unsigned t = 0; t < 10; ++t) {
    CHECK_FALSE(rig.bank.poll(100 + t * 10, true));  // stopped, but a voice is playing
  }
  CHECK(rig.voices->sample(0).length == before.length);
  CHECK(rig.voices->sample(0).data == before.data);
  rig.silence();
  CHECK(rig.voices->voices_in_use() == 0u);
  CHECK_FALSE(rig.bank.poll(1000, true));  // quiet now: the settle starts here
  CHECK(rig.bank.poll(1000 + SampleBank::kSettleMs, true));
  CHECK(rig.voices->sample(0).length == 1500u);  // half of 3000
}

TEST_CASE("sample bank: a live note queued or held keeps the handover back and restarts the settle (OS-011a on OS-005g)") {
  Rig rig;
  rig.make(2, 4000);
  // Stopped and the pool silent, but a live note is queued (the pool does not hear it yet): nothing is written, however long.
  for (unsigned i = 0; i < 20; ++i) {
    CHECK_FALSE(rig.bank.poll(1000 + i * 10, true, false));
  }
  CHECK(rig.voices->sample(2).length == 0u);
  CHECK(rig.bank.rebind_pending());
  // It ended: the settle starts from this poll, not from one before the note.
  CHECK_FALSE(rig.bank.poll(2000, true, true));
  CHECK_FALSE(rig.bank.poll(2000 + SampleBank::kSettleMs - 1, true, false));  // a new note: the settle is over
  CHECK_FALSE(rig.bank.poll(3000, true, true));
  CHECK_FALSE(rig.bank.poll(3000 + SampleBank::kSettleMs - 1, true, true));
  CHECK(rig.voices->sample(2).length == 0u);
  CHECK(rig.bank.poll(3000 + SampleBank::kSettleMs, true, true));
  CHECK(rig.voices->sample(2).length == 4000u);
}

namespace {
// One quiet handover from `t` ms: the settle poll, then the writing one.
bool handover(Rig& rig, std::uint64_t t, bool jobs_idle = true) {
  (void)rig.bank.poll(t, true, true, jobs_idle);
  return rig.bank.poll(t + SampleBank::kSettleMs, true, true, jobs_idle);
}
// The audio `make()` writes: 1000 + (i + seed) % 50.
bool pcm_is_intact(const SampleBank& bank, unsigned slot, std::uint32_t frames, unsigned seed = 0) {
  const SampleBank::Pcm p = bank.pcm(slot);
  if (p.frames != frames || p.data == nullptr) {
    return false;
  }
  for (std::uint32_t i = 0; i < frames; ++i) {
    if (p.data[i] != static_cast<std::int16_t>(1000 + static_cast<int>((i + seed) % 50))) {
      return false;
    }
  }
  return true;
}
}  // namespace

TEST_CASE("sample bank: replacing a slot's loaded WAV gives the old block's room back at the handover, the audio moved down intact (OS-FU1)") {
  Rig rig;
  rig.make(3, 1000);
  CHECK(handover(rig, 100));
  CHECK(rig.bank.free_samples() == 19000u);
  CHECK(rig.bank.reclaims() == 0u);
  std::int16_t* const first = const_cast<std::int16_t*>(rig.bank.pcm(3).data);
  // Replace it by a shorter one: while the load is pending the old audio is where the engine reads it.
  rig.make(3, 600);
  CHECK(rig.bank.free_samples() == 19000u - 600u);
  CHECK(rig.bank.pcm(3).data == first + 1000);
  CHECK(rig.voices->sample(3).data == first);  // the engine still has the old descriptor
  const std::uint32_t v = rig.bank.version(3);
  CHECK(handover(rig, 1000));
  CHECK(rig.bank.reclaims() == 1u);
  CHECK(rig.bank.pcm(3).data == first);        // moved down over the old block
  CHECK(pcm_is_intact(rig.bank, 3, 600));
  CHECK(rig.bank.free_samples() == 20000u - 600u);
  CHECK(rig.bank.version(3) != v);             // a peak cache of the audio is stale
  CHECK(rig.voices->sample(3).data == first);  // the descriptor follows
  CHECK(rig.voices->sample(3).length == 600u);
  // And again, with a longer one than the first: the arena stays where the last block ends.
  rig.make(3, 1500);
  CHECK(handover(rig, 2000));
  CHECK(rig.bank.reclaims() == 2u);
  CHECK(rig.bank.pcm(3).data == first);
  CHECK(pcm_is_intact(rig.bank, 3, 1500));
  CHECK(rig.bank.free_samples() == 20000u - 1500u);
  CHECK(rig.voices->sample(3).length == 1500u);
}

TEST_CASE("sample bank: the reclaim waits for the playing voice like every rewrite, and for the jobs (OS-FU1)") {
  Rig rig;
  rig.make(0, 1000);
  CHECK(handover(rig, 100));
  rig.make(0, 800);
  // A job is running (it may read the audio): the descriptor is written as ever, the move waits.
  CHECK(handover(rig, 1000, false));
  CHECK(rig.voices->sample(0).data == rig.bank.pcm(0).data);
  CHECK(rig.bank.reclaims() == 0u);
  CHECK(rig.bank.free_samples() == 20000u - 1800u);
  // The job ended: the mark is still there; the next quiet handover moves it.
  CHECK(handover(rig, 2000, true));
  CHECK(rig.bank.reclaims() == 1u);
  CHECK(rig.bank.pcm(0).data == rig.memory.data());
  CHECK(pcm_is_intact(rig.bank, 0, 800));
  CHECK(rig.bank.free_samples() == 20000u - 800u);
  // A sounding voice holds the whole thing back, the move included.
  rig.make(0, 400);
  rig.voices->release_all();
  rig.play(0, 0);
  CHECK(rig.voices->voices_in_use() == 1u);
  for (unsigned t = 0; t < 10; ++t) {
    CHECK_FALSE(rig.bank.poll(3000 + t * 10, true));
  }
  CHECK(rig.bank.reclaims() == 1u);
  CHECK(rig.voices->sample(0).length == 800u);  // the old audio, unchanged under the voice
  rig.silence();
  CHECK(handover(rig, 4000));
  CHECK(rig.bank.reclaims() == 2u);
  CHECK(pcm_is_intact(rig.bank, 0, 400));
}

TEST_CASE("sample bank: no reclaim when the blocks are not side by side at the top, or the audio is too long to move (OS-FU1)") {
  {  // another slot's block in between, and the new audio longer than the old block: nowhere to go (OS-ARENA-RECLAIM: a shorter one goes into the old block)
    Rig rig;
    rig.make(0, 1000);
    rig.make(1, 500);
    rig.make(0, 1200);
    CHECK(handover(rig, 100));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(rig.bank.free_samples() == 20000u - 2700u);
    CHECK(pcm_is_intact(rig.bank, 0, 1200));
    CHECK(pcm_is_intact(rig.bank, 1, 500));
  }
  {  // something allocated after the new block (an edit's buffer, a slice table): it is not the last block
    Rig rig;
    rig.make(0, 1000);
    CHECK(handover(rig, 100));
    rig.make(0, 600);
    CHECK(rig.bank.alloc(10) != nullptr);
    CHECK(handover(rig, 1000));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(pcm_is_intact(rig.bank, 0, 600));
  }
  {  // a beat slice instrument's table is made at the handover, after the new audio
    Rig rig;
    rig.make(0, 1000);
    CHECK(handover(rig, 100));
    rig.make(0, 600, 5);  // play mode 5: beat slice
    rig.set_slices(0, 4);
    CHECK(handover(rig, 1000));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(pcm_is_intact(rig.bank, 0, 600));
  }
  {  // longer than one pass may move
    const std::uint32_t big = SampleBank::kReclaimMax + 10u;
    Rig rig(2u * big + 100u);
    rig.make(0, big);
    CHECK(handover(rig, 100));
    rig.make(0, big);
    CHECK(handover(rig, 1000));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(rig.bank.pcm(0).frames == big);
  }
  {  // the first load of a slot, or audio that a loader adopted, is nobody's to move
    Rig rig;
    std::int16_t* p = rig.memory.data();
    for (unsigned i = 0; i < 500; ++i) {
      p[i] = static_cast<std::int16_t>(1000 + static_cast<int>(i % 50));
    }
    rig.bank.adopt(5, p, 500);
    rig.bank.set_used(500);
    rig.make(5, 300);
    CHECK(handover(rig, 100));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(rig.bank.pcm(5).data == p + 500);
  }
}

TEST_CASE("sample bank: a project load (clear) forgets a pending reclaim (OS-FU1)") {
  Rig rig;
  rig.make(0, 1000);
  CHECK(handover(rig, 100));
  rig.make(0, 800);
  rig.bank.clear();
  CHECK_FALSE(handover(rig, 1000));
  CHECK(rig.bank.reclaims() == 0u);
  CHECK(rig.bank.free_samples() == 20000u);
}

namespace {
// The arena's invariants: every slot's audio lies inside the used part, and no two different blocks overlap.
bool arena_ok(const Rig& rig) {
  const std::int16_t* const base = rig.memory.data();
  const std::uint32_t top = rig.bank.arena_samples() - rig.bank.free_samples();
  for (unsigned a = 0; a < SampleBank::kSlots; ++a) {
    const SampleBank::Pcm pa = rig.bank.pcm(a);
    if (pa.frames == 0u) {
      continue;
    }
    const std::uint32_t ea = (pa.frames + 1u) & ~1u;
    if (pa.data < base || static_cast<std::uint32_t>(pa.data - base) + ea > top) {
      return false;
    }
    for (unsigned b = a + 1u; b < SampleBank::kSlots; ++b) {
      const SampleBank::Pcm pb = rig.bank.pcm(b);
      if (pb.frames != 0u && pb.data != pa.data && pb.data < pa.data + ea && pa.data < pb.data + ((pb.frames + 1u) & ~1u)) {
        return false;
      }
    }
  }
  return true;
}
}  // namespace

TEST_CASE("sample bank: a project's sample replaced again and again costs no arena while it fits its block, the neighbours untouched (OS-ARENA-RECLAIM)") {
  Rig rig;
  rig.loaded(1, 3000);
  rig.loaded(2, 2000);  // above slot 1's block: it is not the top
  CHECK(rig.bank.free_samples() == 15000u);
  std::int16_t* const block = const_cast<std::int16_t*>(rig.bank.pcm(1).data);
  const std::int16_t* const two = rig.bank.pcm(2).data;
  rig.make(1, 374);  // the unit's case: a short WAV over a long project sample
  CHECK(rig.bank.free_samples() == 15000u - 374u);  // on the tail until the handover
  const std::uint32_t v = rig.bank.version(1);
  CHECK(handover(rig, 100));
  CHECK(rig.bank.reclaims() == 1u);
  CHECK(rig.bank.pcm(1).data == block);  // down in the slot's own block
  CHECK(pcm_is_intact(rig.bank, 1, 374));
  CHECK(rig.bank.free_samples() == 15000u);  // the load cost nothing
  CHECK(rig.bank.version(1) != v);
  CHECK(rig.voices->sample(1).data == block);
  CHECK(rig.voices->sample(1).length == 374u);
  CHECK(rig.bank.pcm(2).data == two);
  CHECK(pcm_is_intact(rig.bank, 2, 2000));
  CHECK(arena_ok(rig));
  // Again, as long as the block (3000) holds it.
  for (std::uint32_t frames : {2999u, 1200u, 3000u}) {
    rig.make(1, frames);
    CHECK(handover(rig, 1000 + frames));
    CHECK(rig.bank.pcm(1).data == block);
    CHECK(pcm_is_intact(rig.bank, 1, frames));
    CHECK(rig.bank.free_samples() == 15000u);
    CHECK(arena_ok(rig));
  }
  CHECK(rig.bank.reclaims() == 4u);
  // Longer than the block, with another block above it: it stays on the tail, nothing moves, nothing is lost.
  rig.make(1, 3100);
  CHECK(handover(rig, 5000));
  CHECK(rig.bank.reclaims() == 4u);
  CHECK(rig.bank.free_samples() == 15000u - 3100u);
  CHECK(pcm_is_intact(rig.bank, 1, 3100));
  CHECK(pcm_is_intact(rig.bank, 2, 2000));
  CHECK(arena_ok(rig));
  // That block is the slot's now, at the top: the next load goes down over it as before (OS-FU1).
  rig.make(1, 3100);
  CHECK(handover(rig, 6000));
  CHECK(rig.bank.reclaims() == 5u);
  CHECK(rig.bank.free_samples() == 15000u - 3100u);
  CHECK(pcm_is_intact(rig.bank, 1, 3100));
  CHECK(arena_ok(rig));
}

TEST_CASE("sample bank: a block another holder reads is never overwritten or moved: clipboard, a paste, an undo record, a playing voice, a running job (OS-ARENA-RECLAIM)") {
  {  // the clipboard holds the slot's audio
    Rig rig;
    rig.loaded(1, 3000);
    rig.loaded(2, 2000);
    rig.bank.clip_set(*rig.arena->project.instrument(1), 1);
    std::int16_t* const block = const_cast<std::int16_t*>(rig.bank.pcm(1).data);
    rig.make(1, 374);
    CHECK(handover(rig, 100));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(rig.bank.pcm(1).data != block);
    CHECK(block[0] == 1000);  // the clipboard's block is as it was
    CHECK(block[2999] == static_cast<std::int16_t>(1000 + 2999 % 50));
    CHECK(rig.bank.free_samples() == 15000u - 374u);
    CHECK(arena_ok(rig));
  }
  {  // another slot reads it (a paste)
    Rig rig;
    rig.loaded(1, 3000);
    rig.loaded(2, 2000);
    rig.bank.clip_set(*rig.arena->project.instrument(1), 1);
    CHECK(rig.bank.paste_audio(3));
    rig.bank.clip_set(*rig.arena->project.instrument(2), 2);  // (the clipboard has let go: the other slot still reads it)
    rig.make(1, 374);
    CHECK(handover(rig, 100));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(pcm_is_intact(rig.bank, 3, 3000));
    CHECK(arena_ok(rig));
  }
  {  // an undoable load: its record names the old block and the new one
    Rig rig;
    rig.loaded(1, 3000);
    rig.loaded(2, 2000);
    const std::int16_t* const old_block = rig.bank.pcm(1).data;
    std::uint32_t cap = 0;
    std::int16_t* p = rig.bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < 374; ++i) {
      p[i] = static_cast<std::int16_t>(1000 + static_cast<int>(i % 50));
    }
    CHECK(rig.bank.commit_load_undoable(1, p, 374));
    CHECK(handover(rig, 100));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(rig.bank.audio_undone(1));  // the undo finds the old audio where it was
    CHECK(rig.bank.pcm(1).data == old_block);
    CHECK(pcm_is_intact(rig.bank, 1, 3000));
    CHECK(arena_ok(rig));
  }
  {  // a voice is playing the slot, then the job: the move waits for both, and the old audio is untouched meanwhile
    Rig rig;
    rig.loaded(1, 3000);
    rig.loaded(2, 2000);
    std::int16_t* const block = const_cast<std::int16_t*>(rig.bank.pcm(1).data);
    rig.play(0, 1);
    CHECK(rig.voices->voices_in_use() == 1u);
    rig.make(1, 374);
    for (unsigned t = 0; t < 10; ++t) {
      CHECK_FALSE(rig.bank.poll(100 + t * 10, true));
    }
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(rig.voices->sample(1).data == block);
    CHECK(block[1] == 1001);
    rig.silence();
    CHECK(handover(rig, 1000, false));  // a job runs
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(block[1] == 1001);
    CHECK(handover(rig, 2000, true));
    CHECK(rig.bank.reclaims() == 1u);
    CHECK(rig.bank.pcm(1).data == block);
    CHECK(pcm_is_intact(rig.bank, 1, 374));
    CHECK(arena_ok(rig));
  }
}

TEST_CASE("sample bank: an instrument deleted and the slot loaded again reuses the slot's block; a project load forgets it (OS-ARENA-RECLAIM)") {
  Rig rig;
  rig.loaded(1, 3000);
  rig.loaded(2, 2000);
  // (Delete does not tell the bank: the audio stays with the slot, so that its undo finds it.)
  rig.make(1, 800);
  CHECK(handover(rig, 100));
  CHECK(rig.bank.reclaims() == 1u);
  CHECK(rig.bank.free_samples() == 15000u);
  rig.bank.clear();
  CHECK(rig.bank.free_samples() == 20000u);
  rig.make(1, 800);
  rig.make(1, 700);
  CHECK(handover(rig, 1000));
  CHECK(rig.bank.reclaims() == 2u);  // clear() forgot the capacity; the plain OS-FU1 case still holds
  CHECK(arena_ok(rig));
}

TEST_CASE("sample bank: two or three loads before the handover still end in the slot's original block (OS-ARENA-RECLAIM)") {
  for (unsigned loads = 2; loads <= 3; ++loads) {
    Rig rig;
    rig.loaded(1, 3000);
    rig.loaded(2, 2000);
    const std::int16_t* const block = rig.bank.pcm(1).data;
    const std::uint32_t sizes[3] = {800u, 600u, 400u};
    for (unsigned i = 0; i < loads; ++i) {
      rig.make(1, sizes[i], 0, 1 + i);  // (the transport runs: no handover in between; each load has its own pattern)
      CHECK(rig.bank.pcm(1).data != block);
    }
    CHECK(handover(rig, 1000));
    CHECK(rig.bank.reclaims() == 1u);
    CHECK(rig.bank.pcm(1).data == block);  // the newest audio went into the original block, not the first load's
    CHECK(rig.bank.pcm(1).frames == sizes[loads - 1]);
    CHECK(pcm_is_intact(rig.bank, 1, sizes[loads - 1], loads));  // the newest load's audio was moved, not an older one's
    for (unsigned older = 0; older < loads; ++older) {
      CHECK_FALSE(pcm_is_intact(rig.bank, 1, sizes[loads - 1], older));  // (the project's own pattern and the earlier loads' differ)
    }
    CHECK(pcm_is_intact(rig.bank, 2, 2000));
    CHECK(rig.voices->sample(1).data == block);
    CHECK(arena_ok(rig));
    // The capacity is still the original 3000: the next load goes down again.
    rig.make(1, 2900, 0, 7);
    CHECK(handover(rig, 2000));
    CHECK(rig.bank.reclaims() == 2u);
    CHECK(rig.bank.pcm(1).data == block);
    CHECK(pcm_is_intact(rig.bank, 1, 2900, 7));
    CHECK(arena_ok(rig));
  }
}

TEST_CASE("sample bank: the move stops at kReclaimMax frames, a beat slice slot is refused, adopt() ignores a block that does not fit its audio (OS-ARENA-RECLAIM)") {
  {  // exactly kReclaimMax frames moves, two more do not
    for (std::uint32_t frames : {SampleBank::kReclaimMax, SampleBank::kReclaimMax + 2u}) {
      Rig rig(2u * frames + 100u);
      rig.loaded(0, frames);
      rig.loaded(1, 100);  // above slot 0's block: the new audio goes into the block, never down over it
      rig.make(0, frames, 0, 3);  // (a pattern unlike the project's: the moved audio is the new one)
      CHECK(handover(rig, 1000));
      CHECK(rig.bank.reclaims() == (frames == SampleBank::kReclaimMax ? 1u : 0u));
      CHECK(rig.bank.pcm(0).frames == frames);
      CHECK(pcm_is_intact(rig.bank, 0, frames, 3));
      CHECK(pcm_is_intact(rig.bank, 1, 100));
      CHECK(arena_ok(rig));
    }
  }
  {  // a beat slice instrument: its slice table is allocated after the new audio, so the new block is not the top
    Rig rig;
    rig.loaded(0, 1000);
    rig.loaded(1, 500);
    rig.make(0, 600, 5);
    rig.set_slices(0, 4);
    CHECK(handover(rig, 1000));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(pcm_is_intact(rig.bank, 0, 600));
    CHECK(pcm_is_intact(rig.bank, 1, 500));
    CHECK(arena_ok(rig));
  }
  {  // a block that is odd, shorter than the audio or larger than the arena is not claimed: the slot owns nothing, so a replace is never reclaimed
    const std::uint32_t blocks[3] = {1001u, 998u, 40000u};
    for (std::uint32_t block : blocks) {
      Rig rig;
      std::int16_t* const p = rig.memory.data();
      for (unsigned i = 0; i < 1000; ++i) {
        p[i] = static_cast<std::int16_t>(1000 + static_cast<int>(i % 50));
      }
      rig.bank.adopt(3, p, 1000, block);
      rig.bank.set_used(1000);
      rig.make(3, 300);
      CHECK(handover(rig, 1000));
      CHECK(rig.bank.reclaims() == 0u);
      CHECK(rig.bank.free_samples() == 20000u - 1000u - 300u);
      CHECK(pcm_is_intact(rig.bank, 3, 300));
      CHECK(arena_ok(rig));
    }
  }
}

TEST_CASE("sample bank: a slot with no sample instrument, or no audio, is made silent") {
  Rig rig;
  rig.make(1, 2000);
  rig.bank.rebind_now(1);
  CHECK(rig.voices->sample(1).length == 2000u);
  CHECK(rig.arena->project.clear_instrument(1) == model::Status::Ok);
  rig.bank.request_rebind(1);
  CHECK_FALSE(rig.bank.poll(10, true));
  CHECK(rig.bank.poll(10 + SampleBank::kSettleMs, true));
  CHECK(rig.voices->sample(1).length == 0u);
  // An instrument with no audio behind it.
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  CHECK(rig.arena->project.set_instrument(5, in) == model::Status::Ok);
  rig.bank.request_rebind(5);
  rig.bank.rebind_now(5);
  CHECK(rig.voices->sample(5).length == 0u);
}

TEST_CASE("sample bank: a beat slice instrument gets one slice table in the arena, made once and rewritten in place") {
  Rig rig;
  rig.make(4, 8000, 5);  // play mode 5: beat slice
  rig.set_slices(4, 4);
  const std::uint32_t free_before = rig.bank.free_samples();
  rig.bank.rebind_now(4);
  const audio::SampleDesc d = rig.voices->sample(4);
  CHECK(d.beat_slice);
  CHECK(d.slice_count == 4u);
  NEED(d.slices != nullptr);
  CHECK(rig.bank.free_samples() == free_before - SampleBank::kTableSamples);
  // The boundaries: slice k starts at floor(k * 65535 / 4 * 8000 / 65535) frames, as two 16-bit halves.
  const auto boundary = [&](unsigned i) { return static_cast<std::uint32_t>(d.slices[2 * i]) | (static_cast<std::uint32_t>(d.slices[2 * i + 1]) << 16); };
  CHECK(boundary(0) == 0u);
  CHECK(boundary(2) > 3900u);
  CHECK(boundary(2) <= 4000u);
  CHECK(boundary(4) == 8000u);
  // A second rebind (more slices) reuses the table: the arena does not grow.
  rig.set_slices(4, 9);
  rig.bank.rebind_now(4);
  CHECK(rig.voices->sample(4).slice_count == 9u);
  CHECK(rig.voices->sample(4).slices == d.slices);
  CHECK(rig.bank.free_samples() == free_before - SampleBank::kTableSamples);
}

TEST_CASE("sample bank: clear forgets every slot and the marks, and the version moves so that a peak cache of the old audio is stale") {
  Rig rig;
  rig.make(7, 1000);
  const std::uint32_t v = rig.bank.version(7);
  rig.bank.clear();
  CHECK(rig.bank.pcm(7).frames == 0u);
  CHECK(rig.bank.pcm(7).data == nullptr);
  CHECK(rig.bank.version(7) != v);
  CHECK_FALSE(rig.bank.rebind_pending());
  CHECK(rig.bank.free_samples() == 20000u);
}

TEST_CASE("sample bank: adopt and set_used are what a loader tells it; set_used never moves back") {
  Rig rig;
  rig.bank.adopt(9, rig.memory.data() + 10, 500);
  CHECK(rig.bank.pcm(9).frames == 500u);
  rig.bank.set_used(2001);
  CHECK(rig.bank.free_samples() == 20000u - 2002u);
  rig.bank.set_used(100);
  CHECK(rig.bank.free_samples() == 20000u - 2002u);
  rig.bank.adopt(9, nullptr, 500);  // no pointer: the slot is empty
  CHECK(rig.bank.pcm(9).frames == 0u);
  rig.bank.adopt(99, rig.memory.data(), 5);  // no such slot: ignored
}

// ---- The recorder's take (OS-011c) ------------------------------------------------------------------------------------------------------------

TEST_CASE("sample bank: a take holds the whole tail, gives back what it did not use, and is committed through the handover like a loaded WAV") {
  Rig rig;
  rig.make(0, 1000);  // a sample already in the arena
  const std::uint32_t used = 1000;
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.take_begin(cap);
  NEED(p != nullptr);
  CHECK(p == rig.memory.data() + used);
  CHECK(cap == 20000u - used);
  CHECK(rig.bank.take_held());
  // Nothing else can take arena memory while the render writes the take.
  CHECK(rig.bank.free_samples() == 0u);
  std::uint32_t other = 0;
  CHECK(rig.bank.reserve(other) == nullptr);
  CHECK(rig.bank.alloc(10) == nullptr);
  CHECK(rig.bank.take_begin(other) == nullptr);  // one take at a time
  // The take ends 5001 frames long: the arena is used up to its (even) end and the rest is free again.
  rig.bank.take_end(5001);
  CHECK_FALSE(rig.bank.take_held());
  CHECK(rig.bank.free_samples() == 20000u - used - 5002u);
  CHECK(rig.bank.take().data == p);
  CHECK(rig.bank.take().frames == 5001u);
  // Not committed: the next take goes over it (nothing was allocated after it).
  std::int16_t* q = rig.bank.take_begin(cap);
  CHECK(q == p);
  CHECK(cap == 20000u - used);
  rig.bank.take_end(3000);
  // A slot gets a window of it (an odd offset is rounded down to an even index: the audio stays word aligned).
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  CHECK(rig.arena->project.set_instrument(5, in) == model::Status::Ok);
  const std::uint32_t v0 = rig.bank.version(5);
  CHECK_FALSE(rig.bank.take_commit(5, 0, 0));
  CHECK_FALSE(rig.bank.take_commit(5, 2999, 3));  // past the end (2998 + 3)
  CHECK_FALSE(rig.bank.take_commit(5, 3000, 1));
  CHECK_FALSE(rig.bank.take_commit(SampleBank::kSlots, 0, 10));
  CHECK(rig.bank.take_commit(5, 101, 2000));
  CHECK(rig.bank.pcm(5).data == q + 100);
  CHECK(rig.bank.pcm(5).frames == 2000u);
  CHECK(rig.bank.version(5) != v0);
  CHECK(rig.bank.pending(5));  // the handover applies it when playback is stopped and the live notes are idle, never before
  CHECK(rig.bank.take().frames == 0u);  // the slot owns the take now
  // The next take starts after it, not over it.
  std::int16_t* r = rig.bank.take_begin(cap);
  CHECK(r == q + 3000);
  rig.bank.take_end(0);  // nothing was recorded: the tail is as it was
  CHECK(rig.bank.free_samples() == 20000u - used - 3000u);
  CHECK(rig.bank.take().frames == 0u);
}

// ---- OS-LOOPER L2a: a take with a reserved tail (the loop's undo buffer and bitmap) ---------------------------------------------------------

TEST_CASE("sample bank: the loop take's layout helpers (bitmap size, the capacity that fits a room)") {
  CHECK(SampleBank::undo_bitmap_samples(0) == 0u);
  CHECK(SampleBank::undo_bitmap_samples(1) == 2u);        // one block, one bit, one sample, even
  CHECK(SampleBank::undo_bitmap_samples(1024) == 2u);     // 16 blocks = 16 bits = one sample, even
  CHECK(SampleBank::undo_bitmap_samples(1025) == 2u);     // 17 blocks
  CHECK(SampleBank::undo_bitmap_samples(2048) == 2u);     // 32 blocks = two samples
  CHECK(SampleBank::undo_bitmap_samples(2049) == 4u);     // 33 blocks = three samples, even 4
  CHECK(SampleBank::undo_bitmap_samples(352800) == 346u); // 5513 blocks = 345.06 -> 346 samples
  // loop_capacity() is the largest c whose take, undo buffer and bitmap fit: 2 even(c) + bitmap(c) <= room.
  const auto fits = [](std::uint32_t c) { return 2u * ((c + 1u) & ~1u) + SampleBank::undo_bitmap_samples(c); };
  for (std::uint32_t room : {0u, 1u, 3u, 4u, 5u, 6u, 7u, 100u, 2047u, 2048u, 2049u, 20000u, 60000u, 99999u, 4194304u, 8000000u}) {
    const std::uint32_t c = SampleBank::loop_capacity(room);
    CHECK(fits(c) <= room);
    CHECK(fits(c + 1u) > room);
  }
  CHECK(SampleBank::loop_capacity(0) == 0u);
}

TEST_CASE("sample bank: a take with a tail keeps the tail from every other taker, and take_end(frames, 0) is unchanged") {
  Rig rig;
  rig.make(0, 1000);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.take_begin(cap);
  NEED(p != nullptr);
  rig.bank.take_end(3001, 3001 + 4);  // an odd loop: the take is 3002 samples, the tail 3006 (even 3006)
  CHECK(rig.bank.take().data == p);
  CHECK(rig.bank.take().frames == 3001u);  // take() reports the take alone
  CHECK(rig.bank.free_samples() == 20000u - 1000u - 3002u - 3006u);
  std::int16_t* const tail = p + 3002;  // where the undo buffer begins
  // reserve, alloc, an edit buffer and a slice table all land behind the tail.
  std::uint32_t room = 0;
  CHECK(rig.bank.reserve(room) == tail + 3006);
  CHECK(room == 20000u - 1000u - 3002u - 3006u);
  std::int16_t* a = rig.bank.alloc(10);
  CHECK(a == tail + 3006);
  rig.make(1, 600, 5);  // (reserve + commit_load: behind the tail as well)
  CHECK(rig.bank.pcm(1).data >= a + 10);
  rig.set_slices(1, 4);
  rig.bank.rebind_now(1);
  const audio::SampleDesc d = rig.voices->sample(1);
  NEED(d.slices != nullptr);
  CHECK(reinterpret_cast<const std::int16_t*>(d.slices) >= a + 10);
  const std::int16_t* e = rig.bank.edit_begin(1, 600);
  NEED(e != nullptr);
  CHECK(e >= a + 10);
  // Without a tail the arena is used exactly to the take's even end, as before.
  Rig plain;
  std::uint32_t c2 = 0;
  std::int16_t* q = plain.bank.take_begin(c2);
  NEED(q != nullptr);
  plain.bank.take_end(3001);
  CHECK(plain.bank.free_samples() == 20000u - 3002u);
  plain.bank.take_end(5, 5);  // not held: ignored
  CHECK(plain.bank.free_samples() == 20000u - 3002u);
}

TEST_CASE("sample bank: a take with a tail is given back whole on DISCARD while nothing came after it, and the next take starts over it") {
  Rig rig;
  rig.make(0, 1000);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.take_begin(cap);
  NEED(p != nullptr);
  rig.bank.take_end(2000, 2000 + 2);
  std::int16_t* q = rig.bank.take_begin(cap);
  CHECK(q == p);  // both the take and the tail are free again
  CHECK(cap == 20000u - 1000u);
  rig.bank.take_end(0);
  CHECK(rig.bank.free_samples() == 20000u - 1000u);
  // Something allocated after the tail while the loop was under review: the discarded take is not rewound (the bump allocator), and its tail is no longer anyone's.
  q = rig.bank.take_begin(cap);
  NEED(q != nullptr);
  rig.bank.take_end(2000, 2002);
  std::int16_t* a = rig.bank.alloc(10);
  CHECK(a == q + 2000 + 2002);
  std::int16_t* r = rig.bank.take_begin(cap);
  NEED(r != nullptr);
  CHECK(r == a + 10);
  CHECK(cap == 20000u - 1000u - 2000u - 2002u - 10u);
  // The tail of the dropped take does not leak into the new one.
  rig.bank.take_end(100);
  CHECK(rig.bank.free_samples() == cap - 100u);
}

// OS-BUG-MEMDISCARD: the free-space figure (the strip's MEM) gives a discarded take's room back at once, not at the next take.
TEST_CASE("sample bank: take_drop gives a discarded take and its tail back at once while nothing came after them, and never one that is held or was committed") {
  Rig rig;
  rig.make(0, 1000);
  const std::uint32_t before = rig.bank.free_samples();
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.take_begin(cap);
  NEED(p != nullptr);
  rig.bank.take_drop();  // held: not ours yet
  CHECK(rig.bank.free_samples() == 0u);
  rig.bank.take_end(2000, 2000 + 2);
  CHECK(rig.bank.free_samples() == before - 2000u - 2002u);
  rig.bank.take_drop();
  CHECK(rig.bank.free_samples() == before);
  CHECK(rig.bank.take().frames == 0u);  // forgotten
  rig.bank.take_drop();  // twice is nothing
  CHECK(rig.bank.free_samples() == before);
  // A note of the take pads may still read the take (live_idle false): the drop waits, and happens once the queue is idle.
  p = rig.bank.take_begin(cap);
  NEED(p != nullptr);
  rig.bank.take_end(700);
  rig.bank.take_drop(false);
  CHECK(rig.bank.free_samples() == before - 700u);
  CHECK(rig.bank.take().frames == 700u);  // still the live take
  rig.bank.take_drop(true);
  CHECK(rig.bank.free_samples() == before);
  CHECK(rig.bank.take_begin(cap) == p);  // and the next take starts over the same place
  CHECK(cap == before);
  rig.bank.take_end(0);
  // Something allocated after the take: the bump allocator does not rewind, the take is forgotten all the same.
  p = rig.bank.take_begin(cap);
  NEED(p != nullptr);
  rig.bank.take_end(500);
  std::int16_t* a = rig.bank.alloc(10);
  NEED(a != nullptr);
  const std::uint32_t after = rig.bank.free_samples();
  rig.bank.take_drop();
  CHECK(rig.bank.free_samples() == after);
  CHECK(rig.bank.take().frames == 0u);
  // A committed take is the slot's: nothing to drop.
  p = rig.bank.take_begin(cap);
  NEED(p != nullptr);
  rig.bank.take_end(600);
  const std::uint32_t kept = rig.bank.free_samples();
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  CHECK(rig.arena->project.set_instrument(1, in) == model::Status::Ok);
  CHECK(rig.bank.take_commit(1, 0, 600));
  rig.bank.take_drop();
  CHECK(rig.bank.free_samples() == kept);
  CHECK(rig.bank.pcm(1).data == p);
}

TEST_CASE("sample bank: SAVE trims the tail when nothing came after it, and leaks it (the take is committed) when something did") {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  {  // nothing after the tail: the arena shrinks back to the take's even end
    Rig rig;
    rig.make(0, 1000);
    std::uint32_t cap = 0;
    std::int16_t* p = rig.bank.take_begin(cap);
    NEED(p != nullptr);
    rig.bank.take_end(2001, 2001 + 4);
    CHECK(rig.bank.free_samples() == 20000u - 1000u - 2002u - 2006u);
    CHECK(rig.arena->project.set_instrument(5, in) == model::Status::Ok);
    CHECK(rig.bank.take_commit(5, 0, 2001));
    CHECK(rig.bank.pcm(5).data == p);
    CHECK(rig.bank.free_samples() == 20000u - 1000u - 2002u);  // the tail is free again
    std::int16_t* n = rig.bank.alloc(10);
    CHECK(n == p + 2002);  // ... and the next taker gets it
  }
  {  // a trimmed window commits as well, and the whole tail is still given back
    Rig rig;
    std::uint32_t cap = 0;
    std::int16_t* p = rig.bank.take_begin(cap);
    NEED(p != nullptr);
    rig.bank.take_end(2000, 2002);
    CHECK(rig.arena->project.set_instrument(5, in) == model::Status::Ok);
    CHECK(rig.bank.take_commit(5, 100, 500));
    CHECK(rig.bank.pcm(5).data == p + 100);
    CHECK(rig.bank.free_samples() == 20000u - 2000u);
  }
  {  // something was allocated after the tail during the review: the tail stays (the leaked cost is len + bitmap), the take is committed all the same
    Rig rig;
    std::uint32_t cap = 0;
    std::int16_t* p = rig.bank.take_begin(cap);
    NEED(p != nullptr);
    rig.bank.take_end(2000, 2000 + 4);
    std::int16_t* a = rig.bank.alloc(10);
    CHECK(a == p + 2000 + 2004);
    const std::uint32_t free_before = rig.bank.free_samples();
    CHECK(rig.arena->project.set_instrument(5, in) == model::Status::Ok);
    CHECK(rig.bank.take_commit(5, 0, 2000));
    CHECK(rig.bank.pcm(5).data == p);
    CHECK(rig.bank.pcm(5).frames == 2000u);
    CHECK(rig.bank.free_samples() == free_before);  // nothing came back: 2004 samples (the undo buffer and the bitmap) are lost until a project load
    CHECK(rig.bank.alloc(10) == a + 10);
    // the take is the slot's; a later take starts after everything
    std::uint32_t c = 0;
    std::int16_t* n = rig.bank.take_begin(c);
    CHECK(n == a + 20);
    rig.bank.take_end(0);
  }
}

TEST_CASE("sample bank: a tail larger than the arena is clamped, and a project load forgets the tail") {
  Rig rig(1000);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.take_begin(cap);
  NEED(p != nullptr);
  rig.bank.take_end(600, 5000);  // a caller that sized it wrongly stays inside the arena
  CHECK(rig.bank.free_samples() == 0u);
  CHECK(rig.bank.take().frames == 600u);
  rig.bank.clear();
  CHECK(rig.bank.free_samples() == 1000u);
  CHECK(rig.bank.take_begin(cap) == p);
  CHECK(cap == 1000u);  // the old tail does not shift the rewind test
  rig.bank.take_end(10);
  CHECK(rig.bank.free_samples() == 1000u - 10u);
  // an empty take has no tail
  std::int16_t* q = rig.bank.take_begin(cap);
  CHECK(q == p);
  rig.bank.take_end(0, 100);
  CHECK(rig.bank.free_samples() == 1000u);
}

TEST_CASE("sample bank: a recording never rewrites audio a voice may read, and the handover waits for it") {
  Rig rig;
  rig.make(0, 1000);
  rig.bank.rebind_now(0);  // the voices have slot 0's audio
  const audio::SampleDesc before = rig.voices->sample(0);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.take_begin(cap);
  NEED(p != nullptr);
  CHECK(p >= rig.memory.data() + 1000);  // fresh memory, behind everything a slot has
  for (std::uint32_t i = 0; i < 400; ++i) {
    p[i] = static_cast<std::int16_t>(i);
  }
  rig.bank.take_end(400);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  CHECK(rig.arena->project.set_instrument(0, in) == model::Status::Ok);
  CHECK(rig.bank.take_commit(0, 0, 400));
  // A voice is playing slot 0 (a live note or a pattern): the engine's descriptor is still the old one, valid memory, until the pool is quiet.
  rig.play(0, 0);
  CHECK_FALSE(rig.bank.poll(10, true, true));
  CHECK(rig.voices->sample(0).length == before.length);
  CHECK(rig.voices->sample(0).data == before.data);
  rig.silence();
  CHECK_FALSE(rig.bank.poll(1000, true, true));  // the settle starts
  CHECK(rig.bank.poll(1000 + SampleBank::kSettleMs, true, true));
  CHECK(rig.voices->sample(0).data != before.data);
  CHECK(rig.voices->sample(0).length == 400u);
  // While a take is held no handover is applied (a slice table would take the tail the take is writing into).
  CHECK(rig.bank.take_begin(cap) != nullptr);
  rig.bank.request_rebind(0);
  rig.silence();
  CHECK_FALSE(rig.bank.poll(2000, true, true));
  CHECK_FALSE(rig.bank.poll(2100, true, true));
  rig.bank.take_end(0);
  CHECK_FALSE(rig.bank.poll(3000, true, true));
  CHECK(rig.bank.poll(3000 + SampleBank::kSettleMs, true, true));
}

TEST_CASE("sample bank: a project load (clear) forgets a take in progress and a live one") {
  Rig rig;
  std::uint32_t cap = 0;
  CHECK(rig.bank.take_begin(cap) != nullptr);
  CHECK(rig.bank.take_held());
  rig.bank.clear();
  CHECK_FALSE(rig.bank.take_held());
  CHECK(rig.bank.free_samples() == 20000u);
  rig.bank.take_end(100);  // the recorder's late end is harmless
  CHECK(rig.bank.take().frames == 0u);
  CHECK(rig.bank.take_begin(cap) != nullptr);
  rig.bank.take_end(100);
  CHECK(rig.bank.take().frames == 100u);
  rig.bank.clear();
  CHECK(rig.bank.take().frames == 0u);
  CHECK_FALSE(rig.bank.take_commit(0, 0, 10));
}

TEST_CASE("sample bank: new_project(true) keeps the audio and forgets the old project's swaps, edit history and take, so the arena can be reclaimed (OS-NEWKEEP)") {
  Rig rig;
  rig.make(3, 1000);
  CHECK(handover(rig, 100));
  // An undoable load of a shorter audio into slot 3: the swap record names the old block, which the handover could not win back while the record lives.
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  for (std::uint32_t i = 0; i < 600; ++i) {
    p[i] = static_cast<std::int16_t>(1000 + static_cast<int>(i % 50));
  }
  CHECK(rig.bank.commit_load_undoable(3, p, 600));
  CHECK(rig.bank.pcm_refs(rig.memory.data()) == 1u);  // the swap record holds the old block
  // An edit of slot 3's audio (history), and a recorder take that has ended (live).
  const app::PointsRec points;
  std::int16_t* e = rig.bank.edit_begin(3, 600);
  NEED(e != nullptr);
  CHECK(rig.bank.edit_commit(3, e, 600, points, points));
  CHECK(rig.bank.undo_depth(3) == 1u);
  std::int16_t* t = rig.bank.take_begin(cap);
  NEED(t != nullptr);
  rig.bank.take_end(100);
  CHECK(rig.bank.take().frames == 100u);
  const std::int16_t* const audio3 = rig.bank.pcm(3).data;

  rig.bank.new_project(true);

  CHECK(rig.bank.pcm(3).data == audio3);  // the audio stays
  CHECK(rig.bank.pcm(3).frames == 600u);
  CHECK(rig.bank.card_version(3) == 0u);
  CHECK(rig.bank.undo_depth(3) == 0u);
  CHECK(rig.bank.redo_depth(3) == 0u);
  app::PointsRec restore;
  CHECK_FALSE(rig.bank.edit_undo(3, restore));
  CHECK_FALSE(rig.bank.audio_undone(3));  // no swap on record: the journal that would ask is gone
  CHECK_FALSE(rig.bank.audio_redone(3));
  CHECK(rig.bank.pcm_refs(rig.memory.data()) == 0u);
  CHECK_FALSE(rig.bank.take_held());
  CHECK(rig.bank.take().frames == 0u);
  CHECK_FALSE(rig.bank.take_commit(2, 0, 50));  // a take of the old project is no longer there to confirm
}

TEST_CASE("sample bank: after new_project(true) the block a swap record held is won back at the handover (OS-NEWKEEP)") {
  Rig rig;
  rig.make(3, 1000);
  CHECK(handover(rig, 100));
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  for (std::uint32_t i = 0; i < 600; ++i) {
    p[i] = static_cast<std::int16_t>(1000 + static_cast<int>(i % 50));
  }
  CHECK(rig.bank.commit_load_undoable(3, p, 600));  // the record names the old 1000-sample block: it cannot be reclaimed while it lives
  rig.bank.new_project(true);
  CHECK(handover(rig, 1000));
  CHECK(rig.bank.reclaims() == 1u);
  CHECK(pcm_is_intact(rig.bank, 3, 600));
  CHECK(rig.bank.free_samples() == 20000u - 600u);
}

TEST_CASE("sample bank: a take on a full arena is refused, and an import in progress refuses it too") {
  Rig rig(2000);
  rig.make(0, 2000);
  std::uint32_t cap = 0;
  CHECK(rig.bank.take_begin(cap) == nullptr);
  CHECK(cap == 0u);
  CHECK_FALSE(rig.bank.take_held());
}

TEST_CASE("sample bank: the playback engine of a slot: Amiga at a Paula rate by default, an override wins, and the user can switch the default off") {
  Rig rig;
  rig.make(0, 1000);
  rig.make(1, 1000);
  rig.make(2, 1000, 4);  // a slice instrument: never Amiga
  for (unsigned slot = 0; slot < 2; ++slot) {
    model::Instrument in = *rig.arena->project.instrument(slot);
    in.sample_rate = slot == 0 ? 8287u : 44100u;  // the PAL Paula rate of period 428 (what the MED import and a module sample carry), and a plain 44.1 kHz sample
    in.sampler.volume = 75;
    CHECK(rig.arena->project.set_instrument(slot, in) == model::Status::Ok);
  }
  {
    model::Instrument in = *rig.arena->project.instrument(2);
    in.sample_rate = 8363u;
    CHECK(rig.arena->project.set_instrument(2, in) == model::Status::Ok);
  }
  rig.bank.request_rebind(0);
  rig.bank.request_rebind(1);
  rig.bank.request_rebind(2);
  CHECK_FALSE(rig.bank.poll(1000, true));
  CHECK(rig.bank.poll(1000 + SampleBank::kSettleMs, true));
  CHECK(rig.voices->sample(0).engine == audio::kEngineAmiga);
  CHECK(rig.voices->sample(0).amiga_vol == 64);   // level 75 is +12 dB: over Paula's full scale, held to 64 (OS-BUG-PASTE24; it was 75 / 100 of 64 = 48)
  CHECK(rig.voices->sample(1).engine == audio::kEngineModern);
  CHECK(rig.voices->sample(2).engine == audio::kEngineModern);
  rig.bank.set_engine(1, audio::kEngineBits8);    // an override, whatever the rate
  rig.bank.set_amiga_by_rate(false);              // the maintainer's switch: every slot Modern unless overridden
  CHECK_FALSE(rig.bank.poll(5000, true));
  CHECK(rig.bank.poll(5000 + SampleBank::kSettleMs, true));
  CHECK(rig.voices->sample(0).engine == audio::kEngineModern);
  CHECK(rig.voices->sample(1).engine == audio::kEngineBits8);
  rig.bank.set_engine(1, SampleBank::kEngineAuto);
  rig.bank.set_amiga_by_rate(true);
  CHECK_FALSE(rig.bank.poll(9000, true));
  CHECK(rig.bank.poll(9000 + SampleBank::kSettleMs, true));
  CHECK(rig.voices->sample(0).engine == audio::kEngineAmiga);
  CHECK(rig.voices->sample(1).engine == audio::kEngineModern);
}

// OS-004d: the instrument's own choice (model::Instrument::engine, INST 1.2) is the engine the voices get; the runtime override and the default of an automatic instrument are as before.
TEST_CASE("sample bank: an instrument that chose its engine plays on it, whatever its rate; a runtime override still wins (OS-004d)") {
  Rig rig;
  rig.make(0, 1000);
  rig.make(1, 1000);
  rig.make(2, 1000);
  rig.make(3, 1000);
  const std::uint8_t chosen[4] = {model::kInstEngineModern, model::kInstEngineBits12, model::kInstEngineBits8, model::kInstEngineAmiga};
  for (unsigned slot = 0; slot < 4; ++slot) {
    model::Instrument in = *rig.arena->project.instrument(slot);
    in.sample_rate = slot == 3 ? 44100u : 8287u;  // slot 0 would be Amiga by its rate; slot 3 would be Modern
    in.engine = chosen[slot];
    CHECK(rig.arena->project.set_instrument(slot, in) == model::Status::Ok);
    rig.bank.request_rebind(slot);
  }
  CHECK_FALSE(rig.bank.poll(1000, true));
  CHECK(rig.bank.poll(1000 + SampleBank::kSettleMs, true));
  CHECK(rig.voices->sample(0).engine == audio::kEngineModern);
  CHECK(rig.voices->sample(1).engine == audio::kEngineBits12);
  CHECK(rig.voices->sample(2).engine == audio::kEngineBits8);
  CHECK(rig.voices->sample(3).engine == audio::kEngineAmiga);
  rig.bank.set_engine(0, audio::kEngineBits8);  // the override beats the instrument's choice
  CHECK_FALSE(rig.bank.poll(5000, true));
  CHECK(rig.bank.poll(5000 + SampleBank::kSettleMs, true));
  CHECK(rig.voices->sample(0).engine == audio::kEngineBits8);
  rig.bank.set_engine(0, SampleBank::kEngineAuto);
  {
    model::Instrument in = *rig.arena->project.instrument(0);
    in.engine = model::kInstEngineAuto;  // automatic again: the rate decides (Paula 8,287 Hz plays as one shot: Amiga)
    CHECK(rig.arena->project.set_instrument(0, in) == model::Status::Ok);
  }
  rig.bank.request_rebind(0);
  CHECK_FALSE(rig.bank.poll(9000, true));
  CHECK(rig.bank.poll(9000 + SampleBank::kSettleMs, true));
  CHECK(rig.voices->sample(0).engine == audio::kEngineAmiga);
}

// ---- OS-BUG-SLICEPITCH: the staged descriptor -----------------------------------------------------------------------------------------------------

TEST_CASE("sample bank: a chop reaches the next note while a voice sounds and the transport runs; a voice on the same audio rings on") {
  Rig rig;
  rig.make(2, 4000);
  rig.bank.rebind_now(2);
  CHECK(rig.voices->sample(2).slice_count == 0u);
  rig.play(0, 2);
  NEED(rig.voices->voices_in_use() == 1u);
  // EVEN 4: the instrument is BEAT SLICE at once, the engine is busy (the transport runs and a voice sounds).
  model::Instrument in = *rig.arena->project.instrument(2);
  in.sampler.play_mode = 5;
  CHECK(rig.arena->project.set_instrument(2, in) == model::Status::Ok);
  rig.set_slices(2, 4);
  rig.bank.request_rebind(2);
  CHECK(rig.bank.rebind_pending());
  CHECK_FALSE(rig.bank.poll(100, /*stopped*/ false));  // nothing written for the caller to redraw ...
  CHECK(rig.voices->stage_busy());                     // ... the descriptor is staged
  CHECK_FALSE(rig.voices->sample(2).beat_slice);     // the render has not taken it yet
  rig.render(1);
  CHECK(rig.voices->sample(2).beat_slice);
  CHECK(rig.voices->sample(2).slice_count == 4u);
  CHECK(rig.voices->voices_in_use() == 1u);          // same audio and length: the voice on it was not cut
  CHECK_FALSE(rig.bank.poll(101, false));            // the take is seen
  CHECK_FALSE(rig.bank.rebind_pending());            // nothing is waiting for the sound any more ...
  CHECK(rig.bank.pending(2));                        // ... the quiet handover still owes the bookkeeping
  // A note of the chopped slot starts on the new descriptor: slice 3 of 4, not the whole sample at its note's pitch.
  rig.voices->begin_block();
  CHECK(rig.voices->note_on(1, audio::kSliceRootNote + 3u, 2, audio::kVelocityDefault));
  rig.voices->render(0, audio::VoicePool::kBlock);
  rig.voices->end_block();
  const audio::SampleDesc& d = rig.voices->sample(2);
  NEED(d.slices != nullptr);
  CHECK((audio::slice_bound(d.slices, 3) >= 2999u && audio::slice_bound(d.slices, 3) <= 3001u));
  // The quiet handover follows when it comes, and leaves the same descriptor.
  rig.silence();
  CHECK_FALSE(rig.bank.poll(200, true));
  CHECK(rig.bank.poll(206, true));
  CHECK_FALSE(rig.bank.pending(2));
  CHECK(rig.voices->sample(2).slice_count == 4u);
  CHECK((audio::slice_bound(rig.voices->sample(2).slices, 3) >= 2999u && audio::slice_bound(rig.voices->sample(2).slices, 3) <= 3001u));
}

TEST_CASE("sample bank: a staged descriptor with other audio ends the slot's sounding voices, and nobody reads the old audio from the new descriptor") {
  Rig rig;
  rig.make(2, 4000);
  rig.bank.rebind_now(2);
  const std::int16_t* old_pcm = rig.voices->sample(2).data;
  rig.play(0, 2);
  NEED(rig.voices->voices_in_use() == 1u);
  rig.make(2, 3000);  // a load over the slot: other audio, other length
  CHECK_FALSE(rig.bank.poll(100, false));
  CHECK(rig.voices->stage_busy());
  rig.render(1);
  CHECK(rig.voices->sample(2).data != old_pcm);
  CHECK(rig.voices->sample(2).length == 3000u);
  rig.render(3);
  CHECK(rig.voices->voices_in_use() == 0u);  // the voice ended with its declick tail
  CHECK_FALSE(rig.bank.poll(101, false));    // the take is seen
  // The old descriptor's block stays the slot's `bound` audio until the quiet handover: nothing may overwrite it meanwhile.
  CHECK(rig.bank.rebind_pending() == false);
  CHECK(rig.bank.poll(200, true) == false);
  CHECK(rig.bank.poll(206, true));
}

TEST_CASE("sample bank: a staged descriptor nobody takes is given back after kStageTimeoutMs, and the quiet handover writes it as before") {
  Rig rig;
  rig.make(2, 4000);
  rig.bank.rebind_now(2);
  rig.make(2, 3000);
  CHECK_FALSE(rig.bank.poll(100, false));
  CHECK(rig.voices->stage_busy());                       // staged; no render runs
  CHECK(rig.voices->sample(2).length == 4000u);
  CHECK(rig.bank.rebind_pending());                      // still waiting: the cell was not taken
  CHECK_FALSE(rig.bank.poll(100 + SampleBank::kStageTimeoutMs + 1u, false));
  CHECK_FALSE(rig.voices->stage_busy());                 // given back
  CHECK(rig.voices->sample(2).length == 4000u);
  CHECK_FALSE(rig.bank.poll(300, true));
  CHECK(rig.bank.poll(300 + SampleBank::kSettleMs, true));
  CHECK(rig.voices->sample(2).length == 3000u);
  rig.render(2);                                         // a late take finds nothing: the cell is free
  CHECK(rig.voices->sample(2).length == 3000u);
}

TEST_CASE("sample bank: a direct write takes back the cell staged for its slot, and the ParamChange ring (params_idle) holds the stage back") {
  Rig rig;
  rig.make(2, 4000);
  rig.bank.rebind_now(2);
  rig.make(2, 3000);
  CHECK_FALSE(rig.bank.poll(100, false, true, true, /*params_idle*/ false));  // a change is queued: nothing is staged
  CHECK_FALSE(rig.voices->stage_busy());
  CHECK_FALSE(rig.bank.poll(101, false));
  CHECK(rig.voices->stage_busy());
  rig.bank.rebind_now(2);                                // the project's descriptor, now
  CHECK_FALSE(rig.voices->stage_busy());
  rig.render(1);
  CHECK(rig.voices->sample(2).length == 3000u);
}

// ---- OS-STEREO step 2, slice (b): the side signal S follows M in the same block --------------------------------------------------------------------

namespace {
// A stereo sample in slot `slot`: M as make() writes it, S[i] = -(i % 7) - 1, committed through commit_load with S as one unit.
void make_stereo(Rig& rig, unsigned slot, std::uint32_t frames) {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  in.stereo = 1;
  CHECK(rig.arena->project.set_instrument(slot, in) == model::Status::Ok);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  const std::uint32_t even = (frames + 1u) & ~1u;
  for (std::uint32_t i = 0; i < frames; ++i) {
    p[i] = static_cast<std::int16_t>(1000 + static_cast<int>(i % 50));
    p[even + i] = static_cast<std::int16_t>(-static_cast<int>(i % 7) - 1);
  }
  CHECK(rig.bank.commit_load(slot, p, frames, p + even));
}
bool side_is_intact(const SampleBank& bank, unsigned slot, std::uint32_t frames) {
  const SampleBank::Pcm p = bank.pcm(slot);
  if (p.frames != frames || p.side == nullptr || p.side != p.data + ((frames + 1u) & ~1u)) {
    return false;
  }
  for (std::uint32_t i = 0; i < frames; ++i) {
    if (p.side[i] != static_cast<std::int16_t>(-static_cast<int>(i % 7) - 1)) {
      return false;
    }
  }
  return true;
}
}  // namespace

TEST_CASE("sample bank: a load with a side signal commits M and S as one block of 2 x even(frames); a mono load has none") {
  Rig rig(5000);
  rig.make(0, 100);
  CHECK_FALSE(rig.bank.has_side(0));
  CHECK(rig.bank.pcm(0).side == nullptr);
  make_stereo(rig, 1, 999);
  CHECK(rig.bank.has_side(1));
  CHECK(rig.bank.free_samples() == 5000u - 100u - 2000u);  // 999 rounds up to 1000, twice
  CHECK(pcm_is_intact(rig.bank, 1, 999));                  // M is what a mono load writes
  CHECK(side_is_intact(rig.bank, 1, 999));
  CHECK_FALSE(rig.bank.has_side(2));
  // Refusals change nothing: S not where it must be, or no room for both.
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  CHECK_FALSE(rig.bank.commit_load(2, p, 100, p + 50));
  CHECK_FALSE(rig.bank.commit_load(2, p, 100, p + 102));
  CHECK_FALSE(rig.bank.commit_load(2, p, 1500, p + 1500));  // M fits (1500 <= 2900) but both (3000) do not
  CHECK(rig.bank.pcm(2).frames == 0u);
  CHECK(rig.bank.free_samples() == 5000u - 100u - 2000u);
  // clear() forgets it.
  rig.bank.clear();
  CHECK_FALSE(rig.bank.has_side(1));
}

TEST_CASE("sample bank: adopt takes S only at pcm + even(frames) inside the arena; the block it owns covers both") {
  Rig rig;
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  rig.bank.adopt(1, p, 100, 200, p + 50);  // not the second half: ignored, a mono sample
  CHECK_FALSE(rig.bank.has_side(1));
  rig.bank.adopt(1, p, 100, 200, p + 100);
  CHECK(rig.bank.has_side(1));
  CHECK(rig.bank.pcm(1).side == p + 100);
  rig.bank.adopt(2, p + 200, 100, 100, p + 300);  // the block (100) is too small for both: S is taken, the block is not the slot's to move
  CHECK(rig.bank.has_side(2));
  rig.bank.adopt(3, p + 400, 100, 0, nullptr);
  CHECK_FALSE(rig.bank.has_side(3));
  rig.bank.adopt(1, p, 100, 100);  // adopted again as mono: S is gone
  CHECK_FALSE(rig.bank.has_side(1));
}

TEST_CASE("sample bank: RECLAIM never moves a block with S, and wins back a stereo block a mono load replaced (OS-STEREO (b), rule 5)") {
  {  // a stereo load over a mono block: not moved, both blocks intact
    Rig rig;
    rig.make(3, 1000);
    CHECK(handover(rig, 100));
    make_stereo(rig, 3, 600);
    CHECK(handover(rig, 1000));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(side_is_intact(rig.bank, 3, 600));
    CHECK(rig.bank.free_samples() == 20000u - 1000u - 1200u);
  }
  {  // a stereo load over a stereo block: not moved
    Rig rig;
    make_stereo(rig, 3, 500);
    CHECK(handover(rig, 100));
    make_stereo(rig, 3, 400);
    CHECK(handover(rig, 1000));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(side_is_intact(rig.bank, 3, 400));
  }
  {  // a mono load over a stereo block: the whole old block (M and S) is the room, the audio moves down intact and S is gone
    Rig rig;
    make_stereo(rig, 3, 500);
    CHECK(handover(rig, 100));
    std::int16_t* const first = const_cast<std::int16_t*>(rig.bank.pcm(3).data);
    rig.make(3, 600);
    CHECK(handover(rig, 1000));
    CHECK(rig.bank.reclaims() == 1u);
    CHECK(rig.bank.pcm(3).data == first);
    CHECK(pcm_is_intact(rig.bank, 3, 600));
    CHECK_FALSE(rig.bank.has_side(3));
    CHECK(rig.bank.free_samples() == 20000u - 600u);
    CHECK(arena_ok(rig));
  }
  {  // the clipboard reads the stereo block (through S as well): a mono load does not overwrite it
    Rig rig;
    rig.loaded(1, 100);
    make_stereo(rig, 3, 500);
    rig.bank.clip_set(*rig.arena->project.instrument(3), 3);
    const SampleBank::Pcm held = rig.bank.pcm(3);
    rig.make(3, 300);
    CHECK(handover(rig, 100));
    CHECK(rig.bank.reclaims() == 0u);
    CHECK(held.side[0] == -1);
    CHECK(held.side[499] == static_cast<std::int16_t>(-static_cast<int>(499 % 7) - 1));
    CHECK(held.data[499] == static_cast<std::int16_t>(1000 + 499 % 50));
  }
}

TEST_CASE("sample bank: a paste shares M and S; undo and redo of a load or an edit give the slot its S back") {
  Rig rig;
  make_stereo(rig, 1, 500);
  CHECK(handover(rig, 100));
  rig.bank.clip_set(*rig.arena->project.instrument(1), 1);
  CHECK(rig.bank.paste_audio(2));
  CHECK(rig.bank.has_side(2));
  CHECK(rig.bank.pcm(2).side == rig.bank.pcm(1).side);
  CHECK(rig.bank.audio_undone(2));  // the paste's undo: the slot is empty again
  CHECK_FALSE(rig.bank.has_side(2));
  CHECK(rig.bank.audio_redone(2));
  CHECK(side_is_intact(rig.bank, 2, 500));
  // An undoable (mono) load over the stereo slot drops S; its undo brings it back, its redo drops it again.
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  for (std::uint32_t i = 0; i < 300; ++i) {
    p[i] = 7;
  }
  CHECK(rig.bank.commit_load_undoable(1, p, 300));
  CHECK_FALSE(rig.bank.has_side(1));
  CHECK(rig.bank.audio_undone(1));
  CHECK(side_is_intact(rig.bank, 1, 500));
  CHECK(rig.bank.audio_redone(1));
  CHECK_FALSE(rig.bank.has_side(1));
  CHECK(rig.bank.audio_undone(1));
  CHECK(side_is_intact(rig.bank, 1, 500));
  // A sample edit's result is mono; the edit's undo and redo carry S.
  app::PointsRec rec;
  std::int16_t* buf = rig.bank.edit_begin(1, 500);
  NEED(buf != nullptr);
  for (std::uint32_t i = 0; i < 500; ++i) {
    buf[i] = 3;
  }
  CHECK(rig.bank.edit_commit(1, buf, 500, rec, rec));
  CHECK_FALSE(rig.bank.has_side(1));
  CHECK(rig.bank.edit_undo(1, rec));
  CHECK(side_is_intact(rig.bank, 1, 500));
  CHECK(rig.bank.edit_redo(1, rec));
  CHECK_FALSE(rig.bank.has_side(1));
  CHECK(rig.bank.edit_undo(1, rec));
  CHECK(side_is_intact(rig.bank, 1, 500));
  // The blocks of the history are not overwritten by a later handover: S is still what it was.
  CHECK(handover(rig, 2000));
  CHECK(side_is_intact(rig.bank, 1, 500));
}

TEST_CASE("sample bank: reconcile_stereo clears the flag of an instrument whose slot has no S, and keeps one that has it") {
  Rig rig;
  make_stereo(rig, 1, 200);  // flag set, S there
  rig.make(2, 200);
  model::Instrument in = *rig.arena->project.instrument(2);
  in.stereo = 1;  // flag set, no S
  CHECK(rig.arena->project.set_instrument(2, in) == model::Status::Ok);
  CHECK(rig.bank.reconcile_stereo() == 1u);
  CHECK(rig.arena->project.instrument(1)->stereo == 1u);
  CHECK(rig.arena->project.instrument(2)->stereo == 0u);
  CHECK(rig.bank.reconcile_stereo() == 0u);
}
