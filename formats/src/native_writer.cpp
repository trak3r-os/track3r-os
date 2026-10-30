// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/native.h>

#include "native_codec.h"
#include "native_state.h"
#include "settings_chunks.h"

#include <algorithm>

namespace o3os::formats::native {

using namespace codec;  // NOLINT: the shared field cursors and size formulas
using model::Instrument;
using model::Pattern;
using model::Project;

namespace {

std::size_t phdr_size(const Project& p) noexcept { return kPhdrFixed + text_len(p.name(), model::kProjectNameMax); }

std::size_t trak_size(const Project& p) noexcept {
  std::size_t n = kTrakFixed;
  for (std::uint32_t i = 0; i < model::kMaxTracks; ++i) {
    n += kTrakSlotFixed + text_len(p.track(i)->name, model::kTrackNameMax);
  }
  return n;
}

std::size_t modr_size(const Project& p) noexcept {
  std::size_t n = kModrFixed;
  for (std::uint32_t i = 0; i < p.module_count(); ++i) {
    n += kModrModuleFixed + text_len(p.module(i)->name, model::kModuleNameMax);
  }
  return n;
}

std::size_t song_size(const Project& p) noexcept { return kSongFixed + p.song_length(); }

// INST 1.1; 1.2 (one byte more) for an instrument that chose its playback engine (OS-004d); 1.3 (the engine byte always, and twelve more) for an instrument one of whose MIDI fields or VEL> is not its default
// (OS-INSTUI S8b + S9). The version is the smallest one that holds what the instrument has, so a project that never used the newer fields is the file it was.
// 1.4 (OS-LFOSINE: the 1.3 layout, whole) for an instrument with a Sine LFO.
// 1.5 (OS-STEREO step 2: the 1.3 layout, whole, and the stereo byte) for a Sample instrument with the stereo flag; it covers a Sine LFO too (1.5 is above the 1.4 gate).
std::uint16_t inst_version(const Instrument& in) noexcept {
  if (in.stereo != 0 && in.kind == model::InstrumentKind::Sample) {
    return kInstVersionStereo;
  }
  if (model::inst_uses_sine(in)) {
    return kInstVersionLfoSine;
  }
  return !model::inst_v13_default(in) ? kInstVersionMidi : (in.engine != model::kInstEngineAuto ? kInstVersionEngine : kInstVersion);
}

std::size_t inst_size(const Instrument& in) noexcept {
  const std::uint16_t v = inst_version(in);
  return kInstFixed + text_len(in.name, model::kInstrumentNameMax) + text_len(in.sample_path, model::kSamplePathMax) + in.param_len + kInstRate + (v >= kInstVersionEngine ? kInstEngine : 0) +
         (v >= kInstVersionMidi ? kInstMidi : 0) + (v >= kInstVersionStereo ? kInstStereo : 0);
}

void put_header(Enc& e, std::uint32_t id, std::size_t payload) noexcept {
  e.u32(id);
  e.u32(static_cast<std::uint32_t>(payload));
}

void put_pad(Enc& e, std::size_t payload) noexcept {
  if ((payload & 1u) != 0) {
    e.u8(0);
  }
}

}  // namespace

void Writer::begin(const Project& project, core::ByteSink& sink) noexcept {
  project_ = &project;
  sink_ = &sink;
  progress_ = Progress::Running;
  report_ = Report{};
  crc_.reset();
  unit_ = Unit::Measure;
  phase_ = Phase::Fixed;
  sized_ = false;
  generation_ = project.generation();
  form_len_ = 0;
  built_ = 0;
  cursor_ = 0;
  sub_ = 0;
  measure_total_ = 0;
  measure_id_ = 0;
  measure_track_ = 0;
  measure_payload_ = 0;
  patt_sized_ = 0;
  patt_written_ = 0;
  piece_len_ = 0;
  piece_off_ = 0;
}

// One slice of the sizing pre-pass: adds up the FORM length (the form type, then every chunk's header, payload
// and pad) item by item. A call does at least one item and goes on until `budget` bytes of project data have
// been looked at; a pattern track costs the steps scanned for its trailing empty ones, so a call reads at
// most one track (896 bytes) past the budget. Returns true when the size is known.
bool Writer::measure(std::size_t& budget) noexcept {
  const Project& p = *project_;
  std::size_t spent = 0;
  bool first = true;
  while (phase_ != Phase::Done && (first || spent < budget)) {
    first = false;
    switch (phase_) {
      case Phase::Fixed:
        measure_total_ = 4 + chunk_total(phdr_size(p)) + chunk_total(trak_size(p)) + chunk_total(modr_size(p)) +
                         chunk_total(song_size(p)) + chunk_total(kCrcPayload);
        spent += 256;  // names and tables, a few hundred bytes
        phase_ = Phase::Patterns;
        measure_id_ = 1;
        measure_track_ = 0;
        break;

      case Phase::Patterns: {
        while (measure_id_ <= model::kMaxPatternId && p.pattern(measure_id_) == nullptr) {
          ++measure_id_;
        }
        if (measure_id_ > model::kMaxPatternId) {
          phase_ = Phase::Instruments;
          measure_id_ = 0;
          break;
        }
        const Pattern& pat = *p.pattern(measure_id_);
        if (measure_track_ == 0) {
          measure_payload_ = kPattHead + text_len(pat.name, model::kPatternNameMax);
        }
        const std::uint32_t n = stored_steps(pat, measure_track_);
        measure_payload_ += kPattTrackFixed + n * sizeof(model::Step);
        spent += (model::kMaxPatternLength - n + (n > 0 ? 1u : 0u)) * sizeof(model::Step);
        if (++measure_track_ == model::kMaxTracks) {
          measure_total_ += chunk_total(measure_payload_);
          patt_payload_of_[patt_sized_++] = static_cast<std::uint16_t>(measure_payload_);  // <= 14,403
          ++measure_id_;
          measure_track_ = 0;
        }
        break;
      }

      case Phase::Instruments:
        while (measure_id_ < model::kMaxInstruments && p.instrument(measure_id_) == nullptr) {
          ++measure_id_;
        }
        if (measure_id_ >= model::kMaxInstruments) {
          // OS-002d: SETS when a project setting is not its default, LRNB when a binding is in use
          if (!p.settings().values_default()) {
            measure_total_ += chunk_total(settings_chunks::sets_payload_size(p.settings()));
          }
          if (p.settings().learn_count() != 0) {
            measure_total_ += chunk_total(settings_chunks::lrnb_payload_size(p.settings()));
          }
          for (unsigned part = 0; part < state_chunks::kParts; ++part) {  // OS-004d
            const std::size_t n = state_chunks::payload_size(p, static_cast<state_chunks::Part>(part));
            if (n != 0) {
              measure_total_ += chunk_total(n);
            }
          }
          phase_ = Phase::UnknownChunks;
          measure_id_ = 0;
          break;
        }
        measure_total_ += chunk_total(inst_size(*p.instrument(measure_id_)));
        spent += 128;  // the two names
        ++measure_id_;
        break;

      case Phase::UnknownChunks:
        if (measure_id_ >= p.unknown_count()) {
          form_len_ = measure_total_;
          phase_ = Phase::Done;
          break;
        }
        measure_total_ += chunk_total(p.unknown(measure_id_)->length);
        spent += 8;
        ++measure_id_;
        break;

      case Phase::Done:
        break;
    }
  }
  budget -= std::min(budget, spent);
  if (phase_ == Phase::Done) {
    sized_ = true;
  }
  return sized_;
}

void Writer::fail(Error e, const char* detail) noexcept {
  report_.error = e;
  report_.detail = detail;
  report_.offset = report_.bytes;
  progress_ = Progress::Failed;
}

// Builds the next piece (at most kScratchBytes, never empty) into scratch_. False only on an error, which
// has then been recorded.
bool Writer::build_piece() noexcept {
  const Project& p = *project_;
  Enc e(scratch_, kScratchBytes);
  bool is_crc = false;
  bool produced = false;

  while (!produced) {
    switch (unit_) {
      case Unit::Form:
        e.u32(kForm);
        e.u32(form_len_);
        e.u32(kTypeO3pj);
        unit_ = Unit::Phdr;
        produced = true;
        break;

      case Unit::Phdr: {
        const std::size_t nl = text_len(p.name(), model::kProjectNameMax);
        const std::size_t payload = kPhdrFixed + nl;
        report_.chunk = kChunkPhdr;
        put_header(e, kChunkPhdr, payload);
        e.u16(kChunkVersion);
        e.u8(p.active_tracks());
        e.u8(p.default_pattern_length());
        e.u32(p.tempo_x100());
        e.u8(p.swing());
        const model::MasterSettings& m = p.master();
        e.u8(m.volume);
        e.u8(m.dry_volume);
        e.u8(m.reverb_volume);
        e.u8(m.delay_volume);
        e.u8(m.flags);
        e.u16(static_cast<std::uint16_t>(p.pattern_count()));
        e.u8(static_cast<std::uint8_t>(p.instrument_count()));
        e.u8(static_cast<std::uint8_t>(p.module_count()));
        e.text(p.name(), nl);
        put_pad(e, payload);
        unit_ = Unit::Trak;
        produced = true;
        break;
      }

      case Unit::Trak: {
        const std::size_t payload = trak_size(p);
        report_.chunk = kChunkTrak;
        put_header(e, kChunkTrak, payload);
        e.u16(kChunkVersion);
        e.u8(static_cast<std::uint8_t>(model::kMaxTracks));
        for (std::uint32_t i = 0; i < model::kMaxTracks; ++i) {
          const model::TrackSlot& t = *p.track(i);
          e.u8(static_cast<std::uint8_t>(t.type));
          e.u8(t.flags);
          e.u8(t.volume);
          e.u8(t.pan);
          e.u8(t.bus);
          e.text(t.name, text_len(t.name, model::kTrackNameMax));
        }
        put_pad(e, payload);
        unit_ = Unit::Modr;
        produced = true;
        break;
      }

      case Unit::Modr: {
        const std::size_t payload = modr_size(p);
        report_.chunk = kChunkModr;
        put_header(e, kChunkModr, payload);
        e.u16(kChunkVersion);
        e.u8(static_cast<std::uint8_t>(p.module_count()));
        for (std::uint32_t i = 0; i < p.module_count(); ++i) {
          const model::ModuleRef& m = *p.module(i);
          e.u8(m.abi_major);
          e.u8(m.abi_minor);
          e.text(m.name, text_len(m.name, model::kModuleNameMax));
        }
        put_pad(e, payload);
        unit_ = Unit::Song;
        produced = true;
        break;
      }

      case Unit::Song: {
        const std::size_t payload = song_size(p);
        report_.chunk = kChunkSong;
        put_header(e, kChunkSong, payload);
        e.u16(kChunkVersion);
        e.u8(static_cast<std::uint8_t>(p.song_length()));
        for (std::uint32_t i = 0; i < p.song_length(); ++i) {
          e.u8(p.order(i));
        }
        put_pad(e, payload);
        unit_ = Unit::Patt;
        cursor_ = 1;
        sub_ = 0;
        produced = true;
        break;
      }

      case Unit::Patt: {
        if (sub_ == 0) {
          while (cursor_ <= model::kMaxPatternId && p.pattern(cursor_) == nullptr) {
            ++cursor_;
          }
          if (cursor_ > model::kMaxPatternId) {
            unit_ = Unit::Inst;
            cursor_ = 0;
            break;
          }
        }
        const Pattern& pat = *p.pattern(cursor_);
        report_.chunk = kChunkPatt;
        if (sub_ == 0) {  // header and name; the payload size was worked out by the sizing pre-pass
          const std::size_t nl = text_len(pat.name, model::kPatternNameMax);
          patt_payload_ = patt_payload_of_[patt_written_];
          put_header(e, kChunkPatt, patt_payload_);
          e.u16(kChunkVersion);
          e.u8(pat.id);
          e.u8(static_cast<std::uint8_t>(model::kMaxTracks));
          e.text(pat.name, nl);
          sub_ = 1;
        } else {  // one track: its length, the stored step count and the steps
          const std::uint32_t t = sub_ - 1;
          const std::uint32_t n = stored_steps(pat, t);
          e.u8(pat.track_length[t]);
          e.u8(static_cast<std::uint8_t>(n));
          e.bytes(pat.steps[t], n * sizeof(model::Step));  // the in-memory step layout is the wire layout
          if (t + 1 == model::kMaxTracks) {
            put_pad(e, patt_payload_);
            ++cursor_;
            ++patt_written_;
            sub_ = 0;
          } else {
            ++sub_;
          }
        }
        produced = true;
        break;
      }

      case Unit::Inst: {
        while (cursor_ < model::kMaxInstruments && p.instrument(cursor_) == nullptr) {
          ++cursor_;
        }
        if (cursor_ >= model::kMaxInstruments) {
          unit_ = Unit::Sets;
          cursor_ = 0;
          sub_ = 0;
          break;
        }
        const Instrument& in = *p.instrument(cursor_);
        const std::size_t payload = inst_size(in);
        report_.chunk = kChunkInst;
        put_header(e, kChunkInst, payload);
        const std::uint16_t version = inst_version(in);
        e.u16(version);
        e.u8(static_cast<std::uint8_t>(cursor_));
        e.u8(static_cast<std::uint8_t>(in.kind));
        e.u8(in.midi_channel);
        e.u8(in.module_index);
        e.text(in.name, text_len(in.name, model::kInstrumentNameMax));
        e.text(in.sample_path, text_len(in.sample_path, model::kSamplePathMax));
        encode_sampler(e, in.sampler);
        e.u16(in.param_len);
        e.bytes(in.params, in.param_len);
        e.u32(in.sample_rate);
        if (version >= kInstVersionEngine) {
          e.u8(in.engine);
        }
        if (version >= kInstVersionMidi) {
          e.u8(in.midi_velocity);
          e.u8(in.midi_program);
          e.u8(in.midi_out);
          e.u8(in.midi_pb_range);
          e.u8(in.midi_offset);
          for (const std::uint8_t cc : in.midi_cc) {
            e.u8(cc);
          }
          e.u8(in.vel_sens);
        }
        if (version >= kInstVersionStereo) {
          e.u8(in.stereo);
        }
        put_pad(e, payload);
        ++cursor_;
        produced = true;
        break;
      }

      case Unit::Sets: {
        unit_ = Unit::Lrnb;
        if (p.settings().values_default()) {
          break;  // nothing to say: every project setting is its default
        }
        const std::size_t payload = settings_chunks::sets_payload_size(p.settings());
        report_.chunk = kChunkSets;
        put_header(e, kChunkSets, payload);
        settings_chunks::encode_sets(e, p.settings());
        put_pad(e, payload);
        produced = true;
        break;
      }

      case Unit::Lrnb: {
        unit_ = Unit::State;
        cursor_ = 0;
        if (p.settings().learn_count() == 0) {
          break;
        }
        const std::size_t payload = settings_chunks::lrnb_payload_size(p.settings());
        report_.chunk = kChunkLrnb;
        put_header(e, kChunkLrnb, payload);
        settings_chunks::encode_lrnb(e, p.settings());
        put_pad(e, payload);
        produced = true;
        break;
      }

      case Unit::State: {  // OS-004d: MIDI, USBS, MIXR, PERF, APPM, each only when the project has it
        if (cursor_ >= state_chunks::kParts) {
          unit_ = Unit::Unknown;
          cursor_ = 0;
          sub_ = 0;
          break;
        }
        const state_chunks::Part part = static_cast<state_chunks::Part>(cursor_++);
        const std::size_t payload = state_chunks::payload_size(p, part);
        if (payload == 0) {
          break;
        }
        report_.chunk = state_chunks::chunk_id(part);
        put_header(e, state_chunks::chunk_id(part), payload);
        state_chunks::encode(e, p, part);
        put_pad(e, payload);
        produced = true;
        break;
      }

      case Unit::Unknown: {
        if (cursor_ >= p.unknown_count()) {
          unit_ = Unit::Crc;
          break;
        }
        const model::UnknownChunk& u = *p.unknown(cursor_);
        report_.chunk = u.id;
        if (sub_ == 0) {
          put_header(e, u.id, u.length);
        }
        const std::size_t take = std::min<std::size_t>(u.length - sub_, kScratchBytes - e.size() - 1);
        e.bytes(p.unknown_data(cursor_) + sub_, take);
        sub_ += static_cast<std::uint32_t>(take);
        if (sub_ == u.length) {
          put_pad(e, u.length);
          ++cursor_;
          sub_ = 0;
        }
        produced = true;
        break;
      }

      case Unit::Crc:
        if (built_ + chunk_total(kCrcPayload) != form_len_ + 8u) {
          fail(Error::Internal, "writer size formulas disagree with the output");
          return false;
        }
        report_.chunk = kChunkCrc;
        put_header(e, kChunkCrc, kCrcPayload);
        e.u16(kChunkVersion);
        e.u32(crc_.value());  // of every byte before this chunk's header
        unit_ = Unit::End;
        is_crc = true;
        produced = true;
        break;

      case Unit::Measure:
      case Unit::End:
        return false;  // not reached: step() sizes first and stops at End
    }
  }

  if (e.overflow()) {
    fail(Error::Internal, "writer piece larger than its buffer");
    return false;
  }
  piece_len_ = e.size();
  piece_off_ = 0;
  built_ += static_cast<std::uint32_t>(piece_len_);
  if (!is_crc) {
    crc_.update(scratch_, piece_len_);
  }
  return true;
}

Progress Writer::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    if (unit_ == Unit::Measure) {  // the sizing pre-pass: nothing is written yet
      if (budget == 0) {
        return Progress::Running;
      }
      if (project_->generation() != generation_) {
        fail(Error::Modified, "the project changed while it was being saved");
        break;
      }
      if (!measure(budget)) {
        return Progress::Running;  // the budget is spent
      }
      unit_ = Unit::Form;
      continue;
    }
    if (piece_off_ == piece_len_) {  // nothing pending: the last piece is out, or none was built yet
      if (unit_ == Unit::End) {
        progress_ = Progress::Done;
        break;
      }
      if (budget == 0) {
        return Progress::Running;
      }
      if (project_->generation() != generation_) {
        fail(Error::Modified, "the project changed while it was being saved");
        break;
      }
      if (!build_piece()) {
        break;  // failed, recorded
      }
    }
    if (budget == 0) {
      return Progress::Running;
    }
    const std::size_t ask = std::min(piece_len_ - piece_off_, budget);
    const core::IoResult r = sink_->write(scratch_ + piece_off_, ask);
    if (r.status == core::IoStatus::WouldBlock) {
      return Progress::Running;
    }
    if (r.status != core::IoStatus::Ok || r.count == 0 || r.count > ask) {
      fail(Error::SinkFailed, r.status == core::IoStatus::Ok ? "sink broke its contract" : "sink failed");
      break;
    }
    piece_off_ += r.count;
    budget -= r.count;
    report_.bytes += static_cast<std::uint32_t>(r.count);
  }
  return progress_;
}

}  // namespace o3os::formats::native
