// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `diag` console of the MK66 image (OS-006b): the shell of usb/diag_shell.h on the CDC port, and the commands whose data is the
// platform's. Everything here runs in the main loop; the shell never blocks (its output is a ring that drops its oldest bytes when the host
// does not read) and a command that keeps talking (`input`) is a stream that says a bounded amount per pass.
//
// What each command reads (nothing here writes hardware but `reset`):
//   help       the table
//   version    the build identity (CMake writes it per build), the USB identity and serial
//   stage      the crash module's stage and module words, uptime, reset sources
//   cpu        audio::AudioStatus through audio::diag::format_cpu (ADR 0013 D8); `cpu reset` zeroes the per-run figures (the maxima and the fault counters: OS-PERF3, the one
//              thing this command writes: AudioStatus::reset_peaks); it also asks the module instances' min / mean / max (`mod`, OS-303PERF) to start again, and turns them ON (they are off from boot);
//              `cpu perf off` turns them off again (the governor's figures carry three counter reads a segment while they are on)
//   mem        the memory map and budgets through audio::diag::format_mem, with the image's own hot-code and SRAM_U figures
//   crash      the record the previous run left (kept before main erased it)
//   usb        the driver's counters and the control pipe's
//   log        the ring boot code and drivers write to
//   input      the events the app polls, live (and `input stats`)
//   mix        the audio graph's meters through audio::diag::format_mix (OS-005b), with the master limiter's gain reduction (OS-LIMIT)
//   mon        the line-in monitor's level (OS-005b): the one command that writes the engine, as a ParamChange from the main loop (the ring's
//              producer), never a register
//   peek       OS-HW1 bring-up: aligned 32-bit reads of the modules the image has clocked on (usb/mem_access.h): a read, nothing written
//   poke       OS-HW1: one 32-bit write, I2S0's registers only (the audio clock bring-up is what it is for)
//   audio      OS-HW1: `audio start` calls the audio device's start() again from here (the main loop) when it failed at boot
//   rot        OS-HW1: REG_ROTATE written through the display driver, the display idle (not saved)
//   sd         OS-007e: the SD card (slot, mount, identity, speed, counters); `sd mount` initialises and mounts it again. Reads the card.
//   ls         OS-007e: a directory of the card, a pager. A read.
//   load       OS-007e: starts loading an OG project folder of the card into the document (appimg::start_project_load): reads the card, replaces the
//              project in memory and (OS-007f) loads its samples into the SDRAM arena, with the transport stopped; `sd` shows the result and the samples'
//              (`pcm:`) line. Nothing is ever written to the card (hal/storage.h has no write call).
//   inst       OS-007h: what the voices were given for one instrument (app::describe_instrument): window start and length, loop, root, rate, volume,
//              a 16-bit checksum of its first 64 samples. A memory read.
//   mod        OS-012a: the SD modules (ADR 0002): `mod` lists the memory in use, the loaded modules, the bound instruments and the quarantine; `mod load <name>` reads
//              modules/{instruments,effects,apps}/<name>.tmod (the first directory that has the stem: loader.cpp; an app is loaded and checked here, never started: launch it from MODULES with F1) into the slab and the module region (a slice a pass; the result is in `log`); `mod bind <1..64> <name>` gives instrument n an
//              instance of a loaded module (until the next project load); `mod note <n> <note> [vel]` / `mod off <n>` play it on track 16 (the outputs must be open: press Play
//              once); `mod set <n> <id> <value>` changes one of its parameters. Reads the card (load); writes no register.
//   radio      OS-RAD1: the FM radio driver's state, step, last failure (step/cause), last I2C result, registers as last read and pin levels; `radio retry` runs the
//              power-up again from the reset (the one thing it writes: the receiver, over the control bus, from the main loop)
//   voices     OS-VOX: the sampler voices' in use, peak and steal count, as the render published them (audio::VoicePool::voice_snapshot, lock-free), through
//              audio::diag::format_voices; OS-ODD11 slice 2 adds `advanced`, the spans of muted voices the closed form took since boot. A memory read.
//   reset      mutes the outputs (OS-OUTON: the amplifier key, then the codec), then SYSRESETREQ, after the output has had time to leave. Nothing else.
//   key, pad, enc   OS-HIL: input injection into the scanner's own event queue (usb/hil_inject.h): what the app sees is a finger's events; each is logged as injected;
//              refused while an `fw` update is armed or running. `enc accel [off | <t1> <t2> [gap_ms]]` (OS-AUDIT-28) is no injection: it shows and sets the numbers of the
//              encoder's acceleration in lists (app/enc_accel.h), runtime only
//   state      OS-HIL: the app's state as one key=value line (app/state_line.h). A read.
//   shot       OS-HIL: the screen as framed base64 lines (usb/shot_stream.h): CMD_SNAPSHOT2 into the spare RAM_G, read back over the SPI a few lines a pass of the main loop.
//              The one command that makes the FT812's coprocessor run, and the one that writes RAM_G (the spare region only).
//   leds       OS-HIL: the pad LED levels the driver holds, one line. A read.
//   bench      OS-PERF3: `bench <kernel|all> [blocks]` times a block of a render kernel with the cycle counter (src/bench.cpp, bench_report.h has the line); the main loop is busy for the
//              run (blocks x ~1.5 ms). It plays track 16's voice and the TB-303 instance for a moment and builds effect instances of its own on the stack: stop the transport first.
//   cache      OS-PERF3: the LMEM code cache (lmem.h): `cache` shows the state, `cache on|off` switches it (PCCCR only), `cache inv` invalidates it, `cache check` is the coherence check of the cached SDRAM alias (it writes a scratch stretch of the unused ui_cache region). For A/B measurements; writes LMEM_PCCCR and that scratch, nothing else.
//   power      OS-POWER P1: the Power switch (PTC3): `power` the pin in the three pulls, the hold's state and counters, the shutdown's stages; `power press|release|pin` feed the hold in place of the
//              pin (a scripted hold needs no finger); `power off [dry]` runs the shutdown (stages 0 to F: USB off, the lights out, then, after the release of Power, the VLPR off of OS-POWER P2: dark, PTC3 polled, a press
//              boots the unit; `dry` ends in a restart instead, for the first run on the unit). Once the shutdown has begun the shell refuses every command but the reads (help, version, stage, log, crash, power, state, cpu, mem): `ERR powering off`.
//   fw         OS-006c: the firmware update path (update/fw_service.h): `fw` lists Firmware/ and the write state, `fw put` receives a .ptf, `fw rm` removes a dev
//              image, `fw arm` / `fw update` hand over to the bootloader (the one place that writes `_fwinfo` and drives the bootloader request pin: fw_host_mk66.cpp).
//              The only command that writes the card (OS-FWANY: any writable FAT card with a Firmware/ folder; no marker file).
#include <o3os/platform/mk66/usb/console.h>

#include "../../src/internal.h"

#include <o3os/audio/diag.h>
#include <o3os/core/ring.h>
#include <o3os/platform/mk66/audio.h>
#include <o3os/platform/mk66/diag_report.h>
#include <o3os/platform/mk66/display/device.h>
#include <o3os/platform/mk66/input/device.h>
#include <o3os/platform/mk66/mem_map.h>
#include <o3os/platform/mk66/platform.h>
#include <o3os/platform/mk66/power/power_device.h>
#include <o3os/platform/mk66/storage/device.h>
#include <o3os/platform/mk66/storage/ls_command.h>
#include <o3os/platform/mk66/storage/sd_text.h>
#include <o3os/platform/mk66/usb/command_table.h>
#include <o3os/platform/mk66/update/fw_device.h>
#include <o3os/platform/mk66/usb/diag_text.h>
#include <o3os/platform/mk66/usb/hil_inject.h>
#include <o3os/platform/mk66/usb/input_hold.h>
#include <o3os/platform/mk66/usb/shot_stream.h>
#include <o3os/platform/mk66/usb/log_command.h>
#include <o3os/platform/mk66/usb/mem_access.h>
#include <o3os/platform/mk66/usb/soc_facts.h>
#include <o3os/platform/mk66/usb/usb0.h>
#include <o3os/audio/stem_map.h>
#include <o3os/usb/diag_log.h>
#include <o3os/usb/diag_pump.h>
#include <o3os/usb/diag_shell.h>
#include <o3os/usb/identity.h>

#include "o3os_build_info.h"

#include <cstdint>
#include <cstring>
#include <new>

// Linker symbols (mk66.ld): the hot code and the SRAM_U the image uses.
extern "C" {
extern std::uint32_t __ramfunc_l_start[];
extern std::uint32_t __ramfunc_l_end[];
extern std::uint32_t __tapmem_l_start[];
extern std::uint32_t __tapmem_l_end[];
extern std::uint32_t __noinit_start[];
extern std::uint32_t __bss_end[];
}

namespace o3os::platform::mk66::console {
namespace {

namespace shell = o3os::usb::diag;

// ---- The log ---------------------------------------------------------------------------------------------------------------
shell::LogRing g_log;
constexpr std::uint32_t kLogRecords = mem::kDiagLogSize / sizeof(shell::LogRecord);
static_assert(sizeof(shell::LogRecord) == 64 && (kLogRecords & (kLogRecords - 1u)) == 0u && kLogRecords * sizeof(shell::LogRecord) == mem::kDiagLogSize,
              "the diag_log region holds a power-of-two number of 64-byte records");
static_assert(kLogMax == kLogRecords, "`log` can show the whole ring");

// ---- The banner ---------------------------------------------------------------------------------------------------------------
constexpr char kBanner[] = "Track3r diag - o3os " O3OS_BUILD_LABEL " - 'help' lists the commands";

// ---- Input tap ----------------------------------------------------------------------------------------------------------------
// Both ends run in the main loop (the app's poll and the console's stream), so the ring's single-producer, single-consumer rule holds.
class TeeInput final : public hal::Input {
 public:
  explicit TeeInput(hal::Input& source) noexcept : source_(source) {}
  bool poll(core::InputEvent& out) noexcept override {
    if (locked_) {
      while (source_.poll(out)) {  // OS-POWER P1 (D2): the shutdown has begun: the keys, pads and the encoder are read and dropped, the app is given nothing
      }
      return false;
    }
    if (!source_.poll(out)) {
      return false;
    }
    (void)ring_.push(out);  // a full ring drops the newest: nobody is reading
    return true;
  }
  hal::InputStats stats() const noexcept override { return source_.stats(); }
  bool pop(core::InputEvent& out) noexcept { return ring_.pop(out); }
  void lock() noexcept {  // never undone: a restart ends the run
    source_.pass(false);  // (a hold ends first: the locked poll reads the kept releases and the queue dry)
    locked_ = true;
  }
  void hold(bool held) noexcept { source_.pass(held); }  // OS-SCREENSHOT (spec Q4), once a pass

