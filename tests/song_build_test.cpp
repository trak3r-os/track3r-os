// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SONG-FORM (docs/specs/OS-SONG-BUILD.md; new Community OS behaviour, no format change): the TOOLS dialog's FORM, a song shape from 1 to 3 patterns by subtraction. The forms' tables, the planner (sources, copies,
// sharing, the pre-check and its exact texts), the executor through the dialog (the new slots after the rectangle, the cursor, the copies' contents, one undo unit that redo restores), every refusal checked to write
// nothing (by hash), the edit while the transport runs, and the dialog's rows and footer. BRIDGE has its own cases below the FORM ones (OS-SONG-BRIDGE).
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/model.h>
#include <o3os/app/song_build.h>
#include <o3os/app/song_edit.h>
#include <o3os/app/song_tools_view.h>
#include <o3os/app/view_state.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/model/limits.h>
#include <o3os/model/project.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::SongBuildRefusal;
using app::SongForm;
using model::UndoArea;

namespace {

alignas(8) std::uint8_t g_region[model::kUndoRegionBytes];

struct JumpLog final : app::TransportSink {
  bool queue_jump(unsigned) noexcept override { return true; }
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
};

constexpr std::uint8_t note_of(unsigned id, unsigned track) { return static_cast<std::uint8_t>(40u + 8u * id + track); }

using Order = std::vector<unsigned>;

bool empty_step_of(const model::Step& s) { return model::is_empty(s); }

// An App on the Song screen with a document and the device's three rings. Patterns 1..`patterns` of `length` steps; pattern p holds a note on step 0 of each of the first `tracks` tracks (note_of).
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoHistory history;
  std::unique_ptr<model::Editor> editor;
  JumpLog log;
  audio::PlayPosition position;

  explicit Rig(const Order& order, unsigned patterns = 3, unsigned length = 16, unsigned tracks = 12) {
    model::Project& p = project();
    p.reset();
    NEED(p.set_active_tracks(tracks) == model::Status::Ok);
    for (unsigned id = 1; id <= patterns; ++id) {
      NEED(p.create_pattern(id, length) == model::Status::Ok);
      for (unsigned t = 0; t < tracks; ++t) {
        model::Step s = model::kEmptyStep;
        s.note = note_of(id, t);
        s.instr = 1;
        NEED(p.set_step(id, t, 0, s) == model::Status::Ok);
      }
    }
    for (const unsigned id : order) {
      NEED(p.append_order(id) == model::Status::Ok);
    }
    history.begin(g_region, sizeof g_region);
    editor = std::make_unique<model::Editor>(p, history);
    app->attach_document(editor.get());
    app->attach_transport(&log);
    app->attach_play_position(&position);
    app->run_once();
    tap(Button::Song);
  }
  model::Project& project() { return test::arena(0).project; }
  const app::SongView& song() const { return app->state().song; }
  const app::ViewState& state() const { return app->state(); }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b, unsigned n = 1) {
    for (unsigned i = 0; i < n; ++i) {
      press(b);
      pass(1);
      release(b);
      pass(1);
    }
  }
  void shift_tap(Button b, unsigned n = 1) {
    press(Button::Shift);
    pass(1);
    tap(b, n);
    release(Button::Shift);
    pass(1);
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass(1);
  }
  std::string last() const { return state().notify.last_text(); }
  Order order() {
    Order v;
    for (std::uint32_t i = 0; i < project().song_length(); ++i) {
      v.push_back(project().order(i));
    }
    return v;
  }
  model::Step step(unsigned id, unsigned track, unsigned row = 0) { return project().pattern(id)->steps[track][row]; }
  unsigned song_depth() const { return history.ring[1].depth(); }
  std::size_t song_bytes() const { return history.ring[1].used_bytes(); }

  // Everything a refused build must leave alone: a hash of the list, the pattern pool and the steps, and the Song ring.
  struct Snap {
    std::uint64_t hash = 0;
    unsigned depth = 0;
    std::size_t bytes = 0;
    bool operator==(const Snap& o) const { return hash == o.hash && depth == o.depth && bytes == o.bytes; }
  };
  Snap snap() {
    std::uint64_t h = 0xCBF29CE484222325ull;
    const auto mix = [&h](const void* data, std::size_t n) {
      const auto* b = static_cast<const std::uint8_t*>(data);
      for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001B3ull;
      }
    };
    const model::Project& p = project();
    for (std::uint32_t i = 0; i < p.song_length(); ++i) {
      const std::uint8_t id = static_cast<std::uint8_t>(p.order(i));
      mix(&id, 1);
    }
    const std::uint32_t count = p.pattern_count();
    mix(&count, sizeof count);
    for (unsigned id = 1; id <= model::kMaxPatternId; ++id) {
      const model::Pattern* pat = p.pattern(id);
      if (pat != nullptr) {
        mix(&id, sizeof id);
        mix(pat->track_length, sizeof pat->track_length);
        mix(pat->name, sizeof pat->name);
        mix(pat->steps, sizeof pat->steps);
      }
    }
    return {h, song_depth(), song_bytes()};
  }

  // F6, the OP FORM, the FORM row `form` (0..2) and the DRUMS row `drums`; the dialog stays open.
  void open_form(unsigned form, unsigned drums = 4) {
    tap(Button::F6);
    while (state().tools.tool != app::SongTool::Form) {
      turn(1);
    }
    tap(Button::Down);
    turn(static_cast<int>((form + 3u - static_cast<unsigned>(state().tools.form)) % 3u));
    tap(Button::Down);
    turn(static_cast<int>(drums) - static_cast<int>(state().tools.drums));
  }
  void build(unsigned form, unsigned drums = 4) {
    open_form(form, drums);
    tap(Button::F1);
  }
  app::SongBuildPlan plan(unsigned form, unsigned drums, int s0, int s1) {
    app::SongBuildPlan out;
    app::song_form_plan(project(), app::SongRect{s0, s1, 0, 15}, static_cast<SongForm>(form), drums, out);
    return out;
  }
};

// The source of the pattern a plan's slot names: the copy's `from`, or the pattern itself.
unsigned source_of(const app::SongBuildPlan& plan, unsigned id) {
  for (unsigned c = 0; c < plan.info.copies; ++c) {
    if (plan.copy[c].to == id) {
      return plan.copy[c].from;
    }
  }
  return id;
}

std::uint16_t cleared_of(const app::SongBuildPlan& plan, unsigned id) {
  for (unsigned c = 0; c < plan.info.copies; ++c) {
    if (plan.copy[c].to == id) {
      return plan.copy[c].cleared;
    }
  }
  return 0;
}

// The three forms, written out again from the spec's table (not from the code's bytes): per slot the source role and the groups cleared.
const std::vector<std::string> kExpected[3] = {
    {"A:M", "A:K", "A:-", "A:-", "A:KP", "A:-", "A:-", "A:M"},
    {"A:KP", "A:-", "A:K", "B:-", "A:-", "B:-", "C:KP", "B:-", "B:KP"},
    {"A:PM", "A:M", "A:-", "A:-", "B:KP", "B:K", "B:-", "C:-", "A:M", "A:PM"},
};

std::string entry_text(std::uint8_t e) {
  std::string s(1, static_cast<char>('A' + ((e >> 4) & 3)));
  s += ':';
  std::string g;
  g += (e & app::kFormKick) != 0 ? "K" : "";
  g += (e & app::kFormPerc) != 0 ? "P" : "";
  g += (e & app::kFormMusic) != 0 ? "M" : "";
  return s + (g.empty() ? "-" : g);
}

}  // namespace

// ---- the forms ---------------------------------------------------------------------------------------------------------------------------

TEST_CASE("song form: every form has its slot count and, per slot, the source role and the groups cleared the spec's table gives") {
  for (unsigned f = 0; f < app::kSongFormCount; ++f) {
    unsigned n = 0;
    const std::uint8_t* table = app::song_form_table(static_cast<SongForm>(f), n);
    NEED(n == kExpected[f].size());
    for (unsigned k = 0; k < n; ++k) {
      CHECK(entry_text(table[k]) == kExpected[f][k]);
    }
  }
  unsigned n = 0;
  (void)app::song_form_table(SongForm::LoopSong, n);
  CHECK(n == 8u);
  (void)app::song_form_table(SongForm::Pop, n);
  CHECK(n == 9u);
  (void)app::song_form_table(SongForm::Techno, n);
  CHECK(n == 10u);
  CHECK(std::string(app::song_form_name(SongForm::LoopSong)) == "LOOP>SONG");
  CHECK(std::string(app::song_form_name(SongForm::Pop)) == "POP");
  CHECK(std::string(app::song_form_name(SongForm::Techno)) == "TECHNO");
}

