// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sequencer v2 (seq/sequencer.h). A comment names the PAR row or the spec where a rule is a compatibility fact, and says where the OS differs.
#include <o3os/seq/sequencer.h>
#include <o3os/seq/rows.h>
#include <o3os/seq/perform.h>

#include <algorithm>

namespace o3os::seq {

using audio::EventKind;
using core::Q32;
using core::SQ32;

namespace {

constexpr std::uint32_t kMinTempoX100 = model::kMinTempoX100;
constexpr std::uint32_t kMaxTempoX100 = model::kMaxTempoX100;
constexpr std::uint32_t kNoOrder = 0xFFFFu;
constexpr std::uint8_t kPlayBeatSlice = 5;  // SamplerParams::play_mode of a beat slice instrument (app/og_samples.cpp): its note picks a slice, not a pitch

// The low 32 bits of a Q32 as a signed number: the time minus its nearest sample, in 2^-32 sample (the fraction is f below a half, f - 2^32 above).
constexpr std::int32_t sfrac(Q32 q) noexcept { return static_cast<std::int32_t>(static_cast<std::uint32_t>(q & 0xFFFFFFFFu)); }

constexpr std::uint32_t clamp_u32(std::uint32_t v, std::uint32_t lo, std::uint32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
constexpr std::int32_t clamp_i32(std::int32_t v, std::int32_t lo, std::int32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

enum class NoteKind : std::uint8_t { Empty, Note, Off, Cut, Fade };
constexpr NoteKind note_kind(std::uint8_t note) noexcept {
  if (note >= model::kNoteFirst && note <= model::kNoteLast) {
    return NoteKind::Note;
  }
  switch (note) {
    case model::kNoteOff:
      return NoteKind::Off;
    case model::kNoteCut:
      return NoteKind::Cut;
    case model::kNoteFade:
      return NoteKind::Fade;
    default:
      return NoteKind::Empty;  // 0, and the reserved 129..252 (carried by the model, inert here)
  }
}

constexpr bool is_pfx_fx(std::uint8_t type) noexcept { return type == fx::kChord || type == fx::kArp || type == fx::kTune; }
// The offset of the i-th set bit of a non-empty mask, counted from 0 (i < the number of set bits): the tone i of a chord or an arpeggio (at most 11 steps).
std::uint32_t nth_offset(std::uint32_t mask, std::uint32_t i) noexcept {
  for (; i != 0u; --i) {
    mask &= mask - 1u;  // clear the lowest set bit
  }
  return static_cast<std::uint32_t>(__builtin_ctz(mask));
}

// The arpeggio walk (PAR-078, docs/specs/OS-PFX.md OS-PFXFIX): `pos` (0..11) is where the next search starts. Up searches upward from it (wrapping at 12) for a set bit of the
// 12-bit mask and leaves pos at that offset + 1; Down searches downward and leaves offset - 1; Random starts the upward search `r` (0..10) offsets further on. Returns the offset.
// At most 12 steps; the mask is never empty (bit 0 is in every one), the 0 after the loop is unreachable.
std::uint32_t arp_pick(std::uint32_t mask, ArpDir dir, std::uint8_t& pos, std::uint32_t r) noexcept {
  const bool down = dir == ArpDir::Down;
  const std::uint32_t from = (pos + r) % 12u;
  for (std::uint32_t i = 0; i < 12u; ++i) {
    const std::uint32_t o = down ? (from + 12u - i) % 12u : (from + i) % 12u;
    if (((mask >> o) & 1u) != 0u) {
      pos = static_cast<std::uint8_t>(down ? (o + 11u) % 12u : (o + 1u) % 12u);
      return o;
    }
  }
  return 0;
}

// The value range of a column's FX for Random FX value (an unknown id answers the None row, 0..100: fx::row()).
void fx_range(std::uint8_t type, std::int32_t& lo, std::int32_t& hi) noexcept {
  if (type == fx::kNone || type >= kFxInfoCount) {
    lo = 0;
    hi = type >= fx::kEngineFirst ? 255 : 100;
    return;
  }
  lo = kFxInfo[type].min;
  hi = kFxInfo[type].max;
}

}  // namespace

Sequencer::Sequencer(const model::Project& project) noexcept : project_(project) {
#ifndef O3OS_SEQ_ORDER_EXTERNAL
  store_ = &own_store_;
#endif
  ctl_.reset();
  rng_.seed(cfg_.seed);
}

void Sequencer::configure(const Config& c) noexcept { cfg_ = c; }

// ---- events -----------------------------------------------------------------------------------------------------------------

bool Sequencer::emit(std::uint64_t at, EventKind kind, std::uint8_t track, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t flags,
                     std::int32_t value) noexcept {
  audio::SeqEvent e{};
  e.offset = 0;
  e.kind = kind;
  e.track = track;
  e.a = a;
  e.b = b;
  e.c = c;
  e.flags = flags;
  e.value = value;
  if (!queue_.push(at, e)) {
    ++stats_.queue_overflow;
    return false;
  }
  return true;
}

void Sequencer::emit_changes(const ModChange* ch, std::uint32_t n, std::uint64_t at) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint8_t g = static_cast<std::uint8_t>(ch[i].group);
    if (ch[i].now.active) {
      emit(at, EventKind::ModSet, ch[i].track, ch[i].now.fx_type, ch[i].now.value, ch[i].now.layer, g, ch[i].now.perf);
    } else {
      emit(at, EventKind::ModEnd, ch[i].track, 0, 0, 0, g, 0);
    }
  }
}

// ---- tempo and swing in force -------------------------------------------------------------------------------------------------

std::uint32_t Sequencer::nominal_tempo_x100() const noexcept {
  if (follow_ != nullptr && follow_->has_phase()) {
    return follow_->bpm_x100();
  }
  std::uint32_t t = perf_tempo_x100_ != 0u ? perf_tempo_x100_ : (fx_tempo_x100_ != 0u ? fx_tempo_x100_ : static_cast<std::uint32_t>(tempo_sm_.current()));
  if (t == 0u) {
    t = project_.tempo_x100();
  }
  return clamp_u32(t, kMinTempoX100, kMaxTempoX100);
}

std::uint32_t Sequencer::tempo_x100() const noexcept {
  const std::uint32_t t = nominal_tempo_x100();
  return rate_ == 1u ? t / 2u : (rate_ == 2u ? t * 2u : t);  // for the display and the LFOs; the step timing scales the exact samples per tick (played_spt), not this
}

Q32 Sequencer::played_spt() const noexcept {
  const Q32 spt = core::samples_per_tick_q32(nominal_tempo_x100());
  return rate_ == 1u ? spt << 1 : (rate_ == 2u ? spt >> 1 : spt);
}

std::uint32_t Sequencer::swing() const noexcept {
  const std::uint32_t s = swing_fx_ != 0u ? swing_fx_ : (swing_param_ != 0u ? swing_param_ : project_.swing());
  return clamp_u32(s, model::kMinSwing, model::kMaxSwing);
}

bool Sequencer::apply(const core::ParamChange& pc) noexcept {
  if (core::param_kind(pc.id) != core::ParamKind::Project || core::param_index(pc.id) != 0u) {
    return false;
  }
  const unsigned field = core::param_field(pc.id);
  if (field == seq_param::Tempo) {
    // BPM as a float; 40..800, kept as BPM x 100 in the smoother. NaN and out-of-range values are clamped.
    float v = pc.value;
    if (!(v >= 40.0f)) {
      v = 40.0f;
    } else if (v > 800.0f) {
      v = 800.0f;
    }
    tempo_sm_.retarget(static_cast<std::int32_t>(v * 100.0f + 0.5f), pc.ramp_samples);
    return true;
  }
  if (field == seq_param::Swing) {
    float v = pc.value;
    if (!(v >= 25.0f)) {
      v = 25.0f;
    } else if (v > 75.0f) {
      v = 75.0f;
    }
    swing_param_ = static_cast<std::uint32_t>(v + 0.5f);
    return true;
  }
  if (field == seq_param::Metro) {
    metro_on_ = pc.value >= 0.5f;  // (NaN is off)
    return true;
  }
  if (field == seq_param::MidiProgram) {  // OS-INSTUI S8b: the PROGRAM of instrument `value` was edited: step() sends it (the render is the only reader and writer of these bits)
    if (pc.value >= 0.0f && pc.value < static_cast<float>(model::kMaxInstruments)) {
      const std::uint32_t slot = static_cast<std::uint32_t>(pc.value);
      prog_edit_[slot >> 5] |= 1u << (slot & 31u);
    }
    return true;
  }
  return false;
}

// ---- OS-INSTUI S8b: the MIDI instrument's output state -----------------------------------------------------------------------------------

// A project that was loaded since the last look (Project::load_epoch) is not the one the receivers were told about: no program is known to be on any channel.
void Sequencer::midi_sync_state() noexcept {
  const std::uint32_t epoch = project_.load_epoch();
  if (epoch != load_epoch_) {
    load_epoch_ = epoch;
    for (std::uint8_t& p : chan_prog_) {
      p = 0xFF;
    }
    prog_edit_[0] = 0;
    prog_edit_[1] = 0;
  }
}

namespace {
// The program (0..127) a MIDI instrument names, or -1 for none (a sample or module instrument, PROGRAM none, a value out of range).
constexpr int program_of(const model::Instrument& in) noexcept {
  return in.kind != model::InstrumentKind::Midi || in.midi_program == model::kMidiProgramNone || in.midi_program > model::kMidiProgramMax ? -1 : static_cast<int>(in.midi_program) - 1;
}
}  // namespace

int Sequencer::program_due(const model::Instrument& in) noexcept {
  midi_sync_state();
  const int program = program_of(in);
  return program >= 0 && chan_prog_[midi_route(in).ch] != program ? program : -1;
}

void Sequencer::program_sent(const model::Instrument& in, int program) noexcept {
  chan_prog_[midi_route(in).ch] = static_cast<std::uint8_t>(program);
}

// The PROGRAM of an instrument was edited (apply): it is sent at the start of this block, on its channel and its route, whether or not the channel already holds it (an edit is a request). At most 64 bits are looked at.
void Sequencer::flush_program_edits() noexcept {
  midi_sync_state();
  if ((prog_edit_[0] | prog_edit_[1]) == 0u) {
    return;
  }
  for (std::uint32_t slot = 0; slot < model::kMaxInstruments; ++slot) {
    std::uint32_t& word = prog_edit_[slot >> 5];
    if ((word & (1u << (slot & 31u))) == 0u) {
      continue;
    }
    word &= ~(1u << (slot & 31u));
    const model::Instrument* in = project_.instrument(slot);
    const int program = in != nullptr ? program_of(*in) : -1;
    if (program < 0) {
      continue;
    }
    if (!emit_midi(now_, EventKind::MidiProgram, audio::kNoTrack, static_cast<std::uint8_t>(program), 0, midi_route(*in))) {
      word |= 1u << (slot & 31u);  // a full queue: the edit is still a request, asked again at the next block (and the channel is not recorded as told)
      return;
    }
    program_sent(*in, program);
  }
}

bool Sequencer::emit_midi(std::uint64_t at, EventKind kind, std::uint8_t track, std::uint8_t a, std::uint8_t b, const MidiRoute& r) noexcept {
  return emit(at, kind, track, a, b, r.ch, r.ports, route_delay_us(r));
}

// CC A..F of the step's FX (and FX 37, CC F): where the value goes depends on the instrument the step plays (OS-INSTUI S8b). A MIDI instrument whose slot is set to a controller sends that controller on its
// channel and port; PROG sends a Program Change, the aftertouches send channel or polyphonic pressure; the default (and any other kind of instrument) sends what the Config page's number for the slot says, on the
// device's notes-out channel, as before INST 1.3. Polyphonic aftertouch needs a note: the one the track is sounding (none: ignored, counted).
void Sequencer::emit_cc(std::uint32_t t, std::uint32_t slot_index, std::uint8_t value, std::uint64_t at) noexcept {
  const TrackState& k = tr_[t];
  const std::uint8_t slot = k.row.instr != 0u ? static_cast<std::uint8_t>(k.row.instr - 1u) : k.last_slot;
  const model::Instrument* in = slot != 0xFFu ? project_.instrument(slot) : nullptr;
  const std::uint8_t track = static_cast<std::uint8_t>(t);
  if (in == nullptr || in->kind != model::InstrumentKind::Midi || slot_index >= model::kMidiCcSlotsPerInstrument) {
    emit(at, EventKind::MidiCc, track, static_cast<std::uint8_t>(slot_index), value, audio::kChannelDefault, 0, 0);
    return;
  }
  const MidiRoute r = midi_route(*in);
  const CcTarget c = cc_target(in->midi_cc[slot_index]);
  const std::uint8_t v7 = value > 127u ? std::uint8_t{127} : value;
  switch (c.kind) {
    case CcKind::Config:
      emit(at, EventKind::MidiCc, track, static_cast<std::uint8_t>(slot_index), v7, audio::kChannelDefault, r.ports, route_delay_us(r));
      break;
    case CcKind::Controller:
      emit_midi(at, EventKind::MidiCc, track, static_cast<std::uint8_t>(audio::kMidiCcExplicit | c.number), v7, r);
      break;
    case CcKind::Program:  // (not noted in chan_prog_: the instrument's own PROGRAM is put back only when it changes, not after the pattern moved the receiver on)
      emit_midi(at, EventKind::MidiProgram, track, v7, 0, r);
      break;
    case CcKind::ChannelAt:
      emit_midi(at, EventKind::MidiChanAt, track, v7, 0, r);
      break;
    case CcKind::PolyAt:
      if (k.midi_on) {
        emit_midi(at, EventKind::MidiPolyAt, track, k.sent_note, v7, r);
      } else {
        ++stats_.fx_ignored;
      }
      break;
  }
}

// A stop (or a restart): every channel that a Micro-tune bent goes back to the centre on every route that a note bent it on (two instruments on one channel may send to different ports: the centre of the
// last one alone would leave the other's receiver out of tune). At most 16 channels x 4 routes events, bounded, none when nothing was bent. A route whose centre found no room in the queue stays bent
// (asked again at the next stop). The centre has no delay: MidiOut::stop_all drops the bends that are staged behind an OFFSET (their notes never sound), so none can land after it.
void Sequencer::centre_bends(std::uint64_t at) noexcept {
  if (bend_routes_ == 0u) {
    return;
  }
  for (std::uint8_t ch = 0; ch < 16u; ++ch) {
    for (std::uint8_t route = 0; route < 4u; ++route) {
      const std::uint64_t bit = std::uint64_t{1} << (4u * ch + route);
      if ((bend_routes_ & bit) != 0u &&
          emit(at, EventKind::MidiPitchBend, audio::kNoTrack, static_cast<std::uint8_t>(kBendCentre & 0x7Fu), static_cast<std::uint8_t>(kBendCentre >> 7), ch, route, 0)) {
        bend_routes_ &= ~bit;
      }
    }
  }
}

// ---- the song's order list (E-SONG-07) ------------------------------------------------------------------------------------------

namespace {

// What one edit of the order list does to a song position. false: the slot at `p` was removed (p is then where the slot that slid into its place is). A value that is not a position
// (kNoOrder, the Pattern mode's) is left alone.
bool remap_slot(const model::OrderOp& op, std::uint32_t& p) noexcept {
  if (p >= model::kMaxSongPositions) {
    return true;
  }
  switch (op.kind) {
    case model::OrderOp::Insert:
      if (p >= op.a) {
        p += op.n;
      }
      break;
    case model::OrderOp::Remove:
      if (p >= static_cast<std::uint32_t>(op.a) + op.n) {
        p -= op.n;
      } else if (p >= op.a) {
        p = op.a;
        return false;
      }
      break;
    case model::OrderOp::Move:  // the slot at a goes to b: the ones between shift by one the other way
      if (p == op.a) {
        p = op.b;
      } else if (op.a < op.b && p > op.a && p <= op.b) {
        --p;
      } else if (op.a > op.b && p >= op.b && p < op.a) {
        ++p;
      }
      break;
    default:
      break;
  }
  return true;
}

}  // namespace

// The queued jump follows the edits made after the list it was authored against (the ops from `from` to the journal's end): its slot moves with the list, and it is cancelled when its slot
// was deleted or when the journal no longer holds those ops. A position authored against a list the copy does not have yet (from ahead of the journal) waits for the next adoption.
void Sequencer::remap_queue(const model::OrderSnapshot& journal, std::uint32_t from) noexcept {
  if (queued_jump_ == kNoJump) {
    queued_from_ = journal.ops;
    return;
  }
  const std::uint32_t behind = journal.ops - from;
  if (static_cast<std::int32_t>(behind) <= 0) {
    return;
  }
  std::uint32_t q = queued_jump_;
  bool keep = behind <= model::kOrderJournal;
  for (std::uint32_t i = 0; keep && i < behind; ++i) {
    const model::OrderOp& op = journal.journal[(from + i) % model::kOrderJournal];
    keep = op.kind != model::OrderOp::Reset && remap_slot(op, q);
  }
  if (keep) {
    queued_jump_ = static_cast<std::uint16_t>(q);
  } else {
    queued_jump_ = kNoJump;
    ++stats_.queue_dropped;
  }
  queued_from_ = journal.ops;
}

void Sequencer::queue_jump(std::uint32_t order_pos, std::uint32_t stamp) noexcept {
  if (!playing_ || order_pos >= model::kMaxSongPositions) {
    queued_jump_ = kNoJump;
    return;
  }
  queued_jump_ = static_cast<std::uint16_t>(order_pos);
  if (store_ == nullptr) {
    queued_jump_ = kNoJump;  // (no list store: no song)
    return;
  }
  queued_from_ = stamp != 0u ? stamp - 1u : store_->cur.ops;
  remap_queue(store_->cur, queued_from_);
}

void Sequencer::attach_order_store(OrderStore& store) noexcept {
  store = OrderStore{};
  store_ = &store;
  order_have_ = false;
}

// At block start: when an edit of the Project's order list finished since the copy was taken, take the list again and carry the song positions over the edits made in between (the journal), so
// the song keeps playing the slot it played. An edit that runs (odd epoch) or ran during the copy is not read: the old copy plays on and the next block tries again (never waits: the writer is
// the thread the render preempted). One load and one compare when nothing changed.
void Sequencer::adopt_order() noexcept {
  if (store_ == nullptr) {
    return;
  }
  model::OrderSnapshot& order_ = store_->cur;
  model::OrderSnapshot& order_next_ = store_->next;
  const std::uint32_t epoch = project_.order_epoch();
  if ((epoch & 1u) != 0u || (order_have_ && epoch == order_.epoch)) {
    return;
  }
  if (!project_.snapshot_order(order_next_)) {
    return;
  }
  if (order_have_) {
    const std::uint32_t behind = order_next_.ops - order_.ops;
    bool lost = behind > model::kOrderJournal;
    for (std::uint32_t i = 0; !lost && i < behind; ++i) {
      const model::OrderOp& op = order_next_.journal[(order_.ops + i) % model::kOrderJournal];
      if (op.kind == model::OrderOp::Reset) {
        lost = true;
        break;
      }
      const bool kept = remap_slot(op, order_pos_);
      after_gone_ = after_gone_ || (!kept && playing_);
      (void)remap_slot(op, located_order_);
      const bool resume_kept = remap_slot(op, resume_order_);
      resume_gone_ = resume_gone_ || (!resume_kept && have_resume_);
    }
    if (lost) {  // a load, a clear or too many edits at once: the song stays where it is as far as the new list allows
      const std::uint32_t last = order_next_.len != 0u ? order_next_.len - 1u : 0u;
      for (std::uint32_t* p : {&order_pos_, &located_order_, &resume_order_}) {
        *p = *p < model::kMaxSongPositions && *p > last ? last : *p;
      }
      after_gone_ = false;
      resume_gone_ = false;
      if (queued_jump_ != kNoJump) {
        queued_jump_ = kNoJump;
        ++stats_.queue_dropped;
      }
      queued_from_ = order_next_.ops;
    } else {
      remap_queue(order_next_, queued_from_);
    }
  } else {
    queued_from_ = order_next_.ops;
  }
  order_ = order_next_;
  order_have_ = true;
}

// ---- the main loop's commands --------------------------------------------------------------------------------------------------

void Sequencer::drain_commands() noexcept {
  Command c;
  for (std::uint32_t i = 0; i < 16u && cmds_.pop(c); ++i) {
    ++stats_.commands;
    switch (c.kind) {
      case Command::Kind::PlayPattern:
        play_pattern(c.a, c.c, c.b);
        break;
      case Command::Kind::PlaySong:
        play_song(c.b, c.c);
        break;
      case Command::Kind::Stop:
        stop();
        break;
      case Command::Kind::Resume:
        resume();
        break;
      case Command::Kind::Locate:
        locate(c.c);
        break;
      case Command::Kind::LocateRow:
        locate_row(c.c);
        break;
      case Command::Kind::SwitchPattern:
        switch_pattern(c.a);
        break;
      case Command::Kind::ReleasePattern:
        release_pattern(c.a);
        break;
      case Command::Kind::QueuePattern:
        queue_pattern(c.a);
        break;
      case Command::Kind::QueueJump:
        queue_jump(c.b, c.c);
        break;
      case Command::Kind::SetMode:
        request_mode(c.a == 0u ? PlayMode::Pattern : PlayMode::Song);
        break;
      case Command::Kind::Select:
        select(c.a == 0u ? PlayMode::Pattern : PlayMode::Song, c.b);
        break;
      case Command::Kind::PerfTrack:
        set_perf_track(c.a, c.b, c.c & 0xFFu, static_cast<TrackPlayMode>((c.c >> 8) & 0x3u));
        break;
      case Command::Kind::QueueTrackPattern:
        queue_track_pattern(c.c, c.b);
        break;
      case Command::Kind::PerfStutter:
        set_perf_stutter(c.a, c.b);
        break;
      case Command::Kind::PerfTempo:
        set_perf_tempo(c.c);
        break;
      case Command::Kind::PerfRate:
        set_play_rate(c.a);
        break;
      case Command::Kind::PerfPatternLen:
        set_perf_pattern_length(c.c);
        break;
      case Command::Kind::MuteQuant:
        mute_defer(c.a, c.b, c.c != 0u);
        break;
      case Command::Kind::MuteCancel:
        mute_cancel(c.a);
        break;
      case Command::Kind::PerfControl:
        if ((c.c >> 16) & 1u) {
          end_perf_control(c.a, static_cast<FxGroup>(c.b));
        } else {
          set_perf_control(c.a, static_cast<FxGroup>(c.b), static_cast<std::int16_t>(c.c & 0xFFFFu));
        }
        break;
    }
  }
}

// ---- transport ---------------------------------------------------------------------------------------------------------------

void Sequencer::play_pattern(std::uint32_t pattern_id, std::uint32_t from_step, std::uint32_t count_in_bars) noexcept {
  pending_start_ = true;
  pending_resume_ = false;
  pending_mode_ = PlayMode::Pattern;
  pending_order_ = clamp_u32(pattern_id, 0u, 255u);  // the pattern id in Pattern mode
  pending_from_ = from_step;
  pending_count_in_ = static_cast<std::uint8_t>(count_in_bars > 2u ? 2u : count_in_bars);
  if (!playing_) {
    mode_ = PlayMode::Pattern;
    pattern_id_ = pending_order_;  // what an external Start would play, from now on
  }
}

void Sequencer::play_song(std::uint32_t order_pos, std::uint32_t from_step) noexcept {
  pending_start_ = true;
  pending_resume_ = false;
  pending_mode_ = PlayMode::Song;
  pending_order_ = order_pos;
  pending_from_ = from_step;
  pending_count_in_ = 0;  // the count-in is the Pattern mode's
  if (!playing_) {
    mode_ = PlayMode::Song;
  }
}

void Sequencer::stop() noexcept {
  pending_stop_ = true;
  pending_start_ = false;
  pending_resume_ = false;
  ext_start_ = false;  // a master's Start that a Stop cancels before begin_run must not make a later local Play reuse its clock number
}

void Sequencer::resume() noexcept {
  pending_resume_ = true;
  pending_start_ = false;
}

void Sequencer::set_perf_track(std::uint32_t track, std::uint32_t pattern_id, std::uint32_t length, TrackPlayMode mode) noexcept {
  if (track >= kMaxTracks) {
    return;
  }
  TrackState& k = tr_[track];
  k.perf_len = static_cast<std::uint8_t>(clamp_u32(length, 0u, 128u));
  k.perf_mode = mode;
  if (k.perf_q) {
    return;  // E-PERF-07: the pattern fields of this track are QueueTrackPattern's until the run ends (the PLAYMODE FX sends pattern 0)
  }
  if (pattern_id > 255u) {
    pattern_id = 0;
  }
  if (!playing_) {
    k.perf_pattern = static_cast<std::uint8_t>(pattern_id);
    k.perf_pending_set = false;
  } else if (pattern_id != k.perf_pattern) {
    // A track's source pattern is switched at the track's own loop end.
    k.perf_pending = static_cast<std::uint8_t>(pattern_id);
    k.perf_pending_set = true;
  }
}

void Sequencer::queue_track_pattern(std::uint32_t mask, std::uint32_t pattern_id) noexcept {
  if (!playing_ || pattern_id > 255u) {
    return;
  }
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    if (((mask >> t) & 1u) == 0u) {
      continue;
    }
    TrackState& k = tr_[t];
    const bool waits_for_it = k.perf_pending_set && k.perf_pending == pattern_id;
    const bool follows_it = pattern_id != 0u && (pattern_id == k.perf_pattern || (k.perf_pattern == 0u && pattern_id == pattern_id_));
    if (waits_for_it || (pattern_id == 0u && k.perf_pattern == 0u)) {
      k.perf_pending_set = false;  // the second press, or the song's pattern for a track that follows the song: the queue is taken back
      k.perf_q = k.perf_pattern != 0u;
    } else if (follows_it) {
      // The pattern the track follows already: nothing to wait for, so the pick pins it (an override at once, staying until TRK SONG, a stop or a restart) and the song's next slot does not move the track.
      k.perf_pattern = static_cast<std::uint8_t>(pattern_id);
      k.perf_pending_set = false;
      k.perf_q = true;
    } else {
      k.perf_pending = static_cast<std::uint8_t>(pattern_id);
      k.perf_pending_set = true;
      k.perf_q = true;
    }
  }
}

