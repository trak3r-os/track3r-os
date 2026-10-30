// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Internal to formats/native: big-endian field cursors and the chunk size formulas shared by the reader and
// the writer. The byte layout is documented in docs/formats/o3-project.md; tools/o3project.py mirrors it.

#include <o3os/formats/native.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::formats::native::codec {

// ---- sizes ---------------------------------------------------------------------------------------------

inline constexpr std::size_t kChunkHeader = 8;
inline constexpr std::size_t kFormHeader = 12;  // "FORM", length, "O3PJ"

inline constexpr std::size_t kPhdrFixed = 19;  // everything but the name's bytes (name length byte included)
inline constexpr std::size_t kPhdrMax = kPhdrFixed + model::kProjectNameMax;
inline constexpr std::size_t kTrakSlotFixed = 6;
inline constexpr std::size_t kTrakFixed = 3;
inline constexpr std::size_t kTrakMax = kTrakFixed + model::kMaxTracks * (kTrakSlotFixed + model::kTrackNameMax);
inline constexpr std::size_t kModrModuleFixed = 3;
inline constexpr std::size_t kModrFixed = 3;
inline constexpr std::size_t kModrMax = kModrFixed + model::kMaxModules * (kModrModuleFixed + model::kModuleNameMax);
inline constexpr std::size_t kSongFixed = 3;
inline constexpr std::size_t kSongMax = kSongFixed + model::kMaxSongPositions;
inline constexpr std::size_t kPattHead = 5;  // version, id, track count, name length
inline constexpr std::size_t kPattTrackFixed = 2;  // track length, stored step count
inline constexpr std::size_t kPattMax =
    kPattHead + model::kPatternNameMax +
    model::kMaxTracks * (kPattTrackFixed + model::kMaxPatternLength * sizeof(model::Step));
inline constexpr std::size_t kSamplerBytes = 296;
inline constexpr std::size_t kInstFixed = 6 + 1 + 1 + kSamplerBytes + 2;  // head, two length bytes, sampler, blob length
inline constexpr std::size_t kInstRate = 4;  // version 1.1 appends the sample rate (u32) after the blob
inline constexpr std::size_t kInstEngine = 1;  // version 1.2 appends the playback engine (u8) after the rate
inline constexpr std::size_t kInstMidi = 12;  // version 1.3 appends velocity, program, out, pb range, offset, CC A..F, VEL> (u8 each) after the engine (OS-INSTUI S8b + S9)
inline constexpr std::size_t kInstStereo = 1;  // version 1.5 appends the stereo flag (u8) after the MIDI bytes (OS-STEREO step 2)
inline constexpr std::size_t kInstMax =
    kInstFixed + model::kInstrumentNameMax + model::kSamplePathMax + model::kModuleParamBytes;  // version 1.0
inline constexpr std::size_t kCrcPayload = 6;

static_assert(kPhdrMax <= kScratchBytes && kTrakMax <= kScratchBytes && kModrMax <= kScratchBytes &&
                  kSongMax <= kScratchBytes && kInstMax <= kScratchBytes,
              "every chunk read whole must fit the scratch buffer");
static_assert(kChunkHeader + kInstMax + kInstRate + kInstEngine + kInstMidi + kInstStereo + 1 <= kScratchBytes, "a whole INST chunk is one writer piece");
static_assert(kInstMax + kInstRate + kInstEngine + kInstMidi + kInstStereo <= kScratchBytes, "an INST chunk of version 1.5 is read whole");
static_assert(kChunkHeader + kPattHead + model::kPatternNameMax <= kScratchBytes, "PATT head piece");
static_assert(kChunkHeader + kTrakMax + 1 <= kScratchBytes && kChunkHeader + kModrMax + 1 <= kScratchBytes &&
                  kChunkHeader + kSongMax + 1 <= kScratchBytes && kChunkHeader + kPhdrMax + 1 <= kScratchBytes,
              "small chunks are one writer piece");
static_assert(kPattTrackFixed + model::kMaxPatternLength * sizeof(model::Step) + 1 <= kScratchBytes,
              "one PATT track row is one piece");

