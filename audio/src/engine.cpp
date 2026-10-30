// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/engine.h>

#include <o3os/audio/kernels_impl.h>

namespace o3os::audio {
namespace {
constexpr std::uint32_t kBlock = hal::audio::kBlock;
}  // namespace

Engine::Engine(CycleFn cycles, TapMem& taps) noexcept : cycles_(cycles), graph_(cycles, taps) { tfx_.attach(&graph_); }

bool Engine::set_tone(bool on) noexcept {
  if (on && !tone_target_.load(std::memory_order_relaxed)) {
    // The script's strips, instant (ramp 1) because the tones are still silent: track 1 is the left channel, track 2 the right.
    struct Setting {
      unsigned slot;
      unsigned field;
      float value;
    };
    static constexpr Setting kProfile[] = {
        {0, graph_param::strip::Volume, 100.0f}, {0, graph_param::strip::Pan, 0.0f},   {0, graph_param::strip::Mute, 0.0f},
        {0, graph_param::strip::Route, static_cast<float>(route::kMain)},
        {1, graph_param::strip::Volume, 100.0f}, {1, graph_param::strip::Pan, 100.0f}, {1, graph_param::strip::Mute, 0.0f},
        {1, graph_param::strip::Route, static_cast<float>(route::kMain)},
    };
    for (const Setting& s : kProfile) {
      if (!params().push(ParamChange{strip_param(s.slot, s.field), s.value, 1u})) {
        return false;  // nothing is started on a ring that could not take the settings; the changes already pushed are harmless
      }
    }
  }
  tone_target_.store(on, std::memory_order_relaxed);
  return true;
}

void Engine::attach_voices(VoicePool* voices) noexcept {
  voices_ = voices;
  if (voices != nullptr) {
    graph_.use_feed_sources((1u << kTracks) - 1u);  // every track takes its voice's block
  }
  graph_.set_instrument_hook(voices != nullptr ? &Engine::instrument_hook : nullptr, this);
}

void Engine::attach_modules(ModuleVoices* modules) noexcept {
  modules_ = modules;
  if (modules != nullptr) {
    modules->set_cycles(cycles_);  // OS-GOV: the governor measures the instances with the engine's counter
    // (Graph::configure() has no production caller yet; when one comes, its mask must OR in kAuditionTrack, or this strip is switched off again.)
    graph_.activate(kAuditionTrack);  // OS-MODSILENT: the strip of track 16 is outside the twelve a new project has; a module note on it was skipped by the strip pass (no sound, no meter, no `mix` line)
  }
}

// OS-MODPITCH: a ModSet / ModEnd (the groups the sampler voices take in flash) or an FxReset for the module instruments: ModuleVoices::fx_event keeps the Slide and Glide FX and ramps the pitch of a module that has `pitch`.
void Engine::module_fx(const SeqEvent& e) noexcept {
  if (modules_ != nullptr) {
    modules_->fx_event(e);
  }
}

// OS-COREFX: the events of the tracker FX processor (Tick, TrkFx, FxReset, the Panning FX's ModSet and ModEnd). In flash; dispatch() calls it for every event and the test is here.
void Engine::tracker_event(const SeqEvent& e) noexcept {
  if (TrackerFx::takes(e)) {
    tfx_.event(e, *voices_);
  }
}

// OS-MOD: the tracks whose voice has a pan modulator (autopan) give the strip the pan they evaluated this block. In flash, called through a long call from play_block: once a block per modulated voice.
void Engine::apply_pan_mods() noexcept {
  for (std::uint32_t m = voices_->take_pan_mods(); m != 0u; m &= m - 1u) {
    const unsigned t = static_cast<unsigned>(__builtin_ctz(m));
    graph_.set_voice_pan(t, voices_->pan_mod_value(t), 0u);
  }
}

// OS-BUG-MODDRIVE: the module instruments' Overdrive and Bit depth, the sampler's curve (VoicePool::shape_pcm) over the instance's block in place, after its own render and filter (the sampler shapes after its
// filter too) and before the feed. Only the tracks of `mask` that a module sounds on this block (ModuleVoices::feed_mut, as feed() selects). The instrument's value of a module track is neutral (no row for it;
// OVERDRIVE 0, BIT DEPTH 16), so the step FX 38 / 39 and the layer decide. One call a block, in flash; the values at the block's end apply to the whole block (an event can act up to one block early).
void Engine::shape_modules(std::uint16_t mask) noexcept {
  for (unsigned t = 0; t < kModuleTracks; ++t) {
    if (((mask >> t) & 1u) == 0u) {
      continue;
    }
    std::int16_t* io = modules_->feed_mut(t);
    if (io == nullptr) {
      continue;
    }
    const VoicePool::Shape s = voices_->shape_track(t, 0u, 16u, 0u);
    VoicePool::shape_pcm(io, kBlock, s.drive, s.crush, s.round != 0u);
  }
}

bool Engine::instrument_hook(void* self, const ParamChange& change) noexcept {
  Engine& e = *static_cast<Engine*>(self);
  const unsigned slot = core::param_index(change.id);
  std::uint32_t pan_tracks = 0;
  if (e.voices_ == nullptr || !e.voices_->apply_param(slot, core::param_field(change.id), change.value, pan_tracks)) {
    return false;
  }
  for (unsigned t = 0; pan_tracks != 0u && t < kVoiceTracks; ++t, pan_tracks >>= 1) {
    if ((pan_tracks & 1u) != 0u) {
      e.graph_.set_voice_pan(t, e.voices_->sample(slot).pan, 0u);  // the voice's pan, within the next block's ramp: the same place a note's start puts it
    }
  }
  const unsigned field = core::param_field(change.id);
  if (field == inst_param::DelaySend || field == inst_param::ReverbSend) {  // OS-SENDFX: a live edit of a send reaches the tracks that play the instrument
    const SampleDesc& d = e.voices_->sample(slot);
    for (std::uint32_t m = e.voices_->slot_tracks(slot); m != 0u; m &= m - 1u) {
      e.graph_.set_voice_sends(static_cast<unsigned>(__builtin_ctz(m)), d.delay_send, d.reverb_send, 0u);  // within the next block's ramp, like the other knobs
    }
  }
  return true;
}

void Engine::attach_events(const EventSource& source) noexcept {
  source_ = source;
  graph_.set_param_hook(source.param, source.self);
}

// What a note event does to the voices; the events of the other kinds (modulation, MIDI, the UI's) are not this slice's.
void Engine::dispatch(const SeqEvent& e) noexcept {
  tracker_event(e);  // OS-COREFX, in flash; before the Amiga voices, which take the Tick event entirely (a track of theirs has no sampler voice: the processor skips it)
  if (amiga_ != nullptr && amiga_->event(e, *voices_)) {  // an Amiga instrument's note, a Tick, an engine FX (OS-005h): the sampler's voice on the track gives way
    if (e.kind == EventKind::NoteOn) {
      voices_->cut(e.track);
      if (modules_ != nullptr) {
        modules_->release_track(e.track);
      }
      graph_.set_voice_pan(e.track, voices_->sample(e.b).pan);
      graph_.set_voice_sends(e.track, voices_->sample(e.b).delay_send, voices_->sample(e.b).reverb_send);
      tfx_.note_started(e.track, voices_->sample(e.b).pan);  // OS-COREFX CF2: the pan the tracker FX holds outlives an Amiga instrument's note as well
    }
    return;
  }
  switch (e.kind) {
    case EventKind::NoteOn:
      if (modules_ != nullptr && modules_->routed(e.b)) {  // an SD module's instrument (OS-012a): the sampler's voice on the track gives way, the pan is the centre
        voices_->cut(e.track);
        if (modules_->note_on(e.track, e.a, e.b, e.c)) {
          graph_.set_voice_pan(e.track, 50);
          graph_.set_voice_sends(e.track, voices_->sample(e.b).delay_send, voices_->sample(e.b).reverb_send);
          tfx_.note_started(e.track, 50);  // ... and an SD module's
        }
        break;
      }
      if (modules_ != nullptr) {
        modules_->release_track(e.track);  // a sample note on a track a module held: the module's note ends
      }
      if (voices_->note_on(e.track, e.a, e.b, e.c)) {
        graph_.set_voice_pan(e.track, voices_->sample(e.b).pan);  // the instrument's pan goes to the track where the note starts (OS-007k)
        graph_.set_voice_sends(e.track, voices_->sample(e.b).delay_send, voices_->sample(e.b).reverb_send);  // and its sends (OS-SENDFX)
        tfx_.note_started(e.track, voices_->sample(e.b).pan);  // OS-COREFX CF2: the pan the tracker FX holds (Channel pan, the Panning FX while held) goes over the instrument's
      }
      break;
    case EventKind::NoteOff:
      voices_->note_off(e.track);
      if (modules_ != nullptr) {
        modules_->note_off(e.track);
      }
      break;
    case EventKind::NoteCut:
      voices_->cut(e.track);
      if (modules_ != nullptr) {
        modules_->note_off(e.track);  // a module ends in its own release
      }
      break;
    case EventKind::NoteFade:
      voices_->fade(e.track);
      if (modules_ != nullptr) {
        modules_->note_off(e.track);
      }
      break;
    case EventKind::StripMute:  // the quantized mute (OS-MUTE) reached its boundary sample
      graph_.set_strip_mute(e.track, e.a != 0u);
      break;
    case EventKind::Metro:  // OS-METRO: a beat; the click is mixed into the TX block by Graph::render()
      graph_.metro_beat(e.offset, e.a != 0u);
      break;
    case EventKind::Stopped:
      graph_.rec().on_stopped(e.offset);  // a take told to end at a boundary ends where the timeline does (OS-REC2)
      graph_.loop().on_stopped(e.offset);  // the loop of a looped take fades out with the transport (OS-LOOPER)
      voices_->release_all();  // the notes the stop owed have come already; this holds if one was lost
      voices_->transport_stopped();  // the LFOs' count runs on the tempo in force from here (OS-BUG-WTLFO)
      if (modules_ != nullptr) {
        modules_->release_notes();
      }
      break;
    case EventKind::ModSet:  // the modulations the voices act on so far: the Slice FX and the Cutoff FX (the other groups are not this slice's)
      if (e.flags == kModGroupSlice) {
        const std::int32_t v = e.c == kLayerPerf ? e.value : static_cast<std::int32_t>(e.b);
        voices_->set_forced_slice(e.track, v < 0 ? 0u : static_cast<unsigned>(v));
      } else if (e.flags == kModGroupFilter) {
        // The winner is the sequencer's Cutoff FX (a is its type, b its value) or the performance layer's slot (c == kLayerPerf: `value` is the type and the offset, a and b the
        // Cutoff FX underneath when the track has one, a == 0 when it has not): the voices take both at once, the layer over the FX.
        const bool fx = e.a >= kFxFilterFirst && e.a < kFxFilterFirst + kFxFilterCount;
        const bool perf = e.c == kLayerPerf;
        voices_->set_filter(e.track, fx, fx ? static_cast<unsigned>(e.a - kFxFilterFirst) : 0u, e.b, perf ? perf_filter_type(e.value) + 1u : 0u, perf_filter_offset(e.value));
      } else {  // (the Filter LFO FX 35, group kModGroupLfoFilter, and the layer's LFO rows: VoicePool::layer_event, in flash; OS-PERF-LFO took the group's own branch out of this SRAM_L function)
        voices_->fx_event(e.track, e);  // the groups that live in flash (the Slide FX, OS-PFX; the performance layer's value, OS-PERFFX): this function is SRAM_L code, and each group named here costs it bytes
        module_fx(e);
      }
      break;
    case EventKind::ModEnd:
      if (e.flags == kModGroupSlice) {
        voices_->clear_forced_slice(e.track);
      } else if (e.flags == kModGroupFilter) {
        voices_->set_filter(e.track, false, 0, 0, 0, 0);  // no source is left: the instrument's own filter
      } else {
        voices_->fx_event(e.track, e);  // no source is left: the note's own pitch (Slide FX), the instrument's own value, no layer
        module_fx(e);
      }
      break;
    case EventKind::FxReset:
      voices_->clear_forced_slices();
      voices_->restart_clock();  // the transport starts: the filter LFOs' tick count too (OS-007m); before the FX end, so that an LFO the FX end starts afresh (mod_start) is not given the stopped count twice
      voices_->clear_filter_fxs();
      voices_->clear_slide_fxs();
      module_fx(e);
      break;
    case EventKind::Step:
      voices_->on_step(e.value < 0 ? 0u : static_cast<unsigned>(e.value));  // the step grid the filter LFOs lock to (OS-007m)
      if ((e.flags & kEvCountIn) == 0u) {  // (OS-METRO: not a step of the count-in: a take armed before it waits for the step after it)
        graph_.rec().on_step(e.value < 0 ? 0u : static_cast<std::uint32_t>(e.value), e.offset);  // a take armed to start on a step begins at this sample (OS-011c)
        graph_.loop().on_step(e.value < 0 ? 0u : static_cast<std::uint32_t>(e.value), e.offset);  // the loop restarts every N bars at this sample (OS-LOOPER)
      }
      break;
    default:
      break;
  }
}

// A live note of an Amiga instrument (OS-005h), per note event, so in flash: as dispatch() does for the sequencer's, the sampler's voice and a module's on the track give way and
// the instrument's pan goes to the track. False: nothing started.
bool Engine::live_amiga_on(unsigned track, unsigned note, unsigned slot, unsigned velocity) noexcept {
  voices_->cut(track);
  if (modules_ != nullptr) {
    modules_->release_track(track);
  }
  if (!amiga_->live_note_on(track, note, slot, velocity, *voices_)) {
    return false;
  }
  graph_.set_voice_pan(track, voices_->sample(slot).pan);
  graph_.set_voice_sends(track, voices_->sample(slot).delay_send, voices_->sample(slot).reverb_send);
  return true;
}

// The live notes of the main loop (audio/live_notes.h, QUEUE CONTRACT): at most kLivePerBlock events, each at sample 0 of the block, before the sequencer's. A note on takes a
// track by pick_track() over what the live notes hold and the voices sound; a ninth live note cuts the oldest first. OS-CPU10: in flash,
// called by play_block() only when the queue is not empty.
void Engine::play_live() noexcept {
  if (live_ == nullptr) {
    return;
  }
  LiveEvent e;
  LiveNotes& held = live_->notes;
  unsigned taken = 0;
  for (unsigned n = 0; n < kLivePerBlock && live_->queue.pop(e); ++n) {
    ++taken;
    if (source_.live != nullptr && source_.live(source_.self, e)) {  // OS-INSTUI S2b: a pad note of a MIDI instrument is the MIDI link's (seq/midi_link.h): no voice, no track
      continue;
    }
    switch (e.kind) {
      case LiveKind::On: {
        std::uint32_t playing = 0;
        for (unsigned t = 0; t < kLiveTracks; ++t) {
          playing |= (voices_->track_playing(t) || (modules_ != nullptr && modules_->track_busy(t)) || (amiga_ != nullptr && amiga_->track_busy(t))) ? 1u << t : 0u;  // a module's note holds its track like a voice
        }
        const LiveNotes::Start s = held.begin(e.source, e.key, e.track, playing, e.note, e.channel);
        if (s.cut != kLiveNone) {
          voices_->cut(s.cut);
          if (amiga_ != nullptr) {
            amiga_->note_off(s.cut);
          }
          if (modules_ != nullptr) {
            modules_->note_off(s.cut);  // as a NoteCut: a module ends in its own release
          }
        }
        const unsigned slot = e.instrument != 0u ? e.instrument - 1u : kSampleSlots;  // instrument 1..64 is slot 0..63; 0 names none
        const unsigned note = e.note > 127u ? 127u : e.note;
        const unsigned velocity = e.velocity == kLiveVelocityDefault ? kVelocityDefault : (e.velocity > 127u ? 127u : e.velocity);
        if (amiga_ != nullptr && slot < kSampleSlots && voices_->sample(slot).engine == kEngineAmiga) {  // an Amiga instrument (OS-005h)
          if (!live_amiga_on(s.track, note, slot, velocity)) {
            (void)held.end(e.source, e.key);
          }
          break;
        }
        if (amiga_ != nullptr) {
          amiga_->release_track(s.track);  // a note of another engine takes the track
        }
        if (modules_ != nullptr && modules_->routed(slot)) {  // as dispatch(): an SD module's instrument: the sampler's voice on the track gives way, the pan is the centre
          voices_->cut(s.track);
          if (modules_->note_on(s.track, note, slot, velocity, true)) {  // a live key over a held one is legato
            graph_.set_voice_pan(s.track, 50);
            graph_.set_voice_sends(s.track, voices_->sample(slot).delay_send, voices_->sample(slot).reverb_send);
          } else {
            (void)held.end(e.source, e.key);
          }
          break;
        }
        if (modules_ != nullptr) {
          modules_->release_track(s.track);  // a sample note on a track a module held: the module's note ends and its block is no longer the track's
        }
        if (voices_->note_on(s.track, note, slot, velocity)) {
          graph_.set_voice_pan(s.track, voices_->sample(slot).pan);  // as dispatch(): the instrument's pan goes to the track where the note starts
          graph_.set_voice_sends(s.track, voices_->sample(slot).delay_send, voices_->sample(slot).reverb_send);
          voices_->mark_live();  // the Wave screen's playhead follows this voice (voices.h, live_position)
        } else {
          (void)held.end(e.source, e.key);  // nothing started (an empty slot): there is no note to release later
        }
        break;
      }
      case LiveKind::Off: {
        const std::uint8_t t = held.end(e.source, e.key);
        if (t != kLiveNone) {
          voices_->note_off(t);
          if (amiga_ != nullptr) {
            amiga_->note_off(t);
          }
          if (modules_ != nullptr) {
            modules_->note_off(t);
          }
        }
        break;
      }
      case LiveKind::AllOff: {
        const std::uint32_t tracks = held.end_all(e.source);
        for (unsigned t = 0; t < kLiveTracks; ++t) {
          if ((tracks & (1u << t)) != 0u) {
            voices_->note_off(t);
            if (amiga_ != nullptr) {
              amiga_->note_off(t);
            }
            if (modules_ != nullptr) {
              modules_->note_off(t);
            }
          }
        }
        break;
      }
      default:
        break;
    }
  }
  if (taken != 0u) {
    live_held_.store(static_cast<std::uint8_t>(held.held()), std::memory_order_release);  // the main loop's live_idle() reads it (single word, no tearing)
  }
}

// The block's events from the source, the notes at their offsets, the voices' blocks to the tracks.
void Engine::play_block() noexcept {
  const std::uint32_t t0 = cycles_();
  std::uint32_t n = 0;
  if (source_.step != nullptr && source_.buffer != nullptr) {
    n = source_.step(source_.self, clock_, kBlock, source_.buffer, source_.capacity);
    n = n < source_.capacity ? n : source_.capacity;
    if (source_.tempo != nullptr) {
      const std::uint32_t tempo = source_.tempo(source_.self);
      voices_->set_tempo_x100(tempo);  // the filter LFOs' tick rate (OS-007m); a Tempo FX mid-block takes effect from the next block
      if (modules_ != nullptr) {
        modules_->set_tempo_x100(tempo);  // OS-MODPITCH: the Slide FX of a module lasts a step of it
      }
      if (tempo != fx_tempo_) {        // the delay's synced times (OS-SENDFX): told when it changed, the call is in flash
        fx_tempo_ = tempo;
        graph_.set_send_fx_tempo(tempo);
      }
    }
  }
  clock_ += kBlock;
  const std::uint32_t t1 = cycles_();
  // OS-ODD11: the voices of an inaudible strip skip their post stages (VoicePool GATE). The mask is the previous block's, from the graph; it is empty at boot, for every block of an export (render_offline clears
  // it first: a stem pass mutes and unmutes the strips itself and must hear every voice from its first block) and for the first live block after it (offline_leave).
  voices_->set_gate(graph_.quiet_tracks() & ~gate_off_);
  voices_->begin_block();  // takes the staged descriptor (OS-BUG-SLICEPITCH): it must run before Graph::drain (graph.cpp, render), which SLICEPITCH relies on
  if (modules_ != nullptr) {
    modules_->begin_block();
  }
  if (amiga_ != nullptr) {
    amiga_->begin_block();
  }
  if (live_ != nullptr && !live_->queue.empty()) {  // OS-CPU10: the test stays here, the body (flash) runs on a pad press only
    play_live();
  }
  std::uint32_t cursor = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const SeqEvent& e = source_.buffer[i];
    const std::uint32_t at = e.offset < kBlock ? e.offset : kBlock - 1u;
    if (at > cursor) {
      voices_->render(cursor, at);  // up to the event: the notes that sound go on, then the event
      if (modules_ != nullptr) {
        modules_->render(cursor, at);
      }
      if (amiga_ != nullptr) {
        amiga_->render(cursor, at);
      }
      cursor = at;
    }
    dispatch(e);
  }
  voices_->render(cursor, kBlock);
  voices_->end_block();
  if (voices_->pan_mods_pending()) {
    apply_pan_mods();
  }
  if (amiga_ != nullptr) {
    amiga_->render(cursor, kBlock);
    amiga_->end_block();
  }
  if (modules_ != nullptr) {
    modules_->render(cursor, kBlock);
    modules_->end_block();
    const std::uint16_t shaping = voices_->shape_mask();  // OS-BUG-MODDRIVE: FX 38 / 39 or the layer's OVERDRV / BITDEPTH held on some track: one flash call a block (shape_modules), else none
    if (shaping != 0u) {
      shape_modules(shaping);
    }
  }
  if (modules_ != nullptr || amiga_ != nullptr) {
    for (unsigned t = 0; t < kTracks; ++t) {
      const std::int16_t* m = modules_ != nullptr ? modules_->feed(t) : nullptr;
      if (m == nullptr && amiga_ != nullptr) {
        m = amiga_->feed(t);
      }
      graph_.feed(t, m != nullptr ? m : voices_->feed(t));
    }
  } else {
    for (unsigned t = 0; t < kTracks; ++t) {
      graph_.feed(t, voices_->feed(t));
    }
  }
  seq_cycles_ = t1 - t0;
  voice_cycles_ = cycles_() - t1;
}

