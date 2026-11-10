// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/core/binding.h>
#include <o3os/core/gesture.h>
#include <o3os/core/input_event.h>

#include <cstdint>
#include <vector>

using namespace o3os::core;

// This tree builds without exceptions, so doctest's REQUIRE is unavailable: check, and leave the test case if the
// size is wrong (the checks after it index into the container).
#define NEED_SIZE(container, n)         \
  do {                                  \
    CHECK((container).size() == (n));   \
    if ((container).size() != (n)) {    \
      return;                           \
    }                                   \
  } while (0)

namespace {

constexpr std::uint8_t kA = 1;
constexpr std::uint8_t kB = 2;
constexpr std::uint8_t kC = 3;
constexpr std::uint8_t kShift = 29;
constexpr std::uint64_t kShiftMask = std::uint64_t{1} << kShift;

constexpr InputRef btn(std::uint8_t id) { return InputRef{InputKind::Button, id}; }
constexpr InputRef pad(std::uint8_t id) { return InputRef{InputKind::Pad, id}; }
constexpr InputRef enc(std::uint8_t id) { return InputRef{InputKind::Encoder, id}; }

constexpr Tick ms(std::uint32_t n) { return ticks_from_ms(n); }

// What the "bindings" want, per button, and which pairs are chords.
struct Wants {
  std::uint8_t of[64] = {};
  bool pair_ab = false;  // chord(A, B) is bound
  bool pair_bc = false;  // chord(B, C) is bound
  bool holdenc[64] = {};  // "encoder 0 turned while this button is held" is bound
};

std::uint8_t wants_fn(const void* ctx, InputRef in) noexcept {
  const Wants& w = *static_cast<const Wants*>(ctx);
  return in.kind == InputKind::Button && in.id < 64 ? w.of[in.id] : std::uint8_t{0};
}

bool chord_fn(const void* ctx, InputRef a, InputRef b) noexcept {
  const Wants& w = *static_cast<const Wants*>(ctx);
  const Trigger t = chord(a, b);
  return (w.pair_ab && t == chord(btn(kA), btn(kB))) || (w.pair_bc && t == chord(btn(kB), btn(kC)));
}

bool hold_encoder_fn(const void* ctx, InputRef button, InputRef encoder) noexcept {
  const Wants& w = *static_cast<const Wants*>(ctx);
  return button.kind == InputKind::Button && button.id < 64 && w.holdenc[button.id] &&
         encoder.kind == InputKind::Encoder && encoder.id == 0;  // only encoder 0 is bound
}

}  // namespace

namespace {

struct Rig {
  GestureRecognizer rec;
  Wants w;
  std::vector<Gesture> log;

  explicit Rig(std::uint64_t modifiers = 0, const GestureTimings& t = kGestureTimings) : rec(modifiers, t) {}

  GestureHints hints() { return GestureHints{&w, wants_fn, chord_fn, hold_encoder_fn}; }
  void take(const GestureList& out) {
    for (const Gesture& g : out) {
      log.push_back(g);
    }
  }
  void feed(const InputEvent& e) {
    GestureList out;
    rec.feed(e, hints(), out);
    take(out);
  }
  void advance(Tick now) {
    GestureList out;
    rec.advance(now, out);
    take(out);
  }
  void press(std::uint32_t at_ms, std::uint8_t id) { feed(make_button_event(ms(at_ms), id, true)); }
  void release(std::uint32_t at_ms, std::uint8_t id) { feed(make_button_event(ms(at_ms), id, false)); }
  void hit(std::uint32_t at_ms, std::uint8_t id, std::int16_t velocity) {
    feed(make_pad_event(ms(at_ms), id, velocity));
  }
  void turn(std::uint32_t at_ms, std::uint8_t id, std::int16_t delta) {
    feed(make_encoder_event(ms(at_ms), id, delta));
  }
  void clear() { log.clear(); }
};

// One gesture, fully described.
bool is(const Gesture& g, GestureKind kind, InputRef input, InputRef modifier, std::uint32_t at_ms,
        std::int16_t value = 0) {
  return g.kind == kind && g.input == input && g.modifier == modifier && g.timestamp == ms(at_ms) &&
         g.value == value;
}

}  // namespace

TEST_CASE("gesture: a button nobody waits on taps at the press and its release is silent") {
  Rig r;
  r.press(10, kA);
  NEED_SIZE(r.log, 1u);
  CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 10));
  r.release(500, kA);
  r.advance(ms(5000));
  CHECK(r.log.size() == 1u);  // no hold, no double tap: nothing waits
  CHECK(r.rec.held() == 0u);
}

TEST_CASE("gesture: a declared modifier is carried by the next tap, pad hit and encoder turn") {
  Rig r(kShiftMask);
  r.press(0, kShift);
  NEED_SIZE(r.log, 1u);
  CHECK(is(r.log[0], GestureKind::Tap, btn(kShift), kNoInput, 0));  // a modifier is a button too
  r.press(5, kA);
  r.hit(6, 7, 100);
  r.turn(7, 0, -2);
  NEED_SIZE(r.log, 4u);
  CHECK(is(r.log[1], GestureKind::Tap, btn(kA), btn(kShift), 5));
  CHECK(is(r.log[2], GestureKind::Tap, pad(7), btn(kShift), 6, 100));
  CHECK(is(r.log[3], GestureKind::Tap, enc(0), btn(kShift), 7, -2));
  r.release(8, kShift);
  r.press(9, kB);
  CHECK(is(r.log[4], GestureKind::Tap, btn(kB), kNoInput, 9));  // released: no longer a modifier
  // A button that is held but not declared as a modifier does not modify.
  r.press(10, kC);
  CHECK(is(r.log[5], GestureKind::Tap, btn(kC), kNoInput, 10));
}

