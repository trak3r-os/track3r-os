// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/native.h>

#include "native_codec.h"
#include "native_state.h"
#include "settings_chunks.h"

#include <algorithm>
#include <cstring>

namespace o3os::formats::native {

using namespace codec;  // NOLINT: the shared field cursors and size formulas

namespace {

// Canonical order of the known chunks. A known chunk may not come before one of a higher rank; only PATT and
// INST may repeat. Unknown chunks may appear anywhere after PHDR.
constexpr std::uint8_t kRankPhdr = 1;
constexpr std::uint8_t kRankPatt = 5;
constexpr std::uint8_t kRankInst = 6;
constexpr std::uint8_t kRankSets = 7;  // OS-002d: after INST, before the unknown chunks and the CRC
constexpr std::uint8_t kRankLrnb = 8;
constexpr std::uint8_t kRankMidi = 9;  // OS-004d: the project-level state, after LRNB in this order (state_chunks::Part), before the unknown chunks and the CRC
constexpr std::uint8_t kRankCrc = 19;  // after the ten state parts (OS-004d: MIDI..APPM, OS-PERSIST2: NOTE, MUTE, SCEN, SNFX, OS-BUG-PASTE24: LAW)

std::uint8_t rank_of(std::uint32_t id) noexcept {
  switch (id) {
    case kChunkPhdr: return kRankPhdr;
    case kChunkTrak: return 2;
    case kChunkModr: return 3;
    case kChunkSong: return 4;
    case kChunkPatt: return kRankPatt;
    case kChunkInst: return kRankInst;
    case kChunkSets: return kRankSets;
    case kChunkLrnb: return kRankLrnb;
    case kChunkMidi: return kRankMidi;
    case kChunkUsbs: return kRankMidi + 1;
    case kChunkMixr: return kRankMidi + 2;
    case kChunkPerf: return kRankMidi + 3;
    case kChunkAppm: return kRankMidi + 4;
    case kChunkNote: return kRankMidi + 5;
    case kChunkMute: return kRankMidi + 6;
    case kChunkScen: return kRankMidi + 7;
    case kChunkSnfx: return kRankMidi + 8;
    case kChunkLaw: return kRankMidi + 9;
    case kChunkCrc: return kRankCrc;
    default: return 0;
  }
}

// The newest minor version of a chunk this reader knows: INST is at 1.3 (the sample rate, the playback engine, the MIDI fields), PERF and SCEN at 1.1 (the Perform FX 20 and 21),
// APPM at 1.1 (OS-APPUX S3: up to 12 apps), every other chunk at 1.0.
std::uint32_t known_minor(std::uint32_t id) noexcept {
  switch (id) {
    case kChunkInst: return kInstVersionLfoSine & 0xFFu;
    case kChunkPerf:
    case kChunkScen: return kStateVersionFx & 0xFFu;
    case kChunkAppm: return state_chunks::kAppsVersionV11 & 0xFFu;
    case kChunkUsbs: return state_chunks::kUsbVersionCount & 0xFFu;
    default: return kChunkVersion & 0xFFu;
  }
}

// Payload bounds of a known chunk as version 1.0 defines them. `lo` holds for every minor version (a newer
// minor only appends fields). `hi` holds for the chunk's version `minor` itself (INST 1.1 is four bytes longer than
// 1.0); for a newer minor it is only how much of the chunk is read (whole-payload chunks) and the rest, which the
// reader does not know, is skipped.
void v1_bounds(std::uint32_t id, std::uint32_t minor, std::size_t& lo, std::size_t& hi) noexcept {
  switch (id) {
    case kChunkPhdr: lo = kPhdrFixed; hi = kPhdrMax; break;
    case kChunkTrak: lo = kTrakFixed; hi = kTrakMax; break;
    case kChunkModr: lo = kModrFixed; hi = kModrMax; break;
    case kChunkSong: lo = kSongFixed; hi = kSongMax; break;
    case kChunkInst: lo = kInstFixed; hi = kInstMax + (minor >= 1 ? kInstRate : 0) + (minor >= 2 ? kInstEngine : 0) + (minor >= 3 ? kInstMidi : 0); break;
    case kChunkPatt: lo = kPattHead + kPattTrackFixed; hi = kPattMax; break;
    case kChunkSets: lo = settings_chunks::kFixed; hi = settings_chunks::kSetsMax; break;
    case kChunkLrnb: lo = settings_chunks::kFixed; hi = settings_chunks::kLrnbMax; break;
    case kChunkMidi: lo = hi = state_chunks::kMidiBytes; break;
    case kChunkUsbs: lo = state_chunks::kUsbFixed; hi = state_chunks::kUsbMax + (minor >= 1 ? 1 : 0); break;  // 1.1 appends the channel count (OS-USBOUTS)
    case kChunkMixr: lo = hi = state_chunks::kMixBytes; break;
    case kChunkPerf: lo = hi = state_chunks::kPerfBytes; break;
    case kChunkAppm: lo = state_chunks::kAppsFixed; hi = minor >= 1 ? state_chunks::kAppsMax : state_chunks::kAppsMax10; break;
    case kChunkNote: lo = hi = state_chunks::kNoteBytes; break;
    case kChunkMute: lo = state_chunks::kMuteFixed; hi = state_chunks::kMuteMax; break;
    case kChunkScen: lo = hi = state_chunks::kSceneBytes; break;
    case kChunkSnfx: lo = state_chunks::kSendFxFixed; hi = state_chunks::kSendFxMax; break;
    case kChunkLaw: lo = hi = state_chunks::kLawBytes; break;
    default: lo = kCrcPayload; hi = kCrcPayload; break;  // CRC
  }
}

constexpr const char* kBadFields = "chunk fields do not fit (short chunk, text too long or containing NUL)";

}  // namespace

const char* error_name(Error e) noexcept {
  switch (e) {
    case Error::None: return "none";
    case Error::Truncated: return "truncated";
    case Error::BadMagic: return "bad magic";
    case Error::BadCrc: return "bad CRC";
    case Error::NewerMajor: return "newer major version";
    case Error::LimitExceeded: return "limit exceeded";
    case Error::Malformed: return "malformed";
    case Error::SourceFailed: return "source failed";
    case Error::SinkFailed: return "sink failed";
    case Error::Modified: return "modified while writing";
    case Error::Internal: return "internal error";
  }
  return "unknown";
}

void Reader::begin(core::ByteSource& source, model::ModelArena& arena) noexcept {
  source_ = &source;
  project_ = &arena.project;
  project_->reset();
  state_ = State::FormHeader;
  progress_ = Progress::Running;
  report_ = Report{};
  settings_.init(core::ParamKind::Project);
  crc_.reset();
  crc_before_chunk_.reset();
  want_ = kFormHeader;
  have_ = 0;
  form_left_ = 0;
  chunk_id_ = 0;
  chunk_len_ = 0;
  chunk_left_ = 0;
  skip_left_ = 0;
  expected_patterns_ = 0;
  expected_instruments_ = 0;
  expected_modules_ = 0;
  rank_ = 0;
  chunk_pad_ = 0;
  chunk_minor_newer_ = false;
  chunk_minor_ = 0;
  law_ = 0;
}

void Reader::fail(Error e, const char* detail) noexcept {
  report_.error = e;
  report_.detail = detail;
  report_.offset = report_.bytes;
  progress_ = Progress::Failed;
  state_ = State::Failed;
  want_ = 0;
  have_ = 0;
  project_->reset();  // never leave a half-loaded project behind
}

bool Reader::check(model::Status s, const char* what) noexcept {
  if (s == model::Status::Ok) {
    return true;
  }
  fail(s == model::Status::Full ? Error::LimitExceeded : Error::Malformed, what);
  return false;
}

bool Reader::check_version(std::uint16_t version) noexcept {
  const std::uint32_t major = static_cast<std::uint32_t>(version) >> 8;
  const std::uint32_t minor = version & 0xFFu;
  if (major == 0) {
    fail(Error::Malformed, "chunk version 0");
    return false;
  }
  if (major > (kChunkVersion >> 8)) {
    fail(Error::NewerMajor, "chunk written by a newer major version of the format");
    return false;
  }
  chunk_minor_ = static_cast<std::uint8_t>(minor);
  if (minor > known_minor(chunk_id_)) {
    chunk_minor_newer_ = true;
    report_.newer_minor = true;
  }
  return true;
}

Reader::Pull Reader::pull(std::size_t& budget) noexcept {
  while (have_ < want_) {
    if (budget == 0) {
      return Pull::Partial;
    }
    const std::size_t ask = std::min(want_ - have_, budget);
    const core::IoResult r = source_->read(scratch_ + have_, ask);
    switch (r.status) {
      case core::IoStatus::Ok:
        if (r.count == 0 || r.count > ask) {
          fail(Error::SourceFailed, "source broke its contract");
          return Pull::Error;
        }
        crc_.update(scratch_ + have_, r.count);
        have_ += r.count;
        budget -= r.count;
        report_.bytes += static_cast<std::uint32_t>(r.count);
        break;
      case core::IoStatus::WouldBlock:
        return Pull::Partial;
      case core::IoStatus::End:
        fail(Error::Truncated, "the file ends before its FORM does");
        return Pull::Error;
      case core::IoStatus::Failed:
      default:
        fail(Error::SourceFailed, "source failed");
        return Pull::Error;
    }
  }
  return Pull::Complete;
}

Progress Reader::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    if (have_ < want_) {
      const Pull r = pull(budget);
      if (r == Pull::Partial) {
        return Progress::Running;
      }
      if (r == Pull::Error) {
        break;  // fail() has set progress_
      }
    }
    advance();
  }
  return progress_;
}

