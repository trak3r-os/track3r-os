// SPDX-License-Identifier: GPL-3.0-or-later
//
// The skeleton's main: the reset sequence (startup.cpp) has brought up the clocks, the interrupts, SysTick and the SDRAM
// and tested the SDRAM; main reports what the last run left behind on the status LED, then idles in WFI with a heartbeat.
// OS-007c adds the display: the FT812 is brought up before the loop and the OS app (the pattern view over demo data) draws on
// it; there is no audio, SD or input yet, those are later OS-007 / OS-005 slices.
//
// Its only output is the LED (blink.h; the codes are in README.md). A dark LED after power-on means the image did not
// get as far as `early_pins`, not that it is idle.
#include "internal.h"
#include "regs.h"

#include <o3os/audio/diag.h>
#include <o3os/platform/mk66/audio.h>
#include <o3os/platform/mk66/blink.h>
#include <o3os/platform/mk66/board.h>
#include <o3os/platform/mk66/crash_record.h>
#include <o3os/platform/mk66/display/device.h>
#include <o3os/platform/mk66/mem_map.h>
#include <o3os/platform/mk66/output_gate.h>
#include <o3os/platform/mk66/platform.h>
#include <o3os/platform/mk66/sections.h>
#include <o3os/platform/mk66/storage/device.h>
#include <o3os/platform/mk66/usb/console.h>
#include <o3os/platform/mk66/usb/usb0.h>

#include <cstdint>

#ifndef O3OS_FAULT_DRILL
#define O3OS_FAULT_DRILL 0
#endif
// OS-005a: the HW-A variant. Off (the default image): the audio path runs and renders silence with the outputs held muted,
// nothing is ever unmuted. On: after the blocks have run without an underrun for a while, the outputs are unmuted and the test
// tone (1 kHz at -20 dBFS) is faded in. OS-005b: the tone is two test sources of the audio graph (track 1 hard left, track 2 hard right, both
// to MAIN at unity), so the image also exercises the graph: the meters read the tone (`diag mix`), and from 10 s on the main loop sweeps the
// tracks' level and pan with ParamChanges (graph_script below; not in the stall image).
#ifndef O3OS_AUDIO_TEST
#define O3OS_AUDIO_TEST 0
#endif
// With O3OS_AUDIO_TEST: 20 s after the tone starts, one render is made to overrun on purpose (3.5 block periods), so HW-A can see the
// underrun counters, the mute, the recovery and that left and right are still on their channels afterwards. The LED then shows the
// sticky audio-fault code, as it should: this image is for that one check, not for the 10-minute soak.
#ifndef O3OS_AUDIO_TEST_STALL
#define O3OS_AUDIO_TEST_STALL 0
#endif

namespace mk66 = o3os::platform::mk66;

