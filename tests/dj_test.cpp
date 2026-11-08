// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-DJ A1-A4: the DJ page of Perform - the crossfader's maths (the curves, the groups), the page on the App (the encoder, the pads' row 4, CURVE, RESET, the groups on Shift + row 1, the filter on F4 or the
// FILT pad with LATCH), what reaches the engine's ParamChange ring and the end of every DJ effect on leaving the page. The strip's own gain (recompute_strip), the filter's DSP and the export are
// audio_perform_test.cpp's and audio_dj_filter_test.cpp's. A4: the pattern row (queue, cancel, banks, LEDs), the tempo nudge (exact return) and the fine tempo.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/dj.h>
#include <o3os/app/module_import.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/perform_screen.h>
#include <o3os/audio/master_scratch.h>
#include <o3os/audio/voices.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/audio_clock.h>
#include <o3os/model/params.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fake_storage.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/seq/fx.h>
#include <o3os/seq/perform.h>
#include <o3os/seq/sequencer.h>
#include <o3os/sim/demo_project.h>

#include "support/seq_fixture.h"
#include "support/static_slot.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::DjCurve;
using app::DjFx;
using app::DjGroup;
using app::DjState;
using app::DjTrans;
using app::PadRole;
using app::ScreenId;

namespace {

namespace gp = audio::graph_param;

alignas(8) std::uint8_t g_ring[64 * 1024];

struct DiscardCommands final : seq::CommandSink {
  unsigned pushed = 0;
  bool accept = true;
  std::vector<seq::Command> got;
  bool push(const seq::Command& c) noexcept override {
    if (!accept) {
      return false;
    }
    ++pushed;
    got.push_back(c);
    return true;
  }
  // The PerfTempo values told since the last call, in order (0: the override ended).
  std::vector<std::uint32_t> tempos() {
    std::vector<std::uint32_t> out;
    for (const seq::Command& c : got) {
      if (c.kind == seq::Command::Kind::PerfTempo) {
        out.push_back(c.c);
      }
    }
    got.clear();
    return out;
  }
};

// A transport that takes the queue (the sequencer's QueuePattern) and keeps what it was asked.
struct Transport final : app::TransportSink {
  std::vector<unsigned> queued;
  std::vector<unsigned> jumped;  // B2: the live jumps asked for
  bool queue_jump(unsigned row) noexcept override {
    jumped.push_back(row);
    return true;
  }
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
  bool queue_pattern(unsigned id) noexcept override {
    queued.push_back(id);
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
  std::unique_ptr<app::PerformBoard> board = std::make_unique<app::PerformBoard>();
  std::unique_ptr<core::ParamRing> ring = std::make_unique<core::ParamRing>();
  DiscardCommands commands;
  Transport transport;
  audio::PlayPosition position;

  Rig() {
    model::Project& p = test::arena(1).project;
    sim::fill_demo_project(p);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    board->out = &commands;
    app->attach_perform(board.get());
    app->attach_param_ring(ring.get());
    app->attach_transport(&transport);
    app->attach_play_position(&position);
    tap(Button::Performance);
  }
  void to_dj() {
    tap(Button::Performance);
    NEED(on_dj());
  }
  bool on_dj() const { return app->state().dj_page(); }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void tap(Button b) {
    press(b);
    release(b);
    pass();
  }
  void pad_down(unsigned n) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(n), 127));
    pass();
  }
  void pad_up(unsigned n) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(n), 0));
    pass();
  }
  void pad_tap(unsigned n) {
    pad_down(n);
    pad_up(n);
  }
  // A lone Shift hold turns the pads into the TRACKS layer; row 1 is the groups on the DJ page.
  void group_tap(unsigned track) {
    press(Button::Shift);
    pass(1);
    pad_tap(track);
    release(Button::Shift);
    pass(1);
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass();
  }
  void shift_turn(int detents) {
    press(Button::Shift);
    pass(1);
    turn(detents);
    release(Button::Shift);
    pass(1);
  }
  // Everything the page told the engine since the last call: the cut of each track, in order.
  struct Told {
    unsigned track;
    float value;
    std::uint32_t ramp;
  };
  std::vector<Told> cuts_;
  std::vector<Told> filt_;
  std::vector<Told> kill_;
  void drain() {
    core::ParamChange c{};
    while (ring->pop(c)) {
      if (core::param_kind(c.id) == core::ParamKind::Track && core::param_field(c.id) == gp::strip::XfGain) {
        cuts_.push_back({core::param_index(c.id), c.value, c.ramp_samples});
      } else if (core::param_kind(c.id) == core::ParamKind::Master && core::param_field(c.id) == gp::master::DjFilter) {
        filt_.push_back({0, c.value, c.ramp_samples});
      } else if (core::param_kind(c.id) == core::ParamKind::Master && core::param_field(c.id) == gp::master::KillEq) {
        kill_.push_back({0, c.value, c.ramp_samples});
      }
    }
  }
  std::vector<Told> told() {
    drain();
    std::vector<Told> out;
    out.swap(cuts_);
    return out;
  }
  // The filter's values told to the engine since the last call, in order (the glide in `ramp`).
  std::vector<Told> filters() {
    drain();
    std::vector<Told> out;
    out.swap(filt_);
    return out;
  }
  // The kill masks told to the engine since the last call, in order.
  std::vector<Told> kills() {
    drain();
    std::vector<Told> out;
    out.swap(kill_);
    return out;
  }
  // The cut the engine would hold for each track after `told`.
  void settle(float (&cut)[app::kDjTracks]) {
    for (const Told& t : told()) {
      cut[t.track] = t.value;
    }
  }
  const app::DjState& dj() const { return app->state().dj; }
  // B1: the Perform layer's commands told since the last call (the stutter and the control table's groups), in order; the rest is dropped.
  std::vector<seq::Command> perf() {
    std::vector<seq::Command> out;
    for (const seq::Command& c : commands.got) {
      if (c.kind == seq::Command::Kind::PerfStutter || c.kind == seq::Command::Kind::PerfControl) {
        out.push_back(c);
      }
    }
    commands.got.clear();
    return out;
  }
  void play(unsigned pattern, std::uint32_t bpm_x100 = 12000) {
    position.publish(true, pattern, 0, bpm_x100);
    pass(2);
  }
  void stop() {
    position.publish(false, 1, 0, 12000);
    pass(2);
  }
  model::Project& project() { return test::arena(1).project; }
};

}  // namespace

TEST_CASE("dj maths: SMOOTH is equal power, from all A to all B, and never boosts") {
  using app::dj_level;
  CHECK(dj_level(DjGroup::A, 0, DjCurve::Smooth) == 255);
  CHECK(dj_level(DjGroup::B, 0, DjCurve::Smooth) == 0);
  CHECK(dj_level(DjGroup::A, 96, DjCurve::Smooth) == 0);
  CHECK(dj_level(DjGroup::B, 96, DjCurve::Smooth) == 255);
  int prev_a = 256;
  int prev_b = -1;
  for (unsigned p = 0; p <= 96; ++p) {
    const int a = dj_level(DjGroup::A, p, DjCurve::Smooth);
    const int b = dj_level(DjGroup::B, p, DjCurve::Smooth);
    CHECK_MESSAGE(a <= prev_a, "A falls at " << p);
    CHECK_MESSAGE(b >= prev_b, "B rises at " << p);
    prev_a = a;
    prev_b = b;
    const double power = (static_cast<double>(a) * a + static_cast<double>(b) * b) / (255.0 * 255.0);  // equal power: a^2 + b^2 = 1 (to the 8-bit table's rounding)
    CHECK_MESSAGE((power > 0.985 && power < 1.015), "power " << power << " at " << p);
    CHECK_MESSAGE(dj_level(DjGroup::A, p, DjCurve::Smooth) == dj_level(DjGroup::B, 96 - p, DjCurve::Smooth), "mirror at " << p);
  }
  CHECK(dj_level(DjGroup::A, 48, DjCurve::Smooth) == 180);  // -3 dB at the centre: 255 * cos(45 deg)
  CHECK(dj_level(DjGroup::A, 48, DjCurve::Smooth) == dj_level(DjGroup::B, 48, DjCurve::Smooth));
  CHECK(dj_level(DjGroup::A, 500, DjCurve::Smooth) == 0);  // held to the travel
}

TEST_CASE("dj maths: CUT is full level until the last position of each end, THRU is never touched") {
  using app::dj_level;
  for (unsigned p = 0; p <= 96; ++p) {
    CHECK(dj_level(DjGroup::A, p, DjCurve::Cut) == (p < 96 ? 255 : 0));
    CHECK(dj_level(DjGroup::B, p, DjCurve::Cut) == (p > 0 ? 255 : 0));
    for (const DjCurve c : {DjCurve::Smooth, DjCurve::Cut}) {
      CHECK(dj_level(DjGroup::Thru, p, c) == 255);
    }
  }
}

TEST_CASE("dj maths: row 4 is the fader in 12 symmetric steps (A, 10..40 %, centre twice, 60..90 %, B), and the lit pad is the nearest") {
  const unsigned expect[12] = {0, 10, 19, 29, 38, 48, 48, 58, 67, 77, 86, 96};
  for (unsigned c = 0; c < 12; ++c) {
    CHECK(app::dj_pad_position(c) == expect[c]);
    CHECK(app::dj_pad_position(c) + app::dj_pad_position(11 - c) == 96);
    CHECK(app::dj_pad_lit(c, app::dj_pad_position(c)));
    CHECK(app::dj_pad_column(app::dj_pad_position(c)) == (c == 6 ? 5u : c));
  }
  // Both centre pads are lit at the centre and only there; one pad otherwise.
  for (unsigned pos = 0; pos <= 96; ++pos) {
    unsigned lit = 0;
    for (unsigned c = 0; c < 12; ++c) lit += app::dj_pad_lit(c, pos) ? 1u : 0u;
    CHECK(lit == (pos == 48 ? 2u : 1u));
  }
  CHECK(app::dj_pad_lit(5, 48));
  CHECK(app::dj_pad_lit(6, 48));
  CHECK(app::dj_pad_lit(7, 49));
  CHECK(app::dj_pad_column(0) == 0);
  CHECK(app::dj_pad_column(96) == 11);
  CHECK(app::dj_pad_position(40) == 96);  // held to the pads
}

TEST_CASE("dj state: the encoder is 4 positions a detent (1 with Shift), held to the ends; the groups cycle; leaving keeps only the groups") {
  DjState dj;
  CHECK(dj.pos == 48);
  CHECK(dj.turn(+1, false));
  CHECK(dj.pos == 52);
  CHECK(dj.turn(-2, false));
  CHECK(dj.pos == 44);
  CHECK(dj.turn(+3, true));
  CHECK(dj.pos == 47);
  CHECK(dj.turn(+100, false));
  CHECK(dj.pos == 96);
  CHECK_FALSE(dj.turn(+1, false));
  CHECK(dj.turn(-100, false));
  CHECK(dj.pos == 0);
  CHECK_FALSE(dj.turn(-1, true));
  CHECK(dj.group_of(3) == DjGroup::Thru);
  CHECK(dj.cycle_group(3));
  CHECK(dj.group_of(3) == DjGroup::A);
  CHECK(dj.cycle_group(3));
  CHECK(dj.group_of(3) == DjGroup::B);
  CHECK(dj.cycle_group(3));
  CHECK(dj.group_of(3) == DjGroup::Thru);
  CHECK_FALSE(dj.cycle_group(12));  // tracks 13-16 are not reachable
  dj.cycle_group(0);
  dj.set_position(90);
  dj.toggle_curve();
  dj.leave();
  CHECK(dj.pos == 48);
  CHECK(dj.curve == DjCurve::Smooth);
  CHECK(dj.group_of(0) == DjGroup::A);  // the groups stay until power off
  // not in view: unity for every track, whatever the fader and the groups were; in view: the groups' cut
  dj.cycle_group(1);
  dj.cycle_group(1);  // B
  dj.set_position(0);
  for (unsigned t = 0; t < 12; ++t) {
    CHECK(dj.want(t, false) == 255);
  }
  CHECK(dj.want(0, true) == 255);  // A at all A
  CHECK(dj.want(1, true) == 0);    // B at all A
  CHECK(dj.want(2, true) == 255);  // THRU
}

TEST_CASE("dj publish: only the tracks that changed go out, with the 20 ms glide, and a refused push is retried") {
  DjState dj;
  core::ParamRing ring;
  CHECK(app::dj_publish(dj, true, &ring) == 0);  // nothing assigned, nothing to tell
  dj.cycle_group(0);                             // A
  dj.cycle_group(5);
  dj.cycle_group(5);  // B
  CHECK(app::dj_publish(dj, true, &ring) == 2);  // centred: both at -3 dB
  core::ParamChange c{};
  NEED(ring.pop(c));
  CHECK(core::param_index(c.id) == 0);
  CHECK(core::param_field(c.id) == gp::strip::XfGain);
  CHECK(c.value == 180.0f);
  CHECK(c.ramp_samples == 882);  // 20 ms
  NEED(ring.pop(c));
  CHECK(core::param_index(c.id) == 5);
  CHECK(c.value == 180.0f);
  CHECK(app::dj_publish(dj, true, &ring) == 0);  // told already
  dj.set_position(96);
  CHECK(app::dj_publish(dj, true, &ring) == 2);
  NEED(ring.pop(c));
  CHECK(c.value == 0.0f);
  NEED(ring.pop(c));
  CHECK(c.value == 255.0f);
  // the ring is full: nothing is lost, the next pass sends it
  while (ring.push(core::ParamChange{})) {
  }
  dj.set_position(0);
  CHECK(app::dj_publish(dj, true, &ring) == 0);
  CHECK(dj.sent[0] == 0);  // still what the engine heard
  while (ring.pop(c)) {
  }
  CHECK(app::dj_publish(dj, true, &ring) == 2);
  CHECK(dj.sent[0] == 255);
  CHECK(dj.sent[5] == 0);
  // no engine: nothing happens
  CHECK(app::dj_publish(dj, false, nullptr) == 0);
}

TEST_CASE("dj page: the Performance button again turns to it, the pads and keys are the DJ's and the slots stay untouched") {
  Rig rig;
  CHECK(rig.app->state().screen == ScreenId::Perform);
  CHECK(app::kPerformScreen.pages() == 2);  // SLOTS, DJ
  CHECK_FALSE(rig.on_dj());
  rig.to_dj();
  CHECK(rig.app->failed_frames() == 0);
  CHECK(app::kPerformScreen.field_context(rig.app->model(), rig.app->state()) == app::Context::PerformDj);
  // the slots' keys, pads and encoder do nothing here
  const unsigned before = rig.commands.pushed;
  const bool latch = rig.board->bank.latch();
  rig.pad_down(27);  // slot 4, row 3 of the slots' page: the FILT pad here, the slot is not touched
  CHECK_FALSE(rig.board->bank.held(3));
  rig.pad_up(27);
  rig.pad_tap(2);
  rig.turn(+3);
  rig.turn(-3);
  rig.tap(Button::F1);
  rig.tap(Button::F6);  // the DJ page's LATCH: the slots' latch is not touched
  rig.tap(Button::F7);
  CHECK(rig.commands.pushed == before);
  CHECK(rig.board->bank.latch() == latch);
  // the pads' LEDs: row 4 is the fader with the nearest pad Front; the FILT pad (row 3, pad 4) is Back; the KILL pads, the TRANS pads (B2) and TAPE STOP (C1) are Back and so is row 2 since B1 (REP, ECHO, CUT: its own test); the rest of the rows above is off (row 1 is the pattern row since A4: its own test)
  PadRole roles[48];
  NEED(rig.app->pad_lights(roles));
  for (unsigned p = app::kDjBankSize; p < 48; ++p) {
    const PadRole want = p == app::kDjFiltPad || p == app::kDjTapePad || (p >= app::kDjKillPad && p < app::kDjKillPad + app::kDjKills) || (p >= app::kDjTransPad && p < app::kDjTransPad + 3u) || (p >= app::kDjFxPad && p < app::kDjFxPad + app::kDjFxPads) ? PadRole::Back : (p < 36 ? PadRole::Off : (app::dj_pad_lit(p - 36, rig.dj().pos) ? PadRole::Front : PadRole::Back));
    CHECK_MESSAGE(roles[p] == want, "pad " << p);
  }
  // the key row: CURVE, FILTER, LATCH and RESET are live, the others read "-"
  const char* const* keys = app::kPerformScreen.shift_row(rig.app->state());
  CHECK(std::string(keys[0]) == "-");
  CHECK(std::string(app::kPerformDjSoftKeys[2]) == "CURVE");
  CHECK(std::string(app::kPerformDjSoftKeys[3]) == "FILTER");
  CHECK(std::string(app::kPerformDjSoftKeys[5]) == "LATCH");
  CHECK(std::string(app::kPerformDjSoftKeys[7]) == "RESET");
  // Master held + the encoder is still the master volume (a chord of Global): the fader does not move
  const unsigned pos = rig.dj().pos;
  rig.press(Button::Master);
  rig.pass(1);
  rig.turn(+3);
  rig.release(Button::Master);
  rig.pass(1);
  CHECK(rig.dj().pos == pos);
  CHECK(rig.on_dj());
  // pressing the button again goes back to the slots
  rig.tap(Button::Performance);
  CHECK_FALSE(rig.on_dj());
}

