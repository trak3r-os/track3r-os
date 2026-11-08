// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI Time Code in (midi/in.h and midi/mtc_in.h, OS-MTC B2; docs/specs/OS-MTC.md 4.1 and 4.2): the parser's two kinds and what MidiIn::drain does with them, and the chaser against
// synthetic masters. The master here is a stream of stamped bytes written from the MIDI specification (its own encoder, drop-frame counted by walking the minutes), sent at known
// times of the unit's own cycle counter, so that every figure the chaser gives (the master's time at a sample, its rate, its state) has an exact answer to be compared with. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/events.h>
#include <o3os/core/audio_clock.h>
#include <o3os/core/qtime.h>
#include <o3os/midi/in.h>
#include <o3os/midi/mtc.h>
#include <o3os/midi/mtc_in.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "support/need.h"

namespace midi = o3os::midi;
namespace mtc = o3os::midi::mtc;
namespace audio = o3os::audio;
namespace core = o3os::core;
using Notice = mtc::Chaser::Notice;
using State = mtc::Chaser::State;

namespace {

constexpr bool kRate44100 = core::kRefCyclesPerSample.num == 1800000u && core::kRefCyclesPerSample.den == 441u;
constexpr std::uint64_t kByte = midi::din::kByteCycles;          // a DIN byte: 320 us
constexpr std::uint32_t kUsbLatency = core::kCpuHz / 2000u;     // the USB port's documented input latency, as MidiIn::Config has it
constexpr std::uint32_t kBlock = 64;

std::uint64_t cyc(std::uint64_t sample) { return core::cycles_from_samples(sample); }
std::int32_t iabs(std::int32_t v) { return v < 0 ? -v : v; }

// ---- the master: bytes from the specification --------------------------------------------------------------------------------------------------
// Frames in a day of each rate code (0 = 24, 1 = 25, 2 = 29.97 drop-frame, 3 = 30) and the cycles of a quarter frame at the real rate.
constexpr std::uint32_t kFramesPerDay[4] = {2073600u, 2160000u, 2589408u, 2592000u};
constexpr std::uint64_t kQfCycles[4] = {1875000u, 1800000u, 1501500u, 1500000u};  // 180 MHz / (4 x 24, 4 x 25, 4 x 30000/1001, 4 x 30)
constexpr std::uint32_t kFps[4] = {24u, 25u, 30u, 30u};
// The frame count of 01:00:00:00 at each code (the drop-frame hour is the well-known 107,892).
constexpr std::uint32_t kHourFrames[4] = {86400u, 90000u, 107892u, 108000u};

struct Lab {
  std::uint32_t h, m, s, f;
};
// The label of a frame count, written the plain way: drop-frame by walking the minutes (a minute has 1800 frames, 1798 when it is not a tenth).
Lab walk(std::uint32_t n, std::uint8_t code) {
  Lab l{};
  if (code != 2u) {
    const std::uint32_t fps = kFps[code];
    l.f = n % fps;
    l.s = (n / fps) % 60u;
    l.m = (n / fps / 60u) % 60u;
    l.h = n / fps / 3600u;
    return l;
  }
  std::uint32_t minute = 0;
  std::uint32_t rem = n;
  for (;;) {
    const std::uint32_t in_minute = (minute % 10u == 0u) ? 1800u : 1798u;
    if (rem < in_minute) {
      break;
    }
    rem -= in_minute;
    ++minute;
  }
  const std::uint32_t within = rem + ((minute % 10u == 0u) ? 0u : 2u);  // the first label of a dropping minute is ;02
  l.f = within % 30u;
  l.s = within / 30u;
  l.m = minute % 60u;
  l.h = minute / 60u;
  return l;
}

// The data byte `0nnn dddd` of quarter frame q: piece q mod 8 of the cycle that starts at the frame q / 4 rounded down to its cycle.
std::uint8_t qf_data_of(std::uint32_t q, std::uint8_t code) {
  const std::uint32_t qpd = 4u * kFramesPerDay[code];
  const std::uint32_t qq = q % qpd;
  const std::uint32_t piece = qq & 7u;
  const Lab l = walk((qq & ~7u) / 4u, code);
  std::uint32_t nibble = 0;
  switch (piece) {
    case 0: nibble = l.f & 15u; break;
    case 1: nibble = l.f >> 4; break;
    case 2: nibble = l.s & 15u; break;
    case 3: nibble = l.s >> 4; break;
    case 4: nibble = l.m & 15u; break;
    case 5: nibble = l.m >> 4; break;
    case 6: nibble = l.h & 15u; break;
    default: nibble = (l.h >> 4) | (static_cast<std::uint32_t>(code) << 1); break;
  }
  return static_cast<std::uint8_t>((piece << 4) | nibble);
}

// ---- the unit: the ring, the map, the drain and the chaser, a block at a time ---------------------------------------------------------------------------
struct Msg {
  std::uint64_t stamp[12];  // the cycle at which each byte was complete
  std::uint8_t byte[12];
  std::uint8_t n;
  std::uint8_t port;
};

struct Seen {
  std::uint64_t sample;
  Notice notice;
};

struct Rig {
  midi::RtInRing ring;
  midi::ClockMap map;
  midi::ClockDll dll;
  midi::MidiIn in;
  mtc::Chaser ch;
  midi::TransportEvent ev[4];
  midi::RtItem pass[64];
  midi::MtcEvent mt[64];
  std::vector<Msg> msgs;
  std::size_t next = 0;
  std::uint64_t sample = 0;
  std::vector<Seen> seen;
  std::vector<std::uint8_t> passed;  // every byte that came back as "not timing", in order
  std::uint32_t qf_events = 0;
  std::uint32_t full_events = 0;
  std::uint32_t blocks_freewheel = 0;

  explicit Rig(std::uint64_t start_sample = 0, midi::Port mtc_port = midi::Port::Din) : sample(start_sample) {
    midi::MidiIn::Config c;
    c.mtc_enabled = true;
    c.mtc_port = mtc_port;
    in.configure(c);
  }

  // Runs whole blocks until the block start reaches `until`.
  void run_to(std::uint64_t until) {
    while (sample < until) {
      const std::uint64_t cend = cyc(sample + kBlock);
      while (next < msgs.size()) {
        const Msg& m = msgs[next];
        if (m.stamp[m.n - 1u] >= cend) {
          break;
        }
        for (std::uint32_t i = 0; i < m.n; ++i) {
          CHECK(ring.push(midi::RtItem{static_cast<std::uint32_t>(m.stamp[i]), m.byte[i], m.port, 0}));
        }
        ++next;
      }
      map.set_ref(sample, static_cast<std::uint32_t>(cyc(sample)));
      const midi::MidiIn::Drained d = in.drain(ring, map, dll, 256, ev, 4, pass, 64, mt, 64);
      for (std::uint32_t i = 0; i < d.passed; ++i) {
        passed.push_back(pass[i].byte);
      }
      for (std::uint32_t i = 0; i < d.mtc; ++i) {
        (mt[i].kind == midi::MtcEvent::Kind::Qf ? qf_events : full_events) += 1u;
        note(ch.feed(mt[i], map));
      }
      note(ch.poll(sample));
      if (ch.state() == State::Freewheel) {
        ++blocks_freewheel;
      }
      sample += kBlock;
    }
  }
  void note(Notice n) {
    if (n != Notice::None) {
      seen.push_back(Seen{sample, n});
    }
  }
  std::uint32_t count(Notice n) const {
    std::uint32_t c = 0;
    for (const Seen& s : seen) {
      c += s.notice == n ? 1u : 0u;
    }
    return c;
  }
  // The block start of the first / last notice of a kind (0 when none).
  std::uint64_t first(Notice n) const {
    for (const Seen& s : seen) {
      if (s.notice == n) {
        return s.sample;
      }
    }
    return 0;
  }
  std::uint64_t last(Notice n) const {
    std::uint64_t r = 0;
    for (const Seen& s : seen) {
      r = s.notice == n ? s.sample : r;
    }
    return r;
  }
};

// A master: sends quarter frames (and full frames) of one rate code at the unit's cycle times. The master's clock runs (1 + ppm / 1e6) times the unit's: a master that is fast
// sends its quarter frames closer together. `jitter_us` moves every message by up to that much either way; `drop_k` of every `drop_n` quarter frames (at random) are not sent, never in
// the first 16 of a segment. A segment is a run of consecutive quarter frames from a base (cycle, number); quiet() and jump() start the next one.
struct Master {
  Rig& r;
  std::uint8_t code;
  std::int32_t ppm = 0;
  std::int32_t jitter_us = 0;
  std::uint32_t drop_k = 0;
  std::uint32_t drop_n = 1;
  std::uint32_t clean_first = 16;  // no drop in the first this many quarter frames of a segment (0: drops from the second on)
  midi::Port port = midi::Port::Din;
  std::uint32_t rng = 12345u;
  std::uint64_t c_base = 0;
  std::int64_t q_base = 0;
  std::int64_t dir = 1;
  std::uint64_t k = 0;  // quarter-frame periods since the base

