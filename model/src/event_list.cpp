// SPDX-License-Identifier: GPL-3.0-or-later
// OS-MIDIREC S1 + S2a: the implementation of model/event_list.h — the pure projection of a pattern's steps as
// the EVENTS list's rows (S1: A2-A8, A15) and the edit planners (S2a: 3.5, A6, A9, A10, A19-A21), one shared
// write-plan path for all of them. Read-only: nothing here writes a step; the planners answer with writes the
// action applies through the Editor.

#include <o3os/model/event_list.h>
#include <o3os/model/fx_table.h>

#include <cstring>

namespace o3os::model::events {

namespace {

// The Micro-move offset of a step (0..46). The first column that holds one wins: the sequencer applies the second
// column's pre-effects first, so the first column's value is the one in force (Sequencer::pre_fx).
std::uint8_t micro_offset(const Step& s) noexcept {
  for (unsigned c = 0; c < 2u; ++c) {
    if (s.fx[c].type == kFxMicroMove) {
      return static_cast<std::uint8_t>(micromove_start(s.fx[c].value) - 1u);
    }
  }
  return 0;
}

// The Gate FX value of a step, or -1 when it holds none (the same column rule as the Micro-move).
int gate_value(const Step& s) noexcept {
  for (unsigned c = 0; c < 2u; ++c) {
    if (s.fx[c].type == kFxGate) {
      return s.fx[c].value;
    }
  }
  return -1;
}

// A track's length, held to 1..kMaxPatternLength (a loaded pattern may hold anything, PAR-003).
std::uint8_t track_len(const Pattern& pat, std::uint32_t t) noexcept {
  const std::uint32_t len = pat.track_length[t];
  return static_cast<std::uint8_t>(len > kMaxPatternLength ? kMaxPatternLength : (len < kMinPatternLength ? kMinPatternLength : len));
}

// The instrument a step plays (A8, F5): its own, else the track's last above it inside the track, else none (0:
// unknown at the top, drawn '?').
std::uint8_t resolve_instr(const Pattern& pat, std::uint32_t t, std::uint32_t s, bool& inherited) noexcept {
  if (pat.steps[t][s].instr != 0u) {
    inherited = false;
    return pat.steps[t][s].instr;
  }
  for (std::uint32_t u = s; u-- > 0u;) {
    if (pat.steps[t][u].instr != 0u) {
      inherited = true;
      return pat.steps[t][u].instr;
    }
  }
  inherited = false;
  return 0;
}

// An end at (t, s) is the end of the note above it (A4, A5) when the first occupied note column above holds a note;
// an end above it ended that note already, so this one is a row of its own.
bool folded_end(const Pattern& pat, std::uint32_t t, std::uint32_t s) noexcept {
  for (std::uint32_t u = s; u-- > 0u;) {
    const std::uint8_t n = pat.steps[t][u].note;
    if (note_occupies(n)) {
      return note_is_note(n);
    }
  }
  return false;
}

// The packed sort value of one row of the step being gathered: microstep, kind, track, and FX column 2 before
// column 1 (A3). Ascending order of the packed value is the row order within one step.
constexpr std::uint16_t pack(std::uint8_t microstep, Kind kind, std::uint8_t track, std::uint8_t column) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(microstep) << 8) | (static_cast<std::uint16_t>(kind) << 5) |
                                    (static_cast<std::uint16_t>(track) << 1) | static_cast<std::uint16_t>(1u - column));
}
constexpr Kind p_kind(std::uint16_t v) noexcept { return static_cast<Kind>((v >> 5) & 7u); }
constexpr std::uint8_t p_track(std::uint16_t v) noexcept { return static_cast<std::uint8_t>((v >> 1) & 15u); }
constexpr std::uint8_t p_column(std::uint16_t v) noexcept { return static_cast<std::uint8_t>(1u - (v & 1u)); }

}  // namespace

Scope scope_track(const Pattern& pat, std::uint32_t track) noexcept {
  const std::uint32_t t = track < kMaxTracks ? track : kMaxTracks - 1u;
  Scope s;
  s.track0 = s.track1 = static_cast<std::uint8_t>(t);
  s.row0 = 0;
  s.row1 = static_cast<std::uint8_t>(track_len(pat, t) - 1u);
  return s;
}

Scope scope_block(std::int16_t row0, std::int16_t row1, std::int8_t track0, std::int8_t track1) noexcept {
  const auto row = [](std::int16_t r) noexcept {
    return static_cast<std::uint8_t>(r < 0 ? 0 : (r > static_cast<std::int16_t>(kMaxPatternLength - 1u) ? static_cast<std::int16_t>(kMaxPatternLength - 1u) : r));
  };
  const auto trk = [](std::int8_t t) noexcept {
    return static_cast<std::uint8_t>(t < 0 ? 0 : (t > static_cast<std::int8_t>(kMaxTracks - 1u) ? static_cast<std::int8_t>(kMaxTracks - 1u) : t));
  };
  Scope s;
  s.row0 = row(row0);
  s.row1 = row(row1);
  if (s.row0 > s.row1) {
    const std::uint8_t r = s.row0;
    s.row0 = s.row1;
    s.row1 = r;
  }
  s.track0 = trk(track0);
  s.track1 = trk(track1);
  if (s.track0 > s.track1) {
    const std::uint8_t t = s.track0;
    s.track0 = s.track1;
    s.track1 = t;
  }
  return s;
}

const Instrument* project_instrument(void* ctx, std::uint32_t slot) noexcept {
  return static_cast<const Project*>(ctx)->instrument(slot);
}

CcOut cc_resolve(const Env& env, const Instrument* in, std::uint8_t letter) noexcept {
  CcOut out;
  if (in != nullptr && in->kind == InstrumentKind::Midi && letter < kMidiCcSlotsPerInstrument) {
    const std::uint8_t stored = in->midi_cc[letter];
    if (stored != kMidiCcConfig && stored <= kMidiCcMax) {  // the same mapping as seq::cc_target
      if (stored <= kMidiCcNumberMax) {
        out.what = CcOut::What::Controller;
        out.number = static_cast<std::uint8_t>(stored - 1u);
      } else if (stored == kMidiCcProg) {
        out.what = CcOut::What::Program;
      } else if (stored == kMidiCcChanAt) {
        out.what = CcOut::What::ChannelAt;
      } else {
        out.what = CcOut::What::PolyAt;
      }
      out.channel = in->midi_channel < 1u ? 1u : (in->midi_channel > 16u ? 16u : in->midi_channel);
      return out;
    }
    // else the Config value (0, or past kMidiCcMax), as seq::cc_target reads it
  }
  // The Config path (F3): the project's number for the slot, on the device's notes channel; an unassigned number
  // sends nothing. Any instrument that is not MIDI takes this path too (Sequencer::emit_cc).
  const std::uint8_t n = env.midi != nullptr && letter < kMidiCcSlots ? env.midi->cc_number[letter] : kMidiCcUnassigned;
  if (n == kMidiCcUnassigned || n > 127u) {
    out.what = CcOut::What::Unassigned;
    out.number = 0;
  } else {
    out.what = CcOut::What::Controller;
    out.number = n;
  }
  const std::uint8_t notes_ch = env.midi != nullptr ? env.midi->notes_channel : 0u;
  out.channel = static_cast<std::uint8_t>(notes_ch + 1u);  // 0..15 on the wire -> 1..16
  return out;
}

namespace {

// S6: a lane event's row kind (a note is a NOTE row, every other kind a CC row) and its percent of a step, exact to a tick.
constexpr Kind lane_row_kind(const LaneEvent& e) noexcept { return (e.kind_channel >> 4) == kLaneNote ? Kind::Note : Kind::Cc; }
constexpr std::uint8_t tick_percent(std::uint32_t ticks) noexcept {
  return static_cast<std::uint8_t>((ticks * 100u + 24u) / 48u);  // ticks 0..47 -> 0..98
}
// The ticks of a track a list of this scope shows: up to the scope's last row, never past the track's length.
std::uint32_t lane_hi(const Pattern& pat, const Scope& sc, std::uint32_t t) noexcept {
  const std::uint32_t last = static_cast<std::uint32_t>(sc.row1) + 1u;
  const std::uint32_t len = track_len(pat, t);
  return (last < len ? last : len) * kLaneTicksPerStep;
}

}  // namespace

Projector::Projector(const Pattern& pat, Env env, Scope scope, Filter filter) noexcept
    : pat_(&pat), env_(env), scope_(scope), filter_(filter), step_(scope.row0) {
  lane_init();
}

