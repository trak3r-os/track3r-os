// SPDX-License-Identifier: GPL-3.0-or-later
// FORM and BRIDGE of the Song TOOLS dialog (OS-SONG-BUILD; song_build.h): the shared numbers, pre-check and slot insert, FORM's planner and executor (a song shape by subtraction) and BRIDGE's (OS-SONG-BRIDGE: the
// hash, the phases and the planner and executor over one cell walk). New Community OS behaviour, no format change.
#include <o3os/app/song_build.h>

#include "song_edit_internal.h"

#include <o3os/app/text_format.h>
#include <o3os/model/editor.h>
#include <o3os/model/limits.h>
#include <o3os/model/project.h>

namespace o3os::app {

namespace {

// ---- the forms (OS-SONG-BUILD D3): one byte per slot, the source role in bits 4-5 (A, B, C) and the groups cleared in the low bits ----------------

constexpr std::uint8_t kA = 0x00;
constexpr std::uint8_t kB = 0x10;
constexpr std::uint8_t kC = 0x20;
constexpr std::uint8_t K = kFormKick;
constexpr std::uint8_t P = kFormPerc;
constexpr std::uint8_t M = kFormMusic;

constexpr std::uint8_t kLoopSong[] = {kA | M, kA | K, kA, kA, kA | K | P, kA, kA, kA | M};
constexpr std::uint8_t kPop[] = {kA | K | P, kA, kA | K, kB, kA, kB, kC | K | P, kB, kB | K | P};
constexpr std::uint8_t kTechno[] = {kA | P | M, kA | M, kA, kA, kB | K | P, kB | K, kB, kC, kA | M, kA | P | M};
static_assert(sizeof kLoopSong <= kSongBuildMax && sizeof kPop <= kSongBuildMax && sizeof kTechno <= kSongBuildMax, "a form's slots fit the plan");

constexpr std::uint8_t kNoCopy = 0xFFu;

// The tracks of every step that holds something, within the track's length: bit t set when track t sounds. (A source's data decides whether a cleared group changes anything, D6.)
std::uint16_t data_tracks(const model::Pattern& pat) noexcept {
  std::uint16_t mask = 0;
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    for (unsigned k = 0; k < pat.track_length[t] && k < model::kMaxPatternLength; ++k) {
      if (!model::is_empty(pat.steps[t][k])) {
        mask = static_cast<std::uint16_t>(mask | (1u << t));
        break;
      }
    }
  }
  return mask;
}

void append_id(core::FixedString<48>& out, unsigned id) noexcept { append_dec(out, id, id > 99u ? 3u : 2u); }

}  // namespace

// ---- shared -----------------------------------------------------------------------------------------------------------------

SongBuildRefusal song_build_precheck(const model::Project& project, unsigned new_slots, unsigned need, SongBuildPlan& plan) noexcept {
  const unsigned len = project.song_length();
  SongIdSet named;
  for (unsigned s = 0; s < len; ++s) {
    named.add(project.order(s));
  }
  unsigned free_count = 0;
  for (unsigned id = 1; id <= model::kMaxPatternId; ++id) {
    free_count += project.pattern(id) == nullptr && !named.has(id) ? 1u : 0u;
  }
  plan.free_numbers = free_count;
  plan.pool_left = model::kMaxPatterns - static_cast<unsigned>(project.pattern_count());
  if (len + new_slots > model::kMaxSongPositions) {
    plan.info.refusal = SongBuildRefusal::SongFull;
    plan.info.number = static_cast<std::uint8_t>(new_slots);
    plan.info.have = static_cast<std::uint8_t>(model::kMaxSongPositions - len);
    return plan.info.refusal;
  }
  const unsigned room = plan.pool_left < free_count ? plan.pool_left : free_count;
  if (need > room) {
    plan.info.refusal = SongBuildRefusal::NoRoom;
    plan.info.number = static_cast<std::uint8_t>(need);
    plan.info.have = static_cast<std::uint8_t>(room);
    return plan.info.refusal;
  }
  return SongBuildRefusal::None;
}