  Master(Rig& rig, std::uint8_t c) : r(rig), code(c) {}

  std::uint32_t rnd() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
  }
  std::uint32_t qpd() const { return 4u * kFramesPerDay[code]; }
  std::uint64_t cycles_at(std::uint64_t kk) const { return c_base + kk * kQfCycles[code] * 1000000u / static_cast<std::uint64_t>(1000000 + ppm); }
  std::uint32_t q_at(std::uint64_t kk) const {
    const std::int64_t m = static_cast<std::int64_t>(qpd());
    std::int64_t q = (q_base + dir * static_cast<std::int64_t>(kk)) % m;
    return static_cast<std::uint32_t>(q < 0 ? q + m : q);
  }
  void start(std::uint64_t cycle, std::uint32_t q, std::int64_t direction = 1) {
    c_base = cycle;
    q_base = q;
    dir = direction;
    k = 0;
  }
  // The master time at the unit's `sample` as the master's own clock has it, Q32 samples, within the day (valid for a forward segment).
  core::Q32 expected(std::uint64_t sample) const {
    const std::int64_t c = static_cast<std::int64_t>(cyc(sample)) - static_cast<std::int64_t>(c_base);
    const std::int64_t mc = q_base * static_cast<std::int64_t>(kQfCycles[code]) + c * (1000000 + ppm) / 1000000;
    const std::int64_t day = static_cast<std::int64_t>(qpd()) * static_cast<std::int64_t>(kQfCycles[code]);
    return core::q32_from_cycles(static_cast<std::uint64_t>(((mc % day) + day) % day));
  }
  // The number of the quarter frame the master is at, in the unit's `sample` (the last one sent or about to be).
  void push(const std::uint8_t* b, std::uint32_t n, std::uint64_t begin) {
    Msg m{};
    m.n = static_cast<std::uint8_t>(n);
    m.port = static_cast<std::uint8_t>(port);
    for (std::uint32_t i = 0; i < n; ++i) {
      m.byte[i] = b[i];
      m.stamp[i] = port == midi::Port::Din ? begin + (i + 1u) * kByte : begin + kUsbLatency;
    }
    r.msgs.push_back(m);
  }
  std::uint64_t jittered(std::uint64_t c) {
    if (jitter_us == 0) {
      return c;
    }
    const std::int64_t j = static_cast<std::int64_t>(rnd() % static_cast<std::uint32_t>(2 * jitter_us + 1)) - jitter_us;
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(c) + j * 180);
  }
  void qf(std::uint32_t q, std::uint64_t begin) {
    const std::uint8_t b[2] = {0xF1, qf_data_of(q, code)};
    push(b, 2, begin);
  }
  // `n` quarter frames, one per period.
  void run(std::uint32_t n) {
    const std::uint64_t first_k = k;
    for (std::uint32_t i = 0; i < n; ++i, ++k) {
      const bool clean = k - first_k + (first_k == 0u ? 0u : clean_first) >= clean_first;
      if (drop_k != 0u && clean && (rnd() % drop_n) < drop_k) {
        continue;
      }
      qf(q_at(k), jittered(cycles_at(k)));
    }
  }
  // The master's clock changes rate here (the time, and the quarter frame it is at, go on): the next segment starts from where the last one is.
  void set_rate(std::int32_t new_ppm) {
    const std::uint64_t c = cycles_at(k);
    const std::uint32_t q = q_at(k);
    ppm = new_ppm;
    start(c, q, dir);
  }
  // The clock runs and nothing is sent for `n` periods.
  void quiet(std::uint32_t n) { start(cycles_at(k + n), q_at(k + n), dir); }
  // The master's time moves by `dq` quarter frames at the next one (a jump, the quarter frames go on).
  void jump(std::int64_t dq) { start(cycles_at(k), static_cast<std::uint32_t>((static_cast<std::int64_t>(q_at(k)) + dq + qpd()) % qpd())); }
  // A full frame of frame count `n`, begun at `begin`; `id` is the device id byte.
  void full_at(std::uint32_t n, std::uint64_t begin, std::uint8_t id = 0x7F) {
    const Lab l = walk(n, code);
    const std::uint8_t b[10] = {0xF0, 0x7F, id, 0x01, 0x01, static_cast<std::uint8_t>((code << 5) | l.h), static_cast<std::uint8_t>(l.m), static_cast<std::uint8_t>(l.s), static_cast<std::uint8_t>(l.f), 0xF7};
    push(b, 10, begin);
  }
  // A full frame at the master's present: the frame the clock is in.
  void full_now() { full_at(q_at(k) / 4u, cycles_at(k)); }
};

constexpr std::uint64_t kStart = 44100u * 20u;  // a start 20 s in: the 32-bit cycle counter wraps within the first four seconds

// The error of the chaser's master time at `sample`, in samples (positive: it is ahead), as the signed Q32; false when it has none.
bool time_error(const Rig& r, const Master& m, std::uint64_t sample, std::int64_t& err_q32) {
  core::Q32 got = 0;
  if (!r.ch.master_time_at(sample, got)) {
    return false;
  }
  // a day of Q32 samples is above 2^63: the distance is taken on the circle with unsigned arithmetic
  const std::uint64_t day = core::q32_from_cycles(static_cast<std::uint64_t>(m.qpd()) * kQfCycles[m.code]);
  const std::uint64_t want = m.expected(sample);
  const std::uint64_t ahead = got >= want ? got - want : day - (want - got);  // how far got is past want, 0 .. day
  std::int64_t d = 0;
  if (ahead <= day / 2u) {
    d = static_cast<std::int64_t>(ahead);
  } else {
    d = -static_cast<std::int64_t>(day - ahead);
  }
  err_q32 = d;
  return true;
}
constexpr std::int64_t kSample = static_cast<std::int64_t>(core::kQ32One);

// The notices that must not appear in a run that is going well.
bool quiet_run(const Rig& r) { return r.count(Notice::Relocated) == 0u && r.count(Notice::Stopped) == 0u && r.count(Notice::Locate) == 0u; }

}  // namespace

// ---- labels and time maths ---------------------------------------------------------------------------------------------------------------------------

TEST_CASE("mtc in: labels, drop-frame counted against a walk of the minutes, and the master time of a quarter frame") {
  for (std::uint8_t code = 0; code < 4; ++code) {
    CHECK(mtc::frames_per_day(code) == kFramesPerDay[code]);
    CHECK(mtc::quarter_frames_per_day_of_code(code) == 4u * kFramesPerDay[code]);
    // a stride through the day, and every frame of the first two hours (drop-frame has the minutes in them)
    const std::uint32_t fpd = kFramesPerDay[code];
    for (std::uint32_t n = 0; n < fpd; n += (n < 220000u ? 1u : 4099u)) {
      const Lab w = walk(n, code);
      const mtc::Time t{static_cast<std::uint8_t>(w.h), static_cast<std::uint8_t>(w.m), static_cast<std::uint8_t>(w.s), static_cast<std::uint8_t>(w.f)};
      NEED(mtc::label_valid(t, code));
      NEED(mtc::frames_of_label(t, code) == n);
      NEED(mtc::label_of_frames(n, code) == t);
    }
    CHECK(mtc::label_of_frames(fpd, code) == mtc::Time{0, 0, 0, 0});  // the day wraps
    CHECK(mtc::frames_of_label(mtc::Time{1, 0, 0, 0}, code) == kHourFrames[code]);
  }
  // drop-frame labels that do not exist; a tenth minute and a non-drop rate keep theirs
  CHECK_FALSE(mtc::label_valid(mtc::Time{1, 1, 0, 0}, 2));
  CHECK_FALSE(mtc::label_valid(mtc::Time{1, 1, 0, 1}, 2));
  CHECK(mtc::label_valid(mtc::Time{1, 1, 0, 2}, 2));
  CHECK(mtc::label_valid(mtc::Time{1, 10, 0, 0}, 2));
  CHECK(mtc::label_valid(mtc::Time{1, 1, 1, 0}, 2));
  CHECK(mtc::label_valid(mtc::Time{1, 1, 0, 0}, 3));
  CHECK_FALSE(mtc::label_valid(mtc::Time{1, 0, 0, 25}, 1));  // 25 frames a second: frames 0..24
  CHECK(mtc::label_valid(mtc::Time{1, 0, 0, 24}, 1));
  CHECK_FALSE(mtc::label_valid(mtc::Time{24, 0, 0, 0}, 3));
  CHECK_FALSE(mtc::label_valid(mtc::Time{0, 60, 0, 0}, 3));
  CHECK_FALSE(mtc::label_valid(mtc::Time{0, 0, 60, 0}, 3));
  // the master time of a quarter frame is A3's at 24, 25 and 30 fps and 0.1 % later at drop-frame; and it agrees with the cycle count of the real clock
  for (std::uint8_t code = 0; code < 4; ++code) {
    for (const std::uint32_t q : std::initializer_list<std::uint32_t>{0u, 1u, 7u, 8u, 4000u, 4u * kHourFrames[code], 4u * kFramesPerDay[code] - 1u}) {
      const core::Q32 want = core::q32_from_cycles(static_cast<std::uint64_t>(q) * kQfCycles[code]);
      const core::Q32 got = mtc::qf_time_real(q, code);
      const std::int64_t d = static_cast<std::int64_t>(got) - static_cast<std::int64_t>(want);
      CHECK_MESSAGE((d < kSample / 100 && d > -kSample / 100), "code " << int{code} << " q " << q);
    }
  }
  CHECK(mtc::qf_time_real(100, 0) == mtc::qf_time_q32(100, 24));
  CHECK(mtc::qf_time_real(100, 1) == mtc::qf_time_q32(100, 25));
  CHECK(mtc::qf_time_real(100, 3) == mtc::qf_time_q32(100, 30));
}

