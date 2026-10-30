// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/instrument.h>

#include <cstring>

namespace o3os::model {

namespace {

bool safe_char(char c) noexcept {
  const unsigned char u = static_cast<unsigned char>(c);
  return u >= 0x20 && u != 0x7F && c != '\\' && c != ':';
}

const char* validate_sampler(const SamplerParams& s) noexcept {
  if (!is_finite_float(s.cutoff) || !is_finite_float(s.resonance)) {
    return "sampler: non-finite filter value";
  }
  for (const Envelope& e : s.envelope) {
    if (!is_finite_float(e.amount) || !is_finite_float(e.sustain)) {
      return "sampler: non-finite envelope value";
    }
  }
  for (const Lfo& l : s.lfo) {
    if (!is_finite_float(l.amount)) {
      return "sampler: non-finite LFO value";
    }
  }
  if (s.sample_type > 1) return "sampler: bad sample type";
  if (s.play_mode > 7) return "sampler: bad play mode";
  if (s.filter_type > 2) return "sampler: bad filter type";
  if (s.volume > kMaxPercent || s.panning > kMaxPercent || s.delay_send > kMaxPercent ||
      s.reverb_send > kMaxPercent || s.overdrive > kMaxPercent) {
    return "sampler: level above 100";
  }
  if (s.bit_depth < 4 || s.bit_depth > 16) return "sampler: bad bit depth";
  if (s.slice_count > kSlices || s.selected_slice >= kSlices) return "sampler: bad slice count";
  if (s.grain_shape > 2 || s.grain_type > 2) return "sampler: bad granular mode";
  return nullptr;
}

}  // namespace

bool is_finite_float(float value) noexcept {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  return (bits & 0x7F800000u) != 0x7F800000u;
}

bool is_safe_relative_path(const char* text, std::size_t len) noexcept {
  if (len == 0) {
    return true;
  }
  std::size_t start = 0;
  for (std::size_t i = 0; i <= len; ++i) {
    const bool end = (i == len);
    if (!end && !safe_char(text[i])) {
      return false;
    }
    if (end || text[i] == '/') {  // the end closes the last component like a '/'
      const std::size_t n = i - start;
      if (n == 0) return false;  // leading '/', empty component or trailing '/'
      if (n == 1 && text[start] == '.') return false;
      if (n == 2 && text[start] == '.' && text[start + 1] == '.') return false;
      start = i + 1;
    }
  }
  return true;
}

bool is_safe_module_name(const char* text, std::size_t len) noexcept {
  if (len == 0 || len > kModuleNameMax) {
    return false;
  }
  for (std::size_t i = 0; i < len; ++i) {
    if (!safe_char(text[i]) || text[i] == '/') {
      return false;
    }
  }
  return !(len == 1 && text[0] == '.') && !(len == 2 && text[0] == '.' && text[1] == '.');
}

void init_instrument(Instrument& out, InstrumentKind kind) noexcept {
  std::memset(&out, 0, sizeof out);
  out.kind = kind;
  out.module_index = kNoModule;
  out.midi_velocity = kMidiVelocityDefault;  // INST 1.3 (instrument.h): the rest of the new fields are 0 = their defaults
  out.midi_pb_range = kMidiPbRangeDefault;
  out.vel_sens = kVelSensDefault;
  // This OS's default instrument parameters; the parameter tables (params.cpp) hold the same values.
  out.sampler.volume = 50;
  out.sampler.panning = 50;
  out.sampler.cutoff = 1.0f;  // the filter is off, and a cutoff of 0 would silence it the moment it was switched on
  out.sampler.bit_depth = 16;
  out.sampler.loop_end = 65535;
  out.sampler.end = 65535;
  for (Envelope& e : out.sampler.envelope) {
    e.amount = 1.0f;
    e.attack = 3000;
    e.sustain = 1.0f;
    e.release = 1000;
  }
  out.sampler.envelope[0].attack = 10;  // the volume envelope: on, 10 ms
  out.sampler.envelope[0].enable = 1;
  for (Lfo& l : out.sampler.lfo) {  // the default LFO: {triangle, 0, 0.5}; every envelope's SOURCE stays Off (the volume's Envelope)
    l.shape = 2;
    l.amount = 0.5f;
  }
}

bool inst_v13_default(const Instrument& in) noexcept {
  for (const std::uint8_t cc : in.midi_cc) {
    if (cc != kMidiCcConfig) {
      return false;
    }
  }
  return in.midi_velocity == kMidiVelocityDefault && in.midi_program == kMidiProgramNone && in.midi_out == kMidiOutConfig && in.midi_pb_range == kMidiPbRangeDefault &&
         in.midi_offset == 0u && in.vel_sens == kVelSensDefault;
}

bool inst_uses_sine(const Instrument& in) noexcept {
  for (const Lfo& l : in.sampler.lfo) {
    if (l.shape == kLfoShapeSine) {
      return true;
    }
  }
  return false;
}

const char* validate_instrument(const Instrument& in, std::uint32_t module_count) noexcept {
  if (in.kind != InstrumentKind::Sample && in.kind != InstrumentKind::Midi && in.kind != InstrumentKind::Module) {
    return "bad instrument kind";
  }
  if (field_length(in.name, kInstrumentNameMax) > kInstrumentNameMax) return "instrument name not terminated";
  if (in.engine > kInstEngineMax) return "bad playback engine";
  if (in.stereo > kInstStereoMax) return "bad stereo flag";
  if (in.stereo != 0 && in.kind != InstrumentKind::Sample) return "stereo flag on a non-sample instrument";
  if (in.midi_velocity < 1 || in.midi_velocity > 127 || in.midi_program > kMidiProgramMax || in.midi_out > kMidiOutMax || in.midi_pb_range > kMidiPbRangeMax || in.midi_offset > kMidiOffsetMaxMs ||
      in.vel_sens > kMaxPercent) {
    return "bad MIDI field";
  }
  for (const std::uint8_t cc : in.midi_cc) {
    if (cc > kMidiCcMax) return "bad MIDI CC slot";
  }
  const std::size_t path_len = field_length(in.sample_path, kSamplePathMax);
  if (path_len > kSamplePathMax) return "sample path not terminated";
  if (!is_safe_relative_path(in.sample_path, path_len)) return "unsafe sample path";

  if (in.kind == InstrumentKind::Midi) {
    if (in.midi_channel < 1 || in.midi_channel > 16) return "bad MIDI channel";
  } else if (in.midi_channel != 0) {
    return "MIDI channel on a non-MIDI instrument";
  }
  if (in.kind == InstrumentKind::Module) {
    if (in.module_index >= module_count) return "module index outside the module table";
    if (in.param_len > kModuleParamBytes) return "module parameter blob too long";
  } else if (in.module_index != kNoModule || in.param_len != 0) {
    return "module data on a non-module instrument";
  }
  return validate_sampler(in.sampler);
}

}  // namespace o3os::model
