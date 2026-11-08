// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/export_source.h (OS-WAVEXP): the WAV an export writes, against a fake renderer and an in-memory card (support/mem_card.h) that applies the real path guard. The claims:
//   * the rate converter makes the file's 44,100 Hz of the engine's rate (a sine stays a sine at the same frequency and phase; the duration is the live one) and is a copy when they are the same;
//   * the file is a canonical 44-byte-header stereo 16-bit PCM WAV that an independent parser reads back, its length is the arrangement plus the tail (to the last sound, or the arrangement's
//     end when that is later), its samples are the renderer's;
//   * the render runs in two passes, the engine stays with the export between them and is given back before the card has committed anything;
//   * a cancel at ANY step, a card pulled at ANY write, a card that is full, a read-back that differs, a renderer that refuses or fails or never lets the audio go: no file under the final
//     name, no temporary file, the engine given back, no unguarded call, and the files of the user's untouched;
//   * the names (SaveService): Export/<project>/<project>_Song0001.wav and _P<n>_0001.wav, the next free number, the project part cut to fit the guard, "Untitled" without a project.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/save_source.h"

#include <o3os/app/atomic_save.h>
#include <o3os/app/export_source.h>
#include <o3os/app/save_service.h>
#include <o3os/platform/host/fakes.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace app = o3os::app;
namespace audio = o3os::audio;
namespace haudio = o3os::hal::audio;
using o3os::test::MemCard;
using o3os::test::run;
using Bytes = MemCard::Bytes;
using Mode = app::AtomicSave::Mode;
using Error = app::AtomicSave::Error;
using Phase = app::ExportSource::Phase;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr double kPi = 3.14159265358979323846;
constexpr double kEngineHz = 180000000.0 * o3os::core::kRefCyclesPerSample.den / o3os::core::kRefCyclesPerSample.num;

// A renderer that makes a known signal: `active` blocks of two sines (440 Hz on the left, 660 Hz on the right, amplitude 12,000), the arrangement ending inside the last of them, then a tail
// whose amplitude halves every block. It can be told to refuse, to fail at a block, never to be ready, and it counts what was asked of it.
class FakeRenderer final : public audio::OfflineRenderer {
 public:
  std::uint32_t settle = 3;                 // "nothing yet" answers before the first block of each pass
  std::uint32_t active = 40;                // blocks of the arrangement (the last one holds its end)
  std::uint32_t fail_block = ~0u;           // Failed at this block of the pass
  bool refuse = false;                      // open() says no
  bool never_ready = false;                 // next() says Wait for ever
  bool constant_tail = false;               // the tail never dies (a drone): the cap ends it
  std::uint32_t echo_block = ~0u;           // one more loud block here, after a gap of silence (a delay's repeat)
  MemCard* card = nullptr;

  unsigned opens = 0;
  unsigned closes = 0;
  bool held = false;
  std::vector<std::string> log_at_close;    // the card's log when close() was called

  bool open(const audio::OfflineRequest& r) noexcept override {
    request = r;
    failure_ = "";
    if (refuse) {
      failure_ = "A NOTE IS HELD";
      return false;
    }
    ++opens;
    held = true;
    waited_ = 0;
    block_ = 0;
    return true;
  }
  Step next(std::int16_t* out) noexcept override {
    if (!held) {
      return Step::Failed;
    }
    if (never_ready) {
      return Step::Wait;
    }
    if (waited_ < settle) {
      ++waited_;
      return Step::Wait;
    }
    if (block_ == fail_block) {
      failure_ = "BOOM";
      return Step::Failed;
    }
    sample_block(block_, out);
    ++block_;
    return Step::Block;
  }
  bool in_tail() const noexcept override { return block_ >= active; }
  unsigned progress() const noexcept override { return block_ >= active ? 999u : block_ * 1000u / active; }
  void close() noexcept override {
    ++closes;
    held = false;
    if (card != nullptr) {
      log_at_close = card->log;
    }
  }
  const char* failure() const noexcept override { return failure_; }

  // The block `b` of the signal, as the renderer makes it (also what the test expects).
  void sample_block(std::uint32_t b, std::int16_t* out) const {
    std::int32_t amp = 12000;
    if (b >= active) {
      const std::uint32_t k = b - active + 1u;
      amp = constant_tail ? 40 : (k >= 15u ? 0 : (12000 >> k));
    }
    if (b == echo_block) {
      amp = 8000;
    }
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      const double n = static_cast<double>(b) * kBlk + i;
      out[2 * i] = static_cast<std::int16_t>(std::lround(amp * std::sin(2.0 * kPi * 440.0 * n / kEngineHz)));
      out[2 * i + 1] = static_cast<std::int16_t>(std::lround(amp * std::sin(2.0 * kPi * 660.0 * n / kEngineHz)));
    }
  }

  audio::OfflineRequest request{};

 private:
  const char* failure_ = "";
  std::uint32_t waited_ = 0;
  std::uint32_t block_ = 0;
};