bool song_build_assign_numbers(const model::Project& project, SongBuildPlan& plan) noexcept {
  SongIdSet named;
  const unsigned len = project.song_length();
  for (unsigned s = 0; s < len; ++s) {
    named.add(project.order(s));
  }
  SongIdSet taken;
  for (unsigned i = 0; i < plan.info.copies; ++i) {
    const unsigned id = free_number(project, named, taken);
    if (id == 0u) {  // (the pre-check ruled this out: the text it would give, so the strip is never blank)
      const unsigned room = plan.pool_left < plan.free_numbers ? plan.pool_left : plan.free_numbers;
      plan.info.refusal = SongBuildRefusal::NoRoom;
      plan.info.number = plan.info.copies;
      plan.info.have = static_cast<std::uint8_t>(room);
      return false;
    }
    taken.add(id);
    plan.copy[i].to = static_cast<std::uint8_t>(id);
  }
  return true;
}

bool song_build_insert_slots(model::Editor& editor, const SongBuildPlan& plan) noexcept {
  const model::Editor::OrderBatch batch(editor);  // a playing song sees the new slots as one change (as DROP SLOT does)
  for (unsigned i = 0; i < plan.info.slots; ++i) {
    if (editor.insert_order(plan.at + i, plan.slot_pattern[i]) != model::Status::Ok) {
      return false;
    }
  }
  return true;
}

void song_build_text(SongBuildKind kind, const SongBuildInfo& info, core::FixedString<48>& out) noexcept {
  switch (info.refusal) {
    case SongBuildRefusal::None:
      out.assign("");
      break;
    case SongBuildRefusal::Select:
      out.assign(kind == SongBuildKind::Form ? "SELECT 1-3 PATTERNS" : "SELECT 2 SLOTS");
      break;
    case SongBuildRefusal::NoPattern:
      out.assign("SLOT ");
      append_id(out, info.number);
      out.append(" HAS NO PATTERN");
      break;
    case SongBuildRefusal::SongFull:
      out.assign("SONG FULL: NEEDS ");
      append_uint(out, info.number);
      out.append(" SLOTS, ");
      append_uint(out, info.have);
      out.append(" FREE");
      break;
    case SongBuildRefusal::NoRoom:
      out.assign("NEEDS ");
      append_uint(out, info.number);
      out.append(info.number == 1u ? " FREE PATTERN, " : " FREE PATTERNS, ");
      append_uint(out, info.have);
      out.append(" FREE");
      break;
    case SongBuildRefusal::NoNext:
      out.assign("NO NEXT SLOT");
      break;
    case SongBuildRefusal::Same:
      out.assign("SAME PATTERN IN BOTH SLOTS");
      break;
  }
}

// ---- FORM -------------------------------------------------------------------------------------------------------------------

const std::uint8_t* song_form_table(SongForm form, unsigned& count) noexcept {
  switch (form) {
    case SongForm::Pop:
      count = sizeof kPop;
      return kPop;
    case SongForm::Techno:
      count = sizeof kTechno;
      return kTechno;
    case SongForm::LoopSong:
      break;
  }
  count = sizeof kLoopSong;
  return kLoopSong;
}

const char* song_form_name(SongForm form) noexcept {
  switch (form) {
    case SongForm::Pop: return "POP";
    case SongForm::Techno: return "TECHNO";
    case SongForm::LoopSong: break;
  }
  return "LOOP>SONG";
}

std::uint16_t song_form_tracks(std::uint8_t groups, unsigned drums) noexcept {
  drums = drums < 1u ? 1u : (drums > kSongDrumsMax ? kSongDrumsMax : drums);
  const std::uint16_t drum_bits = static_cast<std::uint16_t>((1u << drums) - 1u);  // tracks 1..drums
  std::uint16_t mask = 0;
  if ((groups & kFormKick) != 0u) {
    mask = static_cast<std::uint16_t>(mask | 1u);
  }
  if ((groups & kFormPerc) != 0u) {
    mask = static_cast<std::uint16_t>(mask | (drum_bits & ~1u));
  }
  if ((groups & kFormMusic) != 0u) {
    mask = static_cast<std::uint16_t>(mask | (0xFFFFu & ~drum_bits));
  }
  return mask;
}

