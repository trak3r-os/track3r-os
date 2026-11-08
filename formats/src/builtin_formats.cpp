// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/builtin.h>

#include <algorithm>
#include <cstring>

namespace o3os::formats {

namespace {

FormatError map_error(native::Error e) noexcept {
  switch (e) {
    case native::Error::None: return FormatError::None;
    case native::Error::Truncated: return FormatError::Truncated;
    case native::Error::BadMagic: return FormatError::BadMagic;
    case native::Error::BadCrc: return FormatError::BadCrc;
    case native::Error::NewerMajor: return FormatError::UnsupportedVersion;
    case native::Error::LimitExceeded: return FormatError::LimitExceeded;
    case native::Error::Malformed: return FormatError::Malformed;
    case native::Error::SourceFailed: return FormatError::SourceFailed;
    case native::Error::SinkFailed: return FormatError::SinkFailed;
    case native::Error::Modified: return FormatError::Modified;
    case native::Error::Internal: return FormatError::Internal;
  }
  return FormatError::Internal;
}

void copy_report(const native::Report& from, FormatReport& to) noexcept {
  to.error = map_error(from.error);
  to.detail = from.detail;
  to.offset = from.offset;
  to.bytes = from.bytes;
  to.unverified_version = from.newer_minor;  // fields of a newer minor version were skipped
  to.sanitised = from.settings_skipped;      // entries and values that were out of range and read as their defaults (SETS, LRNB, the session state; the writer has none)
  to.patterns = from.patterns;
  to.instruments = from.instruments;
}

bool is(const std::uint8_t* head, std::size_t n, const char* magic, std::size_t len) noexcept {
  return n >= len && std::memcmp(head, magic, len) == 0;
}

bool sniff_native(const std::uint8_t* head, std::size_t n, std::uint32_t) noexcept {
  return n >= 12 && std::memcmp(head, "FORM", 4) == 0 && std::memcmp(head + 8, "O3PJ", 4) == 0;
}

bool sniff_og_project(const std::uint8_t* head, std::size_t n, std::uint32_t) noexcept {
  return is(head, n, "MT", 2) && (n < 4 || (head[2] == 0 && head[3] == 0));
}

bool sniff_og_pattern(const std::uint8_t* head, std::size_t n, std::uint32_t size) noexcept {
  return (is(head, n, "KS", 2) || is(head, n, "PM", 2)) &&
         (size == 0 || size == og::kPatternBytes8 || size == og::kPatternBytes12 || size == og::kPatternBytes16 ||
          size == og::kPatternBytes6_8 || size == og::kPatternBytes6_12 || size == og::kPatternBytes6_16);
  // v5 sizes: 6,184, 9,260 and 12,336 (16 tracks, OS-CEPAT S1b); v6 sizes: OS-CEPAT S1
}

bool is_instrument_head(const std::uint8_t* head, std::size_t n) noexcept {
  return is(head, n, "TI", 2) && (n < 4 || (head[2] == 1 && head[3] == 0));
}

// An .mti has one of two sizes (a file of unknown size is accepted here, so that a name that says .mti agrees, but
// "og-pti" is registered first: sniffing bytes of unknown size never answers "og-mti").
bool sniff_og_mti(const std::uint8_t* head, std::size_t n, std::uint32_t size) noexcept {
  return is_instrument_head(head, n) &&
         (size == 0 || size == og::kInstrumentBytesLegacy || size == og::kInstrumentBytes);
}

bool sniff_og_pti(const std::uint8_t* head, std::size_t n, std::uint32_t size) noexcept {
  return is_instrument_head(head, n) && (size == 0 || size > og::kInstrumentBytes);
}

bool sniff_og_pamd(const std::uint8_t* head, std::size_t n, std::uint32_t) noexcept {
  return is(head, n, "PAMD", 4);
}

bool sniff_settings(const std::uint8_t* head, std::size_t n, std::uint32_t) noexcept { return settings::sniff(head, n); }

FormatError map_settings_error(settings::Error e) noexcept {
  switch (e) {
    case settings::Error::None: return FormatError::None;
    case settings::Error::Truncated: return FormatError::Truncated;
    case settings::Error::BadMagic: return FormatError::BadMagic;
    case settings::Error::BadCrc: return FormatError::BadCrc;
    case settings::Error::NewerMajor: return FormatError::UnsupportedVersion;
    case settings::Error::Malformed: return FormatError::Malformed;
    case settings::Error::TooBig: return FormatError::LimitExceeded;
  }
  return FormatError::Internal;
}

// An OctaMED module: "MMD0" .. "MMD3" (the chained song headers MCN0.. are never a file's start) and room for the header and the song.
bool sniff_med(const std::uint8_t* head, std::size_t n, std::uint32_t size) noexcept {
  return is(head, n, "MMD", 3) && (n < 4 || (head[3] >= '0' && head[3] <= '3')) &&
         (size == 0 || size >= med::caps::kHeaderBytes + med::caps::kSongBytes);
}

// A ProTracker or Scream Tracker 3 module (OS-MODIMP): the signature the reader takes (formats/mod/module.h); the heads shorter than it needs are not claimed.
bool sniff_mod(const std::uint8_t* head, std::size_t n, std::uint32_t) noexcept { return mod::sniff_kind(head, n) == mod::Kind::Mod; }
bool sniff_s3m(const std::uint8_t* head, std::size_t n, std::uint32_t) noexcept { return mod::sniff_kind(head, n) == mod::Kind::S3m; }

bool sniff_wav(const std::uint8_t* head, std::size_t n, std::uint32_t) noexcept {
  return is(head, n, "RIFF", 4) && (n < 12 || std::memcmp(head + 8, "WAVE", 4) == 0);
}

bool sniff_iff(const std::uint8_t* head, std::size_t n, std::uint32_t size) noexcept { return iff::sniff(head, n, size); }

bool sniff_wt(const std::uint8_t* head, std::size_t n, std::uint32_t) noexcept { return is(head, n, "vawt", 4); }

}  // namespace

void detail::NativeReaderStrategy::begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams&) noexcept {
  reader_.begin(source, arena);
  report_ = FormatReport{};
}

