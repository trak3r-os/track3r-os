// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/mod/import.h>

#include <algorithm>
#include <cstring>

namespace o3os::formats::mod {

namespace {

inline std::uint32_t sat_add(std::uint32_t a, std::uint32_t b) noexcept { return a + b < a ? ~0u : a + b; }

// "PAT <n>" or "PAT <n> @<first row>" for a piece that does not start its pattern: the name of an imported pattern.
std::size_t piece_name(std::uint32_t pattern, std::uint32_t first, char* out) noexcept {
  std::size_t n = 0;
  auto put = [&](const char* s) {
    while (*s != '\0' && n < model::kPatternNameMax) {
      out[n++] = *s++;
    }
  };
  auto num = [&](std::uint32_t v) {
    char digits[6];
    std::size_t d = 0;
    do {
      digits[d++] = static_cast<char>('0' + v % 10u);
      v /= 10u;
    } while (v != 0 && d < sizeof digits);
    while (d != 0 && n < model::kPatternNameMax) {
      out[n++] = digits[--d];
    }
  };
  put("PAT ");
  num(pattern);
  if (first != 0) {
    put(" @");
    num(first);
  }
  out[n] = '\0';
  return n;
}

constexpr std::size_t kOpenCost = 64;
constexpr std::size_t kFlowChunk = 2048;

}  // namespace

// OS-FLASHDIET I3: drop_group_name (with its table) lives in formats/src/tracker_sniff.cpp now, so the image links it from formats
// and this library does not have to be linked for the report's names.

void ImportReport::reset() noexcept {
  med.reset();
  kind = Kind::None;
  variant = Variant::None;
  channels = 0;
  engine_fx = false;
  structural = false;
  overflow = false;
  initial_tempo_clamped = false;
  speed_clamped = false;
  for (FxTally& f : fx) {
    f = FxTally();
  }
  periods_off_table = 0;
  bad_notes = 0;
  instrument_refs_dropped = 0;
  volume_invalid = 0;
  adlib_cells = 0;
  unused_cells = 0;
  tick0_slide_cells = 0;
  instruments_beyond = 0;
  samples_trimmed = 0;
  loops_clamped = 0;
  rates_clamped = 0;
  rates_defaulted = 0;
  stereo_samples = 0;
  adlib_instruments = 0;
  packed_samples = 0;
  patterns_in_file = 0;
  global_volume = 64;
  master_volume = 0;
  restart = 0;
  skipped_orders = 0;
  bad_orders = 0;
  fast_slides = false;
  global_volume_written = false;
}

// ---- the job ----------------------------------------------------------------------------------------------------------

void Importer::begin(RandomSource& source, model::ModelArena& arena, const ImportOptions& options) noexcept {
  src_ = &source;
  project_ = &arena.project;
  opt_ = options;
  project_->reset();
  report_.reset();
  linear_.reset();
  for (med::InstFact& f : facts_) {
    f = med::InstFact();
  }
  std::memset(plays_, 0, sizeof plays_);
  phase_ = Phase::Open;
  progress_ = Progress::Running;
  status_ = Status();
  pcm_left_ = med::caps::kSampleArenaBytes;
  structural_ = false;
  pd_ = 0;
  line_ = 0;
  pd_open_ = false;
  slot_ = 0;
  work_cells_ = 0;
  slots_ = 0;
  slot_mask_ = 0;
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
  if (phase_ != Phase::Open) {
    report_.med.flow = linear_.report;
  }
  if (e == Error::LimitExceeded && phase_ == Phase::Flow) {
    report_.overflow = true;
  }
  if (project_ != nullptr) {
    project_->reset();  // a failed whole-project import leaves the project valid and empty
  }
}

bool Importer::channel_counts(std::uint32_t ch) const noexcept {
  const ModuleInfo& g = module_.info();
  return g.kind == Kind::Mod || (g.chan_used[ch] && !g.chan_adlib[ch]);
}

std::size_t Importer::tick_open() noexcept {
  const Status s = module_.open(*src_);
  if (!s.ok()) {
    fail_status(s);
    return 1;
  }
  const ModuleInfo& g = module_.info();
  report_.kind = g.kind;
  report_.variant = g.variant;
  report_.channels = g.channels;
  report_.patterns_in_file = g.pattern_count;
  report_.global_volume = g.global_volume;
  report_.master_volume = g.master_volume;
  report_.restart = g.restart;
  report_.skipped_orders = g.skipped_orders;
  report_.bad_orders = g.bad_orders;
  report_.fast_slides = g.fast_slides;
  slots_ = std::min<std::uint32_t>(g.sample_count, caps::kSlots);
  report_.instruments_beyond = g.sample_count > caps::kSlots ? g.sample_count - caps::kSlots : 0u;
  const bool mod = g.kind == Kind::Mod;
  const bool eng = opt_.engine_fx == EngineFx::On || (opt_.engine_fx == EngineFx::Auto && mod);
  report_.engine_fx = eng;
  mopt_ = med::ImportOptions();
  mopt_.clock = opt_.clock;
  mopt_.overflow = opt_.overflow;
  mopt_.engine_fx = eng;
  mopt_.core_fx = !eng;  // OS-COREFX: the instruments of a song that does not use the engine window play on the Modern engine, which has the tracker FX
  // the song as the mapper sees it: BPM mode, four rows to a beat (a tick lasts 2.5 / BPM seconds in both formats), the note index offset of the format
  song_ = med::SongInfo();
  song_.version = 1;
  song_.flags = mod ? med::kFlagStSlide : std::uint8_t{0};
  song_.flags2 = static_cast<std::uint8_t>(med::kFlag2Bpm | 3u);
  song_.play_transpose = mod ? std::int8_t{0} : std::int8_t{-36};  // S3M: note byte = octave x 12 + semitone + 13, the cell holds the index + 1
  song_.num_samples = static_cast<std::uint8_t>(slots_);
  song_.master_volume = 64;
  song_.def_tempo = g.tempo;
  song_.tempo2 = g.speed;
  med::FlowOptions fo;
  fo.clock = opt_.clock;
  fo.truncate = opt_.overflow == med::Overflow::Truncate;
  flow_.begin(module_, fo, linear_, false);
  phase_ = Phase::Flow;
  return kOpenCost;
}

std::size_t Importer::tick_flow() noexcept {
  const std::uint32_t before = linear_.report.cells;
  const Progress p = flow_.step(kFlowChunk);
  const std::size_t work = 1u + (linear_.report.cells - before);
  if (p == Progress::Running) {
    return work;
  }
  if (p == Progress::Failed) {
    const Status s = flow_.status();
    if (s.error == Error::LimitExceeded && opt_.overflow == med::Overflow::Structural && !structural_) {
      // too much to linearise: patterns are the source patterns, the order is the order list (flow commands are dropped and counted)
      structural_ = true;
      report_.structural = true;
      med::FlowOptions fo;
      fo.clock = opt_.clock;
      flow_.begin(module_, fo, linear_, true);
      return work;
    }
    fail_status(s);
    return work;
  }
  report_.med.flow = linear_.report;
  report_.med.patterns = linear_.pattern_count;
  report_.med.order = linear_.order_count;
  for (std::uint32_t i = 0; i < linear_.order_count; ++i) {
    std::uint8_t& p2 = plays_[linear_.order[i]];
    p2 = p2 == 255 ? std::uint8_t{255} : static_cast<std::uint8_t>(p2 + 1);
  }
  work_cells_ = 0;
  pd_ = 0;
  pd_open_ = false;
  phase_ = Phase::Scan;
  return work;
}

// ---- scan: what each channel carries ------------------------------------------------------------------------------------

std::size_t Importer::tick_scan() noexcept {
  if (pd_ >= linear_.pattern_count) {
    med::plan_tracks(report_.med);
    phase_ = Phase::Header;
    return 1;
  }
  const med::PatternDef& pd = linear_.patterns[pd_];
  if (!pd_open_) {
    if (pd.block >= module_.info().pattern_count || pd.last >= caps::kRows || pd.first > pd.last) {
      fail(Error::Malformed, 0, "a pattern definition outside its pattern");
      return 1;
    }
    pd_open_ = true;
    line_ = pd.first;
    return 1;
  }
  const std::uint32_t cost = row_cost();
  work_cells_ = sat_add(work_cells_, cost);
  if (work_cells_ > opt_.max_scan_cells) {
    fail(Error::LimitExceeded, 0, "import: more cells scanned than the work cap");
    return cost;
  }
  const Status s = module_.read_line(pd.block, line_, cells_);
  if (!s.ok()) {
    fail_status(s);
    return cost;
  }
  const bool replay = (pd.flags & med::kPatReplay) != 0;
  const std::uint32_t w = std::max<std::uint32_t>(plays_[pd_], 1u);
  const std::uint32_t channels = module_.info().channels;
  for (std::uint32_t ch = 0; ch < channels; ++ch) {
    if (!channel_counts(ch)) {
      continue;
    }
    const Cell& c = cells_[ch];
    const bool note = !replay && (c.note != 0 || c.cut);
    const bool any = note || (!replay && (c.instrument != 0 || c.volume != 255)) || c.effect != 0 || c.param != 0;
    if (any) {
      med::TrackReport& tr = report_.med.track[ch];
      tr.cells = sat_add(tr.cells, w);
      if (note) {
        tr.notes = sat_add(tr.notes, w);
      }
    }
  }
  if (++line_ > pd.last) {
    ++pd_;
    pd_open_ = false;
  }
  return cost;
}

// ---- header, instruments ------------------------------------------------------------------------------------------------

std::size_t Importer::tick_header() noexcept {
  const ModuleInfo& g = module_.info();
  model::Project& p = *project_;
  if (p.set_name(g.name, std::strlen(g.name)) != model::Status::Ok) {
    fail(Error::Malformed, 0, "the model refused the module name");
    return 1;
  }
  const med::TempoModel tm = med::tempo_model(song_, opt_.clock);
  const med::Bpm b = med::tempo_to_bpm(tm, linear_.initial_tempo, linear_.initial_spd);
  report_.initial_tempo_clamped = b.clamp != med::Clamp::None;
  if (p.set_tempo_x100(b.x100) != model::Status::Ok) {
    fail(Error::Malformed, 0, "the model refused the tempo");
    return 1;
  }
  const bool mod = g.kind == Kind::Mod;
  for (std::uint32_t t = 0; t < caps::kMaxChannels; ++t) {
    const std::uint8_t slot = report_.med.track[t].slot;
    if (slot == med::kNoSlot) {
      continue;
    }
    model::TrackSlot ts;
    std::memset(&ts, 0, sizeof ts);
    ts.type = model::TrackType::Audio;
    ts.flags = !mod && g.chan_muted[t] ? model::kTrackMute : std::uint8_t{0};
    ts.volume = 100;
    ts.pan = mod ? mod_pan_percent(t) : g.chan_pan[t];
    ts.bus = 0;
    if (p.set_track(slot, ts) != model::Status::Ok) {
      fail(Error::Malformed, 0, "the model refused a track");
      return 1;
    }
  }
  const std::uint32_t active = std::max<std::uint32_t>(report_.med.tracks_kept, 4u);
  if (p.set_active_tracks(active) != model::Status::Ok) {
    fail(Error::Malformed, 0, "the model refused the track count");
    return 1;
  }
  slot_ = 0;
  phase_ = Phase::Instruments;
  return 8;
}

std::size_t Importer::tick_instruments() noexcept {
  if (slot_ >= slots_) {
    mapper_.begin(song_, mopt_, facts_, linear_.initial_tempo, linear_.initial_spd, structural_, report_.med);
    std::memset(mem_, 0, sizeof mem_);
    work_cells_ = 0;
    pd_ = 0;
    pd_open_ = false;
    phase_ = Phase::Patterns;
    return 1;
  }
  const Kind kind = module_.info().kind;
  SampleInfo info;
  const Status s = module_.sample(slot_, info);
  if (!s.ok()) {
    fail_status(s);
    return 1;
  }
  med::InstReport& rep = report_.med.instrument[slot_];
  std::uint16_t extra = 0;
  const bool modern = !report_.engine_fx;  // OS-BUG-MODENGINE: a module whose commands are the OG / core FX plays on the Modern engine, a MOD at the Paula rate too (AUTO would send it to Amiga, which has none of the tracker FX)
  const bool made = map_sample(kind, slot_, info, opt_.clock, pcm_left_, modern, scratch_, rep, extra);
  report_.adlib_instruments += info.kind == SampleKind::AdLib ? 1u : 0u;
  report_.packed_samples += info.kind == SampleKind::Unsupported ? 1u : 0u;
  report_.stereo_samples += (info.notes & kNoteStereo) != 0 && info.kind == SampleKind::Pcm ? 1u : 0u;
  report_.samples_trimmed += (info.notes & kNoteTrimmed) != 0 ? 1u : 0u;
  report_.loops_clamped += (info.notes & kNoteLoopClamped) != 0 ? 1u : 0u;
  report_.rates_clamped += (extra & kSampleRateClamped) != 0 ? 1u : 0u;
  report_.rates_defaulted += (extra & kSampleRateDefault) != 0 ? 1u : 0u;
  if (made) {
    if (project_->set_instrument(slot_, scratch_) != model::Status::Ok) {
      fail(Error::Malformed, info.offset, "the model refused an instrument");
      return 1;
    }
    med::InstFact& f = facts_[slot_ + 1];
    f.svol = std::min<std::uint8_t>(info.volume, 64);
    f.midi = false;
    f.frames = rep.frames;
    ++report_.med.instruments;
    const std::uint64_t bytes = static_cast<std::uint64_t>(rep.frames) * 2u;
    pcm_left_ -= bytes;
    report_.med.pcm_bytes += bytes;
  } else if (rep.result != med::InstResult::None && rep.result != med::InstResult::EmptyData) {
    ++report_.med.instruments_refused;
  }
  ++slot_;
  return 4;
}

// ---- patterns ----------------------------------------------------------------------------------------------------------

bool Importer::open_pattern() noexcept {
  const med::PatternDef& pd = linear_.patterns[pd_];
  if (pd.block >= module_.info().pattern_count || pd.last >= caps::kRows || pd.first > pd.last) {
    fail(Error::Malformed, 0, "a pattern definition outside its pattern");
    return false;
  }
  const std::uint32_t id = pd_ + 1u;
  if (project_->create_pattern(id, pd.last - pd.first + 1u) != model::Status::Ok) {
    fail(Error::Malformed, 0, "the model refused a pattern");
    return false;
  }
  const bool replay = (pd.flags & med::kPatReplay) != 0;
  if (!replay) {
    char name[model::kPatternNameMax + 1] = {};
    const std::size_t n = piece_name(pd.block, pd.first, name);
    if (n != 0 && project_->set_pattern_name(id, name, n) != model::Status::Ok) {
      fail(Error::Malformed, 0, "the model refused a pattern name");
      return false;
    }
  }
  mapper_.begin_piece(pd.entry_tempo, pd.entry_spd, replay);
  slot_mask_ = 0;
  const std::uint32_t channels = module_.info().channels;
  for (std::uint32_t t = 0; t < channels; ++t) {
    if (report_.med.track[t].slot != med::kNoSlot) {
      slot_mask_ |= 1ull << t;
    }
  }
  line_ = pd.first;
  pd_open_ = true;
  return true;
}

// A Tempo, Speed or Break FX of a track without a slot goes onto the first track that has a free FX column at that line (the model's FX of these kinds are global).
bool Importer::carry(std::uint32_t pattern_id, std::uint32_t line, const model::StepFx& fx) noexcept {
  const model::Pattern* pt = project_->pattern(pattern_id);
  if (pt == nullptr) {
    return false;
  }
  for (std::uint32_t slot = 0; slot < report_.med.tracks_kept; ++slot) {
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

// One effect's outcome from the mapper's own counters: what it dropped or approximated beyond what the translation already knew.
void Importer::tally(const Translated& t, std::uint32_t before_drop, std::uint32_t before_approx, std::uint8_t cmd) noexcept {
  if (t.id == kNoFx) {
    return;
  }
  FxTally& f = report_.fx[t.id];
  ++f.seen;
  if (t.dropped) {
    ++f.dropped;
    return;
  }
  const std::uint32_t dd = report_.med.cmd_dropped[cmd] - before_drop;
  const std::uint32_t da = report_.med.cmd_approx[cmd] - before_approx;
  if (dd != 0) {
    ++f.dropped;
  } else if (da != 0 || t.approx) {
    ++f.approx;
  }
}

void Importer::apply_cell(std::uint32_t ch, const Cell& cell) noexcept {
  const ModuleInfo& g = module_.info();
  Cell src = cell;
  if (g.kind == Kind::S3m && ch < caps::kMaxChannels) {  // OS-COREFX CF2: the parameter memory, resolved here so that the written pattern holds the value
    const int mc = memory_class(src.effect);
    if (mc >= 0) {
      std::uint8_t& m = mem_[ch][mc];
      if (src.param != 0) {
        m = src.param;
      } else {
        src.param = m;
      }
    }
  }
  Translated tr;
  translate(g.kind, src, slots_, tr, mopt_.core_fx);
  const bool has_slot = report_.med.track[ch].slot != med::kNoSlot;
  report_.periods_off_table += tr.off_table ? 1u : 0u;
  report_.bad_notes += tr.bad_note ? 1u : 0u;
  report_.instrument_refs_dropped += tr.bad_instrument ? 1u : 0u;
  report_.volume_invalid += tr.volume_invalid ? 1u : 0u;
  med::Cell c0 = tr.cell;
  if (tr.flow && !structural_) {
    c0.command = 0;  // the flow has done it
    c0.data = 0;
  }
  // the MED command whose counters say what the mapper did with this effect
  std::uint8_t cmd = tr.flow ? (structural_ ? c0.command : std::uint8_t{0}) : tr.med_cmd;
  if (tr.timing == OpKind::Speed) {
    cmd = 0x09;
  } else if (tr.timing == OpKind::Tempo) {
    cmd = 0x0F;
  }
  const std::uint32_t before_drop = report_.med.cmd_dropped[cmd];
  const std::uint32_t before_approx = report_.med.cmd_approx[cmd];
  if (has_slot) {
    mapper_.cell(ch, c0, 0);
    for (unsigned i = 0; i < tr.extras; ++i) {
      mapper_.cell(ch, tr.extra[i], 1);
    }
    if (tr.timing == OpKind::Speed) {
      mapper_.set_speed(ch, tr.timing_value, 0x09);
      report_.speed_clamped = report_.speed_clamped || tr.timing_value > 32;
    } else if (tr.timing == OpKind::Tempo) {
      mapper_.set_tempo(ch, tr.timing_value, 0x0F);
    }
    if (report_.engine_fx && g.kind == Kind::Mod &&
        (tr.med_cmd == 0x01 || tr.med_cmd == 0x02 || tr.med_cmd == 0x03 || tr.med_cmd == 0x05 || tr.med_cmd == 0x06 || tr.med_cmd == 0x0A)) {
      ++report_.tick0_slide_cells;
    }
  } else {
    // a channel without a track: what moves time still counts (a speed or a tempo goes onto a track that has a slot), nothing else is mapped
    if (tr.timing == OpKind::Speed) {
      mapper_.set_speed(ch, tr.timing_value, 0x09);
    } else if (tr.timing == OpKind::Tempo) {
      mapper_.set_tempo(ch, tr.timing_value, 0x0F);
    } else if (tr.flow && structural_) {
      mapper_.timing_cell(ch, c0);
    }
    return;  // its effects are not counted twice: the report has the channel's cells and notes
  }
  tally(tr, before_drop, before_approx, cmd);
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
  const med::PatternDef& pd = linear_.patterns[pd_];
  const std::uint32_t cost = row_cost();
  work_cells_ = sat_add(work_cells_, cost);
  if (work_cells_ > opt_.max_map_cells) {
    fail(Error::LimitExceeded, 0, "import: more cells mapped than the work cap");
    return cost;
  }
  const ModuleInfo& g = module_.info();
  const Status s = module_.read_line(pd.block, line_, cells_);
  if (!s.ok()) {
    fail_status(s);
    return cost;
  }
  mapper_.begin_line();
  const bool replay = (pd.flags & med::kPatReplay) != 0;
  for (std::uint32_t ch = 0; ch < g.channels; ++ch) {
    const Cell& c = cells_[ch];
    if (!channel_counts(ch)) {
      const bool any = (!replay && (c.note != 0 || c.cut || c.instrument != 0 || c.volume != 255)) || c.effect != 0 || c.param != 0;
      if (any) {
        (g.chan_adlib[ch] ? report_.adlib_cells : report_.unused_cells) += 1;
      }
      // an effect on such a channel that moves time (a speed, a tempo) still counts: the flow did not skip an AdLib channel, so neither does the mapping
      if (g.kind == Kind::S3m && g.chan_adlib[ch]) {
        const FlowOp op = flow_op(g.kind, c);
        if (op.kind == OpKind::Speed) {
          mapper_.set_speed(ch, op.value, 0x09);
        } else if (op.kind == OpKind::Tempo) {
          mapper_.set_tempo(ch, op.value, 0x0F);
        }
      }
      continue;
    }
    apply_cell(ch, c);
  }
  mapper_.end_line(slot_mask_);
  report_.med.cells = sat_add(report_.med.cells, cost);
  const std::uint32_t id = pd_ + 1u;
  const std::uint32_t at = line_ - pd.first;
  // OS-COREFX CF3: the header's global volume (below 64) is a V at row 0 of the first pattern played, in the first free FX slot of the first track that has one, when that pattern is played once
  // (a pattern the order list repeats would restart the volume at each pass); otherwise it stays in the report as dropped.
  bool want_v = false;
  if (at == 0 && g.kind == Kind::S3m && mopt_.core_fx && !report_.global_volume_written && g.global_volume < 64 && linear_.order_count != 0 && linear_.order[0] == pd_) {
    want_v = true;
    for (std::uint32_t i = 1; i < linear_.order_count; ++i) {
      want_v = want_v && linear_.order[i] != pd_;
    }
  }
  for (std::uint32_t t = 0; t < g.channels; ++t) {
    const std::uint8_t slot = report_.med.track[t].slot;
    model::Step st = mapper_.step(t);
    if (slot == med::kNoSlot) {
      continue;
    }
    if (want_v) {
      for (model::StepFx& f : st.fx) {
        if (f.type == model::kFxNone) {
          f.type = med::fx::kTrkGlobalVol;
          f.value = g.global_volume;
          report_.global_volume_written = true;
          want_v = false;
          break;
        }
      }
    }
    if (model::is_empty(st)) {
      continue;
    }
    if (project_->set_step(id, slot, at, st) != model::Status::Ok) {
      fail(Error::Malformed, 0, "the model refused a step");
      return cost;
    }
  }
  for (std::uint32_t t = 0; t < g.channels; ++t) {  // what moved time on a channel that has no track
    if (report_.med.track[t].slot != med::kNoSlot) {
      continue;
    }
    for (const model::StepFx& f : mapper_.step(t).fx) {
      if (f.type == model::kFxNone) {
        continue;
      }
      if (carry(id, at, f)) {
        ++report_.med.timing_carried;
      } else {
        ++report_.med.timing_lost;
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
    case Phase::Flow: return 20u + std::min<unsigned>(300u, linear_.report.visits / 108u);
    case Phase::Scan: return 320u + (linear_.pattern_count != 0 ? 100u * pd_ / linear_.pattern_count : 100u);
    case Phase::Header: return 420u;
    case Phase::Instruments: return 430u + (slots_ != 0 ? 70u * slot_ / slots_ : 70u);
    case Phase::Patterns: return 500u + (linear_.pattern_count != 0 ? 480u * pd_ / linear_.pattern_count : 480u);
    case Phase::Order: return 990u;
    case Phase::Done: return 1000u;
  }
  return 0;
}

// ---- the PCM ----------------------------------------------------------------------------------------------------------

std::uint32_t Importer::pcm_rate(std::uint32_t slot) const noexcept {
  SampleInfo info;
  if (slot >= caps::kSlots || !module_.sample(slot, info).ok()) {
    return 0;
  }
  return sample_rate_of(module_.info().kind, info, opt_.clock);
}

Status Importer::read_pcm(std::uint32_t slot, std::uint32_t first, std::uint32_t n, std::int16_t* dst) const noexcept {
  if (slot >= caps::kSlots || report_.med.instrument[slot].result != med::InstResult::Sample || progress_ != Progress::Done) {
    return Status{Error::Malformed, 0, "no PCM for this instrument"};
  }
  SampleInfo info;
  const Status s = module_.sample(slot, info);
  if (!s.ok()) {
    return s;
  }
  return module_.read_pcm(info, first, n, dst);
}

// ---- the registered strategy --------------------------------------------------------------------------------------------

void ImportReader::begin(core::ByteSource&, model::ModelArena& arena, const ReadParams& params) noexcept {
  report_ = FormatReport{};
  if (params.random == nullptr) {
    arena.project.reset();
    report_.error = FormatError::Internal;
    report_.detail = "a MOD or S3M import needs ReadParams::random, a random-access source";
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
  report_.error = med::to_format_error(s.error);
  report_.detail = s.detail;
  report_.offset = s.offset;
  if (progress_ == Progress::Running) {
    return;
  }
  if (progress_ == Progress::Done) {
    report_.bytes = importer_.module().info().file_size;
    report_.patterns = r.med.patterns;
    report_.instruments = r.med.instruments;
  }
  std::uint32_t clamps = r.med.notes_out_of_range + r.med.volume_clamped + r.med.tempo_fx_clamped + r.med.flow.tempo_clamps + r.volume_invalid + r.rates_clamped +
                         (r.initial_tempo_clamped ? 1u : 0u);
  std::uint32_t groups = 0;
  std::uint32_t dropped = 0, approx = 0;
  for (const FxTally& f : r.fx) {
    dropped += f.dropped;
    approx += f.approx;
  }
  if (r.adlib_instruments != 0 || r.adlib_cells != 0) groups |= 1u << kDropAdLib;
  if (r.packed_samples != 0) groups |= 1u << kDropPacked;
  if (r.stereo_samples != 0) groups |= 1u << kDropStereo;
  if (r.med.instruments_refused != 0 || r.instruments_beyond != 0) groups |= 1u << kDropInstruments;
  if (r.med.tracks_dropped != 0) groups |= 1u << kDropTracks;
  if (dropped != 0) groups |= 1u << kDropEffects;
  if (approx != 0) groups |= 1u << kDropApprox;
  if (r.kind == Kind::S3m && ((r.global_volume != 64 && !r.global_volume_written) || (r.master_volume & 0x7Fu) != 0)) groups |= 1u << kDropGlobalVolume;
  if (r.tick0_slide_cells != 0) groups |= 1u << kDropSlides;
  if (r.med.flow.loop_found) groups |= 1u << kDropLoopPoint;
  if (r.structural || r.med.flow.truncated) groups |= 1u << kDropFlow;
  if (r.periods_off_table != 0) groups |= 1u << kDropPeriods;
  if (r.kind == Kind::Mod && r.restart != 127 && r.restart != 0 && r.restart < importer_.module().info().order_count) groups |= 1u << kDropRestart;
  report_.sanitised = clamps;
  report_.dropped_groups = groups;
}

}  // namespace o3os::formats::mod
