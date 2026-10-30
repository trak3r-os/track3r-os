// SPDX-License-Identifier: GPL-3.0-or-later
// Command-line helper for the C++ <-> Python round trip of the native project format (ctest project_python,
// driven by tests/o3project_roundtrip.py). Not part of the OS.
//
//   o3pj_tool gen empty|small|maximal|settings|state|midi13|persist2|og|og17|law OUT   write a fixture project (empty = the default project; state = the project-level
//                                       state of OS-004d: MIDI, USBS, MIXR, PERF, APPM and instruments with a playback engine; persist2 = the session
//                                       state of OS-PERSIST2: NOTE, MUTE, SCEN, SNFX; og = what the OG
//                                       readers make of a synthetic OG project, with its OGPM and OGPN chunks; og17 = the same
//                                       from a project v17, whose OGPM chunk is 1.1: it carries the byte at 0x80C; law = the og fixture with the volume
//                                       law 1, so with the LAW chunk of OS-BUG-PASTE24)
//   o3pj_tool gen kit OUT               (OS-E2E-13) the instrument kit of the "pro session" journey: five instruments and no pattern work, and the four WAV files they name in a
//                                       `samples/` folder beside OUT (integer arithmetic only, so the same bytes on every machine). Not a round-trip fixture
//   o3pj_tool gen heavy OUT             (OS-FINALQA FQA-7) the heavy song of docs/architecture/cpu-budget.md section 4 for the unit's CPU run: 10 sustained sampler voices (6 modern, 4 Paula; looped, filtered, modulated, with sends and
//                                       drive), one TB-303 line, one FM voice (12 tracks, the product limit), the Master FX page set and the limiter on, and the WAV files it names (the kit's `sine.wav` and `saw.wav`) in a `samples/`
//                                       folder beside OUT. No modulation matrix (OS-MODMX is parked) and no looper (a loop is not file state). Not a round-trip fixture
//   o3pj_tool gen dense10 OUT           (OS-DENSE) the realistic reference of the 1.0 CPU target, the recipe and the WAVs of heavy: 6 Modern + 2 Paula sampler voices + TB-303 + FM (10 tracks)
//   o3pj_tool gen dense8 OUT            (OS-DENSE) the lighter candidate: 4 Modern + 2 Paula + TB-303 + FM (8 tracks). heavy stays the 12-track stress song
//   o3pj_tool gen moddrive1|moddrive4|moddrive8 OUT, and the same with `off` (moddrive4off)  (OS-BUG-MODDRIVE) 1, 4 or 8 TB-303 tracks that play a note on every step, with the step FX 38 (70) and 39 (8 bits)
//                                       on step 1 of each track, or without them (`off`, the control of the CPU run). No samples
//   o3pj_tool resave IN OUT            load a file and save it again (exit 1 with the error on failure)
//   o3pj_tool dump IN                   print the loaded project as text (same format as `o3project.py dump`)
//   o3pj_tool limits                    print the limits table (same format as `o3project.py limits`)
#include "../support/og_fixture.h"
#include "../support/project_fixture.h"
#include "../../formats/src/native_state.h"  // state_chunks::kAppsVersionV11 (the limits table carries it, so that the Python writer's copy is compared)

#include <o3os/formats/native.h>
#include <o3os/formats/crc32.h>
#include <o3os/formats/og.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace o3os;
using namespace o3os::model;

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    return false;
  }
  std::uint8_t buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
    out.insert(out.end(), buf, buf + n);
  }
  std::fclose(f);
  return true;
}

