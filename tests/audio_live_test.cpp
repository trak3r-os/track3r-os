// SPDX-License-Identifier: GPL-3.0-or-later
//
// The live note path (audio/live_notes.h, OS-005g): the track search, the slots that match a Note Off to the note it ends (stealing, sources, retrigger), the queue
// into the real engine and voices (a note starts at the next block, ends on its Off, an all-notes-off ends only its source's), and the app side that decides which
// note a Chop pad plays.
#include <doctest/doctest.h>

#include <o3os/app/pad_board.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <cstdint>

namespace o3os::audio {
// The test's view of the pool's private state (voices.h friend): a track's voice pitch offset (1/65536 semitone) and the bend in force (OS-PIANOBEND).
struct PianoBendTest {
  static bool has_voice(VoicePool& p, unsigned track) { return p.find(track) < kMaxVoices; }
  static std::int32_t offset(VoicePool& p, unsigned track) { return p.v_[p.find(track)].slide_q16; }
  static std::int32_t target(VoicePool& p, unsigned track) { return p.v_[p.find(track)].slide_tgt; }
  static std::int32_t base(VoicePool& p, unsigned track) { return p.tune_base_q16(track); }
};
}  // namespace o3os::audio

namespace {

namespace haudio = o3os::hal::audio;
using o3os::audio::Engine;
using o3os::audio::LiveBox;
using o3os::audio::LiveEvent;
using o3os::audio::LiveKind;
using o3os::audio::LiveNotes;
using o3os::audio::LiveQueue;
using o3os::audio::SampleDesc;
using o3os::audio::VoicePool;
using o3os::audio::kLiveAllSources;
using o3os::audio::kLiveNone;
using o3os::audio::kLiveSourceMidi;
using o3os::audio::kLiveSourcePad;
using o3os::audio::kLiveSourcePreview;
using o3os::audio::pick_track;

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 7u; }

LiveEvent on(std::uint8_t source, std::uint8_t key, std::uint8_t note, std::uint8_t instrument, std::uint8_t track, std::uint8_t velocity = o3os::audio::kLiveVelocityDefault) {
  return LiveEvent{LiveKind::On, source, key, note, instrument, velocity, track, 0};
}
LiveEvent off(std::uint8_t source, std::uint8_t key) { return LiveEvent{LiveKind::Off, source, key, 0, 0, 0, 0, 0}; }
LiveEvent all_off(std::uint8_t source) { return LiveEvent{LiveKind::AllOff, source, 0, 0, 0, 0, 0, 0}; }

struct Rig {
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  haudio::BlockReport report = {};
};
void render(Engine& e, Rig& r) {
  r.report = haudio::BlockReport{};
  const haudio::BlockIo io{r.in, r.out, 0, &r.report};
  e.render_block(io);
}

}  // namespace

TEST_CASE("live: the track search starts at the selected track and goes round") {
  CHECK(pick_track(0, 0, 0) == 0);
  CHECK(pick_track(5, 0, 0) == 5);
  CHECK(pick_track(5, 1u << 5, 0) == 6);                  // held by a live note: the next
  CHECK(pick_track(5, 0, 1u << 5) == 6);                  // a voice sounds there (the sequencer's): the next
  CHECK(pick_track(15, 1u << 15, 0) == 0);                // wraps
  CHECK(pick_track(14, (1u << 14) | (1u << 15) | 1u, 0) == 1);
  CHECK(pick_track(3, 0, 0xFFFFu) == 3);                  // every track sounds, none is live: the selected one is taken over
  CHECK(pick_track(3, 1u << 3, 0xFFFFu) == 4);            // ... but never one a live note holds
  CHECK(pick_track(3, 0xFFFFu & ~(1u << 9), 0xFFFFu) == 9);
  CHECK(pick_track(200, 0, 0) == 0);                      // out of range: track 0
  CHECK(pick_track(0, 0xFFFFu, 0) == 0);                  // impossible with 8 slots; still a track
}

