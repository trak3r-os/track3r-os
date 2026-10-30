// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-E2E-1b: the simulator's card (sim/simulator.h open_card, sim/sim_project.h, the scenario command `card`): a scratch copy of a fixture that the app lists, opens, saves to and loses, and the
// project loader behind File > OPEN and NEW. The cases run scenario text through the real scenario runner, so what they hold down is what the journeys of sim/scenarios/e2e/ rely on.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_folder_fixture.h"

#include <o3os/app/og_folder.h>
#include <o3os/app/og_samples.h>
#include <o3os/model/project.h>
#include <o3os/platform/host/dir_card.h>
#include <o3os/platform/host/dir_storage.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/scenario_assert.h>
#include <o3os/sim/simulator.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace o3os;
using platform::host::ScratchDir;

namespace {

// The card fixture: one native project, Projects/Small Song (`o3pj_tool gen small`, 2,648 bytes), GENERATED into the build directory by the target e2e_card_fixture (sim/CMakeLists.txt) and
// never committed; tests/e2e_card_fixture_test.py holds it to the generator. The tests read it and copy it into scratch directories; they never write to it.
fs::path e2e_card_fixture() { return fs::path(O3_E2E_CARD_DIR); }
constexpr const char* kSmallSha = "fe075d31e2849cf6cb81a0bb157e1d748d13ed9c7c9141d98119b51370dc74df";

// Parses and runs `text` on `simulator` (the scenario's folder is `dir`). true: the whole script held; `error` is what the run said otherwise.
bool run_on(sim::Simulator& simulator, const std::string& text, std::string& error, const std::string& dir = std::string()) {
  std::vector<sim::ScenarioCommand> cmds;
  if (!sim::parse_scenario(text, cmds, error)) {
    return false;
  }
  simulator.set_scenario_dir(dir);
  return sim::run_scenario(cmds, simulator, [](const std::string&, const sim::Simulator&) { return true; }, error);
}
bool run_text(const std::string& text, std::string& error, const std::string& dir = std::string()) {
  auto simulator = std::make_unique<sim::Simulator>();
  return run_on(*simulator, text, error, dir);
}
bool holds(const std::string& text, const std::string& dir = std::string()) {
  std::string error;
  const bool ok = run_text(text, error, dir);
  CHECK_MESSAGE(error.empty(), error);
  return ok;
}
std::string run_error(const std::string& text, const std::string& dir = std::string()) {
  std::string error;
  CHECK_FALSE(run_text(text, error, dir));
  return error;
}
std::string card_line() { return "document\ncard \"" + e2e_card_fixture().string() + "\"\n"; }
// The File screen's way to a project: the screen, a wait for the listing, F1.
const char* const kOpenFirst = "tap file\nwait 300\ntap f1\nwait 600\n";

void write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST_CASE("sim card: the card commands parse, and a fixture named pull or insert keeps its name with ./") {
  std::vector<sim::ScenarioCommand> cmds;
  std::string error;
  NEED(sim::parse_scenario("card ../cards/x\nCARD Pull\ncard insert\ncard \"a b/c\"\ncard ./pull\n", cmds, error));
  NEED(cmds.size() == 5u);
  CHECK((cmds[0].kind == sim::ScenarioCommand::Kind::Card && cmds[0].value == 0u && cmds[0].name == "../cards/x"));
  CHECK((cmds[1].value == 1u && cmds[1].name == "pull"));
  CHECK((cmds[2].value == 2u && cmds[2].name == "insert"));
  CHECK((cmds[3].value == 0u && cmds[3].name == "a b/c"));  // quotes: one word
  CHECK((cmds[4].value == 0u && cmds[4].name == "./pull"));
  CHECK_FALSE(sim::parse_scenario("card\n", cmds, error));
  CHECK(error.find("line 1: card needs a fixture folder, or the word pull or insert") == 0u);
  CHECK_FALSE(sim::parse_scenario("card a b\n", cmds, error));
  CHECK(error.find("card needs a fixture folder") != std::string::npos);
  CHECK(sim::parse_scenario("card copy \"Export/a b\" Projects/c\n", cmds, error));  // OS-E2E J2: a computer copies a folder of the pulled card
  CHECK((cmds.size() == 1u && cmds[0].value == 3u && cmds[0].name == "Export/a b" && cmds[0].args.size() == 1u && cmds[0].args[0] == "Projects/c"));
}

TEST_CASE("sim card: the fixture is copied, a save goes to the copy, and a second simulator finds the card as it was") {
  const fs::path fixture = e2e_card_fixture();
  NEED(fs::is_directory(fixture));
  const std::string saved = card_line() + kOpenFirst + "tap file\ntap f5\nwait 50\ntap f8\nwait 400\n";
  CHECK(holds(saved + "assert state msg=SAVE_TRACK_01_DONE\nassert file \"Projects/Track 01/project.o3pj\" sha=" + kSmallSha + "\n"));  // (when the run ends, its scratch copy goes)
  CHECK(holds(card_line() + "assert file \"Projects/Track 01\" absent\nassert file \"Projects/Small Song/project.o3pj\" sha=" + kSmallSha + "\n"));  // a fresh simulator: a fresh copy
  CHECK_FALSE(fs::exists(fixture / "Projects" / "Track 01"));  // and the fixture itself is never written to
  CHECK(fs::exists(fixture / "Projects" / "Small Song" / "project.o3pj"));
  std::size_t folders = 0;
  for (const fs::directory_entry& e : fs::directory_iterator(fixture / "Projects")) {
    folders += e.is_directory() ? 1u : 0u;
  }
  CHECK(folders == 1u);
}

TEST_CASE("sim card: a relative fixture is relative to the folder the simulator is given (--cards, else the scenario's), and every mistake says what is wrong") {
  const std::string cards = e2e_card_fixture().parent_path().string();  // <build>/sim/cards
  CHECK(holds("document\ncard e2e_card\nassert file \"Projects/Small Song/project.o3pj\" exists\n", cards));
  CHECK(holds("document\ncard ./e2e_card\nassert file \"Projects/Small Song/project.o3pj\" exists\n", cards));
  CHECK(run_error("document\ncard e2e_card\n", cards + "/e2e_card").find("is not a directory") != std::string::npos);  // relative to the folder it is asked from

  CHECK(run_error("card x\n").find("line 1: card 'x': a document must be open first") == 0u);
  CHECK(run_error("document\ncard \"" + (e2e_card_fixture() / "nowhere").string() + "\"\n").find("is not a directory") != std::string::npos);
  CHECK(run_error("document\ncard \"" + (e2e_card_fixture() / "Projects" / "Small Song" / "project.o3pj").string() + "\"\n").find("is not an image of 512-byte sectors") != std::string::npos);  // a file is an IMAGE (OS-QANET D1), and this one is no image
  CHECK(run_error(card_line() + "card \"" + e2e_card_fixture().string() + "\"\n").find("a card is in the slot already") != std::string::npos);
  CHECK(run_error(card_line() + "document\n").find("line 3: document: a card is in the slot already") == 0u);
  CHECK(run_error("document\ncard pull\n").find("line 2: card pull: there is no card in the slot") == 0u);
  CHECK(run_error("document\ncard insert\n").find("there is no card to put in") != std::string::npos);
  CHECK(run_error(card_line() + "card insert\n").find("the card is in the slot") != std::string::npos);
  CHECK(run_error(card_line() + "card pull\ncard pull\n").find("the card is pulled already") != std::string::npos);
  CHECK(run_error("assert file Projects exists\n").find("assertion failed: assert file Projects exists: no card") != std::string::npos);
}

TEST_CASE("sim card: a fixture with a symbolic link in it is no card (a path of the card could lead out of the scratch copy)") {
  ScratchDir fixture("simcard-link");
  NEED(fixture.ok());
  write_file(fs::path(fixture.path()) / "Projects" / "Song" / "project.o3pj", {1, 2, 3});
  CHECK(holds("document\ncard \"" + fixture.path() + "\"\nassert file \"Projects/Song/project.o3pj\" size=3\n"));  // as it is: a card
  std::error_code ec;
  fs::create_directory_symlink("nowhere", fs::path(fixture.path()) / "Projects" / "Song" / "out", ec);
  NEED(!ec);
  const std::string e = run_error("document\ncard \"" + fixture.path() + "\"\n");
  CHECK(e.find("line 2: card ") == 0u);
  CHECK(e.find("is a symbolic link") != std::string::npos);
}

TEST_CASE("sim card: File > OPEN of the fixture loads it the way the platform does, and the app is on the Pattern screen") {
  auto simulator = std::make_unique<sim::Simulator>();
  std::string error;
  CHECK_MESSAGE(run_on(*simulator, card_line() + kOpenFirst, error), error);
  CHECK(simulator->healthy());
  const model::Project* p = simulator->project();
  NEED(p != nullptr);
  CHECK(std::string(p->name()) == "Small Song");
  CHECK(p->tempo_x100() == 12850u);
  CHECK(p->song_length() == 5u);
  CHECK(std::string(p->instrument(0)->name) == "Kick");
  CHECK(simulator->app().state().screen == app::ScreenId::Pattern);
  CHECK(std::string(simulator->app().message()) == "PROJECT Small Song OPENED");
  // the document was handed over as the platform hands it over: the undo history is the new document's own, and nothing in it counts as unsaved work
  CHECK(simulator->app().overlay() == app::OverlayId::Notify);
  CHECK(holds(card_line() + kOpenFirst + "tap file\ntap f1\nassert state msg=~^OPENING_\n"));  // F1 on a clean document opens again without asking (a question would say UNSAVED CHANGES)
}

TEST_CASE("sim card: F1 on a project with unsaved changes asks first, and the second F1 drops the changes and opens") {
  const std::string edited = card_line() + kOpenFirst + "tap pattern\ntap rec\ntap down\npad 36\nassert model pat.1.0.1.note=37\ntap file\n";
  CHECK(holds(edited + "tap f1\nassert state screen=FILE msg=UNSAVED_CHANGES:_F1_AGAIN_TO_DISCARD_THEM keys=~F1:OPEN[?]\nassert model pat.1.0.1.note=37\n"
                       "tap f1\nwait 600\nassert state screen=PATTERN msg=PROJECT_Small_Song_OPENED\nassert model pat.1.0.1.note=0\n"));
  // a save takes the question back: the project is clean, F1 opens at once
  CHECK(holds(edited + "tap f1\ntap f4\nwait 400\ntap f1\nassert state msg=~^OPENING_\n"));
}

TEST_CASE("sim card: File > OPEN of an OG project reads what our OG loader reads, by the same loader") {
  ScratchDir scratch("simcard-og");
  NEED(scratch.ok());
  for (const test::og::FolderFile& f : test::og::synthetic_folder()) {
    write_file(fs::path(scratch.path()) / "Projects" / "Synthetic" / f.path, f.bytes);
  }
  // what the loader alone makes of the folder
  auto arena = std::make_unique<model::ModelArena>();
  platform::host::DirStorage storage(scratch.path());
  NEED(storage.mount() == hal::storage::Status::Ok);
  auto loader = std::make_unique<app::OgFolderLoader>();
  NEED(loader->begin(storage, "Projects/Synthetic", *arena));
  while (loader->step(1u << 20) == app::OgFolderLoader::State::Running) {
  }
  NEED(loader->state() == app::OgFolderLoader::State::Done);
  const model::Project& expected = arena->project;

  auto simulator = std::make_unique<sim::Simulator>();
  std::string error;
  CHECK_MESSAGE(run_on(*simulator, "document\ncard \"" + scratch.path() + "\"\ntap file\nwait 300\nassert state sel=Synthetic\ntap f1\nwait 800\nassert state screen=PATTERN msg=~OPENED$\n", error), error);
  const model::Project* got = simulator->project();
  NEED(got != nullptr);
  CHECK(got != &expected);
  CHECK(got->tempo_x100() == expected.tempo_x100());
  CHECK(got->song_length() == expected.song_length());
  CHECK(got->pattern_count() == expected.pattern_count());
  CHECK(got->instrument_count() == expected.instrument_count());
  for (unsigned i = 0; i < got->song_length(); ++i) {
    CHECK(got->order(i) == expected.order(i));
  }
  std::size_t steps = 0;
  for (std::uint32_t id = 1; id <= model::kMaxPatternId; ++id) {
    const model::Pattern* a = got->pattern(id);
    const model::Pattern* b = expected.pattern(id);
    CHECK((a == nullptr) == (b == nullptr));
    if (a != nullptr && b != nullptr) {
      CHECK(std::memcmp(a->steps, b->steps, sizeof a->steps) == 0);
      CHECK(std::memcmp(a->track_length, b->track_length, sizeof a->track_length) == 0);
      ++steps;
    }
  }
  CHECK(steps == expected.pattern_count());
  for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* a = got->instrument(i);
    const model::Instrument* b = expected.instrument(i);
    CHECK((a == nullptr) == (b == nullptr));
    if (a != nullptr && b != nullptr) {
      CHECK(std::string(a->name) == std::string(b->name));
    }
  }
  // the project opens on the pattern it was saved on
  CHECK(simulator->app().state().edit.pattern == loader->report().start_pattern);
}

