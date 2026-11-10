// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/editor.h>

#include <cstring>

namespace o3os::model {

namespace detail {

// Sequential reads of one record's payload out of the ring. A read past the end marks the record bad (ok = false) and gives
// zeros, so the caller never uses uninitialised bytes.
struct PayloadReader {
  const UndoJournal& journal;
  std::size_t pos;
  std::size_t left;
  bool ok = true;

  void get(void* dst, std::size_t n) noexcept {
    if (n > left) {
      ok = false;
      std::memset(dst, 0, n);
      return;
    }
    journal.get(pos, dst, n);
    pos = journal.advance(pos, n);
    left -= n;
  }
  void skip(std::size_t n) noexcept {
    if (n > left) {
      ok = false;
      return;
    }
    pos = journal.advance(pos, n);
    left -= n;
  }
  std::uint8_t u8() noexcept {
    std::uint8_t v = 0;
    get(&v, 1);
    return v;
  }
};

}  // namespace detail

namespace {

using detail::PayloadReader;

// What a record is. Only meaningful inside one session's ring (the journal is not persisted), but kept stable anyway.
enum Op : std::uint8_t {
  kSetName = 1,
  kSetTempo,
  kSetSwing,
  kSetDefaultLength,
  kSetActiveTracks,
  kSetMaster,
  kSetTrack,
  kSetTrackName,
  kAppendOrder,
  kInsertOrder,
  kSetOrder,
  kRemoveOrder,
  kClearOrder,
  kCreatePattern,
  kDeletePattern,
  kSetPatternName,
  kSetTrackLength,
  kSetPatternLength,
  kClearPattern,
  kSetStep,
  kSetSteps,
  kSetInstrument,
  kClearInstrument,
  kSetParam,  // OS-002d
  kSetLearn,
  kLaneDelete,  // OS-MIDIREC-P1: u8 id, u8 track, u16 index, u16 count (host order), then count LaneEvents; the reverse inserts them
  kLaneInsert,  // the same payload; the reverse erases them
};

constexpr std::size_t kStepBytes = sizeof(Step);
constexpr std::size_t kRowBytes = kMaxPatternLength * kStepBytes;
constexpr std::size_t kStepsImage = kMaxTracks * kRowBytes;
constexpr std::size_t kDeleteImage = 1 + kMaxTracks + (kPatternNameMax + 1) + kStepsImage;

static_assert(sizeof(Pattern().steps) == kStepsImage, "a pattern's steps are the image the records hold");
static_assert(kDeleteImage <= UndoJournal::kMaxPayloadBytes, "the largest record (delete pattern) fits a record");
static_assert(2 + 2 * sizeof(Instrument) <= UndoJournal::kMaxPayloadBytes, "an instrument record fits a record");
static_assert(sizeof(MasterSettings) == 5 && sizeof(TrackSlot) == 26, "records copy these structs as bytes: no padding");
static_assert(UndoJournal::unit_bytes_for(3 + 2 * kStepBytes) == 48, "a step edit is 48 bytes of journal (editor.h)");
static_assert(UndoJournal::unit_bytes_for(3) == 32, "a song edit is 32 bytes of journal (editor.h)");
static_assert(UndoJournal::unit_bytes_for(1 + kMaxSongPositions) == 284, "clearing a full song is 284 bytes (editor.h)");
static_assert(UndoJournal::unit_bytes_for(2 + 2 * sizeof(Instrument)) == 1472, "an instrument edit is 1,472 bytes (editor.h)");
static_assert(UndoJournal::unit_bytes_for(kDeleteImage) == 14412, "deleting a pattern is 14,412 bytes (editor.h)");
static_assert(UndoJournal::unit_bytes_for(1 + kStepsImage) == 14368, "clearing a pattern is 14,368 bytes (editor.h)");
static_assert(kUndoPatternBytes / UndoJournal::unit_bytes_for(kDeleteImage) >= 11, "a whole-pattern delete keeps 11 levels in PATTERN");
static_assert(kUndoSongBytes / UndoJournal::unit_bytes_for(kDeleteImage) >= 6, "a whole-pattern copy keeps 6 levels in SONG");
static_assert(sizeof(core::LearnBinding) == 8, "a Learn record copies the binding as bytes: no padding");
static_assert(UndoJournal::unit_bytes_for(12) == 40, "a parameter edit is 40 bytes of journal (editor.h)");
static_assert(UndoJournal::unit_bytes_for(1 + 2 * sizeof(core::LearnBinding)) == 48, "a Learn edit is 48 bytes of journal (editor.h)");

// The text of a text record: [length of new][new][length of old][old]. Redo wants the new, undo the old.
std::size_t pick_text(PayloadReader& r, bool undo, char* buf, std::size_t cap) noexcept {
  const std::size_t nl = r.u8();
  if (nl > cap) {
    r.ok = false;
    return 0;
  }
  std::size_t n = nl;
  if (undo) {
    r.skip(nl);
    n = r.u8();
    if (n > cap) {
      r.ok = false;
      return 0;
    }
  }
  r.get(buf, n);
  return n;
}

// Sixteen rows of a pattern's steps back into the pattern.
Status restore_rows(Project& project, PayloadReader& r, std::uint32_t id) noexcept {
  Step row[kMaxPatternLength];
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    r.get(row, sizeof row);
    const Status s = project.set_steps(id, t, 0, row, kMaxPatternLength);
    if (s != Status::Ok) {
      return s;
    }
  }
  return Status::Ok;
}

}  // namespace

