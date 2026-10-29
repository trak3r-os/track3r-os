// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace o3os::usb {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "USB audio samples and descriptors are little-endian and are written without byte swapping");

enum class Speed : std::uint8_t { Full, High };

// STANDARD: MIDI + CDC `diag` (the default). AUDIO: UAC2 + MIDI + CDC (opt-in, ADR 0012 Decision 1). DISK (OS-USBDISK, docs/specs/OS-USBDISK.md 3.1):
// the STANDARD function set plus one Mass Storage interface, a runtime state that is never persisted. Audio stays 1: the stored USB mode setting
// (model::kUsbModeMax) is Standard or Audio only.
enum class Mode : std::uint8_t { Standard, Audio, Disk };

// Stem profiles (ADR 0012 Decision 3, `usb.profile`): number of channels on the IN stream.
//   P2   master L/R                          2 channels
//   P8   T1..T6 + master L/R                 8   (Windows shared-mode safe)
//   P10  T1..T8 + master L/R                10   (default)
//   P11  T1..T9 + master L/R                11   (the largest 16-bit layout that fits a full-speed packet)
//   P4S  4 stereo post-pan stems + master   10
//   P4   T1, T2 + master L/R                 4   (OS-USBOUTS: the per-project OUTPUTS choice; appended, so the values of P2..P4S stay)
//   P6   T1..T4 + master L/R                 6
enum class Profile : std::uint8_t { P2, P8, P10, P11, P4S, P4, P6 };

inline constexpr std::uint32_t kMaxChannels = 11;
inline constexpr std::uint32_t kReturnChannels = 2;  // the OUT (host to device) stream: stereo USB RETURN (a device setting since OS-USBRET: off drops it)
inline constexpr std::uint32_t kBytesPerSample = 2;  // 16-bit only (ADR 0012 Decision 1)
inline constexpr std::uint32_t kDeclaredRateHz = 44100;

// OS-USBDISK: what usb0::request_layout accepts while DISK is presented. The DISK set is final until a restart (v0.6.8: the app's mode sync took the default USB AUDIO for a change and
// re-enumerated the unit as AUDIO in the middle of the computer's enumeration): the same layout asked again is accepted (nothing to do), any other request is refused. `current_*` is the
// layout DISK presents now; `mode`, `profile` and `usb_return` are the request.
constexpr bool disk_layout_accepts(Profile current_profile, bool current_return, Mode mode, Profile profile, bool usb_return) noexcept {
  return mode == Mode::Disk && profile == current_profile && usb_return == current_return;
}

constexpr std::uint32_t profile_channels(Profile p) noexcept {
  switch (p) {
    case Profile::P2:
      return 2;
    case Profile::P8:
      return 8;
    case Profile::P10:
      return 10;
    case Profile::P11:
      return 11;
    case Profile::P4S:
      return 10;
    case Profile::P4:
      return 4;
    case Profile::P6:
      return 6;
  }
  return 0;
}

// OS-USBOUTS (ADR 0012 amendment 8): the return the descriptors carry. The WISH is the device setting USB RETURN (and the console's `usb return`); a layout with 10 or more channels
// never presents it (a 10 OUT / 2 IN set does not open behind a hub with a transaction translator, amendment 5). The wish itself is never changed by this: back at 8 channels the return comes back.
constexpr bool return_presented(Profile p, bool wish) noexcept { return wish && profile_channels(p) < 10u; }

// Frames per packet. The cadence is 44/45 frames per 1 ms frame at full speed (44,100 / 1,000 = 44.1,
// or 44.1176 at the true engine rate) and 5/6 per 125 us microframe at high speed.
constexpr std::uint32_t frames_lo(Speed s) noexcept { return s == Speed::Full ? 44u : 5u; }
constexpr std::uint32_t frames_hi(Speed s) noexcept { return frames_lo(s) + 1u; }

// Largest packet of a stream: channels x 2 bytes x frames_hi. Full speed limit is 1,023 bytes per
// isochronous endpoint (USB 2.0 5.6.3), high speed 1,024.
constexpr std::uint32_t max_packet_bytes(std::uint32_t channels, Speed s) noexcept {
  return channels * kBytesPerSample * frames_hi(s);
}
constexpr std::uint32_t kMaxIsoPacketFull = 1023;
constexpr std::uint32_t kMaxIsoPacketHigh = 1024;
static_assert(max_packet_bytes(kMaxChannels, Speed::Full) <= kMaxIsoPacketFull, "11 channels must fit a FS packet");
static_assert(max_packet_bytes(kMaxChannels + 1u, Speed::Full) > kMaxIsoPacketFull, "12 channels must not");

// One engine block, in frames. The stem ring carries whole blocks (ADR 0012 Decision 3: 128-frame blocks,
// pointers only). Changing it changes the fill thresholds in stream.h, which are expressed in frames.
inline constexpr std::uint32_t kStemBlockFrames = 128;
static_assert(kStemBlockFrames % 2u == 0u, "the SIMD kernels work on frame pairs");

