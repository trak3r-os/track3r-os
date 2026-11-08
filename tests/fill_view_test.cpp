// SPDX-License-Identifier: GPL-3.0-or-later
// OS-PTOOLS part 1, OS-FILL2 and OS-PTOOLS2 S3c: the Fill dialog's rows (app/fill_view.h): the descriptors are valid, the seven rows are used or dim by WHAT, TYPE and MODE, their captions follow, the enumerated rows roll over, the others stop
// at their ends, KEY steps the scale and (Shift) the root, FX TYPE lists the picker, FROM and TO follow the domain of WHAT, a row that does not apply does not change; and the footer names the selection as the target (fill_target_text).
#include <doctest/doctest.h>

#include <o3os/app/fill_view.h>
#include <o3os/app/fx_text.h>
#include <o3os/model/fx_table.h>

#include <cstring>
#include <string>

using namespace o3os;
using namespace o3os::app;

namespace {

namespace fill = o3os::model::fill;

std::string text(const FillView& f, FillRow row, int octave_shift = 0) {
  char buf[40];
  (void)fill_text(f, row, buf, sizeof buf, octave_shift);
  return buf;
}

std::string caption(const FillView& f, FillRow row) { return fill_caption(f, row); }

// The dialog as it opens on a cursor that holds note 62 (OG 61), instrument 9, volume 40 and an FX1 of type 8.
FillView opened(fill::What what = fill::What::Note) {
  FillView f;
  f.open.note = 62;
  f.open.instr = 9;
  f.open.vol = 40;
  f.open.fx_type[0] = 8;
  f.plan.what = what;
  fill::range_default(f.plan, f.open);
  return f;
}

bool used(const FillView& f, FillRow row) { return fill_row_used(f, row); }

}  // namespace

TEST_CASE("fill view: seven rows, every descriptor is valid and the defaults are the dialog's") {
  static_assert(kFillRows == 7, "the dialog has seven rows (OS-PTOOLS2 S3c: no SCOPE row)");
  const FillView f;
  for (unsigned i = 0; i < kFillRows; ++i) {
    CHECK_MESSAGE(core::param_desc_valid(fill_desc(static_cast<FillRow>(i))), "row " << i);
  }
  CHECK(text(f, FillRow::What) == "NOTE");
  CHECK(text(f, FillRow::Type) == "EVERY N");
  CHECK(text(f, FillRow::Count) == "1");
  CHECK(text(f, FillRow::Mode) == "CONST");
  CHECK(text(f, FillRow::Key) == "No scale");
  CHECK(text(f, FillRow::From) == "C-5");   // OG note 60
  CHECK(text(f, FillRow::To) == "-");       // CONST: one value
  CHECK(caption(f, FillRow::Count) == "N");
  CHECK(caption(f, FillRow::Key) == "KEY");
  CHECK(caption(f, FillRow::From) == "VALUE");
}

