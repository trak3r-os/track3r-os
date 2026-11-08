// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The USB MIDI endpoint bound to the timing core (OS-005c2; ADR 0013 D1 and D5, ADR 0012). The device logic of OS-006b (usb/device.h) moves packets between the
// bulk endpoints and two overwrite rings of 32-bit event words (usb::MidiRing); this header is the other end of those rings, the two small functions the
// platform's USB interrupt (priority 64) calls around UsbDevice::isr():
//
//   usb_midi_sof()        BEFORE isr(), when the interrupt was raised with a start-of-frame pending: takes the events of this frame from the timing core's
//                         UsbMidiOut (those due at or before the frame's start, real-time first: midi/usb_out.h) and writes them into the to-host ring, so that
//                         the isr()'s own on_sof() -> service_in() arms the bulk-IN buffer with them in the same millisecond. Writing them after isr() would
//                         send them a frame later.
//   usb_midi_from_host()  AFTER isr(): reads what the pass wrote into the from-host ring, expands each event word into its message bytes and pushes them, all
//                         with the interrupt's entry stamp, into the USB port's `midiRt` ring (RtInRing) for the render (midi/in.h).
//
// CONTEXTS. Both run in the USB interrupt and nowhere else, so that interrupt is the ONE producer of the to-host ring's writes (the render writes UsbMidiOut's
// own rings, not this one), the ONE consumer of the from-host ring (the usb::OverwriteRing's consumer side: the engine side of 006b's comment, which is this
// interrupt, so it never races its own producer) and the ONE producer of the USB RtInRing; the render is that ring's one consumer and UsbMidiOut's one
// producer. UsbMidiOut::frame() is its consumer: only here.
//
// The stamp of an incoming event is the interrupt's entry, not the instant the packet crossed the wire: the host's frame quantisation (0.5 ms on average) is the
// documented input latency of the USB port (MidiIn::Config::latency_cycles, open item O24), which the user's IN OFFSET cancels.
// When the host is not listening (the device is not configured) the frame's events are discarded and counted, so that a long absence does not leave a backlog
// that would spill out in one burst on the next connection.
//
// Bounded: at most kMaxReads reads of 16 events from the from-host ring per call, the writes of one frame (16 events at most), no allocation, no blocking.

#include <o3os/midi/in.h>
#include <o3os/midi/message.h>
#include <o3os/midi/time_ref.h>
#include <o3os/midi/usb_out.h>
#include <o3os/usb/cdc.h>

#include <cstdint>

namespace o3os::midi {

// The message bytes of one USB-MIDI event word (USB-MIDI 1.0 table 4-1): the number written to `out` (0..3): 0 for the reserved code index numbers 0 and 1
// (miscellaneous, cable events), 1 for 0x5 (single-byte system common, SysEx ends with one byte) and 0xF (single byte), 2 for 0x2, 0x6, 0xC, 0xD, 3 for the rest.
// Exception: CIN 0x2 with the status byte F2 (the SPP is a three-byte message even when a host packs it with the two-byte code index) takes 3.
// The cable number (the high nibble of the first byte of the word) is ignored: the device has one port.
std::uint32_t usb_event_bytes(std::uint32_t event, std::uint8_t out[3]) noexcept;

struct UsbBindStats {
  std::uint32_t frames = 0;           // start-of-frame calls that had a time base
  std::uint32_t events_out = 0;       // event words written to the host's ring
  std::uint32_t discarded = 0;        // frame events dropped because the host was not listening
  std::uint32_t no_time = 0;          // frames before the render published a time base
  std::uint32_t events_in = 0;        // event words taken from the host's ring
  std::uint32_t bytes_in = 0;         // message bytes pushed into the RtInRing
  std::uint32_t in_ring_full = 0;     // bytes dropped: the render has not drained the ring
};

inline constexpr std::uint32_t kUsbMaxReads = 4;

// `frame_cycles`: the cycle counter at the interrupt's entry. `host_listening`: the device is configured.
std::uint32_t usb_midi_sof(UsbMidiOut& out, const TimeRef& ref, std::uint32_t frame_cycles, usb::MidiRing& to_host, bool host_listening,
                           UsbBindStats& st) noexcept;

// `stamp`: the cycle counter at the interrupt's entry. Returns the number of bytes pushed.
std::uint32_t usb_midi_from_host(usb::MidiRing& from_host, RtInRing& in, std::uint32_t stamp, UsbBindStats& st) noexcept;

}  // namespace o3os::midi
