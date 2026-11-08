// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET A3: the table of rule 3 (selection_ops.h) and its check. New Community OS behaviour, test tooling only.
//
// THE TABLE IS WRITTEN FROM THE SPECS AS DECIDED, NOT FROM THE CODE. Each entry names its spec (OS-009 and OS-PATUX for the block of Pattern, PAT-OPS for DUPLICATE / EXPAND / SHRINK, OS-SONGSHARE for the range of Song
// slots, OS-SEDFX and OS-011a for the range of Sample edit, the maintainer's decision of 2026-10-21 for ADD SLICE). Where the spec leaves something open, the entry says so (`silent`) and that part is not judged.
//
// What may change is the SPAN of the entry (the cells of a block, the tracks of a block over their length, the song order, the slice table of an instrument, the frames of a selection ...): both documents are
// copied, the span is masked in both copies (the cells set to one constant, the order cleared ...), and the two digests must agree. What the span holds is then compared with the spec by the entry's `inside`.
#include "selection_ops.h"

#include <o3os/app/model.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/song_view.h>
#include <o3os/app/view_state.h>
#include <o3os/app/wave_math.h>
#include <o3os/model/instrument.h>
#include <o3os/model/limits.h>
#include <o3os/model/pattern.h>
#include <o3os/sim/scenario_assert.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;

namespace qanet {

namespace {

constexpr std::uint8_t kFNote = 1, kFInstr = 2, kFVol = 4, kFFx1 = 8, kFFx2 = 16, kFAll = 31;

}  // namespace

// What an entry lets an action change. Masking sets the named parts of a document copy to one constant.
struct Span {
  struct Cells {
    unsigned pattern, t0, t1, r0, r1;  // inclusive
    std::uint8_t fields;
  };
  std::vector<Cells> cells;
  struct Lengths {
    unsigned pattern, t0, t1;
  };
  std::vector<Lengths> lengths;  // the track lengths of a pattern
  bool order = false;            // the song order
  int inst_all = -1;             // an instrument slot, whole (it may be replaced or deleted)
  int inst_slices = -1;          // an instrument slot's slice table, slice count, selected slice and play mode
  int inst_points = -1;          // an instrument slot's start, loop and end points
  int pcm_slot = -1;             // a sample slot whose audio may change ...
  bool pcm_frames_only = false;  // ... only inside [pcm_a, pcm_b) and without changing its length
  std::uint32_t pcm_a = 0, pcm_b = 0;
  bool pcm_length_may_change = false;
};

namespace {

// ---- Reading the images --------------------------------------------------------------------------------------------------------------------------------------------------------------------------

std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    h = (h ^ b[i]) * 1099511628211ull;
  }
  return h;
}

constexpr std::uint32_t kMaxCopyFrames = 4u * 1024u * 1024u;  // a longer sample is hashed, not copied: the inside of a sample edit is then not judged

const model::Step kEmpty = model::kEmptyStep;

model::Step cell(const SelImage& im, unsigned pattern, unsigned t, unsigned r) {
  const model::Pattern* p = im.doc != nullptr ? im.doc->pattern(pattern) : nullptr;
  return p != nullptr && t < model::kMaxTracks && r < model::kMaxPatternLength ? p->steps[t][r] : kEmpty;
}

unsigned track_len(const SelImage& im, unsigned pattern, unsigned t) {
  const model::Pattern* p = im.doc != nullptr ? im.doc->pattern(pattern) : nullptr;
  return p != nullptr && t < model::kMaxTracks ? p->track_length[t] : 0u;
}

bool same(const model::Step& a, const model::Step& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// OS-AUDIT-56: `got` must equal `want` in the block's columns (Note and Instr are one pair: either column takes both) and `orig`, the step as it was before, in every column outside the block; a whole-step block compares whole.
bool same_in(const SelImage& im, const model::Step& got, const model::Step& want, const model::Step& orig) {
  if (im.col0 == 0 && im.col1 == 4) {
    return same(got, want);
  }
  const auto in = [&](int c) { return im.col0 <= c && c <= im.col1; };
  model::Step x = want;
  if (!in(0) && !in(1)) {
    x.note = orig.note;
    x.instr = orig.instr;
  }
  if (!in(2)) {
    x.vol = orig.vol;
  }
  if (!in(3)) {
    x.fx[0] = orig.fx[0];
  }
  if (!in(4)) {
    x.fx[1] = orig.fx[1];
  }
  return same(got, x);
}

// The step fields the block's columns cover (Note and Instr go together).
std::uint8_t block_fields(const SelImage& im) {
  std::uint8_t f = 0;
  for (int c = im.col0; c <= im.col1; ++c) {
    f = static_cast<std::uint8_t>(f | (c <= 1 ? (kFNote | kFInstr) : (c == 2 ? kFVol : (c == 3 ? kFFx1 : kFFx2))));
  }
  return f;
}

unsigned active_tracks(const SelImage& im) { return im.doc != nullptr ? im.doc->active_tracks() : 0u; }

bool is_note(std::uint8_t n) { return n >= model::kNoteFirst && n <= model::kNoteLast; }

}  // namespace

bool image_of(sim::Simulator& s, SelImage& out) {
  const model::Project* p = s.project();
  if (p == nullptr) {
    return false;
  }
  const app::ViewState& v = s.app().state();
  out.doc = std::make_unique<model::Project>(*p);
  out.digest = sim::model_digest(*p);
  out.ok = !out.digest.empty();
  out.screen = v.screen;
  out.page = v.page[app::screen_index(v.screen)];
  out.pattern = v.edit.pattern;
  const app::PatternView& cur = s.app().view();
  out.cur_row = cur.row() < 0 ? 0 : cur.row();
  out.cur_track = cur.track() < 0 ? 0 : cur.track();
  out.cur_col = static_cast<int>(cur.column());
  const app::Selection::Block b = v.edit.selection.block(out.cur_row, out.cur_track);
  out.row0 = b.row0;
  out.row1 = b.row1;
  out.track0 = b.track0;
  out.track1 = b.track1;
  out.col0 = b.col0;
  out.col1 = b.col1;
  out.selected = v.edit.selection.active;
  out.editable = app::pattern_editable(v);
  out.fill_open = v.overlay == app::OverlayId::Fill;
  out.fill_what = static_cast<int>(v.fill.plan.what);
  out.song_pos = v.song.pos();
  out.song_first = v.song.first();
  out.song_last = v.song.last();
  out.song_in_map = v.song.in_map();
  out.slot = v.instrument >= 1 ? v.instrument - 1 : 0;
  out.wave_slice = v.wave.slice;
  out.wave_point = v.wave.point;
  out.wave_chop = v.page[app::screen_index(app::ScreenId::Wave)] == 1u;
  out.wave_zc = v.wave.zc;
  out.sed_a = v.sedit.a;
  out.sed_b = v.sedit.b;
  out.sed_amp = v.sedit.amp;
  const app::SampleBank* bank = s.app().model().samples;
  out.pcm_hash.clear();
  out.pcm.clear();
  out.pcm_ok = false;
  if (bank != nullptr) {
    for (unsigned slot = 0; slot < app::SampleBank::kSlots; ++slot) {
      const app::SampleBank::Pcm pcm = bank->pcm(slot);
      std::uint64_t h = 1469598103934665603ull;
      h = fnv(h, &pcm.frames, sizeof pcm.frames);
      if (pcm.data != nullptr) {
        h = fnv(h, pcm.data, pcm.frames * sizeof(std::int16_t));
      }
      out.pcm_hash.push_back(h);
      if (static_cast<int>(slot) == out.slot && pcm.data != nullptr && pcm.frames <= kMaxCopyFrames) {
        out.pcm.assign(pcm.data, pcm.data + pcm.frames);
        out.pcm_ok = true;
      }
    }
  }
  return true;
}