TEST_CASE("sim card: a project that does not open leaves the app on File with the reason, and NEW makes a fresh document") {
  ScratchDir scratch("simcard-bad");
  NEED(scratch.ok());
  write_file(fs::path(scratch.path()) / "Projects" / "Broken" / "project.o3pj", std::vector<std::uint8_t>(300, 0x5A));
  CHECK(holds("document\ncard \"" + scratch.path() + "\"\ntap file\nwait 300\ntap f1\nwait 300\nassert state screen=FILE overlay=NOTIFY msg=PROJECT_Broken_FAILED:_NOT_AN_O3PJ_PROJECT_FILE\n"));

  // NEW asks (F3 again is ALL), then the document is a fresh one: one song position, no instrument, and the Pattern screen
  CHECK(holds(card_line() + "tap file\nwait 300\ntap f3\nassert state keys=F3:ALL,F1:KEEP_INS msg=NEW_PROJECT:_F3_ALL,_F1_KEEP_INSTRUMENTS\n"
                            "tap f3\nwait 300\nassert state screen=PATTERN msg=NEW_PROJECT\nassert model order.len=1\nassert model pat.1.0.0.note=0\n"));
  CHECK(run_error(card_line() + "tap file\nwait 300\ntap f3\ntap f3\nwait 300\nassert model inst.1.name=Kick\n").find("instrument 1 is empty") != std::string::npos);
}

