// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Scenario scripts for o3sim: one command per line, `#` starts a comment, blank lines are ignored.
//
//   wait <ms>                      advance simulated time by <ms> (0..3600000), one 1 ms tick at a time
//   press <button>                 button down;  release <button>  button up
//   tap <button> [<count>]         press and release, <count> times (1..1000, default 1)
//   pad <0-47> [hold <ms>|down|up] hit and release a pad; `hold <ms>` (1..60000) keeps it down for that much simulated time first; `down` and `up` are its two halves (one tick each), with any commands between
//   encoder <n> [x<k> [every <ms>]]  turn the encoder by <n> detents, +clockwise / -counter (-1000..1000, not 0); `x<k>` does it k times (2..1000), `every <ms>` (2..255) a turn each <ms>, in the
//                                  ticks between them the clock runs on: a sweep a parameter is moved through while a note sounds
//   action <name> [<arg>]          run a registered action directly (app/actions.h, e.g. nav.down 3, learn.toggle),
//                                  as a script or menu would: no gesture, no binding. <arg> is an integer, default 0
//   document [bindings] [new] [sound] [tb303] [tracks <n>]      open the demo document (sim/demo_project.h): ten instruments with their parameters, an undo journal,
//                                  the audio side's ParamChange ring (the Instrument PARAMS page edits it); `bindings` adds two Learn
//                                  bindings; `new` (OS-NEWKEEP) attaches a project sink that does File > NEW (F3, then F3 ALL or F1 KEEP INSTRUMENTS) on it. `sound` (OS-E2E-2) is the document of the audio journeys instead
//                                  of the demo: an empty pattern of 16 steps, four neutral instruments (demo_project.h). `tb303` (OS-E2E-9) is the document of the TB-303 journey: a module instrument and an acid line
//                                  (demo_project.h); `audio on` binds the module (sim_audio.h). Without it the app has no project
//   playhead <step> [<ustep>]      the sequencer is playing the pattern in view at <step> (0..127), <ustep> of the step's 48 microsteps already played (0..47, default 0):
//                                  what the engine publishes (audio/play_position.h), as OS-LIVEREC's live recording reads it; `playhead stop` stops it.
//                                  `playhead <step> [<ustep>] countin <steps>` (OS-METRO): the transport is counting in, <steps> (1..32) of it still to run, the running one included
//                                  `playhead <step> [<ustep>] wait <step|bar|pattern>` (OS-METROALIGN): a take armed on this running transport waits for that alignment (the strip counts down)
//   midicc <channel> <cc> <value>  a MIDI control change message (channel 1..16, cc and value 0..127): the action midi.cc as MIDI input
//                                  will enqueue it. With the Learn overlay up it binds the cc to the target, else it moves what it is bound to
//   midinote <channel> <note> <velocity|off>   OS-QANET C2: a MIDI note on or off (channel 1..16, note 0..127, velocity 1..127) on port 0 through the render's own path (midi::CcInput::feed, then App::poll_midi_notes,
//                                  then one tick); needs `audio on` to be heard
//   power down|up                  OS-POWER P1: the Power switch goes down / up (core::PowerHold, active low). The switch arms after 100 ms of release (wait first); a hold shows the line from 150 ms and
//                                  commits at 2000 ms, when the strip says POWERING OFF and the app takes no more input. `power hold <ms>`: down, <ms> (1..60000) of time, up.
//                                  `power unsaved`: the open document has changes no save took (the line then warns UNSAVED WORK); needs `document` first
//                                  OS-SCREENSHOT: `press shift`, `power down`, (30 ms), `release shift`, `power up` is Shift then Power: with a `card` the screen is saved as Screenshots/SHOT_nnnn.bmp
//                                  (about 400 ticks, the app draws no frame meanwhile; `assert file ... size=768066`, strip SCREENSHOT SAVED: SHOT_0001.BMP); without one the strip says NO SCREENSHOT: NO CARD
//   theme <name>                   switch the colour theme (amiga, black, white; ui/theme.h), as the Shift + Master
//                                  binding would, then redraw
//   snap <name>                    capture the framebuffer as <name> (letters, digits, - and _, up to 64)
//   card <fixture-dir>             OS-E2E-1b: the simulator gets a card: a scratch copy of the fixture folder (a relative one is looked up in o3sim's --cards folder, else the scenario file's), mounted, writable through the real path guard, with a project
//                                  loader behind File > OPEN and NEW (sim_project.h). Needs `document` first (a load replaces it) and comes before any further `document`; one card a run.
//                                  `card slow <bytes-per-ms> <latency-us> [<write-bytes-per-ms>]` (OS-QANET D2) turns the slow-card model on (before `card` or after it): every call of the app to the card charges its latency
//                                  plus its bytes over the rate to the simulated clock in the pass where it happens, as the unit's synchronous call would stretch the pass, and the audio goes on rendering through
//                                  it (platform/host/slow_card.h; the defaults of o3sim's --card-rate and --card-latency are PROVISIONAL until OS-QANET D4). `card slow off` turns it off. A fixture folder named `slow` is written `./slow`.
//                                  `card full` (OS-QANET B8) fills the card: every write that would grow it (a new file or folder, a marker, any written byte) fails with the no-space status, as on a full FAT card; reads, renames and
//                                  deletes still work. `card full off` empties it again. Independent of `card slow`, before `card` or after it. A fixture folder named `full` is written `./full`.
//                                  `card ptfx` (OS-CEPTFX S4a) writes seven synthetic header-only plugins into the card's Effects/ (after `card <folder>`; a folder card only): the PLUGINS page of Sample edit lists them. A fixture folder named `ptfx` is written `./ptfx`.
//   disk idle|working|out|audio   OS-USBDISK U2b: what the platform tells the Config > USB page's CARD AS DISK row (App::set_disk_state; `out` also tells set_card_on_computer): Idle, a hand-over running, the
//                                  card on the computer, or Idle with USB AUDIO presented. The disk is told writable when READ ONLY is Off at that moment (the row shows it while out; turning READ ONLY meanwhile says EJECT THE CARD TO CHANGE IT). The simulator has no USB; the strip text of the Out state is the platform's (a `say` step names it).
//                                  `card pull` takes it out of the slot (CARD REMOVED; a file being written stays torn), `card insert` puts it back (READING CARD, CARD READY). A fixture folder
//                                  named `pull` or `insert` is written `./pull`. The scratch copy is removed when the run ends, so a --twice run starts clean.
//   firmware on                    OS-FWCONFIG: the firmware update dialog gets its port with no flash behind it (simulator.h, platform/host/fake_firmware.h): four scripted files, TRACKER_1.PTF (good, the hand-over
//                                  does not start), SLOW_CHECK.PTF (a check of 600 passes), BAD_IMAGE.PTF (the check fails) and NEVER_ENDS.PTF (the hand-over never ends). Config's INFO page then labels F8 UPDATE;
//                                  F8 there opens the list. Needs nothing before it; once a run.
//                                  `card copy <from> <to>` (OS-E2E J2): a computer copies a folder of the pulled card to a new place (an EXPORT's OG folder into Projects/ to open it); both are relative paths of the card.
//   apps on                        OS-APPUX S0: the card carries the shipped apps (sim_apps.h): MODULES lists them, F1 launches one on its own page, with the key row it declares. Needs `card` before it
//                                  (the apps are files of its modules/apps folder); refused with `audio on`, either way round (the apps' audio tap is not the engine's)
//   audio on                       OS-E2E-2: the real engine renders: the open document's sequencer plays through the sampler voices, the graph, the send effects and the limiter (sim_audio.h). Needs a
//                                  `document` before it (and a `card` too, if one is wanted), and no `document`, `card <fixture>` or `playhead` after it. A `document new` is refused before it (its File > NEW rewrites the project under the engine with no Stop and no wait for the voices; a card's NEW does both). From then on every tick renders the blocks that are due
//   render <name> <ms> [stems]     OS-E2E-2: capture the master for the next <ms> (1..10000) of simulated time, whatever commands move the clock (a window opens now and closes by itself). <name> is letters,
//                                  digits and _ (up to 64). `stems` also captures the tracks (T1..T8, mono) and the stem tap's master pair as <name>.t1 .. <name>.t8 and <name>.m. One window at a time; a window still
//                                  open at the end of the script is an error. o3sim --wav <dir> writes every render as <scenario>_<name>.wav
//   wav <name> <path>              OS-E2E-9: a WAV file of the simulated card (16-bit PCM, stereo; the file an EXPORT wrote, "EXPORT/<project>/<name>.WAV") becomes the finished render <name>, at the file's own
//                                  rate, for `assert audio` and `o3sim --wav`. Needs `audio on` and a `card` before it; the file must be on the card when the line runs (a `wait` for the export first)
//   fingerprint <render> [nof0]    OS-E2E-4: the finished render (a `render`, a `wav`, or a stems part `<name>.t1` .. `<name>.m`) takes part in the scenario's audio fingerprint, tests/audio_fingerprints/<scenario>.json
//                                  (audio_fingerprint.h): `o3sim --fingerprints <dir> --check-fingerprints` compares it with the stored one, `--update-fingerprints` rewrites it. Read only, no tick; without --fingerprints it
//                                  only checks that the render has ended. `nof0`: kept without an f0 track (a render with no steady fundamental). A render is named once.
//   assert audio <name> [in=<from>-<to>] <check> ...   OS-E2E-2: measurements of a finished render (pitch=, rms=, peak<, dc<, silent, clicks=, onset=, envelope=, centroid<, pan=, zipper=, alias<, null=); the
//                                  grammar, the maths and the tolerances are at the top of audio_check.h. Read only, like the other asserts
//   assert state <key>=<value> ... OS-E2E-1a: the unit's `diag state` line (app/state_line.h) holds every pair; <value> is the exact text, or ~<regex> (POSIX extended, searched)
//   assert model <path>=<value>    a value of the open document: tempo, name, order.<n>, pat.<p>.<t>.<s>.<field>, inst.<n>.name, inst.<n>.<param>, or digest=@<label>
//   mark model <label>             remembers the document's digest (the SHA-256 of its native file) for `assert model digest=@<label>`
//   assert file <path> <test>      a file of the simulated card: exists, absent, size=<n>, size><n>, size<<n>, sha=<hex>; "no card" when there is none
//                                  The grammar of the paths and values is in scenario_assert.h. A failed assertion ends the run with "line N: assertion failed: <the line>: <why>"
//                                  (o3sim exits 2); an assertion only reads, it never ticks the clock, so the pictures of a scenario do not change when it gains one.
//
// Command words, button names and action names are case-insensitive; button names are app/buttons.h kButtonNames
// (f1..f8, left, right, up, down, note, instr, vol, fx, pattern, play, ...); an action name must be in the
// registry (app/actions.cpp) or the scenario is rejected. Snap names are unique per scenario, and so are mark labels. A word in double quotes is one word ("Projects/Track 01/project.o3pj").