// The bytes the current state asked for are in scratch_ (or it asked for none): act on them and move on.
// Every path sets a new state, a new want_, or ends the run, so this cannot spin.
void Reader::advance() noexcept {
  switch (state_) {
    case State::FormHeader: {
      if (get32(scratch_) != kForm || get32(scratch_ + 8) != kTypeO3pj) {
        fail(Error::BadMagic, "not an O3PJ project file");
        return;
      }
      const std::uint32_t len = get32(scratch_ + 4);
      if (len > kMaxFileBytes - 8u) {
        fail(Error::LimitExceeded, "file larger than the limit");
        return;
      }
      if (len < 4 || (len & 1u) != 0) {
        fail(Error::Malformed, "bad FORM length");
        return;
      }
      form_left_ = len - 4;
      begin_chunk_header();
      return;
    }
    case State::ChunkHeader:
      on_chunk_header();
      return;
    case State::Version: {
      // The version comes before any length rule: a chunk of a newer major version may have any layout, a
      // newer minor only appends to the one this reader knows.
      if (!check_version(get16(scratch_))) {
        return;
      }
      std::size_t lo = 0;
      std::size_t hi = 0;
      v1_bounds(chunk_id_, chunk_minor_, lo, hi);
      if (chunk_len_ < lo || (!chunk_minor_newer_ && chunk_len_ > hi)) {
        fail(Error::Malformed, "chunk length out of range");
        return;
      }
      if (chunk_id_ == kChunkPatt) {
        state_ = State::PattHead;
        want_ = kPattHead;  // have_ stays 2: the version bytes are already in scratch_
      } else {
        state_ = State::Payload;
        want_ = std::min<std::size_t>(chunk_len_, hi);
      }
      return;
    }
    case State::Payload:
      on_payload();
      return;
    case State::PattHead:
      on_patt_head();
      return;
    case State::PattName:
      if (patt_name_len_ != 0) {
        for (std::size_t i = 0; i < patt_name_len_; ++i) {
          if (scratch_[i] == 0) {
            fail(Error::Malformed, "PATT: NUL in the name");
            return;
          }
        }
        if (!check(project_->set_pattern_name(patt_id_, reinterpret_cast<const char*>(scratch_), patt_name_len_),
                   "PATT: bad name")) {
          return;
        }
        chunk_left_ -= patt_name_len_;
      }
      next_track();
      return;
    case State::PattTrackLen: {
      row_len_ = scratch_[1];
      chunk_left_ -= static_cast<std::uint32_t>(kPattTrackFixed);
      if (!check(project_->set_track_length(patt_id_, patt_track_, scratch_[0]), "PATT: bad track length")) {
        return;
      }
      if (row_len_ > model::kMaxPatternLength) {
        fail(Error::Malformed, "PATT: more than 128 steps in a track");
        return;
      }
      const std::uint32_t bytes = row_len_ * static_cast<std::uint32_t>(sizeof(model::Step));
      if (chunk_left_ < bytes) {
        fail(Error::Malformed, "PATT ends inside a track");
        return;
      }
      if (row_len_ == 0) {
        ++patt_track_;
        next_track();
        return;
      }
      state_ = State::PattTrackData;
      want_ = bytes;
      have_ = 0;
      return;
    }
    case State::PattTrackData:
      std::memcpy(row_, scratch_, want_);  // the wire layout of a step is its in-memory layout
      if (!check(project_->set_steps(patt_id_, patt_track_, 0, row_, row_len_), "PATT: bad steps")) {
        return;
      }
      chunk_left_ -= static_cast<std::uint32_t>(want_);
      ++patt_track_;
      next_track();
      return;
    case State::UnknownBody:
      if (!check(project_->unknown_append(scratch_, static_cast<std::uint32_t>(want_)), "unknown chunk overflow")) {
        return;
      }
      [[fallthrough]];
    case State::Skip:
      skip_left_ -= static_cast<std::uint32_t>(want_);
      if (skip_left_ == 0) {
        end_payload();
      } else {
        want_ = std::min<std::size_t>(skip_left_, kScratchBytes);
        have_ = 0;
      }
      return;
    case State::Pad:
      if (scratch_[0] != 0) {
        fail(Error::Malformed, "pad byte is not zero");
        return;
      }
      end_chunk();
      return;
    case State::Done:
    case State::Failed:
      return;  // not reached: step() stops
  }
}

