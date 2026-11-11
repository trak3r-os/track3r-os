// SPDX-License-Identifier: GPL-3.0-or-later
#include "screens_app.h"

#include <o3os/app/events_view.h>
#include <o3os/app/input_test.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/song_data.h>
#include <o3os/app/song_tools_view.h>
#include <o3os/app/view_state.h>
#include <o3os/model/event_list.h>
#include <o3os/model/project.h>
#include <o3os/sim/demo_project.h>

#include <cstring>
#include <memory>

namespace o3os::sim {

namespace {

using app::InputTestState;
using app::Model;
using app::OverlayId;
using app::ScreenId;
using app::ViewState;

constexpr unsigned kPageList = 0;  // OS-SONG3 S1: the Song screen has two pages, LIST (ORDER and ARRANGEMENT folded) and TIMELINE
constexpr unsigned kPageTimeline = 1;

// ---- content ---------------------------------------------------------------------------------------------------

const Model& demo_model() {
  static const Model model = [] {
    Model m{};
    app::fill_demo(m.pattern);
    app::fill_song_demo(m.song);
    return m;
  }();
  return model;
}

// The dearest song: 64 positions, every pattern name 12 characters without a space (a space costs no word) and every length
// three digits, the play marker wherever the variant puts it.
const app::SongData& worst_song(int play) {
  static app::SongData data = [] {
    app::SongData d{};
    app::fill_song_demo(d);
    for (int i = 0; i < app::kSongPatterns; ++i) {
      for (int n = 0; n < app::kSongNameChars; ++n) {
        d.patterns[i].name[n] = static_cast<char>('A' + (i + n) % 26);
      }
      d.patterns[i].name[app::kSongNameChars] = '\0';
      d.patterns[i].steps = static_cast<std::uint8_t>(112 + i);
    }
    for (int i = 0; i < app::kSongMaxLength; ++i) {
      d.order[i] = static_cast<std::uint8_t>(1 + i % app::kSongPatterns);
    }
    d.length = app::kSongMaxLength;
    return d;
  }();
  data.play = play;  // the harness is not re-entrant, like the budget harness itself
  return data;
}

// A model with the pattern's worst data set and the worst song.
const Model& worst_model(unsigned pattern_set, int play) {
  static Model model{};
  model.pattern = pattern_worst_data(pattern_set);
  model.song = worst_song(play);
  return model;
}

void scene(ui::Renderer& r, const Model& model, const ViewState& view) { app::render_scene(model, view, r); }

// ---- the Song screen ---------------------------------------------------------------------------------------------

ViewState song_view(unsigned page, int pos, const Model& model) {
  ViewState v;
  v.screen = ScreenId::Song;
  v.page[app::screen_index(ScreenId::Song)] = static_cast<std::uint8_t>(page);
  (void)v.song.set(pos, model.song.length);
  return v;
}

void draw_song_typical(ui::Renderer& r, unsigned) { scene(r, demo_model(), song_view(kPageList, 0, demo_model())); }

// Both pages, a cursor at the top, in the middle, past the first window and at the far end; the play marker on the cursor's
// row, on another visible row and out of the window.
constexpr int kSongPositions[] = {0, 5, 15, 16, 30, 63};
constexpr unsigned kSongPositionCount = sizeof(kSongPositions) / sizeof(kSongPositions[0]);
constexpr unsigned kSongWorstVariants = 2 * kSongPositionCount * 3;

void song_worst_variant(unsigned variant, unsigned& page, int& pos, int& play) {
  page = variant % 2;
  variant /= 2;
  pos = kSongPositions[variant % kSongPositionCount];
  variant /= kSongPositionCount;
  play = variant == 0 ? pos : (variant == 1 ? (pos + 1) % app::kSongMaxLength : 0);
}

void draw_song_worst(ui::Renderer& r, unsigned variant) {
  unsigned page;
  int pos, play;
  song_worst_variant(variant, page, pos, play);
  const Model& model = worst_model(0, play);
  scene(r, model, song_view(page, pos, model));
}

// The Song screen over a document (OS-SONG1): the track map of each row. A song of 40 slots over 8 patterns whose tracks hold notes, only FX or nothing (typical: 12 active tracks), and
// the dearest case: 16 active tracks, every cell filled. The harness is not re-entrant: the two documents are built once.
struct SongDoc {
  std::unique_ptr<model::ModelArena> arena;
  Model model;
};

SongDoc& song_doc(bool worst) {
  static SongDoc docs[2];
  SongDoc& d = docs[worst ? 1 : 0];
  if (!d.arena) {
    d.arena = std::make_unique<model::ModelArena>();
    std::memset(d.arena.get(), 0, sizeof(model::ModelArena));
    model::Project& p = d.arena->project;
    p.reset();
    (void)p.set_active_tracks(worst ? 16u : 12u);
    model::Step note = model::kEmptyStep;
    note.note = 61;
    note.instr = 1;
    model::Step fx = model::kEmptyStep;
    fx.fx[0] = {12, 5};
    for (unsigned id = 1; id <= 8; ++id) {
      (void)p.create_pattern(id, id % 3 == 0 ? 64u : 32u);
      (void)p.set_pattern_name(id, "PATTERN NAME", 12);
      for (unsigned t = 0; t < model::kMaxTracks; ++t) {
        // typical: a pattern uses the tracks of a few bits of its number; every fourth track of every pattern holds only FX
        const bool used = worst || ((id * 5u + t * 3u) % 7u) < 4u;
        if (used) {
          (void)p.set_step(id, t, t % 4u, (!worst && t % 4u == 3u) ? fx : note);
        }
      }
    }
    for (unsigned i = 0; i < 40; ++i) {
      (void)p.append_order(1 + i % 8);
    }
    d.model.project = &p;
    d.model.undo_depth = 4;
  }
  return d;
}

ViewState song_doc_view(unsigned page, int pos) {
  ViewState v;
  v.screen = ScreenId::Song;
  v.page[app::screen_index(ScreenId::Song)] = static_cast<std::uint8_t>(page);
  (void)v.song.set(pos, 40);
  return v;
}

// The jump waiting on slot 7 while the song plays slot 3, in the lit half of its blink.
void song_doc_queued(ViewState& v) {
  v.playing = true;
  v.play.playing = true;
  v.play.order = 3;
  v.play.pattern = 4;
  v.play.queued = 7;
  v.blink = true;
}

void draw_song_document(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc(false);
  ViewState v = song_doc_view(kPageList, 5);
  song_doc_queued(v);
  scene(r, d.model, v);
}

// The dearest: both pages, every cell filled and the queued slot lit, over the dearest song positions.
void draw_song_document_worst(ui::Renderer& r, unsigned variant) {
  SongDoc& d = song_doc(true);
  ViewState v = song_doc_view(variant % 2, variant / 2 % 2 == 0 ? 0 : 25);
  song_doc_queued(v);
  v.shift = variant / 4 != 0;
  scene(r, d.model, v);
}

// OS-SONGMAP: the cursor in the track map, over the same documents. `slots` x `tracks` cells are selected, the cursor on the last of them (a plain move into the map is Right: track 1, so the range runs from track 2).
ViewState song_doc_map_view(unsigned page, int pos, int cells, int slots, int tracks) {
  ViewState v = song_doc_view(page, pos);
  (void)v.song.step_col(1, cells);
  (void)v.song.step_col(1, cells);
  for (int i = 1; i < tracks; ++i) {
    (void)v.song.extend_tracks(1, cells);
  }
  for (int i = 1; i < slots; ++i) {
    (void)v.song.extend(1, 40);
  }
  return v;
}

// A rectangle of 4 slots x 3 tracks over the typical document, the jump waiting on slot 11 (outside the rectangle: a flashing slot on a selected row is Play on Sel, which the contrast table has no pair for).
void draw_song_document_map(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc(false);
  ViewState v = song_doc_map_view(kPageList, 5, 12, 4, 3);
  song_doc_queued(v);
  v.play.queued = 11;
  scene(r, d.model, v);
}

// The dearest: 16 tracks, every cell filled, the rectangle over all 16 slots of the window and every track, the queued slot lit, both pages, over the dearest song positions, with and without the Shift layer.
void draw_song_document_map_worst(ui::Renderer& r, unsigned variant) {
  SongDoc& d = song_doc(true);
  ViewState v = song_doc_map_view(variant % 2, variant / 2 % 2 == 0 ? 0 : 25, 16, 16, 15);
  song_doc_queued(v);
  v.shift = variant / 4 != 0;
  scene(r, d.model, v);
}

// OS-BUG-SONGMAPCUR: songs whose last tracks hold nothing in any pattern. 0: 12 active tracks, the first 8 used; 1: 16 active tracks, the first 10 used; 2 (the dearest: every neighbour a different cell):
// 16 active tracks, a note, FX only or nothing by (pattern + track) % 3. The cursor can reach every active track, so the map draws a cell for each (a faint tick where the track is empty).
SongDoc& song_doc_sparse(unsigned kind) {
  static SongDoc docs[3];
  SongDoc& d = docs[kind];
  if (!d.arena) {
    d.arena = std::make_unique<model::ModelArena>();
    std::memset(d.arena.get(), 0, sizeof(model::ModelArena));
    model::Project& p = d.arena->project;
    p.reset();
    (void)p.set_active_tracks(kind == 0 ? 12u : 16u);
    model::Step note = model::kEmptyStep;
    note.note = 61;
    note.instr = 1;
    model::Step fx = model::kEmptyStep;
    fx.fx[0] = {12, 5};
    for (unsigned id = 1; id <= 8; ++id) {
      (void)p.create_pattern(id, id % 3 == 0 ? 64u : 32u);
      (void)p.set_pattern_name(id, "PATTERN NAME", 12);
      for (unsigned t = 0; t < model::kMaxTracks; ++t) {
        if (kind == 2) {
          if ((id + t) % 3u != 2u) {
            (void)p.set_step(id, t, 0, (id + t) % 3u == 0u ? note : fx);
          }
        } else if (t < (kind == 0 ? 8u : 10u)) {
          (void)p.set_step(id, t, t % 4u, note);
        }
      }
    }
    for (unsigned i = 0; i < 40; ++i) {
      (void)p.append_order(1 + i % 8);
    }
    d.model.project = &p;
    d.model.undo_depth = 4;
  }
  return d;
}

// The cursor on `track` (1-based) of the map: Right enters at track 1, one press more for each track after it.
ViewState song_doc_map_at(unsigned page, int pos, int cells, int track) {
  ViewState v = song_doc_view(page, pos);
  for (int i = 0; i < track; ++i) {
    (void)v.song.step_col(1, cells);
  }
  return v;
}

// The last track of a 12-track song that uses 8, and of a 16-track song that uses 10 (where the 12-lane window has scrolled to tracks 5-16).
void draw_song_document_map_empty(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc_sparse(0);
  scene(r, d.model, song_doc_map_at(kPageList, 5, 12, 12));
}

void draw_song_document_map_16(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc_sparse(1);
  scene(r, d.model, song_doc_map_at(kPageList, 5, 16, 16));
}

// The dearest of the ticks: 16 tracks of notes, FX only and empty cells side by side (a colour change at every cell), the cursor on the last track, both pages, over the dearest song positions, with and without the Shift layer.
void draw_song_document_map_churn(ui::Renderer& r, unsigned variant) {
  SongDoc& d = song_doc_sparse(2);
  ViewState v = song_doc_map_at(variant % 2, variant / 2 % 2 == 0 ? 0 : 25, 16, 16);
  song_doc_queued(v);
  v.shift = variant / 4 != 0;
  scene(r, d.model, v);
}

// The question of 1.6 up: its text on the strip and the key row marking F1 ALL and F2 UNIQUE.
void draw_song_document_ask(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc(false);
  ViewState v = song_doc_map_view(kPageList, 5, 12, 1, 1);
  v.song.ask.live = true;
  v.song.ask.shared = 1;
  v.song.ask.first = 2;
  v.song.ask.others = 4;
  app::AskText head;
  app::AskText text;
  app::song_ask_head(v.song.ask, head);
  app::ask_text(app::Ask::Shared, head.c_str(), text);
  (void)v.notify.post(core::Severity::Warn, text.c_str(), "song", 4000);
  scene(r, d.model, v);
}

// The transport playing slot `order` (pattern `pattern`) at `step`, for the pages that show it.
void song_play_at(ViewState& v, unsigned order, unsigned pattern, unsigned step) {
  v.playing = true;
  v.play.playing = true;
  v.play.order = static_cast<std::uint16_t>(order);
  v.play.pattern = static_cast<std::uint8_t>(pattern);
  v.play.step = static_cast<std::uint16_t>(step);
  v.play.ustep = 24;
}

// OS-SONG-TIMELINE: the TIMELINE page. The typical one is the 12-track document with the cursor on slot 12, slot 14 playing at step 20. The dearest is the slot cap (timeline_slot_cap): every
// pattern one bar (16 steps) long with notes in every track, as many slots as the budget allows for the track count, with and without the playhead and the cursor at the start and in the middle.
SongDoc& song_doc_short() {
  static SongDoc d;
  if (!d.arena) {
    d.arena = std::make_unique<model::ModelArena>();
    std::memset(d.arena.get(), 0, sizeof(model::ModelArena));
    model::Project& p = d.arena->project;
    p.reset();
    (void)p.set_active_tracks(16u);
    model::Step note = model::kEmptyStep;
    note.note = 61;
    note.instr = 1;
    for (unsigned id = 1; id <= 8; ++id) {
      (void)p.create_pattern(id, 16u);
      for (unsigned t = 0; t < model::kMaxTracks; ++t) {
        for (unsigned s = 0; s <= (t + id) % 8u; ++s) {
          (void)p.set_step(id, t, s, note);
        }
      }
    }
    for (unsigned i = 0; i < 40; ++i) {
      (void)p.append_order(1 + i % 8);
    }
    d.model.project = &p;
    d.model.undo_depth = 4;
  }
  return d;
}

void draw_song_timeline_document(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc(false);
  ViewState v = song_doc_view(kPageTimeline, 12);
  song_play_at(v, 14, 7, 20);
  scene(r, d.model, v);
}

// The sweep covers every count of active tracks (1 to 16: the slots in view are min(40, budget / (tracks + overhead)), so the dearest corner moves with the count), the cursor at the start
// and in the middle of the 40 one-bar slots, and the playhead off and on. Single-digit ids cycling 1..8 change the colour at every slot and fit their number on every block.
void draw_song_timeline_worst(ui::Renderer& r, unsigned variant) {
  SongDoc& d = song_doc_short();
  (void)d.arena->project.set_active_tracks(variant / 4 + 1u);
  ViewState v = song_doc_view(kPageTimeline, variant % 2 == 0 ? 0 : 20);
  if (variant / 2 % 2 == 1) {
    song_play_at(v, variant % 2 == 0 ? 3 : 22, 2, 9);
  }
  scene(r, d.model, v);
}

// OS-SONG3 S2: the editable TIMELINE. The typical one is the 12-track document with the cursor in lane 2 of slot 12 and a rectangle of 4 slots x 3 tracks selected (the slots run to the right of the cursor, the
// tracks from lane 2). The dearest is the sweep of the timeline's, with the rectangle over every slot of the window and every lane (the second AccentText pass of the note map, the band, the cursor's cell) and the cursor's
// cell: both ends of the song, the playhead on and off.
void draw_song_timeline_edit(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc(false);
  ViewState v = song_doc_map_view(kPageTimeline, 12, 12, 4, 3);
  scene(r, d.model, v);
}

void draw_song_timeline_edit_worst(ui::Renderer& r, unsigned variant) {
  SongDoc& d = song_doc_short();
  const unsigned tracks = variant / 4 + 1u;
  (void)d.arena->project.set_active_tracks(tracks);
  const int cells = tracks < 12u ? static_cast<int>(tracks) : 12;
  ViewState v = song_doc_map_view(kPageTimeline, variant % 2 == 0 ? 0 : 20, cells, 1, cells);
  v.song.select(39);  // the range from the cursor to the last slot: every slot of the window is in it
  if (variant / 2 % 2 == 1) {
    song_play_at(v, variant % 2 == 0 ? 3 : 22, 2, 9);
  }
  scene(r, d.model, v);
}

// OS-SONG3 S3 (review fold): the timeline_worst sweep with alternating tracks muted, the worst count of unmuted runs (6 of 12 lanes), so the note map's Text pass is sectioned the most. The full-window selection
// stays in timeline_edit_worst: with it the dim sweep would need a eighth map section and not fit the standard. The mute flags are cleared again after the frame: song_doc_short() is shared with the sweeps above.
// The top bit of the variant (variant / 8) is the shared mark's own worst case (OS-SONG3 S3): the song truncated to 32 slots, so the blocks are wide enough for the number and the `*` together, and every slot on
// pattern 1, so every block of the window wears the mark. Both are restored after the frame, like the flags.
void draw_song_timeline_edit_worst_dim(ui::Renderer& r, unsigned variant) {
  SongDoc& d = song_doc_short();
  const unsigned sweep = variant % 64u;             // bits 0..5: the sweep of timeline_worst (bit 0 the cursor's end, bit 1 the playhead, bits 2..5 the track count 1..16)
  const bool marks_worst = variant / 64u != 0u;      // bit 6: the shared mark's worst case on top
  const unsigned tracks = sweep / 4 + 1u;
  (void)d.arena->project.set_active_tracks(tracks);
  ViewState v = song_doc_view(kPageTimeline, sweep % 2 == 0 ? 0 : 20);
  if (sweep / 2 % 2 == 1) {
    song_play_at(v, sweep % 2 == 0 ? 3 : 22, 2, 9);
  }
  model::Project& p = d.arena->project;
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    const model::TrackSlot* ts = p.track(t);
    if (ts != nullptr) {
      model::TrackSlot copy = *ts;
      copy.flags = t % 2u == 0u ? model::kTrackMute : 0u;
      (void)p.set_track(t, copy);
    }
  }
  std::uint8_t saved_order[40];
  if (marks_worst) {
    for (unsigned i = 0; i < 40u; ++i) {
      saved_order[i] = static_cast<std::uint8_t>(p.order(i));
      (void)p.set_order(i, 1u);
    }
    for (unsigned i = 39u; i >= 32u; --i) {
      (void)p.remove_order(i);
    }
  }
  scene(r, d.model, v);
  if (marks_worst) {
    for (unsigned i = 32u; i < 40u; ++i) {
      (void)p.insert_order(i, saved_order[i]);
    }
    for (unsigned i = 0; i < 40u; ++i) {
      (void)p.set_order(i, saved_order[i]);  // all 40, not only the re-inserted tail: the sweep shares song_doc_short() with timeline_worst and timeline_edit_worst, whose frames would otherwise lose their colour changes
    }
  }
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    const model::TrackSlot* ts = p.track(t);
    if (ts != nullptr && ts->flags != 0u) {
      model::TrackSlot copy = *ts;
      copy.flags = 0u;
      (void)p.set_track(t, copy);
    }
  }
}