bool write_file(const char* path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* f = std::fopen(path, "wb");
  if (f == nullptr) {
    return false;
  }
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

void print_hex(const char* label, const char* text, std::size_t n) {
  std::printf("%s=", label);
  for (std::size_t i = 0; i < n; ++i) {
    std::printf("%02x", static_cast<unsigned char>(text[i]));
  }
}

std::uint32_t bits(float f) {
  std::uint32_t b;
  std::memcpy(&b, &f, sizeof b);
  return b;
}

std::size_t len_of(const char* field, std::size_t cap) { return field_length(field, cap); }

void dump(const Project& p) {
  print_hex("project name", p.name(), len_of(p.name(), kProjectNameMax));
  std::printf(" tempo=%u swing=%u deflen=%u active=%u master=%u,%u,%u,%u,%u\n", p.tempo_x100(), p.swing(),
              p.default_pattern_length(), p.active_tracks(), p.master().volume, p.master().dry_volume,
              p.master().reverb_volume, p.master().delay_volume, p.master().flags);
  for (std::uint32_t i = 0; i < kMaxTracks; ++i) {
    const TrackSlot& t = *p.track(i);
    std::printf("track %u type=%u flags=%u vol=%u pan=%u bus=%u ", i, static_cast<unsigned>(t.type), t.flags,
                t.volume, t.pan, t.bus);
    print_hex("name", t.name, len_of(t.name, kTrackNameMax));
    std::printf("\n");
  }
  for (std::uint32_t i = 0; i < p.module_count(); ++i) {
    const ModuleRef& m = *p.module(i);
    std::printf("module %u abi=%u.%u ", i, m.abi_major, m.abi_minor);
    print_hex("name", m.name, len_of(m.name, kModuleNameMax));
    std::printf("\n");
  }
  std::printf("song %u", p.song_length());
  for (std::uint32_t i = 0; i < p.song_length(); ++i) {
    std::printf(" %u", p.order(i));
  }
  std::printf("\n");
  for (std::uint32_t id = 1; id <= kMaxPatternId; ++id) {
    const Pattern* pat = p.pattern(id);
    if (pat == nullptr) {
      continue;
    }
    std::printf("pattern %u lens=", id);
    for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
      std::printf("%s%u", t == 0 ? "" : ",", pat->track_length[t]);
    }
    std::printf(" ");
    print_hex("name", pat->name, len_of(pat->name, kPatternNameMax));
    std::printf(" rows=");
    for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
      std::printf("%s%08x", t == 0 ? "" : ",",
                  formats::crc32(reinterpret_cast<const std::uint8_t*>(pat->steps[t]), sizeof pat->steps[t]));
    }
    std::printf("\n");
  }
  for (std::uint32_t i = 0; i < kMaxInstruments; ++i) {
    const Instrument* in = p.instrument(i);
    if (in == nullptr) {
      continue;
    }
    std::printf("instrument %u kind=%u midi=%u module=%u ", i, static_cast<unsigned>(in->kind), in->midi_channel,
                in->module_index);
    print_hex("name", in->name, len_of(in->name, kInstrumentNameMax));
    std::printf(" ");
    print_hex("path", in->sample_path, len_of(in->sample_path, kSamplePathMax));
    std::printf(" rate=%u engine=%u blob=%u/%08x m13=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", in->sample_rate, in->engine, in->param_len, formats::crc32(in->params, in->param_len), in->midi_velocity,
                in->midi_program, in->midi_out, in->midi_pb_range, in->midi_offset, in->midi_cc[0], in->midi_cc[1], in->midi_cc[2], in->midi_cc[3], in->midi_cc[4], in->midi_cc[5], in->vel_sens);
    const SamplerParams& s = in->sampler;
    std::printf("  sampler %u %u %u %u %d %d %u %u %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u | %u | %08x %08x",
                s.sample_type, s.play_mode, s.filter_type, s.filter_enable, s.tune, s.fine_tune, s.volume,
                s.panning, s.delay_send, s.reverb_send, s.overdrive, s.bit_depth, s.slice_count, s.selected_slice,
                s.grain_shape, s.grain_type, s.glide, s.wavetable_window_size, s.grain_length, s.grain_position,
                s.start, s.loop_start, s.loop_end, s.end, s.wavetable_window, bits(s.cutoff), bits(s.resonance));
    for (const Envelope& e : s.envelope) {
      std::printf(" | env %08x %u %u %u %u %08x %u %u %u", bits(e.amount), e.delay, e.attack, e.hold, e.decay,
                  bits(e.sustain), e.release, e.loop, e.enable);
    }
    for (const Lfo& l : s.lfo) {
      std::printf(" | lfo %u %u %08x", l.shape, l.speed, bits(l.amount));
    }
    std::printf(" | slices");
    for (std::uint16_t v : s.slices) {
      std::printf(" %u", v);
    }
    std::printf("\n");
  }
  // The project's settings (the ones that are not their defaults) and its Learn bindings, in slot order (OS-002d).
  for (unsigned f = 0; f < p.settings().field_count(); ++f) {
    core::ParamId id = core::param_id(core::ParamKind::Project, 0, f);
    const core::ParamDesc* d = param_desc(id);
    if (d != nullptr && p.settings().get(f) != d->def) {
      std::printf("setting %u %d\n", f, static_cast<int>(p.settings().get(f)));
    }
  }
  for (unsigned s = 0; s < core::kLearnSlots; ++s) {
    const core::LearnBinding& b = p.settings().learn(s);
    if (!core::learn_empty(b)) {
      std::printf("learn %u param=%08x cc=%u channel=%u mode=%u flags=%u\n", s, static_cast<unsigned>(b.param), b.cc, b.channel,
                  b.mode, b.flags);
    }
  }
  // The project-level state of OS-004d, only the parts the project has (the same lines as o3project.py's dump).
  if ((p.state_present() & Project::kStateMidi) != 0) {
    const MidiState& m = p.midi();
    std::printf("midi out=%u note=%u chan=%u follow=%u cc=", m.out_ports, m.note_ports, m.notes_channel, m.follow);
    for (std::uint32_t i = 0; i < kMidiCcSlots; ++i) {
      std::printf("%s%u", i == 0 ? "" : ",", m.cc_number[i]);
    }
    std::printf(" in_off=%d,%d out_off=%d,%d\n", m.in_offset_dms[0], m.in_offset_dms[1], m.out_offset_dms[0], m.out_offset_dms[1]);
  }
  if ((p.state_present() & Project::kStateUsb) != 0) {
    const UsbState& u = p.usb();
    std::printf("usb mode=%u profile=%u slots=%u masks=", u.mode, u.profile, u.slots);
    for (std::uint32_t i = 0; i < u.slots; ++i) {
      std::printf("%s%04x", i == 0 ? "" : ",", u.mask[i]);
    }
    std::printf("\n");
  }
  if ((p.state_present() & Project::kStateMix) != 0) {
    const MixState& m = p.mix();
    std::printf("mix route=");
    for (std::uint32_t i = 0; i < kMixStrips; ++i) {
      std::printf("%s%u", i == 0 ? "" : ",", m.route[i]);
    }
    std::printf(" ret=%u,%u out_mode=%u cue=%u in=%u\n", m.ret_route[0], m.ret_route[1], m.out_mode, m.cue_level, m.in_level);
  }
  if ((p.state_present() & Project::kStatePerf) != 0) {
    const PerfState& s = p.perf();
    for (std::uint32_t i = 0; i < kPerfSlots; ++i) {
      std::printf("perf %u fx=%u row=%u values=%d,%d,%d\n", i, s.fx[i], s.row[i], s.value[i][0], s.value[i][1], s.value[i][2]);
    }
  }
  for (std::uint32_t i = 0; i < p.app_params_count(); ++i) {
    const AppParams& a = *p.app_params(i);
    std::printf("app %u ", i);
    print_hex("name", a.name, len_of(a.name, kAppNameMax));
    std::printf(" abi=%u.%u values=", a.abi_major, a.abi_minor);
    for (std::uint32_t k = 0; k < a.count; ++k) {
      std::printf("%s%08x", k == 0 ? "" : ",", bits(a.value[k]));
    }
    std::printf("\n");
  }
  // The session state of OS-PERSIST2, the same lines as o3project.py's dump.
  if ((p.state_present() & Project::kStateNote) != 0) {
    const NoteState& n = p.note();
    std::printf("note root=%u scale=%u octave=%u lock=%u jump=%u\n", n.root, n.scale, n.octave, n.lock, n.jump);
  }
  if ((p.state_present() & Project::kStateMute) != 0) {
    std::printf("mute modes=");
    for (std::uint32_t i = 0; i < kMaxTracks; ++i) {
      std::printf("%s%u", i == 0 ? "" : ",", p.mute().mode[i]);
    }
    std::printf("\n");
  }
  if ((p.state_present() & Project::kStateScenes) != 0) {
    const ScenesState& s = p.scenes();
    std::printf("scenes latch=%u stored=", s.latch);
    for (std::uint32_t i = 0; i < kSceneCount; ++i) {
      std::printf("%s%u", i == 0 ? "" : ",", s.scene[i].stored);
    }
    std::printf("\n");
    for (std::uint32_t i = 0; i < kSceneCount; ++i) {
      for (std::uint32_t k = 0; k < kPerfSlots && s.scene[i].stored != 0; ++k) {
        const PerfState& q = s.scene[i].slots;
        std::printf("scene %u slot %u fx=%u row=%u values=%d,%d,%d\n", i, k, q.fx[k], q.row[k], q.value[k][0], q.value[k][1], q.value[k][2]);
      }
    }
  }
  if ((p.state_present() & Project::kStateSendFx) != 0) {
    std::printf("sendfx values=");
    for (std::uint32_t i = 0; i < kSendFxCount; ++i) {
      std::printf("%s%u", i == 0 ? "" : ",", p.send_fx().value[i]);
    }
    std::printf("\n");
  }
  if (p.volume_law() != 0u) {  // OS-BUG-PASTE24: the LAW chunk
    std::printf("law=%u\n", p.volume_law());
  }
  for (std::uint32_t i = 0; i < p.unknown_count(); ++i) {
    const UnknownChunk& u = *p.unknown(i);
    const char id[5] = {static_cast<char>(u.id >> 24), static_cast<char>(u.id >> 16), static_cast<char>(u.id >> 8),
                        static_cast<char>(u.id), '\0'};
    std::printf("unknown %u id=%s len=%u crc=%08x\n", i, id, u.length, formats::crc32(p.unknown_data(i), u.length));
    std::uint16_t groups = 0;
    std::uint32_t file_length = 0;
    if (u.id == formats::og::kChunkSettings) {
      std::uint8_t tail = 0;
      const bool valid = formats::og::inspect_settings(p.unknown_data(i), u.length, groups, &tail);
      std::printf("ogpm %u valid=%d groups=%04x tail=%u\n", i, valid ? 1 : 0, groups, static_cast<unsigned>(tail));
    } else if (u.id == formats::og::kChunkNames) {
      const bool valid = formats::og::inspect_names(p.unknown_data(i), u.length, file_length);
      std::printf("ogpn %u valid=%d length=%u\n", i, valid ? 1 : 0, file_length);
    }
  }
  std::printf("unknown_dropped=%u\n", p.unknown_dropped_chunks());
}

