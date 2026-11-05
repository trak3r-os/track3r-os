// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/core/param.h>
#include <o3os/core/power_hold.h>
#include <o3os/eve/dl.h>
#include <o3os/hal/storage.h>
#include <o3os/platform/host/fake_firmware.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/host/slow_card.h>
#include <o3os/sim/audio_check.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/theme.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace o3os::model {
class Project;
}
namespace o3os::midi {
class CcInput;
}

namespace o3os::sim {

struct SimDocument;  // a project, its undo journal and editor, and the fake audio side of the ParamChange ring (simulator.cpp)
class SimProjectSink;  // OS-NEWKEEP: the File screen's NEW over the simulator's document, for `document new` (simulator.cpp)
class SimAudio;  // OS-E2E-2: the real engine, its sequencer and the captures of `render` (sim_audio.h)
class SimApps;  // OS-APPUX S0: the shipped apps as host objects over the card (sim_apps.h)
struct SimShot;  // OS-SCREENSHOT: the screenshot service over the simulator's frame buffer and card (simulator.cpp)
struct SimCard;  // OS-E2E-1b: the card of `card <dir>`: a scratch copy of a fixture, the sample memory and bank, and the project loader (simulator.cpp, sim_project.h)

// The application wired to the host fakes: scripted input, a clock that only moves when the simulator moves
// it, and the display-list rasteriser. There is no wall-clock anywhere, so a run is a pure function of the
// commands given to it.
//
// Time advances in fixed ticks of kTickUs; each tick is one pass of the app's main loop. An input command
// delivers its event stamped with the current time and runs one pass, so successive events are one tick apart.
class Simulator final : private platform::host::CardClock {
 public:
  static constexpr std::uint32_t kTickUs = 1000;

  // `app_dl_words` is the frame buffer the app builds its screen in and `sink_words` the capacity of the display
  // list the display accepts; both default to the chip's 2048 words. Smaller values exist for tests of what
  // happens when a frame does not fit.
  explicit Simulator(std::size_t app_dl_words = eve::kMaxDisplayListWords,
                     std::size_t sink_words = platform::host::FakeDisplay::kDefaultCapacityWords);
  ~Simulator();
  Simulator(const Simulator&) = delete;
  Simulator& operator=(const Simulator&) = delete;

  void step();                     // one tick: advance the clock, run one pass of the app
  void wait_ms(std::uint32_t ms);  // ms / 1 ms ticks
  void press(app::Button button);
  void release(app::Button button);
  void tap(app::Button button);    // press, one tick, release, one tick
  void pad(unsigned index);        // pad hit at full velocity and release, one tick each
  void encoder(int delta);         // one event of `delta` detents
  void set_theme(ui::ThemeId id);  // the colour theme, then one tick (the screen is redrawn)
  // Runs an action by id, as a script would (no gesture, no binding), then one tick. The result says whether it
  // ran; Unknown and Disabled are for the caller to report.
  app::DispatchResult action(core::ActionId id, core::ActionArg arg);

  // OS-POWER P1 (scenario command `power down|up|hold <ms>`): the Power switch, as the platform's pin read feeds core::PowerHold once a pass (active low, ready from the start: the hold arms after
  // 100 ms of release, so a scenario waits before the first `power down`). The line and the strip go to the app (App::set_power); at the commit the shutdown begins as far as the app goes
  // (App::power_begin: no input is read, the strip says POWERING OFF), the platform's stages are not simulated. Then one tick.
  void power_switch(bool down);
  bool powering_off() const { return power_committed_; }
  // OS-SCREENSHOT (scenario: `press shift`, `power down`, `release shift`, `power up`): Shift then Power saves the screen as Screenshots/SHOT_nnnn.bmp on the card of `card <dir>`, with the platform's
  // ScreenshotService (platform/mk66/usb/screenshot_service.h) over a ShotPort that reads this simulator's frame buffer in RGB565, as the FT812's snapshot would (without its colour artefact).
  // The picture is the first frame after both keys are released; the app draws no frame while it is written; the strip says SCREENSHOT SAVED: SHOT_0001.BMP, or NO SCREENSHOT: NO CARD.
  bool screenshot_running() const;
  // Scenario `power unsaved`: attaches a save layer with no card (SaveService::dirty is what the app asks) and changes the open document so that it differs from the one the save layer took as its baseline. A no-op without a document.
  void mark_unsaved();