TEST_CASE("sim card: OS-QAFIX-4, an open of a project that cannot load leaves the open document as it is, and says why") {
  ScratchDir scratch("simcard-refuse");
  NEED(scratch.ok());
  fs::copy(e2e_card_fixture(), scratch.path(), fs::copy_options::recursive);
  // The card is a copy of this directory made when `card` runs: the file is cut short (a power cut, a bad copy) before that. The only project on it is the refused one, so the document the walk
  // edits is the one the scenario started with.
  const fs::path file = fs::path(scratch.path()) / "Projects" / "Small Song" / "project.o3pj";
  fs::resize_file(file, fs::file_size(file) / 2);
  auto simulator = std::make_unique<sim::Simulator>();
  std::string error;
  const std::string card = "document\ncard \"" + scratch.path() + "\"\n";
  CHECK_MESSAGE(run_on(*simulator, card + "tap pattern\ntap rec\ntap down\npad 36\nassert model pat.1.0.1.note=37\n", error), error);
  const std::string before = [&]() {
    const model::Project* p = simulator->project();
    return std::string(p->name()) + "/" + std::to_string(p->tempo_x100()) + "/" + std::to_string(p->song_length());
  }();
  // F1 asks about the unsaved changes, the second F1 opens, and the open is refused before anything is replaced.
  CHECK_MESSAGE(run_on(*simulator, "tap file\nwait 300\ntap f1\nassert state msg=UNSAVED_CHANGES:_F1_AGAIN_TO_DISCARD_THEM\ntap f1\nwait 600\n"
                                   "assert state screen=FILE msg=PROJECT_Small_Song_FAILED:_THE_FILE_IS_CUT_SHORT\n"
                                   "assert model pat.1.0.1.note=37\n", error), error);
  const model::Project* p = simulator->project();
  NEED(p != nullptr);
  CHECK(std::string(p->name()) + "/" + std::to_string(p->tempo_x100()) + "/" + std::to_string(p->song_length()) == before);
  CHECK(simulator->healthy());
  // the document is still the user's unsaved work: the history is there (undo takes the note back), and the open asks again
  CHECK_MESSAGE(run_on(*simulator, "tap file\ntap f1\nassert state msg=UNSAVED_CHANGES:_F1_AGAIN_TO_DISCARD_THEM\n"
                                   "tap pattern\naction edit.undo\nassert model pat.1.0.1.note=0\n", error), error);
}

