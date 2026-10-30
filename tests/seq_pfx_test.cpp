// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PFX (docs/specs/OS-PFX.md): the step FX Chord 14, Arp 21, Tune 40 and Slide 41 / 42 in the sequencer: what each one makes of a note (the notes and where in time they
// start), the values and their edges, the FX that share a step with them, the instrument kinds (MIDI, sample, beat slice), and reproducibility over any block size. The voice side
// of Slide (the pitch ramp) is in audio_voices_test.cpp. Host and Cortex-M4F (`qemu_seq_model`: one project in .bss).
#include <doctest/doctest.h>

#include <o3os/seq/fx.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>

#include "support/need.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using namespace o3os::test;
using audio::EventKind;
namespace fx = o3os::seq::fx;

namespace {

StaticSlot<seq::Sequencer>& g_slot = seq_slot();
Collector& g_col = seq_collector();

constexpr std::uint8_t kMidiInstr = 11;  // the MIDI instrument is slot 10, which a step names as 11 (fresh_project): channel 3, as 2 on the wire

seq::Sequencer& make() {
  seq::Sequencer& s = g_slot.make(seq_project());
  g_col.n = 0;
  g_col.total_events = 0;
  g_col.hash = 0xCBF29CE484222325ull;
  return s;
}

void pattern16(std::uint32_t tracks = 4) {
  fresh_project(tracks, 6);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
}

// The sample of microstep j of step n at 120 BPM (core/qtime.h: round(ticks * kCpuHz * den * 125 / (4 * num * bpm_x100)), half up).
std::uint64_t at_us(std::uint32_t n, std::uint32_t j) {
  const std::uint64_t ticks = 48u * n + j;
  const std::uint64_t num = ticks * core::kCpuHz * core::kRefCyclesPerSample.den * 125u;
  const std::uint64_t d = std::uint64_t{4u} * core::kRefCyclesPerSample.num * 12000u;  // 64-bit: 4 * 1800000 * 12000 does not fit 32
  return (2u * num + d) / (2u * d);
}

// The notes of the events of `kind` on `track` at sample `t`, in the order they came out. Returns how many (at most 8).
std::uint32_t notes_at(EventKind kind, std::uint8_t track, std::uint64_t t, std::uint8_t out[8]) {
  std::uint32_t c = 0;
  for (std::uint32_t i = 0; i < g_col.n && c < 8u; ++i) {
    if (g_col.rec[i].ev.kind == kind && g_col.rec[i].ev.track == track && g_col.rec[i].t == t) {
      out[c++] = g_col.rec[i].ev.a;
    }
  }
  return c;
}

const Rec* find_at(EventKind k, std::uint8_t track, std::uint64_t t) {
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == k && g_col.rec[i].ev.track == track && g_col.rec[i].t == t) {
      return &g_col.rec[i];
    }
  }
  return nullptr;
}

// One note at row 0 of track 0 with the given FX, run to the start of row 2.
void one_note(std::uint8_t note, std::uint8_t instr, std::uint8_t fx0, std::uint8_t v0, std::uint8_t fx1 = 0, std::uint8_t v1 = 0) {
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(note, instr, fx0, v0, fx1, v1)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(2, 0), 64);
}

// The 30 chords (docs/specs/OS-PFX.md; written out again, an independent copy of seq::kChordMask): the count of tones above the root, their
// offsets (up to three) and the mask of the value.
struct Want {
  std::uint8_t n, iv[3];
  std::uint16_t mask;
};
constexpr Want kWant[30] = {
    {0, {0, 0, 0}, 0x0001}, {2, {2, 7, 0}, 0x0085}, {2, {2, 8, 0}, 0x0105},
    {2, {3, 6, 0}, 0x0049}, {2, {3, 7, 0}, 0x0089}, {2, {4, 7, 0}, 0x0091},
    {2, {4, 8, 0}, 0x0111}, {2, {5, 7, 0}, 0x00A1}, {2, {5, 10, 0}, 0x0421},
    {2, {5, 12, 0}, 0x1021}, {2, {6, 7, 0}, 0x00C1}, {2, {7, 12, 0}, 0x1081},
    {2, {7, 14, 0}, 0x4081}, {3, {2, 7, 9}, 0x0285}, {3, {2, 7, 10}, 0x0485},
    {3, {2, 7, 11}, 0x0885}, {3, {3, 6, 9}, 0x0249}, {3, {3, 6, 10}, 0x0449},
    {3, {3, 7, 8}, 0x0189}, {3, {3, 7, 9}, 0x0289}, {3, {3, 7, 10}, 0x0489},
    {3, {3, 7, 11}, 0x0889}, {3, {4, 7, 9}, 0x0291}, {3, {4, 7, 10}, 0x0491},
    {3, {4, 7, 11}, 0x0891}, {3, {4, 8, 9}, 0x0311}, {3, {4, 8, 10}, 0x0511},
    {3, {4, 8, 11}, 0x0911}, {3, {5, 7, 10}, 0x04A1}, {3, {5, 7, 11}, 0x08A1},
};

// Arp periods in microsteps by rate index, written out again (docs/specs/OS-PFX.md).
constexpr std::uint16_t kWantPeriod[11] = {384, 288, 192, 144, 96, 48, 24, 16, 12, 8, 6};

}  // namespace