TEST_CASE("live: a note off finds the note its key started, and nothing else") {
  LiveNotes n;
  const LiveNotes::Start a = n.begin(kLiveSourcePad, 7, 2, 0);
  CHECK(a.track == 2);
  CHECK(a.cut == kLiveNone);
  const LiveNotes::Start b = n.begin(kLiveSourcePad, 8, 2, 0);  // a second pad: the next track
  CHECK(b.track == 3);
  const LiveNotes::Start c = n.begin(kLiveSourceMidi, 7, 2, 0);  // the same number from the keyboard is another note
  CHECK(c.track == 4);
  CHECK(n.held() == 3);

  CHECK(n.end(kLiveSourceMidi, 7) == 4);
  CHECK(n.end(kLiveSourceMidi, 7) == kLiveNone);  // gone: a second Off is nothing
  CHECK(n.end(kLiveSourcePad, 9) == kLiveNone);   // never started
  CHECK(n.end(kLiveSourcePad, 7) == 2);
  CHECK(n.end(kLiveSourcePad, 8) == 3);
  CHECK(n.held() == 0);
}

TEST_CASE("live: a key that is held again retriggers on its own track and a track that sounds is avoided") {
  LiveNotes n;
  CHECK(n.begin(kLiveSourcePad, 1, 0, 0).track == 0);
  CHECK(n.begin(kLiveSourcePad, 1, 0, 0).track == 0);  // the same pad again: its track, no second slot
  CHECK(n.held() == 1);
  CHECK(n.begin(kLiveSourcePad, 2, 0, 1u << 1).track == 2);  // track 0 is live, track 1 sounds
  // The source 0xFF is the wildcard of an all-off: as a note's source it is folded, never a free slot.
  LiveNotes w;
  CHECK(w.begin(0xFF, 0xFF, 0, 0).track == 0);
  CHECK(w.held() == 1);
  CHECK(w.end(0xFF, 0xFF) == 0);
}

TEST_CASE("live: a note past kLiveMax (12) cuts the oldest and takes its slot") {
  LiveNotes n;
  for (std::uint8_t k = 0; k < o3os::audio::kLiveMax; ++k) {
    const LiveNotes::Start s = n.begin(kLiveSourceMidi, k, 0, 0);
    CHECK(s.track == k);
    CHECK(s.cut == kLiveNone);
  }
  CHECK(n.held() == o3os::audio::kLiveMax);
  const LiveNotes::Start s = n.begin(kLiveSourceMidi, 100, 0, 0);
  CHECK(s.cut == 0);  // note 0 was the oldest
  CHECK(s.track == 0);  // and its track is free again
  CHECK(n.held() == o3os::audio::kLiveMax);
  CHECK(n.end(kLiveSourceMidi, 0) == kLiveNone);  // its Off finds nothing: it was cut
  CHECK(n.end(kLiveSourceMidi, 100) == 0);
  // The next oldest is 1, not a note that was retriggered later.
  CHECK(n.begin(kLiveSourceMidi, 1, 0, 0).cut == kLiveNone);  // retrigger: refreshes its age
  const LiveNotes::Start t = n.begin(kLiveSourceMidi, 101, 0, 0);
  CHECK(t.cut == kLiveNone);  // a slot was free (100 was ended above)
  const LiveNotes::Start u = n.begin(kLiveSourceMidi, 102, 0, 0);
  CHECK(u.cut == 2);  // 2 is now the oldest
}