void Sequencer::set_perf_stutter(std::uint32_t track, std::uint32_t value) noexcept {
  if (track < kMaxTracks) {
    tr_[track].perf_stutter = static_cast<std::uint8_t>(value > 15u ? 0u : value);
  }
}

void Sequencer::set_perf_control(std::uint32_t track, FxGroup g, std::int16_t value) noexcept {
  ModChange ch[1];
  const std::uint32_t n = ctl_.set_perf(static_cast<std::uint8_t>(track), g, value, ch);
  emit_changes(ch, n, command_time());
}

void Sequencer::end_perf_control(std::uint32_t track, FxGroup g) noexcept {
  ModChange ch[1];
  const std::uint32_t n = ctl_.end_perf(static_cast<std::uint8_t>(track), g, ch);
  emit_changes(ch, n, command_time());
}

void Sequencer::set_follow(const midi::ClockDll* dll, std::int32_t offset_us) noexcept {
  follow_ = dll;
  const std::uint32_t mag = offset_us < 0 ? static_cast<std::uint32_t>(-static_cast<std::int64_t>(offset_us)) : static_cast<std::uint32_t>(offset_us);
  const SQ32 q = static_cast<SQ32>(core::q32_from_micros(mag));
  follow_offset_ = offset_us < 0 ? -q : q;
}