void Reader::begin_chunk_header() noexcept {
  if (form_left_ == 0) {
    fail(Error::Malformed, "the FORM ends without a CRC chunk");
    return;
  }
  if (form_left_ < kChunkHeader) {
    fail(Error::Malformed, "chunk header overruns the FORM");
    return;
  }
  crc_before_chunk_ = crc_;  // the CRC chunk covers everything before its own header
  state_ = State::ChunkHeader;
  want_ = kChunkHeader;
  have_ = 0;
}

void Reader::on_chunk_header() noexcept {
  const std::uint32_t id = get32(scratch_);
  const std::uint32_t len = get32(scratch_ + 4);
  form_left_ -= static_cast<std::uint32_t>(kChunkHeader);
  chunk_id_ = id;
  chunk_len_ = len;
  report_.chunk = id;
  chunk_pad_ = static_cast<std::uint8_t>(len & 1u);
  if (len > form_left_ || len + chunk_pad_ > form_left_) {
    fail(Error::Malformed, "chunk overruns the FORM");
    return;
  }
  form_left_ -= len + chunk_pad_;
  chunk_left_ = len;
  chunk_minor_newer_ = false;
  chunk_minor_ = 0;

  if (rank_ == 0 && id != kChunkPhdr) {
    fail(Error::Malformed, "the first chunk must be PHDR");
    return;
  }
  const std::uint8_t rank = rank_of(id);
  if (rank != 0) {
    if (rank < rank_ || (rank == rank_ && rank != kRankPatt && rank != kRankInst)) {
      fail(Error::Malformed, "chunk out of order or repeated");
      return;
    }
    rank_ = rank;
  }

  if (rank != 0) {  // a known chunk: its version first (a chunk shorter than that has no layout at all)
    if (len < 2) {
      fail(Error::Malformed, "chunk length out of range");
      return;
    }
    state_ = State::Version;
    want_ = 2;
    have_ = 0;
    return;
  }
  if (!model::is_printable_fourcc(id)) {
    fail(Error::Malformed, "chunk id is not four printable characters");
    return;
  }
  switch (project_->unknown_begin(id, len)) {
    case model::Status::Ok:
      state_ = State::UnknownBody;
      skip_left_ = len;
      if (len == 0) {
        end_payload();
      } else {
        want_ = std::min<std::size_t>(len, kScratchBytes);
        have_ = 0;
      }
      return;
    case model::Status::Full:  // no room in the pool: counted as dropped, skipped
      enter_skip(len);
      return;
    default:
      fail(Error::Malformed, "bad unknown chunk");
      return;
  }
}

