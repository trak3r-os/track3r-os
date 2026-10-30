// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/formats/wav.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "support/memory_io.h"
#include "support/need.h"
#include "support/rng.h"

// The WAV importer (OS-002c): every supported variant, resumability under hostile sources, and hostile files (PAR-011 style).
// No heap: the file and the destination are static, so the same file also runs as the QEMU suite `qemu_wav`.

using namespace o3os;
namespace wav = o3os::formats::wav;

namespace {

constexpr std::size_t kMaxFile = 12000;
constexpr std::size_t kDst = 4096;     // samples the destination holds
constexpr std::size_t kGuard = 16;     // canary samples on both sides of it

// A WAV file under construction.
struct File {
  std::uint8_t b[kMaxFile];
  std::size_t n = 0;

  void u8(std::uint32_t v) { if (n < kMaxFile) b[n++] = static_cast<std::uint8_t>(v); else overflow = true; }
  bool overflow = false;
  void u16(std::uint32_t v) { u8(v); u8(v >> 8); }
  void u32(std::uint32_t v) { u16(v & 0xFFFFu); u16(v >> 16); }
  void tag(const char* t) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(t[i])); }
  void zeros(std::size_t k) { for (std::size_t i = 0; i < k; ++i) u8(0); }

  File& riff(std::uint32_t size = 0xFFFFFFFEu) {  // 0xFFFFFFFE: patched by end()
    n = 0;
    tag("RIFF");
    u32(size);
    tag("WAVE");
    return *this;
  }
  // A plain fmt chunk.
  File& fmt(unsigned tag_value, unsigned channels, std::uint32_t rate, unsigned bits) {
    tag("fmt ");
    u32(16);
    u16(tag_value);
    u16(channels);
    u32(rate);
    u32(rate * channels * (bits / 8));
    u16(channels * (bits / 8));
    u16(bits);
    return *this;
  }
  // WAVE_FORMAT_EXTENSIBLE around `sub` (1 PCM, 3 float).
  File& fmt_ext(unsigned sub, unsigned channels, std::uint32_t rate, unsigned bits, bool good_guid = true) {
    tag("fmt ");
    u32(40);
    u16(0xFFFE);
    u16(channels);
    u32(rate);
    u32(rate * channels * (bits / 8));
    u16(channels * (bits / 8));
    u16(bits);
    u16(22);
    u16(bits);
    u32(channels == 1 ? 4u : 3u);
    u16(sub);
    static const std::uint8_t tail[14] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    for (int i = 0; i < 14; ++i) u8(good_guid ? tail[i] : static_cast<std::uint8_t>(tail[i] ^ (i == 5 ? 1 : 0)));
    return *this;
  }
  File& data_header(std::uint32_t size) { tag("data"); u32(size); return *this; }
  // Pads to an even size as RIFF wants, after a chunk body of `body` bytes.
  File& pad(std::size_t body) { if (body & 1u) u8(0); return *this; }
  File& end() {  // patch the RIFF size
    const std::uint32_t size = static_cast<std::uint32_t>(n - 8);
    b[4] = static_cast<std::uint8_t>(size);
    b[5] = static_cast<std::uint8_t>(size >> 8);
    b[6] = static_cast<std::uint8_t>(size >> 16);
    b[7] = static_cast<std::uint8_t>(size >> 24);
    return *this;
  }
  File& smpl(std::uint32_t unity, std::uint32_t loops, std::uint32_t type, std::uint32_t start, std::uint32_t end_incl,
             std::uint32_t claimed_loops = 0xFFFFFFFFu) {
    tag("smpl");
    u32(36 + 24 * loops);
    u32(0); u32(0); u32(22675); u32(unity); u32(0); u32(0); u32(0);
    u32(claimed_loops == 0xFFFFFFFFu ? loops : claimed_loops);
    u32(0);
    for (std::uint32_t i = 0; i < loops; ++i) {
      u32(i); u32(type); u32(start + i); u32(end_incl); u32(0); u32(0);
    }
    return *this;
  }
  File& cue(const std::uint32_t* offsets, std::uint32_t count, std::uint32_t claimed = 0xFFFFFFFFu) {
    tag("cue ");
    u32(4 + 24 * count);
    u32(claimed == 0xFFFFFFFFu ? count : claimed);
    for (std::uint32_t i = 0; i < count; ++i) {
      u32(i + 1); u32(offsets[i]); tag("data"); u32(0); u32(0); u32(offsets[i]);
    }
    return *this;
  }
  File& list_chunk() {  // a LIST/INFO chunk of odd size: skipped, with its pad byte
    tag("LIST");
    u32(13);
    for (int i = 0; i < 13; ++i) u8(static_cast<std::uint8_t>('a' + i));
    u8(0);
    return *this;
  }
};

File g_file;
// The tests of the interleaved layout keep stereo stereo; the downmix tests turn this off.
bool g_keep_stereo = true;
std::uint8_t g_channel = 0;  // OS-LOADCH: Importer::begin `channel` (0 the mean, 1 left, 2 right)
std::int16_t g_buf[kGuard + kDst + kGuard];
std::int16_t* const g_dst = g_buf + kGuard;

void clear_dst() {
  for (std::size_t i = 0; i < sizeof g_buf / sizeof g_buf[0]; ++i) g_buf[i] = 0x5A5A;
}
bool guards_intact() {
  for (std::size_t i = 0; i < kGuard; ++i) {
    if (g_buf[i] != 0x5A5A || g_buf[kGuard + kDst + i] != 0x5A5A) return false;
  }
  return true;
}
bool untouched(std::size_t from, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) if (g_dst[from + i] != 0x5A5A) return false;
  return true;
}

// One whole import of g_file through a plain source, steps of `budget` bytes. Returns the final progress.
struct Run {
  wav::Importer imp;
  wav::Progress result = wav::Progress::Running;
  unsigned steps = 0;
};

wav::Progress run(Run& r, test::MemorySource& src, std::size_t capacity, std::size_t budget = 4096,
                  unsigned max_steps = 200000) {
  clear_dst();
  r.imp.begin(src, g_dst, capacity, g_keep_stereo, false, g_channel);
  r.steps = 0;
  r.result = wav::Progress::Running;
  while (r.result == wav::Progress::Running && r.steps < max_steps) {
    r.result = r.imp.step(budget);
    ++r.steps;
  }
  return r.result;
}

wav::Progress run_file(Run& r, std::size_t capacity = kDst, std::size_t budget = 4096) {
  test::MemorySource src(g_file.b, g_file.n);
  return run(r, src, capacity, budget);
}

void ramp16(File& f, std::uint32_t samples, std::int32_t first = -300) {
  for (std::uint32_t i = 0; i < samples; ++i) f.u16(static_cast<std::uint32_t>((first + static_cast<std::int32_t>(i) * 7) & 0xFFFF));
}
std::int16_t ramp_value(std::uint32_t i, std::int32_t first = -300) {
  return static_cast<std::int16_t>(static_cast<std::uint16_t>((first + static_cast<std::int32_t>(i) * 7) & 0xFFFF));
}

// A 16-bit mono file of `frames` ramp samples.
void make_pcm16(std::uint32_t frames, std::uint32_t rate = 44100) {
  g_file.riff().fmt(1, 1, rate, 16).data_header(frames * 2);
  ramp16(g_file, frames);
  g_file.end();
}

}  // namespace

