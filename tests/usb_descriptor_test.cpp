// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006a: descriptor builder, validator, strings (ADR 0012 S1).
#include <doctest/doctest.h>

#include <o3os/usb/descriptors.h>
#include <o3os/usb/stream.h>
#include <o3os/usb/validate.h>

#include "usb_golden.h"
#include "usb_test_util.h"

#include <cstdint>
#include <cstring>
#include <iterator>

using namespace o3os::usb;

namespace {

constexpr Profile kProfiles[] = {Profile::P2, Profile::P8, Profile::P10, Profile::P11, Profile::P4S, Profile::P4, Profile::P6};

struct Built {
  std::uint8_t bytes[kMaxConfigDescriptorSize];
  std::size_t size;
};

Built build(const Config& cfg) {
  Built b{};
  b.size = build_config_descriptor(cfg, b.bytes, sizeof b.bytes);
  return b;
}

Config audio(Profile p = Profile::P10, Speed s = Speed::Full, bool usb_return = true) {
  Config c;
  c.mode = Mode::Audio;
  c.profile = p;
  c.speed = s;
  c.usb_return = usb_return;
  return c;
}

ValidateResult check(const Built& b, const Config& cfg) { return validate_config_descriptor(b.bytes, b.size, cfg); }
std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

// Offset of the first descriptor of `type` at or after `from` (0 when none); with `sub` >= 0 a
// class-specific descriptor of that subtype.
std::size_t find_from(const Built& b, std::size_t from, std::uint8_t type, int sub = -1) {
  for (std::size_t pos = from; pos < b.size; pos += b.bytes[pos]) {
    if (b.bytes[pos + 1] == type && (sub < 0 || b.bytes[pos + 2] == sub)) {
      return pos;
    }
  }
  return 0;
}
std::size_t find_iad(const Built& b, int nth) {
  std::size_t pos = 0;
  for (int i = 0; i <= nth; ++i) {
    pos = find_from(b, pos == 0 ? 9 : pos + b.bytes[pos], 0x0B);
  }
  return pos;
}
std::size_t find_interface(const Built& b, std::uint8_t number, std::uint8_t alt) {
  for (std::size_t pos = 9; pos < b.size; pos += b.bytes[pos]) {
    if (b.bytes[pos + 1] == 4 && b.bytes[pos + 2] == number && b.bytes[pos + 3] == alt) {
      return pos;
    }
  }
  return 0;
}
std::size_t find_ep(const Built& b, std::uint8_t addr) {
  for (std::size_t pos = 9; pos < b.size; pos += b.bytes[pos]) {
    if (b.bytes[pos + 1] == 5 && b.bytes[pos + 2] == addr) {
      return pos;
    }
  }
  return 0;
}

unsigned endpoint_numbers(const Built& b) {
  std::uint16_t seen = 0;
  for (std::size_t pos = 9; pos < b.size; pos += b.bytes[pos]) {
    if (b.bytes[pos + 1] == 5) {
      seen = static_cast<std::uint16_t>(seen | (1u << (b.bytes[pos + 2] & 0x0Fu)));
    }
  }
  return static_cast<unsigned>(__builtin_popcount(seen));
}

}  // namespace

TEST_CASE("usb descriptors: golden bytes for every mode x speed x profile") {
  CHECK(std::size(usb_golden::kCases) == 16);  // STANDARD x 2 speeds, AUDIO x 2 speeds x 7 profiles
  for (const usb_golden::Case& g : usb_golden::kCases) {
    Config cfg;
    cfg.mode = g.mode;
    cfg.speed = g.speed;
    cfg.profile = g.profile;
    const Built b = build(cfg);
    INFO("mode " << static_cast<int>(g.mode) << " speed " << static_cast<int>(g.speed) << " profile "
                 << static_cast<int>(g.profile));
    CHECK(b.size == g.size);
    CHECK((b.size == g.size && std::memcmp(b.bytes, g.bytes, g.size) == 0));
    CHECK(b.size <= kMaxConfigDescriptorSize);
    CHECK(static_cast<int>(check(b, cfg).error) == static_cast<int>(ValidateError::Ok));
  }
}

TEST_CASE("usb descriptors: STANDARD ignores the profile; sizes are 175 and 384 bytes") {
  Config a;
  a.profile = Profile::P2;
  Config c;
  c.profile = Profile::P11;
  const Built ba = build(a);
  const Built bc = build(c);
  CHECK(ba.size == 175);
  CHECK(bc.size == 175);
  CHECK(std::memcmp(ba.bytes, bc.bytes, 175) == 0);
  for (Profile p : kProfiles) {
    CHECK(build(audio(p)).size == 384);
  }
  CHECK(kMaxConfigDescriptorSize >= 384);
}

TEST_CASE("usb descriptors: wMaxPacketSize is channels x 2 B x 45 at FS and x 6 at HS") {
  const std::uint16_t fs[] = {180, 720, 900, 990, 900, 360, 540};
  for (std::size_t i = 0; i < std::size(kProfiles); ++i) {
    INFO("profile " << i);
    CHECK(in_stream_max_packet(kProfiles[i], Speed::Full) == fs[i]);
    CHECK(in_stream_max_packet(kProfiles[i], Speed::Full) <= 1023);
    CHECK(in_stream_max_packet(kProfiles[i], Speed::High) == profile_channels(kProfiles[i]) * 2u * 6u);
    const Built b = build(audio(kProfiles[i]));
    CHECK(rd16(b.bytes + find_ep(b, layout::kEpAudioIn) + 4) == fs[i]);
    CHECK(rd16(b.bytes + find_ep(b, layout::kEpAudioOut) + 4) == 180);
    CHECK(rd16(b.bytes + find_ep(b, layout::kEpAudioFeedback) + 4) == 3);
    const Built h = build(audio(kProfiles[i], Speed::High));
    CHECK(rd16(h.bytes + find_ep(h, layout::kEpAudioIn) + 4) == profile_channels(kProfiles[i]) * 12u);
    CHECK(rd16(h.bytes + find_ep(h, layout::kEpAudioOut) + 4) == 24);
    CHECK(rd16(h.bytes + find_ep(h, layout::kEpAudioFeedback) + 4) == 4);
  }
  CHECK(out_stream_max_packet(Speed::Full) == 180);
  CHECK(out_stream_max_packet(Speed::High) == 24);
}

