// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The live note path (OS-005g): pads, a MIDI keyboard and the previews play the voices the way the sequencer does, from the main loop.
//
// QUEUE CONTRACT (the one thing a reviewer needs to read).
//   LiveQueue is a core::SpscRing of LiveEvent (8 bytes, 64 of them: 520 bytes; with the table of held notes it is a LiveBox, which the platform places in SDRAM beside the
//   control-change ring: SRAM_U is full).
//     producer  the main loop, ONE context (App's actions and App::poll_midi_cc through the platform's PreviewSink): push() only, never blocks, a full ring drops the
//               event and the producer counts it. 64 events are far more than one pass of the main loop can make (a pad is one On and one Off, the MIDI poll takes
//               16 messages a pass) and the render takes kLivePerBlock a block, so a full ring is a stalled render, not a busy hand: the platform does not push at all
//               while the audio is not running, and a lost Off is repaired by the AllOff that App::reset_input() sends.
//     consumer  the render, ONE context (Engine::play_live, once a block, FIRST: before the sequencer's events of the block): pop() at most kLivePerBlock events (the rest
//               waits for the next block), each acted on at sample 0 of the block, in the order they were pushed. The merge is therefore the same as an event of the
//               sequencer at offset 0 that comes before the sequencer's own: a note the sequencer starts at offset 0 on the same track wins, and a live note
//               reaches the voice one block (64 samples, 1.5 ms) later at worst.
//               OS-INSTUI S2b: before acting, play_live offers each event to EventSource::live (seq::MidiLink::live_event, same context): a PAD note-on of a MIDI instrument is
//               taken there and becomes a MIDI note on the instrument's channel (no voice, no track), and a pad's Off / an AllOff ends it from the channel and note stored at the
//               note-on (seq/midi_link.h LIVE MIDI NOTES). The producer needs no knowledge of the kind: one queue, one consumer.
//     No allocation, no blocking, no wait: a pop and a handful of integer compares an event. The state a Note Off needs (which track a key started on) lives in
//     LiveNotes, which only the render touches; the ring carries values, never pointers.
//
// WHICH VOICE (NEW COMMUNITY OS BEHAVIOUR; how the official firmware assigns voices to live notes is an open research question, Q). A live note takes a track of the voice pool
// (kVoiceTracks, one voice a track, so the track's strip, with its level, pan, sends and mute, is its strip) by pick_track(): searching from the selected track, a track that no live note
// holds and no voice sounds on; failing that the first one no live note holds (the sequencer's voice there is taken over); a live note past kLiveMax cuts the oldest. Because the tracks and
// the voices are shared, a sequencer note that lands on a track a live note holds takes that voice and the live Note Off then releases it; the search above avoids it unless every track sounds.
//
// SLICES. A pad of the Chop page plays slice n as a note: a beat slice instrument plays slice (note - kSliceRootNote) (voices.h BEAT SLICE), so the app asks for
// note kSliceRootNote + n. There is no separate slice field: the pool's forced slice is the sequencer's per-track FX state (ModSet / ModEnd) and a live note must not
// set it, or clear it, under a pattern that uses it.
//
// Real time: render context, bounded (kLivePerBlock events, pick_track is 16 steps, kLiveMax slots), no heap, no division.

#include <o3os/core/ring.h>

#include <cstdint>
#include <type_traits>