TEST_CASE("gesture: tap versus hold at the threshold") {
  Rig r;
  r.w.of[kA] = kWantHold;

  SUBCASE("released one millisecond early: a tap, delivered at the release") {
    r.press(0, kA);
    CHECK(r.log.empty());
    r.release(399, kA);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 399));
    r.advance(ms(10000));
    CHECK(r.log.size() == 1u);
  }
  SUBCASE("released exactly at the threshold: a hold, and the release ends it (HoldEnd)") {
    r.press(0, kA);
    r.release(400, kA);
    NEED_SIZE(r.log, 2u);
    CHECK(is(r.log[0], GestureKind::Hold, btn(kA), kNoInput, 400));
    CHECK(is(r.log[1], GestureKind::HoldEnd, btn(kA), kNoInput, 400));
  }
  SUBCASE("still down: the hold comes with the deadline's time, however late anyone looks") {
    r.press(0, kA);
    r.advance(ms(399));
    CHECK(r.log.empty());
    r.advance(ms(1000));
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Hold, btn(kA), kNoInput, 400));
    r.advance(ms(60000));
    CHECK(r.log.size() == 1u);  // fires once, however often anyone looks
    r.release(61000, kA);
    NEED_SIZE(r.log, 2u);       // the release only ends the hold, at its own time
    CHECK(is(r.log[1], GestureKind::HoldEnd, btn(kA), kNoInput, 61000));
  }
  SUBCASE("the timings table is honoured") {
    Rig quick(0, GestureTimings{100, 50, 10});
    quick.w.of[kA] = kWantHold;
    quick.press(0, kA);
    quick.advance(ms(99));
    CHECK(quick.log.empty());
    quick.advance(ms(100));
    CHECK(quick.log.size() == 1u);
  }
}

TEST_CASE("gesture: double tap window edges") {
  Rig r;
  r.w.of[kA] = kWantDoubleTap;

  SUBCASE("a lone tap is delivered when the window has passed, not before") {
    r.press(0, kA);
    r.release(50, kA);
    r.advance(ms(349));
    CHECK(r.log.empty());
    r.advance(ms(350));
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 350));  // release + 300 ms
    r.advance(ms(5000));
    CHECK(r.log.size() == 1u);
  }
  SUBCASE("a second press one millisecond inside the window is a double tap, with no tap at all") {
    r.press(0, kA);
    r.release(50, kA);
    r.press(349, kA);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::DoubleTap, btn(kA), kNoInput, 349));
    r.release(380, kA);
    r.advance(ms(5000));
    CHECK(r.log.size() == 1u);
  }
  SUBCASE("a second press exactly at the end of the window is a new press: the first tap comes out first") {
    r.press(0, kA);
    r.release(50, kA);
    r.press(350, kA);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 350));
    r.release(360, kA);  // and the new press waits for its own second tap
    r.advance(ms(659));
    CHECK(r.log.size() == 1u);
    r.advance(ms(660));
    CHECK(r.log.size() == 2u);
  }
  SUBCASE("a duplicate or bounced release while waiting does not lose the tap") {
    r.press(0, kA);
    r.release(50, kA);
    r.release(60, kA);  // a bounce, or the same release twice
    r.advance(ms(349));
    CHECK(r.log.empty());
    r.advance(ms(350));
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 350));  // the window still ends 300 ms after the first release
  }
  SUBCASE("another input in between ends the wait: the older tap goes out first, in order") {
    r.press(0, kA);
    r.release(50, kA);
    r.press(100, kB);
    NEED_SIZE(r.log, 2u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 100));
    CHECK(is(r.log[1], GestureKind::Tap, btn(kB), kNoInput, 100));
    r.press(120, kA);  // A again: a fresh first press, not a double tap
    r.advance(ms(10000));
    CHECK(r.log.size() == 2u);
    r.release(10001, kA);
    r.advance(ms(11000));
    CHECK(is(r.log[2], GestureKind::Tap, btn(kA), kNoInput, 10301));
  }
  SUBCASE("a pad hit or an encoder turn ends the wait too") {
    r.press(0, kA);
    r.release(10, kA);
    r.hit(20, 3, 127);
    NEED_SIZE(r.log, 2u);
    CHECK(r.log[0].kind == GestureKind::Tap);
    CHECK(r.log[0].input == btn(kA));
    CHECK(r.log[1].input == pad(3));
  }
  SUBCASE("hold and double tap wanted together: a long press is a hold, a short pair a double tap") {
    r.w.of[kA] = kWantHold | kWantDoubleTap;
    r.press(0, kA);
    r.advance(ms(400));
    NEED_SIZE(r.log, 1u);
    CHECK(r.log[0].kind == GestureKind::Hold);
    r.release(500, kA);
    r.advance(ms(2000));
    NEED_SIZE(r.log, 2u);  // a hold is not also the first tap of a double tap: its release only ends it
    CHECK(r.log[1].kind == GestureKind::HoldEnd);
    r.clear();
    r.press(3000, kA);
    r.release(3050, kA);
    r.press(3100, kA);
    NEED_SIZE(r.log, 1u);
    CHECK(r.log[0].kind == GestureKind::DoubleTap);
  }
}