TEST_CASE("usb descriptors: high speed changes packet sizes and intervals only") {
  const Built fs = build(audio(Profile::P10, Speed::Full));
  const Built hs = build(audio(Profile::P10, Speed::High));
  USB_REQUIRE(fs.size == hs.size);
  std::size_t diff = 0;
  for (std::size_t i = 0; i < fs.size; ++i) {
    diff += fs.bytes[i] != hs.bytes[i] ? 1u : 0u;
  }
  CHECK(diff > 0);
  CHECK(diff < 24);
  const std::size_t fb_fs = find_ep(fs, layout::kEpAudioFeedback);
  const std::size_t fb_hs = find_ep(hs, layout::kEpAudioFeedback);
  CHECK(fs.bytes[fb_fs + 3] == 0x11);  // isochronous, no sync, usage feedback
  CHECK(fs.bytes[fb_fs + 6] == 1);     // every frame
  CHECK(hs.bytes[fb_hs + 6] == 4);     // 2^(4-1) microframes = 1 ms
  CHECK(fs.bytes[find_ep(fs, layout::kEpCdcOut) + 4] == 64);
  CHECK(rd16(hs.bytes + find_ep(hs, layout::kEpCdcOut) + 4) == 512);
}

TEST_CASE("usb descriptors: interface, endpoint and entity shape") {
  const Built b = build(audio());
  CHECK(b.bytes[4] == 7);  // CDC 0,1; UAC2 AC 2, AS-IN 3, AS-OUT 4; MIDI 5,6 (UAC2 first: Linux, see usb_types.h)
  const std::size_t as_in0 = find_interface(b, layout::kIfAudioIn, 0);
  const std::size_t as_out0 = find_interface(b, layout::kIfAudioOut, 0);
  USB_REQUIRE(as_in0 != 0 && as_out0 != 0);
  CHECK(b.bytes[as_in0 + 4] == 0);   // alt 0: no endpoints, so no bandwidth
  CHECK(b.bytes[as_out0 + 4] == 0);
  CHECK(b.bytes[find_interface(b, layout::kIfAudioIn, 1) + 4] == 1);
  CHECK(b.bytes[find_interface(b, layout::kIfAudioOut, 1) + 4] == 2);  // data + feedback
  CHECK(endpoint_numbers(b) == 5);                                      // of 15
  CHECK(endpoint_numbers(build(Config{})) == 3);
  // The IAD groups: CDC (0..1), UAC2 (2..4), MIDI (5..6); each function its own IAD.
  const std::size_t i0 = find_iad(b, 0), i1 = find_iad(b, 1), i2 = find_iad(b, 2);
  CHECK((b.bytes[i0 + 2] == 0 && b.bytes[i0 + 3] == 2));
  CHECK((b.bytes[i1 + 2] == 2 && b.bytes[i1 + 3] == 3 && b.bytes[i1 + 6] == 0x20));   // UAC2 (IP_VERSION_02_00)
  CHECK((b.bytes[i2 + 2] == 5 && b.bytes[i2 + 3] == 2 && b.bytes[i2 + 6] == 0x00));   // MIDI (UAC1)
  CHECK(find_iad(build(Config{}), 2) == 0);  // STANDARD has two
  // STANDARD keeps CDC 0,1 and MIDI 2,3.
  const Built st = build(Config{});
  CHECK((st.bytes[find_iad(st, 1) + 2] == 2 && st.bytes[find_iad(st, 1) + 3] == 2));
  CHECK((find_interface(st, 2, 0) != 0 && find_interface(st, 3, 0) != 0 && find_interface(st, 4, 0) == 0));
  // Clock Source: internal fixed, frequency control read-only.
  const std::size_t clk = find_from(b, find_interface(b, layout::kIfAudioControl, 0), 0x24, 0x0A);
  CHECK((b.bytes[clk + 3] == layout::kEntClock && b.bytes[clk + 4] == 0x01 && b.bytes[clk + 5] == 0x01));
}

TEST_CASE("usb descriptors: device descriptor golden bytes and validation") {
  static const std::uint8_t standard[18] = {0x12, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40, 0xD0,
                                            0x16, 0x9F, 0x0E, 0x01, 0x00, 0x01, 0x02, 0x03, 0x01};
  // The same 16D0:0E9F in both modes; the AUDIO descriptor differs in bcdDevice (0x0101, bytes 01 01) only (ADR 0012 amendment 3).
  static const std::uint8_t audio_dev[18] = {0x12, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40, 0xD0,
                                             0x16, 0x9F, 0x0E, 0x01, 0x01, 0x01, 0x02, 0x03, 0x01};
  std::uint8_t d[kDeviceDescriptorSize];
  CHECK(build_device_descriptor(Config{}, d, sizeof d) == 18);
  CHECK(std::memcmp(d, standard, 18) == 0);
  CHECK(validate_device_descriptor(d, 18, Config{}).ok());
  CHECK(build_device_descriptor(audio(), d, sizeof d) == 18);
  CHECK(std::memcmp(d, audio_dev, 18) == 0);
  CHECK(validate_device_descriptor(d, 18, audio()).ok());
  CHECK_FALSE(validate_device_descriptor(d, 18, Config{}).ok());  // the bcdDevice of the other mode
  // Identity (maintainer decision): one VID/PID for both modes, told apart by bcdDevice.
  static_assert(kVendorId == 0x16D0 && kProductId == 0x0E9F, "the tracker's own identity");
  CHECK(kDeviceBcdStandard != kDeviceBcdAudio);
  std::uint8_t ds[kDeviceDescriptorSize];
  CHECK(build_device_descriptor(Config{}, ds, sizeof ds) == 18);
  CHECK(rd16(ds + 8) == rd16(d + 8));    // same VID
  CHECK(rd16(ds + 10) == rd16(d + 10));  // same PID
  CHECK(rd16(ds + 12) == kDeviceBcdStandard);
  CHECK(rd16(d + 12) == kDeviceBcdAudio);
  CHECK(rd16(ds + 12) != rd16(d + 12));  // the two modes differ in bcdDevice
  d[7] = 8;
  CHECK_FALSE(validate_device_descriptor(d, 18, audio()).ok());
  CHECK(validate_device_descriptor(d, 17, audio()).error == ValidateError::Truncated);
  CHECK(build_device_descriptor(audio(), d, 17) == 0);  // too small: refuses
}

