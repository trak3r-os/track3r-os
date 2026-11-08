// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/pattern_verbs.h>

#include <o3os/model/fx_table.h>
#include <o3os/model/limits.h>

#include <cstring>

namespace o3os::model::verbs {

namespace {

// The draw of the HUMANIZE app (modules/apps/common/o3m_edit.h Draw, mix32), kept equal so that the native verb and the app give the same steps for the same seed. (The model does not include the SDK.)
constexpr std::uint32_t mix32(std::uint32_t x) noexcept {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

struct Draw {
  std::uint32_t s = 1;
  void seed(std::uint32_t user_seed, std::uint32_t stream) noexcept {
    const std::uint32_t x = mix32(user_seed * 2654435761u + (stream + 1u) * 0x9E3779B9u);
    s = x != 0u ? x : 0x6D2B79F5u;
  }
  std::uint32_t next() noexcept {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
};

bool same(const Step& a, const Step& b) noexcept { return std::memcmp(&a, &b, sizeof a) == 0; }

unsigned clampu(unsigned v, unsigned hi) noexcept { return v > hi ? hi : v; }

// The column's value of a step as a number (only meaningful where has_value says so).
int value_of(Column c, const Step& s) noexcept {
  switch (c) {
    case Column::Note: return s.note;
    case Column::Instr: return s.instr;
    case Column::Vol: return s.vol;
    case Column::Fx1: return s.fx[0].value;
    default: return s.fx[1].value;
  }
}

// a + (b - a) * k / d, rounded to the nearest, halves away from zero (d > 0, k 0..d).
int lerp(int a, int b, int k, int d) noexcept {
  const int num = (b - a) * k;
  return num >= 0 ? a + (2 * num + d) / (2 * d) : a - (2 * -num + d) / (2 * d);
}

}  // namespace

bool has_value(Column column, const Step& s) noexcept {
  switch (column) {
    case Column::Note: return s.note >= kNoteFirst && s.note <= kNoteLast;
    case Column::Instr: return s.instr >= 1u && s.instr <= edit::kInstrumentSlots;
    case Column::Vol: return s.vol <= kVolumeMax;
    case Column::Fx1: return s.fx[0].type != kFxNone;
    default: return s.fx[1].type != kFxNone;
  }
}

Count interpolate(Column column, Step* steps, unsigned len) noexcept {
  Count r;
  if (len > kMaxPatternLength) {
    len = kMaxPatternLength;
  }
  const unsigned fx = column == Column::Fx2 ? 1u : 0u;
  const bool is_fx = column == Column::Fx1 || column == Column::Fx2;
  // An FX anchor is a type the engine executes (fx::is_executable), not just any non-None type: the editor never writes the others. An INSTR / VOL
  // anchor also holds a note: those ramps write noted rows only (below), so a stale value on a note-less row never sets an endpoint.
  const auto anchor = [&](unsigned i) noexcept {
    if (!has_value(column, steps[i])) {
      return false;
    }
    if (is_fx) {
      return fx::is_executable(steps[i].fx[fx].type);
    }
    return column == Column::Note || has_value(Column::Note, steps[i]);
  };
  unsigned a = len;
  unsigned b = len;
  for (unsigned i = 0; i < len; ++i) {
    if (anchor(i) && a == len) {
      a = i;
    }
  }
  if (a == len) {
    return r;
  }
  for (unsigned i = a; i < len; ++i) {
    if (anchor(i) && (!is_fx || steps[i].fx[fx].type == steps[a].fx[fx].type)) {
      b = i;  // the last anchor (for an FX: the last step of the first anchor's type)
    }
  }
  if (b == len || b == a) {
    return r;
  }
  const int va = value_of(column, steps[a]);
  const int vb = value_of(column, steps[b]);
  const int d = static_cast<int>(b - a);
  for (unsigned i = a + 1u; i < b; ++i) {
    Step& s = steps[i];
    const Step before = s;
    const int v = lerp(va, vb, static_cast<int>(i - a), d);
    switch (column) {
      case Column::Note:
        if (s.note != kNoteEmpty && !has_value(Column::Note, s)) {
          ++r.skipped;  // a cut, a fade, an off or a reserved byte stays
          continue;
        }
        if (s.note == kNoteEmpty && s.instr == 0u) {
          s.instr = steps[a].instr;
        }
        s.note = static_cast<std::uint8_t>(v);
        break;
      case Column::Instr:
      case Column::Vol:
        if (!has_value(Column::Note, s)) {
          ++r.skipped;  // the INSTR and VOL ramps write the rows that hold a note only: an instrument or a volume on a note-less step is nothing
          continue;
        }
        if (column == Column::Instr) {
          s.instr = static_cast<std::uint8_t>(v);
        } else {
          s.vol = static_cast<std::uint8_t>(v);
        }
        break;
      default:
        if (s.fx[fx].type != kFxNone && s.fx[fx].type != steps[a].fx[fx].type) {
          ++r.skipped;  // another FX type stays
          continue;
        }
        s.fx[fx].type = steps[a].fx[fx].type;
        s.fx[fx].value = static_cast<std::uint8_t>(v);
        break;
    }
    if (!same(before, s)) {
      ++r.changed;
    }
  }
  return r;
}

Count randomise(unsigned amount, unsigned chance, unsigned seed, unsigned stream, Column column, Step* steps, unsigned len) noexcept {
  Count r;
  if (len > kMaxPatternLength) {
    len = kMaxPatternLength;
  }
  amount = clampu(amount, kMaxAmount);
  chance = clampu(chance, kMaxChance);
  if (amount == 0u || chance == 0u) {
    return r;
  }
  Draw d;
  d.seed(seed, stream);
  for (unsigned i = 0; i < len; ++i) {
    const std::uint32_t pick = d.next();  // two draws for every step of the range, used or not
    const std::uint32_t jitter = d.next();
    Step& s = steps[i];
    if (!has_value(column, s) || pick % 100u >= chance) {
      continue;
    }
    int lo = 0;
    int hi = 0;
    unsigned fx = 0;
    switch (column) {
      case Column::Note: lo = kNoteFirst; hi = kNoteLast; break;
      case Column::Instr: lo = 1; hi = static_cast<int>(edit::kInstrumentSlots); break;
      case Column::Vol: lo = 0; hi = kVolumeMax; break;
      default: {
        fx = column == Column::Fx2 ? 1u : 0u;
        const std::uint8_t type = s.fx[fx].type;
        if (!fx::is_executable(type) || s.fx[fx].value > fx::random_hi(type)) {
          ++r.skipped;  // a type the editor does not write, or a Chance condition: not a number to move
          continue;
        }
        lo = fx::row(type).lo;
        hi = fx::random_hi(type);
        break;
      }
    }
    const int old = value_of(column, s);
    int v = old + static_cast<int>(jitter % (2u * amount + 1u)) - static_cast<int>(amount);
    v = v < lo ? lo : (v > hi ? hi : v);
    if (v == old) {
      continue;
    }
    switch (column) {
      case Column::Note: s.note = static_cast<std::uint8_t>(v); break;
      case Column::Instr: s.instr = static_cast<std::uint8_t>(v); break;
      case Column::Vol: s.vol = static_cast<std::uint8_t>(v); break;
      default: s.fx[fx].value = static_cast<std::uint8_t>(v); break;
    }
    ++r.changed;
  }
  return r;
}

Count rotate(bool up, unsigned elements, Step* steps, unsigned len) noexcept {
  Count r;
  if (len > kMaxPatternLength) {
    len = kMaxPatternLength;
  }
  if (len < 2u || (elements & edit::kElMask) == 0u) {
    return r;
  }
  Step src[kMaxPatternLength];
  std::memcpy(src, steps, sizeof(Step) * len);
  const bool note = (elements & (edit::kElNote | edit::kElInstr)) != 0u;
  for (unsigned i = 0; i < len; ++i) {
    const Step& from = src[up ? (i + 1u) % len : (i + len - 1u) % len];
    Step& to = steps[i];
    if (note) {
      to.note = from.note;
      to.instr = from.instr;
    }
    if ((elements & edit::kElVol) != 0u) {
      to.vol = from.vol;
    }
    if ((elements & edit::kElFx1) != 0u) {
      to.fx[0] = from.fx[0];
    }
    if ((elements & edit::kElFx2) != 0u) {
      to.fx[1] = from.fx[1];
    }
    if (!same(src[i], to)) {
      ++r.changed;
    }
  }
  return r;
}

Count humanise(unsigned time, unsigned vol, unsigned seed, unsigned stream, Step* steps, unsigned len) noexcept {
  Count r;
  if (len > kMaxPatternLength) {
    len = kMaxPatternLength;
  }
  time = clampu(time, kMaxTime);
  vol = clampu(vol, kMaxAmount);
  Draw d;
  d.seed(seed, stream);
  for (unsigned j = 0; j < len; ++j) {
    Step& s = steps[j];
    if (s.note < kNoteFirst || s.note > kNoteLast) {
      continue;
    }
    const Step before = s;
    const std::uint32_t delay = d.next() % (time + 1u);  // two draws for every step with a note, used or not
    const std::uint32_t jitter = d.next() % (2u * vol + 1u);
    if (vol != 0u && s.vol != kVolumeUnset) {
      int v = static_cast<int>(s.vol) + static_cast<int>(jitter) - static_cast<int>(vol);
      v = v < 0 ? 0 : (v > kVolumeMax ? kVolumeMax : v);
      s.vol = static_cast<std::uint8_t>(v);
    }
    if (time != 0u) {
      if (s.fx[0].type == kFxMicroMove) {
        s.fx[0].value = static_cast<std::uint8_t>(delay);
      } else if (s.fx[1].type == kFxMicroMove) {
        s.fx[1].value = static_cast<std::uint8_t>(delay);
      } else if (delay != 0u) {
        if (s.fx[1].type == kFxNone) {
          s.fx[1] = {kFxMicroMove, static_cast<std::uint8_t>(delay)};
        } else if (s.fx[0].type == kFxNone) {
          s.fx[0] = {kFxMicroMove, static_cast<std::uint8_t>(delay)};
        } else {
          ++r.skipped;  // both FX slots are taken: the step keeps its timing
        }
      }
    }
    if (!same(before, s)) {
      ++r.changed;
    }
  }
  return r;
}

Count run(const Plan& plan, unsigned stream, Step* steps, unsigned len) noexcept {
  switch (plan.verb) {
    case Verb::Interpolate: return interpolate(plan.column, steps, len);
    case Verb::Randomise: return randomise(plan.amount, plan.chance, plan.seed, stream, plan.column, steps, len);
    case Verb::RotateUp: return rotate(true, plan.elements, steps, len);
    case Verb::RotateDown: return rotate(false, plan.elements, steps, len);
    default: return humanise(plan.time, plan.vol, plan.seed, stream, steps, len);
  }
}

BlockResult apply_block(Editor& editor, core::ActionId action, core::ActionArg arg, std::uint32_t pattern, const Block& block, const Plan& plan) noexcept {
  BlockResult out;
  if (block.tracks == 0u || block.rows == 0u || block.track0 >= kMaxTracks || block.tracks > kMaxTracks - block.track0 || block.row0 >= kMaxPatternLength ||
      block.rows > kMaxPatternLength - block.row0) {
    out.status = Status::BadIndex;
    return out;
  }
  const Pattern* p = editor.project().pattern(pattern);
  if (p == nullptr) {
    out.status = Status::NotFound;
    return out;
  }
  editor.begin_group(action, arg);
  for (unsigned l = 0; l < block.tracks; ++l) {
    const std::uint32_t track = block.track0 + l;
    const unsigned track_len = p->track_length[track];
    if (block.row0 >= track_len) {
      ++out.refused;  // the block starts past this track's end
      continue;
    }
    const unsigned n = block.rows < track_len - block.row0 ? block.rows : track_len - block.row0;  // never past the track's own length
    Step buf[kMaxPatternLength];
    std::memcpy(buf, &p->steps[track][block.row0], sizeof(Step) * n);
    const Count c = run(plan, l, buf, n);
    out.count.skipped += c.skipped;
    if (c.changed == 0u) {
      continue;
    }
    const Status s = editor.set_steps(pattern, track, block.row0, buf, n);
    if (s != Status::Ok) {
      out.status = s;
      break;
    }
    out.count.changed += c.changed;
    ++out.tracks_written;
  }
  editor.end_group();
  return out;
}

}  // namespace o3os::model::verbs
