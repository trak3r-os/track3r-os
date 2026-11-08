// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/med/import.h>

#include <algorithm>
#include <cstring>

namespace o3os::formats::med {

namespace {

inline std::uint32_t sat_add(std::uint32_t a, std::uint32_t b) noexcept { return a + b < a ? ~0u : a + b; }

// "<block name> @<first line>": the name of a piece that does not start its block, cut to the model's name length.
std::size_t piece_name(const char* block_name, std::uint32_t first, char* out) noexcept {
  char tail[8];
  std::size_t tn = 0;
  if (first != 0) {
    char digits[6];
    std::size_t dn = 0;
    for (std::uint32_t v = first; v != 0 && dn < sizeof digits; v /= 10u) {
      digits[dn++] = static_cast<char>('0' + v % 10u);
    }
    tail[tn++] = '@';
    while (dn != 0) {
      tail[tn++] = digits[--dn];
    }
  }
  std::size_t bn = std::min<std::size_t>(std::strlen(block_name), model::kPatternNameMax);
  if (first != 0) {
    bn = std::min<std::size_t>(bn, model::kPatternNameMax - tn - (bn != 0 ? 1u : 0u));
  }
  std::memcpy(out, block_name, bn);
  std::size_t n = bn;
  if (first != 0) {
    if (n != 0) {
      out[n++] = ' ';
    }
    std::memcpy(out + n, tail, tn);
    n += tn;
  }
  return n;
}

}  // namespace

// OS-FLASHDIET I3: drop_group_name (with its table) lives in formats/src/tracker_sniff.cpp now, so the image links it from formats
// and this library does not have to be linked for the report's names.

// ---- the job ----------------------------------------------------------------------------------------------------------

void Importer::begin(RandomSource& source, model::ModelArena& arena, const ImportOptions& options) noexcept {
  src_ = &source;
  project_ = &arena.project;
  opt_ = options;
  project_->reset();
  report_.reset();
  linear_.reset();
  for (InstFact& f : facts_) {
    f = InstFact();
  }
  std::memset(plays_, 0, sizeof plays_);
  phase_ = Phase::Open;
  progress_ = Progress::Running;
  status_ = Status();
  pcm_left_ = caps::kSampleArenaBytes;
  pd_ = 0;
  line_ = 0;
  pd_open_ = false;
  slot_ = 0;
  work_cells_ = 0;
}

Progress Importer::step(std::size_t budget) noexcept {
  std::size_t work = 0;
  while (progress_ == Progress::Running && (work == 0 || work < budget)) {
    work += tick();
  }
  return progress_;
}

std::size_t Importer::tick() noexcept {
  switch (phase_) {
    case Phase::Open: return tick_open();
    case Phase::Validate: return tick_validate();
    case Phase::Flow: return tick_flow();
    case Phase::Scan: return tick_scan();
    case Phase::Header: return tick_header();
    case Phase::Instruments: return tick_instruments();
    case Phase::Patterns: return tick_patterns();
    case Phase::Order: return tick_order();
    case Phase::Done: break;
  }
  progress_ = Progress::Done;
  return 1;
}

void Importer::fail(Error e, std::uint32_t offset, const char* detail) noexcept {
  status_ = Status{e, offset, detail};
  progress_ = Progress::Failed;
  if (phase_ != Phase::Open && phase_ != Phase::Validate) {
    report_.flow = linear_.report;
  }
  if (e == Error::LimitExceeded && phase_ == Phase::Flow) {
    report_.overflow = true;
  }
  if (project_ != nullptr) {
    project_->reset();  // a failed whole-project import leaves the project valid and empty
  }
}

namespace {
constexpr std::size_t kOpenCost = 64;
constexpr std::size_t kValidateChunk = 64;
constexpr std::size_t kFlowChunk = 2048;
}  // namespace

std::size_t Importer::tick_open() noexcept {
  const Status s = reader_.open(*src_);
  if (!s.ok()) {
    fail_status(s);
    return 1;
  }
  sum_ = Summary();
  vstate_ = ValidateState();
  phase_ = Phase::Validate;
  return kOpenCost;
}

// The walk of the reader over everything the song reaches, a chunk of units at a time.
std::size_t Importer::tick_validate() noexcept {
  const Status s = reader_.validate_step(vstate_, sum_, kValidateChunk);
  if (!s.ok()) {
    fail_status(s);
    return kValidateChunk;
  }
  if (vstate_.phase != ValidateState::Phase::Done) {
    return kValidateChunk;
  }
  const SongInfo& song = reader_.song();
  report_.version = song.version;
  report_.ignored = sum_.ignored;
  report_.master_volume_ignored = song.master_volume != 0 && song.master_volume != 64;
  FlowOptions fo;
  fo.clock = opt_.clock;
  fo.truncate = opt_.overflow == Overflow::Truncate;
  flow_.begin(reader_, fo, linear_, false);
  phase_ = Phase::Flow;
  return kValidateChunk;
}

void Importer::count_plays() noexcept {
  for (std::uint32_t i = 0; i < linear_.order_count; ++i) {
    std::uint8_t& p = plays_[linear_.order[i]];
    p = p == 255 ? std::uint8_t{255} : static_cast<std::uint8_t>(p + 1);
  }
}

std::size_t Importer::tick_flow() noexcept {
  const std::uint32_t before = linear_.report.cells + linear_.report.settle_steps;
  const Progress p = flow_.step(kFlowChunk);
  const std::size_t work = 1u + (linear_.report.cells + linear_.report.settle_steps - before);
  if (p == Progress::Running) {
    return work;
  }
  if (p == Progress::Failed) {
    const Status s = flow_.status();
    if (s.error == Error::LimitExceeded && opt_.overflow == Overflow::Structural && !report_.structural) {
      // too much to linearise: patterns are the blocks, the order is the play list (flow commands are dropped and counted)
      report_.structural = true;
      FlowOptions fo;
      fo.clock = opt_.clock;
      flow_.begin(reader_, fo, linear_, true);
      return work;
    }
    fail_status(s);
    return work;
  }
  report_.flow = linear_.report;
  report_.patterns = linear_.pattern_count;
  report_.order = linear_.order_count;
  count_plays();
  work_cells_ = 0;
  pd_ = 0;
  pd_open_ = false;
  phase_ = Phase::Scan;
  return work;
}

// ---- scan: what each track carries ------------------------------------------------------------------------------------

std::size_t Importer::tick_scan() noexcept {
  if (pd_ >= linear_.pattern_count) {
    plan_tracks(report_);
    phase_ = Phase::Header;
    return 1;
  }
  const PatternDef& pd = linear_.patterns[pd_];
  if (!pd_open_) {
    const Status s = reader_.block(pd.block, blk_);
    if (!s.ok()) {
      fail_status(s);
      return 1;
    }
    if (blk_.empty || pd.last >= blk_.lines || pd.first > pd.last) {
      fail(Error::Malformed, blk_.offset, "a pattern definition outside its block");
      return 1;
    }
    pd_open_ = true;
    line_ = pd.first;
    return 1;
  }
  const std::uint32_t cost = piece_cost();
  work_cells_ = sat_add(work_cells_, cost);
  if (work_cells_ > opt_.max_scan_cells) {
    fail(Error::LimitExceeded, blk_.offset, "import: more cells scanned than the work cap");
    return cost;
  }
  const bool replay = (pd.flags & kPatReplay) != 0;
  const std::uint32_t w = std::max<std::uint32_t>(plays_[pd_], 1u);
  for (std::uint32_t page = 0; page <= blk_.pages; ++page) {
    const Status s = reader_.read_line(blk_, line_, page, cells_, false);
    if (!s.ok()) {
      fail_status(s);
      return cost;
    }
    for (std::uint32_t t = 0; t < blk_.tracks; ++t) {
      const Cell& c = cells_[t];
      const bool played = page == 0 && !replay;  // a repeated line (1E) has its notes cleared
      const bool note = played && (c.note != 0 || c.note_hi);
      const bool any = note || (played && c.instrument != 0) || c.command != 0 || c.data != 0;
      if (any) {
        TrackReport& tr = report_.track[t];
        tr.cells = sat_add(tr.cells, w);
        if (note) {
          tr.notes = sat_add(tr.notes, w);
        }
      }
    }
  }
  if (++line_ > pd.last) {
    ++pd_;
    pd_open_ = false;
  }
  return cost;
}

// ---- header, instruments ---------------------------------------------------------------------------------------------

std::size_t Importer::tick_header() noexcept {
  const SongInfo& song = reader_.song();
  model::Project& p = *project_;
  const std::size_t name_len = std::strlen(song.name);
  if (p.set_name(song.name, name_len) != model::Status::Ok) {
    fail(Error::Malformed, 0, "the model refused the song name");
    return 1;
  }
  const Bpm b = tempo_to_bpm(tempo_model(song, opt_.clock), linear_.initial_tempo, linear_.initial_spd);
  report_.initial_tempo_clamped = b.clamp != Clamp::None;
  if (p.set_tempo_x100(b.x100) != model::Status::Ok) {
    fail(Error::Malformed, 0, "the model refused the tempo");
    return 1;
  }
  const bool pans = reader_.has_track_pans();
  for (std::uint32_t t = 0; t < caps::kMaxTracks; ++t) {
    const std::uint8_t slot = report_.track[t].slot;
    if (slot == kNoSlot) {
      continue;
    }
    model::TrackSlot ts;
    std::memset(&ts, 0, sizeof ts);
    ts.type = model::TrackType::Audio;
    ts.flags = 0;
    ts.volume = track_volume_percent(reader_.track_volume(t));
    ts.pan = track_pan_percent(song, pans, reader_.track_pan(t), t);
    ts.bus = 0;
    if (p.set_track(slot, ts) != model::Status::Ok) {
      fail(Error::Malformed, 0, "the model refused a track");
      return 1;
    }
  }
  const std::uint32_t active = std::max<std::uint32_t>(report_.tracks_kept, 4u);  // the highest slot used, at least 4
  if (p.set_active_tracks(active) != model::Status::Ok) {
    fail(Error::Malformed, 0, "the model refused the track count");
    return 1;
  }
  slot_ = 0;
  phase_ = Phase::Instruments;
  return 8;
}

std::size_t Importer::tick_instruments() noexcept {
  const SongInfo& song = reader_.song();
  if (slot_ >= song.num_samples) {
    mapper_.begin(song, opt_, facts_, linear_.initial_tempo, linear_.initial_spd, report_.structural, report_);
    work_cells_ = 0;
    pd_ = 0;
    pd_open_ = false;
    phase_ = Phase::Patterns;
    return 1;
  }
  InstrumentInfo info;
  const Status s = reader_.instrument(slot_, info);
  if (!s.ok()) {
    fail_status(s);
    return 1;
  }
  InstReport& rep = report_.instrument[slot_];
  const bool made = map_instrument(reader_, slot_, info, opt_.clock, pcm_left_, scratch_, rep);
  if (made && rep.result == InstResult::Sample && !opt_.engine_fx) {
    scratch_.engine = model::kInstEngineModern;  // OS-BUG-MODENGINE: the OG / core mapping is for the Modern engine; AUTO at the Paula rate would load on Amiga
  }
  if (made) {
    if (project_->set_instrument(slot_, scratch_) != model::Status::Ok) {
      fail(Error::Malformed, info.offset, "the model refused an instrument");
      return 1;
    }
    InstFact& f = facts_[slot_ + 1];
    f.svol = std::min<std::uint8_t>(info.volume, 64);
    f.midi = rep.result == InstResult::Midi;
    f.frames = rep.frames;
    ++report_.instruments;
    if (rep.result == InstResult::Sample) {
      const std::uint64_t bytes = static_cast<std::uint64_t>(rep.frames) * 2u;
      pcm_left_ -= bytes;
      report_.pcm_bytes += bytes;
    }
  } else if (rep.result != InstResult::None && rep.result != InstResult::EmptyData) {
    ++report_.instruments_refused;
  }
  ++slot_;
  return 4;
}

// ---- patterns ----------------------------------------------------------------------------------------------------------

bool Importer::open_pattern() noexcept {
  const PatternDef& pd = linear_.patterns[pd_];
  Status s = reader_.block(pd.block, blk_);
  if (!s.ok()) {
    fail_status(s);
    return false;
  }
  if (blk_.empty || pd.last >= blk_.lines || pd.first > pd.last) {
    fail(Error::Malformed, blk_.offset, "a pattern definition outside its block");
    return false;
  }
  const std::uint32_t id = pd_ + 1u;
  if (project_->create_pattern(id, pd.last - pd.first + 1u) != model::Status::Ok) {
    fail(Error::Malformed, 0, "the model refused a pattern");
    return false;
  }
  const bool replay = (pd.flags & kPatReplay) != 0;
  if (!replay) {
    char block_name[caps::kBlockNameMax + 1] = {};
    s = reader_.block_name(blk_, block_name, sizeof block_name);
    if (!s.ok()) {
      fail_status(s);
      return false;
    }
    char name[model::kPatternNameMax + 1] = {};
    const std::size_t n = piece_name(block_name, pd.first, name);
    if (n != 0 && project_->set_pattern_name(id, name, n) != model::Status::Ok) {
      fail(Error::Malformed, 0, "the model refused a pattern name");
      return false;
    }
  }
  mapper_.begin_piece(pd.entry_tempo, pd.entry_spd, replay);
  slot_mask_ = 0;
  for (std::uint32_t t = 0; t < blk_.tracks; ++t) {
    if (report_.track[t].slot != kNoSlot) {
      slot_mask_ |= 1ull << t;
    }
  }
  line_ = pd.first;
  pd_open_ = true;
  return true;
}

// A Tempo, Speed or Break FX of a track without a slot goes onto the first track that has a free FX column at that line (the
// model's FX of these kinds are global). False when every track's two columns are taken.
bool Importer::carry(std::uint32_t pattern_id, std::uint32_t line, const model::StepFx& fx) noexcept {
  const model::Pattern* pt = project_->pattern(pattern_id);
  if (pt == nullptr) {
    return false;
  }
  for (std::uint32_t slot = 0; slot < report_.tracks_kept; ++slot) {
    model::Step st = pt->steps[slot][line];
    for (model::StepFx& f : st.fx) {
      if (f.type == model::kFxNone) {
        f = fx;
        return project_->set_step(pattern_id, slot, line, st) == model::Status::Ok;
      }
    }
  }
  return false;
}

std::size_t Importer::tick_patterns() noexcept {
  if (pd_ >= linear_.pattern_count) {
    phase_ = Phase::Order;
    return 1;
  }
  if (!pd_open_) {
    open_pattern();
    return 2;
  }
  const PatternDef& pd = linear_.patterns[pd_];
  const std::uint32_t cost = piece_cost();
  work_cells_ = sat_add(work_cells_, cost);
  if (work_cells_ > opt_.max_map_cells) {
    fail(Error::LimitExceeded, blk_.offset, "import: more cells mapped than the work cap");
    return cost;
  }
  mapper_.begin_line();
  for (std::uint32_t page = 0; page <= blk_.pages; ++page) {
    const Status s = reader_.read_line(blk_, line_, page, cells_, true);
    if (!s.ok()) {
      fail_status(s);
      return cost;
    }
    for (std::uint32_t t = 0; t < blk_.tracks; ++t) {
      if (report_.track[t].slot != kNoSlot) {
        mapper_.cell(t, cells_[t], page);
      } else {
        mapper_.timing_cell(t, cells_[t]);  // the rest of such a track is in the report as the track's cells: not counted twice
      }
    }
  }
  mapper_.end_line(slot_mask_);
  report_.cells = sat_add(report_.cells, cost);
  const std::uint32_t id = pd_ + 1u;
  const std::uint32_t at = line_ - pd.first;
  for (std::uint32_t t = 0; t < blk_.tracks; ++t) {
    const std::uint8_t slot = report_.track[t].slot;
    const model::Step& st = mapper_.step(t);
    if (slot == kNoSlot || model::is_empty(st)) {
      continue;
    }
    if (project_->set_step(id, slot, at, st) != model::Status::Ok) {
      fail(Error::Malformed, 0, "the model refused a step");
      return cost;
    }
  }
  for (std::uint32_t t = 0; t < blk_.tracks; ++t) {  // what moved time on a track that has no slot
    if (report_.track[t].slot != kNoSlot) {
      continue;
    }
    for (const model::StepFx& f : mapper_.step(t).fx) {
      if (f.type == model::kFxNone) {
        continue;
      }
      if (carry(id, at, f)) {
        ++report_.timing_carried;
      } else {
        ++report_.timing_lost;
      }
    }
  }
  if (++line_ > pd.last) {
    ++pd_;
    pd_open_ = false;
  }
  return cost;
}

std::size_t Importer::tick_order() noexcept {
  for (std::uint32_t i = 0; i < linear_.order_count; ++i) {
    if (project_->append_order(linear_.order[i] + 1u) != model::Status::Ok) {
      fail(Error::Malformed, 0, "the model refused an order entry");
      return 1;
    }
  }
  phase_ = Phase::Done;
  progress_ = Progress::Done;
  status_ = Status();
  return 1u + linear_.order_count;
}

unsigned Importer::permille() const noexcept {
  switch (phase_) {
    case Phase::Open: return 0;
    case Phase::Validate: return 10;
    case Phase::Flow: return 20u + std::min<unsigned>(300u, linear_.report.visits / 108u);
    case Phase::Scan: return 320u + (linear_.pattern_count != 0 ? 100u * pd_ / linear_.pattern_count : 100u);
    case Phase::Header: return 420u;
    case Phase::Instruments: return 430u + (reader_.song().num_samples != 0 ? 70u * slot_ / reader_.song().num_samples : 70u);
    case Phase::Patterns:
      return 500u + (linear_.pattern_count != 0 ? 480u * pd_ / linear_.pattern_count : 480u);
    case Phase::Order: return 990u;
    case Phase::Done: return 1000u;
  }
  return 0;
}

// ---- the PCM ----------------------------------------------------------------------------------------------------------

Status Importer::read_pcm(std::uint32_t slot, std::uint32_t first, std::uint32_t n, std::int16_t* dst) const noexcept {
  if (slot >= caps::kMaxSamples || report_.instrument[slot].result != InstResult::Sample || progress_ != Progress::Done) {
    return Status{Error::Malformed, 0, "no PCM for this instrument"};
  }
  const InstReport& r = report_.instrument[slot];
  if (first > r.frames || n > r.frames - first || (n != 0 && dst == nullptr)) {
    return Status{Error::Malformed, r.src, "PCM range outside the instrument"};
  }
  const bool sixteen = r.src_bits == 16;
  const bool stereo = r.src_right != 0;
  const std::uint32_t bpf = sixteen ? 2u : 1u;
  std::uint8_t left[512], right[512];
  const std::uint32_t chunk = 512u / bpf;
  std::uint32_t done = 0;
  while (done < n) {
    const std::uint32_t k = std::min(chunk, n - done);
    const std::uint32_t at = (first + done) * bpf;
    if (!src_->read_at(r.src + at, left, k * bpf) || (stereo && !src_->read_at(r.src_right + at, right, k * bpf))) {
      return Status{Error::SourceFailed, r.src + at, "the byte source failed"};
    }
    for (std::uint32_t i = 0; i < k; ++i) {
      int l, rr = 0;
      if (sixteen) {
        l = (left[2 * i] << 8) | left[2 * i + 1];
        l = l >= 32768 ? l - 65536 : l;
        if (stereo) {
          rr = (right[2 * i] << 8) | right[2 * i + 1];
          rr = rr >= 32768 ? rr - 65536 : rr;
        }
      } else {
        l = static_cast<std::int8_t>(left[i]) * 256;
        if (stereo) {
          rr = static_cast<std::int8_t>(right[i]) * 256;
        }
      }
      dst[done + i] = static_cast<std::int16_t>(stereo ? (l + rr) / 2 : l);
    }
    done += k;
  }
  return Status{};
}

// ---- the registered strategy --------------------------------------------------------------------------------------------

void ImportReader::begin(core::ByteSource&, model::ModelArena& arena, const ReadParams& params) noexcept {
  report_ = FormatReport{};
  if (params.random == nullptr) {
    arena.project.reset();
    report_.error = FormatError::Internal;
    report_.detail = "a MED import needs ReadParams::random, a random-access source";
    progress_ = Progress::Failed;
    return;
  }
  importer_.begin(*params.random, arena, options_);
  progress_ = Progress::Running;
}

Progress ImportReader::step(std::size_t byte_budget) noexcept {
  if (progress_ != Progress::Running) {
    return progress_;
  }
  progress_ = importer_.step(byte_budget);
  sync();
  return progress_;
}

void ImportReader::sync() noexcept {
  const ImportReport& r = importer_.report();
  const Status& s = importer_.status();
  report_.error = to_format_error(s.error);
  report_.detail = s.detail;
  report_.offset = s.offset;
  if (progress_ == Progress::Running) {
    return;
  }
  if (progress_ == Progress::Done) {
    report_.bytes = importer_.reader().song().file_size;
    report_.patterns = r.patterns;
    report_.instruments = r.instruments;
  }
  std::uint32_t clamps = r.notes_out_of_range + r.volume_clamped + r.tempo_fx_clamped + r.flow.tempo_clamps +
                         (r.initial_tempo_clamped ? 1u : 0u);
  std::uint32_t groups = r.ignored & ((1u << kDropMasterVolume) - 1u);
  std::uint32_t dropped_cmds = 0;
  for (std::uint32_t c = 0; c < 256; ++c) {
    dropped_cmds += r.cmd_dropped[c];
  }
  for (std::uint32_t i = 0; i < caps::kMaxSamples; ++i) {
    const InstReport& ir = r.instrument[i];
    clamps += (ir.flags & kInstTuneClamped) != 0 ? 1u : 0u;
    if ((ir.flags & (kInstSynth | kInstHybrid)) != 0) groups |= 1u << kDropSynthTables;
    if ((ir.flags & kInstHoldDecay) != 0) groups |= 1u << kDropHoldDecay;
    if ((ir.flags & kInstDefaultPitch) != 0) groups |= 1u << kDropDefaultPitch;
    if ((ir.flags & kInstMidiDropped) != 0) groups |= 1u << kDropMidiProgram;
  }
  if (r.master_volume_ignored) groups |= 1u << kDropMasterVolume;
  if (r.tracks_dropped != 0) groups |= 1u << kDropTracks;
  if (dropped_cmds != 0) groups |= 1u << kDropCommands;
  if (r.flow.loop_found) groups |= 1u << kDropLoopPoint;
  if (r.instruments_refused != 0) groups |= 1u << kDropInstruments;
  if (r.structural || r.flow.truncated) groups |= 1u << kDropFlow;
  report_.sanitised = clamps;
  report_.dropped_groups = groups;
}

}  // namespace o3os::formats::med