Progress detail::NativeReaderStrategy::step(std::size_t byte_budget) noexcept {
  const Progress p = reader_.step(byte_budget);
  copy_report(reader_.report(), report_);
  return p;
}

void detail::NativeWriterStrategy::begin(const model::Project& project, core::ByteSink& sink, const WriteParams&) noexcept {
  writer_.begin(project, sink);
  report_ = FormatReport{};
}

Progress detail::NativeWriterStrategy::step(std::size_t byte_budget) noexcept {
  const Progress p = writer_.step(byte_budget);
  copy_report(writer_.report(), report_);
  return p;
}

// ---- the device settings file (OS-002d) -----------------------------------------------------------------------------------

void detail::SettingsReaderStrategy::fail(FormatError e, const char* why) noexcept {
  report_.error = e;
  report_.detail = why;
  report_.offset = report_.bytes;
  progress_ = Progress::Failed;
  if (target_ != nullptr) {
    target_->init(core::ParamKind::Device);  // a file that fails leaves the defaults, valid
  }
}

void detail::SettingsReaderStrategy::begin(core::ByteSource& source, model::ModelArena&, const ReadParams& params) noexcept {
  source_ = &source;
  target_ = params.settings;
  report_ = FormatReport{};
  have_ = 0;
  progress_ = Progress::Running;
  if (target_ == nullptr) {
    fail(FormatError::Internal, "no settings block to read into");
    return;
  }
  target_->init(core::ParamKind::Device);
}

Progress detail::SettingsReaderStrategy::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running && budget != 0) {
    const std::size_t ask = std::min(budget, sizeof buf_ - have_);
    if (ask == 0) {
      fail(FormatError::LimitExceeded, "file larger than the limit");
      break;
    }
    const core::IoResult r = source_->read(buf_ + have_, ask);
    if (r.status == core::IoStatus::WouldBlock) {
      return Progress::Running;
    }
    if (r.status == core::IoStatus::End) {
      const settings::Report done = settings::read(buf_, have_, *target_);  // leaves the defaults after an error
      report_.error = map_settings_error(done.error);
      report_.detail = done.detail;
      report_.offset = done.offset;
      report_.sanitised = done.skipped_entries;
      report_.unverified_version = done.newer_minor;
      progress_ = done.error == settings::Error::None ? Progress::Done : Progress::Failed;
      break;
    }
    if (r.status != core::IoStatus::Ok || r.count == 0 || r.count > ask) {
      fail(FormatError::SourceFailed, r.status == core::IoStatus::Ok ? "source broke its contract" : "source failed");
      break;
    }
    have_ += r.count;
    budget -= r.count;
    report_.bytes += static_cast<std::uint32_t>(r.count);
  }
  return progress_;
}