TEST_CASE("gesture: chords") {
  Rig r;
  r.w.of[kA] = kWantChord;
  r.w.of[kB] = kWantChord;
  r.w.pair_ab = true;

  SUBCASE("two buttons within the window: one chord, in canonical order, whichever came first") {
    r.press(0, kA);
    CHECK(r.log.empty());  // held back: a partner may follow
    r.press(39, kB);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Chord, btn(kB), btn(kA), 39));  // input = the greater, modifier = the lesser
    r.release(100, kA);
    r.release(110, kB);
    r.advance(ms(5000));
    CHECK(r.log.size() == 1u);  // neither button also taps

    Rig reversed;
    reversed.w = r.w;
    reversed.press(0, kB);
    reversed.press(10, kA);
    NEED_SIZE(reversed.log, 1u);
    CHECK(is(reversed.log[0], GestureKind::Chord, btn(kB), btn(kA), 10));
  }
  SUBCASE("the window's last millisecond: 39 is a chord, 40 is two taps") {
    r.press(0, kA);
    r.press(40, kB);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 40));  // A's window closed at 40, its tap at the deadline
    r.advance(ms(80));
    NEED_SIZE(r.log, 2u);
    CHECK(is(r.log[1], GestureKind::Tap, btn(kB), kNoInput, 80));
  }
  SUBCASE("a chord button alone: a tap when the window ends, or at the release if that comes first") {
    r.press(0, kA);
    r.advance(ms(39));
    CHECK(r.log.empty());
    r.advance(ms(40));
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 40));
    r.release(500, kA);
    CHECK(r.log.size() == 1u);

    r.clear();
    r.press(1000, kB);
    r.release(1010, kB);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kB), kNoInput, 1010));
  }
  SUBCASE("released before the other is pressed: not a chord") {
    r.press(0, kA);
    r.release(10, kA);
    r.press(20, kB);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 10));
  }
  SUBCASE("a pair that is not bound is not a chord: the first button taps before the second") {
    r.w.pair_ab = false;
    r.press(0, kA);
    r.press(10, kB);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 10));
  }
  SUBCASE("any other input ends the wait and the older tap goes first") {
    r.press(0, kA);
    r.press(10, kC);  // C wants nothing
    NEED_SIZE(r.log, 2u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 10));
    CHECK(is(r.log[1], GestureKind::Tap, btn(kC), kNoInput, 10));
    r.clear();
    r.release(20, kA);
    r.release(21, kC);
    r.press(100, kB);
    r.hit(110, 4, 90);
    NEED_SIZE(r.log, 2u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kB), kNoInput, 110));
    CHECK(r.log[1].input == pad(4));
  }
  SUBCASE("a button that also wants a hold keeps waiting for it; a chord still wins inside the window") {
    r.w.of[kA] = kWantChord | kWantHold;
    r.press(0, kA);
    r.advance(ms(100));
    CHECK(r.log.empty());  // the chord window passed silently, the hold is still pending
    r.advance(ms(400));
    NEED_SIZE(r.log, 1u);
    CHECK(r.log[0].kind == GestureKind::Hold);

    r.release(900, kA);  // (this release ends the hold: HoldEnd)
    r.clear();
    r.press(1000, kA);
    r.press(1010, kB);
    NEED_SIZE(r.log, 1u);
    CHECK(r.log[0].kind == GestureKind::Chord);
    r.advance(ms(5000));
    CHECK(r.log.size() == 1u);  // consumed: no hold afterwards
  }
  SUBCASE("with a hold wanted as well, the window still ends at chord_ms: 39 is a chord, 40 is not") {
    r.w.of[kA] = kWantChord | kWantHold;
    r.press(0, kA);
    r.press(39, kB);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Chord, btn(kB), btn(kA), 39));

    Rig late;
    late.w = r.w;
    late.press(0, kA);
    late.press(40, kB);  // too late: nothing for A to do yet (its hold is still pending), B waits for its own window
    CHECK(late.log.empty());
    late.advance(ms(80));
    NEED_SIZE(late.log, 1u);
    CHECK(is(late.log[0], GestureKind::Tap, btn(kB), kNoInput, 80));
    late.advance(ms(400));
    NEED_SIZE(late.log, 2u);
    CHECK(is(late.log[1], GestureKind::Hold, btn(kA), kNoInput, 400));
  }
}

TEST_CASE("gesture: an encoder turn with a button held") {
  Rig r;

  SUBCASE("a held button with a HoldEncoder binding takes the turn; its release is silent") {
    r.w.of[kA] = kWantHold;
    r.w.holdenc[kA] = true;
    r.press(0, kA);
    r.turn(100, 0, 3);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::HoldEncoder, enc(0), btn(kA), 100, 3));
    r.turn(110, 0, -1);
    NEED_SIZE(r.log, 2u);
    CHECK(is(r.log[1], GestureKind::HoldEncoder, enc(0), btn(kA), 110, -1));
    r.advance(ms(5000));
    r.release(5001, kA);
    CHECK(r.log.size() == 2u);  // no hold (consumed by the turn), no tap at the release
  }
  SUBCASE("without such a binding the turn is a plain encoder tap") {
    r.press(0, kB);
    r.turn(10, 0, 5);
    NEED_SIZE(r.log, 2u);
    CHECK(is(r.log[1], GestureKind::Tap, enc(0), kNoInput, 10, 5));
  }
  SUBCASE("a button waiting for a chord that takes the turn does not also tap") {
    r.w.of[kA] = kWantChord;
    r.w.holdenc[kA] = true;
    r.press(0, kA);
    r.turn(5, 0, 1);
    r.release(10, kA);
    r.advance(ms(1000));
    NEED_SIZE(r.log, 1u);
    CHECK(r.log[0].kind == GestureKind::HoldEncoder);
  }
  SUBCASE("the most recently pressed interested button wins over a modifier") {
    Rig s(kShiftMask);
    s.w.holdenc[kA] = true;
    s.press(0, kShift);
    s.press(5, kA);
    s.turn(10, 0, 2);
    CHECK(is(s.log.back(), GestureKind::HoldEncoder, enc(0), btn(kA), 10, 2));
    s.release(20, kA);
    s.turn(30, 0, 1);  // only Shift is held now: it modifies a plain tap
    CHECK(is(s.log.back(), GestureKind::Tap, enc(0), btn(kShift), 30, 1));
  }
  SUBCASE("a turn of an encoder nobody bound is a plain tap, and the held button is not consumed") {
    r.w.of[kA] = kWantHold;
    r.w.holdenc[kA] = true;  // bound for encoder 0 only
    r.press(0, kA);
    r.turn(10, 1, 2);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, enc(1), kNoInput, 10, 2));
    r.advance(ms(400));  // A is still waiting for its hold, and gets it
    NEED_SIZE(r.log, 2u);
    CHECK(r.log[1].kind == GestureKind::Hold);
  }
  SUBCASE("a button that tapped at its press is not consumed twice: the turn still takes it, its release is silent") {
    r.w.holdenc[kB] = true;  // wants nothing else, so it taps at the press and is Decided
    r.press(0, kB);
    r.turn(5, 0, 1);
    NEED_SIZE(r.log, 2u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kB), kNoInput, 0));
    CHECK(is(r.log[1], GestureKind::HoldEncoder, enc(0), btn(kB), 5, 1));
    r.release(10, kB);
    CHECK(r.log.size() == 2u);
  }
  SUBCASE("a button that wants the turn waits for its release: no tap at the press, none after the chord window, none after a turn") {
    r.w.of[kA] = kWantHoldEncoder;
    r.w.holdenc[kA] = true;
    r.press(0, kA);
    r.advance(ms(1000));
    CHECK(r.log.empty());  // nothing is decided while it is held
    r.release(1001, kA);   // let go without turning: the tap, at the release
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 1001));
    r.clear();
    r.press(2000, kA);
    r.turn(2100, 0, 3);    // turned: the turn takes it, and it is neither a tap nor a hold
    r.release(2200, kA);
    r.advance(ms(5000));
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::HoldEncoder, enc(0), btn(kA), 2100, 3));
  }
  SUBCASE("a button in a chord and with a turn bound taps at the release, not when the chord window is over") {
    r.w.of[kA] = kWantChord | kWantHoldEncoder;
    r.w.holdenc[kA] = true;
    r.press(0, kA);
    r.advance(ms(500));  // far past the 40 ms of the chord window
    CHECK(r.log.empty());
    r.turn(600, 0, 1);
    r.release(700, kA);
    NEED_SIZE(r.log, 1u);
    CHECK(r.log[0].kind == GestureKind::HoldEncoder);
    // Another input still settles a chord-only wait, as before: a pad hit gives the held button its tap first.
    r.clear();
    r.press(1000, kA);
    r.hit(1010, 3, 100);
    NEED_SIZE(r.log, 2u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 1010));
    CHECK(r.log[1].kind == GestureKind::Tap);
  }
  SUBCASE("a turn of zero detents is nothing") {
    r.turn(0, 0, 0);
    CHECK(r.log.empty());
  }
}

