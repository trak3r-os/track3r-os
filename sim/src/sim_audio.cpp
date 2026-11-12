// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/sim_audio.h>

#include <o3os/app/og_samples.h>
#include <o3os/hal/audio.h>
#include <o3os/seq/engine_link.h>
#include <o3os/usb/stream.h>

#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace o3os::sim {

namespace {

constexpr std::uint32_t kBlk = hal::audio::kBlock;
constexpr std::uint64_t kBlockMicroFrames = static_cast<std::uint64_t>(kBlk) * 1000000u;  // a block's frames, in millionths

// The engine's cycle counter is a plain function (audio::CycleFn): a count that moves a fixed step every call, reset by each rig, so that the stage reports (the only thing that reads it here) are a
// function of the script. One rig at a time.
std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return g_cycles += 11u; }

// ---- the test bank ---------------------------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kSineFrames = 44100;  // 441 periods of 100 samples
constexpr std::uint32_t kNoiseFrames = 44100;
constexpr std::uint32_t kPluckFrames = 22050;
constexpr std::uint32_t kBankFrames = kSineFrames + kNoiseFrames + kPluckFrames;

// The first quarter of a period of 100 samples, round(16384 sin(2 pi k / 100)), k = 0..25: integers all the way, so that the bank is the same bytes on every machine (no libm in it).
constexpr std::int16_t kQuarter[26] = {0,    1029, 2053,  3070,  4075,  5063,  6031,  6976,  7893,  8779,  9630,  10444, 11216,
                                       11943, 12624, 13255, 13833, 14357, 14825, 15233, 15582, 15869, 16094, 16255, 16352, 16384};

std::int32_t sine100(std::uint32_t n) {
  const std::uint32_t k = n % 100u;
  if (k <= 25u) return kQuarter[k];
  if (k <= 50u) return kQuarter[50u - k];
  if (k <= 75u) return -kQuarter[k - 50u];
  return -kQuarter[100u - k];
}

// OS-RATEPITCH: samples at the engine's rate play at their own pitch at the root note: 100 samples a period is 441.18 Hz at 44,117.647 Hz.
void make_bank(std::int16_t* out) {
  for (std::uint32_t i = 0; i < kSineFrames; ++i) {
    out[i] = static_cast<std::int16_t>(sine100(i));
  }
  std::uint32_t s = 0x1F2E3D4Cu;
  for (std::uint32_t i = 0; i < kNoiseFrames; ++i) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    out[kSineFrames + i] = static_cast<std::int16_t>(static_cast<std::int32_t>(s) >> 17);  // +-16384
  }
  // The pluck: the sine's period under an amplitude 0.8 that falls by e every 0.12 s (per sample x 65524 / 65536 = e^(-1 / 5294), Q16): integer arithmetic.
  std::int64_t amp = 52429;  // 0.8 in Q16
  for (std::uint32_t i = 0; i < kPluckFrames; ++i) {
    out[kSineFrames + kNoiseFrames + i] = static_cast<std::int16_t>((static_cast<std::int64_t>(sine100(i)) * amp) >> 16);
    amp = (amp * 65524) >> 16;
  }
}

}  // namespace

// ---- the app's sinks ---------------------------------------------------------------------------------------------------------------------------