// The number of the first clock of the followed loop whose time is not before the start of this block: the loop's next prediction, advanced over any clock that
// the loop has not been given yet but whose time has passed (the render runs a block ahead of the sound, and the last clock may be a period old). Bounded: a master that
// is quieter than that is stale and the link has stopped following it.
std::uint64_t Sequencer::next_follow_clock() const noexcept {
  const midi::ClockDll& d = *follow_;
  const SQ32 now_rel = (static_cast<SQ32>(now_) - static_cast<SQ32>(d.origin())) * static_cast<SQ32>(core::kQ32One);
  std::uint64_t c = d.tick() + 1u;
  for (std::uint32_t i = 0; i < 64u && d.time_of_tick(c) < now_rel; ++i) {
    ++c;
  }
  return c;
}

void Sequencer::apply(const midi::TransportEvent& ev) noexcept {
  using K = midi::TransportEvent::Kind;
  switch (ev.kind) {
    case K::Spp:
      locate(ev.spp);
      break;
    case K::Stop:
      stop();
      break;
    case K::Start:
      follow_base_tick_ = ev.tick;
      ext_start_ = true;
      pending_start_ = true;
      pending_resume_ = false;
      pending_mode_ = mode_;
      pending_order_ = mode_ == PlayMode::Song ? 0u : pattern_id_;
      pending_from_ = 0;
      pending_count_in_ = 0;  // an external Start is the master's: no pre-roll of ours
      located_ = false;
      break;
    case K::Continue:
      follow_base_tick_ = ev.tick;
      ext_start_ = true;
      pending_resume_ = true;
      pending_start_ = false;
      break;
  }
}

Position Sequencer::position() const noexcept {
  Position p{};
  p.playing = playing_;
  p.mode = mode_;
  p.order_pos = static_cast<std::uint16_t>(mode_ == PlayMode::Song ? order_pos_ : kNoOrder);
  p.pattern_id = static_cast<std::uint8_t>(pattern_id_);
  p.step = static_cast<std::uint16_t>(song_step_);
  p.pattern_len = static_cast<std::uint16_t>(pattern_len_);
  p.tempo_x100 = tempo_x100();
  p.ticks_per_step = static_cast<std::uint8_t>(ticks_per_step_);
  p.ustep = static_cast<std::uint8_t>(ustep_ != 0u ? ustep_ - 1u : 0u);  // `ustep_` counts the microsteps run: the playhead is in the last of them
  p.count_in = static_cast<std::uint8_t>(playing_ ? count_in_left_ : 0u);
  return p;
}

// ---- the song cursor and the tracks' cursors ----------------------------------------------------------------------------------

std::uint32_t Sequencer::song_pattern_len(std::uint32_t pattern_id) const noexcept {
  std::uint32_t len = project_.pattern_length(pattern_id);  // the longest of the active tracks; 0 when the pattern does not exist
  if (len == 0u) {
    len = project_.default_pattern_length();  // a song position naming a missing pattern plays as empty (the model's rule)
  }
  return clamp_u32(len, model::kMinPatternLength, model::kMaxPatternLength);
}

std::uint32_t Sequencer::effective_len() const noexcept {
  return perf_pattern_len_ != 0u ? perf_pattern_len_ : pattern_len_;
}

std::uint32_t Sequencer::song_step_index() const noexcept {
  if (mode_ != PlayMode::Song) {
    return song_step_;
  }
  std::uint32_t total = 0;
  const std::uint32_t n = song_len();
  for (std::uint32_t pos = 0; pos < n && pos < order_pos_; ++pos) {
    total += song_pattern_len(song_at(pos));
  }
  return total + song_step_;
}

void Sequencer::locate(std::uint32_t step_index) noexcept {
  std::uint32_t order = 0;
  std::uint32_t step = 0;
  std::uint32_t pid = pattern_id_;
  if (mode_ == PlayMode::Song) {
    const std::uint32_t n = song_len();
    std::uint32_t total = 0;
    for (std::uint32_t pos = 0; pos < n; ++pos) {
      total += song_pattern_len(song_at(pos));
    }
    if (total != 0u) {
      std::uint32_t rest = step_index % total;  // past the end of the song it wraps, as the song does
      for (std::uint32_t pos = 0; pos < n; ++pos) {
        const std::uint32_t len = song_pattern_len(song_at(pos));
        if (rest < len) {
          order = pos;
          step = rest;
          pid = song_at(pos);
          break;
        }
        rest -= len;
      }
    } else {
      pid = 0;
    }
  } else {
    step = step_index % song_pattern_len(pattern_id_);
    order = kNoOrder;
  }
  located_ = true;
  located_order_ = order;
  located_step_ = step;
  located_pattern_ = pid;
  locate_in_place_ = false;
  if (playing_) {
    locate_pending_ = true;  // a jump at the next step boundary
    switch_pending_ = false;  // OS-PATSWITCH: the later request wins
  }
}

void Sequencer::locate_row(std::uint32_t row) noexcept {
  if (!playing_ || count_in_left_ != 0u) {
    return;
  }
  located_ = true;
  located_order_ = order_pos_;  // the order position stays (kNoOrder in Pattern mode), the pattern is the one now playing
  located_pattern_ = pattern_id_;
  located_step_ = row % std::max<std::uint32_t>(song_pattern_len(pattern_id_), 1u);
  locate_in_place_ = true;
  locate_pending_ = true;
  switch_pending_ = false;  // OS-PATSWITCH: the later request wins
}

// OS-PATSWITCH. The command is the main loop's, taken at the start of a block; the switch itself happens at the next step boundary (end_step).
void Sequencer::switch_pattern(std::uint32_t pattern_id) noexcept {
  if (!playing_ || count_in_left_ != 0u || pattern_id < 1u || pattern_id > 255u) {
    return;
  }
  queued_pattern_ = 0;  // the newest intent wins (the live jump's rule, pattern_end)
  queued_jump_ = kNoJump;
  mode_request_ = false;
  if (locate_pending_) {
    locate_pending_ = false;
    located_ = false;
  }
  if (pattern_id == pattern_id_ && mode_ == PlayMode::Pattern) {
    switch_pending_ = false;  // already there: the pick only takes back one that waits (a detent forth and back)
    return;
  }
  switch_pending_ = true;
  switch_id_ = static_cast<std::uint8_t>(pattern_id);
}

// OS-BUG-PASTEHANG. The command is the main loop's, taken at the start of a block; the release itself happens on the first microstep of the next step (run_microstep), once the rows of that step are read.
void Sequencer::release_pattern(std::uint32_t pattern_id) noexcept {
  if (!playing_ || count_in_left_ != 0u || pattern_id < 1u || pattern_id > 255u) {
    return;
  }
  release_id_ = release_req_ && release_id_ != pattern_id ? std::uint8_t{0} : static_cast<std::uint8_t>(pattern_id);
  release_req_ = true;
}

// `loop`: the pattern restarts at its natural end (pattern_end: it ran to its length; a Break pattern, an Order jump or a live jump are not). A ping-pong track that reads the same
// pattern at the same length then goes on with its bounce (OS-BUG-PERFPING): its cycle is 2 L - 2 steps, longer than the pattern it plays when it is as long as the pattern, so the
// restart would cut every bounce at the turn (the order of FWD). Anything else (a different pattern, a cut, a switch, a resume, a new run) starts it again.
void Sequencer::load_tracks(std::uint32_t from_step, bool loop) noexcept {
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    TrackState& k = tr_[t];
    const std::uint8_t was_pattern = k.pattern_id;
    const std::uint8_t was_len = k.len;
    k.pattern_id = static_cast<std::uint8_t>(k.perf_pattern != 0u ? k.perf_pattern : pattern_id_);
    const std::uint32_t len = k.perf_len != 0u ? k.perf_len : track_length(project_, k.pattern_id, t, pattern_len_);
    k.len = static_cast<std::uint8_t>(clamp_u32(len, 1u, 128u));
    if (loop && k.perf_mode == TrackPlayMode::PingPong && k.pattern_id == was_pattern && k.len == was_len && k.pos < k.len) {
      continue;
    }
    const std::uint32_t p = from_step % k.len;
    k.pos = static_cast<std::uint8_t>(k.perf_mode == TrackPlayMode::Backward ? k.len - 1u - p : p);
    k.back = false;
  }
}

void Sequencer::land_track_queue(std::uint32_t t, std::uint64_t at) noexcept {
  TrackState& k = tr_[t];
  k.perf_pattern = k.perf_pending;
  k.perf_pending_set = false;
  k.perf_q = k.perf_q && k.perf_pattern != 0u;  // E-PERF-07: back on the song's pattern, the track is the song's again
  cancel_track_fx(t, at);
}

// ---- starting and stopping ----------------------------------------------------------------------------------------------------

void Sequencer::reset_tracks() noexcept {
  for (TrackState& k : tr_) {
    TrackState fresh;
    fresh.perf_pattern = k.perf_pattern;
    fresh.perf_len = k.perf_len;
    fresh.perf_mode = k.perf_mode;
    fresh.perf_stutter = k.perf_stutter;
    fresh.perf_pending = k.perf_pending;
    fresh.perf_pending_set = k.perf_pending_set;
    fresh.perf_q = k.perf_q;
    k = fresh;
  }
  for (PfxState& x : pfx_) {
    x = PfxState{};
  }
}

void Sequencer::begin_run(PlayMode mode, std::uint32_t order_pos, std::uint32_t pattern_id, std::uint32_t from_step, bool resuming) noexcept {
  playing_ = true;
  mode_ = mode;
  mode_request_ = false;
  queued_pattern_ = resuming ? queued_pattern_ : 0u;
  queued_jump_ = resuming ? queued_jump_ : kNoJump;
  after_gone_ = false;  // E-SONG-07: the run starts at order_pos, which names the slot to play
  order_pos_ = order_pos;
  pattern_id_ = pattern_id;
  pattern_len_ = song_pattern_len(pattern_id_);
  song_step_ = from_step % std::max<std::uint32_t>(effective_len(), 1u);
  // OS-METRO / OS-COUNTIN: the count-in of a fresh Pattern run. The transport runs for 16 N steps (its own clock, swing, MIDI clock and click) while the play line WAITS on the start point: the tracks
  // play nothing and the step does not advance, so the start point is the first step played when the count-in ends. Anything else (a song, a resume, an external Start) has none.
  count_in_total_ = 0;
  count_in_left_ = 0;
  if (!resuming && mode == PlayMode::Pattern && pending_count_in_ != 0u) {
    count_in_total_ = static_cast<std::uint16_t>(pending_count_in_ * kStepsPerBar);
    count_in_left_ = count_in_total_;
  }
  pending_count_in_ = 0;
  break_pending_ = false;
  jump_pending_ = false;
  locate_pending_ = false;
  switch_pending_ = false;  // OS-PATSWITCH: a run starts with nothing waiting
  switch_release_ = false;
  release_req_ = false;
  release_only_ = 0;
  if (!resuming) {
    // A fresh run: every FX ends and the swing pair starts from its first step, the performance tempo
    // override went at the last stop. The random generator restarts from its seed, so a run is reproducible.
    reset_tracks();
    ctl_.reset_columns();  // the sequencer's FX end; the performance layer's stay (only the sequencer's end): the voices' FxReset does the same
    fx_tempo_x100_ = 0;
    swing_fx_ = 0;
    ticks_per_step_ = kDefaultTicksPerStep;
    rng_.seed(cfg_.seed);
    tempo_sm_.set(static_cast<std::int32_t>(clamp_u32(project_.tempo_x100(), kMinTempoX100, kMaxTempoX100)));
    emit(now_, EventKind::FxReset, audio::kNoTrack, 0, 0, 0, 0, 0);
  } else {
    for (TrackState& k : tr_) {
      k.step_open = false;
      k.note_open = false;
      k.roll_on = false;
      k.arp_on = false;
      k.midi_on = false;
    }
    for (PfxState& x : pfx_) {
      x.sent_extra_n = 0;
    }
  }
  swing_odd_ = false;  // the swing pair starts at the first step of a run (the pair parity is cleared at a stop and only a play after a stop is a run)
  const bool follow = follow_ != nullptr && follow_->has_phase();
  micro_per_clock_ = static_cast<std::uint8_t>(micro_per_clock(rate_));
  ustep_ = 0;
  micro_index_ = 0;
  step_start_ = 0;
  step_dur_ = 0;
  if (follow) {
    origin_ = follow_->origin();
    if (!ext_start_) {
      follow_base_tick_ = next_follow_clock();  // a start of our own (the Play key) under an external clock: on the master's next clock, never in the past
    }
  } else {
    origin_ = now_ + cfg_.start_lead;
  }
  ext_start_ = false;
  load_tracks(song_step_);
  next_abs_ = follow ? micro_time_raw(0) : origin_;
  last_follow_abs_ = next_abs_;
  clk_on_ = cfg_.emit_clock && !follow;
  clk_next_ = 0;
  clk_tl_ = 8u * played_spt();
  run_transport_ = cfg_.emit_transport;  // E-CFG-03: latched, so that end_run() sends the Stop of a Start that went out
  if (cfg_.emit_clock && run_transport_) {
    const std::uint32_t index = song_step_index();
    const std::uint64_t lead = follow ? 0u : cfg_.start_lead;
    const std::uint64_t t_start = lead != 0u ? next_abs_ - lead / 4u : next_abs_;
    if (!resuming && (index == 0u || count_in_total_ != 0u)) {
      // (OS-METRO: a counted run is a fresh run, announced with Start whatever step it starts on: SPP + Continue would reach only the followers that honour a Song Position Pointer, every follower
      // honours Start. The count-in is their bars 1..N, the start point their bar N + 1.)
      emit(t_start, EventKind::MidiStart, audio::kNoTrack, 0, 0, 0, 0, 0);
    } else {
      // Song Position Pointer, then Continue: the receiver goes to the position and carries on from the next clock (MIDI 1.0).
      emit(next_abs_ - lead, EventKind::MidiSpp, audio::kNoTrack, 0, 0, 0, 0, static_cast<std::int32_t>(index & 0x3FFFu));
      emit(t_start, EventKind::MidiContinue, audio::kNoTrack, 0, 0, 0, 0, 0);
    }
  }
}