// ---- OS-WAVEXP: the export has the engine (offline_request): this block is silence and renders nothing (the ownership protocol is in engine.h). One load when it has not. In flash: see engine.h.
void Engine::trampoline(void* user, const hal::audio::BlockIo& io) noexcept {
  Engine& e = *static_cast<Engine*>(user);
  if (e.want_.load(std::memory_order_acquire)) {
    for (std::uint32_t i = 0; i < hal::audio::kBlockSamples; i += 2u) {
      kernel::detail::store_pair(io.out + i, 0u);
    }
    e.parked_.store(true, std::memory_order_release);
    return;
  }
  e.render_block(io);
}

void Engine::render_block(const hal::audio::BlockIo& io) noexcept {
  // ---- the test hook of HW-A: a render that overruns on purpose, once. Before the stage stamps, so the stages stay honest; the
  // total shows it. The iteration count is capped well above what the cycles need (a cycle-counter call is at least ten cycles).
  const std::uint32_t stall = stall_cycles_.load(std::memory_order_relaxed);
  if (stall != 0u) {
    stall_cycles_.store(0, std::memory_order_relaxed);
    const std::uint32_t begin = cycles_();
    for (std::uint32_t polls = 0; polls < stall && static_cast<std::uint32_t>(cycles_() - begin) < stall; ++polls) {
    }
  }

  // ---- the tone script: the left tone from the switch-on, the right one from block `left_only_blocks` on; both phases run from the
  // start, so the script is a pure function of the block count. The sources are the graph's; the script speaks on a change only.
  const bool want = tone_target_.load(std::memory_order_relaxed);
  const std::uint32_t index = want ? blocks_on_ : 0u;
  blocks_on_ = want ? blocks_on_ + 1u : 0u;
  const bool right = want && index >= left_only_blocks_.load(std::memory_order_relaxed);
  if (want != script_left_) {
    script_left_ = want;
    graph_.script_tone(0, want);
  }
  if (right != script_right_) {
    script_right_ = right;
    graph_.script_tone(1, right);
  }

  if (voices_ != nullptr) {
    play_block();  // the events and the voices first: their blocks are the tracks' inputs
  }

  graph_.render(io);

  if (voices_ != nullptr) {
    io.report->stage_cycles[hal::audio::kStageDrain] += seq_cycles_;
    io.report->stage_cycles[hal::audio::kStageRender] += voice_cycles_;
    if (modules_ != nullptr) {
      modules_->block_done(io.report->stage_cycles);  // OS-GOV: the block's stages (wall time, a USB interrupt inside them counts; not the RX copy's 1 % and the commit) are the governor's recent worst render
    }
  }
}