void limits() {
  struct Item {
    const char* name;
    std::uint32_t value;
  };
  const Item items[] = {
      {"tracks", kMaxTracks},
      {"default_active_tracks", kDefaultActiveTracks},
      {"max_output_bus", kMaxOutputBus},
      {"min_pattern_length", kMinPatternLength},
      {"max_pattern_length", kMaxPatternLength},
      {"default_pattern_length", kDefaultPatternLength},
      {"max_pattern_id", kMaxPatternId},
      {"max_patterns", kMaxPatterns},
      {"max_song_positions", kMaxSongPositions},
      {"max_instruments", kMaxInstruments},
      {"max_modules", kMaxModules},
      {"module_param_bytes", kModuleParamBytes},
      {"envelopes", kEnvelopes},
      {"lfos", kLfos},
      {"slices", kSlices},
      {"project_name_max", kProjectNameMax},
      {"pattern_name_max", kPatternNameMax},
      {"track_name_max", kTrackNameMax},
      {"instrument_name_max", kInstrumentNameMax},
      {"sample_path_max", kSamplePathMax},
      {"module_name_max", kModuleNameMax},
      {"max_unknown_chunks", kMaxUnknownChunks},
      {"unknown_pool_bytes", kUnknownPoolBytes},
      {"min_tempo_x100", kMinTempoX100},
      {"max_tempo_x100", kMaxTempoX100},
      {"default_tempo_x100", kDefaultTempoX100},
      {"min_swing", kMinSwing},
      {"max_swing", kMaxSwing},
      {"default_swing", kDefaultSwing},
      {"max_percent", kMaxPercent},
      {"max_file_bytes", formats::native::kMaxFileBytes},
      {"chunk_version", formats::native::kChunkVersion},
      {"inst_version", formats::native::kInstVersion},
      {"inst_version_engine", formats::native::kInstVersionEngine},
      {"inst_version_midi", formats::native::kInstVersionMidi},
      {"inst_version_lfo_sine", formats::native::kInstVersionLfoSine},
      {"state_version_fx", formats::native::kStateVersionFx},
      {"apps_version_v11", formats::state_chunks::kAppsVersionV11},
      {"midi_ports", kMidiPorts},
      {"midi_cc_slots", kMidiCcSlots},
      {"max_midi_offset_dms", static_cast<std::uint32_t>(kMaxMidiOffsetDms)},
      {"stem_slots", kStemSlots},
      {"mix_strips", kMixStrips},
      {"perf_slots", kPerfSlots},
      {"max_perf_fx", kPerfFxMax},
      {"max_app_param_sets", kMaxAppParamSets},
      {"app_param_sets_v10", kAppParamSetsV10},
      {"app_params_bytes_max", kAppParamsBytesMax},
      {"max_app_params", kMaxAppParams},
      {"max_note_root", kNoteRootMax},
      {"max_note_scale", kNoteScaleMax},
      {"max_note_octave", kNoteOctaveMax},
      {"max_note_jump", kNoteJumpMax},
      {"max_mute_mode", kMuteModeMax},
      {"perf_scenes", kSceneCount},
      {"send_fx_rows", kSendFxCount},
      {"send_fx_rows_max", kSendFxCountMax},
  };
  for (const Item& i : items) {
    std::printf("%s=%u\n", i.name, i.value);
  }
}