// ---- the parser and the drain --------------------------------------------------------------------------------------------------------------------------

namespace {
// A drain over a byte list on one port, one byte per call-sized step, collecting everything.
struct Bytes {
  std::vector<std::uint8_t> pass;
  std::vector<midi::MtcEvent> mtc;
  std::uint32_t events = 0;
  midi::MidiIn::Stats stats;
};
Bytes drain_bytes(const std::vector<std::uint8_t>& in_bytes, midi::Port port, const midi::MidiIn::Config& cfg, bool sink, std::uint32_t mtc_cap = 64) {
  midi::RtInRing ring;
  midi::ClockMap map;
  midi::ClockDll dll;
  midi::MidiIn in;
  in.configure(cfg);
  map.set_ref(0, 0);
  std::uint32_t cyc_stamp = 1000000;
  for (const std::uint8_t b : in_bytes) {
    CHECK(ring.push(midi::RtItem{cyc_stamp, b, static_cast<std::uint8_t>(port), 0}));
    cyc_stamp += 57600;
    if (ring.size() >= 200) {
      break;
    }
  }
  Bytes out;
  midi::TransportEvent ev[16];
  midi::RtItem pass[256];
  midi::MtcEvent mt[64];
  for (int guard = 0; guard < 400; ++guard) {
    const midi::MidiIn::Drained d = sink ? in.drain(ring, map, dll, 256, ev, 16, pass, 256, mt, mtc_cap) : in.drain(ring, map, dll, 256, ev, 16, pass, 256);
    for (std::uint32_t i = 0; i < d.passed; ++i) {
      out.pass.push_back(pass[i].byte);
    }
    for (std::uint32_t i = 0; i < d.mtc; ++i) {
      out.mtc.push_back(mt[i]);
    }
    out.events += d.events;
    if (d.items == 0u) {
      break;
    }
  }
  out.stats = in.stats();
  return out;
}
}  // namespace

TEST_CASE("mtc in: the parser takes a quarter frame whole and a valid full frame, and nothing else changes") {
  midi::InParser p;
  p.set_mtc(true);
  CHECK(p.feed(0xF1).kind == midi::ByteKind::Consumed);
  const midi::InParser::Out q = p.feed(0x4D);
  CHECK(q.kind == midi::ByteKind::MtcQf);
  CHECK(q.value == 0x4Du);
  CHECK(p.feed(0x4D).kind == midi::ByteKind::Pass);  // a data byte with no status before it: as ever
  // an F1 cancelled by a status byte: the status byte is whatever it is, the next data byte is its
  CHECK(p.feed(0xF1).kind == midi::ByteKind::Consumed);
  CHECK(p.feed(0x90).kind == midi::ByteKind::Pass);
  CHECK(p.feed(0x3C).kind == midi::ByteKind::Pass);
  // a real-time byte between the F1 and its data byte does not disturb it
  CHECK(p.feed(0xF1).kind == midi::ByteKind::Consumed);
  CHECK(p.feed(0xF8).kind == midi::ByteKind::Clock);
  CHECK(p.feed(0x21).kind == midi::ByteKind::MtcQf);

  // a full frame: the F7 says so, every byte before it is a pass; the value is the packed time
  const std::uint8_t ff[] = {0xF0, 0x7F, 0x05, 0x01, 0x01, 0x21, 0x3B, 0x2D, 0x0C, 0xF7};  // code 1 (25 fps), 01:59:45:12, device 5
  midi::InParser::Out last{midi::ByteKind::Pass, 0};
  for (const std::uint8_t b : ff) {
    last = p.feed(b);
    if (b != 0xF7) {
      CHECK(last.kind == midi::ByteKind::Pass);
    }
  }
  CHECK(last.kind == midi::ByteKind::MtcFull);
  CHECK(last.value == mtc::pack_full(mtc::Time{1, 59, 45, 12}, 1));
  // not full frames: another sub-id, a short one, a long one, a field out of range, a frame that the rate does not have, a drop-frame label that does not exist
  const std::vector<std::vector<std::uint8_t>> not_full = {
      {0xF0, 0x7F, 0x7F, 0x01, 0x02, 0x21, 0x3B, 0x2D, 0x0C, 0xF7},            // sub-id 2 (a user bits message)
      {0xF0, 0x7E, 0x7F, 0x01, 0x01, 0x21, 0x3B, 0x2D, 0x0C, 0xF7},            // non-real-time
      {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 0x3B, 0x2D, 0xF7},                  // one byte short
      {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 0x3B, 0x2D, 0x0C, 0x00, 0xF7},      // one long
      {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x38, 0x3B, 0x2D, 0x0C, 0xF7},            // hours 24
      {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 0x3C, 0x2D, 0x0C, 0xF7},            // minutes 60
      {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 0x3B, 0x2D, 0x19, 0xF7},            // frame 25 at 25 fps
      {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x41, 0x01, 0x00, 0x01, 0xF7},            // drop-frame 01:01:00;01 does not exist
  };
  for (const auto& m : not_full) {
    p.reset();
    p.set_mtc(true);
    midi::InParser::Out o{midi::ByteKind::Pass, 0};
    for (const std::uint8_t b : m) {
      o = p.feed(b);
      CHECK(o.kind == midi::ByteKind::Pass);
    }
  }
  // MTC off: F1 and its data byte, and the full frame, are what they were
  p.reset();
  CHECK(p.feed(0xF1).kind == midi::ByteKind::Pass);
  CHECK(p.feed(0x4D).kind == midi::ByteKind::Pass);
  for (const std::uint8_t b : ff) {
    CHECK(p.feed(b).kind == midi::ByteKind::Pass);
  }
  // the pre-MTC kinds are untouched by it: an SPP and the clock
  p.set_mtc(true);
  CHECK(p.feed(0xF2).kind == midi::ByteKind::Consumed);
  CHECK(p.feed(0x10).kind == midi::ByteKind::Consumed);
  const midi::InParser::Out spp = p.feed(0x02);
  CHECK(spp.kind == midi::ByteKind::Spp);
  CHECK(spp.value == (0x10u | (0x02u << 7)));
  CHECK(p.feed(0xF8).kind == midi::ByteKind::Clock);
}

