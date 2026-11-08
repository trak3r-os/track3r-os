// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI 1.0 messages as the timing core sees them (OS-005c): at most three bytes, built by value. A SysEx message is not one of these: it
// travels as a run of three-byte chunks (midi::OutItem::more), because the emitter's whole job is to know where the bytes of one message
// end and where a real-time byte may go in. No heap, no I/O, nothing that blocks.

#include <cstdint>

namespace o3os::midi {

// System real-time messages (0xF8..0xFF): one byte, no data, and "may be inserted into the middle of another message" without cancelling
// running status (MIDI 1.0 detailed specification, section on system real-time messages; the radar card midi-clock-sync cites it).
inline constexpr std::uint8_t kClock = 0xF8;
inline constexpr std::uint8_t kStart = 0xFA;
inline constexpr std::uint8_t kContinue = 0xFB;
inline constexpr std::uint8_t kStop = 0xFC;
inline constexpr std::uint8_t kActiveSense = 0xFE;
inline constexpr std::uint8_t kSystemReset = 0xFF;

inline constexpr std::uint8_t kQuarterFrame = 0xF1;  // OS-MTC: one data byte `0nnndddd`
inline constexpr std::uint8_t kSongPosition = 0xF2;  // two data bytes, 7 bits each, least significant first
inline constexpr std::uint8_t kSysExStart = 0xF0;
inline constexpr std::uint8_t kSysExEnd = 0xF7;

constexpr bool is_realtime(std::uint8_t b) noexcept { return b >= 0xF8u; }
// A status byte (bit 7 set): the first byte of a message, or a real-time byte.
constexpr bool is_status(std::uint8_t b) noexcept { return (b & 0x80u) != 0u; }

// Data bytes that follow a status byte: channel voice 0x80..0xEF (program change and channel pressure take one), system common
// 0xF1 (one), 0xF2 (two), 0xF3 (one), 0xF6, 0xF7 (none), 0xF0 (SysEx: not a fixed length, 0 here). Real-time bytes take none.
constexpr std::uint8_t data_bytes_of(std::uint8_t status) noexcept {
  if (status < 0x80u) {
    return 0;
  }
  if (status < 0xF0u) {
    const std::uint8_t hi = status & 0xF0u;
    return (hi == 0xC0u || hi == 0xD0u) ? 1u : 2u;
  }
  switch (status) {
    case 0xF1u:
    case 0xF3u:
      return 1;
    case 0xF2u:
      return 2;
    default:
      return 0;
  }
}

struct Msg {
  std::uint8_t len;      // 1..3
  std::uint8_t b[3];
};

constexpr Msg msg1(std::uint8_t s) noexcept { return Msg{1, {s, 0, 0}}; }
constexpr Msg msg2(std::uint8_t s, std::uint8_t d1) noexcept { return Msg{2, {s, static_cast<std::uint8_t>(d1 & 0x7Fu), 0}}; }
constexpr Msg msg3(std::uint8_t s, std::uint8_t d1, std::uint8_t d2) noexcept {
  return Msg{3, {s, static_cast<std::uint8_t>(d1 & 0x7Fu), static_cast<std::uint8_t>(d2 & 0x7Fu)}};
}

// Channel 0..15 (the wire's), note and velocity masked to 7 bits.
constexpr Msg note_on(std::uint8_t ch, std::uint8_t note, std::uint8_t vel) noexcept {
  return msg3(static_cast<std::uint8_t>(0x90u | (ch & 0x0Fu)), note, vel);
}
// Velocity 0 on the off message.
constexpr Msg note_off(std::uint8_t ch, std::uint8_t note) noexcept {
  return msg3(static_cast<std::uint8_t>(0x80u | (ch & 0x0Fu)), note, 0);
}
constexpr Msg control_change(std::uint8_t ch, std::uint8_t cc, std::uint8_t value) noexcept {
  return msg3(static_cast<std::uint8_t>(0xB0u | (ch & 0x0Fu)), cc, value);
}
constexpr Msg program_change(std::uint8_t ch, std::uint8_t program) noexcept {
  return msg2(static_cast<std::uint8_t>(0xC0u | (ch & 0x0Fu)), program);
}
// OS-INSTUI S8b: channel aftertouch (two bytes), polyphonic aftertouch, and the pitch bend (14 bits, least significant 7 first; 8192 is the centre).
constexpr Msg channel_pressure(std::uint8_t ch, std::uint8_t value) noexcept {
  return msg2(static_cast<std::uint8_t>(0xD0u | (ch & 0x0Fu)), value);
}
constexpr Msg poly_pressure(std::uint8_t ch, std::uint8_t note, std::uint8_t value) noexcept {
  return msg3(static_cast<std::uint8_t>(0xA0u | (ch & 0x0Fu)), note, value);
}
constexpr Msg pitch_bend(std::uint8_t ch, std::uint16_t value14) noexcept {
  return msg3(static_cast<std::uint8_t>(0xE0u | (ch & 0x0Fu)), static_cast<std::uint8_t>(value14 & 0x7Fu), static_cast<std::uint8_t>((value14 >> 7) & 0x7Fu));
}
// Song Position Pointer in MIDI beats (a beat is six clocks, a sixteenth note: one step of this tracker), 14 bits.
constexpr Msg song_position(std::uint16_t beats) noexcept {
  return msg3(kSongPosition, static_cast<std::uint8_t>(beats & 0x7Fu), static_cast<std::uint8_t>((beats >> 7) & 0x7Fu));
}

// USB-MIDI 1.0 event packet (chapter 4): byte 0 is the cable number in the high nibble and the code index number (CIN) in the low one,
// then the three message bytes; in memory order, which is wire order on this little-endian target. CIN 0x8 note off, 0x9 note on, 0xB
// control change, 0xC program change (two bytes), 0x3 SysEx continuing, 0x4 SysEx starts or continues, 0x5 single-byte system common, 0xF a
// single byte (real-time), 0x2 two-byte system common, 0x3 three-byte system common (the SPP).
constexpr std::uint8_t usb_cin(const Msg& m) noexcept {
  const std::uint8_t s = m.b[0];
  if (s >= 0xF8u) {
    return 0xFu;
  }
  if (s >= 0xF0u) {
    return s == kSongPosition ? 0x3u : (m.len == 2 ? 0x2u : 0x5u);
  }
  return static_cast<std::uint8_t>(s >> 4);
}
constexpr std::uint32_t usb_event(std::uint8_t cable, const Msg& m) noexcept {
  return static_cast<std::uint32_t>((static_cast<std::uint32_t>(cable & 0x0Fu) << 4) | usb_cin(m)) |
         (static_cast<std::uint32_t>(m.b[0]) << 8) | (static_cast<std::uint32_t>(m.len > 1 ? m.b[1] : 0u) << 16) |
         (static_cast<std::uint32_t>(m.len > 2 ? m.b[2] : 0u) << 24);
}

// OS-MTC A3: a SysEx message as USB-MIDI event words, F0 + `n` data bytes + F7 cut in threes (chapter 4): code index 0x4 for a chunk of three that is not the last, then the last chunk by its
// length: 0x5 (F7 alone), 0x6 (two bytes), 0x7 (three bytes). Writes (n + 2 + 2) / 3 words to `out` and returns that count; the data bytes are masked to 7 bits. `out` must hold
// that many (a full frame, n = 8, is four: F0 7F 7F | 01 01 hh | mm ss ff | F7).
constexpr std::uint32_t usb_sysex_events(std::uint8_t cable, const std::uint8_t* data, std::uint32_t n, std::uint32_t* out) noexcept {
  const std::uint32_t bytes = n + 2u;
  const std::uint32_t words = (bytes + 2u) / 3u;
  std::uint32_t k = 0;  // index in the stream F0, data..., F7
  for (std::uint32_t w = 0; w < words; ++w) {
    std::uint8_t b[3] = {0, 0, 0};
    std::uint32_t len = 0;
    for (; len < 3u && k < bytes; ++len, ++k) {
      b[len] = k == 0u ? kSysExStart : (k == bytes - 1u ? kSysExEnd : static_cast<std::uint8_t>(data[k - 1u] & 0x7Fu));
    }
    const std::uint8_t cin = len == 3u ? (k == bytes ? 0x7u : 0x4u) : (len == 2u ? 0x6u : 0x5u);
    out[w] = static_cast<std::uint32_t>((static_cast<std::uint32_t>(cable & 0x0Fu) << 4) | cin) | (static_cast<std::uint32_t>(b[0]) << 8) | (static_cast<std::uint32_t>(b[1]) << 16) |
             (static_cast<std::uint32_t>(b[2]) << 24);
  }
  return words;
}

}  // namespace o3os::midi
