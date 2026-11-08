// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/demo_project.h>

#include <o3os/app/pattern_data.h>
#include <o3os/core/learn.h>
#include <o3os/model/event_list.h>
#include <o3os/model/params.h>

#include <cstring>
#include <memory>

namespace o3os::sim {

namespace {

constexpr const char* kNames[kDemoInstruments] = {"KICK", "SNARE", "HAT", "ACID", "BASS", "LEAD", "PAD", "PERC", "SYNTH", "DRUMS"};

void set(model::Project& p, unsigned slot, unsigned field, float value) noexcept {
  (void)p.set_param(core::param_id(core::ParamKind::Instrument, slot, field), value);
}

}  // namespace

// Pattern 1 is the demo of the Pattern screen (app::fill_demo: 12 tracks of 64 steps), the 12 track slots are named and typed as its headers
// are, and the song has the one position that plays it: what the platform's fresh document has, filled (OS-009: the `document` command shows
// the same picture the demo does, and the edit scenarios start from it).
void fill_demo_pattern(model::Project& p) noexcept {
  auto data = std::make_unique<app::PatternData>();
  app::fill_demo(*data);
  (void)p.create_pattern(1, app::kSteps);
  for (unsigned t = 0; t < static_cast<unsigned>(app::kTracks); ++t) {
    (void)p.set_steps(1, t, 0, data->steps[t], static_cast<std::uint32_t>(app::kSteps));
    model::TrackSlot slot = *p.track(t);
    const char type = app::track_type(static_cast<int>(t));
    slot.type = type == 'M' ? model::TrackType::Midi : (type == 'E' ? model::TrackType::ExternalAudio : model::TrackType::Audio);
    (void)p.set_track(t, slot);
    (void)p.set_track_name(t, app::track_name(static_cast<int>(t)), std::strlen(app::track_name(static_cast<int>(t))));
  }
  (void)p.append_order(1);
}

void fill_demo_project(model::Project& p) noexcept {
  namespace ip = model::instrument_param;
  p.reset();
  fill_demo_pattern(p);
  for (unsigned i = 0; i < kDemoInstruments; ++i) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::memcpy(in.name, kNames[i], std::strlen(kNames[i]));
    (void)p.set_instrument(i, in);
    const float f = static_cast<float>(i);
    set(p, i, ip::Volume, static_cast<float>(100 - 4 * static_cast<int>(i)));
    set(p, i, ip::Panning, static_cast<float>(50 + 12 * (static_cast<int>(i % 5) - 2)));
    set(p, i, ip::Tune, static_cast<float>(static_cast<int>(i % 7) - 3));
    set(p, i, ip::FineTune, static_cast<float>(10 * (static_cast<int>(i % 5) - 2)));
    set(p, i, ip::Glide, static_cast<float>(100 * (i % 4)));
    set(p, i, ip::FilterEnable, i % 3 == 1 ? 0.0f : 1.0f);
    set(p, i, ip::FilterType, static_cast<float>(i % 3));
    set(p, i, ip::Cutoff, 0.30f + 0.06f * f);
    set(p, i, ip::Resonance, 0.35f * static_cast<float>(i % 6));
    set(p, i, ip::DelaySend, static_cast<float>(10 * (i % 5)));
    set(p, i, ip::ReverbSend, static_cast<float>(8 * (i % 7)));
    set(p, i, ip::Overdrive, static_cast<float>(5 * (i % 4)));
    set(p, i, ip::BitDepth, static_cast<float>(16 - static_cast<int>(i % 3) * 4));
    // The amplitude envelope (volume, envelope 0) and the cutoff's (envelope 2).
    set(p, i, ip::envelope(0, ip::EnvEnable), 1.0f);
    set(p, i, ip::envelope(0, ip::EnvAmount), 1.0f);
    set(p, i, ip::envelope(0, ip::EnvAttack), static_cast<float>(10 + 40 * (i % 3)));
    set(p, i, ip::envelope(0, ip::EnvDecay), static_cast<float>(200 + 150 * (i % 4)));
    set(p, i, ip::envelope(0, ip::EnvSustain), 0.6f + 0.05f * static_cast<float>(i % 5));
    set(p, i, ip::envelope(0, ip::EnvRelease), static_cast<float>(300 + 250 * (i % 3)));
    set(p, i, ip::envelope(2, ip::EnvEnable), 1.0f);
    set(p, i, ip::envelope(2, ip::EnvAmount), 0.5f);
    set(p, i, ip::envelope(2, ip::EnvAttack), 1000.0f + 100.0f * f);
    set(p, i, ip::envelope(2, ip::EnvDecay), 2000.0f);
    set(p, i, ip::envelope(2, ip::EnvSustain), 0.2f);
    set(p, i, ip::envelope(2, ip::EnvRelease), 1500.0f);
  }
}

