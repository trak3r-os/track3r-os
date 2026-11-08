// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/og.h>

#include "og_codec.h"

#include <algorithm>
#include <cstring>

namespace o3os::formats::og {

using namespace codec;  // NOLINT: the shared field access and layouts

namespace {

constexpr std::uint8_t kFw[4] = {1, 9, 0, 1};  // firmware 1.9.0.1, what Export writes (docs/formats/og.md section 5)
constexpr std::uint8_t kInstrumentVersion = 9;
constexpr std::uint16_t kStructBytes = 372;     // the header's size field: the sampler struct
constexpr std::uint32_t kMaxPcmBytes = 2u * kMaxPcmFrames;

// The image of an instrument file as the model's Instrument. Out-of-range values that the model cannot hold are
// replaced by the instrument defaults and counted; everything else is kept
// as written, to be sanitised where it is used.
class Decoder {
 public:
  Decoder(const std::uint8_t* image, std::uint8_t version, std::uint16_t slot, std::uint32_t& counter) noexcept
      : b_(image), version_(version), slot_(slot), count_(counter) {}

  void run(model::Instrument& out) noexcept {
    model::init_instrument(out, model::InstrumentKind::Sample);
    // name: the sample's name; the path is ours, so nothing in the file can name a place outside the project
    const std::size_t n = field_text_len(b_ + instr::kFileName, instr::kFileNameLen, instr::kFileNameLen - 1);
    std::memcpy(out.name, b_ + instr::kFileName, n);
    const char* stem = "samples/instr";
    std::size_t at = std::strlen(stem);
    std::memcpy(out.sample_path, stem, at);
    out.sample_path[at++] = static_cast<char>('0' + (slot_ + 1u) / 10u);
    out.sample_path[at++] = static_cast<char>('0' + (slot_ + 1u) % 10u);
    std::memcpy(out.sample_path + at, ".wav", 4);

    model::SamplerParams& s = out.sampler;
    s.sample_type = limited(b_[instr::kSampleType], 1, 0);
    s.wavetable_window_size = get16(b_ + instr::kWindowSize);
    s.play_mode = b_[instr::kPlayMode];
    if (version_ == 1 && s.play_mode > 4) {  // version 1 had one play mode fewer
      ++s.play_mode;
    }
    s.play_mode = limited(s.play_mode, 7, 0);
    s.start = get16(b_ + instr::kStart);
    s.loop_start = get16(b_ + instr::kLoopStart);
    s.loop_end = get16(b_ + instr::kLoopEnd);
    s.end = get16(b_ + instr::kEnd);
    s.wavetable_window = get32(b_ + instr::kCurrentWindow);
    for (std::uint32_t i = 0; i < model::kEnvelopes; ++i) {
      const std::uint8_t* e = b_ + instr::kEnvelope + i * instr::kEnvelopeStride;
      model::Envelope& v = s.envelope[i];
      v.amount = finite(get_f32(e), 1.0f);
      v.delay = get16(e + 4);
      v.attack = get16(e + 6);
      v.hold = get16(e + 8);
      v.decay = get16(e + 10);
      v.sustain = finite(get_f32(e + 12), 1.0f);
      v.release = get16(e + 16);
      v.loop = e[18];
      v.enable = e[19];
    }
    if (version_ <= 2) {  // the fine-tune envelope arrived with version 3: the defaults of 1.1
      model::Envelope& v = s.envelope[model::kEnvelopes - 1];
      v = model::Envelope{1.0f, 0, 3000, 0, 0, 1.0f, 1000, 0, 0};
    }
    for (std::uint32_t i = 0; i < model::kLfos; ++i) {
      const std::uint8_t* l = b_ + instr::kLfo + i * instr::kLfoStride;
      const bool wt = model::is_wt_loop(s.envelope[i].loop);  // OS-CEWTENV W1 (W3a review: tested exactly, only 2 and 3 exist — a garbage loop byte is not a WT slot): loop 2 WT Env, 3 WT LFO (verified parity, tracker-ce@7fd6c2c:core/source/mtStructs.h:629-637): the shape is the contour 0..255, verbatim, and bytes +2/+3 are the warp and sustain, read
      s.lfo[i].shape = wt ? l[0] : (l[0] > 4u ? model::kLfoShapeTriangle : l[0]);  // OS-LFOSINE: for loop <= 1 the OG has the shapes 0..4; a larger byte played as Triangle here and still does (5 is this OS's Sine, never an OG value). (field by field: an aggregate would leave bytes of an Lfo indeterminate, and equal instruments are equal bytes)
      s.lfo[i].speed = l[1];
      s.lfo[i].wt_bend = wt ? l[2] : std::uint8_t{0};  // for loop <= 1 today's rule holds: bytes +2/+3 are dropped
      s.lfo[i].wt_sust = wt ? l[3] : std::uint8_t{0};
      s.lfo[i].amount = finite(get_f32(l + 4), 0.5f);
    }
    s.cutoff = finite(get_f32(b_ + instr::kCutoff), 1.0f);
    s.resonance = finite(get_f32(b_ + instr::kResonance), 0.0f);
    s.filter_type = limited(b_[instr::kFilterType], 2, 0);
    s.filter_enable = b_[instr::kFilterEnable];
    s.tune = static_cast<std::int8_t>(b_[instr::kTune]);
    s.fine_tune = static_cast<std::int8_t>(b_[instr::kFineTune]);
    s.volume = limited(b_[instr::kVolume], model::kMaxPercent, 50);
    s.glide = get16(b_ + instr::kGlide);
    const std::uint16_t pan = get16(b_ + instr::kPanning);  // an i16 in the file; 0..100 is the range this OS accepts
    s.panning = pan > model::kMaxPercent ? replaced(50) : static_cast<std::uint8_t>(pan);
    s.delay_send = limited(b_[instr::kDelaySend], model::kMaxPercent, 0);
    out.slice_thru = b_[instr::kPlayThru] != 0 ? std::uint8_t{1} : std::uint8_t{0};  // OS-CEPLAYTHRU P1: any non-zero byte is On, in every version (verified parity, tracker-ce@7fd6c2c:core/FileManager/fileManagerInstrument.cpp:663-706 reads the struct raw, with no version rule for it); byte 279 was alignment padding before
    for (std::uint32_t i = 0; i < model::kSlices; ++i) {
      s.slices[i] = get16(b_ + instr::kSlices + 2 * i);
    }
    s.slice_count = b_[instr::kSliceCount] > model::kSlices ? replaced(model::kSlices) : b_[instr::kSliceCount];
    s.selected_slice = limited(b_[instr::kSelectedSlice], model::kSlices - 1, 0);
    s.grain_length = get16(b_ + instr::kGrainLength);
    s.grain_position = get16(b_ + instr::kGrainPosition);
    s.grain_shape = limited(b_[instr::kGrainShape], 2, 0);
    s.grain_type = limited(b_[instr::kGrainType], 2, 0);
    s.reverb_send = limited(b_[instr::kReverbSend], model::kMaxPercent, 0);
    // Overdrive is real from version 6 (1.2.0) and bit depth from version 8 (1.3.1): the bytes before are padding
    // (the fixtures hold junk in them).
    s.overdrive = version_ <= 5 ? std::uint8_t{0} : limited(b_[instr::kOverdrive], model::kMaxPercent, 0);
    const std::uint8_t depth = b_[instr::kBitDepth];
    s.bit_depth = version_ < 8 ? std::uint8_t{16} : ((depth < 4 || depth > 16) ? replaced(16) : depth);
  }

