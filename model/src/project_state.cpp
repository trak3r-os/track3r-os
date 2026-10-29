// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/instrument.h>
#include <o3os/model/project_state.h>

namespace o3os::model {

void init_midi_state(MidiState& out) noexcept {
  std::memset(&out, 0, sizeof out);
  out.note_ports = 1;  // DIN, as midi::MidiConfig starts
  out.follow = 1;      // NORMAL
  std::memset(out.cc_number, kMidiCcUnassigned, sizeof out.cc_number);
}

void init_usb_state(UsbState& out) noexcept {
  std::memset(&out, 0, sizeof out);
  out.profile = 2;  // P10, the default profile
  out.slots = kUsbProfileChannels[2];
  for (std::uint32_t i = 0; i < 8; ++i) {
    out.mask[i] = static_cast<std::uint16_t>(1u << i);  // S1..S8 = T1..T8
  }
  out.mask[8] = 1u << 12;  // Master L
  out.mask[9] = 1u << 13;  // Master R (audio/stem_map.h: the factory slots of P10)
}

void init_mix_state(MixState& out) noexcept {
  for (std::uint32_t i = 0; i < kMixStrips - 1u; ++i) {
    out.route[i] = kMixDefaultTrackRoute;
  }
  out.route[kMixStrips - 1u] = kMixDefaultInputRoute;
  out.ret_route[0] = out.ret_route[1] = 1;  // MAIN
  out.out_mode = 0;
  out.cue_level = kMixDefaultCue;
  out.in_level = 0;
}

void init_perf_state(PerfState& out) noexcept {
  std::memset(&out, 0, sizeof out);
  for (std::uint32_t s = 0; s < kPerfSlots; ++s) {
    out.fx[s] = static_cast<std::uint8_t>(s + 1u);  // our default: the first twelve FX, in order
    for (std::uint32_t r = 0; r < kPerfStoredRows; ++r) {
      out.value[s][r] = kPerfDefaultValues[out.fx[s]][r];  // OS-PRFDEFAULTS
    }
  }
}

void init_note_state(NoteState& out) noexcept {
  out.root = 0;
  out.scale = 0;
  out.octave = kNoteDefaultOctave;
  out.lock = 0;
  out.jump = kNoteDefaultJump;
}

void init_mute_state(MuteState& out) noexcept { std::memset(&out, 0, sizeof out); }  // every track Immediate

void init_scenes_state(ScenesState& out) noexcept {
  std::memset(&out, 0, sizeof out);
  out.latch = 1;  // our default: a slot's row stays selected after the pad is let go
}

void init_send_fx_state(SendFxState& out) noexcept {
  for (std::uint32_t i = 0; i < kSendFxCount; ++i) {
    out.value[i] = kSendFxDefault[i];
  }
}

const char* validate_midi_state(const MidiState& s) noexcept {
  if ((s.out_ports & ~kMidiPortMask) != 0 || (s.note_ports & ~kMidiPortMask) != 0) return "MIDI: a port outside DIN and USB";
  if (s.notes_channel > 15) return "MIDI: channel above 16";
  if (s.follow > kMidiFollowMax) return "MIDI: bad follow tightness";
  for (std::uint8_t n : s.cc_number) {
    if (n > 127 && n != kMidiCcUnassigned) return "MIDI: bad CC number";
  }
  for (std::uint32_t p = 0; p < kMidiPorts; ++p) {
    if (s.in_offset_dms[p] < -kMaxMidiOffsetDms || s.in_offset_dms[p] > kMaxMidiOffsetDms || s.out_offset_dms[p] < -kMaxMidiOffsetDms ||
        s.out_offset_dms[p] > kMaxMidiOffsetDms) {
      return "MIDI: offset out of range";
    }
  }
  return nullptr;
}

const char* validate_usb_state(const UsbState& s) noexcept {
  if (s.mode > kUsbModeMax) return "USB: bad mode";
  if (s.profile > kUsbProfileMax || s.profile == kUsbProfileP4S) return "USB: bad profile";
  if (s.slots != kUsbProfileChannels[s.profile] || s.reserved != 0) return "USB: slot count does not fit the profile";
  for (std::uint32_t i = s.slots; i < kStemSlots; ++i) {
    if (s.mask[i] != 0) return "USB: a stem mask past the profile's slots";
  }
  return nullptr;
}

const char* validate_mix_state(const MixState& s) noexcept {
  for (std::uint32_t i = 0; i < kMixStrips; ++i) {
    if ((s.route[i] & ~kMixRouteMask) != 0) return "MIXR: bad route bits";
  }
  if ((s.route[kMixStrips - 1u] & kMixRouteUsb) != 0) return "MIXR: the input strip has no USB column";
  if (s.ret_route[0] > 3 || s.ret_route[1] > 3) return "MIXR: bad return route";
  if (s.out_mode > 1) return "MIXR: bad out mode";
  if (s.cue_level > kMaxPercent || s.in_level > kMaxPercent) return "MIXR: level above 100";
  return nullptr;
}

const char* validate_perf_state(const PerfState& s) noexcept {
  for (std::uint32_t i = 0; i < kPerfSlots; ++i) {
    if (s.fx[i] > kPerfFxMax) return "PERF: unknown FX";
    if (s.row[i] > kPerfRowMax) return "PERF: bad row";
    for (std::int16_t v : s.value[i]) {
      if (v < -kPerfValueMax || v > kPerfValueMax) return "PERF: value out of range";
    }
  }
  return nullptr;
}

const char* validate_app_params(const AppParams& s) noexcept {
  const std::size_t n = field_length(s.name, kAppNameMax);
  if (n > kAppNameMax || !is_safe_module_name(s.name, n)) return "APPM: bad module name";
  for (std::size_t i = n; i <= kAppNameMax; ++i) {
    if (s.name[i] != '\0') return "APPM: name not NUL padded";
  }
  if (s.count < 1 || s.count > kMaxAppParams || s.reserved != 0) return "APPM: bad parameter count";
  for (std::uint32_t i = 0; i < kMaxAppParams; ++i) {
    if (i >= s.count) {
      std::uint32_t bits;
      std::memcpy(&bits, &s.value[i], sizeof bits);
      if (bits != 0) return "APPM: a value past the count";
    } else if (!is_finite_float(s.value[i])) {
      return "APPM: non-finite value";
    }
  }
  return nullptr;
}

const char* validate_note_state(const NoteState& s) noexcept {
  if (s.root > kNoteRootMax || s.scale > kNoteScaleMax || s.octave > kNoteOctaveMax || s.lock > 1 || s.jump > kNoteJumpMax) return "NOTE: a value out of range";
  return nullptr;
}

const char* validate_mute_state(const MuteState& s) noexcept {
  for (std::uint8_t m : s.mode) {
    if (m > kMuteModeMax) return "MUTE: bad mode";
  }
  return nullptr;
}

namespace {

bool all_zero(const void* p, std::size_t n) noexcept {
  const std::uint8_t* b = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    if (b[i] != 0) return false;
  }
  return true;
}

}  // namespace