// OS-SONG3 S3 (review fold, the combined sweep): everything dear at once — the full-window selection (the rectangle over every slot of the window and every lane: the band, the cursor's cell and the second
// AccentText pass of the note map), alternating muted tracks (the most unmuted runs) and the shared mark on every block (32 slots on one pattern). 128 variants as the dim sweep's. It is not a worst-case state:
// it is held to the chip's 2048-word list only, not the 1792 standard (the combination may cost more; the budget table reports its words and the review says so rather than trim a feature).
void draw_song_timeline_edit_worst_all(ui::Renderer& r, unsigned variant) {
  SongDoc& d = song_doc_short();
  const unsigned sweep = variant % 64u;
  const bool marks_worst = variant / 64u != 0u;
  const unsigned tracks = sweep / 4 + 1u;
  (void)d.arena->project.set_active_tracks(tracks);
  const int cells = tracks < 12u ? static_cast<int>(tracks) : 12;
  ViewState v = song_doc_map_view(kPageTimeline, sweep % 2 == 0 ? 0 : 20, cells, 1, cells);
  v.song.select(39);  // the range from the cursor to the last slot: every slot of the window is in it
  if (sweep / 2 % 2 == 1) {
    song_play_at(v, sweep % 2 == 0 ? 3 : 22, 2, 9);
  }
  model::Project& p = d.arena->project;
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    const model::TrackSlot* ts = p.track(t);
    if (ts != nullptr) {
      model::TrackSlot copy = *ts;
      copy.flags = t % 2u == 0u ? model::kTrackMute : 0u;
      (void)p.set_track(t, copy);
    }
  }
  std::uint8_t saved_order[40];
  if (marks_worst) {
    for (unsigned i = 0; i < 40u; ++i) {
      saved_order[i] = static_cast<std::uint8_t>(p.order(i));
      (void)p.set_order(i, 1u);
    }
    for (unsigned i = 39u; i >= 32u; --i) {
      (void)p.remove_order(i);
    }
  }
  scene(r, d.model, v);
  if (marks_worst) {
    for (unsigned i = 32u; i < 40u; ++i) {
      (void)p.insert_order(i, saved_order[i]);
    }
    for (unsigned i = 0; i < 40u; ++i) {
      (void)p.set_order(i, saved_order[i]);
    }
  }
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    const model::TrackSlot* ts = p.track(t);
    if (ts != nullptr && ts->flags != 0u) {
      model::TrackSlot copy = *ts;
      copy.flags = 0u;
      (void)p.set_track(t, copy);
    }
  }
}