TEST_CASE("wav: 16-bit mono is copied exactly, and the rate is kept") {
  make_pcm16(1000, 22050);
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(r.imp.report().error == wav::Error::None);
  const wav::Info& i = r.imp.info();
  CHECK(i.frames == 1000u);
  CHECK(i.channels == 1);
  CHECK(i.bits == 16);
  CHECK(i.sample_rate == 22050u);
  CHECK(i.format == wav::SampleFormat::Pcm);
  CHECK_FALSE(i.dithered);
  CHECK_FALSE(i.has_loop);
  CHECK(i.slice_count == 0);
  CHECK(i.root_note == 60);
  CHECK(r.imp.permille() == 1000u);
  bool same = true;
  for (std::uint32_t k = 0; k < 1000; ++k) same = same && g_dst[k] == ramp_value(k);
  CHECK(same);
  CHECK(untouched(1000, kDst - 1000));
  CHECK(guards_intact());
  CHECK(r.imp.report().bytes == g_file.n);
}

TEST_CASE("wav: the same file gives the same samples through any step budget and any source behaviour") {
  make_pcm16(700);
  Run ref;
  CHECK(run_file(ref) == wav::Progress::Done);
  static std::int16_t want[700];
  std::memcpy(want, g_dst, sizeof want);

  const std::size_t budgets[] = {1, 2, 3, 7, 64, 511, 512, 513, 4096, 100000};
  for (const std::size_t budget : budgets) {
    Run r;
    CHECK(run_file(r, kDst, budget) == wav::Progress::Done);
    CHECK_MESSAGE(std::memcmp(g_dst, want, sizeof want) == 0, "budget " << budget);
    CHECK(r.imp.info().frames == 700u);
  }
  // A source that gives one byte at a time, blocks every third call, and a stream cut into odd pieces.
  const std::size_t pieces[] = {1, 2, 3, 5, 11, 100};
  for (const std::size_t piece : pieces) {
    test::MemorySource src(g_file.b, g_file.n);
    src.max_per_read = piece;
    src.block_every = 3;
    Run r;
    CHECK(run(r, src, kDst, 97) == wav::Progress::Done);
    CHECK_MESSAGE(std::memcmp(g_dst, want, sizeof want) == 0, "piece " << piece);
    CHECK(r.steps > 1u);  // it really did take several passes
  }
  // permille climbs and never goes back; it reaches 1000 only when Done.
  test::MemorySource src(g_file.b, g_file.n);
  wav::Importer imp;
  clear_dst();
  imp.begin(src, g_dst, kDst);
  unsigned last = 0;
  unsigned n = 0;
  while (imp.step(100) == wav::Progress::Running && ++n < 100000) {
    CHECK(imp.permille() >= last);
    CHECK(imp.permille() <= 999u);
    last = imp.permille();
  }
  CHECK(imp.progress() == wav::Progress::Done);
  CHECK(imp.permille() == 1000u);
  CHECK(last > 500u);
}

TEST_CASE("wav: 8-bit is unsigned and centred on 128") {
  g_file.riff().fmt(1, 1, 8000, 8).data_header(6);
  const std::uint8_t v[6] = {0, 1, 127, 128, 129, 255};
  for (const std::uint8_t x : v) g_file.u8(x);
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(r.imp.info().frames == 6u);
  CHECK(r.imp.info().bits == 8);
  CHECK(r.imp.info().sample_rate == 8000u);
  const std::int16_t want[6] = {-32768, -32512, -256, 0, 256, 32512};
  CHECK(std::memcmp(g_dst, want, sizeof want) == 0);
  CHECK_FALSE(r.imp.info().dithered);
}

TEST_CASE("wav: 24-bit is dithered where it loses something and exact where it does not") {
  // A 16-bit signal stored in 24 bits: the low byte is zero, nothing is lost, nothing is dithered.
  g_file.riff().fmt(1, 1, 44100, 24).data_header(300 * 3);
  for (std::uint32_t i = 0; i < 300; ++i) {
    const std::uint32_t s = static_cast<std::uint32_t>(ramp_value(i)) & 0xFFFFu;
    g_file.u8(0);
    g_file.u16(s);
  }
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  bool exact = true;
  for (std::uint32_t i = 0; i < 300; ++i) exact = exact && g_dst[i] == ramp_value(i);
  CHECK(exact);
  CHECK_FALSE(r.imp.info().dithered);
  CHECK(r.imp.info().bits == 24);

  // A full 24-bit signal: within 1.5 LSB of the exact value, no bias, the same every time, and it is flagged.
  constexpr std::uint32_t kN = 2000;
  test::Rng rng(77);
  static std::int32_t exact24[kN];
  g_file.riff().fmt(1, 1, 44100, 24).data_header(kN * 3);
  for (std::uint32_t i = 0; i < kN; ++i) {
    std::int32_t s = static_cast<std::int32_t>(rng.next() & 0xFFFFFFu) - 0x800000;
    if (i % 16 == 0) s = 8388607;  // the largest: must clip, not wrap
    if (i % 16 == 1) s = -8388608;
    exact24[i] = s;
    const std::uint32_t u = static_cast<std::uint32_t>(s) & 0xFFFFFFu;
    g_file.u8(u);
    g_file.u8(u >> 8);
    g_file.u8(u >> 16);
  }
  g_file.end();
  Run r2;
  CHECK(run_file(r2) == wav::Progress::Done);
  CHECK(r2.imp.info().dithered);
  static std::int16_t first[kN];
  std::memcpy(first, g_dst, sizeof first);
  std::int64_t bias = 0;
  bool close = true;
  for (std::uint32_t i = 0; i < kN; ++i) {
    const std::int32_t err = first[i] * 256 - exact24[i];
    bias += err;
    close = close && err > -384 && err < 384;
  }
  CHECK(close);
  CHECK(bias / static_cast<std::int64_t>(kN) > -16);
  CHECK(bias / static_cast<std::int64_t>(kN) < 16);
  CHECK(first[0] == 32767);   // the 24-bit maximum
  CHECK(first[1] == -32768);
  // Deterministic: a second import, even in other steps, is bit-identical.
  Run r3;
  CHECK(run_file(r3, kDst, 5) == wav::Progress::Done);
  CHECK(std::memcmp(first, g_dst, sizeof first) == 0);
}

TEST_CASE("wav: float32 is exact for 16-bit values, clips, and is NaN-safe") {
  g_file.riff().fmt(3, 1, 48000, 32).data_header(12 * 4);
  const float f[12] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 1.0f / 32768.0f, 32767.0f / 32768.0f, -32768.0f / 32768.0f,
                       0.0f, 0.0f};
  std::uint32_t bits[12];
  std::memcpy(bits, f, sizeof f);
  bits[10] = 0x7FC00000u;  // NaN
  bits[11] = 0x7F800000u;  // +infinity
  for (const std::uint32_t b : bits) g_file.u32(b);
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  const std::int16_t want[12] = {0, 16384, -16384, 32767, -32768, 32767, -32768, 1, 32767, -32768, 0, 32767};
  CHECK(std::memcmp(g_dst, want, sizeof want) == 0);
  CHECK_FALSE(r.imp.info().dithered);  // every one of them is exactly representable or clipped
  CHECK(r.imp.info().format == wav::SampleFormat::Float);
  CHECK(r.imp.info().bits == 32);

  // -infinity, and a value between two 16-bit steps: dithered, within one step either side.
  g_file.riff().fmt(3, 1, 48000, 32).data_header(1000 * 4);
  for (int i = 0; i < 1000; ++i) {
    float x = 0.3f;  // 9830.4 in 16-bit steps
    std::uint32_t b;
    if (i == 0) b = 0xFF800000u;
    else std::memcpy(&b, &x, 4);
    g_file.u32(b);
  }
  g_file.end();
  Run r2;
  CHECK(run_file(r2) == wav::Progress::Done);
  CHECK(r2.imp.info().dithered);
  CHECK(g_dst[0] == -32768);
  unsigned lo = 0, mid = 0, hi = 0;
  for (int i = 1; i < 1000; ++i) {
    CHECK(g_dst[i] >= 9829);
    CHECK(g_dst[i] <= 9831);
    lo += g_dst[i] == 9829;
    mid += g_dst[i] == 9830;
    hi += g_dst[i] == 9831;
  }
  CHECK(mid > 400u);
  CHECK(lo > 0u);
  CHECK(hi > 0u);  // the dither spreads the rounding error: it is not always the same neighbour
}