// A synthetic OG project (non-default settings in every group, 12-track and 8-track patterns, a PAMD file with
// junk in it, a sample instrument) read in as an import reads it. `version` is the project's: 16, or 17 (OS-004c).
bool gen_og(ModelArena& a, unsigned version) {
  using namespace o3os::test::og;
  formats::og::ProjectReader project;
  formats::og::PatternReader pattern;
  formats::og::InstrumentReader pti(true);
  formats::og::PamdReader pamd;
  ProjectSpec spec;
  spec.version = version;
  spec.fill = ProjectSpec::Fill::Groups;
  const PatternSpec p1 = rich_pattern(12, 5, 7);
  const PatternSpec p2 = rich_pattern(8, 4, 8);
  spec.bitmask[0] = usage_byte(p1);
  spec.bitmask[1] = usage_byte(p2);
  Bytes names = pamd_bytes({"Intro", "Verse"});
  names[6] = names[7] = 0x99;
  names[16 + 31] = 0x5A;
  auto run = [&a](formats::FormatReader& r, const Bytes& bytes, std::uint16_t index) {
    o3os::test::MemorySource src(bytes);
    formats::ReadParams params;
    params.index = index;
    r.begin(src, a, params);
    while (r.step(4096) == formats::Progress::Running) {
    }
    return r.progress() == formats::Progress::Done;
  };
  return run(project, project_bytes(spec), 0) && run(pattern, pattern_bytes(p1), 1) &&
         run(pattern, pattern_bytes(p2), 2) && run(pti, instrument_bytes({}), 0) && run(pamd, names, 0);
}

// ---- OS-E2E-13: the instrument kit of the "pro session" journey --------------------------------------------------------------------------------
// Four sample instruments and a MIDI one, made by integer arithmetic only (no libm: the same bytes on every machine), and the WAV files the first four name. The journey opens this project and
// starts a NEW one that KEEPS ITS INSTRUMENTS, as a player who keeps a kit does. The rates are chosen so that a pitch is a clean number at the root note (pad 12, note C-5): SINE is 100 frames a
// period at 44,100 Hz (441.18 Hz on the engine's 44,117.6 Hz), SAW is 32 frames a period at the Paula rate 8,287 Hz on the AMIGA engine (258.97 Hz at its period 428), PLUCK is the sine's period
// under a decay of e every 0.12 s, HAT a burst of xorshift noise that decays in 50 ms.
constexpr std::int16_t kQuarter[26] = {0,    1029, 2053,  3070,  4075,  5063,  6031,  6976,  7893,  8779,  9630,  10444, 11216,
                                       11943, 12624, 13255, 13833, 14357, 14825, 15233, 15582, 15869, 16094, 16255, 16352, 16384};