void Reader::on_payload() noexcept {
  bool ok = false;
  const std::size_t have = want_;  // bytes of the payload in scratch_: all of it, or the v1 part of a newer minor
  switch (chunk_id_) {
    case kChunkPhdr: ok = parse_phdr(have); break;
    case kChunkTrak: ok = parse_trak(have); break;
    case kChunkModr: ok = parse_modr(have); break;
    case kChunkSong: ok = parse_song(have); break;
    case kChunkInst: ok = parse_inst(have); break;
    case kChunkSets: ok = parse_settings(have, false); break;
    case kChunkLrnb: ok = parse_settings(have, true); break;
    case kChunkMidi: ok = parse_state(have, 0); break;
    case kChunkUsbs: ok = parse_state(have, 1); break;
    case kChunkMixr: ok = parse_state(have, 2); break;
    case kChunkPerf: ok = parse_state(have, 3); break;
    case kChunkAppm: ok = parse_state(have, 4); break;
    case kChunkNote: ok = parse_state(have, 5); break;
    case kChunkMute: ok = parse_state(have, 6); break;
    case kChunkScen: ok = parse_state(have, 7); break;
    case kChunkSnfx: ok = parse_state(have, 8); break;
    case kChunkLaw: ok = parse_state(have, 9); break;
    case kChunkCrc: ok = parse_crc(have); break;
    default: fail(Error::Internal, "payload state for an unknown chunk"); return;
  }
  if (!ok) {
    return;
  }
  // The rest of a newer-minor chunk is fields this reader does not know: skipped, lost on the next save.
  enter_skip(chunk_len_ - static_cast<std::uint32_t>(have));
}

void Reader::end_payload() noexcept {
  if (chunk_pad_ != 0) {
    state_ = State::Pad;
    want_ = 1;
    have_ = 0;
  } else {
    end_chunk();
  }
}

void Reader::end_chunk() noexcept {
  if (chunk_id_ == kChunkCrc) {
    finish();
  } else {
    begin_chunk_header();
  }
}

void Reader::enter_skip(std::uint32_t n) noexcept {
  skip_left_ = n;
  if (n == 0) {
    end_payload();
    return;
  }
  state_ = State::Skip;
  want_ = std::min<std::size_t>(n, kScratchBytes);
  have_ = 0;
}

void Reader::finish() noexcept {
  if (form_left_ != 0) {
    fail(Error::Malformed, "data after the CRC chunk");
    return;
  }
  if (report_.patterns != expected_patterns_ || report_.instruments != expected_instruments_ ||
      project_->module_count() != expected_modules_) {
    fail(Error::Malformed, "chunk counts differ from the header");
    return;
  }
  if (!project_->check_invariants()) {
    fail(Error::Malformed, "the loaded project breaks a model invariant");
    return;
  }
  project_->set_volume_law(law_);
  report_.unknown_kept = project_->unknown_count();
  report_.unknown_dropped = project_->unknown_dropped_chunks();
  state_ = State::Done;
  progress_ = Progress::Done;
}

// ---- PATT ---------------------------------------------------------------------------------------------

void Reader::on_patt_head() noexcept {
  Cursor c(scratch_, kPattHead);
  (void)c.u16();  // the version: checked when the chunk header was read
  const std::uint8_t id = c.u8();
  const std::uint8_t tracks = c.u8();
  const std::uint8_t name_len = c.u8();
  if (id < 1 || tracks < 1 || tracks > model::kMaxTracks || name_len > model::kPatternNameMax) {
    fail(Error::Malformed, "PATT: bad id, track count or name length");
    return;
  }
  chunk_left_ -= static_cast<std::uint32_t>(kPattHead);
  if (chunk_left_ < name_len) {
    fail(Error::Malformed, "PATT ends inside its name");
    return;
  }
  // Tracks the chunk does not store keep the project's default pattern length.
  if (!check(project_->create_pattern(id, project_->default_pattern_length()), "PATT: duplicate id or pattern pool full")) {
    return;
  }
  ++report_.patterns;
  patt_id_ = id;
  patt_tracks_ = tracks;
  patt_track_ = 0;
  patt_name_len_ = name_len;
  state_ = State::PattName;
  want_ = name_len;
  have_ = 0;
}

void Reader::next_track() noexcept {
  if (patt_track_ == patt_tracks_) {
    if (chunk_left_ == 0) {
      end_payload();
    } else if (chunk_minor_newer_) {  // fields of a newer minor version: skipped, lost on the next save
      const std::uint32_t rest = chunk_left_;
      chunk_left_ = 0;
      enter_skip(rest);
    } else {
      fail(Error::Malformed, "unused bytes at the end of PATT");
    }
    return;
  }
  if (chunk_left_ < kPattTrackFixed) {
    fail(Error::Malformed, "PATT ends inside its track list");
    return;
  }
  state_ = State::PattTrackLen;
  want_ = kPattTrackFixed;
  have_ = 0;
}