TEST_CASE("dj page: the encoder, Shift + encoder and the pads of row 4 move the fader and the engine hears the groups' cut") {
  Rig rig;
  rig.to_dj();
  rig.group_tap(0);  // track 1 -> A
  rig.group_tap(4);
  rig.group_tap(4);  // track 5 -> B
  CHECK(rig.dj().group_of(0) == DjGroup::A);
  CHECK(rig.dj().group_of(4) == DjGroup::B);
  float cut[app::kDjTracks];
  for (float& c : cut) {
    c = 255.0f;
  }
  rig.settle(cut);
  CHECK(cut[0] == doctest::Approx(180.0f));  // centred: -3 dB each
  CHECK(cut[4] == doctest::Approx(180.0f));
  CHECK(cut[1] == 255.0f);  // THRU
  // one turn is the whole travel: 24 detents
  rig.turn(-12);
  CHECK(rig.dj().pos == 0);
  rig.settle(cut);
  CHECK(cut[0] == 255.0f);
  CHECK(cut[4] == 0.0f);
  rig.turn(+24);
  CHECK(rig.dj().pos == 96);
  rig.settle(cut);
  CHECK(cut[0] == 0.0f);
  CHECK(cut[4] == 255.0f);
  rig.turn(+5);  // held at the end: nothing more to tell
  CHECK(rig.told().empty());
  // Shift + encoder: one position a detent
  rig.shift_turn(-1);
  CHECK(rig.dj().pos == 95);
  // row 4: pad 37 is all A, pad 48 all B, a tap is a jump (a cut the encoder cannot make) with the glide
  rig.pad_tap(36);
  CHECK(rig.dj().pos == 0);
  rig.pad_tap(36 + 11);
  CHECK(rig.dj().pos == 96);
  rig.pad_tap(36 + 3);  // 30 %
  CHECK(rig.dj().pos == 29);
  rig.pad_tap(36 + 8);  // 70 %
  CHECK(rig.dj().pos == 67);
  rig.pad_tap(36 + 5);  // pads 6 and 7 are both the centre (OS-DJCENTRE)
  CHECK(rig.dj().pos == 48);
  rig.pad_tap(36 + 11);
  rig.pad_tap(36 + 6);
  CHECK(rig.dj().pos == 48);
  const std::vector<Rig::Told> told = rig.told();
  NEED(!told.empty());
  for (const Rig::Told& t : told) {
    CHECK(t.ramp == 882);
  }
  // rows 1-3 are the later slices': nothing moves
  const unsigned pos = rig.dj().pos;
  rig.pad_tap(3);
  rig.pad_tap(20);
  rig.pad_tap(30);
  CHECK(rig.dj().pos == pos);
}

TEST_CASE("dj page: CURVE toggles SMOOTH and CUT, RESET centres the fader and keeps the groups") {
  Rig rig;
  rig.to_dj();
  rig.group_tap(0);
  rig.pad_tap(36);  // all A
  float cut[app::kDjTracks];
  for (float& c : cut) {
    c = 255.0f;
  }
  rig.settle(cut);
  rig.tap(Button::F3);
  CHECK(rig.dj().curve == DjCurve::Cut);
  rig.turn(+12);  // the centre: CUT is full level on both sides
  CHECK(rig.dj().pos == 48);
  rig.settle(cut);
  CHECK(cut[0] == 255.0f);
  rig.turn(+12);  // the last position: the cut
  rig.settle(cut);
  CHECK(cut[0] == 0.0f);
  rig.tap(Button::F3);
  CHECK(rig.dj().curve == DjCurve::Smooth);
  rig.tap(Button::F8);  // RESET
  CHECK(rig.dj().pos == 48);
  CHECK(rig.dj().group_of(0) == DjGroup::A);
  rig.settle(cut);
  CHECK(cut[0] == doctest::Approx(180.0f));
}

TEST_CASE("dj page: leaving it ends every DJ effect - the fader, the curve, the cut at the engine - and keeps the groups; coming back finds it at rest") {
  const Button leaves[] = {Button::Performance, Button::Pattern, Button::Master, Button::Song};
  for (const Button leave : leaves) {
    Rig rig;
    rig.to_dj();
    rig.group_tap(0);
    rig.group_tap(7);
    rig.group_tap(7);
    rig.pad_tap(36 + 11);  // all B: track 1 (A) is cut
    rig.tap(Button::F3);
    float cut[app::kDjTracks];
    for (float& c : cut) {
      c = 255.0f;
    }
    rig.settle(cut);
    NEED(cut[0] == 0.0f);
    rig.tap(leave);
    CHECK_FALSE(rig.on_dj());
    rig.pass();
    // unity for every track, glided; the fader is centred and the curve SMOOTH; the groups stay
    for (const Rig::Told& t : rig.told()) {
      cut[t.track] = t.value;
      CHECK(t.ramp == 882);
    }
    for (unsigned t = 0; t < app::kDjTracks; ++t) {
      CHECK_MESSAGE(cut[t] == 255.0f, "track " << t + 1);
    }
    CHECK(rig.dj().pos == 48);
    CHECK(rig.dj().curve == DjCurve::Smooth);
    CHECK(rig.dj().group_of(0) == DjGroup::A);
    CHECK(rig.dj().group_of(7) == DjGroup::B);
    // the slots page does not hear the DJ at all: a pad is the slot's
    if (leave == Button::Performance) {
      rig.pad_down(27);
      CHECK(rig.board->bank.held(3));
      rig.pad_up(27);
    }
  }
}

TEST_CASE("dj page: leaving by a screen that is not a button press (the peek of Song) also ends it, and a track outside the page is never cut") {
  Rig rig;
  rig.to_dj();
  rig.group_tap(2);
  rig.pad_tap(36 + 11);
  float cut[app::kDjTracks];
  for (float& c : cut) {
    c = 255.0f;
  }
  rig.settle(cut);
  CHECK(cut[2] == 0.0f);
  for (unsigned t = 0; t < app::kDjTracks; ++t) {
    if (t != 2) {
      CHECK(cut[t] == 255.0f);
    }
  }
  rig.press(Button::Song);
  rig.pass(700);  // held: Song is shown for as long as it is held
  rig.settle(cut);
  CHECK(cut[2] == 255.0f);
  rig.release(Button::Song);
  rig.pass();
  CHECK(rig.dj().pos == 48);
}

// ---- A2: the filter ------------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("dj state: a turn fed from data (INT_MAX, INT_MIN) is clamped before it is multiplied - the ends, no overflow") {
  DjState dj;
  CHECK(dj.turn(INT_MAX, false));
  CHECK(dj.pos == 96);
  CHECK(dj.turn(INT_MIN, false));
  CHECK(dj.pos == 0);
  CHECK(dj.turn(INT_MAX, true));
  CHECK(dj.pos == 96);
  CHECK(dj.turn(INT_MIN, true));
  CHECK(dj.pos == 0);
  CHECK(dj.turn_filter(INT_MAX));
  CHECK(dj.filt == 100);
  CHECK(dj.turn_filter(INT_MIN));
  CHECK(dj.filt == -100);
  CHECK_FALSE(dj.turn_filter(INT_MIN + 1));  // (already at the end)
  CHECK(dj.filt == -100);
  CHECK(app::dj_detents(INT_MAX) == app::kDjMaxDetents);
  CHECK(app::dj_detents(INT_MIN) == -app::kDjMaxDetents);
  CHECK(app::dj_detents(-7) == -7);
}

TEST_CASE("dj state: the filter is 10 a detent held to the ends, springs back at the release unless LATCH is on, and RESET and leaving clear it") {
  DjState dj;
  CHECK(dj.at_rest());
  CHECK(dj.turn_filter(+3));
  CHECK(dj.filt == 30);
  CHECK(dj.turn_filter(-5));
  CHECK(dj.filt == -20);
  CHECK(dj.turn_filter(-100));
  CHECK(dj.filt == -100);  // closed low-pass
  CHECK_FALSE(dj.turn_filter(-1));
  CHECK(dj.turn_filter(+100));
  CHECK(dj.filt == 100);  // closed high-pass
  CHECK_FALSE(dj.turn_filter(+1));
  CHECK_FALSE(dj.at_rest());
  // the release: back to 0
  CHECK(dj.release_filter());
  CHECK(dj.filt == 0);
  CHECK_FALSE(dj.release_filter());  // nothing to spring back
  // LATCH: the release leaves it; unlatching springs it back
  dj.toggle_latch();
  CHECK(dj.latch);
  dj.turn_filter(-4);
  CHECK_FALSE(dj.release_filter());
  CHECK(dj.filt == -40);
  dj.toggle_latch();
  CHECK_FALSE(dj.latch);
  CHECK(dj.filt == 0);
  // RESET clears the filter and keeps LATCH (a setting, as the curve is); leaving clears both
  dj.toggle_latch();
  dj.turn_filter(+7);
  CHECK(dj.reset());
  CHECK(dj.filt == 0);
  CHECK(dj.latch);
  CHECK_FALSE(dj.reset());  // already at rest
  dj.turn_filter(+2);
  dj.filt_pad = true;
  dj.leave();
  CHECK(dj.filt == 0);
  CHECK_FALSE(dj.latch);
  CHECK_FALSE(dj.filt_pad);
  CHECK(dj.at_rest());
}

TEST_CASE("dj publish: the filter goes out when it changes - 60 ms a detent, 120 ms to 0 - 0 when the page is not in view, and a refused push is retried") {
  DjState dj;
  core::ParamRing ring;
  CHECK(app::dj_publish(dj, true, &ring) == 0);  // the engine starts bypassed: nothing to tell
  dj.turn_filter(+3);
  CHECK(app::dj_publish(dj, true, &ring) == 1);
  core::ParamChange c{};
  NEED(ring.pop(c));
  CHECK(core::param_kind(c.id) == core::ParamKind::Master);
  CHECK(core::param_field(c.id) == gp::master::DjFilter);
  CHECK(c.value == 30.0f);
  CHECK(c.ramp_samples == 2646);
  CHECK(app::dj_publish(dj, true, &ring) == 0);  // told already
  dj.turn_filter(-1);
  CHECK(app::dj_publish(dj, true, &ring) == 1);
  NEED(ring.pop(c));
  CHECK(c.value == 20.0f);
  // the spring-back is the longer glide
  dj.release_filter();
  CHECK(app::dj_publish(dj, true, &ring) == 1);
  NEED(ring.pop(c));
  CHECK(c.value == 0.0f);
  CHECK(c.ramp_samples == 5292);
  // not in view: the engine hears 0 whatever the page held
  dj.turn_filter(-10);
  CHECK(app::dj_publish(dj, true, &ring) == 1);
  NEED(ring.pop(c));
  CHECK(c.value == -100.0f);
  CHECK(app::dj_publish(dj, false, &ring) == 1);
  NEED(ring.pop(c));
  CHECK(c.value == 0.0f);
  CHECK(c.ramp_samples == 5292);
  // a full ring loses nothing: the next pass sends it
  dj.turn_filter(+5);  // from -100 (the page's own value, which the engine was told to forget): -50
  while (ring.push(core::ParamChange{})) {
  }
  CHECK(app::dj_publish(dj, true, &ring) == 0);
  CHECK(dj.filt_sent == 0);  // still what the engine heard
  while (ring.pop(c)) {
  }
  CHECK(app::dj_publish(dj, true, &ring) == 1);
  CHECK(dj.filt_sent == -50);
  CHECK(app::dj_publish(dj, true, nullptr) == 0);  // no engine: nothing happens
}

TEST_CASE("dj page: F4 held + the encoder is the filter, 10 a detent, and it springs back at the release; the crossfader is untouched") {
  Rig rig;
  rig.to_dj();
  rig.tap(Button::F6);  // (the latch is exercised below; here a plain release)
  rig.tap(Button::F6);
  CHECK_FALSE(rig.dj().latch);
  (void)rig.filters();
  const unsigned pos = rig.dj().pos;
  rig.press(Button::F4);
  rig.pass(1);
  rig.turn(+3);
  CHECK(rig.dj().filt == 30);
  CHECK(rig.dj().pos == pos);  // held F4: the encoder is not the fader
  rig.turn(-5);
  CHECK(rig.dj().filt == -20);
  rig.turn(-100);
  CHECK(rig.dj().filt == -100);
  std::vector<Rig::Told> told = rig.filters();
  NEED(told.size() == 3);
  CHECK(told[0].value == 30.0f);
  CHECK(told[1].value == -20.0f);
  CHECK(told[2].value == -100.0f);
  for (const Rig::Told& t : told) {
    CHECK(t.ramp == 2646);
  }
  rig.release(Button::F4);
  rig.pass();
  CHECK(std::string(rig.app->state().notify.last_text()) != "FILTER: HOLD, TURN THE ENCODER");  // OS-QAFIX-14: the release of a turned F4 is no tap, so it does not hint
  CHECK(rig.dj().filt == 0);  // the spring-back
  told = rig.filters();
  NEED(told.size() == 1);
  CHECK(told[0].value == 0.0f);
  CHECK(told[0].ramp == 5292);
  // the encoder without F4 is the fader again, and it moves no filter
  rig.turn(+2);
  CHECK(rig.dj().pos == pos + 8);
  CHECK(rig.dj().filt == 0);
  CHECK(rig.filters().empty());
  // F4 on its own, a tap, moves no filter and says how it is played (OS-QAFIX-14: a labelled key answers)
  rig.tap(Button::F4);
  CHECK(rig.app->state().notify.last_text() == std::string("FILTER: HOLD, TURN THE ENCODER"));
  CHECK(rig.dj().filt == 0);
  CHECK(rig.dj().pos == pos + 8);
  CHECK(rig.filters().empty());
  CHECK(rig.on_dj());
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("dj page: LATCH keeps the filter at the release, unlatching springs it back, and RESET clears it") {
  Rig rig;
  rig.to_dj();
  rig.tap(Button::F6);
  CHECK(rig.dj().latch);
  rig.press(Button::F4);
  rig.pass(1);
  rig.turn(+4);
  rig.release(Button::F4);
  rig.pass();
  CHECK(rig.dj().filt == 40);  // latched: it stays
  std::vector<Rig::Told> told = rig.filters();
  NEED(told.size() == 1);
  CHECK(told[0].value == 40.0f);
  // a second hold carries on from where it was, and the release still leaves it
  rig.press(Button::F4);
  rig.pass(1);
  rig.turn(+2);
  rig.release(Button::F4);
  rig.pass();
  CHECK(rig.dj().filt == 60);
  rig.tap(Button::F6);  // unlatch: springs back at once
  CHECK_FALSE(rig.dj().latch);
  CHECK(rig.dj().filt == 0);
  told = rig.filters();
  NEED(!told.empty());
  CHECK(told.back().value == 0.0f);
  CHECK(told.back().ramp == 5292);
  // RESET clears a latched filter and keeps the latch; the groups and the fader's centre as before
  rig.tap(Button::F6);
  rig.press(Button::F4);
  rig.pass(1);
  rig.turn(-3);
  rig.release(Button::F4);
  rig.pass();
  CHECK(rig.dj().filt == -30);
  (void)rig.filters();
  rig.tap(Button::F8);
  CHECK(rig.dj().filt == 0);
  CHECK(rig.dj().latch);
  told = rig.filters();
  NEED(told.size() == 1);
  CHECK(told[0].value == 0.0f);
}

TEST_CASE("dj page: the FILT pad held gives the encoder to the filter, the release springs it back, and F4 still works beside it") {
  Rig rig;
  rig.to_dj();
  const unsigned pos = rig.dj().pos;
  (void)rig.filters();
  rig.pad_down(app::kDjFiltPad);
  CHECK(rig.dj().filt_pad);
  PadRole roles[48];
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[app::kDjFiltPad] == PadRole::Front);
  rig.turn(+6);
  CHECK(rig.dj().filt == 60);
  CHECK(rig.dj().pos == pos);
  rig.turn(-2);
  CHECK(rig.dj().filt == 40);
  rig.pad_up(app::kDjFiltPad);
  CHECK_FALSE(rig.dj().filt_pad);
  CHECK(rig.dj().filt == 0);
  const std::vector<Rig::Told> told = rig.filters();
  NEED(told.size() == 3);
  CHECK(told[0].value == 60.0f);
  CHECK(told[1].value == 40.0f);
  CHECK(told[2].value == 0.0f);
  CHECK(told[2].ramp == 5292);
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[app::kDjFiltPad] == PadRole::Back);
  // the encoder is the fader again
  rig.turn(+1);
  CHECK(rig.dj().pos == pos + 4);
  // with LATCH on the pad's release leaves the filter, which then lights the pad
  rig.tap(Button::F6);
  rig.pad_down(app::kDjFiltPad);
  rig.turn(-3);
  rig.pad_up(app::kDjFiltPad);
  CHECK(rig.dj().filt == -30);
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[app::kDjFiltPad] == PadRole::Front);
  // F4 let go while the FILT pad still holds the filter does not spring it back; the pad's release does
  rig.tap(Button::F6);  // unlatch: 0
  rig.pad_down(app::kDjFiltPad);
  rig.press(Button::F4);
  rig.pass(1);
  rig.turn(+2);  // the pad owns the encoder
  CHECK(rig.dj().filt == 20);
  rig.release(Button::F4);
  rig.pass();
  CHECK(rig.dj().filt == 20);
  rig.pad_up(app::kDjFiltPad);
  CHECK(rig.dj().filt == 0);
  // another pad of the page is not the filter's
  rig.pad_down(30);
  CHECK_FALSE(rig.dj().filt_pad);
  rig.pad_up(30);
}