namespace {

// ---- Masking -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// false: the copy could not be masked (the mutator refused): the check says so.
bool mask(model::Project& p, const Span& sp, const SelImage& ref) {
  bool ok = true;
  for (const Span::Cells& c : sp.cells) {
    const model::Pattern* pat = p.pattern(c.pattern);
    if (pat == nullptr) {
      continue;
    }
    for (unsigned t = c.t0; t <= c.t1 && t < model::kMaxTracks; ++t) {
      for (unsigned r = c.r0; r <= c.r1 && r < model::kMaxPatternLength; ++r) {
        model::Step st = pat->steps[t][r];
        if ((c.fields & kFNote) != 0) st.note = kEmpty.note;
        if ((c.fields & kFInstr) != 0) st.instr = kEmpty.instr;
        if ((c.fields & kFVol) != 0) st.vol = kEmpty.vol;
        if ((c.fields & kFFx1) != 0) st.fx[0] = kEmpty.fx[0];
        if ((c.fields & kFFx2) != 0) st.fx[1] = kEmpty.fx[1];
        ok = p.set_step(c.pattern, t, r, st) == model::Status::Ok && ok;
      }
    }
  }
  for (const Span::Lengths& l : sp.lengths) {
    if (p.pattern(l.pattern) == nullptr) {
      continue;
    }
    for (unsigned t = l.t0; t <= l.t1 && t < model::kMaxTracks; ++t) {
      ok = p.set_track_length(l.pattern, t, 1) == model::Status::Ok && ok;
    }
  }
  if (sp.order) {
    ok = p.clear_order() == model::Status::Ok && ok;
  }
  if (sp.inst_all >= 0) {
    (void)p.clear_instrument(static_cast<std::uint32_t>(sp.inst_all));  // (a free slot: NotFound is fine)
  }
  if (sp.inst_slices >= 0) {
    if (const model::Instrument* in = p.instrument(static_cast<std::uint32_t>(sp.inst_slices))) {
      model::Instrument c = *in;
      std::memset(c.sampler.slices, 0, sizeof c.sampler.slices);
      c.sampler.slice_count = 0;
      c.sampler.selected_slice = 0;
      c.sampler.play_mode = 0;
      ok = p.set_instrument(static_cast<std::uint32_t>(sp.inst_slices), c) == model::Status::Ok && ok;
    }
  }
  if (sp.inst_points >= 0) {
    if (const model::Instrument* in = p.instrument(static_cast<std::uint32_t>(sp.inst_points))) {
      model::Instrument c = *in;
      c.sampler.start = c.sampler.loop_start = c.sampler.loop_end = c.sampler.end = 0;
      ok = p.set_instrument(static_cast<std::uint32_t>(sp.inst_points), c) == model::Status::Ok && ok;
    }
  }
  (void)ref;
  return ok;
}

// Where the two masked copies differ, as a fixed phrase.
const char* where_differs(const model::Project& a, const model::Project& b, std::string* detail = nullptr) {
  for (unsigned id = 1; id <= 255; ++id) {
    const model::Pattern* pa = a.pattern(id);
    const model::Pattern* pb = b.pattern(id);
    if ((pa == nullptr) != (pb == nullptr)) {
      return "a pattern was made or removed outside the selection";
    }
    if (pa != nullptr && std::memcmp(pa->track_length, pb->track_length, sizeof pa->track_length) != 0) {
      return "a pattern length changed outside the selection";
    }
    if (pa != nullptr && std::memcmp(pa->steps, pb->steps, sizeof pa->steps) != 0) {
      if (detail != nullptr) {
        for (unsigned t = 0; t < model::kMaxTracks && detail->empty(); ++t) {
          for (unsigned r = 0; r < model::kMaxPatternLength; ++r) {
            if (std::memcmp(&pa->steps[t][r], &pb->steps[t][r], sizeof(model::Step)) != 0) {
              *detail = "pattern " + std::to_string(id) + " track " + std::to_string(t) + " row " + std::to_string(r);
              break;
            }
          }
        }
      }
      return "a pattern step changed outside the selection";
    }
  }
  if (a.song_length() != b.song_length()) {
    return "the song order changed outside the selection";
  }
  for (unsigned i = 0; i < a.song_length(); ++i) {
    if (a.order(i) != b.order(i)) {
      return "the song order changed outside the selection";
    }
  }
  for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* ia = a.instrument(i);
    const model::Instrument* ib = b.instrument(i);
    if ((ia == nullptr) != (ib == nullptr) || (ia != nullptr && std::memcmp(ia, ib, sizeof *ia) != 0)) {
      return "an instrument changed outside the selection";
    }
  }
  return "the document changed outside the selection";
}

// ---- The entries ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

using app::ScreenId;

bool pattern_edit_ok(const SelImage& b, const SelContext&) { return b.editable; }
bool pattern_any(const SelImage&, const SelContext&) { return true; }
bool fill_ok(const SelImage& b, const SelContext&) { return b.fill_open && b.editable; }
bool paste_ok(const SelImage& b, const SelContext& c) { return b.editable && c.clip != nullptr && c.clip->rows > 0; }
bool song_ok(const SelImage& b, const SelContext&) { return !b.song_in_map && b.doc != nullptr && b.doc->song_length() > 0; }
bool wave_ok(const SelImage& b, const SelContext&) {
  const model::Instrument* in = b.doc != nullptr ? b.doc->instrument(static_cast<unsigned>(b.slot)) : nullptr;
  return in != nullptr && in->kind == model::InstrumentKind::Sample;
}
bool chop_ok(const SelImage& b, const SelContext& c) { return wave_ok(b, c) && b.wave_chop; }
bool sedit_ok(const SelImage& b, const SelContext& c) { return wave_ok(b, c) && b.pcm_ok && !b.pcm.empty(); }
bool instrument_ok(const SelImage& b, const SelContext&) { return b.doc != nullptr && b.doc->instrument(static_cast<unsigned>(b.slot)) != nullptr; }
bool instrument_paste_ok(const SelImage& b, const SelContext& c) { return b.doc != nullptr && c.copied_instrument >= 0; }