std::int32_t sine100(std::uint32_t n) {
  const std::uint32_t k = n % 100u;
  if (k <= 25u) return kQuarter[k];
  if (k <= 50u) return kQuarter[50u - k];
  if (k <= 75u) return -kQuarter[k - 50u];
  return -kQuarter[100u - k];
}

void put16(std::vector<std::uint8_t>& o, std::uint32_t v) {
  o.push_back(static_cast<std::uint8_t>(v));
  o.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
  put16(o, v & 0xFFFFu);
  put16(o, v >> 16);
}

// A 16-bit mono PCM WAV of `pcm` at `rate` Hz.
std::vector<std::uint8_t> wav_bytes(const std::vector<std::int16_t>& pcm, std::uint32_t rate) {
  std::vector<std::uint8_t> o;
  const std::uint32_t data = static_cast<std::uint32_t>(pcm.size()) * 2u;
  const char* riff = "RIFF";
  o.insert(o.end(), riff, riff + 4);
  put32(o, 36u + data);
  const char* fmt = "WAVEfmt ";
  o.insert(o.end(), fmt, fmt + 8);
  put32(o, 16u);
  put16(o, 1u);  // PCM
  put16(o, 1u);  // mono
  put32(o, rate);
  put32(o, rate * 2u);
  put16(o, 2u);
  put16(o, 16u);
  const char* dat = "data";
  o.insert(o.end(), dat, dat + 4);
  put32(o, data);
  for (const std::int16_t v : pcm) {
    put16(o, static_cast<std::uint16_t>(v));
  }
  return o;
}

constexpr std::uint32_t kKitSineRate = 44100u;
constexpr std::uint32_t kKitSawRate = 8287u;

void must(Status s) {
  if (s != Status::Ok) {
    std::fputs("o3pj_tool: a mutation of the kit was rejected\n", stderr);
    std::abort();
  }
}

// Slot 0..3: the sample instruments; slot 48 (instrument 49): the MIDI one.
void fill_kit(Project& p) {
  p.reset();
  must(p.create_pattern(1, 16));
  must(p.append_order(1));
  must(p.set_tempo_x100(12000));
  struct Sound {
    const char* name;
    const char* path;
    std::uint32_t rate;
    std::uint8_t play_mode;  // 0 one shot, 1 forward loop
    std::uint8_t engine;
  };
  const Sound sounds[4] = {{"SINE", "samples/sine.wav", kKitSineRate, 1, 0},
                           {"SAW", "samples/saw.wav", kKitSawRate, 1, kInstEngineAmiga},
                           {"PLUCK", "samples/pluck.wav", kKitSineRate, 0, 0},
                           {"HAT", "samples/hat.wav", kKitSineRate, 0, 0}};
  for (unsigned i = 0; i < 4u; ++i) {
    Instrument in;
    init_instrument(in, InstrumentKind::Sample);
    std::strcpy(in.name, sounds[i].name);
    std::strcpy(in.sample_path, sounds[i].path);
    in.sample_rate = sounds[i].rate;
    in.engine = sounds[i].engine;
    in.sampler.play_mode = sounds[i].play_mode;
    in.sampler.envelope[0].enable = 0;  // no amplitude envelope: a note is the sample, as in the audio journeys' document
    must(p.set_instrument(i, in));
  }
  Instrument midi;
  init_instrument(midi, InstrumentKind::Midi);
  std::strcpy(midi.name, "SYNTH");
  midi.midi_channel = 3;
  must(p.set_instrument(48, midi));
}

// OS-FINALQA FQA-7: the heavy song. Tracks 1..10 are the sampler voices (instruments 1..10), track 11 the TB-303 (instrument 11), track 12 the FM voice (instrument 12), one 16-step pattern that
// loops. Every voice sounds for the whole pattern: a looped sample, no cut, a new note on step 1 and 9; the 303 plays a note on every step, the FM voice one on every fourth.
// OS-DENSE: the same song with another split of the sampler voices. `Mix` is the project's name and the Modern voices (on the looped sine) and the Paula voices (on the looped saw); the 303 and the FM
// voice follow the sampler tracks, so Heavy12 (6 + 4), Dense10 (6 + 2) and Dense8 (4 + 2) are one routine. A voice keeps the parameter number i it has in Heavy12 (the Modern ones 0..5, the Paula
// ones from 8), so a Dense voice is the very voice of the stress song, only fewer of them.
struct Mix {
  const char* name;
  unsigned modern;
  unsigned amiga;
};
constexpr Mix kHeavy{"Heavy Song", 6, 4};
constexpr Mix kDense10{"Dense10 Song", 6, 2};
constexpr Mix kDense8{"Dense8 Song", 4, 2};