TEST_CASE("song form: DRUMS 1, 4 and 8 move the kick, the rest of the drums and the music (K is track 1, P tracks 2..N, M the tracks after)") {
  using app::kFormKick;
  using app::kFormMusic;
  using app::kFormPerc;
  CHECK(app::song_form_tracks(kFormKick, 4) == 0x0001u);
  CHECK(app::song_form_tracks(kFormPerc, 4) == 0x000Eu);
  CHECK(app::song_form_tracks(kFormMusic, 4) == 0xFFF0u);
  CHECK(app::song_form_tracks(kFormKick | kFormPerc | kFormMusic, 4) == 0xFFFFu);
  CHECK(app::song_form_tracks(kFormKick, 1) == 0x0001u);
  CHECK(app::song_form_tracks(kFormPerc, 1) == 0x0000u);  // no other drum track
  CHECK(app::song_form_tracks(kFormMusic, 1) == 0xFFFEu);
  CHECK(app::song_form_tracks(kFormPerc, 8) == 0x00FEu);
  CHECK(app::song_form_tracks(kFormMusic, 8) == 0xFF00u);
  CHECK(app::song_form_tracks(0, 4) == 0u);
}

TEST_CASE("song form: copies are shared by the tracks they really empty: DRUMS 1 (no rest of the drums) makes POP's K+P and K one copy, one fewer than DRUMS 4") {
  Rig rig({1, 2, 3}, 3, 16, 12);
  const app::SongBuildPlan four = rig.plan(1, 4, 0, 2);
  NEED(four.info.refusal == SongBuildRefusal::None);
  CHECK(four.info.copies == 4);  // (A,K+P), (A,K), (C,K+P), (B,K+P)
  const app::SongBuildPlan one = rig.plan(1, 1, 0, 2);
  NEED(one.info.refusal == SongBuildRefusal::None);
  CHECK(one.info.copies == 3);  // K+P empties the kick alone, as K does: (A,K), (C,K), (B,K)
  CHECK(one.slot_pattern[0] == one.slot_pattern[2]);  // slot 1 (A, K+P) and slot 3 (A, K) are the same pattern
  CHECK(cleared_of(one, one.slot_pattern[0]) != 0u);
  CHECK(source_of(one, one.slot_pattern[0]) == 1u);
  CHECK(one.slot_pattern[6] != one.slot_pattern[8]);  // C and B are different sources
}

TEST_CASE("song form: three sources give each form's table, A B C in slot order, the copies shared by (source, tracks emptied): 3, 4 and 4 copies at most") {
  Rig rig({1, 2, 3});
  const unsigned copies_of[3] = {3, 4, 4};
  const unsigned want_src[3] = {1, 2, 3};
  for (unsigned f = 0; f < app::kSongFormCount; ++f) {
    const app::SongBuildPlan plan = rig.plan(f, 4, 0, 2);
    NEED(plan.info.refusal == SongBuildRefusal::None);
    CHECK(plan.info.source_count == 3);
    CHECK(plan.info.sources[0] == 1);
    CHECK(plan.info.sources[1] == 2);
    CHECK(plan.info.sources[2] == 3);
    CHECK(plan.info.slots == kExpected[f].size());
    CHECK(plan.info.copies == copies_of[f]);
    CHECK(plan.at == 3u);
    for (unsigned k = 0; k < plan.info.slots; ++k) {
      const std::string& e = kExpected[f][k];
      const unsigned role = static_cast<unsigned>(e[0] - 'A');
      const std::uint16_t clear = e[2] == '-' ? 0u : app::song_form_tracks(static_cast<std::uint8_t>((e.find('K') != std::string::npos ? 1 : 0) | (e.find('P') != std::string::npos ? 2 : 0) | (e.find('M') != std::string::npos ? 4 : 0)), 4);
      CHECK(source_of(plan, plan.slot_pattern[k]) == want_src[role]);
      CHECK(cleared_of(plan, plan.slot_pattern[k]) == clear);
      CHECK((clear == 0u) == (plan.slot_pattern[k] == want_src[role]));  // no clear, no copy: the slot is the source
    }
    CHECK(plan.bytes > app::kSongUnitOverhead);
  }
}

TEST_CASE("song form: LOOP>SONG on one pattern: the numbers of the copies are the first free ones, equal (source, tracks emptied) share one copy, the rest is the source itself") {
  Rig rig({1, 2, 3});
  const app::SongBuildPlan plan = rig.plan(0, 4, 0, 0);
  NEED(plan.info.refusal == SongBuildRefusal::None);
  CHECK(plan.info.source_count == 1);
  CHECK(plan.info.slots == 8);
  CHECK(plan.info.copies == 3);
  const std::uint8_t want[8] = {4, 5, 1, 1, 6, 1, 1, 4};  // slots 1 and 8 share the copy (A, clear M)
  for (unsigned k = 0; k < 8; ++k) {
    CHECK(plan.slot_pattern[k] == want[k]);
  }
  CHECK(plan.copy[0].from == 1);
  CHECK(plan.copy[0].cleared == 0xFFF0u);
  CHECK(plan.copy[1].cleared == 0x0001u);
  CHECK(plan.copy[2].cleared == 0x000Fu);
  // a missing B and C are A: POP on one pattern uses A for every role
  const app::SongBuildPlan pop = rig.plan(1, 4, 0, 0);
  for (unsigned k = 0; k < pop.info.slots; ++k) {
    CHECK(source_of(pop, pop.slot_pattern[k]) == 1u);
  }
  // two sources: C is A
  const app::SongBuildPlan two = rig.plan(1, 4, 0, 1);
  CHECK(two.info.source_count == 2);
  CHECK(source_of(two, two.slot_pattern[3]) == 2u);  // slot 4 is B
  CHECK(source_of(two, two.slot_pattern[6]) == 1u);  // slot 7 is C: A
  // the same pattern in several slots of the rectangle is one source
  Rig again({2, 2, 3});
  CHECK(again.plan(1, 4, 0, 1).info.source_count == 1);
}

TEST_CASE("song form: a cleared group with no data gives no copy: the slot is the source; steps past a track's length are no data") {
  Rig rig({1, 2}, 2, 16, 4);  // four tracks of data: the music (tracks 5..) holds nothing at DRUMS 4
  model::Step far = model::kEmptyStep;
  far.note = 60;
  NEED(rig.project().set_step(1, 6, 100, far) == model::Status::Ok);  // track 7, past its 16 steps: it never sounds
  const app::SongBuildPlan plan = rig.plan(0, 4, 0, 0);
  NEED(plan.info.refusal == SongBuildRefusal::None);
  CHECK(plan.info.copies == 2);          // (A, K) and (A, K+P) only
  CHECK(plan.slot_pattern[0] == 1);      // A, clear M: nothing to clear
  CHECK(plan.slot_pattern[7] == 1);
  CHECK(plan.slot_pattern[1] != 1);
  CHECK(plan.slot_pattern[4] != 1);
  // DRUMS 1: the music starts at track 2 and the data is there; K and K+P empty the same track (no rest of the drums), so they share one copy
  CHECK(rig.plan(0, 1, 0, 0).info.copies == 2);
  // DRUMS 8 on four tracks: the music is empty, the rest of the drums hold data
  const app::SongBuildPlan eight = rig.plan(0, 8, 0, 0);
  CHECK(eight.info.copies == 2);
  CHECK(cleared_of(eight, eight.slot_pattern[4]) == 0x00FFu);
}

// ---- the planner's refusals, each without a write ---------------------------------------------------------------------------------------------