struct Rig {
  Rig() {
    renderer.card = &card;
    app::ExportSource::Wiring w;
    w.renderer = &renderer;
    w.ring = ring;
    w.ring_bytes = sizeof ring;
    source.attach(w);
  }
  bool begin(const char* dir = "Export/Track 01", const char* name = "Track 01_Song0001.wav", audio::OfflineRequest::What what = audio::OfflineRequest::What::Song) {
    audio::OfflineRequest r;
    r.what = what;
    r.pattern = 7;
    if (!source.set(r)) {
      return false;
    }
    return job.begin(card, card, dir, name, Mode::NewOnly, source, "SONG0001", "EXPORT ");
  }
  MemCard card;
  FakeRenderer renderer;
  alignas(4) std::uint8_t ring[app::ExportSource::kRingBytes];
  app::ExportSource source;
  app::AtomicSave job;
};

constexpr const char* kFinal = "Export/Track 01/Track 01_Song0001.wav";
constexpr const char* kTmp = "Export/Track 01/_o3save.tmp";

// ---- A WAV parser that shares nothing with the writer --------------------------------------------------------------------------------------------

struct Wav {
  bool ok = false;
  std::uint16_t format = 0, channels = 0, bits = 0, align = 0;
  std::uint32_t rate = 0, byte_rate = 0, riff_size = 0, data_size = 0;
  std::size_t data_at = 0;
};

std::uint32_t rd32(const Bytes& b, std::size_t at) { return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24); }
std::uint16_t rd16(const Bytes& b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8)); }

Wav parse_wav(const Bytes& b) {
  Wav w;
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
    return w;
  }
  w.riff_size = rd32(b, 4);
  std::size_t at = 12;
  bool have_fmt = false;
  while (at + 8 <= b.size()) {
    const std::uint32_t size = rd32(b, at + 4);
    if (std::memcmp(b.data() + at, "fmt ", 4) == 0 && size >= 16 && at + 8 + 16 <= b.size()) {
      w.format = rd16(b, at + 8);
      w.channels = rd16(b, at + 10);
      w.rate = rd32(b, at + 12);
      w.byte_rate = rd32(b, at + 16);
      w.align = rd16(b, at + 20);
      w.bits = rd16(b, at + 22);
      have_fmt = true;
    } else if (std::memcmp(b.data() + at, "data", 4) == 0) {
      w.data_size = size;
      w.data_at = at + 8;
      w.ok = have_fmt;
      return w;
    }
    at += 8u + size + (size & 1u);
  }
  return w;
}

// What the file must hold: the renderer's blocks through the converter, N frames, N being the rule of export_source.h.
struct Expected {
  Bytes pcm;
  std::uint32_t frames = 0;
};

Expected expected_for(const FakeRenderer& r) {
  FakeRenderer fake;
  fake.active = r.active;
  fake.constant_tail = r.constant_tail;
  app::RateConverter conv;
  conv.reset();
  std::uint32_t out_frames = 0, end_frames = 0, loud_frames = 0, quiet = 0, tail_blocks = 0;
  bool tail = false;
  std::vector<std::int16_t> all;
  std::int16_t in[haudio::kBlockSamples];
  std::int16_t out[app::RateConverter::kMaxOutFrames * 2u];
  for (std::uint32_t b = 0; b < 100000; ++b) {
    fake.sample_block(b, in);
    std::int32_t peak = 0;
    for (std::int16_t s : in) {
      peak = std::max<std::int32_t>(peak, std::abs(static_cast<std::int32_t>(s)));
    }
    const std::uint32_t made = conv.feed(in, kBlk, out);
    all.insert(all.end(), out, out + made * 2u);
    out_frames += made;
    if (peak > app::ExportSource::kSilenceLsb) {
      loud_frames = out_frames;
      quiet = 0;
    } else {
      ++quiet;
    }
    if (!tail && b + 1u >= r.active) {  // in_tail() of the fake: true once the block count is the active blocks'
      tail = true;
      end_frames = out_frames;
    }
    if (tail) {
      ++tail_blocks;
    }
    if (tail && (quiet >= app::ExportSource::kHoldBlocks || tail_blocks >= app::ExportSource::kMaxTailBlocks)) {
      break;
    }
  }
  Expected e;
  e.frames = std::max(end_frames, loud_frames);
  e.pcm.resize(static_cast<std::size_t>(e.frames) * 4u);
  std::memcpy(e.pcm.data(), all.data(), e.pcm.size());
  return e;
}

}  // namespace

