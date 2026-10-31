// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SCREENSHOT: the ScreenshotService (usb/screenshot_service.h) against a fake chip and the host's DirCard (a scratch directory whose write side applies the real path guard): the trigger after
// the release and the frame, every refusal, the BMP bytes, the next-free-name rule and its pace, the aborts and the platform's cancel() that remove the temporary file, the pace of a capture (one
// piece a pass, the frames held), the console's pclk0 flag, and the input that is dropped while the frames are held (usb/input_hold.h, spec Q4).
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include <o3os/hal/screenshot_file.h>
#include <o3os/platform/host/dir_card.h>
#include <o3os/platform/mk66/usb/input_hold.h>
#include <o3os/platform/mk66/usb/screenshot_service.h>

#include <o3os/platform/mk66/display/ft812_regs.h>

#include "mk66_shot_region.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace o3os::platform::mk66::console;
namespace shot = o3os::hal::storage::shot;
namespace host = o3os::platform::host;

namespace {

constexpr std::uint32_t kRegionAddr = o3os::platform::mk66::test::kShotRegion;
constexpr std::uint32_t kRegionBytes = o3os::platform::mk66::test::kShotRegionBytes;  // kRamGShotBytes (checked against the map by plot_ram_g_test): the spare region less the DMA self-test's 64 bytes

// The picture the chip "shows": a byte that depends on its place in the image, so a misplaced or doubled piece is seen.
std::uint8_t pixel_byte(std::uint32_t image_offset) { return static_cast<std::uint8_t>((image_offset * 7u + (image_offset >> 8)) & 0xFFu); }

class FakeChip final : public ShotPort {
 public:
  std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(kRegionBytes, 0xAA);
  bool is_ready = true;
  bool bus = true;
  std::uint32_t reinit = 0;
  std::uint32_t frame_count = 0;
  std::uint32_t clock = 0;
  unsigned polls_to_done = 2;
  unsigned captures = 0;
  unsigned aborts = 0;
  unsigned reads = 0;
  std::size_t max_read = 0;
  std::vector<std::uint32_t> frames_at_capture;
  unsigned pending_polls = 0;
  bool queued = false;
  bool pclk0 = false;  // the console's `shot ... pclk0` flag as the chip port keeps it
  unsigned clock_calls = 0;