TEST_CASE("dj page: leaving the page ends the filter - 0 and LATCH off at the engine, a pad held or not - and the slots page is untouched") {
  const Button leaves[] = {Button::Performance, Button::Pattern, Button::Master, Button::Song};
  for (const bool by_pad : {false, true}) {
    for (const Button leave : leaves) {
      Rig rig;
      rig.to_dj();
      rig.tap(Button::F6);  // LATCH: the filter would stay
      if (by_pad) {
        rig.pad_down(app::kDjFiltPad);
        rig.turn(+5);
      } else {
        rig.press(Button::F4);
        rig.pass(1);
        rig.turn(-5);
        rig.release(Button::F4);
        rig.pass();
      }
      CHECK(rig.dj().filt == (by_pad ? 50 : -50));
      (void)rig.filters();
      rig.tap(leave);
      CHECK_FALSE(rig.on_dj());
      rig.pass();
      CHECK(rig.dj().filt == 0);
      CHECK_FALSE(rig.dj().latch);
      CHECK_FALSE(rig.dj().filt_pad);
      const std::vector<Rig::Told> told = rig.filters();
      NEED(!told.empty());
      CHECK(told.back().value == 0.0f);
      CHECK(told.back().ramp == 5292);
      // a pad let go after the page changed does no harm; coming back finds the filter at rest
      if (by_pad) {
        rig.pad_up(app::kDjFiltPad);
      }
      CHECK(rig.dj().filt == 0);
      if (leave == Button::Performance) {  // the slots page: the pad is the slot's, the encoder is not the filter's
        rig.pad_down(27);
        CHECK(rig.board->bank.held(3));
        rig.turn(+2);
        CHECK(rig.dj().filt == 0);
        rig.pad_up(27);
      }
      CHECK(rig.filters().empty());
    }
  }
}

TEST_CASE("dj page: F4 and F6 are still the slots' on the slots page, and a held F4 on the DJ page leaves the slots' HALF alone") {
  Rig rig;
  const bool latch = rig.board->bank.latch();
  rig.tap(Button::F6);  // the slots page: the slots' LATCH
  CHECK(rig.board->bank.latch() == !latch);
  CHECK_FALSE(rig.dj().latch);
  rig.tap(Button::F6);
  rig.to_dj();
  rig.press(Button::F4);  // the slots' HALF on the other page; here the filter, and the rate is untouched
  rig.pass(1);
  rig.turn(+1);
  rig.release(Button::F4);
  rig.pass();
  CHECK(rig.board->bank.rate() == 0u);  // normal speed
  CHECK(rig.dj().filt == 0);
}

TEST_CASE("dj state: a kill is down with its pad, LATCH makes a tap stay, unlatching, RESET and leaving end it") {
  app::DjState dj;
  CHECK(dj.kill() == 0u);
  CHECK(dj.at_rest());
  CHECK(dj.kill_down(0));
  CHECK(dj.kill_down(2));
  CHECK(dj.kill() == 5u);
  CHECK_FALSE(dj.at_rest());
  CHECK(dj.kill_up(0));
  CHECK(dj.kill() == 4u);  // the other pad is still down
  CHECK_FALSE(dj.kill_up(0));
  CHECK(dj.kill_up(2));
  CHECK(dj.kill() == 0u);
  CHECK_FALSE(dj.kill_down(3));
  CHECK_FALSE(dj.kill_up(3));
  dj.toggle_latch();
  CHECK(dj.kill_down(1));  // LATCH on: a tap stays ...
  CHECK(dj.kill_up(1) == false);
  CHECK(dj.kill() == 2u);
  CHECK(dj.kill_down(1));  // ... and a second tap ends it
  CHECK(dj.kill() == 0u);
  CHECK(dj.kill_down(0));
  CHECK(dj.kill_down(1));
  dj.toggle_latch();  // unlatching ends the latched kills
  CHECK(dj.kill() == 0u);
  dj.toggle_latch();
  CHECK(dj.kill_down(2));
  CHECK(dj.reset());  // RESET ends them too, and keeps LATCH
  CHECK(dj.kill() == 0u);
  CHECK(dj.latch);
  CHECK(dj.kill_down(0));
  dj.leave();
  CHECK(dj.kill() == 0u);
  CHECK(dj.at_rest());
}

TEST_CASE("dj publish: the kill mask goes out when it changes - one master KillEq value, 0 when the page is not in view, and a refused push is retried") {
  app::DjState dj;
  core::ParamRing ring;
  core::ParamChange c{};
  CHECK(app::dj_publish(dj, true, &ring) == 0u);  // nothing is killed: nothing to say
  CHECK_FALSE(ring.pop(c));
  (void)dj.kill_down(1);
  (void)dj.kill_down(2);
  CHECK(app::dj_publish(dj, true, &ring) == 1u);
  NEED(ring.pop(c));
  CHECK(core::param_kind(c.id) == core::ParamKind::Master);
  CHECK(core::param_field(c.id) == gp::master::KillEq);
  CHECK(c.value == 6.0f);
  CHECK(app::dj_publish(dj, true, &ring) == 0u);  // told once
  CHECK(app::dj_publish(dj, false, &ring) == 1u);  // the page is not in view: none, whatever is held
  NEED(ring.pop(c));
  CHECK(c.value == 0.0f);
  CHECK(dj.kill() == 6u);  // (the state itself is the App's to end, poll_dj)
  CHECK(app::dj_publish(dj, true, &ring) == 1u);
  NEED(ring.pop(c));
  CHECK(c.value == 6.0f);
  // a push the ring refuses is not counted as told
  core::ParamRing full;
  core::ParamChange fill{};
  while (full.push(fill)) {
  }
  app::DjState d2;
  (void)d2.kill_down(0);
  CHECK(app::dj_publish(d2, true, &full) == 0u);
  CHECK(d2.kill_sent == 0u);
  CHECK(full.pop(fill));
  CHECK(app::dj_publish(d2, true, &full) == 1u);
  CHECK(d2.kill_sent == 1u);
}

TEST_CASE("dj page: the KILL pads kill a band of the master while they are down, LATCH keeps them, RESET and leaving end them") {
  Rig rig;
  rig.to_dj();
  (void)rig.kills();
  const unsigned lo = app::kDjKillPad;
  const unsigned mid = app::kDjKillPad + 1;
  const unsigned hi = app::kDjKillPad + 2;
  rig.pad_down(lo);
  rig.pad_down(hi);
  rig.pass(1);
  PadRole roles[48];
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[lo] == PadRole::Front);
  CHECK(roles[mid] == PadRole::Back);
  CHECK(roles[hi] == PadRole::Front);
  rig.pad_up(lo);  // one of two let go: the other stays killed
  rig.pass(1);
  rig.pad_up(hi);
  rig.pass(1);
  std::vector<Rig::Told> told = rig.kills();
  NEED(told.size() >= 3);
  const std::size_t n = told.size();
  CHECK(told[n - 3].value == 5.0f);  // (a pass may have told the first pad alone before the second came)
  CHECK(told[n - 2].value == 4.0f);
  CHECK(told[n - 1].value == 0.0f);
  CHECK(rig.dj().kill() == 0u);
  // the crossfader and the filter are untouched, and another pad is not a kill
  CHECK(rig.dj().pos == app::kDjCentre);
  rig.pad_down(30);
  rig.pad_up(30);
  CHECK(rig.kills().empty());
  // LATCH: a tap stays, a second tap ends it, unlatching ends the rest
  rig.tap(Button::F6);
  rig.pad_down(mid);
  rig.pad_up(mid);
  rig.pass(1);
  CHECK(rig.dj().kill() == 2u);
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[mid] == PadRole::Front);
  rig.pad_down(mid);
  rig.pad_up(mid);
  rig.pass(1);
  CHECK(rig.dj().kill() == 0u);
  rig.pad_down(lo);
  rig.pad_up(lo);
  rig.tap(Button::F6);
  rig.pass(1);
  CHECK(rig.dj().kill() == 0u);
  told = rig.kills();
  NEED(told.size() >= 1);
  CHECK(told.back().value == 0.0f);
  // RESET (F8) ends a held kill; leaving the page ends every kill, held or latched, and the engine hears 0
  rig.pad_down(mid);
  rig.pass(1);
  CHECK(rig.dj().kill() == 2u);
  rig.tap(Button::F8);
  CHECK(rig.dj().kill() == 0u);
  rig.tap(Button::F6);
  rig.pad_down(hi);
  rig.pad_up(hi);
  rig.pass(1);
  CHECK(rig.dj().kill() == 4u);
  rig.tap(Button::Performance);  // back to the slots page
  rig.pass(2);
  CHECK(rig.dj().kill() == 0u);
  told = rig.kills();
  NEED(!told.empty());
  CHECK(told.back().value == 0.0f);
}

// ---- A4: the pattern row, the banks, the tempo nudge and the fine tempo -------------------------------------------------------------------------------------------------------------------------

TEST_CASE("dj state: the bank follows the pattern that plays until F1 / F2 move it, is held to 0..21, and leaving gives it back") {
  app::DjState dj;
  CHECK(dj.bank_now(0) == 0u);
  CHECK(dj.bank_now(1) == 0u);
  CHECK(dj.bank_now(12) == 0u);
  CHECK(dj.bank_now(13) == 1u);
  CHECK(dj.bank_now(255) == 21u);
  CHECK(dj.move_bank(1, 13));  // from the bank of pattern 13 (1) to 2
  CHECK(dj.bank_now(13) == 2u);
  CHECK(dj.bank_now(1) == 2u);  // set by hand: the playing pattern no longer moves it
  CHECK(dj.move_bank(-1, 1));
  CHECK(dj.move_bank(-1, 1));
  CHECK(dj.bank_now(1) == 0u);
  CHECK_FALSE(dj.move_bank(-1, 1));  // held at the first
  for (int i = 0; i < 30; ++i) {
    (void)dj.move_bank(1, 1);
  }
  CHECK(dj.bank_now(1) == app::kDjBanks - 1u);
  CHECK_FALSE(dj.move_bank(1, 1));  // and at the last
  CHECK_FALSE(dj.at_rest());
  dj.leave();
  CHECK(dj.at_rest());
  CHECK(dj.bank_now(14) == 1u);  // the bank of the pattern that plays again
}

TEST_CASE("dj maths: the nudge is the tempo -4 % / +4 %, rounded, held to the range, and the step rate converts both ways") {
  CHECK(app::dj_nudged(12000, -1) == 11520u);
  CHECK(app::dj_nudged(12000, 1) == 12480u);
  CHECK(app::dj_nudged(12345, -1) == 11851u);  // 12345 * 96 = 1185120, + 50, / 100
  CHECK(app::dj_nudged(12000, 0) == 12000u);
  CHECK(app::dj_nudged(4000, -1) == model::kMinTempoX100);
  CHECK(app::dj_nudged(80000, 1) == model::kMaxTempoX100);
  CHECK(app::dj_nominal(6000, 1) == 12000u);
  CHECK(app::dj_nominal(24000, 2) == 12000u);
  CHECK(app::dj_nominal(12000, 0) == 12000u);
  CHECK(app::dj_published(12000, 1) == 6000u);
  CHECK(app::dj_published(12000, 2) == 24000u);
  app::DjState dj;
  CHECK(dj.nudge_dir() == 0);
  dj.nudge_down(0);
  CHECK(dj.nudge_dir() == -1);
  dj.nudge_down(1);
  CHECK(dj.nudge_dir() == 0);  // both: they cancel
  CHECK(dj.nudge_up(0));
  CHECK(dj.nudge_dir() == 1);
  CHECK_FALSE(dj.nudge_up(0));
  CHECK(dj.nudge_up(1));
  CHECK(dj.nudge_dir() == 0);
  dj.nudge_down(1);
  CHECK_FALSE(dj.at_rest());
  dj.leave();  // leaving ends the nudge
  CHECK(dj.nudge_dir() == 0);
  CHECK(dj.at_rest());
}

TEST_CASE("dj tempo publish: the override is the base -4 / +4 %, told once, ended at the release, and the base is never nudged again") {
  app::DjState dj;
  DiscardCommands out;
  CHECK_FALSE(app::dj_tempo_publish(dj, true, true, false, 12000, 0, out));  // no key: nothing to say
  dj.nudge_down(1);
  CHECK(app::dj_tempo_publish(dj, true, true, false, 12000, 0, out));
  CHECK(out.tempos() == std::vector<std::uint32_t>{12480});
  CHECK_FALSE(app::dj_tempo_publish(dj, true, true, false, 12480, 0, out));  // told once; the published tempo is now the nudged one and the base stays 12000
  CHECK(dj.nudge_base == 12000u);
  (void)dj.nudge_up(1);
  CHECK(app::dj_tempo_publish(dj, true, true, false, 12480, 0, out));  // the release: the override goes ...
  CHECK(out.tempos() == std::vector<std::uint32_t>{0});
  // ... and a press before the sequencer's tempo has come back starts from the old base, not from the nudged tempo
  dj.nudge_down(1);
  CHECK(app::dj_tempo_publish(dj, true, true, false, 12480, 0, out));
  CHECK(out.tempos() == std::vector<std::uint32_t>{12480});
  (void)dj.nudge_up(1);
  CHECK(app::dj_tempo_publish(dj, true, true, false, 12480, 0, out));
  CHECK(out.tempos() == std::vector<std::uint32_t>{0});
  // the tempo is back: the base is let go, and the next nudge starts from the tempo then (a tempo edit in between)
  CHECK_FALSE(app::dj_tempo_publish(dj, true, true, false, 12000, 0, out));
  CHECK(dj.nudge_base == 0u);
  dj.nudge_down(0);
  CHECK(app::dj_tempo_publish(dj, true, true, false, 12100, 0, out));
  CHECK(out.tempos() == std::vector<std::uint32_t>{11616});
  // the step rate: HALF TIME publishes half, the override is the nominal tempo
  (void)dj.nudge_up(0);
  (void)app::dj_tempo_publish(dj, true, true, false, 5808, 1, out);
  (void)out.tempos();
  (void)app::dj_tempo_publish(dj, true, true, false, 6050, 1, out);
  dj.nudge_down(1);
  CHECK(app::dj_tempo_publish(dj, true, true, false, 6050, 1, out));
  CHECK(out.tempos() == std::vector<std::uint32_t>{12584});  // 12100 * 1.04
}

TEST_CASE("dj tempo publish: not in view, stopped or under a MIDI clock the override is none; a refused push is retried") {
  app::DjState dj;
  DiscardCommands out;
  dj.nudge_down(0);
  CHECK_FALSE(app::dj_tempo_publish(dj, true, false, false, 12000, 0, out));  // stopped
  CHECK_FALSE(app::dj_tempo_publish(dj, true, true, true, 12000, 0, out));    // MIDI Slave
  CHECK_FALSE(app::dj_tempo_publish(dj, false, true, false, 12000, 0, out));  // the page is not in view
  CHECK(out.pushed == 0u);
  out.accept = false;
  CHECK_FALSE(app::dj_tempo_publish(dj, true, true, false, 12000, 0, out));
  CHECK(dj.nudge_sent == 0u);
  out.accept = true;
  CHECK(app::dj_tempo_publish(dj, true, true, false, 12000, 0, out));
  CHECK(dj.nudge_sent == 11520u);
  // the page goes out of view while the key is down: the override ends (a refused 0 is retried too)
  out.accept = false;
  CHECK_FALSE(app::dj_tempo_publish(dj, false, true, false, 11520, 0, out));
  CHECK(dj.nudge_sent == 11520u);
  out.accept = true;
  CHECK(app::dj_tempo_publish(dj, false, true, false, 11520, 0, out));
  CHECK(dj.nudge_sent == 0u);
  CHECK(out.tempos() == std::vector<std::uint32_t>{11520, 0});
}

