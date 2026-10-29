// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/platform/mk66/clock_config.h>
#include <o3os/platform/mk66/platform.h>
#include <o3os/platform/mk66/usb/diag_text.h>

namespace o3os::platform::mk66::console {
namespace {

using audio::diag::Text;

Text& hex_n(Text& t, std::uint32_t v, unsigned digits) {
  t.str("0x");
  for (unsigned i = 0; i < digits; ++i) {
    t.chr("0123456789ABCDEF"[(v >> (4u * (digits - 1u - i))) & 0xFu]);
  }
  return t;
}

Text& hex_digits(Text& t, std::uint32_t v, unsigned digits) {
  for (unsigned i = 0; i < digits; ++i) {
    t.chr("0123456789ABCDEF"[(v >> (4u * (digits - 1u - i))) & 0xFu]);
  }
  return t;
}

Text& yes_no(Text& t, bool v) { return t.str(v ? "yes" : "no"); }

// "N.N us" for a cycle count at the core clock: one decimal, rounded down. 32-bit arithmetic only (a tenth of a microsecond is 18 cycles at
// 180 MHz): a 64-bit division would pull a library helper into the image.
Text& micros(Text& t, std::uint32_t cycles) {
  static_assert(clock::kCoreHz % 10'000'000u == 0u, "a whole number of cycles per tenth of a microsecond");
  const std::uint32_t tenth_us = cycles / (clock::kCoreHz / 10'000'000u);
  return t.dec(tenth_us / 10u).chr('.').dec(tenth_us % 10u).str(" us");
}

}  // namespace

const char* device_state_name(o3os::usb::DeviceState s) noexcept {
  switch (s) {
    case o3os::usb::DeviceState::Default:
      return "Default";
    case o3os::usb::DeviceState::Address:
      return "Address";
    case o3os::usb::DeviceState::Configured:
      return "Configured";
  }
  return "?";
}

const char* stage_name(crash::Stage s) noexcept {
  switch (s) {
    case crash::Stage::Reset:
      return "Reset";
    case crash::Stage::Clocks:
      return "Clocks";
    case crash::Stage::Memory:
      return "Memory";
    case crash::Stage::Interrupts:
      return "Interrupts";
    case crash::Stage::Sdram:
      return "Sdram";
    case crash::Stage::Running:
      return "Running";
    case crash::Stage::Unknown:
      return "Unknown";
  }
  return "?";
}

const char* exception_name(std::uint32_t exception, char* buf) noexcept {
  switch (exception) {
    case 2:
      return "NMI";
    case 3:
      return "HardFault";
    case 4:
      return "MemManage";
    case 5:
      return "BusFault";
    case 6:
      return "UsageFault";
    case crash::kPanicException:
      return "panic";
    default:
      break;
  }
  Text t(buf, 24);
  if (exception >= 16u) {
    t.str("IRQ ").dec(exception - 16u).str(" (no handler)");
  } else {
    t.str("exception ").dec(exception);
  }
  return buf;
}

namespace {

// One line of counters, table driven: "name: key value, key value".
struct Kv {
  const char* key;
  std::uint32_t value;
};
template <std::size_t N>
Text& group(Text& t, const char* name, const Kv (&kv)[N]) {
  t.str(name).str(": ");
  for (std::size_t i = 0; i < N; ++i) {
    t.str(i == 0 ? "" : ", ").str(kv[i].key).chr(' ').dec(kv[i].value);
  }
  return t;
}

}  // namespace

// `usb ep0` (OS-USBDISK U2a): "#03 +123us setup  type 80 req 06 value 0200 index 0000 len 0009 even", "#04 +9us arm-in buf 0x2000ABCD n 9 DATA1 odd", ...
std::size_t format_usb_ep0_entry(unsigned index, const o3os::usb::Ep0Trace::Entry& e, char* out, std::size_t cap) noexcept {
  using Tag = o3os::usb::Ep0Trace::Tag;
  static_assert(clock::kCoreHz == 180'000'000u, "dt64 is 64 cycles: 16/45 us at 180 MHz");
  Text t(out, cap);
  t.chr('#').dec(index < 10u ? 0u : index / 10u).dec(index % 10u).str(" +").dec((static_cast<std::uint32_t>(e.dt64) * 16u) / 45u).str("us ");
  const bool odd = (e.flags & 1u) != 0u;
  const bool tx = (e.flags & 2u) != 0u;
  const unsigned pid = e.flags >> 4;
  switch (static_cast<Tag>(e.tag)) {
    case Tag::Reset:
      t.str("reset");
      break;
    case Tag::Setup:
      t.str("setup type ");
      hex_digits(t, e.w0 & 0xFFu, 2).str(" req ");
      hex_digits(t, (e.w0 >> 8) & 0xFFu, 2).str(" value ");
      hex_digits(t, e.w0 >> 16, 4).str(" index ");
      hex_digits(t, e.extra, 4).str(" len ");
      hex_digits(t, e.bytes, 4).str(odd ? " odd" : " even");
      break;
    case Tag::InDone:
    case Tag::OutDone:
    case Tag::ArmIn:
      t.str(static_cast<Tag>(e.tag) == Tag::InDone ? "in-done  " : (static_cast<Tag>(e.tag) == Tag::OutDone ? "out-done " : "arm-in   ")).str(tx ? "tx " : "rx ").str("buf ");
      hex_n(t, e.w0, 8).str(" n ").dec(e.bytes).str((e.flags & 4u) != 0u ? " DATA1" : " DATA0").str(odd ? " odd" : " even");
      if (pid != 0u) {
        t.str(" pid ");
        hex_digits(t, pid, 1);
      }
      break;
    case Tag::StallSet:
      t.str("stall set");
      break;
    case Tag::StallSeen:
      t.str("stall sent");
      break;
    case Tag::Address:
      t.str("address ").dec(e.w0);
      break;
    case Tag::Error:
      t.str("error ");
      hex_n(t, e.w0, 2);
      break;
    case Tag::Sleep:
      t.str("idle (sleep)");
      break;
    case Tag::StallCleared:
      t.str("stall-cleared");
      break;
    case Tag::SetupArmed:
      t.str("setup-armed even ").str((e.w0 & 1u) != 0u ? "owned" : "FREE").str(" odd ").str((e.w0 & 2u) != 0u ? "owned" : "FREE").str((e.extra & 1u) != 0u ? " freeze released" : " freeze kept").str((e.extra & 2u) != 0u ? " TXSUSPENDTOKENBUSY set" : " TXSUSPENDTOKENBUSY clear");
      break;
    default:
      t.str("?");
      break;
  }
  t.nl();
  return t.size();
}

// The last line of `usb ep0`: the STALL handshakes after the first of each endpoint 0 stall (the host retried a stalled token and was stalled again, not NAKed). Greater than 0 on an attach: the retry reading holds.
std::size_t format_usb_ep0_retries(std::uint32_t retries, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("stall retries ").dec(retries).nl();
  return t.size();
}

std::size_t format_usb(const UsbReport& r, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  const o3os::usb::DeviceCounters& d = *r.device;
  const o3os::usb::ControlCounters& c = *r.control;
  t.str("usb: ").str(device_state_name(r.state)).str(", address ").dec(r.address).str(", suspended ");
  yes_no(t, r.suspended).str(", pull-up ");
  yes_no(t, r.bootstrap_ok != 0u).str(", clock ").dec(r.usb_clock_hz / 1'000'000u).str(" MHz IRC48M\n");
  t.str("terminal: DTR ");
  yes_no(t, r.dtr).str(", RTS ");
  yes_no(t, r.rts).str(", line coding ").dec(r.coding.dte_rate).chr(' ').dec(r.coding.data_bits);
  t.chr(r.coding.parity_type == 0 ? 'N' : (r.coding.parity_type == 1 ? 'O' : (r.coding.parity_type == 2 ? 'E' : '?')));
  t.str(r.coding.char_format == 0 ? "1" : (r.coding.char_format == 1 ? "1.5" : "2")).str(" (stored, ignored)\n");
  group(t, "bus", {{"resets", d.bus_resets}, {"suspends", c.suspends}, {"resumes", c.resumes}, {"SOF", r.audio->sof}}).nl();
  group(t, "isr", {{"entries", d.isr_count}, {"last-cycles", d.isr_cycles_last}, {"worst-cycles", d.isr_cycles_max}, {"loop-limit", d.isr_loop_limit}});
  micros(t.str(" (worst "), d.isr_cycles_max).str(")\n");
  group(t, "ep0", {{"setups", c.setups}, {"tokens", d.tokens_ep0}, {"stalls", c.stalls}, {"configs", c.configurations}, {"out-errors", c.out_stage_errors}}).nl();
  if (c.stalls != 0u) {
    const o3os::usb::SetupPacket& s = c.last_stall;
    t.str("  last stalled request: type ");
    hex_n(t, s.request_type, 2).str(" req ");
    hex_n(t, s.request, 2).str(" value ");
    hex_n(t, s.value, 4).str(" index ");
    hex_n(t, s.index, 4).str(" length ").dec(s.length).nl();
  }
  group(t, "cdc-to-host", {{"packets", d.cdc_tx_packets}, {"bytes", d.cdc_tx_bytes}, {"written", r.tx_bytes_written}, {"dropped-chunks", r.tx_dropped_chunks}}).nl();
  group(t, "cdc-from-host", {{"packets", d.tokens_cdc_out}, {"bytes", d.cdc_rx_bytes}, {"dropped-chunks", d.cdc_rx_dropped_chunks},
                             {"holds", d.cdc_rx_holds}, {"waiting", r.rx_pending_chunks}}).nl();
  group(t, "midi", {{"to-host", d.midi_in_packets}, {"from-host", d.midi_out_packets}, {"events", d.midi_out_events}, {"skipped", d.midi_out_skipped},
                    {"ragged", d.midi_out_bad_bytes}, {"overrun", d.midi_out_dropped}}).nl();
  group(t, "errors", {{"pid", d.errors[0]}, {"crc5", d.errors[1]}, {"crc16", d.errors[2]}, {"dfn8", d.errors[3]}, {"timeout", d.errors[4]},
                      {"dma", d.errors[5]}, {"stuff", d.errors[7]}}).nl();
  group(t, "faults", {{"stray", d.tokens_stray}, {"resyncs", d.pingpong_resyncs}, {"ep0-stalls", d.ep0_stalls}}).nl();
  return t.size();
}

const char* usb_mode_name(o3os::usb::Mode m) noexcept {
  return m == o3os::usb::Mode::Audio ? "AUDIO" : (m == o3os::usb::Mode::Disk ? "DISK" : "STANDARD");
}

const char* usb_profile_name(o3os::usb::Profile p) noexcept {
  switch (p) {
    case o3os::usb::Profile::P2:
      return "2";
    case o3os::usb::Profile::P8:
      return "8";
    case o3os::usb::Profile::P10:
      return "10";
    case o3os::usb::Profile::P11:
      return "11";
    case o3os::usb::Profile::P4S:
      return "4S";
    case o3os::usb::Profile::P4:
      return "4";
    case o3os::usb::Profile::P6:
      return "6";
  }
  return "?";
}

const char* usb_layout_name(o3os::usb::Profile p, bool usb_return, char* buf) noexcept {
  Text t(buf, 20);
  t.dec(o3os::usb::profile_channels(p)).str(" OUT");
  if (usb_return) {
    t.str(" / ").dec(o3os::usb::kReturnChannels).str(" IN");
  }
  return buf;
}

namespace {

// A signed parts-per-million figure of a rate against the nominal one, both frames per SOF in Q16.16: 32-bit arithmetic only (a 64-bit division would
// pull a library helper into the image). Exact to the third digit for the +/-100,000 Q16 units (3.4 %) it clamps to.
Text& ppm(Text& t, std::uint32_t rate_q16) {
  const std::int32_t nominal = static_cast<std::int32_t>(o3os::usb::nominal_frames_q16(o3os::usb::Speed::Full));
  std::int32_t diff = static_cast<std::int32_t>(rate_q16) - nominal;
  diff = diff > 100000 ? 100000 : (diff < -100000 ? -100000 : diff);
  const std::int32_t v = diff * 1000 / (nominal / 1000);
  t.chr(v < 0 ? '-' : '+').dec(static_cast<std::uint32_t>(v < 0 ? -v : v)).str(" ppm");
  return t;
}

// A frame rate, Q16.16, as "44.1176".
Text& frames_per_sof(Text& t, std::uint32_t q16) {
  std::uint32_t whole = q16 >> 16;
  std::uint32_t frac = ((q16 & 0xFFFFu) * 10000u + 32768u) >> 16;
  if (frac >= 10000u) {  // rounds up to the next whole frame
    frac -= 10000u;
    ++whole;
  }
  t.dec(whole).chr('.');
  const std::uint32_t digits[4] = {frac / 1000u % 10u, frac / 100u % 10u, frac / 10u % 10u, frac % 10u};
  for (std::uint32_t d : digits) {
    t.chr(static_cast<char>('0' + d));
  }
  return t;
}

// The ring fill range, "min..max" ("-" before any packet).
Text& range(Text& t, std::uint32_t lo, std::uint32_t hi) {
  if (lo == 0xFFFFFFFFu) {
    return t.chr('-');
  }
  return t.dec(lo).str("..").dec(hi);
}

}  // namespace

std::size_t format_usb_audio(const UsbAudioReport& r, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  const o3os::usb::Counters& c = *r.counters;
  const o3os::usb::DeviceCounters& d = *r.device;
  t.str("usb audio: ").str(usb_mode_name(r.mode)).str(", profile ").str(usb_profile_name(r.profile)).str(" (");
  t.dec(o3os::usb::profile_channels(r.profile)).str(" ch), host ").str(r.configured ? "configured" : "not configured");
  if (r.change != 0u) {
    t.str(", CHANGING to ").str(usb_mode_name(r.next_mode)).str(" ").str(usb_profile_name(r.next_profile)).str(r.change == 1u ? " (draining)" : " (detached)");
  }
  t.nl();
  char layout[20];
  t.str("layout: ").str(usb_layout_name(r.profile, r.usb_return, layout));  // (a separate line: the first one is parsed by people and tools)
  if (r.change != 0u) {
    t.str(", next ").str(usb_layout_name(r.next_profile, r.next_return, layout));
  }
  t.nl();
  t.str("streams: stems ").str(r.in_active ? "streaming " : "idle");
  if (r.in_active) {
    t.dec(r.in_channels).str(" ch");
  }
  t.str(", return ").str(r.out_active ? "streaming" : "idle").nl();
  t.str("rate: ").str(r.locked ? "locked" : "settling").str(", ");
  frames_per_sof(t, r.rate_q16).str(" frames/SOF (");
  ppm(t, r.rate_q16).str(" vs nominal), SOF ").dec(r.sof_period_q8 >> 8).chr('.').dec((r.sof_period_q8 & 0xFFu) * 10u / 256u).str(" cycles, resyncs ").dec(r.resyncs).nl();
  t.str("feedback: ");
  frames_per_sof(t, r.feedback_q16).str(" frames/SOF, trim ").chr(r.trim_q16 < 0 ? '-' : '+').dec(static_cast<std::uint32_t>(r.trim_q16 < 0 ? -r.trim_q16 : r.trim_q16)).str("/65536, value ");
  if (c.fb_last == 0u) {
    t.chr('-');
  } else {
    hex_n(t, c.fb_last, 6).str(" min ");
    hex_n(t, c.fb_min, 6).str(" max ");
    hex_n(t, c.fb_max, 6);
  }
  t.str(", sent ").dec(d.iso_fb_packets).nl();
  group(t, "in", {{"starts", d.iso_in_starts}, {"packets", d.iso_in_packets}, {"size44", c.in_size_lo}, {"size45", c.in_size_hi}, {"other", c.in_size_other},
                  {"prime", c.in_prime_packets}, {"underruns", c.in_underruns}, {"dropped-blocks", c.in_dropped_blocks}, {"missed", d.iso_in_missed}});
  range(t.str(", fill "), c.in_fill_min, c.in_fill_max).str(" now ").dec(r.stem_fill).nl();
  group(t, "out", {{"starts", d.iso_out_starts}, {"packets", c.out_packets}, {"empty", d.iso_out_empty}, {"bad", c.out_bad_packets},
                   {"dropped-frames", c.out_dropped_frames}, {"underruns", c.out_underruns}});
  range(t.str(", fill "), c.out_fill_min, c.out_fill_max).str(" now ").dec(r.return_fill).nl();
  return t.size();
}

std::size_t format_stem_map(const o3os::audio::StemMap& map, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("stem map: profile ").str(usb_profile_name(map.profile())).str(" (runtime only: the factory map after a reboot)\n");
  std::uint32_t named = 0;
  for (std::uint32_t s = 0; s < map.channels(); ++s) {
    char src[48];
    o3os::audio::format_sources(map.slot(s), src, sizeof src);
    t.str("  S").dec(s + 1u).str(" = ").str(src).nl();
    named |= map.slot(s);
  }
  char rest[64];
  const std::uint32_t tracks = (1u << o3os::audio::stem_src::kTracks) - 1u;
  o3os::audio::format_sources(tracks & ~named, rest, sizeof rest);
  t.str("  heard in the master only: ").str(rest).nl();
  return t.size();
}

std::size_t format_reset_sources(std::uint16_t sources, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  struct Bit {
    std::uint16_t mask;
    const char* name;
  };
  const Bit bits[] = {{kResetPower, "power-on"}, {0x0040, "pin"}, {kResetWatchdog, "watchdog"}, {0x0008, "loss-of-lock"},
                      {0x0004, "loss-of-clock"}, {0x0002, "low-voltage"}, {0x0001, "wakeup"}, {kResetSoftware, "software"},
                      {kResetLockup, "lockup"}, {0x0100, "jtag"}};
  bool first = true;
  for (const Bit& b : bits) {
    if ((sources & b.mask) != 0u) {
      if (!first) {
        t.chr('+');
      }
      t.str(b.name);
      first = false;
    }
  }
  if (first) {
    t.str("none");
  }
  return t.size();
}

std::size_t format_stage(std::uint32_t context, std::uint32_t uptime_ms, std::uint16_t reset_sources, bool crash_record_found,
                         std::uint32_t faults, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("stage: ");
  if (context == crash::kUnknownContext) {
    t.str("unknown (the stage words are not trustworthy)");
  } else {
    t.str(stage_name(crash::stage_of(context))).str(", module ").dec(crash::module_of(context));
  }
  t.str("\nuptime: ").dec(uptime_ms / 1000u).chr('.').dec((uptime_ms % 1000u) / 100u).str(" s\n");
  char reset[64];
  format_reset_sources(reset_sources, reset, sizeof reset);
  t.str("reset: ");
  hex_n(t, reset_sources, 4).str(" (").str(reset).str("), crash record at boot: ");
  yes_no(t, crash_record_found).nl();
  t.str("start-up faults: ");
  if (faults == 0u) {
    t.str("none\n");
  } else {
    struct Named {
      std::uint32_t bit;
      const char* what;
    };
    const Named named[] = {{kFaultAudio, "audio (outputs muted, no sound)"},
                           {kFaultDisplay, "display (headless)"},
                           {kFaultApp, "app (display held dark)"},
                           {kFaultSd, "sd card (not mounted)"}};
    bool first = true;
    for (const Named& n : named) {
      if ((faults & n.bit) != 0u) {
        t.str(first ? "" : ", ").str(n.what);
        first = false;
      }
    }
    t.nl();
  }
  return t.size();
}

std::size_t format_crash(const crash::CrashRecord* record, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  if (record == nullptr) {
    t.str("no crash record\n");
    return t.size();
  }
  char name[24];
  t.str("crash #").dec(record->count).str(": ").str(exception_name(record->exception, name)).str(" at ");
  t.dec(record->uptime_ms / 1000u).chr('.').dec((record->uptime_ms % 1000u) / 100u).str(" s of that run\n");
  t.str("  pc ").hex32(record->pc).str("  lr ").hex32(record->lr).str("  xpsr ").hex32(record->xpsr).nl();
  t.str("  cfsr ").hex32(record->cfsr).str("  hfsr ").hex32(record->hfsr).nl();
  t.str("  mmfar ").hex32(record->mmfar).str("  bfar ").hex32(record->bfar).nl();
  t.str("  exc_return ").hex32(record->exc_return).str("  frame ");
  if (record->sp == crash::kInvalidFrame) {
    t.str("unreadable");
  } else {
    t.hex32(record->sp);
  }
  t.nl();
  if (record->context == crash::kUnknownContext) {
    t.str("  stage unknown\n");
  } else {
    t.str("  stage ").str(stage_name(crash::stage_of(record->context))).str(", module ").dec(crash::module_of(record->context)).nl();
  }
  return t.size();
}

std::size_t format_log_entry(const o3os::usb::diag::LogEntry& e, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.dec(e.ms).str(" ms  ").str(e.text).nl();
  return t.size();
}

std::size_t format_input_event(const core::InputEvent& e, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  switch (e.kind) {
    case core::InputKind::Button:
      t.str("button ").dec(e.id).str(e.value != 0 ? " down" : " up");
      break;
    case core::InputKind::Pad:
      t.str("pad ").dec(e.id);
      if (e.value != 0) {
        t.str(" down ").dec(static_cast<std::uint32_t>(e.value));
      } else {
        t.str(" up");
      }
      break;
    case core::InputKind::Encoder:
      t.str("encoder ").dec(e.id).chr(' ').chr(e.value < 0 ? '-' : '+');
      t.dec(static_cast<std::uint32_t>(e.value < 0 ? -static_cast<std::int32_t>(e.value) : static_cast<std::int32_t>(e.value)));
      break;
    default:
      t.str("kind ").dec(static_cast<std::uint32_t>(e.kind)).str(" id ").dec(e.id).str(" value ").dec(static_cast<std::uint32_t>(e.value));
      break;
  }
  t.str("  @").dec(e.timestamp / core::kTicksPerMs).str(" ms\n");
  return t.size();
}

std::size_t format_input_stats(const hal::InputStats& s, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("input: pads ");
  yes_no(t, (s.online & hal::InputStats::kPadsOnline) != 0u).str(", buttons ");
  yes_no(t, (s.online & hal::InputStats::kButtonsOnline) != 0u);
  t.str(" online; dropped ").dec(s.dropped).str(", bus errors ").dec(s.errors).str(", filtered ").dec(s.filtered).str(", resyncs ").dec(s.resyncs).nl();
  const hal::PadBusStats& bus = s.pad_bus;
  if (bus.scanner.count != 0u || bus.leds.count != 0u || bus.clears != 0u) {
    char chips[56];
    char lines[48];
    format_pad_bus_chips(bus, chips, sizeof chips);
    format_pad_bus_lines(bus, lines, sizeof lines);
    t.str(chips).str("; ").str(lines).nl();
  }
  return t.size();
}

namespace {

Text& probe(Text& t, const char* address, const hal::BusProbe& p) noexcept {
  return t.str(address).chr(' ').str(hal::bus_result_name(p.first)).chr('/').str(hal::bus_result_name(p.last)).str(" x").dec(p.count);
}

Text& levels(Text& t, std::uint8_t bits) noexcept {
  if (bits == hal::BusProbe::kNone) {
    return t.str("---");
  }
  return t.chr((bits & hal::PadBusStats::kLineScl) != 0u ? '1' : '0')
      .chr((bits & hal::PadBusStats::kLineSda) != 0u ? '1' : '0')
      .chr((bits & hal::PadBusStats::kLineInt) != 0u ? '1' : '0');
}

}  // namespace

std::size_t format_pad_bus_chips(const hal::PadBusStats& s, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  probe(t, "pads 34", s.scanner).chr(' ');
  probe(t, "74", s.leds);
  return t.size();
}

std::size_t format_pad_bus_lines(const hal::PadBusStats& s, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("pads clr ").dec(s.clears).chr(' ');
  t.str(s.clear == hal::PadBusStats::kClearReleased ? "rel" : (s.clear == hal::PadBusStats::kClearHeld ? "HELD" : "--"));
  t.str(", lines ");
  levels(t, s.lines_first).chr('>');
  levels(t, s.lines_now);
  return t.size();
}

std::size_t format_version(const char* build, std::uint16_t vid, std::uint16_t pid, std::uint16_t bcd_device, const char* serial, char* out,
                           std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("Track3r o3os, build ").str(build).nl();
  t.str("usb ");
  hex_digits(t, vid, 4).chr(':');
  hex_digits(t, pid, 4).str(" rev ");
  hex_digits(t, bcd_device, 4).str(" serial ").str(serial).nl();
  return t.size();
}

const char* audio_result_name(std::uint8_t result) noexcept {
  switch (static_cast<hal::audio::Device::Result>(result)) {
    case hal::audio::Device::Result::Ok:
      return "Ok";
    case hal::audio::Device::Result::AlreadyStarted:
      return "AlreadyStarted";
    case hal::audio::Device::Result::CodecAbsent:
      return "CodecAbsent";
    case hal::audio::Device::Result::CodecIo:
      return "CodecIo";
    case hal::audio::Device::Result::ClockFault:
      return "ClockFault";
  }
  return "?";
}

const char* audio_fault_where_name(std::uint8_t where) noexcept {
  switch (where) {
    case AudioClockFault::McrWait:
      return "mcr";
    case AudioClockFault::MdrWait:
      return "mdr";
    case AudioClockFault::NoBoundaries:
      return "boundaries";
    default:
      return "none";
  }
}

std::size_t format_audio_start(std::uint8_t result, const AudioClockFault& fault, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("audio start: ").str(audio_result_name(result)).str(" (").dec(result).chr(')');
  if (result == static_cast<std::uint8_t>(hal::audio::Device::Result::Ok)) {
    t.str(", outputs open when clean");
  }
  t.nl();
  if (result == static_cast<std::uint8_t>(hal::audio::Device::Result::ClockFault) && fault.where != AudioClockFault::None) {
    t.str("audio clock fault at ").str(audio_fault_where_name(fault.where)).str(", MCR ").hex32(fault.mcr).str(", MDR ").hex32(fault.mdr).nl();
  }
  return t.size();
}

}  // namespace o3os::platform::mk66::console