void detail::SettingsWriterStrategy::fail(FormatError e, const char* why) noexcept {
  report_.error = e;
  report_.detail = why;
  report_.offset = report_.bytes;
  progress_ = Progress::Failed;
}

void detail::SettingsWriterStrategy::begin(const model::Project&, core::ByteSink& sink, const WriteParams& params) noexcept {
  sink_ = &sink;
  report_ = FormatReport{};
  size_ = 0;
  sent_ = 0;
  progress_ = Progress::Running;
  if (params.settings == nullptr) {
    fail(FormatError::Internal, "no settings block to write");
    return;
  }
  size_ = settings::write(*params.settings, buf_, sizeof buf_);
  if (size_ == 0) {
    fail(FormatError::Malformed, "the block is not valid settings of the device");
  }
}

Progress detail::SettingsWriterStrategy::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    if (sent_ == size_) {
      progress_ = Progress::Done;
      break;
    }
    if (budget == 0) {
      return Progress::Running;
    }
    const std::size_t ask = std::min(size_ - sent_, budget);
    const core::IoResult r = sink_->write(buf_ + sent_, ask);
    if (r.status == core::IoStatus::WouldBlock) {
      return Progress::Running;
    }
    if (r.status != core::IoStatus::Ok || r.count == 0 || r.count > ask) {
      fail(FormatError::SinkFailed, r.status == core::IoStatus::Ok ? "sink broke its contract" : "sink failed");
      break;
    }
    sent_ += r.count;
    budget -= r.count;
    report_.bytes += static_cast<std::uint32_t>(r.count);
  }
  return progress_;
}

bool register_builtin_formats(FormatRegistry& registry, BuiltinFormats& s) noexcept {
  bool ok = true;
  auto add = [&](const char* id, const char* title, const char* names, FormatKind kind, SniffFn sniff,
                 FormatReader* reader, FormatWriter* writer, GroupNameFn group_name = nullptr) {
    FormatDesc d;
    d.id = id;
    d.title = title;
    d.names = names;
    d.kind = kind;
    d.sniff = sniff;
    d.group_name = group_name;
    d.reader = reader;
    d.writer = writer;
    ok = registry.add(d) && ok;
  };
  add("native", "Tracker OS project", ".o3pj", FormatKind::Project, sniff_native, &s.native_reader, &s.native_writer);
  add("og-project", "Tracker OG project.mt", ".mt", FormatKind::ProjectSettings, sniff_og_project,
      &s.og_project_reader, &s.og_project_writer, og::drop_group_name);
  add("og-pattern", "Tracker OG pattern", ".mtp", FormatKind::Pattern, sniff_og_pattern, &s.og_pattern_reader,
      &s.og_pattern_writer);
  add("og-pti", "Tracker OG instrument (.pti)", ".pti", FormatKind::Instrument, sniff_og_pti, &s.og_pti_reader,
      &s.og_pti_writer);
  add("og-mti", "Tracker OG instrument (.mti)", ".mti", FormatKind::Instrument, sniff_og_mti, &s.og_mti_reader,
      nullptr);
  add("og-pamd", "Tracker OG pattern names", "patternsmetadata", FormatKind::PatternNames, sniff_og_pamd,
      &s.og_pamd_reader, &s.og_pamd_writer);
  add("wav", "WAV sample", ".wav", FormatKind::Sample, sniff_wav, &s.wav_reader, nullptr);
  add("surge-wt", "Surge wavetable", ".wt", FormatKind::Sample, sniff_wt, &s.wt_reader, nullptr);
  add("iff-sample", "AIFF / 8SVX sample", ".aif .aiff .aifc .8svx .svx .iff", FormatKind::Sample, sniff_iff, &s.iff_reader, nullptr);
  add("settings", "Tracker OS device settings", ".o3st", FormatKind::Settings, sniff_settings, &s.settings_reader, &s.settings_writer);
  add("med", "OctaMED module", ".med .mmd .mmd0 .mmd1 .mmd2 .mmd3", FormatKind::Project, sniff_med, &s.med_reader, nullptr,
      med::drop_group_name);
  add("mod", "ProTracker module", ".mod", FormatKind::Project, sniff_mod, &s.mod_reader, nullptr, mod::drop_group_name);
  add("s3m", "Scream Tracker 3 module", ".s3m", FormatKind::Project, sniff_s3m, &s.mod_reader, nullptr, mod::drop_group_name);
  return ok;
}

}  // namespace o3os::formats