// ---- the overlays ---------------------------------------------------------------------------------------------------

ViewState pattern_view(int row, int moves) {
  ViewState v;
  v.pattern.scroll_rows(row);
  for (int i = 0; i < moves; ++i) {
    v.pattern.move_right();
  }
  return v;
}

// The cursor is put where the overlay's rectangle leaves it visible (an upper row), so a picture shows the cursor and the
// selected row behind the dialog.
void draw_learn(ui::Renderer& r, unsigned) {
  ViewState v = pattern_view(3, 6);
  v.overlay = OverlayId::Learn;
  scene(r, demo_model(), v);
}

void draw_navigator(ui::Renderer& r, unsigned) {
  ViewState v = pattern_view(2, 11);
  v.overlay = OverlayId::Navigator;
  v.navigator = app::screen_position(app::screen_registry(), ScreenId::Song);
  scene(r, demo_model(), v);
}

void notice(ViewState& v, core::Severity severity, const char* text, const char* source) {
  (void)v.notify.post(severity, text, source, 2000);
}

void draw_pattern_notify(ui::Renderer& r, unsigned) {
  ViewState v = pattern_view(5, 1);
  notice(v, core::Severity::Info, "F3 JUMP", "ui");
  scene(r, demo_model(), v);
}

void draw_song_notify(ui::Renderer& r, unsigned) {
  ViewState v = song_view(kPageList, 3, demo_model());
  notice(v, core::Severity::Info, "F1 PLAY PAT", "ui");
  scene(r, demo_model(), v);
}