 private:
  InputHold source_;  // between the key scan and the app: the screenshot's capture takes the presses (usb/input_hold.h)
  core::InputQueue ring_;
  bool locked_ = false;  // main loop only
};
alignas(TeeInput) unsigned char g_tee_storage[sizeof(TeeInput)];
TeeInput* g_tee = nullptr;

// ---- Crash record of the previous run -------------------------------------------------------------------------------------------
crash::CrashRecord g_boot_crash;
bool g_have_boot_crash = false;

// Start-up faults main went on after (boot_faults.h): a mask, written in the main thread only.
std::uint32_t g_boot_faults = 0;

// ---- Output ----------------------------------------------------------------------------------------------------------------------
void sink_write(void*, const char* data, std::size_t n) { usb0::cdc_writer().write(data, n); }
void sink_flush(void*) { usb0::cdc_writer().flush(); }
std::size_t sink_room(void*) { return usb0::cdc_writer().room_bytes(); }

constexpr std::size_t kTextBuffer = 1536;  // the longest reply is `usb`, about 1.2 KB

void send(shell::Shell& sh, std::size_t n, const char* buffer) { sh.out().text(buffer, n); }

bool same(const char* a, const char* b) {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// ---- Commands ------------------------------------------------------------------------------------------------------------------------
void cmd_help(shell::Shell& sh, const shell::Args&) {
  for (std::size_t i = 0; i < sh.command_count(); ++i) {
    const shell::Command& c = sh.commands()[i];
    sh.out().text(c.name);
    std::size_t len = 0;
    while (c.name[len] != '\0') {
      ++len;
    }
    for (; len < 8u; ++len) {  // the help texts line up
      sh.out().chr(' ');
    }
    sh.out().text(c.help).nl();
  }
}

void cmd_version(shell::Shell& sh, const shell::Args&) {
  char buf[kTextBuffer];
  send(sh, format_version(O3OS_BUILD_LABEL, o3os::usb::kVendorId, o3os::usb::kProductId, o3os::usb::kDeviceBcdStandard, usb0::serial(), buf, sizeof buf), buf);
}

void cmd_stage(shell::Shell& sh, const shell::Args&) {
  char buf[kTextBuffer];
  const BootInfo& boot = boot_info();
  send(sh, format_stage(boot::load_context(), static_cast<std::uint32_t>(system_clock().uptime_ms()), boot.reset_sources, boot.crash_record_found, g_boot_faults, buf, sizeof buf), buf);
}

void cmd_cpu(shell::Shell& sh, const shell::Args& a) {
  if (a.argc == 2 && same(a.argv[1], "reset")) {
    audio_cpu_reset();  // OS-PERF3: the maxima and the fault counters back to zero, for a measurement of one run
    if (audio::ModuleVoices* mv = appimg::module_voices()) {
      mv->reset_perf();  // OS-303PERF: and the module instances' min / mean / max (`diag mod`) start again with it
    }
    if (o3os::audio::AppTap* const tap = audio_engine().graph().app_tap()) {
      tap->reset_cost();  // OS-H2a: the render zeroes the tap's cost figures at its next block
    }
    sh.out().text("cpu: peaks and fault counters reset\n");
    return;
  }
  if (a.argc == 3 && same(a.argv[1], "perf") && same(a.argv[2], "off")) {
    if (audio::ModuleVoices* mv = appimg::module_voices()) {
      mv->stop_perf();  // OS-303PERF: the render reads no extra stamps again; the governor's figures are the ones without the split
    }
    sh.out().text("cpu: module perf off (cpu reset starts it again)\n");
    return;
  }
  if (a.argc != 1) {
    sh.out().text("usage: cpu [reset | perf off]\n");
    return;
  }
  char buf[kTextBuffer];
  send(sh, o3os::audio::diag::format_cpu(audio_status().snapshot(), hal::audio::kCyclesPerBlock, buf, sizeof buf), buf);
  const o3os::audio::Pipeline& pipe = audio_pipeline();
  send(sh, o3os::audio::diag::format_shed(pipe.shed_blocks(), pipe.shed_late(), pipe.shed_runs(), pipe.shed_longest(), pipe.starve_blocks(), buf, sizeof buf), buf);  // OS-MAINGUARD: since boot or `cpu reset` (the render zeroes them)
  if (const o3os::audio::AppTap* const tap = audio_engine().graph().app_tap()) {  // OS-H2a: the apps' audio tap, when an app has used it since boot or `cpu reset`
    const o3os::audio::AppTap::Cost c = tap->cost();
    if (c.blocks != 0u || tap->source() != o3os::audio::AppTap::Source::Off) {
      char line[128];
      const std::size_t n = o3os::audio::diag::format_app_tap(static_cast<unsigned>(tap->source()), c.blocks, c.last, c.sum, c.max, hal::audio::kCpuHz, line, sizeof line - 1);
      line[n] = '\n';
      sh.out().text(line, n + 1);
    }
  }
}

// `guard [n <1..255>]` (OS-MAINGUARD tuning): the main-loop guard's starvation window N, the over-budget renders in a row before the render sheds. With no argument it prints the window;
// `guard n 12` sets it (the render reads it at its next block; it is not saved, a reset goes back to the default). `cpu` shows it at the end of its guard line.
void cmd_guard(shell::Shell& sh, const shell::Args& a) {
  std::uint32_t n = 0;
  if (a.argc == 3 && same(a.argv[1], "n") && parse_dec(a.argv[2], o3os::audio::kStarveBlocksMax, n) && n != 0u) {
    audio_set_starve_blocks(n);
  } else if (a.argc != 1) {
    sh.out().text("usage: guard [n <1..255>]\n");
    return;
  }
  char buf[kTextBuffer];
  const o3os::audio::Pipeline& pipe = audio_pipeline();
  send(sh, o3os::audio::diag::format_shed(pipe.shed_blocks(), pipe.shed_late(), pipe.shed_runs(), pipe.shed_longest(), pipe.starve_blocks(), buf, sizeof buf), buf);
}

void cmd_mem(shell::Shell& sh, const shell::Args&) {
  MemTables tables;
  const std::uint32_t hot = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__ramfunc_l_end) - reinterpret_cast<std::uintptr_t>(__ramfunc_l_start));
  const std::uint32_t used = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__bss_end) - reinterpret_cast<std::uintptr_t>(__noinit_start));
  const std::uint32_t taps = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__tapmem_l_end) - reinterpret_cast<std::uintptr_t>(__tapmem_l_start));
  fill_mem_tables(tables, hot, used, taps);
  char buf[kTextBuffer];
  send(sh, o3os::audio::diag::format_mem(tables.report, buf, sizeof buf), buf);
}

void cmd_crash(shell::Shell& sh, const shell::Args&) {
  char buf[kTextBuffer];
  send(sh, format_crash(g_have_boot_crash ? &g_boot_crash : nullptr, buf, sizeof buf), buf);
}

// ---- `usb audio | mode | profile | return | map` (OS-006d): the AUDIO mode's view and controls. The mode, the profile and the map are RUNTIME state here: a boot
// comes up in the mode the device setting USB AUDIO says (OS-USBAUDIO; default AUDIO) with the factory map, and the profile and the map are the project's (USBS) once saved. `usb mode` is runtime only:
// it does NOT write USB AUDIO (the maintainer's setting is Config's, press to apply; a reboot comes up as it says). USB RETURN (OS-USBRET) is the device setting of that name: the main loop copies a change
// made here to the setting and a change of the setting to the driver (app_main.cpp sync_usb_return).
void usb_audio_view(shell::Shell& sh) {
  const o3os::usb::IsoAudio* iso = usb0::iso();
  if (iso == nullptr) {
    sh.out().text("usb audio: the driver is not running\n");
    return;
  }
  const o3os::usb::ControlPipe& pipe = usb0::control();
  UsbAudioReport r{usb0::mode(),
                   usb0::profile(),
                   usb0::change_phase(),
                   usb0::pending_mode(),
                   usb0::pending_profile(),
                   pipe.configured(),
                   iso->in_active(),
                   iso->out_active(),
                   iso->in_channels(),
                   &usb0::stream_counters(),
                   &usb0::device_counters(),
                   iso->rate().locked(),
                   iso->rate().period_cycles_q8(),
                   iso->rate().resyncs(),
                   iso->rate().frames_per_sof_q16(),
                   iso->feedback_rate_q16(),
                   iso->feedback().trim_q16(),
                   iso->stem_fill_frames(),
                   iso->return_fill_frames(),
                   usb0::return_enabled(),
                   usb0::pending_return()};
  char buf[kTextBuffer];
  send(sh, format_usb_audio(r, buf, sizeof buf), buf);
}

// `wish` is the return the user asks for (OS-USBOUTS: the layout presents it only below 10 channels; the line says so).
void usb_request(shell::Shell& sh, o3os::usb::Mode mode, o3os::usb::Profile profile, bool wish) {
  const bool usb_return = o3os::usb::return_presented(profile, wish);
  if (!usb0::request_layout(mode, profile, wish)) {
    sh.out().text(usb0::change_phase() != 0u ? "a mode change is already in progress\n"
                  : usb0::mode() == o3os::usb::Mode::Disk ? "refused: the USB DISK mode ends with a restart\n"
                                                        : "refused: profile not supported yet (4s needs post-pan stems) or driver down\n");
    return;
  }
  char layout[20];
  (void)usb_layout_name(profile, usb_return, layout);
  if (wish && !usb_return) {
    sh.out().text("usb: the return is not presented at 10 or more channels (the setting stays On; 8 channels or fewer bring it back)\n");
  }
  if (usb0::change_phase() == 0u) {
    if (usb0::mode() == o3os::usb::Mode::Disk) {  // (accepted only because it is the layout DISK already presents)
      sh.out().text("usb: DISK mode is held by the boot disk until a restart, nothing changed: ").text(layout).nl();
      return;
    }
    sh.out().text(mode == o3os::usb::Mode::Audio ? "usb: already presenting AUDIO with that layout: " : "usb: STANDARD, layout set (used by the next AUDIO attach): ").text(layout).nl();
    return;
  }
  sh.out().text("usb: re-enumerating as ").text(usb_mode_name(mode)).text(" (").text(layout).text(
      "); this terminal drops, reconnect in about a second\n");
}

