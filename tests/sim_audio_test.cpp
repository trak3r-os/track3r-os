// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-E2E-2: the real engine in the simulator (sim/include/o3os/sim/sim_audio.h; the scenario commands `audio on`, `render`, `assert audio`, `pad <n> hold|down|up`, `encoder <n> x<k> every <ms>` and
// `document sound`). The cases run scenario text through the real scenario runner and look at the simulator behind it: the pacing of the blocks, what a render is, that a run is the same samples twice, that the
// app's sinks reach the sequencer and the voices, that a project load stops the transport first, and every refusal. The measurements themselves are tests/audio_check_test.cpp; the journeys are
// sim/scenarios/e2e/e2e_audio*.txt (label e2e).
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>
#include <o3os/model/project.h>
#include <o3os/sim/audio_check.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/sim_audio.h>
#include <o3os/sim/simulator.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;

namespace {

bool run_on(sim::Simulator& simulator, const std::string& text, std::string& error) {
  std::vector<sim::ScenarioCommand> cmds;
  if (!sim::parse_scenario(text, cmds, error)) {
    return false;
  }
  simulator.set_scenario_dir(std::filesystem::path(O3_E2E_CARD_DIR).parent_path().string());  // <build>/sim/cards: `card e2e_card`
  return sim::run_scenario(cmds, simulator, [](const std::string&, const sim::Simulator&) { return true; }, error);
}
bool holds(sim::Simulator& simulator, const std::string& text) {
  std::string error;
  const bool ok = run_on(simulator, text, error);
  CHECK_MESSAGE(error.empty(), error);
  return ok;
}
std::string run_error(const std::string& text) {
  auto simulator = std::make_unique<sim::Simulator>();
  std::string error;
  CHECK_FALSE(run_on(*simulator, text, error));
  return error;
}
std::string parse_error(const std::string& text) {
  std::vector<sim::ScenarioCommand> cmds;
  std::string error;
  CHECK_FALSE(sim::parse_scenario(text, cmds, error));
  return error;
}
bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// A simulator with `document sound` and the engine on, one tick in.
std::unique_ptr<sim::Simulator> sound_simulator() {
  auto simulator = std::make_unique<sim::Simulator>();
  simulator->open_document(false, false, 0, true);
  std::string error;
  CHECK_MESSAGE(simulator->audio_on(error), error);
  return simulator;
}

const char* const kHeader = "document sound\naudio on\n";

}  // namespace

TEST_CASE("sim audio: the commands parse, with what they mean") {
  std::vector<sim::ScenarioCommand> cmds;
  std::string error;
  NEED(sim::parse_scenario("document sound tracks 4\naudio ON\nrender a 150\nrender b 10000 STEMS\npad 12 hold 300\npad 5 down\npad 5 up\npad 7\nencoder -1 x60 every 5\nencoder 3 x2\nencoder 2\n"
                           "assert audio b.t3 silent\n", cmds, error));
  NEED(cmds.size() == 12u);
  CHECK((cmds[0].kind == sim::ScenarioCommand::Kind::Document && (cmds[0].value & 4u) != 0u && ((cmds[0].value >> 8) & 0xFFu) == 4u));  // `sound` is bit 2, the tracks keep theirs
  CHECK(cmds[1].kind == sim::ScenarioCommand::Kind::Audio);
  CHECK((cmds[2].kind == sim::ScenarioCommand::Kind::Render && cmds[2].name == "a" && (cmds[2].value & 0x7FFFFFFFu) == 150u && (cmds[2].value & 0x80000000u) == 0u));
  CHECK((cmds[3].kind == sim::ScenarioCommand::Kind::Render && cmds[3].name == "b" && (cmds[3].value & 0x7FFFFFFFu) == 10000u && (cmds[3].value & 0x80000000u) != 0u));  // stems: bit 31
  CHECK((cmds[4].kind == sim::ScenarioCommand::Kind::Pad && cmds[4].value == 12u && cmds[4].mode == 1u && cmds[4].delta == 300));
  CHECK((cmds[5].mode == 2u && cmds[5].value == 5u));
  CHECK((cmds[6].mode == 3u && cmds[6].value == 5u));
  CHECK((cmds[7].mode == 0u && cmds[7].value == 7u));
  CHECK((cmds[8].kind == sim::ScenarioCommand::Kind::Encoder && cmds[8].delta == -1 && cmds[8].value == 60u && cmds[8].mode == 5u));
  CHECK((cmds[9].delta == 3 && cmds[9].value == 2u && cmds[9].mode == 1u));  // x2 with no `every`: back to back
  CHECK((cmds[10].delta == 2 && cmds[10].value == 1u));
  CHECK((cmds[11].kind == sim::ScenarioCommand::Kind::Assert && cmds[11].target == sim::ScenarioCommand::Target::Audio));  // b.t3: a stems render's part, known after `render b ... stems`
}

