// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/og.h>

#include "og_codec.h"

#include <algorithm>
#include <cstring>

namespace o3os::formats::og {

namespace {

constexpr const char* kDropNames[kDropGroupCount] = {
    "session", "delay", "limiter", "reverb", "inputs", "performance", "midi velocity", "pattern bitmask",
    "1.3-1.6 extension", "1.7-1.9 extension",
};

std::uint32_t add_sat(std::uint32_t a, std::size_t n) noexcept {
  return n >= 0xFFFFFFFFu - a ? 0xFFFFFFFFu : a + static_cast<std::uint32_t>(n);
}

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// `n` characters of `s` equal to the lower-case word `word`, without regard to case.
bool word_at(const char* s, const char* word) noexcept {
  for (; *word != '\0'; ++s, ++word) {
    if (lower(*s) != *word) {
      return false;
    }
  }
  return true;
}

// 1..max_digits decimal digits at s; value into `v`, count into `digits`.
bool digits_at(const char* s, unsigned max_digits, std::uint32_t& v, unsigned& digits) noexcept {
  v = 0;
  digits = 0;
  while (s[digits] >= '0' && s[digits] <= '9') {
    if (digits == max_digits) {
      return false;
    }
    v = v * 10u + static_cast<std::uint32_t>(s[digits] - '0');
    ++digits;
  }
  return digits != 0;
}

bool ends_with(const char* s, std::size_t len, const char* lower_suffix) noexcept {
  std::size_t n = 0;
  while (lower_suffix[n] != '\0') {
    ++n;
  }
  if (len < n) {
    return false;
  }
  return word_at(s + len - n, lower_suffix);
}

}  // namespace

const char* drop_group_name(unsigned index) noexcept { return index < kDropGroupCount ? kDropNames[index] : ""; }

void ExportDrops::add(const ExportDrops& o) noexcept {
  auto sum = [](std::uint32_t& a, std::uint32_t b) { a = b > 0xFFFFFFFFu - a ? 0xFFFFFFFFu : a + b; };
  sum(tracks_beyond_12, o.tracks_beyond_12);
  sum(engine_fx, o.engine_fx);
  sum(core_fx, o.core_fx);
  sum(volume_as_fx18, o.volume_as_fx18);
  sum(volume_dropped, o.volume_dropped);
  sum(track_settings, o.track_settings);
  sum(audio_on_midi_tracks, o.audio_on_midi_tracks);
  sum(track_names_cut, o.track_names_cut);
  sum(project_settings, o.project_settings);
  sum(module_instruments, o.module_instruments);
  sum(midi_instruments, o.midi_instruments);
  sum(sample_slots_beyond_48, o.sample_slots_beyond_48);
  sum(unknown_chunks, o.unknown_chunks);
  sum(instrument_names_cut, o.instrument_names_cut);
  sum(lfo_sine, o.lfo_sine);
  sum(pattern_names, o.pattern_names);
  sum(settings_ignored, o.settings_ignored);
  sum(midi_settings, o.midi_settings);
  sum(resampled, o.resampled);
  sum(rate_ignored, o.rate_ignored);
  sum(amiga_engine, o.amiga_engine);
  sum(bit_engine, o.bit_engine);
  sum(pcm_too_long, o.pcm_too_long);
  sum(instruments_not_written, o.instruments_not_written);
  sum(no_sample_memory, o.no_sample_memory);
  sum(levels_converted, o.levels_converted);
  sum(levels_clamped, o.levels_clamped);
  sum(stereo_mid, o.stereo_mid);
  defaulted_groups |= o.defaulted_groups;
}

bool ExportDrops::any_lost() const noexcept {
  return (tracks_beyond_12 | engine_fx | core_fx | volume_dropped | track_settings | track_names_cut | project_settings |
          module_instruments | midi_instruments | sample_slots_beyond_48 | unknown_chunks | instrument_names_cut |
          pattern_names | settings_ignored | midi_settings | lfo_sine | rate_ignored | amiga_engine | bit_engine | pcm_too_long | instruments_not_written | levels_clamped | stereo_mid) != 0;
}

bool inspect_settings(const std::uint8_t* payload, std::uint32_t length, std::uint16_t& groups,
                      std::uint8_t* v17_byte) noexcept {
  codec::settings::Parsed parsed;
  const bool ok = payload != nullptr && codec::settings::parse(payload, length, parsed);
  groups = ok ? parsed.groups : std::uint16_t{0};
  if (v17_byte != nullptr) {
    *v17_byte = ok && parsed.tail != nullptr ? *parsed.tail : std::uint8_t{0};
  }
  return ok;
}

bool inspect_names(const std::uint8_t* payload, std::uint32_t length, std::uint32_t& file_length) noexcept {
  file_length = 0;
  if (payload == nullptr || length < codec::settings::kNamesHeader + 4) {
    return false;
  }
  const std::uint32_t version = static_cast<std::uint32_t>(payload[0] << 8 | payload[1]);
  const std::uint32_t total = (static_cast<std::uint32_t>(payload[2]) << 24) | (static_cast<std::uint32_t>(payload[3]) << 16) |
                              (static_cast<std::uint32_t>(payload[4]) << 8) | payload[5];
  const std::uint32_t kept = length - codec::settings::kNamesHeader;
  // a PAMD file: 16..12,816 bytes, "PAMD" first; the kept part (the file without its trailing zeros) is no longer than it
  if ((version >> 8) != 1 || total < kPamdHeader || total > kPamdBytes || kept > total ||
      std::memcmp(payload + codec::settings::kNamesHeader, "PAMD", 4) != 0) {
    return false;
  }
  file_length = total;
  return true;
}

// ---- readers ----------------------------------------------------------------------------------------------

void detail::ReaderBase::begin_base(core::ByteSource& source, model::ModelArena& arena) noexcept {
  source_ = &source;
  project_ = &arena.project;
  progress_ = Progress::Running;
  report_ = ImportReport{};
  crc_.reset();
}

void detail::ReaderBase::fail(FormatError e, const char* detail) noexcept {
  report_.error = e;
  report_.detail = detail;
  report_.offset = report_.bytes;
  progress_ = Progress::Failed;
}

detail::ReaderBase::Pull detail::ReaderBase::pull(std::uint8_t* dst, std::size_t want, std::size_t& have,
                                                  std::size_t& budget, bool crc) noexcept {
  while (have < want) {
    if (budget == 0) {
      return Pull::Partial;
    }
    const std::size_t ask = std::min(want - have, budget);
    const core::IoResult r = source_->read(dst + have, ask);
    switch (r.status) {
      case core::IoStatus::Ok:
        if (r.count == 0 || r.count > ask) {
          fail(FormatError::SourceFailed, "source broke its contract");
          return Pull::Error;
        }
        if (crc) {
          crc_.update(dst + have, r.count);
        }
        have += r.count;
        budget -= r.count;
        report_.bytes = add_sat(report_.bytes, r.count);
        break;
      case core::IoStatus::WouldBlock:
        return Pull::Partial;
      case core::IoStatus::End:
        return Pull::End;
      case core::IoStatus::Failed:
      default:
        fail(FormatError::SourceFailed, "source failed");
        return Pull::Error;
    }
  }
  return Pull::Complete;
}

// ---- writers ----------------------------------------------------------------------------------------------

void detail::WriterBase::begin_base(const model::Project& project, core::ByteSink& sink) noexcept {
  project_ = &project;
  sink_ = &sink;
  progress_ = Progress::Running;
  report_ = ExportReport{};
  crc_.reset();
  generation_ = project.generation();
}

void detail::WriterBase::fail(FormatError e, const char* detail) noexcept {
  report_.error = e;
  report_.detail = detail;
  report_.offset = report_.bytes;
  progress_ = Progress::Failed;
}

detail::WriterBase::Flush detail::WriterBase::flush(const std::uint8_t* buf, std::size_t len, std::size_t& off,
                                                    std::size_t& budget) noexcept {
  while (off < len) {
    if (budget == 0) {
      return Flush::Partial;
    }
    const std::size_t ask = std::min(len - off, budget);
    const core::IoResult r = sink_->write(buf + off, ask);
    switch (r.status) {
      case core::IoStatus::Ok:
        if (r.count == 0 || r.count > ask) {
          fail(FormatError::SinkFailed, "sink broke its contract");
          return Flush::Error;
        }
        off += r.count;
        budget -= r.count;
        report_.bytes = add_sat(report_.bytes, r.count);
        break;
      case core::IoStatus::WouldBlock:
        return Flush::Partial;
      case core::IoStatus::End:
      case core::IoStatus::Failed:
      default:
        fail(FormatError::SinkFailed, "sink failed");
        return Flush::Error;
    }
  }
  return Flush::Done;
}

bool detail::WriterBase::unchanged() noexcept {
  if (project_->generation() != generation_) {
    fail(FormatError::Modified, "the project changed while it was being written");
    return false;
  }
  return true;
}

// ---- instruments and file names ---------------------------------------------------------------------------

ExportClass export_class(const model::Project& project, std::uint32_t slot) noexcept {
  const model::Instrument* in = project.instrument(slot);
  if (in == nullptr) {
    return ExportClass::Empty;
  }
  switch (in->kind) {
    case model::InstrumentKind::Sample:
      return slot < kSampleSlots ? ExportClass::Pti : ExportClass::SlotBeyond48;
    case model::InstrumentKind::Midi:
      return (slot >= kMidiFirstSlot && in->midi_channel == slot - kMidiFirstSlot + 1) ? ExportClass::ImplicitMidi
                                                                                       : ExportClass::MidiMoved;
    case model::InstrumentKind::Module:
      return ExportClass::Module;
    case model::InstrumentKind::None:
    default:
      return ExportClass::Empty;
  }
}

void count_instrument_drops(const model::Project& project, ExportDrops& drops) noexcept {
  for (std::uint32_t slot = 0; slot < model::kMaxInstruments; ++slot) {
    switch (export_class(project, slot)) {
      case ExportClass::Module: ++drops.module_instruments; break;
      case ExportClass::MidiMoved: ++drops.midi_instruments; break;
      case ExportClass::SlotBeyond48: ++drops.sample_slots_beyond_48; break;
      case ExportClass::ImplicitMidi: drops.midi_settings += model::inst_v13_default(*project.instrument(slot)) ? 0u : 1u; break;
      default: break;
    }
  }
}

FileName classify_file_name(const char* name) noexcept {
  FileName out;
  if (name == nullptr) {
    return out;
  }
  const char* base = name;
  for (const char* p = name; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  std::size_t len = 0;
  while (base[len] != '\0') {
    ++len;
  }
  std::uint32_t v;
  unsigned digits;

  if (len == 10 && word_at(base, "project.mt")) {
    out.file_class = FileClass::Project;
  } else if (len == 16 && word_at(base, "patternsmetadata")) {
    out.file_class = FileClass::PatternNames;
  } else if (word_at(base, "pattern_") && digits_at(base + 8, 3, v, digits) && v >= 1 && v <= 255 &&
             len == 8 + digits + 4 && word_at(base + 8 + digits, ".mtp")) {
    out.file_class = FileClass::Pattern;
    out.index = static_cast<std::uint16_t>(v);
  } else if (word_at(base, "instrument_") && digits_at(base + 11, 2, v, digits) && v >= 1 && v <= kSampleSlots &&
             len == 11 + digits + 4 && (word_at(base + 11 + digits, ".mti") || word_at(base + 11 + digits, ".pti"))) {
    // instrument_NN.mti is the name in the older .mti fixtures; instrument_NN.pti is the Workspace name documented for firmware 1.5+ (not verified)
    out.file_class = lower(base[11 + digits + 3]) == 'i' && lower(base[11 + digits + 1]) == 'm' ? FileClass::Mti : FileClass::Pti;
    out.index = static_cast<std::uint16_t>(v - 1);
  } else if (digits_at(base, 3, v, digits) && base[digits] == ' ' && v >= 1 && v <= kSampleSlots &&
             ends_with(base + digits + 1, len - digits - 1, ".pti") && len - digits - 1 >= 4) {
    out.file_class = FileClass::Pti;
    out.index = static_cast<std::uint16_t>(v - 1);
  }
  return out;
}

std::size_t format_pti_name(char* dst, std::size_t cap, std::uint32_t slot, const char* name) noexcept {
  constexpr std::size_t kFixed = 3 + 4;  // "NN " and ".pti"
  if (dst == nullptr || cap < kFixed + 1 || slot >= kSampleSlots) {
    return 0;
  }
  const std::uint32_t nn = slot + 1;
  dst[0] = static_cast<char>('0' + nn / 10);
  dst[1] = static_cast<char>('0' + nn % 10);
  dst[2] = ' ';
  std::size_t n = 3;
  if (name != nullptr) {
    for (; *name != '\0' && n + 5 < cap; ++name, ++n) {
      const unsigned char c = static_cast<unsigned char>(*name);
      const bool bad = c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' || c == '"' || c == '*' ||
                       c == '?' || c == '<' || c == '>' || c == '|';
      dst[n] = bad ? '_' : *name;
    }
  }
  dst[n++] = '.';
  dst[n++] = 'p';
  dst[n++] = 't';
  dst[n++] = 'i';
  dst[n] = '\0';
  return n;
}

}  // namespace o3os::formats::og
