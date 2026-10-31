// SPDX-License-Identifier: GPL-3.0-or-later
//
// `diag shot` (OS-HIL, usb/shot_stream.h) against a fake chip: the bands and the commands the coprocessor is given, the framed base64 lines with their CRCs and
// offsets, that the whole picture comes back exactly once and in order, the per-pass bound on what is read over the SPI, the back-pressure (a host that does not
// read, a bus that is busy: nothing is read, nothing is dropped), and every way it ends in an ERR line. Runs on the host and on QEMU's Cortex-M4F (`qemu_mk66`).
#include <doctest/doctest.h>

#include <o3os/platform/mk66/display/ft812_regs.h>
#include <o3os/platform/mk66/usb/shot_stream.h>
#include <o3os/usb/diag_shell.h>

#include "mk66_shot_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace console = o3os::platform::mk66::console;
namespace ft = o3os::platform::mk66::display::ft812;
namespace sh = o3os::usb::diag;

// This tree builds without exceptions, so doctest's REQUIRE is unavailable: MUST checks and leaves the test case on failure.
#define MUST(cond)                \
  do {                            \
    const bool must_ok_ = (cond); \
    CHECK(must_ok_);              \
    if (!must_ok_) {              \
      return;                     \
    }                             \
  } while (0)

namespace {

constexpr std::uint32_t kRegion = o3os::platform::mk66::test::kShotRegion;
constexpr std::uint32_t kRegionBytes = o3os::platform::mk66::test::kShotRegionBytes;  // kRamGShotBytes (checked against the map by plot_ram_g_test): the spare region less the DMA self-test's 64 bytes (platform/mk66/src/app_main.cpp shot_region)

// The picture the "chip" has: a byte is a function of its offset in the image, so any misplaced line shows.
std::uint8_t image_byte(std::uint32_t offset) { return static_cast<std::uint8_t>((offset * 131u + (offset >> 8) * 7u + 3u) & 0xFFu); }

struct Cap {
  std::uint32_t format, addr, y, w, rows;
};

class FakePort final : public console::ShotPort {
 public:
  bool is_ready = true;
  bool bus = true;
  bool refuse_command = false;
  unsigned polls_until_done = 2;  // Pending answers per capture
  bool never_done = false;
  bool fault = false;
  std::uint32_t row_bytes = 1600;  // what the test's picture has per row
  std::uint32_t clock = 1000;
  std::uint32_t frame_count = 5;
  std::vector<Cap> captures;
  unsigned reads_this_pass = 0;
  unsigned max_reads_per_pass = 0;
  std::size_t bytes_this_pass = 0;
  std::size_t max_bytes_per_pass = 0;
  unsigned reads = 0;
  bool read_while_busy = false;
  bool in_flight = false;
  unsigned pending_polls = 0;
  std::uint32_t band_row0 = 0;
  unsigned aborts = 0;            // abort_capture() calls
  std::uint32_t reinit_count = 0;  // what the display driver says it has re-initialised

  bool ready() noexcept override { return is_ready; }
  bool bus_free() noexcept override { return bus; }
  bool start_capture(std::uint32_t format, std::uint32_t addr, std::uint32_t y, std::uint32_t w, std::uint32_t rows) noexcept override {
    if (refuse_command) {
      return false;
    }
    captures.push_back(Cap{format, addr, y, w, rows});
    in_flight = true;
    pending_polls = polls_until_done;
    band_row0 = y;
    return true;
  }
  Capture poll_capture() noexcept override {
    if (fault) {
      return Capture::Fault;
    }
    if (never_done) {
      return Capture::Pending;
    }
    if (pending_polls > 0) {
      --pending_polls;
      return Capture::Pending;
    }
    in_flight = false;
    return Capture::Done;
  }
  void abort_capture() noexcept override {
    ++aborts;
    in_flight = false;
  }
  std::uint32_t reinits() noexcept override { return reinit_count; }
  void read(std::uint32_t addr, std::uint8_t* out, std::size_t n) noexcept override {
    if (!bus) {
      read_while_busy = true;
    }
    ++reads;
    ++reads_this_pass;
    bytes_this_pass += n;
    // the band's bytes are in RAM_G at kRegion, the picture's at row band_row0 on
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = image_byte(band_row0 * row_bytes + (addr - kRegion) + static_cast<std::uint32_t>(i));
    }
  }
  std::uint32_t frames() noexcept override { return frame_count; }
  std::uint32_t now_ms() noexcept override { return clock; }
  void new_pass() {
    max_reads_per_pass = reads_this_pass > max_reads_per_pass ? reads_this_pass : max_reads_per_pass;
    max_bytes_per_pass = bytes_this_pass > max_bytes_per_pass ? bytes_this_pass : max_bytes_per_pass;
    reads_this_pass = 0;
    bytes_this_pass = 0;
  }

};