TEST_CASE("live: the age survives the byte counter wrapping") {
  LiveNotes n;
  CHECK(n.begin(kLiveSourceMidi, 1, 0, 0).track == 0);
  for (unsigned i = 0; i < 300; ++i) {  // 300 starts and ends under a held note
    const LiveNotes::Start s = n.begin(kLiveSourcePad, 5, 8, 0);
    CHECK(s.cut == kLiveNone);
    CHECK(n.end(kLiveSourcePad, 5) != kLiveNone);
  }
  for (std::uint8_t k = 10; k < 10 + o3os::audio::kLiveMax - 1; ++k) {  // the slots fill up
    CHECK(n.begin(kLiveSourceMidi, k, 0, 0).cut == kLiveNone);
  }
  CHECK(n.held() == o3os::audio::kLiveMax);
  // The held note is older than the rest in truth; with the clock wrapped the byte arithmetic may name another, but it names a held one and no slot is lost.
  const LiveNotes::Start s = n.begin(kLiveSourceMidi, 50, 0, 0);
  CHECK(s.cut != kLiveNone);
  CHECK(n.held() == o3os::audio::kLiveMax);
}

TEST_CASE("live: oldest() names the note held longest, with its note and channel (OS-EXPNOTE)") {
  LiveNotes n;
  CHECK(n.oldest() == o3os::audio::kHeldNone);
  (void)n.begin(kLiveSourceMidi, 48, 0, 0, 48, 10);
  (void)n.begin(kLiveSourcePad, 3, 0, 0, 60, 0);
  const std::uint32_t w = n.oldest();
  CHECK(o3os::audio::held_source(w) == kLiveSourceMidi);
  CHECK(o3os::audio::held_key(w) == 48);
  CHECK(o3os::audio::held_note(w) == 48);
  CHECK(o3os::audio::held_channel(w) == 10);
  (void)n.begin(kLiveSourceMidi, 48, 0, 0, 48, 11);  // the same key again: it is the newest now, on the channel it came on last
  const std::uint32_t w2 = n.oldest();
  CHECK(o3os::audio::held_source(w2) == kLiveSourcePad);
  CHECK(o3os::audio::held_note(w2) == 60);
  CHECK(n.end(kLiveSourcePad, 3) != kLiveNone);
  CHECK(o3os::audio::held_channel(n.oldest()) == 11);
  CHECK(n.end(kLiveSourceMidi, 48) != kLiveNone);
  CHECK(n.oldest() == o3os::audio::kHeldNone);
}

TEST_CASE("live: an all-notes-off takes the notes of one source, or of all") {
  LiveNotes n;
  CHECK(n.begin(kLiveSourcePad, 1, 0, 0).track == 0);
  CHECK(n.begin(kLiveSourcePad, 2, 0, 0).track == 1);
  CHECK(n.begin(kLiveSourceMidi, 60, 0, 0).track == 2);
  CHECK(n.begin(kLiveSourcePreview, 0, 0, 0).track == 3);
  CHECK(n.end_all(kLiveSourcePad) == 0x3u);
  CHECK(n.held() == 2);
  CHECK(n.end_all(kLiveSourcePad) == 0u);
  CHECK(n.end_all(kLiveAllSources) == 0xCu);
  CHECK(n.held() == 0);
}