// Every public mutator opens one of these: it makes a unit of its own when no group is open, and checks the project has not
// changed behind the Editor's back; when it ends, whatever happened, the journal's idea of the generation is brought up to date.
class Editor::Scope {
 public:
  explicit Scope(Editor& e) noexcept : e_(e), implicit_(e.depth_ == 0) {
    if (implicit_) {
      e_.area_ = UndoArea::Auto;  // the first record chooses the ring (the unit is begun there, lazily)
      e_.pending_action_ = core::ActionId::None;
      e_.pending_arg_ = 0;
      e_.open_ = nullptr;
      e_.muted_ = false;
      e_.hint_ = 0;
    }
    e_.check_sync();
  }
  ~Scope() {
    if (implicit_) {
      e_.close_unit();
    }
    e_.synced();
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Editor& e_;
  bool implicit_;
};

Editor::Editor(Project& project, UndoHistory& history) noexcept : project_(project) {
  for (unsigned i = 0; i < kUndoAreas; ++i) {
    rings_[i] = &history.ring[i];
  }
  history_reset();
}

Editor::Editor(Project& project, UndoJournal& journal) noexcept : project_(project) {
  for (UndoJournal*& r : rings_) {
    r = &journal;
  }
  history_reset();
}

void Editor::check_sync() noexcept {
  if (project_.generation() != synced_) {
    history_reset();
    ++stale_;
  }
}

void Editor::history_reset() noexcept {
  for (UndoJournal* r : rings_) {
    r->clear();
  }
  synced_ = project_.generation();
}

bool Editor::adopt_instrument_load(std::uint32_t slot) noexcept {
  if (depth_ != 0 || open_ != nullptr) {
    const bool had = has_history();
    history_reset();
    return had;
  }
  static const auto names = [](std::uint8_t op, std::uint32_t head, unsigned slot_arg) noexcept {
    switch (op) {
      case kSetInstrument:
      case kClearInstrument: return (head & 0xFFu) == slot_arg;
      case kSetParam: return core::param_kind(static_cast<core::ParamId>(head)) == core::ParamKind::Instrument && core::param_index(static_cast<core::ParamId>(head)) == slot_arg;
      default: return false;
    }
  };
  UndoJournal* sound = rings_[ring_index(UndoArea::Sound)];
  bool lost = false;
  if (sound->any_record(+names, slot)) {
    lost = sound->depth() != 0 || sound->redo_depth() != 0;
    sound->clear();
  }
  synced();
  return lost;
}

bool Editor::has_history() const noexcept {
  for (const UndoJournal* r : rings_) {
    if (r->depth() != 0 || r->redo_depth() != 0) {
      return true;
    }
  }
  return false;
}

void Editor::merge_break() noexcept {
  for (UndoJournal* r : rings_) {
    r->break_group();
  }
}

void Editor::begin_group(core::ActionId action, core::ActionArg arg, UndoArea area) noexcept {
  if (depth_++ == 0) {
    check_sync();
    area_ = area;
    pending_action_ = action;
    pending_arg_ = arg;
    muted_ = area == UndoArea::None;
    hint_ = 0;
    open_ = nullptr;
    if (area == UndoArea::Auto && rings_[0] == rings_[1] && rings_[1] == rings_[2]) {
      area = UndoArea::Pattern;  // one ring for every area: nothing to wait for, the unit is open as it always was
    }
    if (area == UndoArea::Pattern || area == UndoArea::Song || area == UndoArea::Sound) {
      open_ = rings_[ring_index(area)];
      open_->begin_unit(action, arg);
    }
  }
}

void Editor::end_group() noexcept {
  if (depth_ != 0 && --depth_ == 0) {
    close_unit();
  }
}

void Editor::close_unit() noexcept {
  if (open_ != nullptr) {
    // A unit that was committed ends the click groups of the other rings too: "any other undoable dispatch" closes a group (D8).
    if (open_->end_unit(hint_)) {
      for (UndoJournal* r : rings_) {
        if (r != open_) {
          r->break_group();
        }
      }
    }
    open_ = nullptr;
  }
  hint_ = 0;
  muted_ = false;
  area_ = UndoArea::Auto;
}

namespace {

// The ring an Auto unit goes to by its first record (D2 rule 3).
UndoArea auto_area(std::uint8_t op) noexcept {
  switch (op) {
    case kCreatePattern:
    case kDeletePattern:
    case kSetPatternName:
    case kSetTrackLength:
    case kSetPatternLength:
    case kClearPattern:
    case kSetStep:
    case kSetSteps:
    case kLaneDelete:
    case kLaneInsert:
      return UndoArea::Pattern;
    case kSetInstrument:
    case kClearInstrument:
    case kSetParam:
    case kSetLearn:
      return UndoArea::Sound;
    default:
      return UndoArea::Song;  // order, tempo, swing, the title and the project-level fields
  }
}

}  // namespace

UndoJournal* Editor::writer(std::uint8_t op) noexcept {
  if (muted_ || final_) {
    return nullptr;
  }
  if (open_ == nullptr) {
    open_ = rings_[ring_index(area_ == UndoArea::Auto ? auto_area(op) : area_)];
    open_->begin_unit(pending_action_, pending_arg_);
  }
  return open_;
}

// The rings that name a pattern (PATTERN, SONG) or an instrument slot (SOUND) in their records.
namespace {
constexpr unsigned kNamePattern = 0b011;
constexpr unsigned kNameInstrument = 0b100;
}  // namespace

// A structural record written to `ring`: the other rings that name the object have records of the object that is about to change. A creation or a fill only makes their
// redo units untrue (they would make it again); a deletion or a clear makes all their units untrue.
void Editor::fence_write(UndoJournal& ring, std::uint8_t op) noexcept {
  const unsigned names = op == kCreatePattern || op == kDeletePattern ? kNamePattern : kNameInstrument;
  const bool removal = op == kDeletePattern || op == kClearInstrument;
  for (unsigned i = 0; i < kUndoAreas; ++i) {
    UndoJournal* r = rings_[i];
    if (((names >> i) & 1u) != 0u && r != &ring) {
      if (removal) {
        r->clear();
      } else {
        r->drop_redo();
      }
    }
  }
}

// A structural unit was undone or redone from `keep`: every other ring that names the object loses its whole history. Returns the bit of each ring it cleared.
std::uint8_t Editor::fence_others(const UndoJournal& keep, bool pattern) noexcept {
  const unsigned names = pattern ? kNamePattern : kNameInstrument;
  std::uint8_t mask = 0;
  for (unsigned i = 0; i < kUndoAreas; ++i) {
    UndoJournal* r = rings_[i];
    if (((names >> i) & 1u) != 0u && r != &keep && (r->depth() != 0 || r->redo_depth() != 0)) {
      r->clear();
      mask = static_cast<std::uint8_t>(mask | (1u << i));
    }
  }
  return mask;
}

bool Editor::record(std::uint8_t op, const Piece* pieces, std::size_t count, bool structural) noexcept {
  UndoJournal* j = writer(op);
  if (j == nullptr) {
    return false;
  }
  std::size_t total = 0;
  for (std::size_t i = 0; i < count; ++i) {
    total += pieces[i].size;
  }
  if (!j->reserve(op, total)) {
    return false;
  }
  for (std::size_t i = 0; i < count; ++i) {
    j->put(pieces[i].data, pieces[i].size);
  }
  j->commit();
  if (structural) {
    fence_write(*j, op);
  }
  return true;
}

// ---- header, tracks, master -----------------------------------------------------------------------------------------

Status Editor::set_name(const char* text) noexcept { return set_name(text, text != nullptr ? std::strlen(text) : 0); }

Status Editor::set_name(const char* text, std::size_t len) noexcept {
  Scope scope(*this);
  const std::uint8_t ol = static_cast<std::uint8_t>(field_length(project_.name(), kProjectNameMax));
  char old[kProjectNameMax + 1];
  std::memcpy(old, project_.name(), ol);
  const Status st = project_.set_name(text, len);
  if (st == Status::Ok && (len != ol || (len != 0 && std::memcmp(text, old, len) != 0))) {
    const std::uint8_t nl = static_cast<std::uint8_t>(len);
    const Piece p[] = {{&nl, 1}, {text, len}, {&ol, 1}, {old, ol}};
    (void)record(kSetName, p, 4);
  }
  return st;
}

Status Editor::set_tempo_x100(std::uint32_t value) noexcept {
  Scope scope(*this);
  const std::uint32_t old = project_.tempo_x100();
  const Status st = project_.set_tempo_x100(value);
  if (st == Status::Ok && value != old) {
    UndoJournal* j = writer(kSetTempo);
    if (j == nullptr || !merge_tempo(*j, value)) {
      const Piece p[] = {{&value, 4}, {&old, 4}};
      (void)record(kSetTempo, p, 2);
    }
  }
  return st;
}

// OS-PATTEMPO: the tempo edits of one Pattern hold (taps and detents) are one click group, keyed kTempoMergeKey (merge_hint): the unit committed last is reopened and its set_tempo
// record takes the new value, keeping the first old one. A hold that comes back to the tempo it began at leaves nothing. False: write a record as for any edit.
bool Editor::merge_tempo(UndoJournal& ring, std::uint32_t now) noexcept {
  const std::uint32_t key = static_cast<std::uint32_t>(kTempoMergeKey);
  if (hint_ != key) {
    return false;
  }
  if (!ring.started_ && !ring.reopen(hint_)) {
    return false;
  }
  UndoJournal::RecRef last;
  if (!ring.last_record(last) || last.op != kSetTempo || last.len != 8) {
    return false;
  }
  PayloadReader r{ring, last.payload, last.len};
  std::uint32_t was = 0, first = 0;
  r.get(&was, 4);
  r.get(&first, 4);
  if (!r.ok) {
    return false;
  }
  if (was != first && now == first) {
    ring.drop_last(last);
  } else {
    ring.patch(last, 0, &now, 4);
  }
  return true;
}

Status Editor::set_swing(std::uint32_t percent) noexcept {
  Scope scope(*this);
  const std::uint8_t old = project_.swing();
  const Status st = project_.set_swing(percent);
  const std::uint8_t now = static_cast<std::uint8_t>(percent);
  if (st == Status::Ok && now != old) {
    const Piece p[] = {{&now, 1}, {&old, 1}};
    (void)record(kSetSwing, p, 2);
  }
  return st;
}

Status Editor::set_default_pattern_length(std::uint32_t steps) noexcept {
  Scope scope(*this);
  const std::uint8_t old = project_.default_pattern_length();
  const Status st = project_.set_default_pattern_length(steps);
  const std::uint8_t now = static_cast<std::uint8_t>(steps);
  if (st == Status::Ok && now != old) {
    const Piece p[] = {{&now, 1}, {&old, 1}};
    (void)record(kSetDefaultLength, p, 2);
  }
  return st;
}

Status Editor::set_active_tracks(std::uint32_t count) noexcept {
  Scope scope(*this);
  const std::uint8_t old = project_.active_tracks();
  const Status st = project_.set_active_tracks(count);
  const std::uint8_t now = static_cast<std::uint8_t>(count);
  if (st == Status::Ok && now != old) {
    const Piece p[] = {{&now, 1}, {&old, 1}};
    (void)record(kSetActiveTracks, p, 2);
  }
  return st;
}

Status Editor::set_master(const MasterSettings& value) noexcept {
  Scope scope(*this);
  const MasterSettings old = project_.master();
  const Status st = project_.set_master(value);
  if (st == Status::Ok && std::memcmp(&old, &value, sizeof old) != 0) {
    const Piece p[] = {{&value, sizeof value}, {&old, sizeof old}};
    (void)record(kSetMaster, p, 2);
  }
  return st;
}

Status Editor::set_track(std::uint32_t slot, const TrackSlot& value) noexcept {
  Scope scope(*this);
  const TrackSlot* cur = project_.track(slot);
  if (cur == nullptr) {
    return project_.set_track(slot, value);  // BadIndex
  }
  const TrackSlot old = *cur;
  const Status st = project_.set_track(slot, value);
  if (st == Status::Ok && std::memcmp(&old, project_.track(slot), sizeof old) != 0) {
    const std::uint8_t s = static_cast<std::uint8_t>(slot);
    const Piece p[] = {{&s, 1}, {&value, sizeof value}, {&old, sizeof old}};
    (void)record(kSetTrack, p, 3);
  }
  return st;
}

Status Editor::set_track_name(std::uint32_t slot, const char* text, std::size_t len) noexcept {
  Scope scope(*this);
  const TrackSlot* cur = project_.track(slot);
  if (cur == nullptr) {
    return project_.set_track_name(slot, text, len);  // BadIndex
  }
  const std::uint8_t ol = static_cast<std::uint8_t>(field_length(cur->name, kTrackNameMax));
  char old[kTrackNameMax + 1];
  std::memcpy(old, cur->name, ol);
  const Status st = project_.set_track_name(slot, text, len);
  if (st == Status::Ok && (len != ol || (len != 0 && std::memcmp(text, old, len) != 0))) {
    const std::uint8_t s = static_cast<std::uint8_t>(slot);
    const std::uint8_t nl = static_cast<std::uint8_t>(len);
    const Piece p[] = {{&s, 1}, {&nl, 1}, {text, len}, {&ol, 1}, {old, ol}};
    (void)record(kSetTrackName, p, 5);
  }
  return st;
}

// ---- song -----------------------------------------------------------------------------------------------------------

Status Editor::append_order(std::uint32_t pattern_id) noexcept {
  Scope scope(*this);
  const Status st = project_.append_order(pattern_id);
  if (st == Status::Ok) {
    const std::uint8_t id = static_cast<std::uint8_t>(pattern_id);
    const Piece p[] = {{&id, 1}};
    (void)record(kAppendOrder, p, 1);
  }
  return st;
}

Status Editor::insert_order(std::uint32_t pos, std::uint32_t pattern_id) noexcept {
  Scope scope(*this);
  const Status st = project_.insert_order(pos, pattern_id);
  if (st == Status::Ok) {
    const std::uint8_t b[2] = {static_cast<std::uint8_t>(pos), static_cast<std::uint8_t>(pattern_id)};
    const Piece p[] = {{b, 2}};
    (void)record(kInsertOrder, p, 1);
  }
  return st;
}

Status Editor::set_order(std::uint32_t pos, std::uint32_t pattern_id) noexcept {
  Scope scope(*this);
  const std::uint8_t old = project_.order(pos);
  const Status st = project_.set_order(pos, pattern_id);
  if (st == Status::Ok && old != pattern_id) {
    const std::uint8_t b[3] = {static_cast<std::uint8_t>(pos), static_cast<std::uint8_t>(pattern_id), old};
    const Piece p[] = {{b, 3}};
    (void)record(kSetOrder, p, 1);
  }
  return st;
}

Status Editor::remove_order(std::uint32_t pos) noexcept {
  Scope scope(*this);
  const std::uint8_t old = project_.order(pos);
  const Status st = project_.remove_order(pos);
  if (st == Status::Ok) {
    const std::uint8_t b[2] = {static_cast<std::uint8_t>(pos), old};
    const Piece p[] = {{b, 2}};
    (void)record(kRemoveOrder, p, 1);
  }
  return st;
}

Status Editor::clear_order() noexcept {
  Scope scope(*this);
  std::uint8_t old[1 + kMaxSongPositions];
  const std::uint32_t n = project_.song_length();
  old[0] = static_cast<std::uint8_t>(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    old[1 + i] = project_.order(i);
  }
  const Status st = project_.clear_order();
  if (st == Status::Ok && n != 0) {
    const Piece p[] = {{old, 1 + n}};
    (void)record(kClearOrder, p, 1);
  }
  return st;
}

Status Editor::move_order(std::uint32_t from, std::uint32_t to) noexcept {
  Scope scope(*this);
  const std::uint32_t n = project_.song_length();
  if (from >= n || to >= n) {
    return Status::BadIndex;
  }
  if (from == to) {
    return Status::Ok;
  }
  const std::uint8_t id = project_.order(from);
  if (n < kMaxSongPositions) {
    // The copy goes where the slot lands, the old one is removed (it is one place on when the copy went above it).
    const std::uint8_t ins[2] = {static_cast<std::uint8_t>(to > from ? to + 1u : to), id};
    const std::uint8_t rem[2] = {static_cast<std::uint8_t>(to > from ? from : from + 1u), id};
    const Piece pi[] = {{ins, 2}};
    const Piece pr[] = {{rem, 2}};
    (void)record(kInsertOrder, pi, 1);
    (void)record(kRemoveOrder, pr, 1);
  } else {
    // A full list cannot take the insert: the slots between are rewritten one by one.
    const std::uint32_t lo = from < to ? from : to;
    const std::uint32_t hi = from < to ? to : from;
    for (std::uint32_t i = lo; i <= hi; ++i) {
      const std::uint8_t now = i == to ? id : project_.order(from < to ? i + 1u : i - 1u);
      const std::uint8_t old = project_.order(i);
      if (now != old) {
        const std::uint8_t b[3] = {static_cast<std::uint8_t>(i), now, old};
        const Piece p[] = {{b, 3}};
        (void)record(kSetOrder, p, 1);
      }
    }
  }
  return project_.move_order(from, to);
}

// ---- patterns -------------------------------------------------------------------------------------------------------

Status Editor::create_pattern(std::uint32_t id, std::uint32_t length) noexcept {
  Scope scope(*this);
  const Status st = project_.create_pattern(id, length);
  if (st == Status::Ok) {
    const std::uint8_t b[2] = {static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(length)};
    const Piece p[] = {{b, 2}};
    (void)record(kCreatePattern, p, 1, true);
  }
  return st;
}

// Delete and clear throw away a whole pattern (14 KB) that the record must keep, and the Project overwrites it: the record is
// reserved and filled from the pattern's own memory before the edit, and committed once the Project accepted it. Both can
// only fail when the pattern does not exist, which is checked first, so the redo history is never given up for nothing (and
// clearing a pattern that has no step in it records nothing).
Status Editor::delete_pattern(std::uint32_t id) noexcept {
  Scope scope(*this);
  const Pattern* pat = project_.pattern(id);
  if (pat == nullptr) {
    return project_.delete_pattern(id);  // NotFound
  }
  const std::size_t lane_bytes = lanes_free_bytes(id);
  if (lane_bytes != 0 && !undo_fits(kDeletePattern, lane_bytes + UndoJournal::kRecordOverheadBytes + kDeleteImage + 3u)) {
    return Status::Full;  // the lanes' records and the pattern's would not fit the undo ring: nothing is deleted (editor.h)
  }
  lanes_free(id);  // OS-MIDIREC-P1 A16: before the pattern's record, so an undo brings the pattern back first
  const std::uint8_t idb = static_cast<std::uint8_t>(id);
  UndoJournal* j = writer(kDeletePattern);
  const bool rec = j != nullptr && j->reserve(kDeletePattern, kDeleteImage);
  if (rec) {
    j->put(&idb, 1);
    j->put(pat->track_length, sizeof pat->track_length);
    j->put(pat->name, sizeof pat->name);
    j->put(pat->steps, sizeof pat->steps);
  }
  const Status st = project_.delete_pattern(id);
  if (rec) {
    if (st == Status::Ok) {
      j->commit();
      fence_write(*j, kDeletePattern);
    } else {
      j->abort();
    }
  }
  return st;
}

Status Editor::clear_pattern(std::uint32_t id) noexcept {
  Scope scope(*this);
  const Pattern* pat = project_.pattern(id);
  bool any = false;  // a pattern that is already empty has nothing to bring back
  for (std::uint32_t t = 0; pat != nullptr && t < kMaxTracks && !any; ++t) {
    for (std::uint32_t k = 0; k < kMaxPatternLength && !any; ++k) {
      any = !is_empty(pat->steps[t][k]);
    }
  }
  if (pat != nullptr) {
    const std::size_t lanes = lanes_free_bytes(id);
    if (lanes != 0 && !undo_fits(kClearPattern, lanes + (any ? UndoJournal::kRecordOverheadBytes + 1 + kStepsImage + 3u : 0))) {
      return Status::Full;  // the lanes' records and the steps' would not fit the undo ring: nothing is cleared (editor.h)
    }
    lanes_free(id);  // OS-MIDIREC-P1 A16: CLEAR frees the pattern's lanes too (their own records, in this unit)
  }
  if (!any) {
    return project_.clear_pattern(id);  // NotFound, or nothing to clear: not recorded
  }
  const std::uint8_t idb = static_cast<std::uint8_t>(id);
  UndoJournal* j = writer(kClearPattern);
  const bool rec = j != nullptr && j->reserve(kClearPattern, 1 + kStepsImage);
  if (rec) {
    j->put(&idb, 1);
    j->put(pat->steps, sizeof pat->steps);
  }
  const Status st = project_.clear_pattern(id);
  if (rec) {
    if (st == Status::Ok) {
      j->commit();
    } else {
      j->abort();
    }
  }
  return st;
}

Status Editor::set_pattern_name(std::uint32_t id, const char* text, std::size_t len) noexcept {
  Scope scope(*this);
  const Pattern* pat = project_.pattern(id);
  if (pat == nullptr) {
    return project_.set_pattern_name(id, text, len);  // NotFound
  }
  const std::uint8_t ol = static_cast<std::uint8_t>(field_length(pat->name, kPatternNameMax));
  char old[kPatternNameMax + 1];
  std::memcpy(old, pat->name, ol);
  const Status st = project_.set_pattern_name(id, text, len);
  if (st == Status::Ok && (len != ol || (len != 0 && std::memcmp(text, old, len) != 0))) {
    const std::uint8_t idb = static_cast<std::uint8_t>(id);
    const std::uint8_t nl = static_cast<std::uint8_t>(len);
    const Piece p[] = {{&idb, 1}, {&nl, 1}, {text, len}, {&ol, 1}, {old, ol}};
    (void)record(kSetPatternName, p, 5);
  }
  return st;
}

Status Editor::set_track_length(std::uint32_t id, std::uint32_t track, std::uint32_t length) noexcept {
  Scope scope(*this);
  const Pattern* pat = project_.pattern(id);
  if (pat == nullptr || track >= kMaxTracks) {
    return project_.set_track_length(id, track, length);  // NotFound or BadIndex
  }
  const std::uint8_t old = pat->track_length[track];
  if (length < old && length >= kMinPatternLength) {
    const std::size_t lanes = lane_cut_bytes(id, track, length * kLaneTicksPerStep);
    if (lanes != 0 && !undo_fits(kSetTrackLength, lanes + UndoJournal::kRecordOverheadBytes + 4u)) {
      return Status::Full;  // the cut lane's record and the length's would not fit the undo ring: nothing changes (editor.h)
    }
  }
  const Status st = project_.set_track_length(id, track, length);
  if (st == Status::Ok && old != length) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(track),
                               static_cast<std::uint8_t>(length), old};
    const Piece p[] = {{b, 4}};
    (void)record(kSetTrackLength, p, 1);
  }
  if (st == Status::Ok && length < old) {
    lane_cut(id, track, length * kLaneTicksPerStep);  // A17
  }
  return st;
}

