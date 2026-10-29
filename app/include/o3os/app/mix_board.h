// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The mixer's runtime side (OS-011b, new Community OS behaviour): what the Mixer / Routing / Master FX / Stems pages show and edit that the project model does not store.
//
//   - the engine fields that have no model storage (audio/graph_params.h: the routing matrix, the CUE level, OUT MODE, the effect returns' routes, the INPUT strip's level). The
//     board holds the last value the app asked for; the engine is told by a ParamChange. Since OS-004d they are the project's: the MIXR chunk (app/project_state.h copies them to the project when they
//     change and back when a project is loaded), and the engine's defaults for a project that has none. The model's own parameters (track volume, pan, mute, solo, the master
//     levels) are read from the project and edited as parameters, with undo.
//   - the meters' ballistics (ui/plot/meters.h MeterState, one per measured channel) and the dB scale, fed once a window from the engine's meter snapshot (audio/meters.h);
//   - the stem map and the way to hand it to the USB stream (audio/stem_map.h; the project's USBS chunk, kept by the platform with the USB mode and profile).
//
// It is large for the main loop's SRAM (about 1.4 KB, most of it the 28 MeterStates) and SRAM_U has no room: the platform places it in SDRAM behind the sample arena's other tail
// parts (platform/mk66/src/app_main.cpp), the way OS-011a's SampleBankMem and OS-005g's LiveBox are. The App reads and writes it from the main loop only.

#include <o3os/audio/graph_params.h>
#include <o3os/audio/meters.h>
#include <o3os/app/limiter_rows.h>
#include <o3os/app/send_fx_rows.h>
#include <o3os/audio/stem_map.h>
#include <o3os/ui/plot/meters.h>

#include <cstdint>

namespace o3os::app {

inline constexpr unsigned kMixTracks = 16;           // the graph's track strips (audio::kTracks)
inline constexpr unsigned kMixStrips = 17;           // + the input strip (audio::kInputStrip)
inline constexpr unsigned kRouteCols = 6;            // MAIN, CUE, SNDA, SNDB, REC, USB: column c is bit (1 << c) of audio::route
inline constexpr unsigned kMixDefaultTracks = 12;    // what the pages show when there is no project

static_assert(kMixTracks == audio::kInputStrip && kMixStrips == audio::kInputStrip + 1u, "the board has the graph's strips (the input strip follows the 16 tracks)");
static_assert((1u << (kRouteCols - 1u)) == audio::route::kUsb, "a routing column is a bit of audio::route");

// The columns of the Mixer page after the tracks, in the order they are drawn and the cursor visits them.
enum class MixBus : std::uint8_t {
  In = 0,      // the codec's line input (strip 16): its level is the board's, the engine starts it at 0 (graph.h "monitoring")
  SndA = 1,    // the delay send bus: its level is the master DELAY level, its mute DELAY MUTE
  SndB = 2,    // the reverb send bus: master REVERB, REVERB MUTE
  Cue = 3,     // the CUE bus: its level is the board's
  Master = 4,  // the master: master VOLUME
  Count = 5,
};
inline constexpr unsigned kMixBuses = static_cast<unsigned>(MixBus::Count);

// The names of the routing columns and of the OUT MODE.
inline constexpr const char* kRouteColNames[kRouteCols] = {"MAIN", "CUE", "SNDA", "SNDB", "REC", "USB"};
inline constexpr const char* kOutModeNames[2] = {"STEREO", "MAIN L / CUE R"};

// The route byte after an encoder turn on one column: a positive turn sets the bit, a negative one clears it (explicit, so that a turn never has to be guessed). The input strip has
// no USB bit (graph_params.h): that column is left as it is. Columns past the sixth are ignored.
constexpr std::uint8_t route_turn(std::uint8_t route, unsigned strip, unsigned col, int detents) noexcept {
  if (col >= kRouteCols || detents == 0 || (strip == audio::kInputStrip && col == kRouteCols - 1u)) {
    return route;
  }
  const std::uint8_t bit = static_cast<std::uint8_t>(1u << col);
  return static_cast<std::uint8_t>(detents > 0 ? (route | bit) : (route & ~bit)) & audio::route::kAll;
}

// A return's route (bit 0 MAIN, bit 1 CUE: graph_params.h RetRouteA / B), the same way.
constexpr std::uint8_t return_turn(std::uint8_t route, unsigned col, int detents) noexcept {
  if (col >= 2u || detents == 0) {
    return route;
  }
  const std::uint8_t bit = static_cast<std::uint8_t>(1u << col);
  return static_cast<std::uint8_t>((detents > 0 ? (route | bit) : (route & ~bit)) & 3u);
}

class TrackSink;  // app/track_state.h

struct MixBoard {
  // What the platform wires (null in a bare test: the pages then show no meters and say the stems are not there).
  const audio::MeterBank* meters = nullptr;  // the engine's meters (audio::Graph::meters())
  audio::StemMap* stems = nullptr;           // the stem map of the USB stream (usb0::stem_map())
  void (*apply_stems)() noexcept = nullptr;  // hands the map to the stem tap (usb0::apply_stem_map)
  // OS-USBOUTS: asks the USB driver for another number of outputs (usb0::request_layout with the profile; the platform's hook refuses while a change or a project's USB apply is in flight). False: refused
  // for now (busy, DISK, driver down): the page keeps its pending value. The driver resets the stem map to the new profile's factory slots when it attaches. Null in a bare test.
  bool (*request_outputs)(usb::Profile profile) noexcept = nullptr;
  TrackSink* tracks = nullptr;               // the sequencer's per-track side (OS-MUTE: deferred mutes, note counters); null: mutes are immediate, no activity

