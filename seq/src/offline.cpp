// SPDX-License-Identifier: GPL-3.0-or-later
//
// The offline render of a song or a pattern (seq/offline.h).
#include <o3os/seq/offline.h>

#include <o3os/audio/graph_params.h>

namespace o3os::seq {

namespace {

constexpr std::uint32_t clamp_u32(std::uint32_t v, std::uint32_t lo, std::uint32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// The tracker's name of a note (0 = C-0, so MIDI note 60 is C-5; the note tile's three characters, ui::note_tile_text): "C-4", "D#3", and in the tenth octave "C10" with a lowercase sharp ("c10").
// Appends to `out` (room for 4 characters) and returns the new end.
char* append_note(char* out, unsigned note) noexcept {
  static const char kNames[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
  const unsigned semitone = note % 12u;
  const unsigned octave = note / 12u;
  const bool sharp = kNames[semitone * 2u + 1u] == '#';
  if (octave >= 10u) {
    *out++ = sharp ? static_cast<char>(kNames[semitone * 2u] + ('a' - 'A')) : kNames[semitone * 2u];
    *out++ = '1';
    *out++ = static_cast<char>('0' + octave - 10u);
    return out;
  }
  *out++ = kNames[semitone * 2u];
  *out++ = kNames[semitone * 2u + 1u];
  *out++ = static_cast<char>('0' + octave);
  return out;
}

char* append_text(char* out, const char* text) noexcept {
  while (*text != '\0') {
    *out++ = *text++;
  }
  return out;
}

}  // namespace

// The steps of a pattern as the sequencer plays it (Sequencer::song_pattern_len: the longest active track; a pattern that does not exist plays as an empty one of the default length).
std::uint32_t OfflineRender::steps_of(std::uint32_t pattern_id) const noexcept {
  std::uint32_t len = w_.project->pattern_length(pattern_id);
  if (len == 0u) {
    len = w_.project->default_pattern_length();
  }
  return clamp_u32(len, model::kMinPatternLength, model::kMaxPatternLength);
}

// The last order position a Song request plays (OS-SONGSHARE S5): its order_last, or the song's last one when that is past the list or the request leaves it alone.
std::uint32_t OfflineRender::last_slot() const noexcept {
  const std::uint32_t length = w_.project->song_length();
  if (length == 0u) {
    return 0u;  // (open() refuses an empty song; the helper still must not wrap)
  }
  const std::uint32_t last = length - 1u;
  return request_.order_last < last ? request_.order_last : last;
}

// Asks the effects to forget what they hold (the caller's ParamChange ring: the main loop is its producer, the engine's drain its consumer, which is whoever renders the next block) and the
// instrument sends of the last notes to go back to nothing. A ring that is full loses the request (the effect then keeps its tail into the settle: the pass starts a little less quiet).
void OfflineRender::push_clears() noexcept {
  namespace gp = audio::graph_param;
  audio::ParamRing& ring = w_.engine->params();
  // LATENT (OS-WAVEXP section 8, recorded, not fixed): these three pushes drop the clear without a word when the ring is full, and the pass then starts with the effect's tail, so the file is not the
  // one of a pass from rest. The ring has 256 slots and the engine drains it every block, so it is not reachable today; if it ever is, check the room first (as push_mixer does) and fail the pass.
  (void)ring.push(audio::ParamChange{audio::master_param(gp::master::DelayClear), 1.0f, 0u});
  (void)ring.push(audio::ParamChange{audio::master_param(gp::master::ReverbClear), 1.0f, 0u});
  (void)ring.push(audio::ParamChange{audio::master_param(gp::master::VoiceSendsReset), 1.0f, 0u});
}

// The mixer of a masked request (audio::OfflineRequest: a stem, a selection) or, with `stem` false, the project's own: the 16 tracks' MUTE and SOLO and the master's DRY, DELAY and REVERB mutes, one
// ParamChange each, instant (ramp 1: the render settles for a long time before the transport starts, and a mixer that moved during a note would be heard). The project is where the mixer's values
// are kept (App: project_state.cpp tracks_apply pushes the same ones), so these are what the live graph has too. False: the ring has no room for all 35 changes (of its 256, drained every block: it has, unless something else filled it) and NOTHING was pushed.
bool OfflineRender::push_mixer(bool stem) noexcept {
  namespace gp = audio::graph_param;
  audio::ParamRing& ring = w_.engine->params();
  constexpr unsigned kChanges = 2u * audio::kTracks + 3u;
  if (ring.capacity() - ring.size() < kChanges) {
    return false;  // all or nothing: a mixer half told is worse than one not told (size() may count what the render has just taken: it can only say "full" too soon)
  }
  const audio::OfflineRequest& rq = request_;
  const bool tracks = stem && rq.track_mask != audio::OfflineRequest::kAllTracks;
  bool ok = true;
  for (unsigned t = 0; t < audio::kTracks; ++t) {
    const model::TrackSlot* slot = w_.project->track(t);
    const std::uint8_t flags = slot != nullptr ? slot->flags : std::uint8_t{0};
    bool mute = (flags & model::kTrackMute) != 0u;
    bool solo = (flags & model::kTrackSolo) != 0u;
    if (tracks) {  // the masked-in tracks sound, whatever the project's mixer says; the others are muted; no solo (a solo would silence the masked-in ones)
      mute = ((rq.track_mask >> t) & 1u) == 0u;
      solo = false;
    }
    ok = ring.push(audio::ParamChange{audio::strip_param(t, gp::strip::Mute), mute ? 1.0f : 0.0f, 1u}) && ok;
    ok = ring.push(audio::ParamChange{audio::strip_param(t, gp::strip::Solo), solo ? 1.0f : 0.0f, 1u}) && ok;
  }
  const std::uint8_t mf = w_.project->master().flags;
  bool dry_mute = (mf & model::kMasterDryMute) != 0u;
  bool delay_mute = (mf & model::kMasterDelayMute) != 0u;
  bool reverb_mute = (mf & model::kMasterReverbMute) != 0u;
  if (stem) {
    if (tracks) {
      dry_mute = false;  // a track stem or a selection is the dry sound of the tracks: the project's DRY MUTE (a bus-only mix) would make it silent
    }
    dry_mute = dry_mute || rq.bus_only;
    delay_mute = delay_mute || rq.dry_sends || (rq.buses & 1u) == 0u;
    reverb_mute = reverb_mute || rq.dry_sends || (rq.buses & 2u) == 0u;
  }
  ok = ring.push(audio::ParamChange{audio::master_param(gp::master::DryMute), dry_mute ? 1.0f : 0.0f, 1u}) && ok;
  ok = ring.push(audio::ParamChange{audio::master_param(gp::master::DelayMute), delay_mute ? 1.0f : 0.0f, 1u}) && ok;
  ok = ring.push(audio::ParamChange{audio::master_param(gp::master::ReverbMute), reverb_mute ? 1.0f : 0.0f, 1u}) && ok;
  return ok;
}

// A Sequencer of the export's own, built in place over the same project: a fresh one for every pass, so that nothing of the last pass (its resume point, a queued pattern, a swing in force)
// is in the next. The live sequencer's Config is the base (the seed, the Tick events of an Amiga instrument).
void OfflineRender::build_sequencer() noexcept {
  seq_ = w_.make_sequencer(w_.make_ctx);
  Sequencer::Config c = w_.live != nullptr ? w_.live->config() : Sequencer::Config{};
  c.song_loop = false;      // the song stops at the end of its last pattern
  c.pattern_once = true;    // a pattern is played once
  c.emit_clock = false;     // no MIDI: the export's sequencer has no link, and no clock to send
  c.start_lead = 0;
  c.pattern_stop = 0;
  c.order_stop = 0;
  if (request_.what == audio::OfflineRequest::What::Song && last_slot() + 1u < w_.project->song_length()) {
    c.order_stop = static_cast<std::uint16_t>(last_slot() + 1u);  // a range of slots: the transport stops at the end of the last one (the whole song ends with its list: no stop)
  }
  if (request_.what == audio::OfflineRequest::What::Pattern && request_.row_last != audio::OfflineRequest::kToEnd && request_.row_last + 1u < steps_of(request_.pattern)) {
    c.pattern_stop = static_cast<std::uint16_t>(request_.row_last + 1u);  // a selection: the transport stops at the end of its last row
  }
  seq_->configure(c);
  source_ = audio::EventSource{};
  source_.self = this;
  source_.step = &OfflineRender::step_events;
  source_.param = &OfflineRender::param_event;
  source_.tempo = &OfflineRender::tempo_event;
  source_.buffer = w_.events;
  source_.capacity = w_.event_cap;
}

std::uint32_t OfflineRender::step_events(void* self, std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept {
  OfflineRender& r = *static_cast<OfflineRender*>(self);
  const std::uint32_t count = r.seq_->step(block_start, n, out, cap);
  for (std::uint32_t i = 0; i < count && i < cap; ++i) {
    r.stopped_ = r.stopped_ || out[i].kind == audio::EventKind::Stopped;  // (the sequencer stops inside this block: its time is out[i].offset)
  }
  return count;
}

bool OfflineRender::param_event(void* self, const core::ParamChange& change) noexcept { return static_cast<OfflineRender*>(self)->seq_->apply(change); }

std::uint32_t OfflineRender::tempo_event(void* self) noexcept { return static_cast<OfflineRender*>(self)->seq_->tempo_x100(); }

void OfflineRender::start_pass() noexcept {
  settle_left_ = kSettleBlocks;
  blocks_ = 0;
  run_blocks_ = 0;
  saw_playing_ = false;
  stopped_ = false;
  tail_ = false;
  best_ = 0;
  last_order_ = 0xFFFFFFFFu;
  prefix_ = 0;
  if (request_.what == audio::OfflineRequest::What::Song) {
    total_steps_ = 0;
    for (std::uint32_t pos = request_.order_first, last = last_slot(); pos <= last; ++pos) {  // (the whole song, or the range of slots)
      total_steps_ += steps_of(w_.project->order(pos));
    }
  } else {
    const std::uint32_t steps = steps_of(request_.pattern);
    const std::uint32_t last = request_.row_last < steps ? request_.row_last : steps - 1u;
    total_steps_ = last + 1u - (request_.row_first < last ? request_.row_first : last);  // the rows played (a selection: from row_first to row_last)
  }
  if (total_steps_ == 0u) {
    total_steps_ = 1u;
  }
  w_.engine->offline_quiet();
  push_clears();
  if (request_.masked()) {
    mixer_set_ = true;  // (even a half-set mixer is put back by close())
    if (!push_mixer(true)) {
      failure_ = "THE MIXER IS BUSY";
      state_ = State::Failed;
      return;
    }
  } else if (mixer_set_) {  // a pass after a masked one (the engine stayed with the export) that is the plain mix: the project's mixer
    mixer_set_ = !push_mixer(false);
  }
  state_ = State::Settling;
}

// OS-EXPNOTE: what the engine stopped when the export took it (Engine::offline_enter ends the held live notes), as one strip line: "STOPPED PAD C-4", "STOPPED MIDI CH10 C-3", and with more than
// one held "... +2" (the oldest is named, then how many others). "" when nothing was held. No allocation: stopped_text_ is the buffer.
void OfflineRender::note_stopped() noexcept {
  const audio::Engine& e = *w_.engine;
  const std::uint32_t w = e.live_stopped_what();
  const unsigned count = e.live_stopped_count();
  stopped_text_[0] = '\0';
  if (w == audio::kHeldNone || count == 0u) {
    return;
  }
  char* p = append_text(stopped_text_, "STOPPED ");
  const unsigned note = audio::held_note(w);
  const unsigned ch = audio::held_channel(w);
  if (audio::held_source(w) == audio::kLiveSourcePad && note <= 127u) {
    p = append_text(p, "PAD ");
    p = append_note(p, note);
  } else if (audio::held_source(w) == audio::kLiveSourceMidi && audio::held_key(w) <= 127u) {
    p = append_text(p, "MIDI ");
    if (ch >= 1u && ch <= 16u) {
      p = append_text(p, "CH");
      if (ch >= 10u) {
        *p++ = '1';
      }
      *p++ = static_cast<char>('0' + ch % 10u);
      *p++ = ' ';
    }
    p = append_note(p, audio::held_key(w));  // (a MIDI note's key is its note)
  } else {
    p = append_text(p, "A NOTE");  // a preview
  }
  if (count > 1u) {
    *p++ = ' ';
    *p++ = '+';
    if (count - 1u >= 10u) {
      *p++ = static_cast<char>('0' + (count - 1u) / 10u);
    }
    *p++ = static_cast<char>('0' + (count - 1u) % 10u);
  }
  *p = '\0';
}

bool OfflineRender::open(const audio::OfflineRequest& request) noexcept {
  failure_ = "";
  if (w_.engine == nullptr || w_.project == nullptr || w_.events == nullptr || w_.event_cap == 0u || w_.make_sequencer == nullptr) {
    return refuse("EXPORT IS NOT AVAILABLE");
  }
  if (request.what == audio::OfflineRequest::What::Song && w_.project->song_length() == 0u) {
    return refuse("THE SONG IS EMPTY");
  }
  if (request.what == audio::OfflineRequest::What::Song && (request.order_first > request.order_last || request.order_first >= w_.project->song_length())) {
    return refuse("NO SUCH SLOTS");  // (a range starts in the list, and its last slot is not before its first)
  }
  if (request.what == audio::OfflineRequest::What::Pattern && (request.pattern < 1u || request.pattern > 255u)) {
    return refuse("NO SUCH PATTERN");
  }
  if (request.what == audio::OfflineRequest::What::Pattern && (request.row_first > request.row_last || request.row_first >= steps_of(request.pattern))) {
    return refuse("NO SUCH ROWS");  // (a selection starts in the pattern, and its last row is not before its first)
  }
  audio::Engine& e = *w_.engine;
  const bool again = e.offline_entered();  // a second pass over the same request: the engine stays with the export
  if (!again) {
    if (state_ == State::Requested) {
      return refuse("AN EXPORT IS ALREADY STARTING");
    }
    if (w_.live != nullptr && w_.live->play_position().read().playing) {
      return refuse("STOP THE TRANSPORT FIRST");
    }
    stopped_text_[0] = '\0';  // (a note that is held now is stopped when the export takes the engine: Engine::offline_enter, note_stopped())
    if (e.graph().rec().state() != audio::RecTap::State::Idle) {
      return refuse("THE RECORDER IS ARMED");
    }
  }
  request_ = request;
  if (again) {
    build_sequencer();
    start_pass();
    return state_ != State::Failed;  // (a mixer that could not be set: failure() says so)
  }
  build_sequencer();  // (Engine::offline_enter copies its source when the live render has stood down)
  e.offline_request();
  state_ = State::Requested;
  return true;
}

void OfflineRender::render(std::int16_t* out) noexcept {
  report_ = hal::audio::BlockReport{};
  const hal::audio::BlockIo io{silent_in_, out, 0u, &report_};
  w_.engine->render_offline(io);
}

void OfflineRender::update_progress() noexcept {
  const audio::PlayPosition::Value v = seq_->play_position().read();
  std::uint32_t done = v.step;
  if (request_.what == audio::OfflineRequest::What::Pattern) {
    done = v.step > request_.row_first ? v.step - request_.row_first : 0u;  // a selection starts at row_first
  }
  if (request_.what == audio::OfflineRequest::What::Song && v.order != audio::PlayPosition::kNoOrder) {
    if (v.order != last_order_) {  // a new pattern of the song (or a jump back): the steps before it, once
      last_order_ = v.order;
      prefix_ = 0;
      for (std::uint32_t pos = request_.order_first; pos < v.order; ++pos) {  // (a range counts from its first slot)
        prefix_ += steps_of(w_.project->order(pos));
      }
    }
    done += prefix_;
  }
  const unsigned p = static_cast<unsigned>(done >= total_steps_ ? 998u : done * 1000u / total_steps_);
  best_ = p > best_ ? (p > 998u ? 998u : p) : best_;
}

audio::OfflineRenderer::Step OfflineRender::next(std::int16_t* out) noexcept {
  switch (state_) {
    case State::Closed:
    case State::Failed:
      return Step::Failed;
    case State::Requested: {
      const bool live = w_.live_running != nullptr && w_.live_running();
      if (!w_.engine->offline_ready(live)) {
        return Step::Wait;  // the live render has not seen the request yet: its next block start
      }
      if (!w_.engine->offline_enter(source_)) {
        failure_ = "THE INSERTS DID NOT SETTLE";  // OS-INSFX: `ins` counts it (export unsettled); close() gives the engine back
        state_ = State::Failed;
        return Step::Failed;
      }
      note_stopped();
      start_pass();
      return Step::Wait;
    }
    case State::Settling:
      render(out);  // dropped
      if (--settle_left_ == 0u) {
        if (request_.what == audio::OfflineRequest::What::Song) {
          seq_->play_song(request_.order_first, 0);
        } else {
          seq_->play_pattern(request_.pattern, request_.row_first);
        }
        state_ = State::Running;
      }
      return Step::Wait;
    case State::Running:
      break;
  }
  render(out);
  ++blocks_;
  ++run_blocks_;
  if (stopped_) {
    tail_ = true;  // the Stopped event was in this block: the arrangement ended inside it (the sequencer itself stopped a block or two earlier: it runs ahead of the sample clock)
  } else if (seq_->playing()) {
    saw_playing_ = true;
  } else if (!saw_playing_ && run_blocks_ >= kStartBlocks) {
    failure_ = "THE TRANSPORT DID NOT START";
    state_ = State::Failed;
    return Step::Failed;
  }
  if (!tail_) {
    update_progress();
  }
  return Step::Block;
}

unsigned OfflineRender::progress() const noexcept { return tail_ ? 999u : best_; }

void OfflineRender::close() noexcept {
  if (w_.engine == nullptr) {
    return;
  }
  if (w_.engine->offline_entered()) {
    push_clears();  // the effects start the live render's next block from quiet, whatever the last pass left in them
    if (mixer_set_ && push_mixer(false)) {  // the project's mutes, solos and master mutes are the graph's again before the live render's first block
      mixer_set_ = false;
    }  // (else they are still owed: service() pushes them, once per pass, when the ring has room)
  }
  w_.engine->offline_leave();
  state_ = State::Closed;
}

void OfflineRender::service() noexcept {
  if (mixer_set_ && state_ == State::Closed && w_.engine != nullptr && !w_.engine->offline_entered() && push_mixer(false)) {
    mixer_set_ = false;
  }
}

}  // namespace o3os::seq
