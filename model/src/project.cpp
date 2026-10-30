// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/project.h>

#include <cstring>

namespace o3os::model {

namespace {

// E-SONG-07: what the render copies while the main loop may be changing it (the order list, its length, the op count and the journal; Project::snapshot_order) is written and read as relaxed atomics,
// so the copy is free of data races and ThreadSanitizer can check it (the seqlock's epoch says whether the copy is whole). On the MK66 a relaxed load or store of an aligned word or a byte is a plain
// ldr / ldrb / str / strb: the reader's copy is the same word loads as a memcpy; the writer's shifts are byte stores in a loop instead of a memmove (once per edit, at most 255 of them).
using AliasU32 = std::uint32_t __attribute__((may_alias));  // the journal's four bytes and four bytes of the list as one word (both are word aligned)
inline std::uint32_t load_word(const void* p) noexcept { return __atomic_load_n(static_cast<const AliasU32*>(p), __ATOMIC_RELAXED); }
inline void store_word(void* p, std::uint32_t v) noexcept { __atomic_store_n(static_cast<AliasU32*>(p), v, __ATOMIC_RELAXED); }
inline std::uint8_t load_byte(const std::uint8_t* p) noexcept { return __atomic_load_n(p, __ATOMIC_RELAXED); }
inline void store_byte(std::uint8_t* p, std::uint8_t v) noexcept { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
inline void store_op(OrderOp* slot, const OrderOp& v) noexcept {
  std::uint32_t w;
  std::memcpy(&w, &v, sizeof w);
  store_word(slot, w);
}

// Stores `n` characters of `src` into a text field of `cap` + 1 bytes, NUL-padded to the end of the field so
// nothing stale survives. False (nothing written) when it does not fit or contains a NUL.
bool store_text(char* dst, std::size_t cap, const char* src, std::size_t n) noexcept {
  if (n > cap || (src == nullptr && n != 0)) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (src[i] == '\0') {
      return false;
    }
  }
  if (n != 0) {
    std::memcpy(dst, src, n);
  }
  std::memset(dst + n, 0, cap + 1 - n);
  return true;
}

bool valid_track(const TrackSlot& t) noexcept {
  return static_cast<std::uint8_t>(t.type) <= static_cast<std::uint8_t>(TrackType::ExternalAudio) &&
         (t.flags & ~kTrackFlagsMask) == 0 && t.volume <= kMaxPercent && t.pan <= kMaxPercent &&
         t.bus <= kMaxOutputBus && field_length(t.name, kTrackNameMax) <= kTrackNameMax;
}

bool valid_master(const MasterSettings& m) noexcept {
  return m.volume <= kMaxPercent && m.dry_volume <= kMaxPercent && m.reverb_volume <= kMaxPercent &&
         m.delay_volume <= kMaxPercent && (m.flags & ~kMasterFlagsMask) == 0;
}

// Where the generation counter of a parameter stored in the Project is (kNoParamSlot for any other).
unsigned generation_slot(core::ParamId id) noexcept {
  const unsigned slot = param_slot(id);
  if (slot == kNoParamSlot) {
    return kNoParamSlot;
  }
  switch (core::param_kind(id)) {
    case core::ParamKind::Instrument:
      return core::param_index(id) * instrument_param::kCount + slot;
    case core::ParamKind::Track:
      return static_cast<unsigned>(kMaxInstruments) * instrument_param::kCount + core::param_index(id) * track_param::kCount + slot;
    case core::ParamKind::Master:
      return static_cast<unsigned>(kMaxInstruments) * instrument_param::kCount + static_cast<unsigned>(kMaxTracks) * track_param::kCount + slot;
    default:
      return kNoParamSlot;
  }
}

// Every step of the pattern empty: one row filled, the other fifteen copied from it.
void fill_empty(Pattern& p) noexcept {
  for (Step& s : p.steps[0]) {
    s = kEmptyStep;
  }
  for (std::uint32_t t = 1; t < kMaxTracks; ++t) {
    std::memcpy(p.steps[t], p.steps[0], sizeof p.steps[0]);
  }
}

}  // namespace

// Declared, not left to the compiler (ADR 0011): the arena is the same size on the 64-bit host and on the
// MK66. Worst case, everything full: 128 patterns x 14,384 B = 1,841,152 B, 64 instruments x 720 B = 46,080 B (OS-STEREO step 2: the stereo byte, +4 B each),
// the 16 KiB unknown-chunk pool, and 15,728 B of header, tracks, song, tables and generation counters (2,520 B) and, since OS-002d,
// the parameters' generations (3,224 x 4 B), their epoch and the project's settings block (348 B; 388 B since OS-006e made room for 24 fields): 13,248 B (13,288), and, since OS-PERSIST2, the session state: 341 B (+3 of padding), and, since OS-APPUX S3, twelve app parameter sets of 164 B instead of six (+984 B).
static_assert(sizeof(Pattern) * kMaxPatterns == 1841152, "pattern pool size");
static_assert(sizeof(Instrument) * kMaxInstruments == 46080, "instrument pool size");
static_assert(kModelArenaBytes == 1933760, "ModelArena size changed: update docs/formats/o3-project.md, docs/architecture/os-tree.md and the mirror in platform/mk66 mem_map.h");

void Project::reset(bool keep_instruments) noexcept {
  order_depth_ = 0;  // E-SONG-07: a reset never runs inside a bracket, and at boot the arena is raw SDRAM; the epoch and the op count are never rewound (a stale value would look like "no change")
  OrderEdit order_scope(*this);
  std::memset(name_, 0, sizeof name_);
  swing_ = kDefaultSwing;
  default_pattern_length_ = kDefaultPatternLength;
  active_tracks_ = kDefaultActiveTracks;
  store_byte(&song_length_, 0);
  pattern_count_ = 0;
  if (!keep_instruments) {  // OS-NEWKEEP: the instrument table, and the module table its Module instruments name, stay as they are
    instrument_count_ = 0;
    module_count_ = 0;
  }
  unknown_filling_ = kNoSlot;
  tempo_x100_ = kDefaultTempoX100;
  master_ = MasterSettings{kDefaultVolume, kDefaultVolume, kDefaultVolume, kDefaultVolume, 0};
  for (TrackSlot& t : tracks_) {
    t = TrackSlot{TrackType::Audio, 0, kDefaultVolume, kDefaultPan, 0, {}};
  }
  for (std::uint8_t& b : song_) {
    store_byte(&b, 0);
  }
  order_log(OrderOp::Reset, 0, 0, 0);
  std::memset(slot_of_id_, kNoSlot, sizeof slot_of_id_);
  if (!keep_instruments) {
    std::memset(modules_, 0, sizeof modules_);
  }
  state_present_ = 0;  // OS-004d
  if (!keep_instruments) {
    volume_law_ = 0;  // OS-BUG-PASTE24: a new document plays at the 1.2+ law; KEEP INSTRUMENTS keeps the instruments and with them the law their levels were read through
  }
  app_params_count_ = 0;
  init_midi_state(midi_);
  init_usb_state(usb_);
  init_mix_state(mix_);
  init_perf_state(perf_);
  init_note_state(note_);  // OS-PERSIST2
  init_mute_state(mute_);
  init_scenes_state(scenes_);
  init_send_fx_state(send_fx_);
  std::memset(app_params_, 0, sizeof app_params_);
  unknown_count_ = 0;
  unknown_used_ = 0;
  unknown_filled_ = 0;
  unknown_dropped_chunks_ = 0;
  unknown_dropped_bytes_ = 0;
  // Pool slots are marked free; their old step data stays where it is and is overwritten by create_pattern.
  for (Pattern& p : patterns_) {
    p.id = 0;
  }
  if (!keep_instruments) {
    for (Instrument& i : instruments_) {
      i.kind = InstrumentKind::None;
    }
  }
  ++generation_;
  names_state_ = kNamesNone;  // OS-FILES-C H15
  ++tracks_generation_;
  ++song_generation_;
  for (std::uint32_t& g : pattern_generation_) {
    g = generation_;
  }
  for (std::uint32_t& g : instrument_generation_) {
    ++g;
  }
  ++param_epoch_;  // every parameter's generation moves (the counters themselves only ever count up)
  settings_.init(core::ParamKind::Project);
}

const Pattern* Project::pattern(std::uint32_t id) const noexcept {
  if (id == 0 || id > kMaxPatternId || slot_of_id_[id] == kNoSlot) {
    return nullptr;
  }
  return &patterns_[slot_of_id_[id]];
}

Pattern* Project::find_pattern(std::uint32_t id) noexcept {
  if (id == 0 || id > kMaxPatternId || slot_of_id_[id] == kNoSlot) {
    return nullptr;
  }
  return &patterns_[slot_of_id_[id]];
}

const Instrument* Project::instrument(std::uint32_t index) const noexcept {
  if (index >= kMaxInstruments || instruments_[index].kind == InstrumentKind::None) {
    return nullptr;
  }
  return &instruments_[index];
}

std::uint32_t Project::pattern_length(std::uint32_t id) const noexcept {
  const Pattern* p = pattern(id);
  if (p == nullptr) {
    return 0;
  }
  std::uint32_t longest = 0;
  for (std::uint32_t t = 0; t < active_tracks_; ++t) {
    longest = p->track_length[t] > longest ? p->track_length[t] : longest;
  }
  return longest;
}

std::uint32_t Project::pattern_generation(std::uint32_t id) const noexcept {
  const Pattern* p = pattern(id);
  return p != nullptr ? pattern_generation_[p - patterns_] : 0;
}

std::uint32_t Project::instrument_generation(std::uint32_t index) const noexcept {
  return index < kMaxInstruments ? instrument_generation_[index] : 0;
}

// A pattern's generation is the project's counter at its last change, not a counter of its own per pool slot:
// a pattern deleted and created again (possibly in another slot) can never show a value it showed before.
void Project::touch_pattern(const Pattern& p) noexcept {
  touch();
  pattern_generation_[&p - patterns_] = generation_;
}

// ---- header, tracks, master ---------------------------------------------------------------------------

Status Project::set_name(const char* text) noexcept {
  return set_name(text, text != nullptr ? std::strlen(text) : 0);
}

Status Project::set_name(const char* text, std::size_t len) noexcept {
  if (!store_text(name_, kProjectNameMax, text, len)) {
    return Status::BadValue;
  }
  ++tracks_generation_;
  touch();
  return Status::Ok;
}

Status Project::set_tempo_x100(std::uint32_t value) noexcept {
  if (value < kMinTempoX100 || value > kMaxTempoX100) {
    return Status::BadValue;
  }
  tempo_x100_ = value;
  ++tracks_generation_;
  touch();
  return Status::Ok;
}

Status Project::set_swing(std::uint32_t percent) noexcept {
  if (percent < kMinSwing || percent > kMaxSwing) {
    return Status::BadValue;
  }
  swing_ = static_cast<std::uint8_t>(percent);
  ++tracks_generation_;
  touch();
  return Status::Ok;
}

Status Project::set_default_pattern_length(std::uint32_t steps) noexcept {
  if (steps < kMinPatternLength || steps > kMaxPatternLength) {
    return Status::BadValue;
  }
  default_pattern_length_ = static_cast<std::uint8_t>(steps);
  ++tracks_generation_;
  touch();
  return Status::Ok;
}

Status Project::set_active_tracks(std::uint32_t count) noexcept {
  if (count < 1 || count > kMaxTracks) {
    return Status::BadValue;
  }
  active_tracks_ = static_cast<std::uint8_t>(count);
  ++tracks_generation_;
  touch();
  return Status::Ok;
}

Status Project::set_master(const MasterSettings& value) noexcept {
  if (!valid_master(value)) {
    return Status::BadValue;
  }
  for (unsigned f = 0; f < master_param::kCount; ++f) {
    const core::ParamId id = core::param_id(core::ParamKind::Master, 0, f);
    ParamField pf;
    if (find_param(id, pf) && param_load(&master_, pf) != param_load(&value, pf)) {
      ++param_generation_[generation_slot(id)];
    }
  }
  master_ = value;
  ++tracks_generation_;
  touch();
  return Status::Ok;
}

Status Project::set_track(std::uint32_t slot, const TrackSlot& value) noexcept {
  if (slot >= kMaxTracks) {
    return Status::BadIndex;
  }
  if (!valid_track(value)) {
    return Status::BadValue;
  }
  TrackSlot& t = tracks_[slot];
  for (unsigned f = 0; f < track_param::kCount; ++f) {
    const core::ParamId id = core::param_id(core::ParamKind::Track, slot, f);
    ParamField pf;
    if (find_param(id, pf) && param_load(&t, pf) != param_load(&value, pf)) {
      ++param_generation_[generation_slot(id)];
    }
  }
  t = value;
  const std::size_t n = field_length(value.name, kTrackNameMax);
  std::memset(t.name + n, 0, kTrackNameMax + 1 - n);  // nothing stale after the terminator
  ++tracks_generation_;
  touch();
  return Status::Ok;
}

Status Project::set_track_name(std::uint32_t slot, const char* text, std::size_t len) noexcept {
  if (slot >= kMaxTracks) {
    return Status::BadIndex;
  }
  if (!store_text(tracks_[slot].name, kTrackNameMax, text, len)) {
    return Status::BadValue;
  }
  ++tracks_generation_;
  touch();
  return Status::Ok;
}

// ---- song ---------------------------------------------------------------------------------------------

Status Project::append_order(std::uint32_t pattern_id) noexcept {
  return insert_order(song_length_, pattern_id);
}

Status Project::insert_order(std::uint32_t pos, std::uint32_t pattern_id) noexcept {
  if (pattern_id < 1 || pattern_id > kMaxPatternId) {
    return Status::BadValue;
  }
  if (pos > song_length_) {
    return Status::BadIndex;
  }
  if (song_length_ >= kMaxSongPositions) {
    return Status::Full;
  }
  OrderEdit scope(*this);
  for (std::uint32_t i = song_length_; i > pos; --i) {
    store_byte(&song_[i], song_[i - 1u]);
  }
  store_byte(&song_[pos], static_cast<std::uint8_t>(pattern_id));
  store_byte(&song_length_, static_cast<std::uint8_t>(song_length_ + 1u));
  order_log(OrderOp::Insert, pos, 0, 1);
  ++song_generation_;
  touch();
  return Status::Ok;
}

Status Project::set_order(std::uint32_t pos, std::uint32_t pattern_id) noexcept {
  if (pattern_id < 1 || pattern_id > kMaxPatternId) {
    return Status::BadValue;
  }
  if (pos >= song_length_) {
    return Status::BadIndex;
  }
  OrderEdit scope(*this);  // (no journal op: no index changes; the epoch moves so the render takes the new id)
  store_byte(&song_[pos], static_cast<std::uint8_t>(pattern_id));
  ++song_generation_;
  touch();
  return Status::Ok;
}

Status Project::remove_order(std::uint32_t pos) noexcept {
  if (pos >= song_length_) {
    return Status::BadIndex;
  }
  OrderEdit scope(*this);
  for (std::uint32_t i = pos; i + 1u < song_length_; ++i) {
    store_byte(&song_[i], song_[i + 1u]);
  }
  store_byte(&song_length_, static_cast<std::uint8_t>(song_length_ - 1u));
  store_byte(&song_[song_length_], 0);
  order_log(OrderOp::Remove, pos, 0, 1);
  ++song_generation_;
  touch();
  return Status::Ok;
}

Status Project::clear_order() noexcept {
  if (song_length_ == 0) {
    return Status::Ok;
  }
  OrderEdit scope(*this);
  for (std::uint8_t& b : song_) {
    store_byte(&b, 0);
  }
  store_byte(&song_length_, 0);
  order_log(OrderOp::Reset, 0, 0, 0);
  ++song_generation_;
  touch();
  return Status::Ok;
}

Status Project::move_order(std::uint32_t from, std::uint32_t to) noexcept {
  if (from >= song_length_ || to >= song_length_) {
    return Status::BadIndex;
  }
  if (from == to) {
    return Status::Ok;
  }
  OrderEdit scope(*this);
  const std::uint8_t id = song_[from];
  if (from < to) {
    for (std::uint32_t i = from; i < to; ++i) {
      store_byte(&song_[i], song_[i + 1u]);
    }
  } else {
    for (std::uint32_t i = from; i > to; --i) {
      store_byte(&song_[i], song_[i - 1u]);
    }
  }
  store_byte(&song_[to], id);
  order_log(OrderOp::Move, from, to, 1);
  ++song_generation_;
  touch();
  return Status::Ok;
}

// ---- the order list's edit journal (E-SONG-07) -------------------------------------------------------------------------------------------------

// Writer side (the main loop, one writer). A seqlock: the epoch is odd while a change runs; the compiler fence and the release fence keep the odd epoch before the first write of the list and
// the even one after the last (on the MK66 the render preempts the main loop on one core, so what must not happen is a reorder, not a second core).
void Project::order_begin() noexcept {
  if (order_depth_++ == 0) {
    __atomic_store_n(&order_epoch_, __atomic_load_n(&order_epoch_, __ATOMIC_RELAXED) | 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    order_open_ = order_ops_;
  }
}

void Project::order_end() noexcept {
  if (--order_depth_ == 0) {
    __atomic_store_n(&order_epoch_, (__atomic_load_n(&order_epoch_, __ATOMIC_RELAXED) | 1u) + 1u, __ATOMIC_RELEASE);
  }
}

// One op of the change that runs. Inside one bracket an op that touches the one before it joins it (a range delete or paste, the undo of one: a run of single inserts or removes), so such a
// change is one op, whatever its length; the render cannot have read it (the epoch is odd). The merge keeps the effect on every index the same as the two ops one after the other.
void Project::order_log(std::uint8_t kind, std::uint32_t a, std::uint32_t b, std::uint32_t n) noexcept {
  if (order_ops_ != order_open_) {
    OrderOp* slot = &order_journal_[(order_ops_ - 1u) % kOrderJournal];
    OrderOp last = *slot;  // (the writer reads its own words)
    if (kind == OrderOp::Insert && last.kind == OrderOp::Insert && a >= last.a && a <= last.a + last.n && last.n + n <= 255u) {
      last.n = static_cast<std::uint8_t>(last.n + n);
      store_op(slot, last);
      return;
    }
    if (kind == OrderOp::Remove && last.kind == OrderOp::Remove && last.n + n <= 255u && (a == last.a || a + n == last.a)) {
      last.a = static_cast<std::uint8_t>(a);
      last.n = static_cast<std::uint8_t>(last.n + n);
      store_op(slot, last);
      return;
    }
  }
  store_op(&order_journal_[order_ops_ % kOrderJournal],
           OrderOp{kind, static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(n)});
  store_word(&order_ops_, order_ops_ + 1u);
}

// Reader side (the render): a copy of the list and the journal that belongs to an even epoch, or false (an edit runs, or ran during the copy: the caller tries again at the next block; it must
// not wait, the writer is the thread it preempted).
bool Project::snapshot_order(OrderSnapshot& out, void (*between)(void*) noexcept, void* ctx) const noexcept {
  const std::uint32_t e = __atomic_load_n(&order_epoch_, __ATOMIC_ACQUIRE);
  if ((e & 1u) != 0u) {
    return false;
  }
  out.len = load_byte(&song_length_);
  constexpr std::uint32_t kListWords = kMaxSongPositions / 4u;  // (song_ is word aligned; the bytes after the last whole word one by one)
  for (std::uint32_t i = 0; i < kListWords; ++i) {
    const std::uint32_t w = load_word(song_ + 4u * i);
    std::memcpy(out.list + 4u * i, &w, sizeof w);
  }
  for (std::uint32_t i = 4u * kListWords; i < kMaxSongPositions; ++i) {
    out.list[i] = load_byte(&song_[i]);
  }
  out.ops = load_word(&order_ops_);
  for (std::uint32_t i = 0; i < kOrderJournal; ++i) {
    const std::uint32_t w = load_word(&order_journal_[i]);
    std::memcpy(&out.journal[i], &w, sizeof w);
  }
  if (between != nullptr) {
    between(ctx);
  }
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if (__atomic_load_n(&order_epoch_, __ATOMIC_RELAXED) != e) {
    return false;
  }
  out.epoch = e;
  return true;
}

// ---- patterns -----------------------------------------------------------------------------------------

Status Project::create_pattern(std::uint32_t id, std::uint32_t length) noexcept {
  if (id < 1 || id > kMaxPatternId) {
    return Status::BadIndex;
  }
  if (length < kMinPatternLength || length > kMaxPatternLength) {
    return Status::BadValue;
  }
  if (slot_of_id_[id] != kNoSlot) {
    return Status::Exists;
  }
  std::uint32_t slot = 0;
  while (slot < kMaxPatterns && patterns_[slot].id != 0) {
    ++slot;
  }
  if (slot == kMaxPatterns) {
    return Status::Full;
  }
  Pattern& p = patterns_[slot];
  p.id = static_cast<std::uint8_t>(id);
  std::memset(p.track_length, static_cast<int>(length), sizeof p.track_length);
  std::memset(p.name, 0, sizeof p.name);
  fill_empty(p);
  slot_of_id_[id] = static_cast<std::uint8_t>(slot);
  ++pattern_count_;
  names_state_ = names_state_ == kNamesFresh ? kNamesStale : names_state_;  // OS-FILES-C H15
  touch_pattern(p);
  return Status::Ok;
}

Status Project::delete_pattern(std::uint32_t id) noexcept {
  Pattern* p = find_pattern(id);
  if (p == nullptr) {
    return Status::NotFound;
  }
  touch_pattern(*p);
  p->id = 0;
  slot_of_id_[id] = kNoSlot;
  --pattern_count_;
  names_state_ = names_state_ == kNamesFresh ? kNamesStale : names_state_;  // OS-FILES-C H15
  return Status::Ok;
}

Status Project::set_pattern_name(std::uint32_t id, const char* text, std::size_t len) noexcept {
  Pattern* p = find_pattern(id);
  if (p == nullptr) {
    return Status::NotFound;
  }
  if (!store_text(p->name, kPatternNameMax, text, len)) {
    return Status::BadValue;
  }
  touch_pattern(*p);
  return Status::Ok;
}

Status Project::set_track_length(std::uint32_t id, std::uint32_t track, std::uint32_t length) noexcept {
  Pattern* p = find_pattern(id);
  if (p == nullptr) {
    return Status::NotFound;
  }
  if (track >= kMaxTracks) {
    return Status::BadIndex;
  }
  if (length < kMinPatternLength || length > kMaxPatternLength) {
    return Status::BadValue;
  }
  p->track_length[track] = static_cast<std::uint8_t>(length);
  touch_pattern(*p);
  return Status::Ok;
}

Status Project::set_pattern_length(std::uint32_t id, std::uint32_t length) noexcept {
  Pattern* p = find_pattern(id);
  if (p == nullptr) {
    return Status::NotFound;
  }
  if (length < kMinPatternLength || length > kMaxPatternLength) {
    return Status::BadValue;
  }
  std::memset(p->track_length, static_cast<int>(length), sizeof p->track_length);
  touch_pattern(*p);
  return Status::Ok;
}

Status Project::clear_pattern(std::uint32_t id) noexcept {
  Pattern* p = find_pattern(id);
  if (p == nullptr) {
    return Status::NotFound;
  }
  fill_empty(*p);
  touch_pattern(*p);
  return Status::Ok;
}

Status Project::set_step(std::uint32_t id, std::uint32_t track, std::uint32_t step, const Step& value) noexcept {
  Pattern* p = find_pattern(id);
  if (p == nullptr) {
    return Status::NotFound;
  }
  if (track >= kMaxTracks || step >= kMaxPatternLength) {
    return Status::BadIndex;
  }
  p->steps[track][step] = value;
  touch_pattern(*p);
  return Status::Ok;
}

Status Project::set_steps(std::uint32_t id, std::uint32_t track, std::uint32_t first, const Step* values,
                          std::uint32_t count) noexcept {
  Pattern* p = find_pattern(id);
  if (p == nullptr) {
    return Status::NotFound;
  }
  if (track >= kMaxTracks || first > kMaxPatternLength || count > kMaxPatternLength - first) {
    return Status::BadIndex;
  }
  if (count == 0) {
    return Status::Ok;  // accepted, changes nothing, bumps nothing
  }
  if (values == nullptr) {
    return Status::BadValue;
  }
  std::memcpy(&p->steps[track][first], values, count * sizeof(Step));
  touch_pattern(*p);
  return Status::Ok;
}

// ---- instruments and modules --------------------------------------------------------------------------

Status Project::set_instrument(std::uint32_t index, const Instrument& value) noexcept {
  if (index >= kMaxInstruments) {
    return Status::BadIndex;
  }
  if (validate_instrument(value, module_count_) != nullptr) {
    return Status::BadValue;
  }
  Instrument& slot = instruments_[index];
  bump_instrument_params(index, slot.kind == InstrumentKind::None ? nullptr : &slot, value);
  if (slot.kind == InstrumentKind::None) {
    ++instrument_count_;
  }
  slot = value;
  // Canonical form: nothing after a terminator or past the blob length, so equal instruments are equal bytes.
  std::size_t n = field_length(value.name, kInstrumentNameMax);
  std::memset(slot.name + n, 0, kInstrumentNameMax + 1 - n);
  n = field_length(value.sample_path, kSamplePathMax);
  std::memset(slot.sample_path + n, 0, kSamplePathMax + 1 - n);
  std::memset(slot.params + value.param_len, 0, kModuleParamBytes - value.param_len);
  std::memset(slot.reserved, 0, sizeof slot.reserved);
  ++instrument_generation_[index];
  touch();
  return Status::Ok;
}

Status Project::clear_instrument(std::uint32_t index) noexcept {
  if (index >= kMaxInstruments) {
    return Status::BadIndex;
  }
  if (instruments_[index].kind == InstrumentKind::None) {
    return Status::NotFound;
  }
  for (unsigned f = 0; f < instrument_param::kCount; ++f) {  // every parameter of the slot is gone: all advance
    ++param_generation_[index * instrument_param::kCount + f];
  }
  instruments_[index].kind = InstrumentKind::None;
  --instrument_count_;
  ++instrument_generation_[index];
  touch();
  return Status::Ok;
}

// ---- the project-level state (OS-004d) ---------------------------------------------------------------------------------------------

namespace {

// Stores `value` into `slot` and marks the part present; a part that is already present with the same value is left alone (no generation).
template <typename T>
bool put_state(T& slot, const T& value, std::uint8_t& present, std::uint8_t bit) noexcept {
  if ((present & bit) != 0 && slot == value) {
    return false;
  }
  slot = value;
  present = static_cast<std::uint8_t>(present | bit);
  return true;
}

}  // namespace

Status Project::set_midi(const MidiState& value) noexcept {
  if (validate_midi_state(value) != nullptr) {
    return Status::BadValue;
  }
  if (put_state(midi_, value, state_present_, kStateMidi)) {
    touch();
  }
  return Status::Ok;
}

Status Project::set_usb(const UsbState& value) noexcept {
  if (validate_usb_state(value) != nullptr) {
    return Status::BadValue;
  }
  if (put_state(usb_, value, state_present_, kStateUsb)) {
    touch();
  }
  return Status::Ok;
}

Status Project::set_mix(const MixState& value) noexcept {
  if (validate_mix_state(value) != nullptr) {
    return Status::BadValue;
  }
  if (put_state(mix_, value, state_present_, kStateMix)) {
    touch();
  }
  return Status::Ok;
}

Status Project::set_perf(const PerfState& value) noexcept {
  if (validate_perf_state(value) != nullptr) {
    return Status::BadValue;
  }
  if (put_state(perf_, value, state_present_, kStatePerf)) {
    touch();
  }
  return Status::Ok;
}

Status Project::set_note(const NoteState& value) noexcept {
  if (validate_note_state(value) != nullptr) {
    return Status::BadValue;
  }
  if (put_state(note_, value, state_present_, kStateNote)) {
    touch();
  }
  return Status::Ok;
}

Status Project::set_mute(const MuteState& value) noexcept {
  if (validate_mute_state(value) != nullptr) {
    return Status::BadValue;
  }
  if (put_state(mute_, value, state_present_, kStateMute)) {
    touch();
  }
  return Status::Ok;
}

Status Project::set_scenes(const ScenesState& value) noexcept {
  if (validate_scenes_state(value) != nullptr) {
    return Status::BadValue;
  }
  if (put_state(scenes_, value, state_present_, kStateScenes)) {
    touch();
  }
  return Status::Ok;
}

Status Project::set_send_fx(const SendFxState& value) noexcept {
  if (validate_send_fx_state(value) != nullptr) {
    return Status::BadValue;
  }
  if (put_state(send_fx_, value, state_present_, kStateSendFx)) {
    touch();
  }
  return Status::Ok;
}

const AppParams* Project::find_app_params(const char* name, std::size_t len) const noexcept {
  for (std::uint32_t i = 0; i < app_params_count_; ++i) {
    if (std::strlen(app_params_[i].name) == len && std::memcmp(app_params_[i].name, name, len) == 0) {
      return &app_params_[i];
    }
  }
  return nullptr;
}

std::uint32_t Project::app_params_bytes() const noexcept {
  std::uint32_t n = 3;  // the version and the count of apps
  for (std::uint32_t i = 0; i < app_params_count_; ++i) {
    n += 4u + static_cast<std::uint32_t>(std::strlen(app_params_[i].name)) + 4u * app_params_[i].count;
  }
  return n;
}

Status Project::set_app_params(const char* name, std::size_t len, std::uint32_t abi_major, std::uint32_t abi_minor, const float* values,
                               std::uint32_t count) noexcept {
  if (name == nullptr || len > kAppNameMax || !is_safe_module_name(name, len) || abi_major > 255 || abi_minor > 255 || values == nullptr || count < 1 ||
      count > kMaxAppParams) {
    return Status::BadValue;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!is_finite_float(values[i])) {
      return Status::BadValue;
    }
  }
  AppParams entry;
  std::memset(&entry, 0, sizeof entry);
  std::memcpy(entry.name, name, len);
  entry.abi_major = static_cast<std::uint8_t>(abi_major);
  entry.abi_minor = static_cast<std::uint8_t>(abi_minor);
  entry.count = static_cast<std::uint8_t>(count);
  std::memcpy(entry.value, values, count * sizeof(float));
  // Where the entry of that name is now (it is replaced and moves to the end), else the end; a full table drops its oldest.
  std::uint32_t from = app_params_count_;
  for (std::uint32_t i = 0; i < app_params_count_; ++i) {
    if (std::strcmp(app_params_[i].name, entry.name) == 0) {
      from = i;
      break;
    }
  }
  if (from == app_params_count_ - 1u && std::memcmp(&app_params_[from], &entry, sizeof entry) == 0) {
    return Status::Ok;  // the newest entry already says this
  }
  if (from == app_params_count_) {
    if (app_params_count_ == kMaxAppParamSets) {
      from = 0;
    } else {
      ++app_params_count_;
      from = app_params_count_ - 1u;
    }
  }
  for (std::uint32_t i = from; i + 1u < app_params_count_; ++i) {
    app_params_[i] = app_params_[i + 1u];
  }
  app_params_[app_params_count_ - 1u] = entry;
  // What the chunk can hold: the oldest go until the entries fit one chunk piece (the newest, just stored, always does: 166 bytes at most).
  while (app_params_count_ > 1u && app_params_bytes() > kAppParamsBytesMax) {
    for (std::uint32_t i = 0; i + 1u < app_params_count_; ++i) {
      app_params_[i] = app_params_[i + 1u];
    }
    --app_params_count_;
    std::memset(&app_params_[app_params_count_], 0, sizeof app_params_[0]);
  }
  touch();
  return Status::Ok;
}

void Project::clear_app_params() noexcept {
  if (app_params_count_ != 0) {
    app_params_count_ = 0;
    std::memset(app_params_, 0, sizeof app_params_);
    touch();
  }
}

Status Project::add_module(const char* name, std::size_t len, std::uint32_t abi_major,
                           std::uint32_t abi_minor) noexcept {
  if (name == nullptr || !is_safe_module_name(name, len) || abi_major > 255 || abi_minor > 255) {
    return Status::BadValue;
  }
  for (std::uint32_t i = 0; i < module_count_; ++i) {
    if (field_length(modules_[i].name, kModuleNameMax) == len && std::memcmp(modules_[i].name, name, len) == 0) {
      return Status::Exists;
    }
  }
  if (module_count_ >= kMaxModules) {
    return Status::Full;
  }
  ModuleRef& m = modules_[module_count_];
  (void)store_text(m.name, kModuleNameMax, name, len);  // length and content checked above
  m.abi_major = static_cast<std::uint8_t>(abi_major);
  m.abi_minor = static_cast<std::uint8_t>(abi_minor);
  ++module_count_;
  touch();
  return Status::Ok;
}

Status Project::clear_modules() noexcept {
  if (module_count_ == 0) {
    return Status::Ok;
  }
  for (const Instrument& i : instruments_) {
    if (i.kind == InstrumentKind::Module) {
      return Status::BadValue;
    }
  }
  std::memset(modules_, 0, sizeof modules_);
  module_count_ = 0;
  touch();
  return Status::Ok;
}

// ---- unknown chunks -----------------------------------------------------------------------------------

Status Project::unknown_begin(std::uint32_t id, std::uint32_t length) noexcept {
  if (!is_printable_fourcc(id)) {
    return Status::BadValue;
  }
  if (unknown_count_ >= kMaxUnknownChunks || length > kUnknownPoolBytes - unknown_used_) {
    ++unknown_dropped_chunks_;
    // Saturating: a hostile length must not wrap the counter.
    unknown_dropped_bytes_ = length > 0xFFFFFFFFu - unknown_dropped_bytes_ ? 0xFFFFFFFFu : unknown_dropped_bytes_ + length;
    unknown_filling_ = kNoSlot;
    touch();
    return Status::Full;
  }
  unknown_[unknown_count_] = UnknownChunk{id, length, unknown_used_};
  std::memset(unknown_pool_ + unknown_used_, 0, length);
  unknown_filling_ = static_cast<std::uint8_t>(unknown_count_);
  unknown_filled_ = 0;
  ++unknown_count_;
  unknown_used_ += length;
  touch();
  return Status::Ok;
}

Status Project::unknown_append(const std::uint8_t* data, std::uint32_t count) noexcept {
  if (unknown_filling_ == kNoSlot) {
    return Status::BadValue;
  }
  const UnknownChunk& c = unknown_[unknown_filling_];
  if (count > c.length - unknown_filled_ || (data == nullptr && count != 0)) {
    return Status::BadValue;
  }
  if (count != 0) {
    std::memcpy(unknown_pool_ + c.offset + unknown_filled_, data, count);
    unknown_filled_ += count;
  }
  return Status::Ok;
}

void Project::clear_unknown() noexcept {
  unknown_count_ = 0;
  unknown_used_ = 0;
  unknown_filled_ = 0;
  unknown_filling_ = kNoSlot;
  unknown_dropped_chunks_ = 0;
  unknown_dropped_bytes_ = 0;
  touch();
}


// ---- parameters (OS-002d) -----------------------------------------------------------------------------

// The slots' generations that a whole-instrument write changes: the parameters whose value differs between `before` (nullptr: the
// slot was free, so every parameter is new) and `after`.
void Project::bump_instrument_params(std::uint32_t index, const Instrument* before, const Instrument& after) noexcept {
  for (unsigned slot = 0; slot < instrument_param::kCount; ++slot) {
    ParamField f;
    if (!find_param(core::param_id(core::ParamKind::Instrument, index, param_field_of_slot(core::ParamKind::Instrument, slot)), f)) {
      continue;
    }
    if (before == nullptr || param_load(before, f) != param_load(&after, f)) {
      ++param_generation_[index * instrument_param::kCount + slot];
    }
  }
}

namespace {

// A module parameter id (OS-012b): the slot, and the index in the module's blob, or false.
bool module_param_of(core::ParamId id, unsigned& slot, unsigned& index) noexcept {
  if (core::param_kind(id) != core::ParamKind::Instrument || !instrument_param::is_module_field(core::param_field(id)) ||
      core::param_index(id) >= kMaxInstruments) {
    return false;
  }
  slot = core::param_index(id);
  index = core::param_field(id) - instrument_param::kModuleFirst;
  return true;
}

}  // namespace

bool Project::has_param(core::ParamId id) const noexcept {
  unsigned mslot = 0, mindex = 0;
  if (module_param_of(id, mslot, mindex)) {  // a Module instrument whose blob holds the value
    const Instrument& in = instruments_[mslot];
    return in.kind == InstrumentKind::Module && 4u * (mindex + 1u) <= in.param_len;
  }
  ParamField f;
  if (!find_param(id, f)) {
    return false;
  }
  switch (core::param_kind(id)) {
    case core::ParamKind::Instrument:
      return instruments_[core::param_index(id)].kind != InstrumentKind::None;
    case core::ParamKind::Track:
    case core::ParamKind::Master:
    case core::ParamKind::Project:
      return true;
    default:
      return false;
  }
}

float Project::get_param(core::ParamId id) const noexcept {
  unsigned mslot = 0, mindex = 0;
  if (module_param_of(id, mslot, mindex)) {
    float v = 0.0f;
    if (has_param(id)) {
      std::memcpy(&v, instruments_[mslot].params + 4u * mindex, 4);
    }
    return v;
  }
  ParamField f;
  if (!has_param(id) || !find_param(id, f)) {
    return 0.0f;
  }
  switch (core::param_kind(id)) {
    case core::ParamKind::Instrument: return param_load(&instruments_[core::param_index(id)], f);
    case core::ParamKind::Track: return param_load(&tracks_[core::param_index(id)], f);
    case core::ParamKind::Master: return param_load(&master_, f);
    case core::ParamKind::Project: return settings_.get(core::param_field(id));
    default: return 0.0f;
  }
}

std::uint32_t Project::param_generation(core::ParamId id) const noexcept {
  if (core::param_kind(id) == core::ParamKind::Project) {
    return settings_.generation(core::param_field(id));
  }
  unsigned mslot = 0, mindex = 0;
  if (module_param_of(id, mslot, mindex)) {
    return param_epoch_ + param_generation_[mslot * instrument_param::kCount + mindex];  // mindex < kModuleMax < kCount
  }
  const unsigned slot = generation_slot(id);
  return slot == kNoParamSlot ? 0u : param_epoch_ + param_generation_[slot];
}

Status Project::set_param(core::ParamId id, float value) noexcept {
  unsigned mslot = 0, mindex = 0;
  if (module_param_of(id, mslot, mindex)) {  // OS-012b: a module's parameter, a float in the instrument's blob
    if (instruments_[mslot].kind != InstrumentKind::Module) {
      return Status::NotFound;
    }
    if (!has_param(id)) {
      return Status::BadIndex;  // the blob does not reach it: the caller writes the instrument whole first
    }
    if (!(value > -3.0e38f && value < 3.0e38f)) {
      return Status::BadValue;  // NaN and infinity
    }
    std::uint8_t* at = instruments_[mslot].params + 4u * mindex;
    float before = 0.0f;
    std::memcpy(&before, at, 4);
    if (before != value) {
      std::memcpy(at, &value, 4);
      ++param_generation_[mslot * instrument_param::kCount + mindex];
      ++instrument_generation_[mslot];
      touch();
    }
    return Status::Ok;
  }
  ParamField f;
  if (!find_param(id, f)) {
    return Status::BadIndex;
  }
  void* owner = nullptr;
  switch (core::param_kind(id)) {
    case core::ParamKind::Instrument:
      if (instruments_[core::param_index(id)].kind == InstrumentKind::None) {
        return Status::NotFound;
      }
      owner = &instruments_[core::param_index(id)];
      break;
    case core::ParamKind::Track:
      owner = &tracks_[core::param_index(id)];
      break;
    case core::ParamKind::Master:
      owner = &master_;
      break;
    case core::ParamKind::Project: {
      const std::uint32_t before = settings_.generation(core::param_field(id));
      const Status st = settings_.set(core::param_field(id), value);
      if (st == Status::Ok && settings_.generation(core::param_field(id)) != before) {
        touch();
      }
      return st;
    }
    default:
      return Status::BadIndex;  // a Device parameter is the device's, not a project's
  }
  const float before = param_load(owner, f);
  unsigned char raw_before[2] = {0, 0};  // OS-INSTUI S6: SOURCE reads two bytes as one value, and a stored enable 0 / loop 1 reads as Off: setting Off rewrites the loop byte to 0, which is a change
  if (f.store == ParamStore::EnvState) {
    std::memcpy(raw_before, static_cast<const unsigned char*>(owner) + f.offset, sizeof raw_before);
  }
  if (!param_store(owner, f, value)) {
    return Status::BadValue;
  }
  if (param_load(owner, f) != before || (f.store == ParamStore::EnvState && std::memcmp(raw_before, static_cast<const unsigned char*>(owner) + f.offset, sizeof raw_before) != 0)) {
    // A parameter that got here exists, so its slot does; the test only tells the compiler what the lookup guarantees (GCC at -O3 cannot
    // see that generation_slot() gives kNoParamSlot, 65535, only for an id that find_param refused, and warned about the index).
    const unsigned gs = generation_slot(id);
    if (gs < kProjectParamSlots) {
      ++param_generation_[gs];
    }
    if (core::param_kind(id) == core::ParamKind::Instrument) {
      ++instrument_generation_[core::param_index(id)];
    } else {
      ++tracks_generation_;
    }
    touch();
  }
  return Status::Ok;
}

Status Project::set_settings(const Settings& value) noexcept {
  const std::uint32_t before_learn = settings_.learn_generation();
  std::uint32_t before[Settings::kMaxFields];
  for (unsigned i = 0; i < Settings::kMaxFields; ++i) {
    before[i] = settings_.generation(i);
  }
  const Status st = settings_.assign(value);
  if (st != Status::Ok) {
    return st;
  }
  bool changed = settings_.learn_generation() != before_learn;
  for (unsigned i = 0; i < Settings::kMaxFields; ++i) {
    changed = changed || settings_.generation(i) != before[i];
  }
  if (changed) {
    touch();
  }
  return Status::Ok;
}

Status Project::set_learn_slot(unsigned slot, const core::LearnBinding& b) noexcept {
  const std::uint32_t before = settings_.learn_generation();
  const Status st = settings_.set_learn(slot, b);
  if (st == Status::Ok && settings_.learn_generation() != before) {
    touch();
  }
  return st;
}

// ---- invariants ---------------------------------------------------------------------------------------

bool Project::check_invariants() const noexcept {
  if (field_length(name_, kProjectNameMax) > kProjectNameMax) return false;
  if (tempo_x100_ < kMinTempoX100 || tempo_x100_ > kMaxTempoX100) return false;
  if (swing_ < kMinSwing || swing_ > kMaxSwing) return false;
  if (default_pattern_length_ < kMinPatternLength || default_pattern_length_ > kMaxPatternLength) return false;
  if (active_tracks_ < 1 || active_tracks_ > kMaxTracks) return false;
  if (!valid_master(master_)) return false;
  for (const TrackSlot& t : tracks_) {
    if (!valid_track(t)) return false;
  }
  for (std::uint32_t i = 0; i < song_length_; ++i) {
    if (song_[i] == 0) return false;
  }
  std::uint32_t used = 0;
  for (std::uint32_t slot = 0; slot < kMaxPatterns; ++slot) {
    const Pattern& p = patterns_[slot];
    if (p.id == 0) continue;
    ++used;
    if (slot_of_id_[p.id] != slot) return false;
    for (std::uint8_t len : p.track_length) {
      if (len < kMinPatternLength || len > kMaxPatternLength) return false;
    }
    if (field_length(p.name, kPatternNameMax) > kPatternNameMax) return false;
  }
  if (used != pattern_count_) return false;
  for (std::uint32_t id = 1; id <= kMaxPatternId; ++id) {
    if (slot_of_id_[id] != kNoSlot && (slot_of_id_[id] >= kMaxPatterns || patterns_[slot_of_id_[id]].id != id)) {
      return false;
    }
  }
  if (module_count_ > kMaxModules) return false;
  for (std::uint32_t i = 0; i < module_count_; ++i) {
    const std::size_t n = field_length(modules_[i].name, kModuleNameMax);
    if (n > kModuleNameMax || !is_safe_module_name(modules_[i].name, n)) return false;
  }
  used = 0;
  for (const Instrument& i : instruments_) {
    if (i.kind == InstrumentKind::None) continue;
    ++used;
    if (validate_instrument(i, module_count_) != nullptr) return false;
  }
  if (used != instrument_count_) return false;
  if (settings_.kind() != core::ParamKind::Project || !settings_.check_invariants()) return false;
  if (app_params_count_ > kMaxAppParamSets || app_params_bytes() > kAppParamsBytesMax) return false;  // (every bit of state_present_ is a part now)
  if (validate_midi_state(midi_) != nullptr || validate_usb_state(usb_) != nullptr || validate_mix_state(mix_) != nullptr || validate_perf_state(perf_) != nullptr) return false;
  if (validate_note_state(note_) != nullptr || validate_mute_state(mute_) != nullptr || validate_scenes_state(scenes_) != nullptr || validate_send_fx_state(send_fx_) != nullptr) return false;
  for (std::uint32_t i = 0; i < app_params_count_; ++i) {
    if (validate_app_params(app_params_[i]) != nullptr) return false;
    for (std::uint32_t j = 0; j < i; ++j) {
      if (std::strcmp(app_params_[i].name, app_params_[j].name) == 0) return false;
    }
  }
  if (unknown_count_ > kMaxUnknownChunks) return false;
  std::uint32_t offset = 0;
  for (std::uint32_t i = 0; i < unknown_count_; ++i) {
    if (!is_printable_fourcc(unknown_[i].id) || unknown_[i].offset != offset ||
        unknown_[i].length > kUnknownPoolBytes - offset) {
      return false;
    }
    offset += unknown_[i].length;
  }
  return offset == unknown_used_;
}

}  // namespace o3os::model