// The sweeps put each overlay over the dearest base the pattern screen has (every field filled, and the sparse mix of its
// own worst case), at windows spread over the pattern, with the overlay's own variations.
constexpr int kWorstTracks[] = {0, 3, 6, 11};
constexpr int kWorstRows[] = {0, 5, 15, 40, 63};
constexpr unsigned kWorstTrackCount = sizeof(kWorstTracks) / sizeof(kWorstTracks[0]);
constexpr unsigned kWorstRowCount = sizeof(kWorstRows) / sizeof(kWorstRows[0]);
constexpr unsigned kWindows = 2 * kPatternWorstDataSets * kWorstTrackCount * kWorstRowCount;  // demo or document first

struct Window {
  unsigned set;
  int track;
  int row;
  bool doc;  // the base is a document with the header marks, the Shift layer and a block (OS-009), not the demo
};

// The base window of variant `v` (consumed from it): demo or document, the data set, the first column of a track, a row.
Window window_of(unsigned& v) {
  Window w{};
  w.doc = v % 2 != 0;
  v /= 2;
  w.set = v % kPatternWorstDataSets;
  v /= kPatternWorstDataSets;
  w.track = kWorstTracks[v % kWorstTrackCount];
  v /= kWorstTrackCount;
  w.row = kWorstRows[v % kWorstRowCount];
  v /= kWorstRowCount;
  return w;
}

const Model& window_model(const Window& w) {
  if (w.doc) {
    return pattern_doc_model(1 + w.set, true);
  }
  static Model model{};
  model.pattern = pattern_worst_data(w.set);
  app::fill_song_demo(model.song);
  return model;
}

// The view over that base: the cursor's window, and for a document the Shift layer and the window's block.
ViewState window_view(const Window& w) {
  return w.doc ? pattern_doc_worst_view(w.row, w.track * app::kColumns, true) : pattern_view(w.row, w.track * app::kColumns);
}

constexpr unsigned kNavigatorCells = 11;
constexpr unsigned kNavigatorVariants = kWindows * kNavigatorCells;
constexpr unsigned kSeverities = 3;
constexpr unsigned kNotifyVariants = kWindows * kSeverities;

