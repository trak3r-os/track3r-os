// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/og.h>

#include "og_codec.h"

#include <cstring>

namespace o3os::formats::og {

using namespace codec;  // NOLINT: the shared field access and layouts

// ---- reader -----------------------------------------------------------------------------------------------
//
// A pattern file is a 28-byte head, then track rows, then a 4-byte CRC. Versions 1 to 5 hold 769-byte rows (a length
// byte plus 128 steps of 6 bytes): eight of them are the 8-track file of firmware up to 1.8 (6,184 B), twelve the
// 1.9 file (9,260 B), sixteen the 16-track v5 of tracker-ce (12,336 B, OS-CEPAT S1b); which one it is shows only in
// the stream: at the end of the eighth row, four bytes and the end of the source mean 8 tracks, five more bytes a
// ninth row; at the end of the twelfth row, four bytes and the end mean 12 tracks, a 13th row means 16. Version 6
// (tracker-ce, OS-CEPAT S1) holds 1,025-byte rows (128 steps of 8 bytes, with the volume and micromove columns) and
// says its track count in the declared size at offset 12 of the head: 8, 12 or 16 rows, 8,232 / 12,332 / 16,432 B.
// Rows are turned into model steps as they arrive, so only one row (and its 128 converted steps) is ever held; if
// the file then proves bad, the pattern is deleted again.

void PatternReader::begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept {
  begin_base(source, arena);
  id_ = 0;
  created_ = false;
  v6_ = false;
  tracks_total_ = 0;
  track_ = 0;
  first_length_ = 0;
  crc8_ = 0;
  crc12_ = 0;
  want_ = kPatternHead;
  have_ = 0;
  state_ = State::Head;
  if (params.index < 1 || params.index > model::kMaxPatternId) {
    reject(FormatError::Malformed, "pattern id outside 1..255");
    return;
  }
  id_ = static_cast<std::uint8_t>(params.index);
}

void PatternReader::reject(FormatError e, const char* detail) noexcept {
  fail(e, detail);
  state_ = State::Failed;
}

void PatternReader::on_head() noexcept {
  const bool magic = (scratch_[0] == 'K' && scratch_[1] == 'S') || (scratch_[0] == 'P' && scratch_[1] == 'M');
  if (!magic) {
    reject(FormatError::BadMagic, "not an OG pattern file");
    return;
  }
  const std::uint8_t version = scratch_[kVersionAt];
  if (version == 0) {
    reject(FormatError::UnsupportedVersion, "pattern structure version 0");
    return;
  }
  report_.version = version;
  report_.unverified_version = version > kPatternVerified;
  if (version >= 6) {
    // OS-CEPAT S1: v6 and above declare their size at offset 12; only the three v6 sizes have a known layout, and
    // they give the track count (a v7+ file of a v6 size is read with the v6 layout and flagged unverified above).
    const std::uint32_t tracks = pattern_tracks_v6(get16(scratch_ + 12));
    if (tracks == 0) {
      reject(FormatError::Malformed, "v6 pattern of an unknown size");
      return;
    }
    v6_ = true;
    tracks_total_ = static_cast<std::uint8_t>(tracks);
  }
  switch (project_->create_pattern(id_, project_->default_pattern_length())) {
    case model::Status::Ok:
      created_ = true;
      break;
    case model::Status::Full:
      reject(FormatError::LimitExceeded, "the pattern pool is full");
      return;
    default:
      reject(FormatError::Malformed, "the project already holds a pattern with this id");
      return;
  }
  state_ = State::Row;
  want_ = v6_ ? kPatternRow6 : kPatternRow;
  have_ = 0;
}

void PatternReader::on_row() noexcept {
  const std::uint8_t length = scratch_[0];  // the index of the last step: 0..127
  if (length >= model::kMaxPatternLength) {
    reject(FormatError::Malformed, "track length byte above 127");
    return;
  }
  if (v6_) {  // 128 steps of 8 bytes: bytes 0-5 as v5, then the volume and micromove columns (OS-CEPAT S1, 3.3)
    for (std::uint32_t s = 0; s < model::kMaxPatternLength; ++s) {
      row_[s] = step_from_og_v6(scratch_ + 1 + s * patt::kStep6, &report_.micromoves_dropped, &report_.values_clamped,
                                &report_.ce_fx_cleared);
    }
  } else {
    for (std::uint32_t s = 0; s < model::kMaxPatternLength; ++s) {
      row_[s] = step_from_og(scratch_ + 1 + s * patt::kStep, &report_.ce_fx_cleared);
    }
  }
  if (project_->set_steps(id_, track_, 0, row_, model::kMaxPatternLength) != model::Status::Ok ||
      project_->set_track_length(id_, track_, static_cast<std::uint32_t>(length) + 1u) != model::Status::Ok) {
    reject(FormatError::Internal, "the model refused a track row");
    return;
  }
  if (track_ == 0) {
    first_length_ = static_cast<std::uint8_t>(length + 1);
  }
  ++track_;
}

void PatternReader::finish() noexcept {
  // Tracks the file does not hold keep the length of the first.
  for (std::uint32_t t = track_; t < model::kMaxTracks; ++t) {
    if (project_->set_track_length(id_, t, first_length_) != model::Status::Ok) {
      reject(FormatError::Internal, "the model refused a track length");
      return;
    }
  }
  report_.tracks = track_;
  report_.patterns = 1;
  state_ = State::Done;
  progress_ = Progress::Done;
}

Progress PatternReader::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    const Pull r = pull(scratch_, want_, have_, budget, state_ != State::Crc && state_ != State::End);
    if (r == Pull::Partial) {
      return Progress::Running;
    }
    if (r == Pull::Error) {
      break;
    }
    if (r == Pull::End) {
      if (state_ == State::Tail && have_ == 4) {  // the file ends after eight tracks: those four bytes are the CRC
        if (get32(scratch_) != crc8_) {
          reject(FormatError::BadCrc, "the CRC does not match the pattern");
        } else {
          finish();
        }
      } else if (state_ == State::Tail12 && have_ == 4) {  // ... and after twelve: those four bytes are the CRC
        if (get32(scratch_) != crc12_) {  // (OS-CEPAT S1b: a 16-track v5 file goes on instead, see State::Tail12)
          reject(FormatError::BadCrc, "the CRC does not match the pattern");
        } else {
          finish();
        }
      } else if (state_ == State::End) {  // the 12- or 16-track file ends where it should
        finish();
      } else {
        reject(FormatError::Truncated, "the file ends inside the pattern");
      }
      break;
    }
    switch (state_) {
      case State::Head:
        on_head();
        break;
      case State::Row:
        on_row();
        if (progress_ != Progress::Running) {
          break;
        }
        if (v6_) {  // the declared size fixed the track count: after the last row come the 4 CRC bytes
          if (track_ == tracks_total_) {
            state_ = State::Crc;
            want_ = 4;
            have_ = 0;
          } else {
            want_ = kPatternRow6;
            have_ = 0;
          }
        } else if (track_ == 8) {
          crc8_ = crc_.value();
          state_ = State::Tail;
          want_ = 5;
          have_ = 0;
        } else if (track_ == kTracks) {  // twelve rows in: a 13th row means 16 tracks (OS-CEPAT S1b), else the CRC
          crc12_ = crc_.value();
          state_ = State::Tail12;
          want_ = 5;
          have_ = 0;
        } else if (track_ == model::kMaxTracks) {  // the sixteenth row of a 16-track v5 file: the CRC follows
          state_ = State::Crc;
          want_ = 4;
          have_ = 0;
        } else {
          want_ = kPatternRow;
          have_ = 0;
        }
        break;
      case State::Tail:  // five bytes: the first of the ninth row, so this is not the 8-track file
        state_ = State::Row;
        want_ = kPatternRow;  // have_ stays 5: those bytes are the start of the row
        break;
      case State::Tail12:  // five bytes: the first of the 13th row, so this is the 16-track file (OS-CEPAT S1b)
        state_ = State::Row;
        want_ = kPatternRow;  // have_ stays 5: those bytes are the start of the row
        break;
      case State::Crc:
        if (get32(scratch_) != crc_.value()) {
          reject(FormatError::BadCrc, "the CRC does not match the pattern");
          break;
        }
        state_ = State::End;
        want_ = 1;
        have_ = 0;
        break;
      case State::End:
        reject(FormatError::Malformed, "data after the CRC");
        break;
      case State::Done:
      case State::Failed:
        break;
    }
  }
  if (progress_ == Progress::Failed && created_) {  // leave the project as it was
    (void)project_->delete_pattern(id_);
    created_ = false;
  }
  return progress_;
}