Status Editor::set_pattern_length(std::uint32_t id, std::uint32_t length) noexcept {
  Scope scope(*this);
  const Pattern* pat = project_.pattern(id);
  if (pat == nullptr) {
    return project_.set_pattern_length(id, length);  // NotFound
  }
  std::uint8_t b[2 + kMaxTracks];
  b[0] = static_cast<std::uint8_t>(id);
  b[1] = static_cast<std::uint8_t>(length);
  std::memcpy(b + 2, pat->track_length, kMaxTracks);
  bool changes = false;
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    changes = changes || b[2 + t] != b[1];
  }
  if (length >= kMinPatternLength && length <= kMaxPatternLength) {
    std::size_t lanes = 0;
    for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
      lanes += length < b[2 + t] ? lane_cut_bytes(id, t, length * kLaneTicksPerStep) : 0;
    }
    if (lanes != 0 && !undo_fits(kSetPatternLength, lanes + UndoJournal::kRecordOverheadBytes + ((sizeof b + 3u) & ~std::size_t{3}))) {
      return Status::Full;  // the cut lanes' records and the lengths' would not fit the undo ring: nothing changes (editor.h)
    }
  }
  const Status st = project_.set_pattern_length(id, length);
  if (st == Status::Ok && changes) {
    const Piece p[] = {{b, sizeof b}};
    (void)record(kSetPatternLength, p, 1);
  }
  if (st == Status::Ok) {
    for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
      if (length < b[2 + t]) {
        lane_cut(id, t, length * kLaneTicksPerStep);  // A17
      }
    }
  }
  return st;
}