TEST_CASE("usb descriptors: builders refuse a buffer that is too small") {
  std::uint8_t small[100];
  CHECK(build_config_descriptor(audio(), small, sizeof small) == 0);
  CHECK(build_config_descriptor(Config{}, small, sizeof small) == 0);
  std::uint8_t exact[384];
  CHECK(build_config_descriptor(audio(), exact, sizeof exact) == 384);
  CHECK(build_config_descriptor(audio(), exact, sizeof exact - 1) == 0);
}

TEST_CASE("usb validator: rejects hand-made broken descriptors") {
  const Config cfg = audio();
  const Built good = build(cfg);
  USB_REQUIRE(check(good, cfg).ok());

  const std::size_t iad_cdc = find_iad(good, 0);
  const std::size_t iad_uac = find_iad(good, 1);
  const std::size_t if_ac = find_interface(good, layout::kIfAudioControl, 0);
  const std::size_t if_midi_ac = find_interface(good, layout::if_midi_control(Mode::Audio), 0);
  const std::size_t if_midi_ms = find_interface(good, layout::if_midi_stream(Mode::Audio), 0);
  const std::size_t as_in0 = find_interface(good, layout::kIfAudioIn, 0);
  const std::size_t as_in1 = find_interface(good, layout::kIfAudioIn, 1);
  const std::size_t as_out1 = find_interface(good, layout::kIfAudioOut, 1);
  const std::size_t ep_cdc_in = find_ep(good, layout::kEpCdcIn);
  const std::size_t ep_audio_in = find_ep(good, layout::kEpAudioIn);
  const std::size_t ep_fb = find_ep(good, layout::kEpAudioFeedback);
  const std::size_t ep_last = find_ep(good, layout::kEpMidiIn) + 9;  // the last descriptor of the set is the MIDI IN class endpoint (5 B)
  const std::size_t ac_header = find_from(good, if_ac, 0x24, 0x01);
  const std::size_t it_stems = find_from(good, if_ac, 0x24, 0x02);
  const std::size_t ms_header = find_from(good, if_midi_ms, 0x24, 0x01);
  const std::size_t as_general_in = find_from(good, as_in1, 0x24, 0x01);
  const std::size_t midi_ac_header = find_from(good, if_midi_ac, 0x24, 0x01);
  USB_REQUIRE((iad_cdc && iad_uac && if_ac && as_in0 && as_in1 && as_out1 && ep_cdc_in && ep_audio_in && ep_fb &&
               ac_header && it_stems && ms_header && as_general_in && midi_ac_header) != 0);
  CHECK(ep_last + 5 == good.size);

  const auto run = [&](const char* what, ValidateError expect, std::size_t off, std::uint8_t value) {
    Built b = good;
    b.bytes[off] = value;
    const ValidateResult r = check(b, cfg);
    INFO(what);
    CHECK(static_cast<int>(r.error) == static_cast<int>(expect));
  };

  run("bNumInterfaces too high", ValidateError::BadNumInterfaces, 4, 8);
  run("bNumInterfaces too low", ValidateError::BadNumInterfaces, 4, 6);
  run("configuration value", ValidateError::BadHeader, 5, 2);
  run("wTotalLength too small", ValidateError::BadTotalLength, 2, static_cast<std::uint8_t>(good.size - 1));
  run("wTotalLength too large", ValidateError::BadTotalLength, 2, static_cast<std::uint8_t>(good.size + 1));
  run("zero bLength", ValidateError::BadLength, iad_cdc, 0);
  run("bLength of 1", ValidateError::BadLength, ep_cdc_in, 1);
  run("endpoint bLength 9 outside MIDI", ValidateError::BadLength, ep_cdc_in, 9);
  run("descriptor runs past the buffer", ValidateError::Truncated, ep_last, 40);
  run("unknown descriptor type", ValidateError::BadType, ep_cdc_in + 1, 0x33);
  run("interface numbering gap", ValidateError::InterfaceNumbering, find_interface(good, layout::kIfAudioOut, 0) + 2, 9);
  run("alternate setting skipped", ValidateError::AltNumbering, as_in1 + 3, 2);
  run("duplicate endpoint address", ValidateError::DuplicateEndpoint, ep_cdc_in + 2, layout::kEpCdcOut);
  run("endpoint number 0", ValidateError::EndpointNumber, ep_cdc_in + 2, 0x80);
  run("endpoint number above 15", ValidateError::EndpointNumber, ep_cdc_in + 2, 0x90);
  run("bNumEndpoints larger than present", ValidateError::EndpointCount, as_in1 + 4, 2);
  run("bNumEndpoints smaller than present", ValidateError::EndpointCount, as_out1 + 4, 1);
  run("AS alt 0 declares bandwidth", ValidateError::Alt0HasBandwidth, as_in0 + 4, 1);
  run("IN wMaxPacketSize one frame short", ValidateError::BadMaxPacket, ep_audio_in + 4, 0x82);  // 898 < 900
  run("IN wMaxPacketSize above 1023", ValidateError::BadMaxPacket, ep_audio_in + 5, 0x04);      // 1156
  run("feedback endpoint size", ValidateError::BadFeedback, ep_fb + 4, 4);
  run("feedback endpoint turned into data", ValidateError::BadMaxPacket, ep_fb + 3, 0x05);
  run("IAD covers too few interfaces", ValidateError::IadMismatch, iad_uac + 3, 2);
  run("IAD first interface wrong", ValidateError::IadMismatch, iad_uac + 2, 5);
  run("IAD class differs from its interface", ValidateError::IadMismatch, iad_cdc + 4, 0x08);
  run("UAC2 AC wTotalLength", ValidateError::BadTotalLength, ac_header + 6, 0x4C);
  run("MIDI MS wTotalLength", ValidateError::BadTotalLength, ms_header + 5, 0x24);
  run("MIDI AC points at the wrong stream interface", ValidateError::BadClassSpecific, midi_ac_header + 8, 5);
  run("clock reference dangling", ValidateError::BadEntity, it_stems + 7, 9);
  run("duplicate entity id", ValidateError::BadEntity, it_stems + 3, 1);
  run("AS channel count differs from terminal", ValidateError::BadEntity, as_general_in + 10, 9);
  run("AS terminal link dangling", ValidateError::BadEntity, as_general_in + 3, 30);
  run("AS format is 24-bit", ValidateError::BadClassSpecific, find_from(good, as_in1, 0x24, 0x02) + 5, 24);
  run("string index out of range", ValidateError::BadString, as_in0 + 8, 99);
  run("channel-name strings run past the end", ValidateError::BadString, it_stems + 13, 30);
}

