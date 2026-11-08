// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MIDI out router (midi/out.h).
#include <o3os/midi/out.h>
#include <o3os/midi/mtc.h>

namespace o3os::midi {

namespace {
constexpr std::uint8_t kAllPorts = static_cast<std::uint8_t>((1u << kPortCount) - 1u);
// An event's own delay (SeqEvent::value, microseconds, 0 .. kRouteDelayMaxUs), in CPU cycles. A value outside that is held to it.
std::uint64_t route_delay_cycles(const audio::SeqEvent& ev) noexcept {
  if (ev.value <= 0) {
    return 0;  // the usual case: no OFFSET
  }
  const std::int32_t us = ev.value > audio::kRouteDelayMaxUs ? audio::kRouteDelayMaxUs : ev.value;
  return static_cast<std::uint64_t>(us) * (core::kCpuHz / 1000000u);
}
static_assert(core::kCpuHz % 1000000u == 0u, "a microsecond is a whole number of CPU cycles");
}  // namespace

void MidiOut::reset() noexcept {
  staging_.clear();
  book_.clear();
  panic_pending_ = false;
  panic_channels_ = 0;
  panic_due_ = 0;
  retry_ports_ = 0;
  used_ports_ = 0;
  stats_ = Stats{};
}

std::uint64_t MidiOut::offset_due(std::uint64_t due, std::uint32_t port) const noexcept {
  const std::int64_t off = cfg_.offset_cycles[port];
  if (off >= 0) {
    return due + static_cast<std::uint64_t>(off);
  }
  const std::uint64_t back = static_cast<std::uint64_t>(-off);
  return due > back ? due - back : 0u;  // an advance cannot reach before the start of the timeline
}

std::uint8_t MidiOut::stage_full_frame(std::uint64_t due, std::uint8_t ports, std::uint32_t packed) noexcept {
  const Msg m{0, {static_cast<std::uint8_t>(packed & 0xFFu), static_cast<std::uint8_t>((packed >> 8) & 0xFFu), static_cast<std::uint8_t>((packed >> 16) & 0xFFu)}};
  return stage_as(due, ports, m, kStagedFullFrame, false);
}

std::uint8_t MidiOut::stage_as(std::uint64_t due, std::uint8_t ports, const Msg& m, std::uint8_t rt_field, bool note_on) noexcept {
  std::uint8_t took = 0;
  for (std::uint32_t p = 0; p < kPortCount; ++p) {
    if ((ports & (1u << p)) == 0u) {
      continue;
    }
    if (!staging_.push(offset_due(due, p), Staged{m, static_cast<std::uint8_t>(p), rt_field, static_cast<std::uint8_t>(note_on && took == 0u ? 1u : 0u)})) {
      ++stats_.staging_overflow;  // the queue is full: this item is lost (a full queue means the ports are not draining, a driver fault)
    } else {
      ++stats_.queued;
      took = static_cast<std::uint8_t>(took | (1u << p));
    }
  }
  return took;
}

bool MidiOut::send(const audio::SeqEvent& ev, std::uint64_t block_start) noexcept {
  using audio::EventKind;
  const std::uint64_t sample = block_start + ev.offset;
  std::uint64_t due = core::cycles_from_samples(sample);
  switch (ev.kind) {
    case EventKind::MidiClock:
    case EventKind::MidiStart:
    case EventKind::MidiContinue:
    case EventKind::MidiStop:
    case EventKind::MidiMtcQf:  // OS-MTC A3: timed like the clock, to a fraction of a sample
      due = core::cycles_from_samples_sfrac(sample, ev.value);
      break;
    case EventKind::MidiMtcFull:  // `value` is the packed time, not a fraction
    case EventKind::MidiNoteOn:
    case EventKind::MidiNoteOff:
    case EventKind::MidiCc:
    case EventKind::MidiProgram:
    case EventKind::MidiSpp:
    case EventKind::MidiPitchBend:
    case EventKind::MidiChanAt:
    case EventKind::MidiPolyAt:
      break;
    default:
      return false;
  }
  if (suppress_) {
    ++stats_.suppressed;
    return true;
  }
  // OS-INSTUI S8b: the route of a channel message. `flags` names the ports (0: the device's MIDI OUT), `value` is the instrument's OFFSET in microseconds, added to the due time (bounded: held to
  // kRouteDelayMaxUs, so the staging queue holds an item at most 100 ms past its block). The clock and transport kinds do not use either.
  std::uint8_t ports = cfg_.note_ports;
  const bool channel_msg = (ev.kind >= EventKind::MidiNoteOn && ev.kind <= EventKind::MidiProgram) || (ev.kind >= EventKind::MidiPitchBend && ev.kind <= EventKind::MidiPolyAt);  // (the other kinds are the clock's and the transport's)
  if (channel_msg) {
    due += route_delay_cycles(ev);
    if ((ev.flags & audio::kRoutePorts) != 0u) {
      ports = static_cast<std::uint8_t>(ev.flags & audio::kRoutePorts);
    }
  }
  const std::uint8_t want = static_cast<std::uint8_t>(ports & kAllPorts);
  switch (ev.kind) {
    case EventKind::MidiNoteOn:
      // The book follows what was queued: a note-on that no port took (a full queue) owes no off; one that no port was asked to carry is counted as ever.
      if (stage(due, ports, note_on(ev.c, ev.a, ev.b), false, true) != 0u || want == 0u) {
        book_.note_on(ev.c, ev.a);
        used_ports_ = static_cast<std::uint8_t>(used_ports_ | want);
      }
      break;
    case EventKind::MidiNoteOff:
      if (book_.count(ev.c, ev.a) == 0u) {
        ++stats_.stray_offs;  // not owed (already released by a panic, or never sent): nothing to say to the receiver
      } else if (stage(due, ports, note_off(ev.c, ev.a), false) == want) {
        book_.note_off(ev.c, ev.a);  // paid when every port has the off; if one lost it (full queue) the note stays owed, so a later off, a stop or a panic releases it
        if (book_.owed() == 0u) {
          used_ports_ = 0;
        }
      }
      break;
    case EventKind::MidiCc: {
      const std::uint8_t number = (ev.a & audio::kMidiCcExplicit) != 0u ? static_cast<std::uint8_t>(ev.a & 0x7Fu) : (ev.a < 6u ? cfg_.cc_number[ev.a] : kCcUnassigned);
      if (number == kCcUnassigned) {
        ++stats_.dropped_cc;
        break;
      }
      const std::uint8_t ch = ev.c == audio::kChannelDefault ? cfg_.cc_channel : ev.c;
      stage(due, ports, control_change(ch, number, ev.b), false);
      break;
    }
    case EventKind::MidiProgram: {
      const std::uint8_t ch = ev.c == audio::kChannelDefault ? cfg_.cc_channel : ev.c;
      stage(due, ports, program_change(ch, ev.a), false);
      break;
    }
    case EventKind::MidiPitchBend:
      if (ev.track == audio::kNoTrack && ev.a == 0u && ev.b == 0x40u) {  // (8,192 is the centre)
        // The sequencer's own centre (a stop or a restart: Sequencer::centre_bends; a note's centre carries its track): the bends of this channel that are staged behind an OFFSET for these ports go, so none
        // can land after it and leave the receiver bent. (A stop drops them again in stop_all, which also covers a centre that found no room.)
        struct DropBends {
          std::uint8_t status;
          std::uint8_t ports;
          bool operator()(const Staged& s) const noexcept {
            return s.rt == 0u && s.m.b[0] == status && (ports & (1u << s.port)) != 0u && !(s.m.b[1] == 0u && s.m.b[2] == 0x40u);
          }
        };
        staging_.cancel_if(0, DropBends{static_cast<std::uint8_t>(0xE0u | (ev.c & 0x0Fu)), want});
      }
      stage(due, ports, pitch_bend(ev.c, static_cast<std::uint16_t>((ev.a & 0x7Fu) | ((ev.b & 0x7Fu) << 7))), false);
      break;
    case EventKind::MidiChanAt:
      stage(due, ports, channel_pressure(ev.c, ev.a), false);
      break;
    case EventKind::MidiPolyAt:
      stage(due, ports, poly_pressure(ev.c, ev.a, ev.b), false);
      break;
    case EventKind::MidiClock:
      stage(due, cfg_.clock_ports, msg1(kClock), true);
      break;
    case EventKind::MidiStart:
      stage(due, cfg_.clock_ports, msg1(kStart), true);
      break;
    case EventKind::MidiContinue:
      stage(due, cfg_.clock_ports, msg1(kContinue), true);
      break;
    case EventKind::MidiStop:
      stage(due, cfg_.clock_ports, msg1(kStop), true);
      break;
    case EventKind::MidiSpp:
      stage(due, cfg_.clock_ports, song_position(static_cast<std::uint16_t>(ev.value & 0x3FFF)), false);
      break;
    case EventKind::MidiMtcQf:
      stage(due, cfg_.mtc_ports, msg2(kQuarterFrame, ev.a), false);
      break;
    case EventKind::MidiMtcFull:
      stage_full_frame(due, cfg_.mtc_ports, static_cast<std::uint32_t>(ev.value) & 0xFFFFFFu);
      break;
    default:
      break;
  }
  return true;
}

void MidiOut::flush(std::uint64_t block_end_cycles) noexcept {
  // The panic first: the offs it owes are queued like any other note-off, due now. An off that finds no room (a full staging queue) must not be lost - the
  // book has already forgotten the note - so it is kept in the retry slot (the ports that still lack it) and staged first on the next flush; the pump stops
  // for the block meanwhile (OS-SMALL3). Bounded: at most kPanicPerBlock stage() calls per flush.
  if (panic_pending_) {
    bool stalled = false;
    if (retry_ports_ != 0u) {
      retry_ports_ = static_cast<std::uint8_t>(retry_ports_ & ~stage(retry_due_, retry_ports_, note_off(retry_ch_, retry_note_), false));
      stalled = retry_ports_ != 0u;
    }
    for (std::uint32_t i = 0; i < kPanicPerBlock && !stalled; ++i) {
      std::uint8_t ch = 0;
      std::uint8_t note = 0;
      if (!book_.take_next(ch, note, panic_channels_)) {
        panic_pending_ = false;
        panic_channels_ = 0;
        if (book_.owed() == 0u) {
          used_ports_ = 0;
        }
        break;
      }
      const std::uint8_t want = static_cast<std::uint8_t>((used_ports_ != 0u ? used_ports_ : cfg_.note_ports) & kAllPorts);  // (S8b: the ports the owed notes went to, which an instrument may have chosen itself)
      const std::uint8_t took = stage(panic_due_, want, note_off(ch, note), false);
      ++stats_.panic_offs;
      if (took != want) {
        retry_ports_ = static_cast<std::uint8_t>(want & ~took);
        retry_ch_ = ch;
        retry_note_ = note;
        retry_due_ = panic_due_;
        stalled = true;
      }
    }
  }
  const std::uint64_t horizon = block_end_cycles + kHorizonCycles;
  std::uint32_t moved = 0;
  bool full[kPortCount] = {false, false};
  while (moved < kMaxMovePerBlock && !staging_.empty() && staging_.front_key() <= horizon) {
    const Staged s = *staging_.front();
    const std::uint64_t due = staging_.front_key();
    const std::uint32_t p = s.port;
    if (full[p]) {
      break;  // the port's ring is full: keep the order, try again next block
    }
    bool ok = false;
    if (s.rt == kStagedFullFrame) {
      // OS-MTC A3: the full frame, whole or not at all (DIN: push_sysex checks the room of the whole run; USB: four words, so the ring needs room for four).
      const std::uint32_t packed = static_cast<std::uint32_t>(s.m.b[0]) | (static_cast<std::uint32_t>(s.m.b[1]) << 8) | (static_cast<std::uint32_t>(s.m.b[2]) << 16);
      std::uint8_t body[mtc::kFullFrameBodyBytes];
      mtc::full_frame_body(packed, body);
      if (p == 0u) {
        ok = din_ != nullptr ? din_->push_sysex(due, body, mtc::kFullFrameBodyBytes) : true;
      } else if (usb_ != nullptr) {
        std::uint32_t words[4];
        const std::uint32_t nw = usb_sysex_events(cfg_.usb_cable, body, mtc::kFullFrameBodyBytes, words);
        if (usb_->room() >= nw) {
          for (std::uint32_t i = 0; i < nw; ++i) {
            usb_->push(due, words[i]);  // cannot fail: the room was checked and the ring only drains meanwhile
          }
          ok = true;
        }
      } else {
        ok = true;
      }
    } else if (p == 0u) {
      if (din_ != nullptr) {
        ok = s.rt != 0u ? din_->push_rt(due, s.m.b[0]) : din_->push_msg(due, s.m);
      } else {
        ok = true;  // not attached: dropped
      }
    } else if (usb_ != nullptr) {
      ok = usb_->push(due, usb_event(cfg_.usb_cable, s.m));
    } else {
      ok = true;
    }
    if (!ok) {
      full[p] = true;
      ++stats_.port_full;
      break;
    }
    staging_.pop();
    ++stats_.handed;
    ++moved;
  }
}

void MidiOut::stop_all(std::uint64_t due_cycles) noexcept {
  // Notes that were queued and not handed over never sound, so their offs are not owed: they are dropped and the count their note-on added is paid.
  // (A note staged for both ports is one logical note: the count is paid on one copy, the DIN one, or the USB one when DIN carries no notes.)
  // Real-time bytes, SPP, controllers and note-OFFs stay: a staged off may belong to a note that is already sounding. A staged pitch bend that is not the centre goes with its note (OS-INSTUI S8b: a bend
  // staged behind an OFFSET must not land after the centre the sequencer sends with the stop and leave the receiver bent); a staged centre stays.
  struct DropNoteOns {
    HangBook* book;
    bool operator()(const Staged& s) const noexcept {
      if (s.rt != 0u) {
        return false;
      }
      if ((s.m.b[0] & 0xF0u) == 0xE0u) {
        return !(s.m.b[1] == 0u && s.m.b[2] == 0x40u);  // (8,192: the centre)
      }
      if ((s.m.b[0] & 0xF0u) != 0x90u) {
        return false;
      }
      if (s.pay != 0u) {  // (S8b: the copy that paid the book is marked when it is staged, since an instrument may route a note to a port of its own)
        book->note_off(static_cast<std::uint8_t>(s.m.b[0] & 0x0Fu), s.m.b[1]);
      }
      return true;
    }
  };
  staging_.cancel_if(0, DropNoteOns{&book_});
  // The ports are not flushed: what they hold is at most one horizon (2.9 ms) of messages and plays out, and a note-on already handed over must
  // not be able to follow the off that is owed for it. So the offs are due after the horizon, and the FIFO of each port keeps them behind.
  panic_pending_ = book_.owed() != 0u || retry_ports_ != 0u;  // an off that found no room is still owed
  panic_channels_ = 0xFFFFu;
  panic_due_ = due_cycles + kHorizonCycles + din::kByteCycles;
}

void MidiOut::release_channel(std::uint8_t ch, std::uint64_t due_cycles) noexcept {
  panic_channels_ = static_cast<std::uint16_t>(panic_channels_ | (1u << (ch & 0x0Fu)));
  panic_pending_ = true;
  panic_due_ = due_cycles + kHorizonCycles + din::kByteCycles;  // behind everything already handed over, as in stop_all
}

}  // namespace o3os::midi