void Projector::reset() noexcept {
  step_ = scope_.row0;
  nkeys_ = 0;
  emit_ = 0;
  cells_ = 0;
  index_ = 0;
  started_ = false;
  done_ = false;
  lane_init();
}

// Every scope track's lane cursor goes to the first event of the scope's first row (the lane is sorted by tick).
void Projector::lane_init() noexcept {
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    lane_pos_[t] = 0;
  }
  if (env_.lanes == nullptr) {
    return;
  }
  for (std::uint32_t t = scope_.track0; t <= scope_.track1; ++t) {
    lane_pos_[t] = static_cast<std::uint16_t>(
        LaneStore::lower_bound(env_.lanes->lane(pat_->id, t), static_cast<std::uint32_t>(scope_.row0) * kLaneTicksPerStep));
  }
}

bool Projector::lane_head(std::uint8_t& track, std::uint32_t& tick) noexcept {
  if (env_.lanes == nullptr) {
    return false;
  }
  bool found = false;
  for (std::uint32_t t = scope_.track0; t <= scope_.track1; ++t) {
    const LaneStore::View v = env_.lanes->lane(pat_->id, t);
    const std::uint32_t hi = lane_hi(*pat_, scope_, t);
    std::uint32_t pos = lane_pos_[t];
    while (pos < v.count && v.events[pos].tick < hi && !filter_keeps(filter_, lane_row_kind(v.events[pos]))) {
      ++pos;
    }
    lane_pos_[t] = static_cast<std::uint16_t>(pos);
    if (pos >= v.count || v.events[pos].tick >= hi) {
      continue;
    }
    if (!found || v.events[pos].tick < tick) {  // strictly earlier: a tie keeps the lower track
      found = true;
      tick = v.events[pos].tick;
      track = static_cast<std::uint8_t>(t);
    }
  }
  return found;
}

void Projector::lane_build(std::uint8_t track, Row& out) noexcept {
  const std::uint32_t pos = lane_pos_[track];
  const LaneEvent& e = env_.lanes->lane(pat_->id, track).events[pos];
  const std::uint8_t kind = static_cast<std::uint8_t>(e.kind_channel >> 4);
  const std::uint32_t off = e.tick % kLaneTicksPerStep;
  out = Row{};
  out.key = Key{track, static_cast<std::uint8_t>(e.tick / kLaneTicksPerStep), lane_row_kind(e), 0, static_cast<std::uint16_t>(pos + 1u)};
  out.microstep = static_cast<std::uint8_t>(off);
  out.percent = tick_percent(off);
  out.midi = true;
  out.channel = static_cast<std::uint8_t>((e.kind_channel & 15u) + 1u);
  out.lane_kind = kind;
  out.lane_port = e.port;
  out.lane_a = e.a;
  out.lane_b = e.b;
  out.lane_len = e.len;
  if (kind == kLaneNote) {
    out.vol = e.b;
    out.len_steps = static_cast<std::uint16_t>(e.len / kLaneTicksPerStep);
    out.len_percent = tick_percent(e.len % kLaneTicksPerStep);
  }
  lane_pos_[track] = static_cast<std::uint16_t>(pos + 1u);
}

// The time (tick) of the next step row the filter keeps, gathering the next step when this one is spent.
bool Projector::step_head(std::uint32_t& time) noexcept {
  for (;;) {
    while (emit_ < nkeys_) {
      const std::uint16_t v = keys_[emit_];
      if (filter_keeps(filter_, p_kind(v))) {
        time = static_cast<std::uint32_t>(step_) * kLaneTicksPerStep + (v >> 8);
        return true;
      }
      ++emit_;
    }
    if (done_ || !gather()) {
      return false;
    }
  }
}

bool Projector::gather() noexcept {
  nkeys_ = 0;
  emit_ = 0;
  for (std::uint32_t s = started_ ? static_cast<std::uint32_t>(step_) + 1u : scope_.row0; s <= scope_.row1; ++s) {
    std::uint8_t n = 0;
    for (std::uint32_t t = scope_.track0; t <= scope_.track1; ++t) {
      if (s >= track_len(*pat_, t)) {
        continue;  // past the track's length: kept in the pattern, not played, not listed
      }
      const Step& st = pat_->steps[t][s];
      cells_ += 3;  // the note cell and the two FX cells, read once
      if (note_is_note(st.note)) {
        keys_[n++] = pack(micro_offset(st), Kind::Note, static_cast<std::uint8_t>(t), 0);
      } else if (note_is_end(st.note) && !folded_end(*pat_, t, s)) {
        keys_[n++] = pack(micro_offset(st), end_kind_of(st.note), static_cast<std::uint8_t>(t), 0);
      }
      for (unsigned c = 0; c < 2u; ++c) {
        if (is_cc_type(st.fx[c].type)) {
          keys_[n++] = pack(0, Kind::Cc, static_cast<std::uint8_t>(t), static_cast<std::uint8_t>(c));
        }
      }
    }
    if (n != 0u) {
      for (std::uint8_t i = 1; i < n; ++i) {  // insertion sort, ascending: the packed value is the A3 order
        const std::uint16_t v = keys_[i];
        std::uint8_t j = i;
        while (j > 0u && keys_[j - 1u] > v) {
          keys_[j] = keys_[j - 1u];
          --j;
        }
        keys_[j] = v;
      }
      step_ = static_cast<std::uint8_t>(s);
      nkeys_ = n;
      started_ = true;
      return true;
    }
  }
  done_ = true;
  return false;
}

void Projector::build(std::uint16_t packed, Row& out) const noexcept {
  const std::uint8_t t = p_track(packed);
  const std::uint8_t s = step_;
  const Kind kind = p_kind(packed);
  const std::uint8_t column = p_column(packed);
  const Step& st = pat_->steps[t][s];

  out = Row{};
  out.key = Key{t, s, kind, column};
  const std::uint8_t off = kind == Kind::Cc ? 0u : micro_offset(st);  // a CC goes out at the step's start (F6)
  out.microstep = off;
  out.percent = percent_of(off);

  bool inherited = false;
  const std::uint8_t instr = resolve_instr(*pat_, t, s, inherited);
  out.instr = instr;
  out.inherited = inherited;
  const Instrument* in = instr != 0u ? env_.instrument_of(static_cast<std::uint32_t>(instr - 1u)) : nullptr;
  out.midi = in != nullptr && in->kind == InstrumentKind::Midi;
  out.channel = 0;
  if (kind == Kind::Cc) {
    out.cc_letter = cc_letter_of(st.fx[column].type);
    out.cc = cc_resolve(env_, in, out.cc_letter);
    out.channel = out.cc.channel;  // the CH column of a CC row: the channel it goes out on (F3)
  } else if (out.midi) {
    out.channel = in->midi_channel < 1u ? 1u : (in->midi_channel > 16u ? 16u : in->midi_channel);
  }
  out.vol = st.vol;

  if (kind == Kind::Note) {
    // LEN (A5): the first occupied note column below ends the note; none: it plays on past the track's end.
    const std::uint8_t len = track_len(*pat_, t);
    long total;
    const int gate = gate_value(st);
    if (gate >= 0) {  // a Gate FX shows the gated length, gate percent of one step (F8)
      out.gated = true;
      total = static_cast<long>(gate) * 48L / 100L;
    } else {
      std::uint32_t end = len;  // one past the last step
      for (std::uint32_t u = s + 1u; u < len; ++u) {
        if (note_occupies(pat_->steps[t][u].note)) {
          end = u;
          break;
        }
      }
      const std::uint8_t end_off = end < len ? micro_offset(pat_->steps[t][end]) : 0u;
      out.len_runs_on = end >= len;
      total = static_cast<long>(end - s) * 48L + static_cast<long>(end_off) - static_cast<long>(off);
    }
    if (total < 0L) {
      total = 0;
    }
    out.len_steps = static_cast<std::uint16_t>(total / 48L);
    out.len_percent = percent_of(static_cast<std::uint8_t>(total % 48L));
  }
}

// The merge of the step rows (gathered a step at a time, sorted) and the lane rows (one cursor a track): by time, a step row
// first at one tick, then a lane row (the lowest track, then lane order).
bool Projector::next(Row& out) noexcept {
  std::uint32_t step_time = 0;
  const bool has_step = step_head(step_time);
  std::uint8_t lane_track = 0;
  std::uint32_t lane_tick = 0;
  if (lane_head(lane_track, lane_tick) && (!has_step || lane_tick < step_time)) {
    lane_build(lane_track, out);
    ++index_;
    return true;
  }
  if (!has_step) {
    return false;
  }
  build(keys_[emit_++], out);
  ++index_;
  return true;
}