// ---- The rate converter --------------------------------------------------------------------------------------------------------------------------

TEST_CASE("export: the rate converter keeps a sine's frequency and phase on the way to 44,100 Hz, and counts like the sums say") {
  app::RateConverter c;
  c.reset();
  const double ratio = app::RateConverter::identity() ? 1.0 : 2500.0 / 2499.0;  // input frames per output frame
  constexpr std::uint32_t kFrames = 64 * 400;
  std::vector<std::int16_t> in(kFrames * 2u);
  for (const double hz : {100.0, 1000.0, 8000.0}) {
    c.reset();
    for (std::uint32_t n = 0; n < kFrames; ++n) {
      in[2 * n] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(2.0 * kPi * hz * n / kEngineHz)));
      in[2 * n + 1] = static_cast<std::int16_t>(std::lround(-10000.0 * std::cos(2.0 * kPi * hz * n / kEngineHz)));
    }
    std::vector<std::int16_t> out;
    std::int16_t block[app::RateConverter::kMaxOutFrames * 2u];
    for (std::uint32_t b = 0; b < kFrames / kBlk; ++b) {
      const std::uint32_t made = c.feed(in.data() + b * kBlk * 2u, kBlk, block);
      CHECK(made <= app::RateConverter::kMaxOutFrames);
      out.insert(out.end(), block, block + made * 2u);
    }
    // The output frame m is the sine at output time m / 44,100 s: the same function sampled at the file's rate. (Two frames of the input are held back, the interpolation looks ahead.)
    const double outer = 44100.0;
    std::int32_t worst = 0;
    const std::size_t frames_out = out.size() / 2u;
    CHECK(frames_out > kFrames / 2u);
    for (std::size_t m = 4; m < frames_out; ++m) {
      const double md = static_cast<double>(m);
      const double l = 20000.0 * std::sin(2.0 * kPi * hz * md / outer);
      const double r = -10000.0 * std::cos(2.0 * kPi * hz * md / outer);
      worst = std::max<std::int32_t>(worst, static_cast<std::int32_t>(std::lround(std::fabs(out[2 * m] - l))));
      worst = std::max<std::int32_t>(worst, static_cast<std::int32_t>(std::lround(std::fabs(out[2 * m + 1] - r))));
    }
    INFO("hz " << hz);
    CHECK(worst <= (hz < 2000.0 ? 2 : 750));  // the cubic: sub-LSB below 2 kHz; at 8 kHz up to 3.7 % of the amplitude (0.3 dB) at the middle of a cycle of the phase (the 17.6 Hz ripple of export_source.h)
    // The count: output m is made once input frame floor(m * ratio) + 2 has come.
    // (The identity copies: every input frame is an output frame, with nothing held back.)
    const std::uint64_t expect = app::RateConverter::identity() ? kFrames : static_cast<std::uint64_t>((static_cast<double>(kFrames) - 3.0) / ratio) + 1u;
    CHECK(frames_out >= expect - 1u);
    CHECK(frames_out <= expect + 1u);
  }
}

TEST_CASE("export: the rate converter is a copy at one rate, passes DC, keeps the channels apart, and counting alone makes the same count") {
  app::RateConverter c;
  c.reset();
  std::int16_t in[haudio::kBlockSamples];
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    in[2 * i] = 5000;
    in[2 * i + 1] = -3000;
  }
  std::int16_t out[app::RateConverter::kMaxOutFrames * 2u];
  std::uint32_t total = 0, counted = 0;
  app::RateConverter d;
  d.reset();
  for (int b = 0; b < 50; ++b) {
    const std::uint32_t made = c.feed(in, kBlk, out);
    counted += d.feed(in, kBlk, nullptr);
    total += made;
    if (b >= 2) {
      for (std::uint32_t i = 0; i < made; ++i) {
        CHECK(out[2 * i] == 5000);
        CHECK(out[2 * i + 1] == -3000);
      }
    }
  }
  CHECK(total == counted);
  if (app::RateConverter::identity()) {
    CHECK(total == 50u * kBlk);
  } else {
    CHECK(total >= 50u * kBlk - 4u);
    CHECK(total <= 50u * kBlk);
  }
  // A full-scale square wave overshoots in a cubic: it saturates, never wraps.
  c.reset();
  std::int16_t sq[haudio::kBlockSamples];
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    sq[2 * i] = (i / 4u) % 2u == 0u ? 32767 : -32768;
    sq[2 * i + 1] = sq[2 * i];
  }
  for (int b = 0; b < 20; ++b) {
    const std::uint32_t made = c.feed(sq, kBlk, out);
    for (std::uint32_t i = 0; i < made * 2u; ++i) {
      CHECK(out[i] >= -32768);
      CHECK(out[i] <= 32767);
    }
  }
}