TEST_CASE("dj page: the pattern row queues at the end of the pattern, a second tap cancels, and the LEDs say what exists, what plays and what waits") {
  Rig rig;
  rig.project().reset();
  (void)rig.project().create_pattern(1, 16);
  (void)rig.project().create_pattern(2, 16);
  (void)rig.project().create_pattern(14, 16);
  rig.to_dj();
  rig.play(1);
  PadRole roles[48];
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[0] == PadRole::Front);  // pattern 1 plays
  CHECK(roles[1] == PadRole::Back);   // 2 exists
  CHECK(roles[2] == PadRole::Off);    // 3 does not
  rig.pad_tap(1);  // queue pattern 2
  NEED(rig.transport.queued.size() == 1);
  CHECK(rig.transport.queued.back() == 2u);
  CHECK(rig.app->state().edit.queued_pattern == 2u);
  bool lit = false;
  bool dark = false;
  for (unsigned i = 0; i < 4; ++i) {  // the queued pad blinks
    rig.pass(300);
    NEED(rig.app->pad_lights(roles));
    (lit = lit || roles[1] == PadRole::Front);
    (dark = dark || roles[1] == PadRole::Off);
  }
  CHECK(lit);
  CHECK(dark);
  CHECK(roles[0] == PadRole::Front);
  rig.pad_tap(1);  // a second tap on the queued pad cancels it
  CHECK(rig.transport.queued.back() == 0u);
  CHECK(rig.app->state().edit.queued_pattern == 0u);
  rig.pad_tap(1);  // queue again, then the sequencer starts it: the blink ends and the pad is the one that plays
  rig.play(2);
  CHECK(rig.app->state().edit.queued_pattern == 0u);
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[1] == PadRole::Front);
  CHECK(roles[0] == PadRole::Back);
  // the banks: F2 shows 13..24 (pattern 14 exists), a tap there queues 13 + col; F1 comes back; held at the first bank
  rig.tap(Button::F2);
  CHECK(rig.app->state().dj_bank() == 1u);
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[0] == PadRole::Off);
  CHECK(roles[1] == PadRole::Back);
  rig.pad_tap(1);
  CHECK(rig.transport.queued.back() == 14u);
  CHECK(rig.app->state().edit.queued_pattern == 14u);
  rig.tap(Button::F1);
  rig.tap(Button::F1);
  CHECK(rig.app->state().dj_bank() == 0u);
  // a queued pattern stays queued when the page is left, and the bank goes back to the playing pattern's
  rig.tap(Button::F2);
  rig.tap(Button::Performance);
  CHECK_FALSE(rig.app->state().dj_page());
  CHECK(rig.app->state().edit.queued_pattern == 14u);
  CHECK_FALSE(rig.dj().bank_set);
  rig.to_dj();
  CHECK(rig.app->state().dj_bank() == 0u);
  // stopped, a tap selects the pattern for editing (the PATS tap), and the bank follows the one in view until moved
  rig.stop();
  rig.pad_tap(0);
  CHECK(rig.app->state().edit.pattern == 1u);
  // Shift + row 1 is still the groups
  rig.group_tap(0);
  CHECK(rig.dj().group_of(0) == DjGroup::A);
}

TEST_CASE("dj page: an arrow held nudges the tempo -4 / +4 %, the release gives the song's tempo back exactly, leaving ends it, and the slots page's arrows are the slots'") {
  Rig rig;
  rig.to_dj();
  const unsigned t0 = rig.project().tempo_x100();
  rig.play(1, 12345);
  (void)rig.commands.tempos();
  rig.press(Button::Left);
  rig.pass();
  CHECK(rig.dj().nudge_dir() == -1);
  CHECK(rig.commands.tempos() == std::vector<std::uint32_t>{11851});
  rig.position.publish(true, 1, 0, 11851);  // the sequencer now says the nudged tempo
  rig.pass(20);
  CHECK(rig.commands.tempos().empty());  // told once, however long it is held
  rig.release(Button::Left);
  rig.pass();
  CHECK(rig.dj().nudge_dir() == 0);
  CHECK(rig.commands.tempos() == std::vector<std::uint32_t>{0});  // the override ends: the project's / the song's own tempo is back, nothing was written to it
  rig.position.publish(true, 1, 0, 12345);
  rig.pass(20);
  CHECK(rig.dj().nudge_base == 0u);
  // the project's tempo was never touched
  CHECK(rig.project().tempo_x100() == t0);
  // right: +4 %
  rig.press(Button::Right);
  rig.pass();
  CHECK(rig.commands.tempos() == std::vector<std::uint32_t>{12839});
  // leaving the page while the arrow is down ends the nudge at once; the release afterwards does no harm
  rig.tap(Button::Performance);
  CHECK(rig.dj().nudge_dir() == 0);
  CHECK(rig.commands.tempos() == std::vector<std::uint32_t>{0});
  rig.release(Button::Right);
  rig.pass();
  CHECK(rig.commands.tempos().empty());
  // stopped: no nudge, and the strip says so
  rig.to_dj();
  rig.stop();
  rig.press(Button::Left);
  rig.pass();
  CHECK(rig.dj().nudge_dir() == 0);
  rig.release(Button::Left);
  rig.pass();
  CHECK(rig.commands.tempos().empty());
}

TEST_CASE("dj page: MIDI Slave refuses the nudge and the fine tempo, with a line on the strip, and nothing is written") {
  Rig rig;
  rig.to_dj();
  rig.play(1);
  (void)rig.commands.tempos();
  CHECK(rig.app->device_settings().set(model::device_setting::ClockMode, 2.0f) == model::Status::Ok);
  const unsigned before = rig.project().tempo_x100();
  rig.press(Button::Left);
  rig.pass();
  CHECK(rig.dj().nudge_dir() == 0);
  rig.release(Button::Left);
  rig.pass();
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Right);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.project().tempo_x100() == before);
  CHECK(rig.commands.tempos().empty());
  CHECK(rig.app->device_settings().set(model::device_setting::ClockMode, 0.0f) == model::Status::Ok);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Right);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.project().tempo_x100() == before + 10u);  // the same press, the clock ours again
}

TEST_CASE("dj page: Shift + an arrow is the project tempo by 0.1 BPM, kept, one undo unit a press, held to the range, and the sequencer hears it") {
  Rig rig;
  rig.to_dj();
  const unsigned t0 = rig.project().tempo_x100();
  const auto shift_arrow = [&](Button b) {
    rig.press(Button::Shift);
    rig.pass(1);
    rig.tap(b);
    rig.release(Button::Shift);
    rig.pass(1);
  };
  (void)rig.told();
  shift_arrow(Button::Right);
  CHECK(rig.project().tempo_x100() == t0 + 10u);
  shift_arrow(Button::Right);
  shift_arrow(Button::Left);
  shift_arrow(Button::Left);
  shift_arrow(Button::Left);
  CHECK(rig.project().tempo_x100() == t0 - 10u);
  CHECK(rig.dj().nudge_dir() == 0);  // not a nudge
  CHECK(rig.commands.tempos().empty());
  // one unit a press: five undos give the tempo back
  for (int i = 0; i < 5; ++i) {
    CHECK(rig.editor->undo(model::UndoArea::Song) == model::UndoResult::Ok);
  }
  CHECK(rig.project().tempo_x100() == t0);
  // the range: held at 40 and 800 BPM
  CHECK(rig.editor->set_tempo_x100(model::kMaxTempoX100) == model::Status::Ok);
  shift_arrow(Button::Right);
  CHECK(rig.project().tempo_x100() == model::kMaxTempoX100);
  CHECK(rig.editor->set_tempo_x100(model::kMinTempoX100) == model::Status::Ok);
  shift_arrow(Button::Left);
  CHECK(rig.project().tempo_x100() == model::kMinTempoX100);
}

// The nudge over a REAL sequencer (the other nudge tests read the command stream): the override is told through the sequencer's own command ring, it plays at the nudged tempo, and at the release the
// tempo is exactly the one before - the reported tempo, the nominal tempo and the length of the steps - with the project's tempo never touched.
namespace {
struct RingSink final : seq::CommandSink {
  seq::CommandRing& ring;
  explicit RingSink(seq::CommandRing& r) : ring(r) {}
  bool push(const seq::Command& c) noexcept override { return ring.push(c); }
};
}  // namespace

TEST_CASE("dj nudge: over a real sequencer the release gives the tempo back exactly, and the steps are on the song's grid again") {
  for (const int dir : {+1, -1}) {
    test::fresh_project(4, 4);
    model::Project& p = test::seq_project();
    CHECK(p.set_tempo_x100(12345) == model::Status::Ok);  // an odd tempo: a rounding that stuck would show
    CHECK(p.create_pattern(1, 16) == model::Status::Ok);
    static test::StaticSlot<seq::Sequencer> slot;
    seq::Sequencer& s = slot.make(p);
    s.configure(seq::Sequencer::Config{});
    s.play_pattern(1);
    RingSink sink(s.commands());
    DjState dj;
    audio::SeqEvent ev[audio::kMaxEventsPerBlock];
    std::uint64_t t = 0;
    std::vector<std::uint64_t> steps;
    const auto pass = [&](unsigned blocks) {  // the App's pass: tell the sequencer, then one block
      for (unsigned b = 0; b < blocks; ++b) {
        (void)app::dj_tempo_publish(dj, true, s.playing(), false, s.tempo_x100(), 0, sink);
        const std::uint32_t c = s.step(t, 64, ev, audio::kMaxEventsPerBlock);
        for (std::uint32_t i = 0; i < c; ++i) {
          if (ev[i].kind == audio::EventKind::Step) {
            steps.push_back(t + ev[i].offset);
          }
        }
        t += 64;
      }
    };
    const auto gap = [&](std::size_t i) { return steps[i + 1] - steps[i]; };
    pass(300);
    NEED(steps.size() >= 4);
    CHECK(s.tempo_x100() == 12345u);
    const std::uint64_t grid = gap(steps.size() - 2);
    const unsigned nudged = app::dj_nudged(12345, dir);
    dj.nudge_down(dir > 0 ? 1u : 0u);
    pass(400);
    CHECK(s.tempo_x100() == nudged);
    CHECK(s.nominal_tempo_x100() == nudged);
    NEED(steps.size() >= 8);
    const std::uint64_t held = gap(steps.size() - 2);
    CHECK((dir > 0 ? held < grid : held > grid));
    (void)dj.nudge_up(dir > 0 ? 1u : 0u);
    pass(1);
    CHECK(s.tempo_x100() == 12345u);  // exactly the one before, in the block after the release
    CHECK(s.nominal_tempo_x100() == 12345u);
    CHECK(p.tempo_x100() == 12345u);
    const std::size_t from = steps.size();
    pass(500);
    NEED(steps.size() >= from + 3);
    for (std::size_t i = from; i + 1 < steps.size(); ++i) {  // the steps that begin after the release are the song's again
      CHECK(gap(i) + 1 >= grid);
      CHECK(gap(i) <= grid + 1);
    }
    CHECK(dj.nudge_base == 0u);
  }
}

// ---- B1: row 2 per group, REP, ECHO and CUT ---------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kEndBit = 1u << 16;  // PerfControl's c: the group's end

bool is_stutter(const seq::Command& c, unsigned track, unsigned value) { return c.kind == seq::Command::Kind::PerfStutter && c.a == track && c.b == value; }
// A PerfControl of `group` on `track` that sets `value` (an int16 in c's low half), or ends the group when `end`.
bool is_control(const seq::Command& c, unsigned track, seq::FxGroup group, int value, bool end = false) {
  return c.kind == seq::Command::Kind::PerfControl && c.a == track && c.b == static_cast<unsigned>(group) &&
         c.c == (end ? kEndBit : static_cast<std::uint32_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(value))));
}
unsigned count_for(const std::vector<seq::Command>& cmds, unsigned track) {
  unsigned n = 0;
  for (const seq::Command& c : cmds) {
    n += c.a == track ? 1u : 0u;
  }
  return n;
}

}  // namespace

TEST_CASE("dj maths (B1): the twelve pads of row 2 are mirrored, REP is a note value on the 16th-note step, and ECHO waits one beat") {
  for (unsigned col = 0; col < app::kDjFxPads; ++col) {
    const DjGroup g = app::dj_fx_group(col);
    CHECK(g == (col < 6 ? DjGroup::A : DjGroup::B));
    CHECK(app::dj_fx_col(g, app::dj_fx_kind(col)) == col);
    CHECK(app::dj_fx_kind(col) == app::dj_fx_kind(11 - col));  // the halves mirror: each hand has the same order from the outside in
  }
  CHECK(app::dj_fx_kind(0) == DjFx::Rep4);
  CHECK(app::dj_fx_kind(3) == DjFx::Rep32);
  CHECK(app::dj_fx_kind(4) == DjFx::Echo);
  CHECK(app::dj_fx_kind(5) == DjFx::Cut);
  CHECK(app::dj_fx_kind(6) == DjFx::Cut);
  CHECK(app::dj_fx_kind(7) == DjFx::Echo);
  CHECK(app::dj_fx_kind(11) == DjFx::Rep4);
  // the stutter values are the sequencer's, in microsteps of a beat (24 clocks x 8): 1/4 = 192, 1/8 = 96, 1/16 = 48, 1/32 = 24
  const unsigned beat = seq::kMicrostepsPerClock * 24u;
  CHECK(seq::stutter_microsteps(app::dj_rep_value(DjFx::Rep4)) * 1u == beat);
  CHECK(seq::stutter_microsteps(app::dj_rep_value(DjFx::Rep8)) * 2u == beat);
  CHECK(seq::stutter_microsteps(app::dj_rep_value(DjFx::Rep16)) * 4u == beat);
  CHECK(seq::stutter_microsteps(app::dj_rep_value(DjFx::Rep32)) * 8u == beat);
  CHECK(app::dj_rep_value(DjFx::Echo) == 0);
  CHECK(app::dj_rep_value(DjFx::Cut) == 0);
  // a beat in milliseconds, held to the tempo range
  CHECK(app::dj_beat_ms(12000) == 500);
  CHECK(app::dj_beat_ms(6000) == 1000);
  CHECK(app::dj_beat_ms(0) == 6000000u / model::kMinTempoX100);
  CHECK(app::dj_beat_ms(99999) == 6000000u / model::kMaxTempoX100);
}

TEST_CASE("dj state (B1): a row 2 pad is held while it is down, LATCH makes a tap stay, one REP is latched a group, unlatching, RESET and leaving end them") {
  DjState dj;
  dj.group[0] = static_cast<std::uint8_t>(DjGroup::A);
  dj.group[1] = static_cast<std::uint8_t>(DjGroup::B);
  CHECK(dj.at_rest());
  CHECK_FALSE(dj.fx_down(12));  // not a pad of the row
  CHECK(dj.fx_down(2));  // A REP 1/16
  CHECK(dj.pad_held());
  CHECK_FALSE(dj.at_rest());
  CHECK(dj.fx_in_force(DjGroup::A, DjFx::Rep16));
  CHECK_FALSE(dj.fx_in_force(DjGroup::B, DjFx::Rep16));
  CHECK_FALSE(dj.fx_in_force(DjGroup::Thru, DjFx::Rep16));
  CHECK(dj.fx_values(0, true).stutter == 8);
  CHECK(dj.fx_values(1, true).stutter == 0);  // the other group
  CHECK(dj.fx_values(0, false).stutter == 0);  // the page is not in view
  CHECK(dj.fx_values(2, true).stutter == 0);   // a THRU track
  CHECK(dj.fx_up(2));
  CHECK_FALSE(dj.fx_up(2));
  CHECK(dj.at_rest());
  // two REP pads down: the later one decides, and the first is back when the later is let go
  CHECK(dj.fx_down(0));
  CHECK(dj.fx_down(3));
  DjFx kind = DjFx::Echo;
  CHECK(dj.rep_pick(DjGroup::A, kind));
  CHECK(kind == DjFx::Rep32);
  CHECK(dj.fx_up(3));
  CHECK(dj.rep_pick(DjGroup::A, kind));
  CHECK(kind == DjFx::Rep4);
  CHECK(dj.fx_up(0));
  CHECK_FALSE(dj.rep_pick(DjGroup::A, kind));
  // LATCH: a tap stays and a second tap ends it; a REP latched replaces the other REP of its group only
  dj.toggle_latch();
  CHECK(dj.fx_down(1));  // A REP 1/8
  CHECK(dj.fx_up(1) == false);
  CHECK(dj.fx_in_force(DjGroup::A, DjFx::Rep8));
  CHECK(dj.fx_down(4));  // A ECHO
  CHECK(dj.fx_down(6));  // B CUT
  CHECK(dj.fx_down(2));  // A REP 1/16 replaces REP 1/8
  CHECK_FALSE(dj.fx_in_force(DjGroup::A, DjFx::Rep8));
  CHECK(dj.fx_in_force(DjGroup::A, DjFx::Rep16));
  CHECK(dj.fx_in_force(DjGroup::A, DjFx::Echo));
  CHECK(dj.fx_in_force(DjGroup::B, DjFx::Cut));
  CHECK(dj.fx_down(11));  // B REP 1/4: group B's own REP, A's stays
  CHECK(dj.fx_in_force(DjGroup::A, DjFx::Rep16));
  CHECK(dj.fx_in_force(DjGroup::B, DjFx::Rep4));
  CHECK(dj.fx_down(6));  // a second tap ends B CUT
  CHECK_FALSE(dj.fx_in_force(DjGroup::B, DjFx::Cut));
  const DjState::FxValues v = dj.fx_values(0, true);
  CHECK(v.stutter == 8);
  CHECK(v.delay == 100);
  CHECK(v.gain == 0);  // the cut waits for the beat (fx_sync)
  dj.toggle_latch();   // unlatching ends every latched FX
  CHECK(dj.fx_on() == 0u);
  CHECK(dj.at_rest());
  // RESET and leaving end held and latched pads
  CHECK(dj.fx_down(5));
  CHECK(dj.reset());
  CHECK(dj.fx_on() == 0u);
  dj.toggle_latch();
  CHECK(dj.fx_down(5));
  CHECK(dj.fx_down(7));
  dj.leave();
  CHECK(dj.fx_on() == 0u);
  CHECK(dj.group_of(0) == DjGroup::A);  // the groups stay
}

