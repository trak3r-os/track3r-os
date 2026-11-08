// SPDX-License-Identifier: GPL-3.0-or-later
// OS-PERSIST2: the runtime side of the session state (app/project_state.h, seq/perform.h): the note-entry settings and JUMP, the tracks' MUTE MODEs, the send effects' controls and the Perform
// scenes with LATCH, copied into the project when they change and back (with the engine told) when a project is loaded. The format side is persist2_format_test.cpp.
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/mem_card.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/native_load.h>
#include <o3os/app/note_entry.h>
#include <o3os/app/pattern_edit.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/project_state.h>
#include <o3os/app/send_fx_rows.h>
#include <o3os/app/track_state.h>
#include <o3os/audio/graph_params.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/seq/perform.h>
#include <o3os/sim/demo_project.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
namespace gp = audio::graph_param;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

struct Sink final : seq::CommandSink {
  std::vector<seq::Command> got;
  bool push(const seq::Command& c) noexcept override {
    got.push_back(c);
    return true;
  }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  core::ParamRing ring;
  std::unique_ptr<app::MixBoard> board = std::make_unique<app::MixBoard>();
  std::unique_ptr<app::PerformBoard> perform = std::make_unique<app::PerformBoard>();

  Rig() {
    model::Project& p = test::arena(1).project;
    sim::fill_demo_project(p);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_param_ring(&ring);
    app->attach_mix(board.get());
    app->attach_perform(perform.get());
    app->run_once();
    (void)sent();
  }
  model::Project& project() { return test::arena(1).project; }
  const app::ViewState& state() const { return app->state(); }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  std::vector<core::ParamChange> sent() {
    std::vector<core::ParamChange> out;
    core::ParamChange c;
    while (ring.pop(c)) {
      out.push_back(c);
    }
    return out;
  }
  // The note-entry settings the way the musician sets them: Note held, then the panel's pads (row 1 ROOT, 2 SCALE, 3 JUMP, 4 OCTAVE and LOCK).
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void tap_pad(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 100));
    pass(1);
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 0));
    pass(1);
  }
  void panel(unsigned root, unsigned scale, unsigned jump, unsigned octave, unsigned lock) {
    press(Button::Note);
    pass(core::kGestureTimings.hold_ms + 1);
    tap_pad(root);
    tap_pad(12 + scale);
    tap_pad(24 + jump);
    tap_pad(36 + octave);
    tap_pad(36 + 10 + lock);
    release(Button::Note);
    pass(1);
  }
};

bool has(const std::vector<core::ParamChange>& v, core::ParamId id, float value) {
  for (const core::ParamChange& c : v) {
    if (c.id == id && c.value == value) {
      return true;
    }
  }
  return false;
}

constexpr std::uint8_t kSession = model::Project::kStateNote | model::Project::kStateMute | model::Project::kStateScenes | model::Project::kStateSendFx;

}  // namespace

TEST_CASE("persist2 (app): the model's ranges and defaults are the runtime's") {
  // note entry
  app::PatternEdit edit;
  model::NoteState def;
  model::init_note_state(def);
  CHECK(app::note_state_of(edit) == def);  // the defaults of the runtime (NoteEntry, edit_step) are the model's defaults
  CHECK(def.jump == edit.edit_step);
  CHECK(def.octave == app::NoteEntry{}.octave);
  CHECK(model::kNoteScaleMax == 38u);  // OS-SCALES28: the official numbering, in the model and in the runtime
  CHECK(sizeof model::kNotePanelScales == app::kNoteScales);  // the panel's twelve are the model's map
  CHECK(model::kNoteOctaveMax + 1u == app::kEntryOctaves);
  // mute modes
  CHECK(model::kMuteModeMax + 1u == app::kMuteModes);
  // the send effects' rows
  CHECK(model::kSendFxCount == app::kSendFxRows);
  app::MixBoard board;
  model::SendFxState fx;
  model::init_send_fx_state(fx);
  CHECK(app::send_fx_state_of(board) == fx);  // a board that was never touched is the model's defaults
  for (unsigned i = 0; i < app::kSendFxRows; ++i) {
    CHECK(app::kSendFxTable[i].min == model::kSendFxMin[i]);
    CHECK(app::kSendFxTable[i].max == model::kSendFxMax[i]);
    CHECK(app::kSendFxTable[i].def == model::kSendFxDefault[i]);
  }
  model::MuteState mute;
  model::init_mute_state(mute);
  CHECK(app::mute_state_of(board) == mute);
  // the Perform bank
  seq::PerformBank bank;
  model::ScenesState scenes;
  model::init_scenes_state(scenes);
  CHECK(bank.scenes_state() == scenes);
  CHECK(model::kSceneCount == seq::kPerfScenes);
}