namespace {

// Seconds since boot, counted by the main loop; the loop is the only writer. Meant for a debugger and, later, `diag`.
volatile std::uint32_t g_heartbeat = 0;

// The module id the deliberate fault of the drill build carries in its crash record, so the next boot can tell it from a
// real crash. Real module ids start at 1 (ADR 0002); this one is outside any a loader will hand out.
constexpr std::uint16_t kDrillModule = 0xD1;
// The module id a crash during the display bring-up carries (the same convention: outside any id a loader hands out).
constexpr std::uint16_t kDisplayModule = 0xD2;
// The module id a crash during the USB bring-up carries (the same convention).
constexpr std::uint16_t kUsbModule = 0xD3;
// The module id a crash during the SD mount carries (the same convention).
constexpr std::uint16_t kStorageModule = 0xD4;
constexpr std::uint32_t kDrillAfterMs = 10'000;
constexpr std::uint32_t kExceptionUsageFault = 6;
constexpr std::uint32_t kCfsrUndefinstr = 1u << 16;

// The audio verdict is polled this often (the snapshot is a few dozen loads) and the tone waits for this many blocks (about 0.6 s)
// of a clean run before the outputs open.
constexpr std::uint32_t kAudioPollMs = 100;
constexpr std::uint32_t kToneSettleBlocks = 400;
// No boundary for this long is a stopped block clock (a block is 1.45 ms).
constexpr std::uint32_t kAudioStallMs = 300;
// The forced overrun of the O3OS_AUDIO_TEST_STALL image comes this long after the tone starts.
constexpr std::uint32_t kStallAfterMs = 20'000;

// The graph sweeps run in the HW-A image (B) and its retune and drill variants, not in the stall image (F), which is about left and right
// after an underrun and must not move the pans.
#if O3OS_AUDIO_TEST && !O3OS_AUDIO_TEST_STALL
#define O3OS_AUDIO_SWEEP 1
#else
#define O3OS_AUDIO_SWEEP 0
#endif
#if O3OS_AUDIO_SWEEP
// OS-005b: after the channel check the HW-A image sweeps the graph's gains, so that "no clicks on level and pan sweeps" can be listened for.
// Three phases of kScriptPhaseMs, repeating, from kScriptStartMs after the tone started: the two tones fixed (track 1 left, track 2 right, at
// unity), a level sweep of both (100 down to 20 and back, a triangle), a pan sweep of both in opposite directions (hard left to hard right and
// back, and the other way round). Every value is a ParamChange from this main loop (the ring's one producer), applied in the graph's pass (after the
// block's voices) with the engine's one-block smoothing (ramp 0): what is heard is the smoothing contract, not a step per 50 ms.
constexpr std::uint32_t kScriptStartMs = 10'000;
constexpr std::uint32_t kScriptPhaseMs = 10'000;
constexpr std::uint32_t kScriptStepMs = 50;
volatile std::uint32_t g_script_drops = 0;  // changes the ring had no room for (a probe can read it; zero in a healthy run)

void script_push(unsigned slot, unsigned field, float value) {
  if (!mk66::audio_engine().params().push(o3os::audio::ParamChange{o3os::audio::strip_param(slot, field), value, 0u})) {
    g_script_drops = g_script_drops + 1u;
  }
}

// A triangle between `lo` and `hi` with the given period, at time t (ms).
float script_triangle(std::uint32_t t, std::uint32_t period, float lo, float hi) {
  const std::uint32_t half = period / 2u;
  const std::uint32_t m = t % period;
  const float x = static_cast<float>(m < half ? m : period - m) / static_cast<float>(half);
  return lo + (hi - lo) * x;
}

void graph_script(std::uint32_t since_tone_ms) {
  if (since_tone_ms < kScriptStartMs) {
    return;
  }
  const std::uint32_t since = since_tone_ms - kScriptStartMs;
  const std::uint32_t phase = (since / kScriptPhaseMs) % 3u;
  const std::uint32_t t = since % kScriptPhaseMs;
  namespace tp = o3os::audio::graph_param::strip;
  switch (phase) {
    case 0:  // fixed
      script_push(0, tp::Volume, 100.0f);
      script_push(1, tp::Volume, 100.0f);
      script_push(0, tp::Pan, 0.0f);
      script_push(1, tp::Pan, 100.0f);
      break;
    case 1: {  // level sweep
      const float v = script_triangle(t, kScriptPhaseMs, 100.0f, 20.0f);
      script_push(0, tp::Volume, v);
      script_push(1, tp::Volume, v);
      break;
    }
    default: {  // pan sweep, opposite directions
      const float p = script_triangle(t, kScriptPhaseMs, 0.0f, 100.0f);
      script_push(0, tp::Pan, p);
      script_push(1, tp::Pan, 100.0f - p);
      break;
    }
  }
}
#endif

// Which blink pattern the audio counters call for: the heartbeat (0) while everything is clean, else a sticky code. A fault is "seen since boot" (fault_seen): `diag cpu reset` zeroes the
// counters but not that latch, so a fault that landed between two polls of this loop still lights the LED.
std::uint8_t audio_pattern(const o3os::audio::StatusSnapshot& s) {
  if (s.fault_seen != 0u) {
    return mk66::blink::kAudioFault;
  }
  if (s.governor_over != 0u) {
    return mk66::blink::kAudioOverBudget;
  }
  return 0;
}

// The LED is driven from here, a function that lives in SRAM_L: if the pulse is there, `.ramfunc_l` was copied from flash
// and runs, which the first hardware session wants to see (README.md).
O3OS_RAMFUNC_L void led_from_sram_l(bool on) noexcept {
  const bool high = on != mk66::board::kStatusLedActiveLow;
  if (high) {
    mk66::regs::gpio_psor(mk66::board::kStatusLed) = mk66::regs::mask_of(mk66::board::kStatusLed);
  } else {
    mk66::regs::gpio_pcor(mk66::board::kStatusLed) = mk66::regs::mask_of(mk66::board::kStatusLed);
  }
}

// Plays blink code `pulses` `periods` times, blocking (boot-time reporting only).
void play_code(std::uint8_t pulses, std::uint32_t periods) {
  const std::uint32_t total_ms = periods * mk66::blink::period_ms(pulses);
  for (std::uint32_t t = 0; t < total_ms; t += 10) {
    led_from_sram_l(mk66::blink::led_on(pulses, t));
    mk66::boot::delay_ms(10);
  }
  led_from_sram_l(false);
}

// Is this the record the drill build writes: its own module id, a UsageFault whose cause is an undefined instruction, a
// stacked PC inside flash? Anything else is a real crash.
bool is_drill_record(const mk66::crash::CrashRecord& r) {
  return mk66::crash::module_of(r.context) == kDrillModule && r.exception == kExceptionUsageFault &&
         (r.cfsr & kCfsrUndefinstr) != 0u && r.pc < mk66::mem::kFlashSize &&
         mk66::crash::stage_of(r.context) == mk66::crash::Stage::Running;
}

}  // namespace