TEST_CASE("mtc in: with MTC IN absent the drain does what it did, for every other byte; with it on, only the quarter frames are taken") {
  // a stream with notes, running status, a program change, an SPP, a SysEx that is not time code, quarter frames, a full frame, clocks and an active-sensing byte
  const std::vector<std::uint8_t> stream = {
      0x90, 0x3C, 0x64, 0x3E, 0x64,                                    // note on, then running status
      0xF1, 0x00, 0xF1, 0x11,                                          // two quarter frames
      0xC1, 0x05, 0xB0, 0x07, 0x7F,                                    // a program change, a control change
      0xF2, 0x10, 0x02,                                                // an SPP
      0xF0, 0x7E, 0x00, 0x09, 0x01, 0xF7,                              // a general MIDI on SysEx
      0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 0x3B, 0x2D, 0x0C, 0xF7,      // a full frame
      0xF8, 0xFE, 0xF1, 0x23, 0x80, 0x3C, 0x00, 0xF8, 0x91, 0x40, 0x7F,
      0xF1, 0xF8, 0x35,                                                // a quarter frame with a clock inside
  };
  midi::MidiIn::Config off;  // MTC off
  midi::MidiIn::Config on;
  on.mtc_enabled = true;
  on.mtc_port = midi::Port::Din;
  midi::MidiIn::Config other_port = on;
  other_port.mtc_port = midi::Port::Usb;

  const Bytes base = drain_bytes(stream, midi::Port::Din, off, false);  // the call as it was before B2: no sink
  const Bytes a = drain_bytes(stream, midi::Port::Din, off, true);      // a sink and MTC off
  const Bytes b = drain_bytes(stream, midi::Port::Din, on, false);      // MTC on and no sink
  const Bytes c = drain_bytes(stream, midi::Port::Din, other_port, true);  // MTC on, the other port
  CHECK(a.pass == base.pass);
  CHECK(b.pass == base.pass);
  CHECK(c.pass == base.pass);
  CHECK(a.mtc.empty());
  CHECK(b.mtc.empty());
  CHECK(c.mtc.empty());
  CHECK(a.stats.mtc_qf == 0u);
  // what the old drain passed: everything but the clocks, the SPP (consumed) and nothing else (the active-sensing byte is "not ours" and passes)
  const std::vector<std::uint8_t> expect_old = {0x90, 0x3C, 0x64, 0x3E, 0x64, 0xF1, 0x00, 0xF1, 0x11, 0xC1, 0x05, 0xB0, 0x07, 0x7F,
                                                0xF0, 0x7E, 0x00, 0x09, 0x01, 0xF7, 0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 0x3B, 0x2D, 0x0C, 0xF7,
                                                0xFE, 0xF1, 0x23, 0x80, 0x3C, 0x00, 0x91, 0x40, 0x7F, 0xF1, 0x35};
  CHECK(base.pass == expect_old);

  // MTC on, the port of the stream: the quarter frames (F1 and its data byte) leave `pass`, the rest is as before, and four quarter frames and one full frame reach the sink in order
  const Bytes d = drain_bytes(stream, midi::Port::Din, on, true);
  const std::vector<std::uint8_t> without = {0x90, 0x3C, 0x64, 0x3E, 0x64, 0xC1, 0x05, 0xB0, 0x07, 0x7F, 0xF0, 0x7E, 0x00, 0x09, 0x01, 0xF7,
                                             0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 0x3B, 0x2D, 0x0C, 0xF7, 0xFE, 0x80, 0x3C, 0x00, 0x91, 0x40, 0x7F};
  CHECK(d.pass == without);
  CHECK(d.stats.mtc_qf == 4u);
  CHECK(d.stats.mtc_full == 1u);
  NEED(d.mtc.size() == 5u);
  CHECK(d.mtc[0].kind == midi::MtcEvent::Kind::Qf);
  CHECK(d.mtc[0].data == 0x00u);
  CHECK(d.mtc[1].data == 0x11u);
  CHECK(d.mtc[2].kind == midi::MtcEvent::Kind::Full);
  CHECK(d.mtc[2].value == mtc::pack_full(mtc::Time{1, 59, 45, 12}, 1));
  CHECK(d.mtc[3].data == 0x23u);
  CHECK(d.mtc[4].data == 0x35u);
  CHECK(d.mtc[0].port == static_cast<std::uint8_t>(midi::Port::Din));
}

TEST_CASE("mtc in: the stamps are the instant the sender began the message: DIN by its byte times, USB by its latency") {
  midi::MidiIn::Config din_cfg;
  din_cfg.mtc_enabled = true;
  din_cfg.mtc_port = midi::Port::Din;
  midi::MidiIn::Config usb_cfg = din_cfg;
  usb_cfg.mtc_port = midi::Port::Usb;
  const std::vector<std::uint8_t> qf = {0xF1, 0x4D};
  const Bytes a = drain_bytes(qf, midi::Port::Din, din_cfg, true);
  NEED(a.mtc.size() == 1u);
  // the bytes are stamped 1000000 and 1057600 (the helper's spacing is a byte time, 57,600 cycles): the quarter frame began two byte times before the second stamp
  CHECK(a.mtc[0].cycles == 1057600u - 2u * static_cast<std::uint32_t>(kByte));
  const Bytes b = drain_bytes(qf, midi::Port::Usb, usb_cfg, true);
  NEED(b.mtc.size() == 1u);
  CHECK(b.mtc[0].cycles == 1057600u - kUsbLatency);
  const std::vector<std::uint8_t> ff = {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 0x3B, 0x2D, 0x0C, 0xF7};
  const Bytes c = drain_bytes(ff, midi::Port::Din, din_cfg, true);
  NEED(c.mtc.size() == 1u);
  CHECK(c.mtc[0].cycles == 1000000u + 9u * 57600u - 10u * static_cast<std::uint32_t>(kByte));  // the F7 is the tenth byte: the message began at the first byte's start
}

TEST_CASE("mtc in: a full sink holds the byte for the next call, and nothing is lost or reordered") {
  std::vector<std::uint8_t> s;
  for (std::uint8_t i = 0; i < 6; ++i) {
    s.push_back(0xF1);
    s.push_back(static_cast<std::uint8_t>(i << 4));
  }
  midi::MidiIn::Config on;
  on.mtc_enabled = true;
  on.mtc_port = midi::Port::Din;
  midi::RtInRing ring;
  midi::ClockMap map;
  midi::ClockDll dll;
  midi::MidiIn in;
  in.configure(on);
  map.set_ref(0, 0);
  std::uint32_t t = 5000000;
  for (const std::uint8_t b : s) {
    CHECK(ring.push(midi::RtItem{t, b, 0, 0}));
    t += 57600;
  }
  midi::TransportEvent ev[4];
  midi::RtItem pass[8];
  midi::MtcEvent mt[2];
  std::vector<std::uint8_t> got;
  for (int i = 0; i < 20; ++i) {
    const midi::MidiIn::Drained d = in.drain(ring, map, dll, 256, ev, 4, pass, 8, mt, 2);
    CHECK(d.mtc <= 2u);
    CHECK(d.passed == 0u);
    for (std::uint32_t j = 0; j < d.mtc; ++j) {
      got.push_back(mt[j].data);
    }
  }
  const std::vector<std::uint8_t> want = {0x00, 0x10, 0x20, 0x30, 0x40, 0x50};
  CHECK(got == want);
  // a sink given with room for none is no sink at all: the quarter frames pass as ever
  midi::MtcEvent none[1];
  midi::RtInRing ring2;
  midi::MidiIn in2;
  in2.configure(on);
  CHECK(ring2.push(midi::RtItem{t, 0xF1, 0, 0}));
  CHECK(ring2.push(midi::RtItem{t + 57600u, 0x4D, 0, 0}));
  const midi::MidiIn::Drained d2 = in2.drain(ring2, map, dll, 256, ev, 4, pass, 8, none, 0);
  CHECK(d2.mtc == 0u);
  CHECK(d2.passed == 2u);
}

// ---- the chaser against masters -----------------------------------------------------------------------------------------------------------------------

TEST_CASE("mtc in: a synthetic master at 24, 25, 29.97 drop-frame and 30 fps is read, and its time is known to a sample") {
  for (std::uint8_t code = 0; code < 4; ++code) {
    Rig r(kStart);
    Master m(r, code);
    const std::uint64_t c0 = cyc(kStart + 4410u);
    m.start(c0, 4u * kHourFrames[code]);
    m.run(6u * 4u * kFps[code]);  // six seconds of quarter frames
    r.run_to(kStart + 44100u * 5u);
    CAPTURE(int{code});
    CHECK(r.ch.state() == State::Acquired);
    CHECK(r.count(Notice::Acquired) == 1u);
    CHECK(quiet_run(r));
    CHECK(r.ch.code() == code);
    CHECK(r.ch.fps() == kFps[code]);
    CHECK(r.qf_events > 400u);
    CHECK(r.passed.empty());  // a quarter frame is not a byte for anyone else
    // acquired within one cycle after the fit: at most 15 quarter frames after the first
    CHECK(r.first(Notice::Acquired) <= kStart + 4410u + 15u * 44100u / (4u * kFps[code]) + 3u * kBlock);
    CHECK(r.ch.last_time().h == 1u);
    CHECK(iabs(r.ch.rate_ppm()) < 20);
    for (std::uint64_t s = kStart + 44100u * 2u; s < kStart + 44100u * 5u; s += 6007u) {
      std::int64_t e = 0;
      NEED(time_error(r, m, s, e));
      CHECK_MESSAGE((e < kSample / 4 && e > -kSample / 4), "sample " << s << " error " << e);
    }
    // and the inverse: the sample at which the master reaches a time
    const core::Q32 target = m.expected(kStart + 44100u * 4u);
    std::uint64_t at = 0;
    NEED(r.ch.sample_at_master_time(target, at));
    CHECK(at >= kStart + 44100u * 4u - 1u);
    CHECK(at <= kStart + 44100u * 4u + 1u);
  }
}