TEST_CASE("dj state (B1): ECHO opens the send at once and cuts the group's dry level one beat later, the other group and THRU are not touched, and ending ECHO ends both") {
  DjState dj;
  dj.group[0] = static_cast<std::uint8_t>(DjGroup::A);
  dj.group[1] = static_cast<std::uint8_t>(DjGroup::B);
  CHECK(dj.fx_down(4));  // A ECHO
  dj.fx_sync(1000, 500);
  DjState::FxValues v = dj.fx_values(0, true);
  CHECK(v.delay == 100);
  CHECK(v.gain == 0);
  dj.fx_sync(1499, 500);
  CHECK(dj.fx_values(0, true).gain == 0);
  dj.fx_sync(1500, 500);  // one beat
  v = dj.fx_values(0, true);
  CHECK(v.delay == 100);
  CHECK(v.gain == -100);
  CHECK(dj.fx_values(1, true).delay == 0);  // group B
  CHECK(dj.fx_values(1, true).gain == 0);
  CHECK(dj.fx_values(5, true).delay == 0);  // THRU
  CHECK(dj.fx_up(4));
  dj.fx_sync(1600, 500);
  v = dj.fx_values(0, true);
  CHECK(v.delay == 0);
  CHECK(v.gain == 0);
  // the next ECHO is timed from its own start, and the clock may wrap
  CHECK(dj.fx_down(7));  // B ECHO
  dj.fx_sync(0xFFFFFF00u, 500);
  CHECK(dj.fx_values(1, true).gain == 0);
  dj.fx_sync(0xFFFFFF00u + 499u, 500);
  CHECK(dj.fx_values(1, true).gain == 0);
  dj.fx_sync(0xFFFFFF00u + 500u, 500);  // (wraps past 2^32)
  CHECK(dj.fx_values(1, true).gain == -100);
  // CUT alone is at once, and its release is at once
  CHECK(dj.fx_down(5));  // A CUT
  CHECK(dj.fx_values(0, true).gain == -100);
  CHECK(dj.fx_up(5));
  CHECK(dj.fx_values(0, true).gain == 0);
}

TEST_CASE("perform bank (B1): the DJ layer wins over the slots' FX on any track, the slot's value is told again when it ends, and the layer is not part of the project's state") {
  seq::PerformBank bank;
  DiscardCommands out;
  CHECK(bank.toggle_track(3, out));
  bank.set_edit(true);  // the encoder stores the value in the row
  CHECK(bank.pad_down(18, out));  // slot 6 (DELAY), row 1
  CHECK(bank.turn(5, out));
  bank.pad_up(18, out);
  bank.set_edit(false);
  const int slot_value = bank.live(6);
  NEED(slot_value != 0);
  (void)out.got.size();
  out.got.clear();
  const model::PerfState before = bank.state();
  // the layer opens the send whole on the affected track 3 and a track the slots do not affect
  CHECK(bank.set_dj_layer(3, 0, 100, 0));
  CHECK(bank.set_dj_layer(5, 8, 100, -100));
  CHECK_FALSE(bank.set_dj_layer(5, 8, 100, -100));  // nothing changed
  CHECK(bank.apply(out));
  std::vector<seq::Command> got = out.got;
  out.got.clear();
  CHECK(got.size() == 4);
  bool seen_send3 = false;
  bool seen_stutter5 = false;
  bool seen_send5 = false;
  bool seen_gain5 = false;
  for (const seq::Command& c : got) {
    seen_send3 = seen_send3 || is_control(c, 3, seq::FxGroup::DelaySend, 100);
    seen_stutter5 = seen_stutter5 || is_stutter(c, 5, 8);
    seen_send5 = seen_send5 || is_control(c, 5, seq::FxGroup::DelaySend, 100);
    seen_gain5 = seen_gain5 || is_control(c, 5, seq::FxGroup::Volume, -100);
  }
  CHECK(seen_send3);
  CHECK(seen_stutter5);
  CHECK(seen_send5);
  CHECK(seen_gain5);
  // stored state: the layer is not in it
  const model::PerfState during = bank.state();
  CHECK(std::memcmp(&before, &during, sizeof before) == 0);
  // the layer ends: track 3 has the slot's DELAY again, track 5 has nothing
  CHECK(bank.set_dj_layer(3, 0, 0, 0));
  CHECK(bank.set_dj_layer(5, 0, 0, 0));
  CHECK(bank.apply(out));
  got = out.got;
  out.got.clear();
  CHECK(got.size() == 4);
  seen_send3 = false;
  seen_stutter5 = false;
  seen_send5 = false;
  seen_gain5 = false;
  for (const seq::Command& c : got) {
    seen_send3 = seen_send3 || is_control(c, 3, seq::FxGroup::DelaySend, slot_value);
    seen_stutter5 = seen_stutter5 || is_stutter(c, 5, 0);
    seen_send5 = seen_send5 || is_control(c, 5, seq::FxGroup::DelaySend, 0, true);
    seen_gain5 = seen_gain5 || is_control(c, 5, seq::FxGroup::Volume, 0, true);
  }
  CHECK(seen_send3);
  CHECK(seen_stutter5);
  CHECK(seen_send5);
  CHECK(seen_gain5);
  // bad tracks and out-of-range values are held
  CHECK_FALSE(bank.set_dj_layer(seq::kPerfTracks, 8, 100, -100));
  CHECK(bank.set_dj_layer(1, 99, 500, -500));
  CHECK(bank.apply(out));
  bool ok = false;
  for (const seq::Command& c : out.got) {
    ok = ok || is_stutter(c, 1, 15) || is_control(c, 1, seq::FxGroup::DelaySend, 100) || is_control(c, 1, seq::FxGroup::Volume, -100);
  }
  CHECK(ok);
}

TEST_CASE("dj page (B1): REP, ECHO and CUT act on the tracks of the pad's group only, a THRU track is never touched, and the right half is the mirror") {
  Rig rig;
  rig.to_dj();
  rig.group_tap(0);  // track 1 -> A
  rig.group_tap(1);  // track 2 -> A
  rig.group_tap(4);
  rig.group_tap(4);  // track 5 -> B
  // track 3 stays THRU
  rig.play(1);
  (void)rig.perf();
  PadRole roles[48];
  NEED(rig.app->pad_lights(roles));
  for (unsigned c = 0; c < app::kDjFxPads; ++c) {
    CHECK(roles[app::kDjFxPad + c] == PadRole::Back);
  }
  // A REP 1/16 (pad 3 of row 2): a stutter of one step on tracks 1 and 2
  rig.pad_down(app::kDjFxPad + 2);
  std::vector<seq::Command> got = rig.perf();
  CHECK(got.size() == 2);
  CHECK(is_stutter(got[0], 0, 8));
  CHECK(is_stutter(got[1], 1, 8));
  CHECK(count_for(got, 2) == 0);
  CHECK(count_for(got, 4) == 0);
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[app::kDjFxPad + 2] == PadRole::Front);
  // B REP 1/4 (pad 12 of row 2, the far right): track 5 only, A's stays
  rig.pad_down(app::kDjFxPad + 11);
  got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_stutter(got[0], 4, 5));
  // the releases end each
  rig.pad_up(app::kDjFxPad + 2);
  got = rig.perf();
  CHECK(got.size() == 2);
  CHECK(is_stutter(got[0], 0, 0));
  CHECK(is_stutter(got[1], 1, 0));
  rig.pad_up(app::kDjFxPad + 11);
  got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_stutter(got[0], 4, 0));
  // CUT: A's is pad 6, B's is pad 7; the group is silent while the pad is down
  rig.pad_down(app::kDjFxPad + 5);
  got = rig.perf();
  CHECK(got.size() == 2);
  CHECK(is_control(got[0], 0, seq::FxGroup::Volume, -100));
  CHECK(is_control(got[1], 1, seq::FxGroup::Volume, -100));
  rig.pad_up(app::kDjFxPad + 5);
  got = rig.perf();
  CHECK(got.size() == 2);
  CHECK(is_control(got[0], 0, seq::FxGroup::Volume, 0, true));
  rig.pad_down(app::kDjFxPad + 6);
  got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_control(got[0], 4, seq::FxGroup::Volume, -100));
  rig.pad_up(app::kDjFxPad + 6);
  got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_control(got[0], 4, seq::FxGroup::Volume, 0, true));
  // the crossfader and the filter are untouched by any of it
  CHECK(rig.dj().pos == app::kDjCentre);
  CHECK(rig.dj().filt == 0);
  // a track that joins the group while a pad is down is told at the next pass (the layer follows the groups)
  rig.pad_down(app::kDjFxPad + 4);  // A ECHO
  (void)rig.perf();
  rig.group_tap(2);  // track 3 -> A
  got = rig.perf();
  bool track3 = false;
  for (const seq::Command& c : got) {
    track3 = track3 || is_control(c, 2, seq::FxGroup::DelaySend, 100);
  }
  CHECK(track3);
  rig.pad_up(app::kDjFxPad + 4);
}

TEST_CASE("dj page (B1): ECHO opens the group's send at once, cuts the group one beat later and gives it all back at the release; at DELAY level 0 the strip says so") {
  Rig rig;
  rig.to_dj();
  rig.group_tap(0);  // track 1 -> A
  rig.play(1, 12000);  // 120 BPM: a beat is 500 ms
  (void)rig.perf();
  const unsigned posts = rig.app->state().notify.posts();
  NEED(rig.project().master().delay_volume > 0u);
  rig.pad_down(app::kDjFxPad + 4);
  std::vector<seq::Command> got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_control(got[0], 0, seq::FxGroup::DelaySend, 100));
  CHECK(rig.app->state().notify.posts() == posts);  // the master DELAY level is up: no warning
  rig.pass(200);
  rig.pass(200);
  CHECK(rig.perf().empty());  // 400 ms: the beat is not over
  rig.pass(200);              // 600 ms
  got = rig.perf();
  NEED(got.size() == 1);
  CHECK(is_control(got[0], 0, seq::FxGroup::Volume, -100));
  rig.pass(500);
  CHECK(rig.perf().empty());  // told once
  rig.pad_up(app::kDjFxPad + 4);
  got = rig.perf();
  CHECK(got.size() == 2);
  bool volume_end = false;
  bool delay_end = false;
  for (const seq::Command& c : got) {
    volume_end = volume_end || is_control(c, 0, seq::FxGroup::Volume, 0, true);
    delay_end = delay_end || is_control(c, 0, seq::FxGroup::DelaySend, 0, true);
  }
  CHECK(volume_end);
  CHECK(delay_end);
  // a release inside the beat never cuts
  rig.pad_down(app::kDjFxPad + 4);
  (void)rig.perf();
  rig.pass(300);
  rig.pad_up(app::kDjFxPad + 4);
  got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_control(got[0], 0, seq::FxGroup::DelaySend, 0, true));
  // the master DELAY level is 0: the strip says so, and the send is still opened
  model::MasterSettings m = rig.project().master();
  m.delay_volume = 0;
  CHECK(rig.project().set_master(m) == model::Status::Ok);
  rig.pad_down(app::kDjFxPad + 4);
  CHECK(std::string(rig.app->state().notify.last_text()) == "DELAY LEVEL IS 0");
  got = rig.perf();
  CHECK(got.size() == 1);
  rig.pad_up(app::kDjFxPad + 4);
  // a CUT pad says nothing about the delay
  const unsigned posts2 = rig.app->state().notify.posts();
  rig.pad_down(app::kDjFxPad + 5);
  CHECK(rig.app->state().notify.posts() == posts2);
  rig.pad_up(app::kDjFxPad + 5);
}

TEST_CASE("dj page (B1): LATCH keeps the row 2 pads, unlatching, RESET and leaving the page end every one, a pad let go after leaving ends nothing twice, and nothing outlasts the page") {
  Rig rig;
  rig.to_dj();
  rig.group_tap(0);  // track 1 -> A
  rig.play(1);
  (void)rig.perf();
  // LATCH: a tap stays
  rig.tap(Button::F6);
  rig.pad_down(app::kDjFxPad + 1);  // A REP 1/8
  rig.pad_up(app::kDjFxPad + 1);
  std::vector<seq::Command> got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_stutter(got[0], 0, 7));
  CHECK(rig.dj().fx_latched == 2u);
  // another REP replaces it, ECHO and CUT add to it
  rig.pad_tap(app::kDjFxPad + 2);
  got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_stutter(got[0], 0, 8));
  rig.pad_tap(app::kDjFxPad + 5);  // CUT
  got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_control(got[0], 0, seq::FxGroup::Volume, -100));
  PadRole roles[48];
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[app::kDjFxPad + 2] == PadRole::Front);
  CHECK(roles[app::kDjFxPad + 5] == PadRole::Front);
  CHECK(roles[app::kDjFxPad + 1] == PadRole::Back);
  // a second tap on CUT ends it
  rig.pad_tap(app::kDjFxPad + 5);
  got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_control(got[0], 0, seq::FxGroup::Volume, 0, true));
  // unlatching ends the rest
  rig.tap(Button::F6);
  got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_stutter(got[0], 0, 0));
  CHECK(rig.dj().fx_on() == 0u);
  // RESET (F8) ends a held pad
  rig.pad_down(app::kDjFxPad + 5);
  (void)rig.perf();
  rig.tap(Button::F8);
  got = rig.perf();
  CHECK(got.size() == 1);
  CHECK(is_control(got[0], 0, seq::FxGroup::Volume, 0, true));
  CHECK(rig.dj().fx_held == 0u);
  rig.pad_up(app::kDjFxPad + 5);  // the pad's release finds nothing
  CHECK(rig.perf().empty());
  // leaving the page ends a held pad and a latched one, the groups stay, and the late release does nothing
  rig.pad_down(app::kDjFxPad + 3);  // A REP 1/32, held
  rig.tap(Button::F6);
  rig.pad_tap(app::kDjFxPad + 5);   // CUT, latched
  (void)rig.perf();
  CHECK(rig.dj().fx_on() != 0u);
  rig.tap(Button::Performance);  // the slots page
  rig.pass(2);
  got = rig.perf();
  CHECK(rig.dj().fx_on() == 0u);
  CHECK(rig.dj().latch == false);
  CHECK(rig.dj().group_of(0) == DjGroup::A);
  CHECK(got.size() == 2);
  bool stutter_end = false;
  bool volume_end = false;
  for (const seq::Command& c : got) {
    stutter_end = stutter_end || is_stutter(c, 0, 0);
    volume_end = volume_end || is_control(c, 0, seq::FxGroup::Volume, 0, true);
  }
  CHECK(stutter_end);
  CHECK(volume_end);
  rig.pad_up(app::kDjFxPad + 3);
  CHECK(rig.perf().empty());
  // on the slots page a row 2 pad is the slots' pad, and the DJ layer does not come back
  rig.pad_tap(app::kDjFxPad + 5);
  rig.pass(2);
  CHECK(rig.dj().fx_on() == 0u);
  for (const seq::Command& c : rig.perf()) {
    CHECK(c.kind != seq::Command::Kind::PerfStutter);
  }
}

// ---- B2: TRANS HP, TRANS LP and TRANS ECHO -------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

// The platform's transport as far as the queue goes: a command on the sequencer's ring.
struct SeqTransport final : app::TransportSink {
  seq::CommandRing& ring;
  unsigned refuse = 0;  // the next `refuse` commands are refused (a full ring)
  explicit SeqTransport(seq::CommandRing& r) : ring(r) {}
  bool play(unsigned) noexcept override { return false; }
  bool play_song(unsigned) noexcept override { return false; }
  bool stop() noexcept override { return false; }
  bool refuse_jump = false;  // the live jumps are refused (the pick's commands go)
  bool queue_jump(unsigned row) noexcept override { return !refuse_jump && take() && ring.push(seq::Command{seq::Command::Kind::QueueJump, 0, static_cast<std::uint16_t>(row < 0xFFFFu ? row : 0xFFFFu), 0}); }
  bool queue_pattern(unsigned id) noexcept override { return take() && ring.push(seq::Command{seq::Command::Kind::QueuePattern, static_cast<std::uint8_t>(id), 0, 0}); }
  bool take() noexcept { return refuse == 0u || (--refuse, false); }
};