// A pattern rectangle: the block's tracks and rows (clipped to the tracks that exist).
void span_block(const SelImage& b, std::uint8_t fields, Span& sp) { sp.cells.push_back({b.pattern, static_cast<unsigned>(b.track0), static_cast<unsigned>(b.track1), static_cast<unsigned>(b.row0), static_cast<unsigned>(b.row1), fields}); }

void span_none(const SelImage&, int, const SelContext&, Span&) {}
void span_block_all(const SelImage& b, int, const SelContext&, Span& sp) { span_block(b, block_fields(b), sp); }
void span_block_note(const SelImage& b, int, const SelContext&, Span& sp) {
  // OS-009 step.note_delta: a single step may also take the last-used instrument (an empty step + gives note 60 and the instrument); several steps change the notes only.
  span_block(b, (b.row0 == b.row1 && b.track0 == b.track1) ? static_cast<std::uint8_t>(kFNote | kFInstr) : kFNote, sp);
}
void span_block_note_instr(const SelImage& b, int, const SelContext&, Span& sp) { span_block(b, kFNote | kFInstr, sp); }
void span_block_vol(const SelImage& b, int, const SelContext&, Span& sp) { span_block(b, kFVol | kFNote | kFInstr, sp); }  // an empty step takes the note 60 and the instrument with its first volume (OS-009 table, step.instrument)
void span_block_fx(const SelImage& b, int, const SelContext&, Span& sp) { span_block(b, kFFx1 | kFFx2, sp); }
void span_block_clear(const SelImage& b, int, const SelContext&, Span& sp) { span_block(b, block_fields(b), sp); }

// Insert and Pull: the selected tracks from the block's first row (the cursor's row with no block; maintainer 2026-10-22, OS-009) through step 127.
void span_from_cursor(const SelImage& b, int, const SelContext&, Span& sp) {
  sp.cells.push_back({b.pattern, static_cast<unsigned>(b.track0), static_cast<unsigned>(b.track1), static_cast<unsigned>(b.row0), model::kMaxPatternLength - 1u, block_fields(b)});
}

// Paste: the clip's rectangle at the cursor, whatever the selection.
void span_paste(const SelImage& b, int, const SelContext& c, Span& sp) {
  if (c.clip == nullptr || c.clip->rows <= 0) {
    return;
  }
  sp.cells.push_back({b.pattern, static_cast<unsigned>(b.cur_track), static_cast<unsigned>(b.cur_track + c.clip->tracks - 1), static_cast<unsigned>(b.cur_row), static_cast<unsigned>(b.cur_row + c.clip->rows - 1), kFAll});
}

// The Fill dialog (OS-PTOOLS, OS-FILL2; OS-PTOOLS2 S3c: no SCOPE row, the selection is the target): the block says the places — the cursor step with none, a range, the whole column of Shift + Up at row 1 (its rows are 0..steps-1), every
// track when that selection covers them all; "each track is cut at its own length (D7)". WHAT says the fields: NOTE and INSTR write the step by the single-step rule (an empty step also takes the last instrument or the note 60), VOLUME the
// volume (the single-step rule too), FX 1 and FX 2 only their own column ("only the FX column is written. The note, instrument and volume stay").
void span_fill(const SelImage& b, int, const SelContext&, Span& sp) {
  std::uint8_t fields = kFNote | kFInstr;
  fields = b.fill_what == 2 ? static_cast<std::uint8_t>(kFVol | kFNote | kFInstr) : fields;
  fields = b.fill_what == 3 ? kFFx1 : fields;
  fields = b.fill_what == 4 ? kFFx2 : fields;
  span_block(b, fields, sp);
}

// DUPLICATE, EXPAND, SHRINK: the block's tracks (every active track with no selection) over their whole column and their lengths (PAT-OPS Q1 a).
void span_pattern_op(const SelImage& b, int, const SelContext&, Span& sp) {
  const unsigned t0 = b.selected ? static_cast<unsigned>(b.track0) : 0u;
  const unsigned t1 = b.selected ? static_cast<unsigned>(b.track1) : (active_tracks(b) > 0 ? active_tracks(b) - 1u : 0u);
  sp.cells.push_back({b.pattern, t0, t1, 0, model::kMaxPatternLength - 1u, kFAll});
  sp.lengths.push_back({b.pattern, t0, t1});
}

// ---- Pattern: the inside ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

template <typename Fn>
const char* each_block_cell(const SelImage& b, Fn fn) {
  for (int t = b.track0; t <= b.track1 && t < static_cast<int>(model::kMaxTracks); ++t) {
    const unsigned len = track_len(b, b.pattern, static_cast<unsigned>(t));
    for (int r = b.row0; r <= b.row1 && r < static_cast<int>(len); ++r) {  // (a step past the track's end is not written: D7)
      if (const char* why = fn(static_cast<unsigned>(t), static_cast<unsigned>(r))) {
        return why;
      }
    }
  }
  return nullptr;
}

// OS-PTOOLS D7: "each track is cut at its own length": a fill writes no step past the end of its track. The target is the selection (OS-PTOOLS2 S3c), so the tracks are the block's.
const char* inside_fill(const SelImage& b, const SelImage& a, int, const SelContext&) {
  const unsigned t0 = static_cast<unsigned>(b.track0);
  const unsigned t1 = static_cast<unsigned>(b.track1);
  for (unsigned t = t0; t <= t1 && t < model::kMaxTracks; ++t) {
    for (unsigned r = track_len(b, b.pattern, t); r < model::kMaxPatternLength; ++r) {
      if (!same(cell(a, b.pattern, t, r), cell(b, b.pattern, t, r))) {
        return "a fill wrote a step past the end of its track";
      }
    }
  }
  return nullptr;
}

// OS-009 table, step.clear: "Note/Instr = note empty and instrument 0; FX = type 0" and the Vol column the volume. The default mask comes from the column under the cursor.
const char* inside_clear(const SelImage& b, const SelImage& a, int, const SelContext&) {
  return each_block_cell(b, [&](unsigned t, unsigned r) -> const char* {
    const model::Step s = cell(a, b.pattern, t, r);
    switch (b.cur_col) {
      case 2: return s.vol == model::kVolumeUnset ? nullptr : "a cleared step still holds its volume";
      case 3: return s.fx[0].type == model::kFxNone ? nullptr : "a cleared step still holds its first effect";
      case 4: return s.fx[1].type == model::kFxNone ? nullptr : "a cleared step still holds its second effect";
      default: return s.note == model::kNoteEmpty && s.instr == 0 ? nullptr : "a cleared step still holds its note or instrument";
    }
  });
}

