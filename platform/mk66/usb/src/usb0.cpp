// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MK66's USB0 device driver (OS-006b): see usb0.h. This file is the registers and the wiring; the logic is usb/device.h.
//
// SEQUENCE (start(); each step cites where the values come from: soc_facts.h has the numbers, README.md the whole table):
//   0. preconditions checked BEFORE anything is touched: the descriptor table is 512-byte aligned (BDT_PAGE1..3 hold address bits
//      31..9), the descriptors built (a builder that refuses its buffer returns 0) and the configuration's wTotalLength is its length.
//      The structural validator is not run here: STANDARD's descriptors are a constant of the build, host-tested byte for byte against the
//      validator and the goldens (mk66_usb_soc_test.cpp), and the validator costs 2.3 KB of flash. If a check fails the pull-up stays off.
//   1. clock: SIM_SOPT2 USBSRC = 1 with PLLFLLSEL = 3 (IRC48M), SIM_CLKDIV2 = 0 (divide by one), SIM_SCGC4 gates the module on. IRC48M
//      because no divider makes 48 MHz from the 180 MHz PLL (soc_facts.h proves it).
//   2. the system MPU: bus master 4 (USB FS) gets read and write access to region 0 (the whole address space): without it the module's
//      DMA is refused and ERRSTAT[DMAERR] is raised.
//   3. USB0_CLK_RECOVER_IRC_EN = IRC_EN | REG_EN (the 48 MHz oscillator and its regulator on), then USB0_CLK_RECOVER_CTRL =
//      CLOCK_RECOVER_EN | RESTART_IFRTRIM_EN (the oscillator is steered by the bus' start-of-frame packets; from then on its trim
//      follows the host). A short wait for the oscillator.
//   4. USB0_BDTPAGE1..3 = the table's address; ISTAT / ERRSTAT / OTGISTAT cleared; CTL = USBENSOFEN (module on); USBCTRL = 0 (the
//      transceiver is never put to sleep: see README.md "VBUS and suspend").
//   5. UsbDevice::prepare(): endpoint 0 armed to receive, the address 0, interrupts enabled in INTEN/ERREN, in the state a bus
//      reset leaves the module in. THE TABLE AND ENDPOINT 0 ARE READY BEFORE THE HOST CAN SEE THE DEVICE.
//   6. NVIC: pending cleared, the line enabled (its priority was set at boot from the table of ADR 0013 D1: 64).
//   7. USB0_CONTROL = DPPULLUPNONOTG: the D+ pull-up. The host now sees a full-speed device and resets the bus; the first interrupt
//      is that reset.
//
// VBUS. Whether the board routes VBUS to the chip is not known (docs/hw/tracker-hardware-notes.md: "no USB routing recorded"; ADR 0012
// validation item 1 is the board trace), so this driver does not read it: it switches the pull-up on at boot and leaves it. Consequences, all benign for a bus-powered device and listed as hardware checks: with no host attached the pull-up
// drives nothing that listens; a host that is switched off but supplies VBUS to the cable sees D+ pulled up as soon as it powers up,
// which is the normal attach; "unplugged" is seen only as the bus going idle (suspend interrupt) and "plugged in again" as a bus reset,
// and both are handled the same way whatever the cause (usb/control.h bus_reset, on_suspend).
#include <o3os/platform/mk66/usb/usb0.h>

#include "../../src/internal.h"
#include "../../src/regs.h"

#include <o3os/audio/stem_map.h>
#include <o3os/platform/mk66/audio.h>
#include <o3os/platform/mk66/bench_disk.h>
#include <o3os/platform/mk66/mem_map.h>
#include <o3os/platform/mk66/platform.h>
#include <o3os/platform/mk66/sections.h>
#include <o3os/platform/mk66/storage/device.h>
#include <o3os/platform/mk66/usb/console.h>
#include <o3os/platform/mk66/usb/msc_bridge.h>
#include <o3os/platform/mk66/usb/soc_facts.h>
#include <o3os/midi/usb_bind.h>
#include <o3os/usb/descriptors.h>
#include <o3os/usb/ep0_trace.h>
#include <o3os/usb/identity.h>
#include <o3os/usb/iso_audio.h>
#include <o3os/usb/msc.h>
#include <o3os/usb/msc_link.h>
#include <o3os/usb/msc_throttle.h>

#include <cstdint>
#include <cstring>
#include <new>