TEST_CASE("sim audio: every misuse of the commands is refused when the script is read") {
  CHECK(has(parse_error("audio\n"), "line 1: audio needs the word 'on'"));
  CHECK(has(parse_error("audio off\n"), "audio needs the word 'on'"));
  CHECK(has(parse_error("audio on\n"), "line 1: audio on: needs a `document` line before it"));
  CHECK(has(parse_error("document\naudio on\naudio on\n"), "line 3: audio on: audio is on already"));
  CHECK(has(parse_error("document\naudio on\ndocument\n"), "line 3: document: refused after `audio on`"));
  CHECK(has(parse_error("document\naudio on\ncard x\n"), "line 3: card: refused after `audio on`"));
  CHECK(has(parse_error("document\naudio on\nplayhead 3\n"), "line 3: playhead: refused after `audio on`"));
  CHECK(has(parse_error("document\naudio on\nplayhead stop\n"), "line 3: playhead: refused after `audio on`"));
  // `document new` has a project sink whose File > NEW rewrites the project in place, under an engine that plays it, with no Stop and no wait for the voices: `audio on` after it is refused. The last
  // `document` decides, and a card replaces the sink with its own loader (which stops the transport and waits for the voices).
  CHECK(has(parse_error("document new\naudio on\n"), "line 2: audio on: refused after `document new`"));
  CHECK(has(parse_error("document sound new tracks 4\nwait 5\naudio on\n"), "line 3: audio on: refused after `document new`"));
  // `card pull` and `card insert` stay: the card is the same card.
  std::vector<sim::ScenarioCommand> cmds;
  std::string error;
  CHECK(sim::parse_scenario("document\naudio on\ncard pull\ncard insert\n", cmds, error));
  CHECK(sim::parse_scenario("document new\ndocument sound\naudio on\n", cmds, error));
  CHECK(sim::parse_scenario("document sound new\ncard x\naudio on\n", cmds, error));
  // render
  CHECK(has(parse_error("render a 10\n"), "line 1: render: needs `audio on` before it"));
  const std::string head = "document\naudio on\n";
  CHECK(has(parse_error(head + "render\n"), "render needs a name"));
  CHECK(has(parse_error(head + "render a\n"), "render needs a name"));
  CHECK(has(parse_error(head + "render a-b 10\n"), "render needs a name of letters, digits and _"));
  CHECK(has(parse_error(head + "render a.b 10\n"), "render needs a name of letters, digits and _"));
  CHECK(has(parse_error(head + "render a+b 10\n"), "render needs a name of letters, digits and _"));
  CHECK(has(parse_error(head + "render " + std::string(65, 'a') + " 10\n"), "render needs a name of letters, digits and _"));
  CHECK(has(parse_error(head + "render a 0\n"), "a time 1..10000 ms"));
  CHECK(has(parse_error(head + "render a 10001\n"), "a time 1..10000 ms"));
  CHECK(has(parse_error(head + "render a -5\n"), "a time 1..10000 ms"));
  CHECK(has(parse_error(head + "render a x\n"), "a time 1..10000 ms"));
  CHECK(has(parse_error(head + "render a 10 loud\n"), "optionally the word 'stems'"));
  CHECK(has(parse_error(head + "render a 10\nrender a 20\n"), "line 4: render name 'a' is used twice"));
  CHECK(sim::parse_scenario(head + "render " + std::string(64, 'a') + " 10\nrender b_1 1 stems\n", cmds, error));
  // assert audio
  CHECK(has(parse_error(head + "assert audio\n"), "assert needs state, model, file or audio, and what to check"));
  CHECK(has(parse_error(head + "assert audio a silent\n"), "line 3: assert audio: no 'render a' before this line"));
  CHECK(has(parse_error(head + "render a 10\nassert audio a rms<@b\n"), "no 'render b' before this line"));
  CHECK(has(parse_error(head + "render a 10\nassert audio a null=b,-30\n"), "no 'render b' before this line"));
  CHECK(has(parse_error(head + "render a 10\nassert audio a null=a+c,-30\n"), "no 'render c' before this line"));
  CHECK(has(parse_error(head + "render a 10\nassert audio a.t1 silent\n"), "no 'render a.t1' before this line"));  // only `stems` makes the parts
  CHECK(has(parse_error(head + "render a 10\nassert audio a volume=3\n"), "unknown check 'volume'"));
  CHECK(has(parse_error(head + "render a 10\nassert audio a pitch=441\n"), "pitch=<Hz>"));
  CHECK(sim::parse_scenario(head + "render a 10 stems\nassert audio a.t8 silent\nassert audio a.m null=a,-60\nassert audio a null=a.t1+a.t2,-60,lag=63\n", cmds, error));
  // pad and encoder
  CHECK(has(parse_error("pad 5 hold\n"), "pad needs one index, 0..47"));
  CHECK(has(parse_error("pad 5 hold 0\n"), "pad needs one index, 0..47"));
  CHECK(has(parse_error("pad 5 hold 60001\n"), "pad needs one index, 0..47"));
  CHECK(has(parse_error("pad 5 sideways\n"), "pad needs one index, 0..47"));
  CHECK(has(parse_error("pad 5 down 3\n"), "pad needs one index, 0..47"));
  CHECK(has(parse_error("pad 48 down\n"), "pad needs one index, 0..47"));
  CHECK(has(parse_error("pad -1\n"), "pad needs one index, 0..47"));
  CHECK(has(parse_error("encoder 1 x1\n"), "encoder needs one non-zero number of detents"));
  CHECK(has(parse_error("encoder 1 x1001\n"), "encoder needs one non-zero number of detents"));
  CHECK(has(parse_error("encoder 1 every 5\n"), "encoder needs one non-zero number of detents"));
  CHECK(has(parse_error("encoder 1 x3 every 1\n"), "encoder needs one non-zero number of detents"));
  CHECK(has(parse_error("encoder 1 x3 every 256\n"), "encoder needs one non-zero number of detents"));
  CHECK(has(parse_error("encoder 1 x3 each 5\n"), "encoder needs one non-zero number of detents"));
  CHECK(has(parse_error("encoder 0 x3\n"), "encoder needs one non-zero number of detents"));
  CHECK(has(parse_error("document sound sound\n"), "document takes nothing, or the words 'bindings', 'new', 'sound', 'tb303', 'inserts', 'events' and 'tracks <n>'"));  // (events: OS-MIDIREC S1, `document events`)
}

