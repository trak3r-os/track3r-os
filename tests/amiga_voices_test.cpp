// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Amiga voices (audio/amiga_voices.h, OS-005h): a note of an Amiga instrument through the pool and through the engine, the held sample and the volume, the pitch of the
// table, the MED commands driven by Tick events, the cut and the steal, the filters, and the bit-reduction engines of the sampler pool. The Paula model itself is held against
// pt2-clone in paula_oracle_test.cpp, the tick processor against libxmp in med_tick_test.cpp. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/amiga_voices.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/event_source.h>
#include <o3os/audio/paula.h>
#include <o3os/audio/voices.h>
#include <o3os/core/audio_clock.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

using o3os::audio::AmigaVoices;
using o3os::audio::Engine;
using o3os::audio::EventKind;
using o3os::audio::SampleDesc;
using o3os::audio::SeqEvent;
using o3os::audio::VoicePool;
namespace haudio = o3os::hal::audio;
namespace audio = o3os::audio;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr std::uint8_t kEngineA = audio::kEngineAmiga;

// A looped sample the Paula way: 8 bits in the top byte. `value` is the int8 of every frame.
struct Wave {
  std::int16_t d[600];
  explicit Wave(int value) {
    for (std::int16_t& s : d) {
      s = static_cast<std::int16_t>(value * 256);
    }
  }
};
// A ramp: frame i holds i (as int8), over 120 frames.
struct Ramp {
  std::int16_t d[128];
  Ramp() {
    for (int i = 0; i < 128; ++i) {
      d[i] = static_cast<std::int16_t>(i * 256);
    }
  }
};

SampleDesc desc_of(const std::int16_t* data, std::uint32_t len, bool loop, std::uint8_t engine = kEngineA) {
  SampleDesc d;
  d.data = data;
  d.length = len;
  d.loop_start = 0;
  d.loop_end = loop ? len : 0u;
  d.engine = engine;
  return d;
}

SeqEvent ev(EventKind kind, unsigned track, unsigned a = 0, unsigned b = 0, unsigned c = 0, unsigned flags = 0) {
  SeqEvent e{};
  e.kind = kind;
  e.track = static_cast<std::uint8_t>(track);
  e.a = static_cast<std::uint8_t>(a);
  e.b = static_cast<std::uint8_t>(b);
  e.c = static_cast<std::uint8_t>(c);
  e.flags = static_cast<std::uint8_t>(flags);
  return e;
}
SeqEvent note_on(unsigned track, unsigned note, unsigned slot, unsigned vel = audio::kVelocityDefault) { return ev(EventKind::NoteOn, track, note, slot, vel); }
SeqEvent engine_fx(unsigned track, unsigned cmd, unsigned data, unsigned col = 0) {
  return ev(EventKind::ModSet, track, 128u + cmd, data, audio::kLayerFx1, col == 0u ? audio::kModGroupEngine0 : audio::kModGroupEngine1);
}
SeqEvent tick(unsigned i, unsigned ticks) { return ev(EventKind::Tick, audio::kNoTrack, i, ticks); }
SeqEvent trk_fx(unsigned track, unsigned type, unsigned value, unsigned col = 0) { return ev(EventKind::TrkFx, track, type, value, 0, col); }

struct Rig {
  AmigaVoices* a;
  VoicePool* p;
  std::int16_t blk[16][kBlk] = {};  // the feeds of the last block (zeros where there is none)
  bool fed[16] = {};
  Rig(AmigaVoices* av, VoicePool* pv) : a(av), p(pv) {}

  void block() {
    a->begin_block();
    a->render(0, kBlk);
    a->end_block();
    for (unsigned t = 0; t < 16; ++t) {
      const std::int16_t* f = a->feed(t);
      fed[t] = f != nullptr;
      for (std::uint32_t i = 0; i < kBlk; ++i) {
        blk[t][i] = f != nullptr ? f[i] : std::int16_t{0};
      }
    }
  }
  void blocks(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      block();
    }
  }
  bool send(const SeqEvent& e) { return a->event(e, *p); }
};

AmigaVoices& pool_a() {
  static o3os::test::StaticSlot<AmigaVoices> slot;
  return slot.make();
}
VoicePool& pool_p() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}

}  // namespace

TEST_CASE("amiga voices: a held sample at volume 64 is steady at its level, the velocity and the instrument's volume scale it") {
  static Wave w(64);  // int8 64: at volume 64 the sample is 64 * 64 / 8192 = 0.5, Q15 16384
  Rig r(&pool_a(), &pool_p());
  r.a->set_filter(audio::paula::Model::Off, false);
  r.p->set_sample(0, desc_of(w.d, 600, true));
  CHECK(r.send(note_on(0, 60, 0, 127)));  // velocity 127: (127 + 1) / 2 = 64
  r.blocks(2);
  CHECK(r.fed[0]);
  CHECK(r.blk[0][kBlk - 1] == 16384);
  CHECK(r.send(note_on(1, 60, 0, 63)));   // 32
  r.blocks(2);
  CHECK(r.blk[1][kBlk - 1] == 8192);
  SampleDesc d = desc_of(w.d, 600, true);
  d.amiga_vol = 40;
  r.p->set_sample(1, d);
  CHECK(r.send(note_on(2, 60, 1)));       // no velocity: the instrument's own, 40
  r.blocks(2);
  CHECK(r.blk[2][kBlk - 1] == 10240);
  CHECK(r.a->voices_in_use() == 3);
}

TEST_CASE("amiga voices: the pitch is the ProTracker period, sample and hold") {
  static Ramp ramp;
  Rig r(&pool_a(), &pool_p());
  r.a->set_filter(audio::paula::Model::Off, false);
  r.p->set_sample(0, desc_of(ramp.d, 128, false));
  CHECK(r.send(note_on(0, 60, 0, 127)));  // period 428: one sample every 5.3237 output samples
  std::int16_t seen[8 * kBlk];
  for (unsigned b = 0; b < 8; ++b) {
    r.block();
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      seen[b * kBlk + i] = r.blk[0][i];
    }
  }
  // After 400 output samples 400 * 0.18783 = 75.1 samples have been fetched: the held value is frame 75 (Q15: 256 a frame), within the BLEP's ripple.
  CHECK(std::abs(seen[400] - 75 * 256) <= 2 * 256);
  // An octave up (note 72, period 214) goes twice as far.
  CHECK(r.send(note_on(1, 72, 0, 127)));
  for (unsigned b = 0; b < 4; ++b) {
    r.block();
  }
  CHECK(r.a->track_state(1).period() == 214);
  CHECK(r.a->track_state(0).period() == 428);
}