// ---- The file ------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("export: the file is a canonical stereo 16-bit 44,100 Hz WAV, its length the arrangement plus the tail, its samples the renderer's") {
  Rig rig;
  NEED(rig.begin());
  const unsigned steps = run(rig.job);
  CHECK(steps > 5);
  NEED(rig.job.done());
  CHECK(rig.job.error() == Error::None);
  CHECK(rig.job.progress() == o3os::core::kJobProgressMax);
  CHECK(std::string(rig.job.name()) == "EXPORT SONG0001");
  CHECK(rig.card.violations == 0);
  CHECK_FALSE(rig.card.has(kTmp));
  const Bytes* file = rig.card.get(kFinal);
  NEED(file != nullptr);
  const Wav w = parse_wav(*file);
  NEED(w.ok);
  CHECK(w.format == 1);
  CHECK(w.channels == 2);
  CHECK(w.rate == 44100u);
  CHECK(w.byte_rate == 176400u);
  CHECK(w.align == 4);
  CHECK(w.bits == 16);
  CHECK(w.data_at == 44u);
  CHECK(w.riff_size == file->size() - 8u);
  CHECK(w.data_size == file->size() - 44u);
  const Expected e = expected_for(rig.renderer);
  CHECK(e.frames > 40u * kBlk);
  CHECK(w.data_size == e.frames * 4u);
  // OS-CECHECK K5: the header says 44,100 Hz (above) and the measuring pass's frame count is the file's, so the file lasts frames / 44,100 s.
  CHECK(rig.source.length_frames() == w.data_size / 4u);
  NEED(file->size() == 44u + e.pcm.size());
  CHECK(std::memcmp(file->data() + 44, e.pcm.data(), e.pcm.size()) == 0);
  // The tail is in it: the arrangement is 40 blocks (2,560 frames), the file runs on to the last block above the silence (12 halvings of 12,000 more blocks).
  CHECK(e.frames >= (40u + 10u) * kBlk - 4u);
  CHECK(e.frames <= (40u + 14u) * kBlk);
  // Two passes: the engine stayed with the export between them, and was given back before the card committed anything.
  CHECK(rig.renderer.opens == 2u);
  CHECK(rig.renderer.closes == 1u);
  CHECK_FALSE(rig.renderer.held);
  bool renamed_before = false;
  for (const std::string& l : rig.renderer.log_at_close) {
    renamed_before = renamed_before || l.rfind("rename ", 0) == 0;
  }
  CHECK_FALSE(renamed_before);
  // The measuring pass wrote nothing: the first write is the header.
  const std::string first_write = [&] {
    for (const std::string& l : rig.card.log) {
      if (l.rfind("write ", 0) == 0) {
        return l;
      }
    }
    return std::string();
  }();
  CHECK(first_write == "write 44");
}

TEST_CASE("export: the progress rises, never falls, and ends at the top; the size is known after the measuring pass") {
  Rig rig;
  NEED(rig.begin());
  unsigned last = 0;
  bool known_before = false, known_after = false;
  for (unsigned i = 0; i < 100000; ++i) {
    const o3os::core::JobStatus s = rig.job.step(2000);
    const unsigned p = rig.job.progress();
    CHECK(p >= last);
    last = p;
    if (rig.source.phase() == Phase::Measure) {
      known_before = known_before || rig.source.size_hint() != 0u;
    } else if (rig.source.phase() == Phase::Write) {
      known_after = known_after || rig.source.size_hint() == 44u + rig.source.length_frames() * 4u;
    }
    if (s != o3os::core::JobStatus::Running) {
      break;
    }
  }
  CHECK(rig.job.done());
  CHECK(last == o3os::core::kJobProgressMax);
  CHECK_FALSE(known_before);
  CHECK(known_after);
}

TEST_CASE("export: a tail that never dies is cut 30 s after the arrangement, and a file never exceeds the cap") {
  Rig rig;
  rig.renderer.constant_tail = true;  // amplitude 40 for ever
  NEED(rig.begin());
  run(rig.job);
  NEED(rig.job.done());
  const Wav w = parse_wav(*rig.card.get(kFinal));
  NEED(w.ok);
  const Expected e = expected_for(rig.renderer);
  CHECK(w.data_size == e.frames * 4u);
  // The tail starts with the block that holds the arrangement's end (the 40th, index 39) and is cut when it has lasted kMaxTailBlocks; the engine's frames, through the converter's ratio
  const double in_frames = static_cast<double>(39u + app::ExportSource::kMaxTailBlocks) * kBlk;
  const double out_frames = app::RateConverter::identity() ? in_frames : in_frames * 2499.0 / 2500.0;
  CHECK(static_cast<double>(e.frames) >= out_frames - 8.0);
  CHECK(static_cast<double>(e.frames) <= out_frames + 8.0);
}

