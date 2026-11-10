// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Declarations shared between the translation units of the MK66 image. Private to src/.

#include <o3os/midi/config.h>
#include <o3os/platform/mk66/bench_report.h>
#include <o3os/platform/mk66/crash_record.h>
#include <o3os/platform/mk66/i2c_bus.h>
#include <o3os/platform/mk66/i2c_config.h>
#include <o3os/platform/mk66/idle_hook.h>

#include <o3module.h>

#include <cstdint>

namespace o3os::hal {
class Display;
}
namespace o3os::audio {
class ModuleVoices;
}

namespace o3os::platform::mk66::boot {

// ---- boot.cpp: the reset sequence's steps, in the order startup.cpp calls them -------------------------------------
void early_pins() noexcept;        // port clocks on; power key (PTA24) high; audio muted at once; ~2 ms wait (board::kRailSettleUs); then the other quiet pins (LED off, FT812 released, MIDI TX idle); starts the cycle counter
void enable_dwt() noexcept;        // cycle counter running (hal::cycles(), the delays below)
void init_clocks() noexcept;       // crystal -> HSRUN -> PLL: 180 MHz core, 90 MHz bus and FlexBus. Fails visibly, never hangs.
void init_interrupts() noexcept;   // NVIC priorities from hal/irq.h, fault handlers enabled, divide-by-zero traps
void start_systick() noexcept;     // 1 kHz tick; the clock reads valid from here
void init_sdram() noexcept;        // power, pins, controller, JEDEC sequence, then the wiring and capacity tests
void dma_init() noexcept;          // eDMA clock gates, DMA_CR and every DCHPRI from resources.h: the ONE owner of the eDMA's global configuration
void capture_reset_cause() noexcept;

// Busy-wait delays on the cycle counter, bounded whatever the counter does. Accurate once init_clocks() has finished;
// before that they are sized for the clock the core is running at (see clock_config.h kFeiHz), good to tens of percent.
void delay_us(std::uint32_t us) noexcept;
void delay_ms(std::uint32_t ms) noexcept;
// OS-POWER P2: the core clock the delays above are sized for, when the VLPR off changes it (init_clocks() is the only other writer). Main thread only.
void set_core_hz(std::uint32_t hz) noexcept;

// OS-003f: a function delay_ms() calls once every millisecond it waits (never from inside itself, and never from an interrupt: the
// main thread only), so that the display keeps being serviced while the audio bring-up blocks for most of a second (the codec's
// waits are delay_ms(); the USB start does not wait with it): the boot logo's swap and the backlight ramp would otherwise wait for
// the main loop. nullptr (the default, and what every wait before the display exists sees) is no hook. The object is idle_hook.h.
void set_idle_hook(IdleHook::Function hook) noexcept;

void led_set(bool on) noexcept;

// Plays blink code `pulses` for ever. For a failure at boot: it is all the image can say.
[[noreturn]] void fail(std::uint8_t pulses) noexcept;

// ---- crash_store.cpp ------------------------------------------------------------------------------------------------
void capture_boot_record() noexcept;                       // copies a valid noinit record aside, at boot
crash::CrashRecord load_stored_record() noexcept;          // raw copy of the noinit block's record (may be garbage)
void store_record(const crash::CrashRecord& r) noexcept;
std::uint32_t load_context() noexcept;                     // crash::kUnknownContext if the stage words are not trustworthy
void reset_context() noexcept;                             // stage Reset, no module: the first thing a run does, so a crash that early does not claim the last run's stage
[[noreturn]] void reset_chip() noexcept;                   // SYSRESETREQ
std::uint32_t uptime_ms_low() noexcept;                    // clock.cpp: SysTick milliseconds since boot, no lock (fault path)
std::uint32_t rail_up_ms() noexcept;                       // boot.cpp, OS-BUG-PADS: the uptime (ms) at which the power key (PTA24) went on: 0, early_pins raises it before SysTick starts

}  // namespace o3os::platform::mk66::boot

