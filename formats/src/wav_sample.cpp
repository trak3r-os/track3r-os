// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/wav_sample.h>

#include <cstring>

namespace o3os::formats::wav {

namespace {

// A position in frames as the OG's 0..65535 fraction of the sample's length, rounded to nearest.
std::uint16_t position(std::uint32_t frame, std::uint32_t frames) noexcept {
  const std::uint64_t p = (static_cast<std::uint64_t>(frame) * 65535u + frames / 2u) / frames;
  return static_cast<std::uint16_t>(p > 65535u ? 65535u : p);
}

FormatError map_error(Error e) noexcept {
  switch (e) {
    case Error::None: return FormatError::None;
    case Error::Truncated: return FormatError::Truncated;
    case Error::BadMagic: return FormatError::BadMagic;
    case Error::Unsupported: return FormatError::Unsupported;
    case Error::LimitExceeded: return FormatError::LimitExceeded;
    case Error::Malformed: return FormatError::Malformed;
    case Error::SourceFailed: return FormatError::SourceFailed;
    case Error::Cancelled: return FormatError::Cancelled;
  }
  return FormatError::Internal;
}

bool valid_frame(unsigned n) noexcept { return n >= 32u && n <= 2048u && (n & (n - 1u)) == 0u; }  // our wavetable window sizes

}  // namespace

unsigned wavetable_frame(std::uint32_t frames, unsigned marker, std::uint16_t wavetable, bool* unusable) noexcept {
  if (unusable != nullptr) {
    *unusable = false;
  }
  if (wavetable == kWtOff || frames == 0u) {
    return 0u;
  }
  unsigned size = 0;
  if (wavetable >= 2u) {  // an explicit size, over the marker's
    size = wavetable;
  } else if (marker != 0u && (wavetable != kWtDetect || (valid_frame(marker) && frames >= marker))) {
    size = marker;  // (a user's "load as wavetable" falls back to the length when the marker is no size the engine has)
  } else if (wavetable == kWtDetect) {
    size = frames % 2048u == 0u ? 2048u : (frames % 1024u == 0u ? 1024u : (frames % 256u == 0u ? 256u : 0u));
    return size;  // (nothing to say about a length that fits no frame: it is a sample)
  } else {
    return 0u;  // kWtAuto, no marker: a sample
  }
  if (!valid_frame(size) || frames < size) {  // not a size the engine has, or not one whole window
    if (unusable != nullptr) {
      *unusable = true;
    }
    return 0u;
  }
  return size;
}

void make_wavetable_fields(model::SamplerParams& s, unsigned frame) noexcept {
  s.sample_type = 1;
  s.play_mode = 6;
  s.wavetable_window_size = static_cast<std::uint16_t>(frame);
  s.wavetable_window = 0;
}

void make_sample_instrument(const wav::Info& info, std::uint32_t slot, const char* file_name,
                            model::Instrument& out, std::uint16_t wavetable) noexcept {
  model::init_instrument(out, model::InstrumentKind::Sample);

  // name: the file's name, its directory and extension cut, printable ASCII only
  const char* base = file_name != nullptr ? file_name : "";
  for (const char* p = base; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  std::size_t len = std::strlen(base);
  for (std::size_t i = len; i > 1; --i) {
    if (base[i - 1] == '.') {
      len = i - 1;
      break;
    }
  }
  std::size_t n = 0;
  for (std::size_t i = 0; i < len && n < model::kInstrumentNameMax; ++i) {
    const unsigned char c = static_cast<unsigned char>(base[i]);
    out.name[n++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
  }

  // the path is ours, as for the OG import
  const char stem[] = "samples/instr";
  std::memcpy(out.sample_path, stem, sizeof stem - 1);
  std::size_t at = sizeof stem - 1;
  const std::uint32_t number = slot + 1u;  // 1..64
  out.sample_path[at++] = static_cast<char>('0' + number / 10u);
  out.sample_path[at++] = static_cast<char>('0' + number % 10u);
  std::memcpy(out.sample_path + at, ".wav", 4);

  out.sample_rate = info.sample_rate;

  model::SamplerParams& s = out.sampler;
  s.start = 0;
  s.end = 65535;
  if (info.frames == 0) {
    return;
  }
  const unsigned frame = wavetable_frame(info.frames, info.wt_frame, wavetable);
  if (frame != 0u) {  // a wavetable: the stack of windows, no loop, no slices (the engine plays the current window as a loop of its own)
    make_wavetable_fields(s, frame);
    return;
  }
  if (info.has_loop) {
    const std::uint16_t a = position(info.loop_start, info.frames);
    const std::uint16_t b = position(info.loop_end, info.frames);
    if (a < b) {
      s.loop_start = a;
      s.loop_end = b;
      s.play_mode = info.loop_type == LoopType::Forward ? 1 : (info.loop_type == LoopType::Backward ? 2 : 3);
    }
  }
  unsigned count = 0;
  std::uint16_t last = 0;
  for (unsigned i = 0; i < info.slice_count && count < model::kSlices; ++i) {
    const std::uint16_t p = position(info.slices[i], info.frames);
    if (p != 0 && p != last) {  // two cue points the 16-bit positions cannot tell apart are one slice
      s.slices[count++] = p;
      last = p;
    }
  }
  s.slice_count = static_cast<std::uint8_t>(count);
}

void SampleReader::fail(FormatError e, const char* detail) noexcept {
  progress_ = formats::Progress::Failed;
  report_.error = e;
  report_.detail = detail;
  report_.offset = report_.bytes;
}

void SampleReader::begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept {
  report_ = FormatReport{};
  project_ = &arena.project;
  slot_ = params.index;
  wavetable_ = params.wavetable;
  progress_ = formats::Progress::Running;
  name_[0] = '\0';
  if (params.name != nullptr) {
    std::strncpy(name_, params.name, sizeof name_ - 1);  // make_sample_instrument cuts the extension, then the length
    name_[sizeof name_ - 1] = '\0';
  }
  if (params.index >= model::kMaxInstruments) {
    fail(FormatError::LimitExceeded, "the instrument slot does not exist");
    return;
  }
  if (params.pcm == nullptr || params.pcm_capacity == 0) {
    fail(FormatError::LimitExceeded, "no room was given for the audio");
    return;
  }
  importer_.begin(source, params.pcm, params.pcm_capacity, params.keep_stereo, false, params.channel, params.side);
}

formats::Progress SampleReader::step(std::size_t byte_budget) noexcept {
  if (progress_ != formats::Progress::Running) {
    return progress_;
  }
  const wav::Progress p = importer_.step(byte_budget);
  const wav::Report& r = importer_.report();
  report_.bytes = r.bytes;
  if (p == wav::Progress::Running) {
    return progress_;
  }
  if (p == wav::Progress::Failed) {
    progress_ = formats::Progress::Failed;
    report_.error = map_error(r.error);
    report_.detail = r.detail;
    report_.offset = r.offset;
    return progress_;
  }
  finish();
  return progress_;
}

void SampleReader::finish() noexcept {
  const wav::Info& info = importer_.info();
  const wav::Report& r = importer_.report();
  model::Instrument inst;
  make_sample_instrument(info, slot_, name_, inst, wavetable_);
  bool unusable = false;
  const unsigned frame = wavetable_frame(info.frames, info.wt_frame, wavetable_, &unusable);
  const unsigned tail = frame != 0u ? info.frames % frame : 0u;  // frames after the last whole window: the engine does not play them
  if (project_->set_instrument(slot_, inst) != model::Status::Ok) {
    fail(FormatError::Internal, "the model refused the instrument");
    return;
  }
  report_.instruments = 1;
  report_.pcm_frames = info.frames;
  report_.pcm_channels = info.channels;
  report_.pcm_side = info.side;
  // What the file had that is not in the instrument: never silent.
  report_.sanitised = (r.smpl_ignored ? 1u : 0u) + (r.cue_ignored ? 1u : 0u) + r.loops_dropped + r.slices_dropped + (r.clm_ignored ? 1u : 0u) + (unusable ? 1u : 0u) + (tail != 0u ? 1u : 0u) +
                      (frame != 0u && (info.has_loop || info.slice_count != 0u) ? 1u : 0u);  // a wavetable has no loop or slices of the file's
  report_.unverified_version = false;
  progress_ = formats::Progress::Done;
}

void SampleReader::cancel() noexcept {
  if (progress_ == formats::Progress::Running) {
    importer_.cancel();
    fail(FormatError::Cancelled, "cancelled");  // nothing was applied to the model
  }
}

}  // namespace o3os::formats::wav