  bool ready() noexcept override { return is_ready; }
  bool bus_free() noexcept override { return bus; }
  bool start_capture(std::uint32_t format, std::uint32_t addr, std::uint32_t y, std::uint32_t width_param, std::uint32_t rows) noexcept override {
    CHECK(format == o3os::platform::mk66::display::ft812::kFmtRgb565);
    CHECK(width_param == 800u);
    CHECK(addr == kRegionAddr);
    for (std::uint32_t i = 0; i < rows * 1600u; ++i) {
      ram[addr - kRegionAddr + i] = pixel_byte(y * 1600u + i);
    }
    ++captures;
    frames_at_capture.push_back(frame_count);
    pending_polls = polls_to_done;
    queued = true;
    return true;
  }
  Capture poll_capture() noexcept override {
    if (pending_polls != 0u) {
      --pending_polls;
      return Capture::Pending;
    }
    queued = false;
    return Capture::Done;
  }
  void abort_capture() noexcept override {
    ++aborts;
    queued = false;
  }
  std::uint32_t reinits() noexcept override { return reinit; }
  void read(std::uint32_t addr, std::uint8_t* out, std::size_t n) noexcept override {
    ++reads;
    max_read = std::max(max_read, n);
    std::memcpy(out, ram.data() + (addr - kRegionAddr), n);
  }
  std::uint32_t frames() noexcept override { return frame_count; }
  std::uint32_t now_ms() noexcept override { return clock; }
  void stop_clock(bool on) noexcept override {
    pclk0 = on;
    ++clock_calls;
  }
};

// The key scan, as a script: what the test pushes is what the app's input would hold.
class ScriptedInput final : public o3os::hal::Input {
 public:
  std::deque<o3os::core::InputEvent> queue;
  bool poll(o3os::core::InputEvent& out) noexcept override {
    if (queue.empty()) {
      return false;
    }
    out = queue.front();
    queue.pop_front();
    return true;
  }
  void pad(std::uint8_t id, bool down) { queue.push_back(o3os::core::make_pad_event(++t, id, down ? 127 : 0)); }
  void button(std::uint8_t id, bool down) { queue.push_back(o3os::core::make_button_event(++t, id, down)); }
  void turn(std::int16_t delta) { queue.push_back(o3os::core::make_encoder_event(++t, 0, delta)); }
  o3os::core::Tick t = 0;
};

class FakeHost final : public ScreenshotHost {
 public:
  shot::Refusal card = shot::Refusal::None;
  bool console = false;
  std::vector<std::string> strip;
  std::vector<bool> warned;
  shot::Refusal card_refusal() noexcept override { return card; }
  bool console_shot_active() noexcept override { return console; }
  void notice(const char* text, bool warn) noexcept override {
    strip.emplace_back(text);
    warned.push_back(warn);
  }
};

struct Rig {
  Rig() : scratch("shotsvc"), card(scratch.path()) {
    svc.attach(hostx, card, card, buf, kRegionAddr, kRegionBytes);
    NEED(scratch.ok());
    NEED(card.mount() == o3os::hal::storage::Status::Ok);
  }
  host::ScratchDir scratch;
  host::DirCard card;
  FakeChip chip;
  FakeHost hostx;
  alignas(4) std::uint8_t buf[ScreenshotService::kBufferBytes];
  ScreenshotService svc;
  ScriptedInput input;
  InputHold hold{input};
  std::vector<o3os::core::InputEvent> app_events;  // what the fake app was given
  bool keys = false;
  unsigned passes = 0;
  unsigned frozen_passes = 0;
  unsigned max_reads_in_pass = 0;
  bool app_frames = true;  // the fake app commits a frame a pass unless the service holds them