TEST_CASE("wav: stereo stays interleaved, with the right frame count") {
  g_file.riff().fmt(1, 2, 96000, 16).data_header(100 * 4);
  for (std::uint32_t i = 0; i < 100; ++i) {
    g_file.u16(i);
    g_file.u16(0x8000u + i);
  }
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(r.imp.info().frames == 100u);
  CHECK(r.imp.info().channels == 2);
  CHECK(r.imp.info().sample_rate == 96000u);
  bool ok = true;
  for (std::uint32_t i = 0; i < 100; ++i) {
    ok = ok && g_dst[2 * i] == static_cast<std::int16_t>(i) && g_dst[2 * i + 1] == static_cast<std::int16_t>(0x8000u + i);
  }
  CHECK(ok);
  CHECK(untouched(200, kDst - 200));

  // Stereo needs room for both channels: 100 frames do not fit a buffer of 199 samples, and fit exactly 200.
  Run r2;
  CHECK(run_file(r2, 199) == wav::Progress::Failed);
  CHECK(r2.imp.report().error == wav::Error::LimitExceeded);
  CHECK(untouched(0, kDst));  // nothing was written
  Run r3;
  CHECK(run_file(r3, 200) == wav::Progress::Done);
  CHECK(guards_intact());
}

TEST_CASE("wav: channel 1 / 2 keeps the left / right of a stereo file at 8, 16, 24 bits and float, in the room of the mix, however the steps cut the frames (OS-LOADCH)") {
  g_keep_stereo = false;
  constexpr std::uint32_t kFrames = 200;
  const auto l16 = [](std::uint32_t i) { return ramp_value(i, -900); };
  const auto r16 = [](std::uint32_t i) { return static_cast<std::int16_t>(-ramp_value(i, 40) / 2 + 3); };
  for (unsigned depth : {8u, 16u, 24u, 32u}) {
    if (depth == 8u) {
      g_file.riff().fmt(1, 2, 44100, 8).data_header(kFrames * 2);
      for (std::uint32_t i = 0; i < kFrames; ++i) {
        g_file.u8(static_cast<std::uint32_t>((l16(i) >> 8) + 128));
        g_file.u8(static_cast<std::uint32_t>((r16(i) >> 8) + 128));
      }
    } else if (depth == 16u) {
      g_file.riff().fmt(1, 2, 44100, 16).data_header(kFrames * 4);
      for (std::uint32_t i = 0; i < kFrames; ++i) {
        g_file.u16(static_cast<std::uint16_t>(l16(i)));
        g_file.u16(static_cast<std::uint16_t>(r16(i)));
      }
    } else if (depth == 24u) {
      g_file.riff().fmt(1, 2, 44100, 24).data_header(kFrames * 6);
      for (std::uint32_t i = 0; i < kFrames; ++i) {  // the low byte is zero: nothing to dither, so exact
        g_file.u8(0); g_file.u16(static_cast<std::uint16_t>(l16(i)));
        g_file.u8(0); g_file.u16(static_cast<std::uint16_t>(r16(i)));
      }
    } else {
      g_file.riff().fmt(3, 2, 44100, 32).data_header(kFrames * 8);
      for (std::uint32_t i = 0; i < kFrames; ++i) {  // k / 32768 is exact in float and a whole 16-bit step
        for (const std::int16_t v : {l16(i), r16(i)}) {
          const float f = static_cast<float>(v) / 32768.0f;
          std::uint32_t bits;
          std::memcpy(&bits, &f, 4);
          g_file.u32(bits);
        }
      }
    }
    g_file.end();
    for (std::uint8_t pick : {std::uint8_t{1}, std::uint8_t{2}}) {
      g_channel = pick;
      for (std::size_t budget : {std::size_t{4096}, std::size_t{7}}) {  // 7: frames split across reads
        Run r;
        CHECK(run_file(r, kFrames, budget) == wav::Progress::Done);  // the room of the mix: one sample a frame
        CHECK(r.imp.info().frames == kFrames);
        CHECK(r.imp.info().channels == 1);
        CHECK(r.imp.info().source_channels == 2);
        CHECK_FALSE(r.imp.info().dithered);
        bool exact = true;
        for (std::uint32_t i = 0; i < kFrames; ++i) {
          const std::int16_t want = pick == 1 ? l16(i) : r16(i);
          const std::int16_t expect = depth == 8u ? static_cast<std::int16_t>((want >> 8) * 256) : want;
          exact = exact && g_dst[i] == expect;
        }
        CHECK_MESSAGE(exact, "depth " << depth << " channel " << int(pick) << " budget " << budget);
      }
    }
  }
  // a mono file has no channel to pick; keep_stereo wins over the pick
  g_channel = 2;
  make_pcm16(100);
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(g_dst[5] == ramp_value(5));
  g_channel = 0;
  g_keep_stereo = true;
}