TEST_CASE("mtc in: the pieces are assembled from any starting piece, and from any time of the day") {
  for (std::uint8_t code = 0; code < 4; ++code) {
    for (std::uint32_t piece = 0; piece < 8; ++piece) {
      Rig r(kStart);
      Master m(r, code);
      const std::uint32_t n0 = kHourFrames[code] + 2u * piece + 6u;  // an even frame count, a different time each time
      m.start(cyc(kStart + 1000u), 4u * (n0 & ~1u) + piece);          // the first quarter frame sent has this piece
      m.run(400);
      r.run_to(kStart + 44100u * 2u);
      CAPTURE(int{code});
      CAPTURE(piece);
      CHECK(r.ch.state() == State::Acquired);
      CHECK(r.count(Notice::Acquired) == 1u);
      // first piece 0 is (8 - piece) mod 8 quarter frames in, and the cycle after it is whole 8 later
      const std::uint32_t qf_to_piece7 = ((8u - piece) & 7u) + 7u;
      CHECK(r.first(Notice::Acquired) <= kStart + 1000u + qf_to_piece7 * 44100u / (4u * kFps[code]) + 3u * kBlock);
      std::int64_t e = 0;
      NEED(time_error(r, m, kStart + 44100u * 2u - 500u, e));
      CHECK_MESSAGE((e < kSample / 4 && e > -kSample / 4), "error " << e);
    }
  }
  // across midnight: 23:59:58:00 for four seconds
  for (std::uint8_t code = 0; code < 4; ++code) {
    Rig r(kStart);
    Master m(r, code);
    m.start(cyc(kStart + 1000u), 4u * (kFramesPerDay[code] - 2u * kFps[code]));
    m.run(700);
    r.run_to(kStart + 44100u * 4u);
    CAPTURE(int{code});
    CHECK(r.ch.state() == State::Acquired);
    CHECK(r.count(Notice::Relocated) == 0u);
    CHECK(r.ch.stats().relocations == 0u);
    CHECK(r.ch.stats().resyncs == 0u);
    for (std::uint64_t s = kStart + 44100u * 3u; s < kStart + 44100u * 4u; s += 1999u) {
      std::int64_t e = 0;
      NEED(time_error(r, m, s, e));  // compared as a circle: the day wrapped in the middle
      CHECK_MESSAGE((e < kSample / 4 && e > -kSample / 4), "sample " << s << " error " << e);
    }
  }
}

TEST_CASE("mtc in: the USB port, stamped by its frame, is read as well") {
  Rig r(kStart, midi::Port::Usb);
  Master m(r, 1);
  m.port = midi::Port::Usb;
  m.start(cyc(kStart + 1000u), 4u * kHourFrames[1]);
  m.run(400);
  r.run_to(kStart + 44100u * 3u);
  CHECK(r.ch.state() == State::Acquired);
  std::int64_t e = 0;
  NEED(time_error(r, m, kStart + 44100u * 2u, e));
  CHECK_MESSAGE((e < kSample / 4 && e > -kSample / 4), "error " << e);
}

TEST_CASE("mtc in: +-2 ms jitter and one quarter frame in eight dropped: an Armed and Locked chaser stays locked and its time stays within a millisecond") {
  for (const std::uint8_t code : {std::uint8_t{1}, std::uint8_t{0}, std::uint8_t{2}, std::uint8_t{3}}) {
    Rig r(kStart);
    Master m(r, code);
    m.jitter_us = 2000;
    m.drop_k = 1;
    m.drop_n = 8;
    m.start(cyc(kStart + 1000u), 4u * kHourFrames[code] + 3u);
    m.run(30u * 4u * kFps[code]);  // 30 s
    bool armed = false;
    std::int64_t worst = 0;
    std::uint32_t bad_state = 0;
    for (std::uint64_t until = kStart + 4410u; until < kStart + 44100u * 29u; until += kBlock * 4u) {
      r.run_to(until);
      if (!armed && r.ch.state() == State::Acquired) {
        CHECK(r.ch.arm(r.ch.epoch()));
        CHECK(r.ch.state() == State::Armed);
        CHECK(r.ch.lock());
        armed = true;
      }
      const State st = r.ch.state();
      if (armed && st != State::Locked && st != State::Freewheel) {
        ++bad_state;
      }
      std::int64_t e = 0;
      if (armed && until > kStart + 44100u * 4u && time_error(r, m, until, e)) {
        worst = std::max(worst, e < 0 ? -e : e);
      }
    }
    CAPTURE(int{code});
    CHECK(armed);
    CHECK(bad_state == 0u);
    CHECK(quiet_run(r));
    CHECK(r.ch.stats().relocations == 0u);
    CHECK(r.ch.stats().resyncs == 0u);
    CHECK(r.ch.stats().stops == 0u);
    CHECK(r.ch.stats().slips == 0u);
    CHECK(r.ch.stats().refits == 0u);
    CHECK(r.ch.stats().ignored == 0u);
    CHECK(r.ch.state() == State::Locked);
    CHECK(r.ch.loop().stats().reacquires == 0u);
    CHECK(r.ch.stats().cycles > 100u);  // whole cycles still come now and then, and every one agreed with the mapping
    CHECK_MESSAGE(worst < kSample * 36, "worst error " << worst / (kSample / 1000) << " milli-samples");  // sampled every 4 blocks (a quarter of a millisecond apart) once settled: under 36 samples, 0.8 ms
    MESSAGE("code " << int{code} << ": worst error " << worst / (kSample / 1000) << " milli-samples, Freewheel blocks " << r.blocks_freewheel << ", cycles " << r.ch.stats().cycles);
  }
}

TEST_CASE("mtc in: a master slowed to gaps of 1.5 to 2.5 periods mid-run is not followed: the run stops, and the master back at its rate is followed again") {
  // Every quarter frame of such a master reads to the loop as a dropout and its period never moves: the chaser has to see the dropouts for what they are.
  for (const std::int32_t slow_ppm : {-340000, -500000, -592000}) {  // gaps of 1.52, 2.0 and 2.45 periods
    CAPTURE(slow_ppm);
    const std::uint64_t slow_den = static_cast<std::uint64_t>(1000000 + slow_ppm);
    Rig r(kStart);
    Master m(r, 1);
    m.start(cyc(kStart + 1000u), 4u * kHourFrames[1]);
    m.run(200);
    const std::uint64_t slow_from = kStart + 1000u + 200u * 441u;
    m.set_rate(slow_ppm);
    m.run(300);
    const std::uint64_t slow_to = slow_from + 300ull * 441ull * 1000000ull / slow_den;
    m.set_rate(0);
    m.run(800);
    const Master tail = m;
    r.run_to(slow_from - 441u);
    NEED(r.ch.state() == State::Acquired);
    CHECK(r.ch.arm(r.ch.epoch()));
    CHECK(r.ch.lock());
    r.run_to(slow_to - 441u);
    // 12 slow quarter frames later at the latest, the run is told to stop (the master is not followed), and nothing is Locked or Armed any more
    CHECK(r.count(Notice::Stopped) == 1u);
    CHECK(r.first(Notice::Stopped) > slow_from);
    CHECK(r.first(Notice::Stopped) <= slow_from + 12ull * 441ull * 1000000ull / slow_den + 3u * kBlock);  // measured: 7, 4 and 3 slow quarter frames
    CHECK(r.ch.stats().slips == 1u);                  // it is the slip check that stops it (4 slips among the first slow quarter frames), not the rate gate (rate_hold_qf = 16 quarter frames outside the limits, counted once the loop has moved the rate out of them)
    CHECK(r.ch.state() != State::Locked);
    CHECK(r.ch.state() != State::Armed);
    CHECK(r.ch.state() != State::Acquired);
    CHECK(r.count(Notice::Acquired) == 1u);  // not again while it crawls
    CHECK(iabs(r.ch.rate_ppm() - slow_ppm) < 20000);  // and the rate it reports is the crawl, not the old rate
    // the master back at its rate: followed again, and its time known to a sample
    r.run_to(kStart + 1000u + (200u + 800u) * 441u + 300ull * 441ull * 1000000ull / slow_den - 441u);
    CHECK(r.ch.state() == State::Acquired);
    CHECK(r.count(Notice::Acquired) == 2u);
    CHECK_FALSE(r.ch.refused());
    std::int64_t e = 0;
    NEED(time_error(r, tail, kStart + 1000u + (200u + 800u) * 441u + 300ull * 441ull * 1000000ull / slow_den - 3000u, e));
    CHECK_MESSAGE((e < kSample && e > -kSample), "error " << e);
  }
}

