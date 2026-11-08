// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Synthetic OG files for the tests of formats/og (OS-004b), written from the byte maps of
// the file format notes (docs/formats/og.md) and its default table, deliberately sharing no code with the
// readers and writers under test: an offset wrong in both would show up as a failure against the real fixtures
// (og_fixture_survey_test.cpp), not here. Everything is header-only so the target (QEMU) suite can use it.

#include "memory_io.h"

#include <o3os/formats/crc32.h>
#include <o3os/formats/format.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace o3os::test::og {

using Bytes = std::vector<std::uint8_t>;

// Two arenas of 1.8 MiB (static: far too big for a stack). The target suite has 4 MiB of RAM, so no more.
inline model::ModelArena& arena(int index) {
  static model::ModelArena arenas[2];
  return arenas[index];
}

// ---- little-endian helpers ---------------------------------------------------------------------------------

inline void p16(Bytes& b, std::size_t at, std::uint32_t v) {
  b[at] = static_cast<std::uint8_t>(v);
  b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
inline void p32(Bytes& b, std::size_t at, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void pf(Bytes& b, std::size_t at, float f) {
  std::uint32_t bits;
  std::memcpy(&bits, &f, 4);
  p32(b, at, bits);
}
inline std::uint32_t fbits(float f) {
  std::uint32_t bits;
  std::memcpy(&bits, &f, 4);
  return bits;
}
inline std::uint16_t r16(const Bytes& b, std::size_t at) {
  return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}
inline std::uint32_t r32(const Bytes& b, std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
         (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
inline void ptext(Bytes& b, std::size_t at, const std::string& s, std::size_t field) {
  for (std::size_t i = 0; i < field; ++i) b[at + i] = i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0;
}
inline void seal(Bytes& b) {  // zlib CRC-32 of everything before the last four bytes, little-endian, into them
  p32(b, b.size() - 4, formats::crc32(b.data(), b.size() - 4));
}
inline void header(Bytes& b, char m0, char m1, unsigned type, const std::uint8_t fw[4], unsigned version,
                   std::uint32_t size_field) {
  b[0] = static_cast<std::uint8_t>(m0);
  b[1] = static_cast<std::uint8_t>(m1);
  p16(b, 2, type);
  for (unsigned i = 0; i < 4; ++i) b[4 + i] = fw[i];
  for (unsigned i = 0; i < 4; ++i) b[8 + i] = static_cast<std::uint8_t>(version);
  p16(b, 12, size_field);
}

// ---- patterns (PAR-007) ------------------------------------------------------------------------------------

struct PatternSpec {
  std::uint8_t version = 5;
  unsigned tracks = 12;                                   // 8, 12 or 16 (16: tracker-ce's v5, 12,336 B)
  char magic[2] = {'K', 'S'};
  std::uint8_t fw[4] = {1, 9, 0, 1};                      // header bytes 4-7
  std::uint8_t pad[2] = {0, 0};                           // header bytes 14-15 (junk in real files)
  std::uint8_t head_tail[12] = {0, 0, 0xC8, 0x42, 0, 0, 0, 0, 0, 0, 0, 0};  // bytes 16-27: 100.0f, 0.0f, reserved
  std::uint8_t length[16] = {};                           // the length byte of each track: its last step index
  std::uint8_t step[16][128][6] = {};                     // note (i8), instrument, FX1 type, value, FX2 type, value
};

inline PatternSpec empty_pattern(unsigned tracks, unsigned version = 5) {
  PatternSpec p;
  p.tracks = tracks;
  p.version = static_cast<std::uint8_t>(version);
  for (unsigned t = 0; t < 16; ++t) {
    p.length[t] = 31;
    for (unsigned s = 0; s < 128; ++s) p.step[t][s][0] = 0xFF;  // -1: empty
  }
  return p;
}

// Notes of every kind, instruments, FX of every kind (types 0-42 only: an export keeps those and writes the core FX 43-127 and the engine FX as none), junk values on
// FX-less steps, steps past the track length, and a per-track length. Deterministic in `seed`.
inline PatternSpec rich_pattern(unsigned tracks, unsigned version, std::uint32_t seed) {
  PatternSpec p = empty_pattern(tracks, version);
  std::uint32_t x = seed * 2654435761u + 1;
  auto rnd = [&x]() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
  };
  static const std::uint8_t lengths[16] = {31, 15, 63, 127, 31, 0, 47, 31, 95, 31, 7, 31, 63, 31, 15, 31};
  for (unsigned t = 0; t < tracks; ++t) {
    p.length[t] = lengths[t];
    for (unsigned s = 0; s < 128; ++s) {
      std::uint8_t* st = p.step[t][s];
      const std::uint32_t r = rnd();
      switch (r % 9) {
        case 0: st[0] = static_cast<std::uint8_t>(rnd() % 128); st[1] = static_cast<std::uint8_t>(rnd() % 64); break;
        case 1: st[0] = 0xFC; st[1] = 0; break;                                 // off, no instrument
        case 2: st[0] = 0xFD; st[1] = static_cast<std::uint8_t>(1 + rnd() % 20); break;  // cut
        case 3: st[0] = 0xFE; st[1] = static_cast<std::uint8_t>(rnd() % 50); break;     // fade
        case 4: st[0] = static_cast<std::uint8_t>(0x80 + rnd() % 120); st[1] = 3; break;  // other negatives
        default: break;                                                         // empty
      }
      if (r % 3 == 0) {
        st[2] = static_cast<std::uint8_t>(rnd() % 43);
        st[3] = static_cast<std::uint8_t>(rnd());
      }
      if (r % 5 == 0) {
        st[4] = static_cast<std::uint8_t>(rnd() % 43);
        st[5] = static_cast<std::uint8_t>(rnd());
      }
      if (r % 7 == 0) st[3] = static_cast<std::uint8_t>(rnd());  // a leftover value on an FX-less column
    }
  }
  return p;
}

inline Bytes pattern_bytes(const PatternSpec& p) {
  const std::size_t len = 28 + 769u * p.tracks + 4;  // 6,184 (8 tracks), 9,260 (12), 12,336 (16, tracker-ce's v5)
  Bytes b(len, 0);
  header(b, p.magic[0], p.magic[1], 2, p.fw, p.version, static_cast<std::uint32_t>(len));
  b[14] = p.pad[0];
  b[15] = p.pad[1];
  for (unsigned i = 0; i < 12; ++i) b[16 + i] = p.head_tail[i];
  for (unsigned t = 0; t < p.tracks; ++t) {
    const std::size_t base = 28 + 769u * t;
    b[base] = p.length[t];
    std::memcpy(&b[base + 1], p.step[t], 768);
  }
  seal(b);
  return b;
}

// Independent calculation of the pattern-usage bitmask byte (PAR-006): tracks 1-8,
// steps before the first track's length byte, note byte >= 0 as an i8.
inline std::uint8_t usage_byte(const PatternSpec& p) {
  std::uint8_t m = 0;
  for (unsigned t = 0; t < 8; ++t) {
    for (unsigned s = 0; s < p.length[0]; ++s) {
      if (p.step[t][s][0] < 128) {
        m = static_cast<std::uint8_t>(m | (1u << t));
        break;
      }
    }
  }
  return m;
}

// ---- v6 patterns (OS-CEPAT S1, the byte map of docs/specs/OS-CEPAT.md 2.1; tracker-ce@7fd6c2c:core/source/
// mtSequencer.h:282-336, cited as facts, no code taken) ------------------------------------------------------
//
// A 16-byte header (magic 2, type u16, firmware [4], structure version [4] at offset 8, size u16 at offset 12, two
// padding bytes), then tempo f32, swing f32 and 4 reserved bytes (a 28-byte head), then n rows of one length byte
// plus 128 steps of 8 bytes, then a CRC-32 over everything before it. A step is note i8, instrument u8, fx[2] (type
// u8, value u8), volume i8, micromove i8; -1 means none for both columns. n is 8, 12 or 16 and the size field says
// the whole file: 28 + n * 1,025 + 4.

struct PatternSpec6 {
  std::uint8_t version = 6;
  unsigned tracks = 16;                                   // 8, 12 or 16
  char magic[2] = {'K', 'S'};
  std::uint8_t fw[4] = {1, 9, 0, 1};                      // header bytes 4-7
  std::uint8_t pad[2] = {0, 0};                           // header bytes 14-15
  std::uint8_t head_tail[12] = {0, 0, 0xC8, 0x42, 0, 0, 0, 0, 0, 0, 0, 0};  // bytes 16-27: 100.0f, 0.0f, reserved
  std::uint8_t length[16] = {};                           // the length byte of each track: its last step index
  std::uint8_t step[16][128][8] = {};                     // note (i8), instrument, FX1 type, value, FX2 type, value, volume (i8), micromove (i8)
};

inline PatternSpec6 empty_pattern6(unsigned tracks, unsigned version = 6) {
  PatternSpec6 p;
  p.tracks = tracks;
  p.version = static_cast<std::uint8_t>(version);
  for (unsigned t = 0; t < 16; ++t) {
    p.length[t] = 31;
    for (unsigned s = 0; s < 128; ++s) {
      p.step[t][s][0] = 0xFF;  // -1: empty note
      p.step[t][s][6] = 0xFF;  // -1: no volume
      p.step[t][s][7] = 0xFF;  // -1: no micromove
    }
  }
  return p;
}

// Notes of every kind, instruments, FX types of every class of the id map, and volume and micromove values of every
// range (negative, 0..100, above 100), on steps with and without FX columns. Deterministic in `seed`.
inline PatternSpec6 rich_pattern6(unsigned tracks, unsigned version, std::uint32_t seed) {
  PatternSpec6 p = empty_pattern6(tracks, version);
  std::uint32_t x = seed * 2654435761u + 1;
  auto rnd = [&x]() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
  };
  static const std::uint8_t lengths[16] = {31, 15, 63, 127, 31, 0, 47, 31, 95, 31, 7, 31, 63, 31, 15, 31};
  static const std::int8_t volumes[8] = {-1, -2, 0, 1, 50, 100, 101, 127};
  static const std::int8_t moves[6] = {-1, -2, 0, 50, 100, 127};
  for (unsigned t = 0; t < tracks; ++t) {
    p.length[t] = lengths[t];
    for (unsigned s = 0; s < 128; ++s) {
      std::uint8_t* st = p.step[t][s];
      const std::uint32_t r = rnd();
      switch (r % 9) {
        case 0: st[0] = static_cast<std::uint8_t>(rnd() % 128); st[1] = static_cast<std::uint8_t>(rnd() % 64); break;
        case 1: st[0] = 0xFC; st[1] = 0; break;                                       // off, no instrument
        case 2: st[0] = 0xFD; st[1] = static_cast<std::uint8_t>(1 + rnd() % 20); break;  // cut
        case 3: st[0] = 0xFE; st[1] = static_cast<std::uint8_t>(rnd() % 50); break;       // fade
        case 4: st[0] = static_cast<std::uint8_t>(0x80 + rnd() % 120); st[1] = 3; break;  // other negatives
        default: break;                                                                   // empty
      }
      if (r % 3 == 0) {
        st[2] = static_cast<std::uint8_t>(rnd() % 128);  // any type: 0..42 shared, 43..74 tracker-ce, 75..127 cleared
        st[3] = static_cast<std::uint8_t>(rnd());
      }
      if (r % 5 == 0) {
        st[4] = static_cast<std::uint8_t>(rnd() % 128);
        st[5] = static_cast<std::uint8_t>(rnd());
      }
      if (r % 4 == 0) st[6] = static_cast<std::uint8_t>(volumes[rnd() % 8]);
      if (r % 4 == 1) st[7] = static_cast<std::uint8_t>(moves[rnd() % 6]);
    }
  }
  return p;
}

inline Bytes pattern6_bytes(const PatternSpec6& p) {
  const std::size_t len = 28 + 1025u * p.tracks + 4;
  Bytes b(len, 0);
  header(b, p.magic[0], p.magic[1], 2, p.fw, p.version, static_cast<std::uint32_t>(len));
  b[14] = p.pad[0];
  b[15] = p.pad[1];
  for (unsigned i = 0; i < 12; ++i) b[16 + i] = p.head_tail[i];
  for (unsigned t = 0; t < p.tracks; ++t) {
    const std::size_t base = 28 + 1025u * t;
    b[base] = p.length[t];
    std::memcpy(&b[base + 1], p.step[t], 1024);
  }
  seal(b);
  return b;
}

// ---- project.mt (PAR-006) ----------------------------------------------------------------------------------

struct ProjectSpec {
  unsigned version = 16;
  std::string name = "Synthetic Project";
  float tempo = 133.5f;
  unsigned pattern_length = 48;
  std::vector<std::uint8_t> playlist = {1, 2, 2, 3, 12};
  std::string names[16] = {"Kick", "Snare", "Hat Closed", "Bass", "Lead 1", "Pad", "FX", "Perc",
                           "Midi 9", "Midi 10", "M11", "", "Midi 13", "Midi 14", "Midi 15", "Midi 16"};
  std::uint8_t mute[8] = {0, 1, 0, 0, 0, 0, 1, 0};
  std::uint8_t actual_pattern = 0;  // 0x114, the pattern the project opens on; 0: the first playlist entry (what Export writes)
  std::uint8_t volume[8] = {100, 86, 70, 0, 100, 55, 86, 90};
  std::uint8_t reverb_volume = 60, delay_volume = 40, reverb_mute = 1, delay_mute = 0, dry_mute = 1, dry_volume = 77;
  std::uint8_t bitmask[255] = {};
  // What the bytes the model has no field for hold.
  //   Defaults  the settings groups the version has at the values Export writes (docs/formats/og.md), the session
  //             state at what Export writes, everything else zero: a project Export would write back as it is
  //   Junk      as Defaults, with junk in the bytes Export rebuilds or defaults (session state, padding, the
  //             uninitialised bytes behind the name)
  //   Groups    as Junk, with a distinct non-default value in every byte of every settings group the version has
  enum class Fill : std::uint8_t { Defaults, Junk, Groups };
  Fill fill = Fill::Defaults;
  std::uint32_t junk = 12345;
  // Version 17 (official 1.9.2; the `observed_only` class of docs/formats/og.md section 2, unverified): the new byte at 0x80C. Zero is what a file that never
  // touched the feature holds; with Groups it is non-zero unless this says otherwise.
  std::uint8_t byte_80c = 0;
  bool own_80c = false;  // Groups: write `byte_80c` (even a zero) instead of the pattern the other group bytes get
};

inline std::size_t project_length(unsigned version) {
  switch (version) {
    case 2: return 1084;
    case 3: return 1100;
    case 4: return 1268;
    case 5: return 1280;
    case 6:
    case 7: return 1284;
    case 11:
    case 12:
    case 13:
    case 14: return 1572;
    case 17: return 2324;
    default: return 2320;
  }
}

inline std::size_t project_name_at(unsigned version) {
  switch (version) {
    case 2: return 0x418;
    case 3: return 0x428;
    case 4: return 0x4D0;
    case 5: return 0x4DC;
    case 6:
    case 7: return 0x4E0;
    case 11:
    case 12:
    case 13:
    case 14: return 0x600;
    case 17: return 0x810;
    default: return 0x80C;
  }
}

// The byte ranges of the settings groups that a version has: [begin, end) file offsets, as the specs list them.
struct GroupRange {
  std::size_t begin, end;
};
inline std::vector<GroupRange> group_ranges(unsigned v) {
  std::vector<GroupRange> r = {{0x11A, 0x11B}, {0x11C, 0x11F}, {0x120, 0x122}, {0x124, 0x12B}, {0x134, 0x13A},
                               {0x13C, 0x140}, {0x142, 0x1BE}, {0x2F5, 0x309}};
  if (v >= 3) r.push_back({0x418, 0x428});
  if (v <= 7) r.push_back({0x309, 0x319});
  if (v >= 11) r.push_back({0x4DE, 0x600});
  if (v >= 13) r.push_back({0x309, 0x319});
  if (v == 16 || v == 17) {
    r.push_back({0x600, 0x603});
    r.push_back({0x643, v == 17 ? std::size_t{0x80D} : std::size_t{0x80C}});  // v17: the new byte at 0x80C rides with the block
  }
  return r;
}

inline Bytes project_bytes(const ProjectSpec& s) {
  const unsigned v = s.version;
  Bytes b(project_length(v), 0);
  // the session state at what Export writes, the settings groups at their defaults
  b[0x114] = s.actual_pattern != 0 ? s.actual_pattern : (s.playlist.empty() ? std::uint8_t{1} : s.playlist[0]);
  b[0x116] = 11; b[0x117] = 24; b[0x118] = 8;  // pad board
  b[0x119] = 50;                               // old volume
  b[0x12B] = 1;                                // edit step
  b[0x11A] = 50; p16(b, 0x11C, 500); b[0x11E] = 0xC3;                 // delay
  p16(b, 0x120, 100); pf(b, 0x124, 0.512f); p16(b, 0x128, 30200); b[0x12A] = 16;  // limiter
  pf(b, 0x13C, 87.5f);                                                // radio
  for (unsigned i = 0; i < 8; ++i) p16(b, 0x142 + 2 * i, 1);          // performance
  for (unsigned i = 0; i < 12; ++i) b[0x152 + i] = static_cast<std::uint8_t>(i + 1);
  if (v >= 3) { pf(b, 0x418, 0.5f); pf(b, 0x41C, 0.5f); pf(b, 0x420, 0.5f); pf(b, 0x424, 0.68f); }
  if (v <= 7) for (unsigned i = 0; i < 16; ++i) b[0x309 + i] = 127;   // MIDI velocity of the old versions
  if (v >= 11) {
    if (v >= 13) b[0x309] = 86; else b[0x4F7] = 86;                   // the value 1.4.0.2 moved
    b[0x4F8] = 3; b[0x4F9] = 1; b[0x4FC] = 1;
    static const std::uint8_t rec[7] = {0x7F, 0x01, 0x05, 0x07, 0x0B, 0x40, 0x80};
    for (unsigned k = 0; k < 16; ++k) {
      b[0x4FD + 16 * k] = static_cast<std::uint8_t>(k);
      std::memcpy(&b[0x4FD + 16 * k + 1], rec, 7);
    }
    p16(b, 0x5FE, 20000);
  }
  if (s.fill != ProjectSpec::Fill::Defaults) {  // junk in what Export rebuilds or defaults
    std::uint32_t x = s.junk | 1u;
    auto junk = [&x]() {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      return static_cast<std::uint8_t>(x >> 9);
    };
    auto fill = [&](std::size_t from, std::size_t to) { for (std::size_t i = from; i < to; ++i) b[i] = junk(); };
    fill(0x10F, 0x114 + 1);  // song position, last used values, current pattern
    fill(0x115, 0x11A);      // pad board, old volume
    b[0x11B] = junk(); b[0x11F] = junk(); b[0x122] = junk(); b[0x123] = junk(); b[0x12B] = junk();
    b[0x13A] = junk(); b[0x13B] = junk(); b[0x140] = junk(); b[0x141] = junk(); b[0x1BE] = junk(); b[0x1BF] = junk();
    fill(0x1C6, 0x2F5);      // the save lists
    if (v == 5) { b[0x4DA] = junk(); b[0x4DB] = junk(); }
    if (v == 6 || v == 7) { b[0x4DE] = junk(); b[0x4DF] = junk(); }
    if (v == 16) fill(0x82C, 0x90C);
    if (v == 17) { fill(0x80D, 0x810); fill(0x830, 0x910); }  // padding after the new byte; the NUL, the second string, its spare byte
    b[0x114] = static_cast<std::uint8_t>(1 + (b[0x114] % 200));
  }
  if (s.fill == ProjectSpec::Fill::Groups) {  // a distinct non-default value in every byte of every group
    for (const GroupRange& g : group_ranges(v)) {
      for (std::size_t i = g.begin; i < g.end; ++i) b[i] = static_cast<std::uint8_t>(i * 13 + 5);
    }
    if (v == 11 || v == 12) b[0x4F7] = 77;  // before 1.4.0.2 the value sat in the 1.3 block
  }
  if (v == 17 && (s.fill != ProjectSpec::Fill::Groups || s.own_80c)) b[0x80C] = s.byte_80c;
  static const std::uint8_t fw16[4] = {1, 9, 0, 255};
  static const std::uint8_t fw17[4] = {1, 9, 2, 255};
  const std::uint8_t* fw = v == 17 ? fw17 : fw16;
  header(b, 'M', 'T', 0, fw, v, static_cast<std::uint32_t>(b.size()));
  b[14] = b[15] = 0;
  if (s.fill != ProjectSpec::Fill::Defaults) { b[14] = 0xEB; b[15] = 0x1D; }
  // the modelled fields
  for (unsigned i = 0; i < 255; ++i) b[0x10 + i] = i < s.playlist.size() ? s.playlist[i] : std::uint8_t{0};
  if (s.playlist.empty() && (v == 16 || v == 17) && s.fill == ProjectSpec::Fill::Defaults) b[0x10] = 1;  // the default song: pattern 1 (the default table of docs/formats/og.md)
  pf(b, 0x1C0, s.tempo);
  p16(b, 0x1C4, s.pattern_length);
  for (unsigned i = 0; i < 8; ++i) b[0x12C + i] = s.mute[i];
  std::memcpy(&b[0x319], s.bitmask, 255);
  if (v >= 4) {
    for (unsigned i = 0; i < 8; ++i) ptext(b, 0x428 + 21 * i, s.names[i], 21);
  }
  if (v >= 5) {
    for (unsigned i = 0; i < 8; ++i) b[0x4D0 + i] = s.volume[i];
    b[0x4D8] = s.reverb_volume;
    b[0x4D9] = s.delay_volume;
  }
  if (v >= 6) {
    b[0x4DA] = s.reverb_mute;
    b[0x4DB] = s.delay_mute;
    b[0x4DC] = s.dry_mute;
    b[0x4DD] = s.dry_volume;
  }
  if (v == 16 || v == 17) {
    for (unsigned k = 0; k < 8; ++k) ptext(b, 0x603 + 8 * k, s.names[8 + k], 8);
  }
  ptext(b, project_name_at(v), s.name, 32);
  seal(b);
  return b;
}

// ---- instruments (PAR-005-004) -----------------------------------------------------------------------------

struct InstrumentSpec {
  unsigned version = 9;
  bool pti = true;                // 392 bytes + PCM; false: a whole .mti
  std::uint32_t frames = 1000;    // .pti: mono frames after the head
  std::string file_name = "Kick 808.wav";
  bool canonical = true;          // unmodelled bytes as Export writes them (zero); false: junk
};

// A 392-byte (388 for version <= 3) image with a distinct, in-range value in every field the model holds.
inline Bytes instrument_head(const InstrumentSpec& s) {
  const bool legacy = s.version <= 3;
  Bytes b(legacy ? 388 : 392, 0);
  static const std::uint8_t fw[4] = {1, 9, 0, 1};
  header(b, 'T', 'I', 1, fw, s.version, 372);
  if (!s.canonical) {
    std::uint32_t x = 0x9E3779B9u;
    for (std::size_t i = 14; i < 16; ++i) b[i] = static_cast<std::uint8_t>(x >> i);
    b[17] = 7; b[18] = 9; b[53] = 5; b[66] = 3; b[77] = 1; b[86] = 2; b[273] = 4;  // (279 is the play-thru flag now, OS-CEPLAYTHRU P1: it stays 0 here)
    p32(b, 56, 0x2000ABCDu);   // a live RAM pointer
    p32(b, 68, 77);            // wavetable window number
    b[72] = 'a'; b[73] = 'b';
  }
  b[16] = 1;                       // the active flag
  b[20] = 1;                       // wavetable sample
  ptext(b, 21, s.file_name, 32);
  p32(b, 60, s.pti ? s.frames : 4321);   // the sample length in frames
  p16(b, 64, 1024);                // wavetable window size
  b[76] = 5;                       // play mode
  p16(b, 78, 111); p16(b, 80, 222); p16(b, 82, 333); p16(b, 84, 444);
  p32(b, 88, 9);                   // current wavetable window
  for (unsigned i = 0; i < 6; ++i) {  // envelopes
    const std::size_t e = 92 + 20 * i;
    pf(b, e, 0.1f * static_cast<float>(i + 1));
    p16(b, e + 4, 10 + i); p16(b, e + 6, 200 + i); p16(b, e + 8, 30 + i); p16(b, e + 10, 400 + i);
    pf(b, e + 12, 0.05f * static_cast<float>(i + 2));
    p16(b, e + 16, 500 + i);
    b[e + 18] = i & 1; b[e + 19] = (i + 1) & 1;
  }
  for (unsigned i = 0; i < 6; ++i) {  // LFOs
    const std::size_t l = 212 + 8 * i;
    b[l] = static_cast<std::uint8_t>(i % 5); b[l + 1] = static_cast<std::uint8_t>(3 + i);
    pf(b, l + 4, 0.25f + 0.1f * static_cast<float>(i));
  }
  pf(b, 260, 0.37f);               // cutoff
  pf(b, 264, 0.62f);               // resonance
  b[268] = 2; b[269] = 1;          // band-pass, enabled
  b[270] = static_cast<std::uint8_t>(-5); b[271] = 17;  // tune, fine tune
  b[272] = 91;                     // volume
  p16(b, 274, 123);                // glide
  p16(b, 276, 31);                 // panning
  b[278] = 44;                     // delay send
  for (unsigned i = 0; i < 48; ++i) p16(b, 280 + 2 * i, 1000 + 1000 * i);
  b[376] = 37; b[377] = 12;        // slice count, selected slice
  p16(b, 378, 4410); p16(b, 380, 7777);
  b[382] = 2; b[383] = 1;          // grain shape, type
  if (!legacy) {
    b[384] = 23;                   // reverb send
    b[385] = s.version >= 6 ? 65 : (s.canonical ? 0 : 99);   // overdrive
    b[386] = s.version >= 8 ? 9 : (s.canonical ? 0 : 200);   // bit depth
    b[387] = 0;
  }
  if (legacy) {
    p32(b, 384, formats::crc32(b.data(), 384));
  } else {
    p32(b, 388, formats::crc32(b.data(), 388));
  }
  return b;
}

inline Bytes instrument_bytes(const InstrumentSpec& s) {
  Bytes b = instrument_head(s);
  if (s.pti) {
    for (std::uint32_t i = 0; i < 2 * s.frames; ++i) b.push_back(static_cast<std::uint8_t>(i * 7 + 3));
  }
  return b;
}

// ---- PAMD (PAR-008) ----------------------------------------------------------------------------------------

// Record i holds names[i] (empty when past the end).
inline Bytes pamd_bytes(const std::vector<std::string>& names, unsigned records = 256, unsigned version = 1) {
  Bytes b(16 + 50u * records, 0);
  std::memcpy(b.data(), "PAMD", 4);
  p16(b, 4, version);
  p32(b, 8, static_cast<std::uint32_t>(b.size()));
  for (unsigned i = 0; i < records && i < names.size(); ++i) ptext(b, 16 + 50u * i, names[i], 30);
  return b;
}

// ---- running a reader or writer over memory -----------------------------------------------------------------

template <class R>
formats::Progress run_reader(R& r, const Bytes& in, model::ModelArena& a, formats::ReadParams params = {},
                             std::size_t budget = 4096, std::size_t max_per_read = static_cast<std::size_t>(-1),
                             std::uint32_t block_every = 0) {
  MemorySource src(in);
  src.max_per_read = max_per_read;
  src.block_every = block_every;
  r.begin(src, a, params);
  for (unsigned guard = 0; guard < 50'000'000; ++guard) {
    const formats::Progress p = r.step(budget);
    if (p != formats::Progress::Running) return p;
  }
  return formats::Progress::Running;
}

template <class W>
formats::Progress run_writer(W& w, const model::Project& p, Bytes& out, formats::WriteParams params = {},
                             std::size_t budget = 4096) {
  VectorSink sink;
  w.begin(p, sink, params);
  formats::Progress r = formats::Progress::Running;
  for (unsigned guard = 0; guard < 50'000'000 && r == formats::Progress::Running; ++guard) r = w.step(budget);
  out = sink.bytes;
  return r;
}

}  // namespace o3os::test::og