TEST_CASE("sim card: a pulled card is NoCard for the app and the assertions, and the same card is back after the insert") {
  CHECK(holds(card_line() + "tap file\nwait 300\ncard pull\nassert state msg=CARD_REMOVED\ntap f4\nwait 100\nassert state msg=NO_CARD\n"));
  CHECK(run_error(card_line() + "card pull\nassert file \"Projects/Small Song/project.o3pj\" exists\n").find("the card is pulled") != std::string::npos);
  CHECK(run_error(card_line() + "card pull\nassert file \"Projects/Small Song\" absent\n").find("the card is pulled") != std::string::npos);  // not "absent" either
  // put back: READING CARD, then CARD READY; the files are there and a save works
  CHECK(holds(card_line() + "tap file\nwait 300\ncard pull\ncard insert\nwait 100\nassert state msg=CARD_READY\n"
                            "assert file \"Projects/Small Song/project.o3pj\" sha=" + std::string(kSmallSha) + "\n"
                            "tap f4\nwait 600\nassert state msg=~^SAVE_.*_DONE$\nassert file \"Projects/Track 01/project.o3pj\" exists\n"));
}

TEST_CASE("sim card: a card pulled in the middle of a save leaves no half file under the final name, and the next save works") {
  // The save runs a slice a tick, so a pull a few ticks after F4 cuts it at different places: the strip says it failed, whatever is left of the Temp is not the project, and after the insert the same
  // key saves it: the stale Temp goes, the old file is kept as its .bak and the new one is whole.
  for (const unsigned ticks : {1u, 2u, 3u, 5u}) {
    const std::string cut = card_line() + kOpenFirst + "tap pattern\ntap rec\ntap down\npad 36\ntap file\ntap f4\nwait " + std::to_string(ticks) + "\ncard pull\nwait 100\n";
    CAPTURE(ticks);
    CHECK(holds(cut + "assert state msg=~^SAVE_SMALL_SONG_FAILED\n"
                      "card insert\nwait 100\n"
                      "assert file \"Projects/Small Song/project.o3pj\" sha=" + std::string(kSmallSha) + "\n"  // the file under its final name is still the old one, whole
                      "tap f4\nwait 600\n"
                      "assert state msg=SAVE_SMALL_SONG_DONE\n"
                      "assert file \"Projects/Small Song/_o3save.tmp\" absent\n"
                      "assert file \"Projects/Small Song/project.o3pj.bak\" sha=" + std::string(kSmallSha) + "\n"
                      "assert file \"Projects/Small Song/project.o3pj\" size>2000\n"));
  }
}