namespace o3os::audio {

enum class LiveKind : std::uint8_t {
  None = 0,
  On = 1,      // note on: `note` 0..127 of `instrument` 1..64, `velocity` 1..127 or kLiveVelocityDefault, searching for a track from `track`
  Off = 2,     // the note that `source`/`key` started enters its release; no such note: nothing happens
  AllOff = 3,  // every note of `source` (kLiveAllSources: of every source) enters its release
};

// Who started a note: the key a Note Off names is the pair (source, key), so a pad and a keyboard key never end each other's note.
inline constexpr std::uint8_t kLiveSourcePad = 0;      // key = the pad 0..47
inline constexpr std::uint8_t kLiveSourceMidi = 1;     // key = the MIDI note number (any port, any channel)
inline constexpr std::uint8_t kLiveSourcePreview = 2;  // key = 0: the preview voice, one at a time
inline constexpr std::uint8_t kLiveSourceApp = 3;      // key = the running app's note slot 0..3 (OS-APPUX S3: an app's play_note, ABI 1.10; at most four held, all ended when the app's page closes)
inline constexpr std::uint8_t kLiveAllSources = 0xFF;  // AllOff.source
inline constexpr std::uint8_t kLiveVelocityDefault = 0xFF;  // the instrument's own level (the same as audio::kVelocityDefault)

struct LiveEvent {
  LiveKind kind;
  std::uint8_t source;
  std::uint8_t key;
  std::uint8_t note;
  std::uint8_t instrument;
  std::uint8_t velocity;
  std::uint8_t track;  // On: the track the search starts from, 0..15 (the selected one)
  std::uint8_t channel;  // On of kLiveSourceMidi: the MIDI channel 1..16 it came on, only named in a message (the export's refusal). On of kLiveSourcePad: slice + 1 (0 none), the pad's own slice of a Slice mode instrument (OS-BUG-SLICEPAD, Engine::play_live). 0 otherwise
};
static_assert(sizeof(LiveEvent) == 8, "LiveEvent is two words");
static_assert(std::is_trivially_copyable<LiveEvent>::value, "LiveEvent is copied bytewise through the ring");

inline constexpr std::uint32_t kLiveQueueSize = 64;  // power of two
using LiveQueue = core::SpscRing<LiveEvent, kLiveQueueSize>;

inline constexpr unsigned kLiveMax = 12;        // notes held at once: a pattern's tracks (NEW COMMUNITY OS BEHAVIOUR, maintainer 2026-10-13: a 12-note chord from a MIDI keyboard), not the voice pool's size (kMaxVoices, 12): a held note takes one track, never more than a voice
inline constexpr unsigned kLivePerBlock = 16;   // events the render takes a block
inline constexpr unsigned kLiveTracks = 16;     // the graph's tracks (audio_model_ids_test holds it equal to kVoiceTracks)
inline constexpr std::uint8_t kLiveNone = 0xFF;

// The track a new live note plays on: from `base` (wrapping over the 16 tracks) the first with neither a live note (`live`, a bit a track) nor a sounding voice
// (`playing`); else the first with no live note; else `base`. A pure function of its arguments. `base` past the last track is taken as track 0.
constexpr unsigned pick_track(unsigned base, std::uint32_t live, std::uint32_t playing) noexcept {
  const unsigned b = base < kLiveTracks ? base : 0u;
  for (unsigned i = 0; i < kLiveTracks; ++i) {
    const unsigned t = (b + i) % kLiveTracks;
    if (((live | playing) & (1u << t)) == 0u) {
      return t;
    }
  }
  for (unsigned i = 0; i < kLiveTracks; ++i) {
    const unsigned t = (b + i) % kLiveTracks;
    if ((live & (1u << t)) == 0u) {
      return t;
    }
  }
  return b;
}

// One held note as a word, for the message that names it (OS-EXPNOTE): bits 0..7 source, 8..15 key, 16..23 the note it plays (the pad's: the key is the pad), 24..31 the MIDI channel 1..16
// (0: none). kHeldNone: no note is held.
inline constexpr std::uint32_t kHeldNone = 0xFFFFFFFFu;
constexpr std::uint8_t held_source(std::uint32_t w) noexcept { return static_cast<std::uint8_t>(w); }
constexpr std::uint8_t held_key(std::uint32_t w) noexcept { return static_cast<std::uint8_t>(w >> 8); }
constexpr std::uint8_t held_note(std::uint32_t w) noexcept { return static_cast<std::uint8_t>(w >> 16); }
constexpr std::uint8_t held_channel(std::uint32_t w) noexcept { return static_cast<std::uint8_t>(w >> 24); }

// What the render knows about the notes that are held: kLiveMax slots of (source, key) -> track. Render context only. 74 bytes (in a LiveBox: SDRAM).
class LiveNotes {
 public:
  struct Start {
    std::uint8_t track;  // where the note plays
    std::uint8_t cut;    // kLiveNone, or the track of the oldest live note that gave up its slot (cut it before starting this one)
  };

  // A note on: the track it plays on, and the note it displaced if the slots were full. A key that is already held retriggers on its own track.
  // `note` and `channel` are only remembered, for oldest().
  constexpr Start begin(std::uint8_t source, std::uint8_t key, unsigned base, std::uint32_t playing, std::uint8_t note = 0, std::uint8_t channel = 0) noexcept {
    const std::uint16_t id = make_id(source, key);
    for (Slot& s : slots_) {
      if (s.id == id) {
        s.age = ++clock_;
        s.note = note;
        s.channel = channel;
        return Start{s.track, kLiveNone};
      }
    }
    Slot* free_slot = nullptr;
    Slot* oldest = nullptr;
    std::uint32_t live = 0;
    for (Slot& s : slots_) {
      if (s.id == kFree) {
        free_slot = free_slot != nullptr ? free_slot : &s;
      } else {
        live |= 1u << s.track;
        if (oldest == nullptr || static_cast<std::uint8_t>(clock_ - s.age) > static_cast<std::uint8_t>(clock_ - oldest->age)) {
          oldest = &s;
        }
      }
    }
    std::uint8_t cut = kLiveNone;
    if (free_slot == nullptr) {  // every slot holds a note: the oldest is cut and its slot is this one's
      cut = oldest->track;
      live &= ~(1u << cut);
      free_slot = oldest;
    }
    const unsigned t = pick_track(base, live, playing);
    free_slot->id = id;
    free_slot->track = static_cast<std::uint8_t>(t);
    free_slot->age = ++clock_;
    free_slot->note = note;
    free_slot->channel = channel;
    return Start{static_cast<std::uint8_t>(t), cut};
  }