TEST_CASE("wav: a stereo file is mixed to mono by default, (L + R) / 2, and keep_stereo keeps it stereo") {
  g_keep_stereo = false;
  // L = R: the mean is the sample itself, bit-exact, nothing dithered, whatever the depth stored it in.
  g_file.riff().fmt(1, 2, 44100, 16).data_header(300 * 4);
  for (std::uint32_t i = 0; i < 300; ++i) {
    g_file.u16(static_cast<std::uint32_t>(ramp_value(i)) & 0xFFFFu);
    g_file.u16(static_cast<std::uint32_t>(ramp_value(i)) & 0xFFFFu);
  }
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  const wav::Info& info = r.imp.info();
  CHECK(info.frames == 300u);
  CHECK(info.channels == 1);
  CHECK(info.source_channels == 2);
  CHECK(info.downmixed);
  CHECK_FALSE(info.dithered);
  bool exact = true;
  for (std::uint32_t i = 0; i < 300; ++i) exact = exact && g_dst[i] == ramp_value(i);
  CHECK(exact);
  CHECK(untouched(300, kDst - 300));  // one sample a frame

  // The room: 300 frames need 300 samples mixed and 600 kept.
  CHECK(run_file(r, 299) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::LimitExceeded);
  CHECK(run_file(r, 300) == wav::Progress::Done);
  g_keep_stereo = true;
  CHECK(run_file(r, 599) == wav::Progress::Failed);
  CHECK(run_file(r, 600) == wav::Progress::Done);
  CHECK(r.imp.info().channels == 2);
  CHECK_FALSE(r.imp.info().downmixed);
  CHECK(g_dst[2 * 7] == ramp_value(7));  // interleaved, as it was
  CHECK(g_dst[2 * 7 + 1] == ramp_value(7));
  g_keep_stereo = false;

  // L = 1, R = 0: the mean is half a step, which dithers to 0 or 1, about half and half.
  g_file.riff().fmt(1, 2, 44100, 16).data_header(2000 * 4);
  for (int i = 0; i < 2000; ++i) {
    g_file.u16(1);
    g_file.u16(0);
  }
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(r.imp.info().dithered);
  unsigned ones = 0;
  bool only01 = true;
  for (int i = 0; i < 2000; ++i) {
    ones += g_dst[i] == 1;
    only01 = only01 && (g_dst[i] == 0 || g_dst[i] == 1);
  }
  CHECK(only01);
  CHECK(ones > 900u);
  CHECK(ones < 1100u);
  // Opposite signs cancel exactly.
  g_file.riff().fmt(1, 2, 44100, 16).data_header(4 * 4);
  for (std::uint32_t i = 0; i < 4; ++i) {
    g_file.u16(1000 * (i + 1));
    g_file.u16(static_cast<std::uint32_t>(-static_cast<std::int32_t>(1000 * (i + 1))) & 0xFFFFu);
  }
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK((g_dst[0] == 0 && g_dst[1] == 0 && g_dst[2] == 0 && g_dst[3] == 0));
  CHECK_FALSE(r.imp.info().dithered);

  // 8-bit: 255 and 129 are +127 and +1 steps of 256: the mean 16,384 is whole.
  g_file.riff().fmt(1, 2, 8000, 8).data_header(4);
  g_file.u8(255);
  g_file.u8(129);
  g_file.u8(0);
  g_file.u8(0);
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(g_dst[0] == 16384);
  CHECK(g_dst[1] == -32768);
  CHECK_FALSE(r.imp.info().dithered);

  // 24-bit: both channels at the top and at the bottom clip, never wrap.
  g_file.riff().fmt(1, 2, 44100, 24).data_header(2 * 6);
  for (const std::int32_t v : {8388607, 8388607, -8388608, -8388608}) {
    const std::uint32_t u = static_cast<std::uint32_t>(v) & 0xFFFFFFu;
    g_file.u8(u);
    g_file.u8(u >> 8);
    g_file.u8(u >> 16);
  }
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(g_dst[0] == 32767);
  CHECK(g_dst[1] == -32768);

  // Float: 0.5 and -0.25 mix to 0.125 = 4096 exactly; a NaN reads as silence, so NaN and 1.0 mix to half of full scale.
  g_file.riff().fmt(3, 2, 48000, 32).data_header(2 * 8);
  const float fl[4] = {0.5f, -0.25f, 0.0f, 1.0f};
  std::uint32_t fb[4];
  std::memcpy(fb, fl, sizeof fl);
  fb[2] = 0x7FC00000u;
  for (const std::uint32_t b : fb) g_file.u32(b);
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(g_dst[0] == 4096);
  CHECK(g_dst[1] == 16384);
  CHECK_FALSE(r.imp.info().dithered);

  // However the bytes arrive (a frame split across reads and steps), the mix is the same, and the same every time.
  g_file.riff().fmt(1, 2, 44100, 24).data_header(500 * 6);
  test::Rng rng(31);
  for (int i = 0; i < 1000; ++i) {
    const std::uint32_t u = rng.next() & 0xFFFFFFu;
    g_file.u8(u);
    g_file.u8(u >> 8);
    g_file.u8(u >> 16);
  }
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Done);
  static std::int16_t want[500];
  std::memcpy(want, g_dst, sizeof want);
  CHECK(r.imp.info().dithered);
  for (const std::size_t piece : {std::size_t{1}, std::size_t{5}, std::size_t{7}, std::size_t{11}}) {
    test::MemorySource src(g_file.b, g_file.n);
    src.max_per_read = piece;
    Run r2;
    CHECK(run(r2, src, kDst, 13) == wav::Progress::Done);
    CHECK_MESSAGE(std::memcmp(g_dst, want, sizeof want) == 0, "piece " << piece);
  }
  // A mono file has nothing to mix: the flag changes nothing.
  make_pcm16(100);
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK((r.imp.info().channels == 1 && r.imp.info().source_channels == 1 && !r.imp.info().downmixed));
  CHECK(g_dst[5] == ramp_value(5));
  g_keep_stereo = true;
}

TEST_CASE("wav: any sample rate is kept as it is") {
  const std::uint32_t rates[] = {1, 8000, 11025, 22050, 44100, 44117, 48000, 88200, 96000, 192000, 384000, 0x7FFFFFFFu};
  for (const std::uint32_t rate : rates) {
    g_file.riff().fmt(1, 1, rate, 16).data_header(20);
    ramp16(g_file, 10);
    g_file.end();
    Run r;
    CHECK(run_file(r) == wav::Progress::Done);
    CHECK(r.imp.info().sample_rate == rate);
  }
}

TEST_CASE("wav: WAVE_FORMAT_EXTENSIBLE with PCM or float; a foreign sub-format is refused") {
  g_file.riff().fmt_ext(1, 2, 48000, 24).data_header(10 * 6);
  for (std::uint32_t i = 0; i < 20; ++i) {
    g_file.u8(0);
    g_file.u16(i * 100);
  }
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(r.imp.info().bits == 24);
  CHECK(r.imp.info().channels == 2);
  CHECK(g_dst[7] == 700);

  g_file.riff().fmt_ext(3, 1, 44100, 32).data_header(8);
  const float one = 0.25f;
  std::uint32_t b;
  std::memcpy(&b, &one, 4);
  g_file.u32(b);
  g_file.u32(b);
  g_file.end();
  Run r2;
  CHECK(run_file(r2) == wav::Progress::Done);
  CHECK(r2.imp.info().format == wav::SampleFormat::Float);
  CHECK(g_dst[1] == 8192);

  g_file.riff().fmt_ext(1, 1, 44100, 16, false).data_header(4);
  g_file.u32(0);
  g_file.end();
  Run r3;
  CHECK(run_file(r3) == wav::Progress::Failed);
  CHECK(r3.imp.report().error == wav::Error::Unsupported);

  g_file.riff().fmt_ext(2, 1, 44100, 16).data_header(4);  // ADPCM sub-format
  g_file.u32(0);
  g_file.end();
  Run r4;
  CHECK(run_file(r4) == wav::Progress::Failed);
  CHECK(r4.imp.report().error == wav::Error::Unsupported);
}

TEST_CASE("wav: other chunks are skipped, odd sizes are padded, and a missing final pad is accepted") {
  g_file.riff().list_chunk().fmt(1, 1, 44100, 16).list_chunk();
  g_file.data_header(7 * 2);
  ramp16(g_file, 7);
  g_file.list_chunk();
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(r.imp.info().frames == 7u);
  CHECK(g_dst[3] == ramp_value(3));
  CHECK_FALSE(r.imp.report().trailing_ignored);

  // An odd data chunk (8-bit, 7 samples) with and without its pad byte at the end of the RIFF.
  g_file.riff().fmt(1, 1, 8000, 8).data_header(7);
  for (int i = 0; i < 7; ++i) g_file.u8(static_cast<std::uint32_t>(128 + i));
  g_file.end();  // no pad written: the RIFF ends right after the data
  Run r2;
  CHECK(run_file(r2) == wav::Progress::Done);
  CHECK(r2.imp.info().frames == 7u);
  CHECK_FALSE(r2.imp.report().trailing_ignored);
  g_file.u8(0);  // with the pad (and a RIFF size that counts it)
  g_file.end();
  Run r3;
  CHECK(run_file(r3) == wav::Progress::Done);
}