// ---- lanes (OS-MIDIREC-P1) --------------------------------------------------------------------------------------------

// One lane record: u8 id, u8 track, u16 index, u16 count (host order), then the events. The events of a record are at most one lane's (4096 x 8 + 6 bytes).
void Editor::lane_erase(std::uint32_t pattern, std::uint32_t track, std::uint32_t index, std::uint32_t count) noexcept {
  const LaneStore::View v = lanes_->lane(pattern, track);
  if (v.count < index + count) {
    return;
  }
  const std::uint16_t ix = static_cast<std::uint16_t>(index);
  const std::uint16_t n = static_cast<std::uint16_t>(count);
  const std::uint8_t id = static_cast<std::uint8_t>(pattern);
  const std::uint8_t tr = static_cast<std::uint8_t>(track);
  const Piece p[] = {{&id, 1}, {&tr, 1}, {&ix, 2}, {&n, 2}, {v.events + index, count * sizeof(LaneEvent)}};
  (void)record(kLaneDelete, p, 5);  // the pieces point into the pool: the journal copies them before the erase
  (void)lanes_->erase(pattern, track, index, count);
  (void)project_.touch_lanes(pattern);
}

bool Editor::lane_insert(std::uint32_t pattern, std::uint32_t track, std::uint32_t index, const LaneEvent* events,
                         std::uint32_t count) noexcept {
  LaneEvent* gap = lanes_->open_gap(pattern, track, index, count);
  if (gap == nullptr) {
    return false;
  }
  std::memcpy(gap, events, count * sizeof(LaneEvent));
  const std::uint16_t ix = static_cast<std::uint16_t>(index);
  const std::uint16_t n = static_cast<std::uint16_t>(count);
  const std::uint8_t id = static_cast<std::uint8_t>(pattern);
  const std::uint8_t tr = static_cast<std::uint8_t>(track);
  const Piece p[] = {{&id, 1}, {&tr, 1}, {&ix, 2}, {&n, 2}, {events, count * sizeof(LaneEvent)}};
  (void)record(kLaneInsert, p, 5);
  (void)project_.touch_lanes(pattern);
  return true;
}