// OS-009 table, step.note_delta (multi: "only notes, clamp 0..127"; a note is the bytes 1..128, the Off, Cut and Fade bytes and an empty step are not notes). Single step: the table's rank-order rule
// (an empty step with + gets note 60 and the last-used instrument) is not judged here, only a note that is there.
const char* inside_note_delta_by(const SelImage& b, const SelImage& a, int delta) {
  const bool single = b.row0 == b.row1 && b.track0 == b.track1;
  return each_block_cell(b, [&](unsigned t, unsigned r) -> const char* {
    const model::Step was = cell(b, b.pattern, t, r);
    const model::Step now = cell(a, b.pattern, t, r);
    if (!is_note(was.note)) {
      return single || same(was, now) ? nullptr : "a step with no note changed";
    }
    // A single step goes on below C-0 into Empty, Fade, Cut and Off (OS-009 rank order; model::edit::note_delta clamps at -4, not at C-0): it stops being a note there (OS-BUG-R3SEL: judged as a clamp
    // at C-0 before, which the table gives to several steps only). The rank order itself stays unjudged; above C-0 the clamp at 127 holds for both.
    if (single && static_cast<int>(was.note) + delta < static_cast<int>(model::kNoteFirst)) {
      if (is_note(now.note)) {
        return "a single step moved below C-0 is still a note";
      }
      return nullptr;
    }
    const int want = std::clamp(static_cast<int>(was.note) + delta, static_cast<int>(model::kNoteFirst), static_cast<int>(model::kNoteLast));
    if (now.note != want) {
      return "a note was not moved by the delta, clamped to 0..127";
    }
    if (!single && (now.instr != was.instr || now.vol != was.vol || std::memcmp(now.fx, was.fx, sizeof now.fx) != 0)) {
      return "a field other than the note changed";
    }
    return nullptr;
  });
}
const char* inside_note_delta(const SelImage& b, const SelImage& a, int arg, const SelContext&) { return inside_note_delta_by(b, a, arg); }
const char* inside_note_octave(const SelImage& b, const SelImage& a, int arg, const SelContext&) { return inside_note_delta_by(b, a, std::clamp(arg, -10, 10) * 12); }

// OS-009 table, step.note: "multi: only steps that already have a note change". A step with a note takes the new one; an empty step stays as it was. (Off, Cut and Fade: the spec does not say.)
const char* inside_note_set(const SelImage& b, const SelImage& a, int arg, const SelContext&) {
  if (b.row0 == b.row1 && b.track0 == b.track1) {
    return nullptr;  // the single-step branch (an empty step takes the note and the last-used instrument): the table's other rule
  }
  return each_block_cell(b, [&](unsigned t, unsigned r) -> const char* {
    const model::Step was = cell(b, b.pattern, t, r);
    const model::Step now = cell(a, b.pattern, t, r);
    if (is_note(was.note)) {
      return now.note == arg ? nullptr : "a step with a note did not take the note";
    }
    if (was.note == model::kNoteEmpty) {
      return same(was, now) ? nullptr : "an empty step changed";
    }
    return nullptr;
  });
}

// OS-PATUX OS-PATCOPY: "the clip's top-left step goes on the cursor ... whole steps overwrite what is there. Clipped at each track's own end and at the last track: what does not fit is dropped".
const char* inside_paste(const SelImage& b, const SelImage& a, int, const SelContext& c) {
  if (c.clip == nullptr) {
    return nullptr;
  }
  for (int j = 0; j < c.clip->tracks; ++j) {
    const int t = b.cur_track + j;
    if (t >= static_cast<int>(active_tracks(b))) {
      continue;  // past the last track: dropped
    }
    for (int i = 0; i < c.clip->rows; ++i) {
      const int r = b.cur_row + i;
      const bool fits = r < static_cast<int>(track_len(b, b.pattern, static_cast<unsigned>(t)));
      const model::Step want = fits ? c.clip->at(j, i) : cell(b, b.pattern, static_cast<unsigned>(t), static_cast<unsigned>(r));
      if (!same(cell(a, b.pattern, static_cast<unsigned>(t), static_cast<unsigned>(r)), want)) {
        return fits ? "a pasted step is not the clip's step" : "a step past the track's end was written";
      }
    }
  }
  return nullptr;
}

// OS-009 table, step.insert: "shifts the selected tracks down from the block's first row (the cursor with no block) through step 127 and clears that step (steps pushed past the length are kept)";
// OS-PATUX: "a track whose end is above that row is left alone".
const char* inside_insert(const SelImage& b, const SelImage& a, int, const SelContext&) {
  for (int t = b.track0; t <= b.track1 && t < static_cast<int>(model::kMaxTracks); ++t) {
    const unsigned u = static_cast<unsigned>(t);
    const bool alone = b.row0 >= static_cast<int>(track_len(b, b.pattern, u));
    for (unsigned r = static_cast<unsigned>(b.row0); r < model::kMaxPatternLength; ++r) {
      const model::Step want = alone ? cell(b, b.pattern, u, r) : (r == static_cast<unsigned>(b.row0) ? kEmpty : cell(b, b.pattern, u, r - 1u));
      if (!same_in(b, cell(a, b.pattern, u, r), want, cell(b, b.pattern, u, r))) {
        return alone ? "a track that ends above the block's first row was changed" : "the steps were not shifted down from the block's first row with that step cleared";
      }
    }
  }
  return nullptr;
}

// OS-DELPULL: "deletes the step at the block's first row (the cursor with no block) and moves every selected track up from there through step 127 (the last step is cleared)".
const char* inside_pull(const SelImage& b, const SelImage& a, int, const SelContext&) {
  for (int t = b.track0; t <= b.track1 && t < static_cast<int>(model::kMaxTracks); ++t) {
    const unsigned u = static_cast<unsigned>(t);
    const bool alone = b.row0 >= static_cast<int>(track_len(b, b.pattern, u));
    for (unsigned r = static_cast<unsigned>(b.row0); r < model::kMaxPatternLength; ++r) {
      const model::Step want = alone ? cell(b, b.pattern, u, r) : (r + 1u < model::kMaxPatternLength ? cell(b, b.pattern, u, r + 1u) : kEmpty);
      if (!same_in(b, cell(a, b.pattern, u, r), want, cell(b, b.pattern, u, r))) {
        return alone ? "a track that ends above the block's first row was changed" : "the steps were not pulled up from the block's first row with the last step cleared";
      }
    }
  }
  return nullptr;
}

// OS-009 P1b: INVERT "swaps symmetric steps of each selected track": step r0 + i takes the old step r1 - i.
const char* inside_invert(const SelImage& b, const SelImage& a, int, const SelContext&) {
  for (int t = b.track0; t <= b.track1 && t < static_cast<int>(model::kMaxTracks); ++t) {
    const unsigned u = static_cast<unsigned>(t);
    if (b.row1 >= static_cast<int>(track_len(b, b.pattern, u))) {
      continue;  // a block that runs past this track's end: the spec does not say how the track is cut
    }
    for (int r = b.row0; r <= b.row1; ++r) {
      if (!same_in(b, cell(a, b.pattern, u, static_cast<unsigned>(r)), cell(b, b.pattern, u, static_cast<unsigned>(b.row0 + b.row1 - r)), cell(b, b.pattern, u, static_cast<unsigned>(r)))) {
        return "the steps of a track were not reversed";
      }
    }
  }
  return nullptr;
}

