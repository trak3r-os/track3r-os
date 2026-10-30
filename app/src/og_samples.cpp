// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/og_samples.h>
#include <o3os/app/save_service.h>
#include <o3os/app/wave_math.h>

#include <o3os/core/fast_math.h>
#include <o3os/formats/og.h>
#include <o3os/model/params.h>

#include "text_line.h"

#include <cstring>

namespace o3os::app {
namespace {

using hal::storage::Status;
namespace og = formats::og;

constexpr std::size_t kMinRoom = 512;           // a slice always moves something, whatever budget it was given
constexpr unsigned kSlots = audio::kSampleSlots;  // the voice table's, which is the model's kMaxInstruments
constexpr std::uint32_t kSampleRate = 44100;    // an OG folder takes 44,100 Hz only (whether the official firmware refuses other rates is an open research question)
constexpr std::uint8_t kPlayOneShot = 0;        // play mode 0 of the OG instrument file
constexpr std::uint8_t kPlayForwardLoop = 1;
constexpr std::uint8_t kPlayBackwardLoop = 2;
constexpr std::uint8_t kPlayPingpong = 3;
constexpr std::uint8_t kPlaySlice = 4;
constexpr std::uint8_t kPlayBeatSlice = 5;
constexpr std::uint8_t kPlayWavetable = 6;
constexpr std::uint8_t kPlayGranular = 7;
constexpr unsigned kSlotPan = 1;                // the instrument file's envelope and LFO order: amp, pan, cutoff, wavetable position, granular position, finetune (model Envelope order)
constexpr unsigned kSlotCutoff = 2;
constexpr unsigned kSlotWtPos = 3;
constexpr unsigned kSlotGranPos = 4;
constexpr unsigned kSlotFinetune = 5;

// floor(point * length / 65535), our conversion of a position (a float multiply by length / 65535, truncated; whether the official firmware converts the same way is an open
// research question, docs/specs/OS-007-platform.md). The last point is the end of the sample exactly: the float product can land a sample short of it, which this avoids.
std::uint32_t at_point(std::uint16_t point, std::uint32_t frames) noexcept {
  if (point >= 65535u) {
    return frames;
  }
  return static_cast<std::uint32_t>(static_cast<float>(point) * (static_cast<float>(frames) / 65535.0f));
}

// A card that is gone or failing, as OgFolderLoader says it: every other answer is about the one file.
bool card_fault(Status s) noexcept { return s == Status::Io || s == Status::NotMounted || s == Status::NoCard; }

}  // namespace

const char* sample_problem_name(SampleProblem p) noexcept {
  switch (p) {
    case SampleProblem::NotFound: return "not found";
    case SampleProblem::Unreadable: return "unreadable";
    case SampleProblem::Unusable: return "unusable";
    case SampleProblem::NoRoom: return "no room";
  }
  return "?";
}

// ---- What the voices get ------------------------------------------------------------------------------------------------------------------
namespace {

unsigned slice_count_of(const model::SamplerParams& p) noexcept { return p.slice_count > model::kSlices ? model::kSlices : p.slice_count; }

// Slice `k` as two points of 0..65535 (slice and beat slice modes): it starts at slices[k] (0 when the instrument has none), and ends where the
// next one starts, the last (or the only) one at the end of the sample.
void slice_points(const model::SamplerParams& p, unsigned k, unsigned& from, unsigned& to) noexcept {
  const unsigned n = slice_count_of(p);
  from = n > 0u ? p.slices[k < model::kSlices ? k : model::kSlices - 1u] : 0u;
  to = (n > 1u && k != n - 1u && k + 1u < model::kSlices) ? p.slices[k + 1u] : 65535u;
}

}  // namespace

std::uint32_t slice_table_samples(const model::Instrument& in) noexcept {
  const model::SamplerParams& p = in.sampler;
  return (p.play_mode == kPlayBeatSlice && slice_count_of(p) != 0u) ? 2u * (slice_count_of(p) + 1u) : 0u;
}

bool sample_desc_reads(unsigned field) noexcept {
  namespace ip = model::instrument_param;
  if (field <= ip::FineTune || (field >= ip::FilterEnable && field <= ip::Resonance) || field == ip::DelaySend || field == ip::ReverbSend) {  // (OS-SENDFX: the sends are the descriptor's too)
    return true;
  }
  if (field == ip::Glide || field == ip::Overdrive || field == ip::BitDepth) {  // OS-IFX1: and so are the glide, the overdrive and the bit depth
    return true;
  }
  if (ip::is_mod_field(field)) {
    return true;  // OS-INSTUI S6: SOURCE (the loop byte) and the LFO's shape, speed and amount are read when the descriptor is built (make_sample_desc), not by the engine's ring
  }
  if (field < ip::kEnvelopeFirst) {
    return false;
  }
  const unsigned e = (field - ip::kEnvelopeFirst) / ip::kEnvelopeStride;
  const unsigned f = (field - ip::kEnvelopeFirst) % ip::kEnvelopeStride;
  return e < ip::kEnvelopes && f < ip::kEnvelopeFields;  // all six slots: amp (0), pan (1), cutoff (2), wavetable position (3), granular position (4), finetune (5)
}

namespace {
// The loop make_sample_desc binds for `in` over `frames`: its start and end and the loop's, in samples from the window's start (the descriptor's `data`); false when the descriptor has no loop
// (a mode that is not a loop, an empty window, a loop outside it or under 2 samples: a one shot).
bool bound_loop(const model::Instrument& in, std::uint32_t frames, std::uint32_t& start, std::uint32_t& end, std::uint32_t& ls, std::uint32_t& le) noexcept {
  const model::SamplerParams& p = in.sampler;
  if (in.kind != model::InstrumentKind::Sample || (p.play_mode != kPlayForwardLoop && p.play_mode != kPlayBackwardLoop && p.play_mode != kPlayPingpong)) {
    return false;
  }
  start = at_point(p.start, frames);
  end = at_point(p.end, frames);
  const std::uint32_t l0 = at_point(p.loop_start, frames);
  const std::uint32_t l1 = at_point(p.loop_end, frames);
  if (start >= end || l0 < start || l0 >= l1 || l1 > end || l1 - l0 < 2u) {
    return false;
  }
  ls = l0 - start;
  le = l1 - start;
  return true;
}
}  // namespace

unsigned live_loop_moves(const model::Instrument& before, const model::Instrument& after, std::uint32_t frames, LoopMove out[2]) noexcept {
  std::uint32_t s0 = 0, e0 = 0, a0 = 0, b0 = 0, s1 = 0, e1 = 0, a1 = 0, b1 = 0;
  if (!bound_loop(before, frames, s0, e0, a0, b0) || !bound_loop(after, frames, s1, e1, a1, b1) || s0 != s1 || e0 != e1 || before.sampler.play_mode != after.sampler.play_mode) {
    return 0;
  }
  namespace ip = audio::inst_param;
  const LoopMove start{ip::LoopStart, static_cast<float>(a1)};
  const LoopMove end{ip::LoopEnd, static_cast<float>(b1)};
  unsigned n = 0;
  if (a1 + 2u > b0) {  // the new start is not under the old end: the end goes first, or the start would be held to the old end
    if (b1 != b0) out[n++] = end;
    if (a1 != a0) out[n++] = start;
  } else {
    if (a1 != a0) out[n++] = start;
    if (b1 != b0) out[n++] = end;
  }
  return n;
}


audio::SampleDesc make_sample_desc(const model::Instrument& in, const std::int16_t* pcm, std::uint32_t frames, bool* approximated, std::int16_t* slice_table,
                                   audio::LevelLaw law) noexcept {
  audio::SampleDesc d;  // length 0: silent
  if (approximated != nullptr) {
    *approximated = false;
  }
  const model::SamplerParams& p = in.sampler;
  if (pcm == nullptr || frames == 0u) {
    return d;
  }
  switch (p.play_mode) {
    case kPlaySlice: {  // the selected slice, at the note's pitch; the instrument's own start and end are not used
      unsigned from = 0;
      unsigned to = 0;
      slice_points(p, p.selected_slice, from, to);
      const std::uint32_t start = at_point(static_cast<std::uint16_t>(from), frames);
      std::uint32_t end = at_point(static_cast<std::uint16_t>(to), frames);
      if (end < start) {
        end = frames;  // an end before the start: the slice runs to the end of the sample
      }
      if (start >= end) {
        return d;
      }
      d.data = pcm + start;
      d.length = end - start;
      d.offset = start;
      break;
    }
    case kPlayBeatSlice: {  // the whole sample and the slices' boundaries: the voices pick the slice from the note
      d.data = pcm;
      d.length = frames;
      d.beat_slice = true;
      const unsigned n = slice_count_of(p);
      if (n != 0u && slice_table != nullptr) {
        for (unsigned i = 0; i <= n; ++i) {
          const std::uint32_t b = i < n ? at_point(p.slices[i], frames) : frames;
          slice_table[2u * i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(b & 0xFFFFu));
          slice_table[2u * i + 1u] = static_cast<std::int16_t>(static_cast<std::uint16_t>(b >> 16));
        }
        d.slice_count = static_cast<std::uint8_t>(n);
        d.slices = reinterpret_cast<const std::uint16_t*>(slice_table);  // the same 16-bit objects, read unsigned: the voices' view of the halves
      } else if (n != 0u && approximated != nullptr) {
        *approximated = true;  // no room for the table: the whole sample at the root's pitch
      }
      break;
    }
    case kPlayWavetable: {  // the current window as an oscillator; start and end are not used
      // A stored size is used as it is (a window the sample cannot hold plays nothing, OS-007h); an unset or invalid one (0: a native instrument never given one) is 2048 held to the sample, the window the page
      // shows (wt_play_size is the one rule: the pages and the Wave screen draw no window where it gives 0).
      const unsigned n = wt_play_size(p.wavetable_window_size, frames);
      if (n == 0u || frames < n) {
        return d;  // not one whole window
      }
      const std::uint32_t whole = frames - frames % n;
      std::uint64_t at = static_cast<std::uint64_t>(n) * p.wavetable_window;
      if (at > whole - n) {
        at = whole - n;  // clamped to the last whole window
      }
      d.data = pcm + at;
      d.length = n;
      d.loop_start = 0;
      d.loop_end = n;
      d.wt_size = static_cast<std::uint16_t>(n);
      d.wt_windows = whole / n;  // the stack's windows and the one bound: the voices can move between them (audio/voices.h POSITION MODULATION)
      d.wt_pos = static_cast<std::uint32_t>(at / n);
      d.offset = static_cast<std::uint32_t>(at);
      break;
    }
    case kPlayGranular: {  // the whole sample and the grain: start, end and loop points are not used; type 0 forward / 1 backward / 2 ping-pong is the grain's direction
      d.data = pcm;
      d.length = frames;
      d.granular = true;
      d.gr_len = p.grain_length >= 44u ? p.grain_length : 441u;  // OS-INSTUI S3: a native instrument starts with a grain of 0 (init_instrument zeroes it, the file's bytes stay what they were): the default 441, as the smallest grain length is 44
      d.gr_pos = p.grain_position;
      d.gr_shape = p.grain_shape;
      d.loop_mode = p.grain_type;
      break;
    }
    default: {
      const std::uint32_t start = at_point(p.start, frames);
      const std::uint32_t end = at_point(p.end, frames);
      if (start >= end) {
        return d;  // a start point past the end: nothing plays
      }
      d.data = pcm + start;
      d.length = end - start;
      d.offset = start;
      if (p.play_mode == kPlayForwardLoop || p.play_mode == kPlayBackwardLoop || p.play_mode == kPlayPingpong) {
        const std::uint32_t loop_start = at_point(p.loop_start, frames);
        const std::uint32_t loop_end = at_point(p.loop_end, frames);
        if (loop_start >= start && loop_start < loop_end && loop_end <= end) {  // the loop conditions; a loop that fails them is a one shot
          d.loop_start = loop_start - start;
          d.loop_end = loop_end - start;
          d.loop_mode = static_cast<std::uint8_t>(p.play_mode - kPlayForwardLoop);  // 1 forward -> 0, 2 backward -> 1, 3 ping-pong -> 2 (voices.h LOOPS)
        }
      }
      break;
    }
  }
  // OS-RATEPITCH: the sample plays at the pitch it was recorded at: the factor of its rate (INST sample_rate, 0 = 44,100) scales the voices' increment. A wavetable's pitch is its window's.
  d.rate_q28 = d.wt_size == 0u ? audio::rate_factor_q28(in.sample_rate) : 0u;
  // The voice, filter and envelope fields: audio::apply_instrument_param is the one place a stored parameter becomes a descriptor field, so that a live edit of a Params control (the
  // engine's ring: VoicePool::apply_param) and this rebuild of the descriptor from the project agree field for field (OS-LIVEP). 1.2+ law up to +24 dB (the voices saturate): 50 is unity
  // under it, 100 under the 1.1.14 law.
  namespace ip = audio::inst_param;
  const auto put = [&d, law](unsigned field, float value) { (void)audio::apply_instrument_param(d, field, value, law); };
  put(ip::Volume, p.volume);
  put(ip::Tune, p.tune);
  put(ip::FineTune, p.fine_tune);
  put(ip::Panning, p.panning);
  put(ip::DelaySend, p.delay_send);   // OS-SENDFX: what the note's track sends to the delay and the reverb (Engine: Graph::set_voice_sends)
  put(ip::ReverbSend, p.reverb_send);
  put(ip::Glide, p.glide);            // OS-IFX1: the glide time (ms), the overdrive and the bit depth (voices.h GLIDE, OVERDRIVE, BIT DEPTH)
  put(ip::Overdrive, p.overdrive);
  put(ip::BitDepth, p.bit_depth);
  // The envelope fields in the order of ip::EnvEnable ... ip::EnvRelease; the descriptor's `lfo` flag is set first (an LFO slot cannot be enabled as an ADSR).
  const auto put_envelope = [&put](unsigned slot, const model::Envelope& e) {
    const float values[ip::kEnvelopeFields] = {e.enable != 0u ? 1.0f : 0.0f, e.amount, static_cast<float>(e.attack), static_cast<float>(e.decay), e.sustain, static_cast<float>(e.release)};
    for (unsigned f = 0; f < ip::kEnvelopeFields; ++f) {
      put(ip::envelope(slot, f), values[f]);
    }
  };
  // The amp envelope, slot 0 (voices.h AMP ENVELOPE): an ADSR when its loop byte is clear, else (an LFO-based envelope) a tremolo of lfo[0]'s shape, rate (the
  // amp table's) and amount (AMP LFO MODULATION), and the plain gain.
  const model::Envelope& e = p.envelope[0];
  d.env.lfo = e.loop != 0u;
  d.env.delay = audio::env_units(e.delay);
  d.env.hold = audio::env_units(e.hold);  // 0 from an OG file (the readers drop it): one group at full level
  put_envelope(0, e);
  audio::FilterMod& am = d.amp_mod;
  am.lfo = e.loop != 0u;
  am.shape = p.lfo[0].shape;
  am.rate = p.lfo[0].speed;
  am.amount = p.lfo[0].amount;
  am.lfo_amount = p.lfo[0].amount;
  // The filter (voices.h FILTER): the instrument's type, cutoff and resonance as stored; a disabled one is bypassed by the voices.
  put(ip::FilterType, p.filter_type);
  put(ip::FilterEnable, p.filter_enable);
  put(ip::Cutoff, p.cutoff);
  put(ip::Resonance, p.resonance);
  // The modulators of slots 2 (the cutoff, voices.h FILTER MODULATION), 1 (the pan) and 5 (the finetune): enabled, the slot is an LFO when its loop byte is set (an LFO-based envelope:
  // shape, rate and amount from lfo[slot]), else an ADSR; the voices hold every field to range. The instrument's lfo[slot] shape and amount go in whatever the modulator is: the Filter LFO FX
  // (35) plays the cutoff's with its own rate (OS-FU2, voices.h FILTER LFO FX).
  const auto put_mod = [&](unsigned slot, audio::FilterMod& m) {
    const model::Lfo& l = p.lfo[slot];
    const model::Envelope& me = p.envelope[slot];
    m.shape = l.shape;
    m.lfo_amount = l.amount;
    m.lfo = me.loop != 0u;
    m.delay = me.delay;
    m.hold = me.hold;
    m.rate = l.speed;  // (OS-LFOLIVE: held whatever the modulator is, as the amp slot's is: a live SOURCE edit finds it in the descriptor, and the rebuild equals the live edits)
    if (m.lfo) {
      m.amount = l.amount;
    }
    put_envelope(slot, me);
  };
  put_mod(kSlotCutoff, d.filter.mod);
  put_mod(kSlotPan, d.pan_mod);
  put_mod(kSlotFinetune, d.tune_mod);
  // OS-GRAN: the position modulator, slot 3 for a wavetable (more than one window) and slot 4 for a granular instrument: one descriptor.
  if (d.granular) {
    put_mod(kSlotGranPos, d.pos_mod);
  } else if (d.wt_size != 0u && d.wt_windows > 1u) {
    put_mod(kSlotWtPos, d.pos_mod);
  }
  return d;
}

// ---- Control ------------------------------------------------------------------------------------------------------------------------------
bool OgSampleLoader::join(const char* a, const char* b) noexcept {
  std::size_t n = 0;
  const char* parts[3] = {base_, a, b};
  for (const char* part : parts) {
    if (part == nullptr || part[0] == '\0') {
      continue;
    }
    const std::size_t len = std::strlen(part);
    const std::size_t need = len + (n != 0u ? 1u : 0u);
    if (n + need + 1u > sizeof path_) {
      return false;
    }
    if (n != 0u) {
      path_[n++] = '/';
    }
    std::memcpy(path_ + n, part, len);
    n += len;
  }
  path_[n] = '\0';
  return n != 0u;
}

void OgSampleLoader::close_all() noexcept {
  if (storage_ != nullptr && file_.is_open()) {
    storage_->close(file_);
  }
  if (storage_ != nullptr && dir_.is_open()) {
    storage_->closedir(dir_);
  }
  file_ = hal::storage::FileHandle{};
  dir_ = hal::storage::DirHandle{};
}

bool OgSampleLoader::begin(hal::storage::Storage& storage, const char* folder, const model::Project& project, std::uint64_t pti_slots,
                           audio::VoicePool& voices, std::int16_t* arena, std::uint32_t arena_samples, audio::LevelLaw law, SampleBank* bank, bool native) noexcept {
  close_all();  // anything running is abandoned
  state_ = State::Idle;
  if (!hal::storage::valid_path(folder) || arena == nullptr) {
    return false;
  }
  const std::size_t len = std::strlen(folder);
  if (len == 0u || len > OgFolderLoader::kFolderMax) {
    return false;
  }
  std::memcpy(base_, folder, len + 1u);
  storage_ = &storage;
  feed_ = nullptr;
  project_ = &project;
  voices_ = &voices;
  bank_ = bank;
  if (bank_ != nullptr) {
    bank_->clear();  // a project load replaces every sample
  }
  arena_ = arena;
  arena_samples_ = arena_samples & ~1u;  // every sample starts on an even index: 4-byte aligned, as the importer wants
  law_ = law;
  native_ = native;
  used_ = 0;
  pti_slots_ = pti_slots;
  settled_ = 0;
  slot_ = 0;
  cursor_ = 0;
  entries_ = 0;
  pti_left_ = 0;
  pti_frames_ = 0;
  pti_bytes_ = 0;
  report_ = SampleLoadReport{};
  source_.reset();
  stage_ = pti_slots != 0u ? Stage::ListOpen : Stage::WavNext;
  state_ = State::Running;
  return true;
}

bool OgSampleLoader::begin_feed(PcmFeed& feed, const model::Project& project, audio::VoicePool& voices, std::int16_t* arena, std::uint32_t arena_samples, audio::LevelLaw law,
                                SampleBank* bank) noexcept {
  close_all();
  state_ = State::Idle;
  if (arena == nullptr) {
    return false;
  }
  storage_ = nullptr;
  feed_ = &feed;
  project_ = &project;
  voices_ = &voices;
  bank_ = bank;
  if (bank_ != nullptr) {
    bank_->clear();  // an import replaces every sample
  }
  arena_ = arena;
  arena_samples_ = arena_samples & ~1u;
  law_ = law;
  native_ = false;
  used_ = 0;
  pti_slots_ = 0;
  settled_ = 0;
  slot_ = 0;
  cursor_ = 0;
  entries_ = 0;
  pti_left_ = 0;
  pti_frames_ = 0;
  pti_bytes_ = 0;
  report_ = SampleLoadReport{};
  source_.reset();
  stage_ = Stage::FeedNext;
  state_ = State::Running;
  return true;
}

void OgSampleLoader::abort() noexcept {
  close_all();
  if (state_ == State::Running) {
    state_ = State::Idle;
  }
}

bool OgSampleLoader::is_sample_slot(unsigned slot) const noexcept {
  const model::Instrument* in = project_->instrument(slot);
  return in != nullptr && in->kind == model::InstrumentKind::Sample;
}

// The slot stays silent: counted, remembered (the first kMaxProblems), and never tried again.
void OgSampleLoader::settle(std::uint8_t slot, SampleProblem why) noexcept {
  settled_ |= std::uint64_t{1} << (slot & 63u);
  if (why == SampleProblem::NoRoom) {
    ++report_.no_room;
  } else {
    ++report_.missing;
  }
  if (report_.problem_count < SampleLoadReport::kMaxProblems) {
    report_.problems[report_.problem_count++] = {slot, why};
  }
}

// The card ended the loading: what has not had its turn is counted as missing by finish(). Not a failed load: the instruments already bound play.
void OgSampleLoader::card_failed(Status s) noexcept {
  close_all();
  report_.storage_error = s;
  stage_ = Stage::Finish;
}

// ---- The slices ---------------------------------------------------------------------------------------------------------------------------
OgSampleLoader::State OgSampleLoader::step(std::size_t byte_budget) noexcept {
  if (state_ != State::Running) {
    return state_;
  }
  if (bank_ != nullptr && bank_->clear_busy()) {
    return state_;  // the render still reads (a loop) or writes (a take) the arena the clear emptied: nothing is written over it until it has let go (a block)
  }
  const std::size_t budget = byte_budget < kMinRoom ? kMinRoom : byte_budget;
  const std::uint32_t start = moved();
  for (unsigned actions = 0; state_ == State::Running && actions < kMaxActionsPerStep; ++actions) {
    const std::size_t used = static_cast<std::uint32_t>(moved() - start);
    if (used >= budget) {
      break;
    }
    advance(budget - used);
  }
  return state_;
}

void OgSampleLoader::advance(std::size_t room) noexcept {
  switch (stage_) {
    case Stage::ListOpen: list_open(); return;
    case Stage::ListNext: list_next(); return;
    case Stage::PtiRead: pti_read(room); return;
    case Stage::WavNext: wav_next(); return;
    case Stage::WavRead: wav_read(room); return;
    case Stage::SideNext: side_next(); return;
    case Stage::SideRead: side_read(room); return;
    case Stage::Finish: finish(); return;
    case Stage::FeedNext: feed_next(); return;
    case Stage::FeedRead: feed_read(room); return;
  }
}

// Opens base_/sub[/name] into file_. False: the slot is settled (its file is not there, or would not open) or the card failed; nothing is open.
bool OgSampleLoader::open_file(const char* sub, const char* name, std::uint8_t slot) noexcept {
  if (!join(sub, name)) {
    settle(slot, SampleProblem::NotFound);  // a name that does not fit a path no card can hold
    return false;
  }
  const Status s = storage_->open(path_, file_);
  if (s == Status::Ok) {
    return true;
  }
  if (card_fault(s)) {
    card_failed(s);
  } else if (s == Status::NotFound || s == Status::NotAFile || s == Status::NotADirectory || s == Status::BadPath) {
    settle(slot, SampleProblem::NotFound);
  } else {
    settle(slot, SampleProblem::Unreadable);  // no free handle, an unsupported volume
  }
  return false;
}

// What the slot's audio is, once it is in the arena at used_: the voices' table, the accounting. A window with no sound leaves the arena as it was. A beat slice
// instrument's slice table follows its PCM (slice_table_samples: even, so the next sample is on an even index too); an instrument whose table does not fit is silent.
void OgSampleLoader::bind(std::uint8_t slot, std::uint32_t frames, std::uint32_t rate_hz, bool side) noexcept {
  if (frames > arena_samples_) {
    settle(slot, SampleProblem::NoRoom);  // (before the rounding: a count near 2^32 would wrap to 0)
    return;
  }
  const model::Instrument* in = project_->instrument(slot);
  const std::uint32_t pcm_samples = (frames + 1u) & ~1u;
  const std::uint32_t block = side ? 2u * pcm_samples : pcm_samples;  // OS-STEREO (b): M, then S (the slice table follows both)
  const std::uint32_t table_samples = in != nullptr ? slice_table_samples(*in) : 0u;
  if (block > arena_samples_ - used_ || table_samples > arena_samples_ - used_ - block) {
    settle(slot, SampleProblem::NoRoom);
    return;
  }
  bool approximated = false;
  audio::SampleDesc desc =
      in != nullptr ? make_sample_desc(*in, arena_ + used_, frames, &approximated, table_samples != 0u ? arena_ + used_ + block : nullptr, law_) : audio::SampleDesc{};
  if (desc.length == 0u) {
    settle(slot, SampleProblem::Unusable);
    return;
  }
  if (rate_hz != 0u && desc.wt_size == 0u) {
    desc.rate_q28 = audio::rate_factor_q28(rate_hz);  // OS-RATEPITCH: what the file holds is what plays (the instrument's own rate agrees, or the file is 44,100 Hz)
  }
  if (bank_ != nullptr) {
    desc.engine = bank_->engine_for(slot, *in);  // OS-BUG-MODENGINE: the instrument's saved engine (or its AUTO rule) is the load's, as rebind_now gives it
  }
  voices_->set_sample(slot, desc);
  if (bank_ != nullptr) {
    bank_->adopt(slot, arena_ + used_, frames, block, side ? arena_ + used_ + pcm_samples : nullptr);  // (OS-ARENA-RECLAIM: the slot owns its block; a later load reuses its room)
  }
  used_ += block + table_samples;
  report_.samples = used_;
  ++report_.loaded;
  if (approximated) {
    ++report_.approximated;
  }
  settled_ |= std::uint64_t{1} << (slot & 63u);
}

// ---- .pti: the PCM follows the 392-byte head -----------------------------------------------------------------------------------------------
void OgSampleLoader::list_open() noexcept {
  if (!join("instruments", nullptr)) {
    stage_ = Stage::WavNext;
    return;
  }
  const Status s = storage_->opendir(path_, dir_);
  if (s == Status::Ok) {
    entries_ = 0;
    stage_ = Stage::ListNext;
  } else if (s == Status::NotFound || s == Status::NotADirectory) {
    stage_ = Stage::WavNext;  // the folder went away since the project was read: every slot tries its WAV
  } else {
    card_failed(s);
  }
}

void OgSampleLoader::list_next() noexcept {
  const Status s = storage_->readdir(dir_, entry_);
  const bool cut = s == Status::Ok && ++entries_ > kMaxEntriesPerDirectory;
  if (s == Status::End || cut) {
    storage_->closedir(dir_);
    stage_ = Stage::WavNext;
    return;
  }
  if (s != Status::Ok) {
    card_failed(s);
    return;
  }
  if (entry_.is_dir) {
    return;
  }
  const og::FileName fn = og::classify_file_name(entry_.name);
  const unsigned slot = fn.index;
  if (fn.file_class != og::FileClass::Pti || slot >= kSlots || ((pti_slots_ >> slot) & 1u) == 0u || ((settled_ >> slot) & 1u) != 0u ||
      !is_sample_slot(slot)) {
    return;  // not a .pti, or not the one OgFolderLoader applied to a sample slot (a second one for a slot loses to the first)
  }
  slot_ = static_cast<std::uint8_t>(slot);
  if (!open_file("instruments", entry_.name, slot_)) {
    return;
  }
  std::uint32_t size = 0;
  Status st = storage_->size(file_, size);
  const std::uint32_t frames = size > kPtiHeadBytes ? (size - kPtiHeadBytes) / 2u : 0u;  // an odd last byte is ignored, as the reader does
  if (st == Status::Ok && frames != 0u && frames <= arena_samples_ - used_) {
    st = storage_->seek(file_, kPtiHeadBytes);
    if (st == Status::Ok) {
      pti_left_ = frames * 2u;
      pti_frames_ = frames;
      stage_ = Stage::PtiRead;
      return;
    }
  }
  storage_->close(file_);
  if (card_fault(st)) {
    card_failed(st);
  } else if (st != Status::Ok) {
    settle(slot_, SampleProblem::Unreadable);
  } else {
    settle(slot_, frames == 0u ? SampleProblem::Unusable : SampleProblem::NoRoom);
  }
}

void OgSampleLoader::pti_read(std::size_t room) noexcept {
  std::size_t want = room < kMinRoom ? kMinRoom : room;
  want = want > pti_left_ ? pti_left_ : want;
  want = want > kPtiChunkBytes ? kPtiChunkBytes : want;
  std::uint8_t* const dst = reinterpret_cast<std::uint8_t*>(arena_ + used_) + (pti_frames_ * 2u - pti_left_);
  std::size_t got = 0;
  const Status s = storage_->read(file_, dst, want, got);
  if (s != Status::Ok || got > want) {
    card_failed(s != Status::Ok ? s : Status::Io);  // a read that fails mid-file is the card, as in the project load
    return;
  }
  if (got == 0u) {
    storage_->close(file_);
    settle(slot_, SampleProblem::Unreadable);  // the file ended before its size said
    stage_ = Stage::ListNext;
    return;
  }
  pti_left_ -= static_cast<std::uint32_t>(got);
  pti_bytes_ += static_cast<std::uint32_t>(got);
  if (pti_left_ == 0u) {
    storage_->close(file_);
    bind(slot_, pti_frames_);
    stage_ = Stage::ListNext;
  }
}

// ---- samples/instrNN.wav --------------------------------------------------------------------------------------------------------------------
void OgSampleLoader::wav_next() noexcept {
  while (cursor_ < kSlots && (((settled_ >> cursor_) & 1u) != 0u || !is_sample_slot(cursor_))) {
    ++cursor_;  // at most 64 steps
  }
  if (cursor_ >= kSlots) {
    stage_ = Stage::Finish;
    return;
  }
  const std::uint8_t slot = cursor_;
  slot_ = slot;
  const char* const path = project_->instrument(slot)->sample_path;
  if (path[0] == '\0') {
    settle(slot, SampleProblem::NotFound);  // the instrument names no file
    return;
  }
  if (arena_samples_ == used_) {
    settle(slot, SampleProblem::NoRoom);
    return;
  }
  if (!open_file(path, nullptr, slot)) {
    return;
  }
  source_.attach(*storage_, file_);
  source_.clear_error();
  importer_.begin(source_, arena_ + used_, arena_samples_ - used_);
  stage_ = Stage::WavRead;
}

void OgSampleLoader::wav_read(std::size_t room) noexcept {
  const formats::wav::Progress p = importer_.step(room);
  if (p == formats::wav::Progress::Running) {
    return;
  }
  storage_->close(file_);
  stage_ = Stage::WavNext;
  const std::uint8_t slot = slot_;
  if (p == formats::wav::Progress::Failed) {
    if (source_.error() != Status::Ok) {
      card_failed(source_.error());  // the card failed under the importer: that is the card's error, not the file's
    } else if (importer_.report().error == formats::wav::Error::LimitExceeded) {
      settle(slot, SampleProblem::NoRoom);  // the audio does not fit what is left (the importer says so before it writes a byte); a smaller one may
    } else {
      settle(slot, SampleProblem::Unreadable);
    }
    return;
  }
  const formats::wav::Info& info = importer_.info();
  // An OG folder takes 44,100 Hz only. A native project's WAV is one this OS wrote with the instrument's own sample_rate in the header (save_service: 0 is 44,100), and plays
  // at that rate's pitch (OS-RATEPITCH); a file of any other rate than the instrument stores is not the one that was saved.
  const model::Instrument* const in = project_->instrument(slot);
  const std::uint32_t stored = in != nullptr && in->sample_rate != 0u ? in->sample_rate : kSampleRate;
  const bool rate_ok = info.sample_rate == kSampleRate || (native_ && info.sample_rate == stored);
  if (info.frames == 0u || info.channels != 1u || !rate_ok) {
    settle(slot, SampleProblem::Unusable);
    return;
  }
  m_frames_ = info.frames;
  m_rate_ = info.sample_rate;
  m_stamp_ = hal::storage::FileStamp{};
  if (bank_ != nullptr && storage_->stat(path_, m_stamp_) != Status::Ok) {
    m_stamp_ = hal::storage::FileStamp{};  // unknown: a remount cannot confirm it, the next save writes the slot
  }
  if (native_ && bank_ != nullptr && in != nullptr && in->stereo != 0u) {  // OS-STEREO (b): a stereo instrument's side file follows its mid; bound together or not at all
    stage_ = Stage::SideNext;
    return;
  }
  bind_and_mark(false);
}

void OgSampleLoader::bind_and_mark(bool side) noexcept {
  bind(slot_, m_frames_, m_rate_, side);
  if (bank_ != nullptr) {
    bank_->mark_on_card(slot_, m_stamp_.size, m_stamp_.stamp);  // read from the project's own folder: a save into it has nothing to write for this slot until the audio changes (OS-FU2), nor after a remount while the file still looks as it did (OS-BUG-REMOUNT)
  }
}

// OS-STEREO (b), S2: the side file of the instrument whose mid was just read, samples/<stem>_s.wav, goes into the arena right after the mid. Anything wrong with it (not there, would not open, not mono, another length
// or rate than the mid, no room) binds the mid alone, counted in the report (stereo_lost); the flag is cleared when the loader finishes (SampleBank::reconcile_stereo). A card fault ends the load as always.
void OgSampleLoader::side_next() noexcept {
  stage_ = Stage::WavNext;
  const model::Instrument* in = project_->instrument(slot_);
  const std::uint32_t even = (m_frames_ + 1u) & ~1u;
  const std::uint32_t rest = arena_samples_ - used_;
  char side_path[model::kSamplePathMax + 8];
  if (in != nullptr && even <= rest && even <= rest - even && stereo_side_path(*project_, slot_, in->sample_path, side_path, sizeof side_path) && join(side_path, nullptr)) {
    const Status s = storage_->open(path_, file_);
    if (s == Status::Ok) {
      source_.attach(*storage_, file_);
      source_.clear_error();
      importer_.begin(source_, arena_ + used_ + even, rest - even);
      stage_ = Stage::SideRead;
      return;
    }
    if (card_fault(s)) {
      card_failed(s);
      return;
    }
  }
  ++report_.stereo_lost;
  bind_and_mark(false);
}

void OgSampleLoader::side_read(std::size_t room) noexcept {
  const formats::wav::Progress p = importer_.step(room);
  if (p == formats::wav::Progress::Running) {
    return;
  }
  storage_->close(file_);
  stage_ = Stage::WavNext;
  if (p == formats::wav::Progress::Failed && source_.error() != Status::Ok) {
    card_failed(source_.error());
    return;
  }
  const formats::wav::Info& info = importer_.info();
  const bool match = p == formats::wav::Progress::Done && info.frames == m_frames_ && info.source_channels == 1u && info.sample_rate == m_rate_;
  if (!match) {
    ++report_.stereo_lost;
  }
  bind_and_mark(match);
}

// ---- a PcmFeed: the next sample instrument, then its frames in pieces ------------------------------------------------------------------------
void OgSampleLoader::feed_next() noexcept {
  while (cursor_ < kSlots && (((settled_ >> cursor_) & 1u) != 0u || !is_sample_slot(cursor_))) {
    ++cursor_;  // at most 64 steps
  }
  if (cursor_ >= kSlots) {
    stage_ = Stage::Finish;
    return;
  }
  const std::uint8_t slot = cursor_;
  slot_ = slot;
  const std::uint32_t frames = feed_->frames(slot);
  if (frames == 0u) {
    settle(slot, SampleProblem::NotFound);  // the module has no audio for it
    return;
  }
  if (frames > arena_samples_) {
    settle(slot, SampleProblem::NoRoom);  // (before the rounding below: a count near 2^32 would wrap to 0)
    return;
  }
  const model::Instrument* in = project_->instrument(slot);
  const std::uint32_t need = ((frames + 1u) & ~1u) + (in != nullptr ? slice_table_samples(*in) : 0u);
  if (need > arena_samples_ - used_) {
    settle(slot, SampleProblem::NoRoom);
    return;
  }
  pti_left_ = frames;  // (the frames still to read; pti_frames_ the whole)
  pti_frames_ = frames;
  stage_ = Stage::FeedRead;
}

void OgSampleLoader::feed_read(std::size_t room) noexcept {
  std::uint32_t want = static_cast<std::uint32_t>(room / 2u);
  want = want < 256u ? 256u : want;
  want = want > pti_left_ ? pti_left_ : want;
  want = want > kFeedChunkFrames ? kFeedChunkFrames : want;
  const std::uint32_t first = pti_frames_ - pti_left_;
  if (!feed_->read(slot_, first, want, arena_ + used_ + first)) {
    settle(slot_, SampleProblem::Unreadable);
    stage_ = Stage::FeedNext;
    return;
  }
  pti_left_ -= want;
  pti_bytes_ += want * 2u;
  if (pti_left_ == 0u) {
    bind(slot_, pti_frames_, feed_->rate(slot_));
    stage_ = Stage::FeedNext;
  }
}

void OgSampleLoader::finish() noexcept {
  close_all();
  if (bank_ != nullptr) {
    bank_->set_used(used_);
    (void)bank_->reconcile_stereo();  // OS-STEREO (b), rule 4: a flag with no S in the bank is cleared (a mono load; the document is the load's own)
  }
  for (unsigned slot = 0; slot < kSlots; ++slot) {
    if (is_sample_slot(slot) && ((settled_ >> slot) & 1u) == 0u) {
      settle(static_cast<std::uint8_t>(slot), SampleProblem::Unreadable);  // only after a card error: nothing else leaves a slot without its turn
    }
  }
  state_ = State::Done;
}

// ---- Text ---------------------------------------------------------------------------------------------------------------------------------
std::size_t describe_samples(const SampleLoadReport& r, char* out, std::size_t cap) noexcept {
  Line line(out, cap);
  line.str("samples ").dec(r.loaded).str(" loaded, ").dec(r.missing).str(" missing, ").dec((r.samples * 2u + 512u) / 1024u).str(" KB");
  if (r.no_room != 0u) {
    line.str(", ").dec(r.no_room).str(" no room");
  }
  if (r.stereo_lost != 0u) {
    line.str(", ").dec(r.stereo_lost).str(" stereo lost");
  }
  if (r.storage_error != Status::Ok) {
    line.str(", ").str(hal::storage::status_name(r.storage_error));
  }
  return line.size();
}

// OS-BUG-PASTE24: what a `diag inst` line ends with, on every branch: the law the document's volumes are read through (1114: the 1.1.14 table, 12: the 1.2+ law); the bank's and the voices' copies of
// it, only when one differs from the document's (a drift: a rebuild reads the document's, a live volume edit the voices'); and the demo kit while the slots hold it.
static void inst_tail(Line& line, int law, int bank_law, int voices_law, bool demo) noexcept {
  if (law >= 0) {
    line.str(" law ").dec(static_cast<unsigned>(law));
  }
  if (bank_law >= 0 && bank_law != law) {
    line.str(" BANK-LAW ").dec(static_cast<unsigned>(bank_law));
  }
  if (voices_law >= 0 && voices_law != law) {
    line.str(" VOICES-LAW ").dec(static_cast<unsigned>(voices_law));
  }
  if (demo) {
    line.str(" demo");
  }
}

// " vol 50 gain +0.0 dB": the stored level and the gain Q16 it is (dB to a tenth, "-inf dB" for a silent one). The one format of the bound and the unbound line, which the HIL parsers read.
static void vol_gain(Line& line, unsigned volume, std::int32_t gain) noexcept {
  line.str(" vol ").dec(volume).str(" gain ");
  if (gain <= 0) {
    line.str("-inf");
  } else {
    // dB = 20 log10(g / 65536) = 6.0206 log2(g / 65536), to a tenth (a round half away from zero; the main loop, not the render context)
    const float tenths = 60.206f * core::log2_fast(static_cast<float>(gain) * (1.0f / 65536.0f));
    const std::uint32_t t = static_cast<std::uint32_t>((tenths < 0.0f ? -tenths : tenths) + 0.5f);
    line.str(tenths < 0.0f && t != 0u ? "-" : "+").dec(t / 10u).str(".").dec(t % 10u);
  }
  line.str(" dB");
}

std::size_t describe_instrument(unsigned slot, const model::Instrument* in, const audio::SampleDesc& d, char* out, std::size_t cap, int law, bool demo, int bank_law, int voices_law) noexcept {
  Line line(out, cap);
  const unsigned n = slot + 1u;
  line.str("inst ").str(n < 10u ? "0" : "").dec(n).str(": ");
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    line.str("not a sample instrument");
    inst_tail(line, law, bank_law, voices_law, demo);
    return line.size();
  }
  line.str("mode ").dec(in->sampler.play_mode);
  if (d.length == 0u || d.data == nullptr) {
    line.str(", silent: nothing is bound");
    // The level of an instrument with no PCM (an OG fixture, a slot before its load): the stored volume, and the gain the law makes of it (no descriptor holds one yet); the descriptor's own when no law is passed.
    std::int32_t gain = d.gain;
    if (law >= 0) {
      gain = audio::level_gain_q16(in->sampler.volume, law == 1114 ? audio::LevelLaw::Og1114 : audio::LevelLaw::Og12);
      gain = gain > audio::kernel::kMaxGain ? audio::kernel::kMaxGain : gain;
    }
    vol_gain(line, in->sampler.volume, gain);
    inst_tail(line, law, bank_law, voices_law, demo);
    return line.size();
  }
  std::uint32_t sum = 0;  // the 16-bit sum of the first 64 samples of the window, as unsigned halfwords: the bench compares it with the file's
  for (std::uint32_t i = 0; i < 64u && i < d.length; ++i) {
    sum += static_cast<std::uint16_t>(d.data[i]);
  }
  line.str(" start ").dec(d.offset).str(" len ").dec(d.length).str(" loop ").dec(d.loop_start).str("..").dec(d.loop_end);
  line.str(" root ").dec(audio::kRootNote).str(" rate ").dec(in->sample_rate != 0u ? in->sample_rate : kSampleRate);
  vol_gain(line, in->sampler.volume, d.gain);
  if (d.beat_slice) {
    line.str(" beat slices ").dec(d.slice_count).str(" first-note ").dec(audio::kSliceRootNote);
  }
  if (d.wt_size != 0u) {
    line.str(" wt ").dec(d.wt_size);
    if (d.wt_windows != 0u) {
      line.str(" win ").dec(d.wt_pos).str("/").dec(d.wt_windows);  // OS-GRAN: the window bound, of how many
    }
  }
  line.str(" sum ").hex(sum, 4);
  inst_tail(line, law, bank_law, voices_law, demo);
  return line.size();
}

std::size_t describe_problem(const SampleLoadReport::Problem& p, char* out, std::size_t cap) noexcept {
  Line line(out, cap);
  const unsigned n = static_cast<unsigned>(p.slot) + 1u;
  line.str("load: instrument ").str(n < 10u ? "0" : "").dec(n).str(" sample ").str(sample_problem_name(p.why));
  return line.size();
}

}  // namespace o3os::app