// ---- Cancel, a pulled card, and the failures: nothing under the final name ------------------------------------------------------------------------

namespace {

void check_nothing_left(const Rig& rig) {
  CHECK_FALSE(rig.card.has(kFinal));
  CHECK_FALSE(rig.card.has(kTmp));
  CHECK(rig.card.violations == 0);
  CHECK_FALSE(rig.renderer.held);
}

}  // namespace

TEST_CASE("export: a cancel at any step leaves no file and gives the engine back") {
  unsigned total = 0;
  {
    Rig rig;
    NEED(rig.begin());
    total = run(rig.job);
    NEED(total > 20u);
  }
  for (unsigned k = 0; k < total; k += (k < 40u ? 1u : 7u)) {
    Rig rig;
    NEED(rig.begin());
    for (unsigned i = 0; i < k && rig.job.running(); ++i) {
      (void)rig.job.step(2000);
    }
    const bool was_running = rig.job.running();
    rig.job.cancel();
    INFO("cancelled after " << k << " steps");
    if (was_running) {
      CHECK(rig.job.failed());
      CHECK(rig.job.error() == Error::None);
    }
    check_nothing_left(rig);
    CHECK(rig.renderer.closes == (rig.renderer.opens != 0u ? 1u : 0u));  // (a cancel before the first step has not taken the engine)
  }
}

TEST_CASE("export: a card that goes at any write (power cut, pulled) leaves no file under the final name, and the engine is given back") {
  std::size_t ops = 0;
  {
    Rig rig;
    NEED(rig.begin());
    run(rig.job);
    NEED(rig.job.done());
    ops = rig.card.ops;
  }
  NEED(ops > 5u);
  for (std::size_t cut = 0; cut < ops; ++cut) {
    Rig rig;
    rig.card.cut_at = cut;
    NEED(rig.begin());
    run(rig.job);
    INFO("cut at mutating call " << cut);
    CHECK(rig.job.failed());
    CHECK_FALSE(rig.card.has(kFinal));
    CHECK(rig.card.violations == 0);
    CHECK_FALSE(rig.renderer.held);
    // A cut during the rename itself may leave the temp (a later save removes it), but never the final name with a part of a file.
    if (rig.card.has(kTmp)) {
      CHECK(rig.card.dead);
    }
  }
}

TEST_CASE("export: the refusals - a full card, a read-back that differs, a renderer that says no, fails, or never lets the audio go") {
  {
    Rig rig;
    rig.card.space_left = 3000;  // the header and a little more
    NEED(rig.begin());
    run(rig.job);
    CHECK(rig.job.failed());
    CHECK(rig.job.error() == Error::NoSpace);
    check_nothing_left(rig);
  }
  {
    Rig rig;
    rig.card.corrupt_readback = true;
    NEED(rig.begin());
    run(rig.job);
    CHECK(rig.job.failed());
    CHECK(rig.job.error() == Error::Verify);
    check_nothing_left(rig);
    CHECK(rig.renderer.closes == 1u);  // (given back before the read-back, not again after)
  }
  {
    Rig rig;
    rig.renderer.refuse = true;
    NEED(rig.begin());
    run(rig.job);
    CHECK(rig.job.failed());
    CHECK(rig.job.error() == Error::Source);
    CHECK(std::string(rig.job.detail()) == "A NOTE IS HELD");
    check_nothing_left(rig);
    CHECK(rig.renderer.opens == 0u);
  }
  {
    Rig rig;
    rig.renderer.fail_block = 10;  // in the measuring pass
    NEED(rig.begin());
    run(rig.job);
    CHECK(rig.job.failed());
    CHECK(std::string(rig.job.detail()) == "BOOM");
    check_nothing_left(rig);
    CHECK(rig.renderer.closes == 1u);
  }
  {
    Rig rig;
    rig.renderer.never_ready = true;
    NEED(rig.begin());
    const unsigned steps = run(rig.job);
    CHECK(steps != 0u);  // it ends: the wait is bounded
    CHECK(rig.job.failed());
    CHECK(std::string(rig.job.detail()) == "THE AUDIO DID NOT STAND DOWN");
    check_nothing_left(rig);
  }
}