TEST_CASE("persist2 (app): note_apply puts the settings and JUMP in and lays the pad board out again, and what is stored is always valid") {
  app::PatternEdit edit;
  model::NoteState s;
  model::init_note_state(s);
  s.root = 7;
  s.scale = 15;  // Blues, the official index (OS-SCALES28)
  s.octave = 5;
  s.lock = 1;
  s.jump = 12;
  CHECK(app::note_apply(edit, s));
  CHECK(edit.entry.root == 7);
  CHECK(edit.entry.scale == 15);
  CHECK(edit.entry.octave == 5);
  CHECK(edit.entry.lock == 1);
  CHECK(edit.edit_step == 12);
  CHECK(app::note_state_of(edit) == s);
  // the NOTES board follows the settings (with SCALE LOCK on the scale, from the octave's root note)
  CHECK(edit.pads.scale() == edit.entry.core_scale());
  CHECK(edit.pads.root() == edit.entry.board_root());
  CHECK_FALSE(app::note_apply(edit, s));  // nothing changed
  s.lock = 0;
  CHECK(app::note_apply(edit, s));
  CHECK(edit.pads.scale() == 1);  // the chromatic layout while the lock is off
  // a runtime that somehow holds a value the format does not (the F3 chooser's range is wider elsewhere) is stored as its default: the part is never refused
  edit.entry.root = 200;
  edit.edit_step = 99;
  const model::NoteState read = app::note_state_of(edit);
  CHECK(model::validate_note_state(read) == nullptr);
  CHECK(read.root == 0);
  CHECK(read.jump == 1);
  CHECK(read.scale == 15);
}

TEST_CASE("persist2 (app): mute_apply and send_fx_apply: the board follows, the engine is told only what changed, as the page's own parameter change") {
  app::MixBoard board;
  core::ParamRing ring;
  model::MuteState m;
  model::init_mute_state(m);
  m.mode[2] = 1;
  m.mode[9] = 2;
  CHECK(app::mute_apply(board, m) == 2u);
  CHECK(board.mute_mode[2] == 1);
  CHECK(board.mute_mode[9] == 2);
  CHECK(app::mute_state_of(board) == m);
  CHECK(app::mute_apply(board, m) == 0u);
  core::ParamChange c;
  CHECK_FALSE(ring.pop(c));  // a mute mode tells nobody: it says when a later toggle takes effect

  model::SendFxState f;
  model::init_send_fx_state(f);
  CHECK(app::send_fx_apply(board, f, &ring) == 0u);  // already there
  CHECK_FALSE(ring.pop(c));
  f.value[3] = 1200;  // DLY TIME
  f.value[5] = 40;    // DLY TONE
  f.value[6] = 1;     // DLY MODE
  f.value[11] = 1;    // RVB QUALITY
  CHECK(app::send_fx_apply(board, f, &ring) == 4u);
  CHECK(app::send_fx_state_of(board) == f);
  std::vector<core::ParamChange> sent;
  while (ring.pop(c)) {
    sent.push_back(c);
    CHECK(c.ramp_samples == 0u);  // the effects' own smoothing
  }
  CHECK(sent.size() == 4u);
  CHECK(has(sent, audio::master_param(gp::master::DelayTime), 1200.0f));
  CHECK(has(sent, audio::master_param(gp::master::DelayTone), 40.0f));
  CHECK(has(sent, audio::master_param(gp::master::DelayMode), 1.0f));
  CHECK(has(sent, audio::master_param(gp::master::ReverbQuality), 1.0f));
  CHECK(app::send_fx_apply(board, f, &ring) == 0u);
  // the same ids as the page uses: every row goes to its own field (the rows that were already at their top are not repeated)
  model::SendFxState all = f;
  unsigned differ = 0;
  for (unsigned i = 0; i < app::kSendFxRows; ++i) {
    all.value[i] = model::kSendFxMax[i];
    differ += all.value[i] != f.value[i] ? 1u : 0u;
  }
  CHECK(app::send_fx_apply(board, all, &ring) == differ);
  sent.clear();
  while (ring.pop(c)) {
    sent.push_back(c);
  }
  CHECK(sent.size() == differ);
  for (unsigned i = 0; i < app::kSendFxRows; ++i) {
    CHECK(has(sent, audio::master_param(app::kSendFxTable[i].field), static_cast<float>(model::kSendFxMax[i])) == (all.value[i] != f.value[i]));
  }
  CHECK(app::send_fx_apply(board, all, nullptr) == 0u);
  CHECK(app::send_fx_apply(board, f, nullptr) > 0u);  // without a ring the board still follows
  CHECK(app::send_fx_state_of(board) == f);
}