TEST_CASE("song form: more than 3 distinct patterns, or a slot that names no pattern, refuses with its text and writes nothing") {
  {  // (one Rig at a time: they share the project of arena 0)
    Rig rig({1, 2, 3, 3, 1, 9}, 3, 16, 12);  // 9 does not exist
    CHECK(rig.plan(0, 4, 0, 3).info.refusal == SongBuildRefusal::None);  // 1 2 3 3: three distinct
    CHECK(rig.plan(0, 4, 0, 4).info.refusal == SongBuildRefusal::None);  // 1 2 3 3 1: still three
    const app::SongBuildPlan missing = rig.plan(0, 4, 4, 5);
    CHECK(missing.info.refusal == SongBuildRefusal::NoPattern);
    CHECK(missing.info.number == 5);
    const Rig::Snap before_missing = rig.snap();
    rig.tap(Button::Down, 4);
    rig.shift_tap(Button::Down, 1);
    rig.build(0);
    CHECK(rig.last() == "SLOT 05 HAS NO PATTERN");
    CHECK(rig.snap() == before_missing);
  }
  {
    Rig four({1, 2, 3, 4}, 4);
    CHECK(four.plan(0, 4, 0, 3).info.refusal == SongBuildRefusal::Select);
    const Rig::Snap before = four.snap();
    four.shift_tap(Button::Down, 3);
    four.build(0);
    CHECK(four.last() == "SELECT 1-3 PATTERNS");
    CHECK(four.state().overlay == app::OverlayId::None);
    CHECK(four.snap() == before);
  }
}

TEST_CASE("song form: the pre-check counts slots and patterns exactly: the song one slot short, the pool one short, the numbers (a slot naming a missing pattern holds its number)") {
  // the song one slot short: 248 slots and the form needs 8 (255 is the most)
  {
    Order o = {1};
    for (unsigned id = 8; id <= 254; ++id) {
      o.push_back(id);  // 247 slots naming patterns that do not exist
    }
    Rig rig(o);
    CHECK(rig.project().song_length() == 248u);
    const app::SongBuildPlan plan = rig.plan(0, 4, 0, 0);
    CHECK(plan.info.refusal == SongBuildRefusal::SongFull);
    CHECK(plan.info.number == 8);
    CHECK(plan.info.have == 7);
    const Rig::Snap before = rig.snap();
    rig.build(0);
    CHECK(rig.last() == "SONG FULL: NEEDS 8 SLOTS, 7 FREE");
    CHECK(rig.snap() == before);
    // and 247 slots take the form exactly (255)
    NEED(rig.editor->remove_order(247) == model::Status::Ok);
    CHECK(rig.plan(0, 4, 0, 0).info.refusal == SongBuildRefusal::None);
  }
  // the pool one short: 127 patterns, the form needs 3 copies
  {
    Rig rig({1}, 127);
    const app::SongBuildPlan plan = rig.plan(0, 4, 0, 0);
    CHECK(plan.info.refusal == SongBuildRefusal::NoRoom);
    CHECK(plan.info.number == 3);
    CHECK(plan.info.have == 1);
    CHECK(plan.pool_left == 1u);
    const Rig::Snap before = rig.snap();
    rig.build(0);
    CHECK(rig.last() == "NEEDS 3 FREE PATTERNS, 1 FREE");
    CHECK(rig.snap() == before);
  }
  // the numbers: 7 patterns, and 246 slots that name ids 8..253 (none exists): two numbers are free
  {
    Order o = {1};
    for (unsigned id = 8; id <= 253; ++id) {
      o.push_back(id);
    }
    Rig rig(o, 7);
    const app::SongBuildPlan plan = rig.plan(0, 4, 0, 0);
    CHECK(plan.info.refusal == SongBuildRefusal::NoRoom);
    CHECK(plan.info.number == 3);
    CHECK(plan.info.have == 2);
    CHECK(plan.free_numbers == 2u);
    CHECK(plan.pool_left == 121u);
    const Rig::Snap before = rig.snap();
    rig.build(0);
    CHECK(rig.last() == "NEEDS 3 FREE PATTERNS, 2 FREE");
    CHECK(rig.snap() == before);
  }
  // a slot that names a missing pattern holds its number back: 4 and 5 are named, the copies are 6, 7, 8
  {
    Rig rig({1, 4, 5});
    const app::SongBuildPlan plan = rig.plan(0, 4, 0, 0);
    NEED(plan.info.refusal == SongBuildRefusal::None);
    CHECK(plan.copy[0].to == 6);
    CHECK(plan.copy[1].to == 7);
    CHECK(plan.copy[2].to == 8);
  }
  // the texts: one pattern is singular
  core::FixedString<48> text;
  app::SongBuildInfo info;
  info.refusal = SongBuildRefusal::NoRoom;
  info.number = 1;
  info.have = 0;
  app::song_build_text(app::SongBuildKind::Form, info, text);
  CHECK(std::string(text.c_str()) == "NEEDS 1 FREE PATTERN, 0 FREE");
  info.refusal = SongBuildRefusal::NoPattern;
  info.number = 130;
  app::song_build_text(app::SongBuildKind::Form, info, text);
  CHECK(std::string(text.c_str()) == "SLOT 130 HAS NO PATTERN");
  info.refusal = SongBuildRefusal::Select;
  app::song_build_text(app::SongBuildKind::Bridge, info, text);
  CHECK(std::string(text.c_str()) == "SELECT 2 SLOTS");
  info.refusal = SongBuildRefusal::NoNext;
  app::song_build_text(app::SongBuildKind::Bridge, info, text);
  CHECK(std::string(text.c_str()) == "NO NEXT SLOT");
  info.refusal = SongBuildRefusal::Same;
  app::song_build_text(app::SongBuildKind::Bridge, info, text);
  CHECK(std::string(text.c_str()) == "SAME PATTERN IN BOTH SLOTS");
}

TEST_CASE("song form: a unit over half of the Song ring refuses with the cap text and writes nothing; the planner's bytes are what the editor writes") {
  Rig rig({1, 2, 3}, 3, 128, 16);
  model::Step dense = model::kEmptyStep;
  dense.note = 60;
  dense.instr = 1;
  for (unsigned t = 0; t < 16; ++t) {
    for (unsigned k = 0; k < 128; ++k) {
      NEED(rig.project().set_step(1, t, k, dense) == model::Status::Ok);
    }
  }
  const app::SongBuildPlan big = rig.plan(0, 4, 0, 0);
  NEED(big.info.refusal == SongBuildRefusal::None);
  CHECK(big.bytes > app::song_undo_limit(rig.history.ring[1].capacity_bytes()));
  const Rig::Snap before = rig.snap();
  rig.build(0);
  CHECK(rig.last() == "TOO BIG TO UNDO: SELECT LESS");
  CHECK(rig.snap() == before);
  // the sparse patterns of slots 2 and 3 are well inside the ceiling, and the plan's bytes are exact
  rig.tap(Button::Down);
  const app::SongBuildPlan small = rig.plan(0, 4, 1, 1);
  NEED(small.info.refusal == SongBuildRefusal::None);
  const std::size_t b0 = rig.song_bytes();
  rig.build(0);
  CHECK(rig.last() == "FORM LOOP>SONG: 8 SLOTS, 3 NEW PATTERNS");
  CHECK(rig.song_bytes() - b0 == small.bytes);
}

// ---- the executor ------------------------------------------------------------------------------------------------------------------------

TEST_CASE("song form: the slots go after the last slot of the rectangle, the originals stay, the cursor is on the first new slot and the selection collapses; the strip says what was made") {
  Rig rig({1, 2, 3});
  rig.build(0);  // the cursor's slot alone
  CHECK(rig.order().size() == 11u);
  CHECK(rig.order()[0] == 1u);
  CHECK(rig.order()[9] == 2u);   // the originals after the new slots
  CHECK(rig.order()[10] == 3u);
  CHECK(rig.last() == "FORM LOOP>SONG: 8 SLOTS, 3 NEW PATTERNS");
  CHECK(rig.song().pos() == 1);
  CHECK_FALSE(rig.song().selecting());
  CHECK(rig.project().pattern_count() == 6u);
  CHECK(rig.state().overlay == app::OverlayId::None);
  // a selection of slots 1..2 (patterns 2, 3 of the new list are copies; take the two originals at 9, 10): POP after the last of them
  rig.tap(Button::Down, 8);
  rig.shift_tap(Button::Down, 1);
  CHECK(rig.song().first() == 9);
  CHECK(rig.song().last() == 10);
  rig.build(1);
  CHECK(rig.last() == "FORM POP: 9 SLOTS, 3 NEW PATTERNS");  // two sources: C is A, so 3 copies
  CHECK(rig.order().size() == 20u);
  CHECK(rig.song().pos() == 11);
  CHECK_FALSE(rig.song().selecting());
  CHECK(rig.order()[9] == 2u);
  CHECK(rig.order()[10] == 3u);
  CHECK(rig.order()[19] != 3u);  // the last new slot names a copy of a source
}

