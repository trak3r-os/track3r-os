// SPDX-License-Identifier: GPL-3.0-or-later
#include "project_fixture.h"

#include "memory_io.h"
#include "rng.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace o3os::test {

using namespace model;
using formats::native::Progress;
using formats::native::Reader;
using formats::native::Report;
using formats::native::Writer;

ModelArena& arena(int index) {
  static ModelArena arenas[3];
  return arenas[index];
}

namespace {

// A Perform bank as the builds before OS-PRFDEFAULTS made it: every row value 0 (init_perf_state gives the default values now).
void zero_perf_values(PerfState& s) {
  for (auto& row : s.value) {
    for (std::int16_t& v : row) {
      v = 0;
    }
  }
}

Step make_step(std::uint8_t note, std::uint8_t instr, std::uint8_t vol, std::uint8_t t0, std::uint8_t v0,
               std::uint8_t t1, std::uint8_t v1) {
  Step s{};
  s.note = note;
  s.instr = instr;
  s.vol = vol;
  s.fx[0] = {t0, v0};
  s.fx[1] = {t1, v1};
  return s;
}

// A fixture is built from valid values: a rejected mutation is a bug in the fixture, not a test result.
void ok(Status s) {
  if (s != Status::Ok) {
    std::fputs("project_fixture: a mutation was rejected\n", stderr);
    std::abort();
  }
}

void add_unknown(Project& p, const char (&id)[5], const std::uint8_t* data, std::uint32_t n) {
  const std::uint32_t fourcc = (static_cast<std::uint32_t>(static_cast<unsigned char>(id[0])) << 24) |
                               (static_cast<std::uint32_t>(static_cast<unsigned char>(id[1])) << 16) |
                               (static_cast<std::uint32_t>(static_cast<unsigned char>(id[2])) << 8) |
                               static_cast<std::uint32_t>(static_cast<unsigned char>(id[3]));
  ok(p.unknown_begin(fourcc, n));
  ok(p.unknown_append(data, n));
}

// A finite float in [0, 8): from an integer, no floating-point literal needed.
float some_float(Rng& r) { return static_cast<float>(r.below(8000)) / 1000.0f; }

void fill_sampler(Rng& r, SamplerParams& s) {
  s.cutoff = some_float(r);
  s.resonance = some_float(r);
  for (Envelope& e : s.envelope) {
    e.amount = some_float(r);
    e.sustain = some_float(r);
    e.delay = static_cast<std::uint16_t>(r.next());
    e.attack = static_cast<std::uint16_t>(r.next());
    e.hold = static_cast<std::uint16_t>(r.next());
    e.decay = static_cast<std::uint16_t>(r.next());
    e.release = static_cast<std::uint16_t>(r.next());
    e.loop = r.byte();
    e.enable = r.byte();
  }
  for (Lfo& l : s.lfo) {
    l.shape = static_cast<std::uint8_t>(r.byte() % 5u);  // the shapes 0..4 (a byte past the version's last shape reads as Triangle since OS-LFOSINE, which a file made of arbitrary bytes would not round trip; 5, Sine, is the `lfosine` fixture's)
    l.speed = r.byte();
    l.amount = some_float(r);
  }
  s.wavetable_window = r.next();
  s.start = static_cast<std::uint16_t>(r.next());
  s.loop_start = static_cast<std::uint16_t>(r.next());
  s.loop_end = static_cast<std::uint16_t>(r.next());
  s.end = static_cast<std::uint16_t>(r.next());
  for (std::uint16_t& v : s.slices) {
    v = static_cast<std::uint16_t>(r.next());
  }
  s.glide = static_cast<std::uint16_t>(r.next());
  s.wavetable_window_size = static_cast<std::uint16_t>(r.next());
  s.grain_length = static_cast<std::uint16_t>(r.next());
  s.grain_position = static_cast<std::uint16_t>(r.next());
  s.sample_type = static_cast<std::uint8_t>(r.below(2));
  s.play_mode = static_cast<std::uint8_t>(r.below(8));
  s.filter_type = static_cast<std::uint8_t>(r.below(3));
  s.filter_enable = r.byte();
  s.tune = static_cast<std::int8_t>(r.byte());
  s.fine_tune = static_cast<std::int8_t>(r.byte());
  s.volume = static_cast<std::uint8_t>(r.below(101));
  s.panning = static_cast<std::uint8_t>(r.below(101));
  s.delay_send = static_cast<std::uint8_t>(r.below(101));
  s.reverb_send = static_cast<std::uint8_t>(r.below(101));
  s.overdrive = static_cast<std::uint8_t>(r.below(101));
  s.bit_depth = static_cast<std::uint8_t>(4 + r.below(13));
  s.slice_count = static_cast<std::uint8_t>(r.below(49));
  s.selected_slice = static_cast<std::uint8_t>(r.below(48));
  s.grain_shape = static_cast<std::uint8_t>(r.below(3));
  s.grain_type = static_cast<std::uint8_t>(r.below(3));
}

}  // namespace