void fill_heavy(Project& p, const Mix& mix) {
  const unsigned voices = mix.modern + mix.amiga;  // sampler tracks 0..voices-1; the 303 is track `voices`, the FM voice the next one
  p.reset();
  must(p.set_name(mix.name, std::strlen(mix.name)));
  must(p.set_tempo_x100(12500));
  must(p.set_default_pattern_length(16));
  must(p.set_active_tracks(static_cast<std::uint8_t>(voices + 2u)));
  must(p.set_master(MasterSettings{90, 100, 60, 60, kMasterLimiter}));
  SendFxState fx;
  const std::uint16_t sends[kSendFxCount] = {1, 0, 0, 375, 55, 80, 1, 60, 50, 50, 80, 0};  // ping-pong on, 375 ms, stereo; the plate at HALF (quality 0)
  for (unsigned i = 0; i < kSendFxCount; ++i) {
    fx.value[i] = sends[i];
  }
  must(p.set_send_fx(fx));
  must(p.add_module("tb303", 5, 1, 8));
  must(p.add_module("fm", 2, 1, 8));
  must(p.create_pattern(1, 16));
  must(p.append_order(1));
  static const std::uint8_t kChord[12] = {0, 4, 7, 11, 12, 16, 19, 23, 24, 28, 31, 35};  // semitones above C-3
  for (unsigned j = 0; j < voices; ++j) {
    // The original 12-voice mix without its modern voices 7 and 8 (OS-HEAVY12): the voice parameters keep the number i they had there, so the Paula voices are unchanged.
    const unsigned i = j < mix.modern ? j : j + 8u - mix.modern;
    Instrument in;
    init_instrument(in, InstrumentKind::Sample);
    const bool saw = i >= 8u;  // the Modern voices on the sine, the Paula voices on the saw
    std::snprintf(in.name, sizeof in.name, "HV%02u", j + 1u);
    std::strcpy(in.sample_path, saw ? "samples/saw.wav" : "samples/sine.wav");
    in.sample_rate = saw ? kKitSawRate : kKitSineRate;
    in.engine = saw ? kInstEngineAmiga : 0;
    SamplerParams& s = in.sampler;
    s.play_mode = 1;  // forward loop: the voice sounds until the next note
    s.volume = 30;
    s.panning = static_cast<std::uint8_t>(10u + 7u * i);
    s.delay_send = 30;
    s.reverb_send = 30;
    s.filter_enable = 1;
    s.filter_type = static_cast<std::uint8_t>(i % 3u);
    s.cutoff = 0.6f;
    s.resonance = 0.3f;
    s.overdrive = i % 4u == 0 ? 25 : 0;
    s.bit_depth = i % 4u == 1 ? 12 : 16;
    Envelope& cut = s.envelope[2];  // the cutoff: an envelope on the even voices, an LFO on the odd ones (SOURCE: loop byte 1 + enable = LFO, loop 0 + enable = envelope)
    cut.enable = 1;
    cut.loop = i % 2u == 1 ? 1 : 0;
    cut.amount = 0.5f;
    cut.attack = 100;
    cut.decay = 400;
    cut.sustain = 0.4f;
    s.lfo[2].shape = 2;
    s.lfo[2].speed = 8;
    s.lfo[2].amount = 0.4f;
    if (i % 3u == 0) {  // the pan on an LFO, 1 voice in 3
      s.envelope[1].enable = 1;
      s.envelope[1].loop = 1;
      s.lfo[1].speed = 10;
    }
    must(p.set_instrument(j, in));
    TrackSlot t = *p.track(j);
    t.volume = 100;
    must(p.set_track(j, t));
    Step st = kEmptyStep;
    st.instr = static_cast<std::uint8_t>(j + 1u);
    st.note = static_cast<std::uint8_t>(kNoteFirst + 48u + kChord[i]);
    must(p.set_step(1, j, 0, st));
    st.note = static_cast<std::uint8_t>(kNoteFirst + 53u + kChord[i]);
    must(p.set_step(1, j, 8, st));
  }
  for (unsigned i = 0; i < 2u; ++i) {  // the two module instruments: parameters at the module's own defaults (no blob)
    Instrument in;
    init_instrument(in, InstrumentKind::Module);
    in.module_index = static_cast<std::uint8_t>(i);
    std::strcpy(in.name, i == 0 ? "ACID" : "FM KEYS");
    must(p.set_instrument(voices + i, in));
  }
  static const std::uint8_t kAcid[16] = {0, 0, 12, 0, 3, 0, 7, 10, 0, 12, 0, 3, 5, 7, 0, 15};  // semitones above C-2
  Step st = kEmptyStep;
  st.instr = static_cast<std::uint8_t>(voices + 1u);
  for (unsigned k = 0; k < 16u; ++k) {
    st.note = static_cast<std::uint8_t>(kNoteFirst + 36u + kAcid[k]);
    must(p.set_step(1, voices, k, st));
  }
  st.instr = static_cast<std::uint8_t>(voices + 2u);
  static const std::uint8_t kKeys[4] = {0, 7, 3, 10};  // semitones above C-4
  for (unsigned k = 0; k < 4u; ++k) {
    st.note = static_cast<std::uint8_t>(kNoteFirst + 60u + kKeys[k]);
    must(p.set_step(1, voices + 1u, 4u * k, st));
  }
  must(p.set_track_name(voices, "Acid", 4));
  must(p.set_track_name(voices + 1u, "FM", 2));
}