// ---- whole-payload chunks ---------------------------------------------------------------------------

namespace {

// Short chunk, over-long or NUL-containing text, or unused bytes the version does not explain.
bool fields_ok(const Cursor& c, bool minor_newer) noexcept { return !c.bad() && (c.left() == 0 || minor_newer); }

}  // namespace

bool Reader::parse_phdr(std::size_t len) noexcept {
  Cursor c(scratch_, len);
  (void)c.u16();  // the version: checked when the chunk header was read
  const std::uint8_t active = c.u8();
  const std::uint8_t default_length = c.u8();
  const std::uint32_t tempo = c.u32();
  const std::uint8_t swing = c.u8();
  model::MasterSettings m{};
  m.volume = c.u8();
  m.dry_volume = c.u8();
  m.reverb_volume = c.u8();
  m.delay_volume = c.u8();
  m.flags = c.u8();
  const std::uint32_t patterns = c.u16();
  const std::uint32_t instruments = c.u8();
  const std::uint32_t modules = c.u8();
  const std::uint8_t* name = nullptr;
  std::size_t name_len = 0;
  (void)c.text(model::kProjectNameMax, name, name_len);
  if (!fields_ok(c, chunk_minor_newer_)) {
    fail(Error::Malformed, kBadFields);
    return false;
  }
  if (patterns > model::kMaxPatterns || instruments > model::kMaxInstruments || modules > model::kMaxModules) {
    fail(Error::LimitExceeded, "the header announces more patterns, instruments or modules than the model holds");
    return false;
  }
  expected_patterns_ = patterns;
  expected_instruments_ = instruments;
  expected_modules_ = modules;
  return check(project_->set_active_tracks(active), "PHDR: bad active track count") &&
         check(project_->set_default_pattern_length(default_length), "PHDR: bad default pattern length") &&
         check(project_->set_tempo_x100(tempo), "PHDR: tempo out of range") &&
         check(project_->set_swing(swing), "PHDR: swing out of range") &&
         check(project_->set_master(m), "PHDR: bad master settings") &&
         check(project_->set_name(reinterpret_cast<const char*>(name), name_len), "PHDR: bad name");
}

bool Reader::parse_trak(std::size_t len) noexcept {
  Cursor c(scratch_, len);
  (void)c.u16();  // the version: checked when the chunk header was read
  const std::uint32_t slots = c.u8();
  if (c.bad() || slots < 1 || slots > model::kMaxTracks) {
    fail(Error::Malformed, "TRAK: bad slot count");
    return false;
  }
  for (std::uint32_t i = 0; i < slots; ++i) {
    model::TrackSlot t{};
    t.type = static_cast<model::TrackType>(c.u8());
    t.flags = c.u8();
    t.volume = c.u8();
    t.pan = c.u8();
    t.bus = c.u8();
    const std::uint8_t* name = nullptr;
    std::size_t name_len = 0;
    if (!c.text(model::kTrackNameMax, name, name_len)) {
      fail(Error::Malformed, kBadFields);
      return false;
    }
    std::memcpy(t.name, name, name_len);
    if (!check(project_->set_track(i, t), "TRAK: bad track slot")) return false;
  }
  if (!fields_ok(c, chunk_minor_newer_)) {
    fail(Error::Malformed, kBadFields);
    return false;
  }
  return true;
}

bool Reader::parse_modr(std::size_t len) noexcept {
  Cursor c(scratch_, len);
  (void)c.u16();  // the version: checked when the chunk header was read
  const std::uint32_t count = c.u8();
  if (c.bad()) {
    fail(Error::Malformed, kBadFields);
    return false;
  }
  if (count > model::kMaxModules) {
    fail(Error::LimitExceeded, "MODR: more modules than the table holds");
    return false;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint8_t major = c.u8();
    const std::uint8_t minor = c.u8();
    const std::uint8_t* name = nullptr;
    std::size_t name_len = 0;
    if (!c.text(model::kModuleNameMax, name, name_len)) {
      fail(Error::Malformed, kBadFields);
      return false;
    }
    if (!check(project_->add_module(reinterpret_cast<const char*>(name), name_len, major, minor),
               "MODR: bad or duplicate module")) {
      return false;
    }
  }
  if (!fields_ok(c, chunk_minor_newer_)) {
    fail(Error::Malformed, kBadFields);
    return false;
  }
  return true;
}

bool Reader::parse_song(std::size_t len) noexcept {
  Cursor c(scratch_, len);
  (void)c.u16();  // the version: checked when the chunk header was read
  const std::uint32_t count = c.u8();
  const std::uint8_t* ids = c.take(count);
  if (!fields_ok(c, chunk_minor_newer_)) {
    fail(Error::Malformed, kBadFields);
    return false;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!check(project_->append_order(ids[i]), "SONG: pattern id 0")) return false;
  }
  return true;
}