// ---- writer -----------------------------------------------------------------------------------------------

namespace {
constexpr std::uint8_t kFw[4] = {1, 9, 0, 1};  // firmware 1.9.0.1, what Export writes (docs/formats/og.md section 4)
constexpr std::uint8_t kPatternVersion = 5;
}  // namespace

void PatternWriter::begin(const model::Project& project, core::ByteSink& sink, const WriteParams& params) noexcept {
  begin_base(project, sink);
  piece_ = 0;
  built_ = false;
  len_ = 0;
  off_ = 0;
  id_ = 0;
  if (params.index < 1 || params.index > model::kMaxPatternId || project.pattern(params.index) == nullptr) {
    fail(FormatError::Malformed, "no such pattern in the project");
    return;
  }
  id_ = static_cast<std::uint8_t>(params.index);
}

void PatternWriter::build_piece() noexcept {
  const model::Pattern* p = project_->pattern(id_);
  if (piece_ == 0) {  // head: header, tempo (an unused float, 100.0 as in 1.9), swing, reserved bytes
    std::memset(scratch_, 0, kPatternHead);
    stamp_header(scratch_, 'K', 'S', kTypePattern, kFw, kPatternVersion, static_cast<std::uint16_t>(kPatternBytes12));
    put_f32(scratch_ + patt::kTempoReserve, 100.0f);
    len_ = kPatternHead;
  } else if (piece_ <= kTracks) {
    const std::uint32_t t = piece_ - 1u;
    scratch_[0] = static_cast<std::uint8_t>(p->track_length[t] - 1);
    for (std::uint32_t s = 0; s < model::kMaxPatternLength; ++s) {
      step_to_og(p->steps[t][s], scratch_ + 1 + s * patt::kStep, report_.drops);
    }
    len_ = kPatternRow;
  } else {  // the CRC; tracks 13-16 are not written, say what was in them
    for (std::uint32_t t = kTracks; t < model::kMaxTracks; ++t) {
      for (std::uint32_t s = 0; s < model::kMaxPatternLength; ++s) {
        if (!model::is_empty(p->steps[t][s])) {
          ++report_.drops.tracks_beyond_12;
        }
      }
    }
    put32(scratch_, crc_.value());
    len_ = 4;
    return;  // the CRC is not part of itself
  }
  crc_.update(scratch_, len_);
}

Progress PatternWriter::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    if (!unchanged()) {
      break;
    }
    if (piece_ > kTracks + 1u) {
      progress_ = Progress::Done;
      break;
    }
    if (!built_) {
      build_piece();
      built_ = true;
      off_ = 0;
    }
    const Flush f = flush(scratch_, len_, off_, budget);
    if (f == Flush::Partial) {
      return Progress::Running;
    }
    if (f == Flush::Error) {
      break;
    }
    built_ = false;
    ++piece_;
  }
  return progress_;
}

}  // namespace o3os::formats::og