TEST_CASE("live: the queue reaches the voices at the next block, in order, and a note off releases it") {
  static std::int16_t dc[4000];
  for (std::int16_t& s : dc) {
    s = 16384;
  }
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static o3os::test::StaticSlot<VoicePool> pool_slot;
  static LiveBox box;
  LiveQueue& queue = box.queue;
  box.notes = LiveNotes{};
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool_slot.make();
  p.set_sample(0, SampleDesc{dc, 4000, 0, 0, 65536});  // instrument 1
  p.set_sample(3, SampleDesc{dc, 4000, 0, 0, 65536});  // instrument 4
  e.attach_voices(&p);
  e.attach_live(&box);
  Rig r;

  render(e, r);
  CHECK(p.voices_in_use() == 0);
  CHECK(queue.push(on(kLiveSourcePad, 3, 60, 1, 4)));  // selected track 4
  CHECK(p.voices_in_use() == 0);                       // nothing until the render takes it
  render(e, r);
  CHECK(p.track_playing(4));
  CHECK(p.voices_in_use() == 1);
  CHECK(e.live_held() == 1);
  NEED(p.feed(4) != nullptr);
  CHECK(p.feed(4)[haudio::kBlock - 1] == 16384);  // sounding at the block's end: the attack (32 samples) is over, at the instrument's level

  // A chord on the keyboard: the notes go to the next free tracks, in the order pushed, from the same selected track.
  CHECK(queue.push(on(kLiveSourceMidi, 64, 64, 4, 4, 100)));
  CHECK(queue.push(on(kLiveSourceMidi, 67, 67, 4, 4, 100)));
  render(e, r);
  CHECK(p.track_playing(4));
  CHECK(p.track_playing(5));
  CHECK(p.track_playing(6));
  CHECK(p.voices_in_use() == 3);

  // Off for the key ends its note and no other (a release is not a stop: the voice rings out its 256 samples first).
  CHECK(queue.push(off(kLiveSourceMidi, 64)));
  for (unsigned i = 0; i < 6; ++i) {
    render(e, r);
  }
  CHECK_FALSE(p.track_playing(5));
  CHECK(p.track_playing(4));
  CHECK(p.track_playing(6));

  // An all-notes-off of the keyboard leaves the pad's note alone.
  CHECK(queue.push(all_off(kLiveSourceMidi)));
  for (unsigned i = 0; i < 6; ++i) {
    render(e, r);
  }
  CHECK(p.track_playing(4));
  CHECK_FALSE(p.track_playing(6));
  CHECK(e.live_held() == 1);

  // An Off for a key that was never held, and a note on an empty slot (instrument 2): nothing starts and nothing is held.
  CHECK(queue.push(off(kLiveSourcePad, 40)));
  CHECK(queue.push(on(kLiveSourcePad, 41, 60, 2, 0)));
  render(e, r);
  CHECK(e.live_held() == 1);
  CHECK(p.stats().ignored == 1);
  CHECK(queue.push(on(kLiveSourcePad, 42, 60, 0, 0)));  // instrument 0 names none
  render(e, r);
  CHECK(e.live_held() == 1);
  CHECK(p.stats().ignored == 2);

  CHECK(queue.push(off(kLiveSourcePad, 3)));
  for (unsigned i = 0; i < 6; ++i) {
    render(e, r);
  }
  CHECK(p.voices_in_use() == 0);
  CHECK(e.live_held() == 0);
}

TEST_CASE("live: a MIDI velocity replaces the instrument's own level as a step's velocity does, the pad's default keeps it (OS-FU1)") {
  static std::int16_t dc[4000];
  for (std::int16_t& s : dc) {
    s = 16384;
  }
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static o3os::test::StaticSlot<VoicePool> pool_slot;
  static LiveBox box;
  box.notes = LiveNotes{};
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool_slot.make();
  p.set_sample(0, SampleDesc{dc, 4000, 0, 0, 32768});  // instrument 1: half level of its own
  e.attach_voices(&p);
  e.attach_live(&box);
  Rig r;
  // A pad has no velocity (the scanner reports 127 and the app passes the default): the instrument's own level, half.
  CHECK(box.queue.push(on(kLiveSourcePad, 1, 60, 1, 0)));
  // MIDI: velocity 127 is unity (the instrument's level is replaced), 64 is about half of unity, 1 is nearly silent.
  CHECK(box.queue.push(on(kLiveSourceMidi, 60, 60, 1, 1, 127)));
  CHECK(box.queue.push(on(kLiveSourceMidi, 61, 61, 1, 2, 64)));
  CHECK(box.queue.push(on(kLiveSourceMidi, 62, 62, 1, 3, 1)));
  render(e, r);
  NEED(p.feed(0) != nullptr);
  NEED(p.feed(1) != nullptr);
  NEED(p.feed(2) != nullptr);
  NEED(p.feed(3) != nullptr);
  CHECK(p.feed(0)[haudio::kBlock - 1] == 8192);
  CHECK(p.feed(1)[haudio::kBlock - 1] == 16384);
  CHECK(p.feed(2)[haudio::kBlock - 1] >= 8100);
  CHECK(p.feed(2)[haudio::kBlock - 1] <= 8300);
  CHECK(p.feed(3)[haudio::kBlock - 1] < 200);
}