  // The sequencer's position, as the engine publishes it (audio/play_position.h; scenario command `playhead`): playing the pattern in view at `step`, `ustep` of its 48 microsteps
  // played (or stopped), then one tick. The app reads it as the device does, once at the top of every pass. `count_in` (OS-METRO): the steps of the count-in still to run, the running one included (0: none).
  // `wait` (OS-METROALIGN, scenario `playhead ... wait <step|bar|pattern>`; 0 none): a take armed on this running transport waits for that alignment (RecTap::Align as a number): the simulator gives the app a
  // recorder with no engine behind it that says so, for the strip's countdown; 0 takes it away again.
  void playhead(bool playing, unsigned step, unsigned ustep = 0, unsigned count_in = 0, unsigned wait = 0);

  // Opens a document, as the platform will once projects are loaded (scenario command `document`): a project of the demo data
  // (sim/demo_project.h; `with_bindings` adds two MIDI Learn bindings), its undo journal and editor, attached to the app, and a fake audio
  // consumer on the ParamChange ring that the simulator drains at every tick. Replaces a document that is open.
  // `with_new` (OS-NEWKEEP, `document new`) also attaches a project sink that can do File > NEW (ALL or KEEP INSTRUMENTS) on this document, at once; it has no card, so OPEN does nothing.
  // `tracks` (OS-ZOOM, `document tracks <n>`): the project has n active tracks, 1..16; 0 keeps the demo's twelve.
  // `tb303` (OS-E2E-9, `document tb303`): fill_tb303_project() (the document of the TB-303 journey: a module instrument and an acid line, demo_project.h).
  // `inserts` (OS-INSFX S4, `document inserts`): fill_insert_project(): the demo with three insert records (the Mixer INSERTS page; with `apps on` the picker lists the card's modules/effects).
  // `sound` (OS-E2E-2, `document sound`): fill_sound_project() instead of the demo (the document of the audio journeys: an empty pattern and four neutral instruments, demo_project.h).
  void open_document(bool with_bindings = false, bool with_new = false, unsigned tracks = 0, bool sound = false, bool tb303 = false, bool inserts = false);
  // OS-E2E-1b (scenario `card <fixture-dir>`, `card pull`, `card insert`): the simulator gets a card. open_card() copies the fixture folder into a fresh scratch directory (removed when the simulator ends, so
  // a `--twice` run starts clean), mounts it as the card of the app (the File screen lists it), gives the app the card's write side (File's SAVE, SAVE AS, EXPORT write there, through the real path guard),
  // the sample memory and bank, and a project loader that opens what the card holds (sim_project.h: File > OPEN and NEW work). It needs a document (`document` first: the loader replaces it), and a relative
  // `fixture` is relative to the folder set_scenario_dir gave (o3sim: --cards, else the scenario file's folder). One card a run, and open_document() after it takes the card away with the old document (the scenario runner refuses the combination). card_pull() takes it out of the slot (every card call fails, the strip says CARD REMOVED, a file being written stays
  // torn), card_insert() puts it back and mounts it (READING CARD, then CARD READY), as the platform's slot poll does. false: `error` says why.
  bool open_card(const std::string& fixture, std::string& error);
  bool card_pull(std::string& error);
  bool card_insert(std::string& error);
  // OS-E2E J2 (scenario `card copy <from> <to>`): the user took the card to a computer and copied a folder to another place (an EXPORT's OG folder into Projects/, to open it): <from> is a folder of the
  // card, <to> does not exist yet and its parent does; both are plain relative paths. Only while the card is pulled (a computer does it), the guard of the writes does not see it; `card insert` follows.
  bool card_copy(const std::string& from, const std::string& to, std::string& error);
  // OS-QANET D2 (scenario `card slow <bytes-per-ms> <latency-us> [<write-bytes-per-ms>]` and `card slow off`; o3sim --card-rate, --card-latency, --card-write-rate): the slow-card model
  // (platform/host/slow_card.h) over the card the app sees: every call of the app, the loader, the apps and the screenshot charges its time to the simulated clock in the pass where it
  // happens, and the audio engine goes on rendering through the stretched pass (one block a millisecond of the clock, as the unit's ISR does while the main loop waits for a sector). A zero rate is
  // refused. It may come before `card` or after it; off (the default) nothing is charged and a run is as before. The numbers are PROVISIONAL until OS-QANET D4 (CardTiming).
  bool set_card_timing(bool on, std::uint32_t read_bytes_per_ms, std::uint32_t latency_us, std::uint32_t write_bytes_per_ms, std::string& error);
  const platform::host::CardTiming& card_timing() const { return card_timing_; }
  // OS-QANET B8 (scenario `card full` / `card full off`): the card is full: the app's writes that would grow it (create, make_dir, a marker, any written byte) are refused with NoSpace, reads and deletes
  // still work. Independent of the slow-card model; may come before `card` or after it; off (the default) nothing changes.
  void set_card_full(bool full) { card_timing_.full = full; }
  // Microseconds the model has charged since the card was inserted (0 without a card or with the model off).
  std::uint64_t card_charged_us() const;
  // The simulated clock in microseconds since the start (the ticks and the card's time).
  std::uint64_t uptime_us() const { return clock_.uptime_us(); }
  bool has_card() const { return card_ != nullptr; }
  // The card as the app reads it (`assert file` asks it); nullptr without one. A card that is pulled is still returned: it answers NoCard and NotMounted itself.
  hal::storage::Storage* card_storage() const noexcept;
  // Where a relative fixture path of `card` starts (o3sim sets it to --cards, else the folder of the scenario file). Empty: the working directory.
  void set_scenario_dir(std::string dir) { scenario_dir_ = std::move(dir); }