std::uint32_t count_rows(const Pattern& pat, Env env, Scope scope, Filter filter) noexcept {
  Projector p(pat, env, scope, filter);
  Row r;
  std::uint32_t n = 0;
  while (p.next(r)) {
    ++n;
  }
  return n;
}

std::uint32_t window(const Pattern& pat, Env env, Scope scope, Filter filter, std::uint32_t from, Row* out, std::uint32_t cap,
                     std::uint32_t* total) noexcept {
  Projector p(pat, env, scope, filter);
  Row r;
  std::uint32_t i = 0;
  std::uint32_t n = 0;
  while (p.next(r)) {
    if (i >= from && n < cap) {
      out[n++] = r;
    }
    ++i;
  }
  if (total != nullptr) {
    *total = i;
  }
  return n;
}

bool key_of_index(const Pattern& pat, Env env, Scope scope, Filter filter, std::uint32_t index, Key& out) noexcept {
  Projector p(pat, env, scope, filter);
  Row r;
  std::uint32_t i = 0;
  while (p.next(r)) {
    if (i == index) {
      out = r.key;
      return true;
    }
    ++i;
  }
  return false;
}

std::int32_t index_of_key(const Pattern& pat, Env env, Scope scope, Filter filter, const Key& key) noexcept {
  Projector p(pat, env, scope, filter);
  Row r;
  std::uint32_t i = 0;
  while (p.next(r)) {
    if (r.key == key) {
      return static_cast<std::int32_t>(i);
    }
    ++i;
  }
  return -1;
}

// ---- S2a: the edit planners, one shared write-plan path (3.5, A6, A9, A10, A19-A21) ----

