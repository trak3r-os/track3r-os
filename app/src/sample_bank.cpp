// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/sample_bank.h>

#include <o3os/app/og_samples.h>

#include <o3os/audio/demo_kit.h>
#include <o3os/formats/og.h>
#include <o3os/model/demo.h>

#include <cstring>

namespace o3os::app {

void SampleBank::init(audio::VoicePool& voices, model::ModelArena& project, SampleBankMem& mem, std::int16_t* arena, std::uint32_t arena_samples, std::int16_t* peak_storage,
                      std::size_t peak_elems) noexcept {
  voices_ = &voices;
  voices.set_law(law_);
  voices.bind_stage(&mem.stage);
  project_ = &project;
  mem_ = &mem;
  arena_ = arena;
  arena_samples_ = arena_samples & ~1u;  // every sample starts on an even index: 4-byte aligned, as the importer and the peak cache want
  peak_storage_ = peak_storage;
  peak_elems_ = peak_elems;
  mem.wav_desc = formats::FormatDesc{};
  mem.wav_desc.id = "wav";
  mem.wav_desc.title = "WAV sample";
  mem.wav_desc.names = ".wav";
  mem.wav_desc.kind = formats::FormatKind::Sample;
  mem.wav_desc.reader = &mem.reader;
  mem.wt_desc = formats::FormatDesc{};
  mem.wt_desc.id = "surge-wt";
  mem.wt_desc.title = "Surge wavetable";
  mem.wt_desc.names = ".wt";
  mem.wt_desc.kind = formats::FormatKind::Sample;
  mem.wt_desc.reader = &mem.wt_reader;
  mem.iff_desc = formats::FormatDesc{};
  mem.iff_desc.id = "iff-sample";
  mem.iff_desc.title = "AIFF / 8SVX sample";
  mem.iff_desc.names = ".aif .aiff .aifc .8svx .svx .iff";
  mem.iff_desc.kind = formats::FormatKind::Sample;
  mem.iff_desc.reader = &mem.iff_reader;
  clear();
}

void SampleBank::clear() noexcept {
  demo_kit_ = false;
  if (mem_ == nullptr) {
    return;
  }
  if (mem_->clear_hook != nullptr) {
    mem_->clear_hook(mem_->clear_self);
  }
  for (std::uint8_t& e : engine_) {
    e = 0;  // a new project: the instruments' own defaults
  }
  for (SampleBankMem::Slot& s : mem_->slots) {
    s.pcm = nullptr;
    s.frames = 0;
    s.side = nullptr;
    ++s.version;  // a peak cache of what was there is stale
    s.card_version = 0;
    s.table = nullptr;
    s.bound = nullptr;
    s.own = nullptr;
    s.own_len = 0;
    s.stale = nullptr;
    s.stale_len = 0;
  }
  mem_->reclaim_marks = 0;
  if (voices_ != nullptr) {
    (void)voices_->stage_cancel();  // OS-BUG-SLICEPITCH: the descriptors and tables it names are gone with the arena
  }
  todo_ = 0;
  staged_ = 0;
  staging_ = kSlots;
  busy_seen_ = false;
  mem_->clip_valid = false;  // OS-INSTUI O6: the clipboard's audio lived in the arena that is gone
  mem_->clip_audio = SampleBankMem::Audio{};
  mem_->swaps_done_n = 0;
  mem_->swaps_undone_n = 0;
  hist_slot_ = kSlots;
  undo_n_ = 0;
  redo_n_ = 0;
  spare_n_ = 0;  // (the arena is empty again: the buffers are gone)
  used_ = 0;
  pending_ = 0;
  stale_ = 0;
  quiet_ = false;
  mem_->take_held = false;  // the arena is empty again: a take in progress is gone, and the recorder is told by take_held()
  mem_->take_live = false;
  mem_->take_frames = 0;
  mem_->take_tail = 0;
  mem_->over_pcm = nullptr;  // OS-REC2: the arena is gone, a window of it is no longer what the preview descriptor reads
  mem_->over_frames = 0;
}

void SampleBank::new_project(bool keep_audio) noexcept {
  if (!keep_audio) {
    clear();
    return;
  }
  if (mem_ == nullptr) {
    return;
  }
  if (mem_->clear_hook != nullptr) {
    mem_->clear_hook(mem_->clear_self);  // (the take's memory is forgotten below: a loop that plays it lets go)
  }
  for (SampleBankMem::Slot& s : mem_->slots) {
    s.card_version = 0;
  }
  // The new project has an empty journal and no recorder take: nothing of the old project's own bookkeeping may stay. The swap records name audio blocks of units that are gone (they would never
  // be asked for, and referenced() counts them, so RECLAIM would never win those blocks back), the edit history's steps hold buffers of the pool the same way, and a take in progress or live would
  // still be committable into an instrument of the new project. The audio of the slots, their marks and the arena stay.
  mem_->swaps_done_n = 0;
  mem_->swaps_undone_n = 0;
  hist_slot_ = kSlots;
  undo_n_ = 0;
  redo_n_ = 0;
  mem_->take_held = false;
  mem_->take_live = false;
  mem_->take_frames = 0;
  mem_->take_tail = 0;
  mem_->over_pcm = nullptr;
  mem_->over_frames = 0;
}

void SampleBank::adopt(unsigned slot, std::int16_t* pcm, std::uint32_t frames, std::uint32_t block, std::int16_t* side) noexcept {
  if (slot >= kSlots || mem_ == nullptr) {
    return;
  }
  demo_kit_ = false;  // a slot's audio is replaced: the kit is no longer all there is (install_demo_kit sets it again after its own adopts)
  SampleBankMem::Slot& s = mem_->slots[slot];
  s.pcm = frames != 0u ? pcm : nullptr;
  s.frames = pcm != nullptr ? frames : 0u;
  const std::uint32_t even = (frames + 1u) & ~1u;
  // OS-STEREO (b): S is the block's second half, or it is not S (a pointer anywhere else would make a read past the block): M and S are one unit, so a bad S leaves a mono sample
  s.side = (side != nullptr && s.pcm != nullptr && side == s.pcm + even && arena_ != nullptr && s.pcm >= arena_ && static_cast<std::uint64_t>(s.pcm - arena_) + 2ull * even <= arena_samples_) ? side : nullptr;
  s.bound = s.pcm;  // a loader that adopts has bound the voices itself (the engine is quiet then)
  s.own = nullptr;  // (commit_load() claims the block after this call)
  s.own_len = 0;
  if (block != 0u && s.pcm != nullptr && arena_ != nullptr && s.pcm >= arena_ && block >= (s.side != nullptr ? 2u * even : even) && (block & 1u) == 0u && block <= arena_samples_ &&
      static_cast<std::uint32_t>(s.pcm - arena_) <= arena_samples_ - block) {
    s.own = s.pcm;  // OS-ARENA-RECLAIM: a loader's block of its own, the slot's to reuse
    s.own_len = block;
  }
  s.stale = nullptr;
  mem_->reclaim_marks &= ~(std::uint64_t{1} << slot);
  ++s.version;
}

void SampleBank::set_used(std::uint32_t samples) noexcept {
  const std::uint32_t even = (samples + 1u) & ~1u;
  if (even > used_) {
    used_ = even < arena_samples_ ? even : arena_samples_;
  }
}

unsigned SampleBank::reconcile_stereo() noexcept {
  if (project_ == nullptr || mem_ == nullptr) {
    return 0;
  }
  unsigned cleared = 0;
  for (unsigned slot = 0; slot < kSlots; ++slot) {
    const model::Instrument* in = project_->project.instrument(slot);
    if (in != nullptr && in->stereo != 0u && !has_side(slot)) {
      model::Instrument fresh = *in;
      fresh.stereo = 0;
      if (project_->project.set_instrument(slot, fresh) == model::Status::Ok) {
        ++cleared;
      }
    }
  }
  return cleared;
}

SampleBank::Pcm SampleBank::pcm(unsigned slot) const noexcept {
  if (slot >= kSlots || mem_ == nullptr) {
    return {};
  }
  return Pcm{mem_->slots[slot].pcm, mem_->slots[slot].frames, mem_->slots[slot].side};
}

hal::storage::Status SampleBank::open_wav(hal::storage::Storage& storage, const char* path, WavPlan& plan) noexcept {
  using hal::storage::Status;
  if (wav_file_.is_open() || !ready()) {
    return Status::NoHandle;
  }
  std::uint32_t capacity = 0;
  std::int16_t* pcm = reserve(capacity);
  if (pcm == nullptr) {
    return Status::NoHandle;
  }
  hal::storage::FileHandle file;
  Status st = storage.open(path, file);
  if (st != Status::Ok) {
    return st;
  }
  std::uint32_t size = 0;
  st = storage.size(file, size);
  if (st != Status::Ok) {
    storage.close(file);
    return st;
  }
  std::uint8_t head[12] = {};  // OS-SAMPFMT: the first bytes pick the reader; the file is then read from its start again
  std::size_t got = 0;
  st = storage.read(file, head, sizeof head, got);
  if (st == Status::Ok) {
    st = storage.seek(file, 0);
  }
  if (st != Status::Ok) {
    storage.close(file);
    return st;
  }
  plan.format = nullptr;
  if (got >= 4 && std::memcmp(head, "RIFF", 4) == 0) {
    plan.format = &mem_->wav_desc;
  } else if (formats::iff::sniff(head, got, size)) {
    plan.format = &mem_->iff_desc;
  } else if (got >= 4 && std::memcmp(head, "vawt", 4) == 0) {
    plan.format = &mem_->wt_desc;
  }
  wav_storage_ = &storage;
  wav_file_ = file;
  mem_->wav_source.reset();
  mem_->wav_source.attach(storage, file);
  plan.source = &mem_->wav_source;
  plan.pcm = pcm;
  plan.pcm_capacity = capacity;
  plan.size = size;
  return Status::Ok;
}

void SampleBank::close_wav() noexcept {
  if (wav_storage_ != nullptr && wav_file_.is_open()) {
    wav_storage_->close(wav_file_);
  }
  wav_file_ = hal::storage::FileHandle{};
  mem_->wav_source.reset();
}

std::int16_t* SampleBank::reserve(std::uint32_t& capacity) noexcept {
  capacity = free_samples();
  return (arena_ != nullptr && capacity != 0u) ? arena_ + used_ : nullptr;
}

bool SampleBank::commit_load(unsigned slot, std::int16_t* pcm, std::uint32_t frames, std::int16_t* side) noexcept {
  const std::uint32_t even = (frames + 1u) & ~1u;
  const std::uint32_t block = side != nullptr ? 2u * even : even;  // OS-STEREO (b): M and S are one block
  if (slot >= kSlots || mem_ == nullptr || arena_ == nullptr || pcm != arena_ + used_ || frames == 0u || even > free_samples() || (side != nullptr && (side != pcm + even || even > free_samples() - even))) {
    return false;
  }
  if (hist_slot_ == slot) {  // OS-BUG-SLDOVER: the sample edit's history is of the audio this load replaces (set_slot_audio does the same)
    hist_slot_ = kSlots;
    undo_n_ = 0;
    redo_n_ = 0;
  }
  SampleBankMem::Slot& s = mem_->slots[slot];
  // The block this replaces is reusable when it is the slot's own block (a load's or a loader's) and no other holder reads it; reclaim() decides at the handover whether the new audio goes
  // down over it (RECLAIM, sample_bank.h).
  std::int16_t* const old_own = (s.own != nullptr && s.pcm == s.own && s.own + s.own_len <= pcm && !referenced(s.own, s.own + s.own_len, slot)) ? s.own : nullptr;
  const std::uint32_t old_len = s.own_len;
  // A second load before the handover: the slot's original block (the lowest stale one, with its larger capacity) stays the one to reclaim into, so the newest audio ends up there;
  // the block of the first load stays where it is (a hole until the next project load). reclaim() re-checks every holder of the original block.
  std::int16_t* const first_stale = (s.stale != nullptr && (mem_->reclaim_marks & (std::uint64_t{1} << slot)) != 0u && s.stale < pcm) ? s.stale : nullptr;
  const std::uint32_t first_len = s.stale_len;
  used_ += block;
  adopt(slot, pcm, frames, 0, side);
  s.own = pcm;
  s.own_len = block;
  if (first_stale != nullptr) {
    s.stale = first_stale;
    s.stale_len = first_len;
    mem_->reclaim_marks |= std::uint64_t{1} << slot;
  } else if (old_own != nullptr) {
    s.stale = old_own;
    s.stale_len = old_len;
    mem_->reclaim_marks |= std::uint64_t{1} << slot;
  }
  s.table = nullptr;  // the old table (if any) belonged to the old audio; a new one is made when a beat slice instrument needs it
  request_rebind(slot);
  return true;
}

std::int16_t* SampleBank::alloc(std::uint32_t samples) noexcept {
  const std::uint32_t even = (samples + 1u) & ~1u;
  if (arena_ == nullptr || even == 0u || even > free_samples() || wav_file_.is_open()) {  // (an import is reading into the tail: it is not ours to take)
    return nullptr;
  }
  std::int16_t* p = arena_ + used_;
  used_ += even;
  return p;
}

// ---- The recorder's take ----------------------------------------------------------------------------------------------------------------

void SampleBank::take_drop(bool live_idle) noexcept {
  if (preview_quiet(live_idle)) {
    forget_take();
  }
}

void SampleBank::forget_take() noexcept {
  if (mem_ == nullptr || mem_->take_held) {
    return;
  }
  SampleBankMem& m = *mem_;
  if (m.take_live && used_ == m.take_start + ((m.take_frames + 1u) & ~1u) + m.take_tail) {
    used_ = m.take_start;
  }
  m.take_live = false;
  m.take_tail = 0;
}

std::int16_t* SampleBank::take_begin(std::uint32_t& capacity) noexcept {
  capacity = 0;
  if (!ready() || mem_->take_held || wav_file_.is_open()) {
    return nullptr;
  }
  SampleBankMem& m = *mem_;
  forget_take();  // the last take was not kept: if nothing came after it (nor after its reserved tail) the next one goes over both
  if (used_ >= arena_samples_) {
    return nullptr;
  }
  m.take_start = used_;
  capacity = arena_samples_ - used_;
  used_ = arena_samples_;  // held: nothing else takes arena memory until take_end()
  m.take_held = true;
  return arena_ + m.take_start;
}

void SampleBank::take_end(std::uint32_t frames, std::uint32_t tail) noexcept {
  if (mem_ == nullptr || !mem_->take_held) {
    return;
  }
  SampleBankMem& m = *mem_;
  const std::uint32_t room = arena_samples_ - m.take_start;
  m.take_frames = frames < room ? frames : room;
  const std::uint32_t end = m.take_start + ((m.take_frames + 1u) & ~1u);
  std::uint32_t t = m.take_frames != 0u ? ((tail + 1u) & ~1u) : 0u;
  if (end > arena_samples_ || t > arena_samples_ - end) {  // (the recorder sized the tail with loop_capacity(): this only keeps a wrong caller inside the arena)
    t = end > arena_samples_ ? 0u : (arena_samples_ - end) & ~1u;
  }
  m.take_tail = t;
  used_ = end + t;
  m.take_held = false;
  m.take_live = m.take_frames != 0u;
}

std::uint32_t SampleBank::loop_capacity(std::uint32_t room) noexcept {
  auto footprint = [](std::uint32_t c) noexcept { return static_cast<std::uint64_t>(2u * ((c + 1u) & ~1u)) + undo_bitmap_samples(c); };
  // 2 c + c / 1024 <= room, to within the roundings: start at the estimate and walk to the exact answer (a few steps).
  std::uint32_t c = static_cast<std::uint32_t>(static_cast<std::uint64_t>(room) * 1024u / 2049u);
  while (c != 0u && footprint(c) > room) {
    --c;
  }
  while (footprint(c + 1u) <= room) {
    ++c;
  }
  return c;
}

bool SampleBank::take_crop(std::uint32_t offset, std::uint32_t frames) noexcept {
  if (mem_ == nullptr) {
    return false;
  }
  SampleBankMem& m = *mem_;
  const std::uint32_t at = offset & ~1u;
  if (!m.take_live || m.take_tail != 0u || frames == 0u || at >= m.take_frames || frames > m.take_frames - at) {
    return false;
  }
  std::int16_t* const start = arena_ + m.take_start;
  if (at != 0u) {
    std::memmove(start, start + at, static_cast<std::size_t>(frames) * sizeof(std::int16_t));
  }
  const std::uint32_t old_end = m.take_start + ((m.take_frames + 1u) & ~1u);
  if (used_ == old_end) {  // nothing was allocated after the take: the room behind the window is free again (the strip's MEM gives it back at once)
    used_ = m.take_start + ((frames + 1u) & ~1u);
  }
  m.take_frames = frames;
  return true;
}

bool SampleBank::take_commit(unsigned slot, std::uint32_t offset, std::uint32_t frames) noexcept {
  if (mem_ == nullptr) {
    return false;
  }
  SampleBankMem& m = *mem_;
  const std::uint32_t at = offset & ~1u;
  if (slot >= kSlots || !m.take_live || frames == 0u || at >= m.take_frames || frames > m.take_frames - at) {
    return false;
  }
  adopt(slot, arena_ + m.take_start + at, frames);
  m.slots[slot].table = nullptr;  // as commit_load(): the old table belonged to the old audio
  request_rebind(slot);
  const std::uint32_t end = m.take_start + ((m.take_frames + 1u) & ~1u);
  if (m.take_tail != 0u && used_ == end + m.take_tail) {
    used_ = end;  // OS-LOOPER L2: nothing was allocated after the reserved tail (the loop's undo buffer): it is free again, so the next taker may land on it. PRECONDITION: no render write to the tail is possible any more. L2a/L2b have no writer; L2c's two-step SAVE (spec 3c.2: quiesce, wait for writing() false, then commit) must be in place before one exists
  }
  m.take_tail = 0;
  m.take_live = false;  // the slot owns it now
  return true;
}

// ---- The edit history --------------------------------------------------------------------------------------------------------------------

bool SampleBank::alive(const std::int16_t* p) const noexcept {
  for (const SampleBankMem::Slot& s : mem_->slots) {
    if (s.pcm == p || s.bound == p) {
      return true;
    }
  }
  for (unsigned i = 0; i < undo_n_; ++i) {
    if (mem_->undo[i].pcm == p) {
      return true;
    }
  }
  for (unsigned i = 0; i < redo_n_; ++i) {
    if (mem_->redo[i].pcm == p) {
      return true;
    }
  }
  return pcm_refs(p) != 0u;  // OS-INSTUI S7: the clipboard and the swap records hold blocks as well
}

void SampleBank::drop_oldest_undo() noexcept {
  for (unsigned i = 0; i + 1u < undo_n_; ++i) {
    mem_->undo[i] = mem_->undo[i + 1u];
  }
  if (undo_n_ != 0u) {
    --undo_n_;
  }
}

void SampleBank::set_audio(unsigned slot, std::int16_t* pcm, std::uint32_t frames, std::int16_t* side) noexcept {
  demo_kit_ = false;  // (OS-BUG-PASTE24: another sound in a slot of the kit)
  SampleBankMem::Slot& s = mem_->slots[slot];
  s.pcm = pcm;
  s.frames = frames;
  s.side = pcm != nullptr ? side : nullptr;
  ++s.version;
  request_rebind(slot);
}

std::int16_t* SampleBank::edit_begin(unsigned slot, std::uint32_t frames) noexcept {
  if (mem_ == nullptr || slot >= kSlots || frames == 0u || mem_->slots[slot].pcm == nullptr) {
    return nullptr;
  }
  if (hist_slot_ != slot) {  // another slot: its history starts over (the audio of both stays as it is)
    hist_slot_ = slot;
    undo_n_ = 0;
    redo_n_ = 0;
  }
  for (;;) {
    for (unsigned i = 0; i < spare_n_; ++i) {
      if (mem_->spare_cap[i] >= frames && !alive(mem_->spare[i])) {
        return mem_->spare[i];
      }
    }
    if (spare_n_ < kSpares) {
      const std::uint32_t cap = frames > mem_->slots[slot].frames ? frames : mem_->slots[slot].frames;  // every later edit is no longer than the sample is now
      std::int16_t* p = alloc(cap);
      if (p != nullptr) {
        mem_->spare[spare_n_] = p;
        mem_->spare_cap[spare_n_] = (cap + 1u) & ~1u;
        ++spare_n_;
        return p;
      }
    }
    if (undo_n_ != 0u) {
      drop_oldest_undo();  // its buffer, if it was one of ours, is free now
    } else if (redo_n_ != 0u) {
      redo_n_ = 0;  // the edit would clear them anyway
    } else {
      return nullptr;
    }
  }
}

bool SampleBank::edit_commit(unsigned slot, std::int16_t* buf, std::uint32_t frames, const PointsRec& before, const PointsRec& after) noexcept {
  bool ours = false;
  for (unsigned i = 0; i < spare_n_; ++i) {
    ours = ours || (mem_->spare[i] == buf && mem_->spare_cap[i] >= frames);
  }
  if (slot != hist_slot_ || !ours || frames == 0u || alive(buf)) {
    return false;
  }
  SampleBankMem::Step step;
  step.pcm = mem_->slots[slot].pcm;
  step.frames = mem_->slots[slot].frames;
  step.side = mem_->slots[slot].side;  // the edit's result is mono (S goes with the version it belongs to, and comes back with its undo; slice (d) edits both blocks)
  step.before = before;
  step.after = after;
  redo_n_ = 0;
  if (undo_n_ == kHistory) {
    drop_oldest_undo();
  }
  mem_->undo[undo_n_++] = step;
  set_audio(slot, buf, frames, nullptr);
  return true;
}

bool SampleBank::edit_undo(unsigned slot, PointsRec& restore) noexcept {
  if (slot != hist_slot_ || undo_n_ == 0u) {
    return false;
  }
  SampleBankMem::Step step = mem_->undo[--undo_n_];
  SampleBankMem::Step redo = step;
  redo.pcm = mem_->slots[slot].pcm;  // the version being left is what redo comes back to
  redo.frames = mem_->slots[slot].frames;
  redo.side = mem_->slots[slot].side;
  restore = step.before;
  mem_->redo[redo_n_++] = redo;
  set_audio(slot, step.pcm, step.frames, step.side);
  return true;
}

bool SampleBank::edit_redo(unsigned slot, PointsRec& restore) noexcept {
  if (slot != hist_slot_ || redo_n_ == 0u) {
    return false;
  }
  SampleBankMem::Step step = mem_->redo[--redo_n_];
  SampleBankMem::Step undo = step;
  undo.pcm = mem_->slots[slot].pcm;
  undo.frames = mem_->slots[slot].frames;
  undo.side = mem_->slots[slot].side;
  restore = step.after;
  mem_->undo[undo_n_++] = undo;
  set_audio(slot, step.pcm, step.frames, step.side);
  return true;
}

static_assert(model::kInstEngineModern == audio::kEngineModern + 1 && model::kInstEngineAmiga == audio::kEngineAmiga + 1 && model::kInstEngineBits12 == audio::kEngineBits12 + 1 &&
                  model::kInstEngineBits8 == audio::kEngineBits8 + 1,
              "model::kInstEngine* is audio::kEngine* + 1 (INST 1.2)");

// OS-005h: the engine of a slot (sample_bank.h). The Paula rates are those of period 428 (the unity note of a MED import): PAL 3,546,895 / 428 = 8,287 Hz, NTSC 8,363 Hz.
std::uint8_t SampleBank::engine_for(unsigned slot, const model::Instrument& in) const noexcept {
  const std::uint8_t o = engine_override(slot);
  if (o != kEngineAuto) {
    return o;
  }
  if (in.engine != model::kInstEngineAuto) {
    return static_cast<std::uint8_t>(in.engine - 1u);  // the instrument's own choice (the stored value is the engine + 1)
  }
  const std::uint32_t rate = in.sample_rate;
  const bool plain = in.sampler.play_mode <= 1u;  // one shot or a forward loop: what Paula plays
  return amiga_by_rate_ && plain && (rate == 8287u || rate == 8363u) ? audio::kEngineAmiga : audio::kEngineModern;
}

void SampleBank::set_engine(unsigned slot, std::uint8_t engine) noexcept {
  if (slot >= kSlots) {
    return;
  }
  const std::uint8_t v = engine == kEngineAuto ? std::uint8_t{0} : static_cast<std::uint8_t>(engine + 1u);
  if (engine_[slot] != v) {
    engine_[slot] = v;
    request_rebind(slot);
  }
}

void SampleBank::set_amiga_by_rate(bool on) noexcept {
  if (amiga_by_rate_ != on) {
    amiga_by_rate_ = on;
    for (unsigned slot = 0; slot < kSlots; ++slot) {
      request_rebind(slot);
    }
  }
}

void SampleBank::request_rebind(unsigned slot) noexcept {
  if (slot < kSlots) {
    const std::uint64_t bit = std::uint64_t{1} << slot;
    pending_ |= bit;
    stale_ |= bit;
    todo_ |= bit;      // a new descriptor to stage ...
    staged_ &= ~bit;   // ... the one the render took is not the project's any more
    staging_ = staging_ == slot ? kSlots : staging_;  // (a cell in flight is older than this mark: its take is not credited)
  }
}

void SampleBank::request_rebind_loop(unsigned slot) noexcept {
  if (slot < kSlots) {
    pending_ |= std::uint64_t{1} << slot;
  }
}

bool SampleBank::preview_bind(std::uint32_t frames, std::uint32_t rate_hz, bool live_idle) noexcept {
  if (!preview_ready() || frames == 0u || frames > mem_->preview_cap || !preview_quiet(live_idle)) {
    return false;
  }
  audio::SampleDesc d;
  d.data = mem_->preview_pcm;
  d.length = frames;
  preview_tuning(rate_hz, d.tune, d.fine_tune);
  voices_->set_sample(audio::kPreviewSlot, d);
  mem_->over_pcm = nullptr;
  mem_->over_frames = 0;
  return true;
}

bool SampleBank::preview_bind_over(const std::int16_t* pcm, std::uint32_t frames, std::uint32_t rate_hz, bool live_idle) noexcept {
  if (voices_ == nullptr || mem_ == nullptr || pcm == nullptr || frames == 0u || !preview_quiet(live_idle)) {
    return false;
  }
  audio::SampleDesc d;
  d.data = pcm;
  d.length = frames;
  preview_tuning(rate_hz, d.tune, d.fine_tune);
  voices_->set_sample(audio::kPreviewSlot, d);
  mem_->over_pcm = pcm;
  mem_->over_frames = frames;
  return true;
}

// The descriptor of `slot` from the project's instrument and the PCM of the slot; a slot with no sample instrument or no PCM becomes silent. A beat slice
// instrument's table lives in the arena after everything else (one per slot, made the first time, rewritten in place: it is read by note_on only, and
// this runs with the engine quiet).
void SampleBank::sync_law() noexcept {
  if (project_ == nullptr) {
    return;
  }
  const audio::LevelLaw want = project_law(project_->project);
  if (want != law_ || (voices_ != nullptr && voices_->law() != want)) {
    set_law(want);
  }
}

void SampleBank::rebind_now(unsigned slot) noexcept {
  if (slot >= kSlots || voices_ == nullptr || project_ == nullptr) {
    return;
  }
  sync_law();  // OS-BUG-PASTE24: a rebuild reads the law of the document it rebuilds from
  stale_ &= ~(std::uint64_t{1} << slot);  // the descriptor is the project's again (window, mode, PCM and loop)
  todo_ &= ~(std::uint64_t{1} << slot);   // (voices_->set_sample below also takes back a cell staged for the slot)
  staged_ &= ~(std::uint64_t{1} << slot);
  staging_ = staging_ == slot ? kSlots : staging_;
  SampleBankMem::Slot& s = mem_->slots[slot];
  const model::Instrument* in = project_->project.instrument(slot);
  audio::SampleDesc d;  // silent
  if (in != nullptr && in->kind == model::InstrumentKind::Sample && s.pcm != nullptr && s.frames != 0u) {
    std::int16_t* table = nullptr;
    if (slice_table_samples(*in) != 0u) {
      if (s.table == nullptr) {
        s.table = alloc(kTableSamples);
      }
      table = s.table;
    }
    bool approximated = false;
    d = make_sample_desc(*in, s.pcm, s.frames, &approximated, table, law_);
    d.engine = engine_for(slot, *in);
  }
  voices_->set_sample(slot, d);
  s.bound = d.length != 0u ? s.pcm : nullptr;
  ++rebinds_;
}

// RECLAIM (sample_bank.h): the history's steps (the old versions edit undo and redo would put back) that lie in [from, to).
bool SampleBank::block_in_history(const std::int16_t* from, const std::int16_t* to) const noexcept {
  const auto in = [&](const std::int16_t* p) { return p != nullptr && p >= from && p < to; };  // (a step's S lies in its own block: it is covered too)
  for (unsigned i = 0; i < undo_n_; ++i) {
    if (in(mem_->undo[i].pcm) || in(mem_->undo[i].side)) {
      return true;
    }
  }
  for (unsigned i = 0; i < redo_n_; ++i) {
    if (in(mem_->redo[i].pcm) || in(mem_->redo[i].side)) {
      return true;
    }
  }
  return false;
}

// Called inside a quiet handover, after the slot was rebound to its new audio. The conditions are the ones listed under RECLAIM; a mark that cannot be honoured is dropped by
// the caller (the old block stays where it is).
bool SampleBank::reclaim(unsigned slot) noexcept {
  SampleBankMem::Slot& s = mem_->slots[slot];
  const std::uint32_t even = (s.frames + 1u) & ~1u;
  // OS-STEREO (b), rule 5: a block with S (the slot's new audio) is never moved. A stale block with S is only room: it is released whole, and every holder of M or S is tested below.
  if (s.stale == nullptr || s.pcm == nullptr || s.side != nullptr || s.pcm != s.own || s.stale + s.stale_len > s.pcm || s.pcm + even != arena_ + used_ || s.frames > kReclaimMax ||
      block_in_history(s.stale, s.stale + s.stale_len) || referenced(s.stale, s.stale + s.stale_len, slot) ||
      referenced(s.pcm, s.pcm + even, slot)) {  // (OS-INSTUI S7: another slot, the clipboard or a swap record reads the old block or the new one)
    return false;
  }
  const bool adjacent = s.stale + s.stale_len == s.pcm;  // (a) the old block ends where the new one starts: the arena shrinks to the moved audio
  if (!adjacent && even > s.stale_len) {                 // (b) otherwise the new audio must fit the old block's capacity
    return false;
  }
  std::int16_t* const dst = s.stale;
  const std::uint32_t capacity = adjacent ? even : s.stale_len;
  const std::uint32_t top = adjacent ? static_cast<std::uint32_t>(dst - arena_) + even : static_cast<std::uint32_t>(s.pcm - arena_);  // (b): the load cost nothing
  voices_->set_sample(slot, audio::SampleDesc{});  // silent while the audio moves (nothing can start on it: the window is quiet; a note on an empty slot is ignored)
  std::memmove(dst, s.pcm, static_cast<std::size_t>(s.frames) * sizeof(std::int16_t));
  s.pcm = dst;
  s.own = dst;
  s.own_len = capacity;
  s.stale = nullptr;
  s.stale_len = 0;
  s.table = nullptr;  // (none can exist: the new block was the last of the arena)
  ++s.version;        // the audio is somewhere else: a peak cache of it is stale
  used_ = top;
  rebind_now(slot);
  ++mem_->reclaims;
  return true;
}

// The stage of STAGED DESCRIPTOR (sample_bank.h): the lowest slot that wants one goes to the render's cell. The cell takes one at a time; the render empties it at its next block, and a cell still
// full after kStageTimeoutMs means no render is running: it is given back and the quiet handover below does the work.
bool SampleBank::stage_step(std::uint64_t now_ms, bool params_idle) noexcept {
  if (voices_ == nullptr || mem_ == nullptr || project_ == nullptr) {
    return false;
  }
  if (voices_->stage_busy()) {
    if (!busy_seen_) {
      busy_seen_ = true;
      busy_since_ = now_ms;
    } else if (now_ms - busy_since_ >= kStageTimeoutMs) {
      (void)voices_->stage_cancel();
      busy_seen_ = false;
      staging_ = kSlots;
    }
    return false;
  }
  busy_seen_ = false;
  if (staging_ != kSlots) {  // the cell is free again: the render took it, or it was given back
    if (voices_->stage_takes() != staging_takes_) {
      staged_ |= std::uint64_t{1} << staging_;
    }
    staging_ = kSlots;
  }
  if (todo_ == 0u || !params_idle || wav_file_.is_open() || mem_->take_held) {  // (a slice table is taken from the arena's tail, which an import or a recording holds)
    return false;
  }
  const unsigned slot = static_cast<unsigned>(__builtin_ctzll(todo_));
  const std::uint64_t bit = std::uint64_t{1} << slot;
  todo_ &= ~bit;
  sync_law();
  SampleBankMem::Slot& s = mem_->slots[slot];
  const model::Instrument* in = project_->project.instrument(slot);
  audio::SampleDesc d;  // silent
  std::uint16_t* dst = nullptr;
  unsigned words = 0;
  if (in != nullptr && in->kind == model::InstrumentKind::Sample && s.pcm != nullptr && s.frames != 0u) {
    std::int16_t* table = nullptr;
    if (slice_table_samples(*in) != 0u) {
      if (s.table == nullptr) {
        s.table = alloc(kTableSamples);
      }
      table = s.table != nullptr ? reinterpret_cast<std::int16_t*>(mem_->stage.table) : nullptr;  // built in the cell: the render copies it over the slot's own table with the descriptor
    }
    bool approximated = false;
    d = make_sample_desc(*in, s.pcm, s.frames, &approximated, table, law_);
    d.engine = engine_for(slot, *in);
    if (d.slices != nullptr) {
      dst = reinterpret_cast<std::uint16_t*>(s.table);
      words = 2u * (static_cast<unsigned>(d.slice_count) + 1u);
      d.slices = dst;
    }
  }
  staging_takes_ = voices_->stage_takes();
  if (!voices_->stage_sample(slot, d, mem_->stage.table, words, dst)) {
    todo_ |= bit;
    return false;
  }
  staging_ = slot;
  busy_seen_ = true;
  busy_since_ = now_ms;
  return true;
}

bool SampleBank::poll(std::uint64_t now_ms, bool stopped, bool live_idle, bool jobs_idle, bool params_idle) noexcept {
  (void)stage_step(now_ms, params_idle);  // (not a write the caller redraws for: the screens read rebind_pending())
  if (pending_ == 0u && (mem_ == nullptr || mem_->reclaim_marks == 0u)) {
    quiet_ = false;
    return false;
  }
  if (voices_ == nullptr || !stopped || !live_idle || wav_file_.is_open() || mem_->take_held || voices_->voices_in_use() != 0u) {  // (an import or a recording in progress holds the tail: a slice table would take it)
    quiet_ = false;
    return false;
  }
  if (!quiet_) {
    quiet_ = true;
    quiet_since_ = now_ms;
    return false;
  }
  if (now_ms - quiet_since_ < kSettleMs) {
    return false;
  }
  const std::uint64_t marks = pending_;
  pending_ = 0;
  stale_ = 0;  // (a subset of the marks, all applied below)
  todo_ = 0;
  staged_ = 0;
  for (unsigned slot = 0; slot < kSlots; ++slot) {
    if (((marks >> slot) & 1u) != 0u) {
      rebind_now(slot);
    }
  }
  bool wrote = marks != 0u;
  if (jobs_idle) {  // (a mark waits for a quiet moment with no job running: a job may be reading the audio that would move)
    for (unsigned slot = 0; slot < kSlots && mem_->reclaim_marks != 0u; ++slot) {
      const std::uint64_t bit = std::uint64_t{1} << slot;
      if ((mem_->reclaim_marks & bit) != 0u) {
        mem_->reclaim_marks &= ~bit;
        wrote = reclaim(slot) || wrote;
      }
    }
  }
  quiet_ = false;
  return wrote;
}

// ---- The clipboard and shared audio (OS-INSTUI S7) -----------------------------------------------------------------------------------------------

unsigned SampleBank::pcm_refs(const std::int16_t* p) const noexcept {
  if (p == nullptr || mem_ == nullptr) {
    return 0;
  }
  unsigned n = 0;
  for (const SampleBankMem::Slot& s : mem_->slots) {
    n += s.pcm == p ? 1u : 0u;
  }
  n += (mem_->clip_valid && mem_->clip_audio.pcm == p) ? 1u : 0u;
  for (unsigned i = 0; i < mem_->swaps_done_n; ++i) {
    n += (mem_->swaps_done[i].before.pcm == p ? 1u : 0u) + (mem_->swaps_done[i].after.pcm == p ? 1u : 0u);
  }
  for (unsigned i = 0; i < mem_->swaps_undone_n; ++i) {
    n += (mem_->swaps_undone[i].before.pcm == p ? 1u : 0u) + (mem_->swaps_undone[i].after.pcm == p ? 1u : 0u);
  }
  return n;
}

bool SampleBank::referenced(const std::int16_t* from, const std::int16_t* to, unsigned skip) const noexcept {
  const auto in = [&](const std::int16_t* p) { return p != nullptr && p >= from && p < to; };
  for (unsigned i = 0; i < kSlots; ++i) {
    if (i != skip && (in(mem_->slots[i].pcm) || in(mem_->slots[i].bound) || in(mem_->slots[i].side))) {
      return true;
    }
  }
  if (mem_->clip_valid && (in(mem_->clip_audio.pcm) || in(mem_->clip_audio.side))) {
    return true;
  }
  for (unsigned i = 0; i < mem_->swaps_done_n; ++i) {
    const SampleBankMem::Swap& w = mem_->swaps_done[i];
    if (in(w.before.pcm) || in(w.after.pcm) || in(w.before.side) || in(w.after.side)) {
      return true;
    }
  }
  for (unsigned i = 0; i < mem_->swaps_undone_n; ++i) {
    const SampleBankMem::Swap& w = mem_->swaps_undone[i];
    if (in(w.before.pcm) || in(w.after.pcm) || in(w.before.side) || in(w.after.side)) {
      return true;
    }
  }
  return false;
}

void SampleBank::clip_set(const model::Instrument& in, unsigned slot) noexcept {
  if (mem_ == nullptr) {
    return;
  }
  mem_->clip_inst = in;
  mem_->clip_audio = SampleBankMem::Audio{};
  if (in.kind == model::InstrumentKind::Sample && slot < kSlots) {
    mem_->clip_audio.pcm = mem_->slots[slot].pcm;
    mem_->clip_audio.frames = mem_->slots[slot].pcm != nullptr ? mem_->slots[slot].frames : 0u;
    mem_->clip_audio.side = mem_->slots[slot].pcm != nullptr ? mem_->slots[slot].side : nullptr;
  }
  mem_->clip_valid = true;
}

// The slot reads `a`: its audio, a new version (a peak cache of the old audio is stale), a rebind at the next quiet handover. The block the slot owned (own) stays recorded, so a restore finds it
// owned again; whatever was waiting to be reclaimed below the old audio is dropped (the new audio is not that block).
void SampleBank::set_slot_audio(unsigned slot, const SampleBankMem::Audio& a) noexcept {
  SampleBankMem::Slot& s = mem_->slots[slot];
  if (s.pcm == a.pcm && s.frames == a.frames && s.side == a.side) {
    return;  // the same block (a unit that did not change the audio): nothing moves, no version, no rebind
  }
  s.stale = nullptr;
  s.stale_len = 0;
  mem_->reclaim_marks &= ~(std::uint64_t{1} << slot);
  s.card_version = 0;  // (the project folder does not hold this audio)
  if (hist_slot_ == slot) {  // the sample edit's history is of the audio that was replaced
    hist_slot_ = kSlots;
    undo_n_ = 0;
    redo_n_ = 0;
  }
  set_audio(slot, a.pcm, a.frames, a.side);
}

void SampleBank::push_swap(unsigned slot, const SampleBankMem::Audio& before, const SampleBankMem::Audio& after) noexcept {
  SampleBankMem& m = *mem_;
  m.swaps_undone_n = 0;  // a new unit: the journal drops the redo units, so the swaps of the undone ones are never asked for
  if (m.swaps_done_n == SampleBankMem::kSwaps) {
    for (unsigned i = 0; i + 1u < SampleBankMem::kSwaps; ++i) {
      m.swaps_done[i] = m.swaps_done[i + 1u];
    }
    --m.swaps_done_n;
  }
  SampleBankMem::Swap& w = m.swaps_done[m.swaps_done_n++];
  w.slot = static_cast<std::uint8_t>(slot);
  w.valid = true;
  w.before = before;
  w.after = after;
}

bool SampleBank::paste_audio(unsigned slot) noexcept {
  if (mem_ == nullptr || slot >= kSlots || !mem_->clip_valid) {
    return false;
  }
  SampleBankMem::Slot& s = mem_->slots[slot];
  const SampleBankMem::Audio before{s.pcm, s.frames, s.side};
  // Every paste is one swap on record, also one that leaves the audio alone (a MIDI instrument, a slot that already reads that block): undo and redo pair the units of the journal with the swaps
  // one for one, and a unit without its swap would be given another unit's.
  const SampleBankMem::Audio after = mem_->clip_inst.kind == model::InstrumentKind::Sample ? mem_->clip_audio : before;
  push_swap(slot, before, after);
  set_slot_audio(slot, after);
  return before.pcm != after.pcm || before.frames != after.frames || before.side != after.side;
}

bool SampleBank::commit_load_undoable(unsigned slot, std::int16_t* pcm, std::uint32_t frames) noexcept {
  if (slot >= kSlots || mem_ == nullptr) {
    return false;
  }
  const SampleBankMem::Slot& s = mem_->slots[slot];
  const SampleBankMem::Audio before{s.pcm, s.frames, s.side};
  if (!commit_load(slot, pcm, frames)) {
    return false;
  }
  push_swap(slot, before, SampleBankMem::Audio{mem_->slots[slot].pcm, mem_->slots[slot].frames, mem_->slots[slot].side});
  return true;
}

namespace {

// Takes the newest swap of `slot` out of `from` (newest last); a unit whose swap was pushed out of the stack has none, and is given a mark (valid false) so that it still pairs.
SampleBankMem::Swap take_swap(SampleBankMem::Swap* from, unsigned& n, unsigned slot) noexcept {
  for (unsigned i = n; i-- > 0;) {
    if (from[i].slot == slot) {
      const SampleBankMem::Swap w = from[i];
      for (unsigned k = i; k + 1u < n; ++k) {
        from[k] = from[k + 1u];
      }
      --n;
      return w;
    }
  }
  SampleBankMem::Swap lost;
  lost.slot = static_cast<std::uint8_t>(slot);
  lost.valid = false;
  return lost;
}

void put_swap(SampleBankMem::Swap* to, unsigned& n, const SampleBankMem::Swap& w) noexcept {
  if (n == SampleBankMem::kSwaps) {  // the oldest goes (its unit is found by its mark when it comes)
    for (unsigned k = 0; k + 1u < SampleBankMem::kSwaps; ++k) {
      to[k] = to[k + 1u];
    }
    --n;
  }
  to[n++] = w;
}

}  // namespace

bool SampleBank::audio_undone(unsigned slot) noexcept {
  SampleBankMem& m = *mem_;
  const SampleBankMem::Swap w = take_swap(m.swaps_done, m.swaps_done_n, slot);
  put_swap(m.swaps_undone, m.swaps_undone_n, w);
  if (w.valid) {
    set_slot_audio(slot, w.before);
  }
  return w.valid;
}

bool SampleBank::audio_redone(unsigned slot) noexcept {
  SampleBankMem& m = *mem_;
  const SampleBankMem::Swap w = take_swap(m.swaps_undone, m.swaps_undone_n, slot);
  put_swap(m.swaps_done, m.swaps_done_n, w);
  if (w.valid) {
    set_slot_audio(slot, w.after);
  }
  return w.valid;
}

bool demo_kit_fits(const model::Project& project) noexcept {
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = project.instrument(i);  // (null for an empty slot)
    // A MIDI instrument stops the kit too unless it is in an OG channel slot (48..63, which every OG project holds): a MIDI-only project keeps its slots 0..47, its tempo and its pattern 1.
    if (in != nullptr && (in->kind != model::InstrumentKind::Midi || i < formats::og::kMidiFirstSlot)) {
      return false;
    }
  }
  return true;
}

bool install_demo_kit(SampleBank& bank, audio::VoicePool& voices, model::Project& project, std::int16_t* arena, std::uint32_t arena_samples) noexcept {
  if (voices.has_samples() || !demo_kit_fits(project)) {
    return false;
  }
  audio::DemoKit kit;  // 84 bytes, only while the kit is made
  if (!audio::generate_demo_kit(arena, arena_samples, kit)) {
    return false;
  }
  bank.clear();  // the table was empty: so is the bank; the kit's sounds are the first slots' whole samples
  for (unsigned i = 0; i < audio::kDemoKitSounds; ++i) {
    voices.set_sample(i, kit.sample[i]);
    bank.adopt(i, arena + (kit.sample[i].data - arena), kit.sample[i].length);
  }
  bank.set_used(kit.samples);
  (void)model::demo::add_instruments(project, unity_level(project_law(project)));
  if (model::demo::pattern_is_empty(project, 1)) {
    (void)model::demo::fill_pattern(project, 1);
  }
  bank.set_demo_kit(true);
  return true;
}

}  // namespace o3os::app