TEST_CASE("song form: a copy keeps the source's name and every track's length; the cleared tracks are empty, the others hold the steps up to the length and nothing past it") {
  Rig rig({1, 2}, 2, 16, 12);
  NEED(rig.editor->set_pattern_name(1, "INTRO", 5) == model::Status::Ok);
  NEED(rig.editor->set_track_length(1, 2, 8) == model::Status::Ok);
  NEED(rig.editor->set_track_length(1, 7, 32) == model::Status::Ok);
  model::Step s = model::kEmptyStep;
  s.note = 70;
  s.instr = 3;
  s.vol = 90;
  s.fx[1] = {12, 5};
  NEED(rig.editor->set_step(1, 2, 5, s) == model::Status::Ok);    // track 3: inside its 8 steps
  NEED(rig.editor->set_step(1, 2, 12, s) == model::Status::Ok);   // past them: it never sounds
  NEED(rig.editor->set_step(1, 7, 30, s) == model::Status::Ok);   // track 8, the music, a long track
  const app::SongBuildPlan plan = rig.plan(0, 4, 0, 0);
  NEED(plan.info.refusal == SongBuildRefusal::None);
  rig.build(0);
  const unsigned clear_music = plan.slot_pattern[0];  // (A, clear M)
  const unsigned clear_kick = plan.slot_pattern[1];   // (A, clear K)
  NEED(rig.project().pattern(clear_music) != nullptr);
  NEED(rig.project().pattern(clear_kick) != nullptr);
  const model::Pattern& drums_only = *rig.project().pattern(clear_music);
  CHECK(std::string(drums_only.name) == "INTRO");
  CHECK(drums_only.track_length[2] == 8);
  CHECK(drums_only.track_length[7] == 32);
  CHECK(drums_only.track_length[0] == 16);
  CHECK(std::memcmp(&drums_only.steps[2][5], &s, sizeof s) == 0);
  CHECK(empty_step_of(drums_only.steps[2][12]));  // not copied
  CHECK(std::memcmp(&drums_only.steps[0][0], &rig.project().pattern(1)->steps[0][0], sizeof s) == 0);
  for (unsigned t = 4; t < 16; ++t) {  // the music is empty
    for (unsigned k = 0; k < 32; ++k) {
      CHECK(empty_step_of(drums_only.steps[t][k]));
    }
  }
  const model::Pattern& no_kick = *rig.project().pattern(clear_kick);
  CHECK(std::string(no_kick.name) == "INTRO");
  for (unsigned k = 0; k < 16; ++k) {
    CHECK(empty_step_of(no_kick.steps[0][k]));  // the kick is empty
  }
  CHECK(std::memcmp(&no_kick.steps[7][30], &s, sizeof s) == 0);  // the music is there
  CHECK(std::memcmp(&no_kick.steps[1][0], &rig.project().pattern(1)->steps[1][0], sizeof s) == 0);
  // the source is untouched
  CHECK(std::memcmp(&rig.project().pattern(1)->steps[2][12], &s, sizeof s) == 0);
}

TEST_CASE("song form: one undo takes the slots and the copies back together, redo restores them; the unit is one of the Song ring and the build says so on the Pattern history") {
  Rig rig({1, 2, 3});
  // a Pattern edit behind it: the creation of a pattern in a Song unit ends that history on undo (OS-UNDOGRP D4)
  rig.editor->begin_group(core::action_id("step.note"), 0, UndoArea::Pattern);
  model::Step edited = model::kEmptyStep;
  edited.note = 99;
  NEED(rig.editor->set_step(3, 0, 5, edited) == model::Status::Ok);
  rig.editor->end_group();
  rig.shift_tap(Button::Down, 2);  // slots 0..2: three sources
  const Rig::Snap before = rig.snap();
  const unsigned depth = rig.song_depth();
  const app::SongBuildPlan plan = rig.plan(1, 4, 0, 2);
  rig.build(1);
  CHECK(rig.last() == "FORM POP: 9 SLOTS, 4 NEW PATTERNS");
  const Rig::Snap built = rig.snap();
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.project().song_length() == 12u);
  CHECK(rig.project().pattern_count() == 7u);
  CHECK(built.bytes - before.bytes == plan.bytes);  // the planner counts what the editor writes
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 3});
  CHECK(rig.project().pattern_count() == 3u);
  CHECK(rig.song_depth() == depth);
  CHECK(rig.snap().hash == before.hash);
  CHECK(rig.last() == "PATTERN HISTORY CLEARED");
  CHECK(rig.editor->fence_cleared(UndoArea::Pattern));
  rig.shift_tap(Button::F8);
  CHECK(rig.snap().hash == built.hash);
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.project().song_length() == 12u);
  CHECK(rig.project().pattern_count() == 7u);
}

TEST_CASE("song form: it works while the transport runs: one change of the order list for the playing song, and the strip says what was made") {
  Rig rig({1, 2, 3});
  rig.position.publish(true, 1, 0, 12000, 0);
  rig.pass(1);
  CHECK(rig.state().play.playing);
  model::OrderSnapshot before{};
  model::OrderSnapshot after{};
  NEED(rig.project().snapshot_order(before));
  rig.build(0);  // the new slots go before the playing slot 1
  CHECK(rig.last() == "FORM LOOP>SONG: 8 SLOTS, 3 NEW PATTERNS");
  CHECK(rig.state().play.playing);
  NEED(rig.project().snapshot_order(after));
  CHECK(after.ops - before.ops == 1u);  // the 8 inserts are one op (an OrderBatch), as a range paste is
  const model::OrderOp op = after.journal[(after.ops - 1u) % model::kOrderJournal];
  CHECK(op.kind == model::OrderOp::Insert);
  CHECK(op.a == 1);
  CHECK(op.n == 8);
}

// ---- the dialog --------------------------------------------------------------------------------------------------------------------------

TEST_CASE("song tools view: FORM is the fifth OP with three rows (FORM rolls over, DRUMS stops at 1 and 8), the texts, the old four stay where they were") {
  app::SongToolsView v;
  CHECK(app::song_tools_rows(v) == 2u);
  v.tool = app::SongTool::Form;
  CHECK(app::song_tools_rows(v) == 3u);
  CHECK(static_cast<unsigned>(app::SongTool::Transpose) == 0u);
  CHECK(static_cast<unsigned>(app::SongTool::ExportWav) == 3u);
  CHECK(static_cast<unsigned>(app::SongTool::Form) == 4u);
  char text[24];
  (void)app::song_tools_text(v, app::SongToolsRow::Op, text, sizeof text);
  CHECK(std::string(text) == "FORM");
  CHECK(std::string(app::song_tools_caption(v, app::SongToolsRow::Value)) == "FORM");
  CHECK(std::string(app::song_tools_caption(v, app::SongToolsRow::Drums)) == "DRUMS");
  (void)app::song_tools_text(v, app::SongToolsRow::Value, text, sizeof text);
  CHECK(std::string(text) == "LOOP>SONG");
  CHECK(app::song_tools_step(v, app::SongToolsRow::Value, 1));
  (void)app::song_tools_text(v, app::SongToolsRow::Value, text, sizeof text);
  CHECK(std::string(text) == "POP");
  CHECK(app::song_tools_step(v, app::SongToolsRow::Value, 2));
  CHECK(v.form == SongForm::LoopSong);  // POP, TECHNO, round to LOOP>SONG
  CHECK(app::song_tools_step(v, app::SongToolsRow::Value, -1));
  CHECK(v.form == SongForm::Techno);
  (void)app::song_tools_text(v, app::SongToolsRow::Drums, text, sizeof text);
  CHECK(std::string(text) == "TRACKS 1-4");
  CHECK(v.drums == 4);
  CHECK(app::song_tools_step(v, app::SongToolsRow::Drums, 100));
  CHECK(v.drums == 8);
  CHECK_FALSE(app::song_tools_step(v, app::SongToolsRow::Drums, 1));  // stops at the end
  CHECK(app::song_tools_step(v, app::SongToolsRow::Drums, -100));
  CHECK(v.drums == 1);
  (void)app::song_tools_text(v, app::SongToolsRow::Drums, text, sizeof text);
  CHECK(std::string(text) == "TRACK 1");
  // the OP goes on from EXPORT WAV to FORM, BRIDGE (OS-SONG-BRIDGE) and rolls over to TRANSPOSE
  v.tool = app::SongTool::ExportWav;
  CHECK(app::song_tools_step(v, app::SongToolsRow::Op, 1));
  CHECK(v.tool == app::SongTool::Form);
  CHECK(app::song_tools_step(v, app::SongToolsRow::Op, 1));
  CHECK(v.tool == app::SongTool::Bridge);
  CHECK(app::song_tools_step(v, app::SongToolsRow::Op, 1));
  CHECK(v.tool == app::SongTool::Transpose);
}