std::uint32_t crc32_of(const std::vector<std::uint8_t>& data) {  // an independent bitwise CRC-32 (zlib's), not the firmware's table
  std::uint32_t c = 0xFFFFFFFFu;
  for (const std::uint8_t b : data) {
    c ^= b;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) != 0u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
  }
  return ~c;
}

bool base64_decode(const std::string& text, std::vector<std::uint8_t>& out) {
  out.clear();
  if (text.size() % 4u != 0u) {
    return false;
  }
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const int a = value(text[i]);
    const int b = value(text[i + 1]);
    const bool pad2 = text[i + 2] == '=';
    const bool pad3 = text[i + 3] == '=';
    const int c = pad2 ? 0 : value(text[i + 2]);
    const int d = pad3 ? 0 : value(text[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad3) || ((pad2 || pad3) && i + 4 != text.size())) {
      return false;
    }
    const std::uint32_t v = (static_cast<std::uint32_t>(a) << 18) | (static_cast<std::uint32_t>(b) << 12) | (static_cast<std::uint32_t>(c) << 6) | static_cast<std::uint32_t>(d);
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    if (!pad2) {
      out.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    if (!pad3) {
      out.push_back(static_cast<std::uint8_t>(v));
    }
  }
  return true;
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> v;
  std::size_t at = 0;
  while (at <= s.size()) {
    const std::size_t end = s.find(' ', at);
    v.push_back(s.substr(at, end == std::string::npos ? std::string::npos : end - at));
    if (end == std::string::npos) {
      break;
    }
    at = end + 1;
  }
  return v;
}

// The sink. The picture is 768,000 bytes and 8000 lines of text: nothing of it is kept (the QEMU target has 4 MiB for everything); every data line is checked as it
// arrives, against the picture the fake chip has and against an independent CRC-32.
struct Text {
  std::size_t room = 1u << 20;  // bytes the sink says it can take
  std::string partial;
  std::vector<std::string> others;  // BEGIN, END, ERR: every line that is not a data line
  std::size_t total = 0;            // bytes written
  unsigned data_lines = 0;
  unsigned short_lines = 0;
  unsigned bad_lines = 0;           // wrong shape, CRC, offset order or bytes
  std::uint32_t expect_offset = 0;
  std::uint32_t image_crc = 0xFFFFFFFFu;  // running, bitwise, not the firmware's table
  bool non_ascii = false;
  std::size_t longest = 0;

  static void write(void* c, const char* d, std::size_t n) { static_cast<Text*>(c)->take(d, n); }
  static void flush(void*) {}
  static std::size_t get_room(void* c) { return static_cast<Text*>(c)->room; }
  sh::Output output() { return sh::Output(sh::Sink{&write, &flush, this, &get_room}); }

