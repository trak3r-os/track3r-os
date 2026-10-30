// SPDX-License-Identifier: GPL-3.0-or-later
// The saves of the device (OS-007n): see app/save_service.h. Names are built here from the guard's own (hal/save_guard.h) and handed to AtomicSave, which is the only thing
// that talks to the writer; nothing in this file names a path the guard does not know.
#include <o3os/app/save_service.h>

#include <o3os/app/pti_load.h>
#include <o3os/app/recorder.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/sample_load_view.h>

#include <cstring>

namespace o3os::app {

namespace {

namespace guard = hal::storage::save;
using hal::storage::WriteStatus;

constexpr bool same_text(const char* a, const char* b) noexcept {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
static_assert(same_text(guard::kInstrumentsDir, kInstrumentsFolder), "the Instrument screen lists the folder SAVE writes to (sample_load_view.h)");
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the WAV source writes the samples as they are in memory: WAV is little-endian");

void put16(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) noexcept {
  put16(p, v);
  put16(p + 2, v >> 16);
}
std::uint32_t get16(const std::uint8_t* p) noexcept { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8); }
std::uint32_t get32(const std::uint8_t* p) noexcept { return get16(p) | (get16(p + 2) << 16); }

// "Projects/<name>" with a name the guard takes: the name into `out` (kNameMax + 1 bytes).
bool project_name_of(const char* folder, char* out) noexcept {
  if (folder == nullptr) {
    return false;
  }
  const std::size_t dir = sizeof guard::kProjectsDir - 1;
  for (std::size_t i = 0; i < dir; ++i) {
    if (guard::detail::lower(folder[i]) != guard::detail::lower(guard::kProjectsDir[i])) {
      return false;
    }
  }
  if (folder[dir] != '/' || !guard::valid_name(folder + dir + 1)) {
    return false;
  }
  std::strcpy(out, folder + dir + 1);
  return true;
}

bool same_ci(const char* a, const char* b) noexcept {
  for (;; ++a, ++b) {
    if (guard::detail::lower(*a) != guard::detail::lower(*b)) {
      return false;
    }
    if (*a == '\0') {
      return true;
    }
  }
}

// An instrument's sample path as a file of the project's samples folder: "samples/<stem>.wav" with a stem the guard takes (any case); the file's name ("instr01.wav") into `out`
// (kNameMax + 5 bytes). Anything else (a path outside samples/, a name the guard refuses) is not written.
bool sample_file_of(const char* path, char* out) noexcept {
  const std::size_t dir = sizeof guard::kProjectSamplesDir - 1;
  if (path == nullptr) {
    return false;
  }
  for (std::size_t i = 0; i < dir; ++i) {
    if (guard::detail::lower(path[i]) != guard::detail::lower(guard::kProjectSamplesDir[i])) {
      return false;
    }
  }
  const char* file = path + dir;
  if (file[0] != '/') {
    return false;
  }
  ++file;
  const std::size_t n = std::strlen(file);
  const std::size_t suffix = sizeof guard::kWavSuffix - 1;
  if (n <= suffix || n > guard::kNameMax + suffix || !guard::detail::same(file + n - suffix, suffix, guard::kWavSuffix) || !guard::detail::valid_name(file, n - suffix)) {
    return false;
  }
  std::memcpy(out, file, n + 1u);
  return true;
}

void upper_label(char* out, std::size_t cap, const char* text) noexcept {
  std::size_t n = 0;
  for (; text[n] != '\0' && n + 1 < cap; ++n) {
    const char c = text[n];
    out[n] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  out[n] = '\0';
}

}  // namespace

bool stereo_side_path(const model::Project& project, unsigned slot, const char* sample_path, char* out, std::size_t cap) noexcept {
  constexpr char kSuffix[] = "_s.wav";
  if (sample_path == nullptr || out == nullptr) {
    return false;
  }
  const std::size_t dir = sizeof guard::kProjectSamplesDir - 1u;
  const std::size_t ext = sizeof guard::kWavSuffix - 1u;
  const std::size_t n = std::strlen(sample_path);
  if (n <= dir + 1u + ext || sample_path[dir] != '/' || !guard::detail::same(sample_path, dir, guard::kProjectSamplesDir) || !guard::detail::same(sample_path + n - ext, ext, guard::kWavSuffix)) {
    return false;
  }
  const std::size_t stem = n - ext;          // "samples/<name>"
  const std::size_t name = stem - dir - 1u;  // <name>
  if (name + 2u > guard::kNameMax || stem + sizeof kSuffix > cap) {
    return false;
  }
  std::memcpy(out, sample_path, stem);
  std::memcpy(out + stem, kSuffix, sizeof kSuffix);
  if (!guard::detail::valid_name(out + dir + 1u, name + 2u)) {
    return false;
  }
  for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* other = project.instrument(i);
    if (i != slot && other != nullptr && same_ci(other->sample_path, out)) {
      return false;
    }
  }
  return true;
}

// ---- Sources -----------------------------------------------------------------------------------------------------------------------------------

void NativeSaveSource::begin(core::ByteSink& sink) noexcept {
  if (project_ != nullptr) {
    writer_.begin(*project_, sink, stereo_mask_);
  }
}

formats::Progress NativeSaveSource::step(std::size_t byte_budget) noexcept {
  return project_ != nullptr ? writer_.step(byte_budget) : formats::Progress::Failed;
}

const char* NativeSaveSource::failure() const noexcept {
  return writer_.report().error == formats::native::Error::Modified ? "CHANGED: TRY AGAIN" : "";
}

void WavSaveSource::make_header(std::uint8_t* out, std::uint32_t frames, std::uint8_t channels, std::uint32_t rate) noexcept {
  const std::uint32_t data = frames * channels * 2u;
  std::memcpy(out, "RIFF", 4);
  put32(out + 4, 36u + data);
  std::memcpy(out + 8, "WAVEfmt ", 8);
  put32(out + 16, 16);
  put16(out + 20, 1);  // PCM
  put16(out + 22, channels);
  put32(out + 24, rate);
  put32(out + 28, rate * channels * 2u);
  put16(out + 32, channels * 2u);
  put16(out + 34, 16);
  std::memcpy(out + 36, "data", 4);
  put32(out + 40, data);
}

bool WavSaveSource::set(const std::int16_t* pcm, std::uint32_t frames, std::uint8_t channels, std::uint32_t rate, Check check, const void* ctx) noexcept {
  const std::uint64_t data = static_cast<std::uint64_t>(frames) * channels * 2u;
  if (pcm == nullptr || frames == 0u || channels < 1u || channels > 2u || rate == 0u || data + 36u > 0xFFFFFFFFu || static_cast<std::uint64_t>(rate) * channels * 2u > 0xFFFFFFFFu) {
    return false;
  }
  pcm_ = pcm;
  frames_ = frames;
  channels_ = channels;
  rate_ = rate;
  data_bytes_ = data;
  check_ = check;
  ctx_ = ctx;
  failure_ = "";
  return true;
}

void WavSaveSource::begin(core::ByteSink& sink) noexcept {
  sink_ = &sink;
  pos_ = 0;
  make_header(header_, frames_, channels_, rate_);
}

formats::Progress WavSaveSource::step(std::size_t byte_budget) noexcept {
  if (check_ != nullptr && !check_(ctx_)) {
    failure_ = "CHANGED WHILE SAVING";
    return formats::Progress::Failed;
  }
  const std::uint64_t total = kHeaderBytes + data_bytes_;
  while (byte_budget != 0u && pos_ < total) {
    const std::uint8_t* src;
    std::size_t n;
    if (pos_ < kHeaderBytes) {
      src = header_ + pos_;
      n = kHeaderBytes - static_cast<std::size_t>(pos_);
    } else {
      src = reinterpret_cast<const std::uint8_t*>(pcm_) + (pos_ - kHeaderBytes);
      n = static_cast<std::size_t>(total - pos_);
    }
    n = n < byte_budget ? n : byte_budget;
    const core::IoResult r = sink_->write(src, n);
    if (r.status != core::IoStatus::Ok || r.count == 0u || r.count > n) {
      return formats::Progress::Failed;
    }
    pos_ += r.count;
    byte_budget -= r.count;
  }
  return pos_ >= total ? formats::Progress::Done : formats::Progress::Running;
}

// ---- The held check (OS-BUG-OGSAVE) ---------------------------------------------------------------------------------------------------------------
// A small chunk walker over the bytes of a file, in order: RIFF/WAVE, then chunks; 'fmt ' must be the PCM format this source writes, other chunks before 'data' are skipped, and the
// data chunk must hold this audio's whole frames (and less than one more frame) and begin with them, byte for byte. Everything else is Failed: a caller that gets that writes as it
// would without the check.
static_assert(WavSaveSource::kHeaderBytes >= 16, "the held check collects a 16-byte 'fmt ' head in header_");

bool WavSaveSource::start(std::uint32_t file_size) noexcept {
  if (pcm_ == nullptr || file_size < kHeaderBytes + data_bytes_) {
    return false;  // (a WAV with these frames is at least this long: 12 + 8 + 16 + 8 bytes of head, then the data)
  }
  held_ = Held::Riff;
  held_fmt_ = false;
  held_have_ = 0;
  held_need_ = 12;
  held_left_ = 0;
  held_off_ = 0;
  held_pos_ = 0;
  return true;
}

// The 16 bytes of a 'fmt ' chunk in header_: PCM, this source's channels and rate, 16 bits, its frame size (the byte rate is derived, not compared).
bool WavSaveSource::fmt_ok() const noexcept {
  return get16(header_) == 1u && get16(header_ + 2) == channels_ && get32(header_ + 4) == rate_ && get16(header_ + 12) == channels_ * 2u && get16(header_ + 14) == 16u;
}

formats::Progress WavSaveSource::feed(const std::uint8_t* data, std::size_t n) noexcept {
  if (check_ != nullptr && !check_(ctx_)) {
    return formats::Progress::Failed;  // the audio moved on: the file cannot be said to hold it
  }
  while (n != 0u) {
    if (held_ != Held::Data && held_off_ > kHeldHeaderMax) {
      return formats::Progress::Failed;  // a head that goes on and on
    }
    if (held_ == Held::Data) {
      const std::size_t take = n < data_bytes_ - held_pos_ ? n : static_cast<std::size_t>(data_bytes_ - held_pos_);
      if (std::memcmp(data, reinterpret_cast<const std::uint8_t*>(pcm_) + held_pos_, take) != 0) {
        return formats::Progress::Failed;
      }
      held_pos_ += static_cast<std::uint32_t>(take);
      if (held_pos_ == data_bytes_) {
        return formats::Progress::Done;  // the whole frames are there; a stray half frame, the rest of the file, are not read
      }
      data += take;
      n -= take;
    } else if (held_ == Held::Skip) {
      const std::size_t take = n < held_left_ ? n : held_left_;
      held_left_ -= static_cast<std::uint32_t>(take);
      held_off_ += static_cast<std::uint32_t>(take);
      data += take;
      n -= take;
      if (held_left_ == 0u) {
        held_ = Held::ChunkHead;
        held_need_ = 8;
      }
    } else {  // Riff, ChunkHead, Fmt: collect the head into header_
      const std::size_t take = n < static_cast<std::size_t>(held_need_ - held_have_) ? n : static_cast<std::size_t>(held_need_ - held_have_);
      std::memcpy(header_ + held_have_, data, take);
      held_have_ = static_cast<std::uint8_t>(held_have_ + take);
      held_off_ += static_cast<std::uint32_t>(take);
      data += take;
      n -= take;
      if (held_have_ < held_need_) {
        continue;
      }
      held_have_ = 0;
      if (held_ == Held::Riff) {
        if (std::memcmp(header_, "RIFF", 4) != 0 || std::memcmp(header_ + 8, "WAVE", 4) != 0) {
          return formats::Progress::Failed;
        }
        held_ = Held::ChunkHead;
        held_need_ = 8;
      } else if (held_ == Held::ChunkHead) {
        const std::uint32_t size = get32(header_ + 4);
        if (std::memcmp(header_, "data", 4) == 0) {
          // the whole frames of this audio, and less than one frame more (a stray half frame, an odd chunk with no pad byte)
          if (!held_fmt_ || size < data_bytes_ || size - data_bytes_ >= channels_ * 2u) {
            return formats::Progress::Failed;
          }
          held_ = Held::Data;
        } else if (std::memcmp(header_, "fmt ", 4) == 0) {
          if (held_fmt_ || size < 16u || size > kHeldHeaderMax) {
            return formats::Progress::Failed;
          }
          held_ = Held::Fmt;
          held_need_ = 16;
          held_left_ = size - 16u + (size & 1u);  // (the rest of the chunk, and its pad byte, are skipped after the 16 bytes)
        } else {
          if (size > kHeldHeaderMax) {
            return formats::Progress::Failed;
          }
          held_ = Held::Skip;
          held_left_ = size + (size & 1u);  // (an odd chunk is followed by one pad byte)
          if (held_left_ == 0u) {
            held_ = Held::ChunkHead;
          }
        }
      } else {  // Fmt
        if (!fmt_ok()) {
          return formats::Progress::Failed;
        }
        held_fmt_ = true;
        held_ = held_left_ != 0u ? Held::Skip : Held::ChunkHead;
        held_need_ = 8;
      }
    }
  }
  return formats::Progress::Running;
}

core::IoResult PtiSaveSource::Samples::read(void* dst, std::size_t max) noexcept {
  if (pos_ >= size_) {
    return {0, core::IoStatus::End};
  }
  const std::size_t n = size_ - pos_ < max ? size_ - pos_ : max;
  std::memcpy(dst, bytes_ + pos_, n);
  pos_ += n;
  return {n, core::IoStatus::Ok};
}

bool PtiSaveSource::set(const model::Project& project, unsigned slot, const std::int16_t* pcm, std::uint32_t frames, Check check, const void* ctx, std::int16_t level) noexcept {
  const model::Instrument* in = project.instrument(slot);
  if (in == nullptr || in->kind != model::InstrumentKind::Sample || pcm == nullptr || frames == 0u || frames > formats::og::kMaxPcmFrames) {
    return false;
  }
  project_ = &project;
  slot_ = slot;
  pcm_ = pcm;
  frames_ = frames;
  level_ = level;
  fine_ = formats::WriteParams::kNoFineOverride;
  resampling_ = false;
  check_ = check;
  ctx_ = ctx;
  failure_ = "";
  return true;
}
void PtiSaveSource::set_resample(const OgPcmPlan& plan, AmigaKernel& kernel, std::int16_t fine) noexcept {
  fine_ = fine;
  resampling_ = !plan.identity && plan.status == OgPcmStatus::Ok && plan.step_q32 != 0u;
  plan_ = plan;
  kernel_ = &kernel;
}

void PtiSaveSource::begin(core::ByteSink& sink) noexcept {
  formats::WriteParams params;
  params.index = static_cast<std::uint16_t>(slot_);
  if (resampling_) {
    resampler_.set(*kernel_, pcm_, frames_, plan_.step_q32, plan_.frames_out, og_loop_of(*project_->instrument(slot_), frames_));
    params.pcm = &resampler_;
    params.pcm_frames = plan_.frames_out;
  } else {
    samples_.set(pcm_, frames_);
    params.pcm = &samples_;
    params.pcm_frames = frames_;
  }
  params.standalone = true;
  params.level_override = level_;
  params.fine_override = fine_;
  writer_.begin(*project_, sink, params);
}

formats::Progress PtiSaveSource::step(std::size_t byte_budget) noexcept {
  if (project_ == nullptr) {
    return formats::Progress::Failed;
  }
  if (check_ != nullptr && !check_(ctx_)) {
    failure_ = "CHANGED WHILE SAVING";
    return formats::Progress::Failed;
  }
  if (resampling_) {
    resampler_.arm();  // one slice of the resampler a step
  }
  const formats::Progress p = writer_.step(byte_budget);
  if (p == formats::Progress::Failed) {
    failure_ = writer_.report().error == formats::FormatError::Modified ? "CHANGED WHILE SAVING" : "COULD NOT WRITE THE INSTRUMENT";
  }
  return p;
}

formats::Progress BufferSaveSource::step(std::size_t byte_budget) noexcept {
  while (byte_budget != 0u && pos_ < size_) {
    const std::size_t n = size_ - pos_ < byte_budget ? size_ - pos_ : byte_budget;
    const core::IoResult r = sink_->write(data_ + pos_, n);
    if (r.status != core::IoStatus::Ok || r.count == 0u || r.count > n) {
      return formats::Progress::Failed;
    }
    pos_ += r.count;
    byte_budget -= r.count;
  }
  return pos_ >= size_ ? formats::Progress::Done : formats::Progress::Running;
}

// ---- The service -------------------------------------------------------------------------------------------------------------------------------

const char* SaveService::gate_text(WriteStatus status) noexcept {
  switch (status) {
    case WriteStatus::Ok: return "";
    case WriteStatus::NoCard: return "NO CARD";
    case WriteStatus::WriteProtected: return "CARD IS WRITE PROTECTED";
    case WriteStatus::Unsupported: return "EXFAT CARD: SAVING NEEDS FAT32";
    case WriteStatus::Host: return "CARD ON COMPUTER - EJECT FIRST";  // OS-USBDISK 4.3
    default: break;
  }
  return "CARD CANNOT BE WRITTEN";
}

bool SaveService::watch_ok(const void* ctx) noexcept {
  const Watch& w = *static_cast<const Watch*>(ctx);
  if (w.bank != nullptr) {
    const SampleBank::Pcm now = w.bank->pcm(w.slot);
    return w.bank->version(w.slot) == w.version && now.data == w.pcm && now.frames == w.frames;
  }
  if (w.take != nullptr) {
    return w.take->state() == Recorder::State::Review && w.take->take_data() + w.take->trim_start() == w.pcm && w.take->trim_end() - w.take->trim_start() == w.frames;
  }
  return true;
}

// The first name of prefix + a number (`digits` wide) + suffix that is free in `dir`, trying `cursor` .. `last` and then `first` .. cursor - 1. At most `last - first + 1`
// probes of the card (a directory lookup each), the first one almost always the answer.
bool SaveService::free_name(char* out, std::size_t cap, const char* dir, const char* prefix, const char* suffix, unsigned digits, unsigned first, unsigned last, unsigned& cursor) noexcept {
  const unsigned span = last - first + 1u;
  unsigned n = cursor < first || cursor > last ? first : cursor;
  for (unsigned tried = 0; tried < span; ++tried, n = n == last ? first : n + 1u) {
    char name[guard::kNameMax + 8];
    std::size_t len = 0;
    for (const char* p = prefix; *p != '\0'; ++p) {
      name[len++] = *p;
    }
    for (unsigned d = digits, scale = 1; d != 0; --d) {
      scale = 1;
      for (unsigned k = 1; k < d; ++k) {
        scale *= 10u;
      }
      name[len++] = static_cast<char>('0' + (n / scale) % 10u);
    }
    for (const char* p = suffix; *p != '\0'; ++p) {
      name[len++] = *p;
    }
    name[len] = '\0';
    char path[guard::kPathCap];
    if (len >= cap || !guard::join(path, sizeof path, dir, name)) {
      return false;
    }
    if (card_->exists(path, nullptr) == hal::storage::Status::NotFound) {
      std::memcpy(out, name, len + 1u);
      cursor = n;
      return true;
    }
  }
  return false;
}

SaveService::Start SaveService::launch(core::JobRunner& jobs, const char* dir, const char* name, AtomicSave::Mode mode, SaveSource& source, const char* label, Kind kind, bool quiet,
                                       const char* verb, HeldCheck* held) noexcept {
  yield_recent();  // (the caller has passed its gates: this job really starts, so what steps aside for it does now)
  char text[24];
  upper_label(text, sizeof text, label);
  jobs_ = &jobs;
  silent_slot_ = -1;
  const bool in_plan = plan_.active || stems_.active || og_.active;  // (a set of stems or of OG files keeps its card like a project's samples do)
  if (!in_plan || !plan_.pinned) {  // a save begins: its card is the one it ends on, whichever job of it finds another (OS-FU5)
    writer_->begin_save();
    plan_.pinned = in_plan;
  }
  if (!job_.begin(*card_, *writer_, dir, name, mode, source, text, verb)) {
    if (!in_plan) {
      end_save();
    }
    return Start::NoName;  // a path the guard does not take: never from the names built here
  }
  job_.set_held(held);
  const int slot = jobs.start(job_);
  if (slot < 0) {
    job_.cancel();
    if (!in_plan) {
      end_save();  // (a plan's next pass tries again and keeps its card)
    }
    return Start::NoJobSlot;
  }
  kind_ = kind;
  quiet_slot_ = quiet ? slot : -1;
  slot_ = slot;
  guard::join(last_, sizeof last_, dir, name);
  return Start::Started;
}

SaveService::Start SaveService::save_project(core::JobRunner& jobs, const model::Project& project, const char* loaded_folder, bool as_new, const char* as_name, const SampleBank* bank) noexcept {
  if (writer_ == nullptr || card_ == nullptr) {
    return Start::NoWriter;
  }
  reconcile();
  if (user_blocked()) {
    return Start::Busy;
  }
  if (claim_ != nullptr && claim_()) {
    return Start::CardInUse;
  }
  if (writer_->gate() != WriteStatus::Ok) {
    return Start::NotWritable;
  }
  save_epoch_ = ~remount_epoch_;  // (OS-BUG-REMOUNT) this save has checked no sample yet (never equal to remount_epoch_): a value left by an earlier save that failed must not make the beliefs current at this one's end
  side_ = SidePlan{};             // OS-STEREO (b): what this save owes the side files (samples_to_write fills it when there is a bank)
  mono_count_ = 0;
  mono_name_[0] = '\0';
  const char* loaded = loaded_folder != nullptr ? loaded_folder : "";
  char name[guard::kNameMax + 1] = {};
  bool replace = false;
  const bool typed = as_new && as_name != nullptr;
  if (typed) {
    char path[guard::kPathCap];
    if (!guard::valid_name(as_name) || !guard::join(path, sizeof path, guard::kProjectsDir, as_name)) {
      return Start::BadName;
    }
    if (card_->exists(path, nullptr) != hal::storage::Status::NotFound) {
      return Start::NameTaken;  // a folder (or anything) of that name: Save As never replaces
    }
    std::strcpy(name, as_name);  // (valid_name bounds it by kNameMax)
  }
  if (!as_new) {
    if (current_[0] != '\0' && std::strcmp(anchor_, loaded) == 0) {
      std::strcpy(name, current_);  // the project the user saved last, while it is still the one they are in
      replace = true;
    } else if (project_name_of(loaded, name)) {
      replace = true;  // the folder it was loaded from
    }
  }
  if (!replace && !typed && !free_name(name, sizeof name, guard::kProjectsDir, "Track ", "", 2, 1, kMaxTrackNumber, next_track_)) {
    return Start::NoName;
  }
  char dir[kFolderChars + 1];
  if (!guard::join(dir, sizeof dir, guard::kProjectsDir, name)) {
    return Start::NoName;
  }
  if (bank != nullptr && bank->ready()) {  // the samples the folder does not hold yet go first, one job each (SAMPLES in save_service.h); the project file is the plan's last job
    char loaded_name[guard::kNameMax + 1] = {};
    const bool from_folder = !as_new && project_name_of(loaded, loaded_name) && same_ci(loaded_name, name);
    forget_other_card();
    save_epoch_ = remount_epoch_;  // the samples are checked below, against the card as it is now
    const std::uint64_t todo = samples_to_write(project, *bank, name, from_folder, side_);
    for (unsigned slot = 0; slot < model::kMaxInstruments; ++slot) {
      if (((side_.refused >> slot) & 1u) != 0u) {
        note_mono(project, slot);
      }
    }
    if (todo != 0u || side_.todo != 0u) {
      plan_ = Plan{};
      plan_.active = true;
      plan_.bank = bank;
      plan_.project = &project;
      plan_.todo = todo;
      plan_.replace = replace;
      plan_.from_folder = from_folder;
      std::strcpy(plan_.name, name);
      std::strncpy(plan_.anchor, loaded, sizeof plan_.anchor - 1);
      pending_generation_ = project.generation();
      const Start first = advance_plan(jobs);
      if (first == Start::Started) {
        if (!replace && !typed) {
          ++next_track_;
        }
        return Start::Started;
      }
      plan_.active = false;
      end_save();
      return first;
    }
  }
  native_.set(project, side_.ok);  // (no bank: side_ is the default, every flag goes as it is)
  pending_generation_ = project.generation();
  const Start started = launch(jobs, dir, guard::kProjectFile, replace ? AtomicSave::Mode::Replace : AtomicSave::Mode::NewOnly, native_, name, Kind::Project, false);
  if (started == Start::Started) {
    proj_job_ = slot_;
    std::strcpy(pending_name_, name);
    std::strncpy(pending_anchor_, loaded, sizeof pending_anchor_ - 1);
    pending_anchor_[sizeof pending_anchor_ - 1] = '\0';
    if (!replace && !typed) {
      ++next_track_;
    }
    guard::join(last_, sizeof last_, dir);  // the message names the folder
  }
  return started;
}

// The slots whose audio Projects/<name>/samples does not hold yet: every sample instrument that has audio in the bank and a path the guard takes, except one that an earlier save
// wrote into that folder at this version of the audio, or (`read_from_folder`: the project came from this very folder) one the loader read from it and that has not changed since.
std::uint64_t SaveService::samples_to_write(const model::Project& project, const SampleBank& bank, const char* name, bool read_from_folder, SidePlan& side) const noexcept {
  std::uint64_t todo = 0;
  side = SidePlan{};
  const bool known_here = same_ci(known_dir_, name);
  for (unsigned slot = 0; slot < 64u; ++slot) {
    const model::Instrument* in = project.instrument(slot);
    if (in != nullptr && in->stereo != 0u) {
      side.ok &= ~(std::uint64_t{1} << slot);  // (rule 4: a flagged slot keeps its flag only where its side file is held or written, set below)
    }
    if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
      continue;
    }
    const SampleBank::Pcm pcm = bank.pcm(slot);
    char file[guard::kNameMax + 8];
    if (pcm.data == nullptr || pcm.frames == 0u || !sample_file_of(in->sample_path, file)) {
      continue;
    }
    const std::uint32_t version = bank.version(slot);
    const bool saved_here = known_here && known_[slot] == version;
    const bool loaded_here = read_from_folder && bank.card_version(slot) == version;
    bool held = saved_here || loaded_here;
    if (held && revalidate_) {
      // The card was out and back since the belief was formed: the file has to look as it did (size and stamp; a belief with nothing recorded cannot be confirmed).
      char dir[guard::kPathCap];
      char path[guard::kPathCap];
      hal::storage::FileStamp now;
      held = guard::join(dir, sizeof dir, guard::kProjectsDir, name, guard::kProjectSamplesDir) && guard::join(path, sizeof path, dir, file) && card_ != nullptr &&
             card_->stat(path, now) == hal::storage::Status::Ok;
      if (held) {
        const hal::storage::FileStamp then = saved_here ? known_file_[slot] : hal::storage::FileStamp{bank.card_size(slot), bank.card_stamp(slot)};
        held = (then.size != 0u || then.stamp != 0u) && now == then;
      }
    }
    if (!held) {
      todo |= std::uint64_t{1} << slot;
    }
    // OS-STEREO (b), rule 8: a stereo instrument (flag set, side signal in the bank) also owes its S file, unless the folder holds it (written at this version by an earlier save, or read from the folder with the mid). After a remount nothing
    // is believed about it (no stamp is kept for S): it is written again, once.
    char side_path[model::kSamplePathMax + 8];
    if (in->stereo != 0u && pcm.side != nullptr) {  // (an instrument set back to MONO keeps no side file: a reopen would not read it)
      if (stereo_side_path(project, slot, in->sample_path, side_path, sizeof side_path)) {
        const bool held_side = !revalidate_ && ((known_here && known_side_[slot] == version) || loaded_here);
        if (!held_side) {
          side.todo |= std::uint64_t{1} << slot;
        }
        side.ok |= std::uint64_t{1} << slot;  // (held, or written before the project file: advance_plan takes it back if that fails)
      } else {
        side.refused |= std::uint64_t{1} << slot;  // a name that collides or does not fit: saved mono, never silently
      }
    }
  }
  return todo;
}