TEST_CASE("fill view: which rows are used for each WHAT, TYPE and MODE, and what the captions say") {
  FillView f = opened();
  // NOTE, EVERY N, CONST: MODE and KEY are used, TO is not.
  CHECK((used(f, FillRow::Mode) && used(f, FillRow::Key) && used(f, FillRow::From)));
  CHECK_FALSE(used(f, FillRow::To));
  fill_set(f, FillRow::Mode, 1.0f);  // RAMP
  CHECK(used(f, FillRow::To));
  CHECK(caption(f, FillRow::From) == "FROM");
  // TYPE RANDOM and SCALE choose the notes: MODE is dim, FROM..TO is their window and is used even in CONST.
  fill_set(f, FillRow::Mode, 0.0f);
  for (const float type : {1.0f, 3.0f}) {
    fill_set(f, FillRow::Type, type);
    CHECK_FALSE(used(f, FillRow::Mode));
    CHECK(text(f, FillRow::Mode) == "-");
    CHECK(used(f, FillRow::To));
    CHECK(caption(f, FillRow::From) == "FROM");
    CHECK_FALSE(fill_step(f, FillRow::Mode, 1));
    CHECK(f.plan.mode == fill::Mode::Const);
  }
  // The same TYPE for another WHAT is a plain fill: MODE is used again.
  fill_set(f, FillRow::What, 2.0f);  // VOLUME
  CHECK(used(f, FillRow::Mode));
  CHECK_FALSE(used(f, FillRow::To));
  CHECK_FALSE(used(f, FillRow::Key));  // INSTR and VOLUME have no key
  CHECK(text(f, FillRow::Key) == "-");
  fill_set(f, FillRow::What, 1.0f);  // INSTR
  CHECK_FALSE(used(f, FillRow::Key));
  for (const float what : {3.0f, 4.0f}) {  // FX 1, FX 2
    fill_set(f, FillRow::What, what);
    CHECK(used(f, FillRow::Key));
    CHECK(caption(f, FillRow::Key) == "FX TYPE");
  }
  // The caption of row 4 follows TYPE: N, DENSITY, EVENTS, N.
  const char* const counts[4] = {"N", "DENSITY", "EVENTS", "N"};
  for (unsigned t = 0; t < 4; ++t) {
    fill_set(f, FillRow::Type, static_cast<float>(t));
    CHECK(caption(f, FillRow::Count) == counts[t]);
    CHECK(used(f, FillRow::Count));
  }
  // The rows that are always used.
  for (const FillRow row : {FillRow::What, FillRow::Type, FillRow::Count, FillRow::From}) {
    CHECK(used(f, row));
  }
}

TEST_CASE("fill view: the enumerated rows roll over, the numbered rows stop at their ends, an unused row does not change") {
  FillView f;
  CHECK(fill_step(f, FillRow::Type, -1));  // EVERY N rolls to SCALE
  CHECK(f.plan.type == fill::Type::Scale);
  CHECK(fill_step(f, FillRow::Type, 1));
  CHECK(f.plan.type == fill::Type::Every);
  CHECK(fill_step(f, FillRow::What, -1));  // NOTE rolls to FX 2
  CHECK(f.plan.what == fill::What::Fx2);
  CHECK(fill_step(f, FillRow::What, 1));
  CHECK(f.plan.what == fill::What::Note);
  CHECK(fill_step(f, FillRow::Mode, -1));  // CONST rolls to RANDOM
  CHECK(f.plan.mode == fill::Mode::Random);
  CHECK(fill_step(f, FillRow::Mode, 1));
  CHECK(f.plan.mode == fill::Mode::Const);
  CHECK(fill_step(f, FillRow::Count, 100));
  CHECK(f.plan.n == 32);
  CHECK_FALSE(fill_step(f, FillRow::Count, 1));  // stopped at its end
  CHECK(fill_step(f, FillRow::Count, -100));
  CHECK(f.plan.n == 1);
  // EUCLID keeps its own count (EVENTS), RANDOM its DENSITY, so switching type does not lose N.
  f.plan.n = 7;
  fill_set(f, FillRow::Type, 2.0f);
  CHECK(text(f, FillRow::Count) == "4");
  CHECK(fill_step(f, FillRow::Count, 1));
  CHECK(f.plan.events == 5);
  CHECK(f.plan.n == 7);
  fill_set(f, FillRow::Type, 1.0f);
  CHECK(text(f, FillRow::Count) == "30");
  CHECK(fill_step(f, FillRow::Count, 5));
  CHECK(text(f, FillRow::Count) == "35");
  CHECK(fill_step(f, FillRow::Count, 1000));
  CHECK(f.plan.density == 100);
  CHECK_FALSE(fill_step(f, FillRow::Count, 1));
  CHECK(f.plan.events == 5);
  CHECK(f.plan.n == 7);
  // TO is dim in CONST and does not change.
  CHECK_FALSE(fill_step(f, FillRow::Mode, 0));
  fill_set(f, FillRow::Type, 0.0f);
  CHECK_FALSE(fill_step(f, FillRow::To, 1));
  CHECK(text(f, FillRow::To) == "-");
}