void fill_small(Project& p) {
  p.reset();
  ok(p.set_name("Small Song"));
  ok(p.set_tempo_x100(12850));
  ok(p.set_swing(54));
  ok(p.set_default_pattern_length(16));
  ok(p.set_active_tracks(12));
  ok(p.set_master(MasterSettings{90, 80, 70, 60, static_cast<std::uint8_t>(kMasterLimiter | kMasterDelayMute)}));

  ok(p.set_track_name(0, "Kick", 4));
  TrackSlot t = *p.track(1);
  t.type = TrackType::Midi;
  t.pan = 20;
  ok(p.set_track(1, t));
  t = *p.track(2);
  t.flags = kTrackMute;
  t.volume = 40;
  ok(p.set_track(2, t));
  t = *p.track(3);
  t.flags = static_cast<std::uint8_t>(kTrackSolo);
  t.bus = 2;
  ok(p.set_track(3, t));
  t = *p.track(8);
  t.type = TrackType::ExternalAudio;
  ok(p.set_track(8, t));
  ok(p.set_track_name(8, "Line in", 7));

  ok(p.add_module("tb303", 5, 1, 0));
  ok(p.add_module("fmsynth", 7, 1, 2));

  ok(p.create_pattern(1, 16));
  ok(p.set_track_length(1, 3, 12));  // tracks with their own lengths
  ok(p.set_track_length(1, 15, 128));
  ok(p.set_pattern_name(1, "Intro", 5));
  ok(p.set_step(1, 0, 0, make_step(kNoteFirst + 24, 1, 100, 18, 90, 0, 0)));
  ok(p.set_step(1, 0, 4, make_step(kNoteCut, 0, kVolumeUnset, 0, 0, 0, 0)));
  ok(p.set_step(1, 3, 15, make_step(kNoteOff, 0, kVolumeUnset, 128, 7, 200, 255)));
  ok(p.set_step(1, 15, 127, make_step(kNoteFade, 64, 0, 42, 255, 99, 1)));  // past the length: kept
  ok(p.create_pattern(2, 32));
  for (std::uint32_t s = 0; s < 32; s += 2) {
    ok(p.set_step(2, 1, s, make_step(static_cast<std::uint8_t>(kNoteFirst + 36 + s), 2, 90, 0, 0, 0, 0)));
  }
  ok(p.create_pattern(7, 3));  // odd name length and an empty pattern
  ok(p.set_track_length(7, 0, 5));
  ok(p.set_pattern_name(7, "Fill", 4));

  for (std::uint32_t id : {1u, 2u, 1u, 7u, 2u}) {
    ok(p.append_order(id));
  }

  Instrument in;
  init_instrument(in, InstrumentKind::Sample);
  std::strcpy(in.name, "Kick");
  std::strcpy(in.sample_path, "samples/kick.wav");
  in.sampler.envelope[0].amount = 1.5f;
  in.sampler.envelope[0].attack = 12;
  in.sampler.cutoff = 0.25f;
  in.sampler.slice_count = 3;
  in.sampler.slices[1] = 21845;
  in.sampler.slices[2] = 43690;
  in.sampler.tune = -3;
  in.sample_rate = 22050;  // a WAV imported at its own rate (OS-002c)
  ok(p.set_instrument(0, in));
  init_instrument(in, InstrumentKind::Midi);
  std::strcpy(in.name, "Synth A");
  in.midi_channel = 3;
  ok(p.set_instrument(48, in));
  init_instrument(in, InstrumentKind::Module);
  std::strcpy(in.name, "Acid");
  in.module_index = 0;
  in.param_len = 5;
  for (std::uint8_t i = 0; i < 5; ++i) {
    in.params[i] = static_cast<std::uint8_t>(0xA0 + i);
  }
  ok(p.set_instrument(50, in));

  const std::uint8_t odd[5] = {'h', 'e', 'l', 'l', 'o'};
  add_unknown(p, "XTRA", odd, 5);
  add_unknown(p, "EMPT", nullptr, 0);
}