TEST_CASE("amiga voices: the Amiga engine ignores the sample's rate factor (OS-RATEPITCH): its pitch is the period's, whatever the descriptor says") {
  static Ramp ramp;
  std::int16_t seen[2][8 * kBlk];
  int period[2] = {};
  for (unsigned run = 0; run < 2; ++run) {
    Rig r(&pool_a(), &pool_p());
    r.a->set_filter(audio::paula::Model::Off, false);
    SampleDesc d = desc_of(ramp.d, 128, false);
    d.rate_q28 = run == 0 ? 0u : audio::rate_factor_q28(22050);  // the second is a 22,050 Hz sample, which a Modern voice would play at half the pitch
    r.p->set_sample(0, d);
    CHECK(r.send(note_on(0, 60, 0, 127)));
    for (unsigned b = 0; b < 8; ++b) {
      r.block();
      for (std::uint32_t i = 0; i < kBlk; ++i) {
        seen[run][b * kBlk + i] = r.blk[0][i];
      }
    }
    period[run] = r.a->track_state(0).period();
  }
  CHECK(period[0] == 428);
  CHECK(period[1] == 428);
  CHECK(std::memcmp(seen[0], seen[1], sizeof seen[0]) == 0);
}

TEST_CASE("amiga voices: the MED commands of the Tick events move the period and the volume") {
  static Wave w(64);
  Rig r(&pool_a(), &pool_p());
  r.p->set_sample(0, desc_of(w.d, 600, true));
  CHECK(r.send(engine_fx(0, 0x01, 4)));  // slide up 4 a tick
  CHECK(r.send(note_on(0, 60, 0, 127)));
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  // the note's tick 0 ran in the NoteOn (-4), the Tick 0 of its row was skipped, ticks 1..5 follow: 428 - 6 * 4
  CHECK(r.a->track_state(0).period() == 404);
  CHECK(r.send(engine_fx(0, 0x0D, 0x02)));  // volume slide down 2 a tick
  const unsigned before = r.a->track_state(0).volume();
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  CHECK(r.a->track_state(0).volume() == before - 12);
  // the commands end with a ModEnd: nothing moves any more
  SeqEvent end = ev(EventKind::ModEnd, 0, 0, 0, 0, audio::kModGroupEngine0);
  CHECK(r.send(end));
  const unsigned p0 = r.a->track_state(0).period();
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  CHECK(r.a->track_state(0).period() == p0);
}

TEST_CASE("amiga voices: events of other instruments and other kinds are left to the sampler pool") {
  static Wave w(10);
  Rig r(&pool_a(), &pool_p());
  r.p->set_sample(0, desc_of(w.d, 600, true, audio::kEngineModern));
  CHECK_FALSE(r.send(note_on(0, 60, 0, 100)));  // Modern: not taken
  CHECK(r.a->voices_in_use() == 0);
  CHECK_FALSE(r.send(ev(EventKind::ModSet, 0, 27, 50, 0, audio::kModGroupFilter)));  // the Cutoff FX is the sampler's
  CHECK_FALSE(r.send(ev(EventKind::Step, audio::kNoTrack)));
  CHECK(r.send(tick(0, 6)));                    // a Tick is this pool's
  CHECK_FALSE(r.send(note_on(0, 60, 63, 100)));  // an empty slot is not an Amiga one
}

TEST_CASE("amiga voices: a note off, a cut and the transport stop end the voices, the ring runs dry and the feed is quiet") {
  static Wave w(64);
  Rig r(&pool_a(), &pool_p());
  r.a->set_filter(audio::paula::Model::Off, false);
  r.p->set_sample(0, desc_of(w.d, 600, true));
  for (unsigned t = 0; t < 3; ++t) {
    CHECK(r.send(note_on(t, 60, 0, 127)));
  }
  r.blocks(3);
  CHECK(r.a->voices_in_use() == 3);
  CHECK_FALSE(r.send(ev(EventKind::NoteOff, 0)));
  CHECK_FALSE(r.send(ev(EventKind::NoteCut, 1)));
  CHECK_FALSE(r.send(ev(EventKind::Stopped, audio::kNoTrack)));
  r.blocks(3);
  CHECK(r.a->voices_in_use() == 0);
  for (unsigned t = 0; t < 3; ++t) {
    CHECK(r.blk[t][kBlk - 1] == 0);
  }
}

TEST_CASE("amiga voices: nine tracks on eight voices steal the oldest, a note on a track takes the track's own voice") {
  static Wave w(64);
  Rig r(&pool_a(), &pool_p());
  r.p->set_sample(0, desc_of(w.d, 600, true));
  for (unsigned t = 0; t < 8; ++t) {
    CHECK(r.send(note_on(t, 60, 0, 127)));
  }
  CHECK(r.a->stats().steals == 0);
  CHECK(r.send(note_on(3, 62, 0, 127)));  // the same track: no steal
  CHECK(r.a->stats().steals == 0);
  CHECK(r.a->voices_in_use() == 8);
  CHECK(r.send(note_on(8, 60, 0, 127)));  // the ninth track: the oldest (track 0) goes
  CHECK(r.a->stats().steals == 1);
  CHECK(r.a->track_busy(8));
  CHECK_FALSE(r.a->track_busy(0));
}