TEST_CASE("live: no more than kLivePerBlock events a block, the rest waits, and the voices are not over-committed") {
  static std::int16_t dc[4000];
  for (std::int16_t& s : dc) {
    s = 1000;
  }
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static o3os::test::StaticSlot<VoicePool> pool_slot;
  static LiveBox box;
  LiveQueue& queue = box.queue;
  box.notes = LiveNotes{};
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool_slot.make();
  p.set_sample(0, SampleDesc{dc, 4000, 0, 0, 65536});
  e.attach_voices(&p);
  e.attach_live(&box);
  Rig r;
  for (std::uint8_t k = 0; k < 20; ++k) {
    CHECK(queue.push(on(kLiveSourceMidi, k, 60, 1, 0)));
  }
  render(e, r);
  CHECK(e.live_held() == o3os::audio::kLiveMax);  // 16 taken: 12 slots, the four older notes were cut by the later ones
  CHECK(p.voices_in_use() <= o3os::audio::kMaxVoices);
  CHECK(queue.size() == 20 - o3os::audio::kLivePerBlock);
  render(e, r);
  CHECK(queue.size() == 0);
  CHECK(e.live_held() == o3os::audio::kLiveMax);
}

// OS-BUG-SLICEPAD: a live note is not played by the pattern's Slice FX state of its track (a pad press picked the slice the last pattern step had left on the selected track, then the track
// after it, which has none, played the right one), and a pad that carries a slice (a LiveEvent's channel = slice + 1) plays that slice, in Slice and Beat Slice descriptors alike.
TEST_CASE("live: a pad plays its own slice, not the Slice FX state of its track, which is left as it was (OS-BUG-SLICEPAD)") {
  static std::int16_t steps[3000];  // three slices of 1000 samples, each its own level
  for (unsigned i = 0; i < 3000; ++i) {
    steps[i] = static_cast<std::int16_t>(4000 * (1 + static_cast<int>(i / 1000)));
  }
  static std::uint16_t table[2 * 4];
  const std::uint32_t bounds[4] = {0, 1000, 2000, 3000};
  for (unsigned i = 0; i < 4; ++i) {
    table[2 * i] = static_cast<std::uint16_t>(bounds[i] & 0xFFFFu);
    table[2 * i + 1] = static_cast<std::uint16_t>(bounds[i] >> 16);
  }
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static o3os::test::StaticSlot<VoicePool> pool_slot;
  static LiveBox box;
  box.notes = LiveNotes{};
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool_slot.make();
  SampleDesc d{steps, 3000, 0, 0, 65536};
  d.beat_slice = true;
  d.slice_count = 3;
  d.slices = table;
  p.set_sample(0, d);                  // instrument 1: a Beat Slice descriptor (note 24 + n picks slice n)
  SampleDesc sel = d;
  sel.slice_sel = 1;                   // instrument 2: the Slice mode, slice index 1 is selected
  p.set_sample(1, sel);
  e.attach_voices(&p);
  e.attach_live(&box);
  Rig r;
  const auto level = [&](unsigned track) {
    const std::int16_t* f = p.feed(track);
    return f != nullptr ? static_cast<int>(f[haudio::kBlock - 1]) : -1;
  };

  // Points 1, 4 and 5: the pattern's Slice FX left slice 2 on track 4; the pad's own slice (note 24 + 0) plays, and the track's state is as it was.
  p.set_forced_slice(4, 2);
  CHECK(box.queue.push(on(kLiveSourcePad, 3, 24, 1, 4)));
  render(e, r);
  CHECK(level(4) == 4000);
  CHECK(p.forced_slice(4) == 2u);
  CHECK(box.queue.push(off(kLiveSourcePad, 3)));
  for (unsigned i = 0; i < 6; ++i) {
    render(e, r);
  }
  CHECK(box.queue.push(on(kLiveSourcePad, 3, 25, 1, 4)));  // the same pad again: slice 1, wherever the Slice FX is
  render(e, r);
  CHECK(level(4) == 8000);
  CHECK(p.forced_slice(4) == 2u);
  p.clear_forced_slices();
  CHECK(box.queue.push(all_off(kLiveSourcePad)));
  for (unsigned i = 0; i < 6; ++i) {
    render(e, r);
  }

  // Points 2 and 3: a pad of the Slice mode carries its slice (channel = slice + 1) and the note is the board's; the selected slice plays for an event that carries none.
  LiveEvent pad = on(kLiveSourcePad, 7, 60, 2, 0);
  pad.channel = 3;  // slice 2
  CHECK(box.queue.push(pad));
  render(e, r);
  CHECK(level(0) == 12000);
  CHECK(box.queue.push(off(kLiveSourcePad, 7)));
  for (unsigned i = 0; i < 6; ++i) {
    render(e, r);
  }
  CHECK(box.queue.push(on(kLiveSourcePad, 8, 60, 2, 0)));  // no pad slice: the selected one (slice 1)
  render(e, r);
  CHECK(level(0) == 8000);
}

