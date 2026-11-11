// SPDX-License-Identifier: GPL-3.0-or-later
//
// The performance mode's slots (seq/perform.h).
#include <o3os/seq/perform.h>

#include <o3os/audio/events.h>

namespace o3os::seq {

namespace {

constexpr int clamp_int(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

constexpr int clamp_to(PerfFx fx, int v) noexcept {
  const PerfFxInfo& i = perf_info(fx);
  return clamp_int(v, i.min, i.max);
}

constexpr unsigned idx(PerfFx fx) noexcept { return static_cast<unsigned>(fx); }

// The type a filter FX stands for in the voices (voices.h kFilterLow, kFilterHigh, kFilterBand: 0, 1, 2).
constexpr unsigned filter_type(PerfFx fx) noexcept { return fx == PerfFx::LowPass ? 0u : (fx == PerfFx::HighPass ? 1u : 2u); }

}  // namespace

unsigned perf_value_text(PerfFx fx, int value, char (&out)[8]) noexcept {
  const PerfFxInfo& info = perf_info(fx);
  const char* text = nullptr;
  char digits[8];
  unsigned n = 0;
  if (info.labels != nullptr) {
    text = info.labels[clamp_int(value, 0, info.max)];
  } else if (value == 0) {
    text = "---";
  } else {
    unsigned v = value < 0 ? static_cast<unsigned>(-value) : static_cast<unsigned>(value);
    unsigned count = 0;
    do {
      digits[count++] = static_cast<char>('0' + v % 10u);
      v /= 10u;
    } while (v != 0u && count < 5u);
    if (value < 0) {
      out[n++] = '-';
    }
    while (count != 0u) {
      out[n++] = digits[--count];
    }
    out[n] = '\0';
    return n;
  }
  while (text[n] != '\0' && n < 7u) {
    out[n] = text[n];
    ++n;
  }
  out[n] = '\0';
  return n;
}

bool perf_command(unsigned track, PerfFx fx, int value, Command& out) noexcept {
  if (track >= kPerfTracks || fx == PerfFx::None || idx(fx) >= kPerfFxCount) {
    return false;
  }
  const std::uint8_t t = static_cast<std::uint8_t>(track);
  switch (fx) {
    case PerfFx::StepRepeater:
      out = Command{Command::Kind::PerfStutter, t, static_cast<std::uint16_t>(clamp_to(fx, value)), 0};
      return true;
    case PerfFx::PatternPlayMode:
      // The play mode goes with the track's source pattern and length in one command (PerfTrack); pattern 0 and length 0 leave both as the song has them.
      out = Command{Command::Kind::PerfTrack, t, 0, static_cast<std::uint32_t>(clamp_to(fx, value)) << 8};
      return true;
    case PerfFx::PatternLength:
      out = Command{Command::Kind::PerfPatternLen, 0, 0, perf_length_steps(clamp_to(fx, value))};
      return true;
    default:
      break;
  }
  const FxGroup g = perf_info(fx).group;
  if (g == FxGroup::None) {
    return false;  // (no row of the table reaches here but the sequencer's own FX, handled above)
  }
  std::uint32_t c;
  if (value == 0) {
    c = 1u << 16;  // the end
  } else {
    const int v = clamp_to(fx, value);
    const bool filter = fx == PerfFx::LowPass || fx == PerfFx::HighPass || fx == PerfFx::BandPass;
    c = static_cast<std::uint16_t>(filter ? audio::perf_filter_value(filter_type(fx), v) : static_cast<std::int16_t>(v));
  }
  out = Command{Command::Kind::PerfControl, t, static_cast<std::uint16_t>(g), c};
  return true;
}

void PerformBank::reset() noexcept {
  for (unsigned s = 0; s < kPerfSlots; ++s) {
    slots_[s].fx = static_cast<std::uint8_t>(s + 1u < kPerfFxCount ? s + 1u : 0u);  // the first twelve, in order
    slots_[s].row = 0;
    slots_[s].value[0] = 0;
    for (unsigned r = 1; r < kPerfRows; ++r) {
      slots_[s].value[r] = model::kPerfDefaultValues[slots_[s].fx][r - 1u];  // OS-PRFDEFAULTS: what init_perf_state gives
    }
    live_[s] = 0;
  }
  held_ = 0;
  cc_mask_ = 0;
  affected_ = 0;
  edit_ = false;
  edit_slot_ = 0;
  copy_ = Slot{0, 0, {0, 0, 0, 0}};
  copied_ = false;
  for (unsigned t = 0; t < kPerfTracks; ++t) {
    for (unsigned f = 0; f < kPerfFxCount; ++f) {
      sent_[t][f] = 0;
    }
  }
  sent_length_ = 0;
  for (DjLayer& d : dj_) {
    d = DjLayer{0, 0, 0};
  }
  for (PerfScene& sc : scenes_) {
    sc = PerfScene{};
  }
  latch_ = true;
  rate_ = 0;
  sent_rate_ = 0;
}

static_assert(kPerfSlots == model::kPerfSlots && kPerfRows == model::kPerfStoredRows + 1u && kPerfFxCount == model::kPerfFxMax + 1u, "model/project_state.h mirrors seq/perform.h");

model::PerfState PerformBank::state() const noexcept {
  model::PerfState s;
  for (unsigned i = 0; i < kPerfSlots; ++i) {
    s.fx[i] = slots_[i].fx;
    s.row[i] = slots_[i].row;
    for (unsigned r = 1; r < kPerfRows; ++r) {
      s.value[i][r - 1u] = slots_[i].value[r];
    }
  }
  return s;
}

void PerformBank::load_slots(const model::PerfState& s) noexcept {
  for (unsigned i = 0; i < kPerfSlots; ++i) {
    const PerfFx fx = s.fx[i] < kPerfFxCount ? static_cast<PerfFx>(s.fx[i]) : PerfFx::None;
    slots_[i].fx = static_cast<std::uint8_t>(fx);
    slots_[i].row = s.row[i] < kPerfRows ? s.row[i] : std::uint8_t{0};
    slots_[i].value[0] = 0;
    for (unsigned r = 1; r < kPerfRows; ++r) {
      slots_[i].value[r] = static_cast<std::int16_t>(clamp_to(fx, s.value[i][r - 1u]));
    }
    load_row(i);
  }
  held_ = 0;
  cc_mask_ = 0;  // OS-MIDICCMAP S3: a load ends every slot's CC mode
  affected_ = 0;
  edit_ = false;
  edit_slot_ = 0;
  for (PerfScene& sc : scenes_) {
    sc = PerfScene{};  // the scenes are the session's (runtime): another project starts with none
  }
}

model::ScenesState PerformBank::scenes_state() const noexcept {
  model::ScenesState s{};  // zeros: a scene that is not stored holds nothing
  s.latch = latch_ ? 1u : 0u;
  for (unsigned i = 0; i < kPerfScenes; ++i) {
    s.scene[i] = scenes_[i];
  }
  return s;
}

void PerformBank::load_scenes(const model::ScenesState& s) noexcept {
  for (unsigned i = 0; i < kPerfScenes; ++i) {
    scenes_[i] = s.scene[i];
  }
  latch_ = s.latch != 0u;
}

void PerformBank::load_row(unsigned slot) noexcept {
  const Slot& s = slots_[slot];
  live_[slot] = s.row == 0u ? std::int16_t{0} : static_cast<std::int16_t>(clamp_to(static_cast<PerfFx>(s.fx), s.value[s.row]));
}

void PerformBank::end_slot(unsigned slot) noexcept {
  held_ = static_cast<std::uint16_t>(held_ & ~(1u << slot));
  cc_mask_ = static_cast<std::uint16_t>(cc_mask_ & ~(1u << slot));  // OS-MIDICCMAP S3: an ended slot is not in CC mode
  slots_[slot].row = 0;
  live_[slot] = 0;
}

// A slot that has the same FX, or one this FX blocks, ends.
void PerformBank::exclude(unsigned slot) noexcept {
  const PerfFx fx = static_cast<PerfFx>(slots_[slot].fx);
  if (fx == PerfFx::None) {
    return;
  }
  const PerfFxInfo& info = perf_info(fx);
  for (unsigned s = 0; s < kPerfSlots; ++s) {
    if (s == slot) {
      continue;
    }
    const PerfFx other = static_cast<PerfFx>(slots_[s].fx);
    if (other == fx || (other != PerfFx::None && (other == info.blocks[0] || other == info.blocks[1]))) {
      end_slot(s);
    }
  }
}

int PerformBank::desired(unsigned track, unsigned fx) const noexcept {
  if (track < kPerfTracks) {  // OS-DJ B1: the DJ layer wins over the slots
    const DjLayer& d = dj_[track];
    const int v = fx == idx(PerfFx::StepRepeater) ? d.stutter : (fx == idx(PerfFx::DelaySend) ? d.delay : (fx == idx(PerfFx::Volume) ? d.gain : 0));
    if (v != 0) {
      return v;
    }
  }
  if (track >= kPerfTracks || ((affected_ >> track) & 1u) == 0u) {
    return 0;
  }
  for (unsigned s = 0; s < kPerfSlots; ++s) {
    if (slots_[s].fx == fx && live_[s] != 0 && (slots_[s].row != 0u || ((cc_mask_ >> s) & 1u) != 0u)) {  // a slot in CC mode wants its live value too
      return live_[s];
    }
  }
  return 0;
}

int PerformBank::play_mode(unsigned track) const noexcept {
  return desired(track, idx(PerfFx::PatternPlayMode));
}

bool PerformBank::set_dj_layer(unsigned track, int stutter, int delay, int gain) noexcept {
  if (track >= kPerfTracks) {
    return false;
  }
  const DjLayer to{static_cast<std::int8_t>(clamp_to(PerfFx::StepRepeater, stutter)), static_cast<std::int8_t>(clamp_to(PerfFx::DelaySend, delay)), static_cast<std::int8_t>(clamp_to(PerfFx::Volume, gain))};
  DjLayer& d = dj_[track];
  const bool changed = d.stutter != to.stutter || d.delay != to.delay || d.gain != to.gain;
  d = to;
  return changed;
}

bool PerformBank::pad_down(unsigned pad, CommandSink& out) noexcept {
  if (pad >= kPerfPads) {
    return false;
  }
  const unsigned slot = pad % kPerfSlots;
  cc_mask_ = static_cast<std::uint16_t>(cc_mask_ & ~(1u << slot));  // OS-MIDICCMAP S3: a pad of the slot ends its CC mode
  Slot& s = slots_[slot];
  s.row = s.fx == 0u ? std::uint8_t{0} : static_cast<std::uint8_t>(pad / kPerfSlots);  // a slot with no FX has no row
  held_ = static_cast<std::uint16_t>(held_ | (1u << slot));
  exclude(slot);
  load_row(slot);
  (void)apply(out);
  return true;
}

bool PerformBank::pad_up(unsigned pad, CommandSink& out) noexcept {
  if (pad >= kPerfPads) {
    return false;
  }
  const unsigned slot = pad % kPerfSlots;
  cc_mask_ = static_cast<std::uint16_t>(cc_mask_ & ~(1u << slot));  // OS-MIDICCMAP S3: the pad's release ends the slot's CC mode too
  const bool own_row = slots_[slot].row == pad / kPerfSlots;  // the pad's row is still the slot's selected one (another pad of the slot may have taken it over)
  // The row's stored value is reloaded and then the slot lets go: what the encoder made of the value while the pad was held does not outlive the pad,
  // the FX stays on at the stored value until another row of the slot is pressed.
  load_row(slot);
  held_ = static_cast<std::uint16_t>(held_ & ~(1u << slot));
  if (!latch_ && own_row) {
    end_slot(slot);  // LATCH off (OS-PERF2): the FX lasted as long as the pad
  }
  (void)apply(out);
  return true;
}

bool PerformBank::turn(int detents, CommandSink& out) noexcept {
  if (detents == 0) {
    return false;
  }
  bool changed = false;
  for (unsigned s = 0; s < kPerfSlots; ++s) {
    Slot& slot = slots_[s];
    // OS-MIDICCMAP S3: a held slot in CC mode is turned as for a row, also when its row is 0.
    if (((held_ >> s) & 1u) == 0u || slot.fx == 0u || (slot.row == 0u && ((cc_mask_ >> s) & 1u) == 0u)) {
      continue;
    }
    const PerfFx fx = static_cast<PerfFx>(slot.fx);
    const int next = clamp_to(fx, clamp_int(live_[s], -100000, 100000) + clamp_int(detents, -100000, 100000));
    if (next != live_[s]) {
      live_[s] = static_cast<std::int16_t>(next);
      changed = true;
    }
    if (edit_ && slot.row != 0u && slot.value[slot.row] != live_[s]) {
      slot.value[slot.row] = live_[s];
      changed = true;
    }
  }
  if (changed) {
    (void)apply(out);
  }
  return changed;
}

bool PerformBank::toggle_track(unsigned track, CommandSink& out) noexcept {
  if (track >= kPerfTracks) {
    return false;
  }
  affected_ = static_cast<std::uint16_t>(affected_ ^ (1u << track));
  (void)apply(out);
  return true;
}

void PerformBank::enter() noexcept {
  held_ = 0;
  for (unsigned s = 0; s < kPerfSlots; ++s) {
    load_row(s);
  }
}

bool PerformBank::turn_off(CommandSink& out) noexcept {
  affected_ = 0;
  held_ = 0;
  cc_mask_ = 0;  // OS-MIDICCMAP S3: leaving the screen ends every slot's CC mode
  return apply(out);
}

// OS-MIDICCMAP S3 (docs/specs/OS-MIDICCMAP.md 3.4): a mapped CC 51..62 on the slot. The slot enters CC mode (a runtime bit, never stored), its live
// value becomes the CC's place in its FX's range (the official law of F5, as app::cc_map_perf), the slots holding the same FX or one it blocks end
// (exclude, as F5) and apply() sends it to the affected tracks. The stored row and values are untouched. A slot with no FX ignores the message.
bool PerformBank::set_cc(unsigned slot, unsigned v, CommandSink& out) noexcept {
  if (slot >= kPerfSlots || v > 127u) {
    return false;
  }
  const PerfFx fx = static_cast<PerfFx>(slots_[slot].fx);
  if (fx == PerfFx::None) {
    return false;  // a slot with no FX ignores it
  }
  const PerfFxInfo& info = perf_info(fx);
  const int value = info.min + static_cast<int>(v) * (info.max - info.min) / 127;
  const bool was_cc = ((cc_mask_ >> slot) & 1u) != 0u;
  cc_mask_ = static_cast<std::uint16_t>(cc_mask_ | (1u << slot));
  exclude(slot);  // ends the other slots of the same FX (and their CC mode, through end_slot)
  const bool changed = !was_cc || live_[slot] != value;
  live_[slot] = static_cast<std::int16_t>(value);
  (void)apply(out);
  return changed;
}

bool PerformBank::move_edit(int delta) noexcept {
  const int next = clamp_int(static_cast<int>(edit_slot_) + delta, 0, static_cast<int>(kPerfSlots) - 1);
  const bool changed = next != edit_slot_;
  edit_slot_ = static_cast<std::uint8_t>(next);
  return changed;
}

bool PerformBank::change_fx(unsigned slot, int delta, CommandSink& out) noexcept {
  if (slot >= kPerfSlots || delta == 0) {
    return false;
  }
  Slot& s = slots_[slot];
  const int count = static_cast<int>(kPerfFxCount);
  int fx = static_cast<int>(s.fx) + (delta > 0 ? 1 : -1);  // one place, wrapping
  fx = fx >= count ? 0 : (fx < 0 ? count - 1 : fx);
  s.fx = static_cast<std::uint8_t>(fx);
  s.value[0] = 0;
  for (unsigned r = 1; r < kPerfRows; ++r) {
    s.value[r] = model::kPerfDefaultValues[fx][r - 1u];  // OS-PRFDEFAULTS: the new FX's own three
  }
  end_slot(slot);
  (void)apply(out);
  return true;
}

bool PerformBank::swap_slots(unsigned a, unsigned b, CommandSink& out) noexcept {
  if (a >= kPerfSlots || b >= kPerfSlots || a == b) {
    return false;
  }
  const Slot sa = slots_[a];
  slots_[a].fx = slots_[b].fx;
  slots_[b].fx = sa.fx;
  for (unsigned r = 0; r < kPerfRows; ++r) {
    slots_[a].value[r] = slots_[b].value[r];
    slots_[b].value[r] = sa.value[r];
  }
  // The rows would stay where they were, pointing at the other FX's values; here both slots go off, so that what a slot shows is what it does.
  end_slot(a);
  end_slot(b);
  (void)apply(out);
  return true;
}

bool PerformBank::scene_matches(unsigned i) const noexcept {
  if (!scene_stored(i)) {
    return false;
  }
  const model::PerfState& a = scenes_[i].slots;
  const model::PerfState now = state();
  for (unsigned s = 0; s < kPerfSlots; ++s) {
    if (a.fx[s] != now.fx[s] || a.row[s] != now.row[s]) {
      return false;
    }
    for (unsigned r = 0; r + 1u < kPerfRows; ++r) {
      if (a.value[s][r] != now.value[s][r]) {
        return false;
      }
    }
  }
  return true;
}

bool PerformBank::store_scene(unsigned i) noexcept {
  if (i >= kPerfScenes) {
    return false;
  }
  scenes_[i].slots = state();
  scenes_[i].stored = 1;
  return true;
}

bool PerformBank::recall_scene(unsigned i, CommandSink& out) noexcept {
  if (!scene_stored(i)) {
    return false;
  }
  cc_mask_ = 0;  // OS-MIDICCMAP S3: a scene recall ends every slot's CC mode
  // The slots as load_slots() takes them (an id the table does not have is none, a value is held to its FX's range, a row past 3 is off) but the affected tracks and the held pads stay:
  // it is a change of the slots under the performer's hands, not a load.
  const model::PerfState& s = scenes_[i].slots;
  for (unsigned k = 0; k < kPerfSlots; ++k) {
    const PerfFx fx = s.fx[k] < kPerfFxCount ? static_cast<PerfFx>(s.fx[k]) : PerfFx::None;
    slots_[k].fx = static_cast<std::uint8_t>(fx);
    slots_[k].row = fx == PerfFx::None || s.row[k] >= kPerfRows ? std::uint8_t{0} : s.row[k];
    slots_[k].value[0] = 0;
    for (unsigned r = 1; r < kPerfRows; ++r) {
      slots_[k].value[r] = static_cast<std::int16_t>(clamp_to(fx, s.value[k][r - 1u]));
    }
    load_row(k);
  }
  (void)apply(out);
  return true;
}

bool PerformBank::all_off(CommandSink& out) noexcept {
  bool changed = false;
  for (unsigned s = 0; s < kPerfSlots; ++s) {
    changed = changed || slots_[s].row != 0u || live_[s] != 0 || ((held_ >> s) & 1u) != 0u;
    end_slot(s);
  }
  (void)apply(out);
  return changed;
}

void PerformBank::copy_slot(unsigned slot) noexcept {
  if (slot < kPerfSlots) {
    copy_ = slots_[slot];
    copied_ = true;
  }
}

bool PerformBank::paste_slot(unsigned slot, CommandSink& out) noexcept {
  if (slot >= kPerfSlots || !copied_) {
    return false;
  }
  slots_[slot].fx = copy_.fx;
  for (unsigned r = 0; r < kPerfRows; ++r) {
    slots_[slot].value[r] = copy_.value[r];
  }
  end_slot(slot);
  (void)apply(out);
  return true;
}

bool PerformBank::apply(CommandSink& out) noexcept {
  unsigned sent = 0;
  if (rate_ != sent_rate_) {
    // The step rate first: it is the one command whose moment matters to the music (it lands on the next step boundary).
    if (!out.push(Command{Command::Kind::PerfRate, rate_, 0, 0})) {
      return false;
    }
    ++sent;
    sent_rate_ = rate_;
  }
  // Ends first, then sets, so that two FX of one group (the filters) that swap places end the old one before the new one takes the group.
  for (unsigned pass = 0; pass < 2u; ++pass) {
    for (unsigned t = 0; t < kPerfTracks; ++t) {
      for (unsigned f = 1; f < kPerfFxCount; ++f) {
        if (f == idx(PerfFx::PatternLength)) {
          continue;  // the sequencer's one pattern length: below
        }
        const int want = desired(t, f);
        if (want == sent_[t][f] || (pass == 0u) != (want == 0)) {
          continue;
        }
        const PerfFx fx = static_cast<PerfFx>(f);
        if (want == 0) {
          // An end while another FX of the same control-table group wants the group (a filter replaced by another) is silent: the new one's set takes the group over.
          const FxGroup g = perf_info(fx).group;
          bool taken = false;
          for (unsigned o = 1; g != FxGroup::None && o < kPerfFxCount; ++o) {
            taken = taken || (o != f && perf_info(static_cast<PerfFx>(o)).group == g && desired(t, o) != 0);
          }
          if (taken) {
            sent_[t][f] = 0;
            continue;
          }
        }
        Command c{};
        if (!perf_command(t, fx, want, c)) {
          sent_[t][f] = static_cast<std::int16_t>(want);  // nothing to tell the engine for this FX: remembered, not retried
          continue;
        }
        if (sent >= kMaxCommandsPerApply || !out.push(c)) {
          return false;  // the ring is full (or this apply has said enough): the rest goes at the next one
        }
        ++sent;
        sent_[t][f] = static_cast<std::int16_t>(want);
      }
    }
  }
  // The pattern length is the sequencer's, not a track's: the first affected track that wants one decides.
  int want_length = 0;
  for (unsigned t = 0; t < kPerfTracks && want_length == 0; ++t) {
    want_length = desired(t, idx(PerfFx::PatternLength));
  }
  if (want_length != sent_length_) {
    Command c{};
    if (!perf_command(0, PerfFx::PatternLength, want_length, c) || sent >= kMaxCommandsPerApply || !out.push(c)) {
      return false;
    }
    sent_length_ = static_cast<std::int16_t>(want_length);
  }
  return true;
}

}  // namespace o3os::seq
