// SPDX-License-Identifier: GPL-3.0-or-later
// The Pattern screen over a document (OS-009 P1a.1), as gallery and budget states: the edit mode with a block and the header marks, the
// block in the worst case, the Shift layer, and the part of the `worst` sweep that adds the marks, the layer and the block to the dearest
// content. Everything is drawn through the app's scene (app::render_scene) from a Model whose `project` is a document built here: pattern 1
// of the demo, or of the worst-case data, with the track slots named. The harness is not re-entrant (like the budget harness itself): the
// two documents are rewritten between draws.
#include "screens_app.h"

#include <o3os/app/model.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/view_state.h>
#include <o3os/model/project.h>
#include <o3os/sim/demo_project.h>

#include <cstring>
#include <memory>

namespace o3os::sim {

namespace {

using app::Model;
using app::PatternMode;
using app::ViewState;

// One document per content: the demo's pattern, and the worst-case data sets. Allocated on first use (1.9 MB each).
struct Doc {
  std::unique_ptr<model::ModelArena> arena;
  Model model;
};

Doc& doc(unsigned which) {
  static Doc docs[1 + kPatternWorstDataSets];
  Doc& d = docs[which < 1 + kPatternWorstDataSets ? which : 0];
  if (!d.arena) {
    d.arena = std::make_unique<model::ModelArena>();
    std::memset(d.arena.get(), 0, sizeof(model::ModelArena));
    model::Project& p = d.arena->project;
    p.reset();
    if (which == 0) {
      fill_demo_pattern(p);
    } else {
      const app::PatternData& data = pattern_worst_data(which - 1);
      (void)p.create_pattern(1, app::kSteps);
      for (unsigned t = 0; t < static_cast<unsigned>(app::kTracks); ++t) {
        (void)p.set_steps(1, t, 0, data.steps[t], static_cast<std::uint32_t>(app::kSteps));
        model::TrackSlot slot = *p.track(t);
        slot.type = t % 3 == 0 ? model::TrackType::Audio : (t % 3 == 1 ? model::TrackType::Midi : model::TrackType::ExternalAudio);
        (void)p.set_track(t, slot);
        (void)p.set_track_name(t, "ABCDEFGHIJKLMNOPQRST", model::kTrackNameMax);  // the longest name: the header clips it
      }
      (void)p.append_order(1);
      model::Instrument in;
      model::init_instrument(in, model::InstrumentKind::Sample);
      std::memset(in.name, 'W', model::kInstrumentNameMax);  // the longest instrument name: the strip clips it
      for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
        (void)p.set_instrument(i, in);
      }
    }
    d.model.project = &p;
    d.model.undo_depth = 12;
  }
  return d;
}

// Every track muted and soloed (or none): the header marks.
void set_marks(model::Project& p, bool on) {
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    model::TrackSlot slot = *p.track(t);
    slot.flags = on ? model::kTrackFlagsMask : std::uint8_t{0};
    (void)p.set_track(t, slot);
  }
}

// Puts the cursor on step `top` + 15 (`bottom`) or `top` (not), on track `left` + 4 (`right`) or `left` (not), in `column`. The step window is
// centred on the cursor's row (OS-PATCENTER: its first step is the cursor's row - 7, and the slots past the ends of the pattern are blank), so
// `top` and `bottom` name the cursor's row and not the window's first step; the track window scrolls at its edges, so it is reached from the far corner.
void place(ViewState& v, int top, int left, bool bottom, bool right, int column) {
  v.pattern.scroll_rows(top + 15);
  for (int i = 0; i < (left + 4) * app::kColumns; ++i) {
    v.pattern.move_right();
  }
  if (!bottom) {
    v.pattern.scroll_rows(-15);
  }
  if (!right) {
    for (int i = 0; i < 4 * app::kColumns; ++i) {
      v.pattern.move_left();
    }
  }
  for (int c = 0; c < column; ++c) {
    v.pattern.move_right();
  }
}

void set_block(ViewState& v, int row0, int track0, int row1, int track1) {
  row0 = row0 < 0 ? 0 : row0;  // (the window's first row is negative while the cursor is within seven rows of the start: OS-PATCENTER; a block lies inside the pattern)
  row1 = row1 > app::kSteps - 1 ? app::kSteps - 1 : row1;
  v.edit.selection.active = true;
  v.edit.selection.start_row = static_cast<std::int16_t>(row0);
  v.edit.selection.start_track = static_cast<std::int8_t>(track0);
  v.edit.selection.end_row = static_cast<std::int16_t>(row1);
  v.edit.selection.end_track = static_cast<std::int8_t>(track1);
}

void scene(ui::Renderer& r, const Model& model, const ViewState& view) { app::render_scene(model, view, r); }