void draw_learn_worst(ui::Renderer& r, unsigned variant) {
  const Window w = window_of(variant);
  ViewState v = window_view(w);
  v.overlay = OverlayId::Learn;
  scene(r, window_model(w), v);
}

void draw_navigator_worst(ui::Renderer& r, unsigned variant) {
  const unsigned cell = variant % kNavigatorCells;
  variant /= kNavigatorCells;
  const Window w = window_of(variant);
  ViewState v = window_view(w);
  v.overlay = OverlayId::Navigator;
  v.navigator = static_cast<int>(cell);
  scene(r, window_model(w), v);
}

// 48 characters without a space (a space costs no word), and a source of 8: the longest line the strip can carry.
// Posted 99 times: coalesced into one entry with the largest count, so the chip carries " x99" too.
void worst_notice(ViewState& v, unsigned severity) {
  for (unsigned i = 0; i < core::kNotifyMaxCount; ++i) {
    notice(v, static_cast<core::Severity>(severity), "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJKL", "ABCDEFGH");
  }
}

void draw_pattern_notify_worst(ui::Renderer& r, unsigned variant) {
  const unsigned severity = variant % kSeverities;
  variant /= kSeverities;
  const Window w = window_of(variant);
  ViewState v = window_view(w);
  worst_notice(v, severity);
  scene(r, window_model(w), v);
}

constexpr unsigned kSongNotifyVariants = 2 * kSongPositionCount * kSeverities;

void draw_song_notify_worst(ui::Renderer& r, unsigned variant) {
  const unsigned severity = variant % kSeverities;
  variant /= kSeverities;
  unsigned page;
  int pos, play;
  song_worst_variant(variant, page, pos, play);
  const Model& model = worst_model(0, play);
  ViewState v = song_view(page, pos, model);
  worst_notice(v, severity);
  scene(r, model, v);
}

// OS-SONGMAP: the Song screen's TOOLS dialog over the document with the cursor in the map and a rectangle selected. The sweep takes every tool and both rows, the extreme values (the widest numbers and the longest
// scope line) and both pages.
void song_tools_state(ViewState& v, unsigned tool, unsigned row, bool extreme) {
  v.overlay = OverlayId::SongTools;
  v.tools.tool = static_cast<app::SongTool>(tool % 6);
  v.tools.row = static_cast<std::uint8_t>(row % app::song_tools_rows(v.tools));
  v.tools.form = extreme ? app::SongForm::LoopSong : app::SongForm::Pop;  // (LOOP>SONG is the longest name)
  v.tools.drums = extreme ? 8 : 4;
  // FORM's footer is the plan (what the planner says for the demo's patterns is another test's): the typical one is the spec's example, the extreme one the longest line, three 3-digit ids.
  v.tools.plan = app::SongBuildInfo{};
  v.tools.plan.slots = extreme ? 10 : 8;
  v.tools.plan.copies = extreme ? 4 : 3;
  v.tools.plan.source_count = 3;
  v.tools.plan.sources[0] = extreme ? 255 : 2;
  v.tools.plan.sources[1] = extreme ? 254 : 5;
  v.tools.plan.sources[2] = extreme ? 253 : 7;
  if (v.tools.tool == app::SongTool::Bridge) {  // OS-SONG-BRIDGE: `PATTERN 02 TO 05  4 SLOTS  4 NEW PATTERNS`; the extreme one is the longest line (16 slots, two 3-digit ids)
    v.tools.bridge_slots = extreme ? 16 : 4;
    v.tools.plan.slots = v.tools.bridge_slots;
    v.tools.plan.copies = v.tools.bridge_slots;
    v.tools.plan.source_count = 2;
  }
  v.tools.amount = extreme ? -24 : 7;
  v.tools.instrument = extreme ? 64 : 5;
  v.tools.what = extreme ? app::SongWhat::Volume : app::SongWhat::All;
  v.tools.rect = extreme ? app::SongRect{0, 39, 0, 15} : app::SongRect{5, 8, 1, 3};
  v.tools.patterns = extreme ? 8 : 3;
  v.tools.whole = extreme;  // (EXPORT WAV with every track: the scope line says ALL TRACKS)
}

void draw_song_tools(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc(false);
  ViewState v = song_doc_map_view(kPageList, 5, 12, 4, 3);
  song_tools_state(v, 0, 0, false);
  scene(r, d.model, v);
}

// OS-SONG-BUILD: the dialog on FORM, its three rows and the plan in the footer.
void draw_song_tools_form(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc(false);
  ViewState v = song_doc_map_view(kPageList, 5, 12, 4, 3);
  song_tools_state(v, static_cast<unsigned>(app::SongTool::Form), 1, false);
  scene(r, d.model, v);
}

// OS-SONG-BRIDGE: the dialog on BRIDGE, its SLOTS row and the plan in the footer.
void draw_song_tools_bridge(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc(false);
  ViewState v = song_doc_map_view(kPageList, 5, 12, 4, 3);
  song_tools_state(v, static_cast<unsigned>(app::SongTool::Bridge), 1, false);
  scene(r, d.model, v);
}

constexpr unsigned kSongToolsVariants = 6 * 3 * 2 * 2;

void draw_song_tools_worst(ui::Renderer& r, unsigned variant) {
  SongDoc& d = song_doc(true);
  const unsigned tool = variant % 6;
  variant /= 6;
  const unsigned row = variant % 3;
  variant /= 3;
  const bool extreme = variant % 2 != 0;
  variant /= 2;
  ViewState v = song_doc_map_view(variant % 2, 25, 16, 16, 15);
  song_doc_queued(v);
  song_tools_state(v, tool, row, extreme);
  scene(r, d.model, v);
}

// ---- the EVENTS overlay (OS-MIDIREC S1b/S1c) ---------------------------------------------------------------------------
//
// The document of `document events` (demo_project.cpp), with the overlay opened the way events_open opens it: the scope from the
// view's selection and cursor, the cursor on the first row at or after the grid cursor's step. The typical state is the cursor's
// track; the sweep takes every window of the dearest base under it, the cursor on every field of the row (the boxed field), the
// block scopes (the TRK column), the three filters (S1c) and the play marker (3.7, S1c), and the empty list.

struct EventsDoc {
  std::unique_ptr<model::ModelArena> arena;
  std::unique_ptr<model::LaneStore> lanes;  // S6: the take lane of track 5 (fill_events_lanes); the lane rows of every picture
  Model model;
};