TEST_CASE("persist2 (app): the Perform bank's scenes and LATCH are the project's: scenes_state, load_scenes after load_slots, and recalling a loaded scene") {
  seq::PerformBank a;
  Sink sink;
  model::PerfState slots;
  model::init_perf_state(slots);
  slots.fx[0] = static_cast<std::uint8_t>(seq::PerfFx::Tune);
  slots.row[0] = 2;
  slots.value[0][1] = -24;
  a.load_slots(slots);
  CHECK(a.store_scene(1));
  a.set_latch(false);
  const model::ScenesState s = a.scenes_state();
  CHECK(s.latch == 0);
  CHECK(s.scene[0].stored == 0);
  CHECK(s.scene[1].stored == 1);
  CHECK(s.scene[1].slots == a.state());
  CHECK(s.scene[1].slots.value[0][1] == -24);
  CHECK(model::validate_scenes_state(s) == nullptr);

  // a project is loaded: the slots first (which clears the scenes), then the scenes
  seq::PerformBank b;
  model::PerfState other;
  model::init_perf_state(other);
  b.load_slots(other);
  CHECK_FALSE(b.scene_stored(1));
  b.load_scenes(s);
  CHECK(b.scene_stored(1));
  CHECK_FALSE(b.scene_stored(0));
  CHECK_FALSE(b.latch());
  CHECK(b.scenes_state() == s);
  CHECK_FALSE(b.scene_matches(1));  // the slots are the project's, not the scene's
  CHECK(b.recall_scene(1, sink));
  CHECK(b.fx(0) == seq::PerfFx::Tune);
  CHECK(b.row(0) == 2);
  CHECK(b.stored(0, 2) == -24);
  CHECK(b.scene_matches(1));
  // loading the slots again clears the scenes (a project that has none), and load_scenes of the defaults is "no scene, LATCH on"
  b.load_slots(other);
  CHECK_FALSE(b.scene_stored(1));
  model::ScenesState def;
  model::init_scenes_state(def);
  b.load_scenes(def);
  CHECK(b.latch());
  CHECK(b.scenes_state() == def);
  // a scene with a value past its FX's range (a project written by a later version, or by hand) loads as it is and is held to the range when recalled
  model::ScenesState wide = s;
  wide.scene[1].slots.value[0][1] = 100;  // Tune is +-48
  b.load_slots(other);
  b.load_scenes(wide);
  CHECK(b.scenes_state() == wide);
  CHECK(b.recall_scene(1, sink));
  CHECK(b.stored(0, 2) == 48);
}