void usb_map_command(shell::Shell& sh, const shell::Args& a) {
  char buf[kTextBuffer];
  o3os::audio::StemMap& map = usb0::stem_map();
  if (a.argc == 3 && same(a.argv[2], "reset")) {
    map.reset(map.profile());
    usb0::apply_stem_map();
  } else if (a.argc == 4) {
    std::uint32_t slot = 0;
    std::uint32_t sources = 0;
    if (!parse_dec(a.argv[2], o3os::usb::kMaxChannels, slot) || slot == 0u || !o3os::audio::parse_sources(a.argv[3], sources) || !map.set_slot(slot - 1u, sources)) {
      sh.out().text("usage: usb map [reset | <slot> <sources>]   slot 1..profile channels, sources like T1+T9, ML, MR, SA, SB or -\n");
      return;
    }
    usb0::apply_stem_map();
  } else if (a.argc != 2) {
    sh.out().text("usage: usb map [reset | <slot> <sources>]\n");
    return;
  }
  send(sh, format_stem_map(map, buf, sizeof buf), buf);
}

// OS-USBDISK U2a: the first events of endpoint 0 in the last DISK attach (usb/ep0_trace.h), one line each; the ring survives the rollback to the previous layout, so this is read once the console is back.
void usb_ep0_view(shell::Shell& sh) {
  const o3os::usb::Ep0Trace& trace = usb0::ep0_trace();
  const unsigned n = trace.count();
  sh.out().text("usb ep0: ").text(n == 0u ? "nothing recorded (no DISK attach since the boot)\n" : "the first events of the last DISK attach\n");
  char line[96];
  for (unsigned i = 0; i < n; ++i) {
    send(sh, format_usb_ep0_entry(i, trace.entry(i), line, sizeof line), line);
  }
  if (n != 0u) {
    send(sh, format_usb_ep0_retries(trace.stall_retries(), line, sizeof line), line);
  }
}

void usb_subcommand(shell::Shell& sh, const shell::Args& a) {
  const char* sub = a.argv[1];
  if (same(sub, "audio")) {
    if (a.argc == 3 && same(a.argv[2], "reset")) {
      usb0::reset_stream_counters();
    }
    usb_audio_view(sh);
  } else if (same(sub, "mode")) {
    if (a.argc == 2) {
      char layout[20];
      sh.out().text("usb mode: ").text(usb_mode_name(usb0::mode())).text(", profile ").text(usb_profile_name(usb0::profile())).text(", ").text(
          usb_layout_name(usb0::profile(), usb0::return_enabled(), layout)).nl();
    } else if (a.argc == 3 && same(a.argv[2], "audio")) {
      usb_request(sh, o3os::usb::Mode::Audio, usb0::profile(), usb0::return_wanted());
    } else if (a.argc == 3 && same(a.argv[2], "standard")) {
      usb_request(sh, o3os::usb::Mode::Standard, usb0::profile(), usb0::return_wanted());
    } else {
      sh.out().text("usage: usb mode [standard|audio]\n");
    }
  } else if (same(sub, "profile")) {
    std::uint32_t channels = 0;
    o3os::usb::Profile profile = o3os::usb::Profile::P10;
    // The third word, when there is one, chooses USB RETURN with the profile: `out` is capture only ("10 OUT"), `duplex` adds the stereo return ("8 OUT / 2 IN").
    bool usb_return = usb0::return_wanted();
    const bool word = a.argc == 4 && (same(a.argv[3], "out") || same(a.argv[3], "duplex"));
    if (word) {
      usb_return = same(a.argv[3], "duplex");
    }
    if ((a.argc == 3 || word) && parse_dec(a.argv[2], 11u, channels) && (channels == 2u || channels == 4u || channels == 6u || channels == 8u || channels == 10u || channels == 11u)) {
      profile = channels == 2u ? o3os::usb::Profile::P2 : (channels == 4u ? o3os::usb::Profile::P4 : (channels == 6u ? o3os::usb::Profile::P6 : (channels == 8u ? o3os::usb::Profile::P8 : (channels == 11u ? o3os::usb::Profile::P11 : o3os::usb::Profile::P10))));
      usb_request(sh, usb0::mode(), profile, usb_return);  // in AUDIO mode this re-enumerates; in STANDARD it only sets what the next AUDIO attach uses
    } else {
      sh.out().text("usage: usb profile <2|4|6|8|10|11> [out|duplex]   (10 = T1..T8 + master, 8 = T1..T6 + master, 6 = T1..T4, 4 = T1, T2 + master; out = capture only: `profile 10 out` is \"10 OUT\",\n"
                    "       duplex adds the stereo return: `profile 8 duplex` is \"8 OUT / 2 IN\", the layout that opens both ways behind a hub)\n");
    }
  } else if (same(sub, "return")) {
    if (a.argc == 2) {
      char layout[20];
      sh.out().text("usb return: ").text(usb0::return_wanted() ? "on" : "off").text(" (").text(usb_layout_name(usb0::profile(), usb0::return_enabled(), layout)).text(")\n");
    } else if (a.argc == 3 && (same(a.argv[2], "on") || same(a.argv[2], "off"))) {
      usb_request(sh, usb0::mode(), usb0::profile(), same(a.argv[2], "on"));
    } else {
      sh.out().text("usage: usb return [on|off]   (off = no playback stream: the host cannot open a return the bus has no room for)\n");
    }
  } else if (same(sub, "map")) {
    usb_map_command(sh, a);
  } else if (same(sub, "ep0")) {
    usb_ep0_view(sh);
  } else {
    sh.out().text("usage: usb [audio [reset] | mode [standard|audio] | profile <2|4|6|8|10|11> [out|duplex] | return [on|off] | map [reset | <slot> <sources>] | ep0]\n");
  }
}

void cmd_usb(shell::Shell& sh, const shell::Args& a) {
  if (a.argc > 1) {
    usb_subcommand(sh, a);
    return;
  }
  const o3os::usb::ControlPipe& pipe = usb0::control();
  UsbReport r{&usb0::device_counters(),
              &pipe.counters(),
              &usb0::stream_counters(),
              pipe.state(),
              pipe.address(),
              pipe.suspended(),
              pipe.cdc().dtr(),
              pipe.cdc().rts(),
              pipe.cdc().coding,
              pipe.cdc().coding_sets,
              pipe.cdc().control_sets,
              usb0::cdc_writer().bytes_written(),
              usb0::cdc_writer().dropped_chunks(),
              usb0::cdc_rx().size(),
              usb0::attached() ? 1u : 0u,
              usb0::soc::kUsbClockHz};
  char buf[kTextBuffer];
  send(sh, format_usb(r, buf, sizeof buf), buf);
}

void cmd_log(shell::Shell& sh, const shell::Args& a) { run_log_command(sh, g_log, a); }

constexpr unsigned kInputEventsPerTick = 8;

bool input_stream(shell::Shell& sh, void*) {
  core::InputEvent e;
  char buf[96];
  for (unsigned i = 0; i < kInputEventsPerTick && g_tee != nullptr && g_tee->pop(e); ++i) {
    send(sh, format_input_event(e, buf, sizeof buf), buf);
  }
  return true;  // runs until a key is pressed
}

void cmd_input(shell::Shell& sh, const shell::Args& a) {
  char buf[kTextBuffer];
  if (a.argc > 1) {
    if (same(a.argv[1], "stats")) {
      send(sh, format_input_stats(input::device::scanner().stats(), buf, sizeof buf), buf);
    } else {
      sh.out().text("usage: input [stats]\n");
    }
    return;
  }
  if (g_tee == nullptr) {
    sh.out().text("no input tap: the app has not started\n");
    return;
  }
  core::InputEvent stale;
  while (g_tee->pop(stale)) {  // what happened before the command is not live
  }
  sh.out().text("input events, live (any key stops):\n");
  sh.start_stream(&input_stream, nullptr);
}

void cmd_mix(shell::Shell& sh, const shell::Args&) {
  o3os::audio::MeterSnapshot snap;
  (void)audio_engine().graph().meters().snapshot(snap);  // a read that raced a window is shown as read: the next `mix` is whole
  char buf[kTextBuffer];
  const o3os::audio::LimiterReport limiter = audio_engine().graph().limiter_report();  // OS-LIMIT: MAIN is metered before the limiter; its gain reduction is the line after the header
  send(sh, o3os::audio::diag::format_mix(snap, limiter, buf, sizeof buf), buf);
}

// `mon`: shows the monitor's level; `mon off`, `mon on` (70) or `mon <0..100>` sets it, with the engine's one-block smoothing. The line input
// goes through the engine into MAIN and CUE (its default routes); it starts at 0, so nothing is heard until this is typed. Not with OUT patched to IN.
constexpr float kMonitorOn = 70.0f;
void cmd_mon(shell::Shell& sh, const shell::Args& a) {
  char buf[160];
  if (a.argc > 1) {
    float level = -1.0f;
    if (same(a.argv[1], "off")) {
      level = 0.0f;
    } else if (same(a.argv[1], "on")) {
      level = kMonitorOn;
    } else {
      std::uint32_t v = 0;
      bool digits = a.argv[1][0] != '\0';
      for (const char* p = a.argv[1]; *p != '\0' && digits; ++p) {
        digits = *p >= '0' && *p <= '9' && v < 1000u;
        v = digits ? v * 10u + static_cast<std::uint32_t>(*p - '0') : v;
      }
      level = digits && v <= 100u ? static_cast<float>(v) : -1.0f;
    }
    if (level < 0.0f) {
      sh.out().text("usage: mon [off|on|0..100]\n");
      return;
    }
    const bool queued = audio_engine().params().push(
        o3os::audio::ParamChange{o3os::audio::strip_param(o3os::audio::kInputStrip, o3os::audio::graph_param::strip::Volume), level, 0u});
    if (!queued) {
      sh.out().text("mon: the parameter ring is full, try again\n");
      return;
    }
  }
  audio::diag::Text t(buf, sizeof buf);
  t.str("monitor gain (Q16, 65536 = unity) ").dec(static_cast<std::uint32_t>(audio_engine().graph().level_gain(o3os::audio::kInputStrip)));
  t.str(", routes MAIN + CUE; mon on = ").dec(static_cast<std::uint32_t>(kMonitorOn)).str(", mon off = 0").nl();
  send(sh, t.size(), buf);
}

// ---- OS-HW1 bring-up diagnostics ------------------------------------------------------------------------------------------------------
// `peek <hexaddr> [count 1..16]`: aligned 32-bit reads, only inside the modules this image has clocked on (mem_access.h): a read of a gated module
// is a bus fault. A volatile word read of a listed address, nothing else.
void cmd_peek(shell::Shell& sh, const shell::Args& a) {
  std::uint32_t addr = 0;
  std::uint32_t count = 1;
  if (a.argc < 2 || a.argc > 3 || !parse_hex32(a.argv[1], addr) || (a.argc == 3 && (!parse_dec(a.argv[2], kPeekMaxWords, count) || count == 0u))) {
    sh.out().text("usage: peek <hexaddr> [count 1..16]\n");
    return;
  }
  const Access ok = check_peek(addr, count);
  if (ok == Access::Unaligned) {
    sh.out().text("peek: the address must be 4-byte aligned\n");
    return;
  }
  if (ok != Access::Ok) {
    sh.out().text("peek: refused, outside the modules clocked on in this image (a read of a gated module is a bus fault)\n");
    return;
  }
  char buf[kTextBuffer];
  audio::diag::Text t(buf, sizeof buf);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t word_addr = addr + 4u * i;
    t.hex32(word_addr).str(": ").hex32(*reinterpret_cast<volatile std::uint32_t*>(static_cast<std::uintptr_t>(word_addr))).nl();
  }
  send(sh, t.size(), buf);
}

