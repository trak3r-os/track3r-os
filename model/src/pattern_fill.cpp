// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/pattern_fill.h>

#include <o3os/core/scales.h>
#include <o3os/model/fx_table.h>
#include <o3os/model/limits.h>

namespace o3os::model::fill {

bool euclid_hit(unsigned events, unsigned len, unsigned i) noexcept {
  if (len == 0u || events == 0u || i >= len) {
    return false;
  }
  const unsigned e = events > len ? len : events;
  return (i * e) % len < e;  // e <= len <= 128 and i < len: no overflow
}

unsigned scale_up(unsigned note, unsigned root, unsigned scale) noexcept {
  for (unsigned n = note; n <= 127u; ++n) {
    if (core::is_in_scale(static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(root), static_cast<std::uint8_t>(scale))) {
      return n;
    }
  }
  return 128u;
}

unsigned select(const Plan& plan, unsigned len, Rng& rng, bool* hit) noexcept {
  const unsigned every = plan.n < 1u ? 1u : (plan.n > kMaxN ? kMaxN : plan.n);
  const unsigned density = plan.density > 100u ? 100u : plan.density;
  unsigned count = 0;
  for (unsigned i = 0; i < len; ++i) {
    bool h = false;
    switch (plan.type) {
      case Type::Every:
      case Type::Scale:
        h = i % every == 0u;
        break;
      case Type::Random:
        h = rng.below(100) < density;
        break;
      case Type::Euclid:
        h = euclid_hit(plan.events > kMaxN ? kMaxN : plan.events, len, i);
        break;
    }
    hit[i] = h;
    count += h ? 1u : 0u;
  }
  return count;
}

Range domain(const Plan& plan) noexcept {
  switch (plan.what) {
    case What::Instr: return {1, static_cast<std::uint8_t>(edit::kInstrumentSlots)};
    case What::Vol: return {0, static_cast<std::uint8_t>(kVolumeMax)};
    case What::Fx1:
    case What::Fx2: {
      const fx::Row& s = fx::row(plan.fx_type);
      return {s.lo, s.hi};
    }
    case What::Note: break;
  }
  return {0, 127};
}

void range_default(Plan& plan, const Value& value) noexcept {
  switch (plan.what) {
    case What::Note: {
      const unsigned base = value.note >= 1u && value.note <= 128u ? value.note - 1u : edit::kDefaultNote;  // an OG note
      plan.from = static_cast<std::uint8_t>(base);
      plan.to = static_cast<std::uint8_t>(base + 11u > 127u ? 127u : base + 11u);
      break;
    }
    case What::Instr:
      plan.from = plan.to = static_cast<std::uint8_t>(value.instr < 1u ? 1u : (value.instr > edit::kInstrumentSlots ? edit::kInstrumentSlots : value.instr));
      break;
    case What::Vol:
      plan.from = value.vol <= kVolumeMax ? value.vol : edit::kFirstVolume;
      plan.to = 0;
      break;
    case What::Fx1:
    case What::Fx2: {
      const std::uint8_t t = value.fx_type[plan.what == What::Fx1 ? 0 : 1];
      plan.fx_type = t != 0u && fx::picker_index(t) != 0u ? t : fx::kPickerOrder[1];
      plan.from = plan.to = fx::row(plan.fx_type).start;
      break;
    }
  }
}

namespace {

constexpr unsigned kWindow = 128;  // candidates: every note of the window (0..127)

// The in-scale notes of lo..hi (OG notes, at most 127): `cand` holds them ascending. Returns how many.
unsigned window(unsigned lo, unsigned hi, const Plan& plan, std::uint8_t (&cand)[kWindow]) noexcept {
  unsigned c = 0;
  for (unsigned n = lo; n <= hi && n <= 127u; ++n) {
    if (core::is_in_scale(static_cast<std::uint8_t>(n), plan.root, plan.scale)) {
      cand[c++] = static_cast<std::uint8_t>(n);
    }
  }
  return c;
}

// Hit `k` of `hits` of a RAMP from `from` to `to`, in integers: the first hit is `from`, the last is `to`, one hit is `from`.
unsigned ramp(unsigned from, unsigned to, unsigned k, unsigned hits) noexcept {
  if (hits <= 1u) {
    return from;
  }
  return (from * (hits - 1u - k) + to * k + (hits - 1u) / 2u) / (hits - 1u);
}

}  // namespace

unsigned apply(const Plan& plan, Step* steps, unsigned len, Rng& rng, edit::Last& last) noexcept {
  if (len > kMaxPatternLength) {
    len = kMaxPatternLength;
  }
  const bool fx_fill = plan.what == What::Fx1 || plan.what == What::Fx2;
  if (fx_fill && (plan.fx_type == 0u || !fx::is_executable(plan.fx_type))) {
    return 0;  // no type to write (None would clear the column)
  }
  bool hit[kMaxPatternLength];
  const unsigned hits = select(plan, len, rng, hit);
  const edit::Rule rule{true, true};  // the single-step rule: the step takes the value whatever it held (with one step selected)
  const Range dom = domain(plan);
  const unsigned a = plan.from < dom.lo ? dom.lo : (plan.from > dom.hi ? dom.hi : plan.from);
  const unsigned b = plan.to < dom.lo ? dom.lo : (plan.to > dom.hi ? dom.hi : plan.to);
  const unsigned lo = a < b ? a : b;
  const unsigned hi = a < b ? b : a;
  const bool picks = plan.what == What::Note && (plan.type == Type::Random || plan.type == Type::Scale);  // these two choose the notes: FROM..TO is their window and MODE plays no part
  const bool draws = picks || (plan.what == What::Note && plan.mode == Mode::Random);                    // a note drawn from the in-scale notes of lo..hi
  std::uint8_t cand[kWindow];
  const unsigned count = draws ? window(lo, hi, plan, cand) : 0u;
  unsigned run = 0;        // SCALE: the next candidate
  int previous = -1;       // a drawn note: the candidate written last, so a note never follows itself
  unsigned written = 0;
  unsigned k = 0;          // the hit being written, 0-based
  for (unsigned i = 0; i < len; ++i) {
    if (!hit[i]) {
      continue;
    }
    const unsigned at = k++;
    unsigned value = a;  // CONST
    if (!picks) {
      if (plan.mode == Mode::Ramp) {
        value = ramp(a, b, at, hits);
      } else if (plan.mode == Mode::Random && !draws) {
        // OS-CECHANCE C2: a random draw for an FX stays inside random_hi (Chance: 0..100, a condition is never drawn); the other whats draw lo..hi.
        unsigned hi_draw = hi;
        unsigned lo_draw = lo;
        if (fx_fill) {
          const unsigned rhi = fx::random_hi(plan.fx_type);
          hi_draw = hi < rhi ? hi : rhi;
          lo_draw = lo < hi_draw ? lo : hi_draw;
        }
        value = lo_draw + rng.below(hi_draw - lo_draw + 1u);
      }
    }
    switch (plan.what) {
      case What::Instr:
        edit::instrument_set(steps[i], value, rule, last);
        break;
      case What::Vol:
        edit::volume_set(steps[i], value, rule);
        break;
      case What::Fx1:
      case What::Fx2: {
        const unsigned col = plan.what == What::Fx1 ? 0u : 1u;
        edit::fx_type_set(steps[i], col, plan.fx_type);
        edit::fx_value_set(steps[i], col, static_cast<int>(value), rule, last);
        break;
      }
      case What::Note: {
        unsigned note = 128u;  // 128: none, the step stays as it was (PAR-150)
        if (plan.type == Type::Scale) {
          if (count != 0u) {
            note = cand[run % count];
            ++run;
          }
        } else if (draws) {
          if (count != 0u) {
            unsigned c = rng.below(count);
            if (count > 1u && static_cast<int>(c) == previous) {
              c = (c + 1u) % count;
            }
            previous = static_cast<int>(c);
            note = cand[c];
          }
        } else {
          note = scale_up(value, plan.root, plan.scale);
        }
        if (note > 127u) {
          continue;
        }
        edit::note_set(steps[i], note + 1u, rule, last);
        break;
      }
    }
    ++written;
  }
  return written;
}

}  // namespace o3os::model::fill