// PAT-OPS: DUPLICATE: length 2L, steps L..2L-1 repeat steps 0..L-1. EXPAND: length 2L, step i goes to 2i, the odd steps are empty. SHRINK: length ceil(L/2), step i takes old step 2i, the steps from the new length
// up to the old one are cleared. All three leave the steps past both lengths as they were; a result over 128 steps is refused (nothing changes). Scope: the block's tracks, every active track with none.
const char* inside_pattern_op(const SelImage& b, const SelImage& a, int arg, const SelContext&) {
  const unsigned t0 = b.selected ? static_cast<unsigned>(b.track0) : 0u;
  const unsigned t1 = b.selected ? static_cast<unsigned>(b.track1) : (active_tracks(b) > 0 ? active_tracks(b) - 1u : 0u);
  bool refused = false;
  for (unsigned t = t0; t <= t1; ++t) {
    const unsigned len = track_len(b, b.pattern, t);
    refused = refused || (arg != 2 && 2u * len > model::kMaxPatternLength);
  }
  for (unsigned t = t0; t <= t1; ++t) {
    const unsigned len = track_len(b, b.pattern, t);
    const unsigned out = refused ? len : (arg == 2 ? (len + 1u) / 2u : 2u * len);
    if (track_len(a, b.pattern, t) != out) {
      return refused ? "a refused operation changed a track length" : "the track length is not what DUPLICATE, EXPAND or SHRINK makes";
    }
    if (refused) {
      continue;
    }
    for (unsigned r = 0; r < model::kMaxPatternLength; ++r) {
      model::Step want = cell(b, b.pattern, t, r);
      if (arg == 0 && r >= len && r < 2u * len) {
        want = cell(b, b.pattern, t, r - len);
      } else if (arg == 1 && r < 2u * len) {
        want = (r % 2u == 0u) ? cell(b, b.pattern, t, r / 2u) : kEmpty;
      } else if (arg == 2 && r < len) {
        want = r < out ? cell(b, b.pattern, t, 2u * r) : kEmpty;
      }
      if (!same(cell(a, b.pattern, t, r), want)) {
        return "the steps are not what DUPLICATE, EXPAND or SHRINK makes";
      }
    }
  }
  return nullptr;
}

// ---- Song: the inside ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> order_of(const SelImage& im) {
  std::vector<std::uint8_t> v;
  for (unsigned i = 0; i < im.doc->song_length(); ++i) {
    v.push_back(static_cast<std::uint8_t>(im.doc->order(i)));
  }
  return v;
}

void span_order(const SelImage&, int, const SelContext&, Span& sp) { sp.order = true; }

// OS-SONGSHARE S2 and SONG-UX Delete / F4: the range (no range: the cursor's slot) leaves the list. All the slots: refused, the list stays.
const char* inside_song_drop(const SelImage& b, const SelImage& a, int, const SelContext&) {
  std::vector<std::uint8_t> want = order_of(b);
  const int count = static_cast<int>(want.size());
  const int first = std::clamp(b.song_first, 0, count - 1);
  const int last = std::clamp(b.song_last, first, count - 1);
  if (last - first + 1 < count) {
    want.erase(want.begin() + first, want.begin() + last + 1);
  }
  return order_of(a) == want ? nullptr : "the slots that left the list are not the range";
}

// OS-SONGSHARE S3: "the range moves one position. The slot it steps over goes to the other side of the range". At the top or the bottom nothing happens.
const char* inside_song_move(const SelImage& b, const SelImage& a, int arg, const SelContext&) {
  std::vector<std::uint8_t> want = order_of(b);
  const int count = static_cast<int>(want.size());
  const int first = std::clamp(b.song_first, 0, count - 1);
  const int last = std::clamp(b.song_last, first, count - 1);
  const int over = arg < 0 ? first - 1 : last + 1;
  if (over >= 0 && over < count) {
    if (arg < 0) {
      std::rotate(want.begin() + over, want.begin() + first, want.begin() + last + 1);
    } else {
      std::rotate(want.begin() + first, want.begin() + last + 1, want.begin() + over + 1);
    }
  }
  return order_of(a) == want ? nullptr : "the range did not move one position with the slot it stepped over on its other side";
}

// ---- Wave: the inside ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr unsigned kPointEnd = 65535;  // the points are 0..65535 along the sample (app/wave_math.h)

const model::Instrument* inst_of(const SelImage& im) { return im.doc != nullptr ? im.doc->instrument(static_cast<unsigned>(im.slot)) : nullptr; }

void span_slices(const SelImage& b, int, const SelContext&, Span& sp) { sp.inst_slices = b.slot; }
void span_nudge(const SelImage& b, int, const SelContext&, Span& sp) {
  if (b.wave_chop) {
    sp.inst_slices = b.slot;
  } else {
    sp.inst_points = b.slot;
  }
}

std::vector<unsigned> slices_of(const model::Instrument& in) {
  std::vector<unsigned> v;
  for (unsigned i = 0; i < in.sampler.slice_count && i < model::kSlices; ++i) {
    v.push_back(in.sampler.slices[i]);
  }
  return v;
}

// The maintainer's decision of 2026-10-21 (OS-BUG-ADDSLICE): ADD puts the new slice at the middle of the selected slice's start and end, so it splits the selected slice in two. With no slice yet the selected slice
// is the whole sample (the first ADD also puts a slice at 0). With ZC on the point may move to a zero crossing near the middle: then it has to lie between the start and the end.
const char* inside_wave_add(const SelImage& b, const SelImage& a, int, const SelContext&) {
  const model::Instrument* ib = inst_of(b);
  const model::Instrument* ia = inst_of(a);
  if (ib == nullptr || ia == nullptr) {
    return nullptr;
  }
  std::vector<unsigned> was = slices_of(*ib);
  const std::vector<unsigned> now = slices_of(*ia);
  const unsigned sel = b.wave_slice;
  unsigned start = 0;
  unsigned end = kPointEnd;
  if (!was.empty()) {
    start = sel < was.size() ? was[sel] : was.back();
    end = sel + 1u < was.size() ? was[sel + 1u] : kPointEnd;
  } else {
    was.push_back(0);  // the slice at 0 that the first ADD makes
  }
  if (now.size() != was.size() + 1u) {
    return "no slice was added between the selected start and end";
  }
  unsigned added = 0;
  std::vector<unsigned> rest = now;
  for (unsigned p : was) {
    const auto it = std::find(rest.begin(), rest.end(), p);
    if (it == rest.end()) {
      return "a slice other than the selected one moved when a slice was added";
    }
    rest.erase(it);
  }
  added = rest.empty() ? 0u : rest[0];
  if (!(added > start && added < end)) {
    return "the new slice is not between the selected slice's start and end";
  }
  const unsigned mid = (start + end) / 2u;
  if (!b.wave_zc && (added + 1u < mid || added > mid + 1u)) {
    return "the new slice is not at the middle of the selected slice";
  }
  return nullptr;
}