// ---- OS-WAVEXP: offline rendering (engine.h, OFFLINE RENDERING) ---------------------------------------------------------------------------------------

void Engine::offline_request() noexcept {
  parked_.store(false, std::memory_order_relaxed);
  want_.store(true, std::memory_order_release);
}

bool Engine::offline_ready(bool live_running) const noexcept {
  return want_.load(std::memory_order_relaxed) && (!live_running || parked_.load(std::memory_order_acquire));
}

// Every voice, module note and Amiga voice stops at once (the residual of a sampler voice fades within a block). The graph's strips, the effects' memory and the limiter are not touched.
void Engine::offline_quiet() noexcept {
  if (voices_ == nullptr) {
    return;
  }
  for (unsigned t = 0; t < kTracks; ++t) {
    voices_->cut(t);
    if (amiga_ != nullptr) {
      amiga_->release_track(t);
    }
    if (modules_ != nullptr) {
      modules_->release_track(t);
    }
  }
  if (modules_ != nullptr) {
    modules_->release_notes();
  }
  if (offline_entered_ && amiga_ != nullptr) {
    amiga_->reset_pass();  // the Amiga voices, the MED tracks and the song's LED switch start every pass of the export as they were built (release_track above only fades a voice)
  }
  if (offline_entered_) {
    voices_->seed_random();  // the random LFO shape draws from its seed in every pass of the export (not when the export gives the engine back)
  }
  graph_.flush_master_limiter();  // the limiter's look-ahead holds the last block of what rang before (a cancel in the middle of a sound): a live block must not start with it, nor a pass of the export with the last pass's
  graph_.clear_step_sends(1u);  // OS-PERFFX P3 (spec 8.15): a step FX 30 / 32 a Stop left (or the export's own, at its end) ends at once, as perf_clear's marks do: with ramp 0 the first block of the export (or of the live render) would climb through it; TrackerFx::reset below then finds none
  tfx_.reset(*voices_);  // OS-COREFX: the tracker FX processor's state (a channel volume or pan a play left, an arpeggio) and the strips' gains and pans it set: the export starts from a transport start's, and a cancelled one leaves no FX behind
}

