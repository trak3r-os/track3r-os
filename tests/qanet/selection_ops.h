// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET A3 (docs/specs/OS-QANET.md part A, rule 3; new Community OS behaviour, test tooling only).
//
//   RULE 3. An operation on a selection acts on the selection. The table of selection_ops.cpp lists the actions that take a selection (a block of Pattern, a range of Song slots, the selected slice or
//           point of Wave, the selection of Sample edit, the selected instrument) and, for each, the SPAN it may change, written from the spec as decided (the entry names the spec) and not from the code.
//           For one run of such an action the document outside its span must be what it was (the digest of the project with the span masked, and the audio of every sample slot the span does not name), and
//           the result inside the span must be what the spec says. Where the spec is silent about the inside the entry says so and only the outside is judged.
//
// The same check serves the walk of selection_test.cpp (a state per entry, the action, the images before and after) and the random user of part B (B4): image_of() reads what the rule needs from a
// simulator (a copy of the document, the selection and the cursor, the audio of the slot in view), check_selection() judges one action. The check needs no knowledge of the state it is called in: an entry
// that does not apply (the action ran on another screen, the pattern is not editable) is not judged.
#pragma once

#include "rules.h"

#include <o3os/app/screens/ids.h>
#include <o3os/model/project.h>
#include <o3os/model/step.h>
#include <o3os/sim/simulator.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qanet {

// The part of the world that rule 3 looks at, copied out of a simulator.
struct SelImage {
  bool ok = false;                              // a document is open and the native writer could digest it
  std::shared_ptr<o3os::model::Project> doc;    // the document (a flat object: a copy is a copy; an image that is copied shares it, a test that edits one makes its own)
  std::string digest;                           // sim::model_digest(*doc)
  o3os::app::ScreenId screen = o3os::app::ScreenId::Pattern;
  unsigned page = 0;
  // Pattern: the pattern in view, the cursor, the column under it (0 NOTE, 1 INSTR, 2 VOL, 3 FX1, 4 FX2), the block (the cursor step when nothing is selected), whether a range was selected and whether the
  // pattern can be edited now (Edit on, not following the playhead).
  unsigned pattern = 1;
  int cur_row = 0, cur_track = 0, cur_col = 0;
  int row0 = 0, row1 = 0, track0 = 0, track1 = 0;
  int col0 = 0, col1 = 4;  // OS-AUDIT-56: the columns of the block (a block inside one track keeps the columns it was made over; whole steps otherwise)
  bool selected = false;
  bool editable = false;
  // The Fill dialog (F4): whether it is open and WHAT (0 NOTE, 1 INSTR, 2 VOLUME, 3 FX 1, 4 FX 2). OS-PTOOLS2 S3c: there is no SCOPE row; the target is the selection (the block fields above).
  bool fill_open = false;
  int fill_what = 0;
  // Song: the cursor slot, the range (the cursor's slot alone with no range), whether the cursor is in the map (the cells' operations are not judged here).
  int song_pos = 0, song_first = 0, song_last = 0;
  bool song_in_map = false;
  // The instrument in view (0-based slot), Wave: the selected slice, the selected point, the page (CHOP), ZC; Sample edit: the selection [a, b) and Amplify's percentage.
  int slot = 0;
  unsigned wave_slice = 0, wave_point = 0;
  bool wave_chop = false, wave_zc = false;
  std::uint32_t sed_a = 0, sed_b = 0;
  unsigned sed_amp = 0;
  // The audio: a hash of the frames of every sample slot, and the frames of the slot in view (empty when it has none or is too long to copy: pcm_ok false).
  std::vector<std::uint64_t> pcm_hash;
  std::vector<std::int16_t> pcm;
  bool pcm_ok = false;
};

// Reads the image from `s`. false: no document.
bool image_of(o3os::sim::Simulator& s, SelImage& out);

// What the caller knows that the images do not hold: the pattern clipboard (the App's is private: the caller that pressed Copy knows what it copied), and the instrument slot that Copy took.
struct Clip {
  int rows = 0, tracks = 0;
  std::vector<o3os::model::Step> steps;  // tracks columns of rows steps
  const o3os::model::Step& at(int track, int row) const { return steps[static_cast<std::size_t>(track) * static_cast<std::size_t>(rows) + static_cast<std::size_t>(row)]; }
};

struct SelContext {
  const Clip* clip = nullptr;   // block.paste: what is on the clipboard (null: unknown, the inside of a paste is not judged)
  int copied_instrument = -1;   // instrument.paste: the slot that Copy took (-1: unknown)
  bool must_act = false;        // the state is one where the action has to do something (the walk's states are): an action that leaves the document as it was is a finding. B4 leaves it false.
  std::string* detail = nullptr;  // where a finding says what exactly differed (for the log, never part of the baseline)
};

// One entry of the table: an action, the spec it is written from, and what its run may change. `span` names the places that may change (cells of a pattern, the song order, the slice table of an instrument ...);
// `inside` returns null when the result inside the span is what the spec says and a fixed phrase when it is not; null function: the spec is silent (`silent` says what the spec leaves open).
struct Span;
struct SelOp {
  const char* action;
  o3os::app::ScreenId screen;  // the screen the action works on: an entry that runs on another screen is not judged
  const char* spec;     // where the span and the inside come from
  const char* silent;   // "" when the spec decides the inside; else what it leaves open (that part is not judged)
  bool (*applies)(const SelImage& before, const SelContext& ctx);
  void (*span)(const SelImage& before, int arg, const SelContext& ctx, Span& out);
  const char* (*inside)(const SelImage& before, const SelImage& after, int arg, const SelContext& ctx);
};

const std::vector<SelOp>& selection_ops();                // the table
const SelOp* find_selection_op(const std::string& action);

// Actions that take a selection and are not judged yet, with the reason: they are counted and printed, never silently dropped.
struct NotJudged {
  const char* action;
  const char* reason;
};
const std::vector<NotJudged>& selection_ops_not_judged();

// The findings of rule 3 for one action that ran between the two images: `action` is the registry name, `arg` its argument. Rule "R3". Empty for an action that is not in the table or does not apply.
std::vector<Finding> check_selection(const SelImage& before, const SelImage& after, const std::string& action, int arg, const SelContext& ctx = SelContext{});

}  // namespace qanet