TEST_CASE("song tools: the footer is the plan, made when the dialog opens and after a value changes; a refusal's text takes its place") {
  Rig rig({1, 2, 5, 3});
  rig.tap(Button::F6);
  while (rig.state().tools.tool != app::SongTool::Form) {
    rig.turn(1);
  }
  core::FixedString<48> footer;
  app::song_form_footer(rig.state().tools.plan, footer);
  CHECK(std::string(footer.c_str()) == "FROM 01  8 SLOTS  3 NEW PATTERNS");
  rig.tap(Button::F8);
  rig.shift_tap(Button::Down, 1);  // slots 0..1: patterns 1 and 2
  rig.tap(Button::F6);
  app::song_form_footer(rig.state().tools.plan, footer);
  CHECK(std::string(footer.c_str()) == "FROM 01 02  8 SLOTS  3 NEW PATTERNS");
  rig.tap(Button::Down);
  rig.turn(1);  // POP
  app::song_form_footer(rig.state().tools.plan, footer);
  CHECK(std::string(footer.c_str()) == "FROM 01 02  9 SLOTS  3 NEW PATTERNS");
  // slot 2 names pattern 5, which does not exist
  Rig rig2({1, 2, 5, 3});
  rig2.tap(Button::Down, 2);
  CHECK(rig2.song().pos() == 2);
  rig2.open_form(0);
  app::song_form_footer(rig2.state().tools.plan, footer);
  CHECK(std::string(footer.c_str()) == "SLOT 02 HAS NO PATTERN");
  // the longest line fits the 48 characters of the dialog
  app::SongBuildInfo info;
  info.slots = 10;
  info.copies = 4;
  info.source_count = 3;
  info.sources[0] = 255;
  info.sources[1] = 254;
  info.sources[2] = 253;
  app::song_form_footer(info, footer);
  CHECK(std::string(footer.c_str()) == "FROM 255 254 253  10 SLOTS  4 NEW PATTERNS");
  CHECK(footer.size() <= 43u);
}

// ---- BRIDGE (OS-SONG-BRIDGE) --------------------------------------------------------------------------------------------------------------

namespace {

bool same_step(const model::Step& a, const model::Step& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// Pattern `id` made of `length` steps on every track, every step full (`kind` 'A': notes 1..60, instrument 1, volume 10, FX 1:5 and 3:7; 'B': notes 61..120, instrument 2, volume 100, FX 2:9 and 4:1: a cell is
// one or the other whole) or empty ('-').
void fill(model::Project& p, unsigned id, unsigned length, char kind) {
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    NEED(p.set_track_length(id, t, length) == model::Status::Ok);
    for (unsigned i = 0; i < model::kMaxPatternLength; ++i) {
      model::Step s = model::kEmptyStep;
      if (kind != '-' && i < length) {
        const bool a = kind == 'A';
        s.note = static_cast<std::uint8_t>(a ? 1u + (t * 7u + i) % 60u : 61u + (t * 5u + i) % 60u);
        s.instr = a ? 1 : 2;
        s.vol = a ? 10 : 100;
        s.fx[0] = {static_cast<std::uint8_t>(a ? 1 : 2), static_cast<std::uint8_t>(a ? 5 : 9)};
        s.fx[1] = {static_cast<std::uint8_t>(a ? 3 : 4), static_cast<std::uint8_t>(a ? 7 : 1)};
      }
      NEED(p.set_step(id, t, i, s) == model::Status::Ok);
    }
  }
}

// F6, the OP BRIDGE and the SLOTS row `slots`; the dialog stays open.
void open_bridge(Rig& rig, unsigned slots) {
  rig.tap(Button::F6);
  while (rig.state().tools.tool != app::SongTool::Bridge) {
    rig.turn(1);
  }
  rig.tap(Button::Down);
  rig.turn(static_cast<int>(slots) - static_cast<int>(rig.state().tools.bridge_slots));
}

void bridge(Rig& rig, unsigned slots) {
  open_bridge(rig, slots);
  rig.tap(Button::F1);
}

app::SongBuildPlan bridge_plan(Rig& rig, int s0, int s1, unsigned slots) {
  app::SongBuildPlan out;
  app::song_bridge_plan(rig.project(), app::SongRect{s0, s1, 0, 15}, slots, out);
  return out;
}

// The cells of a bridge pattern that are B's (and differ from A's), A's, and whether every cell is one of the two whole.
struct Cells {
  unsigned from_b = 0;
  unsigned from_a = 0;
  bool whole = true;
};

Cells count_cells(Rig& rig, unsigned id, const model::Pattern& a, const model::Pattern& b) {
  Cells c;
  const model::Pattern* pat = rig.project().pattern(id);
  CHECK(pat != nullptr);
  if (pat == nullptr) {
    c.whole = false;
    return c;
  }
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    for (unsigned i = 0; i < pat->track_length[t]; ++i) {
      const model::Step& s = pat->steps[t][i];
      const model::Step& sa = a.steps[t][i % a.track_length[t]];
      const model::Step& sb = b.steps[t][i % b.track_length[t]];
      if (same_step(s, sb) && !same_step(sa, sb)) {
        ++c.from_b;
      } else if (same_step(s, sa)) {
        ++c.from_a;
      } else if (!same_step(s, sb)) {
        c.whole = false;
      }
    }
  }
  return c;
}

}  // namespace

TEST_CASE("song bridge: the hash, the phases and t16 are pinned (the seed is a contract); phases are the 16 multiples of 2048") {
  CHECK(app::kBridgeSeed == 0x0B91D6E5u);
  CHECK(app::song_bridge_hash(app::kBridgeSeed, 0) == 0x50E62CACu);
  CHECK(app::song_bridge_hash(app::kBridgeSeed, 1) == 0x993A9B2Cu);
  CHECK(app::song_bridge_hash(app::kBridgeSeed, 15) == 0xC0F916BFu);
  CHECK(app::song_bridge_hash(app::kBridgeSeed, 0, 0) == 0x3208C479u);
  CHECK(app::song_bridge_hash(app::kBridgeSeed, 3, 7) == 0x8050757Eu);
  CHECK(app::song_bridge_hash(app::kBridgeSeed, 15, 127) == 0x5D902989u);
  std::uint16_t phase[16];
  app::song_bridge_phases(app::kBridgeSeed, phase);
  const std::uint16_t expected[16] = {8192, 12288, 16384, 0, 28672, 6144, 26624, 2048, 24576, 18432, 14336, 22528, 4096, 10240, 30720, 20480};
  CHECK(std::memcmp(phase, expected, sizeof phase) == 0);
  unsigned seen = 0;
  for (unsigned t = 0; t < 16; ++t) {
    CHECK(phase[t] % 2048u == 0u);
    seen |= 1u << (phase[t] / 2048u);
  }
  CHECK(seen == 0xFFFFu);  // a permutation: every rank once
  CHECK(app::song_bridge_thr(app::kBridgeSeed, phase, 0, 0) == 14596u);
  CHECK(app::song_bridge_thr(app::kBridgeSeed, phase, 3, 7) == 16424u);
  CHECK(app::song_bridge_thr(app::kBridgeSeed, phase, 15, 127) == 32456u);
  // k / (N + 1) of the 65536 range
  CHECK(app::song_bridge_t16(1, 1) == 32768u);
  CHECK(app::song_bridge_t16(1, 4) == 13107u);
  CHECK(app::song_bridge_t16(2, 4) == 26214u);
  CHECK(app::song_bridge_t16(4, 4) == 52428u);
  CHECK(app::song_bridge_t16(16, 16) == 61680u);
  for (unsigned n = 1; n <= 16; ++n) {
    for (unsigned k = 2; k <= n; ++k) {
      CHECK(app::song_bridge_t16(k, n) > app::song_bridge_t16(k - 1, n));  // strictly rising
    }
  }
}