namespace {

constexpr int kUsteps = 48;  // microsteps a step (A1)

int floor_div(int a, int b) noexcept {
  const int q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// The first occupied note column at or below `from` on track t, within the track's length; len when none.
std::uint32_t first_occupied_below(const Pattern& pat, std::uint32_t t, std::uint32_t from, std::uint32_t len) noexcept {
  for (std::uint32_t u = from; u < len; ++u) {
    if (note_occupies(pat.steps[t][u].note)) {
      return u;
    }
  }
  return len;
}

// The first note (not an end) at or below `from`; len when none.
std::uint32_t first_note_below(const Pattern& pat, std::uint32_t t, std::uint32_t from, std::uint32_t len) noexcept {
  for (std::uint32_t u = from; u < len; ++u) {
    if (note_is_note(pat.steps[t][u].note)) {
      return u;
    }
  }
  return len;
}

// The Micro-move column of a step cleared (A6: an offset of 0 removes it).
void clear_micro(Step& s) noexcept {
  for (unsigned c = 0; c < 2u; ++c) {
    if (s.fx[c].type == kFxMicroMove) {
      s.fx[c] = StepFx{kFxNone, 0};
    }
  }
}

// An end row's owned bytes cleared (A4, A10): the note column and its Micro-move; the rest of the step stays.
void clear_end(Step& s) noexcept {
  s.note = kNoteEmpty;
  clear_micro(s);
}

// A NOTE row's owned bytes cleared (A4, A10, A21's move source): note, instrument, volume and every FX column
// that is not a CC FX; the CC columns stay on their step.
void clear_note_owned(Step& s) noexcept {
  s.note = kNoteEmpty;
  s.instr = 0;
  s.vol = kVolumeUnset;
  for (unsigned c = 0; c < 2u; ++c) {
    if (!is_cc_type(s.fx[c].type)) {
      s.fx[c] = StepFx{kFxNone, 0};
    }
  }
}

// A6: the Micro-move of `offset` into the step. An existing Micro-move keeps its column; a new one goes to
// FX 2 when it is free, else to FX 1; offset 0 removes it (m02 00 is never written) — every Micro-move column,
// so the note really is at .00. False: no free column.
bool micro_set(Step& s, std::uint8_t offset) noexcept {
  int col = -1;
  for (unsigned c = 0; c < 2u; ++c) {
    if (s.fx[c].type == kFxMicroMove) {
      col = static_cast<int>(c);
      break;
    }
  }
  if (offset == 0) {
    for (unsigned c = 0; c < 2u; ++c) {
      if (s.fx[c].type == kFxMicroMove) {
        s.fx[c] = StepFx{kFxNone, 0};
      }
    }
    return true;
  }
  if (col < 0) {
    if (s.fx[1].type == kFxNone) {
      col = 1;
    } else if (s.fx[0].type == kFxNone) {
      col = 0;
    } else {
      return false;
    }
  }
  s.fx[col] = StepFx{kFxMicroMove, static_cast<std::uint8_t>(micro_value(static_cast<int>(offset)))};
  return true;
}

// A21's move: the FX a NOTE row carries into the target step. Each carried FX keeps its column when that is
// free at the target, else takes the other one; the Micro-move carries the new offset (A6's offset 0 removes
// it); a Micro-move the source did not have follows A6's placement. False: a carried FX has no free column.
bool carry_note_fx(const Step& from, Step& to, std::uint8_t off) noexcept {
  bool has_micro = false;
  for (unsigned c = 0; c < 2u; ++c) {
    if (from.fx[c].type == kFxMicroMove) {
      has_micro = true;
    }
  }
  bool done[2] = {false, false};
  for (unsigned pass = 0; pass < 2u; ++pass) {
    for (unsigned c = 0; c < 2u; ++c) {
      if (done[c] || from.fx[c].type == kFxNone || is_cc_type(from.fx[c].type)) {
        continue;
      }
      if (from.fx[c].type == kFxMicroMove && off == 0) {  // removed, not carried
        done[c] = true;
        continue;
      }
      const unsigned at = pass == 0u ? c : (1u - c);  // keep the column first, then the other one
      if (to.fx[at].type == kFxNone) {
        to.fx[at] = from.fx[c];
        if (from.fx[c].type == kFxMicroMove) {
          to.fx[at].value = static_cast<std::uint8_t>(micro_value(static_cast<int>(off)));
        }
        done[c] = true;
      }
    }
  }
  for (unsigned c = 0; c < 2u; ++c) {
    if (!done[c] && from.fx[c].type != kFxNone && !is_cc_type(from.fx[c].type) &&
        !(from.fx[c].type == kFxMicroMove && off == 0)) {
      return false;
    }
  }
  if (!has_micro && off > 0) {
    return micro_set(to, off);
  }
  return true;
}

// n digits of v (clamped to n digits), most significant first, then the NUL.
void put_digits(char* p, unsigned v, unsigned n) noexcept {
  unsigned m = 1;
  for (unsigned i = 1; i < n; ++i) {
    m *= 10u;
  }
  if (v >= m) {
    v = m - 1u;
  }
  for (unsigned i = n; i-- > 0u;) {
    p[i] = static_cast<char>('0' + (v % 10u));
    v /= 10u;
  }
  p[n] = '\0';
}

// The one write-plan path every planner shares: reads through the pattern, adds at most kMaxPlanWrites step
// writes (a write that changes nothing is not planned), or a refusal / the one-voice notice in text.
class Builder {
 public:
  Builder(const Pattern& pat, Env env, Scope scope) noexcept : pat_(pat), env_(env), scope_(scope) {}

  const Pattern& pat() const noexcept { return pat_; }
  const Env& env() const noexcept { return env_; }
  Scope scope() const noexcept { return scope_; }
  std::uint8_t len_of(std::uint8_t t) const noexcept { return track_len(pat_, t); }
  const Step& at(std::uint8_t t, std::uint8_t s) const noexcept { return pat_.steps[t][s]; }
  // The step as the planned writes make it (a no-op write is not planned, so the pattern's step is it).
  const Step& step_after(std::uint8_t t, std::uint8_t s) const noexcept {
    for (unsigned i = 0; i < n_; ++i) {
      if (plan_.writes[i].track == t && plan_.writes[i].step == s) {
        return plan_.writes[i].value;
      }
    }
    return pat_.steps[t][s];
  }
  // A2: a step the list shows — the scope's track and rows, the rows clamped to the track's length.
  bool in_scope(std::uint8_t t, std::uint8_t s) const noexcept {
    if (t < scope_.track0 || t > scope_.track1) {
      return false;
    }
    const std::uint32_t top = scope_.row1 < (len_of(t) - 1u) ? scope_.row1 : (len_of(t) - 1u);
    return s >= scope_.row0 && s <= top;
  }

  void set(std::uint8_t t, std::uint8_t s, const Step& v) noexcept {
    for (unsigned i = 0; i < n_; ++i) {
      if (plan_.writes[i].track == t && plan_.writes[i].step == s) {
        plan_.writes[i].value = v;  // one write per step: the last set wins (a moved end may land on the vacated source)
        return;
      }
    }
    if (n_ >= kMaxPlanWrites) {
      return;  // cannot happen: every planner writes at most four steps
    }
    if (std::memcmp(&pat_.steps[t][s], &v, sizeof(Step)) == 0) {
      return;  // a no-op write is not planned
    }
    plan_.writes[n_].track = t;
    plan_.writes[n_].step = s;
    plan_.writes[n_].value = v;
    ++n_;
    plan_.n = static_cast<std::uint8_t>(n_);
  }

  void refuse(const char* text) noexcept {
    unsigned i = 0;
    for (; i + 1u < Plan::kTextMax && text[i] != '\0'; ++i) {
      plan_.text[i] = text[i];
    }
    plan_.text[i] = '\0';
    plan_.n = 0;
    n_ = 0;
  }
  void refuse_micro() noexcept { refuse("NO FREE FX COLUMN FOR THE MICRO-MOVE"); }
  void refuse_no_free_step() noexcept { refuse("NO FREE STEP AFTER THIS NOTE"); }
  void refuse_no_free_step_in_scope() noexcept { refuse("NO FREE STEP IN THE SCOPE"); }
  void refuse_gate() noexcept { refuse("THE GATE FX SETS THIS LENGTH"); }
  void refuse_outside_scope() noexcept { refuse("OUTSIDE THE SCOPE"); }
  void refuse_step_holds(unsigned step1) noexcept {  // step1: 1-based
    std::memcpy(plan_.text, "STEP ", 5);
    put_digits(plan_.text + 5, step1, 3);
    std::memcpy(plan_.text + 8, " HOLDS A NOTE", 14);
    plan_.n = 0;
    n_ = 0;
  }
  void refuse_step_no_fx(unsigned step1) noexcept {
    std::memcpy(plan_.text, "STEP ", 5);
    put_digits(plan_.text + 5, step1, 3);
    std::memcpy(plan_.text + 8, ": NO FREE FX COLUMN", 20);
    plan_.n = 0;
    n_ = 0;
  }
  void refuse_ch(unsigned ch) noexcept {  // 1..16
    std::memcpy(plan_.text, "NO MIDI INSTRUMENT ON CH ", 25);
    put_digits(plan_.text + 25, ch, 2);
    plan_.n = 0;
    n_ = 0;
  }
  // A11's one-voice notice: the writes stand and the strip says where the note above ends. The first notice
  // is kept when several rows end a note.
  void notice_ends_note(unsigned step1, std::uint8_t percent) noexcept {
    if (plan_.ends_note) {
      return;
    }
    plan_.ends_note = true;
    std::memcpy(plan_.text, "ENDS THE NOTE AT ", 17);
    put_digits(plan_.text + 17, step1, 3);
    plan_.text[20] = '.';
    put_digits(plan_.text + 21, percent, 2);
  }

  Plan& out() noexcept { return plan_; }

 private:
  const Pattern& pat_;
  Env env_;
  Scope scope_;
  Plan plan_{};
  unsigned n_ = 0;
};

// The key still names a row of its kind (3.2); a stale key plans nothing.
bool key_names_row(Builder& b, const Key& k) noexcept {
  const Step& st = b.at(k.track, k.step);
  return (k.kind == Kind::Note && note_is_note(st.note)) || (k.kind == Kind::Cc && is_cc_type(st.fx[k.column].type)) ||
         (k.kind == Kind::Off && st.note == kNoteOff) || (k.kind == Kind::Cut && st.note == kNoteCut) ||
         (k.kind == Kind::Fade && st.note == kNoteFade);
}

// The microstep arithmetic of a TIME edit (A1): the absolute microstep of (step, offset) moved by delta; the
// unreachable 47 wraps: crossing .96 goes to the next step at .00, crossing .00 down to .96 of the previous.
void time_target(std::uint8_t s, std::uint8_t off, int delta, std::uint8_t& ns, std::uint8_t& noff) noexcept {
  const int total = static_cast<int>(s) * kUsteps + static_cast<int>(off) + delta;
  int st = floor_div(total, kUsteps);
  int r = total - st * kUsteps;
  if (r == kUsteps - 1) {
    if (delta > 0) {
      ++st;
      r = 0;
    } else {
      r = kUsteps - 2;  // .96
    }
  }
  if (st < 0) {
    st = -1;
  }
  if (st > 255) {
    st = 255;
  }
  ns = static_cast<std::uint8_t>(st);
  noff = static_cast<std::uint8_t>(r < 0 ? 0 : r);
}

// A11, F8: a row placed at (t, at) where a note sounds ends that note there — the strip says so. The scan
// reads the pattern as the planned writes make it; `skip` is the placed row's own new step, when the notice
// is about an end placed past it.
void ends_note_notice(Builder& b, std::uint8_t t, std::uint8_t at, std::uint8_t skip, std::uint8_t noff) noexcept {
  for (std::uint32_t u = at; u-- > 0u;) {
    if (u == skip) {
      continue;
    }
    const std::uint8_t n = b.step_after(t, static_cast<std::uint8_t>(u)).note;
    if (note_occupies(n)) {
      if (note_is_note(n)) {
        b.notice_ends_note(static_cast<unsigned>(at) + 1u, percent_of(noff));
      }
      return;
    }
  }
}

// The first free FX column of a step: 0, else 1, else 2 (none).
unsigned free_fx_column(const Step& s) noexcept {
  if (s.fx[0].type == kFxNone) {
    return 0;
  }
  if (s.fx[1].type == kFxNone) {
    return 1;
  }
  return 2;
}

Plan plan_time(Builder& b, const Request& req) noexcept {
  const Key& k = req.key;
  const std::uint8_t t = k.track;
  const std::uint8_t s = k.step;
  const Step& src = b.at(t, s);
  if (k.kind == Kind::Cc) {
    // A21: whole steps only (F6). Keeps its column when that is free, else takes the other one, else refused.
    const int ns = static_cast<int>(s) + req.delta * (req.coarse ? 4 : 1);
    if (ns == static_cast<int>(s)) {
      return b.out();  // nothing moved
    }
    if (ns < 0 || ns > 255 || !b.in_scope(t, static_cast<std::uint8_t>(ns))) {
      b.refuse_outside_scope();
      return b.out();
    }
    const std::uint8_t nst = static_cast<std::uint8_t>(ns);
    const Step& dst = b.at(t, nst);
    unsigned use = 2;
    if (dst.fx[k.column].type == kFxNone) {
      use = k.column;
    } else if (dst.fx[1u - k.column].type == kFxNone) {
      use = 1u - k.column;
    } else {
      b.refuse_step_no_fx(static_cast<unsigned>(nst) + 1u);
      return b.out();
    }
    Step a = src;
    a.fx[k.column] = StepFx{kFxNone, 0};
    Step c = dst;
    c.fx[use] = src.fx[k.column];
    b.set(t, s, a);
    b.set(t, nst, c);
    return b.out();
  }
  // A NOTE or an end row: one microstep through the Micro-move (A6); crossing .96 / .00 moves the step (A21).
  const int delta = req.coarse ? req.delta * kUsteps : req.delta;
  std::uint8_t ns = 0, noff = 0;
  time_target(s, micro_offset(src), delta, ns, noff);
  if (ns == s && noff == micro_offset(src)) {
    return b.out();  // nothing moved
  }
  if (!b.in_scope(t, ns)) {
    b.refuse_outside_scope();
    return b.out();
  }
  if (ns == s) {
    // a microstep edit inside the step: only the Micro-move column changes (A6)
    Step m = src;
    if (!micro_set(m, noff)) {
      b.refuse_micro();
      return b.out();
    }
    b.set(t, s, m);
    return b.out();
  }
  if (note_occupies(b.at(t, ns).note)) {
    b.refuse_step_holds(static_cast<unsigned>(ns) + 1u);
    return b.out();
  }
  if (k.kind == Kind::Note) {
    // A21: the note moves with its owned bytes and its LEN; its CC rows stay on their step.
    const std::uint8_t len = b.len_of(t);
    Step dst = b.at(t, ns);
    dst.note = src.note;
    dst.instr = src.instr;
    dst.vol = src.vol;
    clear_micro(dst);  // the note lands at (ns, noff): a Micro-move the target step had of its own must not delay it
    if (!carry_note_fx(src, dst, noff)) {
      b.refuse_step_no_fx(static_cast<unsigned>(ns) + 1u);
      return b.out();
    }
    Step moved = src;
    clear_note_owned(moved);
    if (folded_end(b.pat(), t, s)) {
      moved.note = kNoteOff;  // it was the end of the note above: an OFF keeps that note's LEN (I2, as A10's Delete)
    }
    const std::uint32_t e = first_occupied_below(b.pat(), t, static_cast<std::uint32_t>(s) + 1u, len);
    const bool has_end = e < len && note_is_end(b.at(t, static_cast<std::uint8_t>(e)).note);
    const int ksteps = static_cast<int>(ns) - static_cast<int>(s);
    Step old_end;
    bool clear_old_end = false;
    std::uint8_t end_note = 0, end_micro = 0;
    if (has_end) {
      end_note = b.at(t, static_cast<std::uint8_t>(e)).note;
      end_micro = micro_offset(b.at(t, static_cast<std::uint8_t>(e)));
      old_end = b.at(t, static_cast<std::uint8_t>(e));
      clear_end(old_end);
      clear_old_end = true;
    }
    // The writes first, so the end's landing step is read as the plan makes it: when the note moves back onto
    // its own end's step (e2 == s), the new end is built from the vacated source, not from the uncleared pattern.
    b.set(t, s, moved);
    b.set(t, ns, dst);
    if (clear_old_end) {
      b.set(t, static_cast<std::uint8_t>(e), old_end);
    }
    std::uint8_t e2 = 0;
    bool write_end = false;
    if (has_end && ksteps != 0) {
      const long e2l = static_cast<long>(e) + ksteps;
      if (e2l >= 0 && e2l < static_cast<long>(len)) {
        e2 = static_cast<std::uint8_t>(e2l);
        const Step& landing = b.step_after(t, e2);  // the vacated source when e2 == s
        if (e2 != s && note_is_end(landing.note)) {
          b.refuse_step_holds(static_cast<unsigned>(e2) + 1u);  // an orphan end row is there (A4): the end may not replace it
          return b.out();
        }
        if (!note_is_note(landing.note)) {
          Step new_end = landing;
          new_end.note = end_note;  // an end keeps its kind (A20)
          if (end_micro > 0) {
            if (!micro_set(new_end, end_micro)) {  // the end's Micro-move goes with it (A20)
              b.refuse_step_no_fx(static_cast<unsigned>(e2) + 1u);
              return b.out();
            }
          } else {
            clear_micro(new_end);
          }
          b.set(t, e2, new_end);
          write_end = true;
        }
      }
    }
    ends_note_notice(b, t, ns, 255u, noff);
    if (write_end) {
      // the end placed inside another note's span ends that note there (A11, F8)
      ends_note_notice(b, t, e2, ns, end_micro);
    }
    return b.out();
  }
  // An end row: as a NOTE, without LEN (3.5) — its note column and its Micro-move move.
  Step dst = b.at(t, ns);
  dst.note = src.note;
  if (noff > 0) {
    if (!micro_set(dst, noff)) {
      b.refuse_step_no_fx(static_cast<unsigned>(ns) + 1u);
      return b.out();
    }
  } else {
    clear_micro(dst);
  }
  Step moved = src;
  moved.note = kNoteEmpty;
  clear_micro(moved);
  b.set(t, s, moved);
  b.set(t, ns, dst);
  ends_note_notice(b, t, ns, 255u, noff);
  return b.out();
}

Plan plan_type(Builder& b, const Request& req) noexcept {
  const Key& k = req.key;
  if (k.kind != Kind::Cc) {
    return b.out();  // '-' on the other rows (3.5)
  }
  const Step& src = b.at(k.track, k.step);
  int nl = (static_cast<int>(cc_letter_of(src.fx[k.column].type)) + req.delta) % 6;
  if (nl < 0) {
    nl += 6;
  }
  Step s = src;
  s.fx[k.column].type = nl == 5 ? kCcF : static_cast<std::uint8_t>(kCcFirst + nl);  // the value stays
  b.set(k.track, k.step, s);
  return b.out();
}

Plan plan_ch(Builder& b, const Request& req) noexcept {
  const Key& k = req.key;
  if (k.kind != Kind::Note) {
    return b.out();  // read-only on a CC row, '-' on an end row (3.5)
  }
  const std::uint8_t t = k.track, s = k.step;
  const Step& src = b.at(t, s);
  bool inherited = false;
  const std::uint8_t instr = resolve_instr(b.pat(), t, s, inherited);
  const Instrument* in = instr != 0u ? b.env().instrument_of(static_cast<std::uint32_t>(instr - 1u)) : nullptr;
  const int cur = (in != nullptr && in->kind == InstrumentKind::Midi) ? static_cast<int>(in->midi_channel) : 0;
  const int nch = ((cur + req.delta - 1) % 16 + 16) % 16 + 1;  // 1..16
  // Q1 b: the step's own instrument when it is the MIDI one on that channel, else the lowest slot that has it.
  std::uint8_t pick = 0;
  if (in != nullptr && in->kind == InstrumentKind::Midi && static_cast<int>(in->midi_channel) == nch) {
    pick = instr;
  } else {
    for (std::uint32_t slot = 0; slot < kMaxInstruments; ++slot) {
      const Instrument* o = b.env().instrument_of(slot);
      if (o != nullptr && o->kind == InstrumentKind::Midi && static_cast<int>(o->midi_channel) == nch) {
        pick = static_cast<std::uint8_t>(slot + 1u);
        break;
      }
    }
  }
  if (pick == 0) {
    b.refuse_ch(static_cast<unsigned>(nch));
    return b.out();
  }
  Step st = src;
  st.instr = pick;
  b.set(t, s, st);
  return b.out();
}

Plan plan_data(Builder& b, const Request& req) noexcept {
  const Key& k = req.key;
  if (k.kind != Kind::Note) {
    return b.out();  // read-only on a CC row, '-' on an end row (3.5)
  }
  const edit::Rule rule{true, true};  // the list's row is one step
  Step s = b.at(k.track, k.step);
  edit::Last l = b.out().last;
  if (req.absolute) {
    edit::note_set(s, req.value, rule, l);  // a pad: the note the app resolved through the entry
  } else {
    edit::note_delta(s, req.delta * (req.coarse ? 12 : 1), rule, l);
  }
  b.out().last = l;
  b.set(k.track, k.step, s);
  return b.out();
}

Plan plan_val(Builder& b, const Request& req) noexcept {
  const Key& k = req.key;
  if (k.kind != Kind::Note && k.kind != Kind::Cc) {
    return b.out();  // '-' on an end row (3.5)
  }
  const edit::Rule rule{true, true};
  Step s = b.at(k.track, k.step);
  edit::Last l = b.out().last;
  if (k.kind == Kind::Note) {
    if (req.pad) {
      edit::volume_set(s, static_cast<unsigned>(edit::map_int(static_cast<int>(req.value), 0, 47, 0, kVolumeMax)), rule);
    } else if (req.absolute) {
      edit::volume_set(s, req.value, rule);
    } else {
      edit::volume_delta(s, req.delta * (req.coarse ? 10 : 1), rule);
    }
  } else {
    if (req.pad) {
      edit::fx_pad(s, k.column, req.value, rule, l);  // the 0..127 ramp over the CC's range
    } else if (req.absolute) {
      edit::fx_value_set(s, k.column, static_cast<int>(req.value), rule, l);
    } else {
      edit::fx_value_delta(s, k.column, req.delta * (req.coarse ? 10 : 1), rule, l);
    }
  }
  b.out().last = l;
  b.set(k.track, k.step, s);
  return b.out();
}

// OS-EVFX: the encoder on a NOTE row's FX1 or FX2 is the grid's FX value walk on that slot (an empty slot takes the last used
// type, a coarse turn is a nibble); the type is not edited here.
Plan plan_fx(Builder& b, const Request& req, unsigned slot) noexcept {
  const Key& k = req.key;
  if (k.kind != Kind::Note) {
    return b.out();  // '-' on the other rows
  }
  const edit::Rule rule{true, true};
  Step s = b.at(k.track, k.step);
  edit::Last l = b.out().last;
  if (s.fx[slot].type == 0 && req.delta != 0) {
    // an empty slot repeats the last used type: none to repeat, or a CC type (which a NOTE row's FX slot does not carry), is refused
    const int last_type = fx::type_of_picker(l.fx);
    if (last_type == 0) {
      b.refuse("NO FX TO REPEAT");
      return b.out();
    }
    if (is_cc_type(static_cast<std::uint8_t>(last_type))) {
      b.refuse("A CC FX NEEDS A CC ROW");
      return b.out();
    }
  }
  edit::fx_value_delta(s, slot, req.delta * (req.coarse ? 16 : 1), rule, l);
  b.out().last = l;
  b.set(k.track, k.step, s);
  return b.out();
}

Plan plan_len(Builder& b, const Request& req) noexcept {
  const Key& k = req.key;
  if (k.kind != Kind::Note) {
    return b.out();  // '-' on the other rows (3.5)
  }
  const std::uint8_t t = k.track, s = k.step;
  const Step& src = b.at(t, s);
  if (gate_value(src) >= 0) {
    b.refuse_gate();  // A19
    return b.out();
  }
  const std::uint8_t len = b.len_of(t);
  const int d = req.delta * (req.coarse ? 4 : 1);
  const std::uint32_t e = first_occupied_below(b.pat(), t, static_cast<std::uint32_t>(s) + 1u, len);
  long e1 = static_cast<long>(e) + d;
  if (e1 < static_cast<long>(s) + 1) {
    e1 = static_cast<long>(s) + 1;  // the shortest LEN is the note's next step (A20)
  }
  const std::uint32_t next_note = first_note_below(b.pat(), t, static_cast<std::uint32_t>(s) + 1u, len);
  const std::uint32_t top = next_note < len ? next_note : len;  // the next note, or the track's end
  const bool has_end = e < len && note_is_end(b.at(t, static_cast<std::uint8_t>(e)).note);
  if (e1 >= static_cast<long>(top)) {
    // moving past the next note (or the track's end) removes the end: the note runs to the next note (A20)
    if (has_end) {
      Step c = b.at(t, static_cast<std::uint8_t>(e));
      clear_end(c);
      b.set(t, static_cast<std::uint8_t>(e), c);
    }
    return b.out();
  }
  const std::uint8_t e1s = static_cast<std::uint8_t>(e1);
  if (e1s != e && note_is_end(b.at(t, e1s).note)) {
    b.refuse_step_holds(static_cast<unsigned>(e1s) + 1u);  // an orphan end row is there (A4): the end may not replace it
    return b.out();
  }
  const std::uint8_t kind_note = has_end ? b.at(t, static_cast<std::uint8_t>(e)).note : kNoteOff;  // a new end is an OFF
  const std::uint8_t micro = has_end ? micro_offset(b.at(t, static_cast<std::uint8_t>(e))) : 0;
  Step ne = b.at(t, e1s);
  ne.note = kind_note;
  if (micro > 0) {
    if (!micro_set(ne, micro)) {  // when an end leaves a step, its Micro-move goes with it (A20)
      b.refuse_micro();
      return b.out();
    }
  } else {
    clear_micro(ne);
  }
  if (has_end && e1s != e) {
    Step c = b.at(t, static_cast<std::uint8_t>(e));
    clear_end(c);
    b.set(t, static_cast<std::uint8_t>(e), c);
  }
  b.set(t, e1s, ne);
  return b.out();
}

Plan plan_in(Builder& b, const Request& req) noexcept {
  const Key& k = req.key;
  if (k.kind != Kind::Note) {
    return b.out();  // read-only on a CC row, '-' on an end row (3.5)
  }
  const edit::Rule rule{true, true};
  Step s = b.at(k.track, k.step);
  edit::Last l = b.out().last;
  edit::instrument_delta(s, req.delta * (req.coarse ? 8 : 1), rule, l, req.coarse ? 64 : 5);  // the grid's Instr walk
  b.out().last = l;
  b.set(k.track, k.step, s);
  return b.out();
}

// A9: a CC row in the first free FX column of (t, s) — `fx` is what the column gets (CC A 000, or the copied
// letter and value). False, with the column message, when both columns are taken; the caller then plans nothing.
bool put_cc_row(Builder& b, std::uint8_t t, std::uint8_t s, const StepFx& fx) noexcept {
  const unsigned col = free_fx_column(b.at(t, s));
  if (col == 2u) {
    b.refuse_step_no_fx(static_cast<unsigned>(s) + 1u);
    return false;
  }
  Step st = b.at(t, s);
  st.fx[col] = fx;
  b.set(t, s, st);
  b.out().new_key = Key{t, s, Kind::Cc, static_cast<std::uint8_t>(col)};
  b.out().has_new_key = true;
  return true;
}

// A9: a NOTE at the default note (as the pads enter them) on (t, s), then the one-voice notice.
void put_note_row(Builder& b, const Request& req, std::uint8_t t, std::uint8_t s) noexcept {
  const Step& src = b.at(t, s);
  const edit::Rule rule{true, true};
  edit::Last l = b.out().last;
  Step st = src;
  edit::note_set(st, req.absolute ? req.value : edit::kDefaultNote + 1u, rule, l);  // as the pads enter them
  b.out().last = l;
  b.set(t, s, st);
  b.out().new_key = Key{t, s, Kind::Note, 0};
  b.out().has_new_key = true;
  ends_note_notice(b, t, s, 255u, micro_offset(src));
}

Plan plan_insert(Builder& b, const Request& req) noexcept {
  const Scope sc = b.scope();
  std::uint8_t t0 = req.cursor_track;
  if (t0 < sc.track0) {
    t0 = sc.track0;
  }
  if (t0 > sc.track1) {
    t0 = sc.track1;
  }
  if (!req.has_key) {
    // An empty list: a NOTE (Shift + Insert: a CC A) at the scope's first step on the grid cursor's track (A9)
    const std::uint8_t len = b.len_of(t0);
    const std::uint8_t s = sc.row0 < len ? sc.row0 : static_cast<std::uint8_t>(len - 1u);
    if (!req.shift) {
      put_note_row(b, req, t0, s);
    } else {
      put_cc_row(b, t0, s, StepFx{kCcFirst, 0});  // CC A 000
    }
    return b.out();
  }
  const Key& k = req.key;
  if (!key_names_row(b, k)) {
    return b.out();  // a stale key plans nothing
  }
  const std::uint8_t t = k.track, s = k.step;
  const Step& src = b.at(t, s);
  if (k.kind == Kind::Note) {
    if (!req.shift) {
      // A copy of the note (note, instrument, volume; no Micro-move and no other FX) on the row's end step (A9)
      const std::uint8_t len = b.len_of(t);
      const std::uint32_t e = first_occupied_below(b.pat(), t, static_cast<std::uint32_t>(s) + 1u, len);
      if (e >= len || note_is_note(b.at(t, static_cast<std::uint8_t>(e)).note) || e > sc.row1) {
        b.refuse_no_free_step();  // runs into the next note, or the end lies outside the scope
        return b.out();
      }
      Step c = b.at(t, static_cast<std::uint8_t>(e));
      c.note = src.note;
      c.instr = src.instr;
      c.vol = src.vol;
      for (unsigned col = 0; col < 2u; ++col) {
        if (!is_cc_type(c.fx[col].type)) {
          c.fx[col] = StepFx{kFxNone, 0};
        }
      }
      b.set(t, static_cast<std::uint8_t>(e), c);
      b.out().new_key = Key{t, static_cast<std::uint8_t>(e), Kind::Note, 0};
      b.out().has_new_key = true;
      return b.out();
    }
    // Shift + Insert: a CC A 000 in the first free FX column of the row's step (A9)
    put_cc_row(b, t, s, StepFx{kCcFirst, 0});
    return b.out();
  }
  if (k.kind == Kind::Cc) {
    if (!req.shift) {
      // The same letter and value on the next step of the scope that has a free FX column (A9)
      const std::uint8_t len = b.len_of(t);
      const std::uint32_t top = sc.row1 < (len - 1u) ? sc.row1 : (len - 1u);
      for (std::uint32_t u = static_cast<std::uint32_t>(s) + 1u; u <= top; ++u) {
        const std::uint8_t us = static_cast<std::uint8_t>(u);
        if (free_fx_column(b.at(t, us)) != 2u) {
          put_cc_row(b, t, us, src.fx[k.column]);
          return b.out();
        }
      }
      b.refuse_no_free_step_in_scope();
      return b.out();
    }
    // Shift + Insert: a NOTE on the row's step when its note column is empty (A9)
    if (note_occupies(src.note)) {
      b.refuse_step_holds(static_cast<unsigned>(s) + 1u);
      return b.out();
    }
    put_note_row(b, req, t, s);
    return b.out();
  }
  return b.out();  // an end row: Insert is not defined there (A9)
}

Plan plan_delete(Builder& b, const Request& req) noexcept {
  if (!req.has_key || !key_names_row(b, req.key)) {
    return b.out();
  }
  const Key& k = req.key;
  const std::uint8_t t = k.track, s = k.step;
  const Step& src = b.at(t, s);
  if (k.kind == Kind::Note) {
    // Its owned bytes are cleared, end included (A10, A4)
    Step d = src;
    clear_note_owned(d);
    if (folded_end(b.pat(), t, s)) {
      d.note = kNoteOff;  // it was the end of the note above: an OFF keeps that note's LEN (I2)
    }
    b.set(t, s, d);
    const std::uint8_t len = b.len_of(t);
    const std::uint32_t e = first_occupied_below(b.pat(), t, static_cast<std::uint32_t>(s) + 1u, len);
    if (e < len && note_is_end(b.at(t, static_cast<std::uint8_t>(e)).note)) {
      Step c = b.at(t, static_cast<std::uint8_t>(e));
      clear_end(c);
      b.set(t, static_cast<std::uint8_t>(e), c);
    }
    return b.out();
  }
  if (k.kind == Kind::Cc) {
    Step d = src;  // its FX column is cleared (A10)
    d.fx[k.column] = StepFx{kFxNone, 0};
    b.set(t, s, d);
    return b.out();
  }
  Step d = src;  // an end row: its note column and Micro-move are cleared (A10)
  clear_end(d);
  b.set(t, s, d);
  return b.out();
}

// S3: one row's QUANT (an offset under kQuantHalf goes back to .00, else forward to the next step at .00), through the TIME
// planner, so the move rule, the end's move and the refusals are A21's. The rows the rule does not touch are refusals with
// their own text, nothing written.
Plan plan_quant(Builder& b, const Request& req) noexcept {
  if (!req.has_key || !key_names_row(b, req.key)) {
    return b.out();
  }
  const Key& k = req.key;
  if (k.kind == Kind::Cc) {
    b.refuse("CC IS ON THE STEP");  // a CC has no sub-step time (F6)
    return b.out();
  }
  const std::uint8_t off = micro_offset(b.at(k.track, k.step));
  if (off == 0u) {
    b.refuse("ALREADY ON THE STEP");
    return b.out();
  }
  Request r = req;
  r.op = Op::Edit;
  r.field = Field::Time;
  r.coarse = false;
  r.delta = off < kQuantHalf ? -static_cast<int>(off) : static_cast<int>(kUsteps) - static_cast<int>(off);
  return plan_time(b, r);
}

Plan plan_field(Builder& b, const Request& req) noexcept {
  if (!req.has_key || !key_names_row(b, req.key)) {
    return b.out();  // a stale key plans nothing
  }
  switch (req.field) {
    case Field::Time:
      return plan_time(b, req);
    case Field::Type:
      return plan_type(b, req);
    case Field::Ch:
      return plan_ch(b, req);
    case Field::Data:
      return plan_data(b, req);
    case Field::Val:
      return plan_val(b, req);
    case Field::Len:
      return plan_len(b, req);
    case Field::In:
      return plan_in(b, req);
    case Field::Fx1:
    case Field::Fx2:
      return plan_fx(b, req, req.field == Field::Fx1 ? 0u : 1u);
  }
  return b.out();
}

// ---- S6: the lane rows (OS-MIDIREC-P1). Every plan is one LaneWrite over the ticks it touches. ----

// Copies the lane events of the ticks [t0, t1) into the plan's lane write, leaving `room` free slots; false (the refusal set) when
// they do not fit. `first` gets the lane index of the first one: the write's event i is the lane's event first + i.
bool lane_range(Builder& b, const LaneStore::View& v, std::uint8_t track, std::uint32_t t0, std::uint32_t t1, unsigned room,
                std::uint32_t& first) noexcept {
  first = LaneStore::lower_bound(v, t0);
  const std::uint32_t end = LaneStore::lower_bound(v, t1);
  if (end - first + room > kMaxLaneWriteEvents) {
    b.refuse("TOO MANY EVENTS HERE");
    return false;
  }
  LaneWrite& w = b.out().lane;
  w.on = true;
  w.track = track;
  w.t0 = static_cast<std::uint16_t>(t0);
  w.t1 = static_cast<std::uint16_t>(t1);
  w.n = static_cast<std::uint8_t>(end - first);
  std::memcpy(w.events, v.events + first, (end - first) * sizeof(LaneEvent));
  return true;
}

// Puts `e` after the last event at or before its tick (ties keep the insertion order); returns its place in the write.
unsigned lane_put(LaneWrite& w, const LaneEvent& e) noexcept {
  unsigned q = w.n;
  while (q > 0u && w.events[q - 1u].tick > e.tick) {
    --q;
  }
  std::memmove(w.events + q + 1u, w.events + q, (w.n - q) * sizeof(LaneEvent));
  w.events[q] = e;
  ++w.n;
  return q;
}

void lane_drop(LaneWrite& w, unsigned p) noexcept {
  std::memmove(w.events + p, w.events + p + 1u, (w.n - p - 1u) * sizeof(LaneEvent));
  --w.n;
}

// The key of the lane row at lane index `i` (0-based) of the event `e`.
Key lane_key(std::uint8_t track, const LaneEvent& e, std::uint32_t i) noexcept {
  return Key{track, static_cast<std::uint8_t>(e.tick / kLaneTicksPerStep), lane_row_kind(e), 0, static_cast<std::uint16_t>(i + 1u)};
}

// The value an edit gives a lane event's data byte or bytes, per kind: DATA is `a` on a note, a CC, a program and a poly
// pressure's note; VAL is `b` on a note, a CC and a poly pressure, `a` on a channel pressure, 14 bits b << 7 | a on a bend.
bool lane_data_edit(LaneEvent& e, const Request& req) noexcept {
  const std::uint8_t kind = static_cast<std::uint8_t>(e.kind_channel >> 4);
  if (kind == kLaneChanAt || kind == kLaneBend) {
    return false;
  }
  int a = e.a;
  if (kind == kLaneNote && req.absolute) {
    a = static_cast<int>(req.value) - 1;  // the model note byte 1..128 -> 0..127
  } else {
    a += req.delta * (req.coarse ? (kind == kLaneNote ? 12 : 10) : 1);
  }
  e.a = static_cast<std::uint8_t>(a < 0 ? 0 : (a > 127 ? 127 : a));
  return true;
}

bool lane_val_edit(LaneEvent& e, const Request& req) noexcept {
  const std::uint8_t kind = static_cast<std::uint8_t>(e.kind_channel >> 4);
  if (kind == kLaneProgram) {
    return false;
  }
  if (kind == kLaneBend) {
    const int v = static_cast<int>((static_cast<unsigned>(e.b) << 7) | e.a);
    int n = v + req.delta * (req.coarse ? 128 : 1);
    n = n < 0 ? 0 : (n > static_cast<int>(kLaneBendMax) ? static_cast<int>(kLaneBendMax) : n);
    e.a = static_cast<std::uint8_t>(n & 127);
    e.b = static_cast<std::uint8_t>(n >> 7);
    return true;
  }
  std::uint8_t& byte = kind == kLaneChanAt ? e.a : e.b;
  int n = byte;
  if (req.pad) {
    n = edit::map_int(static_cast<int>(req.value), 0, 47, 0, 127);
  } else if (req.absolute) {
    n = static_cast<int>(req.value);
  } else {
    n += req.delta * (req.coarse ? 10 : 1);
  }
  const int lo = kind == kLaneNote ? 1 : 0;  // a velocity of 0 would be a note-off
  byte = static_cast<std::uint8_t>(n < lo ? lo : (n > 127 ? 127 : n));
  return true;
}

void plan_lane_row(Builder& b, const Request& req) noexcept {
  const Key& k = req.key;
  const std::uint8_t track = k.track;
  const LaneStore::View v = b.env().lanes != nullptr ? b.env().lanes->lane(b.pat().id, track) : LaneStore::View{nullptr, 0};
  const std::uint32_t idx = static_cast<std::uint32_t>(k.lane) - 1u;
  if (idx >= v.count || v.events[idx].tick / kLaneTicksPerStep != k.step) {
    return;  // a stale key plans nothing
  }
  LaneEvent e = v.events[idx];
  const std::uint32_t tick = e.tick;
  const std::uint8_t kind = static_cast<std::uint8_t>(e.kind_channel >> 4);
  const std::uint32_t hi = lane_hi(b.pat(), b.scope(), track);  // one past the last tick the list shows on this track
  std::uint32_t first = 0;
  if (req.op == Op::Quant) {
    b.refuse("NO QUANT ON A LANE ROW");
    return;
  }
  if (req.op == Op::Delete) {
    if (lane_range(b, v, track, tick, tick + 1u, 0, first)) {
      lane_drop(b.out().lane, idx - first);
    }
    return;
  }
  if (req.op == Op::Insert) {
    // a copy of the event: the row's end for a note, the next step for the other kinds, the same tick with Shift (A9's rule)
    const std::uint32_t at = req.shift ? tick : (kind == kLaneNote ? tick + e.len : tick + kLaneTicksPerStep);
    if (at >= hi) {
      b.refuse(kind == kLaneNote ? "NO ROOM AFTER THIS NOTE" : "OUTSIDE THE SCOPE");
      return;
    }
    if (lane_range(b, v, track, at, at + 1u, 1, first)) {
      LaneEvent c = e;
      c.tick = static_cast<std::uint16_t>(at);
      b.out().new_key = lane_key(track, c, first + lane_put(b.out().lane, c));
      b.out().has_new_key = true;
    }
    return;
  }
  // an edit of one field
  if (req.field == Field::Time) {
    const long d = static_cast<long>(req.delta) * (req.coarse ? kLaneTicksPerStep : 1);
    const long nt = static_cast<long>(tick) + d;
    if (d == 0) {
      return;
    }
    const std::uint32_t lo = static_cast<std::uint32_t>(b.scope().row0) * kLaneTicksPerStep;
    if (nt < static_cast<long>(lo) || nt >= static_cast<long>(hi)) {
      b.refuse_outside_scope();
      return;
    }
    const std::uint32_t t0 = nt < static_cast<long>(tick) ? static_cast<std::uint32_t>(nt) : tick;
    const std::uint32_t t1 = (nt < static_cast<long>(tick) ? tick : static_cast<std::uint32_t>(nt)) + 1u;
    if (lane_range(b, v, track, t0, t1, 0, first)) {
      lane_drop(b.out().lane, idx - first);
      e.tick = static_cast<std::uint16_t>(nt);
      b.out().new_key = lane_key(track, e, first + lane_put(b.out().lane, e));
      b.out().has_new_key = true;
    }
    return;
  }
  const LaneEvent old = e;
  switch (req.field) {
    case Field::Ch:
      e.kind_channel = static_cast<std::uint8_t>((e.kind_channel & 0xF0u) | static_cast<unsigned>(((e.kind_channel & 15) + req.delta) % 16 + 16) % 16u);
      break;
    case Field::Data:
      if (!lane_data_edit(e, req)) {
        return;
      }
      break;
    case Field::Val:
      if (!lane_val_edit(e, req)) {
        return;
      }
      break;
    case Field::Len: {
      if (kind != kLaneNote) {
        return;
      }
      long n = static_cast<long>(e.len) + static_cast<long>(req.delta) * (req.coarse ? kLaneTicksPerStep : 1);
      n = n < 1 ? 1 : (n > static_cast<long>(kLaneTicks) ? static_cast<long>(kLaneTicks) : n);
      e.len = static_cast<std::uint16_t>(n);
      break;
    }
    default:
      return;  // TYPE, IN and the FX slots: '-' on a lane row
  }
  if (std::memcmp(&e, &old, sizeof e) == 0) {
    return;  // nothing changed
  }
  if (lane_range(b, v, track, tick, tick + 1u, 0, first)) {
    b.out().lane.events[idx - first] = e;
  }
}

Plan plan_lane(Builder& b, const Request& req) noexcept {
  plan_lane_row(b, req);
  Plan& p = b.out();
  if (p.text[0] != '\0') {
    p.lane.on = false;  // a refusal writes nothing
  }
  return p;
}

}  // namespace

Plan plan(const Pattern& pat, Env env, Scope scope, const edit::Last& last, const Request& req) noexcept {
  Builder b(pat, env, scope);
  b.out().last = last;
  Request r = req;
  if (r.delta > 4096) {
    r.delta = 4096;
  } else if (r.delta < -4096) {
    r.delta = -4096;
  }
  if (r.has_key && r.key.lane != 0u) {
    return plan_lane(b, r);
  }
  switch (r.op) {
    case Op::Insert:
      return plan_insert(b, r);
    case Op::Delete:
      return plan_delete(b, r);
    case Op::Quant:
      return plan_quant(b, r);
    case Op::Edit:
    default:
      return plan_field(b, r);
  }
}

// ---- S3: QUANT of every NOTE and end row (spec S3) ----

namespace {

bool text_ends(const char* t, const char* tail) noexcept {
  const std::size_t n = std::strlen(t), m = std::strlen(tail);
  return n >= m && std::memcmp(t + n - m, tail, m) == 0;
}

void put_text(char* out, std::size_t cap, std::size_t& at, const char* s) noexcept {
  for (; *s != '\0' && at + 1u < cap; ++s) {
    out[at++] = *s;
  }
}

void put_number(char* out, std::size_t cap, std::size_t& at, std::uint32_t v) noexcept {
  char d[10];
  unsigned n = 0;
  do {
    d[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u && n < sizeof d);
  while (n > 0u && at + 1u < cap) {
    out[at++] = d[--n];
  }
}

}  // namespace

QuantResult quant_all(const Pattern& pat, Env env, Scope scope, Filter filter, const edit::Last& last, QuantApply apply, void* ctx) noexcept {
  QuantResult res;
  // A pass walks the steps from the last back, so a row that moves on frees its step (when its move leaves it empty) for the
  // row before it in the same pass — one pass, and a second only as a guard. Each row is planned against the pattern as the
  // rows before it left it: every row done is a row on its step, and the rows still refused are the ones counted as left.
  for (unsigned pass = 0; pass <= kMaxPatternLength; ++pass) {  // the cap is a guard, never reached
    res.left = 0;
    res.why = QuantWhy::None;
    std::uint32_t pass_done = 0;
    for (std::uint32_t s = static_cast<std::uint32_t>(scope.row1) + 1u; s-- > scope.row0;) {
      Scope one = scope;
      one.row0 = one.row1 = static_cast<std::uint8_t>(s);
      Projector p(pat, env, one, filter);
      Row row;
      while (p.next(row)) {
        if (row.key.kind == Kind::Cc || row.key.lane != 0u || row.microstep == 0u) {
          continue;  // a CC is on its step, a lane row is recorded time (S6), and a row on the step has nothing to quantise: none is counted
        }
        Request req;
        req.op = Op::Quant;
        req.key = row.key;
        const Plan pl = plan(pat, env, scope, last, req);
        if (pl.n == 0u) {
          ++res.left;
          if (res.why == QuantWhy::None) {
            res.why = text_ends(pl.text, "HOLDS A NOTE") ? QuantWhy::StepTaken
                     : (text_ends(pl.text, "OUTSIDE THE SCOPE") ? QuantWhy::Outside
                     : (text_ends(pl.text, "NO FREE FX COLUMN") ? QuantWhy::NoFxColumn : QuantWhy::Other));
          }
          continue;
        }
        if (!apply(ctx, pl)) {
          return res;  // the apply failed: stop, no half-applied move goes on
        }
        ++res.done;
        ++pass_done;
      }
    }
    if (pass_done == 0u || res.left == 0u) {
      return res;
    }
  }
  return res;
}

void quant_message(const QuantResult& r, char* out, std::size_t cap) noexcept {
  if (cap == 0u) {
    return;
  }
  std::size_t at = 0;
  if (r.done == 0u && r.left == 0u) {
    put_text(out, cap, at, "NOTHING TO QUANTISE");
  } else {
    put_text(out, cap, at, "QUANTISED ");
    put_number(out, cap, at, r.done);
    if (r.left != 0u) {
      put_text(out, cap, at, ", ");
      put_number(out, cap, at, r.left);
      put_text(out, cap, at, " LEFT (");
      put_text(out, cap, at, r.why == QuantWhy::StepTaken ? "STEP TAKEN"
                          : (r.why == QuantWhy::Outside ? "OUTSIDE SCOPE"
                          : (r.why == QuantWhy::NoFxColumn ? "NO FX COLUMN" : "REFUSED")));
      put_text(out, cap, at, ")");
    }
  }
  out[at] = '\0';
}

}  // namespace o3os::model::events