bool Engine::offline_param(void* self, const ParamChange& change) noexcept {
  Engine& e = *static_cast<Engine*>(self);
  const bool exported = e.source_.param != nullptr && e.source_.param(e.source_.self, change);
  const bool live = e.live_source_.param != nullptr && e.live_source_.param(e.live_source_.self, change);
  return exported || live;
}

void Engine::offline_enter(const EventSource& source) noexcept {
  if (offline_entered_ || !want_.load(std::memory_order_relaxed)) {
    return;
  }
  offline_entered_ = true;
  live_source_ = source_;
  live_clock_ = clock_;
  source_ = source;
  graph_.set_param_hook(&Engine::offline_param, this);
  clock_ = 0;
  fx_tempo_ = 0;  // the send effects are told the tempo again, the export's, by its first block
  stems_were_on_ = graph_.stems().enabled();
  graph_.stems().set_enabled(false);
  feed_was_ = graph_.input_feed();
  graph_.set_input_feed(InputFeed::CodecOnly);  // the codec's RX is the silent block the caller gives; the USB return is pulled (its ring must not overrun) but not heard
  AppTap* const tap = graph_.app_tap();  // OS-H2a: an app's tap, scope or spectrum is not fed the export (the main loop is their one switch and the export is modal)
  tap_was_ = tap != nullptr ? static_cast<std::uint32_t>(tap->source()) : 0u;
  if (tap != nullptr) {
    tap->set_source(AppTap::Source::Off);
  }
  if (modules_ != nullptr) {
    governed_was_ = modules_->governed();  // the SD modules are not judged by the wall clock of the main loop: an export sounds like a live Play that fits
    modules_->set_governed(false);
  }
  scope_was_ = graph_.scope_enabled();
  spectrum_was_ = graph_.spectrum_enabled();
  graph_.set_scope_enabled(false);
  graph_.set_spectrum_enabled(false);
  graph_.loop().park(true);  // OS-LOOPER: an export hears no loop and moves none
  if (voices_ != nullptr) {  // OS-PERFFX P0: the export's sequencer has no Perform layer, so the live one (held in Master or Song) is put away: it must not colour the export's notes, and the export's own events must not wipe it
    voices_->perf_save(perf_voices_);
    voices_->perf_clear();
  }
  graph_.perf_save(perf_strips_);
  graph_.perf_clear();
  offline_quiet();
  stop_live_notes();
}