TEST_CASE("export: the staging ring wraps: a long file through a small ring is the same file") {
  Rig rig;
  rig.renderer.active = 600;
  NEED(rig.begin());
  run(rig.job);
  NEED(rig.job.done());
  const Expected e = expected_for(rig.renderer);
  const Bytes* file = rig.card.get(kFinal);
  NEED(file != nullptr);
  NEED(file->size() == 44u + e.pcm.size());
  CHECK(e.pcm.size() > 4u * app::ExportSource::kRingBytes);
  CHECK(std::memcmp(file->data() + 44, e.pcm.data(), e.pcm.size()) == 0);
  // No write was more than a step's budget.
  for (const std::string& l : rig.card.log) {
    if (l.rfind("write ", 0) == 0) {
      CHECK(std::stoul(l.substr(6)) <= app::AtomicSave::kMaxStepBytes);
    }
  }
}

// ---- The names and the service ---------------------------------------------------------------------------------------------------------------------

namespace {

struct Service {
  Service() {
    renderer.card = &card;
    app::ExportSource::Wiring w;
    w.renderer = &renderer;
    w.ring = ring;
    w.ring_bytes = sizeof ring;
    svc.attach(card, card);
    svc.attach_export(w);
    settings.init(o3os::core::ParamKind::Device);
    renderer.active = 10;
    renderer.settle = 0;
  }
  // Starts an export and pumps the runner until the job has ended; returns the start status.
  app::SaveService::Start go(const app::ExportRequest& rq, bool pump = true) {
    const app::SaveService::Start s = svc.save_export(jobs, rq);
    if (s == app::SaveService::Start::Started && pump) {
      for (unsigned i = 0; i < 200000 && jobs.running() != 0u; ++i) {
        (void)jobs.run(clock, 2000);
        clock.advance(2000);
      }
      svc.poll(jobs, settings, 0);
      for (unsigned slot = 0; slot < o3os::core::JobRunner::kSlots; ++slot) {
        if (jobs.state(slot) != o3os::core::SlotState::Free && jobs.state(slot) != o3os::core::SlotState::Running) {
          jobs.release(slot);
        }
      }
    }
    return s;
  }
  MemCard card;
  FakeRenderer renderer;
  alignas(4) std::uint8_t ring[app::ExportSource::kRingBytes];
  app::SaveService svc;
  o3os::core::JobRunner jobs;
  o3os::platform::host::FakeClock clock;
  o3os::model::Settings settings;
};

app::ExportRequest song(const char* loaded) {
  app::ExportRequest r;
  r.what.what = audio::OfflineRequest::What::Song;
  r.loaded_folder = loaded;
  return r;
}
app::ExportRequest pattern(const char* loaded, unsigned id) {
  app::ExportRequest r;
  r.what.what = audio::OfflineRequest::What::Pattern;
  r.what.pattern = static_cast<std::uint16_t>(id);
  r.loaded_folder = loaded;
  return r;
}

}  // namespace

TEST_CASE("export: the files are Export/<project>/<project>_Song0001.wav and _P<n>_0001.wav, the next free number, never over a file") {
  Service s;
  CHECK(s.go(song("Projects/My Song")) == app::SaveService::Start::Started);
  CHECK(s.card.has("Export/My Song/My Song_Song0001.wav"));
  CHECK(std::string(s.svc.last_name()) == "Export/My Song/My Song_Song0001.wav");
  CHECK(s.go(song("Projects/My Song")) == app::SaveService::Start::Started);
  CHECK(s.card.has("Export/My Song/My Song_Song0002.wav"));
  CHECK(s.go(pattern("Projects/My Song", 7)) == app::SaveService::Start::Started);
  CHECK(s.card.has("Export/My Song/My Song_P7_0001.wav"));
  CHECK(s.go(pattern("Projects/My Song", 123)) == app::SaveService::Start::Started);
  CHECK(s.card.has("Export/My Song/My Song_P123_0001.wav"));
  // a hole in the numbers is not filled over a file, and a file of the user's with a number is skipped
  s.card.put("Export/My Song/My Song_Song0003.wav", std::string("mine"));
  CHECK(s.go(song("Projects/My Song")) == app::SaveService::Start::Started);
  CHECK(s.card.has("Export/My Song/My Song_Song0004.wav"));
  CHECK(s.card.same("Export/My Song/My Song_Song0003.wav", Bytes{'m', 'i', 'n', 'e'}));
  // no project: "Untitled"
  CHECK(s.go(song("Workspace")) == app::SaveService::Start::Started);
  CHECK(s.card.has("Export/Untitled/Untitled_Song0001.wav"));
  CHECK(s.go(song("")) == app::SaveService::Start::Started);
  CHECK(s.card.has("Export/Untitled/Untitled_Song0002.wav"));
  // a project name of the longest the guard takes: the project part of the stem is cut so that the stem fits (31)
  const std::string longest(30, 'x');
  CHECK(s.go(song((std::string("Projects/") + longest).c_str())) == app::SaveService::Start::Started);
  CHECK(s.card.has(("Export/" + longest + "/" + std::string(22, 'x') + "_Song0001.wav").c_str()));
  CHECK(s.go(pattern((std::string("Projects/") + longest).c_str(), 255)) == app::SaveService::Start::Started);
  CHECK(s.card.has(("Export/" + longest + "/" + std::string(21, 'x') + "_P255_0001.wav").c_str()));
  CHECK(s.card.violations == 0);
  // only ever new files: nothing removed but temporaries
  for (const std::string& r : s.card.removed()) {
    CHECK(r.find("_o3save.tmp") != std::string::npos);
  }
}