bool Reader::parse_inst(std::size_t len) noexcept {
  Cursor c(scratch_, len);
  (void)c.u16();  // the version: checked when the chunk header was read
  model::init_instrument(inst_, model::InstrumentKind::Sample);
  const std::uint32_t index = c.u8();
  const std::uint32_t kind = c.u8();
  inst_.midi_channel = c.u8();
  inst_.module_index = c.u8();
  const std::uint8_t* text = nullptr;
  std::size_t text_len = 0;
  if (c.text(model::kInstrumentNameMax, text, text_len)) {
    std::memcpy(inst_.name, text, text_len);
  }
  if (c.text(model::kSamplePathMax, text, text_len)) {
    std::memcpy(inst_.sample_path, text, text_len);
  }
  decode_sampler(c, inst_.sampler);
  inst_.param_len = c.u16();
  if (!c.bad() && inst_.param_len > model::kModuleParamBytes) {
    fail(Error::Malformed, "INST: module parameter blob too long");
    return false;
  }
  const std::uint8_t* blob = c.take(inst_.param_len);
  inst_.sample_rate = chunk_minor_ >= 1 ? c.u32() : 0;  // version 1.1 appends it; 1.0 did not record it
  inst_.engine = chunk_minor_ >= 2 ? c.u8() : std::uint8_t{0};  // version 1.2 appends it; before, the engine was automatic
  if (inst_.engine > model::kInstEngineMax) {
    inst_.engine = model::kInstEngineAuto;  // an engine a later version added: this one plays the instrument as it would have without the choice
  }
  if (chunk_minor_ >= 3) {  // version 1.3 appends the MIDI fields and VEL>; before, they were the defaults (init_instrument). A value out of its range is its default, as the engine is (a later version may add values)
    const std::uint8_t velocity = c.u8();
    const std::uint8_t program = c.u8();
    const std::uint8_t out = c.u8();
    const std::uint8_t pb_range = c.u8();
    const std::uint8_t offset = c.u8();
    std::uint8_t cc[model::kMidiCcSlotsPerInstrument];
    for (std::uint8_t& n : cc) {
      n = c.u8();
    }
    const std::uint8_t vel_sens = c.u8();
    inst_.midi_velocity = velocity >= 1u && velocity <= 127u ? velocity : model::kMidiVelocityDefault;
    inst_.midi_program = program <= model::kMidiProgramMax ? program : model::kMidiProgramNone;
    inst_.midi_out = out <= model::kMidiOutMax ? out : model::kMidiOutConfig;
    inst_.midi_pb_range = pb_range <= model::kMidiPbRangeMax ? pb_range : model::kMidiPbRangeDefault;
    inst_.midi_offset = offset <= model::kMidiOffsetMaxMs ? offset : std::uint8_t{0};
    for (unsigned i = 0; i < model::kMidiCcSlotsPerInstrument; ++i) {
      inst_.midi_cc[i] = cc[i] <= model::kMidiCcMax ? cc[i] : model::kMidiCcConfig;
    }
    inst_.vel_sens = vel_sens <= model::kMaxPercent ? vel_sens : model::kVelSensDefault;
  }
  for (model::Lfo& l : inst_.sampler.lfo) {  // OS-LFOSINE: the shapes are 0..4 before INST 1.4 and 0..5 from it; a larger one (one this version does not have, or a stray 5 in an older chunk) plays as Triangle, as the engine has always held it
    const std::uint8_t last = chunk_minor_ >= 4 ? model::kLfoShapeSine : std::uint8_t{4};
    l.shape = l.shape > last ? model::kLfoShapeTriangle : l.shape;
  }
  if (!fields_ok(c, chunk_minor_newer_)) {
    fail(Error::Malformed, kBadFields);
    return false;
  }
  if (inst_.param_len != 0) {
    std::memcpy(inst_.params, blob, inst_.param_len);
  }
  if (index >= model::kMaxInstruments || kind < 1 || kind > 3) {
    fail(Error::Malformed, "INST: bad slot index or kind");
    return false;
  }
  if (project_->instrument(index) != nullptr) {
    fail(Error::Malformed, "INST: duplicate slot");
    return false;
  }
  inst_.kind = static_cast<model::InstrumentKind>(kind);
  if (const char* why = model::validate_instrument(inst_, project_->module_count())) {
    fail(Error::Malformed, why);
    return false;
  }
  if (!check(project_->set_instrument(index, inst_), "INST: rejected by the model")) return false;
  ++report_.instruments;
  return true;
}

// SETS and LRNB (OS-002d): decoded into the Reader's block, which starts as the defaults and which the project then takes whole.
// Entries it cannot use are counted and skipped, the setting keeps its default; only a chunk whose length does not fit its count fails.
bool Reader::parse_settings(std::size_t len, bool bindings) noexcept {
  Cursor c(scratch_, len);
  settings_chunks::Counts counts;
  const bool shape = bindings ? settings_chunks::decode_lrnb(c, chunk_minor_newer_, settings_, counts)
                              : settings_chunks::decode_sets(c, chunk_minor_newer_, settings_, counts);
  if (!shape) {
    fail(Error::Malformed, bindings ? "LRNB does not fit its entry count" : "SETS does not fit its entry count");
    return false;
  }
  report_.settings_skipped += counts.skipped;
  return check(project_->set_settings(settings_), "the project's settings break a model invariant");
}