TEST_CASE("song bridge: the new slots go between A and B, the cursor lands on the first, the names are BRIDGE k/N, nothing else changes, the strip says what was made") {
  Rig rig({1, 2, 3}, 3, 32, 16);
  fill(rig.project(), 1, 32, 'A');
  fill(rig.project(), 2, 32, 'B');
  const model::Pattern a = *rig.project().pattern(1);
  const model::Pattern b = *rig.project().pattern(2);
  const model::Pattern c = *rig.project().pattern(3);
  bridge(rig, 4);
  CHECK(rig.last() == "BRIDGE: 4 SLOTS, 4 NEW PATTERNS");
  const Order o = rig.order();
  NEED(o.size() == 7u);
  CHECK(o[0] == 1u);
  CHECK(o[5] == 2u);
  CHECK(o[6] == 3u);
  CHECK(rig.song().pos() == 1);
  CHECK_FALSE(rig.song().selecting());
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.project().pattern_count() == 7u);
  const char* names[4] = {"BRIDGE 1/4", "BRIDGE 2/4", "BRIDGE 3/4", "BRIDGE 4/4"};
  for (unsigned k = 0; k < 4; ++k) {
    CHECK(o[1 + k] > 3u);
    CHECK(std::string(rig.project().pattern(o[1 + k])->name) == names[k]);
  }
  // the three sources are as they were
  CHECK(std::memcmp(rig.project().pattern(1), &a, sizeof a) == 0);
  CHECK(std::memcmp(rig.project().pattern(2), &b, sizeof b) == 0);
  CHECK(std::memcmp(rig.project().pattern(3), &c, sizeof c) == 0);
  // two slots selected: A is the first, B the second (slots 5 and 6 now: patterns 2 and 3)
  rig.tap(Button::Down, 4);
  rig.shift_tap(Button::Down, 1);
  CHECK(rig.song().first() == 5);
  CHECK(rig.song().last() == 6);
  const app::SongBuildPlan plan = bridge_plan(rig, 5, 6, 2);
  CHECK(plan.info.sources[0] == 2);
  CHECK(plan.info.sources[1] == 3);
  CHECK(plan.at == 6u);
  bridge(rig, 2);
  CHECK(rig.last() == "BRIDGE: 2 SLOTS, 2 NEW PATTERNS");
  CHECK(rig.order().size() == 9u);
  CHECK(rig.order()[5] == 2u);
  CHECK(rig.order()[8] == 3u);
  CHECK(rig.song().pos() == 6);
}

TEST_CASE("song bridge: the result is the same every time: two builds from the same project are the same bytes") {
  std::uint64_t first = 0;
  {
    Rig rig({1, 2}, 2, 32, 16);
    fill(rig.project(), 1, 32, 'A');
    fill(rig.project(), 2, 32, 'B');
    bridge(rig, 6);
    first = rig.snap().hash;
    rig.tap(Button::F8);  // and again on the same rig, after the undo
    rig.tap(Button::Up, 3);
    bridge(rig, 6);
    CHECK(rig.snap().hash == first);
  }
  Rig other({1, 2}, 2, 32, 16);
  fill(other.project(), 1, 32, 'A');
  fill(other.project(), 2, 32, 'B');
  bridge(other, 6);
  CHECK(other.snap().hash == first);
}

TEST_CASE("song bridge: k/(N+1) spreading: each cell is A's or B's step whole, a cell that took B's keeps it, the first slot is mostly A and the last mostly B, N = 1 is about half") {
  for (const unsigned n : {1u, 4u, 8u, 16u}) {
    // (a full 16-track pattern is 3.7 KB of undo for 16 steps: the longer bridges use shorter patterns to stay under the Song ring's cap)
    const unsigned len = n <= 4u ? 32u : (n == 8u ? 16u : 8u);
    Rig rig({1, 2}, 2, len, 16);
    fill(rig.project(), 1, len, 'A');
    fill(rig.project(), 2, len, 'B');
    const model::Pattern a = *rig.project().pattern(1);
    const model::Pattern b = *rig.project().pattern(2);
    bridge(rig, n);
    NEED(rig.order().size() == n + 2u);
    std::uint16_t phase[16];
    app::song_bridge_phases(app::kBridgeSeed, phase);
    unsigned previous = 0;
    std::vector<model::Pattern> slots;
    for (unsigned k = 1; k <= n; ++k) {
      const unsigned id = rig.order()[k];
      slots.push_back(*rig.project().pattern(id));
      const Cells cells = count_cells(rig, id, a, b);
      CHECK(cells.whole);
      CHECK(cells.from_a + cells.from_b == 16u * len);
      CHECK(cells.from_b >= previous);  // monotone
      previous = cells.from_b;
      // the cells are the ones the primitives say (the walk is wired to them)
      unsigned expected = 0;
      for (unsigned t = 0; t < 16; ++t) {
        for (unsigned i = 0; i < len; ++i) {
          expected += app::song_bridge_thr(app::kBridgeSeed, phase, t, i) < app::song_bridge_t16(k, n) ? 1u : 0u;
        }
      }
      CHECK(cells.from_b == expected);
      if (k == 1 && n >= 4) {
        CHECK(cells.from_b < 16u * len / 2u);  // mostly A
      }
      if (k == n && n >= 4) {
        CHECK(cells.from_b > 16u * len * 9u / 10u);  // mostly B
      }
      if (n == 1) {
        CHECK(cells.from_b > 16u * len * 35u / 100u);  // about half
        CHECK(cells.from_b < 16u * len * 65u / 100u);
      }
    }
    // a cell that took B's step at slot k is B's in every later slot
    for (unsigned k = 1; k < n; ++k) {
      for (unsigned t = 0; t < 16; ++t) {
        for (unsigned i = 0; i < len; ++i) {
          if (same_step(slots[k - 1].steps[t][i], b.steps[t][i])) {
            CHECK(same_step(slots[k].steps[t][i], b.steps[t][i]));
          }
        }
      }
    }
  }
}

TEST_CASE("song bridge: A and B of different lengths: the cells loop, the track length is A's up to the middle and B's after") {
  Rig rig({1, 2}, 2, 16, 16);
  fill(rig.project(), 1, 16, 'A');
  fill(rig.project(), 2, 24, 'B');
  const model::Pattern a = *rig.project().pattern(1);
  const model::Pattern b = *rig.project().pattern(2);
  bridge(rig, 4);  // t16 = 13107, 26214 (A's length), 39321, 52428 (B's)
  NEED(rig.order().size() == 6u);
  const unsigned want[4] = {16, 16, 24, 24};
  for (unsigned k = 1; k <= 4; ++k) {
    const model::Pattern* pat = rig.project().pattern(rig.order()[k]);
    NEED(pat != nullptr);
    for (unsigned t = 0; t < 16; ++t) {
      CHECK(pat->track_length[t] == want[k - 1]);
    }
    const Cells cells = count_cells(rig, rig.order()[k], a, b);
    CHECK(cells.whole);
    CHECK(cells.from_a + cells.from_b == 16u * want[k - 1]);
    for (unsigned i = want[k - 1]; i < model::kMaxPatternLength; ++i) {  // steps past the length are empty
      CHECK(empty_step_of(pat->steps[0][i]));
    }
  }
  // N = 1 sits at the middle exactly (t16 = 32768): A's length
  rig.tap(Button::F8);
  rig.tap(Button::Up, 4);
  bridge(rig, 1);
  CHECK(rig.project().pattern(rig.order()[1])->track_length[0] == 16u);
}