// A real sequencer with the App's pass around it: read the position, run the transition's timer, one block.
struct TransRun {
  seq::Sequencer& s;
  SeqTransport tr;
  DjState dj;
  std::uint8_t queued = 0;  // PatternEdit::queued_pattern: the App clears it when the pattern shows
  unsigned steps;
  std::uint64_t t = 0;
  audio::SeqEvent ev[audio::kMaxEventsPerBlock];
  TransRun(seq::Sequencer& seq_in, unsigned loop_steps) : s(seq_in), tr(seq_in.commands()), steps(loop_steps) {}
  audio::PlayPosition::Value pos() const { return s.play_position().read(); }
  void block() {
    const audio::PlayPosition::Value v = pos();
    if (queued != 0u && v.pattern == queued) {
      queued = 0;
    }
    app::dj_trans_publish(dj, v, steps, queued, &tr, test::seq_project().order_ops());
    (void)s.step(t, 64, ev, audio::kMaxEventsPerBlock);
    t += 64;
  }
  // Blocks until the playhead is on `step` of the loop (never past `max_blocks`).
  void to_step(unsigned step, unsigned max_blocks = 20000) {
    for (unsigned i = 0; i < max_blocks && !(pos().playing && pos().step == step); ++i) {
      block();
    }
  }
  void tap(DjTrans kind, unsigned next_row = 0xFFFFu) { CHECK(app::dj_trans_start(dj, kind, pos(), steps, queued, next_row, tr, test::seq_project().order_ops())); }
  void pick(unsigned id) {  // row 1: the PATS tap
    CHECK(tr.queue_pattern(id));
    queued = static_cast<std::uint8_t>(id);
  }
};

seq::Sequencer& trans_sequencer(unsigned len1, unsigned len2, bool song = false) {
  test::fresh_project(4, 4);
  model::Project& p = test::seq_project();
  CHECK(p.set_tempo_x100(12000) == model::Status::Ok);
  CHECK(p.create_pattern(1, len1) == model::Status::Ok);
  CHECK(p.create_pattern(2, len2) == model::Status::Ok);
  if (song) {
    CHECK(p.append_order(1) == model::Status::Ok);
    CHECK(p.append_order(2) == model::Status::Ok);
  }
  static test::StaticSlot<seq::Sequencer> slot;
  seq::Sequencer& s = slot.make(p);
  s.configure(seq::Sequencer::Config{});
  if (song) {
    s.play_song(0);
  } else {
    s.play_pattern(1);
  }
  return s;
}

constexpr unsigned kBlocksPerStep = 86;  // 120 BPM: a step is 0.125 s = 5512 samples

}  // namespace

TEST_CASE("dj maths (B2): the loop in microsteps, and the loops a switch is held back so that a sweep lasts a bar") {
  CHECK(app::dj_loop_len(16) == 16u * 48u);
  CHECK(app::dj_loop_len(0) == 48u);  // held to 1..128
  CHECK(app::dj_loop_len(500) == 128u * 48u);
  CHECK(app::dj_loop_pos(3, 5) == 3u * 48u + 5u);
  CHECK(app::dj_loop_pos(3, 200) == 3u * 48u + 47u);
  const unsigned bar = 16u * 48u;
  CHECK(app::dj_trans_loops(bar, 32u * 48u) == 0u);          // exactly a bar left
  CHECK(app::dj_trans_loops(bar - 1u, 32u * 48u) == 1u);     // one microstep short: one more loop
  CHECK(app::dj_trans_loops(0, 32u * 48u) == 1u);
  CHECK(app::dj_trans_loops(2u * 48u, 8u * 48u) == 2u);      // an 8-step loop with 2 steps left: 14 steps short, two loops of 8
  CHECK(app::dj_trans_loops(8u * 48u, 8u * 48u) == 1u);
  CHECK(app::dj_trans_loops(8u * 48u, 4u * 48u) == 2u);
  CHECK(app::dj_trans_loops(100u * 48u, 4u * 48u) == 0u);
}

TEST_CASE("dj trans (B2): a sweep runs from the tap to the end of the loop against the play position, then the pattern switches and the filter snaps back") {
  for (const DjTrans kind : {DjTrans::Hp, DjTrans::Lp}) {
    TransRun r(trans_sequencer(32, 32), 32);
    r.to_step(3);
    r.pick(2);
    const std::uint64_t tap_at = r.t;
    r.tap(kind);
    CHECK(r.dj.trans.kind == kind);
    CHECK_FALSE(r.dj.trans.owes);
    CHECK(r.dj.trans.loops == 0u);  // 29 steps left: more than a bar
    int prev = 0;
    int at_half = -1;
    std::uint64_t switched = 0;
    bool snapped = false;
    for (unsigned i = 0; i < 4000u * 2u && r.dj.trans.kind != DjTrans::None; ++i) {
      r.block();
      if (r.dj.trans.kind == DjTrans::None) {
        break;  // this pass saw the switch: the filter is back at 0
      }
      const int f = r.dj.trans_filter();
      CHECK_MESSAGE(std::abs(f) >= std::abs(prev), "the sweep never goes back");
      CHECK_MESSAGE((kind == DjTrans::Hp ? f >= 0 : f <= 0), "HP sweeps up, LP sweeps down");
      prev = f;
      if (at_half < 0 && r.t - tap_at >= 29u * 5512u / 2u) {
        at_half = std::abs(f);
      }
      if (switched == 0u && r.pos().pattern == 2u) {
        switched = r.t;
      }
    }
    snapped = r.dj.trans.snap;
    CHECK(r.dj.trans.kind == DjTrans::None);
    CHECK(snapped);
    CHECK(r.dj.trans_filter() == 0);
    CHECK(std::abs(prev) >= 97);  // it got (almost) to the end of the travel before the switch
    CHECK((at_half >= 47 && at_half <= 53));
    NEED(switched != 0u);
    CHECK(r.pos().pattern == 2u);
    const std::uint64_t len = r.t - tap_at;
    CHECK((len + 3u * 64u >= 29u * 5512u && len <= 29u * 5512u + 4u * 64u));  // the 29 steps that were left, to a few blocks
    CHECK(r.queued == 0u);
  }
}

TEST_CASE("dj trans (B2): with less than a bar left the pick is taken off and the switch waits one more loop; the sweep lasts the rest plus the loop") {
  TransRun r(trans_sequencer(32, 32), 32);
  r.to_step(24);  // 8 steps left
  r.pick(2);
  r.tap(DjTrans::Hp);
  CHECK(r.dj.trans.loops == 1u);
  CHECK(r.dj.trans.owes);
  const std::uint64_t tap_at = r.t;
  std::uint64_t first_end = 0;
  for (unsigned i = 0; i < 12000u && r.dj.trans.kind != DjTrans::None; ++i) {
    r.block();
    if (first_end == 0u && r.dj.trans.wraps == 1u) {
      first_end = r.t;
      CHECK(r.pos().pattern == 1u);  // the first loop's end did NOT switch
      CHECK_FALSE(r.dj.trans.owes);  // the pick is back on the sequencer's queue
      CHECK(r.dj.trans.pct < 40);
    }
  }
  NEED(first_end != 0u);
  CHECK(r.dj.trans.kind == DjTrans::None);
  CHECK(r.pos().pattern == 2u);
  CHECK(r.dj.trans.snap);
  const std::uint64_t len = r.t - tap_at;
  CHECK(len + 4u * 64u >= (8u + 32u) * 5512u);  // 8 steps + one more loop of 32
  CHECK(len >= 16u * 5512u);                     // at least a bar, as the spec says
  CHECK(r.queued == 0u);
}

TEST_CASE("dj trans (B2): a loop shorter than a bar is held back until the sweep lasts a bar") {
  TransRun r(trans_sequencer(8, 8), 8);
  r.to_step(6);  // 2 steps left of an 8-step loop: 14 short of a bar
  r.pick(2);
  r.tap(DjTrans::Lp);
  CHECK(r.dj.trans.loops == 2u);
  const std::uint64_t tap_at = r.t;
  for (unsigned i = 0; i < 6000u && r.dj.trans.kind != DjTrans::None; ++i) {
    r.block();
  }
  CHECK(r.pos().pattern == 2u);
  const std::uint64_t len = r.t - tap_at;
  CHECK(len + 4u * 64u >= (2u + 16u) * 5512u);
  CHECK(len >= 16u * 5512u);
}

TEST_CASE("dj trans (B2): the same pad again cancels, the filter goes back and the held-back pick returns to the queue, so the switch still comes at the loop's end") {
  TransRun r(trans_sequencer(32, 32), 32);
  r.to_step(24);
  r.pick(2);
  r.tap(DjTrans::Hp);
  for (unsigned i = 0; i < 2u * kBlocksPerStep; ++i) {
    r.block();
  }
  CHECK(r.dj.trans_filter() != 0);
  r.dj.trans_end(false);  // what the pad's second tap does
  CHECK(r.dj.trans_filter() == 0);
  CHECK(r.dj.trans.owes);
  r.block();  // the next pass gives the pick back
  CHECK_FALSE(r.dj.trans.owes);
  CHECK(r.queued == 2u);
  for (unsigned i = 0; i < 6u * kBlocksPerStep + 40u; ++i) {
    r.block();
  }
  CHECK(r.pos().pattern == 2u);  // the first loop's end switched, as it would have without the transition
  CHECK(r.dj.trans.kind == DjTrans::None);
}

TEST_CASE("dj trans (B2): ECHO opens in the last beat before the switch and closes at it; it never waits for a bar") {
  TransRun r(trans_sequencer(32, 32), 32);
  r.to_step(10);
  r.pick(2);
  r.tap(DjTrans::Echo);
  CHECK(r.dj.trans.loops == 0u);
  unsigned opened_at_step = 99;
  for (unsigned i = 0; i < 4000u * 2u && r.dj.trans.kind != DjTrans::None; ++i) {
    r.block();
    if (r.dj.trans.echo && opened_at_step == 99u) {
      opened_at_step = r.dj.trans.last / 48u;  // the step the timer saw
      CHECK(r.dj.fx_values(0, true).delay == 100);  // the whole mix: a THRU track too
      CHECK(r.dj.fx_values(11, true).delay == 100);
    }
    CHECK_MESSAGE((r.dj.trans.echo ? r.dj.trans.last / 48u >= 28u : true), "never before the last beat");
  }
  CHECK(opened_at_step == 28u);  // 4 steps (a beat) before the end of a 32-step loop
  CHECK(r.pos().pattern == 2u);
  CHECK(r.dj.fx_values(0, true).delay == 0);
  CHECK(r.dj.fx_values(0, true).gain == 0);  // the dry level is never cut: the new pattern enters clean
  // tapped inside the last beat it opens at once
  TransRun q(trans_sequencer(32, 32), 32);
  q.to_step(30);
  q.pick(2);
  q.tap(DjTrans::Echo);
  q.block();
  CHECK(q.dj.trans.echo);
}

TEST_CASE("dj trans (B2): in Song mode the held-back loop is a live jump to the slot that plays, then the real queue; the song order is kept") {
  TransRun r(trans_sequencer(16, 16, true), 16);
  r.to_step(12);  // 4 steps left, song slot 0 (pattern 1)
  CHECK(r.pos().order == 0u);
  r.tap(DjTrans::Hp, 1);  // nothing queued: the Song order's next slot
  CHECK(r.dj.trans.loops == 1u);
  CHECK(r.dj.trans.hold == 0u);
  for (unsigned i = 0; i < 16u * kBlocksPerStep && r.dj.trans.wraps == 0u; ++i) {
    r.block();
  }
  CHECK(r.dj.trans.wraps == 1u);
  CHECK(r.pos().pattern == 1u);  // pattern 1 played once more instead of moving on
  CHECK(r.pos().order == 0u);
  for (unsigned i = 0; i < 40u * kBlocksPerStep && r.dj.trans.kind != DjTrans::None; ++i) {
    r.block();
  }
  CHECK(r.dj.trans.kind == DjTrans::None);
  CHECK(r.pos().pattern == 2u);
  CHECK(r.pos().order == 1u);
}

TEST_CASE("dj trans (B2): a queue the sink refuses is told again by the next pass; a stop ends the transition and owes nothing") {
  TransRun r(trans_sequencer(32, 32), 32);
  r.to_step(24);
  r.pick(2);
  r.tap(DjTrans::Hp);
  r.tr.refuse = 1000;  // the ring is full at the loop's end
  for (unsigned i = 0; i < 8u * kBlocksPerStep + 40u; ++i) {
    r.block();
  }
  CHECK(r.dj.trans.owes);
  CHECK(r.pos().pattern == 1u);
  r.tr.refuse = 0;
  r.block();
  CHECK_FALSE(r.dj.trans.owes);
  CHECK(r.queued == 2u);
  r.s.stop();
  for (unsigned i = 0; i < 3u; ++i) {
    r.block();
  }
  CHECK(r.dj.trans.kind == DjTrans::None);
  CHECK_FALSE(r.dj.trans.owes);
}