  // OS-MUTE: each track's MUTE MODE (app::MuteMode, 0 Immediate). Since OS-PERSIST2 the project's (the MUTE chunk: App::sync_project_state copies a change into it, a load puts it back; a project
  // without one sets Immediate).
  std::uint8_t mute_mode[kMixTracks] = {};

  // OS-SENDFX: the send effects' controls of the MASTER FX page (send_fx_rows.h kSendFxTable, in its order), as the app last asked for them; the table's defaults at start (the effects' own).
  // Since OS-PERSIST2 the project's (the SNFX chunk; a load puts them back and tells the engine what changed: project_state.h send_fx_apply).
  std::uint16_t fx[kSendFxRows];
  // OS-LIMIT: the master limiter's THRESHOLD, ATTACK and RELEASE (limiter_rows.h kLimiterTable, in its order): the session's, not stored (the project has no field for them), the table's defaults at start.
  std::uint16_t lim[kLimiterRows];
  // The control a row of the MASTER FX page edits after the CUE LEVEL, by its index among them (kMasterFxControls): the send effects' first, then the limiter's.
  std::uint16_t& control(unsigned index) noexcept { return index < kSendFxRows ? fx[index] : lim[index - kSendFxRows]; }
  std::uint16_t control(unsigned index) const noexcept { return index < kSendFxRows ? fx[index] : lim[index - kSendFxRows]; }

  // The engine fields, as the app last asked for them; the engine's own defaults at start.
  std::uint8_t route[kMixStrips];  // audio::route bits of each strip (the input's has no USB bit)
  std::uint8_t ret_route[2];       // where the SNDA / SNDB effect's return goes: bit 0 MAIN, bit 1 CUE
  std::uint8_t out_mode;           // audio::OutMode
  std::uint8_t cue_level;          // 0..100
  std::uint8_t in_level;           // 0..100

  // The meters: ballistics per measured channel (audio::meter_ch) and the scale they are drawn on.
  ui::plot::DbScale scale;
  ui::plot::MeterState meter[audio::kMeterChannels];
  std::uint32_t windows = 0;  // the engine's meter windows at the last poll

  MixBoard() noexcept { init(); }

  // The engine's defaults: every strip into MAIN, the returns into MAIN, STEREO, CUE at 100, the input at 0.
  void init() noexcept;

  // The Pattern screen's track-header meter (OS-PVU): the bar of track `track` (0..15) on the dB scale as 0..255, its RMS level and its held peak (the tick, never below the level).
  // 0, 0 for a track the board has not got.
  struct Bar {
    std::uint8_t level = 0;
    std::uint8_t peak = 0;
  };
  Bar track_bar(unsigned track) const noexcept;

  // Reads the engine's meters (a lock-free snapshot) and runs the ballistics once per new window. True when a bar, a hold tick or a clip lamp moved (the screen must
  // be redrawn); false when nothing is attached, no new window has come, or the picture is where it was. Main loop only.
  bool poll() noexcept;

  // Hands the stem map to the stream. False without a map or a hook.
  bool commit_stems() const noexcept {
    if (stems == nullptr || apply_stems == nullptr) {
      return false;
    }
    apply_stems();
    return true;
  }
};

}  // namespace o3os::app