TEST_CASE("fill view: KEY steps the scale in a ring of forty and, with Shift, the root in a ring of twelve; No scale shows no root") {
  FillView f;
  CHECK(text(f, FillRow::Key) == "No scale");
  CHECK(fill_step(f, FillRow::Key, -1));  // rolls over at the bottom
  CHECK(f.plan.scale == 39);
  CHECK(fill_step(f, FillRow::Key, 1));   // and at the top
  CHECK(f.plan.scale == 0);
  CHECK(fill_step(f, FillRow::Key, 1));
  CHECK(text(f, FillRow::Key) == "Chromatic");  // takes every note: no root in front of it
  fill_set(f, FillRow::Key, 4.0f);
  CHECK(text(f, FillRow::Key) == "C Dorian");
  CHECK(fill_step_coarse(f, FillRow::Key, 1));  // Shift + encoder: the root
  CHECK(f.plan.root == 1);
  CHECK(f.plan.scale == 4);
  CHECK(text(f, FillRow::Key) == "C# Dorian");
  CHECK(fill_step_coarse(f, FillRow::Key, -2));  // rolls over below C
  CHECK(f.plan.root == 11);
  CHECK(fill_step_coarse(f, FillRow::Key, 13));  // 13 places on a ring of twelve: one
  CHECK(f.plan.root == 0);
  CHECK(fill_step(f, FillRow::Key, 41));         // 41 places on a ring of forty: one
  CHECK(f.plan.scale == 5);
  CHECK(f.plan.root == 0);
  fill_set(f, FillRow::Key, 8.0f);
  CHECK(text(f, FillRow::Key) == "C Phrygian");
  fill_set(f, FillRow::Key, 99.0f);               // clamped to the last scale
  CHECK(f.plan.scale == 39);
  // OS-SCALES28 Q4c: the KEY row shows the one name table's display names, the same the Note panel shows.
  fill_set(f, FillRow::Key, 5.0f);
  CHECK(text(f, FillRow::Key) == "C Lydian");      // was "C Lyd Maj"
  fill_set(f, FillRow::Key, 16.0f);
  CHECK(text(f, FillRow::Key) == "C Blues");      // was "C Blues Min"
  fill_set(f, FillRow::Key, 20.0f);
  CHECK(text(f, FillRow::Key) == "C Hung. minor");  // one of the 27: the panel reaches it with the encoder
  fill_set(f, FillRow::Key, 0.0f);
  CHECK(text(f, FillRow::Key) == "No scale");
  // The root has no step for INSTR and VOLUME (the row is dim).
  fill_set(f, FillRow::What, 1.0f);
  CHECK_FALSE(fill_step_coarse(f, FillRow::Key, 1));
}