// Song mode with a third pattern to pick: order 1, 2, 2 (the slots after the first play 2), pattern 3 the pick.
seq::Sequencer& song_pick_sequencer(unsigned len) {
  seq::Sequencer& s = trans_sequencer(len, len, true);
  model::Project& p = test::seq_project();
  CHECK(p.create_pattern(3, len) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  return s;
}

TEST_CASE("dj trans (B2 fold): in Song mode a pick given back by a cancel or RESET is not lost to the hold jump, and no stale jump stays") {
  for (const bool reset : {false, true}) {
    TransRun r(song_pick_sequencer(16), 16);
    r.to_step(12);  // 4 steps left, slot 0 (pattern 1)
    r.pick(3);
    r.tap(DjTrans::Hp);
    CHECK(r.dj.trans.loops == 1u);
    CHECK(r.dj.trans.hold == 0u);
    r.block();
    CHECK(r.pos().queued == 0u);  // the hold jump waits
    if (reset) {
      (void)r.dj.reset();
    } else {
      r.dj.trans_end(false);  // the pad's second tap
    }
    r.block();
    r.block();
    CHECK_FALSE(r.dj.trans.owes);
    CHECK(r.queued == 3u);
    CHECK(r.pos().queued == audio::PlayPosition::kNoOrder);  // the hold jump is away
    for (unsigned i = 0; i < 6u * kBlocksPerStep && r.pos().pattern == 1u; ++i) {
      r.block();
    }
    CHECK(r.pos().pattern == 3u);  // the pick plays after the pattern that was playing
    CHECK(r.pos().order == audio::PlayPosition::kNoOrder);
  }
}

TEST_CASE("dj trans (B2 fold): the pick and a live jump are both given back, the jump winning at the end as before the tap") {
  TransRun r(song_pick_sequencer(16), 16);
  r.to_step(12);
  r.pick(3);
  CHECK(r.tr.queue_jump(2));
  r.block();
  CHECK(r.pos().queued == 2u);
  r.tap(DjTrans::Hp);
  CHECK(r.dj.trans.jump == 2u);
  CHECK(r.dj.trans.pat == 3u);
  r.block();
  r.dj.trans_end(false);
  r.block();
  r.block();
  CHECK_FALSE(r.dj.trans.owes);
  CHECK(r.pos().queued == 2u);  // the jump is back, not the hold
  for (unsigned i = 0; i < 6u * kBlocksPerStep && r.pos().order == 0u; ++i) {
    r.block();
  }
  CHECK(r.pos().order == 2u);
  CHECK(r.pos().pattern == 2u);
}

TEST_CASE("dj trans (B2 fold): a start the sink stops half-way gives back the pick it took, and says nothing started") {
  TransRun r(song_pick_sequencer(16), 16);
  r.to_step(12);
  r.pick(3);
  r.tr.refuse_jump = true;  // the pick goes off, the hold jump is refused
  CHECK_FALSE(app::dj_trans_start(r.dj, DjTrans::Hp, r.pos(), r.steps, r.queued, 0xFFFFu, r.tr));
  CHECK(r.dj.trans.kind == DjTrans::None);
  CHECK(r.dj.trans.owes);  // the pick is owed
  CHECK(r.dj.trans.hold == audio::PlayPosition::kNoOrder);
  r.block();  // the next pass tells the pick again (it has no jump of its own to send)
  CHECK_FALSE(r.dj.trans.owes);
  CHECK(r.queued == 3u);
  r.tr.refuse_jump = false;
  for (unsigned i = 0; i < 6u * kBlocksPerStep && r.pos().pattern == 1u; ++i) {
    r.block();
  }
  CHECK(r.pos().pattern == 3u);  // the pick was not lost
  // nothing taken, nothing owed
  TransRun q(song_pick_sequencer(16), 16);
  q.to_step(12);
  q.tr.refuse_jump = true;
  CHECK_FALSE(app::dj_trans_start(q.dj, DjTrans::Hp, q.pos(), q.steps, q.queued, 1u, q.tr));
  CHECK_FALSE(q.dj.trans.owes);
}

TEST_CASE("dj trans (B2 fold): a tap that raced the pattern's end does not bring the song back to the slot it has left") {
  TransRun r(song_pick_sequencer(16), 16);
  r.to_step(15);
  const audio::PlayPosition::Value stale = r.pos();  // the position the tap read: slot 0, the last step
  CHECK(stale.order == 0u);
  for (unsigned i = 0; i < 4000u && r.pos().order == 0u; ++i) {
    r.block();
  }
  CHECK(r.pos().order == 1u);  // the pattern's end passed before the command went
  CHECK(app::dj_trans_start(r.dj, DjTrans::Hp, stale, r.steps, r.queued, 1u, r.tr));
  CHECK(r.dj.trans.hold == 0u);
  for (unsigned i = 0; i < 3u; ++i) {
    r.block();
  }
  CHECK(r.dj.trans.kind == DjTrans::None);  // the transition is over
  for (unsigned i = 0; i < 2u; ++i) {
    r.block();
  }
  CHECK(r.pos().queued == audio::PlayPosition::kNoOrder);  // the hold jump is away
  for (unsigned i = 0; i < 20u * kBlocksPerStep && r.pos().order == 1u; ++i) {
    r.block();
  }
  CHECK(r.pos().order == 2u);  // the song went on, not back to slot 0
  // the same with the next slot playing the same pattern: the first wrap shows the hold is not where the song is
  TransRun q(song_pick_sequencer(16), 16);
  CHECK(test::seq_project().set_order(0, 2) == model::Status::Ok);  // slots 2, 2, 2
  q.s.play_song(0);
  q.to_step(15);
  const audio::PlayPosition::Value late = q.pos();
  for (unsigned i = 0; i < 4000u && q.pos().order == 0u; ++i) {
    q.block();
  }
  CHECK(app::dj_trans_start(q.dj, DjTrans::Lp, late, q.steps, q.queued, 1u, q.tr));
  for (unsigned i = 0; i < 4u; ++i) {
    q.block();
  }
  CHECK(q.dj.trans.kind == DjTrans::None);
  CHECK(q.pos().queued == audio::PlayPosition::kNoOrder);
}

TEST_CASE("dj trans (E-SONG-07): an edit of the song list ends the transition; the stale index is dropped, the hold jump cancelled and the pick given back") {
  TransRun r(song_pick_sequencer(16), 16);
  r.to_step(2);
  r.pick(3);
  r.tap(DjTrans::Hp, 1u);  // slot 0 held for the extra loops, the pick owed
  CHECK(r.dj.trans.kind == DjTrans::Hp);
  CHECK(r.dj.trans.hold == 0u);
  CHECK(r.dj.trans.owes);
  CHECK(test::seq_project().insert_order(0, 2) == model::Status::Ok);  // the held slot is now index 1
  r.block();
  CHECK(r.dj.trans.kind == DjTrans::None);
  CHECK(r.dj.trans.jump == audio::PlayPosition::kNoOrder);
  CHECK(r.dj.trans.hold == audio::PlayPosition::kNoOrder);
  CHECK_FALSE(r.dj.trans.owes);
  for (unsigned i = 0; i < 3u; ++i) {
    r.block();
  }
  CHECK(r.pos().queued == audio::PlayPosition::kNoOrder);  // no jump to a stale index waits
  CHECK(r.pos().order == 1u);                              // the song still plays its slot, at its new index
  for (unsigned i = 0; i < 20u * kBlocksPerStep && r.pos().pattern != 3u; ++i) {
    r.block();
  }
  CHECK(r.pos().pattern == 3u);  // the pick was given back to the sequencer: it plays at the pattern's end
}

TEST_CASE("dj trans (B2 fold): the count-in is not the loop: a running transition ends and a wrap is not counted from it") {
  TransRun r(trans_sequencer(32, 32), 32);
  r.to_step(24);  // 8 steps left: the switch is held back a loop
  r.pick(2);
  r.tap(DjTrans::Hp);
  CHECK(r.dj.trans.loops == 1u);
  audio::PlayPosition::Value v = r.pos();
  v.count_in = 4;
  v.step = 0;
  v.ustep = 0;
  app::dj_trans_publish(r.dj, v, r.steps, r.queued, &r.tr);
  CHECK(r.dj.trans.kind == DjTrans::None);
  CHECK(r.dj.trans.wraps == 0u);
  CHECK_FALSE(r.dj.trans.owes);
}

TEST_CASE("dj publish (B2 fold): the snap is cleared by the push, a hand-set filter that stays does not leave it for a later RESET") {
  core::ParamRing ring;
  DjState dj;
  dj.filt = 50;
  dj.trans.kind = DjTrans::Hp;
  dj.trans.pct = 80;
  CHECK(app::dj_publish(dj, true, &ring) >= 1u);
  CHECK(dj.filt_sent == 80);
  dj.trans_end(true);  // the switch: the sweep goes, the hand's 50 stays
  CHECK(dj.trans.snap);
  CHECK(app::dj_publish(dj, true, &ring) == 1u);
  CHECK(dj.filt_sent == 50);
  CHECK_FALSE(dj.trans.snap);
  core::ParamChange c{};
  while (ring.pop(c)) {
  }
  (void)dj.reset();
  CHECK(app::dj_publish(dj, true, &ring) == 1u);
  CHECK(ring.pop(c));
  CHECK(c.value == 0.0f);
  CHECK(c.ramp_samples == app::kDjSpringSamples);  // not the 1-sample snap
  // nothing to push: the snap is cleared at once
  DjState e;
  e.trans.snap = true;
  (void)app::dj_publish(e, true, &ring);
  CHECK_FALSE(e.trans.snap);
}

TEST_CASE("dj page (B2): TRANS HP sweeps the engine's filter against the play position, the pad again cancels, RESET and leaving the page end it, and the queue stays") {
  Rig rig;
  rig.to_dj();
  const unsigned len = rig.project().pattern_length(1) != 0u ? rig.project().pattern_length(1) : rig.project().default_pattern_length();
  NEED(len >= 20u);
  const unsigned posts = rig.app->state().notify.posts();
  rig.pad_tap(app::kDjTransPad);  // stopped
  CHECK(rig.dj().trans.kind == DjTrans::None);
  CHECK(rig.app->state().notify.posts() == posts + 1u);  // TRANS NEEDS PLAY
  rig.play(1, 12000);
  rig.pad_tap(5);  // row 1: pattern 6 is queued
  NEED(rig.app->state().edit.queued_pattern == 6u);
  (void)rig.filters();
  rig.pad_tap(app::kDjTransPad);
  CHECK(rig.dj().trans.kind == DjTrans::Hp);
  CHECK(rig.dj().trans.loops == 0u);
  PadRole roles[48];
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[app::kDjTransPad] == PadRole::Front);  // the one in force is lit
  CHECK(roles[app::kDjTransPad + 1] == PadRole::Back);
  CHECK(roles[app::kDjTransPad + 2] == PadRole::Back);
  CHECK(rig.transport.queued.size() == 1u);  // the pick is the sequencer's: not taken off, not asked twice
  rig.position.publish(true, 1, len / 2, 12000);
  rig.pass();
  std::vector<Rig::Told> f = rig.filters();
  NEED(!f.empty());
  CHECK(f.back().value == doctest::Approx(50.0f).epsilon(0.05));
  CHECK(f.back().ramp == app::kDjGlideSamples);
  rig.position.publish(true, 1, len - 1, 12000);
  rig.pass();
  f = rig.filters();
  NEED(!f.empty());
  CHECK(f.back().value >= 95.0f);
  rig.pad_tap(app::kDjTransPad);  // again: cancel
  CHECK(rig.dj().trans.kind == DjTrans::None);
  f = rig.filters();
  NEED(!f.empty());
  CHECK(f.back().value == 0.0f);
  CHECK(f.back().ramp == app::kDjSpringSamples);
  CHECK(rig.app->state().edit.queued_pattern == 6u);  // the queue stays
  // RESET ends a transition
  rig.position.publish(true, 1, 2, 12000);
  rig.pass();
  rig.pad_tap(app::kDjTransPad + 1);  // LP
  CHECK(rig.dj().trans.kind == DjTrans::Lp);
  rig.position.publish(true, 1, 10, 12000);
  rig.pass();
  CHECK(rig.dj().trans_filter() < 0);
  rig.tap(Button::F8);
  CHECK(rig.dj().trans.kind == DjTrans::None);
  f = rig.filters();
  NEED(!f.empty());
  CHECK(f.back().value == 0.0f);
  // leaving the page ends it too, and nothing is stored: the project is as it was
  rig.pad_tap(app::kDjTransPad);
  CHECK(rig.dj().trans.kind == DjTrans::Hp);
  rig.position.publish(true, 1, 14, 12000);
  rig.pass();
  CHECK(rig.dj().trans_filter() > 0);
  rig.tap(Button::Pattern);
  CHECK_FALSE(rig.on_dj());
  CHECK(rig.dj().trans.kind == DjTrans::None);
  CHECK(rig.dj().at_rest());
  f = rig.filters();
  NEED(!f.empty());
  CHECK(f.back().value == 0.0f);
  CHECK(rig.app->state().edit.queued_pattern == 6u);  // a queued pattern stays queued, as with NEXT PAT
}

TEST_CASE("dj page (B2): with nothing picked the Song order's next slot is queued; TRANS ECHO opens the send of every track in the last beat and gives it back at the switch") {
  Rig rig;
  rig.to_dj();
  rig.group_tap(0);  // track 1 -> A, the others THRU
  model::Project& p = rig.project();
  (void)p.clear_order();
  NEED(p.append_order(1) == model::Status::Ok);
  NEED(p.append_order(2) == model::Status::Ok);
  const unsigned len = p.pattern_length(1) != 0u ? p.pattern_length(1) : p.default_pattern_length();
  rig.play(1, 12000);
  (void)rig.perf();
  rig.pad_tap(app::kDjTransPad + 2);  // TRANS ECHO, nothing picked
  CHECK(rig.dj().trans.kind == DjTrans::Echo);
  NEED(rig.transport.jumped.size() == 1u);
  CHECK(rig.transport.jumped[0] == 1u);  // the slot after the one that plays
  CHECK(rig.transport.queued.empty());
  CHECK(rig.perf().empty());  // armed: the send is still closed
  rig.position.publish(true, 1, len - 5, 12000);
  rig.pass();
  CHECK(rig.perf().empty());  // five steps left: not yet the last beat
  rig.position.publish(true, 1, len - 4, 12000);
  rig.pass();
  const std::vector<seq::Command> on = rig.perf();
  CHECK(is_control(on.front(), 0, seq::FxGroup::DelaySend, 100) == true);
  unsigned sends = 0;
  for (const seq::Command& c : on) {
    sends += c.kind == seq::Command::Kind::PerfControl && c.b == static_cast<unsigned>(seq::FxGroup::DelaySend) ? 1u : 0u;
    CHECK_MESSAGE(c.b != static_cast<unsigned>(seq::FxGroup::Volume), "the dry level is not cut");
  }
  CHECK(sends == app::kDjTracks);  // every track of the page, the THRU ones too
  CHECK(count_for(on, 7) == 1u);
  rig.position.publish(true, 6, 0, 12000);  // the switch: the new pattern plays
  rig.pass();
  CHECK(rig.dj().trans.kind == DjTrans::None);
  unsigned ends = 0;
  for (const seq::Command& c : rig.perf()) {
    ends += is_control(c, c.a, seq::FxGroup::DelaySend, 0, true) ? 1u : 0u;
  }
  CHECK(ends == app::kDjTracks);
  CHECK(rig.app->state().edit.queued_pattern == 0u);
}

TEST_CASE("dj page (B2): with no next slot to queue the pad says why and starts nothing") {
  Rig rig;
  rig.to_dj();
  (void)rig.project().clear_order();
  rig.play(1, 12000);
  const unsigned posts = rig.app->state().notify.posts();
  rig.pad_tap(app::kDjTransPad);
  CHECK(rig.dj().trans.kind == DjTrans::None);
  CHECK(rig.app->state().notify.posts() == posts + 1u);
  CHECK(rig.transport.jumped.empty());
}

TEST_CASE("dj page (B2 fold): a TRANS pad during the count-in says why and starts nothing; after it the pad works") {
  Rig rig;
  rig.to_dj();
  rig.play(1, 12000);
  rig.position.publish_count_in(4);
  rig.pass();
  NEED(rig.app->state().play.count_in == 4u);
  const unsigned posts = rig.app->state().notify.posts();
  rig.pad_tap(app::kDjTransPad);
  CHECK(rig.dj().trans.kind == DjTrans::None);
  CHECK(rig.app->state().notify.posts() == posts + 1u);
  CHECK(rig.transport.queued.empty());
  rig.position.publish_count_in(0);
  rig.pass();
  rig.pad_tap(app::kDjTransPad);
  CHECK(rig.dj().trans.kind == DjTrans::Hp);
}

// int.15/int.16: the count-in's strip ("cntin", rewritten every beat) is the head of the queue, so the refusal that was only posted never showed. It is the head now, for its time, and the strip comes back after it.
TEST_CASE("dj page (SMALLFIX11): the TRANS refusal during a count-in is the strip head for its time, then the count-in strip returns") {
  Rig rig;
  rig.to_dj();
  rig.play(1, 12000);
  rig.position.publish_count_in(8);  // the second beat of a two-beat count-in
  rig.pass();
  NEED(rig.app->state().play.count_in == 8u);
  const auto head = [&]() { return std::string(rig.app->state().notify.empty() ? "" : rig.app->state().notify.head().text.c_str()); };
  NEED(head().rfind("RECORDING IN", 0) == 0);
  rig.pad_tap(app::kDjTransPad);
  rig.pass();
  CHECK(head() == "TRANS NEEDS THE COUNT-IN OVER");
  rig.position.publish_count_in(4);  // the next beat: the strip is rewritten behind the refusal and does not take its place
  rig.pass();
  CHECK(head() == "TRANS NEEDS THE COUNT-IN OVER");
  unsigned ms = 0;
  while (head() == "TRANS NEEDS THE COUNT-IN OVER" && ms < 3000u) {  // its normal time: ViewState::kMessageMs
    rig.pass(100);
    ms += 100;
  }
  CHECK(ms >= app::ViewState::kMessageMs - 100u);
  CHECK(ms <= app::ViewState::kMessageMs + 100u);
  CHECK(head().rfind("RECORDING IN", 0) == 0);  // the count-in strip is back
  CHECK(rig.dj().trans.kind == DjTrans::None);
}

// OS-DJ C1: F5 SCR and the TAPE STOP pad take the arena's free tail as the scratch ring, tell the engine (the test plays the render: it applies the ScratchMode ParamChange and runs the stage's blocks) and give the
// tail back when the engine has let go; RESET and leaving the page end it; a second owner of the tail is refused.
namespace {
struct ScratchRig {
  Rig rig;
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  std::vector<std::int16_t> memory = std::vector<std::int16_t>(100000);
  std::vector<std::int16_t> peaks = std::vector<std::int16_t>(2000);
  app::SampleBank bank;
  audio::MasterScratch stage;
  bool bypass = true;
  float last_mode = -1.0f;
  std::uint32_t last_ramp = 0;
  ScratchRig() {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), static_cast<std::uint32_t>(memory.size()), peaks.data(), peaks.size());
    stage.attach(&bypass);
    rig.app->attach_samples(&bank);
    rig.app->attach_scratch(&stage);
    rig.to_dj();
  }
  // The render: the ScratchMode changes in the ring are applied, then `blocks` blocks of silence run through the stage.
  void engine(unsigned blocks = 0) {
    core::ParamChange c{};
    while (rig.ring->pop(c)) {
      if (core::param_kind(c.id) == core::ParamKind::Master && core::param_field(c.id) == audio::graph_param::master::ScratchMode) {
        last_mode = c.value;
        last_ramp = c.ramp_samples;
        (void)stage.set_mode(c.value, c.ramp_samples);
      }
    }
    for (unsigned b = 0; b < blocks; ++b) {
      float l[64] = {}, r[64] = {};
      stage.process(l, r, 64);
    }
  }
  app::DjScr scr() const { return rig.app->state().dj.scr; }
};
}  // namespace

TEST_CASE("dj scratch (C1): F5 takes the tail, tells the engine, and gives the tail back when the engine has let go") {
  ScratchRig t;
  t.rig.tap(Button::F5);
  CHECK(t.scr() == app::DjScr::On);
  CHECK(t.bank.take_held());
  t.engine();
  CHECK(t.last_mode == 1.0f);
  CHECK(t.stage.phase() == audio::MasterScratch::Phase::Platter);
  CHECK_FALSE(t.bypass);
  t.engine(4);
  const auto fader = t.rig.app->state().dj.pos;
  t.rig.turn(3);
  t.rig.pass();
  CHECK(t.rig.app->state().dj.pos == fader);  // the encoder is the platter now, not the fader
  t.rig.tap(Button::F5);
  CHECK(t.scr() == app::DjScr::Ending);
  t.rig.pass();
  t.engine(1);
  CHECK(t.last_mode == 0.0f);
  t.rig.pass();
  CHECK(t.scr() == app::DjScr::Ending);  // the release is still running: the tail is held
  CHECK(t.bank.take_held());
  t.engine(8);
  CHECK(t.stage.idle());
  t.rig.pass();
  CHECK(t.scr() == app::DjScr::Off);
  CHECK_FALSE(t.bank.take_held());
}

