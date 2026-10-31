// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/og_export.h>

#include <o3os/app/play_mode.h>
#include <o3os/app/sample_bank.h>

#include <cstring>

namespace o3os::app {

namespace guard = hal::storage::save;

bool og_folder_name(char* out, std::size_t cap, const char* project, unsigned n) noexcept {
  if (out == nullptr || cap == 0u) {
    return false;
  }
  out[0] = '\0';
  if (n < 1u || n > guard::kOgFolderMaxN) {
    return false;
  }
  char buf[guard::kNameMax + 1];
  std::size_t len = 0;
  if (project != nullptr) {
    while (project[len] != '\0' && len < guard::kOgNameRoom) {
      buf[len] = project[len];
      ++len;
    }
  }
  while (len != 0u && (buf[len - 1u] == ' ' || buf[len - 1u] == '.')) {
    --len;  // (the cut can leave a space or a dot at the end: the guard takes neither)
  }
  if (len == 0u) {
    std::memcpy(buf, "Untitled", 8);
    len = 8;
  }
  std::memcpy(buf + len, guard::kOgFolderTag, sizeof guard::kOgFolderTag - 1u);
  len += sizeof guard::kOgFolderTag - 1u;
  if (n >= 2u) {
    buf[len++] = ' ';
    if (n >= 10u) {
      buf[len++] = static_cast<char>('0' + n / 10u);
    }
    buf[len++] = static_cast<char>('0' + n % 10u);
  }
  buf[len] = '\0';
  if (len + 1u > cap || !guard::og_export_folder(buf)) {
    return false;
  }
  std::memcpy(out, buf, len + 1u);
  return true;
}

std::size_t og_pattern_file_name(char* out, std::size_t cap, unsigned id) noexcept {
  constexpr std::size_t kPre = sizeof guard::kOgPatternPrefix - 1u;
  constexpr std::size_t kSuf = sizeof guard::kOgPatternSuffix - 1u;
  if (out == nullptr || cap < kPre + 3u + kSuf + 1u || id < 1u || id > 255u) {
    return 0;
  }
  std::memcpy(out, guard::kOgPatternPrefix, kPre);
  std::size_t n = kPre;
  if (id >= 100u) {
    out[n++] = static_cast<char>('0' + id / 100u);
  }
  out[n++] = static_cast<char>('0' + (id / 10u) % 10u);
  out[n++] = static_cast<char>('0' + id % 10u);
  std::memcpy(out + n, guard::kOgPatternSuffix, kSuf + 1u);
  return n + kSuf;
}

// ---- The PCM stage's decision -------------------------------------------------------------------------------------------------------------------

OgInstrumentPlan og_instrument_plan(const model::Project& project, const SampleBank* bank, unsigned slot, formats::og::ExportDrops* count) noexcept {
  OgInstrumentPlan r;
  formats::og::ExportDrops none;
  formats::og::ExportDrops& c = count != nullptr ? *count : none;
  const model::Instrument* in = slot < guard::kOgFolderLastSlot ? project.instrument(slot) : nullptr;
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return r;  // (not a sample instrument: nothing here, the caller's export_class decides what that means)
  }
  const SampleBank::Pcm pcm = bank != nullptr ? bank->pcm(slot) : SampleBank::Pcm{};
  if (pcm.data == nullptr || pcm.frames == 0u) {
    ++c.instruments_not_written;
    c.no_sample_memory += bank == nullptr ? 1u : 0u;  // (no bank at all is its own reason: the sample memory is not up, which is not the same as a slot with no audio)
    return r;
  }
  r.pcm = pcm.data;
  r.frames = pcm.frames;
  const bool amiga = bank->engine_for(slot, *in) == audio::kEngineAmiga;
  c.amiga_engine += amiga ? 1u : 0u;
  const PtiCheck check = pti_check(project, *bank, slot, *in);
  r.result = OgInstrumentPlan::Result::Refused;
  switch (check.verdict) {
    case PtiVerdict::BitEngine:
      ++c.bit_engine;
      ++c.instruments_not_written;
      return r;
    case PtiVerdict::AmigaEngine:
      ++c.instruments_not_written;
      return r;
    case PtiVerdict::TooLong:
      ++c.pcm_too_long;
      ++c.instruments_not_written;
      return r;
    case PtiVerdict::LevelClamps: {  // under -24 dB the 1.2+ law has nothing quieter than level 1: written there (louder than the instrument), counted
      bool clamped = false;
      r.level = static_cast<std::int16_t>(level_1114_to_12(in->sampler.volume, clamped));
      ++c.levels_clamped;
      break;
    }
    case PtiVerdict::Ok:
      r.level = check.level;
      c.levels_converted += check.level >= 0 && check.level != static_cast<std::int16_t>(in->sampler.volume) ? 1u : 0u;
      break;
  }
  r.result = OgInstrumentPlan::Result::Write;
  c.stereo_mid += bank->edit_stereo(slot) ? 1u : 0u;  // OS-STEREO (e): the file holds M (r.pcm), as for a mono sample; S is counted, not written
  r.plan = check.plan;
  r.fine = check.fine;
  c.resampled += !check.plan.identity ? 1u : 0u;
  c.rate_ignored += in->sample_rate != 0u && in->sample_rate != kOgRateHz && in->sampler.play_mode == kPlayModeWavetable ? 1u : 0u;
  return r;
}

