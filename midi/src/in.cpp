// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI in, the timing side (midi/in.h): the byte parser and the drain of the midiRt ring.
#include <o3os/midi/in.h>
#include <o3os/midi/mtc_in.h>

namespace o3os::midi {

namespace {
// The full frame the SysEx data bytes `7F id 01 01 hh mm ss ff` spell, as the packed time of mtc::pack_full(); Pass when a field is out of range for its rate code.
InParser::Out full_frame_out(const std::uint8_t* d) noexcept {
  const std::uint8_t code = static_cast<std::uint8_t>((d[0] >> 5) & 3u);
  const mtc::Time t{static_cast<std::uint8_t>(d[0] & 0x1Fu), d[1], d[2], d[3]};
  if (!mtc::label_valid(t, code)) {
    return InParser::Out{ByteKind::Pass, 0};
  }
  return InParser::Out{ByteKind::MtcFull, mtc::pack_full(t, code)};
}
}  // namespace

InParser::Out InParser::feed(std::uint8_t b) noexcept {
  // A real-time byte may sit anywhere, between the bytes of a message too, and does not disturb it.
  if (is_realtime(b)) {
    switch (b) {
      case kClock:
        return Out{ByteKind::Clock, 0};
      case kStart:
        return Out{ByteKind::Start, 0};
      case kContinue:
        return Out{ByteKind::Continue, 0};
      case kStop:
        return Out{ByteKind::Stop, 0};
      default:
        return Out{ByteKind::Pass, 0};  // active sensing, reset, the undefined ones: not ours
    }
  }
  if (is_status(b)) {
    // The F7 of a full frame: eight data bytes of the right shape (the time itself is checked by full_frame_out).
    Out end{ByteKind::Pass, 0};
    if (mtc_ && b == kSysExEnd && sysex_ && sx_ok_ && sx_n_ == 8u) {
      end = full_frame_out(sx_d_);
    }
    sysex_ = b == kSysExStart;
    sx_ok_ = mtc_ && sysex_;
    sx_n_ = 0;
    qf_ = false;
    have_ = 0;
    if (mtc_ && b == kQuarterFrame) {
      status_ = 0;  // a quarter frame is taken whole: its data byte is next, and it starts no running status
      need_ = 0;
      qf_ = true;
      return Out{ByteKind::Consumed, 0};
    }
    if (b == kSongPosition) {
      status_ = b;
      need_ = 2;
      return Out{ByteKind::Consumed, 0};
    }
    // Everything else, a message of someone else's: remember it for running status (channel voice) and let it through.
    if (b >= 0xF0u) {
      status_ = 0;  // system common and SysEx framing do not start a running status
      need_ = 0;
    } else {
      status_ = b;
      need_ = data_bytes_of(b);
    }
    return end;
  }
  // A data byte.
  if (qf_) {
    qf_ = false;
    return Out{ByteKind::MtcQf, b};
  }
  if (sysex_) {
    if (sx_ok_) {
      // Could this still be `7F <device id> 01 01 hh mm ss ff`? (the universal real-time header of a time code message, any device id)
      const std::uint8_t n = sx_n_;
      if (n == 0u) {
        sx_ok_ = b == 0x7Fu;
      } else if (n == 2u || n == 3u) {
        sx_ok_ = b == 0x01u;
      } else if (n >= 4u && n < 8u) {
        sx_d_[n - 4u] = b;
      } else if (n >= 8u) {
        sx_ok_ = false;  // too long
      }
      if (sx_n_ < 9u) {
        ++sx_n_;
      }
    }
    return Out{ByteKind::Pass, 0};
  }
  if (status_ == kSongPosition) {
    d_[have_++] = b;
    if (have_ == need_) {
      have_ = 0;
      status_ = 0;
      return Out{ByteKind::Spp, static_cast<std::uint16_t>(d_[0] | (d_[1] << 7))};
    }
    return Out{ByteKind::Consumed, 0};
  }
  if (status_ != 0u && need_ != 0u) {
    if (++have_ == need_) {
      have_ = 0;  // running status: the next data byte starts another message of the same kind
    }
  }
  return Out{ByteKind::Pass, 0};
}

void MidiIn::reset() noexcept {
  for (InParser& p : parser_) {
    p.reset();
  }
  armed_ = Armed::None;
  have_held_ = false;
}

MidiIn::Drained MidiIn::drain(RtInRing& ring, const ClockMap& map, ClockDll& dll, std::uint32_t max_items, TransportEvent* ev,
                              std::uint32_t ev_cap, RtItem* pass, std::uint32_t pass_cap, MtcEvent* mtc, std::uint32_t mtc_cap) noexcept {
  Drained d;
  const bool mtc_on = cfg_.mtc_enabled && mtc != nullptr && mtc_cap != 0u;
  while (d.items < max_items) {
    RtItem it;
    if (have_held_) {
      it = held_;
    } else if (!ring.pop(it)) {
      break;
    }
    const std::uint32_t port = it.port < kPortCount ? it.port : 0u;

    // Look at the byte without committing the parser, to know whether there is room for what it makes: a clock makes no output, a transport
    // byte one event, a byte that is not timing one `pass` entry. A byte that has no room waits (held) for the next block.
    const bool mtc_port = mtc_on && port == static_cast<std::uint32_t>(cfg_.mtc_port);
    parser_[port].set_mtc(mtc_port);
    InParser trial = parser_[port];
    const InParser::Out o = trial.feed(it.byte);
    const bool timing_port = cfg_.clock_enabled && port == static_cast<std::uint32_t>(cfg_.clock_port);
    const bool makes_event = timing_port && (o.kind == ByteKind::Stop || o.kind == ByteKind::Spp);
    const bool makes_mtc = o.kind == ByteKind::MtcQf || o.kind == ByteKind::MtcFull;  // only a parser told MTC is on says so
    const bool makes_pass = o.kind == ByteKind::Pass || o.kind == ByteKind::MtcFull;  // the F7 of a full frame ends a SysEx the note path also gets
    if ((makes_event && d.events >= ev_cap) || (makes_mtc && d.mtc >= mtc_cap) || (makes_pass && d.passed >= pass_cap)) {
      held_ = it;
      have_held_ = true;
      if (makes_pass) {
        ++stats_.pass_overflow;
      }
      break;
    }
    have_held_ = false;
    parser_[port] = trial;
    ++d.items;
    if (stats_.bytes[port] != 0xFFFFFFFFu) {
      ++stats_.bytes[port];
    }

    switch (o.kind) {
      case ByteKind::Pass:
        pass[d.passed++] = it;
        ++stats_.passed;
        break;
      case ByteKind::Consumed:
        break;
      case ByteKind::MtcQf:
      case ByteKind::MtcFull: {
        // The time code message began a byte time per byte before the stamp on DIN (the latency covers one byte, as for a clock); USB is stamped at the frame, as a clock is.
        const std::uint32_t extra = port == static_cast<std::uint32_t>(Port::Din) ? static_cast<std::uint32_t>(din::kByteCycles) * (o.kind == ByteKind::MtcQf ? 1u : 9u) : 0u;
        const std::uint32_t began = it.cycles - cfg_.latency_cycles[port] - extra;
        if (o.kind == ByteKind::MtcQf) {
          mtc[d.mtc++] = MtcEvent{MtcEvent::Kind::Qf, static_cast<std::uint8_t>(port), static_cast<std::uint8_t>(o.value), 0, 0, began};
          ++stats_.mtc_qf;
        } else {
          pass[d.passed++] = it;
          ++stats_.passed;
          mtc[d.mtc++] = MtcEvent{MtcEvent::Kind::Full, static_cast<std::uint8_t>(port), 0, 0, o.value, began};
          ++stats_.mtc_full;
        }
        break;
      }
      case ByteKind::Clock:
      case ByteKind::Start:
      case ByteKind::Continue:
      case ByteKind::Stop:
      case ByteKind::Spp:
        if (!timing_port) {
          ++stats_.ignored_port;
          break;
        }
        if (o.kind == ByteKind::Clock) {
          const core::SQ32 t = map.to_rel(it.cycles - cfg_.latency_cycles[port], dll.origin());
          const ClockDll::Result r = dll.on_clock(t);
          ++stats_.clocks;
          if (armed_ != Armed::None && r != ClockDll::Result::Ignored && d.events < ev_cap) {
            // The transport begins on this clock (the standard: Start and Continue take effect on the next timing clock).
            std::uint64_t tick = dll.tick();
            if (cfg_.start_pulses > 0) {
              tick += static_cast<std::uint64_t>(cfg_.start_pulses);
            } else if (cfg_.start_pulses < 0) {
              const std::uint64_t back = static_cast<std::uint64_t>(-static_cast<std::int32_t>(cfg_.start_pulses));
              tick = tick > back ? tick - back : 0u;
            }
            ev[d.events++] = TransportEvent{armed_ == Armed::Start ? TransportEvent::Kind::Start : TransportEvent::Kind::Continue,
                                            armed_port_, 0, tick};
            ++stats_.transport;
            armed_ = Armed::None;
          }
        } else if (o.kind == ByteKind::Start || o.kind == ByteKind::Continue) {
          armed_ = o.kind == ByteKind::Start ? Armed::Start : Armed::Continue;
          armed_port_ = static_cast<std::uint8_t>(port);
        } else if (o.kind == ByteKind::Stop) {
          armed_ = Armed::None;
          ev[d.events++] = TransportEvent{TransportEvent::Kind::Stop, static_cast<std::uint8_t>(port), 0, 0};
          ++stats_.transport;
        } else {
          ev[d.events++] = TransportEvent{TransportEvent::Kind::Spp, static_cast<std::uint8_t>(port),
                                          static_cast<std::uint16_t>(o.value & 0x3FFFu), 0};
          ++stats_.transport;
        }
        break;
    }
  }
  return d;
}

}  // namespace o3os::midi