// The ring bytes of one lane record of `n` events (0 for none): 8 + align4(u8 id, u8 track, u16 index, u16 count, the events) = 8n + 16.
std::size_t Editor::lane_record_bytes(std::uint32_t n) noexcept {
  return n == 0 ? 0 : UndoJournal::kRecordOverheadBytes + ((6u + n * sizeof(LaneEvent) + 3u) & ~std::size_t{3});
}

std::size_t Editor::lane_cut_bytes(std::uint32_t pattern, std::uint32_t track, std::uint32_t from_tick) noexcept {
  if (lanes_ == nullptr) {
    return 0;
  }
  lanes_->sync(project_.load_epoch());
  const LaneStore::View v = lanes_->lane(pattern, track);
  return lane_record_bytes(v.count - LaneStore::lower_bound(v, from_tick));
}

std::size_t Editor::lanes_free_bytes(std::uint32_t pattern) noexcept {
  std::size_t bytes = 0;
  for (std::uint32_t t = 0; lanes_ != nullptr && t < kMaxTracks; ++t) {
    bytes += lane_cut_bytes(pattern, t, 0);
  }
  return bytes;
}

// Whether a unit that adds `record_bytes` of records fits the ring it is written to (the open unit's own bytes count; the oldest history is made room for by
// eviction, but a unit bigger than the ring is not undoable and ends the history, so the callers refuse it with Full before changing anything).
bool Editor::undo_fits(std::uint8_t op, std::size_t record_bytes) const noexcept {
  if (muted_ || final_) {
    return true;  // nothing is recorded
  }
  const UndoJournal* j = open_ != nullptr ? open_ : rings_[ring_index(area_ == UndoArea::Auto ? auto_area(op) : area_)];
  if (j == nullptr || !j->ready()) {
    return true;
  }
  const std::size_t held = j->open_bytes();  // header and trailer included once the unit has a record
  const std::size_t frame = held == 0 ? UndoJournal::kUnitHeaderBytes + UndoJournal::kUnitTrailerBytes : 0;
  return held + frame + record_bytes <= j->capacity_bytes();
}

void Editor::lane_cut(std::uint32_t pattern, std::uint32_t track, std::uint32_t from_tick) noexcept {
  if (lanes_ == nullptr) {
    return;
  }
  lanes_->sync(project_.load_epoch());
  const LaneStore::View v = lanes_->lane(pattern, track);
  const std::uint32_t at = LaneStore::lower_bound(v, from_tick);
  if (at < v.count) {
    lane_erase(pattern, track, at, v.count - at);
  }
}

void Editor::lanes_free(std::uint32_t pattern) noexcept {
  if (lanes_ == nullptr) {
    return;
  }
  lanes_->sync(project_.load_epoch());
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    const std::uint32_t n = lanes_->lane(pattern, t).count;
    if (n != 0) {
      lane_erase(pattern, t, 0, n);
    }
  }
}

Status Editor::lane_replace(std::uint32_t pattern, std::uint32_t track, std::uint32_t t0, std::uint32_t t1,
                            const LaneEvent* events, std::uint32_t n) noexcept {
  Scope scope(*this);
  return lane_replace_in(pattern, track, t0, t1, events, n, true);
}

Status Editor::lane_clear(std::uint32_t pattern, std::uint32_t track) noexcept {
  Scope scope(*this);
  return lane_replace_in(pattern, track, 0, kLaneTicks, nullptr, 0, false);  // the whole lane, whatever the track's length
}

// `inside_track`: t1 must not pass the track's length x 48 ticks (A17); lane_clear is exempt (it only removes).
Status Editor::lane_replace_in(std::uint32_t pattern, std::uint32_t track, std::uint32_t t0, std::uint32_t t1,
                               const LaneEvent* events, std::uint32_t n, bool inside_track) noexcept {
  const Pattern* pat = project_.pattern(pattern);
  if (lanes_ == nullptr || pat == nullptr) {
    return Status::NotFound;
  }
  if (track >= kMaxTracks) {
    return Status::BadIndex;
  }
  if (t0 >= t1 || t1 > kLaneTicks || n > kMaxLaneEvents || (n != 0 && events == nullptr) || !lane_events_valid(events, n, t0, t1) ||
      (inside_track && t1 > pat->track_length[track] * kLaneTicksPerStep)) {
    return Status::BadValue;
  }
  lanes_->sync(project_.load_epoch());
  const LaneStore::View v = lanes_->lane(pattern, track);
  const std::uint32_t lo = LaneStore::lower_bound(v, t0);
  const std::uint32_t hi = LaneStore::lower_bound(v, t1);
  const std::uint32_t old = hi - lo;
  if (old == n && (n == 0 || std::memcmp(v.events + lo, events, n * sizeof(LaneEvent)) == 0)) {
    return Status::Ok;  // the same events: nothing to apply, nothing to record
  }
  if (v.count - old + n > kMaxLaneEvents || lanes_->total_events() - old + n > kMaxLanePoolEvents ||
      (v.count == 0 && n != 0 && lanes_->lane_count() >= kMaxLanes)) {
    return Status::Full;
  }
  if (!undo_fits(kLaneDelete, lane_record_bytes(old) + lane_record_bytes(n))) {
    return Status::Full;  // the records of this take would not fit the undo ring: nothing is erased, the history stays
  }
  if (old != 0) {
    lane_erase(pattern, track, lo, old);
  }
  if (n != 0) {
    (void)lane_insert(pattern, track, lo, events, n);  // room was checked above (the erase freed the old ones)
  }
  return Status::Ok;
}

Status Editor::set_step(std::uint32_t id, std::uint32_t track, std::uint32_t step, const Step& value) noexcept {
  Scope scope(*this);
  const Pattern* pat = project_.pattern(id);
  if (pat == nullptr || track >= kMaxTracks || step >= kMaxPatternLength) {
    return project_.set_step(id, track, step, value);  // NotFound or BadIndex
  }
  const Step old = pat->steps[track][step];
  const Status st = project_.set_step(id, track, step, value);
  if (st == Status::Ok && std::memcmp(&old, &value, sizeof old) != 0) {
    const std::uint8_t b[3] = {static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(track),
                               static_cast<std::uint8_t>(step)};
    const Piece p[] = {{b, 3}, {&value, sizeof value}, {&old, sizeof old}};
    (void)record(kSetStep, p, 3);
  }
  return st;
}