TEST_CASE("mtc in: a rate step that stays inside the limits does not stop a locked run, and costs no slip") {
  // The loop overshoots an abrupt step (a +4 % step goes to 8 % and back in about 25 quarter frames): the rate gate rides that out, and the pieces keep step with the ticks. At 25 fps
  // (441 samples a quarter frame) and at 30 fps (367.5: the same count of quarter frames is a shorter time, the gate counts quarter frames).
  struct Step {
    std::uint8_t code;
    std::int32_t ppm;
  };
  for (const Step sc : {Step{1, 40000}, Step{1, -40000}, Step{1, 25000}, Step{3, 40000}}) {
    const std::int32_t step_ppm = sc.ppm;
    CAPTURE(static_cast<int>(sc.code));
    CAPTURE(step_ppm);
    const auto samples = [&sc](std::uint64_t quarter_frames) { return quarter_frames * kQfCycles[sc.code] * 441ull / 1800000ull; };  // 44.1 kHz: 1,800,000 cycles are 441 samples
    Rig r(kStart);
    Master m(r, sc.code);
    m.start(cyc(kStart + 1000u), 4u * kHourFrames[sc.code]);
    m.run(300);
    m.set_rate(step_ppm);
    m.run(900);
    const Master tail = m;
    const std::uint64_t step_from = kStart + 1000u + samples(300);
    const std::uint64_t end = step_from + samples(880) * 1000000ull / static_cast<std::uint64_t>(1000000 + step_ppm);
    r.run_to(step_from - samples(10));
    NEED(r.ch.state() == State::Acquired);
    CHECK(r.ch.arm(r.ch.epoch()));
    CHECK(r.ch.lock());
    const std::uint32_t epoch = r.ch.epoch();
    const std::uint32_t resyncs = r.ch.stats().resyncs;
    r.run_to(end);
    CHECK(r.count(Notice::Stopped) == 0u);
    CHECK(r.count(Notice::Relocated) == 0u);
    CHECK(r.ch.stats().slips == 0u);
    CHECK(r.ch.state() == State::Locked);
    CHECK_FALSE(r.ch.refused());
    // the mapping moves only by a resync (the master's own count wins over the prediction): none at 25 fps; at 30 fps the +4 % step costs a few (5 in the first CI run), so the epoch is held to the resyncs
    // and not to zero, while no run is stopped, relocated or slipped
    CHECK(r.ch.epoch() - epoch == r.ch.stats().resyncs - resyncs);
    MESSAGE("code " << static_cast<int>(sc.code) << ", step " << step_ppm << " ppm: resyncs " << (r.ch.stats().resyncs - resyncs) << ", epoch +" << (r.ch.epoch() - epoch));
    CHECK(iabs(r.ch.rate_ppm() - step_ppm) < 2000);
    std::int64_t e = 0;
    NEED(time_error(r, tail, end - 3000u, e));
    CHECK_MESSAGE((e < kSample && e > -kSample), "error " << e);
  }
}

TEST_CASE("mtc in: a loop that is fitting again under a mapping has lost the master: the run stops, and the reading starts again") {
  // A loop that re-acquires (here made to by a gap of 5 quarter frames with max_dropout 2; in the shipped settings it needs a gap past 24) restarts its fit and its ticks are no longer
  // the master's count: the chaser must neither shift its mapping nor refit under it.
  mtc::Chaser::Config cfg;
  cfg.loop.max_dropout = 2;
  Rig r(kStart);
  r.ch.configure(cfg);
  Master m(r, 1);
  m.start(cyc(kStart + 1000u), 4u * kHourFrames[1]);
  m.run(200);
  m.quiet(5);
  m.run(600);
  r.run_to(kStart + 1000u + 190u * 441u);
  NEED(r.ch.state() == State::Acquired);
  CHECK(r.ch.arm(r.ch.epoch()));
  CHECK(r.ch.lock());
  r.run_to(kStart + 44100u * 7u);
  CHECK(r.count(Notice::Stopped) == 1u);
  CHECK(r.ch.stats().fit_losses == 1u);
  CHECK(r.ch.stats().refits == 0u);
  CHECK(r.ch.stats().relocations == 0u);
  CHECK(r.ch.stats().resyncs == 0u);
  CHECK(r.first(Notice::Stopped) > kStart + 1000u + 200u * 441u);
  CHECK(r.first(Notice::Stopped) < kStart + 1000u + 215u * 441u);
  CHECK(r.ch.state() == State::Acquired);  // and read again
  CHECK(r.count(Notice::Acquired) == 2u);
  std::int64_t e = 0;
  NEED(time_error(r, m, kStart + 44100u * 7u - 1000u, e));
  CHECK_MESSAGE((e < kSample && e > -kSample), "error " << e);
}

TEST_CASE("mtc in: a quarter frame lost among the first sixteen, whichever it is, does not hurt the reading") {
  // The loop's fit counts a gap of two periods as one tick (its "odd" path, or a long second interval): a mapping built on its ticks would be one quarter frame off, and its
  // period too long. The first whole cycle after the loss must still give the right time, and the rate must not be refused for it.
  for (std::uint32_t lost = 1; lost <= 15; ++lost) {
    Rig r(kStart);
    Master m(r, 1);
    m.start(cyc(kStart + 1000u), 4u * kHourFrames[1] + 3u);
    m.run(lost);
    m.quiet(1);  // the quarter frame number `lost` is not sent
    m.run(400);
    r.run_to(kStart + 44100u * 3u);
    CAPTURE(lost);
    CHECK(r.ch.state() == State::Acquired);
    CHECK(r.count(Notice::Acquired) == 1u);
    CHECK(quiet_run(r));
    CHECK_FALSE(r.ch.refused());
    CHECK(r.ch.stats().relocations == 0u);
    CHECK(r.ch.stats().resyncs == 0u);
    CHECK(r.ch.stats().slips == 0u);
    // lock within the fit and a cycle after the loss: 8 + 15 quarter frames, and a few
    CHECK(r.first(Notice::Acquired) <= kStart + 1000u + (lost + 30u) * 441u);
    std::int64_t e = 0;
    NEED(time_error(r, m, kStart + 44100u * 3u - 500u, e));
    CHECK_MESSAGE((e < kSample / 4 && e > -kSample / 4), "error " << e);
  }
  // many losses, from the second quarter frame on: 1 in 4 at random, twenty masters
  for (std::uint32_t seed = 1; seed <= 20; ++seed) {
    Rig r(kStart);
    Master m(r, 1);
    m.rng = seed * 7919u;
    m.drop_k = 1;
    m.drop_n = 4;
    m.clean_first = 0;
    m.start(cyc(kStart + 1000u), 4u * kHourFrames[1] + (seed & 7u));
    m.run(800);
    r.run_to(kStart + 44100u * 6u);
    CAPTURE(seed);
    CHECK(r.ch.state() == State::Acquired);
    CHECK(quiet_run(r));
    CHECK(r.ch.stats().relocations == 0u);
    CHECK(r.ch.stats().slips == 0u);
    std::int64_t e = 0;
    NEED(time_error(r, m, kStart + 44100u * 6u - 500u, e));
    CHECK_MESSAGE((e < kSample && e > -kSample), "error " << e);
  }
}

TEST_CASE("mtc in: a gap of 2 frames is a Freewheel and the run goes on in step; a gap of 5 frames is the master stopping") {
  for (const std::uint8_t code : {std::uint8_t{1}, std::uint8_t{3}}) {
    Rig r(kStart);
    Master m(r, code);
    m.start(cyc(kStart + 1000u), 4u * kHourFrames[code]);
    m.run(100);
    const std::uint64_t t_arm = kStart + 1000u + 101u * 44100u / (4u * kFps[code]);  // two periods after the last of the 100 quarter frames
    r.run_to(t_arm);
    CAPTURE(int{code});
    NEED(r.ch.state() == State::Acquired);
    CHECK(r.ch.arm(r.ch.epoch()));
    CHECK(r.ch.lock());
    const std::uint32_t epoch = r.ch.epoch();
    // two frames of silence: 8 quarter frames
    m.quiet(8);
    m.run(400);
    bool saw_freewheel = false;
    for (std::uint64_t s = t_arm; s < kStart + 44100u * 3u; s += kBlock) {
      r.run_to(s + kBlock);
      saw_freewheel = saw_freewheel || r.ch.state() == State::Freewheel;
    }
    CHECK(saw_freewheel);
    CHECK(r.ch.state() == State::Locked);  // the next quarter frame brought it back
    CHECK(quiet_run(r));
    CHECK(r.ch.epoch() == epoch);  // the mapping was never touched
    std::int64_t e = 0;
    NEED(time_error(r, m, kStart + 44100u * 3u - 1000u, e));
    CHECK_MESSAGE((e < kSample && e > -kSample), "error " << e);  // under a sample after the gap
    // five frames of silence: 20 quarter frames
    const std::uint64_t quiet_from = cyc(kStart) ;  // (only to keep the compiler quiet about unused helpers)
    (void)quiet_from;
    m.quiet(20);
    m.run(400);
    r.run_to(kStart + 44100u * 6u);
    CHECK(r.count(Notice::Stopped) == 1u);
    CHECK(r.ch.stats().stops == 1u);
    // the stop came within four frames and a block of the last quarter frame (the master's last was at q = 100 + 200 - 1 periods in; the gap began there)
    CHECK(r.ch.epoch() != epoch);
    // the master came back: read again from the beginning, and followed
    CHECK(r.ch.state() == State::Acquired);
    CHECK(r.count(Notice::Acquired) == 2u);
    NEED(time_error(r, m, kStart + 44100u * 6u - 1000u, e));
    CHECK_MESSAGE((e < kSample / 4 && e > -kSample / 4), "error " << e);
  }
}