TEST_CASE("wav: smpl gives the root note and the first loop; chunks before and after the data are both read") {
  for (int after = 0; after < 2; ++after) {
    g_file.riff().fmt(1, 1, 44100, 16);
    if (after == 0) g_file.smpl(69, 2, 1, 100, 399);
    g_file.data_header(500 * 2);
    ramp16(g_file, 500);
    if (after == 1) g_file.smpl(69, 2, 1, 100, 399);
    g_file.end();
    Run r;
    CHECK(run_file(r) == wav::Progress::Done);
    const wav::Info& i = r.imp.info();
    CHECK(i.root_note == 69);
    CHECK(i.has_loop);
    CHECK(i.loop_type == wav::LoopType::PingPong);
    CHECK(i.loop_start == 100u);
    CHECK(i.loop_end == 400u);  // the file's last frame (399) plus one
    CHECK(r.imp.report().loops_dropped == 1u);
    CHECK_FALSE(r.imp.report().smpl_ignored);
  }
  // Forward and backward, and a loop that ends one past the end (frames) is read as the whole tail.
  const std::uint32_t types[3] = {0, 2, 0};
  const std::uint32_t ends[3] = {499, 499, 500};
  for (int k = 0; k < 3; ++k) {
    g_file.riff().fmt(1, 1, 44100, 16).smpl(60, 1, types[k], 10, ends[k]).data_header(1000);
    ramp16(g_file, 500);
    g_file.end();
    Run r;
    CHECK(run_file(r) == wav::Progress::Done);
    CHECK(r.imp.info().has_loop);
    CHECK(r.imp.info().loop_type == static_cast<wav::LoopType>(types[k]));
    CHECK(r.imp.info().loop_end == 500u);
  }
  // No loops: only the root note. An unusable loop is dropped and reported, the audio is fine.
  g_file.riff().fmt(1, 1, 44100, 16).smpl(40, 0, 0, 0, 0).data_header(1000);
  ramp16(g_file, 500);
  g_file.end();
  Run r1;
  CHECK(run_file(r1) == wav::Progress::Done);
  CHECK(r1.imp.info().root_note == 40);
  CHECK_FALSE(r1.imp.info().has_loop);
  const struct { std::uint32_t type, start, end; } bad[] = {{3, 10, 100}, {0, 101, 100}, {0, 200, 100}, {0, 10, 600}, {0, 10, 0xFFFFFFFFu}};
  for (const auto& l : bad) {
    g_file.riff().fmt(1, 1, 44100, 16).smpl(60, 1, l.type, l.start, l.end).data_header(1000);
    ramp16(g_file, 500);
    g_file.end();
    Run r;
    CHECK(run_file(r) == wav::Progress::Done);
    CHECK_FALSE(r.imp.info().has_loop);
    CHECK(r.imp.report().smpl_ignored);
    CHECK(r.imp.info().frames == 500u);
  }
}

TEST_CASE("wav: cue points become slices: sorted, unique, inside the sample, the lowest 48") {
  const std::uint32_t cues[] = {400, 100, 100, 0, 250, 499, 500, 9000};
  for (int after = 0; after < 2; ++after) {
    g_file.riff().fmt(1, 1, 44100, 16);
    if (after == 0) g_file.cue(cues, 8);
    g_file.data_header(1000);
    ramp16(g_file, 500);
    if (after == 1) g_file.cue(cues, 8);
    g_file.end();
    Run r;
    CHECK(run_file(r) == wav::Progress::Done);
    const wav::Info& i = r.imp.info();
    CHECK(i.slice_count == 4);  // 100, 250, 400, 499: not 0, not 500 or 9000 (outside), the duplicate once
    CHECK(i.slices[0] == 100u);
    CHECK(i.slices[1] == 250u);
    CHECK(i.slices[2] == 400u);
    CHECK(i.slices[3] == 499u);
    CHECK_FALSE(r.imp.report().cue_ignored);
  }
  // 60 distinct cue points: the lowest 48 are kept, 12 are counted as dropped.
  std::uint32_t many[60];
  for (std::uint32_t k = 0; k < 60; ++k) many[k] = 2000 - 3 * k;  // descending: the table must keep the lowest ones
  g_file.riff().fmt(1, 1, 44100, 16).cue(many, 60).data_header(4000);
  ramp16(g_file, 2000);
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(r.imp.info().slice_count == 48);
  CHECK(r.imp.report().slices_dropped == 12u);
  bool ascending = true;
  for (unsigned k = 1; k < 48; ++k) ascending = ascending && r.imp.info().slices[k] > r.imp.info().slices[k - 1];
  CHECK(ascending);
  CHECK(r.imp.info().slices[0] == 2000u - 3u * 59u);
  CHECK(r.imp.info().slices[47] == 2000u - 3u * 12u);
  CHECK(static_cast<unsigned>(wav::kMaxSlices) == 48u);
}

TEST_CASE("wav: hostile smpl and cue chunks are dropped and reported, the audio still comes in") {
  // A smpl that claims four billion loops and a cue that claims four billion points.
  const std::uint32_t cue1[1] = {10};
  g_file.riff().fmt(1, 1, 44100, 16).smpl(60, 1, 0, 10, 100, 0xFFFFFFFFu - 1u).cue(cue1, 1, 0xFFFFFFFFu - 1u).data_header(1000);
  ramp16(g_file, 500);
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(r.imp.report().smpl_ignored);
  CHECK(r.imp.report().cue_ignored);
  CHECK_FALSE(r.imp.info().has_loop);
  CHECK(r.imp.info().slice_count == 0);
  CHECK(r.imp.info().frames == 500u);

  // A root note that is not a MIDI note, and a smpl chunk too short to be one.
  g_file.riff().fmt(1, 1, 44100, 16).smpl(200, 0, 0, 0, 0).data_header(1000);
  ramp16(g_file, 500);
  g_file.end();
  Run r2;
  CHECK(run_file(r2) == wav::Progress::Done);
  CHECK(r2.imp.report().smpl_ignored);
  CHECK(r2.imp.info().root_note == 60);
  g_file.riff().fmt(1, 1, 44100, 16);
  g_file.tag("smpl");
  g_file.u32(12);
  g_file.zeros(12);
  g_file.tag("cue ");
  g_file.u32(2);  // too short for a count
  g_file.zeros(2);
  g_file.data_header(1000);
  ramp16(g_file, 500);
  g_file.end();
  Run r3;
  CHECK(run_file(r3) == wav::Progress::Done);
  CHECK(r3.imp.report().smpl_ignored);
  CHECK(r3.imp.report().cue_ignored);
}

TEST_CASE("wav: a partial frame at the end of the data chunk is dropped and said") {
  g_file.riff().fmt(1, 2, 44100, 16).data_header(13);  // three frames and a byte
  for (int i = 0; i < 13; ++i) g_file.u8(static_cast<std::uint32_t>(i + 1));
  g_file.u8(0);
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Done);
  CHECK(r.imp.info().frames == 3u);
  CHECK(r.imp.report().trailing_audio_bytes);
  CHECK(untouched(6, kDst - 6));
}