  void take(const char* d, std::size_t n) {
    total += n;
    for (std::size_t i = 0; i < n; ++i) {
      const char c = d[i];
      if (!((c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n')) {
        non_ascii = true;
      }
      if (c == '\n') {
        if (!partial.empty() && partial.back() == '\r') {
          partial.pop_back();
        }
        line(partial);
        partial.clear();
      } else {
        partial += c;
      }
    }
  }

  void line(const std::string& l) {
    longest = l.size() + 2u > longest ? l.size() + 2u : longest;
    if (l.compare(0, 7, "SHOT D ") != 0) {
      others.push_back(l);
      return;
    }
    const std::vector<std::string> f = split(l);
    std::vector<std::uint8_t> raw;
    if (f.size() != 5u || f[2].size() != 6u || !base64_decode(f[3], raw) || f[4].size() != 8u || std::stoul(f[4], nullptr, 16) != crc32_of(raw)) {
      ++bad_lines;
      return;
    }
    const std::uint32_t offset = static_cast<std::uint32_t>(std::stoul(f[2], nullptr, 16));
    bool good = offset == expect_offset && !raw.empty() && raw.size() <= console::ShotStream::kLineBytes && l.size() + 2u <= console::ShotStream::kLineTextMax;
    for (std::size_t k = 0; k < raw.size() && good; ++k) {
      good = raw[k] == image_byte(offset + static_cast<std::uint32_t>(k));
    }
    if (!good) {
      ++bad_lines;
      return;
    }
    for (const std::uint8_t b : raw) {
      image_crc ^= b;
      for (int k = 0; k < 8; ++k) {
        image_crc = (image_crc & 1u) != 0u ? (image_crc >> 1) ^ 0xEDB88320u : image_crc >> 1;
      }
    }
    short_lines += raw.size() < console::ShotStream::kLineBytes ? 1u : 0u;
    expect_offset += static_cast<std::uint32_t>(raw.size());
    ++data_lines;
  }
  std::uint32_t crc() const { return ~image_crc; }
  const std::string& last() const { return others.empty() ? partial : others.back(); }
};

// Runs the stream to its end against the port, one step per "pass", with the pass counter bounded.
unsigned run(console::ShotStream& s, Text& t, FakePort& p, unsigned max_passes = 100000) {
  sh::Output out = t.output();
  unsigned passes = 0;
  while (passes < max_passes) {
    ++passes;
    p.clock += 1;
    const bool more = s.step(out, p);
    p.new_pass();
    if (!more) {
      break;
    }
  }
  return passes;
}

console::ShotParams rows(std::uint32_t height) {
  console::ShotParams p;
  p.height = height;
  return p;
}

}  // namespace

TEST_CASE("shot: the whole picture comes back once, in order, in lines of 96 bytes with their CRCs") {
  console::ShotStream s;
  FakePort p;
  Text t;
  CHECK(s.start(console::ShotParams{}, kRegion, kRegionBytes));
  CHECK(s.active());
  run(s, t, p);
  CHECK_FALSE(s.active());
  MUST(t.others.size() == 2u);
  CHECK(t.others.front() == "SHOT BEGIN fmt=565 w=800 h=480 bytes=768000 line=96");
  // two bands of 240 rows, taken in the order the picture goes, into the region, in RGB565, with the width as asked
  MUST(p.captures.size() == 2u);
  CHECK(p.captures[0].format == ft::kFmtRgb565);
  CHECK(p.captures[0].addr == kRegion);
  CHECK(p.captures[0].y == 0u);
  CHECK(p.captures[0].rows == 240u);
  CHECK(p.captures[0].w == 800u);
  CHECK(p.captures[1].y == 240u);
  CHECK(p.captures[1].rows == 240u);
  // every data line: ascending absolute offsets covering 0..768000 exactly, base64 of the right bytes, the line's own CRC (checked as they came)
  CHECK(t.bad_lines == 0u);
  CHECK(t.expect_offset == 768000u);
  CHECK(t.data_lines == 8000u);  // 240 rows * 1600 is a multiple of 96: no short line
  CHECK(t.short_lines == 0u);
  CHECK_FALSE(t.non_ascii);
  CHECK(t.longest <= console::ShotStream::kLineTextMax);
  // the end line: the line count, the CRC of everything, and that no frame moved meanwhile
  char end[64];
  std::snprintf(end, sizeof end, "SHOT END lines=8000 crc=%08X moved=0", static_cast<unsigned>(t.crc()));
  CHECK(t.others.back() == end);
}

TEST_CASE("shot: no more than kLinesPerPass lines (384 bytes) are read over the SPI in a pass") {
  console::ShotStream s;
  FakePort p;
  Text t;
  MUST(s.start(console::ShotParams{}, kRegion, kRegionBytes));
  const unsigned passes = run(s, t, p);
  CHECK(p.max_reads_per_pass <= console::ShotStream::kLinesPerPass);
  CHECK(p.max_bytes_per_pass <= console::ShotStream::kLinesPerPass * console::ShotStream::kLineBytes);
  CHECK(p.max_bytes_per_pass == 384u);  // and it does use the whole budget when it can
  CHECK(passes < 2400u);                // 8000 lines at 4 a pass, plus the commands and the waits: a few seconds of passes at 1 kHz
  CHECK_FALSE(p.read_while_busy);
  CHECK(t.bad_lines == 0u);
}

TEST_CASE("shot: the default w is the chip's: 800 for RGB565, 1600 for ARGB8 (OS-SIMKEY)") {
  // The chip counts w in 16-bit units: ARGB8 with w = 800 wrote 1600 bytes a row, the left half of the screen (a 0.4.8 unit), where 1600 covers the same rows.
  CHECK(console::default_width_param(console::ShotFormat::Rgb565) == 800u);
  CHECK(console::default_width_param(console::ShotFormat::Argb8) == 1600u);
  CHECK(console::kArgb8WidthParam == 2u * console::ShotStream::kWidth);
  CHECK(console::default_width_param(console::ShotFormat::Rgb565) == console::ShotParams{}.width_param);
}

TEST_CASE("shot: ARGB8 with its default w is seven bands of 69 rows") {
  console::ShotStream s;
  FakePort p;
  p.row_bytes = 3200;
  Text t;
  console::ShotParams params;
  params.format = console::ShotFormat::Argb8;
  params.width_param = console::default_width_param(params.format);
  MUST(s.start(params, kRegion, kRegionBytes));
  run(s, t, p);
  MUST(t.others.size() == 2u);
  CHECK(t.others.front() == "SHOT BEGIN fmt=8888 w=800 h=480 bytes=1536000 line=96");
  // The region holds kRamGShotBytes / 6400 rows of what the chip may write (1600 pixels of 4 bytes): 76, so 7 bands of 69 rows (the last one 66).
  static_assert(kRegionBytes / 6400u == 76u, "the region holds 76 rows at the default w; the bands below follow");
  MUST(p.captures.size() == 7u);
  for (unsigned b = 0; b < 7; ++b) {
    CHECK(p.captures[b].format == ft::kFmtArgb8);
    CHECK(p.captures[b].y == 69u * b);
    CHECK(p.captures[b].rows == (b < 6u ? 69u : 66u));
    CHECK(p.captures[b].w == 1600u);  // what the chip is given: the picture's row is still 800 pixels
    CHECK(p.captures[b].addr == kRegion);
  }
  // 69 rows * 3200 = 220800 bytes a band, 2300 lines of 96 (the last band 66 rows, 2200 lines): no short line; the offsets run on across the bands
  CHECK(t.bad_lines == 0u);
  CHECK(t.expect_offset == 1536000u);
  CHECK(t.short_lines == 0u);
  CHECK(t.others.back().find("moved=0") != std::string::npos);
}

TEST_CASE("shot: a frame committed meanwhile is said at the end") {
  console::ShotStream s;
  FakePort p;
  Text t;
  MUST(s.start(rows(48), kRegion, kRegionBytes));
  sh::Output out = t.output();
  for (unsigned i = 0; i < 20; ++i) {
    p.clock += 1;
    (void)s.step(out, p);
  }
  p.frame_count += 1;
  while (s.step(out, p)) {
    p.clock += 1;
  }
  CHECK(t.last().find("moved=1") != std::string::npos);
  CHECK(t.bad_lines == 0u);
}

TEST_CASE("shot: nothing is read while the bus is busy, and nothing is dropped while the output has no room") {
  console::ShotStream s;
  FakePort p;
  Text t;
  MUST(s.start(rows(96), kRegion, kRegionBytes));
  sh::Output out = t.output();
  p.clock += 1;
  CHECK(s.step(out, p));  // BEGIN
  const std::size_t after_begin = t.total;
  // a frame transfer owns the SPI: the command is not given, nothing is read, time passes
  p.bus = false;
  for (unsigned i = 0; i < 50; ++i) {
    p.clock += 1;
    CHECK(s.step(out, p));
  }
  CHECK(p.captures.empty());
  CHECK(p.reads == 0u);
  p.bus = true;
  for (unsigned i = 0; i < 6; ++i) {  // the command, the waits, then lines
    p.clock += 1;
    CHECK(s.step(out, p));
  }
  CHECK(p.captures.size() == 1u);
  CHECK(p.reads > 0u);
  // the host stops reading: the sink says it has less room than a line needs; the reads stop and the output does not grow
  t.room = console::ShotStream::kLineTextMax - 1u;
  const unsigned reads = p.reads;
  const std::size_t size = t.total;
  for (unsigned i = 0; i < 200; ++i) {
    p.clock += 1;
    CHECK(s.step(out, p));
  }
  CHECK(p.reads == reads);
  CHECK(t.total == size);
  CHECK(size > after_begin);
  // it reads again as soon as there is room, and the picture is whole
  t.room = 1u << 20;
  while (s.step(out, p)) {
    p.clock += 1;
  }
  CHECK(t.last().rfind("SHOT END lines=1600", 0) == 0u);  // 96 rows * 1600 bytes / 96
  CHECK(t.bad_lines == 0u);
}

TEST_CASE("shot: it ends in one ERR line when the host never reads, the coprocessor never answers, faults or takes no command, or the display is down") {
  {  // a host that does not read for kStallTimeoutMs
    console::ShotStream s;
    FakePort p;
    Text t;
    MUST(s.start(rows(48), kRegion, kRegionBytes));
    sh::Output out = t.output();
    t.room = 0;
    bool running = true;
    unsigned passes = 0;
    while (running && passes < 10000) {
      p.clock += 1;
      running = s.step(out, p);
      ++passes;
    }
    CHECK_FALSE(running);
    CHECK_FALSE(s.active());
    CHECK(passes >= console::ShotStream::kStallTimeoutMs);
    CHECK(passes < console::ShotStream::kStallTimeoutMs + 50u);
    CHECK(p.captures.empty());
    CHECK(t.last().rfind("SHOT ERR stalled", 0) == 0u);
  }
  {  // a coprocessor that never finishes the snapshot
    console::ShotStream s;
    FakePort p;
    p.never_done = true;
    Text t;
    MUST(s.start(rows(48), kRegion, kRegionBytes));
    run(s, t, p);
    CHECK(t.last() == "SHOT ERR snapshot timeout");
    CHECK(p.reads == 0u);
    CHECK(p.captures.size() == 1u);
    CHECK(p.aborts == 1u);  // the stuck command is dropped: the coprocessor is reset
  }
  {  // a coprocessor fault (the port has already recovered it)
    console::ShotStream s;
    FakePort p;
    p.fault = true;
    Text t;
    MUST(s.start(rows(48), kRegion, kRegionBytes));
    run(s, t, p);
    CHECK(t.last() == "SHOT ERR coprocessor fault");
    CHECK(p.aborts == 0u);  // (the port recovered it itself)
  }
  {  // the command FIFO takes nothing
    console::ShotStream s;
    FakePort p;
    p.refuse_command = true;
    Text t;
    MUST(s.start(rows(48), kRegion, kRegionBytes));
    run(s, t, p);
    CHECK(t.last() == "SHOT ERR the coprocessor took no command");
  }
  {  // the display is not running: nothing is printed but the error, nothing touched
    console::ShotStream s;
    FakePort p;
    p.is_ready = false;
    Text t;
    MUST(s.start(rows(48), kRegion, kRegionBytes));
    run(s, t, p);
    CHECK(t.others.size() == 1u);
    CHECK(t.last() == "SHOT ERR display not running");
    CHECK(p.captures.empty());
  }
}

TEST_CASE("shot: parameters that cannot work are refused up front, cancel stops it and a new start begins afresh") {
  console::ShotStream s;
  console::ShotParams params;
  CHECK_FALSE(s.start(params, kRegion, 1599u));  // not even a row
  CHECK_FALSE(s.start(params, kRegion + 2u, kRegionBytes));  // the region must be on a word
  params.width_param = 0;
  CHECK_FALSE(s.start(params, kRegion, kRegionBytes));
  params.width_param = 4096;
  CHECK_FALSE(s.start(params, kRegion, kRegionBytes));
  params.width_param = 800;
  params.height = 0;
  CHECK_FALSE(s.start(params, kRegion, kRegionBytes));
  params.height = 481;
  CHECK_FALSE(s.start(params, kRegion, kRegionBytes));
  CHECK_FALSE(s.active());
  params.height = 48;
  Text t;
  FakePort p;
  MUST(s.start(params, kRegion, kRegionBytes));
  sh::Output out = t.output();
  for (unsigned i = 0; i < 20; ++i) {
    p.clock += 1;
    (void)s.step(out, p);
  }
  s.cancel(p);
  CHECK_FALSE(s.active());
  CHECK(p.aborts == 0u);  // cancelled between bands (no command in flight): nothing to drop
  const std::size_t size = t.total;
  CHECK_FALSE(s.step(out, p));
  CHECK(t.total == size);
  // a second shot starts from the first line and the first band
  Text t2;
  FakePort p2;
  MUST(s.start(params, kRegion, kRegionBytes));
  run(s, t2, p2);
  CHECK(t2.others.front() == "SHOT BEGIN fmt=565 w=800 h=48 bytes=76800 line=96");
  CHECK(p2.captures.size() == 1u);
  CHECK(p2.captures[0].y == 0u);
  CHECK(p2.captures[0].rows == 48u);
  CHECK(t2.last().rfind("SHOT END lines=800", 0) == 0u);
  CHECK(t2.bad_lines == 0u);
  // a small region makes more, shorter bands; one that holds a row at least still works
  console::ShotStream small;
  FakePort p3;
  Text t3;
  console::ShotParams sp = rows(480);
  MUST(small.start(sp, kRegion, 100u * 1600u));
  run(small, t3, p3);
  CHECK(p3.captures.size() == 5u);  // 480 rows in bands of 96
  CHECK(p3.captures[0].rows == 96u);
  CHECK(p3.captures[4].y == 384u);
  CHECK(t3.last().rfind("SHOT END lines=8000", 0) == 0u);
  CHECK(t3.bad_lines == 0u);
  // a band that is not a multiple of the line length ends in a short line, and the offsets still run on
  console::ShotStream odd;
  FakePort p4;
  Text t4;
  MUST(odd.start(rows(7), kRegion, kRegionBytes));  // 7 * 1600 = 11200 bytes = 116 lines of 96 and one of 64
  run(odd, t4, p4);
  CHECK(t4.data_lines == 117u);
  CHECK(t4.short_lines == 1u);
  CHECK(t4.expect_offset == 11200u);
  CHECK(t4.bad_lines == 0u);
  CHECK(t4.last().rfind("SHOT END lines=117", 0) == 0u);
}

TEST_CASE("shot: the bands are sized by what the chip may write, so a large w never overruns the region") {
  struct Case {
    console::ShotFormat format;
    std::uint32_t w, row_bytes, stride, rows, bands;
  };
  // region 487,360 bytes (kRamGShotBytes): stride = max(800 px, w px) * bytes per pixel; rows = ceil(480 / ceil(480 / (region / stride)))
  static_assert(kRegionBytes == 487360u, "the cases below follow the region's size");
  const Case cases[] = {
      {console::ShotFormat::Rgb565, 800, 1600, 1600, 240, 2},  {console::ShotFormat::Rgb565, 400, 1600, 1600, 240, 2},  // a narrower w writes no more than a full row
      {console::ShotFormat::Rgb565, 1600, 1600, 3200, 120, 4},  {console::ShotFormat::Rgb565, 4095, 1600, 8190, 54, 9},
      {console::ShotFormat::Argb8, 800, 3200, 3200, 120, 4},   {console::ShotFormat::Argb8, 1600, 3200, 6400, 69, 7},
      {console::ShotFormat::Argb8, 4095, 3200, 16380, 29, 17},
  };
  for (const Case& c : cases) {
    INFO("format " << (c.format == console::ShotFormat::Argb8 ? "8888" : "565") << " w " << c.w);
    console::ShotStream s;
    FakePort p;
    p.row_bytes = c.row_bytes;
    Text t;
    console::ShotParams params;
    params.format = c.format;
    params.width_param = c.w;
    MUST(s.start(params, kRegion, kRegionBytes));
    run(s, t, p);
    MUST(p.captures.size() == c.bands);
    std::uint32_t rows_done = 0;
    for (const Cap& cap : p.captures) {
      CHECK(cap.w == c.w);
      CHECK(cap.y == rows_done);
      CHECK(cap.rows <= c.rows);
      CHECK(cap.rows * c.stride <= kRegionBytes);                         // what the chip may write fits the region ...
      CHECK(cap.addr + cap.rows * c.stride <= kRegion + kRegionBytes);    // ... and so does the end address, which is inside RAM_G
      rows_done += cap.rows;
    }
    CHECK(rows_done == 480u);
    CHECK(p.captures[0].rows == c.rows);
    CHECK(t.bad_lines == 0u);
    CHECK(t.expect_offset == 480u * c.row_bytes);  // what is read back is the screen's own row, whatever w was
  }
  // a w whose single row does not fit the region is refused up front
  console::ShotStream s;
  console::ShotParams wide;
  wide.width_param = 4095;
  CHECK_FALSE(s.start(wide, kRegion, 8189u));  // one row of 4095 RGB565 pixels is 8190 bytes
  CHECK(s.start(wide, kRegion, 8190u));        // ... and with exactly that, 480 bands of one row
  CHECK_FALSE(s.start(console::ShotParams{}, kRegion, 1599u));
  wide.format = console::ShotFormat::Argb8;
  CHECK_FALSE(s.start(wide, kRegion, 16379u));
  // the region must lie inside RAM_G
  CHECK_FALSE(s.start(console::ShotParams{}, ft::kRamGBytes - 1600u, 3200u));
  CHECK_FALSE(s.start(console::ShotParams{}, ft::kRamGBytes + 4u, 1600u));
  CHECK(s.start(console::ShotParams{}, ft::kRamGBytes - 1600u * 8u, 1600u * 8u));
}

TEST_CASE("shot: a display that is set up again or stops mid-shot ends it, and a queued snapshot is dropped") {
  {  // re-initialised while a band is being taken
    console::ShotStream s;
    FakePort p;
    p.never_done = true;
    Text t;
    MUST(s.start(rows(48), kRegion, kRegionBytes));
    sh::Output out = t.output();
    for (unsigned i = 0; i < 5; ++i) {
      p.clock += 1;
      CHECK(s.step(out, p));
    }
    CHECK(p.captures.size() == 1u);
    p.reinit_count += 1;
    p.clock += 1;
    CHECK_FALSE(s.step(out, p));
    CHECK(t.last() == "SHOT ERR display re-initialised: the picture is lost");
    CHECK(p.aborts == 1u);
    CHECK_FALSE(s.active());
  }
  {  // re-initialised while lines are being read: nothing more is read, no command to drop
    console::ShotStream s;
    FakePort p;
    Text t;
    MUST(s.start(rows(96), kRegion, kRegionBytes));
    sh::Output out = t.output();
    for (unsigned i = 0; i < 12; ++i) {
      p.clock += 1;
      CHECK(s.step(out, p));
    }
    CHECK(p.reads > 0u);
    const unsigned reads = p.reads;
    p.reinit_count += 2;
    p.clock += 1;
    CHECK_FALSE(s.step(out, p));
    CHECK(p.reads == reads);
    CHECK(t.last().rfind("SHOT ERR display re-initialised", 0) == 0u);
    CHECK(p.aborts == 0u);
  }
  {  // the display gave up
    console::ShotStream s;
    FakePort p;
    p.never_done = true;
    Text t;
    MUST(s.start(rows(48), kRegion, kRegionBytes));
    sh::Output out = t.output();
    for (unsigned i = 0; i < 4; ++i) {
      p.clock += 1;
      CHECK(s.step(out, p));
    }
    p.is_ready = false;
    p.clock += 1;
    CHECK_FALSE(s.step(out, p));
    CHECK(t.last() == "SHOT ERR display stopped");
    CHECK(p.aborts == 1u);
  }
  {  // a re-initialisation that happened BEFORE the shot is not this shot's
    console::ShotStream s;
    FakePort p;
    p.reinit_count = 3;
    Text t;
    MUST(s.start(rows(48), kRegion, kRegionBytes));
    run(s, t, p);
    CHECK(t.last().rfind("SHOT END lines=800", 0) == 0u);
  }
  {  // a key press while a snapshot is queued drops it; between bands there is nothing to drop
    console::ShotStream s;
    FakePort p;
    p.never_done = true;
    Text t;
    MUST(s.start(rows(48), kRegion, kRegionBytes));
    sh::Output out = t.output();
    for (unsigned i = 0; i < 4; ++i) {
      p.clock += 1;
      CHECK(s.step(out, p));
    }
    s.cancel(p);
    CHECK(p.aborts == 1u);
    CHECK_FALSE(s.active());
    s.cancel(p);  // idle: nothing more
    CHECK(p.aborts == 1u);
  }
}

TEST_CASE("shot: base64 is RFC 4648 with padding") {
  const struct {
    const char* in;
    const char* out;
  } vectors[] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
  for (const auto& v : vectors) {
    char buf[16] = {};
    const std::size_t n = console::ShotStream::base64(reinterpret_cast<const std::uint8_t*>(v.in), std::strlen(v.in), buf);
    CHECK(std::string(buf, n) == v.out);
  }
  const std::uint8_t bytes[] = {0xFB, 0xFF, 0xBF};
  char buf[8];
  CHECK(std::string(buf, console::ShotStream::base64(bytes, 3, buf)) == "+/+/");
}