TEST_CASE("song bridge: tracks of A (or B) with a length of their own: each bridge track has A's own length up to the middle and B's own after, and its cells loop over that track's steps") {
  Rig rig({1, 2}, 2, 16, 16);
  fill(rig.project(), 1, 16, 'A');
  fill(rig.project(), 2, 24, 'B');
  NEED(rig.project().set_track_length(1, 3, 8) == model::Status::Ok);   // A's track 4 is 8 steps
  NEED(rig.project().set_track_length(1, 9, 5) == model::Status::Ok);   // A's track 10 is 5 steps
  NEED(rig.project().set_track_length(2, 5, 12) == model::Status::Ok);  // B's track 6 is 12 steps
  NEED(rig.project().set_track_length(2, 9, 7) == model::Status::Ok);   // B's track 10 is 7 steps
  const model::Pattern a = *rig.project().pattern(1);
  const model::Pattern b = *rig.project().pattern(2);
  bridge(rig, 4);  // t16 = 13107, 26214 (A's lengths), 39321, 52428 (B's)
  NEED(rig.order().size() == 6u);
  for (unsigned k = 1; k <= 4; ++k) {
    const model::Pattern* pat = rig.project().pattern(rig.order()[k]);
    NEED(pat != nullptr);
    for (unsigned t = 0; t < 16; ++t) {
      CHECK(pat->track_length[t] == (k <= 2 ? a.track_length[t] : b.track_length[t]));
    }
    const Cells cells = count_cells(rig, rig.order()[k], a, b);
    CHECK(cells.whole);
    unsigned total = 0;
    for (unsigned t = 0; t < 16; ++t) {
      total += pat->track_length[t];
      for (unsigned i = pat->track_length[t]; i < model::kMaxPatternLength; ++i) {  // nothing past a track's own length
        CHECK(empty_step_of(pat->steps[t][i]));
      }
    }
    CHECK(cells.from_a + cells.from_b == total);
  }
}

TEST_CASE("song bridge: the dialog's refresh plans without the cell walk (no bytes, the same counts and refusals as the full plan); F1 still plans in full") {
  Rig rig({1, 2, 3}, 3, 32, 16);
  fill(rig.project(), 1, 32, 'A');
  fill(rig.project(), 2, 32, 'B');
  const app::SongBuildPlan full = bridge_plan(rig, 0, 0, 6);
  NEED(full.info.refusal == SongBuildRefusal::None);
  CHECK(full.bytes > app::kSongUnitOverhead);
  app::SongBuildPlan light;
  app::song_bridge_plan(rig.project(), app::SongRect{0, 0, 0, 15}, 6, light, false);
  CHECK(light.info.refusal == SongBuildRefusal::None);
  CHECK(light.bytes == 0u);
  CHECK(light.info.slots == full.info.slots);
  CHECK(light.info.copies == full.info.copies);
  CHECK(light.at == full.at);
  for (unsigned k = 0; k < full.info.slots; ++k) {
    CHECK(light.slot_pattern[k] == full.slot_pattern[k]);
  }
  // a refusal comes out the same
  app::SongBuildPlan same;
  app::song_bridge_plan(rig.project(), app::SongRect{0, 2, 0, 15}, 6, same, false);
  CHECK(same.info.refusal == SongBuildRefusal::Select);
  // the dialog's own plan (song_tools_refresh) says the counts, and F1 builds the full plan
  open_bridge(rig, 6);
  CHECK(rig.state().tools.plan.refusal == SongBuildRefusal::None);
  CHECK(rig.state().tools.plan.slots == 6);
  rig.tap(Button::F1);
  CHECK(rig.last() == "BRIDGE: 6 SLOTS, 6 NEW PATTERNS");
  CHECK(rig.project().song_length() == 9u);
}

TEST_CASE("song bridge: an empty A fills in with B's notes and a full A thins out to an empty B, step by step") {
  for (const bool fill_in : {true, false}) {
    Rig rig({1, 2}, 2, 32, 16);
    fill(rig.project(), 1, 32, fill_in ? '-' : 'A');
    fill(rig.project(), 2, 32, fill_in ? 'B' : '-');
    bridge(rig, 6);
    NEED(rig.order().size() == 8u);
    unsigned previous = fill_in ? 0u : 16u * 32u;
    for (unsigned k = 1; k <= 6; ++k) {
      const model::Pattern* pat = rig.project().pattern(rig.order()[k]);
      unsigned sounding = 0;
      for (unsigned t = 0; t < 16; ++t) {
        for (unsigned i = 0; i < 32; ++i) {
          sounding += empty_step_of(pat->steps[t][i]) ? 0u : 1u;
        }
      }
      if (fill_in) {
        CHECK(sounding >= previous);
      } else {
        CHECK(sounding <= previous);
      }
      previous = sounding;
    }
    CHECK((fill_in ? previous > 16u * 32u / 2u : previous < 16u * 32u / 2u));
  }
}

TEST_CASE("song bridge: every refusal says its text, closes the dialog and writes nothing") {
  {  // A = B
    Rig rig({1, 1, 2});
    CHECK(bridge_plan(rig, 0, 0, 4).info.refusal == SongBuildRefusal::Same);
    const Rig::Snap before = rig.snap();
    bridge(rig, 4);
    CHECK(rig.last() == "SAME PATTERN IN BOTH SLOTS");
    CHECK(rig.state().overlay == app::OverlayId::None);
    CHECK(rig.snap() == before);
    CHECK(bridge_plan(rig, 1, 1, 4).info.refusal == SongBuildRefusal::None);  // slots 1 and 2: patterns 1 and 2
  }
  {  // the cursor on the last slot
    Rig rig({1, 2});
    rig.tap(Button::Down);
    CHECK(bridge_plan(rig, 1, 1, 4).info.refusal == SongBuildRefusal::NoNext);
    const Rig::Snap before = rig.snap();
    bridge(rig, 4);
    CHECK(rig.last() == "NO NEXT SLOT");
    CHECK(rig.snap() == before);
  }
  {  // three slots selected
    Rig rig({1, 2, 3});
    CHECK(bridge_plan(rig, 0, 2, 4).info.refusal == SongBuildRefusal::Select);
    rig.shift_tap(Button::Down, 2);
    const Rig::Snap before = rig.snap();
    bridge(rig, 4);
    CHECK(rig.last() == "SELECT 2 SLOTS");
    CHECK(rig.snap() == before);
  }
  {  // A names no pattern; B names none
    Rig rig({9, 2});
    const app::SongBuildPlan a_missing = bridge_plan(rig, 0, 0, 4);
    CHECK(a_missing.info.refusal == SongBuildRefusal::NoPattern);
    CHECK(a_missing.info.number == 0);
    const Rig::Snap before = rig.snap();
    bridge(rig, 4);
    CHECK(rig.last() == "SLOT 00 HAS NO PATTERN");
    CHECK(rig.snap() == before);
  }
  {
    Rig rig({1, 9});
    const Rig::Snap before = rig.snap();
    bridge(rig, 4);
    CHECK(rig.last() == "SLOT 01 HAS NO PATTERN");
    CHECK(rig.snap() == before);
  }
}

