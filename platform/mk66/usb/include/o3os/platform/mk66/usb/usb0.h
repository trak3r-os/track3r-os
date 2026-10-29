// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66's USB0 device driver (OS-006b): the register policy, the objects of the pure usb library wired to the chip, the interrupt
// handler and the bring-up. Implemented in the MK66 image only (src/usb0.cpp); the declarations compile everywhere so the header
// check passes on every platform. The logic (control pipe, endpoints, rings) is usb and is tested on the host against a fake
// controller; what is here is the part that touches registers: README.md, "USB" lists every register sequence with its source.
//
// Two descriptor sets (the maintainer's identity 16D0:0E9F "Track3r" in both, told apart by bcdDevice): STANDARD (MIDI + CDC `diag`) and, since OS-006d, AUDIO
// (UAC2 stems IN + stereo return OUT with explicit feedback, plus MIDI and CDC; ADR 0012). The set of the first enumeration is the caller's (start(): since OS-USBAUDIO the device
// setting USB AUDIO decides it, default AUDIO; before it every boot was STANDARD), and a later change is request_mode() from the main loop (the `diag usb mode` command, which is
// runtime only; Config's USB AUDIO and a project's USB part): the device detaches, waits, and attaches with the other set. The firmware-update path (OS-006c) and `diag` run over
// the CDC function, present in both.
//
// Contexts: start() runs once in the main thread; the interrupt handler (USB0_IRQHandler, priority 64, ADR 0013 D1) runs the
// usb::UsbDevice and touches the buffer descriptors and the endpoints alone; everything the main loop and the engine share with it is
// a lock-free ring (usb/device.h).

#include <o3os/audio/stem_map.h>
#include <o3os/midi/in.h>
#include <o3os/midi/time_ref.h>
#include <o3os/midi/usb_bind.h>
#include <o3os/midi/usb_out.h>
#include <o3os/usb/cdc.h>
#include <o3os/usb/control.h>
#include <o3os/usb/counters.h>
#include <o3os/usb/device.h>
#include <o3os/usb/ep0_trace.h>
#include <o3os/usb/iso_audio.h>

#if O3OS_BENCH
#include <o3os/platform/mk66/bench_disk.h>
#endif

#include <cstdint>