namespace {

// OS-COREFX CF1: a tracker FX of an Amiga track is the MED command that means the same. Tracks 0 and 1 play the same note; track 0 hears TrkFx(type, value), track 1 the MED command
// `cmd` + `data` in the engine window; after one row of six ticks they must agree at every tick. Returns what track 0 ended on, and whether its period moved on the way.
struct Mapped {  // moved: the period or the sound differs from the track without the effect
  unsigned start_vol, start_period, vol, period;
  bool moved;
};
Mapped check_mapped(unsigned type, unsigned value, unsigned cmd, unsigned data) {
  static Ramp ramp;  // looped: a change of pitch is heard
  Rig r(&pool_a(), &pool_p());
  r.p->set_sample(0, desc_of(ramp.d, 128, true));
  CHECK(r.send(note_on(0, 60, 0, 63)));  // velocity 63: volume 32
  CHECK(r.send(note_on(1, 60, 0, 63)));
  CHECK(r.send(note_on(2, 60, 0, 63)));  // the control: no effect
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  r.block();
  Mapped m{r.a->track_state(0).volume(), r.a->track_state(0).period(), 0, 0, false};
  (void)r.send(trk_fx(0, type, value));
  CHECK(r.send(engine_fx(1, cmd, data)));
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
    CHECK_MESSAGE(r.a->track_state(0).period() == r.a->track_state(1).period(), "type " << type << " value " << value << " tick " << t);
    CHECK_MESSAGE(r.a->track_state(0).volume() == r.a->track_state(1).volume(), "type " << type << " value " << value << " tick " << t);
    m.moved = m.moved || r.a->track_state(0).period() != m.start_period;
    r.block();  // what the voices play in this tick's time: the arpeggio's pitch is heard, not in the period the track keeps
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      CHECK_MESSAGE(r.blk[0][i] == r.blk[1][i], "type " << type << " value " << value << " tick " << t << " frame " << i);
      m.moved = m.moved || r.blk[0][i] != r.blk[2][i];
    }
  }
  m.vol = r.a->track_state(0).volume();
  m.period = r.a->track_state(0).period();
  return m;
}

}  // namespace

TEST_CASE("amiga voices: the tracker FX are the MED commands that mean the same (46 -> 00, 47 -> 0A, 48 -> 1A / 1B, 49 / 50 -> 11 / 12 in quarter units)") {
  // 46 Arpeggio: 00 xy verbatim
  CHECK(check_mapped(audio::kFxTrkArp, 0x37, 0x00, 0x37).moved);
  CHECK_FALSE(check_mapped(audio::kFxTrkArp, 0x00, 0x00, 0x00).moved);  // an arpeggio of 0 0 is no arpeggio, and not the end of the column either
  // 47 Volume slide: 0A, x up else y down, every tick of the row (the first one included)
  {
    const Mapped m = check_mapped(audio::kFxTrkVolSlide, 0x02, 0x0A, 0x02);
    CHECK(m.vol == m.start_vol - 12);
  }
  {
    const Mapped m = check_mapped(audio::kFxTrkVolSlide, 0x30, 0x0A, 0x30);
    CHECK(m.vol == m.start_vol + 18);
  }
  // 48 Fine volume slide: x up wins, else y down, once: 1A x / 1B y
  {
    const Mapped m = check_mapped(audio::kFxTrkFineVol, 0x30, 0x1A, 0x03);
    CHECK(m.vol == m.start_vol + 3);
  }
  {
    const Mapped m = check_mapped(audio::kFxTrkFineVol, 0x34, 0x1A, 0x03);  // both nibbles: the up one
    CHECK(m.vol == m.start_vol + 3);
  }
  {
    const Mapped m = check_mapped(audio::kFxTrkFineVol, 0x04, 0x1B, 0x04);
    CHECK(m.vol == m.start_vol - 4);
  }
  {
    const Mapped m = check_mapped(audio::kFxTrkFineVol, 0x00, 0x1B, 0x00);
    CHECK(m.vol == m.start_vol);
  }
  // 49 / 50 Fine portamento: 11 / 12 with (value + 2) / 4 (the value is quarters of a Paula period unit, to the nearest), once, up is a lower period
  {
    const Mapped m = check_mapped(audio::kFxTrkFinePortaUp, 8, 0x11, 2);
    CHECK(m.period == m.start_period - 2);
  }
  {
    const Mapped m = check_mapped(audio::kFxTrkFinePortaUp, 5, 0x11, 1);  // (5 + 2) / 4 = 1
    CHECK(m.period == m.start_period - 1);
  }
  {
    const Mapped m = check_mapped(audio::kFxTrkFinePortaUp, 1, 0x11, 0);  // (1 + 2) / 4 = 0: nothing
    CHECK(m.period == m.start_period);
  }
  {
    const Mapped m = check_mapped(audio::kFxTrkFinePortaUp, 255, 0x11, 64);  // (255 + 2) / 4 = 64: the largest, no byte overflow
    CHECK(m.period == m.start_period - 64);
  }
  {
    const Mapped m = check_mapped(audio::kFxTrkFinePortaDown, 8, 0x12, 2);
    CHECK(m.period == m.start_period + 2);
  }
  {
    const Mapped m = check_mapped(audio::kFxTrkFinePortaDown, 254, 0x12, 64);
    CHECK(m.period == m.start_period + 64);
  }
}

TEST_CASE("amiga voices: a TrkFx of type 0 ends the column's command only (per column), a bad track is left alone, the transport stop and FxReset behave as for the MED commands") {
  static Wave w(64);
  Rig r(&pool_a(), &pool_p());
  r.p->set_sample(0, desc_of(w.d, 600, true));
  CHECK(r.send(note_on(0, 60, 0, 127)));
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  const unsigned v0 = r.a->track_state(0).volume();
  const unsigned p0 = r.a->track_state(0).period();
  (void)r.send(trk_fx(0, audio::kFxTrkVolSlide, 0x01, 0));       // column 0: volume down 1 a tick
  (void)r.send(trk_fx(0, audio::kFxTrkFinePortaDown, 8, 1));     // column 1: fine porta, 2 units, once a row
  (void)r.send(trk_fx(200, audio::kFxTrkVolSlide, 0x01, 0));     // a track the pool does not have
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  CHECK(r.a->track_state(0).volume() == v0 - 6);
  CHECK(r.a->track_state(0).period() == p0 + 2);
  (void)r.send(trk_fx(0, 0, 0, 1));  // ends column 1: the slide of column 0 goes on, the porta is done
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  CHECK(r.a->track_state(0).volume() == v0 - 12);
  CHECK(r.a->track_state(0).period() == p0 + 2);
  (void)r.send(trk_fx(0, 0, 0, 0));  // ends column 0: nothing moves
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  CHECK(r.a->track_state(0).volume() == v0 - 12);
  // a command held when the transport stops and starts again is ended by the new run's FxReset, as the MED commands are
  (void)r.send(trk_fx(0, audio::kFxTrkVolSlide, 0x01, 0));
  (void)r.send(ev(EventKind::Stopped, audio::kNoTrack));
  (void)r.send(ev(EventKind::FxReset, audio::kNoTrack));
  CHECK(r.send(note_on(0, 60, 0, 127)));
  const unsigned v1 = r.a->track_state(0).volume();
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  CHECK(r.a->track_state(0).volume() == v1);
}

