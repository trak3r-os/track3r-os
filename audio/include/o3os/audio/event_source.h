// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Where the engine gets its events from (OS-005d). The audio layer may not include the sequencer (which reads the project model), so the engine
// is handed this plain record instead: a pointer to the object and two functions that take it back as their first argument. seq/engine_link.h
// builds one over a Sequencer; a test builds one over a script. No virtual call, no allocation; both functions run in the render context.
//
//   step    the events of the block [block_start, block_start + n), sorted by offset, into buffer[0..cap); returns how many (audio/events.h).
//           Called once per block, always, so that the sequencer's timeline never skips a block (an underrun mute included).
//   param   a ParamChange the graph does not own (the sequencer's tempo and swing: seq::seq_param), offered by the graph's drain; true when it was one.
//   tempo   the tempo in force, BPM x 100 (the sequencer's: the Tempo FX and the performance override included), read once per block after step(); optional: without it the voices
//           assume 120 BPM. It is what the filter LFO's period in samples follows (voices.h FILTER MODULATION).
//   live    (OS-INSTUI S2b) optional: offered every live event (audio/live_notes.h) the engine pops, before it acts on it, in the render context; true = the source took it and the
//           engine must not (a pad note-on of a MIDI instrument, which seq::MidiLink turns into a MIDI note: seq/midi_link.h LIVE MIDI NOTES). A false for an Off or an AllOff is
//           the usual answer: the engine goes on to end whatever voice the key started.
//   resume  (OS-WAVEXP) optional: called by Engine::offline_leave on the live source, in the main loop with the render still parked, before the live render takes the engine back. The source forgets what
//           it kept from before the export (a MIDI link: the bytes the interrupts stamped meanwhile, the clock loop's phase, the parsers' state: their stamps are of a time that is gone).
//   stop_live (OS-EXPNOTE) optional: called by Engine::offline_enter on the live source, in the main loop with the render parked: the notes the source holds outside the engine's table (a MIDI link: the
//           pads on a MIDI instrument, taken by `live`) are ended NOW (their note-offs sent), `*count` says how many, and the return is the first as a held-note word (live_notes.h; kHeldNone: none).
//   clock   (OS-INSFX-ABI2 A3) optional: the playhead as of `sample`, the first sample of the NEXT block, asked once a block at the block boundary (Engine::block_end, in flash; an export block asks
//           at its start): the tempo that plays, the beat (Q16.16, counted from step 0 of the pattern that plays) and the flags. It never runs inside play_block. Without it get_clock says "no sequencer".
//   buffer  the events' memory (kMaxEventsPerBlock events: 2.3 KB), owned by the caller (it is SRAM_U on the MK66), so that the Engine itself stays small.

#include <o3os/audio/events.h>
#include <o3os/audio/live_notes.h>
#include <o3os/core/param.h>

#include <cstdint>

namespace o3os::audio {

// What `clock` fills (OS-INSFX-ABI2 A3). `flags` are the module ABI's o3m_clock flags (O3M_CLOCK_PLAYING 1, STARTED 2, WRAPPED 4); `beat_q16` is the playhead in beats (4 steps) in Q16.16; `tempo_x100` is BPM x 100.
struct ClockPos {
  std::uint32_t flags;
  std::uint32_t beat_q16;
  std::uint32_t tempo_x100;
};

struct EventSource {
  using StepFn = std::uint32_t (*)(void* self, std::uint64_t block_start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept;
  using ParamFn = bool (*)(void* self, const core::ParamChange& change) noexcept;
  using TempoFn = std::uint32_t (*)(void* self) noexcept;
  using LiveFn = bool (*)(void* self, const LiveEvent& e) noexcept;
  using ResumeFn = void (*)(void* self) noexcept;
  using StopLiveFn = std::uint32_t (*)(void* self, std::uint32_t* count) noexcept;
  using ClockFn = ClockPos (*)(void* self, std::uint64_t sample) noexcept;

  void* self = nullptr;
  StepFn step = nullptr;
  ParamFn param = nullptr;
  TempoFn tempo = nullptr;
  LiveFn live = nullptr;
  ResumeFn resume = nullptr;
  StopLiveFn stop_live = nullptr;
  ClockFn clock = nullptr;
  SeqEvent* buffer = nullptr;
  std::uint32_t capacity = 0;
};

}  // namespace o3os::audio