constexpr int kRows[2] = {5, 40};
constexpr int kTracks3[3] = {0, 6, 11};
constexpr unsigned kDocSets = kPatternWorstDataSets;
constexpr unsigned kMarkSettings = 2;  // none, every mark and the Shift layer
constexpr unsigned kBlockKinds = 3;    // none, a few steps of a few tracks, the whole window
static_assert(kPatternDocWorstVariants == kDocSets * kMarkSettings * kBlockKinds * 6, "the sweep's size");

}  // namespace

const app::Model& pattern_doc_model(unsigned which, bool marks) {
  Doc& d = doc(which);
  set_marks(d.arena->project, marks);
  return d.model;
}

// Part 3 of the worst-case sweep: the document, the dearest content, the header marks and the Shift layer on or off, no block, a small block
// and the window's block, at six cursor windows.
void draw_pattern_doc_worst(ui::Renderer& r, unsigned variant) {
  const unsigned window = variant % 6;
  variant /= 6;
  const unsigned block = variant % kBlockKinds;
  variant /= kBlockKinds;
  const bool marks = variant % kMarkSettings != 0;
  variant /= kMarkSettings;
  const unsigned set = variant % kDocSets;

  const Model& model = pattern_doc_model(1 + set, marks);
  const int row = kRows[window % 2];
  const int track = kTracks3[window / 2];
  ViewState v;
  v.pattern.scroll_rows(row);
  for (int i = 0; i < track * app::kColumns; ++i) {
    v.pattern.move_right();
  }
  v.shift = marks;
  v.mode[app::screen_index(app::ScreenId::Pattern)] = static_cast<std::uint8_t>(PatternMode::Edit);  // (a block is made in Edit too: the Select mode is gone, OS-LIVEREC)
  if (block == 1) {
    set_block(v, row - 3, track > 0 ? track - 1 : 0, row + 3, track < app::kTracks - 1 ? track + 1 : track);
  } else if (block == 2) {
    const int left = v.pattern.left_track();
    const int top = v.pattern.top_row();
    set_block(v, top, left, top + 15, left + 4);
  }
  scene(r, model, v);
}

// OS-ZOOM: the Pattern grid at zoom level 1 (8 tracks, Note, Instr, Vol, Label tiles) and 2 (12 tracks, Note, Micro tiles) over the dearest content, with and without the header marks and a block over the
// whole window, the cursor at the first, a middle and the last track. The budget pins the words of the smaller tiles (a note past G10 is three glyph words there, an empty instrument or volume two).
void draw_pattern_zoom(ui::Renderer& r, int level, unsigned variant) {
  const unsigned window = variant % 3;
  variant /= 3;
  const bool block = variant % 2 != 0;
  variant /= 2;
  const bool marks = variant % 2 != 0;
  variant /= 2;
  const unsigned set = variant % kDocSets;
  const Model& model = pattern_doc_model(1 + set, marks);
  const app::PatternView::Dims d{app::kSteps, app::kTracks};
  ViewState v;
  (void)v.pattern.set_zoom(level, d);
  (void)v.pattern.jump_row(window == 1 ? 40 : 5, d);
  (void)v.pattern.move_track(window == 0 ? 0 : (window == 1 ? 6 : 11), d);
  v.shift = marks;
  v.mode[app::screen_index(app::ScreenId::Pattern)] = static_cast<std::uint8_t>(PatternMode::Edit);
  if (block) {
    const int left = v.pattern.left_track();
    const int top = v.pattern.top_row();
    set_block(v, top, left, top + 15, left + v.pattern.visible_tracks() - 1);
  }
  scene(r, model, v);
}

void draw_pattern_zoom8(ui::Renderer& r, unsigned variant) { draw_pattern_zoom(r, 1, variant); }
void draw_pattern_zoom12(ui::Renderer& r, unsigned variant) { draw_pattern_zoom(r, 2, variant); }