namespace o3os::platform::mk66::usb0 {
namespace {

using regs::at;

volatile std::uint8_t& usb_reg(std::uint32_t offset) noexcept { return at<std::uint8_t>(soc::kBase + offset); }

// The register policy of usb/device.h: one-instruction accessors. istat / errstat flags clear by writing a 1.
struct Usb0Regs {
  std::uint8_t istat() { return usb_reg(soc::kOffIstat); }
  void istat_clear(std::uint8_t bits) { usb_reg(soc::kOffIstat) = bits; }
  std::uint8_t stat() { return usb_reg(soc::kOffStat); }
  std::uint8_t errstat() { return usb_reg(soc::kOffErrstat); }
  void errstat_clear(std::uint8_t bits) { usb_reg(soc::kOffErrstat) = bits; }
  void ctl(std::uint8_t value) { usb_reg(soc::kOffCtl) = value; }
  std::uint8_t ctl() { return usb_reg(soc::kOffCtl); }
  void set_address(std::uint8_t address) { usb_reg(soc::kOffAddr) = address; }
  void set_inten(std::uint8_t value) { usb_reg(soc::kOffInten) = value; }
  void set_erren(std::uint8_t value) { usb_reg(soc::kOffErren) = value; }
  std::uint8_t endpt(unsigned number) { return usb_reg(soc::kOffEndpt0 + soc::kEndptStride * number); }
  void endpt(unsigned number, std::uint8_t value) { usb_reg(soc::kOffEndpt0 + soc::kEndptStride * number) = value; }
  void dmb() { asm volatile("dmb" ::: "memory"); }  // the descriptor's address and count are stored before the store that sets OWN
  std::uint32_t cycles() { return regs::dwt_cyccnt(); }
  // The 11-bit frame number of the last SOF (OS-006d, the rate estimator's gap detector): reading FRMNUML latches FRMNUMH.
  std::uint32_t frame() {
    const std::uint32_t low = usb_reg(soc::kOffFrmNumL);
    return low | ((static_cast<std::uint32_t>(usb_reg(soc::kOffFrmNumH)) & 7u) << 8);
  }
};

// The descriptors, built once at start-up in the static constructor (before main): identity from usb/identity.h, the serial from the
// chip's unique ID. They live in RAM and the control pipe sends them from there.
// The layout the static constructor builds and a boot presents when AUDIO is on (OS-USBRET, ADR 0012 amendment 5; the device setting USB AUDIO decides the mode since OS-USBAUDIO, see start()):
// "8 OUT / 2 IN", profile 8 with USB RETURN on. It is the one that opens
// duplex on every host measured (profile 10 with the return does not behind a hub with a transaction translator); STANDARD's descriptors do not depend on it.
constexpr o3os::usb::Profile kBootProfile = o3os::usb::Profile::P8;

struct Descriptors {
  Descriptors() noexcept {
    std::uint32_t uid[4] = {at<std::uint32_t>(soc::kSimUidh), at<std::uint32_t>(soc::kSimUidmh), at<std::uint32_t>(soc::kSimUidml),
                            at<std::uint32_t>(soc::kSimUidl)};
    soc::serial_from_uid(uid, serial);
    cfg.speed = o3os::usb::Speed::Full;
    cfg.serial = serial;
    build(o3os::usb::Mode::Standard, kBootProfile, true);
  }
  // (Re)builds the device and configuration descriptors of a set into the buffers. False when a builder refused its buffer (nothing usable).
  bool build(o3os::usb::Mode mode, o3os::usb::Profile profile, bool usb_return) noexcept {
    cfg.mode = mode;
    cfg.profile = profile;
    cfg.usb_return = usb_return;
    device_len = o3os::usb::build_device_descriptor(cfg, device, sizeof device);
    config_len = o3os::usb::build_config_descriptor(cfg, config, sizeof config);
    return device_len == o3os::usb::kDeviceDescriptorSize && config_len >= 9u;
  }
  o3os::usb::Config cfg;
  char serial[soc::kSerialChars + 1] = {};
  alignas(4) std::uint8_t device[o3os::usb::kDeviceDescriptorSize] = {};  // word aligned like every buffer the DMA reads
  alignas(4) std::uint8_t config[soc::kConfigBufferBytes] = {};
  std::size_t device_len = 0;
  std::size_t config_len = 0;
};

// The objects, all in .bss (zeroed by the reset sequence), the table 512-byte aligned by its type. Declaration order is the order the
// constructors run: the descriptors first, the pipe over them, the device over everything.
Descriptors g_desc;
alignas(o3os::usb::kBdtAlign) o3os::usb::BdtTable g_bdt;
o3os::usb::ControlPipe g_pipe(g_desc.cfg, g_desc.device, g_desc.device_len, g_desc.config, g_desc.config_len);
o3os::usb::Counters g_stream_counters;
o3os::usb::CdcTxRing g_cdc_tx;
o3os::usb::CdcRxRing g_cdc_rx;
o3os::usb::MidiRing g_midi_to_host;
o3os::usb::MidiRing g_midi_from_host;
o3os::usb::CdcTxWriter g_writer(g_cdc_tx);
Usb0Regs g_regs;
o3os::usb::UsbDevice<Usb0Regs> g_dev(g_regs, g_bdt, g_pipe, g_stream_counters, g_cdc_tx, g_cdc_rx, g_midi_to_host, g_midi_from_host);
// OS-USBDISK U2a: endpoint 0's first 32 events of a DISK attach (ep0_trace.h, 388 B in .bss; `usb ep0`). Armed by start() / attach() for the DISK set only, with the interrupt line off, disarmed by detach(); the USB
// interrupt is the only writer while it is armed, so it survives the rollback to the previous layout (the way back does not arm it) until the next DISK attach.
o3os::usb::Ep0Trace g_ep0_trace;
volatile bool g_attached = false;  // the pull-up is on: written by start() and the mode change in the main thread, read by the console

// ---- AUDIO mode (OS-006d) -------------------------------------------------------------------------------------------------------------------
// The return ring (USB ISR writes, the engine's pull reads: 2 KB), its engine-side reader, and the streams object, built by start() once the engine exists (it
// holds a reference to the engine's stem ring, which must not be asked for during static initialisation). The stem map is the user's slot mapping.
o3os::usb::ReturnRing g_return_ring;
o3os::usb::ReturnReader g_return_reader(g_return_ring, g_stream_counters);
alignas(o3os::usb::IsoAudio) unsigned char g_iso_storage[sizeof(o3os::usb::IsoAudio)];
o3os::usb::IsoAudio* g_iso = nullptr;
o3os::audio::StemMap g_map;
static_assert(soc::kConfigBufferBytes >= o3os::usb::kMaxConfigDescriptorSize, "the AUDIO configuration descriptor fits the buffer");

// ---- DISK mode (OS-USBDISK U1c, docs/specs/OS-USBDISK.md 3.4) ---------------------------------------------------------------------------------
// The Mass Storage function: the link (the two packet rings, 1.1 KB, .bss) is bound to the device by start() and is the ONLY thing the USB interrupt shares with the main loop in DISK mode (single
// writer per field, usb/msc_link.h); the engine (the SCSI state machine, its block buffers) and the pump are built by start() and touched by the main loop alone (disk_pass), never by the
// interrupt. The bridge is the engine's BlockStore over the card (storage/block_device.h), a medium only while the owner is Host. Built once, never destroyed; null until start().
using MscBridge = o3os::platform::mk66::usb::MscBridge;
o3os::usb::MscLink g_msc_link;
alignas(MscBridge) unsigned char g_bridge_storage[sizeof(MscBridge)];
// The engine (1.1 KB: both 512-byte block buffers) is in the SDRAM slack window, not SRAM_U (spec 3.4 item 5): main loop only (placement-new in start(), then disk_pass/disk_state), the SDHC is POLLED
// (the CPU moves every block through its FIFO), and the USB DMA touches only the link's 64-byte slots (mem_map.h), so nothing but the core reads or writes these bytes. start() constructs it; null until then.
O3OS_SDRAM_NOINIT alignas(o3os::usb::msc::Engine) unsigned char g_engine_storage[sizeof(o3os::usb::msc::Engine)];
alignas(o3os::usb::MscPump) unsigned char g_pump_storage[sizeof(o3os::usb::MscPump)];
o3os::usb::MscThrottle g_throttle;  // Q11: main loop only (fed by set_render_load, asked by disk_pass)
MscBridge* g_bridge = nullptr;
o3os::usb::msc::Engine* g_engine = nullptr;
o3os::usb::MscPump* g_pump = nullptr;

#if O3OS_BENCH
// `diag bench disk` (OS-USBDISK U1d): the wall time of every pump pass (follow + the rounds of card I/O, interrupts included), main loop only. Bench images only; a release image has none of it.
o3os::platform::mk66::bench::disk::PassStat g_pass_stat;
#endif

// The pump publishes an IN packet: pend the USB interrupt (STIR, privileged) so the descriptor is armed at once and not at the next start of frame. Main loop; the interrupt is the only
// reader of the rings, so a spurious pend costs one empty pass of the handler.
void kick_usb() { regs::nvic_stir() = soc::kIrqNumber; }

// A mode change in progress (request_mode / service): written and read by the main thread only.
constexpr std::uint32_t kDrainMs = 120;   // the console's reply leaves over the CDC port first (the host polls the bulk IN endpoint every few ms)
constexpr std::uint32_t kDetachMs = 700;  // D+ low for this long: long enough for any host to drop the old device and its drivers
struct Change {
  std::uint8_t phase = 0;  // 0 none, 1 draining, 2 detached
  o3os::usb::Mode mode = o3os::usb::Mode::Standard;
  o3os::usb::Profile profile = kBootProfile;
  bool usb_return = true;
  std::uint32_t since_ms = 0;
};
Change g_change;
// OS-USBOUTS (ADR 0012 amendment 8, rule 1): the WISH for the return (the device setting, the console's `usb return`) is kept apart from the PRESENTED value in g_desc.cfg.usb_return, which is
// `wish && channels < 10` (usb::return_presented). Main thread only; set when a request is ACCEPTED (queued, or taken at once in STANDARD / the same layout), never on a refusal and never by a
// DISK request the DISK set accepts as it is. The ISR side reads only g_desc.cfg (by reference, through the ControlPipe): it is never written outside phase 0.
bool g_return_wish = true;
void queue_change(o3os::usb::Mode mode, o3os::usb::Profile profile, bool wish) noexcept {  // the three steps of service() follow; the callers have checked phase == 0
  g_return_wish = wish;  // committed at queue time: phases 1 and 2 cannot be refused
  g_change.mode = mode;
  g_change.profile = profile;
  g_change.usb_return = o3os::usb::return_presented(profile, wish);
  g_change.since_ms = boot::uptime_ms_low();
  g_change.phase = 1;
}
bool g_disk_boot = false;   // OS-USBDISK: start() presented DISK (the boot disk): final until a restart; the running mode's layout (disk_layout_enter) never sets it (main thread only)
bool g_powered_off = false;  // OS-POWER P1: detach_for_power() ran: no later mode change, no attach (main thread only)

// OS-005c2: the MIDI timing core's ends of the two MIDI rings (bind_midi). The interrupt reads the three pointers; bind_midi() writes them with interrupts masked.
o3os::midi::UsbMidiOut* volatile g_midi_out = nullptr;
o3os::midi::RtInRing* volatile g_midi_in = nullptr;
const o3os::midi::TimeRef* volatile g_midi_ref = nullptr;
o3os::midi::UsbBindStats g_midi_stats;

static_assert(alignof(o3os::usb::BdtTable) == 512, "the buffer descriptor table is 512-byte aligned by type (ADR 0011 lesson 1)");

// The rare events of the bus, as log lines. Called from the USB interrupt: console::log is wait-free.
void on_usb_event(void*, o3os::usb::UsbEvent event, std::uint32_t arg) {
  using o3os::usb::UsbEvent;
  switch (event) {
    case UsbEvent::BusReset:
      console::log("usb: bus reset");
      break;
    case UsbEvent::Addressed:
      console::log_dec("usb: address", arg);
      break;
    case UsbEvent::Configured:
      console::log("usb: configured");
      break;
    case UsbEvent::Deconfigured:
      console::log("usb: deconfigured");
      break;
    case UsbEvent::Suspended:
      console::log("usb: suspend");
      break;
    case UsbEvent::Resumed:
      console::log(arg != 0u ? "usb: resume (frames again)" : "usb: resume");
      break;
    case UsbEvent::Halted:
      console::log_hex("usb: halt endpoint", arg);
      break;
    case UsbEvent::HaltCleared:
      console::log_hex("usb: halt cleared endpoint", arg);
      break;
  }
}

// The streams started or stopped (ISR context, from usb::IsoAudio): the engine's stem tap and the return pull follow the host's alternate settings, so an
// idle device pays nothing for either. StemTap::set_enabled and Graph::set_return_enabled are single atomic stores.
void on_streams(void*, bool in_active, std::uint32_t channels, bool out_active) {
  o3os::audio::Graph& graph = audio_engine().graph();
  graph.stems().set_enabled(in_active);
  graph.set_return_enabled(out_active);
  console::log_dec(in_active ? "usb: stems stream, channels" : "usb: stems stream stopped, was", channels);
  console::log(out_active ? "usb: return stream on" : "usb: return stream off");
}

const char* mode_log(o3os::usb::Mode mode) noexcept {
  switch (mode) {
    case o3os::usb::Mode::Audio:
      return "usb: pull-up on, AUDIO";
    case o3os::usb::Mode::Disk:
      return "usb: pull-up on, DISK";
    case o3os::usb::Mode::Standard:
      break;
  }
  return "usb: pull-up on, STANDARD";
}

constexpr std::uint32_t kOscillatorWaitUs = 100;  // margin for the 48 MHz oscillator to start: not from the RM

// Step 4 of the sequence, the module on from off: the table, every flag (the STAT FIFO is four deep and a write of TOKDNE pops one entry, so four writes empty it), CTL = USBENSOFEN, USBCTRL. start() and
// attach() both run exactly this (the module is off before it in both: detach() switches it off), so a runtime attach starts from the state of a cold start and not from the previous layout's.
void module_on() noexcept {
  const std::uint32_t address = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_bdt));
  usb_reg(soc::kOffBdtPage1) = soc::bdt_page1(address);
  usb_reg(soc::kOffBdtPage2) = soc::bdt_page2(address);
  usb_reg(soc::kOffBdtPage3) = soc::bdt_page3(address);
  for (unsigned i = 0; i < 4u; ++i) {
    usb_reg(soc::kOffIstat) = 0xFF;
  }
  usb_reg(soc::kOffErrstat) = 0xFF;
  usb_reg(soc::kOffOtgIstat) = 0xFF;
  usb_reg(soc::kOffCtl) = o3os::usb::sie::kCtlUsbEnSofEn;
  usb_reg(soc::kOffUsbCtrl) = soc::kUsbCtrlRun;
}

}  // namespace