  // A note off: the track the key's note is on (the slot is freed), kLiveNone when it holds none.
  constexpr std::uint8_t end(std::uint8_t source, std::uint8_t key) noexcept {
    const std::uint16_t id = make_id(source, key);
    for (Slot& s : slots_) {
      if (s.id == id) {
        s.id = kFree;
        return s.track;
      }
    }
    return kLiveNone;
  }

  // An all-notes-off: the tracks of every held note of `source` (kLiveAllSources: of all), as a bit a track; the slots are freed.
  constexpr std::uint32_t end_all(std::uint8_t source) noexcept {
    std::uint32_t tracks = 0;
    for (Slot& s : slots_) {
      if (s.id != kFree && (source == kLiveAllSources || (s.id >> 8) == source)) {
        tracks |= 1u << s.track;
        s.id = kFree;
      }
    }
    return tracks;
  }

  constexpr unsigned held() const noexcept {
    unsigned n = 0;
    for (const Slot& s : slots_) {
      n += s.id != kFree ? 1u : 0u;
    }
    return n;
  }

  // The oldest held note (the one that has been held longest: the likeliest to be a lost release) as a word (see kHeldNone), kHeldNone when none is held.
  constexpr std::uint32_t oldest() const noexcept {
    const Slot* o = nullptr;
    for (const Slot& s : slots_) {
      if (s.id != kFree && (o == nullptr || static_cast<std::uint8_t>(clock_ - s.age) > static_cast<std::uint8_t>(clock_ - o->age))) {
        o = &s;
      }
    }
    return o == nullptr ? kHeldNone : (static_cast<std::uint32_t>(o->id >> 8) | (static_cast<std::uint32_t>(o->id & 0xFFu) << 8) | (static_cast<std::uint32_t>(o->note) << 16) | (static_cast<std::uint32_t>(o->channel) << 24));
  }

 private:
  static constexpr std::uint16_t kFree = 0xFFFF;
  struct Slot {
    std::uint16_t id = kFree;  // source << 8 | key
    std::uint8_t track = 0;
    std::uint8_t note = 0;     // the note it plays (for a pad: the key is the pad)
    std::uint8_t channel = 0;  // MIDI: the channel 1..16 of the note on, else 0
    std::uint8_t age = 0;      // clock_ at the start; the oldest has the greatest (clock_ - age) as a byte, right while fewer than 256 notes start under a held one
  };
  // A source of 0xFF names no source (it is AllOff's wildcard) and would make kFree a valid id: it is folded into 0xFE.
  static constexpr std::uint16_t make_id(std::uint8_t source, std::uint8_t key) noexcept {
    return static_cast<std::uint16_t>(((source == 0xFFu ? 0xFEu : static_cast<unsigned>(source)) << 8) | key);
  }
  Slot slots_[kLiveMax];
  std::uint8_t clock_ = 0;
};

// The queue and the render's table of held notes in one block of memory, so that the platform places both in SDRAM behind one pointer (the Engine keeps just that pointer: SRAM_U
// has no room for the table, 74 bytes, and the queue, 520). The render reads and writes `notes` (a few words an event, not a word a sample: SDRAM's wait states do not matter);
// the main loop touches only `queue.push` (and, with the render parked for an export, the table: Engine::offline_enter).
struct LiveBox {
  LiveQueue queue;
  LiveNotes notes;
  // MAIN LOOP ONLY (the render never reads these two): the one note that arrived before the outputs were open (OS-FU1). The platform keeps the latest note on here
  // while it waits, delivers it to the queue when the outputs open, and, if the key was released meanwhile, the Off after it. Kind None: nothing waits.
  LiveEvent pending{};
  bool pending_off = false;
  // MAIN LOOP ONLY, written by Engine::offline_enter while the render is parked (OS-EXPNOTE): what an export stopped at its start. `stopped_what` is the oldest held note as a word (see
  // kHeldNone), `stopped_count` how many were held. Read by seq::OfflineRender to tell the strip.
  std::uint32_t stopped_what = kHeldNone;
  std::uint8_t stopped_count = 0;
};

}  // namespace o3os::audio