TEST_CASE("amiga voices: a note stolen from a track that holds a tracker FX leaves the stealing track alone; the effect goes on for the next note of its own track") {
  static Wave w(64);
  Rig r(&pool_a(), &pool_p());
  r.p->set_sample(0, desc_of(w.d, 600, true));
  for (unsigned t = 0; t < 8; ++t) {
    CHECK(r.send(note_on(t, 60, 0, 127)));
  }
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  (void)r.send(trk_fx(0, audio::kFxTrkVolSlide, 0x02, 0));
  CHECK(r.send(note_on(8, 60, 0, 127)));  // the ninth track takes the oldest voice: track 0's
  CHECK(r.a->stats().steals == 1);
  CHECK_FALSE(r.a->track_busy(0));
  CHECK(r.a->track_busy(8));
  const unsigned v8 = r.a->track_state(8).volume();
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  CHECK(r.a->track_state(8).volume() == v8);  // track 0's slide is not heard on track 8
  r.blocks(2);
  CHECK(r.a->voices_in_use() == 8);
  // track 0 plays again (stealing back the oldest): its column still holds the slide, as the MED command would
  CHECK(r.send(note_on(0, 60, 0, 127)));
  const unsigned v0 = r.a->track_state(0).volume();
  for (unsigned t = 0; t < 6; ++t) {
    CHECK(r.send(tick(t, 6)));
  }
  CHECK(r.a->track_state(0).volume() < v0);
}

TEST_CASE("amiga voices: the LED filter takes the top off, 0F F9 switches it, and the A500 low-pass is duller than the A1200") {
  static Ramp ramp;
  auto energy_of = [&](audio::paula::Model m, bool led, bool song_led = false) {
    Rig r(&pool_a(), &pool_p());
    r.a->set_filter(m, led);
    r.p->set_sample(0, desc_of(ramp.d, 128, true));
    if (song_led) {
      CHECK(r.send(engine_fx(0, 0x0F, 0xF9)));
    }
    CHECK(r.send(note_on(0, 72, 0, 127)));
    std::int64_t e = 0;
    r.blocks(2);
    for (unsigned b = 0; b < 6; ++b) {
      r.block();
      for (std::uint32_t i = 1; i < kBlk; ++i) {
        const std::int32_t d = r.blk[0][i] - r.blk[0][i - 1];
        e += static_cast<std::int64_t>(d) * d;
      }
    }
    return e;
  };
  const std::int64_t a1200 = energy_of(audio::paula::Model::A1200, false);
  const std::int64_t a500 = energy_of(audio::paula::Model::A500, false);
  const std::int64_t led = energy_of(audio::paula::Model::A500, true);
  CHECK(a500 < a1200);
  CHECK(led < a500);
  // the song's own switch: a row with 0F F9 turns the LED on, as the LED flag of the engine does
  const std::int64_t song_led = energy_of(audio::paula::Model::A500, false, true);
  CHECK(song_led < a500);
  CHECK(std::llabs(song_led - led) * 100 < led);  // the same, up to the one block the switch takes to reach the voices
}

TEST_CASE("sampler voices: the 12-bit and 8-bit engines round the output; the Bit Depth parameter / FX truncates") {
  static std::int16_t dc[2000];
  for (std::int16_t& s : dc) {
    s = 16100;
  }
  auto level = [&](std::uint8_t engine, std::uint8_t depth = 16, unsigned fx_bits = 0) {
    VoicePool& p = pool_p();
    SampleDesc d;
    d.data = dc;
    d.length = 2000;
    d.engine = engine;
    d.bit_depth = depth;
    p.set_sample(0, d);
    if (fx_bits != 0u) {
      p.fx_event(0, audio::kModGroupBitDepth, audio::kFxBitDepth, fx_bits);
    }
    CHECK(p.note_on(0, 60, 0, 127));
    std::int16_t last = 0;
    for (unsigned b = 0; b < 4; ++b) {
      p.begin_block();
      p.render(0, kBlk);
      p.end_block();
      last = p.feed(0)[kBlk - 1];
    }
    if (fx_bits != 0u) {
      p.fx_event(0, audio::kModGroupBitDepth, 0, 0);
    }
    return last;
  };
  CHECK(level(audio::kEngineModern) == 16100);
  CHECK(level(audio::kEngineBits12) == 16096);  // (16100 + 8) & ~15
  CHECK(level(audio::kEngineBits8) == 16128);   // (16100 + 128) & ~255
  CHECK(level(audio::kEngineBits8, 12) == 16128);       // the engine's 8 bits are the lower: it rounds
  CHECK(level(audio::kEngineBits12, 16, 8) == 15872);   // the FX's 8 bits are the lower: the mask, 16100 & ~255
}

TEST_CASE("paula filter: the graph insert is the same filter on a Q15 block") {
  audio::paula::Filter a;
  audio::paula::Filter b;
  a.set(audio::paula::Model::A500, true);
  b.set(audio::paula::Model::A500, true);
  std::int16_t q[kBlk];
  float f[kBlk];
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    q[i] = static_cast<std::int16_t>((i % 7u) * 3000 - 9000);
    f[i] = static_cast<float>(q[i]) * (1.0f / 32768.0f);
  }
  audio::paula::Filter::insert(&a, q, kBlk);
  b.process(f, kBlk);
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    CHECK(q[i] == audio::paula::Channel::to_q15(f[i]));
  }
}

namespace {
SeqEvent g_script[4];
unsigned g_script_n = 0;
std::uint32_t script_step(void*, std::uint64_t block_start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
  std::uint32_t c = 0;
  if (block_start / n == 1u) {  // the events of block 1
    for (unsigned i = 0; i < g_script_n && c < cap; ++i) {
      out[c++] = g_script[i];
    }
  }
  return c;
}
std::uint32_t g_fake_cycles = 0;
std::uint32_t fake_cycles() noexcept { return g_fake_cycles += 7u; }
}  // namespace