bool start(o3os::usb::Mode mode, o3os::usb::Profile profile, bool wish) noexcept {
  g_return_wish = wish;
  const bool usb_return = o3os::usb::return_presented(profile, wish);  // what the descriptors carry
  const std::uintptr_t table = reinterpret_cast<std::uintptr_t>(&g_bdt);
  // 0. Preconditions. Nothing is touched, and the line stays down, if the table or the descriptors are wrong.
  if ((table & (o3os::usb::kBdtAlign - 1u)) != 0u) {
    console::log_hex("usb: BDT not 512-byte aligned, pull-up off:", static_cast<std::uint32_t>(table));
    return false;
  }
  // The set of this first enumeration (OS-USBAUDIO). The static constructor built STANDARD with the boot layout; the descriptors are rebuilt here, before the pull-up and with the interrupt line
  // still off, so no host has seen anything else. A layout the engine cannot feed, or a set that does not build, is STANDARD with the boot layout.
  if (mode != g_desc.cfg.mode || profile != g_desc.cfg.profile || usb_return != g_desc.cfg.usb_return) {
    if (!o3os::audio::StemMap::supported(profile) || !g_desc.build(mode, profile, usb_return)) {
      console::log("usb: boot layout refused, STANDARD");
      (void)g_desc.build(o3os::usb::Mode::Standard, kBootProfile, true);
    }
    g_pipe.rebind(g_desc.cfg, g_desc.device, g_desc.device_len, g_desc.config, g_desc.config_len);
  }
  g_disk_boot = g_desc.cfg.mode == o3os::usb::Mode::Disk;
  if (g_desc.device_len != o3os::usb::kDeviceDescriptorSize || g_desc.config_len < 9u ||
      (g_desc.config[2] | (g_desc.config[3] << 8)) != static_cast<int>(g_desc.config_len)) {
    console::log("usb: descriptors did not build, pull-up off");
    return false;
  }
  g_dev.set_event_hook(&on_usb_event, nullptr);
  // AUDIO mode's streams, bound to the engine's stem ring and the return reader (nothing runs until a host selects an alternate setting in AUDIO mode).
  {
    o3os::audio::Graph& graph = audio_engine().graph();
    g_iso = new (g_iso_storage) o3os::usb::IsoAudio(graph.stems().ring(), g_return_ring, g_return_reader, g_stream_counters);
    g_iso->set_hook(&on_streams, nullptr);
    g_dev.bind_audio(g_iso);
    // DISK mode's link and engine (nothing is armed unless the descriptor set is DISK and the host configures it): bound before the interrupt line is enabled.
    g_bridge = new (g_bridge_storage) MscBridge(storage::device::block(), storage::device::swap());
    g_bridge->set_blocking_hook(&appimg::service_while_blocked);  // a card that follow() starts blocks for its initialisation: the display stays serviced (U1c part 2b review, minor 1)
    std::memset(g_engine_storage, 0, sizeof g_engine_storage);  // the SDRAM holds whatever it held: the engine's members the constructor leaves alone (the buffers) read zero, as they did in .bss
    g_engine = new (g_engine_storage) o3os::usb::msc::Engine(*g_bridge);
    g_pump = new (g_pump_storage) o3os::usb::MscPump(g_msc_link, *g_engine, &kick_usb);
    g_dev.bind_msc(&g_msc_link);
    g_dev.bind_trace(&g_ep0_trace);
    graph.attach_return(&g_return_reader);
    g_map.reset(g_desc.cfg.profile);  // (the map class starts at profile 10; the boot layout is the one the caller chose, kBootProfile by default)
    g_map.apply(graph.stems());
  }

  // 1. Clock: the IRC48M as the USB clock, gated on.
  regs::sim_sopt2() = (regs::sim_sopt2() & ~soc::kSopt2PllFllSelMask) | soc::kSopt2UsbBits;  // read-modify-write: CLKOUTSEL and FBSL are the SDRAM's
  at<std::uint32_t>(soc::kSimClkdiv2) = soc::kClkdiv2;
  regs::sim_scgc4() = regs::sim_scgc4() | soc::kScgc4UsbOtg;

  // 2. The system MPU: the USB master may read and write region 0.
  at<std::uint32_t>(soc::kMpuRgdaac0) = at<std::uint32_t>(soc::kMpuRgdaac0) | soc::kMpuUsbMaster;

  // 3. The oscillator and its clock recovery.
  usb_reg(soc::kOffClkRecoverIrcEn) = soc::kClkRecoverIrcEnable;
  usb_reg(soc::kOffClkRecoverCtrl) = soc::kClkRecoverCtrlEnable;
  boot::delay_us(kOscillatorWaitUs);

  // 4. The table, the flags, the module on.
  module_on();

  // 5. Endpoint 0 and the table ready (the state a bus reset leaves), interrupts enabled in the module.
  if (g_desc.cfg.mode == o3os::usb::Mode::Disk) {
    g_ep0_trace.arm();  // (the interrupt line is still off)
  }
  g_dev.prepare();

  // 6. The interrupt line.
  regs::nvic_icpr(soc::kIrqNumber) = regs::irq_bit(soc::kIrqNumber);
  regs::nvic_iser(soc::kIrqNumber) = regs::irq_bit(soc::kIrqNumber);

  // 7. The pull-up, last.
  usb_reg(soc::kOffControl) = soc::kControlPullUp;
  g_attached = true;
  console::log(mode_log(g_desc.cfg.mode));
  return true;
}