// Interface numbers, endpoint addresses and UAC2 entity IDs. Endpoint addresses and entity IDs are the same in
// every profile, speed and mode, so the driver (006b) and the class-request handler can use constants. IN and
// OUT of one endpoint number share the number as in PJRC's USB_EVERYTHING (one hardware endpoint register per
// number on the K66): STANDARD uses endpoint numbers 1..3, AUDIO adds 4..5.
//
// Interface numbers depend on the mode, because of Linux (snd-usb-audio): the driver sets chip->ctrl_intf to the
// FIRST AudioControl interface it probes (the lowest number) and resolves an AudioStreaming interface's
// bTerminalLink only in the descriptors of THAT interface (snd_usb_find_input/output_terminal_descriptor on
// chip->ctrl_intf). With the MIDI function (a UAC1 AudioControl) first, the UAC2 terminals were never found:
// "usb 3-1.4.3: 5:1 : bogus bTerminalLink 3", and no PCM stream appeared. So in AUDIO mode the UAC2 function
// comes BEFORE the MIDI function (each its own IAD; Windows' usbaudio2.sys does not handle MIDI, so MIDI is not
// folded into the UAC2 IAD):
//   STANDARD: 0 CDC comm, 1 CDC data, 2 MIDI AudioControl (UAC1), 3 MIDIStreaming
//   AUDIO:    0 CDC comm, 1 CDC data, 2 UAC2 AudioControl, 3 AS IN (stems), 4 AS OUT (return),
//             5 MIDI AudioControl (UAC1), 6 MIDIStreaming
//   AUDIO with USB RETURN off (OS-USBRET): no AS OUT interface and no feedback endpoint, so the MIDI function moves down:
//             0 CDC comm, 1 CDC data, 2 UAC2 AudioControl, 3 AS IN (stems), 4 MIDI AudioControl, 5 MIDIStreaming
//   DISK:     the STANDARD interfaces 0..3, then 4 Mass Storage (class 08h/06h/50h, Bulk-Only), a single-interface function with no IAD
// The STANDARD descriptor bytes are unchanged by this. The CDC numbers are the same in both modes (the console and
// the OS-006c update path rely on that). `usb_return` is Config::usb_return; it only matters in AUDIO mode, and every function
// below defaults it to ON, the layout before OS-USBRET.
namespace layout {
inline constexpr std::uint8_t kIfCdcComm = 0;
inline constexpr std::uint8_t kIfCdcData = 1;
// AUDIO mode only (the interfaces do not exist in STANDARD).
inline constexpr std::uint8_t kIfAudioControl = 2;
inline constexpr std::uint8_t kIfAudioIn = 3;   // AudioStreaming, device to host (stems)
inline constexpr std::uint8_t kIfAudioOut = 4;  // AudioStreaming, host to device (USB RETURN on only; with it off number 4 is the MIDI AudioControl)
inline constexpr std::uint8_t kIfMsc = 4;       // DISK mode only: the Mass Storage interface, after the STANDARD four
// The MIDI function follows the CDC function in STANDARD and the UAC2 function in AUDIO.
constexpr std::uint8_t if_midi_control(Mode m, bool usb_return = true) noexcept {
  return m == Mode::Audio ? (usb_return ? std::uint8_t{5} : std::uint8_t{4}) : std::uint8_t{2};
}
constexpr std::uint8_t if_midi_stream(Mode m, bool usb_return = true) noexcept {
  return static_cast<std::uint8_t>(if_midi_control(m, usb_return) + 1u);
}
// Number of interfaces of the configuration.
constexpr std::uint8_t interface_count(Mode m, bool usb_return = true) noexcept {
  return m == Mode::Audio ? (usb_return ? std::uint8_t{7} : std::uint8_t{6}) : (m == Mode::Disk ? std::uint8_t{5} : std::uint8_t{4});
}

inline constexpr std::uint8_t kEpCdcNotify = 0x81;
inline constexpr std::uint8_t kEpCdcOut = 0x02;
inline constexpr std::uint8_t kEpCdcIn = 0x82;
inline constexpr std::uint8_t kEpMidiOut = 0x03;
inline constexpr std::uint8_t kEpMidiIn = 0x83;
inline constexpr std::uint8_t kEpAudioOut = 0x04;       // USB RETURN on only
inline constexpr std::uint8_t kEpAudioIn = 0x84;
inline constexpr std::uint8_t kEpAudioFeedback = 0x85;  // USB RETURN on only
// DISK mode: the Mass Storage bulk pair on two endpoint numbers (4 OUT, 5 IN), so the K66's one EPSTALL per number halts each direction on its own
// (spec 3.1, F9). The numbers and addresses coincide with the AUDIO ones; a mode has only one of the two sets.
inline constexpr std::uint8_t kEpMscOut = 0x04;
inline constexpr std::uint8_t kEpMscIn = 0x85;

inline constexpr std::uint8_t kEntClock = 1;         // Clock Source, internal fixed 44,100 Hz
inline constexpr std::uint8_t kEntStemsInput = 2;    // Input Terminal feeding the IN stream
inline constexpr std::uint8_t kEntStreamToHost = 3;  // Output Terminal, USB streaming (bTerminalLink of kIfAudioIn)
inline constexpr std::uint8_t kEntStreamFromHost = 4;  // Input Terminal, USB streaming (bTerminalLink of kIfAudioOut)
inline constexpr std::uint8_t kEntReturnOutput = 5;    // Output Terminal consuming the OUT stream
}  // namespace layout

}  // namespace o3os::usb