TEST_CASE("sim audio: the engine renders at its own rate, a block every 1.45 ticks, and a render is the blocks of its ticks") {
  auto s = sound_simulator();
  NEED(s->has_audio());
  sim::SimAudio& rig = *s->audio();
  CHECK(rig.blocks_rendered() == 0u);  // audio_on() was one tick; 44.12 frames of a 64 do not make a block yet
  s->wait_ms(999);
  // The rate is 44,117,647 millihertz: a tick brings 44.117647 frames, a block is 64: 1,000 ticks make 689 blocks, exactly, with no wall clock in it.
  CHECK(rig.blocks_rendered() == 1000ull * hal::audio::sample_rate_mhz() / (64ull * 1000000ull));
  CHECK(rig.blocks_rendered() == 689u);

  // A window is the blocks of the ticks it covers: from the next tick, 100 of them. 100 ticks are 68.9 blocks: 68 or 69, the same every run.
  const std::uint64_t before = rig.blocks_rendered();
  std::string error;
  NEED(s->render_begin("a", 100, false, error));
  std::uint32_t left = 0;
  CHECK(s->render_running(&left) == "a");
  CHECK(left == 100u);
  CHECK(s->render_result("a") == nullptr);  // open: nothing to look at yet
  s->wait_ms(99);
  CHECK(s->render_running(&left) == "a");
  CHECK(left == 1u);
  s->step();
  CHECK(s->render_running(&left).empty());
  const sim::Pcm* a = s->render_result("a");
  NEED(a != nullptr);
  CHECK(a->frames() == 64u * (rig.blocks_rendered() - before));
  CHECK((a->frames() == 64u * 68u || a->frames() == 64u * 69u));
  CHECK(a->rate == doctest::Approx(static_cast<double>(hal::audio::sample_rate_mhz()) / 1000.0).epsilon(1e-9));  // 44,100 Hz by default
  CHECK(s->render_result("nothing") == nullptr);

  // One at a time, a name once, a sensible length, and the engine first.
  NEED(s->render_begin("b", 50, false, error));
  CHECK_FALSE(s->render_begin("c", 50, false, error));
  CHECK(has(error, "render 'b' is still running (50 ms more): one at a time"));
  s->wait_ms(50);
  CHECK_FALSE(s->render_begin("b", 50, false, error));
  CHECK(has(error, "render 'b' exists already"));
  CHECK_FALSE(s->render_begin("c", 0, false, error));
  CHECK(has(error, "a render is 1..10000 ms"));
  CHECK_FALSE(s->render_begin("c", 10001, false, error));
  sim::Simulator bare;
  CHECK_FALSE(bare.render_begin("x", 10, false, error));
  CHECK(has(error, "audio is off (`audio on` first)"));
  CHECK_FALSE(bare.has_audio());
  CHECK(bare.render_result("x") == nullptr);
  CHECK(bare.render_digests().empty());
  CHECK(has(run_error("document\naudio on\nrender a 10\nwait 5\nrender b 10\n"), "line 5: render 'b': render 'a' is still running (5 ms more): one at a time"));
}