TEST_CASE("seq pfx: the value functions are the spec's: chords, arp directions and rates, tune and slide") {
  static_assert(seq::kChordCount == 30 && seq::kChordMaxExtra == 3, "the chord table of docs/specs/OS-PFX.md");
  for (std::uint32_t v = 0; v < 30; ++v) {
    CAPTURE(v);
    CHECK(seq::chord_mask(static_cast<std::uint8_t>(v)) == kWant[v].mask);
    std::uint32_t bits = 1u;  // the root, always
    for (std::uint32_t i = 0; i < kWant[v].n; ++i) {
      bits |= 1u << kWant[v].iv[i];
    }
    CHECK(seq::chord_mask(static_cast<std::uint8_t>(v)) == bits);  // the offsets and the mask agree
    CHECK(seq::arp_mask(static_cast<std::uint8_t>(v)) == (v == 0 ? 0x0FFFu : (bits & 0x0FFFu)));
  }
  CHECK(seq::chord_mask(30) == 1);  // a value past the table is the root alone (this OS's rule, seq/fx.h chord_mask)
  CHECK(seq::chord_mask(200) == 1);
  CHECK(seq::arp_mask(0) == 0x0FFF);   // Chord 0 with an Arp: chromatic over offsets 0..11
  CHECK(seq::arp_mask(30) == 0x0FFF);
  CHECK(seq::arp_mask(9) == 0x0021);   // 5C = 0 5 12: the 12 is never visited
  CHECK(seq::arp_mask(11) == 0x0081);  // 7C: 0 7
  CHECK(seq::arp_mask(12) == 0x0081);  // 7E: 0 7
  CHECK(seq::kFxInfo[fx::kChord].max == 29);
  for (std::uint32_t v = 1; v <= 33; ++v) {
    const std::uint32_t d = (v - 1u) / 11u;
    const std::uint32_t r = (v - 1u) % 11u;
    CHECK_MESSAGE(static_cast<std::uint32_t>(seq::arp_dir(static_cast<std::uint8_t>(v))) == d, "arp " << v);
    CHECK_MESSAGE(seq::arp_microsteps(static_cast<std::uint8_t>(v)) == kWantPeriod[r], "arp " << v);
  }
  CHECK(seq::arp_microsteps(1) == 384);  // slowest, up: eight steps between notes
  CHECK(seq::arp_microsteps(6) == 48);   // one step
  CHECK(seq::arp_microsteps(11) == 6);   // fastest, up: 1/8 of a step
  CHECK(seq::arp_dir(12) == seq::ArpDir::Down);
  CHECK(seq::arp_dir(23) == seq::ArpDir::Random);
  CHECK(seq::arp_microsteps(33) == 6);
  CHECK(seq::arp_microsteps(250) == 6);  // past the range reads as the last
  CHECK(seq::arp_microsteps(0) == 384);   // 0 is off: never asked, but it must not index outside the table
  CHECK(seq::tune_semitones(0) == -24);
  CHECK(seq::tune_semitones(24) == 0);
  CHECK(seq::tune_semitones(48) == 24);
  CHECK(seq::tune_semitones(255) == 24);
  CHECK(seq::slide_sixteenths(fx::kSlideUp, 255) == 255);
  CHECK(seq::slide_sixteenths(fx::kSlideDown, 255) == -255);
  CHECK(seq::slide_sixteenths(fx::kSlideDown, 0) == 0);
}

TEST_CASE("seq pfx: Chord on a MIDI track sends its notes at the note's sample, root first, and ends them together") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kChord, 5)) == model::Status::Ok);  // Maj (value 5, mask 47)
  CHECK(p.set_step(1, 0, 4, make_step(62, kMidiInstr)) == model::Status::Ok);                  // a plain note: the chord ends, this is one note
  CHECK(p.set_step(1, 0, 6, special_step(model::kNoteOff, kMidiInstr)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(8, 0), 64);
  std::uint8_t n[8];
  NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(0, 0), n) == 3u);
  CHECK(n[0] == 60);
  CHECK(n[1] == 64);
  CHECK(n[2] == 67);
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::MidiNoteOn && g_col.rec[i].t == at_us(0, 0)) {
      CHECK(g_col.rec[i].ev.b == 127);  // one velocity for the chord
      CHECK(g_col.rec[i].ev.c == 2);    // one channel
    }
  }
  // the next note ends all three at its own sample, before it starts
  NEED(notes_at(EventKind::MidiNoteOff, 0, at_us(4, 0), n) == 3u);
  CHECK(n[0] == 60);
  CHECK(n[1] == 64);
  CHECK(n[2] == 67);
  NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(4, 0), n) == 1u);  // the plain note brings no chord along
  CHECK(n[0] == 62);
  NEED(notes_at(EventKind::MidiNoteOff, 0, at_us(6, 0), n) == 1u);
  CHECK(n[0] == 62);
  CHECK(g_col.count(EventKind::NoteOn) == 0u);
}

TEST_CASE("seq pfx: every Chord value 0..29 sends exactly the root and its table's offsets") {
  for (std::uint32_t v = 0; v < 30; ++v) {
    one_note(48, kMidiInstr, fx::kChord, static_cast<std::uint8_t>(v));
    std::uint8_t n[8];
    CAPTURE(v);
    NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(0, 0), n) == 1u + kWant[v].n);
    CHECK(n[0] == 48);
    for (std::uint32_t i = 0; i < kWant[v].n; ++i) {
      CHECK(n[1 + i] == 48 + kWant[v].iv[i]);
    }
    CHECK(g_col.count(EventKind::MidiNoteOn) == 1u + kWant[v].n);
  }
}

TEST_CASE("seq pfx: a chord loses the notes past 127, and a chord left with only its root does nothing more") {
  one_note(120, kMidiInstr, fx::kChord, 24);  // MajMaj7 (47B): 124, 127 and 131
  std::uint8_t n[8];
  NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(0, 0), n) == 3u);
  CHECK(n[0] == 120);
  CHECK(n[1] == 124);
  CHECK(n[2] == 127);
  one_note(127, kMidiInstr, fx::kChord, 5);
  NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(0, 0), n) == 1u);
  CHECK(n[0] == 127);
  // every MIDI note that came on goes off again on a stop
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(120, kMidiInstr, fx::kChord, 24)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(2, 0), 64);
  s.stop();
  g_col.run(s, at_us(2, 0), at_us(2, 0) + 64, 64);
  CHECK(g_col.count(EventKind::MidiNoteOff) == 3u);
}

TEST_CASE("seq pfx: Chord on a sample instrument plays the root alone (one voice a track) and is counted as ignored") {
  one_note(60, 1, fx::kChord, 20);
  CHECK(g_col.count(EventKind::NoteOn) == 1u);
  CHECK(g_col.count(EventKind::MidiNoteOn) == 0u);
  const Rec* on = find_at(EventKind::NoteOn, 0, at_us(0, 0));
  NEED(on != nullptr);
  CHECK(on->ev.a == 60);
}

TEST_CASE("seq pfx: a Chord, Arp or Tune value past its range plays as the range's end (the info line's words say the same: pattern_edit_rules_test)") {
  const auto played = [](std::uint8_t fx0, std::uint8_t v0, std::uint8_t fx1, std::uint8_t v1) {
    one_note(60, kMidiInstr, fx0, v0, fx1, v1);
    CHECK(g_col.count(EventKind::MidiNoteOn) >= 2u);  // the FX did something
    return g_col.hash;
  };
  const std::uint64_t last = played(fx::kChord, 29, 0, 0);
  CHECK(played(fx::kChord, 30, 0, 0) == last);  // fx_sanitize holds a Chord at 29 (57B), not at "none"
  CHECK(played(fx::kChord, 255, 0, 0) == last);
  const std::uint64_t arp = played(fx::kArp, 33, fx::kChord, 20);
  CHECK(played(fx::kArp, 34, fx::kChord, 20) == arp);
  CHECK(played(fx::kArp, 255, fx::kChord, 20) == arp);
  const std::uint64_t tune = played(fx::kTune, 48, fx::kChord, 5);
  CHECK(played(fx::kTune, 49, fx::kChord, 5) == tune);
  CHECK(played(fx::kTune, 255, fx::kChord, 5) == tune);
}