bool attached() noexcept { return g_attached; }

o3os::usb::Mode mode() noexcept { return g_desc.cfg.mode; }
o3os::usb::Profile profile() noexcept { return g_desc.cfg.profile; }
bool return_enabled() noexcept { return g_desc.cfg.usb_return; }  // the PRESENTED return
bool return_wanted() noexcept { return g_return_wish; }            // the wish (the setting): what the syncs, the console and the DISK hand-over carry
std::uint8_t change_phase() noexcept { return g_change.phase; }
o3os::usb::Mode pending_mode() noexcept { return g_change.mode; }
o3os::usb::Profile pending_profile() noexcept { return g_change.profile; }
bool pending_return() noexcept { return g_change.usb_return; }
o3os::audio::StemMap& stem_map() noexcept { return g_map; }
void apply_stem_map() noexcept { g_map.apply(audio_engine().graph().stems()); }
const o3os::usb::IsoAudio* iso() noexcept { return g_iso; }

void reset_stream_counters() noexcept {
  std::uint32_t primask;
  asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
  g_stream_counters.reset();
  asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}

bool request_mode(o3os::usb::Mode mode, o3os::usb::Profile profile) noexcept { return request_layout(mode, profile, g_return_wish); }

bool request_layout(o3os::usb::Mode mode, o3os::usb::Profile profile, bool wish) noexcept {
  if (g_powered_off || g_iso == nullptr || g_change.phase != 0u || !o3os::audio::StemMap::supported(profile)) {
    return false;
  }
  if (g_desc.cfg.mode == o3os::usb::Mode::Disk) {
    // The DISK set is final until a restart (OS-USBDISK boot disk; v0.6.8: the app's mode sync took the default USB AUDIO for a change and re-enumerated the unit as AUDIO in the middle of the computer's
    // enumeration). Nothing re-enumerates away from it or inside it: the same layout asked again is Ok (nothing to do), any other is refused (usb::disk_layout_accepts, host-tested). The running mode (U2)
    // leaves DISK by its own call, not through here.
    const bool ok = o3os::usb::disk_layout_accepts(g_desc.cfg.profile, g_desc.cfg.usb_return, mode, profile, o3os::usb::return_presented(profile, wish));
    if (ok) {
      g_return_wish = wish;  // the presented set is the same: the wish is stored (no re-enumeration), so a later layout change uses it
    }
    return ok;
  }
  if (mode == o3os::usb::Mode::Standard && g_desc.cfg.mode == o3os::usb::Mode::Standard) {
    // STANDARD's descriptors depend on neither the profile nor USB RETURN: nothing to re-enumerate, the next AUDIO attach uses them.
    g_return_wish = wish;
    g_desc.cfg.usb_return = o3os::usb::return_presented(profile, wish);
    if (profile != g_map.profile()) {
      g_map.reset(profile);
      g_desc.cfg.profile = profile;
      apply_stem_map();
    }
    return true;
  }
  if (mode == g_desc.cfg.mode && profile == g_desc.cfg.profile && o3os::usb::return_presented(profile, wish) == g_desc.cfg.usb_return) {
    g_return_wish = wish;  // the same set is presented (a wish for the return at 10 channels changes nothing the host sees): kept, no re-enumeration
    return true;
  }
  queue_change(mode, profile, wish);
  return true;
}