  // OS-FWCONFIG (scenario `firmware on`): the firmware update dialog gets a port with no flash behind it, the fake sink of platform/host/fake_firmware.h, with four scripted files in this order:
  // TRACKER_1.PTF (the reset target: a good image, checked in 3 passes; its hand-over ends in DidNotStart after 3), SLOW_CHECK.PTF (a good image whose check takes 600 passes; its hand-over never
  // ends), BAD_IMAGE.PTF (the check fails: "flash configuration field is not the required one" at byte 1104) and NEVER_ENDS.PTF (a good image whose hand-over never ends: the chip has reset, as on the unit).
  // Config's INFO page then labels F8 UPDATE. Nothing is read from a card and no pin exists. false: it is on already.
  bool firmware_on(std::string& error);
  bool has_firmware() const { return firmware_on_; }
  const platform::host::FakeFirmwareSink& firmware() const { return firmware_; }
  // OS-APPUX S0 (scenario `apps on`): the card carries the shipped apps (sim_apps.h): their host objects are the module set of the app's MODULES screen, `modules/apps/<stem>.tmod` is written on the card, and the
  // app gets the module system (App::attach_modules) over the card. It needs the card (the apps are files of it) and no engine (`audio on` and the apps exclude each other: the tap is idle). open_document() takes
  // it away with the card. false: `error` says why.
  bool apps_on(std::string& error);
  bool has_apps() const { return apps_ != nullptr; }

  // OS-E2E-2 (scenario `audio on`): the real engine. audio_on() builds the rig over the open document (sim_audio.h): the engine and its voices (the card's pool when there is a card, else a pool of its
  // own with the test bank), the sequencer as the engine's event source, the live notes, the send effects and the limiter; the app gets the engine's ParamChange ring, the sequencer's position, a
  // transport, a live preview, the mix and perform boards. It needs a document without the File > NEW sink of `document new` (refused: its NEW rewrites the project under the engine, with no Stop and no wait; a card's loader has both), and the document and the card are not replaced after it (the scenario runner refuses `document` and `card`; open_document()
  // drops the rig). From then on every tick renders the blocks that are due. false: `error` says why.
  bool audio_on(std::string& error);
  bool has_audio() const { return audio_ != nullptr; }
  SimAudio* audio() { return audio_.get(); }
  // `render <name> <ms> [stems]`: a window of `ms` ticks from now whose blocks are the render (it needs audio_on()). One at a time, names once.
  bool render_begin(const std::string& name, std::uint32_t ms, bool stems, std::string& error);
  // OS-E2E-9 (scenario `wav <name> <path>`): the WAV file `path` of the card (16-bit PCM, two channels, as an EXPORT writes it) becomes the finished render `name`, at its own rate. false: `error` says why.
  bool load_wav(const std::string& name, const std::string& path, std::string& error);
  // A finished render (a stems render's parts are `<name>.t1`.. and `<name>.m`); nullptr when there is none or it is still running.
  const Pcm* render_result(const std::string& name) const;
  // The render that is still running ("" when none) and the ticks it has left.
  std::string render_running(std::uint32_t* ticks_left = nullptr) const;
  // Every finished render, in the order of its name, with the SHA-256 of its samples (what --twice compares).
  std::vector<std::pair<std::string, std::string>> render_digests() const;
  // The pad held down for `ms` (OS-E2E-2, scenario `pad <n> hold <ms>`): down, `ms` ticks, up, one tick.
  void pad_hold(unsigned index, unsigned ms);
  void pad_down(unsigned index);  // the two halves of pad(): the event and one tick each
  void pad_up(unsigned index);
  // OS-QANET C2 (scenario `midinote <channel> <note> <velocity | off>`): a note message on MIDI port 0 as the render feeds it to midi::CcInput (the parser, then the notes' ring), then the app's
  // poll_midi_notes() once, as the platform's main loop does; the app hands the note to the live preview and, in Edit on Pattern, writes the step. Then one tick. velocity 0 is a note off.
  void midi_note(unsigned channel, unsigned note, unsigned velocity, bool on);