constexpr std::uint32_t chunk_total(std::size_t payload) noexcept {
  return static_cast<std::uint32_t>(kChunkHeader + payload + (payload & 1u));
}

// Number of steps stored for a track: up to the last one that is not empty (writers emit the minimum).
inline std::uint32_t stored_steps(const model::Pattern& p, std::uint32_t track) noexcept {
  std::uint32_t n = model::kMaxPatternLength;
  while (n > 0 && model::is_empty(p.steps[track][n - 1])) {
    --n;
  }
  return n;
}

inline std::size_t text_len(const char* field, std::size_t cap) noexcept {
  const std::size_t n = model::field_length(field, cap);
  return n > cap ? cap : n;  // a field in a valid project is always terminated
}

// ---- big-endian reading and writing -----------------------------------------------------------------

inline std::uint16_t get16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(p[0]) << 8) | p[1]);
}

inline std::uint32_t get32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// Reads fields out of a buffer; running past the end never touches memory beyond it, it returns zeros and
// latches bad().
class Cursor {
 public:
  Cursor(const std::uint8_t* p, std::size_t n) noexcept : p_(p), left_(n) {}

  std::uint8_t u8() noexcept {
    const std::uint8_t* b = take(1);
    return b != nullptr ? b[0] : std::uint8_t{0};
  }
  std::uint16_t u16() noexcept {
    const std::uint8_t* b = take(2);
    return b != nullptr ? get16(b) : std::uint16_t{0};
  }
  std::uint32_t u32() noexcept {
    const std::uint8_t* b = take(4);
    return b != nullptr ? get32(b) : 0u;
  }
  float f32() noexcept {
    const std::uint32_t bits = u32();
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
  }
  // `n` bytes in place, nullptr (and bad()) when fewer remain
  const std::uint8_t* take(std::size_t n) noexcept {
    if (bad_ || n > left_) {
      bad_ = true;
      return nullptr;
    }
    const std::uint8_t* r = p_;
    p_ += n;
    left_ -= n;
    return r;
  }
  // A text of at most `max` characters preceded by its length byte; false when too long or short of bytes or
  // when it contains a NUL (names are C strings in the model).
  bool text(std::size_t max, const std::uint8_t*& out, std::size_t& len) noexcept {
    len = u8();
    if (len > max) {
      bad_ = true;
      return false;
    }
    out = take(len);
    if (out == nullptr) {
      return false;
    }
    for (std::size_t i = 0; i < len; ++i) {
      if (out[i] == 0) {
        bad_ = true;
        return false;
      }
    }
    return true;
  }
  bool bad() const noexcept { return bad_; }
  std::size_t left() const noexcept { return left_; }

 private:
  const std::uint8_t* p_;
  std::size_t left_;
  bool bad_ = false;
};

// Writes fields into a buffer; running past the end latches overflow() and writes nothing more.
class Enc {
 public:
  Enc(std::uint8_t* p, std::size_t cap) noexcept : p_(p), cap_(cap) {}

  void u8(std::uint8_t v) noexcept {
    if (std::uint8_t* b = room(1)) {
      b[0] = v;
    }
  }
  void u16(std::uint16_t v) noexcept {
    if (std::uint8_t* b = room(2)) {
      b[0] = static_cast<std::uint8_t>(v >> 8);
      b[1] = static_cast<std::uint8_t>(v);
    }
  }
  void u32(std::uint32_t v) noexcept {
    if (std::uint8_t* b = room(4)) {
      b[0] = static_cast<std::uint8_t>(v >> 24);
      b[1] = static_cast<std::uint8_t>(v >> 16);
      b[2] = static_cast<std::uint8_t>(v >> 8);
      b[3] = static_cast<std::uint8_t>(v);
    }
  }
  void f32(float v) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    u32(bits);
  }
  void bytes(const void* src, std::size_t n) noexcept {
    if (std::uint8_t* b = room(n)) {
      if (n != 0) {
        std::memcpy(b, src, n);
      }
    }
  }
  // length byte + characters (n <= 255)
  void text(const char* s, std::size_t n) noexcept {
    u8(static_cast<std::uint8_t>(n));
    bytes(s, n);
  }
  std::size_t size() const noexcept { return len_; }
  bool overflow() const noexcept { return over_; }

 private:
  std::uint8_t* room(std::size_t n) noexcept {
    if (over_ || n > cap_ - len_) {
      over_ = true;
      return nullptr;
    }
    std::uint8_t* r = p_ + len_;
    len_ += n;
    return r;
  }

  std::uint8_t* p_;
  std::size_t cap_;
  std::size_t len_ = 0;
  bool over_ = false;
};