TEST_CASE("mtc in: the stop comes four frames after the last quarter frame, whatever the poll does") {
  Rig r(kStart);
  Master m(r, 1);
  m.start(cyc(kStart + 1000u), 4u * kHourFrames[1]);
  m.run(60);
  // the last quarter frame was the 60th, 59 periods after the first; the master is silent from there on
  const std::uint64_t last_qf = kStart + 1000u + 59u * 441u;
  r.run_to(last_qf + 2u * 441u);
  NEED(r.ch.state() == State::Acquired);
  CHECK(r.ch.arm(r.ch.epoch()));
  r.run_to(kStart + 44100u * 3u);
  CHECK(r.count(Notice::Stopped) == 1u);
  const std::uint64_t stopped = r.first(Notice::Stopped);
  CHECK(stopped > last_qf + 16u * 441u);
  CHECK(stopped <= last_qf + 16u * 441u + 3u * kBlock);  // four frames (16 periods) and a block or two
  CHECK(r.ch.state() == State::Idle);
}

TEST_CASE("mtc in: a jump of 10 s while running is a relocation; a jump of a frame is a resync; a full frame while running is a relocation") {
  // One master, all its segments queued first: 200 quarter frames; +1 frame; +10 s; a full frame in the gap before a quarter frame; and on.
  Rig r(kStart);
  Master m(r, 1);
  m.start(cyc(kStart + 1000u), 4u * kHourFrames[1]);
  m.run(200);
  m.jump(4);
  m.run(100);
  const Master seg2 = m;
  m.jump(10 * 100);  // 10 s at 25 fps: 1000 quarter frames
  m.run(200);
  const Master seg3 = m;
  m.full_at(m.q_at(m.k) / 4u, m.cycles_at(m.k) - 4000u * 180u);  // 4 ms before the next quarter frame, after the one before it has gone
  m.run(300);
  const Master seg4 = m;
  const std::uint64_t seg2_start = kStart + 1000u + 200u * 441u;
  const std::uint64_t seg3_start = seg2_start + 100u * 441u;
  const std::uint64_t seg4_start = seg3_start + 200u * 441u;

  r.run_to(seg2_start - 441u);  // the master is on its 200th quarter frame
  NEED(r.ch.state() == State::Acquired);
  CHECK(r.ch.arm(r.ch.epoch()));
  CHECK(r.ch.lock());
  const std::uint32_t epoch0 = r.ch.epoch();
  CHECK(r.ch.stats().relocations == 0u);

  // +1 frame (4 quarter frames): a resync, the master's own count wins, no notice
  r.run_to(seg3_start - 441u);
  CHECK(r.count(Notice::Relocated) == 0u);
  CHECK(r.ch.stats().resyncs >= 1u);
  CHECK(r.ch.state() == State::Locked);
  CHECK(r.ch.epoch() != epoch0);
  std::int64_t e = 0;
  NEED(time_error(r, seg2, seg3_start - 3000u, e));
  CHECK_MESSAGE((e < kSample && e > -kSample), "after the resync: error " << e);

  // +10 s: a relocation, the chaser is Acquired at the new time once the cycle that showed it is whole
  const std::uint32_t epoch1 = r.ch.epoch();
  r.run_to(seg4_start - 441u);
  CHECK(r.count(Notice::Relocated) == 1u);
  CHECK(r.ch.stats().relocations == 1u);
  CHECK(r.ch.state() == State::Acquired);  // Relocated took it out of Locked; the whole cycle that showed the move made it Acquired again
  CHECK(r.ch.epoch() != epoch1);
  NEED(time_error(r, seg3, seg4_start - 3000u, e));
  CHECK_MESSAGE((e < kSample && e > -kSample), "after the relocation: error " << e);
  // the notice came within a cycle and a half of the jump (the first whole cycle after it)
  CHECK(r.first(Notice::Relocated) >= seg3_start);
  CHECK(r.first(Notice::Relocated) <= seg3_start + 16u * 441u + 3u * kBlock);

  // a full frame while Armed: a relocation, the chaser reads again and is Acquired at the same time
  CHECK(r.ch.arm(r.ch.epoch()));
  CHECK(r.ch.lock());
  r.run_to(seg4_start + 300u * 441u - 441u);
  CHECK(r.count(Notice::Relocated) == 2u);
  CHECK(r.ch.stats().relocations == 2u);
  CHECK(r.ch.stats().full == 1u);
  CHECK(r.ch.state() == State::Acquired);
  CHECK(r.first(Notice::Locate) == 0u);
  NEED(time_error(r, seg4, seg4_start + 300u * 441u - 3000u, e));
  CHECK_MESSAGE((e < kSample && e > -kSample), "after the full frame: error " << e);
}

TEST_CASE("mtc in: pieces in reverse order are not followed; a run that was locked is stopped, and forward again is followed") {
  Rig r(kStart);
  Master m(r, 1);
  const std::uint32_t q0 = 4u * kHourFrames[1] + 400u;
  m.start(cyc(kStart + 1000u), q0 + 8u * 3u + 5u, -1);
  m.run(200);  // reverse from the start: pieces 7..0, the time falling
  r.run_to(kStart + 44100u * 2u + 3000u);
  CHECK(r.ch.reverse());
  CHECK(r.ch.state() == State::Reading);
  CHECK(r.count(Notice::Acquired) == 0u);
  CHECK(r.ch.stats().cycles == 0u);
  CHECK_FALSE(r.ch.time_known());
  // forward from there: followed
  m.start(cyc(kStart + 44100u * 2u + 3000u + 1000u), q0 + 4000u, 1);
  m.run(200);
  r.run_to(kStart + 44100u * 4u + 3000u);
  CHECK_FALSE(r.ch.reverse());
  CHECK(r.ch.state() == State::Acquired);
  CHECK(r.count(Notice::Acquired) == 1u);
  CHECK(r.ch.arm(r.ch.epoch()));
  CHECK(r.ch.lock());
  // reverse again, now: the run is stopped
  m.start(cyc(kStart + 44100u * 4u + 3000u + 1000u), q0 + 8000u + 3u, -1);
  m.run(100);
  r.run_to(kStart + 44100u * 5u + 3000u);
  CHECK(r.count(Notice::Stopped) == 1u);
  CHECK(r.ch.state() == State::Reading);
  CHECK(r.ch.reverse());
  CHECK(r.first(Notice::Stopped) <= kStart + 44100u * 4u + 3000u + 1000u + 10u * 441u);  // within the 3 backward steps that tell it, and then some
}

TEST_CASE("mtc in: a master 0.999 or 1.001 times our rate is followed, 1.07 and 0.93 are refused, a master that leaves the limits stops a run") {
  for (const std::int32_t ppm : {-1000, 1000, -999, 40000, -50000}) {
    Rig r(kStart);
    Master m(r, ppm == -999 ? std::uint8_t{3} : std::uint8_t{1});  // 29.97 fps sent as 30 for -999
    m.ppm = ppm;
    m.start(cyc(kStart + 1000u), 4u * kHourFrames[m.code]);
    m.run(500);
    r.run_to(kStart + 44100u * 4u);
    CAPTURE(ppm);
    CHECK(r.ch.state() == State::Acquired);
    CHECK_FALSE(r.ch.refused());
    CHECK(iabs(r.ch.rate_ppm() - ppm) < 150);  // the rate it reports is the master's
    std::int64_t e = 0;
    NEED(time_error(r, m, kStart + 44100u * 3u, e));
    CHECK_MESSAGE((e < kSample * 2 && e > -kSample * 2), "error " << e);
  }
  for (const std::int32_t ppm : {70000, -70000, 150000}) {
    Rig r(kStart);
    Master m(r, 1);
    m.ppm = ppm;
    m.start(cyc(kStart + 1000u), 4u * kHourFrames[1]);
    m.run(500);
    r.run_to(kStart + 44100u * 4u);
    CAPTURE(ppm);
    CHECK(r.ch.state() == State::Reading);
    CHECK(r.ch.refused());
    CHECK(r.count(Notice::Acquired) == 0u);
    CHECK(iabs(r.ch.rate_ppm() - ppm) < 500);
    // refused means no time: nothing to chase
    CHECK(r.ch.time_known());  // the cycles are still read, only the rate gates
  }
  // a locked run whose master changes its rate by 7 % at once, either way, is out of the rate limits for good: the loop settles at the new rate and the rate gate stops the run (after the
  // loop's overshoot has been ridden out: rate_hold_qf); the master back at its own rate is followed again
  for (const std::int32_t step_ppm : {70000, -70000}) {
    CAPTURE(step_ppm);
    const std::uint64_t den = static_cast<std::uint64_t>(1000000 + step_ppm);
    Rig r(kStart);
    Master m(r, 1);
    m.start(cyc(kStart + 1000u), 4u * kHourFrames[1]);
    m.run(300);
    m.set_rate(step_ppm);
    m.run(600);
    m.set_rate(0);
    m.run(1500);
    const Master tail = m;
    const std::uint64_t step_from = kStart + 1000u + 300u * 441u;
    const std::uint64_t step_to = step_from + 600ull * 441ull * 1000000ull / den;
    r.run_to(step_from - 441u * 10u);
    NEED(r.ch.state() == State::Acquired);
    CHECK(r.ch.arm(r.ch.epoch()));
    CHECK(r.ch.lock());
    r.run_to(step_to - 441u);
    CHECK(r.count(Notice::Stopped) >= 1u);
    CHECK(r.first(Notice::Stopped) > step_from);
    // upper bound: the loop's move out of the limits, then rate_hold_qf = 16 quarter frames; measured 25,164 samples after the +7 % step (57 quarter frames), the bound is 80 nominal ones (0.8 s)
    CHECK_MESSAGE(r.first(Notice::Stopped) <= step_from + 80ull * 441ull, "stopped " << (r.first(Notice::Stopped) - step_from) << " samples after the step");
    CHECK(r.ch.state() != State::Locked);
    CHECK(r.ch.state() != State::Armed);
    const std::uint64_t end = step_to + 1500ull * 441ull - 441u;
    r.run_to(end);
    CHECK_FALSE(r.ch.refused());
    CHECK(r.ch.state() == State::Acquired);
    std::int64_t e = 0;
    NEED(time_error(r, tail, end - 3000u, e));
    CHECK_MESSAGE((e < kSample * 2 && e > -kSample * 2), "error " << e);
  }
}