void song_form_plan(const model::Project& project, const SongRect& rect, SongForm form, unsigned drums, SongBuildPlan& plan) noexcept {
  plan = SongBuildPlan{};
  SongBuildInfo& info = plan.info;
  const int count = static_cast<int>(project.song_length());
  const int s0 = rect.s0 < 0 ? 0 : rect.s0;
  const int s1 = rect.s1 < count ? rect.s1 : count - 1;
  if (s1 < s0) {
    info.refusal = SongBuildRefusal::Select;
    return;
  }
  // The sources: the distinct patterns of the slots in slot order (D4), at most 3; a slot that names no pattern refuses (a bridge or a form of nothing is no song).
  SongIdSet seen;
  unsigned distinct = 0;
  for (int s = s0; s <= s1; ++s) {
    const unsigned id = project.order(static_cast<std::uint32_t>(s));
    if (id == 0u || project.pattern(id) == nullptr) {
      info.refusal = SongBuildRefusal::NoPattern;
      info.number = static_cast<std::uint8_t>(s);
      return;
    }
    if (!seen.has(id)) {
      seen.add(id);
      if (distinct < 3u) {
        info.sources[distinct] = static_cast<std::uint8_t>(id);
      }
      ++distinct;
    }
  }
  if (distinct > 3u) {
    info.refusal = SongBuildRefusal::Select;
    return;
  }
  info.source_count = static_cast<std::uint8_t>(distinct);

  // The form's slots: a source, or a copy of it with some tracks cleared (D6: no copy when the cleared groups hold no data; D5: a missing B or C is A). Slots with the same (source, groups) share one copy.
  std::uint16_t data[3] = {};
  for (unsigned i = 0; i < distinct; ++i) {
    data[i] = data_tracks(*project.pattern(info.sources[i]));
  }
  unsigned n = 0;
  const std::uint8_t* table = song_form_table(form, n);
  std::uint8_t slot_copy[kSongBuildMax];
  std::uint8_t key_source[kSongBuildMax];
  std::uint16_t key_cleared[kSongBuildMax];  // the tracks a copy really empties (cleared and holding data): two slots that empty the same ones share a copy, whatever the groups (DRUMS 1: K+P = K)
  for (unsigned k = 0; k < n; ++k) {
    const unsigned role = (table[k] >> 4) & 3u;
    const unsigned src = role < distinct ? role : 0u;
    const std::uint8_t groups = table[k] & 7u;
    const std::uint16_t tracks = song_form_tracks(groups, drums);
    slot_copy[k] = kNoCopy;
    plan.slot_pattern[k] = info.sources[src];
    const std::uint16_t effective = static_cast<std::uint16_t>(tracks & data[src]);
    if (effective == 0u) {
      continue;  // nothing to clear: the slot is the source itself
    }
    unsigned c = 0;
    while (c < info.copies && !(key_source[c] == src && key_cleared[c] == effective)) {
      ++c;
    }
    if (c == info.copies) {
      key_source[c] = static_cast<std::uint8_t>(src);
      key_cleared[c] = effective;
      plan.copy[c].from = info.sources[src];
      plan.copy[c].cleared = tracks;
      ++info.copies;
    }
    slot_copy[k] = static_cast<std::uint8_t>(c);
  }
  info.slots = static_cast<std::uint8_t>(n);
  plan.at = static_cast<unsigned>(s1) + 1u;

  if (song_build_precheck(project, n, info.copies, plan) != SongBuildRefusal::None) {
    return;
  }
  if (!song_build_assign_numbers(project, plan)) {
    return;  // (refused: it said why)
  }
  std::size_t body = 0;
  for (unsigned c = 0; c < info.copies; ++c) {
    body += copy_pattern_body_masked(project, plan.copy[c].from, plan.copy[c].cleared);
  }
  for (unsigned k = 0; k < n; ++k) {
    if (slot_copy[k] != kNoCopy) {
      plan.slot_pattern[k] = plan.copy[slot_copy[k]].to;
    }
    body += song_record_bytes(2);  // Editor::insert_order: [pos, id]
  }
  plan.bytes = kSongUnitOverhead + body;
}