EventsDoc& events_doc() {
  static EventsDoc d;
  if (!d.arena) {
    d.arena = std::make_unique<model::ModelArena>();
    std::memset(d.arena.get(), 0, sizeof(model::ModelArena));
    fill_events_project(d.arena->project);
    d.lanes = std::make_unique<model::LaneStore>();
    d.lanes->sync(d.arena->project.load_epoch());
    fill_events_lanes(*d.lanes);
    d.model.project = &d.arena->project;
    d.model.lanes = d.lanes.get();
    d.model.undo_depth = 12;
  }
  return d;
}

// The view of the list open on `v` (the base view's cursor, window and selection): the scope, the cursor's start and the field.
void events_view_state(ViewState& v, unsigned pattern, unsigned field, model::events::Filter filter) {
  EventsDoc& d = events_doc();
  const model::Pattern* pat = d.arena->project.pattern(pattern);
  v.screen = ScreenId::Pattern;
  v.edit.pattern = static_cast<std::uint8_t>(pattern);
  v.overlay = OverlayId::Events;
  if (pat == nullptr) {
    return;
  }
  namespace ev = model::events;
  v.events.scope = app::events_scope_of(*pat, v.edit, v.pattern.row(), v.pattern.track());
  v.events.filter = filter;
  v.events.field = static_cast<std::uint8_t>(field % 8u);
  ev::Env env = ev::env_of(d.arena->project);
  env.lanes = d.lanes.get();  // S6: the lane rows count in the list the cursor starts in
  v.events.cursor = app::events_start_cursor(*pat, env, v.events.scope, v.events.filter, v.pattern.row());
  v.events.top = v.events.cursor >= app::kEventsRows ? v.events.cursor - app::kEventsRows + 1u : 0u;
}

void draw_events(ui::Renderer& r, unsigned) {
  EventsDoc& d = events_doc();
  ViewState v = pattern_view(0, 0);  // the cursor on track 1, step 1
  events_view_state(v, 1, 0, model::events::Filter::All);
  scene(r, d.model, v);
}

// OS-LEARNMAP: the LEARN MAP overlay. The project of the typical state has the demo's two bindings; the dearest one has all 32 slots in use
// (the longest parameter names, every mode, a follow binding, a hit dot on each) and the cursor on every page of the window.
struct LearnMapDoc {
  std::unique_ptr<model::ModelArena> arena;
  Model model;
};

LearnMapDoc& learn_map_doc(bool full) {
  static LearnMapDoc typical;
  static LearnMapDoc worst;
  LearnMapDoc& d = full ? worst : typical;
  if (!d.arena) {
    d.arena = std::make_unique<model::ModelArena>();
    std::memset(d.arena.get(), 0, sizeof(model::ModelArena));
    if (full) {
      fill_demo_project(d.arena->project);
      for (unsigned i = 0; i < core::kLearnSlots; ++i) {
        const unsigned field = model::instrument_param::Cutoff + (i % 3u);
        core::LearnBinding b{core::param_id(core::ParamKind::Instrument, i % 12u, field), static_cast<std::uint8_t>(100 + i), static_cast<std::uint8_t>(i % 17u),
                             static_cast<std::uint8_t>(i % 3u), static_cast<std::uint8_t>(i % 5u == 0 ? core::kLearnFollow : 0u)};
        (void)d.arena->project.set_learn_slot(i, b);
      }
    } else {
      fill_demo_project_with_bindings(d.arena->project);
    }
    d.model.project = &d.arena->project;
    d.model.undo_depth = 12;
  }
  return d;
}

void learn_map_view(ViewState& v, unsigned cursor, unsigned column) {
  v.overlay = OverlayId::LearnMap;
  v.learn_map.cursor = static_cast<std::uint8_t>(cursor);
  v.learn_map.column = static_cast<std::uint8_t>(column);
}

void draw_learn_map(ui::Renderer& r, unsigned variant) {
  LearnMapDoc& d = learn_map_doc(false);
  ViewState v = pattern_view(3, 6);
  learn_map_view(v, 2, 2);
  if (variant == 1) {  // a CC moved the second binding: its dot is lit; waiting for a new one
    v.learn_map.hit[0] = 2u;
    v.learn_map.wait = app::LearnMapView::kWaitNew;
  }
  scene(r, d.model, v);
}

constexpr unsigned kLearnMapWorstVariants = 4 * 5 * 3;  // four cursor places, five columns, the head's three states

void draw_learn_map_worst(ui::Renderer& r, unsigned variant) {
  LearnMapDoc& d = learn_map_doc(true);
  static constexpr unsigned kCursors[4] = {0, 1, 16, 32};
  const unsigned head = variant % 3u;
  variant /= 3u;
  const unsigned column = variant % 5u;
  variant /= 5u;
  ViewState v = pattern_view(3, 6);
  learn_map_view(v, kCursors[variant % 4u], column);
  v.learn_map.hit[0] = 0xFFFFFFFFu;  // every dot lit
  v.enc_target = core::param_id(core::ParamKind::Instrument, 11, model::instrument_param::Cutoff);
  if (head == 1) {
    v.learn_map.wait = app::LearnMapView::kWaitRelearn;
  } else if (head == 2) {
    v.learn.confirm = true;
  }
  scene(r, d.model, v);
}

constexpr unsigned kEventsFieldVariants = 8;    // the cursor on every field (the valid ones of the row are boxed)
constexpr unsigned kEventsFilterVariants = 3;    // S1c: ALL, NOTES, CC (the header count and the F1 label follow)
constexpr unsigned kEventsPlayVariants = 2;      // S1c: the play marker off / on (3.7)
constexpr unsigned kEventsBaseVariants = kWindows * kEventsFieldVariants * kEventsFilterVariants * kEventsPlayVariants;
constexpr unsigned kEventsWorstVariants = 2 * kEventsBaseVariants;  // S6: the second half is the lane-heavy window (track 5, the take lane)