// `poke <hexaddr> <hexvalue>`: one aligned 32-bit write, inside I2S0 only; prints the value read back.
void cmd_poke(shell::Shell& sh, const shell::Args& a) {
  std::uint32_t addr = 0;
  std::uint32_t value = 0;
  if (a.argc != 3 || !parse_hex32(a.argv[1], addr) || !parse_hex32(a.argv[2], value)) {
    sh.out().text("usage: poke <hexaddr> <hexvalue>\n");
    return;
  }
  const Access ok = check_poke(addr);
  if (ok == Access::Unaligned) {
    sh.out().text("poke: the address must be 4-byte aligned\n");
    return;
  }
  if (ok != Access::Ok) {
    sh.out().text("poke: refused, only I2S0 (0x4002F000..0x4002F1FF) may be written\n");
    return;
  }
  volatile std::uint32_t& word = *reinterpret_cast<volatile std::uint32_t*>(static_cast<std::uintptr_t>(addr));
  word = value;
  char buf[96];
  audio::diag::Text t(buf, sizeof buf);
  t.str("poke ").hex32(addr).str(" <- ").hex32(value).str(", read back ").hex32(word).nl();
  send(sh, t.size(), buf);
}

// `audio start`: the full start() again, in the main loop (this handler runs from service(), never from the USB interrupt). It is safe after a failed
// attempt: started_ is false, the I2S transmitter and receiver are soft-reset and disabled, both DMA channels are quiesced and re-programmed, the pipeline
// is re-initialised, the two interrupt lines are off until start() re-enables them and the codec's power-up plan is plain register writes. The display is
// serviced from the idle hook during its waits (the codec's ramp is 0.5 s). On success main notices audio_running() and takes it from there.
void cmd_audio(shell::Shell& sh, const shell::Args& a) {
  if (a.argc != 2 || !same(a.argv[1], "start")) {
    sh.out().text("usage: audio start\n");
    return;
  }
  if (audio_running()) {
    sh.out().text("audio: already running\n");
    return;
  }
  appimg::service_while_blocked(true);
  const auto result = static_cast<std::uint8_t>(audio_device().start(audio_engine().callback()));
  appimg::service_while_blocked(false);
  const AudioClockFault fault = audio_clock_fault();
  char buf[kTextBuffer];
  send(sh, format_audio_start(result, fault, buf, sizeof buf), buf);
  if (audio_running()) {
    clear_boot_fault(kFaultAudio);
    log("audio start: Ok, outputs open when clean");
  } else {
    log_dec("audio start: FAILED, result", result);
    log_audio_clock_fault("audio start", fault);
  }
}

// `rot <0..7>`: REG_ROTATE through the chip, at once (ft812_regs.h), not saved. Only with the display idle (a transfer in flight owns the SPI):
// busy says so and a second try works. The scan-out re-reads the display list every frame, so there is nothing to redraw.
void cmd_rot(shell::Shell& sh, const shell::Args& a) {
  std::uint32_t value = 0;
  if (a.argc != 2 || !parse_dec(a.argv[1], 7u, value)) {
    sh.out().text("usage: rot <0..7>\n");
    return;
  }
  display::Ft812Display& screen = display::device::display();
  if (!screen.ready() || screen.failed()) {
    sh.out().text("rot: the display is not running\n");
    return;
  }
  if (!screen.idle()) {
    sh.out().text("rot: the display is busy with a frame, try again\n");
    return;
  }
  display::device::chip().wr32(display::ft812::reg::kRotate, value);
  sh.out().text("rot: REG_ROTATE = ").dec(value).text(" (not saved)\n");
}

// `sd`: the slot, the mount, the card, the counters (storage/sd_text.h); the loader's last summary is the last line but one. `sd mount` runs the
// card's initialisation and the mount again (a card put back in, a retry after a failure): it blocks for the card's own time (the display stays
// serviced), and not while the card is in use (a project loading, a save, a firmware upload, a listing: OS-SDSWAP, OS-USBDISK F3) or on the computer (`ERR card on host`).
void cmd_sd(shell::Shell& sh, const shell::Args& a) {
  if (a.argc > 1) {
    if (!same(a.argv[1], "mount") || a.argc > 2) {
      sh.out().text("usage: sd [mount]\n");
      return;
    }
    if (storage::device::owner() == storage::CardOwner::Host) {  // OS-USBDISK F1: a mount under the computer would be a second writer on its FAT
      sh.out().text("ERR card on host\n");
      return;
    }
    if (storage::device::card_busy()) {  // OS-SDSWAP: a save is as much "in use" as a load: the mount closes every handle, which would end it
      sh.out().text("sd: the card is in use (a load, a save, an upload, a listing), try again in a moment\n");
      return;
    }
    ls_release(storage::device::storage());  // a listing that a key press ended
    appimg::service_while_blocked(true);
    const hal::storage::Status s = storage::device::mount();
    appimg::service_while_blocked(false);
    sh.out().text("sd mount: ").text(hal::storage::status_name(s)).nl();
    log(s == hal::storage::Status::Ok ? "sd: mounted again from the console" : "sd: mount from the console FAILED");
  }
  char buf[kTextBuffer];
  storage::SdReport report = storage::device::report();
  report.load_summary = appimg::project_load_summary();
  report.sample_summary = appimg::project_sample_summary();
  send(sh, storage::format_sd(report, buf, sizeof buf), buf);
}

// `disk`, `disk share [ro]`, `disk eject` (OS-USBDISK U2a, the running mode): the unit gives the card to the computer while it goes on playing, and takes it back. `share` waits for a due settings write (2 s),
// switches the USB set to DISK (the console's port re-enumerates: open it again), and hands the card over once the computer has configured the disk; `ro` exports it write-protected. `eject` is the unit's side
// of an eject (the computer's next command is UNIT ATTENTION, a copy in flight ends at its next block): it asks no confirmation, the console command is the confirmation. The Tracker takes the card back only through
// an eject, never on its own. `disk` alone prints the state.
void cmd_disk(shell::Shell& sh, const shell::Args& a) {
  const char* refusal = nullptr;
  if (a.argc >= 2 && same(a.argv[1], "share") && a.argc <= 3 && (a.argc == 2 || same(a.argv[2], "ro"))) {
    refusal = appimg::disk_share_request(a.argc == 3);
    if (refusal == nullptr) {
      sh.out().text(a.argc == 3 ? "disk: sharing read-only; the console port re-enumerates, open it again and `disk` shows the state\n" : "disk: sharing; the console port re-enumerates, open it again and `disk` shows the state\n");
      log("disk: share asked from the console");
      return;
    }
  } else if (a.argc == 2 && same(a.argv[1], "eject")) {
    refusal = appimg::disk_eject_request();
    if (refusal == nullptr) {
      sh.out().text("disk: ejecting; the card is remounted, the USB set comes back\n");
      log("disk: eject asked from the console");
      return;
    }
  } else if (a.argc != 1) {
    sh.out().text("usage: disk [share [ro]|eject]\n");
    return;
  }
  if (refusal != nullptr) {
    sh.out().text("ERR disk: ").text(refusal).nl();
    return;
  }
  const usb0::DiskState d = usb0::disk_state();
  sh.out().text("disk: ").text(appimg::disk_share_state_name()).text(", owner ").text(storage::device::owner() == storage::CardOwner::Host ? "computer" : "tracker").text(d.configured ? ", configured" : ", not configured");
  sh.out().text(d.writable ? ", writable" : ", read-only").text(usb0::disk_throttled() ? ", throttled" : "").nl();
}

// `ls [path]`: the card's directory, a few lines a pass (storage/ls_command.h). Never writes: hal/storage.h has no write call.
void cmd_ls(shell::Shell& sh, const shell::Args& a) { run_ls_command(sh, storage::device::storage(), a); }

// `load [folder]`: the OG project folder `folder` of the card (default Workspace, the folder an OG card keeps its working project in) is read into the document, a
// slice a pass of the main loop (appimg::pass); the screen holds its last picture meanwhile, `sd` shows the result. Spaces in the folder name are
// taken as typed. The card is only read.
void cmd_load(shell::Shell& sh, const shell::Args& a) {
  char folder[hal::storage::kPathMax + 1];
  if (!join_path_args(a, 1, folder, sizeof folder)) {
    if (a.argc > 1) {
      sh.out().text("load: folder name too long\n");
      return;
    }
    std::strcpy(folder, "Workspace");
  }
  const appimg::LoadStart started = appimg::start_project_load(folder);
  if (started == appimg::LoadStart::Started) {
    sh.out().text("load: reading ").text(folder).text(" (`sd` shows the result)\n");
    log("load: started by the console");
  } else {
    sh.out().text("load: ").text(appimg::load_start_name(started)).nl();
  }
}

// `inst <1..64>` (OS-007h): what the voices were given for instrument n of the document, one line (app::describe_instrument): the play mode, the window's start
// and length in frames, the loop, the root note, the rate, the volume as a share of 127 and as a gain, and the 16-bit sum of the first 64 samples of the
// window, so that the bench can set the bound sound against the file it came from. A memory read: nothing on the card or in a register.
void cmd_inst(shell::Shell& sh, const shell::Args& a) {
  std::uint32_t n = 0;
  if (a.argc != 2 || !parse_dec(a.argv[1], 64u, n) || n == 0u) {
    sh.out().text("usage: inst <1..64>\n");
    return;
  }
  char buf[192];
  const std::size_t len = appimg::project_instrument_line(n - 1u, buf, sizeof buf);
  sh.out().text(buf, len).nl();
}