TEST_CASE("sim card: open_document after a card takes the card away with the old document") {
  auto simulator = std::make_unique<sim::Simulator>();
  std::string error;
  CHECK_MESSAGE(run_on(*simulator, card_line() + kOpenFirst, error), error);
  CHECK(simulator->has_card());
  simulator->open_document();  // (the scenario runner refuses `document` after `card`; the API does not dangle)
  CHECK_FALSE(simulator->has_card());
  CHECK(simulator->card_storage() == nullptr);
  CHECK(simulator->healthy());
  CHECK(simulator->project()->tempo_x100() == 13000u);  // the demo document again
  CHECK_MESSAGE(run_on(*simulator, "tap file\nwait 300\ntap f4\nwait 100\nassert state msg=NO_CARD_WRITE_LAYER\n", error), error);
  CHECK(simulator->healthy());
}

// OS-E2E-9: File > IMPORT of a module through the simulator's own project sink (sim/sim_project.h import_module: the platform's start_module_import and its phases). The module is the ProTracker
// fixture of the reference journeys (tools/e2e_ref_cards.py: one channel of a 32-byte sine), GENERATED into the build directory beside the e2e card; the journeys compare what it sounds like with libopenmpt.
TEST_CASE("sim card: File > IMPORT of a module replaces the document with its song, and the project is unsaved work") {
  const fs::path card = e2e_card_fixture().parent_path() / "e2e_ref_mod";
  NEED(fs::is_directory(card));
  const std::string head = "document\ncard \"" + card.string() + "\"\ntap file\nwait 50\ntap f6\nwait 100\nassert state screen=FILE sel=ref.mod\n";
  CHECK(holds(head + "tap f1\nwait 800\nassert state screen=FILE msg=~^IMPORTED_REF.MOD\nassert model order.len=1\nassert model tempo=12500\nassert model inst.1.name=sine\n"
                     "assert model pat.1.0.0.note=61\nassert model pat.1.0.0.instr=1\n"));
  // an imported song is on no card: the second F1 on a module asks before it drops the song (UNSAVED CHANGES), where a project that was opened and not touched would not
  CHECK(holds(head + "tap f1\nwait 800\ntap f1\nassert state msg=UNSAVED_CHANGES:_F1_AGAIN_TO_DISCARD_THEM keys=~F1:IMPORT[?]\n"));
  // FX AMIGA is the engine window (128 + the MED command: arpeggio 037 is 128:55); FX MODRN (F7 twice from AUTO) imports the same bytes for the Modern engine, where the same cell is the core tracker FX 46 (46:55)
  CHECK(holds(head + "tap f1\nwait 800\nassert model pat.1.0.1.fx1=128:55\n"));
  CHECK(holds(head + "tap f7\ntap f7\ntap f1\nwait 800\nassert state msg=~^IMPORTED_REF.MOD\nassert model pat.1.0.1.fx1=46:55\n"));
}

TEST_CASE("sim card: a file that is not a module is refused before anything starts (OS-IMPORT-ANY I3): the strip says why, the document stays, and the NEW question is never asked") {
  ScratchDir scratch("simcard-badmod");
  NEED(scratch.ok());
  write_file(fs::path(scratch.path()) / "junk.mod", std::vector<std::uint8_t>(300, 0x5A));
  CHECK(holds("document\ncard \"" + scratch.path() + "\"\ntap file\nwait 50\ntap f6\nwait 100\ntap f1\nwait 300\nassert state screen=FILE msg=NOT_A_MODULE_THIS_OS_READS\n"
                     "assert model inst.1.name=KICK\nassert model order.len=1\n"));  // (the demo document, untouched: the old importer left an empty one)
}