TEST_CASE("amiga voices: through the engine the note is the track's input, the sampler's voice gives way, and a Modern note takes the track back") {
  static Wave w(64);
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static SeqEvent events[audio::kMaxEventsPerBlock];
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool_p();
  AmigaVoices& a = pool_a();
  p.set_sample(0, desc_of(w.d, 600, true));                               // Amiga
  p.set_sample(1, desc_of(w.d, 600, true, audio::kEngineModern));         // Modern
  a.set_filter(audio::paula::Model::Off, false);
  e.attach_voices(&p);
  e.attach_amiga(&a);
  audio::EventSource src;
  src.step = &script_step;
  src.buffer = events;
  src.capacity = audio::kMaxEventsPerBlock;
  e.attach_events(src);
  g_script_n = 0;
  g_script[g_script_n++] = note_on(0, 60, 0, 127);
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  haudio::BlockReport report{};
  const haudio::BlockIo io{in, out, 0, &report};
  e.render_block(io);  // block 0: nothing
  CHECK(a.feed(0) == nullptr);
  e.render_block(io);  // block 1: the note
  NEED(a.feed(0) != nullptr);
  CHECK(a.voices_in_use() == 1);
  CHECK(p.voices_in_use() == 0);  // the sampler pool has no voice for it
  CHECK(out[2 * (kBlk - 1)] != 0);  // the graph made it audible
  CHECK(e.amiga() == &a);
}

// ---- OS-SLICEFIX: a beat slice instrument on the Amiga engine plays the slice the note names, as the sampler's voices do --------------------------------------------------------------

namespace {

// Four slices of 100 frames, each a constant (20, 40, 60, 80 of int8: Q15 5120, 10240, 15360, 20480 at volume 64), a table of the sample arena's kind (low half, high half).
struct Slices {
  std::int16_t d[400];
  std::uint16_t t[10];
  explicit Slices(std::initializer_list<std::uint32_t> bounds = {0, 100, 200, 300, 400}) {
    for (int k = 0; k < 4; ++k) {
      for (int i = 0; i < 100; ++i) {
        d[100 * k + i] = static_cast<std::int16_t>(20 * (k + 1) * 256);
      }
    }
    unsigned i = 0;
    for (const std::uint32_t b : bounds) {
      t[2 * i] = static_cast<std::uint16_t>(b & 0xFFFFu);
      t[2 * i + 1] = static_cast<std::uint16_t>(b >> 16);
      ++i;
    }
  }
  SampleDesc desc(int tune = 0) const {
    SampleDesc s = desc_of(d, 400, false);
    s.beat_slice = true;
    s.slice_count = 4;
    s.slices = t;
    s.tune = static_cast<std::int8_t>(tune);
    return s;
  }
};

constexpr int slice_level(int k) { return 20 * (k + 1) * 256; }

}  // namespace

TEST_CASE("amiga voices: a beat slice plays the slice the note names (note - 24, clamped), whole, one shot, at the root's pitch (OS-SLICEFIX)") {
  static Slices sl;
  Rig r(&pool_a(), &pool_p());
  r.a->set_filter(audio::paula::Model::Off, false);
  r.p->set_sample(0, sl.desc());
  // note 24 + k is slice k
  for (unsigned k = 0; k < 4; ++k) {
    CHECK(r.send(note_on(k, audio::kSliceRootNote + k, 0, 127)));
  }
  r.blocks(2);
  for (unsigned k = 0; k < 4; ++k) {
    CHECK_MESSAGE(r.blk[k][kBlk - 1] == slice_level(static_cast<int>(k)), "slice " << k);
    CHECK(r.a->track_state(k).period() == 428);   // the root's pitch (60), whatever the note
  }
  // a note past the last slice is the last slice, one below the root is the first, and the pitch is still the root's
  CHECK(r.send(note_on(4, 24 + 9, 0, 127)));
  CHECK(r.send(note_on(5, 60, 0, 127)));
  CHECK(r.send(note_on(6, 100, 0, 127)));
  CHECK(r.send(note_on(7, 10, 0, 127)));
  r.blocks(2);
  CHECK(r.blk[4][kBlk - 1] == slice_level(3));
  CHECK(r.blk[5][kBlk - 1] == slice_level(3));
  CHECK(r.blk[6][kBlk - 1] == slice_level(3));
  CHECK(r.blk[7][kBlk - 1] == slice_level(0));
  for (unsigned t = 4; t < 8; ++t) CHECK(r.a->track_state(t).period() == 428);
  // the instrument's tune still applies (an octave up: period 214)
  r.p->set_sample(1, sl.desc(12));
  CHECK(r.send(note_on(8, 25, 1, 127)));
  r.blocks(2);
  CHECK(r.blk[8][kBlk - 1] == slice_level(1));
  CHECK(r.a->track_state(8).period() == 214);
  // the slice is one shot: it ends where it ends (100 frames at 0.188 a sample are 532 output samples) and is silent after, it does not run on into the next slice
  r.blocks(12);
  CHECK(r.blk[0][kBlk - 1] == 0);
  CHECK(r.blk[1][kBlk - 1] == 0);
  // an instrument that is not a beat slice plays all of its sample from the start, at the pitch of the note (the regression this must not be)
  SampleDesc whole = desc_of(sl.d, 400, false);
  r.p->set_sample(2, whole);
  CHECK(r.send(note_on(9, 72, 2, 127)));
  r.blocks(2);
  CHECK(r.blk[9][kBlk - 1] == slice_level(0));
  CHECK(r.a->track_state(9).period() == 214);
}

TEST_CASE("amiga voices: the Slice FX forces the slice, value modulo the count, until it ends; a slice with no length plays nothing (OS-SLICEFIX)") {
  static Slices sl;
  static Slices gap({0, 100, 100, 300, 400});   // slice 1 is empty
  Rig r(&pool_a(), &pool_p());
  r.a->set_filter(audio::paula::Model::Off, false);
  r.p->set_sample(0, sl.desc());
  r.p->set_forced_slice(0, 6);                    // 6 % 4 = slice 2, whatever the note
  CHECK(r.send(note_on(0, 24, 0, 127)));
  CHECK(r.send(note_on(1, 24, 0, 127)));          // another track has no FX: the note's slice 0
  r.blocks(2);
  CHECK(r.blk[0][kBlk - 1] == slice_level(2));
  CHECK(r.blk[1][kBlk - 1] == slice_level(0));
  r.p->set_forced_slice(0, 3);
  CHECK(r.send(note_on(2, 40, 0, 127)));
  r.p->set_forced_slice(2, 7);
  CHECK(r.send(note_on(2, 40, 0, 127)));          // retriggered on the track: 7 % 4 = slice 3
  r.blocks(2);
  CHECK(r.blk[2][kBlk - 1] == slice_level(3));
  r.p->clear_forced_slice(2);
  CHECK(r.send(note_on(3, 25, 0, 127)));
  r.p->clear_forced_slice(0);
  CHECK(r.send(note_on(0, 25, 0, 127)));          // the FX ended: the note's own slice 1
  r.blocks(2);
  CHECK(r.blk[0][kBlk - 1] == slice_level(1));
  CHECK(r.blk[3][kBlk - 1] == slice_level(1));
  r.p->set_forced_slice(5, 254);                  // the largest value the voices keep: 254 % 4 = 2
  CHECK(r.send(note_on(5, 24, 0, 127)));
  r.blocks(2);
  CHECK(r.blk[5][kBlk - 1] == slice_level(2));
  // a slice with no length is silent, and its note takes the track from the voice that was on it
  r.p->set_sample(1, gap.desc());
  r.p->clear_forced_slices();
  CHECK(r.send(note_on(6, 24, 1, 127)));
  r.blocks(2);
  const unsigned before = r.a->voices_in_use();
  const std::uint32_t ignored = r.a->stats().ignored;
  CHECK(r.send(note_on(7, 25, 1, 127)));          // slice 1 of the gapped table: [100, 100)
  CHECK(r.a->voices_in_use() == before);
  CHECK(r.a->stats().ignored == ignored + 1);
  CHECK(r.send(note_on(8, 26, 1, 127)));          // slice 2: [100, 300), which runs over the constants of slices 1 and 2
  r.blocks(2);
  CHECK(r.blk[8][kBlk - 1] == slice_level(1));
}