TEST_CASE("seq pfx: Chord shares its step with Gate, Velocity, Tune, Roll and Chance, and the first column wins over the second") {
  std::uint8_t n[8];
  // Gate 50 in the other column: the whole chord ends 24 microsteps in
  one_note(60, kMidiInstr, fx::kChord, 5, fx::kGate, 50);
  NEED(notes_at(EventKind::MidiNoteOff, 0, at_us(0, 24), n) == 3u);
  CHECK(n[0] == 60);
  // Velocity: one velocity, 50 % of 127 (truncating), for all of the chord
  one_note(60, kMidiInstr, fx::kChord, 5, fx::kVelocity, 50);
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::MidiNoteOn) {
      CHECK(g_col.rec[i].ev.b == 63);
    }
  }
  CHECK(g_col.count(EventKind::MidiNoteOn) == 3u);
  // Tune +7 transposes the whole chord
  one_note(60, kMidiInstr, fx::kChord, 5, fx::kTune, 31);
  NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(0, 0), n) == 3u);
  CHECK(n[0] == 67);
  CHECK(n[1] == 71);
  CHECK(n[2] == 74);
  // the Chord of the first column wins over the second's
  one_note(60, kMidiInstr, fx::kChord, 4, fx::kChord, 5);  // Min, Maj
  NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(0, 0), n) == 3u);
  CHECK(n[1] == 63);
  CHECK(n[2] == 67);
  // Chance 0 cancels the note (a chance of 0 plays only on a draw of 0, which this seed does not give), and with it the chord
  one_note(60, kMidiInstr, fx::kChord, 5, fx::kChance, 0);
  CHECK(g_col.count(EventKind::MidiNoteOn) % 3u == 0u);  // never a partial chord
  // Roll: every repeat sends the whole chord again, and every note that came on goes off
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kChord, 5, fx::kRoll, 4)) == model::Status::Ok);  // 1/4 step: 12 microsteps
  CHECK(seq_project().set_step(1, 0, 3, special_step(model::kNoteOff, kMidiInstr)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(4, 0), 64);
  CHECK(g_col.count(EventKind::MidiNoteOn) >= 9u);
  CHECK(g_col.count(EventKind::MidiNoteOn) % 3u == 0u);
  CHECK(g_col.count(EventKind::MidiNoteOn) == g_col.count(EventKind::MidiNoteOff));
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::MidiNoteOn && g_col.rec[i].ev.a == 60) {
      std::uint8_t k[8];
      NEED(notes_at(EventKind::MidiNoteOn, 0, g_col.rec[i].t, k) == 3u);
      CHECK(k[1] == 64);
      CHECK(k[2] == 67);
    }
  }
}

TEST_CASE("seq pfx: Chord on a step without a note does nothing and leaves the column's voice FX alone") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kPanning, 80)) == model::Status::Ok);  // a sample note with a Panning FX in the first column
  CHECK(p.set_step(1, 0, 1, fx_step(fx::kChord, 3)) == model::Status::Ok);               // a note-less row with a Chord in that column
  CHECK(p.set_step(1, 0, 2, fx_step(fx::kTune, 30)) == model::Status::Ok);               // and Tune
  CHECK(p.set_step(1, 0, 3, fx_step(fx::kArp, 5)) == model::Status::Ok);                 // and Arp
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(5, 0), 64);
  CHECK(g_col.count(EventKind::NoteOn) == 1u);       // the one note
  CHECK(g_col.count(EventKind::ModSet) == 1u);       // the Panning FX of the note
  CHECK(g_col.count(EventKind::ModEnd) == 0u);       // ... which these rows do not end
  CHECK(g_col.count(EventKind::MidiNoteOn) == 0u);
}

TEST_CASE("seq pfx: the step FX 23, 24, 33 and 36 reach the voices as the ModSet the LFO rows read (OS-PERF-LFO-STEP: a = the FX, b = the value, c = the step layer, flags = the group)") {
  struct Row {
    std::uint8_t fx;
    std::uint8_t group;
    std::uint8_t value;
  };
  const Row rows[4] = {{fx::kVolumeLfo, audio::kModGroupLfoVolume, 7}, {fx::kPanningLfo, audio::kModGroupLfoPanning, 12}, {fx::kFinetuneLfo, audio::kModGroupLfoFinetune, 20},
                       {fx::kPositionLfo, audio::kModGroupLfoPosition, 28}};
  for (const Row& r : rows) {
    pattern16();
    CHECK(seq_project().set_step(1, 0, 0, make_step(60, 1, r.fx, r.value)) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(2, 0), 64);
    const Rec* set = find_at(EventKind::ModSet, 0, at_us(0, 0));
    NEED(set != nullptr);
    CHECK(set->ev.a == r.fx);
    CHECK(set->ev.b == r.value);
    CHECK(set->ev.c == audio::kLayerFx1);
    CHECK(set->ev.flags == r.group);
  }
}

TEST_CASE("seq pfx: Arp plays the chord a note at a time, up, down or at random, at the rate's microsteps, from the note's own sample") {
  std::uint8_t n[8];
  // Up, rate 48 (value 6): 60 64 67 60 ... one note a step, the note before it ending as the next begins
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kArp, 6, fx::kChord, 5)) == model::Status::Ok);
  CHECK(seq_project().set_step(1, 0, 8, special_step(model::kNoteOff, kMidiInstr)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(10, 0), 64);
  const std::uint8_t up[8] = {60, 64, 67, 60, 64, 67, 60, 64};
  for (std::uint32_t row = 0; row < 8; ++row) {
    CAPTURE(row);
    NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(row, 0), n) == 1u);  // one note at a time
    CHECK(n[0] == up[row]);
    if (row > 0) {
      NEED(notes_at(EventKind::MidiNoteOff, 0, at_us(row, 0), n) == 1u);
      CHECK(n[0] == up[row - 1]);
    }
  }
  NEED(notes_at(EventKind::MidiNoteOff, 0, at_us(8, 0), n) == 1u);  // the Off ends the last one, and the arp with it
  CHECK(n[0] == up[7]);
  CHECK(g_col.count(EventKind::MidiNoteOn) == 8u);
  // Down (value 17 = 1 + 11 + 5): 67 64 60 67 ...
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kChord, 5, fx::kArp, 17)) == model::Status::Ok);  // the columns the other way round
  seq::Sequencer& s2 = make();
  s2.play_pattern(1);
  g_col.run(s2, 0, at_us(5, 0), 64);
  const std::uint8_t down[5] = {67, 64, 60, 67, 64};
  for (std::uint32_t row = 0; row < 5; ++row) {
    CAPTURE(row);
    NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(row, 0), n) == 1u);
    CHECK(n[0] == down[row]);
  }
}

TEST_CASE("seq pfx: every Arp rate starts the next note exactly its microsteps after the last") {
  for (std::uint32_t r = 0; r < 11; ++r) {
    pattern16();
    CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kArp, static_cast<std::uint8_t>(1 + r), fx::kChord, 20)) == model::Status::Ok);  // Min7, up
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(12, 0), 64);
    CAPTURE(r);
    const std::uint8_t want[5] = {60, 63, 67, 70, 60};  // Min7 is four notes
    for (std::uint32_t k = 0; k < 5; ++k) {
      const std::uint32_t j = kWantPeriod[r] * k;
      if (j >= 12u * 48u) {
        break;
      }
      std::uint8_t n[8];
      CAPTURE(k);
      NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(j / 48u, j % 48u), n) == 1u);
      CHECK(n[0] == want[k]);
    }
  }
}