Status Editor::set_steps(std::uint32_t id, std::uint32_t track, std::uint32_t first, const Step* values,
                         std::uint32_t count) noexcept {
  Scope scope(*this);
  const Pattern* pat = project_.pattern(id);
  if (pat == nullptr || track >= kMaxTracks || first > kMaxPatternLength || count > kMaxPatternLength - first ||
      count == 0 || values == nullptr) {
    return project_.set_steps(id, track, first, values, count);  // rejected, or an accepted no-op
  }
  Step old[kMaxPatternLength];
  std::memcpy(old, &pat->steps[track][first], count * kStepBytes);
  const Status st = project_.set_steps(id, track, first, values, count);
  if (st == Status::Ok && std::memcmp(old, values, count * kStepBytes) != 0) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(track),
                               static_cast<std::uint8_t>(first), static_cast<std::uint8_t>(count)};
    const Piece p[] = {{b, 4}, {values, count * kStepBytes}, {old, count * kStepBytes}};
    (void)record(kSetSteps, p, 3);
  }
  return st;
}

// ---- instruments ----------------------------------------------------------------------------------------------------

Status Editor::set_param(core::ParamId id, float value) noexcept {
  Scope scope(*this);
  const float old = project_.get_param(id);
  const std::uint32_t generation = project_.param_generation(id);
  const Status st = project_.set_param(id, value);
  const float now = project_.get_param(id);  // what was stored: an integer field rounds, so redo applies exactly this
  // (the generation moved too when the stored bytes changed but the value did not: a SOURCE read as Off that was Off with a loop byte set, OS-INSTUI S6)
  if (st == Status::Ok && (now != old || project_.param_generation(id) != generation)) {
    UndoJournal* j = writer(kSetParam);
    if (j != nullptr && !merge_param(*j, id, now)) {
      const std::uint32_t raw = static_cast<std::uint32_t>(id);
      const Piece p[] = {{&raw, 4}, {&now, 4}, {&old, 4}};
      (void)record(kSetParam, p, 3);
    }
  }
  return st;
}

// OS-UNDOGRP: a click on the parameter the unit committed last was a click on (merge_hint): the unit is reopened and its set_param record takes the
// new value, keeping the first old value. A sweep that comes back to where it started leaves nothing. False: write a record as for any edit.
bool Editor::merge_param(UndoJournal& ring, core::ParamId id, float now) noexcept {
  if (hint_ == 0 || hint_ != static_cast<std::uint32_t>(id)) {
    return false;
  }
  if (!ring.started_ && !ring.reopen(hint_)) {
    return false;
  }
  UndoJournal::RecRef last;
  if (!ring.last_record(last) || last.op != kSetParam || last.len != 12) {
    return false;
  }
  PayloadReader r{ring, last.payload, last.len};
  std::uint32_t raw = 0;
  float was = 0.0f, first = 0.0f;
  r.get(&raw, 4);
  r.get(&was, 4);
  r.get(&first, 4);
  if (!r.ok || raw != static_cast<std::uint32_t>(id)) {
    return false;
  }
  if (was != first && now == first) {
    ring.drop_last(last);  // back where the sweep began: nothing to undo (a record that never changed the value stays)
  } else {
    ring.patch(last, 4, &now, 4);
  }
  return true;
}

Status Editor::set_learn_slot(unsigned slot, const core::LearnBinding& b) noexcept {
  Scope scope(*this);
  if (slot >= core::kLearnSlots) {
    return project_.set_learn_slot(slot, b);  // BadIndex
  }
  const core::LearnBinding old = project_.settings().learn(slot);
  const Status st = project_.set_learn_slot(slot, b);
  if (st == Status::Ok && std::memcmp(&old, &project_.settings().learn(slot), sizeof old) != 0) {
    const std::uint8_t s = static_cast<std::uint8_t>(slot);
    const Piece p[] = {{&s, 1}, {&b, sizeof b}, {&old, sizeof old}};
    (void)record(kSetLearn, p, 3);
  }
  return st;
}

Status Editor::set_instrument(std::uint32_t index, const Instrument& value) noexcept {
  Scope scope(*this);
  const Instrument* cur = project_.instrument(index);
  const bool has_old = cur != nullptr;
  Instrument old;
  if (has_old) {
    old = *cur;
  }
  const Status st = project_.set_instrument(index, value);
  if (st == Status::Ok && !(has_old && std::memcmp(&old, project_.instrument(index), sizeof old) == 0)) {
    const std::uint8_t b[2] = {static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(has_old ? 1 : 0)};
    const Piece p[] = {{b, 2}, {&value, sizeof value}, {&old, has_old ? sizeof old : 0u}};
    (void)record(kSetInstrument, p, 3, !has_old);
  }
  return st;
}

Status Editor::add_module(const char* name, std::size_t len, std::uint32_t abi_major, std::uint32_t abi_minor) noexcept {
  const bool was_in_sync = in_sync();
  const Status st = project_.add_module(name, len, abi_major, abi_minor);
  if (st == Status::Ok && was_in_sync) {
    synced();  // only the table grew: no record is invalidated, so the history stays
  }
  return st;
}

// OS-004d: the project-level state is not part of any record, so a change of it keeps an in-sync history in sync.
#define O3OS_EDITOR_STATE_SETTER(call)       \
  const bool was_in_sync = in_sync();        \
  const std::uint32_t before = project_.generation(); \
  const Status st = call;                    \
  if (st == Status::Ok && was_in_sync && project_.generation() != before) { \
    synced();                                \
  }                                          \
  return st;

Status Editor::set_midi_state(const MidiState& value) noexcept { O3OS_EDITOR_STATE_SETTER(project_.set_midi(value)) }
Status Editor::set_usb_state(const UsbState& value) noexcept { O3OS_EDITOR_STATE_SETTER(project_.set_usb(value)) }
Status Editor::set_mix_state(const MixState& value) noexcept { O3OS_EDITOR_STATE_SETTER(project_.set_mix(value)) }
Status Editor::set_perf_state(const PerfState& value) noexcept { O3OS_EDITOR_STATE_SETTER(project_.set_perf(value)) }
Status Editor::set_note_state(const NoteState& value) noexcept { O3OS_EDITOR_STATE_SETTER(project_.set_note(value)) }
Status Editor::set_mute_state(const MuteState& value) noexcept { O3OS_EDITOR_STATE_SETTER(project_.set_mute(value)) }
Status Editor::set_scenes_state(const ScenesState& value) noexcept { O3OS_EDITOR_STATE_SETTER(project_.set_scenes(value)) }
Status Editor::set_send_fx_state(const SendFxState& value) noexcept { O3OS_EDITOR_STATE_SETTER(project_.set_send_fx(value)) }
Status Editor::set_active_tracks_state(std::uint32_t count) noexcept { O3OS_EDITOR_STATE_SETTER(project_.set_active_tracks(count)) }
Status Editor::set_app_params(const char* name, std::size_t len, std::uint32_t abi_major, std::uint32_t abi_minor, const float* values, std::uint32_t count) noexcept {
  O3OS_EDITOR_STATE_SETTER(project_.set_app_params(name, len, abi_major, abi_minor, values, count))
}
Status Editor::set_insert(const InsertRecord& value) noexcept { O3OS_EDITOR_STATE_SETTER(project_.set_insert(value)) }
Status Editor::clear_insert(std::uint32_t where, std::uint32_t slot) noexcept { O3OS_EDITOR_STATE_SETTER(project_.clear_insert(where, slot)) }
#undef O3OS_EDITOR_STATE_SETTER