TEST_CASE("export: the service refuses while a save runs, on a card that may not be written, and without a renderer; a cancel leaves the file out") {
  {
    Service s;
    CHECK(s.go(song("Projects/A"), false) == app::SaveService::Start::Started);
    CHECK(s.svc.busy());
    CHECK(s.go(song("Projects/A"), false) == app::SaveService::Start::Busy);
    s.svc.cancel_export(s.jobs);
    for (unsigned slot = 0; slot < o3os::core::JobRunner::kSlots; ++slot) {
      CHECK(s.jobs.state(slot) != o3os::core::SlotState::Running);
      s.jobs.release(slot);
    }
    CHECK_FALSE(s.card.has("Export/A/A_Song0001.wav"));
    CHECK_FALSE(s.renderer.held);
    CHECK_FALSE(s.svc.busy());
  }
  {
    Service s;
    s.card.gate_status = o3os::hal::storage::WriteStatus::WriteProtected;
    CHECK(s.go(song("Projects/A")) == app::SaveService::Start::NotWritable);
    CHECK(s.card.files.empty());
    CHECK(s.renderer.opens == 0u);
  }
  {
    MemCard card;
    app::SaveService svc;
    o3os::core::JobRunner jobs;
    svc.attach(card, card);
    CHECK_FALSE(svc.export_attached());
    CHECK(svc.save_export(jobs, song("Projects/A")) == app::SaveService::Start::NoExport);
  }
  {
    Service s;
    char name[96];
    CHECK(s.svc.export_name(song("Projects/My Song"), name, sizeof name));
    CHECK(std::string(name) == "Export/My Song/My Song_Song0001.wav");
  }
}

// ---- OS-WAVEXP slice B: the files of a set of stems, and the selection's trimmed tail --------------------------------------------------------------------

TEST_CASE("stems: a file after the first has no measuring pass - it is written to the set's length, one render pass, and the bar covers the whole set") {
  Rig rig;
  audio::OfflineRequest mix;
  mix.what = audio::OfflineRequest::What::Song;
  audio::OfflineRequest stem = mix;
  stem.track_mask = 0x0004;
  stem.dry_sends = true;
  constexpr unsigned kUnits = 3;  // the MIX's measuring pass, and two files
  // the first file: measures the MIX, then writes its own request
  NEED(rig.source.set_stem(stem, mix, 0, 0, kUnits));
  NEED(rig.job.begin(rig.card, rig.card, "Export/Track 01", "Track 01_Song_S0001_T03.wav", Mode::NewOnly, rig.source, "T03", "EXPORT "));
  unsigned last = 0;
  bool measured = false;
  for (unsigned i = 0; i < 100000; ++i) {
    const o3os::core::JobStatus s = rig.job.step(2000);
    const int own = rig.source.progress_permille();  // (the job maps it into its own 5..45 %: the source's account is what is checked)
    CHECK(static_cast<unsigned>(own) >= last);
    last = static_cast<unsigned>(own);
    measured = measured || rig.source.phase() == Phase::Write;
    if (rig.source.phase() == Phase::Measure) {
      CHECK(own <= static_cast<int>(1000u / kUnits));  // the measuring pass is one pass of three
    }
    if (rig.source.phase() == Phase::Write) {
      CHECK(own >= static_cast<int>(1000u / kUnits) - 1);
      CHECK(own <= static_cast<int>(2u * 1000u / kUnits) + 1);  // ... and the first file's write is the second
    }
    if (s != o3os::core::JobStatus::Running) {
      break;
    }
  }
  NEED(rig.job.done());
  CHECK(measured);
  CHECK(rig.renderer.opens == 2u);
  CHECK(rig.renderer.request.track_mask == 0x0004);  // the writing pass was the stem's
  const std::uint32_t length = rig.source.length_frames();
  CHECK(length > 40u * kBlk);
  const Bytes* first = rig.card.get("Export/Track 01/Track 01_Song_S0001_T03.wav");
  NEED(first != nullptr);
  CHECK(first->size() == 44u + length * 4u);
  // the second file: the length is known, so it is written at once; one render pass; the bar picks up where the first left off
  audio::OfflineRequest other = mix;
  other.bus_only = true;
  other.buses = 1;
  NEED(rig.source.set_stem(other, mix, length, 2, kUnits));
  NEED(rig.job.begin(rig.card, rig.card, "Export/Track 01", "Track 01_Song_S0001_DELAY.wav", Mode::NewOnly, rig.source, "DELAY", "EXPORT "));
  unsigned last2 = 0;
  for (unsigned i = 0; i < 100000; ++i) {
    const o3os::core::JobStatus s = rig.job.step(2000);
    const int own = rig.source.progress_permille();
    CHECK(static_cast<unsigned>(own) >= last2);
    last2 = static_cast<unsigned>(own);
    CHECK(rig.source.phase() != Phase::Measure);
    if (i == 5) {
      CHECK(rig.source.size_hint() == 44u + length * 4u);  // known from the job's first steps: the card can say it is full before anything is written
    }
    if (rig.source.phase() == Phase::Write) {
      CHECK(own >= static_cast<int>(2u * 1000u / kUnits) - 1);
    }
    if (s != o3os::core::JobStatus::Running) {
      break;
    }
  }
  NEED(rig.job.done());
  CHECK(rig.renderer.opens == 3u);  // one more pass, not two
  CHECK(rig.renderer.request.bus_only);
  const Bytes* second = rig.card.get("Export/Track 01/Track 01_Song_S0001_DELAY.wav");
  NEED(second != nullptr);
  CHECK(second->size() == first->size());  // the same length: a DAW lines them up
  CHECK(rig.card.violations == 0);
  CHECK_FALSE(rig.renderer.held);
}