void fill_midi13(Project& p) {
  fill_small(p);
  Instrument in = *p.instrument(0);
  in.engine = kInstEngineAmiga;
  in.vel_sens = 7;
  ok(p.set_instrument(0, in));
  init_instrument(in, InstrumentKind::Midi);
  in.midi_channel = 5;
  std::strcpy(in.name, "Bass synth");
  in.midi_velocity = 100;
  in.midi_program = 6;
  in.midi_out = kMidiOutBoth;
  in.midi_pb_range = 12;
  in.midi_offset = 40;
  const std::uint8_t cc[6] = {75, kMidiCcProg, kMidiCcChanAt, kMidiCcPolyAt, 1, kMidiCcNumberMax};
  for (unsigned i = 0; i < 6; ++i) {
    in.midi_cc[i] = cc[i];
  }
  in.vel_sens = 33;
  ok(p.set_instrument(20, in));
  init_instrument(in, InstrumentKind::Midi);
  in.midi_channel = 16;
  in.midi_offset = 100;
  ok(p.set_instrument(21, in));
}

void fill_state(Project& p) {
  fill_small(p);
  Instrument in = *p.instrument(0);
  in.engine = kInstEngineAmiga;  // slot 0: a sample instrument on the Amiga engine (INST 1.2)
  ok(p.set_instrument(0, in));
  init_instrument(in, InstrumentKind::Sample);
  std::strcpy(in.name, "Snare 8");
  std::strcpy(in.sample_path, "samples/snare.wav");
  in.sample_rate = 8287;
  in.engine = kInstEngineBits8;  // slot 1: 8-bit; the others of fill_small stay automatic (the 1.1 chunk)
  ok(p.set_instrument(1, in));

  MidiState m;
  init_midi_state(m);
  m.out_ports = 3;
  m.note_ports = 2;
  m.notes_channel = 9;
  m.follow = 2;
  m.cc_number[0] = 74;
  m.cc_number[1] = 71;
  m.cc_number[5] = 127;
  m.in_offset_dms[0] = 15;
  m.in_offset_dms[1] = -250;
  m.out_offset_dms[0] = -1000;
  m.out_offset_dms[1] = 1000;
  ok(p.set_midi(m));

  UsbState u;
  std::memset(&u, 0, sizeof u);
  u.mode = 1;     // AUDIO
  u.profile = 1;  // P8
  u.slots = 8;
  for (std::uint32_t i = 0; i < u.slots; ++i) {
    u.mask[i] = static_cast<std::uint16_t>(1u << (i % 12));
  }
  u.mask[6] = 0x3003;  // T1 + T2 + Master L + Master R on a slot
  u.mask[7] = 0xF000;
  ok(p.set_usb(u));

  MixState x;
  init_mix_state(x);
  x.route[0] = 1 | 2 | 4;          // MAIN + CUE + SNDA
  x.route[5] = 16 | 32;            // REC + USB, no MAIN
  x.route[16] = 1 | 2 | 8 | 16;    // the input strip: MAIN + CUE + SNDB + REC
  x.ret_route[0] = 3;
  x.ret_route[1] = 2;
  x.out_mode = 1;
  x.cue_level = 64;
  x.in_level = 75;
  ok(p.set_mix(x));

  PerfState s;
  init_perf_state(s);
  zero_perf_values(s);             // the fixture is a file of before OS-PRFDEFAULTS: the pinned sizes and CRCs say the format did not move
  s.fx[2] = 0;                     // an empty slot
  s.fx[11] = 19;
  s.row[0] = 2;
  s.row[4] = 1;
  s.value[0][0] = -100;
  s.value[0][1] = 100;
  s.value[0][2] = 1;
  s.value[4][0] = 55;
  s.value[11][2] = 8;
  ok(p.set_perf(s));

  float v[kMaxAppParams];
  for (std::uint32_t i = 0; i < kMaxAppParams; ++i) {
    v[i] = static_cast<float>(i) * 0.5f - 3.0f;
  }
  ok(p.set_app_params("pads", 4, 1, 1, v, 5));
  ok(p.set_app_params("fm-tool", 7, 1, 0, v, kMaxAppParams));  // all 32 values
}

