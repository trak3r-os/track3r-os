// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_state.h"

#include <cstring>

namespace o3os::formats::state_chunks {

using native::codec::Cursor;
using native::codec::Enc;

namespace {

constexpr std::uint8_t kProfileP8 = 1;  // the chunk's profile byte of P8 (usb::Profile::P8)

// OS-USBOUTS: a project with 4 or 6 outputs (usb::Profile P4 = 5, P6 = 6) is written as USBS 1.1.
bool usb_counted(const model::UsbState& u) noexcept { return u.profile == 5 || u.profile == 6; }

// The factory mask of slot i of P8 (T1..T6, Master L, Master R: audio/stem_map.h; the bits are stem_src).
std::uint16_t p8_factory_mask(std::uint32_t i) noexcept { return static_cast<std::uint16_t>(i < 6 ? 1u << i : 1u << (i == 6 ? 12 : 13)); }

}  // namespace

bool present(const model::Project& p, Part part) noexcept {
  switch (part) {
    case Part::Midi: return (p.state_present() & model::Project::kStateMidi) != 0;
    case Part::Usb: return (p.state_present() & model::Project::kStateUsb) != 0;
    case Part::Mix: return (p.state_present() & model::Project::kStateMix) != 0;
    case Part::Perf: return (p.state_present() & model::Project::kStatePerf) != 0;
    case Part::Apps: return p.app_params_count() != 0;
    case Part::Note: return (p.state_present() & model::Project::kStateNote) != 0;
    case Part::Mute: return (p.state_present() & model::Project::kStateMute) != 0;
    case Part::Scene: return (p.state_present() & model::Project::kStateScenes) != 0;
    case Part::SendFx: return (p.state_present() & model::Project::kStateSendFx) != 0;
    case Part::Law: return p.volume_law() != 0u;
  }
  return false;
}

std::size_t payload_size(const model::Project& p, Part part) noexcept {
  if (!present(p, part)) {
    return 0;
  }
  switch (part) {
    case Part::Midi: return kMidiBytes;
    case Part::Usb: return usb_counted(p.usb()) ? kUsbCountP8Bytes : kUsbFixed + 2u * p.usb().slots;
    case Part::Mix: return kMixBytes;
    case Part::Perf: return kPerfBytes;
    case Part::Apps: {
      std::size_t n = kAppsFixed;
      for (std::uint32_t i = 0; i < p.app_params_count(); ++i) {
        const model::AppParams& a = *p.app_params(i);
        n += kAppEntryFixed + native::codec::text_len(a.name, model::kAppNameMax) + 4u * a.count;
      }
      return n;
    }
    case Part::Note: return kNoteBytes;
    case Part::Mute: return kMuteMax;  // written with all 16 tracks
    case Part::Scene: return kSceneBytes;
    case Part::SendFx: return kSendFxFixed + 2u * model::kSendFxCount;
    case Part::Law: return kLawBytes;
  }
  return 0;
}

namespace {

bool uses_fx_after_v10(const model::PerfState& s) noexcept {
  for (std::uint32_t i = 0; i < model::kPerfSlots; ++i) {
    if (s.fx[i] > kPerfFxMaxV10) return true;
  }
  return false;
}

// Takes the FX the chunk's version does not have out of the slots (none, with the row and values reset: they meant something else); how many.
unsigned drop_fx_after_v10(model::PerfState& s) noexcept {
  unsigned dropped = 0;
  for (std::uint32_t i = 0; i < model::kPerfSlots; ++i) {
    if (s.fx[i] > kPerfFxMaxV10) {
      s.fx[i] = 0;
      s.row[i] = 0;
      for (std::int16_t& v : s.value[i]) v = 0;
      ++dropped;
    }
  }
  return dropped;
}

}  // namespace

std::uint16_t version_of(const model::Project& p, Part part) noexcept {
  // APPM is written as 1.0 while it holds no more than the 6 apps 1.0 could (byte for byte what it always was: an older OS opens it), and as 1.1 beyond (OS-APPUX S3).
  if (part == Part::Apps && p.app_params_count() > model::kAppParamSetsV10) {
    return state_chunks::kAppsVersionV11;
  }
  if (part == Part::Usb && usb_counted(p.usb())) {
    return state_chunks::kUsbVersionCount;
  }
  if (part == Part::Perf && uses_fx_after_v10(p.perf())) {
    return native::kStateVersionFx;
  }
  if (part == Part::Scene) {
    for (const model::SceneState& sc : p.scenes().scene) {
      if (sc.stored != 0 && uses_fx_after_v10(sc.slots)) return native::kStateVersionFx;
    }
  }
  return kVersion;
}

void encode(Enc& e, const model::Project& p, Part part) noexcept {
  e.u16(version_of(p, part));
  switch (part) {
    case Part::Midi: {
      const model::MidiState& m = p.midi();
      e.u8(m.out_ports);
      e.u8(m.note_ports);
      e.u8(m.notes_channel);
      e.u8(m.follow);
      e.bytes(m.cc_number, sizeof m.cc_number);
      for (std::int16_t v : m.in_offset_dms) e.u16(static_cast<std::uint16_t>(v));
      for (std::int16_t v : m.out_offset_dms) e.u16(static_cast<std::uint16_t>(v));
      return;
    }
    case Part::Usb: {
      const model::UsbState& u = p.usb();
      e.u8(u.mode);
      if (usb_counted(u)) {  // 1.1: P8 with 8 slots (the real ones, then P8's factory masks), then the real count
        e.u8(kProfileP8);
        e.u8(8);
        for (std::uint32_t i = 0; i < 8; ++i) e.u16(i < u.slots ? u.mask[i] : p8_factory_mask(i));
        e.u8(u.slots);
        return;
      }
      e.u8(u.profile);
      e.u8(u.slots);
      for (std::uint32_t i = 0; i < u.slots; ++i) e.u16(u.mask[i]);
      return;
    }
    case Part::Mix: {
      const model::MixState& m = p.mix();
      e.u8(static_cast<std::uint8_t>(model::kMixStrips));
      e.bytes(m.route, sizeof m.route);
      e.bytes(m.ret_route, sizeof m.ret_route);
      e.u8(m.out_mode);
      e.u8(m.cue_level);
      e.u8(m.in_level);
      return;
    }
    case Part::Perf: {
      const model::PerfState& s = p.perf();
      e.u8(static_cast<std::uint8_t>(model::kPerfSlots));
      for (std::uint32_t i = 0; i < model::kPerfSlots; ++i) {
        e.u8(s.fx[i]);
        e.u8(s.row[i]);
        for (std::int16_t v : s.value[i]) e.u16(static_cast<std::uint16_t>(v));
      }
      return;
    }
    case Part::Apps: {
      e.u8(static_cast<std::uint8_t>(p.app_params_count()));
      for (std::uint32_t i = 0; i < p.app_params_count(); ++i) {
        const model::AppParams& a = *p.app_params(i);
        e.text(a.name, native::codec::text_len(a.name, model::kAppNameMax));
        e.u8(a.abi_major);
        e.u8(a.abi_minor);
        e.u8(a.count);
        for (std::uint32_t k = 0; k < a.count; ++k) e.f32(a.value[k]);
      }
      return;
    }
    case Part::Note: {
      const model::NoteState& n = p.note();
      e.u8(n.root);
      e.u8(n.scale);
      e.u8(n.octave);
      e.u8(n.lock);
      e.u8(n.jump);
      return;
    }
    case Part::Mute: {
      e.u8(static_cast<std::uint8_t>(model::kMaxTracks));
      e.bytes(p.mute().mode, sizeof p.mute().mode);
      return;
    }
    case Part::Scene: {
      const model::ScenesState& s = p.scenes();
      e.u8(s.latch);
      e.u8(static_cast<std::uint8_t>(model::kSceneCount));
      for (const model::SceneState& sc : s.scene) {
        e.u8(sc.stored);
        for (std::uint32_t i = 0; i < model::kPerfSlots; ++i) {
          e.u8(sc.slots.fx[i]);
          e.u8(sc.slots.row[i]);
          for (std::int16_t v : sc.slots.value[i]) e.u16(static_cast<std::uint16_t>(v));
        }
      }
      return;
    }
    case Part::SendFx: {
      e.u8(static_cast<std::uint8_t>(model::kSendFxCount));
      for (std::uint16_t v : p.send_fx().value) e.u16(v);
      return;
    }
    case Part::Law:
      e.u8(1);  // written only for the 1.1.14 table
      return;
  }
}

namespace {

constexpr const char* kShort = "chunk fields do not fit (short chunk, text too long or containing NUL)";

std::int16_t s16(Cursor& c) noexcept { return static_cast<std::int16_t>(c.u16()); }

// Short chunk, or unused bytes the version does not explain.
bool shape_ok(const Cursor& c, bool newer_minor) noexcept { return !c.bad() && (c.left() == 0 || newer_minor); }

}  // namespace

const char* decode(Cursor& c, bool newer_minor, model::Project& p, Part part, std::uint32_t& reset) noexcept {
  const unsigned chunk_minor = c.u16() & 0xFFu;  // the version was checked when the chunk header was read; PERF and SCEN need its minor (1.1: the FX 20 and 21), APPM too (1.1: up to 12 apps)
  switch (part) {
    case Part::Midi: {
      model::MidiState m;
      m.out_ports = c.u8();
      m.note_ports = c.u8();
      m.notes_channel = c.u8();
      m.follow = c.u8();
      for (std::uint8_t& n : m.cc_number) n = c.u8();
      for (std::int16_t& v : m.in_offset_dms) v = s16(c);
      for (std::int16_t& v : m.out_offset_dms) v = s16(c);
      if (!shape_ok(c, newer_minor)) return kShort;
      if (const char* why = model::validate_midi_state(m)) return why;
      return p.set_midi(m) == model::Status::Ok ? nullptr : "MIDI: rejected by the model";
    }
    case Part::Usb: {
      model::UsbState u;
      std::memset(&u, 0, sizeof u);
      u.mode = c.u8();
      u.profile = c.u8();
      u.slots = c.u8();
      if (c.bad() || u.slots > model::kStemSlots) return c.bad() ? kShort : "USBS: more slots than a profile has";
      for (std::uint32_t i = 0; i < u.slots; ++i) u.mask[i] = c.u16();
      const std::uint32_t count = chunk_minor >= 1 ? c.u8() : 0u;  // USBS 1.1 (OS-USBOUTS): the real channel count follows the masks
      if (!shape_ok(c, newer_minor)) return kShort;
      if (u.profile > model::kUsbProfileMaxChunk) return "USBS: bad profile";  // the chunk's byte is 0..3, as in 1.0 (P4S and the model's P4 / P6 values are not stored)
      // 1.1 is strict (P8, 8 slots, count 4 or 6, exact length); a future minor (> 1) is lenient: the count is used only when P8, 8 slots and a count of 4 or 6 are all there, else the 1.0 view
      const bool counted = chunk_minor == 1 ? true : (chunk_minor > 1 && u.profile == kProfileP8 && u.slots == 8 && (count == 4 || count == 6));
      if (counted) {
        if (u.profile != kProfileP8 || u.slots != 8 || (count != 4 && count != 6)) return "USBS: bad channel count";
        u.profile = count == 4 ? 5 : 6;
        u.slots = static_cast<std::uint8_t>(count);
        for (std::uint32_t i = count; i < model::kStemSlots; ++i) u.mask[i] = 0;
      }
      if (const char* why = model::validate_usb_state(u)) return why;
      return p.set_usb(u) == model::Status::Ok ? nullptr : "USBS: rejected by the model";
    }
    case Part::Mix: {
      model::MixState m;
      const std::uint32_t strips = c.u8();
      for (std::uint8_t& r : m.route) r = c.u8();
      for (std::uint8_t& r : m.ret_route) r = c.u8();
      m.out_mode = c.u8();
      m.cue_level = c.u8();
      m.in_level = c.u8();
      if (!shape_ok(c, newer_minor)) return kShort;
      if (strips != model::kMixStrips) return "MIXR: not 17 strips";
      if (const char* why = model::validate_mix_state(m)) return why;
      return p.set_mix(m) == model::Status::Ok ? nullptr : "MIXR: rejected by the model";
    }
    case Part::Perf: {
      model::PerfState s;
      const std::uint32_t slots = c.u8();
      for (std::uint32_t i = 0; i < model::kPerfSlots; ++i) {
        s.fx[i] = c.u8();
        s.row[i] = c.u8();
        for (std::int16_t& v : s.value[i]) v = s16(c);
      }
      if (!shape_ok(c, newer_minor)) return kShort;
      if (slots != model::kPerfSlots) return "PERF: not 12 slots";
      // Tolerant where a later version may have extended a table (like INST's engine): an FX id the chunk's version does not have (above 19 in 1.0, above 21 in 1.1) is "none" (with its row
      // and values reset: they meant something else), a row above 3 is off. Everything else is strict.
      const std::uint8_t fx_max = chunk_minor >= 1u ? model::kPerfFxMax : kPerfFxMaxV10;
      for (std::uint32_t i = 0; i < model::kPerfSlots; ++i) {
        if (s.fx[i] > fx_max) {
          s.fx[i] = 0;
          s.row[i] = 0;
          for (std::int16_t& v : s.value[i]) v = 0;
        } else if (s.row[i] > model::kPerfRowMax) {
          s.row[i] = 0;
        }
      }
      if (const char* why = model::validate_perf_state(s)) return why;
      return p.set_perf(s) == model::Status::Ok ? nullptr : "PERF: rejected by the model";
    }
    case Part::Apps: {
      const std::uint32_t count = c.u8();
      const std::uint32_t most = chunk_minor >= 1u ? model::kMaxAppParamSets : model::kAppParamSetsV10;  // 1.0 held at most 6, 1.1 up to 12
      if (c.bad() || count < 1 || count > most) return c.bad() ? kShort : "APPM: bad app count";
      p.clear_app_params();
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* name = nullptr;
        std::size_t name_len = 0;
        if (!c.text(model::kAppNameMax, name, name_len)) return kShort;
        const std::uint8_t major = c.u8();
        const std::uint8_t minor = c.u8();
        const std::uint32_t n = c.u8();
        if (c.bad() || n < 1 || n > model::kMaxAppParams) return c.bad() ? kShort : "APPM: bad parameter count";
        float values[model::kMaxAppParams];
        for (std::uint32_t k = 0; k < n; ++k) values[k] = c.f32();
        if (c.bad()) return kShort;
        if (p.find_app_params(reinterpret_cast<const char*>(name), name_len) != nullptr) return "APPM: duplicate app";
        if (p.set_app_params(reinterpret_cast<const char*>(name), name_len, major, minor, values, n) != model::Status::Ok) {
          return "APPM: bad module name or value";
        }
      }
      return shape_ok(c, newer_minor) ? nullptr : kShort;
    }
    // OS-PERSIST2: the tolerant parts. The shape is checked first (a short or over-long chunk, a count the layout does not fit); then every value out of its range becomes its default
    // (model::sanitise_*) and is counted in `reset`; the part is present with what was taken.
    case Part::Note: {
      model::NoteState n;
      n.root = c.u8();
      n.scale = c.u8();
      n.octave = c.u8();
      n.lock = c.u8();
      n.jump = c.u8();
      if (!shape_ok(c, newer_minor)) return kShort;
      reset += model::sanitise_note_state(n);
      return p.set_note(n) == model::Status::Ok ? nullptr : "NOTE: rejected by the model";
    }
    case Part::Mute: {
      const std::uint32_t count = c.u8();
      if (c.bad() || count < 1 || count > model::kMaxTracks) return c.bad() ? kShort : "MUTE: bad track count";
      model::MuteState m;
      model::init_mute_state(m);  // the tracks the chunk does not have are Immediate
      for (std::uint32_t i = 0; i < count; ++i) m.mode[i] = c.u8();
      if (!shape_ok(c, newer_minor)) return kShort;
      reset += model::sanitise_mute_state(m);
      return p.set_mute(m) == model::Status::Ok ? nullptr : "MUTE: rejected by the model";
    }
    case Part::Scene: {
      model::ScenesState s;
      std::memset(&s, 0, sizeof s);
      s.latch = c.u8();
      const std::uint32_t count = c.u8();
      if (c.bad()) return kShort;
      if (count != model::kSceneCount) return "SCEN: not 3 scenes";
      for (model::SceneState& sc : s.scene) {
        sc.stored = c.u8();
        for (std::uint32_t i = 0; i < model::kPerfSlots; ++i) {
          sc.slots.fx[i] = c.u8();
          sc.slots.row[i] = c.u8();
          for (std::int16_t& v : sc.slots.value[i]) v = s16(c);
        }
      }
      if (!shape_ok(c, newer_minor)) return kShort;
      if (chunk_minor == 0u) {  // 1.0 has no FX 20 and 21: none in a stored scene (a scene that is not stored means nothing and sanitise zeroes it), counted as the ids past 21 are
        for (model::SceneState& sc : s.scene) {
          if (sc.stored == 1) reset += drop_fx_after_v10(sc.slots);
        }
      }
      reset += model::sanitise_scenes_state(s);
      return p.set_scenes(s) == model::Status::Ok ? nullptr : "SCEN: rejected by the model";
    }
    case Part::SendFx: {
      const std::uint32_t rows = c.u8();
      if (c.bad() || rows < 1 || rows > model::kSendFxCountMax) return c.bad() ? kShort : "SNFX: bad row count";
      model::SendFxState f;
      model::init_send_fx_state(f);  // the rows the chunk does not have are their defaults
      for (std::uint32_t i = 0; i < rows; ++i) {
        const std::uint16_t v = c.u16();
        if (i < model::kSendFxCount) {
          f.value[i] = v;
        } else {
          ++reset;  // a row of a later version: skipped
        }
      }
      if (!shape_ok(c, newer_minor)) return kShort;
      reset += model::sanitise_send_fx_state(f);
      return p.set_send_fx(f) == model::Status::Ok ? nullptr : "SNFX: rejected by the model";
    }
    case Part::Law: {
      const std::uint8_t law = c.u8();
      if (!shape_ok(c, newer_minor)) return kShort;
      return law > 1 ? "LAW: unknown law" : nullptr;  // not a default: a law this version does not know would play up to 24 dB wrong; the Reader applies it at Done
    }
  }
  return "internal";
}

}  // namespace o3os::formats::state_chunks