TEST_CASE("sim audio: a stems render is whole stem blocks, its parts are renders, and the master pair is the master") {
  auto s = sound_simulator();
  std::string error;
  NEED(s->render_begin("s", 101, true, error));
  CHECK(s->render_result("s.t1") == nullptr);  // the parts are open with the whole
  s->pad_hold(12, 60);
  s->wait_ms(40);
  const sim::Pcm* master = s->render_result("s");
  NEED(master != nullptr);
  CHECK(master->frames() % 128u == 0u);  // two render blocks make a stem block: an odd last block of the master is left out
  for (unsigned t = 1; t <= 8; ++t) {
    const sim::Pcm* part = s->render_result("s.t" + std::to_string(t));
    NEED(part != nullptr);
    CHECK(part->frames() == master->frames());
    for (std::size_t i = 0; i + 1 < part->lr.size(); i += 2) {  // mono: both channels the same
      if (part->lr[i] != part->lr[i + 1]) {
        CHECK(part->lr[i] == part->lr[i + 1]);
        break;
      }
    }
  }
  const sim::Pcm* pair = s->render_result("s.m");
  NEED(pair != nullptr);
  CHECK(pair->lr == master->lr);  // what the stem tap calls Master L and R is what the codec gets
  CHECK_FALSE(sim::is_silent(sim::to_signal(*s->render_result("s.t1"))));  // the note went to T1 (the live notes search from the selected track)
  CHECK(sim::is_silent(sim::to_signal(*s->render_result("s.t2"))));
  // The tap is switched off again when the window closes (the USB stream is the tap's only other user).
  CHECK_FALSE(s->audio()->engine().graph().stems().enabled());
  // A render without `stems` has no parts.
  NEED(s->render_begin("plain", 10, false, error));
  s->wait_ms(10);
  CHECK(s->render_result("plain") != nullptr);
  CHECK(s->render_result("plain.t1") == nullptr);
}