TEST_CASE("seq pfx: Random arp draws from the chord's notes, reproducibly, and the Up and Down walks wrap") {
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kArp, 28, fx::kChord, 20)) == model::Status::Ok);  // random, 48 microsteps, Min7
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(16, 0), 64);
  const std::uint64_t h1 = g_col.hash;
  std::uint32_t seen = 0;  // a bit for each of the four notes
  std::uint32_t count = 0;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::MidiNoteOn) {
      const std::uint8_t a = g_col.rec[i].ev.a;
      CHECK((a == 60 || a == 63 || a == 67 || a == 70));
      seen |= a == 60 ? 1u : (a == 63 ? 2u : (a == 67 ? 4u : 8u));
      ++count;
    }
  }
  CHECK(count == 16u);   // one a step, the first included
  CHECK(seen == 15u);    // 16 draws of four notes reach all of them with this seed
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kArp, 28, fx::kChord, 20)) == model::Status::Ok);
  seq::Sequencer& s2 = make();
  s2.play_pattern(1);
  g_col.run(s2, 0, at_us(16, 0), 64);
  CHECK(g_col.hash == h1);  // the same seed, the same arpeggio
}

TEST_CASE("seq pfx: an Arp on a sample instrument retriggers its voice with the chord's notes, and the next note, Off or Cut ends it") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kChord, 4, fx::kArp, 7)) == model::Status::Ok);  // Min, up, 24 microsteps (two a step)
  CHECK(p.set_step(1, 0, 2, make_step(50, 1)) == model::Status::Ok);                                 // a plain note: the arp ends with the replaced note
  CHECK(p.set_step(1, 1, 0, make_step(60, 2, fx::kChord, 5, fx::kArp, 6)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 3, special_step(model::kNoteCut, 2)) == model::Status::Ok);
  CHECK(p.set_step(1, 2, 0, make_step(60, 3, fx::kChord, 5, fx::kArp, 6)) == model::Status::Ok);
  CHECK(p.set_step(1, 2, 3, special_step(model::kNoteOff, 3)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(8, 0), 64);
  // track 0: 60 63 67 at 0, 24, 48 microsteps, then the plain note 50 at step 2 and nothing after it (a Min is three notes: 60 63 67 60 63 ... at 24 apart up to step 2)
  const std::uint8_t want0[4] = {60, 63, 67, 60};
  for (std::uint32_t k = 0; k < 4; ++k) {
    const Rec* r = find_at(EventKind::NoteOn, 0, at_us(0, 24 * k));
    NEED(r != nullptr);
    CHECK(r->ev.a == want0[k]);
    CHECK(r->ev.b == 0);  // the instrument slot
    CHECK(r->ev.flags == (k == 0 ? 0 : audio::kEvRetrigger));
  }
  NEED(find_at(EventKind::NoteOn, 0, at_us(2, 0)) != nullptr);
  CHECK(find_at(EventKind::NoteOn, 0, at_us(2, 0))->ev.a == 50);
  CHECK(g_col.count(EventKind::NoteOn, 0) == 5u);  // four arp notes (the fifth would be at step 2 itself, where the plain note replaces it) and the plain one
  // track 1: one a step, and the Cut at step 3 stops them (a voice cut, no more notes)
  CHECK(g_col.count(EventKind::NoteOn, 1) == 3u);
  CHECK(find_at(EventKind::NoteCut, 1, at_us(3, 0)) != nullptr);
  // track 2: likewise, the Off ends the note and the arp
  CHECK(g_col.count(EventKind::NoteOn, 2) == 3u);
  CHECK(find_at(EventKind::NoteOff, 2, at_us(3, 0)) != nullptr);
}

TEST_CASE("seq pfx: an Arp with no Chord FX in the other column, or a chord left with only its root, does nothing, and is counted") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kArp, 6)) == model::Status::Ok);
  CHECK(p.set_step(1, 2, 0, make_step(127, kMidiInstr, fx::kArp, 6, fx::kChord, 5)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(4, 0), 64);
  CHECK(g_col.count(EventKind::MidiNoteOn, 0) == 1u);
  CHECK(s.stats().fx_ignored == 1u);
  // OS-PFXFIX: the arp clamps at 127 (min(base + offset, 127)) instead of losing the tones past it, so at 127 it still arpeggiates (the same note, a step each)
  CHECK(g_col.count(EventKind::MidiNoteOn, 2) == 4u);
}

// OS-PFXFIX: `n < 2 -> fx_ignored` can only ever mean "a chord left with only its root" for the CHORD (past 127); every arp mask, for every Chord value, holds the root and
// another tone below 12, so an arp is never ignored for that reason.
TEST_CASE("seq pfx: every arp mask holds the root and at least one more tone below 12") {
  for (std::uint32_t v = 0; v < 256; ++v) {
    CAPTURE(v);
    const std::uint32_t m = seq::arp_mask(static_cast<std::uint8_t>(v));
    CHECK((m & 1u) == 1u);
    CHECK((m & ~0x0FFFu) == 0u);
    CHECK(__builtin_popcount(m) >= 2);
  }
}

// docs/specs/OS-PFX.md: Chord 0 with an Arp is the chromatic arpeggio over the twelve semitones above the note.
TEST_CASE("seq pfx: an Arp with Chord 0 in the other column is the chromatic arpeggio, up and down, over offsets 0..11") {
  std::uint8_t n[8];
  for (std::uint32_t dir = 0; dir < 2; ++dir) {
    pattern16();
    CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kArp, static_cast<std::uint8_t>(1 + 11 * dir + 5), fx::kChord, 0)) == model::Status::Ok);  // 48 microsteps: a step a note
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(14, 0), 64);
    CAPTURE(dir);
    for (std::uint32_t row = 0; row < 14; ++row) {
      CAPTURE(row);
      NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(row, 0), n) == 1u);
      CHECK(n[0] == (dir == 0 ? 60 + row % 12 : 71 - row % 12));  // 60 .. 71, 60, 61 / 71 .. 60, 71, 70
    }
  }
  // a base note near the top plays 127 for the offsets past it (min(base + offset, 127)) and goes round again: up 120..127, 127 x 4, then 120 (OS-PFXFIX)
  for (std::uint32_t dir = 0; dir < 2; ++dir) {
    pattern16();
    CHECK(seq_project().set_step(1, 0, 0, make_step(120, kMidiInstr, fx::kArp, static_cast<std::uint8_t>(1 + 11 * dir + 5), fx::kChord, 0)) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(14, 0), 64);
    for (std::uint32_t row = 0; row < 14; ++row) {
      CAPTURE(dir);
      CAPTURE(row);
      NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(row, 0), n) == 1u);
      const std::uint32_t off = dir == 0 ? row % 12 : 11 - row % 12;
      CHECK(n[0] == (120 + off > 127 ? 127 : 120 + off));
    }
  }
  // Chord 0 alone is the plain note
  one_note(60, kMidiInstr, fx::kChord, 0);
  CHECK(g_col.count(EventKind::MidiNoteOn) == 1u);
}

