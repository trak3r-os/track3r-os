// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI controller input for MIDI Learn (midi/cc_in.h, OS-013a): the running-status parser that picks the control changes out of the bytes MidiIn hands back, the 32-bit
// message word, and the ring between the render and the main loop. The link that feeds it is seq_midi_link_test.cpp; what the app does with a message is params_app_test.cpp.
// Host and Cortex-M4F (`qemu_midi`).
#include <doctest/doctest.h>

#include <o3os/midi/cc_in.h>

#include <cstdint>
#include <initializer_list>

#include "support/need.h"

namespace midi = o3os::midi;

namespace {

struct Got {
  std::uint8_t ch, cc, value;
  bool operator==(const Got& o) const { return ch == o.ch && cc == o.cc && value == o.value; }
};

// The control changes a run of bytes makes (no heap: the same file runs on the target).
struct Result {
  Got g[16];
  unsigned n = 0;
  bool empty() const { return n == 0; }
  unsigned size() const { return n; }
  bool is(std::initializer_list<Got> want) const {
    if (want.size() != n) {
      return false;
    }
    unsigned i = 0;
    for (const Got& w : want) {
      if (!(g[i++] == w)) {
        return false;
      }
    }
    return true;
  }
};

Result parse(midi::CcParser& p, std::initializer_list<std::uint8_t> bytes) {
  Result out;
  for (const std::uint8_t b : bytes) {
    std::uint8_t ch = 0, cc = 0, v = 0;
    if (p.feed(b, ch, cc, v) && out.n < 16u) {
      out.g[out.n++] = Got{ch, cc, v};
    }
  }
  return out;
}

}  // namespace

TEST_CASE("cc parser: a control change, with running status, with real-time bytes inside it, and across a different message") {
  midi::CcParser p;
  CHECK(parse(p, {0xB0, 74, 100}).is({{0, 74, 100}}));
  CHECK(parse(p, {0xBF, 1, 0}).is({{15, 1, 0}}));
  // running status: the next pairs are control changes of the same channel
  CHECK(parse(p, {0xB2, 10, 1, 11, 2, 12, 3}).is({{2, 10, 1}, {2, 11, 2}, {2, 12, 3}}));
  // a clock (or active sensing) between the bytes of a message changes nothing
  CHECK(parse(p, {0xB1, 0xF8, 20, 0xFE, 0xF8, 99}).is({{1, 20, 99}}));
  // the status byte is cut short by another: nothing is made of the half message
  CHECK(parse(p, {0xB3, 30, 0xB4, 31, 32}).is({{4, 31, 32}}));
}

TEST_CASE("cc parser: notes, program changes, pressure, pitch bend, SysEx and system common are not control changes, and do not leave the parser in a wrong state") {
  midi::CcParser p;
  CHECK(parse(p, {0x90, 60, 100, 0x80, 60, 0, 0xE0, 1, 2, 0xA0, 60, 5}).empty());
  CHECK(parse(p, {0xC1, 5, 0xD1, 9}).empty());  // one data byte each: running status carries on in kind, nothing is picked
  CHECK(parse(p, {0xC1, 5, 6, 7}).empty());
  // SysEx data is nobody's control change, and a SysEx cancels running status: the pair after F7 belongs to no message
  CHECK(parse(p, {0xB0, 1, 2, 0xF0, 0x7D, 9, 10, 0xF7, 11, 12}).is({{0, 1, 2}}));
  // system common cancels running status: data bytes after it belong to nobody
  CHECK(parse(p, {0xB0, 1, 2, 0xF3, 4, 5, 6}).is({{0, 1, 2}}));
  CHECK(parse(p, {0x45, 0x46}).empty());  // data bytes with no status at all
  // and a control change right after all of that is read
  CHECK(parse(p, {0xB5, 7, 8}).is({{5, 7, 8}}));
  // a program change in running status does not turn the next pair into a control change
  CHECK(parse(p, {0xB6, 1, 2, 0xC6, 3, 4, 5}).size() == 1u);
}

TEST_CASE("cc parser: channel mode messages (120..127) are dropped, 119 is a controller") {
  midi::CcParser p;
  CHECK(parse(p, {0xB0, 119, 1}).is({{0, 119, 1}}));
  CHECK(parse(p, {0xB0, 120, 0, 121, 0, 123, 0, 127, 127}).empty());
  CHECK(parse(p, {0xB0, 7, 9}).is({{0, 7, 9}}));
}