TEST_CASE("gesture: pads give a tap with their velocity, releases are not gestures") {
  Rig r;
  r.hit(0, 12, 127);
  r.hit(1, 12, 0);
  r.hit(2, 47, 1);
  NEED_SIZE(r.log, 2u);
  CHECK(is(r.log[0], GestureKind::Tap, pad(12), kNoInput, 0, 127));
  CHECK(is(r.log[1], GestureKind::Tap, pad(47), kNoInput, 2, 1));
}

TEST_CASE("gesture: reserved kinds, stray releases and lost releases") {
  Rig r;
  r.feed(InputEvent{0, InputKind::PcKey, 4, 1});
  r.feed(InputEvent{0, InputKind::MidiNote, 60, 100});
  r.feed(InputEvent{0, InputKind::None, 0, 1});
  r.release(5, kA);  // never pressed
  CHECK(r.log.empty());
  CHECK(r.rec.held() == 0u);

  r.press(10, kA);
  r.press(20, kA);  // the release was lost: the button is simply pressed again
  CHECK(r.log.size() == 2u);
  CHECK(r.rec.held() == 1u);
  r.release(30, kA);
  CHECK(r.rec.held() == 0u);
}

TEST_CASE("gesture: reset forgets held buttons and pending waits without emitting anything") {
  Rig r(kShiftMask);
  r.w.of[kA] = kWantDoubleTap;
  r.press(0, kShift);  // its release is "lost"
  r.press(10, kA);
  r.release(20, kA);   // A waits for a second tap
  CHECK(r.rec.held() == 2u);
  r.clear();
  r.rec.reset();
  CHECK(r.rec.held() == 0u);
  r.advance(ms(5000));
  CHECK(r.log.empty());  // the pending tap is dropped, not delivered late
  r.press(6000, kB);
  NEED_SIZE(r.log, 1u);
  CHECK(is(r.log[0], GestureKind::Tap, btn(kB), kNoInput, 6000));  // no Shift stuck to it
  r.turn(6001, 0, 1);
  CHECK(r.log.back().modifier == kNoInput);

  // After a reset time may also start again from an earlier tick (a new run): no stale "newest time seen".
  Rig fresh;
  fresh.press(5000, kB);
  fresh.rec.reset();
  fresh.log.clear();
  fresh.press(10, kB);
  NEED_SIZE(fresh.log, 1u);
  CHECK(is(fresh.log[0], GestureKind::Tap, btn(kB), kNoInput, 10));
}

TEST_CASE("gesture: time never runs backwards") {
  Rig r;
  r.w.of[kA] = kWantHold;
  r.press(1000, kA);
  r.release(500, kA);  // stamped before the press: taken as the newest time seen, 1000
  NEED_SIZE(r.log, 1u);
  CHECK(is(r.log[0], GestureKind::Tap, btn(kA), kNoInput, 1000));
  r.advance(ms(10));  // an older "now" changes nothing
  CHECK(r.log.size() == 1u);
}

TEST_CASE("gesture: timers survive the wrap of the 32-bit tick") {
  Rig r;
  r.w.of[kA] = kWantHold;
  const Tick start = 0xFFFFFFFFu - ms(150);  // 150 ms before the counter wraps
  r.feed(make_button_event(start, kA, true));
  r.advance(start + ms(399));
  CHECK(r.log.empty());
  r.advance(start + ms(400));  // past the wrap
  NEED_SIZE(r.log, 1u);
  CHECK(r.log[0].kind == GestureKind::Hold);
  CHECK(r.log[0].timestamp == static_cast<Tick>(start + ms(400)));
}

TEST_CASE("gesture: several holds fire in deadline order and fit the gesture list") {
  Rig r;
  for (std::uint8_t id = 10; id < 16; ++id) {
    r.w.of[id] = kWantHold;
  }
  for (std::uint8_t i = 0; i < 6; ++i) {
    r.press(i * 10u, static_cast<std::uint8_t>(10 + i));
  }
  CHECK(r.rec.held() == 6u);
  GestureList out;
  r.rec.advance(ms(10000), out);
  NEED_SIZE(out, 6u);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(is(out[i], GestureKind::Hold, btn(static_cast<std::uint8_t>(10 + i)), kNoInput, 400 + i * 10u));
  }
}