// docs/specs/OS-PFX.md: the arp visits the offsets below 12 only; the chord itself sounds all of them.
TEST_CASE("seq pfx: an Arp never plays a chord's offsets 12 or 14, the chord alone does") {
  struct Case {
    std::uint8_t chord;
    std::uint8_t arp[2];
    std::uint8_t block_len;
    std::uint8_t block[3];
  };
  const Case cases[] = {{9, {60, 65}, 3, {60, 65, 72}}, {11, {60, 67}, 3, {60, 67, 72}}, {12, {60, 67}, 3, {60, 67, 74}}};
  for (const Case& c : cases) {
    CAPTURE(int(c.chord));
    std::uint8_t n[8];
    one_note(60, kMidiInstr, fx::kChord, c.chord);
    NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(0, 0), n) == c.block_len);
    for (std::uint32_t i = 0; i < c.block_len; ++i) {
      CHECK(n[i] == c.block[i]);
    }
    pattern16();
    CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kArp, 6, fx::kChord, c.chord)) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(6, 0), 64);
    for (std::uint32_t row = 0; row < 6; ++row) {
      NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(row, 0), n) == 1u);
      CHECK(n[0] == c.arp[row % 2]);
    }
  }
}

TEST_CASE("seq pfx: a down arp starts on the chord's highest tone and wraps in order") {
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kArp, 17, fx::kChord, 28)) == model::Status::Ok);  // 57A = 0 5 7 10, down, a step a note
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(6, 0), 64);
  const std::uint8_t want[6] = {70, 67, 65, 60, 70, 67};
  for (std::uint32_t row = 0; row < 6; ++row) {
    std::uint8_t n[8];
    NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(row, 0), n) == 1u);
    CHECK(n[0] == want[row]);
  }
}

// OS-PFXFIX (docs/specs/OS-PFX.md): the arp clamps at 127 (min(base + offset, 127)), the chord skips the tones of 128 and up.
TEST_CASE("seq pfx: the arp clamps at 127 and loses no tone, the chord still skips the tones past it") {
  std::uint8_t n[8];
  // Chord 13 "279" (offsets 0 2 7 9) arpeggiated up from 120: 120 122 127 129->127, round again
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(120, kMidiInstr, fx::kArp, 6, fx::kChord, 13)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(8, 0), 64);
  const std::uint8_t up[8] = {120, 122, 127, 127, 120, 122, 127, 127};
  for (std::uint32_t row = 0; row < 8; ++row) {
    CAPTURE(row);
    NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(row, 0), n) == 1u);
    CHECK(n[0] == up[row]);
  }
  // down: 9 first (129 -> 127), then 7, 2, 0
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(120, kMidiInstr, fx::kArp, 17, fx::kChord, 13)) == model::Status::Ok);
  seq::Sequencer& s2 = make();
  s2.play_pattern(1);
  g_col.run(s2, 0, at_us(5, 0), 64);
  const std::uint8_t down[5] = {127, 127, 122, 120, 127};
  for (std::uint32_t row = 0; row < 5; ++row) {
    CAPTURE(row);
    NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(row, 0), n) == 1u);
    CHECK(n[0] == down[row]);
  }
  // an arp on 127 itself plays 127 each time; it is not ignored
  one_note(127, kMidiInstr, fx::kArp, 6, fx::kChord, 13);
  CHECK(s.stats().fx_ignored == 0u);
  // the CHORD (block) at 127 and at 120 keeps skipping the tones of 128 and up: Chord 13 at 127 is the root alone and is counted, at 120 it is 120 122 127
  one_note(127, kMidiInstr, fx::kChord, 13);
  NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(0, 0), n) == 1u);
  CHECK(n[0] == 127);
  one_note(120, kMidiInstr, fx::kChord, 13);
  NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(0, 0), n) == 3u);
  CHECK(n[0] == 120);
  CHECK(n[1] == 122);
  CHECK(n[2] == 127);
}

TEST_CASE("seq pfx: the walks are position walks: a 3-tone chord and the chromatic one, up and down, hand-computed") {
  std::uint8_t n[8];
  // Chord 1 "27" (0 2 7). Up: from 0 -> 0 (pos 1), 2 (pos 3), 7 (pos 8), 0 (pos 8..11 empty, wraps) ... Down: from 11 -> 7 (pos 6), 2 (pos 1), 0 (pos 0 -> 0 hit, pos 11), 7
  const struct {
    std::uint8_t arp;
    std::uint8_t chord;
    std::uint8_t want[8];
  } cases[] = {
      {6, 1, {60, 62, 67, 60, 62, 67, 60, 62}},                       // up
      {17, 1, {67, 62, 60, 67, 62, 60, 67, 62}},                      // down
      {6, 0, {60, 61, 62, 63, 64, 65, 66, 67}},                       // chromatic up
      {17, 0, {71, 70, 69, 68, 67, 66, 65, 64}},                      // chromatic down
  };
  for (const auto& c : cases) {
    pattern16();
    CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kArp, c.arp, fx::kChord, c.chord)) == model::Status::Ok);
    seq::Sequencer& s = make();
    s.play_pattern(1);
    g_col.run(s, 0, at_us(8, 0), 64);
    for (std::uint32_t row = 0; row < 8; ++row) {
      CAPTURE(int(c.arp));
      CAPTURE(int(c.chord));
      CAPTURE(row);
      NEED(notes_at(EventKind::MidiNoteOn, 0, at_us(row, 0), n) == 1u);
      CHECK(n[0] == c.want[row]);
    }
  }
}