bool song_form_apply(model::Editor& editor, const SongBuildPlan& plan) noexcept {
  for (unsigned c = 0; c < plan.info.copies; ++c) {
    if (song_copy_pattern_masked(editor, plan.copy[c].from, plan.copy[c].to, plan.copy[c].cleared) != model::Status::Ok) {
      return false;
    }
  }
  return song_build_insert_slots(editor, plan);
}

void song_form_footer(const SongBuildInfo& info, core::FixedString<48>& out) noexcept {
  if (info.refusal != SongBuildRefusal::None) {
    song_build_text(SongBuildKind::Form, info, out);
    return;
  }
  out.assign("FROM");
  for (unsigned i = 0; i < info.source_count && i < 3u; ++i) {
    out.append(" ");
    append_id(out, info.sources[i]);
  }
  out.append("  ");
  append_uint(out, info.slots);
  out.append(" SLOTS  ");
  append_uint(out, info.copies);
  out.append(info.copies == 1u ? " NEW PATTERN" : " NEW PATTERNS");
}

// ---- BRIDGE -----------------------------------------------------------------------------------------------------------------

namespace {

// lowbias32: a xorshift-multiply mix of one 32-bit word.
constexpr std::uint32_t mix32(std::uint32_t x) noexcept {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

// The whole way from A to B in t16's unit (1/65536); a bridge pattern's track lengths change from A's to B's past half of it (2 x t16 > kBridgeOne).
constexpr unsigned kBridgeOne = 65536u;

// The length a new bridge pattern is created with (its tracks that differ get a set_track_length): the derived length of the source the slot's track lengths come from (A up to the middle, B after), or the
// project's default for a source with no active track.
unsigned bridge_made(const model::Project& p, unsigned a_id, unsigned b_id, unsigned t16) noexcept {
  const unsigned len = p.pattern_length(2u * t16 <= kBridgeOne ? a_id : b_id);
  return len != 0u ? len : p.default_pattern_length();
}

// One bridge slot's tracks, the planner and the executor over one walk (`sink.track(tr, length)` per track, `sink.steps(tr, first, count, steps)` for the span from its first to its last step that holds something).
template <class Sink>
void bridge_slot_walk(const model::Pattern& a, const model::Pattern& b, const std::uint16_t phase[model::kMaxTracks], unsigned t16, Sink& sink) noexcept {
  model::Step buf[model::kMaxPatternLength];  // 896 B of the main loop's stack
  for (unsigned tr = 0; tr < model::kMaxTracks; ++tr) {
    const unsigned len = song_bridge_track(a, b, phase, tr, t16, buf);
    sink.track(tr, len);
    int lo = -1;
    int hi = -1;
    for (unsigned i = 0; i < len; ++i) {
      if (!model::is_empty(buf[i])) {
        lo = lo < 0 ? static_cast<int>(i) : lo;
        hi = static_cast<int>(i);
      }
    }
    if (lo >= 0) {
      sink.steps(tr, static_cast<unsigned>(lo), static_cast<unsigned>(hi - lo + 1), &buf[lo]);
    }
  }
}

// The journal bytes of one bridge slot's body: the records the executor's calls make (create_pattern is counted by the caller).
struct BridgeBytes {
  unsigned made;
  std::size_t bytes = 0;
  void track(unsigned, unsigned len) noexcept {
    bytes += len != made ? song_record_bytes(4) : 0u;  // set_track_length: [id, track, new, old]
  }
  void steps(unsigned, unsigned, unsigned count, const model::Step*) noexcept {
    bytes += song_record_bytes(4u + 2u * count * sizeof(model::Step));  // set_steps: [id, track, first, count], the new steps, the old
  }
};

struct BridgeWrite {
  model::Editor& editor;
  unsigned id;
  unsigned made;
  bool ok = true;
  void track(unsigned tr, unsigned len) noexcept {
    if (len != made) {
      ok = editor.set_track_length(id, tr, len) == model::Status::Ok && ok;
    }
  }
  void steps(unsigned tr, unsigned first, unsigned count, const model::Step* values) noexcept {
    ok = editor.set_steps(id, tr, first, values, count) == model::Status::Ok && ok;
  }
};

// "BRIDGE k/N" (D12).
void bridge_name(unsigned k, unsigned n, core::FixedString<48>& out) noexcept {
  out.assign("BRIDGE ");
  append_uint(out, k);
  out.append("/");
  append_uint(out, n);
}

}  // namespace

std::uint32_t song_bridge_hash(std::uint32_t seed, unsigned track, unsigned step) noexcept {
  const std::uint32_t h = mix32(seed + (track + 1u) * 0x9E3779B1u);
  return step == kBridgeNoStep ? h : mix32(h ^ ((step + 1u) * 0x85EBCA6Bu));
}

void song_bridge_phases(std::uint32_t seed, std::uint16_t out[model::kMaxTracks]) noexcept {
  std::uint32_t h[model::kMaxTracks];
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    h[t] = song_bridge_hash(seed, t);
  }
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    unsigned rank = 0;
    for (unsigned u = 0; u < model::kMaxTracks; ++u) {
      rank += h[u] < h[t] || (h[u] == h[t] && u < t) ? 1u : 0u;
    }
    out[t] = static_cast<std::uint16_t>(2048u * rank);
  }
}