TEST_CASE("sim audio: a run is the same samples twice, and one thing changed changes them") {
  const std::string script = std::string(kHeader) + "wait 30\nrender n 200\npad 12 hold 120\nwait 79\n";
  const auto digests = [&](const std::string& text) {
    auto simulator = std::make_unique<sim::Simulator>();
    CHECK(holds(*simulator, text));
    return simulator->render_digests();
  };
  const auto a = digests(script);
  const auto b = digests(script);
  NEED(a.size() == 1u);
  CHECK(a == b);  // a hash is a comparison between two runs of one build, never a golden (audio_check.h): the floats are reproducible on one machine and compiler
  CHECK(a[0].first == "n");
  CHECK(a[0].second.size() == 64u);
  CHECK(digests(std::string(kHeader) + "wait 30\nrender n 200\npad 14 hold 120\nwait 79\n") != a);  // another note
  CHECK(digests(std::string(kHeader) + "wait 31\nrender n 200\npad 12 hold 120\nwait 79\n") != a);  // the same note a tick later: the block grid moves under it
  // Two simulators at once do not disturb each other's counters (the rig resets the fake cycle count on its own start; the stage reports are the only reader).
  auto first = std::make_unique<sim::Simulator>();
  auto second = std::make_unique<sim::Simulator>();
  CHECK(holds(*first, script));
  CHECK(holds(*second, script));
  CHECK(first->render_digests() == a);
  CHECK(second->render_digests() == a);
}

TEST_CASE("sim audio: the test bank is bound through the instruments of the document, and `document sound` is neutral") {
  auto s = sound_simulator();
  const model::Project* project = s->project();
  NEED(project != nullptr);
  CHECK(project->instrument_count() == 4u);
  CHECK(project->tempo_x100() == 12000u);
  CHECK(project->song_length() == 1u);
  CHECK(project->pattern_length(1) == 16u);
  audio::VoicePool& voices = s->audio()->voices();
  // The sine and the noise loop over their whole 44,100 frames, the pluck is a one shot of half that; the fourth instrument is the sine again, the fifth slot is empty.
  const audio::SampleDesc* d0 = &voices.sample(0);
  CHECK(d0->length == 44100u);
  CHECK(d0->loop_start == 0u);
  CHECK(d0->loop_end == 44100u);
  CHECK(d0->gain == audio::kernel::kUnity);  // volume 50 under the 1.2+ law: 0 dB
  CHECK(d0->pan == 50u);
  CHECK(d0->bit_depth == 16u);
  CHECK(d0->overdrive == 0u);
  CHECK(voices.sample(1).length == 44100u);
  CHECK(voices.sample(1).loop_end == 44100u);
  CHECK(voices.sample(2).length == 22050u);
  CHECK(voices.sample(2).loop_end == 0u);  // no loop
  CHECK(voices.sample(3).length == 44100u);
  CHECK(voices.sample(4).length == 0u);
  // The first period of the sine, by the table in the source: 100 frames, a peak of 0.5 at frame 25.
  CHECK(d0->data[0] == 0);
  CHECK(d0->data[25] == 16384);
  CHECK(d0->data[50] == 0);
  CHECK(d0->data[75] == -16384);
  CHECK(d0->data[100] == 0);
  // The plain demo: ten instruments (their volumes are not unity) and the test bank under them: the sine in every slot but the noise and the pluck.
  sim::Simulator demo;
  demo.open_document();
  std::string error;
  NEED(demo.audio_on(error));
  CHECK(demo.project()->instrument_count() == 10u);
  CHECK(demo.audio()->voices().sample(9).length == 44100u);
  CHECK(demo.audio()->voices().sample(0).gain != audio::kernel::kUnity);  // the demo's KICK is at 100: +24 dB, as its page says
  CHECK(demo.audio()->voices().sample(10).length == 0u);
}

TEST_CASE("sim audio: the app's sinks reach the engine: pads, Play, Stop and the parameter ring") {
  auto s = sound_simulator();
  audio::VoicePool& voices = s->audio()->voices();
  std::string error;
  CHECK(holds(*s, "assert state screen=PATTERN playing=0\n"));
  CHECK(voices.voices_in_use() == 0u);
  // A pad: the live note takes a voice, the release lets it go (256 samples, 5.8 ms).
  s->pad_down(12);
  s->wait_ms(10);
  CHECK(voices.voices_in_use() == 1u);
  s->pad_up(12);
  s->wait_ms(20);
  CHECK(voices.voices_in_use() == 0u);
  // Play is a command on the sequencer's ring, taken at the next block; the state line reads the sequencer's position, and Stop is the same.
  CHECK(holds(*s, "tap play\nwait 5\nassert state playing=1\n"));
  CHECK(s->audio()->sequencer().playing());
  CHECK(holds(*s, "tap play\nwait 5\nassert state playing=0\n"));
  CHECK_FALSE(s->audio()->sequencer().playing());
  // The app's parameter ring is the engine's: an edit is drained by the engine in a block, after its voices (and the fake ring's list stays empty).
  const std::uint32_t before = s->audio()->engine().params_applied();
  CHECK(holds(*s, "tap params\ntap f2\nencoder -10\nwait 10\nassert model inst.1.volume=40\n"));
  CHECK(s->audio()->engine().params_applied() > before);
  CHECK(s->heard_params().empty());
  // Without the engine the fake ring hears it, as it always did.
  sim::Simulator bare;
  bare.open_document(false, false, 0, true);
  CHECK(holds(bare, "tap params\ntap f2\nencoder -10\n"));
  CHECK_FALSE(bare.heard_params().empty());
}