namespace {

// The notes that this OS's Random walk can pick next (offsets, as a bit set) from position `pos`: the first set bit of `mask` at or above (pos + r) mod 12, for r = 0..10; written
// out again here, apart from the sequencer's own search.
std::uint32_t random_reach(std::uint32_t mask, std::uint32_t pos) {
  std::uint32_t reach = 0;
  for (std::uint32_t r = 0; r < 11; ++r) {
    for (std::uint32_t i = 0; i < 12; ++i) {
      const std::uint32_t o = (pos + r + i) % 12;
      if ((mask >> o) & 1u) {
        reach |= 1u << o;
        break;
      }
    }
  }
  return reach;
}

// A Random arp (rate 6 microsteps, 8 notes a step) of `chord` from base 60 on a 128-step pattern, run for `loops` pattern passes; the offsets of its notes in order (each pass restarts
// the walk at position 0). Returns how many were recorded, into `out` (up to `cap`).
std::uint32_t random_walk(std::uint8_t chord, std::uint32_t loops, std::uint8_t* out, std::uint32_t cap, std::uint32_t* starts) {
  fresh_project(4, 6);
  model::Project& p = seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, 128) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kArp, 33, fx::kChord, chord)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  std::uint32_t total = 0;
  for (std::uint32_t l = 0; l < loops; ++l) {
    g_col.n = 0;
    g_col.run(s, at_us(128u * l, 0), at_us(128u * (l + 1u), 0), 64);
    if (starts != nullptr) {
      starts[l] = total;
    }
    for (std::uint32_t i = 0; i < g_col.n && total < cap; ++i) {
      if (g_col.rec[i].ev.kind == EventKind::MidiNoteOn && g_col.rec[i].ev.track == 0) {
        out[total++] = static_cast<std::uint8_t>(g_col.rec[i].ev.a - 60u);
      }
    }
  }
  return total;
}

}  // namespace

constexpr std::uint8_t kRandomChords[3] = {5, 26, 0};

TEST_CASE("seq pfx: Random arp: every pick is reachable from the position, and the chromatic one never repeats a note") {
  static std::uint8_t offs[4096];
  // Chord 5 "47" (0 4 7), Chord 26 "48A" (0 4 8 10) and Chord 0 (chromatic): the first pick of each pass starts at position 0, the others at the last offset + 1
  for (const std::uint8_t chord : kRandomChords) {
    CAPTURE(int(chord));
    static std::uint32_t starts[2];
    const std::uint32_t got = random_walk(chord, 2, offs, 4096, starts);
    NEED(got > 1500u);
    const std::uint32_t mask = seq::arp_mask(chord);
    std::uint32_t seen = 0;
    for (std::uint32_t i = 0; i < got; ++i) {
      const bool first = i == 0 || i == starts[1];
      const std::uint32_t pos = first ? 0u : (offs[i - 1] + 1u) % 12u;
      CAPTURE(i);
      NEED(offs[i] < 12u);
      CHECK(((random_reach(mask, pos) >> offs[i]) & 1u) == 1u);
      seen |= 1u << offs[i];
      if (chord == 0 && i > 0 && !first) {
        CHECK(offs[i] != offs[i - 1]);  // r never reaches (pos + 11): the note just played, one below the position, is not a start of the search
      }
    }
    CHECK(seen == mask);  // all of them come up in two thousand draws
  }
}

// Chord 11 "7C": the arp visits 0 and 7. This OS's Random is NOT uniform over the tones (whether the official firmware's is the same is Q, docs/specs/OS-PFX.md). From position p the search starts at (p + r) mod 12 for r = 0..10 (equally likely) and takes
// the first set offset upward: after a 0 (p = 1) the starts 1..11 give 7 for 1..7 and 0 for 8..11 (P(0) = 4/11); after a 7 (p = 8) the starts 8..11, 0 give 0 and 1..6 give 7 (P(0) = 5/11).
// The chain's stationary share of 0 is a with a = 4a/11 + 5(1 - a)/11, a = 5/12 = 0.4167; a uniform pick would be 0.5.
TEST_CASE("seq pfx: Random arp over the tones 0 and 7 is not uniform: 5/12 of the notes are the root (the search rule)") {
  static std::uint8_t offs[4096];
  std::uint32_t zeros = 0, after0 = 0, after0_7 = 0, after7 = 0, after7_0 = 0;
  const std::uint32_t total = random_walk(11, 4, offs, 4096, nullptr);
  NEED(total > 3000u);
  for (std::uint32_t i = 0; i < total; ++i) {
    CHECK((offs[i] == 0 || offs[i] == 7));
    zeros += offs[i] == 0 ? 1u : 0u;
    if (i > 0) {  // (the four pass starts are counted as transitions too: four of four thousand)
      if (offs[i - 1] == 0) {
        ++after0;
        after0_7 += offs[i] == 7 ? 1u : 0u;
      } else {
        ++after7;
        after7_0 += offs[i] == 0 ? 1u : 0u;
      }
    }
  }
  // per mille, loose: uniform would be 500 / 500 / 500; the search rule gives 417 / 636 (7 after 0) / 455 (0 after 7)
  CHECK(zeros * 1000u / total > 380u);
  CHECK(zeros * 1000u / total < 455u);
  CHECK(after0_7 * 1000u / after0 > 590u);
  CHECK(after0_7 * 1000u / after0 < 690u);
  CHECK(after7_0 * 1000u / after7 > 400u);
  CHECK(after7_0 * 1000u / after7 < 510u);
}

TEST_CASE("seq pfx: a note with an empty instrument and a Chord + Arp plays nothing, and no arp note of the earlier instrument follows") {
  constexpr std::uint8_t kEmpty = 8;  // slot 7: no instrument (fresh_project makes the sample slots 0..5 and the MIDI one at 10)
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.instrument(7) == nullptr);  // Project::instrument answers null for a free slot
  // track 0: a sample note, then a note of the empty slot with Chord 1 + Arp 5 (two a step); track 1: the same after a MIDI note; track 2: Arp first column, Chord second
  CHECK(p.set_step(1, 0, 0, make_step(60, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 2, make_step(64, kEmpty, fx::kChord, 5, fx::kArp, 7)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 0, make_step(60, kMidiInstr)) == model::Status::Ok);
  CHECK(p.set_step(1, 1, 2, make_step(64, kEmpty, fx::kChord, 5, fx::kArp, 7)) == model::Status::Ok);
  CHECK(p.set_step(1, 2, 0, make_step(60, 2)) == model::Status::Ok);
  CHECK(p.set_step(1, 2, 2, make_step(64, kEmpty, fx::kArp, 28, fx::kChord, 20)) == model::Status::Ok);  // a random arp
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(8, 0), 64);
  CHECK(g_col.count(EventKind::NoteOn, 0) == 1u);      // the first note only: nothing at step 2 and no retrigger on the sample slot after it
  CHECK(g_col.count(EventKind::MidiNoteOn, 1) == 1u);  // likewise on the MIDI slot ...
  CHECK(g_col.count(EventKind::MidiNoteOff, 1) == 1u); // ... whose note the new one ended, once
  CHECK(g_col.count(EventKind::NoteOn, 2) == 1u);
}

TEST_CASE("seq pfx: a stop ends an arpeggio, and the next run starts clean") {
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, kMidiInstr, fx::kArp, 6, fx::kChord, 5)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(2, 30), 64);
  CHECK(g_col.count(EventKind::MidiNoteOn) == 3u);
  s.stop();
  g_col.run(s, at_us(2, 30), at_us(2, 30) + 64, 64);
  CHECK(g_col.count(EventKind::MidiNoteOff) == 3u);  // every note on went off
  const std::uint32_t ons = g_col.count(EventKind::MidiNoteOn);
  g_col.run(s, at_us(2, 30) + 64, at_us(8, 0), 64);
  CHECK(g_col.count(EventKind::MidiNoteOn) == ons);  // stopped: no more arp notes
}