// MIDI, USBS, MIXR, PERF and APPM (OS-004d): strict, like every known chunk (state_chunks::decode). NOTE, MUTE, SCEN and SNFX (OS-PERSIST2): tolerant of a value out of its range (its default,
// counted with the settings entries that were skipped).
bool Reader::parse_state(std::size_t len, unsigned part) noexcept {
  Cursor c(scratch_, len);
  std::uint32_t reset = 0;
  if (const char* why = state_chunks::decode(c, chunk_minor_newer_, *project_, static_cast<state_chunks::Part>(part), reset)) {
    fail(Error::Malformed, why);
    return false;
  }
  report_.settings_skipped += reset;
  if (part == static_cast<unsigned>(state_chunks::Part::Law)) {
    law_ = scratch_[2];  // validated (0 or 1) by decode; applied at Done, so a load that fails later commits nothing
  }
  return true;
}

bool Reader::parse_crc(std::size_t len) noexcept {
  Cursor c(scratch_, len);
  (void)c.u16();  // the version: checked when the chunk header was read
  const std::uint32_t stored = c.u32();
  if (!fields_ok(c, chunk_minor_newer_)) {
    fail(Error::Malformed, kBadFields);
    return false;
  }
  if (stored != crc_before_chunk_.value()) {
    fail(Error::BadCrc, "the CRC chunk does not match the file");
    return false;
  }
  return true;
}

// ---- Verifier (OS-QAFIX-4) -------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The Reader's framing checks, nothing of its content: see the class comment in native.h. The tables (rank_of, known_minor, v1_bounds) are the Reader's.

static_assert(kPhdrMax <= Verifier::kScratch && kCrcPayload <= Verifier::kScratch, "the verifier's scratch holds the FORM header, a chunk header, the PHDR and the CRC chunk");

void Verifier::begin(core::ByteSource& source) noexcept {
  source_ = &source;
  state_ = State::FormHeader;
  progress_ = Progress::Running;
  report_ = Report{};
  crc_.reset();
  crc_before_chunk_.reset();
  want_ = kFormHeader;
  have_ = 0;
  form_left_ = 0;
  chunk_id_ = 0;
  chunk_len_ = 0;
  skip_left_ = 0;
  expected_patterns_ = 0;
  expected_instruments_ = 0;
  rank_ = 0;
  chunk_pad_ = 0;
  chunk_minor_newer_ = false;
  chunk_minor_ = 0;
}

void Verifier::fail(Error e, const char* detail) noexcept {
  report_.error = e;
  report_.detail = detail;
  report_.offset = report_.bytes;
  progress_ = Progress::Failed;
  state_ = State::Failed;
  want_ = 0;
  have_ = 0;
}

bool Verifier::pull(std::size_t& budget) noexcept {
  while (have_ < want_) {
    if (budget == 0) {
      return false;
    }
    const std::size_t ask = std::min(want_ - have_, budget);
    const core::IoResult r = source_->read(scratch_ + have_, ask);
    switch (r.status) {
      case core::IoStatus::Ok:
        if (r.count == 0 || r.count > ask) {
          fail(Error::SourceFailed, "source broke its contract");
          return false;
        }
        crc_.update(scratch_ + have_, r.count);
        have_ += r.count;
        budget -= r.count;
        report_.bytes += static_cast<std::uint32_t>(r.count);
        break;
      case core::IoStatus::WouldBlock:
        return false;
      case core::IoStatus::End:
        fail(Error::Truncated, "the file ends before its FORM does");
        return false;
      case core::IoStatus::Failed:
      default:
        fail(Error::SourceFailed, "source failed");
        return false;
    }
  }
  return true;
}

Progress Verifier::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    if (have_ < want_ && !pull(budget)) {
      break;  // out of budget or would block (still Running), or fail() has set progress_
    }
    advance();
  }
  return progress_;
}

void Verifier::advance() noexcept {
  switch (state_) {
    case State::FormHeader: {
      if (get32(scratch_) != kForm || get32(scratch_ + 8) != kTypeO3pj) {
        fail(Error::BadMagic, "not an O3PJ project file");
        return;
      }
      const std::uint32_t len = get32(scratch_ + 4);
      if (len > kMaxFileBytes - 8u) {
        fail(Error::LimitExceeded, "file larger than the limit");
        return;
      }
      if (len < 4 || (len & 1u) != 0) {
        fail(Error::Malformed, "bad FORM length");
        return;
      }
      form_left_ = len - 4;
      begin_chunk_header();
      return;
    }
    case State::ChunkHeader:
      on_chunk_header();
      return;
    case State::Version: {
      const std::uint16_t version = get16(scratch_);
      const std::uint32_t major = static_cast<std::uint32_t>(version) >> 8;
      const std::uint32_t minor = version & 0xFFu;
      if (major == 0) {
        fail(Error::Malformed, "chunk version 0");
        return;
      }
      if (major > (kChunkVersion >> 8)) {
        fail(Error::NewerMajor, "chunk written by a newer major version of the format");
        return;
      }
      chunk_minor_ = static_cast<std::uint8_t>(minor);
      chunk_minor_newer_ = minor > known_minor(chunk_id_);
      std::size_t lo = 0;
      std::size_t hi = 0;
      v1_bounds(chunk_id_, chunk_minor_, lo, hi);
      if (chunk_len_ < lo || (!chunk_minor_newer_ && chunk_len_ > hi)) {
        fail(Error::Malformed, "chunk length out of range");
        return;
      }
      if (chunk_id_ == kChunkPatt) {
        ++report_.patterns;
      } else if (chunk_id_ == kChunkInst) {
        ++report_.instruments;
      }
      if (chunk_id_ == kChunkPhdr || chunk_id_ == kChunkCrc) {  // the two the verifier reads: the payload whole (or the part this reader knows), the version bytes already in scratch_
        state_ = State::Payload;
        want_ = std::min<std::size_t>(chunk_len_, hi);
      } else {
        enter_skip(chunk_len_ - 2u);
      }
      return;
    }
    case State::Payload:
      on_payload();
      return;
    case State::Skip:
      skip_left_ -= static_cast<std::uint32_t>(want_);
      if (skip_left_ == 0) {
        end_payload();
      } else {
        want_ = std::min<std::size_t>(skip_left_, kScratch);
        have_ = 0;
      }
      return;
    case State::Pad:
      if (scratch_[0] != 0) {
        fail(Error::Malformed, "pad byte is not zero");
        return;
      }
      end_chunk();
      return;
    case State::Done:
    case State::Failed:
      return;
  }
}