// One sample of the plan as a job: the bank's audio as a 16-bit mono WAV into Projects/<name>/samples, the file the instrument names, a new file or, over one, a replacing save
// (the old one stays as <file>.bak). TooBig: this one cannot be a WAV file; the plan skips it.
// OS-STEREO (b): `side` writes the instrument's side signal instead, as <stem>_s.wav beside the mid (stereo_side_path: the one naming rule), the same way; Empty when the instrument has no side signal or no side name.
SaveService::Start SaveService::start_sample(core::JobRunner& jobs, unsigned slot, bool side) noexcept {
  const model::Instrument* in = plan_.project->instrument(slot);
  const SampleBank::Pcm pcm = plan_.bank->pcm(slot);
  char file[guard::kNameMax + 8];
  if (in == nullptr || !sample_file_of(in->sample_path, file) || pcm.data == nullptr || pcm.frames == 0u) {
    return Start::Empty;
  }
  const std::int16_t* audio = pcm.data;
  if (side) {
    char side_path[model::kSamplePathMax + 8];
    if (in->stereo == 0u || pcm.side == nullptr || !stereo_side_path(*plan_.project, slot, in->sample_path, side_path, sizeof side_path)) {
      return Start::Empty;
    }
    audio = pcm.side;
    std::strcpy(file, side_path + sizeof guard::kProjectSamplesDir);  // the name after "samples/" (the guard bounds it: stereo_side_path)
  }
  watch_ = Watch{plan_.bank, slot, plan_.bank->version(slot), nullptr, pcm.data, pcm.frames};  // (the slot's version and block say it is still this audio, side signal too: they move together)
  if (!wav_.set(audio, pcm.frames, 1, in->sample_rate != 0u ? in->sample_rate : 44100u, &SaveService::watch_ok, &watch_)) {
    return Start::TooBig;
  }
  char dir[guard::kPathCap];
  char path[guard::kPathCap];
  if (!guard::join(dir, sizeof dir, guard::kProjectsDir, plan_.name, guard::kProjectSamplesDir) || !guard::join(path, sizeof path, dir, file)) {
    return Start::NoName;
  }
  const bool exists = card_->exists(path, nullptr) == hal::storage::Status::Ok;
  // OS-BUG-OGSAVE: a file that is there, for audio that nothing says the folder holds (the project came from this folder, the slot was not read from it and no earlier save wrote it: an OG project's .pti), may already hold
  // this very audio in another form (the official firmware's WAV beside its .pti): the job reads it first and leaves it alone when it does. An audio the folder held before and that has
  // changed since is not asked: the file is the old one.
  const bool unknown = !side && plan_.from_folder && plan_.bank->card_version(slot) == 0u && !(same_ci(known_dir_, plan_.name) && known_[slot] != 0u);  // (a side file was never read by the official firmware: nothing to ask)
  char label[guard::kNameMax + 8];
  std::memcpy(label, file, std::strlen(file) - (sizeof guard::kWavSuffix - 1));  // the file without ".wav": the strip's label
  label[std::strlen(file) - (sizeof guard::kWavSuffix - 1)] = '\0';
  const Start started = launch(jobs, dir, file, exists ? AtomicSave::Mode::Replace : AtomicSave::Mode::NewOnly, wav_, label, Kind::Sample, false, "SAVE ", exists && unknown ? &wav_ : nullptr);
  if (started == Start::Started) {
    smp_slot_ = slot;
    smp_side_ = side;
    smp_version_ = watch_.version;
    smp_card_ = writer_->card_id();  // (the job holds its card: it fails when another is mounted)
    std::strncpy(smp_path_, path, sizeof smp_path_ - 1u);
    smp_path_[sizeof smp_path_ - 1u] = '\0';
  }
  return started;
}