void Sequencer::end_run(bool emit_stop, std::uint64_t at) noexcept {
  if (mq_pending_ != 0u) {
    mute_release(mq_pending_, at);  // no boundary is coming: the toggles apply with the stop
  }
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    TrackState& k = tr_[t];
    if (k.sent_valid) {
      if (k.sent_midi) {
        if (k.midi_on) {
          send_note_off(t, at);
        }
      } else if (k.note_open) {
        send_note_off(t, at);
      }
    }
    k.note_open = false;
    k.step_open = false;
    k.roll_on = false;
    k.arp_on = false;
    pfx_[t].sent_extra_n = 0;
    if (k.perf_q) {
      // E-PERF-07: the track queue is live only: a landed override and a wait that never landed go with the run
      k.perf_q = false;
      k.perf_pattern = 0;
      k.perf_pending_set = false;
    }
  }
  queue_.cancel_if(at, [](const audio::SeqEvent& e) { return e.kind == EventKind::Tick; });
  centre_bends(at);  // OS-INSTUI S8b: after the notes' offs
  queued_jump_ = kNoJump;  // a stopped transport has nothing queued (a resume starts a run of its own)
  after_gone_ = false;
  switch_pending_ = false;  // OS-PATSWITCH: a switch that had not landed is given up with the run (a stop, a restart and a resume alike)
  switch_release_ = false;
  release_req_ = false;
  release_only_ = 0;
  if (emit_stop) {
    if (cfg_.emit_clock && run_transport_) {
      emit(at, EventKind::MidiStop, audio::kNoTrack, 0, 0, 0, 0, 0);
    }
    emit(at, EventKind::Stopped, audio::kNoTrack, 0, 0, 0, 0, 0);
  }
  if (emit_stop && locate_pending_) {
    locate_pending_ = false;  // a jump (OS-PLAYCUR, or an SPP) that had not landed before the stop is given up: a resume or a Continue goes on where the run stopped, not at the located row
    located_ = false;
  }
  // Where the song cursor was, for resume() (only resume() and a Continue read this).
  have_resume_ = true;
  resume_mode_ = mode_;
  resume_order_ = order_pos_;
  resume_gone_ = false;
  resume_pattern_ = pattern_id_;
  resume_step_ = song_step_;
  playing_ = false;
  count_in_total_ = 0;  // a stop ends the count-in (OS-METRO)
  count_in_left_ = 0;
  clk_on_ = false;
  fx_tempo_x100_ = 0;  // a stop: the performance tempo is 0
  swing_fx_ = 0;
}

// ---- the block --------------------------------------------------------------------------------------------------------------------

std::uint32_t Sequencer::step(std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept {
  if (n == 0u) {
    return 0;
  }
  now_ = block_start;
  block_end_ = block_start + n;
  in_step_ = true;
  adopt_order();  // (also while stopped: a Play after an edit starts from the new list)
  drain_commands();
  refresh_silence();
  flush_program_edits();

  // Transport commands take effect at the start of the block.
  if (pending_stop_) {
    pending_stop_ = false;
    if (playing_) {
      end_run(true, now_);
    }
  }
  if (pending_start_) {
    pending_start_ = false;
    if (playing_) {
      end_run(false, now_);  // a play while playing restarts: the notes go, the transport does not announce a stop
    }
    located_ = false;
    std::uint32_t order = kNoOrder;
    std::uint32_t pid = pending_order_;
    if (pending_mode_ == PlayMode::Song) {
      order = pending_order_ < song_len() ? pending_order_ : 0u;
      pid = song_at(order);
    }
    begin_run(pending_mode_, order, pid, pending_from_, false);
  } else if (pending_resume_) {
    pending_resume_ = false;
    if (playing_) {
      end_run(false, now_);
    }
    PlayMode m = mode_;
    std::uint32_t order = order_pos_;
    std::uint32_t pid = pattern_id_;
    std::uint32_t from = 0;
    if (m == PlayMode::Song && !located_ && !have_resume_) {
      order = 0;  // a Continue before anything has run (OS-MIDISLV) starts the song as a Start does, at its first position, whatever select() was told
      pid = song_at(0);
    }
    if (located_) {
      order = located_order_;
      pid = located_pattern_;
      from = located_step_;
    } else if (have_resume_) {
      m = resume_mode_;
      order = resume_order_;
      pid = resume_pattern_;
      from = resume_step_;
    }
    // E-SONG-07: the slot the stopped song was in was deleted: `order` already is the slot that slid into its place, which plays after the rest of that pattern (a located row has its own slot).
    const bool gone = have_resume_ && !located_ && resume_gone_;
    located_ = false;
    begin_run(m, order, pid, from, true);
    after_gone_ = gone;
    resume_gone_ = false;
  }

  // The tempo smoother moves one block (a ParamChange's ramp, audio/smoothing.h); a step reads it where it starts.
  (void)tempo_sm_.next_block();

  if (playing_) {
    // Bounded by the block: the fastest microstep (800 BPM at double time, the widest swing) is 6.5 samples, so n / 6 + 1 of them fit; the guard is slack for a fault.
    std::uint32_t guard = n / 4u + 64u;
    while (guard != 0u && playing_ && next_abs_ < block_end_) {
      --guard;
      run_microstep(next_abs_);
    }
    if (playing_ && next_abs_ < block_end_) {
      ++stats_.microstep_overrun;
    }
    if (playing_ && clk_on_ && follow_ == nullptr) {
      fill_clock(block_end_ > origin_ ? (block_end_ - origin_) << core::kQ32Frac : 0u);
    }
  }

  // The events of this block, in order; what does not fit waits.
  std::uint32_t count = 0;
  while (!queue_.empty() && queue_.front_key() < block_end_) {
    if (count >= cap) {
      ++stats_.deferred;
      break;
    }
    const std::uint64_t key = queue_.front_key();
    audio::SeqEvent e = *queue_.front();
    queue_.pop();
    std::uint64_t off = 0;
    if (key >= block_start) {
      off = key - block_start;
    } else {
      ++stats_.late_events;
    }
    e.offset = static_cast<std::uint16_t>(off > 0xFFFFu ? 0xFFFFu : off);
    out[count++] = e;
  }
  stats_.events += count;
  in_step_ = false;
  const Position at = position();
  published_.publish(at.playing, at.pattern_id, at.step, at.tempo_x100, at.order_pos, at.ustep);
  published_.publish_count_in(at.count_in);
  published_.publish_queued(queued_jump_);  // the live jump that waits (the Song screen blinks its slot)  // for the screen and the log: two relaxed word stores, once a block
  static_assert(kPerfTracks == audio::PlayPosition::kTrackWords && kPerfTracks <= kMaxTracks, "one published word per track the Perform layer reaches");
  for (std::uint32_t t = 0; t < kPerfTracks; ++t) {  // E-PERF-07: the track queue for the Perform boxes (relaxed stores, 2 a track, once a block)
    const TrackState& k = tr_[t];
    published_.publish_track_pattern(t, k.perf_pattern | (k.perf_pending_set ? (static_cast<std::uint32_t>(k.perf_pending) << 8) | 0x10000u : 0u));
  }
  return count;
}

void Sequencer::run_microstep(std::uint64_t at) noexcept {
  const bool first = ustep_ == 0u;
  const std::uint32_t u = ustep_ + 1u;  // 1..48
  if (first) {
    begin_step(at);
  }
  const std::uint32_t active = std::min<std::uint32_t>(project_.active_tracks(), kMaxTracks);
  if (first && release_req_) {
    release_req_ = false;  // OS-BUG-PASTEHANG: this step's first microstep releases the tracks of the rewritten pattern (a landing, whose switch_release_ is already set, has released every track)
    // The tracks are matched by the pattern they play at THIS boundary, after any pattern end has loaded the next one: a pattern that the song starts at exactly this boundary is the edited pattern's too, so the notes
    // that the pattern left sounding are released with it (a one-step window; the release is a note off, and the first row of the pattern replaces it as ever).
    release_only_ = switch_release_ ? std::uint8_t{0} : release_id_;
    switch_release_ = true;
  }
  if (count_in_left_ == 0u) {  // OS-COUNTIN: a step of the count-in plays nothing, the tracks wait on the start point
    for (std::uint32_t t = 0; t < active; ++t) {
      play_microstep(t, u, at);
    }
  }
  if (first) {
    finish_step();
  }
  if (follow_ != nullptr && cfg_.emit_clock && (micro_index_ % kMicrostepsPerClock) == 0u) {
    emit(at, EventKind::MidiClock, audio::kNoTrack, 0, 0, 0, 0, 0);  // the relay: the clock follows the audio, offset included
  }
  ++stats_.microsteps;
  ++micro_index_;
  if (++ustep_ >= kMicrostepsPerStep) {
    ustep_ = 0;
    end_step();
  }
  next_abs_ = next_micro_time();
}

void Sequencer::begin_step(std::uint64_t at) noexcept {
  rebase();
  if (follow_ == nullptr) {
    micro_per_clock_ = static_cast<std::uint8_t>(micro_per_clock(rate_));  // the map is only read in follow mode: kept current for an attach mid-run
  } else if (micro_per_clock_ != micro_per_clock(rate_)) {
    // Half / double time under an external clock: at this step boundary (micro_index_ is a multiple of 48, so of every microsteps-per-clock) the step grid moves onto the master's
    // clock that it stands on, and the new count of microsteps a clock spans starts from there. The time of `at` is the same on both maps (the clock itself, no fraction).
    follow_base_tick_ += micro_index_ / micro_per_clock_;
    micro_index_ = 0;
    micro_per_clock_ = static_cast<std::uint8_t>(micro_per_clock(rate_));
  }
  tempo_fine_row_ = 0;
  ++stats_.steps;
  // The UI's step change (the events ring of ADR 0013 D6): pattern, step, order position.
  const std::uint32_t op = mode_ == PlayMode::Song ? order_pos_ : kNoOrder;
  const std::uint8_t in_count = count_in_left_ != 0u ? audio::kEvCountIn : std::uint8_t{0};  // OS-METRO: a step of the count-in (the engine keeps it from a take that waits for a step)
  emit(at, EventKind::Step, audio::kNoTrack, static_cast<std::uint8_t>(pattern_id_), static_cast<std::uint8_t>(op & 0xFFu),
       static_cast<std::uint8_t>(op >> 8), in_count, static_cast<std::int32_t>(song_step_));
  if (metro_on_) {
    // A beat on every 4th step, the first of a bar (16 steps) accented: counted from the start of the pattern, and in the count-in from its own first step (so its first beat is a bar's whatever the
    // pattern's length). Same sample as the Step event: swing moves the odd steps, and a beat is an even one.
    const std::uint32_t k = count_in_left_ != 0u ? static_cast<std::uint32_t>(count_in_total_ - count_in_left_) : song_step_;
    if (k % 4u == 0u) {
      emit(at, EventKind::Metro, audio::kNoTrack, (k % kStepsPerBar) == 0u ? 1u : 0u, 0, 0, in_count, static_cast<std::int32_t>(song_step_));
    }
  }
  if (mq_pending_ != 0u && count_in_left_ == 0u) {
    // The boundary of a deferred mute: the first microstep of the step that starts a bar or the pattern, on that sample (before the step's own notes).
    std::uint16_t due = 0;
    for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
      if ((mq_pending_ & (1u << t)) != 0u && (song_step_ == 0u || (mq_kind_[t] == static_cast<std::uint8_t>(MuteQuant::Bar) && song_step_ % kStepsPerBar == 0u))) {
        due = static_cast<std::uint16_t>(due | (1u << t));
      }
    }
    if (due != 0u) {
      mute_release(due, at);
    }
  }
}