bool disk_layout_enter(o3os::usb::Profile profile, bool wish) noexcept {
  if (g_powered_off || g_iso == nullptr || g_change.phase != 0u || g_desc.cfg.mode == o3os::usb::Mode::Disk || !o3os::audio::StemMap::supported(profile)) {
    return false;
  }
  queue_change(o3os::usb::Mode::Disk, profile, wish);  // (always a re-enumeration: the Disk set is another device, even from STANDARD)
  return true;
}

bool disk_layout_leave(o3os::usb::Mode mode, o3os::usb::Profile profile, bool wish) noexcept {
  if (g_powered_off || g_iso == nullptr || g_change.phase != 0u || g_desc.cfg.mode != o3os::usb::Mode::Disk || g_disk_boot || mode == o3os::usb::Mode::Disk ||
      !o3os::audio::StemMap::supported(profile)) {
    return false;  // the boot disk's set is final (a reset is its only exit); DISK to DISK is no way out
  }
  queue_change(mode, profile, wish);
  return true;
}

namespace {

// Step 2 of a mode change: D+ released, the interrupt off, the module off (CTL = 0: it stays off until attach() runs step 4 again, as at a cold start), every stream stopped (the hook turns the engine's taps off).
// With the interrupt masked nothing else touches the device objects, so they are rebuilt in attach() without a lock.
void detach() noexcept {
  usb_reg(soc::kOffControl) = 0;
  regs::nvic_icer(soc::kIrqNumber) = regs::irq_bit(soc::kIrqNumber);
  usb_reg(soc::kOffCtl) = 0;
  g_ep0_trace.disarm();
  g_attached = false;
  g_iso->reset_session(g_desc.cfg.speed);
}

// Step 3: the new descriptor set, the pipe and the device over it, then the module on and the pull-up as in start() (steps 4 to 7 of the sequence at the top of the file).
void attach() noexcept {
  if (!g_desc.build(g_change.mode, g_change.profile, g_change.usb_return)) {
    console::log("usb: descriptors did not build, back to STANDARD");
    g_desc.build(o3os::usb::Mode::Standard, kBootProfile, true);
  }
  if (g_desc.cfg.profile != g_map.profile()) {
    g_map.reset(g_desc.cfg.profile);
  }
  apply_stem_map();
  g_stream_counters.reset();
  g_pipe.rebind(g_desc.cfg, g_desc.device, g_desc.device_len, g_desc.config, g_desc.config_len);
  module_on();
  if (g_desc.cfg.mode == o3os::usb::Mode::Disk) {
    g_ep0_trace.arm();  // (the interrupt line is off: the main loop is the writer for the prepare() below, the interrupt from the pull-up on)
  }
  g_dev.prepare();
  regs::nvic_icpr(soc::kIrqNumber) = regs::irq_bit(soc::kIrqNumber);
  regs::nvic_iser(soc::kIrqNumber) = regs::irq_bit(soc::kIrqNumber);
  usb_reg(soc::kOffControl) = soc::kControlPullUp;
  g_attached = true;
  console::log(mode_log(g_desc.cfg.mode));
}

}  // namespace