void SaveService::note_mono(const model::Project& project, unsigned slot) noexcept {
  const model::Instrument* in = project.instrument(slot);
  if (mono_count_++ == 0u && in != nullptr) {
    upper_label(mono_name_, sizeof mono_name_, in->name[0] != '\0' ? in->name : "INSTRUMENT");
  }
}

// The next job of a plan: the next sample, or, when there is none left, the project file. Started: a job runs (or, from poll, nothing is left to do); anything else that is not
// NoJobSlot ends the plan.
SaveService::Start SaveService::advance_plan(core::JobRunner& jobs) noexcept {
  if (!plan_.active || (!yieldable() && (job_.running() || kind_ != Kind::None))) {
    return Start::Busy;
  }
  if ((claim_ != nullptr && claim_()) || writer_->gate() != WriteStatus::Ok) {
    plan_.active = false;  // the firmware update path took the card, or the card went read-only: the save is dropped, the project file is not written
    end_save();
    return Start::CardInUse;
  }
  yield_recent();  // (the plan goes ahead: what steps aside for the user does now)
  while (plan_.todo != 0u) {
    unsigned slot = 0;
    while (((plan_.todo >> slot) & 1u) == 0u) {
      ++slot;
    }
    const Start s = start_sample(jobs, slot, false);
    if (s == Start::Started) {
      plan_.todo &= ~(std::uint64_t{1} << slot);
      return Start::Started;
    }
    if (s == Start::NoJobSlot) {
      return s;  // a slot of the runner is not free now: the next pass tries again
    }
    plan_.todo &= ~(std::uint64_t{1} << slot);  // TooBig, Empty: this sample is skipped
  }
  while (side_.todo != 0u) {  // OS-STEREO (b), rule 8: every mid is down, then the side files, then the project file (a failed side file ends the plan like a failed sample)
    unsigned slot = 0;
    while (((side_.todo >> slot) & 1u) == 0u) {
      ++slot;
    }
    const Start s = start_sample(jobs, slot, true);
    if (s == Start::Started) {
      side_.todo &= ~(std::uint64_t{1} << slot);
      return Start::Started;
    }
    if (s == Start::NoJobSlot) {
      return s;
    }
    side_.todo &= ~(std::uint64_t{1} << slot);
    side_.ok &= ~(std::uint64_t{1} << slot);  // Empty, TooBig, NoName: no side file of this save (nor held): the flag is not written (rule 4), and the strip says so
    note_mono(*plan_.project, slot);
  }
  char dir[kFolderChars + 1];
  if (!guard::join(dir, sizeof dir, guard::kProjectsDir, plan_.name)) {
    plan_.active = false;
    end_save();
    return Start::NoName;
  }
  native_.set(*plan_.project, side_.ok);  // (what the plan wrote or found held, not the bank as it is now: an undo in between must not make a flag without its file)
  pending_generation_ = plan_.project->generation();
  const Start started = launch(jobs, dir, guard::kProjectFile, plan_.replace ? AtomicSave::Mode::Replace : AtomicSave::Mode::NewOnly, native_, plan_.name, Kind::Project, false);
  if (started == Start::Started) {
    proj_job_ = slot_;
    std::strcpy(pending_name_, plan_.name);
    std::strcpy(pending_anchor_, plan_.anchor);
    guard::join(last_, sizeof last_, dir);
    plan_.active = false;
  } else if (started != Start::NoJobSlot) {
    plan_.active = false;
    end_save();
  }
  return started;
}

bool SaveService::suggest_name(char* out, std::size_t cap) noexcept {
  if (card_ == nullptr || out == nullptr) {
    return false;
  }
  unsigned cursor = next_track_;
  return free_name(out, cap, guard::kProjectsDir, "Track ", "", 2, 1, kMaxTrackNumber, cursor);  // (the cursor is a copy: asking does not use the number up)
}