// `ogexp [status|cancel]` (OS-FILES-C C2a): the project in the document is written in the OG format into Export/<project> OG[ n] on the card: the patterns, patterns/patternsMetadata, then
// project.mt last (a folder without it is no project), one file a pass of the main loop, each atomically (temp, read-back, rename), through the save layer's guard and the card gate. It never
// goes into a folder that exists (OG, OG 2 .. OG 99) and never replaces or removes a file. The sample instruments of slots 1-48 are written first, as instruments/NN <name>.pti, resampled to 44,100 Hz where they play at another rate (OS-FILES-C C2b).
// With no argument it starts; `status` says how it goes (the last one); `cancel` ends it after the file in progress. Not while a take, the transport, a job or a load is running. Open the result with `load Export/<name> OG`.
void cmd_ogexp(shell::Shell& sh, const shell::Args& a) {
  const bool status = a.argc == 2 && same(a.argv[1], "status");
  if (a.argc > 2 || (a.argc == 2 && !status && !same(a.argv[1], "cancel"))) {
    sh.out().text("usage: ogexp [status|cancel]\n");
    return;
  }
  if (a.argc == 2 && !status) {
    appimg::cancel_og_export();
    sh.out().text("ogexp: cancel asked, no file after the one in progress\n");
    return;
  }
  if (a.argc == 1) {
    const char* const why = appimg::start_og_export();
    if (why != nullptr) {
      sh.out().text("ogexp: ").text(why).nl();
      return;
    }
  }
  appimg::OgExportStatus s;
  if (!appimg::og_export_status(s)) {
    sh.out().text("ogexp: this image has no save layer\n");
    return;
  }
  sh.out().text("ogexp: ").text(s.state).text(" ").dec(s.done).text("/").dec(s.total).text(" files");
  if (s.folder[0] != '\0') {
    sh.out().text(" ").text(s.folder);
  }
  if (s.why[0] != '\0') {
    sh.out().text(" (").text(s.why).text(")");
  }
  if (s.lost) {
    // C2c: which kinds, by the names of the counters (the strip and the EXPORT page say the same in words); the others of the report are the EXPORT page's
    const struct {
      const char* name;
      unsigned n;
    } counts[] = {{"instruments_not_written", s.instruments_not_written}, {"no_sample_memory", s.no_sample_memory}, {"bit_engine", s.bit_engine},
                  {"amiga_engine", s.amiga_engine},                       {"pcm_too_long", s.pcm_too_long},         {"levels_clamped", s.levels_clamped}};
    sh.out().text(" [left out:");
    bool any = false;
    for (const auto& c : counts) {
      if (c.n != 0u) {
        sh.out().text(any ? ", " : " ").text(c.name).text(" ").dec(c.n);
        any = true;
      }
    }
    sh.out().text(any ? "; see the EXPORT page for the rest]" : " other kinds, see the EXPORT page]");
  }
  if (s.levels[0] != '\0') {
    sh.out().text(" ").text(s.levels);
  }
  if (s.clipped != 0u) {
    sh.out().text(" (CLIPPED)");
  }
  if (s.max_step_us != 0u) {
    sh.out().text(" step max ").dec(s.max_step_us).text(" us, overruns ").dec(s.overruns);  // T16
  }
  sh.out().nl();
  for (unsigned i = 0; i < 10u; ++i) {  // T16: where the time went, the worst step then one line a stage that ran (app::OgTiming)
    char line[72];
    const std::size_t n = appimg::og_timing_line(i, line, sizeof line);
    if (n != 0u) {
      sh.out().text(line, n).nl();
    }
  }
}

// `mod ...` (OS-012a): see the header of this file. Every sub-command is a main-loop call into appimg (app_main.cpp); the listing is capped so that it fits the shell's ring.
bool parse_value(const char* s, float& out) {
  bool neg = false;
  if (*s == '-') {
    neg = true;
    ++s;
  }
  if (*s < '0' || *s > '9') {
    return false;
  }
  float v = 0.0f;
  while (*s >= '0' && *s <= '9') {
    v = v * 10.0f + static_cast<float>(*s - '0');
    ++s;
  }
  if (*s == '.') {
    ++s;
    float scale = 0.1f;
    while (*s >= '0' && *s <= '9') {
      v += scale * static_cast<float>(*s - '0');
      scale *= 0.1f;
      ++s;
    }
  }
  out = neg ? -v : v;
  return *s == '\0';
}

void cmd_mod(shell::Shell& sh, const shell::Args& a) {
  constexpr unsigned kMaxLines = 24;
  if (a.argc == 1) {
    char line[160];  // (the perf line of an instance is about 130 characters, OS-303PERF)
    unsigned n = 0;
    for (unsigned i = 0; n < kMaxLines; ++i, ++n) {
      const std::size_t len = appimg::module_line(i, line, sizeof line);
      if (len == 0u) {
        return;
      }
      sh.out().text(line, len).nl();
    }
    // OS-303PERF: the perf measure doubles the instances' lines, so a listing can reach the limit: say so (the refused stems are listed first and are never what is cut)
    if (appimg::module_line(kMaxLines, line, sizeof line) != 0u) {
      sh.out().text("mod: more lines than the listing shows (cpu perf off drops the perf lines)\n");
    }
    return;
  }
  const char* sub = a.argv[1];
  std::uint32_t slot = 0;
  if (std::strcmp(sub, "load") == 0 && a.argc == 3) {
    const appimg::ModStart r = appimg::start_module_load(a.argv[2]);
    sh.out().text("mod: ").text(a.argv[2]).text(": ").text(appimg::mod_start_name(r)).nl();
    if (r == appimg::ModStart::Started) {
      log("mod: load started by the console");
    }
  } else if (std::strcmp(sub, "bind") == 0 && a.argc == 4 && parse_dec(a.argv[2], 64u, slot) && slot != 0u) {
    const char* why = appimg::module_bind(slot - 1u, a.argv[3]);
    sh.out().text("mod: bind ").text(why == nullptr ? "ok" : why).nl();
  } else if (std::strcmp(sub, "note") == 0 && (a.argc == 4 || a.argc == 5) && parse_dec(a.argv[2], 64u, slot) && slot != 0u) {
    std::uint32_t note = 0, vel = 100;
    if (!parse_dec(a.argv[3], 127u, note) || (a.argc == 5 && !parse_dec(a.argv[4], 127u, vel))) {
      sh.out().text("usage: mod note <1..64> <0..127> [1..127]\n");
      return;
    }
    sh.out().text(appimg::module_audition(slot - 1u, note, vel, true) ? "mod: note queued\n" : "mod: not bound, or the queue is full\n");
  } else if (std::strcmp(sub, "off") == 0 && a.argc == 3 && parse_dec(a.argv[2], 64u, slot) && slot != 0u) {
    sh.out().text(appimg::module_audition(slot - 1u, 0, 0, false) ? "mod: off queued\n" : "mod: not bound, or the queue is full\n");
  } else if (std::strcmp(sub, "notice") == 0 && (a.argc == 2 || a.argc == 3) && (a.argc == 2 || parse_dec(a.argv[2], 1000u, slot))) {
    // OS-012d: the licence notices of the built-in modules (module/builtin.h), kMaxLines lines from line `slot` (0: the first); the next call's number is printed when there is more.
    const char* p = appimg::module_notices();
    for (std::uint32_t skip = 0; skip < slot && *p != '\0'; ++skip) {
      while (*p != '\0' && *p != '\n') {
        ++p;
      }
      p += *p == '\n' ? 1 : 0;
    }
    std::uint32_t at = slot;
    for (; *p != '\0' && at < slot + kMaxLines; ++at) {
      const char* e = p;
      while (*e != '\0' && *e != '\n') {
        ++e;
      }
      sh.out().text(p, static_cast<std::size_t>(e - p)).nl();
      p = *e == '\n' ? e + 1 : e;
    }
    if (*p != '\0') {
      sh.out().text("mod: more: mod notice ").dec(at).nl();
    }
  } else if (std::strcmp(sub, "set") == 0 && a.argc == 5 && parse_dec(a.argv[2], 64u, slot) && slot != 0u) {
    std::uint32_t id = 0;
    float value = 0.0f;
    if (!parse_dec(a.argv[3], 31u, id) || !parse_value(a.argv[4], value)) {
      sh.out().text("usage: mod set <1..64> <parameter 0..31> <value>\n");
      return;
    }
    sh.out().text(appimg::module_set_param(slot - 1u, id, value) ? "mod: queued\n" : "mod: not bound, or the queue is full\n");
  } else if (std::strcmp(sub, "gov") == 0 && (a.argc == 2 || a.argc == 4)) {
    // OS-GOV: the CPU governor of the module instruments (audio/module_governor.h): its state, and the two knobs for the unit's tuning (runtime only, not saved). The threshold is held to
    // 50..95 % (under 50 the notes starve, over 95 nothing is left for the interrupts); the budget to 10..100 % (10 is the forced-heavy test: it can only mute).
    audio::ModuleVoices* mv = appimg::module_voices();
    std::uint32_t percent = 0;
    if (mv == nullptr) {
      sh.out().text("mod gov: this image has no module voices\n");
      return;
    }
    if (a.argc == 4) {
      const bool thr = std::strcmp(a.argv[2], "thr") == 0;
      const std::uint32_t lo = thr ? audio::gov::kMinThresholdPercent : audio::gov::kMinBudgetPercent;
      const std::uint32_t hi = thr ? audio::gov::kMaxThresholdPercent : audio::gov::kMaxBudgetPercent;
      if (!thr && std::strcmp(a.argv[2], "budget") != 0) {
        sh.out().text("usage: mod gov [thr <50..95> | budget <10..100>]\n");
        return;
      }
      if (!parse_dec(a.argv[3], 100u, percent) || percent < lo || percent > hi) {
        sh.out().text(thr ? "mod gov: thr is refused outside 50..95 (percent of the block)\n" : "mod gov: budget is refused outside 10..100 (percent of the block)\n");
        return;
      }
      if (thr) {
        mv->governor().set_threshold_percent(percent);
      } else {
        mv->governor().set_budget_percent(percent);
      }
    }
    const audio::gov::Snapshot g = mv->governor().snapshot();
    static const char* const kKinds[] = {"none", "drop", "mute"};
    sh.out().text("mod gov: recent worst render ").dec(g.worst).text(" cycles (").dec(audio::gov::tenths_of_block(g.worst) / 10u).text("%), without the modules ").dec(g.base).text(" (")
        .dec(audio::gov::tenths_of_block(g.base) / 10u).text("%), threshold ").dec(g.threshold).text(" (").dec(audio::gov::tenths_of_block(g.threshold) / 10u).text("%), instance budget ")
        .dec(g.budget).text(" (").dec(audio::gov::tenths_of_block(g.budget) / 10u).text("%)\n");
    sh.out().text("mod gov: new voices admitted ").dec(g.admitted).text(", notes dropped ").dec(g.drops).text(", refused while muted ").dec(g.refused).text(", instances muted ").dec(g.mutes)
        .text("; last ").text(kKinds[static_cast<unsigned>(g.last) % 3u]);
    if (g.last != audio::gov::Event::None) {
      sh.out().text(" on instrument ").dec(g.last_slot + 1u);
    }
    sh.out().nl();
  } else {
    sh.out().text("usage: mod | mod load <name> | mod bind <1..64> <name> | mod note <1..64> <note> [vel] | mod off <1..64> | mod set <1..64> <id> <value> | mod gov [thr <50..95> | budget <10..100>] | mod notice [line]\n");
  }
}