void fill_sound_project(model::Project& p) noexcept {
  namespace ip = model::instrument_param;
  p.reset();
  (void)p.create_pattern(1, 16);
  (void)p.append_order(1);
  (void)p.set_tempo_x100(12000);  // a step is 125 ms
  constexpr const char* kSoundNames[kSoundInstruments] = {"SINE", "NOISE", "PLUCK", "SINE2"};
  for (unsigned i = 0; i < kSoundInstruments; ++i) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::memcpy(in.name, kSoundNames[i], std::strlen(kSoundNames[i]));
    in.sampler.play_mode = i == 2u ? std::uint8_t{0} : std::uint8_t{1};  // the looped sounds sustain; the pluck is a one shot
    (void)p.set_instrument(i, in);
    set(p, i, ip::Volume, 50.0f);
    set(p, i, ip::Panning, 50.0f);
    set(p, i, ip::Tune, 0.0f);
    set(p, i, ip::FineTune, 0.0f);
    set(p, i, ip::Glide, 0.0f);
    set(p, i, ip::FilterEnable, 0.0f);
    set(p, i, ip::DelaySend, 0.0f);
    set(p, i, ip::ReverbSend, 0.0f);
    set(p, i, ip::Overdrive, 0.0f);
    set(p, i, ip::BitDepth, 16.0f);
    set(p, i, ip::envelope(0, ip::EnvEnable), 0.0f);
  }
}

void fill_insert_project(model::Project& p) noexcept {
  fill_demo_project(p);
  (void)p.add_module("biquad", 6, 1, 8);
  (void)p.add_module("reverb", 6, 1, 8);
  const struct {
    std::uint8_t where, slot, module, flags;
  } kRecords[3] = {{0, 0, 0, 0}, {0, 1, 1, model::kInsertFlagBypass}, {2, 0, 0, 0}};
  for (const auto& r : kRecords) {
    model::InsertRecord rec{};
    rec.where = r.where;
    rec.slot = r.slot;
    rec.module = r.module;
    rec.flags = r.flags;
    (void)p.set_insert(rec);
  }
}

void fill_tb303_project(model::Project& p) noexcept {
  p.reset();
  (void)p.create_pattern(1, 16);
  (void)p.append_order(1);
  (void)p.set_tempo_x100(15000);  // a step is 100 ms
  (void)p.add_module("tb303", 5, 1, 8);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Module);
  std::memcpy(in.name, "TB 303", 6);
  in.module_index = 0;
  std::memcpy(in.params, kTb303Params, sizeof kTb303Params);
  in.param_len = sizeof kTb303Params;
  (void)p.set_instrument(0, in);
  for (const Tb303Step& s : kTb303Line) {
    model::Step step = model::kEmptyStep;
    if (s.midi == 0u) {
      step.note = model::kNoteOff;
    } else {
      step.note = static_cast<std::uint8_t>(s.midi + 1u);
      step.instr = 1;
      step.vol = s.velocity;
    }
    (void)p.set_step(1, 0, s.row, step);
  }
}