TEST_CASE("usb validator: structural errors that need an edited layout") {
  const Config cfg = audio();
  const Built b = build(cfg);
  // An endpoint before the first interface.
  {
    Built e = b;
    std::memmove(e.bytes + 9 + 7, e.bytes + 9, e.size - 9 - 7);
    const std::uint8_t ep[7] = {7, 5, 0x81, 3, 16, 0, 16};
    std::memcpy(e.bytes + 9, ep, 7);
    CHECK(check(e, cfg).error == ValidateError::EndpointOutsideInterface);
  }
  // AUDIO bytes against a STANDARD configuration, and the reverse.
  CHECK(check(b, Config{}).error == ValidateError::BadString);  // the audio strings do not exist in STANDARD
  CHECK(check(build(Config{}), audio()).error == ValidateError::ModeCounts);
  // Profile and speed expectations.
  CHECK_FALSE(check(b, audio(Profile::P8)).ok());
  CHECK(check(b, audio(Profile::P4S)).ok());  // P4S: the same 10 channels and packet sizes as P10
  CHECK_FALSE(check(b, audio(Profile::P10, Speed::High)).ok());
  // A short buffer.
  CHECK(validate_config_descriptor(b.bytes, 5, cfg).error == ValidateError::Truncated);
  // Trailing bytes that wTotalLength covers but no descriptor owns.
  Built g = b;
  g.bytes[g.size++] = 0;
  g.bytes[g.size++] = 0;
  g.bytes[2] = static_cast<std::uint8_t>(g.size);
  CHECK_FALSE(check(g, cfg).ok());
}

TEST_CASE("usb strings: language, identity, serial and channel names") {
  std::uint8_t d[kMaxStringDescriptorSize];
  const Config cfg = audio();
  CHECK(build_string_descriptor(cfg, 0, d, sizeof d) == 4);
  CHECK(d[0] == 4);
  CHECK(d[1] == 3);
  CHECK(rd16(d + 2) == 0x0409);

  char text[40];
  const auto read = [&](const Config& c, std::uint8_t idx) -> const char* {
    const std::size_t n = build_string_descriptor(c, idx, d, sizeof d);
    std::size_t len = 0;
    CHECK(n >= 2);
    if (n >= 2) {
      CHECK(d[0] == n);
      CHECK(d[1] == 3);
      for (std::size_t i = 2; i + 1 < n && len < sizeof text - 1; i += 2) {
        CHECK(d[i + 1] == 0);
        text[len++] = static_cast<char>(d[i]);
      }
    }
    text[len] = '\0';
    return text;
  };
  CHECK(std::strcmp(read(cfg, kStrManufacturer), kManufacturerString) == 0);
  CHECK(std::strcmp(kManufacturerString, "track3r-os") == 0);  // the project's own name (the VID:PID is the official firmware's pair, kept by the maintainer's decision: identity.h)
  // The device name is "Track3r" in both modes; the interface strings carry it as their prefix (maintainer decision).
  CHECK(std::strcmp(read(cfg, kStrProduct), "Track3r") == 0);
  CHECK(std::strcmp(read(Config{}, kStrProduct), "Track3r") == 0);
  CHECK(std::strcmp(read(Config{}, kStrCdc), "Track3r diag") == 0);
  CHECK(std::strcmp(read(Config{}, kStrMidi), "Track3r MIDI") == 0);
  CHECK(std::strcmp(read(cfg, kStrAudio), "Track3r Audio") == 0);
  CHECK(std::strcmp(read(cfg, kStrSerial), kSerialPlaceholder) == 0);
  Config withserial = cfg;
  withserial.serial = "A1B2C3D4E5F60718";
  CHECK(std::strcmp(read(withserial, kStrSerial), "A1B2C3D4E5F60718") == 0);
  withserial.serial = "";
  CHECK(std::strcmp(read(withserial, kStrSerial), kSerialPlaceholder) == 0);
  withserial.serial = nullptr;
  CHECK(std::strcmp(read(withserial, kStrSerial), kSerialPlaceholder) == 0);
  withserial.serial = "0123456789012345678901234567890123456789";  // longer than kMaxSerialChars: cut
  CHECK(std::strlen(read(withserial, kStrSerial)) == kMaxSerialChars);

  // P10: S1..S8, Master L/R, then Return L/R, at consecutive indices from kStrChannelNames.
  CHECK(string_count(cfg) == 8u + 10u + 2u);
  const char* const p10[] = {"S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8", "Master L", "Master R", "Return L", "Return R"};
  for (std::uint8_t i = 0; i < 12; ++i) {
    CHECK(std::strcmp(read(cfg, static_cast<std::uint8_t>(kStrChannelNames + i)), p10[i]) == 0);
  }
  CHECK(build_string_descriptor(cfg, static_cast<std::uint8_t>(string_count(cfg)), d, sizeof d) == 0);
  CHECK(build_string_descriptor(cfg, 200, d, sizeof d) == 0);
  CHECK(build_string_descriptor(Config{}, kStrAudio, d, sizeof d) == 0);  // no audio strings in STANDARD
  CHECK(string_count(Config{}) == 6u);

  char name[10];
  CHECK(channel_name(Profile::P2, 0, name) == 8);
  CHECK(std::strcmp(name, "Master L") == 0);
  CHECK(channel_name(Profile::P2, 1, name) == 8);
  CHECK(std::strcmp(name, "Master R") == 0);
  CHECK(channel_name(Profile::P2, 2, name) == 0);
  CHECK(channel_name(Profile::P11, 8, name) == 2);
  CHECK(std::strcmp(name, "S9") == 0);
  CHECK(channel_name(Profile::P8, 5, name) == 2);
  CHECK(std::strcmp(name, "S6") == 0);
  CHECK(channel_name(Profile::P4S, 3, name) == 4);
  CHECK(std::strcmp(name, "S2 R") == 0);
  CHECK(channel_name(Profile::P4S, 8, name) == 8);
  CHECK(std::strcmp(name, "Master L") == 0);
  // OS-USBOUTS: P4 = S1, S2, Master L/R; P6 = S1..S4, Master L/R
  CHECK(channel_name(Profile::P4, 1, name) == 2);
  CHECK(std::strcmp(name, "S2") == 0);
  CHECK(channel_name(Profile::P4, 2, name) == 8);
  CHECK(std::strcmp(name, "Master L") == 0);
  CHECK(channel_name(Profile::P4, 3, name) == 8);
  CHECK(std::strcmp(name, "Master R") == 0);
  CHECK(channel_name(Profile::P4, 4, name) == 0);
  CHECK(channel_name(Profile::P6, 3, name) == 2);
  CHECK(std::strcmp(name, "S4") == 0);
  CHECK(channel_name(Profile::P6, 4, name) == 8);
  CHECK(channel_name(Profile::P6, 6, name) == 0);
  CHECK(build_string_descriptor(cfg, kStrProduct, d, 10) == 0);  // buffer too small: refuses
}

