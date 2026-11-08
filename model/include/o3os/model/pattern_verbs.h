// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The four verbs the Pattern tools lacked (OS-PTOOLS2 S4, docs/specs/OS-PTOOLS2.md section 3.2; maintainer decision Q4: native in flash): INTERPOLATE, RANDOMISE in place, ROTATE up and down and HUMANISE, as pure
// functions over ONE track's range of steps, plus `apply_block`, which walks a block of tracks through the Editor as ONE undo unit. NEW COMMUNITY OS BEHAVIOUR: no official firmware does any of the four (the
// spec's inventory); every rule below is this OS's own and has a host test (tests/pattern_verbs_test.cpp). Nothing here changes a step the rule does not name.
//
//   INTERPOLATE  `column` of the range ramps in a straight line from its first to its last value. The two anchors are the first and the last step that HOLD a value in the column (NOTE: a note 1..128; INSTR: 1..64;
//                VOL: 0..127, not unset; FX1 / FX2: a type other than None that the engine executes (fx::is_executable), and the last anchor is the last step with the FIRST anchor's type; INSTR / VOL anchors also
//                hold a note, since those ramps write noted rows only). Every row strictly between them
//                takes the interpolated value, rounded to the nearest (halves away from zero), whether it held one or not — except that the INSTR and VOL ramps write only the rows that hold a note: an instrument or a
//                volume on a note-less step is nothing, so such a row is left alone and counted. The anchors never change, the ramp is monotonic. A row between the anchors that the column cannot take is left alone and
//                counted: NOTE keeps a cut, a fade, an off and a reserved byte; FX keeps a different FX type. A NOTE ramp gives a row that has no instrument the first anchor's. Fewer than two anchors, or
//                anchors on one step, change nothing. (Assumption, recorded: the NOTE and FX ramps write empty rows too, as the PC trackers' interpolate does; the INSTR and VOL ramps are restricted to rows that hold
//                a note.)
//   RANDOMISE    in place: every step that holds a value in `column` is picked with probability CHANCE percent (0..100) and moves by a number drawn uniformly in -AMOUNT..+AMOUNT (0..64), clamped to the
//                column's range (NOTE 1..128, INSTR 1..64, VOL 0..127, an FX value to its type's range; a Chance value above its probability range and a type the editor does not write are left alone). A step
//                that holds no value never gains one (that is Fill's job). The draw is two numbers per step of the range, whatever the step holds, so a step's result does not depend on the other
//                steps' values; the same SEED and the same stream give the same steps. CHANCE 0 or AMOUNT 0 change nothing.
//   ROTATE       the steps of the range move one row up (UP: row i takes row i + 1, the first row goes to the end) or one down, wrapping inside the range. `elements` (edit.h kEl*) names the fields that move: the
//                whole step, or only the volume, FX 1, FX 2 or note-and-instrument of a column block; the others stay where they are.
//   HUMANISE     the arithmetic of the HUMANIZE app (modules/apps/humanize/humanize_math.h), step for step the same: TIME 0..100 is the largest delay, written as FX Micro-move (type 2); VOL 0..64 the volume
//                jitter of a step that carries one; only steps with a note change, only in VOL, FX1 and FX2; the same SEED and stream give the same steps as the app does. The .tmod stays on the card.
//
// One undo unit per verb: `apply_block` opens one group around every track it writes (the outermost group names the unit). It writes a track only when the verb changed it, never past a track's own length, and
// leaves the pattern alone (NotFound) when it does not exist; with nothing changed no unit is recorded. No heap, no globals, no `double`.

#include <o3os/core/action.h>
#include <o3os/model/editor.h>
#include <o3os/model/pattern_fill.h>
#include <o3os/model/status.h>
#include <o3os/model/step.h>

#include <cstdint>

namespace o3os::model::verbs {

using Column = fill::What;  // Note, Instr, Vol, Fx1, Fx2

enum class Verb : std::uint8_t { Interpolate = 0, Randomise = 1, RotateUp = 2, RotateDown = 3, Humanise = 4 };

inline constexpr unsigned kMaxAmount = 64;  // RANDOMISE AMOUNT and HUMANISE VOL
inline constexpr unsigned kMaxChance = 100;
inline constexpr unsigned kMaxTime = 100;   // HUMANISE TIME (the Micro-move FX range)
inline constexpr unsigned kMaxSeed = 9999;
inline constexpr std::uint8_t kFxMicroMove = 2;

// What a verb did to a range (or, from `apply_block`, to the whole block).
struct Count {
  unsigned changed = 0;  // steps whose bytes are now different
  unsigned skipped = 0;  // steps the verb meant to change and had to leave alone (see each verb)
};

// One setting set for every verb: a verb reads only its own fields (the dialog keeps one per verb for the session).
struct Plan {
  Verb verb = Verb::Interpolate;
  Column column = Column::Vol;                        // INTERPOLATE, RANDOMISE
  std::uint8_t elements = edit::kElMask;  // ROTATE: the fields that move (edit.h kEl*); the whole step by default
  std::uint8_t amount = 4;                            // RANDOMISE, 0..kMaxAmount
  std::uint8_t chance = 100;                          // RANDOMISE, 0..100 %
  std::uint8_t time = 20;                             // HUMANISE, 0..kMaxTime
  std::uint8_t vol = 8;                               // HUMANISE, 0..kMaxAmount
  std::uint16_t seed = 0;                             // RANDOMISE, HUMANISE, 0..kMaxSeed
};

// Does the step hold a value in the column (the anchors of INTERPOLATE, the steps RANDOMISE moves)?
bool has_value(Column column, const Step& step) noexcept;

// The verbs over `steps[0..len)`, one track's range (len 0..128). `stream` is the track's place in the block (0..15): it keeps two tracks of one block from drawing the same numbers.
Count interpolate(Column column, Step* steps, unsigned len) noexcept;
Count randomise(unsigned amount, unsigned chance, unsigned seed, unsigned stream, Column column, Step* steps, unsigned len) noexcept;
Count rotate(bool up, unsigned elements, Step* steps, unsigned len) noexcept;
Count humanise(unsigned time, unsigned vol, unsigned seed, unsigned stream, Step* steps, unsigned len) noexcept;

// The verb of `plan` over `steps[0..len)`.
Count run(const Plan& plan, unsigned stream, Step* steps, unsigned len) noexcept;

// A block: tracks track0..track0 + tracks - 1, rows row0..row0 + rows - 1 (a cursor step is a block of one).
struct Block {
  std::uint32_t track0 = 0;
  std::uint32_t tracks = 1;
  std::uint32_t row0 = 0;
  std::uint32_t rows = 1;
};

struct BlockResult {
  Status status = Status::Ok;  // Ok (also when nothing changed); NotFound: no such pattern; BadIndex: the block is outside its range, nothing written. A set_steps failure mid-block is reported as it is,
                               // with the tracks written before it still inside the one group (the caller's unit).
  Count count;                 // over the whole block
  unsigned tracks_written = 0;
  unsigned refused = 0;        // tracks whose part of the block starts past the track's end (never written)
};

// Runs the plan's verb on every track of the block (each cut at its own length) and writes the tracks it changed, all inside ONE undo group named by `action` and `arg`.
BlockResult apply_block(Editor& editor, core::ActionId action, core::ActionArg arg, std::uint32_t pattern, const Block& block, const Plan& plan) noexcept;

}  // namespace o3os::model::verbs