// The Play button's transport: a command on the sequencer's ring, taken at the next block (platform/mk66/src/app_main.cpp SeqTransport, without the outputs that open late).
class SimAudio::Transport final : public app::TransportSink {
 public:
  Transport(seq::CommandRing& ring, const seq::Sequencer& sequencer) : sequencer_(sequencer), ring_(ring) {}
  bool play(unsigned pattern) noexcept override { return push(seq::Command::Kind::PlayPattern, pattern); }
  bool play_count_in(unsigned pattern, unsigned bars) noexcept override { return push(seq::Command::Kind::PlayPattern, pattern, static_cast<std::uint16_t>(bars > 2u ? 2u : bars)); }
  bool play_from(unsigned pattern, unsigned row) noexcept override { return push(seq::Command::Kind::PlayPattern, pattern, 0, row); }
  bool locate_row(unsigned row) noexcept override { return push(seq::Command::Kind::LocateRow, 0, 0, row); }
  bool play_song(unsigned row) noexcept override { return push(seq::Command::Kind::PlaySong, 0, static_cast<std::uint16_t>(row < 0xFFFFu ? row : 0xFFFFu)); }
  bool stop() noexcept override { return push(seq::Command::Kind::Stop, 0); }
  bool queue_jump(unsigned row) noexcept override { return push(seq::Command::Kind::QueueJump, 0, static_cast<std::uint16_t>(row < seq::kNoJump ? row : seq::kNoJump), sequencer_.jump_stamp()); }  // E-SONG-07: the stamp of the list the position was authored against
  bool loop_range(unsigned first, unsigned last) noexcept override { return first < seq::kNoJump && push(seq::Command::Kind::SetLoop, first, static_cast<std::uint16_t>(last < seq::kNoJump ? last : seq::kNoJump), sequencer_.jump_stamp()); }  // OS-SONG3 S7: the range as of the list now
  bool loop_rows(unsigned first, unsigned last) noexcept override { return first < seq::kNoJump && push(seq::Command::Kind::SetRowLoop, first, static_cast<std::uint16_t>(last < seq::kNoJump ? last : seq::kNoJump)); }  // OS-UXF Focus Loop
  bool queue_pattern(unsigned id) noexcept override { return id <= 255u && push(seq::Command::Kind::QueuePattern, id); }
  bool switch_pattern(unsigned id) noexcept override { return id >= 1u && id <= 255u && push(seq::Command::Kind::SwitchPattern, id); }
  bool release_pattern(unsigned id) noexcept override { return id >= 1u && id <= 255u && push(seq::Command::Kind::ReleasePattern, id); }
  bool select(bool song, unsigned pattern) noexcept override {
    return pattern <= 255u && ring_.push(seq::Command{seq::Command::Kind::Select, static_cast<std::uint8_t>(song ? 1u : 0u), static_cast<std::uint16_t>(pattern), 0});
  }

 private:
  bool push(seq::Command::Kind kind, unsigned pattern, std::uint16_t row = 0, std::uint32_t from_step = 0) noexcept {
    return ring_.push(seq::Command{kind, static_cast<std::uint8_t>(pattern), row, from_step});
  }
  const seq::Sequencer& sequencer_;  // (for jump_stamp() only)
  seq::CommandRing& ring_;  // (the ring, not the sequencer: the sequencer's own command ring is the producer side of the one SPSC queue, the main loop is its producer)
};