 private:
  std::uint8_t replaced(std::uint32_t replacement) noexcept {
    ++count_;
    return static_cast<std::uint8_t>(replacement);
  }
  std::uint8_t limited(std::uint8_t v, std::uint32_t max, std::uint32_t fallback) noexcept {
    return v > max ? replaced(fallback) : v;
  }
  float finite(float f, float fallback) noexcept {
    if (finite_float(f)) {
      return f;
    }
    ++count_;
    return fallback;
  }

  const std::uint8_t* b_;
  std::uint8_t version_;
  std::uint16_t slot_;
  std::uint32_t& count_;
};

}  // namespace

// ---- reader -----------------------------------------------------------------------------------------------

void InstrumentReader::begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept {
  begin_base(source, arena);
  state_ = State::Head;
  want_ = kInstrumentBytesLegacy;
  have_ = 0;
  slot_ = params.index;
  size_hint_ = params.size_hint;
  pcm_bytes_ = 0;
  version_ = 0;
  standalone_ = params.standalone;
  if (slot_ >= (standalone_ ? model::kMaxInstruments : kSampleSlots)) {
    fail(FormatError::Malformed, standalone_ ? "instrument slot outside 0..63" : "instrument slot outside 0..47");
    state_ = State::Failed;
  }
}

// The first 388 bytes are in image_: enough for the header, and for the whole of a version 1-3 file.
void InstrumentReader::on_head() noexcept {
  if (image_[0] != 'T' || image_[1] != 'I' || get16(image_ + 2) != kTypeInstrument) {
    fail(FormatError::BadMagic, "not an OG instrument file");
    return;
  }
  version_ = image_[kVersionAt];
  report_.version = version_;
  if (version_ == 0 || (pti_ && version_ < 4)) {
    fail(FormatError::UnsupportedVersion, version_ == 0 ? "instrument structure version 0"
                                                        : "a .pti of structure version 1-3 does not exist");
    return;
  }
  report_.unverified_version = version_ > kInstrumentVerified;
  if (version_ <= 3) {  // 388 bytes, CRC at 384: the fields that came later read as zero
    if (crc32(image_, instr::kCrcLegacy) != get32(image_ + instr::kCrcLegacy)) {
      fail(FormatError::BadCrc, "the CRC does not match the instrument");
      return;
    }
    std::memset(image_ + instr::kReverbSend, 0, kInstrumentBytes - instr::kReverbSend);
    decode();
    state_ = State::Probe;
    want_ = 1;
    have_ = 0;
    return;
  }
  state_ = State::Rest;
  want_ = kInstrumentBytes;  // have_ stays 388
}

void InstrumentReader::on_image() noexcept {
  if (crc32(image_, instr::kCrc) != get32(image_ + instr::kCrc)) {
    if (pti_ && !report_.unverified_version) {  // a .pti's CRC is only warned about: other tools write none
      report_.crc_mismatch = true;
    } else {
      fail(FormatError::BadCrc, "the CRC does not match the instrument");
      return;
    }
  }
  decode();
  if (!pti_) {
    state_ = State::Probe;
    want_ = 1;
    have_ = 0;
  } else if (size_hint_ != 0) {  // the platform knows the size: the PCM is not read through
    pcm_bytes_ = size_hint_ > kInstrumentBytes ? size_hint_ - kInstrumentBytes : 0;
    on_end();
  } else {
    state_ = State::Pcm;
    want_ = sizeof drain_;
    have_ = 0;
  }
}

void InstrumentReader::decode() noexcept {
  Decoder(image_, version_, slot_, report_.sanitised).run(inst_);
}

void InstrumentReader::on_end() noexcept {
  if (pti_) {
    const std::uint32_t frames = pcm_bytes_ / 2;  // an odd last byte is ignored
    if (pcm_bytes_ > kMaxPcmBytes) {
      fail(FormatError::LimitExceeded, "the PCM is longer than the sample memory");
      return;
    }
    if (frames == 0) {  // a .pti with no PCM is no instrument: the slot stays as it is
      state_ = State::Done;
      progress_ = Progress::Done;
      return;
    }
    const std::uint32_t declared = get32(image_ + instr::kLength);
    if (declared != 0 && declared != frames) {  // the header's frame count disagrees: stereo, or damaged
      fail(FormatError::Unsupported, "the PCM is not mono (the header's length does not match it)");
      return;
    }
    report_.pcm_frames = frames;
  } else if (image_[instr::kActive] == 0) {  // an inactive instrument has no sample
    state_ = State::Done;
    progress_ = Progress::Done;
    return;
  }
  if (standalone_) {  // the caller applies it (an undo unit of its own): checked here as the model would
    if (model::validate_instrument(inst_, 0) != nullptr) {
      fail(FormatError::Malformed, "the instrument is not valid");
      return;
    }
  } else if (project_->set_instrument(slot_, inst_) != model::Status::Ok) {
    fail(FormatError::Internal, "the model refused the instrument");
    return;
  }
  report_.instruments = 1;
  state_ = State::Done;
  progress_ = Progress::Done;
}

Progress InstrumentReader::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    Pull r = Pull::Error;
    switch (state_) {
      case State::Head: r = pull(image_, want_, have_, budget, false); break;
      case State::Rest: r = pull(image_, want_, have_, budget, false); break;
      case State::Probe: r = pull(drain_, want_, have_, budget, false); break;
      case State::Pcm: r = pull(drain_, want_, have_, budget, false); break;
      case State::Done:
      case State::Failed:
      default: return progress_;
    }
    if (r == Pull::Partial) {
      return Progress::Running;
    }
    if (r == Pull::Error) {
      break;
    }
    if (r == Pull::End) {  // the source is over: right for the end of the file, short anywhere else
      if (state_ == State::Probe) {
        on_end();
      } else if (state_ == State::Pcm) {
        pcm_bytes_ += static_cast<std::uint32_t>(have_);
        on_end();
      } else {
        fail(FormatError::Truncated, "the file ends inside the instrument");
      }
      break;
    }
    switch (state_) {
      case State::Head: on_head(); break;
      case State::Rest: on_image(); break;
      case State::Probe: fail(FormatError::Malformed, "data after the instrument"); break;
      case State::Pcm:
        pcm_bytes_ += static_cast<std::uint32_t>(have_);
        have_ = 0;
        if (pcm_bytes_ > kMaxPcmBytes) {
          fail(FormatError::LimitExceeded, "the PCM is longer than the sample memory");
        }
        break;
      case State::Done:
      case State::Failed: break;
    }
  }
  if (progress_ == Progress::Failed) {
    state_ = State::Failed;
  }
  return progress_;
}