TEST_CASE("song bridge: the pre-check counts exactly: the song one slot short, the pool one short, the numbers, and a unit over half of the Song ring; each leaves the project alone") {
  {  // 248 slots (246 of them pattern 1 again): a bridge of 8 needs 8 and 7 are free
    Order o = {1, 2};
    for (unsigned k = 0; k < 246; ++k) {
      o.push_back(1);
    }
    Rig rig(o);
    CHECK(rig.project().song_length() == 248u);
    const app::SongBuildPlan plan = bridge_plan(rig, 0, 0, 8);
    CHECK(plan.info.refusal == SongBuildRefusal::SongFull);
    CHECK(plan.info.number == 8);
    CHECK(plan.info.have == 7);
    const Rig::Snap before = rig.snap();
    bridge(rig, 8);
    CHECK(rig.last() == "SONG FULL: NEEDS 8 SLOTS, 7 FREE");
    CHECK(rig.snap() == before);
    CHECK(bridge_plan(rig, 0, 0, 7).info.refusal == SongBuildRefusal::None);  // 7 fit exactly (255)
  }
  {  // the pool: 126 patterns, 4 needed, 2 left
    Rig rig({1, 2}, 126);
    const app::SongBuildPlan plan = bridge_plan(rig, 0, 0, 4);
    CHECK(plan.info.refusal == SongBuildRefusal::NoRoom);
    CHECK(plan.info.number == 4);
    CHECK(plan.info.have == 2);
    const Rig::Snap before = rig.snap();
    bridge(rig, 4);
    CHECK(rig.last() == "NEEDS 4 FREE PATTERNS, 2 FREE");
    CHECK(rig.snap() == before);
    bridge(rig, 1);  // one pattern: the singular, and it fits
    CHECK(rig.last() == "BRIDGE: 1 SLOT, 1 NEW PATTERN");
  }
  {  // the numbers: 7 patterns and 246 slots that name ids 8..253 (none exists): 254 and 255 are free
    Order o = {1, 2};
    for (unsigned id = 8; id <= 253; ++id) {
      o.push_back(id);
    }
    Rig rig(o, 7);
    const app::SongBuildPlan plan = bridge_plan(rig, 0, 0, 3);
    CHECK(plan.info.refusal == SongBuildRefusal::NoRoom);
    CHECK(plan.info.number == 3);
    CHECK(plan.info.have == 2);
    CHECK(plan.free_numbers == 2u);
    const Rig::Snap before = rig.snap();
    bridge(rig, 3);
    CHECK(rig.last() == "NEEDS 3 FREE PATTERNS, 2 FREE");
    CHECK(rig.snap() == before);
  }
  {  // a slot that names a missing pattern holds its number back: 4 and 5 are named, the new patterns are 3, 6, 7
    Rig rig({1, 2, 4, 5}, 2);
    const app::SongBuildPlan plan = bridge_plan(rig, 0, 0, 3);
    NEED(plan.info.refusal == SongBuildRefusal::None);
    CHECK(plan.copy[0].to == 3);
    CHECK(plan.copy[1].to == 6);
    CHECK(plan.copy[2].to == 7);
  }
  {  // the undo ring: two dense 128-step patterns make a unit of about 29 KB a pattern
    Rig rig({1, 2}, 2, 128, 16);
    fill(rig.project(), 1, 128, 'A');
    fill(rig.project(), 2, 128, 'B');
    const app::SongBuildPlan plan = bridge_plan(rig, 0, 0, 4);
    NEED(plan.info.refusal == SongBuildRefusal::None);
    CHECK(plan.bytes > app::song_undo_limit(rig.history.ring[1].capacity_bytes()));
    const Rig::Snap before = rig.snap();
    bridge(rig, 4);
    CHECK(rig.last() == "TOO BIG TO UNDO: SELECT LESS");
    CHECK(rig.snap() == before);
  }
}

TEST_CASE("song bridge: the planner's bytes are what the editor writes, one undo takes slots and patterns back together, redo restores them") {
  Rig rig({1, 2, 3}, 3, 32, 16);
  fill(rig.project(), 1, 32, 'A');
  fill(rig.project(), 2, 24, 'B');
  rig.editor->begin_group(core::action_id("step.note"), 0, UndoArea::Pattern);
  model::Step edited = model::kEmptyStep;
  edited.note = 99;
  NEED(rig.editor->set_step(3, 0, 5, edited) == model::Status::Ok);
  rig.editor->end_group();
  const Rig::Snap before = rig.snap();
  const unsigned depth = rig.song_depth();
  const app::SongBuildPlan plan = bridge_plan(rig, 0, 0, 5);
  NEED(plan.info.refusal == SongBuildRefusal::None);
  bridge(rig, 5);
  CHECK(rig.last() == "BRIDGE: 5 SLOTS, 5 NEW PATTERNS");
  const Rig::Snap built = rig.snap();
  CHECK(built.bytes - before.bytes == plan.bytes);  // the planner counts what the editor writes
  CHECK(rig.song_depth() == depth + 1u);
  CHECK(rig.project().song_length() == 8u);
  CHECK(rig.project().pattern_count() == 8u);
  rig.tap(Button::F8);
  CHECK(rig.order() == Order{1, 2, 3});
  CHECK(rig.project().pattern_count() == 3u);
  CHECK(rig.song_depth() == depth);
  CHECK(rig.snap().hash == before.hash);
  CHECK(rig.last() == "PATTERN HISTORY CLEARED");
  rig.shift_tap(Button::F8);
  CHECK(rig.snap().hash == built.hash);
  CHECK(rig.project().song_length() == 8u);
  CHECK(rig.project().pattern_count() == 8u);
}

TEST_CASE("song bridge: it works while the transport runs: one change of the order list, the new slots between A and B") {
  Rig rig({1, 2, 3});
  rig.position.publish(true, 1, 0, 12000, 0);
  rig.pass(1);
  CHECK(rig.state().play.playing);
  model::OrderSnapshot before{};
  model::OrderSnapshot after{};
  NEED(rig.project().snapshot_order(before));
  bridge(rig, 4);  // between slot 0 and the playing slot 1
  CHECK(rig.last() == "BRIDGE: 4 SLOTS, 4 NEW PATTERNS");
  CHECK(rig.state().play.playing);
  NEED(rig.project().snapshot_order(after));
  CHECK(after.ops - before.ops == 1u);  // the 4 inserts are one op (an OrderBatch)
  const model::OrderOp op = after.journal[(after.ops - 1u) % model::kOrderJournal];
  CHECK(op.kind == model::OrderOp::Insert);
  CHECK(op.a == 1);
  CHECK(op.n == 4);
}

TEST_CASE("song tools view: BRIDGE is the sixth OP with two rows (SLOTS 1..16, Shift steps 4), and the footer is the plan") {
  app::SongToolsView v;
  v.tool = app::SongTool::Bridge;
  CHECK(static_cast<unsigned>(app::SongTool::Bridge) == 5u);
  CHECK(app::song_tools_rows(v) == 2u);
  CHECK(std::string(app::song_tools_caption(v, app::SongToolsRow::Value)) == "SLOTS");
  CHECK(app::song_tools_desc(v, app::SongToolsRow::Value).coarse == 4);
  char text[24];
  (void)app::song_tools_text(v, app::SongToolsRow::Op, text, sizeof text);
  CHECK(std::string(text) == "BRIDGE");
  (void)app::song_tools_text(v, app::SongToolsRow::Value, text, sizeof text);
  CHECK(std::string(text) == "4");
  CHECK(app::song_tools_step(v, app::SongToolsRow::Value, 100));
  CHECK(v.bridge_slots == 16);
  CHECK_FALSE(app::song_tools_step(v, app::SongToolsRow::Value, 1));
  CHECK(app::song_tools_step(v, app::SongToolsRow::Value, -100));
  CHECK(v.bridge_slots == 1);
  // the OP rolls over the six values
  CHECK(app::song_tools_step(v, app::SongToolsRow::Op, 1));
  CHECK(v.tool == app::SongTool::Transpose);
  CHECK(app::song_tools_step(v, app::SongToolsRow::Op, -1));
  CHECK(v.tool == app::SongTool::Bridge);

  Rig rig({1, 2, 5, 3});
  open_bridge(rig, 4);
  core::FixedString<48> footer;
  app::song_bridge_footer(rig.state().tools.plan, footer);
  CHECK(std::string(footer.c_str()) == "PATTERN 01 TO 02  4 SLOTS  4 NEW PATTERNS");
  rig.turn(-3);  // SLOTS 1
  app::song_bridge_footer(rig.state().tools.plan, footer);
  CHECK(std::string(footer.c_str()) == "PATTERN 01 TO 02  1 SLOT  1 NEW PATTERN");
  rig.tap(Button::F8);
  rig.tap(Button::Down);  // slot 1 (pattern 2) and slot 2 (pattern 5, which does not exist)
  rig.tap(Button::F6);
  app::song_bridge_footer(rig.state().tools.plan, footer);
  CHECK(std::string(footer.c_str()) == "SLOT 02 HAS NO PATTERN");
  // the longest line
  app::SongBuildInfo info;
  info.slots = 16;
  info.copies = 16;
  info.source_count = 2;
  info.sources[0] = 255;
  info.sources[1] = 254;
  app::song_bridge_footer(info, footer);
  CHECK(std::string(footer.c_str()) == "PATTERN 255 TO 254  16 SLOTS  16 NEW PATTERNS");
  CHECK(footer.size() <= 48u);
  info.refusal = SongBuildRefusal::Same;
  app::song_bridge_footer(info, footer);
  CHECK(std::string(footer.c_str()) == "SAME PATTERN IN BOTH SLOTS");
}