// ---- cache.cpp: the LMEM code cache (OS-PERF3; lmem.h has the facts) -----------------------------------------------------------------------
namespace o3os::platform::mk66::cache {
bool enabled() noexcept;               // PCCCR ENCACHE
std::uint32_t control() noexcept;      // PCCCR as it reads
std::uint32_t region_modes() noexcept; // PCCRMR
bool enable() noexcept;                // invalidate both ways and turn the cache and its write buffer on; false: the command did not finish
bool disable() noexcept;               // cache and write buffer off (flash is read-only, so nothing is lost)
bool invalidate_all() noexcept;        // both ways, when the cache is on (a no-op otherwise); false: the command did not finish
bool invalidate_range(std::uint32_t address, std::uint32_t bytes) noexcept;  // the lines of [address, address + bytes), when the cache is on and the range is cacheable
}  // namespace o3os::platform::mk66::cache

// ---- bench.cpp: `diag bench` (OS-PERF3; bench_report.h has the line it prints). Only in an image built with -DO3OS_BENCH=ON (OS-BENCHOPT) -------------
#if O3OS_BENCH
namespace o3os::platform::mk66::bench {
// Called once for each line a run prints (a line ends with a newline). The text is only valid during the call.
using Emit = void (*)(void* ctx, const char* line, std::size_t n);
// Runs `kernel` for `blocks` blocks (0: the default; the `mem` kernel ignores it) with its data placed as `variant` says and emits its lines. Blocks the main loop for about blocks x 1.5 ms. Main thread only.
void run(Kernel kernel, std::uint32_t blocks, Variant variant, Emit emit, void* ctx) noexcept;
// `diag cache check` (OS-PERF3): the single-alias rule on the unit, main loop only. A scratch stretch of SDRAM written and read through the alias and the system-bus address; a negative control
// (words changed through the other address while the cache holds their lines read stale until an invalidate); WRITE HITS (stores of 32, 16 and 8 bits into lines the cache holds, read back through
// the alias, through 0x7000_0000 and through the alias after a full invalidate); the scratch put back; then the loaded samples' PCM summed through the alias (cached), through 0x7000_0000
// (uncached) and again through the alias after a full invalidate. One line per sub-check, `cache check <part>: PASS|FAIL|SKIP ...`, then `cache check: PASS`, `cache check: FAIL <parts>` or
// `cache check: SKIP ...` (cache_check.h has the lines).
void check_coherence(Emit emit, void* ctx) noexcept;
// `diag bench disk reset` (OS-USBDISK U1d): opens the measuring window of the USB interrupt and of the MSC pump passes (`bench disk` reads it). Main loop only.
void disk_reset() noexcept;
// `diag bench disk clean` (OS-USBDISK U1d, Q14): removes the write arm's scratch file `o3-bench/disk-scratch.bin` (same gate as the write arm: a card the Tracker owns), one line. Main loop only.
void disk_clean(Emit emit, void* ctx) noexcept;
}  // namespace o3os::platform::mk66::bench
#endif

// ---- i2c.cpp: the I2C master of the board's two buses (OS-005a for the codec on I2C0, shared with the input drivers, OS-007d) -------
namespace o3os::platform::mk66::i2c {

using Result = I2cResult;

// Clock gate, pins (open drain), divider, enable of one instance (I2C0: the control bus, PTD8/PTD9; I2C2: the grid bus, PTA12/PTA13).
// Releases a bus a slave is holding (a reset in the middle of a transaction): up to nine SCL pulses, then a STOP. Main thread, after
// the clock tree is up; calling it again is harmless (the codec's `init()` and the input bring-up each call it for I2C0).
Result init(i2c_cfg::Instance which) noexcept;
Result init() noexcept;  // I2C0, the codec's bus
// The instance's master as the seam the input drivers use (i2c_bus.h). One object per instance, shared by every client.
I2cBus& bus(i2c_cfg::Instance which) noexcept;
// 16-bit register write / read of a 16-bit-register device on I2C0 (the SGTL5000: register address and value big endian). Polled with
// every wait bounded in time (5 ms a byte); a failure leaves the bus released. Main thread only.
// OS-011c: the Si4703 radio picks its 2-wire mode at the rising edge of its reset if SDIO (this bus's SDA) is low. sda_hold(true) drives SDA low as a GPIO (the peripheral
// is detached from the pin); sda_hold(false) releases it and runs init() again, which puts the pins back to the peripheral. I2C0, main thread only, nothing else on the
// bus meanwhile (the main loop is the bus's only user).
Result sda_hold(bool low) noexcept;
Result write_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t value) noexcept;
Result read_reg(std::uint8_t address7, std::uint16_t reg, std::uint16_t& value) noexcept;

}  // namespace o3os::platform::mk66::i2c