#include <o3os/app/buttons.h>
#include <o3os/core/action.h>
#include <o3os/sim/simulator.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace o3os::sim {

struct ScenarioCommand {
  enum class Kind : std::uint8_t { Wait, Press, Release, Tap, Pad, Encoder, Action, Theme, Snap, Document, MidiCc, Playhead, Power, Assert, Mark, Card, Audio, Render, Wav, Fingerprint, Apps, Disk, MidiNote, Firmware };
  enum class Target : std::uint8_t { State, Model, File, Audio };  // assert: what is looked at
  Kind kind = Kind::Wait;
  std::uint32_t value = 0;  // wait: milliseconds; power: 0 up, 1 down, 2 hold (delta: the milliseconds), 3 unsaved; pad: index (delta: the ms it is held; `mode` below); midinote: the note; tap: repeat count; render: the ms, bit 31 `stems`; fingerprint: 1 `nof0`; document: bit 0 `bindings`, bit 1 `new`, bit 2 `sound`, bit 3 `tb303`, bits 8..15 `tracks <n>`; card: 0 open the fixture, 1 pull, 2 insert, 3 copy, 4 slow on (name: the read rate, args: the latency, the write rate), 5 slow off; playhead: the step
  std::int32_t delta = 0;   // midinote: the velocity (0: note off); encoder detents; playhead: the microsteps played; encoder: with `value` turns
  std::uint8_t mode = 0;    // midinote: the channel 1..16; pad: 0 hit, 1 hold (delta ms), 2 down, 3 up; encoder: the turns' spacing in ms (1: back to back)
  app::Button button = app::Button::F1;
  core::ActionId action = core::ActionId::None;  // action: what to run
  core::ActionArg arg = 0;                       // action: its argument; playhead: the steps of the count-in left (0: not counting in)
  std::uint8_t wait = 0;                         // playhead: the alignment a take waits for (RecTap::Align: 1 step, 2 bar, 3 pattern; 0 none)
  ui::ThemeId theme = ui::ThemeId::Amiga;        // theme: which
  std::string name;         // snap name; action: the name as written, lower-cased; mark: the label; card: the fixture folder; render and wav: its name
  Target target = Target::State;  // assert
  std::vector<std::string> args;  // assert: the words after `assert <state|model|file>`; wav: the path
  std::string text;               // assert: the line as written (comment cut), for the message of a failure
  int line = 0;             // 1-based source line, for messages
};

// Parses a whole script. false: `error` is "line N: <problem>" and `out` is unspecified.
bool parse_scenario(const std::string& text, std::vector<ScenarioCommand>& out, std::string& error);

// Called at every snap; return false to stop the run (e.g. a file could not be written).
using SnapshotFn = std::function<bool(const std::string& name, const Simulator& sim)>;

// Runs the commands on `sim`, which should be fresh: one tick first so the initial frame exists, then each
// command in order. Stops with false and `error` set on the first unhealthy frame or refused snapshot. `log` (when not null) gets a line for every passed `assert audio`: what it measured.
bool run_scenario(const std::vector<ScenarioCommand>& commands, Simulator& sim, const SnapshotFn& on_snapshot,
                  std::string& error, std::vector<std::string>* log = nullptr);

// Lower-case hex SHA-256 of the framebuffer's RGBA bytes: what the goldens file stores.
std::string framebuffer_sha256(const EveRaster& raster);

}  // namespace o3os::sim