TEST_CASE("sim audio: pad hold, pad down and up and an encoder sweep take the ticks they say") {
  // A window of N ticks that `body` just fills ends with the script; one tick longer and the script ends with the window open (the runner refuses that). So `body` is exactly N ticks.
  const auto takes = [](const std::string& body, unsigned ticks) {
    const std::string head = std::string(kHeader) + "render w ";
    auto s = std::make_unique<sim::Simulator>();
    std::string error;
    const bool fits = run_on(*s, head + std::to_string(ticks) + "\n" + body, error);
    CHECK_MESSAGE(fits, body << " in " << ticks << " ticks: " << error);
    auto longer = std::make_unique<sim::Simulator>();
    std::string why;
    const bool more = run_on(*longer, head + std::to_string(ticks + 1u) + "\n" + body, why);
    CHECK_MESSAGE(!more, body << " is more than " << ticks << " ticks");
    CHECK_MESSAGE(why.find("(1 ms more)") != std::string::npos, why);
  };
  takes("pad 12 hold 50\n", 51u);  // down, 50 ticks, up and one more
  takes("pad 12\n", 2u);
  takes("pad 12 down\n", 1u);
  takes("pad 12 down\npad 12 up\n", 2u);
  takes("encoder 1\n", 1u);
  takes("encoder 1 x3\n", 3u);
  takes("encoder 1 x3 every 5\n", 3u + 2u * 4u);  // three turns of a tick each, and four ticks of time between two
  takes("encoder -2 x4 every 255\n", 4u + 3u * 254u);
}

TEST_CASE("sim audio: a project load stops the transport and lets the voices die before it starts, a load without the engine is as it was") {
  const std::string card = "document sound\ncard e2e_card\naudio on\n";
  const char* const kOpen = "tap file\nwait 300\ntap f1\nwait 600\n";
  {
    auto s = std::make_unique<sim::Simulator>();
    CHECK(holds(*s, card + "tap play\nwait 60\nassert state playing=1\n" + kOpen + "assert state playing=0 msg=~OPENED\n"));
    CHECK_FALSE(s->audio()->sequencer().playing());
    CHECK(s->audio()->voices().voices_in_use() == 0u);
  }
  // The card's pool is the engine's: a note on a slot its loader never bound is ignored, and counted (the test bank is not bound to a pool that a card's project is about to fill).
  {
    auto s = std::make_unique<sim::Simulator>();
    CHECK(holds(*s, card + "pad 12 hold 20\nwait 20\n"));
    CHECK(s->audio()->voices().voices_in_use() == 0u);
    CHECK(s->audio()->voices().sample(0).length == 0u);
  }
  // Without the engine: the same load, no Stop and no Settle (the badge only), and it ends as before.
  auto bare = std::make_unique<sim::Simulator>();
  CHECK(holds(*bare, "document sound\ncard e2e_card\n" + std::string(kOpen) + "assert state msg=~OPENED\n"));
}