unsigned song_bridge_t16(unsigned k, unsigned n) noexcept { return static_cast<unsigned>((static_cast<std::uint32_t>(k) * kBridgeOne) / (n + 1u)); }

unsigned song_bridge_thr(std::uint32_t seed, const std::uint16_t phase[model::kMaxTracks], unsigned track, unsigned step) noexcept {
  return phase[track] + (song_bridge_hash(seed, track, step) >> 17);
}

unsigned song_bridge_track(const model::Pattern& a, const model::Pattern& b, const std::uint16_t phase[model::kMaxTracks], unsigned track, unsigned t16, model::Step* out) noexcept {
  const unsigned len_a = a.track_length[track] != 0u ? a.track_length[track] : 1u;
  const unsigned len_b = b.track_length[track] != 0u ? b.track_length[track] : 1u;
  unsigned len = 2u * t16 <= kBridgeOne ? len_a : len_b;
  len = len > model::kMaxPatternLength ? model::kMaxPatternLength : len;
  for (unsigned i = 0; i < len; ++i) {
    out[i] = song_bridge_thr(kBridgeSeed, phase, track, i) < t16 ? b.steps[track][i % len_b] : a.steps[track][i % len_a];
  }
  return len;
}

void song_bridge_plan(const model::Project& project, const SongRect& rect, unsigned slots, SongBuildPlan& plan, bool with_bytes) noexcept {
  plan = SongBuildPlan{};
  SongBuildInfo& info = plan.info;
  const int count = static_cast<int>(project.song_length());
  const int s0 = rect.s0 < 0 ? 0 : rect.s0;
  const int s1 = rect.s1 < count ? rect.s1 : count - 1;
  if (s1 < s0 || s1 - s0 > 1) {
    info.refusal = SongBuildRefusal::Select;  // not the cursor's slot, nor exactly two (D8)
    return;
  }
  if (s1 == s0 && s0 + 1 >= count) {
    info.refusal = SongBuildRefusal::NoNext;
    return;
  }
  // A is the first slot, B the one after it; both must name a pattern that exists, and not the same (D19).
  for (int s = s0; s <= s0 + 1; ++s) {
    const unsigned id = project.order(static_cast<std::uint32_t>(s));
    if (id == 0u || project.pattern(id) == nullptr) {
      info.refusal = SongBuildRefusal::NoPattern;
      info.number = static_cast<std::uint8_t>(s);
      return;
    }
    info.sources[s - s0] = static_cast<std::uint8_t>(id);
  }
  info.source_count = 2;
  if (info.sources[0] == info.sources[1]) {
    info.refusal = SongBuildRefusal::Same;
    return;
  }
  const unsigned n = slots < 1u ? 1u : (slots > kSongBridgeMax ? kSongBridgeMax : slots);
  info.slots = static_cast<std::uint8_t>(n);
  info.copies = static_cast<std::uint8_t>(n);
  plan.at = static_cast<unsigned>(s0) + 1u;
  if (song_build_precheck(project, n, n, plan) != SongBuildRefusal::None) {
    return;
  }
  if (!song_build_assign_numbers(project, plan)) {
    return;  // (refused: it said why)
  }
  const model::Pattern& a = *project.pattern(info.sources[0]);
  const model::Pattern& b = *project.pattern(info.sources[1]);
  for (unsigned k = 1; k <= n; ++k) {
    plan.copy[k - 1].from = info.sources[0];
    plan.slot_pattern[k - 1] = plan.copy[k - 1].to;
  }
  if (!with_bytes) {
    return;  // the counts and the numbers are all the footer needs: no cell walk, `bytes` stays 0
  }
  std::uint16_t phase[model::kMaxTracks];
  song_bridge_phases(kBridgeSeed, phase);
  std::size_t body = 0;
  for (unsigned k = 1; k <= n; ++k) {
    const unsigned t16 = song_bridge_t16(k, n);
    BridgeBytes sink{bridge_made(project, info.sources[0], info.sources[1], t16)};
    bridge_slot_walk(a, b, phase, t16, sink);
    core::FixedString<48> name;
    bridge_name(k, n, name);
    body += song_record_bytes(2) + sink.bytes + song_record_bytes(3u + name.size()) + song_record_bytes(2);  // create_pattern, the body, set_pattern_name, insert_order
  }
  plan.bytes = kSongUnitOverhead + body;
}