TEST_CASE("fill view: FX TYPE lists every picker type but None in picker order, rolls over, and a new type starts FROM and TO at its default") {
  FillView f = opened(fill::What::Fx1);
  CHECK(f.plan.fx_type == 8);                    // the cursor step's type in that column
  CHECK(text(f, FillRow::Key) == "MIDI CC A");
  CHECK(fill_get(f, FillRow::Key) == static_cast<float>(model::fx::picker_index(8)));
  // Walk the whole list from Off: 59 entries, the picker's own names, and one more step comes back to Off.
  fill_set(f, FillRow::Key, 1.0f);
  CHECK(f.plan.fx_type == model::fx::kPickerOrder[1]);
  CHECK(text(f, FillRow::Key) == "Off");
  for (unsigned i = 1; i < model::fx::kPickerCount; ++i) {
    CHECK_MESSAGE(f.plan.fx_type == model::fx::kPickerOrder[i], "entry " << i);
    char name[kFxNameMax];
    fx_picker_name(i, name, sizeof name);
    CHECK_MESSAGE(text(f, FillRow::Key) == name, "entry " << i);
    CHECK(fill_step(f, FillRow::Key, 1));
  }
  CHECK(f.plan.fx_type == model::fx::kPickerOrder[1]);  // 59 steps: round
  CHECK(fill_step(f, FillRow::Key, -1));                 // below Off is the last entry
  CHECK(text(f, FillRow::Key) == "Grain Length");  // the last entry since OS-CEWTENV W6b (z)
  // D3: a new type puts FROM and TO at its default; the same type keeps what was set.
  fill_set(f, FillRow::Key, static_cast<float>(model::fx::picker_index(17)));  // Swing, default 50, 25..75
  CHECK((f.plan.from == 50 && f.plan.to == 50));
  fill_set(f, FillRow::Mode, 1.0f);
  fill_set(f, FillRow::From, 30.0f);
  fill_set(f, FillRow::Key, static_cast<float>(model::fx::picker_index(17)));
  CHECK(f.plan.from == 30);
  fill_set(f, FillRow::Key, static_cast<float>(model::fx::picker_index(40)));  // Tune, default 24
  CHECK((f.plan.from == 24 && f.plan.to == 24));
  CHECK(text(f, FillRow::From) == "TUNE 0 ST");
  fill_set(f, FillRow::To, 31.0f);
  CHECK(text(f, FillRow::To) == "TUNE +7 ST");  // the words of the value where it has some (fx_text.h)
  fill_set(f, FillRow::Key, static_cast<float>(model::fx::picker_index(18)));  // Volume/Velocity: a plain number
  CHECK(text(f, FillRow::From) == "0");
  // FX 2 with no type under the cursor starts at the first picker type.
  fill_set(f, FillRow::What, 4.0f);
  CHECK(f.plan.fx_type == model::fx::kPickerOrder[1]);
}

TEST_CASE("fill view: FROM and TO follow the domain of WHAT, start from what the cursor holds, and WHAT starts them again") {
  FillView f = opened();
  CHECK((f.plan.from == 61 && f.plan.to == 72));
  CHECK(text(f, FillRow::From) == "C#5");
  CHECK(text(f, FillRow::From, -1) == "C#4");  // MIDDLE C shifts the names as it does in the grid
  fill_set(f, FillRow::Mode, 1.0f);
  CHECK(text(f, FillRow::To) == "C-6");
  // A note: one step, Shift + encoder twelve; stops at 0 and 127.
  CHECK(fill_step(f, FillRow::To, 1));
  CHECK(f.plan.to == 73);
  CHECK(fill_step_coarse(f, FillRow::To, 1));
  CHECK(f.plan.to == 85);
  CHECK(fill_step_coarse(f, FillRow::To, 100));
  CHECK(f.plan.to == 127);
  CHECK_FALSE(fill_step(f, FillRow::To, 1));
  CHECK(text(f, FillRow::To) == "G10");
  CHECK(fill_step_coarse(f, FillRow::From, -100));
  CHECK(f.plan.from == 0);
  CHECK(text(f, FillRow::From) == "C-0");
  // Changing WHAT puts FROM and TO at the dialog's values for it.
  fill_set(f, FillRow::What, 1.0f);  // INSTR
  CHECK((f.plan.from == 9 && f.plan.to == 9));
  CHECK(text(f, FillRow::From) == "09");
  CHECK(fill_step(f, FillRow::From, 1));      // the instrument walk
  CHECK(f.plan.from == 10);
  fill_set(f, FillRow::From, 1.0f);
  CHECK(fill_step(f, FillRow::From, -1));     // below 1 is 49: the MIDI channels come first in the order
  CHECK(f.plan.from == 49);
  fill_set(f, FillRow::From, 64.0f);
  CHECK_FALSE(fill_step(f, FillRow::From, -1));  // 64 is the first of the order: it stays
  fill_set(f, FillRow::From, 200.0f);            // clamped to 64
  CHECK(f.plan.from == 64);
  fill_set(f, FillRow::What, 2.0f);  // VOLUME: the volume under the cursor down to 0
  CHECK((f.plan.from == 40 && f.plan.to == 0));
  CHECK(text(f, FillRow::From) == "40");
  CHECK(fill_step_coarse(f, FillRow::From, 1));
  CHECK(f.plan.from == 50);
  CHECK(fill_step_coarse(f, FillRow::From, 100));
  CHECK(f.plan.from == 127);
  fill_set(f, FillRow::What, 0.0f);  // and back to NOTE: the cursor's note again, the edits are gone (Q2)
  CHECK((f.plan.from == 61 && f.plan.to == 72));
  fill_set(f, FillRow::What, 0.0f);  // the same WHAT keeps what was set
  fill_set(f, FillRow::From, 70.0f);
  fill_set(f, FillRow::What, 0.0f);
  CHECK(f.plan.from == 70);
}