// OS-EXPNOTE: the live notes that are held (a pad, a MIDI key, a note-on that never got its note-off) end here, at the export's start, instead of refusing it: the voices were cut by
// offline_quiet(), the table forgets the notes (a late note off then finds none and does nothing), the live source ends the notes it holds itself (the MIDI link's pads), the queue is
// emptied of what was pushed before the export (a note on the render never took does not start after it), and what was stopped is kept for the strip. Offs pushed during the export
// stay in the queue and find no note when the engine is back. Main loop, the render parked: the table and the queue's consumer end are the main loop's now (see render_offline).
void Engine::stop_live_notes() noexcept {
  if (live_ == nullptr) {
    return;
  }
  LiveNotes& notes = live_->notes;
  std::uint32_t what = notes.oldest();
  std::uint32_t count = notes.held();
  (void)notes.end_all(kLiveAllSources);
  // What the live source holds outside the table (the pads on a MIDI instrument, sent as MIDI by the link): its offs go out now.
  if (live_source_.stop_live != nullptr) {
    std::uint32_t pads = 0;
    const std::uint32_t first = live_source_.stop_live(live_source_.self, &pads);
    if (pads != 0u) {
      what = what == kHeldNone ? first : what;
      count += pads;
    }
  }
  // A note on the main loop pushed just before the request that the render never took: it is dropped (it would start when the engine is given back, and its off may have been lost), and counted
  // and, if nothing else is named, named. (A key pressed again over a held one counts twice: a word on a strip, not a state.)
  LiveEvent e;
  while (live_->queue.pop(e)) {
    if (e.kind == LiveKind::On) {
      if (what == kHeldNone) {
        what = static_cast<std::uint32_t>(e.source) | (static_cast<std::uint32_t>(e.key) << 8) | (static_cast<std::uint32_t>(e.note) << 16) | (static_cast<std::uint32_t>(e.channel) << 24);
      }
      ++count;
    }
  }
  live_->stopped_what = what;
  live_->stopped_count = static_cast<std::uint8_t>(count > 255u ? 255u : count);
  live_held_.store(0, std::memory_order_release);
}

