// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/model/limits.h>

#include <cstddef>
#include <cstdint>

namespace o3os::model {

// An instrument slot (kind None = free). Sample instruments carry the OG sampler parameters, which is what
// OG import needs to be lossless (docs/formats/og.md: field names follow the OG's, positions are the OG's
// 0..65535 fractions of the sample length). MIDI instruments name a channel. Module instruments (ADR 0002)
// name an entry of the project's module table and carry a parameter blob that the core never interprets and
// keeps verbatim. Floats are always finite (validate_instrument); everything is carried as written and
// sanitised where it is used.
enum class InstrumentKind : std::uint8_t { None = 0, Sample = 1, Midi = 2, Module = 3 };

inline constexpr std::uint8_t kNoModule = 0xFF;

// The playback engine of a sample instrument (OS-004d; the engines are audio/voices.h's kEngine* and radar card `amiga-playback-mode`). The stored value is the engine plus
// one, so that 0 is "automatic", what every instrument was before the field existed: the engine follows the instrument (a sample at a Paula rate, 8,287 or 8,363 Hz, that plays as
// one shot or a forward loop is Amiga, the rest Modern; app::SampleBank::engine_for). The model does not include the audio library: a static_assert in app/sample_bank.cpp holds
// these equal to audio::kEngine* + 1. INST chunk version 1.2 (docs/formats/o3-project.md).
inline constexpr std::uint8_t kInstEngineAuto = 0;
inline constexpr std::uint8_t kInstEngineModern = 1;
inline constexpr std::uint8_t kInstEngineAmiga = 2;
inline constexpr std::uint8_t kInstEngineBits12 = 3;
inline constexpr std::uint8_t kInstEngineBits8 = 4;
inline constexpr std::uint8_t kInstEngineMax = 4;

// OS-INSTUI S8b + S9 (INST chunk version 1.3, docs/formats/o3-project.md and docs/formats/INST-1.3-diff.md): the stored fields of a MIDI instrument and the sample instrument's VEL>. All are bytes, all have the
// default in the `Instrument` that init_instrument makes, and the file carries them only when one differs from its default (so a project that never used them is the file it was). NEW COMMUNITY OS BEHAVIOUR.
//   midi_velocity  1..127, the velocity of a pad note and of a step note that names none (default 127, our choice)
//   midi_program   0 none, 1..128 = program 0..127 (sent on the instrument's channel when it changes and before the first note after a project load)
//   midi_out       where the instrument's notes and CCs go: 0 the device's MIDI OUT port (default), 1 DIN, 2 USB, 3 DIN and USB
//   midi_pb_range  0..24 semitones the pitch bend spans each way for the Micro-tune FX (default 2: a bend range of 2 semitones, the MIDI default of most receivers, no RPN is sent; 0 sends no bend)
//   midi_offset    0..100 ms the instrument's MIDI is delayed by (default 0)
//   midi_cc[6]     what CC A..F send: 0 the Config page's number for the slot (default), 1..128 the controller 0..127 (+ 1), then the options kMidiCcProg / kMidiCcChanAt / kMidiCcPolyAt
//   vel_sens       0..100 %, a SAMPLE instrument's: how much the velocity of a note from the MIDI keyboard counts (default 100 = the velocity as it is, 0 = ignored, linear between: app/vel_scale.h; a pad has no velocity: full level)
inline constexpr std::uint8_t kMidiVelocityDefault = 127;
inline constexpr std::uint8_t kMidiProgramNone = 0;
inline constexpr std::uint8_t kMidiProgramMax = 128;    // stored value: program + 1
inline constexpr std::uint8_t kMidiOutConfig = 0;
inline constexpr std::uint8_t kMidiOutDin = 1;
inline constexpr std::uint8_t kMidiOutUsb = 2;
inline constexpr std::uint8_t kMidiOutBoth = 3;
inline constexpr std::uint8_t kMidiOutMax = 3;
inline constexpr std::uint8_t kMidiPbRangeDefault = 2;
inline constexpr std::uint8_t kMidiPbRangeMax = 24;
inline constexpr std::uint8_t kMidiOffsetMaxMs = 100;
inline constexpr std::uint8_t kMidiCcConfig = 0;        // the slot's number from the Config page (the project's MIDI chunk), as before INST 1.3
inline constexpr std::uint8_t kMidiCcNumberMax = 128;   // 1..128: controller 0..127
inline constexpr std::uint8_t kMidiCcProg = 129;        // the slot's FX value is a Program Change
inline constexpr std::uint8_t kMidiCcChanAt = 130;      // channel aftertouch
inline constexpr std::uint8_t kMidiCcPolyAt = 131;      // polyphonic aftertouch on the note the track sounds
inline constexpr std::uint8_t kMidiCcMax = 131;
inline constexpr std::uint8_t kVelSensDefault = 100;
inline constexpr unsigned kMidiCcSlotsPerInstrument = 6;

struct Envelope {
  float amount;
  std::uint16_t delay, attack, hold, decay;
  float sustain;
  std::uint16_t release;
  std::uint8_t loop;
  std::uint8_t enable;
};

// OS-LFOSINE: the LFO shapes: 0 Rev Saw, 1 Saw, 2 Triangle, 3 Square, 4 Random, 5 Sine (INST 1.4). A larger value plays and reads as Triangle (audio::kLfoShapeCount, held equal by audio_model_ids_test).
inline constexpr std::uint8_t kLfoShapeTriangle = 2;
inline constexpr std::uint8_t kLfoShapeSine = 5;
inline constexpr std::uint8_t kLfoShapeCount = 6;

struct Lfo {
  std::uint8_t shape;
  std::uint8_t speed;
  float amount;
};

// Envelope order (the order of the six envelopes of an OG instrument file; the slot names are ours): amp, pan, cutoff, wavetable position, granular position, finetune.
struct SamplerParams {
  float cutoff;
  float resonance;
  Envelope envelope[kEnvelopes];
  Lfo lfo[kLfos];
  std::uint32_t wavetable_window;  // current window
  std::uint16_t start, loop_start, loop_end, end;
  std::uint16_t slices[kSlices];
  std::uint16_t glide;
  std::uint16_t wavetable_window_size;
  std::uint16_t grain_length;
  std::uint16_t grain_position;
  std::uint8_t sample_type;   // 0 wave file, 1 wavetable
  std::uint8_t play_mode;     // 0..7, 0 one-shot .. 7 granular
  std::uint8_t filter_type;   // 0 low-pass, 1 high-pass, 2 band-pass
  std::uint8_t filter_enable;
  std::int8_t tune;
  std::int8_t fine_tune;
  std::uint8_t volume;        // 0..100
  std::uint8_t panning;       // 0..100, 50 centre
  std::uint8_t delay_send;    // 0..100
  std::uint8_t reverb_send;   // 0..100
  std::uint8_t overdrive;     // 0..100
  std::uint8_t bit_depth;     // 4..16, 16 = off
  std::uint8_t slice_count;   // 0..48
  std::uint8_t selected_slice;  // 0..47
  std::uint8_t grain_shape;   // 0 square, 1 triangle, 2 gauss
  std::uint8_t grain_type;    // 0 forward, 1 backward, 2 ping-pong
};

struct Instrument {
  InstrumentKind kind;
  std::uint8_t midi_channel;  // 1..16 for Midi, 0 otherwise
  std::uint8_t module_index;  // index into the module table for Module, kNoModule otherwise
  std::uint8_t engine;        // OS-004d: kInstEngine* (0 = automatic); this was the struct's padding byte, so the layout is the one it was
  std::uint16_t param_len;    // bytes of `params` in use; 0 unless Module
  char name[kInstrumentNameMax + 1];
  char sample_path[kSamplePathMax + 1];  // relative to the project folder, "" = none (see is_safe_relative_path)
  // The rate of the sample's PCM in Hz (OS-002c: a WAV import keeps it, nothing is resampled; the engine plays at the native
  // rate). 0 = not recorded: 44,100 Hz (the rate of the PCM in an OG .pti file). Any other value is carried as it is and sanitised where it is used. Native
  // INST chunk version 1.1 (docs/formats/o3-project.md).
  std::uint32_t sample_rate;
  SamplerParams sampler;
  std::uint8_t params[kModuleParamBytes];
  // OS-INSTUI S8b + S9, INST 1.3 (see kMidiVelocityDefault): twelve bytes after the module blob, so every older field keeps its offset.
  std::uint8_t midi_velocity;
  std::uint8_t midi_program;
  std::uint8_t midi_out;
  std::uint8_t midi_pb_range;
  std::uint8_t midi_offset;
  std::uint8_t midi_cc[kMidiCcSlotsPerInstrument];
  std::uint8_t vel_sens;
  // OS-STEREO step 2 (INST 1.5, NEW COMMUNITY OS BEHAVIOUR): 1 = the instrument is a stereo sample (kInstStereoMax); a Sample instrument's only, 0 for every other kind. It is the instrument's wish: the sample
  // plays stereo only while the bank also holds its side signal (decision S4 of docs/specs/OS-STEREO.md), which nothing provides before slice (b). One byte after VEL>: the struct grows to 720 B.
  std::uint8_t stereo;
  // The three bytes that would be tail padding, made explicit so a whole-instrument memcmp (Editor::set_instrument, PASTE, the instrument-file load) compares defined bytes. Always 0: init_instrument zeroes them
  // and Project::set_instrument stores them zero (the canonical form); the file has no field for them.
  std::uint8_t reserved[3];
};

// True when every INST 1.3 field has the value init_instrument gives it (the writer then writes the chunk as the older minor).
bool inst_v13_default(const Instrument& in) noexcept;

// OS-STEREO step 2: the largest value of Instrument::stereo (0 mono, 1 stereo); a file's byte above it reads 0 (as an engine byte above kInstEngineMax reads automatic).
inline constexpr std::uint8_t kInstStereoMax = 1;

// OS-LFOSINE: true when one of the six LFOs is the Sine shape (the writer then writes INST 1.4, which only says that a shape of 5 is Sine).
bool inst_uses_sine(const Instrument& in) noexcept;

// The layout is declared, not left to the compiler (ADR 0011): 4-byte alignment because of the floats, and
// the same size on the 64-bit host and on the MK66, which the arena size relies on.
static_assert(sizeof(Envelope) == 20 && alignof(Envelope) == 4, "Envelope layout");
static_assert(sizeof(Lfo) == 8 && alignof(Lfo) == 4, "Lfo layout");
static_assert(sizeof(SamplerParams) == 308 && alignof(SamplerParams) == 4, "SamplerParams layout");
static_assert(sizeof(Instrument) == 720 && alignof(Instrument) == 4, "Instrument layout");

// Sets `out` to a fresh instrument of `kind`: all zero except bit_depth 16, volume 100, panning 50 and the
// sample points spanning the whole sample (loop_end = end = 65535), sample_rate 0 (not recorded), engine 0 (automatic) and the INST 1.3 fields at their defaults (velocity 127, no program, the device's port, a bend
// range of 2, no offset, the Config page's CC numbers, VEL> 100).
void init_instrument(Instrument& out, InstrumentKind kind) noexcept;

// nullptr when `in` is a valid, canonical instrument of a project with `module_count` module entries,
// otherwise a static string saying what is wrong. Kind None is not valid here (free the slot instead).
const char* validate_instrument(const Instrument& in, std::uint32_t module_count) noexcept;

// A sample path stays inside the project folder: "" or components separated by single '/', none empty, ".",
// ".." or containing a backslash, a colon or a control character; no leading or trailing '/'.
bool is_safe_relative_path(const char* text, std::size_t len) noexcept;
// A module name is a file stem in /modules: 1..31 characters, no '/', '\\', ':' or control character, and
// not "." or "..".
bool is_safe_module_name(const char* text, std::size_t len) noexcept;

// Length of the text in a field of `cap` + 1 bytes: n <= cap when the NUL is at index n, cap + 1 when the
// field is not terminated (invalid).
inline std::size_t field_length(const char* field, std::size_t cap) noexcept {
  std::size_t n = 0;
  while (n <= cap && field[n] != '\0') {
    ++n;
  }
  return n;
}

// True when the float is neither NaN nor infinite (checked on the bits: no <cmath>).
bool is_finite_float(float value) noexcept;

}  // namespace o3os::model