TEST_CASE("fill view: an FX value is clamped to the type's range") {
  FillView f = opened(fill::What::Fx1);
  fill_set(f, FillRow::Key, static_cast<float>(model::fx::picker_index(3)));  // Roll 0..53, default 1
  CHECK((f.plan.from == 1 && f.plan.to == 1));
  fill_set(f, FillRow::Mode, 2.0f);
  fill_set(f, FillRow::To, 200.0f);
  CHECK(f.plan.to == 53);
  CHECK(fill_step_coarse(f, FillRow::To, -1));
  CHECK(f.plan.to == 43);
  CHECK_FALSE(fill_step(f, FillRow::To, 0));
  fill_set(f, FillRow::Key, static_cast<float>(model::fx::picker_index(13)));  // Break pattern: one value
  CHECK((f.plan.from == 1 && f.plan.to == 1));
  CHECK_FALSE(fill_step(f, FillRow::From, 1));
}

TEST_CASE("fill view: OS-PTOOLS2 S3c, the footer names the selection as the target") {
  char buf[48];
  const auto target = [&](const Selection& sel, int steps, int tracks) {
    (void)fill_target_text(sel, 0, 0, steps, tracks, buf, sizeof buf);
    return std::string(buf);
  };
  Selection sel;
  // Nothing selected: the cursor step.
  CHECK(target(sel, 64, 12) == "STEP  TRACK 1  ROW 1");
  sel.active = true;  // a one-cell selection is still the step
  sel.start_row = sel.end_row = 4;
  sel.start_track = sel.end_track = 2;
  CHECK(target(sel, 64, 12) == "STEP  TRACK 3  ROW 5");
  // A block: rows 1-8 of track 3.
  sel.start_row = 0;
  sel.end_row = 7;
  sel.start_track = sel.end_track = 2;
  CHECK(target(sel, 64, 12) == "BLOCK  TRACK 3  ROWS 1-8");
  // A block over several tracks.
  sel.start_track = 1;
  sel.end_track = 3;
  CHECK(target(sel, 64, 12) == "BLOCK  TRACKS 2-4  ROWS 1-8");
  // Shift + Up at row 1: the whole column of one track (rows 1-64 of 64 steps).
  sel.clear();
  sel.active = true;
  sel.column = true;
  sel.start_row = 0;
  sel.end_row = 63;
  sel.start_track = sel.end_track = 2;
  CHECK(target(sel, 64, 12) == "TRACK 3  ROWS 1-64");
  // ... and again: every track, the pattern.
  sel.start_track = 0;
  sel.end_track = 11;
  CHECK(target(sel, 64, 12) == "PATTERN  ALL TRACKS  ROWS 1-64");
  // One track in the pattern: its column is the track, not the pattern.
  sel.end_track = 0;
  CHECK(target(sel, 64, 1) == "TRACK 1  ROWS 1-64");
  // A block that happens to cover every row of one track is that track.
  sel.clear();
  sel.active = true;
  sel.start_row = 0;
  sel.end_row = 63;
  sel.start_track = sel.end_track = 2;
  CHECK(target(sel, 64, 12) == "TRACK 3  ROWS 1-64");
}