// The pads, the previews and the keyboard: events on the live queue, taken first in the next block (app_main.cpp LivePreview, which also holds back a note until the outputs open: they always are here).
class SimAudio::Preview final : public app::PreviewSink {
 public:
  Preview(audio::LiveBox& box, audio::Engine& engine, audio::VoicePool& voices) : box_(box), engine_(engine), voices_(voices) {}
  void preview(unsigned instrument, unsigned note) noexcept override {
    (void)push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePreview, 0, clamp7(note), clamp_instrument(instrument), audio::kLiveVelocityDefault, 0, 0});
  }
  void pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note) noexcept override { pad_on(pad, track, instrument, note, 0); }
  void slice_pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note, unsigned slice) noexcept override {
    pad_on(pad, track, instrument, note, static_cast<std::uint8_t>(slice + 1u > 255u ? 0u : slice + 1u));  // the event's channel is the pad's slice + 1 (live_notes.h)
  }
  void take_pad_down(unsigned pad, unsigned track, unsigned note) noexcept override {
    if (pad >= app::PadBoard::kPads) {
      return;
    }
    if (push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), clamp7(note), static_cast<std::uint8_t>(audio::kPreviewSlot + 1u), audio::kLiveVelocityDefault,
                              clamp_track(track), 0})) {
      held_ |= std::uint64_t{1} << pad;
    }
  }
  void audition(bool on, unsigned track) noexcept override {
    if (on) {
      (void)push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePreview, 0, audio::kRootNote, static_cast<std::uint8_t>(audio::kPreviewSlot + 1u), audio::kLiveVelocityDefault, clamp_track(track), 0});
    } else {
      release(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePreview, 0, 0, 0, 0, 0, 0});
    }
  }
  void hold_note(bool on, unsigned track, unsigned instrument, unsigned note) noexcept override {
    if (on) {
      (void)push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePreview, 1, clamp7(note), clamp_instrument(instrument), audio::kLiveVelocityDefault, clamp_track(track), 0});
    } else {
      release(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePreview, 1, 0, 0, 0, 0, 0});
    }
  }
  std::uint32_t live_position() const noexcept override { return voices_.live_position(); }
  void pad_up(unsigned pad) noexcept override {
    if (pad >= app::PadBoard::kPads || (held_ & (std::uint64_t{1} << pad)) == 0u) {
      return;
    }
    held_ &= ~(std::uint64_t{1} << pad);
    release(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), 0, 0, 0, 0, 0});
  }
  void midi_note(bool on, unsigned track, unsigned instrument, unsigned note, unsigned velocity, unsigned channel) noexcept override {
    if (note > 127u) {
      return;
    }
    if (on) {
      (void)push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourceMidi, static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(note), clamp_instrument(instrument),
                                  clamp7(velocity == 0u ? 1u : velocity), clamp_track(track), static_cast<std::uint8_t>(channel > 16u ? 0u : channel)});
    } else {
      release(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourceMidi, static_cast<std::uint8_t>(note), 0, 0, 0, 0, 0});
    }
  }
  // OS-APPUX S3 (ABI 1.10): the notes of the running app, source kLiveSourceApp with the app's slot 0..3 as the key (at most four held: ModuleUi counts them); an AllOff of the source ends them all.
  bool app_note(bool on, unsigned slot, unsigned instrument, unsigned note, unsigned velocity) noexcept override {
    if (slot >= 4u || note > 127u) {
      return false;
    }
    if (on) {
      return push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourceApp, static_cast<std::uint8_t>(slot), clamp7(note), clamp_instrument(instrument), clamp7(velocity == 0u ? 1u : velocity), 0, 0});
    }
    release(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourceApp, static_cast<std::uint8_t>(slot), 0, 0, 0, 0, 0});
    return true;
  }
  void app_notes_off() noexcept override { release(audio::LiveEvent{audio::LiveKind::AllOff, audio::kLiveSourceApp, 0, 0, 0, 0, 0, 0}); }
  void notes_off(bool midi_only) noexcept override {
    if (!midi_only) {
      held_ = 0;
    }
    release(audio::LiveEvent{audio::LiveKind::AllOff, midi_only ? audio::kLiveSourceMidi : audio::kLiveAllSources, 0, 0, 0, 0, 0, 0});
  }
  bool live_idle() const noexcept override { return !flush_ && engine_.live_idle(); }
  std::uint32_t dropped() const noexcept { return dropped_; }

 private:
  void pad_on(unsigned pad, unsigned track, unsigned instrument, unsigned note, std::uint8_t channel) noexcept {  // channel: the pad's slice + 1, 0 none (live_notes.h)
    if (pad >= app::PadBoard::kPads) {
      return;
    }
    if (push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), clamp7(note), clamp_instrument(instrument), audio::kLiveVelocityDefault, clamp_track(track),
                              channel})) {
      held_ |= std::uint64_t{1} << pad;
    }
  }
  static audio::LiveEvent all_notes_off() noexcept { return audio::LiveEvent{audio::LiveKind::AllOff, audio::kLiveAllSources, 0, 0, 0, 0, 0, 0}; }
  static std::uint8_t clamp7(unsigned v) noexcept { return static_cast<std::uint8_t>(v > 127u ? 127u : v); }
  static std::uint8_t clamp_instrument(unsigned v) noexcept { return static_cast<std::uint8_t>(v >= 1u && v <= audio::kSampleSlots ? v : 0u); }
  static std::uint8_t clamp_track(unsigned v) noexcept { return static_cast<std::uint8_t>(v < audio::kLiveTracks ? v : 0u); }
  bool push(const audio::LiveEvent& e) noexcept {
    if (flush_ && !box_.queue.push(all_notes_off())) {  // a release was lost: an all-notes-off goes first
      ++dropped_;
      return false;
    }
    flush_ = false;
    if (!box_.queue.push(e)) {
      ++dropped_;
      return false;
    }
    return true;
  }
  void release(const audio::LiveEvent& e) noexcept {
    if (!push(e)) {
      flush_ = true;
    }
  }
  audio::LiveBox& box_;
  audio::Engine& engine_;
  audio::VoicePool& voices_;
  std::uint64_t held_ = 0;  // the pads this sink pushed a note on, not yet released
  bool flush_ = false;
  std::uint32_t dropped_ = 0;
};