// ---- What the user is told ----------------------------------------------------------------------------------------------------------------------

unsigned og_loss_items(const formats::og::ExportDrops& d, OgLossItem (&items)[kOgLossMax]) noexcept {
  const auto minus = [](std::uint32_t a, std::uint32_t b) { return a > b ? a - b : 0u; };
  const struct {
    std::uint32_t n;
    const char* brief;
    const char* full;
  } kinds[kOgLossMax] = {
      {minus(d.instruments_not_written, d.no_sample_memory), "NOT WRITTEN", "SAMPLE INSTRUMENTS WITH NO FILE (ALL REASONS)"},
      {d.no_sample_memory, "NO SAMPLE MEM", "INSTRUMENTS NOT WRITTEN: SAMPLE MEMORY NOT READY"},
      {d.bit_engine, "12/8 BIT", "12 / 8 BIT ENGINE INSTRUMENTS (NOT WRITTEN)"},
      {d.amiga_engine, "AMIGA", "AMIGA ENGINE INSTRUMENTS (ENGINE NOT KEPT)"},
      {d.stereo_mid, "STEREO", "STEREO INSTRUMENTS (MID WRITTEN, SIDE NOT KEPT)"},
      {d.pcm_too_long, "TOO LONG", "INSTRUMENTS TOO LONG FOR A FILE (NOT WRITTEN)"},
      {d.levels_clamped, "CLAMPED", "LEVELS CLAMPED (LOUDER THAN PLAYED)"},
      {d.module_instruments, "MODULES", "MODULE INSTRUMENTS"},
      {d.engine_fx + d.core_fx + d.volume_dropped, "FX", "EFFECT AND VOLUME COLUMNS"},
      {d.tracks_beyond_12, "STEPS 13-16", "STEPS ON TRACKS 13-16"},
      {d.track_settings, "TRACK SET", "TRACKS WITH PAN, SOLO OR BUS"},
      {d.midi_instruments + d.midi_settings, "MIDI INSTR", "MIDI INSTRUMENTS AND SETTINGS"},
      {d.sample_slots_beyond_48, "SLOTS 49+", "SAMPLE INSTRUMENTS IN SLOTS 49-64"},
      {d.lfo_sine, "SINE LFO", "SINE LFO SHAPES WRITTEN AS TRIANGLE"},
      {d.track_names_cut + d.instrument_names_cut, "NAMES CUT", "NAMES CUT SHORT"},
      {d.project_settings + d.unknown_chunks + d.pattern_names + d.settings_ignored + d.rate_ignored, "OTHER", "OTHER SETTINGS, NAMES AND RATES"},
  };
  unsigned count = 0;
  for (const auto& k : kinds) {
    if (k.n != 0u) {
      items[count++] = OgLossItem{k.n, k.brief, k.full};
    }
  }
  return count;
}

bool og_loss_line(const formats::og::ExportDrops& d, char* out, unsigned cap) noexcept {
  if (cap == 0u) {
    return false;
  }
  out[0] = '\0';
  OgLossItem items[kOgLossMax];
  const unsigned count = og_loss_items(d, items);
  unsigned pos = 0;
  const auto put = [&](const char* text, unsigned len) {
    for (unsigned i = 0; i < len && pos + 1u < cap; ++i) {
      out[pos++] = text[i];
    }
    out[pos] = '\0';
  };
  for (unsigned i = 0; i < count; ++i) {
    char part[40];
    unsigned len = 0;
    char digits[10];
    unsigned nd = 0;
    for (std::uint32_t v = items[i].n; v != 0u || nd == 0u; v /= 10u) {
      digits[nd++] = static_cast<char>('0' + v % 10u);
    }
    while (nd != 0u) {
      part[len++] = digits[--nd];
    }
    part[len++] = ' ';
    for (const char* b = items[i].brief; *b != '\0' && len + 1u < sizeof part; ++b) {
      part[len++] = *b;
    }
    const unsigned sep = pos != 0u ? 2u : 0u;
    const bool more = i + 1u < count;
    if (pos + sep + len + (more ? 4u : 0u) + 1u > cap) {  // (room for ", .." after it when others follow)
      if (pos == 0u) {
        put(part, len);  // the first item alone is too long for the line: cut
      } else {
        put(", ..", 4);
      }
      return true;
    }
    if (sep != 0u) {
      put(", ", 2);
    }
    put(part, len);
  }
  return count != 0u;
}

