// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/core/learn.h>

namespace o3os::core {

namespace {

// Pickup: how close, on the knob's 0..127 scale, counts as having reached the parameter.
constexpr float kPickupReach = 1.0f;

bool bindable_kind(ParamKind kind) noexcept {
  return kind == ParamKind::Instrument || kind == ParamKind::Track || kind == ParamKind::Master ||
         kind == ParamKind::Project || kind == ParamKind::Device || kind == ParamKind::Insert;
}

}  // namespace

bool learn_valid(const LearnBinding& b) noexcept {
  if (learn_empty(b)) {
    return b.cc == 0 && b.channel == 0 && b.mode == 0 && b.flags == 0;
  }
  if (!bindable_kind(param_kind(b.param)) || b.cc > 127 || b.channel > 16 || b.mode >= kLearnModeCount ||
      (b.flags & ~kLearnFollow) != 0) {
    return false;
  }
  if ((b.flags & kLearnFollow) != 0 && (param_kind(b.param) != ParamKind::Instrument || param_index(b.param) != 0)) {
    return false;
  }
  if (param_kind(b.param) == ParamKind::Insert && !insert_param_valid(b.param)) {  // OS-INSFX S7: an insert id this version has (a track's slot or the master's, a field below 32)
    return false;
  }
  return true;
}

bool learn_targets(const LearnBinding& b, ParamId id) noexcept {
  if (learn_empty(b)) {
    return false;
  }
  if ((b.flags & kLearnFollow) != 0) {
    return param_kind(id) == param_kind(b.param) && param_field(id) == param_field(b.param);
  }
  return id == b.param;
}

bool learn_overlap(const LearnBinding& a, const LearnBinding& b) noexcept {
  return !learn_empty(a) && !learn_empty(b) && a.cc == b.cc && (a.channel == 0 || b.channel == 0 || a.channel == b.channel);
}

int learn_find_param(const LearnBinding* slots, unsigned count, ParamId id) noexcept {
  for (unsigned i = 0; i < count; ++i) {
    if (learn_targets(slots[i], id)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int learn_find_cc(const LearnBinding* slots, unsigned count, unsigned channel, unsigned cc, unsigned start) noexcept {
  for (unsigned i = start; i < count; ++i) {
    const LearnBinding& b = slots[i];
    if (!learn_empty(b) && b.cc == cc && (b.channel == 0 || channel == 0 || b.channel == channel)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool learn_plan(const LearnBinding* slots, unsigned count, const LearnBinding& b, LearnPlan& out) noexcept {
  if (!learn_valid(b) || learn_empty(b)) {
    return false;
  }
  // The same parameter again (compared as the binding names it: a follow binding is the field, not an instrument).
  int write = -1;
  for (unsigned i = 0; i < count; ++i) {
    if (!learn_empty(slots[i]) && slots[i].param == b.param && ((slots[i].flags ^ b.flags) & kLearnFollow) == 0) {
      write = static_cast<int>(i);
      break;
    }
  }
  std::uint32_t clear = 0;
  for (unsigned i = 0; i < count && i < 32; ++i) {
    if (static_cast<int>(i) != write && learn_overlap(slots[i], b)) {
      clear |= std::uint32_t{1} << i;
    }
  }
  if (write < 0) {
    // A new parameter takes the slot of the first binding it displaces, else the first empty one.
    if (clear != 0) {
      unsigned first = 0;
      while ((clear & (std::uint32_t{1} << first)) == 0) {
        ++first;
      }
      write = static_cast<int>(first);
      clear &= ~(std::uint32_t{1} << first);
    } else {
      for (unsigned i = 0; i < count; ++i) {
        if (learn_empty(slots[i])) {
          write = static_cast<int>(i);
          break;
        }
      }
    }
  }
  if (write < 0) {
    return false;  // full
  }
  out.write = write;
  out.clear = clear;
  return true;
}

bool learn_store_valid(const LearnBinding* slots, unsigned count) noexcept {
  for (unsigned i = 0; i < count; ++i) {
    if (!learn_valid(slots[i])) {
      return false;
    }
    for (unsigned j = 0; j < i; ++j) {
      if (learn_overlap(slots[i], slots[j]) ||
          (!learn_empty(slots[i]) && slots[i].param == slots[j].param && ((slots[i].flags ^ slots[j].flags) & kLearnFollow) == 0)) {
        return false;
      }
    }
  }
  return true;
}

bool learn_absolute(LearnRuntime& rt, ParamId id, LearnMode mode, unsigned cc_value, float current_norm, std::uint32_t current_gen,
                    float& norm_out) noexcept {
  const unsigned cc = cc_value > 127 ? 127u : cc_value;
  const float knob = static_cast<float>(cc) / 127.0f;
  if (rt.gen != current_gen || rt.key != learn_key(id)) {
    // Another parameter, or the parameter was moved by something else: the knob has to catch it again, and where the knob was before
    // that says nothing about where it is relative to the parameter now (it must not "cross" a value set after it was last seen).
    rt.gen = current_gen;
    rt.key = learn_key(id);
    rt.flags = 0;
  }
  if (mode == LearnMode::Pickup && (rt.flags & kLearnPicked) == 0) {
    const float pos = current_norm * 127.0f;  // where the parameter sits on the knob's scale
    const float here = static_cast<float>(cc);
    bool reached = (pos > here ? pos - here : here - pos) <= kPickupReach;
    if (!reached && (rt.flags & kLearnHasLast) != 0) {
      const float a = static_cast<float>(rt.last);
      const float lo = a < here ? a : here;
      const float hi = a < here ? here : a;
      reached = pos >= lo && pos <= hi;  // the knob went through the parameter between two messages
    }
    rt.last = static_cast<std::uint8_t>(cc);
    rt.flags |= kLearnHasLast;
    if (!reached) {
      return false;
    }
    rt.flags |= kLearnPicked;
  }
  rt.last = static_cast<std::uint8_t>(cc);
  rt.flags |= kLearnHasLast;
  norm_out = knob;
  return true;
}

int learn_relative_delta(unsigned cc_value) noexcept {
  if (cc_value >= 1 && cc_value <= 63) {
    return static_cast<int>(cc_value);
  }
  if (cc_value >= 65 && cc_value <= 127) {
    return -static_cast<int>(128u - cc_value);
  }
  return 0;
}

}  // namespace o3os::core