TEST_CASE("mtc in: a full frame while the master is stopped is a locate request") {
  for (std::uint8_t code = 0; code < 4; ++code) {
    CAPTURE(int{code});
    for (const std::uint8_t id : {std::uint8_t{0x7F}, std::uint8_t{0x00}, std::uint8_t{0x15}}) {
      Rig r(kStart);
      Master m(r, code);
      const std::uint32_t n = kHourFrames[code] + 12345u;
      const std::uint64_t begin = cyc(kStart + 5000u);
      m.full_at(n, begin, id);
      r.run_to(kStart + 44100u);
      CHECK(r.count(Notice::Locate) == 1u);
      CHECK(r.ch.state() == State::Idle);
      CHECK(r.full_events == 1u);
      const mtc::Chaser::Locate& l = r.ch.locate();
      CHECK(l.valid);
      CHECK(l.code == code);
      const Lab w = walk(n, code);
      CHECK(l.label == mtc::Time{static_cast<std::uint8_t>(w.h), static_cast<std::uint8_t>(w.m), static_cast<std::uint8_t>(w.s), static_cast<std::uint8_t>(w.f)});
      const std::int64_t d = static_cast<std::int64_t>(l.time) - static_cast<std::int64_t>(core::q32_from_cycles(4u * static_cast<std::uint64_t>(n) * kQfCycles[code]));
      CHECK(d < kSample / 100);
      CHECK(d > -kSample / 100);
      // the stamp: the sample at which the master began the message, to the sample
      const std::uint64_t want = kStart + 5000u;
      CHECK(l.sample + 1u >= want);
      CHECK(l.sample <= want + 1u);
      // the bytes of the SysEx went to the note path complete, as before
      CHECK(r.passed.size() == 10u);
      CHECK(r.passed.front() == 0xF0u);
      CHECK(r.passed.back() == 0xF7u);
    }
  }
  // damaged ones are no locate: the bytes pass and nothing else happens
  Rig r(kStart);
  Master m(r, 1);
  std::vector<std::uint8_t> bad = {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 0x3B, 0x2D, 0x19, 0xF7};  // frame 25 at 25 fps
  m.push(bad.data(), 10, cyc(kStart + 5000u));
  bad[8] = 0x0C;
  bad[4] = 0x02;
  m.push(bad.data(), 10, cyc(kStart + 6000u));
  r.run_to(kStart + 44100u);
  CHECK(r.count(Notice::Locate) == 0u);
  CHECK(r.full_events == 0u);
  CHECK(r.passed.size() == 20u);
  CHECK_FALSE(r.ch.locate().valid);
  // a full frame, then the quarter frames of a run: the locate, then a reading at once
  Rig r2(kStart);
  Master m2(r2, 1);
  m2.start(cyc(kStart + 20000u), 4u * kHourFrames[1] + 200u);
  m2.full_at(kHourFrames[1] + 50u, cyc(kStart + 5000u));
  m2.run(100);
  r2.run_to(kStart + 20000u + 60u * 441u);
  CHECK(r2.count(Notice::Locate) == 1u);
  CHECK(r2.ch.state() == State::Acquired);
  CHECK(r2.first(Notice::Locate) < r2.first(Notice::Acquired));
}

TEST_CASE("mtc in: a MIDI clock byte inside a full frame or between the bytes of a quarter frame is a clock, and the message is whole") {
  // the DIN port carries both: MIDI clock follow on and MTC IN on
  midi::MidiIn::Config c;
  c.clock_enabled = true;
  c.clock_port = midi::Port::Din;
  c.mtc_enabled = true;
  c.mtc_port = midi::Port::Din;
  midi::RtInRing ring;
  midi::ClockMap map;
  midi::ClockDll dll;
  midi::MidiIn in;
  in.configure(c);
  map.set_ref(0, 0);
  dll.reset(0, core::q32_from_cycles(3750000u));
  const std::uint8_t bytes[] = {0xF0, 0x7F, 0x7F, 0xF8, 0x01, 0x01, 0x21, 0xF8, 0x3B, 0x2D, 0x0C, 0xF8, 0xF7,  // a full frame with a clock inside three times
                                0xF1, 0xF8, 0x4D,                                                                // a quarter frame with a clock inside
                                0xF8, 0xF1, 0x4E, 0xF8};
  std::uint32_t t = 1000000;
  for (const std::uint8_t b : bytes) {
    CHECK(ring.push(midi::RtItem{t, b, 0, 0}));
    t += 57600;
  }
  midi::TransportEvent ev[4];
  midi::RtItem pass[32];
  midi::MtcEvent mt[8];
  const midi::MidiIn::Drained d = in.drain(ring, map, dll, 256, ev, 4, pass, 32, mt, 8);
  CHECK(d.mtc == 3u);
  CHECK(in.stats().clocks == 6u);  // the clocks reached the loop
  NEED(d.mtc == 3u);
  CHECK(mt[0].kind == midi::MtcEvent::Kind::Full);
  CHECK(mt[0].value == mtc::pack_full(mtc::Time{1, 59, 45, 12}, 1));
  CHECK(mt[1].data == 0x4Du);
  CHECK(mt[2].data == 0x4Eu);
  // the SysEx's ten bytes passed in order without the clocks
  NEED(d.passed == 10u);
  const std::uint8_t want[10] = {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 0x3B, 0x2D, 0x0C, 0xF7};
  for (std::uint32_t i = 0; i < 10; ++i) {
    CHECK(pass[i].byte == want[i]);
  }
}

TEST_CASE("mtc in: A3's generator is the master: its full frame, its quarter frames and its time are read back exactly") {
  if (!kRate44100) {
    return;
  }
  for (const std::uint32_t fps : {24u, 25u, 30u}) {
    CAPTURE(fps);
    Rig r(kStart);
    mtc::Generator g;
    g.configure(fps, 1);
    const std::uint64_t at = kStart + 3000u;
    g.anchor(at, 0);
    // the generator's events over 3 s of blocks, as bytes with the stamps the ISR would give (DIN)
    const std::uint8_t code = mtc::rate_code(fps);
    Master m(r, code);  // only for its pushing
    for (std::uint64_t bs = kStart; bs < kStart + 44100u * 3u; bs += kBlock) {
      audio::SeqEvent out[mtc::Generator::kMaxEvents];
      const std::uint32_t n = g.step(bs, kBlock, out);
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint64_t begin = core::cycles_from_samples_sfrac(bs + out[i].offset, out[i].value);
        if (out[i].kind == audio::EventKind::MidiMtcQf) {
          const std::uint8_t b[2] = {0xF1, out[i].a};
          m.push(b, 2, begin);
        } else {
          std::uint8_t b[10];
          b[0] = 0xF0;
          mtc::full_frame_body(static_cast<std::uint32_t>(out[i].value), b + 1);
          b[9] = 0xF7;
          m.push(b, 10, begin);
        }
      }
    }
    r.run_to(kStart + 44100u * 3u);
    CHECK(r.count(Notice::Locate) == 1u);
    CHECK(r.ch.locate().label == mtc::Time{1, 0, 0, 0});
    CHECK(r.ch.locate().code == code);
    CHECK(r.ch.locate().time == core::q32_from_samples(158760000u));  // 01:00:00:00 in samples
    CHECK(r.ch.state() == State::Acquired);
    CHECK(r.ch.code() == code);
    CHECK(iabs(r.ch.rate_ppm()) < 20);
    for (std::uint64_t s = kStart + 44100u * 2u; s < kStart + 44100u * 3u; s += 3001u) {
      core::Q32 got = 0;
      NEED(r.ch.master_time_at(s, got));
      const std::int64_t want = static_cast<std::int64_t>(core::q32_from_samples(158760000u)) + static_cast<std::int64_t>((s - at) << 32);
      const std::int64_t e = static_cast<std::int64_t>(got) - want;
      CHECK_MESSAGE((e < kSample / 4 && e > -kSample / 4), "sample " << s << " error " << e);
    }
  }
}