const char* validate_scenes_state(const ScenesState& s) noexcept {
  if (s.latch > 1 || s.reserved != 0) return "SCEN: bad latch";
  for (const SceneState& sc : s.scene) {
    if (sc.reserved != 0 || sc.stored > 1) return "SCEN: bad scene";
    if (sc.stored == 0) {
      if (!all_zero(&sc.slots, sizeof sc.slots)) return "SCEN: a scene that is not stored holds values";
    } else if (const char* why = validate_perf_state(sc.slots)) {
      return why;
    }
  }
  return nullptr;
}

const char* validate_send_fx_state(const SendFxState& s) noexcept {
  for (std::uint32_t i = 0; i < kSendFxCount; ++i) {
    if (s.value[i] < kSendFxMin[i] || s.value[i] > kSendFxMax[i]) return "SNFX: a value out of its row's range";
  }
  return nullptr;
}

unsigned sanitise_note_state(NoteState& s) noexcept {
  unsigned reset = 0;
  const auto fix = [&reset](std::uint8_t& v, std::uint8_t max, std::uint8_t def) {
    if (v > max) {
      v = def;
      ++reset;
    }
  };
  fix(s.root, kNoteRootMax, 0);
  fix(s.scale, kNoteScaleMax, 0);
  fix(s.octave, kNoteOctaveMax, kNoteDefaultOctave);
  fix(s.lock, 1, 0);
  fix(s.jump, kNoteJumpMax, kNoteDefaultJump);
  return reset;
}

unsigned sanitise_mute_state(MuteState& s) noexcept {
  unsigned reset = 0;
  for (std::uint8_t& m : s.mode) {
    if (m > kMuteModeMax) {
      m = 0;
      ++reset;
    }
  }
  return reset;
}

unsigned sanitise_scenes_state(ScenesState& s) noexcept {
  unsigned reset = 0;
  s.reserved = 0;
  if (s.latch > 1) {
    s.latch = 1;
    ++reset;
  }
  for (SceneState& sc : s.scene) {
    sc.reserved = 0;
    if (sc.stored > 1) {
      ++reset;
      sc.stored = 0;  // not a scene this version knows: not stored
    }
    if (sc.stored == 0) {
      std::memset(&sc.slots, 0, sizeof sc.slots);  // the bytes of a scene that is not stored mean nothing
      continue;
    }
    for (std::uint32_t i = 0; i < kPerfSlots; ++i) {
      if (sc.slots.fx[i] > kPerfFxMax) {  // an FX a later version added: this one has none (the row and values meant something else)
        sc.slots.fx[i] = 0;
        sc.slots.row[i] = 0;
        std::memset(sc.slots.value[i], 0, sizeof sc.slots.value[i]);
        ++reset;
        continue;
      }
      if (sc.slots.row[i] > kPerfRowMax) {
        sc.slots.row[i] = 0;
        ++reset;
      }
      for (std::int16_t& v : sc.slots.value[i]) {
        if (v < -kPerfValueMax || v > kPerfValueMax) {
          v = 0;
          ++reset;
        }
      }
    }
  }
  return reset;
}

unsigned sanitise_send_fx_state(SendFxState& s) noexcept {
  unsigned reset = 0;
  for (std::uint32_t i = 0; i < kSendFxCount; ++i) {
    if (s.value[i] < kSendFxMin[i] || s.value[i] > kSendFxMax[i]) {
      s.value[i] = kSendFxDefault[i];
      ++reset;
    }
  }
  return reset;
}

}  // namespace o3os::model