SaveService::Start SaveService::save_wav(core::JobRunner& jobs, const WavRequest& rq) noexcept {
  if (writer_ == nullptr || card_ == nullptr) {
    return Start::NoWriter;
  }
  reconcile();
  if (user_blocked()) {  // (a project plan's next job must not find a WAV running)
    return Start::Busy;
  }
  if (claim_ != nullptr && claim_()) {
    return Start::CardInUse;
  }
  if (rq.pcm == nullptr || rq.frames == 0u) {
    return Start::Empty;
  }
  if (writer_->gate() != WriteStatus::Ok) {
    return Start::NotWritable;
  }
  watch_ = Watch{rq.bank, rq.slot, rq.bank != nullptr ? rq.bank->version(rq.slot) : 0u, rq.take, rq.pcm, rq.frames};
  wav_take_done_ = false;
  if (!wav_.set(rq.pcm, rq.frames, rq.channels, rq.rate, &SaveService::watch_ok, &watch_)) {
    return Start::TooBig;
  }
  const bool smp = rq.prefix != nullptr && std::strcmp(rq.prefix, "SMP") == 0;
  unsigned& cursor = smp ? next_smp_ : next_rec_;
  char prefix[8];
  std::strcpy(prefix, smp ? "SMP_" : "REC_");
  char file[guard::kNameMax + 8];
  if (!free_name(file, sizeof file, guard::kSamplesDir, prefix, guard::kWavSuffix, 3, 1, kMaxWavNumber, cursor)) {
    return Start::NoName;
  }
  char label[guard::kNameMax + 8];
  const std::size_t stem = std::strlen(file) - (sizeof guard::kWavSuffix - 1);  // the file name without ".wav": the strip's label
  std::memcpy(label, file, stem);
  label[stem] = '\0';
  const Start started = launch(jobs, guard::kSamplesDir, file, AtomicSave::Mode::NewOnly, wav_, label, Kind::Wav, false);
  if (started == Start::Started) {
    ++cursor;
  }
  return started;
}

bool SaveService::take_written(const Recorder& take) noexcept {
  reconcile();
  if (!wav_take_done_ || watch_.take != &take) {
    return false;
  }
  wav_take_done_ = false;
  return watch_ok(&watch_);  // (the take is still the one that was written)
}

// The sanitised stem of an instrument's file (PTI in save_service.h). The first byte that cannot start a name is dropped, a byte the guard does not take becomes '_', a second '.' in a row too,
// a trailing space or dot goes, and a name the guard still refuses (CON, NUL, COM1 ...) gets a '_' (CON_ is a name). Nothing is left: "Instrument NN".
void SaveService::instrument_file_stem(const char* name, unsigned slot, char* out, std::size_t cap) noexcept {
  const std::size_t room = cap > 0u ? (cap - 1u < guard::kNameMax ? cap - 1u : guard::kNameMax) : 0u;
  std::size_t n = 0;
  for (std::size_t i = 0; name != nullptr && name[i] != '\0' && i < model::kInstrumentNameMax && n < room; ++i) {
    char c = name[i];
    const bool word = guard::detail::alnum(c);
    if (!word && c != ' ' && c != '_' && c != '-' && c != '.') {
      c = '_';  // a path separator, a character FAT refuses, a control byte, UTF-8
    }
    if (n == 0u && !guard::detail::alnum(c)) {
      continue;  // (a name starts with a letter or a digit)
    }
    if (c == '.' && n != 0u && out[n - 1u] == '.') {
      c = '_';
    }
    out[n++] = c;
  }
  while (n != 0u && (out[n - 1u] == ' ' || out[n - 1u] == '.')) {
    --n;
  }
  if (n != 0u && n < room && !guard::detail::valid_name(out, n)) {
    out[n++] = '_';
  }
  if (cap == 0u) {
    return;
  }
  out[n] = '\0';
  if (n == 0u || !guard::valid_name(out)) {
    const unsigned number = slot + 1u;
    const char text[] = "Instrument ";
    std::size_t at = 0;
    for (; text[at] != '\0' && at + 3u < cap; ++at) {
      out[at] = text[at];
    }
    if (at + 3u <= cap) {
      out[at++] = static_cast<char>('0' + (number / 10u) % 10u);
      out[at++] = static_cast<char>('0' + number % 10u);
    }
    out[at] = '\0';
  }
}

SaveService::Start SaveService::save_instrument(core::JobRunner& jobs, const model::Project& project, const SampleBank& bank, unsigned slot) noexcept {
  pti_hz_ = 0u;  // set only by a save that starts: a refusal must not leave the last save's rate behind
  if (writer_ == nullptr || card_ == nullptr) {
    return Start::NoWriter;
  }
  reconcile();
  if (user_blocked()) {
    return Start::Busy;
  }
  if (claim_ != nullptr && claim_()) {
    return Start::CardInUse;
  }
  const model::Instrument* in = slot < 64u ? project.instrument(slot) : nullptr;
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return Start::NotSample;
  }
  const SampleBank::Pcm pcm = bank.pcm(slot);
  if (pcm.data == nullptr || pcm.frames == 0u) {
    return Start::Empty;
  }
  if (pcm.frames > formats::og::kMaxPcmFrames) {
    return Start::TooBig;
  }
  if (writer_->gate() != WriteStatus::Ok) {
    return Start::NotWritable;
  }
  // A .pti has no rate and its level is read through the 1.2+ law (docs/specs/OS-FILES-C.md H1, H2): the audio is resampled to 44,100 Hz and the level converted so that the file sounds like the instrument
  // (OS-FILES-C C1b); what has no equivalent in a .pti is refused, not written.
  const PtiCheck check = pti_check(project, bank, slot, *in);
  if (check.verdict != PtiVerdict::Ok) {
    switch (check.verdict) {
      case PtiVerdict::AmigaEngine: return Start::PtiAmiga;
      case PtiVerdict::BitEngine: return Start::PtiBits;
      case PtiVerdict::TooLong: return Start::PtiTooLong;
      default: return Start::PtiLevel;
    }
  }
  watch_ = Watch{&bank, slot, bank.version(slot), nullptr, pcm.data, pcm.frames};
  if (!pti_.set(project, slot, pcm.data, pcm.frames, &SaveService::watch_ok, &watch_, check.level)) {
    return Start::NotSample;
  }
  pti_.set_resample(check.plan, bank.resample_kernel(), check.fine);
  // The name: the sanitised stem, then "<stem> 2" .. "<stem> 99" (the stem cut to leave room for the number) for the first that is free: an existing file is never replaced.
  char stem[guard::kNameMax + 1];
  instrument_file_stem(in->name, slot, stem, sizeof stem);
  char file[guard::kNameMax + 8];
  bool found = false;
  for (unsigned n = 1; n <= 99u && !found; ++n) {
    char base[guard::kNameMax + 1];
    std::strcpy(base, stem);
    std::size_t len = std::strlen(base);
    if (n > 1u) {
      const std::size_t keep = guard::kNameMax - 3u;  // " 99"
      len = len < keep ? len : keep;
      while (len != 0u && (base[len - 1u] == ' ' || base[len - 1u] == '.')) {
        --len;
      }
      base[len++] = ' ';
      if (n >= 10u) {
        base[len++] = static_cast<char>('0' + n / 10u);
      }
      base[len++] = static_cast<char>('0' + n % 10u);
      base[len] = '\0';
    }
    char path[guard::kPathCap];
    if (!guard::valid_name(base) || len + sizeof guard::kPtiSuffix > sizeof file) {
      return Start::NoName;
    }
    std::memcpy(file, base, len);
    std::memcpy(file + len, guard::kPtiSuffix, sizeof guard::kPtiSuffix);
    if (!guard::join(path, sizeof path, guard::kInstrumentsDir, file)) {
      return Start::NoName;
    }
    found = card_->exists(path, nullptr) == hal::storage::Status::NotFound;
    if (found) {
      std::memcpy(stem, base, len + 1u);
    }
  }
  if (!found) {
    return Start::NoName;
  }
  const Start started = launch(jobs, guard::kInstrumentsDir, file, AtomicSave::Mode::NewOnly, pti_, stem, Kind::Instrument, false);
  pti_hz_ = started == Start::Started ? check.source_hz : 0u;
  if (started == Start::Started) {
    pti_job_ = slot_;  // (only here: a refused press, such as a second one while this save runs, leaves it for the job in progress)
  }
  return started;
}

// ---- Export (OS-WAVEXP) ------------------------------------------------------------------------------------------------------------------------

// The project part of an export's names: the folder the user saved the project to last while it is still the one they are in, else the loaded folder's name, else "Untitled".
bool SaveService::export_project(const ExportRequest& rq, char* project) noexcept {
  if (card_ == nullptr) {
    return false;
  }
  const char* loaded = rq.loaded_folder != nullptr ? rq.loaded_folder : "";
  project[0] = '\0';
  if (current_[0] != '\0' && std::strcmp(anchor_, loaded) == 0) {
    std::strcpy(project, current_);  // the project the user saved last, while it is still the one they are in
  } else if (!project_name_of(loaded, project)) {
    std::strcpy(project, "Untitled");
  }
  return true;
}

// The folder, the file name and the strip's label of the next export of `rq`. The project part of the name is the project's folder name, "Untitled" when the user's project has none; the
// names are those of docs/specs/OS-WAVEXP.md (<Project>_Song0001, <Project>_P<n>_0001) with the project part cut so that the stem fits the guard (kNameMax, 31): the project's name can
// be 31 characters long itself. A selection is <Project>_SEL0001. A range of the song's slots (OS-SONGSHARE S5, NEW) is <Project>_S<first>-<last>_0001, the slot numbers as the Song screen counts them.
bool SaveService::export_names(const ExportRequest& rq, char* dir, std::size_t dir_cap, char* file, std::size_t file_cap, char* label, std::size_t label_cap) noexcept {
  char project[guard::kNameMax + 1] = {};
  if (!export_project(rq, project) || !guard::join(dir, dir_cap, guard::kExportDir, project)) {
    return false;
  }
  // "Song" + 4 digits, or "P" + the pattern + "_" + 4 digits, or "SEL" + 4 digits (a selection), or "S" + the first slot + "-" + the last + "_" + 4 digits: what follows the project's name and an underscore
  char tail[16];
  if (rq.form == ExportRequest::Form::Selection) {
    std::strcpy(tail, "SEL");
  } else if (rq.what.what == audio::OfflineRequest::What::Song && rq.what.order_last != audio::OfflineRequest::kToEnd) {
    unsigned n = 0;
    tail[n++] = 'S';
    for (unsigned k = 0; k < 2u; ++k) {  // the first slot, '-', the last: unpadded, as the pattern number of P<n>_
      const unsigned slot = k == 0u ? rq.what.order_first : rq.what.order_last;
      if (k != 0u) {
        tail[n++] = '-';
      }
      for (unsigned d = 10000; d != 0u; d /= 10u) {
        if (slot >= d || d == 1u) {
          tail[n++] = static_cast<char>('0' + (slot / d) % 10u);
        }
      }
    }
    tail[n++] = '_';
    tail[n] = '\0';
  } else if (rq.what.what == audio::OfflineRequest::What::Song) {
    std::strcpy(tail, "Song");
  } else {
    tail[0] = 'P';
    unsigned n = 1;
    const unsigned id = rq.what.pattern;
    if (id >= 100u) {
      tail[n++] = static_cast<char>('0' + id / 100u);
    }
    if (id >= 10u) {
      tail[n++] = static_cast<char>('0' + (id / 10u) % 10u);
    }
    tail[n++] = static_cast<char>('0' + id % 10u);
    tail[n++] = '_';
    tail[n] = '\0';
  }
  const std::size_t tail_len = std::strlen(tail);
  const std::size_t room = guard::kNameMax - 4u - 1u - tail_len;  // the stem: project, '_', tail, 4 digits
  char prefix[guard::kNameMax + 1];
  std::size_t n = std::strlen(project);
  n = n < room ? n : room;
  std::memcpy(prefix, project, n);
  prefix[n++] = '_';
  std::memcpy(prefix + n, tail, tail_len + 1u);
  unsigned cursor = 1;
  if (!free_name(file, file_cap, dir, prefix, guard::kWavSuffix, 4, 1, 9999, cursor)) {
    return false;
  }
  upper_label(label, label_cap, file + n);  // the strip says "EXPORT SONG0001": the part after the project's name
  const std::size_t len = std::strlen(label);
  if (len > 4u && label[len - 4u] == '.') {
    label[len - 4u] = '\0';  // (the ".wav" goes)
  }
  return true;
}