TEST_CASE("usb strings: every index in range builds a descriptor") {
  for (const usb_golden::Case& g : usb_golden::kCases) {
    Config cfg;
    cfg.mode = g.mode;
    cfg.speed = g.speed;
    cfg.profile = g.profile;
    std::uint8_t d[kMaxStringDescriptorSize];
    for (std::uint32_t i = 0; i < string_count(cfg); ++i) {
      CHECK(build_string_descriptor(cfg, static_cast<std::uint8_t>(i), d, sizeof d) >= 4);
    }
  }
}

// The Linux rule (sound/usb/stream.c): snd-usb-audio takes the FIRST AudioControl interface of the configuration as
// chip->ctrl_intf, and looks the bTerminalLink of every AudioStreaming interface up in that interface's AC descriptors
// only (snd_usb_find_input/output_terminal_descriptor). Hardware, image 0.4.3: "bogus bTerminalLink 3" with the MIDI
// (UAC1) AudioControl first, and no PCM streams. This mimics the lookup on the built bytes.
TEST_CASE("usb descriptors: AUDIO mode - the first AudioControl interface is UAC2 and holds every terminal link") {
  for (const bool ret : {true, false}) {
  for (Speed sp : {Speed::Full, Speed::High}) {
    for (Profile pr : kProfiles) {
      const Built b = build(audio(pr, sp, ret));
      USB_REQUIRE(b.size != 0);
      int ctrl_intf = -1;               // number of the first AudioControl interface
      std::uint16_t bcd_adc = 0;        // its bcdADC
      std::uint8_t terminal[256] = {};  // subtype (2 input, 3 output) of the terminals it declares, by id
      int as_in_link = -1, as_out_link = -1;
      int midi_ac_intf = -1;
      std::uint8_t cur_number = 0, cur_class = 0, cur_sub = 0, cur_proto = 0, cur_alt = 0;
      for (std::size_t pos = 9; pos < b.size; pos += b.bytes[pos]) {
        const std::uint8_t* d = b.bytes + pos;
        if (d[1] == 4) {
          cur_number = d[2];
          cur_alt = d[3];
          cur_class = d[5];
          cur_sub = d[6];
          cur_proto = d[7];
          if (cur_class == 1 && cur_sub == 1 && cur_alt == 0 && ctrl_intf < 0) {
            ctrl_intf = cur_number;
          }
          if (cur_class == 1 && cur_sub == 1 && cur_proto == 0 && cur_alt == 0) {
            midi_ac_intf = cur_number;
          }
        } else if (d[1] == 0x24 && cur_class == 1 && cur_sub == 1 && cur_number == ctrl_intf) {
          if (d[2] == 0x01) {
            bcd_adc = rd16(d + 3);
          } else if (d[2] == 0x02 || d[2] == 0x03) {
            terminal[d[3]] = d[2];
          }
        } else if (d[1] == 0x24 && cur_class == 1 && cur_sub == 2 && cur_proto == 0x20 && cur_alt == 1 && d[2] == 0x01) {
          (cur_number == layout::kIfAudioIn ? as_in_link : as_out_link) = d[3];
        }
      }
      INFO("profile " << static_cast<int>(pr) << " speed " << static_cast<int>(sp) << " return " << ret);
      CHECK(ctrl_intf == layout::kIfAudioControl);
      CHECK(bcd_adc == 0x0200);  // the first AudioControl interface is the UAC2 one, not the MIDI (bcdADC 1.00) one
      CHECK(midi_ac_intf == layout::if_midi_control(Mode::Audio, ret));
      CHECK(midi_ac_intf > ctrl_intf);
      // snd_usb_find_output_terminal_descriptor for the IN stream, snd_usb_find_input_terminal_descriptor for the OUT stream (which USB RETURN off does not have).
      CHECK((as_in_link > 0 && terminal[as_in_link] == 0x03));
      if (ret) {
        CHECK((as_out_link > 0 && terminal[as_out_link] == 0x02));
      } else {
        CHECK(as_out_link == -1);
      }
    }
  }
  }
}