  const model::Project* project() const;
  // What the fake audio side has been told, in order: every ParamChange drained from the ring since open_document().
  const std::vector<core::ParamChange>& heard_params() const;

  const EveRaster& raster() const { return raster_; }
  const app::App& app() const { return app_; }
  app::App& app() { return app_; }
  const RasterDisplay& display() const { return display_; }
  RasterDisplay& display() { return display_; }  // for tests that put a frame of their own on the screen

  // False once a frame did not fit the display list (it was dropped, the previous picture stays), the raster
  // refused a list, or the UI's bitmaps could not be uploaded to RAM_G at start-up; failure() says what.
  bool healthy() const;
  std::string failure() const;

 private:
  friend struct SimShot;  // OS-SCREENSHOT: reads the raster, the clock and the card, tells the app
  platform::host::FakeClock clock_;
  platform::host::CardTiming card_timing_;  // OS-QANET D2 (before the card whose SlowCard reads it)
  std::uint32_t card_extra_us_ = 0;         // card time not yet given to the audio as whole ticks
  bool card_catching_up_ = false;           // the audio is rendering inside a card call (no second round from there)
  platform::host::FakeInput input_;
  EveRaster raster_;
  RasterDisplay display_;
  audio::PlayPosition play_;  // the engine's word for the position; the fake one is the scenario's
  std::unique_ptr<app::Recorder> wait_rec_;  // OS-METROALIGN: the take that waits, for `playhead ... wait` (before the app: it must outlive it)
  std::unique_ptr<SimDocument> document_;  // (before the app, so that it is destroyed after it: ~App closes a take's undo group on the editor, OS-LIVEREC)
  std::unique_ptr<SimProjectSink> project_sink_;  // (after the document it works on, before the app that points at it)
  std::unique_ptr<SimCard> card_;                 // `card` (OS-E2E-1b): after the document it loads into, before the save layer that writes to it and the app that points at both
  std::unique_ptr<app::SaveService> saves_;       // `power unsaved` (no card) or `card`: the save layer (before the app that points at it)
  std::unique_ptr<SimApps> apps_;                 // `apps on` (OS-APPUX S0): over the card, before the app that holds its module system
  std::unique_ptr<SimShot> shot_;                 // OS-SCREENSHOT: made by the first Shift + Power with a card; after the card it writes to
  std::unique_ptr<SimAudio> audio_;               // `audio on` (OS-E2E-2): after the document and the card it plays and renders from, before the app that holds its sinks
  std::string scenario_dir_;
  platform::host::FakeFirmwareSink firmware_;     // `firmware on` (OS-FWCONFIG): the update port with no flash behind it (before the app that points at it)
  bool firmware_on_ = false;
  std::unique_ptr<midi::CcInput> midi_in_;  // `midinote` (OS-QANET C2): made by the first one; the app polls it, so it outlives nothing it points at
  app::App app_;
  bool assets_loaded_ = false;
  core::PowerHold power_hold_{true};  // OS-POWER P1: active low, as the board is assumed to be
  bool power_down_ = false;           // the switch is held
  bool shift_down_ = false;           // Shift is held (the hold is spent when Shift is down; Shift then Power is the screenshot, OS-SCREENSHOT)
  bool power_committed_ = false;
  void power_pass();
  void card_time(std::uint32_t us) noexcept override;  // platform::host::CardClock: the clock moves, and the audio renders what the stretched pass lets pass
};

}  // namespace o3os::sim