bool song_bridge_apply(model::Editor& editor, const SongBuildPlan& plan) noexcept {
  const unsigned n = plan.info.slots;
  std::uint16_t phase[model::kMaxTracks];
  song_bridge_phases(kBridgeSeed, phase);
  for (unsigned k = 1; k <= n; ++k) {
    const model::Project& project = editor.project();
    const model::Pattern* a = project.pattern(plan.info.sources[0]);
    const model::Pattern* b = project.pattern(plan.info.sources[1]);
    if (a == nullptr || b == nullptr) {
      return false;
    }
    const unsigned t16 = song_bridge_t16(k, n);
    const unsigned id = plan.copy[k - 1].to;
    BridgeWrite sink{editor, id, bridge_made(project, plan.info.sources[0], plan.info.sources[1], t16)};
    if (editor.create_pattern(id, sink.made) != model::Status::Ok) {
      return false;
    }
    bridge_slot_walk(*a, *b, phase, t16, sink);  // (a and b are fixed storage: create_pattern takes a free slot of the pool and moves none)
    core::FixedString<48> name;
    bridge_name(k, n, name);
    if (!sink.ok || editor.set_pattern_name(id, name.c_str(), name.size()) != model::Status::Ok) {
      return false;
    }
  }
  return song_build_insert_slots(editor, plan);
}

void song_bridge_footer(const SongBuildInfo& info, core::FixedString<48>& out) noexcept {
  if (info.refusal != SongBuildRefusal::None) {
    song_build_text(SongBuildKind::Bridge, info, out);
    return;
  }
  out.assign("PATTERN ");
  append_id(out, info.sources[0]);
  out.append(" TO ");
  append_id(out, info.sources[1]);
  out.append("  ");
  append_uint(out, info.slots);
  out.append(info.slots == 1u ? " SLOT  " : " SLOTS  ");
  append_uint(out, info.copies);
  out.append(info.copies == 1u ? " NEW PATTERN" : " NEW PATTERNS");
}

}  // namespace o3os::app