// The dearest sweep: every window of the dearest base (the document with the block gives the TRK column), the cursor on every
// field, every filter, the play marker on and off, and the last window's variants on the empty pattern (the empty list).
void draw_events_worst(ui::Renderer& r, unsigned variant) {
  EventsDoc& d = events_doc();
  const bool lane_window = variant >= kEventsBaseVariants;  // S6: the cursor on track 5, whose lane the list merges with its step note
  variant %= kEventsBaseVariants;
  const unsigned field = variant % kEventsFieldVariants;
  variant /= kEventsFieldVariants;
  const unsigned filter = variant % kEventsFilterVariants;
  variant /= kEventsFilterVariants;
  const bool playing = variant % kEventsPlayVariants != 0;
  variant /= kEventsPlayVariants;
  const bool empty = variant >= kWindows - 1u;
  if (empty) {
    variant = kWindows - 1u;
  }
  Window w = window_of(variant);
  if (lane_window && !empty) {
    w.track = 4;
  }
  ViewState view = window_view(w);
  const unsigned pattern = empty ? 2u : 1u;
  events_view_state(view, pattern, field, static_cast<model::events::Filter>(filter));
  if (playing) {  // the play marker (3.7): the sequencer on a step of the pattern on show
    view.play.playing = true;
    view.play.pattern = static_cast<std::uint8_t>(pattern);
    view.play.step = 8u;
  }
  scene(r, d.model, view);
}

// ---- the Pattern screen's choosers (OS-009 P1a.1) -----------------------------------------------------------------------
//
// A strip over the info line, over a document: the typical states show the demo's pattern in Edit, the sweep puts each chooser over the
// dearest content with every header mark and the Shift layer on.

ViewState chooser_view(OverlayId id) {
  ViewState v;
  v.mode[app::screen_index(ScreenId::Pattern)] = static_cast<std::uint8_t>(app::PatternMode::Edit);
  v.pattern.scroll_rows(6);
  for (int i = 0; i < 6; ++i) {
    v.pattern.move_right();
  }
  v.overlay = id;
  return v;
}

void draw_pattern_chooser(ui::Renderer& r, unsigned) {
  scene(r, pattern_doc_model(0, false), chooser_view(OverlayId::PatternChooser));
}
void draw_pattern_length(ui::Renderer& r, unsigned) {
  scene(r, pattern_doc_model(0, false), chooser_view(OverlayId::LengthChooser));
}
void draw_pattern_jump(ui::Renderer& r, unsigned) {
  scene(r, pattern_doc_model(0, false), chooser_view(OverlayId::JumpChooser));
}

constexpr unsigned kChooserVariants = kWindows / 2 * 3;  // the choosers are only there over a document

void draw_pattern_chooser_worst(ui::Renderer& r, unsigned variant) {
  static constexpr OverlayId kIds[3] = {OverlayId::PatternChooser, OverlayId::LengthChooser, OverlayId::JumpChooser};
  const OverlayId id = kIds[variant % 3];
  variant = variant / 3 * 2 + 1;  // the window's document bit set
  const Window w = window_of(variant);
  ViewState v = window_view(w);
  v.overlay = id;
  scene(r, window_model(w), v);
}

// ---- the F7 verb menu (OS-PTOOLS2 S3) -------------------------------------------------------------------------------------
//
// The menu open over the Pattern screen the way verb_open opens it: the cursor stays where the last open left it. The typical state is the cursor on the first
// verb over a document; the sweep takes every window of the dearest base with the cursor on each of the sixteen verbs.

void draw_verb_menu(ui::Renderer& r, unsigned) {
  ViewState v = pattern_view(3, 6);
  v.mode[app::screen_index(ScreenId::Pattern)] = static_cast<std::uint8_t>(app::PatternMode::Edit);
  v.overlay = OverlayId::VerbMenu;
  scene(r, pattern_doc_model(0, false), v);
}

constexpr unsigned kVerbMenuWorstVariants = kWindows * app::kVerbCount;

void draw_verb_menu_worst(ui::Renderer& r, unsigned variant) {
  const unsigned cursor = variant % app::kVerbCount;
  variant /= app::kVerbCount;
  const Window w = window_of(variant);
  ViewState v = window_view(w);
  v.overlay = OverlayId::VerbMenu;
  v.verb_menu.cursor = static_cast<std::uint8_t>(cursor);
  scene(r, window_model(w), v);
}

// ---- the hidden INPUT TEST screen (OS-007d) ------------------------------------------------------------------------------

ViewState input_test_view() {
  ViewState v;
  v.screen = ScreenId::InputTest;
  v.input_test.stats.online = hal::InputStats::kPadsOnline | hal::InputStats::kButtonsOnline;
  return v;
}

void draw_input_test_typical(ui::Renderer& r, unsigned) {
  ViewState v = input_test_view();
  core::InputEvent e{};
  const core::Tick now = 5000;
  // Shift and F1 down, Play pressed and let go, pad 5 held, pad 30 touched twice (a bounce), the dial turned.
  const unsigned down[] = {29, 0};
  for (const unsigned b : down) {
    e = core::make_button_event(1000, static_cast<std::uint8_t>(b), true);
    (void)v.input_test.observe(e, now);
  }
  e = core::make_button_event(1000, 23, true);
  (void)v.input_test.observe(e, now);
  e = core::make_button_event(2000, 23, false);
  (void)v.input_test.observe(e, now);
  e = core::make_pad_event(1000, 5, 127);
  (void)v.input_test.observe(e, now);
  for (int i = 0; i < 2; ++i) {
    e = core::make_pad_event(1000, 30, 127);
    (void)v.input_test.observe(e, now);
    e = core::make_pad_event(1500, 30, 0);
    (void)v.input_test.observe(e, now);
  }
  e = core::make_encoder_event(3000, 0, 3);
  (void)v.input_test.observe(e, now);
  e = core::make_encoder_event(3500, 0, -1);
  (void)v.input_test.observe(e, now);
  v.input_test.stats.filtered = 2;
  scene(r, demo_model(), v);
}

void draw_input_test_worst(ui::Renderer& r, unsigned) {
  ViewState v = input_test_view();
  InputTestState& s = v.input_test;
  for (unsigned b = 0; b < app::kButtonCount; ++b) {
    s.button_presses[b] = app::kInputTestCountMax;
  }
  for (unsigned p = 0; p < app::kInputTestPads; ++p) {
    s.pad_presses[p] = app::kInputTestCountMax;
  }
  // Every control counted and none down: a framed cell with its name and its count is the dearest cell (a cell that is down is one
  // filled box).
  s.encoder_total = -2147483647;
  s.encoder_last = -32768;
  s.encoder_events = 4294967295u;
  s.events = 4294967295u;
  s.last_age_us = 4294967295u;
  s.max_age_us = 4294967295u;
  s.stats.dropped = s.stats.errors = s.stats.filtered = s.stats.resyncs = 4294967295u;
  // OS-BUG-PADS: the pad bus line, the longest it gets (both chips refused, the counts past the display's end, a held bus, the lines read).
  s.stats.pad_bus.scanner = {2, 2, 4294967295u};
  s.stats.pad_bus.leds = {1, 3, 4294967295u};
  s.stats.pad_bus.clear = hal::PadBusStats::kClearHeld;
  s.stats.pad_bus.clears = 255;
  s.stats.pad_bus.lines_first = hal::PadBusStats::kLineScl | hal::PadBusStats::kLineSda;
  s.stats.pad_bus.lines_now = hal::PadBusStats::kLineScl | hal::PadBusStats::kLineSda | hal::PadBusStats::kLineInt;
  scene(r, demo_model(), v);
}

}  // namespace