void fill_persist2(Project& p) {
  fill_small(p);
  NoteState n;
  init_note_state(n);
  n.root = 7;    // G
  n.scale = 9;   // Blues
  n.octave = 5;
  n.lock = 1;
  n.jump = 4;
  ok(p.set_note(n));

  MuteState m;
  init_mute_state(m);
  m.mode[0] = 1;
  m.mode[3] = 2;
  m.mode[15] = 1;
  ok(p.set_mute(m));

  ScenesState s;
  init_scenes_state(s);
  s.latch = 0;
  s.scene[0].stored = 1;
  init_perf_state(s.scene[0].slots);
  zero_perf_values(s.scene[0].slots);
  s.scene[0].slots.fx[2] = 0;
  s.scene[0].slots.fx[11] = 19;
  s.scene[0].slots.row[0] = 2;
  s.scene[0].slots.row[4] = 3;
  s.scene[0].slots.value[0][0] = -100;
  s.scene[0].slots.value[0][1] = 100;
  s.scene[0].slots.value[0][2] = 1;
  s.scene[0].slots.value[4][2] = -48;
  s.scene[0].slots.value[11][1] = 8;
  s.scene[2].stored = 1;
  init_perf_state(s.scene[2].slots);
  zero_perf_values(s.scene[2].slots);
  s.scene[2].slots.row[11] = 1;
  s.scene[2].slots.value[11][0] = 15;
  ok(p.set_scenes(s));  // scene 1 is not stored

  SendFxState f;
  init_send_fx_state(f);
  const std::uint16_t values[kSendFxCount] = {1, 1, 18, 3500, 70, 0, 1, 100, 0, 77, 15, 1};
  std::memcpy(f.value, values, sizeof f.value);
  ok(p.set_send_fx(f));
}

void fill_inserts(Project& p) {
  fill_persist2(p);
  // Three inserts, one of every shape: a track's INS B with the module's whole blob and its bypass flag, a track's INS A without parameters, the master with three. The modules are the
  // two fill_small has (MODR 0 and 1): the format does not know what an effect is, only an index.
  InsertRecord r;
  std::memset(&r, 0, sizeof r);
  r.where = 3;
  r.slot = 1;
  r.module = 1;
  r.flags = kInsertFlagBypass;
  r.n = kInsertParamsMax;
  for (std::uint32_t k = 0; k < kInsertParamsMax; ++k) r.param[k] = 0.5f * static_cast<float>(k) - 3.25f;
  ok(p.set_insert(r));
  std::memset(&r, 0, sizeof r);
  r.where = 0;
  r.slot = 0;
  r.module = 0;
  ok(p.set_insert(r));
  std::memset(&r, 0, sizeof r);
  r.where = kInsertWhereMaster;
  r.module = 1;
  r.n = 3;
  r.param[0] = 1.0f;
  r.param[1] = -0.0f;
  r.param[2] = 123456.789f;
  ok(p.set_insert(r));
}