// OS-011a F5 DEL: "the selected slice goes".
const char* inside_wave_del(const SelImage& b, const SelImage& a, int, const SelContext&) {
  const model::Instrument* ib = inst_of(b);
  const model::Instrument* ia = inst_of(a);
  if (ib == nullptr || ia == nullptr) {
    return nullptr;
  }
  std::vector<unsigned> want = slices_of(*ib);
  if (b.wave_slice < want.size()) {
    want.erase(want.begin() + b.wave_slice);
  }
  return slices_of(*ia) == want ? nullptr : "the slices left are not the slices without the selected one";
}

// OS-011a: the encoder moves "the selected point (WAVE) or slice (CHOP)": the others stay.
const char* inside_wave_nudge(const SelImage& b, const SelImage& a, int, const SelContext&) {
  const model::Instrument* ib = inst_of(b);
  const model::Instrument* ia = inst_of(a);
  if (ib == nullptr || ia == nullptr) {
    return nullptr;
  }
  if (b.wave_chop) {
    const std::vector<unsigned> was = slices_of(*ib);
    const std::vector<unsigned> now = slices_of(*ia);
    if (was.size() != now.size()) {
      return "the number of slices changed when one was moved";
    }
    for (unsigned i = 0; i < was.size(); ++i) {
      if (i != b.wave_slice && was[i] != now[i]) {
        return "a slice other than the selected one moved";
      }
    }
    return nullptr;
  }
  // QA-WAV-015 (OS-011a): "a loop point is pushed ahead of the start and pulled in behind the end by the same edit": the points stay in order (start <= loop start < loop end <= end), and a point that the moved one
  // would pass is carried along. So another point may change, but only on the side the selected point moved towards.
  const int was[4] = {ib->sampler.start, ib->sampler.loop_start, ib->sampler.loop_end, ib->sampler.end};
  const int now[4] = {ia->sampler.start, ia->sampler.loop_start, ia->sampler.loop_end, ia->sampler.end};
  const unsigned sel = b.wave_point < 4u ? b.wave_point : 0u;
  const int dir = now[sel] > was[sel] ? 1 : (now[sel] < was[sel] ? -1 : 0);
  for (unsigned i = 0; i < 4; ++i) {
    if (i != sel && was[i] != now[i] && !((i > sel && dir > 0) || (i < sel && dir < 0))) {
      return "a point other than the selected one moved";
    }
  }
  return nullptr;
}

// ---- Sample edit: the inside -----------------------------------------------------------------------------------------------------------------------------------------------------------------------

void range_of(const SelImage& b, std::uint32_t& lo, std::uint32_t& hi) {
  const std::uint32_t n = static_cast<std::uint32_t>(b.pcm.size());
  lo = std::min(b.sed_a, b.sed_b);
  hi = std::max(b.sed_a, b.sed_b);
  hi = std::min(hi, n);
  if (lo >= hi) {  // no selection: the whole sample (OS-SEDFX: "the selection [a, b) when there is one, else the whole sample")
    lo = 0;
    hi = n;
  }
}

void span_sedit(const SelImage& b, int arg, const SelContext&, Span& sp) {
  std::uint32_t lo = 0, hi = 0;
  range_of(b, lo, hi);
  sp.pcm_slot = b.slot;
  sp.pcm_a = lo;
  sp.pcm_b = hi;
  sp.pcm_frames_only = arg != 0;  // Crop (EditOp 0) changes the length: everything outside the selection goes by definition
  sp.pcm_length_may_change = arg == 0;
}

// OS-SEDFX 2: "Frames outside the range are copied as they are". The inside, where the specs say it: REVERSE reverses the range, CROP keeps the range, NORMALISE brings the range's peak to 32767 (or leaves it
// when it is at full scale or silent), DC REMOVE leaves a range whose mean is below 1 (the removal is partial only where full-scale frames hold it back), AMPLIFY multiplies by the percentage (clamped),
// a fade never makes a frame louder. (EditOp: 0 Crop, 1 Normalise, 2 Reverse, 3 FadeIn, 4 FadeOut, 5 Amplify, 7 DcRemove.)
const char* inside_sedit(const SelImage& b, const SelImage& a, int arg, const SelContext&) {
  std::uint32_t lo = 0, hi = 0;
  range_of(b, lo, hi);
  if (!a.pcm_ok) {
    return nullptr;
  }
  const std::vector<std::int16_t>& in = b.pcm;
  const std::vector<std::int16_t>& out = a.pcm;
  if (arg == 0) {
    return out.size() == hi - lo && std::equal(out.begin(), out.end(), in.begin() + lo) ? nullptr : "the sample after a crop is not the selection";
  }
  if (out.size() != in.size()) {
    return "the length of the sample changed";
  }
  switch (arg) {
    case 2:
      for (std::uint32_t i = 0; i < hi - lo; ++i) {
        if (out[lo + i] != in[hi - 1u - i]) {
          return "the selection was not reversed";
        }
      }
      return nullptr;
    case 1: {
      int peak_in = 0, peak_out = 0;
      for (std::uint32_t i = lo; i < hi; ++i) {
        peak_in = std::max(peak_in, std::abs(static_cast<int>(in[i])));
        peak_out = std::max(peak_out, std::abs(static_cast<int>(out[i])));
      }
      if (peak_in == 0 || peak_in >= 32767) {
        return std::equal(out.begin(), out.end(), in.begin()) ? nullptr : "a sample at full scale or silent was changed by NORMALISE";
      }
      return peak_out >= 32767 ? nullptr : "the peak of the selection is not 32767 after NORMALISE";
    }
    case 5: {
      const int pct = static_cast<int>(b.sed_amp);
      for (std::uint32_t i = lo; i < hi; ++i) {
        const long want = std::clamp(static_cast<long>(in[i]) * pct / 100, -32768L, 32767L);
        if (std::labs(static_cast<long>(out[i]) - want) > 1) {
          return "a frame of the selection is not the amplified frame";
        }
      }
      return nullptr;
    }
    case 3:
    case 4:
      for (std::uint32_t i = lo; i < hi; ++i) {
        if (std::abs(static_cast<int>(out[i])) > std::abs(static_cast<int>(in[i])) + 1) {
          return "a fade made a frame louder";
        }
      }
      return nullptr;
    case 7: {
      long long sum = 0;
      bool full = false;
      for (std::uint32_t i = lo; i < hi; ++i) {
        sum += out[i];
        full = full || in[i] >= 32767 || in[i] <= -32768;
      }
      const long long n = static_cast<long long>(hi - lo);
      return (n == 0 || std::llabs(sum) < n || full) ? nullptr : "the selection still has a DC offset after DC REMOVE";
    }
    default:
      return nullptr;
  }
}

// ---- Instrument: the selected slot -----------------------------------------------------------------------------------------------------------------------------------------------------------------

void span_instrument(const SelImage& b, int, const SelContext&, Span& sp) {
  sp.inst_all = b.slot;
  sp.pcm_slot = b.slot;
  sp.pcm_length_may_change = true;
}

// OS-INSTUI S5: Shift + F4 "the instrument leaves its slot".
const char* inside_instrument_delete(const SelImage&, const SelImage& a, int, const SelContext&) {
  return inst_of(a) == nullptr ? nullptr : "the instrument did not leave its slot";
}