void Engine::offline_leave() noexcept {
  if (offline_entered_) {
    offline_entered_ = false;
    offline_quiet();
    if (voices_ != nullptr) {
      voices_->perf_restore(perf_voices_);  // OS-PERFFX P0: the Perform layer is back as the live sequencer's control table still holds it (the voices are cut: the next note takes it)
    }
    graph_.perf_restore(perf_strips_);
    graph_.clear_quiet();  // OS-ODD11: the first live block renders whole
    source_ = live_source_;
    graph_.set_param_hook(source_.param, source_.self);
    clock_ = live_clock_;
    fx_tempo_ = 0;  // the live source's tempo is told to the send effects again
    graph_.set_input_feed(feed_was_);
    graph_.stems().set_enabled(stems_were_on_);
    if (graph_.app_tap() != nullptr) {
      graph_.app_tap()->set_source(static_cast<AppTap::Source>(tap_was_));
    }
    graph_.set_scope_enabled(scope_was_);
    graph_.set_spectrum_enabled(spectrum_was_);
    graph_.loop().park(false);  // (halted: it resumes on the first bar of the live transport)
    if (modules_ != nullptr) {
      modules_->set_governed(governed_was_);
    }
    if (source_.resume != nullptr) {
      source_.resume(source_.self);  // the live source (restored above) forgets what it kept from before: the MIDI link's rings and clock loop. Still parked.
    }
  }
  want_.store(false, std::memory_order_release);
  parked_.store(false, std::memory_order_relaxed);
}

void Engine::render_offline(const hal::audio::BlockIo& io) noexcept {
  if (!offline_entered_) {
    return;
  }
  // The live notes' queue is not read: play_live() returns at once with no box (the main loop owns `live_` while the render is parked, and puts it back before the engine is given away).
  LiveBox* const live = live_;
  live_ = nullptr;
  graph_.clear_quiet();  // OS-ODD11: no voice is gated in an export (the strips' state is the live render's until the export's first drain)
  if (voices_ != nullptr) {
    play_block();
  }
  live_ = live;
  graph_.render(io);
}

}  // namespace o3os::audio