Status Editor::set_instrument_name(std::uint32_t index, const char* text, std::size_t len) noexcept {
  const Instrument* cur = project_.instrument(index);
  if (cur == nullptr) {
    return index >= kMaxInstruments ? Status::BadIndex : Status::NotFound;
  }
  if (text == nullptr || len > kInstrumentNameMax || std::memchr(text, 0, len) != nullptr) {
    return Status::BadValue;
  }
  Instrument next = *cur;
  std::memset(next.name, 0, sizeof next.name);
  std::memcpy(next.name, text, len);
  return set_instrument(index, next);  // (records nothing when the name is the same)
}

Status Editor::clear_instrument(std::uint32_t index) noexcept {
  Scope scope(*this);
  const Instrument* cur = project_.instrument(index);
  if (cur == nullptr) {
    return project_.clear_instrument(index);  // BadIndex or NotFound
  }
  const Instrument old = *cur;
  const Status st = project_.clear_instrument(index);
  if (st == Status::Ok) {
    const std::uint8_t b = static_cast<std::uint8_t>(index);
    const Piece p[] = {{&b, 1}, {&old, sizeof old}};
    (void)record(kClearInstrument, p, 2, true);
  }
  return st;
}

// ---- undo and redo --------------------------------------------------------------------------------------------------

// Applies one side of one record to the project through its mutators: the old values (undo) or the new ones (redo).
bool Editor::replay(std::uint8_t op, std::size_t payload, std::size_t len, bool undo) noexcept {
  PayloadReader r{*walking_, payload, len};
  Status st = Status::BadValue;
  switch (op) {
    case kSetName: {
      char text[kProjectNameMax + 1];
      const std::size_t n = pick_text(r, undo, text, kProjectNameMax);
      st = r.ok ? project_.set_name(text, n) : Status::BadValue;
      break;
    }
    case kSetTempo: {
      std::uint32_t now = 0, old = 0;
      r.get(&now, 4);
      r.get(&old, 4);
      st = project_.set_tempo_x100(undo ? old : now);
      break;
    }
    case kSetSwing:
    case kSetDefaultLength:
    case kSetActiveTracks: {
      const std::uint32_t now = r.u8();
      const std::uint32_t old = r.u8();
      const std::uint32_t v = undo ? old : now;
      st = op == kSetSwing ? project_.set_swing(v)
                           : (op == kSetDefaultLength ? project_.set_default_pattern_length(v) : project_.set_active_tracks(v));
      break;
    }
    case kSetMaster: {
      MasterSettings now{}, old{};
      r.get(&now, sizeof now);
      r.get(&old, sizeof old);
      st = project_.set_master(undo ? old : now);
      break;
    }
    case kSetTrack: {
      const std::uint32_t slot = r.u8();
      TrackSlot now{}, old{};
      r.get(&now, sizeof now);
      r.get(&old, sizeof old);
      st = project_.set_track(slot, undo ? old : now);
      break;
    }
    case kSetTrackName: {
      const std::uint32_t slot = r.u8();
      char text[kTrackNameMax + 1];
      const std::size_t n = pick_text(r, undo, text, kTrackNameMax);
      st = r.ok ? project_.set_track_name(slot, text, n) : Status::BadValue;
      break;
    }
    case kAppendOrder: {
      const std::uint32_t id = r.u8();
      const std::uint32_t n = project_.song_length();
      st = undo ? (n != 0 ? project_.remove_order(n - 1) : Status::BadIndex) : project_.append_order(id);
      break;
    }
    case kInsertOrder: {
      const std::uint32_t pos = r.u8();
      const std::uint32_t id = r.u8();
      st = undo ? project_.remove_order(pos) : project_.insert_order(pos, id);
      break;
    }
    case kSetOrder: {
      const std::uint32_t pos = r.u8();
      const std::uint32_t now = r.u8();
      const std::uint32_t old = r.u8();
      st = project_.set_order(pos, undo ? old : now);
      break;
    }
    case kRemoveOrder: {
      const std::uint32_t pos = r.u8();
      const std::uint32_t old = r.u8();
      st = undo ? project_.insert_order(pos, old) : project_.remove_order(pos);
      break;
    }
    case kClearOrder: {
      const std::uint32_t n = r.u8();
      if (!undo) {
        st = project_.clear_order();
        break;
      }
      st = Status::Ok;
      for (std::uint32_t i = 0; i < n && st == Status::Ok; ++i) {
        st = project_.append_order(r.u8());
      }
      break;
    }
    case kCreatePattern: {
      const std::uint32_t id = r.u8();
      const std::uint32_t length = r.u8();
      fence_pattern_ = true;
      st = undo ? project_.delete_pattern(id) : project_.create_pattern(id, length);
      break;
    }
    case kDeletePattern: {
      const std::uint32_t id = r.u8();
      fence_pattern_ = true;
      if (!undo) {
        st = project_.delete_pattern(id);
        break;
      }
      std::uint8_t lengths[kMaxTracks];
      char name[kPatternNameMax + 1];
      r.get(lengths, sizeof lengths);
      r.get(name, sizeof name);
      // The pattern comes back as the new empty pattern of length 1 and is then given back its lengths, name and steps.
      st = project_.create_pattern(id, kMinPatternLength);
      for (std::uint32_t t = 0; t < kMaxTracks && st == Status::Ok; ++t) {
        st = project_.set_track_length(id, t, lengths[t]);
      }
      if (st == Status::Ok) {
        st = project_.set_pattern_name(id, name, field_length(name, kPatternNameMax));
      }
      if (st == Status::Ok) {
        st = restore_rows(project_, r, id);
      }
      break;
    }
    case kSetPatternName: {
      const std::uint32_t id = r.u8();
      char text[kPatternNameMax + 1];
      const std::size_t n = pick_text(r, undo, text, kPatternNameMax);
      st = r.ok ? project_.set_pattern_name(id, text, n) : Status::BadValue;
      break;
    }
    case kSetTrackLength: {
      const std::uint32_t id = r.u8();
      const std::uint32_t track = r.u8();
      const std::uint32_t now = r.u8();
      const std::uint32_t old = r.u8();
      st = project_.set_track_length(id, track, undo ? old : now);
      break;
    }
    case kSetPatternLength: {
      const std::uint32_t id = r.u8();
      const std::uint32_t now = r.u8();
      if (!undo) {
        st = project_.set_pattern_length(id, now);
        break;
      }
      std::uint8_t lengths[kMaxTracks];
      r.get(lengths, sizeof lengths);
      st = Status::Ok;
      for (std::uint32_t t = 0; t < kMaxTracks && st == Status::Ok; ++t) {
        st = project_.set_track_length(id, t, lengths[t]);
      }
      break;
    }
    case kClearPattern: {
      const std::uint32_t id = r.u8();
      st = undo ? restore_rows(project_, r, id) : project_.clear_pattern(id);
      break;
    }
    case kSetStep: {
      const std::uint32_t id = r.u8();
      const std::uint32_t track = r.u8();
      const std::uint32_t step = r.u8();
      Step now{}, old{};
      r.get(&now, sizeof now);
      r.get(&old, sizeof old);
      st = project_.set_step(id, track, step, undo ? old : now);
      break;
    }
    case kSetSteps: {
      const std::uint32_t id = r.u8();
      const std::uint32_t track = r.u8();
      const std::uint32_t first = r.u8();
      const std::uint32_t count = r.u8();
      if (count == 0 || count > kMaxPatternLength) {
        r.ok = false;
        break;
      }
      Step steps[kMaxPatternLength];
      if (undo) {
        r.skip(count * kStepBytes);
      }
      r.get(steps, count * kStepBytes);
      st = project_.set_steps(id, track, first, steps, count);
      break;
    }
    case kSetInstrument: {
      const std::uint32_t index = r.u8();
      const bool has_old = r.u8() != 0;
      fence_sound_ = fence_sound_ || !has_old;
      Instrument inst;
      if (undo && !has_old) {
        st = project_.clear_instrument(index);
        break;
      }
      if (undo) {
        r.skip(sizeof inst);
      }
      r.get(&inst, sizeof inst);
      st = project_.set_instrument(index, inst);
      break;
    }
    case kClearInstrument: {
      const std::uint32_t index = r.u8();
      fence_sound_ = true;
      Instrument inst;
      if (!undo) {
        st = project_.clear_instrument(index);
        break;
      }
      r.get(&inst, sizeof inst);
      st = project_.set_instrument(index, inst);
      break;
    }
    case kSetParam: {
      std::uint32_t raw = 0;
      float now = 0.0f, old = 0.0f;
      r.get(&raw, 4);
      r.get(&now, 4);
      r.get(&old, 4);
      const core::ParamId id = static_cast<core::ParamId>(raw);
      st = project_.set_param(id, undo ? old : now);
      if (st == Status::Ok) {
        note_applied(id, undo ? old : now);
      }
      break;
    }
    case kSetLearn: {
      const std::uint32_t slot = r.u8();
      core::LearnBinding now{}, old{};
      r.get(&now, sizeof now);
      r.get(&old, sizeof old);
      st = project_.set_learn_slot(slot, undo ? old : now);
      break;
    }
    case kLaneDelete:
    case kLaneInsert: {
      const std::uint32_t id = r.u8();
      const std::uint32_t track = r.u8();
      std::uint16_t index = 0, count = 0;
      r.get(&index, 2);
      r.get(&count, 2);
      if (!r.ok || lanes_ == nullptr || count == 0 || r.left != count * sizeof(LaneEvent)) {
        r.ok = false;
        break;
      }
      if ((op == kLaneInsert) != undo) {  // forward of an insert, or the reverse of a delete: put the events back in
        LaneEvent* gap = lanes_->open_gap(id, track, index, count);
        if (gap == nullptr) {
          st = Status::Full;
          break;
        }
        r.get(gap, count * sizeof(LaneEvent));
        st = project_.touch_lanes(id) ? Status::Ok : Status::NotFound;
      } else {
        st = lanes_->erase(id, track, index, count) && project_.touch_lanes(id) ? Status::Ok : Status::BadIndex;
      }
      break;
    }
    default:
      break;
  }
  if (op == kSetTrack || op == kSetMaster || op == kSetInstrument || op == kClearInstrument) {
    applied_incomplete_ = true;  // parameters changed that the applied list does not carry
  }
  return r.ok && st == Status::Ok;
}