TEST_CASE("wav: the RIFF size may be unknown (0 or 0xFFFFFFFF) when a streaming writer left it; the data still has to be there") {
  for (const std::uint32_t size : {0u, 0xFFFFFFFFu}) {
    make_pcm16(100);
    g_file.b[4] = static_cast<std::uint8_t>(size);
    g_file.b[5] = static_cast<std::uint8_t>(size >> 8);
    g_file.b[6] = static_cast<std::uint8_t>(size >> 16);
    g_file.b[7] = static_cast<std::uint8_t>(size >> 24);
    Run r;
    CHECK(run_file(r) == wav::Progress::Done);
    CHECK(r.imp.info().frames == 100u);
  }
  // With the size unknown, the file ends where the source ends: right after the data is a clean end.
  make_pcm16(100);
  g_file.b[4] = g_file.b[5] = g_file.b[6] = g_file.b[7] = 0xFF;
  g_file.n -= 10;  // ...but cut inside the audio it is Truncated
  Run r;
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Truncated);
}

// ---- hostile files -------------------------------------------------------------------------------------------------------

TEST_CASE("wav: not a WAV file: bad magic, foreign containers, tiny and empty sources") {
  Run r;
  {
    test::MemorySource src(g_file.b, 0);
    CHECK(run(r, src, kDst) == wav::Progress::Failed);
    CHECK(r.imp.report().error == wav::Error::Truncated);
  }
  make_pcm16(50);
  for (std::size_t cut = 0; cut < 12; ++cut) {  // inside the 12-byte header
    test::MemorySource src(g_file.b, cut);
    CHECK(run(r, src, kDst) == wav::Progress::Failed);
    CHECK(r.imp.report().error == wav::Error::Truncated);
  }
  const char* bad[] = {"RIFX", "RF64", "BW64"};
  for (const char* id : bad) {
    make_pcm16(50);
    std::memcpy(g_file.b, id, 4);
    CHECK(run_file(r) == wav::Progress::Failed);
    CHECK(r.imp.report().error == wav::Error::Unsupported);
  }
  make_pcm16(50);
  std::memcpy(g_file.b, "FORM", 4);
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::BadMagic);
  make_pcm16(50);
  std::memcpy(g_file.b + 8, "AVI ", 4);
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::BadMagic);
  make_pcm16(50);
  g_file.b[4] = 3;
  g_file.b[5] = g_file.b[6] = g_file.b[7] = 0;  // a RIFF of 3 bytes cannot even hold its form type
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);
  CHECK(guards_intact());
}

TEST_CASE("wav: truncation at every offset ends in Truncated or a clean Done, never anything else, never out of bounds") {
  const std::uint32_t cues[] = {100, 200};
  g_file.riff().fmt(1, 2, 44100, 16).list_chunk().smpl(60, 1, 0, 10, 100).data_header(2 * 2 * 150);
  ramp16(g_file, 300);
  g_file.cue(cues, 2);
  g_file.end();
  const std::size_t total = g_file.n;
  std::size_t audio_start = 0;
  for (std::size_t i = 12; i + 8 <= total; ++i) {  // where the data chunk's audio starts
    if (std::memcmp(g_file.b + i, "data", 4) == 0) { audio_start = i + 8; break; }
  }
  NEED(audio_start != 0);
  const std::size_t audio_end = audio_start + 2 * 2 * 150;
  for (std::size_t cut = 0; cut < total; ++cut) {
    test::MemorySource src(g_file.b, cut);
    Run r;
    const wav::Progress p = run(r, src, kDst, 37);
    if (cut < audio_end) {
      CHECK_MESSAGE(p == wav::Progress::Failed, "cut " << cut);
      CHECK_MESSAGE(r.imp.report().error == wav::Error::Truncated, "cut " << cut);
    } else {
      // After the audio only the cue chunk is missing or cut: the sample is complete.
      CHECK_MESSAGE(p == wav::Progress::Done, "cut " << cut);
      CHECK_MESSAGE(r.imp.info().frames == 150u, "cut " << cut);
    }
    CHECK(guards_intact());
  }
  CHECK(guards_intact());
}

TEST_CASE("wav: a data chunk that does not fit is refused before anything is written") {
  make_pcm16(1000);
  Run r;
  CHECK(run_file(r, 999) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::LimitExceeded);
  CHECK(untouched(0, kDst));
  CHECK(run_file(r, 1000) == wav::Progress::Done);
  CHECK(run_file(r, 0) == wav::Progress::Failed);  // no room at all
  CHECK(r.imp.report().error == wav::Error::LimitExceeded);
  {
    test::MemorySource src(g_file.b, g_file.n);
    wav::Importer imp;
    imp.begin(src, nullptr, 5000);  // no buffer: capacity is nothing, whatever was said
    CHECK(imp.step() == wav::Progress::Failed);
    CHECK(imp.report().error == wav::Error::LimitExceeded);
  }
  // The data chunk claims 4 GiB - 1 in a RIFF that says it is small, and in one that says nothing.
  make_pcm16(100);
  const std::size_t size_at = 12 + 8 + 16 + 4;
  g_file.b[size_at] = g_file.b[size_at + 1] = g_file.b[size_at + 2] = g_file.b[size_at + 3] = 0xFF;
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);  // longer than the RIFF
  g_file.b[4] = g_file.b[5] = g_file.b[6] = g_file.b[7] = 0xFF;
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::LimitExceeded);  // no RIFF bound to stop it: the buffer is the bound
  CHECK(guards_intact());
}

TEST_CASE("wav: a bad fmt chunk is refused with the right error") {
  struct Case {
    unsigned tag, channels;
    std::uint32_t rate;
    unsigned bits;
    wav::Error error;
  };
  const Case cases[] = {
      {1, 1, 44100, 16, wav::Error::None},
      {2, 1, 44100, 4, wav::Error::Unsupported},       // ADPCM
      {0x55, 1, 44100, 16, wav::Error::Unsupported},   // MP3
      {6, 1, 8000, 8, wav::Error::Unsupported},        // A-law
      {1, 3, 44100, 16, wav::Error::Unsupported},      // 3 channels
      {1, 6, 48000, 16, wav::Error::Unsupported},      // 5.1
      {1, 0, 44100, 16, wav::Error::Malformed},        // no channels
      {1, 1, 0, 16, wav::Error::Malformed},            // rate 0
      {1, 1, 44100, 0, wav::Error::Malformed},         // depth 0
      {1, 1, 44100, 32, wav::Error::Unsupported},      // 32-bit integer
      {1, 1, 44100, 12, wav::Error::Unsupported},
      {3, 1, 44100, 64, wav::Error::Unsupported},      // 64-bit float
      {3, 1, 44100, 16, wav::Error::Malformed},        // 16-bit "float"
  };
  for (const Case& c : cases) {
    g_file.riff();
    g_file.tag("fmt ");
    g_file.u32(16);
    g_file.u16(c.tag);
    g_file.u16(c.channels);
    g_file.u32(c.rate);
    g_file.u32(c.rate * c.channels * (c.bits / 8));
    g_file.u16(c.channels * (c.bits / 8));
    g_file.u16(c.bits);
    g_file.data_header(64);
    g_file.zeros(64);
    g_file.end();
    Run r;
    const wav::Progress p = run_file(r);
    CHECK_MESSAGE(r.imp.report().error == c.error, "tag " << c.tag << " ch " << c.channels << " bits " << c.bits);
    CHECK((p == wav::Progress::Done) == (c.error == wav::Error::None));
  }
  // The block alignment must agree with the format; the fmt chunk must be at least 16 bytes.
  g_file.riff().fmt(1, 1, 44100, 16);
  g_file.b[12 + 8 + 12] = 3;  // block align 3 for 16-bit mono
  g_file.data_header(64);
  g_file.zeros(64);
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);
  g_file.riff();
  g_file.tag("fmt ");
  g_file.u32(14);
  g_file.zeros(14);
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);
}

