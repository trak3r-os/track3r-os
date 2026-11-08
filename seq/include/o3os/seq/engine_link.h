// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The sequencer as the audio engine's event source (OS-005d). The engine (audio layer) may not include the sequencer, so it takes an
// audio::EventSource: this header builds one over a Sequencer. The engine calls Sequencer::step() once per block on the sample clock it counts and
// plays the events; the ParamChanges its graph does not own (seq_param::Tempo, Swing) reach Sequencer::apply().
//
//     static audio::SeqEvent events[audio::kMaxEventsPerBlock];
//     engine.attach_events(seq::event_source(sequencer, events, audio::kMaxEventsPerBlock));
//
// The buffer is the caller's (2.3 KB; SRAM_U on the MK66). Before the audio starts (Engine::attach_events).

#include <o3os/audio/event_source.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>

namespace o3os::seq {

inline audio::EventSource event_source(Sequencer& sequencer, audio::SeqEvent* buffer, std::uint32_t capacity) noexcept {
  audio::EventSource source;
  source.self = &sequencer;
  source.step = [](void* self, std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept -> std::uint32_t {
    return static_cast<Sequencer*>(self)->step(block_start, n, out, cap);
  };
  source.param = [](void* self, const core::ParamChange& change) noexcept -> bool { return static_cast<Sequencer*>(self)->apply(change); };
  source.tempo = [](void* self) noexcept -> std::uint32_t { return static_cast<Sequencer*>(self)->tempo_x100(); };  // the filter LFOs' rate (OS-007m)
  source.clock = [](void* self, std::uint64_t sample) noexcept -> audio::ClockPos { return static_cast<Sequencer*>(self)->clock_at(sample); };  // OS-INSFX-ABI2 A3: get_clock
  source.buffer = buffer;
  source.capacity = capacity;
  return source;
}

}  // namespace o3os::seq