void detach_for_power() noexcept {
  if (g_powered_off) {
    return;
  }
  g_powered_off = true;  // first: a mode change in its detached wait must not attach again
  g_change.phase = 0;
  if (g_iso != nullptr && g_attached) {
    detach();  // D+ released, the interrupt off, the streams stopped (the same steps as a mode change's detach)
  } else if (g_iso != nullptr) {
    regs::nvic_icer(soc::kIrqNumber) = regs::irq_bit(soc::kIrqNumber);  // already detached by a mode change in progress: only the interrupt and the pull-up stay off
    usb_reg(soc::kOffControl) = 0;
  }
  console::log("usb: detached for power off");
}

void irc48m_off() noexcept {
  if ((regs::sim_scgc4() & soc::kScgc4UsbOtg) == 0u) {
    return;  // the module was never clocked: its registers must not be touched (a gated peripheral bus-faults) and its oscillator never ran
  }
  usb_reg(soc::kOffClkRecoverCtrl) = 0;
  usb_reg(soc::kOffClkRecoverIrcEn) = 0;
}

void service() noexcept {
  if (g_powered_off || g_change.phase == 0u) {
    return;
  }
  const std::uint32_t elapsed = boot::uptime_ms_low() - g_change.since_ms;
  if (g_change.phase == 1u) {
    if (elapsed >= kDrainMs) {
      detach();
      console::log("usb: detached for a mode change");  // (detach() is silent: detach_for_power() has its own line)
      g_change.phase = 2;
      g_change.since_ms = boot::uptime_ms_low();
    }
    return;
  }
  if (elapsed >= kDetachMs) {
    attach();
    g_change.phase = 0;
  }
}