std::uint32_t og_loss_signature(const formats::og::ExportDrops& d) noexcept {
  static_assert(sizeof(formats::og::ExportDrops) % sizeof(std::uint32_t) == 0, "ExportDrops is a run of 32-bit counters");
  std::uint32_t h = 2166136261u;
  for (std::size_t i = 0; i < sizeof d; i += sizeof(std::uint32_t)) {
    std::uint32_t w;
    std::memcpy(&w, reinterpret_cast<const unsigned char*>(&d) + i, sizeof w);
    h = (h ^ w) * 16777619u;
  }
  return h;
}

// ---- The plan ---------------------------------------------------------------------------------------------------------------------------------

bool OgExportPlan::begin(const model::Project& project, const SampleBank* bank) noexcept {
  project_ = &project;
  bank_ = bank;
  generation_ = project.generation();
  drops_ = formats::og::ExportDrops{};
  write_mask_ = 0;
  bytes_ = 0;
  unsigned instruments = 0;
  for (unsigned slot = 0; slot < guard::kOgFolderLastSlot; ++slot) {
    versions_[slot] = bank != nullptr ? bank->version(slot) : 0u;
    if (formats::og::export_class(project, slot) == formats::og::ExportClass::Pti) {
      const OgInstrumentPlan ip = og_instrument_plan(project, bank, slot, &drops_);
      if (ip.result == OgInstrumentPlan::Result::Write) {
        write_mask_ |= std::uint64_t{1} << slot;
        ++instruments;
        bytes_ += formats::og::kInstrumentBytes + ip.plan.frames_out * 2u;  // (16-bit mono PCM after the instrument's header)
      }
    }
  }
  slot_ = 0;
  cursor_ = 1;
  taken_ = 0;
  total_ = instruments + project.pattern_count() + 2u;  // the instruments, the patterns, patternsMetadata, project.mt
  bytes_ += project.pattern_count() * formats::og::kPatternBytes12 + formats::og::kPamdBytes + formats::og::kProjectBytes16;
  stage_ = Stage::Instruments;
  seek_instrument();
  if (project.pattern_count() == 0u) {
    stage_ = Stage::Done;
    total_ = 0;
    return false;
  }
  return true;
}

bool OgExportPlan::changed() const noexcept {
  if (project_ == nullptr || project_->generation() != generation_) {
    return true;
  }
  for (unsigned slot = 0; bank_ != nullptr && slot < guard::kOgFolderLastSlot; ++slot) {
    if (bank_->version(slot) != versions_[slot]) {
      return true;
    }
  }
  return false;
}

// The cursor to the next sample slot that gets a file; past the last one the stage is the patterns'.
void OgExportPlan::seek_instrument() noexcept {
  while (stage_ == Stage::Instruments && slot_ < guard::kOgFolderLastSlot && ((write_mask_ >> slot_) & 1u) == 0u) {
    ++slot_;
  }
  if (stage_ == Stage::Instruments && slot_ >= guard::kOgFolderLastSlot) {
    stage_ = Stage::Patterns;
    seek_pattern();
  }
}

// The cursor to the next pattern id the project holds; past the last one the stage is the PAMD file's.
void OgExportPlan::seek_pattern() noexcept {
  while (stage_ == Stage::Patterns && cursor_ <= 255u && project_->pattern(cursor_) == nullptr) {
    ++cursor_;
  }
  if (stage_ == Stage::Patterns && cursor_ > 255u) {
    stage_ = Stage::Pamd;
  }
}