TEST_CASE("sim audio: the API after the engine is on") {
  auto s = sound_simulator();
  std::string error;
  CHECK_FALSE(s->audio_on(error));
  CHECK(has(error, "audio is on already"));
  CHECK_FALSE(s->open_card("e2e_card", error));
  CHECK(has(error, "audio is on already: the engine's voices are bound to the document and the card in place"));
  sim::Simulator none;
  CHECK_FALSE(none.audio_on(error));
  CHECK(has(error, "a document must be open first"));
  // The API refuses what the parser does: a document with the File > NEW sink, unless a card has taken the sink over.
  sim::Simulator with_new;
  with_new.open_document(false, true, 0, true);
  CHECK_FALSE(with_new.audio_on(error));
  CHECK(has(error, "refused after `document new`"));
  CHECK_FALSE(with_new.has_audio());
  with_new.wait_ms(5);
  CHECK(with_new.healthy());
  sim::Simulator carded;
  CHECK(holds(carded, "document sound new\ncard e2e_card\naudio on\nwait 5\n"));
  CHECK(carded.has_audio());
  // A document opened over the engine drops the rig (its sequencer plays the document that goes) and gives the app its fake position back.
  s->open_document();
  CHECK_FALSE(s->has_audio());
  CHECK(s->audio() == nullptr);
  CHECK(s->render_result("x") == nullptr);
  s->wait_ms(5);
  CHECK(s->healthy());
}

// ---- OS-E2E-9: the module in the engine, the WAV of the card as a render, the Amiga voices -----------------------------------------------------------------------------------------------

TEST_CASE("sim audio: document tb303 binds the TB-303 module to its instrument, and the acid line sounds (Play, a take)") {
  auto simulator = std::make_unique<sim::Simulator>();
  std::string error;
  CHECK_MESSAGE(run_on(*simulator, "document tb303\naudio on\nwait 20\nrender a 700\ntap play\nwait 700\ntap play\nassert audio a sound\n", error), error);
  sim::SimAudio* audio = simulator->audio();
  NEED(audio != nullptr);
  CHECK(audio->modules().instances() == 1u);
  CHECK(audio->modules().bound(0));
  CHECK_FALSE(audio->modules().bound(1));
  CHECK(audio->modules().stats().note_ons >= 3u);   // the line has notes at steps 0, 2 and 3 in the first 700 ms
  // the same script twice gives the same samples: the module is deterministic, and so is the rest
  auto twin = std::make_unique<sim::Simulator>();
  CHECK_MESSAGE(run_on(*twin, "document tb303\naudio on\nwait 20\nrender a 700\ntap play\nwait 700\n", error), error);
  const auto a = simulator->render_digests();
  const auto b = twin->render_digests();
  CHECK((!a.empty() && a == b));
}

TEST_CASE("sim audio: a document without a module instrument has no module bound, and the engine plays it as before") {
  auto simulator = sound_simulator();
  CHECK(simulator->audio()->modules().instances() == 0u);
}

TEST_CASE("sim audio: the wav command reads a WAV file of the card as a render, and every mistake is refused with its line") {
  std::vector<sim::ScenarioCommand> cmds;
  std::string error;
  NEED(sim::parse_scenario("document\naudio on\nwav a \"EXPORT/X/X_SONG0001.WAV\"\nassert audio a sound\n", cmds, error));
  CHECK((cmds[2].kind == sim::ScenarioCommand::Kind::Wav && cmds[2].name == "a" && cmds[2].args.size() == 1u && cmds[2].args[0] == "EXPORT/X/X_SONG0001.WAV"));
  CHECK(has(parse_error("wav a x.wav\n"), "line 1: wav: needs `audio on` before it"));
  CHECK(has(parse_error("document\naudio on\nwav\n"), "wav needs a name of letters, digits and _"));
  CHECK(has(parse_error("document\naudio on\nwav a\n"), "wav needs a name of letters, digits and _"));
  CHECK(has(parse_error("document\naudio on\nwav a-b x.wav\n"), "wav needs a name of letters, digits and _"));
  CHECK(has(parse_error("document\naudio on\nrender a 10\nwav a x.wav\n"), "render name 'a' is used twice"));
  // the file is read when the line runs: no card, a file that is not there, a file that is not a WAV
  CHECK(has(run_error("document\naudio on\nwav a x.wav\n"), "line 3: wav 'a': there is no mounted card"));
  const std::filesystem::path card = std::filesystem::path(O3_E2E_CARD_DIR);
  const std::string head = "document\ncard \"" + card.string() + "\"\naudio on\n";
  CHECK(has(run_error(head + "wav a nothing.wav\n"), "wav 'a': nothing.wav cannot be opened"));
  CHECK(has(run_error(head + "wav a \"Projects/Small Song/project.o3pj\"\n"), "is not a RIFF WAVE file"));
}