TEST_CASE("wav: chunks in the wrong order, missing or doubled, are refused") {
  Run r;
  g_file.riff().data_header(8);  // data before fmt
  g_file.zeros(8);
  g_file.fmt(1, 1, 44100, 16);
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);

  g_file.riff().fmt(1, 1, 44100, 16).fmt(1, 1, 44100, 16).data_header(8);  // two fmt
  g_file.zeros(8);
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);

  g_file.riff().fmt(1, 1, 44100, 16).data_header(8);  // two data
  g_file.zeros(8);
  g_file.data_header(8);
  g_file.zeros(8);
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);

  g_file.riff().fmt(1, 1, 44100, 16).list_chunk();  // no data
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);

  g_file.riff().list_chunk();  // no fmt
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);

  g_file.riff().fmt(1, 1, 44100, 16).data_header(0);  // an empty data chunk, and one shorter than a frame
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);
  g_file.riff().fmt(1, 2, 44100, 16).data_header(3);
  g_file.zeros(4);
  g_file.end();
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);
}

TEST_CASE("wav: a chunk longer than the RIFF is refused before the audio, and ignored after it") {
  g_file.riff().fmt(1, 1, 44100, 16);
  g_file.tag("LIST");
  g_file.u32(5000);  // far beyond the RIFF
  g_file.zeros(10);
  g_file.end();
  Run r;
  CHECK(run_file(r) == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::Malformed);

  make_pcm16(100);
  const std::size_t audio_end = g_file.n;
  g_file.tag("junk");
  g_file.u32(0x40000000u);  // after the audio: the sample is good, the rest is not read
  g_file.zeros(4);
  g_file.end();
  CHECK(audio_end < g_file.n);
  Run r2;
  CHECK(run_file(r2) == wav::Progress::Done);
  CHECK(r2.imp.info().frames == 100u);
  CHECK(r2.imp.report().trailing_ignored);
}

TEST_CASE("wav: a source that fails, lies or blocks") {
  make_pcm16(500);
  {
    test::MemorySource src(g_file.b, g_file.n);
    src.lie = test::MemorySource::Lie::TooMany;
    Run r;
    CHECK(run(r, src, kDst) == wav::Progress::Failed);
    CHECK(r.imp.report().error == wav::Error::SourceFailed);
  }
  {
    test::MemorySource src(g_file.b, g_file.n);
    src.lie = test::MemorySource::Lie::OkZero;
    Run r;
    CHECK(run(r, src, kDst) == wav::Progress::Failed);
    CHECK(r.imp.report().error == wav::Error::SourceFailed);
  }
  for (const std::size_t at : {std::size_t{0}, std::size_t{5}, std::size_t{30}, std::size_t{200}, g_file.n - 1}) {
    test::MemorySource src(g_file.b, g_file.n);
    src.fail_at = at;
    Run r;
    CHECK(run(r, src, kDst, 50) == wav::Progress::Failed);
    CHECK_MESSAGE(r.imp.report().error == wav::Error::SourceFailed, "fail at " << at);
  }
  // A source that always blocks never completes and never spins: every step returns.
  {
    test::MemorySource src(g_file.b, g_file.n);
    src.block_every = 1;
    wav::Importer imp;
    imp.begin(src, g_dst, kDst);
    for (int i = 0; i < 100; ++i) {
      CHECK(imp.step(4096) == wav::Progress::Running);
    }
    CHECK(imp.report().bytes == 0u);
  }
  CHECK(guards_intact());
}

TEST_CASE("wav: cancel ends the run as Cancelled, once, and a finished run is left alone") {
  make_pcm16(1000);
  test::MemorySource src(g_file.b, g_file.n);
  wav::Importer imp;
  clear_dst();
  imp.begin(src, g_dst, kDst);
  CHECK(imp.step(300) == wav::Progress::Running);
  imp.cancel();
  CHECK(imp.progress() == wav::Progress::Failed);
  CHECK(imp.report().error == wav::Error::Cancelled);
  CHECK(imp.step(300) == wav::Progress::Failed);  // not stepped again
  CHECK(imp.report().error == wav::Error::Cancelled);

  test::MemorySource src2(g_file.b, g_file.n);
  imp.begin(src2, g_dst, kDst);  // begin() starts afresh
  CHECK(imp.progress() == wav::Progress::Running);
  while (imp.step() == wav::Progress::Running) {}
  CHECK(imp.progress() == wav::Progress::Done);
  imp.cancel();
  CHECK(imp.progress() == wav::Progress::Done);
  CHECK(imp.report().error == wav::Error::None);
}

TEST_CASE("wav: random damage to a rich file: always terminates, always inside the buffer, a Done is self-consistent") {
#if defined(__arm__)
  constexpr unsigned kRounds = 300;
#else
  constexpr unsigned kRounds = 30000;
#endif
  const std::uint32_t cues[] = {50, 120, 7000};
  g_file.riff().fmt(1, 2, 44100, 16).list_chunk().smpl(61, 2, 0, 20, 110).data_header(2 * 2 * 150);
  ramp16(g_file, 300);
  g_file.cue(cues, 3);
  g_file.end();
  static std::uint8_t pristine[kMaxFile];
  const std::size_t total = g_file.n;
  std::memcpy(pristine, g_file.b, total);

  test::Rng rng(0xDA3A6E5u);
  unsigned done = 0;
  unsigned failed = 0;
  for (unsigned round = 0; round < kRounds; ++round) {
    std::memcpy(g_file.b, pristine, total);
    const unsigned edits = 1 + rng.below(4);
    for (unsigned e = 0; e < edits; ++e) {
      const std::size_t at = rng.below(static_cast<std::uint32_t>(total));
      switch (rng.below(3)) {
        case 0: g_file.b[at] = rng.byte(); break;
        case 1: g_file.b[at] ^= static_cast<std::uint8_t>(1u << rng.below(8)); break;
        default: g_file.b[at] = (rng.below(2) != 0) ? 0xFF : 0x00; break;
      }
    }
    const std::size_t len = rng.below(8) == 0 ? rng.below(static_cast<std::uint32_t>(total)) : total;
    test::MemorySource src(g_file.b, len);
    src.max_per_read = 1 + rng.below(300);
    src.block_every = rng.below(4) == 0 ? 2 + rng.below(5) : 0;
    const std::size_t capacity = (rng.below(6) == 0) ? rng.below(400) : kDst;
    g_keep_stereo = rng.below(2) != 0;  // both layouts get damaged files
    Run r;
    const wav::Progress p = run(r, src, capacity, 1 + rng.below(700), 100000);
    CHECK_MESSAGE(p != wav::Progress::Running, "round " << round << " did not terminate");
    CHECK_MESSAGE(guards_intact(), "round " << round << " wrote outside the buffer");
    const wav::Info& i = r.imp.info();
    // Nothing past what the info says was converted may be written (the part after is still the canary).
    const std::size_t written = static_cast<std::size_t>(i.frames) * (i.channels != 0 ? i.channels : 1u);
    const bool clean_tail = untouched(written, kDst - written);
    CHECK_MESSAGE((clean_tail || p == wav::Progress::Failed), "round " << round);
    if (p == wav::Progress::Done) {
      ++done;
      CHECK(i.frames >= 1u);
      CHECK(static_cast<std::size_t>(i.frames) * i.channels <= capacity);
      CHECK((i.channels == 1 || i.channels == 2));
      CHECK(i.sample_rate >= 1u);
      CHECK(i.slice_count <= wav::kMaxSlices);
      for (unsigned k = 0; k < i.slice_count; ++k) {
        CHECK(i.slices[k] >= 1u);
        CHECK(i.slices[k] < i.frames);
        CHECK((k == 0 || i.slices[k] > i.slices[k - 1]));
      }
      if (i.has_loop) {
        CHECK(i.loop_start < i.loop_end);
        CHECK(i.loop_end <= i.frames);
      }
      CHECK(i.root_note <= 127);
      CHECK(r.imp.permille() == 1000u);
    } else {
      ++failed;
      CHECK(r.imp.report().error != wav::Error::None);
      CHECK(r.imp.report().detail[0] != '\0');
    }
  }
  g_keep_stereo = true;
  CHECK(done > kRounds / 20);   // the damage is not always fatal (a hit in the audio itself, in a skipped chunk...)
  CHECK(failed > kRounds / 20); // and often it is
}