TEST_CASE("gesture: more buttons than it can track forget the oldest and do not crash") {
  Rig r;
  for (std::uint8_t id = 0; id < 10; ++id) {
    r.press(id, id);
  }
  CHECK(r.log.size() == 10u);  // every press still gave its tap
  CHECK(r.rec.held() <= GestureRecognizer::kMaxHeld);
}

TEST_CASE("gesture: how often time is looked at does not change what is recognised") {
  // A scripted sequence of presses, releases, pads and turns, run twice: once with advance() every millisecond,
  // once only through the events' own timestamps. The gestures, their order and their timestamps must match.
  struct Step {
    std::uint32_t at_ms;
    int what;  // 0 press, 1 release, 2 pad, 3 turn
    std::uint8_t id;
  };
  std::vector<Step> script;
  std::uint32_t lcg = 12345;
  const auto next = [&] {
    lcg = lcg * 1664525u + 1013904223u;
    return lcg >> 8;
  };
  std::uint32_t t = 0;
  bool down[4] = {};
  for (int i = 0; i < 600; ++i) {
    t += 1 + next() % 130;
    const unsigned pick = next() % 8;
    if (pick < 5) {
      const std::uint8_t id = static_cast<std::uint8_t>(1 + pick % 4);  // buttons 1..4
      script.push_back(Step{t, down[id - 1] ? 1 : 0, id});
      down[id - 1] = !down[id - 1];
    } else if (pick == 5) {
      script.push_back(Step{t, 2, static_cast<std::uint8_t>(next() % 48)});
    } else {
      script.push_back(Step{t, 3, 0});
    }
  }

  const auto run = [&](bool tick_every_ms) {
    Rig r(std::uint64_t{1} << 4);  // button 4 is a modifier
    r.w.of[1] = kWantHold | kWantDoubleTap;
    r.w.of[2] = kWantChord;
    r.w.of[3] = kWantChord | kWantHold;
    r.w.holdenc[3] = true;
    r.w.pair_ab = true;
    r.w.pair_bc = true;
    std::uint32_t now = 0;
    for (const Step& s : script) {
      if (tick_every_ms) {
        for (; now < s.at_ms; ++now) {
          r.advance(ms(now));
        }
      }
      switch (s.what) {
        case 0:
          r.press(s.at_ms, s.id);
          break;
        case 1:
          r.release(s.at_ms, s.id);
          break;
        case 2:
          r.hit(s.at_ms, s.id, 100);
          break;
        default:
          r.turn(s.at_ms, 0, 1);
          break;
      }
    }
    r.advance(ms(t + 100000));
    return r.log;
  };
  const std::vector<Gesture> every_ms = run(true);
  const std::vector<Gesture> events_only = run(false);
  NEED_SIZE(every_ms, events_only.size());
  CHECK(every_ms.size() > 100u);  // the script really does produce gestures of every kind
  bool seen[6] = {};  // Tap, Hold, DoubleTap, HoldEncoder, Chord, HoldEnd
  for (std::size_t i = 0; i < every_ms.size(); ++i) {
    const Gesture& a = every_ms[i];
    const Gesture& b = events_only[i];
    CHECK((a.timestamp == b.timestamp && a.kind == b.kind && a.input == b.input && a.modifier == b.modifier &&
           a.value == b.value));
    seen[static_cast<unsigned>(a.kind)] = true;
  }
  for (bool s : seen) {
    CHECK(s);
  }
}

// ---- OS-002b: HoldEnd, the release that ends a hold -------------------------------------------------------------------

TEST_CASE("gesture: the release of a button whose hold fired is HoldEnd, with the modifier of its press; others are silent") {
  Rig r(kShiftMask);
  r.w.of[kA] = kWantHold;

  SUBCASE("Shift held at the press: the Hold and its end carry it, whatever happens to Shift later") {
    r.press(0, kShift);
    r.press(10, kA);
    r.advance(ms(410));
    NEED_SIZE(r.log, 2u);  // Tap of Shift (unbound), Hold of A
    CHECK(is(r.log[1], GestureKind::Hold, btn(kA), btn(kShift), 410));
    r.release(500, kShift);
    r.release(900, kA);
    NEED_SIZE(r.log, 3u);
    CHECK(is(r.log[2], GestureKind::HoldEnd, btn(kA), btn(kShift), 900));  // the modifier of the press, not of the release
  }
  SUBCASE("a tap that was held back for the hold is not a hold: no HoldEnd") {
    r.press(0, kA);
    r.release(399, kA);
    r.advance(ms(5000));
    NEED_SIZE(r.log, 1u);
    CHECK(r.log[0].kind == GestureKind::Tap);
  }
  SUBCASE("a button that tapped at its press, a chord member and a consumed button release in silence") {
    r.press(0, kB);  // nobody waits on B
    r.release(5, kB);
    CHECK(r.log.size() == 1u);

    r.clear();
    r.w.of[kB] = kWantChord;
    r.w.of[kC] = kWantChord;
    r.w.pair_bc = true;
    r.press(100, kB);
    r.press(110, kC);  // the chord
    r.release(200, kC);
    r.release(210, kB);
    NEED_SIZE(r.log, 1u);
    CHECK(r.log[0].kind == GestureKind::Chord);
  }
  SUBCASE("an encoder turn bound with the held button does not lose the HoldEnd") {
    r.w.holdenc[kA] = true;
    r.press(0, kA);
    r.advance(ms(400));
    r.turn(500, 0, 2);
    r.release(900, kA);
    NEED_SIZE(r.log, 3u);
    CHECK(r.log[0].kind == GestureKind::Hold);
    CHECK(is(r.log[1], GestureKind::HoldEncoder, enc(0), btn(kA), 500, 2));
    CHECK(is(r.log[2], GestureKind::HoldEnd, btn(kA), kNoInput, 900));
  }
  SUBCASE("a press with no release in between closes the old hold with its HoldEnd, then starts afresh") {
    r.press(0, kA);
    r.advance(ms(400));
    r.press(1000, kA);  // the release was lost
    NEED_SIZE(r.log, 2u);
    CHECK(is(r.log[1], GestureKind::HoldEnd, btn(kA), kNoInput, 1000));
    r.advance(ms(1400));
    NEED_SIZE(r.log, 3u);
    CHECK(is(r.log[2], GestureKind::Hold, btn(kA), kNoInput, 1400));
  }
}

