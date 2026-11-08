// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/step_edit.h>

#include <o3os/model/fx_table.h>

namespace o3os::model::edit {

namespace {

constexpr int kMaxDelta = 4096;  // a delta is clamped before it is added: an encoder fed from data cannot overflow
constexpr int kReserved = 1 << 20;

constexpr int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// The OG's signed note of a model byte: -1 empty, -2 fade, -3 cut, -4 off, 0..127; kReserved for the bytes 129..252.
constexpr int og_note(unsigned m) noexcept {
  if (m == 0) {
    return -1;
  }
  if (m <= 128) {
    return static_cast<int>(m) - 1;
  }
  return m == kNoteFade ? -2 : (m == kNoteCut ? -3 : (m == kNoteOff ? -4 : kReserved));
}

constexpr std::uint8_t model_note(int n) noexcept {
  switch (n) {
    case -1:
      return kNoteEmpty;
    case -2:
      return kNoteFade;
    case -3:
      return kNoteCut;
    case -4:
      return kNoteOff;
    default:
      return static_cast<std::uint8_t>(n + 1);  // 0..127: callers clamp
  }
}

constexpr bool is_note(int n) noexcept { return n >= 0 && n <= 127; }

// A fade, a cut or an off carries no instrument of its own (the model's 0, step.h). When a
// rule turns such a step into a note, the note gets instrument 1 (our rule).
void give_instrument(Step& step, int note_before) noexcept {
  if (!is_note(note_before) && is_note(og_note(step.note)) && step.instr == 0) {
    step.instr = 1;
  }
}

// The instrument order: positions 0..15 are slots 63..48, positions 16..63 are slots 0..47.
constexpr unsigned order_slot(unsigned pos) noexcept { return pos < 16u ? 63u - pos : pos - 16u; }
// The position of a slot, 0 when it is not in the order (our rule: a value outside the order answers 0).
constexpr unsigned order_pos(unsigned instrument) noexcept {
  if (instrument < 1u || instrument > kInstrumentSlots) {
    return 0;
  }
  const unsigned slot = instrument - 1u;
  return slot < 48u ? 16u + slot : 63u - slot;
}

// OS-CECHANCE C2, the coarse walk of Chance (F12, verified parity with tracker-ce@7fd6c2c mtSequencer_patternEdit.cpp:731-757): a turn of more
// than one detent walks 0..100 by the delta (clamped at 100), then 101, 102, 103 one each, then by 6 up to 174 — and back the same way.
constexpr std::uint8_t chance_coarse(std::uint8_t v, int delta) noexcept {
  if (delta > 1) {
    if (v < fx::kChanceProbMax) {
      return static_cast<std::uint8_t>(v + delta > fx::kChanceProbMax ? fx::kChanceProbMax : v + delta);
    }
    if (v == fx::kChanceProbMax) {
      return fx::kChanceWithLast;
    }
    if (v == fx::kChanceWithLast) {
      return fx::kChanceNotLast;
    }
    if (v == fx::kChanceNotLast) {
      return fx::kChanceCycleFirst;
    }
    return static_cast<std::uint8_t>(v + fx::kChanceCycleStep > fx::kChanceMax ? fx::kChanceMax : v + fx::kChanceCycleStep);
  }
  if (v <= fx::kChanceProbMax) {
    return static_cast<std::uint8_t>(v + delta < 0 ? 0 : v + delta);
  }
  if (v == fx::kChanceWithLast) {
    return fx::kChanceProbMax;
  }
  if (v == fx::kChanceNotLast) {
    return fx::kChanceWithLast;
  }
  if (v < fx::kChanceCycleFirst + fx::kChanceCycleStep) {
    return fx::kChanceNotLast;
  }
  return static_cast<std::uint8_t>(v - fx::kChanceCycleStep);  // v >= 109: no underflow
}

// The type in an FX slot after the single-step rule picks the last used one for an empty slot.
constexpr std::uint8_t last_fx_type(const Last& last) noexcept {
  return static_cast<std::uint8_t>(fx::type_of_picker(last.fx));
}

}  // namespace

unsigned instrument_walk(unsigned instrument, int delta, int limit) noexcept {
  const int pos = static_cast<int>(order_pos(instrument)) + clampi(delta, -limit, limit);
  return order_slot(static_cast<unsigned>(clampi(pos, 0, 63))) + 1u;
}

void note_set(Step& step, unsigned note, const Rule& rule, Last& last) noexcept {
  if (!is_enterable_note(note)) {
    return;
  }
  const int n = og_note(step.note);
  if (rule.single_cell) {
    if (n == -1) {
      step.instr = last.instrument;
    }
    step.note = static_cast<std::uint8_t>(note);
    give_instrument(step, n);
  } else if (n != -1 && n != kReserved) {
    step.note = static_cast<std::uint8_t>(note);
    give_instrument(step, n);
  }
}

void note_delta(Step& step, int delta, const Rule& rule, Last& last) noexcept {
  delta = clampi(delta, -kMaxDelta, kMaxDelta);
  const int n = og_note(step.note);
  if (delta == 0 || n == kReserved) {
    return;
  }
  if (rule.single_cell) {
    if (n == -1 && delta > 0) {
      step.instr = last.instrument;
      step.note = model_note(static_cast<int>(kDefaultNote));
    } else if (n == -1) {
      step.instr = last.instrument;
      step.note = model_note(clampi(-1 + delta, -4, 127));
    } else {
      step.note = model_note(clampi(n + delta, -4, 127));
      give_instrument(step, n);
    }
  } else if (is_note(n)) {
    step.note = model_note(clampi(n + delta, 0, 127));
  }
}

void instrument_set(Step& step, unsigned instrument, const Rule& rule, Last& last) noexcept {
  if (instrument < 1u || instrument > kInstrumentSlots) {
    return;
  }
  int n = og_note(step.note);
  if (rule.single_cell) {
    if (n == -1) {
      n = static_cast<int>(kDefaultNote);
      step.note = model_note(n);
    }
    if (is_note(n)) {
      step.instr = static_cast<std::uint8_t>(instrument);
      last.instrument = static_cast<std::uint8_t>(instrument);
    }
  } else if (is_note(n)) {
    step.instr = static_cast<std::uint8_t>(instrument);
  }
}

void instrument_delta(Step& step, int delta, const Rule& rule, Last& last, int limit) noexcept {
  delta = clampi(delta, -kMaxDelta, kMaxDelta);
  const int n = og_note(step.note);
  if (delta == 0) {
    return;
  }
  if (rule.single_cell) {
    if (is_note(n)) {
      step.instr = static_cast<std::uint8_t>(instrument_walk(step.instr, delta, limit));
      last.instrument = step.instr;
    } else if (n == -1) {
      step.note = model_note(static_cast<int>(kDefaultNote));
      step.instr = last.instrument;
    }
  } else if (is_note(n)) {
    step.instr = static_cast<std::uint8_t>(instrument_walk(step.instr, delta, limit));
  }
}

void volume_set(Step& step, unsigned value, const Rule& rule) noexcept {
  const std::uint8_t v = static_cast<std::uint8_t>(value > kVolumeMax ? kVolumeMax : value);
  if (rule.single_state || step.vol <= kVolumeMax) {
    step.vol = v;
  }
}

void volume_delta(Step& step, int delta, const Rule& rule) noexcept {
  delta = clampi(delta, -kMaxDelta, kMaxDelta);
  if (delta == 0) {
    return;
  }
  if (step.vol == kVolumeUnset) {
    if (rule.single_state) {
      step.vol = kFirstVolume;  // the first entry takes the default, whatever the direction (as an FX value does)
    }
    return;
  }
  if (step.vol <= kVolumeMax) {
    step.vol = static_cast<std::uint8_t>(clampi(static_cast<int>(step.vol) + delta, 0, kVolumeMax));
  }  // 128..254 is not a volume: carried
}

void fx_value_delta(Step& step, unsigned fx, int delta, const Rule& rule, Last& last) noexcept {
  delta = clampi(delta, -kMaxDelta, kMaxDelta);
  if (fx > 1u || delta == 0 || !fx::is_executable(step.fx[fx].type)) {
    return;
  }
  StepFx& f = step.fx[fx];
  if (rule.single_state && f.type == 0) {
    f.type = last_fx_type(last);
    f.value = f.type == 0 ? std::uint8_t{0} : fx::row(f.type).start;
  } else {
    last.fx = fx::picker_index(f.type);  // (for a None slot of a range too: the last used FX becomes None)
    if (f.type != 0) {
      const fx::Row& s = fx::row(f.type);
      if (f.type == fx::kChanceType && (delta > 1 || delta < -1)) {
        f.value = chance_coarse(f.value, delta);  // OS-CECHANCE C2: the coarse walk of F12 (parity); a plain turn walks every value
      } else {
        f.value = static_cast<std::uint8_t>(clampi(static_cast<int>(f.value) + delta, s.lo, s.hi));
      }
    }
  }
}

namespace {

// The entry rules shared by the absolute value and the pad: an executable type only; the single branch gives an empty slot the last
// used type; a slot that is still None has no value to set (a value written for it is ignored).
bool fx_enter(Step& step, unsigned fx, const Rule& rule, const Last& last) noexcept {
  if (fx > 1u || !fx::is_executable(step.fx[fx].type)) {
    return false;
  }
  StepFx& f = step.fx[fx];
  if (rule.single_state && f.type == 0) {
    f.type = last_fx_type(last);
  }
  return f.type != 0;
}

}  // namespace

void fx_value_set(Step& step, unsigned fx, int value, const Rule& rule, Last& last) noexcept {
  if (fx_enter(step, fx, rule, last)) {
    const fx::Row& s = fx::row(step.fx[fx].type);
    step.fx[fx].value = static_cast<std::uint8_t>(clampi(value, s.lo, s.hi));
  }
}

void fx_pad(Step& step, unsigned fx, unsigned pad, const Rule& rule, Last& last) noexcept {
  if (pad >= kPadCount) {
    return;
  }
  if (fx_enter(step, fx, rule, last)) {
    const fx::Row& s = fx::row(step.fx[fx].type);
    // OS-CECHANCE C2: the pads map 0..47 over 0..100 for Chance (random_hi), over the type's whole range for the others.
    const int v = map_int(static_cast<int>(pad), 0, 47, s.lo, static_cast<int>(fx::random_hi(step.fx[fx].type)));
    step.fx[fx].value = static_cast<std::uint8_t>(clampi(v, s.lo, s.hi));
  }
}

void fx_type_delta(Step& step, unsigned fx, int delta) noexcept {
  if (fx > 1u || delta == 0) {
    return;
  }
  StepFx& f = step.fx[fx];
  if (f.type == 0) {
    return;
  }
  const bool runnable = fx::is_executable(f.type);
  // The picker index is a uint8_t that is stepped, so one place below the first wraps to 255, which reads as None.
  const std::uint8_t index = static_cast<std::uint8_t>(fx::picker_index(f.type) + (delta > 0 ? 1 : -1));
  f.type = static_cast<std::uint8_t>(fx::type_of_picker(index));
  const fx::Row& s = fx::row(f.type);
  f.value = runnable ? static_cast<std::uint8_t>(clampi(f.value, s.lo, s.hi)) : s.start;
  if (f.type == 0) {
    f.value = 0;
  }
}

void fx_type_set(Step& step, unsigned fx, unsigned type) noexcept {
  if (fx > 1u || type > 255u || !fx::is_executable(static_cast<std::uint8_t>(type))) {
    return;
  }
  StepFx& f = step.fx[fx];
  const bool was_none = f.type == 0;
  f.type = static_cast<std::uint8_t>(type);
  const fx::Row& s = fx::row(f.type);
  // New Community OS behaviour (OS-PFXFIX): a type picked on an empty cell starts at its table default (Tune 24 = 0 st, Micro-tune 99), as the value edit does.
  // A cell that already has a type keeps its value, carried over and clamped.
  f.value = f.type == 0 ? std::uint8_t{0} : (was_none ? s.start : static_cast<std::uint8_t>(clampi(f.value, s.lo, s.hi)));
}

void clear(Step& step, unsigned elements) noexcept {
  if ((elements & (kElNote | kElInstr)) != 0u) {
    step.note = kNoteEmpty;
    step.instr = 0;
  }
  if ((elements & kElFx1) != 0u) {
    step.fx[0] = StepFx{kFxNone, 0};
  }
  if ((elements & kElFx2) != 0u) {
    step.fx[1] = StepFx{kFxNone, 0};
  }
  if ((elements & kElVol) != 0u) {
    step.vol = kVolumeUnset;
  }
}

}  // namespace o3os::model::edit
