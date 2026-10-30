// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/wav.h>

#include <cstring>

namespace o3os::formats::wav {

namespace {

constexpr std::uint64_t kUnknownEnd = ~std::uint64_t{0};

constexpr std::uint32_t four(char a, char b, char c, char d) noexcept {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

constexpr std::uint32_t kRiff = four('R', 'I', 'F', 'F');
constexpr std::uint32_t kRifx = four('R', 'I', 'F', 'X');
constexpr std::uint32_t kRf64 = four('R', 'F', '6', '4');
constexpr std::uint32_t kBw64 = four('B', 'W', '6', '4');
constexpr std::uint32_t kWave = four('W', 'A', 'V', 'E');
constexpr std::uint32_t kFmt = four('f', 'm', 't', ' ');
constexpr std::uint32_t kData = four('d', 'a', 't', 'a');
constexpr std::uint32_t kSmpl = four('s', 'm', 'p', 'l');
constexpr std::uint32_t kCue = four('c', 'u', 'e', ' ');
constexpr std::uint32_t kClm = four('c', 'l', 'm', ' ');  // OS-GRAN: Serum's wavetable marker

constexpr std::uint16_t kTagPcm = 1;
constexpr std::uint16_t kTagFloat = 3;
constexpr std::uint16_t kTagExtensible = 0xFFFE;
constexpr std::size_t kFmtRead = 40;  // the part of a fmt chunk that is interpreted (WAVEFORMATEXTENSIBLE)

// The 14 bytes of the standard sub-format GUID that follow the two-byte format tag.
constexpr std::uint8_t kGuidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

std::uint16_t le16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t le32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint32_t be32(const std::uint8_t* p) noexcept {
  return four(static_cast<char>(p[0]), static_cast<char>(p[1]), static_cast<char>(p[2]), static_cast<char>(p[3]));
}

std::int16_t clamp16(std::int32_t v) noexcept {
  return static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

// Triangular dither of +-1 LSB of the 16-bit result in 24-bit units (-255..255), from one xorshift32 step.
std::int32_t tpdf(std::uint32_t& rng) noexcept {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return static_cast<std::int32_t>((rng >> 8) & 0xFFu) + static_cast<std::int32_t>((rng >> 20) & 0xFFu) - 255;
}

// Float bits to int16. NaN reads as 0, infinities and overs clip; a value that is exactly a 16-bit value passes untouched;
// anything else is rounded with TPDF dither of +-1 LSB.
std::int16_t from_float(std::uint32_t bits, std::uint32_t& rng, bool& dithered) noexcept {
  if ((bits & 0x7F800000u) == 0x7F800000u) {
    if ((bits & 0x007FFFFFu) != 0) {
      return 0;
    }
    return (bits >> 31) != 0 ? std::int16_t{-32768} : std::int16_t{32767};
  }
  float x;
  std::memcpy(&x, &bits, sizeof x);
  const float s = x * 32768.0f;
  if (s >= 32767.0f) {
    return 32767;
  }
  if (s <= -32768.0f) {
    return -32768;
  }
  const std::int32_t whole = static_cast<std::int32_t>(s);  // toward zero; |s| < 32768 here
  if (static_cast<float>(whole) == s) {
    return static_cast<std::int16_t>(whole);  // nothing to lose
  }
  dithered = true;
  const float t = s + static_cast<float>(tpdf(rng)) * (1.0f / 256.0f);
  return clamp16(static_cast<std::int32_t>(t + (t >= 0.0f ? 0.5f : -0.5f)));
}

// A sample of the file as a 24-bit value (+-2^23), for the downmix: 8-bit and 16-bit are exact in it, float is rounded.
std::int32_t wide_pcm8(std::uint8_t v) noexcept { return (static_cast<std::int32_t>(v) - 128) * 65536; }
std::int32_t wide_pcm16(const std::uint8_t* p) noexcept { return static_cast<std::int16_t>(le16(p)) * 256; }
std::int32_t wide_pcm24(const std::uint8_t* p) noexcept {
  const std::uint32_t u = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                          (static_cast<std::uint32_t>(p[2]) << 16);
  return static_cast<std::int32_t>(u << 8) >> 8;  // sign-extended
}
std::int32_t wide_float(std::uint32_t bits) noexcept {
  if ((bits & 0x7F800000u) == 0x7F800000u) {
    return (bits & 0x007FFFFFu) != 0 ? 0 : ((bits >> 31) != 0 ? -8388608 : 8388607);
  }
  float x;
  std::memcpy(&x, &bits, sizeof x);
  const float s = x * 8388608.0f;
  if (s >= 8388607.0f) {
    return 8388607;
  }
  if (s <= -8388608.0f) {
    return -8388608;
  }
  return static_cast<std::int32_t>(s + (s >= 0.0f ? 0.5f : -0.5f));
}

// A 24-bit value to int16: exact where the low byte is zero, else rounded with TPDF dither.
std::int16_t quantise24(std::int32_t w, std::uint32_t& rng, bool& dithered) noexcept {
  if ((w & 0xFF) == 0) {
    return static_cast<std::int16_t>(w >> 8);
  }
  dithered = true;
  return clamp16((w + 128 + tpdf(rng)) >> 8);
}

}  // namespace

const char* error_name(Error e) noexcept {
  switch (e) {
    case Error::None: return "none";
    case Error::Truncated: return "truncated";
    case Error::BadMagic: return "bad magic";
    case Error::Unsupported: return "unsupported";
    case Error::LimitExceeded: return "limit exceeded";
    case Error::Malformed: return "malformed";
    case Error::SourceFailed: return "source failed";
    case Error::Cancelled: return "cancelled";
  }
  return "?";
}

void Importer::begin(core::ByteSource& source, std::int16_t* dst, std::size_t capacity_samples,
                     bool keep_stereo, bool clip, std::uint8_t channel, bool side) noexcept {
  source_ = &source;
  dst_ = dst;
  capacity_ = dst != nullptr ? capacity_samples : 0;
  state_ = State::Riff;
  progress_ = Progress::Running;
  report_ = Report{};
  info_ = Info{};
  budget_ = 0;
  want_ = 0;
  have_ = 0;
  pos_ = 0;
  riff_end_ = kUnknownEnd;
  chunk_id_ = 0;
  chunk_size_ = 0;
  chunk_left_ = 0;
  skip_left_ = 0;
  fmt_seen_ = false;
  data_seen_ = false;
  float_ = false;
  keep_stereo_ = keep_stereo;
  want_side_ = side && !keep_stereo;
  side_ = false;
  side_off_ = 0;
  clip_ = clip && !want_side_;
  clipped_ = false;
  downmix_ = false;
  pick_ = channel <= 2u && !want_side_ ? channel : std::uint8_t{0};
  out_channels_ = 0;
  bps_ = 0;
  unit_ = 0;
  convert_left_ = 0;
  data_samples_ = 0;
  out_ = 0;
  carry_ = 0;
  rng_ = 0x2545F491u;  // fixed seed: the output is a pure function of the input
  clm_seen_ = false;
  smpl_seen_ = false;
  loop_seen_ = false;
  loop_type_raw_ = 0;
  loop_start_raw_ = 0;
  loop_end_raw_ = 0;
  cue_left_ = 0;
  cue_count_ = 0;
}

unsigned Importer::permille() const noexcept {
  if (progress_ == Progress::Done) {
    return 1000;
  }
  if (!data_seen_ || data_samples_ == 0) {
    return 0;
  }
  unsigned shift = 0;  // keep out_ * 1000 inside 32 bits for any length
  while ((data_samples_ >> shift) > 4194303u) {
    ++shift;
  }
  const unsigned p = (out_ >> shift) * 1000u / (data_samples_ >> shift);
  return p > 999u ? 999u : p;
}

void Importer::cancel() noexcept {
  if (progress_ == Progress::Running) {
    fail(Error::Cancelled, "cancelled");
  }
}

void Importer::fail(Error e, const char* detail) noexcept {
  state_ = State::Failed;
  progress_ = Progress::Failed;
  report_.error = e;
  report_.detail = detail;
  report_.offset = pos_ > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(pos_);
}

Importer::Pull Importer::read_some(void* dst, std::size_t ask, std::size_t& got) noexcept {
  got = 0;
  const core::IoResult r = source_->read(dst, ask);
  switch (r.status) {
    case core::IoStatus::Ok:
      if (r.count == 0 || r.count > ask) {
        fail(Error::SourceFailed, "the source broke its contract");
        return Pull::Error;
      }
      got = r.count;
      budget_ -= got;
      pos_ += got;
      return Pull::Complete;
    case core::IoStatus::WouldBlock:
      budget_ = 0;  // nothing now: the step ends and the next pass asks again
      return Pull::Partial;
    case core::IoStatus::End:
      return Pull::End;
    case core::IoStatus::Failed:
      break;
  }
  fail(Error::SourceFailed, "the source failed");
  return Pull::Error;
}

// Reads until scratch_ holds `want` bytes of the current record. True when it does; false when the step must end (not enough
// budget or the source would block), the source ended (end_of_source decided what that means) or it failed.
bool Importer::take(std::size_t want) noexcept {
  want_ = want;
  while (have_ < want) {
    if (budget_ == 0) {
      return false;
    }
    std::size_t ask = want - have_;
    ask = ask > budget_ ? budget_ : ask;
    std::size_t got = 0;
    switch (read_some(scratch_ + have_, ask, got)) {
      case Pull::Complete:
        have_ += got;
        break;
      case Pull::Partial:
        return false;
      case Pull::End:
        end_of_source();
        return false;
      case Pull::Error:
        return false;
    }
  }
  have_ = 0;  // the caller now owns the record in scratch_
  return true;
}

// The source ended where more was wanted. After a complete data chunk that is the end of the useful file; before it the
// file is cut.
void Importer::end_of_source() noexcept {
  if (data_complete()) {
    // A clean end between chunks, or a missing final pad byte, is normal; anything else is a chunk cut short.
    if (!((state_ == State::ChunkHead && have_ == 0) || state_ == State::Pad)) {
      report_.trailing_ignored = true;
    }
    finish();
  } else {
    fail(Error::Truncated, data_seen_ ? "the audio is cut short" : "the file ends before its audio");
  }
}

void Importer::finish() noexcept {
  if (!fmt_seen_) {
    fail(Error::Malformed, "no fmt chunk");
    return;
  }
  if (!data_seen_) {
    fail(Error::Malformed, "no data chunk");
    return;
  }
  const std::uint32_t frames = info_.frames;
  if (loop_seen_) {
    // The file's loop end is the last frame played (inclusive). A writer that stores one past it (the frame count) is
    // read as the whole sample rather than refused.
    std::uint64_t end = static_cast<std::uint64_t>(loop_end_raw_) + 1u;
    end = (end == static_cast<std::uint64_t>(frames) + 1u) ? frames : end;
    if (loop_type_raw_ > 2u || loop_start_raw_ >= end || end > frames) {
      report_.smpl_ignored = true;
    } else {
      info_.has_loop = true;
      info_.loop_type = static_cast<LoopType>(loop_type_raw_);
      info_.loop_start = loop_start_raw_;
      info_.loop_end = static_cast<std::uint32_t>(end);
    }
  }
  info_.slice_count = 0;
  for (unsigned i = 0; i < cue_count_; ++i) {
    if (cues_[i] > 0 && cues_[i] < frames) {
      info_.slices[info_.slice_count++] = cues_[i];
    }
  }
  state_ = State::Done;
  progress_ = Progress::Done;
}

void Importer::enter_skip(std::uint32_t n) noexcept {
  chunk_left_ = 0;
  skip_left_ = n;
  state_ = State::Skip;
}

void Importer::end_chunk() noexcept {
  if (chunk_left_ != 0) {
    enter_skip(chunk_left_);
  } else {
    after_chunk();
  }
}

void Importer::after_chunk() noexcept {
  // RIFF chunks are padded to an even size; a writer that leaves the pad off at the very end of the RIFF is accepted.
  if ((chunk_size_ & 1u) != 0 && (riff_end_ == kUnknownEnd || pos_ < riff_end_)) {
    state_ = State::Pad;
  } else {
    next_chunk();
  }
}

void Importer::next_chunk() noexcept {
  if (riff_end_ != kUnknownEnd && pos_ + 8u > riff_end_) {
    finish();  // the RIFF is over (and so is anything after it)
  } else {
    state_ = State::ChunkHead;
  }
}

Progress Importer::step(std::size_t byte_budget) noexcept {
  if (progress_ != Progress::Running) {
    return progress_;
  }
  budget_ = byte_budget != 0 ? byte_budget : 1u;
  while (progress_ == Progress::Running) {
    switch (state_) {
      case State::Riff:
        if (!take(12)) {
          break;
        }
        on_riff();
        break;
      case State::ChunkHead:
        if (!take(8)) {
          break;
        }
        on_chunk_head();
        break;
      case State::Fmt:
        if (!take(chunk_size_ < kFmtRead ? chunk_size_ : kFmtRead)) {
          break;
        }
        chunk_left_ -= static_cast<std::uint32_t>(want_);
        on_fmt();
        if (progress_ == Progress::Running) {
          end_chunk();
        }
        break;
      case State::SmplHead:
        if (!take(36)) {
          break;
        }
        chunk_left_ -= 36;
        on_smpl_head();
        break;
      case State::SmplLoop:
        if (!take(24)) {
          break;
        }
        chunk_left_ -= 24;
        on_smpl_loop();
        end_chunk();
        break;
      case State::CueHead:
        if (!take(4)) {
          break;
        }
        chunk_left_ -= 4;
        on_cue_head();
        break;
      case State::CueEntry:
        if (!take(24)) {
          break;
        }
        chunk_left_ -= 24;
        on_cue_entry();
        break;
      case State::Clm: {
        const std::size_t n = chunk_size_ < 16u ? chunk_size_ : 16u;  // "<!>2048 20000000": the frame size is in the first 8 bytes
        if (!take(n)) {
          break;
        }
        chunk_left_ -= static_cast<std::uint32_t>(want_);
        on_clm();
        end_chunk();
        break;
      }
      case State::Data:
        step_data();
        break;
      case State::Skip: {
        if (skip_left_ == 0) {
          after_chunk();
          break;
        }
        if (budget_ == 0) {
          break;
        }
        std::size_t ask = skip_left_ < kScratchBytes ? skip_left_ : kScratchBytes;
        ask = ask > budget_ ? budget_ : ask;
        std::size_t got = 0;
        const Pull p = read_some(scratch_, ask, got);
        if (p == Pull::Complete) {
          skip_left_ -= static_cast<std::uint32_t>(got);
        } else if (p == Pull::End) {
          end_of_source();
        }
        break;
      }
      case State::Pad:
        if (!take(1)) {
          break;
        }
        next_chunk();
        break;
      case State::Done:
      case State::Failed:
        break;
    }
    if (budget_ == 0 && progress_ == Progress::Running) {
      break;  // spent (or the source would block): the next pass goes on
    }
  }
  report_.bytes = pos_ > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(pos_);
  return progress_;
}

void Importer::on_riff() noexcept {
  const std::uint32_t id = be32(scratch_);
  const std::uint32_t size = le32(scratch_ + 4);
  const std::uint32_t form = be32(scratch_ + 8);
  if (id == kRifx || id == kRf64 || id == kBw64) {
    fail(Error::Unsupported, "RIFX, RF64 and BW64 files are not supported");
    return;
  }
  if (id != kRiff || form != kWave) {
    fail(Error::BadMagic, id != kRiff ? "not a RIFF file" : "not a WAVE file");
    return;
  }
  if (size == 0 || size == 0xFFFFFFFFu) {
    riff_end_ = kUnknownEnd;  // a streaming writer that never went back to fill the size in
  } else if (size < 4) {
    fail(Error::Malformed, "the RIFF is too small");
    return;
  } else {
    riff_end_ = static_cast<std::uint64_t>(size) + 8u;
  }
  chunk_size_ = 0;
  next_chunk();
}

void Importer::on_chunk_head() noexcept {
  chunk_id_ = be32(scratch_);
  chunk_size_ = le32(scratch_ + 4);
  chunk_left_ = chunk_size_;
  if (riff_end_ != kUnknownEnd && chunk_size_ > riff_end_ - pos_) {
    if (data_complete()) {
      report_.trailing_ignored = true;
      finish();
    } else {
      fail(Error::Malformed, "a chunk is longer than the RIFF");
    }
    return;
  }
  switch (chunk_id_) {
    case kFmt:
      if (fmt_seen_) {
        fail(Error::Malformed, "a second fmt chunk");
      } else if (chunk_size_ < 16) {
        fail(Error::Malformed, "the fmt chunk is too short");
      } else {
        state_ = State::Fmt;
      }
      return;
    case kData: {
      if (!fmt_seen_) {
        fail(Error::Malformed, "the data chunk comes before fmt");
        return;
      }
      if (data_seen_) {
        fail(Error::Malformed, "a second data chunk");
        return;
      }
      const std::uint32_t block = static_cast<std::uint32_t>(info_.source_channels) * bps_;
      std::uint32_t frames = chunk_size_ / block;
      if (frames == 0) {
        fail(Error::Malformed, "the data chunk holds no frame");
        return;
      }
      std::uint64_t samples = static_cast<std::uint64_t>(frames) * out_channels_;
      if (samples > capacity_ && clip_ && capacity_ >= out_channels_) {
        frames = static_cast<std::uint32_t>(capacity_ / out_channels_);  // OS-FU2: the first frames that fit
        samples = static_cast<std::uint64_t>(frames) * out_channels_;
        clipped_ = true;
      }
      if (samples > capacity_) {
        fail(Error::LimitExceeded, "the audio does not fit the sample buffer");  // nothing was written
        return;
      }
      if (side_) {  // OS-STEREO (b): S follows M at the even index after it and never clips: both fit, or the import fails before a byte is written
        side_off_ = (frames + 1u) & ~1u;
        if (2ull * side_off_ > capacity_) {
          fail(Error::LimitExceeded, "the audio and its side signal do not fit the sample buffer");
          return;
        }
        info_.side = true;
        info_.side_offset = side_off_;
      }
      data_seen_ = true;
      data_samples_ = static_cast<std::uint32_t>(samples);
      convert_left_ = frames * block;
      state_ = State::Data;
      return;
    }
    case kSmpl:
      if (chunk_size_ < 36 || smpl_seen_) {
        report_.smpl_ignored = true;
        enter_skip(chunk_size_);
      } else {
        smpl_seen_ = true;
        state_ = State::SmplHead;
      }
      return;
    case kCue:
      if (chunk_size_ < 4) {
        report_.cue_ignored = true;
        enter_skip(chunk_size_);
      } else {
        state_ = State::CueHead;
      }
      return;
    case kClm:
      if (chunk_size_ < 4 || clm_seen_) {
        report_.clm_ignored = true;
        enter_skip(chunk_size_);
      } else {
        clm_seen_ = true;
        state_ = State::Clm;
      }
      return;
    default:
      enter_skip(chunk_size_);
      return;
  }
}

// "<!>" then the frame size in decimal (Serum writes "<!>2048 20000000 wavetable (www.xferrecords.com)"): a number of 1..65535, else the chunk is dropped and reported.
void Importer::on_clm() noexcept {
  const std::size_t n = want_;
  std::uint32_t frame = 0;
  std::size_t i = 3;
  if (n >= 4 && scratch_[0] == '<' && scratch_[1] == '!' && scratch_[2] == '>') {
    while (i < n && scratch_[i] >= '0' && scratch_[i] <= '9' && frame <= 65535u) {
      frame = frame * 10u + static_cast<std::uint32_t>(scratch_[i] - '0');
      ++i;
    }
  }
  if (i == 3 || frame == 0 || frame > 65535u) {
    report_.clm_ignored = true;
    return;
  }
  info_.wt_frame = static_cast<std::uint16_t>(frame);
}

void Importer::on_fmt() noexcept {
  const std::uint8_t* f = scratch_;
  std::uint16_t tag = le16(f);
  const std::uint16_t channels = le16(f + 2);
  const std::uint32_t rate = le32(f + 4);
  const std::uint16_t block_align = le16(f + 12);
  const std::uint16_t bits = le16(f + 14);
  if (tag == kTagExtensible) {
    if (chunk_size_ < kFmtRead || le16(f + 16) < 22) {
      fail(Error::Malformed, "the extensible fmt chunk is too short");
      return;
    }
    if (std::memcmp(f + 26, kGuidTail, sizeof kGuidTail) != 0) {
      fail(Error::Unsupported, "an extensible sub-format that is not PCM or float");
      return;
    }
    tag = le16(f + 24);
  }
  if (tag != kTagPcm && tag != kTagFloat) {
    fail(Error::Unsupported, "only PCM and float WAV files are supported");
    return;
  }
  if (channels == 0 || rate == 0 || bits == 0) {
    fail(Error::Malformed, "the fmt chunk has a zero channel count, rate or depth");
    return;
  }
  if (channels > 2) {
    fail(Error::Unsupported, "more than two channels");
    return;
  }
  if (tag == kTagPcm && bits != 8 && bits != 16 && bits != 24) {
    fail(Error::Unsupported, bits == 32 ? "32-bit integer PCM" : "an unusual PCM depth");
    return;
  }
  if (tag == kTagFloat && bits != 32) {
    fail(bits == 64 ? Error::Unsupported : Error::Malformed, bits == 64 ? "64-bit float" : "a float depth that is not 32");
    return;
  }
  const unsigned bytes = bits / 8u;
  if (block_align != channels * bytes) {
    fail(Error::Malformed, "the block alignment does not match the format");
    return;
  }
  fmt_seen_ = true;
  float_ = tag == kTagFloat;
  bps_ = static_cast<std::uint8_t>(bytes);
  info_.sample_rate = rate;
  info_.source_channels = static_cast<std::uint8_t>(channels);
  downmix_ = channels == 2 && !keep_stereo_;
  out_channels_ = downmix_ ? std::uint8_t{1} : static_cast<std::uint8_t>(channels);
  unit_ = downmix_ ? static_cast<std::uint8_t>(2u * bytes) : static_cast<std::uint8_t>(bytes);
  info_.channels = out_channels_;
  info_.downmixed = downmix_;
  side_ = want_side_ && downmix_;
  info_.bits = static_cast<std::uint8_t>(bits);
  info_.format = float_ ? SampleFormat::Float : SampleFormat::Pcm;
}

void Importer::on_smpl_head() noexcept {
  const std::uint32_t unity = le32(scratch_ + 12);
  const std::uint32_t loops = le32(scratch_ + 28);
  if (unity > 127u || 36u + 24ull * loops > chunk_size_) {
    report_.smpl_ignored = true;  // inconsistent: none of it is used
    end_chunk();
    return;
  }
  info_.root_note = static_cast<std::uint8_t>(unity);
  if (loops == 0) {
    end_chunk();
    return;
  }
  report_.loops_dropped += loops - 1u;
  state_ = State::SmplLoop;
}

void Importer::on_smpl_loop() noexcept {
  loop_seen_ = true;
  loop_type_raw_ = le32(scratch_ + 4);
  loop_start_raw_ = le32(scratch_ + 8);
  loop_end_raw_ = le32(scratch_ + 12);
}

void Importer::on_cue_head() noexcept {
  const std::uint32_t count = le32(scratch_);
  if (4u + 24ull * count > chunk_size_) {
    report_.cue_ignored = true;
    end_chunk();
    return;
  }
  cue_left_ = count;
  if (count == 0) {
    end_chunk();
  } else {
    state_ = State::CueEntry;
  }
}

void Importer::on_cue_entry() noexcept {
  add_cue(le32(scratch_ + 20));  // dwSampleOffset
  if (--cue_left_ == 0) {
    end_chunk();
  }
}

// Keeps the lowest kMaxSlices distinct offsets, ascending. Offsets that are not inside the sample are removed at the end
// (the frame count may not be known yet), and they are the highest, so they are the first to be pushed out.
void Importer::add_cue(std::uint32_t offset) noexcept {
  unsigned i = 0;
  while (i < cue_count_ && cues_[i] < offset) {
    ++i;
  }
  if (i < cue_count_ && cues_[i] == offset) {
    return;  // the same place again
  }
  if (cue_count_ == kMaxSlices) {
    ++report_.slices_dropped;
    if (i == kMaxSlices) {
      return;  // higher than everything kept
    }
    --cue_count_;  // the highest one makes room
  }
  for (unsigned k = cue_count_; k > i; --k) {
    cues_[k] = cues_[k - 1];
  }
  cues_[i] = offset;
  ++cue_count_;
}

bool Importer::step_data() noexcept {
  if (convert_left_ == 0) {
    if (clipped_) {  // OS-FU2: the rest of the file is not read (it may be tens of megabytes)
      report_.trailing_ignored = true;
      finish();
      return true;
    }
    report_.trailing_audio_bytes = chunk_left_ != 0;
    end_chunk();  // the bytes after the last whole frame are skipped
    return true;
  }
  if (budget_ == 0) {
    return false;
  }
  std::size_t ask = kScratchBytes - carry_;
  ask = ask > budget_ ? budget_ : ask;
  ask = ask > convert_left_ ? convert_left_ : ask;
  std::size_t got = 0;
  switch (read_some(scratch_ + carry_, ask, got)) {
    case Pull::Complete:
      break;
    case Pull::Partial:
      return false;
    case Pull::End:
      end_of_source();
      return false;
    case Pull::Error:
      return false;
  }
  convert_left_ -= static_cast<std::uint32_t>(got);
  chunk_left_ -= static_cast<std::uint32_t>(got);
  const std::size_t total = carry_ + got;
  const std::size_t whole = total - total % unit_;
  if (downmix_) {
    convert_downmix(scratch_, whole / unit_);
  } else {
    convert(scratch_, whole / unit_);
  }
  carry_ = static_cast<std::uint8_t>(total - whole);
  if (carry_ != 0) {
    std::memmove(scratch_, scratch_ + whole, carry_);
  }
  info_.frames = out_ / out_channels_;
  return true;
}

// Converts `frames` whole stereo frames at `src` to mono int16 at dst_[out_...]: the mean (or the channel pick_ chose) at 24-bit precision, then
// quantised (dithered where it is not a whole 16-bit step).
void Importer::convert_downmix(const std::uint8_t* src, std::size_t frames) noexcept {
  std::int16_t* out = dst_ + out_;
  std::int16_t* const side = side_ ? dst_ + side_off_ + out_ : nullptr;  // (the output is mono here: out_ counts frames)
  bool dithered = false;
  for (std::size_t i = 0; i < frames; ++i) {
    std::int32_t l;
    std::int32_t r;
    switch (bps_) {
      case 1:
        l = wide_pcm8(src[2 * i]);
        r = wide_pcm8(src[2 * i + 1]);
        break;
      case 2:
        l = wide_pcm16(src + 4 * i);
        r = wide_pcm16(src + 4 * i + 2);
        break;
      case 3:
        l = wide_pcm24(src + 6 * i);
        r = wide_pcm24(src + 6 * i + 3);
        break;
      default:  // float32
        l = wide_float(le32(src + 8 * i));
        r = wide_float(le32(src + 8 * i + 4));
        break;
    }
    // round half up: |l + r| < 2^24, no overflow; OS-LOADCH: or one channel alone, at the same 24-bit precision (a 16-bit file passes bit-exact)
    out[i] = quantise24(pick_ == 1u ? l : (pick_ == 2u ? r : (l + r + 1) >> 1), rng_, dithered);
    if (side != nullptr) {  // S = L - M, L rounded to 16 bits with no dither (the mid's dither stream is untouched): L = M + S exactly unless the sum clamps (S3 of docs/specs/OS-STEREO.md)
      side[i] = clamp16(((l + 128) >> 8) - out[i]);
    }
  }
  info_.dithered = info_.dithered || dithered;
  out_ += static_cast<std::uint32_t>(frames);
}

// Converts `samples` whole samples at `src` to int16 at dst_[out_...]; room was checked when the data chunk began.
void Importer::convert(const std::uint8_t* src, std::size_t samples) noexcept {
  std::int16_t* out = dst_ + out_;
  bool dithered = false;
  switch (bps_) {
    case 1:
      for (std::size_t i = 0; i < samples; ++i) {
        out[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(src[i]) - 128) * 256);
      }
      break;
    case 2:
      for (std::size_t i = 0; i < samples; ++i) {
        out[i] = static_cast<std::int16_t>(le16(src + 2 * i));
      }
      break;
    case 3:
      for (std::size_t i = 0; i < samples; ++i) {
        const std::uint8_t* p = src + 3 * i;
        const std::uint32_t u = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                (static_cast<std::uint32_t>(p[2]) << 16);
        const std::int32_t s = static_cast<std::int32_t>(u << 8) >> 8;  // sign-extended 24-bit value
        if ((s & 0xFF) == 0) {
          out[i] = static_cast<std::int16_t>(s >> 8);  // nothing below the 16-bit step: exact, no dither
        } else {
          dithered = true;
          out[i] = clamp16((s + 128 + tpdf(rng_)) >> 8);
        }
      }
      break;
    default:  // 4: float32
      for (std::size_t i = 0; i < samples; ++i) {
        out[i] = from_float(le32(src + 4 * i), rng_, dithered);
      }
      break;
  }
  info_.dithered = info_.dithered || dithered;
  out_ += static_cast<std::uint32_t>(samples);
}

}  // namespace o3os::formats::wav
