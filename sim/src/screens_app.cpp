// SPDX-License-Identifier: GPL-3.0-or-later
#include "screens_app.h"

#include <o3os/app/input_test.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/song_data.h>
#include <o3os/app/song_tools_view.h>
#include <o3os/app/view_state.h>
#include <o3os/model/project.h>

#include <cstring>
#include <memory>

namespace o3os::sim {

namespace {

using app::InputTestState;
using app::Model;
using app::OverlayId;
using app::ScreenId;
using app::ViewState;

constexpr unsigned kPageOrder = 0;
constexpr unsigned kPageArrangement = 1;
constexpr unsigned kPageTimeline = 2;
constexpr unsigned kPageStage = 3;

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

void draw_song_typical(ui::Renderer& r, unsigned) { scene(r, demo_model(), song_view(kPageOrder, 0, demo_model())); }

void draw_song_arrangement(ui::Renderer& r, unsigned) {
  scene(r, demo_model(), song_view(kPageArrangement, 8, demo_model()));
}

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
  ViewState v = song_doc_view(kPageOrder, 5);
  song_doc_queued(v);
  scene(r, d.model, v);
}

void draw_song_document_arrangement(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc(false);
  scene(r, d.model, song_doc_view(kPageArrangement, 12));
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
  ViewState v = song_doc_map_view(kPageOrder, 5, 12, 4, 3);
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

// The last track of a 12-track song that uses 8, on the Order page, and of a 16-track song that uses 10, on the Arrangement page (the map at the right edge of the screen).
void draw_song_document_map_empty(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc_sparse(0);
  scene(r, d.model, song_doc_map_at(kPageOrder, 5, 12, 12));
}

void draw_song_document_map_16(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc_sparse(1);
  scene(r, d.model, song_doc_map_at(kPageArrangement, 5, 16, 16));
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
  ViewState v = song_doc_map_view(kPageOrder, 5, 12, 1, 1);
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

// OS-SONG-STAGE: the STAGE page over the document. The typical one plays slot 2 (a 64-step pattern) at bar 2 with slot 3 next; the dearest sweeps stopped, next, a live jump, the loop at the
// song's end and a pattern-mode playback, with the playhead at the start, in the middle and at the end of the pattern.
void song_stage_play(ViewState& v, unsigned order, unsigned pattern, unsigned step) {
  v.playing = true;
  v.play.playing = true;
  v.play.order = static_cast<std::uint16_t>(order);
  v.play.pattern = static_cast<std::uint8_t>(pattern);
  v.play.step = static_cast<std::uint16_t>(step);
  v.play.ustep = 24;
}

void draw_song_stage_document(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc(false);
  ViewState v = song_doc_view(kPageStage, 0);
  song_stage_play(v, 2, 3, 21);
  scene(r, d.model, v);
}

void draw_song_stage_worst(ui::Renderer& r, unsigned variant) {
  SongDoc& d = song_doc(true);
  ViewState v = song_doc_view(kPageStage, 5);
  const unsigned kind = variant % 5;
  const unsigned step = (variant / 5 % 3) * 31;  // 0, 31, 62 of 64
  if (kind == 1) {
    song_stage_play(v, 2, 3, step);
  } else if (kind == 2) {
    song_stage_play(v, 2, 3, step);
    v.play.queued = 7;
  } else if (kind == 3) {
    song_stage_play(v, 39, 8, step);
  } else if (kind == 4) {
    song_stage_play(v, audio::PlayPosition::kNoOrder, 3, step);
  }
  scene(r, d.model, v);
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
  song_stage_play(v, 14, 7, 20);
  scene(r, d.model, v);
}

// The sweep covers every count of active tracks (1 to 16: the slots in view are min(40, budget / (tracks + overhead)), so the dearest corner moves with the count), the cursor at the start
// and in the middle of the 40 one-bar slots, and the playhead off and on. Single-digit ids cycling 1..8 change the colour at every slot and fit their number on every block.
void draw_song_timeline_worst(ui::Renderer& r, unsigned variant) {
  SongDoc& d = song_doc_short();
  (void)d.arena->project.set_active_tracks(variant / 4 + 1u);
  ViewState v = song_doc_view(kPageTimeline, variant % 2 == 0 ? 0 : 20);
  if (variant / 2 % 2 == 1) {
    song_stage_play(v, variant % 2 == 0 ? 3 : 22, 2, 9);
  }
  scene(r, d.model, v);
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
  ViewState v = song_view(kPageOrder, 3, demo_model());
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
  v.tools.tool = static_cast<app::SongTool>(tool % 4);
  v.tools.row = static_cast<std::uint8_t>(row % 2);
  v.tools.amount = extreme ? -24 : 7;
  v.tools.instrument = extreme ? 64 : 5;
  v.tools.what = extreme ? app::SongWhat::Volume : app::SongWhat::All;
  v.tools.rect = extreme ? app::SongRect{0, 39, 0, 15} : app::SongRect{5, 8, 1, 3};
  v.tools.patterns = extreme ? 8 : 3;
  v.tools.whole = extreme;  // (EXPORT WAV with every track: the scope line says ALL TRACKS)
}

void draw_song_tools(ui::Renderer& r, unsigned) {
  SongDoc& d = song_doc(false);
  ViewState v = song_doc_map_view(kPageOrder, 5, 12, 4, 3);
  song_tools_state(v, 0, 0, false);
  scene(r, d.model, v);
}

constexpr unsigned kSongToolsVariants = 4 * 2 * 2 * 2;

void draw_song_tools_worst(ui::Renderer& r, unsigned variant) {
  SongDoc& d = song_doc(true);
  const unsigned tool = variant % 4;
  variant /= 4;
  const unsigned row = variant % 2;
  variant /= 2;
  const bool extreme = variant % 2 != 0;
  variant /= 2;
  ViewState v = song_doc_map_view(variant % 2, 25, 16, 16, 15);
  song_doc_queued(v);
  song_tools_state(v, tool, row, extreme);
  scene(r, d.model, v);
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
    {"arrangement", true, 1, kBudgetWords, draw_song_arrangement, false},
    {"worst", true, kSongWorstVariants, kBudgetWords, draw_song_worst},
    // OS-SONG1: over a document, with the track map of each row and a queued jump blinking.
    {"document", true, 1, kBudgetWords, draw_song_document},
    {"document_arrangement", true, 1, kBudgetWords, draw_song_document_arrangement},
    {"document_worst", true, 8, kBudgetWords, draw_song_document_worst, false},
    // OS-SONGMAP: the cursor in the track map with a rectangle selected, the dearest of it (16 tracks, 16 slots), and the question's key row.
    {"document_map", true, 1, kBudgetWords, draw_song_document_map},
    {"document_map_worst", true, 8, kBudgetWords, draw_song_document_map_worst, false},
    {"document_ask", true, 1, kBudgetWords, draw_song_document_ask},
    // OS-BUG-SONGMAPCUR: the cursor on the last track of a map whose last tracks are empty (12 and 16 tracks), and the dearest of the empty cells' ticks.
    {"document_map_empty", true, 1, kBudgetWords, draw_song_document_map_empty},
    {"document_map_16", true, 1, kBudgetWords, draw_song_document_map_16},
    {"document_map_churn", true, 8, kBudgetWords, draw_song_document_map_churn, false},
    // OS-SONG-STAGE: the STAGE page playing, and the dearest sweep (five modes x three playhead places).
    // OS-SONG-TIMELINE: the TIMELINE page playing, and the dearest sweep (the thumbnail cap, four variants).
    {"timeline", true, 1, kBudgetWords, draw_song_timeline_document},
    {"timeline_worst", true, 64, kBudgetWords, draw_song_timeline_worst, false},
    {"stage", true, 1, kBudgetWords, draw_song_stage_document},
    {"stage_worst", true, 15, kBudgetWords, draw_song_stage_worst, false},
};

// The `_worst` sweeps (and the arrangement page, which the song scenario shows) are measured but not rendered to the
// gallery: the typical states are what is looked at.
const ScreenState kOverlayStates[14] = {
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
    {"song_tools_worst", true, kSongToolsVariants, kBudgetWords, draw_song_tools_worst, false},
};

}  // namespace o3os::sim