// OS-CEPLAYTHRU P2: with the descriptor's slice_thru, the Amiga voice plays its slice on to the sample end (beat_slice_span is the one rule of both engines), the Slice FX's forced slice too.
TEST_CASE("amiga voices: a beat slice with play through runs to the sample end, forced slice too (OS-CEPLAYTHRU P2)") {
  static Slices sl;
  Rig r(&pool_a(), &pool_p());
  r.a->set_filter(audio::paula::Model::Off, false);
  SampleDesc d = sl.desc();
  d.slice_thru = 1;
  r.p->set_sample(0, d);
  // the span itself: the note's slice and the forced one end at the length, not at the next bound
  std::uint32_t from = 0, len = 0;
  bool ok = audio::beat_slice_span(d, 24, audio::kNoForcedSlice, from, len);
  CHECK(ok);
  CHECK((from == 0u && len == 400u));
  ok = audio::beat_slice_span(d, 60, 1, from, len);  // forced 1 % 4 = slice 1
  CHECK(ok);
  CHECK((from == 100u && len == 300u));
  // the render: the same note without the flag stops at its slice's own end (slice 0 is 100 frames, over well before 16 blocks); with the flag it plays on (still sounding at 16 blocks)
  r.p->set_sample(0, sl.desc());          // no flag
  CHECK(r.send(note_on(0, 24, 0, 127)));   // slice 0
  r.blocks(16);
  CHECK(r.blk[0][kBlk - 1] == 0);
  r.p->set_sample(0, d);                   // slice_thru = 1
  CHECK(r.send(note_on(1, 24, 0, 127)));   // slice 0 again, on track 1
  r.blocks(16);
  CHECK(r.blk[1][kBlk - 1] != 0);          // it runs to the sample end
  // the Slice FX's forced slice too: track 2, forced 1 (slice 1, [100, 400) with play through; alone it would be over by now)
  r.p->set_forced_slice(2, 1);
  CHECK(r.send(note_on(2, 60, 0, 127)));
  r.blocks(16);
  CHECK(r.blk[2][kBlk - 1] != 0);
  r.p->clear_forced_slices();
}

// ---- OS-BUG-PASTE24 E1 (QA-INS-203): the peak of an Amiga instrument against the instrument's own gain ------------------------------------------------------
// The unit read an Amiga instrument above its gain (a square chip wave, levels 25 and 37, full scale at level 45) while the Modern engine peaked exactly at it. Host findings, held here:
//  - a note that names no velocity (a pad: audio::kLiveVelocityDefault) plays at amiga_vol / 64, and the gain it comes from is the Modern engine's: a triangle peaks within the volume's rounding of it;
//  - a velocity REPLACES the instrument's level in both engines (voices.h LEVEL), at (v + 1) / 2 of 64 here and v / 127 there: the same level within the rounding, whatever the instrument's own;
//  - the excess is the minBLEP of Paula (pt2-clone's table, held against it by paula_oracle_test): a step of 2a rings to 1/6 of its size past the new value, so a full-scale square (steps of 2a, one every
//    half period) peaks at 4/3 of its held level, +2.5 dB, with the A1200 model and with none; the A500's 4.9 kHz low-pass takes most of it. The peak is the oracle's own ringing, not a level error (new Community OS behaviour).
namespace {

// A full-scale chip wave of 32 frames, 8 bits in the top byte (the Paula way): kind 0 square, 1 triangle.
struct Chip {
  std::int16_t d[32];
  explicit Chip(int kind) {
    for (int i = 0; i < 32; ++i) {
      const int v = kind == 0 ? (i < 16 ? 127 : -127) : (i < 16 ? -127 + i * 254 / 15 : 127 - (i - 16) * 254 / 15);
      d[i] = static_cast<std::int16_t>(v * 256);
    }
  }
};

SampleDesc chip_at(const Chip& wave, unsigned level, std::uint8_t engine) {
  SampleDesc d = desc_of(wave.d, 32, true, engine);
  CHECK(audio::apply_instrument_param(d, audio::inst_param::Volume, static_cast<float>(level), audio::LevelLaw::Og12) == audio::InstApply::Applied);
  return d;
}

// The peak (Q15 magnitude) of the track-0 feed over blocks 8..39 of a note.
template <typename Pool>
int peak_of(Pool& pool, const VoicePool* amiga_samples) {
  (void)amiga_samples;
  int peak = 0;
  for (unsigned b = 0; b < 40; ++b) {
    pool.begin_block();
    pool.render(0, kBlk);
    pool.end_block();
    const std::int16_t* f = pool.feed(0);
    if (f != nullptr && b >= 8) {
      for (std::uint32_t i = 0; i < kBlk; ++i) {
        const int m = f[i] < 0 ? -static_cast<int>(f[i]) : static_cast<int>(f[i]);
        peak = m > peak ? m : peak;
      }
    }
  }
  return peak;
}

// The Amiga pool's peak for a live note of an instrument at `level` over `wave`.
int amiga_peak(const Chip& wave, unsigned level, unsigned velocity, audio::paula::Model model) {
  AmigaVoices& a = pool_a();
  VoicePool& p = pool_p();
  a.set_filter(model, false);
  p.set_sample(0, chip_at(wave, level, kEngineA));
  CHECK(a.live_note_on(0, 60, 0, velocity, p));
  const int peak = peak_of(a, &p);
  a.release_all();
  for (unsigned b = 0; b < 8; ++b) {
    a.begin_block();
    a.render(0, kBlk);
    a.end_block();
  }
  return peak;
}

// The sampler's peak for the same note.
int sampler_peak(const Chip& wave, unsigned level, unsigned velocity) {
  VoicePool& p = pool_p();
  p.set_sample(0, chip_at(wave, level, 0));
  CHECK(p.note_on(0, 60, 0, velocity));
  const int peak = peak_of(p, nullptr);
  p.cut(0);
  return peak;
}

// |a - b| <= tolerance_pct percent of b.
bool near_pct(int a, int b, int tolerance_pct) { return (a > b ? a - b : b - a) * 100 <= b * tolerance_pct; }

}  // namespace