// ---- app_main.cpp: the image's composition root (the one platform file that may include the app and ui layers) ----------
namespace o3os::platform::mk66::appimg {
// Uploads the UI's bitmaps to RAM_G through `display`, puts the boot screen up (OS-003f) and builds the app over it. false: the upload failed,
// no app is running.
bool init(hal::Display& display) noexcept;
// One pass of the main loop: the display driver's service, then the app's frame tick when it is due (not before the boot screen has had
// its minimum, core/boot_hold.h). Returns 0, or the blink code of
// a fault found (blink.h kDisplayStalled, kAppFailure): the display is held dark and the caller shows the code in place of the
// heartbeat, for good.
std::uint8_t pass() noexcept;
// OS-007d: true when the control-button key scanner did not answer at boot; main.cpp plays blink::kInputDegraded once, before the audio start.
bool input_degraded() noexcept;
// OS-BUG-PADS: the pad bus is brought up late (pad_bus.h), so the pads have no boot verdict. main.cpp tells the input that the audio start is over
// (the second condition of the pad bus's gate), and shows blink::kInputDegraded once, from its loop and without waiting, when this says the pad
// key scanner has refused its first configurations since the bus opened and has never answered (the retries go on, a late answer brings the pads online).
void input_audio_started() noexcept;
bool pads_gave_no_answer() noexcept;
// OS-LEDBPM: the status LED. status_led_level() is main.cpp's LED level once a pass (the saved STATUS LED setting, Blink / BPM / Off, or the console's session override; a fault code wins in
// every mode; BPM reads the sequencer's play position, lock-free). status_led_set(mode) is the `diag status` override for this session, not saved (-1: back to the setting); status_led_info()
// the mode in force, whether it is the override, the beat flashes since boot and the play step of the last one (0xFFFF: a free-run flash). Main loop only.
struct StatusLedInfo {
  std::uint8_t mode;  // blink::LedMode
  bool overridden;
  std::uint32_t flashes;
  std::uint16_t last_pos;
};
bool status_led_level(std::uint8_t shown_code, std::uint32_t ms) noexcept;
void status_led_set(int mode) noexcept;
StatusLedInfo status_led_info() noexcept;
// OS-USBDISK U1c, the boot disk (mode (b)): true when the chord Shift + F1 + F7 was held at power-up (init() decides, once). main.cpp then mounts nothing, loads nothing and never opens the
// outputs. boot_disk_open() is its last step, after the USB port started with the DISK set: the card's owner becomes the computer and the card is started for it. Main loop only.
bool boot_disk() noexcept;
void boot_disk_open() noexcept;
// OS-HW1: around a long blocking call made from the main loop (`diag audio start`: the codec's 0.5 s ramp) the display is serviced from the idle hook,
// as it is during the boot's waits. `on` false puts back what was there: no hook, or the hook of the boot screen's hold while that lasts.
void service_while_blocked(bool on) noexcept;
// OS-005d, OS-OUTON: every image boots with the outputs muted and main.cpp opens them once, at boot (mk66::open_wanted: after the SD mount and the boot project load, once
// the audio path has run clean; the codec's unmute blocks for 0.2 s), and keeps them open until power-off or a reset, so a Play never waits. The request below is the
// fallback: a Play, a Song Play or a live note that finds them closed (an audio start that came late, `diag audio start` after a boot failure) asks for them and holds the
// play back; main.cpp opens them through the same gate and then says so (audio_opened), which starts the play. false whenever the image has no sound (the tone image).
bool audio_open_pending() noexcept;
void audio_opened() noexcept;
// OS-OUTON: whether the outputs are open (audio_opened() set it), and the other way: audio_closed() says the outputs were muted behind the app's back (`fw update`'s quiesce
// before the hand-over), so that a Play waits for the gate again and main.cpp opens them again once the update path let go. The tone image: false, no-op.
bool audio_outputs_open() noexcept;
void audio_closed() noexcept;
// OS-OUTON: the firmware update path holds the card or waits for the bootloader (an upload, a re-check, the hand-over): main.cpp never raises the outputs then. Main loop only.
bool firmware_update_active() noexcept;
// OS-006c: stops the sequencer's transport (a Stop on its ring, the badge says so), for the hand-over to the bootloader (`fw update`).
void stop_transport() noexcept;

// ---- OS-POWER P1 (power/src/power_host_mk66.cpp, docs/specs/OS-POWER.md 4): what the shutdown asks of the composition root. Main loop only.
// The power-off latch: set by power_quiesce() (stage A), before anything stops, and never cleared (a restart ends the run). main.cpp's audio poll takes no stall verdict and no pattern while it is
// set, and mk66::open_wanted() is false: nothing reopens the outputs (4.6).
bool power_off_latched() noexcept;
// Stage 0: a WAV export, a set of stems or an OG project export runs (App::export_running); the second hold stops it (App::export_stop).
bool power_export_running() noexcept;
void power_export_stop() noexcept;
// Stage A: the latch, the input locked (the keys, pads and encoder are dropped, the MIDI input is not read, the console refuses its commands), the transport and the live notes stopped, the app in
// its power mode (App::power_begin: its card jobs cancelled, the save layer takes nothing new but the settings flush).
void power_quiesce() noexcept;
// Stage B: the save layer's own (SaveService::power_busy / power_committing / power_cancel).
bool power_save_busy() noexcept;
bool power_save_committing() noexcept;
void power_save_cancel() noexcept;
// Stage E, at its end: the display is dark and held and the chip is in power-down, so appimg::pass() does not touch it from here.
void power_display_off() noexcept;


// OS-007e: loading an OG project folder from the card into the document (app/og_folder.h), in slices of the main loop: pass() steps the loader and,
// while it runs, holds the app's frames (the project is changing under them; the screen keeps showing the last frame and the display is still
// serviced). When it ends the undo history is reset, the app is re-attached to the document and the result is logged.
enum class LoadStart : std::uint8_t { Started, NoCard, NoDocument, Busy, BadPath, Scratch };
inline const char* load_start_name(LoadStart s) noexcept {
  switch (s) {
    case LoadStart::Started: return "started";
    case LoadStart::NoCard: return "the card is not mounted";
    case LoadStart::NoDocument: return "no document (the app did not start)";
    case LoadStart::Busy: return "a load or a save is already running";
    case LoadStart::BadPath: return "not a folder path";
    case LoadStart::Scratch: return "END THE SCRATCH FIRST";  // OS-DJ C1: the DJ scratch holds the arena's tail (the File actions say the same)
  }
  return "?";
}
// What a project folder holds (OS-007o): an OG project (app/og_folder.h), a native one (project.o3pj, app/native_load.h), or the backup of a native one
// (project.o3pj.bak: what a power cut inside a replacing save leaves, app/atomic_save.h).
enum class LoadFormat : std::uint8_t { Og, Native, NativeBackup };
// Starts loading `folder` of the mounted card ("Workspace", "Projects/My Song") into the document; the next passes do the work.
LoadStart start_project_load(const char* folder, LoadFormat format = LoadFormat::Og) noexcept;
// OS-QAFIX-E-FILE-10: opens the last project at power-on through the app (strip tells the end). False: nothing started (none on the card, no app): main.cpp falls back to the Workspace.
bool boot_open_last_project() noexcept;
void load_device_settings() noexcept;  // OS-007n: Settings/device.o3st into the app's device settings (read only; main.cpp calls it once the card is mounted)
bool start_usb() noexcept;             // OS-USBAUDIO: usb0::start() with the set the device settings decide (main.cpp calls it after load_device_settings, so the host enumerates once)
bool project_loading() noexcept;
// OS-007o: File F3 NEW. A fresh project in the document (the model's defaults, the kit's instruments, an empty pattern), through the hand-over of a load. Same answers as
// start_project_load without the card's.
LoadStart start_new_project(bool keep_instruments) noexcept;
// OS-FILES part A (File > IMPORT): a module file of the mounted card ("MOD/song.mod") replaces the document; `engine` is app::ImportEngine (0 auto, 1 Amiga, 2 Modern). The phases are advance_load's.
LoadStart start_module_import(const char* file, std::uint8_t engine) noexcept;
// OS-007o: the card is in use by the app: a project is loading or a save job is running. The firmware update path (`fw put|rm|arm|update`) refuses while it is true,
// and the save layer refuses to start while the update path holds the card (SaveService::attach_claim): FatFs on the volume is not re-entrant and a hand-over to the
// bootloader must not cut a save. Main loop only.
bool storage_in_use() noexcept;
// One line saying what the last load did (app::describe_load), "" before any.
const char* project_load_summary() noexcept;
// OS-USBDISK U2a, the running mode (disk_share.h): the console's `disk share [ro]` / `disk eject` / `disk`. nullptr: the request went through (the state machine takes it from the next pass); else the reason it did not.
// Main loop only. The state name is a short phrase for `disk`.
const char* disk_share_request(bool read_only) noexcept;
const char* disk_eject_request() noexcept;
const char* disk_share_state_name() noexcept;
// OS-007f: after a project has loaded, its sample instruments' PCM follows (app/og_samples.h): the transport is stopped when the load starts, the voices'
// table is cleared, the samples go into the SDRAM sample arena and are bound to the voices, all in slices of the main loop with the app's frames held; a project
// with no sample instrument gets the demo kit again. One line saying what it did ("samples 18 loaded, 2 missing, 3210 KB"), "" before any.
const char* project_sample_summary() noexcept;
// OS-007h: `diag inst <n>`: one line saying what the voices were given for instrument slot `slot` (app::describe_instrument): the bound window's start and length,
// loop, root, rate, volume and a 16-bit checksum of its first 64 samples. Reads the document and the voices' table; the main loop only.
std::size_t project_instrument_line(unsigned slot, char* out, std::size_t cap) noexcept;
// OS-FILES-C C2a, `diag ogexp`: the project in the document written in the OG format into Export/<project> OG[ n] on the card (app::App::start_og_export, one file a pass of the main loop, project.mt last).
// start_og_export returns nullptr when it started, else why not ("STOP THE TRANSPORT FIRST", the card's own refusals). og_export_status fills `out` with how the last one is going and returns false when this
// image has no save layer. Main loop only.
struct OgExportStatus {
  const char* state;   // IDLE, RUNNING, DONE, FAILED, CANCELLED, MODIFIED
  unsigned done;       // files finished
  unsigned total;
  const char* folder;  // "Export/Song OG", "" before the first
  const char* why;     // after FAILED, MODIFIED: in a few words
  bool lost;           // something was left out or rewritten (the counters are in the log)
  const char* levels;  // C2b: LEVELS WRITTEN AS PLAYED (1.1.14 -> 1.2+) or LEVELS WRITTEN AS STORED, "" before the first
  unsigned clipped;    // C2b: outputs the resampler saturated to 16 bits
  // C2c: the counters that say what the files do not hold, by the name of the field of formats::og::ExportDrops (known when the export starts; 0 before the first). `diag ogexp status` prints the non-zero ones.
  unsigned instruments_not_written;
  unsigned no_sample_memory;  // of those, the ones skipped because the sample memory was not up
  unsigned bit_engine;
  unsigned amiga_engine;
  unsigned pcm_too_long;
  unsigned levels_clamped;
  unsigned max_step_us;  // T16: the longest single step of any file's job (core::JobStats), over the files finished
  unsigned overruns;     // ... and the steps that took clearly longer than their slice
};
const char* start_og_export() noexcept;
void cancel_og_export() noexcept;
bool og_export_status(OgExportStatus& out) noexcept;
// T16: line i (0 .. 9) of the OG export's per-stage timing (app::OgTiming::line: "time worst 263412 us in write", "time write max .. us n .. sum .. ms") into out[0..cap); the length, 0 when there is none.
std::size_t og_timing_line(unsigned i, char* out, std::size_t cap) noexcept;
// OS-005c2: the clock follow's state for a status line (locked, tempo, clocks, error), a snapshot a block old at most. The MIDI configuration is the device settings'
// (CLOCK IN, CLOCK MODE, CLOCK SYNC, MIDI OUT, OUT CHANNEL: seq::sync_config_from_settings, OS-013a); the offsets, FOLLOW and the CC table are the link's own and have no setter yet.
midi::SyncStatus midi_status() noexcept;
// OS-012a: SD modules (ADR 0002), for `diag mod`. start_module_load: `mod load <stem>` reads modules/instruments/<stem>.tmod (or effects/) in slices of the main loop and
// logs the result (the result is the log's: `diag log`). module_line(i): line i of the listing (the memory in use, the quarantine, the loaded modules, the bound instruments with their perf line while the measure is on), 0 when
// there is none; module_bind(slot, stem): an instance of a loaded module on instrument `slot` (0-based) without a project (it goes at the next project load): null or the reason it
// could not; module_audition: a note on track 16 for the module of `slot` (needs the outputs open, which they are from boot); module_set_param: a parameter change for the module of `slot`.
enum class ModStart : std::uint8_t { Started, NoCard, Busy, BadName, Already, Quarantined, NoSound };
inline const char* mod_start_name(ModStart s) noexcept {
  switch (s) {
    case ModStart::Started: return "started";
    case ModStart::NoCard: return "the card is not mounted";
    case ModStart::Busy: return "busy: a load is running";
    case ModStart::BadName: return "not a module name";
    case ModStart::Already: return "already loaded";
    case ModStart::Quarantined: return "quarantined until reboot (it failed validation, or crashed the last run)";
    case ModStart::NoSound: return "this image has no sound";
  }
  return "?";
}
ModStart start_module_load(const char* stem) noexcept;
std::size_t module_line(unsigned i, char* out, std::size_t cap) noexcept;
// OS-CEPTFX S2c (`ptfx quarantine`): ptfx_line(i, ...) is line i of the quarantine set (one line an id), then its closing line with the full flag; the length, 0 when there is no such line. The set is loaded at
// boot from the crash record's module word. Main loop only.
std::size_t ptfx_line(unsigned i, char* out, std::size_t cap) noexcept;
// OS-CEPTFX S3b (`ptfx list`, `info`, `run`; main loop only, synchronous): ptfx_scan reads the Effects folder into the catalogue (a run in progress is given up first): null or the reason.
// ptfx_list_line(i): the plugin i, then the closing line; ptfx_info_line(n, i): line 0 is plugin n's header (or "ERR ptfx: <reason>"), then one line a parameter; both return 0 past the last line.
// ptfx_run(n): loads plugin n and runs it over the sample in view and its selection (App::run_ptfx): null when the job started, else the reason.
const char* ptfx_scan() noexcept;
std::size_t ptfx_list_line(unsigned i, char* out, std::size_t cap) noexcept;
std::size_t ptfx_info_line(unsigned n, unsigned i, char* out, std::size_t cap) noexcept;
const char* ptfx_run(unsigned n, const float* values = nullptr) noexcept;  // values: the 12 A15 values of the PLUGINS page (S4b), nullptr: the defaults
const char* ptfx_run_why() noexcept;  // after a refused ptfx_run: which check said PLUGIN NOT READY ("host", "plugin", "stereo", "scratch"), else nullptr (the console appends it)
const char* module_bind(unsigned slot, const char* stem) noexcept;
// OS-INSFX S1 (`diag ins`): an effect module (kind 2, loaded by `mod load`) as an insert of `track` (0..15), `slot` 0 (A) or 1 (B). Each returns null or the reason it could not. Nothing is saved: a project
// load drops the inserts. insert_line(i): line 0 the totals (OS-INSFX-ABI2 A5: with the stereo reservation, "stereo reserved N of 4"), lines 1.. the bound inserts (phase, forced, cost); 0 when there is none. Main loop only.
const char* insert_bind(unsigned track, unsigned slot, const char* stem) noexcept;
const char* insert_unbind(unsigned track, unsigned slot) noexcept;
const char* insert_bypass(unsigned track, unsigned slot, bool on) noexcept;
// OS-INSFX-ABI2 A8 (`mod preset <slot> <n>`, `ins preset <track> <slot> <n>`): factory preset `n` (1-based) of the module on instrument `slot` (0-based) or of the insert at (`track`, `slot`), written to the
// project and told to the instance (app::ModuleUi::apply_preset: an instrument's is one undo unit, an insert's none). `name` (at least 16 bytes) gets the preset's name, `dropped` how many values
// reached no instance (none bound, or the ring full: the project holds them). Each returns null or the reason it could not.
const char* module_preset(unsigned slot, unsigned n, char* name, std::size_t cap, unsigned& dropped) noexcept;
const char* insert_preset(unsigned track, unsigned slot, unsigned n, char* name, std::size_t cap, unsigned& dropped) noexcept;
// OS-MODPRESET P3: the card's presets of a module, from the console: `mod presets <stem>` (the list), `mod psave <slot> <name>` (save the values in force as modules/presets/<stem>/<name>.o3mp, the next free
// "<name> 2" on a taken name), `mod pload <slot> <name>` (read, resolve by name, apply: one undo unit), and the `ins` forms of the two for the insert at (track, slot), 0-based. Main loop only.
// module_preset_scan(stem, more): rescans the folder into the ModuleUi's list and returns how many names it holds (0: no card or no folder); `more`: the folder held more than the list's 128.
// module_preset_at(i): the i-th name (sorted case-insensitively), "" when there is none. The saves return null on success and write the file name chosen into `final`; the loads return null on success and write
// the reason into `why` otherwise ("PRESET DAMAGED", "PRESET OF <STEM>", "NO SUCH PRESET", ...); `skipped` gets the entry names no parameter has, `dropped` the values that reached no instance.
unsigned module_preset_scan(const char* stem, bool& more) noexcept;
const char* module_preset_at(unsigned i) noexcept;
const char* module_preset_save(unsigned slot, const char* name, char* final, std::size_t cap) noexcept;
const char* module_preset_load(unsigned slot, const char* name, char* why, std::size_t why_cap, unsigned& skipped, unsigned& dropped) noexcept;
const char* insert_preset_save(unsigned track, unsigned slot, const char* name, char* final, std::size_t cap) noexcept;
const char* insert_preset_load(unsigned track, unsigned slot, const char* name, char* why, std::size_t why_cap, unsigned& skipped, unsigned& dropped) noexcept;
std::size_t insert_line(unsigned i, char* out, std::size_t cap) noexcept;
#if O3OS_BENCH
const o3m_module* insert_bench_module(bool stereo = false) noexcept;  // `diag bench insert`: the first loaded effect module (`stereo`: the first whose InsertHost::shape_of is not Mono), or null
// OS-CEWTENV W3a (`diag bench wt`): the sample arena's free bytes (the recorder's room), main loop only.
std::uint32_t sample_arena_free_bytes() noexcept;
#endif
// OS-012d: the licence notices of the built-in modules, for `diag mod notice` (OS-FLASHDIET P1: a deflate stream in flash, streamed through the shared asset decoder, so main loop
// only and never across passes). Calls `sink` for lines [first, first + max_lines): pieces of a line (text, n, false) and its end (nullptr, 0, true); `more`: text follows the page, `next`:
// the line a following page starts at. False when the stream could not be read.
using NoticeSink = void (*)(void* ctx, const char* text, std::size_t n, bool eol) noexcept;
bool module_notices_page(unsigned first, unsigned max_lines, NoticeSink sink, void* ctx, bool& more, unsigned& next) noexcept;
// OS-PERF3 (`diag cache check`): how many bytes of the sample arena hold loaded PCM (the demo kit or a project's samples), from the start of the arena.
std::uint32_t sample_pcm_bytes() noexcept;
bool module_audition(unsigned slot, unsigned note, unsigned velocity, bool on) noexcept;
// OS-PERF3 (`diag bench tb303`): the module voices of the sound, null when the image has none or the sound is not built yet. Main loop only.
audio::ModuleVoices* module_voices() noexcept;
bool module_set_param(unsigned slot, unsigned id, float value) noexcept;
// OS-RAD1: `diag radio`: the FM radio's driver text and the lines' levels (capture_radio_report), at most `cap` bytes, and the length (0 when this image has no sound, so no
// radio); `diag radio retry` starts the power-up again from the reset, unless the recorder says no (a take from the radio is armed or recording, or the radio is not the selected
// source: Recorder::radio_retry), and returns that verdict (NoRadio: this image has no sound). Main loop only.
std::size_t radio_report(char* out, std::size_t cap) noexcept;
enum class RadioRetry : std::uint8_t { Started, NoRadio, NotSelected, TakeRunning };  // (the app layer is not visible here: app::Recorder::RadioRetry, one for one)
RadioRetry radio_retry() noexcept;
// OS-HIL (`diag state`, `key`, `shot`): the app's state as one `key=value` line (app::describe_state; `app=down` before the app has started), at most cap - 1 characters and a NUL, and the length. Main loop only.
std::size_t state_line(char* out, std::size_t cap) noexcept;
// OS-BUG-PLAYSTOP (`diag trans`): the transport's cold counters (every refusal reason of SeqTransport::play()/stop()) and the app's and the sequencer's `playing` flags, as one line. Main loop only.
std::size_t transport_line(char* out, std::size_t cap) noexcept;
// OS-MTC A3 (`diag mtc`, `diag mtc at <order> <step>`): the time code generator's state, or the song time of a step (A1's map) and the cycles the walk took, as one line. Main loop only.
std::size_t mtc_line(char* out, std::size_t cap, bool at, std::uint32_t order, std::uint32_t step) noexcept;
// OS-AUDIT-28 (`enc accel`): the encoder acceleration's numbers (app/enc_accel.h). enc_accel_get is false before the app has started; enc_accel_set is false then, or for numbers EncAccel::valid refuses. Main loop only.
struct EncAccelNumbers {
  bool on;
  std::uint8_t t1;
  std::uint8_t t2;
  std::uint8_t gap_ms;
};
bool enc_accel_get(EncAccelNumbers& out) noexcept;
bool enc_accel_set(const EncAccelNumbers& in) noexcept;
// The code of a front-panel button word ("play", "f1", "shift", or a decimal code), -1 when it is none (app::button_code).
int button_code(const char* name) noexcept;
// The part of the chip's RAM_G that nothing else owns and `diag shot` may write its snapshot bands into: the map's spare region (ui/plot/ram_g_map.h) less the DMA self-test's scratch.
void shot_region(std::uint32_t& addr, std::uint32_t& bytes) noexcept;
// OS-SCREENSHOT: a Shift + Power screenshot is armed or running; the console's `shot` is refused meanwhile (the two share the RAM_G region).
bool screenshot_active() noexcept;
}  // namespace o3os::platform::mk66::appimg