// The sequencer's per-track side (the deferred mute): app_main.cpp SeqTrackSink.
class SimAudio::Tracks final : public app::TrackSink {
 public:
  explicit Tracks(seq::Sequencer& sequencer) : seq_(sequencer), ring_(sequencer.commands()) {}
  bool playing() const noexcept override { return seq_.play_position().read().playing; }
  bool mute_defer(unsigned track, app::MuteMode mode, bool was_muted) noexcept override {
    return ring_.push(seq::Command{seq::Command::Kind::MuteQuant, static_cast<std::uint8_t>(track), static_cast<std::uint16_t>(mode), was_muted ? 1u : 0u});
  }
  bool mute_cancel(unsigned track) noexcept override { return ring_.push(seq::Command{seq::Command::Kind::MuteCancel, static_cast<std::uint8_t>(track), 0, 0}); }
  std::uint16_t mute_pending() const noexcept override { return seq_.mute_pending(); }
  std::uint8_t note_count(unsigned track) const noexcept override { return seq_.note_count(track); }
  void auto_mask(std::uint8_t mask) noexcept override { seq_.set_auto_mask(mask); }
  std::uint8_t auto_take(std::uint8_t* val) noexcept override { return seq_.take_auto(val); }

 private:
  seq::Sequencer& seq_;
  seq::CommandRing& ring_;  // (the pushes go through the ring itself: the one SPSC queue whose producer is the main loop)
};

// The Perform screen's commands, on the same ring.
class SimAudio::Perform final : public seq::CommandSink {
 public:
  explicit Perform(seq::CommandRing& ring) : ring_(ring) {}
  bool push(const seq::Command& command) noexcept override { return ring_.push(command); }

 private:
  seq::CommandRing& ring_;
};

// ---- the rig ---------------------------------------------------------------------------------------------------------------------------------