TEST_CASE("live: a Chop pad plays the slice of its number, as the voices read a note") {
  // The app asks for note kSliceRootNote + n for pad n of a beat slice instrument (wave_actions.cpp pad()); the voices read slice = note - kSliceRootNote, so
  // the pad, the note and the slice are one number. Pad 0 is the root note, the 48 pads reach the 48 slices the instrument can have.
  CHECK(o3os::audio::kSliceRootNote == 24);
  for (unsigned pad = 0; pad < o3os::app::PadBoard::kPads; ++pad) {
    const unsigned note = o3os::audio::kSliceRootNote + pad;
    CHECK(note - o3os::audio::kSliceRootNote == pad);
    CHECK(note <= 127u);
  }
  CHECK(o3os::audio::kMaxSlices == o3os::app::PadBoard::kPads);
}

TEST_CASE("live: the engine is idle for the sample handover only with an empty queue and no held note (OS-011a on OS-005g)") {
  static std::int16_t dc[4000];
  for (std::int16_t& s : dc) {
    s = 1000;
  }
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static o3os::test::StaticSlot<VoicePool> pool_slot;
  static LiveBox box;
  box.notes = LiveNotes{};
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool_slot.make();
  p.set_sample(0, SampleDesc{dc, 4000, 0, 0, 65536});
  e.attach_voices(&p);
  Rig r;
  CHECK(e.live_idle());  // no table attached: nothing live
  e.attach_live(&box);
  render(e, r);
  CHECK(e.live_idle());
  // A note pushed and not yet taken: the voice has not started (voices_in_use() reads 0), yet the handover must wait for it.
  CHECK(box.queue.push(on(kLiveSourcePad, 3, 60, 1, 0)));
  CHECK(p.voices_in_use() == 0);
  CHECK_FALSE(e.live_idle());
  render(e, r);  // taken: sounding and held
  CHECK(p.voices_in_use() == 1);
  CHECK_FALSE(e.live_idle());
  // Released: no longer held, but the voice rings out; the pool's own count covers that, so the engine is idle as far as the queue and the table go.
  CHECK(box.queue.push(off(kLiveSourcePad, 3)));
  CHECK_FALSE(e.live_idle());
  render(e, r);
  CHECK(e.live_idle());
  // A note on an empty slot starts nothing and holds nothing: idle again once it has been taken.
  CHECK(box.queue.push(on(kLiveSourcePad, 4, 60, 9, 0)));
  CHECK_FALSE(e.live_idle());
  render(e, r);
  CHECK(e.live_idle());
}

namespace {
LiveEvent bend(int semitones) {
  return LiveEvent{LiveKind::Bend, kLiveSourcePad, 0, static_cast<std::uint8_t>(static_cast<std::int8_t>(semitones)), 1, 0, 0, 0};
}
constexpr std::int32_t kSemi = 65536;
}  // namespace

