// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/core/gesture.h>

namespace o3os::core {

namespace {

constexpr std::uint8_t kNoMod = 0xFF;  // Entry::mod: no modifier was held
constexpr std::uint8_t kWaits = kWantHold | kWantDoubleTap | kWantChord | kWantHoldEncoder | kWantModifier;

constexpr InputRef button_ref(std::uint8_t id) noexcept { return InputRef{InputKind::Button, id}; }
constexpr InputRef modifier_ref(std::uint8_t mod) noexcept { return mod == kNoMod ? kNoInput : button_ref(mod); }

void emit(GestureList& out, Tick t, GestureKind kind, InputRef input, InputRef modifier, std::int16_t value, bool repeat = false) noexcept {
  (void)out.push_back(Gesture{t, kind, input, modifier, value, repeat});  // capacity covers the worst case (header)
}

// When the (n+1)th repeat is due, in ms after the press: the sum of the first n+1 gaps, the last gap for ever after the table.
constexpr std::uint32_t repeat_time_ms(std::uint32_t n) noexcept {
  std::uint32_t t = 0;
  for (std::uint32_t i = 0; i <= n && i < kRepeatSteps; ++i) {
    t += kRepeatGapMs[i];
  }
  return n < kRepeatSteps ? t : t + (n - (kRepeatSteps - 1)) * kRepeatGapMs[kRepeatSteps - 1];
}

// How many repeats are due at `ms` after the press (the closed form beyond the table: no loop over a long stall).
constexpr std::uint32_t repeats_due(std::uint32_t ms) noexcept {
  std::uint32_t n = 0;
  while (n < kRepeatSteps && repeat_time_ms(n) <= ms) {
    ++n;
  }
  return n < kRepeatSteps ? n : n + (ms - repeat_time_ms(kRepeatSteps - 1)) / kRepeatGapMs[kRepeatSteps - 1];
}

}  // namespace

void GestureRecognizer::reset(bool keep_held) noexcept {
  unsigned kept = 0;
  if (keep_held) {
    for (unsigned i = 0; i < count_; ++i) {
      const Entry& e = entries_[i];
      // Still down and still meaningful: a fired hold, or a modifier (a released one, WaitDouble, is no longer held), the bound ones (kWantModifier) included.
      if (e.state == State::Held || (e.state != State::WaitDouble && (is_modifier(e.id) || (e.wants & kWantModifier) != 0))) {
        entries_[kept++] = e;
      }
    }
  }
  count_ = kept;
  started_ = false;
}

void GestureRecognizer::feed(const InputEvent& event, const GestureHints& hints, GestureList& out) noexcept {
  advance(event.timestamp, out);
  const Tick t = now_;  // the event's time, or the newest one seen if the event is older
  switch (event.kind) {
    case InputKind::Button:
      if (event.value != 0) {
        press(event.id, t, hints, out);
      } else {
        release(event.id, t, true, out);
      }
      break;
    case InputKind::Pad:
      if (event.value > 0) {  // a pad release is not a gesture
        hit_pad(event.id, event.value, t, out);
      }
      break;
    case InputKind::Encoder:
      turn(event.id, event.value, t, hints, out);
      break;
    default:  // reserved kinds: nothing produces them yet
      break;
  }
}

void GestureRecognizer::advance(Tick now, GestureList& out) noexcept {
  if (started_ && !tick_reached(now, now_)) {
    return;  // older than what has been seen: time does not run backwards
  }
  for (;;) {
    int best = -1;
    Tick best_deadline = 0;
    for (unsigned i = 0; i < count_; ++i) {
      Tick d = 0;
      if (!deadline_of(entries_[i], d) || !tick_reached(now, d)) {
        continue;
      }
      // The earliest deadline is the one furthest in the past; ties go to the older press.
      if (best < 0 || tick_elapsed(now, d) > tick_elapsed(now, best_deadline)) {
        best = static_cast<int>(i);
        best_deadline = d;
      }
    }
    if (best < 0) {
      break;
    }
    fire(static_cast<unsigned>(best), best_deadline, now, out);  // every firing ends that entry's wait (a repeat moves its deadline past `now`): bounded by kMaxHeld
  }
  now_ = now;
  started_ = true;
}

bool GestureRecognizer::deadline_of(const Entry& e, Tick& deadline) const noexcept {
  switch (e.state) {
    case State::WaitDouble:
      deadline = e.t1 + ticks_from_ms(timings_.double_tap_ms);
      return true;
    case State::Down:
      if ((e.wants & kWantHold) != 0) {
        deadline = e.t0 + ticks_from_ms(timings_.hold_ms);
        return true;
      }
      if ((e.wants & kWantChord) != 0 && (e.wants & (kWantDoubleTap | kWantHoldEncoder)) == 0) {
        deadline = e.t0 + ticks_from_ms(timings_.chord_ms);  // the chord window is over: it was a plain tap
        return true;
      }
      return false;
    case State::Decided:
      if (e.repeat) {
        deadline = e.t0 + ticks_from_ms(repeat_time_ms(e.reps));
        return true;
      }
      break;
    case State::Held:
      break;
  }
  return false;
}

void GestureRecognizer::fire(unsigned index, Tick deadline, Tick now, GestureList& out) noexcept {
  Entry& e = entries_[index];
  const InputRef self = button_ref(e.id);
  if (e.state == State::Decided && e.repeat) {  // a repeat: one Tap, then on to the first one still ahead (the ones a stall skipped are not given)
    if (e.mod != kNoMod && find(e.mod) < 0) {
      e.repeat = false;  // the modifier it was pressed under (Shift + arrow) has been let go: the repeats end, they do not turn into another gesture
      return;
    }
    emit(out, deadline, GestureKind::Tap, self, modifier_ref(e.mod), 0, true);
    const std::uint32_t due = repeats_due(tick_elapsed(now, e.t0) / kTicksPerMs);
    e.reps = due > e.reps + 1 ? due : e.reps + 1;
    return;
  }
  if (e.state == State::WaitDouble) {
    emit(out, deadline, GestureKind::Tap, self, modifier_ref(e.mod), 0);
    remove(index);
    return;
  }
  const bool hold = (e.wants & kWantHold) != 0;
  emit(out, deadline, hold ? GestureKind::Hold : GestureKind::Tap, self, modifier_ref(e.mod), 0);
  e.state = hold ? State::Held : State::Decided;
}

void GestureRecognizer::press(std::uint8_t id, Tick t, const GestureHints& hints, GestureList& out) noexcept {
  int i = find(id);
  if (i >= 0 && entries_[i].state != State::WaitDouble) {
    release(id, t, false, out);  // a press with no release in between: the release was lost, close the old press
    i = find(id);
  }
  settle_waiting(t, out, id);
  i = find(id);
  const InputRef self = button_ref(id);
  if (i >= 0) {  // the entry is waiting for exactly this: the second tap
    consume_modifiers(id);
    Entry& e = entries_[i];
    emit(out, t, GestureKind::DoubleTap, self, modifier_ref(e.mod), 0);
    e.state = State::Decided;
    e.t0 = t;
    return;
  }
  // A button waiting for a chord partner: is this the partner?
  if (hints.chord != nullptr) {
    for (unsigned k = 0; k < count_; ++k) {
      Entry& p = entries_[k];
      if (p.state != State::Down || (p.wants & kWantChord) == 0 ||
          tick_elapsed(t, p.t0) >= ticks_from_ms(timings_.chord_ms) ||
          !hints.chord(hints.ctx, button_ref(p.id), self)) {
        continue;
      }
      const InputRef first = button_ref(p.id);
      const bool self_greater = input_less(first, self);
      emit(out, t, GestureKind::Chord, self_greater ? self : first, self_greater ? first : self, 0);
      p.state = State::Decided;
      consume_modifiers(id);  // any other undecided bound modifier is consumed as before; the partner is decided already
      add(id, State::Decided, 0, kNoMod, t);
      return;
    }
  }
  // Only now: a bound modifier that is also half of a chord (Master + Insert, OS-MIXINSHOLD) must still be able to pair inside the chord window.
  consume_modifiers(id);
  settle_chords(t, out, id);  // another input arrived: nobody is waiting for a chord with it any more
  std::uint8_t wants = 0;
  if (hints.wants != nullptr) {
    wants = hints.wants(hints.ctx, self);
    if (is_modifier(id)) {
      wants = static_cast<std::uint8_t>(wants & ~kWantModifier);  // Shift is a modifier already and keeps its own Tap and Hold timing
    }
  }
  const std::uint8_t mod = held_modifier();
  if ((wants & kWaits) == 0) {
    emit(out, t, GestureKind::Tap, self, modifier_ref(mod), 0);
    add(id, State::Decided, wants, mod, t);
    entries_[count_ - 1].repeat = id < 64 && ((repeats_ >> id) & 1u) != 0;
  } else {
    add(id, State::Down, wants, mod, t);
  }
}

void GestureRecognizer::release(std::uint8_t id, Tick t, bool allow_double, GestureList& out) noexcept {
  const int i = find(id);
  if (i < 0) {
    return;  // a release for a button never seen down
  }
  Entry& e = entries_[i];
  if (e.state == State::WaitDouble) {
    return;  // already released: a bounce or a duplicate must not lose the Tap that is waiting
  }
  if (e.state == State::Down) {
    if (allow_double && (e.wants & kWantDoubleTap) != 0) {
      e.state = State::WaitDouble;
      e.t1 = t;
      return;
    }
    emit(out, t, GestureKind::Tap, button_ref(id), modifier_ref(e.mod), 0);
  } else if (e.state == State::Held) {
    emit(out, t, GestureKind::HoldEnd, button_ref(id), modifier_ref(e.mod), 0);  // the hold is over
  }
  remove(static_cast<unsigned>(i));
}

void GestureRecognizer::hit_pad(std::uint8_t id, std::int16_t velocity, Tick t, GestureList& out) noexcept {
  settle_waiting(t, out, kNoMod);
  settle_chords(t, out, kNoMod);
  emit(out, t, GestureKind::Tap, InputRef{InputKind::Pad, id}, modifier_ref(held_modifier()), velocity);
}

void GestureRecognizer::turn(std::uint8_t id, std::int16_t delta, Tick t, const GestureHints& hints,
                             GestureList& out) noexcept {
  if (delta == 0) {
    return;
  }
  const InputRef encoder{InputKind::Encoder, id};
  if (hints.hold_encoder != nullptr) {
    for (unsigned k = count_; k-- > 0;) {  // the most recently pressed button first
      Entry& e = entries_[k];
      if (e.state == State::WaitDouble || !hints.hold_encoder(hints.ctx, button_ref(e.id), encoder)) {
        continue;
      }
      const std::uint8_t held = e.id;
      // A button still waiting (Down) is consumed: it will give neither Tap nor Hold. One that tapped at its
      // press is already Decided, one whose Hold is out stays Held (its release still ends the hold).
      if (e.state == State::Down) {
        e.state = State::Decided;
      }
      settle_waiting(t, out, held);
      settle_chords(t, out, held);
      emit(out, t, GestureKind::HoldEncoder, encoder, button_ref(held), delta);
      return;
    }
  }
  settle_waiting(t, out, kNoMod);
  settle_chords(t, out, kNoMod);
  emit(out, t, GestureKind::Tap, encoder, modifier_ref(held_modifier()), delta);
}

// The press of another button ends the wait of a bound modifier (Note on Pattern) that is still undecided: it was held to modify, so it gives neither Tap nor Hold.
// Shift does not count: Shift pressed after Note leaves Note intact (Shift + Note is Shift first).
void GestureRecognizer::consume_modifiers(std::uint8_t pressed) noexcept {
  if (is_modifier(pressed)) {
    return;
  }
  for (unsigned i = 0; i < count_; ++i) {
    Entry& e = entries_[i];
    if (e.state == State::Down && e.id != pressed && (e.wants & kWantModifier) != 0) {
      e.state = State::Decided;
    }
  }
}

// A second tap can no longer come for a button that another input has interrupted: its Tap goes out now.
void GestureRecognizer::settle_waiting(Tick t, GestureList& out, std::uint8_t except) noexcept {
  for (unsigned i = 0; i < count_;) {
    const Entry& e = entries_[i];
    if (e.state == State::WaitDouble && e.id != except) {
      emit(out, t, GestureKind::Tap, button_ref(e.id), modifier_ref(e.mod), 0);
      remove(i);
    } else {
      ++i;
    }
  }
}

// A button that was waiting only for a chord partner is a plain tap once another input has arrived.
void GestureRecognizer::settle_chords(Tick t, GestureList& out, std::uint8_t except) noexcept {
  for (unsigned i = 0; i < count_; ++i) {
    Entry& e = entries_[i];
    // A Shift that joins keeps a bound modifier waiting (Master held, Shift, then the encoder or Insert: OS-MIXINSHOLD); the context change it makes already dropped every other waiter.
    if (e.state == State::Down && e.id != except && (e.wants & kWantChord) != 0 &&
        (e.wants & (kWantHold | kWantDoubleTap)) == 0 && !((e.wants & kWantModifier) != 0 && is_modifier(except))) {
      emit(out, t, GestureKind::Tap, button_ref(e.id), modifier_ref(e.mod), 0);
      e.state = State::Decided;
    }
  }
}

int GestureRecognizer::find(std::uint8_t id) const noexcept {
  for (unsigned i = 0; i < count_; ++i) {
    if (entries_[i].id == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void GestureRecognizer::add(std::uint8_t id, State state, std::uint8_t wants, std::uint8_t mod, Tick t) noexcept {
  if (count_ == kMaxHeld) {  // more fingers than the table holds: forget a settled press, else the oldest
    unsigned victim = 0;
    for (unsigned i = 0; i < count_; ++i) {
      if (entries_[i].state == State::Decided) {
        victim = i;
        break;
      }
    }
    remove(victim);
  }
  entries_[count_++] = Entry{t, t, id, state, wants, mod};
}

void GestureRecognizer::remove(unsigned index) noexcept {
  for (unsigned i = index + 1; i < count_; ++i) {
    entries_[i - 1] = entries_[i];
  }
  --count_;
}

// The declared modifier (Shift) wins over a bound one (Note); among equals the latest press.
std::uint8_t GestureRecognizer::held_modifier() const noexcept {
  std::uint8_t bound = kNoMod;
  for (unsigned i = count_; i-- > 0;) {
    const Entry& e = entries_[i];
    if (e.state == State::WaitDouble) {
      continue;
    }
    if (is_modifier(e.id)) {
      return e.id;
    }
    if (bound == kNoMod && (e.wants & kWantModifier) != 0) {
      bound = e.id;
    }
  }
  return bound;
}

}  // namespace o3os::core