TEST_CASE("stems: a cancel or a failure in a later file leaves no partial file of it, the earlier files stay, and the engine is given back each time") {
  for (const unsigned cancel_after : {1u, 4u, 9u}) {
    CAPTURE(cancel_after);
    Rig rig;
    audio::OfflineRequest mix;
    mix.what = audio::OfflineRequest::What::Song;
    NEED(rig.source.set_stem(mix, mix, 0, 0, 2));
    NEED(rig.job.begin(rig.card, rig.card, "Export/Track 01", "a_MIX.wav", Mode::NewOnly, rig.source, "MIX", "EXPORT "));
    NEED(run(rig.job) != 0u);
    NEED(rig.job.done());
    const std::uint32_t length = rig.source.length_frames();
    NEED(rig.source.set_stem(mix, mix, length, 2, 3));
    NEED(rig.job.begin(rig.card, rig.card, "Export/Track 01", "b_MIX.wav", Mode::NewOnly, rig.source, "MIX", "EXPORT "));
    for (unsigned i = 0; i < cancel_after && rig.job.step(2000) == o3os::core::JobStatus::Running; ++i) {
    }
    rig.job.cancel();
    CHECK_FALSE(rig.card.has("Export/Track 01/b_MIX.wav"));
    CHECK_FALSE(rig.card.has(kTmp));
    CHECK(rig.card.has("Export/Track 01/a_MIX.wav"));  // the finished one stays
    CHECK(rig.card.violations == 0);
    CHECK_FALSE(rig.renderer.held);
  }
}

TEST_CASE("selection: trim_tail ends the file at the first silent block after the arrangement, with no hold - a later repeat after a gap is not in it, and without trim_tail it is") {
  for (const bool trim : {false, true}) {
    CAPTURE(trim);
    Rig rig;
    rig.renderer.echo_block = 80;  // 29 silent blocks, then a burst (a repeat of a delay)
    audio::OfflineRequest rq;
    rq.what = audio::OfflineRequest::What::Pattern;
    rq.pattern = 7;
    rq.trim_tail = trim;
    NEED(rig.source.set(rq));
    NEED(rig.job.begin(rig.card, rig.card, "Export/Track 01", "Track 01_SEL0001.wav", Mode::NewOnly, rig.source, "SEL0001", "EXPORT "));
    NEED(run(rig.job) != 0u);
    NEED(rig.job.done());
    const std::uint32_t frames = rig.source.length_frames();
    if (trim) {
      CHECK(frames > (40u + 9u) * kBlk);
      CHECK(frames < (40u + 14u) * kBlk);  // the tail's last sound above the silence (block 50) and not a block more
    } else {
      CHECK(frames > 80u * kBlk);  // the repeat is in the file
    }
    CHECK(rig.card.violations == 0);
  }
}