// OS-BUG-MODDRIVE: the CPU run of Overdrive and Bit depth on module instruments (the HIL scenario qa_cpu_moddrive). `n` TB-303 tracks (instruments 1..n, all the module "tb303"), a note on every step of
// one 16-step pattern that loops; with `fx` the first step of every track carries the step FX 38 (Overdrive 70) and 39 (Bit depth 8), held until the end of the song: the same song without them is the control.
void fill_moddrive(Project& p, unsigned n, bool fx) {
  char name[24];
  std::snprintf(name, sizeof name, "ModDrive%u%s", n, fx ? "" : " off");
  p.reset();
  must(p.set_name(name, std::strlen(name)));
  must(p.set_tempo_x100(12500));
  must(p.set_default_pattern_length(16));
  must(p.set_active_tracks(static_cast<std::uint8_t>(n)));
  must(p.set_master(MasterSettings{90, 100, 60, 60, kMasterLimiter}));
  must(p.add_module("tb303", 5, 1, 8));
  must(p.create_pattern(1, 16));
  must(p.append_order(1));
  static const std::uint8_t kAcid[16] = {0, 0, 12, 0, 3, 0, 7, 10, 0, 12, 0, 3, 5, 7, 0, 15};  // semitones above C-2
  for (unsigned j = 0; j < n; ++j) {
    Instrument in;
    init_instrument(in, InstrumentKind::Module);
    in.module_index = 0;
    std::snprintf(in.name, sizeof in.name, "ACID%u", j + 1u);
    must(p.set_instrument(j, in));
    for (unsigned k = 0; k < 16u; ++k) {
      Step st = kEmptyStep;
      st.instr = static_cast<std::uint8_t>(j + 1u);
      st.note = static_cast<std::uint8_t>(kNoteFirst + 36u + kAcid[(k + j) % 16u]);
      if (fx && k == 0u) {
        st.fx[0] = StepFx{38, 70};
        st.fx[1] = StepFx{39, 8};
      }
      must(p.set_step(1, j, k, st));
    }
  }
}

// The four WAVs of the kit, into `dir` ("<folder of the project>/samples").
bool write_kit_samples(const std::string& dir) {
  std::vector<std::int16_t> sine(4400);  // 44 periods
  for (std::uint32_t i = 0; i < sine.size(); ++i) {
    sine[i] = static_cast<std::int16_t>(sine100(i));
  }
  std::vector<std::int16_t> saw(32 * 8);  // 8 periods of a falling saw, 32 frames each
  for (std::uint32_t i = 0; i < saw.size(); ++i) {
    saw[i] = static_cast<std::int16_t>(12000 - static_cast<std::int32_t>(i % 32u) * 750);
  }
  std::vector<std::int16_t> pluck(11025);  // 0.25 s
  std::int64_t amp = 52429;                // 0.8 in Q16, falling by e every 0.12 s
  for (std::uint32_t i = 0; i < pluck.size(); ++i) {
    pluck[i] = static_cast<std::int16_t>((static_cast<std::int64_t>(sine100(i)) * amp) >> 16);
    amp = (amp * 65524) >> 16;
  }
  std::vector<std::int16_t> hat(2205);  // 50 ms of noise falling by e every 10 ms
  std::uint32_t s = 0x1F2E3D4Cu;
  amp = 65536;
  for (std::uint32_t i = 0; i < hat.size(); ++i) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    hat[i] = static_cast<std::int16_t>(((static_cast<std::int64_t>(static_cast<std::int32_t>(s) >> 17)) * amp) >> 16);
    amp = (amp * 65388) >> 16;  // x e^(-1/441)
  }
  return write_file((dir + "/sine.wav").c_str(), wav_bytes(sine, kKitSineRate)) && write_file((dir + "/saw.wav").c_str(), wav_bytes(saw, kKitSawRate)) &&
         write_file((dir + "/pluck.wav").c_str(), wav_bytes(pluck, kKitSineRate)) && write_file((dir + "/hat.wav").c_str(), wav_bytes(hat, kKitSineRate));
}