// ---- writer -----------------------------------------------------------------------------------------------

void InstrumentWriter::begin(const model::Project& project, core::ByteSink& sink, const WriteParams& params) noexcept {
  begin_base(project, sink);
  phase_ = Phase::Done;
  off_ = 0;
  pcm_have_ = 0;
  pcm_off_ = 0;
  slot_ = params.index;
  pcm_ = params.pcm;
  frames_ = pcm_ != nullptr ? params.pcm_frames : 0;
  pcm_left_ = 0;
  level_ = params.level_override >= 0 && params.level_override <= 100 ? params.level_override : std::int16_t{-1};
  fine_ = params.fine_override >= -100 && params.fine_override <= 100 ? params.fine_override : WriteParams::kNoFineOverride;
  const model::Instrument* held = project.instrument(slot_);
  if (params.standalone ? (held == nullptr || held->kind != model::InstrumentKind::Sample) : export_class(project, slot_) != ExportClass::Pti) {
    fail(FormatError::Malformed, params.standalone ? "the slot holds no sample instrument" : "the slot holds no sample instrument OG can store (slots 1-48)");
    return;
  }
  if (frames_ > kMaxPcmFrames) {
    fail(FormatError::LimitExceeded, "more PCM than the sample memory holds");
    return;
  }
  pcm_left_ = frames_ * 2u;
  build();
  phase_ = Phase::Head;
}