bool OgExportPlan::next(OgFile& out) const noexcept {
  out = OgFile{};
  switch (stage_) {
    case Stage::Instruments: {
      char stem[guard::kNameMax + 1];
      const model::Instrument* in = project_->instrument(slot_);
      if (in == nullptr || og_pti_stem(slot_, in->name, stem, sizeof stem) == 0u) {
        return false;
      }
      out.kind = OgFile::Kind::Instrument;
      out.id = static_cast<std::uint16_t>(slot_);
      out.in_instruments = true;
      std::memcpy(out.name, stem, std::strlen(stem));
      std::strcpy(out.name + std::strlen(stem), guard::kPtiSuffix);
      std::strcpy(out.label, "INSTRUMENT ");
      out.label[11] = static_cast<char>('0' + (slot_ + 1u) / 10u);
      out.label[12] = static_cast<char>('0' + (slot_ + 1u) % 10u);
      out.label[13] = '\0';
      return true;
    }
    case Stage::Patterns: {
      out.kind = OgFile::Kind::Pattern;
      out.id = static_cast<std::uint16_t>(cursor_);
      out.in_patterns = true;
      (void)og_pattern_file_name(out.name, sizeof out.name, cursor_);
      std::memcpy(out.label, "PATTERN ", 8);
      std::size_t n = 8;
      if (cursor_ >= 100u) {
        out.label[n++] = static_cast<char>('0' + cursor_ / 100u);
      }
      out.label[n++] = static_cast<char>('0' + (cursor_ / 10u) % 10u);
      out.label[n++] = static_cast<char>('0' + cursor_ % 10u);
      out.label[n] = '\0';
      return true;
    }
    case Stage::Pamd:
      out.kind = OgFile::Kind::Pamd;
      out.in_patterns = true;
      std::strcpy(out.name, guard::kOgPatternsMetadata);
      std::strcpy(out.label, "PATTERN NAMES");
      return true;
    case Stage::Project:
      out.kind = OgFile::Kind::Project;
      std::strcpy(out.name, guard::kProjectMtFile);
      std::strcpy(out.label, "PROJECT");
      return true;
    case Stage::Done:
      break;
  }
  return false;
}

void OgExportPlan::advance() noexcept {
  switch (stage_) {
    case Stage::Instruments:
      ++slot_;
      seek_instrument();
      break;
    case Stage::Patterns:
      ++cursor_;
      seek_pattern();
      break;
    case Stage::Pamd:
      stage_ = Stage::Project;
      break;
    case Stage::Project:
      stage_ = Stage::Done;
      break;
    case Stage::Done:
      return;
  }
  ++taken_;
}

// ---- The survey: the writers' counters without a card -------------------------------------------------------------------------------------------------

namespace {

// A sink that takes everything and keeps nothing.
class CountSink final : public core::ByteSink {
 public:
  core::IoResult write(const void*, std::size_t n) noexcept override { return core::IoResult{n, core::IoStatus::Ok}; }
};

// Runs a writer to its end; true when it finished (a document that changes mid-way cannot here: nothing runs between the steps).
template <typename Writer>
bool run_dry(Writer& w, const model::Project& project, CountSink& sink, const formats::WriteParams& params, formats::og::ExportDrops& out) noexcept {
  w.begin(project, sink, params);
  formats::Progress p = formats::Progress::Running;
  for (unsigned guard_steps = 0; p == formats::Progress::Running && guard_steps < 100000u; ++guard_steps) {
    p = w.step(1u << 16);
  }
  if (p != formats::Progress::Done) {
    return false;
  }
  out.add(w.export_report().drops);
  return true;
}

}  // namespace

void OgExportPlan::survey_start(formats::og::ExportDrops& out) noexcept {
  out = drops_;
  survey_stage_ = project_ != nullptr ? SurveyStage::Project : SurveyStage::Done;
  survey_next_ = 0;
}