// After every track has run its first microstep (the tracks run first, then the step's duration follows from the tempo and swing the
// rows left): this step's tempo, length and, if asked for, its ticks.
void Sequencer::finish_step() noexcept {
  switch_release_ = false;  // every track has run its first microstep (play_microstep): OS-PATSWITCH's release is over
  release_only_ = 0;
  if (tempo_fine_row_ != 0) {
    // Tempo fine adds to the tempo of its row and, like the Tempo FX it refines, stays (med.md section 4; the FX 44 alone adjusts the tempo in force).
    const std::int32_t t = static_cast<std::int32_t>(nominal_tempo_x100()) + tempo_fine_row_;
    fx_tempo_x100_ = static_cast<std::uint32_t>(clamp_i32(t, static_cast<std::int32_t>(kMinTempoX100), static_cast<std::int32_t>(kMaxTempoX100)));
  }
  if (follow_ == nullptr) {
    const Q32 spt = played_spt();
    if (8u * spt != clk_tl_) {
      if (clk_on_) {
        fill_clock(step_start_);  // the ticks before this step keep the old tempo: the new one applies from the row that asked for it
      }
      clk_tl_ = 8u * spt;
    }
    step_dur_ = step_length_q32(spt, swing(), swing_odd_);
    swing_odd_ = !swing_odd_;
  }
  if (cfg_.emit_ticks) {
    const std::uint32_t ticks = ticks_per_step_;
    // OS-PERF3: floor(step_dur_ x i / ticks) for i = 0, 1, ... without a 64-bit division for each tick (the library's __aeabi_uldivmod, hundreds of cycles): with
    // step_dur_ = per x ticks + rem it is per x i + floor(rem x i / ticks), exactly, and rem x i < ticks^2 is a 32-bit division. One 64-bit division a step.
    const Q32 per = ticks != 0u ? step_dur_ / ticks : 0u;
    const std::uint32_t rem = ticks != 0u ? static_cast<std::uint32_t>(step_dur_ - per * ticks) : 0u;
    for (std::uint32_t i = 0; i < ticks; ++i) {
      if (follow_ == nullptr) {
        const Q32 q = step_start_ + per * i + (rem * i) / ticks;
        emit(abs_of(q), EventKind::Tick, audio::kNoTrack, static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(ticks), 0, 0, sfrac(q));
      } else {
        const std::uint32_t j = i * kMicrostepsPerStep / ticks;  // follow mode: on the microstep grid
        emit(micro_time_raw(micro_index_ + j), EventKind::Tick, audio::kNoTrack, static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(ticks), 0, 0, 0);
      }
    }
  }
}

// The sample of a time is the nearest one and the event carries the rest, signed (audio/events.h): the same rounding as every other event, so that an event is never
// earlier than the microstep that made it and a block boundary cannot move it.
void Sequencer::fill_clock(Q32 until) noexcept {
  while (clk_next_ < until) {
    emit(abs_of(clk_next_), EventKind::MidiClock, audio::kNoTrack, 0, 0, 0, 0, sfrac(clk_next_));
    clk_next_ += clk_tl_;
  }
}

void Sequencer::rebase() noexcept {
  if (follow_ != nullptr || step_start_ < (Q32{1} << 62)) {
    return;
  }
  const Q32 whole = step_start_ >> core::kQ32Frac;
  const Q32 shift = whole << core::kQ32Frac;
  origin_ += whole;
  step_start_ -= shift;
  clk_next_ = clk_next_ >= shift ? clk_next_ - shift : 0u;
}

// Follow mode: the sample of microstep `m` of the run, from the loop: clock m / 8 plus m % 8 eighths of the way to the next, shifted by the user's offset. Pure: it does not
// move any state, so the ticks and the step boundary may ask for times ahead.
std::uint64_t Sequencer::micro_time_raw(std::uint64_t m) const noexcept {
  const midi::ClockDll& d = *follow_;
  const std::uint64_t c = follow_base_tick_ + m / micro_per_clock_;
  const std::uint32_t j = static_cast<std::uint32_t>(m % micro_per_clock_);
  const SQ32 t_c = d.time_of_tick(c);
  const SQ32 t_n = d.time_of_tick(c + 1u);
  SQ32 t = t_c + (t_n - t_c) * static_cast<SQ32>(j) / static_cast<SQ32>(micro_per_clock_);
  // The user's offset: a delay shifts the timeline, an advance may reach back one clock period at most (the loop only predicts one clock ahead).
  const SQ32 period = static_cast<SQ32>(d.period());
  t += follow_offset_ < -period ? -period : follow_offset_;
  const std::int64_t abs = static_cast<std::int64_t>(d.origin()) + core::sq32_round(t);
  return abs < 0 ? 0u : static_cast<std::uint64_t>(abs);
}

// The sample of the next microstep to run: S + D j / 48 of its step, nearest sample (core/qtime.h); in follow mode the loop's time, and the timeline never goes back (the loop
// never revises a time it gave, so this only guards an offset that changes).
std::uint64_t Sequencer::next_micro_time() noexcept {
  if (follow_ != nullptr && follow_->has_phase()) {
    std::uint64_t a = micro_time_raw(micro_index_);
    if (a < last_follow_abs_) {
      a = last_follow_abs_;
    }
    last_follow_abs_ = a;
    return a;
  }
  return abs_of(ustep_ == 0u ? step_start_ : step_start_ + step_dur_ * ustep_ / kMicrostepsPerStep);
}

std::uint64_t Sequencer::step_boundary_time() const noexcept {
  if (follow_ != nullptr && follow_->has_phase()) {
    return micro_time_raw(micro_index_);
  }
  return abs_of(step_start_);
}

// ---- the end of a step, of a pattern -------------------------------------------------------------------------------------------

void Sequencer::end_step() noexcept {
  if (follow_ == nullptr) {
    step_start_ += step_dur_;
  }
  if (count_in_left_ != 0u) {
    --count_in_left_;  // the step that has just run was one of the count-in's: the play line stays on the start point (OS-COUNTIN)
    return;
  }
  advance_tracks();
  ++song_step_;
  if (locate_pending_) {
    // An SPP or a locate_row() (OS-PLAYCUR) while playing: the song cursor goes there at this step boundary. This branch comes before the break / jump test below, so a break or a jump queued for this boundary
    // is not acted on here: it stays queued and fires one step after the located row, as with an SPP.
    locate_pending_ = false;
    located_ = false;
    // E-SONG-07: a locate to a slot (an SPP) plays that slot once, whatever slot was deleted before. A locate_row() keeps the mark: it names the slot that slid into the deleted one's place and the
    // deleted one's pattern, which plays on and is followed by that slot. (Not told apart by the indices: a song that repeats a pattern has the same ones for both.)
    after_gone_ = after_gone_ && locate_in_place_;
    order_pos_ = located_order_;
    pattern_id_ = located_pattern_;
    pattern_len_ = song_pattern_len(pattern_id_);
    song_step_ = located_step_;
    load_tracks(song_step_);
    const std::uint64_t at = step_boundary_time();
    emit(at, EventKind::Pattern, audio::kNoTrack, static_cast<std::uint8_t>(pattern_id_), static_cast<std::uint8_t>((mode_ == PlayMode::Song ? order_pos_ : kNoOrder) & 0xFFu),
         static_cast<std::uint8_t>((mode_ == PlayMode::Song ? order_pos_ : kNoOrder) >> 8), 0, static_cast<std::int32_t>(pattern_len_));
    return;
  }
  if (switch_pending_) {
    // OS-PATSWITCH: the playing pattern changes here, at the step boundary, and every track goes on at the same row. The row: the next row of the old pattern, or 0 when the old pattern ends at this boundary
    // (its last step has run, or a Break pattern / Order jump ends it: those belong to the pattern left and go with it), then modulo the running length of the new one (the Perform length when it is set).
    // `song_step_` is already the next row here. The grid, the swing pair and the metronome count (the Step events) are not touched: only the row changes.
    switch_pending_ = false;
    const bool old_ends = song_step_ >= effective_len() || break_pending_ || jump_pending_;
    const std::uint32_t next = old_ends ? 0u : song_step_;
    break_pending_ = false;
    jump_pending_ = false;
    mode_ = PlayMode::Pattern;
    pattern_id_ = switch_id_;
    pattern_len_ = song_pattern_len(pattern_id_);
    song_step_ = next % std::max<std::uint32_t>(effective_len(), 1u);
    load_tracks(song_step_);
    switch_release_ = true;
    const std::uint32_t count = ((landing_pub_.load(std::memory_order_relaxed) >> 16) + 1u) & 0xFFFFu;
    landing_pub_.store((count << 16) | (pattern_id_ << 8) | (song_step_ & 0xFFu), std::memory_order_relaxed);
    const std::uint64_t at = step_boundary_time();
    emit(at, EventKind::Pattern, audio::kNoTrack, static_cast<std::uint8_t>(pattern_id_), static_cast<std::uint8_t>(kNoOrder & 0xFFu), static_cast<std::uint8_t>(kNoOrder >> 8), 0,
         static_cast<std::int32_t>(pattern_len_));
    return;
  }
  if (song_step_ >= effective_len() || break_pending_ || jump_pending_ || (cfg_.pattern_once && cfg_.pattern_stop != 0u && song_step_ >= cfg_.pattern_stop)) {
    pattern_end();  // (pattern_stop is the export of a selection's: pattern_end() then stops the run, as at the end of the pattern)
  }
}

void Sequencer::cancel_track_fx(std::uint32_t t, std::uint64_t at) noexcept {
  std::uint32_t bits = ctl_.active_groups(static_cast<std::uint8_t>(t));
  for (std::uint32_t g = 1; g < kFxGroupCount; ++g) {
    if ((bits >> g) & 1u) {
      emit(at, EventKind::ModEnd, static_cast<std::uint8_t>(t), 0, 0, 0, static_cast<std::uint8_t>(g), 0);
    }
  }
  ctl_.clear_track(static_cast<std::uint8_t>(t));
}

void Sequencer::advance_tracks() noexcept {
  const std::uint32_t active = std::min<std::uint32_t>(project_.active_tracks(), kMaxTracks);
  for (std::uint32_t t = 0; t < active; ++t) {
    TrackState& k = tr_[t];
    bool loop_end = false;
    switch (k.perf_mode) {
      case TrackPlayMode::Forward:
        if (++k.pos >= k.len) {
          k.pos = 0;
          loop_end = true;
        }
        break;
      case TrackPlayMode::Backward:
        if (k.pos == 0u || k.pos >= k.len) {
          k.pos = static_cast<std::uint8_t>(k.len - 1u);
          loop_end = true;
        } else {
          --k.pos;
        }
        break;
      case TrackPlayMode::PingPong: {
        // 0, 1, ... L-1, L-2, ... 1, 0, 1 ...: a clean bounce (our choice: each end step once, back to step 0). L is the track's length, or the Perform pattern
        // length when that is shorter (the pattern restart does not cut the bounce: load_tracks). A shrinking L takes a track that is past it to the top.
        const std::uint32_t span = std::min<std::uint32_t>(k.len, effective_len());
        if (span <= 1u) {
          k.pos = 0;
          loop_end = true;
        } else {
          if (k.pos >= span) {
            k.pos = static_cast<std::uint8_t>(span - 1u);
          }
          if (!k.back) {
            if (k.pos + 1u >= span) {
              k.pos = static_cast<std::uint8_t>(span - 2u);
              if (k.pos == 0u) {
                loop_end = true;  // two steps: 0 1 0 1 ... the turn is already the bounce's end
              } else {
                k.back = true;
              }
            } else {
              ++k.pos;
            }
          } else if (k.pos <= 1u) {
            k.back = false;
            k.pos = 0;
            loop_end = true;  // the bounce is over: the track's own loop end
          } else {
            --k.pos;
          }
        }
        break;
      }
      case TrackPlayMode::Random:
        k.pos = static_cast<std::uint8_t>(rng_.range(0, static_cast<std::int32_t>(k.len)));
        break;
    }
    if (loop_end && k.perf_pending_set) {
      // The track's own loop end: its source pattern changes, its FX end.
      land_track_queue(t, step_boundary_time());
      k.pattern_id = static_cast<std::uint8_t>(k.perf_pattern != 0u ? k.perf_pattern : pattern_id_);
      const std::uint32_t len = k.perf_len != 0u ? k.perf_len : track_length(project_, k.pattern_id, t, pattern_len_);
      k.len = static_cast<std::uint8_t>(clamp_u32(len, 1u, 128u));
      k.pos = 0;
    }
  }
}