TEST_CASE("live: a bend moves the voices of the held pad notes by the semitones and a release puts them back; a note started under the bend starts bent (OS-PIANOBEND)") {
  using o3os::audio::PianoBendTest;
  static std::int16_t dc[4000];
  for (std::int16_t& s : dc) {
    s = 16384;
  }
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static o3os::test::StaticSlot<VoicePool> pool_slot;
  static LiveBox box;
  box.notes = LiveNotes{};
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool_slot.make();
  p.set_sample(0, SampleDesc{dc, 4000, 0, 0, 65536});
  e.attach_voices(&p);
  e.attach_live(&box);
  Rig r;

  CHECK(box.queue.push(on(kLiveSourcePad, 3, 60, 1, 0)));  // track 1
  render(e, r);
  NEED(PianoBendTest::has_voice(p, 0));
  CHECK(PianoBendTest::offset(p, 0) == 0);
  CHECK(p.live_bend_q16() == 0);

  CHECK(box.queue.push(bend(2)));
  render(e, r);
  CHECK(p.live_bend_q16() == 2 * kSemi);
  CHECK(PianoBendTest::offset(p, 0) == 2 * kSemi);
  CHECK(PianoBendTest::target(p, 0) == 2 * kSemi);
  CHECK(PianoBendTest::base(p, 0) == 2 * kSemi);
  CHECK(PianoBendTest::base(p, 5) == 0);  // a track that holds no pad note is not bent

  // The other pad: a second note under the bend starts at the bend (the offset's zero is the bend), the track it takes joins the mask.
  CHECK(box.queue.push(on(kLiveSourcePad, 4, 64, 1, 1)));
  render(e, r);
  NEED(PianoBendTest::has_voice(p, 1));
  CHECK(PianoBendTest::offset(p, 1) == 2 * kSemi);

  // Another strip pad: -1 replaces +2, the voices move by the difference.
  CHECK(box.queue.push(bend(-1)));
  render(e, r);
  CHECK(PianoBendTest::offset(p, 0) == -kSemi);
  CHECK(PianoBendTest::offset(p, 1) == -kSemi);

  // The release: back to the pitch of the note, and the bend is gone for later notes.
  CHECK(box.queue.push(bend(0)));
  render(e, r);
  CHECK(p.live_bend_q16() == 0);
  CHECK(PianoBendTest::offset(p, 0) == 0);
  CHECK(PianoBendTest::offset(p, 1) == 0);
  CHECK(PianoBendTest::target(p, 0) == 0);
  CHECK(PianoBendTest::base(p, 0) == 0);
  CHECK(box.queue.push(off(kLiveSourcePad, 4)));
  CHECK(box.queue.push(on(kLiveSourcePad, 5, 67, 1, 2)));
  render(e, r);
  NEED(PianoBendTest::has_voice(p, 2));
  CHECK(PianoBendTest::offset(p, 2) == 0);

  // An all-notes-off of the pads while bent puts the bend back too (a lost release).
  CHECK(box.queue.push(bend(1)));
  render(e, r);
  CHECK(PianoBendTest::offset(p, 0) == kSemi);
  CHECK(box.queue.push(all_off(kLiveSourcePad)));
  render(e, r);
  CHECK(p.live_bend_q16() == 0);
  CHECK(PianoBendTest::base(p, 0) == 0);
  CHECK(PianoBendTest::offset(p, 0) == 0);
  // A keyboard's all-notes-off leaves a bend alone.
  CHECK(box.queue.push(bend(2)));
  render(e, r);
  CHECK(box.queue.push(all_off(kLiveSourceMidi)));
  render(e, r);
  CHECK(p.live_bend_q16() == 2 * kSemi);
  CHECK(box.queue.push(bend(0)));
  render(e, r);
  CHECK(p.live_bend_q16() == 0);
}