namespace {

// OS-MIDIREC-P1 S4: pattern 1 of every track holds a lane that marks every step: a note of two steps with a CC on its first step and one on its second,
// so the even steps are "starts" with a CC and the odd ones "held" with a CC (the dearest marks: both bars on every step, `MID nn cc` and `MID`).
const model::LaneStore& lane_worst_store() {
  static std::unique_ptr<model::LaneStore> store;
  if (!store) {
    store = std::make_unique<model::LaneStore>();
    model::LaneStore::WriteWindow window(*store);
    for (unsigned t = 0; t < static_cast<unsigned>(app::kTracks); ++t) {
      model::LaneEvent ev[3 * (app::kSteps / 2)];
      unsigned n = 0;
      for (unsigned s = 0; s < static_cast<unsigned>(app::kSteps); s += 2) {
        ev[n++] = {static_cast<std::uint16_t>(s * 48), 96, 0x00, 0, 60, 100};  // note, 2 steps
        ev[n++] = {static_cast<std::uint16_t>(s * 48 + 8), 0, 0x10, 0, 1, 64};  // CC on the even step
        ev[n++] = {static_cast<std::uint16_t>((s + 1) * 48 + 8), 0, 0x10, 0, 1, 90};  // CC on the odd step
      }
      model::LaneEvent* gap = store->open_gap(1, t, 0, n);
      if (gap != nullptr) {
        std::memcpy(gap, ev, n * sizeof(model::LaneEvent));
      }
    }
  }
  return *store;
}

// What a take leaves: on every third track a CC sweep (one event every other step), on the next a note of three steps every four with a CC between, the third left empty.
const model::LaneStore& lane_typical_store() {
  static std::unique_ptr<model::LaneStore> store;
  if (!store) {
    store = std::make_unique<model::LaneStore>();
    model::LaneStore::WriteWindow window(*store);
    for (unsigned t = 0; t < static_cast<unsigned>(app::kTracks); ++t) {
      model::LaneEvent ev[app::kSteps];
      unsigned n = 0;
      for (unsigned s = 0; s < static_cast<unsigned>(app::kSteps) && t % 3 != 2; s += (t % 3 == 0 ? 2u : 4u)) {
        if (t % 3 == 1) {
          ev[n++] = {static_cast<std::uint16_t>(s * 48), 3 * 48, 0x00, 0, 60, 100};
          ev[n++] = {static_cast<std::uint16_t>((s + 3) * 48 + 12), 0, 0x10, 0, 74, 80};
        } else {
          ev[n++] = {static_cast<std::uint16_t>(s * 48 + 4), 0, 0x10, 0, 1, 64};
        }
      }
      model::LaneEvent* gap = n != 0 ? store->open_gap(1, t, 0, n) : nullptr;
      if (gap != nullptr) {
        std::memcpy(gap, ev, n * sizeof(model::LaneEvent));
      }
    }
  }
  return *store;
}

}  // namespace

// A document over the demo's content with typical lanes (see lane_typical_store).
void draw_pattern_lane_typical(ui::Renderer& r, unsigned) {
  static Model model;
  model = pattern_doc_model(0, false);
  model.lanes = &lane_typical_store();
  ViewState v;
  v.mode[app::screen_index(app::ScreenId::Pattern)] = static_cast<std::uint8_t>(PatternMode::Edit);
  scene(r, model, v);
}

// The worst lane-marked frame: the dearest content with a lane on every track, at the three zooms, the header marks and a block over the window or not, at three cursor windows.
void draw_pattern_lane_worst(ui::Renderer& r, unsigned variant) {
  const unsigned window = variant % 3;
  variant /= 3;
  const bool block = variant % 2 != 0;
  variant /= 2;
  const bool marks = variant % 2 != 0;
  variant /= 2;
  const unsigned set = variant % kDocSets;
  variant /= kDocSets;
  const int level = static_cast<int>(variant % 3);
  static Model model;
  model = pattern_doc_model(1 + set, marks);
  model.lanes = &lane_worst_store();
  const app::PatternView::Dims d{app::kSteps, app::kTracks};
  ViewState v;
  (void)v.pattern.set_zoom(level, d);
  (void)v.pattern.jump_row(window == 1 ? 40 : 5, d);
  (void)v.pattern.move_track(window == 0 ? 0 : (window == 1 ? 6 : 11), d);
  v.shift = marks;
  v.mode[app::screen_index(app::ScreenId::Pattern)] = static_cast<std::uint8_t>(PatternMode::Edit);
  if (block) {
    const int left = v.pattern.left_track();
    const int top = v.pattern.top_row();
    set_block(v, top, left, top + 15, left + v.pattern.visible_tracks() - 1);
  }
  scene(r, model, v);
}

// A document equal to the demo, Edit on, a block of 5 steps over 2 tracks, the second track muted and the third soloed, three edits undoable:
// what a person sees a minute into editing.
void draw_pattern_edit_on(ui::Renderer& r, unsigned) {
  Doc& d = doc(0);
  set_marks(d.arena->project, false);
  model::TrackSlot muted = *d.arena->project.track(1);
  muted.flags = model::kTrackMute;
  (void)d.arena->project.set_track(1, muted);
  model::TrackSlot solo = *d.arena->project.track(2);
  solo.flags = model::kTrackSolo;
  (void)d.arena->project.set_track(2, solo);
  ViewState v;
  v.mode[app::screen_index(app::ScreenId::Pattern)] = static_cast<std::uint8_t>(PatternMode::Edit);
  v.pattern.scroll_rows(6);
  for (int i = 0; i < 5; ++i) {
    v.pattern.move_right();
  }
  set_block(v, 4, 1, 8, 2);
  v.edit.selection.extending = false;
  d.model.undo_depth = 3;
  scene(r, d.model, v);
  d.model.undo_depth = 12;
}