TEST_CASE("persist2 (app): a change of the mute modes, the send controls, the note entry or the scenes is the project's at the next pass, keeps the undo history, and writes nothing twice") {
  Rig rig;
  model::Project& p = rig.project();
  CHECK((p.state_present() & kSession) == 0);  // the boot state is not a change
  CHECK(rig.editor->set_name("Song", 4) == model::Status::Ok);
  CHECK(rig.editor->can_undo());
  const unsigned depth = rig.journal.depth();

  rig.board->mute_mode[3] = 2;
  rig.board->mute_mode[15] = 1;
  rig.board->fx[3] = 750;   // DLY TIME
  rig.board->fx[5] = 25;    // DLY TONE
  rig.board->fx[11] = 1;    // RVB QUALITY
  seq::PerformBank& bank = rig.perform->bank;
  CHECK(bank.store_scene(2));
  bank.set_latch(false);
  rig.panel(4, 2, 5, 6, 1);  // E, Minor (pad 3 = the official 1), JUMP 5, OCTAVE 6, LOCK on
  CHECK(rig.state().edit.entry.root == 4);
  CHECK(rig.state().edit.entry.scale == 1);
  CHECK(rig.state().edit.entry.octave == 6);
  CHECK(rig.state().edit.entry.lock == 1);
  CHECK(rig.state().edit.edit_step == 5);
  rig.pass();
  CHECK((p.state_present() & kSession) == kSession);
  CHECK(p.mute().mode[3] == 2);
  CHECK(p.mute().mode[15] == 1);
  CHECK(p.mute().mode[0] == 0);
  CHECK(p.send_fx().value[3] == 750);
  CHECK(p.send_fx().value[5] == 25);
  CHECK(p.send_fx().value[11] == 1);
  CHECK(p.send_fx().value[0] == 0);
  CHECK(p.scenes().latch == 0);
  CHECK(p.scenes().scene[2].stored == 1);
  CHECK(p.scenes().scene[0].stored == 0);
  CHECK(p.scenes().scene[2].slots == bank.state());
  CHECK(p.note().root == 4);
  CHECK(p.note().scale == 1);
  CHECK(p.note().octave == 6);
  CHECK(p.note().lock == 1);
  CHECK(p.note().jump == 5);
  CHECK(rig.editor->can_undo());  // not an edit behind the editor's back
  CHECK(rig.editor->in_sync());
  CHECK(rig.journal.depth() == depth);  // and not an undo unit
  const std::uint32_t generation = p.generation();
  rig.pass();
  CHECK(p.generation() == generation);  // nothing changed: nothing written
  CHECK(rig.sent().empty());            // and the copy into the project tells the engine nothing

  // one more change is one more write, of that part only
  rig.board->mute_mode[3] = 0;
  rig.pass();
  CHECK(p.generation() != generation);
  CHECK(p.mute().mode[3] == 0);
  CHECK(p.send_fx().value[3] == 750);
}