TEST_CASE("amiga level (QA-INS-203): a note with no velocity peaks at the instrument's gain, as the Modern engine's does") {
  static Chip tri(1);
  for (const audio::paula::Model model : {audio::paula::Model::Off, audio::paula::Model::A1200}) {
    for (const unsigned level : {25u, 37u, 45u}) {
      CAPTURE(level);
      const int modern = sampler_peak(tri, level, audio::kVelocityDefault);
      const int amiga = amiga_peak(tri, level, audio::kVelocityDefault, model);
      // the sample's 127 of 128 is in both; the Amiga volume is the gain rounded to 1/64 (at most 1.6% at level 25)
      CHECK(near_pct(modern, static_cast<int>(audio::level_gain_q16(level, audio::LevelLaw::Og12) * 32512LL / 65536), 1));
      CHECK(near_pct(amiga, modern, 4));  // level 45: volume 49 of 48.5
    }
  }
}

TEST_CASE("amiga level (QA-INS-203): a velocity replaces the instrument's level, in both engines alike") {
  static Chip tri(1);
  const int at25 = amiga_peak(tri, 25, 100, audio::paula::Model::Off);
  const int at45 = amiga_peak(tri, 45, 100, audio::paula::Model::Off);
  CHECK(near_pct(at25, at45, 1));  // the instrument's level does not scale it (MED 0C: the velocity is the volume), as the sampler's
  CHECK(near_pct(sampler_peak(tri, 25, 100), sampler_peak(tri, 45, 100), 1));
  CHECK(near_pct(at25, sampler_peak(tri, 25, 100), 2));  // (100 + 1) / 2 of 64 against 100 of 127
}

TEST_CASE("amiga level (QA-INS-203): a full-scale square peaks at 4/3 of its held level, the minBLEP's overshoot (pt2-clone parity)") {
  static Chip sq(0);
  for (const unsigned level : {25u, 37u}) {
    CAPTURE(level);
    const int held = static_cast<int>(chip_at(sq, level, kEngineA).amiga_vol) * 127 * 4;  // volume x 127 / 8192, in Q15
    CHECK(near_pct(amiga_peak(sq, level, audio::kVelocityDefault, audio::paula::Model::Off), held * 4 / 3, 1));
    const int a1200 = amiga_peak(sq, level, audio::kVelocityDefault, audio::paula::Model::A1200);  // plus the high-pass's droop
    CHECK(a1200 > held * 130 / 100);
    CHECK(a1200 < held * 140 / 100);
  }
  // level 45 is volume 49 (0.766 of full scale): the overshoot takes the held square past full scale, where the voice's Q15 saturates
  CHECK(amiga_peak(sq, 45, audio::kVelocityDefault, audio::paula::Model::A1200) >= 32767);
  // the A500 model's low-pass takes most of it
  CHECK(amiga_peak(sq, 25, audio::kVelocityDefault, audio::paula::Model::A500) * 100 < chip_at(sq, 25, kEngineA).amiga_vol * 127 * 4 * 112);
}