TEST_CASE("gesture: reset forgets everything, reset(true) spares the modifiers and the buttons whose hold has fired") {
  Rig r(kShiftMask);
  r.w.of[kA] = kWantHold;
  r.w.of[kB] = kWantHold;
  r.press(0, kShift);
  r.press(10, kA);
  r.advance(ms(410));  // A: Hold fired. Shift: tapped at its press
  r.press(500, kB);    // B: still waiting for its hold
  r.clear();

  SUBCASE("reset(true): Shift and A are still down and stay; the waiting B is forgotten") {
    r.rec.reset(true);
    CHECK(r.rec.held() == 2u);
    r.release(600, kB);
    CHECK(r.log.empty());  // forgotten: its release is for a button never seen down
    r.release(700, kA);    // the hold of A still ends with its modifier
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::HoldEnd, btn(kA), btn(kShift), 700));
    r.press(800, kC);      // Shift is still held: it modifies the next key
    NEED_SIZE(r.log, 2u);
    CHECK(is(r.log[1], GestureKind::Tap, btn(kC), btn(kShift), 800));
    r.release(900, kShift);
    r.press(1000, kC);     // and once released, it does not
    NEED_SIZE(r.log, 3u);
    CHECK(is(r.log[2], GestureKind::Tap, btn(kC), kNoInput, 1000));
    r.advance(ms(60000));  // B's hold never fires
    CHECK(r.log.size() == 3u);
  }
  SUBCASE("reset(): all of them are forgotten, the Held one and Shift too") {
    r.rec.reset();
    CHECK(r.rec.held() == 0u);
    r.release(700, kA);
    CHECK(r.log.empty());
    r.press(800, kC);
    NEED_SIZE(r.log, 1u);
    CHECK(is(r.log[0], GestureKind::Tap, btn(kC), kNoInput, 800));  // Shift no longer modifies
    r.advance(ms(60000));  // and B's hold never fires
    CHECK(r.log.size() == 1u);
  }
  SUBCASE("a modifier that has been released and is waiting for a second tap is not held, and is forgotten") {
    Rig s(kShiftMask);
    s.w.of[kShift] = kWantDoubleTap;  // the modifier itself has a double tap
    s.press(0, kShift);
    s.release(50, kShift);  // WaitDouble: released
    s.rec.reset(true);
    CHECK(s.rec.held() == 0u);
    s.clear();
    s.advance(ms(1000));
    CHECK(s.log.empty());  // its pending Tap is gone with it
  }
  SUBCASE("a modifier that is still waiting for its own hold goes on waiting, and a hold on Shift itself modifies") {
    Rig s(kShiftMask);
    s.w.of[kShift] = kWantHold;
    s.press(0, kShift);
    s.advance(ms(100));
    s.rec.reset(true);  // before its hold has fired
    CHECK(s.rec.held() == 1u);
    s.advance(ms(400));
    NEED_SIZE(s.log, 1u);
    CHECK(is(s.log[0], GestureKind::Hold, btn(kShift), kNoInput, 400));
    s.clear();
    s.press(500, kA);
    NEED_SIZE(s.log, 1u);
    CHECK(is(s.log[0], GestureKind::Tap, btn(kA), btn(kShift), 500));
    s.rec.reset();
    s.clear();
    s.press(600, kB);  // after a full reset it no longer does
    NEED_SIZE(s.log, 1u);
    CHECK(is(s.log[0], GestureKind::Tap, btn(kB), kNoInput, 600));
  }
}

// ---- OS-ZOOM: a bound modifier (kWantModifier): Note on Pattern. A is the Note button here: it has a Hold and is named as the modifier of other bindings. ----

namespace {
constexpr std::uint8_t kNoteBtn = kA;
constexpr std::uint8_t kModWants = kWantHold | kWantModifier;
}  // namespace

TEST_CASE("gesture: a bound modifier alone still taps at the release and holds at hold_ms") {
  Rig s(kShiftMask);
  s.w.of[kNoteBtn] = kModWants;
  s.press(0, kNoteBtn);
  CHECK(s.log.empty());  // its Tap waits for the release, like the hold-encoder buttons
  s.release(100, kNoteBtn);
  NEED_SIZE(s.log, 1u);
  CHECK(is(s.log[0], GestureKind::Tap, btn(kNoteBtn), kNoInput, 100));
  s.clear();
  s.press(1000, kNoteBtn);
  s.advance(ms(1500));
  NEED_SIZE(s.log, 1u);
  CHECK(is(s.log[0], GestureKind::Hold, btn(kNoteBtn), kNoInput, 1400));
  s.clear();
  s.release(1600, kNoteBtn);
  NEED_SIZE(s.log, 1u);
  CHECK(is(s.log[0], GestureKind::HoldEnd, btn(kNoteBtn), kNoInput, 1600));
}

TEST_CASE("gesture: another button pressed with a bound modifier is made under it and consumes its Tap and Hold") {
  Rig s(kShiftMask);
  s.w.of[kNoteBtn] = kModWants;
  s.press(0, kNoteBtn);
  s.press(50, kB);
  NEED_SIZE(s.log, 1u);
  CHECK(is(s.log[0], GestureKind::Tap, btn(kB), btn(kNoteBtn), 50));
  s.release(60, kB);
  s.advance(ms(2000));  // the Hold never fires
  s.release(2100, kNoteBtn);  // and the release is silent
  CHECK(s.log.size() == 1u);
  CHECK(s.rec.held() == 0u);
  // The modifier goes on modifying after the first key: Note + Right, Right again.
  s.clear();
  s.press(3000, kNoteBtn);
  s.press(3010, kB);
  s.release(3020, kB);
  s.press(3100, kB);
  NEED_SIZE(s.log, 2u);
  CHECK(is(s.log[0], GestureKind::Tap, btn(kB), btn(kNoteBtn), 3010));
  CHECK(is(s.log[1], GestureKind::Tap, btn(kB), btn(kNoteBtn), 3100));
}