// The block at its widest: every visible step of every visible track, the cursor at each corner of the window, in each column, over a few
// windows and the two contents.
void draw_pattern_selection_worst(ui::Renderer& r, unsigned variant) {
  const unsigned column = variant % 5;
  variant /= 5;
  const bool right = variant % 2 != 0;
  variant /= 2;
  const bool bottom = variant % 2 != 0;
  variant /= 2;
  const unsigned window = variant % 9;
  variant /= 9;
  const unsigned set = variant % kDocSets;
  static constexpr int kTops[3] = {0, 20, 48};
  static constexpr int kLefts[3] = {0, 3, 7};
  const int top = kTops[window / 3];
  const int left = kLefts[window % 3];
  ViewState v;
  v.mode[app::screen_index(app::ScreenId::Pattern)] = static_cast<std::uint8_t>(PatternMode::Edit);
  place(v, top, left, bottom, right, static_cast<int>(column));
  set_block(v, v.pattern.top_row(), v.pattern.left_track(), v.pattern.top_row() + 15, v.pattern.left_track() + 4);
  scene(r, pattern_doc_model(1 + set, false), v);
}

// Shift held on the pattern: the key row shows the Shift layer and the marks that are set are drawn bold; three tracks muted, one soloed.
void draw_pattern_shift_layer(ui::Renderer& r, unsigned) {
  Doc& d = doc(0);
  set_marks(d.arena->project, false);
  model::TrackSlot a = *d.arena->project.track(0);
  a.flags = model::kTrackMute | model::kTrackSolo;
  (void)d.arena->project.set_track(0, a);
  model::TrackSlot b = *d.arena->project.track(1);
  b.flags = model::kTrackMute;
  (void)d.arena->project.set_track(1, b);
  model::TrackSlot c = *d.arena->project.track(3);
  c.flags = model::kTrackSolo;
  (void)d.arena->project.set_track(3, c);
  ViewState v;
  v.mode[app::screen_index(app::ScreenId::Pattern)] = static_cast<std::uint8_t>(PatternMode::Edit);
  v.shift = true;
  v.pattern.scroll_rows(3);
  scene(r, d.model, v);
}

// Rec mode (OS-REVIEW, OS-RECMODE2): LIVE on the document, the sequencer playing: the red frame round the grid, the REC badge in the strip and the playhead's bar in the Rec red, the
// cursor on another row and a step with a note under the bar. Variants 1, 2 and 3 are the states that do NOT show it (STEP stopped, WATCH playing, and STEP playing), which the tests compare
// (pattern_view_test.cpp): no red frame. Variant 0 is LIVE (Shift + Rec, the red chip), 1 STEP stopped and 3 STEP playing (Rec, the amber chip), 2 WATCH (Browse, playing).
void draw_pattern_rec(ui::Renderer& r, unsigned variant) {
  Doc& d = doc(0);
  set_marks(d.arena->project, false);
  ViewState v;
  v.mode[app::screen_index(app::ScreenId::Pattern)] = static_cast<std::uint8_t>(variant % 3 == 2 ? PatternMode::Browse : PatternMode::Edit);
  v.edit.rec_free = variant == 1 || variant == 3;
  v.playing = variant % 3 != 1;
  v.play.playing = variant % 3 != 1;
  v.play.pattern = v.edit.pattern;
  v.play.step = 4;
  v.pattern.scroll_rows(7);
  scene(r, d.model, v);
}

// A ViewState of the Pattern screen at (row, track window) in Edit with the Shift layer on, and the window's own block when
// `block`: the base the overlay sweeps are drawn over (screens_app.cpp), with the document's header marks (pattern_doc_model(.., true)).
app::ViewState pattern_doc_worst_view(int row, int track_moves, bool block) {
  ViewState v;
  v.pattern.scroll_rows(row);
  for (int i = 0; i < track_moves; ++i) {
    v.pattern.move_right();
  }
  v.mode[app::screen_index(app::ScreenId::Pattern)] = static_cast<std::uint8_t>(PatternMode::Edit);
  v.shift = true;
  if (block) {
    const int left = v.pattern.left_track();
    const int top = v.pattern.top_row();
    set_block(v, top, left, top + 15, left + 4);
  }
  return v;
}

}  // namespace o3os::sim