void InstrumentWriter::build() noexcept {
  const model::Instrument& in = *project_->instrument(slot_);
  const model::SamplerParams& s = in.sampler;
  std::uint8_t* b = image_;
  stamp_header(b, 'T', 'I', kTypeInstrument, kFw, kInstrumentVersion, kStructBytes);
  std::memset(b + kHeaderLen, 0, kInstrumentBytes - kHeaderLen);
  b[instr::kActive] = 1;
  b[instr::kSampleType] = s.sample_type;
  // the sample's name: the instrument's, else the file name of its sample
  const char* name = in.name;
  std::size_t n = text_len(in.name, model::kInstrumentNameMax);
  if (n == 0) {
    const char* slash = std::strrchr(in.sample_path, '/');
    name = slash != nullptr ? slash + 1 : in.sample_path;
    n = text_len(name, model::kSamplePathMax);
  }
  if (n > instr::kFileNameLen - 1) {
    n = instr::kFileNameLen - 1;
    ++report_.drops.instrument_names_cut;
  }
  std::memcpy(b + instr::kFileName, name, n);
  put32(b + instr::kLength, frames_);
  put16(b + instr::kWindowSize, s.wavetable_window_size);
  b[instr::kPlayMode] = s.play_mode;
  put16(b + instr::kStart, s.start);
  put16(b + instr::kLoopStart, s.loop_start);
  put16(b + instr::kLoopEnd, s.loop_end);
  put16(b + instr::kEnd, s.end);
  put32(b + instr::kCurrentWindow, s.wavetable_window);
  std::uint32_t wt_mod = 0;  // OS-CEWTENV: WT slots (loop 2 or 3) written off, and slot 3's dropped enable bit 0x02 (the granular length, W5), one per drop
  for (std::uint32_t i = 0; i < model::kEnvelopes; ++i) {
    std::uint8_t* e = b + instr::kEnvelope + i * instr::kEnvelopeStride;
    const model::Envelope& v = s.envelope[i];
    put_f32(e, v.amount);
    put16(e + 4, v.delay);
    put16(e + 6, v.attack);
    put16(e + 8, v.hold);
    put16(e + 10, v.decay);
    put_f32(e + 12, v.sustain);
    put16(e + 16, v.release);
    const bool wt = model::is_wt_loop(v.loop);  // OS-CEWTENV W1 (Q4 a): OG 1.9 has no WT modes, so a WT slot is written as an official one: off (loop 0, enable 0), counted in wt_mod
    e[18] = wt ? std::uint8_t{0} : v.loop;
    // OS-CEWTENV W5 (F2): slot 3's enable byte is a bit mask (0x01 the wavetable position, 0x02 the granular length): the export clears bit 0x02 (OG 1.9 has no length destination) and counts it in wt_mod;
    // every other bit goes down as stored, so a file without bit 0x02 is written byte for byte as before
    if (!wt && i == 3u && (v.enable & 2u) != 0u) {
      ++wt_mod;
    }
    e[19] = wt ? std::uint8_t{0} : (i == 3u ? static_cast<std::uint8_t>(v.enable & ~2u) : v.enable);
  }
  bool sine = false;
  for (std::uint32_t i = 0; i < model::kLfos; ++i) {
    std::uint8_t* l = b + instr::kLfo + i * instr::kLfoStride;
    const bool wt = model::is_wt_loop(s.envelope[i].loop);
    const bool is_sine = !wt && s.lfo[i].shape == model::kLfoShapeSine;  // OS-LFOSINE: OG has five shapes; Sine is written as Triangle and counted — only for loop <= 1 (a WT slot's shape is a contour, held to 0..4 below instead)
    sine = sine || is_sine;
    wt_mod += wt ? 1u : 0u;
    l[0] = wt ? (s.lfo[i].shape > 4u ? std::uint8_t{4} : s.lfo[i].shape) : (is_sine ? model::kLfoShapeTriangle : s.lfo[i].shape);
    l[1] = s.lfo[i].speed;
    put_f32(l + 4, s.lfo[i].amount);  // l[2] and l[3] stay 0 (the image is zeroed): the warp and sustain bytes are not written
  }
  report_.drops.lfo_sine += sine ? 1u : 0u;
  report_.drops.wt_mod += wt_mod;
  put_f32(b + instr::kCutoff, s.cutoff);
  put_f32(b + instr::kResonance, s.resonance);
  b[instr::kFilterType] = s.filter_type;
  b[instr::kFilterEnable] = s.filter_enable;
  b[instr::kTune] = static_cast<std::uint8_t>(s.tune);
  b[instr::kFineTune] = static_cast<std::uint8_t>(fine_ != WriteParams::kNoFineOverride ? fine_ : s.fine_tune);
  b[instr::kVolume] = level_ >= 0 ? static_cast<std::uint8_t>(level_) : s.volume;
  put16(b + instr::kGlide, s.glide);
  put16(b + instr::kPanning, s.panning);
  b[instr::kDelaySend] = s.delay_send;
  b[instr::kPlayThru] = in.slice_thru;  // OS-CEPLAYTHRU P1: 0 or 1 (the model's flag); official 1.9 writes 0 there (measured: 0 in all 70 fixture instruments), so a file without the flag is byte-identical
  for (std::uint32_t i = 0; i < model::kSlices; ++i) {
    put16(b + instr::kSlices + 2 * i, s.slices[i]);
  }
  b[instr::kSliceCount] = s.slice_count;
  b[instr::kSelectedSlice] = s.selected_slice;
  put16(b + instr::kGrainLength, s.grain_length);
  put16(b + instr::kGrainPosition, s.grain_position);
  b[instr::kGrainShape] = s.grain_shape;
  b[instr::kGrainType] = s.grain_type;
  b[instr::kReverbSend] = s.reverb_send;
  b[instr::kOverdrive] = s.overdrive;
  b[instr::kBitDepth] = s.bit_depth;
  put32(b + instr::kCrc, crc32(b, instr::kCrc));
}