// ---- OS-STEREO step 2, slice (b): the side signal -------------------------------------------------------------------------------------------------

namespace {
struct SideRun {
  wav::Importer imp;
  std::vector<std::int16_t> out;
  wav::Progress result = wav::Progress::Running;
};
// Reads g_file into `capacity` samples with the given mode; steps of `budget` bytes.
void side_run(SideRun& r, std::size_t capacity, bool side, std::uint8_t channel = 0, bool keep = false, std::size_t budget = 4096) {
  r.out.assign(capacity + 4u, 0x5A5A);
  test::MemorySource src(g_file.b, g_file.n);
  r.imp.begin(src, r.out.data(), capacity, keep, false, channel, side);
  r.result = wav::Progress::Running;
  for (unsigned steps = 0; r.result == wav::Progress::Running && steps < 200000u; ++steps) {
    r.result = r.imp.step(budget);
  }
}
// A stereo 16-bit file: L = a ramp, R = another one with a different slope (the side is neither zero nor constant).
void make_stereo16(std::uint32_t frames) {
  g_file.riff().fmt(1, 2, 44100, 16).data_header(frames * 4);
  for (std::uint32_t i = 0; i < frames; ++i) {
    g_file.u16(static_cast<std::uint32_t>(ramp_value(i)) & 0xFFFFu);
    g_file.u16(static_cast<std::uint32_t>(ramp_value(i, 900) * -3) & 0xFFFFu);
  }
  g_file.end();
}
std::int16_t clamp_s16(std::int32_t v) { return static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }
}  // namespace

TEST_CASE("wav: side mode reads the mid bit for bit as the mono load does, and S = L - M after it at the even index") {
  for (const std::uint32_t frames : {300u, 301u}) {
    make_stereo16(frames);
    SideRun mono;
    side_run(mono, 4000, false);
    NEED(mono.result == wav::Progress::Done);
    SideRun st;
    side_run(st, 4000, true);
    NEED(st.result == wav::Progress::Done);
    const wav::Info& info = st.imp.info();
    CHECK(info.frames == frames);
    CHECK(info.channels == 1);
    CHECK(info.side);
    const std::uint32_t even = (frames + 1u) & ~1u;
    CHECK(info.side_offset == even);
    CHECK_FALSE(mono.imp.info().side);
    bool m_same = true;
    bool s_ok = true;
    bool l_ok = true;
    for (std::uint32_t i = 0; i < frames; ++i) {
      m_same = m_same && st.out[i] == mono.out[i];
      const std::int16_t l = ramp_value(i);
      s_ok = s_ok && st.out[even + i] == clamp_s16(static_cast<std::int32_t>(l) - st.out[i]);
      l_ok = l_ok && clamp_s16(static_cast<std::int32_t>(st.out[i]) + st.out[even + i]) == l;  // L = M + S, exact (it does not clamp here)
    }
    CHECK(m_same);
    CHECK(s_ok);
    CHECK(l_ok);
    if (even != frames) {
      CHECK(st.out[frames] == 0x5A5A);  // the gap between M and S is not written
    }
    CHECK(st.out[even + frames] == 0x5A5A);  // nothing past S
    // Small steps give the same bytes.
    SideRun small;
    side_run(small, 4000, true, 0, false, 7);
    NEED(small.result == wav::Progress::Done);
    CHECK(small.out == st.out);
  }
}

TEST_CASE("wav: side mode needs room for both blocks and fails before writing a byte, never clipping") {
  make_stereo16(301);  // even 302: M and S take 604
  SideRun r;
  side_run(r, 603, true);
  CHECK(r.result == wav::Progress::Failed);
  CHECK(r.imp.report().error == wav::Error::LimitExceeded);
  bool untouched_all = true;
  for (std::size_t i = 0; i < r.out.size(); ++i) {
    untouched_all = untouched_all && r.out[i] == 0x5A5A;
  }
  CHECK(untouched_all);
  side_run(r, 604, true);
  CHECK(r.result == wav::Progress::Done);
  // The mono load of the same file needs 301.
  side_run(r, 301, false);
  CHECK(r.result == wav::Progress::Done);
}

TEST_CASE("wav: side mode with a 24-bit file keeps the mid's dither stream, and a mono file or keep_stereo has no S") {
  const std::uint32_t frames = 500;
  g_file.riff().fmt(1, 2, 44100, 24).data_header(frames * 6);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const std::uint32_t l = (i * 12345u + 77u) & 0xFFFFFFu;
    const std::uint32_t r = (i * 54321u + 5u) & 0xFFFFFFu;
    g_file.u8(l); g_file.u8(l >> 8); g_file.u8(l >> 16);
    g_file.u8(r); g_file.u8(r >> 8); g_file.u8(r >> 16);
  }
  g_file.end();
  SideRun mono;
  side_run(mono, 4000, false);
  SideRun st;
  side_run(st, 4000, true, 2 /* a channel pick is ignored with side */);
  NEED(mono.result == wav::Progress::Done);
  NEED(st.result == wav::Progress::Done);
  CHECK(st.imp.info().dithered);
  bool same = true;
  for (std::uint32_t i = 0; i < frames; ++i) {
    same = same && st.out[i] == mono.out[i];
  }
  CHECK(same);

  make_pcm16(200);  // a mono file: as ever, no S
  side_run(st, 4000, true);
  NEED(st.result == wav::Progress::Done);
  CHECK_FALSE(st.imp.info().side);
  CHECK(st.imp.info().frames == 200u);

  make_stereo16(100);  // keep_stereo wins: interleaved, no S
  side_run(st, 4000, true, 0, true);
  NEED(st.result == wav::Progress::Done);
  CHECK_FALSE(st.imp.info().side);
  CHECK(st.imp.info().channels == 2);
}