// Shift + Delete: "every parameter back to its default, kind / sample / module / name kept".
const char* inside_instrument_init(const SelImage& b, const SelImage& a, int, const SelContext&) {
  const model::Instrument* ib = inst_of(b);
  const model::Instrument* ia = inst_of(a);
  if (ib == nullptr || ia == nullptr) {
    return nullptr;
  }
  const bool kept = ib->kind == ia->kind && ib->module_index == ia->module_index && std::strcmp(ib->name, ia->name) == 0 && std::strcmp(ib->sample_path, ia->sample_path) == 0;
  return kept ? nullptr : "init changed the kind, sample, module or name";
}

// Shift + F3 PASTE: "into an empty slot or over a used one": the slot then holds what Copy took.
const char* inside_instrument_paste(const SelImage& b, const SelImage& a, int, const SelContext& c) {
  const model::Instrument* src = c.copied_instrument >= 0 ? b.doc->instrument(static_cast<unsigned>(c.copied_instrument)) : nullptr;
  const model::Instrument* now = inst_of(a);
  if (src == nullptr) {
    return nullptr;
  }
  if (now == nullptr) {
    return "the pasted slot is empty";
  }
  // (A sample instrument shares the copied audio and gets this slot's own sample file name: OS-INSTUI S6, so the path is not compared.)
  const bool same_inst = now->kind == src->kind && now->module_index == src->module_index && std::memcmp(&now->sampler, &src->sampler, sizeof now->sampler) == 0;
  return same_inst ? nullptr : "the pasted instrument is not the copied one";
}

}  // namespace

// ---- The table ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

const std::vector<SelOp>& selection_ops() {
  static const std::vector<SelOp> table = {
      // Pattern: the block (Selection::block: the cursor step with no range).
      {"block.copy", ScreenId::Pattern, "OS-PATCOPY: the block into the clipboard, \"nothing is changed (no undo level)\"", "(none: the span is empty, a copy changes nothing)", pattern_any, span_none, nullptr},
      {"block.paste", ScreenId::Pattern, "OS-PATCOPY: the clip at the cursor, whole steps, clipped at each track's end", "", paste_ok, span_paste, inside_paste},
      {"step.clear", ScreenId::Pattern, "OS-009 table: the block's elements by the column under the cursor", "what else of a step Delete clears in the Note column (\"the whole step\")", pattern_edit_ok, span_block_clear, inside_clear},
      {"step.note_delta", ScreenId::Pattern, "OS-009 table: several steps, only notes, clamp 0..127", "the single step's rank order (Off < Cut < Fade < Empty < C-0)", pattern_edit_ok, span_block_note, inside_note_delta},
      {"step.note_octave", ScreenId::Pattern, "OS-009 table: the same rule, 12 notes per click, per-note clamp (Q-P1-4)", "the single step's rank order", pattern_edit_ok, span_block_note, inside_note_octave},
      {"step.note", ScreenId::Pattern, "OS-009 table: several steps, only steps that already have a note change", "Off, Cut and Fade steps, and the instrument of a changed step", pattern_edit_ok, span_block_note_instr, inside_note_set},
      {"step.instrument", ScreenId::Pattern, "OS-009 table: the Instr column", "the several-step branch: the table gives the single step's rule only", pattern_edit_ok, span_block_note_instr, nullptr},
      {"step.instrument_delta", ScreenId::Pattern, "OS-009 table: the Instr column", "the several-step branch", pattern_edit_ok, span_block_note_instr, nullptr},
      {"step.instrument_coarse", ScreenId::Pattern, "OS-009 table (OS-SHIFTENC): the Instr column, 8 places", "the several-step branch", pattern_edit_ok, span_block_note_instr, nullptr},
      {"step.vol_delta", ScreenId::Pattern, "OS-009 table: the Vol column, the same single / multi rule as an FX value", "the several-step branch", pattern_edit_ok, span_block_vol, nullptr},
      {"step.vol_coarse", ScreenId::Pattern, "OS-009 table (OS-SHIFTENC): the Vol column, 10 a click", "the several-step branch", pattern_edit_ok, span_block_vol, nullptr},
      {"step.fx_value_delta", ScreenId::Pattern, "OS-009 table: the FX value, clamped to the type's range", "the several-step branch", pattern_edit_ok, span_block_fx, nullptr},
      {"step.fx_value_coarse", ScreenId::Pattern, "OS-009 table (OS-SHIFTENC): the FX value, 16 a click", "the several-step branch", pattern_edit_ok, span_block_fx, nullptr},
      {"step.fx_type_delta", ScreenId::Pattern, "OS-009 table: the FX type on the cursor's FX column", "the several-step branch", pattern_edit_ok, span_block_fx, nullptr},
      {"step.note_pad", ScreenId::Pattern, "OS-009 table: the pad enters step.note with the pad board's note", "the note of the pad board; the several-step branch", pattern_edit_ok, span_block_note_instr, nullptr},
      {"step.vol", ScreenId::Pattern, "OS-009 table: the Vol column, the same single / multi rule as an FX value", "the several-step branch", pattern_edit_ok, span_block_vol, nullptr},
      {"step.vol_pad", ScreenId::Pattern, "OS-009 table: the Vol column by pad", "the several-step branch", pattern_edit_ok, span_block_vol, nullptr},
      {"step.fx_value", ScreenId::Pattern, "OS-009 table: the FX value, clamped to the type's range", "the several-step branch", pattern_edit_ok, span_block_fx, nullptr},
      {"step.fx_pad", ScreenId::Pattern, "OS-009 table: the FX value by pad, scaled onto the type's range", "the several-step branch", pattern_edit_ok, span_block_fx, nullptr},
      {"step.fx_type", ScreenId::Pattern, "OS-009 table: the FX type on the cursor's FX column", "the several-step branch", pattern_edit_ok, span_block_fx, nullptr},
      {"fill.apply", ScreenId::Pattern, "OS-PTOOLS, OS-FILL2, OS-PTOOLS2 S3c: the fill writes the selection (a block, a column block, the cursor step; the track or the pattern of Shift + Up at step 1), the field WHAT names, each track cut at its own length", "which steps of the selection the type picks and what they get", fill_ok, span_fill, inside_fill},
      {"step.insert", ScreenId::Pattern, "OS-009 table, OS-PATUX: the selected tracks shift down from the block's first row through step 127", "", pattern_edit_ok, span_from_cursor, inside_insert},
      {"step.pull", ScreenId::Pattern, "OS-DELPULL: the selected tracks come up from the block's first row through step 127", "", pattern_edit_ok, span_from_cursor, inside_pull},
      {"pattern.invert", ScreenId::Pattern, "OS-009 P1b: INVERT swaps the symmetric steps of each selected track", "a block that runs past a track's end", pattern_edit_ok, span_block_all, inside_invert},
      {"pattern.op", ScreenId::Pattern, "PAT-OPS Q1 a: DUPLICATE, EXPAND, SHRINK on the block's tracks over their whole length", "", pattern_edit_ok, span_pattern_op, inside_pattern_op},
      // Song: the range of slots (the cursor's slot with no range).
      {"song.copy", ScreenId::Song, "SONG-UX: Copy puts the range into the clipboard; the list is not changed", "(none: the span is empty, a copy changes nothing)", song_ok, span_none, nullptr},
      {"song.cut", ScreenId::Song, "OS-SONGSHARE S2: the range goes into the clipboard and out of the list; all the slots: refused", "", song_ok, span_order, inside_song_drop},
      {"song.delete_slot", ScreenId::Song, "SONG-UX: F4 deletes the range; all the slots: refused (SONG NEEDS A SLOT)", "", song_ok, span_order, inside_song_drop},
      {"song.clear", ScreenId::Song, "SONG-UX: Delete outside the map is F4's DEL SLOT", "", song_ok, span_order, inside_song_drop},
      {"song.move", ScreenId::Song, "OS-SONGSHARE S3: the range moves one position, the slot it steps over to its other side", "", song_ok, span_order, inside_song_move},
      // Wave: the selected slice or point.
      {"wave.add", ScreenId::Wave, "maintainer 2026-10-21 (OS-BUG-ADDSLICE): the new slice at the middle of the selected slice's start and end", "", chop_ok, span_slices, inside_wave_add},
      {"wave.del", ScreenId::Wave, "OS-011a F5 DEL: the selected slice goes", "", chop_ok, span_slices, inside_wave_del},
      {"wave.nudge", ScreenId::Wave, "OS-011a: the encoder moves the selected point (WAVE) or slice (CHOP)", "how far it moves (zero-crossing snap)", wave_ok, span_nudge, inside_wave_nudge},
      // Sample edit: the selection [a, b) of the sample in view (the whole sample with none).
      {"sedit.run", ScreenId::SampleEdit, "OS-SEDFX 2, OS-011a: the effect works on the selection and copies the frames outside it as they are", "the fades' curve, Amiga's conversion", sedit_ok, span_sedit, inside_sedit},
      // Instrument: the selected slot.
      {"instrument.copy", ScreenId::Instrument, "OS-INSTUI S6: Copy takes the instrument; nothing is changed", "(none: the span is empty, a copy changes nothing)", instrument_ok, span_none, nullptr},
      {"instrument.paste", ScreenId::Instrument, "OS-INSTUI S6: Paste into the empty slot or over the used one, one unit", "", instrument_paste_ok, span_instrument, inside_instrument_paste},
      {"instrument.delete", ScreenId::Instrument, "OS-INSTUI S5: the instrument leaves its slot", "", instrument_ok, span_instrument, inside_instrument_delete},
      {"instrument.init", ScreenId::Instrument, "OS-INSTUI: every parameter back to its default, kind, sample, module and name kept", "the defaults themselves", instrument_ok, span_instrument, inside_instrument_init},
  };
  return table;
}