// The layout that failed on the unit (MIDI function first) must be rejected by the validator.
TEST_CASE("usb validator: AUDIO mode with the MIDI function ahead of UAC2 is rejected") {
  const Config cfg = audio();
  const Built good = build(cfg);
  USB_REQUIRE(check(good, cfg).ok());
  const std::size_t iad_uac = find_iad(good, 1);
  const std::size_t iad_midi = find_iad(good, 2);
  USB_REQUIRE(iad_uac != 0 && iad_midi > iad_uac);
  // Old order: header + CDC, MIDI (2,3), UAC2 (4..6).
  Built old{};
  std::size_t n = iad_uac;
  std::memcpy(old.bytes, good.bytes, n);
  std::memcpy(old.bytes + n, good.bytes + iad_midi, good.size - iad_midi);
  n += good.size - iad_midi;
  std::memcpy(old.bytes + n, good.bytes + iad_uac, iad_midi - iad_uac);
  n += iad_midi - iad_uac;
  old.size = n;
  // Renumber: walk each descriptor, IADs and interfaces and the MIDI AC header's baInterfaceNr.
  const std::size_t uac_at = iad_uac + (good.size - iad_midi);
  for (std::size_t pos = iad_uac; pos < old.size; pos += old.bytes[pos]) {
    std::uint8_t* d = old.bytes + pos;
    const bool is_uac = pos >= uac_at;
    if (d[1] == 0x0B || d[1] == 4) {
      d[2] = static_cast<std::uint8_t>(is_uac ? d[2] + 2u : d[2] - 3u);  // UAC2 2..4 -> 4..6, MIDI 5..6 -> 2..3
    } else if (d[1] == 0x24 && !is_uac && d[0] == 9 && d[2] == 0x01 && d[3] == 0 && d[4] == 1) {
      d[8] = 3;
    }
  }
  CHECK(check(old, cfg).error == ValidateError::ModeCounts);
}

// ---- USB RETURN off (OS-USBRET, ADR 0012 amendment 5) --------------------------------------------------------------------------------

namespace {

// The data (isochronous) endpoints of a built set, by address.
struct IsoEp {
  std::uint8_t address;
  std::uint16_t max_packet;
};
unsigned iso_endpoints(const Built& b, IsoEp* out, unsigned cap) {
  unsigned n = 0;
  for (std::size_t pos = 9; pos < b.size; pos += b.bytes[pos]) {
    if (b.bytes[pos + 1] == 5 && (b.bytes[pos + 3] & 3u) == 1u && n < cap) {
      out[n++] = IsoEp{b.bytes[pos + 2], rd16(b.bytes + pos + 4)};
    }
  }
  return n;
}
unsigned count_endpoints(const Built& b) {
  unsigned n = 0;
  for (std::size_t pos = 9; pos < b.size; pos += b.bytes[pos]) {
    n += b.bytes[pos + 1] == 5 ? 1u : 0u;
  }
  return n;
}
unsigned count_interfaces(const Built& b) {  // alt 0 only
  unsigned n = 0;
  for (std::size_t pos = 9; pos < b.size; pos += b.bytes[pos]) {
    n += (b.bytes[pos + 1] == 4 && b.bytes[pos + 3] == 0) ? 1u : 0u;
  }
  return n;
}

}  // namespace