TEST_CASE("persist2 (app): a project that is loaded sets the runtime, tells the engine, and a project that has none sets the defaults: nothing leaks from the one before") {
  Rig rig;
  model::Project& p = rig.project();
  seq::PerformBank& bank = rig.perform->bank;

  // the project has the four parts
  p.reset();
  model::NoteState n;
  model::init_note_state(n);
  n.root = 9;
  n.scale = 23;  // Whole tone, the official index (OS-SCALES28); the twelfth pad of row 2
  n.octave = 2;
  n.lock = 1;
  n.jump = 32;
  model::MuteState m;
  model::init_mute_state(m);
  m.mode[0] = 2;
  m.mode[7] = 1;
  model::ScenesState sc;
  model::init_scenes_state(sc);
  sc.latch = 0;
  sc.scene[0].stored = 1;
  model::init_perf_state(sc.scene[0].slots);
  sc.scene[0].slots.row[1] = 3;
  sc.scene[0].slots.value[1][2] = -60;
  model::SendFxState f;
  model::init_send_fx_state(f);
  f.value[1] = 1;     // DLY SYNC
  f.value[2] = 9;     // DLY RATE
  f.value[7] = 90;    // RVB SIZE
  f.value[11] = 1;    // RVB QUALITY
  CHECK(p.set_note(n) == model::Status::Ok);
  CHECK(p.set_mute(m) == model::Status::Ok);
  CHECK(p.set_scenes(sc) == model::Status::Ok);
  CHECK(p.set_send_fx(f) == model::Status::Ok);
  rig.app->document_replaced();
  CHECK(rig.board->mute_mode[0] == 2);
  CHECK(rig.board->mute_mode[7] == 1);
  CHECK(rig.board->mute_mode[1] == 0);
  CHECK(app::send_fx_state_of(*rig.board) == f);
  CHECK(bank.scene_stored(0));
  CHECK_FALSE(bank.scene_stored(1));
  CHECK_FALSE(bank.latch());
  CHECK(bank.scenes_state() == sc);
  CHECK(rig.state().edit.entry.root == 9);
  CHECK(rig.state().edit.entry.scale == 23);
  CHECK(rig.state().edit.entry.octave == 2);
  CHECK(rig.state().edit.entry.lock == 1);
  CHECK(rig.state().edit.edit_step == 32);
  CHECK(rig.state().edit.pads.root() == rig.state().edit.entry.board_root());  // the NOTES board was laid out again
  const std::vector<core::ParamChange> sent = rig.sent();
  CHECK(has(sent, audio::master_param(gp::master::DelaySync), 1.0f));
  CHECK(has(sent, audio::master_param(gp::master::DelayRate), 9.0f));
  CHECK(has(sent, audio::master_param(gp::master::ReverbSize), 90.0f));
  CHECK(has(sent, audio::master_param(gp::master::ReverbQuality), 1.0f));
  // only what differed from the board: the others (still at the effects' own defaults) are not repeated
  CHECK_FALSE(has(sent, audio::master_param(gp::master::DelayTime), 500.0f));
  // loading is not a change: the project is not written to by the pass that follows, so it is not unsaved
  const std::uint32_t generation = p.generation();
  rig.pass();
  CHECK(p.generation() == generation);
  CHECK(rig.sent().empty());
  CHECK(p.scenes() == sc);

  // the next project says nothing: the defaults, and the engine is told what changed back
  p.reset();
  rig.app->document_replaced();
  CHECK(rig.board->mute_mode[0] == 0);
  CHECK(rig.board->mute_mode[7] == 0);
  model::SendFxState def;
  model::init_send_fx_state(def);
  CHECK(app::send_fx_state_of(*rig.board) == def);
  CHECK_FALSE(bank.scene_stored(0));
  CHECK(bank.latch());
  CHECK(rig.state().edit.entry.root == 0);
  CHECK(rig.state().edit.entry.scale == 0);
  CHECK(rig.state().edit.entry.octave == 3);
  CHECK(rig.state().edit.entry.lock == 0);
  CHECK(rig.state().edit.edit_step == 1);
  const std::vector<core::ParamChange> back = rig.sent();
  CHECK(has(back, audio::master_param(gp::master::DelaySync), 0.0f));
  CHECK(has(back, audio::master_param(gp::master::DelayRate), 0.0f));
  CHECK(has(back, audio::master_param(gp::master::ReverbSize), 50.0f));
  CHECK(has(back, audio::master_param(gp::master::ReverbQuality), 0.0f));
  const std::uint32_t g2 = p.generation();
  rig.pass();
  CHECK(p.generation() == g2);
  CHECK((p.state_present() & kSession) == 0);  // nothing was written: the project that said nothing still says nothing
}

TEST_CASE("persist2 (app): the whole way: the runtime is saved with the project, another project is loaded over it, the first is loaded again, and the runtime is as it was") {
  Rig rig;
  model::Project& p = rig.project();
  rig.board->mute_mode[5] = 1;
  rig.board->fx[4] = 30;     // DLY FEEDBACK
  rig.board->fx[6] = 1;      // DLY MODE STEREO
  rig.perform->bank.store_scene(0);
  rig.perform->bank.set_latch(false);
  rig.panel(11, 5, 2, 8, 1);  // B, Lydian (pad 6 = the official 4), JUMP 2, OCTAVE 8, LOCK on
  rig.pass();
  std::vector<std::uint8_t> file;
  NEED(test::save(p, file) == formats::native::Progress::Done);
  const model::ScenesState scenes = p.scenes();

  // another project (none of the parts) is loaded: the defaults
  p.reset();
  rig.app->document_replaced();
  CHECK(rig.board->mute_mode[5] == 0);
  CHECK(rig.board->fx[4] == 50);
  CHECK(rig.board->fx[6] == 0);
  CHECK_FALSE(rig.perform->bank.scene_stored(0));
  CHECK(rig.perform->bank.latch());
  CHECK(rig.state().edit.entry.root == 0);
  (void)rig.sent();
  // and the saved one again, through the reader
  formats::native::Report rep;
  NEED(test::load(test::arena(1), file, &rep) == formats::native::Progress::Done);
  rig.app->document_replaced();
  CHECK(rig.board->mute_mode[5] == 1);
  CHECK(rig.board->fx[4] == 30);
  CHECK(rig.board->fx[6] == 1);
  CHECK(rig.perform->bank.scenes_state() == scenes);
  CHECK_FALSE(rig.perform->bank.latch());
  CHECK(rig.state().edit.entry.root == 11);
  CHECK(rig.state().edit.entry.scale == 4);
  CHECK(rig.state().edit.edit_step == 2);
  CHECK(rig.state().edit.entry.octave == 8);
  CHECK(rig.state().edit.entry.lock == 1);
  const std::vector<core::ParamChange> sent = rig.sent();
  CHECK(has(sent, audio::master_param(gp::master::DelayFeedback), 30.0f));
  CHECK(has(sent, audio::master_param(gp::master::DelayMode), 1.0f));
  const std::uint32_t generation = p.generation();
  rig.pass();
  CHECK(p.generation() == generation);
  std::vector<std::uint8_t> again;
  NEED(test::save(p, again) == formats::native::Progress::Done);
  CHECK(again == file);  // what the load put in the runtime is what the next save writes
}