SimAudio::SimAudio(const model::Project& project, audio::VoicePool* voices)
    : own_voices_(voices == nullptr ? std::make_unique<audio::VoicePool>() : nullptr), voices_(voices != nullptr ? *voices : *own_voices_), project_(&project) {
  g_cycles = 0;
  taps_ = std::make_unique<audio::TapMem>();
  engine_ = std::make_unique<audio::Engine>(&fake_cycles, *taps_);
  sequencer_ = std::make_unique<seq::Sequencer>(project);
  {
    seq::Sequencer::Config cfg = sequencer_->config();
    cfg.emit_ticks = true;  // app_main.cpp make_sound: the Tick events (core FX 43) the Amiga voices' MED tick processor and the tracker FX run on
    sequencer_->configure(cfg);
  }
  amiga_ = std::make_unique<audio::AmigaVoices>();
  live_ = std::make_unique<audio::LiveBox>();
  events_ = std::make_unique<audio::SeqEvent[]>(audio::kMaxEventsPerBlock);
  delay_ = std::make_unique<audio::SendDelay>();
  delay_mem_ = std::make_unique<std::uint32_t[]>(audio::SendDelay::kRingFrames);
  reverb_ = std::make_unique<audio::SendReverb>();
  reverb_mem_ = std::make_unique<std::int16_t[]>(audio::SendReverb::kMemSamples);
  limiter_ = std::make_unique<audio::MasterLimiter>();
  transport_ = std::make_unique<Transport>(sequencer_->commands(), *sequencer_);
  preview_ = std::make_unique<Preview>(*live_, *engine_, voices_);
  tracks_ = std::make_unique<Tracks>(*sequencer_);
  perform_sink_ = std::make_unique<Perform>(sequencer_->commands());
  mix_ = std::make_unique<app::MixBoard>();
  perform_ = std::make_unique<app::PerformBoard>();
  mix_->meters = &engine_->graph().meters();
  mix_->tracks = tracks_.get();
  perform_->out = perform_sink_.get();

  // app_main.cpp make_sound(): the voices, the live notes, the sequencer as the event source, then the send effects and the limiter in their slots, before the audio starts.
  engine_->attach_voices(&voices_);
  engine_->attach_live(live_.get());
  engine_->attach_amiga(amiga_.get());
  modules_ = std::make_unique<audio::ModuleVoices>();
  module_scratch_ = AlignedBlock(static_cast<std::uint8_t*>(::operator new[](audio::ModuleVoices::kScratchBytes, std::align_val_t{16})));
  (void)modules_->attach_scratch(module_scratch_.get(), audio::ModuleVoices::kScratchBytes);
  modules_->set_sample_rate(static_cast<float>(hal::audio::sample_rate_mhz()) * 0.001f);
  engine_->attach_modules(modules_.get());
  engine_->attach_events(seq::event_source(*sequencer_, events_.get(), audio::kMaxEventsPerBlock));
  delay_->attach(delay_mem_.get(), audio::SendDelay::kRingFrames);
  reverb_->attach(reverb_mem_.get(), audio::SendReverb::kMemSamples);
  engine_->graph().install_send_fx(0, &audio::SendDelay::process_fx, delay_.get(), &audio::SendDelay::param_fx);
  engine_->graph().install_send_fx(1, &audio::SendReverb::process_fx, reverb_.get(), &audio::SendReverb::param_fx);
  engine_->graph().set_send_fx_bypass(0, false);
  engine_->graph().set_send_fx_bypass(1, false);
  engine_->graph().install_master_limiter(*limiter_);
  if (voices == nullptr) {
    bind_test_bank(project);
  }
  (void)bind_modules(project);
}

unsigned SimAudio::bind_modules(const model::Project& project) {
#ifdef O3SIM_TB303_HOST_SO
  unsigned bound = 0;
  for (unsigned slot = 0; slot < audio::kModuleSlots; ++slot) {
    const model::Instrument* in = project.instrument(slot);
    if (in == nullptr || in->kind != model::InstrumentKind::Module) {
      continue;
    }
    const model::ModuleRef* ref = project.module(in->module_index);
    if (ref == nullptr || std::strcmp(ref->name, "tb303") != 0) {
      continue;
    }
    if (module_handle_ == nullptr) {
      module_handle_ = dlopen(O3SIM_TB303_HOST_SO, RTLD_NOW | RTLD_LOCAL);
    }
    const auto* entry = module_handle_ != nullptr ? static_cast<const o3m_module*>(dlsym(module_handle_, "o3m_entry")) : nullptr;
    if (entry == nullptr) {
      return bound;
    }
    const std::size_t bytes = (static_cast<std::size_t>(entry->state_bytes) + 15u) & ~static_cast<std::size_t>(15u);
    AlignedBlock state(static_cast<std::uint8_t*>(::operator new[](bytes, std::align_val_t{16})));
    std::memset(state.get(), 0, bytes);
    if (modules_->bind(slot, *entry, static_cast<std::uint16_t>(0x8000u + slot), state.get(), in->params, in->param_len) == audio::ModuleVoices::Bind::Ok) {
      module_states_.push_back(std::move(state));
      ++bound;
    }
  }
  return bound;
#else
  (void)project;
  return 0;
#endif
}

// ---- the export (platform/mk66/src/app_main.cpp wire_export, make_export_sequencer) ----------------------------------------------------------------------

struct SimExportRig {
  explicit SimExportRig(const model::Project& p) : project(p), events(std::make_unique<audio::SeqEvent[]>(audio::kMaxEventsPerBlock)), ring(new std::uint8_t[app::ExportSource::kRingBytes]) {}
  const model::Project& project;
  std::unique_ptr<audio::SeqEvent[]> events;
  std::unique_ptr<std::uint8_t[]> ring;                 // (new[] gives 16-byte alignment: the ring wants 4)
  std::unique_ptr<seq::Sequencer> sequencer;            // the export's own, built afresh for every pass
  std::unique_ptr<seq::OfflineRender> render;
};