TEST_CASE("gesture: a bound modifier whose Hold has fired is still a modifier, and its release is a HoldEnd") {
  Rig s(kShiftMask);
  s.w.of[kNoteBtn] = kModWants;
  s.press(0, kNoteBtn);
  s.advance(ms(500));
  NEED_SIZE(s.log, 1u);
  CHECK(is(s.log[0], GestureKind::Hold, btn(kNoteBtn), kNoInput, 400));
  s.clear();
  s.press(600, kB);
  NEED_SIZE(s.log, 1u);
  CHECK(is(s.log[0], GestureKind::Tap, btn(kB), btn(kNoteBtn), 600));
  s.release(650, kB);
  s.clear();
  s.release(700, kNoteBtn);
  NEED_SIZE(s.log, 1u);
  CHECK(is(s.log[0], GestureKind::HoldEnd, btn(kNoteBtn), kNoInput, 700));
}

TEST_CASE("gesture: a pad hit or an encoder turn under a bound modifier does not consume it") {
  Rig s(kShiftMask);
  s.w.of[kNoteBtn] = kModWants;
  s.press(0, kNoteBtn);
  s.hit(50, 3, 90);
  s.turn(60, 0, 2);
  NEED_SIZE(s.log, 2u);
  CHECK(is(s.log[0], GestureKind::Tap, pad(3), btn(kNoteBtn), 50, 90));
  CHECK(is(s.log[1], GestureKind::Tap, enc(0), btn(kNoteBtn), 60, 2));
  s.release(100, kNoteBtn);  // Note's own Tap still comes
  NEED_SIZE(s.log, 3u);
  CHECK(is(s.log[2], GestureKind::Tap, btn(kNoteBtn), kNoInput, 100));
}

TEST_CASE("gesture: Shift is not consumed by, and wins over, a bound modifier") {
  Rig s(kShiftMask);
  s.w.of[kNoteBtn] = kModWants;
  s.w.of[kShift] = kWantModifier;  // the bindings name Shift as a modifier too: the recogniser ignores that for its own modifiers
  s.press(0, kShift);
  CHECK(s.log.size() == 1u);  // Shift taps at its press as ever
  s.clear();
  // Shift pressed after Note leaves Note intact.
  Rig t(kShiftMask);
  t.w.of[kNoteBtn] = kModWants;
  t.press(0, kNoteBtn);
  t.press(50, kShift);
  t.clear();
  t.release(60, kShift);
  t.release(100, kNoteBtn);
  NEED_SIZE(t.log, 1u);
  CHECK(is(t.log[0], GestureKind::Tap, btn(kNoteBtn), kNoInput, 100));
  // Shift first: Shift + Note; and with both down the declared modifier is the one a key is made under.
  Rig u(kShiftMask);
  u.w.of[kNoteBtn] = kModWants;
  u.press(0, kShift);
  u.clear();
  u.press(10, kNoteBtn);
  u.release(60, kNoteBtn);
  NEED_SIZE(u.log, 1u);
  CHECK(is(u.log[0], GestureKind::Tap, btn(kNoteBtn), btn(kShift), 60));
  u.clear();
  u.press(100, kNoteBtn);
  u.press(110, kB);
  NEED_SIZE(u.log, 1u);
  CHECK(is(u.log[0], GestureKind::Tap, btn(kB), btn(kShift), 110));
}

TEST_CASE("gesture: reset(keep_held) keeps a bound modifier that is still down") {
  Rig s(kShiftMask);
  s.w.of[kNoteBtn] = kModWants;
  s.press(0, kNoteBtn);
  s.press(10, kB);  // consumes Note
  s.release(20, kB);
  s.clear();
  s.rec.reset(true);
  CHECK(s.rec.held() == 1u);
  s.press(100, kC);
  NEED_SIZE(s.log, 1u);
  CHECK(is(s.log[0], GestureKind::Tap, btn(kC), btn(kNoteBtn), 100));
  // An undecided one is kept too and still waits for its Hold.
  Rig t(kShiftMask);
  t.w.of[kNoteBtn] = kModWants;
  t.press(0, kNoteBtn);
  t.rec.reset(true);
  CHECK(t.rec.held() == 1u);
  t.advance(ms(400));
  NEED_SIZE(t.log, 1u);
  CHECK(is(t.log[0], GestureKind::Hold, btn(kNoteBtn), kNoInput, 400));
  // A full reset forgets it.
  t.rec.reset();
  t.clear();
  t.press(500, kC);
  NEED_SIZE(t.log, 1u);
  CHECK(is(t.log[0], GestureKind::Tap, btn(kC), kNoInput, 500));
}

// ---- OS-SONGSHARE S3: Copy on Song is a bound modifier with no Hold: A is the Copy button, B an arrow (a repeat button). Wants: the modifier bit alone. ----

TEST_CASE("gesture: a bound modifier with no Hold taps at the release however long it was held, an arrow under it carries it and its repeats keep it, and its own release is silent") {
  Rig s(kShiftMask);
  s.w.of[kA] = kWantModifier;  // Copy on Song: Copy + Up / Down are bound, Copy has no Hold or double tap
  s.rec.set_repeat_buttons(std::uint64_t{1} << kB);
  s.press(0, kA);
  CHECK(s.log.empty());  // its Tap waits for the release
  s.advance(ms(1000));
  CHECK(s.log.empty());  // and no Hold ever comes: nothing is bound to it
  s.release(1200, kA);
  NEED_SIZE(s.log, 1u);
  CHECK(is(s.log[0], GestureKind::Tap, btn(kA), kNoInput, 1200));
  // Copy + the arrow: the arrow is made under Copy and consumes Copy's Tap; held, its repeats keep Copy as the modifier.
  s.clear();
  s.press(2000, kA);
  s.press(2050, kB);
  NEED_SIZE(s.log, 1u);
  CHECK(is(s.log[0], GestureKind::Tap, btn(kB), btn(kA), 2050));
  CHECK_FALSE(s.log[0].repeat);
  for (std::uint32_t t = 2100; t <= 3000; t += 50) {
    s.advance(ms(t));  // (a call that finds several repeats due gives one: look at the clock as the main loop does)
  }
  CHECK(s.log.size() >= 4u);  // the schedule's repeats: 400, 300, 200 ms after the press
  for (std::size_t i = 1; i < s.log.size(); ++i) {
    CHECK(s.log[i].kind == GestureKind::Tap);
    CHECK(s.log[i].input == btn(kB));
    CHECK(s.log[i].modifier == btn(kA));
    CHECK(s.log[i].repeat);
  }
  const std::size_t made = s.log.size();
  s.release(3090, kB);  // (the next repeat is due at 3100)
  s.release(3200, kA);  // silent: its Tap was consumed
  CHECK(s.log.size() == made);
  CHECK(s.rec.held() == 0u);
}