TEST_CASE("persist2 (app): a project folder with the session chunks loads from the card, and the load line says how many values were reset") {
  using o3os::test::MemCard;
  test::fill_persist2(test::arena(0).project);
  std::vector<std::uint8_t> clean;
  NEED(test::save(test::arena(0).project, clean) == formats::native::Progress::Done);
  // two values out of range in NOTE (the CRC made right)
  std::vector<test::RawChunk> cs = test::split_chunks(clean);
  for (test::RawChunk& c : cs) {
    if (c.id == formats::native::kChunkNote) {
      c.payload[2] = 99;
      c.payload[6] = 99;
    }
  }
  const std::vector<std::uint8_t> damaged = test::build_file(cs, {});
  struct Result {
    std::uint32_t sanitised;
    std::string line;
  };
  const auto load = [](const std::vector<std::uint8_t>& file) {
    Result r{0xFFFFFFFFu, ""};
    MemCard card;
    card.put("Projects/Song/project.o3pj", file);
    model::ModelArena& target = test::arena(1);
    target.project.reset();
    app::NativeFolderLoader loader;
    if (!loader.begin(card, "Projects/Song", target)) {
      return r;
    }
    while (loader.step(4096) == app::NativeFolderLoader::State::Running) {
    }
    char line[128];
    (void)app::describe_native_load(loader, line, sizeof line);
    r.sanitised = loader.report().sanitised;
    r.line = line;
    return r;
  };
  const Result ok = load(clean);
  CHECK(ok.sanitised == 0u);
  CHECK(ok.line.find("reset") == std::string::npos);
  CHECK(test::arena(1).project.send_fx().value[3] == 3500);
  const Result bad = load(damaged);
  CHECK(bad.sanitised == 2u);
  CHECK(bad.line.find(", 2 values reset") != std::string::npos);
  CHECK(test::arena(1).project.note().root == 0);  // 99: its default
  CHECK(test::arena(1).project.note().scale == 15);  // Blues, the official index (the 1.0 byte was 9)
  // one value
  for (test::RawChunk& c : cs) {
    if (c.id == formats::native::kChunkNote) {
      c.payload[2] = 7;
    }
  }
  const Result one = load(test::build_file(cs, {}));
  CHECK(one.sanitised == 1u);
  CHECK(one.line.find(", 1 value reset") != std::string::npos);
}

