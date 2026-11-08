// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The text of the `diag` commands that are specific to the MK66 platform (ADR 0011 item 1, OS-006b): `usb`, `stage`, `crash`, `log`,
// `input`, `version`. Pure functions over plain structs, tested on the host and on QEMU, so the command handlers in the image
// (usb/src/diag_commands.cpp) only gather numbers and print what these return. The `cpu` and `mem` texts are the audio library's
// formatters (audio/diag.h) and are bound the same way. No printf: audio::diag::Text is the bounded builder.

#include <o3os/audio/diag.h>
#include <o3os/audio/stem_map.h>
#include <o3os/core/input_event.h>
#include <o3os/hal/input.h>
#include <o3os/platform/mk66/audio.h>
#include <o3os/platform/mk66/crash_record.h>
#include <o3os/platform/mk66/usb/boot_faults.h>
#include <o3os/usb/cdc.h>
#include <o3os/usb/control.h>
#include <o3os/usb/counters.h>
#include <o3os/usb/device.h>
#include <o3os/usb/diag_log.h>

#include <cstddef>
#include <cstdint>

namespace o3os::platform::mk66::console {

// Everything `diag usb` prints, gathered by the image from the driver's objects.
struct UsbReport {
  const o3os::usb::DeviceCounters* device;
  const o3os::usb::ControlCounters* control;
  const o3os::usb::Counters* audio;  // the ADR 0012 stream counters: all zero in STANDARD mode (no stream is driven yet)
  o3os::usb::DeviceState state;
  std::uint8_t address;
  bool suspended;
  bool dtr;
  bool rts;
  o3os::usb::LineCoding coding;
  std::uint32_t coding_sets;
  std::uint32_t control_sets;
  std::uint32_t tx_bytes_written;     // bytes the console wrote to the CDC transmit ring
  std::uint32_t tx_dropped_chunks;    // chunks the writer overwrote because the host did not read
  std::uint32_t rx_pending_chunks;    // chunks waiting for the main loop
  std::uint32_t bootstrap_ok;         // 1 when the pull-up was enabled (descriptors valid, table aligned), else 0
  std::uint32_t usb_clock_hz;         // the clock the module is fed (the IRC48M)
};

// Everything `diag usb audio` prints (OS-006d), gathered by the image from the driver's objects.
struct UsbAudioReport {
  o3os::usb::Mode mode;           // the descriptor set presented now
  o3os::usb::Profile profile;
  std::uint8_t change;            // 0 none, 1 waiting for the console reply to leave, 2 detached (re-enumerating)
  o3os::usb::Mode next_mode;      // what the change in progress is going to present (meaningful when change != 0)
  o3os::usb::Profile next_profile;
  bool configured;                // the host selected the configuration
  bool in_active;                 // the stems interface is at alt 1
  bool out_active;                // the return interface is at alt 1
  std::uint32_t in_channels;
  const o3os::usb::Counters* counters;
  const o3os::usb::DeviceCounters* device;
  bool locked;                    // the rate estimator has settled (about 4 s of SOFs)
  std::uint32_t sof_period_q8;    // the estimated SOF period in CPU cycles, Q8
  std::uint32_t resyncs;
  std::uint32_t rate_q16;         // frames per SOF, as estimated and as advertised
  std::uint32_t feedback_q16;
  std::int32_t trim_q16;          // the return ring's fill trim inside the advertised rate
  std::uint32_t stem_fill;        // smoothed stem ring fill in frames (0 until primed)
  std::uint32_t return_fill;      // frames in the return ring
  bool usb_return = true;         // OS-USBRET: the set presented now carries the return stream (an aggregate initialiser may leave it out)
  bool next_return = true;        // ... and the one a change in progress is going to present
};
const char* usb_mode_name(o3os::usb::Mode m) noexcept;            // "STANDARD", "AUDIO"
const char* usb_profile_name(o3os::usb::Profile p) noexcept;      // "2", "8", "10", "11", "4S"
// The user's name of a layout (OS-USBRET): channels out of the Tracker, and the stereo return into it when there is one. "10 OUT", "8 OUT / 2 IN".
// Written into `buf` (at least 20 bytes) and returned.
const char* usb_layout_name(o3os::usb::Profile p, bool usb_return, char* buf) noexcept;
std::size_t format_usb_audio(const UsbAudioReport& r, char* out, std::size_t cap) noexcept;
// `diag usb map`: one line per slot of the profile, "S3 = T3", then the sources nobody names ("heard in the master only").
std::size_t format_stem_map(const o3os::audio::StemMap& map, char* out, std::size_t cap) noexcept;

const char* device_state_name(o3os::usb::DeviceState s) noexcept;
const char* stage_name(crash::Stage s) noexcept;
// Name of an exception number as the crash record keeps it (IPSR): "HardFault", "UsageFault", "IRQ 53", "panic", ...
// Written into `buf` (at least 24 bytes), returned for chaining.
const char* exception_name(std::uint32_t exception, char* buf) noexcept;

std::size_t format_usb(const UsbReport& r, char* out, std::size_t cap) noexcept;
// One line of `usb ep0` (OS-USBDISK U2a, usb/ep0_trace.h): the entry's number, the time since the one before, the event and its fields.
std::size_t format_usb_ep0_entry(unsigned index, const o3os::usb::Ep0Trace::Entry& e, char* out, std::size_t cap) noexcept;
// The closing line of `usb ep0`: "stall retries N" (the STALL handshakes after the first of each endpoint 0 stall, Ep0Trace::stall_retries).
std::size_t format_usb_ep0_retries(std::uint32_t retries, char* out, std::size_t cap) noexcept;
// `context` is a stage/module word (crash::make_context) or crash::kUnknownContext; `reset_sources` BootInfo::reset_sources; `faults` the
// BootFault bits `main` recorded (the start-up steps that failed and were gone on from).
std::size_t format_stage(std::uint32_t context, std::uint32_t uptime_ms, std::uint16_t reset_sources, bool crash_record_found,
                         std::uint32_t faults, char* out, std::size_t cap) noexcept;
// A crash record, or "no crash record" for nullptr.
std::size_t format_crash(const crash::CrashRecord* record, char* out, std::size_t cap) noexcept;
// One log line: "  12345 ms  text".
std::size_t format_log_entry(const o3os::usb::diag::LogEntry& e, char* out, std::size_t cap) noexcept;
// One input event, as the INPUT TEST screen would show it: "pad 12 down 127", "button 3 up", "encoder 0 +2" and its tick.
std::size_t format_input_event(const core::InputEvent& e, char* out, std::size_t cap) noexcept;
// `input stats`: the health line, and (OS-BUG-PADS) a second line for the pad bus once it has any record. No change to the first line.
std::size_t format_input_stats(const hal::InputStats& s, char* out, std::size_t cap) noexcept;
// OS-BUG-PADS: the pad bus record in the two short pieces a log line (55 characters) holds, no newline: "pads 34 NAK/NAK x12 74 --/-- x0" (the chip at that address: first/last result and the
// transactions tried) and "pads clr 1 rel, lines 111>111" (bus clears and the last one's result, then SCL SDA INT before the first transaction and now: 1 = high, - = not read).
std::size_t format_pad_bus_chips(const hal::PadBusStats& s, char* out, std::size_t cap) noexcept;
std::size_t format_pad_bus_lines(const hal::PadBusStats& s, char* out, std::size_t cap) noexcept;
// `version`: the product name, the build identity, the USB identity.
std::size_t format_version(const char* build, std::uint16_t vid, std::uint16_t pid, std::uint16_t bcd_device, const char* serial,
                           char* out, std::size_t cap) noexcept;

// OS-HW1: the audio start's outcome. `result` is a hal::audio::Device::Result value; the fault detail (which wait, the raw MCR and MDR) follows
// for a ClockFault. Used by `diag audio start` and, line by line, by the boot log.
const char* audio_result_name(std::uint8_t result) noexcept;
const char* audio_fault_where_name(std::uint8_t where) noexcept;  // "mcr", "mdr", "boundaries", "none"
std::size_t format_audio_start(std::uint8_t result, const AudioClockFault& fault, char* out, std::size_t cap) noexcept;

// The reset sources of BootInfo as words: "power-on", "pin", "watchdog", "software", "lockup", joined with '+'.
std::size_t format_reset_sources(std::uint16_t sources, char* out, std::size_t cap) noexcept;

// OS-CEPTFX S2c, `ptfx quarantine`: plain scalars, so this layer needs no module headers. format_ptfx_quarantine: line `index` of the set ("ptfx q 1 id 0x8ABC crash"), and for index >= count the closing
// line "ptfx quarantine 3/16, full no, refused 0" (full: the ids refused after the 16th).
std::size_t format_ptfx_quarantine(unsigned index, unsigned count, std::uint16_t id, const char* why, unsigned refused, char* out,
                                   std::size_t cap) noexcept;

}  // namespace o3os::platform::mk66::console