void fill_maximal(Project& p) {
  Rng r(20261011);
  p.reset();
  ok(p.set_name("0123456789abcdef0123456789ABCDEF", kProjectNameMax));
  ok(p.set_tempo_x100(kMaxTempoX100));
  ok(p.set_swing(kMaxSwing));
  ok(p.set_default_pattern_length(kMaxPatternLength));
  ok(p.set_active_tracks(kMaxTracks));
  ok(p.set_master(MasterSettings{100, 100, 100, 100, kMasterFlagsMask}));

  for (std::uint32_t i = 0; i < kMaxTracks; ++i) {
    TrackSlot t{};
    t.type = static_cast<TrackType>(i % 3);
    t.flags = static_cast<std::uint8_t>(i % 4);
    t.volume = static_cast<std::uint8_t>(100 - i);
    t.pan = static_cast<std::uint8_t>(i * 6);
    t.bus = static_cast<std::uint8_t>(i);
    std::memset(t.name, 'a' + static_cast<int>(i), kTrackNameMax);
    ok(p.set_track(i, t));
  }

  for (std::uint32_t i = 0; i < kMaxModules; ++i) {
    char name[kModuleNameMax + 1];
    std::memset(name, 'm', kModuleNameMax);
    name[0] = static_cast<char>('A' + i);
    name[kModuleNameMax] = '\0';
    ok(p.add_module(name, kModuleNameMax, i, 255 - i));
  }

  std::uint32_t ids[kMaxPatterns];
  for (std::uint32_t i = 0; i + 1 < kMaxPatterns; ++i) {
    ids[i] = i + 1;
  }
  ids[kMaxPatterns - 1] = kMaxPatternId;
  for (std::uint32_t k = 0; k < kMaxPatterns; ++k) {
    const std::uint32_t id = ids[k];
    ok(p.create_pattern(id, 1 + (k * 37) % kMaxPatternLength));
    for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
      ok(p.set_track_length(id, t, 1 + (k * 37 + t * 11) % kMaxPatternLength));
    }
    char name[kPatternNameMax + 1];
    std::memset(name, 'p', kPatternNameMax);
    ok(p.set_pattern_name(id, name, k % 2 == 0 ? kPatternNameMax : k % 31));
    for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
      Step row[kMaxPatternLength];
      for (Step& s : row) {  // never empty: a note is always set
        s = make_step(static_cast<std::uint8_t>(1 + r.below(255)), r.byte(), r.byte(), r.byte(), r.byte(), r.byte(),
                      r.byte());
      }
      ok(p.set_steps(id, t, 0, row, kMaxPatternLength));
    }
  }
  for (std::uint32_t i = 0; i < kMaxSongPositions; ++i) {
    ok(p.append_order(ids[(i * 7) % kMaxPatterns]));
  }

  for (std::uint32_t i = 0; i < kMaxInstruments; ++i) {
    Instrument in;
    const InstrumentKind kind = static_cast<InstrumentKind>(1 + i % 3);
    init_instrument(in, kind);
    fill_sampler(r, in.sampler);
    std::memset(in.name, 'i', i % 2 == 0 ? kInstrumentNameMax : 7);
    if (kind == InstrumentKind::Sample) {
      // "d/" repeated: a path of the maximal length made of safe components
      for (std::uint32_t k = 0; k < kSamplePathMax; ++k) {
        in.sample_path[k] = (k % 4 == 3) ? '/' : static_cast<char>('a' + k % 4);
      }
      in.sample_path[kSamplePathMax - 1] = 'w';
      in.sample_rate = i % 5 == 0 ? 0xFFFFFFFFu : 8000 + 1000 * i;  // 0 = not recorded; any value is carried
    } else if (kind == InstrumentKind::Midi) {
      in.midi_channel = static_cast<std::uint8_t>(1 + i % 16);
    } else {
      in.module_index = static_cast<std::uint8_t>(i % kMaxModules);
      in.param_len = static_cast<std::uint16_t>(i % 5 == 0 ? kModuleParamBytes : r.below(kModuleParamBytes + 1));
      for (std::uint32_t k = 0; k < in.param_len; ++k) {
        in.params[k] = r.byte();
      }
    }
    ok(p.set_instrument(i, in));
  }

  std::uint8_t blob[1024];
  for (std::uint32_t i = 0; i < kMaxUnknownChunks; ++i) {
    for (std::uint8_t& b : blob) {
      b = r.byte();
    }
    char id[5] = {'U', '0', static_cast<char>('0' + i / 10), static_cast<char>('0' + i % 10), '\0'};
    add_unknown(p, id, blob, 1024);
  }
  if (!p.check_invariants()) {
    std::abort();
  }
}

Progress save(const Project& p, std::vector<std::uint8_t>& out, std::size_t budget) {
  VectorSink sink;
  Writer w;
  w.begin(p, sink);
  Progress pr = Progress::Running;
  for (std::uint32_t guard = 0; pr == Progress::Running && guard < 100000000u; ++guard) {
    pr = w.step(budget);
  }
  out = std::move(sink.bytes);
  return pr;
}

Progress load(ModelArena& a, const std::vector<std::uint8_t>& bytes, Report* report, std::size_t budget) {
  MemorySource src(bytes);
  Reader rd;
  rd.begin(src, a);
  Progress pr = Progress::Running;
  for (std::uint32_t guard = 0; pr == Progress::Running && guard < 100000000u; ++guard) {
    pr = rd.step(budget);
  }
  if (report != nullptr) {
    *report = rd.report();
  }
  return pr;
}

bool is_default_empty(const Project& p) {
  return p.pattern_count() == 0 && p.instrument_count() == 0 && p.module_count() == 0 && p.song_length() == 0 &&
         p.unknown_count() == 0 && p.unknown_dropped_chunks() == 0 && p.check_invariants();
}

}  // namespace o3os::test