TEST_CASE("usb descriptors: USB RETURN off has no OUT interface, no return terminals and no feedback endpoint, for every profile and speed") {
  for (const Speed sp : {Speed::Full, Speed::High}) {
    for (const Profile pr : kProfiles) {
      const Config cfg = audio(pr, sp, false);
      const Built b = build(cfg);
      INFO("profile " << static_cast<int>(pr) << " speed " << static_cast<int>(sp));
      USB_REQUIRE(b.size != 0);
      CHECK(static_cast<int>(check(b, cfg).error) == static_cast<int>(ValidateError::Ok));
      CHECK(b.size == 384u - 62u - 29u);  // the 62-byte OUT AudioStreaming interface (two alternates, format, data and feedback endpoints) and the 29 bytes of return terminals
      CHECK(b.bytes[4] == 6);             // CDC 0,1; UAC2 AC 2, AS IN 3; MIDI 4,5
      CHECK(count_interfaces(b) == 6u);
      CHECK(rd16(b.bytes + 2) == b.size);
      // Three functions, each its own IAD: CDC (0..1), UAC2 (2..3), MIDI (4..5).
      const std::size_t i0 = find_iad(b, 0), i1 = find_iad(b, 1), i2 = find_iad(b, 2);
      CHECK((b.bytes[i0 + 2] == 0 && b.bytes[i0 + 3] == 2));
      CHECK((b.bytes[i1 + 2] == 2 && b.bytes[i1 + 3] == 2 && b.bytes[i1 + 6] == 0x20));
      CHECK((b.bytes[i2 + 2] == 4 && b.bytes[i2 + 3] == 2 && b.bytes[i2 + 6] == 0x00));
      CHECK(find_iad(b, 3) == 0);
      // Interface 4 is the MIDI AudioControl (class 1, subclass 1, protocol 0) and 5 the MIDIStreaming one; the AS interface is 3 alone.
      const std::size_t if4 = find_interface(b, 4, 0), if5 = find_interface(b, 5, 0);
      USB_REQUIRE((if4 != 0 && if5 != 0));
      CHECK((b.bytes[if4 + 5] == 1 && b.bytes[if4 + 6] == 1 && b.bytes[if4 + 7] == 0));
      CHECK((b.bytes[if5 + 5] == 1 && b.bytes[if5 + 6] == 3));
      CHECK(find_interface(b, 6, 0) == 0);
      CHECK(find_interface(b, 4, 1) == 0);  // (nothing has a second alternate there)
      CHECK(b.bytes[find_interface(b, layout::kIfAudioIn, 1) + 4] == 1);  // the stems stream: the data endpoint alone, no feedback
      // Endpoints: CDC 3, the stems IN, MIDI 2. No 0x04 OUT, no 0x85 feedback; numbers 1,2,3,4.
      CHECK(count_endpoints(b) == 6u);
      CHECK(endpoint_numbers(b) == 4);
      CHECK(find_ep(b, layout::kEpAudioIn) != 0);
      CHECK(find_ep(b, layout::kEpAudioOut) == 0);
      CHECK(find_ep(b, layout::kEpAudioFeedback) == 0);
      IsoEp iso[4];
      USB_REQUIRE(iso_endpoints(b, iso, 4) == 1u);
      CHECK(iso[0].address == layout::kEpAudioIn);
      CHECK(iso[0].max_packet == profile_channels(pr) * 2u * (sp == Speed::Full ? 45u : 6u));
      // The entity graph: the clock, the stems input terminal and the stream-to-host output terminal; the UAC2 AC header says so.
      const std::size_t ac = find_from(b, find_interface(b, layout::kIfAudioControl, 0), 0x24, 0x01);
      CHECK(rd16(b.bytes + ac + 6) == 9u + 8u + 17u + 12u);
      CHECK(find_from(b, ac, 0x24, 0x02) != 0);
      std::size_t terminals = 0;
      for (std::size_t pos = ac; pos < find_interface(b, layout::kIfAudioIn, 0); pos += b.bytes[pos]) {
        terminals += (b.bytes[pos + 1] == 0x24 && (b.bytes[pos + 2] == 0x02 || b.bytes[pos + 2] == 0x03)) ? 1u : 0u;
      }
      CHECK(terminals == 2u);
      // Strings: the return's two names are not there.
      CHECK(string_count(cfg) == 8u + profile_channels(pr));
      std::uint8_t d[kMaxStringDescriptorSize];
      CHECK(build_string_descriptor(cfg, static_cast<std::uint8_t>(8u + profile_channels(pr)), d, sizeof d) == 0);
      CHECK(string_count(audio(pr, sp, true)) == 8u + profile_channels(pr) + 2u);
      // The bytes validate for their own Config only: the other value of USB RETURN is a different set of descriptors.
      CHECK_FALSE(check(b, audio(pr, sp, true)).ok());
      CHECK_FALSE(check(build(audio(pr, sp, true)), cfg).ok());
    }
  }
}

TEST_CASE("usb descriptors: USB RETURN does not change STANDARD, and with it on the AUDIO bytes are the golden ones") {
  Config st;
  st.usb_return = false;
  CHECK(build(st).size == 175);
  CHECK(std::memcmp(build(st).bytes, build(Config{}).bytes, 175) == 0);
  CHECK(validate_config_descriptor(build(st).bytes, 175, st).ok());
  CHECK(string_count(st) == 6u);
  CHECK(layout::interface_count(Mode::Standard, false) == 4);
  CHECK(layout::if_midi_control(Mode::Standard, false) == 2);
  CHECK(layout::interface_count(Mode::Audio, true) == 7);
  CHECK(layout::interface_count(Mode::Audio, false) == 6);
  CHECK(layout::if_midi_control(Mode::Audio, true) == 5);
  CHECK(layout::if_midi_control(Mode::Audio, false) == 4);
  CHECK(layout::if_midi_stream(Mode::Audio, false) == 5);
  // The Config default is the layout before OS-USBRET: every golden set was built without naming the field.
  CHECK(Config{}.usb_return);
  for (const usb_golden::Case& g : usb_golden::kCases) {
    Config cfg;
    cfg.mode = g.mode;
    cfg.speed = g.speed;
    cfg.profile = g.profile;
    cfg.usb_return = true;
    const Built b = build(cfg);
    CHECK((b.size == g.size && std::memcmp(b.bytes, g.bytes, g.size) == 0));
  }
}