TEST_CASE("dj scratch (C1): the TAPE STOP pad is held, its ramp is the beat, SCR on refuses it; RESET and leaving end a scratch") {
  ScratchRig t;
  t.rig.pad_down(app::kDjTapePad);
  CHECK(t.scr() == app::DjScr::Tape);
  t.engine(2);
  CHECK(t.last_mode == 2.0f);
  // 120 BPM: a beat is half a second of the engine's rate, 22,050 samples (22,058 at 44,117.647 Hz): the clock's cycles over a beat, truncated
  CHECK(t.last_ramp == std::uint64_t{o3os::core::kCpuHz / 2u} * o3os::core::kRefCyclesPerSample.den / o3os::core::kRefCyclesPerSample.num);
  t.rig.pad_up(app::kDjTapePad);
  CHECK(t.scr() == app::DjScr::Ending);
  t.rig.pass();
  t.engine(8);
  t.rig.pass();
  CHECK(t.scr() == app::DjScr::Off);
  t.rig.tap(Button::F5);
  CHECK(t.scr() == app::DjScr::On);
  t.engine(2);
  const unsigned posts = t.rig.app->state().notify.posts();
  t.rig.pad_tap(app::kDjTapePad);
  CHECK(t.rig.app->state().notify.posts() == posts + 1u);  // SCRATCH IS ON
  CHECK(t.scr() == app::DjScr::On);
  t.rig.tap(Button::F8);  // RESET
  CHECK(t.scr() == app::DjScr::Ending);
  t.rig.pass();
  t.engine(8);
  t.rig.pass();
  CHECK(t.scr() == app::DjScr::Off);
  CHECK_FALSE(t.bank.take_held());
  t.rig.tap(Button::F5);
  t.engine(2);
  t.rig.tap(Button::Performance);  // leaving the page
  CHECK_FALSE(t.rig.on_dj());
  CHECK(t.scr() == app::DjScr::Ending);
  t.rig.pass();
  t.engine(8);
  t.rig.pass();
  CHECK(t.scr() == app::DjScr::Off);
  CHECK_FALSE(t.bank.take_held());
  CHECK(t.rig.app->state().dj.at_rest());
}

TEST_CASE("dj scratch (OS-DJSTOPLEN): the encoder while the TAPE STOP pad is held sets the stop's length, 1/8 to 4 beats, and the next hold starts with it") {
  ScratchRig t;
  const auto& dj = t.rig.app->state().dj;
  CHECK(dj.stop_len == app::kDjStopLenDefault);
  t.rig.pad_down(app::kDjTapePad);
  t.engine(50);
  const std::uint64_t beat = std::uint64_t{o3os::core::kCpuHz / 2u} * o3os::core::kRefCyclesPerSample.den / o3os::core::kRefCyclesPerSample.num;  // 120 BPM: 22,050 samples (22,058 at 44,117.647 Hz)
  CHECK(t.last_ramp == beat);  // the default is one beat
  const float before = t.stage.rate();
  const auto fader = dj.pos;
  t.rig.turn(1);
  t.rig.pass();
  CHECK(dj.pos == fader);  // the encoder is the stop's length while the pad is held, not the fader
  CHECK(dj.stop_len == 4u);
  t.engine(1);
  CHECK(t.last_mode == 3.0f);
  CHECK(t.last_ramp == 2 * beat);  // two beats
  CHECK(t.stage.rate() == doctest::Approx(before).epsilon(0.02));  // rebased: the rate carries on from where it was
  t.rig.turn(1);
  t.rig.pass();
  t.rig.turn(1);  // held to the end: 4 beats
  t.rig.pass();
  CHECK(dj.stop_len == 5u);
  t.engine(1);
  CHECK(t.last_ramp == 4 * beat);
  t.rig.turn(-9);
  t.rig.pass();
  CHECK(dj.stop_len == 0u);  // held to the other end: 1/8 beat
  t.engine(1);
  CHECK(t.last_ramp == beat / 8u);
  CHECK(t.stage.rate() == doctest::Approx(before).epsilon(0.1f));
  t.engine(60);
  CHECK(t.stage.rate() == 0.0f);  // the short stop is over
  t.rig.pad_up(app::kDjTapePad);
  t.rig.pass();
  t.engine(8);
  t.rig.pass();
  CHECK(t.scr() == app::DjScr::Off);
  t.rig.turn(1);  // the pad is up: the encoder is the fader again
  t.rig.pass();
  CHECK(dj.stop_len == 0u);
  CHECK(dj.pos != fader);
  t.rig.pad_down(app::kDjTapePad);  // the next hold starts with the length it was left at
  t.engine(1);
  CHECK(t.last_mode == 3.0f);
  CHECK(t.last_ramp == beat / 8u);
  CHECK(t.stage.phase() == audio::MasterScratch::Phase::Tape);
}

TEST_CASE("dj scratch (OS-DJSTOPLEN): the lengths are the beat halved or doubled, and mode 3 is refused outside a TAPE STOP") {
  CHECK(app::dj_stop_len_samples(22058u, 0) == 2757u);
  CHECK(app::dj_stop_len_samples(22058u, 1) == 5514u);
  CHECK(app::dj_stop_len_samples(22058u, 2) == 11029u);
  CHECK(app::dj_stop_len_samples(22058u, 3) == 22058u);
  CHECK(app::dj_stop_len_samples(22058u, 4) == 44116u);
  CHECK(app::dj_stop_len_samples(22058u, 5) == 88232u);
  audio::MasterScratch off;
  CHECK_FALSE(off.set_mode(3.0f, 1000u));
}

TEST_CASE("dj scratch (C1): the second owner of the arena tail is refused, and so is a scratch with no room") {
  ScratchRig t;
  std::uint32_t room = 0;
  CHECK(t.bank.take_begin(room) != nullptr);  // the looper's take
  const unsigned posts = t.rig.app->state().notify.posts();
  t.rig.tap(Button::F5);
  CHECK(t.scr() == app::DjScr::Off);
  CHECK(t.rig.app->state().notify.posts() == posts + 1u);  // NO ROOM: LOOPER IS RECORDING
  CHECK(t.bank.take_held());                               // the looper's take is untouched
  t.bank.take_end(0);
  t.rig.tap(Button::F5);  // and the other way round: the ring holds the tail, so the looper's take_begin is refused
  CHECK(t.scr() == app::DjScr::On);
  CHECK(t.bank.take_begin(room) == nullptr);
  CHECK_FALSE(t.stage.stage(nullptr, 100000));  // and the stage itself will not take a second ring while busy
}

// OS-DJ C1 review fold: the scratch ring is the arena's tail, so a project load, an import or a new project (which empty the arena and let their loader write from its start) is refused while SCR or TAPE STOP
// runs and while it ends - the backspin's 0.5 s, which is all that is left once the DJ page has been left for the File screen. The refusal is the File screen's own strip line.
namespace {
class FakeLoad final : public app::ProjectSink {
 public:
  unsigned opens = 0;
  unsigned news = 0;
  unsigned imports = 0;
  Start open(const char*, Format) noexcept override {
    ++opens;
    return Start::Started;
  }
  Start start_new() noexcept override {
    ++news;
    return Start::Started;
  }
  Start start_new_keeping_instruments() noexcept override {
    ++news;
    return Start::Started;
  }
  Start import_module(const char*, app::ImportEngine) noexcept override {
    ++imports;
    return Start::Started;
  }
  bool busy() const noexcept override { return false; }
  const char* folder() const noexcept override { return ""; }
  bool loaded() const noexcept override { return true; }
  const char* detail() const noexcept override { return ""; }
};

struct LoadRig : ScratchRig {
  platform::host::FakeStorage card;
  FakeLoad load;
  LoadRig() {
    card.add_dir("Projects");
    card.add_dir("Projects/Song");
    card.add_file("Projects/Song/project.mt", 100);
    card.add_dir("MOD");
    card.add_file("MOD/kc.mod", 2000);
    rig.app->attach_storage(&card);
    rig.app->attach_project_sink(&load);
  }
  void to_file() {  // no pass in between: the page's scratch is still On
    (void)rig.app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(ScreenId::File));
  }
  void settle() {
    for (unsigned i = 0; i < 2000; ++i) {
      rig.pass();
      const app::DirScan::State st = rig.app->model().files->state();
      if (st != app::DirScan::State::Scanning && st != app::DirScan::State::Idle) {
        return;
      }
    }
    FAIL_CHECK("the scan did not end");
  }
  void act(const char* id) { (void)rig.app->invoke(core::action_id(id)); }
  std::string said() const { return rig.app->state().notify.last_text(); }  // (the news posted last: the earlier SCR ON line may still be on the strip)
  // SCR on, the stage running; then the scratch is ended and the engine runs its release to the end.
  void scr_on() {
    rig.tap(Button::F5);
    engine(2);
    NEED(scr() == app::DjScr::On);
  }
  void scr_over() {
    engine(8);
    rig.pass();
    CHECK(scr() == app::DjScr::Off);
    CHECK_FALSE(bank.take_held());
  }
};
}  // namespace

TEST_CASE("dj scratch (C1 fold): a new project is refused while SCR is on and while it ends, and the arena is untouched") {
  LoadRig t;
  t.scr_on();
  t.to_file();
  t.act("file.new");  // the same frame: SCR is still On
  CHECK(t.said() == "END THE SCRATCH FIRST");
  CHECK(t.scr() == app::DjScr::On);
  CHECK_FALSE(t.rig.app->state().file.new_choice);  // no question was asked
  CHECK(t.load.news == 0u);
  CHECK(t.bank.take_held());
  t.rig.pass();  // the DJ page is out of view: the scratch is ending, and the engine is still on its release
  CHECK(t.scr() == app::DjScr::Ending);
  t.act("file.new");
  CHECK(t.said() == "END THE SCRATCH FIRST");
  CHECK_FALSE(t.rig.app->state().file.new_choice);
  t.engine(1);  // the release has begun (the head is crossfading to live): the render still holds the ring
  CHECK(t.stage.busy());
  t.act("file.new");
  CHECK(t.said() == "END THE SCRATCH FIRST");
  CHECK(t.load.news == 0u);
  CHECK(t.bank.take_held());
  t.scr_over();
  t.act("file.new");  // and once the render has let go the question is asked as always
  CHECK(t.said() != "END THE SCRATCH FIRST");
  CHECK(t.rig.app->state().file.new_choice);
}

TEST_CASE("dj scratch (C1 fold): TAPE STOP held or ending also refuses a new project") {
  LoadRig t;
  t.rig.pad_down(app::kDjTapePad);
  NEED(t.scr() == app::DjScr::Tape);
  t.engine(2);
  t.to_file();
  t.act("file.new");
  CHECK(t.said() == "END THE SCRATCH FIRST");
  CHECK(t.load.news == 0u);
  t.rig.pass();
  t.act("file.new");  // (the pad is still down: leaving the page ends the scratch)
  CHECK(t.said() == "END THE SCRATCH FIRST");
  t.scr_over();
  t.act("file.new");
  CHECK(t.rig.app->state().file.new_choice);
}

TEST_CASE("dj scratch (C1 fold): a project load is refused while the scratch runs or ends") {
  LoadRig t;
  t.scr_on();
  t.to_file();
  t.act("file.open");  // the same frame: SCR is On
  CHECK(t.said() == "END THE SCRATCH FIRST");
  CHECK(t.load.opens == 0u);
  t.rig.pass();
  t.settle();  // the Projects listing, the cursor on Song
  NEED(t.scr() == app::DjScr::Ending);
  t.rig.tap(Button::F1);
  CHECK(t.said() == "END THE SCRATCH FIRST");
  CHECK(t.load.opens == 0u);
  CHECK(t.bank.take_held());
  t.scr_over();
  t.rig.tap(Button::F1);
  CHECK(t.load.opens == 1u);  // the listing's first row is the project: it opens now
}

TEST_CASE("dj scratch (C1 fold): an import is refused while the scratch ends") {
  LoadRig t;
  t.scr_on();
  t.to_file();
  t.rig.pass();
  NEED(t.scr() == app::DjScr::Ending);
  t.rig.tap(Button::F6);  // the IMPORT page, listing the card's root
  t.settle();
  for (unsigned i = 0; i < 6 && std::string(t.rig.app->model().files->index().name(static_cast<unsigned>(t.rig.app->state().file.pos()))) != "MOD"; ++i) {
    t.rig.tap(Button::Down);
  }
  t.rig.tap(Button::F1);  // enter MOD
  t.settle();
  t.rig.tap(Button::F1);  // the module file
  CHECK(t.said() == "END THE SCRATCH FIRST");
  CHECK(t.load.imports == 0u);
  CHECK(t.bank.take_held());
  t.scr_over();
  t.rig.tap(Button::F1);
  CHECK(t.load.imports == 1u);
}

TEST_CASE("dj scratch (C1 fold 2): the scratch owns the arena from SCR on to the end of its release - the test the platform's three load starters ask, the console's `diag load` among them") {
  LoadRig t;
  CHECK_FALSE(t.rig.app->state().dj.scratch_holds_arena());
  t.scr_on();
  CHECK(t.rig.app->state().dj.scratch_holds_arena());
  t.to_file();
  t.rig.pass();  // the DJ page is out of view: the scratch is ending
  NEED(t.scr() == app::DjScr::Ending);
  CHECK(t.rig.app->state().dj.scratch_holds_arena());
  t.engine(1);  // the release has begun: the render still holds the ring
  CHECK(t.stage.busy());
  CHECK(t.rig.app->state().dj.scratch_holds_arena());
  t.scr_over();
  CHECK_FALSE(t.rig.app->state().dj.scratch_holds_arena());
  // the stage busy with the page already Off (a ring the page does not know) is held too
  std::vector<std::int16_t> ring(2u * 5000u);
  CHECK(t.stage.stage(ring.data(), 5000));
  CHECK(t.rig.app->state().dj.scratch_holds_arena());
  t.stage.unstage();
  CHECK_FALSE(t.rig.app->state().dj.scratch_holds_arena());
  LoadRig tape;  // TAPE STOP held
  tape.rig.pad_down(app::kDjTapePad);
  NEED(tape.scr() == app::DjScr::Tape);
  CHECK(tape.rig.app->state().dj.scratch_holds_arena());
}

TEST_CASE("dj state (int.15): the filter in force is the TRANS sweep's while it runs and the hand's value again after it; the hand's value is never overwritten") {
  core::ParamRing ring;
  DjState dj;
  dj.filt = 50;
  CHECK(dj.filt_now() == 50);
  dj.trans.kind = DjTrans::Hp;
  dj.trans.pct = 80;
  CHECK(dj.filt_now() == 80);
  CHECK(dj.filt == 50);
  (void)app::dj_publish(dj, true, &ring);
  CHECK(dj.filt_sent == dj.filt_now());  // what the bar shows is what the engine was told
  dj.trans.kind = DjTrans::Lp;
  dj.trans.pct = 30;
  CHECK(dj.filt_now() == -30);
  (void)app::dj_publish(dj, true, &ring);
  CHECK(dj.filt_sent == dj.filt_now());
  dj.trans_end(true);  // the switch: the filter snaps back to the hand's 50
  CHECK(dj.filt_now() == 50);
  CHECK(dj.filt == 50);
  DjState bare;  // no hand-set filter: the sweep shows on a bar that read OFF, and OFF is back at the snap
  CHECK(bare.filt_now() == 0);
  bare.trans.kind = DjTrans::Hp;
  bare.trans.pct = 100;
  CHECK(bare.filt_now() == 100);
  bare.trans_end(true);
  CHECK(bare.filt_now() == 0);
}

TEST_CASE("dj scratch (C1 fold): SCR is refused while the render still holds a ring, even one the bank no longer knows") {
  ScratchRig t;
  std::vector<std::int16_t> other(2u * 5000u);
  CHECK(t.stage.stage(other.data(), 5000));  // a ring the render has not let go of, with no take behind it in the bank (a clear forgot it)
  CHECK_FALSE(t.bank.take_held());
  t.rig.tap(Button::F5);
  CHECK(t.scr() == app::DjScr::Off);
  CHECK(std::string(t.rig.app->state().notify.last_text()) == "NO ROOM: SCRATCH IS ENDING");  // (0ad289b took the tail, found the stage busy and said NO ROOM FOR THE SCRATCH)
  CHECK_FALSE(t.bank.take_held());
  t.stage.unstage();
  t.rig.tap(Button::F5);
  CHECK(t.scr() == app::DjScr::On);
}