TEST_CASE("seq pfx: Tune shifts the note by value - 24 semitones on a sample track and on a MIDI track, held to 0..127") {
  struct Case {
    std::uint8_t note, value, want;
  };
  const Case cases[] = {{60, 24, 60}, {60, 31, 67}, {60, 17, 53}, {60, 0, 36}, {60, 48, 84}, {120, 48, 127}, {5, 0, 0},  {0, 0, 0},
                        {127, 48, 127}, {100, 40, 116}, {60, 25, 61}, {60, 23, 59}, {10, 3, 0}, {126, 26, 127}, {60, 100, 84} /* past the range reads as 48 */};
  for (const Case& c : cases) {
    CAPTURE(int(c.note));
    CAPTURE(int(c.value));
    one_note(c.note, 1, fx::kTune, c.value);
    const Rec* r = find_at(EventKind::NoteOn, 0, at_us(0, 0));
    NEED(r != nullptr);
    CHECK(r->ev.a == c.want);
    one_note(c.note, kMidiInstr, fx::kTune, c.value);
    r = find_at(EventKind::MidiNoteOn, 0, at_us(0, 0));
    NEED(r != nullptr);
    CHECK(r->ev.a == c.want);
  }
}

TEST_CASE("seq pfx: Tune acts when a note starts: the first column wins, a step without a note ignores it, and a beat slice instrument keeps its slice") {
  // the first column wins over the second
  one_note(60, 1, fx::kTune, 31, fx::kTune, 17);
  NEED(find_at(EventKind::NoteOn, 0, at_us(0, 0)) != nullptr);
  CHECK(find_at(EventKind::NoteOn, 0, at_us(0, 0))->ev.a == 67);
  // a step without a note: no event at all (a held note is not retuned)
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 1)) == model::Status::Ok);
  CHECK(seq_project().set_step(1, 0, 1, fx_step(fx::kTune, 36)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(4, 0), 64);
  CHECK(g_col.count(EventKind::NoteOn) == 1u);
  CHECK(g_col.count(EventKind::ModSet) == 0u);
  // a beat slice instrument (play mode 5): the note picks the slice, so Tune leaves it, counted as ignored
  pattern16();
  model::Instrument slice;
  model::init_instrument(slice, model::InstrumentKind::Sample);
  slice.sampler.play_mode = 5;
  CHECK(seq_project().set_instrument(5, slice) == model::Status::Ok);
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 6, fx::kTune, 31)) == model::Status::Ok);
  seq::Sequencer& s2 = make();
  s2.play_pattern(1);
  g_col.run(s2, 0, at_us(2, 0), 64);
  NEED(find_at(EventKind::NoteOn, 0, at_us(0, 0)) != nullptr);
  CHECK(find_at(EventKind::NoteOn, 0, at_us(0, 0))->ev.a == 60);
  CHECK(s2.stats().fx_ignored >= 1u);
  // Tune with Roll: the repeats of the roll start from the tuned note
  pattern16();
  CHECK(seq_project().set_step(1, 0, 0, make_step(60, 1, fx::kTune, 31, fx::kRoll, 4)) == model::Status::Ok);
  seq::Sequencer& s3 = make();
  s3.play_pattern(1);
  g_col.run(s3, 0, at_us(2, 0), 64);
  CHECK(g_col.count(EventKind::NoteOn, 0) >= 4u);
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.kind == EventKind::NoteOn) {
      CHECK(g_col.rec[i].ev.a == 67);
    }
  }
}

TEST_CASE("seq pfx: Slide goes to the voice as a modulation before the note, ends with the next note's columns, and the first column wins") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kTune, 28, fx::kSlideUp, 40)) == model::Status::Ok);  // Tune +4 in the first column, Slide up 40 in the second
  CHECK(p.set_step(1, 0, 2, make_step(62, 1)) == model::Status::Ok);                                    // a plain note ends it
  CHECK(p.set_step(1, 0, 3, fx_step(fx::kSlideDown, 100)) == model::Status::Ok);                        // a row without a note slides the sounding voice
  CHECK(p.set_step(1, 1, 0, make_step(60, 1, fx::kSlideUp, 16, fx::kSlideDown, 200)) == model::Status::Ok);  // both columns: the first wins
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(5, 0), 64);
  // track 0: the ModSet comes with the note, before it, at the same sample
  const Rec* mod = nullptr;
  std::uint32_t mod_i = 0, on_i = 0;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    if (g_col.rec[i].ev.track == 0 && g_col.rec[i].t == at_us(0, 0)) {
      if (g_col.rec[i].ev.kind == EventKind::ModSet && g_col.rec[i].ev.flags == audio::kModGroupSlide) {
        mod = &g_col.rec[i];
        mod_i = i;
      }
      if (g_col.rec[i].ev.kind == EventKind::NoteOn) {
        on_i = i;
      }
    }
  }
  NEED(mod != nullptr);
  CHECK(mod->ev.a == fx::kSlideUp);
  CHECK(mod->ev.b == 40);
  CHECK(mod->ev.c == audio::kLayerFx2);
  CHECK(mod_i < on_i);
  CHECK(g_col.nth(EventKind::NoteOn, 0, 0)->ev.a == 64);  // the Tune of the first column
  // the plain note ends the Slide
  const Rec* end = find_at(EventKind::ModEnd, 0, at_us(2, 0));
  NEED(end != nullptr);
  CHECK(end->ev.flags == audio::kModGroupSlide);
  // the row without a note starts a new one
  const Rec* down = find_at(EventKind::ModSet, 0, at_us(3, 0));
  NEED(down != nullptr);
  CHECK(down->ev.flags == audio::kModGroupSlide);
  CHECK(down->ev.a == fx::kSlideDown);
  CHECK(down->ev.b == 100);
  // track 1, both columns: Slide up of the first column wins; the second column's change makes no event of its own
  const Rec* up = find_at(EventKind::ModSet, 1, at_us(0, 0));
  NEED(up != nullptr);
  CHECK(up->ev.a == fx::kSlideUp);
  CHECK(up->ev.b == 16);
  CHECK(up->ev.c == audio::kLayerFx1);
  CHECK(g_col.count(EventKind::ModSet, 1) == 1u);
}