Progress InstrumentWriter::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    if (!unchanged()) {
      break;
    }
    if (phase_ == Phase::Head) {
      const Flush f = flush(image_, kInstrumentBytes, off_, budget);
      if (f == Flush::Partial) {
        return Progress::Running;
      }
      if (f == Flush::Error) {
        break;
      }
      phase_ = Phase::Pcm;
      continue;
    }
    if (phase_ == Phase::Done) {
      progress_ = Progress::Done;
      break;
    }
    // PCM: one buffer at a time, pulled from the source and pushed to the sink
    if (pcm_off_ == pcm_have_) {
      if (pcm_left_ == 0) {
        phase_ = Phase::Done;
        continue;
      }
      const std::size_t ask = std::min<std::size_t>(sizeof pcm_buf_, pcm_left_);
      const core::IoResult r = pcm_->read(pcm_buf_, ask);
      if (r.status == core::IoStatus::WouldBlock) {
        return Progress::Running;
      }
      if (r.status == core::IoStatus::End) {
        fail(FormatError::Truncated, "the PCM source ended before the frames it promised");
        break;
      }
      if (r.status != core::IoStatus::Ok || r.count == 0 || r.count > ask) {
        fail(FormatError::SourceFailed, "the PCM source failed or broke its contract");
        break;
      }
      pcm_have_ = r.count;
      pcm_off_ = 0;
      pcm_left_ -= static_cast<std::uint32_t>(r.count);
    }
    const Flush f = flush(pcm_buf_, pcm_have_, pcm_off_, budget);
    if (f == Flush::Partial) {
      return Progress::Running;
    }
    if (f == Flush::Error) {
      break;
    }
  }
  return progress_;
}

}  // namespace o3os::formats::og