// The folder and the file-name prefix of the next set of stems, "<project>_Song_S0001_" (or "_P<n>_S0001_"): the project part is cut so that the longest name of the set (the label REVERB) fits the
// guard's 31 characters, and the set number is the first that none of the 19 names a set can hold is taken by.
bool SaveService::stem_names(const ExportRequest& rq, char* dir, std::size_t dir_cap, char* prefix, std::size_t prefix_cap) noexcept {
  char project[guard::kNameMax + 1] = {};
  if (!export_project(rq, project) || !guard::join(dir, dir_cap, guard::kExportDir, project)) {
    return false;
  }
  char tail[8];  // "Song", or "P" + the pattern
  if (rq.what.what == audio::OfflineRequest::What::Song) {
    std::strcpy(tail, "Song");
  } else {
    unsigned n = 0;
    tail[n++] = 'P';
    const unsigned id = rq.what.pattern;
    if (id >= 100u) {
      tail[n++] = static_cast<char>('0' + id / 100u);
    }
    if (id >= 10u) {
      tail[n++] = static_cast<char>('0' + (id / 10u) % 10u);
    }
    tail[n++] = static_cast<char>('0' + id % 10u);
    tail[n] = '\0';
  }
  constexpr std::size_t kLongest = 6;  // REVERB
  const std::size_t tail_len = std::strlen(tail);
  const std::size_t room = guard::kNameMax - (1u + tail_len + 2u + 4u + 1u + kLongest);  // '_' tail "_S" 4 digits '_' label
  char base[guard::kNameMax + 1];
  std::size_t n = std::strlen(project);
  n = n < room ? n : room;
  std::memcpy(base, project, n);
  base[n++] = '_';
  std::memcpy(base + n, tail, tail_len);
  n += tail_len;
  base[n++] = '_';
  base[n++] = 'S';
  for (unsigned number = 1; number <= 9999u; ++number) {
    bool vacant = true;
    char name[guard::kNameMax + 8];
    std::size_t len = n;
    std::memcpy(name, base, n);
    for (unsigned d = 1000; d != 0u; d /= 10u) {
      name[len++] = static_cast<char>('0' + (number / d) % 10u);
    }
    name[len++] = '_';
    for (unsigned i = 0; i < kStemLabelCount && vacant; ++i) {
      char label[StemPlan::kLabelChars];
      stem_label_at(i, label, sizeof label);
      std::size_t l = len;
      for (const char* p = label; *p != '\0'; ++p) {
        name[l++] = *p;
      }
      std::memcpy(name + l, guard::kWavSuffix, sizeof guard::kWavSuffix);
      char path[guard::kPathCap];
      if (!guard::join(path, sizeof path, dir, name)) {
        return false;
      }
      vacant = card_->exists(path, nullptr) == hal::storage::Status::NotFound;
    }
    if (vacant) {
      if (len + 1u > prefix_cap) {
        return false;
      }
      name[len] = '\0';
      std::memcpy(prefix, name, len + 1u);
      return true;
    }
  }
  return false;
}

bool SaveService::export_name(const ExportRequest& rq, char* out, std::size_t cap) noexcept {
  char dir[guard::kPathCap];
  if (rq.form == ExportRequest::Form::Stems) {  // "Export/<project>/<set>_*.wav": the set's files share the part before the star
    char prefix[guard::kNameMax + 1];
    char file[guard::kNameMax + 8];
    if (!stem_names(rq, dir, sizeof dir, prefix, sizeof prefix) || std::strlen(prefix) + 1u + sizeof guard::kWavSuffix > sizeof file) {
      return false;
    }
    std::strcpy(file, prefix);
    std::strcat(file, "*");
    std::strcat(file, guard::kWavSuffix);
    return guard::join(out, cap, dir, file);
  }
  char file[guard::kNameMax + 8];
  char label[24];
  return export_names(rq, dir, sizeof dir, file, sizeof file, label, sizeof label) && guard::join(out, cap, dir, file);
}

void SaveService::stem_now(char* out, std::size_t cap) const noexcept {
  if (cap == 0u) {
    return;
  }
  out[0] = '\0';
  if (stems_.next >= 1u && stems_.next <= stems_.plan.count && kind_ == Kind::Export) {
    stem_label(stems_.plan.part[stems_.next - 1u], out, cap);
  }
}

// The first file of a set: the plan, the names, and the job. The set holds the card from here to its last file.
SaveService::Start SaveService::start_stems(core::JobRunner& jobs, const ExportRequest& rq) noexcept {
  if (rq.project == nullptr) {
    return Start::NoExport;
  }
  stems_ = StemRun{};
  plan_stems(*rq.project, rq.what, stems_.plan);
  if (!stem_names(rq, stems_.dir, sizeof stems_.dir, stems_.prefix, sizeof stems_.prefix)) {
    stems_.plan.count = 0;
    return Start::NoName;
  }
  stems_.mix = stems_.plan.part[stems_.plan.count - 1u].rq;  // (the MIX is the last part: the base request as it was given)
  stems_.active = true;
  const Start s = start_stem(jobs);
  if (s != Start::Started) {
    stems_.active = false;
    end_save();
  }
  return s;
}

// The file `next` of the set. The first file measures the MIX (the length every file has); the others only write, to that length. A set of M files is M + 1 passes: the bar counts them all.
SaveService::Start SaveService::start_stem(core::JobRunner& jobs) noexcept {
  const unsigned k = stems_.next;
  const StemPart& part = stems_.plan.part[k];
  char label[StemPlan::kLabelChars];
  stem_label(part, label, sizeof label);
  char file[guard::kNameMax + 8];
  const std::size_t pl = std::strlen(stems_.prefix);
  std::memcpy(file, stems_.prefix, pl);
  std::size_t n = pl;
  for (const char* p = label; *p != '\0'; ++p) {
    file[n++] = *p;
  }
  std::memcpy(file + n, guard::kWavSuffix, sizeof guard::kWavSuffix);
  const unsigned units = stems_.plan.count + 1u;
  if (!export_.set_stem(part.rq, stems_.mix, k == 0u ? 0u : stems_.length, k == 0u ? 0u : k + 1u, units)) {
    return Start::NoExport;
  }
  const Start s = launch(jobs, stems_.dir, file, AtomicSave::Mode::NewOnly, export_, label, Kind::Export, false, "EXPORT ");
  if (s == Start::Started) {
    ++stems_.next;
  }
  return s;
}

// The next file of the set once the last one has ended (poll, and the App when it has taken the job's end in). A card taken for an update, one that went read-only, a cancel, or a file the guard does
// not take end the set.
SaveService::Start SaveService::advance_stems(core::JobRunner& jobs) noexcept {
  if (!stems_.active || (!yieldable() && (job_.running() || kind_ != Kind::None))) {
    return Start::Busy;
  }
  if (stems_.stop || stems_.next >= stems_.plan.count || (claim_ != nullptr && claim_()) || writer_->gate() != WriteStatus::Ok) {
    stems_.active = false;  // what was finished stays on the card
    end_save();
    return stems_.stop ? Start::Busy : Start::CardInUse;
  }
  yield_recent();
  const Start s = start_stem(jobs);
  if (s != Start::Started && s != Start::NoJobSlot) {
    stems_.active = false;
    end_save();
  }
  return s;
}

SaveService::Start SaveService::export_next(core::JobRunner& jobs) noexcept {
  reconcile();
  return advance_stems(jobs);
}

SaveService::Start SaveService::save_export(core::JobRunner& jobs, const ExportRequest& rq) noexcept {
  if (writer_ == nullptr || card_ == nullptr) {
    return Start::NoWriter;
  }
  reconcile();
  if (user_blocked()) {
    return Start::Busy;
  }
  if (claim_ != nullptr && claim_()) {
    return Start::CardInUse;
  }
  if (!export_.attached()) {
    return Start::NoExport;
  }
  if (writer_->gate() != WriteStatus::Ok) {
    return Start::NotWritable;
  }
  char dir[guard::kPathCap];
  char file[guard::kNameMax + 8];
  char label[24];
  if (rq.form == ExportRequest::Form::Stems) {
    return start_stems(jobs, rq);
  }
  if (!export_names(rq, dir, sizeof dir, file, sizeof file, label, sizeof label)) {
    return Start::NoName;
  }
  if (!export_.set(rq.what)) {
    return Start::NoExport;
  }
  return launch(jobs, dir, file, AtomicSave::Mode::NewOnly, export_, label, Kind::Export, false, "EXPORT ");
}

void SaveService::cancel_export(core::JobRunner& jobs) noexcept {
  if (stems_.active) {
    stems_.stop = true;  // no file after the one in progress (it may finish before the cancel reaches it)
  }
  if (kind_ == Kind::Export && job_.running() && slot_ >= 0) {
    jobs.cancel(static_cast<unsigned>(slot_));
  }
}

// ---- A whole project in the OG format (OS-FILES-C C2a) -----------------------------------------------------------------------------------------------------------------

const char* SaveService::og_start_text(Start s, WriteStatus gate) noexcept {
  switch (s) {
    case Start::NoWriter: return "NO CARD WRITE LAYER";
    case Start::NotWritable: return gate_text(gate);
    case Start::Busy: return "A SAVE IS ALREADY RUNNING";
    case Start::NoJobSlot: return "TOO MANY JOBS";
    case Start::NoName: return "NO FREE OG FOLDER NAME (OG .. OG 99)";
    case Start::BadName: return "PROJECT NAME CANNOT MAKE A FOLDER NAME";
    case Start::Empty: return "NOTHING TO EXPORT";
    case Start::CardInUse: return "FIRMWARE UPDATE HAS THE CARD";
    default: break;
  }
  return "COULD NOT START";
}

// The first of OG, OG 2 .. OG 99 that is not on the card: an export never goes into a folder that exists. Started: `path` ("Export/<name> OG[ n]") and `label` (the folder's name) are filled.
// NoName: the project has no name to make a folder of, or OG .. OG 99 are all taken; BadName: the name makes no folder name the guard takes.
SaveService::Start SaveService::og_pick_folder(const char* loaded_folder, char* path, std::size_t path_cap, char* label, std::size_t label_cap) noexcept {
  ExportRequest rq;
  rq.loaded_folder = loaded_folder;
  char name[guard::kNameMax + 1] = {};
  if (!export_project(rq, name)) {
    return Start::NoName;
  }
  for (unsigned n = 1; n <= guard::kOgFolderMaxN; ++n) {
    const Start s = og_folder_try(name, n, path, path_cap, label, label_cap);
    if (s != Start::NoName) {
      return s;  // Started (n is free) or BadName
    }
  }
  return Start::NoName;
}

// One candidate of the pick: Started (the folder is not on the card; `path` and `label` filled), NoName (it is taken: the next n), BadName.
SaveService::Start SaveService::og_folder_try(const char* name, unsigned n, char* path, std::size_t path_cap, char* label, std::size_t label_cap) noexcept {
  char folder[guard::kNameMax + 1];
  char full[guard::kPathCap];
  if (!og_folder_name(folder, sizeof folder, name, n) || !guard::join(full, sizeof full, guard::kExportDir, folder)) {
    return Start::BadName;  // (the project's name makes no folder name the guard takes; NoName is OG .. OG 99 all taken)
  }
  if (card_->exists(full, nullptr) != hal::storage::Status::NotFound) {
    return Start::NoName;
  }
  if (std::strlen(full) + 1u > path_cap || std::strlen(folder) + 1u > label_cap) {
    return Start::BadName;
  }
  std::strcpy(path, full);
  std::strcpy(label, folder);
  return Start::Started;
}

SaveService::Start SaveService::og_survey_begin(const model::Project& project, const SampleBank* bank, const char* loaded_folder) noexcept {
  og_timing_.clear_survey();
  const OgTiming::Scope timed(&og_timing_, OgTiming::Stage::Survey, false);
  ogs_.stage = SurveyRun::Stage::Idle;
  ogs_.out = OgSurvey{};
  if (writer_ == nullptr || card_ == nullptr) {
    ogs_.why = Start::NoWriter;
    return Start::NoWriter;
  }
  ExportRequest rq;
  rq.loaded_folder = loaded_folder;
  if (!export_project(rq, ogs_.name)) {
    ogs_.why = Start::NoName;
    return Start::NoName;
  }
  ogs_.project = &project;
  ogs_.bank = bank != nullptr && bank->ready() ? bank : nullptr;
  ogs_.n = 1;
  ogs_.why = Start::Started;
  ogs_.stage = SurveyRun::Stage::Folder;
  return Start::Started;
}