  // One pass of the main loop: the clock moves a millisecond, the service steps, the app draws unless it is held.
  bool pass() {
    ++chip.clock;
    const unsigned reads_before = chip.reads;
    const bool frozen = svc.step(chip.clock, keys, chip);
    max_reads_in_pass = std::max(max_reads_in_pass, chip.reads - reads_before);
    hold.pass(frozen);  // as pass() in app_main.cpp: the input is held with the frames
    if (frozen) {
      ++frozen_passes;
    } else {
      if (app_frames) {
        ++chip.frame_count;
      }
      o3os::core::InputEvent e;
      while (hold.poll(e)) {
        app_events.push_back(e);
      }
    }
    ++passes;
    return frozen;
  }
  bool run_until_idle(unsigned max = 20000) {
    for (unsigned i = 0; i < max && svc.active(); ++i) {
      pass();
    }
    return !svc.active();
  }
  // The press, a hold of the keys for 100 passes, the release.
  bool press_and_release() {
    if (!svc.request(chip.clock)) {
      return false;
    }
    keys = true;
    for (int i = 0; i < 100; ++i) {
      pass();
    }
    keys = false;
    return true;
  }
  std::string path(const char* name) const { return scratch.path() + "/Screenshots/" + name; }
  std::vector<std::uint8_t> file(const char* name) const {
    std::ifstream f(path(name), std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }
  bool exists(const char* name) const { return std::ifstream(path(name), std::ios::binary).good(); }
  void put(const char* name, const char* content) {
    std::filesystem::create_directories(scratch.path() + "/Screenshots");
    std::ofstream(path(name), std::ios::binary) << content;
  }
};

void expect_image(const std::vector<std::uint8_t>& f) {
  NEED(f.size() == shot::kFileBytes);
  std::uint8_t header[shot::kHeaderBytes];
  shot::bmp_header(header);
  CHECK(std::memcmp(f.data(), header, sizeof header) == 0);
  bool same = true;
  for (std::uint32_t i = 0; i < shot::kPixelBytes && same; ++i) {
    same = f[shot::kHeaderBytes + i] == pixel_byte(i);
  }
  CHECK(same);
}

}  // namespace

TEST_CASE("screenshot service: a press, the release and a frame later the picture is saved as SHOT_0001.bmp, and the strip says so") {
  Rig r;
  CHECK_FALSE(r.exists("SHOT_0001.bmp"));
  NEED(r.press_and_release());
  CHECK(r.chip.captures == 0u);  // nothing is taken while the keys are down
  CHECK(r.svc.active());
  NEED(r.run_until_idle());
  CHECK(r.svc.saved() == 1u);
  expect_image(r.file("SHOT_0001.bmp"));
  NEED(r.hostx.strip.size() == 1u);
  CHECK(r.hostx.strip[0] == "SCREENSHOT SAVED: SHOT_0001.BMP");
  CHECK_FALSE(r.hostx.warned[0]);
  CHECK_FALSE(r.exists("_o3save.tmp"));  // the temporary file became the picture
  CHECK(r.card.violations() == 0u);
  CHECK(r.chip.captures == 2u);  // two bands of 240 rows
  CHECK(r.chip.aborts == 0u);
}

TEST_CASE("screenshot service: the next shot is 0002, a free number is taken in order and nothing is overwritten") {
  Rig r;
  r.put("SHOT_0001.bmp", "first");
  r.put("SHOT_0002.bmp", "second");
  r.put("SHOT_0004.bmp", "fourth");
  NEED(r.press_and_release());
  NEED(r.run_until_idle());
  expect_image(r.file("SHOT_0003.bmp"));
  NEED(r.press_and_release());
  NEED(r.run_until_idle());
  expect_image(r.file("SHOT_0005.bmp"));
  CHECK(r.file("SHOT_0001.bmp").size() == 5u);
  CHECK(r.file("SHOT_0004.bmp").size() == 6u);
  CHECK(r.hostx.strip.back() == "SCREENSHOT SAVED: SHOT_0005.BMP");
  CHECK(r.svc.saved() == 2u);
}

TEST_CASE("screenshot service: the picture is taken after the release and after the first frame the app commits, and the app's frames are held for the capture") {
  Rig r;
  r.chip.polls_to_done = 3;
  NEED(r.svc.request(r.chip.clock));
  r.keys = true;
  for (int i = 0; i < 50; ++i) {
    CHECK_FALSE(r.pass());
  }
  r.keys = false;
  const std::uint32_t release_frames = r.chip.frame_count;
  unsigned guard_passes = 0;
  while (r.chip.captures == 0u && guard_passes++ < 1000u) {
    r.pass();
  }
  NEED(r.chip.captures == 1u);
  CHECK(r.chip.frame_count > release_frames);  // at least one frame was drawn after the release
  NEED(r.run_until_idle());
  NEED(r.chip.frames_at_capture.size() == 2u);
  CHECK(r.chip.frames_at_capture[0] == r.chip.frames_at_capture[1]);  // both bands from one display list: no frame between them
  CHECK(r.frozen_passes > 370u);                                      // 376 pieces (the header rides on the first), one a pass
  CHECK(r.chip.max_read <= ScreenshotService::kBufferBytes);
  CHECK(r.max_reads_in_pass <= 1u);  // at most one piece a pass
  expect_image(r.file("SHOT_0001.bmp"));
}

TEST_CASE("screenshot service: an app that draws nothing is waited for kFrameWaitMs, then the picture is taken") {
  Rig r;
  r.app_frames = false;
  NEED(r.press_and_release());
  const std::uint32_t released_at = r.chip.clock;
  unsigned guard_passes = 0;
  while (r.chip.captures == 0u && guard_passes++ < 2000u) {
    r.pass();
  }
  NEED(r.chip.captures == 1u);
  CHECK(r.chip.clock - released_at >= ScreenshotService::kFrameWaitMs + ScreenshotService::kSettleMs - 2u);
  NEED(r.run_until_idle());
  expect_image(r.file("SHOT_0001.bmp"));
}

TEST_CASE("screenshot service: a key press during the capture does not stop it") {
  Rig r;
  NEED(r.press_and_release());
  unsigned guard_passes = 0;
  while (r.chip.captures == 0u && guard_passes++ < 1000u) {
    r.pass();
  }
  r.keys = true;  // Shift or Power pressed again in the middle
  NEED(r.run_until_idle());
  CHECK(r.svc.saved() == 1u);
  expect_image(r.file("SHOT_0001.bmp"));
}

TEST_CASE("screenshot service: every refusal says why, takes nothing and leaves the service idle") {
  struct Row {
    shot::Refusal card;
    bool console;
    const char* text;
  };
  const Row rows[] = {
      {shot::Refusal::NoCard, false, "NO SCREENSHOT: NO CARD"},
      {shot::Refusal::OnComputer, false, "NO SCREENSHOT: CARD ON THE COMPUTER"},
      {shot::Refusal::Unwritable, false, "NO SCREENSHOT: CARD NOT WRITABLE"},
      {shot::Refusal::Busy, false, "NO SCREENSHOT: CARD BUSY"},
      {shot::Refusal::None, true, "NO SCREENSHOT: CONSOLE SHOT RUNNING"},
  };
  for (const Row& row : rows) {
    INFO(row.text);
    Rig r;
    r.hostx.card = row.card;
    r.hostx.console = row.console;
    CHECK_FALSE(r.svc.request(r.chip.clock));
    CHECK_FALSE(r.svc.active());
    NEED(r.hostx.strip.size() == 1u);
    CHECK(r.hostx.strip[0] == row.text);
    CHECK(r.hostx.warned[0]);
    for (int i = 0; i < 2000; ++i) {
      r.pass();
    }
    CHECK(r.chip.captures == 0u);
    CHECK_FALSE(r.exists("SHOT_0001.bmp"));
    CHECK(r.svc.refused() == 1u);
  }
}

TEST_CASE("screenshot service: a card that went busy or away between the press and the picture is refused then") {
  Rig r;
  NEED(r.press_and_release());
  r.hostx.card = shot::Refusal::Busy;  // a save job started meanwhile
  NEED(r.run_until_idle());
  NEED(r.hostx.strip.size() == 1u);
  CHECK(r.hostx.strip[0] == "NO SCREENSHOT: CARD BUSY");
  CHECK(r.chip.captures == 0u);
  CHECK_FALSE(r.exists("SHOT_0001.bmp"));
  r.hostx.card = shot::Refusal::None;
  NEED(r.press_and_release());
  r.hostx.console = true;  // a console `shot` started meanwhile
  NEED(r.run_until_idle());
  CHECK(r.hostx.strip.back() == "NO SCREENSHOT: CONSOLE SHOT RUNNING");
  CHECK(r.chip.captures == 0u);
}

TEST_CASE("screenshot service: a second press while one is going is refused and the first goes on") {
  Rig r;
  NEED(r.press_and_release());
  CHECK_FALSE(r.svc.request(r.chip.clock));
  NEED(r.hostx.strip.size() == 1u);
  CHECK(r.hostx.strip[0] == "NO SCREENSHOT: ONE IS BEING SAVED");
  NEED(r.run_until_idle());
  expect_image(r.file("SHOT_0001.bmp"));
  CHECK(r.hostx.strip.back() == "SCREENSHOT SAVED: SHOT_0001.BMP");
}

TEST_CASE("screenshot service: 9999 files in the folder is NO SCREENSHOT: FOLDER FULL, and a gap is found after the last number") {
  {
    Rig r;
    char name[shot::kNameCap];
    r.put("SHOT_0001.bmp", "x");
    for (unsigned n = 1; n <= shot::kLastNumber; ++n) {
      NEED(shot::file_name(name, n));
      std::ofstream(r.path(name), std::ios::binary) << "x";
    }
    NEED(r.press_and_release());
    NEED(r.run_until_idle());
    NEED(r.hostx.strip.size() == 1u);
    CHECK(r.hostx.strip[0] == "NO SCREENSHOT: FOLDER FULL");
    CHECK(r.chip.captures == 0u);
  }
  {
    Rig r;
    char name[shot::kNameCap];
    r.put("SHOT_0001.bmp", "x");
    for (unsigned n = 3; n <= shot::kLastNumber; ++n) {  // 2 is the one free number, below the search's start after 9999 is reached
      NEED(shot::file_name(name, n));
      std::ofstream(r.path(name), std::ios::binary) << "x";
    }
    NEED(r.press_and_release());
    NEED(r.run_until_idle());
    expect_image(r.file("SHOT_0002.bmp"));
  }
}

TEST_CASE("screenshot service: a stale temporary file of an earlier capture is replaced") {
  Rig r;
  r.put("_o3save.tmp", "left over from a pulled card");
  NEED(r.press_and_release());
  NEED(r.run_until_idle());
  expect_image(r.file("SHOT_0001.bmp"));
  CHECK_FALSE(r.exists("_o3save.tmp"));
}

TEST_CASE("screenshot service: a card taken away in the middle fails the capture and no SHOT file exists") {
  Rig r;
  NEED(r.press_and_release());
  unsigned guard_passes = 0;
  while (r.svc.active() && r.chip.reads < 100u && guard_passes++ < 5000u) {
    r.pass();
  }
  NEED(r.svc.active());
  r.card.set_present(false);
  NEED(r.run_until_idle());
  CHECK(r.svc.failed() == 1u);
  CHECK(r.svc.saved() == 0u);
  CHECK(r.hostx.strip.back() == "SCREENSHOT FAILED");
  CHECK(r.hostx.warned.back());
  CHECK_FALSE(r.exists("SHOT_0001.bmp"));
  // the card comes back: the next shot starts clean (the torn temporary file is replaced) and is 0001
  r.card.set_present(true);
  NEED(r.card.mount() == o3os::hal::storage::Status::Ok);
  NEED(r.press_and_release());
  NEED(r.run_until_idle());
  expect_image(r.file("SHOT_0001.bmp"));
}

TEST_CASE("screenshot service: a display that was set up again fails the capture and removes the partial file") {
  Rig r;
  NEED(r.press_and_release());
  unsigned guard_passes = 0;
  while (r.chip.reads < 50u && guard_passes++ < 5000u) {
    r.pass();
  }
  ++r.chip.reinit;  // the driver set the chip up again: its RAM_G is gone
  NEED(r.run_until_idle());
  CHECK(r.hostx.strip.back() == "SCREENSHOT FAILED");
  CHECK_FALSE(r.exists("SHOT_0001.bmp"));
  CHECK_FALSE(r.exists("_o3save.tmp"));  // the card was there: the partial file is deleted
  CHECK(r.svc.failed() == 1u);
}

TEST_CASE("screenshot service: a chip that never gets a free bus stalls out after kStallTimeoutMs, the snapshot is dropped and the file removed") {
  Rig r;
  NEED(r.press_and_release());
  unsigned guard_passes = 0;
  while (r.chip.captures == 0u && guard_passes++ < 1000u) {
    r.pass();
  }
  r.chip.bus = false;
  const std::uint32_t from = r.chip.clock;
  NEED(r.run_until_idle());
  CHECK(r.chip.clock - from >= BandReader::kStallTimeoutMs);
  CHECK(r.chip.clock - from < BandReader::kStallTimeoutMs + 50u);
  CHECK(r.hostx.strip.back() == "SCREENSHOT FAILED");
  CHECK_FALSE(r.exists("_o3save.tmp"));
  CHECK_FALSE(r.exists("SHOT_0001.bmp"));
}

TEST_CASE("screenshot service: keys that never come up lapse the request without a word, and keys pressed again before the picture wait for the next release") {
  {
    Rig r;
    NEED(r.svc.request(r.chip.clock));
    r.keys = true;
    for (unsigned i = 0; i < ScreenshotService::kKeysMaxMs + 10u; ++i) {
      r.pass();
    }
    CHECK_FALSE(r.svc.active());
    CHECK(r.hostx.strip.empty());
    CHECK(r.chip.captures == 0u);
  }
  {
    Rig r;
    NEED(r.svc.request(r.chip.clock));
    r.pass();  // released at once
    r.keys = true;  // and down again within the settling time
    for (int i = 0; i < 20; ++i) {
      r.pass();
    }
    CHECK(r.chip.captures == 0u);
    r.keys = false;
    NEED(r.run_until_idle());
    expect_image(r.file("SHOT_0001.bmp"));
  }
}

TEST_CASE("screenshot service: a pclk0 the console left set is cleared when the capture starts, and nothing before it touches the port's clock") {
  Rig r;
  r.chip.pclk0 = true;  // `shot 565 pclk0` ran earlier
  NEED(r.press_and_release());
  CHECK(r.chip.clock_calls == 0u);
  CHECK(r.chip.pclk0);
  unsigned guard_passes = 0;
  while (r.chip.captures == 0u && guard_passes++ < 1000u) {
    r.pass();
  }
  NEED(r.chip.captures == 1u);
  CHECK(r.chip.clock_calls == 1u);
  CHECK_FALSE(r.chip.pclk0);
  NEED(r.run_until_idle());
  CHECK(r.chip.clock_calls == 1u);  // once a capture, not once a pass
  expect_image(r.file("SHOT_0001.bmp"));
}

TEST_CASE("screenshot service: cancel() in the middle of a capture removes the temporary file, releases the card, says nothing and leaves the next shot clean") {
  Rig r;
  NEED(r.press_and_release());
  unsigned guard_passes = 0;
  while (r.chip.reads < 50u && guard_passes++ < 5000u) {
    r.pass();
  }
  NEED(r.svc.capturing());
  r.svc.cancel(r.chip);
  CHECK_FALSE(r.svc.active());
  CHECK_FALSE(r.svc.capturing());  // the card's claim is let go
  CHECK_FALSE(r.exists("_o3save.tmp"));
  CHECK_FALSE(r.exists("SHOT_0001.bmp"));
  CHECK(r.svc.failed() == 1u);
  CHECK(r.svc.saved() == 0u);
  CHECK(r.hostx.strip.empty());
  CHECK(r.card.violations() == 0u);
  const unsigned captures = r.chip.captures;
  for (int i = 0; i < 200; ++i) {
    CHECK_FALSE(r.pass());  // nothing goes on, the frames are not held
  }
  CHECK(r.chip.captures == captures);
  NEED(r.press_and_release());
  NEED(r.run_until_idle());
  expect_image(r.file("SHOT_0001.bmp"));
  CHECK(r.svc.saved() == 1u);
}

TEST_CASE("screenshot service: cancel() while a band is queued drops the snapshot in the coprocessor") {
  Rig r;
  r.chip.polls_to_done = 50;
  NEED(r.press_and_release());
  unsigned guard_passes = 0;
  while (r.chip.captures == 0u && guard_passes++ < 1000u) {
    r.pass();
  }
  NEED(r.chip.queued);
  r.svc.cancel(r.chip);
  CHECK(r.chip.aborts == 1u);
  CHECK_FALSE(r.chip.queued);
  CHECK_FALSE(r.svc.active());
  CHECK_FALSE(r.exists("_o3save.tmp"));
}

TEST_CASE("screenshot service: cancel() of an armed request, with nothing started, makes no file and no word; on an idle service it does nothing") {
  {
    Rig r;
    NEED(r.svc.request(r.chip.clock));
    r.keys = true;
    for (int i = 0; i < 10; ++i) {
      r.pass();
    }
    r.svc.cancel(r.chip);
    CHECK_FALSE(r.svc.active());
    r.keys = false;
    for (int i = 0; i < 1000; ++i) {
      r.pass();
    }
    CHECK(r.chip.captures == 0u);
    CHECK(r.hostx.strip.empty());
    CHECK(r.svc.failed() == 0u);
    CHECK_FALSE(r.exists("SHOT_0001.bmp"));
  }
  {
    Rig r;
    r.svc.cancel(r.chip);
    CHECK(r.svc.failed() == 0u);
    CHECK_FALSE(r.svc.active());
  }
  {
    ScreenshotService unattached;  // (the card's write side is not built)
    FakeChip chip;
    unattached.cancel(chip);
    CHECK_FALSE(unattached.active());
  }
}

TEST_CASE("screenshot service: the name search makes at most two lookups a pass, and a search that was cut off resumes where it was") {
  static_assert(ScreenshotService::kNamesPerPass <= 2u, "a lookup walks the folder on the card: two a pass at most");
  constexpr unsigned kTaken = 400;
  const auto fill = [](Rig& r) {
    char name[shot::kNameCap];
    for (unsigned n = 1; n <= kTaken; ++n) {
      NEED(shot::file_name(name, n));
      r.put(name, "x");
    }
  };
  const auto passes_to_capture = [](Rig& r, unsigned limit) {
    unsigned n = 0;
    while (r.chip.captures == 0u && n < limit) {
      r.pass();
      ++n;
    }
    return n;
  };
  {
    Rig r;
    fill(r);
    NEED(r.press_and_release());
    const unsigned first = passes_to_capture(r, 2000u);
    CHECK(first >= (kTaken + 1u) / ScreenshotService::kNamesPerPass - 3u);  // 401 lookups at two a pass
    CHECK(first < 2000u);
    NEED(r.run_until_idle());
    expect_image(r.file("SHOT_0401.bmp"));
  }
  {
    Rig r;
    fill(r);
    NEED(r.press_and_release());
    for (int i = 0; i < 100; ++i) {  // about 200 names looked at, then the search is cut off
      r.pass();
    }
    NEED(r.chip.captures == 0u);
    r.svc.cancel(r.chip);
    NEED(r.press_and_release());
    const unsigned second = passes_to_capture(r, 2000u);
    CHECK(second < 150u);  // it began after the 200 names it had seen, not at 1
    NEED(r.run_until_idle());
    expect_image(r.file("SHOT_0401.bmp"));
    CHECK(r.svc.saved() == 1u);
  }
}

TEST_CASE("screenshot service: input during the capture is dropped, the release of a key that was down before is delivered after it") {
  Rig r;
  r.input.pad(12, true);  // a pad is held when the screenshot is asked for
  r.pass();
  NEED(r.app_events.size() == 1u);
  NEED(r.press_and_release());
  unsigned guard_passes = 0;
  while (!r.svc.capturing() && guard_passes++ < 1000u) {
    r.pass();
  }
  NEED(r.svc.capturing());
  r.app_events.clear();
  r.input.button(8, true);  // a soft key, a turn, another pad, all during the freeze
  r.input.turn(2);
  r.input.pad(30, true);
  r.input.pad(12, false);  // the held pad is let go
  r.input.button(8, false);
  r.pass();
  CHECK(r.input.queue.empty());  // read dry while held
  CHECK(r.app_events.empty());   // the frozen pass gives the app nothing
  r.input.pad(30, false);        // let go after the capture is done
  NEED(r.run_until_idle());
  r.pass();
  CHECK(r.svc.saved() == 1u);
  NEED(r.app_events.size() == 1u);  // only the held pad's release
  CHECK(r.app_events[0].kind == o3os::core::InputKind::Pad);
  CHECK(r.app_events[0].id == 12u);
  CHECK(r.app_events[0].value == 0);
}
