// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-APPUX S2 (docs/specs/OS-APPUX.md 3.2): the one grammar of an app page, tested per shipped app through the shared helper (support/app_grammar.h). The apps are the host objects of the simulator's
// card (`apps on`), launched from MODULES, driven through the whole stack: Up / Down the row, Left / Right the page, the encoder and Shift + the encoder the value, Delete the default, and no other key
// changes a value. The six apps that draw their own rows are here; the five that declare rows join with S3.
#include <doctest/doctest.h>

#include "support/app_grammar.h"
#include "support/need.h"

using namespace o3os;
using namespace o3os::test::grammar;

namespace {

void run(const char* stem, const Spec& spec) {
  AppRig rig;
  NEED(spec.open_from_modules ? rig.open(stem, O3_E2E_CARD_DIR) : rig.open_tool(stem, O3_E2E_CARD_DIR));
  CHECK(rig.ui->app_param_count() == spec.declared);  // the rows are declared (storage, OS-APPUX S3) ...
  CHECK(!rig.ui->app_os_column());                     // ... and drawn by the app: the OS lists none of them
  check_grammar(rig, spec);
  check_persistence(rig, stem, spec);
  CHECK(rig.sim.healthy());
  CHECK(rig.on_page());
}

}  // namespace

TEST_CASE("app grammar: Humanize speaks the grammar: rows TIME, VOL, SEED, coarse steps, Delete is the default") {
  Spec spec;
  spec.declared = 3;
  run("humanize", spec);
}

TEST_CASE("app grammar: Chords speaks the grammar: eight rows, KEY a wheel, lists with no coarse step") {
  Spec spec;
  spec.declared = 8;
  spec.page_first_row = {"KEY", "PATTERN"};  // the eight settings, then the target rows (OS-APPUX S5)
  spec.fine_only = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 7);  // KEY SCALE PROG OCTAVE SEVENTHS MODE
  spec.text_rows = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 7);               // (text values)
  run("chords", spec);
}

TEST_CASE("app grammar: Chords' target rows are its second page; APPLY writes at the moved target (OS-APPUX S5)") {
  AppRig rig;
  NEED(rig.open_tool("chords", O3_E2E_CARD_DIR));
  TargetSpec ts;
  ts.pages_right = 1;
  ts.writes = true;
  check_target_rows(rig, "chords", ts);
}

TEST_CASE("app grammar: Mutate speaks the grammar: AMOUNT and SEED coarse, GROUP is a list that follows the instrument") {
  Spec spec;
  spec.declared = 3;
  run("mutate", spec);
}

TEST_CASE("app grammar: Morph speaks the grammar: SOURCE is a list of the instrument's kind, POS coarse") {
  Spec spec;
  spec.declared = 2;
  spec.persist_row = 1;  // POS (SOURCE is a list of the instruments that match: this card's project may have none)
  spec.moves_others = 1u << 2;  // INSTRUMENT A: SOURCE B lists the other instruments of A's kind
  run("morph", spec);
}

TEST_CASE("app grammar: SMF Export speaks the grammar on its one row: PATTERN, Shift + the encoder by ten, Delete the pattern it opened on") {
  Spec spec;
  spec.declared = 1;
  spec.relaunch_from_modules = true;  // opened from TOOLS it starts at the cursor's pattern; the kept pattern is the start from MODULES
  run("smfexport", spec);
}

TEST_CASE("app grammar: Rhythm Gen speaks the grammar on its five pages: four lanes and the shared values") {
  Spec spec;
  spec.declared = 24;
  spec.page_first_row = {"HITS", "HITS", "HITS", "HITS", "INSTR"};  // (the shared page ends with the four target rows, OS-APPUX S5)
  spec.text_rows = (1u << 3) | (1u << 9) | (1u << 10);  // a lane's NOTE (a note name), the shared VOL (UNSET, then a number) and MODE (a word)
  spec.fine_only = 1u << 10;
  run("rhythmgen", spec);
}

TEST_CASE("app grammar: Rhythm Gen's target rows end its shared page; APPLY writes at the moved target (OS-APPUX S5)") {
  AppRig rig;
  NEED(rig.open_tool("rhythmgen", O3_E2E_CARD_DIR));
  TargetSpec ts;
  ts.pages_right = 4;
  ts.writes = true;
  check_target_rows(rig, "rhythmgen", ts);
}

// ---- OS-APPUX S3: the five apps that declared rows draw their own now, in the same grammar -----------------------------------------------------------------------------------------------

TEST_CASE("app grammar: Tap Calc speaks the grammar: rows BPM and AVERAGE, 0.1 and Shift 1.0 a detent, Delete the default, Left / Right the ms and Hz pages") {
  Spec spec;
  spec.declared = 2;
  spec.fine_only = 1u << 1;  // AVERAGE has no coarse step
  spec.pads_act = true;      // the pads are its taps
  spec.open_from_modules = spec.relaunch_from_modules = true;
  run("tapcalc", spec);
}

TEST_CASE("app grammar: MIDI Monitor speaks the grammar: rows CHANNEL and TYPE, Delete is the default (it does not clear the list: F2 does)") {
  Spec spec;
  spec.declared = 2;
  spec.text_rows = (1u << 0) | (1u << 1);  // "ALL", then a number, and the type words
  spec.fine_only = 1u << 1;
  spec.open_from_modules = spec.relaunch_from_modules = true;
  run("midimon", spec);
}

TEST_CASE("app grammar: Loudness VU speaks the grammar: rows SOURCE and HOLD, Delete is the default (it does not clear the figures: F1 does)") {
  Spec spec;
  spec.declared = 2;
  spec.text_rows = 1u << 0;
  spec.fine_only = 1u << 0;
  spec.persist_row = 1;  // HOLD
  spec.open_from_modules = spec.relaunch_from_modules = true;
  run("loudness", spec);
}

TEST_CASE("app grammar: Tuner speaks the grammar: rows A4 and SOURCE") {
  Spec spec;
  spec.declared = 2;
  spec.text_rows = 1u << 1;
  spec.fine_only = 1u << 1;
  spec.open_from_modules = spec.relaunch_from_modules = true;
  run("tuner", spec);
}

TEST_CASE("app grammar: Scope Spectrum speaks the grammar: rows VIEW, SOURCE and TIMEBASE") {
  Spec spec;
  spec.declared = 3;
  spec.text_rows = (1u << 0) | (1u << 1);
  spec.fine_only = (1u << 0) | (1u << 1);
  spec.persist_row = 2;  // TIMEBASE
  spec.open_from_modules = spec.relaunch_from_modules = true;
  run("scope", spec);
}