bool disk_open(bool writable) noexcept {
  if (g_bridge == nullptr || g_engine == nullptr) {
    return false;
  }
  g_bridge->set_writable(writable);  // before the card is opened: no block can be written under another policy
  const bool opened = g_bridge->open();
  g_engine->medium_changed();  // whatever the computer saw before this (nothing, normally) was not this card
  console::log(opened ? (writable ? "usb: disk opened, writable" : "usb: disk opened, read-only") : "usb: disk: no card, or it did not start");
  return opened;
}

void disk_pass(std::uint32_t now_ms) noexcept {
  if (g_pump == nullptr || g_desc.cfg.mode != o3os::usb::Mode::Disk || !g_attached || g_change.phase != 0u || !g_pipe.configured()) {
    return;
  }
#if O3OS_BENCH
  const std::uint32_t pass_start = regs::dwt_cyccnt();
#endif
  if (g_bridge->follow(now_ms)) {  // the slot changed under the computer: the card let go / the new one started; the engine answers UNIT ATTENTION
    g_engine->medium_changed();
    console::log(g_bridge->opened() ? "usb: disk: card changed, started" : "usb: disk: card removed or did not start");
  }
  if (g_throttle.allow(now_ms)) {  // Q11: the card is followed every pass; only the pump (the transfer) is held to every 2 ms above 85 % render load
    g_pump->step();
  }
#if O3OS_BENCH
  g_pass_stat.add(regs::dwt_cyccnt() - pass_start);
#endif
}