void Verifier::begin_chunk_header() noexcept {
  if (form_left_ == 0) {
    fail(Error::Malformed, "the FORM ends without a CRC chunk");
    return;
  }
  if (form_left_ < kChunkHeader) {
    fail(Error::Malformed, "chunk header overruns the FORM");
    return;
  }
  crc_before_chunk_ = crc_;
  state_ = State::ChunkHeader;
  want_ = kChunkHeader;
  have_ = 0;
}

void Verifier::on_chunk_header() noexcept {
  const std::uint32_t id = get32(scratch_);
  const std::uint32_t len = get32(scratch_ + 4);
  form_left_ -= static_cast<std::uint32_t>(kChunkHeader);
  chunk_id_ = id;
  chunk_len_ = len;
  report_.chunk = id;
  chunk_pad_ = static_cast<std::uint8_t>(len & 1u);
  if (len > form_left_ || len + chunk_pad_ > form_left_) {
    fail(Error::Malformed, "chunk overruns the FORM");
    return;
  }
  form_left_ -= len + chunk_pad_;
  if (rank_ == 0 && id != kChunkPhdr) {
    fail(Error::Malformed, "the first chunk must be PHDR");
    return;
  }
  const std::uint8_t rank = rank_of(id);
  if (rank != 0) {
    if (rank < rank_ || (rank == rank_ && rank != kRankPatt && rank != kRankInst)) {
      fail(Error::Malformed, "chunk out of order or repeated");
      return;
    }
    rank_ = rank;
    if (len < 2) {
      fail(Error::Malformed, "chunk length out of range");
      return;
    }
    state_ = State::Version;
    want_ = 2;
    have_ = 0;
    return;
  }
  if (!model::is_printable_fourcc(id)) {
    fail(Error::Malformed, "chunk id is not four printable characters");
    return;
  }
  enter_skip(len);
}

void Verifier::on_payload() noexcept {
  const std::size_t have = want_;
  Cursor c(scratch_, have);
  (void)c.u16();  // the version: checked
  if (chunk_id_ == kChunkPhdr) {
    (void)c.u8();   // active tracks
    (void)c.u8();   // default pattern length
    (void)c.u32();  // tempo
    (void)c.u8();   // swing
    for (unsigned i = 0; i < 5; ++i) {
      (void)c.u8();  // the master settings
    }
    const std::uint32_t patterns = c.u16();
    const std::uint32_t instruments = c.u8();
    const std::uint32_t modules = c.u8();
    const std::uint8_t* name = nullptr;
    std::size_t name_len = 0;
    (void)c.text(model::kProjectNameMax, name, name_len);
    if (!fields_ok(c, chunk_minor_newer_)) {
      fail(Error::Malformed, kBadFields);
      return;
    }
    if (patterns > model::kMaxPatterns || instruments > model::kMaxInstruments || modules > model::kMaxModules) {
      fail(Error::LimitExceeded, "the header announces more patterns, instruments or modules than the model holds");
      return;
    }
    expected_patterns_ = patterns;
    expected_instruments_ = instruments;
  } else {  // the CRC chunk
    const std::uint32_t stored = c.u32();
    if (!fields_ok(c, chunk_minor_newer_)) {
      fail(Error::Malformed, kBadFields);
      return;
    }
    if (stored != crc_before_chunk_.value()) {
      fail(Error::BadCrc, "the CRC chunk does not match the file");
      return;
    }
  }
  enter_skip(chunk_len_ - static_cast<std::uint32_t>(have));
}

void Verifier::enter_skip(std::uint32_t n) noexcept {
  skip_left_ = n;
  if (n == 0) {
    end_payload();
    return;
  }
  state_ = State::Skip;
  want_ = std::min<std::size_t>(n, kScratch);
  have_ = 0;
}

void Verifier::end_payload() noexcept {
  if (chunk_pad_ != 0) {
    state_ = State::Pad;
    want_ = 1;
    have_ = 0;
  } else {
    end_chunk();
  }
}

void Verifier::end_chunk() noexcept {
  if (chunk_id_ == kChunkCrc) {
    finish();
  } else {
    begin_chunk_header();
  }
}

void Verifier::finish() noexcept {
  if (form_left_ != 0) {
    fail(Error::Malformed, "data after the CRC chunk");
    return;
  }
  if (report_.patterns != expected_patterns_ || report_.instruments != expected_instruments_) {
    fail(Error::Malformed, "chunk counts differ from the header");
    return;
  }
  state_ = State::Done;
  progress_ = Progress::Done;
}

}  // namespace o3os::formats::native