// One slice. The folder name: up to kFolderTries lookups of the card a pass (OG .. OG 99 are all taken only on a card with 99 exports); then the plan; then one writer's dry run.
SaveService::SurveyStep SaveService::og_survey_step() noexcept {
  constexpr unsigned kFolderTries = 4;
  using Stage = SurveyRun::Stage;
  const OgTiming::Scope timed(&og_timing_, OgTiming::Stage::Survey, false);  // (T16) a slice of the count
  auto fail = [this](Start why) {
    ogs_.stage = Stage::Failed;
    ogs_.why = why;
    ogs_.out = OgSurvey{};
    return SurveyStep::Failed;
  };
  switch (ogs_.stage) {
    case Stage::Folder:
      for (unsigned i = 0; i < kFolderTries && ogs_.n <= guard::kOgFolderMaxN; ++i, ++ogs_.n) {
        const Start s = og_folder_try(ogs_.name, ogs_.n, ogs_.path, sizeof ogs_.path, ogs_.out.folder, sizeof ogs_.out.folder);
        if (s == Start::Started) {
          ogs_.stage = Stage::Plan;
          return SurveyStep::More;
        }
        if (s != Start::NoName) {
          return fail(s);
        }
      }
      return ogs_.n > guard::kOgFolderMaxN ? fail(Start::NoName) : SurveyStep::More;
    case Stage::Plan:
      if (!ogs_.plan.begin(*ogs_.project, ogs_.bank)) {
        return fail(Start::Empty);
      }
      ogs_.plan.survey_start(ogs_.out.drops);  // the plan's counters, and the writers' as they are added
      ogs_.out.files = ogs_.plan.total();
      ogs_.out.bytes = ogs_.plan.bytes();
      ogs_.stage = Stage::Writers;
      return SurveyStep::More;
    case Stage::Writers:
      if (ogs_.plan.changed()) {  // the document or the audio moved under the run: the counters would be a mix, count again
        ogs_.n = 1;
        ogs_.out = OgSurvey{};
        ogs_.stage = Stage::Folder;
        return SurveyStep::More;
      }
      if (ogs_.plan.survey_step(ogs_.out.drops)) {
        ogs_.stage = Stage::Done;
        return SurveyStep::Done;
      }
      return SurveyStep::More;
    case Stage::Done:
      return SurveyStep::Done;
    case Stage::Failed:
      return SurveyStep::Failed;
    case Stage::Idle:
      break;
  }
  return SurveyStep::Failed;
}

SaveService::Start SaveService::og_survey(const model::Project& project, const SampleBank* bank, const char* loaded_folder, OgSurvey& out) noexcept {
  out = OgSurvey{};
  const Start begun = og_survey_begin(project, bank, loaded_folder);
  if (begun != Start::Started) {
    return begun;
  }
  SurveyStep s = SurveyStep::More;
  for (unsigned guard_steps = 0; s == SurveyStep::More && guard_steps < 1000u; ++guard_steps) {
    s = og_survey_step();
  }
  if (s != SurveyStep::Done) {
    og_survey_cancel();
    return s == SurveyStep::Failed ? ogs_.why : Start::Empty;
  }
  out = ogs_.out;
  return Start::Started;
}

// The folder, the plan and the first file. The set holds the card from here to its last file; project.mt is the last (OgExportPlan), so a folder that was cut short is never a project.
SaveService::Start SaveService::start_og_export(core::JobRunner& jobs, const model::Project& project, const char* loaded_folder, const SampleBank* bank) noexcept {
  if (writer_ == nullptr || card_ == nullptr) {
    return Start::NoWriter;
  }
  reconcile();
  if (user_blocked()) {
    return Start::Busy;
  }
  if (claim_ != nullptr && claim_()) {
    return Start::CardInUse;
  }
  if (writer_->gate() != WriteStatus::Ok) {
    return Start::NotWritable;
  }
  og_.active = false;
  og_.stop = false;
  og_.state = OgState::Idle;
  og_.done = 0;
  og_.drops = formats::og::ExportDrops{};
  og_.bank = bank != nullptr && bank->ready() ? bank : nullptr;
  og_.clipped = 0;
  og_.max_step_us = 0;
  og_.overruns = 0;
  og_.why = "";
  og_.folder[0] = '\0';
  og_.label[0] = '\0';
  og_timing_.reset();  // (T16) the survey's row stays
  Start picked;
  {
    const OgTiming::Scope timed(&og_timing_, OgTiming::Stage::Dir);
    picked = og_pick_folder(loaded_folder, og_.folder, sizeof og_.folder, og_.label, sizeof og_.label);
  }
  if (picked != Start::Started) {
    og_.folder[0] = '\0';
    og_.label[0] = '\0';
    return picked;
  }
  bool planned;
  {
    const OgTiming::Scope timed(&og_timing_, OgTiming::Stage::Plan);
    planned = og_.plan.begin(project, og_.bank);
  }
  if (!planned) {
    og_.folder[0] = '\0';
    return Start::Empty;
  }
  og_.drops = og_.plan.counters();  // the PCM stage's, known now; each file adds what its writer left out
  og_.state = OgState::Running;
  og_.active = true;
  const Start s = start_og_file(jobs);
  if (s != Start::Started) {
    og_.active = false;
    og_.state = OgState::Failed;
    end_save();
  }
  return s;
}

// The next file of the plan as a job: NewOnly, quiet (the strip is told at the last file and at a failure; the console reads the state).
SaveService::Start SaveService::start_og_file(core::JobRunner& jobs) noexcept {
  OgFile file;
  OgTiming::Scope plan_timed(&og_timing_, OgTiming::Stage::Plan);  // (T16) the next file's set-up: up to the job's start
  if (!og_.plan.next(file)) {
    return Start::Empty;
  }
  char dir[guard::kPathCap];
  if (!(file.in_patterns ? guard::join(dir, sizeof dir, og_.folder, guard::kOgPatternsDir)
                         : file.in_instruments ? guard::join(dir, sizeof dir, og_.folder, guard::kOgInstrumentsDir) : guard::join(dir, sizeof dir, og_.folder))) {
    return Start::NoName;
  }
  og_.file = file;
  SaveSource* source = &og_.source;
  if (file.kind == OgFile::Kind::Instrument) {  // the standalone .pti save's source (the OG instrument writer and the PCM stage), over the decision the plan counted
    const model::Project& project = *og_.plan.project();
    const OgInstrumentPlan ip = og_instrument_plan(project, og_.bank, file.id, nullptr);
    if (ip.result != OgInstrumentPlan::Result::Write) {
      return Start::NotSample;  // (the audio or the engine changed under the plan, which its snapshot should have caught: the export ends)
    }
    watch_ = Watch{og_.bank, file.id, og_.bank->version(file.id), nullptr, ip.pcm, ip.frames};
    if (!pti_.set(project, file.id, ip.pcm, ip.frames, &SaveService::watch_ok, &watch_, ip.level)) {
      return Start::NotSample;
    }
    pti_.set_resample(ip.plan, og_.bank->resample_kernel(), ip.fine);
    source = &pti_;
  } else {
    og_.source.set(*og_.plan.project(), file, og_.plan.generation());
  }
  const bool last = og_.plan.last();
  plan_timed.end();
  const OgTiming::Scope launch_timed(&og_timing_, OgTiming::Stage::Other);  // (T16) the job's start: the card pin, the guard's checks
  const Start s = launch(jobs, dir, file.name, AtomicSave::Mode::NewOnly, *source, last ? og_.label : file.label, Kind::OgExport, !last, last ? "EXPORT " : "OG ");
  if (s == Start::Started) {
    job_.set_timing(&og_timing_);
    og_.plan.advance();
  }
  return s;
}

void SaveService::end_og(OgState state, const char* why) noexcept {
  og_.active = false;
  og_.state = state;
  og_.why = why;
  end_save();  // (what was finished stays on the card; nothing is removed)
}

// The next file once the last one has ended (poll). A cancel, a card taken for an update or gone read-only, and a document that moved since the plan began end the set; the revision is rechecked HERE,
// before every file and before project.mt, so that the files of one export are all of one revision.
SaveService::Start SaveService::advance_og(core::JobRunner& jobs) noexcept {
  if (!og_.active || (!yieldable() && (job_.running() || kind_ != Kind::None))) {
    return Start::Busy;
  }
  if (og_.stop) {
    end_og(OgState::Cancelled, "");
    return Start::Busy;
  }
  if (claim_ != nullptr && claim_()) {
    end_og(OgState::Failed, "FIRMWARE UPDATE HAS THE CARD");
    return Start::CardInUse;
  }
  {
    const OgTiming::Scope timed(&og_timing_, OgTiming::Stage::Dir);  // (T16) the gate asks the card (mounted, FAT, write protect)
    if (writer_->gate() != WriteStatus::Ok) {
      end_og(OgState::Failed, gate_text(writer_->gate()));
      return Start::NotWritable;
    }
  }
  {
    const OgTiming::Scope timed(&og_timing_, OgTiming::Stage::Plan);
    if (og_.plan.changed()) {
      end_og(OgState::Modified, "THE PROJECT CHANGED");
      return Start::Busy;
    }
  }
  yield_recent();
  const Start s = start_og_file(jobs);
  if (s != Start::Started && s != Start::NoJobSlot) {
    end_og(OgState::Failed, "COULD NOT START THE NEXT FILE");
  }
  return s;
}

const char* SaveService::og_levels() const noexcept {
  if (og_.state == OgState::Idle) {
    return "";
  }
  return og_.drops.levels_converted != 0u || og_.drops.levels_clamped != 0u ? "LEVELS WRITTEN AS PLAYED (1.1.14 -> 1.2+)" : "LEVELS WRITTEN AS STORED";
}

void SaveService::cancel_og_export(core::JobRunner& jobs) noexcept {
  if (!og_.active) {
    return;
  }
  og_.stop = true;  // no file after the one in progress (it may finish before the cancel reaches it)
  if (kind_ == Kind::OgExport && job_.running() && slot_ >= 0) {
    jobs.cancel(static_cast<unsigned>(slot_));
  }
}

std::uint32_t SaveService::signature(const model::Settings& device) noexcept {
  std::uint32_t sum = device.learn_generation();
  for (unsigned i = 0; i < device.field_count(); ++i) {
    sum = sum * 31u + device.generation(i);
  }
  return sum;
}

bool SaveService::dirty(const model::Project& project) noexcept {
  reconcile();
  return unsaved_ || !clean_valid_ || project.generation() != clean_generation_;
}

void SaveService::watch(const model::Project& project) noexcept {
  if (!clean_valid_) {
    clean_generation_ = project.generation();
    clean_valid_ = true;
  }
}

void SaveService::document_replaced(const model::Project& project, bool unsaved) noexcept {
  reconcile();  // (a save that ended is taken in first, so that it cannot overwrite what this sets)
  clean_generation_ = project.generation();
  clean_valid_ = true;
  unsaved_ = unsaved;
  current_[0] = '\0';
  anchor_[0] = '\0';
  plan_.active = false;  // (a plan that was between two jobs belonged to the document that is gone)
  if (!job_.running()) {
    end_save();  // (a job still running ends the save in reconcile)
  }
}

// OS-BUG-REMOUNT (see the declaration).
void SaveService::card_remounted() noexcept {
  reconcile();
  ++remount_epoch_;
  revalidate_ = true;
  recent_loaded_ = false;
}

// What earlier saves wrote is on the card they wrote it on: a card swapped since (another volume) holds none of it, so a project saved to it would skip its samples.
void SaveService::forget_other_card() noexcept {
  const std::uint32_t id = writer_->card_id();
  if (id != known_card_) {
    std::memset(known_, 0, sizeof known_);
    std::memset(known_side_, 0, sizeof known_side_);
    known_dir_[0] = '\0';
    known_card_ = id;
  }
}