namespace {
bool sim_live_running() noexcept { return true; }  // the simulator's render runs from the first tick: the export waits for it to park
seq::Sequencer* make_export_sequencer(void* ctx) noexcept {
  auto* rig = static_cast<SimExportRig*>(ctx);
  rig->sequencer = std::make_unique<seq::Sequencer>(rig->project);
  return rig->sequencer.get();
}
}  // namespace

app::ExportSource::Wiring SimAudio::export_wiring() {
  if (!export_) {
    seq::OfflineRender::Wiring w;
    export_ = std::make_unique<SimExportRig>(*project_);
    w.engine = engine_.get();
    w.project = project_;
    w.live = sequencer_.get();
    w.events = export_->events.get();
    w.event_cap = audio::kMaxEventsPerBlock;
    w.live_running = &sim_live_running;
    w.make_sequencer = &make_export_sequencer;
    w.make_ctx = export_.get();
    export_->render = std::make_unique<seq::OfflineRender>(w);
  }
  app::ExportSource::Wiring ew;
  ew.renderer = export_->render.get();
  ew.ring = export_->ring.get();
  ew.ring_bytes = app::ExportSource::kRingBytes;
  ew.cycles = nullptr;   // no counter: kMaxBlocksPerStep paces a step
  ew.slice_cycles = 0;
  return ew;
}

bool SimAudio::add_render(const std::string& name, Pcm pcm, std::string& error) {
  if (renders_.count(name) != 0u) {
    error = "render '" + name + "' exists already";
    return false;
  }
  renders_[name] = std::move(pcm);
  return true;
}

SimAudio::~SimAudio() {
  if (modules_ != nullptr) {
    modules_->retire_all();   // (no render is running: the instances shut down at once, before their state and the shared object go)
    modules_->release_all();
  }
  if (module_handle_ != nullptr) {
    dlclose(module_handle_);
  }
}

app::TransportSink& SimAudio::transport() { return *transport_; }
app::PreviewSink& SimAudio::preview() { return *preview_; }
app::MixBoard& SimAudio::mix() { return *mix_; }
app::PerformBoard& SimAudio::perform() { return *perform_; }

void SimAudio::bind_test_bank(const model::Project& project) {
  bank_pcm_ = std::make_unique<std::int16_t[]>(kBankFrames);
  make_bank(bank_pcm_.get());
  const audio::LevelLaw law = app::project_law(project);
  for (unsigned slot = 0; slot < audio::kSampleSlots; ++slot) {
    const model::Instrument* in = project.instrument(slot);
    if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
      continue;
    }
    const std::int16_t* pcm = bank_pcm_.get();
    std::uint32_t frames = kSineFrames;
    if (slot == 1u) {
      pcm += kSineFrames;
      frames = kNoiseFrames;
    } else if (slot == 2u) {
      pcm += kSineFrames + kNoiseFrames;
      frames = kPluckFrames;
    }
    audio::FilterMod grlen{};  // OS-CEWTENV W5: the granular length source (slot 3 bit 0x02) goes with the descriptor
    audio::SampleDesc d = app::make_sample_desc(*in, pcm, frames, nullptr, nullptr, law, nullptr, &grlen);
    voices_.set_sample(slot, d, d.granular ? &grlen : nullptr);
  }
}

// ---- time ------------------------------------------------------------------------------------------------------------------------------------

void SimAudio::tick() {
  acc_ += hal::audio::sample_rate_mhz();  // the rate in mHz is the frames a tick (1 ms) brings, in millionths
  if (acc_ >= kBlockMicroFrames) {
    acc_ -= kBlockMicroFrames;
    render_block();
  }
  if (!open_name_.empty() && open_ticks_ != 0u && --open_ticks_ == 0u) {
    close_window();
  }
}