// ---- OS-CPU10: the Paula path's digest, pinned from the code before the fusion (A1 BLEP taps, A2 fused pass, A3 locals) -----------------------------------------------------------
// One scenario through AmigaVoices::render per filter configuration: loops, a sub-loop, a one shot, every pitch from the longest period to the clamp, slides, a volume slide, a
// retrigger, a note off and a cut, events in the middle of a block (the engine renders in pieces up to an event), and a second scenario with no Tick event, in which the one shot
// ends while the voice is still held (`on` with the channel inactive: zeros go through the filter's tail). The FNV-1a digest of every fed int16 is pinned: the same constants on the
// host and on the Cortex-M4F (the qemu suite), which the optimisation must leave alone (conditions of the OS-CPU10 risk review).
namespace {

struct Digest {
  std::uint32_t h = 2166136261u;
  void byte(std::uint8_t b) { h = (h ^ b) * 16777619u; }
  void word(std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) {
      byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
  }
};

struct Noisy {
  std::int16_t d[600];
  Noisy() {
    std::uint32_t r = 12345u;
    for (unsigned i = 0; i < 600; ++i) {
      r = r * 1664525u + 1013904223u;
      const int saw = static_cast<int>((i * 7u) % 200u) - 100;
      const int b = saw + static_cast<int>((r >> 24) % 21u) - 10;
      d[i] = static_cast<std::int16_t>(b * 256 + static_cast<int>((i * 37u) & 0xFFu));
    }
  }
};

void feed_digest(Digest& dg, const AmigaVoices& a) {
  for (unsigned t = 0; t < 16; ++t) {
    const std::int16_t* f = a.feed(t);
    dg.word(f != nullptr ? 0xF00Du + t : 0xDEADu);
    for (std::uint32_t i = 0; f != nullptr && i < kBlk; ++i) {
      dg.word(static_cast<std::uint16_t>(f[i]));
    }
  }
}

// Two pools driven in lockstep: `a` renders with render() (the fused pass), `r` with render_ref() (the three passes). Both get every call; end_block() compares the feeds of the two
// sample by sample (int16 bit-equal) and counts the blocks in which they differ.
struct Twin {
  AmigaVoices& a;
  AmigaVoices& r;
  unsigned mismatches = 0;
  void set_filter(audio::paula::Model m, bool led) {
    a.set_filter(m, led);
    r.set_filter(m, led);
  }
  void begin_block() {
    a.begin_block();
    r.begin_block();
  }
  void render(std::uint32_t begin, std::uint32_t end) {
    a.render(begin, end);
    r.render_ref(begin, end);
  }
  bool event(const SeqEvent& e, const VoicePool& p) {
    const bool x = a.event(e, p);
    mismatches += r.event(e, p) != x ? 1u : 0u;
    return x;
  }
  void end_block() {
    a.end_block();
    r.end_block();
    for (unsigned t = 0; t < 16; ++t) {
      const std::int16_t* fa = a.feed(t);
      const std::int16_t* fr = r.feed(t);
      if ((fa == nullptr) != (fr == nullptr) || (fa != nullptr && std::memcmp(fa, fr, kBlk * sizeof fa[0]) != 0)) {
        ++mismatches;
      }
    }
    mismatches += a.voices_in_use() != r.voices_in_use() ? 1u : 0u;
  }
};

std::uint32_t paula_scenario(audio::paula::Model model, bool led, bool ticks, unsigned* in_use = nullptr, unsigned* mismatches = nullptr) {
  static Noisy noisy;
  static o3os::test::StaticSlot<AmigaVoices> slot_a, slot_r;
  Twin a{slot_a.make(), slot_r.make()};
  VoicePool& p = pool_p();
  p.set_sample(0, desc_of(noisy.d, 600, true));
  SampleDesc sub = desc_of(noisy.d, 600, true);
  sub.loop_start = 100;
  sub.loop_end = 400;
  p.set_sample(1, sub);
  p.set_sample(2, desc_of(noisy.d, 24, false));
  a.set_filter(model, led);
  a.begin_block();  // the filters take the setting at the end of a block
  a.render(0, kBlk);
  a.end_block();
  Digest dg;
  const unsigned blocks = ticks ? 40u : 12u;
  for (unsigned b = 0; b < blocks; ++b) {
    a.begin_block();
    const bool split = (b % 3u) == 1u;
    const std::uint32_t at = split ? 37u : 0u;
    if (split) {
      a.render(0, at);
    }
    if (ticks) {
      (void)a.event(tick(b % 6u, 6), p);
      if (b == 0) {
        (void)a.event(note_on(0, 60, 0, 127), p);
        (void)a.event(note_on(1, 72, 1, 100), p);
        (void)a.event(note_on(2, 36, 0), p);
        (void)a.event(note_on(3, 96, 0, 90), p);
        (void)a.event(note_on(4, 60, 2, 127), p);
      } else if (b == 2) {
        (void)a.event(engine_fx(1, 0x01, 3), p);
      } else if (b == 4) {
        (void)a.event(engine_fx(0, 0x0D, 0x02), p);
      } else if (b == 8) {
        (void)a.event(ev(EventKind::NoteOff, 2), p);
      } else if (b == 12) {
        (void)a.event(ev(EventKind::NoteCut, 3), p);
      } else if (b == 14) {
        (void)a.event(note_on(3, 84, 1, 110), p);
        (void)a.event(note_on(0, 55, 0, 127), p);
      } else if (b == 20) {
        (void)a.event(note_on(5, 60, 2, 127), p);
        (void)a.event(engine_fx(1, 0x02, 2), p);
      }
    } else if (b == 0) {
      (void)a.event(note_on(0, 60, 2, 127), p);  // a one shot over two blocks
      (void)a.event(note_on(1, 84, 2, 80), p);   // another, which ends the voice with its channel
      (void)a.event(engine_fx(0, 0x1F, 0x50), p);
      (void)a.event(note_on(0, 62, 2, 100), p);  // a note delayed by 5 ticks over the playing one, and no Tick comes: the voice is held (`wait`) after its channel is over, warm filter
    }
    a.render(at, kBlk);
    a.end_block();
    feed_digest(dg, a.a);
  }
  dg.word(a.a.voices_in_use());
  if (in_use != nullptr) {
    *in_use = a.a.voices_in_use();
  }
  if (mismatches != nullptr) {
    *mismatches = a.mismatches;
  }
  return dg.h;
}

}  // namespace

TEST_CASE("amiga voices (OS-CPU10): the Paula path's digest is pinned, host and Cortex-M4F, for every filter model and LED setting") {
  using audio::paula::Model;
  // The digest is per clock (core/audio_clock.h): a Paula channel's step is the Paula clock over the engine's rate, so the 0.04 % between the pre-1.0 clock (4080 cycles a
  // sample, 44,117.647 Hz) and the default (44,100 Hz) moves every sample of every case, and the filter coefficients (paula.h) are per clock too. `pre` is the value dev
  // pinned before OS-RATE (the OFF build must still match it: only the clock differs); `now` is the default clock's, taken from this tree's run (OS-RATE).
  struct Case {
    Model model;
    bool led;
    bool ticks;
    std::uint32_t pre;
    std::uint32_t now;
  };
  constexpr bool kPreClock = o3os::core::kRefCyclesPerSample.num == 4080u && o3os::core::kRefCyclesPerSample.den == 1u;
  static const Case kCases[] = {
      {Model::Off, false, true, 187283949u, 3127083575u},    {Model::Off, true, true, 2771740872u, 889150649u},      {Model::A500, false, true, 3374053021u, 756090176u},
      {Model::A500, true, true, 564755277u, 2065305337u},    {Model::A1200, false, true, 2556006351u, 1967718722u},  {Model::A1200, true, true, 2645940797u, 976175044u},
      {Model::Off, false, false, 3114138733u, 28514277u},    {Model::A1200, false, false, 1434709678u, 3083565467u}, {Model::A1200, true, false, 3833815840u, 1288991662u},
      {Model::A500, true, false, 1406848224u, 632766650u},   {Model::Off, true, false, 3916418623u, 3896461347u},
  };
  for (const Case& c : kCases) {
    CAPTURE(static_cast<int>(c.model));
    CAPTURE(c.led);
    CAPTURE(c.ticks);
    unsigned in_use = 0, mismatches = 99;
    CHECK(paula_scenario(c.model, c.led, c.ticks, &in_use, &mismatches) == (kPreClock ? c.pre : c.now));
    CHECK(mismatches == 0u);  // OS-CPU10 A2: the fused pass and the three-pass reference give the same int16 in every block, and keep the same voices
    if (!c.ticks) {
      CHECK(in_use == 1u);  // no Tick: the second one shot is over and gone, the first's voice is held with its channel inactive
    }
  }
}