// An oracle that is not the builder: cut the return stream out of the reviewed golden bytes by hand (the OUT AudioStreaming interface, the two return terminals,
// the IAD count, the MIDI function's interface numbers) and compare with what the builder writes for USB RETURN off.
TEST_CASE("usb descriptors: USB RETURN off equals the golden AUDIO set with the return stream cut out by hand") {
  for (const usb_golden::Case& g : usb_golden::kCases) {
    if (g.mode != Mode::Audio) {
      continue;
    }
    Built gold{};
    gold.size = g.size;
    std::memcpy(gold.bytes, g.bytes, g.size);
    const std::size_t ac = find_from(gold, find_interface(gold, layout::kIfAudioControl, 0), 0x24, 0x01);
    std::size_t term_in = 0;
    std::size_t term_out = 0;
    for (std::size_t pos = ac; pos < find_interface(gold, layout::kIfAudioIn, 0); pos += gold.bytes[pos]) {
      if (gold.bytes[pos + 1] == 0x24 && gold.bytes[pos + 2] == 0x02 && gold.bytes[pos + 3] == layout::kEntStreamFromHost) {
        term_in = pos;
      }
      if (gold.bytes[pos + 1] == 0x24 && gold.bytes[pos + 2] == 0x03 && gold.bytes[pos + 3] == layout::kEntReturnOutput) {
        term_out = pos;
      }
    }
    const std::size_t as_out = find_interface(gold, layout::kIfAudioOut, 0);
    const std::size_t midi_iad = find_iad(gold, 2);
    USB_REQUIRE((term_in != 0 && term_out == term_in + 17 && as_out != 0 && midi_iad > as_out && term_out + 12 < as_out));
    // (the descriptors run: AC with the return's two terminals last, the stems interface, the return interface, then the MIDI function)
    // Assemble: [0, term_in) + [term_out + 12, as_out) + [midi_iad, end).
    Built cut{};
    std::size_t n = 0;
    const auto copy = [&](std::size_t from, std::size_t to) {
      std::memcpy(cut.bytes + n, gold.bytes + from, to - from);
      n += to - from;
    };
    copy(0, term_in);
    copy(term_out + 12, as_out);
    copy(midi_iad, gold.size);
    cut.size = n;
    CHECK(gold.size - cut.size == 29u + 62u);
    // Patch what the cut changes: wTotalLength, bNumInterfaces, the UAC2 IAD's count, the AC header's length, the MIDI function's numbers.
    cut.bytes[2] = static_cast<std::uint8_t>(cut.size & 0xFFu);
    cut.bytes[3] = static_cast<std::uint8_t>(cut.size >> 8);
    cut.bytes[4] = 6;
    cut.bytes[find_iad(cut, 1) + 3] = 2;
    const std::size_t cut_ac = find_from(cut, find_interface(cut, layout::kIfAudioControl, 0), 0x24, 0x01);
    cut.bytes[cut_ac + 6] = static_cast<std::uint8_t>(rd16(cut.bytes + cut_ac + 6) - 29u);
    std::uint8_t midi_number = 5;
    for (std::size_t pos = find_iad(cut, 2); pos < cut.size; pos += cut.bytes[pos]) {
      std::uint8_t* d = cut.bytes + pos;
      if (d[1] == 0x0B) {
        d[2] = 4;
      } else if (d[1] == 4) {
        d[2] = static_cast<std::uint8_t>(d[2] - 1u);
        midi_number = d[2];
      } else if (d[1] == 0x24 && d[0] == 9 && d[2] == 0x01 && d[3] == 0 && d[4] == 1 && midi_number == 4) {
        d[8] = 5;  // the MIDI AudioControl header names the MIDIStreaming interface
      }
    }
    Config cfg;
    cfg.mode = Mode::Audio;
    cfg.speed = g.speed;
    cfg.profile = g.profile;
    cfg.usb_return = false;
    const Built built = build(cfg);
    INFO("speed " << static_cast<int>(g.speed) << " profile " << static_cast<int>(g.profile));
    CHECK(built.size == cut.size);
    CHECK((built.size == cut.size && std::memcmp(built.bytes, cut.bytes, cut.size) == 0));
  }
}

// ADR 0012 amendment 5, the bandwidth table. The isochronous bytes of one full-speed frame come from the wMaxPacketSize values the builder writes (the worst case:
// 45 frames a packet at 44.1 kHz async), plus the 9 bytes per transaction that ADR 0012 Decision 1 counts for token, handshake-less data framing and inter-packet gaps.
TEST_CASE("usb descriptors: bandwidth per profile with and without USB RETURN, every endpoint fits its packet limit") {
  struct Row {
    Profile profile;
    unsigned in_only;       // payload bytes per frame, USB RETURN off
    unsigned with_return;   // + the 180-byte OUT packet and the 3-byte feedback
  };
  const Row rows[] = {{Profile::P2, 180, 363}, {Profile::P8, 720, 903}, {Profile::P10, 900, 1083}, {Profile::P11, 990, 1173}, {Profile::P4S, 900, 1083},
                    {Profile::P4, 360, 543}, {Profile::P6, 540, 723}};
  constexpr unsigned kOverhead = 9;
  constexpr unsigned kFrameBytes = 1500;                 // 12 Mbit/s for 1 ms
  constexpr unsigned kPeriodicBytes = kFrameBytes * 9 / 10;  // USB 2.0 5.6.4: at most 90 % of a frame for isochronous and interrupt transfers
  for (const Row& row : rows) {
    for (const bool ret : {false, true}) {
      const Built b = build(audio(row.profile, Speed::Full, ret));
      IsoEp iso[4];
      const unsigned n = iso_endpoints(b, iso, 4);
      CHECK(n == (ret ? 3u : 1u));  // stems IN; with USB RETURN also the OUT data endpoint and the feedback IN
      unsigned payload = 0;
      for (unsigned i = 0; i < n; ++i) {
        payload += iso[i].max_packet;  // (the feedback endpoint is isochronous too: its 3 bytes are in the sum)
        CHECK(iso[i].max_packet <= kMaxIsoPacketFull);
        if (iso[i].address != layout::kEpAudioFeedback) {
          // A data endpoint holds the 45-frame worst case of 44.1 kHz asynchronous audio, and nothing more than it.
          const unsigned channels = (iso[i].address & 0x80u) != 0 ? profile_channels(row.profile) : kReturnChannels;
          CHECK(iso[i].max_packet == channels * kBytesPerSample * 45u);
        }
      }
      const unsigned bus = payload + kOverhead * n;
      INFO("profile " << static_cast<int>(row.profile) << " return " << ret << ": payload " << payload << ", bus " << bus);
      CHECK(payload == (ret ? row.with_return : row.in_only));
      CHECK(bus <= kPeriodicBytes);  // every combination fits the bus-level limit; the transaction translator of a hub is what ran out (ADR amendment 5)
    }
  }
  // The figures the ADR's table quotes.
  CHECK(900u + 180u + 3u + 3u * kOverhead == 1110u);
  CHECK(kPeriodicBytes == 1350u);
  // The packet buffers of the driver hold the worst-case packets.
  static_assert(kReturnMaxChunk * 4u >= max_packet_bytes(kReturnChannels, Speed::Full), "the OUT buffer holds a 45-frame return packet");
  static_assert(sizeof(InPacketBuffer) >= max_packet_bytes(kMaxChannels, Speed::Full), "the IN buffer holds a 45-frame packet of the widest profile");
}