// ---- the sampler block of an INST chunk (kSamplerBytes bytes) -------------------------------------------

inline void encode_sampler(Enc& e, const model::SamplerParams& s) noexcept {
  e.u8(s.sample_type);
  e.u8(s.play_mode);
  e.u8(s.filter_type);
  e.u8(s.filter_enable);
  e.u8(static_cast<std::uint8_t>(s.tune));
  e.u8(static_cast<std::uint8_t>(s.fine_tune));
  e.u8(s.volume);
  e.u8(s.panning);
  e.u8(s.delay_send);
  e.u8(s.reverb_send);
  e.u8(s.overdrive);
  e.u8(s.bit_depth);
  e.u8(s.slice_count);
  e.u8(s.selected_slice);
  e.u8(s.grain_shape);
  e.u8(s.grain_type);
  e.u16(s.glide);
  e.u16(s.wavetable_window_size);
  e.u16(s.grain_length);
  e.u16(s.grain_position);
  e.u16(s.start);
  e.u16(s.loop_start);
  e.u16(s.loop_end);
  e.u16(s.end);
  e.u32(s.wavetable_window);
  e.f32(s.cutoff);
  e.f32(s.resonance);
  for (const model::Envelope& v : s.envelope) {
    e.f32(v.amount);
    e.u16(v.delay);
    e.u16(v.attack);
    e.u16(v.hold);
    e.u16(v.decay);
    e.f32(v.sustain);
    e.u16(v.release);
    e.u8(v.loop);
    e.u8(v.enable);
  }
  for (const model::Lfo& v : s.lfo) {
    e.u8(v.shape);
    e.u8(v.speed);
    e.f32(v.amount);
  }
  for (std::uint16_t v : s.slices) {
    e.u16(v);
  }
}

inline void decode_sampler(Cursor& c, model::SamplerParams& s) noexcept {
  s.sample_type = c.u8();
  s.play_mode = c.u8();
  s.filter_type = c.u8();
  s.filter_enable = c.u8();
  s.tune = static_cast<std::int8_t>(c.u8());
  s.fine_tune = static_cast<std::int8_t>(c.u8());
  s.volume = c.u8();
  s.panning = c.u8();
  s.delay_send = c.u8();
  s.reverb_send = c.u8();
  s.overdrive = c.u8();
  s.bit_depth = c.u8();
  s.slice_count = c.u8();
  s.selected_slice = c.u8();
  s.grain_shape = c.u8();
  s.grain_type = c.u8();
  s.glide = c.u16();
  s.wavetable_window_size = c.u16();
  s.grain_length = c.u16();
  s.grain_position = c.u16();
  s.start = c.u16();
  s.loop_start = c.u16();
  s.loop_end = c.u16();
  s.end = c.u16();
  s.wavetable_window = c.u32();
  s.cutoff = c.f32();
  s.resonance = c.f32();
  for (model::Envelope& v : s.envelope) {
    v.amount = c.f32();
    v.delay = c.u16();
    v.attack = c.u16();
    v.hold = c.u16();
    v.decay = c.u16();
    v.sustain = c.f32();
    v.release = c.u16();
    v.loop = c.u8();
    v.enable = c.u8();
  }
  for (model::Lfo& v : s.lfo) {
    v.shape = c.u8();
    v.speed = c.u8();
    v.amount = c.f32();
  }
  for (std::uint16_t& v : s.slices) {
    v = c.u16();
  }
}

}  // namespace o3os::formats::native::codec