// OS-MIXINSHOLD: Master is a bound modifier (Insert under it is bound) that also waits for a chord (Master + Insert inside the window) and taps at its release.
TEST_CASE("gesture: a bound modifier that also waits for a chord (Master + Insert)") {
  const std::uint8_t kMaster = kA;
  const std::uint8_t kInsert = kB;
  const std::uint32_t kLate = 500;  // well past the 40 ms chord window
  Rig s(kShiftMask);
  s.w.of[kMaster] = kWantModifier | kWantChord | kWantHoldEncoder;  // as the bindings give it: Master + encoder (OS-MASTERVOL) is why it has no 40 ms deadline
  s.w.of[kInsert] = kWantChord;
  s.w.pair_ab = true;
  SUBCASE("the partner inside the window is the chord; after the window it is a key made under Master, and Master's release is silent") {
    s.press(0, kMaster);
    s.press(10, kInsert);
    NEED_SIZE(s.log, 1u);
    CHECK(s.log[0].kind == GestureKind::Chord);  // (both orders keep working: the pair is bound)
    s.release(20, kInsert);
    s.release(30, kMaster);
    CHECK(s.log.size() == 1u);
    s.clear();
    s.press(1000, kMaster);
    s.advance(ms(1000 + kLate));
    CHECK(s.log.empty());  // Master goes on waiting past the window: it has a use as a modifier
    s.press(1000 + kLate, kInsert);
    s.advance(ms(1000 + kLate + 50));  // Insert waits for its own chord window, then taps
    NEED_SIZE(s.log, 1u);
    CHECK(is(s.log[0], GestureKind::Tap, btn(kInsert), btn(kMaster), 1000 + kLate + 40));
    s.release(1000 + kLate + 20, kInsert);
    s.release(1000 + kLate + 400, kMaster);
    CHECK(s.log.size() == 1u);  // the Tap of Master was consumed: no Mixer at the release
    CHECK(s.rec.held() == 0u);
  }
  SUBCASE("a Shift that joins does not end the wait") {
    s.press(0, kMaster);
    s.press(100, kShift);
    s.advance(ms(100 + kLate));
    NEED_SIZE(s.log, 1u);
    CHECK(s.log[0].kind == GestureKind::Tap);  // Shift's own tap; Master did not tap when Shift came, nor when the window ended
    CHECK(s.log[0].input == btn(kShift));
    CHECK(s.log.size() == 1u);
    s.press(100 + kLate, kInsert);
    s.advance(ms(100 + kLate + 50));
    NEED_SIZE(s.log, 2u);
    CHECK(s.log[1].kind == GestureKind::Tap);
    CHECK(s.log[1].input == btn(kInsert));
    s.release(100 + kLate + 10, kInsert);
    s.release(100 + kLate + 20, kShift);
    s.release(100 + kLate + 30, kMaster);
    CHECK(s.log.size() == 2u);  // Master's release is silent
    CHECK(s.rec.held() == 0u);
  }
  SUBCASE("a Shift that joins and no other key: Master's tap still comes at its release") {
    s.press(0, kMaster);
    s.press(100, kShift);
    s.advance(ms(100 + kLate));
    CHECK(s.log.size() == 1u);  // Shift's own tap only
    s.release(100 + kLate, kMaster);
    NEED_SIZE(s.log, 2u);
    CHECK(is(s.log[1], GestureKind::Tap, btn(kMaster), kNoInput, 100 + kLate));
  }
  SUBCASE("Master alone still taps at its release, however long it was held") {
    s.press(0, kMaster);
    s.advance(ms(2000));
    CHECK(s.log.empty());
    s.release(2100, kMaster);
    NEED_SIZE(s.log, 1u);
    CHECK(is(s.log[0], GestureKind::Tap, btn(kMaster), kNoInput, 2100));
    CHECK(s.rec.held() == 0u);
  }
}

TEST_CASE("gesture: Shift + a bound modifier with no Hold still taps at the release, under the Shift that was held at the press, and a key pressed under both is made under Shift") {
  Rig s(kShiftMask);
  s.w.of[kA] = kWantModifier;
  s.press(0, kShift);
  s.clear();
  s.press(10, kA);
  CHECK(s.log.empty());  // (the paste of Shift + Copy waits for the release too)
  s.release(60, kA);
  NEED_SIZE(s.log, 1u);
  CHECK(is(s.log[0], GestureKind::Tap, btn(kA), btn(kShift), 60));
  s.release(70, kShift);
  // Copy, then Shift: Shift is not a key that consumes Copy, and the Tap is the one the press began, Copy's own (the modifier is the one held at the press).
  Rig t(kShiftMask);
  t.w.of[kA] = kWantModifier;
  t.press(0, kA);
  t.press(30, kShift);
  t.clear();
  t.release(60, kA);
  NEED_SIZE(t.log, 1u);
  CHECK(is(t.log[0], GestureKind::Tap, btn(kA), kNoInput, 60));
  t.release(70, kShift);
  // Shift held with Copy held: an arrow is made under Shift (Shift + Up is the range, not the move).
  Rig u(kShiftMask);
  u.w.of[kA] = kWantModifier;
  u.press(0, kA);
  u.press(10, kShift);
  u.clear();
  u.press(20, kB);
  NEED_SIZE(u.log, 1u);
  CHECK(is(u.log[0], GestureKind::Tap, btn(kB), btn(kShift), 20));
}