const ScreenState kInputTestStates[2] = {
    {"typical", true, 1, kBudgetWords, draw_input_test_typical},
    {"worst", true, 1, kBudgetWords, draw_input_test_worst},
};

// Every state is held to the standard budget in every theme and both modes: the typical ones are one-variant worst cases.
const ScreenState kSongStates[16] = {
    {"typical", true, 1, kBudgetWords, draw_song_typical},
    {"worst", true, kSongWorstVariants, kBudgetWords, draw_song_worst},
    // OS-SONG1: over a document, with the track map of each row and a queued jump blinking.
    {"document", true, 1, kBudgetWords, draw_song_document},
    {"document_worst", true, 8, kBudgetWords, draw_song_document_worst, false},
    // OS-SONGMAP: the cursor in the track map with a rectangle selected, the dearest of it (16 tracks, 16 slots), and the question's key row.
    {"document_map", true, 1, kBudgetWords, draw_song_document_map},
    {"document_map_worst", true, 8, kBudgetWords, draw_song_document_map_worst, false},
    {"document_ask", true, 1, kBudgetWords, draw_song_document_ask},
    // OS-BUG-SONGMAPCUR: the cursor on the last track of a map whose last tracks are empty (12 and 16 tracks), and the dearest of the empty cells' ticks.
    {"document_map_empty", true, 1, kBudgetWords, draw_song_document_map_empty},
    {"document_map_16", true, 1, kBudgetWords, draw_song_document_map_16},
    {"document_map_churn", true, 8, kBudgetWords, draw_song_document_map_churn, false},
    // OS-SONG-TIMELINE: the TIMELINE page playing, and the dearest sweep (the thumbnail cap, four variants).
    {"timeline", true, 1, kBudgetWords, draw_song_timeline_document},
    {"timeline_worst", true, 64, kBudgetWords, draw_song_timeline_worst, false},
    // OS-SONG3 S2: the TIMELINE as an editing grid: the cursor's cell and a rectangle, and the dearest sweep of it.
    {"timeline_edit", true, 1, kBudgetWords, draw_song_timeline_edit},
    {"timeline_edit_worst", true, 64, kBudgetWords, draw_song_timeline_edit_worst, false},
    // OS-SONG3 S3 (review fold): the same sweep with alternating tracks muted (the worst run count of unmuted lanes); the top variant bit truncates the song to 32 slots on one pattern, the shared mark's worst case (every block wide enough for number and `*`).
    {"timeline_edit_worst_dim", true, 128, kBudgetWords, draw_song_timeline_edit_worst_dim, false},
    // OS-SONG3 S3 (review fold): the combined sweep — the full-window selection, alternating mutes and the shared marks together. Not a worst-case state: held to the chip's 2048 only, not the 1792 standard.
    {"timeline_edit_worst_all", false, 128, 0, draw_song_timeline_edit_worst_all, false},
};

// The `_worst` sweeps are measured but not rendered to the
// gallery: the typical states are what is looked at.
const ScreenState kOverlayStates[22] = {
    {"pattern_learn", true, 1, kBudgetWords, draw_learn},
    {"pattern_learn_worst", true, kWindows, kBudgetWords, draw_learn_worst, false},
    {"pattern_navigator", true, 1, kBudgetWords, draw_navigator},
    {"pattern_navigator_worst", true, kNavigatorVariants, kBudgetWords, draw_navigator_worst, false},
    {"pattern_notify", true, 1, kBudgetWords, draw_pattern_notify},
    {"pattern_notify_worst", true, kNotifyVariants, kBudgetWords, draw_pattern_notify_worst, false},
    {"song_notify", true, 1, kBudgetWords, draw_song_notify},
    {"song_notify_worst", true, kSongNotifyVariants, kBudgetWords, draw_song_notify_worst, false},
    // OS-009 P1a.1: the choosers of F1, F2 and F3 over the Pattern screen.
    {"pattern_chooser_typical", true, 1, kBudgetWords, draw_pattern_chooser},
    {"pattern_chooser_worst", true, kChooserVariants, kBudgetWords, draw_pattern_chooser_worst, false},
    {"pattern_length", true, 1, kBudgetWords, draw_pattern_length},
    {"pattern_jump", true, 1, kBudgetWords, draw_pattern_jump},
    // OS-SONGMAP: the Song screen's TOOLS dialog.
    {"song_tools", true, 1, kBudgetWords, draw_song_tools},
    {"song_tools_form", true, 1, kBudgetWords, draw_song_tools_form},
    {"song_tools_bridge", true, 1, kBudgetWords, draw_song_tools_bridge},
    {"song_tools_worst", true, kSongToolsVariants, kBudgetWords, draw_song_tools_worst, false},
    // OS-MIDIREC S1b: the EVENTS overlay over the Pattern screen.
    {"events", true, 1, kBudgetWords, draw_events},
    {"events_worst", true, kEventsWorstVariants, kBudgetWords, draw_events_worst, false},
    // OS-LEARNMAP: the LEARN MAP overlay (typical: two bindings, idle and waiting for a new knob; worst: all 32 slots).
    {"learn_map", true, 2, kBudgetWords, draw_learn_map},
    {"learn_map_worst", true, kLearnMapWorstVariants, kBudgetWords, draw_learn_map_worst, false},
    // OS-PTOOLS2 S3: the F7 verb menu over the Pattern screen (its own budget state).
    {"pattern_verb_menu", true, 1, kBudgetWords, draw_verb_menu},
    {"pattern_verb_menu_worst", true, kVerbMenuWorstVariants, kBudgetWords, draw_verb_menu_worst, false},
};

}  // namespace o3os::sim