int main() {
  // The log first: the SDRAM has been tested (the test overwrites all of it), so its `diag_log` region can take the ring now. What the
  // reset sequence did before this point is logged from what it recorded.
  mk66::console::init_log();
  mk66::console::log("boot: main reached");
  mk66::console::log_hex("boot: reset sources", mk66::boot_info().reset_sources);
  bool record_seen = false;
  // 0: the heartbeat. A fault found at boot or while running (blink.h kAppFailure, kDisplayStalled) replaces it for good: the OS
  // keeps running, the display is held dark, and the code is shown over and over so it cannot be taken for a quiet black screen.
  std::uint8_t fault_code = 0;
  if (const mk66::crash::CrashRecord* record = mk66::boot_crash_record()) {
    record_seen = true;
    mk66::console::note_boot_crash(record);  // `diag crash` keeps it for this run; the noinit copy is erased below
    mk66::console::log_hex("boot: previous run crashed, exception", record->exception);
    play_code(is_drill_record(*record) ? mk66::blink::kDrillRoundTrip : mk66::blink::kCrashSeen, 2);
    mk66::clear_crash_record();
  }

  // ---- Display (OS-007c) -------------------------------------------------------------------------------------------
  // SPI2 and the eDMA on, the FT812 reset and brought up (bounded, retried), the DMA path self-tested, the backlight left
  // off until the app has a frame on screen. A display that does not come up is a blink code and a headless run: the
  // heartbeat below still tells that the image is alive.
  mk66::set_module(kDisplayModule);
  // The eDMA's global configuration (DMA_CR, every DCHPRI) was written once by boot::dma_init() in the reset sequence, before this
  // point and before the audio engine starts; the display's two channels only program their own descriptors.
  const mk66::display::device::BringUpResult shown = mk66::display::device::bring_up();
  mk66::console::log(shown.up ? "boot: display up" : "boot: display FAILED, headless");
  if (shown.up) {  // OS-HW1: logged at once, before the app's init and the polled-path blink code below take their seconds
    mk66::console::log(shown.dma ? "boot: display dma ok" : "boot: display dma FAILED, polled path");
  }
  if (!shown.up) {
    mk66::console::note_boot_fault(mk66::console::kFaultDisplay);
  }
  if (shown.up) {
    if (!mk66::appimg::init(mk66::display::device::display())) {
      // The safe state for an app that cannot start (its bitmaps did not go up): the display is held dark, never a lit black
      // screen that looks like a quiet success, and the LED shows kAppFailure in place of the heartbeat for good.
      mk66::display::device::display().hold_dark();
      fault_code = mk66::blink::kAppFailure;
      mk66::console::note_boot_fault(mk66::console::kFaultApp);
      mk66::console::log("boot: app FAILED, display held dark");
    }
    if (!shown.dma) {
      // Up, but the byte-wide DMA write did not check out: the polled path carries it. Played AFTER appimg::init(): the code blocks for
      // about 3.6 s, longer than the backlight's own fallback (display.h kBacklightFallbackMs, 2 s from the display coming up), so
      // before the boot frame is committed it would let the ramp start on a black screen (OS-003f). Now the boot screen is up, and
      // the idle hook keeps the display serviced while the code plays.
      play_code(mk66::blink::kDisplayPolled, 1);
    }
  } else {
    play_code(mk66::blink::kDisplayFailure, 2);
  }
  // OS-007d: a key scanner that did not answer at boot (INPUT TEST says which); the OS runs with what did.
  if (mk66::appimg::input_degraded()) {
    play_code(mk66::blink::kInputDegraded, 1);
  }
  mk66::set_module(0);

  // ---- Watchdog re-enable point ----------------------------------------------------------------------------------
  // The reset sequence switched the watchdog off. This is where a later slice turns it back on, with
  // the idle loop below as the place that feeds it. It is deliberately not done in the skeleton: the watchdog's clock
  // source and timeout units have not been checked against a reference manual or a unit here, and a wrong guess would be a
  // reset loop on the very first boot. README.md lists what to verify first (watchdog reset, then the crash record
  // surviving it, ADR 0002 validation item 6).

  // ---- Audio (OS-005a): the codec is brought up muted, the I2S/DMA path runs, the engine renders silence. start() never
  // unmutes; a failure at any step leaves the outputs muted. OS-006b: the image does NOT stop on that failure any more. The console is
  // how a failure is diagnosed, and the likeliest first-boot failure is exactly this one (the codec's I2C link, the MCLK divider): so the
  // code is latched for the LED like the display's, the failure is logged and listed by `diag stage`, and USB comes up below. The audio
  // status polling and the tone of the HW-A image are skipped (no block clock is running to watch).
  bool audio_up =  // `diag audio start` can bring it up later (OS-HW1): the loop below notices
      mk66::audio_device().start(mk66::audio_engine().callback()) == o3os::hal::audio::Device::Result::Ok;
  if (audio_up) {
    mk66::console::log("boot: audio started, outputs muted");
  } else {
    fault_code = mk66::blink::kAudioFailure;
    mk66::console::note_boot_fault(mk66::console::kFaultAudio);
    mk66::console::log_dec("boot: audio FAILED, outputs muted, result", mk66::audio_last_result());
    mk66::console::log_audio_clock_fault("boot: audio", mk66::audio_clock_fault());  // OS-HW1: which wait, MCR and MDR
  }
  // OS-BUG-PADS: the audio start is over, worked or not: the pad bus (I2C2) may open a second after the board's rail came up, and the pads are brought up from the loop below.
  mk66::appimg::input_audio_started();

  // ---- SD card (OS-007e): READ ONLY. After the audio path runs (OS-USBAUDIO: and BEFORE the USB port is started, see below). The card is initialised and its FAT volume mounted;
  // the display is serviced from the idle hook during the card's waits (every wait is bounded: a card that does not answer costs the init, under 3 s, plus up to 0.5 s for each read call that fails). Then,
  // the project of the card's Workspace folder (Workspace/project.mt) is loaded, in
  // slices of the main loop (appimg::pass); `diag load <folder>` loads another. The instruments' sample PCM follows the project (OS-007f), in the same slices.
  mk66::set_module(kStorageModule);
  mk66::storage::device::init();
  // OS-USBDISK (b), the boot disk (chord Shift + F1 + F7): the card is the computer's. Nothing is mounted, no settings or project are read, the slot is not watched; the hand-over happens below,
  // after the USB port started with the DISK set (4.2: the layout first, the owner last).
  const bool boot_disk = mk66::appimg::boot_disk();
  if (boot_disk) {
    mk66::console::log("boot: USB DISK mode: the card is not mounted");
  }
  o3os::hal::storage::Status sd_status = o3os::hal::storage::Status::NotMounted;
  if (!boot_disk) {
    mk66::appimg::service_while_blocked(true);
    sd_status = mk66::storage::device::mount();
    mk66::appimg::service_while_blocked(false);
    mk66::storage::device::watch_begin();  // OS-SDSWAP: from here the main loop (appimg::pass) follows the slot: a card taken out is released, one put in is mounted; the boot's own result above is unchanged
  }
  if (boot_disk) {
    // (nothing to read: the defaults stand and no project loads)
  } else if (sd_status == o3os::hal::storage::Status::Ok) {
    const o3os::hal::storage::VolumeInfo volume = mk66::storage::device::storage().volume();
    char line[56];
    o3os::audio::diag::Text text(line, sizeof line);
    text.str("boot: sd mounted, ").str(o3os::hal::storage::fs_kind_name(volume.kind)).str(", ").dec(volume.capacity_mb).str(" MB");
    mk66::console::log(line);
    mk66::appimg::load_device_settings();  // OS-007n: Settings/device.o3st (read only), before the project: the settings are the device's, not the project's
    // OS-QAFIX-E-FILE-10 (maintainer, 2026-10-22): the project that was open last comes back at power-on; none that is still on the card: the Workspace, as before. A last project that is damaged is not
    // replaced by the Workspace: the load refuses it (OS-QAFIX-4), the document stays as it booted and the strip says why.
    bool is_dir = false;
    if (mk66::appimg::boot_open_last_project()) {
      // opened through the app: its strip tells the end
    } else if (mk66::storage::device::storage().exists("Workspace/project.mt", &is_dir) == o3os::hal::storage::Status::Ok && !is_dir) {
      const mk66::appimg::LoadStart started = mk66::appimg::start_project_load("Workspace");
      mk66::console::log(started == mk66::appimg::LoadStart::Started ? "boot: loading Workspace" : mk66::appimg::load_start_name(started));
    } else {
      mk66::console::log("boot: sd has no Workspace/project.mt: nothing to load");
    }
  } else {
    char line[56];
    o3os::audio::diag::Text text(line, sizeof line);
    text.str("boot: sd NOT mounted: ").str(o3os::hal::storage::status_name(sd_status));
    mk66::console::log(line);
    if (mk66::storage::device::storage().card_present()) {  // a card is in and did not mount: a fault (`diag stage` lists it); an empty slot is not
      mk66::console::note_boot_fault(mk66::console::kFaultSd);
    }
  }
  mk66::set_module(0);

  // ---- USB (OS-006b): MIDI and the CDC `diag` console, and since OS-USBAUDIO the audio device. OS-USBAUDIO moved it here from before the card: the device setting USB AUDIO (default On: the unit
  // comes up as the audio device, no `usb mode audio`) is in Settings/device.o3st, so the file is read first and the port enumerates ONCE, in the mode the setting says, with the saved USB RETURN
  // (a card that is absent or unreadable gives the defaults: AUDIO). Still after the audio path runs: enumeration happens at the USB priority, below the audio DMA interrupt, and must never be able
  // to delay its bring-up. The cost of the order: the host sees the unit when the card has been read (tens of ms; for a card that does not answer, the init, under 3 s, plus up to 0.5 s per failing read call) instead of before, and a hang
  // or crash inside the mount, which every wait bounds, no longer leaves a console enumerated (a crash loop there gets none: remove the card; the crash record is read on the next boot's console, as before). A failure (the table misaligned, the
  // descriptors invalid) leaves the pull-up off and says why in the log; the OS runs without USB.
  mk66::set_module(kUsbModule);
  if (!mk66::appimg::start_usb()) {
    mk66::console::log("boot: USB NOT started");
  } else if (boot_disk) {
    mk66::appimg::boot_disk_open();  // the owner flips last: the card goes to the computer and is started for it
  }
  mk66::set_module(0);

  const mk66::SysTickClock& clock = mk66::system_clock();
  mk66::OutputGate output_gate(kToneSettleBlocks);  // OS-007g: where the audio path last faulted; the outputs open after a clean run (OS-OUTON: at boot)
#if !O3OS_AUDIO_TEST
  mk66::OpenRetry open_retry;   // OS-OUTON: what a failed unmute costs (a backoff, a cap on the boot's own attempts); the loop never stops for it
  bool unmute_fault = false;    // the LED code and the boot fault below are this open's (a retry that works takes them back)
#endif
  std::uint32_t last_second = 0;
  std::uint32_t last_audio_poll = 0;
  std::uint32_t last_boundaries = 0;
  std::uint32_t last_progress_ms = static_cast<std::uint32_t>(clock.uptime_ms());
  std::uint8_t pattern = 0;  // sticky once an audio code is chosen: a fault never un-happens
  bool pads_code_done = false;         // OS-BUG-PADS: the pad scanner's blink code 13 has been started (it plays once)
  std::uint32_t pads_code_from = 0;    // and when
#if O3OS_AUDIO_TEST
  bool tone_started = false;
  std::uint32_t tone_started_ms = 0;
#if O3OS_AUDIO_SWEEP
  std::uint32_t last_script_ms = 0;
#endif
#if O3OS_AUDIO_TEST_STALL
  bool stall_done = false;
#endif
#endif
  // OS-MAINGUARD: from here the loop beats once per pass (audio_main_pass), so a render that runs back to back and starves it sheds blocks until a pass completes
  // (audio/pipeline.h). Armed for every image that runs this loop, the O3OS_AUDIO_TEST ones included.
  mk66::audio_arm_starve_guard();
  for (;;) {
    asm volatile("wfi");  // SysTick wakes the core every millisecond; a DMA completion wakes it sooner
    mk66::console::service();  // the `diag` console: what the host typed, a stream's next lines, the output
    const std::uint8_t fault = mk66::appimg::pass();
    mk66::audio_main_pass();  // the beat: a whole pass (console, UI, keys, SD jobs) has run
    if (fault != 0) {
      if (fault != fault_code) {
        mk66::console::note_boot_fault(fault == mk66::blink::kAppFailure ? mk66::console::kFaultApp : mk66::console::kFaultDisplay);
        mk66::console::log(fault == mk66::blink::kAppFailure ? "app FAILED, display held dark" : "display stopped swapping frames, held dark");
      }
      fault_code = fault;  // latched: nothing in this slice recovers a display that gave up
    }
    const auto ms = static_cast<std::uint32_t>(clock.uptime_ms());
#if !O3OS_AUDIO_TEST
    // OS-OUTON: the outputs open at boot, once (mk66::open_wanted): the SD card is mounted (above, before this loop) and the boot project load has ended, so that the codec's
    // unmute, which blocks for about 0.2 s with the display serviced, never sits on the card's reads or the loader's slices; and the audio path has run clean for the same
    // 0.6 s the tone image waits for (OutputGate, which does NOT wait for the load by itself: with no fault since the start it is ready from boundary 400, seconds ago). The
    // amplifier key's click is therefore at power-on, and a Play, a live note or a Song Play never waits. A request that found them closed (OS-005d: the fallback for an audio
    // start that came late, `diag audio start` after a boot failure, an unmute that failed) goes through the same gate, loading or not. Never while the firmware update path
    // holds the card or waits for the bootloader (its quiesce muted the outputs and cleared the open; the open comes back when that ended without a hand-over).
    // A failed unmute does not stop the unit (the open now runs at every boot, and the codec's I2C bus is shared with the button scanner): it is logged, the LED shows the
    // audio code and `diag stage` the fault, the display and the console go on, and the next attempt waits 2 s (OpenRetry), the boot's own attempts end after three.
    const bool open_asked = mk66::appimg::audio_open_pending();
    // OS-POWER P1: once the shutdown has begun (the power-off latch, set at its stage A and never cleared) nothing reopens the outputs, whatever else says so (4.6).
    if (!boot_disk &&
        mk66::open_wanted(audio_up, mk66::appimg::audio_outputs_open(), open_asked, mk66::appimg::project_loading(), mk66::appimg::firmware_update_active(), mk66::appimg::power_off_latched()) &&
        open_retry.ready(open_asked, ms)) {
      const o3os::audio::StatusSnapshot open_snap = mk66::audio_status().snapshot();
      output_gate.observe(open_snap.boundaries, open_snap.underruns, open_snap.hw_faults);
      if (output_gate.ready(open_snap.boundaries)) {
        mk66::appimg::service_while_blocked(true);
        const bool opened = mk66::audio_device().unmute();  // (a failure muted them again before it returned)
        mk66::appimg::service_while_blocked(false);
        if (opened) {
          open_retry.succeeded();
          if (unmute_fault) {
            unmute_fault = false;
            mk66::console::clear_boot_fault(mk66::console::kFaultAudio);
            if (fault_code == mk66::blink::kAudioFailure) {
              fault_code = 0;
            }
          }
          char line[56];
          o3os::audio::diag::Text text(line, sizeof line);
          text.str("audio: outputs open, underruns ").dec(open_snap.underruns).str(" faults ").dec(open_snap.hw_faults);
          mk66::console::log(line);
          mk66::appimg::audio_opened();  // a Play or a live note that was waiting starts now (none at boot)
        } else {
          open_retry.failed(ms);
          unmute_fault = true;
          mk66::console::note_boot_fault(mk66::console::kFaultAudio);
          fault_code = mk66::blink::kAudioFailure;
          mk66::console::log_dec("audio: unmute FAILED, failures", open_retry.failures());
          if (open_retry.gave_up() && !open_asked) {
            mk66::console::log("audio: unmute gave up, a Play retries");
          }
        }
      }
    }
#endif
    if (!audio_up && mk66::audio_running()) {
      // OS-HW1: `diag audio start` got the audio path going after the boot failure (it cleared the console's fault flag). From here on it is
      // as if it had started at boot: the status polling runs, the boot fault's LED code is gone.
      audio_up = true;
      if (fault_code == mk66::blink::kAudioFailure) {
        fault_code = 0;
      }
      last_audio_poll = ms;
      last_boundaries = 0;
      last_progress_ms = ms;
    }
    if (audio_up && ms - last_audio_poll >= kAudioPollMs) {
      last_audio_poll = ms;
      const o3os::audio::StatusSnapshot snap = mk66::audio_status().snapshot();
      output_gate.observe(snap.boundaries, snap.underruns, snap.hw_faults);
      if (snap.boundaries != last_boundaries) {
        last_boundaries = snap.boundaries;
        last_progress_ms = ms;
      }
      // The block clock stopping (the DMA halted on an error, the clocks died) raises no interrupt and counts nothing: the boundary
      // count standing still is how it shows. OS-POWER P1: not once the shutdown has stopped the audio path on purpose (the power-off latch): the counter stands still by design, the verdict
      // and the pattern are not taken, and the LED keeps its heartbeat (a fault "is final", so one wrongly latched here would show the 11-pulse code until the restart).
      const bool powering_off = mk66::appimg::power_off_latched();
      const bool stalled = !powering_off && ms - last_progress_ms >= kAudioStallMs;
      const std::uint8_t verdict = stalled ? mk66::blink::kAudioFault : audio_pattern(snap);
      if (!powering_off && verdict != 0 && pattern != mk66::blink::kAudioFault) {
        pattern = verdict;  // a fault is final; "over budget" stays until a fault replaces it
      }
#if O3OS_AUDIO_TEST
      if (!tone_started && snap.boundaries >= kToneSettleBlocks && snap.underruns == 0u && snap.hw_faults == 0u) {
        tone_started = true;
        if (mk66::audio_device().unmute() && mk66::audio_engine().set_tone(true)) {  // unmute blocks about 0.2 s: codec ramps, amplifier key last
          tone_started_ms = static_cast<std::uint32_t>(clock.uptime_ms());
#if O3OS_AUDIO_SWEEP
          last_script_ms = tone_started_ms;
#endif
        } else {
          mk66::boot::fail(mk66::blink::kAudioFailure);
        }
      }
#if O3OS_AUDIO_SWEEP
      if (tone_started && ms - last_script_ms >= kScriptStepMs) {
        last_script_ms = ms;
        graph_script(ms - tone_started_ms);
      }
#endif
#if O3OS_AUDIO_TEST_STALL
      if (tone_started && !stall_done && ms - tone_started_ms >= kStallAfterMs) {
        stall_done = true;
        mk66::audio_engine().request_test_stall((o3os::hal::audio::kCyclesPerBlock * 7u) / 2u);
      }
#endif
#else
      (void)kToneSettleBlocks;
      (void)kStallAfterMs;
#endif
    }
    // One LED, two slices: a hard audio fault (kAudioFault) is shown first, then a display or app safe-state code (fault_code), then
    // the audio verdict (kAudioOverBudget or the heartbeat). Each is sticky; the status words tell the rest.
    const std::uint8_t shown_code = pattern == mk66::blink::kAudioFault ? pattern : (fault_code != 0 ? fault_code : pattern);
    // OS-BUG-PADS: the pad scanner is brought up after the audio start (a second after the board's rail at the earliest), so its verdict comes from here, not from the boot. Once the pad
    // scanner has refused its first configurations and never answered, code 13 is shown for ONE period, from its first flash, in place of the heartbeat only (a code already shown for
    // a fault keeps the LED); nothing waits for it, and the retries behind it go on for ever. A pad scanner that answers later brings the pads online with no restart.
    if (!pads_code_done && mk66::appimg::pads_gave_no_answer()) {
      pads_code_done = true;
      pads_code_from = ms;
      mk66::console::log("input: pad scanner did not answer, blink code 13 once, retrying every second");
    }
    if (pads_code_done && shown_code == 0 && ms - pads_code_from < mk66::blink::period_ms(mk66::blink::kInputDegraded)) {
      led_from_sram_l(mk66::blink::led_on(mk66::blink::kInputDegraded, ms - pads_code_from));  // a fault code: shows whatever the STATUS LED row says
    } else {
      // OS-STATUSLED: led_on_status keeps a fault code (shown_code >= 1) blinking whatever the STATUS LED row says, and with the
      // heartbeat off (blink::g_heartbeat_on false) the heartbeat (shown_code 0) and the over-budget code 12 (no fault) keep the LED dark.
      led_from_sram_l(mk66::blink::led_on_status(shown_code, ms, mk66::blink::g_heartbeat_on));
    }
    if (ms / 1000u != last_second) {
      last_second = ms / 1000u;
      g_heartbeat = g_heartbeat + 1u;
    }
#if O3OS_FAULT_DRILL
    // The drill: one deliberate fault, to prove the crash record round trip on hardware. Once per power-up, and never in a
    // run that started with a record (that run is the readback).
    if (!record_seen && ms >= kDrillAfterMs) {
      mk66::set_module(kDrillModule);
      asm volatile(".inst.n 0xde00");  // UDF #0: UsageFault, UFSR.UNDEFINSTR
    }
#else
    (void)record_seen;
    (void)kDrillAfterMs;
#endif
  }
}
