// SPDX-License-Identifier: GPL-3.0-or-later
//
// The USB MIDI endpoint bound to the timing core (midi/usb_bind.h).
#include <o3os/midi/usb_bind.h>

namespace o3os::midi {

std::uint32_t usb_event_bytes(std::uint32_t event, std::uint8_t out[3]) noexcept {
  std::uint32_t n = 0;
  switch (event & 0x0Fu) {
    case 0x5:
    case 0xF:
      n = 1;
      break;
    case 0x2:
      // A two-byte system common (MTC, Song Select) takes two bytes — but the SPP (F2) is a three-byte message, and a host that packs
      // it with this code index still means all three bytes. Decoding only two leaves the parser (midi/src/in.cpp InParser::feed)
      // waiting for the second data byte forever — every byte after it is real-time and passes through untouched — so the SPP is
      // silently ignored (OS-MIDISYNC M2b: on the unit the SPP arrived packed as CIN 0x2 and was dropped, while the CIN 0xF
      // real-time bytes, clock and Start/Continue/Stop, all worked).
      n = (static_cast<std::uint8_t>((event >> 8) & 0xFFu) == kSongPosition) ? 3u : 2u;
      break;
    case 0x6:
    case 0xC:
    case 0xD:
      n = 2;
      break;
    case 0x0:
    case 0x1:
      return 0;  // reserved: a device ignores them (usb/cdc.h midi_event_valid)
    default:  // 0x3 (three-byte system common, SysEx continues), 0x4 (SysEx starts or continues), 0x7 (SysEx ends with three), 0x8..0xB, 0xE
      n = 3;
      break;
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = static_cast<std::uint8_t>((event >> (8u * (i + 1u))) & 0xFFu);
  }
  return n;
}

std::uint32_t usb_midi_sof(UsbMidiOut& out, const TimeRef& ref, std::uint32_t frame_cycles, usb::MidiRing& to_host, bool host_listening,
                           UsbBindStats& st) noexcept {
  std::uint64_t frame_start = 0;
  if (!ref.to_timeline(frame_cycles, frame_start)) {
    ++st.no_time;  // nothing is due before the audio runs; the rings hold what the render hands over
    return 0;
  }
  ++st.frames;
  std::uint32_t ev[usb_out::kEventsPerFrame];
  const std::uint32_t n = out.frame(frame_start, ev, usb_out::kEventsPerFrame);
  if (n == 0u) {
    return 0;
  }
  if (!host_listening) {
    st.discarded += n;
    return 0;
  }
  to_host.write(ev, n);  // n <= kMidiEventsPerPacket (16) = the ring's chunk limit
  st.events_out += n;
  return n;
}

std::uint32_t usb_midi_from_host(usb::MidiRing& from_host, RtInRing& in, std::uint32_t stamp, UsbBindStats& st) noexcept {
  std::uint32_t pushed = 0;
  for (std::uint32_t r = 0; r < kUsbMaxReads; ++r) {
    std::uint32_t ev[usb::kMidiEventsPerPacket];
    const std::uint32_t n = from_host.read(ev, usb::kMidiEventsPerPacket);
    if (n == 0u) {
      break;
    }
    st.events_in += n;
    for (std::uint32_t i = 0; i < n; ++i) {
      std::uint8_t b[3];
      const std::uint32_t len = usb_event_bytes(ev[i], b);
      for (std::uint32_t k = 0; k < len; ++k) {
        if (in.push(RtItem{stamp, b[k], static_cast<std::uint8_t>(Port::Usb), 0})) {
          ++pushed;
          ++st.bytes_in;
        } else {
          ++st.in_ring_full;
        }
      }
    }
  }
  return pushed;
}

}  // namespace o3os::midi