TEST_CASE("cc input: a message is one word with its port and channel 1..16, the two ports have their own running status, and a full ring counts what it drops") {
  const midi::CcMsg m{1, 16, 127, 99};
  CHECK(midi::cc_unpack(midi::cc_pack(m)).port == 1);
  CHECK(midi::cc_unpack(midi::cc_pack(m)).channel == 16);
  CHECK(midi::cc_unpack(midi::cc_pack(m)).cc == 127);
  CHECK(midi::cc_unpack(midi::cc_pack(m)).value == 99);

  static midi::CcInput in;  // 170 bytes: static, like the platform's
  midi::CcMsg out{};
  CHECK_FALSE(in.pop(out));
  const auto feed = [&](std::initializer_list<std::uint8_t> bytes, std::uint8_t port) {
    for (const std::uint8_t b : bytes) {
      in.feed(midi::RtItem{0, b, port, 0});
    }
  };
  // the bytes of two ports interleave: each has its own state
  feed({0xB0, 74}, 0);
  feed({0xB1, 7}, 1);
  feed({100}, 0);
  feed({0x40, 8, 0x41}, 1);  // the first message on USB is complete, a running-status one follows
  NEED(in.pop(out));
  CHECK(out.port == 0);
  CHECK(out.channel == 1);
  CHECK(out.cc == 74);
  CHECK(out.value == 100);
  NEED(in.pop(out));
  CHECK(out.port == 1);
  CHECK(out.channel == 2);
  CHECK(out.cc == 7);
  CHECK(out.value == 0x40);
  NEED(in.pop(out));
  CHECK(out.port == 1);
  CHECK(out.cc == 8);
  CHECK(out.value == 0x41);
  CHECK_FALSE(in.pop(out));
  CHECK(in.dropped() == 0u);
  // a port number out of range is the first port's
  feed({0xB0, 1, 2}, 9);
  NEED(in.pop(out));
  CHECK(out.port == 0);
  // more messages than the ring holds: the new ones are dropped and counted, the old ones are kept in order
  for (std::uint32_t i = 0; i < midi::CcInput::kRingSize + 5u; ++i) {
    feed({0xB0, static_cast<std::uint8_t>(i & 0x3Fu), static_cast<std::uint8_t>(i)}, 0);
  }
  CHECK(in.dropped() == 5u);
  for (std::uint32_t i = 0; i < midi::CcInput::kRingSize; ++i) {
    NEED(in.pop(out));
    CHECK(out.value == i);
  }
  CHECK_FALSE(in.pop(out));
}

TEST_CASE("notes in: note on, note off, velocity 0, all-notes-off, running status, and a second ring that a knob flood cannot push out of") {
  const midi::NoteMsg n{1, 16, midi::NoteKind::AllOff, 127, 99};
  CHECK(midi::note_unpack(midi::note_pack(n)).port == 1);
  CHECK(midi::note_unpack(midi::note_pack(n)).channel == 16);
  CHECK(midi::note_unpack(midi::note_pack(n)).kind == midi::NoteKind::AllOff);
  CHECK(midi::note_unpack(midi::note_pack(n)).note == 127);
  CHECK(midi::note_unpack(midi::note_pack(n)).velocity == 99);

  static midi::CcInput in;
  const auto feed = [&](std::initializer_list<std::uint8_t> bytes, std::uint8_t port) {
    for (const std::uint8_t b : bytes) {
      in.feed(midi::RtItem{0, b, port, 0});
    }
  };
  midi::NoteMsg m{};
  midi::CcMsg c{};
  feed({0x92, 60, 100, 0xF8, 64, 90}, 0);  // note on, a clock byte inside, and a running-status note on
  feed({0x82, 60, 40, 0x92, 62, 0}, 1);    // note off with a release velocity, and a note on with velocity 0
  feed({0xB0, 123, 0, 0xB1, 120, 0, 0xB0, 121, 0, 0xE0, 1, 2, 0xA0, 5, 5}, 0);  // all-notes-off x2, reset controllers (dropped), pitch bend and poly pressure (dropped)
  NEED(in.pop_note(m));
  CHECK((m.port == 0 && m.channel == 3 && m.kind == midi::NoteKind::On && m.note == 60 && m.velocity == 100));
  NEED(in.pop_note(m));
  CHECK((m.port == 0 && m.channel == 3 && m.kind == midi::NoteKind::On && m.note == 64 && m.velocity == 90));
  NEED(in.pop_note(m));
  CHECK((m.port == 1 && m.channel == 3 && m.kind == midi::NoteKind::Off && m.note == 60 && m.velocity == 40));
  NEED(in.pop_note(m));
  CHECK((m.port == 1 && m.kind == midi::NoteKind::Off && m.note == 62));
  NEED(in.pop_note(m));
  CHECK((m.port == 0 && m.channel == 1 && m.kind == midi::NoteKind::AllOff && m.note == 123));
  NEED(in.pop_note(m));
  CHECK((m.channel == 2 && m.kind == midi::NoteKind::AllOff && m.note == 120));
  CHECK_FALSE(in.pop_note(m));
  CHECK_FALSE(in.pop(c));  // none of them is a control change (the ring of MIDI Learn is untouched)
  CHECK(in.note_dropped() == 0u);

  // A flood of control changes does not take the notes' room, nor the reverse.
  for (std::uint32_t i = 0; i < midi::CcInput::kRingSize + 10u; ++i) {
    feed({0xB0, 74, static_cast<std::uint8_t>(i & 0x7Fu)}, 0);
  }
  feed({0x90, 61, 77}, 0);
  NEED(in.pop_note(m));
  CHECK((m.kind == midi::NoteKind::On && m.note == 61));
  CHECK(in.dropped() == 10u);
  CHECK(in.note_dropped() == 0u);
  for (std::uint32_t i = 0; i < midi::CcInput::kNoteRingSize + 3u; ++i) {
    feed({0x90, 61, 77}, 0);
  }
  CHECK(in.note_dropped() == 3u);
  // CcParser::feed (the controllers only) still reports no note.
  midi::CcParser p;
  std::uint8_t ch = 0, cc = 0, v = 0;
  CHECK_FALSE(p.feed(0x90, ch, cc, v));
  CHECK_FALSE(p.feed(60, ch, cc, v));
  CHECK_FALSE(p.feed(100, ch, cc, v));
}