// `radio [retry]` (OS-RAD1): the FM radio driver's state, step, last failure, last bus result, the registers as last read and the lines' levels; `radio retry` runs the power-up
// again from the reset (main loop, like the recorder's source change does), unless a take from the radio is armed or recording or the radio is not the selected source (then the state
// only, and why). A read apart from the retry, which writes the radio's registers.
void cmd_radio(shell::Shell& sh, const shell::Args& a) {
  if (a.argc > 2 || (a.argc == 2 && !same(a.argv[1], "retry"))) {
    sh.out().text("usage: radio [retry]\n");
    return;
  }
  const appimg::RadioRetry verdict = a.argc == 2 ? appimg::radio_retry() : appimg::RadioRetry::NoRadio;
  if (a.argc == 2 && verdict == appimg::RadioRetry::NoRadio) {
    sh.out().text("radio: this image has no radio\n");
    return;
  }
  char buf[kTextBuffer];
  const std::size_t n = appimg::radio_report(buf, sizeof buf);
  if (n == 0u) {
    sh.out().text("radio: this image has no radio\n");
    return;
  }
  if (verdict == appimg::RadioRetry::Started) {
    sh.out().text("radio: power-up started again from the reset; `radio` shows how it went\n");
  } else if (verdict == appimg::RadioRetry::TakeRunning) {
    sh.out().text("radio: retry refused, a take from the radio is armed or recording (the reset would record silence); stop it first. State only:\n");
  } else if (verdict == appimg::RadioRetry::NotSelected) {
    sh.out().text("radio: retry skipped, the radio is not the selected record source (select it on the Record screen first). State only:\n");
  }
  send(sh, n, buf);
}

// ---- OS-HIL: input injection, the app's state, the screen, the pad LEDs ----------------------------------------------------------------------------
// The scanner's queue is the one the key chips feed, both ends in the main loop, and so is the console: the injector's pushes never race the scan.
Injector g_injector;

std::uint32_t uptime_ms32() { return static_cast<std::uint32_t>(system_clock().uptime_ms()); }

bool inject_push(void*, core::InputKind kind, std::uint8_t id, std::int16_t value) { return input::device::scanner().inject(kind, id, value); }

// No injection while the firmware update path is mid-flight: a token is armed, a screen's check waits for its INSTALL question (OS-FWCONFIG: only a pressed key answers it), or a
// put / arm / hand-over is running (the arm token is bound to what the unit shows, and a stray key must not be what cancels or confirms anything).
bool injection_allowed() {
  return !update::device::service().blocks_injection();
}

const char* gesture_name(InjectGesture g) {
  switch (g) {
    case InjectGesture::Tap: return "tap";
    case InjectGesture::Down: return "down";
    case InjectGesture::Up: return "up";
    case InjectGesture::Hold: return "hold";
  }
  return "?";
}

void run_inject(shell::Shell& sh, InjectParse parsed, const InjectRequest& r, const char* what, const char* unknown, const char* usage) {
  if (parsed == InjectParse::Usage) {
    sh.out().text("usage: ").text(usage).nl();
    return;
  }
  if (parsed == InjectParse::Unknown) {
    sh.out().text("ERR ").text(unknown).nl();
    return;
  }
  if (g_tee == nullptr) {
    sh.out().text("ERR no input: the app has not started\n");
    return;
  }
  if (!injection_allowed()) {
    sh.out().text("ERR refused: an fw update is armed or running\n");
    return;
  }
  const Injector::Result result = g_injector.inject(r, uptime_ms32(), &inject_push, nullptr);
  if (result != Injector::Result::Ok) {
    sh.out().text(result == Injector::Result::NoRoom ? "ERR too many releases pending, try again\n" : "ERR the input queue is full, try again\n");
    return;
  }
  char line[shell::kLogTextMax + 1];  // the log: every injected input is marked as one (a person at the unit can tell it from a finger)
  audio::diag::Text t(line, sizeof line);
  t.str("inject ").str(what).chr(' ').dec(r.id);
  if (r.kind == core::InputKind::Encoder) {
    t.chr(' ').chr(r.value < 0 ? '-' : '+').dec(static_cast<std::uint32_t>(r.value < 0 ? -r.value : r.value));
  } else {
    t.chr(' ').str(gesture_name(r.gesture));
    if (r.gesture == InjectGesture::Hold) {
      t.chr(' ').dec(r.hold_ms);
    }
  }
  log(line);
  sh.out().text("ok ").text(what).chr(' ');
  if (r.kind == core::InputKind::Encoder) {
    sh.out().chr(r.value < 0 ? '-' : '+').dec(static_cast<std::uint32_t>(r.value < 0 ? -r.value : r.value));
  } else {
    sh.out().dec(r.id);
  }
  sh.out().nl();
}

void cmd_key(shell::Shell& sh, const shell::Args& a) {
  InjectRequest r;
  const InjectParse p = parse_key(a.argc, a.argv, &appimg::button_code, r);
  run_inject(sh, p, r, "key", "unknown button (a name like f1, play, shift, or 0..32)", "key <button> [tap|down|up|hold <ms>]   button by name (f1, play, shift ...) or 0..32");
}

void cmd_pad(shell::Shell& sh, const shell::Args& a) {
  InjectRequest r;
  const InjectParse p = parse_pad(a.argc, a.argv, r);
  run_inject(sh, p, r, "pad", "pad is 0..47", "pad <0..47> [tap|down|up] [vel 1..127]");
}

// `enc accel [off | <t1> <t2> [gap_ms]]` (OS-AUDIT-28): the encoder acceleration in lists (app/enc_accel.h). Shows the numbers; with arguments sets them first (not saved: a reset goes back to the defaults).
void cmd_enc_accel(shell::Shell& sh, const shell::Args& a) {
  EncAccelRequest q;
  const InjectParse p = parse_enc_accel(a.argc, a.argv, q);
  if (p == InjectParse::Usage) {
    sh.out().text("usage: enc accel [off | <t1> <t2> [gap_ms]]   x4 from t1 detents of a run, x8 from t2 (1..255), gap 5..250 ms (default 6 16 60)\n");
    return;
  }
  if (p == InjectParse::Unknown) {
    sh.out().text("ERR need 1 <= t1 <= t2 <= 255 and gap_ms 5..250\n");
    return;
  }
  appimg::EncAccelNumbers n{};
  if (q.set) {
    n = appimg::EncAccelNumbers{q.on, q.t1, q.t2, q.gap_ms};
    if (!appimg::enc_accel_set(n)) {
      sh.out().text("ERR no app: it has not started\n");
      return;
    }
  }
  if (!appimg::enc_accel_get(n)) {
    sh.out().text("ERR no app: it has not started\n");
    return;
  }
  if (!n.on) {
    sh.out().text("enc accel off (a row a detent)\n");
    return;
  }
  sh.out().text("enc accel x4 from ").dec(n.t1).text(", x8 from ").dec(n.t2).text(" detents of a run, gap ").dec(n.gap_ms).text(" ms\n");
}

void cmd_enc(shell::Shell& sh, const shell::Args& a) {
  if (a.argc >= 2 && same(a.argv[1], "accel")) {
    cmd_enc_accel(sh, a);
    return;
  }
  InjectRequest r;
  const InjectParse p = parse_enc(a.argc, a.argv, r);
  run_inject(sh, p, r, "encoder", "n is -64..64, not 0", "enc <n>   n detents, -64..64, not 0 (minus = counter-clockwise)");
}

constexpr std::size_t kStateBuffer = 384;  // the longest line is about 310 characters (msg up to 48, sel up to 40)

void cmd_state(shell::Shell& sh, const shell::Args&) {
  char buf[kStateBuffer];
  const std::size_t n = appimg::state_line(buf, sizeof buf);
  sh.out().text(buf, n).nl();
}

void cmd_leds(shell::Shell& sh, const shell::Args&) {
  const input::PadLeds& leds = input::device::leds();
  char buf[kStateBuffer];
  audio::diag::Text t(buf, sizeof buf);
  unsigned lit = 0;
  for (unsigned i = 0; i < hal::kPadLedCount; ++i) {
    lit += leds.level(i) != 0u ? 1u : 0u;
  }
  t.str("leds online=").chr(leds.online() ? '1' : '0').str(" errors=").dec(leds.errors()).str(" reinits=").dec(leds.reinits());
  t.str(" lit=").dec(lit).str(" sync=").chr(leds.in_sync() ? '1' : '0').str(" levels=");
  for (unsigned i = 0; i < hal::kPadLedCount; ++i) {  // pad 0 first, two hex digits a pad: the staged level (the driver applies its gamma)
    t.chr("0123456789ABCDEF"[leds.level(i) >> 4]).chr("0123456789ABCDEF"[leds.level(i) & 0xFu]);
  }
  sh.out().text(buf, t.size()).nl();
}

// The chip as `shot` uses it. Everything here is the main loop's, and so is the display driver: a transaction of ours never overlaps one of its own.
class ChipShotPort final : public ShotPort {
 public:
  bool ready() noexcept override {
    const display::Ft812Display& d = display::device::display();
    return d.ready() && !d.failed() && !d.needs_assets();
  }
  bool bus_free() noexcept override { return display::device::display().bus_idle(); }
  bool start_capture(std::uint32_t format, std::uint32_t addr, std::uint32_t y, std::uint32_t width_param, std::uint32_t rows) noexcept override {
    const std::uint32_t words[5] = {display::ft812::kCmdSnapshot2, format, addr, y << 16, (rows << 16) | (width_param & 0xFFFFu)};
    display::Chip& chip = display::device::chip();
    // OS-SIMKEY2, `shot ... pclk0`: the Guide's way, the panel clock off while the band is taken (the default leaves it running, H-2). Restored by every way out of a band below.
    if (stop_clock_ && !clock_stopped_) {
      saved_pclk_ = chip.rd8(display::ft812::reg::kPclk);
      chip.wr8(display::ft812::reg::kPclk, 0);
      clock_stopped_ = saved_pclk_ != 0u;
    }
    if (!chip.cmd_write(words, 5, 20)) {
      restore_clock();
      return false;
    }
    return true;
  }
  Capture poll_capture() noexcept override {
    display::Chip& chip = display::device::chip();
    if (chip.cmd_check()) {
      restore_clock();
      return Capture::Fault;  // (recovered: the coprocessor is running again)
    }
    const std::uint32_t read = chip.rd16(display::ft812::reg::kCmdRead) & 0xFFFu;  // (the 12-bit ring pointers, read as the driver's own cmd_check does)
    const std::uint32_t write = chip.rd16(display::ft812::reg::kCmdWrite) & 0xFFFu;
    if (read != write) {
      return Capture::Pending;
    }
    restore_clock();
    return Capture::Done;
  }
  void abort_capture() noexcept override {
    display::device::chip().cmd_recover();  // the driver's own recovery (CPURESET, pointers zeroed); blocks 10 ms, on a failure path only
    restore_clock();
  }
  void stop_clock(bool on) noexcept override { stop_clock_ = on; }
  std::uint32_t reinits() noexcept override { return display::device::display().reinit_count(); }
  void read(std::uint32_t addr, std::uint8_t* out, std::size_t n) noexcept override { display::device::chip().read_block(addr, out, n); }
  std::uint32_t frames() noexcept override { return display::device::display().stats().frames; }
  std::uint32_t now_ms() noexcept override { return uptime_ms32(); }