// The RECENT file's write is cancelled when a save, a move or an export asks for the card: it is small and quiet, nothing waits for it, and refusing the user's key for it would be wrong. The slot is let
// go (nothing says a word of it) and the list is marked unwritten, so that the next idle pass writes it. An app's file (OS-H4) yields the same way: the user's request is never refused because of an
// app, the app is told FAILED.
void SaveService::yield_recent() noexcept {
  if (kind_ == Kind::AppFile) {
    if (!job_.past_write()) {  // (an app's file that is reading back or committing is let finish)
      drop_app_file();
    }
    return;
  }
  if (kind_ != Kind::Recent || !job_.running() || jobs_ == nullptr || slot_ < 0) {
    return;
  }
  jobs_->cancel(static_cast<unsigned>(slot_));
  jobs_->release(static_cast<unsigned>(slot_));
  kind_ = Kind::None;
  quiet_slot_ = -1;
  silent_slot_ = -1;
  recent_saved_ = recent_.generation() - 1u;
  if (!plan_.active) {
    end_save();
  }
}

void SaveService::end_save() noexcept {
  writer_->end_save();
  plan_.pinned = false;
}

// ---- A file an app writes (OS-H4) ------------------------------------------------------------------------------------------------------------------

SaveService::Start SaveService::start_app_file(const char* app, const char* stem, const char* ext, SaveSource& source, char* name_out, std::size_t cap, bool& no_suffix) noexcept {
  no_suffix = false;
  if (writer_ == nullptr || card_ == nullptr || jobs_ == nullptr) {
    return Start::NoWriter;
  }
  reconcile();
  if (occupied()) {  // (the RECENT file steps aside in launch(); an app's file or any other job does not)
    return Start::Busy;
  }
  if (claim_ != nullptr && claim_()) {
    return Start::CardInUse;
  }
  if (writer_->gate() != WriteStatus::Ok) {
    return Start::NotWritable;
  }
  // The guard decides again what the names may be: the app folder and the stem are names it takes, the extension is on its list.
  const std::size_t stem_len = stem != nullptr ? std::strlen(stem) : 0u;
  const std::size_t ext_len = ext != nullptr ? std::strlen(ext) : 0u;
  char dir[guard::kPathCap];
  if (!guard::app_ext_ok(ext) || !guard::valid_name(app) || !guard::valid_name(stem) || !guard::join(dir, sizeof dir, guard::kAppExportDir, app)) {
    return Start::NoName;
  }
  char file[guard::kNameMax + 8];
  std::memcpy(file, stem, stem_len);
  file[stem_len] = '.';
  std::memcpy(file + stem_len + 1u, ext, ext_len + 1u);
  char path[guard::kPathCap];
  if (!guard::join(path, sizeof path, dir, file)) {
    return Start::NoName;
  }
  const hal::storage::Status first = card_->exists(path, nullptr);
  if (first != hal::storage::Status::NotFound && first != hal::storage::Status::Ok) {
    return Start::NotWritable;  // the card cannot say (a read error, no card): not a taken name
  }
  if (first == hal::storage::Status::Ok) {  // taken (a file or a folder of that name): the numbered forms
    char prefix[guard::kNameMax + 2];
    const std::size_t keep = stem_len < guard::kNameMax - 4u ? stem_len : guard::kNameMax - 4u;  // "_001" must fit the 31 bytes
    std::memcpy(prefix, stem, keep);
    prefix[keep] = '_';
    prefix[keep + 1u] = '\0';
    char suffix[guard::kNameMax];
    suffix[0] = '.';
    std::memcpy(suffix + 1, ext, ext_len + 1u);
    // Each probe is a directory lookup on the card, made inside the app's call (a frame of its time budget), so the number is bounded: 1..8 one by one (the usual answer), then the step doubles from
    // the last taken number and a binary search closes in between the last taken and the first free one found. That is at most about 25 lookups for 999 names, where a plain scan was 999. With the taken
    // numbers in one run (a name exported again and again) the answer is the lowest free one; with holes it is a free one, never a taken one (NewOnly never replaces either way).
    const auto numbered = [&](unsigned n, char* out) {
      std::size_t len = 0;
      for (const char* q = prefix; *q != '\0'; ++q) {
        out[len++] = *q;
      }
      out[len++] = static_cast<char>('0' + n / 100u);
      out[len++] = static_cast<char>('0' + (n / 10u) % 10u);
      out[len++] = static_cast<char>('0' + n % 10u);
      std::memcpy(out + len, suffix, std::strlen(suffix) + 1u);
    };
    const auto is_free = [&](unsigned n) {
      char name[guard::kNameMax + 8];
      char full[guard::kPathCap];
      numbered(n, name);
      return guard::join(full, sizeof full, dir, name) && card_->exists(full, nullptr) == hal::storage::Status::NotFound;
    };
    unsigned found = 0;
    for (unsigned n = 1; n <= 8u && found == 0u; ++n) {
      found = is_free(n) ? n : 0u;
    }
    if (found == 0u) {
      unsigned taken = 8;
      unsigned step = 8;
      while (taken < 999u && found == 0u) {
        const unsigned probe = taken + step < 999u ? taken + step : 999u;
        if (is_free(probe)) {
          found = probe;
        } else {
          taken = probe;
          step *= 2u;
        }
      }
      while (found != 0u && found - taken > 1u) {
        const unsigned mid = taken + (found - taken) / 2u;
        if (is_free(mid)) {
          found = mid;
        } else {
          taken = mid;
        }
      }
    }
    if (found == 0u) {
      no_suffix = true;
      return Start::NoName;
    }
    numbered(found, file);
  }
  char label[guard::kNameMax + 8];
  upper_label(label, sizeof label, file);
  const Start started = launch(*jobs_, dir, file, AtomicSave::Mode::NewOnly, source, label, Kind::AppFile, false, "EXPORT ");
  if (started == Start::Started) {
    app_result_ = AppFileState::None;
    const std::size_t n = std::strlen(file);
    if (name_out != nullptr && cap != 0u) {
      const std::size_t take = n < cap - 1u ? n : cap - 1u;
      std::memcpy(name_out, file, take);
      name_out[take] = '\0';
    }
  }
  return started;
}

SaveService::AppFileState SaveService::app_file() noexcept {
  if (writer_ == nullptr) {
    return AppFileState::None;
  }
  reconcile();
  return kind_ == Kind::AppFile && job_.running() ? AppFileState::Running : app_result_;
}

void SaveService::drop_app_file() noexcept {
  if (writer_ == nullptr) {
    return;
  }
  reconcile();
  if (kind_ != Kind::AppFile || !job_.running() || jobs_ == nullptr || slot_ < 0) {
    return;
  }
  jobs_->cancel(static_cast<unsigned>(slot_));  // the job repairs: its Temp is removed now
  jobs_->release(static_cast<unsigned>(slot_));
  kind_ = Kind::None;
  quiet_slot_ = -1;
  silent_slot_ = -1;
  app_result_ = AppFileState::Failed;
  if (!plan_.active) {
    end_save();
  }
}

// A save that ended is taken in before anything is asked of the service again (an action may come before the pass's poll).
void SaveService::reconcile() noexcept {
  if (kind_ == Kind::None || job_.running()) {
    return;
  }
  if (kind_ == Kind::Sample) {
    if (job_.done()) {  // the folder holds this version of the slot's audio now
      if (!same_ci(known_dir_, plan_.name) || smp_card_ != known_card_) {
        std::memset(known_, 0, sizeof known_);
        std::memset(known_side_, 0, sizeof known_side_);
        for (hal::storage::FileStamp& f : known_file_) {
          f = hal::storage::FileStamp{};
        }
        std::strcpy(known_dir_, plan_.name);
        known_card_ = smp_card_;
      }
      if (smp_slot_ < 64u && smp_side_) {
        known_side_[smp_slot_] = smp_version_;  // (no stamp: after a remount the side file is written again)
      } else if (smp_slot_ < 64u) {
        known_[smp_slot_] = smp_version_;
        hal::storage::FileStamp written;
        known_file_[smp_slot_] = card_ != nullptr && card_->stat(smp_path_, written) == hal::storage::Status::Ok ? written : hal::storage::FileStamp{};  // (what it looks like now, for a later remount)
      }
    } else {
      plan_.active = false;  // a sample did not go: the project file is not written (the strip has said why)
    }
  }
  if (kind_ == Kind::Project && job_.done()) {
    recent_touch(pending_name_);
    if (save_epoch_ == remount_epoch_) {
      revalidate_ = false;  // every sample of this save was checked, skipped or written, and the card did not come back since: the beliefs are current
    }
    clean_generation_ = pending_generation_;  // the file on the card is the project as it was when the save started (and the writer failed the save if it moved)
    clean_valid_ = true;
    unsaved_ = false;
    std::strcpy(current_, pending_name_);  // from now on SAVE replaces this project's file, while it is the project the user is in
    std::strcpy(anchor_, pending_anchor_);
  }
  if (kind_ == Kind::AppFile) {
    app_result_ = job_.done() ? AppFileState::Done : AppFileState::Failed;  // (OS-H4) the app reads it with export_status
  }
  if (kind_ == Kind::Wav) {
    wav_take_done_ = job_.done() && watch_.take != nullptr;  // E-SREC-09: the file of a take is on the card
  }
  if (kind_ == Kind::Export && stems_.active) {  // a file of a set of stems: counted, and the set goes on or ends here
    if (job_.done()) {
      ++stems_.done;
      if (stems_.length == 0u) {
        stems_.length = export_.length_frames();  // the first file's measuring pass: every other file is this long
      }
      stems_.active = stems_.next < stems_.plan.count;
    } else {
      stems_.active = false;  // cancelled or failed: the files before it stay, none is started after it
    }
  }
  if (kind_ == Kind::OgExport) {  // a file of an OG export: counted, and the set goes on (advance_og) or ends here
    const OgTiming::Scope timed(&og_timing_, OgTiming::Stage::Other);  // (T16)
    if (job_.done()) {
      ++og_.done;
      if (og_.file.kind == OgFile::Kind::Instrument) {
        og_.drops.add(pti_.drops());
        og_.clipped += pti_.clipped();
      } else {
        og_.drops.add(og_.source.drops());
      }
      if (og_.done >= og_.plan.total()) {  // project.mt, the last, is down: the folder is a project
        og_.active = false;
        og_.state = OgState::Done;
      }
    } else {
      og_.active = false;  // cancelled or failed: the files before it stay, none is started after it, and project.mt is not among them
      og_.state = job_.error() == AtomicSave::Error::None ? OgState::Cancelled : OgState::Failed;
      og_.why = job_.detail();
    }
  }
  kind_ = Kind::None;
  quiet_slot_ = -1;
  silent_slot_ = -1;
  if (!plan_.active && !stems_.active && !og_.active) {
    end_save();  // (a project's plan ends with its last job, or with the sample that failed; a set of stems with its last file)
  }
}

void SaveService::poll(core::JobRunner& jobs, const model::Settings& device, std::uint64_t now_ms) noexcept {
  if (writer_ == nullptr || card_ == nullptr) {
    return;
  }
  jobs_ = &jobs;
  reconcile();
  if (power_ && !job_.running()) {
    stems_.active = false;  // OS-POWER: a set that was told to stop ends with the file that was in flight
    og_.active = false;
  }
  app_export_.tick(now_ms);  // (OS-H4: an app's file left open with no write is given up)
  export_.service();  // (an export's mixer that close() could not put back: tried again every pass)
  if (plan_.active) {
    (void)advance_plan(jobs);  // the next job of a project save (OS-FU2); the settings wait for it
    return;
  }
  if (og_.active) {
    (void)advance_og(jobs);  // the next file of an OG export (OS-FILES-C C2a)
    return;
  }
  if (stems_.active) {
    (void)advance_stems(jobs);  // the next file of a set of stems (the App starts it at once; this is for a pass with no free slot)
    return;
  }
  if (hold_ || writer_->gate() == WriteStatus::Host) {
    return;  // OS-USBDISK 4.2/4.3 (Q9): the card is being handed to the computer or is out: the RECENT list and the device settings stay dirty in memory (nothing is marked saved) and are written after the eject
  }
  if (recent_.generation() != recent_saved_ && !power_ && kind_ == Kind::None && !job_.running() && (claim_ == nullptr || !claim_())) {
    // OS-FILES part B: the RECENT file, after a change of the list. A card that may not be written keeps the list in memory (not asked again until it changes); a failed write is silent.
    recent_saved_ = recent_.generation();
    const std::size_t size = writer_->gate() == WriteStatus::Ok ? recent_.write(recent_buf_, sizeof recent_buf_) : 0u;
    if (size != 0u) {
      recent_source_.set(recent_buf_, size);
      if (launch(jobs, guard::kSettingsDir, guard::kRecentFile, AtomicSave::Mode::Replace, recent_source_, "RECENT", Kind::Recent, true) == Start::Started) {
        silent_slot_ = slot_;
      }
      return;
    }
  }
  const std::uint32_t sig = signature(device);
  if (!settings_seen_) {  // the first pass: what the settings are now (the file's values, or the defaults) is what is saved
    settings_seen_ = true;
    settings_sig_ = settings_saved_ = sig;
    settings_moved_ms_ = now_ms;
    return;
  }
  if (sig != settings_sig_) {
    settings_sig_ = sig;
    settings_moved_ms_ = now_ms;
  }
  settings_clean_ = sig == settings_saved_;
  if (power_gave_up_ || sig == settings_saved_ || (!power_ && now_ms - settings_moved_ms_ < kSettingsDebounceMs) || kind_ != Kind::None || job_.running()) {
    return;  // (the shutdown writes the settings at once: no 1.5 s of quiet to wait for)
  }
  if (claim_ != nullptr && claim_()) {
    return;  // the firmware update path has the card: the settings wait (the signature still differs, so they are saved when it lets go)
  }
  if (writer_->gate() != WriteStatus::Ok) {
    settings_saved_ = sig;  // a card that may not be written keeps the settings in memory: not asked again until they change
    return;
  }
  const std::size_t size = formats::settings::write(device, settings_, sizeof settings_);
  if (size == 0u) {
    settings_saved_ = sig;
    return;
  }
  buffer_.set(settings_, size);
  if (launch(jobs, guard::kSettingsDir, guard::kSettingsFile, AtomicSave::Mode::Replace, buffer_, "SETTINGS", Kind::Settings, true) == Start::Started) {
    settings_saved_ = sig;  // (a change while it runs moves the signature again: the next save follows it)
  }
}