namespace o3os::platform::mk66::usb0 {

// The module is on only between an attach and the next detach (step 4 of the sequence in usb0.cpp is shared by start() and every attach, so a runtime attach starts from the state of a cold start).
// Brings the module up and, last, switches the D+ pull-up on so that the host sees the device: IRC48M clock and its recovery, the
// system MPU grant, the buffer descriptor table, the endpoint 0 receive descriptors, the interrupt. Returns false, with the pull-up
// OFF and the reason in the log, if a precondition fails (the descriptor table is not 512-byte aligned, the descriptors did not build or
// are inconsistent). Call once, in the main thread, after the clock tree, the SDRAM, the interrupt priorities and the log.
// The descriptor set of that first enumeration is `mode` with `profile` and `usb_return` (OS-USBAUDIO: chosen before the pull-up, so the host sees one device and no STANDARD-then-AUDIO
// switch). A profile the engine cannot feed, or descriptors that do not build, fall back to STANDARD (logged). In STANDARD the profile and the return are only remembered for a later AUDIO attach.
bool start(o3os::usb::Mode mode, o3os::usb::Profile profile, bool wish) noexcept;  // (`wish`: the return the setting wants; the descriptors carry it only below 10 channels)

// True once start() switched the pull-up on.
bool attached() noexcept;

// Read-only views for `diag usb` and the MIDI binding (OS-005). The MIDI rings are the engine's ends of the two MIDI streams: the
// engine writes midi_to_host() and reads midi_from_host(), each from one context. Since OS-005c2 that one context is the USB interrupt itself
// (bind_midi below): it is the only writer of midi_to_host() (the frame's events from the MIDI timing core) and the only reader of
// midi_from_host() (the host's bytes, stamped), so nothing else may touch either ring.
const o3os::usb::DeviceCounters& device_counters() noexcept;
const o3os::usb::ControlPipe& control() noexcept;
// OS-USBDISK U2a: the first events of endpoint 0 in the last DISK attach (`usb ep0`); empty before one. Kept across the rollback to the previous layout.
const o3os::usb::Ep0Trace& ep0_trace() noexcept;
const o3os::usb::Counters& stream_counters() noexcept;
o3os::usb::CdcTxWriter& cdc_writer() noexcept;
o3os::usb::CdcRxRing& cdc_rx() noexcept;
o3os::usb::MidiRing& midi_to_host() noexcept;
o3os::usb::MidiRing& midi_from_host() noexcept;
const char* serial() noexcept;

// OS-005c2: binds the two MIDI rings to the MIDI timing core (midi/usb_bind.h). From now on the USB interrupt (priority 64) is the ONE consumer of `out` (the frame's events
// go into midi_to_host() before the device logic arms the bulk-IN buffer, see usb0.cpp) and the ONE producer of `in` (the host's bytes, stamped at the interrupt's
// entry), and it reads `ref` for the time. The three objects must outlive the USB interrupt (statics); call once, main thread, any time after start(): the binding is
// published with interrupts masked, so the interrupt sees all of it or none. Until then the from-host ring is simply overwritten and nothing is sent to the host.
void bind_midi(o3os::midi::UsbMidiOut& out, o3os::midi::RtInRing& in, const o3os::midi::TimeRef& ref) noexcept;
const o3os::midi::UsbBindStats& midi_bind_stats() noexcept;

// ---- AUDIO mode (OS-006d, ADR 0012) --------------------------------------------------------------------------------------------------------
// The descriptor set presented now, and the profile it carries (the channel layout of the stems stream), and whether it carries the return stream
// (USB RETURN, OS-USBRET: false is capture only, no OUT interface and no feedback endpoint). The layout of a boot is profile 8 with the return on.
o3os::usb::Mode mode() noexcept;
o3os::usb::Profile profile() noexcept;
bool return_enabled() noexcept;  // the return the descriptors PRESENT: the wish, and fewer than 10 channels (OS-USBOUTS, ADR 0012 amendment 8)
// The WISH for the return: the device setting USB RETURN / the console's `usb return`, kept whatever the profile (setting On at 10 channels presents nothing, and stays On). What the syncs, the console and
// the DISK hand-over pass back to request_layout() / disk_layout_*(), so that a move 10 -> 4 -> 10 never stores a lost return. Main thread.
bool return_wanted() noexcept;

// Asks for another descriptor set. Non-blocking: the change is carried out by service() from the main loop, in three steps: wait a moment for the
// console's reply to leave over the CDC port, detach (pull-up off, streams stopped, the stem tap off), wait for the host to notice, then rebuild the
// descriptors and attach again. The host re-enumerates (CDC `diag` terminal sessions end; MIDI and the firmware-update path come back with the new set).
// False, nothing changed, when a change is already in progress, the profile is one the engine cannot feed (P4S), or the driver is not running.
// Asking for the set that is presented now is a successful no-op. A profile different from the current one resets the stem map to that profile's
// factory slots (the slot masks mean something else on another layout).
bool request_mode(o3os::usb::Mode mode, o3os::usb::Profile profile) noexcept;  // keeps the current wish for USB RETURN
// The same with USB RETURN chosen too. In AUDIO a different mode, profile or return re-enumerates; in STANDARD (whose descriptors depend on none of them) the
// choice is only remembered for the next AUDIO attach.
// `wish` is the setting's value; the set presented carries it only below 10 channels, and the same set asked again (a wish that changes nothing the host sees) is a no-op that keeps the wish. A refusal
// (false) leaves the wish as it was.
bool request_layout(o3os::usb::Mode mode, o3os::usb::Profile profile, bool wish) noexcept;

// 0: nothing in progress, 1: waiting for the reply to leave, 2: detached. The target of a change in progress.
std::uint8_t change_phase() noexcept;
o3os::usb::Mode pending_mode() noexcept;
o3os::usb::Profile pending_profile() noexcept;
bool pending_return() noexcept;

// The state machine of request_mode(). Main loop, every pass (console::service() calls it). No waits beyond a few cycles.
void service() noexcept;

// OS-POWER P1 (stage D of the shutdown, docs/specs/OS-POWER.md 4.1): the detach of request_mode() without the attach that follows it. D+ released (the computer sees the device go), the interrupt
// masked, every stream stopped (the stem tap and the return pull follow). Never undone: a mode change in progress is dropped, no later request_layout() is taken and service() does nothing from here;
// the run ends in a restart, which enumerates again. Main loop. Idempotent.
void detach_for_power() noexcept;

// OS-POWER P2 (the VLPR off, docs/specs/OS-POWER.md 4.4 step 3): the 48 MHz oscillator that clocks the module (IRC48M, started by start()) off, because "it should be disabled by software prior to VLPR entry"
// (RM 6.7.2). Clock recovery off first, then the oscillator and its regulator (CLK_RECOVER_IRC_EN cleared). Only after detach_for_power(): the module must be detached, with its interrupt off. A module that was never
// clocked (SIM_SCGC4 gate off) has nothing running and its registers are not touched. Main loop, interrupts masked; no wait, never returns an error. Never undone: the run ends in a reset.
void irc48m_off() noexcept;

// ---- DISK mode (OS-USBDISK U1c, docs/specs/OS-USBDISK.md 3.4) ------------------------------------------------------------------------------
// The Mass Storage function of the DISK descriptor set (CDC + MIDI + MSC, mode Disk). start() builds the link, the engine and the pump and binds the link to the device; the USB interrupt only
// moves packets between the controller and the link's rings, and everything below is main loop only.
// disk_open(): after the Disk layout is on the bus and the card's owner is Host (storage::device::boot_to_host() or hand_to_host(); 4.2: the owner last), starts the card for the computer
// (MscBridge::open, blocks for the card's initialisation: the caller keeps the display serviced) and tells the engine the medium is new. True when the card started. `writable` is the policy
// for this session (MscBridge::set_writable, applied before the card is opened and kept for any card put in later): false exports the card write-protected (WP bit, DATA PROTECT).
bool disk_open(bool writable) noexcept;
// Every pass of the main loop: nothing unless the mode is DISK, the host has configured it and no layout change is running. Then: follows the card slot (a settled insert or pull: close, open,
// Engine::medium_changed) and runs the pump (the SCSI state machine, one block of card I/O at most per round, up to MscPump::kMaxRounds rounds). `now_ms` is the millisecond clock.
void disk_pass(std::uint32_t now_ms) noexcept;
struct DiskState {
  bool configured = false;  // DISK is presented and the computer has configured the device
  bool present = false;     // a card is started and answers
  bool ejected = false;     // the computer ejected the medium (START STOP UNIT)
  bool writable = false;    // the disk is exported writable: the session's policy allows it and the card has no write-protect bit of its own (!MscBridge::write_protected)
  bool busy = false;        // a SCSI command is in flight (Engine::idle() is false): a WRITE(10) may be half done
  std::uint32_t blocks = 0;
};
DiskState disk_state() noexcept;
// OS-USBDISK U2a (the running mode, docs/specs/OS-USBDISK.md 4.2, 6.1; DiskShare drives these). disk_layout_enter(): queues the DISK set (CDC + MIDI + MSC) over the layout presented now (request_mode's three steps:
// the reply leaves, detach, attach); false when a change is in progress, DISK is already presented or the profile cannot be fed. disk_layout_leave(): queues the layout `mode`/`profile`/`usb_return` out of DISK, only
// when DISK was NOT the boot layout (the boot disk's exit is a reset). request_layout()/request_mode() keep refusing every change while DISK is presented.
bool disk_layout_enter(o3os::usb::Profile profile, bool wish) noexcept;
bool disk_layout_leave(o3os::usb::Mode mode, o3os::usb::Profile profile, bool wish) noexcept;
// The computer ejected the medium (START STOP UNIT): true once. The unit's eject: the computer's next command is UNIT ATTENTION, then NOT READY, MEDIUM NOT PRESENT. Main loop.
// Q11: the render's load in percent of the block budget, once a pass (main loop); above 85 % (smoothed, released below 80 %) disk_pass() runs the pump every 2 ms only (usb/msc_throttle.h). disk_throttled(): `diag usb` and the tests.
void set_render_load(unsigned percent) noexcept;
bool disk_throttled() noexcept;
bool disk_take_eject_request() noexcept;
void disk_unit_eject() noexcept;
// OS-POWER P1 (stage C of the shutdown in the boot disk mode, docs/specs/OS-POWER.md section 5): the MSC close. The engine answers NOT READY, MEDIUM NOT PRESENT from here (a block in flight has finished:
// this is the main loop, which runs the pump); the computer sees the unplug at stage D without an eject. Idempotent; nothing when no disk was opened.
void disk_close() noexcept;
#if O3OS_BENCH
// `diag bench disk` (OS-USBDISK U1d, bench images only): the wall time of the pump's passes since the window opened, and the window's reset (the passes and the USB interrupt's worst entry).
const o3os::platform::mk66::bench::disk::PassStat& disk_pass_stat() noexcept;
void bench_window_reset() noexcept;
#endif

// The stem map (runtime only; see audio/stem_map.h). Main loop. After editing a slot call apply_stem_map().
o3os::audio::StemMap& stem_map() noexcept;
void apply_stem_map() noexcept;

// The streams, for `diag usb audio`: null before start(). The getters are plain words, torn at most across fields.
const o3os::usb::IsoAudio* iso() noexcept;
// Zeroes the stream counters (`diag usb audio reset`), with the USB interrupt masked for the few words.
void reset_stream_counters() noexcept;

}  // namespace o3os::platform::mk66::usb0