#if O3OS_BENCH
const o3os::platform::mk66::bench::disk::PassStat& disk_pass_stat() noexcept { return g_pass_stat; }
void bench_window_reset() noexcept {
  g_pass_stat.reset();
  g_dev.reset_isr_peak();
}
#endif

void disk_close() noexcept {
  if (g_bridge != nullptr && g_bridge->opened()) {
    g_bridge->close();
    console::log("usb: disk closed");
  }
}

void set_render_load(unsigned percent) noexcept { g_throttle.feed(percent); }
bool disk_throttled() noexcept { return g_throttle.throttled(); }
bool disk_take_eject_request() noexcept { return g_engine != nullptr && g_engine->take_eject_request(); }
void disk_unit_eject() noexcept {
  if (g_engine != nullptr) {
    g_engine->unit_eject();
  }
}

DiskState disk_state() noexcept {
  DiskState d;
  if (g_bridge == nullptr || g_engine == nullptr) {
    return d;
  }
  d.present = g_bridge->present();
  d.ejected = g_engine->ejected();
  d.writable = !g_bridge->write_protected();  // what the computer is told (the policy AND the card's own protect bit), not the policy alone
  d.busy = !g_engine->idle();
  d.blocks = g_bridge->block_count();
  d.configured = g_attached && g_desc.cfg.mode == o3os::usb::Mode::Disk && g_pipe.configured();
  return d;
}

const o3os::usb::DeviceCounters& device_counters() noexcept { return g_dev.counters(); }
const o3os::usb::ControlPipe& control() noexcept { return g_pipe; }
const o3os::usb::Counters& stream_counters() noexcept { return g_stream_counters; }
const o3os::usb::Ep0Trace& ep0_trace() noexcept { return g_ep0_trace; }
o3os::usb::CdcTxWriter& cdc_writer() noexcept { return g_writer; }
o3os::usb::CdcRxRing& cdc_rx() noexcept { return g_cdc_rx; }
o3os::usb::MidiRing& midi_to_host() noexcept { return g_midi_to_host; }
o3os::usb::MidiRing& midi_from_host() noexcept { return g_midi_from_host; }
const char* serial() noexcept { return g_desc.serial; }

void bind_midi(o3os::midi::UsbMidiOut& out, o3os::midi::RtInRing& in, const o3os::midi::TimeRef& ref) noexcept {
  std::uint32_t primask;
  asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
  g_midi_ref = &ref;
  g_midi_in = &in;
  g_midi_out = &out;
  asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}
const o3os::midi::UsbBindStats& midi_bind_stats() noexcept { return g_midi_stats; }

// The USB interrupt body: the device logic of usb/device.h with the MIDI binding around it.
//   * BEFORE the device logic, when a start of frame is pending: the frame's events go into the to-host ring, so that the same pass's on_sof() -> service_in() arms the
//     bulk-IN buffer with them and they leave in this frame, not the next. (A SOF that arrives while the device logic is already running is handled inside it, without
//     this call: that frame's events leave one frame later. Rare, and one millisecond.)
//   * AFTER it, whatever the pass wrote into the from-host ring becomes stamped bytes in the USB midiRt ring. The stamp is the cycle counter at entry.
// Bounded (midi/usb_bind.h) and wait-free; the same priority as the device logic, so neither can race the other.
namespace {
void usb_isr() noexcept {
  const std::uint32_t entry = regs::dwt_cyccnt();
  o3os::midi::UsbMidiOut* const out = g_midi_out;
  if (out != nullptr && (g_regs.istat() & o3os::usb::sie::kIstatSofTok) != 0u) {
    o3os::midi::usb_midi_sof(*out, *g_midi_ref, entry, g_midi_to_host, g_pipe.configured(), g_midi_stats);
  }
  g_dev.isr();
  o3os::midi::RtInRing* const in = g_midi_in;
  if (in != nullptr) {
    o3os::midi::usb_midi_from_host(g_midi_from_host, *in, entry, g_midi_stats);
  }
}
}  // namespace

}  // namespace o3os::platform::mk66::usb0

// The USB0 interrupt (vector 16 + 53, priority 64 from the table in irq_map.h). It is the whole of the driver's interrupt work; see
// usb/device.h for what it does and the bound on it.
extern "C" void USB0_IRQHandler() { o3os::platform::mk66::usb0::usb_isr(); }