void SimAudio::render_block() {
  alignas(8) std::int16_t in[hal::audio::kBlockSamples] = {};
  alignas(8) std::int16_t out[hal::audio::kBlockSamples] = {};
  hal::audio::BlockReport report{};
  const hal::audio::BlockIo io{in, out, 0, &report};
  const hal::audio::BlockCallback live = engine_->callback();   // the unit's own entry: it parks while an export holds the engine, and then the block is silence
  live.fn(live.user, io);
  ++blocks_;
  if (open_name_.empty()) {
    return;
  }
  Pcm& master = renders_[open_name_];
  master.lr.insert(master.lr.end(), out, out + hal::audio::kBlockSamples);
  if (open_stems_) {
    ++open_blocks_;
    collect_stems();
  }
}

void SimAudio::collect_stems() {
  constexpr unsigned kChannels = audio::StemTap::kDefaultChannels;  // T1..T8, Master L, Master R
  alignas(4) std::int16_t buf[usb::kStemBlockFrames * kChannels];
  while (reader_->fill_frames() >= usb::kStemBlockFrames) {
    if (reader_->read_interleaved(buf, usb::kStemBlockFrames, kChannels) != usb::kStemBlockFrames) {
      break;
    }
    for (unsigned t = 0; t < kStemTracks; ++t) {
      Pcm& p = renders_[open_name_ + ".t" + std::to_string(t + 1u)];
      for (std::uint32_t f = 0; f < usb::kStemBlockFrames; ++f) {
        const std::int16_t v = buf[f * kChannels + t];
        p.lr.push_back(v);
        p.lr.push_back(v);
      }
    }
    Pcm& m = renders_[open_name_ + ".m"];
    for (std::uint32_t f = 0; f < usb::kStemBlockFrames; ++f) {
      m.lr.push_back(buf[f * kChannels + kStemTracks]);
      m.lr.push_back(buf[f * kChannels + kStemTracks + 1u]);
    }
  }
}

void SimAudio::close_window() {
  if (open_stems_) {
    engine_->graph().stems().set_enabled(false);
    reader_.reset();
    if ((open_blocks_ & 1u) != 0u) {  // a stem block is two render blocks: the odd last block of the master is left out
      Pcm& master = renders_[open_name_];
      master.lr.resize(master.lr.size() - hal::audio::kBlockSamples);
    }
  }
  open_name_.clear();
  open_stems_ = false;
}

bool SimAudio::begin_render(const std::string& name, std::uint32_t ms, bool stems, std::string& error) {
  if (!open_name_.empty()) {
    error = "render '" + open_name_ + "' is still running (" + std::to_string(open_ticks_) + " ms more): one at a time";
    return false;
  }
  if (renders_.count(name) != 0u) {
    error = "render '" + name + "' exists already";
    return false;
  }
  if (ms == 0u || ms > kMaxRenderMs) {
    error = "a render is 1.." + std::to_string(kMaxRenderMs) + " ms";
    return false;
  }
  Pcm pcm;
  pcm.rate = static_cast<double>(hal::audio::sample_rate_mhz()) / 1000.0;
  renders_[name] = pcm;
  if (stems) {
    for (unsigned t = 0; t < kStemTracks; ++t) {
      renders_[name + ".t" + std::to_string(t + 1u)] = pcm;
    }
    renders_[name + ".m"] = pcm;
    reader_ = std::make_unique<usb::StemReader>(engine_->graph().stems().ring());
    reader_->restart();
    engine_->graph().stems().set_enabled(true);
  }
  open_name_ = name;
  open_ticks_ = ms;
  open_stems_ = stems;
  open_blocks_ = 0;
  return true;
}

const Pcm* SimAudio::render(const std::string& name) const {
  if (!open_name_.empty() && (name == open_name_ || name.compare(0, open_name_.size() + 1u, open_name_ + ".") == 0)) {
    return nullptr;
  }
  const auto it = renders_.find(name);
  return it == renders_.end() ? nullptr : &it->second;
}

std::string SimAudio::open_render(std::uint32_t* ticks_left) const {
  if (ticks_left != nullptr) {
    *ticks_left = open_ticks_;
  }
  return open_name_;
}

std::vector<SimAudio::Finished> SimAudio::finished() const {
  std::vector<Finished> out;
  for (const auto& entry : renders_) {
    if (render(entry.first) != nullptr) {
      out.push_back({entry.first, &entry.second});
    }
  }
  return out;
}

}  // namespace o3os::sim