// OS-MIDIREC-P1 S3 (A8): the stamped ring. The render stamps a block's MIDI only while the main loop has a take open; with none the ring is not touched and feed() is what it was.
TEST_CASE("stamped ring: nothing is stamped without a stamp, the stamp is carried while on, a full ring counts what it drops, and the other rings are unchanged") {
  static midi::CcInput in;
  const auto feed = [&](std::initializer_list<std::uint8_t> bytes, std::uint8_t port) {
    for (const std::uint8_t b : bytes) {
      in.feed(midi::RtItem{0, b, port, 0});
    }
  };
  midi::StampMsg s{};
  midi::NoteMsg n{};
  CHECK(in.take_track() == midi::CcInput::kNoTrack);
  feed({0x90, 60, 100, 0xB0, 74, 9}, 0);  // no stamp set: nothing is copied
  CHECK_FALSE(in.pop_stamped(s));
  NEED(in.pop_note(n));
  CHECK(n.note == 60);

  in.open_take(16);  // not a track: no take
  CHECK(in.take_track() == midi::CcInput::kNoTrack);
  in.open_take(3);
  CHECK(in.take_track() == 3);
  in.set_stamp(7, 1234);
  feed({0x91, 61, 90, 0xB1, 74, 55, 0x81, 61, 0, 0xB1, 123, 0}, 1);
  in.clear_stamp();
  feed({0x91, 62, 90}, 1);  // after clear_stamp: not stamped
  NEED(in.pop_stamped(s));
  CHECK((s.kind == midi::StampKind::On && s.tick == 1234 && s.pattern == 7 && s.port == 1 && s.channel == 2 && s.a == 61 && s.b == 90));
  NEED(in.pop_stamped(s));
  CHECK((s.kind == midi::StampKind::Cc && s.a == 74 && s.b == 55 && s.tick == 1234));
  NEED(in.pop_stamped(s));
  CHECK((s.kind == midi::StampKind::Off && s.a == 61));
  NEED(in.pop_stamped(s));
  CHECK((s.kind == midi::StampKind::AllOff && s.a == 123));
  CHECK_FALSE(in.pop_stamped(s));
  CHECK(in.stamp_dropped() == 0u);
  // the two older rings got everything too (the grid's note entry and MIDI Learn read them as before)
  unsigned notes = 0;
  while (in.pop_note(n)) {
    ++notes;
  }
  CHECK(notes == 4u);  // on, off, all-off, and the unstamped on
  midi::CcMsg c{};
  unsigned ccs = 0;
  while (in.pop(c)) {
    ++ccs;
  }
  CHECK(ccs == 2u);  // the first one (before any take) and the stamped one; the 123 is a note message

  // a full stamped ring drops what does not fit and counts it
  in.set_stamp(1, 5);
  for (std::uint32_t i = 0; i < midi::CcInput::kStampRingSize + 4u; ++i) {
    feed({0xB0, 1, static_cast<std::uint8_t>(i & 0x7Fu)}, 0);
  }
  CHECK(in.stamp_dropped() == 4u);
  unsigned got = 0;
  while (in.pop_stamped(s)) {
    ++got;
  }
  CHECK(got == midi::CcInput::kStampRingSize);
  in.close_take();
  CHECK(in.take_track() == midi::CcInput::kNoTrack);
}