TEST_CASE("seq ifx: Glide, Overdrive and Bit depth go to the voices as modulations of their own groups, a plain note ends them (OS-IFX1)") {
  pattern16();
  model::Project& p = seq_project();
  CHECK(p.set_step(1, 0, 0, make_step(60, 1, fx::kGlide, 10, fx::kOverdrive, 40)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 1, make_step(60, 1, fx::kBitDepth, 8)) == model::Status::Ok);
  CHECK(p.set_step(1, 0, 2, make_step(62, 1)) == model::Status::Ok);
  seq::Sequencer& s = make();
  s.play_pattern(1);
  g_col.run(s, 0, at_us(4, 0), 64);
  bool seen_glide = false, seen_drive = false;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    const audio::SeqEvent& e = g_col.rec[i].ev;
    if (e.kind != EventKind::ModSet || e.track != 0 || g_col.rec[i].t != at_us(0, 0)) {
      continue;
    }
    if (e.flags == audio::kModGroupGlide) {
      seen_glide = true;
      CHECK(e.a == audio::kFxGlide);
      CHECK(e.b == 10);
    } else if (e.flags == audio::kModGroupOverdrive) {
      seen_drive = true;
      CHECK(e.a == audio::kFxOverdrive);
      CHECK(e.b == 40);
    }
  }
  CHECK(seen_glide);
  CHECK(seen_drive);
  // step 1: the glide and the overdrive end (their columns are gone), the bit depth starts
  bool bits = false, glide_end = false, drive_end = false;
  for (std::uint32_t i = 0; i < g_col.n; ++i) {
    const audio::SeqEvent& e = g_col.rec[i].ev;
    if (e.track != 0 || g_col.rec[i].t != at_us(1, 0)) {
      continue;
    }
    if (e.kind == EventKind::ModSet && e.flags == audio::kModGroupBitDepth) {
      bits = true;
      CHECK(e.a == audio::kFxBitDepth);
      CHECK(e.b == 8);
    }
    glide_end = glide_end || (e.kind == EventKind::ModEnd && e.flags == audio::kModGroupGlide);
    drive_end = drive_end || (e.kind == EventKind::ModEnd && e.flags == audio::kModGroupOverdrive);
  }
  CHECK(bits);
  CHECK(glide_end);
  CHECK(drive_end);
  const Rec* end = find_at(EventKind::ModEnd, 0, at_us(2, 0));
  NEED(end != nullptr);
  CHECK(end->ev.flags == audio::kModGroupBitDepth);
}

TEST_CASE("seq pfx: the same pattern gives the same events over any block size, random arps and chords included") {
  const auto build = [] {
    pattern16(6);
    model::Project& p = seq_project();
    for (std::uint32_t r = 0; r < 16; ++r) {
      CHECK(p.set_step(1, 0, r, make_step(static_cast<std::uint8_t>(48 + r), kMidiInstr, fx::kChord, static_cast<std::uint8_t>(1 + r % 29), fx::kArp, static_cast<std::uint8_t>(1 + (r * 7) % 33))) == model::Status::Ok);
      CHECK(p.set_step(1, 1, r, make_step(static_cast<std::uint8_t>(50 + r), 1, fx::kTune, static_cast<std::uint8_t>(r * 3), fx::kSlideUp, static_cast<std::uint8_t>(r * 16))) == model::Status::Ok);
      CHECK(p.set_step(1, 2, r, make_step(static_cast<std::uint8_t>(52 + r), 2, fx::kChord, static_cast<std::uint8_t>(1 + r % 29), fx::kArp, static_cast<std::uint8_t>(23 + r))) == model::Status::Ok);
    }
  };
  build();
  seq::Sequencer& a = make();
  a.play_pattern(1);
  g_col.run(a, 0, at_us(40, 0), 64);
  const std::uint64_t h64 = g_col.hash;
  const std::uint32_t total = g_col.total_events;
  CHECK(total > 300u);
  build();
  seq::Sequencer& b = make();
  b.play_pattern(1);
  const std::uint32_t sizes[] = {1, 7, 64, 5, 33, 130, 2, 61};
  g_col.run_sizes(b, 0, at_us(40, 0), sizes, 8);
  CHECK(g_col.hash == h64);
  CHECK(g_col.total_events == total);
}

TEST_CASE("seq pfx: random Chord, Arp and Tune rows on MIDI and sample tracks never leave a MIDI note hanging, never more than four sound on a track, and a stop ends them all") {
  pattern16(4);
  model::Project& p = seq_project();
  std::uint32_t lcg = 12345u;
  const auto next = [&lcg](std::uint32_t mod) {
    lcg = lcg * 1664525u + 1013904223u;
    return (lcg >> 8) % mod;
  };
  for (std::uint32_t t = 0; t < 4; ++t) {
    for (std::uint32_t r = 0; r < 16; ++r) {
      model::Step s = model::kEmptyStep;
      const std::uint32_t kind = next(10);
      if (kind < 6) {
        s = make_step(static_cast<std::uint8_t>(next(128)), static_cast<std::uint8_t>(t < 2 ? kMidiInstr : 1));
      } else if (kind == 6) {
        s = special_step(model::kNoteOff, static_cast<std::uint8_t>(t < 2 ? kMidiInstr : 1));
      } else if (kind == 7) {
        s = special_step(model::kNoteCut, static_cast<std::uint8_t>(t < 2 ? kMidiInstr : 1));
      }
      static const std::uint8_t types[] = {fx::kChord, fx::kArp, fx::kTune, fx::kSlideUp, fx::kRoll, fx::kGate, fx::kChance, fx::kVelocity, fx::kMicroMove, 0};
      s.fx[0] = {types[next(10)], static_cast<std::uint8_t>(next(256))};
      s.fx[1] = {types[next(10)], static_cast<std::uint8_t>(next(256))};
      CHECK(p.set_step(1, t, r, s) == model::Status::Ok);
    }
  }
  seq::Sequencer& s = make();
  s.play_pattern(1);
  std::int32_t sounding[16][128] = {};
  std::int32_t per_track[16] = {};
  audio::SeqEvent out[audio::kMaxEventsPerBlock];
  const auto run = [&](std::uint64_t from, std::uint64_t to) {
    for (std::uint64_t t = from; t < to; t += 64) {
      const std::uint32_t c = s.step(t, 64, out, audio::kMaxEventsPerBlock);
      for (std::uint32_t i = 0; i < c; ++i) {
        const audio::SeqEvent& e = out[i];
        if (e.kind == EventKind::MidiNoteOn) {
          CHECK(e.a < 128);
          ++sounding[e.c][e.a];
          ++per_track[e.track];
          CHECK(per_track[e.track] <= 4);
        } else if (e.kind == EventKind::MidiNoteOff) {
          --sounding[e.c][e.a];
          --per_track[e.track];
          CHECK(sounding[e.c][e.a] >= 0);
          CHECK(per_track[e.track] >= 0);
        }
      }
    }
  };
  run(0, at_us(16 * 6, 0));
  s.stop();
  run(at_us(16 * 6, 0), at_us(16 * 6, 0) + 64 * 4);
  for (std::uint32_t c = 0; c < 16; ++c) {
    for (std::uint32_t n = 0; n < 128; ++n) {
      CHECK_MESSAGE(sounding[c][n] == 0, "channel " << c << " note " << n);
    }
  }
  CHECK(s.stats().microstep_overrun == 0u);
  CHECK(s.stats().queue_overflow == 0u);
}