bool OgExportPlan::survey_step(formats::og::ExportDrops& out) noexcept {
  if (project_ == nullptr) {
    return true;
  }
  CountSink sink;
  formats::WriteParams params;
  params.target = formats::og::kTargetOg190;
  for (;;) {
    switch (survey_stage_) {
      case SurveyStage::Project: {
        formats::og::ProjectWriter w;
        (void)run_dry(w, *project_, sink, params, out);
        survey_stage_ = SurveyStage::Pamd;
        return false;
      }
      case SurveyStage::Pamd: {
        formats::og::PamdWriter w;
        (void)run_dry(w, *project_, sink, params, out);
        survey_stage_ = SurveyStage::Patterns;
        survey_next_ = 1;
        return false;
      }
      case SurveyStage::Patterns: {
        while (survey_next_ <= 255u && project_->pattern(survey_next_) == nullptr) {
          ++survey_next_;
        }
        if (survey_next_ > 255u) {
          survey_stage_ = SurveyStage::Instruments;
          survey_next_ = 0;
          continue;
        }
        formats::og::PatternWriter w;
        params.index = static_cast<std::uint16_t>(survey_next_++);
        (void)run_dry(w, *project_, sink, params, out);
        return false;
      }
      case SurveyStage::Instruments: {
        while (survey_next_ < hal::storage::save::kOgFolderLastSlot && (write_mask_ >> survey_next_ & 1u) == 0u) {
          ++survey_next_;
        }
        if (survey_next_ >= hal::storage::save::kOgFolderLastSlot) {
          survey_stage_ = SurveyStage::Done;
          return true;
        }
        formats::og::InstrumentWriter w;  // the head only (no PCM): the names cut are the head's
        params.standalone = true;
        params.index = static_cast<std::uint16_t>(survey_next_++);
        (void)run_dry(w, *project_, sink, params, out);
        return false;
      }
      case SurveyStage::Done:
        return true;
    }
  }
}

void OgExportPlan::survey(formats::og::ExportDrops& out) noexcept {
  survey_start(out);
  for (unsigned guard_steps = 0; guard_steps < 1000u && !survey_step(out); ++guard_steps) {
  }
}

// ---- One file -----------------------------------------------------------------------------------------------------------------------------------

void OgFileSource::begin(core::ByteSink& sink) noexcept {
  failure_ = "";
  if (project_ == nullptr) {
    return;
  }
  if (project_->generation() != generation_) {  // (the step fails the file: nothing is written from a revision the plan did not snapshot)
    failure_ = "CHANGED: TRY AGAIN";
    return;
  }
  formats::WriteParams params;
  params.target = formats::og::kTargetOg190;  // slice 1 writes the formats of 1.9.0 only (OS-FILES-C section 7)
  switch (kind_) {
    case OgFile::Kind::Pattern:
      params.index = id_;
      pattern_writer_.begin(*project_, sink, params);
      break;
    case OgFile::Kind::Pamd:
      pamd_writer_.begin(*project_, sink, params);
      break;
    case OgFile::Kind::Project:
      project_writer_.begin(*project_, sink, params);
      break;
    case OgFile::Kind::Instrument:  // (SaveService's PtiSaveSource makes these, not this source)
      failure_ = "COULD NOT WRITE THE FILE";
      break;
  }
}

formats::Progress OgFileSource::step(std::size_t byte_budget) noexcept {
  if (project_ == nullptr || failure_[0] != '\0') {
    return formats::Progress::Failed;
  }
  if (project_->generation() != generation_) {
    failure_ = "CHANGED: TRY AGAIN";
    return formats::Progress::Failed;
  }
  formats::Progress p = formats::Progress::Failed;
  const formats::FormatError* error = nullptr;
  switch (kind_) {
    case OgFile::Kind::Pattern:
      p = pattern_writer_.step(byte_budget);
      error = &pattern_writer_.report().error;
      break;
    case OgFile::Kind::Pamd:
      p = pamd_writer_.step(byte_budget);
      error = &pamd_writer_.report().error;
      break;
    case OgFile::Kind::Project:
      p = project_writer_.step(byte_budget);
      error = &project_writer_.report().error;
      break;
    case OgFile::Kind::Instrument:
      failure_ = "COULD NOT WRITE THE FILE";
      return formats::Progress::Failed;
  }
  if (p == formats::Progress::Failed) {
    failure_ = *error == formats::FormatError::Modified ? "CHANGED: TRY AGAIN" : "COULD NOT WRITE THE FILE";
  }
  return p;
}

std::uint32_t OgFileSource::size_hint() const noexcept {
  switch (kind_) {
    case OgFile::Kind::Pattern: return formats::og::kPatternBytes12;
    case OgFile::Kind::Pamd: return formats::og::kPamdBytes;
    case OgFile::Kind::Project: return formats::og::kProjectBytes16;
    case OgFile::Kind::Instrument: break;
  }
  return 0;
}

const formats::og::ExportDrops& OgFileSource::drops() const noexcept {
  switch (kind_) {
    case OgFile::Kind::Pattern: return pattern_writer_.export_report().drops;
    case OgFile::Kind::Pamd: return pamd_writer_.export_report().drops;
    case OgFile::Kind::Project:
    case OgFile::Kind::Instrument: break;
  }
  return project_writer_.export_report().drops;
}

}  // namespace o3os::app
