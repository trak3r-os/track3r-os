// SPDX-License-Identifier: GPL-3.0-or-later
// A frame that does not fit is dropped, never shown truncated: builder -> RasterDisplay -> Simulator -> scenario.
#include <doctest/doctest.h>

#include <o3os/core/action.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/sim/scenario.h>
#include <o3os/model/params.h>
#include <o3os/sim/simulator.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dl = o3os::eve::dl;
using namespace o3os;

namespace {

std::uint32_t pixel(const sim::EveRaster& r, int x, int y) {
  const std::uint8_t* p = r.rgba() + (static_cast<std::size_t>(y) * sim::EveRaster::kWidth + static_cast<std::size_t>(x)) * 4;
  return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
}

// A frame: clear to `rgb`, then `extra` filler words (a CLEAR of the same colour each).
bool send(eve::DisplayListBuilder& b, std::uint8_t red, std::size_t extra) {
  if (!b.begin_frame()) {
    return false;
  }
  b.emit(dl::clear_color_rgb(red, 0, 0));
  b.emit(dl::clear(true, true, true));
  for (std::size_t i = 0; i < extra; ++i) {
    b.emit(dl::clear(true, false, false));
  }
  return b.end_frame();
}

std::vector<sim::ScenarioCommand> parse(const std::string& text) {
  std::vector<sim::ScenarioCommand> cmds;
  std::string error;
  CHECK(sim::parse_scenario(text, cmds, error));
  return cmds;
}

}  // namespace

TEST_CASE("overflow: a tiny builder over a RasterDisplay drops the frame and the old picture stays") {
  sim::EveRaster raster;
  sim::RasterDisplay display(raster);
  std::uint32_t storage[6];  // 5 words + DISPLAY
  eve::DisplayListBuilder builder(display, storage, 6);

  CHECK(send(builder, 0x40, 3));  // 2 + 3 words: fits
  CHECK(display.frames() == 1u);
  CHECK(pixel(raster, 5, 5) == 0x400000);

  CHECK_FALSE(send(builder, 0x80, 4));  // 2 + 4 = 6 words: one too many
  CHECK(display.aborted() == 1u);
  CHECK(display.frames() == 1u);          // not drawn
  CHECK(display.render_failures() == 0u);  // and not a rasteriser problem either
  CHECK(pixel(raster, 5, 5) == 0x400000);  // the previous picture is still the visible one
  CHECK(raster.frames() == 1u);

  CHECK(send(builder, 0x20, 0));  // the next frame is not affected
  CHECK(pixel(raster, 5, 5) == 0x200000);
}

TEST_CASE("overflow: a sink too small for the list refuses it, the builder aborts, the old picture stays") {
  sim::EveRaster raster;
  sim::RasterDisplay display(raster, 5);  // RAM_DL of 5 words
  std::uint32_t storage[64];
  eve::DisplayListBuilder builder(display, storage, 64);
  CHECK(send(builder, 0x40, 2));  // 4 words + DISPLAY = 5: exactly fits
  CHECK(pixel(raster, 5, 5) == 0x400000);
  CHECK_FALSE(send(builder, 0x80, 3));  // 5 + DISPLAY = 6
  CHECK(display.aborted() == 1u);
  CHECK(display.frames() == 1u);
  CHECK(pixel(raster, 5, 5) == 0x400000);
}

TEST_CASE("overflow: a frame the rasteriser cannot execute is counted, keeps its first error and the old picture") {
  sim::EveRaster raster;
  sim::RasterDisplay display(raster);
  const std::uint32_t good[] = {dl::clear_color_rgb(0, 0, 0x30), dl::clear(true, true, true), dl::display()};
  CHECK(display.begin_frame());
  CHECK(display.write(good, 3));
  display.end_frame();
  CHECK(display.frames() == 1u);

  const std::uint32_t alpha_func[] = {0x09000000u, dl::display()};  // ALPHA_FUNC: outside the subset
  const std::uint32_t stencil_op[] = {0x0C000000u, dl::display()};  // STENCIL_OP
  for (const auto* bad : {alpha_func, stencil_op}) {
    CHECK(display.begin_frame());
    CHECK(display.write(bad, 2));
    display.end_frame();
  }
  CHECK(display.render_failures() == 2u);
  CHECK(display.frames() == 1u);
  CHECK(display.first_error().find("0x09000000") != std::string::npos);  // the first one, not the last
  CHECK(pixel(raster, 5, 5) == 0x000030);
}

