// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/amiga_voices.h>

namespace o3os::audio {
namespace {

constexpr unsigned kMaxTicks = 32;

}  // namespace

AmigaVoices::AmigaVoices() noexcept {
  for (unsigned i = 0; i < paula::kBlepTableLen; ++i) {
    blep_[i] = paula::kMinBlep[i];
  }
  refresh_filters();
}

void AmigaVoices::refresh_filters() noexcept {
  const std::uint8_t model = model_;
  const std::uint8_t led = static_cast<std::uint8_t>((led_ | song_led_) != 0u ? 1u : 0u);
  applied_model_ = model;
  applied_led_ = led;
  for (unsigned i = 0; i < kAmigaVoices; ++i) {
    v_[i].filt.set(static_cast<paula::Model>(model > 2u ? 0u : model), led != 0u);
  }
}

// ---- render ----------------------------------------------------------------------------------------------------------------------

std::int16_t* AmigaVoices::touch(unsigned i) noexcept {
  std::int16_t* const block = blocks_[i];
  if ((touched_ & (1u << i)) == 0u) {
    touched_ |= 1u << i;
    for (std::uint32_t k = 0; k < kBlock; ++k) {
      block[k] = 0;
    }
  }
  return block;
}

void AmigaVoices::render(std::uint32_t begin, std::uint32_t end) noexcept {
  if (begin >= end || end > kBlock) {
    return;
  }
  const std::uint32_t n = end - begin;
  for (unsigned i = 0; i < kAmigaVoices; ++i) {
    Voice& v = v_[i];
    if (!v.on) {
      continue;
    }
    std::int16_t* const block = touch(i) + begin;
    v.ch.run_q15(block, n, blep_, v.filt);  // OS-CPU10 A2: the channel, the model's filter and the Q15 conversion in one loop (render_ref is the three passes)
    if (!v.ch.active() && v.wait == 0u) {
      v.on = false;
    }
  }
}

// The specification of render(): the voice's float block from the channel, the filter over it, the rounding to Q15, three passes. Flash, for the tests (the fused loop of render()
// must give the same int16 for every model and LED setting, an inactive channel included: tests/amiga_voices_test.cpp).
void AmigaVoices::render_ref(std::uint32_t begin, std::uint32_t end) noexcept {
  if (begin >= end || end > kBlock) {
    return;
  }
  const std::uint32_t n = end - begin;
  for (unsigned i = 0; i < kAmigaVoices; ++i) {
    Voice& v = v_[i];
    if (!v.on) {
      continue;
    }
    std::int16_t* const block = touch(i) + begin;
    float tmp[kBlock];
    v.ch.run(tmp, n, blep_);
    v.filt.process(tmp, n);
    for (std::uint32_t k = 0; k < n; ++k) {
      block[k] = paula::Channel::to_q15(tmp[k]);
    }
    if (!v.ch.active() && v.wait == 0u) {
      v.on = false;
    }
  }
}

void AmigaVoices::end_block() noexcept {
  for (unsigned t = 0; t < kAmigaTracks; ++t) {
    feed_[t] = nullptr;
  }
  for (unsigned i = 0; i < kAmigaVoices; ++i) {
    if ((touched_ & (1u << i)) != 0u && ((owner_ >> v_[i].track) & 1u) != 0u) {
      feed_[v_[i].track] = blocks_[i];
    }
  }
  if (model_ != applied_model_ || static_cast<std::uint8_t>((led_ | song_led_) != 0u ? 1u : 0u) != applied_led_) {
    refresh_filters();
  }
}

// ---- notes -----------------------------------------------------------------------------------------------------------------------

unsigned AmigaVoices::find(unsigned track) const noexcept {
  for (unsigned i = 0; i < kAmigaVoices; ++i) {
    if (v_[i].on && v_[i].track == track) {
      return i;
    }
  }
  return kNone;
}

unsigned AmigaVoices::allocate() noexcept {
  unsigned oldest = 0;
  for (unsigned i = 0; i < kAmigaVoices; ++i) {
    if (!v_[i].on) {
      return i;
    }
    if (static_cast<std::int32_t>(v_[i].age - v_[oldest].age) < 0) {
      oldest = i;
    }
  }
  ++stats_.steals;
  return oldest;
}

void AmigaVoices::restart(Voice& v, std::uint32_t offset) noexcept {
  const bool loops = v.loop_end - v.loop_start >= 2u;
  const std::uint32_t end = loops ? v.loop_end : v.length;
  v.ch.start(v.data, offset < end ? offset : end, end, loops ? v.loop_start : 0u, loops ? v.loop_end : 0u);
}

void AmigaVoices::apply(unsigned vi, unsigned track, const med::Out& o) noexcept {
  (void)track;
  Voice& v = v_[vi];
  if (o.led >= 0) {
    song_led_ = static_cast<std::uint8_t>(o.led);
    refresh_filters();
  }
  if (o.cut) {
    v.ch.fade_out();
    v.wait = 0;
    return;
  }
  if (o.period != 0u && o.period != v.ch.period()) {
    v.ch.set_period(o.period);
  }
  v.ch.set_volume(o.volume);
  if (o.start) {
    restart(v, o.offset);
    v.wait = 0;
  }
}

bool AmigaVoices::note_on(unsigned track, unsigned note, unsigned slot, unsigned velocity, const SampleDesc& d, unsigned forced_slice) noexcept {
  (void)slot;
  if (track >= kAmigaTracks || d.length == 0u || d.data == nullptr) {
    ++stats_.ignored;
    return false;
  }
  // The span of the sample this note plays: all of it, or (a beat slice) the slice the note or the Slice FX names, one shot at the root's pitch.
  const std::int16_t* data = d.data;
  std::uint32_t length = d.length;
  bool loops = d.loop_end - d.loop_start >= 2u && d.loop_end <= d.length;
  unsigned pitch_note = note;
  if (d.beat_slice) {
    std::uint32_t from = 0, span = d.length;
    if (!beat_slice_span(d, note, forced_slice, from, span)) {
      note_off(track);  // a slice with no length plays nothing, and the note replaces the track's
      ++stats_.ignored;
      return false;
    }
    data += from;
    length = span;
    loops = false;
    pitch_note = d.slice_sel == kNoForcedSlice ? kRootNote : note;  // OS-BUG-SLICEPAD: the Slice mode follows the note's pitch
  }
  unsigned vi = find(track);
  if (vi == kNone) {
    vi = allocate();
  }
  Voice& v = v_[vi];
  v.data = data;
  v.length = length;
  v.loop_start = loops ? d.loop_start : 0u;
  v.loop_end = loops ? d.loop_end : 0u;
  v.track = static_cast<std::uint8_t>(track);
  owner_ |= 1u << track;
  v.age = ++age_;
  v.on = true;
  v.wait = static_cast<std::uint8_t>(ticks_);
  ++stats_.note_ons;

  med::Note n;
  const int pitch = static_cast<int>(pitch_note) + d.tune;
  n.note = pitch < 0 ? 0 : (pitch > 127 ? 127 : pitch);
  const int fine = d.fine_tune;  // the file's hundredths of a semitone; a MED finetune step is 1/8 of one: 12.5
  const int steps = (2 * fine + (fine < 0 ? -12 : 12)) / 25;
  n.finetune = steps < -8 ? -8 : (steps > 7 ? 7 : steps);
  const unsigned vol = velocity == kVelocityDefault ? d.amiga_vol : (velocity + 1u) >> 1;
  n.volume = static_cast<std::uint8_t>(vol > 64u ? 64u : vol);
  tracks_[track].note_on(n);
  med::Cfg cfg;
  cfg.slide_tick0 = slide_tick0_ != 0u;
  med::Out o;
  tracks_[track].tick(0, ticks_, cfg, o);
  ran0_[track] = true;
  apply(vi, track, o);
  return true;
}

bool AmigaVoices::live_note_on(unsigned track, unsigned note, unsigned slot, unsigned velocity, const VoicePool& samples) noexcept {
  if (slot >= kSampleSlots) {
    return false;
  }
  const SampleDesc& d = samples.sample(slot);
  return d.engine == kEngineAmiga && note_on(track, note, slot, velocity, d, samples.forced_slice(track));
}

void AmigaVoices::note_off(unsigned track) noexcept {
  const unsigned vi = find(track);
  if (vi != kNone) {
    v_[vi].ch.fade_out();
    v_[vi].wait = 0;
  }
}

void AmigaVoices::release_track(unsigned track) noexcept {
  if (track < kAmigaTracks) {
    note_off(track);
    owner_ &= ~(1u << track);
  }
}

void AmigaVoices::release_all() noexcept {
  for (unsigned i = 0; i < kAmigaVoices; ++i) {
    if (v_[i].on) {
      v_[i].ch.fade_out();
      v_[i].wait = 0;
    }
  }
}

void AmigaVoices::reset_pass() noexcept {
  for (Voice& v : v_) {
    v = Voice();
  }
  for (med::Track& t : tracks_) {
    t.reset();
  }
  for (bool& r : ran0_) {
    r = false;
  }
  for (const std::int16_t*& f : feed_) {
    f = nullptr;
  }
  touched_ = 0;
  owner_ = 0;
  age_ = 0;
  ticks_ = 6;
  song_led_ = 0;
  refresh_filters();
}

// ---- ticks and events --------------------------------------------------------------------------------------------------------------

void AmigaVoices::on_tick(unsigned index, unsigned ticks) noexcept {
  ticks_ = ticks < 1u ? 6u : (ticks > kMaxTicks ? kMaxTicks : ticks);
  ++stats_.ticks;
  med::Cfg cfg;
  cfg.slide_tick0 = slide_tick0_ != 0u;
  for (unsigned vi = 0; vi < kAmigaVoices; ++vi) {
    if (v_[vi].wait > 0u) {
      --v_[vi].wait;
    }
  }
  for (unsigned tr = 0; tr < kAmigaTracks; ++tr) {
    if (index == 0u && ran0_[tr]) {
      ran0_[tr] = false;  // the row's note already ran its tick 0
      continue;
    }
    const unsigned vi = find(tr);
    if (vi == kNone) {
      continue;
    }
    med::Out o;
    tracks_[tr].tick(index, ticks_, cfg, o);
    apply(vi, tr, o);
  }
}

bool AmigaVoices::event(const SeqEvent& e, const VoicePool& samples) noexcept {
  switch (e.kind) {
    case EventKind::NoteOn: {
      if (e.b >= kSampleSlots) {
        return false;
      }
      const SampleDesc& d = samples.sample(e.b);
      if (d.engine != kEngineAmiga) {
        release_track(e.track);  // a note of another engine takes the track
        return false;
      }
      (void)note_on(e.track, e.a, e.b, e.c, d, samples.forced_slice(e.track));
      return true;
    }
    case EventKind::NoteOff:
    case EventKind::NoteCut:
    case EventKind::NoteFade:
      if (e.track < kAmigaTracks) {
        note_off(e.track);
      }
      return false;
    case EventKind::Stopped:
      release_all();
      return false;
    case EventKind::FxReset:
      for (unsigned t = 0; t < kAmigaTracks; ++t) {
        tracks_[t].end_cmd(0);
        tracks_[t].end_cmd(1);
      }
      ticks_ = 6;
      return false;
    case EventKind::ModSet:
      if ((e.flags == kModGroupEngine0 || e.flags == kModGroupEngine1) && e.track < kAmigaTracks && e.a >= kFxEngineFirst) {
        tracks_[e.track].set_cmd(e.flags == kModGroupEngine0 ? 0u : 1u, static_cast<std::uint8_t>(e.a - kFxEngineFirst), e.b);
        return true;
      }
      return false;
    case EventKind::ModEnd:
      if ((e.flags == kModGroupEngine0 || e.flags == kModGroupEngine1) && e.track < kAmigaTracks) {
        tracks_[e.track].end_cmd(e.flags == kModGroupEngine0 ? 0u : 1u);
        return true;
      }
      return false;
    case EventKind::TrkFx:
      // OS-COREFX: the tracker FX (46..50) of an Amiga track are the MED commands that mean the same, held until the sequencer ends them with the row (a TrkFx of type 0). The sampler voices
      // run them in audio/tracker_fx.h, which sees the event first; this pool keeps the command whether or not a note of its own plays (it runs only on a track that has an Amiga voice).
      if (e.track < kAmigaTracks) {
        const unsigned col = e.flags & 1u;
        std::uint8_t cmd = 0;
        std::uint8_t data = e.b;
        switch (e.a) {
          case kFxTrkArp: cmd = 0x00; break;
          case kFxTrkVolSlide: cmd = 0x0A; break;
          case kFxTrkFineVol: cmd = (e.b >> 4) != 0u ? 0x1A : 0x1B; data = (e.b >> 4) != 0u ? static_cast<std::uint8_t>(e.b >> 4) : static_cast<std::uint8_t>(e.b & 15u); break;
          case kFxTrkFinePortaUp: cmd = 0x11; data = static_cast<std::uint8_t>((e.b + 2u) / 4u); break;       // quarters of a Paula period unit, to nearest
          case kFxTrkFinePortaDown: cmd = 0x12; data = static_cast<std::uint8_t>((e.b + 2u) / 4u); break;
          // CF2: MED has the retrigger (1F: every y ticks, beside a note; the volume change x has no MED counterpart and is dropped); a pan slide, the waveforms, the glissando and the tremor have none
          // (the tracker commands of the sampler voices only): they end the column's command, like a type 0.
          case kFxTrkRetrig: cmd = (e.b & 15u) != 0u ? 0x1F : 0x00; data = static_cast<std::uint8_t>(e.b & 15u); break;
          default: break;
        }
        if (cmd == 0u && e.a != kFxTrkArp) {
          tracks_[e.track].end_cmd(col);
        } else {
          tracks_[e.track].set_cmd(col, cmd, data);
        }
      }
      return false;
    case EventKind::Tick:
      on_tick(e.a, e.b);
      return true;
    default:
      return false;
  }
}

}  // namespace o3os::audio