TEST_CASE("OS-SCALES28 (app): a NOTE 1.1 project sets the entry and lays the NOTES pads out for its scale, and a row-2 pad writes 1.0 again") {
  Rig rig;
  model::Project& p = rig.project();
  test::fill_scales(test::arena(0).project);  // Hirajoshi (the official 26), SCALE LOCK on: a NOTE 1.1 chunk
  std::vector<std::uint8_t> file;
  NEED(test::save(test::arena(0).project, file) == formats::native::Progress::Done);
  {
    const std::vector<test::RawChunk> cs = test::split_chunks(file);
    const test::RawChunk* note = nullptr;
    for (const test::RawChunk& c : cs) {
      if (c.id == formats::native::kChunkNote) note = &c;
    }
    NEED(note != nullptr);
    CHECK(note->payload == std::vector<std::uint8_t>({0x01, 0x01, 7, 0, 5, 1, 4, 26}));  // 1.1, scale10 0, the appended official scale
  }
  formats::native::Report rep;
  NEED(test::load(test::arena(1), file, &rep) == formats::native::Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  CHECK(rep.settings_skipped == 0u);
  rig.app->document_replaced();
  CHECK(rig.state().edit.entry.scale == 26);                       // the official index in the runtime
  CHECK(rig.state().edit.entry.lock == 1);
  CHECK(rig.state().edit.pads.scale() == rig.state().edit.entry.core_scale());  // 27: Hirajoshi in the core table, laid out with the lock on
  CHECK(rig.state().edit.pads.scale() == 27);
  // a pad of row 2 (Blues, pad 10) sets one of the twelve and the next save writes NOTE 1.0 with its byte 9
  rig.press(Button::Note);
  rig.pass(core::kGestureTimings.hold_ms + 1);
  rig.tap_pad(12 + 9);
  rig.release(Button::Note);
  rig.pass(1);
  CHECK(rig.state().edit.entry.scale == 15);
  rig.pass();
  CHECK(p.note().scale == 15);
  std::vector<std::uint8_t> again;
  NEED(test::save(p, again) == formats::native::Progress::Done);
  const std::vector<test::RawChunk> cs = test::split_chunks(again);
  for (const test::RawChunk& c : cs) {
    if (c.id == formats::native::kChunkNote) {
      CHECK(c.payload == std::vector<std::uint8_t>({0x01, 0x00, 7, 9, 5, 1, 4}));  // 1.0 again, byte for byte as before
    }
  }
}

TEST_CASE("OS-SCALES28 (app): a scale above 38 read from a file never reaches the runtime; the entry's index and name stay inside the 39 (and core_scale() clamps a stray one)") {
  // The file side: a NOTE 1.1 chunk whose appended scale is 39, 100 or 255 (the loader falls back to the scale of the 1.0 byte: pad 4 of the panel's map, Dorian), SCALE LOCK on.
  test::fill_persist2(test::arena(2).project);
  std::vector<std::uint8_t> base;
  NEED(test::save(test::arena(2).project, base) == formats::native::Progress::Done);
  const unsigned pad_byte = 3;  // the 1.0 byte, the panel's fourth pad
  const std::uint8_t fallback = model::kNotePanelScales[pad_byte];
  for (const unsigned appended : {39u, 100u, 255u}) {
    Rig rig;
    std::vector<test::RawChunk> cs = test::split_chunks(base);
    for (test::RawChunk& c : cs) {
      if (c.id == formats::native::kChunkNote) {
        c.payload = {0x01, 0x01, 7, static_cast<std::uint8_t>(pad_byte), 5, 1, 4, static_cast<std::uint8_t>(appended)};
      }
    }
    formats::native::Report rep;
    NEED(test::load(test::arena(1), test::build_file(cs, {}), &rep) == formats::native::Progress::Done);
    CHECK_MESSAGE(rep.settings_skipped == 1u, "appended " << appended << ": counted");
    rig.app->document_replaced();
    const app::NoteEntry& e = rig.state().edit.entry;
    CHECK_MESSAGE(e.scale == fallback, "appended " << appended << ": the 1.0 byte's scale, not " << int(e.scale));
    CHECK(e.scale <= model::kNoteScaleMax);
    CHECK(e.core_scale() == fallback + 1u);
    CHECK(std::strcmp(e.scale_name(), core::kScaleNames[fallback + 1u]) == 0);
    CHECK(e.lock == 1);
    CHECK(rig.state().edit.pads.scale() == e.core_scale());  // the NOTES board lays out a scale that exists
    CHECK(rig.project().note().scale == fallback);
  }
  // The runtime side: an index the loader would never hand over (a stray write) still reads a real scale: core_scale() is Chromatic, never a row past the table.
  app::NoteEntry stray;
  for (const unsigned s : {39u, 40u, 100u, 255u}) {
    stray.scale = static_cast<std::uint8_t>(s);
    CHECK_MESSAGE(stray.core_scale() == 1u, "scale " << s);
    CHECK(std::strcmp(stray.scale_name(), "Chromatic") == 0);
    CHECK(stray.core_scale() < core::kScaleCount);
    (void)stray.in_scale(60);  // reads inside the table (the sanitizer build is the judge)
    (void)stray.snap(61);
  }
  stray.scale = model::kNoteScaleMax;  // the last valid one is not clamped
  CHECK(stray.core_scale() == core::kScaleCount - 1u);
  CHECK(std::strcmp(stray.scale_name(), "Enigmatic") == 0);
}