void Sequencer::pattern_end() noexcept {
  const std::uint64_t at = step_boundary_time();
  // The pattern ran to its length (no Break pattern, Order jump or live jump cut it): a ping-pong track keeps its bounce through the restart (load_tracks).
  const bool natural = song_step_ >= effective_len() && !break_pending_ && !jump_pending_ && queued_jump_ == kNoJump;
  break_pending_ = false;
  // E-SONG-07: the playing slot was deleted while it played: order_pos_ already is the slot that slid into its place, so the natural advance does not step over it. Cleared here, on every way out.
  bool stay = after_gone_;
  after_gone_ = false;
  if (cfg_.pattern_once && mode_ == PlayMode::Pattern) {
    end_run(true, at);  // OS-WAVEXP: one pass of the pattern, and the transport stops where it ends (nothing queued or requested goes on)
    return;
  }
  bool order_set = false;
  // The live jump (queue_jump): the song carries on at that position. The pattern's own Order jump FX wins (the queue waits for the next end); a position the song no longer has is dropped.
  bool live = false;
  if (!jump_pending_ && queued_jump_ != kNoJump) {
    live = queued_jump_ < song_len();
    if (live) {
      mode_ = PlayMode::Song;
      order_pos_ = queued_jump_;
      order_set = true;
      queued_pattern_ = 0;
      mode_request_ = false;
    } else {
      ++stats_.fx_ignored;
    }
    queued_jump_ = kNoJump;
  }
  if (live) {
    // nothing more to choose: the jump's position plays next
  } else if (jump_pending_) {
    jump_pending_ = false;
    if (mode_ == PlayMode::Song && jump_order_ < song_len()) {
      order_pos_ = jump_order_;  // Order jump (core FX 45): the song carries on at that position
      order_set = true;
    } else {
      ++stats_.fx_ignored;  // out of range, or no song to jump in: it ends the pattern like a Break pattern (the pattern restarts)
    }
    mode_request_ = false;
  } else if (queued_pattern_ != 0u) {
    // The sequential switch (this OS's; whether the official firmware has one is an open research question): the queued pattern plays next and the mode is Pattern.
    mode_ = PlayMode::Pattern;
    pattern_id_ = queued_pattern_;
    queued_pattern_ = 0;
    mode_request_ = false;
    order_set = true;  // nothing to advance
  } else {
    if (mode_request_) {
      mode_request_ = false;
      if (requested_mode_ == PlayMode::Song && mode_ == PlayMode::Pattern) {
        // Pattern to Song without stopping: the song carries on after the first position that holds the pattern being played.
        std::uint32_t found = song_len();
        for (std::uint32_t pos = 0; pos < song_len(); ++pos) {
          if (song_at(pos) == pattern_id_) {
            found = pos;
            break;
          }
        }
        if (found < song_len()) {
          order_pos_ = found;  // the song carries on from the position after it
          stay = false;
        } else {
          order_pos_ = 0;      // the pattern is not in the song: the song starts at its beginning
          order_set = true;
        }
      }
      mode_ = requested_mode_;
    }
  }
  if (!order_set && mode_ == PlayMode::Song) {
    order_pos_ += stay ? 0u : 1u;
    if (order_pos_ >= song_len()) {
      emit(at, EventKind::SongEnd, audio::kNoTrack, 0, 0, 0, 0, 0);
      if (cfg_.song_loop) {
        order_pos_ = 0;
      } else {
        end_run(true, at);
        return;
      }
    }
  }
  if (mode_ == PlayMode::Song) {
    if (cfg_.order_stop != 0u && order_pos_ >= cfg_.order_stop) {
      end_run(true, at);  // OS-SONGSHARE S5: the export of a range of slots ends with its last slot (and a jump past it ends it too); a position compare once a pattern, never per block
      return;
    }
    pattern_id_ = song_at(order_pos_);
  }
  // E-PERF-07: a queued track that cannot reach its own loop end (a Random track, or one longer than the pattern that is ending, which the restart below cuts) lands here instead.
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    const TrackState& k = tr_[t];
    if (k.perf_q && k.perf_pending_set && (k.perf_mode == TrackPlayMode::Random || k.len > effective_len())) {
      land_track_queue(t, at);
    }
  }
  pattern_len_ = song_pattern_len(pattern_id_);
  song_step_ = 0;
  load_tracks(0, natural);
  const std::uint32_t op = mode_ == PlayMode::Song ? order_pos_ : kNoOrder;
  emit(at, EventKind::Pattern, audio::kNoTrack, static_cast<std::uint8_t>(pattern_id_), static_cast<std::uint8_t>(op & 0xFFu), static_cast<std::uint8_t>(op >> 8), 0,
       static_cast<std::int32_t>(pattern_len_));
}

// ---- the tracks ---------------------------------------------------------------------------------------------------------------------

// The tracks whose MIDI notes are held back: the muted ones and, when any active track is soloed, every active track that is not (the graph's rule: Graph::recompute_strip). One read of
// the project's flags per block, from the render context as every other read of the project here.
void Sequencer::refresh_silence() noexcept {
  const std::uint32_t active = std::min<std::uint32_t>(project_.active_tracks(), kMaxTracks);
  std::uint32_t mute = 0;
  std::uint32_t solo = 0;
  for (std::uint32_t t = 0; t < active; ++t) {
    const model::TrackSlot* s = project_.track(t);
    if (s != nullptr) {
      mute |= static_cast<std::uint32_t>((s->flags & model::kTrackMute) != 0u) << t;
      solo |= static_cast<std::uint32_t>((s->flags & model::kTrackSolo) != 0u) << t;
    }
  }
  mute = (mute & ~static_cast<std::uint32_t>(mq_pending_)) | (mq_hold_ & mq_pending_);  // a deferred toggle: the old value until its boundary
  const std::uint32_t all = (1u << active) - 1u;
  silent_ = static_cast<std::uint16_t>((mute | (solo != 0u ? all & ~solo : 0u)) & all);
}

// A deferred mute toggle (MuteQuant): with the transport stopped there is no boundary, it applies now.
void Sequencer::mute_defer(std::uint32_t t, std::uint32_t quant, bool held) noexcept {
  if (t >= kMaxTracks) {
    return;
  }
  if (!playing_ || (quant != static_cast<std::uint32_t>(MuteQuant::Bar) && quant != static_cast<std::uint32_t>(MuteQuant::Pattern))) {
    mute_release(static_cast<std::uint16_t>(1u << t), command_time());
    return;
  }
  mq_pending_ = static_cast<std::uint16_t>(mq_pending_ | (1u << t));
  mq_hold_ = static_cast<std::uint16_t>(held ? (mq_hold_ | (1u << t)) : (mq_hold_ & ~(1u << t)));
  mq_kind_[t] = static_cast<std::uint8_t>(quant);
  pending_pub_.store(mq_pending_, std::memory_order_relaxed);
  refresh_silence();
}

void Sequencer::mute_cancel(std::uint32_t t) noexcept {
  if (t >= kMaxTracks || (mq_pending_ & (1u << t)) == 0u) {
    return;
  }
  mq_pending_ = static_cast<std::uint16_t>(mq_pending_ & ~(1u << t));
  pending_pub_.store(mq_pending_, std::memory_order_relaxed);
  refresh_silence();
}

// The boundary (or a stop): every track in `mask` takes the project's mute now: the strip is told at `at`, the hold ends. A track with nothing pending in `mask` is told as well (the immediate case).
void Sequencer::mute_release(std::uint16_t mask, std::uint64_t at) noexcept {
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    if ((mask & (1u << t)) == 0u) {
      continue;
    }
    const model::TrackSlot* s = project_.track(t);
    emit(at, EventKind::StripMute, static_cast<std::uint8_t>(t), (s != nullptr && (s->flags & model::kTrackMute) != 0u) ? 1u : 0u, 0, 0, 0, 0);
  }
  mq_pending_ = static_cast<std::uint16_t>(mq_pending_ & ~mask);
  pending_pub_.store(mq_pending_, std::memory_order_relaxed);
  refresh_silence();
}

bool Sequencer::is_midi_slot(std::uint8_t slot) const noexcept {
  const model::Instrument* in = project_.instrument(slot);
  return in != nullptr && in->kind == model::InstrumentKind::Midi;
}

// The velocity a note has when the step names none: a sample instrument's volume 0..100 as 0..127 (volume * 127 / 100:
// integer, truncating), a MIDI instrument's VELOCITY (OS-INSTUI S8b, INST 1.3; 127 until it is set, the default MIDI velocity).
std::uint8_t Sequencer::default_velocity(std::uint8_t slot) const noexcept {
  const model::Instrument* in = project_.instrument(slot);
  if (in == nullptr) {
    return 127;
  }
  if (in->kind == model::InstrumentKind::Midi) {
    return midi_velocity_of(*in);
  }
  const std::uint32_t v = in->sampler.volume > 100u ? 100u : in->sampler.volume;
  return static_cast<std::uint8_t>(v * 127u / 100u);
}

void Sequencer::read_row(std::uint32_t t) noexcept {
  TrackState& k = tr_[t];
  k.pattern_id = static_cast<std::uint8_t>(k.perf_pattern != 0u ? k.perf_pattern : pattern_id_);
  const std::uint32_t len = k.perf_len != 0u ? k.perf_len : track_length(project_, k.pattern_id, t, pattern_len_);
  k.len = static_cast<std::uint8_t>(clamp_u32(len, 1u, 128u));
  if (k.pos >= k.len) {
    k.pos = 0;  // the track got shorter under the cursor (an edit while playing)
  }
  const RowStatus st = read_step(project_, k.pattern_id, t, k.pos, k.row);
  if (st == RowStatus::Torn) {
    ++stats_.rows_torn;
  } else if (st == RowStatus::NoPattern && t == 0u) {
    ++stats_.rows_missing;  // counted once a step: the pattern is missing for every track
  }
  fx_sanitize(k.row.fx[0].type, k.row.fx[0].value);
  fx_sanitize(k.row.fx[1].type, k.row.fx[1].value);
  k.cancel = false;
  k.delayed = false;
  k.start_at = 1;
  k.gate = 0;
  k.force_roll = false;
  k.vel = k.row.vol != model::kVolumeUnset ? static_cast<std::uint8_t>(std::min<std::uint32_t>(k.row.vol, 127u)) : audio::kVelocityDefault;
}

// OS-PATSWITCH: the first step after a landing, for one track, once its row is read and its Chance and Micro-move are known (pre_fx). A switch in mid-pattern skips the OFF rows of the pattern left, so what
// they would have ended is ended here, at the sample of the landing, as an OFF row ends it (start_step): a MIDI note still on gets its off whatever the new row is (nothing hangs, also on a track the new pattern
// leaves empty), a sampler note still open is released (a note off, not a cut: the voice's own release runs), and the step it opened is closed, so that a Roll, an arpeggio or a stutter of the old row does not go on
// through the new pattern's empty rows. A track whose new row starts a note (a note, an Off, a Cut or a Fade on this microstep) is left to that row: start_step sees to the old note.
void Sequencer::switch_release(std::uint32_t t, std::uint64_t at) noexcept {
  TrackState& k = tr_[t];
  const bool midi_on = k.midi_on;
  if (midi_on) {
    send_note_off(t, at);
  }
  const bool starts = note_kind(k.row.note) != NoteKind::Empty && !k.cancel && !k.delayed;
  if (!starts && k.sent_valid && (midi_on || k.note_open || k.step_open)) {
    if (!k.sent_midi && k.note_open) {
      send_note_off(t, at);
    }
    k.note_open = false;
    k.step_open = false;
    k.roll_on = false;
    k.arp_on = false;
  }
}

void Sequencer::send_note_on(std::uint32_t t, std::uint8_t note, std::uint8_t slot, std::uint8_t vel, bool retrigger, std::uint64_t at) noexcept {
  TrackState& k = tr_[t];
  const model::Instrument* in = project_.instrument(slot);
  if (in == nullptr || in->kind == model::InstrumentKind::None) {
    return;  // a free slot makes no sound (a note on an inactive instrument returns at once)
  }
  activity_[t].store(static_cast<std::uint8_t>(activity_[t].load(std::memory_order_relaxed) + 1u), std::memory_order_relaxed);  // the one writer: a lamp's pulse (note_count)
  if (in->kind == model::InstrumentKind::Midi) {
    if (k.midi_on) {
      send_note_off(t, at);  // one voice per track: the note that is sounding ends first, whatever sounded before it (PAR-063)
    }
    const MidiRoute r = midi_route(*in);  // OS-INSTUI S8b: the channel, the port and the OFFSET of the instrument
    const std::uint8_t ch = r.ch;
    const std::uint8_t v = vel == audio::kVelocityDefault ? midi_velocity_of(*in) : (vel == 0u ? std::uint8_t{1} : vel);  // velocity 0 would be a note-off on the wire
    if (!midi_silenced(t)) {  // a muted (or solo-silenced) MIDI track holds its notes back: the graph does not carry MIDI (OS-MUTE)
      const int program = program_due(*in);  // the receiver is put on the instrument's program before its first note (and again when another instrument changed it on the channel)
      if (program >= 0 && emit_midi(at, EventKind::MidiProgram, static_cast<std::uint8_t>(t), static_cast<std::uint8_t>(program), 0, r)) {
        program_sent(*in, program);  // (recorded only when the event is in the queue: a full queue asks again at the next note)
      }
      int cents = 0;  // the Micro-tune FX of the row (the first column wins over the second, as the voices' arbitration does); a roll or arp repeat reads the same row
      for (int col = 1; col >= 0; --col) {
        if (k.row.fx[col].type == fx::kMicroTune) {
          cents = microtune_cents(k.row.fx[col].value);
        }
      }
      const std::uint16_t bend = pitch_bend_value(cents, in->midi_pb_range);  // the Micro-tune FX of the step as a pitch bend; the centre again for a note without one
      // The bend is per route: a route is bent from its first non-centre bend until a centre goes to it (a note of its own without the FX, or the stop). Every bend is sent; the centre only to a route that is
      // bent. The route's bit is changed only when the event is in the queue (a full queue records nothing as sent).
      const std::uint64_t bit = std::uint64_t{1} << (4u * ch + r.ports);
      const bool centre = bend == kBendCentre;
      if (centre ? (bend_routes_ & bit) != 0u : true) {
        if (emit_midi(at, EventKind::MidiPitchBend, static_cast<std::uint8_t>(t), static_cast<std::uint8_t>(bend & 0x7Fu), static_cast<std::uint8_t>(bend >> 7), r)) {
          bend_routes_ = centre ? (bend_routes_ & ~bit) : (bend_routes_ | bit);
        }
      }
      emit_midi(at, EventKind::MidiNoteOn, static_cast<std::uint8_t>(t), note, v, r);
      k.midi_on = true;
      PfxState& x = pfx_[t];
      x.sent_extra_n = 0;
      for (std::uint32_t i = 0; i < x.chord_n; ++i) {  // OS-PFX: the Chord FX's other notes, together with the first (a repeat of a Roll sends them again, above its own note)
        const std::uint32_t n = static_cast<std::uint32_t>(note) + x.chord[i];
        if (n <= 127u) {
          emit_midi(at, EventKind::MidiNoteOn, static_cast<std::uint8_t>(t), static_cast<std::uint8_t>(n), v, r);
          x.sent_extra[x.sent_extra_n++] = static_cast<std::uint8_t>(n);
        }
      }
    }
    k.sent_midi = true;
    k.sent_ch = ch;
    k.sent_ports = r.ports;  // the off goes where the on went, whatever the instrument says by then (an OUT, CHANNEL or OFFSET edit under a held note)
    k.sent_delay_ms = r.delay_ms;
  } else {
    emit(at, EventKind::NoteOn, static_cast<std::uint8_t>(t), note, slot, vel, retrigger ? audio::kEvRetrigger : std::uint8_t{0}, 0);
    k.sent_midi = false;
  }
  k.sent_valid = true;
  k.sent_note = note;
  k.sent_slot = slot;
  k.sent_vel = vel;
  k.last_slot = slot;
}