 private:
  void restore_clock() noexcept {
    if (clock_stopped_) {
      display::device::chip().wr8(display::ft812::reg::kPclk, saved_pclk_);
      clock_stopped_ = false;
    }
  }
  bool stop_clock_ = false;     // `shot ... pclk0`: REG_PCLK 0 for the time of a band (main loop only)
  bool clock_stopped_ = false;  // REG_PCLK is 0 now because of us, saved_pclk_ is what it was
  std::uint8_t saved_pclk_ = 0;
};
ChipShotPort g_shot_port;
ShotStream g_shot;

bool shot_stream(shell::Shell& sh, void*) { return g_shot.step(sh.out(), g_shot_port); }

}  // namespace

bool shot_active() noexcept { return g_shot.active(); }

ShotPort& screenshot_port() noexcept { return g_shot_port; }  // (a getter: the service clears `pclk0` itself when it starts a capture)

namespace {

// `shot [565|8888 [w]]`: 565 (default) takes two bands of 240 rows, 8888 (32-bit pixels) six of 80; `w` is what CMD_SNAPSHOT2 is given as its width
// (default default_width_param: 800 for 565, 1600 for 8888; the chip counts w in 16-bit units), for a hardware session that finds the picture sheared: a larger w
// makes the bands shorter (ShotStream::start sizes them by what the chip may write, so the region is never overrun). A key press ends the stream and drops a snapshot that is still queued.
void cmd_shot(shell::Shell& sh, const shell::Args& a) {
  ShotParams params;
  std::uint32_t w = 0;
  // A trailing `pclk0` (OS-SIMKEY2) stops the panel clock while a band is taken, the Guide's recipe; rects were 0.545 of their colour in captures without it.
  std::size_t argc = a.argc;
  const bool pclk0 = argc >= 2 && same(a.argv[argc - 1], "pclk0");
  if (pclk0) {
    --argc;
  }
  if (argc > 3 || (argc >= 2 && !same(a.argv[1], "565") && !same(a.argv[1], "8888")) || (argc == 3 && (!parse_dec(a.argv[2], 4095u, w) || w == 0u))) {
    sh.out().text("usage: shot [565|8888 [w]] [pclk0]\n");
    return;
  }
  if (appimg::screenshot_active()) {  // OS-SCREENSHOT: the same RAM_G region
    sh.out().text("ERR shot: a screenshot is being saved\n");
    return;
  }
  g_shot_port.stop_clock(pclk0);
  if (argc >= 2 && same(a.argv[1], "8888")) {
    params.format = ShotFormat::Argb8;
  }
  params.width_param = argc == 3 ? w : default_width_param(params.format);
  std::uint32_t region = 0;
  std::uint32_t region_bytes = 0;
  appimg::shot_region(region, region_bytes);
  if (!g_shot.start(params, region, region_bytes)) {
    sh.out().text("ERR shot: the RAM_G region cannot hold bands of that width (try a smaller w)\n");
    return;
  }
  log("shot: started by the console");
  sh.start_stream(&shot_stream, nullptr, true);
}

constexpr std::uint32_t kResetDrainMs = 40;

void cmd_voices(shell::Shell& sh, const shell::Args&) {
  const o3os::audio::VoicePool::VoiceSnapshot v = audio_voices().voice_snapshot();
  char buf[160];
  send(sh, o3os::audio::diag::format_voices(v.in_use, o3os::audio::kMaxVoices, v.peak, v.steals, v.advanced, buf, sizeof buf), buf);
}

void cmd_reset(shell::Shell& sh, const shell::Args&) {
  sh.out().text("resetting (SYSRESETREQ)\n");
  sh.out().flush();
  // OS-OUTON: the outputs are open from boot, so the way out is quiet: the amplifier key first, then the codec's outputs and its DAC (two register writes, no ramp to wait for: the DAC
  // ramps down by itself in the codec while the packets below leave), then the reset (reset_chip drops the key again, a no-op here). Idempotent, and plain when the audio never started.
  audio_device().mute();
  // Give the packets time to leave: a bounded wait of at most kResetDrainMs, then the reset whatever the host did.
  const std::uint64_t start = system_clock().uptime_ms();
  while (system_clock().uptime_ms() - start < kResetDrainMs) {
    asm volatile("wfi");
  }
  boot::reset_chip();
}

// `fw ...` (OS-006c): everything is in update/fw_service.cpp; this binds the image's service (the store on the SD volume, the board's hand-over) to the shell.
void cmd_fw(shell::Shell& sh, const shell::Args& a) { update::device::service().run(sh, a); }

// `bench <kernel|all> [blocks]` (OS-PERF3, src/bench.cpp). `all` is the table of kernels the render spends its block on; `mem` and `stages` are asked for by name.
// OS-BENCHOPT: only in an image built with -DO3OS_BENCH=ON (a bench image); a release image answers with one line.
#if O3OS_BENCH
void bench_emit(void* ctx, const char* line, std::size_t n) {
  shell::Shell& sh = *static_cast<shell::Shell*>(ctx);
  sh.out().text(line, n);
  sh.out().flush();  // a line printed before a long step (the scratch fill) goes out whole, not at the next console tick
}

void bench_usage(shell::Shell& sh) {
  sh.out().text("usage: bench <kernel|all> [blocks 1..4096, default 64] [alias|direct|sram]   |   bench disk reset|clean\n");
  for (const bench::KernelInfo& k : bench::kKernelInfo) {
    sh.out().text("  ").text(k.name);
    for (std::size_t len = std::strlen(k.name); len < 13u; ++len) {
      sh.out().chr(' ');
    }
    sh.out().text(k.help).nl();
  }
}

void cmd_bench(shell::Shell& sh, const shell::Args& a) {
  if (a.argc == 3 && same(a.argv[1], "disk") && same(a.argv[2], "clean")) {  // OS-USBDISK U1d (Q14): removes the write arm's scratch file; a card the Tracker owns only
    bench::disk_clean(&bench_emit, &sh);
    return;
  }
  if (a.argc == 3 && same(a.argv[1], "disk") && same(a.argv[2], "reset")) {  // OS-USBDISK U1d: opens the window `bench disk` reports (the USB interrupt and the MSC pump passes)
    bench::disk_reset();
    sh.out().text("BENCH disk window opened (bench disk reads it; within 60 s)\n");
    return;
  }
  if (a.argc < 2 || a.argc > 4) {
    bench_usage(sh);
    return;
  }
  std::uint32_t blocks = 0;
  bench::Variant variant = bench::Variant::Normal;
  for (unsigned i = 2; i < a.argc; ++i) {  // [blocks] [alias|sram], in either order
    const bench::Variant v = bench::find_variant(a.argv[i]);
    if (v != bench::Variant::Count) {
      variant = v;
    } else if (!parse_dec(a.argv[i], bench::kMaxBlocks, blocks) || blocks == 0u) {
      bench_usage(sh);
      return;
    }
  }
  if (same(a.argv[1], "all")) {
    static constexpr bench::Kernel kAll[] = {bench::Kernel::Voice,   bench::Kernel::VoiceFilter, bench::Kernel::VoiceDrive,     bench::Kernel::VoiceCrush, bench::Kernel::Tb303,        bench::Kernel::Delay,  bench::Kernel::Reverb,
                                             bench::Kernel::ReverbFull, bench::Kernel::Limiter, bench::Kernel::LimiterQuiet, bench::Kernel::Strips};
    for (const bench::Kernel k : kAll) {
      bench::run(k, blocks, variant, &bench_emit, &sh);
    }
    sh.out().text("BENCH done\n");
    return;
  }
  const bench::Kernel k = bench::find_kernel(a.argv[1]);
  if (k == bench::Kernel::Count) {
    bench_usage(sh);
    return;
  }
  bench::run(k, blocks, variant, &bench_emit, &sh);
  sh.out().text("BENCH done\n");
}
#else
void cmd_bench(shell::Shell& sh, const shell::Args&) { sh.out().text("ERR bench is not in this image (build with -DO3OS_BENCH=ON)\n"); }
#endif

// `cache [on|off|inv]` (OS-PERF3, src/cache.cpp, lmem.h).
void cmd_cache(shell::Shell& sh, const shell::Args& a) {
  if (a.argc == 2) {
    bool ok = true;
    if (same(a.argv[1], "on")) {
      ok = cache::enable();
    } else if (same(a.argv[1], "off")) {
      ok = cache::disable();
    } else if (same(a.argv[1], "inv")) {
      ok = cache::invalidate_all();
    } else if (same(a.argv[1], "check")) {
#if O3OS_BENCH
      bench::check_coherence(&bench_emit, &sh);  // OS-PERF3: the single-alias rule on the unit (bench.cpp)
#else
      sh.out().text("ERR cache check is not in this image (build with -DO3OS_BENCH=ON)\n");
#endif
      return;
    } else {
      sh.out().text("usage: cache [on|off|inv|check]\n");
      return;
    }
    if (!ok) {
      sh.out().text("ERR the cache command did not finish\n");
    }
  } else if (a.argc != 1) {
    sh.out().text("usage: cache [on|off|inv|check]\n");
    return;
  }
  char buf[160];
  audio::diag::Text t(buf, sizeof buf);
  t.str("code cache ").str(cache::enabled() ? "on" : "off").str("  PCCCR ").hex32(cache::control()).str("  PCCRMR ").hex32(cache::region_modes());
  t.str("  (8 KiB, 2 ways, 16 B lines; flash and the 0x0800_0000 SDRAM alias only)\n");
  send(sh, t.size(), buf);
}

// `power ...` (OS-POWER P1, power/src/power_host_mk66.cpp): the Power switch and the shutdown. The stages are in the diag log as `power: <stage> +N ms` (`log` right after `power off`: the CDC ends at USB off).
const char* power_refusal_text(power::Refusal why) {
  switch (why) {
    case power::Refusal::Firmware: return "a firmware update is running";
    case power::Refusal::Export: return "an export is running (stop it first)";
    case power::Refusal::Disk: return "the USB disk may be written by the computer (eject it first)";
    case power::Refusal::Running: return "already powering off";
    case power::Refusal::Held: return "Power is down (or reads pressed: check the polarity with power)";
    case power::Refusal::None: break;
  }
  return "";
}

void cmd_power(shell::Shell& sh, const shell::Args& a) {
  if (a.argc == 1) {
    char buf[kTextBuffer];
    send(sh, power::format_power_report(power::device::report(), buf, sizeof buf), buf);
    return;
  }
  if (a.argc == 2 && same(a.argv[1], "press")) {
    power::device::inject(power::Inject::Pressed);
    log("power: switch held by the console");
    sh.out().text("power: the console holds the switch down (`power release` lets go, `power pin` reads the pin again)\n");
    return;
  }
  if (a.argc == 2 && same(a.argv[1], "release")) {
    power::device::inject(power::Inject::Released);
    log("power: switch released by the console");
    sh.out().text("power: the console holds the switch up (`power pin` reads the pin again)\n");
    return;
  }
  if (a.argc == 2 && same(a.argv[1], "pin")) {
    power::device::inject(power::Inject::Real);
    sh.out().text("power: the hold reads the pin\n");
    return;
  }
  if ((a.argc == 2 || a.argc == 3) && same(a.argv[1], "off") && (a.argc == 2 || same(a.argv[2], "dry"))) {
    const bool dry = a.argc == 3;
    const power::Refusal why = power::device::request_off(uptime_ms32(), dry);
    if (why != power::Refusal::None) {
      sh.out().text("ERR power: ").text(power_refusal_text(why)).nl();
      return;
    }
    sh.out().text(dry ? "power: off dry: the stages 0 to E, USB off, the lights out, then a restart after Power is released\n"
                      : (power::device::vlpr_off() ? "power: off: the stages 0 to F, USB off, the lights out, then the VLPR off after Power is released (a press boots the unit)\n"
                                                   : "power: off: the stages 0 to F, USB off, the lights out, then a restart after Power is released (built without the VLPR off)\n"));
    return;
  }
  sh.out().text("usage: power [press|release|pin|off [dry]]\n");
}

// Names and help come from command_table.h by enumerator; only the handler is bound here.
constexpr shell::Command bind(Cmd c, shell::CommandFn run) { return shell::command(info(c).name, info(c).help, run); }
constexpr shell::Command kCommands[] = {
    bind(Cmd::Help, &cmd_help),   bind(Cmd::Version, &cmd_version), bind(Cmd::Stage, &cmd_stage), bind(Cmd::Cpu, &cmd_cpu),
    bind(Cmd::Mem, &cmd_mem),     bind(Cmd::Crash, &cmd_crash),     bind(Cmd::Usb, &cmd_usb),     bind(Cmd::Log, &cmd_log),
    bind(Cmd::Input, &cmd_input), bind(Cmd::Mix, &cmd_mix),         bind(Cmd::Mon, &cmd_mon),     bind(Cmd::Peek, &cmd_peek),
    bind(Cmd::Poke, &cmd_poke),   bind(Cmd::Audio, &cmd_audio),     bind(Cmd::Rot, &cmd_rot),    bind(Cmd::Sd, &cmd_sd),
    bind(Cmd::Ls, &cmd_ls),       bind(Cmd::Load, &cmd_load),       bind(Cmd::Inst, &cmd_inst),
    bind(Cmd::Mod, &cmd_mod),     bind(Cmd::Reset, &cmd_reset),     bind(Cmd::Fw, &cmd_fw),
    bind(Cmd::Radio, &cmd_radio),
    bind(Cmd::Voices, &cmd_voices),
    bind(Cmd::Ogexp, &cmd_ogexp),
    bind(Cmd::Key, &cmd_key),     bind(Cmd::Pad, &cmd_pad),         bind(Cmd::Enc, &cmd_enc),     bind(Cmd::State, &cmd_state),
    bind(Cmd::Shot, &cmd_shot),   bind(Cmd::Leds, &cmd_leds),       bind(Cmd::Bench, &cmd_bench), bind(Cmd::Cache, &cmd_cache),
    bind(Cmd::Power, &cmd_power),   bind(Cmd::Disk, &cmd_disk),
    bind(Cmd::Guard, &cmd_guard),
};
static_assert(sizeof(kCommands) / sizeof(kCommands[0]) == static_cast<std::size_t>(Cmd::Count), "every command has a handler");

// Once the one-way commit has happened (D2) the shell answers every command that could start anything with `ERR powering off`; the reads that diagnose the shutdown stay (command_table.h kPowerReads,
// pinned by mk66_usb_text_test). The table is not in the order of Cmd, so the bits are found by the command's id, at compile time: one bit per ROW of kCommands, and the gate is one index and one test.
constexpr std::uint64_t power_read_rows() noexcept {
  std::uint64_t rows = 0;
  for (unsigned row = 0; row < static_cast<unsigned>(Cmd::Count); ++row) {
    for (unsigned c = 0; c < static_cast<unsigned>(Cmd::Count); ++c) {
      if (power_read(static_cast<Cmd>(c)) && kCommands[row].id == shell::command_id(kCommandInfo[c].name)) {
        rows |= std::uint64_t{1} << row;
      }
    }
  }
  return rows;
}
constexpr std::uint64_t kPowerReadRows = power_read_rows();
static_assert(__builtin_popcountll(kPowerReadRows) == __builtin_popcountll(kPowerReads), "every read of the power-off gate is a row of the table");

const char* power_gate(const shell::Command& cmd) {
  if (!power::device::locked()) {
    return nullptr;
  }
  const std::size_t row = static_cast<std::size_t>(&cmd - kCommands);  // (Shell::find() hands out the rows of the table it was built over, kCommands)
  return row < static_cast<std::size_t>(Cmd::Count) && ((kPowerReadRows >> row) & 1u) != 0u ? nullptr : "ERR powering off\n";
}

shell::Shell g_shell(kCommands, sizeof(kCommands) / sizeof(kCommands[0]), shell::Output(shell::Sink{&sink_write, &sink_flush, nullptr, &sink_room}));
shell::ConsolePump g_pump(g_shell, usb0::cdc_rx());
bool g_dtr_seen = false;

}  // namespace