// Insert `id` at a, then remove the slot at b (indices of the list after the insert) is the move of that slot when it is `id`: the slot at b-1 (b above a) goes to a, or the slot at b
// (b below a) goes to a-1. Anything else (the ids differ, a == b, an index out of the list) is not a move and is left to the two records.
bool Editor::replay_move(const UndoJournal::RecRef& first, const UndoJournal::RecRef& second) noexcept {
  if (first.len != 2 || second.len != 2) {
    return false;
  }
  PayloadReader ra{*walking_, first.payload, first.len};
  PayloadReader rb{*walking_, second.payload, second.len};
  const std::uint32_t a = ra.u8();
  const std::uint8_t id = ra.u8();
  const std::uint32_t b = rb.u8();
  const std::uint8_t id_b = rb.u8();
  const std::uint32_t n = project_.song_length();
  if (!ra.ok || !rb.ok || id != id_b || a == b || a > n || b > n) {
    return false;
  }
  const std::uint32_t from = b > a ? b - 1u : b;
  const std::uint32_t to = b > a ? a : a - 1u;
  if (from >= n || to >= n || project_.order(from) != id) {
    return false;
  }
  return project_.move_order(from, to) == Status::Ok;
}

void Editor::note_applied(core::ParamId id, float value) noexcept {
  if (applied_n_ < kAppliedMax) {
    applied_[applied_n_++] = AppliedParam{id, value};
  } else {
    applied_incomplete_ = true;
  }
}

UndoResult Editor::walk(bool undo, UndoArea area) noexcept {
  applied_n_ = 0;
  applied_incomplete_ = false;
  fenced_ = UndoArea::Auto;
  fence_mask_ = 0;
  fence_pattern_ = false;
  fence_sound_ = false;
  UndoJournal& ring = *rings_[ring_index(area)];  // the ring the unit comes from
  walking_ = &ring;
  if (depth_ != 0 || ring.group_open()) {
    return UndoResult::Busy;
  }
  merge_break();  // undo and redo end every click group
  if (project_.generation() != synced_) {
    history_reset();
    ++stale_;
    return UndoResult::Stale;
  }
  UndoJournal::UnitRef u;
  const UndoJournal::Peek peek = undo ? ring.peek_undo(u) : ring.peek_redo(u);
  if (peek == UndoJournal::Peek::Empty) {
    return UndoResult::Nothing;
  }
  bool good = peek == UndoJournal::Peek::Ok;
  if (good) {
    std::size_t body = u.len - UndoJournal::kUnitHeaderBytes - UndoJournal::kUnitTrailerBytes;
    // Undo walks the unit backwards from its trailer, redo forwards from its header.
    std::size_t pos = undo ? ring.retreat(ring.cur(), UndoJournal::kUnitTrailerBytes)
                           : ring.advance(u.start, UndoJournal::kUnitHeaderBytes);
    Project::OrderEdit order_unit(project_);  // E-SONG-07: the unit's edits of the order list reach a playing song together
    for (std::uint16_t k = 0; k < u.records && good; ++k) {
      UndoJournal::RecRef rec;
      if (!(undo ? ring.record_before(pos, body, rec) : ring.record_at(pos, body, rec))) {
        good = false;
        break;
      }
      body -= rec.total;
      pos = undo ? rec.start : ring.advance(pos, rec.total);
      // E-SONG-07: the insert and the remove of one slot that Editor::move_order wrote are applied as one move again (not the insert and then the remove: a playing song would lose the slot it plays).
      if (k + 1u < u.records && rec.op == (undo ? kRemoveOrder : kInsertOrder)) {
        UndoJournal::RecRef next;
        if ((undo ? ring.record_before(pos, body, next) : ring.record_at(pos, body, next)) && next.op == (undo ? kInsertOrder : kRemoveOrder) &&
            replay_move(rec, next)) {
          body -= next.total;
          pos = undo ? next.start : ring.advance(pos, next.total);
          ++k;
          continue;
        }
      }
      if (!replay(rec.op, rec.payload, rec.len, undo)) {
        // The project refused a step that it must accept: a bug. What was applied of the unit stays; the history ends here.
        good = false;
      }
    }
    good = good && body == 0;
  }
  synced();
  if (!good) {
    ++failures_;
    ring.damage();
    history_reset();  // the project is half way through a unit: no area's records can be trusted against it
    return UndoResult::Failed;
  }
  if (undo) {
    ring.finish_undo(u);
  } else {
    ring.finish_redo(u);
  }
  // D4, the structure fence: a unit that created or deleted a pattern (filled or cleared an instrument slot) changed what the other rings' records name, wherever it
  // was made: they cannot be applied any more and end.
  if (fence_pattern_) {
    fence_mask_ = static_cast<std::uint8_t>(fence_mask_ | fence_others(ring, true));
  }
  if (fence_sound_) {
    fence_mask_ = static_cast<std::uint8_t>(fence_mask_ | fence_others(ring, false));
  }
  for (unsigned i = 0; i < kUndoAreas && fenced_ == UndoArea::Auto; ++i) {
    if (((fence_mask_ >> i) & 1u) != 0u) {
      fenced_ = i == 0 ? UndoArea::Pattern : (i == 1 ? UndoArea::Song : UndoArea::Sound);
    }
  }
  return UndoResult::Ok;
}

UndoResult Editor::undo(UndoArea area) noexcept { return walk(true, area); }
UndoResult Editor::redo(UndoArea area) noexcept { return walk(false, area); }

}  // namespace o3os::model