void Sequencer::send_note_off(std::uint32_t t, std::uint64_t at) noexcept {
  TrackState& k = tr_[t];
  if (!k.sent_valid) {
    return;
  }
  if (k.sent_midi) {
    if (k.midi_on) {
      const MidiRoute r{k.sent_ch, k.sent_ports, k.sent_delay_ms};
      emit_midi(at, EventKind::MidiNoteOff, static_cast<std::uint8_t>(t), k.sent_note, 0, r);
      PfxState& x = pfx_[t];
      for (std::uint32_t i = 0; i < x.sent_extra_n; ++i) {  // OS-PFX: and the chord's other notes
        emit_midi(at, EventKind::MidiNoteOff, static_cast<std::uint8_t>(t), x.sent_extra[i], 0, r);
      }
      x.sent_extra_n = 0;
      k.midi_on = false;
    }
  } else {
    emit(at, EventKind::NoteOff, static_cast<std::uint8_t>(t), k.sent_note, k.sent_slot, 0, 0, 0);
  }
}

// Note CUT and FADE: the voice of the track stops. A MIDI note has no such thing: it ends (PAR-063: this OS's choice, a MIDI note
// never hangs; what the official firmware does is an open research question).
void Sequencer::send_voice_off(std::uint32_t t, EventKind kind, std::uint64_t at) noexcept {
  TrackState& k = tr_[t];
  if (k.sent_midi && k.midi_on) {
    send_note_off(t, at);
    return;
  }
  emit(at, kind, static_cast<std::uint8_t>(t), 0, 0, 0, 0, 0);
}

// The number of steps from the step at the cursor to the next note of the track, or to the end of the track (a track's own length is used). Bounded by the track's length.
std::uint16_t Sequencer::roll_length_steps(std::uint32_t t) const noexcept {
  const TrackState& k = tr_[t];
  std::uint32_t b = 1;
  for (std::uint32_t a = static_cast<std::uint32_t>(k.pos) + 1u; a < k.len; ++a, ++b) {
    model::Step s;
    if (read_step(project_, k.pattern_id, t, a, s) == RowStatus::Ok && note_kind(s.note) != NoteKind::Empty) {
      break;
    }
  }
  return static_cast<std::uint16_t>(b);
}

// The velocity of a note of a volume roll: the velocity of the first note times the progress through
// the stretch up to the next note, rising or falling. Integer here (floor of the exact product); a float computation can differ by one on an exact boundary.
std::uint8_t Sequencer::roll_velocity(const TrackState& k) const noexcept {
  const std::uint32_t total = static_cast<std::uint32_t>(k.roll_len_steps) * kMicrostepsPerStep;
  const std::uint32_t timer = std::min<std::uint32_t>(k.step_timer, total);
  const std::uint32_t vmax = k.roll_start_vel;
  const RollDir d = roll_dir(k.roll_val);
  if (d == RollDir::VolUp) {
    return static_cast<std::uint8_t>(vmax * timer / total);
  }
  if (d == RollDir::VolDown) {
    return static_cast<std::uint8_t>(vmax * (total - timer) / total);
  }
  return 127;
}

void Sequencer::setup_roll(std::uint32_t t, std::uint32_t col, std::uint8_t value, std::uint8_t start_velocity) noexcept {
  TrackState& k = tr_[t];
  if (roll_period(value) == 0u) {
    k.roll_on = false;  // period 0 is "none": inert (a division by zero otherwise; open item)
    ++stats_.fx_ignored;
    return;
  }
  k.roll_on = true;
  k.roll_col = static_cast<std::uint8_t>(col);
  k.roll_val = value;
  const RollDir d = roll_dir(value);
  if (d == RollDir::VolUp || d == RollDir::VolDown) {
    k.roll_len_steps = roll_length_steps(t);
    k.roll_start_vel = start_velocity;
  }
}

void Sequencer::apply_columns(std::uint32_t t, std::uint8_t mask, const std::uint8_t type[2], const std::uint8_t value[2], std::uint64_t at) noexcept {
  ModChange ch[4];
  const std::uint32_t n = ctl_.set_columns(static_cast<std::uint8_t>(t), mask, type, value, ch);
  emit_changes(ch, n, at);
}

// A note starts: both FX columns of its row become the track's: the previous FX of a column ends, a voice FX starts,
// anything else leaves the column empty. A velocity FX is the note's velocity on a step with a note (pre_fx), not a voice override.
void Sequencer::note_start_fx(std::uint32_t t, std::uint64_t at) noexcept {
  const TrackState& k = tr_[t];
  std::uint8_t type[2];
  std::uint8_t value[2];
  for (std::uint32_t c = 0; c < 2u; ++c) {
    type[c] = fx_kind(k.row.fx[c].type) == FxKind::RowVoice ? std::uint8_t{fx::kNone} : k.row.fx[c].type;
    value[c] = k.row.fx[c].value;
  }
  apply_columns(t, 3, type, value, at);
}

// The pre-effects of a row (at the first microstep): second column first, then the first.
void Sequencer::pre_fx(std::uint32_t t, std::uint64_t at) noexcept {
  TrackState& k = tr_[t];
  model::Step& r = k.row;
  const NoteKind nk = note_kind(r.note);
  const bool has_note = nk == NoteKind::Note;

  // Random FX value: the other column's value becomes a random one within +-this value, inside the other FX's range.
  for (std::uint32_t c = 0; c < 2u; ++c) {
    if (r.fx[c].type == fx::kRandomValue) {
      const std::uint32_t o = 1u - c;
      std::int32_t lo = 0;
      std::int32_t hi = 0;
      fx_range(r.fx[o].type, lo, hi);
      const std::int32_t center = r.fx[o].value;
      const std::int32_t spread = r.fx[c].value;
      const std::int32_t low = clamp_i32(center - spread, lo, hi);
      const std::int32_t high = clamp_i32(center + spread, lo, hi);
      const std::uint8_t v = static_cast<std::uint8_t>(rng_.range(low, high + 1));
      r.fx[0].value = v;  // written into both columns
      r.fx[1].value = v;
      break;
    }
  }

  bool consumed_any = false;
  std::uint8_t trk_now = 0;   // OS-COREFX: the columns that hold a tracker FX on this row
  std::uint8_t col_mask = 0;  // the columns whose FX is replaced by this row, applied together after the loop
  std::uint8_t col_type[2] = {0, 0};
  std::uint8_t col_value[2] = {0, 0};
  for (int col = 1; col >= 0; --col) {
    const std::uint8_t type = r.fx[col].type;
    const std::uint8_t value = r.fx[col].value;
    bool consumed = false;
    switch (type) {
      case fx::kMicroMove:
        kill_roll(k, col);
        consumed = true;
        k.delayed = true;
        k.start_at = micromove_start(value);
        break;
      case fx::kVelocity:
        if (has_note) {
          kill_roll(k, col);
          consumed = true;
          k.vel = velocity_from_fx(value);
        }
        break;
      case fx::kRandomVelocity:
        if (has_note) {
          kill_roll(k, col);
          consumed = true;
          k.vel = velocity_from_fx(static_cast<std::uint8_t>(rng_.range(0, static_cast<std::int32_t>(value) + 1)));
        }
        break;
      case fx::kOff:
        kill_roll(k, col);
        consumed = true;
        if (k.roll_col == col) {
          k.delayed = false;
          k.cancel = false;
          k.roll_on = false;
        }
        col_mask = static_cast<std::uint8_t>(col_mask | (1u << col));  // the column's FX ends
        col_type[col] = fx::kNone;
        break;
      case fx::kChance:
        kill_roll(k, col);
        consumed = true;
        if (rng_.range(0, 100) > static_cast<std::int32_t>(value)) {
          k.cancel = true;  // a draw in 0..99 above the value cancels: a value of 100 always plays
        }
        break;
      case fx::kCcA:
      case fx::kCcA + 1:
      case fx::kCcA + 2:
      case fx::kCcA + 3:
      case fx::kCcE:
        kill_roll(k, col);
        consumed = true;
        emit_cc(t, static_cast<std::uint32_t>(type - fx::kCcA), value, at);
        break;
      case fx::kSlot37:
        kill_roll(k, col);
        consumed = true;
        if (kSlot37IsProgramChange) {
          emit(at, EventKind::MidiProgram, static_cast<std::uint8_t>(t), value, 0, audio::kChannelDefault, 0, 0);
        } else {
          emit_cc(t, 5, value, at);
        }
        break;
      case fx::kTempo:
        kill_roll(k, col);
        consumed = true;
        // PAR-082 (documented in the release notes, not verified): the Tempo FX is value * 2 BPM, values below 40 BPM are 40 (the notes say fixed in 1.5.0).
        fx_tempo_x100_ = clamp_u32(static_cast<std::uint32_t>(value) * 2u, kMinTempoFxBpm, 800u) * 100u;
        break;
      case fx::kBreakPattern:
        kill_roll(k, col);
        consumed = true;
        break_pending_ = true;
        break;
      case fx::kSwing:
        consumed = true;
        swing_fx_ = value;
        break;
      case fx::kGate:
        consumed = true;
        k.gate = value;
        break;
      case fx::kChord:
      case fx::kArp:
      case fx::kTune:
        kill_roll(k, col);
        consumed = true;  // read when a note starts (pfx_note); on a step without a note they do nothing
        break;
      case fx::kTrkArp:
      case fx::kTrkVolSlide:
      case fx::kTrkFineVol:
      case fx::kTrkFinePortaUp:
      case fx::kTrkFinePortaDown:
      case fx::kTrkPanSlide:
      case fx::kTrkVibWave:
      case fx::kTrkTremWave:
      case fx::kTrkGliss:
      case fx::kTrkRetrig:
      case fx::kTrkTremor:
      case fx::kTrkChanPan:
      case fx::kTrkChanVol:
      case fx::kTrkChanVolSlide:
      case fx::kTrkGlobalVol:
      case fx::kTrkGlobalVolSlide:
      case fx::kTrkPanbrello:
        // OS-COREFX: a tracker FX lasts the row it stands in. It goes to the voices as an event of its own (not a ModSet: the control table would hold it past the row), on the row's first
        // sample, before the row's note and its Tick 0; audio/tracker_fx.h runs it on the Tick events, and a TrkFx of type 0 ends it on the first row of the column that holds none (below).
        // The column's previous FX stays as it was, like Speed's.
        kill_roll(k, col);
        consumed = true;
        trk_now = static_cast<std::uint8_t>(trk_now | (1u << col));
        emit(at, EventKind::TrkFx, static_cast<std::uint8_t>(t), type, value, 0, static_cast<std::uint8_t>(col), 0);
        break;
      case fx::kSpeed:
        consumed = true;
        ticks_per_step_ = value;
        break;
      case fx::kTempoFine:
        consumed = true;
        tempo_fine_row_ += (static_cast<std::int32_t>(value) - 128) * 25;  // (v - 128) / 4 BPM, in BPM x 100
        break;
      case fx::kOrderJump:
        consumed = true;
        jump_pending_ = true;
        jump_order_ = value;
        break;
      default:
        break;
    }
    if (consumed) {
      consumed_any = true;
    }
    // A step without a note: what is left goes to the voice: a Roll starts a roll of the last note, every other type
    // replaces what the column held. A column the sequencer consumed is left as it was.
    if (nk == NoteKind::Empty && !consumed && !(kSkipVoiceFxAfterSeqFx && consumed_any) && type != fx::kNone) {
      if (type == fx::kRoll) {
        kill_roll(k, col);
        k.step_timer = 1;
        k.force_roll = true;
        setup_roll(t, static_cast<std::uint32_t>(col), value, k.sent_valid ? (k.sent_vel == audio::kVelocityDefault ? default_velocity(k.sent_slot) : k.sent_vel) : std::uint8_t{127});
      } else {
        col_mask = static_cast<std::uint8_t>(col_mask | (1u << col));
        col_type[col] = type;
        col_value[col] = value;
      }
    }
  }
  if ((k.trk_cols | trk_now) != 0u) {  // OS-COREFX: a column that held a tracker FX on the row before and holds none now ends it (the voices keep it until told)
    std::uint8_t pending = trk_now;
    for (std::uint32_t c = 0; c < 2u; ++c) {
      if (((k.trk_cols & ~trk_now) >> c & 1u) != 0u &&
          !emit(at, EventKind::TrkFx, static_cast<std::uint8_t>(t), 0, 0, 0, static_cast<std::uint8_t>(c), 0)) {
        pending = static_cast<std::uint8_t>(pending | (1u << c));  // the queue was full: the end is still owed, the next row tries again
      }
    }
    k.trk_cols = pending;
  }
  if (col_mask != 0u) {
    apply_columns(t, col_mask, col_type, col_value, at);
  }
}