const SelOp* find_selection_op(const std::string& action) {
  for (const SelOp& op : selection_ops()) {
    if (action == op.action) {
      return &op;
    }
  }
  return nullptr;
}

const std::vector<NotJudged>& selection_ops_not_judged() {
  static const std::vector<NotJudged> table = {
      {"song.paste", "DEFERRED: its outside is the whole document but for the order, and its inside is silent on the position (before or after the cursor's slot); the walk has no case that copies slots first (the song clipboard), so it is not run yet"},
      {"song.add_slot", "DEFERRED: arg 0 changes only the order, but arg 1 (Shift + Insert) also copies the pattern into the first free number, so its span needs the pattern bank, which the masks of this table do not have yet"},
      {"song.tools", "the dialog over the map's rectangle (SONG-UX): the map's cells are not a range of slots"},
      {"song.select_tracks", "selects the tracks of the map's rectangle only; the cells' operations of the map are judged with the map (not in this slice)"},
      {"perform.copy", "the perform board's slot copy: not a selection of the document"},
      {"learn.delete", "acts on the learn list's row under the cursor: not a selection of the document"},
      {"file.delete", "acts on the file under the cursor of the card: asks first, and the card is not the document"},
  };
  return table;
}

// ---- The check ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

std::vector<Finding> check_selection(const SelImage& before, const SelImage& after, const std::string& action, int arg, const SelContext& ctx) {
  std::vector<Finding> out;
  const SelOp* op = find_selection_op(action);
  if (op == nullptr || !before.ok || !after.ok || before.doc == nullptr || after.doc == nullptr || before.screen != op->screen || !op->applies(before, ctx)) {
    return out;
  }
  auto add = [&](const char* reason, const std::string& detail) {
    out.push_back(Finding{"R3", reason});
    if (ctx.detail != nullptr) {
      *ctx.detail += detail + "\n";
    }
  };
  Span sp;
  op->span(before, arg, ctx, sp);
  // The outside: both documents with the span masked must agree.
  {
    auto b = std::make_unique<model::Project>(*before.doc);
    auto a = std::make_unique<model::Project>(*after.doc);
    if (!mask(*b, sp, before) || !mask(*a, sp, before)) {
      add("the span could not be masked: the net is wrong", "mask refused");
      return out;
    }
    const std::string db = sim::model_digest(*b);
    const std::string da = sim::model_digest(*a);
    if (db.empty() || da.empty()) {
      add("the document could not be digested", "digest");
      return out;
    }
    if (db != da) {
      std::string where;
      const char* why = where_differs(*b, *a, &where);
      add(why, std::string(op->action) + ": " + why + (where.empty() ? "" : " (first: " + where + ")"));
    }
  }
  // The audio: every slot that the span does not name is what it was; the slot it names changes only inside its frames (or in length for a crop, a delete, a paste).
  for (std::size_t i = 0; i < before.pcm_hash.size() && i < after.pcm_hash.size(); ++i) {
    if (static_cast<int>(i) != sp.pcm_slot && before.pcm_hash[i] != after.pcm_hash[i]) {
      add("the audio of a sample slot changed outside the selection", std::string(op->action) + ": slot " + std::to_string(i));
      break;
    }
  }
  if (sp.pcm_slot >= 0 && sp.pcm_frames_only && before.pcm_ok && after.pcm_ok) {
    if (before.pcm.size() != after.pcm.size()) {
      add("the audio changed in length: the selection's frames are all an effect may change", "length");
    } else {
      for (std::size_t f = 0; f < before.pcm.size(); ++f) {
        if ((f < sp.pcm_a || f >= sp.pcm_b) && before.pcm[f] != after.pcm[f]) {
          add("sample frames outside the selection changed", std::string(op->action) + ": frame " + std::to_string(f));
          break;
        }
      }
    }
  }
  // The inside: where the spec decides it. An action that left the document as it was is judged only where the state says it had to act.
  const bool acted = before.digest != after.digest || before.pcm_hash != after.pcm_hash;
  if (op->inside != nullptr && (acted || ctx.must_act)) {
    if (const char* why = op->inside(before, after, arg, ctx)) {
      add(why, std::string(op->action) + ": " + why);
    }
  }
  return out;
}

}  // namespace qanet