// ---- The log -------------------------------------------------------------------------------------------------------------------
void init_log() noexcept {
  g_shell.set_gate(&power_gate);  // OS-POWER P1 (D2): after the one-way commit the shell refuses what could start anything
  g_log.init(reinterpret_cast<shell::LogRecord*>(static_cast<std::uintptr_t>(mem::kDiagLogBase)), kLogRecords);
}

void log(const char* text) noexcept { g_log.write(boot::uptime_ms_low(), text); }

void log_dec(const char* text, std::uint32_t value) noexcept {
  char buf[shell::kLogTextMax + 1];
  audio::diag::Text t(buf, sizeof buf);
  t.str(text).chr(' ').dec(value);
  log(buf);
}

void log_hex(const char* text, std::uint32_t value) noexcept {
  char buf[shell::kLogTextMax + 1];
  audio::diag::Text t(buf, sizeof buf);
  t.str(text).chr(' ').hex32(value);
  log(buf);
}

// The clock fault detail of a failed audio start, as two log lines (a line is cut at 55 characters): where it stopped, then MCR and MDR.
void log_audio_clock_fault(const char* prefix, const AudioClockFault& fault) noexcept {
  if (fault.where == AudioClockFault::None) {
    return;
  }
  char buf[shell::kLogTextMax + 1];
  audio::diag::Text t(buf, sizeof buf);
  t.str(prefix).str(" clock fault at ").str(audio_fault_where_name(fault.where));
  log(buf);
  audio::diag::Text u(buf, sizeof buf);
  u.str(prefix).str(" MCR ").hex32(fault.mcr).str(" MDR ").hex32(fault.mdr);
  log(buf);
}

// ---- The console ---------------------------------------------------------------------------------------------------------------
void note_boot_fault(BootFault fault) noexcept { g_boot_faults |= static_cast<std::uint32_t>(fault); }
void clear_boot_fault(BootFault fault) noexcept { g_boot_faults &= ~static_cast<std::uint32_t>(fault); }

void note_boot_crash(const crash::CrashRecord* record) noexcept {
  if (record != nullptr) {
    g_boot_crash = *record;
    g_have_boot_crash = true;
  }
}

hal::Input& tap_input(hal::Input& source) noexcept {
  if (g_tee == nullptr) {
    g_tee = new (g_tee_storage) TeeInput(source);
  }
  return *g_tee;
}

void lock_input() noexcept {
  if (g_tee != nullptr) {
    g_tee->lock();
  }
}

void hold_input(bool held) noexcept {
  if (g_tee != nullptr) {
    g_tee->hold(held);
  }
}

void service() noexcept {
  g_injector.service(uptime_ms32(), injection_allowed(), &inject_push, nullptr);  // OS-HIL: the releases of taps and holds, whether or not a terminal is open
  if (g_shot.active() && !g_shell.streaming()) {
    g_shot.cancel(g_shot_port);  // a key press (or the terminal closing) ended the stream
  }
  update::device::service().poll(g_shell);  // OS-006c: a hand-over that has started ends by its own clock, with or without a terminal
  usb0::service();  // OS-006d: a STANDARD / AUDIO switch in progress (drain, detach, attach)
  if (!usb0::attached()) {
    if (g_dtr_seen) {  // the device detached for a mode change: the terminal that was open is gone, as if DTR had dropped
      g_shell.stop_stream();
      update::device::service().terminal_closed();
      g_dtr_seen = false;
    }
    return;
  }
  // What the host typed, with back-pressure (usb/diag_pump.h): input waits while the output has no room for a reply or a pager is running.
  g_pump.service();
  const bool dtr = usb0::control().cdc().dtr();
  if (dtr && !g_dtr_seen) {
    update::device::service().terminal_closed();  // a stale upload or token of a terminal that vanished without DTR dropping (OS-006c)
    g_shell.begin(kBanner);  // a terminal opened the port
  } else if (!dtr && g_dtr_seen) {
    g_shell.stop_stream();   // it closed: a stream (and a raw receiver) has no one to talk to
    update::device::service().terminal_closed();  // OS-006c: an upload in progress is abandoned and its temporary file deleted
  }
  g_dtr_seen = dtr;
  g_shell.tick();
  usb0::cdc_writer().flush();
}

}  // namespace o3os::platform::mk66::console