// ---- The shutdown (OS-POWER P1) -----------------------------------------------------------------------------------------------------------------------

void SaveService::power_begin() noexcept {
  power_ = true;
  stems_.stop = true;  // (no file after the one in progress)
  og_.stop = true;
  yield_recent();      // the RECENT file's write, or an app's file still being written, gives way: the shutdown has the card
}

bool SaveService::power_busy() noexcept {
  if (writer_ == nullptr) {
    return false;
  }
  reconcile();
  // OS-USBDISK 4.3/7 (U2a-3): with the card handed over or being handed over (hold_, gate Host) poll() writes nothing, so the settings never become clean: the shutdown does not wait for them
  // (it would run out its bound). A change made on the unit during Host is lost, as in any power cut (spec 7).
  const bool deferred = hold_ || writer_->gate() == WriteStatus::Host;
  return occupied() || (power_ && !power_gave_up_ && !settings_clean_ && !deferred);  // (a set that was told to stop is dropped by poll() once its file has ended)
}

void SaveService::power_cancel() noexcept {
  power_gave_up_ = true;
  plan_.active = false;
  stems_.active = false;
  og_.active = false;
  if (job_.running() && jobs_ != nullptr && slot_ >= 0) {
    jobs_->cancel(static_cast<unsigned>(slot_));  // the job repairs: its Temp is removed now and the old file stays whole
  }
  reconcile();
  if (!job_.running() && kind_ == Kind::None) {
    end_save();  // (a plan between two jobs had pinned the writer)
  }
}

// ---- Moving what the user made (OS-FILES part B) ---------------------------------------------------------------------------------------------

bool SaveService::open_project_name(const char* loaded_folder, char* out, std::size_t cap) const noexcept {
  const char* loaded = loaded_folder != nullptr ? loaded_folder : "";
  char name[guard::kNameMax + 1] = {};
  if (current_[0] != '\0' && std::strcmp(anchor_, loaded) == 0) {
    std::strcpy(name, current_);  // the project the user saved last, while it is still the one they are in
  } else if (!project_name_of(loaded, name)) {
    return false;
  }
  if (out == nullptr || std::strlen(name) + 1u > cap) {
    return false;
  }
  std::strcpy(out, name);
  return true;
}

SaveService::Move SaveService::move_ready() noexcept {
  if (writer_ == nullptr || card_ == nullptr) {
    return Move::NoWriter;
  }
  reconcile();
  if (user_blocked()) {
    return Move::Busy;
  }
  if (claim_ != nullptr && claim_()) {
    return Move::CardInUse;
  }
  if (writer_->gate() != WriteStatus::Ok) {
    return Move::NotWritable;
  }
  yield_recent();  // (the move goes ahead: what steps aside for the user does now)
  return Move::Done;
}

void SaveService::forget_dir(const char* name) noexcept {
  if (same_ci(known_dir_, name)) {
    std::memset(known_, 0, sizeof known_);
    std::memset(known_side_, 0, sizeof known_side_);  // a new folder of this name must not be taken for the one that was here: its samples are not on the card
    known_dir_[0] = '\0';
  }
}

// DELETE: the empty marker file Projects/<name>/.deleted (create-new, one directory entry, no cluster). Nothing is renamed or moved, so there is no half-done state but "marker written or not".
SaveService::Move SaveService::delete_project(const char* loaded_folder, const char* name) noexcept {
  const Move ready = move_ready();
  if (ready != Move::Done) {
    return ready;
  }
  if (!guard::valid_name(name)) {
    return Move::BadName;
  }
  char open[guard::kNameMax + 1];
  if (open_project_name(loaded_folder, open, sizeof open) && same_ci(open, name)) {
    return Move::OpenProject;
  }
  char folder[guard::kPathCap];
  char marker[guard::kPathCap];
  if (!guard::join(folder, sizeof folder, guard::kProjectsDir, name) || !guard::join(marker, sizeof marker, folder, guard::kDeletedMarker)) {
    return Move::BadName;
  }
  if (card_->exists(folder, nullptr) != hal::storage::Status::Ok) {
    return Move::NotFound;
  }
  writer_->begin_save();
  const WriteStatus st = writer_->create_marker(marker);
  writer_->end_save();
  move_status_ = st;
  Move done = Move::Failed;
  if (st == WriteStatus::Ok) {
    done = Move::Done;
  } else if (st == WriteStatus::Exists) {
    done = folder_marked_deleted(*card_, folder) ? Move::Done : Move::Failed;  // marked already: the same answer; a FOLDER called .deleted is the user's, not a marker: "name exists"
  } else if (st == WriteStatus::NotFound) {
    done = Move::NotFound;
  } else if (card_->exists(marker, nullptr) == hal::storage::Status::Ok) {
    done = Move::Done;  // it went through; what failed came after (FatFs's bookkeeping)
  }
  if (done == Move::Done) {
    forget_dir(name);
    recent_refresh();
    (void)recent_.remove(name);
  }
  return done;
}

// ---- RECENT -----------------------------------------------------------------------------------------------------------------------------------

void SaveService::recent_touch(const char* name) noexcept {
  recent_refresh();
  (void)recent_.add(name);
}

void SaveService::note_opened(const char* folder) noexcept {
  char name[guard::kNameMax + 1];
  if (project_name_of(folder, name)) {
    recent_touch(name);
  }
}

namespace {

// Settings/recent.o3rc (or the .bak a write cut between its two renames leaves: atomic_save.h) into `list`. False: the card cannot say (not mounted, an error), the list is untouched. A file that is missing or
// damaged gives an empty list (true).
bool read_recent(hal::storage::Storage& card, RecentList& list) noexcept {
  char path[guard::kPathCap];
  if (!guard::join(path, sizeof path, guard::kSettingsDir, guard::kRecentFile)) {
    return false;
  }
  hal::storage::FileHandle file;
  hal::storage::Status opened = card.open(path, file);
  if (opened == hal::storage::Status::NotFound) {
    const std::size_t len = std::strlen(path);
    std::memcpy(path + len, guard::kBackupSuffix, sizeof guard::kBackupSuffix);
    opened = card.open(path, file);
  }
  if (opened != hal::storage::Status::Ok && opened != hal::storage::Status::NotFound) {
    return false;
  }
  std::uint8_t buffer[RecentList::kMaxFileBytes + 1];
  std::size_t have = 0;
  if (opened == hal::storage::Status::Ok) {
    hal::storage::Status s = hal::storage::Status::Ok;
    while (have < sizeof buffer && s == hal::storage::Status::Ok) {
      std::size_t got = 0;
      s = card.read(file, buffer + have, sizeof buffer - have, got);
      if (got == 0u) {
        break;
      }
      have += got;
    }
    card.close(file);
    if (s != hal::storage::Status::Ok) {
      have = 0;  // unreadable: an empty list
    }
  }
  (void)list.parse(buffer, have);  // (a file one byte over the longest good one is damaged: parse() takes none)
  return true;
}

}  // namespace

void SaveService::recent_refresh() noexcept {
  if (card_ == nullptr) {
    return;
  }
  const std::uint32_t id = writer_ != nullptr ? writer_->card_id() : 0u;
  if ((recent_loaded_ && id == recent_card_) || recent_saved_ != recent_.generation() || kind_ == Kind::Recent) {
    return;  // read already, or changes in memory are waiting for their write: they are newer than the file
  }
  if (!read_recent(*card_, recent_)) {
    return;  // the card cannot say (not mounted, an error): the list stays as it was and the next visit asks again
  }
  recent_loaded_ = true;
  recent_card_ = id;
  recent_saved_ = recent_.generation();
}

bool boot_recent_project(hal::storage::Storage& card, char* folder, std::size_t cap, bool& native) noexcept {
  native = false;
  RecentList list;
  if (folder == nullptr || cap == 0u || !read_recent(card, list)) {
    return false;
  }
  folder[0] = '\0';
  if (list.count() == 0u) {
    return false;
  }
  const char* name = list.name(0);  // the newest: the one that was open last (a SAVE AS or an OPEN puts it first)
  char path[guard::kPathCap];
  if (!guard::join(path, sizeof path, guard::kProjectsDir, name)) {
    return false;
  }
  const std::size_t len = std::strlen(path);
  if (len + 1u > cap || folder_marked_deleted(card, path)) {
    return false;
  }
  char file[guard::kPathCap];
  bool is_dir = false;
  if (!guard::join(file, sizeof file, path, "project.o3pj") || card.exists(file, &is_dir) != hal::storage::Status::Ok || is_dir) {
    if (!guard::join(file, sizeof file, path, "project.mt") || card.exists(file, &is_dir) != hal::storage::Status::Ok || is_dir) {
      return false;  // nothing of a project is there any more
    }
  } else {
    native = true;
  }
  std::memcpy(folder, path, len + 1u);
  return true;
}

bool load_device_settings(hal::storage::Storage& card, model::Settings& out) noexcept {
  out.init(core::ParamKind::Device);
  char path[guard::kPathCap];
  if (!guard::join(path, sizeof path, guard::kSettingsDir, guard::kSettingsFile)) {
    return false;
  }
  hal::storage::FileHandle file;
  hal::storage::Status opened = card.open(path, file);
  if (opened == hal::storage::Status::NotFound) {
    // A save of the settings cut between the two renames of its steps 8 and 9 (app/atomic_save.h) leaves no device.o3st and the old one whole as device.o3st.bak: read that, so that
    // the user's settings are not lost to the cut. Read only: the next save of the settings (Restore, its step 2) puts the name back.
    const std::size_t len = std::strlen(path);
    if (len + sizeof guard::kBackupSuffix > sizeof path) {
      return false;
    }
    std::memcpy(path + len, guard::kBackupSuffix, sizeof guard::kBackupSuffix);
    opened = card.open(path, file);
  }
  if (opened != hal::storage::Status::Ok) {
    return false;
  }
  std::uint8_t buffer[formats::settings::kMaxFileBytes];
  std::size_t have = 0;
  hal::storage::Status s = hal::storage::Status::Ok;
  while (have < sizeof buffer && s == hal::storage::Status::Ok) {
    std::size_t got = 0;
    s = card.read(file, buffer + have, sizeof buffer - have, got);
    if (got == 0u) {
      break;
    }
    have += got;
  }
  card.close(file);
  if (s != hal::storage::Status::Ok) {
    return false;
  }
  const formats::settings::Report report = formats::settings::read(buffer, have, out);
  return report.error == formats::settings::Error::None;
}

}  // namespace o3os::app