// OS-MIDIREC S1 (scenario `document events`): the document of the EVENTS list's pictures and the budget sweep. Pattern 1 of 64
// steps on the default 12 tracks, with every row kind in the first four tracks: a note with a folded OFF (one row, a LEN), a
// micro-moved note listed after its step's CC rows, a gated note with a Chord FX (the G and + flags), a note that runs on past
// the track's end (LEN >), an OFF that ends no note above it, a dim sample-instrument row, an inherited instrument at the top of
// a track (IN ??), and the CC rows of what really goes out (a MIDI instrument's controller and PROG, the Config numbers on the
// notes channel, an unassigned number). Instrument 1 a sample, instrument 49 (slot 48) a MIDI instrument on channel 3 whose
// CC A is controller 74 and CC B a program change; the project's numbers: CC A 74, CC C 71, the rest unassigned.
void fill_events_project(model::Project& p) noexcept {
  namespace ev = model::events;
  p.reset();
  (void)p.create_pattern(1, 64);
  (void)p.append_order(1);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  std::memcpy(in.name, "KICK", 4);
  (void)p.set_instrument(0, in);
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = 3;
  in.midi_cc[0] = model::kMidiCcNumberMax - 53u;  // 75: controller 74
  in.midi_cc[1] = model::kMidiCcProg;              // CC B: a program change
  std::memcpy(in.name, "MIDI 3", 6);
  (void)p.set_instrument(48, in);
  model::MidiState midi{};
  midi.notes_channel = 0;  // the device's notes channel: 1 on the wire
  for (unsigned i = 0; i < model::kMidiCcSlots; ++i) {
    midi.cc_number[i] = model::kMidiCcUnassigned;
  }
  midi.cc_number[0] = 74;
  midi.cc_number[2] = 71;
  (void)p.set_midi(midi);

  model::Step s = model::kEmptyStep;
  // Track 1: a note whose OFF on step 5 is folded into its LEN, a micro-moved note, a dim sample row, a gated note with a Chord,
  // an OFF of its own, and a note that runs on past the end.
  s.note = 61; s.instr = 49; s.vol = 100;
  (void)p.set_step(1, 0, 0, s);
  s = model::kEmptyStep; s.note = model::kNoteOff;
  (void)p.set_step(1, 0, 4, s);
  s = model::kEmptyStep; s.note = 64; s.instr = 49; s.fx[1] = {ev::kFxMicroMove, 50};
  (void)p.set_step(1, 0, 8, s);
  s = model::kEmptyStep; s.note = 67; s.instr = 1;  // the volume unset: VAL ---
  (void)p.set_step(1, 0, 12, s);
  s = model::kEmptyStep; s.note = 71; s.instr = 49; s.vol = 90; s.fx[0] = {14, 47}; s.fx[1] = {ev::kFxGate, 50};
  (void)p.set_step(1, 0, 16, s);
  s = model::kEmptyStep; s.note = model::kNoteOff;  // ends the note on 017: folded into its LEN, no row
  (void)p.set_step(1, 0, 24, s);
  s = model::kEmptyStep; s.note = model::kNoteOff;  // ends no note above it (the one on 017 ended at 025): a row of its own
  (void)p.set_step(1, 0, 25, s);
  s = model::kEmptyStep; s.note = 72; s.instr = 49; s.vol = 80;
  (void)p.set_step(1, 0, 28, s);  // nothing below: LEN >36.00
  // Track 2: a CC row before the note of its step, and a micro-moved note with its step's CC before it.
  s = model::kEmptyStep; s.note = 61; s.instr = 49; s.fx[0] = {ev::kCcFirst, 74};
  (void)p.set_step(1, 1, 0, s);
  s = model::kEmptyStep; s.note = 62; s.instr = 49; s.fx[0] = {ev::kCcFirst + 1, 12}; s.fx[1] = {ev::kFxMicroMove, 50};
  (void)p.set_step(1, 1, 8, s);
  // Track 3: what the CC rows send: the MIDI instrument's controller and PROG on channel 3, then the Config numbers on the notes
  // channel (CC C 71) and an unassigned one (--).
  s = model::kEmptyStep; s.note = 61; s.instr = 49;
  (void)p.set_step(1, 2, 0, s);
  s = model::kEmptyStep; s.fx[0] = {ev::kCcFirst, 74};
  (void)p.set_step(1, 2, 1, s);
  s = model::kEmptyStep; s.fx[0] = {ev::kCcFirst + 1, 0};
  (void)p.set_step(1, 2, 2, s);
  s = model::kEmptyStep; s.note = 61; s.instr = 1;
  (void)p.set_step(1, 2, 4, s);
  s = model::kEmptyStep; s.fx[0] = {ev::kCcFirst + 2, 100};
  (void)p.set_step(1, 2, 5, s);
  s = model::kEmptyStep; s.fx[0] = {ev::kCcFirst + 3, 100};
  (void)p.set_step(1, 2, 6, s);
  // Track 4: an inherited instrument at the top of the track (IN ??), then one of its own.
  s = model::kEmptyStep; s.note = 61; s.instr = 0;
  (void)p.set_step(1, 3, 0, s);
  s = model::kEmptyStep; s.note = 63; s.instr = 49;
  (void)p.set_step(1, 3, 4, s);
  // A pattern that exists and holds nothing, for the empty list.
  (void)p.create_pattern(2, 16);
}

void fill_demo_project_with_bindings(model::Project& p) noexcept {
  fill_demo_project(p);
  namespace ip = model::instrument_param;
  core::LearnBinding cutoff{core::param_id(core::ParamKind::Instrument, 0, ip::Cutoff), 74, 0,
                            static_cast<std::uint8_t>(core::LearnMode::Pickup), 0};
  core::LearnBinding resonance{core::param_id(core::ParamKind::Instrument, 0, ip::Resonance), 71, 1,
                               static_cast<std::uint8_t>(core::LearnMode::Jump), 0};
  (void)p.set_learn_slot(0, cutoff);
  (void)p.set_learn_slot(1, resonance);
}

}  // namespace o3os::sim