int fail(const char* what) {
  std::fprintf(stderr, "o3pj_tool: %s\n", what);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "limits") == 0) {
    limits();
    return 0;
  }
  if (argc == 4 && std::strcmp(argv[1], "gen") == 0) {
    ModelArena& a = test::arena(0);
    if (std::strcmp(argv[2], "empty") == 0) {
      a.project.reset();
    } else if (std::strcmp(argv[2], "small") == 0) {
      test::fill_small(a.project);
    } else if (std::strcmp(argv[2], "maximal") == 0) {
      test::fill_maximal(a.project);
    } else if (std::strcmp(argv[2], "settings") == 0) {
      // A small project with project settings that are not their defaults and three Learn bindings (OS-002d).
      test::fill_small(a.project);
      (void)a.project.set_param(core::param_id(core::ParamKind::Project, 0, project_setting::SendsMode), 1.0f);
      (void)a.project.set_param(core::param_id(core::ParamKind::Project, 0, project_setting::LimiterMode), 1.0f);
      (void)a.project.set_learn_slot(0, core::LearnBinding{core::param_id(core::ParamKind::Instrument, 0, instrument_param::Cutoff), 74, 0,
                                                              static_cast<std::uint8_t>(core::LearnMode::Pickup), 0});
      (void)a.project.set_learn_slot(1, core::LearnBinding{core::param_id(core::ParamKind::Instrument, 0, instrument_param::Resonance), 71, 1,
                                                              static_cast<std::uint8_t>(core::LearnMode::Jump), 0});
      (void)a.project.set_learn_slot(2, core::LearnBinding{core::param_id(core::ParamKind::Instrument, 0, instrument_param::Volume), 7, 0,
                                                              static_cast<std::uint8_t>(core::LearnMode::Relative), core::kLearnFollow});
    } else if (std::strcmp(argv[2], "state") == 0) {
      test::fill_state(a.project);
    } else if (std::strcmp(argv[2], "midi13") == 0) {
      test::fill_midi13(a.project);
    } else if (std::strcmp(argv[2], "lfosine") == 0) {
      // OS-LFOSINE: the small project with a Sine LFO (shape 5) on instrument 0, so INST 1.4, a Triangle (2) beside it and a Random (4): both sides read and write 1.4 alike.
      test::fill_small(a.project);
      model::Instrument in = *a.project.instrument(0);
      in.sampler.lfo[0].shape = 5;
      in.sampler.lfo[1].shape = 2;
      in.sampler.lfo[5].shape = 4;
      (void)a.project.set_instrument(0, in);
    } else if (std::strcmp(argv[2], "persist2") == 0) {
      test::fill_persist2(a.project);
    } else if (std::strcmp(argv[2], "kit") == 0) {
      fill_kit(a.project);
    } else if (std::strcmp(argv[2], "heavy") == 0) {
      fill_heavy(a.project, kHeavy);
    } else if (std::strcmp(argv[2], "dense10") == 0) {
      fill_heavy(a.project, kDense10);
    } else if (std::strcmp(argv[2], "dense8") == 0) {
      fill_heavy(a.project, kDense8);
    } else if (std::strncmp(argv[2], "moddrive", 8) == 0 && (argv[2][8] == '1' || argv[2][8] == '4' || argv[2][8] == '8') &&
               (argv[2][9] == '\0' || std::strcmp(argv[2] + 9, "off") == 0)) {
      fill_moddrive(a.project, static_cast<unsigned>(argv[2][8] - '0'), argv[2][9] == '\0');
    } else if (std::strcmp(argv[2], "law") == 0) {
      // OS-BUG-PASTE24: an OG project below version 7 saved native: its OGPM and OGPN chunks and the LAW chunk (law 1, the 1.1.14 table).
      if (!gen_og(a, 16u)) {
        return fail("gen: the synthetic OG project did not import");
      }
      a.project.set_volume_law(1);
    } else if (std::strcmp(argv[2], "og") == 0 || std::strcmp(argv[2], "og17") == 0) {
      if (!gen_og(a, std::strcmp(argv[2], "og17") == 0 ? 17u : 16u)) {
        return fail("gen: the synthetic OG project did not import");
      }
    } else {
      return fail("gen: empty, small, maximal, settings, state, midi13, persist2, lfosine, kit, heavy, dense10, dense8, og, og17 or law");
    }
    std::vector<std::uint8_t> out;
    if (test::save(a.project, out) != formats::native::Progress::Done) {
      return fail("gen: save failed");
    }
    if (!write_file(argv[3], out)) {
      return fail("gen: cannot write the output");
    }
    if (std::strcmp(argv[2], "kit") == 0 || std::strcmp(argv[2], "heavy") == 0 || std::strcmp(argv[2], "dense10") == 0 ||
        std::strcmp(argv[2], "dense8") == 0) {  // the WAVs beside it, in <folder of OUT>/samples (the build makes the folder)
      std::string dir = argv[3];
      const std::size_t slash = dir.find_last_of('/');  // none: OUT is in the working directory, not in the root
      dir = (slash == std::string::npos ? std::string(".") : dir.substr(0, slash)) + "/samples";
      if (!write_kit_samples(dir)) {
        return fail("gen kit: cannot write the samples (the folder next to OUT must exist)");
      }
    }
    return 0;
  }
  if ((argc == 4 && std::strcmp(argv[1], "resave") == 0) || (argc == 3 && std::strcmp(argv[1], "dump") == 0)) {
    std::vector<std::uint8_t> in;
    if (!read_file(argv[2], in)) {
      return fail("cannot read the input");
    }
    ModelArena& a = test::arena(0);
    formats::native::Report report;
    if (test::load(a, in, &report) != formats::native::Progress::Done) {
      std::fprintf(stderr, "o3pj_tool: load failed: %s (%s) at byte %u\n", formats::native::error_name(report.error),
                   report.detail, report.offset);
      return 1;
    }
    if (argv[1][0] == 'd') {
      dump(a.project);
      return 0;
    }
    std::vector<std::uint8_t> out;
    if (test::save(a.project, out) != formats::native::Progress::Done) {
      return fail("resave: save failed");
    }
    return write_file(argv[3], out) ? 0 : fail("resave: cannot write the output");
  }
  return fail("usage: o3pj_tool gen empty|small|maximal|settings|state|midi13|persist2|kit|heavy|dense10|dense8|og|og17|law OUT | resave IN OUT | dump IN | limits");
}