// The note of a row starts. `at` is the sample of the microstep it starts on (Micro-move moves it).
void Sequencer::start_step(std::uint32_t t, std::uint64_t at) noexcept {
  TrackState& k = tr_[t];
  const model::Step& r = k.row;
  const NoteKind nk = note_kind(r.note);
  if (nk == NoteKind::Note) {
    // A step that is open ends with a new note. A MIDI note that is still sounding ends too, whatever the new note is
    // (PAR-063: this OS's choice; a MIDI note never hangs under a later sample note).
    if (k.midi_on) {
      send_note_off(t, at);
    }
    k.step_open = false;
    k.note_open = false;
    k.roll_on = false;
    k.arp_on = false;  // OS-PFX: the arpeggio of the note this one replaces ends with it, and so does its chord
    pfx_[t].chord_n = 0;
  }
  std::uint8_t slot = r.instr != 0u ? static_cast<std::uint8_t>(r.instr - 1u) : k.last_slot;
  std::uint8_t note = nk == NoteKind::Note ? static_cast<std::uint8_t>(r.note - 1u) : std::uint8_t{0};

  // The FX that act when the step starts, second column first.
  for (int col = 1; col >= 0; --col) {
    const std::uint8_t value = r.fx[col].value;
    switch (r.fx[col].type) {
      case fx::kRoll:
        if (nk == NoteKind::Note) {
          const std::uint8_t sv = k.vel == audio::kVelocityDefault ? default_velocity(slot == 0xFFu ? std::uint8_t{0} : slot) : k.vel;
          kill_roll(k, col);
          setup_roll(t, static_cast<std::uint32_t>(col), value, sv);
        }
        break;
      case fx::kRandomNote:
        if (nk == NoteKind::Note) {
          kill_roll(k, col);
          note = static_cast<std::uint8_t>(clamp_i32(rng_.range(static_cast<std::int32_t>(note) - value, static_cast<std::int32_t>(note) + value + 1), 0, 127));
        }
        break;
      case fx::kRandomInstrument:
        if (nk == NoteKind::Note && slot != 0xFFu) {
          kill_roll(k, col);
          slot = static_cast<std::uint8_t>(clamp_i32(rng_.range(static_cast<std::int32_t>(slot) - value, static_cast<std::int32_t>(slot) + value + 1), 0,
                                                     static_cast<std::int32_t>(model::kMaxInstruments) - 1));
        }
        break;
      default:
        break;
    }
  }

  // Open the step: its timer counts up from here, its note lasts 9999 microsteps unless a Roll (half its period) or a Gate shortens it.
  k.step_open = true;
  k.step_timer = 0;
  k.note_open = true;
  k.note_len = static_cast<std::uint16_t>(kStepOpenLimit);
  k.note_timer = 0;
  if (k.roll_on) {
    k.note_len = static_cast<std::uint16_t>(roll_microsteps(roll_period(k.roll_val)) / 2u);
  } else if (k.gate != 0u) {
    // Gate length (PAR-080, 1.4.0: 0..100, the mapping is not sourced): the note is `gate` percent of a step long, on the microstep grid, at least one. Open item.
    const std::uint32_t len = static_cast<std::uint32_t>(k.gate) * kMicrostepsPerStep / 100u;
    k.note_len = static_cast<std::uint16_t>(len == 0u ? 1u : len);
  }

  switch (nk) {
    case NoteKind::Note: {
      if (slot == 0xFFu) {
        ++stats_.fx_ignored;  // a note with no instrument and none played before on this track: nothing to play (open item)
        k.note_open = false;
        break;
      }
      std::uint8_t vel = k.vel;
      if (k.roll_on && roll_dir(k.roll_val) == RollDir::NoteRandom) {
        note = static_cast<std::uint8_t>(clamp_i32(rng_.range(static_cast<std::int32_t>(note) - 12, static_cast<std::int32_t>(note) + 13), 0, 127));
      }
      if (k.roll_on && (roll_dir(k.roll_val) == RollDir::VolUp || roll_dir(k.roll_val) == RollDir::VolDown)) {
        vel = roll_velocity(k);  // step_timer is 0: a rising roll starts silent, a falling one at its full velocity
      }
      if (!is_midi_slot(slot)) {
        note_start_fx(t, at);
      }
      if (is_pfx_fx(r.fx[0].type) || is_pfx_fx(r.fx[1].type)) {
        note = pfx_note(t, note, slot);  // OS-PFX: Tune shifts the note, Chord and Arp set up the notes that go with it (a row without them pays this test only)
      }
      send_note_on(t, note, slot, vel, false, at);
      break;
    }
    case NoteKind::Off:
      send_note_off(t, at);
      k.note_open = false;
      k.step_open = false;
      k.roll_on = false;
      k.arp_on = false;
      break;
    case NoteKind::Cut:
      send_voice_off(t, EventKind::NoteCut, at);
      k.note_open = false;
      k.arp_on = false;
      break;
    case NoteKind::Fade:
      send_voice_off(t, EventKind::NoteFade, at);
      k.note_open = false;
      k.arp_on = false;
      break;
    case NoteKind::Empty:
      break;
  }
}

// OS-PFX (docs/specs/OS-PFX.md): what the note that starts is made of. `note` is the step's note after Random note, 0..127. Tune 40 shifts it, on any instrument that plays
// by pitch (a beat slice instrument is the exception: its note picks a slice, which a Tune would change). Chord 14 sets the track's chord: a MIDI instrument sends all its
// notes together, any other plays the root alone (its voice is one a track). Arp 21 plays that chord one note after another, on any instrument; without a Chord in the other
// column there is nothing to arpeggiate and it does nothing (a Chord of value 0 is a chromatic arpeggio: seq/fx.h arp_mask). The first column wins over the second for each type. Returns the note to send, which is the arpeggio's first.
std::uint8_t Sequencer::pfx_note(std::uint32_t t, std::uint8_t note, std::uint8_t slot) noexcept {
  TrackState& k = tr_[t];
  PfxState& x = pfx_[t];
  x.chord_n = 0;
  k.arp_on = false;
  const model::Instrument* in = project_.instrument(slot);
  if (in == nullptr) {
    return note;  // a free slot makes no sound (send_note_on returns before it sets sent_slot): an arp would play the notes on the instrument of an earlier step
  }
  int shift = 0;
  std::uint8_t chord = 0;
  std::uint8_t arp = 0;
  bool have_chord = false;  // a Chord FX of value 0 is a Chord FX: with an Arp it is the chromatic arpeggio (PAR-078, docs/specs/OS-PFX.md)
  for (int col = 1; col >= 0; --col) {  // the second column first, so that the first overwrites it
    const model::StepFx& f = k.row.fx[col];
    if (f.type == fx::kTune) {
      shift = tune_semitones(f.value);
    } else if (f.type == fx::kChord) {
      chord = f.value;
      have_chord = true;
    } else if (f.type == fx::kArp) {
      arp = f.value;
    }
  }
  if (shift != 0) {
    if (in->kind == model::InstrumentKind::Sample && in->sampler.play_mode == kPlayBeatSlice) {
      ++stats_.fx_ignored;
    } else {
      note = static_cast<std::uint8_t>(clamp_i32(static_cast<std::int32_t>(note) + shift, 0, 127));
    }
  }
  if (!have_chord || (chord == 0u && arp == 0u)) {
    if (arp != 0u) {
      ++stats_.fx_ignored;  // an Arp needs a Chord FX in the other column
    }
    return note;
  }
  // The tones: the chord's mask, root first and ascending. The arp visits the offsets 0..11 (or all twelve for Chord 0) and clamps its note at 127 (min(base + offset, 127)),
  // so it loses no tone near the top; the chord (MIDI, block) skips the tones of 128 and up (section 4.1).
  std::uint32_t mask = (arp != 0u ? arp_mask(chord) : chord_mask(chord)) & 0x7FFFu;
  if (arp == 0u && 127u - note < 14u) {
    mask &= (2u << (127u - note)) - 1u;
  }
  const std::uint32_t n = static_cast<std::uint32_t>(__builtin_popcount(mask));  // bit 0 of every mask is the root
  if (n < 2u) {
    ++stats_.fx_ignored;  // a chord with only its root left (never an arp: every arp mask holds two tones below 12, tests/seq_pfx_test.cpp)
    return note;
  }
  if (arp != 0u) {
    k.arp_on = true;
    x.arp_dir = static_cast<std::uint8_t>(arp_dir(arp));
    k.arp_period = arp_microsteps(arp);
    x.arp_base = note;
    x.arp_mask = static_cast<std::uint16_t>(mask);
    x.arp_pos = x.arp_dir == static_cast<std::uint8_t>(ArpDir::Down) ? std::uint8_t{11} : std::uint8_t{0};
    return arp_note(x);
  }
  if (is_midi_slot(slot)) {
    x.chord_n = static_cast<std::uint8_t>(n - 1u);  // n <= 4: no chord mask holds more than three tones above the root (tests/seq_pfx_test.cpp checks the table)
    for (std::uint32_t i = 1; i < n; ++i) {
      x.chord[i - 1u] = static_cast<std::uint8_t>(nth_offset(mask, i));
    }
  } else {
    ++stats_.fx_ignored;  // no voice for the other notes
  }
  return note;
}

// The arpeggio's next note (OS-PFX): the walk from the position (arp_pick); the note before it ends as the new one starts.
void Sequencer::arp_next(std::uint32_t t, std::uint64_t at) noexcept {
  TrackState& k = tr_[t];
  PfxState& x = pfx_[t];
  k.note_open = true;
  k.note_timer = 0;
  send_note_on(t, arp_note(x), k.sent_slot, k.sent_vel, true, at);
}

// The note of the next pick: min(base + offset, 127). Random draws r in 0..10 (the upper bound 11 is excluded).
std::uint8_t Sequencer::arp_note(PfxState& x) noexcept {
  const ArpDir dir = static_cast<ArpDir>(x.arp_dir);
  const std::uint32_t r = dir == ArpDir::Random ? static_cast<std::uint32_t>(rng_.range(0, 11)) : 0u;
  const std::uint32_t note = x.arp_base + arp_pick(x.arp_mask, dir, x.arp_pos, r);
  return static_cast<std::uint8_t>(note > 127u ? 127u : note);
}

void Sequencer::roll_repeat(std::uint32_t t, std::uint64_t at) noexcept {
  TrackState& k = tr_[t];
  std::uint8_t note = k.sent_note;
  switch (roll_dir(k.roll_val)) {
    case RollDir::NoteUp:
      note = note < 127u ? static_cast<std::uint8_t>(note + 1u) : std::uint8_t{127};
      break;
    case RollDir::NoteDown:
      note = note > 0u ? static_cast<std::uint8_t>(note - 1u) : std::uint8_t{0};
      break;
    case RollDir::NoteRandom:
      note = static_cast<std::uint8_t>(clamp_i32(rng_.range(static_cast<std::int32_t>(note) - 12, static_cast<std::int32_t>(note) + 13), 0, 127));
      break;
    default:
      break;
  }
  k.note_open = true;
  k.note_timer = 0;
  k.note_len = static_cast<std::uint16_t>(roll_microsteps(roll_period(k.roll_val)) / 2u);
  std::uint8_t vel = k.sent_vel;
  if (roll_dir(k.roll_val) == RollDir::VolUp || roll_dir(k.roll_val) == RollDir::VolDown) {
    vel = roll_velocity(k);
  }
  send_note_on(t, note, k.sent_slot, vel, true, at);
}

void Sequencer::play_microstep(std::uint32_t t, std::uint32_t u, std::uint64_t at) noexcept {
  TrackState& k = tr_[t];
  if (u != 1u && !k.step_open && !k.note_open && !k.delayed && !k.midi_on) {
    return;  // OS-CPU10 B2: nothing is due (no counter to run, no delayed start, no roll, arpeggio or stutter without an open step, no MIDI note whose off is owed); force_roll is false after a microstep
  }
  if (u == 1u) {
    read_row(t);
  }
  const bool muted = midi_silenced(t);

  // The counters: an open step counts, a note ends when its timer reaches its length, a MIDI note also when the track is muted (PAR-063).
  if (k.step_open) {
    if (++k.step_timer >= kStepOpenLimit) {
      k.step_open = false;
      k.roll_on = false;
      k.arp_on = false;
    }
  }
  if (k.note_open) {
    ++k.note_timer;
    if (k.note_timer >= k.note_len || (muted && k.sent_midi)) {
      send_note_off(t, at);
      k.note_open = false;
    }
  }
  if (muted && k.midi_on) {
    send_note_off(t, at);  // a MIDI note that is still sounding ends with the mute (above); the track itself goes on: rows, FX and sample notes are not a mute's business (OS-MUTE)
  }

  if (u == 1u) {
    pre_fx(t, at);
    if (switch_release_ && (release_only_ == 0u || k.pattern_id == release_only_)) {
      switch_release(t, at);  // OS-PATSWITCH: the first step after a landing (OS-BUG-PASTEHANG: or after a paste over the pattern that plays, on its tracks)
    }
  }

  bool start = false;
  if (note_kind(k.row.note) != NoteKind::Empty && !k.cancel) {
    if (!k.delayed && u == 1u) {
      start = true;
    } else if (k.delayed && u == k.start_at) {
      start = true;
    }
  }
  if (start) {
    start_step(t, at);
  }

  // Continue the note: the performance stutter retriggers the last note; a Roll repeats it.
  if (k.perf_stutter != 0u) {
    const std::uint32_t period = stutter_microsteps(k.perf_stutter);
    if (k.step_open && k.sent_valid && period != 0u && (k.step_timer % period) == 1u) {
      k.note_open = true;
      k.note_timer = 0;
      send_note_on(t, k.sent_note, k.sent_slot, k.sent_vel, true, at);
    }
  } else if (k.step_open && (k.roll_on || k.arp_on)) {  // one test for an idle track, as before OS-PFX
    if (k.arp_on) {
      // OS-PFX: the arpeggio's next note is exactly arp_period microsteps after the previous one, counted from the note's start (step_timer is 0 there); it wins over a Roll
      if (k.step_timer != 0u && (k.step_timer % k.arp_period) == 0u) {
        arp_next(t, at);
      }
    } else {
      const std::uint32_t ms = roll_microsteps(roll_period(k.roll_val));
      if (ms != 0u && (k.step_timer % ms) == 1u && (k.step_timer != 1u || k.force_roll) && k.sent_valid) {
        roll_repeat(t, at);
      }
    }
  }
  k.force_roll = false;
}

}  // namespace o3os::seq