TEST_CASE("overflow: the simulator reports a frame the app could not build, and does not retry it in a loop") {
  for (const bool tiny_buffer : {true, false}) {
    // Either the app's own frame buffer or the display's list is far smaller than the ~1500-word screen.
    sim::Simulator simulator(tiny_buffer ? 100 : eve::kMaxDisplayListWords,
                             tiny_buffer ? sim::EveRaster::kWidth : 100);
    CHECK(simulator.healthy());
    simulator.step();
    CHECK_FALSE(simulator.healthy());
    CHECK(simulator.failure().find("did not fit") != std::string::npos);
    CHECK(simulator.failure().find("previous picture") != std::string::npos);
    CHECK(simulator.app().failed_frames() == 1u);
    CHECK(simulator.app().frames() == 0u);
    CHECK(simulator.display().frames() == 0u);
    CHECK(simulator.display().render_failures() == 0u);
    CHECK(simulator.raster().frames() == 0u);
    CHECK(pixel(simulator.raster(), 100, 100) == 0);  // nothing was ever shown, in particular nothing truncated

    simulator.wait_ms(20);  // idle passes do not try again: the same content would overflow again
    CHECK(simulator.app().failed_frames() == 1u);
    CHECK_FALSE(simulator.app().dirty());
    simulator.tap(app::Button::Down);  // a change does
    CHECK(simulator.app().failed_frames() == 2u);
    CHECK(simulator.display().aborted() == 2u);  // either way the builder aborted the frame it had begun
  }
}

TEST_CASE("overflow: a raster failure makes the simulator unhealthy with the rasteriser's words") {
  sim::Simulator simulator;
  simulator.step();
  CHECK(simulator.healthy());
  CHECK(simulator.failure().empty());
  const std::uint32_t bad[] = {0x09000000u, dl::display()};  // ALPHA_FUNC: outside the subset
  CHECK(simulator.display().begin_frame());
  CHECK(simulator.display().write(bad, 2));
  simulator.display().end_frame();
  CHECK(simulator.display().render_failures() == 1u);
  CHECK_FALSE(simulator.healthy());
  CHECK(simulator.failure().find("rasteriser: ") == 0);
  CHECK(simulator.failure().find("unsupported") != std::string::npos);
  CHECK(simulator.app().failed_frames() == 0u);
}

TEST_CASE("overflow: run_scenario stops on the first frame that does not fit and takes no snapshot") {
  int snapshots = 0;
  const sim::SnapshotFn count = [&](const std::string&, const sim::Simulator&) {
    ++snapshots;
    return true;
  };

  // The very first frame does not fit.
  {
    sim::Simulator simulator(eve::kMaxDisplayListWords, 100);
    std::string error;
    CHECK_FALSE(sim::run_scenario(parse("snap a\n"), simulator, count, error));
    CHECK(error.find("initial frame: ") == 0);
    CHECK(error.find("did not fit") != std::string::npos);
    CHECK(snapshots == 0);
  }

  // A later frame does not fit: the sink takes exactly the initial frame's words, and extending the selection by a row paints the block (a rectangle a row), which makes the list longer. The grid
  // is drawn from tiles, one word a field, so moving the cursor does not change the size of the list.
  // (OS-INFOLINE: this was F8 (UNDO) with nothing to undo, whose notification replaces the info line; "pads NOTES" now makes that line the longer one. A written note and a dialog over the grid
  // both shorten the list: a note's fields are one word each, an empty instrument or volume two, and a dialog culls what it covers.)
  // (OS-FU3: this was a pad in the Instr column naming instrument 2, one glyph longer than 1; the strip's CPU and MEM now take the room that made the longer name fit.)
  // (OS-METRO: the METRO chip, on by default, is part of the info line that the notification replaces, so with it on the notification is no longer the longer list: the case runs with METRO Off.)
  std::size_t first = 0;
  {
    sim::Simulator probe;
    CHECK(probe.app().device_settings().set(model::device_setting::MetroMode, 0.0f) == model::Status::Ok);
    probe.step();
    first = probe.display().last_frame_words();
    probe.action(core::action_id("select.extend"), 1);
    CHECK(probe.display().last_frame_words() > first);  // the premise of this case
  }
  {
    sim::Simulator simulator(eve::kMaxDisplayListWords, first);
    CHECK(simulator.app().device_settings().set(model::device_setting::MetroMode, 0.0f) == model::Status::Ok);
    std::string error;
    const auto cmds = parse("snap a\naction select.extend 1\nsnap b\n");
    CHECK_FALSE(sim::run_scenario(cmds, simulator, count, error));
    CHECK(snapshots == 1);  // "a" was taken, "b" never
    CHECK(error.find("line ") == 0);
    CHECK(error.find("did not fit") != std::string::npos);
    CHECK(simulator.app().failed_frames() >= 1u);
  }
}
